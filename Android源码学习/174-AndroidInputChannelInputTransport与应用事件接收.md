# 174 Android InputChannel、InputTransport 与应用事件接收

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态只读源码，不要求编译或连接设备  
> 前置章节：第 20、113、163、173 章

---

## 1. 选中窗口以后，事件还要跨四个完成点

第 173 章停在 InputDispatcher 已选出目标 Window。本章继续追一笔 MotionEntry 怎样跨进程成为 App 主线程看到的 MotionEvent，以及完成回执怎样反向结账：

```text
为目标 Connection 创建 DispatchEntry
→ 进入 outboundQueue
→ InputPublisher 把 packet 写入 server socket
→ DispatchEntry 移到 waitQueue
→ App Looper 从 client socket 读取
→ native InputConsumer 构造 InputEvent
→ JNI 回调 Java InputEventReceiver
→ ViewRootImpl InputStage / View 处理
→ Java finishInputEvent(seq, handled)
→ native 把 FINISHED packet 写回同一 socket
→ Dispatcher 读取 FINISHED 并移除 wait entry
```

必须分清：

| 完成点 | 能证明 | 仍不能证明 |
|---|---|---|
| entry 在 outbound | 已决定向该连接派发 | packet 已进入内核 |
| publish 返回 OK / entry 在 wait | 内核接受完整 packet，Dispatcher 开始等回执 | App 已读或已处理 |
| Java callback 开始 | App Looper 已读到并创建 Java 事件 | View 已完成 |
| Dispatcher 移除 wait entry | FINISHED 已被系统处理 | handled=true、画面已更新 |

“事件发给 App 了”若不注明属于哪一行，几乎没有诊断价值。

---

## 2. Binder 只交接 client FD，运行期走全双工 socket

普通 WindowState 建立：

```java
InputChannel[] pair = InputChannel.openInputChannelPair(name);
mInputChannel = pair[0];   // server
mClientChannel = pair[1];  // client
mWmService.mInputManager.registerInputChannel(mInputChannel);
mInputWindowHandle.token = mInputChannel.getToken();
```

server/client 是用途名称，不是单向通信限制。两端属于同一 Unix socketpair，都能 send/recv：

```text
建立阶段：system_server -- Binder Parcel(name, token, FD) --> App
事件阶段：InputDispatcher -- server socket --> client socket -- App
回执阶段：InputDispatcher <-- server socket <-- client socket -- App
```

逐个 Key/Motion 不走 WMS Binder。把 Binder trace 中没有每次触摸当成“输入没发送”，是观察层选错了。

### 2.1 token 与 FD 分别是什么身份

创建 pair 时只 new 一个 `BBinder` token，两端共享它。Dispatcher 用 InputWindowInfo.token 找已注册 connection；token 也让 server/client 被识别为同一逻辑连接。

但两个 InputChannel 对象和两个 FD 不是同一个。FD 经 Binder 复制到 App 的进程 FD 表，整数值可变化；比较两进程的 fd 数字不能判断是否同一 endpoint。

### 2.2 transferTo 是所有权转移

WMS 通过 addWindow 的 out 参数交出 client channel，随后 dispose 自己的 wrapper 并把 `mClientChannel=null`。正常模式下 system_server 不应长期多持一份 client endpoint，否则 App 死亡时额外引用可能拖延 EOF/HANGUP 的出现。

server channel 注册成功也不代表窗口已能命中；还要等上一章的 InputWindowInfo 快照带着相同 token 安装到 Dispatcher。

---

## 3. 源码地图

通道、线协议与测试：

```text
frameworks/native/include/input/InputTransport.h
frameworks/native/libs/input/InputTransport.cpp
frameworks/native/libs/input/tests/
├── InputChannel_test.cpp
├── InputPublisherAndConsumer_test.cpp
└── StructLayout_test.cpp
```

system_server 创建、注册与 Dispatcher：

```text
frameworks/base/services/core/java/com/android/server/wm/
├── WindowState.java
└── WindowManagerService.java

frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp

frameworks/native/services/inputflinger/dispatcher/
├── Connection.h
├── Connection.cpp
└── InputDispatcher.cpp
```

App 接收链：

```text
frameworks/base/core/java/android/view/
├── InputChannel.java
├── InputEventReceiver.java
├── InputEventCompatProcessor.java
└── ViewRootImpl.java

frameworks/base/core/jni/
├── android_view_InputChannel.cpp
└── android_view_InputEventReceiver.cpp
```

