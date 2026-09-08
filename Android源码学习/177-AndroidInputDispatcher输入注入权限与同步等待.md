# 177 Android InputDispatcher 输入注入、权限与同步等待

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态只读源码，不要求编译或连接设备  
> 前置章节：第 20、51、174、175、176 章

---

## 1. inject 返回 true，不等于目标控件被点击

`adb shell input`、系统测试、输入过滤器和部分自动化能力都能构造 KeyEvent/MotionEvent，再交给 InputDispatcher。最危险的测试断言是：

```text
injectInputEvent(...) == true
→ 目标 View 已收到
→ View 返回 handled=true
→ 点击业务已经完成
```

这条推理在 Android 11 每一步都可能断开。Java API 的同步 mode 只选择调用方等到哪个 Dispatcher 里程碑：

| mode | 成功主要证明 | 不保证 |
|---|---|---|
| ASYNC | 合法 entry 已入 inbound，调用者不等路由结论 | 有目标、权限通过、已 publish |
| WAIT_FOR_RESULT | 注入结果不再 PENDING；典型成功来自目标与权限确定，也可能是 policy 正常消费 | socket 成功、App 已读 |
| WAIT_FOR_FINISH | result 成功，且关联的 foreground DispatchEntry 计数归零 | handled=true、monitor 完成、业务完成 |

即使最强的 WAIT_FOR_FINISH，也可能因 connection broken 后清队列而成功返回；普通 click 还可能在输入 FINISHED 后才由 App 主 Looper 执行。

本章目标是把 caller identity、目标权限、`InjectionState`、两段条件变量和 foreground 计数串起来。读完应能解释 true/false/SecurityException 各在哪层产生，以及等待超时后为什么事件仍可能迟到。

---

## 2. 源码地图与三种同步常量

Java/Binder/JNI 入口：

```text
frameworks/base/core/java/android/hardware/input/InputManager.java
frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
```

native 路由与等待状态：

```text
frameworks/native/services/inputflinger/dispatcher/
├── InputDispatcher.cpp / InputDispatcher.h
├── InjectionState.cpp / InjectionState.h
├── Entry.cpp / Entry.h
└── include/InputDispatcherInterface.h
```

命令与权限声明：

```text
frameworks/base/cmds/input/src/com/android/commands/input/Input.java
frameworks/base/packages/Shell/AndroidManifest.xml
frameworks/base/core/res/AndroidManifest.xml
```

Java 常量名与 native 名略有不同：

```text
Java ASYNC            ↔ native SYNC_NONE
Java WAIT_FOR_RESULT  ↔ native SYNC_WAIT_FOR_RESULT
Java WAIT_FOR_FINISH  ↔ native SYNC_WAIT_FOR_FINISHED
```

`InputManager.injectInputEvent()` 是隐藏 API。Java 先拒绝 null 和非法 mode，再通过 `IInputManager` Binder 进入 system_server；JNI 只接受 KeyEvent 与 MotionEvent，其他 InputEvent 子类抛 RuntimeException。

---

## 3. IMS 保存原 caller，再清 Binder identity

服务端入口的核心顺序是：

```java
pid = Binder.getCallingPid();
uid = Binder.getCallingUid();
ident = Binder.clearCallingIdentity();
nativeInjectInputEvent(..., pid, uid, mode, 30_000, ...);
Binder.restoreCallingIdentity(ident);
```

两套身份承担不同职责：

- 清除前保存的 pid/uid 写进 `InjectionState`，供目标相关权限判断；
- clear 后的 Binder identity 让 IMS 内部调用不意外继承远端权限上下文；
- finally 无论成功、异常或超时都恢复 identity。

所以 `clearCallingIdentity()` 不会把远端注入者伪装成 system UID。native 后来回调 Java 检查 `INJECT_EVENTS` 时，仍显式传入最初保存的 pid/uid。

30 秒预算从进入 native `injectInputEvent()`、计算 `endTime=now()+timeout` 开始；Binder 请求在此前的排队不属于这个 native deadline。后续校验、policy intercept、等 Dispatcher 锁、入队、选目标和 finish 等待共享同一个绝对 endTime。

---

## 4. INJECT_EVENTS 是跨 UID 能力，同 UID 路径仍可放行

平台把 `android.permission.INJECT_EVENTS` 声明为 signature 权限。入口先调用：

