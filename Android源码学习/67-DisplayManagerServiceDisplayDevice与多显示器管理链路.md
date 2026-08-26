# 67 DisplayManagerService、DisplayDevice 与多显示器管理链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：建立 Android 多显示器的统一对象模型，理解各种 DisplayAdapter 如何发现设备，DisplayManagerService 如何把 DisplayDevice 转成 LogicalDisplay 和公开 DisplayInfo，WMS 如何为每个逻辑显示建立 DisplayContent，以及镜像、窗口投放、输入 viewport、电源和刷新率怎样连接起来。

---

## 1. 本章最重要的结论

Android 不会把“插入一块 HDMI 屏”直接等价为“App 得到一个 Display 对象”。中间至少经过：

```text
真实/虚拟输出来源
→ DisplayAdapter 发现
→ DisplayDevice 描述设备出口
→ DisplayManagerService 分配 displayId/layerStack
→ LogicalDisplay 描述系统逻辑显示
→ DisplayInfo 暴露可见状态
→ App 得到 android.view.Display
→ WMS 建立 DisplayContent，承载窗口和 Task
→ SurfaceFlinger 按映射把 Layer 合成到设备
```

最容易混淆的四个对象：

| 对象 | 回答的问题 |
|---|---|
| `DisplayDevice` | 系统现在有哪些显示输出设备？ |
| `LogicalDisplay` | Android 把哪些内容空间组织成逻辑显示？ |
| `Display` | 某个进程可见的逻辑显示 API 快照是什么？ |
| `DisplayContent` | WMS 在这块逻辑显示上管理哪些窗口、Task 和焦点？ |

一句口诀：

> **Device 是输出端，LogicalDisplay 是显示空间，Display 是客户端视图，DisplayContent 是窗口容器。**

---

## 2. 为什么需要“物理设备”和“逻辑显示”两层

如果永远是一块屏显示自己内容，一一对应似乎足够。但系统还要支持：

- 主屏内容镜像到 HDMI/Wi-Fi/VirtualDisplay；
- 一个虚拟输出暂时没有自己的窗口，自动回退镜像主屏；
- 多个物理输出显示同一个逻辑内容；
- 显示设备断开后移除逻辑显示和窗口；
- 屏幕旋转、缩放、overscan，把逻辑坐标映射到设备像素；
- 折叠设备或动态显示拓扑变化。

因此源码故意拆成：

```text
LogicalDisplay：内容坐标系、displayId、layerStack、DisplayInfo
DisplayDevice：SurfaceFlinger display token、物理模式、Surface、状态
```

在 Android 11 常见场景中经常一一对应，但 `configureDisplayLocked()` 已明确允许一个 Device 显示另一个 LogicalDisplay 的内容，这就是镜像。

---

## 3. 核心源码地图

```text
frameworks/base/services/core/java/com/android/server/display/
    DisplayManagerService.java
    DisplayAdapter.java
    DisplayDevice.java
    DisplayDeviceInfo.java
    LogicalDisplay.java
    LocalDisplayAdapter.java
    WifiDisplayAdapter.java
    VirtualDisplayAdapter.java
    OverlayDisplayAdapter.java
    DisplayPowerController.java
    DisplayModeDirector.java

frameworks/base/core/java/android/hardware/display/
    DisplayManager.java
    DisplayManagerGlobal.java
    IDisplayManager.aidl
    IDisplayManagerCallback.aidl

frameworks/base/core/java/android/view/
    Display.java
    DisplayInfo.java

frameworks/base/services/core/java/com/android/server/wm/
    RootWindowContainer.java
    DisplayContent.java
    DisplayArea.java
    TaskDisplayArea.java
```

Native 显示端继续接：

```text
SurfaceControl
→ SurfaceFlinger
→ CompositionEngine / HWC
```

---

## 4. DisplayAdapter：把不同来源翻译成统一事件

`DisplayAdapter` 是发现器和适配层。它不代表一块具体屏，而是可以产生零个或多个 `DisplayDevice`。

