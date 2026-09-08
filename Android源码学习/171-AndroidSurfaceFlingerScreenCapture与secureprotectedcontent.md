# 171 Android SurfaceFlinger ScreenCapture、secure/protected content 与截图完成边界

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态只读源码，不要求编译或连接设备  
> 前置章节：第 160—170 章

---

## 1. 截图不是复制面板上的最终像素

直觉模型常常是：

```text
屏幕已有完整画面
→ 复制 framebuffer
→ 得到截图
```

Android 11 r48 的主要截图链不是这样。SurfaceFlinger 会重新选择 Display 或 Layer 子树，把 drawing-state Layer 转成 RenderEngine 的 `LayerSettings`，再合成到一块新 `GraphicBuffer`：

```text
Java SurfaceControl
→ JNI
→ ScreenshotClient / ISurfaceComposer
→ SF Binder 线程
→ SF 主线程选择 Layer
→ RenderEngine 离屏合成
→ 等 draw fence
→ GraphicBuffer 返回调用者
```

因此屏幕输出和截图是两条不同证据轴：

- 屏幕可由 HWC DEVICE layer、client target 与 protected path 共同完成；
- 截图在本章路径固定用 RenderEngine 重新合成；
- 截图 draw fence 不等于 Display present fence；
- secure/protected 内容可能被拒绝或画黑；
- 截图正常不能证明 HWC、display power 或面板 scanout 正常；
- 截图黑也不能直接证明屏幕真实像素为黑。

本章的主问题是：一次同步截图到底采了哪层状态、通过哪些安全门，并完成到了哪里。

---

## 2. r48 是同步 GraphicBuffer 接口，不是后续异步 ScreenCapture API

当前源码的主要对象是：

```text
SurfaceControl.screenshot / screenshotToBuffer / captureLayers
→ nativeScreenshot / nativeCaptureLayers
→ ScreenshotClient
→ ISurfaceComposer 同步 Binder
→ SurfaceFlinger::captureScreen / captureLayers
→ GraphicBuffer
```

不要把 Android 12+ 的 CaptureArgs、ScreenCaptureListener 或异步 callback 语义套回来。

### 2.1 五个完成点

| 完成点 | 能证明什么 |
|---|---|
| Binder `transact()` 返回 | 传输/服务端 onTransact 完成 |
| reply 中业务 status 成功 | SF 截图流程按其有限错误传播规则返回成功 |
| SF 主线程 lambda 返回 | 已选择 Layer 并调用 RenderEngine，取得或未取得 draw fence |
| draw fence signal | 本次离屏目标 buffer 可按 fence 语义安全使用 |
| Display present / panel scanout | 另一条真实显示链完成，截图 API 不直接证明 |

Java/JNI 调用是同步的：成功路径会等 SF 主线程任务，Binder 线程随后对有效 draw fence 做无限等待。但 r48 对 draw 与 wait 错误有盲区，所以“返回非空”仍不是逐像素正确性的绝对证明。

---

## 3. 源码地图

Java 与 JNI：

```text
frameworks/base/core/java/android/view/SurfaceControl.java
frameworks/base/core/jni/android_view_SurfaceControl.cpp
```

Binder proxy/stub 与客户端包装：

```text
frameworks/native/libs/gui/
├── ISurfaceComposer.cpp
├── SurfaceComposerClient.cpp
└── include/gui/
    ├── ISurfaceComposer.h
    └── SurfaceComposerClient.h
```

SurfaceFlinger 与渲染：

```text
frameworks/native/services/surfaceflinger/
├── SurfaceFlinger.cpp
├── DisplayDevice.h
├── RenderArea.h
├── RenderArea.cpp
├── BufferLayer.cpp
└── CompositionEngine/src/Output.cpp

frameworks/native/libs/renderengine/
├── include/renderengine/RenderEngine.h
└── gl/GLESRenderEngine.cpp
```

---

## 4. Java 有三种输出，参数预处理也不相同

### 4.1 Bitmap 仍包装硬件 buffer

便利方法最终调用：

```java
Bitmap.wrapHardwareBuffer(
        buffer.getGraphicBuffer(),
        buffer.getColorSpace());
```

