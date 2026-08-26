# 95 Android SystemServiceManager：系统服务启动、BootPhase、依赖顺序与用户生命周期

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 阅读环境：macOS 只读本地源码，不要求编译或连接设备。  
> 本章目标：理解 system_server 如何创建、启动和编排 `SystemService`，能区分构造、`onStart()`、Binder 发布、BootPhase、`systemReady()`、用户解锁与 boot completed 等不同完成点。

---

## 1. SystemServiceManager 是什么

`SystemServiceManager`（下文简称 SSM）是 system_server 进程内的系统服务生命周期调度器。它主要负责：

- 创建继承 `SystemService` 的服务；
- 调用 `onStart()`；
- 保存已启动服务列表；
- 顺序分发 `onBootPhase()`；
- 分发用户 start/unlock/switch/stop 生命周期；
- 记录慢回调和启动 trace；
- 加载位于额外 jar 中的 SystemService。

它不负责：

- 保存跨进程 Binder 服务目录——那是 servicemanager；
- 为应用构造 Manager wrapper——那是 SystemServiceRegistry；
- 自动推断服务依赖图；
- 自动把耗时初始化放到后台线程；
- 自动保证每个服务已经“业务可用”。

三章关系：

```text
SystemServiceManager
  管 system_server 内服务对象的启动与生命周期
           │ onStart 中 publishBinderService
           ↓
ServiceManager
  管名字 → Binder 的跨进程目录
           │ 客户端按名字取得 Binder
           ↓
SystemServiceRegistry
  为 Context 构造并缓存 Java Manager wrapper
```

---

## 2. 为什么需要统一生命周期框架

system_server 中有大量服务。若每个服务都由 SystemServer 手写完整启动、ready、用户切换逻辑，会产生：

- 生命周期接口不统一；
- 启动耗时难以统一记录；
- 异常信息缺少服务名和阶段；
- 用户事件容易漏发；
- 服务启动顺序难以审查。

`SystemService` 抽象类提供统一协议：

```java
public abstract void onStart();
public void onBootPhase(int phase) {}
public void onUserStarting(TargetUser user) {}
public void onUserUnlocking(TargetUser user) {}
public void onUserUnlocked(TargetUser user) {}
public void onUserSwitching(TargetUser from, TargetUser to) {}
public void onUserStopping(TargetUser user) {}
public void onUserStopped(TargetUser user) {}
```

服务只覆写自己关心的回调。

---

## 3. 本章源码地图

```text
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/services/core/java/com/android/server/SystemServiceManager.java
frameworks/base/services/core/java/com/android/server/SystemService.java
frameworks/base/services/core/java/com/android/server/SystemServerInitThreadPool.java
frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
frameworks/base/services/core/java/com/android/server/am/UserController.java
frameworks/base/core/java/com/android/server/LocalServices.java
```

实例服务可选择：

```text
frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java
frameworks/base/services/core/java/com/android/server/BatteryService.java
```

---

## 4. SSM 在哪里创建

SystemServer 初始化 system Context 后：

```java
mSystemServiceManager = new SystemServiceManager(mSystemContext);
mSystemServiceManager.setStartInfo(
        mRuntimeRestart,
        mRuntimeStartElapsedTime,
        mRuntimeStartUptime);
LocalServices.addService(SystemServiceManager.class, mSystemServiceManager);
SystemServerInitThreadPool.start();
```

含义：

1. SSM 持有 system Context。
2. 记录此次 system_server 是正常开机还是 runtime restart，以及时间基准。
3. 把 SSM 自己放进 LocalServices，供 system_server 内其他代码取得。
4. 启动一个只用于启动阶段并行任务的线程池。

SSM 不是 Binder 服务，普通应用不能按名字查询它。

---

## 5. SystemServer 的三大启动区

```java
startBootstrapServices(t);
startCoreServices(t);
startOtherServices(t);
```

### Bootstrap services

用于“把系统托起来”的关键服务，依赖纠缠最复杂，例如 Installer、ATMS/AMS、PowerManager、DisplayManager、PMS、UserManager。

### Core services

基本而重要、但不属于早期依赖死结的服务，例如 BatteryService、UsageStatsService 等。

### Other services

数量最多的其余 Framework 服务，并在后半段推进 ready 和允许应用启动。

这三组不是安全等级，也不是三类进程；主要是 SystemServer 源码组织与依赖顺序。

---

## 6. 启动顺序不是自动拓扑排序

SSM 没有读取类似：

```text
PowerManager dependsOn LightsService
```

再自动计算拓扑序。Android 11 的主要依赖表达方式是：

- SystemServer 中显式代码顺序；
- 注释说明“必须在 X 前/后”；
- BootPhase 契约；
- `Future`/latch 等显式等待；
- 调用前检查某服务是否完成初始化。

例如 DisplayManager 必须先提供默认显示，随后才进入：

```java
startBootPhase(PHASE_WAIT_FOR_DEFAULT_DISPLAY);
```

再继续启动 PackageManager。

所以移动 SystemServer 中一段启动代码可能改变系统正确性，即使它仍能编译。

---

## 7. `startService(Class)` 的完整流程

```java
PowerManagerService service =
        mSystemServiceManager.startService(PowerManagerService.class);
```

