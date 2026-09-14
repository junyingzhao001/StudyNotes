# 241 Android 触摸 Region、Touchable Insets、Task 裁剪与 Surface 坐标变换链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一块局部 Region 怎样完成正向命中与坐标回程

第 240 章已经把输入窗口表从 `WindowState` 追到 InputDispatcher。本章只放大其中的几何闭环：

```text
一块由 App 在窗口局部坐标声明的可触摸区域，
怎样经 WMS 的 frame 与策略运算、SurfaceFlinger 的 Layer 几何变成屏幕命中区；
命中以后，屏幕上的 pointer 坐标又怎样变回 App 看到的局部 getX()/getY()？
```

这不是“把一个矩形平移两次”那么简单。沿途至少有四类对象：

| 对象 | 能表达什么 | 不能偷换成什么 |
|---|---|---|
| `Rect` | 一个轴对齐、右下边界不包含的整数矩形 | 任意曲线或逐像素蒙版 |
| `Region` | 多个轴对齐矩形的并、交、差，可不连续、可有洞 | 单一外接矩形 |
| window / Layer frame | 某一阶段的坐标原点与外接范围 | 永远不变的“窗口位置” |
| transform | Layer 局部到屏幕的几何映射 | r48 对 Region 已完整应用的任意矩阵 |

主线只讨论 Android 11 r48 的普通 pointer 窗口路径。公开的 system gesture exclusion 是第 242 章的主题；本章只解释名字相近但用途不同的 WMS tap-exclude。旋转、翻转、portal、clone 和零缩放会在相应边界处单列，不能拿纯轴向缩放算例替代源码行为。

读完后，应能对一个具体点同时回答三件事：它是否落在 Dispatcher 的最终 Region 中，目标保存了什么 offset/scale，以及客户端的 `getRawX()` 与 `getX()` 各从哪份值计算。

## 2. 先建坐标账：Region 正向链与触点坐标回程

把 App 声明的形状记为 `Rapp`，把一次普通触点记为 `p`。一条完整账本如下：

| 阶段 | 主要载体 | 坐标口径 | 关键动作 |
|---|---|---|---|
| App 声明 | `InternalInsetsInfo` | App 窗口局部 | 选择 FRAME/CONTENT/VISIBLE/REGION |
| 跨 Binder 前 | `Rect` / `Region` 参数 | 仍相对窗口；compat translator 可能先改单位尺度 | 只在值变化或 insets pending 时发送 |
| WMS 保存 | `mGiven*`、`mTouchableInsets` | WindowState 使用的尺度，Region 仍相对 frame | 必要时乘 `mGlobalScale` |
| WMS 策略 | 临时 `Region` | Display 全局 | nonmodal 才按 frame 解释 App 声明；modal 改用系统范围，再做 Task/tap-exclude 运算 |
| Surface 事务 | `InputWindowHandle.touchableRegion` | 关联 Surface 的局部 | 减 `mFrame.left/top`，特定 size-compat 再逆缩放 |
| SF 输出 | native `InputWindowInfo` | Display 屏幕 | Layer 轴向 scale、最终 frame 平移、crop/replace/clone |
| Dispatcher 目标 | `InputTarget` / `DispatchEntry` | 屏幕命中结果加回程参数 | 先 contains，再保存 final frame 负偏移与 window scale |
| App 事件 | `PointerCoords` 加 `mXScale/mYScale/mXOffset/mYOffset` | 保存坐标与窗口局部读数并存 | raw 读保存值，local 再应用 scale/offset |

因此“局部”至少有两种：App 监听器填写的逻辑局部坐标，以及 WMS 为某个 Surface 准备的局部 Region。中间可能已经经过 compatibility 与 size-compat 尺度处理，不能只看数值相同就认为坐标系相同。

对通过全部 Region 裁剪后仍幸存的触点，在最简单的正、非零轴向 `scale + translate` 场景，坐标回程可以写成：

```text
screenPoint = finalFrameOrigin + appLocalPoint × layerScale
appLocalPoint = (storedRawPoint - finalFrameOrigin) × windowScale
windowScale = 1 / layerScale
```

这组等式要求：没有 per-pointer 归一化、没有零坐标保护、没有额外事件变换，`storedRawPoint` 确实还是该 Display 点。后文会逐项拆掉这些假设。

## 3. App 入口：四种模式共享一次完整声明

`ViewTreeObserver.InternalInsetsInfo` 是隐藏接口，包含 `contentInsets`、`visibleInsets`、`touchableRegion` 和 `mTouchableInsets`。前两项是从 frame 四边向内扣除的距离；`touchableRegion` 是相对 window frame 原点的形状；最后一项决定 WMS 采用哪组值：

| 模式 | 值 | WMS 的几何种子 |
|---|---:|---|
| `TOUCHABLE_INSETS_FRAME` | 0 | 整个 `mFrame` |
| `TOUCHABLE_INSETS_CONTENT` | 1 | frame 扣 `contentInsets` |
| `TOUCHABLE_INSETS_VISIBLE` | 2 | frame 扣 `visibleInsets` |
| `TOUCHABLE_INSETS_REGION` | 3 | `touchableRegion` 平移到 frame 原点 |

CONTENT 与 VISIBLE 都不是 View 树或 Surface 透明像素的自动扫描结果。它们是监听器写入、客户端上报、WMS 保存的四边声明；模式只决定稍后选哪一组。

每次派发监听器前，ViewRoot 都调用 `reset()`：三个几何值清空，模式恢复 FRAME。多个监听器按数组顺序收到同一个 `inoutInfo`，后一个监听器能继续修改前一个的结果，框架不会自动替它们求并集。回调因此必须描述本轮完整状态，不能依赖上轮 Region 残留。

`isEmpty()` 也有严格含义：三组几何为空且模式为 FRAME 才算空。只把模式设为 REGION、却留下空 `touchableRegion`，仍是一份非默认声明；在 nonmodal 的 `getTouchableRegion()` 路径中，它得到空显式 Region，而不是自动退回 FRAME。modal 的 surface 路径会在后文另行扩张。

