# 219 Android ViewRootImpl reportNextDraw、finishDrawing 与 WMS draw state 状态机

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只做静态源码核对：可以证明客户端何时登记并结清绘制回报、WMS 怎样推进单个 WindowState 的 draw state，以及 show 事务何时被准备；不能据此声称 SurfaceFlinger 已 latch、HWC 已 present 或面板已经扫描。第 220 章再接 Activity 级 `allDrawn`、`reportedDrawn`、`nowVisible` 与启动完成回调。

第 218 章已经看到 starting window 可在真实窗口进入 `performShowLocked()` 时开始清理。本章反向追真实窗口：**一次 `reportNextDraw()` 为什么既不是一帧，也不是完成；`finishDrawing()` 返回为什么仍不能证明窗口已经显示；`DRAW_PENDING → COMMIT_DRAW_PENDING → READY_TO_SHOW → HAS_DRAWN` 每一步究竟关闭哪本账？**

## 1. 固定普通首次硬件窗，再给每个完成点取唯一名字

先固定 `L_normal`：普通跨进程 Activity 的首个可见主窗口；新建非 BLAST Surface；硬件渲染已启用、Surface有效、本帧可画且swap成功；没有根 `SurfaceHolder`、子 `SurfaceView` 或额外 `WindowCallbacks`；`OnPreDraw` 不取消；WMS、App transition 与 policy 最终均允许显示；全程无异常。

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `S_alloc` | WMS 创建窗口 SurfaceControl，draw state 进入 DRAW_PENDING，初始带 HIDDEN | App 已拿到可写 Java Surface |
| `R_first` | 同步 relayout 把 `RELAYOUT_RES_FIRST_TIME` 返回给 ViewRootImpl | 这是 WindowState 一生第一次 relayout |
| `D_arm` | `reportNextDraw()` 首次把根绘制债加一并置 `mReportNextDraw=true` | traversal 已开始 |
| `V_draw` | `performDraw()` 捕获本轮 report 标志并进入绘制 | Buffer 已 queue |
| `Q_buf` | 固定硬件路线的 swap 已把窗口 Buffer 交给 producer queue | SurfaceFlinger 已 latch |
| `H_done` | HWUI frame-complete callback 在 RenderThread 侧被调用 | GPU fence、SF latch 或 present 已完成 |
| `U_done` | callback 投到 App UI Handler 后执行 `pendingDrawFinished()`，根债归零 | WMS 找得到原 WindowState |
| `B_enter` | 普通 App 的同步 `IWindowSession.finishDrawing()` 进入 system_server | draw state 一定会变化 |
| `W_commit` | WMS 在 global lock 内把该窗从 DRAW_PENDING 改为 COMMIT_DRAW_PENDING | placement 已运行 |
| `Q_place` | WMS 已合并一次 surface-placement 请求 | AnimationThread 已处理该请求 |
| `B_return` | 同步 AIDL 调用回到 App | Window 已 READY/HAS |
| `W_ready` | placement 调 `commitFinishDrawingLocked()`，状态成为 READY_TO_SHOW | Activity 聚合门已满足 |
| `A_gate` | 固定路线中 Activity 的真实窗口计数使 `allDrawn` 成立 | 下一轮 show 已成功 |
| `W_has` | `performShowLocked()` 通过自身门并把状态置为 HAS_DRAWN | SurfaceControl show 已成功 |
| `X_show` | `prepareSurfaceLocked()` 成功执行 show，并把相关 transaction 合入 WMS 全局事务 | SF 已消费这笔事务 |
| `F_latch` | SF 为这扇窗取得可用于合成的 Buffer | show 状态已同时生效 |
| `P_real` | 可归因于该真实窗口的 present fence signal | 用户业务语义已经完成 |

固定路线的客户端与服务端主链是：

```text
S_alloc < R_first < D_arm < V_draw < Q_buf < H_done < U_done
U_done < B_enter < W_commit < Q_place < B_return
Q_place < W_ready < A_gate < W_has < X_show
Q_buf < F_latch
```

`B_return` 与 `W_ready` 不应互相强排：Binder路径在锁内调用 `requestTraversal()`，已有调度时会合并、layout defer时只登记延迟请求，只有未defer且此前未调度时才新post到AnimationThread；锁释放后，已获调度的AnimationThread也可能在Binder返回前取得锁。最终物理可见还需要 `X_show` 对应的transaction到达SF，并与 `F_latch` 汇合后再compose/present；因此Java主链不能补写一条不存在的 `X_show < F_latch` 或 `W_has = P_real`。

本章随后会逐项放宽 `L_normal`。FIRST_TIME、`resized(reportDraw=true)`、软件绘制、SurfaceHolder、SurfaceView、BLAST sync、窗口移除与重复回报都可能改变局部顺序，但不会改变“六本账不能合并”这个结论。

## 2. 客户端债、Buffer、窗口五态、Activity 聚合、事务与 present 是六本账

一句“首帧画完”经常同时指向六个不同对象：

