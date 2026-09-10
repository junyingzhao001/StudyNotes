# 213 Android ViewRootImpl 首次 performTraversals：测量、relayout 与 Surface 建立

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只读源码，可以证明 App 主线程、同步窗口 Binder、system_server 窗口状态机与渲染回报之间的源码顺序；不能据此测量某台设备的首帧耗时，也不能用 `performTraversals()` 返回代替 SurfaceFlinger latch 或显示硬件 present 证据。

第 212 章停在 `Activity.makeVisible()`：Decor 已经有 ViewRootImpl parent，WMS 也已经登记 WindowState，但 View 树还没有 AttachInfo，窗口还没有可绘制的 Java `Surface`。

本章只追一个问题：**普通首次可见 Activity 的第一轮 `performTraversals()` 怎样把一次 App 尺寸提议变成 WMS frame 和可写 Surface；为什么 attach、measure、relayout、layout、draw、`finishDrawing()`、show 与 present 必须分别结账？**

## 1. 固定普通首次窗口，用十八个完成点定义“首轮走完”

先固定 `B_target`，避免把不同窗口、渲染后端与可见性分支揉成一条伪全序：

| 维度 | 固定值或前提 |
|---|---|
| 上游 | 沿用第 212 章的普通 Activity 首次主窗口；`addView()` 已正常返回，随后同一主线程执行了 `makeVisible()` |
| 窗口 | `TYPE_BASE_APPLICATION`，宽高均为 `MATCH_PARENT`；不是子窗、starting window、IME、壁纸或其他系统窗 |
| ViewRoot | `mAdded=true`、`mFirst=true`、`mSurface` 尚无效；第一次 traversal 已排队，尚未 dispatch attach |
| 可见性 | Decor 为 VISIBLE，`mAppVisible=true`；服务端 `ActivityRecord.isClientVisible()` 为 true |
| 图形分支 | 普通硬件加速 Decor；DecorView 虽实现 `RootViewSurfaceTaker`，但 `willYouTakeTheSurface()` 返回 null，故 `mSurfaceHolder==null`；主线固定 WMS 的 `mUseBLAST=false`，第 9 节再比较 BLAST 分支 |
| 尺寸/焦点 | add 返回的 frame hint 与本次 relayout 返回的当前 frame 相同；无 compat scale、拖拽缩放、Window weight；add 与 relayout 的 touch mode 一致，不引起焦点变化 |
| Insets/config | 初始 Insets 稳定，首次分发后 `mApplyInsetsRequested` 保持 false；relayout 不带来新的 cutout、always-consume-bars、caption、system UI visibility 或 Configuration 变化 |
| 回调 | attach、Insets、GlobalLayout、InternalInsets、PreDraw 等可控回调正常返回，不 remove 根、不改可见性，也不制造新的有效 layout 请求；无需要等待的 `WindowCallbacks` draw latch |
| 并发 | 当前窗口不被其他线程或 system_server 事件并发 remove、replace、resize、hide；Surface 分配与 Renderer 初始化成功 |
| 显示门 | relayout 返回 `RELAYOUT_RES_FIRST_TIME`；PreDraw 全部放行，Activity 容器允许 show，目标 Buffer 最终有独立 present 证据 |

十八个完成点如下：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `T_start` | Choreographer 调用 TraversalRunnable；`doTraversal()` 撤 barrier 并进入 `performTraversals()` | View 已 attached |
| `A_tree` | `dispatchAttachedToWindow()`、window-attached 通知和首轮 `dispatchApplyInsets()` 已返回 | 根 View 已测量 |
| `M_offer` | 首次 `measureHierarchy()` 返回，根 measured size 成为 App 尺寸提议 | WMS 已接受这个尺寸 |
| `B_enter` | App 主线程进入同步 `IWindowSession.relayout()` | WMS 已完成窗口布局 |
| `W_record` | WMS 找到原 WindowState，记录请求尺寸、属性、可见性与 Insets pending 状态 | 本次 frame 已算出 |
| `W_layout` | 强制 `performSurfacePlacement(true)` 返回，服务端本轮 WindowState frame/layout 已按窗口政策结算 | 后续焦点、IME、orientation 与回复用 Insets/control 快照已全部结算 |
| `W_layer` | `createSurfaceLocked()` 成功，WindowSurfaceController 已存在且 draw state 为 `DRAW_PENDING` | 图层已经 show |
| `B_reply` | WMS 完成后续窗口状态更新，填好 frame、Insets/control、SurfaceControl 等 out 参数，并从同步 relayout Binder 返回 App | Java `Surface` 已接好 |
| `S_ready` | ViewRoot 已连接 Java `Surface`、接纳 frame/Insets，并完成新 Surface 的 Renderer 初始化 | View 已按返回 frame layout |
| `M_settle` | relayout 后的补测条件已结算；固定路径尺寸、touch mode、Insets 与 Configuration 均稳定，因此不补测 | 子 View 坐标已确定 |
| `L_tree` | `performLayout()` 返回，Decor 与子树的窗口局部坐标已确定 | 本帧一定会 draw |
| `P_allow` | GlobalLayout/InternalInsets/焦点阶段结束，`dispatchOnPreDraw()` 未取消 | RenderThread 已完成帧 |
| `D_submit` | 硬件 `performDraw()` 与本轮 traversal 返回；UI 线程已调用 Renderer 提交帧工作 | WMS 已收到完成回报 |
| `C_ready` | 主 Handler 开始执行 Renderer frame-complete callback 投来的 Runnable，并调用 `pendingDrawFinished()` 结算 | WindowState 已可显示 |
| `F_commit` | WMS 在同步 `finishDrawing()` 调用内部把 draw state 从 `DRAW_PENDING` 写成 `COMMIT_DRAW_PENDING`，并请求后续 traversal | App 已观察到 Binder 返回，或 WindowState 已是 `HAS_DRAWN` |
| `W_drawn` | 后续 surface placement 把状态推进到 `READY_TO_SHOW`，显示门通过后写成 `HAS_DRAWN` | Surface show 已提交给合成器 |
| `S_show` | 后续 animation/prepare-surfaces 阶段对窗口 Buffer 图层发出 show，并提交相应 SurfaceControl 事务 | 目标 Buffer 已 latch 或上屏 |
| `F_present` | 目标首帧以 present fence、SurfaceFlinger latency 或等价显示时间戳证据完成 present | 较早任一阶段没有耗时问题 |

在固定非 BLAST 硬件路径上，可以使用这条局部全序：

```text
T_start < A_tree < M_offer < B_enter < W_record < W_layout
        < W_layer < B_reply < S_ready < M_settle < L_tree < P_allow
        < D_submit < C_ready < F_commit < W_drawn < S_show < F_present
```