```text
injectorUid == 0
或者 checkInjectEventsPermissionNonReentrant(pid, uid)
→ POLICY_FLAG_TRUSTED
```

但没有该权限并不在 Binder 入口立即失败。真正目标确定后，`checkInjectionPermission()` 的条件是：

```text
有 InjectionState
且（没有具体 window，或 window.ownerUid != injectorUid）
且调用者既不是 root 也没有 INJECT_EVENTS
→ PERMISSION_DENIED
```

所以权限模型是：

| 场景 | 是否需要全局权限 |
|---|---|
| 目标 window.ownerUid 与 injectorUid 相同 | 不需要 |
| 目标属于其他 UID | 需要 root 或 `INJECT_EVENTS` |
| 没有 foreground window、只靠 gesture monitor 成功 | 需要全局权限 |

Java 权限注释写“自己进程”，实现实际比较 UID。shared UID 下不同进程/包仍可能满足本地放行条件；精确诊断应看 `ownerUid`，不是包名或 PID。

同 UID 但无 signature 权限的事件可以通过目标门，却不会因此自动带 `POLICY_FLAG_TRUSTED`。入口的 trusted 标记与后面的 owner-UID 例外是两层判断，不能互相替代。

### 4.1 root、shell 与 system 不是同一个理由

- UID 0 在 native 中直接通过；
- shell 不是 root，它的 Shell APK 声明 `INJECT_EVENTS`；
- platform-signed 系统组件可因 signature permission 通过；
- 普通调用方只能依靠实际目标 ownerUid 相同的局部能力。

跨 UID 权限检查可能在入口设置 trusted 时做一次，目标确定后又做一次。传入显式 pid/uid，因此 Binder identity 已清除不会改变结果。

---

## 5. 注入事件仍经过变换、policy 与 filter 防环

所有普通注入先加：

```text
POLICY_FLAG_INJECTED
```

有全局资格时再加 TRUSTED。若未带 `POLICY_FLAG_FILTERED`：

```text
Key    → interceptKeyBeforeQueueing()
Motion → interceptMotionBeforeQueueing()
```

policy 可以修改 `PASS_TO_USER` 等 flag。若它正常消费，dispatch 阶段把 injection result 设为 SUCCEEDED，但不创建 App 目标。这种成功是“输入策略接受并终止”，不是 View 收到。

InputFilter 的回注走特殊入口：

```text
pid=0, uid=0
mode=ASYNC
timeout=0
policyFlags |= FILTERED
```

root 身份允许跨窗口，FILTERED 避免再次进入同一个 filter 形成循环，ASYNC 避免 filter callback 等待下游。timeout=0 不妨碍它，因为 ASYNC 根本不进入两个等待循环。

### 5.1 native 会重建事件，而不是原对象直通

Key 与 Motion 的 deviceId 都被改为 `VIRTUAL_KEYBOARD_ID`，source、displayId、事件时间及主体字段继续保留。Key 还会运行 Meta shortcut 加速：

```text
Meta + Backspace → BACK
Meta + Enter     → HOME
```

DOWN 的替换记录在 `mReplacedKeys`，对应 UP 即使 Meta 已松开也会保持同一替换。应用不能凭调用者伪造的 deviceId 把 injected event 冒充某个物理设备。

---

## 6. Motion history 会展开，而 r48 只给最后一条挂 InjectionState

native MotionEvent 的 sample 数组按历史到当前排列。注入代码先用第一个 sample 建 `MotionEntry`，再循环 `historySize` 次递增指针，最终把 current sample 也建成 entry：

```text
一个 Java MotionEvent（history 2）
→ MotionEntry(sample 0)
→ MotionEntry(sample 1)
→ MotionEntry(current)
```

这些 entry 共享原 event id、action、source、display 和 gesture 字段，但 eventTime/coords 取各自 sample；deviceId 都换成 virtual keyboard。它们依次进入 inbound，可能在 App 侧又被 Motion batch 合并。

r48 随后只执行一次：

```cpp
injectedEntries.back()->injectionState = injectionState;
```

因此只有最后的 current entry 携带：

- 原 injector pid/uid；
- PENDING/result；
- async 日志属性；
- foreground finish 计数。

### 6.1 这是结果范围，也是目标权限范围

前面的历史 entry 不是“共享同一个 state 但不重复计数”，而是 `injectionState == nullptr`。`checkInjectionPermission(window, nullptr)` 直接返回 true，`setInjectionResult()` 也无动作。