| 账本 | 代表字段或对象 | 回答的问题 |
|---|---|---|
| ViewRoot 回报账 | `mReportNextDraw`、`mDrawsNeededToReport` | 客户端是否还欠 WMS 一次 draw report |
| 图形生产账 | UI DisplayList、RenderThread、BufferQueue slot/fence | 内容是否录制、提交或 queue |
| WindowState draw 账 | `WindowStateAnimator.mDrawState` | WMS 对单扇窗口认可到哪个阶段 |
| Activity 聚合账 | `mNumInterestingWindows`、`mNumDrawnWindows`、`allDrawn` | 一组真实窗口是否满足 Activity 级门 |
| SurfaceControl 事务账 | hidden/show、alpha、matrix、post-draw transaction | 哪些 layer 状态已 stage/apply |
| 显示账 | SF latch、composition、HWC present fence | 哪个 Buffer 何时真正出现在屏幕 |

`finishDrawing` 定义在 Framework 的 `IWindowSession.aidl`。普通 App 用 Window Session Binder把“这一轮需要回报的客户端绘制债已经收口”交给 WMS；它不是 App 直接调用 SurfaceFlinger，更不是 present fence 回调。

同样，`mDrawState` 只存在于 system_server 的 `WindowStateAnimator`。它不等于 View dirty、RenderNode 录制状态、GraphicBuffer slot、SurfaceController 的 shown 值或 Activity 的 `reportedDrawn`。诊断时必须先说字段属于哪本账，再解释它能证明到哪里。

## 3. WMS 先创建隐藏 Surface；五态是可重入流程，不是一次性枚举

r48 的五个状态是：

| 状态 | 本层含义 | 关键否定 |
|---|---|---|
| `NO_SURFACE` | WMS 当前不把这扇窗视为持有可用窗口 Surface | 不保证 SF 层级中绝无保留旧 layer |
| `DRAW_PENDING` | Surface 已建立绘制目标，WMS 等客户端完成报告 | 不保证 App 已开始 draw |
| `COMMIT_DRAW_PENDING` | WMS 接受了从 DRAW_PENDING 发来的 finish | 尚未由 placement 提交为 drawn |
| `READY_TO_SHOW` | placement 已提交 draw 完成，`isDrawnLw()`开始为 true | 仍可能被 Activity、transition、policy 或可见性挡住 |
| `HAS_DRAWN` | `performShowLocked()` 已通过自身状态门并安排显示 | show 调用可能尚未执行或失败，更不是 present |

首次创建时，`createSurfaceLocked()`先 `setHasSurface(false)`，再调用 `resetDrawState()`把状态置为 DRAW_PENDING；创建 SurfaceControl 的初始 flag包含 `SurfaceControl.HIDDEN`。只有构造成功后 `mHasSurface`才置 true；资源不足或其他构造异常会把状态退回 NO_SURFACE。若方法一开始就发现已有 `mSurfaceController`，则直接返回旧controller，不执行这次reset。

`resetDrawState()`还会在窗口属于 Activity 且 Activity 当前不处于 transition 动画时清 `allDrawn`。这防止新一轮 Surface 沿用上一轮 Activity 聚合结果，但它也说明 draw state 和 Activity state 仍是两级账。

最小状态图是：

```text
NO_SURFACE --create成功--> DRAW_PENDING
DRAW_PENDING --有效finish--> COMMIT_DRAW_PENDING
COMMIT_DRAW_PENDING --placement--> READY_TO_SHOW
READY_TO_SHOW --全部显示门通过--> HAS_DRAWN
任意有Surface状态 --destroy--> NO_SURFACE
HAS_DRAWN/READY/... --orientation、drag resize或显式等待重画--> DRAW_PENDING
```

代码还允许 READY_TO_SHOW 在多轮 placement 中原地等待；`commitFinishDrawingLocked()`也接受 COMMIT 与 READY 两种输入。它不是只走一次且永不回头的线性状态机。

## 4. FIRST_TIME、resized reportDraw、BLAST 与显式请求是四种回报需求

最常见需求来自同步 relayout。`relayoutVisibleWindow()`在“此前不可见”或“当前还未 `isDrawnLw()`”时加 `RELAYOUT_RES_FIRST_TIME`；格式无法原地修改、drag resize需要保留旧 Surface 时也会再加这个 bit。

所以 FIRST_TIME 不是 WindowState 对象一生仅一次，也不保证收到它时状态恰好是 DRAW_PENDING。一个已画过但重新从不可见变可见的窗口仍可得到它；之后的 `finishDrawing()`可能因状态已是 HAS_DRAWN而不推进五态。

WMS还有一条反向请求：`IWindow`声明为oneway，所以普通跨进程客户端异步接收；同进程local Stub则可能在调用线程内联进入客户端。`WindowState.reportResized()`先计算

```java
reportDraw = drawState == DRAW_PENDING
        || useBLASTSync()
        || !mRedrawForSyncReported;
```

