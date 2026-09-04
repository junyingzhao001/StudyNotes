# 95 Android SystemServiceManager：系统服务启动、BootPhase 与用户生命周期

> 源码版本：Android 11（`android-11.0.0_r48`）
> 阅读环境：macOS 本地只读源码；验证命令不要求编译或连接设备。
> 贯穿实例：`StorageManagerService.Lifecycle`，对外 Binder 名为 `mount`。

## 1. 先看真实故障：`mount` 已存在，为什么存储仍未就绪

一次开机卡顿可能呈现出看似矛盾的现象：

```text
service check mount：found
SystemServer：已经打印 StartStorageManagerService
后续服务：仍然因为存储、包信息或用户目录未准备好而失败
```

只看 Binder 名字，很容易得出“StorageManagerService 已完全启动”的错误结论。

Android 11 的真实路径却是：

```text
构造 Lifecycle
  → 先加入 SystemServiceManager.mServices
  → Lifecycle.onStart()
      → 构造 StorageManagerService
      → publishBinderService("mount", ...)
      → 尝试连接 vold / storaged
  → 后续 phase 500、550、1000 再推进不同能力
  → 每个用户又有 start / unlock / switch / stop 生命周期
```

其中连接 daemon 失败会安排重试；某些 phase 方法还只是向服务自己的 Handler 投递消息。
所以“服务名已注册”只覆盖了这条链中的一个完成点。

**一句话结论：SystemServiceManager（SSM）按显式顺序调用生命周期方法，但它既不计算依赖图，也不证明服务已经业务可用。**

学完本章，你应该能解决三类问题：

- 开机为何卡在某个服务或某个 BootPhase；
- 服务为何“查得到”，调用时却仍遇到未初始化依赖；
- 用户切换、解锁或停止时，为什么某个服务状态没有及时更新。

本章只讨论 system_server 内 `SystemService` 生命周期框架。Binder 目录、应用侧
Manager wrapper、native daemon 自身生命周期只在边界处说明。

## 2. 先分清三个名字相近的角色

| 角色 | 所在位置 | 管理对象 | 本章实例中的动作 |
|---|---|---|---|
| `SystemServiceManager` | system_server | `SystemService` Java 对象及生命周期 | 构造并启动 `StorageManagerService.Lifecycle` |
| `ServiceManager` / servicemanager | system_server 门面 / 独立进程 | 服务名到 Binder 的目录 | 保存 `mount → IStorageManager Binder` |
| `SystemServiceRegistry` | Framework 客户端侧 | `Context.getSystemService()` 的 Manager wrapper | 创建并缓存 `StorageManager` |

三者的连接关系是：

```text
SystemServer
  → SystemServiceManager.startService(Lifecycle)
  → Lifecycle.onStart()
  → publishBinderService("mount", binder)
  → ServiceManager.addService("mount", binder)

应用 Context.getSystemService(Context.STORAGE_SERVICE)
  → SystemServiceRegistry 创建 StorageManager
  → StorageManager 再查询 Binder 名 "mount"
```

这里甚至有两个不同的字符串：`Context.STORAGE_SERVICE` 是 `"storage"`，
IStorageManager 的 Binder 目录名是 `"mount"`。不要用 wrapper 名推断 Binder 名。

关键源码集中在 `SystemServer.java`、`SystemServiceManager.java`、`SystemService.java`、
`StorageManagerService.java`、`ActivityManagerService.java`、`UserController.java`，以及客户端侧的
`SystemServiceRegistry.java`、`StorageManager.java`。

SystemServer 创建 SSM 后，只把它放进进程内的 `LocalServices`：

```java
mSystemServiceManager = new SystemServiceManager(mSystemContext);
mSystemServiceManager.setStartInfo(...);
LocalServices.addService(SystemServiceManager.class, mSystemServiceManager);
```

SSM 自己不是可供应用查询的 Binder 服务。

随后 SystemServer 依次调用：

```java
startBootstrapServices(t);
startCoreServices(t);
startOtherServices(t);
```

这三段首先是源码组织和人工编排边界，不是 SSM 自动计算出的依赖层级。

## 3. 一条主线看完：StorageManagerService 怎样启动和发布

这个服务同时具备字符串类名启动、Lifecycle wrapper、Binder 早发布、三个 phase，
以及新旧用户回调，适合在同一路径观察 Android 11 的迁移期边界。

