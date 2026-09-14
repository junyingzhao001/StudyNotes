# 235 Android InputDispatcher焦点窗口、等待队列与输入ANR判定链

上一章沿着 `InputChannel` 追到了应用侧的 `finishInputEvent`：事件被应用取走，并不等于系统已经把这笔账结清。本章站回 system_server 与 inputflinger 一侧，回答更容易误诊的问题：`Input dispatching timed out` 到底是在等窗口出现，还是在等一个已经发布的事件回执？

先给结论：**“输入分发超时”不是“应用主线程执行超过默认 5 秒”的同义词。** Android 11 的 InputDispatcher 至少存在两条入口不同的 ANR 路径：事件尚未找到 focused window 时，等的是窗口；事件已经成功写入某条 InputChannel 后，等的是该 Connection 的 finished signal（完成信号，后文简称回执）。只有先确定事件位于哪一侧，线程栈、队列和时间才有解释力。

本文以 `android-11.0.0_r48` 为准。核心源码位于：

- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp`
- `frameworks/native/services/inputflinger/dispatcher/Entry.h` 与 `Entry.cpp`
- `frameworks/native/services/inputflinger/dispatcher/AnrTracker.h` 与 `AnrTracker.cpp`
- `frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java`
- `frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java`
- `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`

本章聚焦按焦点寻址的 Key 与非 pointer Motion。pointer 命中、分流、手势监控和 pilfer 将留到下一章。

## 1. 先按 publish 分界，不要先猜主线程

一条输入事件可能停在以下三个位置：

```text
mInboundQueue / mPendingEvent
        │  尚未选出目标
        ▼
Connection.outboundQueue
        │  publish 成功
        ▼
Connection.waitQueue ── finished(seq, handled) ──► 移除并结账
```

这里最重要的边界是 `publish`：

- 没有 focused window 时，事件仍是全局的 `mPendingEvent`。它没有对应的 `DispatchEntry`、Connection、Channel 或 `waitQueue` 记录。
- 找到目标以后，InputDispatcher 为每个目标准备 `DispatchEntry`，先放进该 Connection 的 `outboundQueue`。
- 只有 `publishKeyEvent`、`publishMotionEvent` 等调用成功，条目才从 `outboundQueue` 移到 `waitQueue`，并在 Connection 仍为 responsive 时登记进 `mAnrTracker`。

因此，日志里同样出现 `Input event dispatching timed out`，含义可能完全不同：

| 路径 | 超时前是否 publish | 等待对象 | native reason |
|---|---:|---|---|
| 无 focused window | 否 | 某个 focused application 出现可接收焦点的窗口 | `<app> does not have a focused window` |
| Connection 超时 | 是 | 某个 seq 的完成回执 | `<channel> is not responding. Waited ... for ...` |

这个分界也解释了为什么“看应用主线程栈”不是总能直接找到原因：第一条路径甚至还没有把事件交给应用。

## 2. 正常路径先建立基准：发出、处理、回执

正常链可以压缩成六步：

```text
InputReader / inject
  → InputDispatcher 选目标
  → outboundQueue
  → publish 成功，进入 waitQueue 与 AnrTracker
  → App InputEventReceiver 分发并 finishInputEvent
  → native 收到 finished(seq, handled)，移除 waitQueue 账目