内部步骤：

```text
检查是 SystemService 子类
  → 反射查找 public Constructor(Context)
  → constructor.newInstance(mContext)
  → mServices.add(service)
  → service.onStart()
  → 慢调用检查
  → 返回服务对象
```

源码要求构造器必须形如：

```java
public DemoService(Context context) {
    super(context);
}
```

不是任意构造函数，也不能只有 private/package-private Context 构造器。

---

## 8. 为什么构造和 onStart 分开

建议的语义是：

### 构造器

- 保存依赖；
- 创建基本字段和锁；
- 建立尚未对外暴露的内部对象；
- 尽量不依赖其他尚未启动服务。

### `onStart()`

- 发布 BinderService；
- 发布 LocalService；
- 注册必要观察者；
- 启动该服务的基础能力。

分开后 SSM 能统一：

- 将实例加入生命周期列表；
- 包装 `onStart()` 异常；
- 记录耗时；
- 给所有已启动服务分发后续 phase/user 事件。

但框架没有强制构造器“绝不做 I/O”，这仍靠服务作者遵守启动性能纪律。

---

## 9. 先加入 mServices，再调用 onStart

源码顺序：

```java
mServices.add(service);
try {
    service.onStart();
} catch (RuntimeException ex) {
    throw new RuntimeException(..., ex);
}
```

这意味着 `onStart()` 抛异常前，实例已进入列表。但通常该异常会一路导致 SystemServer 启动失败并终止当前启动，不会继续当作正常已启动服务运行。

不要由此推导出“失败服务之后仍会可靠接收 phase”。启动异常不是正常恢复协议；关键服务失败常被视为 system_server 致命错误。

---

## 10. 三种 startService 入口

### 按 Class

```java
startService(DemoService.class)
```

类型安全且最常见。

### 按类名字符串

```java
startService("com.android.server.demo.DemoService")
```

由指定 ClassLoader 加载。

### 从 jar 加载

```java
startServiceFromJar(className, jarPath)
```

SSM 为路径缓存 `PathClassLoader`，父加载器固定为 system_server class loader。这允许设备特定或模块化服务位于额外 jar。

类找不到的异常提示中特别建议检查设备 feature，说明可选硬件服务应先用 `PackageManager.hasSystemFeature()` 判断，而不是无条件加载不存在实现。

---

## 11. 也可以传入已经创建的实例

SystemServer 有时写：

```java
mSystemServiceManager.startService(
        new OverlayManagerService(mSystemContext));
```

此路径不经过反射，只做：

```text
mServices.add(instance) → instance.onStart()
```

适合构造方式特殊或实例已由外部工厂建立的服务。它仍进入统一 phase/user 生命周期列表。

---

## 12. onStart 完成不等于系统服务完全 ready

至少要区分：

```text
constructed       Java 对象构造完成
started           onStart 返回
published         Binder 名字已 addService
phase-ready       收到某个 onBootPhase 并完成对应工作
systemReady       某些旧服务的专用 systemReady() 已调用
user-ready        某个用户已 start/unlock
boot-completed    PHASE_BOOT_COMPLETED 回调完成
business-ready    该服务所需依赖/数据确实可处理某项业务
```

一个服务可能在 `onStart()` 早期发布 Binder，但方法内部根据 ready 标志拒绝部分调用；也可能等某 phase 才注册广播或接触第三方应用。

所以看到 `service list` 中已有名字，只能证明“已发布且目录可见”，不能证明全部业务准备完成。

---

## 13. 为什么有的服务不走 SystemServiceManager

SystemServer 中仍能看到：

```java
ServiceManager.addService(...);
SomeLegacyService.main(...);
service.systemReady();
```

原因包括：

- 历史遗留结构；
- 服务不是 `SystemService` 子类；
- 特殊静态工厂/复杂互相依赖；
- native 服务或模块服务使用自己的生命周期。

因此 SSM 是主框架，但不是 system_server 里所有对象的唯一启动入口。读某服务时应先确认它是否真的在 `mServices` 中。

---

## 14. BootPhase 是什么

BootPhase 是 system_server 向已启动 SystemService 广播的**单调递增里程碑**。

Android 11 常量：

| 数值 | 阶段 | 核心语义 |
|---:|---|---|
| 100 | `WAIT_FOR_DEFAULT_DISPLAY` | 默认显示应可用 |
| 480 | `LOCK_SETTINGS_READY` | 可取得锁设置数据 |
| 500 | `SYSTEM_SERVICES_READY` | 可安全调用 Power/PMS 等核心服务 |
| 520 | `DEVICE_SPECIFIC_SERVICES_READY` | 设备特定服务 ready |
| 550 | `ACTIVITY_MANAGER_READY` | 可发送广播 |
| 600 | `THIRD_PARTY_APPS_CAN_START` | 可启动/绑定三方应用，应用可调用服务 |
| 1000 | `BOOT_COMPLETED` | `SystemService` 的最终 boot phase；基类契约称 boot completed、Home 已 started，但不能据此证明 Launcher 首帧已绘制或用户已经可交互 |

数值之间故意留空，便于加入新阶段；代码不应假设相邻阶段差 1。

---

## 15. BootPhase 不是“状态枚举切换”

