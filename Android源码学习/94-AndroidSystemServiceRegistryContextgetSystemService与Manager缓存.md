# 94 Android SystemServiceRegistry：Context.getSystemService、Manager 缓存与服务发布链路

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：从 `Context.getSystemService()` 追到 `SystemServiceRegistry`，分清服务名、Manager wrapper、AIDL 接口、Binder 服务端与 LocalService，并理解三种 Fetcher 的缓存范围和并发初始化。

---

## 1. 先回答：`getSystemService()` 得到的究竟是什么

应用常写：

```java
PowerManager pm = context.getSystemService(PowerManager.class);
```

返回的通常不是：

- `PowerManagerService` 服务端实例；
- AIDL 自动生成的 `IPowerManager.Proxy` 本身；
- servicemanager 中保存的裸 `IBinder`。

它通常是一个面向开发者的 **Manager wrapper（Java 管理器门面）**：

```text
PowerManager
 ├─ 保存 Context、Handler 等客户端环境
 ├─ 保存 IPowerManager 接口代理
 ├─ 把易用 Java API 转成 AIDL 调用
 └─ 可能维护客户端监听器、token、缓存或参数转换
```

先记住五层：

```text
Context API
  → SystemServiceRegistry
  → PowerManager（客户端 wrapper）
  → IPowerManager（AIDL Proxy）
  → PowerManagerService.BinderService（system_server）
```

上一章的 ServiceManager 只负责其中“按名字取得 Binder”的一小段。

---

## 2. 三个相似名字必须分开

| 名称 | 例子 | 本质 |
|---|---|---|
| service name | `Context.POWER_SERVICE`，值为 `"power"` | 跨进程目录键和 Framework API 键 |
| Manager class | `PowerManager.class` | 应用拿到的 Java 门面类型 |
| server implementation | `PowerManagerService` / 内部 `BinderService` | system_server 中真正执行系统逻辑的对象 |

名字可能相互关联，但不是同一个对象。

```text
"power" ── Registry 映射 ──> PowerManager.class
   │
   └──── ServiceManager 映射 ──> IPowerManager Binder
```

SystemServiceRegistry 管的是“Framework 如何构造 Manager wrapper”；ServiceManager 管的是“服务名对应哪个 Binder”。

---

## 3. 本章源码地图

```text
frameworks/base/core/java/android/content/Context.java
frameworks/base/core/java/android/content/ContextWrapper.java
frameworks/base/core/java/android/app/ContextImpl.java
frameworks/base/core/java/android/app/SystemServiceRegistry.java
frameworks/base/services/core/java/com/android/server/SystemService.java
frameworks/base/core/java/com/android/server/LocalServices.java
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
frameworks/base/core/java/android/os/PowerManager.java
```

模块化注册还可观察：

```text
frameworks/base/apex/jobscheduler/framework/java/android/app/job/JobSchedulerFrameworkInitializer.java
frameworks/base/telephony/java/android/telephony/TelephonyFrameworkInitializer.java
frameworks/base/wifi/java/android/net/wifi/WifiFrameworkInitializer.java
frameworks/base/apex/statsd/framework/java/android/os/StatsFrameworkInitializer.java
```

---

## 4. 两个公开入口最终汇合

旧式字符串入口：

```java
PowerManager pm = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
```

类型安全入口：

```java
PowerManager pm = context.getSystemService(PowerManager.class);
```

`Context` 中 class 版本并非直接按 class 创建对象：

```java
public final <T> T getSystemService(Class<T> serviceClass) {
    String serviceName = getSystemServiceName(serviceClass);
    return serviceName != null ? (T) getSystemService(serviceName) : null;
}
```

原因是 `getSystemService(String)` 可以被子类重写。因此真实步骤为：

```text
PowerManager.class
  → getSystemServiceName(class)
  → "power"
  → getSystemService("power")
```

class API 更安全，但底层仍落到字符串名字。

---

## 5. ContextWrapper 为什么没有自己的实现

Activity、Service 等常通过 `ContextThemeWrapper` 或其他 `ContextWrapper` 暴露 Context API。`ContextWrapper` 通常只是转发：

```java
public Object getSystemService(String name) {
    return mBase.getSystemService(name);
}
```

层层 unwrap 后，核心实现通常落到 `ContextImpl`。

```text
Activity / Service / ContextWrapper
                │ mBase
                ↓
             ContextImpl
```

但 Manager 构造时可能需要外层 Activity/Service，因此 `ContextImpl` 同时保存 `outerContext`。这解释了源码里为什么常见：

```java
ctx.getOuterContext()
```

base ContextImpl 负责底层实现，outer Context 保留组件身份、主题和显示环境。

---

## 6. ContextImpl 的入口非常短

Android 11 `ContextImpl`：

```java
@Override
public Object getSystemService(String name) {
    // 省略错误 Context 使用检查
    return SystemServiceRegistry.getSystemService(this, name);
}

@Override
public String getSystemServiceName(Class<?> serviceClass) {
    return SystemServiceRegistry.getSystemServiceName(serviceClass);
}
```

这说明 ContextImpl 不为每个系统服务写一大段 switch。统一映射和构造逻辑集中在 `SystemServiceRegistry`。

Android 11 还会检查从错误 Context 获取 UI/visual 服务的问题。例如 WindowManager、LayoutInflater 与 Context 的 display/configuration/主题强相关，不应随意从不合适的非视觉 Context 获取。

