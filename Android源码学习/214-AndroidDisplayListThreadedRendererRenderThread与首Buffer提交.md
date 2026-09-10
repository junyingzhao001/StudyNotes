# 214 Android DisplayList、ThreadedRenderer、RenderThread 与首 Buffer 提交

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只读源码，可以证明 App 主线程、RenderThread、HWUI、BufferQueue 与 WMS/SF 之间的源码约束；不能据此测量某台设备的 GPU 完成、SurfaceFlinger latch 或显示硬件 present 时间。

第 213 章停在普通 Activity 首次 `performTraversals()`：View 树已经 attach、measure、relayout、layout，App 也接好了可用 `Surface`，随后进入硬件 `performDraw()`。这仍不是“首帧已经上屏”。

本章只追一个问题：**UI 线程究竟记录了什么，RenderThread 何时可以放行 UI，首个 GraphicBuffer 怎样被绘制并交还生产者队列，以及 frame-complete、WMS show、SF latch 与 present 为什么必须分账？**

## 1. 固定首个硬件帧，用二十二个完成点代替一条伪全序

先固定 `F_target`。若不先限定分支，“DisplayList 画完”“交换完成”“首帧完成”都会在不同语境里指向不同事件。

| 维度 | 固定值或前提 |
|---|---|
| 上游 | 沿用第 213 章的普通首次可见 Activity 主窗口；首次 traversal 已完成 layout，`mReportNextDraw=true` |
| Surface | Java `Surface` 有效，ThreadedRenderer 已初始化；不是 `SurfaceHolder` 接管路径 |
| 渲染 | ThreadedRenderer 已请求且 enabled；本次有完整 damage，无软件 Canvas、无截图 renderer；所选 GL/Vulkan提交成功，Vulkan semaphore提交与 FD导出也成功 |
| 同步 | `prepareTextures=true`，所以 RenderThread 可以提前 signal UI；`canDrawThisFrame=true`，二者是本章刻意固定的两个独立条件 |
| 队列 | 主线继续使用 r48 默认的非 BLAST、非 shared窗口 BufferQueue；无断连、超时、slot校验失败或 ReliableSurface fallback |
| 回调 | View/Window draw callback正常返回；无 child SurfaceView或其他参与者增加 draw debt；frame-complete callback和主 Handler Runnable均不抛异常 |
| 合成 | acquire fence 正常 signal，SF 最终 latch 目标 Buffer；WMS 显示门放行，目标帧最终有独立 present 证据 |
| 并发 | 窗口不被并发 remove、hide、resize 或 replace；不把厂商 EGL/Vulkan 驱动内部位置冒充 AOSP 公共顺序 |

二十二个完成点如下：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `D_gate` | `ViewRootImpl.draw()` 选择 enabled 的硬件分支，消费 Java dirty 并调用 ThreadedRenderer | View 的命令已记录 |
| `R_view` | 业务根及本次失效子树的 RenderNode DisplayList 已重录或确认复用 | 窗口级 root node 已就绪 |
| `R_root` | ThreadedRenderer 自己的 root RenderNode 已重录或确认复用 | native 当前渲染树已接纳 staging 状态 |
| `J_copy` | JNI 已校验并复制 `FrameInfo`，进入同进程 `RenderProxy` | RenderThread 已开始任务 |
| `Q_post` | `DrawFrameTask` 已投进 RenderThread 队列 | UI已经释放 task互斥量，或 RenderThread已开始任务 |
| `Q_wait` | UI进入条件变量等待协议并原子释放 task互斥量 | RenderThread的同步阶段才刚开始 |
| `T_sync` | RenderThread 完成 `makeCurrent`、layer apply、`prepareTree()` 与结果判定；固定路径可绘制 | UI 线程已经实际恢复执行 |
| `U_signal` | `prepareTextures=true`，RenderThread 对等待条件发 signal | UI 已抢到 CPU，或 RenderThread 已停止向前 |
| `U_resume` | UI 从条件变量醒来，`syncAndDrawFrame()`经 native/JNI 返回 Java | 包住它的 `performDraw()`与 traversal 已返回 |
| `D_return` | ThreadedRenderer处理 sync result后，本次 `performDraw()`与 traversal 已交还主 Looper | RenderThread 已 queue Buffer |
| `B_lease` | 后端已 dequeue目标 Buffer，并取得/建立 producer必须遵守的上一轮 release fence依赖 | 该 fence此刻已 signal，或 GPU已写完本帧 |
| `G_issue` | Skia 回放目标 RenderNode 树并 flush 本帧图形命令 | GPU 已完成，或 Buffer 已被 SF 接收 |
| `Q_accept` | `BufferQueueProducer` 在锁内验证 slot，并把它从 DEQUEUED 改成 QUEUED、放入队列 | `queueBuffer()` 已返回，或 consumer 已 latch |
| `Q_return` | 正常 pipeline queue/swap路径已返回 CanvasContext，且固定路径中的真实 queue已成功 | acquire fence 已 signal，或画面已显示 |
| `G_ready` | 随 Buffer 提交的 GPU 完成 fence 已 signal，consumer 可安全读目标内容 | 图层已经可见或已被 SF 选中 |
| `C_raw` | 固定成功路径的 CanvasContext 看到 `didSwap=true`，调用原始 frame-complete callback | UI 主线程已执行回调投递的 Runnable |
| `M_run` | 主 Handler 开始执行队首 Runnable，调用 `pendingDrawFinished()` 并运行 commit callbacks | WMS 已 show 图层 |
| `F_commit` | 同步 `finishDrawing()` 在 WMS 内把窗口绘制状态推进到 `COMMIT_DRAW_PENDING` | SurfaceControl show 已提交 |
| `W_show` | 后续 WMS surface placement 通过显示门并提交窗口图层 show 事务 | 目标 Buffer 已 latch |
| `L_latch` | SF 为目标图层 acquire/latch 了该 Buffer | 它已参与可见合成 |
| `C_submit` | 目标 Buffer与 show等控制状态汇合，已被选入一次可见合成提交 | acquire fence已 signal，或显示硬件已 present |
| `P_present` | 目标帧由 present fence、Surface frame timestamps或 SF TimeStats等证明确已 present | 更早阶段没有性能问题 |

UI 侧的固定顺序是：

```text
D_gate < R_view < R_root < J_copy < Q_post
Q_post < Q_wait
Q_post < T_sync
{Q_wait, T_sync} < U_signal < U_resume < D_return
```

RenderThread 的正常提交支线是：

```text
U_signal < G_issue < Q_accept < Q_return < C_raw
B_lease < Q_accept
G_issue < G_ready
```

这里故意没有写 `U_resume < G_issue`、`D_return < G_issue`或 `C_raw < U_resume`。signal以后，UI与 RenderThread谁先继续取决于调度；原始 callback甚至可以在 UI还没从 `syncAndDrawFrame()`返回时发生，也可以等 traversal返回后才发生。只有投回同一主 Looper的 Runnable必须同时等待 callback完成投递和当前 traversal交还线程：

```text
{D_return, C_raw} < M_run < F_commit < W_show
```

生产与显示支线也不是一条链：

```text
Q_accept < L_latch
{W_show, L_latch} < C_submit < P_present
G_ready < P_present
```

`Q_accept`与 `G_ready`没有固定先后：producer可以先把带未 signal fence的 Buffer入队，也可能 GPU很快完成后 queue才走到队列状态更新。`L_latch`与 `G_ready`也不能统一排序：传统 BufferQueueLayer的 droppable队头或调试开关允许先 latch并继续携带 fence；非 droppable默认路径才会在 latch门等待。无论哪种分支，最终 present不能越过目标内容的安全读取依赖。

