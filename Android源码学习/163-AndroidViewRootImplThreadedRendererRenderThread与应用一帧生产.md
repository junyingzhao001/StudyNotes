# 163 Android ViewRootImpl、ThreadedRenderer、RenderThread 与应用一帧生产

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 11、12、21、160—162 章

---

## 1. “一帧完成”至少有五种含义

`View.invalidate()` 之后，应用的一帧不会从 Java 调用直接跳到屏幕。先建立五个完成点：

| 完成点 | 已经发生 | 还不能推出 |
|---|---|---|
| UI record | View 绘制命令已写入 RenderNode | GPU 已执行 |
| RenderThread sync | staging 状态已同步到 RT 使用的树 | draw/swap 已结束 |
| producer queue | buffer 与 producer-done fence 已提交 | fence 已 signal、SF 已采用 |
| SurfaceFlinger latch | 某个合成周期选中了新 buffer | HWC 已 present |
| present | 合成结果进入显示管线，由 present fence/时间戳描述 | 人眼在同一纳秒感知 |

本章主链：

```text
invalidate / requestLayout
  → scheduleTraversals
  → Choreographer traversal
  → measure/layout（按需）
  → UI 线程录制 RenderNode
  → syncAndDrawFrame
  → RenderThread prepare/draw
  → Surface dequeue/queue
  → BufferQueue 或 BLAST Transaction
  → SurfaceFlinger 选帧/latch
  → 合成/present
```

六个名字尤其容易误导：

```text
View.draw()             硬件路径主要在 UI 线程录命令
DisplayList             不是 bitmap，也不是 GraphicBuffer
syncAndDrawFrame()      默认不等屏幕显示
FrameCompleteCallback  不等于 present callback
eglSwapBuffers()        不等于 SF 已 latch
BLAST                   不是另一种 GPU renderer
```

本章的判断原则是：

> 每看到一个“完成、提交、帧、fence”，先确认它属于 UI、RenderThread、producer、SurfaceFlinger 还是显示设备。

---

## 2. 源码、进程与线程地图

主线文件：

```text
frameworks/base/core/java/android/view/
├── View.java
├── ViewRootImpl.java
├── Choreographer.java
└── ThreadedRenderer.java

frameworks/base/graphics/java/android/graphics/
├── HardwareRenderer.java
├── RenderNode.java
├── RecordingCanvas.java
└── BLASTBufferQueue.java

frameworks/base/libs/hwui/
├── jni/android_graphics_HardwareRenderer.cpp
├── renderthread/
│   ├── RenderProxy.cpp
│   ├── DrawFrameTask.cpp
│   ├── CanvasContext.cpp
│   ├── ReliableSurface.cpp
│   ├── EglManager.cpp
│   └── VulkanManager.cpp
└── pipeline/skia/
    ├── SkiaOpenGLPipeline.cpp
    └── SkiaVulkanPipeline.cpp

frameworks/native/libs/gui/
├── Surface.cpp
├── BufferQueueProducer.cpp
└── BLASTBufferQueue.cpp

frameworks/native/services/surfaceflinger/
├── SurfaceFlinger.cpp
├── BufferLayer.cpp
├── BufferQueueLayer.cpp
└── BufferStateLayer.cpp
```

线程边界：

| 阶段 | 典型进程 | 典型执行上下文 |
|---|---|---|
| traversal、View record | 窗口客户端 | main/UI thread |
| RenderNode sync、Skia、swap | 同一客户端进程 | RenderThread |
| BLAST 本地消费 | 同一客户端进程 | queue/callback 上下文 |
| Transaction、latch、composition | `surfaceflinger` | SF 主线程及工作线程 |
| GPU/HWC/scanout | driver、composer、硬件 | 异步时间线 |

`RenderProxy` 不是远端 Binder 服务，只是 UI 线程操作应用进程内 RenderThread/`CanvasContext` 的 native 代理。Java→JNI 和 UI→RT 是两个边界；直到 Surface/Transaction 进入 SF，才跨到显示服务进程。

---

## 3. scheduleTraversals：合并请求并让 traversal 越过屏障

`invalidate()` 与 `requestLayout()` 不是同一个请求：

```text
invalidate()      内容或视觉属性脏，通常需要 draw
requestLayout()   尺寸/位置可能变化，需要 measure/layout，随后可能 draw
```

它们都可能走到：