这不是先把所有像素复制到 Java 堆。若要软件修改，还需显式复制为 software bitmap。

### 4.2 ScreenshotGraphicBuffer 保留底层语义

`screenshotToBuffer()` 返回 GraphicBuffer、ColorSpace 与 `containsSecureLayers`。系统组件可以继续把它用于 GPU 或 Surface；这个 secure 标志只表示选中集合里遇到 secure Layer，后文会说明它不代表 secure 像素真的被导出。

### 4.3 截图到 Surface 多了一条目标队列

`screenshot(display, consumer, ...)` 先取得 ScreenshotGraphicBuffer，再调用 `attachAndQueueBufferWithColorSpace()`。此时至少分成：

```text
离屏截图 buffer 绘制完成
→ attach/queue 到目标 Surface
→ 目标 consumer latch/present
```

前一步完成不能替代后两步。

### 4.4 Bitmap 便利路径会原地改 crop

当 rotation 为 90°/270°，Java 先交换方向值，再由 `rotateCropForSF()` 交换传入 `Rect` 的坐标。这个方法直接修改调用者传入的 crop 对象。

诊断方向/裁剪错误时必须记录：

- crop 属于调用前哪个方向；
- Java 是否已原地旋转 crop；
- SF 怎样结合逻辑/物理显示方向；
- `useIdentityTransform` 是否忽略 Layer 自身变换。

---

## 5. Binder 有调用权限门，也有业务结果门

`CAPTURE_SCREEN` 与 `CAPTURE_LAYERS` 要求 calling UID 是 graphics，或持有 `READ_FRAME_BUFFER`。按数字 display id/layerStack 的 `CAPTURE_SCREEN_BY_ID` 则只允许 root、graphics、system、shell。

这只是“能否调用”：

```text
入口权限
→ 选中的 Layer 是否含 secure
→ 调用 UID 是否是 system/graphics
→ RenderArea 是否 secure
→ protected 内容是否有受保护目标
```

持有 `READ_FRAME_BUFFER` 不会自动令 `forSystem=true`。后者在实现中只认 `AID_GRAPHICS` 或 `AID_SYSTEM`。

### 5.1 transact 成功不等于截图成功

stub 的结构是：

```cpp
status_t res = captureScreen(...);
reply->writeInt32(res);
if (res == NO_ERROR) {
    reply->write(*outBuffer);
    reply->writeBool(capturedSecureLayers);
}
return NO_ERROR;
```

最后的 NO_ERROR 是 onTransact 处理结果，业务 status 在 reply 头部。proxy 先检查 `transact()`，再读取业务 status；JNI 遇到任一非 NO_ERROR 通常只返回 Java `null`，把 `BAD_VALUE`、`NAME_NOT_FOUND`、`PERMISSION_DENIED` 等差异折叠掉。

---

## 6. Display capture 先冻结目标参数，再到主线程采样 Layer

token 版 `captureScreen()` 先在 `mStateLock` 下找 `DisplayDevice`。空 token 返回 BAD_VALUE，找不到返回 NAME_NOT_FOUND。

另一内部重载接受 `displayOrLayerStack`：

```text
先解释成 physical display id
→ 找不到再解释成 layerStack
```

数字本身并非天然无歧义，必须结合调用的 Binder transaction 判断含义。

### 6.1 任一尺寸为零会重置两者

Display 路径的逻辑是：

```cpp
if (reqWidth == 0 || reqHeight == 0) {
    reqWidth = display->getViewport().width();
    reqHeight = display->getViewport().height();
}
```

例如 width=500、height=0 不会保留宽 500，而是宽高一起回到 viewport。两个值均非零时则直接作为目标尺寸；这里没有截图级像素总量上限校验，巨大请求的分配失败又受后文错误传播缺口影响。

### 6.2 非法 rotation 回退 ROT_0

`toRotationFlags()` 得到 ROT_INVALID 时，SF 只记错误日志并回退 ROT_0，不返回 BAD_VALUE。因此“调用成功但方向不对”应先检查输入枚举。

### 6.3 ColorMode 查询与 capture 不是原子操作

