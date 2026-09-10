# 188 Android 输入完成确认与 WaitQueue 出队：一次 handled 怎样关闭系统账本

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> 核心问题：App 已经收到甚至处理完输入，为什么 Dispatcher 仍可能认定它“没有响应”？一条 Java `finishInputEvent()` 又怎样穿过三套序号、两端 socket 队列和 policy 后处理，最终删掉正确的 `waitQueue` 项？

---

## 1. 从失败现场定义“完成”

### App 收到，不等于 Dispatcher 完成

一条 Key、Motion 或 Focus 写入目标 `InputChannel` 后，`InputDispatcher` 不能立刻销毁这次投递的状态。它还要等待 App 沿同一条双向 channel 发回 `FINISHED`，其中带着本次投递的 transport seq 和 handled。

必须分开三个时刻：

1. Dispatcher 已决定目标；
2. 消息已成功写入目标 channel；
3. App 已发回与该投递匹配的完成确认。

在正常协议结算中，只有第 3 步闭合，Dispatcher 才会按 seq 从该 connection 的 `waitQueue` 移除对应 `DispatchEntry`；HUP、unregister 或 broken drain 也能无 FINISHED 地 release 它，但那表示传输终止，不是 App 完成。
源码锚点：`Connection.h:55-60`、`InputDispatcher.cpp:2602-2612,2655-2694,2696-2762,4379-4401,4751-4806`。

### 最典型的失败现场

假设窗口主线程已经执行 `onTouchEvent()`，业务回调也已返回，但接收链没有成功调用 `InputEventReceiver.finishInputEvent()`。App 可能认为工作结束，system_server 看到的却仍是：

```text
connection.waitQueue: 该 transport seq 仍在
FINISHED: 尚未收到
ANR deadline: 尚未删除
```

`waitQueue` 记录的是协议上的未确认投递，不是 View 回调是否运行过，也不是业务代码是否返回。

### 同一种“未出队”可能停在四处

一次输入迟迟未完成，至少可能停在：

- `outboundQueue`：尚未成功写入 channel；
- App native batch：消息已从 socket 读走，但 MOVE 尚未生成 Java `MotionEvent`；
- ViewRoot/InputStage：Java 对象仍在同步处理或异步等待；
- native `mFinishQueue`：Java 已 finish，但反向 `FINISHED` 遇到写背压。

这四处都可能让 Dispatcher 的完成状态迟迟不前，但它们不是同一个队列。尤其是 App native 已读走消息，仍不代表 Dispatcher 已收到完成确认。

### 普通 Key/Motion 的最小闭环

```text
EventEntry
  -> 为一个 connection 创建 DispatchEntry(channelSeq)
  -> outboundQueue
  -> publish 成功
  -> waitQueue
  -> InputConsumer.consume(outSeq=channelSeq)
  -> 创建 Java InputEvent(java mSeq)
  -> mSeqMap[java mSeq] = channelSeq
  -> ViewRoot/InputStage
  -> finishInputEvent(javaEvent, handled)
  -> 查表得到 channelSeq
  -> FINISHED(channelSeq, handled)
  -> Dispatcher 按 channelSeq 查 waitQueue 并移除
```

`dispatchInputEvent(int seq, InputEvent event)` 的参数 `seq` 是 channel/transport seq；Java 对象编号是 `event.getSequenceNumber()`，不能把前者标成 `javaSeq`。

### handled 与完成是两个维度

`handled=false` 仍是当前 publish leg 的合法完成凭证，含义是“目标没有消费，但这轮处理已给出结果”。相反，业务处理过事件却没有 `FINISHED`，协议上仍未完成。Key 的 false 还可能让同一 `DispatchEntry` 进入 fallback 第二轮，因此收到本轮完成也不必然等于 entry 已最终 release。

可以把状态理解成：

```text
(是否收到 FINISHED, 收到后携带的 handled 值)
```

只有第一项为真，第二项才进入后续 Key/Motion policy 语义。

### waitQueue 不是 Java 队列

`Connection.h` 的定义是：`outboundQueue` 保存待发布事件，`waitQueue` 保存已发布但尚未收到 finished response 的事件。Java 侧还可能有 ViewRoot pending queue 和各 AsyncInputStage 私有队列。

因此以下推断都不成立：

- `waitQueue.size()==1` 等于 Java 当前只有一个对象；
- Java pending queue 为空等于没有未完成输入；
- App 从 fd 读走消息等于 waitQueue 可以出队；
- handled=false 等于 Dispatcher 应继续等待。

分析正常协议结算时，核心观察对象是“某个 connection 上某个 transport seq 的 FINISHED 是否回来”；分析队列突然清空时还必须排除 broken/unregister drain，因为它无需 FINISHED。

### 错误 finish 也会制造假象

`InputEventReceiver.finishInputEvent()` 不按对象 identity 查找，而是读对象当前 `mSeq`，再查当前 receiver 的 `mSeqMap`。copy、错误 receiver 或回收后复用的对象，可能查不到，也可能在非法复用场景命中新生命周期；Motion 还可能在 map miss 后再次 recycle 并抛异常。

所以“调用过 finish”不等于“正确的 DispatchEntry 已完成”。后续三节将分别建立对象、编号、队列和 Java 生命周期账本。

## 2. 对象账本与三套编号

### 跨进程的六层对象

一次输入不是同一个对象从 InputReader 一直走到 View：

| 层次 | 对象 | 所在侧 | 职责 |
|---|---|---|---|
| 事件 | `EventEntry` / `KeyEntry` / `MotionEntry` / `FocusEntry` | system_server | 描述事件本身，可被多个目标引用 |
| 投递 | `DispatchEntry` | system_server | 描述发给一个 connection 的一次交付 |
| 线协议 | `InputMessage` | channel | 携带 transport seq、eventId 与事件字段 |
| native 消费 | native `KeyEvent` / `MotionEvent` / `FocusEvent` | App native | `InputConsumer.consume()` 的产物 |
| Java 事件 | `android.view.KeyEvent` / `MotionEvent` | App Java | 交给 receiver 与 View 层 |
| Java 包装 | `QueuedInputEvent` | App Java | 保存 event、receiver、flags、链表指针 |

一个 `EventEntry` 可产生多个 `DispatchEntry`。native Key/Motion 会转换或复制成新的 Java 对象；Focus 则不创建 Java `InputEvent`。`QueuedInputEvent` 没有独立 transport seq 字段。

### EventEntry 与 DispatchEntry 不是一对一

`EventEntry` 有事件层的 `id`、type、eventTime、policyFlags。`DispatchEntry` 则持有自己的 `seq`、EventEntry 引用、target flags、坐标变换、resolved id/action/flags，以及发送后才有意义的 delivery/timeout 时间。

同一 Motion 可投递给主窗口、wallpaper、monitor，或因不同 dispatch mode 转成不同动作。更精确的创建单位是“connection × 实际命中的 dispatch mode”：一个合并了多种 mode flag 的 `InputTarget` 也可能生成多份 `DispatchEntry`；每一份都有独立 transport seq，并分别等待 FINISHED。

源码锚点：`Entry.h:56-63,190-224`、`InputDispatcher.cpp:2265-2295`。

### 三套编号必须分开

| 编号 | 生成位置 | 用途 | 边界 |
|---|---|---|---|
| eventId | `EventEntry.id`，InputReader 或 Dispatcher `IdGenerator` | 事件身份、验证、trace | 作为消息字段跨进程 |
| transport seq | `DispatchEntry.seq`，Dispatcher 原子递增 | 某 connection 上投递与 FINISHED 的匹配 | uint32，非 0 |
| Java `mSeq` | `InputEvent.mNextSeq.getAndIncrement()` | Java 对象当前使用周期的 map key | 进程本地，可为 0/负数 |

三者即使偶然数值相同，也不能互换。`event.getId()`、`event.getSequenceNumber()` 和 JNI 回调参数 `seq` 分别来自不同账本。

### eventId 属于事件语义

Motion 以 AS_IS 发送时，`DispatchEntry.resolvedEventId` 通常沿用 `MotionEntry.id`。显式按 OUTSIDE、HOVER_ENTER、HOVER_EXIT、SLIPPERY_ENTER 或 SLIPPERY_EXIT mode 转义时，Dispatcher 会为转义后的事件生成新 resolved event id。一个细节例外是：AS_IS 的 `HOVER_MOVE` 因 connection 尚未处于 hover 状态而被补成 `HOVER_ENTER` 时，前面已经沿用了原 id，因此这次动作变化不会另换 id。

eventId 是否沿用不影响完成匹配；每个 `DispatchEntry` 仍靠自己的 transport seq 等待确认。

源码锚点：`InputDispatcher.cpp:2314-2400`、`InputTransport.cpp:440-553`。

### transport seq 属于投递

`DispatchEntry::nextSeq()` 使用原子计数生成 `uint32_t` 并显式跳过 0：

```cpp
do {
    seq = android_atomic_inc(&sNextSeqAtomic);
} while (!seq);
```

Key/Motion publisher 也拒绝 seq 0，`InputConsumer.sendFinishedSignal()` 同样拒绝用 0 发回执。这一非 0 约束只属于 transport seq。

源码锚点：`Entry.cpp:241-270`、`InputTransport.cpp:459-462,511-514,1074-1077`。

### Java mSeq 可以为 0 或负数

Java 侧是独立计数器：

```java
private static final AtomicInteger mNextSeq = new AtomicInteger();

InputEvent() {
    mSeq = mNextSeq.getAndIncrement();
}
```

`AtomicInteger` 默认从 0 开始，所以 `mSeq==0` 合法；越过 `Integer.MAX_VALUE` 后还会显示为负数。代码没有跳过这些值，因为 `mSeq` 是本进程 Java map key，不直接写入 `FINISHED`。

源码锚点：`InputEvent.java:34-48,159-163`。

### uint32 transport seq 经 jint 也可能为负

native `consumeEvents()` 使用 `uint32_t seq`，Java 回调却是 `dispatchInputEvent(int, InputEvent)`；`mSeqMap` value 与 `nativeFinishInputEvent` 参数也是 `int/jint`。

当 uint32 seq 的最高位为 1 时，Java 日志可能按有符号 int 显示负数；回到 C++ `finishInputEvent(uint32_t, ...)` 后，32 位值重新按 uint32 解释。这不是 Java `mSeq`，也不违反 transport seq 非 0 约束。

源码锚点：`android_view_InputEventReceiver.cpp:64,240,336-337,393-397`、`InputEventReceiver.java:48,53,175-177`。

### mSeq 标识对象的使用周期

新对象构造时取得 `mSeq`。`recycle()` 只结束当前生命周期并把可池化对象放回池；真正的新编号在下次 `obtain()` 调用 `prepareForReuse()` 时生成：

```text
构造                         -> 分配 mSeq
recycle                      -> mSeq 暂未改变，但对象已禁止使用
再次 obtain/prepareForReuse  -> 分配新 mSeq
```

源码锚点：`InputEvent.java:126-162`、`KeyEvent.java:1595-1607`、`MotionEvent.java:1652-1665`。

### copy 不继承 Java mSeq

`KeyEvent.copy()` 与 `MotionEvent.copy()` 都 obtain 一个新 Java 对象，因此得到新 `mSeq`。内容或 eventId 是否复制，与 Java 生命周期编号是否复制是两件事。

所以内容完全相同的 copy 通常不能 finish 原投递：receiver 登记的是入站对象的 `mSeq`，不是 copy 的新值。

源码锚点：`KeyEvent.java:1669-1692`、`MotionEvent.java:2002-2007`。

### 用具体数字核账

假设同一 `MotionEntry.id=0x41000021` 发给两个 connection：

```text
主窗口 DispatchEntry.seq = 300
wallpaper DispatchEntry.seq = 301
```

主窗口进程创建的 Java MotionEvent，其 `mSeq` 可以恰好为 0：

```text
mainReceiver.mSeqMap[0] = 300
```

wallpaper 在另一个进程有独立 Java 计数器，`mSeq` 可能为 928：

```text
wallpaperReceiver.mSeqMap[928] = 301
```

eventId 可以相同，两个 transport seq 必须分别确认；两个 Java mSeq 没有跨进程可比性。

## 3. DispatchEntry 从 outbound 到 wait

### 创建后先进入 outboundQueue

Dispatcher 选出 `InputTarget` 后，为匹配的 dispatch mode 创建 `DispatchEntry`。它解析 action、eventId、flags，更新该 connection 的 `InputState`，foreground target 还会增加注入等待计数，随后才执行：