---

## 4. 底层是非阻塞 AF_UNIX SOCK_SEQPACKET

真正的创建调用是：

```cpp
socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets);
```

- AF_UNIX：只在本机内核传输；
- socketpair：一次得到两个已连接 endpoint；
- SOCK_SEQPACKET：可靠、有序且保留 packet 边界；
- O_NONBLOCK + MSG_DONTWAIT：Dispatcher 与 App Looper 不在 send/recv 系统调用中睡死。

源码日志偶尔仍写 pipe full，这是历史用词，不能把架构画成匿名 pipe 或字节流 SOCK_STREAM。

### 4.1 send/recv 的状态边界

```text
send/recv 遇 EINTR → 重试
EAGAIN/EWOULDBLOCK → WOULD_BLOCK
对端关闭类错误/EOF → DEAD_OBJECT
其他 errno → 对应负 status
```

sender 还使用 MSG_NOSIGNAL，避免对端关闭时用 SIGPIPE 杀死进程。重试和队列策略由上层 Looper/Connection 决定，不是 socket 自动完成。

### 4.2 32 KiB 只是 setsockopt 请求，而且错误被忽略

r48 对两个 endpoint 的 SO_SNDBUF 与 SO_RCVBUF 都请求 32 KiB，总共调用四次 `setsockopt()`。源码既不读取实际值，也不检查这四个返回码；内核还可能调整缓冲口径。

因此这只能说明设计期背压目标，不能证明运行设备每个方向恰好有 32768 字节。它也不持久化事件：进程死亡后 packet 不会留下。

---

## 5. InputMessage 是固定 ABI packet，seq 才配对 FINISHED

r48 线协议只有四类：

```text
KEY
MOTION
FOCUS
FINISHED
```

KEY/MOTION 带 dispatch seq、eventId、时间、设备/source/display、HMAC 及类型字段；MOTION 还带 scale/offset、pointer 属性与坐标。FINISHED 只需 seq 和 handled。

### 5.1 三种编号不要混用

| 编号 | 产生位置 | 职责 |
|---|---|---|
| eventId | Reader/Dispatcher IdGenerator | 逻辑事件、trace 与部分验证语义 |
| DispatchEntry.seq | 每个目标派发条目 | socket 请求与 FINISHED 精确配对；0 禁用 |
| Java InputEvent sequenceNumber | App 内 Java 对象 | 让 receiver 找回 native dispatch seq |

同一个 EventEntry 投向多个窗口会有多个 DispatchEntry 与 seq。App 回执不能拿 eventId 代替 seq。

r48 的 VerifiedInputEvent 结构没有 eventId；Motion 也只对 DOWN/UP 生成有效签名，普通 MOVE 是 INVALID_HMAC。eventId 是身份，不是不可伪造证明。

### 5.2 layout 必须跨 32/64 位一致

InputMessage 使用显式 padding、8 字节对齐，并由 StructLayout 测试守护。MOTION 的 pointers 固定数组必须是最后一个字段；实际长度按 pointerCount 截短：

```cpp
sizeof(Motion) - sizeof(Pointer) * MAX_POINTERS
               + sizeof(Pointer) * pointerCount
```

接收端要求实际 packet size 精确匹配，并验证 `1 <= pointerCount <= MAX_POINTERS`。它不是 Java Parcel，也不会尝试拼接部分结构。

### 5.3 sanitized copy 防止 padding 泄漏

发送前 `getSanitizedCopy()` 先 memset 整个对象为零，再逐字段复制有效数据；PointerCoords 只复制 bitset 声明存在的 axis。

这是 ABI 与安全边界：若直接发送含未初始化 padding 的 C++ 结构，可能把旧栈字节泄露给另一进程。

---

## 6. Connection 用两个用户态队列记录发送前后

注册 server channel 时，Dispatcher：

```text
检查 token 尚未注册
→ new Connection(inputChannel)
→ mConnectionsByFd[fd] = connection
→ mInputChannelsByToken[token] = inputChannel
→ Looper addFd(serverFd, ALOOPER_EVENT_INPUT)
```

server fd 被监听为 INPUT，是因为 App 的 FINISHED 是 Dispatcher 的入站 packet。

Connection 的核心状态：

