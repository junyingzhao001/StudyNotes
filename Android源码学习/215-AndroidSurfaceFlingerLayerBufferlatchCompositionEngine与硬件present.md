# 215 Android SurfaceFlinger Layer、Buffer latch、CompositionEngine 与硬件 present

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只能静态核对 AOSP：可以证明 Buffer 怎样进入 SurfaceFlinger、Layer 怎样 latch、CompositionEngine 怎样组织输出以及 fence 怎样传递；不能据此声称某台设备实际选择了哪种合成类型，也不能给出真实面板时延。

第 214 章停在 `Q_accept`：App RenderThread 已把首个窗口 Buffer 交给 producer 队列，WMS 的 show 控制事务则从另一条支线到达 SurfaceFlinger。现场最容易误判的一句话是“Buffer 已提交，所以首帧上屏了”。这中间至少还隔着队列通知、SF VSync、状态 commit、Buffer latch、可见性计算、HWC 策略协商、显示提交和 present fence signal。

本章只追一个问题：**一块已 queue 的普通窗口 Buffer，怎样与 show 状态在 SurfaceFlinger 汇合，成为某个 Display 的合成输入，并最终取得可归因的 present 证据？**

## 1. 固定一帧，用二十二个完成点拆开“上屏”

先固定 `F_target`，否则不同 Layer 类型、HWC 快慢路径与多 Display 会把一条主线撕成许多条件分支。

| 维度 | 固定值或前提 |
|---|---|
| 上游 | 沿用第 214 章普通首次可见 Activity 主窗口；r48 默认非 BLAST，目标内容进入 SF 侧 `BufferQueueLayer` |
| Buffer | 第一块业务Buffer；非shared、非droppable；`onFrameAvailable()`在 `handlePageFlip()`冻结候选前完整返回；时间戳严格早于本轮 `expectedPresentTime`；acquire fence在latch前已signal |
| 控制面 | WMS的show/alpha/crop等事务已ready，并在所选INVALIDATE前写入目标Layer current state，但尚未被更早的transaction pass提交到drawing；由所选INVALIDATE执行该commit；无并发hide、remove、resize、reparent或后继Buffer替换 |
| latch | 无 pending refresh、deferred sync point、reject、sideband、auto-refresh 或 `updateTexImage()` 错误；目标层在线上 Layer 树且未被完全遮挡 |
| Output | 已启用的 primary internal display，power mode 为 ON；无 hotplug、配置切换、VR handoff 或 backpressure early return |
| 策略 | SF条件已令另一个可见层在validate前成为CLIENT候选；validate返回混合方案，SF暂存accept命令并应用本地最终视图；目标窗口层为DEVICE；present批次执行成功 |
| client target | dequeue、RenderEngine draw、queue、FramebufferSurface acquire、`setClientTarget()`命令暂存及present时批量执行全部成功 |
| present | HWC present与release-fence查询成功，返回有效且实现未声明不可靠的 present fence；signal 前没有另一帧替换目标内容 |

二十二个完成点如下：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `Q_accept` | `BufferQueueProducer` 已把目标 slot 改成 QUEUED，并把 `BufferItem` 放入真实队列 | producer调用已返回，或SF已收到回调 |
| `Q_notice` | `BufferQueueLayer::onFrameAvailable()` 已把目标项加入 SF shadow queue、增加 `mQueuedFrames`并请求 layer update | 主线程已开始 INVALIDATE |
| `W_apply` | show等控制 patch 已写入目标 Layer current state并请求 transaction | 已进入 drawing state，或已有可见输出 |
| `I_enter` | SF主线程进入所选 `onMessageInvalidate(expectedVSyncTime)` | 前一帧背压门已通过 |
| `T_commit` | Layer内部状态与全局 Layer 树均已从请求态提交到本帧 drawing 视图 | Buffer已被 consumer acquire |
| `K_select` | `handlePageFlip()`已把目标层冻结进本轮 `mLayersWithQueuedFrames` | latch一定成功 |
| `L_acquire` | `updateTexImage()`已令真实 BufferQueue consumer acquire 目标项 | 它已成为CompositionEngine读取的活动Buffer |
| `L_active` | `updateActiveBuffer()`已把目标 GraphicBuffer、slot与fence装入 `mBufferInfo` | Layer可见或已交给HWC |
| `D_mark` | 首Buffer/几何变化已触发bounds重算，并把对应Output damage与后续geometry更新标脏 | 每个Output的visible region已计算 |
| `R_post` | INVALIDATE确认有刷新工作，`signalRefresh()`已投递/合并REFRESH | CompositionEngine已开始 |
| `E_pre` | `CompositionEngine::preComposition()`已完成，开始逐Output准备geometry | 目标层一定生成OutputLayer |
| `O_visible` | primary Output已经为目标LayerFE保留可见 `OutputLayer` | 合成类型已经最终确定 |
| `H_write` | 目标OutputLayer的候选状态、Buffer与acquire fence命令已写入Composer command stream | 命令已执行，或最终类型已经反映到SF本地视图 |
| `V_stage_apply` | validate结果已读取，`ACCEPT_DISPLAY_CHANGES`已写入Composer command stream，Display已把类型/请求应用到本地最终视图 | 厂商Composer已执行accept命令，或CLIENT内容已画好 |
| `C_lease` | SF已为client target取得可写Buffer及旧内容的fence依赖 | RenderEngine已完成写入 |
| `C_draw` | `drawLayers()`已成功返回；有效ready fence表示绘制完成条件，`NO_FENCE`则表示同步finish后已可读 | client target已交给HWC |
| `C_stage` | FramebufferSurface已调用 `setClientTarget()`，client target及acquire fence命令写入ComposerHal command writer | 命令已送达厂商Composer实现 |
| `P_call` | 固定慢路径进入 `Composer::presentDisplay()`，追加PRESENT_DISPLAY并调用 `execute()`批量下发 | 调用已经返回，或present事件已发生 |
| `P_return` | HWC `present()`已成功返回一个present fence，并取得本帧可用的per-layer release-fence map | 该fence在返回前、调用期间还是返回后signal |
| `F_route` | Output与SF已把release/present fence分发给Layer、旧client target、统计和事务callback | callback已等待实际显示 |
| `P_signal` | 可靠present fence按HAL语义signal；固定条件下可把该display frame归因到目标内容 | video-mode整屏扫描已经结束，或用户视觉反应已发生 |
| `B_reuse` | 后续replacement/removal令目标Buffer的release fence满足，producer可安全复用它 | 与 `P_signal` 必然同一时刻 |

固定路径有以下主干：

```text
Q_accept < Q_notice < K_select
W_apply < I_enter < T_commit < K_select
K_select < L_acquire < L_active < D_mark < R_post
R_post < E_pre < O_visible < H_write < V_stage_apply
V_stage_apply < C_lease < C_draw < C_stage < P_call < P_return < F_route
P_call < P_signal
L_active < B_reuse
```