---

## 7. SystemServiceRegistry 的三张静态表

核心成员：

```java
Map<Class<?>, String> SYSTEM_SERVICE_NAMES;
Map<String, ServiceFetcher<?>> SYSTEM_SERVICE_FETCHERS;
Map<String, String> SYSTEM_SERVICE_CLASS_NAMES;
```

分别回答：

| 表 | 输入 → 输出 | 用途 |
|---|---|---|
| `SYSTEM_SERVICE_NAMES` | Manager class → service name | class API 转字符串 API |
| `SYSTEM_SERVICE_FETCHERS` | service name → ServiceFetcher | 创建或取得 Manager wrapper |
| `SYSTEM_SERVICE_CLASS_NAMES` | service name → 简单类名 | 诊断错误 Context 使用 |

注意这里没有保存 system_server 的服务对象。Fetcher 知道怎样在客户端创建 wrapper，必要时才调用上一章的 `ServiceManager` 获取 Binder。

---

## 8. `registerService()` 做的只是登记配方

```java
private static <T> void registerService(
        String serviceName,
        Class<T> serviceClass,
        ServiceFetcher<T> serviceFetcher) {
    SYSTEM_SERVICE_NAMES.put(serviceClass, serviceName);
    SYSTEM_SERVICE_FETCHERS.put(serviceName, serviceFetcher);
    SYSTEM_SERVICE_CLASS_NAMES.put(serviceName, serviceClass.getSimpleName());
}
```

例如 PowerManager：

```java
registerService(Context.POWER_SERVICE, PowerManager.class,
        new CachedServiceFetcher<PowerManager>() {
    public PowerManager createService(ContextImpl ctx)
            throws ServiceNotFoundException {
        IBinder powerBinder = ServiceManager.getServiceOrThrow(
                Context.POWER_SERVICE);
        IPowerManager powerService = IPowerManager.Stub.asInterface(powerBinder);
        // 还会取得 thermal service
        return new PowerManager(ctx.getOuterContext(), powerService,
                thermalService, ctx.mMainThread.getHandler());
    }
});
```

注册时并没有立即构造 PowerManager，也不一定立即查询 Binder。它只是登记一个延迟创建配方。

---

## 9. 静态初始化何时发生

`SystemServiceRegistry` 第一次主动使用时由 Java 类加载器执行静态初始化块。Android 核心类通常在 Zygote 预加载阶段已加载，因此许多注册工作会在 Zygote 中完成，然后由应用进程继承静态表。

但不要把这句话绝对化成“每次一定在 Zygote 完成”。关键语义是：

- 每个进程看到自己的 Java 静态状态；
- 注册配方在类静态初始化阶段建立；
- Manager 实例通常仍是第一次 `getSystemService` 时惰性创建；
- Binder 代理属于具体进程，不是把 Zygote 中已连接的业务 Binder 随便当全局对象共享。

SystemServer 启动后还把：

```java
SystemServiceRegistry.sEnableServiceNotFoundWtf = true;
```

用于强化核心系统组件查询不存在服务时的诊断。

---

## 10. 获取服务的主入口

```java
public static Object getSystemService(ContextImpl ctx, String name) {
    if (name == null) return null;
    ServiceFetcher<?> fetcher = SYSTEM_SERVICE_FETCHERS.get(name);
    if (fetcher == null) return null;
    return fetcher.getService(ctx);
}
```

主链可以画成：

```text
context.getSystemService(PowerManager.class)
  → class 映射为 "power"
  → ContextImpl.getSystemService("power")
  → SYSTEM_SERVICE_FETCHERS.get("power")
  → CachedServiceFetcher.getService(ctx)
  → 首次：createService(ctx)
  → ServiceManager.getServiceOrThrow("power")
  → IPowerManager.Stub.asInterface(binder)
  → new PowerManager(...)
  → 放入此 ContextImpl 的缓存槽
```

第二次由同一 ContextImpl 获取时，一般直接返回缓存中的 PowerManager。

---

## 11. ServiceFetcher 是策略接口

```java
interface ServiceFetcher<T> {
    T getService(ContextImpl ctx);
}
```

Android 11 主要有三种实现：

| Fetcher | 实例范围 | 是否传入 Context | 代表例子 |
|---|---|---|---|
| `CachedServiceFetcher` | 每个 ContextImpl 一个 | 是 | PowerManager、UserManager、WindowManager |
| `StaticServiceFetcher` | 每个进程一个 | 否 | HdmiControlManager 等 |
| `StaticApplicationContextServiceFetcher` | 每进程一个，首次构造时选用 application Context | Application Context | Android 11 的 ConnectivityManager、TestNetworkManager |

“缓存范围”说的是 Java Manager wrapper，不是服务端 Binder 实例数。

无论应用里有多少个 PowerManager wrapper，它们通常都指向 system_server 中同一个 `power` Binder 服务。

---

## 12. CachedServiceFetcher 的槽位设计

每创建一个 `CachedServiceFetcher`，构造器分配固定索引：

```java
private final int mCacheIndex;

CachedServiceFetcher() {
    mCacheIndex = sServiceCacheSize++;
}
```

静态注册完成后，`sServiceCacheSize` 就是每个 ContextImpl 所需缓存槽数量。ContextImpl 构造时创建：