```text
LocalDisplayAdapter   → SurfaceFlinger/HWC 本地内屏、HDMI 等
WifiDisplayAdapter    → Wi-Fi Display 远端显示
VirtualDisplayAdapter → App/system 创建的 VirtualDisplay
OverlayDisplayAdapter → 开发者选项模拟副屏
```

统一接口很小：

```java
registerLocked();
sendDisplayDeviceEventLocked(device, ADDED/CHANGED/REMOVED);
sendTraversalRequestLocked();
```

这是一种典型的“多来源归一化”：上游怎样发现设备各不相同，但进入 DMS 后都变成同样的设备事件。

### 4.1 为什么事件异步 post

`sendDisplayDeviceEventLocked()` 不在 Adapter 当前锁栈直接调用 DMS，而是 post 到 Handler：

```java
mHandler.post(() -> mListener.onDisplayDeviceEvent(device, event));
```

这样可以统一事件顺序，避免持有外部栈/Adapter 锁时重入 DMS 的全局 `SyncRoot`。

---

## 5. DMS 启动时注册哪些 Adapter

`registerDefaultDisplayAdapters()` 先注册：

```text
LocalDisplayAdapter
VirtualDisplayAdapter
```

默认显示是系统启动基础，开机流程会等待它出现。Virtual Adapter 也较早注册，因为独立 VR 等设备可能依赖虚拟主显示启动 SetupWizard/Launcher。

稍后 `registerAdditionalDisplayAdapters()` 注册：

```text
OverlayDisplayAdapter
WifiDisplayAdapter（设备配置允许时）
```

安全模式或 only-core 模式会跳过非必要 Adapter，降低故障恢复阶段的依赖。

这说明 Adapter 注册顺序不是随意的：默认显示必须先可用，非核心无线/模拟显示可以后加载。

---

## 6. DisplayDevice：一条显示输出的 system_server 模型

抽象基类保存：

```text
所属 DisplayAdapter
SurfaceFlinger display token
uniqueId
当前 layerStack
orientation
layerStackRect / displayRect
当前输出 Surface
```

子类必须提供 `DisplayDeviceInfo`，其中常见字段包括：

```text
name / uniqueId / address
width / height / densityDpi
supportedModes / modeId / refreshRate
colorModes / hdrCapabilities
state / brightness
flags / type / touch
ownerUid / ownerPackageName
```

### 6.1 `uniqueId`、`displayId`、`modeId` 不要混

```text
uniqueId：设备身份，某些设备可跨重启稳定
displayId：LogicalDisplay 的运行期整数 ID，默认屏固定为 0
modeId：某个分辨率和刷新率组合的 ID
layerStack：SurfaceFlinger 用于选择该显示呈现哪组 Layer 的编号
```

源码当前让 `layerStack == displayId`，旁边注释明确说这并非必须永远相同。不要把实现巧合当概念定义。

### 6.2 DeviceInfo 采用提交式更新

Adapter 发 `CHANGED` 前后，DMS 比较旧 `mDebugLastLoggedDeviceInfo` 与新 info；之后调用：

```java
device.applyPendingDisplayDeviceInfoChangesLocked();
```

约定要求 DeviceInfo 在正式 change 事件前保持稳定，变化时分配/呈现新快照。这样 DMS 能可靠计算 state、color mode 等 diff。

---

## 7. 设备添加主线

以插入 HDMI 或创建 VirtualDisplay 为例：

```text
Adapter 发现输出
→ 构造具体 DisplayDevice
→ sendDisplayDeviceEventLocked(ADDED)
→ DMS Handler
→ handleDisplayDeviceAddedLocked(device)
→ device.getDisplayDeviceInfoLocked()
→ 加入 mDisplayDevices
→ addLogicalDisplayLocked(device)
→ 更新电源状态
→ scheduleTraversalLocked(false)
```

### 7.1 分配 displayId

```java
return isDefault ? Display.DEFAULT_DISPLAY : mNextNonDefaultDisplayId++;
```

默认屏是 0，其他显示运行期递增。外接屏断开重连后，不能仅凭旧 displayId 判断还是同一设备；需要关注稳定 uniqueId/address。

