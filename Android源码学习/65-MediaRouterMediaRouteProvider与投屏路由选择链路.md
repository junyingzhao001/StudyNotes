# 65 MediaRouter、MediaRouteProvider 与投屏路由选择链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：理解媒体路由是什么，分清 Android 11 中旧 `MediaRouter`、新 `MediaRouter2` 与 AndroidX MediaRouter，追踪路由发现、Provider 发布、跨 Provider 切换、`RoutingSessionInfo`、远端音量和释放会话的完整控制链，并明确“选择投屏设备”与“真正传输音视频数据”不是同一件事。

---

## 1. 先回答：MediaRouter 到底在“路由”什么

假设手机正在播放一首歌，界面出现三个输出目标：

```text
本机扬声器
客厅电视
卧室智能音箱
```

MediaRouter 体系负责的是：

1. 收集当前可用的媒体输出目标；
2. 告诉应用每个目标支持什么能力；
3. 接收用户选择；
4. 协调目标 Provider 建立或改变一次路由会话；
5. 把会话、选中设备和音量变化回传给应用。

它首先是一套**发现和控制协议**，并不是统一的音视频传输协议。

```text
控制面：发现电视 → 选择电视 → 建立会话 → 调音量 → 释放会话
数据面：视频 URL、压缩码流、PCM、画面像素究竟怎样到电视
```

数据面取决于具体实现：

- 本机扬声器通常进入 `AudioTrack → AudioFlinger → AudioPolicy → Audio HAL`；
- 蓝牙音频由音频策略和蓝牙协议栈配合输出；
- 远端播放 Provider 可能把媒体 URL 和控制命令发给电视，让电视自己拉流；
- 屏幕镜像可能经过 Wi-Fi Display、编码器和网络传输；
- `MediaProjection + VirtualDisplay` 则是另一套屏幕捕获入口。

所以第一条必须记住：

> **路由选择成功，只代表控制面建立了目标和会话；不能据此推断媒体字节经过 MediaRouter。**

---

## 2. Android 11 有三套容易混淆的名字

搜索源码前一定先辨认版本和包名。

| 名字 | 所在位置 | Android 11 中的角色 |
|---|---|---|
| `android.media.MediaRouter` | 平台 Framework | 较早的路由 API，包含 `RouteInfo`、`RouteCategory`、`UserRouteInfo` 和远端显示逻辑 |
| `android.media.MediaRouter2` | 平台 Framework，API 30 | 新的 session/provider 模型，本章主线 |
| `android.media.MediaRoute2ProviderService` | 平台 Framework，API 30 | 第三方/远端路由提供者发布 route 和管理 session 的 Service 基类 |
| `androidx.mediarouter.media.MediaRouter` | AndroidX 仓库 | 面向普通应用、兼容多个系统版本的支持库实现 |
| `androidx.mediarouter.media.MediaRouteProvider` | AndroidX 仓库 | AndroidX Provider 抽象；**不在当前 AOSP 平台目录中** |

章节标题沿用常见说法“MediaRouteProvider”，但阅读当前 Android 11 平台源码时，真正要找的是：

```java
android.media.MediaRoute2ProviderService
```

`MediaRouter2` 类注释还明确提示：这套平台 API 并非普遍推荐给第三方应用；需要跨版本一致行为时通常使用 AndroidX MediaRouter。我们学习平台源码，是为了理解 Android 11 system_server 内部如何聚合路由与管理会话。

### 2.1 为什么不把两代 API 混成一条调用链

旧模型更偏向“选择一个 `RouteInfo`”；新模型把 Provider、route、routing session、controller 分开建模。两者共存不代表一次调用会先经过旧 API 再经过新 API。

```text
旧入口：MediaRouter → IMediaRouterService → legacy client state / remote display provider

新入口：MediaRouter2 → IMediaRouterService → MediaRouter2ServiceImpl
                                      → MediaRoute2Provider
```

后面以新链为主，末尾再解释旧链。

---

## 3. 核心源码地图

### 3.1 App/Provider 侧公开对象

```text
frameworks/base/media/java/android/media/
    MediaRouter2.java
    MediaRouter2Manager.java
    MediaRoute2Info.java
    MediaRoute2ProviderInfo.java
    MediaRoute2ProviderService.java
    RouteDiscoveryPreference.java
    RoutingSessionInfo.java
```

### 3.2 Binder 协议

```text
frameworks/base/media/java/android/media/
    IMediaRouterService.aidl
    IMediaRouter2.aidl
    IMediaRouter2Manager.aidl
    IMediaRoute2ProviderService.aidl
    IMediaRoute2ProviderServiceCallback.aidl
```

注意：`MediaRouter2` 仍通过名为 `IMediaRouterService` 的总入口访问系统服务，不能因为 AIDL 没叫 `IMediaRouter2Service` 就认为找错了。

### 3.3 system_server 实现

