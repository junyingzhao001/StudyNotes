# 237 Android 输入事件注入权限、注入模式与可信事件验证链

上一章已经把触摸目标、`TouchState`、多指拆分和 pilfer 串成一条路由链。本章换到软件注入者的视角：一次 `injectInputEvent()` 调用到底在什么位置完成，谁有权把事件送进哪个窗口，`WAIT_FOR_FINISH` 等到的“完成”是什么，以及 `verifyInputEvent()` 能证明哪些字段。

先给结论：**注入返回值、窗口是否收到事件、前台分发是否释放、事件字段是否可验证，是四个不同问题。** `ASYNC` 只是不等待路由结果；`WAIT_FOR_RESULT` 等的是 dispatcher 的注入结果；`WAIT_FOR_FINISH` 再等已建立的 foreground `DispatchEntry` 全部释放；`VerifiedInputEvent` 则只认证发布时选定的一小组字段。把其中任意两层合并，都会得到过强的结论。

本文以 `android-11.0.0_r48` 为准，核心源码位于：

- `frameworks/base/core/java/android/hardware/input/InputManager.java`
- `frameworks/base/core/java/android/hardware/input/IInputManager.aidl`
- `frameworks/base/services/core/java/com/android/server/input/InputManagerService.java`
- `frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp`
- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp`
- `frameworks/native/services/inputflinger/dispatcher/InjectionState.h` 与 `InjectionState.cpp`
- `frameworks/native/services/inputflinger/dispatcher/Entry.cpp`
- `frameworks/native/include/input/Input.h` 与 `frameworks/native/libs/input/Input.cpp`
- `frameworks/base/core/java/android/view/VerifiedInputEvent.java`、`VerifiedKeyEvent.java` 与 `VerifiedMotionEvent.java`

## 1. 先分四本账：受理、路由、释放与字段认证

一条注入事件至少要经过四本账：

| 账本 | 关键状态 | 它能回答什么 | 它不能回答什么 |
|---|---|---|---|
| 入口受理 | 参数、事件类型、结构校验、是否入 inbound queue | 调用是否已被 dispatcher 接纳 | 最终目标是否合法 |
| 目标路由 | `injectionResult` | policy 是否消费、是否找到目标、目标权限是否通过 | App 是否回 finished |
| 前台释放 | `pendingForegroundDispatches` | 被实际排入队列的 foreground 分发项是否都已释放 | 业务成功、绘制或上屏 |
| 字段认证 | HMAC 与 `VerifiedInputEvent` | 受保护字段是否匹配本次 dispatcher 的签名 | 完整对象、物理硬件来源或一次性使用 |

这四本账不是严格的一对一关系。policy 消费可以让路由账写成成功，却没有任何目标；目标消失可以靠清队列把释放账归零，却没有 App 回调；一个无全局注入权限的进程可以注入到同 UID 窗口，最终事件仍会由 dispatcher 签名；普通 `MOVE` 又会因 HMAC 为全零而无法验证，即使它确实来自系统输入链。

所以排查“`injectInputEvent()` 返回了 true，但界面没有反应”时，不能从一个布尔值跳到 View。先确认调用停在哪本账，再沿下一本账继续。

## 2. 同步Binder进入IMS后保存调用者身份，再进JNI

`InputManager.injectInputEvent()` 先拒绝 null 和三种常量之外的 mode，然后通过 `IInputManager` 发起同步 Binder 调用。即使 mode 名叫 `ASYNC`，这次 Java→system_server→JNI 的方法调用本身仍是同步的；“异步”只描述 native 入队后不等待 `InjectionState` 结果。

`InputManagerService` 又做一遍参数校验，并在清除 Binder identity 之前保存 `Binder.getCallingPid()` 与 `Binder.getCallingUid()`：

```text
caller pid/uid
  → clearCallingIdentity()
  → nativeInjectInputEvent(..., saved pid, saved uid, mode, 30s, DISABLE_KEY_REPEAT)
  → restoreCallingIdentity()
