# 253 Android PackageInfo、ApplicationInfo、PackageUserState与AppsFilter查询可见性链

## 1. 主问题：一次包查询为什么会有五种合法结果

`getPackageInfo()` 看起来像“按包名查表”，实际却是一条带调用者身份、目标用户、旗标、包形态和用户态的裁决链。同一个目标包在不同上下文中，可能合法得到：

1. 字段与所请求组件都齐全的 `PackageInfo`；
2. 只有 Settings 持久信息的最小 `PackageInfo`；
3. 顶层对象存在，但组件数组或可选字段被裁掉的结果；
4. PMS 为隐藏“目标是否存在”而返回的 `null`，应用侧包装层再把它转成 `NameNotFoundException`；
5. 因跨用户权限失败而抛出的 `SecurityException`。

最实用的心智模型不是一张 `Map<String, PackageInfo>`，而是五道先后相接的门：

`入口与用户门 → 名字/对象路由门 → 静态库与 instant 包门 → AppsFilter 可见性门 → PackageUserState 与字段投影门`

前四道门决定“能不能看见哪一个对象”，最后一道门决定“这个对象针对该用户、这些 flags 应长成什么样”。返回 `null` 时，调用方通常无法区分包确实不存在、版本没命中、静态库不可见、instant 规则拒绝、AppsFilter 拒绝或用户态不匹配；这正是防枚举设计的一部分。

本文固定在 Android 11 / API 30 / `android-11.0.0_r48`，以当前源码树中的以下文件为坐标：

- `frameworks/base/core/java/android/app/ApplicationPackageManager.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java`
- `frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java`
- `frameworks/base/core/java/android/content/pm/PackageUserState.java`
- `frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java`
- `frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java`

本章范围止于按名字查询 `PackageInfo` / `ApplicationInfo` 的链路；Intent 候选怎样收集、匹配与排序留给第 254 章。这样可以把“包是否可见”与“哪个组件能处理 Intent”两类问题分开。

## 2. 公共入口：先固定用户，再把“可见性身份”送入内部查询

公开的 `getPackageInfo()` 与 `getPackageInfoVersioned()` 都把当下的 `Binder.getCallingUid()` 作为 `filterCallingUid` 交给 `getPackageInfoInternal()`。内部入口依次做三件事：

- 目标用户不存在，直接返回 `null`；
- 用 `updateFlagsForPackage()` 补齐或校验 flags；
- 用当前 Binder 身份执行跨用户权限检查，然后在 `mLock` 下做名字解析与对象路由。

这里必须区分服务端与 SDK 表面。PMS 找不到目标、看不见目标、状态不匹配，通常都折叠成 `null`；`ApplicationPackageManager.getPackageInfoAsUser()` 收到该空值后抛 `NameNotFoundException`。跨用户访问无权限则由 PMS 的 `enforceCrossUserPermission()` 抛 `SecurityException`。因此系统内调用不能把所有空结果都解释为未安装，应用调用也不能把 `NameNotFoundException` 解释成“磁盘上一定没有这个包”。

`getApplicationInfoInternal()` 的常规跨用户检查还有一个窄例外：Recents 以受信任身份访问 child profile 时跳过这一处 `enforceCrossUserPermission()`。`getPackageInfoInternal()` 没有对应的同形分支；两类入口不能仅凭名字相近就合并。

`filterCallingUid` 的注释还揭示了一个内部调用约定：可信代码可能先保存原调用者 UID，再清除 Binder 身份，并把原 UID传进来做可见性过滤。它不是任意客户端可伪造的参数，而是 system_server 内部保留原始访问主体的通道。

