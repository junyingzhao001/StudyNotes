# 216 Android Choreographer、DisplayEventReceiver、VSync 与 FrameInfo 时间线

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只能静态核对 AOSP：可以证明请求、队列、线程切换与时间字段怎样更新；不能据此声称某台设备采用了哪组 phase offset，也不能把 HWUI 的 `FrameCompleted` 当成真实面板显示时刻。

第 215 章已经把 App Buffer 追到 SurfaceFlinger、HWC `present()` 与 present fence。本章往前补另一半：一次 UI 工作为什么会被合并到某个 app VSync，`doFrame()` 又怎样把同一批工作交给 UI 与 RenderThread 两条会重新并行的路线。

本章只追一个问题：**一个已 attach 的普通 Activity ViewRoot 发出本轮 traversal 请求后，怎样经过单次 app VSync 进入五类 callback，并留下只能覆盖到 HWUI、不能替代 present 证据的 FrameInfo？**

## 1. 固定一帧，用二十四个完成点拆开“收到 VSync 就画完”

先固定 `F_ui`，否则 Java/SF source、延时 callback、刷新率切换与 RT 非 UI 线程动画会被误拼成一条并不存在的总线。

| 维度 | 固定值或前提 |
|---|---|
| ViewRoot | 普通 Activity 主窗口，已 attach；只观察主线程一个 ViewRoot，入口前 `mTraversalScheduled=false`，本轮需要一次可见硬件绘制 |
| Java 节拍 | `Choreographer.getInstance()`，`VSYNC_SOURCE_APP`；`USE_VSYNC=true`、`USE_FRAME_TIME=true`、`mFPSDivisor=1` |
| callback | 目标TRAVERSAL是首笔立即到期工作并负责arm本帧；其余四类各一笔均在 `F_arm`后、`V_choose`前入队；均不取消、不抛异常 |
| Display | primary internal display 已连接且工作；无 config/hotplug、screen-off synthetic VSync、driver-stall fake VSync 或 phase 切换 |
| 请求现场 | 初始 `mFrameScheduled=false`、`mWaitingForVsync=false`、connection为 `None`；EventThread与该receiver的BitTube没有更老VSync；请求在下一次app-phase事件前被SF执行 |
| 传输 | JNI 初始化、Binder oneway 提交、EventThread `postEvent()` 与 App fd 读取全部成功；无重复 pending Java VSync |
| 时间 | event timestamp 不在未来，当前 Java `mFrameIntervalNanos` 与所选显示周期一致，`doFrame()` 不走倒退或 FPS-divisor 重试 |
| HWUI | ThreadedRenderer 使用 SkiaGL 且 surface 有效；`prepareTextures=true`、`canDrawThisFrame=true`、非空帧、需要且成功 swap/queue |

二十四个完成点如下。点名是本章的诊断坐标，不是 AOSP 自带 trace 名。

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `T_barrier` | `scheduleTraversals()` 已置 `mTraversalScheduled=true`，并取得 sync barrier token | traversal callback 已执行 |
| `F_arm` | TRAVERSAL `CallbackRecord` 已入队，`scheduleFrameLocked()` 已置 `mFrameScheduled=true` | native 请求已到 SF |
| `N_submit` | `requestNextVsync()` 的 oneway Binder 事务已交给驱动 | SF 的 `EventThreadConnection` 已改成 `Single` |
| `N_wait` | App native `DisplayEventDispatcher` 已置 `mWaitingForVsync=true` | `S_single` 必然在它之后 |
| `S_single` | SF EventThread 已把该 connection 的 `None` 改成 `Single` | app-phase event 已产生 |
| `V_emit` | `DispSyncSource` 已回调，EventThread 已把带 timestamp/count 的 VSync 放入 pending events | 该 connection 已被选中 |
| `E_take` | EventThread 已为该 connection 选择事件，并把 `Single` 清回 `None` | BitTube 写入成功 |
| `B_write` | `postEvent()` 已把事件成功写入该 connection 的 BitTube | App Looper 已读 fd |
| `L_dispatch` | App Looper 已 drain fd、清 `mWaitingForVsync`，JNI 已回调 Java `dispatchVsync()` | `doFrame()` 已开始 |
| `M_post` | Java `onVsync()` 已保存 timestamp/count，并投递异步 frame Message | Message 已被 dispatch |
| `D_enter` | `FrameDisplayEventReceiver.run()` 已清 pending 标志并进入 `doFrame()`，且通过 `mFrameScheduled` 门 | 本次时间已被接受 |
| `V_choose` | jitter/倒退/FPS 门均通过，FrameInfo 已写 intended/used VSync，`mFrameScheduled=false` | 任一 callback 已完成 |
| `I_done` | 本帧到期 INPUT callbacks 已完成 | ANIMATION 已执行 |
| `A_done` | 本帧到期 ANIMATION callbacks 已完成 | INSETS_ANIMATION 已执行 |
| `X_done` | 本帧到期 INSETS_ANIMATION callbacks 已完成 | ViewRoot traversal 已进入 |
| `T_run` | 目标 `doTraversal()` 已移除 barrier，并进入 `performTraversals()` | draw 已开始 |
| `D_mark` | `ThreadedRenderer.draw()` 已写 `DrawStart`，开始更新 root DisplayList | UI 的 9 项已交给 native |
| `S_queue` | JNI 已复制 UI 侧 9 项，`DrawFrameTask` 已写 `SyncQueued` 并把任务投给 RenderThread | RT 已开始同步 |
| `R_sync` | RT 已导入 UI 9 项、写 `SyncQueued`，并记录 `SyncStart` | UI 已从同步等待恢复 |
| `U_release` | 固定的 `prepareTextures=true` 分支已 signal UI 线程继续 | UI COMMIT 或 RT draw 谁先完成 |
| `C_done` | UI 已从 traversal 返回并完成到期 COMMIT callbacks | `FrameCompleted` 已写入 |
| `R_issue` | RT 已记录 `IssueDrawCommandsStart` 并进入 draw/swap 工作 | Buffer 已 queue 成功 |
| `Q_return` | pipeline 已先写 `SwapBuffers` marker，随后固定的 swap/queue 已成功返回 | GPU、SF latch 或 present 已完成 |
| `R_frame` | RT 已写 `FrameCompleted`，并执行本帧 JankTracker/FrameMetrics 上报 | acquire fence、present fence 或像素扫描已完成 |

