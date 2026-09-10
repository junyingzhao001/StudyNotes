# 187 Android 输入事件批处理与触摸重采样：多条消息怎样变成一帧 MotionEvent

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> 核心问题：Dispatcher 已经逐条 publish 的多笔移动消息，为什么到 App 时可能只回调一个带 history 的 `MotionEvent`，它又怎样在不丢 FINISHED 账的前提下对齐一帧 VSync？

---

## 1. 先看现象：三条 MOVE 只回调一次，不等于中间两条被 Dispatcher 丢了

### 一条常见时间线

假设同一根手指已经 DOWN，随后连接上出现：

```text
M1: MOVE @  8 ms, x=10
M2: MOVE @ 11 ms, x=13
M3: MOVE @ 14 ms, x=17
U4: UP   @ 16 ms, x=18
```

App 不一定收到三次 MOVE 回调。它可能先收到一个 `MotionEvent`：

```text
history[0] = M1
history[1] = M2
current    = M3
```

然后再收到 `U4`。若按有效 frame time 消费且开启 resampling，current 还可能是额外估算出来的 sample，最近的真实消息则退入 history。

### 四句话必须同时成立

1. Dispatcher 为每个目标窗口的每个 `DispatchEntry` 分配独立 transport seq。
2. `InputPublisher` 仍对每笔 MOVE 写一条独立 `InputMessage::MOTION`。
3. App 进程里的 native `InputConsumer` 才把兼容 MOVE 装进一个 `MotionEvent`。
4. Java 最终只 finish 一次合成事件，但 `InputConsumer` 会把这个完成结果展开为每个底层 seq 的 FINISHED。

所以“Java 回调数变少”只能证明发生了客户端聚合，不能单独证明 Reader、Dispatcher 或 socket 丢了采样。

### 本章的完成点

本章追到的终点不是“View 看见一个坐标”，而是：

```text
多条 InputMessage
  → 一个 MotionEvent 的 history/current
  → 可选 synthetic current
  → 一次 Java handled
  → 多条 FINISHED
  → Dispatcher 逐项清 waitQueue
```

---

## 2. 不要混账：这里同时有四本账和两类时间

### 四本账回答不同问题

| 账本 | 所在端 | 一项代表什么 | 能证明什么 |
|---|---|---|---|
| `DispatchEntry.seq` / waitQueue | system_server Dispatcher | 某事件给某 connection 的一次发送 | 这条底层发送是否已收到 FINISHED |
| `InputConsumer::Batch.samples` | App native | 尚未交给 Java 的兼容 MOTION 消息 | 哪些消息仍在等批次消费 |
| `MotionEvent.mSampleEventTimes/Coords` | App native/Java 对象 | history 与 current sample | 这次 Java 回调携带了哪些 sample |
| `mSeqChains` | App native | 合成事件的最后 seq 怎样追回前序 seq | 一次 Java finish 要展开成哪些 FINISHED |

`Batch` 与 Dispatcher waitQueue 从来不是同一对象，而且可以同时记录同一批底层发送：`consumeSamples()` 只从 App 端 Batch 移除 samples，不会改变 waitQueue；后者必须等 FINISHED 或 connection 清理。`MotionEvent.history` 也不是 Dispatcher 的 recent queue。它们只是相关数据在不同阶段、不同进程里的独立表示。

### 一个 Java sequence 也不是 transport seq

JNI 把 native 返回的 transport `seq` 作为参数传给 Java `dispatchInputEvent(seq, event)`。Java 又用 `event.getSequenceNumber()` 作为 `SparseIntArray mSeqMap` 的 key：

```text
Java InputEvent sequence
    └──映射到 native 返回的 transport seq（batch 时为最后一条）
```

App 调用 `finishInputEvent(event, handled)` 时先查这张 map，再把 transport seq 交回 native。不要拿 `MotionEvent.getId()`、Java sequence 与 socket seq 相互替代。

### 两类时间也不能混

| 时间 | 来源 | 用途 |
|---|---|---|
| `InputMessage.eventTime` | 输入采样链的 monotonic 时间 | 排 history、选可消费前缀、做插值/外推 |
| `frameTimeNanos` | Choreographer 本帧 frame time，同为 monotonic/`System.nanoTime()` 基准 | 决定这一帧最多消费到哪里 |

`frameTimeNanos` 不是 App 真正开始处理的 wall clock，也不是显示 present fence。r48 只是用它算消费与预测目标。

### 两个同名 TouchState

第 186 章的 `InputDispatcher::TouchState` 保存窗口路由；本章 `InputConsumer::TouchState` 保存最近两份真实坐标和上一份重采样坐标。二者：

- 位于不同进程端；
- key 不同；
- 生命周期不同；
- 没有共享对象或自动同步关系。

后文单说 `TouchState` 时，均指 `InputConsumer` 内部这一个。

---

## 3. Dispatcher 发送侧没有 history：一笔 DispatchEntry 仍是一条消息

### MotionEntry 只容纳一个 eventTime

r48 `MotionEntry` 的核心形状是：

```cpp
nsecs_t eventTime;
uint32_t pointerCount;
PointerProperties pointerProperties[MAX_POINTERS];
PointerCoords pointerCoords[MAX_POINTERS];
```

它没有 sample 数组。Reader 连续产生三笔 MOVE，Dispatcher 看到的是三项独立事件，而不是一项自带三段 history 的事件。

### 每个 connection 的坐标版本可不同

`startDispatchCycleLocked()` 从 connection 的 outboundQueue 取一个 `DispatchEntry`，按该目标的：

- resolved action/flags；
- window X/Y scale 与 offset；
- global scale；
- `ZERO_COORDS`；
- split 后 pointer 集合

形成这次 publish 的字段。批处理发生得再晚，也不能倒推出所有窗口收到同一组原始坐标。

### publishMotionEvent 一次只写一份 sample

`InputPublisher::publishMotionEvent()` 构造的 `InputMessage::MOTION` 包含：

```text
seq / eventId / deviceId / source / displayId / hmac
action / flags / metaState / buttonState / classification
scale / offset / precision / cursorPosition
downTime / eventTime
pointerCount × {PointerProperties, PointerCoords}
```

消息里没有 `historySize`，也没有多组 eventTime。历史只能在接收端把多条消息装入 `MotionEvent` 后出现。

### SOCK_SEQPACKET 保留消息边界，但缓冲有限

`InputChannel` 使用 non-blocking Unix `SOCK_SEQPACKET`。每次 `sendMessage()`：

1. 先按字段生成清零 padding 的 sanitized copy；
2. 按本消息实际 size 一次 `send(..., MSG_DONTWAIT | MSG_NOSIGNAL)`；
3. 写不下返回 `WOULD_BLOCK`，对端关闭返回 `DEAD_OBJECT`。

32 KiB socket buffer 只是在 App 暂时落后时容纳若干消息，不会在内核里把 MOVE 合成 history，也不是无限队列。

### stale、pruning 与 batching 是三件事