```java
void scheduleTraversals() {
    if (!mTraversalScheduled) {
        mTraversalScheduled = true;
        mTraversalBarrier =
                mHandler.getLooper().getQueue().postSyncBarrier();
        mChoreographer.postCallback(
                CALLBACK_TRAVERSAL, mTraversalRunnable, null);
        notifyRendererOfFramePending();
        pokeDrawLockIfNeeded();
    }
}
```

这里同时解决三件事：

1. `mTraversalScheduled` 把同一轮内的多次请求合并；
2. traversal 对齐 Choreographer 的下一次 frame pulse；
3. sync barrier 暂时挡住屏障后的普通同步 Message，给异步帧消息优先机会。

Choreographer 安排 `MSG_DO_FRAME` 时调用 `setAsynchronous(true)`，所以能越过 barrier。屏障不是“冻结主线程”：

```text
屏障前消息       仍可执行
屏障后同步消息   暂缓
屏障后异步消息   可通过
native/GPU 工作  不由这个 Java MessageQueue 屏障控制
```

`doTraversal()` 先清标志、移除 barrier，再调用 `performTraversals()`。若 barrier 遗留，受影响的是后续同步消息，不只是 UI 绘制。

---

## 4. Choreographer 定节拍，performTraversals 按状态选工作

Android 11 r48 一次 `doFrame()` 的顺序是：

```java
doCallbacks(CALLBACK_INPUT, frameTimeNanos);
doCallbacks(CALLBACK_ANIMATION, frameTimeNanos);
doCallbacks(CALLBACK_INSETS_ANIMATION, frameTimeNanos);
doCallbacks(CALLBACK_TRAVERSAL, frameTimeNanos);
doCallbacks(CALLBACK_COMMIT, frameTimeNanos);
```

直觉是先吸收输入，再推进动画与 Insets，然后用最新状态 traversal，最后执行 commit callbacks。

如果 UI 真正开始时间比 VSync 时间晚超过一个刷新周期，`doFrame()` 用：

```text
jitter = startNanos - intended frameTime
skippedFrames = jitter / frameInterval
new frameTime = startNanos - jitter % frameInterval
```

把逻辑帧时间移到最近的有效周期。“Skipped N frames”是时间差估算，不表示系统保存了 N 张图片。

r48 还有第二次时间修正：到 `CALLBACK_COMMIT` 时若已迟到至少两个周期，只给 commit 阶段重新计算 frame time，并更新 `mLastFrameTimeNanos`。所以不能假设一帧所有阶段在极端迟到时观察到的时间处理完全相同。

`performTraversals()` 不是固定三连：

```text
layout requested / 窗口尺寸或参数变化
  → 可能 measure + layout

内容脏 / 动画 / accessibility focus 变化
  → 可能 draw

Surface 无效、显示关闭、View 不可见
  → draw 可跳过

OnPreDrawListener 返回取消
  → 本轮不 draw；可见时重新 scheduleTraversals
```

因此，在 `OnPreDrawListener` 中持续取消仍会反复产生 traversal 成本，并非免费的“暂停绘制”。

---

## 5. ViewRootImpl.draw：先确认 Surface 和渲染分支

`performDraw()` 会处理首次绘制上报、frame commit callback 和 BLAST 同步事务，再调用 `draw(fullRedrawNeeded)`。真正进入硬件 renderer 还要满足：

```text
Surface 有效
且 dirty 非空 / 正在动画 / accessibility focus 改变
且 ThreadedRenderer 存在并已 enabled
```

随后：

```java
mThreadedRenderer.draw(mView, mAttachInfo, this);
```

但有三条不能混用的分支。

### RootViewSurfaceTaker 接管

若 `mSurfaceHolder != null`，`ViewRootImpl.draw()` 直接清 dirty 并返回，因为根 View 通过内部 `RootViewSurfaceTaker` 协议接管了窗口 Surface。

这不等于“布局里有普通 `SurfaceView` 就不画根窗口”。`SurfaceView` 通常有独立子 Surface，其业务像素不进根 DisplayList，但 ViewRootImpl 仍可绘制窗口里的其他 View。

### 软件渲染

没有可用硬件 renderer 时，软件路径在 UI 线程 `lockCanvas(dirty)`、执行 View 绘制，再 `unlockCanvasAndPost()`。如果 renderer 是“已请求但暂时 disabled”，r48 会先尝试重新初始化；失败路径不会贸然锁软件 Canvas，以免妨碍硬件 Surface 恢复。

所以“View.draw 只录命令”必须限定在硬件加速窗口路径。

### r48 的 dirty offset 不对称

