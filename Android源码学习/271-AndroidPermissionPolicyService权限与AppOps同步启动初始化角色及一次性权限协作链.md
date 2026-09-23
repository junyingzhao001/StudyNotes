# 271 Android PermissionPolicyService：权限与 AppOps 同步、启动初始化、角色及一次性权限协作链

## 1. 先看结论：它维护的是收敛协议，不是一份新权限数据库

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base`提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。这一版的 `PermissionPolicyService`位于 system_server，却同时读写几个由不同模块拥有的状态：

| 状态平面 | 权威状态 | 本类做什么 |
|---|---|---|
| permission 事实 | PMS 的 grant 与 flags；shared UID成员共用 `PermissionsState` | 读取，不另存副本 |
| AppOps 政策 | AppOpsService 的 UID mode 与 package mode | 从 permission事实派生并调用内部 setter |
| 生命周期门 | `mIsStarted`与两组 scheduled marker | 决定何时接收事件、怎样合并任务 |
| Controller 工作 | runtime数据库升级、user-sensitive计算 | 发起请求并等待或仅投递 |
| 相邻合同 | Role旧 action门、one-time session | 只接触边界，不拥有完整状态机 |

因此“同步完成”必须带主语。permission 已写、AppOps 内存 mode 已写、AppOps XML 已提交、user-sensitive 已算完、初始化 callback 已返回，是五个不同完成点。源码没有跨 PMS、PermissionController 和 AppOpsService 的共同事务；变化期间短暂不一致是设计的一部分，部分 r48 边界甚至会留下不能靠当前事件自动修复的旧状态。

阅读本章时采用三步法：先确认触发入口和执行线程，再确认候选 mode 怎样产生，最后确认 setter究竟改了 UID 层、package 层，还是被外部门禁返回值遮住而没有落下持久政策。

## 2. 服务先发布内部接口，再安装四组长期观察者

SystemServer在 PMS `systemReady()`之前启动 `PermissionPolicyService`。构造器立刻把 `PermissionPolicyInternal`放入 LocalServices；`onStart()`随后取得 `PackageManagerInternal`、`PermissionManagerServiceInternal`与 `IAppOpsService`，并安装长期监听。它没有公开自己的 Binder 服务。

四组触发源不要混成一条：

1. `PackageListObserver`接收包 added、changed、removed，用于 permission→AppOps 同步及 APPOP permission残余清理；
2. PMS 的 runtime-permission listener在 grant、revoke或相关 runtime flags变化后触发包同步；
3. 同一个 `IAppOpsCallback`被注册到 runtime permission的 switch op、soft-restricted extra op和一部分 APPOP-protection op；
4. 另一个 PACKAGE_ADDED/PACKAGE_CHANGED 广播 receiver只更新 user-sensitive flags，不参与核心 mode候选计算。

此外，`onStart()`还单独给 `Process.myUserHandle()`安排一次 60 秒后的全量 user-sensitive 更新。它是 system user 的一次异步任务，不是每个用户启动时都建立的定时器。

### 练习 1：把发布、依赖与四组观察者画在一张图上

先标出 SystemServer 启动与 PMS `systemReady()`的先后，再分别给包观察、permission监听、AppOps监听和 sensitive广播画出目的地。不要把相同 PACKAGE action 当成同一条业务链。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mSystemServiceManager.startService(PermissionPolicyService.class);' frameworks/base/services/java/com/android/server/SystemServer.java
grep -n -F 'LocalServices.addService(PermissionPolicyInternal.class, new Internal());' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'packageManagerInternal.getPackageList(new PackageListObserver() {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'permissionManagerInternal.addOnRuntimePermissionStateChangedListener(' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mAppOpsCallback = new IAppOpsCallback.Stub() {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'getContext().registerReceiverAsUser(new BroadcastReceiver() {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'FgThread.getHandler().postDelayed(manager::updateUserSensitive,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'ModeCallback cb = mModeWatchers.get(callback.asBinder());' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (switchOp != AppOpsManager.OP_NONE) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (callback.mWatchedOpCode == ALL_OPS) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'notifyOpChanged(clonedCallbacks,  code, uid, null);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 3. 包事件同步执行，permission 与 AppOps 事件才二次排队

旧的“主要工作都跑在 FgThread”模型不成立。执行方式按入口分裂：

| 入口 | started 门 | 实际工作 |
|---|---|---|
| package added | 是 | 在 observer 当前线程直接同步目标包和 shared UID成员 |
| package changed | 是 | 先直接同步，再直接做 UID 的 APPOP残余清理 |
| package removed | 是 | 直接做 UID残余清理，不再读取已删包 |
| runtime permission变化 | 是 | PMS 已先投 FgThread；PPS listener再投一次去重任务 |
| AppOps mode变化 | 是 | callback分别安排 package/user同步与 UID清理 |
| `onStartUser()` | 自己建立门 | 在生命周期调用线程同步完成升级等待和全用户扫描 |

异步 package任务以 `(packageName,userId)`去重，UID清理以完整 UID去重。两者都在任务真正开始时先删除 marker，所以执行期间再来一次事件可以排下一轮；这是一种尾部变化收敛机制，不是对某个状态快照的互斥保护。

`isStarted()`检查发生在取得 scheduled 集合锁之前。用户可能在检查后进入 stopping，任务仍会入队；工作入口只移除 marker，不重新检查 started。`onStopUser()`又只删除 `mIsStarted`，不取消 FgThread消息、不清 scheduled集合。因而“stop 返回”不保证旧任务停止。包 observer的同步路径也根本不享受这两组异步去重。

### 练习 2：构造 stop 与排队任务交错

画两条时序：一条让 permission callback通过 started检查后立刻 stop，另一条让任务开始、删除 marker后再到一个同包事件。验证第一条仍执行一次，第二条可再排一轮。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'this::synchronizePackagePermissionsAndAppOpsAsyncForUser);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsPackageSyncsScheduled.add(new Pair<>(packageName, changedUserId))' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsPackageSyncsScheduled.remove(new Pair<>(packageName, userId));' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsUidSyncScheduled.put(uid, true);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsUidSyncScheduled.delete(uid);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsStarted.delete(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'PermissionManagerService::doNotifyRuntimePermissionStateChanged,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

## 4. 一个 callback 监听很多 op，但回调里的 op 未必是实际变化项

`onStart()`对每个 runtime permission调用 `getSwitchOp(permission)`后注册；没有 AppOp映射时会得到 `OP_NONE`。soft-restricted permission还可能注册 extra op。APPOP-protection permission则用直接映射的 op注册。相同 switch op可被多次请求，但 AppOpsService 的索引集合会把同一 callback wrapper去重。

r48 的 AppOpsService以 callback Binder为主键，只在第一次注册时创建 `ModeCallback`；后续注册把同一个 wrapper加入更多 op索引，却不更新 wrapper里保存的 `mWatchedOpCode`和 flags。于是：

- 若首次是具体 op，后续其他索引触发时仍可能向 callback报告首次 op；
- 若首次是 `OP_NONE + null package`，那次注册本身没有通知来源，但 wrapper记录为 ALL_OPS；后续加入具体索引后，一次 switch变化可展开为多个 switched code回调；
- PermissionPolicy的 `opChanged()`完全不使用 `op`参数，只按 UID和 package安排重算，所以这一首注册黏连没有直接改变它的目标选择，却会改变回调次数。

AppOps user restriction变化又用 `UID_ANY`与 null package通知 op-indexed watcher。这里无法给 PermissionPolicy一个真实受影响包：负 UID被换算到 system user，null package任务通常在包查询处结束，UID清理也无法枚举该哨兵 UID。restriction本身仍直接参与 AppOps裁决，但不能据此声称 watcher一定重新计算了所有相关持久 mode。

r48 的 mode watcher入口源码还没有特权权限保护。这里的调用者是 system_server内服务，不受影响；这个事实也不能反推公开入口已经有同等授权门。

## 5. 用户初始化包含两套升级者，started 不是最终完成标志

`onStartUser(userId)`先查 started，再调用 `grantOrUpgradeDefaultRuntimePermissionsIfNeeded()`。r48 的“默认授权与升级”实际分在两边：

- PMS `systemReady()`先找出 upgrade-needed用户，仍由 system_server内的 `DefaultPermissionGrantPolicy.grantDefaultPermissions()`执行默认授权；
- PermissionController 的同名入口先调用一个空的 default-grant占位方法，实际随后执行 `RuntimePermissionsUpgradeController.upgradeIfNeeded()`完成 runtime permission数据库升级。

所以不能把所有默认权限授予都归给可更新 Controller。PPS等待的是 Controller这次 grant/upgrade请求的 Boolean结果；失败会记录严重错误、让本地 future异常完成，并在 started置位之前抛出 `IllegalStateException`。

成功后的顺序是：投递 user-sensitive全量更新、更新 runtime permission fingerprint、在 `mLock`下写 `mIsStarted=true`并取得单个 callback、同步做全用户 permission→AppOps扫描，最后调用 callback。`isInitialized(userId)`只是返回当前 started值，所以它在首轮 AppOps扫描之前已经为 true，用户 stopping后又会变回 false。

还有一个失败窗口：started在全量同步之前置位。如果构造 synchroniser、查询包或写 AppOps时抛异常，callback不会执行，但 started保留为 true；随后再次进入 `onStartUser()`会直接返回。源码里的幂等还依赖生命周期调用串行，因为最初的 get与后面的 put不是一个临界区。

## 6. 名义 60 秒超时、main looper 与 fingerprint 是三种不同保证

本地 `AndroidFuture`使用无超时参数的 `future.get()`。但其下层 `PermissionControllerManager`把 request timeout设为 60 秒，`ServiceConnector.CompletionAwareJob`会调用 `orTimeout()`；所以简单说“完全没有超时”也不准确。

关键在线程：r48 `AndroidFuture`默认把 timeout消息发给进程 main Handler。system user在 AMS启动期间由 system_server main同步进入 `SystemServiceManager.startUser()`，boot phase补扫也在主启动路径调用；此时同一 main线程阻塞在 `future.get()`，用于解除等待的 timeout消息可能永远得不到执行。普通 secondary user的 `USER_START_MSG`则运行在 AMS自己的 ServiceThread，main仍可在约 60 秒触发 timeout。正常 Controller结果由 FgThread executor完成 future，两条路径都能被真实结果唤醒。

Controller成功后调用 `updateUserSensitive()`只把远端任务投递出去，PPS不等它完成就更新 fingerprint。`updateRuntimePermissionsFingerprint()`也只是改内存 fingerprint并安排 runtime-permission文件异步写。因此：

- fingerprint更新不证明 user-sensitive成功；后者失败只在 manager链记录日志；
- 方法返回不证明 fingerprint已经落盘，崩溃后仍可能重做；
- `Settings.isPermissionUpgradeNeeded()`读的是启动时计算的 `mPermissionUpgradeNeeded`缓存，不是每次现比字符串；更新 fingerprint没有把该缓存改为 false。若本次启动最初为 true，同一 boot里 stop后再 start仍可重复走 Controller升级。

### 练习 3：证明超时能否发生取决于调用线程

把外层等待、ServiceConnector的 60 秒任务、AndroidFuture的 main Handler与两种用户启动线程连起来。分别回答 Controller永不完成时 system user和普通 secondary user会怎样。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'future.get();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'private static final long REQUEST_TIMEOUT_MILLIS = 60000;' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'protected long getRequestTimeoutMs() {' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'orTimeout(requestTimeout, TimeUnit.MILLISECONDS);' frameworks/base/core/java/com/android/internal/infra/ServiceConnector.java
grep -n -F 'private @NonNull Handler mTimeoutHandler = Handler.getMain();' frameworks/base/core/java/com/android/internal/infra/AndroidFuture.java
grep -n -F 'mSystemServiceManager.startUser(t, currentUserId);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mInjector.getSystemServiceManager().startUser(TimingsTraceAndSlog.newAsyncLog(),' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'return mPermissionUpgradeNeeded.get(userId, true);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mFingerprints.put(userId, mExtendedFingerprint);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mDefaultPermissionGrantPolicy.grantDefaultPermissions(userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'performDefaultPermissionGrants();' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
grep -n -F 'RuntimePermissionsUpgradeController.INSTANCE.upgradeIfNeeded(this, () -> {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
grep -n -F 'mIsStarted.put(userId, true);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'synchronizePermissionsAndAppOpsForUser(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'callback.onInitialized(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 7. initialized callback 会引出下一轮，而 user-sensitive 是另一条松耦合链

r48 树中 `PermissionPolicyInternal`的 initialized callback只有一个字段，后注册者覆盖前者，也不会给已经 initialized的用户重放。唯一实际注册者是 PMS：PPS首轮 AppOps同步后调用它，PMS再执行一次 `updateAllPermissions()`。

这次 PMS再评估很重要。PMS在处理 hard/soft restricted permission时用 `isInitialized(userId)`决定是否真正应用 restriction；callback里的全量更新可能刚好改变 grant或 flags，随后 PMS把 runtime状态变化先投到 FgThread，PPS listener又投递自己的 package同步。于是实际闭环可以是：

`started=true → 首轮 AppOps同步 → initialized callback → PMS重评 restriction → runtime通知 → 后续 AppOps同步`

因此 callback到达或返回都不是这条二次收敛的强屏障。外部消费者也不能用“先查 `isInitialized()`，否则注册 callback”拼出无竞态协议：检查与单槽注册并非原子操作，且会覆盖 PMS的固定回调。

user-sensitive广播链更独立。receiver虽注册到 `UserHandle.ALL`，却用服务默认 context读取 `Settings.Secure.USER_SETUP_COMPLETE`，没有按广播 user显式读取。setup被判断为未完成时，来自所有用户的 UID进入同一个列表；它没有 ContentObserver或定时唤醒，只在以后又收到一个相关包广播且这次判断为完成时才批量刷新。

刷新阶段会从完整 UID得到 `UserHandle`，按 user缓存 `PermissionControllerManager`，所以真正 `updateUserSensitiveForApp(uid)`仍投向对应用户。可是 receiver不查 started，`onStopUser()`也不清待处理 UID或 manager缓存。onStart末尾那次延迟全量更新又只针对 system user。两套链各有自己的生命周期缺口。

### 练习 4：区分 initialized、sensitive投递和二次同步

先顺着唯一 callback找到 PMS 的 `updateAllPermissions()`，再构造 secondary user尚未setup、system context却已setup的广播。记录每一步只是布尔置位、同步调用、消息入队还是远端完成。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'permissionPolicyInternal.setOnInitializedCallback(userId -> {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.updateAllPermissions(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionPolicyInternal.isInitialized(userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Settings.Secure.getInt(getContext().getContentResolver(),' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mUserSetupUids.add(uid);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mUserSetupUids.clear();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'UserHandle user = UserHandle.getUserHandleForUid(uid);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'manager.updateUserSensitiveForApp(uid);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'service.updateUserSensitiveForApp(uid, future);' frameworks/base/core/java/android/permission/PermissionControllerManager.java
```