SSM 只保存最新的：

```java
private int mCurrentPhase = -1;
```

推进时要求：

```java
if (phase <= mCurrentPhase) {
    throw new IllegalArgumentException(
            "Next phase must be larger than previous");
}
```

所以它是里程碑序列：到达 600 意味着此前 100、480、500、520、550 已按主启动路径经过，而不是“系统当前只处于 600、已离开 500”。

服务常写：

```java
if (phase == PHASE_SYSTEM_SERVICES_READY) { ... }
```

也有服务根据 `phase >= 某值` 推导能力。

---

## 16. phase 如何分发

```java
final int serviceLen = mServices.size();
for (int i = 0; i < serviceLen; i++) {
    SystemService service = mServices.get(i);
    service.onBootPhase(mCurrentPhase);
}
```

关键事实：

- 按加入 `mServices` 的顺序串行调用；
- 默认运行在调用 `startBootPhase()` 的 SystemServer 线程，通常是主启动线程；
- 每个服务回调有 trace；
- 超过 50ms 会警告；
- 一个服务抛异常会包装为 RuntimeException，通常中断本阶段和启动过程。

它不是并行广播，也没有“失败后自动跳过继续”的通用容错。

---

## 17. 为什么只通知“此刻已经启动”的服务

源码进入 phase 时先固定：

```java
final int serviceLen = mServices.size();
```

只遍历当时列表长度。如果在某服务 `onBootPhase()` 中又加入新服务，新服务不会在同一轮尾部被意外调用。

更重要的是，SSM 的 `startService()` 并不会自动补发所有已经过去的 phase。因此服务应在其依赖的 phase 之前启动；晚启动服务不能假设框架会从 100 开始补课。

这也是启动位置属于接口契约的一部分。

---

## 18. phase 回调为什么必须快

`SERVICE_CALL_WARN_TIME_MS = 50`。超过 50ms 只会打印 warning，不会自动杀死服务或取消工作。

但多个服务串行执行，若 100 个服务各阻塞 100ms，就额外增加 10 秒启动时间。因此回调应：

- 只做里程碑所需的最小同步操作；
- 可并行且无即时依赖的重活提交给受控线程池；
- 明确在进入下一关键阶段前是否必须 join/wait；
- 不在主线程等待可能反向调用 system_server 的 Binder 链。

“异步”不能只启动线程而不建立完成屏障，否则后续阶段可能在依赖未完成时继续。

---

## 19. `SystemServerInitThreadPool` 的使用边界

SystemServer 启动阶段会提交可并行任务，例如读取配置、启动某些 native 服务或 WebView 准备。

模式：

```text
submit task → 主线程继续启动无依赖服务
            → 在真正依赖点 wait Future/latch
            → PHASE_BOOT_COMPLETED 后关闭 init pool
```

`startBootPhase(PHASE_BOOT_COMPLETED)` 结束时：

```java
SystemServerInitThreadPool.shutdown();
```

它是启动专用线程池，不应被当作系统服务运行期的通用 executor。

---

## 20. 第一阶段为何是默认显示

Bootstrap 启动 DisplayManager 后：

```java
mDisplayManagerService =
        mSystemServiceManager.startService(DisplayManagerService.class);
mSystemServiceManager.startBootPhase(
        t, PHASE_WAIT_FOR_DEFAULT_DISPLAY);
```

随后才启动 PackageManager 等。早期 Framework 创建 Resources、configuration、系统 UI 环境时需要可靠的默认显示信息。

这里 phase 名称表达“调用此阶段时主启动代码认为应满足的系统条件”。SSM 本身没有检查物理显示是否真的 ready；具体服务回调和上游启动代码共同履行契约。

---

## 21. SYSTEM_SERVICES_READY 做什么

在大量服务已创建，LockSettings ready 后，SystemServer 推进：

```java
PHASE_LOCK_SETTINGS_READY
PHASE_SYSTEM_SERVICES_READY
```

后者意味着服务可以更安全地调用 PowerManager、PackageManager 等核心系统服务。

“可以安全调用”不是说此前 Binder 名字绝对不存在，而是整体依赖和内部初始化已到约定里程碑。早于该阶段调用某接口可能遇到：

- 对方尚未发布；
- 已发布但内部 systemReady 尚未完成；
- 锁和回调依赖形成启动死锁；
- 数据库/用户数据尚不可用。

---

## 22. ACTIVITY_MANAGER_READY 与应用可启动不同

`PHASE_ACTIVITY_MANAGER_READY = 550` 表示可以发送广播，但不要直接等同于三方应用已经可以自由启动。

下一阶段：

```text
PHASE_THIRD_PARTY_APPS_CAN_START = 600
```

才表示服务可以 start/bind 三方应用，并允许应用进入并调用系统服务。

两阶段分开，是为了让系统组件先完成广播、网络、WebView、包数据等准备，再打开三方代码这个更复杂的世界。

---

## 23. PHASE_BOOT_COMPLETED 从哪里触发

它并非紧接 `startOtherServices()` 同步调用。Android 11 中最终由 ActivityManagerService 的 `finishBooting()` 推进：

```java
mSystemServiceManager.startBootPhase(
        t, SystemService.PHASE_BOOT_COMPLETED);
```