### 练习 1：验证 reset、共享对象与空 REGION

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public final static class InternalInsetsInfo {' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'public final Rect contentInsets = new Rect();' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'public final Region touchableRegion = new Region();' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'public static final int TOUCHABLE_INSETS_FRAME = 0;' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'public static final int TOUCHABLE_INSETS_REGION = 3;' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'mTouchableInsets = TOUCHABLE_INSETS_FRAME;' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F '&& mTouchableInsets == TOUCHABLE_INSETS_FRAME;' frameworks/base/core/java/android/view/ViewTreeObserver.java
grep -n -F 'access.get(i).onComputeInternalInsets(inoutInfo);' frameworks/base/core/java/android/view/ViewTreeObserver.java
```

设监听器 A 选择 REGION 并写入 `[0,0,100,100] ∪ [200,0,300,100]`，监听器 B 从同一个 Region 减去 `[50,0,250,50]`。画出 B 收到的输入与最终输出；再分别判断“什么都不写”和“只设 REGION 模式”是否会让 `isEmpty()` 返回真。最后说明为什么下一轮开始时 A 必须重新写入两个岛。

## 4. Traversal 与 Binder：删除最后一个监听器仍会再清一次

`performTraversals()` 不是只有“当前存在监听器”才计算 internal insets。它的门是：

```text
hasComputeInternalInsetsListeners()
或 mHasNonEmptyGivenInternalInsets
```

第二项解决一个容易遗漏的撤销场景：最后一个监听器刚被删除时，WMS 仍保存旧的非空值；ViewRoot 还需经历一轮 reset、无监听器派发和差异比较，把服务端清回默认。

首次布局或可见性改变时，`insetsPending` 可随 relayout 发给 WMS。它表示客户端的最终 internal insets 尚未算完，避免服务端暂时把未经计算的 frame 内容用于其他窗口布局。随后正式计算会因为 pending 或值变化进入 `setInsets()`；WMS 收到后清掉 `mGivenInsetsPending`。

比较发生在 translator 之前：`mLastGivenInsets` 保存 App 侧本轮声明。确需发送时，compatibility translator 分别取得 content、visible 与 touchable area 的转换副本；对 Region 主要是尺度转换，仍没有替它加 window frame 原点。

`IWindowSession.setInsets()` 在 r48 的 AIDL 中没有 `oneway`，所以这是同步 Binder 调用。`Session` 只转发；WMS 在全局锁内保存字段、请求 display layout 并执行一次 surface placement。不过 Binder 返回只证明这段服务端调用返回，不证明第 240 章的 SurfaceFlinger→InputDispatcher 快照链已经完成。

### 练习 2：给“撤销旧声明”画出两条完成线

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F '|| mAttachInfo.mHasNonEmptyGivenInternalInsets;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'insetsPending = computesInternalInsets && (mFirst || viewVisibilityChanged);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'insets.reset();' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mAttachInfo.mTreeObserver.dispatchOnComputeInternalInsets(insets);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if (insetsPending || !mLastGivenInsets.equals(insets)) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'touchableRegion = mTranslator.getTranslatedTouchableArea(insets.touchableRegion);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mWindowSession.setInsets(mWindow, insets.mTouchableInsets,' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'void setInsets(IWindow window, int touchableInsets, in Rect contentInsets,' frameworks/base/core/java/android/view/IWindowSession.aidl
grep -n -F 'mService.setInsetsWindow(this, window, touchableInsets, contentInsets,' frameworks/base/services/core/java/com/android/server/wm/Session.java
grep -n -F 'w.mGivenInsetsPending = false;' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'mWindowPlacerLocked.performSurfacePlacement();' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'mInputTransaction.setInputWindowInfo(' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'transaction->setInputWindowInfo(ctrl, *handle->getInfo());' frameworks/base/core/jni/android_view_SurfaceControl.cpp
grep -n -F 'mInputFlinger->setInputWindows(inputHandles,' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'mDispatcher->setInputWindows(handlesPerDisplay);' frameworks/native/services/inputflinger/InputManager.cpp
```

场景：上一轮是非空 REGION，本轮开始前删除最后一个 listener；本轮不是首次布局、可见性未变，因此 `insetsPending=false`。第一条线画到 `setInsets()` 同步返回，第二条线画到 Dispatcher 真正换表；标出 `reset`、`equals`、translator、Binder、surface placement 和第 240 章后续事务边界。解释为何第一条线不能替代第二条线。

## 5. WMS 保存的是声明，不是最终命中区域

`WindowManagerService.setInsetsWindow()` 找到 `WindowState` 后，覆盖四份长期状态：

```text
mGivenContentInsets
mGivenVisibleInsets
mGivenTouchableRegion
mTouchableInsets
```

若 `mGlobalScale != 1`，三份几何声明会立即按该比例缩放。这个服务端 size-compat 尺度与客户端 `CompatibilityInfo.Translator` 是两个独立检查点；不能把两者都笼统叫作“系统自动换成屏幕坐标”，也不能假定任意窗口一定同时经过两次。

字段写入后，WMS 调用 `setDisplayLayoutNeeded()` 和 `performSurfacePlacement()`，并通知 accessibility controller 窗口区域可能变化。这说明 touchable 声明不只是 InputDispatcher 的私有输入：它也会影响需要观察窗口几何的服务端消费者。

此时 `mGivenTouchableRegion` 仍相对 frame，content/visible 仍是四边 inset。真正把它们变成 Display 全局 Region 的是后续 `WindowState.getTouchableRegion()`；真正交给输入事务的又是 `getSurfaceTouchableRegion()`。调试时若只打印 `mGiven*`，还看不到 modal 扩张、Task 裁剪、tap-exclude、Layer crop 或最终 frame。

三个动作也有不同完成含义：

```text
保存 mGiven*             客户端声明已进入 WindowState
performSurfacePlacement  WMS 开始把新事实写进 Surface 事务
Dispatcher 换表          新 Region 才成为后续命中的 native 快照
```

## 6. 四种模式先变成 Display 全局 Region

`getTouchableRegion(outRegion)` 以当前 `mWindowFrames.mFrame` 为锚。设 frame 为 `[L,T,R,B]`，四边 inset 为 `[l,t,r,b]`：

```text
FRAME   = [L, T, R, B]
CONTENT = [L+l, T+t, R-r, B-b]
VISIBLE = 同式，但使用 mGivenVisibleInsets
REGION  = mGivenTouchableRegion + (L,T)
```

这里的右、下 inset 是从 `R`、`B` 向内减，不是相对左上角的坐标。REGION 则保留多岛、洞和差集；把它先退化成 bounds 会永久丢失形状。

四选一只是几何种子。随后函数无条件尝试 `cropRegionToStackBoundsIfNeeded()`，再尝试 `subtractTouchExcludeRegionIfNeeded()`。所以在这条 nonmodal 路径里，App 声明只是后续取交集或做差的上界；modal 的 surface 路径会改用更大的系统范围，不能套用“只会收窄”。

还要把模式与窗口 flags 分开。`TOUCHABLE_INSETS_*` 回答“显式区域长什么样”；`FLAG_NOT_TOUCHABLE`、modal 与 native 准入回答“这份区域是否、怎样参与目标选择”。空 Region 的意义也必须结合普通 WMS 路径是否已显式加上 `FLAG_NOT_TOUCH_MODAL` 判断。

### 练习 3：手算四种模式、Task 交集与一个洞

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'private static void applyInsets(Region outRegion, Rect frame, Rect inset) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'case TOUCHABLE_INSETS_FRAME:' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'case TOUCHABLE_INSETS_CONTENT:' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'case TOUCHABLE_INSETS_VISIBLE:' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'case TOUCHABLE_INSETS_REGION: {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'outRegion.translate(frame.left, frame.top);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'cropRegionToStackBoundsIfNeeded(outRegion);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'subtractTouchExcludeRegionIfNeeded(outRegion);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'region.op(mTmpRect, Region.Op.INTERSECT);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'touchableRegion.op(touchExcludeRegion, Region.Op.DIFFERENCE);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
```

给定 `frame=[100,200,500,700]`、`contentInsets=[20,30,40,50]`、`visibleInsets=[0,80,0,120]`、局部 Region 为 `[10,20,110,120] ∪ [250,300,350,400]`。分别算四种全局结果；再假定 Java Stack crop 门已开启、stack 不是 organizer 创建，与 bounds `[150,250,480,650]` 求交，并从结果减去屏幕 Region `[200,300,260,360]`。保留 Region 的分块结果，不得只写外接矩形。

## 7. modal 有三种口径，输入快照只走其中一条

源码里有三个名字相近的入口：

| 方法 | 主要使用者 | modal 时怎样处理 |
|---|---|---|
| `getTouchableRegion()` | accessibility、IME、Display 内部逻辑等 | 不主动扩大，只算四种模式并裁剪/做差 |
| `getEffectiveTouchableRegion()` | r48 的 system gesture exclusion 计算 | 先用 Display bounds，再做 Stack crop 与 tap-exclude |
| `getSurfaceTouchableRegion()` | `InputMonitor.populateInputWindowHandle()` | 把普通 modal 编成显式大 Region，并给 flags 加 `NOT_TOUCH_MODAL` |

`getSurfaceTouchableRegion()` 以传入 flags 同时不含 `FLAG_NOT_TOUCH_MODAL` 与 `FLAG_NOT_FOCUSABLE` 作为 modal。这个分支不先调用 `getTouchableRegion()`，也就不会先消费 App 的四种模式或给其 Region 加 frame 原点；它直接重设系统范围。Activity 窗口先尝试 letterbox inner bounds；为空且 `task != null` 时只取该 Task dim bounds，只有 `task == null && getRootTask() != null` 才取 root-task dim bounds，并不存在“Task 结果为空再退到 root”的二次回退。freeform 会把所得矩形向外扩一个 resize-handle 宽度，之后仍可能做 Java Stack 交集。

没有 `ActivityRecord` 的 modal 系统窗口不会简单使用当前 frame。r48 以 Display 宽高构造一个足够大的 Region，目的是窗口移动后仍覆盖 Display；随后再减 tap-exclude。

完成显式化后，WMS 把 `FLAG_NOT_TOUCH_MODAL` 写回输出 flags。于是普通 `WindowState` 到 native 时通常统一按 Region 命中。Dispatcher 里保留的 native modal 分支仍然重要，但主要覆盖手工构造、未经过这次转换的 handle，不能反推普通窗口会忽略其 Region。

`getEffectiveTouchableRegion()` 的 Display-bounds modal 口径并不是输入快照的替代实现。它服务于另一位调用者；第 242 章会用它解释 system gesture exclusion 为什么只能发生在有效可触摸范围内。

## 8. tap-exclude 同时做局部裁剪、全局登记与 Region 差集

本章的 `mTapExcludeRegion` 主要由 `TaskEmbedder` 一类宿主通过 `IWindowSession.updateTapExcludeRegion()` 更新。WMS 接口注释给它三项效果：区域内的 DOWN 不切换焦点到宿主窗口、不把其 Display 移到顶层，也不把触摸发送给宿主窗口。

传入 Region 以宿主窗口局部坐标解释。`getTapExcludeRegion()` 先用 `[0,0,frame.width,frame.height]` 裁掉窗口外部分，再加 `frame.left/top` 变成屏幕坐标。`subtractTouchExcludeRegionIfNeeded()` 用 `Region.Op.DIFFERENCE` 从该窗口的全局 touchable Region 中减去它；差集可以留下多个条带或洞。

同一局部声明还会被 union 进 `DisplayContent.mTouchExcludeRegion`，交给 `TaskTapPointerEventListener`。后者只在 DOWN 不位于这份 Display exclude Region 时调用 `handleTapOutsideTask()`。这条“任务点击/resize/focus”观察链与窗口自身的 InputWindowInfo Region 是两份消费者，不要只验证其中一份。

它也不是公开的 system gesture exclusion：

```text
tap-exclude
    IWindowSession.updateTapExcludeRegion → WindowState.mTapExcludeRegion
    影响宿主窗口命中与 TaskTapPointerEventListener

system gesture exclusion
    View.setSystemGestureExclusionRects → WindowState / DisplayContent 的另一组状态
    参与左右系统手势边缘限制与策略仲裁
```

名字都有 exclusion，并不代表可以共用 API、长度限制或策略结论。

### 练习 4：追一块嵌入区域的两份去向

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'session.updateTapExcludeRegion(window, tapExcludeRegion);' frameworks/base/core/java/android/window/TaskEmbedder.java
grep -n -F 'void updateTapExcludeRegion(IWindow window, in Region region);' frameworks/base/core/java/android/view/IWindowSession.aidl
grep -n -F 'mTapExcludeRegion.set(region);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'outRegion.op(mTmpRect, Region.Op.INTERSECT);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'outRegion.translate(mWindowFrames.mFrame.left, mWindowFrames.mFrame.top);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'touchableRegion.op(touchExcludeRegion, Region.Op.DIFFERENCE);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'amendWindowTapExcludeRegion(mTouchExcludeRegion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'win.getTapExcludeRegion(region);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'mTapDetector.setTouchExcludeRegion(mTouchExcludeRegion);' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (!mTouchExcludeRegion.contains(x, y)) {' frameworks/base/services/core/java/com/android/server/wm/TaskTapPointerEventListener.java
grep -n -F 'mSystemGestureExclusion = new Region();' frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
grep -n -F 'if (modal) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'updateRegionForModalActivityWindow(region);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'if (task != null) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '} else if (getRootTask() != null) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'region.set(-dw, -dh, dw + dw, dh + dh);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
```

设 `mGlobalScale=1`、无其他 crop，宿主 frame 为 `[100,200,500,700]`，窗口局部 tap-exclude 为 `[-10,50,150,250]`，原全局 touchable Region 为整个 frame。先裁到窗口局部 bounds，再平移并从 touchable Region 做差；同时说明同一屏幕区域如何进入 TaskTapPointerEventListener 的 Display exclude 集。最后列出它与 system gesture exclusion 的状态字段和消费者差异，并用 modal 分支锚点说明为什么 modal 不先消费这份 App touchable Region。

## 9. Task 裁剪不是“两层总会同时开启”

Task/Stack 对 touchable Region 有三个独立门，不能简化为“Java 裁一次，SF 再裁一次”。

第一门是 `task.cropWindowsToStackBounds()`。top-most 的 HOME/RECENTS 在特定条件下返回 false，其余路径最终看 `isResizeable()`；所以“窗口属于 Task”本身不保证裁剪。

第二门是 Java Region 交集。只有第一门为真、存在 stack 且 `stack.mCreatedByOrganizer == false` 时，才取得 stack dim bounds 并求交。freeform 没有跳过这一步，而是先把 dim bounds 向外扩 `RESIZE_HANDLE_WIDTH_IN_DP`，为阴影和 resize handle 留出命中带。

第三门是 `InputWindowHandle` 的 Surface crop 引用。第一门为真且存在 stack、窗口又不是 freeform 时，WMS 保存 stack `SurfaceControl`；仅当 replace 位为 false，SF 才用该 Layer 的 `mScreenBounds` 与 Region 求交。freeform 在这里清空 stack crop handle：从未 replace 的普通 handle 会保留 Java 结果，已有 sticky replace 的 handle 则会改用当前输入 Layer 自己的 bounds。

organizer 还引入两组不同条件：

- `stack.mCreatedByOrganizer` 让 Java dim-bounds 交集直接跳过；
- `child.getTask().isOrganized()` 让 `InputMonitor` 在 populate 后调用 `replaceTouchableRegionWithCrop(null)`，把 crop handle 改成 null 并把 replace 位设为 true，SF 最终改用该输入 Layer 自己的 `mScreenBounds`。

后一条 replacement 会覆盖先前算出的 App REGION、modal 范围、tap-exclude 差集和 stack crop 引用；在后续有效 clone-root 交集之前，几何以当前 Layer bounds 为准。`mCreatedByOrganizer` 与 `isOrganized()` 不是同一个字段。更不能把 replace 当成本轮临时开关：helper 只把 `replaceTouchableRegionWithCrop` 置为 true，普通 `setTouchableRegionCrop()` 不会复位。读长期复用的 handle 时要把这个 sticky 位纳入状态。

### 练习 5：填完 Java crop、Surface crop 与 replace 矩阵

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'boolean cropWindowsToStackBounds() {' frameworks/base/services/core/java/com/android/server/wm/Task.java
grep -n -F 'if (isActivityTypeHome() || isActivityTypeRecents()) {' frameworks/base/services/core/java/com/android/server/wm/Task.java
grep -n -F 'return isResizeable();' frameworks/base/services/core/java/com/android/server/wm/Task.java
grep -n -F 'if (stack == null || stack.mCreatedByOrganizer) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'adjustRegionInFreefromWindowMode(mTmpRect);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'region.op(mTmpRect, Region.Op.INTERSECT);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'if (stack == null || inFreeformWindowingMode()) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'handle.setTouchableRegionCrop(stack.getSurfaceControl());' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'if (child.getTask() != null && child.getTask().isOrganized()) {' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.replaceTouchableRegionWithCrop(null' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'replaceTouchableRegionWithCrop = true;' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'touchableRegionSurfaceControl = new WeakReference<>(bounds);' frameworks/base/core/java/android/view/InputWindowHandle.java
```

先假定 handle 从未 replace、相关 stack 均存在。为下列互斥行填写五列：`cropWindowsToStackBounds`、Java 交集、最终 crop handle、replace 位、SF 使用原 Region 还是 bounds：①无 Task；②可 resize、非 HOME/RECENTS、普通 stack、非 freeform、child Task 未 organized；③与②相同但 freeform；④crop 门为真、`stack.mCreatedByOrganizer=true`、child Task 未 organized、非 freeform；⑤crop 门为真、普通 stack、`child.getTask().isOrganized()=true`、非 freeform。再增加第⑥行：同一 handle 曾经走过 replace，后来只调用普通 crop helper，会发生什么。

## 10. 回到 Surface 局部，以及 size-compat 的一次定向补偿

无论 modal 还是非 modal，WMS 都先在 Display 全局坐标完成 Task 交集与 tap-exclude 差集，随后执行：

```text
region.translate(-mFrame.left, -mFrame.top)
```

这不是无效绕路。全局阶段适合与 stack dim bounds、Display bounds 和 screen-space exclude 运算；Surface 局部阶段则让 Region 可以随关联 Layer 的 position、scale 与 crop 一起提交。若窗口只移动，WMS 不必把每个局部小矩形都手工改成最终屏幕坐标。

r48 对 size-compat 有一个条件严格的补偿：只有存在 `ActivityRecord`、`hasSizeCompatBounds()` 为真且 `mGlobalScale != 1`，才在提交前把 Region 乘 `mInvGlobalScale`。源码理由是 `mFrame` 已经 post-scaled，而 SF 还会再次应用 Layer scale；不先逆一次会让 Region 被重复放大。

这不是“所有 Region 交给 SF 前都逆缩放”。普通动画缩放、系统窗口或没有 size-compat bounds 的窗口不走该分支。`InputMonitor` 另把 `scaleFactor=1/mGlobalScale` 写入 handle，JNI 将它复制成 `globalScaleFactor`；Dispatcher 发布时，这个字段直接调节 touch/tool major/minor，X/Y 的局部回程则依靠 SF 给出的 window scale 与 offset。它不能和提交前的 Region 补偿合并成一个字段。

以 `mGlobalScale=1.5` 为例，若给定局部 Region 已在 WMS 中变为 `[15,30,165,180]`，满足该 size-compat 条件时，提交前先以 `2/3` 还原为 `[10,20,110,120]`；SF 的 Layer 轴向 scale 再把它变回视觉尺度。实际整数 Region 会经过各层取整，所以非整数边缘还要以源码输出为准。

## 11. SurfaceFlinger 先定 frame，再把局部 Region 放到屏幕

`Layer::fillInputInfo()` 复制 `mDrawingState.inputInfo` 后才改本次输出。几何顺序是：

1. 取 `t.sx()`、`t.sy()`；若不是 1，按这两个分量缩放 Region，并把 `windowXScale/windowYScale` 分别乘其倒数。分量为 0 时，对应 window scale 被设为 0。
2. 同一轴向分量缩放 `surfaceInset` 并四舍五入。
3. 普通 Layer 从 drawing buffer size 取 `layerBounds`，无效时退到 cropped buffer size；portal 则用当前 touchable Region 的 bounds。
4. 对 `layerBounds` 应用完整 `Transform`，得到屏幕轴对齐外接矩形。
5. 把非负 surface inset 限制在各轴尺寸一半以内，并从四边内缩；结果写成最终 frame。
6. 把已轴向缩放的 Region 加 `frameLeft/frameTop`，随后才进入 crop/replace/clone。

`surfaceInset` 与 content inset 不同。InputMonitor 在 r48 只取 `attrs.surfaceInsets.left` 这个标量；SF 将同一初值分别按 x/y scale 得到两轴 inset。它缩小并移动最终 frame 原点，却不先从 touchable Region 做一个同形差集，因而影响后续事件 offset 的坐标原点。

还要正视一处实现边界：frame 的四个角走完整矩阵，Region 在本函数中只显式走 `scaleSelf(t.sx(), t.sy())` 再平移。这里没有把 Region 的每个矩形套入任意旋转或 shear 矩阵；甚至 `sx()/sy()` 就是矩阵对角元素。纯轴向算例可以闭合，但不能把它推广成 r48 对旋转命中形状的完整证明。

源码注释说 Region 应与 layer bounds 匹配，但这段函数在平移后没有无条件执行“Region ∩ final frame”。真正可见的进一步改写或约束来自 crop handle、replace 与 clone 分支；其中 replace 甚至可能把 Region 放大或移位。调试一个超出 frame 的 Region 时，应读实际运算，而不是把注释当作隐含的 intersect。

### 练习 6：手算 surfaceInset、最终 frame 与正向 Region

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'InputWindowInfo info = mDrawingState.inputInfo;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'const float xScale = t.sx();' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.windowXScale *= (xScale != 0.0f) ? 1.0f / xScale : 0.0f;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion.scaleSelf(xScale, yScale);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'xSurfaceInset = std::round(xSurfaceInset * xScale);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'Rect layerBounds = info.portalToDisplayId == ADISPLAY_ID_NONE' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'layerBounds = t.transform(layerBounds);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'std::min(xSurfaceInset, layerBounds.getWidth() / 2)' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.frameLeft = layerBounds.left;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = info.touchableRegion.translate(info.frameLeft, info.frameTop);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'return mMatrix[0][0];' frameworks/native/libs/ui/Transform.cpp
grep -n -F 'Region& Region::scaleSelf(float sx, float sy) {' frameworks/native/libs/ui/Region.cpp
```

假定非 portal、无 crop/clone，初始 `windowXScale=windowYScale=1`。给定 buffer bounds `[0,0,200,100]`、Surface 局部 Region `[10,20,110,80]`、`surfaceInset=10`，Layer 只做 `x×2、y×1.5` 并平移到 `(100,200)`。算出 transform 后 bounds、两轴 inset、最终 frame、最终屏幕 Region 与 `windowXScale/windowYScale`。然后把 transform 换成 90° 旋转，只说明源码分别怎样处理 frame 与 Region，不用纯缩放公式伪造一个旋转后 Region。

## 12. intersect、replace 与 clone 决定最终改写

SF 尝试把 Java 弱引用提升为 crop Layer。三条分支按固定顺序执行：

```text
replace == true 且 crop Layer 存在
    Region = crop Layer.mScreenBounds

replace == true 且 crop Layer 为空
    Region = 当前输入 Layer.mScreenBounds

replace == false 且 crop Layer 存在
    Region = 原屏幕 Region ∩ crop Layer.mScreenBounds

replace == false 且 crop Layer 为空
    保留原屏幕 Region
```

replace 会完全丢弃 App 声明、modal 扩张和前面的 Region 形状；intersect 才保留其洞与多岛。null 的含义依赖 replace 位，绝不能固定解释成“不裁剪”。

只有 `isClone()` 为真且 `getClonedRoot()` 返回非空 Layer，SF 才在上述分支之后再与 cloned root 的 `mScreenBounds` 求交。这是最后一层防止克隆区域外出现命中的保护；若 clone 找不到 root，本段不会凭空制造一个空裁剪。它仍是 bounds 交集，而不是视觉像素透明度蒙版。

设前一节得到 Region `[140,245,340,335]`，crop Layer bounds 为 `[160,230,320,320]`：

```text
intersect → [160,245,320,320]
replace   → [160,230,320,320]
```

如果 clone root bounds 再给出 `[180,250,300,310]`，两条结果最终都还要与它求交。边界点恰好等于 right 或 bottom 时不在该 `Rect` 内。

### 练习 7：证明 null crop 的语义取决于 sticky replace 位

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public void replaceTouchableRegionWithCrop(@Nullable SurfaceControl bounds) {' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'setTouchableRegionCrop(bounds);' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'replaceTouchableRegionWithCrop = true;' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'public void setTouchableRegionCrop(@Nullable SurfaceControl bounds) {' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'auto cropLayer = mDrawingState.touchableRegionCrop.promote();' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'if (info.replaceTouchableRegionWithCrop) {' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = Region(Rect{mScreenBounds});' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = Region(Rect{cropLayer->mScreenBounds});' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = info.touchableRegion.intersect(Rect{cropLayer->mScreenBounds});' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'if (isClone()) {' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'if (clonedRoot != nullptr) {' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'Rect rect(clonedRoot->mScreenBounds);' frameworks/native/services/surfaceflinger/Layer.cpp
```

假定原屏幕 Region 为 `[140,245,340,335]`，当前输入 Layer 的 self bounds 为 `[120,215,480,335]`，存活的 crop A/B bounds 分别为 `[160,230,320,320]` 与 `[180,240,360,330]`，clone root bounds 为 `[190,250,300,310]`。对五种历史分别写出 weak crop、replace 位和非 clone 的 SF 结果：①新 handle，从未调用任何 crop helper；②只调用 `setTouchableRegionCrop(A)`；③调用 `replaceTouchableRegionWithCrop(null)`；④先 replace(A)，再 set crop(null)；⑤先 replace(A)，再 set crop(B)。最后给每个结果加上 clone root 交集，保留中间值。

## 13. Dispatcher 命中只消费最终屏幕事实

`findTouchedWindowAtLocked()` 按 display 内 front-to-back 顺序遍历 handle。窗口必须属于当前 display 且 `visible`；带 `FLAG_NOT_TOUCHABLE` 的窗口不会成为前景命中窗口。随后才判断：

```text
native modal
或 touchableRegionContainsPoint(int x, int y)
```

普通触摸从 `MotionEntry.pointerCoords` 取 X/Y 并转为 `int32_t`；mouse 使用 cursor position。到这里，point 与 Region 都是 Dispatcher 所理解的 Display 屏幕坐标，Dispatcher 不再知道它最初来自 CONTENT、REGION、Task dim bounds 还是 Layer crop。

普通 `WindowState` 原本的 modal 已在 WMS 改写为 `NOT_TOUCH_MODAL + 显式大 Region`，所以仍依靠 contains。手工 handle 若保持 native modal，则可绕过 Region contains；这就是“空 Region 一定不可触摸”不是跨所有构造路径定理的原因。

`NOT_TOUCHABLE` 也不等于“绝不会收到本次手势的任何事件”。初始 DOWN 查找前景窗口时会开启 outside-target 收集；遍历到一个可见且带 `WATCH_OUTSIDE_TOUCH` 的窗口，即使它不可触摸或点不在 Region 内，仍可能先以 `DISPATCH_AS_OUTSIDE` 加入 `TouchState`。若它与最终前景窗口的 owner UID 不同，Dispatcher 才再给它加 `FLAG_ZERO_COORDS`，避免泄露坐标。

Region 的命中也不是每个 MotionEvent 都重算。DOWN、SCROLL、hover，以及手势已 split 时的新 `POINTER_DOWN` 走 Case 1 查找；普通 MOVE/UP/CANCEL 和 non-split `POINTER_DOWN` 走 Case 2，复用已有 `TouchState`。Case 2 里只有单指 slippery MOVE 会再次调用窗口查找。因此，单凭 Region 在手势中途改变通常不会把当前流改投另一窗口；窗口生命周期与取消等其他状态变化仍需另查。

命中只决定“选谁”。窗口上方的遮挡、outside observer、portal 递归、split 所有权和注入权限还会影响最终 targets，但它们不会重新解释 App 的四种 touchable-insets 模式。选中后，坐标回程才从最终 frame 与 window scale 开始。

## 14. InputTarget 保存逆变换，多指还会改写保存的 raw

`addWindowTargetLocked()` 先按 token 找 input channel；没有已注册 channel 就不产生该 target。正常窗口把这些值写进 `InputTarget`：

```text
xOffset = -final frameLeft
yOffset = -final frameTop
windowXScale / windowYScale = SF 已累计的逆 Layer 轴向 scale
globalScaleFactor = InputMonitor 写入的 scaleFactor；mGlobalScale != 1 时为 1/mGlobalScale，否则为 1
```

offset 在 target 中先以未缩放的负 frame 保存。发布时 Dispatcher 才计算：

```text
publishedXOffset = xOffset × windowXScale
publishedYOffset = yOffset × windowYScale
```

`InputTarget` 不是只能存一组几何，但这不表示不同 connection 的窗口会挤进同一个 target。`addWindowTargetLocked()` 以相同 connection token 找到并复用一项，随后还断言 `targetFlags` 与 `globalScaleFactor` 相同；`addPointers()` 又断言新旧非空 pointerIds 不重叠。只有这些条件同时成立，才是合法合并。不同 token 各自形成 target；相应 target 带 `FLAG_SPLIT` 且只含 pointer 子集时，才会各自拆出事件。

创建这类 `DispatchEntry` 时，规范基准是 `pointerIds.firstMarkedBit()`，即集合中最小的 pointer id，不是 MotionEvent 数组里口语所说的“第一根手指”。源码把其他 pointer 的 `PointerCoords` 先移到各自窗口原点、按 `currentScale/firstScale` 归一化，再移回基准 frame。这样一枚事件只需携带一组最终 scale/offset，却仍能让各 pointer 的局部值闭合。

代价是：非基准 pointer 传到客户端的保存坐标可能已不是原 Display 点，因此它的 `getRawX(pointerIndex)` 也可能改变。`raw` 在 MotionEvent 中的严格含义是“不应用该事件保存的 mXScale/mXOffset”，不是“永远未经 Dispatcher 改写”。

`globalScaleFactor != 1` 时，发布循环复制 PointerCoords 并调用 `scale(globalScaleFactor, 1, 1)`。由于 X/Y 的 window scale 参数明确传 1，这一步不改变 X/Y；它缩放的是 `TOUCH_MAJOR/MINOR` 与 `TOOL_MAJOR/MINOR`。注释说明 global scale 已包含在 windowX/YScale 的几何回程中，不能再乘到 X/Y 一次。pressure、size 与 orientation 也不会在这里缩放。

退化 scale 不能硬套逆变换。SF 遇到某轴 scale 为 0 会把对应 window scale 设为 0；同 target 的多指规范化却直接用 `currentScale/firstScale`，没有为基准 scale 为 0 建立可逆坐标。本文所有数值闭环都明确排除这类场景。

若 target 带 `FLAG_ZERO_COORDS`，Dispatcher 会清空坐标以避免向 outside 接收者泄露位置。此时任何普通 raw/local 等式都不适用。

### 练习 8：先算单指逆变换，再证明多指归一化

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'it->addPointers(pointerIds, -windowInfo->frameLeft, -windowInfo->frameTop,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'inputWindowHandle.scaleFactor = 1.0f/child.mGlobalScale;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputTarget.inputChannel->getConnectionToken() ==' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'inputTarget.globalScaleFactor = windowInfo->globalScaleFactor;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (inputTarget.flags & InputTarget::FLAG_SPLIT) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'splitMotionEvent(originalMotionEntry, inputTarget.pointerIds);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerInfos[pointerId].xOffset = xOffset;' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F 'inputTarget.pointerInfos[inputTarget.pointerIds.firstMarkedBit()];' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const PointerInfo& firstPointerInfo =' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'float scaleXDiff = currPointerInfo.windowXScale / firstPointerInfo.windowXScale;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].applyOffset(currPointerInfo.xOffset, currPointerInfo.yOffset);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].scale(1, scaleXDiff, scaleYDiff);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].applyOffset(-firstPointerInfo.xOffset,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'xOffset = dispatchEntry->xOffset * xScale;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'scaledCoords[i].scale(globalScaleFactor, 1' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'scaleAxisValue(*this, AMOTION_EVENT_AXIS_TOUCH_MAJOR, globalScaleFactor);' frameworks/native/libs/input/Input.cpp
grep -n -F 'scaledCoords[i].clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!(flags & InputWindowInfo::FLAG_NOT_TOUCHABLE)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (addOutsideTargets && (flags & InputWindowInfo::FLAG_WATCH_OUTSIDE_TOUCH)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (touchedWindow.targetFlags & InputTarget::FLAG_DISPATCH_AS_OUTSIDE) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (inputWindowHandle->getInfo()->ownerUid != foregroundWindowUid) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool newGesture = (maskedAction == AMOTION_EVENT_ACTION_DOWN ||' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '/* Case 2: Pointer move, up, cancel or non-splittable pointer down. */' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'tempTouchState.isSlippery()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

先用 `frameLeft=120`、`windowXScale=0.5`、屏幕点 `x=300` 算发布的 xOffset 与客户端局部 X。再假定相同 connection token、相同 target flags 与相同 `globalScaleFactor=1` 使两组几何合法合入一个 target：新加入的 pointerIds 分别为互不重叠的 `{0}`、`{1}`，pointer 0 的 `(offset,scale)=(-100,1)`，pointer 1 的 `(-300,0.5)`，两者原保存 X 分别为 160 与 500。以最小 pointer id 0 为基准逐行执行归一化，验证最终统一参数仍让两根手指各自回到原窗口局部值，并指出哪一根的客户端 raw 已不再等于输入的 Display X。最后说明 token 不同时先形成两个 target；只有相应 target 带 `FLAG_SPLIT` 且只拥有 pointer 子集时，`prepareDispatchCycleLocked()` 才把原 MotionEntry 拆成各自事件。

## 15. InputTransport 与 MotionEvent 同时保留保存坐标和局部读数

`InputPublisher::publishMotionEvent()` 把 PointerCoords、x/y scale、x/y offset 一并写入 `InputMessage`。App 端 `InputConsumer::initializeMotionEvent()` 原样取出，再交给 native `MotionEvent::initialize()` 保存为：

```text
mSamplePointerCoords
mXScale / mYScale
mXOffset / mYOffset
```

读取 API 才产生两种口径：

```text
getRawX(i) = savedPointerCoords[i].X
getX(i)    = savedPointerCoords[i].X × mXScale + mXOffset
```

对普通单窗口、单几何、非 zero-coords、未再变换的 pointer，saved X 仍是 Display 点，而 `mXOffset=(-frameLeft)×windowXScale`，所以：

```text
getX = (displayX - finalFrameLeft) × windowXScale
```

这也是幸存触点在纯正轴向 scale 下的坐标闭环。沿用第 11 节的 `finalFrame=[120,215,480,335]`、`windowScale=(0.5,2/3)`，取屏幕点 `(300,300)`：

```text
published offset = (-60, -143.333...)
getX = 300×0.5 - 60 = 90
getY = 300×(2/3) - 215×(2/3) = 56.666...
```

局部点落在最初 `[10,20,110,80]` 内，和正向 Region 命中一致。整数 Region 的命中与浮点 MotionEvent 坐标在边缘可能受取整影响，所以不要用一个恰落边界的样本验证。

事件从 `InputConsumer` 进入 View hierarchy 前还可能再变换。`ViewRootImpl.processPointerEvent()` 在存在 compatibility translator 时调用 `translateEventInScreenToAppWindow()`，后者执行 `event.scale(applicationInvertedScale)`；native `MotionEvent::scale()` 会缩放保存的 PointerCoords、offset 与 precision，因此 App 回调看到的 raw X/Y 也会随之缩放。`mCurScrollY` 路径随后调用 `offsetLocation()`，它只累加事件的 local offset，不改保存的 raw PointerCoords。

进入 View hierarchy 也不是终点。常规非 CANCEL 分发中，`ViewGroup` 向一个 identity-matrix child 分发时，用 `offsetX=mScrollX-child.mLeft`、`offsetY=mScrollY-child.mTop` 调用 `offsetLocation()`；child 的 `getX()/getY()` 因而是 child-local，raw 保持不变。child 若有非 identity matrix，框架会复制或拆分事件，在 offset 后再调用 `transform(child.getInverseMatrix())`。native `MotionEvent::transform()` 通过重算 offset 只刻意保持 pointer index 0 的 raw X/Y；其余 pointer 的保存坐标会按新 offset 回写，raw 可能变化。若调用参数 `cancel` 为真或原 action 已是 CANCEL，`ViewGroup` 直接改写/转发 CANCEL，不做上述 child 坐标变换。

所以 `getRawX()` 也不是触摸芯片的 evdev `ABS_X`。它之前已经经过 InputReader 校准、方向与 Display viewport 映射；同 target 的多指归一化、zero-coords、注入、ViewRoot compatibility scale 和非首 pointer 的 child matrix transform 还可能改变保存值。准确表述应是：raw API 跳过当前 MotionEvent 保存的 `mXScale/mXOffset`；Y 轴同理跳过 `mYScale/mYOffset`。

调试错位时按同一时间窗口记录：

1. listener 输出的模式与局部 `InternalInsetsInfo`；
2. `WindowState.mGiven*`、`mFrame`、Task 与 tap-exclude；
3. SurfaceFlinger 最终 frame、Region、window scale 与 crop；
4. `dumpsys input` 中 Dispatcher 当前窗口表；
5. App 同一 pointer 的 `rawX/rawY` 与 `x/y`。

单独一份 dump 只是采样，不足以证明它和另一进程日志属于同一 Surface transaction；第 240 章的 sync 边界仍然适用。

### 练习 9：用源码和数值闭合一次命中

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'msg.body.motion.xScale = xScale;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'msg.body.motion.xOffset = xOffset;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'void InputConsumer::initializeMotionEvent(MotionEvent* event, const InputMessage* msg) {' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'msg->body.motion.xScale, msg->body.motion.yScale, msg->body.motion.xOffset,' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'mXScale = xScale;' frameworks/native/libs/input/Input.cpp
grep -n -F 'mXOffset = xOffset;' frameworks/native/libs/input/Input.cpp
grep -n -F 'return getRawPointerCoords(pointerIndex)->getAxisValue(axis);' frameworks/native/libs/input/Input.cpp
grep -n -F 'return value * mXScale + mXOffset;' frameworks/native/libs/input/Input.cpp
grep -n -F 'public final float getX() {' frameworks/base/core/java/android/view/MotionEvent.java
grep -n -F 'return nativeGetAxisValue(mNativePtr, AXIS_X, 0, HISTORY_CURRENT);' frameworks/base/core/java/android/view/MotionEvent.java
grep -n -F 'public final float getRawX() {' frameworks/base/core/java/android/view/MotionEvent.java
grep -n -F 'return nativeGetRawAxisValue(mNativePtr, AXIS_X, 0, HISTORY_CURRENT);' frameworks/base/core/java/android/view/MotionEvent.java
grep -n -F 'mTranslator.translateEventInScreenToAppWindow(event);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.scale(applicationInvertedScale);' frameworks/base/core/java/android/content/res/CompatibilityInfo.java
grep -n -F 'event.offsetLocation(0, mCurScrollY);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mSamplePointerCoords.editItemAt(i).scale(globalScaleFactor);' frameworks/native/libs/input/Input.cpp
grep -n -F 'final float offsetX = mScrollX - child.mLeft;' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'event.offsetLocation(offsetX, offsetY);' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'transformedEvent.offsetLocation(offsetX, offsetY);' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'transformedEvent.transform(child.getInverseMatrix());' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'if (cancel || oldAction == MotionEvent.ACTION_CANCEL) {' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'void MotionEvent::transform(const float matrix[9]) {' frameworks/native/libs/input/Input.cpp
grep -n -F 'float scaledRawX = getRawX(0) * mXScale;' frameworks/native/libs/input/Input.cpp
grep -n -F 'c.setAxisValue(AMOTION_EVENT_AXIS_X, (x - mXOffset) / mXScale);' frameworks/native/libs/input/Input.cpp
```

使用这组固定输入：无 compatibility translator，`mGlobalScale=1`，`mCurScrollY=0`，窗口 nonmodal 并选择 REGION；WMS frame `[100,200,340,360]`，App 局部 Region `[10,20,110,120] ∪ [150,30,210,90]`，无 Java policy crop/exclude；非 portal、非 clone、初始 window scale 为 1，buffer `[0,0,240,160]`，Layer 映射 `(x,y)→(2x+100,1.5y+200)`，`surfaceInset=10`，replace=false 且 SF crop `[160,250,500,380]`，屏幕点 `(300,350)`。依次写出 WMS 全局 Region、Surface 局部 Region、SF frame/屏幕 Region、Dispatcher contains 结果、发布参数，以及 InputConsumer 初始化后、ViewRoot/child 再变换前的 raw/local。再令这是单指 ACTION_DOWN，ViewGroup 的 `cancel=false`、desired pointer 集不删 pointer 0，父 ViewGroup scroll 为 `(5,10)`、identity child 的 left/top 为 `(30,40)`；算 child 回调的 local，并说明 raw 是否改变。

再独立推演三个固定变体：①改为 `replaceTouchableRegionWithCrop(null)`，并给当前输入 Layer 的 self bounds `[100,200,580,440]`；②保持相同 connection token、flags 与 `globalScaleFactor=1`，令互不重叠的 pointerIds `{0}`、`{1}` 合入同一 target，最小 id pointer 0 使用主场景的 `(offset,scale)=((-120,-215),(.5,2/3))` 和 raw `(300,350)`，非基准 pointer 1 使用 `((-300,-100),(.25,.5))` 和原 Display 点 `(700,300)`；③给 target 加 `FLAG_ZERO_COORDS`。逐一指出 Region、saved raw 与 local 公式中哪一项先发生变化。

## 16. r48 结论、适用边界与下一章

这条几何链可以压缩为九个判断：

1. `InternalInsetsInfo` 是 App 每轮完整重写的窗口局部声明；CONTENT/VISIBLE 不是透明像素扫描。
2. ViewRoot 先比较未转换声明，再按需做 compatibility 尺度转换；`setInsets()` 同步返回不等于 Dispatcher 已换表。
3. WMS 保存 `mGiven*` 后，四种模式才围绕 `mFrame` 产生 Display 全局 Region。
4. nonmodal 会继续改写 App 声明；modal 则直接换成 letterbox/Task/root-task/Display 系统范围，之后才做相关裁剪与 tap-exclude；`getTouchableRegion`、effective 与 surface 三种口径不能互换。
5. Java dim-bounds intersect、Surface crop 与 organized-task replace 有不同门；freeform、HOME/RECENTS、`mCreatedByOrganizer`、`isOrganized()` 会组成不同结果。
6. WMS 完成全局运算后把 Region 变回 Surface 局部；size-compat 逆缩放只修 r48 指定的重复缩放条件。
7. SF 用完整 transform 定 frame，却只用对角 scale 加平移处理 Region；crop/replace 与有效 clone-root 交集再决定最终屏幕形状，replace 不保证只收窄。
8. Dispatcher 在新命中时用最终屏幕 Region 选目标，再用最终 frame 负偏移和逆轴向 scale 建立事件回程；现有流通常复用 TouchState，同一 target 内的多指归一化还可能改写非基准 pointer 的保存坐标。
9. child 分发会继续产生 child-local 坐标；`getRawX()` 只跳过当前 MotionEvent 保存的 scale/offset，不承诺等于原 Display 点，更不等于硬件未经校准的值。

可严格闭合的只是幸存触点相对最终 frame 的平移与正、非零轴向 scale；Region 的交、差、替换和 clone 裁剪本身不可逆。发生负/零 scale、旋转、same-target 多指归一化或额外事件改写时，应逐层读取最终 Region、frame、PointerCoords 与参数，而不是把示例公式外推。

下一章进入 `View.setSystemGestureExclusionRects()`、ViewRoot 坐标收集、WMS touchable/遮挡交集、左右边缘长度限制与导航手势仲裁，彻底把公开的 system gesture exclusion 与本章 tap-exclude 分开。