Dispatcher 的 stale、app-switch、blocked pruning 可能让事件在到达 App 前被丢弃；InputConsumer batching 则是把**已经成功穿过 channel 的兼容消息**合并交付。看到较少回调时，必须先区分：

```text
上游 drop                 客户端 batch
没有进入该 App 的 channel    已逐条进入 channel
没有对应 Java sample          成为 history/current
按各自原因完成或清理           仍靠 seq chain 逐条 FINISHED
```

---

## 4. fd 可读时先“吸干消息”：consume(false) 怎样把 MOVE 留在 native

### Looper 回调先禁止主动消费 Batch

`NativeInputEventReceiver::handleEvent()` 收到 `ALOOPER_EVENT_INPUT` 后调用 `consumeEvents(env, false, -1, nullptr)`。

这里的 `false` 不是“禁止建 Batch”，而是“channel 暂时读空时，不要主动把仍可继续追加的 Batch 交给 Java”。

### consume 每轮先决定消息从哪里来

`InputConsumer::consume()` 反复执行：

```text
mMsgDeferred 为真
  → 重用上一轮已从 socket 取出的 mMsg
否则
  → receiveMessage(&mMsg)

有事件可返回
  → 结束本次 consume
只有可积累 MOVE
  → 继续读下一条
socket WOULD_BLOCK 且 consumeBatches=false
  → 暂留 Batch
```

因此一次 fd callback 可以读取很多条消息；是否多次回调 Java，取决于读到的是即时事件还是被暂留的 batchable move。

### 只有两个精确 action 会开新 Batch

当同组 Batch 尚不存在时，只有：

```cpp
action == ACTION_MOVE || action == ACTION_HOVER_MOVE
```

才会把消息放进新 `Batch.samples`。这不是 masked-action 范围判断，也没有先要求 pointer class：

- touchscreen MOVE 可 batch；
- mouse HOVER_MOVE 可 batch；
- 非 pointer source 若产生精确 MOVE，也可进入 Batch；
- DOWN、UP、POINTER_DOWN/UP、SCROLL、CANCEL、HOVER_ENTER/EXIT、BUTTON_* 不会成为 Batch 头。

结构边界因此仍以独立事件交付。

### 追加成功仍不立即返回 Java

若同组 Batch 已存在且 `canAddSample()` 为真，新消息只会 `push()`。switch 结束后外层 while 继续收下一条，直到：

- 遇到必须先冲刷旧 Batch 的同组边界；
- 遇到可立即返回的别类事件；
- channel 暂时读空；
- channel 出错或关闭。

“收到第一条 MOVE”与“App 得到第一个 MOVE 回调”之间可以隔着一个 VSync callback。

### mBatches 可以同时有多组

`mBatches` 是 vector，不是一条全 channel 的唯一 batch。不同 `deviceId + source` 可各自积累：

```text
Batch A: device 3 + TOUCHSCREEN
Batch B: device 7 + MOUSE
Batch C: device 9 + JOYSTICK
```

这为后面的跨组交付顺序与 pending-source 边界埋下了伏笔。

---

## 5. 能不能追加，只检查很窄的一组字段

### findBatch 的 key 没有 displayId

`findBatch()` 只比较 batch head 的：

```text
deviceId 相同 && source 完全相同
```

`displayId` 不在 key 中。正常窗口路由通常不会把同一 consumer 的同一设备/source 跨 display 混用，但 r48 的这个函数本身没有替调用者守这条边界。

### canAddSample 比较三件事

找到同组 Batch 后，当前消息必须同时满足：

1. `pointerCount` 相同；
2. 完整 `action` 相同；
3. 每个 array index 上的 `PointerProperties` 相同。

r48 的 `PointerProperties` 只有 pointer id 与 tool type。即使 id 集合相同，只要顺序改变，也不能追加。

### 坐标不比较，时间也不校验单调

坐标本来就应随 MOVE 改变，所以不参与兼容判断。更容易忽略的是：`canAddSample()` 也不检查后到消息的 `eventTime` 是否严格递增。后续的“取不晚于目标的前缀”依赖正常输入链维持消息顺序与时间顺序，函数本身没有排序或修复异常时间。

### 绝大多数标量来自本次 consumed prefix 的第一条

`consumeSamples()` 用**本次传入前缀**的第一条消息 `initializeMotionEvent()`，后续只做：

```cpp
event->setMetaState(event->getMetaState() | msg.metaState);
event->addSample(msg.eventTime, msg.pointerCoords);
```

于是字段来源是：

| 最终 MotionEvent 内容 | 来源 |
|---|---|
| eventId、displayId、HMAC | 第一条 |
| action/actionButton、flags、edgeFlags | 第一条 |
| buttonState、classification | 第一条 |
| scale、offset、precision | 第一条 |
| cursorPosition、downTime | 第一条 |
| PointerProperties | 第一条，且兼容检查保证后续相同 |
| 每份 sample 的 eventTime/PointerCoords | 各自消息 |
| metaState | 所有被合并消息按位 OR |

所以“后一个 sample 的所有属性都保存在 history 里”是错的。history 主要保存每次时间与 pointer coordinates。

### 被忽略的变化不会形成逐 sample 元数据

`canAddSample()` 不比较：

- displayId；
- flags / edgeFlags；
- buttonState / classification；
- downTime；
- x/y scale、offset、precision；
- cursor X/Y；
- eventId / HMAC；
- metaState。

其中 metaState 还有 OR 汇总，其余大多静默沿用本次 prefix 第一条。若一组 Batch 被分帧消费，余下 suffix 下次会成为新的 consumed prefix，重新提供对象字段头、metaState 聚合与独立 seq chain。r48 依赖连续同流 MOVE 的这些字段通常稳定；若上游在兼容 action/properties 不变时改了它们，Java 对象不能逐 history sample 还原变化。

### cursorPosition 是一个尤其直观的例子

`MotionEvent` 的 raw cursor X/Y 是对象级标量，不在 `addSample()` 的坐标数组里。HOVER_MOVE 虽可 batch，合成对象的 cursorPosition 仍来自本次 prefix 第一条；各 sample 的 pointer X/Y 则来自各自 `PointerCoords`。二者不能被默认视为同一份逐点历史。

---

## 6. 边界到来时不是统一“先清 Batch”：要按消息类别分支

### 同组不兼容 MOTION：旧 Batch 先交付，新消息 defer

当前 MOTION 与已有同 `deviceId + source` Batch 不兼容时，普通路径会：

```text
mMsgDeferred = true
consumeSamples(旧 Batch 的全部 samples)
remove 旧 Batch
返回合成 MotionEvent

下一次 consume
  → 不读 fd，先重新处理 mMsgDeferred
```

这保证同一 motion 组里，先前 MOVE 不会被后到的 UP、POINTER 变化或属性变化越过。

这个强制 flush 直接调用 `consumeSamples()`，不经过 `consumeBatch()` 尾部的 `resampleTouchState()`；即使调用者带着有效 frameTime 且属性开启，也不会为旧 Batch 新建 synthetic sample。已有 `lastResample` 导致的逐消息 rewrite 仍可能发生。

### deferred 已离开 socket