`W_show`与 `L_latch`同样没有固定先后：隐藏图层可以先 latch，show也可以先到而继续等内容。二者在 `C_submit`汇合；合成提交本身仍可携带未 signal的 acquire fence，所以这里只把 `G_ready`硬排在 `P_present`前。

`B_lease`也不能统一塞到 `U_signal`后。Vulkan路径在 `CanvasContext.draw()`的 `getFrame()`中显式 dequeue并建立对旧 release fence的等待依赖，因此固定提前 signal路径满足 `U_signal < B_lease < G_issue`；这不要求 CPU等到旧 fence signal才继续 issue。OpenGL的 ANativeWindow取 Buffer可能被 EGL安排在 `makeCurrent`、begin-frame、首次绘制或 swap附近，AOSP上层不给它与 `U_signal/G_issue`的跨实现全序，只能保证真实 dequeue先于同一 Buffer的 `Q_accept`。

## 2. ViewRootImpl 选择硬件分支：dirty 被消费，不是 Buffer 被清空

`performDraw()`先决定是否需要 Renderer 完成回调。首次窗口仍有 `mReportNextDraw`，所以会捕获 frame commit callbacks，并给 ThreadedRenderer 注册 `FrameCompleteCallback`：

```java
final boolean needFrameCompleteCallback =
        mNextDrawUseBLASTSyncTransaction
        || (commitCallbacks != null && commitCallbacks.size() > 0)
        || mReportNextDraw;

mAttachInfo.mThreadedRenderer.setFrameCompleteCallback(frameNr -> {
    finishBLASTSync(!mSendNextFrameToWm);
    handler.postAtFrontOfQueue(() -> {
        if (reportNextDraw) {
            pendingDrawFinished();
        }
        // 依次执行本轮捕获的 commit callbacks
    });
});
```

这段代码已经分出两个线程边界：native frame-complete 触发 Java callback 时仍在 RenderThread 执行语境；真正操作 ViewRoot draw 计数和应用 callback 的工作被投回主 Handler。

随后进入 `ViewRootImpl.draw()`。主线先过三道门：

```java
if (!mSurface.isValid()) {
    return false;
}

if (mSurfaceHolder != null) {
    dirty.setEmpty();
    return false;
}

if (mAttachInfo.mThreadedRenderer != null
        && mAttachInfo.mThreadedRenderer.isEnabled()) {
    // 硬件路径
}
```

`Surface.isValid()`只说明 wrapper 仍连接有效 native 对象；它不证明已有可写 Buffer。`mSurfaceHolder != null`则表示根内容生产者另有其人，ViewRoot 不走本章渲染链。

首次 `fullRedrawNeeded` 把窗口范围放入 `mDirty`。`dispatchOnDraw()`也在选择硬件/软件分支前运行，所以 `OnDrawListener` 仍在 UI 线程，既不是 RenderThread 回调，也不是提交完成通知。一般路径中它甚至早于后面的 `dirty || animating || accessibilityFocusDirty` 门；listener 被通知后，本次仍可能没有实际 renderer/software draw。

surfaceInsets 会同时改窗口级 x/y offset 和 dirty 坐标。r48 此处源码是：

```java
xOffset -= surfaceInsets.left;
yOffset -= surfaceInsets.top;
dirty.offset(surfaceInsets.left, surfaceInsets.right);
```

第二个 `offset()` 参数写的是 `right` 而不是 `top`。这是本版本源码的精确事实，读 trace 或回溯局部 damage 时应按实际代码核对，不能用对称直觉替换它。

进入硬件分支后：

```java
dirty.setEmpty();
useAsyncReport = true;
mAttachInfo.mThreadedRenderer.draw(mView, mAttachInfo, this);
```

清空 `mDirty` 表示 ViewRoot 已消费本轮 Java 失效债；后续 damage 由 RenderNode、DamageAccumulator、buffer age 与 Surface 状态继续计算。它不是清空 GraphicBuffer，也不是声明本帧无内容。

`useAsyncReport=true`只表示 ViewRoot 可以等 Renderer callback 再报告 WMS。`ThreadedRenderer.draw()`仍会同步等待 RenderThread 的一部分工作。

还要区分三种状态：

| 状态 | ViewRoot 行为 |
|---|---|
| renderer 不存在 | 可以走软件 `drawSoftware()` |
| renderer 存在、requested 但暂时 disabled | 尝试重新 initialize，排下一次 traversal；不会贸然用软件锁同一 Surface |
| Java renderer enabled，但 native context stopped | 仍进入硬件链；native同时返回 `ContextIsStopped`与 `FrameDropped`，不能把它近似成 Java `isEnabled()==false` |

固定首次报告还有一个覆盖规则：draw 前会暂时 `setStopped(false)`，报告处理结束后再与 `mStopped` 对齐。这解释了“停止状态”为什么不能只看某一个布尔值。

## 3. UI 线程记录 View DisplayList：RecordingCanvas 不是像素画布

`ThreadedRenderer.draw()`先标记 FrameInfo 的 DrawStart，再调用：

```java
updateRootDisplayList(view, callbacks);
```

第一步 `updateViewTreeDisplayList(view)`把业务根标成 drawn，依据 invalidated 位设置 `mRecreateDisplayList`，再进入 `View.updateDisplayListIfDirty()`。

View 能否拥有 DisplayList 的判断非常窄：

```java
public boolean canHaveDisplayList() {
    return !(mAttachInfo == null
            || mAttachInfo.mThreadedRenderer == null);
}
```

这里没有检查 renderer 当前是否 enabled。外层 ViewRoot 已经为正常绘制选好分支，但单独阅读该方法时不能把“有 ThreadedRenderer 对象”扩大成“此刻一定能输出硬件帧”。

一次节点更新有三条路：

| 条件 | 结果 |
|---|---|
| 节点无 DisplayList，或明确要求 recreate | 对当前 View重新录制 |
| cache无效，但已有 DisplayList且当前 View无需 recreate | 复用当前 View列表，并用 `dispatchGetDisplayList()`递归检查子节点 |
| cache有效、已有 DisplayList且无需 recreate | 更新标志并直接复用 |

父节点复用不等于整棵子树静止。`ViewGroup.dispatchGetDisplayList()`还会处理可见或有动画的普通 child、transient view、overlay 与 disappearing child；失效可以只让局部节点重录。

需要重录时，UI 线程取得的是命令记录器：

```java
final RecordingCanvas canvas =
        renderNode.beginRecording(width, height);
try {
    if (layerType == LAYER_TYPE_SOFTWARE) {
        // 把软件 drawing cache 作为位图命令记录进去
    } else {
        computeScroll();
        canvas.translate(-mScrollX, -mScrollY);
        if ((mPrivateFlags & PFLAG_SKIP_DRAW) != 0) {
            dispatchDraw(canvas);
        } else {
            draw(canvas);
        }
    }
} finally {
    renderNode.endRecording();
    setDisplayListProperties(renderNode);
}
```

普通自定义 View 的 `onDraw()`因此确实在 UI 线程发生；它调用的 `drawRect`、`drawText`、clip、translate 等操作主要被 RecordingCanvas 写成可回放命令，不是在窗口 GraphicBuffer 上立即栅格化。

`PFLAG_SKIP_DRAW`只跳过当前 View 自己的 `draw()`主流程；`dispatchDraw()`仍会把孩子组织进列表，overlay 和调试绘制也有各自分支。无背景的容器不是“整棵子树不画”。

`RenderNode.endRecording()`调用 `finishRecording()`，再把 native DisplayList 交给 `nSetDisplayList()`。JNI 最终写入 native RenderNode 的 staging DisplayList。只有 RenderThread 后续 `prepareTree()`同步后，当前渲染树才会使用它。

