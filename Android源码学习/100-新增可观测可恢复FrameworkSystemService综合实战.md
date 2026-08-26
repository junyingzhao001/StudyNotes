# 100 新增可观测、可恢复的 Framework SystemService 综合实战

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：用一个完整但不直接写入 AOSP 的教学服务，把 SystemServer、SystemServiceManager、AIDL、ServiceManager、SystemServiceRegistry、Binder 线程、Handler 状态机、多用户、SELinux、死亡恢复、可观测性和测试串成一条工程链。  
> 环境：macOS 只读源码；不要求编译、刷机或连接设备。

---

## 1. 先说明：本章示例不是仓库中已有服务

本章设计一个虚构的 `MowerDiagnosticsService`：系统组件可以提交一次割草机诊断任务，查询状态，并接收完成回调。

它是**教学设计**，不是当前 AOSP 中真实存在的类，也不是可以整段复制后立刻编译的补丁。示例省略了 import、API 审核、构建文件和部分异常处理。真实模式则来自 Android 11 的：

```text
SystemServer / SystemServiceManager / SystemService
SystemConfigService / SystemConfigManager
SystemServiceRegistry / ServiceManager / LocalServices
RemoteCallbackList / AtomicFile / dumpsys / SELinux policy
```

区分这两类内容很重要：真实源码告诉我们平台机制，教学代码负责把机制组合起来。

---

## 2. 需求先于代码

假设产品需求如下：

1. 只有持有签名权限的系统应用可以发起诊断。
2. 每个 Android 用户拥有独立任务视图。
3. 同一用户同时最多运行一个任务。
4. 发起接口必须快速返回，耗时工作不能占住 Binder 线程。
5. 调用方进程死亡后，服务仍能完成任务；无效 callback 要自动清理。
6. `system_server` 重启后，可以读回上次已经持久化的最终结果。
7. `dumpsys` 能解释当前状态、最近失败和队列延迟，但不得泄漏敏感数据。
8. 启动路径、请求数量、失败原因和耗时都可观察。

这里已经隐含了安全、线程、生命周期、存储、恢复和遥测设计。先写 Java 类再补这些，通常会造成返工。

---

## 3. 一张总架构图

```text
系统 App
  Context.getSystemService(MowerDiagnosticsManager.class)
        │ SystemServiceRegistry 创建/缓存 Manager
        ▼
MowerDiagnosticsManager
        │ IMowerDiagnosticsService Proxy
        │ Binder IPC
        ▼
ServiceManager 名字：mower_diagnostics
        ▼
MowerDiagnosticsService.BinderService（system_server Binder 线程）
        │ 权限、UID/userId、参数、调用身份检查
        │ 复制不可变请求，post
        ▼
专用 HandlerThread（唯一可变状态 owner）
        ├── UserState / TaskRecord
        ├── AtomicFile 持久化最终状态
        ├── RemoteCallbackList 通知
        ├── LocalService 供 system_server 内部调用
        └── 可选 native daemon/HAL（本章不展开数据面）
```

关键点：Manager、BinderService、业务状态机不是同一个对象承担的三个名字，而是三个责任层。

---

## 4. 先画出文件地图

若真正接入，可能涉及。下面凡标有“教学拟新增”的路径，在当前
`android-11.0.0_r48` 源码树中都**不存在**；它们是为了说明一次真实接入需要新增哪些文件，不能当成
可直接打开的源码入口：

```text
frameworks/base/core/java/android/content/Context.java
frameworks/base/core/java/android/app/SystemServiceRegistry.java
frameworks/base/core/java/android/os/IMowerDiagnosticsService.aidl       # 教学拟新增；当前树不存在
frameworks/base/core/java/android/os/IMowerDiagnosticsCallback.aidl      # 教学拟新增；当前树不存在
frameworks/base/core/java/android/os/MowerDiagnosticsManager.java         # 教学拟新增；当前树不存在
frameworks/base/services/core/java/com/android/server/mower/              # 教学拟新增目录；当前树不存在
    MowerDiagnosticsService.java                                          # 教学拟新增
    MowerDiagnosticsInternal.java                                         # 教学拟新增
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/core/res/AndroidManifest.xml
system/sepolicy/private/service_contexts
system/sepolicy/private/service.te
system/sepolicy/private/system_server.te
system/sepolicy/public/ 或 private/ 中的客户端策略
frameworks/base/services/tests/servicestests/src/                        # 可放服务端单元测试
```

还可能修改 `Android.bp`、API signature 文件、stats Atom 定义。具体位置取决于接口是公开 API、`@SystemApi`、`@hide`，还是仅设备私有代码。

---

## 5. 三条注册链必须分开

| 注册 | 注册什么 | 谁能取到 | 是否 IPC |
|---|---|---|---|
| `publishBinderService()` | 名字 → Binder 对象 | 获得 `find` 权限的进程 | 是 |
| `SystemServiceRegistry.registerService()` | Context 名字/类型 → Manager 工厂 | App/Framework Java 调用方 | 工厂本身不是 IPC |
| `publishLocalService()` | Java Class → 普通对象 | 同一 `system_server` 进程 | 否 |

只发布 Binder，`ServiceManager.getService()` 可以找到，但 `Context.getSystemService()` 不会凭空认识 Manager。

只注册 Manager，而服务端没发布 Binder，Manager 构造时会找不到远端服务。

`LocalServices` 不是“更快的 Binder”。它是进程内普通 Java 调用，不切线程，也没有 Binder UID 身份和 Parcel 隔离。

---

## 6. 用真实 `SystemConfigService` 校准最小服务形态

真实文件：

```text
frameworks/base/services/java/com/android/server/SystemConfigService.java
```

核心结构可简化为：

```java
public class SystemConfigService extends SystemService {
    private final ISystemConfig.Stub mInterface = new ISystemConfig.Stub() {
        @Override
        public List<String> query() {
            mContext.enforceCallingOrSelfPermission(...);
            return ...;
        }
    };

    @Override
    public void onStart() {
        publishBinderService(Context.SYSTEM_CONFIG_SERVICE, mInterface);
    }
}
```