其中 `performTraversals()` 的正常返回只到 `D_submit`。原始 Renderer frame-complete callback 与该返回没有固定全序；但 callback 投给主 Handler 的 Runnable 执行、`pendingDrawFinished()`、服务端 `F_commit`、WMS 后续 traversal、SurfaceControl show 与物理 present 都在 `D_submit` 之后。

## 2. traversal callback 先撤调度债，再读取本轮可见性

第 212 章已经看到 `setView()` 先排 traversal，再同步 add 窗口：

```java
void scheduleTraversals() {
    if (!mTraversalScheduled) {
        mTraversalScheduled = true;
        mTraversalBarrier =
                mHandler.getLooper().getQueue().postSyncBarrier();
        mChoreographer.postCallback(
                Choreographer.CALLBACK_TRAVERSAL,
                mTraversalRunnable, null);
    }
}
```

`mTraversalScheduled` 把同一段等待期内的多次布局/重绘请求合成一个 callback；它不保证 callback 内只 measure 一次，也不保证一帧不会因回调再排下一轮。

callback 真正被调度时：

```java
void doTraversal() {
    if (mTraversalScheduled) {
        mTraversalScheduled = false;
        queue.removeSyncBarrier(mTraversalBarrier);
        performTraversals();
    }
}
```

所以 `T_start` 不是 `requestLayout()` 那一刻，而是主 Looper 已经执行 traversal callback 的时刻。`performTraversals()` 随即检查：

```java
final View host = mView;
if (host == null || !mAdded) {
    return;
}
mIsInTraversal = true;
mWillDrawSoon = true;
```

这两个过程位不承诺最终会 draw。Surface 资源异常、不可见、PreDraw 取消和 View 被移除都可能让本轮停在更早的位置。

本轮向 WMS 提交的可见性也不是只读 `Decor.getVisibility()`：

```java
return (mAppVisible || mForceDecorViewVisibility)
        ? mView.getVisibility() : View.GONE;
```

固定路径中，`ActivityThread` 在仍占用主线程时先执行 `makeVisible()`，随后才回到 Looper 运行已经排好的 callback，因此第一轮看到 VISIBLE。Theme 的 no-display 只把 `mVisibleFromClient` 初始为 false；若该值保持到 `handleResumeActivity()`，就会跳过 add/ViewRoot，targetSdk 大于 22 且尚未 finish 时还会在 resume 检查抛错。应用可以调用 `setVisible(true)` 覆盖初值，所以不能仅凭 theme 标签断言永不进入。延迟可见、Activity 切换或服务端可见性变化也不共享固定前提。

## 3. 首轮先分发 AttachInfo 与初始 Insets，再开始 measure

`mFirst` 构造时为 true。第一轮先选择 desired window size，随后按下面的顺序接入整棵 View 树：

```java
mAttachInfo.mWindowVisibility = viewVisibility;
host.dispatchAttachedToWindow(mAttachInfo, 0);
mAttachInfo.mTreeObserver.dispatchOnWindowAttachedChange(true);
dispatchApplyInsets(host);
```

`setView()` 末尾的 `view.assignParent(this)` 只写 ViewParent 桥；这里的 `dispatchAttachedToWindow()` 才会递归把同一个 AttachInfo 交给 Decor 和子 View，并触发 `onAttachedToWindow()`。WMS 的 WindowState 又早在 add 阶段存在，三种“接入”不能互换。

初始 Insets 来自 add 返回后交给 `InsetsController` 的状态。`dispatchApplyInsets()` 会重新计算 WindowInsets、清掉 `mApplyInsetsRequested`，再调用 `host.dispatchApplyWindowInsets()`。它是第一份可用快照，不是本轮永不变化的最终承诺；relayout 仍可能返回新的 InsetsState、cutout、controls 或配置。

这给生命周期读法加上两条边界：

- `onAttachedToWindow()` 发生在第一次根测量之前，不能假定 `getWidth()/getHeight()` 已是本轮 layout 几何。
- Insets 回调可以改 padding 或调用 `requestLayout()`；通用路径会吸收这些变化，固定路径则明确排除这类重入改写。

接下来无条件调用：

```java
getRunQueue().executeActions(mAttachInfo.mHandler);
```

名字容易误导：`HandlerActionQueue.executeActions()` 是把每个 action 用 `postDelayed()` 投给 Handler，然后清空队列；它不在这一行同步执行这些 action。因此 RunQueue action 不能插进固定路径的 `A_tree < M_offer`。

这还是 ViewRootImpl 的线程局部静态 RunQueue；每个尚未 attach 的 View 所持有的私有 `mRunQueue` 是另一套。`View.post()` 会暂存到后者，并在该 View 的 `dispatchAttachedToWindow()` 中转交 Handler。两者都使用 HandlerActionQueue，但所有权和清空点不同。

## 4. frame hint、Configuration 上限与根 MeasureSpec 是三件事

首轮 desired size 的来源先按 Window 类型与 LayoutParams 分流：

| 条件 | `desiredWindowWidth/Height` 来源 | 适用边界 |
|---|---|---|
| `shouldUseDisplaySize(lp)` | `Display.getRealSize()` | additional status bar、IME、volume overlay 三类特殊窗口 |
| width 或 height 任一为 `WRAP_CONTENT` | Configuration 的 `screenWidthDp/screenHeightDp` 换算像素 | 给内容决定型窗口一个候选上限 |
| 其余 | add 阶段写入 `mWinFrame` 的 frame hint | 普通 `MATCH_PARENT` Activity 主线 |

frame hint 的价值是提高初测命中率。源码明确希望它“多数窗口接近 relayout 结果”，却没有把它升级成 WMS 本次 relayout 将返回的当前 frame。

`measureHierarchy()` 最终为根 View 生成 MeasureSpec：

| 根维度 | mode | size |
|---|---|---|
| `MATCH_PARENT` | `EXACTLY` | 当前传入的 windowSize |
| `WRAP_CONTENT` | `AT_MOST` | 当前传入的 windowSize |
| 明确像素值 | `EXACTLY` | LayoutParams 中的明确值 |

因此 `MATCH_PARENT` 只表示填满当前窗口约束，不等于物理屏幕尺寸；多窗口、DisplayArea、兼容缩放与窗口政策都可能改变 windowSize。

大屏 Dialog 的小宽度试探还有三道门：width 为 `WRAP_CONTENT`、`config_prefDialogWidth` 能解析出非零 `baseSize`，并且 desired width 大于它。全部满足时才先测 baseSize；报告 `MEASURED_STATE_TOO_SMALL` 后尝试中间宽度，仍太小才使用完整 desired width。小档或中档足够时，`goodMeasure` 会跳过完整宽度测量以及后面的旧 `mWidth` 比较；门不满足时则直接测完整 desired width。因此一次 `measureHierarchy()` 可包含一到三次 `performMeasure()`，其返回值也不能简单等同“measured size 是否变化”。

