# 240 Android WMS InputWindowInfo 构建、SurfaceControl 同帧同步与窗口快照链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：输入窗口表怎样与 Surface 事实对齐

前几章从 InputDispatcher 的窗口表出发，讨论了命中、焦点、触摸转移和拖放。本章反向追问：这张表从哪里来，它为何不是 WMS 直接发送的一组 `WindowState`，窗口消失时旧路由又在哪里收尾？当画面已经移动而点击仍落在旧处、`force=true` 后表没有立刻变化，或 display 移除导致进行中手势被取消时，答案都在这条链上。

先分清四层对象：

| 层次 | 主要对象 | 这一层掌握的事实 |
|---|---|---|
| WMS 语义层 | `WindowState`、Java `InputWindowHandle` | token、flags、焦点、可见性、基础 Region、进程身份 |
| Surface 事务层 | `SurfaceControl.Transaction`、`layer_state_t` | 哪份输入信息绑定到哪个 Layer，并与几何、层级一起提交 |
| SurfaceFlinger 几何层 | `Layer::fillInputInfo()` | drawing state、Layer Z 序、frame transform、Region scale/translate/crop |
| InputDispatcher 路由层 | native `InputWindowHandle`、display 窗口表 | 可命中列表、稳定对象身份、焦点及进行中事件流 |

本章的主问题是：

```text
一个长期存在的 WindowState，怎样在某次 Surface 事务中变成按前后顺序排列的
InputWindowInfo 值快照；新快照替换旧快照时，又怎样保住仍有效的事件流并终止失效状态？
```

读完应能从一项 WMS 字段一路判断它何时被复制、提交、二次几何化和接纳，并能给 scheduled、force、immediate、sync 各画一条完成线。本章不展开四种 touchable insets 的每一步 Region 算术，也不重讲事件进入 App 后的 View 分发；这些分别留给下一章和前文。

标题中的“同帧”首先指事务和 drawing-state 一致性：输入元数据与相关 Surface 层级、位置、裁剪可以进入同一提交，SurfaceFlinger 再从同一轮已提交状态导出列表。它不是“App buffer、输入窗口表、HWC present 在同一 CPU 时刻完成”的承诺。

## 2. 一条主链、三次快照与两个身份键

主链如下：

```text
WindowState
  └─ 长期 Java InputWindowHandle
       ├─ populate：重填本轮窗口语义
       └─ Transaction.setInputWindowInfo(surface, handle)
            └─ JNI 立刻复制为 InputWindowInfo 值
                 └─ layer_state_t 随 Surface 事务提交
                      └─ SurfaceFlinger drawing state
                           └─ reverse Z traversal + Layer::fillInputInfo()
                                └─ vector<InputWindowInfo>
                                     └─ InputManager 按 display 包装 BinderWindowHandle
                                          └─ InputDispatcher 替换 display 窗口表
```

这里至少有三次取值或复制边界：

1. WMS 从 `WindowState` 向可变 Java handle 写入本轮字段。
2. 调用 `setInputWindowInfo()` 时，JNI 把 Java 字段复制进事务；之后再改 Java 字段，不会追改已经排队的那份值。
3. SurfaceFlinger 从 drawing-state Layer 树重新计算 frame、绝对 Region 和顺序，再把值列表交给 InputFlinger。

两个身份键也不能混：

- `InputWindowInfo.id` 由 SurfaceFlinger 写成 Layer 的 `sequence`，标识承载输入信息的 Layer。
- `token` 来自 InputChannel connection token，把窗口元数据连到 Dispatcher 已注册的 channel。

Dispatcher 复用旧 handle 时要求二者都相同。只看 Layer id 会把换过 channel 的窗口当成旧连接；只看 token 又无法唯一标识输入窗口，因为源码明确允许不同输入窗口共享 token。

核心源码地图：

```text
frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
frameworks/base/services/core/java/com/android/server/wm/WindowState.java
frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
frameworks/base/core/java/android/view/InputWindowHandle.java
frameworks/base/core/java/android/view/InputApplicationHandle.java
frameworks/base/core/java/android/view/SurfaceControl.java
frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
frameworks/base/core/jni/android_view_SurfaceControl.cpp
frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
frameworks/native/libs/gui/SurfaceComposerClient.cpp
frameworks/native/libs/input/InputWindow.cpp
frameworks/native/services/surfaceflinger/Layer.cpp
frameworks/native/services/surfaceflinger/BufferLayer.h
frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
frameworks/native/services/inputflinger/InputManager.cpp
frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
frameworks/native/include/input/InputWindow.h
```

## 3. WindowState、InputChannel 与长期 Java handle

`WindowState` 构造时创建 `mInputWindowHandle`，但这时还没有把 channel token 填进去。`openInputChannel()` 创建 server/client pair，先向 IMS 注册 server 端，再把 pair 共有的 connection token 写入 handle，同时建立 WMS 的 token 到 `WindowState` 反查表。

因此下列对象不是同一个 token：

```text
Activity/WindowToken       窗口组织与应用身份
IWindow.asBinder()         客户端窗口 Binder
SurfaceControl handle      Layer 身份
InputChannel token         Dispatcher connection 查找键
InputWindowInfo.id         SurfaceFlinger Layer sequence
```

窗口关闭输入通道时，WMS 先 `unregisterInputChannel()`，再 dispose server/client 端，最后移除拦截信息和反查表并把 Java handle 的 token 清空。`WindowState` 的 handle 可以比一次字段快照活得久，但 token 有明确的打开与关闭区间。

`InputApplicationHandle` 与 window handle 也不是一物：前者保存 application token、名称和 dispatch timeout，给无 focused window 等应用级等待归责；后者携带具体窗口、channel 与几何事实。Activity 窗口可把前者挂进后者，系统窗口则可以没有 application handle。