真实代码说明了三个基本事实：服务继承 `SystemService`；Binder Stub 可以作为成员；`onStart()` 发布服务。复杂服务只是在这副骨架上增加线程、状态和生命周期。

---

## 7. AIDL 先设计语义，不急着设计字段

教学接口：

```aidl
interface IMowerDiagnosticsService {
    long startDiagnosis(in DiagnosisRequest request,
            IMowerDiagnosticsCallback callback);
    DiagnosisStatus getStatus(long taskId);
    void cancel(long taskId);
}

oneway interface IMowerDiagnosticsCallback {
    void onProgress(long taskId, int progress);
    void onFinished(long taskId, in DiagnosisResult result);
}
```

设计理由：

- `startDiagnosis()` 只受理，不等待诊断完成。
- 返回 `taskId`，使任务可查询、去重、取消和恢复。
- `getStatus()` 是 callback 丢失后的补偿通道。
- callback 使用 `oneway`，避免服务因客户端回调处理慢而同步等待。
- `DiagnosisRequest` 和 `DiagnosisResult` 应限制大小，不传日志大文件或大图片。

`oneway` 只表示调用者不等 reply，不表示无限并发、绝不排队或可靠送达。

---

## 8. 为什么不能只提供 callback

callback 会因下列原因丢失：

```text
客户端进程死亡
Binder 引用失效
客户端重新启动但没有旧 callback
oneway 队列拥塞
system_server 重启
版本差异导致无法解释某事件
```

因此可靠设计通常是：

```text
callback = 及时通知
query API = 当前权威状态
持久化结果 = 跨进程/跨重启恢复依据
```

通知不是事实本身。任务状态才是事实。

---

## 9. 数据对象要有演进策略

即使不是 stable AIDL，也应预留兼容思路：

```java
public final class DiagnosisRequest implements Parcelable {
    int version;
    int flags;
    String component;
    PersistableBundle options;
}
```

注意：任意 `Bundle` 看似灵活，却容易形成无文档的私有协议。字段需要写明默认值、上限、未知值处理和敏感性。

如果接口跨 system/vendor 分区或要求独立升级，应评估 stable AIDL 的冻结版本/hash 和 VINTF 规则；普通 platform 内部 AIDL 不应被误称为稳定跨分区 ABI。

---

## 10. Manager 是面向调用者的稳定门面

教学代码：

```java
@SystemService(Context.MOWER_DIAGNOSTICS_SERVICE)
public final class MowerDiagnosticsManager {
    private final Context mContext;
    private final IMowerDiagnosticsService mService;

    public MowerDiagnosticsManager(Context context,
            IMowerDiagnosticsService service) {
        mContext = context;
        mService = service;
    }

    public long startDiagnosis(DiagnosisRequest request, Executor executor,
            Callback callback) {
        // 参数预检、callback 到 Executor 的线程转换、RemoteException 映射
        return ...;
    }
}
```

Manager 的职责包括：

- 提供类型安全 API。
- 把 Binder callback 转发到调用者指定 `Executor`。
- 将 `RemoteException` 转成平台约定的异常或失败结果。
- 做便利性参数校验，但不能代替服务端安全校验。
- 隐藏 AIDL 和重连细节。

客户端检查永远不是安全边界，因为恶意调用方可以绕过 Manager 直接 transact。

---

## 11. 注册到 `SystemServiceRegistry`

真实注册模式位于：

```text
frameworks/base/core/java/android/app/SystemServiceRegistry.java
```

教学代码：

```java
registerService(Context.MOWER_DIAGNOSTICS_SERVICE,
        MowerDiagnosticsManager.class,
        new CachedServiceFetcher<MowerDiagnosticsManager>() {
    @Override
    public MowerDiagnosticsManager createService(ContextImpl ctx)
            throws ServiceNotFoundException {
        IBinder binder = ServiceManager.getServiceOrThrow(
                Context.MOWER_DIAGNOSTICS_SERVICE);
        IMowerDiagnosticsService service =
                IMowerDiagnosticsService.Stub.asInterface(binder);
        return new MowerDiagnosticsManager(ctx.getOuterContext(), service);
    }
});
```

`CachedServiceFetcher` 的缓存是 `ContextImpl` 相关的 Manager 缓存，不是服务端业务状态缓存，也不是 ServiceManager 全局缓存。

必选服务可用 `getServiceOrThrow()`；设备可选功能应考虑 `getService()` 返回 null 的契约。不能随意混用。

---

## 12. Context 常量与 API 面

教学常量：

```java
public static final String MOWER_DIAGNOSTICS_SERVICE = "mower_diagnostics";
```

同一个字符串需要在发布、查找、`service_contexts` 中一致。但“加一个 public 常量”不只是改 Java 文件：公开 Android API 通常涉及 API council、注解、文档、兼容性和 signature 文件更新。

设备内部功能更可能保持 `@hide`，或放在厂商自身 API 层。是否公开由产品和兼容性需求决定，不由代码方便程度决定。

---

## 13. 服务端生命周期骨架

```java
public final class MowerDiagnosticsService extends SystemService {
    private HandlerThread mWorkerThread;
    private Handler mHandler;
    private final BinderService mBinderService = new BinderService();
    private final LocalService mLocalService = new LocalService();

    public MowerDiagnosticsService(Context context) {
        super(context);
    }

    @Override
    public void onStart() {
        mWorkerThread = new HandlerThread("MowerDiagnostics");
        mWorkerThread.start();
        mHandler = new Handler(mWorkerThread.getLooper());
        publishBinderService(Context.MOWER_DIAGNOSTICS_SERVICE,
                mBinderService, false);
        publishLocalService(MowerDiagnosticsInternal.class, mLocalService);
    }
}
```

`allowIsolated=false` 表示 isolated 进程不能按普通路径取得服务，但这不是完整授权；仍需权限、UID/user 和业务校验。

生产代码还要考虑 worker 创建失败、测试注入、线程优先级和启动阶段是否真的需要立即建线程。

---

## 14. `onStart()` 返回不等于服务完全 ready

至少区分：