`ViewRootImpl.draw()` 先计算：

```java
xOffset -= surfaceInsets.left;
yOffset -= surfaceInsets.top;
```

但移动 dirty rect 时写的是：

```java
dirty.offset(surfaceInsets.left, surfaceInsets.right);
```

纵向使用 `right` 而不是前面对应的 `top`。从本文件能确认的是这两处参数不一致；在非对称 surface insets 下可能改变脏区位置。静态审计应保留这个 r48 实现事实，不能擅自把源码抄成更“合理”的 top。

---

## 6. UI 线程产出 RenderNode 命令图

`ThreadedRenderer.draw()` 先：

```java
updateRootDisplayList(view, callbacks);
```

再：

```java
syncAndDrawFrame(choreographer.mFrameInfo);
```

第一步仍在 UI 线程。`RecordingCanvas` 记录的是：

```text
drawRect / drawText / drawBitmap
clip / matrix / alpha
draw child RenderNode
```

DisplayList 不是 bitmap、BufferQueue slot 或已栅格化 GraphicBuffer。

### 一个 View 有三种更新结果

`View.updateDisplayListIfDirty()` 的核心分支：

1. 当前节点需重录：`beginRecording → draw/dispatchDraw → endRecording`；
2. 当前节点 DisplayList 可复用，但调用 `dispatchGetDisplayList()` 让子节点恢复或更新；
3. 当前节点与子树均无需工作，直接保留已有 RenderNode。

`ThreadedRenderer.updateViewTreeDisplayList()` 先把根 View 的 invalidated 状态折叠到 `mRecreateDisplayList`，更新子树后，只有 `mRootNodeNeedsUpdate` 或 root 尚无 DisplayList 时才重录 renderer 自己的根节点：

```text
ThreadedRenderer root
  → onPreDraw commands
  → DecorView RenderNode
      → child RenderNodes
  → onPostDraw commands
```

父 DisplayList 可以引用子 RenderNode；子内容变化不要求每个祖先都重录完整内容。

### 属性动画为何常不重跑 onDraw

translation、scale、rotation、alpha 等属性可存在 RenderNode 属性中。内容命令不变时只需同步属性；文字或自定义内容变化通常要 invalidate 并重录。

```text
translationX 改变   常只改 RenderNode 属性
TextView 文本改变   可能 requestLayout + 重录内容
自定义 onDraw 改变  需要正确 invalidate
```

因此，“硬件加速后 `onDraw()` 在 RenderThread 执行”是错误模型：普通 View 的 `draw/onDraw` 录制在 UI 线程，RT 回放命令并组织 Skia/GPU 工作。

---

## 7. syncAndDrawFrame：UI 等的是状态交接

Java 经 JNI 调到 `RenderProxy`，再使用 `DrawFrameTask`：

```cpp
int DrawFrameTask::drawFrame() {
    mSyncResult = SyncResult::OK;
    mSyncQueued = systemTime(SYSTEM_TIME_MONOTONIC);
    postAndWait();
    return mSyncResult;
}
```

`postAndWait()` 在持有 `mLock` 时把任务放到 RenderThread 队列，然后等待 `mSignal`。它不是 fire-and-forget。

RT 执行顺序：

```cpp
canUnblockUiThread = syncFrameState(info);
canDrawThisFrame = info.out.canDrawThisFrame;

if (canUnblockUiThread) unblockUiThread();

if (canDrawThisFrame) context->draw();
else context->waitOnFences();

if (!canUnblockUiThread) unblockUiThread();
```

所以 UI 至少等到本帧状态同步完成；正常时可在 RT 真正 draw/swap 前返回。

### 提前放行条件不是笼统的“RT ready”

`syncFrameState()` 的返回值就是：

```cpp
return info.prepareTextures;
```

`prepareTextures` 在 full traversal 初始为 true；若为 mutable images 预固定 GPU cache 失败，会设 false 并 unpin 已固定图片。此时 UI 要继续阻塞到本次 draw 或 `waitOnFences()` 结束，避免 UI 与 RT 对可变资源的使用重叠。

因此：

- `canDrawThisFrame == false` 不必然阻止 UI 提前放行；
- 真正决定早放行的是 `prepareTextures`；
- 方法返回不表示 GPU、queue、latch 或 present 完成。

`syncFrameState()` 同时会更新时间基准、`makeCurrent()`、应用 pending layers、设置 content bounds、`prepareTree()`，并生成 LostSurface、ContextStopped、UIRedrawRequired、FrameDropped 等结果位。Java 据此请求 relayout 或 invalidate。