还要避免“每轮从零构造”的错觉。常规刷新会重写许多字段，却不会重建 Java 对象；token 由 channel 生命周期维护，`replaceTouchableRegionWithCrop` 也只在 helper 中置为 `true`，普通 `setTouchableRegionCrop()` 本身不会把它复位。阅读长期可变 handle 时，必须逐字段确认谁负责覆盖。

### 练习 1：画出 channel token 的建立与拆除线

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mInputWindowHandle = new InputWindowHandle(' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'InputChannel[] inputChannels = InputChannel.openInputChannelPair(name);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mWmService.mInputManager.registerInputChannel(mInputChannel);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mInputWindowHandle.token = mInputChannel.getToken();' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mWmService.mInputToWindowMap.put(mInputWindowHandle.token, this);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mWmService.mInputManager.unregisterInputChannel(mInputChannel);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mInputChannel.dispose();' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mClientChannel.dispose();' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mWmService.mInputToWindowMap.remove(mInputWindowHandle.token);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mInputWindowHandle.token = null;' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'public IBinder token;' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'This value should NOT be used to uniquely identify the window.' frameworks/native/include/input/InputWindow.h
```

按时间写出“创建 pair、注册 connection、发布 token、撤销注册、dispose 两端、清映射、清 token”七个完成点，并说明仅从 handle 仍非空不能证明 channel 仍已注册。

## 4. populate：能力、选择结果与几何种子

`InputMonitor.populateInputWindowHandle()` 把字段分成四组：

| 组 | 代表字段 | 含义 |
|---|---|---|
| 身份与责任 | `name`、`type`、`ownerPid/Uid`、`displayId`、application handle | 日志、权限、ANR 与多显示路由上下文 |
| 调度状态 | `visible`、`canReceiveKeys`、`hasFocus`、`paused`、`hasWallpaper` | 当前能力与 WMS 本轮选择结果 |
| 几何种子 | frame、`surfaceInset`、`scaleFactor`、touchable Region | 交给 SurfaceFlinger 二次计算的输入 |
| 策略位 | flags、input features、crop/replace | 命中、遮挡与特殊路由规则 |

`canReceiveKeys` 与 `hasFocus` 是两件事。populate 调用无 user-touch 豁免的 `canReceiveKeys(false)`：它要求窗口 visible-or-adding、View 可见、没有 `mRemoveOnExit`、未设 `FLAG_NOT_FOCUSABLE`、Activity 允许聚焦、没有被 touch-input 策略排除，并且所在 Display 位于顶部或受信任。`hasFocus` 则只是 `w.isFocused()` 的本轮结果。Dispatcher 选新焦点时实际扫描 `hasFocus && visible`，并没有在该处再次测试 `canReceiveKeys`。正常情况下 WMS 保持这些字段一致，分析异常快照时却不能把它们当别名。

WMS 也会先写 `frameLeft/Top/Right/Bottom`，但 SurfaceFlinger 后面会依据 Layer buffer、transform 与 `surfaceInset` 重算四边。因此 WMS 的 frame 是生成 Region 和语义快照的种子，不是传到 Dispatcher 的最终屏幕 frame。

`scaleFactor` 写入 `1 / mGlobalScale`，最后成为 `globalScaleFactor`。SurfaceFlinger 另外根据 Layer transform 累乘 `windowXScale/windowYScale` 的逆缩放；一个影响全局事件尺寸轴，一个参与窗口坐标变换，不能合并成一个“缩放值”。

### 练习 2：给 populate 字段标注来源和后续改写者

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'void populateInputWindowHandle(final InputWindowHandle inputWindowHandle,' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'flags = child.getSurfaceTouchableRegion(inputWindowHandle, flags);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.visible = isVisible;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.canReceiveKeys = child.canReceiveKeys();' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.hasFocus = hasFocus;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.paused = child.mActivityRecord != null ? child.mActivityRecord.paused : false;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.surfaceInset = child.getAttrs().surfaceInsets.left;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.scaleFactor = 1.0f/child.mGlobalScale;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'final boolean hasFocus = w.isFocused();' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'public boolean canReceiveKeys(boolean fromUserTouch) {' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '&& (mViewVisibility == View.VISIBLE) && !mRemoveOnExit' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '&& !cantReceiveTouchInput();' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'return fromUserTouch || getDisplayContent().isOnTop()' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F '|| getDisplayContent().isTrusted();' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'mInfo.visible = env->GetBooleanField(obj,' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'mInfo.globalScaleFactor = env->GetFloatField(obj,' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'info.frameLeft = layerBounds.left;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.visible = hasInputInfo() ? canReceiveInput() : isVisible();' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'windowHandle->getInfo()->hasFocus &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

先用前半组锚点填 WMS 来源；读完第 8、10、14 节后，再用后半组锚点回填“JNI 是否原样复制、SurfaceFlinger 是否改写、Dispatcher 在哪里消费”三列。特别标出 frame 和 visible 并非一路原样透传。

## 5. modal 语义被显式化为 Region 与 crop

`getSurfaceTouchableRegion()` 先判断原始 flags 是否同时没有 `NOT_TOUCH_MODAL` 和 `NOT_FOCUSABLE`。若是 touch-modal 窗口，r48 不把抽象 modal 规则原样交给 native，而是：

```text
输出 flags 增加 FLAG_NOT_TOUCH_MODAL
Activity 窗口：Region 扩到 letterbox inner bounds 或 task/root-task dim bounds
非 Activity 窗口：Region 扩成足够覆盖 Display 移动范围的大矩形
再扣除 tap-exclude 区域
```

这样 Dispatcher 最终可以统一按显式 Region 命中。非 modal 窗口则按 FRAME、CONTENT、VISIBLE 或自定义 REGION 得到区域，再做 stack crop 与 exclude。两条路径最后都把 global Region 平移 `-frame.left/-frame.top`，变成关联 Surface 的局部坐标；size-compat 条件下还会额外逆缩放。

crop 有两种语义：

- 当 `replaceTouchableRegionWithCrop == false` 时，crop handle 存在则让 SurfaceFlinger 用 crop Layer 的 screen bounds 与现有 Region 求交。
- `replaceTouchableRegionWithCrop(surface)` 会先设置同一个 crop handle，再把 replace 位永久置为 `true`（除非调用方显式写回 `false`）；SurfaceFlinger 此后用 crop Layer bounds 替换 Region，handle 为空时则改用当前 Layer 的 `mScreenBounds`。

Java 保存的是 `WeakReference<SurfaceControl>`。JNI 尝试提升弱引用并抽取 native handle；提升失败会清空 crop handle。若 replace 位仍为真，SurfaceFlinger 的空 crop 分支不是“不裁剪”，而是回退到当前 Layer 的 screen bounds。

组织化 Task 会请求 `replaceTouchableRegionWithCrop(null)`，PIP input consumer 会用 root task Surface 作 replacement crop。下一章再逐个推导 insets、Task/Stack 和 transform；本章只要抓住“WMS 局部 Region + Layer crop 引用”是待 SurfaceFlinger 完成的半成品。

这套 modal 改写只属于普通 `WindowState` 的 populate 路径。第 239 章的 drag handle、input consumer 与 portal 都是手工填字段再进入共同的 SurfaceFlinger 下游；尤其不能用本节规则替 drag 的 `flags=0 + 空 Region` 自动补一条不存在的转换。

### 练习 3：手算 modal、局部化与 replacement crop

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'final boolean modal = (flags & (FLAG_NOT_TOUCH_MODAL | FLAG_NOT_FOCUSABLE)) == 0;' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'flags |= FLAG_NOT_TOUCH_MODAL;' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'updateRegionForModalActivityWindow(region);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'region.set(-dw, -dh, dw + dw, dh + dh);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'getTouchableRegion(region);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'region.translate(-mWindowFrames.mFrame.left, -mWindowFrames.mFrame.top);' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'handle.setTouchableRegionCrop(stack.getSurfaceControl());' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'inputWindowHandle.replaceTouchableRegionWithCrop(null /* Use this surfaces crop */);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'replaceTouchableRegionWithCrop = true;' frameworks/base/core/java/android/view/InputWindowHandle.java
grep -n -F 'touchableRegionSurfaceControl = new WeakReference<>(bounds);' frameworks/base/core/java/android/view/InputWindowHandle.java
```

设 Display 为 `1000×800`、窗口 frame 为 `[100,50,500,450]`，并假设 letterbox inner bounds 为空、Task dim bounds 为 `[80,40,600,500]`、`mGlobalScale=1`、无 exclude、无 stack crop。先令 SF 最终 frame 与给定 frame 相同，分别手算 Activity modal、系统 modal、非 modal FRAME 的局部 Region；再把 `[0,0,300,300]` 明确当作 screen-space crop，分别比较 intersect 与 replacement。

## 6. InputMonitor 遍历的不只是“可接收窗口”

更新轮次先把 navigation、PIP、wallpaper、recents animation 四类 input consumer 全部 hide，再以 top-to-bottom 顺序遍历 `DisplayContent`，遇到相应锚点时重新 show、设 input info、设相对层或绝对层。于是 consumer 的最终位置与普通 Window 的输入元数据处于同一 transaction。

常规 `WindowState` 有 channel、handle、未移除且未被输入策略排除时，InputMonitor 才 populate，并在有 Surface 时把 info 绑定到 client view-root Surface。没有 Surface 时，Java handle 虽已被重填，却没有 `setInputWindowInfo()`，因此不能据此断言本轮 native 表已经包含它。

不合格窗口若仍有 Surface，也不总是直接跳过。InputMonitor 先挂一份复用的 invalid/overlay Java info：不可触摸、不可聚焦、`NO_INPUT_CHANNEL`、空 Region，同时留下本窗口 type 与可见性线索。不过它的 token 为空，SurfaceFlinger 的 `hasInputInfo()==false` 分支会把最终 flags 改成仅 `NOT_TOUCH_MODAL`，重新写 `NO_INPUT_CHANNEL`、Layer owner 和 display，并用真实 `isVisible()`；空 Region 配合 non-modal 才使它不能成为普通触摸目标。

SurfaceFlinger 还会为非 cursor 的 BufferLayer 生成这类无 token 信息，用于基于 PID 的遮挡判断。这解释了一个看似矛盾的事实：不能成为事件目标的视觉 Layer，仍可能需要出现在输入侧的遮挡模型中；受信任 type、同进程、可见性和 display 等条件随后决定它是否真的算遮挡。不能把 InputMonitor 写入的三项 flags 当作 Dispatcher 最终看到的原样值。

`mDisableWallpaperTouchEvents` 也按 top-to-bottom 遍历累计：上层或当前窗口一旦设置 private flag，后续更低窗口的 `hasWallpaper` 都会被压掉，并非只检查 wallpaper target 自己。drag 进行时，这次遍历还会向刚出现且合格的默认显示窗口补发 `ACTION_DRAG_STARTED`。所以 input-window refresh 不只是纯数据复制，它也是部分 WMS 协议的观察点。

### 练习 4：区分事件目标、input consumer 与遮挡 Layer

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'resetInputConsumers(mInputTransaction);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mDisplayContent.forAllWindows(this,' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'true /* traverseTopToBottom */);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'if (inputChannel == null || inputWindowHandle == null || w.mRemoved' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'populateOverlayInputInfo(mInvalidInputWindow, w.getName(), type, isVisible);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'inputWindowHandle.inputFeatures = INPUT_FEATURE_NO_INPUT_CHANNEL;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mPipInputConsumer.mWindowHandle.replaceTouchableRegionWithCrop(' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mWallpaperInputConsumer.show(mInputTransaction, w);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mService.mDragDropController.sendDragStartedIfNeededLocked(w);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'w.mWinAnimator.mSurfaceController.getClientViewRootSurface(),' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'virtual bool needsInputInfo() const { return hasInputInfo(); }' frameworks/native/services/surfaceflinger/Layer.h
grep -n -F 'bool needsInputInfo() const override { return !mPotentialCursor; }' frameworks/native/services/surfaceflinger/BufferLayer.h
grep -n -F 'mDrawingState.inputInfo.layoutParamsFlags = InputWindowInfo::FLAG_NOT_TOUCH_MODAL;' frameworks/native/services/surfaceflinger/Layer.cpp
```

先把证据分成三个域：InputMonitor 的 `WindowState` 遍历、遍历过程中的 consumer 专门插层、SurfaceFlinger 后续的 Layer 遍历。再把本节对象分为“普通可命中目标、consumer 特殊目标、无 token 遮挡项、完全跳过”四类，并为每类写出 token、flags、Region、Surface 条件。

## 7. scheduled、force 与 immediately 是三种不同控制

`setUpdateInputWindowsNeededLw()` 只置脏位。`updateInputWindowsLw(false)` 在脏位为假时返回；传 `true` 只是绕过这道判断。两者最终都调用 `scheduleUpdateInputWindows()`，由 animation Handler 合并成至多一个 pending Runnable，因此 `force` 不等于同步或立即执行。

Runnable 在 WMS global lock 内先清 pending 与 needed，再遍历窗口。普通路径把专用 `mInputTransaction` merge 到 `DisplayContent.getPendingTransaction()`，merge 会清空来源 transaction，随后 `scheduleAnimation()`；动画帧的 `prepareSurfaces()` 再把 display pending transaction 合进 global transaction 并提交。

`updateInputWindowsImmediately(t)` 的“立即”只表示：移除已排队 callback，当场运行 populate，然后把 `mInputTransaction` merge 到调用者传入的 `t`。内部标志会阻止它再 merge 到 display pending transaction。它仍不自行 `apply()`，何时提交由调用者决定。

最完整的调用者是 `WindowManagerService.syncInputTransactions()`：先尝试等待动画完成，再执行已计划的 surface placement，收集所有 Display 的 immediate 输入事务到一只 `t`，最后执行 `t.syncInputWindows().apply()`。第一步本身只给 5000 ms 总预算；预算耗尽且动画仍在进行时只打印警告，流程会继续进入后续收集。这与稍后 SurfaceFlinger condition wait 的 5 秒单次超时是两个独立边界。

因此几个词的准确边界是：

```text
scheduled    何时开始重建
immediately  现在重建并交给哪只 transaction
sync         transaction apply 返回前等待到输入窗口表已交给 Dispatcher
```

### 练习 5：证明 force、immediate 和 apply 不是同义词

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!force && !mUpdateInputWindowsNeeded) {' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'if (!mUpdateInputWindowsPending) {' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mHandler.post(mUpdateInputWindows);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mUpdateInputWindowsPending = false;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mUpdateInputWindowsNeeded = false;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mDisplayContent.getPendingTransaction().merge(mInputTransaction);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mHandler.removeCallbacks(mUpdateInputWindows);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'mUpdateInputWindowsImmediately = true;' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 't.merge(mInputTransaction);' frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
grep -n -F 'displayContent.getInputMonitor().updateInputWindowsImmediately(t));' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 't.syncInputWindows().apply();' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'private static final int ANIMATION_COMPLETED_TIMEOUT_MS = 5000;' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'long timeoutRemaining = ANIMATION_COMPLETED_TIMEOUT_MS;' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'Slog.w(TAG, "Timed out waiting for animations to complete.");' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

给“脏位真/假 × force 真/假 × 已有 pending/无 pending”做状态表，再标明每格何时 populate、何时 merge、何时才可能跨进程提交。

## 8. setInputWindowInfo 的真正快照点在 JNI 调用时

Java `Transaction.setInputWindowInfo(sc, handle)` 做完 Surface 前置检查后立即进 JNI。`android_view_InputWindowHandle_getHandle()` 为 Java handle 懒创建一只 `NativeInputWindowHandle`，其中只保存 Java 对象弱引用和一份 native `mInfo`。

`nativeSetInputWindowInfo()` 当场调用 `updateInfo()`，逐字段读取 Java：token、name、flags、timeout、frame、scale、Region、状态、owner、features、display、portal、application info、replace 位和 crop handle。随后 `transaction->setInputWindowInfo(ctrl, *handle->getInfo())` 再按值写入该 Surface 的 `layer_state_t`。

所以 Java handle 与最终 Dispatcher handle 之间不是一根可随时回读的指针：

```text
长期 Java handle
  ↕ 弱引用
NativeInputWindowHandle 的临时缓存值
  → Transaction 中的 InputWindowInfo 值
  → SurfaceFlinger Layer drawing-state 值
  → BinderWindowHandle 值
  → Dispatcher 保留或替换的 handle 对象
```

`updateInfo()` 在 Java 弱引用失效时会 `releaseChannel()` 并返回 false；但常规 `setInputWindowInfo()` 调用本身持有 Java 参数的强局部引用，而且该 JNI 调用没有使用返回值来跳过写入。到了 InputManager 侧，`BinderWindowHandle::updateInfo()` 固定返回 true，因为它已经持有跨 Binder 收到的值，不再回读 Java。把这两个同名 `updateInfo()` 当成同一失效机制会误判生产链。

Transaction merge 同样是所有权边界：native `merge(std::move(*other))` 把 composer states 和 sync command 合入目标，并清空来源。它不是让两只 transaction 此后保持联动。

### 练习 6：锁定 Java 字段变成 transaction 值的瞬间

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'nativeSetInputWindowInfo(mNativeObject, sc.mNativeObject, handle);' frameworks/base/core/java/android/view/SurfaceControl.java
grep -n -F 'sp<NativeInputWindowHandle> handle = android_view_InputWindowHandle_getHandle(' frameworks/base/core/jni/android_view_SurfaceControl.cpp
grep -n -F 'handle->updateInfo();' frameworks/base/core/jni/android_view_SurfaceControl.cpp
grep -n -F 'transaction->setInputWindowInfo(ctrl, *handle->getInfo());' frameworks/base/core/jni/android_view_SurfaceControl.cpp
grep -n -F 'mInfo.touchableRegion.clear();' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'mInfo.token = ibinderForJavaObject(env, tokenObj);' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'mInfo.replaceTouchableRegionWithCrop = env->GetBooleanField(obj,' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'mInfo.touchableRegionCropHandle = ctrl->getHandle();' frameworks/base/core/jni/android_hardware_input_InputWindowHandle.cpp
grep -n -F 'transaction->merge(std::move(*otherTransaction));' frameworks/base/core/jni/android_view_SurfaceControl.cpp
grep -n -F 's->what |= layer_state_t::eInputInfoChanged;' frameworks/native/libs/gui/SurfaceComposerClient.cpp
```

在纸上连续两次修改同一 Java handle，但只在第一次修改后调用 `setInputWindowInfo()`；判断最终事务携带哪组字段。再加入一次 `merge()`，标出来源 transaction 何时被清空。

## 9. SurfaceFlinger：从 current state 提交到 drawing state

SurfaceFlinger 应用 `eInputInfoChanged` 时，`Layer::setInputInfo()` 把值放入 current state，解析 crop Binder 为 `Layer` 弱引用，设置 `modified`、`inputInfoChanged` 和 traversal flag。`Layer::doTransaction()` 看见该位后返回 `eInputInfoChanged`，SurfaceFlinger 再把全局 `mInputInfoChanged` 置真。

事务处理结束前，`commitInputWindowCommands()` 把 pending 的 sync 命令移到本帧命令；`commitTransaction()` 则令 Layer drawing state 与本轮 current state 对齐。稍后同一主线程帧流程调用 `updateInputFlinger()`：可见区域变脏或 input info 改变时，它重新导出整份列表。

这里的“整份”是 SurfaceFlinger 此刻认为需要 input info 的 Layer，而不是 InputMonitor 刚遍历的 Java 窗口数组。普通 Layer 的 `needsInputInfo()` 默认要求显式 token；BufferLayer 可覆盖该策略，让无显式输入目标的可见 Layer 也进入遮挡模型。

任何影响最终 Layer 树的变化都可能使输入列表刷新，例如位置、父子关系、可见区域或 Layer 移除，而不一定要求 WMS 再写一遍所有 Java handle。这正是输入信息挂在 Surface transaction 上的价值：窗口语义与本轮 Layer drawing-state 几何在 SurfaceFlinger 汇合。

## 10. fillInputInfo：最终 frame、Region、visible 与 Z 序

`updateInputWindowInfo()` 以 `traverseInReverseZOrder()` 遍历 drawing-state Layer 树。得到的 vector 是前到后顺序，InputManager 在每个 display 的 vector 中保持遇到顺序，Dispatcher 命中和遮挡逻辑也按 front-to-back 使用它。WMS 的 top-to-bottom 遍历有助于插入 consumer，却不是最终顺序的权威。

对每个 Layer，`fillInputInfo()` 先复制 drawing-state info，再写 `id = sequence`。若 display 尚未指定，则以 Layer stack 补齐。随后：

1. 取 Layer transform 的 X/Y scale，更新窗口逆缩放并缩放局部 touchable Region；r48 在这里没有把任意矩阵或 rotation 直接施加到 Region。
2. portal 先用当前 touchable Region 的 bounds 作为 Layer bounds；非 portal 先取 buffer size，无效时回退 cropped buffer size。随后再用完整 transform 计算 screen-space bounds。
3. 把 `surfaceInset` 按 scale 调整、限幅，再向内收缩 bounds，覆盖最终 frame。
4. 把局部 Region 平移到最终 frame 的屏幕位置。
5. 显式 input info 的 visible 取 `canReceiveInput()`，即没有被 policy 隐藏；这为兼容性允许有输入信息的 Layer 在首个 buffer 前接收输入，`fillInputInfo()` 此处也不直接用 alpha 判可见。无显式 token、只用于遮挡的 Layer 才用真实 `isVisible()`。
6. 最后执行 replacement crop 或 intersect crop，clone Layer 还会再裁到 cloned root。

第 4 步的源码注释写着“restrict it to frame bounds”，但紧随的赋值只有 translate，实际限制来自后面的 crop 分支或其他几何。阅读时应以语句而非注释概括行为，不能凭该注释虚构一次与 frame 的求交。

### 练习 7：从 drawing Layer 推导最终输入列表

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mCurrentState.inputInfoChanged = true;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'flags |= eInputInfoChanged;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'commitInputWindowCommands();' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'updateInputFlinger();' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'mDrawingState.traverseInReverseZOrder([&](Layer* layer) {' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'inputHandles.push_back(layer->fillInputInfo());' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'info.id = sequence;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = info.touchableRegion.translate(info.frameLeft, info.frameTop);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.visible = hasInputInfo() ? canReceiveInput() : isVisible();' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = Region(Rect{cropLayer->mScreenBounds});' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = info.touchableRegion.intersect(Rect{cropLayer->mScreenBounds});' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F '} else if (mInputWindowCommands.syncInputWindows) {' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'const bool syncInput = inputWindowCommands.syncInputWindows;' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'while (!isMainThread && (mTransactionPending || mPendingSyncInputWindows)) {' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'status_t err = mTransactionCV.waitRelative(mStateLock, s2ns(5));' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
grep -n -F 'setInputWindowsListener->onSetInputWindowsFinished();' frameworks/native/services/inputflinger/InputManager.cpp
```

构造前后两个 Layer：前层平移并缩放、后层有局部 Region，再为后层添加 intersect crop。按源码顺序计算两个最终 frame/Region，并解释为何遮挡判断必须保留 vector 顺序。

## 11. syncInputWindows 的完成边界

`syncInputWindows()` 只在 transaction 的 `InputWindowCommands` 中置一位并立即返回；必须随后 `apply()`，SurfaceFlinger 才会收到命令、令本次事务需要 traversal，并让非 SF 主线程的调用者在 condition variable 上等待。

若本帧 input info 或 visible regions 有变化，`updateInputWindowInfo()` 把 vector 交给 `IInputFlinger::setInputWindows()`，同时仅在 sync 位为真时附上 listener。InputManager 先同步调用 `InputDispatcher::setInputWindows()`，等它在锁内完成各 display 的替换与清理后，再回调 listener；SurfaceFlinger 清 `mPendingSyncInputWindows` 并唤醒提交者。

若没有任何输入或可见区域变化，SurfaceFlinger 不会无意义地重发列表，但 `else if` 分支仍直接调用 `setInputWindowsFinished()`，从而完成这次屏障。这个无变化分支是理解 sync 不能遗漏的一半。

以下正常完成解释以没有重叠的 `syncInputWindows` 调用为前提。r48 使用全局 `mPendingSyncInputWindows` 布尔值与共享 listener，而不是逐 transaction 的 ack；调用发生重叠时，先前 callback 可能清掉同一 pending 位并唤醒后来的等待者，因而不能建立调用与 Dispatcher 更新一一对应的完成证明。

正常 listener 回调路径中，`t.syncInputWindows().apply()` 返回说明：在它之前排入并被本次 SurfaceFlinger 顺序覆盖的输入状态，已执行到 Dispatcher 的窗口表替换与清理，或者 SF 确认没有需要重发的变化。SF 到 InputFlinger 的调用和 listener 回调都是 oneway，真正让提交者等待的是 SF 内部 condition variable。

这不是无限等待：等待循环的每次 `waitRelative()` 使用 5 秒超时；一旦某次等待超时，代码会清 pending 标志并返回。而且 Dispatcher 可能因 missing connection 或 wrong display 跳过单个 handle。调用者必须同时把超时日志和 native 准入结果视为失败证据。

即使走正常回调，也不能依赖：

- App 已收到、处理或 finish 任一事件；
- 新 buffer 已由 HWC 显示；
- 下一次输入一定命中某个特定 App，因为新事件、display focus 与 channel 生命周期仍可继续变化。

第 239 章 drag 在 `transferTouchFocus()` 前使用该屏障，正是为了先让 Dispatcher 认识 drag handle/channel；它不是为了等待拖影像素 present。

## 12. InputManager 分组与 Dispatcher 的准入校验

SurfaceFlinger 按 reverse-Z traversal 发送一条 `vector<InputWindowInfo>`。InputManager 为每个值新建 `BinderWindowHandle`，按 `info.displayId` 分组并保持组内遇到顺序，再一次调用 Dispatcher。由于外层容器是 unordered map，不同 display 之间没有 Z 序含义；只有每个 display 内的 front-to-back 顺序参与命中。

调用只替换参数中出现的 display。某个 display 的显式空 vector 表示清掉该 display；完全没出现在 map 中的 display 保持原表。显示移除因此另有 `onDisplayRemoved(displayId)`，直接向 Dispatcher 传该 display 的空列表。

Dispatcher 在一把 `mLock` 内处理本次 map 中的各 display。对每个 incoming handle，它先调用 `updateInfo()`；生产主链中的 `BinderWindowHandle` 固定成功。随后有两道关键校验：

- token 找不到已注册 channel 且不是 portal 时，如果窗口仍可能接触摸或键，并且没有 `NO_INPUT_CHANNEL`，该 handle 被跳过。
- `info.displayId` 与正在更新的 map key 不同，handle 被跳过。

native 的 `canReceiveInput` 局部变量使用 `!NOT_TOUCHABLE || !NOT_FOCUSABLE`：只要触摸和焦点能力中至少一种仍开放，就要求 channel。它不是 Java `WindowState.canReceiveKeys()` 的复现。`NO_INPUT_CHANNEL` 与 portal 是保留无 channel 元数据的两个例外，前者可供遮挡，后者把命中递归转到嵌入 display。

`NO_INPUT_CHANNEL` 是无条件绕过 missing-connection 检查，不会自动修正其他 flags 或 Region。若错误地给一份仍可命中的 info 加该 feature，它能通过准入并在 front-to-back 搜索中挡住后窗，稍后添加事件目标时却找不到 Connection。invalid overlay 之所以安全，依赖 SF 最终的 non-modal + 空 Region 组合，而不是 feature 名称本身。

### 练习 8：验证分组、空列表与无 channel 例外

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'std::unordered_map<int32_t, std::vector<sp<InputWindowHandle>>> handlesPerDisplay;' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'handlesPerDisplay[info.displayId].push_back(new BinderWindowHandle(info));' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mDispatcher->setInputWindows(handlesPerDisplay);' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'bool updateInfo() override {' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mInputManager->getDispatcher()->setInputWindows({{displayId, windowHandles}});' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'if (inputWindowHandles.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mWindowHandlesByDisplay.erase(displayId);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!handle->updateInfo()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'info->portalToDisplayId == ADISPLAY_ID_NONE)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'info->inputFeatures & InputWindowInfo::INPUT_FEATURE_NO_INPUT_CHANNEL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '!(info->layoutParamsFlags & InputWindowInfo::FLAG_NOT_TOUCHABLE) ||' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (info->displayId != displayId) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

给“有/无 channel × touchable/focusable × NO_INPUT_CHANNEL × portal”做准入矩阵；再比较 `{display 2: []}` 与完全不含 display 2 的 map 对旧窗口表的不同效果。

## 13. id + token 复用保住进行中状态

InputManager 每次都创建新的 `BinderWindowHandle`，而 Dispatcher 的 focus、hover、`TouchState` 都保存 handle 指针；后续判断既有裸指针比较，也有 `id + token` 存在性比较。复用会维持 hover/`TouchState` 的对象身份，并让进行中状态读到新的 `mInfo`；仅仅换成新对象并不等于窗口移除，移除清理仍按 `id + token` 判定。

`updateWindowHandlesForDisplayLocked()` 先把该 display 的旧 handles 按 `id` 建表。incoming handle 通过校验后，只有旧表存在相同 id 且旧 token 与新 token 相等，才执行 `oldHandle->updateFrom(new)` 并把旧对象放入新 vector；否则采用新对象。

这项设计同时保留对象身份并更新值内容：

```text
id 相同 + token 相同    同一 Layer、同一 connection，复用旧对象并覆盖 mInfo
id 相同 + token 不同    Layer 身份碰巧相同但连接世代变了，采用新对象
id 不同 + token 相同    可能共享 channel，但不是同一输入窗口，采用新对象
```

快照替换是按 display 执行，但 `setInputWindows()` 持锁覆盖本次 map 中的所有 display 后才唤醒 Looper。可以称它为“对本次提交涉及的窗口表做锁内切换”，却不应说它无条件清空并重建所有 display。

## 14. 新快照如何收尾 focus、hover 与 touch

完成 handle 列表更新后，Dispatcher 从 front-to-back 新 vector 中选择第一个 `hasFocus && visible` 的 handle。它以 token 判断焦点是否变化：旧焦点有 channel 时，先发起 `CANCEL_NON_POINTER_EVENTS` 合成尝试，再排入 focus-lost；随后从 display 焦点 map 移除旧值，记录新焦点并排入 focus-gained。只有被更新 display 也是全局 focused display 时，才向 policy 排队通知 token 变化。

“发起合成尝试”不等于必然排出取消事件：channel 之后还要找到 Connection，Connection 不能是 broken，并且其 `InputState` 必须存在符合 mode/device/display 等筛选条件的 memento；否则合成列表为空。focus event 同样只是 enqueue，不是客户端已收到。

`focused application` 是另一条 IMS 调用和另一张 per-display map，提供“已经有前台应用、但其 focused window 尚未出现”时的等待与 ANR 上下文。它不由本轮窗口 vector 自动推导，也不会被 display 窗口空列表一并清成“所有 display 输入状态都不存在”。

这里故意不取消 pointer：一次触摸在 DOWN 时就绑定 touched window，键焦点切换不应截断仍合法的 pointer 流。

hover 的本地收尾更弱。`mLastHoverWindowHandle` 是全局单值，而 `setInputWindowsLocked()` 只在当前正在更新的 display 新表里查它；找不到就清空指针。因此多 display 批次中，先更新一个不含当前 hover 窗口的 display 也可能把该指针清掉。本处没有合成 `HOVER_EXIT`。若某 connection 同时通过其他清理路径收到 pointer cancellation，`InputState` 可以合成相应结束事件，但不能把那件事归因于这行 hover 清空。

进行中的 touched windows 则逐个用 `hasWindowHandleLocked()` 检查 `id + token` 是否仍存在。失效项若还能找到 channel，会发起 `CANCEL_POINTER_EVENTS` 合成尝试；是否真有事件仍取决于 Connection 与匹配 pointer memento。随后无论 channel、Connection 或合成列表是否存在，都从 `TouchState.windows` 删除。最后遍历更新前的 handles：全局再也找不到相同 id+token 的旧对象时调用 `releaseChannel()`。

这个 `releaseChannel()` 只清除旧 handle 的 `mInfo.token`，以尽快释放 Binder 引用；它不等于 `unregisterInputChannel()`，也不关闭 Dispatcher `Connection`。真正的注册撤销和 fd dispose 仍由 WMS/InputConsumer 的 channel 生命周期完成。窗口表、进行中路由状态、registered connection 是三本关联但独立的账。

### 练习 9：推演一次焦点切换和一次触摸窗口消失

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oldHandlesById[handle->getId()] = handle;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'oldHandlesById.at(handle->getId())->getToken() == handle->getToken())) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'oldHandle->updateFrom(handle);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowHandle->getInfo()->hasFocus &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowHandle->getInfo()->visible) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<InputWindowHandle> mLastHoverWindowHandle GUARDED_BY(mLock);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.h
grep -n -F 'mLastHoverWindowHandle = nullptr;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'CancelationOptions::CANCEL_NON_POINTER_EVENTS,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'enqueueFocusEventLocked(*oldFocusedWindowHandle, false /*hasFocus*/);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!hasWindowHandleLocked(touchedWindow.windowHandle)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'CancelationOptions::CANCEL_POINTER_EVENTS,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<Connection> connection = getConnectionLocked(channel->getConnectionToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->status == Connection::STATUS_BROKEN) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->inputState.synthesizeCancelationEvents(currentTime, options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (cancelationEvents.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.windows.erase(state.windows.begin() + i);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'oldWindowHandle->releaseChannel();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInfo.token.clear();' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'void InputDispatcher::setFocusedApplication(' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

用 A、B 两个窗口推演：A 保持同 id+token 但字段更新；B 从新表消失，且拥有 registered、非 broken Connection，`InputState` 中同时有匹配的 pointer 与 non-pointer memento。再令焦点从 B 转到 A，按源码列出对象复用、两类合成尝试、focus event enqueue、TouchState 删除与 token 引用释放的严格顺序；最后去掉两类 memento，解释为何两次尝试都可得到空列表。

## 15. 三条时间线与可观察断点

普通异步刷新：

```text
WMS 状态变化
  → needed=true
  → Handler 合并一次 Runnable
  → top-to-bottom populate / setInputWindowInfo
  → merge 到 Display pending transaction
  → animation prepareSurfaces / global transaction submit
  → SF current→drawing state
  → reverse Z fillInputInfo
  → Dispatcher 锁内替换与清理
```

显式同步刷新：

```text
syncInputTransactions()
  → 尝试等动画完成并补做已计划的 surface placement
  → 所有 Display 当场 populate 并 merge 到同一 t
  → syncInputWindows + apply
  → Dispatcher setInputWindows 返回
  → InputManager listener
  → SF condition variable 被 listener 唤醒
  → 调用者返回
  ↘ 异常支线一：WMS 动画等待总预算 5000 ms，超时告警后继续
  ↘ 异常支线二：某次 condition wait 满 5 秒后清 pending 标志并超时返回
```

显示移除：

```text
InputMonitor 标记 display removed 并取消 pending update
  → animation Handler 上先提交一个 syncInputWindows 屏障
  → 正常回调时把更早的 setInputWindowInfo 排在 cleanup 前
  ↘ 若屏障等待超时，这一步不构成完成证明
  → IMS onDisplayRemoved(displayId)
  → Dispatcher 收到该 display 的空列表
  → 焦点/touch/旧 handle 按普通替换规则收尾
```

调试时按边界找证据：

| 现象 | 首查位置 | 不要过早归因 |
|---|---|---|
| WMS 日志有窗口，Dispatcher 没有 | 是否有 channel/Surface、是否真的 set+merge+apply、native 准入 | 不要只看 Java handle 字段 |
| 视觉移动后命中仍在旧位置 | pending transaction 是否提交、SF scale/translate/crop 后 Region | 不要只看 WMS frame |
| `force=true` 后仍短暂旧表 | Handler 是否尚未运行、动画事务是否尚未提交 | force 不是 sync |
| sync 返回但 App 尚无事件 | 正常回调也只到 Dispatcher；再查是否出现 5 秒超时 | sync 不等待客户端，超时返回更不构成完成证明 |
| 更新后手势被 CANCEL | id/token 是否变化、窗口是否被 native 跳过 | 不要只比较窗口名字 |
| hover 目标消失却无 EXIT | 本路径只清 last-hover 指针 | 不要虚构 focus/touch 共用收尾 |
| display 清理后窗口又出现 | 检查移除前屏障与迟到 transaction 顺序 | 空列表只清指定 display |

建议同时采集 WindowManager、SurfaceFlinger layers/input trace 与 `dumpsys input`。单独一份 dump 只代表各自采样时刻，不能证明跨进程状态属于同一轮 transaction。

## 16. r48 边界、本章结论与下一章

Android 11 r48 的关键结论可以压缩为八句：

1. `WindowState` 持有长期可变的 Java handle，InputChannel token 却有独立开闭区间。
2. `setInputWindowInfo()` 的字段值在 JNI 调用时复制，不在 transaction apply 时回读 Java。
3. 普通 modal 语义被改写成 `NOT_TOUCH_MODAL + 显式 Region`，再交给 Layer scale、translate 与 crop。
4. needed/force 决定是否跨过脏位门，scheduled 负责 Handler 合并，immediate 决定事务归属，sync 才尝试建立有超时逃生的完成屏障。
5. 输入元数据和 Surface 状态可在同一 transaction 中提交；“同帧”不等于屏幕 present 屏障。
6. SurfaceFlinger 以 drawing Layer 树确定最终 id、frame、visible、Region 和 display 内 front-to-back 顺序；Region 只走 r48 明示的 scale、translate 与 crop，不套用任意旋转矩阵。
7. InputManager 每轮按 display 新包装值，Dispatcher 以 `id + token` 复用旧对象并只替换出现的 display。
8. focus、hover、touch 和 channel 引用各有不同的清理动作；其中 hover 本路径只清指针，handle release 也不注销 connection。

本章在无重叠 sync 调用且未超时的前提下，正常完成点不是“Java 字段已经填好”，而是：承载这些字段的 Surface transaction 已被 SurfaceFlinger 纳入 drawing state，最终列表已经在 Dispatcher 锁内尝试准入并完成替换，失效的进行中状态也按各自协议收尾。重叠调用造成的共享 listener 串扰、WMS 与 SF 两处 5 秒超时路径，以及被 native 跳过的 handle，必须作为这一定义的显式例外。

下一章进入 `WindowState` 的 touchable insets、Region 运算、Task/Stack crop、exclude 区域与 Surface 坐标变换，建立从 App 局部坐标到 Display 命中坐标的完整几何模型。