```text
对象已构造
onStart 已调用
Binder 已发布
Binder 可查找
依赖服务已 ready
数据已加载
当前用户已启动
当前用户 CE 已解锁
外部 daemon/HAL 已连接
```

所以接口需要定义“不 ready”时行为：快速返回明确错误、排队、降级，还是暂不可见。最危险的是无界等待。

---

## 15. 放入 SystemServer 的位置由依赖决定

真实入口：

```text
frameworks/base/services/java/com/android/server/SystemServer.java
```

教学启动代码可能是：

```java
t.traceBegin("StartMowerDiagnosticsService");
mSystemServiceManager.startService(MowerDiagnosticsService.class);
t.traceEnd();
```

不是“越早越好”。要先回答：

- 构造和 `onStart()` 用到哪些服务？
- 首次可接收请求需要哪个 boot phase？
- 是否依赖 PackageManager、用户、存储、网络或 HAL？
- 服务启动失败是否应拖垮整个 system_server？

启动顺序是依赖图的线性展开，不是文件中的随意排列。

---

## 16. BootPhase 只做该阶段必须做的事

```java
@Override
public void onBootPhase(int phase) {
    if (phase == PHASE_SYSTEM_SERVICES_READY) {
        mHandler.post(this::connectDependencies);
    } else if (phase == PHASE_BOOT_COMPLETED) {
        mHandler.post(this::scheduleDeferredMaintenance);
    }
}
```

`SystemServiceManager` 在主线程串行分发 phase。回调里做慢 I/O 会直接增加关键路径时延。

`post()` 后 phase 回调很快结束，但异步任务并未完成。如果其他服务必须依赖该结果，需要显式 readiness 状态或 Future/回调，不能把“已 post”当作“已完成”。

---

## 17. Binder 入口的黄金结构

```java
private final class BinderService extends IMowerDiagnosticsService.Stub {
    @Override
    public long startDiagnosis(DiagnosisRequest request,
            IMowerDiagnosticsCallback callback) {
        enforceManagePermission();
        Objects.requireNonNull(request);
        validateRequest(request);

        final int callingUid = Binder.getCallingUid();
        final int callingUserId = UserHandle.getUserId(callingUid);
        final DiagnosisRequest safeCopy = request.deepCopy();
        final long taskId = nextTaskId();

        final long token = Binder.clearCallingIdentity();
        try {
            mHandler.post(() -> handleStart(taskId, callingUid,
                    callingUserId, safeCopy, callback));
        } finally {
            Binder.restoreCallingIdentity(token);
        }
        return taskId;
    }
}
```

顺序要点：

1. 仍处于调用者身份时做权限与归属检查。
2. 捕获 UID/userId，复制输入。
3. 需要代表系统访问其他服务时才清除身份。
4. 把慢工作交给明确的 owner 线程。
5. 快速返回。

不要在 `clearCallingIdentity()` 后再询问“调用者是谁”。那时看到的已是服务进程身份。

---

## 18. 为什么必须复制请求

AIDL Parcelable 到达服务端通常已经反序列化，但其中可能含可变集合、FD、Binder token 或由服务继续持有的引用。

跨线程前应：

- 校验字段和集合长度。
- 转为服务自己的不可变模型。
- 明确 FD 所有权和关闭时机。
- 不把可变对象同时交给多个线程。
- 不信任 callback 中声明的 package/user。

“已经过 Parcel”不等于“业务上可信且适合长期持有”。

---

## 19. 权限检查要分层

```text
Framework permission：能不能调用这一类能力
AppOps：某些可审计/可动态控制操作是否允许
UID/package 对应关系：调用者是否真的拥有所声明包名
user/profile 规则：能操作哪个用户的数据
对象归属：taskId 是否属于这个调用者
SELinux Binder/service 权限：进程域能否 find/call
```

这些层不是互相替代。SELinux 允许 Binder `call`，不代表业务 API 自动授权；Java permission 通过，也不代表能跨用户读取任意 task。

签名权限示意：

```xml
<permission android:name="android.permission.MANAGE_MOWER_DIAGNOSTICS"
    android:protectionLevel="signature" />
```

真实命名、公开范围和声明位置需要平台 API/权限审核。

---

## 20. 跨用户不是只看 `userId`

客户端传入 `userId=0` 不构成授权。服务端应从 `Binder.getCallingUid()` 推导调用用户，再按需要调用标准跨用户权限检查。

任务记录至少保存：

```text
ownerUid
ownerUserId
taskId
createdElapsedRealtime
state
generation
```

查询和取消时重新比较 owner。若允许 device owner、profile owner 或 system UID 越权管理，应把例外写成明确策略，不要散落 `uid == SYSTEM_UID` 判断。

---

## 21. Handler 作为单一状态 owner

```text
Binder线程1 ─┐
Binder线程2 ─┼─ post Command ─→ MowerDiagnostics Handler
BootPhase  ──┤                         │
User事件    ──┘                         ├─ 唯一修改 UserState
daemon回调 ───────── post Event ───────┘
```

优点：

- 大多数业务状态无需多把锁。
- 顺序更容易推理和复现。
- dumpsys 可以通过 snapshot 获取一致视图。
- 防止 Binder 线程池被慢诊断占满。

但 Handler 并非魔法：阻塞它会让所有业务事件排队。真正重 CPU/I/O 工作还应交给受限 executor，再把结果 post 回 owner。

---

## 22. 状态机比布尔变量可靠

```text
IDLE
  └─ start → QUEUED
QUEUED
  ├─ dispatch → RUNNING
  └─ cancel   → CANCELLED
RUNNING
  ├─ success  → SUCCEEDED
  ├─ error    → FAILED
  ├─ cancel   → CANCELLING → CANCELLED
  └─ backend death → RETRY_WAIT / FAILED
```

每条边写清：触发事件、允许前态、状态写入、持久化点、callback、统计和超时。

两个布尔量 `running`、`cancelled` 能组合出矛盾状态；枚举状态机可以禁止非法转换。

---

## 23. generation 防止旧回调污染新连接

外部 daemon 重连示例：