异常边界也值得单独记账：`View.draw()`抛异常时，`finally`仍会执行 `endRecording()`与属性设置；随后异常继续向外传播，正常 `syncAndDrawFrame()`不会到达。这里没有“本轮整棵 DisplayList 事务自动回滚”的承诺。

## 4. 两层根节点与 retained tree：重录命令、同步属性、输出帧彼此独立

业务根 View 自己有 RenderNode，ThreadedRenderer 还持有一个窗口级 `mRootNode`。后者只在 `mRootNodeNeedsUpdate` 或没有 DisplayList 时重录：

```java
RecordingCanvas canvas =
        mRootNode.beginRecording(mSurfaceWidth, mSurfaceHeight);
try {
    canvas.translate(mInsetLeft, mInsetTop);
    callbacks.onPreDraw(canvas);
    canvas.enableZ();
    canvas.drawRenderNode(view.updateDisplayListIfDirty());
    canvas.disableZ();
    callbacks.onPostDraw(canvas);
} finally {
    mRootNode.endRecording();
}
```

窗口级 root 承担 Surface 尺寸、Insets 平移、ViewRoot 前后绘制内容与 Z 组织；其中记录的是“回放业务根 RenderNode”的引用。业务子树则分别保存自己的命令和属性。

可以把对象关系缩成：

```text
ThreadedRenderer root RenderNode
├── Window/Insets 级变换与 ViewRoot callbacks
└── Decor/业务根 View 的 RenderNode
    ├── child A RenderNode
    ├── child B RenderNode
    └── overlay / disappearing child 等节点
```

这是 retained-mode 的价值：

- 文本或几何内容改变，可能需要重录某个 DisplayList；
- translation、alpha、matrix 等节点属性改变，可以不重新执行普通 Java `onDraw()`；
- 没有 UI 重录，RenderThread 动画仍可改变当前属性、制造 damage 并输出后续 Buffer；
- 多次 `invalidate()`可以在 traversal 前合并，一次 draw 也可能因空 damage 或错误不 queue 新 Buffer。

所以以下四个数量一般不相等：

```text
invalidate 次数
≠ Java onDraw 次数
≠ RenderThread draw 次数
≠ queueBuffer 次数
```

native `RenderNode`同时维护 staging properties/DisplayList 与 RenderThread 当前版本。UI 修改 staging；RenderThread 的 FULL prepare 把需要的属性与 DisplayList推进当前树，重建子节点引用并计算前后 damage。它不是简单把一块 Java 内存原样交给另一个线程。

## 5. Java、JNI、RenderProxy 与 RenderThread：同进程跨线程，不是 Binder

UI 录制结束后，ThreadedRenderer把同一帧 Choreographer `FrameInfo`交给父类：

```java
int syncResult = syncAndDrawFrame(choreographer.mFrameInfo);
```

`HardwareRenderer.syncAndDrawFrame()`只是薄封装：

```java
return nSyncAndDrawFrame(
        mNativeProxy,
        frameInfo.frameInfo,
        frameInfo.frameInfo.length);
```

JNI 先检查数组长度，再用 `GetLongArrayRegion()`把时间线复制进 `RenderProxy::frameInfo()`，然后调用 `proxy->syncAndDrawFrame()`。这里仍在 App 进程和 UI 调用栈上，没有 WMS/SF Binder。

对象与所有权如下：

| 对象 | 主要执行侧 | 本章职责 | 它不是 |
|---|---|---|---|
| `ThreadedRenderer` | App Java / UI | 把 ViewRoot 绘制协议接到 HardwareRenderer | 一条线程 |
| `RenderProxy` | App native / 调用侧 | 保存 CanvasContext 与 DrawFrameTask，向 RenderThread 排任务 | Binder proxy |
| `RenderThread` | App native 专用线程 | 串行执行 HWUI 同步、动画、Skia 与窗口提交 | SurfaceFlinger 线程 |
| `CanvasContext` | RenderThread | 管单个 renderer 的 Surface、pipeline、damage 与帧统计 | Android `Context` |
| `RenderNode` | Java/native 两侧 | 保存命令、属性、树关系及 staging/current 状态 | Java `View`本身 |

`RenderThread::getInstance()`返回进程内静态实例，构造时加载 HWUI 属性并 `start("RenderThread")`。因此通常是一个 App 进程共享一条 RenderThread，不是每个 View 或每个窗口各建一条；各 renderer 则有自己的 CanvasContext。

创建 RenderProxy 时，会在 RenderThread 队列上 `runSync` 创建 CanvasContext。`setSurface()`取得 ANativeWindow 引用后再 `post` 到同一队列；随后 draw task 排在同一串行队列，因而能观察先前 Surface 更新。这种队列顺序不等于跨进程事务，也不等于 Surface 已经有 Buffer。

## 6. DrawFrameTask 的同步协议是两个独立布尔轴，不是“异步画一帧”

`RenderProxy::syncAndDrawFrame()`进入可复用的 `DrawFrameTask`：

```cpp
mSyncResult = SyncResult::OK;
mSyncQueued = systemTime(SYSTEM_TIME_MONOTONIC);
postAndWait();
return mSyncResult;
```

`postAndWait()`持有内部互斥量，把 `run()`投进 RenderThread 队列，再在条件变量等待。它解释了 `Q_wait`：UI 并没有同步执行 GPU 绘制，但也不是 fire-and-forget。

RenderThread 的 `run()`先建立 `TreeInfo::MODE_FULL`，再分别读取两个结果：

```cpp
canUnblockUiThread = syncFrameState(info);
canDrawThisFrame = info.out.canDrawThisFrame;
```

第一个值就是 `info.prepareTextures`：RenderThread 是否已经安全消费本轮 UI staging 数据、允许 UI 继续修改下一帧状态。第二个值表示本次 vsync 是否应该实际调用 `CanvasContext.draw()`。两者彼此独立，完整状态矩阵是：

| `prepareTextures` | `canDrawThisFrame` | RenderThread 行为 | UI 何时被 signal |
|---|---|---|---|
| true | true | 正常 `draw()` | draw 前 |
| true | false | 不 draw，只 `waitOnFences()` | wait 前 |
| false | true | 正常 `draw()` | draw 后 |
| false | false | 不 draw，只 `waitOnFences()` | wait 后 |

把它理解成“可提前放行就画，否则不画”会错误合并两个轴。固定 `F_target`位于第一格；其余三格留到第 15 节结算。

frame-complete callback 在 `syncFrameState()`返回后被移入 CanvasContext，然后才可能 `unblockUiThread()`：

```cpp
if (mFrameCompleteCallback) {
    mContext->addFrameCompleteListener(
            std::move(mFrameCompleteCallback));
}
if (canUnblockUiThread) {
    unblockUiThread();
}
if (canDrawThisFrame) {
    context->draw();
} else {
    context->waitOnFences();
}
```

signal 只让等待者变成可运行。RenderThread 可以继续执行 draw，UI 也可以先抢到 CPU；源码没有赋予两条支线固定胜负。因此 `U_signal` 之后只能画分叉，不能把“UI 已返回”写成 RenderThread 取 Buffer 前的硬边。

`syncFrameState()`的主要顺序是：

```text
接收 UI VSync
→ makeCurrent
→ unpin images
→ apply DeferredLayerUpdater
→ 设置 content draw bounds
→ CanvasContext.prepareTree
→ 汇总 lost/stopped、animation、redraw 与 dropped 结果
```

`makeCurrent()`返回后若 CanvasContext已无 Surface，则加入 `LostSurfaceRewardIfFound`；这既覆盖原本就没有 Surface，也覆盖 pipeline失败后 `setSurface(nullptr)`的情形。只有 Surface仍在而结果为 false时才记 `ContextIsStopped`，在当前实现中对应 `mStopped`短路。Java ThreadedRenderer只对 lost surface强制 relayout，对 `UIRedrawRequired`发起 invalidate。不能把所有不能画都解释为 Surface丢失。