### 3.1 SystemServer 的显式位置就是依赖的一部分

`StorageManagerService$Lifecycle` 在 `startOtherServices()` 中启动：

```java
mSystemServiceManager.startService(STORAGE_MANAGER_SERVICE_CLASS);
storageManager = IStorageManager.Stub.asInterface(
        ServiceManager.getService("mount"));
```

源码紧邻注释说明：后面的通知服务依赖存储服务，所以必须先启动存储。
这条顺序写在 SystemServer 代码里，SSM 没有 `dependsOn()` 图可供解析。

此启动还受工厂测试模式和 `system_init.startmountservice` 属性条件控制。
因此某产品上“类存在”也不等于这条调用一定执行。

### 3.2 `startService(String)` 最终仍走统一反射入口

SSM 的路径可压缩为：

```text
按类名加载 StorageManagerService$Lifecycle
  → 确认它继承 SystemService
  → 查找 public Constructor(Context)
  → constructor.newInstance(mContext)
  → startService(instance)
```

被反射选择的构造器签名必须是 public `(Context)`；类可以另有其他构造器。

`Lifecycle` 构造器本身很轻：

```java
public Lifecycle(Context context) {
    super(context);
}
```

真正的 `StorageManagerService` 到 `onStart()` 才创建。

### 3.3 最容易漏看的顺序：先入表，再 `onStart`

SSM 的核心只有几行：

```java
mServices.add(service);
service.onStart();
warnIfTooLong(..., service, "onStart");
```

因此 `mServices` 表示“应接收后续生命周期的对象”，并不严格等于“已经成功完成
onStart 的对象”。`onStart()` 抛异常时没有自动从列表回滚。

Storage 的 `onStart()` 是：

```java
mStorageManagerService = new StorageManagerService(getContext());
publishBinderService("mount", mStorageManagerService);
mStorageManagerService.start();
```

`publishBinderService()` 最终只是：

```java
ServiceManager.addService(name, service, allowIsolated, dumpPriority);
```

SSM 不替服务创建 Binder Stub，也不检查发布了哪个名字。

### 3.4 `onStart()` 返回时 daemon 仍可能缺席

`StorageManagerService.start()` 尝试连接两个已有 native Binder 服务：

```java
connectStoraged();
connectVold();
```

如果查不到，代码不是在 SSM 主链上无限等待，而是向 `BackgroundThread` 每秒投递重连：

```text
storaged/vold 未找到
  → 记录 trying again
  → postDelayed(connect..., 1 秒)
  → onStart 仍可返回
```

这正是首屏故障的答案：`mount` 可见，表示客户端能取得 Binder；它不证明服务已连上
vold、storaged，也不证明 PackageManager、AppOps、用户存储状态已经准备好。

## 4. 构造、启动、发布和 ready 是不同完成点

排障时不要只说“服务启动了”，应指出下面哪一层已经完成：

| 完成点 | Storage 主线中的证据 | 尚不能推出 |
|---|---|---|
| `Lifecycle` 构造完成 | public `Context` 构造器返回 | 真正存储服务已创建 |
| 加入 `mServices` | `mServices.add(service)` | `onStart()` 成功 |
| 实现对象构造完成 | `new StorageManagerService(...)` 返回 | `mount` 已发布、daemon 已连接 |
| Binder 发布调用完成 | `publishBinderService("mount", ...)` | 所有业务方法都 ready |
| `onStart()` 返回 | `startService()` 可返回 | phase、用户数据、异步重连完成 |
| phase 500 回调返回 | `servicesReady()` 返回 | phase 550/1000 的工作完成 |
| phase 550 回调返回 | `systemReady()` 已投递消息 | Handler 已处理该消息 |
| phase 1000 回调返回 | `bootCompleted()` 同步部分返回 | 所有异步初始化、应用广播完成 |
| 用户 unlocking 回调返回 | 必要 staging 与 daemon 同步调用结束 | `H_COMPLETE_UNLOCK_USER` 已处理 |

### 4.1 `onStart()` 的框架契约很窄

`SystemService` 建议在 `onStart()` 发布 Binder 和 LocalService，但框架并不要求每个服务
都发布 Binder，也不验证“服务业务可用”。Local-only 服务完全可以没有全局名字。