`W_apply`与 `Q_notice`在一般情况下没有固定先后：隐藏层可以先积累Buffer，show也可以先到而等待内容。固定条件只要求control state在所选INVALIDATE前已写入current，并要求Buffer callback在 `handlePageFlip()`冻结候选前完成。Binder线程完全可能在主线程已经进入 `I_enter`、但尚未到 `K_select`时完成 `Q_notice`，目标仍可赶上本轮；因此不能强加 `Q_notice < I_enter`。

`P_return`与 `P_signal`没有规范保证的严格先后。HWC调用可能在目标VSync前返回一个pending fence，也可能跨过该VSync才返回一个已经signal的fence。函数返回本身不声明fence状态；拿到对象后必须查询它的signal状态/时间。`B_reuse`又属于某块Buffer的所有权时间线，不能与display级present点合并。

`C_draw`不要求总能得到一个有效native fence：支持且flush成功时，`drawLayers()`返回的ready fence表示绘制完成条件；它在返回时可能仍pending，也可能已经signal，下游若观察到尚未signal就必须等待或遵守该依赖。不支持native fence或flush失败时，RenderEngine会同步 `finish()`，成功返回的 `NO_FENCE`表示client target此刻已经可读。

## 2. 五类对象与四个执行语境：SF 看不到 App 的 View 树

固定的非BLAST路径中，producer和consumer跨进程，但BufferQueue核心、consumer以及 `BufferQueueLayer`都在 surfaceflinger 进程。App拿到的是 `IGraphicBufferProducer`远端接口；`queueBuffer()`经Binder进入SF侧实现。回调则在SF进程内从BufferQueue直接进入Layer listener。

```text
App进程 / RenderThread
  ANativeWindow → BpGraphicBufferProducer
                    │ Binder queueBuffer
                    ▼
surfaceflinger进程 / Binder线程
  BufferQueueProducer + BufferQueueCore
                    │ 同进程 consumer callback
                    ▼
  BufferQueueLayer::onFrameAvailable
                    │ request SF VSync
                    ▼
surfaceflinger进程 / 主线程
  Layer drawing tree → CompositionEngine → Output
                    │
                    ▼
Composer HAL / 厂商显示栈
  validate / present / fences
```

另一条控制支线来自WMS持有的 `SurfaceControl.Transaction`。它通过 `ISurfaceComposer::setTransactionState()`改Layer current state；这与BufferQueue的 `onFrameAvailable()`不是同一个入口。两条支线只是在后续SF主线程帧边界汇合。

五类核心对象不能互换：

| 对象 | 归属 | 主要职责 | 它不是 |
|---|---|---|---|
| `GraphicBuffer` | 跨进程共享分配 | 承载像素存储描述 | Layer状态或一整帧屏幕 |
| `BufferQueueLayer` | SF Layer树 | 消费传统BufferQueue、维护active buffer和Layer状态 | App内View树 |
| `LayerFE` | CompositionEngine前端接口 | 暴露某Layer本轮几何、内容与Buffer快照 | 独立显示输出 |
| `OutputLayer` | 某个Output | 表示一个Layer在该Display上的投影、可见区和合成状态 | 全局唯一Layer |
| `RenderSurface` / client target | 某个Output | 承载SF把CLIENT层预合成后的整块输入 | App窗口自己的Buffer |

一个Decor里的Button、TextView早已被HWUI画进窗口Buffer。SF只看到窗口层、SurfaceView、壁纸、系统栏、容器/效果层等可独立合成节点。同一SF Layer又可能投影到内屏、外屏或虚拟屏，因而拥有零个、一个或多个OutputLayer。

四个执行语境也必须分清：App RenderThread生产窗口内容；SF Binder线程接收producer或事务调用；SF主线程按VSync提交Layer树并驱动合成；Composer服务/厂商实现再把状态交给显示硬件。SF的RenderEngine虽使用GPU，却不是App RenderThread。

本章的最短源码地图是：

```text
frameworks/native/libs/gui/
└── BufferQueueProducer.cpp

frameworks/native/services/surfaceflinger/
├── SurfaceFlinger.cpp
├── Layer.cpp
├── BufferLayer.cpp
├── BufferQueueLayer.cpp
├── BufferStateLayer.cpp
├── CompositionEngine/src/{CompositionEngine,Output,OutputLayer,Display,RenderSurface}.cpp
└── DisplayHardware/{HWComposer,FramebufferSurface}.cpp

hardware/interfaces/graphics/composer/2.1/
├── IComposer.hal
└── IComposerClient.hal
```

WMS的版本门则在 `WindowManagerService`：`wm_use_blast_adapter`读取默认值为false；只有系统与窗口条件允许时，ViewRoot才启用BLAST adapter。本章固定默认false分支，不把一个可选实验开关写成所有r48窗口的事实。

## 3. `queueBuffer()`回调先于返回：真实队列与shadow queue是两本账

第214章已经证明 `Q_accept`发生在 `BufferQueueProducer::queueBuffer()`锁内：slot从DEQUEUED变为QUEUED，frame number递增，`BufferItem`进入 `mCore->mQueue`。离开BufferQueue主锁后，producer按ticket串行执行回调：

```cpp
if (frameAvailableListener != nullptr) {
    frameAvailableListener->onFrameAvailable(item);
} else if (frameReplacedListener != nullptr) {
    frameReplacedListener->onFrameReplaced(item);
}

if (connectedApi == NATIVE_WINDOW_API_EGL) {
    lastQueuedFence->waitForever("Throttling EGL Production");
}
```

这段顺序带来两个反直觉结论。

第一，回调在 `queueBuffer()`返回前同步发生；固定非BLAST队列的consumer就在SF进程，回调不需要再过一次跨进程Binder。因此 `Q_notice`甚至可以先于App RenderThread的 `Q_return`，SF的VSync请求也可在producer仍做EGL节流时发出。

第二，BufferQueue主锁已释放，但callback lock仍保证callback ticket顺序。慢回调不会破坏Core锁内不变量，却会延长producer返回；“producer卡在queue”不自动等于Core互斥量被长期占用。

`BufferQueueLayer::onFrameAvailable()`维护的是SF为选择帧准备的shadow queue：

```cpp
Mutex::Autolock lock(mQueueItemLock);
mQueueItems.push_back(item);
mQueuedFrames++;
mLastFrameNumberReceived = item.mFrameNumber;
mQueueItemCondition.broadcast();
```

退出该局部锁后，它调用 `signalLayerUpdate()`并把item通知给 `BufferLayerConsumer`。`mQueueItems`保存时间戳、frame number、fence和damage等选择信息；真正的slot所有权仍由 `BufferQueueCore`与consumer维护。shadow queue里“有一项”不等于consumer已经acquire它。