### 练习 1：钉住入口、用户门与内部身份参数

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public PackageInfo getPackageInfo(String packageName, int flags, int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return getPackageInfoInternal(packageName, PackageManager.VERSION_CODE_HIGHEST,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!mUserManager.exists(userId)) return null;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags = updateFlagsForPackage(flags, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.enforceCrossUserPermission(Binder.getCallingUid(), userId,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'packageName = resolveInternalPackageNameLPr(packageName, versionCode);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Important: The provided filterCallingUid is used exclusively to filter out packages' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return generatePackageInfo(ps, flags, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public PackageInfo getPackageInfoAsUser(String packageName, int flags, int userId)' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'if (pi == null) {' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'throw new NameNotFoundException(packageName);' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'if (!isRecentsAccessingChildProfiles(Binder.getCallingUid(), userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

阅读时把六个输入写在纸上：请求名、版本、输入 flags、`filterCallingUid`、当前 Binder UID、`userId`。后续每次看到一次早退，都标明它消费的是哪一个输入，空结果就不会混成一团。

## 3. flags 规范化：跨用户权限与 Direct Boot 默认值不是一回事

`updateFlagsForPackage()` 先处理 `MATCH_ANY_USER`。只要调用者显式带了这个位，就会要求跨用户权限；即使传入的 `userId` 恰好与调用者同用户，也不能靠“这次没有真的跨用户”绕过检查。

另有一个兼容分支：系统用户调用者带 `MATCH_UNINSTALLED_PACKAGES`，且系统用户拥有受管资料时，方法会补上 `MATCH_ANY_USER`。这个位是在显式 `MATCH_ANY_USER` 的权限分支之后添加的，方法内部不会回头重跑前一个检查；查询入口仍会针对目标 `userId` 执行一般的跨用户校验。分析权限时必须按真实控制流，而不是看到最终 flags 就倒推一定走过哪个分支。

随后 `updateFlags()` 处理 Direct Boot：

- 调用者已经指定 `MATCH_DIRECT_BOOT_AWARE` 或 `MATCH_DIRECT_BOOT_UNAWARE`，尊重其选择；
- 用户正在解锁或已解锁，默认同时匹配 aware 与 unaware；
- 用户仍锁定，默认只匹配 aware。

还有一个容易误读的常量：`MATCH_KNOWN_PACKAGES` 是 `MATCH_UNINSTALLED_PACKAGES | MATCH_ANY_USER` 的按位组合。源码多处使用 `(flags & MATCH_KNOWN_PACKAGES) != 0`，这表示两个组成位任意一个存在就能进入该分支，并不要求两位同时存在。

### 练习 2：验证 flags 的权限分支与 Direct Boot 补位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if ((flags & PackageManager.MATCH_ANY_USER) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'MATCH_ANY_USER flag requires INTERACT_ACROSS_USERS permission' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if ((flags & PackageManager.MATCH_UNINSTALLED_PACKAGES) != 0 && isCallerSystemUser' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags |= PackageManager.MATCH_ANY_USER;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & (PackageManager.MATCH_DIRECT_BOOT_UNAWARE' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mUserManager.isUserUnlockingOrUnlocked(userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags |= PackageManager.MATCH_DIRECT_BOOT_AWARE | MATCH_DIRECT_BOOT_UNAWARE;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags |= PackageManager.MATCH_DIRECT_BOOT_AWARE;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public static final int MATCH_KNOWN_PACKAGES = MATCH_UNINSTALLED_PACKAGES | MATCH_ANY_USER;' frameworks/base/core/java/android/content/pm/PackageManager.java
```

## 4. 名字与对象路由：同一个外部包名未必指向同一个内部对象

进入锁后，PMS 先调用 `resolveInternalPackageNameLPr()`。它先应用 renamed-package 映射，再判断归一化后的声明包名是否对应静态共享库的多个版本。若不是静态库，返回归一化名——它可能已经不同于请求名；若是静态库声明名，则只在调用者可见的版本中选择显式版本或最高版本，最后返回内部合成包名。这里还有两套不能混写的版本号：`VersionedPackage.versionCode` 用来匹配 declaring APK 的 long version code，调用包的 `usesStaticLibrariesVersions` 则匹配静态库自身的 long version。

解析完内部名，`getPackageInfoInternal()` 才按对象来源分流：

1. `MATCH_FACTORY_ONLY | MATCH_APEX` 直接向 `ApexManager` 请求 factory APEX；即使未命中也立即返回，不再尝试 factory APK；
2. `MATCH_FACTORY_ONLY` 的 APK 路径查 disabled system package；
3. 常规路径先查活动的 `mPackages`；factory-only 下若命中非 system 活动包会立即返回 `null`，活动 system 包仍可继续；
4. 仅在非 factory-only 且带任一 known-packages 组成位时，才可退到 Settings 中的历史/非活动记录；
5. 带 `MATCH_APEX` 且前面未命中 APK 时，再查 active APEX；
6. 全部未命中才返回 `null`。

APEX 是重要例外：上述两个 APEX 分支直接返回 `ApexManager` 的 `PackageInfo`，没有经过 APK 路径的静态库过滤、AppsFilter、`PackageUserState` 投影与 `generatePackageInfo()`。跨用户入口检查仍在，但不能把 APK 的每一道门机械套到 APEX 上。`ApexManager` 返回的是扫描阶段写入缓存的对象；缓存统一以 `GET_META_DATA | GET_SIGNING_CERTIFICATES | GET_SIGNATURES` 生成，本次包查询的 `GET_*` flags 不会让它重新裁剪。

`getApplicationInfoInternal()` 的路由相似而不完全相同：它有 active APK、APEX、`"android"`/`"system"` 特殊对象和 Settings 退路。它没有 disabled-system 的 `MATCH_FACTORY_ONLY` 分支；APEX 路径反而用 `MATCH_SYSTEM_ONLY` 在 active 与 factory APEX 之间选择。特殊平台对象可直接返回 `mAndroidApplication`；APEX 则从缓存的 APEX `PackageInfo` 取 `applicationInfo`。

### 练习 3：沿 renamed、静态库、factory、active、known 与 APEX 分支走一遍

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'String normalizedPackageName = mSettings.getRenamedPackageLPr(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'LongSparseArray<SharedLibraryInfo> versionedLib =' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (versionCode != PackageManager.VERSION_CODE_HIGHEST) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return highestVersion.getPackageName();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final boolean matchFactoryOnly = (flags & MATCH_FACTORY_ONLY) != 0;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mSettings.getDisabledSystemPkgLPr(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'AndroidPackage p = mPackages.get(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!matchFactoryOnly && (flags & MATCH_KNOWN_PACKAGES) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return mApexManager.getPackageInfo(packageName, ApexManager.MATCH_ACTIVE_PACKAGE);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & PackageManager.MATCH_SYSTEM_ONLY) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'apexFlags = ApexManager.MATCH_FACTORY_PACKAGE;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return mAndroidApplication;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int flags = PackageManager.GET_META_DATA' frameworks/base/services/core/java/com/android/server/pm/ApexManager.java
grep -n -F '| PackageManager.GET_SIGNING_CERTIFICATES' frameworks/base/services/core/java/com/android/server/pm/ApexManager.java
grep -n -F 'final PackageInfo packageInfo = PackageInfoWithoutStateUtils.generate(' frameworks/base/services/core/java/com/android/server/pm/ApexManager.java
grep -n -F 'mAllPackagesCache.add(packageInfo);' frameworks/base/services/core/java/com/android/server/pm/ApexManager.java
```

## 5. 静态共享库：版本解析与结果可见性是两次独立裁决

静态共享库不能只交给 AppsFilter。它有两道专门的门：

- `resolveInternalPackageNameLPr()` 决定声明名应落到哪个内部版本；
- `filterSharedLibPackageLPr()` 决定已经选中的那个库包是否能返回。

`MATCH_STATIC_SHARED_LIBRARIES` 不是对所有应用的万能开关。在 `filterSharedLibPackageLPr()` 这一层，system、shell、root，或持有 `INSTALL_PACKAGES` 的调用者只有同时带该 flag 才能绕过依赖检查；普通调用者仍只能看自己，或自己某个包以“库名 + 精确版本”声明依赖的静态库。依赖库名相同但版本不同，仍会被过滤。前一层的声明名/版本解析又只给 system、shell、root 无限制版本视野，`INSTALL_PACKAGES` 本身并不会让一个声明名自动解析出任意版本；这正说明两道门不能合并成一句“有权限就能查”。

AppsFilter 内部遇到 `targetPkg.isStaticSharedLibrary()` 会返回“不在这里过滤”，原因正是上层已经承担了专门检查。若只从 AppsFilter 的 `false` 推导“静态库对所有人可见”，结论会完全相反。

还要注意身份选择：内部名解析器读取当下的 `Binder.getCallingUid()`，专门的静态库过滤则使用 `filterCallingUid`。公共 Binder 调用中两者通常相同；可信代码清除身份后调用内部方法时，两者可能不同，因此“选中了哪个版本”和“原调用者能否看见该版本”必须分别记录。

## 6. PMS 可见性包装层：isolated UID、同应用与 instant 包先于 AppsFilter

`shouldFilterApplicationLocked()` 不是 AppsFilter 的同义词。它先做 instant-app 包装规则，再只把“普通已安装调用者查询普通已安装目标”的情况交给 AppsFilter：

- isolated UID 先映射回 `mIsolatedOwners` 中的拥有者 UID；
- `ps == null` 时，instant 调用者按隐藏处理，普通调用者暂不在此门拒绝；
- 同一应用直接放行；
- instant 调用者不能看另一个 instant 目标；查询普通目标时，组件级请求要求显式暴露，包级请求要求目标至少有 instant 可见组件；
- 普通调用者查询 instant 目标时，特权查看者可放行；普通组件级查询拒绝，包级查询依赖 `InstantAppRegistry` 的既有授权；
- 剩余的普通对普通查询才进入 `mAppsFilter.shouldFilterApplication()`。

`getPackageInfoInternal()` 用 `filterCallingUid` 做第一次静态库与可见性检查，随后 `generatePackageInfo()` 又读取当前 Binder UID，并再次调用 `shouldFilterApplicationLocked()`。清除身份的可信调用因此存在“原调用者过滤 + 当前执行身份再检查”的双重结构：第一次检查守住原调用者边界，第二次不会替代它。静态库内部版本解析则仍使用当前 Binder 身份。调试内部调用时，至少同时打印这两个 UID，不能只写一个笼统的 caller。

### 练习 4：把静态库门与 instant 包装门分开定位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (Process.isIsolated(callingUid)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'callingUid = mIsolatedOwners.get(callingUid);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (isCallerSameApp(ps.name, callingUid)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (callerIsInstantApp) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return !ps.pkg.isVisibleToInstantApps();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (canViewInstantApps(callingUid, userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return !mInstantAppRegistry.isInstantAccessGranted(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private boolean filterSharedLibPackageLPr(@Nullable PackageSetting ps, int uid, int userId,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & PackageManager.MATCH_STATIC_SHARED_LIBRARIES) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (ps == null || ps.pkg == null || !ps.pkg.isStaticSharedLibrary()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int resolvedUid = UserHandle.getUid(userId, UserHandle.getAppId(uid));' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '== libraryInfo.getLongVersion()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 7. AppsFilter 决策顺序：先找放行理由，最终默认拒绝

AppsFilter 的布尔语义要先翻译清楚：`true` 表示“应过滤”，`false` 表示“可见”。顶层方法先取 `UserHandle.getAppId()`，放行 calling appId 或 target appId 低于 `FIRST_APPLICATION_UID` 的情况，以及相同 appId；随后优先读完整 UID 到完整 UID 的缓存。缓存存在时，命中的普通应用查询不再跑现场规则，缓存就是该路径的权威派生状态。缓存缺少调用 UID 或目标 UID 条目时，代码记录异常并按拒绝处理，这是 fail-closed，而不是临时回退到现场计算。

查询服务路径没有缓存时，以及全量或增量构建 cache cell 时，`shouldFilterApplicationInternal()` 按以下次序裁决：

1. 全局过滤关闭；
2. 调用 Setting 缺失，拒绝；
3. 调用包的兼容变更关闭；shared UID 任一成员关闭，就使整个 shared UID 不受此过滤；
4. 目标 `pkg` 缺失，拒绝；
5. 目标是静态共享库，返回不拒绝并交还上层专门门；
6. 同 appId；
7. shared UID 任一成员在 manifest 请求 `QUERY_ALL_PACKAGES`；
8. 目标 appId 位于 `mForceQueryable`；
9. `<queries><package>`、安装来源等形成的 `mQueriesViaPackage`；
10. intent/provider 查询形成的 `mQueriesViaComponent`；
11. 完整 UID 维度的 `mImplicitlyQueryable`；
12. 调用包是目标 overlay 的合法 actor。

所有放行理由都不成立，最后才返回过滤。注意两个缺失值的拒绝都受更早分支约束：全局关闭时连缺失调用 Setting 也放行，调用包兼容开关关闭时则会早于目标 `pkg` 缺失返回。`QUERY_ALL_PACKAGES` 检查的是 manifest 的 requested-permission 列表，因为建图时权限分析未必完成；不能把它改写成运行时“已授予”。

### 练习 5：按源码真实优先级核对 AppsFilter

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (callingAppId < Process.FIRST_APPLICATION_UID' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mShouldFilterCache != null) { // use cache' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (shouldFilterTargets == null) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (!shouldFilterApplicationInternal(' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (!featureEnabled) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (callingSetting == null) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (targetPkg == null) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (targetPkg.isStaticSharedLibrary()) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F '&& requestsQueryAllPackages(callingPkgSetting.pkg)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mForceQueryable.contains(targetAppId)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mQueriesViaPackage.contains(callingAppId, targetAppId)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mQueriesViaComponentRequireRecompute) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mQueriesViaComponent.contains(callingAppId, targetAppId)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mImplicitlyQueryable.contains(callingUid, targetUid)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mOverlayReferenceMapper.isValidActor(targetName, callingPkgSetting.name)) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
```

## 8. 可见关系怎样建出来：有向边、双向边与三类键空间

AppsFilter 不是每次查询都重新解析所有 manifest。包加入时，它把“调用方可以看目标方”整理成若干有向边：

- `<queries><package>`：精确包名命中，形成 querying appId → target appId；
- `<queries><intent>`：只匹配目标的 exported 组件；receiver 还带入 protected-broadcast 集合参与 `IntentFilter.match()`；
- `<queries><provider>`：只看 exported provider，并把以分号分隔的 authority 逐个匹配；
- 安装来源：作为查询方的“被安装包”可以看自己的 installer，也可以看尚未卸载的 initiating package；originating package 不在这张图中；
- instrumentation：任一方 instrument 另一方时，显式建立双向边；
- overlay actor：不写入前两张查询图，而是在决策尾部通过 mapper 判断；
- 隐式交互授权：写入 `recipientUid → visibleUid`，含 userId 的完整 UID。

package/component 查询、安装来源与 instrumentation 关系都落在 appId 图中，因此自然跨该 appId 的用户实例复用；隐式授权和最终过滤缓存以完整 UID 为键，保留用户边界；overlay mapper 则以 packageName 识别 actor 与 target。这是三类不同键空间。shared UID 的多个包共享 appId，且 `QUERY_ALL_PACKAGES`、兼容开关与 overlay actor 都会遍历成员；分析 shared UID 时应把成员能力看成并集，而不是只检查任意一个包名。

overlay actor 是现场裁决或 cache cell 构建时读取的规则，并不是每次 cache hit 都重新查询 mapper。完整缓存已有布尔结果时，包名关系的后续变化必须先传播到相应 cell，服务查询才会观察到。

`mForceQueryable` 描述的是“目标对其他应用可查询”。来源包括已有 shared appId 状态、调试覆盖，以及 system app 内部的三选一：全局 `mSystemAppsQueryable`、manifest 的 `forceQueryable`、设备包名白名单；与平台签名完全相同的 system app 也会进入集合。manifest 标记并不能让普通非 system app 获得这一状态。它不是“调用者拥有全局查询能力”，方向不要画反。

### 练习 6：从 manifest 关系追到有向图更新

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return !querying.getQueriesPackages().isEmpty()' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'private static boolean canQueryAsInstaller(PackageSetting querying,' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (!provider.isExported()) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'StringTokenizer authorities = new StringTokenizer(provider.getAuthority(), ";",' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (!component.isExported()) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (isReplace) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'removePackage(newPkgSetting);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (mProtectedBroadcasts.addAll(newPkg.getProtectedBroadcasts())) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mQueriesViaComponentRequireRecompute = true;' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mQueriesViaPackage.add(newPkgSetting.appId, existingSetting.appId);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mQueriesViaPackage.add(existingSetting.appId, newPkgSetting.appId);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'private static boolean pkgInstruments(' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mOverlayReferenceMapper.addPkg(newPkgSetting.pkg, existingPkgs);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mFeatureConfig.updatePackageState(newPkgSetting, false' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
```

## 9. 生命周期与缓存：查询链并非绝对只读

包替换时 `addPackage(..., true)` 先清理该 appId 参与的 package 查询图、force、overlay、implicit 与 cache 条目；component 图只有在尚未标脏时才逐边清理，已脏时等待整图重算。随后再加入新关系，但这不等于旧 manifest 贡献的所有全局关系一定已被识别。新包的 protected-broadcast 集合真正扩张全局 union 时，`addAll()` 把 `mQueriesViaComponentRequireRecompute` 置真；remove 路径重收集 union 并发现收缩时也会置真。下一次 `shouldFilterApplicationInternal()` 走到 component 分支时——既可能是 live query，也可能是 cache-cell 构建或刷新——会调用 `recomputeComponentVisibility()`，清空并重建整张组件关系图。因此，API 虽是查询，内部仍可能更新派生状态；把它称为纯函数会漏掉锁竞争和时延尖峰。

`onSystemReady()` 调用异步的 `updateEntireShouldFilterCacheAsync()`。它先复制 Settings 映射、用户数组引用并保存被视为不可变的 package 引用，在后台计算所有用户组合、所有不同 appId 对的 UID→UID 布尔表；相同 appId 依赖顶层快速放行，不写这种 pair。发布前的变化探测很窄：只比较 Settings 的 size，以及当前每个 key 对应的 `AndroidPackage` 引用；它不比较 users、implicit 图、DeviceConfig、InstallSource 等输入：

- 这两项比较没有变化，发布刚算好的缓存；
- size 或 package 引用发生变化，在持锁状态下同步重建后发布；
- 缓存尚未发布时，查询走现场规则；
- 缓存发布后，查询直接消费缓存，缺项按拒绝处理。

所以 `onSystemReady()` 返回只表示任务已排入后台，不表示缓存已经可用。更重要的是，这一版实现的缓存失效并没有覆盖所有全局关系变化：

- protected-broadcast 脏标记只重建 `mQueriesViaComponent`；若完整缓存已存在，普通 cache hit 不进入重算分支，而重建组件图本身也不回写全部既有 UID 对；
- `addPackage()` 只更新新包或替换包参与的 cache pairs；protected broadcast 改变两个既有包之间的匹配时，旧 cell 可能继续生效；
- DeviceConfig 的全局过滤开关 listener 只改 `mFeatureEnabled`，没有触发全缓存刷新；单包 compat 变化才调用 `updateShouldFilterCacheForPackage()`；
- `setInstallerPackageName()` 修改来源后调用的是非 replace 的 `addPackage()`，会增加新 installer 边，却没有先删除旧 installer 边；
- replace 传入的 `PackageSetting` 已指向新解析包，`removePackage()` 又只在当前包仍声明 protected broadcasts 时重收集集合；旧版有广播而新版变空时，这个条件可能漏掉旧 action。

这些不是抽象上的可能性，而是当前实现需要纳入排障的失效边界：现场规则、关系图和 UID 缓存可能短暂或持续不一致。看到与 manifest/DeviceConfig 不符的结果时，要同时检查规则输入、图状态、cache cell 和触发更新的事件。

`grantImplicitAccess(recipientUid, visibleUid)` 同时更新完整 UID 图和已有缓存中的单个条目，值写 `false`，也就是明确放行。参数名已经给出方向：recipient 获得看见 visible 的能力，不要根据一次 IPC 的“调用方/被调用方”角色凭感觉倒置。

### 练习 7：观察懒重算、异步全量缓存与单点放行

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void grantImplicitAccess(int recipientUid, int visibleUid) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mImplicitlyQueryable.add(recipientUid, visibleUid)' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'visibleUids.put(visibleUid, false);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'updateEntireShouldFilterCacheAsync();' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mBackgroundExecutor.execute(() -> {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'SparseArray<SparseBooleanArray> cache =' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (changed[0]) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'updateEntireShouldFilterCache();' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mShouldFilterCache = cache;' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mQueriesViaComponent.clear();' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mQueriesViaComponentRequireRecompute = false;' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mFeatureEnabled = properties.getBoolean(FILTERING_ENABLED_NAME,' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mAppsFilter.updateShouldFilterCacheForPackage(packageName);' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'if (setting.pkg != null && !setting.pkg.getProtectedBroadcasts().isEmpty()) {' frameworks/base/services/core/java/com/android/server/pm/AppsFilter.java
grep -n -F 'mAppsFilter.addPackage(targetPackageSetting);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 10. PackageUserState：同一个解析包在每个用户下都有独立投影

`AndroidPackage` 描述从 APK 解析出的相对稳定事实；`PackageUserState` 描述这个包在某个用户下的状态。关键维度不能互相替代：

- `installed`：是否为该用户安装；
- `hidden`：是否被 owner/admin 隐藏；
- `enabled`：用户级包启停状态；
- `enabledComponents` / `disabledComponents`：组件显式覆盖；
- `instantApp`、`virtualPreload`：安装形态；
- `stopped`、`suspended`：运行与策略状态；
- overlay paths、分类：会参与这里的 `ApplicationInfo` 投影；安装原因、卸载原因等也存于 per-user state，但这条 Info 生成链没有把每个状态成员都写进结果。

`isAvailable(flags)` 的公式非常窄：

`MATCH_ANY_USER || (installed && (!hidden || MATCH_UNINSTALLED_PACKAGES))`

由此得到三个反直觉结论：`MATCH_UNINSTALLED_PACKAGES` 单独存在时，并不会让 `installed == false` 的普通包变得 available；它主要让“已安装但 hidden”的包通过。`MATCH_ANY_USER` 则直接使可用性成立。`stopped`、`suspended` 和 `enabled` 都不参与这条公式，它们分别在输出字段、组件匹配或其他策略中生效。

服务端的 `checkUseInstalledOrHidden()` 还为 system package 增加 known/hidden-until-installed 例外，所以“PackageUserState 自己不可用”不总等于“系统包不能生成信息”。但 hidden-until-installed 是更早的专门拒绝门：目标尚未安装且具有该属性时，即使 `MATCH_ANY_USER` 已使基础公式为真，没有 `MATCH_HIDDEN_UNTIL_INSTALLED_COMPONENTS` 仍会被挡住。必须把基础公式与服务端包装条件一起看。

## 11. 组件匹配：availability、enabled、system-only、Direct Boot 四关串联

`PackageUserState.isMatch()` 对组件按固定顺序裁决：

1. 包对该用户可用；system package 在 known-packages 位命中时有额外退路；
2. `isEnabled()` 判定包级和组件级启停；
3. `MATCH_SYSTEM_ONLY` 要求目标确为 system；
4. 组件的 `directBootAware` 必须命中 aware/unaware flags。

`isEnabled()` 也有明确优先级。`MATCH_DISABLED_COMPONENTS` 直接放行启停检查；否则先看用户级 package enabled state：`DISABLED` / `DISABLED_USER` 立即拒绝，`DISABLED_UNTIL_USED` 只有带相应匹配位才继续，`DEFAULT` 会先检查 manifest 的 package enabled，若为假便在组件覆盖集合之前返回。只有包级门通过，才依次看 `enabledComponents`、`disabledComponents`，最后回到 manifest 的组件默认值。显式组件覆盖只决定该元素是否进入结果数组；生成出的 `ComponentInfo.enabled` 仍来自 manifest，不会被改写成用户覆盖值。

这解释了“顶层 `PackageInfo` 不为空但某个 Activity 不在数组里”：包级可见性通过，不代表每个组件都通过 per-user + Direct Boot 匹配。用户锁定时，入口默认只补 aware 位，unaware 组件自然被裁掉；调用者显式传两类位则可改变集合。

### 练习 8：把包可用性与组件四关逐行对应

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final boolean matchAnyUser = (flags & PackageManager.MATCH_ANY_USER) != 0;' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'return matchAnyUser' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'public boolean isAvailable(int flags) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if (!isAvailable(flags) && !(isSystem && matchUninstalled)) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if (!isEnabled(isPackageEnabled, isComponentEnabled, componentName, flags)) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if ((flags & MATCH_SYSTEM_ONLY) != 0) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if ((flags & MATCH_DISABLED_COMPONENTS) != 0) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'case COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED:' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if (!isPackageEnabled) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if (ArrayUtils.contains(this.enabledComponents, componentName)) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if (ArrayUtils.contains(this.disabledComponents, componentName)) {' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'final boolean matchesUnaware =' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'final boolean matchesAware =' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'return reportIfDebug(matchesUnaware || matchesAware, flags);' frameworks/base/core/java/android/content/pm/PackageUserState.java
grep -n -F 'if ((flags & PackageManager.MATCH_HIDDEN_UNTIL_INSTALLED_COMPONENTS) == 0' frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java
grep -n -F 'return state.isAvailable(flags)' frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java
```

## 12. PackageInfo 组装：权限、GID、签名与组件数组各自受控

`generatePackageInfo()` 先再次做可见性检查。若目标是 system package 且 flags 含 `MATCH_UNINSTALLED_PACKAGES`，它会在这里再补 `MATCH_ANY_USER`，然后才读取目标用户的 `PackageUserState`；这一步发生在入口的显式 any-user 权限检查之后，也不会回头重跑同一个检查。对于有活动 `AndroidPackage` 的正常路径，它准备三类输入后调用 `PackageInfoUtils.generate()`：

- GID：只有 `GET_GIDS` 时才计算，否则内部传空数组，最终 `PackageInfo.gids` 也不会赋值；
- 权限状态：manifest 的 requested permissions 决定有哪些槽位，`PermissionsState` 决定每个槽位是否带 granted 标志；
- instant 目标：已授予集合会进一步移除非 instant 权限，并可能写安全事件日志。

`PackageInfoUtils` 先生成 `ApplicationInfo` 与不含组件的顶层 `PackageInfo`，再按 `GET_ACTIVITIES`、`GET_RECEIVERS`、`GET_SERVICES`、`GET_PROVIDERS`、`GET_INSTRUMENTATION` 分别填数组。前四类中的每个元素还要通过 `ComponentParseUtils.isMatch()`；应用详情 Activity 被显式跳过。Instrumentation 数组虽受 `GET_INSTRUMENTATION` 控制，却没有同样的 per-component match 循环。

普通 APK 生成路径中，旧的 `GET_SIGNATURES` 与新的 `GET_SIGNING_CERTIFICATES` 是两条独立分支。旧接口在证书轮换存在时保留兼容语义，新接口返回 `SigningInfo`。不要因为请求了 permissions 就期待签名，也不要因为请求了签名就期待组件数组；APEX 使用上一节所述的固定扫描缓存，不能套用这条按本次 flags 裁剪的结论。

## 13. ApplicationInfo 投影：manifest 骨架叠加用户态与 Settings 态

正常服务端路径先由 `pkg.toAppInfoWithoutState()` 得到新的顶层 `ApplicationInfo`，再逐层叠加：

1. `initForUser(userId)` 重写 UID 与用户数据目录；
2. `GET_META_DATA` 控制应用级 `metaData`；
3. stopped、installed、suspended 写入公开 flags；
4. instant、virtual-preload、hidden 写入 private flags；
5. 用户级 enabled state 总会写入 `enabledSetting`，并可覆盖 `enabled`；`DEFAULT` 不覆盖，保留 manifest 的 package enabled 值；
6. `seInfoUser` 和用户 overlay paths 被写入；
7. 服务端再用 `PackageSetting` 补 hidden-until-installed、共享库、ABI、`seInfo`、system/updated-system 等状态。

因此 `ApplicationInfo` 不是 APK 解析对象的原样暴露，也不是只属于包的全局常量。特别是 `uid`、dataDir、overlay、enabled、installed、hidden 和 instant 都与目标 `userId` 或 Settings 状态有关；把 user 0 的对象缓存给其他用户会制造错误。

`getApplicationInfoInternal()` 对普通 APK 使用上述投影；APEX 与平台特殊对象则走各自的直接返回分支。所谓“每次查询都新建并完整投影”的结论只适用于正常 APK 生成路径，不能覆盖这些例外。

## 14. 字段裁剪的两个实现陷阱：共享库回填与组件 metadata

flags 名字很容易诱导出两个错误推断。以当前源码树为准，真实行为是：

第一，底层 `generateApplicationInfoUnchecked()` 在没有 `GET_SHARED_LIBRARY_FILES` 时把 `sharedLibraryFiles` 与 `sharedLibraryInfos` 清空；但服务端 `PackageInfoUtils.generateApplicationInfo()` 随后只要收到非空 `PackageSetting`，就从 `PackageStateUnserialized` 无条件回填这两个字段。于是正常 PMS APK 路径中，该 flag 并不能保证最终共享库字段为空；`pkgSetting == null` 的其他调用路径才保留底层裁剪效果。

第二，`GET_META_DATA` 确实控制应用级 `ApplicationInfo.metaData`，也控制 Instrumentation、Permission 与 PermissionGroup 的 metadata；可是 Activity、Receiver、Service、Provider 的 unchecked 生成器直接赋 `getMetaData()`，其中 Receiver 复用 Activity 生成器，服务端包装层没有再清空。因而组件本身若有 metadata，即使未带该 flag，也可能出现在这四类组件信息中。Provider 的 URI permission patterns 则另受 `GET_URI_PERMISSION_PATTERNS` 正常控制。

还有所有权问题：顶层 `PackageInfo`、`ApplicationInfo`、各组件对象及组件数组是新对象，但代码中仍有直接引用赋值，例如 split arrays、组件 `Bundle`、Provider 的 pattern/path arrays、`sharedLibraryInfos` 列表。跨进程 Binder 返回会经过 Parcel，通常形成进程边界副本；system_server 内的本地调用没有这层天然隔离。故应称它为“带浅别名的投影”，而不是深不可变快照。

调试字段缺失时，按三层检查：外层是否请求该字段、元素是否通过用户态匹配、服务端包装是否又覆盖底层结果。只盯 flags 常量名称不够。

## 15. 最小信息、available 与 startable：三个 API 回答不同问题

当 Settings 中有 `PackageSetting`、但 `ps.pkg == null` 时，`generatePackageInfo()` 只有在带 `MATCH_UNINSTALLED_PACKAGES` 且 `state.isAvailable(flags)` 时才构造最小对象。它只填包名、版本、sharedUser、安装时间和一个简化 `ApplicationInfo` 等持久字段；不会凭空恢复 manifest 组件、requested permissions 或签名细节。这是“记录仍在，但解析包体不可用”的降级结果，不等于完整安装态。

三个看似相近的 API 应这样区分：

| API | 正常失败形态 | 它实际回答的问题 |
| --- | --- | --- |
| `getPackageInfo()` | PMS 多数返回 `null`，SDK 包装成 `NameNotFoundException`；跨用户违规抛 `SecurityException` | 调用者能否看见某个路由后的包对象，并按 flags/用户态生成哪些字段 |
| `isPackageAvailable()` | `false`，跨用户违规抛异常 | 活动 `mPackages` 中的可见包，对该用户是否 `installed && !hidden` |
| `checkPackageStartable()` | 不满足即抛 `SecurityException` | 包是否可见且已安装，并通过安全模式、冻结和用户密钥/加密感知检查 |

`isPackageAvailable()` 不接受扩展匹配 flags，也不退到任意 known Settings 记录。`checkPackageStartable()` 先拒绝 instant 调用者和不存在用户，再做跨用户校验；它随后检查 installed、安全模式下的非 system 包、frozen，以及锁屏阶段的 encryption-aware。它并不是 `getPackageInfo() != null` 的布尔别名，也没有把 stopped、suspended、enabled 混进同一判断。

### 练习 9：核对组装、字段陷阱、最小对象与启动资格

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final int[] gids = (flags & PackageManager.GET_GIDS) == 0' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'PackageInfo packageInfo = PackageInfoUtils.generate(p, gids, flags,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if ((flags & MATCH_UNINSTALLED_PACKAGES) != 0 && state.isAvailable(flags)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'pi.applicationInfo = PackageParser.generateApplicationInfo(ai, flags, state, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!ps.getInstalled(userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mFrozenPackages.contains(packageName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!userKeyUnlocked && !AndroidPackageUtils.isEncryptionAware(ps.pkg)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & PackageManager.GET_ACTIVITIES) != 0) {' frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java
grep -n -F 'ComponentParseUtils.isMatch(state, pkg.isSystem(), pkg.isEnabled(), a,' frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java
grep -n -F 'info.sharedLibraryFiles = usesLibraryFiles.isEmpty()' frameworks/base/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java
grep -n -F 'if ((flags & PackageManager.GET_META_DATA) == 0) {' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'ai.metaData = a.getMetaData();' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'si.metaData = s.getMetaData();' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'pi.metaData = p.getMetaData();' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'if ((flags & PackageManager.GET_URI_PERMISSION_PATTERNS) == 0) {' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
grep -n -F 'pi.splitNames = pkg.getSplitNames();' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
```

## 16. 收束：用“拒绝点 + 完成点”而不是包名猜答案

排查一次异常查询，按下面顺序收集证据最省时间：

1. 固定请求名、版本、原始 `filterCallingUid`、当前 Binder UID、目标 `userId` 和输入 flags；
2. 记录 flags 规范化后的 Direct Boot 与 any-user 位，以及跨用户检查是否完成；
3. 记录 renamed/static-lib 解析后的内部包名和实际对象来源：factory APK、active APK、known Settings、APEX 或平台特殊对象；
4. 静态库先看精确依赖门，instant 包先看 PMS 包装门；普通包只有在现场裁决或构建 cache cell 时才有“第一条命中理由”，cache hit 只留下最终布尔值；
5. 缓存路径要区分“明确过滤”与“缓存缺项后 fail-closed”；cache hit 本身不会观察组件图脏标记，也不会触发懒重算，还要核对相关更新事件是否真正把新关系传播到 UID cache cells；
6. 对普通 APK 的非空结果，再用 `PackageUserState` 四关解释 Activity、Receiver、Service、Provider 集合；Instrumentation 没有同样的逐组件匹配，APEX 与平台特殊对象也绕过这条投影链；
7. 最后才按 flags、服务端回填和浅别名检查字段形状。

这条链最重要的完成点有三个：对象路由完成，意味着“查询的是谁”已确定；可见性裁决完成，意味着“调用者能否知道它”已确定；用户态与字段投影完成，意味着“最终能看到多少”已确定。把这三个完成点分开，`null`、最小对象和裁剪对象就不再互相冒充。

下一章进入 254：`ComponentResolver` 如何收集 `IntentFilter` 候选，如何完成 action/type/scheme/category 匹配，并怎样对 Activity 查询结果排序。那里会把本章的“组件是否有资格进入数组”继续推进到“Intent 查询为何选择并排序这些组件”。