由 r48 控制流可静态推出：等待模式最终报告的是最后展开 entry 的结果和 foreground 生命周期，历史 entry 自身不做 caller→target UID 判断，也不纳入该次同步完成账；它们可能已先进入路由，最后 entry 才返回 DENIED/FAILED。

这是明确的版本代码边界，应视作潜在安全与测试一致性风险；实际可利用性还受隐藏 API 可达性、Motion 合法序列、既有 TouchState 粘性和焦点是否在 samples 间变化限制，不能外推到其他 Android 版本。

---

## 7. InjectionState 用一份手工引用计数跨越调用返回

对象字段只有：

```text
refCount
injectorPid / injectorUid
injectionResult（初始 PENDING）
injectionIsAsync
pendingForegroundDispatches
```

构造时 `refCount=1` 属于调用栈；挂到最后 EventEntry 前再加一。调用返回时释放自己的引用，EventEntry 仍可持有 state；Entry 析构才释放另一份。Motion split/组合若复制 state，还会继续增加引用。

这解释了为什么调用方等待超时后，Dispatcher 里的事件仍可安全继续：

```text
调用线程不再等待
≠ InjectionState 已销毁
≠ EventEntry 已从 inbound/pending/outbound/wait 删除
```

引用计数不是 pending target 数。`pendingForegroundDispatches` 只统计后面真正创建的 foreground DispatchEntry，两者不能用同一数值解释。

---

## 8. pointer 权限通过后才提交临时 TouchState

触摸选窗先复制现有状态到 `tempTouchState`，在临时对象中计算：

- 新 gesture、split pointer、slippery 与 hover；
- foreground/outside/wallpaper/gesture monitor 候选；
- 是否至少有 foreground 或 gesture monitor；
- 每个 foreground window 的注入权限。

只要一个 foreground target 跨 UID 且调用者无全局资格，就返回 PERMISSION_DENIED，不提交临时状态。若没有 foreground 但有 gesture monitor，还会以 null window 做全局权限检查。

权限通过后，才按 action 更新 `mTouchStatesByDisplay` 与 hover 状态。失败但拥有全局权限时仍可能提交某些归零/冲突状态，以维持系统输入流一致性；无权限事件则不应借失败路径污染当前手势归属。

OUTSIDE、wallpaper 和普通 monitor 不是 foreground 权限主体。它们可随一个合法 foreground 注入获得副本；跨 UID OUTSIDE 的坐标仍按常规安全规则清零。

---

## 9. ASYNC 只把同步责任截在入队处

Key action、Motion action/pointer 数/id、JNI 类型转换等基础校验仍在 ASYNC 返回之前；非法输入可以立即得到 FAILED 或 RuntimeException。policy pre-queue intercept 也同步运行，所以 ASYNC 不等于 Java 方法零耗时。

合法 entries 入 inbound 后，native 对 `SYNC_NONE` 直接把本地返回值设为 SUCCEEDED，不等待 `InjectionState.injectionResult`：

```text
ASYNC true
= 类型与基础事件校验通过
+ policy intercept 已返回
+ entries 已加入 Dispatcher inbound
```

随后仍可能因为无目标、dispatch disabled、stale、blocked 或跨 UID 权限不足而失败。`setInjectionResult()` 会为普通 async injection 写 native outcome 日志，但已返回的 Java 调用不可能穿越时间再抛 SecurityException。

FILTERED 回注也用 ASYNC，但 `setInjectionResult()` 特意抑制这条异步结果日志，避免系统内部过滤流制造噪声。

---

## 10. WAIT_FOR_RESULT 等的是状态值，不是 socket publish

同步模式先在条件变量 `mInjectionResultAvailable` 上循环：

```text
injectionState.injectionResult != PENDING
或者 now() >= endTime
```

Key/Motion 完成目标选择后，典型顺序是：

```cpp
setInjectionResult(entry, injectionResult);
if (injectionResult != SUCCEEDED) return;
addGlobalMonitoringTargetsLocked(...);
dispatchEventLocked(...);
```

因此契约上的 success 点早于为所有 target 创建 DispatchEntry 和 socket publish。等待线程虽已被 notify，但这几步仍在同一 Dispatcher 锁内，必须等当前临界区释放后才能真正返回；正常空闲 socket 下，它醒来时 publish 常已发生，这只是实现时序巧合。