`performMeasure()` 再调用 `mView.measure()`。`View.measure()` 还会比较旧 spec、force-layout、精确尺寸与 measure cache：某次根调用可能真正进入 `onMeasure()`，也可能恢复缓存。源码中 `performMeasure()` 的次数不能直接替换每个子 View 的 `onMeasure()` 次数。

## 5. 初测只是 App 提议；属性与 Insets 还能在 Binder 前改写它

进入初测的门是：

```java
boolean layoutRequested =
        mLayoutRequested && (!mStopped || mReportNextDraw);
```

停止窗口可以推迟普通布局；若 WMS 正等待下一次 draw 回报，`mReportNextDraw` 又允许必要工作继续。固定首次窗口未 stopped。

第一轮先用 add 返回的 touch-mode 位初始化本地焦点语义，再调用：

```java
windowSizeMayChange |= measureHierarchy(
        host, lp, res, desiredWindowWidth, desiredWindowHeight);
```

得到的 `host.getMeasuredWidth()/getMeasuredHeight()` 是 App 在当前约束下的尺寸提议，不是 WindowState frame。之后仍有三类 Binder 前修正：

1. `collectViewAttributes()` 汇总 keep-screen-on、system UI visibility 等 View 树属性，必要时令 `params=lp`。
2. `SOFT_INPUT_ADJUST_UNSPECIFIED` 会依据当前已显示的 scroll container 选择 resize 或 pan，并更新参数；这不表示 IME 已显示。
3. 若 `mApplyInsetsRequested`，再次分发 Insets；回调重新置 `mLayoutRequested` 时，会在当前 traversal 再跑一次 `measureHierarchy()`。

随后代码清掉旧的 `mLayoutRequested`，以便捕获本方法后半段新产生的布局债。是否跨 Binder 的总门是：

```text
mFirst
|| windowShouldResize
|| viewVisibilityChanged
|| cutoutChanged
|| params != null
|| mForceNextWindowRelayout
```

首次必定命中。固定路径中 `mWindowAttributesChanged` 仍来自 `setView()`，所以 attrs 不为 null；后续 relayout 则允许 attrs 为 null，只单独提交 requested size、visibility、flags 与 frame number。

`computesInternalInsets` 同时覆盖“当前有 ComputeInternalInsets listener”和“过去上报过非空值、现在可能需要清零”两种情况；全新的固定根还没有后一笔历史。首次可见且该条件成立时，relayout 会带 `RELAYOUT_INSETS_PENDING`。它让 WMS 暂不把尚未 layout 完成的 content/visible/touchable 区域当最终信息；这与向 View 分发 WindowInsets 是两套方向相反的协议。

## 6. relayout 是继 add 后的同步窗口 Binder，并携带 measured size 而非 View 树

跨进程前，已有 ThreadedRenderer 会先 `pause()`，因为 WMS 可能销毁或替换当前 Surface。随后 ViewRoot 调用：

```java
relayoutResult =
        relayoutWindow(params, viewVisibility, insetsPending);
```

包装方法计算：

```text
requestedWidth  = round(host.measuredWidth  * applicationScale)
requestedHeight = round(host.measuredHeight * applicationScale)
frameNumber     = mSurface 有效时的 nextFrameNumber，否则 -1
flags           = 是否 RELAYOUT_INSETS_PENDING
```

再同步调用 `IWindowSession.relayout()`。接口不是 oneway；App 主线程要等 system_server 填回这些 out 参数：

- frame、content/visible/stable Insets 与 backdrop frame；
- DisplayCutout、MergedConfiguration；
- 主 SurfaceControl、InsetsState、InsetsSourceControl；
- SurfaceControl 尺寸与可选 BLAST SurfaceControl；
- 一组 `RELAYOUT_RES_*` 结果位。

`Session.relayout()` 本身只包围 Trace，并把参数转给 `WindowManagerService.relayoutWindow()`；但每进程 Session 对象还持有 callback、服务端 `SurfaceSession`、窗口计数、overlay 集合、权限能力、scale 与 package 等会话状态，不能概括成“只保存调用身份”。WMS 从来不读取 App 的 Decor 或子 View；它只看到 IWindow、LayoutParams 副本、根 measured size、可见性与协议状态。

这也是尺寸协商的最短表达：

```text
add frame hint
→ App 根测量提议
→ relayout(requestedWidth, requestedHeight)
→ WMS 窗口政策 frame
→ App 按返回 frame 决定是否补测
```

## 7. WMS 先更新原 WindowState、强制布局，再决定是否给 Surface

`relayoutWindow()` 清调用身份、取得全局锁，并用 Session 与 IWindow 找到第 212 章已经登记的 WindowState。它不会为这次 relayout 新建第二个 WindowState。

固定路径的关键顺序是：

```text
非 GONE：setRequestedSize
→ 如有 attrs：Policy 调整、type/providesInsetsTypes 不变校验、copyFrom
→ mRelayoutCalled=true、mInRelayout=true
→ setViewVisibility(VISIBLE)
→ setDisplayLayoutNeeded
→ 写 mGivenInsetsPending
→ 计算 shouldRelayout
→ performSurfacePlacement(true)
→ relayoutVisibleWindow
→ createSurfaceControl
```

`shouldRelayout` 不是“Decor 是 VISIBLE”一个条件：

```java
viewVisibility == View.VISIBLE
        && (win.mActivityRecord == null
        || win.mAttrs.type == TYPE_APPLICATION_STARTING
        || win.mActivityRecord.isClientVisible())
```

普通真实 Activity 主窗口还要通过服务端 `isClientVisible()`。固定路径通过；若失败且旧 Surface 不存在，WMS 不会在本次调用创建新 Surface。

`performSurfacePlacement(true)` 位于 `createSurfaceControl()` 之前。它强制消化当前窗口布局请求，使 DisplayPolicy、Activity/Task 边界、系统栏、IME、cutout、父窗口与 Display 布局共同决定 WindowState frame。App measured size 是输入，WMS frame 是系统侧决议，二者都不能单独解释最终窗口尺寸。

`relayoutVisibleWindow()` 再设置结果位：

```java
result |= (!wasVisible || !isDrawnLw())
        ? RELAYOUT_RES_FIRST_TIME : 0;
```

所以 `FIRST_TIME` 不是 Java 方法“第几次调用”的计数器。窗口从不可见回来、尚未 drawn、格式重建或某些 resize 也可能再次要求下一次 draw 回报。

## 8. 新 WindowSurfaceController 从 DRAW_PENDING 和隐藏状态开始

`createSurfaceControl()` 最终进入 `WindowStateAnimator.createSurfaceLocked()`。如果已有 controller，它直接复用；真正首次创建才执行：

```java
w.setHasSurface(false);
resetDrawState();              // DRAW_PENDING
int flags = SurfaceControl.HIDDEN;
calculateSurfaceBounds(...);
mSurfaceController = new WindowSurfaceController(...);
w.setHasSurface(true);
```