```cpp
connection->outboundQueue.push_back(dispatchEntry.release());
```

此时含义是“准备向 connection 发布”，不是“App 已收到”。源码锚点：`InputDispatcher.cpp:2298-2425`。

### 发送尝试与 timeout

`startDispatchCycleLocked()` 取 `outboundQueue.front()`，先设置：

```text
deliveryTime = currentTime
timeoutTime  = currentTime + dispatchingTimeout
```

随后按类型调用 `publishKeyEvent()`、`publishMotionEvent()` 或 `publishFocusEvent()`。`Entry.h` 把 delivery/timeout 定义为发送后才有意义；实现确实会在 publish 尝试前先写字段，所以一次 `WOULD_BLOCK` 后留在 outbound 的 entry 可能已有数值，但下一次尝试还会覆盖它们。不要拿这组尚未成功进入 wait 的值推断真实处理时长。

源码锚点：`Entry.h:201-205`、`InputDispatcher.cpp:2456-2572`。

### publish OK 才迁移

只有 publish 返回 OK，Dispatcher 才：

1. 从 `outboundQueue` 删除该指针；
2. 把同一个 `DispatchEntry` 追加到 `waitQueue`；
3. connection 仍 responsive 时，把 timeout 与 token 插入 `mAnrTracker`。

```text
outbound --publish OK--> wait + ANR tracking
```

这里不是创建第二份 DispatchEntry，也不是把 EventEntry 本身搬进 wait。源码锚点：`InputDispatcher.cpp:2602-2612`。

### publish 失败不会迁移到 wait

publish 返回错误时，代码在队列迁移前 return，因此 entry 不会因“尝试发送”就进入 wait，也不会插入该次 ANR deadline。

WOULD_BLOCK 分两种情况：

- `waitQueue` 非空：已有已发未确认事件，当前 entry 留在 outbound，等待后续重试机会；这不证明旧 packet 尚未被 App native 从 socket 读走；
- `waitQueue` 为空：源码认为 channel 满与状态矛盾，进入 broken dispatch cycle。

非 WOULD_BLOCK 硬错误也进入 broken 路径。源码锚点：`InputDispatcher.cpp:2574-2600,2655-2678`。

### outbound 与 wait 可同时非空

一个 connection 可以同时是：

```text
waitQueue     = 已成功发送、等待 FINISHED 的旧 entry
outboundQueue = 因背压尚未发送的新 entry
```

这不是重复入队，而是发布阶段与确认阶段的速度不同。诊断时先确定 seq 位于哪个队列，才能区分“等待写入”和“等待回执”。

### 一个 connection 可有多个在途事件

publish 成功后，`startDispatchCycleLocked()` 的 while 不等待刚才的 FINISHED，而会继续尝试下一条 outbound，因此 `waitQueue` 可以包含多个 seq。App native receiver 也循环 consume。

所以 `InputEventReceiver` 注释中的“finish 前不会收到新事件”不能当成 r48 严格约束。正常协议要求每个已经 publish 的 seq 各自结算；它并不要求窗口内永远只有一个未完成事件。异常 direct-send、断链与 drain 的边界见 §8、§13。

源码锚点：`InputDispatcher.cpp:2467-2613`、`android_view_InputEventReceiver.cpp:237-353`。

### waitQueue 按 seq 查找

App 的 `FINISHED` 带回 transport seq；`Connection::findWaitQueueEntry(seq)` 线性扫描整个 deque，而非只比较 `front()`。不同设备、异步阶段或 native 反向写背压造成的乱序确认仍可匹配。

找不到 seq 时不会猜测队首，也不会删除其他 entry。源码锚点：`Connection.cpp:56-62`、`InputDispatcher.cpp:4751-4763`。

### 删除不是简单 pop_front

首次按 seq 找到 entry 后，Dispatcher 统计 delivery 到 finish 的时长，并执行 Key/Motion 完成后 policy。policy 可能暂时释放锁，因此回来后会再次按 seq 查找，确认队列未被 drain，才执行：

```text
waitQueue.erase(entry)
mAnrTracker.erase(timeout, token)
release，或按 Key policy restart
```

最后重新启动 dispatch cycle。源码锚点：`InputDispatcher.cpp:4751-4806`。

### broken 清队列不等于 handled

正向 publish 直接遇到不可恢复错误并调用 abort 时，Dispatcher drain outbound 与 wait，connection 停在 BROKEN。反向 receive/HUP 或显式 unregister 则先 abort/drain，最终把 connection 标成 ZOMBIE。release foreground `DispatchEntry` 还会减少同步注入 pending 计数，但这只表示投递关系终止，不代表 App 回了 handled=true/false。

观察 waitQueue 下降时，要区分正常 FINISHED 出队和 connection teardown。

### 两个 seq 的状态推演

```text
初始：      outbound=[40,41]  wait=[]
40 发布成功：outbound=[41]     wait=[40]
41 WOULD_BLOCK：outbound=[41]  wait=[40]
收到 FINISHED(40,false)：删除 40，重启 cycle
41 发布成功：outbound=[]       wait=[41]
```

handled=false 没有阻止 seq 40 完成；驱动 wait 出队的是带回正确 transport seq 的 FINISHED。

## 4. Java mSeqMap、finish 与对象复用

### JNI 创建新的 Java 生命周期

App native `InputConsumer.consume()` 返回 `uint32_t seq` 和 native `InputEvent*`。Key 经 `KeyEvent.obtain(...)` 转成 Java；Motion 经 `MotionEvent.obtain()` 后复制 native 内容。

Java 对象在构造或从池中复用时取得自己的 `mSeq`；transport seq 没有写入 `InputEvent.mSeq`。

源码锚点：`android_view_InputEventReceiver.cpp:240-244,292-311`、`android_view_KeyEvent.cpp:96-107`、`android_view_MotionEvent.cpp:81-98`。

### 先登记，后回调

JNI 调用的私有 Java 入口是：

```java
private void dispatchInputEvent(int seq, InputEvent event) {
    mSeqMap.put(event.getSequenceNumber(), seq);
    onInputEvent(event);
}
```

先 put 使默认或自定义 `onInputEvent()` 能在回调栈内同步 finish。基类默认实现会立刻 `finishInputEvent(event, false)`；覆写者若改成异步持有，就承担所有终止分支最终完成的责任。map 属于 receiver 实例；Java `mSeq` 计数器则由进程内所有 `InputEvent` 共享。

源码锚点：`InputEventReceiver.java:47-48,218-220`。

### 正常 finish 的严格顺序

`finishInputEvent(event, handled)` 正常执行：

1. 检查 event 非 null；
2. 检查 native receiver pointer 尚未清零；
3. 用 `event.getSequenceNumber()` 查 map；
4. 读出 transport seq；
5. 先删除 map 项；
6. 调用 `nativeFinishInputEvent()`；
7. native 正常返回后调用 `recycleIfNeededAfterDispatch()`。

“先删 map、后 native、最后 recycle”决定了后面的失败边界。源码锚点：`InputEventReceiver.java:163-180`。

### mSeqMap 不检查 identity

`SparseIntArray` 只以当前 `mSeq` 为 key。finish 不验证是否为原 Java 引用、类型是否相同、eventId/内容是否相同，也不验证调用代码身份。

普通 copy 因取得新 `mSeq` 而查不到；但“必须使用原对象”是所有权规则，不是框架的 identity check。只要某对象暴露出相同 key，receiver 就会把它当成对应在途事件。

### map miss 仍可能回收对象

查不到 key 时只打印 `not in progress`，不会调用 native，也不会发 FINISHED；但方法不会提前 return，仍执行 `event.recycleIfNeededAfterDispatch()`。

- KeyEvent 覆写该方法为空操作，通常只是 warning；
- MotionEvent 会 recycle；
- 已 recycle 且尚未复用的 MotionEvent 会在再次 recycle 时抛 double-recycle。

源码锚点：`InputEventReceiver.java:171-180`、`InputEvent.java:126-152`、`KeyEvent.java:1716-1720`、`MotionEvent.java:2009-2023`。

### recycle 与 reuse 是两个时刻

第一次正确 finish MotionEvent 后，对象通常进入 recycler。旧持有者非法再次使用引用时存在两个窗口：

```text
尚未再次 obtain：mSeq 仍为旧值；map miss 后 double recycle 抛异常
已经再次 obtain：同一对象已 prepareForReuse；mSeq 已换成新值
```

后一种情况下旧引用实际别名到新生命周期。若新 `mSeq` 恰好登记在同一 receiver，旧代码所谓“第二次 finish”可能提前完成新事件。这是破坏“回收后禁止再碰对象”规则的后果。

### 错误 receiver 也会破坏生命周期

每个 receiver 有独立 `mSeqMap`。把 receiver A 收到的 MotionEvent 交给 B finish，B 通常查不到，不会发送 A 的 transport seq，却仍会 recycle 该对象。

A 随后正确 finish 时，可能先发出 FINISHED，再在第二次 recycle 抛异常；若对象已复用，结果还取决于新的 `mSeq`。receiver 不匹配不只是“回执丢失”，还会污染对象所有权。

### map 删除与 native 结果不是事务

map 命中后，Java 会先删除映射，再进入 native，最后才条件回收事件。native 的 OK、WOULD_BLOCK 排队、硬错误、direct-send 和 dispose 状态机集中放到 §7—§8；在 Java 账本这里只需记住两个不可回滚后果：

- 非 DEAD_OBJECT 的 native 硬错误抛回 Java 时，映射已经删除，后续 recycle 尚未执行，正常 Java 路径也无法用原映射重试；
- batched terminal seq 可能已有祖先 FINISHED packet 成功写出，只是逻辑 finish 没有完整成功；失败点及后缀也未必获得正常重试。

Java callback 在自行 finish 后再抛异常，还可能触发一次绕过 map 的 direct false 回执；receiver dispose 则不会遍历 map 或补发完成。二者的精确传输后果见 §8 的异常与 dispose 小节。源码锚点：`InputEventReceiver.java:163-180`、`android_view_InputEventReceiver.cpp:332-352,386-402`、`InputTransport.cpp:1094-1114`。

### 最后的双账本

| 场景 | map | FINISHED | Motion 回收 |
|---|---|---|---|
| map 命中、native OK | 先删除 | 立即尝试成功 | 正常 recycle |
| native WOULD_BLOCK | 先删除 | native 排队重试 | Java 返回后 recycle |
| map miss | 不变 | 不发送 | 仍 recycle，可能 double-recycle |
| receiver 已 dispose | 不变 | 不发送 | 仍 recycle |
| native 硬错误并抛出 | 已删除 | 逻辑 finish 未完整；batch 前缀可能已发 | 后续 recycle 不执行 |
| onInputEvent 抛出且未 finish | 可能残留 | native best-effort false | 不走正常 finish 回收 |

完成协议始终有两个相关但不同的账本：

```text
Dispatcher 账本：transport seq 是否收到 FINISHED
Java 账本：对象生命周期是否仍登记、是否已经回收
```

只有沿正确 receiver、正确 Java 生命周期和正确 transport seq 走完链路，两个账本才同时闭合。

---

## 5. ViewRootImpl：Java 处理链何时才真正 finish

Dispatcher 把消息交给 App 后，Java 并不是在一次方法调用里必然同步处理完。对已经进入普通 Key/Motion `QueuedInputEvent` pipeline 的事件，`ViewRootImpl` 先包装，再让它穿过一组 `InputStage`，走到最终收口才调用 `InputEventReceiver.finishInputEvent()`。compat 空输出会在入 stage 前直接发起 finish，Focus 更绕过普通 `InputEvent`/stage 路径。源码入口主要在：

- `frameworks/base/core/java/android/view/ViewRootImpl.java:1145-1160`
- `frameworks/base/core/java/android/view/ViewRootImpl.java:5287-5589`
- `frameworks/base/core/java/android/view/ViewRootImpl.java:7942-8122`

### stage 链不是一个单独的 View 回调

窗口建立输入管线时，r48 按下面的先后关系连接各 stage：

```text
NativePreImeInputStage
        ↓
ViewPreImeInputStage
        ↓
ImeInputStage
        ↓
EarlyPostImeInputStage
        ↓
NativePostImeInputStage
        ↓
ViewPostImeInputStage
        ↓
SyntheticInputStage
```

`mFirstInputStage` 指向 `NativePreImeInputStage`。 `mFirstPostImeInputStage` 指向 `EarlyPostImeInputStage`。 事件从哪里开始，取决于 `QueuedInputEvent` 的 flags 和事件来源。 已经标记 `FLAG_UNHANDLED`、需要交给 synthesizer 的事件，会直接从 `mSyntheticInputStage` 开始。 带 `FLAG_DELIVER_POST_IME` 的事件跳过 pre-IME 与 IME。