```java
final Object[] mServiceCache =
        SystemServiceRegistry.createServiceCache();

final int[] mServiceInitializationStateArray =
        new int[mServiceCache.length];
```

可视化：

```text
全局注册配方                         某个 ContextImpl
PowerFetcher  mCacheIndex=0  ───────> mServiceCache[0] = PowerManager
WindowFetcher mCacheIndex=1  ───────> mServiceCache[1] = WindowManager
UserFetcher   mCacheIndex=2  ───────> mServiceCache[2] = UserManager
```

数组比以字符串为 key 的每 Context Map 更紧凑、更快；代价是注册顺序和 cache size 必须在 ContextImpl 创建前稳定。

---

## 13. 为什么还需要初始化状态数组

只看 `mServiceCache[index] == null` 无法区分：

- 尚未初始化；
- 正在由另一个线程初始化；
- 初始化成功但结果异常被清空；
- 服务不存在，创建失败并决定缓存 null。

所以每个槽还有状态：

```text
STATE_UNINITIALIZED = 0
STATE_INITIALIZING  = 1
STATE_READY         = 2
STATE_NOT_FOUND     = 3
```

状态机：

```text
UNINITIALIZED
   │ 首个线程抢到初始化权
   ↓
INITIALIZING ── 成功 ──> READY
   │
   └─ ServiceNotFoundException ──> NOT_FOUND
```

`NOT_FOUND` 会让以后查询直接返回 null，避免每次重复尝试和重复日志。

---

## 14. 多线程同时查询如何避免创建两份 Manager

`CachedServiceFetcher.getService()` 的核心并发协议：

1. 所有线程先锁定 `ctx.mServiceCache`。
2. 第一个看到 UNINITIALIZED 的线程把 gate 改为 INITIALIZING。
3. 它释放锁后执行 `createService(ctx)`。
4. 其他线程发现 INITIALIZING，在 cache 对象上等待。
5. 创建线程写入对象和最终状态，调用 `notifyAll()`。
6. 等待线程醒来重查槽位。

为什么构造 Manager 时不持有 cache 锁？

- `createService` 可能查询 Binder；
- 构造器可能执行更复杂初始化；
- 长时间持锁会阻塞同 Context 的其他系统服务查询；
- 还可能形成锁顺序和重入风险。

所以源码采用“锁内抢初始化资格，锁外真正创建，锁内发布结果”。

---

## 15. 等待时为什么保存 interrupt 状态

等待线程被中断时，源码不会直接退出并返回不完整状态，而是记录：

```java
interrupted = true;
```

继续等到初始化完成后，再：

```java
Thread.currentThread().interrupt();
```

理由是系统服务获取 API 没有声明 `InterruptedException`，并发初始化协议又不能让等待者越过尚未发布的结果。于是它完成获取，同时恢复线程的中断标志，让上层之后仍能观察到中断。

---

## 16. READY 但对象是 null 的特殊分支

源码注释提到：若 gate 是 READY，但缓存对象变成 null，就退回 UNINITIALIZED 再创建。

```java
if (gates[index] == STATE_READY) {
    gates[index] = STATE_UNINITIALIZED;
}
```

正常路径 READY 应伴随非空对象。这个分支是防御性恢复：有人或某些内部路径清空对象后，不应让 Context 永久返回 null。

它不是公开的“清理所有 Manager 缓存”API，也不意味着 Framework 定期驱逐这些对象。

---

## 17. StaticServiceFetcher：进程级 wrapper

```java
class StaticServiceFetcher<T> implements ServiceFetcher<T> {
    private T mCachedInstance;

    public final T getService(ContextImpl ctx) {
        synchronized (this) {
            if (mCachedInstance == null) {
                mCachedInstance = createService();
            }
            return mCachedInstance;
        }
    }
}
```

Fetcher 对象本身位于静态注册表，每个进程只有一份，因此 `mCachedInstance` 也是进程级。

它适合不依赖具体 Context、主题、display、用户包装或组件生命周期的 Manager。

若创建抛 `ServiceNotFoundException`，`mCachedInstance` 仍为 null，后续查询会再次尝试；这与 CachedServiceFetcher 的 `STATE_NOT_FOUND` 缓存 null 行为不同。

---

## 18. StaticApplicationContextServiceFetcher

这是 Android 11 中主要为 ConnectivityManager 等少数 manager 保留的特殊折中；r48 的
TestNetworkManager 也使用它：

```java
Context appContext = ctx.getApplicationContext();
mCachedInstance = createService(appContext != null ? appContext : ctx);
```

它仍缓存一份实例，但避免持有某个 Activity Context，降低 Activity 泄漏风险。如果 application Context 暂时为空（系统进程或应用初始化很早期），才使用传入的 ContextImpl。

源码 TODO 仍写着希望在 ConnectivityManager 不再需要后删除这种特殊类型，但同一 r48 文件中
TestNetworkManager 也已经使用它，可见该注释没有完全跟上调用点。判断使用者应以实际
`new StaticApplicationContextServiceFetcher` 搜索结果为准。学习时仍应把它看作少数历史兼容
场景，不要自然推广成新服务的默认模板。

---

## 19. 为什么许多 Manager 必须按 Context 缓存

Context 不只是“能调用 API 的对象”，它携带：