```cpp
Status status;                 // NORMAL / BROKEN / ZOMBIE
bool responsive;
std::deque<DispatchEntry*> outboundQueue;
std::deque<DispatchEntry*> waitQueue;
```

### 6.1 outboundQueue

表示已为这个 target 创建、但尚未成功写入 socket 的条目。此时 delivery/timeout 不构成已发送完成账，App 不可能仅凭该队列已经看到事件。

同一个硬件事件面向 foreground、OUTSIDE、wallpaper 或 monitor 时，各 connection 有独立 DispatchEntry、seq、坐标变换和 deadline。

### 6.2 waitQueue

表示 packet 已 publish，但尚未收到对应 FINISHED。send 成功后，Dispatcher 在同一锁内把 entry 从 outbound 移到 wait，并在 connection responsive 时向 AnrTracker 登记 deadline。

waitQueue 与内核 socket 缓冲不是同一对象：packet 可能还躺在内核接收缓冲，逻辑 entry 已经在 wait 中。

---

## 7. Dispatcher 会连续 publish，不是 stop-and-wait

`startDispatchCycleLocked()` 在 connection NORMAL 且 outbound 非空时循环：

```text
取队头
→ deliveryTime = 本轮 currentTime
→ timeoutTime = currentTime + window timeout
→ publish KEY/MOTION/FOCUS
→ 成功：outbound 删除，wait 尾部加入，登记 ANR，继续下一条
```

它不会每发一条就等待 App 回执。Java `InputEventReceiver.onInputEvent()` 的注释声称 finish 前不会再收到新事件，但 r48 native producer/consumer 实现允许一个 connection 多笔在途；诊断应以代码和队列为准。

### 7.1 publish OK 只到内核边界

InputChannel 要求 `send()` 返回字节数恰好等于完整 msgLength，否则当作 DEAD_OBJECT。OK 能证明完整 seqpacket 被本机内核接受，不证明：

- client fd 已变为 App Looper 当前处理对象；
- native consumer 已 recv；
- Java Event 已创建；
- View callback 已结束；
- FINISHED 已返回。

### 7.2 WOULD_BLOCK 的两种解释

若 socket 满且 waitQueue 非空，当前 entry 留在 outbound，Dispatcher等待 App 处理已有 packet、发来 FINISHED 后再调用 startDispatchCycle。

若 WOULD_BLOCK 时 waitQueue 反而为空，源码认为“没有任何已发送在途账却写满”不合预期，直接 abort broken dispatch cycle。它不会无条件丢掉当前 entry 后继续。

非 WOULD_BLOCK 的 publish 错误也进入 broken 清理。deliveryTime 在每次发送尝试前重写，只有成功 publish 后该 timeout 才进入 AnrTracker。

---

## 8. App Looper 会一次读到 WOULD_BLOCK，但 Java 回调可延迟 finish

ViewRootImpl 用 client channel 和当前 Looper 创建 WindowInputEventReceiver。JNI 把 client fd 注册到对应 MessageQueue Looper；普通应用窗口通常就是主线程 Looper。

fd readable 后，native `consumeEvents(..., consumeBatches=false)` 循环：

```text
InputConsumer.consume
→ KEY/MOTION/FOCUS native 对象
→ Key/Motion 转 Java 对象并同步 dispatchInputEvent
→ FOCUS 调专用 onFocusEvent 并自动 finish true
→ 继续读，直到 WOULD_BLOCK / 错误 / 待帧 batch
```

所以“packet 已进入 App 进程的内核缓冲”与“主线程开始 callback”之间仍可被长任务、锁、GC 或消息队列调度拉开。

### 8.1 Java mSeqMap 是对象编号到派发编号的桥

native callback 同时传 dispatch seq 与 Java event。InputEventReceiver 保存：

```java
mSeqMap.put(event.getSequenceNumber(), seq);
```

`finishInputEvent(event, handled)` 再按 Java sequenceNumber 找回真正 seq、删 map、进入 native finish。重复 finish、错误对象或 dispose 后 finish 只会告警；event 最后仍会 recycle。

### 8.2 Java callback 抛异常会吞掉同轮后续可读事件

native receiver 若在 `dispatchInputEvent()` 回调后发现 Java exception，会把本次 `consumeEvents` 的 `skipCallbacks` 置 true。当前以及随后从 socket 读到的事件不再回调 Java，而是直接发送 `FINISHED(..., false)`，直到读到 WOULD_BLOCK 返回；外层再 raise-and-clear exception。