## 7. prepareTree 才把 staging 树交给 RenderThread，并计算本帧能否绘制

`CanvasContext.prepareTree()`先导入 UI FrameInfo、记录 SyncStart，再为所有窗口级 RenderNode运行 prepare。主 target 使用 FULL 模式，其余非客户端或填充节点使用 RT_ONLY：

```cpp
for (const sp<RenderNode>& node : mRenderNodes) {
    info.mode = node.get() == target
            ? TreeInfo::MODE_FULL
            : TreeInfo::MODE_RT_ONLY;
    node->prepareTree(info);
}
```

native `RenderNode::prepareTreeImpl()`在 FULL 模式推进 staging properties 与 staging DisplayList。属性推进会比较新旧矩阵、边界、alpha、clip、layer 等状态，把变化加入 DamageAccumulator；DisplayList 推进还会更新强引用的 child RenderNodes并递归 prepare。

因此 `T_sync`同时是三件事的边界：

1. UI 本轮录制的命令已被当前渲染树接纳；
2. RenderThread 动画和 layer 更新已参与本轮 damage；
3. CanvasContext 已决定本次能否 draw，以及是否需要 UI 再重绘。

它还不是 GPU 命令发出点。`prepareTree()`处理的是渲染树和资源准备，不会因为名字里有 tree 就自动取得或提交窗口 Buffer。

本次不能 draw 的门包括：

- 当前没有 Surface；
- 同一个 vsync 已经 swap，UI 请求赶不上 RT animation 的本次节拍；
- 某个必需 backdrop node 尚不可渲染；
- 预取下一 Buffer 失败。

最后一项需要按 r48 的实际编译常量解释。源码会调用：

```cpp
int err = mNativeSurface->reserveNext();
```

但 `ReliableSurface.cpp`固定 `DISABLE_BUFFER_PREFETCH=true`，`reserveNext()`会直接返回成功；其中真正调用 `ANativeWindow_dequeueBuffer` 的预取分支在此快照不会运行。不能仅凭调用点就声称 `prepareTree()`已经 dequeue。

`prepareTree()`也会根据 `hasAnimations`、`requiresUiRedraw`和 animated image delay排 RenderThread 后续 callback。已经进入 current RenderNode 的动画可以不经过 Java `onDraw()`继续产帧；若变化需要 UI 内容重录，sync result才要求 ViewRoot invalidate。

## 8. CanvasContext.draw 的空帧、新 Surface 与 ReliableSurface 边界

真正输出从 `CanvasContext.draw()`开始。它先结束 damage 累积：

```cpp
SkRect dirty;
mDamageAccumulator.finish(&dirty);
```

此时 dirty 已综合 View invalidation、RenderNode 属性、动画、layer 与窗口状态，不再等同第 2 节清掉的 Java `mDirty`。

第一道特殊分支是空帧：

```cpp
if (dirty.isEmpty()
        && Properties::skipEmptyFrames
        && !surfaceRequiresRedraw()) {
    // 标记 skipped，并直接调用 frame-complete listeners
    return;
}
```

它没有 `getFrame()`、draw 或 queue，却仍执行 listeners，以免调用者永久等待。这是“frame-complete 不必对应新 Buffer”的第一个源码反例。

固定首 Surface 不会落入这里。`setSurface()`成功后设置 `mHaveNewSurface=true`；`surfaceRequiresRedraw()`还会检查窗口尺寸变化。后续 `computeDirtyRect()`遇到新 Surface、尺寸变化或 `bufferAge==0`时强制全窗口重绘。首个 Buffer不能依赖不存在的历史内容。

CanvasContext并不直接持有原始 ANativeWindow，而是用 `ReliableSurface`包装它、安装 dequeue/cancel/queue 拦截器，并可设置 4 秒 dequeue timeout。这层还有一个非常重要的容错语义：

```text
底层 dequeue 失败
→ ReliableSurface 记录错误
→ 返回一块 1×1 scratch Buffer 给上游继续画
→ 对这块 fallback 的 queue 直接返回成功，不下穿真实 ANativeWindow
```

于是上层 pipeline 可能得到 `didSwap=true`并触发 frame-complete，但真实 `IGraphicBufferProducer::queueBuffer()`根本没有发生。固定主线排除了该分支；诊断现场则必须读取 `ReliableSurface::getAndClearError()`与错误日志，不能把 callback 当成真实 queue 的证明。

正常 `draw()`的骨架是：

```text
finish damage
→ pipeline.getFrame
→ setPresentTime
→ computeDirtyRect
→ pipeline.draw
→ 读取 frame number
→ waitOnFences
→ pipeline.swapBuffers
→ 处理 ReliableSurface error / swap history
→ didSwap 时执行 frame-complete listeners
```

`setPresentTime()`写的是期望调度时间；render-ahead 时甚至可以是未来时间。它不是实际 present timestamp。

## 9. SkiaGL 与 SkiaVulkan 的取 Buffer、提交和错误传播并不对称

r48 不应写死所有设备都使用某一个后端。`Properties::peekRenderPipelineType()`先读设备 `use_vulkan()`结果，再用 `debug.hwui.renderer`对应属性选择 `skiagl`或 `skiavk`；一旦 pipeline 锁定，正常运行中不能随帧切换。

两条路径共享 RenderNode/Skia 上游，窗口接线却不同：

| 阶段 | SkiaGL | SkiaVulkan |
|---|---|---|
| `makeCurrent()` | 绑定 EGLContext/EGLSurface | 返回 AlreadyCurrent |
| `getFrame()` | `EglManager.beginFrame()`，查询尺寸与 buffer age | `VulkanManager.dequeueNextBuffer()` |
| 明确 dequeue 行 | AOSP HWUI 上层没有统一暴露；位置由 EGL/驱动实现决定 | `VulkanSurface::dequeueNativeBuffer()`直接调用 ANativeWindow |
| 绘制目标 | 为默认 framebuffer 建 SkSurface | 为当前 AHardwareBuffer 对应的 SkSurface 绘制 |
| 提交 | `eglSwapBuffersWithDamageKHR()` | 导出 semaphore fence，再 `queueBuffer()` |
| 错误回传 | BAD_SURFACE/BAD_NATIVE_WINDOW令 swap 返回 false | `presentCurrentBuffer()`返回 bool，但上层调用链未使用它 |

OpenGL 的 `beginFrame()`会再次 `makeCurrent`、查询 EGL surface 宽高、buffer age并调用 `eglBeginFrame`。实际 ANativeWindow dequeue可能发生在 sync 阶段的 make-current、begin-frame、第一次渲染或 swap附近；没有驱动证据时只能承认这个区间，不能伪造一个统一 `B_lease`源码行。

Vulkan 路径则很直白：

```cpp
ANativeWindowBuffer* buffer;
int fenceFd;
mNativeWindow->dequeueBuffer(
        mNativeWindow.get(), &buffer, &fenceFd);
```

这里返回的 fence 表示该 Buffer 上一次被 consumer 使用后何时可由 producer 重写，语义上是 producer 等待的 release fence；不能把它叫成本次提交给 consumer 的 acquire fence。VulkanManager会等待或导入该 fence，然后才能让 GPU 安全覆盖 Buffer。

正常 semaphore提交与 FD导出都成功时，Vulkan把 GPU本次写入完成的 semaphore导出为 fence FD，再随 `queueBuffer()`交给 consumer；对 consumer而言，这才是读取新内容前要等待的 acquire fence。