- package/attribution 身份；
- user；
- display 与窗口环境；
- configuration、Resources、theme；
- 主线程 Handler；
- outer Activity/Service。

例如 WindowManager：

```java
return new WindowManagerImpl(ctx);
```

它与 display/window token 等环境强相关。把 Activity A 的 WindowManager 给 Activity B 使用，语义可能错误。

因此 `Context` 文档提醒：通过某个 Context 取得的系统服务可能与这个 Context 紧密关联，不应随意跨 Context 共享。

---

## 20. “每个 Context 一个”要说准确

`CachedServiceFetcher` 缓存位于 **每个 ContextImpl**，不是每个 Java Context wrapper 必然独立。

多个 ContextWrapper 若共享同一个 base ContextImpl，会落到同一缓存；不同 Activity、不同 display Context、不同 configuration Context 可能拥有不同 ContextImpl 和不同 Manager wrapper。

```text
Wrapper A ─┐
           ├─ 同一 ContextImpl → 同一 cached Manager
Wrapper B ─┘

Activity ContextImpl ────────→ Manager A
Application ContextImpl ─────→ Manager B
```

所以最稳妥的措辞是“per-ContextImpl cache”。

---

## 21. Manager 何时查询 Binder

有三类常见模式。

### 模式 A：Registry 构造时立即查 Binder

```java
IBinder b = ServiceManager.getServiceOrThrow(Context.USER_SERVICE);
IUserManager service = IUserManager.Stub.asInterface(b);
return new UserManager(ctx, service);
```

### 模式 B：Manager 构造时自己查

```java
return new ClipboardManager(ctx.getOuterContext(), handler);
```

ClipboardManager 内部再按自身设计取得远端接口。

### 模式 C：传入延迟查询函数

```java
return new TetheringManager(
        ctx, () -> ServiceManager.getService(Context.TETHERING_SERVICE));
```

因此不能看到 Registry 的 `createService()` 没有 `ServiceManager.getService`，就断言该 Manager 没有 Binder 后端。必须继续进入 Manager 类阅读。

---

## 22. `getServiceOrThrow()` 为什么在这里常见

Framework 预期很多核心服务在应用可运行前就已发布。如果缺少，静默返回一个带 null Binder 的 Manager 往往只会把错误推迟到更难理解的位置。

`ServiceManager.getServiceOrThrow(name)` 找不到时抛：

```text
ServiceNotFoundException
```

Fetcher 捕获它并调用 `onServiceNotFound()`：

- 核心 UID 进程通常 `Log.wtf`；
- 普通应用通常写 warning；
- 最终 Manager 可能返回 null。

“OrThrow” 不一定会原样穿透为应用看到的运行时异常，因为 Registry fetcher 已经捕获了受检异常。

---

## 23. Manager 缓存不是 Binder 存活保证

假设 Context 缓存了 PowerManager：

```text
mServiceCache[index] → PowerManager → IPowerManager.Proxy → Binder handle
```

如果远端服务死亡：

- Java Manager 对象仍可能留在缓存；
- 其中 Binder Proxy 可能已经死亡；
- 调用会抛/转换 `RemoteException`；
- 是否自动重连取决于具体 Manager 的实现。

SystemServiceRegistry 的职责是 wrapper 构造与缓存，不是统一的 Binder death 重连框架。

因此不能说“getSystemService 有缓存，所以系统服务重启对应用透明”。

---

## 24. 服务端如何发布 Binder 服务

SystemServer 使用 `SystemService` 生命周期框架。服务通常在 `onStart()` 中：

```java
@Override
public void onStart() {
    publishBinderService(Context.DEMO_SERVICE, new BinderService());
}
```

`SystemService.publishBinderService()` 只是封装：

```java
protected final void publishBinderService(
        String name, IBinder service,
        boolean allowIsolated, int dumpPriority) {
    ServiceManager.addService(name, service, allowIsolated, dumpPriority);
}
```

于是它与上一章衔接：

```text
SystemService.onStart
  → publishBinderService
  → ServiceManager.addService
  → native servicemanager 服务表
```

`publishBinderService` 不是另一个注册中心，只是 system_server 服务的便利方法。

---

## 25. 客户端与服务端如何靠同一个名字接上

服务端：

```java
publishBinderService(Context.POWER_SERVICE, binder);
```

客户端 Registry：

```java
ServiceManager.getServiceOrThrow(Context.POWER_SERVICE);
```

二者共享 `Context.POWER_SERVICE == "power"`。完整闭环：

```text
PowerManagerService
  └─ publish "power" + Binder
             ↓
       native ServiceManager map
             ↓
Registry fetcher query "power"
  └─ IBinder → IPowerManager → PowerManager
```

如果两边名字不同，即使服务已经运行，也接不上。

---

## 26. `SystemService`、`SystemServiceRegistry` 名字很像但职责不同

| 类 | 所在侧 | 职责 |
|---|---|---|
| `com.android.server.SystemService` | system_server | 服务生命周期、boot phase、user 回调、发布 Binder/LocalService |
| `android.app.SystemServiceRegistry` | 每个 Framework Java 进程 | Context API 的 Manager 注册、构造和缓存 |
| `android.os.ServiceManager` | 客户端门面/跨进程 | 按字符串名字注册或取得 Binder |
| native servicemanager | 独立进程 | 保存 Binder 服务目录并做访问控制 |