JNI 先调用：

```cpp
getActiveColorMode(displayToken)
→ pickDataspaceFromColorMode()
→ ScreenshotClient::capture(...)
```

P3、PQ、HLG、BT2020 映射为 DISPLAY_P3，其余映射 V0_SRGB。这个 dataspace 描述输出目标；每个源 Layer 仍按自身 dataspace 转换。

ColorMode 查询和 capture 是两个 Binder 操作。显示模式在中间变化时，返回对象的 ColorSpace 描述来自前一次查询，真正绘制则采样后一次状态，二者没有原子绑定。

### 6.4 DisplayRenderArea 定义投影和 secure 资格

它提供 source crop、logical bounds、display viewport、请求尺寸、旋转、dataspace 与 OPAQUE fill。source crop 为空时使用 Display 的 source clip；显式 crop 还会按逻辑方向校正。

`isSecure()` 的条件是：

```cpp
allowSecureLayers && display->isSecure()
```

所以 unsafe 请求只是其中一门，并不会单独把任意输出变成 secure。

---

## 7. common capture 在主线程和 mStateLock 内提交离屏绘制

Display 遍历以 `mDrawingState.layersSortedByZ` 为根，只把属于目标 layerStack 且 `isVisible()` 的 drawing Layer 交给 visitor。它不是 current transaction，也不是面板最终像素。

这里的 `belongsToDisplay(..., false)` 还会排除 `mPrimaryDisplayOnly` Layer。r48 创建 WINDOW_TYPE_DONT_SCREENSHOT 对应 Layer 时会设置这个内部标志。也就是说，Display 上真实可见但被标记“不进截图”的层，可以在没有触发 secure 拒绝或黑块的情况下直接缺席；它是选择规则，不是渲染失败。

Binder 线程通过：

```cpp
schedule([&] {
    if (mRefreshPending) return EAGAIN;
    Mutex::Autolock lock(mStateLock);
    renderArea.render(...captureScreenImplLocked...);
}).get();
```

等待 SF 主线程。选择 Layer、构造 LayerSettings 和调用 `drawLayers()` 都发生在这次锁作用域内；GPU 后续执行可异步，但 CPU 侧提交会占用 SF 主线程与 state lock。

### 7.1 EAGAIN 是无上限的主线程重排队

外层使用：

```cpp
do {
    schedule(...).get();
} while (result == EAGAIN);
```

没有次数上限，也没有独立 sleep。全局 `mRefreshPending` 长时间不清时，调用者会同步阻塞，并反复向主线程排任务。它不是一次失败后由下个 VSync callback 自动完成的异步协议。

### 7.2 绘制是重建 LayerSettings，不是读回 HWC

对每个 Layer，SF 准备：

```text
buffer/acquire fence 或 solid color
geometry/clip/transform
alpha/blending/opacity
dataspace/HDR metadata
shadow/corner/background blur
security blackout
frame number / buffer id
```

再统一调用 RenderEngine。屏幕上的 DEVICE layer 在截图中也会转成客户端合成参数；HWC 刚 present 的最终结果不会被直接复制。

---

## 8. 输出 buffer、draw fence 与错误传播都有边界

普通截图目标使用：

```text
SW_READ_OFTEN
SW_WRITE_OFTEN
HW_RENDER
HW_TEXTURE
```

其中没有 PROTECTED usage。Factory 总会返回一个 GraphicBuffer 对象，但构造内部的 allocation status 没有在 `captureScreenCommon()` 先检查。

### 8.1 drawLayers status 被丢弃

`RenderEngine::drawLayers()` 返回 `status_t`，GLES 实现可因 output buffer、FBO 或 blur 准备失败而返回错误；SF 调用处没有接收该返回值。随后 `captureScreenImplLocked()` 仍返回 NO_ERROR。

### 8.2 sync_wait status 也被丢弃

主线程只交回 raw draw fence fd。Binder 线程在业务 result 为 NO_ERROR 时执行：

```cpp
sync_wait(syncFd, -1);
close(syncFd);
```

没有检查 `sync_wait()` 返回值；无效 fd 也会走这两句。因此 API success 的实际含义弱于“所有 GPU 工作必然成功且像素正确”。