一般错误路径更窄：若 `semaphoreFd==-1`，`presentCurrentBuffer()`会改用当前 Buffer保存的旧 dequeue fence。semaphore submission失败分支会先等待 graphics queue idle；但 submission成功而 FD导出失败时只记录错误。因而这条 fallback FD不能无条件改名为“本轮 GPU完成 fence”，固定主线才明确排除 submission/export失败。

Vulkan还有一个会直接影响 callback解释的 r48 边界：

```text
SkiaVulkanPipeline.swapBuffers()
→ VulkanManager.swapBuffers()                 # 返回 void
→ VulkanSurface.presentCurrentBuffer()         # 返回 queue 是否成功
```

`VulkanManager`没有使用最后那个 bool，而 pipeline只按 `drew`返回 `requireSwap/didSwap`。所以真实 native `queueBuffer()`失败后，CanvasContext仍可能看到 `didSwap=true`并调用 frame-complete。它是 ReliableSurface fallback 之外的第二个反例。

## 10. Skia 回放、flush 与三类 fence：CPU 返回从不自动等于显示完成

`SkiaPipeline::renderFrame()`先处理硬件 layer，再回放窗口 RenderNode：

```cpp
RenderNodeDrawable root(nodes[0].get(), canvas);
root.draw(canvas);
surface->getCanvas()->flush();
```

RenderThread操作的是 native RenderNode、DisplayList和资源，不会重新进入普通业务 View 的 Java `onDraw()`。矩阵、alpha、clip、elevation、阴影以及 child节点关系在这里共同决定实际 Skia 工作。

`flush()`只保证命令被推向图形后端；GPU 通常仍异步执行。除非启用专门的等待属性或显式等待同步对象，CPU从该函数返回不能推出目标像素已计算完。

源码里容易混淆的等待至少有三类：

| 同步对象 | 谁等待什么 | 与 present 的关系 |
|---|---|---|
| dequeue 返回的 release fence | producer 等上一轮 consumer 释放可复用 Buffer | 只保护重写所有权 |
| queue 携带的 acquire fence | consumer 等本轮 producer/GPU 写完目标 Buffer | 只保护读取新内容 |
| `CanvasContext.waitOnFences()` | 等 `enqueueFrameWork()`登记到 CommonPool 的本帧 futures | 不是统一 GPU finish，更不是显示扫描完成 |

CanvasContext在 swap 前调用的 `waitOnFences()`名字很宽，实际只遍历 `mFrameFences`中的异步 frame work；把它解释成“等待所有 GPU 或 SF 工作”会越过源码。

在固定正常路径中，producer完全可以先完成 `Q_accept`，让 Buffer携带尚未 signal的 acquire fence进入队列。非 droppable的传统队头会在 latch门等待；droppable队头可先 latch但仍携带 fence，后续真正读取/显示不能无视它。这样 CPU、GPU与 SF才能流水并行。

## 11. Surface::queueBuffer 与 BufferQueueProducer：入队点早于函数返回

Java `Surface`包装的 native `android::Surface`实现 ANativeWindow。正常 GL/Vulkan提交最终都要经过它，但传递的不只是一块像素内存。

`Surface::queueBuffer()`先由 buffer handle找 slot，再组装 `QueueBufferInput`：

| 元数据 | 作用 |
|---|---|
| timestamp / auto timestamp | 期望显示调度时间 |
| dataspace / HDR metadata | 颜色解释 |
| crop / scaling / transform | consumer怎样映射内容 |
| surface damage | 哪些区域相对历史内容发生变化 |
| fence | consumer何时可安全读本轮写入 |
| frame timestamp request | 是否回传更完整帧事件 |

surface damage还要从 OpenGL左下原点翻到系统常用的左上原点，并结合 transform做互补旋转。随后才调用：

```cpp
mGraphicBufferProducer->queueBuffer(slot, input, &output);
```

`IGraphicBufferProducer`是接口边界，具体对象可在本地，也可经过 Binder。判断跨进程不能只看 C++ 方法长相；固定非 BLAST窗口的 producer调用进入 SF 进程内的 BufferQueueProducer。

`BufferQueueProducer::queueBuffer()`先在主锁外拒绝 null fence和非法 scaling mode；随后进入 core主锁验证：

- queue 未 abandoned且 producer仍连接；
- slot编号有效，状态确为 DEQUEUED；
- producer已经为该 slot调用 requestBuffer；
- crop位于 Buffer边界内。

验证成功后，关键状态改变是：

```cpp
mSlots[slot].mFence = acquireFence;
mSlots[slot].mBufferState.queue();
++mCore->mFrameCounter;
// 构造 BufferItem
mCore->mQueue.push_back(item); // 或替换一个可丢帧队尾项
```

这就是 `Q_accept`：producer对本轮写所有权的交接已经被队列接受。若替换可丢 Buffer，旧项的 surface damage还会合并进新项，避免局部更新遗漏。

锁释放后，BufferQueueProducer才按 callback ticket保证顺序，调用 `onFrameAvailable()`或 `onFrameReplaced()`；若连接 API 是 EGL，随后还会等待上一笔 queued fence来限制队列过深，最后函数才返回。

所以正常顺序是：

```text
锁内 DEQUEUED → QUEUED / 入队
→ 锁外 consumer callback
→ 可选 EGL producer 节流
→ queueBuffer 返回
```

这带来两个反直觉结论：

1. consumer收到 frame-available 可以早于 producer侧 `queueBuffer()`返回；
2. trace里的 QueueBufferDuration可能包含同步 consumer callback和 EGL节流，不只是锁内改 slot的时间。

成功入队仍只说明“Buffer及其元数据和 fence已经可供 consumer处理”。它不说明 fence已 signal、consumer已经 acquire、图层可见、已经合成或已经 present。

## 12. r48 默认非 BLAST 主线：App producer 入队，SF consumer择机 latch

ViewRoot会在窗口 private flags里请求 BLAST，但 WMS只有在自己的 `mUseBLAST`开关为真时才在 add结果中返回 `ADD_FLAG_USE_BLAST`。r48构造 WMS时读取 DeviceConfig，其缺省值明确为 false。因此本章固定路径继续沿用第 213 章的非 BLAST分支，而不是把“请求”写成“已启用”。

非 BLAST窗口的 SurfaceControl创建 BufferQueueLayer。概念上的两端是：

```text
App / RenderThread
  ANativeWindow(android::Surface)
  → IGraphicBufferProducer proxy
  → 同步 Binder

SurfaceFlinger
  BufferQueueProducer
  → BufferQueue core
  → consumer listener / BufferQueueLayer
  → composition loop
```

App在 `Surface::queueBuffer()`里等待同步 Binder返回；SF进程内的 BufferQueueProducer却会在返回前调用 consumer listener。listener把新 Buffer纳入 SF待处理状态并请求后续合成工作，但它本身不是 latch或present完成点。

一个 slot的典型所有权循环是：

```text
FREE
→ DEQUEUED       producer可写，先满足上一轮 release fence
→ QUEUED         producer已交队列，附本轮 acquire fence
→ ACQUIRED       consumer选中并读取
→ FREE           consumer释放，新的 release fence回到producer侧
```

SF何时从 QUEUED走到 ACQUIRED/LATCHED还受期望时间、frame number、队列策略和 acquire fence约束。`debug.sf.latch_unsignaled`可绕过 fence门；即使该开关为 0，传统 BufferQueueLayer遇到 `mIsDroppable`队头也会先 latch，避免它不断被后帧替换而永远无法选中。后续纹理/合成仍携带 acquire fence，并没有把未完成像素当成安全可读内容。

`Q_accept < L_latch`成立，但 `Q_return < L_latch`不必成立：callback在 queue返回前发生，另一线程可以继续合成调度。`G_ready < L_latch`也只在非 droppable且未启用 latch-unsignaled时成立；通用硬边应放到安全读取与最终 present，而不是 latch动作本身。