### 7.2 只有一块默认显示

若第二个 Device 也声明 `FLAG_DEFAULT_DISPLAY`，DMS 会记录警告并把它降为非默认显示。系统核心代码大量依赖唯一 `DEFAULT_DISPLAY`。

### 7.3 创建 LogicalDisplay

```java
LogicalDisplay display = new LogicalDisplay(displayId, layerStack, device);
display.updateLocked(mDisplayDevices);
```

有效后加入 `mLogicalDisplays`，清理客户端 DisplayInfo cache，并发送 `EVENT_DISPLAY_ADDED`。

---

## 8. LogicalDisplay：从设备能力推导逻辑视图

`LogicalDisplay` 以 primary DisplayDevice 为基础，维护：

```text
displayId / layerStack
primaryDisplayDevice
baseDisplayInfo
overrideDisplayInfo
最终 DisplayInfo
hasContent
requestedColorMode / mode
display offsets / scalingDisabled
```

### 8.1 base 与 override

可以这样理解：

```text
baseDisplayInfo
→ 从 DisplayDeviceInfo 得到设备原生能力和基础尺寸

overrideDisplayInfo
→ WMS 根据旋转、配置、逻辑尺寸等施加的覆盖

最终 DisplayInfo
→ 合并后给 App、WMS 和系统其他模块读取
```

因此 App 看到的逻辑宽高不一定等于面板物理像素，特别是在旋转、兼容缩放、强制分辨率或桌面多屏场景。

### 8.2 `hasContent` 是镜像决策关键

WMS 在 traversal 中告诉 DMS 某 LogicalDisplay 是否有自己的可见内容。若 Device 允许自动镜像且自己的 LogicalDisplay 没有内容，DMS 可以让它显示 `getDisplayIdToMirrorLocked()` 指定的逻辑显示，找不到时回退默认屏。

这解释了：

```text
外接显示刚连接、尚无 Activity
→ 自动镜像主屏

App 把窗口启动到外接 display
→ hasContent=true
→ 显示自己的 layer stack
```

是否如此还受 Device 的 `FLAG_OWN_CONTENT_ONLY` 等属性约束。

---

## 9. 镜像的真正决策点

`DisplayManagerService.configureDisplayLocked()` 核心逻辑：

```java
LogicalDisplay display = findLogicalDisplayForDeviceLocked(device);

if (!ownContent) {
    if (display != null && !display.hasContentLocked()) {
        display = mLogicalDisplays.get(device.getDisplayIdToMirrorLocked());
    }
    if (display == null) {
        display = mLogicalDisplays.get(Display.DEFAULT_DISPLAY);
    }
}

display.configureDisplayLocked(transaction, device, blanked);
```

所以“设备”和“它显示的逻辑内容”可能分离：

```text
Device B 自己对应 LogicalDisplay B
但 B 没有内容且支持镜像
→ 配置时 Device B 使用 LogicalDisplay A 的 layerStack
```

这正是两层模型存在的价值。

---

## 10. LogicalDisplay 如何把内容映射到设备像素

`LogicalDisplay.configureDisplayLocked()` 计算并写入：

```text
layerStack
display projection orientation
logical layerStackRect
physical displayRect
surface
```

常见计算目标：

- 处理 0/90/180/270 度方向；
- 把逻辑内容按比例放入物理宽高；
- 必要时 letterbox/pillarbox，而非拉伸；
- 应用 display offset；
- 对 blanked display 使用空 layer stack。

最后通过 `SurfaceControl.Transaction` 批量提交。之所以不是每改一个字段立即跨进程，是为了让一轮显示配置原子生效，减少中间态撕裂。

---

## 11. traversal：DMS 与 SurfaceFlinger 的提交时刻

设备 add/change/remove 或 Surface/尺寸变化后，DMS 调度 traversal。核心：

```text
performTraversalLocked(transaction)
→ clearViewportsLocked()
→ 遍历所有 DisplayDevice
   → configureDisplayLocked(transaction, device)
   → device.performTraversalLocked(transaction)
→ 更新 InputManager viewports
→ transaction apply（由外层统一提交）
```