```

上一章已经说明应用侧 `ViewRootImpl`、`InputEventReceiver` 和 native consumer 如何发送回执。本章只守住两个协议事实。

第一，InputDispatcher 收到的 `seq` 用来查找 `Connection.waitQueue` 中的 `DispatchEntry.seq`。不要把它直接等同于 Java `InputEvent.getSequenceNumber()`；跨越传输边界时存在映射，InputDispatcher 最终关心的是能否定位自己的 dispatch 账目。

第二，`handled` 与“有没有回执”正交：

- `handled=true`：应用声明业务上已处理。
- `handled=false`：应用声明业务上未处理，Key 还可能进入 policy fallback。
- 两者都是有效的 finished signal，都能结束当前那次 waitQueue 等待。

分析异常前，先用这条正常路径作对照。若连事件有没有 publish 都没有判断，后面的“5 秒”“窗口卡死”“IME 卡住”都只是猜测。

## 3. `dispatchOnce()`怎样把队列、命令和ANR检查串起来

InputDispatcher 由自己的 `InputThread` 驱动。每轮 `dispatchOnce()` 大致执行：

1. 持有 `mLock`；若没有待执行 command，则进入 `dispatchOnceInnerLocked()` 推进事件。
2. 执行 command 队列；某些 command 会暂时释放 `mLock` 调 policy。
3. 调用 `processAnrsLocked()`，得到下一次 ANR 检查时刻。
4. 释放锁后进入 `Looper::pollOnce()`，等待 fd、显式 wake 或定时唤醒。

`nextWakeupTime` 是这些原因的最早值，不是一个单独的“ANR 线程”。无窗口等待和 `mAnrTracker` 都通过它安排下一次检查。

有一个常被忽略的并发边界：`doNotifyAnrLockedInterruptible()` 在调用 Java policy 前会释放 native 的 `mLock`，所以其他线程可以更新窗口、注销 Channel；但这次 Java 回调仍由 InputDispatcher 线程同步执行。也就是说，native 数据锁没有跨 WMS/AMS 持有，不代表 InputDispatcher 线程可以同时继续分发。

`mLastAnrState` 则是在 native 判定超时时抓取的一份状态快照。之后看到的 traces、WMS dump 和进程状态可能已经变化，不能把不同时刻的证据拼成一个原子现场。

## 4. WMS提供两类焦点事实，InputDispatcher各自保存

需要严格区分：

- `focused application` 是预期获得输入焦点的 `InputApplicationHandle`，它说明“当前应当由哪个应用承接焦点”。
- `focused window` 是具体的 `InputWindowHandle`，它说明“现在可以把事件发往哪条窗口 Channel”。

这两类事实进入 native 的路径也不同。普通窗口快照沿着 `InputMonitor → SurfaceControl.Transaction.setInputWindowInfo() → SurfaceFlinger::updateInputWindowInfo() → IInputFlinger/InputManager::setInputWindows() → InputDispatcher::setInputWindows()` 下沉。

focused application 则由 Java InputManagerService 经 JNI 直接交给 `InputDispatcher::setFocusedApplication()`。

对于同一 display，InputDispatcher 在按 top-to-bottom 顺序排列的窗口列表中选择首个同时满足 `hasFocus && visible` 的 handle；这里的“首个”是 z 序最靠上的匹配窗口，不是 Java 窗口层级里的所谓 top-level window。

focused application 已经确定、focused window 仍为空，是应用启动、切换或窗口事务过渡时可能出现的中间态。Android 11 保留一条兼容行为：只有当真正出现一条待按焦点分发的事件时，才开始“等待窗口”的计时。`setFocusedApplication()` 本身不启动这个计时器。

焦点变更还会形成 `FocusEntry/FocusEvent` 通知。旧焦点 Window 只有仍能找到已注册的 InputChannel 时，才合成 `CANCEL_NON_POINTER_EVENTS` 并入队 `focus=false`；新焦点则入队 `focus=true`。focused-window map 的旧值清理不依赖旧 Channel 是否仍存在。

`FocusEvent` 与“按焦点寻址的 Key 或非 pointer Motion”不是同一个概念。本文把后者写全，避免把两者都简称为“焦点事件”。

## 5. 无窗口路径有三种分支，计时只属于其中一种

`findFocusedWindowTargetsLocked()` 取出目标 display 的两类焦点后，先分三种情况。

| focused window | focused application | 结果 |
|---|---|---|
| 空 | 空 | 直接丢弃，返回 injection failed；不启动 ANR 计时 |
| 空 | 非空 | 首次发现时启动无窗口计时，事件保持 pending |
| 非空 | 任意 | 重置无窗口计时，继续权限、paused、Key 排序等检查 |

第二种情况下，超时长度来自 focused application 的 dispatching timeout；未配置时回落到默认 5 秒。InputDispatcher 保存一对全局状态：

```text
mNoFocusedWindowTimeoutTime = currentTime + timeout
mAwaitedFocusedApplication  = focusedApplicationHandle
```

它不是“每个 display 一只计时器”，也不是每条事件各有一只。后续 retry 仍无窗口时复用这对状态。到期后，`processAnrsLocked()` 先走 `onAnrLocked(mAwaitedFocusedApplication)`，随后清空 awaited application；原 deadline 在这一步并未一并清成 `nullopt`。同一 pending 事件再次尝试选目标时，若仍无 focused window，就会因为超过旧 deadline 而被丢弃；若 policy 调用期间窗口已经出现，则转入有效窗口分支、重置计时并继续选目标。

事件推进中最常见的两类重置是：

- pending 事件重试时看到了有效 focused window，调用 `resetNoFocusedWindowTimeoutLocked()`。
- `setFocusedApplication()` 发现旧 application 正是正在等待的对象，而新 application 已换人。

此外，dispatcher 从 frozen 恢复，以及 reset-and-drop 这类全局状态清理也会重置它；这些是控制面清场，不要误写成某个 pointer DOWN 直接清除了计时。

若 policy 返回正的 extension，且此时找不到 Connection、但无窗口 deadline 与 command 携带的 application 仍有效，`extendAnrTimeoutsLocked()` 会用 `now() + extension` 重新设定 deadline 并恢复 awaited application。这里不再次校验当前 focused application 的身份；后续状态更新和目标选择仍要自行收敛。若 policy 返回 0，无窗口路径的 token 是空的，找不到 Connection，因此不会生成 Connection cancel；之后重试时若仍无 focused window，才按旧 deadline 的过期分支丢弃，窗口已经出现则继续正常选目标。

另外两个边界也要分开：

- 对本章的按焦点寻址路径，focused window 的 `paused` 为真时，目标已经存在，但事件仍保持 pending。这里不会创建 waitQueue 记录，也不启动一只“paused 专用 ANR 计时器”；后续是否被 stale 规则丢弃要按事件类型与重试时刻另算。pointer 新手势面对 paused window 走另一套目标选择，不能套用这条结论。
- 新 pointer `DOWN` 可能触发 inbound pruning 或结束 Key 的 500ms 等待，但它不会直接调用 `resetNoFocusedWindowTimeoutLocked()`。是否解除无窗口等待，要看随后的焦点/application 状态变化。

## 6. Key发送前还有两套不同的500ms机制

源码里至少有两套 500ms，目的和起点不同。

`APP_SWITCH_TIMEOUT` 服务于应用切换键。只有未取消、带 `TRUSTED` 与 `PASS_TO_USER` policy flags 的 HOME、ENDCALL 或 APP_SWITCH 才被这段逻辑识别；先看到 DOWN，再在对应 Key Up 入队时用该事件的 `eventTime + 500ms` 设置 `mAppSwitchDueTime`。如果旧事件拖得太久，它用于丢弃先于切换键推进的其他 pending Key/Motion，不是 Connection ANR deadline。

`KEY_WAITING_FOR_EVENTS_TIMEOUT` 服务于 Key 的排序。Key 找到 focused window 后，若 `mAnrTracker` 仍非空，InputDispatcher 最多再等 500ms，让先前未完成的输入有机会引发焦点变化；超时后仍把 Key 发给当前 focused window。

注释用“未处理 Motion 可能改变焦点”解释设计动机，但实现条件是 `mAnrTracker.empty()`，没有只筛 Motion、当前窗口或当前 display。因此准确说法是：设计主要防 Motion 导致焦点切换，实际门槛观察的是全局仍受追踪的 dispatch 条目。

新 pointer `DOWN` 会把已经存在的 `mKeyIsWaitingForEventsTimeout` 改为 `now()`，让 pending Key 尽快结束这次等待。这仍不是 ANR；它只是发 Key 前的顺序协调。

## 7. EventEntry、DispatchEntry与三层队列不是同一笔账

理解队列前先分对象：

- `EventEntry` 表示一条逻辑输入事件，携带 `eventTime`、类型和原始事件信息。
- `DispatchEntry` 表示这条事件面向某个目标的一次投递，带目标 flags、变换信息、`seq`、`deliveryTime` 与 `timeoutTime`。
- 同一个 EventEntry 可以因为多个目标而对应多个 DispatchEntry；ANR 追踪针对具体 Connection 的投递账。

三个常见位置承担不同职责：

| 位置 | 所属范围 | 语义 |
|---|---|---|
| `mInboundQueue` / `mPendingEvent` | InputDispatcher 全局 | 尚在选择与推进的逻辑事件 |
| `Connection.outboundQueue` | 每条 Connection | 已准备给该目标、尚未成功写入 Channel |
| `Connection.waitQueue` | 每条 Connection | 已成功 publish、正在等待回执 |

`DispatchEntry::nextSeq()` 生成的是 InputDispatcher 这端的投递序号。它随 publish 送入 transport，回执携带能映射回这笔账的 seq。排查“回执找不到事件”时，应沿 transport 映射逐层看，而不是拿任意一层显示的 sequence number 直接比较。

`InputState` 还会在准备投递时记录按键、触点等状态，以便需要时合成取消。这个状态更新发生在应用真正看到事件之前；因此“能合成 CANCEL”不等于“原事件已被应用消费”。

## 8. deadline在发布尝试前写入，成功发布后才受追踪

`startDispatchCycleLocked(currentTime, connection)` 从 `outboundQueue.front()` 开始，每次发布尝试前都会写：

```cpp
dispatchEntry->deliveryTime = currentTime;
dispatchEntry->timeoutTime = currentTime + timeout;
```

timeout 通过目标 token 查 Window 的配置；找不到 Window 时才使用默认值。因此“输入 ANR 固定 5 秒”也不严谨：默认是 5 秒，具体 Window/Application 可以提供不同值，instrumentation 或包装环境也可能改变它。

随后才调用 publisher。三种结果不能混为一谈：

1. **publish 成功**：从 outbound 移除、压入 waitQueue；若 Connection 当前 responsive，再把 `(timeoutTime, token)` 加入 `mAnrTracker`。
2. **`WOULD_BLOCK` 且 waitQueue 非空**：说明 pipe 满且应用还有未完成事件，当前条目留在 outbound，等待后续回执腾出空间。
3. **`WOULD_BLOCK` 但 waitQueue 为空**：按协议 Channel 本应可写，这被视为异常，进入 broken dispatch cycle；其他意外错误也走 broken 路径。

由此得到一个略细但很实用的时间边界：deadline 字段在发布尝试前被写入，但只有成功 publish 后才进入 waitQueue/AnrTracker，成为可触发 Connection ANR 的账。若先前因 pipe 满而保留在 outbound，下一次 `startDispatchCycleLocked()` 会用新的 `currentTime` 覆盖它的 delivery/deadline。一次 start cycle 连续发布多条时则共享该次传入的 `currentTime`。

所以有效追踪时间从成功发布的那次尝试开始，而不是从硬件产生事件的 `eventTime` 开始。

## 9. AnrTracker只保存索引，真正的账在waitQueue

`AnrTracker` 可以理解为按 deadline 排序的 `(timeoutTime, connectionToken)` 多重集合。它不保存 `DispatchEntry.seq`，也不是完整队列；真实条目仍在各 Connection 的 `waitQueue`。

`processAnrsLocked()` 每轮先检查无 focused window deadline，再把下一检查点与 `mAnrTracker.firstTimeout()` 取最小值。Connection deadline 到期时，它：

1. 用 tracker 的首个 token 找 Connection。
2. 把 `connection->responsive` 设为 `false`。
3. `eraseToken(token)`，移除该 token 在 tracker 中的全部索引，停止反复按旧 deadline 唤醒。
4. 调 `onAnrLocked(connection)`。

`onAnrLocked(connection)` 若看到空 waitQueue，会把它当作已经恢复而不再上报。这是针对延长历史和队列状态的防御性检查；`processAnrsLocked()` 标记 Connection 与紧接着调用 `onAnrLocked()` 之间没有一次 policy 解锁，不能把这个分支解释成“finished 恰好在两行之间插入”。

生成 reason 时选择 `waitQueue.begin()` 的 oldest entry。它未必就是 tracker 中最早 deadline 对应的条目：窗口 timeout 可能中途变化，让较新的条目更早到期。源码仍选择 oldest，是因为应用通常线性处理输入，它更能描述阻塞链的前端。

这也意味着诊断时必须同时看：tracker 说明哪个 token 何时触发检查，waitQueue oldest 说明 reason 展示哪条等待，二者不是按 seq 一一配对的同一个容器。

## 10. native先留快照，再解锁同步进入Java policy

两条 ANR 路径最终都创建 `doNotifyAnrLockedInterruptible` command，但携带的信息不同：

| 来源 | `inputApplicationHandle` | `inputChannel` / token |
|---|---|---|
| 无 focused window | 等待中的 application | 空 |
| Connection timeout | 空 | 超时 Connection 的 Channel |

在 command 入队前，native 先通过 `updateLastAnrStateLocked()` 保存时间、reason、窗口标签和 dispatcher dump。随后 command 执行：

```text
取 token
  → mLock.unlock()
  → mPolicy->notifyAnr(application, token, reason)
  → mLock.lock()
  → extension > 0 ? 进入延长分支 : 进入 abort 分支