```text
frameworks/base/services/core/java/com/android/server/media/
    MediaRouterService.java
    MediaRouter2ServiceImpl.java
    MediaRoute2Provider.java
    MediaRoute2ProviderWatcher.java
    MediaRoute2ProviderServiceProxy.java
    SystemMediaRoute2Provider.java
    BluetoothRouteProvider.java
```

### 3.4 旧远端显示链

```text
frameworks/base/services/core/java/com/android/server/media/
    RemoteDisplayProviderWatcher.java
    RemoteDisplayProviderProxy.java

frameworks/base/media/java/android/media/
    RemoteDisplayState.java
    IRemoteDisplayProvider.aidl
```

---

## 4. 六个对象先分清

### 4.1 `MediaRoute2Info`：一个候选目标的快照

它描述“可以把媒体路由到哪里”，常见信息包括：

```text
id / name
features
type
connectionState
volumeHandling / volumeMax / volume
isSystemRoute
providerId
```

`features` 表示能力匹配，例如：

```java
FEATURE_LIVE_AUDIO
FEATURE_LIVE_VIDEO
FEATURE_REMOTE_PLAYBACK
FEATURE_REMOTE_AUDIO_PLAYBACK
FEATURE_REMOTE_VIDEO_PLAYBACK
```

重点：`MediaRoute2Info` 是一次状态快照，不是永远在线的设备对象。设备消失、能力改变或音量变化后，应用应以回调收到的新对象为准。

### 4.2 `MediaRoute2ProviderInfo`：一个 Provider 发布的路由集合

Provider 调用：

```java
notifyRoutes(routes);
```

基类会构造新的 `MediaRoute2ProviderInfo`，再通过 Binder callback 把整体状态发布给 system_server。

这是一种**声明式状态**：Provider 告诉系统“我现在有哪些 route”，而不是逐条命令系统添加/删除永久对象。

### 4.3 `RouteDiscoveryPreference`：客户端想找什么

它包含：

- preferred features：希望 route 支持哪些能力；
- active scan：是否需要更积极地扫描。

同一个 `MediaRouter2` 可以注册多个 `RouteCallback`。客户端先合并自己的偏好，system_server 再聚合多个 Router 的偏好并通知 Provider。

因此它不是“扫描到的结果”，而是“扫描需求”。

### 4.4 `RoutingSessionInfo`：一次连接/选路会话的事实

它描述：

```text
ownerPackageName / clientPackageName
providerId
selectedRoutes
selectableRoutes
deselectableRoutes
transferableRoutes
volumeHandling / volumeMax / volume
controlHints
isSystemSession
```

一条 session 可以含多条 selected route，所以它能表达多房间音箱等组播场景。

### 4.5 `MediaRouter2.RoutingController`：客户端控制句柄

Controller 包装一条 `RoutingSessionInfo`，向客户端暴露：

```text
selectRoute / deselectRoute
setVolume
release
```

公开的“转移当前媒体”入口在外层 `MediaRouter2.transferTo(route)`。它检查目标是否属于当前 session 的 `transferableRoutes`：若属于，内部调用 package-private 的 `RoutingController.transferToRoute()`；否则请求目标 Provider 创建新 controller。这个可见性细节很重要，业务代码不应直接调用内部方法。

系统路由对应的 system controller 不能像普通远端 session 一样释放；普通远端 controller 释放后不应继续使用。

### 4.6 Provider：能力和真实设备的拥有者

两类 Provider 要分开：

- `SystemMediaRoute2Provider`：由 system_server 内部创建，代表系统默认、本机/有线/HDMI/USB/蓝牙等系统路由；
- `MediaRoute2ProviderServiceProxy`：system_server 对一个外部 `MediaRoute2ProviderService` 的代理。

Provider 才知道怎样发现、连接和控制它背后的设备。Router 和 system_server 主要负责编排、权限、归属与状态转发。

---

## 5. 总体架构：三个进程、两组 Binder

```text
┌──────────────── 媒体 App 进程 ────────────────┐
│ MediaRouter2                                  │
│ RouteCallback / TransferCallback              │
│ RoutingController                             │
└──────────── IMediaRouterService / IMediaRouter2 ───────┐
                                                         │ Binder
┌──────────────── system_server ─────────────────────────▼┐
│ MediaRouterService                                      │
│   └─ MediaRouter2ServiceImpl                            │
│       ├─ UserRecord / RouterRecord                      │
│       ├─ SystemMediaRoute2Provider                      │
│       ├─ MediaRoute2ProviderWatcher                     │
│       └─ MediaRoute2ProviderServiceProxy                │
└──────── IMediaRoute2ProviderService + callback ─────────┐
                                                         │ Binder
┌──────────────── Provider App 进程 ─────────────────────▼┐
│ MediaRoute2ProviderService                              │
│   ├─ 发现真实电视/音箱                                  │
│   ├─ notifyRoutes()                                     │
│   ├─ onCreateSession()/onTransferToRoute()              │
│   └─ notifySessionCreated/Updated/Released()             │
└──────────────────────────────────────────────────────────┘
```