```java
private int mBackendGeneration;

private void connectBackend() {
    final int generation = ++mBackendGeneration;
    backend.connect(result -> mHandler.post(() -> {
        if (generation != mBackendGeneration) return;
        handleBackendResult(result);
    }));
}
```

旧连接在超时后仍可能送达结果。只检查 taskId 有时不够，因为 task 可能重试。`generation` 把事件绑定到特定连接世代。

---

## 24. callback 生命周期

`RemoteCallbackList` 适合维护跨进程 callback：

```java
private final RemoteCallbackList<IMowerDiagnosticsCallback> mCallbacks =
        new RemoteCallbackList<>();
```

它利用 Binder death 清理死亡客户端，但仍需设计：

- callback 是按用户、按任务还是全局注册？
- 重复注册如何处理？
- `beginBroadcast()`/`finishBroadcast()` 必须配对。
- 回调失败不能破坏主状态机。
- callback 中不能携带敏感的其他用户状态。
- 服务销毁/测试清理时是否 `kill()`。

一次 `RemoteException` 是症状，不应让任务状态回滚。

---

## 25. 不要持锁做外部调用

危险模式：

```java
synchronized (mLock) {
    callback.onFinished(...);       // 跨进程
    packageManager.someCall(...);   // 可能 Binder
}
```

对方可能反向调用、阻塞或等待另一把锁，形成死锁链。

安全模式：锁内只复制必要快照，释放锁后 IPC。若使用单 owner Handler，仍要避免 Handler 内同步调用不可控远端服务；至少设置清楚的失败和超时边界。

---

## 26. 同步、oneway 与 Handler 是三段队列

一次 callback 可能经过：

```text
服务 Handler 等待
 → Binder oneway 发送
 → 目标进程 Binder 线程接收
 → Manager post 到 App Executor
 → App callback 执行
```

所以“服务已经调用 callback”不等于“App 已处理”。可观测性应分别记录状态提交时间和通知尝试时间，不能用 callback 返回代表端到端完成。

---

## 27. Binder 载荷要有硬上限

不推荐：

```aidl
byte[] getFullDiagnosticArchive(long taskId);
List<LogLine> getAllLogs(long taskId);
```

更合理：

- 小型状态直接 Parcelable。
- 列表分页，并限制 page size。
- 大结果写受控文件，通过只读 FD/URI 流式返回。
- 服务端验证调用者并限制并发 FD 数。
- 返回摘要、hash、长度，调用者自行流式读取。

Binder buffer 是进程共享的有限资源；失败可能来自同时在途事务总量，并非单次对象一定超过某个固定 Java 常量。

---

## 28. per-user 内存模型

```java
private final SparseArray<UserState> mUserStates = new SparseArray<>();

private static final class UserState {
    final int userId;
    final LongSparseArray<TaskRecord> tasks = new LongSparseArray<>();
    boolean unlocked;
}
```

所有访问都在 owner Handler 上，就不必给 `mUserStates` 和 `tasks` 分别加锁。

但 `dumpsys` 在 Binder 线程执行。可选择：

1. post 一个 snapshot 请求并有限时等待；或
2. owner 每次状态变化后更新 immutable/volatile snapshot。

不要让 dumpsys 无限等待已经卡住的 Handler。

---

## 29. 用户生命周期与 DE/CE

```java
@Override
public void onUserStarting(TargetUser user) {
    postUserEvent(STARTING, user.getUserIdentifier());
}

@Override
public void onUserUnlocking(TargetUser user) {
    postUserEvent(UNLOCKING, user.getUserIdentifier());
}

@Override
public void onUserStopping(TargetUser user) {
    postUserEvent(STOPPING, user.getUserIdentifier());
}
```

设计原则：

- 解锁前必需的数据放 Device Encrypted（DE）存储。
- 敏感且仅解锁后需要的数据放 Credential Encrypted（CE）存储。
- `PHASE_BOOT_COMPLETED` 是全局服务阶段，不等于每个用户已解锁。
- 用户停止时取消资源、移除 callback、关闭 FD，是否保留最终结果由产品策略决定。

---

## 30. 持久化只保存恢复真正需要的事实

建议保存：

```text
最后一次最终状态
任务创建/完成 wall time 与 elapsed time（注明时钟）
结果摘要和错误码
schema version
backend generation 或恢复标记
```

不要保存活 Binder callback、线程对象或正在运行的 Future。

`AtomicFile` 能改善“写一半文件损坏”，但不能自动解决 schema 兼容、跨文件事务和错误数据语义。

---

## 31. 写盘策略

每个进度百分比都同步写盘会放大 I/O。可采用：

```text
QUEUED/RUNNING：内存权威，必要时节流 checkpoint
SUCCEEDED/FAILED/CANCELLED：提交最终状态后原子写盘
system_server 退出：不能依赖总有优雅清理机会
```

真正重要的恢复语义必须在状态转换时提交，不能只等 `shutdown()`。

敏感原始日志最好由专门受控存储管理，摘要文件不应成为新的隐私泄漏点。

---

## 32. system_server 重启后的恢复

启动加载流程：

```text
读取 schema
 → 校验 owner/user/task 字段
 → 最终态直接恢复
 → 上次 RUNNING 不能假装仍在运行
 → 标记 INTERRUPTED 或依据 backend token 查询
 → 决定安全重试还是明确失败
 → 更新 generation
```

是否自动重试取决于操作是否幂等。若重复执行可能损伤设备或重复收费，必须使用幂等 token/后端去重，或要求人工重试。

---

## 33. 外部服务死亡恢复

```text
Binder deathRecipient
  → 只做极少工作
  → post BACKEND_DIED(generation)
  → owner 校验世代
  → 更新任务状态
  → 指数退避重连
  → 重连后重新查询权威状态
```

不要在 `binderDied()` 中长时间同步重连。死亡回调所在线程不是业务 owner。

退避应有上限和抖动，避免多个服务同时重启形成惊群。

---

## 34. 幂等与重复请求

客户端可能因为超时而重试，但第一次请求其实已被受理。解决方式之一：

```text
clientRequestId + ownerUid + userId
```