frame number通常顺序到达；若回调乱序，Layer会等待前号，单次最多500 ms后记录错误并继续。这是诊断边界，不是一个可靠的帧重排算法。

可丢尾帧还会走 `onFrameReplaced()`：真实队列最后一项被新 `BufferItem`覆盖，damage被合并，shadow queue对应项也被替换，而不是再增加 `mQueuedFrames`。固定首帧排除该支线；现场看到两个producer frame number也不能直接推导出SF积了两个可latch项。

## 4. `signalLayerUpdate()`只请求SF VSync，INVALIDATE还可能提前返回

`onFrameAvailable()`最终调用：

```cpp
void SurfaceFlinger::signalLayerUpdate() {
    mScheduler->resetIdleTimer();
    mPowerAdvisor.notifyDisplayUpdateImminent();
    mEventQueue->invalidate();
}
```

`MessageQueue::invalidate()`调用 `requestNextVsync()`；VSync事件到达后，event receiver才把预测的 `expectedVSyncTimestamp`封装成INVALIDATE消息。它不是立即在当前Binder调用栈运行 `handlePageFlip()`，也不是直接投一个REFRESH。

进入 `onMessageInvalidate()`后，SF先缓存整帧统一使用的expected present time，并检查上一帧present fence：

```cpp
mExpectedPresentTime = expectedVSyncTime;
const TracedOrdinal<bool> framePending = {
        "PrevFramePending", previousFramePending(graceTimeForPresentFenceMs)};
```

若正在切换active config且前一帧仍pending，SF重新请求INVALIDATE并直接返回。启用backpressure传播时，满足对应条件也会 `signalLayerUpdate()`后返回。两种情况都发生在transaction处理和latch之前，所以“trace里出现INVALIDATE入口”仍不能证明 `T_commit`或 `L_active`。

固定路径排除了这些early return。随后真正的顺序是：

```cpp
refreshNeeded = handleMessageTransaction();
refreshNeeded |= handleMessageInvalidate();

if (refreshNeeded && mBootStage != BootStage::BOOTLOADER) {
    signalRefresh();
}
```

`handleMessageTransaction()`先 `flushTransactionQueues()`，再按flags决定是否运行 `handleTransaction()`；`handleMessageInvalidate()`则调用 `handlePageFlip()`、必要时重算bounds，并把latched Layer区域写入对应Output的dirty region。

因此一次INVALIDATE同时容纳两类工作，却没有把它们变成同一种对象：show事务走current/drawing提交；窗口Buffer仍走BufferQueue选择和consumer acquire。若只有一个未来时间戳Buffer，`shouldPresentNow()`会推迟latch并请求下个VSync；若只有几何事务，transaction本身也可以令 `refreshNeeded`为真而发REFRESH。

REFRESH消息使用event mask合并；`signalRefresh()`先把 `mRefreshPending=true`，再直接向SF主Looper分发REFRESH。INVALIDATE和REFRESH常紧邻出现，但这是两条不同消息、两个不同入口。

## 5. current、drawing与Layer树：show提交的是可见资格，不是像素

WMS的show、alpha、position、crop、parent等变更通过 `setTransactionState()`进入SF。服务端 `setClientStateLocked()`按 `layer_state_t::what`位图只修改本次携带的字段，并在Layer current state上推进sequence、flags或派生状态。

普通异步事务可在Binder线程写current state并很快返回。若desired present time或BufferState事务里的acquire fence尚未就绪，同一apply token的事务会进入FIFO；INVALIDATE中的 `flushTransactionQueues()`只从各队头开始应用，不能让后项越过前项。

本章目标内容来自传统BufferQueue，所以它的Buffer不经过 `Transaction::setBuffer()`。不过WMS控制事务仍与BLAST `BufferStateLayer`共用Transaction基础设施。两条输入路径可概括为：

| Layer类型 | Buffer进入SF的主入口 | 时间/fence主门 |
|---|---|---|
| `BufferQueueLayer` | producer `queueBuffer()` → consumer callback | `shouldPresentNow()`与latch前 `fenceHasSignaled()` |
| `BufferStateLayer` | `Transaction.setBuffer()` → `setTransactionState()` | Transaction apply前的desired time/acquire fence；apply后仍过公共latch门 |

主线程 `handleTransactionLocked()`先让BufferLayer通知本地sync points，再遍历有 `eTransactionNeeded` 的Layer：

```cpp
const uint32_t flags = layer->doTransaction(0);
if (flags & Layer::eVisibleRegion) {
    mVisibleRegionsDirty = true;
}
```

`Layer::doTransaction()`会push/apply pending state、处理resize和派生几何，最后执行：

```cpp
commitTransaction(c);
// Layer::commitTransaction
mDrawingState = stateToCommit;
```

随后SF全局 `commitTransactionLocked()`才做：

```cpp
mDrawingState = mCurrentState;
mDrawingState.traverse([](Layer* layer) {
    layer->commitChildList();
});
```

所以current/drawing不是唯一一对浅拷贝：SF有全局Layer树快照，每个Layer也有自己的请求态、drawing态、pending状态与child list。`T_commit`表示这些状态已形成供本帧继续使用的视图，不表示BufferQueue consumer已经acquire内容。

show只解除隐藏/alpha/父裁剪等控制门，使Layer获得“可以参与可见性计算”的资格。若没有active Buffer，普通BufferLayer仍没有可画内容；若已有Buffer但show还没进入drawing，它也可被latch却不出现在该Output。控制面 `W_apply`与数据面 `L_active`必须在可见合成点汇合。

## 6. `handlePageFlip()`先冻结候选：时间戳决定是否赶本轮

`handlePageFlip()`先遍历drawing Layer树，不边遍历边acquire：

```cpp
mDrawingState.traverse([&](Layer* layer) {
    if (layer->hasReadyFrame()) {
        frameQueued = true;
        if (layer->shouldPresentNow(expectedPresentTime)) {
            mLayersWithQueuedFrames.push_back(layer);
        } else {
            layer->useEmptyDamage();
        }
    } else {
        layer->useEmptyDamage();
    }
});
```

先冻结集合是并发安全策略。源码给出的反例是两个producer共享同一command stream：若latch Layer 0期间又接收Layer 0和Layer 1的新帧，再继续动态扩大遍历集合，display可能等待Layer 1，而Layer 1又排在等待display的Layer 0后，形成环。固定集合让本周期只处理遍历开始时的候选代际。

`BufferLayer::hasReadyFrame()`只回答是否有frame update、sideband变更或auto-refresh。传统 `BufferQueueLayer::shouldPresentNow()`再读取shadow queue头项：