固定主干不是一条覆盖所有线程的直线，而是：

```text
T_barrier < F_arm < N_submit
N_submit < N_wait
N_submit < S_single
S_single < V_emit < E_take < B_write < L_dispatch < M_post
N_wait < L_dispatch
M_post < D_enter < V_choose < I_done < A_done < X_done
X_done < T_run < D_mark < S_queue < R_sync < U_release
U_release < C_done
U_release < R_issue < Q_return < R_frame
```

`N_wait` 与 `S_single`没有严格先后。前者是 sender 在 oneway 调用返回后写的本地标志；远端 Binder 线程可能已经开始执行，也可能稍后才执行。类似地，`U_release`之后 UI 与 RT 分叉：`C_done`可以早于或晚于 `R_issue`、`Q_return`、`R_frame`，不能把源码中相邻的“UI 调 RT”画成“RT 全部画完后 UI 才 COMMIT”。

## 2. 四个执行语境与两种 Choreographer，不共享一只“帧对象”

| 执行语境 | 关键对象 | 本章责任 |
|---|---|---|
| App UI Looper | Java `Choreographer`、`FrameDisplayEventReceiver`、`ViewRootImpl` | 合并 callback、选择 frameTime、执行五阶段与 traversal |
| App UI Looper 的 native poll | `NativeDisplayEventReceiver`、`DisplayEventDispatcher` | 监听 BitTube fd、drain event、回调 Java |
| surfaceflinger | Binder worker、app `EventThread`、`DispSyncSource` | 接受 Single 请求、按需启停 source、选择并写回 VSync event |
| App RenderThread | `DrawFrameTask`、`CanvasContext`、native `AChoreographer` | 同步 DisplayList、提交图形工作、维护 RT 动画与 HWUI FrameInfo |

Java `Choreographer` 是 `ThreadLocal`，不是进程全局单例。普通 `getInstance()`要求当前线程已有 Looper，并构造 `VSYNC_SOURCE_APP` 实例；同一进程的其他 Looper 线程可以有自己的对象和 connection。

`getSfInstance()`只是让调用它的 Java Looper 连接 SF-source EventThread。它会被窗口动画等 Java 代码使用，但 SurfaceFlinger 的 native 主线程并不是靠这只 Java 对象运行。SF 自己的 MessageQueue/Scheduler 路线必须另算。

HWUI RenderThread 又有独立的 native `AChoreographer`。它在 r48 也使用 app source，却有自己的 fd、callback 容器和请求门；它与 UI Java Choreographer既不共享 `mFrameScheduled`，也不共享 `FrameInfo` 对象。

```text
UI Java Choreographer ─┐
RT AChoreographer ─────┼─ 各自 connection ─ app EventThread ─ primary DispSync模型
Java getSfInstance() ──┘                    或 sf EventThread
```

共享节拍模型不等于共享 callback、frame count、Buffer frame number，更不等于三个线程在同一纳秒同时开始。

## 3. ViewRoot 先放 barrier，再把一次 traversal 变成帧需求

普通 `requestLayout()`或 `invalidate()`最终可能令 ViewRoot 到达 `scheduleTraversals()`；两条 API 的上游脏区与 layout 语义不同，本章只从共同入口开始：

```java
if (!mTraversalScheduled) {
    mTraversalScheduled = true;
    mTraversalBarrier = mHandler.getLooper().getQueue().postSyncBarrier();
    mChoreographer.postCallback(
            Choreographer.CALLBACK_TRAVERSAL, mTraversalRunnable, null);
    notifyRendererOfFramePending();
}
```

这里有三层不能互换：

1. `mTraversalScheduled`只合并这个 ViewRoot 的 traversal；另一个 ViewRoot 有自己的标志。
2. sync barrier 暂停普通同步 Message，让异步输入/帧 Message穿过；它不是持有到 draw 结束的 Java 锁。
3. `postCallback()`把 `mTraversalRunnable`放入 Choreographer 的 TRAVERSAL 队列，真正运行还要等 `doFrame()`到该阶段。

`doTraversal()`先清 `mTraversalScheduled`并移除 barrier，随后才进 `performTraversals()`。因此 traversal 内再次请求布局可以为下一轮重新设 barrier；当前 barrier 不覆盖整段 measure/layout/draw。

`unscheduleTraversals()`也会清标志、移除 barrier并删除 callback，但它不会反向保证 Choreographer 已取消整个 frame request。`removeCallbacksInternal()`并不把 `mFrameScheduled`清零；同一 Choreographer 的其他 callback 也可能仍需要这次 VSync。

`notifyRendererOfFramePending()`位于 callback 投递之后，只通知 ThreadedRenderer 将有新帧，以调整非 UI 线程动画调度。它没有传递新 DisplayList，也没有替代后面的 `syncAndDrawFrame()`。

## 4. 五个 CallbackQueue 与三层防重，各自只合并自己的债

Choreographer 有五个按 due time 排序的单链表；相同 due time 的记录保持插入顺序：