对 Storage 而言，真正实现对象的构造器已经做了不少事情：创建 HandlerThread、读取配置、
发布 `StorageManagerInternal` LocalService、注册 receiver。它说明“构造器应轻量”是设计建议，
不是 SSM 强制规则。

### 4.2 Binder 发布是并发可见性边界

一旦 `mount` 进入 servicemanager，其他线程或进程就可能立即发事务。
若发布前没有建立 Binder 方法所需的最小不变量，客户端可能观察到半初始化状态。

应在发布前建立基础请求所需的不变量；未到目标 phase 时明确拒绝暂不可用功能；
对可恢复依赖可以重连，但不能把“正在重连”伪装成“已经完全可用”。

### 4.3 `systemReady()` 不是统一完成点

具体服务内部常有 `servicesReady()`、`systemReady()`、`bootCompleted()` 等历史命名。
它们的语义由服务自己定义。只有真实调用点才能说明它对应哪个 phase，不能看到名字就自动
映射为 `PHASE_SYSTEM_SERVICES_READY`。

## 5. BootPhase：单调里程碑，不是依赖求解器

Android 11 的阶段常量如下：

| 值 | 常量 | 契约重点 |
|---:|---|---|
| 100 | `PHASE_WAIT_FOR_DEFAULT_DISPLAY` | 默认显示应可用 |
| 480 | `PHASE_LOCK_SETTINGS_READY` | 可取得锁设置数据 |
| 500 | `PHASE_SYSTEM_SERVICES_READY` | 可安全调用核心系统服务 |
| 520 | `PHASE_DEVICE_SPECIFIC_SERVICES_READY` | 设备特定服务已到约定点 |
| 550 | `PHASE_ACTIVITY_MANAGER_READY` | 可以发送广播 |
| 600 | `PHASE_THIRD_PARTY_APPS_CAN_START` | 可 start/bind 三方应用 |
| 1000 | `PHASE_BOOT_COMPLETED` | Framework 最终 boot phase |

数值有意留空；不要写“下一阶段等于当前阶段加一”。

SSM 推进 phase 的关键逻辑是：

```java
if (phase <= mCurrentPhase) {
    throw new IllegalArgumentException(...);
}
mCurrentPhase = phase;
final int serviceLen = mServices.size();
for (int i = 0; i < serviceLen; i++) {
    mServices.get(i).onBootPhase(mCurrentPhase);
}
```

由此得到五个精确结论：

1. phase 对整个 SSM 严格单调；重复和倒退都会失败。
2. `mCurrentPhase` 在分发前已经更新；中途失败不会回滚。
3. 回调按加入 `mServices` 的顺序串行执行。
4. 本轮先固定 `serviceLen`；回调中新增的服务不会插入本轮尾部。
5. 晚启动服务不会补收已经过去的 phase，只会收到未来 phase。

SSM 本身不切线程：前六个 phase 来自 SystemServer 启动路径，phase 1000 则由 AMS `finishBooting()` 推进；判断线程应追真实调用点。

所以在 phase 500 之后才启动 Storage Lifecycle，它不会自动执行 `servicesReady()`。
这类错误能编译，也可能先看到 `mount`，直到业务访问包或 AppOps 数据时才暴露。

### 5.1 Storage 对三个 phase 的真实响应

```java
if (phase == PHASE_SYSTEM_SERVICES_READY) {
    mStorageManagerService.servicesReady();
} else if (phase == PHASE_ACTIVITY_MANAGER_READY) {
    mStorageManagerService.systemReady();
} else if (phase == PHASE_BOOT_COMPLETED) {
    mStorageManagerService.bootCompleted();
}
```

- phase 500：取得 `PackageManagerInternal`、PackageManager Binder、AppOps Binder，并解析 provider。
- phase 550：注册 screen observer，然后向 Storage Handler 投递 `H_SYSTEM_READY`。
- phase 1000：设置 `mBootCompleted`，投递 `H_BOOT_COMPLETED`，并同步检查 FUSE 属性。

“phase 回调返回”只说明同步方法返回。phase 550 投递的 Handler 消息可能仍在队列中；
SSM 不追踪它，也不会自动为下一 phase 建立屏障。

### 5.2 phase 1000 不等于所有“开机完成”观察点

Android 11 中，phase 1000 由 `ActivityManagerService.finishBooting()` 推进，不是紧跟
`startOtherServices()` 返回立即发生。该方法还会等待 boot animation 等条件。