```cpp
const int64_t addedTime = mQueueItems[0].mTimestamp;
const bool isPlausible =
        addedTime < expectedPresentTime + s2ns(1);
const bool isDue = addedTime < expectedPresentTime;
return isDue || !isPlausible;
```

严格小于意味着timestamp恰好等于expected time时仍不算due；超过expected但少于未来1秒会推迟；更远的异常未来值反而为稳定性被视为implausible并放行，同时计入bad desired-present统计。不能把这里简化成“时间戳越大越晚显示”。

若队列有帧却没有任何候选，或候选均未真正latch，结尾会再请求layer update。若Layer已经从线上树移入 `mOffscreenLayers`，SF另行执行 `latchAndReleaseBuffer()`，目的是持续回收producer slot；这种消费不会让内容进入屏幕。

BLAST `BufferStateLayer::shouldPresentNow()`在r48只检查sideband/auto-refresh/`hasFrameUpdate()`，因为desired time和changed acquire fence主要已在Transaction ready门处理。把传统timestamp规则原样套到BLAST会重复甚至错置门的位置。

## 7. `latchBuffer()`是四道门，再由三步更新形成active buffer

候选集合冻结后，SF在 `mStateLock`下逐层调用公共 `BufferLayer::latchBuffer()`。它并非看到 `mQueuedFrames>0`就无条件成功，而是依次检查：

1. 当前确有ready frame；
2. 上次 `updateTexImage()`没有仍卡在下一次pre-composition之前，即 `mRefreshPending=false`；
3. acquire fence满足本Layer的latch规则；
4. deferred transaction/local sync points已经满足。

核心门如下：

```cpp
if (mRefreshPending) {
    return false;
}
if (!fenceHasSignaled()) {
    mFlinger->signalLayerUpdate();
    return false;
}
if (!allTransactionsSignaled(expectedPresentTime)) {
    mFlinger->setTransactionFlags(eTraversalNeeded);
    return false;
}
```

固定帧是非droppable且fence已signal，因此通过第三道门。一般情况却有两个旁路：`debug.sf.latch_unsignaled`首次读取后静态缓存，非零时允许继续；droppable头项也会被允许latch，以免不断被新帧替换而永远无法取得。两者都不等于GPU内容已完成，后续Layer/HWC状态仍携带acquire fence依赖。

真正推进内容需要三步都成功：

```text
updateTexImage()
  → BufferLayerConsumer从真实BufferQueue选择/acquire目标项
updateActiveBuffer()
  → mBufferInfo取得GraphicBuffer、slot与acquire fence
updateFrameNumber()
  → current frame number与latch时间入账
```

在固定的“头fence已signal”路径，传统 `updateTexImage()`还会限制可跳到的最大frame number：它从shadow queue头开始扫描连续已signal项，一遇后续pending fence就停止，再把最后连续ready的编号传给consumer，从而不让generic时间戳drop越过该后帧。若头项本身pending却因droppable或调试旁路获准latch，局部变量先初始化为最新received编号，这个cap不再提供同样的“不跨pending帧”保证；安全性仍要靠传下去的fence依赖，而不是这段扫描。

consumer可能按及时的后帧丢掉更老的显式时间戳帧；对应shadow项会被移除、damage合并、TimeStats记录清理。因此 `L_acquire`指最终被consumer选中的目标项，而不是调用前肉眼看到的队头必然原样胜出。

三步成功后，`gatherBufferInfo()`补齐格式和frame-latency状态，`mRefreshPending=true`。首个active Buffer会强制可见区域重算；crop、transform、scaling mode、inverse display或尺寸变化也可能重算。latch时间只证明SF已选中内容，不证明它未被遮挡、更不证明HWC已present。

## 8. show、active buffer、visibility与damage在这里真正汇合

`handlePageFlip()`把成功latch并需要刷新区域的Layer放入 `mLayersPendingRefresh`；随后 `handleMessageInvalidate()`在可见区域脏时先 `computeLayerBounds()`，再按每个pending Layer的screen bounds调用 `invalidateLayerStack()`。

```cpp
for (auto& layer : mLayersPendingRefresh) {
    Region visibleReg;
    visibleReg.set(layer->getScreenBounds());
    invalidateLayerStack(layer, visibleReg);
}
```

`invalidateLayerStack()`只标记Layer所属display/layer stack对应Output的dirty region。它不是把像素拷入display framebuffer，也没有在此计算HWC最终类型。

一般时序应画成两条支线：

```text
控制面：SurfaceControl show/alpha/crop → current → drawing ┐
                                                       ├→ 可见LayerFE/OutputLayer
数据面：BufferQueue item → select → latch → active Buffer ┘
```

两条支线不是全局互斥事务：Buffer可以先在隐藏Layer上latch；show也可以先进入drawing而暂时显示空内容。固定路径中，二者赶上同一帧且没有后继变化，才可把后面的display frame归因到目标首Buffer。

即使二者都到齐，Layer仍可能因为以下条件不生成可见OutputLayer：

- Layer不属于该Output的layer stack，或只允许primary而当前不是primary；
- hidden、alpha/父状态或裁剪令其不可见；
- screen bounds落在Output bounds之外；
- 上方不透明区域完全覆盖它；
- transparent-region hint与可见非透明区域计算后为空。

被半透明层覆盖的区域仍可能属于visible region，因为下层像素会参与blend；只有上方opaque coverage才能直接减掉下层区域。固定目标未被完全遮挡，所以会到达 `O_visible`。

`handlePageFlip()`的返回条件是候选集合非空且至少有新数据latched。transaction本身、全量重绘或HWC请求也可能独立要求刷新。`R_post`只说明REFRESH已经排入SF Looper；它仍不是CompositionEngine完成点。

## 9. REFRESH把drawing Layer变成每个Output的合成快照

`onMessageRefresh()`先清SF全局的 `mRefreshPending`，再构造 `CompositionRefreshArgs`。其中既有当前所有Display对应的Output（Output自身稍后再检查是否enabled），也有drawing Layer树的LayerFE，以及本轮候选LayerFE集合、颜色配置、全量重绘与geometry标志：

```cpp
for (const auto& [_, display] : mDisplays) {
    refreshArgs.outputs.push_back(
            display->getCompositionDisplay());
}
mDrawingState.traverseInZOrder([&](Layer* layer) {
    if (auto layerFE = layer->getCompositionEngineLayerFE()) {
        refreshArgs.layers.push_back(layerFE);
    }
});
```

它传的是本轮稳定LayerFE集合，而不是把SF `Layer`对象所有可变字段裸交给HWC。`layersWithQueuedFrames`则供released-layer/fence等逻辑识别本轮更新者；“在这个集合里”仍不等于最终在每个Output可见。

`CompositionEngine::present()`的骨架很短，却给出了严格阅读顺序：