| 队列 | 索引 | 典型工作 |
|---|---:|---|
| INPUT | 0 | batched input 消费 |
| ANIMATION | 1 | `FrameCallback`与动画 |
| INSETS_ANIMATION | 2 | 汇总 Insets animation progress |
| TRAVERSAL | 3 | ViewRoot measure/layout/draw |
| COMMIT | 4 | traversal 后的提交型 callback |

`postFrameCallback()`实际把带 `FRAME_CALLBACK_TOKEN` 的记录放入 ANIMATION；token决定运行时调 `FrameCallback.doFrame(frameTimeNanos)`，普通记录则执行 `Runnable.run()`。

立即到期的 callback 会调用 `scheduleFrameLocked(now)`。延时 callback 先排异步 `MSG_DO_SCHEDULE_CALLBACK`，到期后再看该类队首是否 due；它不会从投递时起永久订阅 VSync。

第一层防重是 ViewRoot 的 `mTraversalScheduled`；第二层是 Java Choreographer 的 `mFrameScheduled`；第三层是 native dispatcher 的 `mWaitingForVsync`。SF connection 还有 `None → Single`门。它们的 key 与完成点不同，不能压成一个“已经调度”布尔值。

当前线程就是绑定 Looper 时，`scheduleFrameLocked()`直接向下请求；从其他线程投递时，它发一个异步 `MSG_DO_SCHEDULE_VSYNC`到队首。队首仍是 MessageQueue 位置，不会在调用线程直接跨过去操作 receiver。

删除最后一笔 callback 不会自动撤销已经 armed 的 Single。事件回来后，`doFrame()`仍可清 `mFrameScheduled`并发现五个队列都没有到期工作。这是一次空的 Choreographer dispatch，不应误报成 ViewRoot draw。

构造也有条件分支：只有 `USE_VSYNC`为真才创建 `FrameDisplayEventReceiver`；关闭 `debug.choreographer.vsync`时改用异步 `MSG_DO_FRAME`与 `sFrameDelay`。固定主线不走这个调试 fallback。

r48 Java `mFrameIntervalNanos`在构造时从默认 display 当前 mode 计算；本类中没有刷新率变更回调去重写它。动态刷新现场不能当然假设 Java jitter 门已同步到最新周期。

## 5. Java 到 native：fd 是回程，oneway Binder 只是请求提交

`DisplayEventReceiver`构造把 Java weak reference、MessageQueue、source 与 config 策略交给 JNI。`NativeDisplayEventReceiver`继承 `DisplayEventDispatcher`，初始化时把 receiver fd 注册进同一 native Looper：

```cpp
mLooper->addFd(mReceiver.getFd(), 0,
        Looper::EVENT_INPUT, this, nullptr);
```

native GUI `DisplayEventReceiver`先同步创建 SF connection并 `stealReceiveChannel()`；以后有两条不同通道：

```text
控制去程：App → IDisplayEventConnection.requestNextVsync() → SF
事件回程：SF EventThread → BitTube send fd → App Looper receive fd
```

`scheduleVsync()`在 `mWaitingForVsync=false`时先调用 `processPendingEvents()`。这次 drain 会处理 hotplug/config，并把旧 VSync 读掉、只记录一条错误日志；它不会把这条旧 VSync再 `dispatchVsync()`给 Java。随后才发新请求并置 `mWaitingForVsync=true`。

`IDisplayEventConnection::requestNextVsync()`使用异步 Binder 调用。于是 `N_submit`只证明事务已提交，Java/native 调用栈返回不包含 SF `S_single` 的 reply；`N_wait`与 `S_single`可以并发交错。

`mWaitingForVsync`也只是单个 native receiver 的本地门。它成功挡住同一 receiver 重复下发，但不能证明远端 event 一定会回来。初始化失败会抛运行时异常；请求函数报错也会由 JNI抛异常，而成功返回仍不等于 EventThread 已执行。

## 6. EventThread 消费 Single，再借同一 primary DispSync 分出 app/SF phase

SF 侧请求入口只在 connection 当前为 `None`时改成 `Single`：

```cpp
if (connection->vsyncRequest == VSyncRequest::None) {
    connection->vsyncRequest = VSyncRequest::Single;
    mCondition.notify_all();
}
```

重复 Single 不会累积成“欠两次”；Periodic 等非 `None`模式也不会被这次请求改写。`requestNextVsync()`还会先触发 resync callback，这与 connection 是否最终从 None 改成 Single 是两步。

EventThread 只要发现任一 connection 有 VSync 需求，就进入 VSync state并启用自己的 `DispSyncSource`。VSync event 到来时，`shouldConsumeEvent()`为目标 connection 执行：

```text
Single → None → 选择本事件
```

清债发生在 `postEvent()`之前。若 BitTube 已满返回 `-EAGAIN`，r48 这里只记录失败，没有把刚清掉的 Single恢复或立即重试；固定帧明确排除该分支。

SurfaceFlinger 创建独立的 app 与 sf connection handle，各自拥有 EventThread/DispSyncSource，但 `Scheduler::makePrimaryDispSyncSource()`都指向同一个 `mPrimaryDispSync`。不同的是 phase offset 与消费者：

```text
primary DispSync model
  ├─ app DispSyncSource(offset = current app phase) → app EventThread
  └─ sf  DispSyncSource(offset = current sf phase)  → sf  EventThread
```

offset 可随 refresh rate、early/late 状态和 VSyncModulator 更新；启用中的 listener可 `changePhaseOffset()`。因此不能背一组固定纳秒，也不能说 app/SF来自两块硬件 VSync。

EventThread 的 `count`是该 EventThread `VSyncState`递增的计数，synthetic/fake event也会递增。它不是 Choreographer 私有序号、BufferQueue frame number或 SurfaceControl transaction id。

r48 VSync event还携带 `expectedVSyncTimestamp`，但 `DisplayEventDispatcher::processPendingEvents()`交给 JNI 的是 event header 的 timestamp、display id与count；Java这条签名没有接收该 expected 字段。两者不要在图里悄悄合成一个时间点。