图层即使当前隐藏，也可以先得到内容。WMS稍后 `finishDrawing()`推进窗口显示状态并提交 show；Buffer路径与控制事务路径在 SF汇合。这正是第 1 节没有给 `W_show`和 `L_latch`排序的原因。

## 13. BLAST 对照：本地消费 Buffer，再把内容绑进 SurfaceControl Transaction

启用 BLAST时，上游 View、RenderNode、RenderThread与 Skia流程不变；改变的是 ANativeWindow背后的 consumer位置及 Buffer交给 SF的方式。

ViewRoot收到服务端 BLAST标志后，创建 `BLASTBufferQueue`并让 renderer使用它的 producer Surface。BLAST构造函数在 App进程内创建一对 BufferQueue producer/consumer，设置无限 dequeue timeout，可选增加可同时 dequeue数量，然后让 `BLASTBufferItemConsumer`监听 frame available。

于是链路变成：

```text
RenderThread queue Buffer
→ App内 BufferQueueProducer 接纳
→ 同步 onFrameAvailable
→ BLAST consumer acquire BufferItem
→ SurfaceControl.Transaction.setBuffer(target, buffer)
→ 同一事务设置 acquire fence、frame、crop、transform、期望时间
→ Transaction送往 SurfaceFlinger
```

`processNextBufferLocked()`还注册 transaction-completed callback。回调更新 frame timestamps和 release fence，并在保持最多一笔 pending release的策略下释放旧 Buffer。它与 HWUI frame-complete不是同一个 callback；局部 transaction可能在同步 `onFrameAvailable()`内很早 apply，所以两个 callback也没有可跨线程强排的固定先后。

BLAST有两种 transaction出口：

```cpp
if (mNextTransaction != nullptr && useNextTransaction) {
    t = mNextTransaction;
    mNextTransaction = nullptr;
    applyTransaction = false;
}

t->setBuffer(...);
t->setAcquireFence(...);

if (applyTransaction) {
    t->apply();
}
```

普通 BLAST帧使用局部 transaction并立即 apply。若 ViewRoot为同步绘制预先调用 `setNextTransaction(mRtBLASTSyncTransaction)`，BLAST只把 Buffer及元数据写入外部 transaction，不在此处 apply。

HWUI原始 frame-complete callback随后先执行 `finishBLASTSync()`：

- `apply=true`时直接提交这笔 transaction；
- 需要把下一帧交给 WMS时，将它 merge进 `mSurfaceChangedTransaction`，再由 `finishDrawing()`携带过去。

因此 BLAST本地 `Q_accept`也不能直接推出 transaction已经到 SF；同步分支故意在两者之间插入了持有期。另一方面，非同步局部 transaction的 acquire和 apply又可能发生在 RenderThread底层 queue调用返回前，因为 consumer callback本来就在 BufferQueueProducer返回前同步调用。

两条路径可这样对照：

| 维度 | 非 BLAST | BLAST |
|---|---|---|
| HWUI上游 | ANativeWindow / SkiaGL或SkiaVulkan | 相同 |
| 直接 consumer | SF进程内 BufferQueueLayer | App进程内 BLASTBufferItemConsumer |
| 交给 SF | BufferQueue consumer路径 | `Transaction.setBuffer()`对应 BufferStateLayer |
| Buffer与窗口几何同步 | 分立路径在 SF汇合 | 可装入同一 Transaction原子提交 |
| 本地 queue成功 | 已进入目标 SF BufferQueue | 只进入 App内队列，仍需 BLAST transaction出口 |

BLAST是 Buffer与图层事务适配器，不是新的绘制引擎，也不替代 Skia、RenderThread或 GPU。

## 14. frame-complete 到 WMS show：callback有契约意图，也有源码反例

正常非空、正常提交的固定路径中，CanvasContext在 pipeline swap返回后读取 ReliableSurface error，更新 swap history和帧统计，然后：

```cpp
if (didSwap) {
    for (auto& func : mFrameCompleteCallbacks) {
        std::invoke(func, frameCompleteNr);
    }
    mFrameCompleteCallbacks.clear();
}
```

因此固定无错误路径可写 `Q_return < C_raw`。但一般情形不能反推 `C_raw ⇒ Q_accept`，因为至少有三类反例：

| 分支 | 真实 producer queue | HWUI callback |
|---|---|---|
| 空 damage且允许跳帧 | 没有 | 直接调用 |
| ReliableSurface提供 1×1 fallback | 没有下穿真实队列 | 上层仍可能调用 |
| Vulkan真实 queue失败 | 尝试但失败 | ignored bool可让 `didSwap=true` |

OpenGL遇到 BAD_SURFACE或 BAD_NATIVE_WINDOW会令 `didSwap=false`，CanvasContext清掉 Surface；本轮 listeners不会在该分支被清空，因而报告可能等到后续恢复帧，而不是在错误点伪装成功。

CanvasContext还在 callback前调用 `markFrameCompleted()`，其邻近源码注释明确质疑应否改用 fence取得真实完成。这已经说明此处的“completed”是 HWUI CPU侧记账边界，不是 GPU、SF或显示硬件完成。

JNI保存 Java callback引用，CanvasContext触发后由当前 RenderThread调用 `onFrameComplete(frameNr)`。ViewRoot callback先处理 BLAST同步，再 `postAtFrontOfQueue()`：

```text
RenderThread: C_raw
  → 可选 finishBLASTSync
  → 向主 Handler 队首 post Runnable

Main thread: U_resume 后继续结束 performDraw / traversal
  → D_return
  → M_run
  → pendingDrawFinished
  → 同步 Binder finishDrawing
  → F_commit
```

`postAtFrontOfQueue()`不能抢占正在执行的 traversal；所以 `M_run`同时晚于 `C_raw`与 `D_return`。原始 callback则可以早于或晚于 `U_resume`，二者无固定先后。

`ViewTreeObserver.registerFrameCommitCallback()`的 API意图是通知内容已渲染并交给 swap chain，且文档明确承认此时未必可见。实现与首帧 WMS报告共用上述 Handler Runnable；它绝不是 present callback，异常容错分支也要求结合错误状态解释。

固定前提没有 SurfaceView或其他额外 draw debt，所以本次 `pendingDrawFinished()`令计数归零并调用 WMS `finishDrawing()`。一般 View树若有参与者先调用 `drawPending()`，则必须等所有债共同归零；一次 HWUI Runnable本身不能直接推出 Binder已经发生。服务端收到回报后先进入 `COMMIT_DRAW_PENDING`，再由后续 surface placement推进 ready/show状态。与此同时，目标 Buffer走独立的 SF消费支线：

```text
C_raw < M_run < F_commit < W_show

Q_accept ──────────> L_latch

W_show  ────────┐
L_latch ────────┴─> C_submit < P_present
G_ready  ──────────────────────> P_present
```

acquire fence可以先随 Buffer或 BLAST transaction到 SF、以后再 signal；传统 droppable队头甚至可先 latch。show事务与 latch也可任意先后。目标内容与可见控制状态先汇入合成提交，物理 present还必须等待安全读取依赖，并需要更后的运行时证据。

## 15. 失败矩阵、trace读法与首帧对象账本

先用失败矩阵给所有“没看到首帧”的结论设上限：