`mMsgDeferred` 里的消息已经被 `receiveMessage()` 取走。此时 fd 可能不再 readable，所以调用者必须：

- 像 JNI 一样持续调用 consume，直到 `WOULD_BLOCK`；或
- 看 `hasDeferredEvent()` 并确保事件循环至少再醒一次。

只等下一次 fd readable 会把这条用户态消息饿死。标准 JNI 的 `consumeEvents()` 会在同一轮 native drain 中立刻再次调用 `consume()`，所以通常无需等下一次 fd callback。

### pointer CANCEL 直接丢弃旧 MOVE

若不兼容消息满足：

```text
source 属于 SOURCE_CLASS_POINTER
&& action 精确等于 ACTION_CANCEL
```

`InputConsumer` 不先构造旧 MOVE 的 `MotionEvent`。它对 batch 中每个 seq 调 `sendFinishedSignal(seq, false)`，随后清 Batch，再把当前 CANCEL 作为独立事件交给 Java。

中间 MOVE 在客户端主动被舍弃，但 Dispatcher 的每条发送账仍应各自收到 `handled=false`。

### CANCEL 丢弃路径有一个回执背压缺口

上述循环直接调用 `InputConsumer::sendFinishedSignal()`，返回值没有被检查，也没有经过 `NativeInputEventReceiver::mFinishQueue`。若反向 socket 此刻 `WOULD_BLOCK`，这些被丢 MOVE 的 FINISHED 没有在此路径重试。

这不是“CANCEL 会删 Dispatcher waitQueue”的无条件保证。正常写成功才会逐项结账；失败后的最终收敛要依赖 connection 清理、超时等外围路径。

### 非 pointer CANCEL 没有这条丢弃特权

batch 的建立不要求 pointer source，但特殊 CANCEL 分支要求 `isPointerEvent(source)`。非 pointer motion 组遇到 CANCEL 会走普通“不兼容：交付旧 Batch、defer CANCEL”，不能把触摸规则外推给所有 MOVE source。

### 不同组或不同消息类型可以越过旧 Batch

旧 Batch 只在**同组 MOTION** 到来时参与兼容检查：

- 另一 device/source 的 MOVE 可以另开 Batch；
- 另一组的非 batch MOTION 可以直接返回；
- KEY 与 FOCUS 分支不会先冲刷某个 pending Motion Batch。

因此 r48 只维护同一 batch 组的 motion 边界顺序，不把整个 channel 按全局 eventTime 重新归并。晚进 socket 的另一组事件可能先回调 Java。

同组 Batch 还可以跨过已即时交付的别类消息继续追加。例如 `A-MOVE(seq10) → KEY(seq11) → A-MOVE(seq12)` 可形成 `12 → 10` 的 seq chain；batch 内的 transport seq 不要求连续。

---

## 7. channel 读空以后，JNI 才把“有 Batch 待办”翻译成 Java 调度

### WOULD_BLOCK 是 pending 通知的入口

`NativeInputEventReceiver::consumeEvents()` 循环调用 `InputConsumer.consume()`。当它得到 `WOULD_BLOCK`，并同时满足：

```text
没有因 Java callback 异常而 skipCallbacks
&& mBatchedInputEventPending == false
&& InputConsumer.hasPendingBatch()
```

才调用 Java `onBatchedInputEventPending(getPendingBatchSource())`。然后 native 函数对这次 fd callback 返回 `OK`；pending 通知本身不是 MotionEvent 交付。

### mBatchedInputEventPending 是单个 receiver 级门

它不是每个 Batch 一位。第一次通知后置 true，直到有人以 `consumeBatches=true` 调 native consume 才先清 false。

这样多条 MOVE 不会为每个 sample 都向 Java 重复排 callback；一轮 batch consumption 若仍没有可消费 sample，native 又能重新发 pending 通知，安排后续机会。

### pending source 只取 vector 第 0 项

`getPendingBatchSource()` 返回 `mBatches[0]` 的 head source。它不表示：

- 所有 pending Batch 都同 source；
- 第 0 项一定是下一笔真正被消费的 Batch；
- 这个 source 的 sample 已经早于本帧目标时间。

ViewRoot 用它判断 source-specific unbuffered 模式时，应记住这只是单个代表值。

这个代表值却会替整个 receiver 做一次调度选择：第 0 组若命中 unbuffered，随后 `consumeBatchedInputEvents(-1)` 会把其他 source 的 Batch 也一起 full-flush；第 0 组若未命中、但后面的组命中 source mask，当前通知仍会选择 buffered，后面的组可以继续等 `CALLBACK_INPUT` 并受 frame cutoff。receiver 级单 latch 没有逐组补发一份独立调度决定。

### 基类 InputEventReceiver 立即全量 flush

基类默认实现：

```java
public void onBatchedInputEventPending(int source) {
    consumeBatchedInputEvents(-1);
}
```

`-1` 会把 Batch 全量消费，不按 VSync 切分，也不生成新的 resampled sample。只有覆写 pending 调度的 receiver，才会真正获得逐帧对齐行为。

### BatchedInputEventReceiver 政策是等 CALLBACK_INPUT

`BatchedInputEventReceiver` 覆写 pending callback，把一个 Runnable 投到 `Choreographer.CALLBACK_INPUT`。Runnable 用 `getFrameTimeNanos()` 消费。

若 native 返回“本轮消费过 batch”且 frameTime 不是 -1，它再排下一帧，避免剩余 future samples 因 fd 已读空而饥饿。

### Java callback 异常会改变 drain 方式

JNI 将 native event 转成 Java 后调用 `dispatchInputEvent()`。若 callback 抛异常，它把 `skipCallbacks=true`；抛异常的**当前事件**会在本轮末尾直接尝试 `sendFinishedSignal(seq, false)`，随后 drain 到的事件也不再回调 Java，而是走同样的 false finish。

这条异常兜底会避免继续进入 Java 业务回调，但直接发送同样没有接入 `mFinishQueue`。Java 又是在调用 `onInputEvent()` 前先写 `mSeqMap`；native 直接 finish 不会删除该 Java mapping，若上层竟捕获异常并继续运行，它会成为 stale entry。

异常本身并没有被这套 drain 吞掉：fd 路径会在消息循环返回时重新抛出，显式 batch consume 也会带着 pending JNI exception 回到 Java，主线程通常随之中断或崩溃。若异常发生在 pending 通知，native 会把 `mBatchedInputEventPending` 改回 false；只有在异常被外围捕获、线程继续存活这一前提下，fd 已读空的旧 Batch 才谈得上等待新输入或显式 consume 再调度。

---

## 8. 普通窗口不是直接用 BatchedInputEventReceiver：ViewRootImpl 自己实现两种节奏

### WindowInputEventReceiver 的三道立即消费门

普通窗口使用 `ViewRootImpl.WindowInputEventReceiver`。收到 pending source 后，它计算：

```text
unbuffered =
    mUnbufferedInputDispatch
 || (source & mUnbufferedInputSource) != SOURCE_CLASS_NONE
 || mStopped
```

若为 true：