它不等于：

- Launcher 首帧已经绘制；
- 每个用户都已解锁；
- 所有 `BOOT_COMPLETED` Receiver 已执行完；
- 每个服务自己投递的异步工作都已结束。

phase 1000 分发完成后，SSM 还会关闭 `SystemServerInitThreadPool`。这只约束启动专用线程池，
不代表所有服务自建 Handler 或 executor 已清空。

## 6. 依赖顺序、晚启动和失败：SSM 不替你做什么

### 6.1 没有自动拓扑排序

SSM 不读取这样的声明：

```text
Storage dependsOn PackageManager
Notification dependsOn Storage
```

Android 11 主要靠以下机制人工表达依赖：

- SystemServer 中的代码先后；
- “必须在 X 前/后”的源码注释；
- 服务选择在哪个 BootPhase 获取依赖；
- `Future`、latch 或显式等待形成的完成屏障；
- 服务自己的 ready 标志和错误处理。

Storage 先于通知服务启动，是代码顺序；Storage 到 phase 500 才取包和 AppOps 依赖，是阶段契约。
SSM 不检查这些选择是否互相一致。

### 6.2 LocalServices 也不会替你等待

`LocalServices.getService(SomeInternal.class)` 是 system_server 内普通 Java 查表：

```text
已发布 → 立即返回对象
未发布 → 返回 null
```

它没有 lazy start、依赖通知或线程切换。LocalService 的方法也在调用者线程直接执行，除非实现
显式 post 到自己的 Handler。

### 6.3 迟启动的修复不是“再发一次旧 phase”

phase 严格递增，同一个 phase 不能重放。发现服务启动位置太晚时，通常应：

1. 把服务移到所需 phase 之前；
2. 或重构服务，让它只依赖未来阶段；
3. 为真正异步依赖设置明确、可验证的完成屏障。

在 `onBootPhase()` 中隐式启动另一服务尤其危险：新服务不会收到当前 phase，依赖关系也更难读。

### 6.4 SSM 抛异常，最终是否致命仍由调用者决定

SSM 会包装并向上抛构造、`onStart()` 和 phase 回调异常，但它不是最终恢复策略。
SystemServer 外层通常把未处理的启动异常记录为 `Failure starting system services` 后重新抛出；
某些具体调用点又会 `catch (Throwable)`、`reportWtf()` 后继续。

Storage 的启动调用正位于局部 `try/catch` 内。若 `onStart()` 失败：

```text
Lifecycle 已在 mServices
  → SSM 抛出异常
  → SystemServer 此处可以记录后继续
  → 以后 phase 仍可能碰到这个半初始化 Lifecycle
```

因此“SSM 一定让 system_server 崩溃”和“SSM 会跳过坏服务继续”都不准确；要追真实调用者。

## 7. 用户生命周期：另一条可重复的时间线

BootPhase 是一次、单调的设备/system_server 时间线；用户事件可对多个用户重复发生：

```text
设备：phase 100 → 480 → 500 → 520 → 550 → 600 ─────→ 1000

用户 0：start → unlocking → unlocked ───────────────────────
用户 10：                 start → unlocking → unlocked → stop → stopped
前台用户：0 ───────────────────────────── switch 0 → 10 ─────
```

SSM 的入口与新式回调对应为：

| SSM 入口 | `SystemService` 回调 | 边界 |
|---|---|---|
| `startUser` | `onUserStarting` | 建立 running user 状态 |
| `unlockUser` | `onUserUnlocking` | CE 已可用，仍在 unlocking |
| `onUserUnlocked` | `onUserUnlocked` | unlocked 转换完成 |
| `switchUser(from,to)` | `onUserSwitching` | 前台用户改变 |
| `stopUser` | `onUserStopping` | 停止前；仍可访问 CE 的最后回调 |
| `cleanupUser` | `onUserStopped` | 用户进程清理完成后的最终释放 |

在分发前，`preSystemReady()` 从 LocalServices 取得 `UserManagerInternal`。
若它尚未设置或 userId 不存在，构造 `TargetUser` 时会抛 `IllegalStateException`。
SSM 自己不维护用户数据库。

`TargetUser` 包装的是 UserManagerService 引用的 live `UserInfo`；服务可以读取，不能修改。
分发还会先调用 `isUserSupported()`（默认 true）；switch 时 from/to 任一用户受支持就会通知。
Storage Lifecycle 没有覆写该过滤器，因此默认接收所有用户。