```cpp
preComposition(args);
for (const auto& output : args.outputs) {
    output->prepare(args, latchedLayers);
}
updateLayerStateFromFE(args);
for (const auto& output : args.outputs) {
    output->present(args);
}
```

`preComposition()`给每个LayerFE记录refresh start。`BufferLayer::onPreComposition()`还会清该Layer自己的 `mRefreshPending`，允许后续VSync再latch下一项；如果它仍有ready frame，CompositionEngine会标记 `needsAnotherUpdate()`，本轮结束后SF再请求layer update。

所有Output先完成geometry prepare，再统一更新LayerFE content/geometry快照，最后逐Output执行present。这可避免第一个Output处理时就破坏第二个Output所需的共享前端状态。`latchedLayers`也确保同一LayerFE的basic geometry每帧最多准备一次，但每个Output仍保留自己的OutputLayer与可见区域。

多个Output的 `present()`在SF主线程上依次调用，并不意味着两个物理显示在同一纳秒present。每个HWC-backed Display有自己的HWC display、client target和present fence；没有HWC display id的Output走基类client路径，不能凭空获得HWC present fence。默认Display的全局统计更不能代替外接屏或虚拟屏证据。

## 10. Output先算可见层、写HWC候选状态，随后才协商策略

`Output::prepare()`在Output enabled且本帧需要更新geometry时重建layer stack。它从前到后评估LayerFE：先检查layer stack归属和 `isVisible`，再计算opaque、covered、transparent、shadow与dirty region，最后把落在Output bounds内的非空结果保存成OutputLayer。

```text
LayerFE全局几何
  → belongsInOutput / hidden门
  → 屏幕footprint与透明提示
  → 减去上方opaque coverage
  → 与Output viewport/bounds求交
  → 创建或复用OutputLayer
```

不更新geometry时，Output复用既有OutputLayer列表；后面的 `updateLayerStateFromFE()`仍可只刷新content。因此“prepare没有重建”不等于本帧没有新Buffer。

每个Output的正常骨架固定为八步：

```cpp
updateColorProfile(refreshArgs);
updateAndWriteCompositionState(refreshArgs);
setColorTransform(refreshArgs);
beginFrame();
prepareFrame();
devOptRepaintFlash(refreshArgs);
finishFrame(refreshArgs);
postFramebuffer();
```

`devOptRepaintFlash()`只在调试脏区闪烁配置启用时额外重绘、present、等待并重新prepare；固定生产路径未启用它，但阅读骨架时不能把这个真实阶段删掉。

第二步会让每个OutputLayer更新候选composition state，并立即调用 `writeStateToHWC()`。对目标BufferLayer，LayerFE快照已经包含 `mBufferInfo.mBuffer`、buffer slot和acquire fence；这些DEVICE候选命令会在validate调用前写入Composer command stream，并由validate的 `execute()`连同验证请求一起送出，不是在RenderEngine完成client target后才首次出现。

`beginFrame()`用dirty、当前是否无可见层、上一帧是否也无可见层计算 `mustRecompose`。从有内容变成空屏时仍输出一次黑帧；连续空屏可以跳过重复重组。这个布尔值传给DisplaySurface，但后续HWC状态机仍可能继续，不能把“skip recompose”翻译成整个 `Output::present()`提前返回。

`prepareFrame()`才调用 `chooseCompositionStrategy()`，再把最终的CLIENT/DEVICE组合告诉RenderSurface。先写候选状态、后validate，是HWC协议要求：设备需要看到Layer属性、Buffer和fence，才能决定哪些输入能由自己合成。

## 11. validate协商责任边界，`presentOrValidate`可能已经完成present

CLIENT和DEVICE是HWC视角的责任标签：

| 最终类型 | 谁合成该Layer | 最终进入HWC的输入 |
|---|---|---|
| CLIENT | HWC客户端，即SurfaceFlinger的RenderEngine | 该Layer先被画入client target |
| DEVICE | HWC/厂商显示栈 | 原Layer Buffer、几何、blend与acquire fence |
| 混合 | 两者各处理一部分 | 一个client target加若干DEVICE Layer |

它不承诺DEVICE一定使用overlay，也不承诺厂商内部绝不调用GPU；源码能证明的是接口责任，不是具体硅片单元。

物理 `Display::chooseCompositionStrategy()`先把Output默认成client-only，再调用：

```cpp
hwc.getDeviceCompositionChanges(
        displayId,
        anyLayersRequireClientComposition(),
        &changes);
```

固定混合帧在validate前就已有候选CLIENT Layer，所以HWComposer走 `validate()`。成功后依次取得changed composition types、display requests、layer requests和client-target property，再调用 `acceptChanges()`。这里的 `Composer::acceptDisplayChanges()`只把 `ACCEPT_DISPLAY_CHANGES`写入command writer，并未执行队列；返回Display后，Display才把changed types与各类request应用到OutputLayer/Output。因此先发生的是accept命令暂存，接着是SF本地最终视图更新；厂商Composer真正执行accept命令，要等后面的 `presentDisplay()`调用 `execute()`，并按writer顺序与client target、present命令一起下发。

这种协商并不对称：SF明确请求CLIENT时，HWC不能把它改回另一类型；SF提交DEVICE候选时，HWC可以在validate中要求它改为CLIENT。最终策略来自SF的初始/强制条件与HWC能力反馈共同收敛，不是HWC不受约束地自由选择。

最终两个聚合布尔值来自所有OutputLayer：

```cpp
state.usesClientComposition =
        anyLayersRequireClientComposition();
state.usesDeviceComposition =
        !allLayersRequireClientComposition();
```

一帧完全可以二者都为true。固定目标窗口层最终为DEVICE；另一个效果层为CLIENT，于是RenderEngine绘制CLIENT内容，并可能执行非CLIENT层的clear-only request；目标窗口的DEVICE内容本身仍作为独立HWC Layer输入。

纯DEVICE候选有另一条重要分支。HWComposer先尝试 `presentOrValidate()`：

```text
state == 1
  → present已成功
  → 保存present fence与release-fence map
  → validateWasSkipped=true

否则且validate结果可继续
  → 读取changed types/requests
  → 暂存ACCEPT_DISPLAY_CHANGES，并在Display本地应用changes
  → 后面走普通present
```

因此 `presentOrValidate`这个名字不能一概解释为“只做validate”。若 `state==1`，present已经发生在 `prepareFrame() → chooseCompositionStrategy()`内部；后面的 `postFramebuffer()`只执行pending command、检查缓存状态并复用fences，不会再调用一次HWC `present()`。若本来就有CLIENT候选，则不能在client target尚未绘制时尝试跳过validate。

validate出错时，Display记录错误并从策略函数返回；虽然Output级聚合布尔仍保留先前默认值，也不能据此宣称系统完成了一次可靠的“全GPU自动回退”。per-layer状态、client target准备和HWC状态机没有帧级回滚承诺。