服务在一定窗口内记录这个键，并返回同一 taskId。不要用随机 taskId 本身替代调用者幂等键。

需要明确：去重窗口、持久化范围、相同 key 不同参数时的错误、用户删除后的清理。

---

## 35. 错误模型要能行动

比 `boolean success` 更有用：

```text
ERROR_PERMISSION_DENIED      # 通常直接抛 SecurityException
ERROR_NOT_READY              # 可稍后重试
ERROR_BUSY                   # 有 retryAfterMillis
ERROR_INVALID_ARGUMENT       # 修改请求
ERROR_BACKEND_DIED           # 服务将恢复或已终止
ERROR_TIMEOUT                # 结果未知，先 query
ERROR_CANCELLED              # 明确终态
ERROR_INTERNAL               # 带稳定子码，不暴露敏感堆栈
```

特别注意：同步调用超时不一定表示远端操作没执行。客户端应凭幂等键或 taskId 查询。

---

## 36. `RemoteException` 的 Manager 处理

平台内部 Manager 常见模式是 `e.rethrowFromSystemServer()`，部分查询 API 会降级为空集合。选择取决于契约：

- 返回空集合是否会把“服务死亡”伪装成“确实没有数据”？
- 调用方能否安全重试？
- 这是命令还是只读查询？
- API 是否已经有向后兼容约定？

本服务的 `startDiagnosis()` 不应在服务死亡时返回假 taskId；`getStatus()` 可以返回明确 unavailable 状态或抛契约化异常。

---

## 37. ServiceManager 名字的 SELinux 标签

概念示意：

```text
# service_contexts
mower_diagnostics    u:object_r:mower_diagnostics_service:s0

# service.te
type mower_diagnostics_service, system_api_service, service_manager_type;
```

标签将服务名映射为 SELinux type。然后分别控制：

```text
system_server 是否可 add
客户端 domain 是否可 find
客户端是否可对服务 Binder call
服务回调客户端是否需要反向 Binder 权限
```

具体宏和 type attribute 必须结合当前树规则及 neverallow 审核，不能照抄示意策略。

---

## 38. `find`、`call` 和业务权限不是一回事

```text
find：从 servicemanager 获得 handle
call：Binder IPC 是否被 SELinux 允许
permission：Framework 服务方法内业务授权
```

典型排错顺序：

1. `service list` 是否存在名字？
2. logcat 是否有 `avc: denied { find }`？
3. transact 是否出现 `{ call }` 拒绝？
4. Java 是否抛 `SecurityException`？
5. user/task ownership 是否拒绝？

不要看到“Permission denied”就只加 Java permission 或只加 allow 规则。

---

## 39. 最小 SELinux 权限原则

不建议：让所有 appdomain 查找和调用，或把服务标成过于宽泛的 attribute。

建议先列访问矩阵：

| 主体 | find | call | callback | 数据文件 |
|---|---:|---:|---:|---:|
| system_server | add/自身对象 | 内部按需 | 是 | 读写 |
| 特权系统 App | 是 | 是 | 被回调 | 否 |
| 普通 App | 否 | 否 | 否 | 否 |
| isolated App | 否 | 否 | 否 | 否 |
| vendor daemon | 按架构决定 | 按架构决定 | 按需 | 独立最小授权 |

先有矩阵，再写 policy。

---

## 40. LocalService 的用途

```java
public abstract class MowerDiagnosticsInternal {
    public abstract Snapshot getSnapshotForSystemServer(int userId);
}
```

其他 system_server 服务：

```java
MowerDiagnosticsInternal service =
        LocalServices.getService(MowerDiagnosticsInternal.class);
```

它适合可信进程内协作，避免把内部能力暴露为公共 Binder API。但调用发生在调用者当前线程，若实现会阻塞，仍要定义异步接口或线程约束。

LocalService 不能依赖 Binder callingUid 做鉴权；调用双方同在 system_server，应以内部 API 最小化和代码所有权保障边界。

---

## 41. dumpsys 是第一现场

教学输出：

```text
MowerDiagnosticsService:
  ready=true bootPhase=1000 backend=CONNECTED generation=4
  queueDepth=1 oldestQueueAgeMs=32
  user 0: unlocked=true tasks=3 running=1
    #1042 RUNNING ageMs=821 progress=40 ownerUid=10032
  recentFailures:
    BACKEND_DIED count=2 lastElapsedMs=...
  latencyMs: accept p50=1 p95=3; run p50=420 p95=1900
```

应避免输出：原始位置、认证 token、完整文件路径、设备唯一标识和任意用户的敏感内容。

`dump()` 自身要做 `DUMP` 权限检查，并支持 `--user`、`--task`、`--proto` 等有界参数。

---

## 42. dumpsys 不能制造第二次卡死

若 owner Handler 已阻塞，Binder dump 再无限等待它，只会让取证工具也挂住。

推荐：

```text
Binder dump线程
 → 检查 DUMP 权限
 → 请求 snapshot
 → 最多等待例如 2 秒
 → 超时则打印 lastKnownSnapshot + "handler unresponsive"
```

这比只打印“timeout”更有诊断价值。

---

## 43. 启动可观测性

SystemServer 启动处：

```java
t.traceBegin("StartMowerDiagnosticsService");
mSystemServiceManager.startService(MowerDiagnosticsService.class);
t.traceEnd();
```

服务内部还可划分：

```text
MowerDiag#LoadDeState
MowerDiag#ConnectBackend
MowerDiag#RegisterObservers
```

不要为了图好看把异步任务 slice 画成同步完成。跨线程异步 trace 需要可关联 cookie/taskId；同步 `traceBegin/End` 只能描述当前线程区间。

---

## 44. 运行期指标

建议最少包含：

| 指标 | 维度 | 用途 |
|---|---|---|
| accepted count | result/调用方类别 | 入口趋势 |
| queue delay | bucket | 是否 owner 线程拥塞 |
| execution latency | operation/result | 后端是否变慢 |
| backend death | backend/version | 稳定性 |
| recovery latency | result | 自愈效果 |
| active tasks | user 类别，不放真实 userId | 容量 |
| payload size | bucket | Binder 风险 |