两个跨进程方向：

1. 媒体 App ↔ system_server：注册 Router、收到 route/session、发起 transfer；
2. system_server ↔ Provider App：下发发现偏好和会话命令、接收 Provider 状态。

真正的电视控制协议或媒体流一般在 Provider 与设备之间，图中没有把它画成 MediaRouter Binder。

---

## 6. 第一条主线：Router 注册与路由发现

### 6.1 获取 Router 时连接系统服务

`MediaRouter2.getInstance(context)` 创建进程内实例，并从 ServiceManager 获得 `IMediaRouterService`。真正让系统开始关心该客户端的是 route callback 和 discovery preference。

应用侧概念代码：

```java
MediaRouter2 router = MediaRouter2.getInstance(context);

RouteDiscoveryPreference preference =
        new RouteDiscoveryPreference.Builder(
                List.of(MediaRoute2Info.FEATURE_REMOTE_VIDEO_PLAYBACK),
                false /* activeScan */)
                .build();

router.registerRouteCallback(executor, callback, preference);
```

这里的 callback 和 executor 都很重要：Binder 回调先进入 `MediaRouter2` 内部，再由指定 executor 交给业务代码。不要默认业务回调一定运行在主线程。

### 6.2 多个发现需求会被合并

一条完整路径可以概括为：

```text
registerRouteCallback(callback, preference)
→ MediaRouter2 更新本地 callback 记录
→ 首次需要时 registerRouter2(IMediaRouter2, packageName)
→ setDiscoveryRequestWithRouter2(...)
→ MediaRouter2ServiceImpl 保存 RouterRecord.mDiscoveryPreference
→ UserHandler 聚合当前用户所有 Router 的发现偏好
→ provider.updateDiscoveryPreference(aggregatedPreference)
→ MediaRoute2ProviderService.onDiscoveryPreferenceChanged()
```

Provider 文档要求它检查 preferred features：没有匹配需求时可以停止扫描，减少网络广播、蓝牙扫描和功耗。

### 6.3 system_server 不是永远绑定所有 Provider

`MediaRoute2ProviderServiceProxy.shouldBind()` 的关键判断是：

```java
return (mLastDiscoveryPreference != null
        && !mLastDiscoveryPreference.getPreferredFeatures().isEmpty())
        || !getSessionInfos().isEmpty();
```

也就是满足以下任一条件才保持绑定：

- 当前确实有人请求发现某类 route；
- Provider 还有活动 session，需要继续控制。

这是一个很好的系统设计点：**发现需求消失可以解绑，但活动播放会话不能因为 UI 关闭就立刻断开。**

### 6.4 Provider Watcher 怎样找到服务

`MediaRoute2ProviderWatcher` 对当前用户查询：

```java
new Intent(MediaRoute2ProviderService.SERVICE_INTERFACE)
```

其 action 常量是：

```java
"android.media.MediaRoute2ProviderService"
```

Watcher 还监听包安装、卸载、变更、替换和重启广播，重新扫描并增删 Proxy。因此 Provider App 更新或崩溃后，代理可以重新建立状态。

### 6.5 Provider 发布 route

Provider 发现电视后调用：

```java
notifyRoutes(currentRoutes);
```

内部主线：

```text
notifyRoutes
→ 构造 MediaRoute2ProviderInfo
→ schedulePublishState
→ 主线程 publishState
→ IMediaRoute2ProviderServiceCallback.updateState
→ Proxy 更新 Provider 状态
→ MediaRouter2ServiceImpl 汇总并过滤
→ IMediaRouter2.notifyRoutesUpdated
→ MediaRouter2 比较前后快照
→ RouteCallback.onRoutesAdded/Changed/Removed
```

注意两个细节：

1. `notifyRoutes()` 发布的是当前集合，设备消失时要发布不含它的新集合；
2. 回调中的 added/changed/removed 是 Router 根据前后状态推导出的视图，不应被理解为 Provider 永久事件日志。

---

## 7. ID 为什么有“原始 ID”和“全局唯一 ID”

不同 Provider 都可能把第一台设备命名为 `living_room`。若 system_server 只保存原 ID，就会冲突。

```text
Provider A: living_room
Provider B: living_room
```

因此服务端给 route/session 加上 Provider 身份，形成全局唯一 ID；向对应 Provider 下发命令时再取回 original ID。

源码中经常看到：

```text
route.getProviderId()
route.getOriginalId()
getProviderId(uniqueSessionId)
getOriginalId(uniqueSessionId)
```

这解释了两个常见疑问：

- 为什么 `MediaRoute2Info.getId()` 看起来不像 Provider 自己创建的 ID；
- 为什么 system_server 调用 Provider 前总要执行 `getOriginalId()`。

学习时不要手工拆字符串，把它当成“跨 Provider 命名空间”即可。

---

## 8. 第二条主线：用户选择 route，创建远端 session