pointer class 或 rotary encoder 的 `MotionEvent` 也由 `shouldSkipIme()` 选择 post-IME 起点。 其他事件从完整链的首节点进入。 因此“交给 View”只是中间环节，不等于已经向 Dispatcher 回了 FINISHED。

### QueuedInputEvent 保存的是 Java 处理状态

`QueuedInputEvent` 的关键字段是：

```text
mEvent       当前 Java InputEvent
mReceiver    最终应向哪个 InputEventReceiver 完成
mFlags       Java pipeline 的处理状态
mNext        App 侧队列链接
```

与本章相关的 flags 包括：

```text
FLAG_DEFERRED
FLAG_FINISHED
FLAG_FINISHED_HANDLED
FLAG_MODIFIED_FOR_COMPATIBILITY
```

这些 flag 不会直接写进 InputChannel。 它们只是帮助 `ViewRootImpl` 决定何时、用哪个对象、以哪个 handled 值调用 receiver。 `mReceiver == null` 表示这是没有 native receiver 回执对象的本地事件。 这类事件走到收口时只执行 `recycleIfNeededAfterDispatch()`，不会发送 FINISHED。

### 当前 ViewRoot 的入口 pending 队列与 Dispatcher waitQueue 不是同一队列

`enqueueInputEvent()` 总是按到达顺序把包装对象追加到：

```text
mPendingInputEventHead ... mPendingInputEventTail
```

`processImmediately == true` 时，它立即调用 `doProcessInputEvents()`。 否则通过一个 asynchronous Handler message 安排稍后处理。 `doProcessInputEvents()` 先把头项从当前 ViewRoot 的 pending 队列摘掉，再调用 `deliverInputEvent(q)`。 所以事件进入某个异步 stage 的等待队列后，已经不在 `mPendingInputEventHead` 这条链上。

与此同时，system_server 中对应的 `DispatchEntry` 仍然留在 connection 的 waitQueue。观察这个 ViewRoot 的 pending 队列为空，不能推出 Dispatcher 已收到 FINISHED。

### InputStage 的三种同步结果

普通 `InputStage` 定义三个结果：

```text
FORWARD             交给下一个 stage
FINISH_HANDLED      标为完成且 handled
FINISH_NOT_HANDLED  标为完成但 unhandled
```

`deliver(q)` 首先检查 `FLAG_FINISHED`。 若前一 stage 已经把事件标成 finished，后续 stage 不再执行 `onProcess()`，只继续向链尾转发。 若 `shouldDropInputEvent(q)` 返回 true，当前 stage 调用 `finish(q, false)`。 否则才执行当前 stage 的 `onProcess(q)` 并应用返回值。 `finish(q, handled)` 只做 Java 状态转换：

```text
设置 FLAG_FINISHED
handled=true 时再设置 FLAG_FINISHED_HANDLED
继续 forward 到链尾
```

它不是 native FINISHED 的发送函数。 当 `onDeliverToNext()` 发现 `mNext == null` 时，才进入 `ViewRootImpl.finishInputEvent(q)`。

### drop 通常仍要完成，但 terminal 例外要看命中哪道条件

root view 已移除、窗口停止或不满足焦点条件时，stage 可能决定 drop。第一道 `mView == null || !mAdded` 会直接返回 true，terminal 事件也走 `finish(q, false)`；只有后面的无焦点、stopped、ambient 或 transition 条件命中时，Key UP、Motion UP/CANCEL/HOVER_EXIT 等 terminal 才先被标 canceled 并继续经过处理链，非 terminal 才 drop。

对普通 receiver-backed q，drop 最终仍会发起 handled=false 的 finish；modified compat q 还要服从 `processInputEventBeforeFinish()`，它若返回 null，就不会在这一 q 上调用 receiver。这里的“drop”始终是 Java pipeline 处置，不是 Dispatcher 侧 drain connection。

### DEFER 是异步 stage 的第四种结果

`DEFER` 只由 `AsyncInputStage` 解释。 普通 `InputStage.apply()` 不认识 `DEFER`；把其他整数交给它会抛 `IllegalArgumentException`。 r48 中会返回 `DEFER` 的典型节点有：

- `NativePreImeInputStage`：把 Key 交给 native `InputQueue` 后等待 callback。
- `ImeInputStage`：IME 返回 `DISPATCH_IN_PROGRESS` 时等待 callback。
- `NativePostImeInputStage`：把事件交给 native `InputQueue` 后等待 callback。

`defer(q)` 设置 `FLAG_DEFERRED`，并把事件放进该异步 stage 自己的队列。 异步完成回调必须在稍后调用 `finish(q, handled)` 或 `forward(q)`。 在此之前不能调用最终 receiver finish，因为异步参与者仍可能使用事件对象。

还要区分异常发生的调用栈：同步 `onInputEvent()` 抛异常时仍回到 JNI 的 `skipCallbacks` 止损分支；已经 DEFER 后，稍后的 IME/InputQueue callback 若在 Java 层抛异常，通常已不在原 `consumeEvents()` 调用栈上，连这层 direct false-send 兜底也没有。异步实现必须自己保证成功、拒绝、取消和异常分支都能收束 q。

### AsyncInputStage 允许异步完成，但维护同设备顺序

stage 的异步操作可以乱序返回。 `AsyncInputStage.forward()` 会先清掉当前事件的 `FLAG_DEFERRED`。 然后它检查队列中是否有更早、且 `deviceId` 相同的事件。 若有，当前事件即使异步工作已经完成，也暂时不能进入下一个 stage。 它会留在 stage 队列中，等更早的同设备事件先向前推进。 若较早事件属于别的 device，当前事件不因此被阻塞。 所以这里维护的是同一设备的串行边界，不是全局严格串行。 较早事件出队后，代码会继续释放已经完成、且被它阻塞的同设备后继。 遇到仍带 `FLAG_DEFERRED` 的后继时才停止。

### DEFER 不暂停 Dispatcher 的超时钟

DEFER 只改变 App 进程内的队列状态。 它不会向 InputDispatcher 发送“暂停计时”消息。 也不会刷新该 `DispatchEntry` 的 `deliveryTime` 或 `timeoutTime`。 因此耗时可概括为：

```text
channel 到达
+ 当前 ViewRoot pending 排队
+ InputStage 同步处理
+ IME/InputQueue 异步等待
+ 最终 finish 回程
```

这些时间都可能落在 Dispatcher 观察到的完成时长内。 如果异步 callback 永远不回来，对应 waitQueue 项就不会因为“处于 DEFER”而获豁免。

### 最终 handled 从哪里取得

链尾的 `ViewRootImpl.finishInputEvent(q)` 读取：

```java
boolean handled =
        (q.mFlags & QueuedInputEvent.FLAG_FINISHED_HANDLED) != 0;
```

如果前面没有 stage 设置 `FLAG_FINISHED_HANDLED`，最终 handled 就是 false。 若 `q.mReceiver != null`，它把事件和 handled 交回 `InputEventReceiver`。 若 receiver 为空，它调用本地事件的 `recycleIfNeededAfterDispatch()`：Motion 会回收，Key 的覆写是 no-op。 因此 Java stage 的 `finish()`、`ViewRootImpl.finishInputEvent()` 与 native FINISHED 是三个不同层次。 只有最后一层成功写回 socket，system_server 才能看到完成确认。

## 6. compatibility processor：返回值就是完成协议的一部分

窗口接收器不是直接把每个 native 事件塞进 InputStage。 `WindowInputEventReceiver.onInputEvent()` 先调用 `InputEventCompatProcessor`。 源码锚点是：

- `frameworks/base/core/java/android/view/InputEventCompatProcessor.java:31-80`
- `frameworks/base/core/java/android/view/ViewRootImpl.java:8182-8211`
- `frameworks/base/core/java/android/view/ViewRootImpl.java:8095-8122`
- `frameworks/base/core/java/android/view/InputEventReceiver.java:163-180`

### 原始 Java sequence 已先登记

native 调用 Java 私有方法 `dispatchInputEvent(int seq, InputEvent event)` 时，Java 先执行：

```java
mSeqMap.put(event.getSequenceNumber(), seq);
onInputEvent(event);
```

因此 compat processor 开始工作时，`mSeqMap` 登记的是原始 Java 对象的 sequence。 普通 `MotionEvent.obtain(other)` 或 `KeyEvent.obtain(other)` 会得到新的对象 sequence。 “内容是原事件的副本”不等于“能命中原始 mSeqMap”。 最终交给 `InputEventReceiver.finishInputEvent()` 的对象必须命中这个 map，才会发 native FINISHED。

### processInputEventForCompatibility 的三值契约

`processInputEventForCompatibility(event)` 返回 `List<InputEvent>` 或 null。 `WindowInputEventReceiver` 对返回值作精确区分：

| 返回值 | ViewRootImpl 行为 | native 完成状态 |
|---|---|---|
| `null` | 原事件按未修改路径入队 | 以后直接用原事件 finish |
| 空 list | 不入 stage，立即调用 `finishInputEvent(original, true)` | 立即发起 handled=true 的完成；反向发送仍可能排队或失败 |
| 非空 list | 每个输出带 `FLAG_MODIFIED_FOR_COMPATIBILITY` 入队 | 以后由 before-finish 决定是否、用哪个对象完成 |

空 list 不是“暂时没有输出”。 它在 r48 中明确表示 compat processor 已经消费原事件。 ViewRoot 随即把原事件按 handled=true 完成。

### 默认 processor 通常不制造新的对象身份

默认实现只为 target SDK 低于 Android M 的 `MotionEvent` 做 stylus button 兼容。 它计算 primary/secondary stylus button 对应的兼容位，并 OR 回原 `MotionEvent`。 随后返回一个只包含该原对象的复用 list。 对于其他事件或较新的 target SDK，它返回 null。 所以默认路径仍然保持一个原始 Java sequence 对一个 channel seq。 接口允许 override 产生多个事件，但框架不会自动替 override 建立多个 mSeqMap 映射。

### modified 事件完成前还要反向归并

带 `FLAG_MODIFIED_FOR_COMPATIBILITY` 的 `QueuedInputEvent` 到达链尾后，不会直接 finish `q.mEvent`。 它先调用：

```java
processedEvent =
        mInputCompatProcessor.processInputEventBeforeFinish(q.mEvent);
```

只有 `processedEvent != null` 时，ViewRoot 才调用：

```java
q.mReceiver.finishInputEvent(processedEvent, handled);
```

若返回 null，这一个 processed output 不产生 receiver finish。代码也不会自动退回去 finish `q.mEvent`。这使 override 可以选择哪一个兼容输出承担原登记项的回执，但 API 没有 handled 参数，也不会替多个输出聚合 handled；真正发回的值来自那个 q 自己的 `FLAG_FINISHED_HANDLED`。对象配对、回执选择和“最终恰好命中一次”的责任都交给了 processor。

### 多输出 processor 必须维护的三个不变量

一次 `dispatchInputEvent()` 只在 `mSeqMap` 登记一个 terminal transport seq。即使 compat processor 产生 N 个 Java 输出，正确实现仍应满足：

1. 在合适的时刻返回能命中原 `mSeqMap` 的对象。
2. 对这一个 map 项最终只触发一次有效 receiver finish；若 terminal seq 还连着 batch `mSeqChains`，native 再把它展开为多条底层 FINISHED。
3. 对暂不 finish 的输出，自己正确管理对象与聚合状态。

中间输出可以在 `processInputEventBeforeFinish()` 返回 null。最终输出通常应返回最初登记过的原对象。若多个输出都返回同一原对象，第一次有效 finish 会删 map 并回收 Motion；后续 Motion 调用不仅 map miss，还可能因再次 recycle 抛 double-recycle，只有 Key 通常退化成 warning。若返回普通 copy，它的新 sequence 通常不在 map 中，也不会发 FINISHED；若所有输出都返回 null，又没有其他地方完成原事件，对应 Dispatcher wait 债务会一直保留。

### before-finish 返回 null 不等于框架代为 recycle

modified 分支在 `processedEvent == null` 时不会调用 receiver。 它也不会进入 `mReceiver == null` 分支去回收 `q.mEvent`。 随后只回收 `QueuedInputEvent` 包装对象，并把其中的 event/receiver 字段清空。 因此自定义 processor 不能把 null 当成无代价的“忽略”。 它需要为被聚合或吞掉的兼容对象定义清楚的所有权与回收策略。

