# 66 MediaProjection、VirtualDisplay 与屏幕捕获投射链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：理解用户授权如何变成 `IMediaProjection` 能力令牌，App 如何用它创建 `VirtualDisplay`，DisplayManagerService、VirtualDisplayAdapter、SurfaceControl、SurfaceFlinger 和目标 `Surface` 如何协作，并分清屏幕捕获、录屏、远端投屏、第二屏渲染与截图。

---

## 1. 先看最终结论

一次典型录屏不是“调用一个截图函数并不断截图”，而是创建一条持续工作的显示输出管线：

```text
用户明确授权
→ App 获得 MediaProjection（能力令牌）
→ App 创建一个带目标 Surface 的 VirtualDisplay
→ system_server 创建逻辑/虚拟显示设备
→ SurfaceFlinger 把被镜像显示的合成结果持续送入目标 Surface
→ ImageReader 读取帧，或 MediaCodec 编码帧
→ App 保存文件、推流或做图像处理
```

最关键的三个对象：

```text
MediaProjection = “允许这个 App 捕获”的授权能力
VirtualDisplay  = “系统里新增的一条显示输出管线”
Surface         = “合成后的帧要送到哪里”
```

它们不能互相替代：仅有授权不会自动产生画面，仅创建 `Surface` 也无权捕获屏幕，`VirtualDisplay` 没有输出 Surface 时也不会把帧送给编码器或 ImageReader。

---

## 2. 先分清五种看起来都叫“投屏”的场景

| 场景 | 主要对象 | 数据怎么走 |
|---|---|---|
| App 录制整块屏幕 | `MediaProjection + VirtualDisplay + MediaCodec` | 屏幕合成帧进入编码器输入 Surface，再写成视频 |
| App 截取一帧/分析画面 | `MediaProjection + VirtualDisplay + ImageReader` | 屏幕帧进入 ImageReader Surface，App 获得 Image |
| 手机画面镜像到网络电视 | 捕获/编码/传输协议，可能结合投屏 Provider | 帧编码后经网络发送；MediaRouter 只负责设备发现与控制 |
| 电视自己播放网络视频 | `MediaRouter2` 的 remote playback session | 手机发 URL/控制命令，电视自己拉流；通常不捕获手机屏幕 |
| App 在第二块 Display 单独显示 UI | `DisplayManager + Presentation/Activity` | WMS 在目标 Display 上布置窗口，不等于镜像主屏 |

一句话：

> **MediaProjection 解决“能否捕获”；VirtualDisplay 解决“建立哪条显示输出”；Surface 决定“输出交给谁”；编码和网络传输仍由其他模块完成。**

---

## 3. 核心源码地图

### 3.1 App 侧 API 与 Binder

```text
frameworks/base/media/java/android/media/projection/
    MediaProjectionManager.java
    MediaProjection.java
    MediaProjectionInfo.java
    IMediaProjectionManager.aidl
    IMediaProjection.aidl
    IMediaProjectionCallback.aidl
    IMediaProjectionWatcherCallback.aidl
```

### 3.2 用户授权界面

```text
frameworks/base/packages/SystemUI/src/com/android/systemui/media/
    MediaProjectionPermissionActivity.java
```

### 3.3 system_server 授权服务

```text
frameworks/base/services/core/java/com/android/server/media/projection/
    MediaProjectionManagerService.java
```

### 3.4 显示 Framework 与 system_server

```text
frameworks/base/core/java/android/hardware/display/
    DisplayManager.java
    DisplayManagerGlobal.java
    VirtualDisplay.java
    VirtualDisplayConfig.java
    IDisplayManager.aidl
    IVirtualDisplayCallback.aidl

frameworks/base/services/core/java/com/android/server/display/
    DisplayManagerService.java
    VirtualDisplayAdapter.java
    LogicalDisplay.java
    DisplayDevice.java
```

### 3.5 Native 显示边界

```text
frameworks/base/core/java/android/view/SurfaceControl.java
frameworks/base/core/jni/android_hardware_display_DisplayManagerGlobal.cpp
frameworks/native/services/surfaceflinger/
```

本章不重复第 12 章的 BufferQueue、fence 和 SurfaceFlinger 合成细节，只把它们接到 VirtualDisplay 输出端。

---

## 4. 七个角色分别负责什么

| 角色 | 进程 | 职责 |
|---|---|---|
| 媒体/录屏 App | App | 请求授权，准备 Surface，消费/编码帧，管理生命周期 |
| `MediaProjectionPermissionActivity` | SystemUI | 显示可信授权对话框，确认调用包身份 |
| `MediaProjectionManagerService` | system_server | 创建并维护当前活动 Projection，校验前台服务、停止与死亡 |
| `MediaProjection` | App 门面 | 包装远端 `IMediaProjection` token，创建 VirtualDisplay，分发 onStop |
| `DisplayManagerService` | system_server | 校验包、flags、Projection 和权限，创建逻辑显示 |
| `VirtualDisplayAdapter` | system_server | 创建 `SurfaceControl` display token 和 `VirtualDisplayDevice`，绑定输出 Surface |
| SurfaceFlinger | native 服务 | 按显示配置合成 Layer，并把结果提交给显示输出/Surface |