随后通过oneway `IWindow.resized(...)`发给客户端。`ViewRootImpl.W`把参数转给 `dispatchResized()`；drag-resize且使用多线程renderer时，后者可先在当前调用线程同步通知 `WindowCallbacks`，然后才选择 `MSG_RESIZED_REPORT`并投递App UI Handler。无论跨进程还是同进程，真正的 `reportNextDraw()`都在消息处理时执行。若调用发生在同一进程，代码复制可变Rect与Configuration，避免发送方继续复用同一对象。

四种需求并排看更清楚：

| 来源 | 客户端入口 | 是否保证 WMS 为 DRAW_PENDING |
|---|---|---|
| relayout FIRST_TIME | traversal处理 relayout结果 | 否；“此前不可见”也可触发 |
| `resized(reportDraw=true)` | `MSG_RESIZED_REPORT` | 否；BLAST或首次sync report也可触发 |
| `RELAYOUT_RES_BLAST_SYNC` | `reportNextDraw()` + `setUseBLASTSyncTransaction()` | 否；它还服务另一套transaction同步账 |
| `setReportNextDraw()` | 本地 `reportNextDraw()`并 `invalidate()` | 否；这是给SystemUI/WMS交互使用的隐藏接口 |

`reportResized()`在调用 client 前就把 `mRedrawForSyncReported=true`；RemoteException时不会靠这个字段自动重发同一请求。需求的产生、客户端收到消息、客户端结债和WMS状态接受必须分别观察。

## 5. reportNextDraw只给根请求加一笔债；真正协议是计数归零

核心代码很短：

```java
private void reportNextDraw() {
    if (mReportNextDraw == false) {
        drawPending();
    }
    mReportNextDraw = true;
}

void drawPending() {
    mDrawsNeededToReport++;
}
```

`mReportNextDraw`是“下一轮 draw带回报职责”的门。它从 false变 true时只为根请求加一次；保持 true期间的重复请求会合并，不再重复加根债。因此它不是帧数，也不是完成标志。

`mDrawsNeededToReport`才是余额。根 View、SurfaceView 等参与者可以继续增加它；每个完成者调用：

```java
if (mDrawsNeededToReport == 0) {
    throw new RuntimeException(
            "Unbalanced drawPending/pendingDrawFinished calls");
}
mDrawsNeededToReport--;
if (mDrawsNeededToReport == 0) {
    reportDrawFinished();
}
```

在没有其他未结余额时多结一次会直接抛异常，少结一次则不会进入 `reportDrawFinished()`；若存在重叠余额，多余旧完成也可能悄悄减掉另一笔债。这段协议本身没有“某个回调超时后替它减一”的逻辑；外部的transition、freeze或其他timeout也不能改写成同一笔客户端计数已正确闭合。

当上一轮异步回报尚未执行、`mReportNextDraw`已经在 `performDraw()`尾部清为 false时，新请求可以再加一笔根债。计数器因此能表达重叠余额，但这里没有给每笔债附带可见的generation id；阅读竞态必须沿具体callback和余额变化，而不是只看boolean。

## 6. report需求可穿透stopped与display-off，却不能穿透不可见和PreDraw取消

`performTraversals()`对 report有几处特别放行：

- layout条件使用 `!mStopped || mReportNextDraw`，所以欠报告时 stopped window仍可 measure/layout；
- `performDraw()`只在 display off且没有report时早退，因此report可以让关屏状态继续完成协议；
- `fullRedrawNeeded = mFullRedrawNeeded || mReportNextDraw`，正常report路线会强制整窗dirty；
- 硬件绘制前会暂时 `threadedRenderer.setStopped(false)`，尾部再恢复到 `mStopped`。

但 report不是无条件通行证。traversal先计算：

```java
cancelDraw = dispatchOnPreDraw() || !isViewVisible;
```

可见窗口的 PreDraw listener取消时，只重新 `scheduleTraversals()`；`mReportNextDraw`和根债保留到后续成功draw。窗口不可见时不会因report强行执行 `performDraw()`。若 `mView==null`，`performDraw()`也会在清flag和结债之前返回。

这组边界解释了两类“卡在客户端”的现场：一类是请求已经登记，但下一轮traversal反复被 PreDraw取消；另一类是View/可见性/Surface生命周期已改变，根本没有进入能清理该flag的尾段。只看到 `D_arm`不能推导 `V_draw`。

## 7. 硬件路径用异步callback结根债；callback不是GPU或present fence

进入 `performDraw()`后，代码先把当前值捕获到局部 `reportNextDraw`。后续异步callback据此决定是否调用 `pendingDrawFinished()`，不会再读取届时的全局boolean；但真正递减的仍是无generation的总余额，所以这个局部值也不能标识某一笔具体债。

ThreadedRenderer启用时，只要本轮含BLAST sync、frame-commit callback或report需求，就安装 frame-complete callback。正常report路线的顺序是：

```text
UI线程设置callback
→ ThreadedRenderer.draw把工作交给RenderThread
→ CanvasContext draw/swap
→ RenderThread侧Java callback
→ finishBLASTSync
→ UI Handler.postAtFrontOfQueue
→ pendingDrawFinished
```

`postAtFrontOfQueue`不会中断当前正在执行的UI消息；它只让回调在能够再次取消息时优先。`performDraw()`尾部先把 `mReportNextDraw=false`，根债仍要等这个UI callback才真正归零。