```

因此 native 看见的是原始注入者，而不是清 identity 后的 system_server 身份。`finally` 只保证 identity 恢复，不改变 native 返回值。

JNI 只接受 `KeyEvent` 与 `MotionEvent`。对象转换失败或类型不是这两者会抛运行时异常；它不会把任意 `InputEvent` 子类悄悄降级为普通失败。JNI 完成 Java 对象转换后才调用 `InputDispatcher::injectInputEvent()`，所以后文的 30 秒 native deadline 不包含 Java 排队、Binder 传输与 JNI 转换之前的时间。

## 3. native先做结构校验与规范化，不替调用者证明完整序列

native 一进入注入函数就建立共同 deadline、设置 `POLICY_FLAG_INJECTED`，再按事件类型规范化。

Key 只接受 `DOWN` 与 `UP`，因此 Java API 虽定义了 `ACTION_MULTIPLE`，r48 注入路径仍会返回失败。合法 Key 的输入 `deviceId` 被改成 `VIRTUAL_KEYBOARD_ID`；id、source、displayId、action、flags、scanCode、repeatCount、downTime 与 eventTime 等继续传入，`accelerateMetaShortcuts()` 还可能改写 keyCode/metaState。传入 HMAC 不会沿用，构造内部事件时直接使用 `INVALID_HMAC`。

Motion 的检查更细：

- masked action 必须在支持集合内；
- `POINTER_DOWN/POINTER_UP` 的 action index 必须落在 pointer 数组中；
- `BUTTON_PRESS/BUTTON_RELEASE` 的 actionButton 不能为 0；
- pointerCount 必须在 1 到 `MAX_POINTERS`；
- 每个 pointer ID 必须在 0 到 `MAX_POINTER_ID`，且本事件内不重复。

这些是单个对象的结构约束，不会证明调用者提交了完整的 `DOWN→MOVE→UP` 序列，也不会逐个判断 pressure、orientation 等轴值是否符合硬件现实。合法 Motion 同样把 deviceId 改成 `VIRTUAL_KEYBOARD_ID`，并在构造 `MotionEntry` 时把输入事件的 x/y offset 应用到每个 sample 的坐标。

## 4. INJECTED、TRUSTED、FILTERED与禁重复键各管一层

`POLICY_FLAG_INJECTED` 表示“来自注入入口”；`POLICY_FLAG_TRUSTED` 表示注入函数开头那次 `hasInjectionPermission(pid, uid)` 成功。它们可以同时出现，也可能只有 INJECTED。uid 0 直接通过，其余调用者由 policy 回调到 system_server 的 `Context.checkPermission(INJECT_EVENTS, pid, uid)`。

TRUSTED 不是最终目标授权凭证。目标窗口尚未选出，native 此时只能记录一次早期权限快照；真正的跨 UID 裁决仍在目标选择阶段进行。反过来，同 UID 注入可以没有全局权限并保持 untrusted，但仍合法进入自己的窗口。

IMS 固定附加 `FLAG_DISABLE_KEY_REPEAT`。dispatcher 只有在 Key 为 trusted 且没有该标志时才启动自己的重复键状态，因此常规 IMS 注入不会靠长按自动合成后续 repeat；调用者传入的 repeatCount 仍是事件数据。

未带 `POLICY_FLAG_FILTERED` 时，Key/Motion 在加锁入 inbound queue 前分别经过 `interceptKeyBeforeQueueing()` 或 `interceptMotionBeforeQueueing()`。输入过滤器回注则带 FILTERED，避免再次走同一前置过滤。policy 可以修改 `PASS_TO_USER` 等 flag，所以“结构合法”仍不保证稍后一定寻找窗口。

## 5. Motion历史被拆成多条Entry，但只有最后样本绑定InjectionState

MotionEvent 的 historical samples 不会作为一个不可分割对象进入 dispatcher。r48 从 sample 数组首项开始，为每个历史项以及当前项各建一条 `MotionEntry`，按数组存储顺序放入 `injectedEntries`；代码本身不重排或验证 eventTime 单调。每条展开项都复用输入对象的同一 action/actionButton，只推进 sampleEventTime 与 PointerCoords。

真正需要仔细看的顺序是：

```text
构造 N 条 MotionEntry
  → new InjectionState(injectorPid, injectorUid)
  → 只给 injectedEntries.back() 绑定 injectionState
  → N 条 Entry 依次进入 inbound queue
