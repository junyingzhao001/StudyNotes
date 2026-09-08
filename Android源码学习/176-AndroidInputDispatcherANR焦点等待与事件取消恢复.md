# 176 Android InputDispatcher ANR、焦点等待与事件取消恢复

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态只读源码，不要求编译或连接设备  
> 前置章节：第 18、20、113、173、174、175 章

---

## 1. “输入 ANR”至少有两条不同路径

看到日志里的 `Input dispatching timed out`，最容易形成的模型是：事件已经进 App，主线程五秒没处理。但 Android 11 还有一种发生在 publish 之前的输入 ANR：

```text
WMS 已指定 focused application
→ InputDispatcher 要为 Key/非 pointer Motion 找 focused window
→ 该应用一直没有可接收输入的 focused window
→ 当前事件停在 mPendingEvent
→ 按 application timeout 报告 ANR
```

另一条才是第 174—175 章的完成链超时：

```text
已找到 connection
→ socket publish 成功
→ DispatchEntry 进入 waitQueue
→ App 没有及时返回 FINISHED
→ 按 connection/window deadline 报告 ANR
```

两者的分界是 publish：

| 类型 | 事件位置 | 归因对象 | App 是否可能已经收到这笔事件 |
|---|---|---|---|
| no-focused-window | 全局 `mPendingEvent` | `InputApplicationHandle` | 否 |
| connection ANR | 某 connection 的 `waitQueue` | InputChannel token | 是 |

本章目标是学会从队列、deadline、token 和取消状态判断是哪一条路径，并说明 WMS/AMS 延长或终止等待后，native 真正做了什么。它不把 ANR 报告等同于杀进程，也不把合成 CANCEL 等同于客户端已经执行清理。

---

## 2. 源码地图与三只容易混淆的时钟

native 调度、超时索引与取消状态：

```text
frameworks/native/services/inputflinger/dispatcher/
├── InputDispatcher.cpp
├── InputDispatcher.h
├── AnrTracker.cpp / AnrTracker.h
├── InputState.cpp / InputState.h
└── CancelationOptions.h
```

Java policy 与进程归因：

```text
frameworks/base/services/core/
├── jni/com_android_server_input_InputManagerService.cpp
└── java/com/android/server/
    ├── input/InputManagerService.java
    ├── wm/InputManagerCallback.java
    ├── wm/ActivityRecord.java
    └── am/ActivityManagerService.java
```

r48 有三只经常同时出现在输入卡顿讨论中的时钟：

| 时长 | 用途 | 到点行为 |
|---|---|---|
| 默认 5s | application/window 没提供其他 dispatch timeout 时兜底 | 进入 ANR policy 链 |
| 2s | FINISHED 后计算出的慢处理告警 | 只写 slow log |
| 500ms | Key 等待更早未完成派发，以给潜在焦点变化机会 | 超时后仍把 Key 发给当前焦点 |

窗口与 application handle 可携带自己的 timeout，所以 ANR 不保证恰好五秒。三者都基于：

```cpp
systemTime(SYSTEM_TIME_MONOTONIC)
```

修改墙上日期不会直接推进 deadline。`mLastAnrState` 中用 `time(nullptr)` 生成的可读日期只用于展示，不参与超时比较。

---

## 3. 目标选择前先分清 focused application 与 window

两份焦点状态按 display 保存：

- focused application：WMS 认为正在成为交互主体的 Activity/application；
- focused window：当前窗口快照中真正可接收 focus-routed 输入的窗口；
- focused display：事件没指定 display 时使用的默认显示屏。

启动或窗口切换期间可以合法地短暂出现：

```text
focusedApplication = 新 Activity
focusedWindow      = null
```

Key 与非 pointer Motion 主要通过 `findFocusedWindowTargetsLocked()` 选目标。普通触摸 DOWN 走 `findTouchedWindowTargetsLocked()`，按坐标、touch region 与已有 `TouchState` 选窗；“无 focused window”不会自动冻结所有触摸。

### 3.1 没有 window 也没有 application 时立即失败

若目标 display 两者皆空，源码返回 `INPUT_EVENT_INJECTION_FAILED`。此时连“该等谁”都无法归因，不建立 no-focus timer；硬件事件会结束为 drop，同步注入则得到相应失败结果。

### 3.2 有 application、无 window 时才开始等待

只有某笔需要焦点路由的事件真正来到目标选择，且发现 application 存在、window 为空时，才执行：