还有一个r48局部实现缺口：`applyClientTargetRequests()`用 `auto outputState = editState()`取得副本，随后对dataspace的赋值不一定写回Output state；RenderSurface的pixel format/dataspace setter仍会执行。阅读现场值时应分别检查两本账。

## 12. CLIENT层先画进SF client target，再把ready fence交给HWC

策略确定后，`finishFrame()`调用 `composeSurfaces()`。只要最终使用CLIENT composition，或HWC提出 `FLIP_CLIENT_TARGET`，SF就先从RenderSurface取得一块输出Buffer：

```cpp
if (hasClientComposition || outputState.flipClientTarget) {
    buf = mRenderSurface->dequeueBuffer(&fd);
    if (buf == nullptr) {
        return {};
    }
}
```

这里的 `fd`保护client target的旧内容，RenderEngine必须在安全后才能重写；它不是App目标窗口Buffer的acquire fence。`C_lease`因此属于Output自己的BufferQueue。

随后SF为 `requiresClientComposition()` 的OutputLayer生成实际内容绘制 `LayerSettings`。每笔请求可带source buffer/solid color、bounds、transform、clip、alpha、blend、dataspace、filter、rounded corner、shadow或blur；此外，HWC的clear-client-target request还可能为满足条件的非CLIENT不透明Layer生成只清理target、不重复绘制其DEVICE内容的请求。

固定帧最终调用：

```cpp
renderEngine.drawLayers(
        clientCompositionDisplay,
        layerPointers,
        buf->getNativeBuffer(),
        true,
        std::move(fd),
        &readyFence);
```

RenderEngine提交并拿到有效native fence时，`readyFence`保护这块client target的新内容；它返回时可以pending，也可以已经signal。走同步 `finish()`时可返回 `NO_FENCE`，表示调用返回时已经可读。它与各App Layer自己的acquire fence角色相似，但资源身份不同：前者保护SF输出目标，后者保护单个Layer输入。

`finishFrame()`再把ready fence交给 `RenderSurface::queueBuffer()`。物理Display的顺序是：

```text
ANativeWindow queue client target + ready fence
→ DisplaySurface::advanceFrame()
→ FramebufferSurface::nextBuffer()
→ consumer acquire该target
→ HWComposer::setClientTarget(slot, buffer, acquireFence, dataspace)
```

最后一步 `HWComposer::setClientTarget()`仍不是一次立即的厂商调用：r48 `Composer::setClientTarget()`只把slot、buffer、acquire fence和dataspace写进 `mWriter`后返回。随后 `Composer::presentDisplay()`再追加PRESENT_DISPLAY并调用 `execute()`，把积累的client-target命令批量送给Composer服务/厂商实现。于是 `C_stage`只证明命令已暂存；到present执行阶段，CLIENT预合成结果才与已validate的DEVICE Layer状态在同一显示提交中汇合。

HWC最终看到的不是“RenderEngine画整屏后再覆盖DEVICE层”，而是client target和DEVICE输入由显示策略统一组合。

三个边界经常被忽略：

- 只有 `FLIP_CLIENT_TARGET`而没有CLIENT Layer时，代码仍可dequeue/queue一块target，却不调用RenderEngine绘制；
- `FramebufferSurface::nextBuffer()`若得到 `NO_BUFFER_AVAILABLE`，会沿用当前target cache并返回成功，不再次调用 `setClientTarget()`；
- client-composition request cache命中同一output buffer与完整请求时，可以复用已有结果，`readyFence`为空而不再draw。

固定路径排除了这三种复用。RenderEngine draw失败只会移除request cache项，并没有撤销整帧；`advanceFrame()`失败主要记录日志；物理Display queue client target失败则会触发fatal，虚拟Display改为cancel。故现场必须同时看draw、queue、advance与HWC setClientTarget，不能只靠一个“GPU composition”slice判成功。

## 13. HWC present返回的是未来完成条件，不是已经亮到屏幕上

`Output::postFramebuffer()`先清dirty region并调用 `mRenderSurface->flip()`。这里的 `flip()`只增加page-flip计数；真正显示提交发生在 `Display::presentAndGetFrameFences()`。

固定慢路径调用：

```cpp
hwcDisplay->present(&displayData.lastPresentFence);
hwcDisplay->getReleaseFences(&releaseFences);
```

`present()`成功表示HWC接受/安排了本display frame，并给SF一个代表该present事件的fence；随后查询本帧可用的per-layer release fences。HAL只要求为本帧收到新Buffer内容的DEVICE Layer返回release fence；某层缺项按规范表示上一帧Buffer已可写，不能假设每个DEVICE Layer必有一条。present返回时其fence可以pending，也可以已经signal，所以 `P_return`只能证明协议成功返回和fence可被检查，不能独自证明 `P_signal`发生或未发生。

纯DEVICE快路径若早先 `presentOrValidate(state==1)`已经present，`presentAndGetReleaseFences()`改为执行Composer pending commands、检查缓存状态，然后直接返回已有的present/release结果。r48名为 `presentError` 的字段实际接收紧随直达present之后 `getReleaseFences()`的结果，这个命名也不应被扩大成另一轮present。把两条路径都画成“validate → RenderEngine → postFramebuffer → presentDisplay”会凭空制造一次调用。

HWC 2.1对present fence的定义按输出类型不同：

| Output | fence signal的规范语义 |
|---|---|
| video-mode物理面板 | 本帧合成结果在某次VSync开始出现在display |
| command-mode物理面板 | 本帧开始传入panel memory |
| virtual display | output buffer写入完成，外部可以安全读取 |

对物理屏，signal不是“整屏最后一行已经扫描结束”，也不是“光子已被人眼感知”。它仍是AOSP可获得的最靠后的结构化显示证据之一，远强于queue、latch、validate或present函数返回。

能力 `PRESENT_FENCE_IS_NOT_RELIABLE`允许Composer声明该fence不能准确表示实际present time。SF据此不把 `DISPLAY_PRESENT`列为受支持frame timestamp，并让相关启动配置知道present timestamp不可依赖；VR composer或无sync framework时，Scheduler也可忽略present fences。没有有效fence时，部分Layer/动画统计会回退到HWC refresh timestamp，这个替代值不能冒充一条可靠sync fence。

present fence属于display frame，而非自动属于某个Layer。只有在固定的“目标已visible、参加该frame、无后继replacement、fence可靠”条件下，才可由 `P_signal`推断目标内容已经到达该显示完成点。现实trace中若中间又latch了下一Buffer，必须用frame number、buffer id与timestamps重新归因。

## 14. release与postComposition闭合所有权，callback仍不等待fence signal

取得frame fences后，Output先调用 `RenderSurface::onPresentDisplayCompleted()`。对FramebufferSurface，这会用本帧present fence保护上一块client target并把旧slot release回它的BufferQueue；当前target仍由显示路径持有，等待后续帧替换。