若目标 connection 已消失、InputState 判定序列不一致而拒绝 enqueue，或 publish 得到 WOULD_BLOCK，result 仍可保持 SUCCEEDED。WAIT_FOR_RESULT 没有一只“至少一个 packet 写成功”的计数器。

policy 消费也是特殊成功：事件根本不选 App 目标，`setInjectionResult(SUCCEEDED)` 后结束。准确表述应是“注入结果已确定且为成功”，而非一律写成“目标窗口已确定”。

### 10.1 Java 如何映射 native 终态

| native result | IMS 行为 |
|---|---|
| SUCCEEDED | 返回 true |
| PERMISSION_DENIED | 抛 SecurityException |
| TIMED_OUT | warning + false |
| FAILED/其他 | warning + false |

PENDING 只在 native 状态对象内部存在。ASYNC 后台得到 PERMISSION_DENIED 时安全门仍有效，只是原调用者已经拿到 true。

---

## 11. WAIT_FOR_FINISH 只数带 foreground flag 的 DispatchEntry

目标 enqueue 时，顺序是：

```text
创建 DispatchEntry
→ InputState.trackKey/trackMotion 通过
→ 若 target 有 FLAG_FOREGROUND，pendingForegroundDispatches++
→ push outbound
```

典型 foreground 是承接 Key 或 touch gesture 的窗口。以下副本不增加这次同步计数：

- global input monitor；
- gesture monitor；
- OUTSIDE observer；
- wallpaper 等非 foreground target。

多窗口 split touch 若产生多个 foreground entry，就每个加一；全部释放后计数才归零。只有 gesture monitor 而没有 foreground window 的成功注入，需要全局权限，但 WAIT_FOR_FINISH 的 count 可以从一开始就是 0，调用很快返回。

结果先 set、计数后 increment 看似有竞态，实际上两者都在 Dispatcher 同一锁内；等待线程无法在 `dispatchEventLocked()` 尚未完成时抢锁看到临时的 0。

### 11.1 handled=false 仍可完成，但 Key fallback 可能延长生命周期

计数在 `releaseDispatchEntry()` 中减少，不读取 App 的 handled。因此 Motion 收到 `FINISHED(false)` 后正常释放，依然能让 WAIT_FOR_FINISH 返回 true。

Key 是一个例外时序：未处理 Key 可经 policy 生成 fallback，原 DispatchEntry 被放回 outbound 重派而不是立即 release。计数不会先减后加，而是保持 1，直到 fallback 生命周期也结束或连接清理。最终仍不要求 handled=true，但 false 不保证当场结束等待。

---

## 12. 哪些释放能让 finish 计数归零

正常路径：

```text
App FINISHED(seq, handled)
→ waitQueue 删除 DispatchEntry
→ 无 fallback restart 时 release
→ foreground count--
```

异常路径中，channel broken 或 unregister 会 drain outbound/wait，并对每个 entry 调用同一个 release。于是：

```text
WAIT_FOR_FINISH 返回 true
可能是 App 正常 ACK
也可能是连接清理让目标 entry 不再存在
```

它证明的是 Dispatcher 已不再管理该注入关联的 foreground entry，不证明客户端代码执行过。

### 12.1 resetAndDropEverything 并不 drain connection 队列

旧稿很容易把 reset 和 broken 清理并列。r48 的 `resetAndDropEverythingLocked()` 会：

```text
为所有 connection 合成 CANCEL_ALL
清 key repeat、pending 与 inbound
清 no-focus timer、AnrTracker、TouchState、hover、replaced keys
```

但它没有遍历并 drain 每个 connection 的 outbound/wait。已经成功选目标、计数非零的注入，不会只因全局 reset 就必然立刻解除 WAIT_FOR_FINISH；原 entry 仍要 ACK、通道清理或其他真实 release。更微妙的是 tracker 已被 clear，旧 wait entry 可能失去 ANR 索引。

pending/inbound 中尚未得 result 的注入则在 `releaseInboundEventLocked()` 中被设为 FAILED，能唤醒第一段等待。这两个阶段要分开。

---

## 13. 30 秒是调用等待预算，超时不会撤销事件

WAIT_FOR_RESULT 与 WAIT_FOR_FINISH 共用同一个 `endTime`：

```text
先等 injectionResult != PENDING
若成功且 mode=WAIT_FOR_FINISH
→ 再等 pendingForegroundDispatches == 0
```