### compat 没有第二套 sequence 防线

compat 输出最终仍回到 §4 的同一个 `InputEventReceiver.finishInputEvent()`：只按当前 Java sequence 查 `mSeqMap`，map miss 仍可能回收 Motion，map 命中仍是先删映射再进 native。processor 没有额外的 P→O 映射或重复完成保护。

### compat 路径的审计问题

阅读自定义 processor 时，应逐项回答：

- null、空 list、非空 list 分别在哪些条件返回？
- 非空 list 中是否包含原对象，还是创建了 copy？
- 每个 modified output 的 before-finish 返回什么？
- 哪一次返回真正登记过的原对象？
- 是否可能所有输出都返回 null？
- 是否可能重复返回同一个原对象？
- 被聚合的临时事件由谁 recycle？

判断标准不是“业务上处理完了几次”，而是原始 channel seq 是否恰好得到一次有效完成。

## 7. App native 回程：FINISHED、mFinishQueue 与 batch seq chain

Java 用原对象 sequence 找回 channel seq 后，进入 `nativeFinishInputEvent()`。 正常路径依次经过：

```text
InputEventReceiver.nativeFinishInputEvent
        ↓
NativeInputEventReceiver::finishInputEvent
        ↓
InputConsumer::sendFinishedSignal
        ↓
InputChannel::sendMessage(FINISHED)
```

核心源码是：

- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp:124-215`
- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp:393-403`
- `frameworks/native/libs/input/InputTransport.cpp:305-380`
- `frameworks/native/libs/input/InputTransport.cpp:788-812`
- `frameworks/native/libs/input/InputTransport.cpp:1068-1123`

### 一个 FINISHED packet 携带什么

`sendUnchainedFinishedSignal(seq, handled)` 构造：

```text
header.type = InputMessage::Type::FINISHED
body.finished.seq = channel seq
body.finished.handled = 0 或 1
```

该消息沿同一 `AF_UNIX/SOCK_SEQPACKET` channel 的反方向发回 publisher。 `seq == 0` 会被 `InputConsumer::sendFinishedSignal()` 拒绝并返回 `BAD_VALUE`。 这里发送的是 channel seq，不是 eventId，也不是 Java `InputEvent.mSeq`。

### sendMessage 的状态边界

channel fd 是 non-blocking。 `InputChannel::sendMessage()` 使用 `MSG_DONTWAIT | MSG_NOSIGNAL`。 它对 `EINTR` 重试当前 send。 其他结果映射如下：

| send 结果 | status |
|---|---|
| 完整 packet 写入 | `OK` |
| `EAGAIN` / `EWOULDBLOCK` | `WOULD_BLOCK` |
| `EPIPE` / `ENOTCONN` / `ECONNREFUSED` / `ECONNRESET` | `DEAD_OBJECT` |
| short write | `DEAD_OBJECT` |
| 其他 errno | `-errno` |

SOCK_SEQPACKET 的完成单位是 packet。 代码不会保存或补发某个 packet 的“剩余字节”。 后面所说的 partial send，是多个完整 FINISHED packet 中只成功了一部分。

### 正常 WOULD_BLOCK 怎样进入 mFinishQueue

`NativeInputEventReceiver::finishInputEvent()` 先直接调用 `sendFinishedSignal()`。 若返回 OK，当前完成已经交给 socket。 若返回 WOULD_BLOCK，它把逻辑完成记录追加到：

```text
mFinishQueue += Finish{seq, handled}
```

当 queue 从空变成一个元素时，fd 监听由：

```text
ALOOPER_EVENT_INPUT
```

扩展成：

```text
ALOOPER_EVENT_INPUT | ALOOPER_EVENT_OUTPUT
```

然后该函数把 WOULD_BLOCK 转成 OK 返回 Java。在 `OK`/`WOULD_BLOCK` 正常分支内，Java 正常返回分别表示已写入 socket，或已由这个 native receiver 接管重试；`DEAD_OBJECT` 也会被 JNI 抑制，但它不属于这两种成功结局，见 §8 的普通 finish 错误矩阵。无论哪种正常返回，都不能证明 Dispatcher 已经读到 FINISHED。

### mFinishQueue 保存逻辑完成，不保存 packet 字节

queue 元素只有 `{seq, handled}`。 OUTPUT callback 会重新调用完整的 `InputConsumer::sendFinishedSignal(seq, handled)`。 这点对 batch 很重要。 一次逻辑 finish 可能需要发送多个 FINISHED packet。 如果初次只发送了其中一部分，未发送部分由 `mSeqChains` 描述，而 queue 只需记住最终 seq。

### OUTPUT callback 的正常重试

fd 可写时，`handleEvent()` 按 `mFinishQueue` 的 Vector 顺序遍历。 每一项都重新调用 `sendFinishedSignal()`。 若当前项成功，循环继续处理下一项。 若当前项再次 WOULD_BLOCK，代码执行：

```text
删除索引 [0, i) 的完整成功 queue 项
保留索引 i 的当前失败项及其后缀
return 1，继续保留 Looper callback
```

若全部成功，代码 clear queue，把监听恢复成仅 `ALOOPER_EVENT_INPUT`，并返回 1。 这里的“成功前缀”有两层：

- mFinishQueue 层：失败记录之前已经完成的逻辑 Finish 项。
- mSeqChains 层：当前 batch Finish 内已经成功写出的底层 seq。

两层分别维护，不能混为同一个数组。

### queue 顺序不等于所有 FINISHED 的全局顺序

即使 `mFinishQueue` 已经非空，新的 Java finish 仍先尝试直接 send。 代码没有“queue 非空就无条件追加”的判断。 如果 system_server 恰好并发腾出了 socket 空间，较新的 finish 可能直接成功。 它可能越过仍留在 `mFinishQueue` 的较早 finish。 这也是 Dispatcher 必须按 seq 搜 waitQueue，而不能假定只完成队首的原因之一。

### batch 怎样建立 mSeqChains

`InputConsumer::consumeSamples()` 把多个 Motion sample 合成一个 Java `MotionEvent`。 假设底层消息 seq 是：

```text
20, 21, 22
```

第一个 sample 初始化 `MotionEvent`。 每追加一个 sample，代码记录“当前 seq 指向前一 seq”：

```text
21 -> 20
22 -> 21
```

最终返回给 JNI 的 `outSeq` 是 22。 Java `mSeqMap` 因而只需要把这个合并后 Java 对象映射到 channel seq 22。 前面的 20、21 由 native `mSeqChains` 保留。

### sendFinishedSignal 怎样展开 chain

finish 22 时，`sendFinishedSignal()` 从 Vector 尾部反向追链。 它取出 22→21，再继续取出 21→20。 随后按最老到最新的顺序发送：

```text
FINISHED(20, handled)
FINISHED(21, handled)
FINISHED(22, handled)
```

同一个 handled 参数用于所有底层 seq。 正常成功后，一个 Java batch finish 可以完成多个 Dispatcher waitQueue 项。

### batch partial-send 的重建算法

继续使用 20、21、22 的例子。 若 20 成功，而 21 返回 WOULD_BLOCK：

```text
20 已完成，不应重发
重建 22 -> 21
sendFinishedSignal(22) 返回 WOULD_BLOCK
NativeInputEventReceiver 入队 Finish{22, handled}
```

OUTPUT 重试 22 时，只发送 21、22。 若第一条 20 就失败，则重建完整的：

```text
21 -> 20
22 -> 21
```

重试仍发送 20、21、22。仍以 WOULD_BLOCK 为例，若 20、21 都成功，只有最终 22 失败，则不需要重建 chain；queue 中的 `{22, handled}` 足以让 OUTPUT 重试只发送 22。若这里是硬错误，调用者不会把它放进正常重试队列。重建的精确定义是：保留失败点及其后继关系，排除已经成功发送的祖先前缀；它本身不承诺后续一定有人调度重试。

### 重建只恢复账目，不负责调度

`InputConsumer::sendFinishedSignal()` 返回错误前会尽量重建未完成 chain。 但它不会注册 OUTPUT，也不会创建定时任务。 重试是否真的发生，取决于调用者。 正常 `NativeInputEventReceiver::finishInputEvent()` 只在 WOULD_BLOCK 时把最终 seq 入 `mFinishQueue`，因此该路径能接续重试。 非 WOULD_BLOCK 错误不会入队。

绕过 `NativeInputEventReceiver::finishInputEvent()` 的 direct-send 路径也不会自动获得重试。

## 8. 失败与关闭边界：direct send、HUP、dispose 和 publisher 接收

“正常 finish 可在 WOULD_BLOCK 后重试”不能外推成“所有 FINISHED 都可靠排队”。 r48 有几条刻意或历史形成的 best-effort 路径，也有明确终止连接的错误路径。 源码锚点包括：

- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp:163-215`
- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp:223-353`
- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp:386-403`
- `frameworks/native/libs/input/InputTransport.cpp:575-595`
- `frameworks/native/libs/input/InputTransport.cpp:647-747`
- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp:2696-2762`
- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp:4379-4401`

### 普通 Java finish 的错误矩阵

普通 Java 调用先从 `mSeqMap` 删除映射，再进入 native。 `NativeInputEventReceiver::finishInputEvent()` 的结果是：

| status | receiver 动作 | Java 可见结果 |
|---|---|---|
| `OK` | 不入队 | 正常返回 |
| `WOULD_BLOCK` | 入 `mFinishQueue`、监听 OUTPUT、转成 OK | 正常返回 |
| `DEAD_OBJECT` | 记录 warning，不入队 | JNI 明确不抛 RuntimeException |
| 其他错误 | 记录 warning，不入队 | JNI 抛 RuntimeException |

由于 mSeqMap 已经先删除，其他错误抛出后没有同一正常映射可直接重试。 这与 WOULD_BLOCK 被 native 接管的路径不同。

### queued retry 遇到致命错误

OUTPUT callback 重试时，非 WOULD_BLOCK 错误会删除失败项之前的完整成功项。 失败的当前项和后缀仍留在 `mFinishQueue` 的内存中。 但 callback 随即返回 0，Looper 会移除 fd callback。 因此这些剩余项不会再得到自动 OUTPUT 重试。 错误不是 DEAD_OBJECT 时，代码创建 RuntimeException，经 `MessageQueue.raiseAndClearException()` 保存。 本轮 native poll 返回 Java 后，异常由 `pollOnce()` 重新抛出。

对于一个 batched logical Finish，hard error 前已写出的祖先 packet 不会重发；`mSeqChains` 只恢复失败点及后缀的未发关系。错误是 DEAD_OBJECT 时，不向 Java 抛这个 RuntimeException，但 callback 同样被移除。所以“Vector/chain 中仍有记录”不等于“仍有活跃重试机制”。

该路径与 ERROR/HUP 分支一样没有把 `mFdEvents` 归零。若 `mFinishQueue` 已非空，后来又有一次直接 finish 遇到 WOULD_BLOCK，`size()==1` 的“首次入队”条件不成立，便不会重新注册 OUTPUT；旧项和新项都只会成为内存残留，等待对象销毁。

### ERROR/HANGUP 优先于 INPUT 和 OUTPUT

`NativeInputEventReceiver::handleEvent()` 首先检查：

```text
ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP
```

只要命中，就直接返回 0。 即使同一次 callback 还带 OUTPUT，也不会先 flush `mFinishQueue`。 该分支不 clear queue，也不抛 RuntimeException。 Looper 删除 callback 后，`mFdEvents` 成员本身没有在这个分支被改成 0。 这些对象状态最终由 dispose/析构收走，而不是继续恢复传输。

### INPUT 与 OUTPUT 同时到达时的优先级

排除 ERROR/HANGUP 后，代码先处理 `ALOOPER_EVENT_INPUT`。该分支调用 `consumeEvents()` 后直接返回，所以同一 events 位图同时包含 INPUT 和 OUTPUT 时，本轮不会继续进入 OUTPUT 分支。若 `consumeEvents()` 返回 `OK` 或 `NO_MEMORY`，callback 仍保留，后续可写通知还能触发 queue 重试，这通常只是延迟。

若 `consumeEvents()` 返回既非 `OK` 也非 `NO_MEMORY`，INPUT 分支直接返回 0：同一次 callback 的 OUTPUT 不再处理，Looper 删除 callback，已有 `mFinishQueue` 不 flush，`mFdEvents` 与 queue 也不清零或重新注册。因而“只是延迟”只适用于 callback 被保留的分支。

### Java dispatch 回调异常是 best-effort 止损

JNI 调用 `dispatchInputEvent(seq, inputEventObj)` 后检查 Java exception。 若出现异常，它把 `skipCallbacks` 设为 true。 当前事件以及同一轮随后 consume 的事件会尝试：

```cpp
mInputConsumer.sendFinishedSignal(seq, false);
```

这次调用绕过 `NativeInputEventReceiver::finishInputEvent()`。 返回 status 被完全忽略。 如果返回 WOULD_BLOCK，即使 batch chain 已恢复未发关系，也没有 `{seq, false}` 被加入 `mFinishQueue`。 因此源码只能说明“尝试以 false 止损”，不能保证异常不会留下 waitQueue 项。

### 异常还可能留下 Java 映射或重复确认

Java `dispatchInputEvent()` 在调用可覆写的 `onInputEvent()` 前已经 put mSeqMap。若 `onInputEvent()` 抛出且未自行 finish，native 的 direct false 不会移除这条 Java map，映射可能继续残留。若 callback 已 finish 后才抛异常，native 又会尝试发送同 seq 的 false FINISHED，形成重复确认。

普通 Motion 或没有进入 fallback 的 Key，第二个同 seq 通常因 waitQueue 已无匹配项而被忽略；但这不是安全保证。若第一条 `handled=false` 的 Key 回执让 policy 选择 fallback，r48 会复用同一个 `DispatchEntry` 和同一个 seq，立即重新 publish。排队的第二条旧回执便可能命中 fallback leg、提前 release；同一 receive callback 共享的旧 `finishTime` 甚至可能早于 fallback 的新 `deliveryTime`，得到负的 eventDuration。same-seq fallback 是“重复回执通常无害”的明确反例。

异常路径因此不是“严格一次”的正常协议，而是尽力避免更大范围堵塞。

### pending batch 遇 CANCEL 也存在 direct-send 边界

`InputConsumer.consume()` 若已有可合并 MOVE batch，又读到 pointer CANCEL，会丢弃旧 batch samples。 它逐个调用：

```cpp
sendFinishedSignal(sampleSeq, false);
```

这些调用同样不经过 `NativeInputEventReceiver::finishInputEvent()`，返回值也被忽略。 随后旧 samples 从 `mBatches` 删除，CANCEL 自身继续作为事件交付。 因此旧 sample 的 false FINISHED 是 best-effort。 若反向 socket 此时 WOULD_BLOCK，旧 sample 不会自动进入 finishQueue。

另一个相邻边界是 `onBatchedInputEventPending()` 抛异常：native 只把 `mBatchedInputEventPending` 清回 false，Batch 中的 samples 仍未 ack；异常随后仍会经 MessageQueue 重新抛出。它不会像 Key/Motion `dispatchInputEvent()` 异常那样进入 `skipCallbacks` direct false-send。

### FocusEvent 的完成路径不同

FocusEvent 不创建普通 Java `InputEvent`，也不进入 mSeqMap。 native receiver 调用 Java `onFocusEvent(hasFocus, inTouchMode)` 后，直接执行：

```cpp
finishInputEvent(seq, true);
```

因为它走的是 `NativeInputEventReceiver::finishInputEvent()`，WOULD_BLOCK 仍可进入 mFinishQueue。但这个调用的非 `WOULD_BLOCK` 错误返回值在 focus 分支被忽略，只留下 native warning。`onFocusEvent()` 若抛异常，该分支仍尝试以 handled=true 完成，然后 `continue`。它不进入 Key/Motion 的 `skipCallbacks` false-send 分支。

在 `ViewRootImpl` 中，这个 Java callback 只是更新 upcoming focus 状态并 post `MSG_WINDOW_FOCUS_CHANGED`；真正的 Insets、IME 与 View focus 工作由 Handler 稍后执行。这里的 handled=true 表示 Focus 通知已交给 Java 排队，不证明 View 树的焦点处理已经全部完成。为让紧随其后的普通输入看到新状态，`deliverInputEvent()` 还会先调用 `handleWindowFocusChanged()`。

### dispose 不会 flush 或合成 FINISHED

`NativeInputEventReceiver::dispose()` 只调用 `setFdEvents(0)` 停止监听。 JNI `nativeDispose()` 随后释放 receiver 的强引用。 receiver 析构时，`mFinishQueue`、`mSeqChains` 以及 native pending batches 都随对象销毁。 没有循环发送剩余 FINISHED 的步骤。

Java `InputEventReceiver.dispose()` 随后另外调用 `mInputChannel.dispose()`，释放 Java wrapper 的 channel 引用。 当客户端 endpoint 的所有引用真正关闭后，publisher 侧会观察到 HUP/EOF。 若还有 duplicated fd 或其他 native 强引用，真正关闭可能延后。 所以 dispose 的语义是终止 receiver 生命周期，不是代替业务正常 finish。

## 9. Dispatcher 收到 FINISHED：先收回执，再执行完成命令

### 反向消息入口

InputChannel 注册时，Dispatcher 把 server 端 fd 以 `ALOOPER_EVENT_INPUT` 加入 Looper；窗口和 monitor 使用同一个 `handleReceiveCallback()`。源码锚点：`InputDispatcher.cpp:2696-2761,4313-4320,4342-4353`。

callback 持有 `mLock`，先按 fd 查 `mConnectionsByFd`。fd 已不在表中时返回 0，让 Looper 移除回调，不猜测 Connection，也不碰任何 waitQueue。

正常可读时，它在一次 callback 内循环调用：

```cpp
status = connection->inputPublisher.receiveFinishedSignal(&seq, &handled);
```

直到返回非 OK。一次唤醒可读出多条 FINISHED，batched Motion 展开的多个底层 seq 常走这条路径。

### 同一轮回执共享 finishTime

`currentTime = now()`在 receive 循环之前只取一次。循环中每条成功消息都调用：

```cpp
finishDispatchCycleLocked(currentTime, connection, seq, handled);
```

因此同一 fd callback 中的多条回执共享同一个 finish timestamp。后面计算的 eventDuration 是“delivery 到本轮 receive 循环开始前采样点”的近似时间，不是每条回执的精确读取时刻，也不是 Command 实际执行的时刻。

### 反向协议只接受 FINISHED

`InputChannel::receiveMessage()`先用 `InputMessage::isValid(actualSize)`校验 packet 长度、已知结构类型及 Motion pointerCount；EAGAIN 返回 WOULD_BLOCK，EOF/常见断链 errno 返回 DEAD_OBJECT，结构非法返回 BAD_VALUE。它只判断“这是合法 InputMessage”，不判断反向协议方向。

`InputPublisher::receiveFinishedSignal()`随后才要求 `header.type == FINISHED`；合法的 KEY/MOTION/FOCUS 仍会在这一层返回 `UNKNOWN_ERROR`。底层读取失败时它先把输出设为 `seq=0, handled=false`；成功时导出 `finished.seq`，并仅以 `finished.handled == 1` 得到 true。源码锚点：`InputTransport.cpp:97-115,343-379,575-594`。

所以非 FINISHED 不能被降级为 `handled=false`；它是连接协议错误。seq=0 在此处没有额外致命校验，但不可能命中正常 DispatchEntry，最终按 unknown seq 处理。

### finishDispatchCycleLocked 只做状态门控和投递

`finishDispatchCycleLocked()`遇到 `STATUS_BROKEN` 或 `STATUS_ZOMBIE` 立即返回；迟到回执不会查队列、清 deadline 或重启 outbound。只有仍可结算的 Connection 才调用 `onDispatchCycleFinishedLocked()`。源码锚点：`InputDispatcher.cpp:2638-2653`。

`onDispatchCycleFinishedLocked()`创建 CommandEntry，保存：

- Connection 强引用；
- receive callback 取得的时间；
- channel seq；
- handled。

其执行函数是 `doDispatchCycleFinishedLockedInterruptible()`。源码锚点：`InputDispatcher.cpp:4511-4520`。

### 接收阶段不会逐条删除 waitQueue

receive 循环里的 finish 函数只 post Command，因此实际顺序是：

```text
receive FINISHED(A) -> post Command(A)
receive FINISHED(B) -> post Command(B)
receive WOULD_BLOCK
run Command(A) -> run Command(B)
```

在前半段收消息时，A、B 对应的 waitQueue entry 尚未逐条 erase。这一点解释了为什么完成匹配必须在 Command 内进行。

### CommandQueue 提供可解锁的 continuation

只要本轮至少读到一条回执，callback 随后调用 `runCommandsLockedInterruptible()`。它 FIFO 取 Command，并持续运行到队列为空；命令可以临时释放 `mLock` 调 policy，再重新加锁。源码锚点：`InputDispatcher.cpp:952-974`。

CommandEntry 持有 Connection 强引用，保证对象可活到命令结束；但 Connection 可能已从 fd/token 表移除，其队列也可能已 drain，所以强引用不等于队列内容仍有效。

### WOULD_BLOCK 与真正错误

成功读到至少一条消息后，最终状态为 WOULD_BLOCK 是正常的“本轮读空”：执行 Command 后 callback 返回 1，继续监听 fd。

以下情况进入注销路径：

- `ALOOPER_EVENT_ERROR` 或 `ALOOPER_EVENT_HANGUP`；
- 第一次 receive 就得到 WOULD_BLOCK，即 `gotOne == false`；
- peer 关闭导致 DEAD_OBJECT；
- 消息大小/格式错误；
- consumer 发来非 FINISHED；
- 其他非 WOULD_BLOCK 错误。

callback 最终调用 `unregisterInputChannelLocked()`并返回 0。普通 window 的 receive error 会记录日志，并可能以 `notify=true` 让 abort post broken Command；但 unregister 紧接着把状态设成 ZOMBIE，Command 真正执行时会被状态门禁跳过，不会再实际调用 policy。monitor 的 DEAD_OBJECT 还会抑制对应日志和 Command；清理队列的原则不变。

### 收到回执不等于已结算

Dispatcher 端应分四层理解：

1. InputChannel 验证 packet 大小与已知结构类型；
2. InputPublisher 再验证反向消息必须是 FINISHED，并抽取 seq/handled；
3. callback 把 `seq/handled/receiveTime` 封装成 Command；
4. Command 才按 seq 查 waitQueue、运行 after hook、清 ANR 记录并决定 release 或 restart。

这也意味着异常连接可能在第 3、4 层之间被注销；完成路径不能长期保存第一次查到的 iterator 或裸指针。

## 10. 按 seq 结算：两次查找、handled 与 Key fallback

### waitQueue 可完成中间项

`Connection::findWaitQueueEntry(seq)`从 deque 头到尾线性扫描，比较每个 `DispatchEntry::seq`，并返回命中的 iterator。源码锚点：`Connection.cpp:56-62`。

因此 Dispatcher 不要求 FINISHED 只能对应队首。正常 App 往往近似按顺序完成，但代码允许按 seq 删除中间项。

### unknown seq 的精确结果

完成 Command 首次查找若失败，会直接 return。它不会：

- 删除 waitQueue 头；
- 根据 handled 猜另一个事件；
- 删除任意 ANR deadline；
- release 任何 DispatchEntry；
- 调用 `startDispatchCycleLocked()`。

unknown seq 可能来自迟到/重复回执，或队列已被 broken/unregister drain。只有状态仍可结算且 seq 命中的 FINISHED，才可能清 entry 并推动 outbound。源码锚点：`InputDispatcher.cpp:4751-4761`。

### eventDuration 的边界

首次命中后计算：

```cpp
eventDuration = finishTime - dispatchEntry->deliveryTime;
```

它包含 publish 后的通道传输、consumer/Java 排队与处理、FINISHED 回传和 Looper 唤醒；不包含回执已读之后的 Command 等待、policy fallback 调用和最终 erase 时间。

超过 2 秒会打印 slow processing 日志。随后 handled 被传给 `reportDispatchStatistics()`，但 r48 实现仍只有 TODO，不能写成已有完整 handled 统计。源码锚点：`InputDispatcher.cpp:92-93,4751-4768,5008-5011`。

### after hook 分流

首次查找和统计后：

- Key 调 `afterKeyEventLockedInterruptible()`；
- Motion 调 `afterMotionEventLockedInterruptible()`；
- Focus 等类型令 `restartEvent=false`。

r48 的 Motion hook 无条件 `return false`；handled=false 不会重派 Motion、换目标或生成 fallback。Focus 也没有专门 after hook，只走公共出队。

### 已是 fallback 的 Key 不再递归 fallback

如果 KeyEntry 已带 `AKEY_EVENT_FLAG_FALLBACK`：

- handled=true：结束；
- handled=false：`mReporter->reportUnhandledKey(keyEntry->id)`；
- 两者都返回 false，不再产生下一层 fallback。

源码锚点：`InputDispatcher.cpp:4812-4818`。

### 原 Key 的 fallback 状态

对非 fallback key，Dispatcher 从 Connection 的 InputState 读取 `originalKeyCode -> fallbackKeyCode`。若原事件是 UP，会先从 InputState 删除映射，但局部变量保留本次判断需要的旧值。

若 `handled || !hasForegroundTarget()`，本次不会新建 fallback。已有 fallback 映射时会：

1. 构造带 CANCELED flag 的原 KeyEvent；
2. 解锁并通知 policy；
3. 对已知 fallback key 合成取消事件；
4. 清 original→fallback 映射。

所以 monitor 等非 foreground target 即使回 handled=false，也不能启动 fallback。源码锚点：`InputDispatcher.cpp:4820-4860`。

### 只有 unhandled foreground 才可能询问 policy

原 key 同时满足 handled=false 和 foreground 才进入 unhandled policy 流程。

没有既存映射时，只有 `DOWN && repeatCount == 0` 能建立 fallback 生命周期；孤立 repeat/UP 会直接返回 false。符合条件时复制 KeyEvent，解锁调用 `dispatchUnhandledKey()`，再重新加锁。

锁释放期间 Connection 可能被注销并 drain。policy 返回后先检查状态仍为 NORMAL；否则删除 fallback 映射并停止。源码锚点：`InputDispatcher.cpp:4861-4895`。

initial DOWN 会锁存 policy 返回的 fallback keycode；不给 fallback 时保存 `AKEYCODE_UNKNOWN`，区别于“尚无映射”的 `-1`。后续 policy 若改变或撤销已锁存 fallback，会先合成旧 fallback 的取消事件，避免生命周期半途换码。源码锚点：`InputDispatcher.cpp:4897-4953`。

### restart 修改 KeyEntry，不创建新 DispatchEntry

policy 选择 fallback 时，Dispatcher 更新 KeyEntry 的时间、device/source/display、flags、keyCode/scanCode/metaState、repeatCount/downTime 等字段，并加 `AKEY_EVENT_FLAG_FALLBACK`，然后返回 `restartEvent=true`。源码锚点：`InputDispatcher.cpp:4954-4973`。

这里没有调用 `enqueueDispatchEntryLocked()`，也没有 new DispatchEntry。`DispatchEntry::seq` 是 const，因此原 key 与 fallback 重派沿用同一 channel seq。源码锚点：`Entry.h:190-224`。

### 为什么必须第二次 find

afterKey 可能在 policy 调用时释放锁，因此返回后第一次 iterator 可能已经失效。公共代码再次执行 `findWaitQueueEntry(seq)`：

- 再次命中：才 erase、清 tracker、恢复 responsive、release/restart；
- 已被 drain：跳过 erase/release，避免二次释放。

Motion 当前不解锁，但共用这套二次验证框架。源码锚点：`InputDispatcher.cpp:4783-4803`。

第二次查找 miss 仍会走到函数末尾尝试 `startDispatchCycleLocked()`；这与 10.2 中首次 unknown seq 直接 return 的边界不同。

### 普通 release 与 fallback restart

二次命中后先删除旧 wait 项和旧 `(timeoutTime, token)`。随后：

- `restartEvent && status==NORMAL`：同一 DispatchEntry `push_front` 回 outbound；
- 否则：`releaseDispatchEntry()`。

最后调用 `startDispatchCycleLocked(now(), connection)`。fallback 再 publish 时刷新 deliveryTime/timeoutTime，成功后重新进入 waitQueue 和新的 ANR deadline。

```text
original wait(seq=S)
  -> FINISHED(S,false)
  -> erase old wait/deadline
  -> same DispatchEntry to outbound front
  -> publish fallback(seq=S)
  -> new wait/deadline
  -> FINISHED(S,handled2)
  -> final release