初始 HIDDEN 是协议，不是偶然：客户端还没有向新生产端画出目标帧，WMS 不能把空内容当成完成画面。

创建参数也说明 frame 与 Buffer 尺寸不是同义词：

- 普通路径以 compat frame 加 `surfaceInsets` 计算 Surface 范围；
- `FLAG_SCALED` 使用 requested size；
- drag-resize 可使用整屏 Surface，避免持续重分配；
- 硬件加速时底层 format 可选 `TRANSLUCENT`；
- `FLAG_SECURE` 映射为 SurfaceControl secure；
- 无 alpha、无 surfaceInsets、非 drag-resize 时才可能加 OPAQUE。

WindowSurfaceController 的主 SurfaceControl 以 WindowState 自己的 container SurfaceControl 为 parent。固定非 BLAST 分支中，它就是承载 Buffer 的图层；它仍被 HIDDEN 标志挡住。

WMS 随后把主 SurfaceControl、可选 BLAST SurfaceControl、frame、Insets、配置与 `outSurfaceSize` 写入 Binder 回复。`W_layer` 只证明服务端图层对象和 `DRAW_PENDING` 已建立，不证明 App 有 Java `Surface`，更不证明已有 Buffer。

## 9. 非 BLAST 与 BLAST 只在“怎样接成 Surface”处分叉

r48 中，WMS 启动时从 native-boot DeviceConfig 读取 `wm_use_blast_adapter`，缺省回退为 false。add 成功结果只有在服务端开关为 true 时才带 `ADD_FLAG_USE_BLAST`，ViewRoot 才令 `useBLAST()` 成立。

两条客户端接线如下：

| 分支 | WMS 图层 | App 怎样得到 `mSurface` |
|---|---|---|
| 非 BLAST 固定主线 | WindowSurfaceController 的主 SurfaceControl 是 Buffer layer | `mSurface.copyFrom(mSurfaceControl)` |
| BLAST | 主 SurfaceControl 改为 container；其下另建非隐藏 BLAST layer，但祖先仍隐藏 | 用 BLAST layer 和 outSurfaceSize 创建/更新 `BLASTBufferQueue`，首次 `getSurface()` 后 `mSurface.transferFrom()` |

BLAST 后续 update 若没有产生新 `Surface`，不会无谓 `transferFrom()`，以免改变 generation id 并迫使 EGL 资源重建。它解决 Buffer 与 SurfaceControl transaction 的同步衔接，不替 View 执行 measure、layout 或 draw。

同一窗口周围至少有七个名字相近但职责不同的对象：

| 对象 | 所在侧与最早时点 | 职责 |
|---|---|---|
| ViewRootImpl `mSurfaceSession` | App；ViewRoot 构造 | 仅供可选 bounds child layer 等客户端 SurfaceControl 使用，不是窗口 Buffer |
| Session `mSurfaceSession` | system_server；该 Session 首窗 attach | 创建服务端窗口 SurfaceControl 的会话 |
| WindowState container SurfaceControl | system_server；add 挂入容器树 | 窗口层级节点，本身不是固定非 BLAST 的 Java Canvas |
| WindowSurfaceController 主 SurfaceControl | system_server；可见 relayout | 非 BLAST 的 Buffer layer，或 BLAST 的父 container |
| BLAST SurfaceControl | system_server；开启 BLAST 的 `createSurfaceLocked()` 内、Binder reply 前 | 主 SurfaceControl 下的非隐藏 Buffer child |
| BLASTBufferQueue | App；开启 BLAST 的 relayout reply 后 | 把 App 生产队列与 BLAST SurfaceControl 接起来的适配器 |
| Java `Surface` | App；wrapper 在 ViewRoot 构造时已有，relayout reply 后才连接 producer | Canvas/HWUI 面向的 Buffer producer 接口 |

因此“ViewRoot 构造已有 SurfaceSession”“WMS add 已有 container SurfaceControl”“relayout 返回 SurfaceControl”都不能单独推出 Java `Surface` 已有效。

## 10. relayout 回复后先接 Surface，再按本次 frame 决定补测

同步 Binder 返回到 `ViewRootImpl.relayoutWindow()` 后，固定非 BLAST 路径先执行 `mSurface.copyFrom(mSurfaceControl)`，并接纳 always-consume-bars、frame、InsetsState 与 controls。外层 `performTraversals()` 随后比较 Surface generation：

```text
surfaceCreated   = 原无效 && 现有效
surfaceDestroyed = 原有效 && 现无效
surfaceReplaced  = generation 变化 && 现有效
surfaceSizeChanged = relayout result 中的显式位
```

新 Surface 会先触发 full redraw、Renderer `initialize(mSurface)`，并在不请求透明区域时尝试 `allocateBuffers()`，然后发出 ViewRoot 的 surface-created callback。随后 ViewRoot 才把返回 frame 的宽高写入 `mWidth/mHeight`；若有 SurfaceHolder，相关尺寸和 created/changed callback 也在这里处理；启用的 Renderer 再以新 `mWidth/mHeight` 执行 `setup(...)`。最后才进入补测条件判断。

这些点仍有严格边界：

- `mSurface.isValid()` 只说明 native producer 句柄可用；
- `initialize()` 只说明 Renderer 已连接；
- `allocateBuffers()` 即使成功，也不表示业务 View 像素已经画入并 queue；
- ViewRoot 的 surface-created callback 与 View 的 `onAttachedToWindow()` 是不同生命周期。

满足任一条件时，ViewRoot 按已经接纳的本次 frame 生成新根 spec 再测：

```text
relayout touch mode 改变焦点
|| mWidth/mHeight 与 host measured size 不同
|| relayout 后重新分发 Insets
|| 接纳了新 Configuration
```

Window `horizontalWeight/verticalWeight` 还可在这次补测后按 `(mWidth-width)*weight` 或 `(mHeight-height)*weight` 调整尺寸，再以 `EXACTLY` 多测一次；差值为负时并不是“扩大”。固定路径的 hint 与本次返回 frame 相同、add/relayout touch mode 一致、`mApplyInsetsRequested` 持续为 false，Insets/config 也稳定，所以 `M_settle` 只完成判断，不发生补测。

同一 traversal 的实际 measure 次数不是固定值：Dialog 宽度试探、Binder 前 Insets、Binder 后 frame/Insets/config、Window weight，以及 layout 中的错误请求都能增加根测量轮次；`View.measure()` cache 又会让根调用数与每个 `onMeasure()` 数不同。

## 11. layout 只处理窗口局部坐标，后面仍有两类窗口回报

补测结算后：

```java
final boolean didLayout =
        layoutRequested && (!mStopped || mReportNextDraw);
if (didLayout) {
    performLayout(lp, mWidth, mHeight);
}
```

`performLayout()` 把 Decor 放在窗口局部原点：

```java
host.layout(0, 0,
        host.getMeasuredWidth(),
        host.getMeasuredHeight());
```