读代码前先看 package，能避免大量混乱。

---

## 27. LocalServices 是另一条完全不同的通道

`SystemService` 还提供：

```java
publishLocalService(MyInternal.class, implementation);
getLocalService(MyInternal.class);
```

它进入 `LocalServices`，本质是 system_server 同进程内的 class → object Map。

```text
Binder Service                         LocalService
跨进程可用                             仅 system_server 同进程
按字符串名字                           按 Java Class
需要 Binder/AIDL/Parcel                普通 Java 方法调用
有 Binder/SELinux/身份边界              依赖调用方已在可信 system_server 内
面向 App/其他进程及系统组件             面向 system_server 服务间内部协作
```

LocalService 常命名为 `XxxManagerInternal`，它不是应用能通过 `Context.getSystemService()` 获取的公共 Manager。

---

## 28. 为什么同时发布 BinderService 和 LocalService

一个系统服务可能提供两张“脸”：

```text
                    ┌─ BinderService：跨进程、权限检查、稳定边界
PowerManagerService ┤
                    └─ LocalService：system_server 内高效内部接口
```

好处：

- 公共 Binder API 不必暴露所有内部能力；
- system_server 服务间调用不必序列化 Parcel；
- 内部接口可以传递只适合同进程的对象或 callback；
- 权限与生命周期边界更清楚。

但 LocalService 不是自动线程安全的。普通 Java 直调会在调用者线程执行，服务实现仍要自行处理锁和线程切换。

---

## 29. 类型安全入口并非编译期保证服务一定存在

```java
context.getSystemService(PowerManager.class)
```

泛型帮助减少错误强转，但运行时仍可能返回 null，例如：

- class 未在 Registry 注册；
- 设备不支持可选功能；
- instant app 无权访问某些服务；
- 底层服务未发布；
- Registry 的 producer 决定返回 null。

所以 API 注解、设备能力和文档仍要一起看。类型安全只保证“非 null 时返回类型应正确”。

---

## 30. 为什么不直接让每个 Manager 自己做单例

如果所有 Manager 都各自实现静态单例，会遇到：

- 无法统一 class/name 映射；
- 难以表达 per-Context 与 per-process 差异；
- Context、user、display、theme 容易被错误共享；
- 并发初始化和缺失服务处理重复实现；
- 模块化 Framework wrapper 难以集中注册。

Registry 把“如何生产和缓存 wrapper”抽成 Fetcher，同时仍允许每个 Manager 自己处理远端连接、callback 和业务状态。

---

## 31. Android 11 的模块化注册

SystemServiceRegistry 静态块末尾调用：

```java
JobSchedulerFrameworkInitializer.registerServiceWrappers();
BlobStoreManagerFrameworkInitializer.initialize();
TelephonyFrameworkInitializer.registerServiceWrappers();
WifiFrameworkInitializer.registerServiceWrappers();
StatsFrameworkInitializer.registerServiceWrappers();
```

这些模块通过公开给系统模块的注册方法接入：

```java
registerStaticService(...)
registerContextAwareService(...)
```

为了保证缓存索引在初始化期固定，Registry 用 `sInitializing` 与 `ensureInitializing()` 限制：只能在它的静态初始化调用链中注册。

这反映 Android 模块化后的设计：服务 wrapper 不必全部硬编码在一个巨大文件中，但仍必须在受控阶段完成注册。

---

## 32. 带 Binder 与不带 Binder 的 producer

模块注册 API 分成：

```text
StaticServiceProducerWithBinder
StaticServiceProducerWithoutBinder
ContextAwareServiceProducerWithBinder
ContextAwareServiceProducerWithoutBinder
```

含义是两个维度的组合：

| 维度 | 选择 A | 选择 B |
|---|---|---|
| wrapper 范围 | static/process | context-aware/per ContextImpl |
| 创建参数 | Registry 先取 Binder 后传入 | producer 自己创建，不直接接收 Binder |

“without Binder”不代表这个 Manager 永远不用 Binder；它可能在内部稍后查询，或本身只包装本地能力。这里只描述 producer 构造签名。

---

## 33. 以 PowerManager 串起完整调用

### 33.1 system_server 启动服务

```text
SystemServer
  → SystemServiceManager.startService(PowerManagerService)
  → PowerManagerService.onStart()
  → publishBinderService("power", BinderService)
```

### 33.2 客户端首次获取 Manager

```text
Context.getSystemService(PowerManager.class)
  → class → "power"
  → CachedServiceFetcher[power]
  → ServiceManager.getServiceOrThrow("power")
  → IPowerManager.Stub.asInterface
  → new PowerManager(context, proxy, ...)
  → cache[index] = PowerManager
```

### 33.3 客户端调用

```java
PowerManager.WakeLock lock = pm.newWakeLock(...);
lock.acquire();
```

概念上：

```text
PowerManager/WakeLock
  → IPowerManager.acquireWakeLock(...)
  → Binder driver
  → PowerManagerService.BinderService
  → 权限/身份检查
  → PowerManagerService 内部状态机
```

SystemServiceRegistry 只参与首次取得 wrapper，不参与每次 wake lock 业务调用。

---

## 34. 并不是每个 Context 服务背后都有独立 Binder 名字

LayoutInflater：