应用调用：

```java
router.transferTo(targetRoute);
```

这是异步请求。正确结果由 `TransferCallback` 告知，而不是由 `transferTo()` 的返回值代表。

### 8.1 完整时序

```text
用户点击“客厅电视”
  ↓
MediaRouter2.transferTo(targetRoute)
  ↓
确定当前 controller / oldSession，创建 requestId
  ↓ Binder
IMediaRouterService.requestCreateSessionWithRouter2(...)
  ↓
MediaRouter2ServiceImpl 校验 Router、route、权限和请求对应关系
  ↓
根据 route.providerId 找到 MediaRoute2Provider
  ↓
provider.requestCreateSession(uniqueRequestId,
                              clientPackageName,
                              route.originalId,
                              sessionHints)
  ↓ Binder
MediaRoute2ProviderService.onCreateSession(...)
  ↓
Provider 与真实设备建立连接/控制会话
  ↓
Provider 构造 RoutingSessionInfo，包含目标 selected route
  ↓
notifySessionCreated(requestId, sessionInfo)
  ↓ Binder callback
Proxy → MediaRouter2ServiceImpl.onSessionCreatedOnHandler
  ↓
匹配 SessionCreationRequest，记录 session 属于哪个 Router
  ↓ Binder callback
IMediaRouter2.notifySessionCreated
  ↓
MediaRouter2 创建新的 RoutingController
  ↓
TransferCallback.onTransfer(oldController, newController)
```

若 Provider 无法连接，应调用：

```java
notifyRequestFailed(requestId, reason);
```

原因可区分拒绝、网络错误、route 已不可用或命令非法。应用最终收到 transfer failure，而不是无限等待成功。

### 8.2 `requestId` 是异步结果配对键

Provider 建连可能要数秒。请求和结果跨 Binder、跨线程，不能依靠调用栈返回值匹配。`requestId` 负责把：

```text
onCreateSession(requestId, ...)
```

与下面二者之一配对：

```text
notifySessionCreated(requestId, sessionInfo)
notifyRequestFailed(requestId, reason)
```

基类最多记住 500 个收到的 request ID，重复或未知 ID 会被拒绝。这能防止 Provider 错把旧结果应答给新请求。

### 8.3 成功回调前为什么还要检查 session

Provider 返回的 session 至少必须：

- ID 唯一；
- 属于正确 Provider；
- selected routes 中包含请求目标；
- route/session 数据符合当前请求。

控制系统不能仅因为远端发来“成功”就无条件接受错误归属的会话。

### 8.4 切换不是瞬间删除旧 controller

新 session 创建、App 真正把播放交给新端点、旧 session 释放之间可能需要过渡。`MediaRouter2` 中为旧 controller 保留了最长约 30 秒的 transfer timeout 机制。

可以把它理解成：

```text
旧输出仍可控
→ 新输出连接成功
→ 应用/Provider 完成媒体交接
→ 释放旧输出
```

这不是 30 秒后媒体一定自动转过去，而是避免旧 controller 永远悬挂的生命周期兜底。

---

## 9. 同一 Provider 内的 route 选择与跨 Provider transfer

这两个动作很容易混淆。

### 9.1 session 内 select/deselect

若 session 声明某条 route 在 `selectableRoutes` 中，Controller 可调用：

```java
controller.selectRoute(route);
```

Provider 收到：

```java
onSelectRoute(requestId, sessionId, routeId)
```

完成后 Provider 不能只改自己的 socket 状态，还应发布新的：

```java
notifySessionUpdated(updatedSessionInfo);
```

类似地，只有列在 `deselectableRoutes` 中的 route 才能合理移除。

这适合“一条会话加入第二个音箱”的场景。

### 9.2 session 内 transfer

若目标在当前 session 的 `transferableRoutes` 中，应用仍调用公开入口 `MediaRouter2.transferTo(route)`；Router 内部会让当前 Controller 请求 Provider 在同一 session 语义下转移：

```text
MediaRouter2.transferTo(route)
→ RoutingController.transferToRoute(route)  // package-private 内部步骤
→ onTransferToRoute(...)
→ notifySessionUpdated(...)
```

### 9.3 跨 Provider 通常创建新 session

从本机系统 Provider 转到某第三方电视 Provider，旧 Provider 不可能直接控制新 Provider 的 route。因此中心服务找到目标 Provider，并让它创建新的 session，再完成 controller 交接。

判断口诀：

```text
同一 Provider、现有 session 支持 → select/deselect/transfer session state
不同 Provider → 目标 Provider 创建新 session，完成 controller transfer
```

---

## 10. `RoutingSessionInfo` 不是一串 selected route 就结束了

以三音箱组播为例：

```text
selectedRoutes     = [客厅]
selectableRoutes   = [厨房, 卧室]
deselectableRoutes = []
transferableRoutes = [书房]
```

含义是：