| 观察点 | 可能分支 | 最多能推出 |
|---|---|---|
| Java `onDraw()`已返回 | 当前 View内容命令已记录一部分；其余 draw流程和 `endRecording()`仍可能在后 | 不能推出该节点 DisplayList已封口，更不能推出 RenderThread已接纳 |
| `syncAndDrawFrame()`很快返回 | `prepareTextures=true`提前 signal | RT可能仍在 getFrame、draw或 queue |
| `ContextIsStopped`加 `FrameDropped` | Surface仍在但 native context stopped | 不应自动强制 relayout |
| `LostSurfaceRewardIfFound`加 `FrameDropped` | CanvasContext无 Surface | Java会请求 relayout，当前帧未画 |
| `canDrawThisFrame=false` | 同 vsync已画、backdrop不可用等 | RT走 `waitOnFences()`，可能另排一帧 |
| frame-complete到达 | 正常 swap，也可能空帧/fallback/Vulkan queue失败 | 不能单独证明真实 producer入队 |
| 普通非 shared `queueBuffer()`成功返回 | 队列已接纳，listener也已被调用 | fence、latch、show、present仍未结账 |
| WMS `finishDrawing()`到达 | App绘制债已回报 | 图层控制状态尚待后续 placement |
| 图层已 show | 可见控制事务已提交 | 目标 Buffer未必 latch |
| SF已 latch | consumer选中目标内容 | 未必已参与可见合成或 present |

trace名称应映射到具体边界，而不是一看到长条就猜根因：

| 现象或指标 | 首查位置 | 额外边界 |
|---|---|---|
| `Record View#draw()`长 | 自定义 `onDraw`、View树重录、overlay | 不含 RenderThread栅格化 |
| `syncAndDrawFrame`等待长 | RT队列积压、prepareTree、纹理空间、不能提前 signal | 不自动等于 GPU慢 |
| `DequeueBufferDuration`长 | 可用 slot、consumer释放、队列背压 | 若 dequeue开始早于 SyncStart，CanvasContext会把本帧该值记为 0 |
| `Issue Draw Commands`长 | RenderNode回放、layer、Skia/驱动提交 | CPU区间返回不等于 GPU完成 |
| `QueueBufferDuration`长 | Binder、consumer callback、EGL producer节流 | 不只是锁内 queue状态更新 |
| queue后到可见仍长 | fence、SF选帧、WMS show、合成/present | r48需查 Surface frame timestamps、SF TimeStats或 present fence |

最后把最常混用的对象放回各自职责：

| 对象 | 保存什么 | 所有权/所在侧 | 不是 |
|---|---|---|---|
| `RecordingCanvas` | 一次 DisplayList录制中的绘制命令 | UI线程临时使用 | 窗口像素 Buffer |
| Java/native `RenderNode` | DisplayList、属性、子节点与 staging/current状态 | View/HWUI | Java View本身 |
| `GraphicBuffer` | 可由 GPU/HWC共享的像素存储 | BufferQueue slot轮转 | DisplayList |
| `android::Surface` | ANativeWindow producer接口与本地元数据 | App渲染侧 | 图层控制句柄 |
| `ReliableSurface` | ANativeWindow拦截、错误与 fallback状态 | CanvasContext包装层 | 一个新 BufferQueue |
| `BufferQueueProducer` | slot校验、queue状态与 producer协议 | 普通路径在 SF；BLAST路径在 App | consumer或合成器 |
| `BLASTBufferQueue` | 本地消费并构造 Buffer transaction | App进程可选适配层 | 渲染后端 |
| `SurfaceControl` | 图层层级、几何、可见性与 Buffer事务目标 | App/WMS/SF控制面 | 可直接调用 View.draw的 Canvas |
| acquire/release fence | 跨所有权边界保护读写时序 | 随 dequeue/queue/transaction传递 | present完成证明 |

读到“首帧完成”时，至少要反问：完成的是 `R_view`、`T_sync`、`Q_accept`、`C_raw`、`F_commit`、`W_show`、`L_latch`，还是 `P_present`？只有答案落到一个可观测完成点，日志和源码才真正对得上。

## 16. 九组只读练习：从 UI 录制追到提交边界，在 present 前停笔

以下命令只搜索 Android 11源码，不启动 emulator、不创建图形上下文，也不改工作树。默认在源码根目录运行；若不在根目录，可先设置 `ANDROID_BUILD_TOP`。

### 练习 1：证明 ViewRoot gate、业务树录制与 renderer root 的顺序

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
T="$SRC/frameworks/base/core/java/android/view/ThreadedRenderer.java"
W="$SRC/frameworks/base/core/java/android/view/View.java"
G="$SRC/frameworks/base/core/java/android/view/ViewGroup.java"
test -f "$V" && test -f "$T" && test -f "$W" && test -f "$G"
grep -nF 'if (!dirty.isEmpty() || mIsAnimating || accessibilityFocusDirty)' "$V"
grep -nE 'performDraw\(\)|setFrameCompleteCallback|dispatchOnDraw|dirty.setEmpty|mThreadedRenderer.draw|updateViewTreeDisplayList|updateRootDisplayList|updateDisplayListIfDirty|beginRecording|endRecording|dispatchGetDisplayList|recreateChildDisplayList' "$V" "$T" "$W" "$G"
```

按源码标出 `D_gate → R_view → R_root`，再圈出 `dispatchOnDraw()`相对 dirty gate的位置。

完成标准：能解释 OnDrawListener为何不是 Buffer提交通知；父 RenderNode复用时为何仍可能递归更新 child。

### 练习 2：把 RecordingCanvas、DisplayList 与 staging properties 分账

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
J="$SRC/frameworks/base/graphics/java/android/graphics/RenderNode.java"
N="$SRC/frameworks/base/libs/hwui/jni/android_graphics_RenderNode.cpp"
R="$SRC/frameworks/base/libs/hwui/RenderNode.cpp"
H="$SRC/frameworks/base/libs/hwui/RenderNode.h"
test -f "$J" && test -f "$N" && test -f "$R" && test -f "$H"
grep -nE 'finishRecording|nSetDisplayList|setStagingDisplayList|mStagingDisplayList|mDisplayList|pushStagingPropertiesChanges|pushStagingDisplayListChanges|syncDisplayList|prepareTreeImpl' "$J" "$N" "$R" "$H"
```

画出 Java RenderNode、native staging DisplayList和 current DisplayList的交接；另找一个 property setter，确认属性不是 RecordingCanvas顺带写入的。

完成标准：`endRecording()`只把新命令送到 staging；FULL prepare才让 RenderThread当前树使用它。

### 练习 3：还原 Java、JNI、RenderProxy、RenderThread 与 CanvasContext

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
H="$SRC/frameworks/base/graphics/java/android/graphics/HardwareRenderer.java"
J="$SRC/frameworks/base/libs/hwui/jni/android_graphics_HardwareRenderer.cpp"
P="$SRC/frameworks/base/libs/hwui/renderthread/RenderProxy.cpp"
R="$SRC/frameworks/base/libs/hwui/renderthread/RenderThread.cpp"
C="$SRC/frameworks/base/libs/hwui/renderthread/CanvasContext.cpp"
test -f "$H" && test -f "$J" && test -f "$P" && test -f "$R" && test -f "$C"
grep -nE 'syncAndDrawFrame|nSyncAndDrawFrame|GetLongArrayRegion|proxy->syncAndDrawFrame|RenderProxy::syncAndDrawFrame|RenderThread::getInstance|start\("RenderThread"\)|runSync|queue\(\).post|CanvasContext::create|setSurface' "$H" "$J" "$P" "$R" "$C"
```

给每一行标线程与进程，并解释 `setSurface()`的 post为何能被后投递的 draw task观察到。

完成标准：JNI/RenderProxy不是 Binder；RenderThread通常是进程 singleton，CanvasContext则按 renderer持有。

### 练习 4：构造 `prepareTextures × canDrawThisFrame` 四象限

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
D="$SRC/frameworks/base/libs/hwui/renderthread/DrawFrameTask.cpp"
test -f "$D"
grep -nE 'DrawFrameTask::drawFrame|postAndWait|syncFrameState|prepareTextures|canUnblockUiThread|canDrawThisFrame|unblockUiThread|context->draw|context->waitOnFences|mSignal.signal|LostSurfaceRewardIfFound|ContextIsStopped|FrameDropped' "$D"
```