```text
mNoFocusedWindowTimeoutTime = currentTime + application timeout
mAwaitedFocusedApplication  = focused application
当前事件返回 INPUT_EVENT_INJECTION_PENDING
```

所以计时起点不是 Activity 启动、resume 或 `setFocusedApplication()` 的时刻，而是 Dispatcher 第一次因一笔事件发现“有应用、无窗口”的时刻。

---

## 4. no-focus 事件停在全局 pending，timer 也是单份

目标选择返回 PENDING 后，同一笔事件仍由 `mPendingEvent` 持有，下一轮 dispatch loop 重试。此时它尚未：

- 为某个窗口创建 `DispatchEntry`；
- 进入某个 connection 的 `outboundQueue`；
- 写入 InputChannel socket；
- 进入 `waitQueue` 或 `AnrTracker`；
- 到达 App 的 `InputEventReceiver`。

这解释了 no-focus ANR 为什么只有 `InputApplicationHandle`，没有 connection token。

### 4.1 window 出现后复用原事件继续选目标

`setInputWindows()` 会唤醒 Dispatcher。下一次同一 pending event 看见有效 focused window 时，`resetNoFocusedWindowTimeoutLocked()` 同时清除 deadline 与 awaited application，然后继续检查注入权限、paused 和 Key 排序门。

“窗口出现”本身没有在 `setInputWindowsLocked()` 中直接清 timer；清理发生在事件重试并确认有 focused window 时。通常仍是原 pending event 继续，不是丢掉后重造。

### 4.2 application 换代只在匹配 awaited 时清 timer

`setFocusedApplication(displayId, new)` 会比较：

```text
该 display 的 old focused application == mAwaitedFocusedApplication
且 new != old
```

满足才 reset。这个条件避免其他 display 的 application 更新误清当前等待，但也说明 r48 的 no-focus 状态并不是 per-display map：

```text
mNoFocusedWindowTimeoutTime   // 全局单份
mAwaitedFocusedApplication    // 全局单份
```

同一时刻只有全局 pending 头事件正在选目标，这种设计与串行 dispatch loop 配套；分析多显示现场时仍不能凭每 display 焦点表臆造多只 timer。

---

## 5. no-focus 到期、abort 与继续等待怎样收尾

每轮 `dispatchOnce()` 在 command 处理之后调用 `processAnrsLocked()`。no-focus 条件满足时：

```text
currentTime >= deadline
→ onAnrLocked(awaited application)
→ 保存 mLastAnrState
→ command queue 加入 notifyANR
→ 清 mAwaitedFocusedApplication
→ 立即再跑一轮
```

注意这里只清 awaited application，没有清 `mNoFocusedWindowTimeoutTime`。随后 policy 有两种返回：

### 5.1 extension > 0

因为 no-focus 没有 connection，`extendAnrTimeoutsLocked()` 用：

```text
now() + extension → 新 no-focus deadline
原 application    → 重新写回 awaited
```

于是相同 pending event 继续等窗口，到下一次 deadline 可再次报告。

### 5.2 extension == 0

abort 分支按 null token 找不到 connection，直接返回，不合成 CANCEL。下一轮目标选择看到旧 deadline 已过，会返回 injection failed 并释放 pending event；在 timer 未被其他 reset 路径清除前，后续 focus-routed 事件也会立即失败。

`processAnrsLocked()` 用 `>=` 报告，到目标选择里判断“已经报告”用 `>`。正常循环先让 pending 重试，再在尾部检查 ANR；边界值附近不要把这两个比较写成同一行源码。

### 5.3 paused focused window 是第三种等待状态

一旦 focused window 非空，源码先 reset no-focus timer；若该 window 的 `paused=true`，焦点路由函数只返回 PENDING，没有为这条分支另建 no-focus 或 connection deadline。它依赖后续窗口状态更新唤醒并解除；普通 touch 命中 paused window 则把它排除，最终可能直接失败。

所以“所有 publish 前等待都会五秒 ANR”不成立。`paused` 是需要单独看窗口快照和注入调用方等待上限的门。

---

## 6. 新触摸可以剪枝，但不直接清 no-focus timer

当 no-focus 等待存在，新 pointer DOWN 入 inbound queue 时，`shouldPruneInboundQueueLocked()` 检查：

```text
坐标命中的 window 属于不同 application token
或者
该 display 至少有一个仍 responsive 的 gesture monitor
```