```java
return new PhoneLayoutInflater(ctx.getOuterContext());
```

WindowManager：

```java
return new WindowManagerImpl(ctx);
```

SensorManager：

```java
return new SystemSensorManager(context, looper);
```

这些 Manager 的实现结构各不相同：

- 有的纯粹是本地 wrapper；
- 有的通过其他全局类/JNI/native 通道连接服务；
- 有的在构造器内部获取 Binder；
- 有的一个 wrapper 组合多个 Binder 服务。

所以 `Context.getSystemService` 是统一的获取门面，不代表所有服务后端具有完全相同的 AIDL 结构。

---

## 35. 一个 Manager 也可能组合多个服务

Android 11 PowerManager 的 fetcher 同时取得：

```text
"power"   → IPowerManager
"thermal" → IThermalService
```

然后构造一个 PowerManager。这说明：

```text
一个 Manager class ≠ 必然只对应一个 Binder name
```

反过来也可能存在一个 Binder 服务被多个客户端 wrapper 或内部 API 使用。Registry 映射是 Framework API 设计，不是强制一对一数据库关系。

---

## 36. 用户、包和 display 身份来自哪里

Manager 方法需要调用者环境时，通常从构造时传入的 Context 获取：

- `getOpPackageName()`；
- attribution tag；
- userId；
- displayId；
- Resources/configuration；
- main looper/Handler。

跨 Binder 后，服务端还能从 Binder 获取真实 calling UID/PID，不能只信客户端传来的 packageName。

因此常见安全模式是：

```text
Context 提供声明身份与环境
+ Binder driver 提供不可伪造的 calling UID/PID
+ 服务端 PackageManager/AppOps/权限校验二者关系
```

Manager wrapper 的便利不等于安全裁决在客户端完成。

---

## 37. Context 泄漏与 Manager 泄漏

若某 Manager 按 ContextImpl 缓存并保存 outer Activity Context，那么它与 Activity 生命周期一致通常没有问题；Activity 被释放时，其 ContextImpl、cache 和 Manager 可一起回收。

危险场景是应用自己把 Activity 获取到的 Manager 放进静态变量：

```text
static field → Manager → Activity Context → View/Window/Resources
```

这可能延长 Activity 生命周期。

但也不要机械规定“所有 Manager 必须从 applicationContext 获取”。WindowManager、LayoutInflater 等视觉服务需要正确的 Activity/display Context。原则是按照 API 语义选择 Context，不是只为了避免泄漏一律改 application Context。

---

## 38. 服务发布时序为何重要

如果客户端在服务端 `publishBinderService()` 前调用 fetcher：

- `getServiceOrThrow()` 可能抛 ServiceNotFoundException；
- CachedServiceFetcher 可能把该槽记为 `STATE_NOT_FOUND`；
- 同一个 ContextImpl 后续不会自动重试该 Manager。

因此 Framework 启动顺序必须保证核心服务在允许对应客户端运行前已发布。SystemServer 的 bootstrap/core/other services 与 boot phases 不只是整理代码，而是在建立依赖时序。

对于真正可晚到或可重启的后端，Manager 往往需要自行设计延迟查询、callback、death recipient 或重连，不能盲目套首次永久缓存失败的模式。

---

## 39. `sEnableServiceNotFoundWtf` 的边界

SystemServer 开启该标志后，Registry 会对未知 Manager 或意外 null 发 `Slog.wtf`，帮助发现系统启动顺序和注册错误。

但源码也为部分可能合法返回 null 的服务做例外，例如 Android 11 中的 Content Capture、App Prediction、Incremental Service。

`wtf` 是严重诊断日志，不等价于 Java 一定立刻抛异常或进程必然崩溃。最终行为还取决于日志/系统策略和调用路径。

---

## 40. 新增一个 Framework 系统服务要改哪些层

概念清单：

### 服务端

1. 实现 SystemService 或在合适宿主中创建服务。
2. 定义 Binder/AIDL 接口及 Stub 实现。
3. 在 SystemServer 合适阶段启动。
4. `publishBinderService("demo", binder)`。
5. 配置 service_contexts 与 SELinux add/find/call。

### 客户端 Framework API

1. 定义 `DemoManager`。
2. 在 Context 定义服务名常量（若属于公共 Context API）。
3. 在 SystemServiceRegistry 注册 name、class、fetcher。
4. 选择正确缓存范围。
5. 在 fetcher 或 Manager 内取得 Binder 并 `asInterface()`。
6. 处理服务不存在、死亡、多用户、线程和 callback 生命周期。

### 可选内部通道

1. 定义 `DemoManagerInternal`。
2. 服务端 `publishLocalService()`。
3. system_server 内消费者 `LocalServices.getService()`。

这比“只向 ServiceManager addService”多出 Framework 易用 API 和生命周期设计。

---

## 41. 如何选择 Fetcher

可以按以下问题判断：

```text
Manager 是否依赖具体 Context 的 user/display/theme/package/Handler？
 ├─ 是 → CachedServiceFetcher
 └─ 否
     ├─ 真正可全进程共享 → StaticServiceFetcher
     └─ 必须持有 Application Context → 特殊 application-context 方案
```

再复查：