```

所以“finish 一定 release”不成立；fallback 是明确例外。

## 11. outbound/wait、ANRTracker、responsive 与异常清理

### ANR 计时从 publish 成功开始

§3 已展开 outbound→wait 的迁移。对本节最关键的增量是：发送尝试前虽会写 `deliveryTime/timeoutTime`，只有 publish OK 后 entry 才进入 wait；Connection 仍 responsive 时，deadline 才插入 `mAnrTracker`。无窗口可取自定义值时使用默认 5 秒；同一次 cycle 的连续成功项还可能共享传入的 currentTime。源码锚点：`InputDispatcher.cpp:79-81,525-530,2456-2473,2602-2613`。

publish WOULD_BLOCK 且 wait 非空时，当前项留在 outbound，已有 wait 只证明“尚未收到 FINISHED”，不能证明 App 还没读 socket。反向 FINISHED 是 Dispatcher 再调用 startDispatch 的调度机会，不是释放正向 socket 空间的物理动作；wait 为空却 WOULD_BLOCK 或其他 publish 硬错误会直接 broken abort。源码锚点：`InputDispatcher.cpp:2574-2599`。

### mAnrTracker 的真实数据模型

AnrTracker 是：

```cpp
multiset<pair<timeoutTime, connectionToken>>
```

它不保存 seq、DispatchEntry 指针、eventId、handled 或 foreground。相同 deadline+token 允许重复；正常 FINISHED 的 `erase(timeout, token)`只删一份。源码锚点：`AnrTracker.h:45-55`、`AnrTracker.cpp:26-49`。

waitQueue 用 seq 精确找对象，tracker 用 deadline+token 快速找全局下一次唤醒，两者职责不同。

### 非 foreground 也会 ANR

成功 publish 后是否插 tracker 只看 `connection->responsive`，不看 `FLAG_FOREGROUND`。outside、wallpaper、monitor、FocusEvent 都可有 wait/deadline；FOREGROUND 仅控制同步注入 pending。源码锚点：`InputDispatcher.cpp:2607-2611,1883-1888,2046-2053`。

因此非 foreground 可以不阻塞 WAIT_FOR_FINISHED，却仍因忘记 FINISHED 触发其 Connection 的 ANR。

### processAnrs 的状态迁移

每轮 Dispatcher 把 `mAnrTracker.firstTimeout()`并入下一唤醒时间。到达全局最早 deadline 后：

1. 用 firstToken 找 Connection；
2. 设置 `responsive=false`；
3. `eraseToken(token)`删除该 Connection 全部 tracker 项；
4. 调用 `onAnrLocked(connection)`；若 waitQueue 已因异常路径变空，它会直接返回，否则才投递 policy 通知。

源码锚点：`InputDispatcher.cpp:489-522`。

ANR reason 取 `waitQueue.front()`和其 deliveryTime。源码明确说，timeout 动态变化可能使较新 entry 实际先到期；日志仍展示队首，因为它通常更有诊断价值。不要把 reason 中的 entry 写成 tracker 精确定位出的 seq。源码锚点：`InputDispatcher.cpp:4545-4577`。

### policy extension 与 cancel 是两条路

对 Connection ANR，Dispatcher 会解锁调用 policy。重锁后若 connection 已被 unregister，`getConnectionLocked()` 返回 null，本轮直接结束；若仍存在，policy 返回正 extension 时把它设回 responsive，以 `now()+extension` 更新符合条件的 wait 项并重插 tracker，返回非正值时调用 `cancelEventsForAnrLocked()`。源码锚点：`InputDispatcher.cpp:4650-4698`。

ANR cancel 不会 break Connection，也不会 drain 原 wait。`cancelEventsForAnrLocked()`仅在状态仍为 NORMAL 时尝试根据 InputState 合成新的 CANCELED Key UP、Motion CANCEL/HOVER_EXIT；没有匹配 memento 时可以一条也不生成。实际生成的事件以普通 AS_IS target 追加到 outbound，仍走 publish→wait→FINISHED。源码锚点：`InputDispatcher.cpp:1364-1376,2799-2864`、`InputState.cpp:268-297`。

cancel 不是 FINISHED 的替身，也不是越过 outbound 的旁路；channel 背压时它仍可能等在旧 outbound 后面。

### responsive 如何恢复

处理匹配 FINISHED 时，先删除该 wait 项；若 Connection 此前不 responsive，再扫描剩余 waitQueue。只要没有 entry 满足 `timeoutTime < now()`就恢复 true，队列无需为空。源码锚点：`InputDispatcher.cpp:4737-4749,4787-4796`。

注意边界差异：恢复检查使用严格 `< now()`；全局 ANR 在时间等于最早 deadline 时已可触发。

### r48 恢复不会重建剩余旧 deadline

ANR 时 `eraseToken()`已清掉该 Connection 全部 tracker 记录。普通 FINISHED 恢复 responsive 的代码只修改 bool，没有遍历仍在 waitQueue 的未来项并重插 tracker。

后续新 publish 会因 responsive=true 正常入 tracker，但不能写成“恢复 responsive 会全量重建旧 deadline”。policy extension 是显式更新/重插的另一条路径。

### mAnrTracker 还短暂阻塞新 Key

`shouldWaitToSendKeyLocked()`发现全局 mAnrTracker 非空时，会让新的 focused Key 最多等待 500ms，让前序输入有机会改变焦点；它并非只看目标窗口的 waitQueue。500ms 后即使仍有未完成输入也继续派发。源码锚点：`InputDispatcher.cpp:1417-1444`。

因此 FINISHED 清 tracker 除防止 ANR 外，还可能解除一个等待焦点稳定的 Key。

### 异常清理会打破 queue 与 tracker 的直觉对应

`drainDispatchQueue()`本身不清 tracker；unregister 会先另行 `eraseToken()`再 drain，direct publish error 的 standalone broken abort 则不会。反过来，global reset 会 clear tracker，却不 drain 每条 Connection 的 outbound/wait。精确顺序见 §13，dumpsys 可见字段见 §14。源码锚点：`InputDispatcher.cpp:2655-2694,4026-4043,4379-4401,4506-4509`。

## 12. InjectionState：WAIT_FOR_FINISHED 与多目标收束

### pending 在 foreground entry 入 outbound 时增加

DispatchEntry 通过 InputState 一致性检查后，若有 `FLAG_FOREGROUND`，Dispatcher 先 `incrementPendingForegroundDispatches(newEntry)`，再 push outbound。源码锚点：`InputDispatcher.cpp:2298-2425`。

所以计数覆盖：

```text
accepted outbound -> publish/wait -> optional fallback restart -> final release
```

它不是 publish 成功后才开始，也不以 handled=true 为结束条件。EventEntry 没有 InjectionState 时，增减函数都是空操作。源码锚点：`InputDispatcher.cpp:3547-3563`。

### 聚合单位是共享 InjectionState

同一注入事件可为多个目标创建 DispatchEntry，每个被接受的 foreground entry 都 `+1`；各 Connection 独立 publish、wait、finish。

split touch 会创建新的 MotionEntry 和 event id，但复制同一个 InjectionState 引用。源码锚点：`InputDispatcher.cpp:2925-3016`。因此跨窗口聚合的是 InjectionState，不保证所有 DispatchEntry 指向同一 EventEntry。

### 计数的是 DispatchEntry，不是窗口数

一个 InputTarget 可合并多个 dispatch mode，`enqueueDispatchEntriesLocked()`会分别构造 entry。若它们保留 FOREGROUND，每项都单独计数。源码锚点：`InputDispatcher.cpp:2265-2295`、`TouchState.cpp:54-68`。

最准确的定义是：

> pendingForegroundDispatches 是仍存活、属于该 InjectionState 的 foreground DispatchEntry 数。

它不是窗口数、InputTarget 数、EventEntry 数、waitQueue 总长度或 handled=false 数量。

### WAIT_FOR_RESULT 与 WAIT_FOR_FINISHED

注入线程先等 `injectionResult` 从 PENDING 变成确定值。只有结果 SUCCEEDED 且 syncMode 为 WAIT_FOR_FINISHED，才继续等待 `pendingForegroundDispatches == 0`。源码锚点：`InputDispatcher.cpp:3418-3467`。

两阶段共用从注入开始计算的绝对 endTime；目标解析消耗的时间不会在 finish 阶段重新补给。

WAIT_FOR_RESULT 只确认权限/目标/派发结果，不等 App 完成；WAIT_FOR_FINISHED 等待已计数 foreground dispatch 退出生命周期。

### non-foreground 不阻塞，但仍须 FINISHED

wallpaper、outside listener、global/gesture monitor 以及普通合成 cancel target 通常没有 FOREGROUND，不增加 pending。因此它们仍在 wait 时，所有 foreground 项若已 release，WAIT_FOR_FINISHED 可以返回。

这不表示这些目标无需完成：只要 Connection responsive，它们仍有 ANR deadline，忘记 FINISHED 仍会导致该 Connection 不响应。

### handled 不直接控制 decrement

正常完成最终调用 `releaseDispatchEntry()`；只要 entry 是 foreground，就 decrement，无论 handled 为 true 或 false。

Motion handled=false 的 after hook 返回 false，所以通常直接 release。Key handled=false 若产生 fallback，则还没有 release，pending 保持不变。

### fallback 让一次 pending 跨两轮 publish

Key fallback 使用同一 DispatchEntry，从旧 wait 回到 outbound，不经过新的 increment，也没有旧轮次 decrement：

```text
original enqueue: 0 -> 1
FINISHED(false), restart: still 1
fallback publish/wait: still 1
fallback final FINISHED: 1 -> 0
```

这正是 WAIT_FOR_FINISHED 能覆盖 fallback 第二程的原因。首次 FINISHED 不能一概写成注入完成。

### release 是统一 decrement 出口

`releaseDispatchEntry()`检查 foreground 后 decrement，再 delete entry。它同时服务于正常 FINISHED、after hook 不 restart、outbound/wait drain、publish broken 和 unregister 清理。源码锚点：`InputDispatcher.cpp:2681-2694,3554-3563`。

只有减到 0 才 `mInjectionSyncFinished.notify_all()`；中间从 N 减到 N-1 不需要唤醒只等待归零的线程。

### broken drain 也可结束等待

broken/unregister drain 会 release 尚未收到 FINISHED 的 foreground entry，因此 pending 也能归零。

WAIT_FOR_FINISHED 的精确定义是“所有已计数 foreground DispatchEntry 不再进行中”，不是“每个目标都返回 handled=true”，甚至不强制每项都实际收到 FINISHED。若 injectionResult 已是 SUCCEEDED，随后 drain 令 pending 归零，等待可按已有结果结束；传输终止与 App 成功处理是不同事实。

### 合成 CANCEL 不替原项减计数

ANR 产生的 cancel 是新的非 foreground DispatchEntry，不删除原 foreground wait，也不继承其 pending 责任。发出 CANCEL 本身不会使原注入计数归零；原项仍需自己的 FINISHED/final release，或在 broken/unregister 时被 drain。

这也是 cancel 与 drain 必须分开的原因：cancel 修复逻辑输入状态，drain 终止传输对象生命周期。

### 同步注入超时不撤回事件

等待 injectionResult 或 pending 归零超过 endTime 时，调用者得到 `INPUT_EVENT_INJECTION_TIMED_OUT`。源码不会因此从 outbound/wait 删除 entry、关闭 channel、发送撤回或阻止 App 稍后处理。

所以注入 API 已超时返回后，事件仍可能稍后 publish、被处理并 FINISHED；“调用者停止等”不等于“Dispatcher 撤销事件”。

### 三个多目标推演

主窗口 foreground + wallpaper：主窗 `+1`，wallpaper `+0`；主窗完成后 pending 可归零，即使 wallpaper 仍在自己的 wait/ANR 闭环。

split touch 给两个 foreground 窗口：共享 InjectionState 计数为 2；A FINISHED 后为 1，B FINISHED 或被 drain 后为 0。两者 MotionEntry/event id 可以不同。

foreground Key 触发 fallback：原 key 首次 FINISHED(false)只 restart，pending 仍为 1；fallback 最终完成并 release 时才归零。

### Dispatcher 端最终不变量

1. publish 成功才把 DispatchEntry 从 outbound 转进 wait；WOULD_BLOCK 时间不算在 wait ANR 内。
2. 只有可结算状态且 seq 命中的 FINISHED 才处理指定 wait 项；unknown seq 不推动 cycle。
3. afterKey 可以让同一 DispatchEntry、同一 seq 从 wait 回 outbound，刷新下一轮 deadline。
4. mAnrTracker 跟踪 responsive Connection 的 deadline/token，不跟踪 handled 或 foreground。
5. responsive 恢复不要求 wait 为空，r48 普通恢复路径也不重建所有旧 tracker 项。
6. WAIT_FOR_FINISHED 等最终 release；正常 FINISHED 和 broken drain 都可能使 pending 归零。

四组概念不要互换：

```text
synthetic CANCEL != FINISHED
responsive recovery != rebuild every old ANR deadline
resetAndDropEverything != drain per-connection outbound/wait
handled=false != 当前 DispatchEntry 必然保留（Motion 会 release，Key fallback 才可能 restart）
```

把这些边界固定下来，就能统一解释乱序回执、Key fallback、ANR/恢复、多目标同步注入和 channel 断裂后的 Dispatcher 端结局。

---

## 13. cancel、drain、unregister 与 reset：四种“收尾”不能混称完成

正常 FINISHED 是按 seq 结算旧 wait 项；下面四类操作的共同点只是“都可能让系统继续前进”，证据与副作用并不相同。

### synthetic CANCEL 是新的正向投递

`synthesizeCancelationEventsForConnectionLocked()`依据 `connection->inputState` 尝试生成 canceled Key UP、Motion CANCEL、HOVER_EXIT 等，再以普通 `FLAG_DISPATCH_AS_IS` 进入 outbound；没有匹配 memento 时也可能不生成事件。它不是 out-of-band 控制命令，也不直接删除旧 wait：

```text
旧 wait seq=80 仍待完成
新 synthetic CANCEL 拥有自己的 DispatchEntry/seq
新事件也要 publish，也要等自己的 FINISHED
```

合成 cancel 的 target 不带 `FLAG_FOREGROUND`，因此不会替原 foreground 注入 entry 减 pending。ANR policy 选择不再延长等待时，`cancelEventsForAnrLocked()`也只是合成 `CANCEL_ALL_EVENTS`，不会 break 或 drain connection；旧超时项、合成取消项与 `responsive=false`可以同时存在。

### broken abort 才 drain 两条 connection 队列

`abortBrokenDispatchCycleLocked()`依次 drain outbound、drain wait，再把仍为 NORMAL 的 connection 标成 BROKEN；`notify=true`时只是在此处 post channel-broken Command。每个被 drain 的 entry 都经 `releaseDispatchEntry()`释放，foreground injection pending 随之递减，但没有 handled 值，也没有伪造 FINISHED。

单独的 direct publish fatal abort 到这里就结束：BROKEN connection 仍在 maps 中，fd callback 也尚未由该函数移除，只是不能再正常 dispatch。它不同于完整 unregister。

### drain 不会替调用者维护 AnrTracker

`drainDispatchQueue()`本身只 pop 和 release，不删 tracker。调用路径必须分别核对：

- unregister 的 `removeConnectionLocked()`先 `eraseToken()`，再 drain；
- direct publish fatal error 直接 abort，可能留下该 token 的旧 tracker 记录；
- global reset 另行整体 clear `mAnrTracker`。

因此“队列已经 drain”不能推出 deadline 索引也由同一函数同步清空。即使 stale tracker 后续触发 `processAnrs()`，`onAnrLocked()`看到 waitQueue 已空也会直接返回。

### unregister 的最终状态是 ZOMBIE

r48 unregister 的共同顺序是：

```text
mAnrTracker.eraseToken(token)
  → 从 mConnectionsByFd 移除
  → 从 mInputChannelsByToken 移除
  → 若是 monitor，再从对应 monitor 列表移除
  → 从 Looper 移除 fd
  → abort/drain outbound 与 wait
  → connection->status = ZOMBIE