避免把 UID、taskId、原始错误文本作为高基数字段写入 statsd。

---

## 45. EventLog、statsd、trace 和 dumpsys 各管什么

```text
trace：一次问题的线程时间线和等待链
EventLog/logcat：离散事件与工程日志
statsd Atom：跨设备/跨时间的结构化聚合
dumpsys：当前状态和有限历史快照
持久化状态：业务恢复事实
```

不能用 statsd 聚合报告恢复某个 task，也不能用大量 logcat 代替结构化指标。

---

## 46. 日志隐私与限流

```java
Slog.i(TAG, "task accepted id=" + taskId + " uid=" + callingUid);
```

即便是系统日志，也要评估 UID/taskId 是否需要散列或省略。循环故障要限流，避免每次重试打印完整堆栈导致 log buffer 被冲掉。

错误码稳定，错误文本服务于人；不要让自动化依赖易变化的英文日志字符串。

---

## 47. shell 命令是受控诊断面

可设计：

```text
cmd mower_diagnostics status --user current
cmd mower_diagnostics run-test --component motor
cmd mower_diagnostics reset-test-state
```

其中会改变状态的命令必须限制 shell/root、debuggable build 或专用权限。测试后门不能默认向生产 App 开放。

shell command 也走 Binder 线程，真正工作仍应 post，并设置明确超时。

---

## 48. 不要默认把服务做成 lazy

SystemServer 管理的 `SystemService` 通常随 system_server 启动，并不因为没有客户端就由 servicemanager 自动销毁。

lazy AIDL 服务常用于独立 native 进程，涉及 init `interface aidl`、servicemanager client callback 和进程启停。把两种生命周期混在一起，会造成“为什么服务不会被停掉”的误判。

本教学服务保持 system_server 内常驻，只让昂贵后端连接按需建立。

---

## 49. 依赖 native daemon/HAL 时的边界

```text
Framework Binder API：App/系统组件调用入口
system_server 状态机：权限、用户、策略、恢复
HAL/daemon API：硬件能力与底层错误
kernel driver：真实设备控制/事件
```

不要原样把 ioctl/HAL error 暴露给 App。Framework 应映射成稳定错误模型，并保留内部诊断子码。

如果跨 system/vendor，必须同时考虑接口稳定性、VINTF、Binder 世界、SELinux 和升级组合。

---

## 50. 完整请求时序

```text
App        Manager       BinderService       OwnerHandler       Backend
 | start()    |                |                   |                |
 |----------->| transact       |                   |                |
 |            |--------------->| permission       |                |
 |            |                | validate/copy     |                |
 |            |                | post START ------>|                |
 |            |<-- taskId -----|                   |                |
 |<-- taskId--|                |                   | start -------->|
 |            |                |                   |<-- progress ---|
 |            |<-- oneway callback ---------------|                |
 |<--Executor callback----------|                   |                |
 | query() -------------------->|------post/query snapshot---------->|
 |<---------------- status ----|                   |                |
```

图中 `taskId` 返回时，任务可能仍是 QUEUED。API 文档必须写清楚。

---

## 51. 后端死亡与恢复时序

```text
Backend      DeathRecipient       OwnerHandler       Client
   X               |                   |                |
   |-- binderDied ->|                   |                |
   |                |-- post(gen=7) --->|                |
   |                |                   | mark RETRY_WAIT|
   |                |                   | persist        |
   |                |                   | callback ------>|
   |                |                   | backoff         |
 new backend        |                   | connect(gen=8)  |
   |<-----------------------------------|                |
   |-- snapshot/result ---------------->| reconcile       |
```

恢复不是“重新 getService 就结束”，还必须把本地状态与后端权威状态对账。

---

## 52. 启动与用户解锁时序

```text
SystemServer
  → startService
  → onStart：线程、Binder、LocalService
  → PHASE_SYSTEM_SERVICES_READY：连接必要依赖
  → user 0 STARTING：加载 DE 摘要
  → user 0 UNLOCKING：允许读取 CE 详情
  → PHASE_BOOT_COMPLETED：安排非关键维护
  → user 10 STARTING/UNLOCKING：独立 UserState
```

全局 boot phase 与每用户生命周期是两条轴。不要用一个 `mBootCompleted` 布尔量代替所有 readiness。

---

## 53. 单元测试切分

尽量把业务状态机从 Android 组件中拆出：

```text
MowerDiagnosticsService：平台生命周期和 Binder 适配
DiagnosticsController：纯状态机
Backend：可替换接口
StateStore：可替换持久层
Clock：可注入时钟
Metrics：可替换记录器
```

这样在 macOS 上即使不编译整套 AOSP，也能通过阅读测试设计验证依赖是否清晰。

关键单测：合法转换、重复请求、取消竞态、旧 generation 回调、超时、写盘失败和恢复损坏文件。

---

## 54. Binder/权限测试矩阵

| 场景 | 预期 |
|---|---|
| 无签名权限调用 | `SecurityException` |
| 有权限同用户调用 | 受理并返回 taskId |
| 伪造 packageName | 拒绝 |
| 查询其他 UID task | 拒绝或脱敏 |
| 跨用户无权限 | 拒绝 |
| isolated UID | 无法获取/调用 |
| 超大 Parcelable | 入口校验拒绝，不拖垮进程 |
| callback 进程死亡 | 自动清理，任务继续 |
| 并发重复 requestId | 返回同一任务或稳定冲突 |

测试不仅看返回值，也看任务表、callback 数量、指标和无资源泄漏。

---

## 55. 生命周期与恢复测试矩阵

| 注入点 | 要验证 |
|---|---|
| `onStart()` 前依赖不可用 | 不无界等待，状态可解释 |
| DE 文件损坏 | 隔离坏数据，安全默认值，有指标 |
| 用户未解锁 | 不读取 CE，API 返回明确状态 |
| 用户停止 | 释放 per-user 资源 |
| backend 运行中死亡 | generation 生效，任务可恢复/明确失败 |
| system_server 重启 | RUNNING 不被误报为仍运行 |
| 写最终态时掉电 | AtomicFile 恢复到旧或新完整版本 |
| owner Handler 卡住 | dumpsys 有限时返回 last snapshot |

