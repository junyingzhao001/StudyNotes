# 242 Android 系统手势排除区域、边缘限制与导航手势仲裁链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一次 exclusion 请求最终能阻止哪次返回手势

一个贴着屏幕左边缘的抽屉和系统“侧滑返回”会争用同一串 pointer。`View.setSystemGestureExclusionRects()` 提供的不是“这块区域归 App 所有”，而是一份有条件的优先级请求：只有 Rect 经客户端可见性映射、WMS 触摸能力与 Z 序裁剪、每侧边缘预算筛选后，才会成为 SystemUI 在下一次 `ACTION_DOWN` 读取的 actual exclusion Region。

本章追问一件事：App 提交的一组 View 局部 Rect，究竟在哪些条件下能阻止某次边缘返回候选；若没有阻止，SystemUI 又到哪个 MOVE 才请求接管原窗口的触摸流？

读完应能分开回答五个完成点：

```text
View 已保存请求
ViewRoot 已把窗口坐标 List 放入 oneway Binder
WMS 已更新该 Display 的 actual/unrestricted Region
SystemUI 主线程已换成本地 Region
本次手势已越阈值；若 native pilfer 返回 OK，当前 windows/portal 路由已清、monitor 保留，只有满足合成条件的旧连接才排入 CANCEL
```

任意前一项都不自动证明后一项。第 241 章已经解释 `getEffectiveTouchableRegion()` 的四种 touchable-insets、modal、Task 与 tap-exclude 几何；本章只把它当作 WMS 聚合的输入，不重走那条链。第 243 章才讨论遮挡安全与 tapjacking。

## 2. 两条时间线：Region 发布链与单次触摸仲裁链

排除区发布和某一次触摸是两条并行时间线：

```text
发布链
View/Window 局部 Rect
→ GestureExclusionTracker 映射成窗口 List
→ IWindowSession oneway
→ WindowState 请求缓存
→ DisplayContent actual/unrestricted
→ ISystemGestureExclusionListener oneway
→ SystemUI main executor 更新本地 Region

触摸链
InputDispatcher 同时送给正常窗口目标与 gesture monitor
→ SystemUI 只在 ACTION_DOWN 读取当时的本地 Region
→ 阈值前继续旁观
→ 横向意图成立后请求 pilfer
→ native 返回 OK 时清窗口路由、保留 monitor，并只对可合成的旧连接排 CANCEL
→ 插件稍后决定 triggerBack 或 cancelBack
```

所以“刚调用 API，紧接着从边缘按下”天然存在代际问题。该 DOWN 读到旧 Region 还是新 Region，取决于客户端 Handler、两次 Binder、WMS 全局锁、SystemUI main executor 与输入消息的实际排队顺序。r48 没有把一次 App 请求与某个未来 pointer id 绑定成事务。

几何口径也有四层：View post-layout 局部、窗口局部 List、Display 全局 Region、SystemUI 当前缓存。调试时必须同时记录坐标系与时间，不能只比较两个形似的矩形数值。

## 3. API 契约：只放松冲突手势，不创造触摸权

`setSystemGestureExclusionRects()` 的文档说系统“可以选择”放松自身手势识别。它不是以下任何承诺：

- 不给窗口增加 InputDispatcher 命中范围；
- 不保证每种系统级手势都服从请求；
- 不保证整个请求都会通过边缘预算；
- 不保证 API 返回后的当前手势立刻采用新结果。

三个 Insets 口径要分开：

| 口径 | 回答的问题 | exclusion 能否覆盖 |
|---|---|---|
| `systemGestures()` | 哪些边缘连续手势可能由系统优先处理 | 普通部分可请求让出优先级 |
| `mandatorySystemGestures()` | 哪些系统手势必须保持优先 | 不能由该 API 覆盖 |
| `tappableElement()` | 简单点击应避开哪些持久系统元素 | 与连续边缘手势不是同一问题 |

WMS 的 `calculateSystemGestureExclusion()` 并没有一行通用的“actual 减 mandatory Region”。mandatory 是 API 与具体系统手势消费者必须遵守的策略边界；在本章的 Edge Back 消费者里，底部手势区检查先于 exclusion。不能把它误写成客户端 Rect 在 WMS 中固定经过的第四次求交。

同理，actual Region 命中只让 SystemUI 放弃这次 Back 候选。App 仍须本来就是正常窗口目标，窗口内部也仍由 ViewGroup 决定哪个 child 接收事件。

## 4. View 声明生命周期：引用、清空与位置监听

View API 的 Rect 是 post-layout 局部坐标。需要精细拖动的 thumb 可以声明自己的局部小范围，不应先加窗口位置。文档建议在 `onLayout()` 或 `onDraw()` 更新，因为尺寸、父层裁剪和位置通常到那时才稳定。

r48 直接把调用者的 `List<Rect>` 引用保存进 `ListenerInfo`，getter 也返回同一引用；它没有逐个深拷贝。调用后原地修改 List 或其中 Rect 会绕过正常的更新消息，所以文档明确要求不要再改。

非空声明会给 RenderNode 注册 `PositionUpdateListener`。position changed/lost 回调可能来自 HWUI worker，它只向 View Handler 队首 post，再由 View 调用 ViewRoot。Handler 为 null 时这次 post 不发生。