### 7.1 Android 11 deprecated bridge 的准确方向

SSM 只调用新式 `TargetUser` 方法。`SystemService` 基类再从新方法桥接到旧方法：

```text
onUserStarting(TargetUser)
  → onStartUser(UserInfo)
  → onStartUser(int)

onUserUnlocking(TargetUser) → onUnlockUser(UserInfo) → onUnlockUser(int)
onUserSwitching(from,to)    → onSwitchUser(UserInfo,UserInfo) → onSwitchUser(int)
onUserStopping(TargetUser)  → onStopUser(UserInfo) → onStopUser(int)
onUserStopped(TargetUser)   → onCleanupUser(UserInfo) → onCleanupUser(int)
```

`onUserUnlocked(TargetUser)` 在 r48 没有对应的 deprecated 链，默认实现为空。

Storage 恰好展示迁移期混用：

- 直接覆写新式 `onUserStarting(TargetUser)`；
- 仍覆写旧式 `onUnlockUser(int)`、`onSwitchUser(int)`、`onStopUser(int)`、
  `onCleanupUser(int)`；
- 依靠基类桥接使这些旧 override 仍被调用；
- 没有覆写 `onUserUnlocked(TargetUser)`。

如果子类改为覆写新方法但不调用 `super`，旧 override 不会再自动执行。这是迁移时常见遗漏。

### 7.2 Storage 的用户完成边界

`onUnlockUser(int)` 有意同步阻塞，先确保用户 staging area 可供 zygote 派生进程 bind mount，
再通知 vold、storaged；之后只投递 `H_COMPLETE_UNLOCK_USER`。

因此回调返回时：关键挂载前置动作已完成，但 volume 广播、旧 OBB 迁移等 Handler 工作可以仍未完成。

`onStopUser(int)` 关闭 storage session 并撤销 package monitor；`onCleanupUser(int)` 才通知
vold/storaged 用户停止并从 `mSystemUnlockedUsers` 移除。两者不是同一事件的两个名字。

### 7.3 用户回调运行在哪个线程

SSM 的 `onUser()` 只是普通 for 循环，没有内部 Handler，也没有为每个服务创建线程。
回调运行在调用 SSM 入口的线程上，并按 `mServices` 顺序串行执行。

在 r48 的真实调用点中：

- 初始 system user 的 `startUser()` 可从 AMS `systemReady()` 主启动路径直接进入；
- 后续 start/unlock/switch 消息多由 `UserController.mHandler` 处理；
- 该 Handler 使用 AMS 自己 `ServiceThread` 的 Looper，而不是服务专属线程；
- Storage 回调内部再决定同步 Binder 调用或投递到 Storage Handler。

所以不能笼统写“所有用户回调都在 SystemServer 主线程”，也不能写“SSM 自动切到服务线程”。

## 8. 异常、50ms 告警和 trace 的真实含义

### 8.1 三类分发的异常策略不同

| 路径 | r48 SSM 行为 | 后果边界 |
|---|---|---|
| 反射构造 | 包装为 `RuntimeException` 并上抛 | 调用者决定终止还是捕获 |
| `onStart()` | 捕获 `RuntimeException`，包装后上抛 | 对象已留在 `mServices` |
| `onBootPhase()` | 捕获 `Exception`，包装后上抛 | 中断本轮；phase 已更新，不回滚 |
| 用户回调 | 每服务捕获 `Exception`，`Slog.wtf` | 继续通知后面的服务 |

用户运行期需要尽量让其他服务仍收到清理事件，所以采用“记录严重错误后继续”；
启动 phase 更强调全局不变量，默认中断。两者都不是自动恢复协议。

### 8.2 50ms 是静态告警阈值，不是实测结论

源码固定：

```java
private static final int SERVICE_CALL_WARN_TIME_MS = 50;
if (duration > SERVICE_CALL_WARN_TIME_MS) {
    Slog.w(...);
}
```

它表示单次同步 `onStart`、`onBootPhase` 或用户回调墙钟时间大于 50ms 就打印 warning。
它不是 ANR、超时取消、CPU 使用率，也不是“此服务在所有设备实测耗时 50ms”。

计时使用 `SystemClock.elapsedRealtime()`，会包含：