- 客厅正在播放；
- 可以把厨房或卧室加入当前组；
- 客厅当前不可被单独移除，可能因为 session 至少要保留一个目标；
- 可以把整个会话转到书房。

这些集合是 Provider 对当前能力的声明，不是 UI 自己推算的。操作完成或拓扑变化后，Provider 应发布新 session 快照。

`controlHints` 则允许 Provider 附带特有控制信息，但它不是跨 Provider 通用媒体传输协议。客户端必须把它当作可选、Provider 特定的数据。

---

## 11. 第三条主线：音量到底改的是谁

MediaRouter2 中至少有两种音量对象。

### 11.1 route volume

```text
MediaRouter2.setRouteVolume(route, volume)
→ MediaRouter2ServiceImpl
→ 根据 route.providerId 找 Provider
→ provider.setRouteVolume(requestId, route.originalId, volume)
→ MediaRoute2ProviderService.onSetRouteVolume(...)
```

它针对单条 route，例如只调卧室音箱。

### 11.2 session volume

```text
RoutingController.setVolume(volume)
→ setSessionVolumeWithRouter2
→ 根据 session.providerId 找 Provider
→ onSetSessionVolume(requestId, session.originalId, volume)
```

它针对整个会话，例如一组同步音箱的主音量。

### 11.3 fixed 与 variable

`volumeHandling` 表示：

- fixed：该对象不接受可变音量控制；
- variable：可以在 `0..volumeMax` 中设置。

发送 `setVolume(8)` 只是命令，不等于客户端本地的 `volume` 字段立刻成为 8。Provider 完成真实设备操作后，应通过新的 route 或 session 状态公布最终事实。

### 11.4 与 MediaSession remote volume 的关系

上一章的 `MediaSession` 也能通过 `VolumeProvider` 声明远端音量。这是“当前播放会话怎样响应系统音量键”的入口；本章的 route/session volume 是“路由设备或路由会话怎样改变音量”。产品实现需要把二者协调起来，但两个 API 对象不会凭名字相同就自动共享状态。

---

## 12. 系统路由为什么特殊

`SystemMediaRoute2Provider` 代表系统掌控的音频输出，并持有 system session。它还结合 `BluetoothRouteProvider` 反映蓝牙路由。

普通应用看到的系统 route 受权限过滤。源码在创建 session 时明确检查：

```java
if (route.isSystemRoute()
        && !routerRecord.mHasModifyAudioRoutingPermission
        && route != defaultRoute) {
    // fail
}
```

也就是说，没有 `MODIFY_AUDIO_ROUTING` 的普通 Router 不能用该接口任意强制切换系统音频设备；系统可能只向它暴露默认系统 route/session 的受限视图。

这是很容易误读的一点：源码里存在“切到某系统 route”的能力，不代表任意三方 App 都拥有这项权力。

此外，系统 controller 代表系统持续存在的路由环境，不能按普通远端 controller 的方式 `release()`。切回本机也更接近“转移到系统 session”，不是把系统音频基础设施销毁。

---

## 13. 生命周期：关闭选路 UI，不等于停止远端播放

需要分开四件事：

| 动作 | 影响 |
|---|---|
| 注销 RouteCallback | 不再接收某类发现结果，可能降低/停止扫描 |
| Activity 销毁 | UI 生命周期结束，不必然释放 routing session |
| Controller.release | 请求 Provider 释放对应远端 session |
| Provider/Router Binder 死亡 | system_server 清理记录并尝试解绑、通知或恢复状态 |

远端 session 还存在时，Proxy 的 `shouldBind()` 仍为真，所以发现 UI 消失后 system_server 仍可保持 Provider 连接。

释放完整链路：

```text
RoutingController.release()
→ IMediaRouterService.releaseSessionWithRouter2
→ MediaRouter2ServiceImpl 校验 session 属于该 Router
→ provider.releaseSession(uniqueRequestId, originalSessionId)
→ MediaRoute2ProviderService.onReleaseSession(...)
→ Provider 关闭真实连接
→ notifySessionReleased(sessionId)
→ system_server 清理 session→router 映射
→ MediaRouter2 使 controller 失效并回调
```

Provider 调用 `notifySessionReleased()` 不会反过来再次触发 `onReleaseSession()`，否则会形成递归协议。

---

## 14. 路由选择、远端播放、屏幕镜像、VirtualDisplay 的区别

这是本章最重要的防混淆表。

| 场景 | 核心含义 | MediaRouter 的位置 |
|---|---|---|
| 本机音频换到蓝牙耳机 | 系统音频策略改变输出设备 | 可把设备呈现为 system route；真正 PCM 路径仍在 Audio 系统 |
| 把网络视频投到电视 | 电视成为 remote playback target | 负责发现、选择、会话、控制；URL/码流传输由 Provider 协议完成 |
| Wi-Fi Display 镜像整屏 | 编码并发送屏幕内容 | 旧 MediaRouter 可参与远端显示选择，但显示/编码/网络链是另一层 |
| App 用 MediaProjection 录屏 | 获取屏幕捕获授权并创建 VirtualDisplay | 不是 `MediaRouter2.transferTo()` 的结果；下一章单独追踪 |
| 在第二块物理屏显示 Presentation | DisplayManager 管理另一个 Display | route 可提供显示入口，但窗口渲染由 Display/WMS/Surface 系统完成 |