这条异常清理直接调用 `mInputConsumer.sendFinishedSignal()`，忽略返回值，不经过能缓存 WOULD_BLOCK 的 receiver `finishInputEvent()` 包装。若反向 socket 恰好写满，完成信号还可能没有进入 `mFinishQueue`。它不是永久关闭 channel，下一次 fd callback 会重新开始；但同轮事件可被自动判未处理，极端拥塞时还可能留下未结 wait entry。

---

## 9. ViewRootImpl 阶段链决定 handled，但 finish 才终止等待

WindowInputEventReceiver 先让 InputEventCompatProcessor 处理兼容逻辑，再按接收顺序进入 ViewRoot pending queue。队列不会按 eventTime 重排，因为注入事件的时间戳不保证单调。

r48 内置 compat 只对 targetSdk < 23 的 stylus button 做原对象修改并返回单元素列表；接口虽然允许 0/N 个事件，不能据此断言当前内置路径一定把一笔输入拆成多笔。

主要 InputStage：

```text
NativePreImeInputStage
→ ViewPreImeInputStage
→ ImeInputStage
→ EarlyPostImeInputStage
→ NativePostImeInputStage
→ ViewPostImeInputStage
→ SyntheticInputStage
```

其中包含 native InputQueue、pre-IME key、IME、touch mode、View key/touch/generic motion、unhandled key 和 synthetic fallback。只有阶段链最终调用 ViewRoot `finishInputEvent(q)`，Java receiver 才开始反向回执。

### 9.1 AsyncInputStage 可延长 wait，但按 deviceId 防越序

IME 与 native stage 可以返回 DEFER，把事件放入各自异步队列，等待 callback 后 finish 或 forward。

某个 deferred 事件前面还有同 deviceId 条目时，后继不能越过；不同 deviceId 则可独立前进。这意味着同一 connection 的 Java finish 不必总是严格按 waitQueue 头顺序发生。

### 9.2 handled 与 finish 是两个事实

```text
finish=true/发生了 finish
= 这笔 seq 的处理生命周期结束

handled=true
= App/阶段链声称消费了事件
```

handled=false 仍必须发送合法 FINISHED，Dispatcher 仍应删 wait entry。Motion 的 `afterMotionEvent...()` 在 r48 直接返回 false，不做类似 fallback；Key 未处理则可能询问 policy 生成 fallback key，甚至把原 DispatchEntry 放回 outbound 重派。

因此“未消费”不是“没有回执”，“回执了”也不等于“消费成功”。

---

## 10. FINISHED 也受反向背压，真正结账在 Dispatcher

正常 Java finish 进入 NativeInputEventReceiver：

```text
InputConsumer.sendFinishedSignal(seq, handled)
→ client socket send FINISHED
```

若反向 send WOULD_BLOCK，receiver 把 `(seq, handled)` 加入 `mFinishQueue`，把 App Looper 监听从 INPUT 改为 INPUT|OUTPUT；fd 可写时按序重发，清空后恢复只监听 INPUT。

所以 Java `finishInputEvent()` 返回时，FINISHED 可能仍在 App native 用户态队列。非 WOULD_BLOCK/DEAD_OBJECT 的错误可经 JNI 抛 RuntimeException；DEAD_OBJECT 则不抛，但系统侧也不会收到正常回执。

### 10.1 Dispatcher 不是只 pop waitQueue 头

server fd readable 后，Dispatcher 循环 `receiveFinishedSignal()` 到 WOULD_BLOCK。每个 seq 先生成完成 command，随后执行：

```text
findWaitQueueEntry(seq)
→ 计算 finishTime - deliveryTime
→ Key/Motion 后处理（期间可解 Dispatcher lock）
→ 再按 seq 查一次
→ erase wait entry 与 AnrTracker deadline
→ release 或按 Key fallback 放回 outbound
→ startDispatchCycle 重试待发条目
```

两次查找是必须的：policy 调用可解锁，队列可能在中途被其他清理路径 drain。搜索整个 deque 也允许不同设备/异步 stage 的回执不严格按队头到达。

### 10.2 同一轮读到的多个 FINISHED 共用一个 finishTime