---

## 8. prepareTree 决定能否画，RT-only 动画可独立推进

`CanvasContext::prepareTree()` 本身会：

```text
导入 UI FrameInfo
  → 启动动画上下文
  → 遍历 RenderNodes
  → 同步 staging 属性/DisplayList
  → 累计 damage
  → pin mutable images
  → 判断 Surface 与本周期能否 draw
  → 安排后续 RT frame callback
```

它自身可因下列条件把 `canDrawThisFrame` 设为 false：

- 没有 Surface；
- 同一 VSync 附近 RT 动画已画过，UI 请求迟到不到 2 ms；
- 多 RenderNode/backdrop 组合中的指定节点还不可 render。

在外围 `syncFrameState()` 中，`makeCurrent()` 发现 context stopped 或 Surface 失败也会禁止绘制。

被跳过时 `syncResult` 可带 `FrameDropped`，但 RenderThread 仍会等待本帧登记的异步 futures，避免工作跨到下一帧。

### reserveNext 在 r48 实际不预取

`prepareTree()` 调用 `mNativeSurface->reserveNext()`，名称像提前 dequeue；但 r48：

```cpp
constexpr bool DISABLE_BUFFER_PREFETCH = true;

int ReliableSurface::reserveNext() {
    if constexpr (DISABLE_BUFFER_PREFETCH) {
        return OK;
    }
    ...
}
```

所以该版本的调用总是直接成功，真正 dequeue 仍由 GL/Vulkan 取帧路径触发。调用点的意图不能替代被编译进版本的实际分支。

### MODE_RT_ONLY 的能力边界

RenderThread 自己收到 frame callback 时可用 `MODE_RT_ONLY` 更新动画并 draw，使部分 RenderNodeAnimator 在 UI 忙时继续推进。

这不覆盖：

- 需要业务代码或 layout 的动画；
- 需要重录 View 内容的变化；
- 输入与应用逻辑；
- Surface、GPU、BufferQueue 或 SF 的后段拥堵。

“可由 RT 驱动”与“整个应用完全不受主线程影响”不是同一结论。

---

## 9. CanvasContext::draw：damage、future 与 swap

RT 真正绘制的骨架：

```cpp
mDamageAccumulator.finish(&dirty);
Frame frame = mRenderPipeline->getFrame();
setPresentTime();
SkRect windowDirty = computeDirtyRect(frame, &dirty);
bool drew = mRenderPipeline->draw(...);
int64_t frameCompleteNr = getFrameNumber();
waitOnFences();
bool didSwap = mRenderPipeline->swapBuffers(...);
```

`computeDirtyRect()` 结合：

```text
本帧 RenderNode damage
新 Surface / 尺寸变化
buffer age
历史 swap damage
preserve / buffer-age 策略
```

若取到的旧 back buffer 已错过若干局部更新，就把对应历史 damage 并入本帧。它与 BufferQueue drop 时合并 damage 解决的是同一类问题：局部更新不能因 buffer 轮换而丢失。

### waitOnFences 不是 sync_file fence

`CanvasContext::mFrameFences` 的类型是：

```cpp
std::vector<std::future<void>>
```

`enqueueFrameWork()` 用 `CommonPool::async()` 加入工作，`waitOnFences()` 对每个 future 调 `get()`。来源包括 frame callback 和部分 RenderNode functor 工作。

它不是 `android::Fence`，没有 sync-file fd，也不是第 162 章的 buffer acquire/release fence。这里只是同名。

### 空 damage 路径比普通完成更早返回

若：

```text
dirty 为空
Properties::skipEmptyFrames 为 true
Surface 不要求重画
```

`draw()` 会标记 `SkippedFrame`，调用并清空 frame-complete callbacks，然后直接 return：

- 不 `getFrame()`；
- 不画；
- 不 `waitOnFences()`；
- 不 swap；
- 还没走到后面的 `markFrameCompleted()` 与常规 jank finish。

回调仍被调用是为了避免上层无限等待，不是证明新 buffer 已提交；本帧已排入 CommonPool 的异步 work 也可能到后续路径才被等待。若 GL 的 damage/preserve 策略要求 swap，`surfaceRequiresRedraw()` 会阻止这个空帧早退。

普通路径也不是“draw 返回 true就必有成功新帧”：pipeline 的 `requireSwap`、swap 错误和 ReliableSurface 保存的错误还会改变结果。

---