触发与启动 Home、完成系统启动、加密/用户流程等条件相关。

`SystemService` 注释把 phase 1000 描述为“boot completed 且 Home application 已启动”，这是
Framework 为系统服务定义的里程碑语义。它不等价于 Launcher 已绘制首帧、设备已经可流畅交互，
也不证明某个用户的 `BOOT_COMPLETED` 广播及所有 Receiver 都已执行完；这些是不同时间线与
不同观测点。

因此：

```text
SystemServer.startOtherServices 返回
    ≠ PHASE_BOOT_COMPLETED 已完成
```

`isBootCompleted()` 只是判断 SSM 当前 phase 是否达到 1000，不是读取 `sys.boot_completed` 属性，也不直接等价于某个用户已完成所有应用级初始化。

---

## 24. BootPhase 与广播的区别

| BootPhase | BOOT_COMPLETED/LOCKED_BOOT_COMPLETED 广播 |
|---|---|
| system_server 内 SystemService 生命周期回调 | Android 组件广播机制 |
| 只给 SSM 管理的服务对象 | 发给符合条件的 Receiver |
| 普通 Java 串行调用 | 经 AMS/BroadcastQueue 调度 |
| 不经过 Intent 解析 | 有用户、权限、进程启动等语义 |
| 更早、更直接、开销较低 | 面向应用/组件生态 |

SystemService 文档建议内部服务优先监听 phase，减少为了同一系统里程碑再注册广播带来的延迟。

---

## 25. `systemReady()` 为什么仍然存在

旧服务或非 SystemService 架构仍有专用：

```java
wm.systemReady();
vibrator.systemReady();
networkService.systemReady();
```

这些调用由 SystemServer 显式编排，有时还返回 latch。它们与统一 `onBootPhase()` 共存，是历史演进结果。

不能把任意类的 `systemReady()` 自动映射为 `PHASE_SYSTEM_SERVICES_READY`：

- 调用时间可能在 phase 前或后；
- 语义由具体服务定义；
- 异常处理方式可能不同；
- 可能有额外参数和完成信号。

读启动链必须跟随真实调用点。

---

## 26. safe mode 如何传给服务

SystemServer 检测安全模式后：

```java
mSystemServiceManager.setSafeMode(safeMode);
```

SystemService 可通过：

```java
isSafeMode()
```

它内部从 LocalServices 取得 SSM，再读取标志。

安全模式标志不是一个 BootPhase。它是一项可供服务查询的启动环境，服务决定是否禁用第三方扩展、调整策略或跳过某些初始化。

---

## 27. runtime restart 是什么

`mRuntimeRestarted` 表示 Android runtime/system_server 重新启动，而非设备从 bootloader 完整冷启动。

服务可用：

```java
isRuntimeRestarted()
getRuntimeStartElapsedTime()
getRuntimeStartUptime()
```

区分它有助于：

- 诊断启动耗时；
- 避免把 runtime restart 当设备首次开机；
- 恢复 native daemon 中仍存在的状态；
- 处理此前 system_server Binder 全部死亡后重新发布。

但 SSM 只提供事实，具体恢复逻辑由各服务实现。

---

## 28. 用户生命周期为何独立于设备 BootPhase

设备可能有多个用户，且用户可在设备已经启动很久后启动、解锁、切换和停止。

```text
设备生命周期：BootPhase 100 → ... → 1000（一次、单调）

用户 0：start → unlocking → unlocked ───────────────
用户 10：             start → unlocking → unlocked → stop → stopped
前台用户：0 ───────────── switch 0→10 ─────────────
```

Boot completed 不等于每个用户都 unlocked；用户 unlocked 也不等于设备刚进入 boot completed。

---

## 29. 用户回调对应关系

SSM 的入口与服务回调：

| SSM 入口 | SystemService 回调 | 含义 |
|---|---|---|
| `startUser` | `onUserStarting` | 建立该运行用户的 per-user 状态 |
| `unlockUser` | `onUserUnlocking` | CE 存储已可用，处于 unlocking |
| `onUserUnlocked` | `onUserUnlocked` | 用户已进入 unlocked |
| `switchUser(from,to)` | `onUserSwitching` | 前台用户切换 |
| `stopUser` | `onUserStopping` | 停止前清理，仍可访问 CE 的最后阶段 |
| `cleanupUser` | `onUserStopped` | 用户进程清理完成后的最终释放 |

命名中的 `cleanupUser` 对应 `onUserStopped`，不是同名直译，读代码时容易看漏。

---

## 30. TargetUser 为什么包装 UserInfo

新回调使用：

```java
SystemService.TargetUser
```

它包装 `UserInfo` 并提供 user handle/id。源码强调内部 UserInfo 是 UserManagerService 引用的“live object”，服务不能修改它。

TargetUser 帮助 API 更明确地表达：

- 当前操作的目标用户；
- switch 时 from/to 两个用户；
- 未来可扩展用户生命周期契约。

SystemService 仍保留旧 `onStartUser(int)` 等 deprecated 方法，新回调默认桥接旧方法，帮助历史服务迁移。

---

## 31. 用户事件如何取得 UserInfo

ActivityManager 进入 system ready 前调用：

```java
mSystemServiceManager.preSystemReady();
```