- Manager 会不会持有 Activity 导致泄漏？
- 不同用户 Context 是否应该得到不同语义？
- 不同 display Context 是否必须产生不同 wrapper？
- 服务晚发布时是否允许缓存 NOT_FOUND？
- Binder 重启后 wrapper 能否恢复？

缓存选择是语义设计，不只是性能优化。

---

## 42. 常见误区纠正

### 误区 1：`getSystemService()` 直接从 servicemanager 返回 Manager

错误。servicemanager 返回 IBinder；SystemServiceRegistry/Manager 构造逻辑产生 Java wrapper。

### 误区 2：Manager 和 system_server Service 是同一个类实例

错误。它们通常跨进程，Manager 是客户端门面，Service 是服务端实现。

### 误区 3：所有 Manager 都是进程单例

错误。大量 Manager 是 per-ContextImpl，只有 StaticServiceFetcher 是进程级 wrapper。

### 误区 4：同一 App 中 `getSystemService()` 永远返回 `==` 相同对象

错误。同一 ContextImpl 的 cached fetcher 通常相同；不同 ContextImpl 可能不同。

### 误区 5：不同 wrapper 意味着不同系统服务端实例

错误。多个 wrapper 可以持有指向同一 Binder 服务的代理。

### 误区 6：Manager 已缓存，所以远端死亡会自动重连

错误。重连策略由具体 Manager 决定。

### 误区 7：LocalServices 是没有权限检查的跨进程快速 Binder

错误。它完全是 system_server 同进程 Java 对象表，不经过 Binder。

### 误区 8：`publishBinderService()` 发布的是 Manager

错误。发布的是 IBinder/Stub；Manager 在客户端构造。

### 误区 9：class API 找不到服务一定抛异常

错误。公开契约通常允许返回 null；内部异常可能被 Fetcher 捕获并记录。

### 误区 10：所有 Context 都应该换成 applicationContext

错误。视觉、display、user 等 Context 相关服务需要正确 Context。

---

## 43. 一张对象与缓存关系图

```text
App Process
┌─────────────────────────────────────────────────────────────┐
│ SystemServiceRegistry（静态配方表）                          │
│   "power" → PowerFetcher(index=0)                           │
│                     │                                       │
│   Activity ContextImpl                 App ContextImpl       │
│   cache[0] → PowerManager A            cache[0] → PM B      │
│                 │ IPowerManager.Proxy          │ Proxy      │
└─────────────────┼──────────────────────────────┼─────────────┘
                  └──────────────┬───────────────┘
                                 │ Binder driver
                                 ↓
System Server
┌─────────────────────────────────────────────────────────────┐
│ PowerManagerService.BinderService（通常同一个服务端 Binder） │
│ PowerManagerService.LocalService（只供本进程内部）            │
└─────────────────────────────────────────────────────────────┘
```

两份 PowerManager wrapper 不等于两份 PowerManagerService。

---

## 44. 一张首次查询时序图

```text
Caller       Context/Impl        Registry/Fetcher       ServiceManager       Server
  │ get(PowerManager.class)             │                     │                │
  ├────────────>│ class→"power"         │                     │                │
  │             ├──────────────────────>│                     │                │
  │             │                       │ cache miss           │                │
  │             │                       │ gate=INITIALIZING    │                │
  │             │                       ├─ getService("power")>│                │
  │             │                       │<────── IBinder ──────┤                │
  │             │                       │ asInterface + new PM │                │
  │             │                       │ cache + READY        │                │
  │<────────────┴───────────────────────┤                     │                │
  │ PowerManager API                    │                     │                │
  ├──────────────────────────────────────── Binder call ─────────────────────>│
```

后续同一 ContextImpl 再查，一般在 Fetcher cache 处返回，不再走 ServiceManager。

---

## 45. Mac 上的只读源码练习

### 练习 1：追 class 与 string 两个入口

```bash
rg -n "getSystemService\(|getSystemServiceName\(" \
  frameworks/base/core/java/android/content/Context.java \
  frameworks/base/core/java/android/app/ContextImpl.java
```

写出：class → name → fetcher 的三步转换。

### 练习 2：对比三个 Fetcher

```bash
rg -n "class (CachedServiceFetcher|StaticServiceFetcher|StaticApplicationContextServiceFetcher)" \
  frameworks/base/core/java/android/app/SystemServiceRegistry.java
```

分别记录缓存对象放在哪里、是否接收 Context、创建失败后是否重试。

### 练习 3：对比三个服务例子

```bash
rg -n "registerService\(Context\.(POWER|WINDOW|CONNECTIVITY)_SERVICE" \
  frameworks/base/core/java/android/app/SystemServiceRegistry.java
```

继续打开完整代码，比较 PowerManager、WindowManager、ConnectivityManager 为什么选择不同方式。

### 练习 4：追服务端发布

```bash
rg -n "publishBinderService\(Context.POWER_SERVICE|publishLocalService" \
  frameworks/base/services frameworks/base/core/java/com/android/server
```

画出 BinderService 与 LocalService 两条分支。

### 练习 5：观察模块化 wrapper 注册

```bash
rg -n "registerServiceWrappers|register(ContextAware|Static)Service" \
  frameworks/base/wifi frameworks/base/telephony frameworks/base/apex \
  frameworks/base/core/java/android/app/SystemServiceRegistry.java
```

目标：理解模块把“配方”注册回核心 Registry，而不是创建第二套 Context API。

---