两步职责不同：

```text
DMS.configureDisplayLocked
→ 选择这个 Device 应展示哪个 LogicalDisplay，并计算映射

device.performTraversalLocked
→ 子类提交自己的 pending surface、size 等设备特有变化
```

上一章 `VirtualDisplayDevice` 正是在 `performTraversalLocked()` 中设置 display size 和输出 Surface。

---

## 12. DisplayInfo 怎样到达 App

App 调用：

```java
DisplayManager dm = context.getSystemService(DisplayManager.class);
Display display = dm.getDisplay(displayId);
Display[] displays = dm.getDisplays();
```

主线：

```text
DisplayManager
→ 进程单例 DisplayManagerGlobal
→ IDisplayManager.getDisplayInfo/getDisplayIds
→ DMS 查 LogicalDisplay
→ 按调用 UID 过滤访问权限
→ 返回 DisplayInfo 快照
→ new/update android.view.Display
```

`Display` 不是 system_server 的远程活对象镜像。它持有 `DisplayManagerGlobal` 和本地缓存信息，需要在查询时更新。因此：

```text
Display 对象仍存在
≠ 物理设备一定仍连接
```

应检查 `display.isValid()` 或监听移除事件。

### 12.1 为什么有缓存失效

DMS 新增、移除或改变 LogicalDisplay 时会清理本地 DisplayInfo cache，并发送事件。频繁查询显示参数不必每次都做 Binder，但状态变化必须使旧缓存失效。

---

## 13. DisplayListener 事件链

App 注册：

```java
displayManager.registerDisplayListener(listener, handler);
```

系统链：

```text
DMS.sendDisplayEventLocked(displayId, ADDED/CHANGED/REMOVED)
→ IDisplayManagerCallback.onDisplayEvent
→ DisplayManagerGlobal.DisplayManagerCallback
→ 本进程 Handler/Looper
→ DisplayListener.onDisplayAdded/Changed/Removed
```

它是状态变化提示，不携带完整长期事实。回调后应重新查询 `Display`/参数，并考虑事件到业务执行之间显示又变化。

回调 Handler 决定业务线程，不要默认一定是主线程。

---

## 14. WMS 为什么还需要 DisplayContent

DMS 知道显示尺寸、模式、输出设备，却不负责窗口层级。显示 added 事件到达 WMS 后：

```text
RootWindowContainer.onDisplayAdded(displayId)
→ getDisplayContentOrCreate(displayId)
→ DisplayManager.getDisplay(displayId)
→ new DisplayContent(display, root)
→ 创建 DisplayArea / TaskDisplayArea / window containers
→ 必要时启动 system decorations
```

`DisplayContent` 管理：

```text
该显示的 WindowToken/WindowState
Activity/Task/TaskDisplayArea
焦点窗口与 focused app
Wallpaper、IME、Insets、系统装饰
旋转与 Configuration
SurfaceControl layer hierarchy
```

对象关系：

```text
RootWindowContainer
├─ DisplayContent(displayId=0)
│  └─ DisplayArea / Task / Activity / Window
└─ DisplayContent(displayId=2)
   └─ DisplayArea / Task / Activity / Window
```

多屏不是只多一个 framebuffer，而是 WMS 多出一棵窗口/任务子树。

---

## 15. Activity 如何被启动到指定 Display

系统或有权限的调用可通过 ActivityOptions 指定 launch display。ATMS/WMS 会检查：

```text
目标 displayId 是否存在
调用者是否允许在该 display 启动
Display 是否 private、owner 是否匹配
Activity 是否支持多显示/resize
目标 TaskDisplayArea 是否可用
```

成功后 ActivityRecord/Task 属于目标 `DisplayContent`，它创建的窗口和 Surface layer 进入该显示的 layer stack。

这与镜像不同：

```text
镜像：同一逻辑内容被另一个 Device 显示
扩展屏：第二个 LogicalDisplay/DisplayContent 有自己的 Task 和窗口
```