1. 取消已排的 Choreographer batch callback；
2. 直接 `consumeBatchedInputEvents(-1)`；
3. 不等下一帧，也不生成新的 resampled sample。

`mStopped` 被单列，是因为 stopped window 可能不再得到 Choreographer callback；继续攒 Batch 会把 Dispatcher waitQueue 拖到 ANR。

### buffered 模式排在本帧最早阶段

正常模式调用 `scheduleConsumeBatchedInput()`，将 Runnable 投到 `CALLBACK_INPUT`。r48 `Choreographer#doFrame()` 的顺序是：

```text
INPUT
  → ANIMATION
  → INSETS_ANIMATION
  → TRAVERSAL
  → COMMIT
```

所以本帧批次先进入 ViewRoot input stages，动画与遍历随后才读取新的交互状态。这是“对齐一帧”的真正收益。

### ViewRoot 的 Runnable 也会自续下一帧

`ConsumeBatchedInputRunnable.run()` 先清 `mConsumeBatchedInputScheduled`，再用本帧 frame time 调 `doConsumeBatchedInput()`。若返回 true，重新 schedule 下一帧。

即使当前没有 sample 早于目标时间，native 在看到 pending Batch 后也可能同步回调 `onBatchedInputEventPending()`，重新安排下一帧；两条重排路径靠 boolean 门去重。

### doConsumeBatchedInput 最后推进 Java pending queue

```java
consumeBatchedInputEvents(frameTimeNanos);
doProcessInputEvents();
```

JNI 每生成一个 Java InputEvent，就调用 `WindowInputEventReceiver.onInputEvent()`；它以 `processImmediately=true` 入 ViewRoot 队列，通常当场进入 InputStage。外层再调用一次 `doProcessInputEvents()`，保证 native consumption 后的 Java pending queue 继续推进。

这仍不承诺所有 async InputStage 已 finish；IME 或其他异步 stage 可把完成点继续向后推。

### View 请求 unbuffered 后，已有 VSync 任务会被改道

触摸处理期间，`View.requestUnbufferedDispatch(event)` 可置 `mUnbufferedDispatchRequested`。ViewRoot 发现请求后进入 `mUnbufferedInputDispatch=true`：

- 若已有 VSync batch callback，改排 Handler immediate runnable；
- 后续 pending batch 直接以 -1 消费；
- 收到该 touch stream 的 UP/CANCEL 等 terminal event 后退出，并重新允许 buffered 调度。

按 source 的 `requestUnbufferedDispatch(int)` 则由 `mUnbufferedInputSource` 控制，不会按一次 touch terminal 自动清除。这个重载只更新并向祖先传播 source mask，不会调用 immediate scheduler；若 pending latch 已置位且 VSync callback 已经排好，现有 Batch 仍可能先等到该 VSync。

### stopped 状态变化也主动排一次 immediate flush

`setWindowStopped()` 最后调用 `scheduleConsumeBatchedInputImmediately()`。这个方法：

1. 取消 Choreographer batch callback；
2. 在 ViewRoot Handler 上 post immediate runnable；
3. 用 frameTime=-1 drain。

它是 Looper 消息，不是调用栈内同步执行；“立即”表示脱离下一次 VSync 等待。

---

## 9. 真正消费 Batch 时，frameTime 决定前缀；vector 位置决定先看哪一组

### frameTime 小于 0：一次取完整的一组

`consumeBatch()` 遇到 `frameTime < 0` 时：

```text
取一组 Batch 的全部 samples
  → consumeSamples
  → 从 mBatches 删除该组
  → 不调用 resampleTouchState
```

一个 `InputConsumer.consume()` 调用只返回一个 `MotionEvent`；JNI 外层会继续调用，才把其余组也 drain 完。

“全量消费”不等于“必有 history”。若这组只有一条 MOVE，`MotionEvent` 只有 current，`historySize == 0`；只有 N 条真实 sample 且 N 大于 1 时，才有 N−1 条 history。

### 有效 frameTime：先算 sampleTime

```text
resampling property 关闭：sampleTime = frameTime
resampling property 开启：sampleTime = frameTime - 5 ms
```

这里减 5 ms 发生在 action/source 重采样资格检查**之前**。只要 consumer 的属性开关为真：

- HOVER_MOVE Batch 也用 `frameTime - 5ms` 选真实前缀；
- 非 pointer source 的 ACTION_MOVE Batch 也一样；
- 它们随后虽不会生成 synthetic sample，真实消息仍可能多留一帧。

所以 5 ms 不只影响“最后发生坐标预测的手指”。

### findSampleNoLaterThan 取的是连续前缀

实现从 `samples[0]` 开始，遇到第一项：

```text
eventTime > sampleTime
```

就停止，并返回前一项 index。等时 `eventTime == sampleTime` 可以消费。

它不是扫描全 vector 后找最大合法 eventTime。若时间异常乱序：

```text
8 ms, 20 ms, 10 ms    sampleTime=12 ms
```

只会选到 8 ms；20 ms 让扫描停止，后面的 10 ms 不会被越过去重新挑出。这也是前面说“函数依赖正常单调时间”的原因。

### 目标前没有 sample 就跳过这一组

返回 `-1` 表示该 Batch 没有可消费前缀。`consumeBatch()` 不会为了本帧强行拿 future sample，而是继续向 vector 更前面找其他组；若所有组都不满足，就返回 `WOULD_BLOCK`。

native 仍看到 `hasPendingBatch()`，因此能再安排下一帧，不需要新的 fd readable 才续命。

### 部分消费会留下一个明确的 next

消费 `[0..split]` 后：

- Batch 空：从 vector 删除，`next=null`；
- Batch 未空：`next=&samples[0]`，即第一条未消费消息。

开启 resampling 时，`resampleTouchState(sampleTime, event, next)` 只看这个 next，不会任意搜索更远的 future。

### 多组 Batch 从 vector 尾向头找

循环顺序是：

```cpp
for (size_t i = mBatches.size(); i > 0; ) {
    i--;
    ...
}
```

也就是先看后创建的组。它不按各组最早 eventTime 做全局排序；这又与 `getPendingBatchSource()` 报告第 0 组形成不对称。

### consumeBatches=true 仍先尽量读 channel

`InputConsumer.consume()` 只有在 `receiveMessage()` 返回非零状态时，才从 pending groups 主动调用 `consumeBatch()`；最常见状态是 channel 已读空的 `WOULD_BLOCK`。

因此显式 batch consumption 开始后，consumer 仍会先处理已经排在 socket 里的新消息。另一组即时事件可能先被返回；同组不兼容消息则走上一节的 flush/defer 分支。

---

## 10. consumeSamples 怎样把 N 条消息排成 history 与 current

### 第一条 initialize，后续 addSample

核心循环是：

```text
i == 0
  → initializeMotionEvent(first)
i > 0
  → OR metaState
  → addSample(eventTime, pointerCoords)
```

`MotionEvent` 内部有平行数组：

```text
mSampleEventTimes
mSamplePointerCoords
```