## 10. SkiaGL、SkiaVulkan 与 ReliableSurface

`CanvasContext::create()` 根据 `Properties::getRenderPipelineType()` 选择：

```text
SkiaOpenGLPipeline
或
SkiaVulkanPipeline
```

`debug.hwui.renderer=skiavk` 选 Vulkan；否则走 SkiaGL，默认值还会参考 build-time `use_vulkan()`。选择一旦锁定，运行中试图改成另一种会 fatal。

公共骨架是：

```text
RenderNode tree
  → Skia renderFrame
  → ANativeWindow-backed frame
  → GPU submit
  → queue/present current buffer
```

分叉点：

| SkiaGL | SkiaVulkan |
|---|---|
| EGLSurface、FBO 0 | VulkanSurface、swapchain image |
| `eglSwapBuffersWithDamageKHR()` | `presentCurrentBuffer(dirty, fenceFd)` |
| `damageRequiresSwap()` 可要求空 damage 也 swap | `requireSwap = drew` |
| EGL sync/driver 路径 | semaphore 导入/导出 sync fd |

因此本章用 GL 函数名说明主线时，不能推出 Android 硬件加速只有 OpenGL 后端。

### ReliableSurface 的兜底不是成功显示

HWUI 用 `ReliableSurface` 包装 ANativeWindow 并安装 dequeue/queue/cancel/perform/query interceptors。真实 dequeue 失败时：

```cpp
*buffer = acquireFallbackBuffer(result);
*fenceFd = -1;
return *buffer ? OK : INVALID_OPERATION;
```

fallback 是本地 `1 × 1` scratch AHardwareBuffer。后续对 scratch 的 queue/cancel 在 wrapper 内被吞掉；真实 BufferQueue 错误保存在 `mBufferQueueState`。

`CanvasContext` swap 后调用 `getAndClearError()`：

```text
TIMED_OUT
  → post 后续 RT frame callback 重试

其他错误或 didSwap=false
  → setSurface(nullptr)
  → 上层通过 lost-surface/relayout 恢复
```

所以底层 EGL/Vulkan 一段看似成功，可能只是兜底目标让渲染栈完成收尾。

r48 还有一个 fd 边界：scratch 的 cancel/queue 仅在 `fenceFd > 0` 时 close；合法 fd 0 不会被关闭。通常图形进程不会把标准 fd 槽位空出来，但源码条件确实不是 `>= 0`，应把它记录为罕见泄漏窗口，而非假装不存在。

---

## 11. swap 只走到 producer 提交边界

SkiaGL：

```cpp
*requireSwap = drew || mEglManager.damageRequiresSwap();
if (*requireSwap &&
        !mEglManager.swapBuffers(frame, screenDirty)) {
    return false;
}
```

`EglManager::swapBuffers()` 最终调用 `eglSwapBuffersWithDamageKHR()`。EGL 的 native-window bridge 再走 Surface/BufferQueue 的 dequeue、queue 与 fence 协议。

SkiaVulkan 则 dequeue native buffer，把 dequeue fence 导入 Vulkan semaphore；无法 dup 时会同步 `sync_wait`。提交后尝试导出 semaphore fd，并把它交给 `presentCurrentBuffer()`。

抽象上两者都完成：

```text
取得可写 buffer + 等待 consumer-done 条件
  → 提交 GPU 工作
  → queue buffer + producer-done 条件
```

但 swap 返回不能推出：

- GPU 命令已经物理完成；
- producer-done fence 已 signal；
- SurfaceFlinger 已获取或 latch；
- HWC 已 present；
- 屏幕已经 scanout。

GL 的 `EGL_BAD_SURFACE/EGL_BAD_NATIVE_WINDOW` 会返回 false 供上层丢弃 Surface；其他 EGL 错误在 r48 直接 fatal。Vulkan 的 semaphore create/export/submit 也有各自日志或等待恢复路径，不能只看统一的 `didSwap` 布尔值猜底层原因。

---

## 12. BLAST：本地消费后把 buffer 填进 Transaction

`ViewRootImpl.setView()` 给窗口参数加 `PRIVATE_FLAG_USE_BLAST`，但最终只有 WMS 返回 `ADD_FLAG_USE_BLAST` 才置 `mUseBLASTAdapter=true`；`mForceDisableBLAST` 还能覆盖它。因此不能把 Android 11 的每个窗口都默认画成 BLAST。

启用时，`getOrCreateBLASTSurface()` 创建：