## 7. BitTube 唤醒 App 后，Java 仍要再排一个异步 Message

fd 可读时，`handleEvent()`循环 drain 所有 event。hotplug/config各自 dispatch；若同批有多个 VSync，后一个覆盖前一个，最终只分发最新值。此处与第 5 节发请求前的 drain 不同：这里会清 `mWaitingForVsync=false`并真正调用 `dispatchVsync()`。

JNI通过全局 weak reference找到 Java receiver；对象仍存活才调用私有 `dispatchVsync()`，再进入 `FrameDisplayEventReceiver.onVsync()`。固定帧 receiver 未 dispose、weak referent 有效。

Java先检查 timestamp：若它大于当前 `System.nanoTime()`，就把它 clamp 到 now。若已经有 pending frame Message，代码会警告、覆盖保存的 timestamp/count，却仍再投递一个 Message；`mHavePendingVsync`是异常检测标志，不是一个严格“只入队一次”的门。

因此 `doFrame()`里的 intended time 最多是“Java 最终保存并交给本次 run 的 event time”。正常固定分支下它等于 SF header timestamp；future clamp 或 pending overwrite 时，不能再称为未经处理的原始 SF 值。

```java
Message msg = Message.obtain(mHandler, this);
msg.setAsynchronous(true);
mHandler.sendMessageAtTime(msg,
        timestampNanos / TimeUtils.NANOS_PER_MS);
```

这一步解释了两个现象：

- VSync fd 已分发不等于 `doFrame()`已进入；时间更早的 Message仍可先运行。
- frame Message 的 asynchronous 标志允许它穿过 ViewRoot sync barrier，否则触发 traversal 的门会把自己的节拍挡住。

r48 Java注释还限定 `scheduleVsync()`隐式请求 internal display，`onVsync()`里的 physical display id没有参与 Choreographer选择。多屏不能从这条主线外推。

## 8. `doFrame()`先选逻辑帧时间，再把 frame request 清账

frame Message 运行先清 `mHavePendingVsync`，再调用 `doFrame(savedTimestamp, savedCount)`。开头的 `!mFrameScheduled`用于丢弃已经被另一条 frame dispatch 消费的 stale/duplicate Message；删除 callback 本身并不会清这个标志。

时间账如下：

```java
long intended = frameTimeNanos;
long start = System.nanoTime();
long jitter = start - frameTimeNanos;
if (jitter >= interval) {
    long skipped = jitter / interval;
    long remainder = jitter % interval;
    frameTimeNanos = start - remainder;
}
```

固定 16 ms 示例中，event time为100 ms、App到137 ms才开始：

```text
jitter = 37 ms
skipped = 2
remainder = 5 ms
used Vsync = 137 - 5 = 132 ms
```

`IntendedVsync`保存100 ms，`Vsync`保存132 ms。jitter发生在本帧 GPU工作提交前，可能来自主线程旧任务、调度或CPU竞争，不能直接命名为GPU耗时。

若修正后时间小于 `mLastFrameTimeNanos`，或 FPS divisor要求继续跳过，本次不执行 callback，而是再请求 VSync；这两条返回都没有清 `mFrameScheduled`。只有时间被接受后才执行：

```java
mFrameInfo.setVsync(intended, frameTimeNanos);
mFrameScheduled = false;
mLastFrameTimeNanos = frameTimeNanos;
```

从此本帧 callback中再投递工作可以 arm 下一次 Single。`Skipped N frames`日志默认要达到30帧才出现；没有日志并不证明 intended与used相同，日志文案也不是根因鉴定。

Java间隔只在构造时取值，所以变刷新率切换窗口里，skipped/remainder/FPS-divisor计算还要结合该对象当时的 `mFrameIntervalNanos`，不能只看屏幕当前宣称的 Hz。

## 9. 五阶段按需摘队列，COMMIT 改时不回写 FrameInfo

时间被接受后顺序固定：

| 顺序 | 动作 | FrameInfo marker |
|---:|---|---|
| 1 | INPUT callbacks | 进入前写 `HandleInputStart` |
| 2 | ANIMATION callbacks | 进入前写 `AnimationStart` |
| 3 | INSETS_ANIMATION callbacks | 无独立 marker，共享 Animation→Traversal 区间 |
| 4 | TRAVERSAL callbacks | 进入前写 `PerformTraversalsStart` |
| 5 | COMMIT callbacks | 无独立 marker |

每个 `doCallbacks(type)`到达时才用当前 `System.nanoTime()`提取该类 due 前缀，不是在 `doFrame()`开头把五个队列一次性冻结。因此 INPUT里零延时投递的ANIMATION可以赶上本帧；TRAVERSAL里新投递的INPUT不会回头，只能等后续 frame。

这里还有一笔容易漏掉的请求：`V_choose`已把 `mFrameScheduled`清成false。早期阶段投递后续阶段 callback时，`postCallback()`既可能让它赶上本帧，又会同时 arm 下一次 VSync；即使本帧后来把记录消费掉，代码也不会自动撤销新请求，下一次可能只是空 dispatch。

COMMIT 的二次校正只有在 COMMIT 队列确有到期 callback时才会执行；队列为空会在进入校正代码前返回。若到该阶段已晚至少两个 interval，它把 `doCallbacks()`的局部 `frameTimeNanos`改到“至少落后一周期”的位置，并更新 `mLastFrameTimeNanos`。

这次校正不会：

- 回写已经由 `setVsync()`写入的 Java `FrameInfo[VSYNC]`；
- 改变 `doFrame()`外层早先锁定的 `AnimationUtils` clock；
- 让 INPUT、ANIMATION、INSETS或TRAVERSAL重跑。