所以“投屏”是产品语言，源码里可能对应完全不同的技术：

```text
remote playback：远端设备自己播放媒体
screen mirroring：手机画面编码后镜像
second display：App 在另一 Display 上单独渲染
```

排查问题前必须先问：现在是哪一种。

---

## 15. 旧 `android.media.MediaRouter` 在做什么

Android 11 保留旧 API。它使用 `RouteInfo` 和 route type，例如 live audio、live video、remote display、user route。

旧链大致是：

```text
App MediaRouter
→ IMediaRouterService.registerClientAsUser
→ system_server 保存 ClientRecord / ClientState
→ 系统或 RemoteDisplayProvider 更新 route
→ clientStateChanged 回调
→ App 更新 RouteInfo
```

远端显示 Provider 由：

```text
RemoteDisplayProviderWatcher
→ RemoteDisplayProviderProxy
→ IRemoteDisplayProvider
```

维护。选择远端显示后，system_server 把 selected display ID 和音量请求下发给对应 Provider。

### 15.1 旧 `UserRouteInfo` 容易造成的误解

旧 API 允许 App 创建 user route，并设置 playback type、volume callback 等。这是旧模型中由应用描述自定义目标的方式，不等同于新模型的 `MediaRoute2ProviderService + RoutingSessionInfo`。

### 15.2 阅读旧代码时的策略

第一次学习不建议同时逐行追两代状态机。先掌握新模型的：

```text
route → provider → session → controller
```

再回头将旧模型理解为：

```text
RouteInfo 列表 + selected route + client state + remote display provider
```

这样不会把 `RouteInfo` 和 `MediaRoute2Info`、`UserRouteInfo` 和 `MediaRoute2ProviderService` 错配。

---

## 16. 权限、安全和多用户边界

### 16.1 system_server 验证调用者身份

注册 Router 时，服务端保存：

```text
uid / pid / packageName / userId
是否有 CONFIGURE_WIFI_DISPLAY
是否有 MODIFY_AUDIO_ROUTING
```

不能只相信客户端传入的 packageName；系统服务会把 Binder 调用身份和包归属关联起来。

### 16.2 session 有所属 Router

`MediaRouter2ServiceImpl` 保存 session 到 Router 的映射。select、deselect、release 等操作会校验请求 Router 是否匹配，避免 App A 操纵 App B 建立的远端会话。

系统 session 是特例，因为它不是某一个普通 Router 独占。

### 16.3 按用户发现 Provider

Watcher 使用 `queryIntentServicesAsUser(..., userId)`，Proxy 也带 `mUserId` 绑定。工作资料、次用户与主用户不能简单共享一份 Provider 会话集合。

### 16.4 不要把发现结果当成授权结果

“看见 route”不一定代表可以连接：

- route 可能在选择时已经消失；
- Provider 可能拒绝目标客户端；
- 网络连接或设备认证可能失败；
- 系统 route 可能需要特权权限。

最终必须以 session creation 成功回调为准。

---

## 17. 线程模型与一致性

### 17.1 Provider 回调为什么主要落在主线程

`MediaRoute2ProviderService` 的 Binder Stub 不直接在 Binder 线程执行抽象回调，而是把消息投递到 `mHandler`；构造函数使用主 Looper。

```text
Binder thread
→ Stub 校验 routeId/sessionId/requestId
→ Handler message
→ Service 主线程 onCreateSession/onSelectRoute/...
```

因此 Provider 不应在这些回调中同步做长时间网络连接。它应启动异步工作，完成后再 `notify...`。

### 17.2 system_server 也做线程收口

Binder 入口先在锁内查 `RouterRecord/UserRecord`，随后把实际 Provider 操作发送到对应用户的 `UserHandler`。这样每用户的 route/session 状态可以按顺序处理，避免大量 Binder 线程同时改状态图。

### 17.3 快照比命令更可信

正确思维是：

```text
命令：我希望音量变为 8
事实：Provider 随后发布的 route/session 显示音量为 8
```

若真实设备只接受 0、5、10，Provider 可能最终公布 10。UI 应展示事实快照，而不是永远乐观保留请求值 8。

---

## 18. 常见误解逐个拆开

### 误解 1：`transferTo()` 返回就说明投屏成功

不对。它只是开始异步请求，最终看 `TransferCallback`。

### 误解 2：route 的 `connectionState=CONNECTED` 就是当前正在播放

不一定。连接状态描述 route 连接性；当前是否属于播放会话要看 `RoutingSessionInfo.selectedRoutes`。已经连接的设备可以暂时不播放。