```java
new BLASTBufferQueue(
        mBlastSurfaceControl,
        width,
        height,
        mEnableTripleBuffering);
```

renderer 仍拿到普通 `Surface`。native BLAST 在窗口客户端进程内创建自己的 producer/consumer：

```cpp
BufferQueue::createBufferQueue(&mProducer, &mConsumer);
mProducer->setDequeueTimeout(INT64_MAX);
if (enableTripleBuffering) {
    mProducer->setMaxDequeuedBufferCount(2);
}
```

巨大正 timeout 让本地 adapter 的 producer 走阻塞式 dequeue；triple buffering 把 `mMaxDequeuedBufferCount` 配成 2。第 162 章所述“首次 queue 前不执行 max-dequeued 检查”的兼容例外仍然存在，不能把配置值误当成所有时刻都由同一分支硬限制。

`onFrameAvailable()` 增加 shadow 计数并调用 `processNextBufferLocked()`。后者最多持有 `MAX_ACQUIRED_BUFFERS + 1 == 2` 张，从本地 BufferQueue acquire 后写 Transaction：

```cpp
t->setBuffer(surfaceControl, buffer);
t->setAcquireFence(surfaceControl, dup(bufferItem.mFence));
t->setFrame(surfaceControl, ...);
t->setCrop(surfaceControl, ...);
t->setTransform(surfaceControl, ...);
t->setDesiredPresentTime(bufferItem.mTimestamp);
t->addTransactionCompletedCallback(...);
```

于是 buffer、acquire fence、几何和 desired-present time 作为同一 Layer 状态进入 SurfaceFlinger。BLAST 没有删除 BufferQueue，也没有绕过 SF 直接调用 HWC。

### setNextTransaction 与 release 为什么都要延后

首次绘制或窗口几何同步时，ViewRoot 可先：

```java
mBlastBufferQueue.setNextTransaction(
        mRtBLASTSyncTransaction);
```

下一张 buffer 到来后，BLAST 把操作写入指定 Transaction，不立即 `apply()` 自己的 local Transaction。这样 buffer 可与 WMS/ViewRoot 已准备的其他 SurfaceControl 状态原子同行。

BLAST 自己是本地 BufferQueue consumer，不能在交给 SF 后立刻 release。transaction callback 的处理是“落后一张”：

```text
callback(Tn)
  → 用 Tn stats.previousReleaseFence
     release 先前 pending item
  → 把 Tn 对应 submitted item
     变成新的 pending release
```

当前显示 buffer 要等后续 Transaction 带回它的 previous-release fence 才能归还；若没有下一帧，它仍可能是 SF 当前使用的 buffer，继续持有是正确的。

---

## 13. SurfaceFlinger 的两条接收路径不能混画

传统路径：

```text
App Surface producer
  → SurfaceFlinger 内的 BufferQueueLayer consumer
  → SF shadow queue
  → handlePageFlip / latchBuffer
```

BLAST 路径：

```text
App Surface producer
  → 客户端 BLAST consumer
  → SurfaceControl Transaction
  → SF BufferStateLayer state
  → transaction apply / latchBuffer
```

两者最后都进入 Layer 合成，但“时间戳和 acquire fence 在哪里拦住帧”并不相同。

### 传统 BufferQueueLayer

`onFrameAvailable()` 先把 `BufferItem` 放进 SF 的 shadow queue、增加 `mQueuedFrames`，再 `signalLayerUpdate()`。它不会在 producer callback 中直接合成。

为保持 callback 顺序，r48 会等期望的连续 frame number；但每次最多等 500 ms，超时只记录并继续 push。这是“尽量保序并有超时退出”，不是绝对永不乱序的证明。

`handlePageFlip()` 遍历 drawing state：

```cpp
if (layer->hasReadyFrame() &&
        layer->shouldPresentNow(expectedPresentTime)) {
    mLayersWithQueuedFrames.push_back(layer);
}
```

`BufferQueueLayer::shouldPresentNow()` 对普通头帧使用严格 `timestamp < expectedPresentTime`；超过 expected 但不到未来 1 秒会推迟，更远的异常时间戳反而被视为 implausible 并允许继续。

之后 `BufferLayer::latchBuffer()` 还要检查 refresh pending、fence、local sync points，再执行 `updateTexImage()`。

### “fence 未 signal 就绝不 latch”在 r48 不成立

`BufferQueueLayer::fenceHasSignaled()` 有两个绕过条件：

```cpp
if (latchUnsignaledBuffers()) return true;
if (mQueueItems[0].mIsDroppable) return true;
```