App 代码看上去只有两次调用：

```java
getMediaProjection(...)
projection.createVirtualDisplay(...)
```

但背后跨过了 SystemUI、两个 system_server 服务和 SurfaceFlinger。

---

## 5. 第一条主线：请求用户授权

### 5.1 `createScreenCaptureIntent()` 不直接授予权限

```java
MediaProjectionManager manager =
        getSystemService(MediaProjectionManager.class);
Intent intent = manager.createScreenCaptureIntent();
startActivityForResult(intent, REQUEST_CAPTURE);
```

`createScreenCaptureIntent()` 从资源配置读取系统授权 Activity 的组件名，并返回显式 Intent：

```text
config_mediaProjectionPermissionDialogComponent
→ MediaProjectionPermissionActivity
```

它只是把 App 带到可信系统 UI。普通 App 不能自己伪造一个同名对话框后给自己发授权，因为真正 grant 由持有 `MANAGE_MEDIA_PROJECTION` 权限的系统组件通过 Binder 创建。

### 5.2 SystemUI 如何确认“是谁在请求”

授权 Activity 读取：

```java
mPackageName = getCallingPackage();
```

再从 PackageManager 得到 `ApplicationInfo`、真实 UID 和展示名称。对话框中的 App 名会做换行/控制字符清理和长度裁剪，避免恶意 label 干扰安全提示。

正向按钮还启用：

```java
setFilterTouchesWhenObscured(true);
```

窗口添加隐藏非系统 overlay 的系统 flag。这些措施针对覆盖攻击：恶意悬浮窗不应诱骗用户点击录屏授权。

### 5.3 用户允许后，SystemUI 创建 Binder grant

核心逻辑：

```java
IMediaProjection projection = mService.createProjection(
        uid,
        packageName,
        MediaProjectionManager.TYPE_SCREEN_CAPTURE,
        false /* permanentGrant */);

Intent result = new Intent();
result.putExtra(EXTRA_MEDIA_PROJECTION, projection.asBinder());
setResult(RESULT_OK, result);
```

注意返回的不是一堆布尔权限，也不是视频数据，而是 `IMediaProjection` Binder 对象。

### 5.4 为什么把它叫“能力令牌”

持有 token 的进程可以在 DisplayManagerService 创建需要屏幕镜像能力的 VirtualDisplay。DMS 会向 MediaProjectionManagerService 验证：

```text
这个 Binder 是否就是当前活动 Projection？
它是否允许 project video？
它允许哪些 VirtualDisplay flags？
```

因此权限不是 App 自己声明的 `boolean allowed=true`，而是一个由系统服务签发、可失效、可关联死亡通知的对象能力。

---

## 6. 第二条主线：resultData 变成活动 MediaProjection

授权结果返回后：

```java
MediaProjection projection = manager.getMediaProjection(resultCode, data);
```

`getMediaProjection()` 做三步：

```text
检查 resultCode == RESULT_OK
→ 从 Intent 取 EXTRA_MEDIA_PROJECTION Binder
→ new MediaProjection(context, IMediaProjection.Stub.asInterface(binder))
```

### 6.1 构造函数为什么立即调用 `mImpl.start()`

```java
mImpl.start(new MediaProjectionCallback());
```

这一步跨 Binder 进入 `MediaProjectionManagerService.MediaProjection.start()`，才把 grant 置为当前活动 Projection。

服务端会：

1. 拒绝同一个 grant 重复 start；
2. 对 target Q 及以上的非特权 App 检查 mediaProjection 类型前台服务；
3. 保存 App 回调并 `linkToDeath`；
4. 必要时临时调整 overlay AppOp；
5. 调用 `startProjectionLocked(this)`；
6. 通知有特权的 Projection watcher。

### 6.2 Android 11 为什么要求前台服务

目标版本 Android 10/Q 及以上的普通 App，需要运行：

```xml
<service
    android:foregroundServiceType="mediaProjection" />
```

并真正启动对应类型的前台服务。原因不是让 App “更容易常驻”，而是让持续、敏感的屏幕捕获对用户可见，并受到系统生命周期约束。

服务端还监听前台服务类型变化：若活动 Projection 的 UID 不再有 `FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION`，就停止授权。

### 6.3 同一时间只有一个活动 Projection

`startProjectionLocked()` 若发现已有 `mProjectionGrant`，会先停止旧 grant，再把当前 token/grant 指向新对象。

```text
Projection A 活动
→ Projection B start
→ A.stop + A.onStop
→ B 成为唯一当前 grant
```

用户切换也会停止现有 Projection。这能避免跨用户画面继续被旧用户 App 捕获。

---

## 7. 为什么必须先注册 `MediaProjection.Callback`

典型代码：