---

## 56. 性能测试矩阵

```text
冷启动：服务 onStart 自身耗时
首次请求：class load/连接后端成本
稳态请求：Binder + queue + backend 各段
并发请求：Binder pool 和 Handler queue 是否饥饿
慢客户端 callback：oneway 队列与 RemoteCallbackList 行为
大结果：分页/FD 流是否有背压
长时间运行：任务表、callback、FD、线程是否泄漏
```

只报告平均值会掩盖长尾，应至少观察 p50/p95/p99、最大值和失败数，并注明样本和时钟。

---

## 57. 故障注入优先于只测成功路径

值得主动模拟：

- callback 在进度 50% 时死亡。
- backend 在命令已执行但 reply 未返回时死亡。
- cancel 与 success 同时到达。
- 同一 clientRequestId 参数不同。
- 磁盘满、文件损坏、schema 过新。
- 用户在任务运行中停止。
- Handler 队列被一个慢任务占住。
- 旧 generation 在重连后返回 success。

每个场景都要回答最终权威状态是什么，而不仅是“有没有 crash”。

---

## 58. macOS 只读练习一：追真实最小闭环

```bash
cd /Users/ninebot/androidSource

sed -n '1,180p' \
  frameworks/base/services/java/com/android/server/SystemConfigService.java

sed -n '1,180p' \
  frameworks/base/core/java/android/os/SystemConfigManager.java

rg -n "SYSTEM_CONFIG_SERVICE|SystemConfigManager" \
  frameworks/base/core/java/android/app/SystemServiceRegistry.java \
  frameworks/base/core/java/android/content/Context.java \
  frameworks/base/services/java/com/android/server/SystemServer.java
```

请手画：Context 常量 → Registry → Manager → AIDL → Service → SystemServer。某些服务的 Manager 会直接查 ServiceManager，某些由 Registry 注入 Binder，模式不必完全相同。

---

## 59. macOS 只读练习二：追发布方法

```bash
cd /Users/ninebot/androidSource

sed -n '450,525p' \
  frameworks/base/services/core/java/com/android/server/SystemService.java

sed -n '1,150p' \
  frameworks/base/core/java/com/android/server/LocalServices.java

rg -n "addService\(|getServiceOrThrow|checkService\(" \
  frameworks/base/core/java/android/os/ServiceManager.java
```

回答：哪条路径跨进程？谁负责名字映射？LocalService 调用在哪条线程执行？

---

## 60. macOS 只读练习三：追用户生命周期

```bash
cd /Users/ninebot/androidSource

sed -n '250,365p' \
  frameworks/base/services/core/java/com/android/server/SystemService.java

rg -n "onUserStarting|onUserUnlocking|onUserStopping" \
  frameworks/base/services/core/java/com/android/server | head -80
```

选一个真实 per-user 服务，记录它在 starting、unlocking、stopping 分别初始化和释放什么。不要仅凭方法名推测，要跟进实现。

---

## 61. macOS 只读练习四：追权限与 dump

```bash
cd /Users/ninebot/androidSource

rg -n "enforceCallingOrSelfPermission|enforceCallingPermission" \
  frameworks/base/services/core/java/com/android/server | head -80

rg -n "DumpUtils.checkDumpPermission|Manifest.permission.DUMP" \
  frameworks/base/services/core/java/com/android/server | head -80

rg -n "service_manager_type|add_service|allow.*find" \
  system/sepolicy/public system/sepolicy/private | head -100
```

练习把一次拒绝定位到 SELinux find/call、Framework permission、跨用户或对象归属中的一层。

---

## 62. 代码评审清单：API

- [ ] 接口是 public、SystemApi、hide 还是设备私有，理由明确。
- [ ] 同步方法保证快速、有界。
- [ ] 长任务有 taskId、query、cancel 和幂等语义。
- [ ] callback 丢失不影响事实恢复。
- [ ] Parcelable 大小、集合数量和版本有上限。
- [ ] 错误码稳定且可行动。
- [ ] Manager callback 线程有明确 Executor。
- [ ] 可选服务与必选服务的 null/throw 契约一致。

---

## 63. 代码评审清单：并发与恢复

- [ ] 可变状态有单一 owner 或清楚锁层级。
- [ ] 不在锁内或关键 Handler 上做不可控 IPC/I/O。
- [ ] Binder 身份在正确时机捕获、清除、恢复。
- [ ] callback/death 回调先 post 到 owner。
- [ ] generation 能淘汰旧事件。
- [ ] cancel/success/death 竞态有唯一终态。
- [ ] 重试有退避、上限、幂等依据。
- [ ] 持久化只恢复可验证事实。

---

## 64. 代码评审清单：安全与隐私

- [ ] 服务端权限检查不可绕过。
- [ ] UID/package/user/task 归属全部验证。
- [ ] `allowIsolated` 选择有依据。
- [ ] SELinux add/find/call 权限最小化。
- [ ] LocalService 没有误暴露危险内部能力。
- [ ] dumpsys/log/stats 不泄露敏感数据。
- [ ] shell 测试入口生产环境受控。
- [ ] 文件标签、目录权限、DE/CE 选择正确。

---

## 65. 代码评审清单：可观测性与性能

- [ ] SystemServer 启动 slice 命名清楚。
- [ ] queue delay 与 execution latency 分开。
- [ ] 状态提交与 callback 通知分开计时。
- [ ] dumpsys 卡死时能有限时降级。
- [ ] 指标避免高基数和敏感字段。
- [ ] payload 有硬上限，大数据走流式通道。
- [ ] Binder、Handler、executor 三类排队可区分。
- [ ] 启动关键路径不做可延后的慢工作。

---

## 66. 常见失败设计一：Binder Stub 直接做所有工作

后果：Binder 线程被硬件 I/O 占用；多个调用者耗尽线程池；Watchdog 可能只看到线程池不可用，而根因藏在某次诊断。

修正：Stub 只鉴权、校验、复制、入队；owner 管状态；受限 worker 执行重工作。

---