随后 `Output::postFramebuffer()`为每个OutputLayer构造release fence：

1. 若有HWC Layer，先查本帧per-layer release fence；
2. 只要Output本帧使用CLIENT composition，就再与当前client-target acquire fence合并；
3. 调用 `LayerFE::onLayerDisplayed(releaseFence)`；
4. 对已不在当前OutputLayer列表、却需要释放的Layer，只能保守地给present fence。

第二步看似多余，是因为r48没有精确追踪上一帧client-target acquire fence，源码选择总与当前target fence合并。它是保守近似，不表示每个Layer都实际被RenderEngine读取。

传统 `BufferQueueLayer::onLayerDisplayed()`把fence交给 `BufferLayerConsumer::setReleaseFence()`。CompositionEngine返回后，SF `postComposition()`先对 `mLayersWithQueuedFrames`调用 `releasePendingBuffer()`；consumer此时释放的是被新active Buffer替换的旧pending项。目标首Buffer本身要等未来另一帧替换或Layer移除，才到 `B_reuse`。

`postComposition()`还会：

```text
取默认Display的client-target acquire fence作为GPU composition done证据
→ 取HWComposer保存的默认Display present fence
→ Layer.onPostComposition记录latch帧的ready/present时间线
→ TransactionCompletedThread挂入present fence并发送callbacks
→ 条件满足时把present fence交Scheduler
→ 更新TimeStats与composition类型计数
```

事务callback在这里拿到的是fence对象，不会先等待它signal。于是callback收到pending fence很正常；若HWC调用较慢，它也可能收到已经signal的fence。WMS事务完成、BLAST transaction callback或应用监听器被调用，都不能单凭回调时刻判断画面是否已present，必须检查fence状态/时间与帧归因。

BLAST对照路径还要避免另一种误解。`BufferStateLayer::releasePendingBuffer()`会finalize callback handles，callback把 `previousReleaseFence`带回App内BLAST。BLAST随后可以立即调用本地consumer `releaseBuffer(item, previousReleaseFence)`：slot逻辑上变FREE并保存这个仍可能pending的fence，producer下一次dequeue可取得slot与fence，再等它后安全写。故“release fence未signal”不必然等于 `releaseBuffer()`或 `dequeueBuffer()`函数本身一直阻塞；真正背压还取决于有限slot、outstanding acquire、callback消费速度以及producer何时等待返回的fence。

三类fence可这样结账：

| fence | 保护的资源 | signal后允许什么 | 不能代替 |
|---|---|---|---|
| Layer acquire fence | 某个App/producer的新Layer Buffer | SF/RE/HWC安全读取 | Layer可见或display present |
| Layer/client-target release fence | 上一代Layer Buffer或旧client target | producer安全覆盖/复用相应分配 | 当前display frame实际显示时间 |
| display present fence | 一次Output frame | 证明到达该Output定义的present点 | 每个Buffer精确release与整屏扫描结束 |

## 15. 从症状倒推断点，并用九组只读练习自证

先按完成点定位，不要先猜“GPU慢”或“HWC坏了”：

| 现象 | 优先核对 | 还要排除 |
|---|---|---|
| App queue已返回，SF迟迟没latch | `Q_notice`、下次INVALIDATE、timestamp、late acquire、sync point | callback先于producer返回，不能只按线程slice排序 |
| show已提交仍黑 | show是否进drawing、是否有active Buffer、Layer是否属于Output/被覆盖 | WMS draw state不等于SF visibility |
| latch有记录但无目标OutputLayer | bounds、hidden/alpha、layer stack、opaque coverage | latch本身没有可见承诺 |
| validate后没有RenderEngine draw | 最终是否纯DEVICE、flip-only或request-cache复用 | validate成功不要求GPU工作 |
| RenderEngine draw有记录但显示未变 | target queue/advance/setClientTarget、HWC present及fence | SF client target不是最终present |
| present调用结束仍看不到 | present fence是否有效、可靠、已signal，frame归因是否被替换 | 函数返回本身不证明fence是否已signal |
| producer复用变慢 | free slot数、outstanding acquire、release fence等待与callback消费 | pending release fence未必阻塞逻辑release/dequeue返回 |
| 多屏结果不一致 | 每个OutputLayer、composition type、present fence | 默认Display统计不可外推 |

以下命令只读本地 `android-11.0.0_r48` 源码；每组都把问题限制到少数文件，Bash 3.2与Zsh 5.9均可运行。

### 练习 1：证明queue回调位于producer返回之前

```bash
cd /Users/ninebot/androidSource
rg -n 'mSlots\[slot\]\.mBufferState\.queue|onFrameAvailable\(item\)|Throttling EGL Production|return NO_ERROR' \
  frameworks/native/libs/gui/BufferQueueProducer.cpp
```

按源码顺序标出 `Q_accept`、consumer callback、EGL节流与函数返回，并说明哪个调用发生在BufferQueue主锁外。

### 练习 2：区分真实队列与BufferQueueLayer shadow queue

```bash
cd /Users/ninebot/androidSource
rg -n 'mQueueItems\.push_back|mQueuedFrames\+\+|signalLayerUpdate|onBufferAvailable|onFrameReplaced' \
  frameworks/native/services/surfaceflinger/BufferQueueLayer.cpp
```

解释shadow queue保存什么、谁仍管理slot所有权，以及replacement为何不等于再增加一个pending slot。

### 练习 3：找出INVALIDATE的两个early return

```bash
cd /Users/ninebot/androidSource
rg -n 'mSetActiveConfigPending|framePending && mPropagateBackpressure|handleMessageTransaction|handleMessageInvalidate|signalRefresh' \
  frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
```

回答哪些条件会让SF在transaction/latch前退出，以及INVALIDATE怎样决定是否再发REFRESH。

### 练习 4：核对Layer与全局current/drawing提交

```bash
cd /Users/ninebot/androidSource
rg -n 'notifyAvailableFrames|layer->doTransaction|commitTransaction\(c\)|mDrawingState = mCurrentState|commitChildList' \
  frameworks/native/services/surfaceflinger/{SurfaceFlinger.cpp,Layer.cpp}
```

画出Layer内部commit和全局Layer树commit，解释为何show transaction返回不能代替 `T_commit`。

### 练习 5：对比两类Layer的present与fence门

```bash
cd /Users/ninebot/androidSource
rg -n 'shouldPresentNow|fenceHasSignaled|mIsDroppable|lastSignaledFrameNumber|transactionIsReadyToBeApplied' \
  frameworks/native/services/surfaceflinger/{BufferQueueLayer.cpp,BufferStateLayer.cpp,SurfaceFlinger.cpp}
```