若 `draw()`返回不能使用异步回报，代码撤掉callback、执行 `finishBLASTSync(true)`；尾段只有在“根SurfaceHolder存在且根Surface有效”时才改走下一节的holder callback，否则用 `ThreadedRenderer.fence()`加同步收口并直接 `pendingDrawFinished()`。这里 `draw()`的boolean回答“能否走异步报告”，不是一张通用的“像素绘制成功证书”；软件绘制成功也返回false，Surface无效或硬件重建失败同样可落到非异步直接结债路径。

HWUI自身进一步限制了callback语义：`CanvasContext`在swap后留下“是否应使用真正completion fence”的疑问；callback在 `didSwap`时触发，而通用的skip-empty-frame分支也会主动触发callback，避免等待者永远悬挂。`reportNextDraw`通常强制full redraw，所以固定首次路线不是空dirty分支；但callback这个机制本身仍不能升级成GPU fence、SF latch或HWC present。

`DrawFrameTask`会先把callback移入`CanvasContext`。若本帧`canDrawThisFrame=false`，或真正draw后`didSwap=false`，本帧不会调用该callback；它可留到以后一次成功swap或空帧收口。RT callback本身与UI侧`ThreadedRenderer.draw()`何时返回没有通用先后，只有它投递的UI Runnable不能穿过当前仍在执行的traversal。

## 8. WindowCallbacks、根SurfaceHolder与SurfaceView用三种方式加入等待

“Decor draw结束”不是所有窗口内容都完成。r48另有三套协调：

| 参与者 | 怎样加入 | 怎样释放 | 与根计数的关系 |
|---|---|---|---|
| `WindowCallbacks` | `requestDrawWindow()`按callback数创建 `CountDownLatch`并调用 `onRequestDraw` | callback调用 `reportDrawFinish()`做 `countDown` | 不直接增加 `mDrawsNeededToReport`；UI线程在结根债前阻塞等latch |
| 根 `SurfaceHolder` 且根Surface有效 | ViewRoot自己不画Surface，创建 `SurfaceCallbackHelper` | helper完成计数达到expected后发 `MSG_DRAW_FINISHED` | 释放根请求原有的那笔债；Surface无效时不走helper |
| 子 `SurfaceView` | 每次 `redrawNeeded`先 `viewRoot.drawPending()` | helper完成计数达到expected后经 `runOnUiThread()`条件调度，再调用 `viewRoot.pendingDrawFinished()` | 为每个待报告SurfaceView额外加债；Handler为空时可能当前线程内联 |

`SurfaceCallbackHelper`对 `Callback2`调用 `surfaceRedrawNeededAsync(holder, drawingFinished)`；默认实现会同步调用旧 `surfaceRedrawNeeded()`再执行完成Runnable。不是 `Callback2`的项在这个helper里直接计为收齐；没有callback时也立即完成。

WindowCallbacks的latch没有本地timeout。以DecorView为例，有BackdropFrameRenderer时把请求交给它；没有renderer且需要report时，attached状态下会立即 `reportDrawFinish()`。若某个参与者不按约定回调，UI可卡在 `await()`；若等待被interrupt，代码只记日志并继续。latch的完成接口既不校验参与者身份也没有generation token：同一参与者重复countDown可冒充另一参与者提前放行，迟到旧完成也可能命中新一轮latch。

SurfaceView则维护自己的 `mPendingReportDraws`。完成Runnable可从任意线程到来，`onDrawFinished()`再调用 `runOnUiThread()`：Handler存在且当前Looper不同时才post，已经在同一Looper时内联，Handler为null时也在当前线程内联。因此正常attached路径通常回到UI Looper，detach后的迟到callback却没有这项线程保证。detach还会主动循环清空尚欠的本地report并偿还ViewRoot余额；所以即使`mReportNextDraw=false`，SurfaceView单独增加的债归零也能触发`finishDrawing()`，而且“detach收口”不代表新像素产生。

`SurfaceCallbackHelper`没有timeout、callback身份去重、one-shot或generation保护：Callback2不执行完成Runnable会一直欠账，同步抛异常会中断收集；同一callback重复执行可能先替尚未完成者提前凑满计数，而计数达到expected后的每个后续完成又会再次执行最终Runnable。对根holder而言，后续重复的 `MSG_DRAW_FINISHED`若遇不到新债会触发unbalanced异常；若恰有新一代债，反而可能错误偿还新债。SurfaceView自己的多余完成在本地余额为零时会被挡住并记错误，但迟到旧callback若撞上新一代`mPendingReportDraws>0`，也可能误偿还新账。由此可见，`mDrawsNeededToReport==0`表示参与协议的回调已按算术收齐，不验证参与者身份、代际归属或每个生产者是否真的画了正确像素。

## 9. reportDrawFinished跨的是同步AIDL边界，返回值却不给App成功证明

余额归零后，ViewRoot调用：

```java
mWindowSession.finishDrawing(mWindow, mSurfaceChangedTransaction);
```