`debug.sf.latch_unsignaled` 默认 0，但首次读取后静态缓存；而 droppable 头帧即使 fence pending 也会被允许 acquire/latch，目的是避免它不断被新帧替换而永远无法 latch。后续纹理/合成仍携带 acquire fence，并非假装 GPU 工作已完成。

当头帧已 signal、后面候选未 signal 时，`BufferQueueLayer::updateTexImage()` 会计算从队头开始连续已 signal 的最后 frame number，并作为 `maxFrameNumber` 传给 consumer，避免通用 `BufferQueueConsumer` 的时间戳 drop 跳到未 ready 的后帧。这是第 162 章 generic TODO 在 SF 调用层上的补充保护。

### BLAST / BufferStateLayer

BLAST Transaction 到 SF 时，`transactionIsReadyToBeApplied()` 已检查：

```text
desiredPresentTime 尚未到且不超过未来 1 秒
  → Transaction 留在 apply-token 队列

Transaction 中 changed acquire fence 为 Unsignaled
  → Transaction 留队
```

等 Transaction 真正应用后，`BufferStateLayer::shouldPresentNow()` 在 r48 只返回 sideband/auto-refresh/`hasFrameUpdate()`，不再比较 `expectedPresentTime`。也就是说 BLAST 的 desired-present 和初始 fence gate 主要前移到了 Transaction apply 阶段。

这正是不能把传统 BufferQueueLayer 的 queue/latch 判断原样套到 BufferStateLayer 的原因。下一章会继续展开 Transaction current/drawing state 与同步点。

---

## 14. 三种 callback 与 HardwareRenderer.fence 的边界

`ViewRootImpl.performDraw()` 在以下情况设置 frame-complete callback：

```text
BLAST sync transaction
存在 ViewTreeObserver frame commit callbacks
WMS 要求 report-next-draw
```

`CanvasContext` 在 `didSwap == true` 后调用这些 callbacks；空 damage 早退也主动调用。普通 swap 后紧接着还有：

```cpp
// TODO: Use a fence for real completion?
mCurrentFrameInfo->markFrameCompleted();
```

所以三者要分开：

| 名称 | r48 能证明什么 |
|---|---|
| frame-complete callback | 本次 HWUI 路径已 swap，或空帧被有意完成 |
| FrameInfo `FrameCompleted` | RT 在该代码点完成 CPU 侧帧记账，源码自己提示缺真实 fence |
| SF present fence/time | 合成提交/显示时间线的后段证据 |

`ViewTreeObserver.registerFrameCommitCallback()` 的文档也明确说“已提交 swap chain，但可能尚不可见”。空帧特例又说明实现为避免死等，会比这段日常描述更宽。

### HardwareRenderer.fence 只清 RenderThread 队列

r48 实现：

```cpp
void RenderProxy::fence() {
    mRenderThread.queue().runSync([]() {});
}
```

它等待队列运行到这个空任务，因此能等到此前 `DrawFrameTask::run()` 返回，通常覆盖 draw/swap API 调用；它不直接等待 GPU fence、SF latch 或 HWC present。

`FrameRenderRequest.setWaitForPresent(true)` 的注释使用了“presented”字样，但同一段又把返回保证收窄为“submitted to Surface”，实际只调用上述 `fence()`。应以实现边界理解为提交/RT 队列完成，不把它升级成屏幕 present fence。

### finishDrawing 是窗口协议回执

首次显示或 WMS 请求 redraw 时，ViewRoot 最终调用：

```java
mWindowSession.finishDrawing(
        mWindow, mSurfaceChangedTransaction);
```

它让 WindowManager 推进窗口展示、转场和 Surface 状态；不是面板扫描完成证明。远端异常还被捕获忽略，因此本地流程结束也不保证服务端一定成功收到。

---

## 15. 用一帧时序和诊断问题闭环

普通硬件加速窗口的一帧可按以下顺序推演：

```text
App UI thread
  1. invalidate/requestLayout 合并
  2. Choreographer input→animation→traversal
  3. measure/layout 按需执行
  4. 脏 View 录制 DisplayList
  5. post DrawFrameTask，并等待 RT sync

App RenderThread
  6. sync staging tree、pin images、计算 damage
  7. 条件允许时先 unblock UI
  8. GL/Vulkan dequeue buffer
  9. 回放 RenderNodes、提交 GPU 命令
 10. swap/queue buffer + acquire fence

Buffer transport
 11a. 传统：直接进入 SF BufferQueueLayer
 11b. BLAST：客户端先 acquire，再写 SurfaceControl Transaction

SurfaceFlinger
 12. Transaction apply 或 frame-available 唤醒
 13. 本周期选择并 latch
 14. client/device composition
 15. HWC present，随后借 release/present fence 闭合复用
```