### 8.3 有效 draw fence 会延长源 buffer 生命周期

若 draw fence fd 有效，SF duplicate 成 release fence，并对真正产生 LayerSettings 的每个源 Layer 调用 `onLayerDisplayed()`。截图 GPU 仍在读取源 buffer，producer 必须等合并后的 release 条件后才能安全复用。

这里的方法名并不表示 Layer 又在物理屏 present 一次；它只是把离屏读取纳入 buffer release 同步。高频截图可能增加 buffer 占用压力。

---

## 9. secure 是导出政策，protected 是受保护执行环境

两者经常同时出现在 DRM 视频，却不是同一概念：

| 属性 | 核心问题 |
|---|---|
| secure Layer | 内容是否允许进入截图、录屏或不安全输出 |
| protected buffer/content | 内存与 GPU/HWC 处理是否必须处于受保护路径 |

BufferLayer 准备客户端合成时的公式最准确：

```cpp
blackOutLayer =
    (isProtected() && !target.supportsProtectedContent) ||
    (isSecure() && !target.isSecure);
```

任一条件成立，真实纹理都不会进入 LayerSettings，而是用不透明黑色替代。

### 9.1 普通调用者遇到 secure Layer 是整次拒绝

在真正绘制前，`captureScreenImplLocked()` 先用同一 traverse 函数累计可见 secure Layer。若存在且调用者不是 system/graphics UID，直接返回 PERMISSION_DENIED。

因此拥有入口权限但 `forSystem=false` 的调用者不会得到“其他层正常、secure 区域黑”的图，而是整次失败；JNI 通常表现为 null。

Layer capture 还有更早的根层检查：即使 `childrenOnly` 最终不绘制 root，只要 root 的 current secure flag 为真，非 system/graphics 调用者仍会先被拒绝。

### 9.2 system 身份只允许继续，不自动输出 secure 像素

system/graphics 通过整次拒绝门后，是否黑掉仍由 RenderArea 决定：

- 普通 Display screenshot 传 `captureSecureLayers=false`，target 非 secure；
- unsafe 版本传 true，但还要求目标 Display 自身 secure；
- LayerRenderArea 的 `isSecure()` 在 r48 固定 false。

所以 system 普通截图可返回成功和 buffer，同时 secure 区域仍是黑块。

### 9.3 protected 内容在本章路径始终不被导出

截图的每层 target 设置把 `supportsProtectedContent` 固定为 false，提交前又调用：

```cpp
getRenderEngine().useProtectedContext(false);
```

输出 buffer 也没有 PROTECTED usage。即使调用者是 system、使用 unsafe API 且 Display secure，源 buffer 只要 protected，仍命中黑块。

物理显示可使用 secure overlay、protected client target 或受保护 GPU context；普通截图 buffer 可交付调用方，安全目标不同。因此“屏幕能看 DRM 视频、截图局部为黑”通常是正确保护行为。

---

## 10. capturedSecureLayers 只报告选中集合，不报告导出结果

Display screenshot 返回的布尔值来自：

```cpp
outCapturedSecureLayers |=
    layer->isVisible() && layer->isSecure();
```

它回答“这次遍历是否遇到 secure Layer”，不回答：

- 该层真实纹理还是黑色替代；
- 它是否同时 protected；
- protected 像素是否进入输出；
- Display 最终是否显示它。

system 普通截图就可能同时得到：

```text
业务成功
containsSecureLayers = true
secure 区域实际为黑
```

Layer capture 内部也计算这个布尔值，但 Binder/Java 结果没有把它向上返回。

exclude 子树不会进入最终 traverse，通常也不会计入该标志；但 Layer capture 对 root current secure 的提前检查发生在 exclude 应用之前。

---

## 11. Java captureLayers 在 r48 实际只画子孙

Java 文档写“layer and its children”，实际链路却是：

```text
SurfaceControl.captureLayers(...)
→ nativeCaptureLayers(...)
→ ScreenshotClient::captureChildLayers(...)
→ ISurfaceComposer.captureLayers(childrenOnly=true)
→ drawing 遍历跳过 parent 本身
```