## 67. 常见失败设计二：认为 `oneway` 等于不阻塞

发送端不等待 reply，但事务仍需序列化、进入驱动队列、消耗 buffer，由目标进程取出。目标处理慢时，同一 Binder node 的异步事务会积压。

修正：合并高频 progress、限制速率、允许客户端查询最新快照，不逐条保证进度通知。

---

## 68. 常见失败设计三：只靠 Binder death 恢复

Binder death 只告诉你连接对象死了，不告诉你最后一个命令是否已经执行。

修正：命令带幂等 ID；后端提供状态查询；重连后 reconcile；不能确认时报告 UNKNOWN/INTERRUPTED，而不是武断重试。

---

## 69. 常见失败设计四：把 boot completed 当 user unlocked

设备可能完成全局启动，但次要用户未启动，主用户也可能仍处 Direct Boot 阶段。

修正：分别维护全局 phase、每用户 running/unlocked、后端连接和数据 loaded 状态。

---

## 70. 常见失败设计五：用一个大锁换“线程安全”

大锁可能把 Binder、dump、用户生命周期和后端回调串成不可预测锁链。

修正：状态归一到 owner Handler；跨线程只传 command/event；必须锁时规定层级，锁内不 IPC/I/O。

---

## 71. 第二遍复读：最容易混淆的十组概念

### 71.1 Binder 服务与 Manager

Binder 服务是跨进程端点；Manager 是客户端 Java 门面。发布一个不自动生成另一个。

### 71.2 Binder 服务与 LocalService

前者有 Parcel、线程切换、UID 和 SELinux；后者是同进程 Java 直调。

### 71.3 `onStart()` 与 ready

`onStart()` 返回只代表该回调完成，不代表用户已解锁、数据已加载或后端已连接。

### 71.4 BootPhase 与用户生命周期

前者是 system_server 全局阶段；后者按 userId 多次发生。

### 71.5 `oneway` 与异步业务

`oneway` 是 Binder reply 语义；业务异步还需要 taskId、状态机、查询和取消。

### 71.6 callback 与权威状态

callback 是可能丢失的通知；服务状态/持久记录才是事实。

### 71.7 Binder death 与操作失败

连接死亡不等于命令未执行。结果可能未知。

### 71.8 AtomicFile 与事务

AtomicFile 防止单文件半写，不提供多文件数据库事务或业务幂等。

### 71.9 SELinux 与 Framework permission

一个管域之间最低通信能力，一个管 API 业务授权；通常两者都要过。

### 71.10 Handler 串行与系统不会卡

Handler 降低共享状态复杂度，但一个慢消息仍会阻塞后续全部消息。

---

## 72. 第二遍修订：给初学者的“餐厅”类比

```text
ServiceManager 名字       = 餐厅门牌登记
SELinux find              = 是否允许看到/取得联系电话
Binder call               = 电话线路是否允许接通
Framework permission      = 是否持有会员/工作证
BinderService             = 前台接单员
Handler owner             = 后厨调度台
worker/backend            = 真正做菜的人和设备
taskId                    = 取餐号
callback                  = 叫号广播
query                     = 主动看取餐屏
persisted final state     = 已完成订单账本
LocalService              = 餐厅内部员工当面协作
```

叫号没听到，不代表菜没做好；电话接通，也不代表你有权下内部订单。这两个类比能同时解释 callback/状态和 SELinux/业务权限的边界。

---

## 73. 第二遍修订：一次请求到底在哪些线程

```text
App 调用线程
  → system_server 某个 Binder 线程：鉴权/校验/入队
  → 专用 HandlerThread：状态转换和调度
  → 受限 worker 或 daemon：慢工作
  → 专用 HandlerThread：提交结果
  → App 某个 Binder 线程：收到 oneway callback
  → App 指定 Executor：业务 callback
```

“服务运行在 system_server”没有回答执行线程。每个箭头都可能排队，排障时要逐段量测。

---

## 74. 第二遍修订：完成的六个层次

```text
请求已到 Binder Stub
请求已被 Handler 接收
后端命令已发出
后端工作已完成
服务状态已提交/持久化
客户端已经处理通知
```

API、日志和指标使用“完成”一词时必须指出是哪一层。本服务把 task 终态定义为“服务已验证后端结果并提交权威状态”；callback 处理不属于该事务的完成条件。

---

## 75. 最终落地顺序建议

若以后真做同类功能，按以下小步评审：

1. 写需求、威胁模型、调用方矩阵和错误语义。
2. 定 AIDL/Manager 契约与数据上限。
3. 实现纯状态机和 fake backend 测试。
4. 接 SystemService、Binder、LocalService 和用户生命周期。
5. 加持久化、死亡恢复与故障注入。
6. 加 permission、SELinux、文件标签和隐私审查。
7. 加 dumpsys、trace、stats 和性能门槛。
8. 最后决定 public/SystemApi/hidden API 表面并完成兼容性审核。

每一步都能单独审查，问题比一次提交几十个文件更容易定位。

---

## 76. 本章结论

新增一个可靠 Framework SystemService，真正困难的不是写出一个 AIDL Stub，而是把以下契约同时闭合：

```text
发现契约：Context / Registry / ServiceManager 名字一致
生命周期契约：SystemServer 顺序、BootPhase、per-user 状态明确
线程契约：Binder 快入口、owner 状态机、受限慢工作
安全契约：SELinux + permission + UID/user/object ownership
可靠性契约：taskId、query、幂等、death、generation、持久化
可观测契约：trace、metrics、dumpsys、隐私和有限时降级
兼容契约：API 面、Parcelable 演进、跨分区稳定性
```

当你能拿这七类契约去评审一个新服务，并沿真实源码验证每个注册点、线程和状态转换，就已经从“会追源码”进入“能设计和评审 Framework 服务”的阶段。

第 100 章是前一阶段的综合设计总结，不是当前学习路线的终点。第 101 章起将回到
Android 11 r48 中真实存在的系统服务和故障链路逐层精读；当前编号路线持续到第 200 章，正文以本地源码为准，
不会把本章的教学拟新增类冒充为 AOSP 现有实现。