`IWindowSession`本身不是 oneway，`finishDrawing`也没有 oneway修饰。对普通跨进程 App，这是同步Binder调用；与之相反，WMS回调客户端的 `IWindow`整个接口是 oneway。system_server内的窗口客户端还可能走同进程本地调用，所以“必经Binder驱动线程切换”也不是普遍事实。

同步只说明调用方等待服务端方法返回。接口返回 `void`，ViewRoot还吞掉 `RemoteException`；App既拿不到“WindowState是否仍存在”，也拿不到“draw state是否从DRAW_PENDING前进”的布尔确认。

`mSurfaceChangedTransaction`可携带客户端希望与draw完成同步的Surface变更。普通非BLAST首次finish时，WindowStateAnimator会把它merge到 `mPostDrawTransaction`，直到真正show时再并入WMS全局事务；若draw state已不是DRAW_PENDING，非空transaction反而会被直接 `apply()`，而五态保持不变。

WMS在服务端锁内调用 `requestTraversal()`后才结束这次调用；该请求可能与已有调度合并，也可能因layout defer只记为延迟请求，并不保证本次新投一个Handler消息。因此逻辑请求点 `Q_place < B_return`，但 `B_return`与AnimationThread真正处理placement没有通用先后：global lock释放后可以竞速。同步Binder返回更不能覆盖稍后的transaction apply、SF latch或present。

`doDie()`还有一条清理特例：移除ViewRoot前若最后一次relayout返回FIRST_TIME，会直接用null transaction调用 `finishDrawing`，避免服务端继续等一扇马上销毁的窗。这再次说明finish是协议收口，不等于新Buffer产生。

## 10. Session只转发；WMS在global lock里决定这次finish是否有效

`Session.finishDrawing()`是薄入口，直接调用 `WindowManagerService.finishDrawingWindow()`。WMS先 `Binder.clearCallingIdentity()`，再在 `mGlobalLock`内按Session和 `IWindow`查 WindowState。

| 服务端现场 | WMS结果 | App可见结果 |
|---|---|---|
| client已无对应WindowState | 查找返回null，什么也不推进 | 同步void仍正常返回 |
| state正是DRAW_PENDING | `win.finishDrawing()`返回true | 请求wallpaper/layout/placement，void返回 |
| state不是DRAW_PENDING | 常规状态不变；非空post transaction可能直接apply | void返回，无法区分重复/过期 |
| BLAST sync活跃 | transaction先进入BLAST同步账，再尝试普通draw-state推进 | void返回，不代表sync listener已完成 |

只有 `win.finishDrawing(...)`返回true时，WMS才根据wallpaper flag补layout change、调用 `setDisplayLayoutNeeded()`并 `requestTraversal()`。后者若发现 `mTraversalScheduled=true`就直接合并返回；否则先置scheduled，layout defer时增加延迟请求计数，未defer时才把 `mPerformSurfacePlacement`投到WMS AnimationThread。

placement还可能被“正在layout”、等待configuration、display尚未ready等条件推迟。由此得到严格下界：有效finish最多在本次同步调用内证明 `W_commit`与 `Q_place`；无效finish连COMMIT都不能证明。

WMS这里不读取BufferQueue的frame number是否已latch，也不等待present fence。它信任窗口协议，再把显示一致性交给后续状态、聚合与Surface事务。

## 11. finishDrawingLocked只认DRAW_PENDING；重复回报仍可能处理transaction

非BLAST窗口最终进入 `WindowStateAnimator.finishDrawingLocked()`。核心规则是：

```text
state == DRAW_PENDING
  → state = COMMIT_DRAW_PENDING
  → merge非空postDrawTransaction
  → return true

state != DRAW_PENDING
  → draw state不变
  → 非空postDrawTransaction立即apply
  → return false
```

所以 `finishDrawing`不是“把任意状态推进一格”。WMS收到时若状态已不是DRAW_PENDING，重复callback、重新可见窗口的FIRST_TIME或普通迟到回报都不会推进五态；窗口已销毁且查找不到时同样被丢弃。但接口没有generation：旧callback若恰好撞上新一代DRAW_PENDING，服务端会把它当成当前有效finish并错误推进到COMMIT。

`COMMIT_DRAW_PENDING`之所以单列，是为了把“客户端报告已被WMS接受”和“WMS在统一placement事务中提交该报告”分开。它已经满足 `isDrawFinishedLw()`，却还不满足 `isDrawnLw()`。

post-draw transaction也不能代替五态。首次有效finish的transaction可能一直留在 `mPostDrawTransaction`，直到Surface真正show；重复finish携带的新transaction则可立即apply，即使state不动。诊断“transaction已apply”与“Window已READY/HAS”必须取不同证据。

## 12. BLAST sync在五态旁边再开一套transaction完成账

本章固定路线排除BLAST，但r48已经有可选同步支线。WMS在relayout结果加 `RELAYOUT_RES_BLAST_SYNC`后，ViewRoot会同时：

```text
reportNextDraw
→ setUseBLASTSyncTransaction
→ mSendNextFrameToWm = true
```