命中任一条件，就把这笔 DOWN 记为 `mNextUnblockedEvent`。在它之前成为 pending 的 Key/Motion 会被标为 `DropReason::BLOCKED`；FOCUS、configuration 与 device-reset 不走这条 drop 分支。走到 sentinel DOWN 本身时标记清除，它再正常选触摸目标。

这让用户有机会离开一个迟迟没有窗口的 App，也让系统手势监控者不被旧 pending 永久堵住。

但剪枝函数没有直接调用 `resetNoFocusedWindowTimeoutLocked()`。它通常依赖新 DOWN 引起 WMS 焦点/application 更新来清旧等待；在该更新真正到达前，旧 no-focus deadline 仍是 native 状态。不要把“旧 Key 已 drop”直接等价为“application ANR timer 已清”。

### 6.1 每笔 BLOCKED drop 还会触发全连接取消

`dropInboundEventLocked()` 对被丢的 Key/非 pointer Motion 合成 `CANCEL_NON_POINTER_EVENTS`，对 pointer Motion 合成 `CANCEL_POINTER_EVENTS`，而且调用的是 `synthesizeCancelationEventsForAllConnectionsLocked()`。

它不是只清理原本可能成为目标的一个窗口。一个全局 inbound 流被截断时，Dispatcher 选择跨所有 connection 修复对应类别的输入状态；这比“删掉 inbound 节点”影响更广。

---

## 7. 500ms Key 门实际查看全局 AnrTracker

焦点窗口有效后，Key 仍可能暂缓。设计动机是：更早触摸可能打开新窗口，用户紧接着按下的键应该有机会送给新焦点，而不是旧窗口。

源码注释常以 Motion 举例，但 r48 的实际条件只是：

```cpp
if (mAnrTracker.empty()) {
    return false;
}
```

因此它看到的是全局所有 responsive connection 已索引的未完成派发，不限定：

- 事件一定是 Motion；
- connection 一定是当前 focused window；
- display 一定与当前 Key 相同。

首次发现 tracker 非空时设置全局 `mKeyIsWaitingForEventsTimeout = now + 500ms`。更早事件全部完成、500ms 到期，或新 pointer DOWN 到来把该 timer 改为 `now()`，都会让 pending Key 继续发给那一刻的 focused window。

另一个边界是：connection 被判 unresponsive 后，`eraseToken()` 会从 tracker 删除它的所有索引，但 waitQueue 仍可非空。此时 Key 门可能因 tracker 空而放行；它并不是扫描所有 waitQueue 的“绝对先前事件屏障”。

500ms 到点只结束排序等待，不报告 ANR，也不改变已在 waitQueue 中事件自己的 deadline。

---

## 8. connection deadline 只在 publish 成功后入索引

目标确定后，`DispatchEntry` 先进入该 connection 的 outbound。`startDispatchCycleLocked(currentTime, connection)` 为队头写：

```text
deliveryTime = 本次函数参数 currentTime
timeoutTime  = currentTime + 当前 window/default timeout
```

然后尝试 `publishKeyEvent`、`publishMotionEvent` 或 `publishFocusEvent`。只有 socket send 成功才：

```text
从 outbound 删除
→ 加到 waitQueue 尾部
→ connection responsive 时把 (timeoutTime, token) 插入 AnrTracker
```

WOULD_BLOCK 会把条目留在 outbound；下一次 start cycle 会用新的 currentTime 覆盖 delivery/timeout。它尚未进入本次 connection ANR 计时。

`startDispatchCycleLocked()` 的 while 循环不会在每次 publish 前重新调用 `now()`。同一轮连续成功写出的多个 entry 会共享函数传入的 deliveryTime 基线，并在 timeout 相同时共享 deadline。这是近似批次时刻，不是每个 packet 独立取时。

### 8.1 waitQueue 与 socket 缓冲可同时表示同一笔事件

publish 成功只证明内核接受完整 packet。App 还没 read 时，packet 可在 socket 接收缓冲，而逻辑 `DispatchEntry` 已在 Dispatcher waitQueue：

```text
waitQueue 有 entry
≠ App Java callback 已开始
≠ View 已处理
≠ FINISHED 已在反向 socket
```

connection timeout 覆盖 socket 等 App 读取、App Looper、IME/ViewRoot/View、以及 FINISHED 返回 Dispatcher 的整个后半段；硬件采样到成功 publish 之前的排队不计入该 entry 的 delivery duration。