## 46. 阅读一个新 Manager 的固定七问

1. Context service name 是什么？
2. Registry 对应哪个 Manager class？
3. 使用哪种 Fetcher，缓存范围是什么？
4. `createService()` 是否立即取得 Binder？
5. Manager 保存 Context、Handler、Binder、callback 中的哪些对象？
6. 服务端在哪里 `publishBinderService()`？
7. 服务死亡、用户切换、Context 销毁后如何清理或恢复？

回答完这七问，通常就不会只停留在 API 表面。

---

## 47. 自测题

1. `getSystemService(PowerManager.class)` 为什么仍要先映射为字符串？
2. SystemServiceRegistry 与 native servicemanager 各保存什么？
3. CachedServiceFetcher 的 cache index 何时分配？
4. 为什么 ContextImpl 同时需要对象数组和状态数组？
5. Manager 创建为什么放在 cache 锁外？
6. StaticServiceFetcher 的“static”是跨设备还是跨进程？
7. 两个 Activity 取得的 PowerManager 一定 `==` 吗？
8. 两个 PowerManager wrapper 是否表示两个 PowerManagerService？
9. `STATE_NOT_FOUND` 会产生什么后果？
10. publishBinderService 最终调用谁？
11. LocalService 能否给普通 App 使用？
12. Registry 中没直接调用 ServiceManager，能否证明该 Manager 无 Binder？

---

## 48. 参考答案

1. 因为子类可以重写字符串版本，Context 契约要求 class 入口先映射名字再调用它。
2. Registry 保存客户端 wrapper 的映射/构造配方；servicemanager 保存名字到 Binder 的运行时目录。
3. Registry 静态初始化创建 CachedServiceFetcher 时。
4. null 不能区分未初始化、正在初始化和确认不存在，还需 gate 协调并发。
5. 避免耗时 Binder/构造操作长期占锁以及锁重入、死锁风险。
6. 当前 Java 进程。
7. 不一定；不同 ContextImpl 有不同缓存槽对象。
8. 不是；它们通常连接同一个服务端 Binder。
9. 同一 ContextImpl 后续该槽通常直接返回 null，不再创建。
10. `android.os.ServiceManager.addService()`，再进入 native servicemanager。
11. 不能；它是 system_server 同进程内部对象表。
12. 不能；Manager 构造器或后续方法可能自己查询 Binder/JNI/native 服务。

---

## 49. 第二遍复读：最容易不理解的地方

### 49.1 “系统服务”一词同时指四样东西

日常说的系统服务可能是：system_server 服务实现、它发布的 Binder、应用拿到的 Manager，或 Context 中的 service name。讨论时最好明确使用 `PowerManagerService`、`IPowerManager Binder`、`PowerManager wrapper`、`"power"`。

### 49.2 两级注册表不能合并理解

SystemServiceRegistry 是每进程内的 Java wrapper 配方表；native servicemanager 是全系统 Binder 名字目录。前者产生易用 API 对象，后者返回跨进程引用。

### 49.3 “缓存 Manager”不等于“缓存服务端”

per-Context cache 存的是 PowerManager 等 wrapper。服务端对象仍在 system_server；Binder driver 和代理连接二者。

### 49.4 “每 Context”准确说是每 ContextImpl

Wrapper 可以共享 base ContextImpl；不同 ContextImpl 才有独立 `mServiceCache`。outer Context 又可能被 Manager 用于组件/视觉语义。

### 49.5 `NOT_FOUND` 是启动时序承诺

CachedServiceFetcher 把找不到结果记在该 ContextImpl 中，说明它适合预期已经发布的服务。可晚到服务若照搬，会出现“后来发布了但旧 Context 仍返回 null”的问题。

### 49.6 LocalService 不是 Binder 的优化模式

它没有 Parcel、Proxy、UID 传播和跨进程能力，只是可信 system_server 内的 Java 对象引用。调用线程也不会自动切换。

---

## 50. 本章总结

主链压缩如下：

```text
Context.getSystemService(Class)
  → Registry 将 class 映射为 name
  → name 找到 Fetcher
  → Fetcher 按 per-Context/process 策略创建并缓存 Manager
  → Manager 构造时或稍后通过 ServiceManager 按 name 获取 Binder
  → AIDL asInterface 形成远端接口
  → Manager API 经 Binder 直达 system_server 的 BinderService
```

服务端的反向链路：

```text
SystemServer 启动 SystemService
  → onStart
  → publishBinderService
  → ServiceManager.addService
  → native servicemanager 保存 name + Binder
```

同进程内部则可另走：

```text
publishLocalService → LocalServices → 普通 Java 直调
```

理解这三条链后，就能准确回答：“我拿到的 Manager 从哪来，它缓存在哪里，它最终调用哪个服务端？”

---

## 51. 下一章预告

第 95 章将学习：

**Android SystemServiceManager：系统服务启动、BootPhase、依赖顺序与用户生命周期**

重点包括：

- SystemServer 如何分阶段启动服务；
- `startService()` 如何反射构造并调用 `onStart()`；
- `startBootPhase()` 为什么只能单调前进；
- 服务启动失败如何影响 system_server；
- `onUserStarting/Unlocking/Unlocked/Stopping/Stopped` 如何分发；
- Binder 发布完成、boot phase 完成和真正业务 ready 的区别。