## 8. 全量和单包扫描都以 user 为范围，shared UID却是共同计算单元

全量同步创建一个目标 user context的 `PermissionToOpSynchroniser`，然后让 `PackageManagerInternal.forEachPackage()`在 PMS package锁下枚举全局解析包。回调阶段只做 `addPackage()`收集；离开枚举后才执行 `syncPackages()`并调用 AppOps，避免拿着 package锁直接进入 AppOps setter。不过 soft-restriction收集本身仍可能查询 StorageManager等同进程服务，不能把 add阶段想成一次纯内存复制。

单包同步先取目标 user的 `PackageInfo`，加入目标包，再取得该 user已安装的 shared-user包数组并逐个加入。PMS返回的 shared数组包含目标包本身，所以 shared UID路径会重复收集目标。ALLOW候选随后仍会逐项执行；FOREGROUND与IGNORE重复项会被 `(uid,op)`键抑制，条件 IGNORE则要等某次 setter真正返回 true后才占键。

同一 shared UID成员的同名 permission grant与 flags来自共享 `PermissionsState`，不存在“A包授予同名权限、B包撤销同名权限”这种独立状态。冲突候选应来自不同 permission映射到同一 switch op，或 soft restriction依据包属性产生的不同 extra-op意图。

`addPackage()`同时取得用户态 `PackageInfo`和内部 `AndroidPackage`，任一为空便跳过；包在两次读取间更新时，两份对象也可能来自不同代际。root UID与 system UID为兼容直接跳过。它遍历的是解析后的 `requestedPermissions`：旧 target的兼容权限和 split permission会在解析时加入，不能把这个数组严格等同于 XML里逐字声明的集合。