```java
projection.registerCallback(new MediaProjection.Callback() {
    @Override
    public void onStop() {
        // 停编码、释放 VirtualDisplay、关闭 ImageReader 等
    }
}, handler);
```

停止可能来自：

- App 主动 `projection.stop()`；
- 用户通过系统 UI 停止；
- 新 Projection 抢占旧 Projection；
- 用户切换；
- 前台服务类型消失；
- App callback Binder 死亡；
- Android 11 中选择旧 `MediaRouter` remote display route 时发生互斥停止。

`onStop()` 的含义是：**授权已经失效**。App 应在这里停止编码并释放持有资源，而不是尝试复用旧 token。

回调通过注册时的 Handler 分发。若 Handler 为空，Android 11 代码会 `new Handler()`，依赖调用线程已有 Looper；稳妥做法是明确传入主线程或专用线程 Handler。

---

## 8. 第三条主线：准备接收屏幕帧的 Surface

`VirtualDisplay` 输出的不是 `Bitmap`，而是持续写入一个 `Surface`。最常见有两类消费者。

### 8.1 ImageReader：读取原始图像

概念代码：

```java
ImageReader reader = ImageReader.newInstance(
        width, height, PixelFormat.RGBA_8888, 2);
Surface surface = reader.getSurface();
```

帧路径：

```text
SurfaceFlinger/显示输出（producer）
→ BufferQueue
→ ImageReader（consumer）
→ acquireLatestImage()
→ Plane/ByteBuffer
```

适合截图、OCR、图像分析。持续录屏若每帧转 Bitmap，内存带宽和拷贝开销通常很高。

### 8.2 MediaCodec input Surface：直接编码

概念代码：

```java
MediaCodec encoder = MediaCodec.createEncoderByType("video/avc");
encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
Surface surface = encoder.createInputSurface();
encoder.start();
```

帧路径：

```text
SurfaceFlinger 合成输出
→ 编码器 input Surface
→ MediaCodec / Codec2 或 OMX
→ H.264/H.265 output buffers
→ MediaMuxer 文件，或网络发送
```

这种路径避免 App 把每帧读回 CPU 后再喂编码器，是录屏常见选择。

### 8.3 Surface 的所有权不要误解

App 创建/获得 Surface，并把句柄跨 Binder 交给 DMS。system_server/SurfaceFlinger 使用它作为显示输出目标；App 仍要管理上层消费者和资源释放。

`VirtualDisplayAdapter.destroyLocked()` 会释放它持有的 Surface 引用，但这不等于 App 的 `ImageReader`、`MediaCodec`、文件或网络资源都自动关闭。

---

## 9. 第四条主线：创建 VirtualDisplay

概念代码：

```java
VirtualDisplay display = projection.createVirtualDisplay(
        "ScreenRecord",
        width,
        height,
        densityDpi,
        DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
        surface,
        callback,
        handler);
```

### 9.1 App 侧调用链

```text
MediaProjection.createVirtualDisplay
→ DisplayManager.createVirtualDisplay(projection, config, callback, handler)
→ DisplayManagerGlobal.createVirtualDisplay
→ 包装 IVirtualDisplayCallback
→ 取 projection.getProjection() 得到 IMediaProjection
→ IDisplayManager.createVirtualDisplay(config, callback, projectionToken, packageName)
```

返回值首先是 `displayId`。`DisplayManagerGlobal` 再取得对应 `Display`，最终包装成 App 可操作的 `VirtualDisplay`。

创建失败时 displayId 小于 0，API 返回 `null`。因此不能假设创建永远成功。

### 9.2 跨 Binder 传了什么

```text
VirtualDisplayConfig：名字、宽、高、density、flags、Surface 等
IVirtualDisplayCallback：生命周期与 Binder 身份 token
IMediaProjection：屏幕捕获能力 token
packageName：用于包/UID 归属验证
```

这里有两个 token，作用完全不同：

```text
IMediaProjection token       → 证明有捕获能力
IVirtualDisplayCallback token → 标识这个 VirtualDisplay，并感知 App 死亡
```

---

## 10. DisplayManagerService 的安全闸门

`DisplayManagerService.BinderService.createVirtualDisplay()` 不是简单转发，它集中执行输入和权限检查。

### 10.1 packageName 必须属于 calling UID

```java
if (!validatePackageName(callingUid, packageName)) {
    throw new SecurityException(...);
}
```

防止 App 假冒另一个包创建显示。

### 10.2 参数和 Surface 检查

```text
callback 不能为空
config 不能为空
Surface 不能是 single-buffered
width/height/density 必须为正（构造/resize 路径检查）
```

显示合成与 BufferQueue 需要合理的多缓冲同步；single-buffer Surface 不适合作为这里的输出目标。

### 10.3 flags 会被正规化

例如：

```text
PUBLIC       → 自动附加 AUTO_MIRROR
OWN_CONTENT_ONLY → 清除 AUTO_MIRROR
```

两者语义冲突时，系统不能同时把它当“只显示自己的内容”和“无自有内容时镜像别的 display”。