两段 `wait_for` 都释放 Dispatcher lock，允许真正的调度线程继续工作；但前段耗掉的时间不会在后段补回。

例如 native 入口后 policy intercept 用 2 秒、等 focused window 用 20 秒、App 处理又用 12 秒：

```text
WAIT_FOR_RESULT 约 22 秒可返回 true
WAIT_FOR_FINISH 在总第 30 秒返回 false
事件仍可能在约第 34 秒完成
```

超时分支只把本次函数的局部返回值改为 TIMED_OUT，并释放调用栈持有的 InjectionState 引用。它没有：

- 从 inbound/pending 找出并删除 EventEntry；
- 从 outbound/wait 撤回 DispatchEntry；
- 回滚 TouchState；
- 合成 CANCEL。

只要 Entry 仍持有引用，事件和 state 就继续存活。自动化测试收到 false 后立即开始下一用例，可能被上一用例的迟到输入污染。

第 176 章的 window/no-focus ANR deadline 与这 30 秒并行存在。policy 若在较短 ANR 后选择继续等，注入调用可耗尽自己的预算；若 policy abort 并释放 pending/connection，则它也可能更早结束。

---

## 14. shell、display、source 与 HMAC 分属不同边界

`adb shell input` 为 Key 和 Motion 都选择 WAIT_FOR_FINISH。Shell APK 声明 `INJECT_EVENTS`，所以可跨 UID；命令等待 foreground entry 结账，适合串行脚本，却不等待 click 后的网络、动画或页面 present。

命令构造 Motion 时会尝试找支持 source 的设备 id，但 native 注入随后统一换成 `VIRTUAL_KEYBOARD_ID`。source 仍必须正确，因为它决定：

- pointer 命中还是 focused-window 路由；
- touch/hover/scroll 与 Motion 合法状态；
- ViewRoot 是否跳过 IME；
- InputState 怎样跟踪。

pointer source 且命令未指定 display 时，shell 工具把 displayId 修正为默认显示；Key 的无效 display 则由 Dispatcher 落到 focused display。错误 display/source 可以让权限检查面对完全不同的目标。

### 14.1 系统签名不能证明物理硬件来源

注入时重建的 native Event 使用 `INVALID_HMAC`，Dispatcher publish 前再以系统内部 key 为可验证字段签名。调用者不能自带 HMAC 绕过注入权限；但允许注入的事件最终也可获得 Dispatcher 签名。

因此 `verifyInputEvent()` 证明的是事件字段经过系统输入通道签名，不是“它必然来自真实硬件”。权限、`POLICY_FLAG_INJECTED`、虚拟 deviceId 与 HMAC 各回答不同问题。

---

## 15. 用结果与队列定位现场，并完成九组静态练习

先按观察结果分流：

| 现象 | 更可能的阶段 | 下一证据 |
|---|---|---|
| 立即 IllegalArgumentException | Java null/mode 校验 | 调用参数 |
| 立即 RuntimeException | JNI 类型/对象转换 | event 实际类型、native ptr |
| 同步 SecurityException | 最后带 state 的 entry 目标跨 UID | ownerUid、injectorUid、permission log |
| ASYNC true 但 UI 无变化 | 后台拒绝、无目标、policy consume 或 View false | native injection log、Pending/RecentQueue |
| WAIT_RESULT false 约 30 秒 | result 长期 PENDING | focused app/window、paused、500ms Key gate |
| WAIT_FINISH false 约 30 秒 | foreground count 未归零 | outbound/wait、connection、App/IME |
| WAIT_FINISH true 但 App 无回调 | policy consume、target 消失、track 拒绝或 broken drain | result 设置点与 entry 创建日志 |
| 带 history 的结果与早期 sample 行为不一致 | state 只挂最后 entry | 展开顺序、每笔 target 与权限 |

以下命令只读 `android-11.0.0_r48` 工作树。

### 练习 1：追 Java 到 JNI 的身份与结果映射

```bash
sed -n '870,920p' frameworks/base/core/java/android/hardware/input/InputManager.java
sed -n '635,680p' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '1444,1490p' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
```

标出两次 mode 校验、caller 保存、identity restore、类型检查与四种 Java 结果。

### 练习 2：证明权限门比较 UID 而非进程

```bash
sed -n '2038,2075p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '1995,2010p' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '3190,3210p' frameworks/base/core/res/AndroidManifest.xml
```