### 15.1 Presentation

`Presentation` 使用目标 `Display` 创建 display-specific Context/Window。它适合在副屏展示独立 UI，但 Activity/窗口能否出现仍受 display access、private/public 和系统策略约束。

---

## 16. private、public、trusted 与 owner

显示不仅有尺寸，还有访问控制属性。

```text
PUBLIC display
→ 可被更广泛使用，常伴随镜像/Presentation 能力

PRIVATE display
→ 通常只允许 owner UID 或已在其上的应用访问

TRUSTED display
→ 可承载更完整的系统窗口/装饰和可信交互，需要特权

ownerUid/ownerPackageName
→ 虚拟显示归属
```

DMS 返回 DisplayInfo 和 WMS 允许 launch 时都会考虑这些属性。知道 displayId 不等于有权在上面读取信息或启动窗口。

不可信虚拟显示不能简单伪装成系统主屏，系统装饰、锁屏内容和输入能力都有额外限制。

---

## 17. 输入系统如何知道触摸对应哪块屏

DMS 每次 traversal 都重建 `DisplayViewport`。根据 DeviceInfo：

```text
默认显示 → VIEWPORT_INTERNAL
外部触摸显示 → VIEWPORT_EXTERNAL
有 uniqueId 的虚拟触摸显示 → VIEWPORT_VIRTUAL
```

Viewport 包含：

```text
displayId / uniqueId
orientation
logicalFrame / physicalFrame
deviceWidth / deviceHeight
是否 active
```

然后发送给 `InputManagerInternal`。InputReader/InputDispatcher 才能把触摸设备坐标映射到正确 Display 的窗口坐标。

所以外接触摸屏“画面正常但点击偏移”通常不是 View 自身问题，要检查：

```text
Display projection
↔ DisplayViewport
↔ input device uniqueId/port association
```

---

## 18. 显示状态、亮度与 DisplayPowerController

`DisplayDeviceInfo.state` 可能是 ON、OFF、DOZE 等。DMS 的全局显示状态变化会遍历 Device：

```text
applyGlobalDisplayStateLocked
→ updateDisplayStateLocked(device)
→ device.requestDisplayStateLocked(state, brightness)
```

若 Device 带 `FLAG_NEVER_BLANK`，DMS 不按普通物理屏方式熄灭它。VirtualDisplay 的 private/auto-mirror flags 会影响这一属性。

物理默认屏的亮度、环境光、doze 和刷新状态主要由 `DisplayPowerController` 协调；真正操作面板通常由 LocalDisplayDevice/SurfaceFlinger/HAL 完成。

不要把：

```text
Display.STATE_OFF
```

理解成 DisplayDevice 对象被删除。off 是设备仍存在但不输出；removed 才是拓扑中不再存在。

---

## 19. 刷新率与 DisplayModeDirector

物理屏可能支持：

```text
1080×2400 @ 60Hz
1080×2400 @ 90Hz
1080×2400 @ 120Hz
```

每组成为 `Display.Mode`。应用窗口可提出 frame rate/mode 偏好，电源、热管理、低功耗、峰值刷新率设置等也会产生约束。

DMS 把应用请求交给 `DisplayModeDirector`，后者综合 votes 得到 `DesiredDisplayModeSpecs`，再下发 Device。

```text
App request
+ settings
+ power/thermal constraints
+ policy
→ DisplayModeDirector votes
→ allowed mode range / preferred mode
→ DisplayDevice.setDesiredDisplayModeSpecsLocked
→ LocalDisplayAdapter/SurfaceFlinger
```

因此 App 请求 120Hz 不等于面板必定切到 120Hz，它只是系统仲裁的一项输入。

---

## 20. 设备 change 与 remove 链路

### 20.1 change

```text
Adapter → DEVICE_EVENT_CHANGED
→ DMS 比较 DisplayDeviceInfo.diff
→ 应用 pending device info
→ updateLogicalDisplaysLocked
→ LogicalDisplay.updateLocked(all devices)
→ 最终 DisplayInfo 若改变
→ EVENT_DISPLAY_CHANGED
→ WMS/App 重新配置
→ 必要时 traversal
```