子 ViewGroup 再递归决定孩子坐标。窗口在屏幕上的 `frame.left/top` 属于 WMS；把 Decor layout 到 `(0,0)` 不是忽略屏幕位置。

若某个 View 在第一遍 layout 中留下有效 `requestLayout()`，ViewRoot 会清理标志并在同一 traversal 再做一次 measure/layout。第二遍仍产生的请求被放进 RunQueue，下一次 traversal 才重新投给 Handler，避免当前调用栈无限循环。这是特定防护机制，不是“所有 Android 布局每帧最多两次”的普遍定律。

固定路径确实执行 layout，但后续几项各有自己的门，不能笼统绑定到本轮 layout：

1. 只有 `didLayout=true` 时才采集请求的透明区域；区域变化会经同步 `setTransparentRegion()` 告知 WMS。
2. `didLayout=true` 或 `mRecomputeGlobalAttributes=true` 时，`dispatchOnGlobalLayout()` 才让观察者看到当前几何。
3. 当前有 ComputeInternalInsets listener，或者过去上报过非空值、现在需要清账时，都会重置并计算 content/visible/touchable 区域；只有前面 pending 或计算值变化，才经同步 `IWindowSession.setInsets()` 回报 WMS。后一个历史分支保证最后一个 listener 移除后仍能向 WMS 发送空值。
4. 首轮按 touch mode 与现有焦点尝试恢复默认焦点。

GlobalLayout、InternalInsets 回报和焦点完成都不是 draw；回调也可能再制造下一轮 traversal。

## 12. mFirst 会在 PreDraw 前清零；FIRST_TIME 只登记一笔 draw 债

layout 与焦点阶段之后，源码先执行：

```java
mFirst = false;
mWillDrawSoon = false;
```

然后才解释 relayout 结果：

```java
if ((relayoutResult & RELAYOUT_RES_FIRST_TIME) != 0) {
    reportNextDraw();
}
if ((relayoutResult & RELAYOUT_RES_BLAST_SYNC) != 0) {
    reportNextDraw();
    setUseBLASTSyncTransaction();
}
```

`reportNextDraw()` 只有在 `mReportNextDraw` 原先为 false 时才调用一次 `drawPending()` 增加 `mDrawsNeededToReport`，随后把布尔位置 true。因此同一结果同时带 FIRST_TIME 与 BLAST_SYNC 时会调用两次方法，却不会为这两行重复增加计数。

接着才到真正 draw gate：

```java
boolean cancelDraw =
        treeObserver.dispatchOnPreDraw() || !isViewVisible;
```

`dispatchOnPreDraw()` 内部对每个 listener 执行 `cancelDraw |= !listener.onPreDraw()`：listener 返回 false 才取消。可见窗口被取消时会重新 `scheduleTraversals()`；但 attach、relayout、Surface 创建都不会倒退，`mFirst` 也已经是 false。

这解释了常见“已 attached、有尺寸、有 Surface，但首窗仍没出现”：WMS 的 Buffer layer 仍隐藏并等待 `finishDrawing()`，PreDraw 可以把这笔 draw 债留给后续 traversal。

## 13. 硬件、软件和 SurfaceHolder 用不同方式结清 draw 债

固定路径进入 `performDraw()`。初始 full-redraw 让 dirty 覆盖整个窗口，硬件分支会在需要 report 时安装 frame-complete callback，再调用：

```java
mAttachInfo.mThreadedRenderer.draw(
        mView, mAttachInfo, this);
```

UI 线程仍要遍历 View、运行 `draw/onDraw` 并记录 RenderNode/DisplayList；“硬件加速”不等于业务绘制完全离开主线程。`ThreadedRenderer.draw()` 返回和 `performTraversals()` 返回，只能证明客户端已走完本轮记录/提交 API，不证明 GPU 已完成或 Buffer 已 present。

固定硬件路径的回报是异步的：

```text
Renderer frame-complete callback
→ finishBLASTSync(...)
→ Handler.postAtFrontOfQueue(...)
→ pendingDrawFinished()
→ reportDrawFinished()
→ IWindowSession.finishDrawing(...)
```

回调可以在渲染线程侧触发，但 `pendingDrawFinished()` 的 Runnable 要等主线程退出当前 traversal 后才能在同一 Looper 执行，所以固定路径有 `D_submit < C_ready < F_commit`。

还有一层容易漏掉的 WindowCallbacks 等待：`updateContentDrawBounds()` 若要求额外窗口绘制，`requestDrawWindow()` 会按 callback 数创建 `mWindowDrawCountDown`；`performDraw()` 在报告前执行无超时 `await()`。对应 callback 必须调用 `reportDrawFinish()` 才能释放 UI 线程。固定路径明确没有这笔 latch，通用路径却可能停在这里。

其他分支不能硬套这个时序：

| 分支 | draw/report 方式 |
|---|---|
| ThreadedRenderer 可异步 | frame-complete callback 后回主 Handler |
| Renderer 不能使用异步回报 | 清 callback，必要时 fence，再在当前 `performDraw()` 内调用 `pendingDrawFinished()` |
| 软件绘制 | `Surface.lockCanvas()`、View 树 draw、`unlockCanvasAndPost()` 后在当前调用栈结算 |
| 根 Surface 被接管且 `mSurface.isValid()` | ViewRoot 自己不画，由 `surfaceRedrawNeededAsync` callbacks 聚合，随后以 Handler 消息结算 |

“Renderer 不能使用异步回报”和普通软件绘制这两条非 SurfaceHolder 路径，可能在 `performTraversals()` 返回前就同步调用 `finishDrawing()`，所以不能把固定硬件路径的 `D_submit < F_commit` 移植过去。根 Surface 已接管且有效时，即使没有 callback、或 callback 同步完成，也只会调用 `postDrawFinished()` 发送 `MSG_DRAW_FINISHED`，再由后续主 Looper 消息执行 `pendingDrawFinished()`，不会在当前栈内直接结算；若 holder 存在但 Surface 无效，则会落入 `!usingAsyncReport` 的当前栈结算分支。并且 `draw()` 某些失败/接管分支返回 false 后，非异步报告仍可能平账；`reportNextDraw` 不是“只在成功画出业务像素后才允许回报”的事务保证。

`finishDrawing(IWindow, Transaction)` 传的是窗口身份和可选 SurfaceControl transaction，不是 Bitmap 或整块像素。它是客户端对 WMS 的“本轮所要求 draw 已完成”协议。

## 14. finishDrawing 只推进 WMS 状态机；show 和 present 仍在后面

Session 把 `finishDrawing()` 同步转给 WMS。普通非 BLAST 路径中：

```text
WindowState.finishDrawing
→ WindowStateAnimator.finishDrawingLocked
→ DRAW_PENDING → COMMIT_DRAW_PENDING
→ WindowManagerService 请求下一次 surface placement
```