清空有一个只属于 r48 实现的尖角：代码移除了 RenderNode listener，却没有把 `mPositionUpdateListener` 字段置 null。随后同一 View 再设非空 List，setter 会主动 post 一次当前结果，但因为字段仍非空，不会重新 add listener；再往后的纯 RenderNode 位置变化可能失去这条通知来源。

### 练习 1：推演一次清空再启用的 listener 状态

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public void setSystemGestureExclusionRects(@NonNull List<Rect> rects) {' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (rects.isEmpty() && mListenerInfo == null) return;' frameworks/base/core/java/android/view/View.java
grep -n -F 'info.mSystemGestureExclusionRects = rects;' frameworks/base/core/java/android/view/View.java
grep -n -F 'mRenderNode.removePositionUpdateListener(info.mPositionUpdateListener);' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (info.mPositionUpdateListener == null) {' frameworks/base/core/java/android/view/View.java
grep -n -F 'mRenderNode.addPositionUpdateListener(info.mPositionUpdateListener);' frameworks/base/core/java/android/view/View.java
grep -n -F 'h.postAtFrontOfQueue(this::updateSystemGestureExclusionRects);' frameworks/base/core/java/android/view/View.java
grep -n -F 'ai.mViewRootImpl.updateSystemGestureExclusionRectsForView(this);' frameworks/base/core/java/android/view/View.java
grep -n -F 'return list;' frameworks/base/core/java/android/view/View.java
```

从一个已 attach 到 ViewRoot、`getHandler()!=null`、但尚未创建 ListenerInfo 的 View 开始，依次执行：①传非空 List `L1`；②不再调用 setter，只把 `L1[0]` 原地改掉；③传空 List；④传新的非空 `L2`；⑤只改变 RenderNode 位置。逐步写出保存的 List、setter 是否向 Handler 成功入队、position listener 是否实际注册，并说明第④步为什么只能触发一次显式更新、第⑤步为何可能没有位置回调。不得假定 setter 会复制 List。

## 5. GestureExclusionTracker：可见映射与 r48 比较缺口

Tracker 用 `WeakReference<View>` 保存来源，不应仅因 exclusion 声明延长 View 生命周期。扫描时，View 已回收、未 attach 或 `isAggregatedVisible()==false` 都返回 GONE；父 View 隐藏因此能让子声明退出最终 List。

对每个局部 Rect，Tracker 先复制，再调用父节点的 `getChildVisibleRect(excludedView, mappedRect, null)`。父链会处理位置、scroll、矩阵与可见裁剪；返回 false 的 Rect 被丢弃，部分可见的 Rect 只保留映射后的窗口部分。这是客户端裁剪，后面的 WMS touchable/unhandled 交集是另一层。

r48 的变化比较存在明确缺口：函数已经算出窗口坐标 `newRects`，却用旧的映射结果 `mExclusionRects` 去比较新的 View 局部 `localRects`。若上轮映射值刚好等于局部值，例如 View 原先位于窗口原点，本轮只移动位置，比较会提前返回 UNCHANGED，刚算出的新映射不会写回。反过来，只要旧映射长期不等于 local，它又会反复走 CHANGED，最后仍可能被总 List 相等检查挡住上报。

字段 `mDirty` 在此版本只被初始化和赋 true，没有被 `update()` 读取；不能把它讲成一次可靠的增量重算门。位置监听发消息，不等于 Tracker 必然接纳本轮 `newRects`。

### 练习 2：算可见映射并复现一次漏更新

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'private final WeakReference<View> mView;' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'if (v == null || !v.isAttachedToWindow() || !v.isAggregatedVisible()) {' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'final List<Rect> localRects = excludedView.getSystemGestureExclusionRects();' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'final List<Rect> newRects = new ArrayList<>(localRects.size());' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'Rect mappedRect = new Rect(src);' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'if (p != null && p.getChildVisibleRect(excludedView, mappedRect, null)) {' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'newRects.add(mappedRect);' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'if (mExclusionRects.equals(localRects)) return UNCHANGED;' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'mExclusionRects = newRects;' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'mDirty' frameworks/base/core/java/android/view/GestureExclusionTracker.java
```

Round 1 中，identity View 位于窗口 `(0,0)`、无裁剪，局部 Rect 为 `[0,0,100,80]`，先完成一次 compute。Round 2 的父链几何改为 View 净平移到 `(30,25)`，并给出祖先窗口坐标 clip `[50,40,110,90]`，局部 List 不变。先算 Round 2 的 `newRects=[50,40,110,90]`，再按源码比较左右两边，说明为何返回 UNCHANGED 并继续保留旧 `[0,0,100,80]`。最后令 aggregated visibility=false，说明下一次全扫描为何会移除此 View。

## 6. Window 根 Rect 与 View Rect 汇成窗口 List

`Window.setSystemGestureExclusionRects()` 面向 `takeSurface()` 等没有普通 View 层级的场景。PhoneWindow 把它交给 ViewRoot 的 root List；这个 List 也是按引用保存，不替换各 View 的声明。

`computeChangedRects()` 的结果顺序是 root Rect 先进入新 ArrayList，再按 Tracker 中 ViewInfo 顺序追加各 View 的映射 Rect。这里仍是 List，不会先合并重叠项为 Region。根列表变化或某 View 被判 CHANGED/GONE 会进入结果比较；只有最终 List 与上次不同，函数才返回非 null。

ViewRoot 随后做两件并列的事：把同一个窗口坐标 List 交给 `IWindowSession`，并调用 App 内 `OnSystemGestureExclusionRectsChangedListener`。这个本地回调看到的是“已变换、准备上报的窗口 List”，不是 WMS 的 actual，也看不到 Z 序、边缘预算或 mandatory 消费策略。

### 练习 3：区分窗口 List 与服务端 actual

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'getViewRootImpl().setRootSystemGestureExclusionRects(rects);' frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java
grep -n -F 'mGestureExclusionTracker.setRootSystemGestureExclusionRects(rects);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mRootGestureExclusionRects = rects;' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'final List<Rect> rects = new ArrayList<>(mRootGestureExclusionRects);' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'rects.addAll(info.mExclusionRects);' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'if (!mGestureExclusionRects.equals(rects)) {' frameworks/base/core/java/android/view/GestureExclusionTracker.java
grep -n -F 'mWindowSession.reportSystemGestureExclusionChanged(mWindow, rectsForWindowManager);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F '.dispatchOnSystemGestureExclusionRectsChanged(rectsForWindowManager);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'listener to add' frameworks/base/core/java/android/view/ViewTreeObserver.java
```

上次总缓存依次为 root R=`[0,0,20,50]`、已稳定映射的 View A=`[0,100,10,130]`、View B=`[100,100,120,140]`。本轮 R 与 A 不变，B 在 Tracker entry 尚未移除时刚 detach，且其余来源为空。写出 compute 返回 List 的顺序、本地 listener 所见内容及 B 的去向；再列出仅凭这次本地 callback 仍无法判断的三项服务端事实。若调用者随后原地修改 root List 却不再次调用 Window setter，说明为什么不能保证产生新消息。

## 7. oneway 上报：客户端返回究竟证明到哪里

`IWindowSession.reportSystemGestureExclusionChanged()` 是 oneway。Session 只清理 calling identity 后转发；真正用 `windowForClientLocked(session, window, true)` 校验窗口属于该 Session 的是 WMS。找到 WindowState 后，相同 List 不触发聚合；变化时服务端清旧项、保存新项，再调用 DisplayContent。

DisplayContent 若没有任何系统 listener，会直接返回而不计算 actual；请求仍已保存在 WindowState。首个 listener 注册时再计算整份 Display。若有 listener，WMS 在全局锁内算出缓存 Region，并通过另一个 oneway 接口通知；SystemUI 的 Binder stub 又 post 到 main executor 后才 `set()` 本地两份 Region。

WindowState List 相等只抑制这次 App RPC 直接触发的重算，不把 actual 冻结。surface placement、Insets 状态变化、导航手势设置变化、Display metrics/density 更新，以及 DeviceConfig 的 limit 或 pre-Q 开关变化，都有独立入口再次调用 `updateSystemGestureExclusion()`；所以 App List 不变时，窗口几何、Z 序、edge frame 或预算变化仍可改写结果。

因此客户端 API/oneway proxy 返回最多说明调用或 Binder 提交没有同步抛错，不证明 WMS 已存值；WMS 发出 callback 不证明 SystemUI main 已落表；SystemUI 落表也只影响之后处理的 DOWN。App 内 ViewTreeObserver 回调与服务端确认没有因果回执关系。

### 练习 4：给五个时刻标完成含义

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oneway void reportSystemGestureExclusionChanged(IWindow window, in List<Rect> exclusionRects);' frameworks/base/core/java/android/view/IWindowSession.aidl
grep -n -F 'mService.reportSystemGestureExclusionChanged(this, window, exclusionRects);' frameworks/base/services/core/java/com/android/server/wm/Session.java
grep -n -F 'final WindowState win = windowForClientLocked(session, window, true);' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'if (win.setSystemGestureExclusion(exclusionRects)) {' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'if (mExclusionRects.equals(exclusionRects)) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mExclusionRects.addAll(exclusionRects);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'if (mSystemGestureExclusionListeners.getRegisteredCallbackCount() == 0) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'mSystemGestureExclusion.set(systemGestureExclusion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'dc.updateSystemGestureExclusion();' frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
grep -n -F 'mDisplayContent.updateSystemGestureExclusion();' frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java
grep -n -F 'mDisplayContent.updateSystemGestureExclusion();' frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java
grep -n -F 'updateSystemGestureExclusionLimit();' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'mUpdateSystemGestureExclusionCallback.run();' frameworks/base/services/core/java/com/android/server/wm/WindowManagerConstants.java
grep -n -F 'oneway interface ISystemGestureExclusionListener {' frameworks/base/core/java/android/view/ISystemGestureExclusionListener.aidl
grep -n -F 'mMainExecutor.execute(() -> {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mExcludeRegion.set(systemGestureExclusion);' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
```

固定时刻：t1=View setter 返回，t2=ViewRoot 的 oneway proxy 返回，t3=WMS 已保存 WindowState List，t4=WMS 已更新 actual 并调用 listener，t5=SystemUI main executor 已执行 Region `set()`。逐项写出能证明与不能证明的下一层。再分别推演“t3 时无 listener”和“新 List 与 WindowState 旧 List 相等”：哪些缓存会更新，哪些不会由这次 RPC 重算？最后列出上述五类非 App-report 重算入口，说明它们各自能改变哪种聚合输入。

## 8. DisplayContent 的 Z 序账本：effective 与 unhandled

System gesture 是 Display 级竞争：多个窗口会重叠，只有 DisplayContent 拥有完整 Z 序。聚合从 `unhandled=整屏` 开始，窗口按 top-to-bottom 遍历。`cantReceiveTouchInput()`、不可见、`FLAG_NOT_TOUCHABLE` 窗口被跳过；可处理窗口先取得第 241 章定义的 effective touchable Region，再与当前 unhandled 求交。这里的 effective 是本聚合算法采用的候选 touchable 口径，不能跳过 SurfaceFlinger/InputDispatcher 链，直接把它叫作最终生产命中 Region。

这里的 unhandled 不是“还未处理 exclusion 的区域”，而是“在本算法里还未被更上层候选 touchable Region 占用的 Display 区域”。当前窗口有没有声明 exclusion 都不影响它随后从 unhandled 中减去已经与 unhandled 求交的本窗口 touchable Region。因此一个不声明排除区的顶层窗口，也能阻止更低窗口借被覆盖位置影响系统手势。

modal effective Region 可以大到 Display/Task 策略范围。顶层 modal 即使 List 为空，也可能让后层请求全部失去暴露部分。相反，不可触摸 overlay 被资格门跳过，不从 unhandled 扣除；这与初始触摸穿过它的行为一致。安全遮挡标志是下一章的另一条账。

## 9. 窗口 List 变成 Display Region：scale、translate、intersect

普通显式请求先用 `rectListToRegion()` 合并；重叠 Rect 到此才成为规范化 Region。随后按 `mGlobalScale` 缩放、加 `mFrame.left/top`，最后与“effective touchable ∩ unhandled”求交：

```text
local = Region(window-coordinate List)
displayRequest = translate(scale(local, mGlobalScale), frameOrigin)
exposedRequest = displayRequest ∩ effectiveTouchable ∩ unhandled
```

所以 unrestricted 也不是 App 原始 List。它已经丢掉 View 不可见部分、窗口触摸范围外部分和被高层窗口占住的部分；“unrestricted”只表示后续没有施加每侧边缘高度预算。

pre-Q sticky immersive 是另一种 local 来源。需同时满足 HIDE_NAVIGATION+IMMERSIVE_STICKY、DeviceConfig 兼容开关开启、存在 ActivityRecord 且 targetSdk<Q，WMS 才用当前暴露 touchable Region 替代显式 List。它仍要经过后面的 restriction 判定与 Display 聚合，不能从兼容条件直接推出任意设备上最终 actual 必为整窗。

### 练习 5：手算 Z 序遮挡与坐标变换

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'unhandled.set(0, 0, mDisplayFrames.mDisplayWidth, mDisplayFrames.mDisplayHeight);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'forAllWindows(w -> {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (w.cantReceiveTouchInput() || !w.isVisible()' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'w.getEffectiveTouchableRegion(touchableRegion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'touchableRegion.op(unhandled, Op.INTERSECT);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'rectListToRegion(w.getSystemGestureExclusion(), local);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'local.scale(w.mGlobalScale);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'final Rect frame = w.getWindowFrames().mFrame;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'local.translate(frame.left, frame.top);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'local.op(touchableRegion, Op.INTERSECT);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'unhandled.op(touchableRegion, Op.DIFFERENCE);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (w.isImplicitlyExcludingAllSystemGestures()) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

Display 为 `[0,0,400,800]`。顶窗 W1 合格、scale=1、frame 原点 `(0,0)`、effective touchable=`[0,0,400,200]`、显式 List=`[0,50,20,180]`；下窗 W2 合格、scale=2、frame 原点 `(0,100)`、effective touchable=`[0,100,400,500]`、局部 List=`[0,10,10,100]`。忽略边缘额度，按 Z 序写出两窗 exposed request 与每步 unhandled；验证 W2 的 Display 请求先变为 `[0,120,20,300]`，再只剩 `[0,200,20,300]`。最后把 W1 改成 effective 覆盖整屏且 List 为空，说明 W2 为何仍无贡献。

## 10. 左右边缘预算：来源、单位与反向消费

左右 edge 不是硬编码的固定宽度，WMS 从 `ITYPE_LEFT_GESTURES`、`ITYPE_RIGHT_GESTURES` InsetsSource frame 读取。高度预算则由 DeviceConfig 的 dp 整数决定，r48 用 `Math.max(200, configured)` 保证至少 200dp，再按每个 Display 的 `densityDpi/160` 做整数换算。

`remainingLeftRight={limit,limit}` 说明左右各有一份完整预算；它们由同一 Display 上受限窗口按 Z 序共享，不是每个 Rect、Window 或进程各有一份。高 Z 窗口先消费，即使它的 Rect 靠上，也会先于低 Z 窗口的底部 Rect；bottom-to-top 只描述同一窗口、同一侧 Region 内部的迭代。请求 Region 完全处于两条 edge 之外的 middle 会直接 union，不消费这两份预算。

每一侧先让源码变量 `local`（此时已经是 Display 坐标）与 edge 求交，再用 `forEachRectReverse()` 按 bottom-to-top、同层 right-to-left 消费规范化 Region 的 Rect。若一个 Rect 高于剩余额度，只保留它底部的剩余高度。这里收费单位是 Region 迭代所得每个 Rect 的 `height()`：不是面积，也不是调用者 List 的简单高度和；两个水平分离但 Y 投影相同的岛会各收一次高度，因此形状和 Region 分解可间接影响消耗。

r48 还有一个必须与最终几何分账的记账尖角。部分截断时，代码虽然把 `rect.top` 改到正确位置，却仍执行 `remaining -= 原始 height`，所以 remaining 可以为负；`grantedExclusion=limit-remaining` 也可能大于真正 union 进 actual 的高度。最终 Region 裁剪仍正确，但 `remaining` 与 WindowState 的 requested/granted 日志字段不能代替 Region 实测长度。

### 练习 6：重算截断、负 remaining 与水平双岛

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'private static final int MIN_GESTURE_EXCLUSION_LIMIT_DP = 200;' frameworks/base/services/core/java/com/android/server/wm/WindowManagerConstants.java
grep -n -F 'mSystemGestureExclusionLimitDp = Math.max(MIN_GESTURE_EXCLUSION_LIMIT_DP,' frameworks/base/services/core/java/com/android/server/wm/WindowManagerConstants.java
grep -n -F '* mDisplayMetrics.densityDpi / DENSITY_DEFAULT;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'getSourceProvider(ITYPE_LEFT_GESTURES)' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'getSourceProvider(ITYPE_RIGHT_GESTURES)' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F '{mSystemGestureExclusionLimit, mSystemGestureExclusionLimit};' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'Order is bottom to top, then right to left.' frameworks/base/services/core/java/com/android/server/wm/utils/RegionUtils.java
grep -n -F 'Collections.reverse(rects);' frameworks/base/services/core/java/com/android/server/wm/utils/RegionUtils.java
grep -n -F 'forEachRectReverse(r, rect -> {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (remaining[0] <= 0) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'final int height = rect.height();' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'rect.top = rect.bottom - remaining[0];' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'remaining[0] -= height;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'final int grantedExclusion = limit - remaining[0];' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'win.setLastExclusionHeights(side, requestedExclusion[0], grantedExclusion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

先固定 densityDpi=320、configured limit=120dp，算出实际 limit=400px；再把 configured 改为300dp重算。随后另取 fresh 左预算200px、edge=`[0,0,40,1000]`，规范化 Rect 从上到下为 A=`[0,0,40,100]`、B=`[0,150,40,240]`、C=`[0,300,40,380]`。按反向顺序写出 actual 片段、每步 remaining、`requestedExclusion` 与源码计算的 `grantedExclusion`；区分 actual 总高度200与最终 remaining=-70、granted=270。

再用 fresh 预算100px和两个水平分离 Rect `[0,400,10,450]`、`[20,400,30,450]`，说明 Y 投影虽只有50px，源码为何消费100px。最后指出右侧预算为何不受这些左侧运算影响。

## 11. 限额与豁免：跳过预算不等于获得触摸

`needsGestureExclusionRestrictions()` 的输出由窗口身份和当前导航栏请求共同决定：

| 情况 | 边缘高度预算 | 仍受哪些上游/下游边界 |
|---|---|---|
| 普通 App | 应用 | View 可见、effective touchable、Z 序、mandatory/消费者 |
| nav 请求不可见且 behavior 为 transient-by-swipe | 跳过 | 仍须 touchable 且未被高层占住 |
| `TYPE_INPUT_METHOD` | 跳过 | 仍须 touchable；Edge Back 底部门仍先执行 |
| `TYPE_NOTIFICATION_SHADE` | 跳过 | 系统窗口身份，不是普通 App 能伪造的类型 |
| HOME Activity | 跳过 | 仍在 Display 聚合与具体消费者策略内 |

IME 的实现会按 `systemGestures()` 左右 inset，从 `visibleTopInsets` 到 root 底部声明两条 Rect；这解释了它为什么需要边缘操作空间，但不把 mandatory 区变成可排除。

pre-Q implicit exclusion 与“跳过预算”也是两个谓词。前者决定 local 取显式 List 还是整个暴露 touchable；后者由 requested Insets visibility/behavior、IME/shade/Home 决定。常见 sticky immersive 路径会同时满足，但推理时仍要分别列条件。

### 练习 7：完成六类窗口的条件矩阵

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'final boolean stickyHideNav =' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F '&& win.mAttrs.insetsFlags.behavior == BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'return (!stickyHideNav || ignoreRequest) && type != TYPE_INPUT_METHOD' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F '&& type != TYPE_NOTIFICATION_SHADE && win.getActivityType() != ACTIVITY_TYPE_HOME;' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'final int immersiveStickyFlags =' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'return immersiveSticky && mWmService.mConstants.mSystemGestureExcludedByPreQStickyImmersive' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '&& mActivityRecord != null && mActivityRecord.mTargetSdk < Build.VERSION_CODES.Q;' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'local.set(touchableRegion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'rootView.getRootWindowInsets().getInsetsIgnoringVisibility(Type.systemGestures());' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'rootView.setSystemGestureExclusionRects(exclusionRects);' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'system gestures cannot be overriden by' frameworks/base/core/java/android/view/WindowInsets.java
```

固定其余资格门均通过，分别判断：A 普通 target R App、导航栏可见；B target R App、requested nav 不可见且 behavior=transient-by-swipe；C `TYPE_INPUT_METHOD`；D `TYPE_NOTIFICATION_SHADE`；E HOME Activity；F target P Activity，legacy HIDE_NAVIGATION+IMMERSIVE_STICKY、兼容开关=true，并固定 requested nav 不可见、behavior=transient-by-swipe。对每行写出 `needsRestrictions(false)`，再只对 F 判断是否 `isImplicitlyExcludingAllSystemGestures()` 以及是否读取显式 List。最后把 F 的兼容开关改 false，说明哪一个谓词变化。

## 12. actual 与 unrestricted：名字不等于通知语义

每个窗口的 exposedRequest（此时已在 Display 坐标）都 union 到 `outExclusionUnrestricted`；actual 则对受限窗口先消费左右预算，对豁免窗口直接 union。unrestricted 因此是“经过客户端可见映射、窗口触摸范围和 Z 序之后，但未施加边缘预算”的 Region，不足以证明某个原始 App Rect 的完整内容。

`calculateSystemGestureExclusion()` 的注释把 boolean 描述成 actual 与 unrestricted 是否不同，但 r48 实现返回“左或右 remaining 是否小于初值”。只要普通受限窗口在 edge 消费过正高度，即使完全落在预算内、两份 Region 相等，boolean 也为 true；若该轮因 actual 改变而广播，或注册路径补发当前缓存，listener 收到的 unrestricted 仍是非 null。

更新通知还有更窄的门：函数先重算 unrestricted 与 restricted flag，随后只比较新旧 actual；actual 相同便直接返回，不广播。因此只改变已被预算拒绝的请求，可以更新 WMS 的 unrestricted 缓存，却让已有 listener 保留旧副本。首个 listener 注册会触发计算；若计算没有自行广播，注册路径再单独回调当前缓存，避免新 listener 永远拿不到初值。

SystemUI 收到 null unrestricted 时把它复制为 actual；非 null 也不自动代表两份不同。对同一次 callback 携带的两份 Region，可用 `unrestricted - actual` 判断该代际被预算拒绝的几何，不能拿参数是否为 null 当差集；但 actual-equality 早退可能使已有 listener 的 unrestricted 缓存落后于 WMS 当前值。

### 练习 8：构造 actual 不变而 unrestricted 变化

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mSystemGestureExclusionWasRestricted = calculateSystemGestureExclusion(' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (mSystemGestureExclusion.equals(systemGestureExclusion)) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'final Region unrestrictedOrNull = mSystemGestureExclusionWasRestricted' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F '.onSystemGestureExclusionChanged(mDisplayId, systemGestureExclusion,' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'outExclusionUnrestricted.op(local, Op.UNION);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'return remainingLeftRight[0] < mSystemGestureExclusionLimit' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'mSystemGestureExclusionListeners.register(listener);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (mSystemGestureExclusionListeners.getRegisteredCallbackCount() == 1) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (!changed) {' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'mUnrestrictedExcludeRegion.set(unrestrictedOrNull != null' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
```

固定 Display=`[0,0,400,300]`、leftEdge=`[0,0,20,300]`、rightEdge=`[380,0,400,300]`，只有一个普通受限窗口，左预算100px且其余裁剪为空；首次前 actual 为空且已有 listener。第一次请求仅含底部 `[0,100,20,200]`，第二次在保持它不变的同时新增更靠上的 `[0,0,20,50]`。按 bottom-to-top 分别算两轮 actual、unrestricted、remaining 与 boolean；说明第二轮为何 actual 不变却 unrestricted 增大，以及已有 listener 是否收到第二次广播。再加入一个全新的第二 listener，推演注册路径为何会把当前缓存直接给它。最后另起 fresh 场景，旧 actual 为空，请求只有 middle Rect `[100,0,120,50]`；说明 boolean 为何是 false、两份内部 Region 为何相同，以及 actual 改变引发的 callback 为何把 unrestricted 参数置 null。

## 13. SystemUI 收表并在 DOWN 依次过门

`EdgeBackGestureHandler` 只有在导航栏已 attach 且当前为 gestural mode 时启用。启用会注册 WMS listener、建立 `monitorGestureInput("edge-swipe", displayId)`、创建主 Looper 的 InputEventReceiver 和边缘插件；禁用则释放这些对象。WMS callback 在 Binder 线程到达后再交给 main executor，输入事件也在主 Looper 消费，最终由两类消息的先后顺序决定某个 DOWN 读哪一版 Region。

DOWN 的门序不能压成一次 contains：

1. quickstep rotation、强制导航键、blocking Activity 与 SysUI flags 先做全局准入；
2. `isWithinTouchRegion()` 先拒绝底部 gesture height；
3. 离两侧超过两倍 margin 的点连统计候选都不是；
4. X 落入 left/right edge width 才有 withinRange，ML 只可能收紧较外侧部分，不会把 range 外点扩进来；
5. transient navbar 显示时在 actual 检查之前返回 withinRange；
6. 非 transient 才用 actual 拒绝 Back，并用 unrestricted contains 记录 rejected-exclusion 统计分类。

这解释了三个反直觉结果：actual 内的底部点先被 bottom 门拒绝，不记成 exclusion 命中；actual 内但本来不在 edge range 的点不能借 exclusion 获得 App 触摸权；`unrestricted contains && !actual contains` 只说明 SystemUI 当前缓存的未限额 Region 覆盖该点，可能来自显式或 pre-Q implicit 路径，也可能因 actual-equality 早退而落后于 WMS 当前缓存，不足以单独归因某个 View。

## 14. 阈值前取消、阈值后 pilfer：触摸流何时换主

DOWN 通过只让插件开始观察，SystemUI 不立刻抢流。正常窗口仍按 InputDispatcher 的 TouchState 收到初期事件，gesture monitor 同时收到副本。阈值前：

- `ACTION_POINTER_DOWN` 取消当前 Back 候选；
- 只有 MOVE 到来并发现 `eventTime-downTime > mLongPressTimeout` 才触发长按取消，没有独立定时器在 250ms 正点执行；
- `dy>dx && dy>touchSlop` 取消纵向手势；
- `dx>dy && dx>touchSlop` 才先置 `mThresholdCrossed=true`，再调用 `pilferPointers()`。

这些分支只在 `!mThresholdCrossed` 时运行；多指若发生在越阈值之后，EdgeBackGestureHandler 不再执行本段的 pre-threshold 取消逻辑。该 POINTER_DOWN 能否再次到达 monitor 与插件，还取决于下述 Dispatcher 分支。

`InputMonitor.pilferPointers()` 又不是同步成功凭证。它调用的 `IInputMonitorHost` 是 oneway；native 可能因 monitor token 未注册、Display 没有 TouchState、monitor 不在本流或流已不 down 返回 `BAD_VALUE`，Java 调用者拿不到这个结果。monitor 有效且正在观察同一 down stream 时，native 才返回 OK；它逐个尝试向旧窗口连接合成 pointer CANCEL，随后无条件用 `filterNonMonitors()` 清空当前 `state.windows` 与 portalWindows，gesture monitors 留下。

“尝试”不能省略：旧 target 缺 InputChannel、Connection 已不存在或 BROKEN、或者 connection inputState 没有可合成的 pointer cancellation 时，都不会排入 CANCEL，但 windows/portal 仍被清掉。因此 native OK 证明路由账已切换，不证明每个旧 target 已排入或收到 CANCEL。

这也只是 pilfer 完成瞬间的状态。`filterNonMonitors()` 保留 `down`、`split`、device/source/display 标识和 gesture monitors；若原 TouchState 已是 split，之后同 device/source/display 的非 mouse `ACTION_POINTER_DOWN` 仍会进入 Dispatcher 的 Case 1。若它命中支持 split、未 paused、Connection 存在且 responsive 的新窗口，且事件不是软件注入或注入权限已通过，新 pointer 才会加入该窗口 target；保留的 monitor 也收到事件，EdgeBackGestureHandler 因阈值已过而把它转给插件。若找不到合格窗口，本次 `isDown=false` 令 `newGestureMonitors` 为空，Case 1 的 no-target 门会丢弃整个 POINTER_DOWN，插件也收不到。两种分支都不会恢复旧 pointer 的原窗口 target。

## 15. triggerBack 与完成点：接管不等于 Back 已消费

pilfer 成功仍不等于系统已经执行返回。当前越阈值 MOVE 随后还会转给 edge plugin；插件根据后续运动/UP 决定调用 `triggerBack()` 或 `cancelBack()`。trigger 分支构造 `KEYCODE_BACK` DOWN/UP，两次都用 `INJECT_INPUT_EVENT_MODE_ASYNC`，且返回值被忽略。

完整完成点应这样读：

| 观察 | 最多证明 |
|---|---|
| App setter 返回 | View 已执行保存/post 逻辑 |
| ViewRoot oneway 返回 | 请求已提交 Binder，不证明 WMS 已算 |
| WMS callback 发出 | WMS actual 缓存已换，不证明 SystemUI main 已换 |
| SystemUI `mExcludeRegion.set` | 未来 DOWN 可读新值，不倒改已判定流 |
| `mThresholdCrossed=true` | 本地状态先翻转，pilfer 仍可能失败 |
| native pilfer 返回 OK | 当前 windows/portal 已清且 monitors 保留；仅合格旧连接可能排入 CANCEL；后续 split POINTER_DOWN 可能加入新窗口，无合格窗口则会被丢弃 |
| plugin `triggerBack()` | 已发起两次异步 Key 注入，不证明目标处理或 Activity 退出 |

`cancelBack()` 只结束 SystemUI/plugin 候选与记录，不会把已经成功 pilfer 的旧窗口 TouchState 自动复原。相反，阈值前的多指、长按或纵向取消没有 pilfer，正常 App 流可继续。

### 练习 9：从 DOWN 门推到 pilfer 与异步 Back

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'boolean isEnabled = mIsAttached && mIsGesturalModeEnabled;' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F '.registerSystemGestureExclusionListener(' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mInputMonitor = InputManager.getInstance().monitorGestureInput(' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (y >= (mDisplaySize.y - mBottomGestureHeight)) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (x > 2 * (mEdgeWidthLeft + mLeftInset)' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'boolean withinRange = x < mEdgeWidthLeft + mLeftInset' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (mIsNavBarShownTransiently) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (mExcludeRegion.contains(x, y)) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mInRejectedExclusion = mUnrestrictedExcludeRegion.contains(x, y);' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mAllowGesture = !mDisabledForQuickstep && mIsBackGestureAllowed' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (action == MotionEvent.ACTION_POINTER_DOWN) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F '(ev.getEventTime() - ev.getDownTime()) > mLongPressTimeout' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'if (dy > dx && dy > mTouchSlop) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F '} else if (dx > dy && dx > mTouchSlop) {' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mThresholdCrossed = true;' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'mInputMonitor.pilferPointers();' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'oneway interface IInputMonitorHost {' frameworks/base/core/java/android/view/IInputMonitorHost.aidl
grep -n -F 'std::optional<int32_t> foundDisplayId = findGestureMonitorDisplayByTokenLocked(token);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!foundDeviceId || !state.down) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizeCancelationEventsForInputChannelLocked(channel, options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (channel != nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection == nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->status == Connection::STATUS_BROKEN) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (cancelationEvents.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.filterNonMonitors();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::vector<TouchedMonitor> newGestureMonitors = isDown' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newTouchedWindowHandle != nullptr && newTouchedWindowHandle->getInfo()->paused) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newTouchedWindowHandle == nullptr && newGestureMonitors.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!checkInjectionPermission(touchedWindow.windowHandle, entry.injectionState)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void TouchState::filterNonMonitors() {' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'sendEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK);' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
grep -n -F 'InputManager.getInstance().injectInputEvent(ev, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);' frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/EdgeBackGestureHandler.java
```

固定 display=`1080×2400`、bottom height=200、左右 edge width=50、insets=0、ML=false、transient=false，其余全局门都放行；actual=`[0,200,50,400]`，unrestricted 还额外含 `[0,500,50,800]`。分别判断 P1=`(20,250)`、P2=`(20,600)`、P3=`(20,2250)`、P4=`(60,600)`、P5=`(1060,600)` 的 withinRange、actual 拒绝、rejected-exclusion 标记与 Back 候选结果。再令 transient=true，仅重算 P1，指出哪一步被绕过。

从允许的 P2 开始，固定 touchSlop=12、longPressTimeout=250ms：100ms MOVE `(dx,dy)=(8,2)`，150ms MOVE `(20,5)`。说明哪一步调用 pilfer。再固定 monitor 有效、同一流仍 down、native 返回 OK，并给四个旧 target：W1 有 Channel、Connection 非 BROKEN 且 inputState 可合成 pointer cancellation；W2 缺 Channel；W3 的 Connection 为 BROKEN；W4 的 inputState 无可合成事件。分别写出谁会排入 CANCEL，以及 filter 后 windows/portal 与 monitors 的状态。

另固定 pilfer 前 TouchState `split=true`，随后发生同 device/source/display 的非 mouse、新硬件 POINTER_DOWN（`entry.injectionState=null`）：先令触点命中支持 split、未 paused、已连接且 responsive 的 W5，再令其没有任何合格窗口，分别说明窗口 target、旧 monitor、插件和该事件的结果；两支都要解释为什么旧 pointer 的原 target 不会恢复。最后分别用阈值前 POINTER_DOWN、251ms 才到来的 MOVE、MOVE `(5,20)` 替换第二个 MOVE，说明为何三者都不 pilfer。即使插件随后调用 triggerBack，也要指出两次 ASYNC inject 尚不能证明什么。

## 16. r48 结论、诊断顺序与下一章

这条链可以压缩成九个判断：

1. exclusion 只请求系统放松冲突手势，不创造窗口或 View 的触摸权。
2. View Rect 是 post-layout 局部坐标；root Rect 是窗口坐标，两者由 Tracker 汇成窗口 List。
3. View 可见映射与 r48 mapped/local 比较缺口，都发生在 Binder 之前。
4. oneway 上报、本地 ViewTreeObserver callback、WMS 聚合和 SystemUI 落表是四个不同完成点。
5. WMS 用本聚合口径的 effective touchable 与 top-to-bottom unhandled 先确定每个窗口在本算法中的暴露请求；这不替代 SF/Dispatcher 最终命中事实。
6. 左右边缘各有一份按 Display 共享的纵向 Rect-height 预算；middle 与豁免窗口跳过该预算，但不跳过触摸/Z 序/mandatory 边界。
7. unrestricted 已经过可见性、坐标、touchable 和 Z 序裁剪，只是未过边缘预算；非 null 不保证 actual 与它不同。
8. SystemUI 只在 DOWN 用当时缓存判候选；bottom、edge range、transient 与 actual 的先后顺序会改变结果。
9. 横向越阈值只发起 oneway pilfer；native OK 会清窗口路由，却只有可合成的旧连接才排入 CANCEL。pilfer 后的 split POINTER_DOWN 可能加入新合格窗口；没有合格窗口便在 Case 1 被丢弃，两支都不恢复旧 pointer target。插件发起 ASYNC Back 注入仍不是消费或界面退出确认。

排查时依次取证：View/root 原始 List，Tracker 窗口 List，WindowState `mExclusionRects`，DisplayContent actual/unrestricted 与左右 edge/limit，SystemUI 本地两份 Region和 DOWN 点，最后才看 threshold、pilfer 的 native 条件、App CANCEL 与 Back 消费结果。只看 App setter 返回或一份 SystemUI dump，无法跨越整条异步链。

下一章进入 `WINDOW_IS_OBSCURED`、`WINDOW_IS_PARTIALLY_OBSCURED`、trusted overlay 与 `filterTouchesWhenObscured`：解释某点即使通过普通 Z 序命中，为什么仍可能因上层不可信窗口而被标记或拒绝，以及它与本章“系统手势优先级请求”的边界。