变化可能来自分辨率、刷新率、颜色模式、rotation、state、Surface 或设备能力。

### 20.2 remove

```text
Adapter → DEVICE_EVENT_REMOVED
→ mDisplayDevices.remove(device)
→ updateLogicalDisplaysLocked
→ primary device 不再存在，LogicalDisplay invalid
→ 从 mLogicalDisplays 删除
→ EVENT_DISPLAY_REMOVED
→ WMS 移除/迁移相关窗口与 Task
→ traversal 清理 SurfaceFlinger 配置
```

外接屏拔掉时，业务不能继续持有旧 displayId 创建窗口。

---

## 21. 四个典型场景放回统一模型

### 21.1 开机内屏

```text
SurfaceFlinger 报告内建 display
→ LocalDisplayAdapter.LocalDisplayDevice
→ FLAG_DEFAULT_DISPLAY
→ LogicalDisplay 0
→ WMS DisplayContent 0
→ Launcher/SystemUI/普通 Activity
```

### 21.2 HDMI 扩展屏

```text
LocalDisplayAdapter 发现外部 display token
→ 非默认 DisplayDevice
→ 新 displayId
→ 新 LogicalDisplay + DisplayContent
→ 可镜像，或在其上启动 Presentation/Activity
```

### 21.3 Wi-Fi Display

```text
WifiDisplayAdapter 发现/连接 sink
→ WifiDisplayDevice
→ DMS 统一成 LogicalDisplay
→ SurfaceFlinger 合成输出
→ 远端显示数据链编码/发送
```

无线发现、会话和码流实现特殊，但进入 DMS 后仍使用统一 Device/LogicalDisplay 模型。

### 21.4 MediaProjection VirtualDisplay

```text
App + Projection token 创建
→ VirtualDisplayAdapter.VirtualDisplayDevice
→ 非默认 LogicalDisplay
→ AUTO_MIRROR 时无自有内容则显示主 LogicalDisplay layer stack
→ 输出 Surface 接 ImageReader/MediaCodec
```

这样就能看清：上一章不是一套独立显示架构，而是统一多显示器模型中的一种 Adapter。

---

## 22. 常见误解修正

### 误解 1：一块 DisplayDevice 永远只显示自己的 LogicalDisplay

不对。允许镜像且无自有内容时，它可以展示另一 LogicalDisplay 的 layer stack。

### 误解 2：`Display` 就是物理屏对象

不对。它是客户端对 LogicalDisplay/DisplayInfo 的视图；虚拟屏和 Wi-Fi 屏也能对应 Display。

### 误解 3：`displayId` 是硬件永久 ID

不对。除默认 0 外通常是运行期分配；识别设备要结合 uniqueId/address。

### 误解 4：Display added 后窗口自然就出现在上面

不对。WMS 还要创建 DisplayContent，Activity/Window 也要被路由到该显示。否则它可能只镜像主屏或没有内容。

### 误解 5：显示 OFF 等同于被拔掉

不对。OFF 是状态变化，REMOVED 是拓扑变化。

### 误解 6：刷新率由 App 一票决定

不对。DisplayModeDirector 综合多个 vote。

### 误解 7：SurfaceFlinger 决定 Activity 放在哪块屏

不对。ATMS/WMS 决定窗口/Task 所属 DisplayContent；SF 负责合成已建立的 Layer/显示映射。

### 误解 8：DMS 管理窗口焦点

不对。DMS 管显示设备和逻辑信息，WMS 的 DisplayContent 管窗口层级与焦点。

### 误解 9：能查询 Display 就能随意在上面启动 Activity

不对。private/owner/trusted 和 launch policy 仍会检查。

### 误解 10：多屏只有画面，没有输入映射问题

不对。DisplayViewport 把触摸设备坐标关联到 displayId，是完整多屏体验的一部分。

---

## 23. 故障排查路径

### 23.1 物理屏已连接但 `getDisplays()` 看不到