SSM 从 LocalServices 取得：

```java
UserManagerInternal
```

之后用户事件才能把 userId 转成 UserInfo/TargetUser。如果尚未 `preSystemReady()` 或 userId 不存在，SSM 抛 IllegalStateException。

这说明用户回调依赖 UserManager 的内部服务已经发布，不是 SSM 自己维护用户数据库。

---

## 32. `isUserSupported()` 的过滤

SystemService 默认：

```java
public boolean isUserSupported(TargetUser user) {
    return true;
}
```

不支持某些用户类型的服务可覆写，例如只支持 full user、不支持 profile 或 headless system user。

SSM 分发前检查；switch 事件特殊处理：只要 from 或 to 任一用户受支持，就仍调用服务，因为服务可能需要离开旧支持用户或进入新支持用户。

```text
from supported, to unsupported → 仍需通知退出
from unsupported, to supported → 仍需通知进入
```

---

## 33. 用户回调的异常策略与 BootPhase 不同

BootPhase 中服务抛异常：包装成 RuntimeException，通常中断系统启动。

用户事件中服务抛异常：

```java
Slog.wtf(...)
```

然后继续给后续服务分发。

原因是用户切换/停止发生在运行期，一个服务失败不应轻易让其他所有服务错过清理。但 `wtf` 仍表示严重 Framework 错误，不是可以忽略的正常事件。

---

## 34. 用户回调在哪个线程执行

Android 11 此处是普通循环直接调用服务方法，没有为每个服务创建线程：

```text
调用 SSM 用户入口的线程
  → service A callback
  → service B callback
  → service C callback
```

因此：

- 回调通常串行；
- 慢服务会拖慢整个用户状态推进；
- 超过 50ms 会警告；
- 服务若需异步工作，要自己安排线程；
- 若后续用户状态依赖任务完成，必须保留正确同步屏障。

“SystemService 生命周期回调”不等于“自动运行在每个服务自己的 HandlerThread”。

---

## 35. onUserUnlocking 与 onUserUnlocked

`onUserUnlocking` 文档说明：用户正处于 `STATE_RUNNING_UNLOCKING`，CE 存储已可用；回调全部完成后才转到 `STATE_RUNNING_UNLOCKED`。

适合：

- 打开 CE 数据库；
- 迁移/加载用户凭据保护数据；
- 建立必须在 unlocked 前完成的状态。

`onUserUnlocked` 发生在状态已是 unlocked 后，更适合：

- 启动不阻塞状态切换的用户功能；
- 通知依赖已解锁语义的内部模块；
- 执行可延后的工作。

若业务同时接受 unlocking/unlocked，应使用 `isUserUnlockingOrUnlocked()`，避免在窗口期错误拒绝。

---

## 36. onUserStopping 与 onUserStopped

`onUserStopping`：

- 在该用户 SHUTDOWN 广播之前；
- 应停止使用用户资源、解绑该用户进程服务；
- 是仍可访问目标用户 CE 存储的最后回调。

`onUserStopped`：

- 该用户所有应用进程清理之后；
- 用于删除内存缓存、listener、session 等最终状态；
- 不应再假设 CE 数据可访问。

把磁盘写入拖到 `onUserStopped` 可能已经太晚。

---

## 37. 前台用户切换与用户启动不是一回事

用户可以已经在后台 running/unlocked，再成为前台用户。此时主要发生：

```text
onUserSwitching(from, to)
```

而不是重新完整调用 start/unlock。

一个服务应分开维护：

- running users 集合；
- unlocked users 集合；
- current foreground user；
- 每用户资源与仅前台资源。

仅用一个 `mCurrentUser` 无法覆盖后台多用户运行模型。

---

## 38. 服务启动异常的传播

反射阶段可能失败：

- 不是 SystemService 子类；
- 没有 public `Context` 构造器；
- 构造器本身抛异常；
- 类不存在；
- jar/ClassLoader 配置错误。

`onStart()` 也可能抛 RuntimeException。SSM 包装后继续抛出，SystemServer 外层记录：

```text
Failure starting system services
```

并重新抛出。关键启动失败通常导致 system_server 退出，由 init/zygote 相关机制重新拉起；反复失败可能进入 Watchdog/RescueParty 等更高层恢复路径。

不要在 `onStart()` 捕获所有 Throwable 后假装成功。若关键不变量不成立，带着半初始化服务继续运行往往更危险；可选能力则应在明确设计下优雅降级。

---

## 39. SystemServer 为什么有大量 try/catch `reportWtf`

并非所有启动步骤都经 SSM，也并非所有失败都同等致命。SystemServer 对某些“make ready”或可选服务调用会：

```java
try { ... } catch (Throwable e) {
    reportWtf("...", e);
}
```

这表示该调用点有意选择记录严重错误后继续启动。它是逐点的容错决策，不是 SSM 默认策略。

阅读异常边界时要看真实调用者：

```text
SSM start/onBootPhase 默认传播
SystemServer 某些显式步骤选择 reportWtf 后继续
用户生命周期 SSM 内部 wtf 后继续下一服务
```

---

## 40. 慢调用 warning 如何理解

SSM 对 `onStart`、`onBootPhase`、用户回调都测量 elapsed realtime，超过 50ms：