分别写出传统BufferQueueLayer和BufferStateLayer在哪一层检查时间/fence，并指出droppable旁路仍保留什么依赖。

### 练习 6：追踪latch到可见OutputLayer

```bash
cd /Users/ninebot/androidSource
rg -n 'handlePageFlip|updateTexImage|updateActiveBuffer|collectVisibleLayers|ensureOutputLayerIfVisible' \
  frameworks/native/services/surfaceflinger/{SurfaceFlinger.cpp,BufferLayer.cpp,BufferQueueLayer.cpp} \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/Output.cpp
```

找出consumer acquire、active buffer、visible region与OutputLayer创建四个不同边界。

### 练习 7：证明HWC有慢路径与直达present路径

```bash
cd /Users/ninebot/androidSource
rg -n 'getDeviceCompositionChanges|presentOrValidate|state == 1|validateWasSkipped|acceptChanges|presentAndGetReleaseFences' \
  frameworks/native/services/surfaceflinger/DisplayHardware/HWComposer.cpp
```

分别画出有CLIENT候选、纯DEVICE且直达、纯DEVICE但仍需validate三条路径。

### 练习 8：追踪SF client target的完整所有权链

```bash
cd /Users/ninebot/androidSource
rg -n 'dequeueBuffer|drawLayers|queueBuffer|advanceFrame|setClientTarget|presentDisplay|Error Composer::execute\(\)|NO_BUFFER_AVAILABLE' \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/{Output.cpp,RenderSurface.cpp} \
  frameworks/native/services/surfaceflinger/DisplayHardware/{FramebufferSurface.cpp,ComposerHal.cpp}
```

说明dequeue fence、RenderEngine ready fence、FramebufferSurface current fence分别从谁的视角命名，并标出 `setClientTarget()`暂存命令与 `presentDisplay()`批量execute之间的边界。

### 练习 9：把present、release与callback分开结账

```bash
cd /Users/ninebot/androidSource
rg -n 'SET_PRESENT_FENCE|SET_RELEASE_FENCES|PRESENT_FENCE_IS_NOT_RELIABLE' \
  hardware/interfaces/graphics/composer/2.1/{IComposer.hal,IComposerClient.hal}
rg -n 'onPresentDisplayCompleted|onLayerDisplayed|releasePendingBuffer|addPresentFence|sendCallbacks' \
  frameworks/native/services/surfaceflinger/{SurfaceFlinger.cpp,BufferQueueLayer.cpp} \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/{Output.cpp,RenderSurface.cpp}
```

用自己的话定义三类fence，并解释为什么callback到达、present返回和present fence signal是三个时刻。

## 16. 结论：先汇合控制与内容，再协商责任，最后等显示证据

本章的完整主线是：

```text
传统producer queue Buffer
→ BufferQueueLayer收到callback并请求SF VSync
→ INVALIDATE提交show等current state
→ handlePageFlip按时间/fence/sync point选择并latch
→ active Buffer与drawing可见状态汇合
→ REFRESH建立LayerFE与每Display OutputLayer
→ 候选DEVICE状态/Buffer/fence先写入Composer command stream
→ validate结果与SF本地应用收敛出CLIENT、DEVICE或混合责任
→ CLIENT层由RenderEngine画成client target
→ client target与DEVICE Layer在HWC汇合
→ 慢路径present或早先presentOrValidate直达
→ release/present fences回传并分发
→ 查询可靠present fence的signal time并完成目标帧归因
```

应带走十二条结论：

1. r48普通窗口主线默认仍可走非BLAST `BufferQueueLayer`；BLAST是独立的Transaction-buffer路径，不能混成一次接收。
2. `queueBuffer()`的SF consumer callback发生在producer函数返回前，且已经离开BufferQueue主锁。
3. shadow queue服务于Layer选帧；真实队列和slot状态仍由BufferQueue consumer/core维护。
4. `signalLayerUpdate()`只请求SF VSync；INVALIDATE还可能被配置切换或背压提前截断。
5. show进入drawing只提供可见资格，Buffer latch只提供活动内容；二者必须在Output可见性计算中汇合。
6. 传统Layer在 `shouldPresentNow()`和latch检查时间/fence；BufferState事务把主要门前移到Transaction ready阶段。
7. droppable或调试旁路允许先latch未signal fence，但不会抹掉后续安全读取依赖。
8. Layer、LayerFE、OutputLayer是全局状态、合成快照接口和每输出投影，不是三个同义名称。
9. DEVICE候选Layer命令在validate前已暂存，并由validate执行后参与协商；RenderEngine之后只补出CLIENT层的client target。
10. `presentOrValidate(state==1)`已经present，后段不会再无条件调用一次HWC present。
11. release fence解决Buffer何时可安全复用；present fence描述display frame，事务callback不会先等它signal。
12. 只有可靠present fence signal加上正确的Layer/frame归因，才能把目标内容推进到本章的 `P_signal`；它仍不是整屏扫描结束或人眼感知证明。

自测：

1. 为什么 `Q_notice`可能早于App的 `queueBuffer()`返回？
2. show已进入drawing、但目标层没有active Buffer，会发生什么？
3. timestamp恰好等于 `expectedPresentTime`时，传统Layer本轮是否due？
4. droppable头帧fence未signal，为什么仍可能latch而又不破坏同步协议？
5. 为什么首Buffer通常触发geometry/visible-region重算？
6. latch成功后，哪些条件仍可能让目标没有OutputLayer？
7. CLIENT中的“client”是谁？client target与App窗口Buffer有何区别？
8. `presentOrValidate`何时省掉后续HWC present？
9. HWC为什么必须在RenderEngine draw前先看到Layer候选状态？
10. callback携带一个pending present fence时，能证明什么、不能证明什么？
11. pending release fence为何不必然阻塞逻辑release/dequeue返回？
12. 多Display场景为何不能只拿默认屏的present fence给所有Output下结论？

答案依次是：consumer callback在producer return前同步执行；只有可见资格而没有内容；严格小于才due；Layer仍把fence传给后续读者；空内容变有内容会改变覆盖关系；layer-stack、hidden/alpha、bounds、opaque coverage等仍可淘汰它；client是SurfaceFlinger，target是SF整合CLIENT层的Output Buffer；`state==1`；HWC要据Buffer/几何/能力协商责任；只证明callback已取得未来条件，不能证明fence已signal；slot可先变FREE并携带fence，真正写入仍须等待；每个Output独立建OutputLayer、client target、策略和present时间线。

下一章进入Choreographer、DisplayEventReceiver、App/SF VSync phase与callback队列，把“谁请求下一次VSync、哪个时间是预测目标、UI与SF为什么错相唤醒、掉帧证据落在哪条时间线”串成一条帧节奏链；仍以Android 11 r48为边界，不引入后续版本完整FrameTimeline模型。