`handleReceiveCallback()` 在进入 recv 循环前只调用一次 `now()`，把同一个 currentTime 交给该轮全部 FINISHED command。slow duration 是“这次 server fd 处理批次的近似时刻”，不是每个 packet 单独调用 clock 的精确收包时间。

通常误差很小，但做亚毫秒级事件时间分析时不应从这些 duration 反推每个 App finish 的精确先后间隔。

---

## 11. MOVE 合批用 seq chain 展开多个回执

普通 fd callback 以 `consumeBatches=false` 读取。连续兼容的 MOVE/HOVER_MOVE 按 deviceId+source 等条件进入 InputConsumer Batch，暂不立即创建 Java Event；读到 WOULD_BLOCK 后，native 通知 `onBatchedInputEventPending()`。

ViewRoot 通常把消费安排到 Choreographer INPUT 阶段。以下情况立即 consume：

- unbuffered input dispatch；
- 指定 source 请求 unbuffered；
- ViewRoot `mStopped`，因为之后可能没有 Choreographer callback。

不兼容事件、UP/CANCEL 等也可促使旧 batch 先消费或被特殊清理。

### 11.1 一个 Java MotionEvent 对应多条 wait entry

假设 batch 有：

```text
MOVE seq 101 → 102 → 103
```

`consumeSamples()` 以 103 为 outSeq，创建链：

```text
102 → 101
103 → 102
```

Java 只 finish 一次 103；`sendFinishedSignal(103, handled)` 沿链先发 101、102，再发 103，三条使用同一个 handled。若中途 send 失败，InputConsumer 会重建尚未完成的链，让上层重试仍有 bookkeeping。

`MotionEvent.getHistorySize()` 是合并的采样历史；mSeqChains 是 native 回执账。两者相关但不是同一个容器。

### 11.2 CANCEL 丢弃待合批 MOVE 时也会回执

某些 pointer CANCEL 到来时，InputConsumer 会直接为 batch 中旧 MOVE 逐条发送 handled=false 的 FINISHED，再清 batch。这里同样直接调用 sendFinishedSignal；代码未把返回状态接入 NativeInputEventReceiver 的 mFinishQueue。

在正常通道中这些小 FINISHED 通常写出；极端反向背压下，旧 batch 的直接清理路径也存在完成信号未被缓存的边界。

---

## 12. resampling 改坐标样本，不新增 dispatch seq

r48 读取只读属性 `ro.input.resampling`，默认 true。Choreographer 消费 batch 时使用：

```text
sampleTime = frameTime - 5ms
```

只对 pointer MOVE 且 tool 为 finger/unknown 做 resample。它可能用下一条消息插值，也可能用最近两条历史有限外推。

### 12.1 插值与外推的限制不对称

两种分支不能合并成一句“样本间隔必须 2—20ms”：

| 分支 | 约束 |
|---|---|
| 有 future sample 的插值 | delta < 2ms 时拒绝；本分支没有 20ms 上限检查 |
| 无 future、用两条历史外推 | delta < 2ms 或 >20ms 时拒绝；最多预测 `min(delta/2, 8ms)` |

输出 sampleTime 可能被限制，坐标加到同一个 MotionEvent history/current 语义中。它不创建新的 EventEntry、DispatchEntry 或 seq；FINISHED 仍对应承载这些样本的原始 message chain。

因此 resampled 坐标不是新的硬件采样事实。需要还原真实采集点时，要看原始 eventTime/history 与 resampled 标记，而不是把 View 读到的最后坐标直接当传感器时刻。

---

## 13. 输入 ANR 从成功 publish 的 deliveryTime 开始

每次发送尝试前 Dispatcher 设置：

```cpp
deliveryTime = currentTime;
timeoutTime = currentTime + getDispatchingTimeoutLocked(token);
```

只有 publish 成功并把 entry 加入 waitQueue 后，responsive connection 的 timeout 才写入 AnrTracker。WOULD_BLOCK 留在 outbound 的时间不属于这条 wait deadline；下次尝试会重写 deliveryTime。

默认 dispatch timeout 为 5 秒，窗口/应用 handle 可提供具体值。计时覆盖：

```text
内核接收缓冲等待 App Looper
+ native/Java 构造
+ ViewRoot pending queue 与 InputStage
+ IME/native DEFER
+ App finish 逻辑
+ FINISHED 反向排队并到达 Dispatcher
```