---

## 9. AnrTracker 是 deadline 索引，不是事件队列

其核心结构是：

```cpp
std::multiset<std::pair<nsecs_t, sp<IBinder>>> mAnrTimeouts;
```

只存 deadline 和 connection token。事件描述、deliveryTime、resolved action 与 target flags 仍在 `waitQueue` 的 `DispatchEntry`。使用 multiset 是因为同一 connection 的多个事件可以拥有相同 deadline；删除一个 `(time, token)` 只移除一份重复项。

每轮 `processAnrsLocked()` 先考虑 no-focus deadline，再与 `mAnrTracker.firstTimeout()` 取最早唤醒点。若 connection deadline 已到：

```text
connection.responsive = false
→ eraseToken(token)，删掉该 connection 的全部 deadline 索引
→ onAnrLocked(connection)
→ 返回 LONG_LONG_MIN，要求立即再跑
```

删除全部索引是为了避免同一不响应连接的第二、第三笔 entry 连续重复报告；waitQueue 本体没有清空，connection 状态也没有因此变为 BROKEN。

### 9.1 ANR reason 解释 oldest，不保证解释触发项

`onAnrLocked(connection)` 使用 `waitQueue.begin()` 的最老发送项计算等待时间并拼 reason。若窗口 timeout 在多笔发送之间变化，最先到期的 tracker entry 可能不是最老 wait entry。

```text
触发 ANR 的最小 deadline
不保证等于
reason 文本里展示的 oldest event
```

源码认为 App 多半线性处理，所以最老未完成项更有诊断价值。精确复盘仍要同时看每笔 wait 与当时 timeout。

### 9.2 waitQueue.empty 检查是防御，不是常规 ACK 竞态

`onAnrLocked(connection)` 开头确实在队列为空时放弃报告。但正常路径中，`processAnrsLocked()` 设置 responsive、erase tracker 并调用 `onAnrLocked()` 都持同一把 Dispatcher 锁、在同一 Dispatcher 线程连续执行；FINISHED 不能插进这几行之间把队列清空。

真正释放 native 锁发生在稍后的 policy command。回调回来后，其他线程可能已 unregister connection，所以 extend/abort 路径会重新按 token 查 connection。不能用开头的 empty 防御证明“到期后、snapshot 前 ACK 可并发取消本次报告”。

---

## 10. native 保存现场后，把处置决定交给 WMS/AMS

两种 `onAnrLocked()` 都先调用 `updateLastAnrStateLocked()`，把 reason、可读墙上时间和当时的完整 dispatcher dump 存入 `mLastAnrState`，再向 command queue 投递 `doNotifyAnrLockedInterruptible`。

command 执行时主动：

```text
unlock InputDispatcher.mLock
→ mPolicy->notifyAnr(application, token, reason)
→ lock InputDispatcher.mLock
```

这是同步 policy 调用，但不持 Dispatcher 全局锁。调用期间 Dispatcher 线程本身被占用，不会同时处理 Looper 上的 FINISHED；其他线程仍可能改变窗口或注销连接。

Java 归因顺序是：

```text
token → WMS mInputToWindowMap → WindowState / ActivityRecord / owner PID
token 非普通窗口 → EmbeddedWindowController → owner PID / host 层级
仍无 Activity 且有 application handle → application token → ActivityRecord
```

WMS 在 global lock 内记录窗口现场，释放后再保存 ATMS 状态并调用 AMS。`preDumpIfLockTooSlow()` 只在 debuggable build 工作，用额外线程探测 WMS/AMS 锁并在过慢时预抓现场；它不改变 native deadline。

JNI policy callback 若抛 Java 异常，会清异常并把 extension 置 0，也就是 native 按 abort 处理，而不是无限重试回调。

---

## 11. AMS 返回的是“是否继续等”，不是杀进程完成点

Activity 路径：

```text
ActivityRecord.keyDispatchingTimedOut(...)
→ boolean abort
→ abort=false：返回 activity.mInputDispatchingTimeoutNanos
→ abort=true ：返回 0
```

只有 PID 的窗口路径：

```text
AMS.inputDispatchingTimedOut(...)
→ 非负毫秒：InputManagerCallback 转成纳秒返回
→ 负值：InputManagerCallback 返回 0
```

最终仍以 native 的 `timeoutExtension > 0` 为继续等待条件；所以正数才延长，Java 返回 0 即使经过“非负”分支也会落到 abort。