说明 ownerUid 相同、root、signature permission 和 null window 四条路径。

### 练习 3：核对 flags、policy 与 filter 回注

```bash
sed -n '3274,3345p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2310,2340p' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
```

回答 INJECTED、TRUSTED、FILTERED 分别在哪生成，哪个 flag 跳过 pre-queue intercept。

### 练习 4：手算 Motion history 的 state 范围

```bash
sed -n '3335,3412p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

用 historySize=2 标出三条 entry，指出哪条携带 pid/uid、result 与 finish count。

### 练习 5：验证 state 引用与等待循环

```bash
sed -n '20,65p' frameworks/native/services/inputflinger/dispatcher/InjectionState.cpp
sed -n '25,70p' frameworks/native/services/inputflinger/dispatcher/InjectionState.h
sed -n '3390,3475p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

解释初始 ref、EventEntry ref、caller release 和两段共用 endTime。

### 练习 6：证明 RESULT 早于 target enqueue/publish

```bash
sed -n '1160,1275p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2260,2430p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

对 Key/Motion 分别找到 `setInjectionResult`、monitor 添加、dispatch 与 foreground increment。

### 练习 7：闭合 finish count 与 fallback

```bash
sed -n '2660,2700p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4741,4865p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '3540,3565p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

说明正常 release、broken drain、Key restart 与 handled 的关系。

### 练习 8：区分 reset 与 drain

```bash
sed -n '990,1010p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4025,4045p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4350,4410p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

列出 pending/inbound、outbound/wait 分别在哪些路径释放，并判断 reset 是否关闭 connection。

### 练习 9：追 shell 命令的真实完成点

```bash
sed -n '345,405p' frameworks/base/cmds/input/src/com/android/commands/input/Input.java
sed -n '72,86p' frameworks/base/packages/Shell/AndroidManifest.xml
sed -n '380,565p' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
```

核对 WAIT_FOR_FINISH、display 修正、Shell 权限与 ASYNC 仍会拒绝非法事件的测试。

---

## 16. 本章结论与自检

最终模型可以压缩成：

```text
身份
= clear 前保存的 Binder pid/uid
+ root/signature 全局权限
+ 目标 ownerUid 局部例外

结果
= InjectionState.injectionResult
+ mInjectionResultAvailable

完成
= 最后带 state 的 EventEntry 所产生的 foreground DispatchEntry 计数
+ mInjectionSyncFinished
```

最重要的边界是：

- ASYNC 仍做同步合法性校验和 policy intercept，只是不等目标结果；
- 同 UID 放行依据 ownerUid，不是 Java 注释里的“同进程”，且不自动获得 TRUSTED；
- WAIT_FOR_RESULT 的 success 状态早于 target enqueue/publish，也可由 policy consume 产生；
- WAIT_FOR_FINISH 不数 monitor/OUTSIDE/wallpaper，不要求 handled=true，但 Key fallback 可延长 entry 生命周期；
- broken/unregister drain 可以让 finish count 归零，global reset 本身却不 drain connection 队列；
- 两段同步等待共享 native 入口起算的 30 秒，timeout 只结束 caller wait，不撤销事件；
- r48 Motion history 只让最后展开 entry 持有 InjectionState，早期 samples 不各自做 UID 检查或同步结账；
- HMAC 证明系统签名字段，不证明物理硬件来源或 UI 业务效果。

自检时应能回答：

1. 保存 caller pid/uid 与 `clearCallingIdentity()` 为什么不矛盾？
2. shared UID 为何会改变“自己的窗口”的准确含义？
3. ASYNC 为什么既可能立即 false，也可能 true 后后台 permission denied？
4. policy consume 为什么能让三种 mode 都得到 success？
5. result 已 SUCCEEDED 时，为何仍可能没有任何 foreground DispatchEntry？
6. 多 foreground target 怎样改变 WAIT_FOR_FINISH？
7. handled=false 的 Motion 与 Key fallback 对计数有何不同？
8. reset、broken、unregister 对已经在 waitQueue 的注入有何区别？
9. 调用超时后哪份引用让事件还能继续？
10. 带 history 的 Motion 为什么必须按“最后 entry 的账”理解？

下一章进入 **InputFilter、AccessibilityInputFilter 与事件变换/重新注入**，解释过滤器怎样截断原始流、通过 FILTERED 防止回环，并在安装/卸载时重置输入状态。