### 10.4 Projection 必须仍然有效

```java
getProjectionService().isValidMediaProjection(projection)
```

服务端比较 token 是否等于当前活动 token。旧 grant、已停止 grant 或被新会话抢占的 grant 都不能继续创建捕获显示。

### 10.5 Projection 可以重写 flags

屏幕捕获类型会：

```text
清除 OWN_CONTENT_ONLY
加入 AUTO_MIRROR
加入 PRESENTATION
```

因此 flags 不是 App 单方面的最终决定。授权类型与系统权限会共同约束真实显示属性。

### 10.6 secure 和 trusted 是更高权限

普通屏幕捕获 grant：

```java
canProjectVideo() == true
canProjectSecureVideo() == false
```

若请求 `VIRTUAL_DISPLAY_FLAG_SECURE`，普通 Projection 仍不足以通过，通常需要 `CAPTURE_SECURE_VIDEO_OUTPUT` 等特权能力。

`TRUSTED`、显示系统装饰等 flags 也有 `ADD_TRUSTED_DISPLAY`、`INTERNAL_SYSTEM_WINDOW` 检查。不要把用户点一次录屏允许理解为获得所有显示特权。

---

## 11. VirtualDisplayAdapter 怎样造出一个“虚拟显示设备”

DMS 完成校验后进入内部创建，最终调用：

```text
VirtualDisplayAdapter.createVirtualDisplayLocked(...)
```

### 11.1 创建 SurfaceControl display token

```java
IBinder displayToken = SurfaceControl.createDisplay(name, secure);
```

这一步跨向 SurfaceFlinger，创建一个虚拟 display 端点。它不是物理屏幕，没有真实面板和 HWC connector，但在显示系统中可作为合成目标。

### 11.2 构造 `VirtualDisplayDevice`

对象保存：

```text
displayToken
app callback token
ownerUid / ownerPackageName
width / height / densityDpi
surface
flags
displayIdToMirror
```

它继承 `DisplayDevice`，因此 DMS 可以像管理物理显示设备一样把它纳入 DisplayDevice/LogicalDisplay 模型，但其输出目标是 App 给的 Surface。

### 11.3 两条死亡/停止保险

创建时注册：

```text
projection.registerCallback(MediaProjectionCallback)
appToken.linkToDeath(VirtualDisplayDevice)
```

所以：

- Projection 停止 → `handleMediaProjectionStoppedLocked()` → VirtualDisplay 停止并断开 Surface；
- App callback Binder 死亡 → 移除 device、销毁 SurfaceControl display、发送 removed 事件。

这是为什么即使 App 崩溃没主动 release，系统也不会永久遗留捕获显示。

这里要把“停止输出”和“App 完整释放”分开理解。Android 11 的 Projection stop 路径调用 `VirtualDisplayDevice.stopLocked()`，核心动作是把 Surface 设为 null 并把 device 标为 stopped，使它永远不再恢复输出；`VirtualDisplay.Callback.onStopped()` 的 API 说明仍要求 App 自己调用 `VirtualDisplay.release()`。此外，Codec、ImageReader、Muxer 和业务线程从来都不是 DMS 能替 App 释放的资源。

---

## 12. DisplayDevice 与 LogicalDisplay 不要混成一个对象

可以用“硬件出口”和“系统视图”理解：

```text
VirtualDisplayDevice
→ 描述这个虚拟输出设备的尺寸、Surface、flags、owner

LogicalDisplay
→ 系统提供给 WMS、应用和 Display API 的逻辑显示
→ 拥有 displayId、DisplayInfo、内容与设备映射
```

创建 VirtualDisplay 后 DMS 把新 Device 纳入显示拓扑，生成或更新 LogicalDisplay，向其他系统组件发送 display added/changed/removed 事件。

App 得到的 `VirtualDisplay.getDisplay()` 是逻辑 `Display` 视图，不是 `VirtualDisplayDevice` 本体；后者只存在于 system_server。

---

## 13. 画面为什么会进入目标 Surface

### 13.1 AUTO_MIRROR 决定无自有内容时镜像

屏幕捕获类型会带 `AUTO_MIRROR`。VirtualDisplayDevice 若没有 `OWN_CONTENT_ONLY`，显示系统可以把指定 display（默认通常为主显示）的内容镜像到这个虚拟输出。

```text
主 Display 的 Layer/窗口内容
→ 显示拓扑决定虚拟 display 镜像源
→ SurfaceFlinger 为虚拟 display 合成
→ setDisplaySurface(displayToken, targetSurface)
→ buffer 写入 target Surface 对应队列
```

`VirtualDisplayConfig` 还可携带 `displayIdToMirror`，系统内部可指定镜像哪块显示。

### 13.2 “复制 framebuffer”不是准确模型

更准确的理解是：SurfaceFlinger 为该虚拟显示输出建立合成目标，根据显示层栈、投影和尺寸产生帧。它可能复用 GPU/HWC 能力，但不是 Java 层循环读取主屏 Bitmap 再拷贝。