```text
Adapter 是否注册
→ 是否发现并构造 DisplayDevice
→ 是否发 ADDED
→ DeviceInfo 是否有效
→ addLogicalDisplayLocked 是否被 demo mode/默认屏规则拦截
→ 调用 UID 是否有访问权
```

### 23.2 Display 存在但黑屏

```text
Device state 是否 ON
→ 输出 Surface/token 是否有效
→ LogicalDisplay.hasContent
→ own-content / mirror 决策
→ layerStack 是否正确
→ projection/displayRect 是否有效
→ SurfaceFlinger/HWC 是否成功提交
```

### 23.3 副屏一直镜像，无法显示独立 Activity

检查 Activity 是否真正启动到目标 display、DisplayContent 是否有可见窗口、WMS 是否把 `hasContent=true` 告诉 DMS，以及显示是否被强制 own-content/mirror 策略限制。

### 23.4 插拔后使用旧 displayId 崩溃

监听 `onDisplayRemoved`，重新查询 Display；不要持久化运行期 displayId 作为设备身份。

### 23.5 分辨率或旋转错误

分层检查：

```text
DisplayDeviceInfo 原生 mode
→ LogicalDisplay base/override DisplayInfo
→ WMS Configuration/rotation
→ layerStackRect/displayRect
→ SurfaceControl projection
```

### 23.6 触摸点与画面错位

检查 DisplayViewport 的 orientation、logicalFrame、physicalFrame，以及 input device 与 display uniqueId/port 的关联。

### 23.7 高刷新率没有生效

检查 App requested mode、DisplayModeDirector votes、电源/热/设置限制、面板 supported modes 和最终 active mode，而不是只看 App 请求值。

---

## 24. macOS 只读源码练习

### 练习 1：列出 Adapter

```bash
rg -n "new .*DisplayAdapter" \
  frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java
```

区分默认和 additional Adapter 的注册时机。

### 练习 2：追一块设备的 added 链

从 `sendDisplayDeviceEventLocked()` 追到 `addLogicalDisplayLocked()`，标出 Handler 和 `mSyncRoot` 边界。

### 练习 3：辨认四种 ID

在 `DisplayDevice.java`、`LogicalDisplay.java` 和 DMS 中找 uniqueId、displayId、layerStack、modeId，分别写一句定义。

### 练习 4：追 DisplayInfo

从 `LogicalDisplay.getDisplayInfoLocked()` 追到 `DisplayManagerGlobal.getDisplayInfo()`，解释 base、override、cache 的作用。

### 练习 5：证明镜像不是复制 Bitmap

阅读 `configureDisplayLocked()` 和 `LogicalDisplay.configureDisplayLocked()`，找出 layerStack 和 SurfaceControl projection。

### 练习 6：追 VirtualDisplay 回统一模型

把第 66 章的 `VirtualDisplayDevice` 接到：

```text
handleDisplayDeviceAddedLocked
→ LogicalDisplay
→ traversal
→ configureDisplayLocked
```

### 练习 7：追 WMS DisplayContent 创建

```text
display added event
→ RootWindowContainer.onDisplayAdded
→ getDisplayContentOrCreate
→ new DisplayContent
```

回答 DisplayContent 与 LogicalDisplay 为什么不能合成一个类。

### 练习 8：追拔屏

从 Device REMOVED 追到 LogicalDisplay invalid、App listener 和 WMS 清理，记录旧 displayId 的命运。

### 练习 9：追输入 viewport

阅读 `getViewportType()`、`populateViewportLocked()` 和 `MSG_UPDATE_VIEWPORT`，画出触摸坐标进入目标 DisplayContent 的连接点。

### 练习 10：比较 OFF 与 REMOVED

分别找到 `requestDisplayStateLocked()` 和 `handleDisplayDeviceRemovedLocked()`，解释对象、事件和窗口的差异。

### 练习 11：观察刷新率仲裁

搜索 `DisplayModeDirector` 的 vote 和 `setDesiredDisplayModeSpecsLocked`，说明 App 请求为什么只是一个输入。

