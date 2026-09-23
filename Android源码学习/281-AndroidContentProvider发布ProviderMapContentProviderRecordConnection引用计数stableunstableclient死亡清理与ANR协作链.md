# 281 Android ContentProvider 发布链：ProviderMap、ContentProviderRecord/Connection、引用计数、死亡清理与 ANR 协作

## 1. 先看结论：拿到 Provider 不是一次查表，而是跨进程租约的建立与收束

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。上一章追到 URI grant 如何在 system_server 落账；本章回到更底层的问题：客户端执行一次 ContentResolver 调用之前，authority 怎样找到 Provider，冷进程怎样完成安装和发布，调用期间谁保护宿主进程，Binder 死亡后又由谁承担后果？

先把完整链压成八个完成点：

1. `ApplicationContentResolver` 把 authority 拆成规范 authority 与 userId，`ActivityThread` 先查本进程缓存；
2. 缓存未命中时，客户端按 `(authority,userId)` 取得一把长期驻留的小锁，再向 AMS 请求 Holder；
3. AMS 先做调用者、跨用户、Provider 可能访问性与 association 检查，再查全局 singleton / 用户级 ProviderMap；
4. 已发布的远端 Provider 立即建立或累加 `ContentProviderConnection`，同步更新 LRU/OOM 后返回 Binder；可本地运行的 Provider 只返回元数据，让调用进程自行实例化；
5. 未发布时，AMS 解析 ProviderInfo、决定 singleton 与宿主进程、登记 launching demand，并把调用者连接标成 waiting；
6. 宿主 attach 后收到 ProviderInfo 列表，先实例化 Provider、执行 `attachInfo()` / `onCreate()`，再批量 `publishContentProviders()`；
7. publish 把可信的 ContentProviderRecord 映射到 class 与全部 authority，在记录锁内写 Binder 与 ProcessRecord、唤醒等待者；
8. 客户端安装 Binder、聚合引用；最后一份引用经过一秒缓冲才从客户端缓存移除，服务端连接、外部句柄、进程死亡和 ANR 上报各有独立终点。

这些步骤不是一个原子事务。ProviderMap 中可以已有“正在启动但尚无 Binder”的记录；等待者超时可以返回 null，却留下 connection、launching 记录和隐式可见性；Provider 可以先于 `Application.onCreate()` 对外发布；一次 publish 也可能只完成同一进程中的部分 Provider。排查时不能只问“Provider 在不在”，而要分别问：记录是否存在、宿主是否活着、Binder 是否发布、调用者租约是否仍在、哪一层已经超时或死亡。

本章中的 stable / unstable 也不是两档 Binder 或 OOM 强度。两者指向同一个 `IContentProvider`，OomAdjuster 遍历 Connection 时并不检查引用类型；差异主要发生在 Provider 死亡时：stable 依赖可能连带终止客户端，unstable 依赖则允许客户端收到死亡通知并重新获取。

## 2. 两端对象图：authority、class、Binder、Connection 与引用聚合不能混成一张表

system_server 的 `ProviderMap` 有四张索引：singleton 按 authority、singleton 按 ComponentName，以及按 userId 分片的 authority/class 两组 Map。查找总是先查全局 singleton，再查指定用户。写入依据 `ContentProviderRecord.singleton` 选择全局或用户表，用户表的 key 来自记录中 Provider 应用 UID，而不是当前请求参数的一份旁路标签。

同一个 Provider class 可以声明用分号分隔的多个 authority。class 索引回答“这个组件是否已有唯一的 ContentProviderRecord”，authority 索引回答“这个名字当前路由到哪个记录”。冷启动时只会先放入本次请求的 authority，publish 才把 `dst.info.authority` 中全部名字补齐；因此“按 class 已有记录”“某个别名可查”“所有别名已发布”是三个不同状态。

服务端核心对象各管一件事：

| 对象 | 主要身份 | 代表什么 | 不代表什么 |
|---|---|---|---|
| `ContentProviderRecord` | component + ProviderInfo + user/singleton | 一代 Provider 的发布、宿主、连接与 launching 状态 | 不保证 `provider` Binder 已非空 |
| `ContentProviderConnection` | 一个 client ProcessRecord → 一个 CPR | framework 客户进程对该 CPR 的 stable/unstable 服务端账 | 不是每个 Java Cursor 一项 |
| external handle | token 或匿名计数 → CPR | 没有 ProcessRecord 的系统侧 demand 与 OOM 保护 | 没有 stable 客户端死亡语义 |
| `ContentProviderHolder` | ProviderInfo + 可选Binder + 可选connection | 一次获取返回给调用端的快照 | 不是长期真相或死亡监听本身 |

客户端也有四类索引。`mProviderMap` 以 `(authority,userId)` 指向 `ProviderClientRecord`；`mProviderRefCountMap` 以 Provider Binder identity 指向 `ProviderRefCount`；`mLocalProviders` 以本地 Binder 指向本地记录；`mLocalProvidersByName` 以 ComponentName 去重本地实例。多个 authority 可以指向同一 Binder 与同一个引用聚合对象，释放时不能按 authority 猜引用数。

对象边界决定了诊断顺序：先用 ProviderMap 判断名字路由，再用 CPR 看 `proc/provider/launchingApp`，再用 Connection 看 `s/u/WAITING/DEAD`，最后回到客户端 Binder 引用聚合。只看某一端容易出现“服务端还有 Connection，但客户端已经没有可释放 Holder”或“客户端 authority 缓存仍有项，但 Binder 已死”的错觉。