```

释放锁是跨 native/Java 与跨锁域调用的必要并发边界。WMS 和 AMS 处理期间，窗口、Connection 乃至原条目都可能变化，所以返回后必须重新按 token 查询，不能继续相信旧裸指针。

调用链是 `InputDispatcher → NativeInputManager::notifyAnr() → InputManagerService.notifyANR() → InputManagerCallback.notifyANRInner()`。它仍是 InputDispatcher 线程上的同步调用。若 Java policy 自身长时间阻塞，native 锁虽然可被其他线程取得，这条 dispatcher loop 仍要等回调返回；`InputManagerCallback.notifyANR()` 也专门记录该调用耗时。JNI callback 若抛出异常，native 会清除异常并把返回值收敛为 0，也就是 abort。

## 11. Java按token把Channel责任映射到Window、Activity或进程

`InputManagerCallback.notifyANRInner()` 在 WMS 锁内完成责任定位与状态保存，顺序如下：

1. token 非空时，先查 `mInputToWindowMap` 得到 `WindowState`，再取得它的 `ActivityRecord`、窗口进程 PID 与 `aboveSystem`。
2. 普通 Window 未命中时，再查 embedded window，至少找 owner PID，并尽量由 host window 判断层级。
3. 若仍没有 Activity、但 native 传来了 `InputApplicationHandle`，用其中的 token 查 `ActivityRecord`。这正是无 focused window 路径。
4. 在 WMS 锁内保存 WMS ANR state；释放 WMS 锁后，再保存 ATMS state 并调用 AM，避免持 WMS 锁进入 AM。

之后有两条 Java 入口：

- 有 Activity 时调用 `ActivityRecord.keyDispatchingTimedOut(reason, windowPid)`。如果窗口 PID 与 Activity 进程不一致，会改走通用 PID 归责，避免把外挂在 Activity token 上的其他进程窗口错怪给 Activity。
- 没有 Activity、但有 Window 或有效 PID 时，调用 `mAmInternal.inputDispatchingTimedOut(windowPid, aboveSystem, reason)`。

AMS 的决策也不是“上报就立刻杀进程”：

- 正在调试的进程返回 keep waiting。
- 有 active instrumentation 的进程会结束 instrumentation，并选择 abort。
- 普通进程把 `appNotResponding` 交给 `mAnrHelper`，随后返回 abort；ANR 处理本身由 helper 推进。
- 找不到可归责主体时直接返回 abort。

Java 最终给 native 的不是布尔值，而是纳秒：正值表示继续等待多久，0 表示中止分发。通用 PID 路径中，AMS 的毫秒返回值会在 `InputManagerCallback` 中乘 `1000000L`。

## 12. extension、abort与broken channel是三种不同动作

policy 返回正数时，`extendAnrTimeoutsLocked()` 会先按 token 重新查 Connection。若它已消失，且这也不是带有效 deadline/application 的无窗口路径，函数直接返回；不会恢复 responsive 或重建 tracker。Connection 仍存在时，代码才把它恢复为 responsive，并计算统一的 `newTimeout = now() + extension`。它只更新满足 `newTimeout >= entry->timeoutTime` 的 waitQueue 条目，并把这些条目重新插入 tracker。

这个条件有一个容易漏掉的 r48 边界：Connection 被判超时时，`eraseToken()` 已移除该 token 的全部 tracker 索引；如果某个 waitQueue 条目的原 deadline 比 `newTimeout` 还晚，它既不会被覆盖，也不会在这个循环里重新插入 tracker。也就是说，waitQueue 与 tracker 并非任何时刻都严格一一对应。

policy 返回 0 时也会重新按 token 查 Connection；若它已经消失，直接返回。Connection 仍存在时才调用 `cancelEventsForAnrLocked()`：

- 不直接断开 Channel。
- 不直接清空既有 waitQueue 或 outboundQueue。
- 不等于杀掉应用进程。
- 在 Connection 仍为 normal 时，构造 `CancelationOptions(CANCEL_ALL_EVENTS, ...)`，再由 `InputState` 生成具体带取消语义的 Key/Motion 事件并加入同一分发体系。

因为 cancel 也要走 Channel，“已生成取消语义”不证明卡住的 client 已经收到它。真正的 Channel 错误走 `abortBrokenDispatchCycleLocked()`，它会清空两条 dispatch 队列、把状态改为 broken，并按 `notify` 参数决定是否通知 policy；publish 异常路径传入的是 `true`。这是与 ANR abort 不同的故障路径。

Connection 变成 unresponsive 后，新 pointer gesture 不再选它作为目标；但源码明确允许按焦点寻址的事件（源码注释称 `focused events`）继续堆积，已经属于它的 touch stream 也有自己的连续性语义。不要把 `responsive=false` 解读成“此进程的所有输入从此全部丢弃”。

## 13. 迟到回执能结账，但不保证重建全部追踪索引

native consumer 读到 `(seq, handled)` 后，经 command 进入 `doDispatchCycleFinishedLockedInterruptible()`：

1. 在 waitQueue 中按 seq 查条目。
2. 计算 `finishTime - deliveryTime`，上报统计；超过 2 秒才打印 slow processing warning。
3. 对 Key/Motion 执行完成后的 policy 动作。
4. 因 policy 可能临时解锁，回来后再次按 seq 查找。
5. 若仍存在，则从 waitQueue 移除，并从 tracker 删除对应 `(timeoutTime, token)`。
6. 若 Connection 先前 unresponsive，扫描剩余 waitQueue；只有不存在 `timeoutTime < now()` 的条目，才恢复 responsive。
7. 再次启动该 Connection 的 dispatch cycle。

所以迟到回执仍有价值：它可以关闭对应账目，并在剩余条目都未过期时恢复 Connection。但恢复代码不会把其他仍在 waitQueue、却已因 `eraseToken()` 消失的旧索引重新加入 tracker；它与 policy extension 的“条件式重建”也不相同。

`handled=false` 同样先表示这次投递已经完成。对于未处理的前台 Key，policy 可能生成 fallback key，并让 `restartEvent` 把同一个 DispatchEntry 推回 outbound 前端。它的 seq 不变，所指 KeyEntry 内容被改成 fallback；旧 wait 实例先结清，随后重新发布会形成新的 delivery/deadline。若恢复判定仍为 unresponsive，发布成功也不会重新插 tracker；fallback 再次未处理时只上报，不会无限递归生成下一层。不能把 `handled=false` 说成“不回 ACK”，也不能说“该逻辑按键永远只投递一次”。

最后，2 秒 slow warning 只在收到完成回执后计算。它说明“已完成但很慢”；ANR 则说明 deadline 到时仍在等。二者可能描述同一条事件的不同阶段，但不是同一个判定器。

## 14. 七套计时机制要按起点和动作分别记账

把常见常量排在一起，最容易看出它们不能相加：

| 时间尺度 | 起点 | 观察对象 | 到期动作 |
|---|---|---|---|
| app switch 500ms | app-switch Key Up 的 `eventTime` | 旧事件是否阻碍应用切换 | 到期后丢弃先于切换键处理的其他 pending Key/Motion |
| Key waiting 500ms | Key 选目标时的 `currentTime` | 全局 tracker 是否仍有条目 | 不再等前序，发给当前焦点窗口 |
| slow warning 2s | `deliveryTime` | 已完成投递耗时 | 回执后打印慢处理日志 |
| IME 2500ms | IMM main looper 成功向当前 IME Channel 发送事件并登记 PendingEvent 后 | IME Session Channel finished / `ImeInputEventSender` 回调 | 以 `handled=false` 恢复应用侧回调并继续 post-IME；原输入仍须最终 finish |
| no-focused-window timeout（默认 5s） | 首次有待发事件发现 application 有、window 无 | focused application 是否获得窗口 | application 路径进入 ANR policy 链 |
| Connection dispatch timeout（默认 5s） | 成功 publish 的那次尝试 | 该 Connection 的完成回执 | Connection 路径进入 ANR policy 链 |
| stale 10s | Key/Motion 的 `eventTime` | 当前分发循环中的事件年龄 | 在发布前丢弃陈旧 Key/Motion |

这里还要补三条限制：

- 5 秒只是默认值，不是所有 Window/Application 的保证值。
- stale drop 在这一版的当前分发循环中针对 Key/Motion；不要推广到 Focus、Configuration、DeviceReset 等所有 EventEntry。
- 两套 500ms 恰好数值相同，但状态变量、起点和作用都不同。

上一章的 IME 2500ms 正好说明“时间重叠而非串联”。Window 事件成功 publish 后，外层 Connection deadline 已经开始走；应用处理这条事件时若进入 IMM 的 2500ms 等待，这段时间消耗的就是同一段外层预算，而不是先等 2.5 秒、再额外获得完整 5 秒。Handler 回调还可能受应用 Looper 排队影响，而 InputDispatcher 的时钟独立推进，因此两边可能竞争到期。

外层 reason 仍按 Window Channel 归责。即使根因在应用内部等待 IME session，InputDispatcher 看到的也只是“这个窗口尚未返回 seq”。诊断必须把 InputDispatcher 的 delivery/deadline、应用主线程栈和 IMM 日志放在同一时间轴上。

## 15. 九组只读练习：从源码重建状态迁移

下面命令只读，默认源码根目录为 `/Users/ninebot/androidSource`，也可把其他 AOSP 根目录作为第一个参数传入。每条 `grep` 都应独立命中；任何一条失败都应视为基线或源码形态不同，而不是忽略后继续推理。

### 练习 1：按焦点寻址事件何时启动无窗口等待

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'int32_t InputDispatcher::findFocusedWindowTargetsLocked(nsecs_t currentTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (focusedWindowHandle == nullptr && focusedApplicationHandle == nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'ALOGI("Dropping %s event because there is no focused window or focused application in "' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (focusedWindowHandle == nullptr && focusedApplicationHandle != nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DEFAULT_INPUT_DISPATCHING_TIMEOUT.count());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mNoFocusedWindowTimeoutTime = currentTime + timeout;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAwaitedFocusedApplication = focusedApplicationHandle;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '*nextWakeupTime = *mNoFocusedWindowTimeoutTime;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return INPUT_EVENT_INJECTION_PENDING;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：比较 window/application 都为空和只有 focused application 两条路径，并解释为何计时从待分发事件到来时才启动。

### 练习 2：等待怎样被解除或升级为无窗口ANR

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!newFocusedWindowHandle && windowHandle->getInfo()->hasFocus &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowHandle->getInfo()->visible) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::resetNoFocusedWindowTimeoutLocked() {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mNoFocusedWindowTimeoutTime = std::nullopt;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (oldFocusedApplicationHandle == mAwaitedFocusedApplication &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'inputApplicationHandle != oldFocusedApplicationHandle) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (currentTime >= *mNoFocusedWindowTimeoutTime) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'onAnrLocked(mAwaitedFocusedApplication);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAwaitedFocusedApplication.clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'android::base::StringPrintf("%s does not have a focused window",' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：推演窗口及时出现、focused application 换人、等待到期三种结局，并写出没有具体 Channel 时的 reason。

### 练习 3：区分dispatching timeout与其他时间尺度

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'constexpr std::chrono::nanoseconds DEFAULT_INPUT_DISPATCHING_TIMEOUT = 5s;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::startDispatchCycleLocked(nsecs_t currentTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->deliveryTime = currentTime;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'getDispatchingTimeoutLocked(connection->inputChannel->getConnectionToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->timeoutTime = currentTime + timeout;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr nsecs_t SLOW_EVENT_PROCESSING_WARNING_TIMEOUT = 2000 * 1000000LL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr nsecs_t STALE_EVENT_TIMEOUT = 10000 * 1000000LL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr std::chrono::nanoseconds KEY_WAITING_FOR_EVENTS_TIMEOUT = 500ms;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL; // 0.5sec' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAppSwitchDueTime = keyEntry.eventTime + APP_SWITCH_TIMEOUT;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'static final long INPUT_METHOD_NOT_RESPONDING_TIMEOUT = 2500;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'if (mCurSender.sendInputEvent(seq, event)) {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mPendingEvents.put(seq, p);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mH.sendMessageDelayed(msg, INPUT_METHOD_NOT_RESPONDING_TIMEOUT);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

要求：分别标注两套 500ms、2s、2500ms、两类默认 5s、10s 的起点和用途，证明 Connection timeout 从成功 publish 的那次 delivery 尝试开始，而非原始 `eventTime`。

### 练习 4：观察outbound到waitQueue的迁移边界

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'while (connection->status == Connection::STATUS_NORMAL && !connection->outboundQueue.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DispatchEntry* dispatchEntry = connection->outboundQueue.front();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '.publishKeyEvent(dispatchEntry->seq, dispatchEntry->resolvedEventId,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (status == WOULD_BLOCK) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->waitQueue.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.erase(std::remove(connection->outboundQueue.begin(),' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.push_back(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->responsive) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.insert(dispatchEntry->timeoutTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：画出 publish 成功、pipe 满且 waitQueue 为空、pipe 满且已有未完成事件三种队列状态，标清哪一种才进入 waitQueue。

### 练习 5：finished signal如何按seq关闭waitQueue账

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'status = connection->inputPublisher.receiveFinishedSignal(&seq, &handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'd->finishDispatchCycleLocked(currentTime, connection, seq, handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const bool handled = commandEntry->handled;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->findWaitQueueEntry(seq);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'reportDispatchStatistics(std::chrono::nanoseconds(eventDuration), *connection, handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.erase(dispatchEntryIt);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.erase(dispatchEntry->timeoutTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'startDispatchCycleLocked(now(), connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'q.mReceiver.finishInputEvent(q.mEvent, handled);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mSeqMap.put(event.getSequenceNumber(), seq);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'int seq = mSeqMap.valueAt(index);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'nativeFinishInputEvent(mReceiverPtr, seq, handled);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'status_t status = mInputConsumer.sendFinishedSignal(seq, handled);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
```