### 误解 3：所有 system route 普通 App 都能选

不对。Android 11 服务端对非默认系统 route 有 `MODIFY_AUDIO_ROUTING` 权限约束和可见性处理。

### 误解 4：Provider 收到 setVolume 后改硬件即可

不完整。还应发布新的 route/session 快照，让系统和客户端知道最终音量。

### 误解 5：取消发现就应该断开远端播放

不对。活动 session 本身就是 Proxy 保持绑定的原因。

### 误解 6：MediaRouter 把视频帧发到电视

通常不对。MediaRouter 管控制面，具体媒体数据由 Provider、播放引擎、显示或网络协议处理。

### 误解 7：AndroidX `MediaRouteProvider` 就在 `frameworks/base`

不对。当前平台对应类是 `MediaRoute2ProviderService`；AndroidX 类要去 AndroidX 源码找。

### 误解 8：一个 controller 永远只对应一台设备

不对。`selectedRoutes` 是列表，一条 session 可以管理一组 route。

---

## 19. 三条源码阅读路线

### 路线 A：只读懂路由发现

```text
1. MediaRouter2.registerRouteCallback
2. RouteDiscoveryPreference
3. MediaRouter2ServiceImpl.setDiscoveryRequestWithRouter2Locked
4. UserHandler.updateDiscoveryPreferenceOnHandler
5. MediaRoute2Provider.updateDiscoveryPreference
6. MediaRoute2ProviderService.onDiscoveryPreferenceChanged
7. notifyRoutes
8. Provider callback updateState
9. Router route callbacks
```

读完应能回答：为什么没有任何客户端发现需求时，第三方 Provider 可以不扫描、不保持绑定？

### 路线 B：只读懂一次跨 Provider transfer

```text
1. MediaRouter2.transferTo
2. requestCreateController
3. IMediaRouterService.requestCreateSessionWithRouter2
4. requestCreateSessionWithRouter2Locked
5. requestCreateSessionWithRouter2OnHandler
6. MediaRoute2ProviderServiceProxy.requestCreateSession
7. MediaRoute2ProviderService.onCreateSession
8. notifySessionCreated
9. onSessionCreatedOnHandler
10. IMediaRouter2.notifySessionCreated
11. TransferCallback
```

读完应能回答：为什么切到另一个 Provider 时要创建新 session，而不只是替换一个 routeId？

### 路线 C：只读懂会话控制

```text
RoutingController.selectRoute / deselectRoute / setVolume / release
以及 MediaRouter2.transferTo（必要时内部进入 RoutingController.transferToRoute）
→ MediaRouter2ServiceImpl 的参数与归属检查
→ 对应 Provider 方法
→ MediaRoute2ProviderService 的 onXxx 回调
→ notifySessionUpdated / notifySessionReleased
→ controller 状态更新
```

读完应能回答：为什么命令之后仍必须等待 Provider 发布新状态？

---

## 20. macOS 只读练习

以下练习不编译，只在源码中搜索和画链路。

### 练习 1：确认两代 API 同时存在

```bash
ls frameworks/base/media/java/android/media/MediaRouter*.java
```

写下 `MediaRouter.java`、`MediaRouter2.java`、`MediaRouter2Manager.java` 的差别。

### 练习 2：找出 Provider Service 全部抽象回调

```bash
rg -n "public abstract void on" \
  frameworks/base/media/java/android/media/MediaRoute2ProviderService.java
```

把结果分成：建连、route 成员改变、音量、释放四组。

### 练习 3：证明 Provider 不是一直绑定

阅读：

```text
MediaRoute2ProviderServiceProxy.shouldBind()
MediaRoute2ProviderServiceProxy.updateBinding()
```

回答发现偏好为空但 session 非空时为何仍绑定。

### 练习 4：追踪 route ID 转换

在 `MediaRouter2ServiceImpl.java` 搜索：

```bash
rg -n "getOriginalId|getProviderId" \
  frameworks/base/services/core/java/com/android/server/media/MediaRouter2ServiceImpl.java
```

选 `setRouteVolumeOnHandler()` 和 `releaseSessionOnHandler()` 各解释一次转换。

### 练习 5：追踪权限检查

搜索：

```bash
rg -n "MODIFY_AUDIO_ROUTING|CONFIGURE_WIFI_DISPLAY" \
  frameworks/base/services/core/java/com/android/server/media/MediaRouter2ServiceImpl.java
```

回答普通 App 为什么不能把平台接口当成任意音频设备切换器。

### 练习 6：画一次创建 session 的请求/响应

必须标出两组 Binder、requestId、Provider original route ID 和客户端看到的 unique session ID。

### 练习 7：比较 route volume 与 session volume

从公开入口一路追到：

```text
onSetRouteVolume
onSetSessionVolume
```

回答多房间音箱中两者可能分别表示什么。

### 练习 8：找出 Provider 的主线程切换