---

## 25. 推荐阅读顺序

```text
第一遍：DisplayAdapter → Device added → addLogicalDisplayLocked
目标：看懂对象怎样出生

第二遍：LogicalDisplay.update/configure → DMS traversal
目标：看懂内容怎样映射到设备

第三遍：DisplayManagerGlobal → DisplayListener
目标：看懂 App 怎样观察显示

第四遍：RootWindowContainer.onDisplayAdded → DisplayContent
目标：看懂窗口和 Task 怎样进入多屏

第五遍：DisplayViewport + DisplayModeDirector + PowerController
目标：补齐输入、刷新率和电源
```

每读一个方法固定问：

```text
当前对象表示 Device、LogicalDisplay 还是 Window hierarchy？
当前 ID 是 uniqueId、displayId、layerStack 还是 modeId？
当前变化是设备事件、逻辑信息事件还是窗口配置事件？
```

---

## 26. 本章完整心智模型

```text
【发现层】
Local / WiFi / Virtual / Overlay DisplayAdapter
                    ↓ ADDED / CHANGED / REMOVED
【设备层】
DisplayDevice + DisplayDeviceInfo + SurfaceFlinger display token
                    ↓ DMS 分配 displayId/layerStack
【逻辑层】
LogicalDisplay + base/override DisplayInfo + hasContent
          ├───────────────┐
          ↓               ↓
【客户端视图】       【窗口层】
android.view.Display   WMS DisplayContent
DisplayListener        DisplayArea/Task/Window/focus
          └───────────────┬──────────────┘
                          ↓
【提交层】
DMS traversal：选择 own content 或 mirror logical display
→ orientation + layerStackRect + displayRect + Surface
→ SurfaceControl.Transaction
→ SurfaceFlinger 合成到物理面板/无线输出/虚拟 Surface

旁路协作：
DisplayViewport → InputManager
DisplayModeDirector → 刷新率 mode
DisplayPowerController → state/brightness
```

一句话总结：

> **DisplayManagerService 把不同 Adapter 发现的输出设备统一成 DisplayDevice，再建立可被系统和 App 使用的 LogicalDisplay；WMS 为每个逻辑显示建立 DisplayContent，DMS 在 traversal 中决定每个 Device 展示自己的内容还是镜像其他逻辑显示，最终由 SurfaceFlinger 完成合成输出。**

---

## 27. 自测题

1. DisplayAdapter、DisplayDevice、LogicalDisplay、Display、DisplayContent 各负责什么？
2. 为什么设备层和逻辑层不能只用一个对象？
3. uniqueId、displayId、layerStack、modeId 有何区别？
4. 设备 ADDED 怎样变成 App 的 `onDisplayAdded()`？
5. `baseDisplayInfo` 和 `overrideDisplayInfo` 分别来自哪里？
6. 外接屏在什么条件下镜像主屏？
7. 镜像与把 Activity 启动到副屏的本质差别是什么？
8. DMS traversal 中 `configureDisplayLocked` 与 Device 的 `performTraversalLocked` 怎样分工？
9. WMS 为什么需要每屏一个 DisplayContent？
10. Display OFF 与 REMOVED 的生命周期差别是什么？
11. 触摸设备怎样通过 DisplayViewport 对应到正确显示？
12. App 请求 120Hz 为什么不保证最终使用 120Hz？
13. VirtualDisplay 怎样接入同一套多显示器模型？

如果能不看文档画出五层对象图，并用 HDMI 扩展屏和 MediaProjection VirtualDisplay 各走一遍 added → content → traversal → output，本章就掌握了。

---

## 28. 下一章预告

下一章进入：

```text
68 DisplayPowerController、自动亮度与屏幕电源状态链路
```

它会从 PowerManagerService 的 display power request 开始，追踪 DisplayPowerController 状态机、AutomaticBrightnessController、环境光传感器、亮度曲线、RampAnimator、DisplayBlanker、LocalDisplayDevice，解释按电源键、自动亮度、Doze/AOD 和真实面板开关怎样协作。