COMMIT记录走普通 `Runnable`，`CallbackRecord.run(frameTimeNanos)`不会把参数传给它；但Runnable内部调用 `getFrameTimeNanos()`或 `getLastFrameTimeNanos()`时，可以观察到已更新的 `mLastFrameTimeNanos`。同一帧 FrameMetrics 的 Vsync 字段仍是 `V_choose`时写入的 used VSync。

callback异常会经过 finally恢复 callback池、`mCallbacksRunning`与动画时钟，但未捕获的业务异常仍向主线程传播，后续阶段不保证执行。固定帧排除异常。

## 10. `syncAndDrawFrame()`只等到可放行 UI，不总等 RT 完成本帧

目标 TRAVERSAL进入 `doTraversal()`后，barrier已经移除。`performTraversals()`完成本次各 gate 实际要求的 traversal 工作并走硬件绘制时，`ThreadedRenderer.draw()`先写 `DrawStart`，更新 root DisplayList，再调用 `syncAndDrawFrame(FrameInfo)`；并非每一轮都必然重做 measure 与 layout。

JNI检查数组长度必须是9，并把当前 Java数组复制到 `DrawFrameTask`自己的buffer。UI进入 `drawFrame()`后记录 `SyncQueued`、把任务post给RenderThread并等待条件变量。

RT执行 `syncFrameState()`：

1. 把 UI传来的 Vsync交给 `TimeLord::vsyncReceived()`；只有它比已保存frame time更新时才真正更新；
2. `CanvasContext.prepareTree()`导入0..8、写 `SyncQueued`、记录 `SyncStart`；
3. 同步RenderNode树、纹理与动画状态；
4. 返回 `info.prepareTextures`作为能否提前放行UI的条件。

固定 `prepareTextures=true`时，RT先 `unblockUiThread()`，再进入 `CanvasContext.draw()`：

```text
                         ┌─ UI醒来 → syncAndDrawFrame返回 → traversal结束 → COMMIT
R_sync → U_release ──────┤
                         └─ RT继续 → R_issue → Q_return → R_frame
```

signal动作先于两条后继，但两个线程实际谁跑得更快没有保证。COMMIT日志早于 `FrameCompleted`很正常，反过来也可能；这不是“同一帧时序损坏”。

若 `prepareTextures=false`，RT会把 UI阻塞到 draw或fence等待之后才放行；若 `canDrawThisFrame=false`，它可能只等fence而不走正常 draw。正因为分支会改变偏序，本章把两者固定为true。

`notifyRendererOfFramePending()`与这里也不能合并：前者早在 scheduleTraversal时通知RT可能有非 UI 线程动画，后者才携带本次UI DisplayList与FrameInfo快照。

## 11. FrameInfo 是1个flags加16格混合数据，不是17个同类时间点

Java只创建9个long；native保留17格，前9项按索引复制：

| 索引 | 名称 | 类型与写入边界 |
|---:|---|---|
| 0 | Flags | 位集合；不是timestamp |
| 1 | IntendedVsync | Java最终采用为intended的event time |
| 2 | Vsync | jitter修正后给动画/绘制使用的时间 |
| 3 | OldestInputEvent | 当前记账窗口内最早input event time；无值时初始为 `Long.MAX_VALUE` |
| 4 | NewestInputEvent | 当前记账窗口内最新input event time；无值时初始为0 |
| 5 | HandleInputStart | INPUT阶段marker |
| 6 | AnimationStart | ANIMATION开始marker；其后还包含Insets阶段 |
| 7 | PerformTraversalsStart | Choreographer开始TRAVERSAL队列的marker |
| 8 | DrawStart | ThreadedRenderer开始View draw/DisplayList更新的marker |
| 9 | SyncQueued | UI把DrawFrameTask排给RT前的时间 |
| 10 | SyncStart | RT导入UI数据并开始同步的时间 |
| 11 | IssueDrawCommandsStart | `CanvasContext::draw()`已完成damage收束并通过空帧跳过门后、调用`getFrame()`前的marker；后面还含dequeue、dirty计算、pipeline draw、fence等待与swap前工作 |
| 12 | SwapBuffers | pipeline尝试swap前写的marker |
| 13 | FrameCompleted | `CanvasContext.draw()`在swap尝试后写的CPU账本点 |
| 14 | DequeueBufferDuration | duration，不是绝对时间 |
| 15 | QueueBufferDuration | duration，不是绝对时间 |
| 16 | GpuCompleted | r48稍后用frame timestamp的正 `acquireTime`回填；无有效值为-1 |

因此更准确的说法是“Java 9项（1 flags + 8时间值）复制到native 17项”，而不是“9个时间点扩展成17个时间点”。

`setVsync()`会重置flags与input两格。`doProcessInputEvents()`只在pending队列非空时，随 `while` 每取一笔event调用一次 `updateInputEventTime()`；空队列不会更新，一次调用也可能更新多次。只有发生在本次reset之后、UI数组被JNI复制之前的更新能进入这个帧快照。立即输入若在 `doFrame()`前已处理，会被下一次reset覆盖；多 ViewRoot共享同一 Choreographer时，input与marker也不天然只属于一棵树。固定主线进一步约束：只有目标root在既定INPUT callback中经 `doConsumeBatchedInput()`、`doProcessInputEvents()`消费指定batched input并更新这两格，期间没有其他root更新，也没有callback外的immediate input处理。

`SwapBuffers`字段记录调用前 marker，不是 `Q_return`。而 `FrameCompleted`在r48是swap尝试后的 `systemTime()`；源码没有在这里等一只“本帧GPU完成fence”。即使其他分支无需swap或swap失败，这两个marker仍可能被写入。只有固定的 requireSwap+成功条件才允许把 `Q_return`放在二者之间。