`getHistorySize()` 等于 sample 数减一；最后一份永远是 current，前面的才由 `getHistorical*()` 读取。

### N 条真实消息、无 synthetic 时

| 输入真实 sample 数 | historySize | current |
|---:|---:|---|
| 1 | 0 | 第 1 条 |
| 2 | 1 | 第 2 条 |
| 3 | 2 | 第 3 条 |

因此 `historySize` 不是这次合并的消息总数，真实 sample 总数是 `historySize + 1`——但只有确定没有 synthetic 时，这个“真实”二字才成立。

### 成功 resample 后，所有真实点都退入 history

`resampleTouchState()` 最后再调用一次：

```cpp
event->addSample(sampleTime, lastResample.pointers);
```

假设这次消费了 3 个真实点并成功生成 1 个 synthetic：

```text
history[0] = real 1
history[1] = real 2
history[2] = real 3
current    = synthetic
historySize = 3
```

此时用 `historySize + 1` 推算底层消息数会多算一条。

### synthetic sample 没有公开标签

最终 Java `MotionEvent` 不带“current 是插值/外推”的独立 bit。App 可以看到时间与坐标，却不能仅凭对象可靠区分：

- 最后一份是真实消息；
- 最后一份是 synthetic；
- 较早真实消息曾被 `rewriteMessage()` 改过 X/Y。

诊断时必须结合消费模式、属性和时序推理，不能只看 historySize。

### eventId 与 outSeq 来自不同位置

合成对象的 `eventId` 由本次 consumed prefix 第一条消息初始化；`consumeSamples()` 输出的 transport `outSeq` 则不断更新，最终是最后一条**真实已消费消息**的 seq。

额外 synthetic sample 没有自己的 Dispatcher eventId 或 transport seq。它只是附加到这个 Java 对象里的坐标时间点。

### metaState 是唯一显式聚合的对象级标量

后续 sample 的 metaState 会与现值按位 OR。最终对象只能证明这一批里某处出现过对应 meta bit，不能指出它从哪个历史位置开始或结束。

---

## 11. 一个 Java finish 怎样展开成 N 个 transport FINISHED

### consumeSamples 边加 sample 边建链

三条底层消息 seq 为 `41, 42, 43` 时：

```text
第一条：chain = 41
第二条：记录 42 → 41，chain = 42
第三条：记录 43 → 42，chain = 43
outSeq = 43
```

一条 sample 的 Batch 没有 `SeqChain`，只直接返回自己的 seq。

### Java map 只记最后 seq

JNI 调用：

```java
dispatchInputEvent(43, motionEvent)
```

`InputEventReceiver` 再记录：

```text
motionEvent.getSequenceNumber() → 43
```

View/InputStage 最终调用 `finishInputEvent(motionEvent, handled)`，才把 43 送回 native。Java 不需要认识 41 与 42。

### sendFinishedSignal 逆向找链、正向发送

`InputConsumer::sendFinishedSignal(43, handled)` 从 `mSeqChains` 尾部反查：

```text
43 → 42 → 41
```

然后实际发送顺序是：

```text
FINISHED 41
FINISHED 42
FINISHED 43
```

对应 links 在找到时从 vector 删除。其他尚未 finish 的 batch chain 仍保留。

若同一大组被按 frame cutoff 分成两次输出，例如先消费 `[41,42]`、后消费 `[43,44]`，两次各建自己的链；不会补一条 `43 → 42`。seq chain 描述的是一次 Java 合成事件，不是 device/source 的整段手势。

### handled 粒度被合并

同一个 Java `handled` 会复制到 41、42、43。App 不能说：

```text
history[0] handled=false
history[1] handled=true
current    handled=true
```

批处理保留了逐 seq 完成数量，没有保留逐 sample 的不同消费结论。

### 正常 Java finish 遇到反向背压会排队

`NativeInputEventReceiver.finishInputEvent()` 先调用 `InputConsumer.sendFinishedSignal()`。若返回 `WOULD_BLOCK`：

1. 把最终 seq 与 handled 放进 `mFinishQueue`；
2. Looper 同时监听 `ALOOPER_EVENT_OUTPUT`；
3. 可写时重试。

`InputConsumer` 在某个前序 FINISHED 发送失败时还会重建剩余 `SeqChain`，所以下次用最终 seq 重试仍能补齐未发部分。

### 这与 CANCEL 丢 Batch 的路径不同

普通 Java finish：

```text
最终 seq → NativeInputEventReceiver → 可排 mFinishQueue
```

pointer CANCEL 丢弃未交付 Batch：

```text
每个旧 seq → InputConsumer 内直接 send → 返回值被忽略
```

二者都调用 `sendFinishedSignal()`，但只有前者有外围 WOULD_BLOCK 重试。判断 Dispatcher waitQueue 是否必然出队时，必须保留这个差别。

### Dispatcher 按 seq 找任意等待项，不要求只完成队首

收到 FINISHED 后，Dispatcher 用 `findWaitQueueEntry(seq)` 线性寻找匹配项，并不要求它正好是 waitQueue 头。因此不同组、不同 Java 事件可以乱序完成；一次 batch 展开出来的 FINISHED 则按旧到新发送。

### batch 等待也占用 Dispatcher 超时账

每条消息 publish 成功后都以自己的 `timeoutTime` 进入 connection waitQueue；connection 仍 responsive 时，每项还会写入 `mAnrTracker`。放入 App native Batch 并不等于完成。等 VSync、UI 线程繁忙或 async InputStage 延迟，都会延后 FINISHED。

一次合成事件正常 finish 可连续清掉多条等待项，但在 finish 发生之前，这些独立 deadline 仍然存在。

---

## 12. 重采样开关不仅控制 lerp：它还控制 cutoff、TouchState 与 rewrite

### 属性在 InputConsumer 构造时读一次

```text
ro.input.resampling
默认 true
```

构造函数把结果保存为 `const bool mResampleTouch`。已有 consumer 不会在每帧或每个 DOWN 时重读；而且这是 `ro.` 属性，不应把运行中改值当成正常热切换协议。

### 关闭属性后仍会 batch

关闭只会让：

- 有效 frameTime 的 cutoff 从 `frameTime - 5ms` 变为 `frameTime`；
- `updateTouchState()` 直接返回；
- 不执行 synthetic resample；
- 不执行依赖该 state 的坐标 rewrite。

`ACTION_MOVE/HOVER_MOVE` 的 Batch 建立、compatibility 与 seq chain 全都还在。

### 生成 synthetic 的顶层资格

在真实前缀已被消费后，`resampleTouchState()` 仍要求：

```text
mResampleTouch == true
&& source 属于 SOURCE_CLASS_POINTER
&& event.getAction() 精确等于 ACTION_MOVE
&& 找到同 deviceId + source 的 TouchState
&& TouchState 至少有一份 history
```

HOVER_MOVE 虽然能 batch，也受 5 ms cutoff，却在 exact-action 门返回；mouse 若是 ACTION_MOVE 可过 source/action 门，但具体 pointer 的坐标是否 lerp 还要看 tool type。

### TouchState 是两槽 raw history 加一份 lastResample