```

也就是说，只有最后一个、也就是当前 sample 参与目标权限、`injectionResult` 和 `pendingForegroundDispatches` 这三类注入账。更早的 historical Entry 虽仍带 `POLICY_FLAG_INJECTED`，却因 `injectionState == nullptr` 而不具备注入者身份；`checkInjectionPermission()` 对它们直接通过，foreground 计数也不会归到本次调用。

这是 r48 源码可静态确认的边界：带历史的 Motion 调用，其返回值与 FINISH 等待只代表最后 sample；早期 sample 的跨 UID 目标检查也缺少注入者账。它是否能在某个具体产品、入口权限和事件序列下形成可利用路径，还需要设备级实验，不能仅凭这段静态代码下结论。

`InjectionState` 用手工引用计数跨越调用栈：调用线程持一份，最后 Entry 及其派生/拆分项继续持有。同步调用超时释放自己的引用，并不会把仍在队列中的对象或事件一并销毁。

## 6. 权限在真实目标出现后裁决，NonReentrant允许持锁调用

`checkInjectionPermission(window, state)` 的规则很短：

1. 没有 `InjectionState`，直接允许；
2. 有窗口且 `window.ownerUid == injectorUid`，直接允许；
3. 窗口为空或目标跨 UID，重新调用 `hasInjectionPermission()`；
4. 函数只返回 bool：focused 或 pointer foreground 调用点把 false 记为 permission denied；失败尾的 null-target 检查若为 false，则保留此前的 failed 等结果，并阻止未授权状态提交。

“自己的窗口”按 UID，不按 PID，也不是按 package 名。于是一个没有 `INJECT_EVENTS` 的调用者仍可注入同 UID 窗口；想注入其他 UID，或走需要 null-target 收口的路径，才需要全局权限。

早期 TRUSTED 与后期授权是两次检查，不是同一个永久决定。权限若在两次之间变化，可能出现“policyFlags 已带 TRUSTED、后期却拒绝”，也可能出现“未带 TRUSTED、后期跨 UID 授权通过”。同 UID 分支不做第二次全局检查。

接口名 `checkInjectEventsPermissionNonReentrant` 也容易被误读。policy 接口的注释明确承诺其实现不会重入 dispatcher，并且可以在持有其他锁时调用；这不是“必须移到锁外”的意思。JNI 回调若抛异常，native 会清除异常并把结果当成 false。

## 7. focused、pointer与monitor走不同的目标授权矩阵

不同路由不能套用同一条“先检查权限，再找目标”的模板：

| 路由 | 目标如何出现 | 注入权限检查 | 无可用目标时 |
|---|---|---|---|
| Key / non-pointer Motion | 当前 focused window | 对选中的 focused window 检查；同 UID可过 | 无 focused app/window 可失败；有 app 无 window、paused window 或 Key 等前序事件时可 pending |
| pointer Motion | TouchState 与 hit-test 得到的 foreground windows | 遍历每个 foreground window；任一拒绝则 permission denied | 没 foreground 且没 gesture monitor 时失败 |
| gesture monitor-only pointer | gesture monitor 已进入临时 TouchState | r48 直接把 permission 记为 granted | monitor 可单独让本次目标选择成功 |
| global monitor | 主目标结果确定后追加 | 不作为独立授权目标 | 不能把主路由失败救成成功 |
| outside | 初始 DOWN hit-test 扫描时收集；可与 monitor-only 成功并存 | 不逐项作为权限主体 | 单独存在仍不足以成功 |
| wallpaper | foreground DOWN 命中的窗口声明 hasWallpaper 后加入 | 不逐项作为权限主体 | 不能单独构成成功 |

pointer 的 paused 新窗口在上一章 Case 1 中会被置空，不会像 focused 路由那样返回 paused-pending。对初始 DOWN 而言，若状态里最终既没有 foreground window，也没有 gesture monitor，结果才是 failed；split 流中途的 POINTER_DOWN 即使新窗口被置空，旧 TouchState 仍可能保有 foreground window，让本次沿旧目标成功。另一方面，monitor-only pointer 也不会再调用 null-target 权限检查，因为代码已把 `injectionPermission` 记为 granted。这正是为什么不能把 focused 的等待语义或 null-window 权限规则机械套到 pointer。

在 pointer 失败尾，如果权限状态仍 unknown，代码才用 null window 做最终检查；未授权事件不能据此提交真实 `TouchState`。但 monitor-only 分支已经 grant，走的是另一条边界。

这里的 granted 只描述“已注册且本次被选中的 gesture monitor”参与目标裁决后的逐事件路径，不会绕过 monitor 创建能力。`InputManagerService.monitorGestureInput()` 先检查 `MONITOR_INPUT`，而 r48 core manifest 把它声明为 signature 权限；普通第三方调用者不能靠 monitor-only 特例自行注册系统级 gesture monitor。

## 8. 路由成功可以没有窗口投递，失败布尔值也会合并多种原因

dispatcher 在 Key/Motion 正常路由时，先得到 target selection 的结果并调用 `setInjectionResult()`，成功后才进入 `dispatchEventLocked()`。因此 `WAIT_FOR_RESULT` 的普通成功表示目标选择阶段通过，并不等 App finished；结果落账以后，目标 Channel 消失、Connection 已坏或 `InputState` 判定流不一致，仍可能让具体 `DispatchEntry` 无法入队或发布。

还有一个必须单列的空投递成功：当 drop reason 是 `POLICY` 时，`dispatchKeyLocked()` 与 `dispatchMotionLocked()` 都把注入结果写成 `INPUT_EVENT_INJECTION_SUCCEEDED`，随后直接结束，不生成窗口 `DispatchEntry`。所以 RESULT 成功既可能是“已有合法路由”，也可能是“policy 已消费，无需路由”。

其他 drop reason、找不到目标、结构非法都会落到 failed；同步等待到 native deadline 则是 timed out；跨 UID 授权失败是 permission denied。Java 层把它们压缩为：

- permission denied：抛 `SecurityException`；
- succeeded：返回 true；
- timed out 或 failed：返回 false。

因此 false 不能区分结构错误、路由错误与超时，必须结合日志和事件形态判断。SecurityException 也不只代表跨 UID：已有 active pointer stream 时，来自不同 device/source 的 MOVE 会直接得到 `INPUT_EVENT_INJECTION_PERMISSION_DENIED`；注入 Motion 又统一使用 `VIRTUAL_KEYBOARD_ID`，所以设备冲突也可能走到同一 Java 异常。`ASYNC` 的后期 permission denied 不会回到调用者；对常规非 FILTERED 注入，native 只会把真实异步结果写入日志。

## 9. 三种mode的核心差别是入队后的等待点

三种 mode 的精确完成点如下：

| mode | native 在哪里返回 | true 能推出什么 |
|---|---|---|
| `ASYNC / SYNC_NONE` | Entry 入 inbound queue、必要时 wake looper 后立即写成功 | 入口前置步骤已通过且事件已入队 |
| `WAIT_FOR_RESULT` | `injectionResult` 不再 pending，或共同 deadline 到期 | 若成功，路由结果已确定；也可能是 policy 消费 |
| `WAIT_FOR_FINISH` | 先等 RESULT 成功，再等 foreground 计数归零；两段共用 deadline | 已计数的 foreground `DispatchEntry` 均已释放 |

`ASYNC` 不是无条件 true。JNI 类型检查、Key/Motion 结构校验都发生在入队前，所以非法类型会抛异常，非法 action、pointer 数量或重复 ID 会同步返回 false。权限预查和 pre-queue policy 回调也在返回前执行，可能消耗真实墙钟时间。“Never blocks”应理解为不等待 dispatcher 路由/finished 条件，而不是零耗时、零 Binder 阻塞。

合法事件一旦入队，ASYNC 就先返回 succeeded。之后才发现无目标或跨 UID 无权限时，调用者仍已拿到 true。反过来，WAIT_FOR_RESULT 会等这类结果，但不会进入 App finished 协议。

## 10. WAIT_FOR_FINISH数的是实际入队的foreground DispatchEntry

`pendingForegroundDispatches` 不是目标数，也不是 App 回调数。`enqueueDispatchEntryLocked()` 先构造面向某条 Connection 的 `DispatchEntry`，解析目标 action/flags，并让 `InputState.trackKey()/trackMotion()` 检查流一致性；只有该项带 `FLAG_FOREGROUND` 且真的走到 outbound 入队前，计数才加一。

这带来四个边界：

- global/gesture monitor、outside 与 wallpaper 没有 foreground flag，不计数；
- policy 消费或合法但零 foreground 的成功，计数从未增加，FINISH 会空等后立即通过；
- split 到多个 foreground 窗口时，最后 sample 可产生多个计数，必须全部释放；
- 流状态不一致而被 `trackKey/trackMotion` 跳过的项不会增加计数。

正常路径在客户端 finished signal 被处理后，从 waitQueue 移除并 `releaseDispatchEntry()`，计数才减一。触发 `abortBrokenDispatchCycleLocked()` 的 publish 失败、Connection 断裂或注销而清队列时，`drainDispatchQueue()` 也会逐项 release。`WOULD_BLOCK` 且 waitQueue 非空时会保留 outbound、等待前项完成；若 waitQueue 为空，同一状态反而被视为异常并 abort/drain。所以 FINISH 可能因异常清理而结束，并不证明应用代码正常回了 finished。

`handled=false` 对 Motion 不触发重派，随后释放；对 Key 则可能进入 `dispatchUnhandledKey()` fallback，把同一 `DispatchEntry` 放回 outbound queue。此时计数不先减再加，而是一直保留到 fallback 最终完成或被清理。因而“App 回了一次 false 就算 FINISH 完成”对 Key 并不成立。

## 11. 注入deadline与窗口ANR是两套时钟，超时不会撤销事件

IMS 传入固定 30 秒，但 deadline 在 `InputDispatcher::injectInputEvent()` 开头、JNI 完成对象转换之后建立。后续的权限预查、policy 回调、排队以及同步等待都消耗这同一预算。它也不是会中断慢回调或锁等待的硬计时器：代码只在进入两段条件变量等待时计算剩余时间。

WAIT_FOR_FINISH 没有“先给 RESULT 30 秒，再给 FINISH 30 秒”。若 RESULT 用掉 25 秒，foreground 释放最多只剩约 5 秒。ASYNC 不进入这两个等待循环，timeout 参数不会成为队列中事件的自动过期时间。

ANR 还要拆成两种。存在 focused application 却没有 focused window 时，目标选择保持 pending，并按 application timeout 报无焦点窗口 ANR；此时尚未创建 `DispatchEntry`。focused window 已存在但 paused 时同样返回 pending，但该分支不建立上述无焦点窗口 ANR deadline。

已发布 Connection 的 dispatch timeout 则属于每条 `DispatchEntry.timeoutTime`，从 publish 阶段按窗口配置计算。ANR policy 可以延长等待；若决定不再延长，r48 会为该 Connection 合成取消事件并停止把它当作 responsive，但这一步本身不释放原 waitQueue 项。原项仍要靠后续 finished，或 channel 断裂、注销后的清队列释放。这两类 ANR 与注入调用 deadline 都不是相加公式。

最关键的是：同步调用超时时，代码只把当前栈上的局部 `injectionResult` 设为 timed out 并返回；它没有从 inbound/outbound/wait queue 删除事件，也没有把 `InjectionState.injectionResult` 强制写成 timed out。事件稍后仍可能选中目标、发布并被处理。调用方重试前必须考虑重复操作风险。

## 12. HMAC在面向目标发布前计算，签的是最终action与受保护flags

事件进入某条 Connection 前，`createDispatchEntry()` 与 `enqueueDispatchEntryLocked()` 会解析该目标真正看见的 action、flags 和必要的派生 event ID。到 `startDispatchCycleLocked()` 发布时，dispatcher 才调用 `getSignature()`。

Key 每次发布都从 `KeyEntry` 生成 `VerifiedKeyEvent`，再把 action 改成该 `DispatchEntry` 的 `resolvedAction`，把 flags 收窄为 `resolvedFlags & VERIFIED_KEY_EVENT_FLAGS`。这使 fallback 或目标侧取消语义按实际发布的受保护内容签名，而不是照搬调用者对象；`FALLBACK` 等未进入 Key 白名单的 flag 本身不受 HMAC 认证。

Motion 只有最终 masked action 为 `DOWN` 或 `UP` 时才签名；`MOVE`、`CANCEL`、`OUTSIDE`、Hover 与 Scroll 返回 `INVALID_HMAC`。原始 `POINTER_DOWN/POINTER_UP` 通常不签，但 split 可把某个目标的局部 action 解析成 DOWN/UP，slippery enter 也可把 MOVE 解析成 DOWN，于是这些派生目标会得到有效签名。Java `VerifiedMotionEvent` 的 action 注解虽还枚举 POINTER 与 CANCEL，r48 的 dispatcher 签名入口仍以这里的 DOWN/UP 条件为准。目标侧加入的 obscured/partially-obscured flags 同样以 resolved flags 为准。

split/slippery 派生项可以得到新的 event ID，但 Verified 结构不含 event ID。签名认证的是选定字段与最终投递语义，不是 dispatcher 内部对象身份。

## 13. 随机密钥只活在dispatcher实例中，HMAC不负责加密或防重放

`HmacKeyManager` 构造时用 `RAND_bytes` 生成 128 字节随机 key；`HMAC(EVP_sha256(), ...)` 产生 32 字节摘要，失败时返回全零 `INVALID_HMAC`。签名失败不会阻止普通 publish，只会让接收事件在后续验证时得到 null。manager 是 `InputDispatcher` 的成员，源码没有持久化或向 App 公开 key，所以 dispatcher/process 重建后，旧事件通常无法用新 key 验证。

签名前把 `VerifiedKeyEvent` 或 `VerifiedMotionEvent` 的 packed struct 原始字节作为输入。`StructLayout_test.cpp` 用 `static_assert` 约束结构大小等于各字段大小之和，避免编译器 padding 悄悄改变签名布局。

HMAC提供完整性与持钥者认证，不是加密：事件字段仍以普通输入消息发送。它也不是防重放 token：Verified 结构没有 nonce、调用者身份、消费次数或 event ID；同一份仍带原 HMAC、字段未变且 dispatcher key 未轮换的事件可以再次通过验证。安全策略若关心新鲜度、调用上下文或“一次操作一次授权”，必须自己补充。

注入对象携带的 HMAC 不会被信任。Key 初始化显式写入 `INVALID_HMAC`，Motion 进入内部 `MotionEntry` 时也不复制输入 HMAC；只有面向最终目标发布时才用本实例的 key 重签。

## 14. VerifiedInputEvent只返回受保护子集，null保持保守未知

`verifyInputEvent()` 不检查 `INJECT_EVENTS`。JNI 将 Key/Motion 转成 native 事件，dispatcher 从传入对象重建 Verified 子集、用当前 key 重算 HMAC；计算失败、全零摘要或与事件携带值不相等时返回 null。

受保护字段如下：

| 类型 | 公共字段 | 类型专属字段 | 受保护flags |
|---|---|---|---|
| Key | type、deviceId、eventTime、source、displayId | action、downTime、keyCode、scanCode、metaState、repeatCount | 仅 `CANCELED` |
| Motion | type、deviceId、eventTime、source、displayId | primary pointer 的 rawX/rawY、masked action、downTime、metaState、buttonState | 仅 `WINDOW_IS_OBSCURED` 与 `WINDOW_IS_PARTIALLY_OBSCURED` |

Motion 未认证完整 pointer 数组、pointer IDs/tool types、第二及后续指针坐标、pressure/size/orientation 等轴、history、actionButton、cursor position、x/y scale 与 offset、precision、edgeFlags、classification、action index 与 event ID。修改这些未覆盖字段不会直接改变 Verified struct；是否仍能携带原 HMAC 到达验证入口，还要看对象复制链。`INJECTED/TRUSTED` 等 policy flags 也不属于 InputEvent 的 Verified 子集。

Java 的 `getFlag(int)` 返回 `Boolean` 是三态设计：受保护位存在返回 true，不存在返回 false；请求未受保护的 flag 返回 null，不能误当 false。`verifyInputEvent() == null` 也只表示本次无法认证，常见原因包括普通 MOVE 的全零 HMAC、事件字段被改动、签名失败或 dispatcher 已重启；它不是“确定伪造”的同义词。若传入的根本不是 Key/Motion，r48 JNI 类型门会抛运行时异常，而不是用 null 表示普通验证失败。

验证成功同样不证明物理硬件来源。进入签名范围的软件注入——Key，以及 resolved action 为 DOWN/UP 的 Motion——也会由 dispatcher 签名；注入 deviceId 被改成 `VIRTUAL_KEYBOARD_ID` 可作为线索，但不是密码学上的硬件证明。可靠表述只能是：**这个 InputEvent 的受保护子集，与当前 dispatcher key 曾签发的发布语义一致。**

## 15. 九组只读练习：逐层重建注入与验证账

下面命令只读，默认源码根目录为 `/Users/ninebot/androidSource`，也可把其他 AOSP 根目录作为第一个参数传入。每条 `grep` 都应独立命中；任一失败都表示源码版本或形态不同，应先核对基线。

### 练习 1：追Java、Binder、JNI与调用者身份

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public boolean injectInputEvent(InputEvent event, int mode) {' frameworks/base/core/java/android/hardware/input/InputManager.java
grep -n -F 'boolean injectInputEvent(in InputEvent ev, int mode);' frameworks/base/core/java/android/hardware/input/IInputManager.aidl
grep -n -F 'final int pid = Binder.getCallingPid();' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'final int uid = Binder.getCallingUid();' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'final long ident = Binder.clearCallingIdentity();' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'INJECTION_TIMEOUT_MILLIS, WindowManagerPolicy.FLAG_DISABLE_KEY_REPEAT);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'Binder.restoreCallingIdentity(ident);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'static jint nativeInjectInputEvent(JNIEnv* env, jclass /* clazz */,' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'env->IsInstanceOf(inputEventObj, gKeyEventClassInfo.clazz)' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'env->IsInstanceOf(inputEventObj, gMotionEventClassInfo.clazz)' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'jniThrowRuntimeException(env, "Invalid input event type.");' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
```