AMS 对正在调试的进程返回“不 abort”，因此 policy 给 Dispatcher 一段新 timeout；active instrumentation 则会被结束并返回 abort。调试与 instrumentation 不是同一豁免。

普通 ANR 工作由 `mAnrHelper.appNotResponding(...)` 发起，后续 traces、对话框或进程处置还有自己的异步阶段。故：

```text
InputDispatcher 发现 deadline
≠ AMS 已采完 traces
≠ ANR 对话框已显示
≠ 进程已退出
```

WMS `notifyANR` 还记录整次回调耗时，因为它运行在 InputDispatcher thread 上；policy 自身变慢会继续阻塞该线程处理其他输入。

---

## 12. extension 会重建 deadline，但 r48 不是无条件全量重建

connection ANR 发生时，旧 tracker 索引已由 `eraseToken()` 全删。policy 返回正 extension 后：

```text
connection.responsive = true
newTimeout = now() + extension
遍历 waitQueue
若 newTimeout >= entry.timeoutTime：
    entry.timeoutTime = newTimeout
    AnrTracker.insert(newTimeout, token)
```

通常所有旧 deadline 都早于新的 extension，于是一起重新受监控。但条件是 `>=`：若某个较新的 wait entry 原 deadline 比 `newTimeout` 更晚，代码保留其 deadline，却没有把它重新插入刚被清空的 tracker。

这是 r48 的索引边界：

```text
waitQueue 仍有 entry
+ connection 被设回 responsive
不必然推出
每个 surviving entry 都重新存在于 AnrTracker
```

后续新派发可再插入该 token 的新索引，但不能据此反推所有旧 entry 已恢复逐笔 deadline 追踪。分析定制 timeout 或较短 extension 时尤其要看源码条件。

no-focus extension 没有此循环，只把单份 timer 改为 `now()+extension` 并恢复 awaited application。

---

## 13. abort 合成的是状态终止事件，不会清旧队列或断通道

connection policy 返回 0 后，native 重新按 token 查 connection。若已注销就结束；若仍是 `STATUS_NORMAL`，调用：

```text
CancelationOptions(CANCEL_ALL_EVENTS, "application not responding")
→ connection.inputState.synthesizeCancelationEvents(...)
→ 为合成事件创建新的 DispatchEntry
→ 追加到同一 connection outbound
→ startDispatchCycleLocked()
```

它不会：

- erase 原有 waitQueue；
- 删除原 outbound 中尚未发送的普通事件；
- 把 connection 标成 BROKEN/ZOMBIE；
- 直接杀 App；
- 保证卡住的客户端马上读到 CANCEL。

connection 可继续保持 `STATUS_NORMAL + responsive=false`，直到窗口/进程处置触发 unregister，或 channel 错误进入 broken 清理。

### 13.1 InputState 在入 outbound 时已经更新

容易误写成“InputState 只记录已 publish 给客户端的状态”，但 r48 在 `enqueueDispatchEntryLocked()` 中先调用 `trackKey/trackMotion()`，然后才把新 entry push 到 outbound。

因此 memento 表示 Dispatcher 计划交给该 connection 的有序流，可能包含尚未成功 publish 的 entry。ANR 取消可据此为它生成终止事件；只要 channel 保持 FIFO 且最终可写，原事件会排在 CANCEL 前到达。

合成的 Key UP、Motion CANCEL/HOVER_EXIT 在再次 enqueue 时又进入 `trackKey/trackMotion()`，立刻把相应 memento 清掉——仍早于客户端真正收到终止 packet。若之后通道断开或长期反压，服务端状态已归零不代表客户端执行了清理。

### 13.2 CANCEL_ALL 不等于“删除所有 DispatchEntry”

InputState 保存：

- 尚未配对 UP 的 Key DOWN；
- 活跃 pointer/non-pointer Motion 状态；
- hover 状态；
- fallback key 映射。

取消结果主要是 canceled Key UP、Motion CANCEL 或 HOVER_EXIT。FOCUS entry 不在这套 memento 中；旧 outbound/wait entry 仍保留。名字里的 ALL 指符合状态类别的全部 tracked key/motion，而不是清空 connection 的所有事件对象。

---

## 14. unresponsive 后的新输入与迟到 FINISHED 都有边界