### 练习 1：验证服务端四张索引与客户端四类缓存

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private final HashMap<String, ContentProviderRecord> mSingletonByName' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'private final HashMap<ComponentName, ContentProviderRecord> mSingletonByClass' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'private final SparseArray<HashMap<String, ContentProviderRecord>> mProvidersByNamePerUser' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'private final SparseArray<HashMap<ComponentName, ContentProviderRecord>> mProvidersByClassPerUser' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'ContentProviderRecord record = mSingletonByName.get(name);' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'return getProvidersByName(userId).get(name);' frameworks/base/services/core/java/com/android/server/am/ProviderMap.java
grep -n -F 'final ArrayMap<ProviderKey, ProviderClientRecord> mProviderMap' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'final ArrayMap<IBinder, ProviderRefCount> mProviderRefCountMap' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'final ArrayMap<IBinder, ProviderClientRecord> mLocalProviders' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'final ArrayMap<ComponentName, ProviderClientRecord> mLocalProvidersByName' frameworks/base/core/java/android/app/ActivityThread.java
```

画出一个 Provider class 声明 `a;b`、同时被两个客户端进程使用时的对象图：服务端有几项 class/name 索引、几条 Connection，单个客户端又有几个 ProviderKey 与几个 ProviderRefCount。然后分别让第二个 authority 和第二个客户端加入，说明哪一层增长、哪一层复用。

## 3. 客户端获取：user-aware key 先命中缓存，小锁只包住 AMS 往返

`ApplicationContentResolver` 不把带 `10@` 的 authority 原样交给 `ActivityThread`。它用 `getAuthorityWithoutUserId()` 去掉 user-info，并用 `getUserIdFromAuthority(auth, contextUser)` 得到 userId。于是客户端缓存键是规范 authority 与用户的二元组，同字符串在个人用户和工作资料是两项；没有显式 user-info 时，Context 的 user 决定路由。

`acquireProvider()` 首先调用 `acquireExistingProvider()`。命中后还要检查 Binder `isBinderAlive()`；若已死，ActivityThread 先清除该 Binder 对应的引用聚合及所有 authority cache，再通知 AMS 处理 unstable death，随后返回 miss。Binder 活着且存在 ProviderRefCount 才增加本地 stable/unstable 引用；本地 Provider 没有这份远端引用账，可直接返回。

miss 路径通过 `getGetProviderLock(auth,userId)` 取锁。这张锁表没有清理逻辑，进程见过的 key 会留下一个小对象。锁只包住 `AMS.getContentProvider()` 的 Binder 往返，`installProvider()` 在退出锁后执行；锁内也没有二次缓存检查。因此它只让同 key 的 AMS 请求串行，不保证第二个线程复用第一个线程已经安装的结果。时序可以是：线程 A 从 AMS 返回、释放小锁但尚未 install，线程 B 随即进入 AMS，于是两者都拿到服务端引用；最终客户端 install 通过 Binder identity 决胜，并归还输掉的 Holder connection。

这也解释两个边界。第一，不同 authority 别名使用不同小锁，即使最后映射到同一 Binder，也能并发请求。第二，小锁不覆盖 Provider 本地实例化，因为本地 `attachInfo()/onCreate()` 可能重入 ContentResolver；把大锁跨过这段代码会制造死锁，r48 选择在安装阶段再补偿竞态。

客户端 user key 还有 singleton 可观测差异。AMS 可把跨用户请求路由到 user 0 singleton，Holder 的 `applicationInfo.uid` 也属于 user 0；`installProviderAuthoritiesLocked()` 据此把 authority 缓存在 user 0 key。最初以 user 10 查询的 key 仍可能 miss，后续再次请求时又去 AMS，随后才按 Binder identity 合并引用。这不改变服务端 singleton 身份，却说明“成功获取一次”不必然填充最初请求的客户端 key。

### 练习 2：重放同 key 双线程、别名并发与 singleton user key

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final ProviderKey key = new ProviderKey(auth, userId);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'final IContentProvider provider = acquireExistingProvider(c, auth, userId, stable);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'synchronized (getGetProviderLock(auth, userId)) {' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'holder = ActivityManager.getService().getContentProvider(' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'holder = installProvider(c, holder, holder.info,' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'mGetProviderLocks.put(key, lock);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'if (!jBinder.isBinderAlive()) {' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'handleUnstableProviderDiedLocked(jBinder, true);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'ContentProvider.getAuthorityWithoutUserId(auth),' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'return ContentProvider.getUserIdFromAuthority(auth, getUserId());' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'final int userId = UserHandle.getUserId(holder.info.applicationInfo.uid);' frameworks/base/core/java/android/app/ActivityThread.java
```

为线程 A/B 写出“cache miss → A 得到 Holder → B 进入 AMS → A/B 先后 install”的合法序列，标注服务端引用怎样由客户端竞态补偿。再比较 authority `a` 与 `b`、请求user 10但返回user 0 singleton两种情况，说明为什么锁 key、cache key 与 Binder identity 分别解决不同问题。

## 4. AMS 准入与快路：先找记录，再证明调用者有可能访问，最后建立依赖

公开 `getContentProvider()` 先拒绝 isolated caller、拒绝 null `IApplicationThread`，并用 AppOps `checkPackage()` 校验可选 callingPackage 是否属于 Binder UID。进入内部实现后再把 IApplicationThread 映射到 ProcessRecord；找不到记录同样抛 SecurityException。external 获取则要求 `ACCESS_CONTENT_PROVIDERS_EXTERNALLY`，没有 client ProcessRecord，稍后只建立 external handle。

查表有两个容易混淆的 singleton 分支。`ProviderMap.getProviderByName(name,userId)` 自身先查全局 singleton，再查该用户；全局直接命中后不会再次执行 `isValidSingletonCall()`。只有初次查表为 null、显式回退查 user 0 记录时，AMS 才同时验证记录确属 singleton 且此次调用可以共享。无论哪条命中，后续仍需 association、跨用户与权限检查，不能把全局 Map 命中当成访问授权。

`checkContentProviderPermissionLocked()` 是“可能访问”预检，不是具体 URI 的最终 Transport enforcement。它在跨用户时先看 authority 下是否已有任意 URI grant，然后才走 `handleIncomingUser()`；静态侧只要 Provider 顶层 read 或 write 任一权限通过，或者任意 PathPermission 声明的某个权限通过，就允许取得 Binder；最后还可凭 authority 下任意 UriPermission 通过。这里没有具体 URI path、没有本次 CRUD 类型，所以拿到 Binder 不代表某个 query/update 已获准。下一章会进入 ContentProvider.Transport 的逐 URI、逐操作、AppOps 与 attribution 校验。

已有记录只有 `cpr.proc != null && !cpr.proc.killed` 才算 running；这不是 Binder ping。若 running 且 `canRunHere()`，条件精确为 `(multiprocess || processName相同) && Provider uid与调用进程应用uid相同`，AMS 返回 provider/connection 均为空的 Holder，让调用端本地实例化。shared UID 配合 multiprocess 也可能满足；external 因为没有 ProcessRecord，绝不会走本地分支。