它不包含事件仍在 InputReader/Dispatcher inbound、尚未向这个窗口成功 publish 的全部早期延迟。

### 13.1 2 秒 slow log 不是 5 秒 ANR

处理 FINISHED command 时，`finishTime-deliveryTime > 2s` 会打印 slow processing 日志。它：

- 只有事件最终 finish 后才能计算；
- 使用第 10 节所说的批次 currentTime；
- 不等于已触发 ANR；
- 不包括 FINISHED 被 Dispatcher 读到之后的后处理耗时。

真正 ANR 由 AnrTracker 最早 deadline 触发。

### 13.2 unresponsive 是政策状态，不是 socket 状态

到期时 Dispatcher 先设 `responsive=false`、删除该 token 的 AnrTracker 条目并异步通知 policy。policy 可返回正 extension，使 connection 恢复 responsive 并重登记需要延长的 wait entries；否则取消该 connection 的事件。

若迟到 FINISHED 到达，移除 entry 后 `isConnectionResponsive()` 可依据剩余 deadline 重新判健康。响应慢并不自动说明 FD 已断开。

---

## 14. BROKEN、ZOMBIE、死亡窗口与注入等待是四种收尾

| 状态/路径 | 含义 | 主要动作 |
|---|---|---|
| responsive=false | 通道存在但事件超时 | policy 延长或取消，可能恢复 |
| STATUS_BROKEN | publish/receive/HANGUP 等不可恢复错误 | drain outbound+wait，可通知 WMS |
| STATUS_ZOMBIE | 显式 unregister | 移除 fd/token/Looper 监听后 drain，不再使用 |
| DeadWindow receiver | App 已死但窗口暂留可见 | system_server dummy client 立即 finish true |

### 14.1 主动销毁先 unregister，再关 FD

WMS `disposeInputChannel()` 先向 Dispatcher unregister server channel，再 dispose server/client。这样正常窗口移除不会因先出现 HANGUP 被误报为 broken。

意外 client 关闭时，server fd 收到 HANGUP/ERROR 或 DEAD_OBJECT。Dispatcher 只有对非 monitor 且仍存在 WindowHandle 的 connection 才请求 broken 通知，随后注销通道。policy 可沿 token 找 WindowState 并 `removeIfPossible()`。

drain queue 会 release 每个 DispatchEntry；若它是注入事件的 foreground target，也会减少 pendingForegroundDispatches。因此清理可以终止等待，但不代表 App 正常处理过事件。

### 14.2 DeadWindowEventReceiver 不把事件交给旧 View 树

某些 App 死亡后窗口会暂留可见以便点击触发重启。`openInputChannel(null)` 不把 client 交给 App，而在 WMS Handler Looper 创建 dummy receiver，所有输入立即 finish true。

它保证 socket 有消费者并让 input monitor 看见触摸；旧 App View 树已经不存在，handled=true 只是 dummy 的确认。

### 14.3 WAIT_FOR_FINISHED 只等 foreground 派发账归零

注入模式：

```text
SYNC_NONE → 入队后不等结果
WAIT_FOR_RESULT → 等选目标/注入结果
WAIT_FOR_FINISHED → 成功后再等 pendingForegroundDispatches == 0
```

每个 foreground DispatchEntry 创建时计数加一，release 时减一。正常 FINISHED、broken drain、ANR cancel 等都可能让它归零；它不要求 handled=true，也不等待由事件触发的绘制、present 或业务异步任务。

---

## 15. 诊断矩阵与九组静态源码练习

先按队列定位：

| 现场 | 更可能的阶段 | 下一证据 |
|---|---|---|
| 找不到目标 Connection | 上一章的窗口快照/token/选窗 | `dumpsys input` WindowHandles |
| outbound 很长、wait 非空 | App 落后导致正向背压 | connection 状态、App finish、socket 错误 |
| wait 有超龄 entry | packet 已发但完成链未闭合 | App Looper、InputStage、mFinishQueue、ANR |
| App 有 callback，Dispatcher wait 不退 | finish 未调用、反向阻塞/错误、seq 错 | mSeqMap、JNI、FINISHED receive |
| 单个 Java MOVE 对应多条 wait | 正常 batch | history、mSeqChains、逐 seq FINISHED |
| slow >2s 但无 ANR | 已完成但较慢 | window timeout 是否仍未到 |