```

`removeConnectionLocked()`包办前两步，调用者再完成其余步骤。abort 可能从 NORMAL 短暂变成 BROKEN，并在 `notify=true`时 post Command；但 unregister 随即设为 ZOMBIE，Command 真正执行时被门禁跳过，所以这条路径不会实际调用 policy 的 channel-broken callback。ZOMBIE 对象可因 Command 强引用短暂存活，却已从 maps 删除，正常 `dumpsys input`连接表也不再显示它。

反向 callback 若先读到合法 FINISHED、随后遇 fatal status，会先运行已 post 的完成 Commands，再 unregister 并 drain 余项；“最终断链”不表示本轮合法前缀完全没被结算。

### reset 名字不等于清空所有 wait

`resetAndDropEverythingLocked()`会为 connections 合成 cancel、reset key repeat、release pending event、drain inboundQueue，并清 no-focused-window timeout、`mAnrTracker`、touch states 等全局状态。它没有遍历并 drain 每个 connection 的 outbound/wait；旧 entry 可能仍在，合成 cancel 还会新增投递。

客户端 dispose 与它也不同：dispose 停止 App 端监听并最终放弃本地 map、finish queue、seq chain 与 batch 状态；server 端要等 HUP 或显式移除才 unregister。额外 fd/native 引用还可能延后 HUP。

### 收尾语义对照

| 路径 | 原 wait 按 seq 命中 | handled 证据 | foreground pending | connection 后续 |
|---|---:|---:|---:|---|
| 正常 FINISHED | 是 | 有 | release 时减少；fallback 跨轮延后 | 继续 NORMAL |
| synthetic CANCEL | 否；它是新投递 | 旧项没有；新 CANCEL 日后自有 | 不替旧项减少 | 继续 NORMAL |
| standalone broken abort | 不需要 seq | 无 | drain 时减少 | BROKEN、仍在 maps，不再正常 dispatch |
| unregister drain | 不需要 seq | 无 | drain 时减少 | 从 maps/fd 移除，最终 ZOMBIE |
| global reset | 不直接处理旧 wait | 无 | 不因 reset 本身减少旧 wait debt | 不一定改变 |

---

## 14. 排障方法：先判断债务停在哪一本账

### `dumpsys input` 在 r48 真正显示什么

connection dump 会列：

```text
status / monitor / responsive
outboundQueue length
waitQueue length
```

逐项输出中：

- outbound 有事件描述、target flags、resolved action、`age`；
- wait 另有由 `now-deliveryTime` 算出的 `wait`；
- **不直接打印** `DispatchEntry.seq`、`deliveryTime`、`timeoutTime` 或 deadline。

所以看到 `wait=4800ms` 可判断接近默认 5s，但不能从 dumpsys 原文抄出 seq 与绝对 timeout。

### waitQueue 长只能证明“未收完成凭证”

它不能单独区分：

- App 尚未从 socket consume；
- native 已 consume、Batch 正等 VSync；
- Java 主线程排队；
- async InputStage 正 defer；
- Java 已 finish、native `mFinishQueue` 正等 OUTPUT；
- direct-send FINISHED 因背压丢了重试；
- 回执已在 socket、Dispatcher 尚未 poll。

必须结合 App trace、日志和队列增长趋势定位。

### outbound 与 wait 的组合更有信息量

| 观察 | 首要方向 | 不能过早下的结论 |
|---|---|---|
| outbound=0、wait=0、status=BROKEN | 首次 publish WOULD_BLOCK 或正向 fatal 后当场 abort/drain | “从未发生发送错误” |
| outbound↑、wait↑ | App/反向回执落后，正向又受阻 | “App 完全没 consume” |
| outbound=0、wait↑ | 所有当前消息已 publish，完成链停滞 | “没有输入积压” |
| wait 下降、outbound 随后下降 | 命中 FINISHED 给了重试机会 | “FINISHED 本身释放正向 socket buffer” |
| connection 从 dump 中消失 | 可能已 unregister/drain | “App handled 了全部事件” |

正向 socket 空间是在 App `receiveMessage()` consume packet 时释放。Dispatcher r48 没在 publisher 侧监听 OUTPUT；它常借命中 FINISHED 后的 `startDispatchCycleLocked()` 再试，所以 FINISHED 是调度机会，而不是内核释放正向 buffer 的物理动作。

`outbound>0 && wait=0`可以作为源码分支的瞬时推演输入，却不是持锁 dumpsys 应看到的稳定背压态：首次 publish 在 wait 为空时返回 WOULD_BLOCK，会在同一临界区立刻 abort 并 drain 两条队列。

### Java 侧要区分三层“没有 pending”

```text
mPendingInputEventCount == 0
  ⇏ AsyncInputStage queue 为空

某个 async queue 为空
  ⇏ mSeqMap 为空

mSeqMap 已删除某项
  ⇏ Dispatcher 已收到 FINISHED