```text
Service X took N ms in onStart/onBootPhase/...
```

这不是 ANR，也不代表 CPU 一直执行了 N ms；其中可能包含锁等待、I/O、Binder 等墙钟延迟。

排查应结合：

- `atrace`/Perfetto 的 `StartService X`、`OnBootPhase` slice；
- 线程栈；
- Binder 调用目标；
- 磁盘 I/O；
- 是否错误等待 init thread pool future；
- 上下游启动依赖。

---

## 41. 一个服务的推荐结构

```java
public final class DemoService extends SystemService {
    private final BinderService mBinderService = new BinderService();
    private final LocalService mLocalService = new LocalService();

    public DemoService(Context context) {
        super(context);
    }

    @Override
    public void onStart() {
        publishBinderService("demo", mBinderService);
        publishLocalService(DemoManagerInternal.class, mLocalService);
    }

    @Override
    public void onBootPhase(int phase) {
        if (phase == PHASE_SYSTEM_SERVICES_READY) {
            // 取得并连接核心依赖
        } else if (phase == PHASE_THIRD_PARTY_APPS_CAN_START) {
            // 开放涉及第三方进程的能力
        }
    }

    @Override
    public void onUserUnlocking(TargetUser user) {
        // 打开该用户 CE 数据
    }

    @Override
    public void onUserStopping(TargetUser user) {
        // 在 CE 最后可用阶段提交并关闭
    }
}
```

真实服务还需设计锁、Handler、权限、SELinux、错误恢复与 dump。

---

## 42. Binder 过早发布的风险

PowerManager 的注释强调：它早期发布后必须立即能够处理 native daemon 调用，包括权限验证。

如果普通服务在 `onStart()` 一开始发布 Binder，随后才初始化必要字段，就可能出现：

```text
publish binder
  → 客户端立刻 transact
  → 服务读到半初始化状态
```

安全方案包括：

- 发布前建立处理基本调用所需不变量；
- Binder 方法检查 ready 状态并返回明确错误；
- 把必须同步完成的初始化放在发布前；
- 不要靠“客户端应该晚一点调用”的时间假设。

Binder 发布是并发可见性边界。

---

## 43. 锁与启动依赖死锁示例

危险时序：

```text
SystemServer main: ServiceA.onStart 持有 A 锁
  → Binder 同步调用 ServiceB
ServiceB Binder thread: 回调 ServiceA
  → 等待 A 锁
SystemServer main 等 ServiceB reply
```

启动时服务未完全 ready、线程池数量有限，更容易形成等待环。

设计原则：

- 不持内部主锁做外部 Binder 调用；
- phase 回调中减少同步跨服务往返；
- 明确 LocalService 直调的调用线程与锁顺序；
- 异步 callback 不假设立即执行；
- 用 trace 和锁图审查依赖。

---

## 44. LocalService 在启动依赖中的作用

服务 A 在 `onStart()` 发布 LocalService 后，后续服务 B 可通过：

```java
LocalServices.getService(AInternal.class)
```

取得同进程接口。这能显式表达“B 必须在 A 发布之后启动”。若顺序反了，可能得到 null。

LocalServices 没有等待、lazy start 或通知机制，因此依赖仍由 SystemServer 顺序/phase 保证。

而且直调不会切换到 A 的线程；若 AInternal 要求特定线程，接口实现必须 post 到 Handler 或明确注释调用约束。

---

## 45. BootPhase 中启动新服务为什么危险

技术上 `onBootPhase()` 可以通过 LocalServices 取得 SSM 再启动服务，但新服务不会自动收到当前或过去 phase，而且会令启动顺序更隐蔽。

除非架构明确设计，否则更易读的方式是：

- 在 SystemServer/受控 initializer 中显式启动；
- 在正确 phase 前加入列表；
- phase 回调只推进已有服务状态。

否则新服务可能认为 `SYSTEM_SERVICES_READY` 将来会来，实际已经错过。

---

## 46. 服务的线程模型不是 SSM 决定的

SSM 只在调用线程同步执行生命周期方法。服务运行期可以选择：

- 在 system_server Binder 线程池处理 Binder 请求；
- 用主线程 Handler；
- 创建专用 HandlerThread；
- 使用共享 BackgroundThread/FgThread/IoThread；
- 把工作交给 native daemon。

因此看到服务由 SSM 在 system_server 主线程 `onStart()`，不能推断它所有 Binder 方法也在主线程。

必须分别追：

```text
生命周期调用线程
Binder 入站线程
Handler 实际处理线程
callback 返回线程
```

---

## 47. SystemServiceManager 是否管理服务停止

Android 11 SSM 没有一个与 `startService()` 对称的通用 `stopService()` 来卸载整个 SystemService。

system_server 服务通常与进程同生命周期：

```text
启动一次 → 运行到 system_server 退出
```

用户停止只清理该用户相关状态，不是销毁整个服务对象。硬件/模块可以内部断开重连，但仍由具体服务实现。

这与应用组件 Service 的 `onCreate/onDestroy` 生命周期完全不同。

---

## 48. 与应用 Service 的区别