### 13.3 WMS 与 SurfaceFlinger 各管什么

```text
WMS：窗口属于哪块 Display、窗口层级、可见性、旋转和安全属性
DMS：显示设备/逻辑显示、尺寸密度、镜像关系、生命周期
SurfaceFlinger：消费 Layer buffer、合成并提交到每个 display 输出
```

MediaProjectionManagerService 不负责逐帧合成；它负责授权和活动会话生命周期。

---

## 14. 尺寸、density 和旋转为什么麻烦

创建参数：

```text
width / height：虚拟显示的像素尺寸
densityDpi：逻辑 dp 到像素的换算基准
```

它们不是“原屏参数的随便副本”。错误组合会导致：

- 画面拉伸或留黑边；
- UI 逻辑尺寸与输出像素不一致；
- 编码分辨率过大造成卡顿/码率过高；
- 旋转后宽高不交换，内容裁切；
- 编码器不支持目标尺寸或对齐要求。

`VirtualDisplay.resize(width, height, densityDpi)` 最终到：

```text
DMS.resizeVirtualDisplayInternal
→ VirtualDisplayAdapter.resizeVirtualDisplayLocked
→ 更新 mode/density
→ SurfaceControl.Transaction.setDisplaySize
```

但仅 resize VirtualDisplay 不一定足够：编码器和目标 Surface 的尺寸能力也要协调。很多录屏实现选择停止并重新创建编码管线，以避免动态重配复杂性。

### 14.1 屏幕旋转不是“像素自动永远正确”

旋转涉及：

```text
源 Display rotation
→ WMS/DisplayContent 配置变化
→ 虚拟显示的投影与 viewport
→ 编码器宽高、视频方向 metadata
```

排查横竖屏问题时必须同时看显示配置和编码输出，不能只改 `MediaFormat.KEY_ROTATION`。

---

## 15. VirtualDisplay 的三个生命周期动作

### 15.1 `setSurface(surface)`

可替换或暂时移除输出 Surface：

```text
VirtualDisplay.setSurface
→ DisplayManagerGlobal.setVirtualDisplaySurface
→ DMS
→ VirtualDisplayDevice.setSurfaceLocked
→ traversal
→ SurfaceControl Transaction 更新 display surface
```

Surface 从非空变 null 时显示进入 off/暂停语义；重新设置后恢复输出。

### 15.2 `resize(...)`

改变虚拟显示 mode 和 density，并触发 display changed/traversal。它不会替 App 重建所有消费者资源。

### 15.3 `release()`

```text
VirtualDisplay.release
→ IDisplayManager.releaseVirtualDisplay(callbackToken)
→ Adapter 移除 VirtualDisplayDevice
→ release Surface 引用
→ SurfaceControl.destroyDisplay(displayToken)
→ VirtualDisplay.Callback.onStopped
```

`release VirtualDisplay` 与 `stop MediaProjection` 不相同：

- release 某个 VirtualDisplay：只销毁这一条显示输出；
- stop Projection：使整个授权失效，并停止所有绑定到它的 VirtualDisplay。

一个 Projection 理论上可创建多个 VirtualDisplay，因此作用域必须分清。

---

## 16. 停止链路完整展开

### 16.1 App 主动停止

```text
MediaProjection.stop()
→ IMediaProjection.stop()
→ MediaProjectionManagerService.stopProjectionLocked
→ 清空 mProjectionToken / mProjectionGrant
→ watcher onStop
→ IMediaProjectionCallback.onStop
→ App MediaProjection.Callback.onStop
```

同时，VirtualDisplayAdapter 注册的 Projection callback 会让关联 Device 停止并断开 Surface。

这一步是系统侧的“停止帧生产”保证；App 收到 `MediaProjection.Callback.onStop()` 后仍应执行自己的幂等资源清理。不要把 callback 当成“所有对象已经被系统 release 完毕”的通知。

### 16.2 App 进程死亡

Projection start 时，服务端把 App callback Binder `linkToDeath`。Binder 死亡会执行：

```text
remove callback
→ projection.stop()
```

VirtualDisplay 自身 callback token 也有独立死亡监听。因此授权层和显示层都有清理兜底。

### 16.3 正确释放顺序

推荐思维：先让生产停止，再收消费者资源。

```text
停止/封口编码写入
→ VirtualDisplay.release()
→ projection.stop()
→ MediaCodec.stop/release
→ ImageReader/Surface release
→ MediaMuxer.stop/release（前提是已真正写入轨道/sample）
```

具体编码器顺序取决于实现，尤其需要正确发送 EOS 和排空输出。核心要求是：`onStop()` 必须能幂等地走同一套清理逻辑，避免系统停止与用户按钮停止竞态导致 double release。

---

## 17. `FLAG_SECURE` 内容为什么通常录不到

窗口可以使用 `WindowManager.LayoutParams.FLAG_SECURE` 表示内容不应出现在截图或非安全显示中，例如 DRM 视频、密码或金融页面。

