# 239 Android 系统拖放：DragState、DragEvent 跨窗口分发与 URI 权限链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一次拖放究竟迁移了什么

上一章只回答了 pointer 流怎样从发起窗口转交给 drag InputChannel。本章继续追同一次操作：App 画出的拖影怎样交给 WMS，哪些窗口先得到数据描述，最终命中的 View 怎样拿到 `ClipData`，URI 访问能力又怎样独立建立和释放。

先把“拖放”拆成四本账：

| 账本 | 记录什么 | 关键完成点 |
|---|---|---|
| 输入控制账 | 当前 pointer 流由哪个 `InputChannel` 接收 | `transferTouchFocus()` 改写路由并为新连接补事件 |
| 窗口通知账 | 哪些 `WindowState` 已由 WMS 发出 `ACTION_DRAG_STARTED` | 加入 `mNotifiedWindows` |
| View 兴趣账 | 一个窗口内哪些 View 对 STARTED 返回 `true` | `mChildrenInterestedInDrag` 与 `PFLAG2_DRAG_CAN_ACCEPT` |
| 数据能力账 | DROP 数据、URI grant、结果回报与资源回收 | `take*()`、`reportDropResult()`、`closeLocked()` 各自收敛 |

这四本账不会自动等价。WMS 向一个窗口发过 STARTED，不代表该窗口内已有 View 接受；View 对 DROP 返回 `true`，也不代表数据已经复制或 URI 权限已经持久化；`finishInputEvent()` 更不代表拖放业务结束。

全章的主问题是：

```text
一条正在进行的 pointer 流，怎样被转换为跨窗口 DragEvent 协议，
并在窗口资格、View 兴趣、DROP 回报和 URI 授权彼此独立的条件下结束？
```

## 2. 两种事件、三道门与全链路

拖放同时存在两种事件：

- 发起窗口在当前 `TouchState` 中拥有的 pointer 份额转入 system_server 的 drag 专用连接，用来移动拖影、做窗口命中并决定何时尝试 DROP；同一 split 流的其他窗口份额以及 monitor 路径不因这次 transfer 自动消失。
- `DragEvent` 由 WMS 发给候选 `IWindow`，再由 `ViewRootImpl`、`ViewGroup` 分给 App 的 View 树。

主链如下：

```text
View.startDragAndDrop()
  ├─ App 创建并绘制 drag Surface
  ├─ IWindowSession.performDrag()
  └─ WMS 创建 DragState
       ├─ 注册 drag InputChannel / InputWindowHandle
       ├─ 同步 input window 后 transferTouchFocus()
       ├─ 向合格窗口发 STARTED，并排队拖影 show/reparent
       ├─ MOVE：移动 Surface + 窗口级 LOCATION/EXITED
       └─ UP / CANCEL / stylus-release MOVE：最终重新命中并发 DROP
                            ├─ View 树返回 consumed
                            ├─ 可选 take URI permission
                            └─ reportDropResult()
                                  ├─ success：直接 close
                                  └─ failure：动画后 close
                                             └─ 向已通知窗口逐个发 ENDED
```

最终目标要连续通过三道门：

1. STARTED 时，`WindowState` 通过 WMS 的 local/global、版本与 profile 筛选。
2. App 收到 STARTED 后，View 或其子树返回 `true`，建立窗口内部的兴趣集合。
3. DROP 时，WMS 重新命中一个已通知窗口；该窗口的 View 树再按坐标选择曾接受 STARTED 的目标，并决定 consumed。

第 1 道门不等待第 2 道门的结果，因为 `IWindow` 是 oneway 接口。WMS 后续仍可能向一个“已通知但客户端兴趣集合保持为空”的窗口发 LOCATION 和 DROP；典型的 `ViewGroup` 根不会把 DROP 交给未入选的 child，最后报告 `false`。这项客户端行为不是 WMS 的窗口资格校验结果。

核心源码地图：

```text
frameworks/base/core/java/android/view/View.java
frameworks/base/core/java/android/view/ViewRootImpl.java
frameworks/base/core/java/android/view/ViewGroup.java
frameworks/base/core/java/android/view/DragEvent.java
frameworks/base/core/java/android/view/DragAndDropPermissions.java
frameworks/base/services/core/java/com/android/server/wm/Session.java
frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
frameworks/base/services/core/java/com/android/server/wm/DragState.java
frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
```

## 3. App 入口：数据、阴影与 localState 分三路

`View.startDragAndDrop(data, shadowBuilder, myLocalState, flags)` 先要求 View 已 attach，且所属 `ViewRootImpl` 的 `Surface` 有效。`data` 非空时，它在出进程前调用：

```text
data.prepareToLeaveProcess((flags & DRAG_FLAG_GLOBAL) != 0)
```

这一步检查和准备 `ClipData` 内的 URI/Intent；它不是 URI grant。`myLocalState` 根本不进入 `performDrag()` 的 Binder 参数，只在成功后写进发起方那个 `ViewRootImpl.mLocalDragState`。因此 r48 的实现边界比“同进程共享”更窄：同一进程的另一个顶层窗口有另一份 ViewRoot，默认仍得到 `null`。

`DragShadowBuilder` 返回 buffer 尺寸和触点在阴影中的偏移。任一尺寸或偏移为负会抛 `IllegalStateException`，但代码不检查偏移是否落在阴影矩形内。若任一尺寸为零：targetSdk P 之前的兼容模式把宽和高都改成 `1`，P 及以后抛异常。

随后 App 创建以当前 ViewRoot Surface 为 parent 的透明 `SurfaceControl`，清空 Canvas，调用 `onDrawShadow()`，再提交 buffer。阴影是 App 主动画出的独立 Surface，不是 WMS 截图。WMS 后面持有 `SurfaceControl`，并通过 transaction 改 position、alpha、scale、visibility 与 parent。

起点取自 `ViewRootImpl` 保存的最后触点与 source，而不是从调用该 API 的 View 几何中心推导。代码甚至复用了原先装阴影尺寸的 `Point` 来承载最后触点；是否仍是当前活跃触摸，要留给 WMS 和 InputDispatcher 的后续路径判断。

`performDrag()` 返回非空 token 后，App 才缓存三项：