| SystemService | 应用 `android.app.Service` |
|---|---|
| 主要在 system_server | 在应用进程 |
| SSM 创建/启动 | AMS/ActiveServices 调度 |
| `onStart/onBootPhase/onUser...` | `onCreate/onStartCommand/onBind/onDestroy` |
| 通常进程级常驻 | 可按组件需求创建销毁 |
| `publishBinderService` 注册全局 Binder 名字 | `onBind` 返回给绑定者 Binder |
| 强特权内部代码 | 受应用 UID/沙箱约束 |

两者名字都有 Service，但不是同一套框架。

---

## 49. 常见误区纠正

### 误区 1：SystemServiceManager 就是 ServiceManager

错误。前者在 system_server 内管理 Java 服务生命周期；后者是 Binder 服务目录。

### 误区 2：`startService()` 返回就代表服务所有功能 ready

错误。它只表明构造和 `onStart()` 返回。

### 误区 3：BootPhase 会自动检查依赖条件

错误。SSM 只分发数值；SystemServer 与各服务共同保证里程碑语义。

### 误区 4：BootPhase 回调并行执行

错误。Android 11 SSM 按服务列表串行直调。

### 误区 5：晚启动服务会收到历史 phase

错误。`startService()` 没有补发机制。

### 误区 6：服务 onStart 抛异常后 SSM 会跳过并继续

错误。默认包装并向上抛，关键启动通常失败。

### 误区 7：用户 unlocked 等于设备 boot completed

错误。两条生命周期相互关联但独立。

### 误区 8：onUserStopped 仍适合写 CE 数据

错误。onUserStopping 才是仍能访问 CE 的最后回调。

### 误区 9：LocalService 调用自动进入服务线程

错误。它在调用者线程普通 Java 直调。

### 误区 10：所有 system_server 服务都由 SSM 启动

错误。仍有历史、native、模块化和特殊静态入口。

---

## 50. 完整启动时序图

```text
SystemServer main
  │
  ├─ createSystemContext
  ├─ new SystemServiceManager
  ├─ start init thread pool
  │
  ├─ startBootstrapServices
  │    ├─ start Power/Display/AMS/PMS...
  │    └─ phase 100: default display
  │
  ├─ startCoreServices
  │    └─ Battery/Usage...
  │
  ├─ startOtherServices
  │    ├─ 更多服务
  │    ├─ phase 480: lock settings
  │    ├─ phase 500: system services
  │    ├─ phase 520: device specific
  │    ├─ phase 550: activity manager
  │    └─ phase 600: third-party apps
  │
  └─ AMS.finishBooting（稍后）
       └─ phase 1000: boot completed
            └─ shutdown init thread pool
```

这不是每行都紧邻执行；部分工作通过 AMS callback、future 和异步线程完成。

---

## 51. 用户生命周期图

```text
UserController / AMS
       │
       ├─ SSM.startUser(10)
       │    └─ services.onUserStarting(10)
       │
       ├─ SSM.unlockUser(10)
       │    └─ services.onUserUnlocking(10)   CE 已可用
       │
       ├─ SSM.onUserUnlocked(10)
       │    └─ services.onUserUnlocked(10)
       │
       ├─ SSM.switchUser(0, 10)
       │    └─ services.onUserSwitching(0,10)
       │
       ├─ SSM.stopUser(10)
       │    └─ services.onUserStopping(10)    CE 最后可用窗口
       │
       └─ SSM.cleanupUser(10)
            └─ services.onUserStopped(10)     进程清理完成
```

---

## 52. 新服务启动设计检查表

### 构造与发布

- 是否有 public `Context` 构造器？
- 构造器是否足够轻？
- 发布 Binder 前是否建立必要不变量？
- 是否需要同时发布 LocalService？
- add/find/call SELinux 和 Framework 权限是否完整？

### 依赖与 phase

- 必须在谁之后启动？
- 谁必须在它之后启动？
- 哪个 phase 才能调用外部服务？
- 是否可能晚于所需 phase 才加入 mServices？
- 异步任务在哪里等待完成？

### 用户

- 数据是 device-wide 还是 per-user？
- 是否依赖 DE/CE？
- 支持 full user、profile、headless system user 中哪些？
- stop 时何时提交 CE 数据？
- switch 与 start 是否被错误合并？

### 性能与恢复

- 回调是否可能超过 50ms？
- 是否持锁跨 Binder 调用？
- system_server runtime restart 后如何恢复？
- 依赖 Binder 死亡后如何重连？

---

## 53. Mac 上的源码阅读练习

### 练习 1：追一项服务的创建

```bash
rg -n "startService\(PowerManagerService.class|class PowerManagerService|void onStart\(" \
  frameworks/base/services
```

写出构造、加入 mServices、onStart、publish Binder 的顺序。

### 练习 2：标记所有 phase 调用点

```bash
rg -n "startBootPhase" \
  frameworks/base/services/java/com/android/server/SystemServer.java \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

目标：不要只看常量声明，要知道每个 phase 在真实启动代码何处推进。

### 练习 3：选一个服务阅读 phase

```bash
rg -n "onBootPhase" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
```

记录每个 phase 前后，服务新增了哪些能力。

### 练习 4：追用户事件来源

```bash
rg -n "startUser\(|unlockUser\(|onUserUnlocked\(|switchUser\(|stopUser\(|cleanupUser\(" \
  frameworks/base/services/core/java/com/android/server/am \
  frameworks/base/services/core/java/com/android/server/SystemServiceManager.java