```text
history[0/1]：最近两份输入消息的 raw 时间、id 集合、PointerCoords
lastResample：上一次成功生成的 sampleTime、id 集合、PointerCoords
```

两槽 history 通过 `historyCurrent ^= 1` 轮换。它不是无限轨迹，也不是 Java `MotionEvent.history` 的镜像。

### MOVE 先存 raw，再改将要交付的消息

`updateTouchState(MOVE)` 的顺序是：

```text
touchState.addHistory(msg)
touchState.rewriteMessage(msg)
```

所以两槽 history 保留的是 rewrite 前的消息坐标；`recentCoordinatesAreIdentical()` 比较的也是最近两份 raw X/Y。当前 `InputMessage` 本身随后可能被改，再用于构造 Java MotionEvent。

### 各 action 怎样维护 state

| masked action | history | lastResample / rewrite | state 生命周期 |
|---|---|---|---|
| DOWN | initialize 后加入 history | 清空旧 lastResample | 建立或重建 |
| MOVE | 加入两槽 history | 再 rewrite | 保留 |
| POINTER_DOWN | 不加入 history | 先清新 id，再 rewrite 其他 id | 保留 |
| POINTER_UP | 不加入 history | 先 rewrite，再清离开 id | 保留 |
| SCROLL | 不加入 history | rewrite | 保留 |
| UP/CANCEL | 不加入 history | 先 rewrite | 随后删除整项 |
| 其他 | 不处理 | 不处理 | 不变 |

UP/CANCEL 不是“只负责清 state”；交给 App 的终止事件 X/Y 也可能先被 rewrite。

因为 POINTER_DOWN/UP、SCROLL、UP/CANCEL 都不先加入 history，它们调用 `recentCoordinatesAreIdentical()` 时看的仍是此前两份 DOWN/MOVE raw history，而不是把当前边界消息也纳入比较。

### CANCEL 丢掉的 Batch 不进入 raw history

pointer CANCEL 特殊分支对旧 samples 直接 finish(false) 并从 Batch 删除，没有逐项调用 `updateTouchState()`。随后当前 CANCEL 才走普通单事件构造：rewrite 后移除 TouchState。

因此两槽 raw history 并不保证记录所有已从 socket 读到的 MOVE。

### key 同样没有 displayId

`findTouchState()` 也只比较 `deviceId + source`。若同一个 consumer 真收到同键、不同 display 的事件：

- 兼容 MOVE 可跨 display 合进同一 MotionEvent，顶层 displayId 来自第一条；
- raw history、future 与 lastResample 可跨 display 混用；
- 另一 display 的 DOWN 会重建共享 state；
- UP/CANCEL 会删除共享 state；
- pointer CANCEL 甚至可丢同键的 pending Batch。

正常窗口拓扑让这种情况少见，但 r48 代码没有 display 级隔离。

---

## 13. 插值与外推共用 sampleTime，却有两套不对称边界

### 先确定 current

`consumeBatch()` 已经消费所有连续满足：

```text
eventTime <= sampleTime
```

的前缀。`TouchState.getHistory(0)` 因此通常是最后一条已消费 raw 消息；合成 `MotionEvent` 的 pointer 集合必须全部存在于这份 current history，否则整次 resample 返回。

### 有 next 就只尝试插值

若 Batch 还留有第一条 future：

```text
currentTime <= sampleTime < nextTime
delta = nextTime - currentTime
alpha = (sampleTime - currentTime) / delta
out = lerp(current, next, alpha)
```

在时间正常单调时，alpha 位于 `[0, 1)`。

### 插值只有 2 ms 下限

若 `delta < 2ms`，本次 resample 直接返回：

- 不找更远的 future；
- 不改用 past 外推；
- 不生成 synthetic sample。

`delta == 2ms` 可以。插值没有 20 ms 上限，也没有 8 ms prediction cap；那两项只属于无 future 的外推分支。

### 没有 next 才尝试外推

至少有两份 raw history 时：

```text
pastTime <= currentTime <= sampleTime
delta = currentTime - pastTime
```

允许边界为：

```text
2 ms <= delta <= 20 ms
```

源码拒绝的是 `< 2ms` 与 `> 20ms`，所以 2 与 20 都包含。

### 外推目标还会被截短

```text
maxPredict =
    currentTime + min(delta / 2, 8ms)
sampleTime = min(sampleTime, maxPredict)
```

最近采样间隔 6 ms 时最多向前 3 ms；间隔 20 ms 时理论一半为 10 ms，但 8 ms cap 把它截成 8 ms。

截短后的 sampleTime 才会写进 `lastResample.eventTime` 与最终 MotionEvent。

### 负 alpha 不是参数写反

外推用：

```text
alpha = (currentTime - sampleTime) / delta
lerp(current, past, alpha)
```

sampleTime 在 current 之后时 alpha 为负。因为第二个点是 past，负 alpha 会从 current 沿“past → current”的速度方向继续向前。

例如：

```text
past:    t=8,  x=10
current: t=12, x=18
target:  t=14

delta=4
alpha=(12-14)/4=-0.5
x=18 + (-0.5) × (10-18) = 22
```

### sampleTime 等于 current 也可能追加重复时间

这时 alpha 为 0；只要其他门都通过，代码仍会 `addSample(sampleTime, ...)`。`MotionEvent::addSample()` 不拒绝与现有 current 相同的 eventTime，于是可出现两个同时间 sample。

### resample 失败不会顺便清旧 lastResample

缺 state、history 不足、current 缺 id、delta 太小/太大等分支都是直接 return。旧 `lastResample` 仍留在 TouchState，后续 `rewriteMessage()` 仍可能用它。

---

## 14. 工具类型只控制 X/Y 的 lerp；synthetic 与 rewrite 覆盖范围更大

### 所有 pointer 都会进入成功的 synthetic sample

通过顶层与时间门后，循环会为最终 MotionEvent 的每个 pointer：

1. 把 id 写进新的 `lastResample.idBits`；
2. 先准备一份 PointerCoords；
3. 最终随同一次 `event->addSample()` 追加。

`shouldResampleTool()` 不决定“有没有这个 synthetic pointer”，只决定是否用 other point 对 X/Y 做 lerp。

### 只有 FINGER 与 UNKNOWN 做 X/Y 估算

```text
FINGER / UNKNOWN
  → other 中也有该 id 时，计算 X/Y

STYLUS / ERASER / MOUSE / 其他
  → X/Y 保持 current
```

所以混合工具事件中可以出现：手指 X/Y 被预测，触笔 X/Y 复制最近 raw current，但二者都处在同一个 synthetic sampleTime。

### 普通分支先复制全部当前轴，再只替换 X/Y

若不走“静止复用”分支：

```text
resampledCoords = copy(currentCoords)
replace X
replace Y
```

pressure、size、orientation、tilt 等来自当前 raw sample。这个结论只适用于这一普通计算分支。

### 静止复用会复制上一份完整 PointerCoords

若：

```text
旧 lastResample 含该 id
&& 最近两份 raw X/Y 完全相同
```

代码直接：