新 pointer gesture 选窗时，unresponsive window 被排除；新 gesture monitor 也经 `selectResponsiveMonitorsLocked()` 过滤。若两者都没有，DOWN 失败，连 WATCH_OUTSIDE 目标也不会因这个无效主目标收到事件。

限定词是“新 gesture”。已经建立的 `TouchState` 后续 MOVE/UP 走持续路由，不在每一笔上重新执行同一 responsive 过滤。焦点路由也没有统一的 responsive 拒绝门；源码注释明确说 focused events 还会积压。

`startDispatchCycleLocked()` 对 `responsive=false` 仍可尝试 publish，只是不向 AnrTracker 插入新 deadline。因此取消事件、焦点事件或既有手势后续可能继续进入 outbound/wait，但在连接恢复前不形成新的逐笔 ANR 闹钟。

### 14.1 迟到 FINISHED 只按剩余 wait deadline 重算 responsive

收到某 seq 的 FINISHED 后，Dispatcher 删除对应 wait entry，并在 connection 当前不 responsive 时扫描剩余队列：

```text
存在 entry.timeoutTime < now → 仍 false
否则                        → true
```

这里使用 `<`，而 `processAnrsLocked()` 的触发比较是 `>=`；精确等于 deadline 时两处边界并不一致。

更重要的是，late-FINISHED 路径把 `responsive` 改回 true 后，没有把剩余、尚未过期的旧 wait entry 重新插入先前已 `eraseToken()` 的 AnrTracker。以后新 publish 会产生新索引，但旧 surviving entry 本身可能没有恢复闹钟。

所以“迟到一笔 ACK 后恢复 true”只说明这一刻剩余 entry 尚未严格过期，不证明旧 waitQueue 的 ANR 索引已完整重建。若还有已过期项，则保持 false；若 connection 已被 unregister，FINISHED 也无法恢复它。

### 14.2 dispatch freeze 不宜理解成暂停所有既有 ANR 时钟

`dispatchOnceInnerLocked()` 在 `mDispatchFrozen` 时提前返回，注释说不投递新事件；但 r48 外层 `dispatchOnce()` 之后仍无条件调用 `processAnrsLocked()`。因此仅凭 frozen 不能推断已登记的 no-focus/connection deadline 会暂停累加。解冻路径会 reset no-focus timer，却不会把已有 connection deliveryTime 整体平移。

这是按 r48 控制流可得的静态结论；真实产品是否长期使用 frozen、上层是否同时清理状态，需要设备现场另证。

---

## 15. 用队列定位现场，并完成九组静态练习

先按证据分流：

| 现场 | 更可能的阶段 | 下一步 |
|---|---|---|
| 有 focused app、无 focused window、PendingEvent 很老 | no-focus | 查 WMS 窗口创建/同步与 application timeout |
| focused window `paused=true`、pending 不前进 | publish 前 paused gate | 查谁设置/解除 paused；不要套 connection ANR |
| outbound 增长、wait 非空 | 正向 socket 反压 | 查 App 消费、channel 与旧 wait |
| wait 很老、responsive=true | deadline 尚未到或索引/时间异常 | 查每笔 wait、window timeout、tracker 逻辑 |
| wait 很老、responsive=false | 已报告 connection ANR | 查 policy 返回、cancel entry、App/进程处置 |
| status=BROKEN/ZOMBIE | 连接错误或注销 | 查 unregister/broken drain，不再只谈响应慢 |
| saved ANR reason 与最短 deadline 对不上 | reason 使用 oldest entry | 对照全部 wait entry 的 timeout 代际 |

当前 dump 会给出 focused applications/windows、PendingEvent、InboundQueue、每个 connection 的 status/responsive/outbound/wait，以及最近一次保存的 ANR snapshot。它是 Dispatcher 单锁下的本地快照，不与 WMS、AMS traces 或 App 主线程现场原子对齐。

以下命令只读 `android-11.0.0_r48` 工作树。

### 练习 1：区分两个 ANR 起点

```bash
sed -n '1447,1507p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2456,2595p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

分别标出 no-focus timer、deliveryTime、publish 成功与 tracker insert；解释为什么前者没有 token。

### 练习 2：证明剪枝不直接 reset timer

```bash
sed -n '688,735p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '640,680p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

找出 `mNextUnblockedEvent` 的设置与消费，并确认这两段没有清 no-focus 成员。

### 练习 3：核对 500ms Key 门的真实范围

```bash
sed -n '1412,1450p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2705,2745p' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
```