```text
AttachInfo.mDragSurface = Surface 包装
AttachInfo.mDragToken   = 源端取消 token
ViewRoot.mLocalDragState = 仅本 ViewRoot 可见的对象
```

返回 `true` 只说明同步的启动调用走到 WMS 成功出口。它不等待其他进程处理 STARTED，不等待目标 View 表态，也不等待拖影像素 present。返回空 token 时，App 销毁本地 `Surface`；WMS 的失败路径则释放自己收到的 `SurfaceControl` 引用，两端引用不能混成一次释放。

### 练习 1：划出 App 启动的失败线与所有权线

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public final boolean startDragAndDrop(ClipData data, DragShadowBuilder shadowBuilder,' frameworks/base/core/java/android/view/View.java
grep -n -F 'startDragAndDrop called on a detached view.' frameworks/base/core/java/android/view/View.java
grep -n -F 'startDragAndDrop called with an invalid surface.' frameworks/base/core/java/android/view/View.java
grep -n -F 'data.prepareToLeaveProcess((flags & View.DRAG_FLAG_GLOBAL) != 0);' frameworks/base/core/java/android/view/View.java
grep -n -F 'shadowBuilder.onProvideShadowMetrics(shadowSize, shadowTouchPoint);' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (!sAcceptZeroSizeDragShadow) {' frameworks/base/core/java/android/view/View.java
grep -n -F '.setName("drag surface")' frameworks/base/core/java/android/view/View.java
grep -n -F 'shadowBuilder.onDrawShadow(canvas);' frameworks/base/core/java/android/view/View.java
grep -n -F 'root.getLastTouchPoint(shadowSize);' frameworks/base/core/java/android/view/View.java
grep -n -F 'token = mAttachInfo.mSession.performDrag(' frameworks/base/core/java/android/view/View.java
grep -n -F 'mAttachInfo.mDragToken = token;' frameworks/base/core/java/android/view/View.java
grep -n -F 'root.setLocalDragState(myLocalState);' frameworks/base/core/java/android/view/View.java
grep -n -F 'surface.destroy();' frameworks/base/core/java/android/view/View.java
```

按“抛异常、返回 false、token 非空”三类出口整理表格，再说明 App 的 `Surface` 包装、WMS 的 `SurfaceControl` 与 `localState` 分别由谁持有。

## 4. performDrag：身份快照与启动事务

`IWindowSession.performDrag()` 是同步 Binder 调用。`Session` 先清除当前 Binder identity，却把创建 Session 时保存的 `mPid/mUid` 显式传给 `DragDropController`；后续 URI grant 使用的源 UID 不是目标 App 可改写的临时 Binder 身份。

默认 AOSP 流程先在 WMS 锁外调用厂商扩展的 `prePerformDrag()`，再在全局锁内依次拒绝：

```text
扩展回调拒绝
已有未 closing 的 DragState
发起 IWindow 找不到对应 WindowState
发起窗口 cantReceiveTouchInput()
发起窗口没有 DisplayContent
```

这里没有重新验证“当前 pointer 的 touched window 仍是发起窗口”，源码只留下未完成事项注释。真正的常见校验来自稍后的 `transferTouchFocus()`；而上一章已经证明它的 `true` 也只是弱成功，不能反推两端补事件已经交付。

控制器创建一个本次 drag token。`DragState` 构造器收到的临时 Binder 立刻被 `mDragState.mToken = dragToken` 覆盖；r48 只有一个 `mToken` 字段，后面还会再次换角色。创建状态后，局部变量 `surface` 被置空，表示后续失败由 `DragState.closeLocked()` 回收，而不是外层 finally 再释放。

输入接管成功以后，未设置 `DRAG_FLAG_OPAQUE` 的阴影 alpha 为 `0.7071`，设置后为 `1`。接下来的启动顺序是：

```text
mData = data
广播 STARTED
必要时把鼠标图标改为 grabbing
记录 shadow touch offset
排队 alpha / position / show / reparentToOverlay（此处未 apply）
scheduleAnimation
主动执行一次 notifyLocationLocked(startX, startY)
返回 dragToken
```

所以第一条 LOCATION 不需要等待 MOVE；STARTED 还早于拖影属性排队和这次初始命中。跨进程 `dispatchDragEvent()` 是 oneway，别的进程可能在源 App 尚未从 `startDragAndDrop()` 返回时就开始处理 STARTED。

这里还藏着一个 r48 的 transaction 断点。`DragState` 用 `mTransactionFactory.get()` 保存一只独立 transaction；`showInputSurface()` 较早的 `apply(true)` 已提交 input Surface。控制器随后只向同一对象追加拖影的 alpha、position、show 与 reparent，没有调用 `apply()`，也没有把它 merge 到 `callingWin.scheduleAnimation()` 使用的 transaction。`notifyLocationLocked()` 同样不提交。第一条常规 MOVE 在 `notifyMoveLocked()` 追加位置并 `.apply()` 时，才把这些启动属性一起送出；若没有 MOVE，不能从 `performDrag()` 成功返回推导拖影 show 已提交。

## 5. 专用输入接管：先让 handle 可见，再迁 pointer 流

AOSP 默认 `IDragDropCallback.registerInputChannel()` 先执行 `state.register(display)`：

```text
创建名为 drag 的 InputChannel pair
注册 server channel
在 WMS Handler Looper 上创建 DragInputEventReceiver
创建 TYPE_DRAG InputApplicationHandle / InputWindowHandle
暂停该 Display 的 rotation
创建全屏 input Surface 并挂 InputWindowInfo
syncInputWindows().apply(true)
调用 InputManagerService.transferTouchFocus(source, drag channel)
```

同步提交的目的，是让 InputDispatcher 在迁移前先认识目标 channel token；它不是等待硬件合成或客户端处理。drag handle 的 frame 覆盖显示屏、`hasFocus=true`、`canReceiveKeys=false`。注释说空 `touchableRegion` 用来拒绝新触摸，但 `layoutParamsFlags=0` 形成 touch-modal 语义；如第 238 章所见，不能把空 Region 单独当成绝对隔离条件。本章只依赖现有流的显式迁移。

若 transfer 返回 `false`，`performDrag()` 返回空 token。由于 STARTED 尚未广播，finally 看到 `!isInProgress()`，会逻辑关闭状态、移除 input/drag Surface，并把 channel teardown 投递到正确 Looper。rotation 的恢复因此可能晚于同步返回。

transfer 返回 `true` 也不证明 drag 客户端已经看到补 `DOWN`。它只允许 WMS 继续建立 STARTED 与视觉状态；Connection 缺失、broken 或 publish 失败等弱成功边界仍沿用上一章结论。

### 练习 2：证明注册、同步、迁移与首个 LOCATION 的顺序

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'return mDragDropController.performDrag(mSurfaceSession, mPid, mUid, window,' frameworks/base/services/core/java/com/android/server/wm/Session.java
grep -n -F 'final boolean callbackResult = mCallback.get().prePerformDrag(window, dragToken,' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (dragDropActiveLocked()) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (callingWin == null || callingWin.cantReceiveTouchInput()) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'final DisplayContent displayContent = callingWin.getDisplayContent();' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState = new DragState(mService, this, token, surface, flags, winBinder);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.mToken = dragToken;' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (!mCallback.get().registerInputChannel(' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mTransaction = service.mTransactionFactory.get();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'state.register(display);' frameworks/base/services/core/java/com/android/server/wm/WindowManagerInternal.java
grep -n -F 'return service.transferTouchFocus(source, state.getInputChannel());' frameworks/base/services/core/java/com/android/server/wm/WindowManagerInternal.java
grep -n -F 'InputChannel.openInputChannelPair("drag");' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'new DragInputEventReceiver(mClientChannel,' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragWindowHandle.layoutParamsFlags = 0;' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragWindowHandle.touchableRegion.setEmpty();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDisplayContent.getDisplayRotation().pause();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.syncInputWindows();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.apply(true);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragState.broadcastDragStartedLocked(touchX, touchY);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'transaction.show(surfaceControl);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'callingWin.scheduleAnimation();' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.notifyLocationLocked(touchX, touchY);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mTransaction.setPosition(mSurfaceControl, x - mThumbOffsetX, y - mThumbOffsetY).apply();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
```

画出两个失败切面：register 前失败和 transfer 返回 `false`。分别写出此时 STARTED、`mDragInProgress`、rotation、两个 channel 与两个 Surface 的状态。

## 6. STARTED：窗口通知资格不是 View 兴趣

`broadcastDragStartedLocked()` 固定起点、缓存 `ClipDescription`、清空通知列表、置 `mDragInProgress=true`，并缓存源 user 的 cross-profile 限制。它遍历发起 `DisplayContent` 的全部窗口，而非只看手指下方窗口。

一个窗口先要满足 `isPotentialDragTarget()`：当前可见、未 removed，并且同时有 `InputChannel` 与 `InputWindowHandle`。这里不检查 `FLAG_NOT_TOUCHABLE`，所以某窗口可以收到 STARTED；若该 flag 到最终命中时仍保留，它就不能通过 DROP 的 touchable hit-test。

然后进入 local/global 分支：

| flags 与目标 | 是否可继续 |
|---|---|
| 非 GLOBAL，目标就是源 `IWindow` | 是 |
| 非 GLOBAL，同进程另一顶层 `IWindow` | 否 |
| GLOBAL，目标有 `ActivityRecord` 且 targetSdk ≥ N | 是 |
| GLOBAL，目标的 `mActivityRecord == null` | 是 |
| GLOBAL，pre-N App 的其他窗口 | 否 |
| GLOBAL，pre-N App 恰好就是源 `IWindow` | 是，退回 local binder 相等分支 |

最后还要过 profile 门：若源 user 在开始时受 `DISALLOW_CROSS_PROFILE_COPY_PASTE` 限制，只允许相同 userId。这个布尔值只在广播开始时取一次快照，拖动中途的限制变化不会重新计算。

`IWindow.dispatchDragEvent()` 属于 oneway。调用未立即抛 `RemoteException` 后，WMS 就把窗口加入 `mNotifiedWindows`；这表示 Binder 事务已发出，不表示 App 主线程已经处理，更不表示某个 View 返回 `true`。

拖动期间新出现或变为可见的窗口也可补收 STARTED：InputMonitor 更新窗口时调用 `sendDragStartedIfNeededLocked()`。r48 这条迟到通知路径还额外限制在 default display；整个 `DragState` 的命中也固定在发起 `DisplayContent`，不要把它解释成成熟的跨显示屏拖放。

### 练习 3：手算 STARTED 的窗口资格矩阵

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oneway interface IWindow {' frameworks/base/core/java/android/view/IWindow.aidl
grep -n -F 'void dispatchDragEvent(in DragEvent event);' frameworks/base/core/java/android/view/IWindow.aidl
grep -n -F 'mCrossProfileCopyAllowed = !userManager.getUserRestriction(' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDisplayContent.forAllWindows(w -> {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (mDragInProgress && isValidDropTarget(newWin)) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mNotifiedWindows.add(newWin);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (!targetWin.isPotentialDragTarget()) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if ((mFlags & View.DRAG_FLAG_GLOBAL) == 0 || !targetWindowSupportsGlobalDrag(targetWin)) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (mLocalWin != targetWin.mClient.asBinder()) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'return mCrossProfileCopyAllowed ||' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'return targetWin.mActivityRecord == null' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F '|| targetWin.mActivityRecord.mTargetSdk >= Build.VERSION_CODES.N;' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'return isVisibleNow() && !mRemoved' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '&& mInputChannel != null && mInputWindowHandle != null;' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'if (mInDrag && isVisible && w.getDisplayContent().isDefaultDisplay) {' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mService.mDragDropController.sendDragStartedIfNeededLocked(w);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
```

沿源码分支列出 local/global、源窗口/其他窗口、M/N target、`mActivityRecord==null`、同 user/跨 user 的有效组合，不把四个二元维度机械压成八组。特别解释为什么“GLOBAL 排除全部 pre-N 窗口”比源码更强。

## 7. Motion 控制链：DOWN 被忽略，CANCEL 却会尝试 DROP

`DragInputEventReceiver` 在 WMS Handler Looper 上处理迁移后的输入。非 `MotionEvent`、非 pointer source 或已 `mMuteInput` 的事件直接返回，但 finally 仍调用 `finishInputEvent(event, handled)`。

动作矩阵如下：

| Motion action | receiver 行为 | WMS 行为 |
|---|---|---|
| `DOWN` | 记日志后返回 | 不移动、不 DROP |
| `MOVE` | 通常继续；若检测到 stylus 主按钮释放则 mute | `notifyMoveLocked()` 或最终 `notifyDropLocked()` |
| `UP` | mute | 最终重新命中并尝试 DROP |
| `CANCEL` | mute | 同样尝试 DROP，不直接走 cancel animation |
| `POINTER_DOWN/POINTER_UP` 等 | default 返回 | 只 finish，本次不移动也不结束 |

这里的 `CANCEL` 语义反直觉：`handleMotionEvent(false, x, y)` 与 UP 共用 `notifyDropLocked()`。如果最终位置命中合格窗口，仍会尝试发 `ACTION_DROP`；命中失败、DROP 派发立即抛错、目标返回 false 或 5 秒未回报都会进入失败结束。

stylus 分支还有一个跨章边界。receiver 在 switch 前用“第一条收到的事件”记录主按钮是否按下；正常 transfer 给全新的 drag Connection 合成的第一条通常是 `DOWN`，而 r48 的补 DOWN 把 `buttonState` 写成中性值 `0`。因此典型路径会把 `mStylusButtonDownAtStart` 记为 false，不能仅凭 receiver 中的按钮释放分支断言该功能在每次 transfer 后都会触发。这是结合第 238 章合成字段得出的实现推论。

`getRawX()/getRawY()` 提供 WMS 命中的显示坐标。多指的 POINTER 动作没有选择某个拖拽 pointer 的逻辑；后续 MOVE 使用事件的 raw 坐标，不能把这段实现描述成按显式 pointerId 跟踪。

### 练习 4：构造 receiver 动作表并验证 stylus 起始快照

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!(event instanceof MotionEvent)' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F '|| (event.getSource() & SOURCE_CLASS_POINTER) == 0' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'if (mIsStartEvent) {' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'mStylusButtonDownAtStart = isStylusButtonDown;' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'case ACTION_DOWN:' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'case ACTION_MOVE:' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'if (mStylusButtonDownAtStart && !isStylusButtonDown) {' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'case ACTION_UP:' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'case ACTION_CANCEL:' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'mDragDropController.handleMotionEvent(!mMuteInput /* keepHandling */, newX, newY);' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'finishInputEvent(event, handled);' frameworks/base/services/core/java/com/android/server/wm/DragInputEventReceiver.java
grep -n -F 'synthesizePointerDownEventsForConnectionLocked(toConnection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '0 /*buttonState*/, MotionClassification::NONE,' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

分别推导 synthetic DOWN→MOVE、DOWN→CANCEL、POINTER_DOWN→MOVE 三条路径。说明每条 Motion 的 `handled`、`mMuteInput`、是否移动 Surface、是否进入 DROP。

## 8. WMS 窗口命中：LOCATION 与 DROP 使用两次选择

`notifyMoveLocked()` 先更新 `mCurrentX/Y`，用一次 Surface transaction 移动拖影，再调用 `notifyLocationLocked()`。拖影视觉移动不依赖 App 是否处理 LOCATION。

LOCATION 的窗口级算法是：

```text
DisplayContent.getTouchableWinAtPointLocked(x, y)
→ 若该窗口不在 mNotifiedWindows，把命中视为空
→ 与旧 mTargetWindow 不同：向旧窗口发 EXITED
→ 新目标非空：向新窗口发 LOCATION
→ 无论 Binder 分发是否抛错，最后更新 mTargetWindow
```

WMS 的 touchable hit-test 要求窗口可见、没有 `FLAG_NOT_TOUCHABLE`、点在 visible bounds 内；点在 `touchableRegion` 内，或者窗口同时没有 `FLAG_NOT_FOCUSABLE/FLAG_NOT_TOUCH_MODAL` 时也可命中。后一个 touch-modal 分支解释了为什么 Region 不是唯一边界。

旧目标的 EXITED 以 `(0,0)` 作为传给 `obtainDragEvent()` 的占位输入，但 helper 仍调用 `translateToWindowX/Y()`；最终数值未必是零，而且 `DragEvent` 契约本就规定 EXITED 坐标无效。正确说法是“不可读取”，而不是“固定为零”。

EXITED 与 LOCATION 包在同一个 `try` 中：若给旧窗口发 EXITED 立即抛 `RemoteException`，本次不会继续给新窗口发 LOCATION，但 `mTargetWindow` 仍改成新窗口。WMS 没有为这类 oneway 投递缺口回滚目标账。

UP/CANCEL 到来时，`notifyDropLocked()` 不信任上一帧 `mTargetWindow`，而是按最终坐标重新调用同一个窗口 hit-test，再检查 `mNotifiedWindows`。它不会先调用 `notifyLocationLocked()`，也不会依据旧 `mTargetWindow` 补窗口 EXITED：UP 若跨到另一窗口，新窗口可以直接收到 DROP，旧窗口随后只等 ENDED。无有效窗口时不发 DROP，也不设置 5 秒回报消息，只把结果置 false 并开始失败结束。

### 练习 5：画出跨两个窗口的命中与投递

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'void notifyMoveLocked(float x, float y) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.setPosition(mSurfaceControl, x - mThumbOffsetX, y - mThumbOffsetY).apply();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'WindowState touchedWin = mDisplayContent.getTouchableWinAtPointLocked(x, y);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (touchedWin != null && !isWindowNotified(touchedWin)) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'DragEvent.ACTION_DRAG_EXITED,' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'DragEvent.ACTION_DRAG_LOCATION,' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTargetWindow = touchedWin;' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'final WindowState touchedWin = mDisplayContent.getTouchableWinAtPointLocked(x, y);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (!isWindowNotified(touchedWin)) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'final DragEvent evt = obtainDragEvent(touchedWin, DragEvent.ACTION_DROP, x, y,' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'final float winX = win.translateToWindowX(x);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if ((flags & FLAG_NOT_TOUCHABLE) != 0) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'return mTmpRegion.contains(x, y) || touchFlags == 0;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

设 A、B 已通知，C 是覆盖其上的未通知窗口。推导 A→B、B→C、最终在 A DROP 三步的 `mTargetWindow`、EXITED/LOCATION/DROP；再加入“给旧窗口发 EXITED 立即失败”的分支。

## 9. View 树：STARTED 返回值只建立客户端兴趣

`ViewRootImpl.W` 收到 oneway `DragEvent` 后把它投递到 UI Handler。LOCATION 使用独立消息号，并在入队前移除尚未处理的旧 LOCATION，因此高频 MOVE 可以逐帧移动服务端 Surface，而 App 只观察到合并后的较新位置。

LOCATION/DROP 的有效坐标会连续变换：receiver 的 raw display 坐标先由 `WindowState.translateToWindowX/Y()` 转成窗口坐标，ViewRoot 再应用 compatibility translator 与 `mCurScrollY`，每层 ViewGroup 最后变换到目标 child 的局部坐标。STARTED 也有有效 x/y，但 ViewRoot 不走 compatibility/scroll 分支；ViewGroup 仍会在 `notifyChildOfDragStart()` 中用 `transformPointToViewLocal()` 逐层换成 child 局部坐标。因此 STARTED 与后续 LOCATION/DROP 不能笼统写成同一条变换链。

Handler 在进入 `handleDragEvent()` 前，把本 ViewRoot 的 `mLocalDragState` 写入事件。STARTED 缓存 `ClipDescription`；后续 LOCATION、ENTERED、EXITED、DROP 由客户端补回这份 description，ENDED 则清空。

STARTED 在 `ViewGroup` 内递归发给当时可见的 children 和容器自身。返回 `true` 的 child 进入 `mChildrenInterestedInDrag` 并带上 `PFLAG2_DRAG_CAN_ACCEPT`。拖动中新增或刚变为 visible 的 child，可利用缓存的 STARTED 补做一次兴趣判断。

LOCATION/DROP 到来时，每层 `ViewGroup` 按 children 数组从末项向首项寻找：

```text
曾接受 STARTED
+ 变换后的坐标落在 child 内
→ 数组逆序遇到的第一个 droppable child
```

这个 helper 没有采用 touch 分发的 Z/custom drawing order 预排序，也不重新检查 visibility；已接受 STARTED 后才变为 invisible 的 child 仍可能命中。因此方法名里的 frontmost 不能扩大成严格的视觉最上层。找不到 child 而容器自身接受过 STARTED 时，容器可成为目标。移除的 View 不再位于树中，即使它早先收过 STARTED，也不保证能收到 ENDED。

N 及以后，最终 `View.dispatchDragEvent(LOCATION/DROP)` 在调用业务 handler 前执行 `ViewRootImpl.setDragFocus()`：旧 View 收 EXITED、新 View 收 ENTERED，二者坐标设为无效占位且 `ClipData=null`。pre-N 进程使用 `sCascadedDragDrop`，由 `ViewGroup` 维持整条父子包含链的 enter/exit 兼容状态。

`OnDragListener` 在 View enabled 时先执行；若返回 `true`，不再调用 `onDragEvent()`，否则继续调用后者。STARTED 的 `true` 建立兴趣，DROP 的实际目标返回值成为最终 consumed，其他 action 的返回值不承担最终业务结果。N 及以后，命中 child 只要调用过 handler，父 ViewGroup 就不再 fallback，即使 child 返回 false；pre-N 兼容分支才用 child 的 boolean 决定是否继续交给父级，不能把结果描述成父子返回值简单 OR。

在非直接窗口 EXITED 的普通分发分支中，dispatch 前后的 `mCurrentDragView` 若改变，ViewRoot 会调用 `dragRecipientExited/Entered(IWindow)`；r48 AOSP 服务端方法只做可选日志，不用它重算 WMS 的窗口级目标。WMS 直接发来的 `ACTION_DRAG_EXITED` 走另一分支，只执行 `setDragFocus(null, event)`，不会调用这两个 Session 方法。窗口目标与 View focus 是两层独立状态。

### 练习 6：证明窗口资格与 View 兴趣不回传

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (event.getAction() == DragEvent.ACTION_DRAG_LOCATION) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mHandler.removeMessages(what);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.mLocalState = mLocalDragState;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mDragDescription = event.mClipDescription;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.mClipDescription = mDragDescription;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if ((what == DragEvent.ACTION_DRAG_LOCATION) || (what == DragEvent.ACTION_DROP)) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.mClipData.prepareToEnterProcess();' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'boolean result = mView.dispatchDragEvent(event);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mWindowSession.reportDropResult(mWindow, result);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'public void setDragFocus(View newDragTarget, DragEvent event) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.mAction = DragEvent.ACTION_DRAG_EXITED;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.mAction = DragEvent.ACTION_DRAG_ENTERED;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'transformPointToViewLocal(point, child);' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'mChildrenInterestedInDrag = new HashSet<View>();' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'if (notifyChildOfDragStart(children[i])) {' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'View target = findFrontmostDroppableChildAt(event.mX, event.mY, localPoint);' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'if (!child.canAcceptDrag()) {' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'li.mOnDragListener.onDrag(this, event)) {' frameworks/base/core/java/android/view/View.java
grep -n -F 'result = onDragEvent(event);' frameworks/base/core/java/android/view/View.java
```

以兴趣集合在 DROP 前保持为空的典型 `ViewGroup` 根为例，证明 WMS 为什么仍可能继续向该 `IWindow` 发 LOCATION/DROP，以及最后的 `reportDropResult(false)` 从哪里产生；再说明普通叶子 View 作为根时为何不能仅凭 WMS 代码推导同一结果。

## 10. DROP：数据只给最终窗口，description 由客户端补回

WMS 的跨进程事件内容并不相同：

| action | WMS 携带的内容 | ViewRoot 补充/处理 |
|---|---|---|
| STARTED | 窗口局部 x/y、`ClipDescription` | 缓存 description，注入本 root 的 localState |
| LOCATION | 窗口局部 x/y | 补 description，可能被消息合并 |
| 窗口级 EXITED | 坐标无效，其他内容空 | 清 View focus，并由客户端补 description |
| DROP | 窗口局部 x/y、完整 `ClipData`、可选 permission Binder | `prepareToEnterProcess()`，补 description，再分发 View 树 |
| ENDED | 最终 result；失败时源 PID 窗口另带当前显示坐标 | 清客户端状态；description 为空 |

`notifyDropLocked()` 在 dispatch 前取得最终窗口的 owning user/package。若 source user 与 target user 不同，它对 `mData.fixUris(mSourceUserId)`，把数据里的 URI 标出来源 user；这发生在 DROP，不发生在 STARTED。

DROP 发给的是进程边界上的最终窗口，而不是“最终返回 true 的 View”才取得数据。目标 View 可以读取数据、请求 URI grant，然后仍返回 false；WMS 的 `mDragResult` 与权限生命周期没有自动绑定。

`ViewRootImpl` 在 UI 线程同步分发整个 View 树，拿到根返回值后通过同步的 `IWindowSession.reportDropResult(mWindow, result)` 回到 WMS。`true` 只表示这次 View 处理声明消费，不证明文件落盘、数据库提交、远端上传或 URI 权限持久化。

## 11. URI 能力：permission Binder 不是已经授权

WMS 创建 `DragAndDropPermissionsHandler` 必须同时满足：

```text
DRAG_FLAG_GLOBAL
+ READ 或 WRITE 至少一位
+ ClipData 非空
```

`PERSISTABLE` 与 `PREFIX` 只加入 mode，本身不能触发 handler。构造器递归收集 item URI、Intent data 与嵌套 ClipData URI，但不检查列表是否为空；因此 permission Binder 非空甚至不能证明 `ClipData` 真含 URI。

handler 在跨 user 的 `fixUris()` 之前收集原始 URI，同时保存独立的 `sourceUserId/targetUserId`。真正 grant 时，服务端显式传入：

```text
permissionOwner
sourceUid
targetPackage
Uri + mode
sourceUserId + targetUserId
```

目标不能借当前 Binder identity 把任意 URI 伪装成源 App 数据。handler 在调用 `UriGrantsManager` 前清 identity，但授权来源仍由 performDrag 时保存的 `sourceUid` 限定。

获得 permission Binder 后还要显式 take：

- `Activity.requestDragAndDropPermissions(event)` 取得包装并调用 `take(activityToken)`；ATMS 找到 Activity 对应的 permission owner，Activity 销毁或显式 `release()` 时撤销。
- 隐藏的 transient 路径创建名为 `drop` 的 owner，并对客户端提供的 token `linkToDeath()`；显式 release 或该 token 死亡时逐 URI revoke。

两种 take 共用一次性 guard；已有 Activity token 或 permission owner 时，后续 take 直接返回。AIDL 方法返回 `void`，客户端包装只以是否抛 `RemoteException` 判断 boolean，因此重复 take 的 `true` 不代表新建了第二组 grant。

逐 URI grant 是循环调用，没有批量事务或显式回滚。更重要的是，`closeLocked()` 不持有这个 handler，也不自动 revoke：目标在 DROP 内 take 后即使返回 false、超时或收到 ENDED，grant 仍按 Activity owner、transient token 或显式 release 的生命周期处理。

### 练习 7：闭合 Activity 与 transient 两条 URI 生命周期

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if ((mFlags & View.DRAG_FLAG_GLOBAL) != 0 && (mFlags & DRAG_FLAGS_URI_ACCESS) != 0' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'dragAndDropPermissions = new DragAndDropPermissionsHandler(' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mData.fixUris(mSourceUserId);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'clipData.collectUris(mUris);' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'if (mActivityToken != null || mPermissionOwnerToken != null) {' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'getUriPermissionOwnerForActivity(mActivityToken);' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'long origId = Binder.clearCallingIdentity();' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'UriGrantsManager.getService().grantUriPermissionFromOwner(' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'mPermissionOwnerToken = LocalServices.getService(UriGrantsManagerInternal.class)' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F '.newUriPermissionOwner("drop");' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'mTransientToken.linkToDeath(this, 0);' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'mTransientToken.unlinkToDeath(this, 0);' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'ugm.revokeUriPermissionFromOwner(permissionOwner, mUris.get(i), mMode, mSourceUserId);' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'public void binderDied() {' frameworks/base/services/core/java/com/android/server/wm/DragAndDropPermissionsHandler.java
grep -n -F 'DragAndDropPermissions dragAndDropPermissions = DragAndDropPermissions.obtain(event);' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'dragAndDropPermissions.take(getActivityToken())' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'mDragAndDropPermissions.takeTransient(mTransientToken);' frameworks/base/core/java/android/view/DragAndDropPermissions.java
```

分别画出 owner 创建、逐 URI grant、显式 release、Activity 销毁和 transient token 死亡。再回答：目标 take 后返回 `false`，哪段源码会自动 revoke？

## 12. DROP 回报：一个 token 字段的两段协议

`DragState.mToken` 在 r48 分两阶段使用：

```text
DROP 之前：performDrag 返回给源 App 的 dragToken
            → 只允许源窗口 cancelDragAndDrop(dragToken)

DROP 发出后：最终目标 IWindow.asBinder()
             → 只允许该窗口 reportDropResult(window, consumed)
```

成功调用 `dispatchDragEvent(DROP)` 后，WMS 为目标 token 安排 5000 ms 的 `MSG_DRAG_END_TIMEOUT`，然后把 `mToken` 改为目标窗口 token。计时从服务端发出 oneway DROP 后开始，包括目标 Binder 排队和 UI Handler 等待；它不等同于 InputDispatcher 的 publish/finish ANR。

正确目标回报时，控制器先比较参数 `IWindow.asBinder()` 与当前目标 token，再移除 timeout，然后重新用 `windowForClientLocked()` 查当前 `WindowState`。这里有一个明确的 r48 断点：若 token 正确但窗口已经移除，代码在取消 timeout 后直接 return，没有调用 `endDragLocked()`。默认路径会留下仍 active 的 `DragState`、已 mute 的 receiver、Surface 与 rotation pause，也失去 5 秒兜底。

若窗口仍在，`consumed=true` 直接 close；`false` 启动返回动画。错误窗口回报抛 `IllegalStateException`。已无 DragState 的迟到回报只记录并返回。

timeout 处理本身把结果置 false 并调用 `endDragLocked()`，并未走普通输入 ANR 责任链；源码注释仍把对目标 App 做 ANR 归责列为未完成事项。5 秒到达只是开始失败动画，逻辑关闭还要等动画结束。

还有一个窄竞态：timeout 已启动返回动画但 `closeLocked()` 尚未执行时，正确目标的迟到回报仍可把 `mDragResult` 改为 true；`endDragLocked()` 因 `mAnimator != null` 返回，最终 ENDED 可能携带 true，却已经播放了失败返回动画。迟到时点落在 close 之后则因 `mDragState==null` 被忽略。

源 App 的公开 `View.cancelDragAndDrop()` 总是传 `skipAnimation=false`，并在调用后清自己的 token。DROP 已发出时 WMS 的 `mToken` 已换成目标窗口 token，原 dragToken 会校验失败，服务端继续等待 DROP 回报或 timeout。

### 练习 8：推演 token、timeout 与窗口消失竞态

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mDragDropController.sendTimeoutMessage(MSG_DRAG_END_TIMEOUT, token);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mToken = token;' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'private static final long DRAG_TIMEOUT_MS = 5000;' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (mDragState.mToken != token) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mHandler.removeMessages(MSG_DRAG_END_TIMEOUT, window.asBinder());' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'WindowState callingWin = mService.windowForClientLocked(null, window, false);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (callingWin == null) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.mDragResult = consumed;' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.endDragLocked();' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mHandler.sendMessageDelayed(msg, DRAG_TIMEOUT_MS);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'case MSG_DRAG_END_TIMEOUT: {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.mDragResult = false;' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (mDragState.mToken != dragToken) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.cancelDragLocked(skipAnimation);' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mAttachInfo.mSession.cancelDragAndDrop(mAttachInfo.mDragToken, false);' frameworks/base/core/java/android/view/View.java
grep -n -F 'mAttachInfo.mDragToken = null;' frameworks/base/core/java/android/view/View.java
```

画出四条时间线：正常 true、正常 false、正确 token 但 WindowState 已消失、timeout 后动画结束前迟到 true。标出每条线的 `mToken`、timeout 是否存在、`mDragResult` 与 `mDragState`。

## 13. 结束：返回动画、取消动画、ENDED 与异步 teardown

业务结果与动画分三路：

| 入口 | 动画 | 终点 |
|---|---|---|
| DROP consumed=true | 无 | 直接 `closeLocked()` |
| 无目标、DROP false、5 秒 timeout、发送 DROP 立即失败 | return animation | 从当前位置回原点，alpha 降到一半，再 close |
| 仍 in-progress 且 `skipAnimation=false` 的显式 cancel | cancel animation | 在当前位置缩放到 0、alpha 到 0，再 close |
| `skipAnimation=true` 或 cancel 时尚未 in-progress | 无 | 直接 close |

公开 View API 不暴露 `skipAnimation=true`。两类动画在 AnimationThread 更新 Surface，结束后发 `MSG_ANIMATION_END` 回 WMS Handler，再在全局锁内 close。

`closeLocked()` 的逻辑顺序是：

```text
mIsClosing = true
投递 drag InputChannel teardown，立即清 mInputInterceptor 字段
向 mNotifiedWindows 中每个窗口发 ENDED(result)
恢复 mouse pointer icon
移除 input Surface
把 drag Surface reparent 到 null
清 ClipData/token/flags/list
DragDropController.mDragState = null
```

ENDED 面向 WMS 已通知窗口，不只面向 DROP 窗口；死亡 Binder 仍可能让某次发送失败。窗口内部则由 ViewGroup 转发给仍在兴趣集合、仍在树中的 View。结果失败时，WMS 为 `ws.mSession.mPid == 源 pid` 的已通知窗口填入 `mCurrentX/Y`，为其他进程窗口填零；这不是只按源 `IWindow` 判断。WMS 对 ENDED 直接 `DragEvent.obtain()`，没有做窗口坐标翻译，因此这组失败坐标是实现私有信息，不应当作普通 ENDED 的有效局部坐标。

InputChannel 并未在 `closeLocked()` 内立即 dispose。Handler 稍后 unregister server channel、dispose receiver 和两端 channel，并恢复 rotation；这是 InputEventReceiver 必须在所属 Looper 线程销毁的约束。逻辑 `mDragState=null`、channel fd 销毁、Surface transaction apply、硬件 present 是不同完成点。

仍满足 `mView != null && mAdded` 的 ViewRoot 正常处理 ENDED 时，会清 `mDragDescription`、`mCurrentDragView`、localState、dragToken，并 release 本地 `Surface` 包装。若根已 detach，`handleDragEvent()` 只 recycle 事件，不走这段客户端清理；服务端仍会解除自己的 `SurfaceControl` parent 并拆 input 资源。两边动作次序与底层对象何时真正消失不能从单次 release 推导。

### 练习 9：给结束链的每个资源标完成点

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!mDragResult) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mAnimator = createReturnAnimationLocked();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mAnimator = createCancelAnimationLocked();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'void closeLocked() {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'MSG_TEAR_DOWN_DRAG_AND_DROP_INPUT, mInputInterceptor);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'for (WindowState ws : mNotifiedWindows) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'if (!mDragResult && (ws.mSession.mPid == mPid)) {' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'DragEvent.ACTION_DRAG_ENDED,' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.remove(mInputSurface).apply();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.reparent(mSurfaceControl, null).apply();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragDropController.onDragStateClosedLocked(this);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mService.mInputManager.unregisterInputChannel(mServerChannel);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mInputEventReceiver.dispose();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDisplayContent.getDisplayRotation().resume();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'case MSG_ANIMATION_END: {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'setLocalDragState(null);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mAttachInfo.mDragSurface.release();' frameworks/base/core/java/android/view/ViewRootImpl.java
```

把“ENDED 已发出、WMS 状态已清、channel 已 dispose、App Surface 已 release、transaction 已 apply、画面已 present”排成偏序，而不是强行写成一条全局同步时间线。

## 14. 三条完整时间线：成功、空目标与丢失窗口

第一条：跨窗口成功，目标在 UI 线程返回 true。

```text
源 App：画 Surface → performDrag 阻塞 → 收到 dragToken
WMS：register/sync/transfer → STARTED → queue shadow show/reparent → 初始 LOCATION
输入：synthetic DOWN(忽略) → 首个 MOVE 提交 shadow transaction → MOVE* → UP
目标：DROP(data, permission Binder) → 可选 take → handler 返回 true
WMS：report token 匹配 → 移除 5 秒消息 → close → ENDED(true)
异步：App release Surface；WMS teardown channel / resume rotation
```

第二条：最终点没有已通知窗口。

```text
UP/CANCEL
→ 最终 hit 为空或命中未通知窗口
→ 不发 DROP，不创建 5 秒消息
→ mDragResult=false
→ return animation
→ close
→ ENDED(false)
```

第三条：DROP 已发出，目标在回报前被 WMS 移除。

```text
DROP oneway 已排队 + 5 秒消息已挂起 + mToken=目标 IWindow
→ 目标以正确 token report
→ WMS 先 remove timeout
→ windowForClientLocked() 返回 null
→ 直接返回，未 end/close
→ receiver 已 mute，DragState 仍 active
```

第三条不是正常契约，而是 r48 的恢复缺口。诊断时如果看到“新 drag 一直报已有 drag、旧影或 rotation 状态不收敛”，不能只检查 5 秒消息是否触发，还要检查正确回报之后的 WindowState 查找。

此外，四条顺序不能拼成全局完成序：

- STARTED/LOCATION/DROP/ENDED 是 `IWindow` oneway，服务端调用返回不等于 UI 线程处理。
- 同一 ViewRoot 会合并尚未处理的 LOCATION。
- `reportDropResult()` 是目标 UI 分发后的同步反向调用。
- Surface transaction apply 与 display present 不是同一步。

## 15. 调试时按账本找断点

遇到“拖影出现但不能放”“目标没收到数据”或“结束后仍卡住”，按下面顺序收窄：

| 观察 | 查哪本账 | r48 的关键问题 |
|---|---|---|
| `startDragAndDrop()` 返回 false | App/WMS 启动 | attach、Surface、metrics、已有 drag、callingWin、display、transfer |
| 有 dragToken 但无移动 | 输入控制 | drag handle 是否同步、transfer 是否弱成功、receiver 是否只收到被忽略动作 |
| 窗口没有 STARTED | 窗口通知 | potential target、local/global、pre-N fallback、profile、display |
| 窗口有 STARTED 但 View 无 LOCATION | View 兴趣 | STARTED 是否返回 true、View 是否仍在树中、坐标变换与 LOCATION 合并 |
| DROP 没发 | 最终窗口门 | 最终 hit 是否 touchable、是否在 `mNotifiedWindows` |
| DROP 有 data 但 URI 仍拒绝 | 数据能力 | 是否 GLOBAL+READ/WRITE、是否 take、source/target user 与 package |
| 目标返回后仍不结束 | 结果账 | token、timeout、WindowState 消失分支、是否已处于 animation |
| ENDED 后 channel/rotation 稍晚恢复 | 资源账 | teardown Handler 是否执行，而非只看 `mDragState` |

建议把日志和断点对齐到这些对象，而不是只搜 action 名：

```text
DragState.mNotifiedWindows / mTargetWindow / mToken / mDragResult / mAnimator
ViewRootImpl.mLocalDragState / mCurrentDragView / mDragDescription
ViewGroup.mChildrenInterestedInDrag / mCurrentDragChild
DragAndDropPermissionsHandler 的 owner/token/URI 列表
DragDropController 的 timeout 与 teardown 消息
```

还要区分五个经常被混写的完成点：

1. `startDragAndDrop()==true`：WMS 启动路径返回。
2. `finishInputEvent()`：一条 drag Motion 已由 receiver 结束处理。
3. `reportDropResult()`：目标 View 的 consumed 决定到达 WMS。
4. `closeLocked()`：WMS 逻辑状态清理并发出结束 transaction/通知。
5. teardown、App release 与硬件 present：各在线程或进程中继续完成。

## 16. r48 边界与本章结论

把本章压成一句话：

```text
系统拖放 = 迁移当前 pointer 控制权
         + 向合格窗口建立 DragEvent 会话
         + 在客户端 View 树维护兴趣与 enter/exit
         + 只向最终窗口交付数据和可请求的 URI 能力
         + 用目标窗口 token、结果消息、动画与异步资源清理收敛
```

阅读 Android 11 r48 时，必须保留这些版本边界：

- 同时只有一个 `DragState`；默认流程固定在发起 `DisplayContent`，迟到窗口通知还限定 default display。
- 启动层没有再次证明 pointer 仍属于发起窗口；input transfer 本身又存在弱成功。
- GLOBAL 对其他 pre-N App 窗口受限，但源 `IWindow` 仍可经 local fallback 收到事件。
- WMS 的通知窗口与客户端的感兴趣 View 是两本账，STARTED 的 boolean 不回传 WMS。
- 初始 LOCATION 由 `performDrag()` 主动触发；后续 LOCATION 可在 ViewRoot 合并。
- `performDrag()` 只排队拖影 show/reparent，首个常规 MOVE 的 `.apply()` 才提交该独立 transaction。
- receiver 忽略 DOWN 与 POINTER 动作，却把 CANCEL 当作最终 DROP 尝试；stylus 起始按钮还受 synthetic DOWN 中性字段影响。
- permission Binder 只提供 take 能力，grant 与 drag result/ENDED 生命周期分离。
- 5 秒等的是 DROP result，不是输入 ANR；正确 token 后窗口消失会出现取消 timeout 却未结束状态的缺口。
- `mToken` 在 DROP 前后换角色；源 token 不能在 DROP 后继续取消。
- ENDED、逻辑 close、channel teardown、Surface apply 与画面 present 没有单一全局完成点。

下一章进入第 240 章：追 `WindowState` 怎样生成 `InputWindowInfo`，并经 `SurfaceControl.Transaction`、input window 同帧同步与 InputDispatcher 窗口快照把几何、层级、Region、transform 和 channel token 交到 native 输入路由。