下一帧由RenderThread使用 `mRtBLASTSyncTransaction`。frame-complete callback执行 `finishBLASTSync(!mSendNextFrameToWm)`：若这笔sync要回WMS，RT transaction会merge进 `mSurfaceChangedTransaction`，再随 `finishDrawing`送回；其他请求可以在客户端直接apply。

服务端 `WindowState.finishDrawing()`若发现 `mUsingBLASTSyncTransaction`，先把post transaction merge进 `mBLASTSyncTransaction`，置 `mNotifyBlastOnSurfacePlacement=true`，然后仍调用 animator 的普通finish函数，但传null transaction。也就是说：

- BLAST transaction ready与WindowState draw state是两本账；
- state只有原本为DRAW_PENDING时才到COMMIT；
- `notifyBlastSyncTransaction()`在 `prepareSurfaces()`阶段通知等待者，不是present通知；
- view visibility变GONE、窗口remove或BLAST timeout会调用 `immediatelyNotifyBlastSync()`，用于防止同步集合永久等待，不证明新内容画出。

不能把BLAST timeout写成“强制窗口显示”，也不能把 `onTransactionReady`写成HWC完成。它只闭合一组待合并SurfaceControl transaction的协调协议。

## 13. placement先把COMMIT变READY；普通Activity常需下一轮才能HAS

有效finish请求placement后，DisplayContent遍历有Surface的窗口并调用：

```java
winAnimator.commitFinishDrawingLocked();
activity.updateDrawnWindowStates(w);
```

`commitFinishDrawingLocked()`接受 COMMIT_DRAW_PENDING或READY_TO_SHOW，先统一写READY。无Activity的窗口可以立即尝试 `performShowLocked()`；starting window也被特判立即尝试。普通Activity窗口则必须先满足：

```java
activity.canShowWindows()
    == allDrawn
       && !(isAnimating(PARENTS)
            && hasNonDefaultColorWindow());
```

在 `L_normal`的首轮placement里，`allDrawn`起初通常为false，所以主窗先停在READY；紧接着 `updateDrawnWindowStates()`把READY视为 `isDrawnLw()`并计入真实窗口。遍历结束后，`updateAllDrawn()`还要求`numInteresting>0`、所有相关子窗都已被评估、`numDrawn>=numInteresting`且`!isRelaunching()`，才置 `allDrawn=true`并要求额外layout pass。下一轮 `commitFinishDrawingLocked()`再次处理READY，才有机会进入HAS。

这不是所有窗口的绝对两轮定律：Activity可能已有满足的聚合状态，starting/non-Activity有特例，多窗口加入顺序、relaunch、freezing、transition与layout循环也会改变轮次。可证的是职责顺序：单窗READY先成为Activity计数输入，Activity聚合结果再反过来开放普通窗口show。

`COMMIT_DRAW_PENDING`若placement尚未执行会停留；`READY_TO_SHOW`若Activity聚合、广色域transition或后续可见性门不满足也会停留。下一章专门展开聚合字段与启动回调，本章只把它作为单窗五态之外的一道门。

## 14. HAS_DRAWN仍早于实际show；四个drawn谓词也不是同义词

`WindowState.performShowLocked()`的顺序很容易被方法名误导：

1. 先检查 `showToCurrentUser()`；
2. READY/HAS且属于Activity时，真实窗先调用 `onFirstWindowDrawn()`，starting窗调用 `onStartingWindowDrawn()`；
3. 再要求状态正是READY且 `isReadyForDisplay()`为true；
4. 应用enter animation，把状态置HAS_DRAWN并 `scheduleAnimationLocked()`；
5. 后续 `prepareSurfaceLocked()`才尝试 `showSurfaceRobustlyLocked()`并合并post-draw transaction。

`isReadyForDisplay()`还检查waiting-to-show transition、parent/client/token visibility、policy visibility、Surface存在、未destroy，以及动画兜底。即使状态已READY，它仍可能返回false。

HAS也不保证 `showSurfaceRobustlyLocked()`成功。`performShowLocked()`先写HAS；稍后的show失败时 `mLastHidden`仍可保留，后续prepare再重试。因此用dump看到HAS最多证明WMS状态机已越过show门，不能证明SurfaceController shown，更不能证明SF/HWC结果。

四个相似谓词的口径是：

| 谓词 | r48状态要求 | 还叠加的主要条件 |
|---|---|---|
| `isDrawFinishedLw()` | COMMIT、READY或HAS | 有Surface且未destroy |
| `isDrawnLw()` | READY或HAS | 有Surface且未destroy |
| `hasDrawnLw()` | 仅HAS | 不额外验证shown/present |
| `isDisplayedLw()` | 先满足 `isDrawnLw()` | policy、parent、Activity requested visibility或animation |

尤其 `isDisplayedLw()`允许READY，因此名字里的displayed也不是SurfaceController show回执。定位故障时可按第一处停点分类：