以下命令只读 `android-11.0.0_r48` 工作树。

### 练习 1：追 server/client 所有权

```bash
sed -n '2450,2520p' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n 'writeToParcel|readFromParcel|transferTo' frameworks/base/core/java/android/view/InputChannel.java frameworks/base/core/jni/android_view_InputChannel.cpp
```

标出 pair、共享 token、server 注册、client transfer 与主动注销顺序。

### 练习 2：核对 socket 与错误映射

```bash
sed -n '235,370p' frameworks/native/libs/input/InputTransport.cpp
```

确认四次 buffer 设置是否检查返回码，并区分 WOULD_BLOCK、DEAD_OBJECT 与部分写。

### 练习 3：检查线协议与 sanitized copy

```bash
sed -n '75,190p' frameworks/native/include/input/InputTransport.h
sed -n '115,235p' frameworks/native/libs/input/InputTransport.cpp
```

说明 pointers 为什么必须最后，以及 padding 为什么不能原样跨进程。

### 练习 4：画出 Connection 双队列迁移

```bash
sed -n '45,75p' frameworks/native/services/inputflinger/dispatcher/Connection.h
sed -n '2450,2620p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

逐条标出 delivery/timeout、publish、WOULD_BLOCK、outbound erase、wait push 与 ANR insert。

### 练习 5：观察 App receiver 的 drain 与异常路径

```bash
sed -n '110,220p' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
sed -n '220,365p' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
sed -n '105,195p' frameworks/base/core/java/android/view/InputEventReceiver.java
```

比较正常 finish 的 mFinishQueue 与 skipCallbacks 直接 send 的错误处理差异。

### 练习 6：核对 ViewRoot 同设备异步顺序

```bash
sed -n '5295,5575p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '7968,8125p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

说明 DEFER、deviceId 阻塞、handled flag 与最终 receiver finish 的关系。

### 练习 7：手算 batch seq chain

```bash
sed -n '620,820p' frameworks/native/libs/input/InputTransport.cpp
sed -n '1060,1125p' frameworks/native/libs/input/InputTransport.cpp
```

用 seq 101/102/103 验证链建立顺序、FINISHED 发送顺序和失败时重建逻辑。

### 练习 8：区分插值与外推上限

```bash
sed -n '45,82p' frameworks/native/libs/input/InputTransport.cpp
sed -n '925,1060p' frameworks/native/libs/input/InputTransport.cpp
```

找到 future 插值没有 20ms 上限检查、历史外推才有 2—20ms 与 8ms cap 的源码证据。

### 练习 9：闭合 FINISHED、ANR 与注入计数

```bash
sed -n '2690,2770p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '3420,3570p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4725,4820p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4500,4710p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

回答 FINISHED 收包、wait erase、slow log、policy ANR、foreground 计数分别在哪个完成点。

---

## 16. 本章结论与自检

核心模型：

```text
InputChannel
= Binder 一次性交接 FD 与 token
+ 运行期全双工非阻塞 seqpacket

Dispatcher 完成账
= outbound（未成功 publish）
+ wait（已 publish、未 FINISHED）
+ per-target seq/deadline

App 完成账
= Looper recv
+ Java sequenceNumber → dispatch seq
+ InputStage handled
+ FINISHED 正常发送或 finish queue
```

完成本章后，应能回答：

- 为什么 server/client 是用途名而不是单向 socket？
- 32 KiB 为什么只是未经确认的请求值？
- eventId、dispatch seq 与 Java sequenceNumber 分别做什么？
- publish OK 为什么只证明内核接受 packet？
- r48 为什么不是一发一回的 stop-and-wait？
- Java callback 抛异常为何可能自动 finish 同轮后续事件，甚至在反向拥塞时漏回执？
- handled=false 为什么仍是有效完成，Key 又为何可能 fallback？
- 一个合批 Java MOVE 怎样关闭多条 wait entry？
- 插值与外推为何不能共用“2—20ms”一句话？
- ANR 从何时计时，2 秒 slow log 与 5 秒默认 timeout 有何区别？
- WAIT_FOR_FINISHED 为什么可因清理归零且不等画面 present？

下一章进入 **ViewRootImpl InputStage、IME 前后阶段与 View 事件分发**，把本章概览的 App 阶段链展开到 pre-IME/post-IME、ViewGroup 命中、intercept、CANCEL 与最终 handled。