按第 6 节四种组合分别画 signal、draw/wait与 UI返回，不要把 signal画成 UI已经运行。

完成标准：能分别解释 texture空间不足、FrameDropped、LostSurface与 ContextIsStopped；两类布尔结果互不推出。

### 练习 5：区分 tree damage、buffer-age damage 与 ReliableSurface fallback

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
C="$SRC/frameworks/base/libs/hwui/renderthread/CanvasContext.cpp"
R="$SRC/frameworks/base/libs/hwui/renderthread/ReliableSurface.cpp"
test -f "$C" && test -f "$R"
grep -nE 'mDamageAccumulator.finish|computeDirtyRect|surfaceRequiresRedraw|mHaveNewSurface|bufferAge|reserveNext|DISABLE_BUFFER_PREFETCH|if constexpr|hook_dequeueBuffer|acquireFallbackBuffer|isFallbackBuffer|getAndClearError|hook_queueBuffer' "$C" "$R"
```

先排出 `finish(dirty) → getFrame → computeDirtyRect`，再沿 `reserveNext()`读到 r48编译常量，最后追一次真实 dequeue失败。

完成标准：不能声称 r48的 prepareTree实际预取了 Buffer；能说明 1×1 fallback为何可能产生 callback却没有真实 producer queue。

### 练习 6：对比 SkiaGL 与 SkiaVulkan 的取帧和提交

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/libs/hwui/Properties.cpp"
S="$SRC/frameworks/base/libs/hwui/pipeline/skia/SkiaPipeline.cpp"
G="$SRC/frameworks/base/libs/hwui/pipeline/skia/SkiaOpenGLPipeline.cpp"
V="$SRC/frameworks/base/libs/hwui/pipeline/skia/SkiaVulkanPipeline.cpp"
E="$SRC/frameworks/base/libs/hwui/renderthread/EglManager.cpp"
M="$SRC/frameworks/base/libs/hwui/renderthread/VulkanManager.cpp"
N="$SRC/frameworks/base/libs/hwui/renderthread/VulkanSurface.cpp"
test -f "$P" && test -f "$S" && test -f "$G" && test -f "$V"
test -f "$E" && test -f "$M" && test -f "$N"
grep -nE 'peekRenderPipelineType|PROPERTY_RENDERER|flush commands|SkiaOpenGLPipeline::getFrame|beginFrame|eglSwapBuffersWithDamageKHR|SkiaVulkanPipeline::getFrame|dequeueNextBuffer|dequeueNativeBuffer|presentCurrentBuffer|queueBuffer' "$P" "$S" "$G" "$V" "$E" "$M" "$N"
```

分别标出两条路径的 `B_lease`、`G_issue`与 queue attempt，并检查 Vulkan中 `presentCurrentBuffer()`返回值的调用去向。

完成标准：不为 GL编造固定 dequeue位置；dequeue fence是 producer侧 release语义，queue fence才是 consumer侧 acquire语义。

### 练习 7：把 BufferQueue 的“锁内接纳、回调、返回”拆成三点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
S="$SRC/frameworks/native/libs/gui/Surface.cpp"
B="$SRC/frameworks/native/libs/gui/BufferQueueProducer.cpp"
test -f "$S" && test -f "$B"
grep -nE 'Surface::queueBuffer|QueueBufferInput input|setSurfaceDamage|mGraphicBufferProducer->queueBuffer|BufferQueueProducer::queueBuffer|isDequeued|mBufferState.queue|mCore->mQueue.push_back|mCallbackMutex|callbackTicket|onFrameAvailable|Throttling EGL Production|return NO_ERROR' "$S" "$B"
```

写出 slot从 DEQUEUED到 QUEUED的精确位置，再找 consumer callback锁和 EGL producer节流。

完成标准：`Q_accept`早于 callback和 `Q_return`；QueueBufferDuration可以覆盖锁内状态改变之外的等待。

### 练习 8：比较非 BLAST BufferQueueLayer 与 BLAST BufferStateLayer

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
W="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
B="$SRC/frameworks/native/libs/gui/BLASTBufferQueue.cpp"
F="$SRC/frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp"
C="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowSurfaceController.java"
Q="$SRC/frameworks/native/services/surfaceflinger/BufferQueueLayer.cpp"
S="$SRC/frameworks/native/services/surfaceflinger/BufferStateLayer.cpp"
test -f "$W" && test -f "$V" && test -f "$B" && test -f "$F"
test -f "$C" && test -f "$Q" && test -f "$S"
grep -nE 'WM_USE_BLAST_ADAPTER_FLAG|ADD_FLAG_USE_BLAST|setBLASTLayer|createBufferQueueLayer|createBufferStateLayer|createBufferQueue|onFrameAvailable|processNextBufferLocked|mNextTransaction|applyTransaction|setAcquireFence|setDesiredPresentTime|fenceHasSignaled|mIsDroppable' "$W" "$V" "$B" "$F" "$C" "$Q" "$S"
```

先证明 WMS开关缺省值，再画普通路径和 BLAST路径；对后者分别画局部 transaction与 `mNextTransaction`。

完成标准：ViewRoot请求不等于服务端启用；同步 BLAST分支的 `setBuffer()`与 transaction apply之间存在显式持有期。

### 练习 9：从原始 callback追到 WMS show，并在 present前停笔

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
J="$SRC/frameworks/base/libs/hwui/jni/android_graphics_HardwareRenderer.cpp"
D="$SRC/frameworks/base/libs/hwui/renderthread/DrawFrameTask.cpp"
C="$SRC/frameworks/base/libs/hwui/renderthread/CanvasContext.cpp"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
I="$SRC/frameworks/base/core/java/android/view/IWindowSession.aidl"
N="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
O="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowSurfaceController.java"
B="$SRC/frameworks/native/libs/gui/BufferQueueProducer.cpp"
Q="$SRC/frameworks/native/services/surfaceflinger/BufferQueueLayer.cpp"
L="$SRC/frameworks/native/services/surfaceflinger/BufferLayer.cpp"
test -f "$V" && test -f "$J" && test -f "$D" && test -f "$C"
test -f "$M" && test -f "$A" && test -f "$S"
test -f "$I" && test -f "$N" && test -f "$O" && test -f "$B"
test -f "$Q" && test -f "$L"
grep -nE 'setFrameCompleteCallback|addFrameCompleteListener|mFrameCompleteCallbacks|if \(didSwap\)|postAtFrontOfQueue|pendingDrawFinished|finishDrawing|finishDrawingWindow|COMMIT_DRAW_PENDING|commitFinishDrawingLocked|READY_TO_SHOW|performShowLocked|showSurfaceRobustlyLocked|mSurfaceControl.show|mBufferState.queue|fenceHasSignaled|latchBuffer' "$V" "$J" "$D" "$C" "$M" "$A" "$S" "$I" "$N" "$O" "$B" "$Q" "$L"
```

画出 `C_raw → M_run → F_commit → W_show`，再单独画 `Q_accept → L_latch`、`{W_show, L_latch} → C_submit → P_present`和 `G_ready → P_present`；只在非 droppable且未开启 latch-unsignaled的分支补上 `G_ready → L_latch`。

完成标准：原始 callback与 UI实际返回无固定先后；Handler不能抢占当前 traversal；静态源码走到 show仍不能证明 `P_present`。

完成九组练习后，应能把“画了”“交了”“显示了”拆成可验证的问题：是谁在什么线程完成了哪一个状态转换，失败路径是否绕开了真实 Buffer提交，最后又用什么运行时证据认领 present。