诊断时不要先问“谁掉帧”，而要逐段找证据：

| 现象 | 优先核对 |
|---|---|
| UI traversal 长 | measure/layout、业务 callback、View record、锁、GC |
| UI 等 DrawFrameTask 长 | RT 队列、prepareTree、mutable image pin 失败、特殊延迟放行 |
| RT draw 长 | RenderNode/Skia 复杂度、纹理上传、shader、GPU 压力 |
| dequeue 长 | 无 free buffer、consumer 慢、release fence |
| swap/queue 长 | producer 背压、driver、Surface 错误 |
| queue 后迟迟无新状态 | BLAST 本地 acquire、Transaction desired time/acquire fence |
| SF 有帧但未 latch | shouldPresent、refresh pending、sync point、fence |
| latch 后 present 慢 | client composition、HWC validate/present、显示管线 |

几个静态审计问题尤其有效：

```text
1. 当前是软件 Canvas，还是 RecordingCanvas？
2. 此次 traversal 真执行了哪些 measure/layout/draw 分支？
3. 哪些 RenderNode 重录，哪些只同步属性？
4. UI 在 syncFrameState 后早放行，还是等到 draw 后？
5. prepareTextures 为什么为 false？
6. 本帧空 damage 早退、didSwap，还是 Surface error？
7. 后端是 SkiaGL 还是 SkiaVulkan？
8. ReliableSurface 是否已切到 scratch fallback？
9. 窗口由 WMS 真正启用了 BLAST 吗？
10. 帧卡在 BQ shadow queue、Transaction queue、latch 还是 present？
```

### 一个例子：平移并修改 TextView 文本

```java
textView.setTranslationX(20f);
textView.setText("new");
```

合理推演是：

1. translation 更新 RenderNode 变换属性；
2. 文本变化可能触发 requestLayout 与 invalidate；
3. 多次请求被 `mTraversalScheduled` 合并；
4. 下一 traversal 按需 measure/layout；
5. TextView 内容 DisplayList 重录，不相关子树继续复用；
6. RT 同步新属性和命令，结合 buffer age 扩大最终 damage；
7. buffer queue 后仍由 BLAST/SF/HWC 的后半段决定能否赶上目标刷新。

这比“invalidate 让 GPU 重新画整屏”更接近真实代码。

---

## 16. 结论：record、draw、queue、latch、present 分层

本章最需要记住的十二条边界：

1. `scheduleTraversals()` 用标志合并请求，以 Choreographer 对齐帧节拍。
2. sync barrier 只拦屏障后的同步 Message；异步帧消息可以通过。
3. `performTraversals()` 按状态选择 measure/layout/draw，pre-draw 还可取消本轮。
4. 硬件路径的 View `draw/onDraw` 主要在 UI 线程向 RecordingCanvas 录命令。
5. DisplayList 是 RenderNode 命令图，不是 bitmap 或 GraphicBuffer。
6. `syncAndDrawFrame()` 确实让 UI 等 RT；是否提前放行由 `prepareTextures` 决定。
7. `CanvasContext::waitOnFences()` 等的是 CommonPool futures，不是 sync-file fd。
8. r48 的 `ReliableSurface::reserveNext()` 因常量开关不真正预取。
9. 空 damage 可不产生 buffer 却仍调用 frame-complete callback。
10. swap 只到 producer 提交边界；`HardwareRenderer.fence()` 也不是 present fence。
11. BLAST 在客户端消费本地 BufferQueue，再把 buffer/fence/几何作为 Transaction 交给 SF。
12. 传统 BufferQueueLayer 与 BufferStateLayer 的时间/fence gate 位置不同；可丢帧还可有意 latch 未 signal 的 fence。

最终主链：

```text
UI 录命令
  → RT 同步并回放
  → GPU 产出 buffer
  → Surface queue
  → BufferQueue 或 BLAST Transaction
  → SF 选择/latch
  → HWC present
```

下一章进入 `SurfaceControl.Transaction`、current/drawing state、`BufferStateLayer` buffer latch、desired-present、同步点、callback 与 release fence，继续解释多个 Layer 状态怎样原子收敛到同一合成周期。