| 现场 | 首要怀疑 | 不能直接下的结论 |
|---|---|---|
| 长停DRAW_PENDING | report未到、PreDraw/参与者债未闭合、client消息丢失 | App一定没有queue任何Buffer |
| 长停COMMIT | placement被defer/配置门阻塞，或现场截在调度间隙 | finish没有进入WMS |
| 长停READY | Activity/allDrawn、transition、visibility或policy门 | Buffer没有生产 |
| HAS但Surface仍hidden | prepare/show失败或尚未处理transaction | present已经完成 |
| show与Buffer均有证据仍黑 | SF latch、composition、secure/alpha/z-order、HWC/present链 | 应回头重复调用finish |

真正的可见汇合是两条支路：`Q_buf → F_latch`提供内容，`W_has → X_show`提供显示状态；它们在SF合成中汇合后才可能到 `P_real`。任一Java字段都不能独自替代这个join。

## 15. 九组 macOS 只读源码练习

以下命令从AOSP根目录执行，只读文件，不要求编译或设备。每个 `rg -e`备选都应独立命中；行号只负责定位，真实顺序要按调用关系重建。

### 练习 1：确认隐藏Surface与五态初始值

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
rg -n -F -e 'static final int NO_SURFACE = 0' -e 'static final int DRAW_PENDING = 1' -e 'static final int COMMIT_DRAW_PENDING = 2' -e 'static final int READY_TO_SHOW = 3' -e 'static final int HAS_DRAWN = 4' -e 'void resetDrawState()' -e 'mDrawState = DRAW_PENDING;' -e 'int flags = SurfaceControl.HIDDEN;' -e 'mDrawState = NO_SURFACE;' frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
```

回答：state存在哪个进程？创建失败后为何不能保留DRAW_PENDING？HIDDEN把“可画”与“可见”分开到哪一层？

### 练习 2：列出四种report需求并区分线程

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e '!wasVisible || !isDrawnLw()' -e 'RELAYOUT_RES_FIRST_TIME' -e 'mWinAnimator.mDrawState == DRAW_PENDING || useBLASTSync()' -e 'mClient.resized' -e 'mRedrawForSyncReported = true' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'RELAYOUT_RES_BLAST_SYNC' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'MSG_RESIZED_REPORT' -e 'RELAYOUT_RES_FIRST_TIME' -e 'RELAYOUT_RES_BLAST_SYNC' -e 'private void reportNextDraw()' -e 'public void setReportNextDraw()' frameworks/base/core/java/android/view/ViewRootImpl.java
```

回答：FIRST_TIME为何不是对象一生一次？`resized`在哪个线程接收、在哪个线程置flag？哪些来源不要求state恰为DRAW_PENDING？

### 练习 3：验证boolean门与计数债的不同

```bash
test -f frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'boolean mReportNextDraw;' -e 'int mDrawsNeededToReport = 0;' -e 'void drawPending()' -e 'mDrawsNeededToReport++;' -e 'void pendingDrawFinished()' -e 'Unbalanced drawPending/pendingDrawFinished calls' -e 'mDrawsNeededToReport--;' -e 'reportDrawFinished();' frameworks/base/core/java/android/view/ViewRootImpl.java
```

回答：重复 `reportNextDraw`何时合并？SurfaceView增加的是flag还是余额？多结和少结分别怎样表现？

### 练习 4：追PreDraw、full redraw与硬件异步回报

```bash
test -f frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e '!mStopped || mReportNextDraw' -e 'dispatchOnPreDraw() || !isViewVisible' -e 'mFullRedrawNeeded || mReportNextDraw' -e 'boolean reportNextDraw = mReportNextDraw' -e 'needFrameCompleteCallback' -e 'setFrameCompleteCallback' -e 'handler.postAtFrontOfQueue' -e 'setFrameCompleteCallback(null)' -e 'mAttachInfo.mThreadedRenderer.fence()' frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'Properties::skipEmptyFrames' -e 'Use a fence for real completion?' -e 'if (didSwap)' frameworks/base/libs/hwui/renderthread/CanvasContext.cpp
```

回答：PreDraw取消为何保留债？callback从哪条线程回到UI？为何frame-complete不是present fence？

### 练习 5：核对三类额外参与者怎样结账

```bash
test -f frameworks/base/core/java/android/view/SurfaceView.java
rg -n -F -e 'mPendingReportDraws++;' -e 'viewRoot.drawPending();' -e 'viewRoot.pendingDrawFinished();' -e 'runOnUiThread(this::performDrawFinished)' frameworks/base/core/java/android/view/SurfaceView.java
rg -n -F -e 'mWindowDrawCountDown = new CountDownLatch' -e 'onRequestDraw(mReportNextDraw)' -e 'mWindowDrawCountDown.await()' -e 'reportDrawFinish()' frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'mFinishDrawingExpected = callbacks.length' -e 'surfaceRedrawNeededAsync' -e 'mRunnable.run();' frameworks/base/core/java/com/android/internal/view/SurfaceCallbackHelper.java
```

回答：WindowCallbacks为何不直接加根计数？Callback2遗漏完成Runnable会卡哪一层？根SurfaceHolder与子SurfaceView各结哪笔债？

### 练习 6：证明resized是oneway而finishDrawing是同步void