普通用户同意的 screen-capture Projection：

```java
canProjectSecureVideo() == false;
```

它不是“用户点击允许后可以绕过所有安全窗口”。安全内容在合成/捕获路径会被排除或遮挡，具体呈现可能是黑块/空白。

这里有两个名称相似的概念：

```text
窗口 FLAG_SECURE
→ 内容本身要求安全保护

VIRTUAL_DISPLAY_FLAG_SECURE
→ 这个虚拟显示被声明可承载安全内容
```

后者要求更高的系统权限或 secure projection 能力；普通 screen-capture token 的 `canProjectSecureVideo()` 返回 false，所以不能靠自己加 flag 绕过限制。

---

## 18. 屏幕画面与系统音频是两条独立数据链

`MediaProjection` 类注释特别说明：通过 screen capture Intent 获得的能力用于屏幕内容，不代表音频样本会自动混进 VirtualDisplay Surface。

画面：

```text
Display/SurfaceFlinger
→ VirtualDisplay Surface
→ Video encoder
```

播放音频捕获：

```text
MediaProjection
→ AudioPlaybackCaptureConfiguration
→ AudioRecord.Builder.setAudioPlaybackCaptureConfig
→ AudioPolicy/AudioFlinger capture mix
→ PCM AudioRecord buffers
→ Audio encoder
```

Android 10 起的 playback capture 还受被录 App 的策略约束：usage、目标版本、manifest 的 `allowAudioPlaybackCapture`、UID/profile 等都会影响是否能录到。

麦克风录音又是另一条 `AudioRecord + RECORD_AUDIO` 路径。若最终生成带声音的视频，App 需要自行编码音频并与视频按时间戳封装同步。

所以“录屏没有声音”首先不要在 VirtualDisplay 中找音轨；VirtualDisplay 只输出图像。

---

## 19. Android 11 与旧 MediaRouter remote display 的互斥

上一章提到旧 `MediaRouter.ROUTE_TYPE_REMOTE_DISPLAY`。Android 11 的 `MediaProjectionManagerService` 注册了旧 Router callback：

```java
if ((type & MediaRouter.ROUTE_TYPE_REMOTE_DISPLAY) != 0) {
    if (mProjectionGrant != null) {
        mProjectionGrant.stop();
    }
}
```

反向地，新 Projection 开始时，如果已有 remote display route，服务会选择 fallback route。

这说明在该版本中，系统主动避免两套远端显示/屏幕捕获机制同时争用显示镜像状态。

不要把它推广成所有 Android 版本和所有 `MediaRouter2` remote playback 都互斥：这里源码明确监听的是旧 `MediaRouter` 的 `ROUTE_TYPE_REMOTE_DISPLAY`。

---

## 20. 一次录屏的完整时序图

```text
录屏 App        SystemUI        ProjectionService       DMS/VDA        SurfaceFlinger/Codec
   |                |                   |                  |                    |
   | create intent  |                   |                  |                    |
   |--------------->|                   |                  |                    |
   | 用户确认       | createProjection |                  |                    |
   |                |------------------>|                  |                    |
   | RESULT_OK + IMediaProjection       |                  |                    |
   |<---------------|                   |                  |                    |
   | getMediaProjection → start(callback)                  |                    |
   |----------------------------------->|                  |                    |
   | 注册 onStop、准备 Codec Surface    |                  |                    |
   | createVirtualDisplay(config, surface, projection)     |                    |
   |------------------------------------------------------>|                    |
   |                                  validate token/flags |                    |
   |                                     create device/display token ---------->|
   |<-------------------- VirtualDisplay + Display -----------------------------|
   |                |                   |                  |                    |
   |                |                   |                  |  合成帧 → Surface  |
   |                |                   |                  |------------------->|
   |                |                   |                  |     编码 output     |
   |                |                   |                  |<-------------------|
   | stop/release   |                   |                  |                    |
   |----------------------------------->|---- callback ---->| destroy display    |
   | onStop         |                   |                  |                    |
   |<-----------------------------------|                  |                    |
```

图中真正逐帧高频运行的是 SurfaceFlinger/BufferQueue/Codec 数据面；ProjectionService 的 Binder 主要出现在授权、创建和停止等低频控制面。

---

## 21. 常见误解逐个修正

### 误解 1：用户允许后，系统就开始录屏

不对。此时只获得 Projection token；还要创建目标 Surface 和 VirtualDisplay。

### 误解 2：VirtualDisplay 是内存里的一张虚拟 Bitmap

不对。它是显示系统中的持续输出设备/逻辑显示，帧通过 Surface/BufferQueue 流动。

### 误解 3：`MediaProjection` 负责把每一帧回调给 App

不对。它负责授权和停止回调；图像帧由目标 Surface 的 consumer 提供。

### 误解 4：创建 secure VirtualDisplay 就能录安全窗口

不对。普通 Projection 明确不能 project secure video，请求 secure flag 还会触发更高权限校验。