`GpuCompleted`也不是当场由GPU callback直接写。`CanvasContext`观察四帧前记录，调用 `native_window_get_frame_timestamps()`取 `outAcquireTime`，正值才回填并交给 `finishGpuDraw()`；`gpuDrawTime()`再计算 `GpuCompleted - SwapBuffers`。字段值代表这条r48路径延后取得的acquire-fence signal time，写入动作则晚得多，不能拿字段名或该差值冒充HWC present或面板扫描证据。

## 12. FrameMetrics 是公开差值视图，TOTAL 与各分项之和存在 r48 缝隙

公开 `FrameMetrics.getMetric()`按固定起止索引直接相减：

| Metric | 起点 → 终点 |
|---|---|
| UNKNOWN_DELAY | IntendedVsync → HandleInputStart |
| INPUT | HandleInputStart → AnimationStart |
| ANIMATION | AnimationStart → PerformTraversalsStart |
| LAYOUT_MEASURE | PerformTraversalsStart → DrawStart |
| DRAW | DrawStart → SyncQueued |
| SYNC | SyncStart → IssueDrawCommandsStart |
| COMMAND_ISSUE | IssueDrawCommandsStart → SwapBuffers |
| SWAP_BUFFERS | SwapBuffers → FrameCompleted |
| TOTAL | IntendedVsync → FrameCompleted |

名称是观察口径，不是自动根因。例如 UNKNOWN_DELAY发生在GPU提交前；ANIMATION区间还包含没有独立marker的 Insets animation；SWAP_BUFFERS以 `FrameCompleted`为终点，却不等待GPU或present fence。

还有一个r48算术边界：分项从DRAW停在 `SyncQueued`，下一项SYNC却从 `SyncStart`开始，因此 UI等待RT的 `SyncStart - SyncQueued`空档没有落入八个阶段分项；TOTAL直接跨过去：

```text
TOTAL = FrameCompleted - IntendedVsync
八个阶段分项之和 = TOTAL - (SyncStart - SyncQueued)
```

在gap为正时，源码注释中“TOTAL等于其他耗时项之和”与实际索引相减并不完全一致。不要为了让报表看起来闭合而擅自把gap塞进SYNC。

`FIRST_DRAW_FRAME`只读 flags；r48公开API不暴露14..16的dequeue、queue或GpuCompleted为独立metric。listener回调还带 `dropCountSinceLastInvocation`，丢报告与“该帧耗时为零”也是两件事。

最重要的终点边界仍是：FrameMetrics TOTAL到 `FrameCompleted`，没有跨越 SurfaceFlinger latch、HWC present return、可靠present fence signal或整屏扫描。

## 13. JankTracker 会调整 duration，它与公开 FrameMetrics 不是同一把尺

HWUI `FrameInfo::duration(start,end)`若区间从 `SyncQueued`之前跨到其后，会减掉正的 `SyncStart - SyncQueued`。注释认为这段RT stall会由前一帧账本体现。默认 `sFrameStart=IntendedVsync`；测试过滤属性可在reset后把它改成 `HandleInputStart`。于是普通默认配置下：

```text
JankTracker total
= FrameCompleted - IntendedVsync - max(SyncStart - SyncQueued, 0)
```

这与第12节公开FrameMetrics raw TOTAL不同。JankTracker令 `offsetDelta = sfOffset - appOffset`；只有它位于0…4 ms时才设置 `dequeueForgiveness = offsetDelta + 4 ms`。若实际dequeue超过500 µs且 `expected = dequeueForgiveness + Vsync - IssueDrawCommandsStart`为正，再扣 `min(expected, actual dequeue)`；不能用一个 `TOTAL_DURATION`值复算所有内部判定。

处理顺序是：

1. 计算经sync gap与条件性dequeue宽免调整的total，并上报frame histogram；
2. `SurfaceCanvas`帧在这里提前返回，不进入通用jank或类型统计；
3. 其余帧在 `totalDuration > frameInterval`时增加通用jank计数；
4. 必要时初始化swap deadline，先用推进前的值判断triple-buffered，再把deadline推进到下一候选边界；
5. 若 `FrameCompleted < mSwapDeadline || totalDuration < frameInterval` 则提前返回，只有triple-buffered条件成立才在这条支路记 `HighInputLatency`；
6. 未走早退才记 `MissedDeadline`、重置deadline并跑四类分段比较。

四类comparison如下：

| 类型 | 源码起止 | `duration()`后的实际含义 | 阈值 |
|---|---|---|---:|
| MissedVsync | IntendedVsync → Vsync | intended与used之差 | 1 ns |
| SlowUI | Vsync → SyncStart | 正常gap下等价 Vsync → SyncQueued | 0.5 interval |
| SlowSync | SyncStart → IssueDrawCommandsStart | RT同步段 | 0.2 interval |
| SlowRT | IssueDrawCommandsStart → FrameCompleted | RT绘制/交换CPU账本段 | 0.75 interval |

所以“某一段超过阈值”不保证每次都会记该type：四类循环位于 MissedDeadline之后。`HighInputLatency`又不在这张comparison表里，它来自triple-buffer/deadline分支，不是固定用 `OldestInputEvent`做一次相减。

单个comparison delta达到10秒会被排除在普通type归类之外，但frame duration、通用jank与超长帧日志仍有各自代码；不能概括成“整帧完全丢弃”。`SurfaceCanvas`是实现中的exempt flag，`WindowLayoutChanged`虽被公开文档描述为通常应豁免，却不在 r48 `EXEMPT_FRAMES_FLAGS`里。

## 14. RenderThread 的 AChoreographer 管 RT callback，不替代 UI→RT 同步

非isolated RenderThread初始化时：

```cpp
mChoreographer = AChoreographer_create();
AChoreographer_registerRefreshRateCallback(
        mChoreographer, RenderThread::refreshRateCallback, this);
mLooper->addFd(AChoreographer_getFd(mChoreographer), ...);
```