所以传入 Layer 在 r48 主要作为子树范围与坐标根，根 Layer 自己不进入截图。native 另有 `ScreenshotClient::captureLayers(... childrenOnly=false)`，但这不是 Java 入口所调用的路径。

### 11.1 Screenshot Parent 只是锁内临时坐标根

`LayerRenderArea::render()` 在 childrenOnly 模式创建无内容的 ContainerLayer：

```text
Screenshot Parent
```

它计算临时 bounds，再通过 `setChildrenDrawingParent()` 让原 root 的 drawing children 以该层为绘制 parent。RAII 析构时把 children 的 drawing parent 恢复为原 root。

这不是公开 transaction：

- 不进入客户端事务队列；
- 不永久改变 current/drawing tree；
- 只服务于这次主线程、state-lock 内的离屏合成。

### 11.2 Display 与 Layer capture 背景不同

DisplayRenderArea 使用 OPAQUE fill，未覆盖区域是 alpha=1 的黑色；LayerRenderArea 使用 CLEAR fill，未覆盖区域是 alpha=0 的透明黑。保存到不支持 alpha 的格式后，透明边缘也可能显示为黑，不能和安全 blackout 混为一谈。

---

## 12. Layer capture 混合 current、drawing 与 current-parent 关系

准备阶段持 `mStateLock`：

| 读取项 | 状态口径 |
|---|---|
| root 是否从 current state 移除 | current 生命周期标记 |
| root secure 快速检查 | `getCurrentState().flags` |
| 默认 crop 尺寸 | `getCroppedBufferSize(getCurrentState())` |
| 目标 Display | `getLayerStack()`，沿 drawing parent/state |
| 真正遍历与 render bounds | drawing |
| exclude 的祖先回溯 | `getParent()`，即 current parent |

这不是一个纯 current 或纯 drawing 快照。事务恰好正在 reparent、改 crop、改 secure 或换 layerStack 时，选择、权限判断、排除关系与实际绘制可能来自不同代际。

### 12.1 空 crop 按宽高分别补

若 sourceCrop 宽度不正，就用 root current cropped-buffer width 补左右；高度单独处理。两者仍为空，或 root 是无边界容器且调用者没给明确 crop，返回 BAD_VALUE。

这和 Display capture 的“任一输出尺寸为 0 就重置两者”不是同一规则。

### 12.2 frameScale 只有下界检查

源码只检查 `frameScale <= 0.0f`，随后：

```cpp
reqWidth = crop.width() * frameScale;
reqHeight = crop.height() * frameScale;
```

正常有限正数若很小，截断后非正会在锁外钳成 1，所以可能得到 1×N 或 1×1。

NaN 不满足 `<= 0`，Infinity/过大有限值也没有上界；它们进入浮点到 int 的不可表示转换，结果属于未定义、不可依赖的输入边界。服务端缺少 `isfinite` 和尺寸上限校验。

### 12.3 exclude 一个父层会排除整棵后代

对每个 drawing 候选，代码从该 Layer 沿 `getParent()` 回溯；祖先任一位于 exclude set，就跳过候选。因此排除父层也排除所有后代。

但 `getParent()` 返回 current parent，遍历本身却是 drawing tree。reparent 尚未收敛时，排除判定可能沿另一棵父链，这是静态阅读必须保留的代际边界。

无效 exclude handle 会让整次 capture 返回 NAME_NOT_FOUND，不会静默忽略。

---

## 13. Java、JNI 与 Parcel 还有三个防御性缺口

### 13.1 captureLayersExcluding 忽略 format 参数

普通 `captureLayers(..., format)` 把 format 传给 JNI；excluding 版本却写死：

```java
nativeCaptureLayers(..., PixelFormat.RGBA_8888);
```

因此它的 public 参数在 r48 没有生效。

### 13.2 JNI 异常早退漏掉数组 release

JNI 先调用 `GetLongArrayElements()`，循环发现某个 native SurfaceControl 指针为 null 时抛异常并立即 return；正常路径末尾才调用 `ReleaseLongArrayElements(..., JNI_ABORT)`。