- CPU 执行；
- 锁等待；
- Binder 同步等待；
- 磁盘 I/O；
- 调度停顿。

反射构造发生在 `onStart` 计时之前，但包含在外层 `StartService <class>` trace 中。
Storage 的真正实现对象是在 Lifecycle.onStart 内构造，因此这部分恰好又计入它的 onStart warning。

### 8.3 trace 结束只标记同步代码返回

SSM 会创建 `StartService <class>`、`OnBootPhase_<phase>_<class>`、
`ssm.on<event>User-<id>_<class>` 等 slice。

slice 能回答“调用线程在这个同步区间停留多久”，不能自动回答：

- post 到 Handler 的任务何时完成；
- daemon 重连何时成功；
- Binder 对端内部异步工作是否结束；
- 此耗时究竟是 CPU、锁、I/O 还是调度造成。

Storage phase 550 的 `systemReady()` 只投递消息，就是最直接的反例。
定位慢启动要把 trace、warning、线程栈、Binder 目标和异步完成信号一起看。

## 9. 按“问题 → 机制 → 验证”排查

### 9.1 问题：`mount` 查得到，但调用仍失败

机制/方案：先把“目录可见、daemon 连接、phase-ready、user-ready”拆成独立状态再逐层检查：

```text
Binder 发布完成了吗？           → service check 只能回答这一层
vold/storaged 连接了吗？         → 看连接/死亡/重试日志
phase 500 servicesReady 到了吗？ → 看 OnBootPhase_500 与依赖字段
目标用户 unlock 到哪一步？      → 看 SSM 用户 trace 与 Storage Handler
```

验证结论必须写成具体完成点，例如“`mount` 已发布，但 `H_COMPLETE_UNLOCK_USER` 未处理”，
而不是笼统写“Storage 没启动”。

### 9.2 问题：服务在 phase 500 取依赖时为空

机制/方案：SSM 没有拓扑排序；应把依赖移到 phase 前发布，或在真实依赖点建立完成屏障。

验证：

1. 找服务的 `startService` 位置；
2. 找依赖的发布位置；
3. 比较两者与所有 `startBootPhase` 调用点；
4. 查是否有异步任务，以及依赖点是否真的等待 Future/latch。

### 9.3 问题：晚启动服务从未执行初始化

机制/方案：`startService()` 不重放历史 phase；应前移启动位置或改为只依赖未来阶段。

验证：确认服务加入 `mServices` 时的 `mCurrentPhase`，再核对它只监听的 phase 是否已经过去。
不要尝试重复调用旧 phase；单调性检查会直接抛异常。

### 9.4 问题：用户解锁被某服务拖慢

机制/方案：SSM 串行直调；必要同步工作保留屏障，非关键重活才异步化。

验证：找 `ssm.onUnlockingUser-<id>_<class>` slice 和超过 50ms 的 warning，再追该回调内部
是否故意阻塞、是否随后还有异步 Handler 工作。

### 9.5 macOS 本地源码命令

在 AOSP 根目录运行：

```bash
# 确认版本
git -C frameworks/base describe --tags --exact-match HEAD

# 启动位置、显式先后与所有 phase 推进点
rg -n "StartStorageManagerService|STORAGE_MANAGER_SERVICE_CLASS|startBootPhase" \
  frameworks/base/services/java/com/android/server/SystemServer.java \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java

# SSM 的加入时机、分发、异常和 50ms 阈值
rg -n "mServices.add|SERVICE_CALL_WARN_TIME_MS|startService\(|startBootPhase|onUser\(" \
  frameworks/base/services/core/java/com/android/server/SystemServiceManager.java

# Storage 的完整生命周期
rg -n "class Lifecycle|void onStart|onBootPhase|onUserStarting|onUnlockUser|onSwitchUser|onStopUser|onCleanupUser" \
  frameworks/base/services/core/java/com/android/server/StorageManagerService.java

# 新用户 API 到 deprecated API 的桥接方向
rg -n "onUserStarting|onStartUser|onUserUnlocking|onUnlockUser|onUserSwitching|onSwitchUser|onUserStopping|onStopUser|onUserStopped|onCleanupUser" \
  frameworks/base/services/core/java/com/android/server/SystemService.java

# 用户事件的真实调用线程来源
rg -n "getSystemServiceManager\(\)\.(startUser|unlockUser|onUserUnlocked|switchUser|stopUser)|systemServiceManagerCleanupUser|getHandler" \
  frameworks/base/services/core/java/com/android/server/am/UserController.java
```