native `AChoreographer`内部同样继承 `DisplayEventDispatcher`，使用 `eVsyncSourceApp`，但它拥有RT自己的 connection。`RenderThread::requestVsync()`用 `mVsyncRequested`合并请求；callback到来先清该标志，只有TimeLord接受且 `mFrameCallbackTaskPending=false`，才按 `frameTime + mDispatchFrameDelay`安排RT frame callbacks。dispatch时又预先申请下一次VSync，服务连续RT动画。

这条路线服务注册成 `IFrameCallback` 的RT-driven RenderNode动画、animated-image推进和失败帧重试等工作。UI本帧的 `DrawFrameTask`则由 `syncAndDrawFrame()`直接post到RT queue，并不先等待RT自己的下一次AChoreographer callback。两条输入在同一RT上竞争，不能把它们画成一条重复调用链。

RT注册的refresh-rate callback会更新 `DeviceInfo`，再让 `setupFrameInterval()`更新TimeLord interval与四分之一周期的 `mDispatchFrameDelay`。这条回调没有调用既存 `JankTracker::setFrameInterval()`，所以不能顺带宣称旧tracker阈值也在此处更新。它与Java Choreographer只在构造时写 `mFrameIntervalNanos`形成r48实现差异。isolated process还会改用16 ms `DummyVsyncSource`，固定主线排除它。

最后把三组编号与终点分开：

| 证据 | 命名空间/终点 | 不能直接join到 |
|---|---|---|
| EventThread VSync count | 某EventThread的递增event count | BufferQueue frame number |
| FrameInfo | UI+RT的CPU账本先到FrameCompleted；GpuCompleted可在后续帧延迟回填 | HWC present fence signal |
| FrameMetrics | 公开差值视图到FrameCompleted | HWC present fence signal |
| Buffer frame number与frame timestamps | 某BufferQueue producer/consumer代际 | Choreographer callback序号 |

要证明像素展示，仍需第215章的Buffer、latch、Output、present与可靠fence证据；VSync callback只是给生产与合成安排机会。

## 15. 从症状找第一处分歧，并用九组只读练习自证

| 现象 | 先看哪两个点 | 还要排除 |
|---|---|---|
| traversal永远不跑 | `T_barrier → F_arm` | callback被remove、Looper错误、barrier未清 |
| 本地显示已请求却无VSync | `N_submit / N_wait → S_single` | oneway尚未执行、connection/dispose、display断开 |
| SF有event而App不醒 | `E_take → B_write → L_dispatch` | BitTube满/断、fd callback被移除 |
| App收到VSync却无UI工作 | `M_post → D_enter → V_choose` | duplicate/stale Message、time倒退、FPS divisor、队列已空 |
| intended与used差很大 | `D_enter → V_choose` | Java interval是否过期、主线程旧任务/调度延迟 |
| traversal完成但FrameCompleted晚 | `U_release`后的双分支 | RT前帧、纹理、GPU/queue阻塞；COMMIT不等待RT完成 |
| FrameMetrics分项加不成TOTAL | `S_queue → R_sync` | SyncQueued→SyncStart gap本来未分桶 |
| FrameCompleted正常但视觉仍卡 | `R_frame`之后 | 异步GPU/acquire fence、SF latch、HWC present、present fence与面板链 |

以下命令只读本地 `android-11.0.0_r48`；每个 `rg`备选都应独立命中，Bash 3.2与Zsh 5.9均可运行。

### 练习 1：证明 barrier、traversal callback 与 renderer 通知是三步

```bash
cd /Users/ninebot/androidSource
rg -n 'scheduleTraversals|mTraversalScheduled|postSyncBarrier|removeSyncBarrier|CALLBACK_TRAVERSAL|notifyRendererOfFramePending' \
  frameworks/base/core/java/android/view/ViewRootImpl.java
```

按源码顺序标出 `T_barrier`、callback投递、renderer通知与 `doTraversal()`移除barrier；说明哪个标志只属于一个ViewRoot。

### 练习 2：找到 callback due-time 队列与 Java frame request 门

```bash
cd /Users/ninebot/androidSource
rg -n 'sThreadInstance|VSYNC_SOURCE_APP|postCallbackDelayedInternal|addCallbackLocked|extractDueCallbacksLocked|scheduleFrameLocked|MSG_DO_SCHEDULE_VSYNC|mFrameScheduled' \
  frameworks/base/core/java/android/view/Choreographer.java
```

解释立即/延时、Looper内/Looper外四种投递怎样收敛，并验证remove callback为什么不会自动清整个frame request。

### 练习 3：对齐 `onVsync()`、Message 与 `doFrame()`选时

```bash
cd /Users/ninebot/androidSource
rg -n 'mHavePendingVsync|timestampNanos > now|sendMessageAtTime|doFrame\(mTimestampNanos|jitterNanos|skippedFrames|mFPSDivisor|mFrameInfo.setVsync' \
  frameworks/base/core/java/android/view/Choreographer.java
```

手算 `event=100 ms、start=137 ms、interval=16 ms`，再指出future clamp与pending overwrite为何会改变“Intended是raw timestamp”这句话。

### 练习 4：证明五阶段与 COMMIT 校正的真实边界

```bash
cd /Users/ninebot/androidSource
rg -n 'CALLBACK_INPUT|CALLBACK_ANIMATION|CALLBACK_INSETS_ANIMATION|CALLBACK_TRAVERSAL|CALLBACK_COMMIT|doCallbacks|extractDueCallbacksLocked|lastFrameOffset|mCallbacksRunning' \
  frameworks/base/core/java/android/view/Choreographer.java
```

标出三个而非五个FrameInfo阶段marker，并解释同帧插入、下一帧arm与COMMIT局部frameTime之间的关系。