远端快路先建立或累加 Connection，再更新 LRU/OOM。一个 client ProcessRecord 对同一 CPR 复用同一 Connection；第一次引用才加入两端列表并启动 association。OOM 更新失败时，AMS 撤掉本次引用；只有该普通客户端已无其他服务端引用、dec返回最后一份时才转冷启动，否则本次直接返回null，external分支也因没有Connection而返回null。只有OOM更新报告成功且 `verifiedAdj` 与新 `setAdj` 不同，才额外读 `/proc/pid/stat` 缩小“进程已被杀但记录尚存”的窗口。即便成功，源码也承认信号仍可能在途，因此返回的 Binder 仍可能很快死亡。

### 练习 3：区分全局 singleton 命中、user 0 回退、可能访问与本地实例化

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'enforceNotIsolatedCaller("getContentProvider");' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (callingPackage != null && mAppOpsService.checkPackage(callingUid, callingPackage)' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr = mProviderMap.getProviderByName(name, userId);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr = mProviderMap.getProviderByName(name, UserHandle.USER_SYSTEM);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F '&& isValidSingletonCall(r == null ? callingUid : r.uid,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (r != null && cpr.canRunHere(r)) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'holder.provider = null;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'return (info.multiprocess || info.processName.equals(app.processName))' frameworks/base/services/core/java/com/android/server/am/ContentProviderRecord.java
grep -n -F 'if (checkComponentPermission(cpi.readPermission, callingPid, callingUid,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'PathPermission[] pps = cpi.pathPermissions;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mUgmInternal.checkAuthorityGrants(callingUid, cpi, userId, checkUser)' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (success && verifiedAdj != cpr.proc.setAdj' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

为“user 10直接命中全局singleton”“user 10查不到后回退user 0”“同UID multiprocess本地运行”“只有某条PathPermission写权限”四例标出查表、singleton验证、Connection与最终CRUD权限的结果。尤其说明为何最后一例可以拿 Binder，却未证明任意query都能成功。

## 5. 冷启动：解析 ProviderInfo、选择一代 CPR、登记 launching demand，再启动或复用进程

快路失效后，AMS 用 PackageManager 按请求user解析 ProviderInfo，并携带 URI permission patterns。它重新计算 singleton；合法 singleton 改到 user 0，再把 ApplicationInfo转换到最终user。association、可能访问、system-ready、system provider安装状态与目标user运行状态都在启动前收束。这里才是“由声明创建记录”的路径，而不是简单把旧的 name Map 项当真。

随后按 ComponentName 查 class Map。没有记录时先处理运行时权限review，取得该user的 ApplicationInfo，构造新 CPR。若刚才发现旧宿主被AMS标死，且 class Map仍指向那一代 CPR，代码复制一份新的 CPR，避免新客户端挂到会在旧进程清理中被连带杀死的记录上。它不是同步等待旧进程死亡；ProcessList 的进程代际协调可能另有短暂等待，但 ContentProvider 获取代码本身继续建立新一代需求。

这里会第二次检查 `canRunHere()`。若满足，本地 Holder 立即返回，发生在远端 ProviderMap name/class 写入和 launching登记之前。因此不能笼统说“新 CPR 总会先写 class Map”；一个仅供调用进程本地实例化的 multiprocess Provider 可以没有这次服务端发布状态。另一方面，attach 时的 `generateApplicationProvidersLocked()` 是宿主进程 eager Provider 列表路径，会按 class 预建记录，两条路径也不能混为一次动作。

远端路径先检查 CPR 是否已在 `mLaunchingProviders`。若不是，则清 package stopped 状态并寻找目标 ProcessRecord：进程已 attach、thread非空且未 killed 时，把 CPR放入 `proc.pubProviders` 后调用 `scheduleInstallProvider()`；只有该Provider类名尚不在 pubProviders 才调度。Binder 调度异常被忽略，CPR仍会进入 launching。若目标进程不存在，就以 content-provider HostingRecord 启动进程；启动失败直接返回 null。

最后给 CPR 写 `launchingApp` 并加入 `mLaunchingProviders`。对远端新 class，AMS才写 class Map；无论是否firstClass，都把**本次请求的 authority**写 name Map，建立/累加 Connection，并在退出AMS大锁前设 `conn.waiting=true`。之后还会给调用UID授予对Provider appId的隐式包可见性。这些状态都早于 Binder publish。

已有进程的安装路径有一个安静失败窗口：`scheduleInstallProvider()` 没有本段专属 publish watchdog；而且如果 class name 已在 `proc.pubProviders`，后续请求不会再次调度。一次预登记后安装失败，可能只剩每次 get 自己的20秒等待来暴露问题。

### 练习 4：验证冷解析、第二次本地分支、进程复用与 launching 建账顺序

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'resolveContentProvider(name,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F '&& isValidSingletonCall(r == null ? callingUid : r.uid,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr = mProviderMap.getProviderByClass(comp, userId);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr = new ContentProviderRecord(this, cpi, ai, comp, singleton);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr = new ContentProviderRecord(cpr);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'return cpr.newHolder(null);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (!proc.pubProviders.containsKey(cpi.name)) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'proc.thread.scheduleInstallProvider(cpi);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'new HostingRecord("content provider",' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr.launchingApp = proc;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mLaunchingProviders.add(cpr);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'conn.waiting = true;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

按源码为三种请求排序：同进程multiprocess、本来已有已attach宿主、必须fork新宿主。分别标注何时有class Map、name Map、pubProviders、mLaunchingProviders、Connection与Binder；解释 `scheduleInstallProvider()` 抛 RemoteException 后哪些状态仍然存在。

## 6. 三只时钟：fork 到 attach、attach 到 publish、单次 get 等 ready

r48 这里至少有三只不能相加成一个“Provider启动超时”的时钟：

| 时钟 | 起点 → 终点 | 常量 | 超时动作 |
|---|---|---|---|
| 进程启动 | fork/start → `attachApplication` | `PROC_START_TIMEOUT=10s`，wrapper为1200s | 按进程启动失败清理 |
| Provider发布 | 已attach且该进程仍承载launching Provider → publish | `CONTENT_PROVIDER_PUBLISH_TIMEOUT=10s` | 对宿主走 publish timeout / 进程问题处置 |
| 当前获取 | get进入等待 → CPR出现Binder | `CONTENT_PROVIDER_READY_TIMEOUT=20s` | 当前调用记录严重日志并返回null |

第二只时钟只在 attach 阶段发现 `checkAppInLaunchingProvidersLocked(app)` 时安排。因此“把Provider安装到已经attach的进程”没有同等的10秒 publish timer；它主要由第三只时钟让请求方结束等待。第一只时钟也不是 Provider 自己的10秒，它监督进程能否 attach；带wrapper的调试进程甚至是1200秒。

单次get在退出AMS锁后同步 `synchronized(cpr)` 等待。每轮用绝对deadline减当前uptime，再调用 `cpr.wait(wait)`。Java 的 `wait(0)` 意味着无限等待；而实现一旦被任意唤醒、Binder仍为空，就直接把 `timedOut` 设true并break，并没有重新核对deadline后持续循环。因此源码形状虽有 while，却不具备严格的“抗虚假唤醒deadline循环”语义。正常publish会写Binder后notify，通常不触发该边界。

更重要的是20秒超时只返回null。它不调用 `decProviderCountLocked()`，不删除 name/class Map，不撤销 mLaunchingProviders，不撤销隐式包可见性，也不归还 external handle。普通调用者没拿到 Holder，就没有 connection capability 可在稍后 release；该服务端引用可能一直留到进程死亡或其他清理。若 Provider更晚发布，这条未交付的 stable Connection仍会成为将来Provider死亡时的依赖处置依据。

`waiting` 也不是完美的“此刻线程在wait”指示。远端冷路退出AMS锁前已把它设true；只有实际进入 `try/finally` 的 wait 才在finally清false。若publish恰在拿到CPR锁前完成，while一次都不进，成功返回的Connection仍可能保留 `WAITING` 标记。这会影响后续启动失败清理如何跳过连接，诊断时应把它视为实现状态而非精确线程采样。

### 练习 5：定位三只时钟并推演 ready timeout 的残留

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static final int PROC_START_TIMEOUT = 10*1000;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'static final int PROC_START_TIMEOUT_WITH_WRAPPER = 1200*1000;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'public static final int CONTENT_PROVIDER_PUBLISH_TIMEOUT_MILLIS = 10 * 1000;' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'public static final int CONTENT_PROVIDER_READY_TIMEOUT_MILLIS =' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'if (providers != null && checkAppInLaunchingProvidersLocked(app)) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mHandler.sendMessageDelayed(msg,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'SystemClock.uptimeMillis() + ContentResolver.CONTENT_PROVIDER_READY_TIMEOUT_MILLIS;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'final long wait = Math.max(0L, timeout - SystemClock.uptimeMillis());' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'cpr.wait(wait);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'timedOut = true;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'return null;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

画出“fork第0秒、attach第8秒、publish第15秒”和“已有进程第0秒scheduleInstall、永不publish”两条时间线。再让客户端在第20秒超时、第25秒宿主发布，列出 CPR、Map、Connection、waiting、客户端Holder五项状态，说明为什么“调用返回null”不等于需求已回滚。

## 7. 宿主 attach 与本地安装：Provider.onCreate 早于 Application.onCreate，也早于服务端 publish

新进程 attach 后，AMS 取消进程启动timeout，调用 `generateApplicationProvidersLocked()` 查询该 processName/uid 的全部 Provider。PackageManager 对列表按 `initOrder` 降序排列；AMS为每个Provider按class查建 CPR，放进 `app.pubProviders`，并把列表送进 `bindApplication`。这条 eager 列表会包含同一宿主的其他Provider，不只最初触发进程启动的那一项。

ActivityThread 创建 Application 对象后，在非restricted backup模式先执行 `installContentProviders(app,data.providers)`，之后才调用 instrumentation 和 `callApplicationOnCreate(app)`。每个Provider安装会选择合适Context与split classloader，经AppComponentFactory实例化，取本地Transport Binder，调用 `localProvider.attachInfo(c,info)`；`attachInfo()` 内部再进入 Provider 的 `onCreate()`，其boolean返回值不会决定是否发布。Provider因此可以在Application.onCreate前初始化和对外可见，Provider.onCreate若依赖Application自定义初始化必须自己处理顺序。

每个本地实例在 `mProviderMap` 锁外构造并运行 `attachInfo/onCreate`，随后才进锁按 ComponentName与本地Binder登记。并发安装同一 class 时，输掉竞态的实例已经执行过构造与onCreate副作用，最后只是改用 `mLocalProvidersByName` 中的胜者；框架没有对败者调用统一shutdown。这是解释“onCreate日志出现两次但最终只有一个路由对象”的关键边界。

安装循环把成功Holder加入results，最后一次 Binder调用批量publish。若某个实例化异常未被Instrumentation处理，进程会因RuntimeException中断；若Instrumentation接住异常，install返回null，该项不进入results，其他项仍可继续。`Application.onCreate()` 自己随后崩溃时，Provider可能已经publish过，于是其他进程短暂获得Binder后再经历宿主死亡。

`noReleaseNeeded` 在本地安装中为true。ActivityThread仍可能为远端、来自system进程且“不需release”的 Binder创建一个 `ProviderRefCount(1000,1000)` 哨兵；不能把这种状态写成“完全没有ProviderRefCount”。真正本地Provider走local maps，不依赖普通远端release协议。

### 练习 6：验证 attach、initOrder、Provider.onCreate 与 Application.onCreate 的先后关系

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'List<ProviderInfo> providers = normalMode ? generateApplicationProvidersLocked(app) : null;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F '.queryContentProviders(app.processName, app.uid,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'app.pubProviders.put(cpi.name, cpr);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'final int v1 = p1.initOrder;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'app = data.info.makeApplication(data.restrictedBackupMode, null);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'installContentProviders(app, data.providers);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'mInstrumentation.callApplicationOnCreate(app);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F '.instantiateProvider(cl, info.name);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'localProvider.attachInfo(c, info);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'ProviderClientRecord pr = mLocalProvidersByName.get(cname);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'mLocalProvidersByName.put(cname, pr);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'publishContentProviders(' frameworks/base/core/java/android/app/ActivityThread.java
```

给两个Provider设置不同initOrder，其中一个onCreate成功、另一个异常被Instrumentation接住，再让Application.onCreate抛异常。写出哪些本地map、publish结果与外部可见窗口可能存在；再加入同class并发安装，指出败者副作用为什么不会由Map竞态补偿消除。

## 8. publish 完成点：信任服务端记录的身份，写全别名，设置 Binder 后唤醒等待者

`publishContentProviders()` 对null列表直接返回，甚至早于isolated与caller校验；空列表则会完成这些校验但不改变Provider状态。对每个非空Holder，AMS要求 `src.info/src.provider` 非空，再只用 `src.info.name` 到发布进程的 `r.pubProviders` 找目标CPR。找不到就忽略该项。

一旦找到 `dst`，真正用于Map身份的是服务端保存的 `dst.info`：按它的package/name写class Map，并拆分它的authority写入所有name Map。上传Holder中的其他ProviderInfo字段没有逐项与dst比对；Binder实现也没有在此验证“确由该Provider class构造”。安全边界依赖发布者已经被绑定到正确ProcessRecord、且name必须命中其pubProviders白名单，而不是信任客户端重建整份ProviderInfo。

Map写入先于 `dst.provider=src.provider`，但二者都发生在AMS大锁内，新get无法在中间观察到半步状态。随后AMS从 `mLaunchingProviders` 删除该CPR；如果删到至少一项，就按**进程**移除 `CONTENT_PROVIDER_PUBLISH_TIMEOUT_MSG`。然后在 `synchronized(dst)` 内写Binder、`setProcess(r)`并`notifyAll()`，清零restart count，更新OOM与usage stats。这一小段才是等待者看到 ready 的完成点。

同一进程有多个launching CPR时，部分发布会暴露一个进程级锐角：任一CPR发布成功就移除该进程唯一的publish timeout message，即使兄弟CPR仍在mLaunchingProviders。兄弟请求仍有各自20秒ready wait，但失去attach后的10秒进程级监督。批量循环中间没有事务回滚，先发布项已经可用，后续无效或异常项不会撤回它。

publish 也没有在写Map前确认“当前class/name Map仍精确指向dst”，不像死亡清理那样做对象identity保护。结合进程代际竞态，晚到旧发布是否可能重写新Map，是审计时应显式检查的缝隙，不能用“Map已是新记录所以旧publish自然被拒”作假设。另一个实现边界是 clearCallingIdentity 的恢复没有包在finally；正常路径会恢复，运行期异常则不能用统一finally语义描述。

### 练习 7：逐行确定 publish 的身份、Map、launching、唤醒与 timeout 顺序

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (providers == null) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'ContentProviderRecord dst = r.pubProviders.get(src.info.name);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'ComponentName comp = new ComponentName(dst.info.packageName, dst.info.name);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'String names[] = dst.info.authority.split(";");' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mProviderMap.putProviderByName(names[j], dst);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (mLaunchingProviders.get(j) == dst) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mHandler.removeMessages(CONTENT_PROVIDER_PUBLISH_TIMEOUT_MSG, r);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'dst.provider = src.provider;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'dst.setProcess(r);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'dst.notifyAll();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'dst.mRestartCount = 0;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'updateOomAdjLocked(r, true, OomAdjuster.OOM_ADJ_REASON_GET_PROVIDER);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

让进程P有CPR-A与CPR-B都在launching，只发布A；列出两项的name/class map、provider、launching与等待计时器状态。再构造src.info.name命中但src.info.authority不同的Holder，指出哪些上传字段决定匹配、哪些服务端字段决定最终Map。

## 9. 客户端安装决胜：authority 映射按 Provider UID 建 key，Binder identity 聚合引用

远端Holder回到ActivityThread后，`installProvider()` 先看Binder identity。若该Binder已有ProviderRefCount，说明另一次获取或另一个authority已经安装成功：本次若需要release，就增加胜者的本地引用，再调用AMS `removeContentProvider(holder.connection,stable)` 归还这次输掉的服务端引用。这样最终使用同一Binder，却不会无故遗失第二次逻辑acquire。

若Binder尚未出现，`installProviderAuthoritiesLocked()` 把ProviderInfo中分号分隔的所有authority映射到新的ProviderClientRecord。每个key的user来自 Holder Provider应用UID。authority key已被占用时只记录警告，不覆盖旧记录；随后仍以这个新Binder创建ProviderRefCount。因此极端别名冲突时，Binder ref map与authority map并不保证一一可达，诊断必须同时看两者。

普通远端ProviderRefCount初值是本次获取类型对应的 `1/0` 或 `0/1`。`noReleaseNeeded` 远端则使用 `1000/1000` 哨兵并仍放入ref map；本地Provider走 `mLocalProvidersByName` 与 `mLocalProviders`，Holder被改成noReleaseNeeded，也没有普通Connection release生命周期。

服务端计数与客户端Java对象数也不是同一粒度。ActivityThread把同一进程、同一Binder的多个ContentResolver操作或ContentProviderClient聚合到ProviderRefCount；只在某类本地计数发生0↔1转换时通知AMS。AMS则按“同一client ProcessRecord→同一CPR”复用Connection。稳态服务端stable/unstable更像每类是否存在的聚合位，但跨authority竞态补偿、隐藏接口或并发过渡可让计数大于1，不能把它声明成严格boolean。

安装决胜还有一个本地路径差异：`canRunHere()` 返回的Holder没有Binder，ActivityThread会自己instantiate并运行onCreate，然后按ComponentName决胜。即使服务端另一进程已经发布了同一multiprocess Provider，只要当前caller满足canRunHere，它仍可能再有一份本地实例；“全系统每class唯一”从来不是multiprocess契约。

## 10. 引用状态机：最后一份 stable 先转成 unstable，一秒后才真正移除

对普通远端Provider，本地stable与unstable引用分别计数。第一次整体获取时，服务端 `incProviderCountLocked()` 已经建立对应的stable或unstable计数，客户端随后只是以 `1/0` 或 `0/1` 创建新ProviderRefCount，不再补发ref调用。已有PRC后，某类本地计数从0→1才通过 `refContentProvider()` 增加服务端该类计数；同类继续增加只改本地账。释放到某类0时才通知服务端；但最后一份总引用采用延迟移除协议，避免短促连续CRUD让Provider缓存抖动。

若最后一份是stable且没有unstable，本地先向AMS发送 `stable -1, unstable +1`，在服务端保留一份临时unstable，然后设置 `removePending`，向主线程Handler延迟一秒发送REMOVE_PROVIDER。若最后一份本来就是unstable，则不先把服务端减到0，只安排相同消息。`refContentProvider()` 明确禁止任何计数变负，也禁止在该入口把总数降到0；真正归零必须由稍后的 `removeContentProvider(connection,false)` 完成。

一秒窗口内重新acquire有两条“抢救”：stable重获把临时unstable转换回stable，发送 `+1/-1`；unstable重获只取消pending，因为那份临时unstable本来仍在服务端。Handler消息可能已经在队列中，`completeRemoveProvider()`先检查removePending，输掉竞态就退出；若确实移除，则按Binder删ref map和全部authority cache，再在锁外让AMS删除服务端Connection。

这固定一秒只属于客户端缓存缓冲。服务端最后Connection从两端表移除时，如果client足够重要，会给宿主写 `lastProviderTime`；OomAdjuster再用可配置、默认20秒的 `content_provider_retain_time` 把宿主当recent-provider保护一段时间。两只计时器起点、对象和作用完全不同。

`refContentProvider()` 在Connection已标dead时仍会提交合法的计数变更，只用返回false报告死亡；ActivityThread的调用处忽略这个boolean。`removeContentProvider()` 又是另一入口，直接走dec并允许最后归零；它没有ref入口相同的负数预检。因此账坏时不能只根据一次返回值断言客户端已经修复或服务端拒绝了修改。

## 11. ContentResolver 调用：query 先用 unstable 探路，再把 stable 租约交给 Cursor

普通 `ContentResolver.query()` 先acquire unstable Provider，并为CancellationSignal向该Provider创建远端transport。query若抛 `DeadObjectException`，客户端清死Binder缓存并通知AMS，然后获取stable Provider重试**一次**；第二次仍失败会落入RemoteException处理，不是无限重启。

得到非空Cursor后，ContentResolver先调用 `getCount()`，按源码意图强制/验证Cursor执行并尽早暴露异常；跨进程BulkCursor在服务端构造descriptor时可能已经取过count，所以不能把这一步一律解释成新增一次远端取数。若重试时已持stable，就沿用它；否则再acquire一次stable。随后用 `CursorWrapperInner` 同时持有Cursor与stable Provider，把局部stable变量清空，finally只释放unstable。调用者close Cursor时，wrapper以AtomicBoolean保证只释放一次stable。

这带来一个窄边界：成功取得qCursor后，额外stable acquire理论上仍可能返回null；构造wrapper并不拒绝null Provider。Cursor仍会返回调用者，却没有预期的stable宿主保护，close时release(null)只返回false。正常系统中同一活Binder应快速命中，问题主要出现在死亡竞态。

文件打开走相似协议。openAsset/openTyped先unstable调用，死亡后stable重试一次；成功后用 `ParcelFileDescriptorInner` 持有stable引用，直到FD close的资源释放回调。CRUD如update、insert、delete通常直接stable acquire，在同步Binder调用finally中立即release；因此“ContentResolver总是先unstable”“所有stable都绑定Cursor”都不成立。

`ContentProviderClient` 又不同：创建client本身已经取得stable或unstable租约，调用者合同要求所有方法复用它直到client close。它返回的CursorWrapper只负责Cursor CloseGuard，不额外acquire/transfer Provider引用，文件描述符也原样返回。文档要求client在仍使用返回数据时保持打开；把ContentResolver.query的“Cursor自己续租”经验套到ContentProviderClient，会过早释放宿主保护。实现中的 `mClosed` 只在close路径判断，CRUD/query入口并不据此拒绝调用，所以close后仍可能发出Binder事务，却已经没有可靠租约；本地Provider第一次deprecated `release()`也可能返回false，AtomicBoolean仍已完成关闭。

### 练习 8：追踪 query、Cursor、文件描述符与 ContentProviderClient 的租约所有者

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'IContentProvider unstableProvider = acquireUnstableProvider(uri);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'remoteCancellationSignal = unstableProvider.createCancellationSignal();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'unstableProviderDied(unstableProvider);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'stableProvider = acquireProvider(uri);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'qCursor.getCount();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final CursorWrapperInner wrapper = new CursorWrapperInner(qCursor, provider);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'ContentResolver.this.releaseProvider(mContentProvider);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'ParcelFileDescriptor pfd = new ParcelFileDescriptorInner(' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'return new ContentProviderClient(this, provider, name, true);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'return new ContentProviderClient(this, provider, uri.getAuthority(), false);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'return new CursorWrapperInner(cursor);' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'if (mClosed.compareAndSet(false, true)) {' frameworks/base/core/java/android/content/ContentProviderClient.java
```

分别画出普通query成功、unstable死亡后重试、openTyped成功、stable ContentProviderClient query四条租约时间线。标出unstable何时释放、stable由谁持有、Cursor/FD/client中谁的close才是最终完成点，并说明stable重获为null的Cursor锐角。

## 12. 一次重试的取消陷阱：旧 Provider 创建的 transport 不属于新 Provider

ContentResolver在第一次unstable调用前执行 `unstableProvider.createCancellationSignal()`。这个Binder RPC让旧Provider进程创建一个 `CancellationSignal.Transport`；回到客户端后持有的是它的远端Binder代理。正常调用回到同一个Provider进程时，Binder驱动能把本地Binder对象还原为Transport实例，`CancellationSignal.fromTransport()`取出实际CancellationSignal。

DeadObject后，query/open代码获取新Provider，却复用原来的 `remoteCancellationSignal`。这个对象仍指向旧进程创建的Binder；传到新Provider时只是代理，不是新进程本地的 `Transport` 实例。r48 的 `fromTransport()`只在 `transport instanceof Transport` 时返回对象，否则返回null。因此重试本身可能不可合作取消；ContentResolver随后在finally执行 `cancellationSignal.setRemote(null)`，只能解除本地关联，不能把旧transport变成新宿主的transport。

这不是所有ContentProviderClient调用的普遍情况。ContentProviderClient不自动重新获取Provider；unstable client捕获DeadObject后通知AMS并重新抛出，调用者必须close失效client、另行acquire并发起新操作。新client的新调用会向新Provider创建新的取消transport。stable client死亡也重抛，但不走unstableProviderDied通知。另一个持有边界是CPC的query/refresh/open等路径会给调用者CancellationSignal设置remote，却不像ContentResolver包装路径那样在finally清空；方法返回不等于自动cancel，旧transport可继续挂在Signal上直到后续替换或对象释放。

还要区分“取消”“等待ready timeout”和“ANR探测”。CancellationSignal只在调用真正进入Provider且实现合作检查时终止工作；20秒ready发生在拿到Binder之前，用户传给CRUD的CancellationSignal不能直接中断AMS的CPR等待；ContentProviderClient的ANR Runnable则只是另一路定时上报，不会调用CancellationSignal，也不取消当前Binder事务。

因此可恢复调用的稳妥模型是：一次Provider代际配一份远端取消transport；Binder代际改变后重新创建操作上下文。若要审计r48，测试应在旧Provider创建transport后强杀旧宿主、让新宿主接收同一代理，再观察Provider侧CancellationSignal为null，而不是只验证客户端对象已经cancel。

## 13. OOM 与 external handle：stable/unstable 同样传播重要性，外部需求是另一种能力

OomAdjuster遍历宿主进程的 `pubProviders`，再遍历每个CPR的connections，读取client当前adj/procState并向宿主传播。循环没有读取 `stableCount`、`unstableCount` 或 `dead`；只要Connection仍在列表，两类引用对宿主OOM/LRU保护相同。stable/unstable的强弱发生在死亡协议，不是“stable把宿主提得更高”。

已有已发布Provider的get会立即updateOomAdj；启动路径要等publish后再更新。LRU提升也有限定：只有该client→CPR Connection总计数变成1，且client已有setAdj足够重要时才把宿主上提LRU。最后Connection从重要client移除后，服务端写lastProviderTime，默认20秒recent-provider保护防止抖动；client进程直接死亡的批量清理不走这个dec路径，所以不建立同样的recent时间点。

没有framework ProcessRecord的使用者走external handle。`incProviderCountLocked(r==null)` 只向CPR登记token或匿名计数并返回null Connection，哪怕调用参数stable=true。它会让OomAdjuster把宿主至少提高到foreground adj / important-foreground procState，却没有“Provider死则杀external客户端”语义，也无法通过connection定位ANR客户端。

非空token首次登记会linkToDeath，同token多次获取只增加acquisition count；token死亡一次删除整项。r48有几处账和诊断边界：linkToDeath失败仍保留handle；binderDied删除handle却不立即调用updateOomAdj；同token计数大于1时显式remove正确减一但返回false，AMS会误记“没有external reference”；传入不匹配token而其他token仍存在时，会走匿名计数递减，甚至降为负数。

匿名external要求调用方显式配对。同步 `getProviderMimeType()` 会在finally归还；异步版本只在远端callback里归还，若oneway调用当场RemoteException或Provider永不callback，该方法不会归还匿名计数；宿主若仍活，它继续构成external OOM边，否则只是旧CPR上的残留账。普通已发布Provider死亡也不会仅因还存在external handle自动重启；handle影响OOM与launching失败是否仍有需求，不是永久重启订阅。

## 14. Provider 与 client 死亡：stable 连带处置、unstable 通知、launching 才可能重启

已发布Provider宿主死亡时，AMS按对象identity从class/name Map摘除旧CPR，避免误删已经替换的新一代；再遍历旧CPR的Connection并置 `dead=true`。若某连接stableCount大于0，非persistent、thread存在、pid有效且非system_server的客户端进程会以 dependency died 原因被kill。stable优先，所以同一Connection同时有stable/unstable时仍走连带处置；这一分支不当场从双向表移除Connection，persistent等免杀客户端因而可能暂时保留dead连接。

没有stable、client thread仍存在且旧Provider Binder非空时，AMS向客户端发 `unstableProviderDied(oldBinder)`，然后立即从CPR与client的双向列表移除Connection；条件不满足的连接由其他进程清理路径收束。ActivityThread收到后按Binder删ProviderRefCount与所有authority cache；客户端主动在调用中先发现DeadObject时也会做相同本地清理，再调用AMS `unstableProviderDied(connection)`，AMS先ping当前Binder，确认已死且仍对应同一宿主后进入appDied。两条路径共同缩小“客户端知道死、服务端尚不知道”的窗口。

启动未完成时 `cpr.provider==null`，规则不同。若仍允许重启，waiting Connection会被直接跳过：不置dead、不kill、不通知，继续等同一CPR下一次宿主。若已终局清理，stable waiter仍可能触发连带kill；unstable waiter因为Provider Binder为空，既没有回调也不会进入立即摘连接分支，get被notify后返回null，Connection可能留到client死亡。

自动重启只属于仍在 `mLaunchingProviders`、普通崩溃清理传入 `alwaysBad=false` 且还有Connection或external handle的需求。这个分支以 `MAX_RETRY_COUNT=3` 把初次尝试后的重启封顶为三次；process-start timeout与10秒publish timeout传 `alwaysBad=true`，第一次超时就终局清理，不享受这组重试。成功publish会把计数清零。已经发布后才死亡的普通Provider，不会只因旧Connection或handle自动拉起；下一次显式acquire才重新形成启动需求。

客户端进程死亡则反向遍历 `app.conProviders`，从各CPR连接表摘除并清空，无需等待逐个release，也不区分stable/unstable。该路径不经过最后引用的dec逻辑，不设置lastProviderTime；常规进程死亡清理随后会全局重算OOM，active instrumentation等分支可抑制这次重算。Provider死亡不会顺带撤销上一章的URI grants，两套生命周期要由各自owner、包清理或授权API收束。

20秒ready timeout使死亡语义更尖锐：超时没有减Connection，调用者也没有Holder可释放；更晚publish后，该stable Connection可能在宿主再死时参与连带kill。诊断“客户端从未成功拿到Provider”不能据此推导“服务端没有把它当依赖”。

## 15. ContentProviderClient ANR 探测：定时上报宿主，不取消调用，也不是通用 CRUD timeout

隐藏/System API `setDetectNotResponding(timeout)` 在timeout大于0时创建每个client自己的Runnable，并共享一个主Looper上的async静态Handler；同时对Provider Binder调用 `Binder.allowBlocking()`。每次远端方法前 `postDelayed`，finally中的afterRemote移除同一个Runnable。设为0或close会把字段置null并恢复defaultBlocking；client的AtomicBoolean保证租约只释放一次，finalize仅是遗忘close时的兜底。

Runnable到期调用 `ActivityThread.appNotRespondingViaProvider(providerBinder)`，ActivityThread用Binder找到ProviderRefCount，再把Holder.connection交给AMS。AMS在这里才运行时强制 `REMOVE_TASKS`，找到Connection关联的宿主ProcessRecord，然后交给AnrHelper。setter上的权限annotation不是本地runtime enforcement；没有权限的隐藏API调用者可能成功配置，直到超时回调才收到SecurityException。

这条链只**报告**“ContentProvider not responding”。它不取消Binder事务、不保证立刻kill，也不向Provider方法注入deadline。ContentResolver里3秒的常量只服务特定异步getType/canonicalize路径，不能推广为query/update的统一超时。若Provider是本地实例而没有PRC，或者引用已从client map移除，ActivityThread可能没有connection可上报；远端noRelease Provider仍有1000/1000的PRC与普通Holder.connection，不能归入该类。external MIME路径的Holder.connection恒为null，AMS也只能记录null。

ContentProviderClient公开文档说明实例不保证线程安全。r48实现进一步说明为什么不应共享并发调用：同一个client的多个调用共用同一个Runnable，其中一个较早完成时 `removeCallbacks` 会移除另一调用的watchdog；close把mAnrRunnable置null，却没有显式移除此前已经排队的旧Runnable，阻塞调用期间close可能留下迟到上报。不同client又可能共享同一Binder代理，allowBlocking/defaultBlocking切换也会互相影响。

watchdog本身还依赖主Looper执行。若受监督的同步Provider调用恰好就在主线程阻塞，Runnable虽然到期，却无法在同一被阻塞Looper上运行；调用返回后的finally又会removeCallbacks。async Handler只能绕过同步屏障，不能让一个阻塞中的Looper并发执行。因此它适合监督其他线程的阻塞调用，不能被当成主线程Provider调用的可靠deadline。把client限定到单线程、成对close，并让监督Looper保持可调度，才符合这段实现的真实前提。

### 练习 9：验证 ANR 定时、权限执行点、connection 路由与并发边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F '@RequiresPermission(android.Manifest.permission.REMOVE_TASKS)' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'mAnrRunnable = new NotRespondingRunnable();' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'sAnrHandler = new Handler(Looper.getMainLooper(), null, true' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'sAnrHandler.postDelayed(mAnrRunnable, mAnrTimeout);' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'sAnrHandler.removeCallbacks(mAnrRunnable);' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'Binder.allowBlocking(mContentProvider.asBinder());' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'Binder.defaultBlocking(mContentProvider.asBinder());' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'mContentResolver.appNotRespondingViaProvider(mContentProvider);' frameworks/base/core/java/android/content/ContentProviderClient.java
grep -n -F 'ProviderRefCount prc = mProviderRefCountMap.get(provider);' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'enforceCallingPermission(REMOVE_TASKS, "appNotRespondingViaProvider()");' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'final ProcessRecord host = conn.provider.proc;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mAnrHelper.appNotResponding(host, "ContentProvider not responding");' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

用同一ContentProviderClient并发发起A/B两次长调用：A先post、B后post、A先完成，逐步写Handler中Runnable数量与B是否仍受监督。再比较本地Provider、普通远端Connection、external MIME三种情况，指出timeout发生时哪条链能定位宿主、哪条只能静默或记日志。

## 16. 诊断矩阵：把“找不到、等不到、拿到后死亡、调用卡住”拆成四类证据

ContentProvider问题最容易被一条“Failed to find provider”或“ANR”概括过头。更可靠的排查矩阵是：

| 现象 | 首先确认 | 能证明 | 仍不能证明 |
|---|---|---|---|
| AMS立即返回null/拒绝 | ProviderInfo、user、association、possible-access、system/user状态 | 请求在启动前哪个门结束 | 具体URI的Transport权限结果 |
| 等待ready超时 | CPR的class/name Map、launchingApp、mLaunchingProviders、conn WAITING | Binder在本次等待内未发布 | Connection、Map、隐式可见性已回滚 |
| Holder返回后DeadObject | 客户端authority cache、Binder ref、服务端CPR代际与death路径 | 某一Binder代际已死 | 下一次acquire一定重启或旧grant已撤销 |
| Provider调用过慢 | CPC watchdog是否配置、connection能否定位host、调用线程与CancellationSignal | 是否触发上报/合作取消 | Binder事务已取消或宿主必然被kill |

推荐按五个问题收束现场：

1. authority携带的user-info是否被规范到预期 `(auth,user)`，singleton最终落在哪个用户key；
2. ProviderMap中的CPR是已发布、launching、旧代际残留，还是仅class eager登记；
3. client到CPR是普通Connection、local instantiate还是external handle，谁有能力释放；
4. 当前看到的是10秒进程attach、10秒attach后publish、20秒单次ready、一秒客户端retain还是20秒服务端recent保护；
5. stable/unstable差异发生在死亡处置还是只被误当成OOM等级，Cursor/FD/client究竟谁持有最后租约。

这五问能解释几类看似矛盾的日志：客户端得到null而dumpsys仍有Connection；同authority重复进入AMS但最终复用同一Binder；Provider.onCreate已打印而Application.onCreate尚未执行；一个Provider发布后同进程兄弟仍在launching却没有publish timer；unstable调用可重获新Binder但复用旧取消transport；没有成功Holder的stable waiter仍可能在稍后宿主死亡时成为依赖。

本文的终点是“Transport Binder已经由一代Provider发布，并由明确租约托住或释放”。它还没有回答一次query/update/open在Provider进程内怎样验证具体URI、calling package与attribution，怎样组合read/write permission、PathPermission、URI grant和AppOps，也没有进入bulk操作的逐项边界。下一章将继续到 **Android ContentProvider Transport：读写权限、PathPermission、URI grant、calling package/attribution、AppOps、跨用户与CRUD/Bulk执行链**。