回答实现为什么是全局 tracker gate，以及新 pointer DOWN 如何只把 timer 推到当前时刻。

### 练习 4：拆开 tracker 与 waitQueue

```bash
sed -n '20,95p' frameworks/native/services/inputflinger/dispatcher/AnrTracker.h
sed -n '20,85p' frameworks/native/services/inputflinger/dispatcher/AnrTracker.cpp
sed -n '489,525p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

说明 `eraseToken()` 删除什么、不删除什么，以及相同 deadline 为什么能有重复项。

### 练习 5：追 ANR snapshot 与 Java policy

```bash
sed -n '4545,4698p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '175,275p' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
sed -n '19819,19890p' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

写出锁释放、token/application 归因、debugger、instrumentation 与 extension 单位转换。

### 练习 6：验证 extension 的索引边界

```bash
sed -n '4650,4698p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

假设某 surviving entry 原 deadline 晚于 `now()+extension`，判断它是否仍在 wait、是否被重新 insert。

### 练习 7：证明 InputState 早于 publish

```bash
sed -n '2298,2405p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2799,2870p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '230,310p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

画出 track → outbound → publish 与 synthesize → track cancel → outbound 的先后。

### 练习 8：检查迟到 FINISHED 的恢复缺口

```bash
sed -n '4741,4808p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

找出 `< now` 比较和 `responsive=true`，再确认此路径是否为剩余旧 entry 重建 tracker。

### 练习 9：对照单元测试与 dump

```bash
sed -n '2360,2640p' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
sed -n '4035,4290p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

用 basic ANR、no-focus extension、相同 deadline 与新手势过滤测试，对照 dump 中能观察和不能观察的状态。

---

## 16. 本章结论与自检

最终模型是三层账加一个 policy 决策：

```text
publish 前
= mPendingEvent + focused application/window + 单份 no-focus timer

publish 后
= per-connection outbound/wait + delivery/timeout + 全局 AnrTracker 索引

状态修复
= enqueue 时更新的 InputState memento + 合成 Key UP/Motion CANCEL/HOVER_EXIT

处置
= native snapshot + 解锁回调 WMS/AMS + extension 或 abort
```

必须记住这些边界：

- no-focus timer 从第一笔焦点路由事件发现缺窗口时开始，不从 Activity 启动开始；
- 有效 focused window 出现后要等 pending 重试才清 timer；paused window 又是无独立 ANR deadline 的 publish 前门；
- 触摸剪枝只标 sentinel 并 drop 之前的 Key/Motion，不直接清 no-focus timer；
- 500ms Key 门看全局 responsive tracker，不只看 Motion、当前窗口或当前 display；
- connection deadline 从成功 publish 起算，同一 send 批次可共享 currentTime；
- tracker 只索引 deadline+token，reason 却描述 waitQueue oldest，两者可不是同一 entry；
- WMS/AMS 返回 0 只让 native尝试合成状态终止事件，不等于杀进程、清队列或关闭 channel；
- InputState 在 entry 入 outbound 时更新，CANCEL 的服务端记账也早于客户端收到；
- extension 与 late-FINISHED 两条恢复路径在 r48 都存在 surviving wait entry 未重新入 tracker 的边界；
- unresponsive 主要排除新 pointer gesture，既有手势与 focused events 仍可能继续排队/发送。

自检时应能回答：

1. 两类输入 ANR 的 publish 分界与归因对象分别是什么？
2. 为什么 no-focus 事件不可能已经进入目标 App？
3. focused window 为 paused 时为何不能套用 no-focus 五秒模型？
4. 新 DOWN 剪枝了 pending Key，为什么 timer 仍可能暂时存在？
5. 500ms Key 门为何可能被其他 display 的非 Motion 派发影响？
6. `eraseToken()` 为什么既必要，又不等于 waitQueue 清空？
7. policy extension 如何重建 deadline，哪个条件可能漏掉旧 entry？
8. CANCEL_ALL 为什么既可能依据未 publish 的状态，又不会删除旧 DispatchEntry？
9. connection 恢复 responsive 为什么不保证旧 tracker 索引完整？
10. native ANR、AMS traces、进程退出和客户端执行 CANCEL 为什么是四个完成点？

下一章进入 **InputDispatcher 输入注入、权限校验与同步等待**，解释 shell/test 事件怎样进入同一派发链，以及 WAIT_FOR_RESULT、WAIT_FOR_FINISHED 到底各等什么。