```cpp
copyFrom(oldLastResample.getPointerById(id));
continue;
```

这里复制的是上一份**全部 axes**，不只是 X/Y。即使最新 raw pressure、size 或 tilt 已变化，只要 raw X/Y 相同，它们也可能暂时沿用旧 resample 值。

因此“重采样永远只影响 X/Y，其他轴永远来自最新真实点”并不成立。

### current 缺一个 id 是整次失败，other 缺 id 只影响该指

在计算前，current history 必须包含最终 event 的每个 pointer id；少一个就整次 return。

进入逐指循环后，若 `other` 没有某个 id：

- 该 pointer 不 lerp X/Y；
- 其他有 id 且工具允许的 pointer 仍可计算；
- 最终 synthetic sample 仍包含完整 pointer 集合。

匹配 Batch 的 future 通常有相同 properties；other 缺 id 更常出现在跨 POINTER 生命周期的 past history。

### rewriteMessage 只写 X/Y，却不检查 tool type

对每个仍在 `lastResample.idBits` 的 id，满足任一条件就把消息 X/Y 替换成 lastResample：

```text
msg.eventTime < lastResample.eventTime
|| 最近两份 raw X/Y 完全相同
```

这里没有 `shouldResampleTool()`。所以 STYLUS、ERASER、MOUSE 只要曾进入成功 synthetic 的 id 集合，也可能走 rewrite；只是它们先前保存的 X/Y 通常等于当时 current。

### “真实坐标变了就清 bit”还缺一个时间条件

只有两个条件都不成立时才清 id bit：

```text
msg.eventTime >= lastResample.eventTime
&& 最近两份 raw X/Y 不同
```

因此 raw X/Y 已变化但消息时间仍早于 synthetic 时间时，消息依旧被覆盖，bit 不清。

等时边界也很精确：

- `eventTime < lastResampleTime`：一定 rewrite；
- `eventTime == lastResampleTime`：不算“早于”，由 raw X/Y 是否相同决定 rewrite 或清 bit。

### POINTER_DOWN 与 POINTER_UP 的顺序不同

- POINTER_DOWN：先清新 action id 的 lastResample bit，再 rewrite 其余 pointers；
- POINTER_UP：先 rewrite 整条消息，再清离开 id；
- UP/CANCEL：先 rewrite，再删整个 TouchState；
- SCROLL：可 rewrite，但不清 state。

这防止新 pointer 直接继承同 id 的旧预测位置，却允许终止事件在离开前与上一份 resample 坐标连续。

---

## 15. 一张判定表串起“何时延迟、何时 synthetic、何时完成”

### 消费与 resample 结果矩阵

| 场景 | 真实前缀 cutoff | 新 synthetic | 额外边界 |
|---|---|---:|---|
| `frameTime=-1` | 整组 | 否 | 单 sample 仍可能 historySize=0 |
| property 关闭、有效 frame | `<= frameTime` | 否 | TouchState/rewrite 也关闭 |
| property 开启、HOVER_MOVE | `<= frameTime-5ms` | 否 | exact ACTION_MOVE 门失败 |
| property 开启、非 pointer MOVE | `<= frameTime-5ms` | 否 | pointer-class 门失败 |
| pointer MOVE、无对应 DOWN state | `<= frameTime-5ms` | 否 | findTouchState 失败 |
| 有 future，delta < 2ms | `<= frameTime-5ms` | 否 | 不回退到外推 |
| 无 future，raw delta <2 或 >20ms | `<= frameTime-5ms` | 否 | 2/20ms 边界本身允许 |
| 门全部通过 | `<= frameTime-5ms` | 是 | 插值或受限外推 |

`frameTime=-1` 不生成**新的** synthetic；但属性开启且旧 `lastResample` 尚有效时，`consumeSamples → updateTouchState → rewriteMessage` 仍可能改写 full-flush 消息的 X/Y。

### JNI 返回的 consumedBatch 是位测试，不是严格证明

JNI 置 `outConsumedBatch=true` 的条件是：

```cpp
motionEvent->getAction() & ACTION_MOVE
```

这里没有与 MOVE 做相等比较，也没有先 masked。低位含 `0x2` 的多个 action 都会命中，例如：

```text
MOVE(2)
CANCEL(3)
POINTER_UP(6)
HOVER_MOVE(7)
HOVER_EXIT(10)
BUTTON_PRESS(11)
```

带 pointer index 的高位不改变这个低 bit。由于一次 `consumeEvents(true, ...)` 还会读并交付非 Batch 消息，返回 true 可能只是遇到这些 action，而不证明真的消费了 Batch。

在正常且尚未进入 `skipCallbacks` 的路径上，真正交付的 batch MOVE/HOVER_MOVE 都会让这个测试为真；实际后果主要是 false positive 可能多排一个 Choreographer callback。若此前某个 Java callback 已抛异常，后续真实 Batch 会被 native drain 并 false-finish，却绕过这段位测试，又形成异常路径的 false negative。

### receive 错误还有一个结果覆盖边界

`receiveMessage()` 返回非零时，只要：

```text
consumeBatches == true
|| 原结果不是 WOULD_BLOCK
```

`consume()` 就调用 `consumeBatch()`，并用后者结果覆盖前者。于是：

- channel 已关闭但还有 pending Batch 时，可能先成功交付 Batch；
- 没有可消费 Batch 时，`DEAD_OBJECT`、`BAD_VALUE` 等可能被新的 `WOULD_BLOCK` 覆盖。

Looper 还可从 HANGUP/ERROR 事件移除 callback，但不能把 `consume()` 的返回值简单解释成未经改写的 socket 状态。

### 四段排障不要互相替代

| 观察点 | 常见积压 | 直接证据 |
|---|---|---|
| Dispatcher inbound/pending | focus、policy、blocked、stale | `dumpsys input` dispatcher state/recent |
| connection outbound/wait | publish 背压、App 未 finish | connection queue、ANR/trace |
| App native Batch | 等 VSync、cutoff 前无 sample、多个 group | pending callback 与消费时序，常规 dump 不直接列 samples |
| ViewRoot/InputStage | UI 忙、IME/async stage defer | ViewRoot pending 数、input trace、应用侧时序 |

`dumpsys input` 看不到 App 进程 `mBatches` 的每个 sample；ViewRoot dump 的 `mConsumeBatchedInputScheduled` 也只能证明 Java 任务状态，不能列出 native Batch 内容。

### App 侧最有用的是打印整条 sample 时间

```java
void dumpMotion(MotionEvent e) {
    for (int h = 0; h < e.getHistorySize(); h++) {
        Log.d("Input", "H " + h + " t=" + e.getHistoricalEventTimeNano(h)
                + " x=" + e.getHistoricalX(0, h));
    }
    Log.d("Input", "C t=" + e.getEventTimeNano() + " x=" + e.getX(0));
}
```

它能证明一次回调携带几份 sample、时间是否单调，却仍不能单独证明 current 是否 synthetic，也看不到对应的每个 transport seq。

### 四个可手算时间线

场景 A：立即 flush。