这里的服务端 COMMIT 写入和 `requestTraversal()` 才是固定路径的 `F_commit`，不是 App 观察到 Binder reply 的时刻。正常的同步调用返回可以证明服务端已经经过 `F_commit`；但 global lock 释放后，AnimationThread 可能在 reply 到达 App 之前就取得锁并推进后续 placement。因此可靠偏序是 `F_commit < W_drawn < S_show`，客户端代理返回与 `W_drawn` 之间没有固定先后。

后续 placement 遍历窗口时，`commitFinishDrawingLocked()` 把状态推进为 `READY_TO_SHOW`。只有 Activity 没有额外等待，或者普通窗口满足 `ActivityRecord.canShowWindows()`，才调用 `performShowLocked()`；它通过 `isReadyForDisplay()` 后写 `HAS_DRAWN` 并安排 animation。

Activity 的 `onFirstWindowDrawn()`、all-drawn/starting-window 协调也处在 WMS 状态层。源码甚至在 `performShowLocked()` 的 ready gate 前调用 first-window 通知，因此这些名字都不能充当像素 present 时间戳。

真正取消初始隐藏还要等 `WindowState.prepareSurfaces()`：

```text
WindowStateAnimator.prepareSurfaceLocked
→ draw state 已是 HAS_DRAWN
→ WindowSurfaceController.showRobustlyInTransaction
→ SurfaceControl.show
→ WMS 关闭/提交 Surface transaction
```

`S_show` 只把图层可见性事务交给合成系统。目标 Buffer 的 acquire/latch 与 show 不能强排：SurfaceFlinger 也可提前 latch、release 暂时隐藏或 offscreen 图层的 ready Buffer；有效 show 与合适的 Buffer 要在可见合成前汇合，之后才由 HWC/GPU 与显示时序完成 `F_present`。静态 Java 源码不能证明某台设备的 `F_present`，应使用该版本可获得的 present fence、SurfaceFlinger latency 或等价运行时显示证据。

## 15. 失败、重入与诊断证据都必须停在最窄完成点

固定主线排除了异常；真实系统没有一个跨 App、WMS、RenderThread、SurfaceFlinger 的原子事务：

| 分支 | r48 行为 | 诊断边界 |
|---|---|---|
| callback 到达时 `mView==null` 或 `!mAdded` | `performTraversals()` 立即返回 | traversal 被调度过不等于 attach |
| attach/Insets/layout/observer 自身抛 RuntimeException | 当前调用栈可被截断，没有覆盖整方法的统一回滚 | 不能假定 `mIsInTraversal/mFirst` 等过程位已走到底 |
| WMS 找不到原 WindowState | 在锁内直接返回 0，未走该方法底部的显式 identity restore | 0 也是正常位集合，不能当作独立成功回执 |
| attrs 改了 type 或 providesInsetsTypes | WMS 抛 `IllegalArgumentException`；它不是 `RELAYOUT_RES_*` | requested size 等较早写入不因此自动回滚 |
| `shouldRelayout=false` | 不创建新 Surface；VISIBLE 且旧 Surface 存在时可返回旧 control，否则释放 out control | Decor.VISIBLE 不足以证明新 Surface |
| `createSurfaceLocked()` 内部捕获资源/构造异常 | draw state 回到 `NO_SURFACE`，返回 null 并释放 out control；结果位不构成独立错误码 | 不能因 relayout 返回非负就认定有 Surface |
| 外层 `createSurfaceControl()` 调用仍抛异常 | WMS 强制更新输入窗口、显式恢复身份并返回 0 | 0 不足以区分这条失败与正常零结果位 |
| relayout Binder `RemoteException` | `performTraversals()` 的局部 catch 吞掉异常并继续 | 本轮后续字段可能沿用旧值，不能宣称 WMS 成功 |
| Renderer 初始化资源不足 | ViewRoot 请求 WMS 回收图形内存；WMS 只有在“回收了泄漏 Surface 或杀掉候选 App”时返回 true，二者都没做到而返回 false 且当前不是 system UID 时，客户端会调用 `killProcess(myPid())`；代码随后置 `mLayoutRequested=true`，调用点若继续执行则提前返回并跳过尾部 `mIsInTraversal=false` | Surface control 存在不等于 Renderer 可画，WMS 返回 true 也不等于杀过进程 |
| relayout 后的 Insets/control listener 重入改 visibility 或 remove | 后面的 `isViewVisible` 仍是 traversal 开头快照 | PreDraw 的可见性判断不自动重读最新 View 状态 |
| PreDraw 取消 | 可见时再排 traversal；WMS 继续等待 draw 回报 | `mFirst=false` 不等于首帧已提交 |
| `finishDrawing()` 抛 `RemoteException` | `reportDrawFinished()` 已先清待报告计数并吞异常 | 客户端本地平账不证明 WMS 进入 COMMIT，也不保证自动重试 |
| Activity/transition 尚不允许 show | draw state 可停在 `READY_TO_SHOW` | `finishDrawing()` 不等于 `HAS_DRAWN` |
| show 命令已写入或提交，但没有目标 Buffer present 证据 | WMS 已发出解除初始隐藏的事务意图，合成侧进度仍未知 | WMS show 不等于用户看到 |

回调还能在同一主线程重入 `requestLayout()`、改属性、改 Insets 或 remove View；system_server 也可能并发发 resize/visibility。只有固定前提成立时，十八点才构成这一条局部全序。

排障时应让证据停在它真正证明的位置：

| 证据 | 至少证明 | 仍不能证明 |
|---|---|---|
| `decor.isAttachedToWindow()` | Decor 的 `mAttachInfo` 已非空 | 整棵树 attach、window-attached 通知或初始 Insets 已返回 |
| `getMeasuredWidth/Height()` 非零 | 某次 View 测量给出结果 | 等于 WMS frame |
| 单独读取客户端 `mWinFrame` | 已有 add frame hint、relayout reply 或 resize 中最近一次写入的快照 | 一定完成过 relayout/window placement |
| WMS WindowState frame 加本次 placement 证据 | 服务端本轮窗口布局快照存在 | App 已收到或按该结果补测/layout |
| `mSurface.isValid()` | App 有可用 producer 句柄 | 已 queue 目标 Buffer |
| WSA `COMMIT_DRAW_PENDING` | WMS 收到所需 draw 回报 | 图层已 show |
| WSA `HAS_DRAWN` | WMS 显示门已推进并安排可见 | show 事务已被 SF 应用 |
| `showRobustlyInTransaction()` 返回 true | 当前隐藏条件已解除，且内部可见状态机认为 Surface 已 shown | 本次一定调用了 `SurfaceControl.show()`、事务已应用、合成侧已取消隐藏或目标 Buffer 已 present |
| 目标帧 present 时间戳 | 所选帧到达显示完成点 | 较早阶段没有卡顿 |

