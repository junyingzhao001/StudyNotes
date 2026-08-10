# Android 11 源码学习笔记

> 源码位置：`/Users/ninebot/androidSource`  
> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`

## 怎么使用这套笔记

请按编号顺序阅读。每一章都包含“本章目标、核心概念、源码入口、阅读步骤、检查题”。不要把目标设成记住所有类名；目标是能说清楚一次调用经过了哪些进程、模块和关键对象。

如果学习中断，恢复时先打开 [00-学习进度.md](./00-学习进度.md)，从“下一步”继续。

## 目录

- [00-学习进度](./00-学习进度.md)
- [01-基础概念介绍](./01-基础概念介绍.md)
- [02-AOSP工程目录介绍](./02-AOSP工程目录介绍.md)
- [03-Android系统启动总览](./03-Android系统启动总览.md)
- [04-init进程与rc脚本](./04-init进程与rc脚本.md)
- [05-Zygote启动与应用孵化](./05-Zygote启动与应用孵化.md)
- [06-SystemServer与系统服务](./06-SystemServer与系统服务.md)
- [07-Binder基础与完整调用链](./07-Binder基础与完整调用链.md)
- [08-Activity启动流程之客户端请求](./08-Activity启动流程之客户端请求.md)
- [09-Activity启动流程之ATMS系统端调度](./09-Activity启动流程之ATMS系统端调度.md)
- [10-Activity启动流程之目标进程与生命周期](./10-Activity启动流程之目标进程与生命周期.md)
- [11-View到Window与ViewRootImpl](./11-View到Window与ViewRootImpl.md)
- [12-Surface到SurfaceFlinger显示链路](./12-Surface到SurfaceFlinger显示链路.md)
- [13-PackageManagerService包管理与APK安装](./13-PackageManagerService包管理与APK安装.md)
- [14-综合实践修改编译与验证](./14-综合实践修改编译与验证.md)
- [15-AMS进程管理与LMKD](./15-AMS进程管理与LMKD.md)
- [16-Broadcast广播注册与分发](./16-Broadcast广播注册与分发.md)
- [17-Service与ContentProvider跨进程组件](./17-Service与ContentProvider跨进程组件.md)
- [18-ANR原理与系统诊断](./18-ANR原理与系统诊断.md)
- [19-WMS窗口管理与焦点切换](./19-WMS窗口管理与焦点切换.md)
- [20-InputReader与InputDispatcher输入系统](./20-InputReader与InputDispatcher输入系统.md)
- [21-Choreographer与VSync帧调度](./21-Choreographer与VSync帧调度.md)
- [22-Android存储文件系统与数据持久化](./22-Android存储文件系统与数据持久化.md)
- [23-Android权限AppOps与SELinux](./23-Android权限AppOps与SELinux.md)
- [24-Android网络栈ConnectivityService与NetworkAgent](./24-Android网络栈ConnectivityService与NetworkAgent.md)
- [25-Android电源管理WakeLock与Doze](./25-Android电源管理WakeLock与Doze.md)
- [26-ARTClassLoaderDEX优化与垃圾回收](./26-ARTClassLoaderDEX优化与垃圾回收.md)
- [27-Android通知系统与SystemUI](./27-Android通知系统与SystemUI.md)
- [28-Android音频系统AudioFlinger与AudioPolicy](./28-Android音频系统AudioFlinger与AudioPolicy.md)
- [29-Android相机系统Camera2与CameraService](./29-Android相机系统Camera2与CameraService.md)
- [30-AndroidMediaCodecCodec2与视频播放](./30-AndroidMediaCodecCodec2与视频播放.md)
- [31-Android蓝牙系统BluetoothService与Profile](./31-Android蓝牙系统BluetoothService与Profile.md)
- [32-Android传感器系统SensorService与SensorHAL](./32-Android传感器系统SensorService与SensorHAL.md)
- [33-Android定位系统LocationManagerService与GNSS](./33-Android定位系统LocationManagerService与GNSS.md)
- [34-Android电话系统TelephonyFramework与RIL](./34-Android电话系统TelephonyFramework与RIL.md)
- [35-AndroidTelecomCallsManager与InCallService](./35-AndroidTelecomCallsManager与InCallService.md)
- [36-Android短信系统SmsManager与InboundSmsHandler](./36-Android短信系统SmsManager与InboundSmsHandler.md)
- [37-AndroidWiFi系统WifiService与ClientModeImpl](./37-AndroidWiFi系统WifiService与ClientModeImpl.md)
- [38-AndroidVPN系统VpnServiceTUN与网络路由](./38-AndroidVPN系统VpnServiceTUN与网络路由.md)
- [39-Android网络共享TetheringIpServer与NAT](./39-Android网络共享TetheringIpServer与NAT.md)
- [40-Android以太网系统EthernetService与IpClient](./40-Android以太网系统EthernetService与IpClient.md)
- [41-AndroidDNSDnsResolverNetd与PrivateDNS](./41-AndroidDNSDnsResolverNetd与PrivateDNS.md)
- [42-Android网络策略NetworkPolicyManagerDataSaver与UID防火墙](./42-Android网络策略NetworkPolicyManagerDataSaver与UID防火墙.md)
- [43-AndroidJobSchedulerJobStore与约束控制器](./43-AndroidJobSchedulerJobStore与约束控制器.md)
- [44-AndroidAlarmManagerAlarmManagerService与精确闹钟](./44-AndroidAlarmManagerAlarmManagerService与精确闹钟.md)
- [45-Android账户同步SyncManagerSyncStorageEngine与SyncAdapter](./45-Android账户同步SyncManagerSyncStorageEngine与SyncAdapter.md)
- [46-AndroidAccountManagerServiceAuthenticator与认证令牌](./46-AndroidAccountManagerServiceAuthenticator与认证令牌.md)
- [47-AndroidKeystoreKeyStoreService与Keymaster](./47-AndroidKeystoreKeyStoreService与Keymaster.md)
- [48-LockSettingsServiceGateKeeperSyntheticPassword与用户解锁](./48-LockSettingsServiceGateKeeperSyntheticPassword与用户解锁.md)
- [49-UserManagerServiceUserController用户启动与多用户隔离](./49-UserManagerServiceUserController用户启动与多用户隔离.md)
- [50-DevicePolicyManagerServiceDeviceOwnerProfileOwner与企业管控](./50-DevicePolicyManagerServiceDeviceOwnerProfileOwner与企业管控.md)
- [51-AccessibilityManagerServiceAccessibilityService与无障碍事件分发](./51-AccessibilityManagerServiceAccessibilityService与无障碍事件分发.md)
- [52-InputMethodManagerServiceInputMethodService与软键盘显示链路](./52-InputMethodManagerServiceInputMethodService与软键盘显示链路.md)
- [53-ClipboardServiceClipData跨应用复制粘贴与隐私控制](./53-ClipboardServiceClipData跨应用复制粘贴与隐私控制.md)
- [54-AutofillManagerServiceAutofillSession与自动填充数据链路](./54-AutofillManagerServiceAutofillSession与自动填充数据链路.md)

## 后续路线

| 编号 | 主题 | 关键收获 |
|---|---|---|
| 04 | [init 进程与 rc 脚本](./04-init进程与rc脚本.md) | Android 用户空间怎样启动 |
| 05 | [Zygote 启动与应用孵化](./05-Zygote启动与应用孵化.md) | Java 世界如何建立 |
| 06 | [SystemServer 与系统服务](./06-SystemServer与系统服务.md) | Framework 服务如何集中启动 |
| 07 | [Binder 基础与完整调用链](./07-Binder基础与完整调用链.md) | 跨进程调用为什么像普通方法调用 |
| 08 | [Activity 启动（一）：客户端请求](./08-Activity启动流程之客户端请求.md) | App 如何发起启动请求 |
| 09 | [Activity 启动（二）：ATMS 系统端调度](./09-Activity启动流程之ATMS系统端调度.md) | ATMS 如何选择和管理 Activity |
| 10 | [Activity 启动（三）：目标进程与生命周期](./10-Activity启动流程之目标进程与生命周期.md) | `onCreate()` 如何在目标进程执行 |
| 11 | [View 到 Window 与 ViewRootImpl](./11-View到Window与ViewRootImpl.md) | 界面如何成为一个窗口 |
| 12 | [Surface 到 SurfaceFlinger 显示链路](./12-Surface到SurfaceFlinger显示链路.md) | 像素最终怎样显示到屏幕 |
| 13 | [PackageManagerService：包管理与 APK 安装](./13-PackageManagerService包管理与APK安装.md) | APK 如何被扫描、安装和查询 |
| 14 | [综合实践：修改、编译与验证](./14-综合实践修改编译与验证.md) | 修改 Activity 启动链路并完成实验闭环 |

## 第二阶段：深入 Framework 运行机制

| 编号 | 主题 | 关键收获 |
|---|---|---|
| 15 | [AMS 进程管理与 LMKD](./15-AMS进程管理与LMKD.md) | 进程怎样启动、分级并在内存压力下回收 |
| 16 | [Broadcast 广播注册与分发](./16-Broadcast广播注册与分发.md) | 注册、解析、队列、超时和后台限制 |
| 17 | [Service 与 ContentProvider](./17-Service与ContentProvider跨进程组件.md) | 跨进程组件、绑定关系和发布流程 |
| 18 | [ANR 与系统诊断](./18-ANR原理与系统诊断.md) | 超时怎样被发现、现场怎样收集和分析 |
| 19 | [WMS 窗口管理与焦点切换](./19-WMS窗口管理与焦点切换.md) | 窗口怎样加入层级、获得 Surface 并切换输入焦点 |
| 20 | [InputReader 与 InputDispatcher 输入系统](./20-InputReader与InputDispatcher输入系统.md) | 原始输入怎样转换、选择窗口、进入 View 并完成回执 |
| 21 | [Choreographer、VSync 与帧调度](./21-Choreographer与VSync帧调度.md) | 一帧怎样被请求、编排、渲染、合成并最终显示 |
| 22 | [Android 存储、文件系统与数据持久化](./22-Android存储文件系统与数据持久化.md) | 分区、挂载、加密、应用目录和 SQLite 怎样协同 |
| 23 | [Android 权限、AppOps 与 SELinux](./23-Android权限AppOps与SELinux.md) | 权限授予、调用身份、操作裁决与内核强制访问控制怎样协同 |
| 24 | [Android 网络栈、ConnectivityService 与 NetworkAgent](./24-Android网络栈ConnectivityService与NetworkAgent.md) | 网络怎样注册、验证、匹配请求并把 socket 导入正确路由 |
| 25 | [Android 电源管理、WakeLock 与 Doze](./25-Android电源管理WakeLock与Doze.md) | 屏幕、CPU 挂起、后台调度和设备空闲策略怎样协同 |
| 26 | [ART、ClassLoader、DEX 优化与垃圾回收](./26-ARTClassLoaderDEX优化与垃圾回收.md) | 类加载、混合执行、dexopt 与堆回收怎样协作 |
| 27 | [Android 通知系统与 SystemUI](./27-Android通知系统与SystemUI.md) | 通知怎样入队、排序、分发、展示并响应点击 |
| 28 | [Android 音频系统、AudioFlinger 与 AudioPolicy](./28-Android音频系统AudioFlinger与AudioPolicy.md) | PCM 怎样经过共享缓冲、混音、路由与 HAL 输出 |
| 29 | [Android 相机系统、Camera2 与 CameraService](./29-Android相机系统Camera2与CameraService.md) | request/result 与图像 buffer 怎样经过 CameraService 和 HAL3 |
| 30 | [Android MediaCodec、Codec2 与视频播放](./30-AndroidMediaCodecCodec2与视频播放.md) | 压缩 sample 怎样解码成 Surface frame 并按时间显示 |
| 31 | [Android 蓝牙系统、BluetoothService 与 Profile](./31-Android蓝牙系统BluetoothService与Profile.md) | 开关、扫描、配对、Profile 与无线数据怎样穿过 Framework 和协议栈 |
| 32 | [Android 传感器系统、SensorService 与 Sensor HAL](./32-Android传感器系统SensorService与SensorHAL.md) | 采样事件怎样从硬件经过批处理、融合与分发到达 App |
| 33 | [Android 定位系统、LocationManagerService 与 GNSS](./33-Android定位系统LocationManagerService与GNSS.md) | 位置请求怎样经过权限、Provider、GNSS HAL 后按策略返回 App |
| 34 | [Android 电话系统、Telephony Framework 与 RIL](./34-Android电话系统TelephonyFramework与RIL.md) | SIM、驻网、通话和移动数据怎样经过 Radio HAL 到达基带 |
| 35 | [Android Telecom、CallsManager 与 InCallService](./35-AndroidTelecomCallsManager与InCallService.md) | 不同通话实现怎样统一成 Call、通话界面和音频路由 |
| 36 | [Android 短信系统、SmsManager 与 InboundSmsHandler](./36-Android短信系统SmsManager与InboundSmsHandler.md) | 短信怎样编码、分段、发送、重组并安全交付默认短信应用 |
| 37 | [Android Wi-Fi 系统、WifiService 与 ClientModeImpl](./37-AndroidWiFi系统WifiService与ClientModeImpl.md) | 从扫描、认证和 IP 配置追到网络验证、默认网络与最终数据路由 |
| 38 | [Android VPN 系统、VpnService、TUN 与网络路由](./38-AndroidVPN系统VpnServiceTUN与网络路由.md) | 从用户授权和 TUN 建立追到 UID 路由、隧道封装、底层网络与防泄漏策略 |
| 39 | [Android 网络共享、Tethering、IpServer 与 NAT](./39-Android网络共享TetheringIpServer与NAT.md) | 从下游接口、DHCP 和上游选择追到转发、NAT、IPv6 与硬件卸载 |
| 40 | [Android 以太网系统、EthernetService 与 IpClient](./40-Android以太网系统EthernetService与IpClient.md) | 从接口发现、物理链路和 IP 配置追到 NetworkAgent、网络验证与数据路径 |
| 41 | [Android DNS、DnsResolver、netd 与 Private DNS](./41-AndroidDNSDnsResolverNetd与PrivateDNS.md) | 从按 netId 配置与缓存追到 A/AAAA 查询、DoT 验证和应用地址连接 |
| 42 | [Android 网络策略、NetworkPolicyManager、Data Saver 与 UID 防火墙](./42-Android网络策略NetworkPolicyManagerDataSaver与UID防火墙.md) | 从计量属性和 UID 状态追到运行规则、配额、防火墙链与内核执行 |
| 43 | [Android JobScheduler、JobStore 与约束控制器](./43-AndroidJobSchedulerJobStore与约束控制器.md) | 从 JobInfo 入队和约束跟踪追到 pending、并发槽位、JobService 执行与重试 |
| 44 | [Android AlarmManager、AlarmManagerService 与精确闹钟](./44-AndroidAlarmManagerAlarmManagerService与精确闹钟.md) | 从时间基准、窗口和批处理追到 kernel 唤醒、Doze 限频与 PendingIntent 投递 |
| 45 | [Android 账户同步、SyncManager、SyncStorageEngine 与 SyncAdapter](./45-Android账户同步SyncManagerSyncStorageEngine与SyncAdapter.md) | 从账户/authority 请求、状态存储和 SyncOperation 追到 Job 调度、Adapter 执行与结果重试 |
| 46 | [Android AccountManagerService、Authenticator 与认证令牌](./46-AndroidAccountManagerServiceAuthenticator与认证令牌.md) | 从账户存储和可见性追到 Authenticator Session、用户交互、token 缓存与权限裁决 |
| 47 | [Android Keystore、KeyStoreService 与 Keymaster](./47-AndroidKeystoreKeyStoreService与Keymaster.md) | 从 JCA Provider、alias/UID 命名空间追到 Keymaster、硬件安全级别、operation 与用户认证约束 |
| 48 | [LockSettingsService、GateKeeper、Synthetic Password 与用户解锁](./48-LockSettingsServiceGateKeeperSyntheticPassword与用户解锁.md) | 从锁屏凭据和可信限速追到 SP 恢复、SID/HAT、FBE CE key 与用户解锁生命周期 |
| 49 | [UserManagerService、UserController、用户启动与多用户隔离](./49-UserManagerServiceUserController用户启动与多用户隔离.md) | 从 userId/appId/uid、用户档案和 DE/CE 数据追到启动、解锁、切换、停止与删除状态机 |
| 50 | [DevicePolicyManagerService、Device Owner、Profile Owner 与企业管控](./50-DevicePolicyManagerServiceDeviceOwnerProfileOwner与企业管控.md) | 从 provisioning、admin/owner 角色和策略持久化追到限制聚合及包、权限、锁屏、网络执行链 |
| 51 | [AccessibilityManagerService、AccessibilityService 与无障碍事件分发](./51-AccessibilityManagerServiceAccessibilityService与无障碍事件分发.md) | 从服务发现绑定和事件过滤追到窗口/节点查询、语义动作、触摸探索与手势注入 |
| 52 | [InputMethodManagerService、InputMethodService 与软键盘显示链路](./52-InputMethodManagerServiceInputMethodService与软键盘显示链路.md) | 从焦点、EditorInfo/InputConnection 和 IME 绑定追到 session、键盘窗口、Insets 与文本提交 |
| 53 | [ClipboardService、ClipData、跨应用复制粘贴与隐私控制](./53-ClipboardServiceClipData跨应用复制粘贴与隐私控制.md) | 从按用户 primary clip 和前台读取检查追到 URI 临时授权、监听通知及跨 profile 复制策略 |
| 54 | [AutofillManagerService、AutofillSession 与自动填充数据链路](./54-AutofillManagerServiceAutofillSession与自动填充数据链路.md) | 从 AutofillId/AssistStructure 和 Session 追到 FillResponse、Dataset 认证、View 填充与保存请求 |
| 55 | [TextClassifierService、TextLinks、智能选择与文本操作](./55-TextClassifierServiceTextLinks智能选择与文本操作.md) | 从 TextView 智能选区和文本分类追到 per-user 服务路由、TextLinks 应用、RemoteAction 与隐私边界 |
| 56 | [ContentCaptureManagerService、ContentCaptureService 与页面内容采集](./56-ContentCaptureManagerServiceContentCaptureService与页面内容采集.md) | 从 Activity 会话和 View 语义事件追到白名单裁决、直连 Binder、批量刷新、UID 校验与隐私边界 |
| 57 | [AppPredictionManagerService、AppPredictionService 与应用预测](./57-AppPredictionManagerServiceAppPredictionService与应用预测.md) | 从预测场景、目标和反馈事件追到权限检查、per-user 服务、连续回调、候选排序与死亡恢复 |
| 58 | [ContentSuggestionsManagerService、ContentSuggestionsService 与内容建议](./58-ContentSuggestionsManagerServiceContentSuggestionsService与内容建议.md) | 从任务快照和硬件 Bitmap 追到区域选择、内容分类、异步回调、私有 Bundle 协议与安全边界 |
| 59 | [PeopleService、ConversationInfo 与联系人/会话数据聚合](./59-PeopleServiceConversationInfo与联系人会话数据聚合.md) | 从 Conversation Shortcut 追到联系人、通知、通话、短信、UsageStats、事件历史持久化与 Direct Share 预测 |
| 60 | [VoiceInteractionManagerService、VoiceInteractionSession 与语音助手](./60-VoiceInteractionManagerServiceVoiceInteractionSession与语音助手.md) | 从助手选择、常驻服务和 Session 浮层追到 Assist 上下文、VoiceInteractor、Direct Actions、URI 授权与热词 DSP |
| 61 | [SpeechRecognizer、RecognitionService 与语音识别回调链](./61-SpeechRecognizerRecognitionService与语音识别回调链.md) | 从默认识别服务选择和惰性绑定追到录音权限、单请求状态机、partial/final/error 回调、线程切换与资源释放 |
| 62 | [TextToSpeech、TextToSpeechService 与语音合成播放链路](./62-TextToSpeechTextToSpeechService与语音合成播放链路.md) | 从 TTS 引擎发现初始化、语言与 Voice 选择追到双队列、流式 PCM、AudioTrack/WAV 输出、进度回调与停止释放 |
| 63 | [MediaSession、MediaController 与系统媒体控制链路](./63-MediaSessionMediaController与系统媒体控制链路.md) | 从播放器状态发布和 Session Token 追到 Controller 双向 Binder、会话排序、媒体键、通知、蓝牙、本地/远端音量与 AudioFocus 边界 |
| 64 | [MediaBrowserService、MediaBrowser 与媒体目录浏览链路](./64-MediaBrowserServiceMediaBrowser与媒体目录浏览链路.md) | 从服务绑定、调用方授权和 BrowserRoot 追到目录订阅、异步 Result、分页、更新重载、单项查询、重连与 Session Token 交接 |
| 65 | [MediaRouter、MediaRouteProvider 与投屏路由选择链路](./65-MediaRouterMediaRouteProvider与投屏路由选择链路.md) | 从发现偏好、Provider 按需绑定和 route 快照追到跨 Provider session 创建、controller 交接、远端音量、释放及真实媒体数据面边界 |
| 66 | [MediaProjection、VirtualDisplay 与屏幕捕获投射链路](./66-MediaProjectionVirtualDisplay与屏幕捕获投射链路.md) | 从 SystemUI 授权和 Projection Token 追到 DMS、VirtualDisplayDevice、SurfaceFlinger、ImageReader/MediaCodec 输出、安全内容、音频分链与停止清理 |
| 67 | [DisplayManagerService、DisplayDevice 与多显示器管理链路](./67-DisplayManagerServiceDisplayDevice与多显示器管理链路.md) | 从各类 DisplayAdapter 和设备事件追到 DisplayDevice、LogicalDisplay、DisplayInfo、WMS DisplayContent、镜像映射、输入 viewport、刷新率和电源状态 |
| 68 | [DisplayPowerController、自动亮度与屏幕电源状态链路](./68-DisplayPowerController自动亮度与屏幕电源状态链路.md) | 从 PMS DisplayPowerRequest 追到 DPC 状态/亮度决策、距离传感器、环境光映射、渐变、WMS blocker、Doze 和真实面板下发 |
| 69 | [DreamManagerService、Doze 与 Always-On Display 链路](./69-DreamManagerServiceDoze与AlwaysOnDisplay链路.md) | 从 PMS Dream/Doze 决策追到 Dream 选择绑定、SystemUI DozeMachine、AOD/pulse、屏幕状态亮度 override、WakeLock 和退出清理 |
| 70 | [Android 热管理、ThermalManagerService 与性能降频链路](./70-Android热管理ThermalManagerService与性能降频链路.md) | 从传感器、thermal governor 和 HAL 追到 Temperature 缓存、overall status、监听器、headroom、热关机及 Framework/底层节流边界 |
| 71 | [BatteryService、Health HAL 与电池状态分发链路](./71-BatteryServiceHealthHAL与电池状态分发链路.md) | 从 IHealth callback 追到 BatteryService 当前状态、插电/低电广播、安全关机、BatteryManager 属性查询及 BatteryStats 历史记账边界 |
| 72 | [BatteryStatsService、BatteryStatsImpl 与 UID 耗电记账链路](./72-BatteryStatsServiceBatteryStatsImpl与UID耗电记账链路.md) | 从事件 note 和插拔电前 external snapshot 追到 TimeBase、UID Timer/Counter、History、WorkSource 归因、PowerProfile/Calculator、BatterySipper 与 Historian 数据来源 |
| 73 | [statsd、StatsCompanionService 与 Atom 系统遥测链路](./73-statsdStatsCompanionService与Atom系统遥测链路.md) | 从 atoms.proto、生成 API 和 statsdw socket 追到 push/pull、StatsdConfig、MetricProducer、维度分桶、UidMap、Java companion、持久化与报告导出 |
| 74 | [UsageStatsService、AppStandbyController 与应用使用/待机分桶链路](./74-UsageStatsServiceAppStandbyController与应用使用待机分桶链路.md) | 从 usage event、按用户 CE/DE 数据和 IntervalStats 追到查询权限、时间转换、AppIdleHistory、bucket/reason、预测/豁免及 Job/Alarm 后台策略 |
| 75 | [AppTimeLimitController、UsageObserver 与 Digital Wellbeing 使用时长限制链路](./75-AppTimeLimitControllerUsageObserver与DigitalWellbeing使用时长限制链路.md) | 从三类 observer 注册、Activity/package/token start-stop 和两级引用计数追到 group 并集计时、timeout、session end、PendingIntent，以及检测、决策、限制执行的边界 |
| 76 | [DeviceConfig、SettingsProvider 与系统动态配置下发链路](./76-DeviceConfigSettingsProvider与系统动态配置下发链路.md) | 从 System/Secure/Global/Config、NameValueCache 和 Provider.call 追到 SettingsState XML、generation、namespace listener、Executor、system_server 动态策略及 native property bridge |
| 77 | [SystemProperties、property_service 与 SELinux 属性链路](./77-SystemPropertiesPropertyService与SELinux属性链路.md) | 从 Java/JNI 和 bionic 只读共享属性区追到 init property service、property_contexts、ro/persist/ctl 语义、持久文件、change callback 与 rc property trigger |
| 78 | [Watchdog、SystemServer 卡死检测与 RescueParty 故障自愈链路](./78-WatchdogSystemServer卡死检测与RescueParty故障自愈链路.md) | 从 HandlerChecker、Monitor、Binder 线程和半程/超时取证追到 system_server 运行时重启，再串联 PackageWatchdog 失败窗口、启动循环、分级配置重置与最终恢复出厂 |
| 79 | [RollbackManager、PackageWatchdog 与模块更新回滚链路](./79-RollbackManagerPackageWatchdog与模块更新回滚链路.md) | 从安装前保存旧代码和数据快照追到 Rollback 状态机、健康观察、降级 multi-package session、APK/APEX 差异、staged reboot 与数据恢复 |
| 80 | [apexd、APEX 激活、staged install 与 boot rollback/checkpoint 链路](./80-apexdAPEX激活stagedinstall与bootrollbackcheckpoint链路.md) | 从 APEX 格式、双层签名、loop/dm-verity 挂载追到 staged pre-reboot verification、ready/activated/applied/success、checkpoint 与启动失败回退 |
| 81 | [vold、fs_mgr、文件系统 checkpoint 与 userdata 回滚实现链路](./81-voldfs_mgr文件系统checkpoint与userdata回滚实现链路.md) | 从 StorageManager/IVold 接口和 metadata 重试计数追到 fstab checkpoint flag、F2FS 原生 checkpoint、dm-bow、提交/重启回退、BootControl 与密钥生命周期 |
| 82 | [BootControl HAL、A/B Slot、update_engine 与 OTA 无缝更新链路](./82-BootControlHALABSlotupdate_engine与OTA无缝更新链路.md) | 从 ApplyPayload、InstallPlan、DeltaPerformer 和目标分区验收追到 postinstall、active/bootable/successful、bootloader tries 退槽、Dynamic/Virtual A/B snapshot merge |
| 83 | [AVB 2.0、vbmeta、dm-verity 与 rollback index 启动验证链路](./83-AVB2vbmetadm-verity与rollbackindex启动验证链路.md) | 从设备信任根、VBMeta 签名和 descriptor 追到 libavb slot verify、chain delegation、Merkle/dm-verity、启动颜色状态、防降级索引提交与 A/B 回退边界 |
| 84 | [init first-stage mount、fstab、Dynamic Partitions 与 Device Mapper 启动挂载链路](./84-initfirst-stagemountfstabDynamicPartitions与DeviceMapper启动挂载链路.md) | 从 ramdisk PID 1、first-stage fstab 和 uevent 设备发现追到 super/LP metadata、dm-linear、Virtual A/B snapshot、AVB verity、mount、SwitchRoot 与 system init exec |
| 85 | [SELinux 初始化、sepolicy 加载、domain transition 与 Android 启动安全边界](./85-SELinux初始化sepolicy加载domaintransition与Android启动安全边界.md) | 从 split/precompiled policy identity、secilc 与 kernel load 追到 enforcing、init re-exec、file/seapp/service/property contexts、daemon/App domain transition、neverallow 与 AVC 最小权限排障 |
| 86 | [Android SELinux 实战：新增 native daemon、设备节点与 Binder/HAL 权限设计](./86-AndroidSELinux实战新增nativedaemon设备节点与BinderHAL权限设计.md) | 用 vendor mower_diag 示例从 Android.bp/init.rc、专属 domain/exec、dev/genfs/data/property type 追到 Binder/vndbinder/HwBinder 架构选择、HAL attributes、AVC 迭代和失陷攻击面复查 |
| 87 | [Android 内核驱动到 Framework：字符设备、sysfs、uevent、JNI 与系统服务接入链路](./87-Android内核驱动到Framework字符设备sysfsueventJNI与系统服务接入链路.md) | 从 char device file_operations、ioctl/sysfs/poll/uevent 追到 JNI 注册、UEventObserver、SystemService、Binder/HAL、Vibrator 真实链路、异步状态机、线程锁与跨层安全 |

## 阅读源码的固定四问

1. 这段代码运行在哪个进程？
2. 这段代码运行在哪个线程？
3. 输入从哪里来，输出到哪里去？
4. 是否发生了 Java/Native 或进程边界切换？