### 练习 5：区分请求前丢旧event与fd到达后分发最新event

```bash
cd /Users/ninebot/androidSource
rg -n 'nativeInit|nativeScheduleVsync|dispatchVsync' \
  frameworks/base/core/jni/android_view_DisplayEventReceiver.cpp
rg -n 'addFd|scheduleVsync|processPendingEvents|mWaitingForVsync|handleEvent|requestNextVsync|dispatchVsync' \
  frameworks/native/libs/gui/DisplayEventDispatcher.cpp
```

分别画出两次 `processPendingEvents()`调用：哪一次只drain旧VSync，哪一次清waiting并回调Java。

### 练习 6：证明 Single 在写 BitTube 前已被消费

```bash
cd /Users/ninebot/androidSource
rg -n 'callRemoteAsync|REQUEST_NEXT_VSYNC' \
  frameworks/native/libs/gui/IDisplayEventConnection.cpp
rg -n 'requestNextVsync|VSyncRequest::Single|shouldConsumeEvent|postEvent|EAGAIN|setVSyncEnabled|mPendingEvents' \
  frameworks/native/services/surfaceflinger/Scheduler/EventThread.cpp
```

解释sender返回、SF执行、Single清零和BitTube成功写入四个完成点；再说明pipe满为何会丢掉这笔单次债。

### 练习 7：验证 app/SF 两个source共享 primary DispSync

```bash
cd /Users/ninebot/androidSource
rg -n 'mAppConnectionHandle|mSfConnectionHandle|createDisplayEventConnection|offsets\.late\.app|offsets\.late\.sf' \
  frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
rg -n 'makePrimaryDispSyncSource|addEventListener|changePhaseOffset|onVSyncEvent' \
  frameworks/native/services/surfaceflinger/Scheduler/{Scheduler.cpp,DispSyncSource.cpp}
```

画出两个EventThread、两个phase source与一个 `mPrimaryDispSync`，不要填入设备无关的固定offset数字。

### 练习 8：把 Java 9项与 native 17项逐格对齐

```bash
cd /Users/ninebot/androidSource
rg -n 'INTENDED_VSYNC|OLDEST_INPUT_EVENT|NEWEST_INPUT_EVENT|markInputHandlingStart|markAnimationsStart|markPerformTraversalsStart|markDrawStart' \
  frameworks/base/graphics/java/android/graphics/FrameInfo.java
rg -n 'UI_THREAD_FRAME_INFO_SIZE|SyncQueued|SyncStart|IssueDrawCommandsStart|SwapBuffers|FrameCompleted|DequeueBufferDuration|QueueBufferDuration|GpuCompleted|static_assert' \
  frameworks/base/libs/hwui/{FrameInfo.h,FrameInfo.cpp}
```

指出哪三格不是timestamp、哪两类callback没有独立marker，以及为什么17项长度必须与FrameMetrics同步。

### 练习 9：复算 UI/RT 分叉、FrameMetrics 与 JankTracker

```bash
cd /Users/ninebot/androidSource
rg -n 'mSyncQueued|postAndWait|unblockUiThread|prepareTextures' \
  frameworks/base/libs/hwui/renderthread/DrawFrameTask.cpp
rg -n 'markIssueDrawCommandsStart|markFrameCompleted|native_window_get_frame_timestamps|finishGpuDraw' \
  frameworks/base/libs/hwui/renderthread/CanvasContext.cpp
rg -n 'markSwapBuffers|Even if we decided to cancel|damageRequiresSwap|mEglManager.swapBuffers' \
  frameworks/base/libs/hwui/pipeline/skia/SkiaOpenGLPipeline.cpp
rg -n 'DURATIONS|TOTAL_DURATION|FRAME_STATS_COUNT' \
  frameworks/base/core/java/android/view/FrameMetrics.java
rg -n 'mDequeueTimeForgiveness|EXEMPT_FRAMES_FLAGS|kHighInputLatency|kMissedDeadline|COMPARISONS|IGNORE_EXCEEDING' \
  frameworks/base/libs/hwui/JankTracker.cpp
rg -n 'AChoreographer_create|registerRefreshRateCallback|mVsyncRequested|mDispatchFrameDelay|requestVsync' \
  frameworks/base/libs/hwui/renderthread/RenderThread.cpp
```

证明 `U_release`后UI COMMIT与RT绘制无全序，分别写出公开raw TOTAL和JankTracker调整后total，并说明refresh callback更新了哪些RT字段、没有更新哪个既存tracker阈值。

## 16. 结论：VSync分配机会，FrameInfo只结到HWUI账本

固定帧可以压缩为四句话：

```text
ViewRoot barrier + TRAVERSAL callback
→ Java/native/SF三层门合并成一笔Single app-VSync请求
→ BitTube fd与异步Message把event交回doFrame，五类callback按阶段取债
→ UI在RT同步后可先恢复；RT另行issue/swap并写FrameCompleted
```

最需要保留的四条边界是：

1. oneway请求返回不等于SF已置Single；Single清零又早于BitTube写成功。
2. Java `onVsync()`到 `doFrame()`隔着异步Message；Intended也可能已经过clamp或overwrite。
3. `U_release`后 UI COMMIT与RT FrameCompleted并行，没有固定先后。
4. FrameMetrics raw TOTAL、JankTracker adjusted total、GpuCompleted回填与可靠present fence是四套不同口径。

因此排查“掉帧”时，先问是哪一笔债没闭合：callback没入队、Single没送达、event没回来、主线程迟到、UI等RT、RT交换慢，还是Buffer已经离开HWUI后才在SF/HWC延迟。只有把完成点落到正确所有者，VSync、jank和“真正显示”才不会被一个模糊的“这一帧”混成同义词。