阅读 `MediaRoute2ProviderServiceStub`，确认 Binder 方法如何通过 `mHandler` 调用抽象方法。说明为什么网络连接不能阻塞回调。

### 练习 9：对照旧远端显示链

搜索：

```bash
rg -n "RemoteDisplayProviderWatcher|RemoteDisplayProviderProxy" \
  frameworks/base/services/core/java/com/android/server/media
```

画出旧链与 Router2 Provider 链的两张小图，不把类名混用。

### 练习 10：判断四个产品需求属于哪条数据面

分别判断：蓝牙耳机、电视拉流播放、整屏镜像、App 录屏。每项写出 MediaRouter 是否搬运媒体数据。

---

## 21. 故障排查：从现象反推层次

### 21.1 设备根本不出现在列表

依次检查：

```text
客户端 preferred features 是否匹配
→ discovery preference 是否到达 Provider
→ Proxy 是否绑定成功
→ Provider 是否发现设备并 notifyRoutes
→ route 是否被权限/feature 过滤
→ RouteCallback 是否在正确 executor 收到结果
```

### 21.2 看得到设备但切换失败

```text
route 是否已过期
→ system_server 权限/归属校验是否通过
→ 是否找到正确 providerId
→ Provider onCreateSession 是否收到
→ 真实设备连接是否失败
→ Provider 是否使用原 requestId 回应
→ session 是否包含请求 route
```

### 21.3 UI 显示已连接但没有画面/声音

先不要继续盯 MediaRouter 状态。若 session 和 selected route 已正确，应转向数据面：

```text
远端拉流协议/媒体 URL/DRM
MediaCodec 或播放器状态
AudioFocus 与 AudioPolicy
网络连接
Surface/Display/VirtualDisplay
```

### 21.4 音量命令有效但 UI 回弹

大概率是请求值和 Provider 最终快照不一致，或者 Provider 没有 `notifyRoutes/notifySessionUpdated`。检查 fixed/variable、max 范围和设备实际离散档位。

### 21.5 退出选路页面后播放断了

检查业务是否错误地在 Activity 销毁时调用了 controller.release，或者 Provider 把 discovery preference 清空误当成 release session。

---

## 22. 本章完整心智模型

把全部内容压缩成这一张图：

```text
发现需求
MediaRouter2 + RouteDiscoveryPreference
        ↓
system_server 按用户聚合
        ↓
Provider Proxy 按需绑定
        ↓
Provider 发现设备并发布 MediaRoute2Info 快照
        ↓
应用收到筛选后的 route 列表
        ↓
用户 transferTo(route)
        ↓
system_server 校验身份、权限、Provider 和旧 session
        ↓
目标 Provider 异步创建 RoutingSessionInfo
        ↓
MediaRouter2 获得新 RoutingController
        ↓
应用/Provider 完成真正媒体数据面的交接
        ↓
session 更新：selected/selectable/deselectable/transferable
        ↓
route/session 音量命令 → Provider → 新状态快照
        ↓
release → Provider 释放真实连接 → controller 失效
```

一句话总结：

> **MediaRouter2 把“谁想找什么设备、哪家 Provider 拥有该设备、哪个 App 拥有哪条 session、当前选中了哪些 route”组织成受权限约束的状态机；具体媒体怎样送达设备，仍由音频、显示、播放器和 Provider 协议完成。**

---

## 23. 自测题

1. 为什么说 MediaRouter 是控制面，而不是统一媒体数据面？
2. Android 11 平台 `MediaRoute2ProviderService` 与 AndroidX `MediaRouteProvider` 有什么区别？
3. `MediaRoute2Info`、`RoutingSessionInfo` 和 `RoutingController` 各表示什么？
4. Provider 为什么只在有发现偏好或活动 session 时保持绑定？
5. route 原始 ID 为什么不能直接作为系统全局 ID？
6. `transferTo()` 为什么必须异步回调结果？
7. 同一 session 中 `selectRoute()` 与跨 Provider transfer 有何不同？
8. route volume 与 session volume 有何不同？
9. 为什么关闭选路 UI 不应自动释放远端播放？
10. `connectionState=CONNECTED` 为什么不等于该 route 正在播放？
11. 普通 App 为什么不能任意切换所有系统音频 route？
12. 电视远端拉流、整屏镜像和 MediaProjection 录屏的差别是什么？

如果能不看文档讲清 8、9、11、12，并画出第 8 节的两组 Binder 时序，本章就真正掌握了。

---

## 24. 下一章预告

下一章进入：

```text
66 MediaProjection、VirtualDisplay 与屏幕捕获投射链路
```

它会承接本章最容易混淆的数据面问题：用户授权录屏后，`MediaProjection` 怎样创建 `VirtualDisplay`，屏幕内容怎样进入应用提供的 `Surface`，WMS、DisplayManagerService、SurfaceFlinger 和 MediaProjectionManagerService 分别负责什么，以及屏幕捕获为什么不等于选择 MediaRoute。