```

区分 UserController 状态变化与 SSM 回调分发。

### 练习 5：找异步任务的完成屏障

```bash
rg -n "SystemServerInitThreadPool|waitForFuture|Future<|CountDownLatch" \
  frameworks/base/services/java/com/android/server/SystemServer.java
```

对每个 submit 问：谁等待它、在哪个依赖点等待、异常如何传播？

---

## 54. 自测题

1. SSM 与 servicemanager 的区别是什么？
2. `startService(Class)` 对构造器有什么要求？
3. 实例何时加入 `mServices`？
4. `onStart()` 返回说明哪些事情，不说明哪些事情？
5. BootPhase 为什么必须单调递增？
6. 某服务在 phase 500 后启动，会自动收到 100～500 吗？
7. phase 回调在哪个线程、按什么顺序调用？
8. 超过 50ms 会怎样？
9. PHASE_BOOT_COMPLETED 为什么不一定在 startOtherServices 返回前发生？
10. onUserUnlocking 与 onUserUnlocked 的区别是什么？
11. 哪个是访问 CE 的最后回调？
12. 用户回调抛异常和 BootPhase 抛异常的处理有何不同？
13. LocalService 调用会自动切线程吗？
14. init thread pool 为什么需要完成屏障？

---

## 55. 参考答案

1. SSM 管 system_server 内 Java 服务生命周期；servicemanager 管跨进程名字到 Binder 的目录。
2. 必须继承 SystemService，并具有 public `Constructor(Context)`。
3. 在调用 `onStart()` 之前。
4. 说明构造和 onStart 完成；不保证 Binder 已发布、所有 phase/用户数据/依赖和业务都 ready。
5. phase 是累积里程碑，倒退或重复会破坏一次性初始化假设。
6. 不会，Android 11 startService 不补发历史 phase。
7. 在调用 startBootPhase 的线程上，按 mServices 注册顺序串行。
8. 记录 warning；不会自动取消，但会拖慢后续服务。
9. 最终由 AMS `finishBooting()` 在其启动条件满足后推进；这只是 Framework phase 里程碑，
   不等价于 Home 首帧、用户已可交互或所有 BOOT_COMPLETED Receiver 已完成。
10. unlocking 时 CE 已可用但用户仍在状态转换；unlocked 是转换完成后。
11. `onUserStopping()`。
12. BootPhase 默认向上抛并中断启动；用户回调记录 wtf 后继续分发其他服务。
13. 不会，是调用者线程的 Java 直调。
14. 否则主线程会越过实际依赖，后续 phase/服务可能看到未完成状态。

---

## 56. 第二遍复读：六个最容易混淆的完成点

### 56.1 created 与 started

构造器返回只是 created；`onStart()` 返回才是 SSM 意义的 started。SSM 在二者之间先把实例加入生命周期列表。

### 56.2 started 与 published

`onStart()` 通常发布 Binder，但框架没有强制每个服务必须发布，也没有自动验证名字。Local-only 服务可能根本没有公共 Binder。

### 56.3 published 与 callable

名字已在 servicemanager 可见，不代表任意客户端有 SELinux find/call、Framework permission，也不代表服务内部 ready。

### 56.4 phase reached 与每个异步任务完成

SSM 只知道同步回调已经返回。服务回调若 fire-and-forget 提交异步任务，除非显式等待，phase 完成并不保证该任务结束。

### 56.5 device boot 与 user unlock

BootPhase 是设备/system_server 主时间线；用户回调是每用户可重复时间线。设备 boot completed 时仍可能有用户未启动或未解锁。

### 56.6 stopping 与 stopped

stopping 是停止过程且 CE 最后可用；stopped 是用户进程清理后的最终内存清理阶段。二者不能只当同一事件的两个名字。

---

## 57. 本章总结

把 SSM 主链压缩为：

```text
SystemServer 按显式依赖顺序
  → SSM 反射构造 SystemService
  → 先加入 mServices
  → 同步 onStart
  → 服务发布 Binder/LocalService
  → SystemServer 单调推进 BootPhase
  → SSM 按启动顺序串行回调所有已启动服务
  → AMS/UserController 在运行期触发每用户生命周期
```

真正读懂启动代码的关键，不是记住 phase 数值，而是始终追问：

```text
谁在什么线程调用？
此前依赖真的完成了吗？
回调返回代表哪个完成点？
是否还有异步任务？
服务已发布是否就能处理所有调用？
当前是设备阶段还是某个用户阶段？
```

---

## 58. 下一章预告

第 96 章将学习：

**Android SystemServer 启动性能：TimingsTrace、InitThreadPool、启动依赖与卡顿排查**

将重点研究：

- SystemServer 主线程启动 trace 如何形成；
- `TimingsTraceAndSlog` 与 Perfetto/atrace 对应关系；
- InitThreadPool 任务如何并行与汇合；
- 50ms slow service warning 怎样定位；
- Binder、锁、磁盘 I/O 和 ClassLoader 如何拖慢启动；
- 如何在不真正编译的情况下做源码级启动关键路径分析。