### 练习 5：用真实 shared-permission模型重做冲突题

令两个 shared成员分别请求两个不同 permission，这两个 permission映射到同一 switch op，且共享状态中一项可授、另一项不可授。再对照目标包重复加入与解析期隐式权限，预测候选列表，而不是给同名 permission虚构逐包 grant。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'synchroniser.addPackage(pkg.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'getSharedUserPackagesForPackage(' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (ps.getInstalled(userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'final PermissionsState permissionsState = ps.getPermissionsState();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'pkg.addRequestedPermission(npi.name)' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'pkg.addRequestedPermission(perm)' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'pi.requestedPermissions[i] = perm;' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'if (uid == Process.ROOT_UID || uid == Process.SYSTEM_UID) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 9. 主 AppOp 候选由 grant、flags、restriction 与背景权限共同产生

Synchroniser构造时缓存所有 dangerous定义，以 permission name查 `PermissionInfo`。每个包只处理 requested集合中能在该 map找到的名字；主 AppOp路径还要求 `permissionInfo.isRuntime()`。

主路径的计算顺序如下：

| 门 | 不通过时的结果 | 通过后 |
|---|---|---|
| `FLAG_PERMISSION_REVIEW_REQUIRED` | 本包这个 permission的主 op不产生候选 | 查 switch op |
| permission→op→switch | `OP_NONE`时不产生主候选 | 进入可授判断 |
| 当前 user grant | false | IGNORE候选 |
| `FLAG_PERMISSION_REVOKED_COMPAT` | true | IGNORE候选 |
| hard restriction的 APPLY位 | 应用中 | IGNORE候选 |
| soft policy `mayGrantPermission()` | false | IGNORE候选 |
| 前景 permission可授 | true | 再看是否声明 background permission |
| 没有 background permission名 | 前景可授 | ALLOW候选 |
| background定义存在且也可授 | true | ALLOW候选 |
| background名存在，但定义缺失或不可授 | 前景仍可授 | FOREGROUND候选 |

`shouldGrantAppOp()`检查 background permission时会读它自己的 grant、REVOKED_COMPAT和 hard/soft规则，但不会再次应用主路径外层的 REVIEW_REQUIRED早退。多个 permission共享 switch op时，各自产生候选，稍后再按优先级合并。

soft-restricted extra op是独立调用：即使主 op因为 REVIEW_REQUIRED已返回，`addExtraAppOp()`仍会根据 `mayAllowExtraAppOp()`与 `mayDenyExtraAppOpIfGranted()`产生 ALLOW、IGNORE或条件 IGNORE候选。它不是 background permission的独立 AppOp；具体存储兼容公式留到下一章展开。

### 练习 6：分别计算主 op 与 extra op

给一个 soft-restricted permission设置 REVIEW_REQUIRED，再分别计算主候选和 extra候选。随后去掉 review位，依次改变 grant、REVOKED_COMPAT、APPLY_RESTRICTION与背景 grant，验证表中每个分支。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'addPermissionAppOp(packageInfo, pkg, permissionInfo);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'addExtraAppOp(packageInfo, pkg, permissionInfo);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean isReviewRequired = (permissionFlags & FLAG_PERMISSION_REVIEW_REQUIRED) != 0;' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'int appOpCode = getSwitchOp(permissionName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean shouldGrantAppOp = shouldGrantAppOp(packageInfo, pkg, permissionInfo);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean shouldGrantBackgroundAppOp = backgroundPermissionInfo != null' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean isRevokedCompat = (permissionFlags & FLAG_PERMISSION_REVOKED_COMPAT)' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (permissionInfo.isHardRestricted()) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (policy.mayDenyExtraAppOpIfGranted()) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 10. ALLOW→FOREGROUND→IGNORE 是候选优先级，不是最终状态定理

`syncPackages()`维护四张候选表：ALLOW、FOREGROUND、IGNORE、IGNORE_IF_NOT_ALLOWED。前三者表达确定目标，最后一类只在当前 raw结果不是 ALLOWED时尝试压到 IGNORED，以免 soft storage策略夺走已有的允许。

执行顺序确实是 ALLOW→FOREGROUND→IGNORE→条件 IGNORE，去重键为 `IntPair(uid,op)`，不含 packageName。可是去重要精确描述：

- ALLOW循环不查 key，所有 ALLOW候选都会执行，并在每次执行后写 key；
- FOREGROUND与IGNORE若发现任何前序候选已占 key便跳过；
- 条件 IGNORE最后执行，只有 setter返回 true才占 key；当前 raw为 ALLOWED时返回 false。

因此它表达“同一 UID/op候选中更宽者优先”。packageName仍保存在 `OpToChange`里，因为 raw检查与可能的 package层清理需要一个具体包。这个列表顺序并不能证明 shared UID每个包最终的 effective mode都等于最宽候选；下一节的读层和写层不对称会打破这个推论。

### 练习 7：逐循环标出谁会跳过去重

为同一 `(uid,op)`依次放入两个 ALLOW、一个 FOREGROUND、一个 IGNORE和两个条件 IGNORE。不要只看注释，按四个循环实际的 key检查位置写出调用次数。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'LongSparseLongArray alreadySetAppOps = new LongSparseLongArray();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int allowCount = mOpsToAllow.size();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'setUidModeAllowed(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int foregroundCount = mOpsToForeground.size();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (alreadySetAppOps.indexOfKey(IntPair.of(op.uid, op.code)) >= 0) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'setUidModeIgnored(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean wasSet = setUidModeIgnoredIfNotAllowed(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'alreadySetAppOps.put(IntPair.of(op.uid, op.code), 1);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 11. unsafeCheckOpRaw 不是纯存储读取，合成值会遮住应该写的 UID 层

`unsafeCheckOpRaw()`只保证不把 `MODE_FOREGROUND`按当前 UID状态动态评价。它仍会经过 CheckOpsDelegate、UID/package身份核对、package suspend与 user restriction，然后才按 UID覆盖→package mode→默认值返回。把它叫作“持久政策读取”会漏掉最危险的 r48边界。

普通 `setUidMode()`先用这个合成 raw值比较 `oldMode`与目标；相同便完全不写 UID层。若不同，才调用 PermissionPolicy专用 UID setter，随后仍用同一包再查 raw；仍不等才把这个候选包的 package mode恢复默认。条件 IGNORE则没有后二次 package清理。

考虑两个 shared成员 A、B都产生同一 `(uid,op)`的 IGNORE候选：UID层没有覆盖，op默认 ALLOWED，A恰有 package-level IGNORED，B没有。若 A先进入 IGNORE循环，raw已等于目标，UID setter被跳过；循环仍把 key视作已处理，B候选被去重跳过，B继续得到 ALLOWED。这里候选优先级正确，最终 shared成员状态却没有统一。

restriction还能制造更隐蔽的版本：permission已撤销、底层仍保存 ALLOWED，但 active restriction先让 raw返回 IGNORED，于是目标 IGNORE被当作 no-op。解除 restriction只产生 `UID_ANY/null`通知，PermissionPolicy不能由该参数枚举原包，旧 ALLOWED可能重新显露。package suspend也会遮住 raw；其解除是否补救要看针对包的 suspend通知链，不能靠本 setter保证。

反向地，在没有 delegate、外部门禁与并发写干扰的典型路径上，写入非默认 UID mode后，UID覆盖会遮住 package override，二次 raw等于目标，package清理不会运行；目标等于真实默认且 UID覆盖不存在或被删除时，当前候选包的旧 package mode才会重新显露并被清除。若 suspend或 restriction令二次 raw仍不等于非默认目标，也可能触发 package-default清理。ALLOW不去重能帮助逐候选包检查，但不请求相关 permission的 shared成员仍可能留有隐藏覆盖。

### 练习 8：手算 raw 遮蔽与 package 残余

分别构造 package override遮蔽、user restriction遮蔽和非默认 UID覆盖遮蔽。每一步写出“外部门禁结果、UID key、package mode、raw返回、setter是否真正执行”，不要只写一个最终 mode。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final int oldMode = mAppOpsManager.unsafeCheckOpRaw(AppOpsManager.opToPublicName(' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (oldMode != mode) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int newMode = mAppOpsManager.unsafeCheckOpRaw(AppOpsManager.opToPublicName(' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (newMode != mode) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mAppOpsManagerInternal.setModeFromPermissionPolicy(opCode, uid, packageName,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (isOpRestrictedDueToSuspend(code, packageName, uid)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (isOpRestrictedLocked(uid, code, packageName, bypass)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return raw ? rawMode : uidState.evalMode(code, rawMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return raw ? op.mode : op.evalMode();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (!requestedPermissions.contains(appOpPermission)) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (appOpMode != defaultAppOpMode) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 12. callbackToIgnore 只压本次自回声，setter 返回也不等于跨模块提交

PermissionPolicy通过 `AppOpsManagerInternal.setUidModeFromPermissionPolicy()`和 `setModeFromPermissionPolicy()`传入同一个 `mAppOpsCallback`。在 UID setter里，AppOpsService看到非 null policy callback会跳过普通 `updatePermissionRevokedCompat()`，避免“permission推 UID mode”又反改 compat flag；package setter本来就没有这一步。两种 setter组装本次 mode watcher集合时都会删除该 Binder对应的 wrapper，阻断直接的写→回调→再写环。

这个机制只有单次 setter范围：它不取消之前已排的 callback，不阻止其他 writer并发改 mode，也不抑制其他 watcher、本地 StorageManager通知或 raw读取顺带提交到期 UID state所产生的 foreground通知。任务开始即释放 scheduled marker，所以同步期间的新 permission或外部 AppOps事件仍能排下一轮。

一次 `syncPackages()`又可能连续写很多 UID/op。前几个 setter成功、后一个查询或写入抛异常时，没有回滚前半结果。setter返回最多说明相应 AppOps内存路径已经返回；第270章的 fast/ordinary write、异步 watcher与 `appops.xml`提交仍有各自完成点。

`mIsStarted`、PMS permission状态和 AppOps mode也没有共同锁。它们依靠事件、幂等比较和后续扫描趋近一致，而不是在 callback被忽略后获得隔离性。

## 13. APPOP-protection permission 的无人请求清理是另一套算法

`mAppOpPermissions`来自带 `PROTECTION_FLAG_APPOP`且能映射 AppOp的 permission。r48明确排除 `ACCESS_NOTIFICATIONS`、`MANAGE_IPSEC_TUNNELS`和 `REQUEST_INSTALL_PACKAGES`；最后一项保留 Settings独立控制非默认 op的产品合同。它不是前面 dangerous runtime候选表。

清理入口取得 UID在对应 user下的全部 package，合并每个包解析后的 requestedPermissions。只要一个 shared成员仍请求该 APPOP permission就不清。若无人请求，则对每个成员查询 effective raw；只在它不等于该 op真实默认值时，先把 UID mode设回默认，再把当前包的 package mode也设回默认。

这个顺序能在第一包处清 UID覆盖，并让后续包暴露各自覆盖；但它仍用非纯存储 raw。若 suspend/restriction返回值恰等于 op默认值，底层非默认 mode可被遮住而跳过。UID已没有 package时函数直接返回，完整 UID删除依赖 AppOpsService的 `uidRemoved()`等其他生命周期路径。

package changed/removed调用的是同步清理，而 AppOps callback走带 UID marker的异步版本。同步入口也会先删除同一 marker，却不会移除已在 FgThread队列中的旧消息，所以同一 UID可能重复扫描。三个排除项、当前请求并集、raw门和两层 setter必须分别记录，不能把函数名理解为“清除该 UID全部 AppOps”。

## 14. PermissionPolicyInternal 只提供三项内部合同，Role 只是旧 action 迁移门

内部接口只有 `isInitialized()`、单槽 initialized callback和 `checkStartActivity()`。前两项服务于 PMS限制权限再评估；它们不是公开等待协议，也没有历史重放、多 listener或一次初始化永久为真的语义。

ActivityStarter把 permission检查、IntentFirewall与 `checkStartActivity()`结果用 abort合并。PermissionPolicy只特殊处理两个旧 action：修改默认拨号器、修改默认短信应用。callingPackage非 null且能查到的应用若 targetSdk≥Q，策略分支返回 false；它本身不构造或抛出 SecurityException，上层把 false并入 abort、取消结果并返回 `START_ABORTED`。

targetSdk<Q时仍允许，并把 `Intent.EXTRA_CALLING_PACKAGE`写回原 Intent，供后续 RequestRoleActivity识别来源。两个放行边界也要看到：callingPackage为 null时完全跳过检查；按 callingUid所属 user查询 ApplicationInfo失败时只记日志，随后仍写 extra并允许。这个内部方法本身不验证 package确属 callingUid，依赖 Activity启动上游提供可信组合。

这里没有查询 Role holder、评估资格或授予 Role特权。Q及以上应用应改走 `RoleManager.createRequestRoleIntent()`，只是 API迁移要求；RoleController、PermissionController和其他策略才拥有后续选择与授权。

## 15. one-time 到期会先由 Controller直接改权限与 AppOps，PPS只是后续收敛层

一次性授权的 session不是 `PermissionPolicyService`持有。PermissionController的 `AppPermissionGroup.persistChanges()`在 permission带 one-time且仍获授时调用 `PermissionManager.startOneTimePermissionSession()`；PMS按 user懒创建 `OneTimePermissionUserManager`，后者以完整 UID保存 listener并监听 importance、alarm和 UID gone。

到期函数先把 session标记 finished并取消 alarm，再向 `mHandler`排入通知 runnable；当前调用线程随后注销三类 importance listener，并从 map删除该 UID。入队早于后两项清理，但 runnable执行可与它们交错，尤其 UID importance入口本就可能来自线程池，不能把“通知执行”和“清理完成”排成严格全序。runnable最终经 ServiceConnector提交 oneway通知，没有等待“权限已撤销”的回执。

PermissionController收到通知后找出 one-time permission group，撤销 runtime permission、清 USER_SET并 `persistChanges()`。这次 persist本身会调用 PackageManager grant/revoke与 flags API，也会直接对 `permission.affectsAppOp()`的项执行 allow/disallow AppOp。前一组 Binder调用内部就可能让 PMS把 runtime-state通知异步投到 FgThread，与 Controller对当前或后续 permission的直接 AppOps写交错；PermissionPolicy最终再按 shared UID、restriction和背景权限做一般收敛。

所以至少要区分四个里程碑：session已判 finished、handler/ServiceConnector/oneway提交已经发生、Controller的 permission/AppOp写入返回、PPS后续重算返回。它们不是一条严格全序；PPS不是 one-time计时器，也不是这条链第一次改变 AppOp的唯一组件。

### 练习 9：把 Role门和 one-time闭环放回各自所有者

为旧默认应用 action标出“允许/阻止/Intent被改写”，再为 one-time标出 session、Controller、PMS和 PPS。检查哪一步有回执，哪一步只是 oneway或异步通知。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public boolean checkStartActivity(@NonNull Intent intent, int callingUid,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'case TelecomManager.ACTION_CHANGE_DEFAULT_DIALER:' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (applicationInfo.targetSdkVersion >= Build.VERSION_CODES.Q) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'intent.putExtra(Intent.EXTRA_CALLING_PACKAGE, callingPackage);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'abort |= !mService.getPermissionPolicyInternal().checkStartActivity' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
grep -n -F 'return START_ABORTED;' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
grep -n -F 'startOneTimePermissionSession(packageName,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mHandler.post(' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mActivityManager.removeOnUidImportanceListener(mStartTimerListener);' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mListeners.remove(mUid);' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mPermissionControllerManager.notifyOneTimePermissionSessionTimeout(' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'oneway interface IPermissionController {' frameworks/base/core/java/android/permission/IPermissionController.aidl
grep -n -F 'group.revokeRuntimePermissions(false);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
grep -n -F 'group.persistChanges(false, ONE_TIME_PERMISSION_REVOKED_REASON);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
grep -n -F 'shouldKillApp |= allowAppOp(permission, uid);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'shouldKillApp |= disallowAppOp(permission, uid);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'notifyRuntimePermissionStateChanged(packageName, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

## 16. 用完成点矩阵收束排障，并把下一章边界留清楚

面对“permission已经改了，AppOp为什么还不对”，先按下面的矩阵定位，不要直接读 `appops.xml`猜内存：

| 已观察到 | 可以证明 | 仍不能证明 |
|---|---|---|
| Controller success callback | 本次 runtime升级请求报告成功 | sensitive完成、fingerprint落盘、started置位 |
| fingerprint API返回 | 内存值已换并安排异步写 | cached upgrade gate已关闭、文件已提交 |
| `mIsStarted=true` | 事件门与 PMS restriction门已打开 | 首轮 AppOps同步完成 |
| `syncPackages()`返回 | 本轮候选 setter均已返回 | 所有 shared包 effective mode最宽、XML已写 |
| initialized callback返回 | PMS本次 `updateAllPermissions()`返回 | 它触发的异步 permission→AppOps任务已完成 |
| `onStopUser()`返回 | started位已删除 | 旧 Fg任务、watcher、sensitive缓存已清 |

排查 mode时再拆四层：permission事实是否共享、候选是否被 REVIEW或 soft规则跳过、raw是否被 suspend/restriction/package层遮住、目标 setter是否真的改了 UID key。候选顺序、实际 effective mode与持久化提交是三件事。

第271章的核心结论由此很清楚：PermissionPolicyService把多个权威模块编排成事件驱动的收敛协议，但 r48 的线程、缓存、raw门和分层 mode使“初始化”“最宽”“忽略自回调”都只能在限定范围内成立。下一章进入 `SoftRestrictedPermissionPolicy`，具体拆开存储权限兼容、shared UID最小 targetSdk、legacy external storage与 extra AppOp三套不对称规则。