## 10. 踩坑、检查题与读完就能做的事

### 10.1 十个常见误写

1. **“SSM 就是 ServiceManager。”** 错；一个管进程内生命周期，一个管跨进程 Binder 名字。
2. **“SystemServiceRegistry 启动系统服务。”** 错；它主要创建应用侧 Manager wrapper。
3. **“三大启动区是自动依赖层。”** 错；它们是 SystemServer 的显式组织和顺序。
4. **“进入 `mServices` 表示 onStart 成功。”** 错；加入发生在 onStart 之前且失败不回滚。
5. **“onStart 返回表示服务完全 ready。”** 错；只表示同步 onStart 返回。
6. **“Binder 名存在表示所有方法可用。”** 错；发布只是并发可见性边界。
7. **“晚启动服务会补收旧 phase。”** 错；r48 没有 replay。
8. **“phase 失败可以原 phase 重试。”** 错；`mCurrentPhase` 已更新，重复值被拒绝。
9. **“所有生命周期回调都自动切到服务线程。”** 错；SSM 在调用者线程直调。
10. **“50ms 是 Android 对该服务的实测耗时。”** 错；它只是静态 warning 阈值。

### 10.2 上线前检查表

- public `Context` 构造器是否存在，构造和 onStart 各自做了什么？
- 对象何时加入 `mServices`，失败后调用者是否可能继续？
- Binder/LocalService 在什么时刻发布，发布前最小不变量是否成立？
- 服务依赖谁，依赖由代码顺序、phase 还是显式屏障保证？
- 服务是否可能晚于自己需要的 phase 才启动？
- callback 返回后是否仍有 Handler、Future 或 native daemon 工作？
- 用户数据依赖 DE 还是 CE，stopping 与 stopped 分工是否正确？
- 用户回调实际从哪个 Handler/Looper 进入？
- 超过 50ms 是 CPU、I/O、锁、Binder 还是调度等待？
- 验证证据证明的是 published、phase-ready、user-ready 还是 business-ready？

### 10.3 自测题

1. 为什么 `mServices.add()` 在 `onStart()` 之前很重要？
2. `service check mount` 成功能证明哪些事实？
3. SSM 如何知道通知服务依赖存储服务？
4. phase 500 之后启动的服务会收到 phase 500 吗？
5. 某个 phase 回调抛异常后，`mCurrentPhase` 会回退吗？
6. Storage phase 550 返回能否证明 `H_SYSTEM_READY` 已处理？
7. r48 中 SSM 调用的是新式还是旧式用户回调？
8. 为什么 Storage 覆写 deprecated `onUnlockUser(int)` 仍有效？
9. `onUserStopping` 和 `onUserStopped` 哪个仍可访问 CE？
10. 50ms warning 为什么不能直接归因于 CPU 慢？

### 10.4 参考答案

1. 它说明列表是生命周期登记表，不是成功服务表；失败不会自动回滚。
2. 只证明当前调用者能从 servicemanager 取得 `mount` Binder，不能证明全部初始化完成。
3. 它不知道；依赖由 SystemServer 的显式顺序和注释表达。
4. 不会；`startService()` 不补发历史 phase。
5. 不会；SSM 先更新 phase，再开始回调，且只允许继续增大。
6. 不能；该方法只投递 Handler 消息。
7. 新式 `TargetUser` 回调。
8. 基类的新式回调默认向下桥接到 deprecated 方法。
9. `onUserStopping`；它是仍可访问目标用户 CE 的最后回调。
10. 计时是 elapsed realtime，可能包含锁、I/O、Binder 和调度等待。

### 10.5 读完就能做的事

把本章压缩成一条排障主线：

```text
SystemServer 显式排序
  → SSM 反射构造 Lifecycle
  → 先加入 mServices
  → 同步 onStart
  → 服务自行发布 Binder / LocalService
  → 单调、串行分发未来 BootPhase
  → AMS / UserController 触发可重复的用户事件
  → 服务自行决定同步完成、异步投递和真正 ready 条件
```

读任何系统服务时，始终追问四句话：

```text
谁在什么线程调用？
返回时究竟完成了哪一层？
依赖由哪条显式顺序或屏障保证？
日志、Binder 名和 trace 分别只能证明什么？
```