该早退没有配对 release，可能延长 pinned/copy 资源生命周期。代码也没有显式检查 `GetLongArrayElements()` 自身返回 null 的情况。

### 13.3 Parcel exclude 数量只查上界

stub 读取有符号 `int numExcludeHandles`，只判断：

```cpp
if (numExcludeHandles >= MAX_LAYERS) return BAD_VALUE;
excludeHandles.reserve(numExcludeHandles);
```

负数绕过上界，转换为 `size_type` 后可能请求巨量容量；没有成对的 `numExcludeHandles < 0` 检查。正常 proxy 来自 set size，不会发送负数，接口又有权限门，所以这是特权 Binder 输入的健壮性缺口，不应夸大成普通应用可直接触发。

---

## 14. 用“截图轴 × 显示轴”诊断，而不是把截图当真值

| 截图 | 屏幕 | 优先方向 | 仍不能直接断言 |
|---|---|---|---|
| 正常 | 黑/异常 | HWC present、power、driver、panel | drawing state 每一项都完全正确 |
| 黑/缺层 | 正常 | 选择/crop/exclude、安全 blackout、RenderEngine | 屏幕合成错误 |
| 都黑 | 都黑 | 更早的 Layer/buffer/可见性链 | 一定是 App 没画 |
| 旧 | 新 | current/drawing 采样、root/Display 选择 | 屏幕掉帧 |
| secure 区域黑 | 正常 | 核对 secure/protected 与 API 模式 | 源 buffer 无像素 |

### 14.1 返回 null 的排查顺序

```text
display/root token 是否有效
→ READ_FRAME_BUFFER / transaction 权限
→ 非 system 是否遇到 root/descendant secure
→ crop 是否仍为空、frameScale 是否有效
→ exclude handle 是否失效
→ layerStack 是否能找到 Display
→ Binder 与 SF 日志中的真实 status
```

Java null 已折叠多种错误，必须回到 native log/status 分层。

### 14.2 返回 buffer 但像素异常

先区分整张与局部：

- 整张：检查 Display/Layer 选择、crop、旋转、尺寸、dataspace、fill 与 RenderEngine 错误盲区；
- 局部：先看该区域是否 secure/protected，再看祖先 exclude、Layer 可见性、buffer/acquire fence；
- 边缘黑：区分 CLEAR alpha 后续丢失、OPAQUE fill、几何未覆盖与安全 blackout。

若真实屏幕也异常，再回到 App→BufferQueue/BLAST→SF drawing→Output/HWC→present 的完整链。

### 14.3 截图不能证明哪一帧已上屏

截图采的是主线程轮到该任务时的 drawing state。它和 Java 发起时刻、上一个 HWC present、下一个面板 scanout 都不是同一个原子点。

要回答“用户看到的那一帧”，还需要 FrameEventHistory、SurfaceTracing/FrameTracer、present fence 与显示现场，而不是给截图文件附一个墙上时钟时间就当成帧身份。

---

## 15. 静态源码练习

以下命令都只读取 `android-11.0.0_r48` 工作树。

1. 从 Java 三种出口追到 JNI：

   ```bash
   sed -n '1940,2180p' frameworks/base/core/java/android/view/SurfaceControl.java
   sed -n '190,365p' frameworks/base/core/jni/android_view_SurfaceControl.cpp
   ```

2. 验证 Binder 权限与 transact/业务 status 两层结果：

   ```bash
   sed -n '4925,4990p' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   sed -n '100,205p' frameworks/native/libs/gui/ISurfaceComposer.cpp
   sed -n '1288,1370p' frameworks/native/libs/gui/ISurfaceComposer.cpp
   ```

3. 检查 Display token、尺寸回退、rotation 与数字重载：

   ```bash
   sed -n '3965,4035p' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   sed -n '5410,5560p' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   sed -n '240,340p' frameworks/native/services/surfaceflinger/DisplayDevice.h
   ```

4. 追 common capture 的 EAGAIN、锁、draw fence 和错误丢失：

   ```bash
   sed -n '5735,5930p' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   ```