```

尤其 DEFER 后 q 已从这个 ViewRoot 的入口 pending 队列摘除，转由 async stage 私有队列持有。

### 常见日志怎样解读

| 日志/现象 | 更精确的含义 |
|---|---|
| `not in progress` | 当前 receiver 的 mSeqMap 没这个 Java sequence；可能错误对象、错误 receiver、重复 finish |
| `recycled twice` | Motion 所有权越过完成点或重复回收；不是 Dispatcher seq 冲突 |
| pipe is full + wait nonempty | publisher 写阻塞且已有未 ack 投递；不证明那些 packet 仍未被 native consume |
| pipe full + wait empty | Dispatcher 认为状态不可能，走 broken abort |
| slow event processing | delivery→回执接收严格超过 2s；尚不等同 ANR |
| channel unrecoverably broken | transport 关系将清理；不携带 handled 结论 |
| Key 被 fallback | 原 foreground Key FINISHED(false) 后 policy 选择了替代键 |

### mAnrTracker 还会让新 Key 最多等 500ms

`shouldWaitToSendKeyLocked()` 只要发现全局 `mAnrTracker` 非空，就可能让新的 focused Key 等最多 `KEY_WAITING_FOR_EVENTS_TIMEOUT=500ms`，给前序输入改变焦点的机会。

它看的是全局 tracker，不是当前 connection 的 wait front。某个窗口未确认的 Motion 因而可能间接让另一条 focused Key 暂缓。

这也是 tracker 与 waitQueue 不同用途的例子：前者不仅驱动 ANR，还参与 Key 派发时序决策。

### 非 foreground 也可能进 wait/ANR

publish 成功后是否插 tracker，只看 connection 是否 responsive，不看 `FLAG_FOREGROUND`。outside、wallpaper、monitor、Focus 等非 foreground entry 仍可拥有 wait debt 和 timeout。

`FOREGROUND` 只影响 injection pending 计数。于是：

```text
WAIT_FOR_FINISHED 已返回
  ⇏ 所有非 foreground waitQueue 都为空
```

### 一套从外到内的定位顺序

1. 看 connection `status/responsive`，先排除已经 unregister 的终态。
2. 比较 outbound/wait 的长度和 `age/wait`，判断发送前还是发送后积压。
3. 看 App main thread 是否停在 traversal、IME、native input queue 或业务 handler。
4. 看 Choreographer input callback，确认 batch 是否在等帧。
5. 查自定义 receiver/compat processor 的所有 finish/null/exception 分支。
6. 若 Java 已结束而 wait 不降，检查 native reverse channel 背压、Looper teardown 和异常 direct-send。
7. Key 场景继续追 policy fallback，避免把第一条 FINISHED 当作最终 release。

### macOS 只读源码检查命令

```bash
sed -n '2456,2620p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2638,2762p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4737,5011p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

```bash
sed -n '150,225p' frameworks/base/core/java/android/view/InputEventReceiver.java
sed -n '5285,5585p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '8088,8125p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

```bash
sed -n '116,220p' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
sed -n '620,820p' frameworks/native/libs/input/InputTransport.cpp
sed -n '1068,1125p' frameworks/native/libs/input/InputTransport.cpp
```

这些命令只阅读固定区间，不要求编译，也不会修改源码树。

---

## 15. 六条完整时间线：用状态迁移而不是一句“finish 了”推演

### 场景 A：普通 Motion handled=true

```text
t0  Dispatcher 创建 seq=100，foreground pending=1，入 outbound
t1  publish OK，seq100 入 wait + tracker
t2  App native consume，JNI 创建 Motion(javaSeq=7)
t3  mSeqMap[7]=100，ViewRoot pipeline 处理
t4  receiver 查 7→100，先删 map，反向发送 FINISHED(100,true)
t5  Dispatcher 收包、post command、seq100 命中
t6  afterMotion=false，erase wait/tracker，release，pending=0
```

完成点在 t6，不在 t3；t4 若 socket `WOULD_BLOCK`，还要插入 native queue 等重试。

### 场景 B：三条 MOVE 合成一次 Java callback

```text
Dispatcher wait: [201, 202, 203]
App Batch:        [201, 202, 203]
consumeSamples:   202→201, 203→202, Java outSeq=203
Java finish:      handled=false
reverse packets:  201 false, 202 false, 203 false
Dispatcher:       三次独立 lookup/erase/release
```

若 resampling 添加 synthetic current，它没有独立 transport seq，也不需要第四条 FINISHED。

### 场景 C：不同 device 的异步结果超车

```text
waitQueue order: K(device1, seq301), M(device2, seq302)
IME defer K
device2 的 M 先完成 → FINISHED(302)
Dispatcher 线性查到第二项并删
K callback 稍后完成 → FINISHED(301)
```

按 seq 查任意位置是正常能力，不是异常容错才用到。

### 场景 D：原 Key false 后变成 fallback

```text
pending=1
publish original key seq=400
← FINISHED(400,false)
policy 把 KeyEntry 改为 fallback
同一 DispatchEntry 从 wait → outbound，pending 仍=1
publish fallback key seq=400，刷新 deadline
← FINISHED(400,true)
release，pending=0
```

若只在第一条 FINISHED 后看日志，会错误认为同步注入理应已经返回。

### 场景 E：Java callback 抛异常且反向 pipe 满

```text
mSeqMap[javaSeq=55]=500
onInputEvent 抛异常
JNI skipCallbacks=true
direct send FINISHED(500,false) → WOULD_BLOCK
返回值被忽略，未入 mFinishQueue
mSeqMap[55] 仍残留
Dispatcher wait seq500 仍可能老化
异常退出 poll，通常会重新抛出
若进程/channel 随后关闭或被显式移除，server 才在 unregister 时 drain
```

这里没有“异常自动可靠销账”的保证。

### 场景 F：ANR 后 policy 尝试合成 CANCEL

```text
seq600 超时 → connection responsive=false
AnrTracker.eraseToken(connection)
policy extension<=0
重锁后 connection 若仍存在，才调用 cancelEventsForAnrLocked
若状态仍为 NORMAL 且 InputState 有匹配 memento，才新建 synthetic CANCEL 到 outbound
旧 wait seq600 仍存在
App 若恢复，可继续回600
App 若被移除，unregister drain 新旧 entries
```

这条时间线能同时解释“新 CANCEL 已入队”与“旧 pending 还没降”为什么不冲突，也提醒我们 policy 返回 0 不保证必有 CANCEL 可合成。

### 每次推演都问六个问题

```text
1. 当前对象是哪一层：EventEntry、DispatchEntry、InputMessage 还是 Java InputEvent？
2. 当前编号是哪一种：event id、transport seq 还是 Java sequence？
3. 义务在哪本账：outbound、wait、mSeqMap、async queue、mFinishQueue？
4. 这一步只改 handled，还是已真正发送 FINISHED？
5. entry 是 release 还是 fallback restart？
6. 结算来自正常 ack，还是 connection drain？
```

回答齐这六项，绝大多数“事件明明处理了为何仍 ANR”的歧义都会消失。

### 本章不变量清单

```text
I1  每份 DispatchEntry 有非0 transport seq；Java mSeq 没这个限制。
I2  publish 成功后才从 outbound 转 wait。
I3  wait 表示未收到匹配完成，不等于 App 尚未 consume socket packet。
I4  mSeqMap 用 Java sequence 查 transport seq，不查 event id 或对象 identity。
I5  正常 native finish 的 WOULD_BLOCK 才由 mFinishQueue 接住；direct-send 例外不保证。
I6  batch Java finish 可展开多条底层 FINISHED，synthetic resample 不新增 seq。
I7  Dispatcher 可按 seq 乱序销账；unknown seq 不推动 cycle。
I8  Key fallback 可用同一 entry/seq 重派，第一程完成不等于 release。
I9  pendingForegroundDispatches 在 release 时减，handled 值不决定它。
I10 synthetic CANCEL、normal FINISHED、broken drain 是三种不同收尾语义。
```

---

## 16. 九道源码练习、复读路线与下一章

### 练习 1：三套序号画线

画出一条 `MotionEntry.id=E`、主窗 `DispatchEntry.seq=S`、Java `MotionEvent.mSeq=J` 的完整链。标出哪一步传 E、哪一步传 S、哪一步用 J 做 map key。

自检：Java 调 native finish 时传的是查回的 S；J 既可能为 0/负数，也不受 transport 非 0 规则约束。

### 练习 2：正向 WOULD_BLOCK

分别推演：

```text
outbound=[A], wait=[]
publish(A) → WOULD_BLOCK
```

与：

```text
outbound=[C], wait=[A,B]
publish(C) → WOULD_BLOCK
```

自检：前者走 broken abort；后者保留 C 等之后的重试机会。`wait=[A,B]` 只证明未 ack，不证明 A/B packet 仍在 socket。

### 练习 3：错误对象完成

原 Motion 的 Java sequence 为 10，copy 为 11。对 copy 调 receiver finish 后，再对 original 正常完成。分别写出 mSeqMap 和两对象回收状态。

自检：copy 查不到并被 recycle；original 的 map 仍在。若错误 receiver 收到 original，它也会 map miss 后回收同一 Motion，可能破坏正确路径。

### 练习 4：compat 一拆三

设计一个自定义 processor：O 产生 P1/P2/P3。写出 `processInputEventBeforeFinish(Pi)` 的返回策略，使 O 只 ack 一次，并说明三份 processed object 谁回收。

自检：框架没有 P→O map，也不会自动聚合 handled；不能让三个 Pi 都直接拿 O 重复 finish。

### 练习 5：batch 部分反压

seq 20→21→22 合成，发送 20 成功、21 `WOULD_BLOCK`。写出重建后的 `mSeqChains`、加入 `mFinishQueue` 的逻辑 seq，以及 retry 成功时的 packet 顺序。

自检：chain 只保留 `22→21`，queue 保存 `{22, handled}`，重试发送 21、22，不重发 20。

### 练习 6：乱序 FINISHED

wait 为 `[31,32,33]`，反向先来 33，再来未知 99，最后来 31。逐步写 wait 和 outbound 是否会被重启。

自检：33 命中并走完整命令尾部，可重启；99 lookup miss，什么都不推进；31 再命中。32 留在 wait。

### 练习 7：fallback 与注入计数

一个 injected foreground Key 原始投递 false，policy 生成 fallback；fallback 也 false。标出 pending 的每次变化、两次 deadline 和最终 reporter 行为。

自检：pending 只在最初 entry 入队时 +1；第一次 FINISHED 不减；fallback 第二次完成才 release/-1；fallback 仍未处理会 reportUnhandledKey。

### 练习 8：ANR 后不杀进程

wait 有两项，第一项超时。policy 返回 3s extension 与返回 0 时分别推演 tracker、responsive、旧 wait，以及可能生成的新 cancel。

自检：重锁后 connection 仍存在时，正 extension 重写/重插符合条件 deadlines；0 保持 unresponsive 并尝试 cancel，但不 drain 旧 wait。只有状态仍为 NORMAL 且 InputState 有匹配 memento 才会生成事件；若 policy 调用期间已经 unregister，两条分支都会因查不到 connection 而结束。

### 练习 9：断链怎样唤醒 WAIT_FOR_FINISHED

一个 foreground entry 还在 outbound，另一个在 wait；connection 被 unregister。解释为什么注入线程可能从 pending=2 到 0，并说明它为何不能据此声称 App handled 两项。

自检：drain 对两项都 `releaseDispatchEntry()`；这是所有权终止，没有 handled 或 FINISHED 证据。

### 建议复读顺序

第一次只抓主干：

```text
DispatchEntry.seq
  → outbound/wait
  → mSeqMap
  → Java handled
  → mFinishQueue
  → Dispatcher lookup/erase/release
```

第二次加入三个分叉：

```text
batch seq chain
Key fallback restart
broken/unregister drain
```

第三次再审异常边界：

```text
compat replacement/null
Java callback exception direct-send
ERROR/HANGUP/dispose
ANR cancel vs drain
```

### 最终自测标准

如果看到一条 `wait=4xxx ms` 的事件，能够不猜测地列出至少四个可能停点，并知道下一步应查哪端；如果看到 `WAIT_FOR_FINISHED` 成功，能够主动说明它不等于 handled=true；如果看到 fallback，知道同一 seq 会经历第二轮 publish——就已经真正掌握本章。

### 下一章

下一章进入 [189 Android InputState 与取消事件合成](189-AndroidInputState与取消事件合成.md)：追每个 connection 怎样记录 key/motion memento，怎样拒绝不一致序列，又怎样在设备 reset、焦点变化、窗口移除和 ANR 时合成 canceled Key、Motion CANCEL 与 HOVER_EXIT。