### 误解 5：VirtualDisplay 自带视频编码

不对。它只输出图像；H.264/H.265 编码由 MediaCodec 等完成。

### 误解 6：录屏音频也从同一个 Surface 出来

不对。视频 Surface 和 AudioPlaybackCapture/AudioRecord 是两条数据链。

### 误解 7：Activity 销毁就一定停止捕获

不一定。Projection 往往由前台 Service 持有。应显式管理 stop/release，不能依赖 Activity 生命周期碰巧清理。

### 误解 8：`VirtualDisplay.release()` 等于 `MediaProjection.stop()`

不对。前者销毁一条输出，后者撤销整次授权并停止所有关联输出。

### 误解 9：Presentation 就是主屏镜像

不对。`PRESENTATION` flag 表示适合作为演示显示；是否镜像还取决于 `AUTO_MIRROR/OWN_CONTENT_ONLY` 和显示内容配置。

### 误解 10：MediaRouter2 投到电视必然先创建 MediaProjection

不对。remote playback 常由电视自己拉流，无需捕获手机屏幕。

---

## 22. 故障排查：按层次找证据

### 22.1 用户点允许后 `getMediaProjection()` 返回 null

检查：

```text
resultCode 是否 RESULT_OK
resultData 是否原样传回
EXTRA_MEDIA_PROJECTION Binder 是否存在
Activity Result 生命周期是否写错
```

### 22.2 构造 MediaProjection 时抛 SecurityException

Android 11 重点检查 target Q+ App 是否已经运行 `mediaProjection` 类型前台服务。Manifest 声明与实际 `startForeground()` 都要正确。

### 22.3 `createVirtualDisplay()` 返回 null 或抛异常

```text
Projection 是否仍为当前有效 token
→ packageName/callingUid 是否匹配
→ width/height/density 是否有效
→ Surface 是否 single-buffered
→ flags 是否需要特权权限
→ DMS 是否成功建立 Display/DisplayInfo
```

### 22.4 VirtualDisplay 创建成功但收不到帧

```text
目标 Surface 是否非 null 且 consumer 正在工作
→ ImageReader 是否及时 acquire/close Image
→ Codec 是否已 configure/start
→ VirtualDisplay 是否 paused/off
→ 镜像源和 flags 是否正确
→ Projection 是否已收到 onStop
```

ImageReader 若不及时关闭 Image，有限 buffer 会被占满，producer 可能阻塞或丢帧。

### 22.5 画面正常但某些区域是黑色

优先判断该窗口/Layer 是否安全内容，而不是先怀疑编码器。然后再检查 DRM、安全 Surface 与系统隐私策略。

### 22.6 录屏只有画面没有声音

这是预期的链路分离。检查是否单独创建 AudioPlaybackCapture/AudioRecord、目标 App 是否允许捕获、usage 是否匹配，以及音视频编码/封装是否启动。

### 22.7 横竖屏切换后拉伸或花屏

同时检查源 display rotation、VirtualDisplay size/density、Surface/Codec 分辨率、编码器重配和容器 rotation metadata。

### 22.8 停止后仍占用编码器或内存

确认 `MediaProjection.Callback.onStop()` 与用户主动停止走同一个幂等清理函数，并释放 VirtualDisplay、Codec、Image、ImageReader、Surface、Muxer 和线程。

---

## 23. macOS 只读源码练习

### 练习 1：找出授权 UI 的真实组件

```bash
rg -n "config_mediaProjectionPermissionDialogComponent" frameworks/base
```

追到 `createScreenCaptureIntent()`，解释为什么使用显式系统组件。

### 练习 2：证明授权结果携带 Binder 而不是 boolean

对照：

```text
MediaProjectionPermissionActivity.getMediaProjectionIntent
MediaProjectionManager.getMediaProjection
```

写出 extra 中对象从 `IMediaProjection` 到 Binder 再回到 Proxy 的过程。

### 练习 3：追踪 Projection start

```text
new MediaProjection
→ mImpl.start(callback)
→ MediaProjectionManagerService.MediaProjection.start
→ startProjectionLocked
```

标出前台服务检查、linkToDeath 和旧 grant 停止的位置。

### 练习 4：追踪创建 VirtualDisplay 的两组 token

从 `DisplayManagerGlobal.createVirtualDisplay()` 追到 DMS，回答 `IMediaProjection` 和 `IVirtualDisplayCallback` 各自证明什么。

### 练习 5：读 flags 正规化

阅读 `DisplayManagerService.createVirtualDisplay()` 和 `MediaProjection.applyVirtualDisplayFlags()`，解释：

```text
PUBLIC
AUTO_MIRROR
OWN_CONTENT_ONLY
PRESENTATION
SECURE
TRUSTED
```

之间的约束。

### 练习 6：从 Adapter 追到 SurfaceFlinger token

```text
VirtualDisplayAdapter.createVirtualDisplayLocked
→ SurfaceControl.createDisplay
→ VirtualDisplayDevice
```