5. 对照 RenderEngine 契约与 GLES 实际错误返回：

   ```bash
   sed -n '155,195p' frameworks/native/libs/renderengine/include/renderengine/RenderEngine.h
   sed -n '1025,1105p' frameworks/native/libs/renderengine/gl/GLESRenderEngine.cpp
   ```

6. 验证 secure/protected blackout 公式：

   ```bash
   sed -n '150,210p' frameworks/native/services/surfaceflinger/BufferLayer.cpp
   sed -n '810,845p' frameworks/native/services/surfaceflinger/CompositionEngine/src/Output.cpp
   ```

7. 阅读 LayerRenderArea、root 快检、crop/scale 与 exclude：

   ```bash
   sed -n '5555,5740p' frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   sed -n '785,805p' frameworks/native/services/surfaceflinger/Layer.h
   ```

8. 确认 Java captureLayers 为何跳过根层：

   ```bash
   sed -n '1935,1970p' frameworks/native/libs/gui/SurfaceComposerClient.cpp
   rg -n 'captureChildLayers|childrenOnly && layer == parent' \
     frameworks/base/core/jni/android_view_SurfaceControl.cpp \
     frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
   ```

9. 定位 format、JNI release 与负 exclude count 三个缺口：

   ```bash
   rg -n 'captureLayersExcluding|PixelFormat.RGBA_8888' \
     frameworks/base/core/java/android/view/SurfaceControl.java
   sed -n '317,350p' frameworks/base/core/jni/android_view_SurfaceControl.cpp
   sed -n '1335,1368p' frameworks/native/libs/gui/ISurfaceComposer.cpp
   ```

练习时为每个结果标注“入口身份、current/drawing 口径、RenderArea secure、业务 status、fence”。这五列能阻止大多数跨层误判。

---

## 16. 结论：截图是受策略约束的同步离屏再合成

Android 11 r48 的截图链可以概括为：

1. Java/JNI 选择 Display 或 Layer 子树，并预处理 crop、rotation、dataspace；
2. Binder 先做调用权限检查，业务结果再写进 reply；
3. SF Binder 线程等待主线程，在 drawing-state 边界生成 LayerSettings；
4. RenderEngine 向普通非 protected GraphicBuffer 重画选中 Layer；
5. Binder 线程等待 draw fence，再返回 buffer；
6. secure 先经过整次拒绝与 target secure 两道门，protected 又受独立执行环境限制。

最重要的 r48 边界是：

- Display 请求任一尺寸为 0，会把两者都重置为 viewport；
- ColorMode 查询与 capture 是两次 Binder 操作；
- EAGAIN 重试没有上限或 sleep；
- drawLayers 与 sync_wait 错误没有改变 capture status；
- system 身份不自动让 target secure，unsafe 也不能导出 protected 像素；
- `capturedSecureLayers=true` 不代表 secure 真实像素进入 buffer；
- Java `captureLayers()` 实际只画子孙，不画根；
- Layer capture 混合 current、drawing 与 current-parent 排除关系；
- excluding 版本忽略 format，JNI/Parcel 还存在资源与长度校验缺口。

### 16.1 自测

1. 为什么截图正常不能证明 HWC present 与面板正常？
2. transact NO_ERROR 与 reply 业务 NO_ERROR 有什么区别？
3. width=500、height=0 时 Display 截图为何不会保留宽 500？
4. ColorMode 查询和 capture 之间有什么非原子窗口？
5. draw fence signal 与 display present fence signal 分别完成什么？
6. 非 system 调用者、system 普通截图、system unsafe 截图遇到 secure Layer 时有何不同？
7. 为什么 unsafe 仍不能导出 protected 视频？
8. Layer capture 为什么会跳过根 Layer？
9. current parent 与 drawing traversal 混用会怎样影响 exclude？
10. 返回非空 buffer 为什么仍不能绝对证明像素正确？

能把“选谁、何时采样、在哪重画、谁能导出、完成到哪”分别回答清楚，就掌握了 r48 截图的真实边界。

---

下一篇将进入 **172-AndroidWindowManagerTraceWinscope与窗口状态时间线**，把显示侧证据扩展到 WMS trace、窗口状态变迁与跨 trace 对齐。