要求：分别代入 `handled=true/false`，证明两者都会关闭当前 waitQueue 账；再说明 Java sequence 与 native DispatchEntry seq 为什么要沿映射核对。

### 练习 6：Connection到期与ANR reason怎样形成

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'nsecs_t InputDispatcher::processAnrsLocked() {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'nextAnrCheck = std::min(nextAnrCheck, mAnrTracker.firstTimeout());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (currentTime < nextAnrCheck) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = false;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.eraseToken(connection->inputChannel->getConnectionToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'onAnrLocked(connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'ALOGI("Not raising ANR because the connection %s has recovered",' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DispatchEntry* oldestEntry = *connection->waitQueue.begin();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'android::base::StringPrintf("%s is not responding. Waited %" PRId64 "ms for %s",' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'oldestEntry->eventEntry->getDescription().c_str());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLastAnrState += StringPrintf(INDENT2 "Reason: %s\n", reason.c_str());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：说明为何先标记 unresponsive 并停止追踪该 token，为什么回调 policy 前还检查 waitQueue，以及 reason 为什么展示 oldest entry。

### 练习 7：native怎样解锁调用policy并处理返回值

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'std::make_unique<CommandEntry>(&InputDispatcher::doNotifyAnrLockedInterruptible);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::doNotifyAnrLockedInterruptible(CommandEntry* commandEntry) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLock.unlock();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->notifyAnr(commandEntry->inputApplicationHandle, token, commandEntry->reason);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLock.lock();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (timeoutExtension > 0) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'extendAnrTimeoutsLocked(commandEntry->inputApplicationHandle, token, timeoutExtension);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'cancelEventsForAnrLocked(connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '// We will not be breaking any connections here, even if the policy wants us to abort dispatch.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'CancelationOptions options(CancelationOptions::CANCEL_ALL_EVENTS,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'abortBrokenDispatchCycleLocked(currentTime, connection, true /*notify*/);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::abortBrokenDispatchCycleLocked(nsecs_t currentTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'drainDispatchQueue(connection->outboundQueue);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'drainDispatchQueue(connection->waitQueue);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->status = Connection::STATUS_BROKEN;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：解释为什么不能持 dispatcher 锁进入 WMS/AMS，以及 extension 大于 0、返回 0、broken dispatch cycle 令 Connection 进入 `STATUS_BROKEN` 三者有何差异。

### 练习 8：Java policy怎样定位责任主体并决定延长或中止

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public long notifyANR(InputApplicationHandle inputApplicationHandle, IBinder token,' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'windowState = mService.mInputToWindowMap.get(token);' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'activity = ActivityRecord.forTokenLocked(inputApplicationHandle.token);' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'mService.saveANRStateLocked(activity, windowState, reason);' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'mService.mAtmInternal.saveANRState(reason);' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'final boolean abort = activity.keyDispatchingTimedOut(reason, windowPid);' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'return activity.mInputDispatchingTimeoutNanos;' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'long timeout = mService.mAmInternal.inputDispatchingTimedOut(windowPid, aboveSystem,' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'return timeout * 1000000L; // nanoseconds' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'return 0; // abort dispatching' frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
grep -n -F 'if (proc.isDebugging()) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (proc.getActiveInstrumentation() != null) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mAnrHelper.appNotResponding(proc, activityShortComponentName, aInfo,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

要求：推演普通 Activity、debugging 进程、instrumentation 进程、无 Activity Window 进程的 abort/extension 语义。

### 练习 9：responsive如何通过延长或完成回执恢复

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'void InputDispatcher::extendAnrTimeoutsLocked(const sp<InputApplicationHandle>& application,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mNoFocusedWindowTimeoutTime = now() + timeoutExtension;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAwaitedFocusedApplication = application;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = true;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const nsecs_t newTimeout = now() + timeoutExtension;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'for (DispatchEntry* entry : connection->waitQueue) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newTimeout >= entry->timeoutTime) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'entry->timeoutTime = newTimeout;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.insert(entry->timeoutTime, connectionToken);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'static bool isConnectionResponsive(const Connection& connection) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (entry->timeoutTime < currentTime) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!connection->responsive) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = isConnectionResponsive(*connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：比较 policy extension 与迟到 finished 两种恢复方式，重点解释 surviving wait entry 的 tracker 索引是否必然恢复。

## 16. 用两条时间线落地诊断，并给下一章留出边界

遇到输入 ANR，先按 reason 把现场放入两条时间线之一。

无 focused window：

```text
按焦点寻址的事件成为 mPendingEvent
  → 只有 focused application，没有 focused window
  → 首次发现时设全局 deadline
  → 窗口出现 / application 换人：重置
  → deadline 到期：保存快照并通知 policy
  → extension：重新等待
     或返回 0：重试时仍无窗口才丢弃；窗口已出现则继续选目标
```

Connection timeout：

```text
选择 Window/Connection
  → DispatchEntry 进入 outbound
  → publish 成功，进入 waitQueue + AnrTracker
  → finished：按 seq 结账
  → deadline 到期：responsive=false、eraseToken、保存快照
  → 若 Connection 仍存在：policy extension 条件式改 deadline 并重建索引
     或 abort 生成取消语义，但不直接断 Channel/杀进程
  → 若 Connection 已消失：不再延长，也不生成取消语义
```

实战时按下面顺序收证据：

1. 读 native reason。`does not have a focused window` 与 `is not responding. Waited` 已经把两条路径分开。
2. 对 Connection 路径，看 dump 中 outbound/waitQueue、oldest entry、deliveryTime/timeoutTime 与 responsive；不要只看 inbound age。
3. 对无窗口路径，对齐 focused display、focused application、可见且 `hasFocus` 的 Window 快照，以及 application 是否在等待期间换人。
4. 查看 `mLastAnrState` 对应的原始时刻，再与 WMS/ATMS 保存状态、ANR trace 的采样时刻区分。
5. 看到应用主线程阻塞在 IME 时，把上一章的 2500ms 内层等待叠到本章外层 delivery deadline 上，而不是相加。
6. 看到 cancel、broken、unresponsive 或 handled=false 时，分别按本章定义核对，避免把协议状态直接翻译成“进程已死”。

可以用四问自测本章是否真正读通：事件有没有成功 publish？等的是 focused application 还是 Connection？当前 deadline 从哪个时间点起算？policy 返回后改变的是 tracker、队列、responsive，还是进程处置？

下一章将转向 pointer 分发：同一个 Motion 如何经过窗口命中、touch state、split、spy/monitor 与 pilfer，形成比“当前 focused window”更复杂的目标集合。