```bash
test -f frameworks/base/core/java/android/view/IWindowSession.aidl
rg -n -F -e 'oneway interface IWindow' -e 'void resized' frameworks/base/core/java/android/view/IWindow.aidl
rg -n -F -e 'void finishDrawing(IWindow window' -e 'postDrawTransaction' frameworks/base/core/java/android/view/IWindowSession.aidl
rg -n -F -e 'mWindowSession.finishDrawing' -e 'catch (RemoteException e)' -e 'mSurfaceChangedTransaction' frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'public void finishDrawing' -e 'mService.finishDrawingWindow' frameworks/base/services/core/java/com/android/server/wm/Session.java
```

回答：普通App在哪个方向等待Binder reply？void返回为何不证明WindowState仍存在？system_server本地窗口为何未必切驱动线程？

### 练习 7：手推WMS接纳、重复finish与post transaction

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'void finishDrawingWindow' -e 'Binder.clearCallingIdentity()' -e 'windowForClientLocked(session, client, false)' -e 'win.finishDrawing(postDrawTransaction)' -e 'win.setDisplayLayoutNeeded()' -e 'mWindowPlacerLocked.requestTraversal()' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'if (mDrawState == DRAW_PENDING)' -e 'mDrawState = COMMIT_DRAW_PENDING;' -e 'mPostDrawTransaction.merge' -e 'postDrawTransaction.apply();' -e 'return layoutNeeded;' frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
```

回答：哪种finish才调用 `requestTraversal()`请求placement？重复finish携带transaction时哪本账变、哪本账不变？App怎样区分这些结果？

### 练习 8：从placement追到READY、Activity门与真实show

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
rg -n -F -e 'commitFinishDrawingLocked()' -e 'activity.updateDrawnWindowStates(w)' -e 'activity.updateAllDrawn()' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
rg -n -F -e 'boolean canShowWindows()' -e 'allDrawn &&' -e 'mDisplayContent.setLayoutNeeded()' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'boolean performShowLocked()' -e 'onFirstWindowDrawn' -e '!isReadyForDisplay()' -e 'mDrawState = HAS_DRAWN' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'showSurfaceRobustlyLocked()' -e 'mergeToGlobalTransaction(mPostDrawTransaction)' frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
```

回答：普通首窗为何常先停READY？`onFirstWindowDrawn`为何早于真正show尝试？HAS后还有哪两级证据才到present？

### 练习 9：比较四个谓词并闭合BLAST逃生路径

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'boolean isReadyForDisplay()' -e 'public boolean isDisplayedLw()' -e 'public boolean isDrawFinishedLw()' -e 'public boolean isDrawnLw()' -e 'public boolean hasDrawnLw()' -e 'mUsingBLASTSyncTransaction' -e 'mNotifyBlastOnSurfacePlacement = true' -e 'void immediatelyNotifyBlastSync()' -e 'mWmService.mH.sendNewMessageDelayed' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'WINDOW_STATE_BLAST_SYNC_TIMEOUT' -e 'ws.immediatelyNotifyBlastSync()' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

回答：COMMIT、READY、HAS分别让哪些谓词为true？`isDisplayedLw`为何可早于show？BLAST timeout关闭的是哪本账？

## 16. 用“债—状态—聚合—事务—物理”五问收口，并把下一章边界留清

先记住八条不变量：

1. `reportNextDraw`第一次只增加一笔根债；重复flag会合并，SurfaceView等参与者才能继续增加余额。
2. report需求有FIRST_TIME、resized、BLAST与显式接口四个来源；它们都不保证WMS当时恰为DRAW_PENDING。
3. PreDraw取消时债可保留；stopped与display-off在有report时可被穿透，不可见窗口仍不会强画。
4. 硬件frame-complete callback在report分支可经RenderThread到UI结清客户端债，同一callback还可处理BLAST sync与captured frame-commit callbacks；这些用途都不是GPU/SF/HWC fence。
5. 普通跨进程 `finishDrawing`是同步void AIDL；有效调用只把DRAW_PENDING推进到COMMIT并请求或登记placement，App拿不到成功位。
6. placement把COMMIT变READY；普通Activity还要经过 `allDrawn`、transition与visibility门，READY可停多轮。
7. `performShowLocked`先写HAS，`prepareSurfaceLocked`才真正尝试show；show成功也只是Surface transaction证据。
8. 内容支路 `Q_buf → F_latch` 与显示支路 `W_has → X_show`必须在SF汇合，可靠present证据不能由任何Java draw字段代替。

现场诊断按五问推进：

```text
客户端是否登记了report债，余额由谁持有？
→ finish是否真的让目标Window从DRAW到COMMIT？
→ placement是否把它推进READY，Activity聚合门是否打开？
→ HAS之后show transaction是否成功stage并提交？
→ 对应Buffer何时latch、compose并取得present fence？
```

这条链能把“App已调用finish”“WMS说drawn”“Activity整体可展示”“Surface已show”和“像素已present”拆成可核对的完成点。下一章继续上移一层：Android Activity `allDrawn`、`reportedDrawn`、`nowVisible`与启动完成回调。