```text
Batch: 8, 11, 14ms
frameTime=-1
输出：history=8,11；current=14；无新 synthetic
```

场景 B：future 插值，且同 `deviceId + source` 的 pointer DOWN 已先建立 TouchState。

```text
frameTime=16ms，property 开
sampleTime=11ms
Batch: 8, 14ms
消费真实 8；next=14
delta=6，alpha=0.5
输出：history=real@8；current=synthetic@11
14ms 仍留在 Batch
```

场景 C：无 future 外推。

```text
raw past: 4ms x=4
raw current: 10ms x=10
frameTime=20ms → requested sampleTime=15ms
delta=6，最多预测 3ms
实际 syntheticTime=13ms
alpha=(10-13)/6=-0.5
x=13
```

场景 D：pointer CANCEL。

```text
未交付 MOVE seq 51,52
CANCEL seq 53 到来
51/52 尝试 FINISHED(false)，不进 Java、不进 SeqChain
53 经 Java/InputStage 自己 finish
```

若 51/52 的反向 send WOULD_BLOCK，它们不能靠这条特殊循环保证出队。

### 最短源码导航

```bash
sed -n '601,814p' frameworks/native/libs/input/InputTransport.cpp
sed -n '815,1068p' frameworks/native/libs/input/InputTransport.cpp
sed -n '1068,1235p' frameworks/native/libs/input/InputTransport.cpp
sed -n '223,340p' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
sed -n '8137,8270p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '700,735p' frameworks/base/core/java/android/view/Choreographer.java
```

建议每读一个条件，就写下它改变的是：Batch、Java sample、TouchState、seq chain，还是 Dispatcher waitQueue。只写“事件被消费”很容易跨层。

---

## 16. 九组练习：把一帧输入拆成可验证的状态迁移

### 练习一：区分四本账

为 MOVE seq `101,102,103` 画四行：

```text
Dispatcher waitQueue
InputConsumer Batch
MotionEvent history/current
mSeqChains
```

分别填写：三条刚 publish、Batch 尚未消费、合成事件已交 Java、Java finish 成功四个时刻。确认“离开 Batch”不等于“离开 waitQueue”。

### 练习二：手算 compatibility

准备五条消息：

1. 同 device/source/action/count/properties，只改坐标；
2. 只改 metaState；
3. 只改 displayId；
4. 交换两个 pointer 的 array index；
5. 把第二指 toolType 从 FINGER 改为 STYLUS。

写出哪些能追加。对能追加的条目，再写最终对象的 displayId、metaState 与 PointerProperties 从哪里来。

### 练习三：证明边界顺序不是全 channel 顺序

按 socket 顺序输入：

```text
touch MOVE A
mouse MOVE B
KEY K
touch UP U
```

逐轮跟踪 `mBatches`、`mMsgDeferred` 与 Java 回调。解释为什么 K 或另一组事件可越过 A，而同组 U 必须先触发 A 的 flush。

### 练习四：比较三种 frameTime

Batch 时间为 `8, 11, 14, 18ms`。分别计算：

- `frameTime=-1`；
- `frameTime=16ms` 且 property 关闭；
- `frameTime=16ms` 且 property 开启。

列出被消费真实前缀、剩余 next、historySize，以及是否进入插值。

### 练习五：比较 future 与 past

给定：

```text
past=6ms,x=6
current=10ms,x=10
target=13ms
```

先设 next=`14ms,x=18`，再设 next=null。分别计算 alpha 与输出 X，并指出哪条路径受 20ms/8ms 约束。

### 练习六：构造同时间 synthetic

令 target 恰好等于 currentTime，且 next-current 恰好 2ms。沿源码写出：

- `findSampleNoLaterThan()` 选哪一项；
- interpolation alpha；
- 是否调用 `addSample()`；
- 最终相同 eventTime 的两份 sample 分别位于 history 还是 current。

### 练习七：追静止点的非 X/Y 轴

让上一份 lastResample 的 `pressure=0.4`，新 raw 点 X/Y 与前一 raw 完全相同，但 pressure 变为 0.8。进入静止复用分支后，写出 synthetic pressure。

再让 X/Y 改变且 eventTime 仍早于 lastResampleTime，判断本条消息的 X/Y 与 bit 是否会清。这个练习用于击破两句过强概括。

### 练习八：展开 FINISHED 与背压

为链：

```text
203 → 202 → 201
```

分别推演：

- 三条 send 全成功；
- 201 成功、202 返回 WOULD_BLOCK；
- pointer CANCEL 丢 Batch 时第一条就 WOULD_BLOCK。

比较 `mSeqChains` 重建、`mFinishQueue` 与 Dispatcher waitQueue 的差别。

### 练习九：做一次最小应用观测

在 `onTouchEvent()` 打印：

- action；
- historySize；
- 每个 historical eventTime/X；
- current eventTime/X；
- callback 到达时的 `System.nanoTime()`。

切换普通 buffered 与 `requestUnbufferedDispatch(event)`，只提出源码允许支持的结论。不要仅凭 historySize 宣称硬件采样率，也不要把回调到达时间当成 frameTime。

### 一页心智模型

```text
Dispatcher
  每个目标、每笔采样：独立 DispatchEntry + seq
        │ publish
        ▼
InputConsumer
  同 device/source 找 Batch
  action/count/properties 决定能否追加
        │ frameTime cutoff
        ▼
MotionEvent
  第一条给对象字段
  后续给 time/coords，meta OR
  可选再加 synthetic current
        │ Java finish(last seq)
        ▼
SeqChain
  oldest ... newest 逐条 FINISHED
        │
        ▼
Dispatcher waitQueue 逐项出队
```

最容易犯的四个错误是：

1. 把 Dispatcher 队列取舍叫成 MOVE batching；
2. 把 history 每一项想成拥有完整独立 MotionEvent 字段；
3. 把 FINGER/UNKNOWN 资格误写成“其他工具不进入 synthetic/rewrite”；
4. 把一次 Java finish 误写成 Dispatcher 只等待一条消息。

### 自检问题

1. `consumeBatches=false` 为什么仍会建立 Batch？
2. 哪三个字段组决定一条 MOVE 能否追加，displayId 为什么是危险缺口？
3. 单 sample Batch 在 frameTime=-1 下为什么没有 history？
4. property 开启后，HOVER_MOVE 为什么可能受 5ms cutoff 却不 resample？
5. 插值失败为什么不会自动回退到 past 外推？
6. 成功 resample 后，N 条真实消息为什么对应 historySize=N？
7. 静止复用为何可能连 pressure 一起沿用旧值？
8. Java 只 finish 最后 seq，前序 waitQueue 怎样出队？
9. `nativeConsumeBatchedInputEvents()` 返回 true 为什么不能作为“必然消费过 Batch”的严格证据？

### 下一章

第 188 章进入输入完成确认与 WaitQueue 出队：从 Java sequence map、InputStage 的同步/异步 finish、native finish queue、`InputPublisher::FINISHED` 到 Dispatcher 的逐项完成与 ANR 账，追“一笔事件何时才真正不再等待”。