本章结算点不是一个笼统“首帧完成”，而是：`M_offer` 结算 App 提议，`W_layout` 结算 WMS frame，`S_ready` 结算 producer 接线，`L_tree` 结算 View 几何，`F_commit` 结算客户端 draw 回报；`F_present` 仍需独立证据。

## 16. 九组只读练习：重建 attach、尺寸、Surface 与显示账

以下命令只检查文件并检索文本。可在 Android 11 r48 源码根运行，也可先设置 `ANDROID_BUILD_TOP`；每组都应在 Bash 3.2 与 Zsh 5.9 中独立以 0 退出。

### 练习 1：证明 callback 调度、barrier 与首次可见性的顺序

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
A="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
Y="$SRC/frameworks/base/core/java/android/app/Activity.java"
test -f "$V" && test -f "$A" && test -f "$Y"
grep -nE 'int getHostVisibility\(\)|void scheduleTraversals\(\)|postSyncBarrier\(\)|CALLBACK_TRAVERSAL|void doTraversal\(\)|removeSyncBarrier\(|private void performTraversals\(\)|host == null|mIsInTraversal = true' "$V"
grep -nE 'decor.setVisibility\(View.INVISIBLE\)|wm.addView\(decor, l\)|r.activity.makeVisible\(' "$A"
grep -nE 'void setVisible\(boolean visible\)|mVisibleFromClient = !mWindow.getWindowStyle|if \(!mVisibleFromClient && !mFinished\)' "$Y"
```

画出 `addView 返回 → makeVisible → T_start`，再说明多次 `scheduleTraversals()` 为什么不等于多次 callback。

完成标准：`T_start` 定义在 callback 真正执行，不定义在首次 `requestLayout()`。

### 练习 2：区分 parent、AttachInfo、初始 Insets 与 RunQueue

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
W="$SRC/frameworks/base/core/java/android/view/View.java"
Q="$SRC/frameworks/base/core/java/android/view/HandlerActionQueue.java"
test -f "$V" && test -f "$W" && test -f "$Q"
grep -nE 'view.assignParent\(this\)|mFirst = true|host.dispatchAttachedToWindow\(|dispatchOnWindowAttachedChange\(true\)|dispatchApplyInsets\(host\)|getRunQueue\(\).executeActions|mFirst = false' "$V"
grep -nE 'void dispatchAttachedToWindow\(AttachInfo info|mAttachInfo = info|onAttachedToWindow\(\)|private HandlerActionQueue getRunQueue\(\)|mRunQueue.executeActions\(info.mHandler\)|getRunQueue\(\).post\(action\)' "$W"
grep -nE 'void executeActions\(Handler handler\)|handler.postDelayed\(|mActions = null|mCount = 0' "$Q"
```

按真实顺序标出 ViewParent、View AttachInfo、WindowInsets 与 Handler action，判断哪些在 `A_tree` 同步完成。

完成标准：RunQueue 的 `executeActions()` 是 post，不把 action 本体插进 measure 之前。

### 练习 3：推导 desired size、根 MeasureSpec 与 Dialog 试探

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
W="$SRC/frameworks/base/core/java/android/view/View.java"
test -f "$V" && test -f "$W"
grep -nE 'shouldUseDisplaySize\(lp\)|mDisplay.getRealSize\(|config.screenWidthDp|frame contains the frameHint|measureHierarchy\(|boolean goodMeasure = false|config_prefDialogWidth|baseSize != 0 && desiredWindowWidth > baseSize|MEASURED_STATE_TOO_SMALL|if \(!goodMeasure\)|windowSizeMayChange = true|getRootMeasureSpec\(|MeasureSpec.EXACTLY|MeasureSpec.AT_MOST' "$V"
grep -nE 'public final void measure\(|mMeasureCache.indexOfKey|onMeasure\(widthMeasureSpec, heightMeasureSpec\)|PFLAG_MEASURED_DIMENSION_SET|mMeasureCache.put' "$W"
```

分别代入 `MATCH_PARENT`、`WRAP_CONTENT`、600px，并列出一次 `measureHierarchy()` 内可能出现的三档 Dialog 宽度。

完成标准：frame hint 是初测输入；小档/中档命中会走 `goodMeasure` 短路；根 measure 次数与每个子 View 的 `onMeasure()` 次数不是同一统计。

### 练习 4：还原 relayout 门、参数与同步 AIDL

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
Q="$SRC/frameworks/base/core/java/android/view/IWindowSession.aidl"
test -f "$V" && test -f "$Q"
grep -nE 'mApplyInsetsRequested|collectViewAttributes\(\)|SOFT_INPUT_ADJUST_UNSPECIFIED|mWindowAttributesChanged|mForceNextWindowRelayout|RELAYOUT_INSETS_PENDING|mThreadedRenderer.pause\(\)|relayoutWindow\(params, viewVisibility, insetsPending\)|mWindowSession.relayout\(' "$V"
grep -nE '^interface IWindowSession|int relayout\(IWindow window|out Rect outFrame|out SurfaceControl outSurfaceControl|out Point outSurfaceSize|out SurfaceControl outBlastSurfaceControl' "$Q"
```

写出首次为什么必跨 Binder、后续为何 attrs 可以为 null，以及 measured size 如何独立于 attrs 传输。

完成标准：AIDL 方法不是 oneway；`RELAYOUT_INSETS_PENDING` 不是 WindowInsets 对象。

### 练习 5：证明 WMS 先布局 frame，再创建窗口 Buffer 图层

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
W="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
D="$SRC/frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java"
test -f "$M" && test -f "$W" && test -f "$D"
grep -nE 'win.setRequestedSize\(|win.mAttrs.type != attrs.type|win.mRelayoutCalled = true|win.setViewVisibility\(|final boolean shouldRelayout|performSurfacePlacement\(true|win.relayoutVisibleWindow\(|createSurfaceControl\(|win.getCompatFrame\(|outSurfaceSize.set' "$M"
grep -nE 'int relayoutVisibleWindow\(|!wasVisible|!isDrawnLw\(\)|RELAYOUT_RES_FIRST_TIME' "$W"
grep -nE 'getDisplayPolicy\(\).layoutWindowLw\(|w.updateLastFrames\(\)|w.updateLastInsetValues\(\)' "$D"
```

按源码排出 `W_record → W_layout → W_layer → B_reply`，并解释 `shouldRelayout=false` 为什么能阻止新 Surface。

完成标准：WMS frame 不是 App measured size 的原样回显；`FIRST_TIME` 也不是调用次数。

### 练习 6：追踪隐藏 SurfaceControl、SurfaceSession 与 BLAST 子层

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
T="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowToken.java"
N="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java"
D="$SRC/frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java"
C="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowSurfaceController.java"
test -f "$M" && test -f "$T" && test -f "$N" && test -f "$D"
test -f "$S" && test -f "$A" && test -f "$C"
grep -nE 'win.mToken.addWindow\(win\)' "$M"
grep -nE 'void addWindow\(final WindowState win\)|addChild\(win, mWindowComparator\)' "$T"
grep -nE 'protected void addChild\(E child, Comparator|child.setParent\(this\)|void onParentChanged\(ConfigurationContainer newParent|createSurfaceControl\(false|getSyncTransaction\(\).show\(mSurfaceControl\)' "$N"
grep -nE 'SurfaceControl.Builder makeChildSurface\(WindowContainer child\)|setContainerLayer\(\)' "$D"
grep -nE 'void windowAddedLocked|mSurfaceSession = new SurfaceSession\(\)|mNumWindow\+\+' "$S"
grep -nE 'void resetDrawState\(\)|mDrawState = DRAW_PENDING|SurfaceControl.HIDDEN|calculateSurfaceBounds\(|new WindowSurfaceController\(|w.setHasSurface\(true\)' "$A"
grep -nE 'setParent\(win.getSurfaceControl\(\)\)|setContainerLayer\(\)|setParent\(mSurfaceControl\)|setHidden\(false\)|setBLASTLayer\(\)' "$C"
```