找出 displayToken、appToken、projection callback 三种 Binder/回调关系。

### 练习 7：追踪 Surface 替换

从 `VirtualDisplay.setSurface()` 追到 `VirtualDisplayDevice.performTraversalLocked()`，解释为什么改变字段后还需要 traversal 和 Transaction。

### 练习 8：追踪三种停止来源

分别画出：App 主动 stop、前台服务类型消失、App Binder 死亡。确认最终怎样触发 App `onStop()` 和 VirtualDisplay 停止。

### 练习 9：证明普通 Projection 不能捕获 secure video

阅读：

```text
MediaProjection.canProjectSecureVideo()
DisplayManagerService.canProjectSecureVideo()
```

再与窗口 `FLAG_SECURE` 的语义对照。

### 练习 10：画图像与音频两条链

图像必须包含 VirtualDisplay Surface，音频必须包含 `AudioPlaybackCaptureConfiguration + AudioRecord`。两条链最后才在编码/封装阶段汇合。

### 练习 11：解释与上一章的关系

分别用一句话解释：remote playback 为什么不需要 MediaProjection；screen mirroring 为什么可能同时需要设备路由控制和画面编码传输。

---

## 24. 推荐阅读顺序

第一次阅读按这四段，不要一开始跳进 SurfaceFlinger：

```text
第一段：MediaProjectionManager + SystemUI PermissionActivity
目标：理解授权 token 怎样产生

第二段：MediaProjectionManagerService.MediaProjection
目标：理解 start、唯一活动 grant、前台服务和 stop

第三段：DisplayManagerGlobal + DMS.createVirtualDisplay
目标：理解跨 Binder 参数、权限和 flags

第四段：VirtualDisplayAdapter.VirtualDisplayDevice
目标：理解 SurfaceControl display、Surface、死亡与 release
```

最后再用第 12 章知识补全：

```text
Layer buffer → SurfaceFlinger composition → virtual display target Surface
```

---

## 25. 本章心智模型

```text
【授权层】
App → SystemUI 用户确认 → ProjectionService 签发并激活 IMediaProjection
                                      │
                                      │ 允许 project video / 约束 flags
                                      ▼
【显示控制层】
DisplayManagerGlobal → DMS → VirtualDisplayAdapter
                           → LogicalDisplay + VirtualDisplayDevice
                                      │
                                      │ displayToken + target Surface
                                      ▼
【图像数据层】
窗口 Layer → SurfaceFlinger 合成 → BufferQueue → ImageReader 或 MediaCodec
                                                       │
                                                       ▼
                                                图片/编码视频/网络流

【独立音频层】
AudioPlaybackCapture → AudioRecord PCM → 音频编码 ───────┘（封装时汇合）
```

一句话总结：

> **MediaProjection 是可撤销的捕获授权，VirtualDisplay 把显示系统连接到 App 指定的 Surface，SurfaceFlinger 负责产生图像帧；App 必须自己消费、编码、保存或传输这些帧，并单独处理音频和完整生命周期。**

---

## 26. 自测题

1. `MediaProjection`、`VirtualDisplay`、`Surface` 各解决什么问题？
2. 为什么 `createScreenCaptureIntent()` 返回 Intent，而不是直接返回授权对象？
3. SystemUI 如何避免 App 冒充别的包或用遮罩诱导授权？
4. `getMediaProjection()` 构造对象时为什么立即跨 Binder 调用 start？
5. Android 11 为什么检查 mediaProjection 类型前台服务？
6. `IMediaProjection` 与 `IVirtualDisplayCallback` 两个 token 分别有什么作用？
7. DMS 为什么还要重新校验 packageName、Projection 和 flags？
8. `AUTO_MIRROR` 与 `OWN_CONTENT_ONLY` 有何冲突？
9. `VirtualDisplayDevice` 与 App 得到的 `Display` 有何不同？
10. 屏幕帧怎样从 SurfaceFlinger 到 ImageReader 或 MediaCodec？
11. `FLAG_SECURE` 窗口与 `VIRTUAL_DISPLAY_FLAG_SECURE` 有何区别？
12. `VirtualDisplay.release()` 与 `MediaProjection.stop()` 作用域有何不同？
13. 为什么录屏没有声音不能只检查 VirtualDisplay？
14. remote playback、整屏镜像和第二屏 Presentation 有何区别？

若你能不看文档画出“授权、显示控制、图像数据、音频数据”四层图，并回答 6、11、12、13，本章就掌握了。

---

## 27. 下一章预告

下一章进入：

```text
67 DisplayManagerService、DisplayDevice 与多显示器管理链路
```

届时不再只看录屏用的 VirtualDisplay，而是从 DisplayAdapter、DisplayDevice、LogicalDisplay、DisplayInfo、DisplayPowerController、WMS DisplayContent 和 Display 事件入手，建立物理内屏、HDMI、Wi-Fi Display、Overlay Display 与 VirtualDisplay 共用的显示管理模型。