要求：说明为什么 clear identity 不会把注入者变成 system_server，并指出 ASYNC 仍必须同步走完哪些跨进程与转换步骤。

### 练习 2：验证Key与Motion的入口防线

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'static bool validateKeyEvent(int32_t action) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'case AKEY_EVENT_ACTION_DOWN:' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'case AKEY_EVENT_ACTION_UP:' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'static bool validateMotionEvent(int32_t action, int32_t actionButton, size_t pointerCount,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return index >= 0 && index < pointerCount;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return actionButton != 0;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerCount < 1 || pointerCount > MAX_POINTERS) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (id < 0 || id > MAX_POINTER_ID) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerIdBits.hasBit(id)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'new KeyEntry(incomingKey.getId(), incomingKey.getEventTime(),' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'new MotionEntry(motionEvent->getId(), *sampleEventTimes, VIRTUAL_KEYBOARD_ID,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：列出 `ACTION_MULTIPLE`、越界 action index、0 pointer、重复 pointer ID 的结果，并区分“单对象合法”与“完整手势序列合法”。

### 练习 3：分开policy flags与目标授权

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'policyFlags |= POLICY_FLAG_INJECTED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (hasInjectionPermission(injectorPid, injectorUid)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'policyFlags |= POLICY_FLAG_TRUSTED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (flags & AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!(policyFlags & POLICY_FLAG_FILTERED)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->interceptKeyBeforeQueueing(&keyEvent, /*byref*/ policyFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->interceptMotionBeforeQueueing(displayId, eventTime, /*byref*/ policyFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '(entry->policyFlags & POLICY_FLAG_TRUSTED) &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '(!(entry->policyFlags & POLICY_FLAG_DISABLE_KEY_REPEAT))) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'policyFlags | WindowManagerPolicy.FLAG_FILTERED);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
```

要求：为“无全局权限、同 UID 目标”和“有全局权限、跨 UID 目标”分别写出 INJECTED/TRUSTED、pre-queue policy 与重复键行为。

### 练习 4：找出Motion历史与InjectionState的绑定边界

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'std::queue<EventEntry*> injectedEntries;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const nsecs_t* sampleEventTimes = motionEvent->getSampleEventTimes();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'for (size_t i = motionEvent->getHistorySize(); i > 0; i--) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'motionEvent->getDisplayId(), policyFlags, action,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'actionButton, motionEvent->getFlags(),' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectedEntries.push(nextInjectedEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InjectionState* injectionState = new InjectionState(injectorPid, injectorUid);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectedEntries.back()->injectionState = injectionState;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'while (!injectedEntries.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionResult(INPUT_EVENT_INJECTION_PENDING),' frameworks/native/services/inputflinger/dispatcher/InjectionState.cpp
grep -n -F 'pendingForegroundDispatches(0) {}' frameworks/native/services/inputflinger/dispatcher/InjectionState.cpp
grep -n -F 'if (injectionState &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：画出含两个历史 sample 与一个当前 sample 的三条 Entry，标明哪条携带注入者身份、结果和 foreground 计数；再说明静态证据能确认到哪里。

### 练习 5：核对同UID、跨UID与monitor-only权限

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'return injectorUid == 0 ||' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->checkInjectEventsPermissionNonReentrant(injectorPid, injectorUid);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'is safe to call while holding other locks.' frameworks/native/services/inputflinger/dispatcher/include/InputDispatcherPolicyInterface.h
grep -n -F 'gServiceClassInfo.checkInjectEventsPermission, injectorPid, injectorUid);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'mContext.checkPermission(android.Manifest.permission.INJECT_EVENTS,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'windowHandle->getInfo()->ownerUid != injectionState->injectorUid) &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!checkInjectionPermission(focusedWindowHandle, entry.injectionState)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!checkInjectionPermission(touchedWindow.windowHandle, entry.injectionState)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool hasGestureMonitor = !tempTouchState.gestureMonitors.empty();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionPermission = INJECTION_PERMISSION_GRANTED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (checkInjectionPermission(nullptr, entry.injectionState)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!checkCallingPermission(android.Manifest.permission.MONITOR_INPUT,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F '<permission android:name="android.permission.MONITOR_INPUT"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F 'android:protectionLevel="signature" />' frameworks/base/core/res/AndroidManifest.xml
grep -n -F '} else if (switchedDevice && maskedAction == AMOTION_EVENT_ACTION_MOVE) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionResult = INPUT_EVENT_INJECTION_PERMISSION_DENIED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：推演 same-UID focused、cross-UID focused、pointer 多 foreground、仅 gesture monitor 与没有任何接收者五种结果；分开 monitor 注册能力与逐事件授权，也不要把 NonReentrant 解释成锁外调用。

### 练习 6：定位三种mode与policy空成功

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'INPUT_EVENT_INJECTION_SYNC_NONE = 0,' frameworks/native/services/inputflinger/dispatcher/InjectionState.h
grep -n -F 'INPUT_EVENT_INJECTION_SYNC_WAIT_FOR_RESULT = 1,' frameworks/native/services/inputflinger/dispatcher/InjectionState.h
grep -n -F 'INPUT_EVENT_INJECTION_SYNC_WAIT_FOR_FINISHED = 2,' frameworks/native/services/inputflinger/dispatcher/InjectionState.h
grep -n -F 'nsecs_t endTime = now() +' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (syncMode == INPUT_EVENT_INJECTION_SYNC_NONE) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionResult = INPUT_EVENT_INJECTION_SUCCEEDED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (injectionResult != INPUT_EVENT_INJECTION_PENDING) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'nsecs_t remainingTimeout = endTime - now();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionResult = INPUT_EVENT_INJECTION_TIMED_OUT;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'syncMode == INPUT_EVENT_INJECTION_SYNC_WAIT_FOR_FINISHED) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '*dropReason == DropReason::POLICY ? INPUT_EVENT_INJECTION_SUCCEEDED' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionState->release();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'case INPUT_EVENT_INJECTION_PERMISSION_DENIED:' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'case INPUT_EVENT_INJECTION_TIMED_OUT:' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'TEST_F(InputDispatcherSingleWindowAnr, Key_StaysPendingWhileMotionIsProcessed) {' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
```

要求：分别写出结构非法、结构合法但跨 UID 目标授权被拒、policy 消费、正常选中目标时，三种 mode 的调用者可观察结果；再用测试证明 timed out 返回不会撤销 pending Key。

### 练习 7：追foreground计数、finished与清队列

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!connection->inputState.trackKey(keyEntry, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!connection->inputState.trackMotion(motionEntry, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (dispatchEntry->hasForegroundTarget()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'incrementPendingForegroundDispatches(newEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_back(dispatchEntry.release());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'while (injectionState->pendingForegroundDispatches != 0) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->inputPublisher.receiveFinishedSignal(&seq, &handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_front(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'drainDispatchQueue(connection->outboundQueue);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'decrementPendingForegroundDispatches(dispatchEntry->eventEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (mNoFocusedWindowTimeoutTime.has_value() && mAwaitedFocusedApplication != nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (focusedWindowHandle->getInfo()->paused) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'cancelEventsForAnrLocked(connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '// We will not be breaking any connections here, even if the policy wants us to abort dispatch.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：说明 monitor、policy drop、流不一致项、正常 Motion finished、Key fallback 与 broken Connection 各自怎样影响计数；再区分无 focused window、focused paused 与已发布 Connection 的三种等待/ANR状态。

### 练习 8：从最终DispatchEntry走到HMAC

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'static std::array<uint8_t, 128> getRandomKey() {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (RAND_bytes(key.data(), key.size()) != 1) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'HMAC(EVP_sha256(), mHmacKey.data(), mHmacKey.size(), data, size, hash.data(), &hashLen);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::array<uint8_t, 32> hmac = getSignature(*keyEntry, *dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::array<uint8_t, 32> hmac = getSignature(*motionEntry, *dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.action = dispatchEntry.resolvedAction;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.flags = dispatchEntry.resolvedFlags & VERIFIED_KEY_EVENT_FLAGS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if ((actionMasked == AMOTION_EVENT_ACTION_UP) || (actionMasked == AMOTION_EVENT_ACTION_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.actionMasked = actionMasked;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.flags = dispatchEntry.resolvedFlags & VERIFIED_MOTION_EVENT_FLAGS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '? AMOTION_EVENT_ACTION_DOWN' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F ': AMOTION_EVENT_ACTION_UP;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_CANCEL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_DOWN;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return INVALID_HMAC;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：代入普通 DOWN、MOVE、split 后的局部 DOWN/UP、slippery exit（resolved CANCEL）/enter（resolved DOWN）和 Key fallback，判断哪个目标签什么 action/受保护 flags，哪个得到全零 HMAC。

### 练习 9：列出Verified子集并证明null语义

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'struct __attribute__((__packed__)) VerifiedInputEvent {' frameworks/native/include/input/Input.h
grep -n -F 'struct __attribute__((__packed__)) VerifiedKeyEvent : public VerifiedInputEvent {' frameworks/native/include/input/Input.h
grep -n -F 'struct __attribute__((__packed__)) VerifiedMotionEvent : public VerifiedInputEvent {' frameworks/native/include/input/Input.h
grep -n -F 'constexpr int32_t VERIFIED_KEY_EVENT_FLAGS = AKEY_EVENT_FLAG_CANCELED;' frameworks/native/include/input/Input.h
grep -n -F 'AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED | AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;' frameworks/native/include/input/Input.h
grep -n -F 'event.getRawX(0),' frameworks/native/libs/input/Input.cpp
grep -n -F 'event.getFlags() & VERIFIED_MOTION_EVENT_FLAGS,' frameworks/native/libs/input/Input.cpp
grep -n -F 'if (calculatedHmac == INVALID_HMAC) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (calculatedHmac != event.getHmac()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'case FLAG_CANCELED:' frameworks/base/core/java/android/view/VerifiedKeyEvent.java
grep -n -F 'case FLAG_WINDOW_IS_PARTIALLY_OBSCURED:' frameworks/base/core/java/android/view/VerifiedMotionEvent.java
grep -n -F 'static_assert(sizeof(VerifiedMotionEvent) == VERIFIED_MOTION_EVENT_SIZE);' frameworks/native/libs/input/tests/StructLayout_test.cpp
```

要求：从 struct 手工列出 Key/Motion 的保护集合，为“受保护位为 false”“请求未保护位”“普通 MOVE 无有效签名”分别写出 false、null 与 verify null 的含义。

## 16. 用三个场景收束完成点，并形成排查顺序

场景一：非 uid 0 且无 `INJECT_EVENTS` 的进程向同 UID focused window 注入 Key，并选择 WAIT_FOR_FINISH。设 pre-queue/dispatch policy 放行，目标未 paused、Channel 可用，App 也在共同 deadline 内完成回执。

```text
保存 caller pid/uid
  → Key结构合法，deviceId改为VIRTUAL_KEYBOARD_ID
  → 早期权限失败：INJECTED但不TRUSTED
  → focused window ownerUid相同：目标授权通过
  → injectionResult = SUCCEEDED
  → foreground DispatchEntry计数并发布，dispatcher生成HMAC
  → App finished；若handled=false且policy给fallback，还要继续同一计数
  → 最终release，计数归零，Java返回true
```

这条链同时说明：untrusted 不等于未授权，软件注入也不等于没有有效 HMAC，FINISH 还可能跨过 Key fallback。

场景二：非 uid 0 且无全局权限的进程用 ASYNC 向跨 UID focused window 注入结构合法的 Key。设 policy 放行，且目标选择时焦点仍停在这个跨 UID、可分发的窗口。

```text
同步Binder/JNI、权限预查、policy前置回调与入队完成
  → ASYNC先返回true
  → dispatcher稍后选中跨UID窗口
  → 第二次权限检查失败，写PERMISSION_DENIED并记录异步日志
  → 不向该窗口建立DispatchEntry
```

这里的 true 只证明受理，不证明目标收到。若调用者必须知道目标裁决，应使用 WAIT_FOR_RESULT；若还要等 foreground 项释放，再选择 WAIT_FOR_FINISH，但仍不能把它解释成业务成功。

场景三：同步 Key 被 policy 消费。

```text
事件已进入dispatcher
  → dispatch阶段得到DropReason::POLICY
  → injectionResult = SUCCEEDED
  → 没有目标、没有foreground计数
  → RESULT与FINISH都可返回true
```

这解释了为什么即使最强的同步 mode 也可能“成功但 App 没收到”。排查时按以下顺序收证据：

1. 确认 Java mode、事件真实类型、action、pointerCount/IDs 与 JNI 是否成功转换。
2. 记录原始 Binder pid/uid，并区分早期 TRUSTED 快照与目标阶段授权。
3. 检查 pre-queue/dispatch policy 是否清了 PASS_TO_USER 或产生 POLICY drop。
4. 还原 focused/TouchState 目标，逐个比较 ownerUid；单列 gesture monitor-only。
5. 查看 `InjectionState.injectionResult`，不要只看 Java 布尔值；带 Motion history 时确认只有最后 sample 绑定状态。
6. 对实际 `DispatchEntry` 区分 foreground 与附加目标，观察 outbound、waitQueue、finished、fallback 和清理。
7. 将注入共同 deadline 与每个窗口的 ANR deadline 分开；调用超时后继续观察迟到投递。
8. 若涉及可信字段，在每个目标的 resolved action/flags 之后检查 HMAC，再按 Verified 子集解释结果。

本章最重要的判断句可以压缩为：**ASYNC 成功是已受理，RESULT 成功是 dispatcher 已裁决，FINISH 成功是已计数前台项已释放，verify 成功是受保护字段匹配当前 dispatcher 签名。它们都不是“用户界面已完成目标业务”的证明。**

下一章将追另一种手势中途改路机制：`transferTouchFocus(fromToken, toToken)` 怎样同时迁移 `TouchState` 与 Connection 的 `InputState`，旧窗口的 CANCEL、新窗口补出的 DOWN，以及系统接管现有触摸流各在哪个完成点发生。