画出 WindowState container、WindowSurfaceController 主层和可选 BLAST layer 的 parent 关系，并标出哪一层初始隐藏。

完成标准：服务端 Session 的 SurfaceSession 早于本次 relayout；创建图层不等于 App 已有 Java `Surface`。

### 练习 7：比较非 BLAST copyFrom 与 BLAST transferFrom

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
S="$SRC/frameworks/base/core/java/android/view/Surface.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
D="$SRC/frameworks/base/core/java/com/android/internal/policy/DecorView.java"
test -f "$V" && test -f "$S" && test -f "$M" && test -f "$D"
grep -nE 'mSurface = new Surface\(\)|ADD_FLAG_USE_BLAST|mUseBLASTAdapter = true|boolean useBLAST\(\)|mSurfaceHolderCallback =|mSurfaceHolder = new TakenSurfaceHolder|mSurface.copyFrom\(mSurfaceControl\)|getOrCreateBLASTSurface\(|new BLASTBufferQueue\(|mSurface.transferFrom\(|surfaceCreated|allocateBuffers\(\)|initialize\(mSurface\)' "$V"
grep -nE 'boolean isValid\(\)|void copyFrom\(SurfaceControl other\)|nativeGetFromSurfaceControl|void transferFrom\(Surface other\)' "$S"
grep -nE 'WM_USE_BLAST_ADAPTER_FLAG|mUseBLAST = DeviceConfig.getBoolean|ADD_FLAG_USE_BLAST' "$M"
grep -nE 'implements RootViewSurfaceTaker|willYouTakeTheSurface\(\)|mTakeSurfaceCallback' "$D"
```

分别写出两条 `W_layer → S_ready` 接线，并说明 generation id 为什么只描述 Surface 句柄代际。

完成标准：Decor 实现接口不等于它接管根 Surface；Java wrapper 存在、`isValid()`、Renderer initialize 和 Buffer present 也是不同完成点。

### 练习 8：重建补测、layout 与 InternalInsets 回报

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
Q="$SRC/frameworks/base/core/java/android/view/IWindowSession.aidl"
test -f "$V" && test -f "$Q"
grep -nE 'mWidth != frame.width\(\)|focusChangedDueToTouchMode|updatedConfiguration|performMeasure\(childWidthMeasureSpec|lp.horizontalWeight|lp.verticalWeight|performLayout\(lp, mWidth, mHeight\)|requestLayoutDuringLayout\(|dispatchOnGlobalLayout\(\)|hasComputeInternalInsetsListeners\(\)|mHasNonEmptyGivenInternalInsets|if \(isViewVisible\)|dispatchOnComputeInternalInsets\(|insetsPending \|\| !mLastGivenInsets.equals|mWindowSession.setInsets\(' "$V"
grep -nE 'void setInsets\(IWindow window' "$Q"
```

列出 Binder 后触发补测的四类条件，再跟踪 layout 中第一遍、补救遍与下一 traversal 三个处理层次。

完成标准：Decor 的 `(0,0)` 是窗口局部原点；InternalInsets 是 View→WMS 的回报。

### 练习 9：从 PreDraw 追到 show，并在 present 前停笔

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
T="$SRC/frameworks/base/core/java/android/view/ViewTreeObserver.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java"
W="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
D="$SRC/frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java"
C="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowSurfaceController.java"
R="$SRC/frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java"
P="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowSurfacePlacer.java"
test -f "$V" && test -f "$T" && test -f "$M" && test -f "$A" && test -f "$W"
test -f "$D" && test -f "$C" && test -f "$R" && test -f "$P"
grep -nE 'RELAYOUT_RES_FIRST_TIME|reportNextDraw\(\)|dispatchOnPreDraw\(\)|performDraw\(\)|setFrameCompleteCallback|postDrawFinished\(\)|MSG_DRAW_FINISHED|pendingDrawFinished\(\)|mWindowSession.finishDrawing\(' "$V"
grep -nE 'cancelDraw \|= !\(access.get\(i\).onPreDraw\(\)\)' "$T"
grep -nE 'void finishDrawingWindow\(|win.finishDrawing\(|mWindowPlacerLocked.requestTraversal\(\)|void closeSurfaceTransaction\(String where\)|SurfaceControl.closeTransaction\(\)' "$M"
grep -nE 'mDrawState = COMMIT_DRAW_PENDING|commitFinishDrawingLocked\(\)|mDrawState = READY_TO_SHOW|void prepareSurfaceLocked\(final boolean recoveringMemory\)|showSurfaceRobustlyLocked\(\)' "$A"
grep -nE 'boolean performShowLocked\(\)|onFirstWindowDrawn\(|isReadyForDisplay\(\)|mWinAnimator.mDrawState = HAS_DRAWN|void prepareSurfaces\(\)' "$W"
grep -nE 'winAnimator.commitFinishDrawingLocked\(\)|prepareSurfaces\(\)' "$D"
grep -nE 'boolean showRobustlyInTransaction\(\)|private boolean showSurface\(\)|mSurfaceControl.show\(\)' "$C"
grep -nE 'applySurfaceChangesTransaction\(\)|closeSurfaceTransaction\("performLayoutAndPlaceSurfaces"\)' "$R"
grep -nE 'void requestTraversal\(\)|mService.mAnimationHandler.post\(mPerformSurfacePlacement\)' "$P"
```

画出 `P_allow → D_submit → C_ready → F_commit → W_drawn → S_show`，并在图末另画一条虚线指向运行时 `F_present`。

完成标准：源码搜索能证明 WMS 状态与 show 调用，不能单靠这些 Java 行证明 SurfaceFlinger latch 或硬件 present。

完成九组练习后，应能对任何“首帧已经完成”的说法追问：完成的是 App 尺寸提议、WMS frame、Java Surface、View layout、客户端 draw 回报、WMS show，还是目标帧 present。
