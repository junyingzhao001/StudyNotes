# 272 Android SoftRestrictedPermissionPolicy：存储权限兼容、legacy external storage与额外AppOp策略链

## 1. 先看结论：这里至少有五种“legacy”，不能压成一个布尔值

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base`提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。`SoftRestrictedPermissionPolicy`不是一个“是否能访问外部存储”的总开关，而是把安装历史、包声明、permission flags和 shared UID事实投影成两类建议：能否明确 grant危险权限，以及 `OP_LEGACY_STORAGE`应该被允许、拒绝还是只保留已有 ALLOWED。

完整链上至少有五个不同状态：

| 状态 | 权威位置 | 它能证明什么 |
|---|---|---|
| READ/WRITE permission 与 flags | PermissionManager | permission事实，以及 restriction是否豁免或应用 |
| `OP_LEGACY_STORAGE` UID/package mode | AppOpsService | legacy政策状态；默认是 DEFAULT，不是天然 ALLOWED |
| `mUidsWithLegacyExternalStorage` | StorageManagerService内存 | 对 legacy AppOp的一份反馈快照，供下一轮策略读取 |
| mount mode与进程 namespace | StorageManagerService、ProcessList、Zygote/vold | 进程实际拿到哪类挂载入口 |
| `Environment.isExternalStorageLegacy()` | 应用进程中的 compat + AppOps查询 | API语义结果，不是对实际 namespace的反向验真 |

`requestLegacyExternalStorage`和 `preserveLegacyExternalStorage`又只是解析后的意图输入。由此可得本章的排障原则：先问正在观察哪一种状态，再问它的生产者、刷新入口和完成点；不要用 Manifest属性、AppOp、缓存、mount mode或 API返回值相互代称。

## 2. 两份同名类不是同一份策略，且只有 READ 产生 extra op

system_server与 PermissionController各有一份 `SoftRestrictedPermissionPolicy`，注释称彼此为 twin，但 r48 的公开形状已经不同。Controller侧只有两个 `shouldShow()`重载，用当前包 targetSdk与 exemption flags决定权限是否显示在 UI；它不计算 shared UID最小 target、不读取 StorageManager，也不产生 legacy AppOp建议。

system_server侧才有 `forPermission()`、`mayGrantPermission()`、`mayAllowExtraAppOp()`与 `mayDenyExtraAppOpIfGranted()`。它只特化两个名字：

- READ与 WRITE都有 permission grant资格公式；
- 只有 READ的 `getExtraAppOpCode()`返回 `OP_LEGACY_STORAGE`；
- WRITE继承默认 `OP_NONE`，不会单独生成 legacy候选；
- 其他名字返回 DUMMY策略：可 grant、无 extra op、allow/deny建议都为 false。

PermissionPolicyService启动监听时甚至用四个 null参数构造策略，只为发现某个 soft-restricted permission有没有 extra op。这条“发现路径”和带真实包的“裁决路径”目的不同。

### 练习 1：对照两份同名类的合同

先写出两份类各自真正公开的问题，再验证 READ、WRITE和其他 permission分别会返回什么。不要从 twin这个词推导实现等价。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static boolean shouldShow(@NonNull PackageInfo pkg' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/SoftRestrictedPermissionPolicy.java
grep -n -F 'public static @NonNull SoftRestrictedPermissionPolicy forPermission' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'case READ_EXTERNAL_STORAGE:' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'return OP_LEGACY_STORAGE;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'case WRITE_EXTERNAL_STORAGE:' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'return DUMMY_POLICY;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'SoftRestrictedPermissionPolicy.forPermission(null, null, null,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 3. 策略对象冻结一次输入；null发现路径还暴露了合同裂缝

对 READ的真实包裁决，可以把构造时捕获的量记成：

| 记号 | 含义 | 维度 |
|---|---|---|
| `E_i` | 当前包 READ flags含任一 SYSTEM/UPGRADE/INSTALLER exemption | 包、用户、permission |
| `R_i` | 当前包 READ flags含 APPLY_RESTRICTION | 包、用户、permission |
| `T_u` | 同 UID可解析成员的最小 targetSdk | UID聚合 |
| `L_u` | StorageManager反馈缓存称该 UID已有 legacy | UID缓存 |
| `Q_u` | 同 UID任一成员请求 legacy | UID聚合 |
| `P_i` | 当前 `AndroidPackage`请求 preserve legacy | 包级 |
| `M_u` | 同 UID任一成员获 `WRITE_MEDIA_STORAGE` | UID聚合 |
| `F_i` | 当前包名命中 forced-scoped静态集合 | 包级 |

这些值在 `forPermission()`里读入局部变量，再被匿名策略对象捕获。对象的方法不会重新读取 flags、AppOps、DeviceConfig或包状态；事件发生后必须新建对象才有新结论。

当 `appInfo == null`时，READ分支把所有布尔量置 false、`T_u`置 0，因此得到“不可 grant、extra为 LEGACY、不可 allow、可 deny”；WRITE得到“不可 grant、无 extra”。这正适合发现 op，却绝不能代表任何包。入口虽把 `context`标成非空，PPS却传 null，当前只因 null-app分支不解引用它而安全。

反方向也有合同缺口：`pkg`标为可空，但 READ在 `appInfo != null`时无条件调用 `pkg.hasPreserveLegacyExternalStorage()`。实际两个裁决调用点都同时提供对象；第三方若只给 appInfo不给 pkg会直接空指针。该类只吞同 UID成员查询中的 `NameNotFoundException`，不会把服务缺失或其他运行时异常降级成保守值。

### 练习 2：区分发现对象与裁决对象

分别手算 null-app READ、null-app WRITE和真实包 READ；再标出哪些方法调用会重读系统状态，答案应为“没有”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (appInfo != null) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'hasRequestedPreserveLegacyExternalStorage =' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'pkg.hasPreserveLegacyExternalStorage();' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'isWhiteListed = false;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'targetSDK = 0;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'hasWriteMediaStorageGrantedForUid = false;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'catch (PackageManager.NameNotFoundException e) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
```

## 4. shared UID聚合混合了 UID级与包级输入，grant入口还有跨用户 UID差异

`getMinimumTargetSDK()`从当前 appInfo的 target开始，遍历 `getPackagesForUid(appInfo.uid)`返回的全部名字，再按显式 `user`查询 ApplicationInfo并取最小值。兄弟包不必请求 READ/WRITE；只要共享 UID且查询成功，就能把 `T_u`拉低。查询失败的成员被跳过，包列表为空时则只保留当前包 target。

`Q_u`与 `M_u`也先用传入 uid取包集合，再做 OR：前者看 ApplicationInfo的 request位，后者逐包检查 `WRITE_MEDIA_STORAGE`。`L_u`直接按 uid查询 StorageManager缓存。相反，`P_i`与 `F_i`只看当前包，`E_i`、`R_i`还绑定当前用户下该包的 READ flags。一个策略对象因此故意混合 UID聚合与包级事实。

PermissionPolicyService用目标 user context取得 `PackageInfo.applicationInfo.uid`，这里是完整的 per-user UID。PermissionManager的直接 grant门却传 `pkg.toAppInfoWithoutState()`；解析包在扫描时保存的是 appId，`toAppInfoWithoutState()`原样写入 appInfo.uid。对非 system user，flags查询仍使用显式目标 user，但 `getPackagesForUid`、`L_u`、`Q_u`和 `M_u`可能按 user 0的数字身份取样。

这是 r48两个调用点的输入差异，不宜在没有设备复现前扩大成所有安装流程都会失败；但它足以否定“传入 UserHandle会自动修正 UID聚合”的说法。排查 secondary user的 grant拒绝时，应同时记录 appInfo.uid和目标 user。

### 练习 3：用两个用户、三个共享包建立输入表

让 user 10独有一个包，另两个同 appId包存在于 user 0；逐行标出 PPS同步与 PMS grant两条调用会枚举哪些名字、在哪个 user查询 flags与 ApplicationInfo。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static int getMinimumTargetSDK' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'String[] uidPkgs = pm.getPackagesForUid(appInfo.uid);' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'minimumTargetSDK = min(minimumTargetSDK, uidPkgInfo.targetSdkVersion);' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'private static boolean hasUidRequestedLegacyExternalStorage' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'private static boolean hasWriteMediaStorageGrantedForUid' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'pkg.toAppInfoWithoutState(), pkg, UserHandle.of(userId), permName)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'appInfo.uid = uid;' frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/PackageImpl.java
grep -n -F 'parsedPackage.setUid(pkgSetting.appId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 5. mayGrant只判断一次授权是否合法，不代表 permission已授或存储已放宽

READ与 WRITE的公式完全相同：

`mayGrant_i = E_i || T_u >= 29`

它不读取 APPLY_RESTRICTION、request、preserve、legacy缓存、forced名单或 `WRITE_MEDIA_STORAGE`。target M以下的 runtime permission请求在 PMS更早处已经返回；target M至 P的包若无 exemption，会被 soft-restricted门拒绝；shared UID最小 target达到 Q后，即使没有 exemption也可通过这个资格门。

`overridePolicy`只影响 POLICY_FIXED检查，不会绕过 soft-restricted公式。通过之后，PMS还要真正写 `PermissionsState`；失败、已有状态或其他门都可能让结果不同。成功 grant READ也没有自动写 `OP_LEGACY_STORAGE`，两者不是同一提交。

PermissionPolicyService同步主 READ/WRITE op时还会先看实际 grant、REVOKED_COMPAT、hard/soft规则；REVIEW_REQUIRED则让主 op根本不产生候选。extra-op路径没有这些三项前置检查，只要解析后的 requestedPermissions中出现 READ就会单独算 legacy建议。因此“先获 READ，才可能允许 LEGACY”不是源码合同；最终 mount仍会用 READ/WRITE permission与主 op把能力卡住。

Controller侧 `shouldShow()`只用当前包 target，而服务端 grant公式用 shared UID最小 target。UI显示与服务端资格在 shared UID边界可以不同；UI方法不是授权裁决。

### 练习 4：拆开 UI、grant门、主 op与 extra op

构造一个 target 30包和一个 target 28包共享 UID、两者均无 exemption。预测 Controller是否显示、PMS是否允许 grant、PPS是否产生主候选和 extra候选，不要只给一个“允许/拒绝”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return isWhiteListed || targetSDK >= Build.VERSION_CODES.Q;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'return isWhiteListed || pkg.getTargetSdkVersion() >= Build.VERSION_CODES.Q;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/SoftRestrictedPermissionPolicy.java
grep -n -F 'if (bp.isSoftRestricted() && !SoftRestrictedPermissionPolicy.forPermission' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!overridePolicy && (flags & PackageManager.FLAG_PERMISSION_POLICY_FIXED) != 0)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean isGranted = mPackageManager.checkPermission(permissionName, packageName)' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'boolean isReviewRequired = (permissionFlags & FLAG_PERMISSION_REVIEW_REQUIRED) != 0;' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'addPermissionAppOp(packageInfo, pkg, permissionInfo);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'addExtraAppOp(packageInfo, pkg, permissionInfo);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 6. READ的新获公式有两个强否决门，WRITE_MEDIA_STORAGE也不能越过

READ的 `mayAllowExtraAppOp()`可精确写成：

`A_i = !R_i && !F_i && (M_u || ((L_u || Q_u) && T_u < 30))`

判断有明确顺序。APPLY_RESTRICTION先否决，forced-scoped包名再否决，之后才轮到 `WRITE_MEDIA_STORAGE`。所以这个 signature|privileged能力能绕过 target、历史与 request条件，却不能绕过 restriction或 forced名单。

普通应用则同时需要两部分：当前已有 legacy反馈或 shared UID任一包请求 legacy，并且 shared UID最小 target小于 R。典型结果如下：

| 输入 | 新获 LEGACY建议 |
|---|---|
| target 28，默认 request=true，无强否决 | ALLOW |
| target 29，显式 request=true，无强否决 | ALLOW |
| target 29，request=false且缓存无 legacy | 不 ALLOW |
| target 30，request=true、无 WMS | 不 ALLOW |
| target 30，有 WMS、无强否决 | ALLOW |
| 任意 target，`R_i`或 `F_i`为 true | 不 ALLOW |

名字 `forced_scoped_storage_whitelist`很容易误导：命中集合的语义是允许平台把该包强制送入 scoped，而不是 exemption白名单。另一个反直觉点是 extra路径不要求 READ当前已 grant；它能先形成 UID政策，但没有主 READ/WRITE能力时不会凭空给出读写挂载。

### 练习 5：逐短路计算新获公式

按源码执行顺序依次翻转 `R_i`、`F_i`、`M_u`、`L_u`、`Q_u`和 `T_u`，记录哪个条件已决定返回值，避免把公式只背成一串 OR。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'shouldApplyRestriction = (flags & FLAG_PERMISSION_APPLY_RESTRICTION) != 0;' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'isForcedScopedStorage = sForcedScopedStorageAppWhitelist' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'if (shouldApplyRestriction) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'if (isForcedScopedStorage) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'return hasWriteMediaStorageGrantedForUid' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'hasLegacyExternalStorage || hasRequestedLegacyExternalStorage' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F '&& targetSDK < Build.VERSION_CODES.R);' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'if (applicationInfo.hasRequestedLegacyExternalStorage()) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'checkPermission(WRITE_MEDIA_STORAGE, packageName)' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'private static final HashSet<String> sForcedScopedStorageAppWhitelist' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'StorageManager.PROP_FORCED_SCOPED_STORAGE_WHITELIST' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'return rawList.split(",");' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
```

## 7. preserve表达“不撤销 ALLOWED”，而不是“给我 ALLOWED”

撤销建议 `D_i`分成两段：

- `T_u < 30`时，`D_i = !A_i`；
- `T_u >= 30`时，`D_i = R_i || F_i || (!M_u && !P_i)`。

PPS按三路消费：

| 策略结果 | extra候选 | 含义 |
|---|---|---|
| `A_i=true` | ALLOW | 明确写 ALLOWED |
| `A_i=false, D_i=true` | IGNORE | 明确写 IGNORED |
| 两者都 false | IGNORE_IF_NOT_ALLOWED | raw恰为 ALLOWED时保留；否则归一 IGNORED |

所以 target R及以上、已有 ALLOWED、无 restriction/forced/WMS而 `P_i=true`的包会走条件候选：它能保住 ALLOWED，却不能把 DEFAULT或已经 IGNORED恢复为 ALLOWED。`L_u`没有进入 R以上撤销式，正因为“是否已有 ALLOWED”由消费端的 raw读取再判断。

shared UID里的 preserve也不能简单做 OR。假设两个请求 READ的 R包共享 UID，一个 `P_i=true`、另一个 false：前者给条件候选，后者给确定 IGNORE；若没有第三个 ALLOW候选，确定 IGNORE先占键，整个 UID仍被压到 IGNORED。反过来，某成员有 WMS产生 ALLOW时，ALLOW会压过其他成员的 IGNORE。

最后还要继承第271章的限制：ALLOW→IGNORE→条件 IGNORE只是候选优先级。`unsafeCheckOpRaw()`会合成 package override、suspend与 restriction；合成值可能让 UID setter误判 no-op，所以“最宽候选优先”不保证每个 shared成员最终 effective mode最宽。

### 练习 6：用三个 R包验证 preserve不是 UID级 OR

让 A声明 preserve、B不声明、C持有 WMS，分三轮加入 A、A+B、A+B+C。分别写出候选表、执行顺序、raw为 DEFAULT与 ALLOWED时的结果。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (targetSDK < Build.VERSION_CODES.R) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F '&& !hasRequestedPreserveLegacyExternalStorage) {' frameworks/base/services/core/java/com/android/server/policy/SoftRestrictedPermissionPolicy.java
grep -n -F 'if (policy.mayAllowExtraAppOp()) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (policy.mayDenyExtraAppOpIfGranted()) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mOpsToIgnoreIfNotAllowed.add(extraOpToChange);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int allowCount = mOpsToAllow.size();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int ignoreCount = mOpsToIgnore.size();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'final int ignoreIfNotAllowedCount = mOpsToIgnoreIfNotAllowed.size();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (currentMode != MODE_ALLOWED) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

## 8. request与preserve的解析默认值不同，包更新还有反扩权撤销

解析器令 request的默认值为 `targetSdk < Q`，Q及以上默认 false，但 Manifest可显式覆盖；preserve无条件默认 false。target R包即使把 request写成 true，也会被 `T_u < R`挡住普通新获，除非 shared旧成员拉低最小 target或 UID持 WMS。preserve则只参与 R以上撤销式，从来不进入新获公式。

PMS在包更新时另有一条安全链。若 target从 Q及以上降到 Q以下，或不是这次跨 Q升级且 request从 false变 true，它会遍历所有 user，把新包所请求的 storage permissions逐项撤销。这防止包通过降 target或突然请求更宽旧模型继承既有授权；它改变的是 permission事实，不是直接改 legacy AppOp。

另一 helper只要看到“replace + 新包当前 request=true + 请求 READ或WRITE”，就把 updatedUserIds扩成所有用户，以便后续 runtime-state通知触发策略重算。它不比较旧 request，因此持续为 true的普通更新也会过度触发；request从 true移除则不靠这个 helper命中，但包 changed观察仍可能带来同步。

r48的 CTS把时间方向写得很清楚：target 30的 preserve能延续一次真实 legacy更新，首次安装、从非 legacy更新、卸载后重装或已经丢失后再加 preserve都不能新建 ALLOWED。

### 练习 7：画出四种包更新的 permission与AppOp时钟

比较 target降级、新增 request、target跨 Q升级、仅保留 request四种 replace。先判断是否撤 permission，再判断是否把所有 user加入更新集，最后才追 PPS的异步 AppOps收敛。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'setRequestLegacyExternalStorage(bool(targetSdk < Build.VERSION_CODES.Q' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'setPreserveLegacyExternalStorage(bool(false' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'boolean downgradedSdk = oldPackage.getTargetSdkVersion() >= Build.VERSION_CODES.Q' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean newlyRequestsLegacy = !upgradedSdk && !oldPackage.isRequestLegacyExternalStorage()' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'STORAGE_PERMISSIONS.contains(permInfo.name)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (replace && pkg.isRequestLegacyExternalStorage() && (' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return getAllUserIds();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'testStorageTargetingSdk30CanPreserveLegacyOnUpdateFromLegacy' cts/tests/tests/permission2/src/android/permission2/cts/RestrictedStoragePermissionTest.java
grep -n -F 'testStorageTargetingSdk30CannotPreserveLegacyOnInstall' cts/tests/tests/permission2/src/android/permission2/cts/RestrictedStoragePermissionTest.java
```

## 9. exemption、forced名单与 WRITE_MEDIA_STORAGE属于三套独立控制

SYSTEM、UPGRADE、INSTALLER exemption按位 OR成 `E_i`，它们允许旧 target通过 permission grant门。APPLY_RESTRICTION是另一个 flag，extra公式只直接读取它，不直接读取 `E_i`。上游权限更新通常会协调两者，但策略源码没有把“有 exemption”写成“legacy自动 ALLOW”。

forced集合来自 DeviceConfig的 `storage_native_boot/forced_scoped_storage_whitelist`。类加载时读取一次，null或空串变空集合，否则直接按逗号 split；没有 trim，也没有本类 listener。运行中改属性不会改变既有 HashSet，也不会由本类自动要求 PPS重算。命名里的 whitelist描述可被强制迁移的包集合，作用方向与 exemption相反。

`WRITE_MEDIA_STORAGE`在 Manifest中是 signature|privileged且已标记 deprecated、不再附送旧 `media_rw` GID，但 r48仍把它作为活跃兼容输入。它能在没有强否决时使 READ策略新授或保留 legacy；StorageManager另要求它与有效 WRITE结合，才返回 FULL mount。单独持有 WMS不是完整文件能力。

forced和 preserve是当前包级输入，而候选最后写 UID mode。于是 shared UID中的另一成员若合法产生 ALLOW，可能压过命中 forced的成员所提 IGNORE；反过来，一个未 preserve的 R成员也可能压过另一成员的条件保留。配置名与单包策略都不能直接推出 shared UID终态。

## 10. PermissionPolicy合并的是候选，不是一份可回滚事务

单包同步会加入触发包和同 user的所有 shared成员；全量同步也按每个包产生候选。每个包只遍历自己解析后的 requestedPermissions，所以未请求 READ的 shared成员不会直接创建 legacy候选，却仍能通过最小 target、request或 WMS聚合影响别人的公式。

对 `OP_LEGACY_STORAGE`没有 FOREGROUND候选，实质优先级是 ALLOW→IGNORE→IGNORE_IF_NOT_ALLOWED。ALLOW循环不预去重；确定 IGNORE遇到已占的 `(uid,op)`会跳过；条件候选只有 setter返回 true后才占键。多个条件候选若各包 raw不同，可能逐个执行，任一非 ALLOWED便可把 UID写成 IGNORED。

setter先以具体候选包查询 effective raw，再决定是否写 UID mode；普通 setter写后还可能用同一个包清 package覆盖。这让 package名参与“是否写”的判断，而目标状态又是 UID级。restriction、suspend或旧 package override可以遮住底层值，候选列表正确也可能漏写，具体反例已在第271章证明。

同步过程中没有锁住 PMS permission、AppOps mode、StorageManager缓存和进程 namespace。前一个 setter成功、后一个抛异常时不会回滚；下一轮包事件、permission通知、AppOps watcher或用户启动扫描才可能继续收敛。

## 11. OP_LEGACY_STORAGE是持久政策；StorageManager通知却早于磁盘提交

AppOps表给 `OP_LEGACY_STORAGE`配置的 permission为 null、默认 mode为 DEFAULT。PPS通常通过 policy专用 UID setter写 ALLOWED或 IGNORED；这是 AppOps运行时政策，而不是 Manifest permission或 StorageManager集合。

`setUidMode()`先在 `mUidStates`改内存并安排普通延迟写，再异步通知 mode watchers；随后还在 setter调用线程通过 LocalServices同步调用 `StorageManagerInternal.onAppOpsChanged()`。默认普通写延迟是30分钟，正常关机另有同步 flush，因此下面四个完成点不能合并：

1. UID mode内存已变；
2. StorageManager同步回调已返回；
3. 普通 watcher已运行；
4. `appops.xml`已经提交。

policy callback只会从普通 watcher集合排除 PPS自身，不会屏蔽同步 StorageManager回调。UID setter给该回调的 packageName是 null；package setter则传具体包，而且即使没有真正改变持久 mode也会走同步内部通知。StorageManager收到的是 setter报告的目标 mode，不是一次重新读取后的跨层事务结果。

## 12. legacy UID集合是有意参与反馈的缓存，也带着顺序与 shared UID缺口

StorageManagerService在 user starting时枚举包含 direct-boot、uninstalled和 any-user匹配标志的 ApplicationInfo，逐包用 `checkOperation(OP_LEGACY_STORAGE)`取 effective结果，再 add/remove完整 UID。SystemServer先注册 StorageManagerService、后注册 PermissionPolicyService，而 SystemServiceManager按服务表顺序发 user-start回调；正常路径因此先快照旧 AppOps，再让 PPS把 `L_u`作为迁移历史输入重算。

这不是从空集合重建的数据库。若 shared UID成员因 package override得到不同 effective结果，逐包 add/remove会令最后一次更新决定集合；包移除也直接按 UID remove，源码注释明确指出没有检查同 shared UID的其他安装包。user stop只注销 PackageMonitor，不按 user清集合；下一次 start也没有先清空该 user的旧 UID。

FUSE开启时，AppOps→StorageManager同步回调遇到 LEGACY会按新 mode更新集合。非 FUSE时，专用 Binder watcher负责重算活跃 UID的 mount，却不维护这份集合。因而 `hasLegacyExternalStorage(uid)`最多是迁移反馈缓存：它不等于实际 AppOps权威值，更不等于某个进程当前 namespace。

### 练习 8：制造“AppOp正确、反馈缓存错误”的 shared UID

让两个成员保留不同 package override，并交换扫描顺序；再模拟移除其中一个包和 user stop/start。逐步记录 AppOps effective值、集合成员与 mount实时查询，找出哪些步骤会重新校准。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static final long WRITE_DELAY = DEBUG ? 1000 : 30*60*1000;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'null, // no permission for OP_LEGACY_STORAGE' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'AppOpsManager.MODE_DEFAULT, // LEGACY_STORAGE' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'new File(systemDir, "appops.xml")' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'notifyOpChangedSync(code, uid, null, mode, previousMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'storageManagerInternal.onAppOpsChanged(code, uid, packageName, mode, previousMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mSystemServiceManager.startService(STORAGE_MANAGER_SERVICE_CLASS);' frameworks/base/services/java/com/android/server/SystemServer.java
grep -n -F 'mSystemServiceManager.startService(PermissionPolicyService.class);' frameworks/base/services/java/com/android/server/SystemServer.java
grep -n -F 'final SystemService service = mServices.get(i);' frameworks/base/services/core/java/com/android/server/SystemServiceManager.java
grep -n -F 'snapshotAndMonitorLegacyStorageAppOp(user.getUserHandle());' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'MATCH_UNINSTALLED_PACKAGES | MATCH_ANY_USER,' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mUidsWithLegacyExternalStorage.add(uid);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mUidsWithLegacyExternalStorage.remove(uid);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'PackageMonitor monitor = mPackageMonitorsForUser.remove(userId);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'return mUidsWithLegacyExternalStorage.contains(uid);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
```

## 13. mount计算不读反馈集合，而是实时查 permission、主 op与 legacy op

先分清两个正交开关：boot快照的 `ENABLE_ISOLATED_STORAGE`决定用哪套 mount算法，`mIsFuseEnabled`决定底层挂载与运行中更新怎样执行。前者开启时，`getExternalStorageMountMode()`进入 `getMountModeInternal()`；关闭时则遍历所有 `ExternalStorageMountPolicy`并取数值最小值，任何 NONE立即拒绝。AppOpsService只在这个关闭分支安装旧 mount policy，而那份 policy只 note READ/WRITE AppOp，不读取 LEGACY op。

以下普通决策属于 isolated-storage开启分支。`getMountModeInternal()`先处理 isolated、无包与 instant app；FUSE external-storage service、Downloads/ExternalStorageProvider和平台签名 MTP进程还有专门返回。普通应用随后计算 `hasRead`与 `hasWrite`，每一项都要求 permission和对应主 AppOp同时通过。

普通决策顺序是：

| 条件 | mount mode |
|---|---|
| WMS granted且 hasWrite | FULL |
| INSTALL_PACKAGES granted，或 shared UID任一包 REQUEST_INSTALL_PACKAGES op为 ALLOWED；并且 hasWrite | INSTALLER |
| LEGACY op为 ALLOWED且 hasWrite | WRITE |
| LEGACY op为 ALLOWED且 hasRead | READ |
| 其他 | DEFAULT |

isolated-storage开启时，这里直接向 AppOpsService查询 LEGACY，不读取 `mUidsWithLegacyExternalStorage`。因此反馈缓存错误会影响下一轮 soft策略，却不会直接污染本次 mount计算。反过来，LEGACY ALLOWED而 READ/WRITE双门都失败，仍只能得到 DEFAULT。关闭 isolated-storage时，旧 policy甚至不消费 LEGACY；此时 Environment仍可能按 compat/AppOps报告 legacy，与 mount fallback形成另一种分歧。

枚举里虽然存在 `MOUNT_EXTERNAL_LEGACY`，但这个正常决策函数不会返回它；legacy op最终选择的是 READ或WRITE。非 FUSE的 Zygote映射又让 LEGACY、WRITE与 INSTALLER都绑定 write视图，所以不能从常量名字反推生产路径。

## 14. “mode变化后生效”要按 FUSE、非 FUSE与新进程三条路拆开

新进程启动时，ProcessList同步问 StorageManagerInternal取得 mount mode，写入 `ProcessRecord.mountMode`并传给 Zygote。非 FUSE时，Zygote按 DEFAULT/READ/WRITE等选择 `/mnt/runtime/*`视图；vold的 `remountUid()`还能扫描现有进程、进入各自 mount namespace并换绑。

r48给 FUSE与 isolated storage的默认值都设为开启，但设备可分别覆盖。FUSE分支中，普通 DEFAULT/READ/WRITE都由 Zygote绑定同一个 `/mnt/user/<userId>`入口，真实可见性主要由 FUSE/MediaProvider在请求时裁决；vold看到 FUSE属性后让 `remountUid()`直接返回。此时 LEGACY op变化在 StorageManager内部只更新反馈集合，不 kill，也没有有效 remount。

非 FUSE配置下，`servicesReady()`为 REQUEST_INSTALL_PACKAGES与 LEGACY注册专用 watcher；callback还要求 boot快照的 isolated-storage功能开启且 UID活跃，才重算并调用 remount。另一条 AppOps同步内部回调会先让 FUSE专用的 REQUEST、MANAGE与 LEGACY分支提前返回；余下的 READ/WRITE，以及非 FUSE下的 REQUEST，只有新 mode为 ALLOWED且 user initialized时才调用重挂载，对降级不对称。

PMS成功 grant READ/WRITE也会在 user initialized时直接要求一次重算。它与 AppOps ALLOWED通知可能重复，但重复调用并不提供事务屏障；FUSE下最终又可能是 no-op。故“LEGACY改变就 remount或 kill”是错误模型：源码没有 LEGACY kill分支。

## 15. Environment的 compat真值表是并行语义门，不是 mount输入

`Environment.isExternalStorageLegacy()`在应用调用线程先排除 isolated和 instant app，再读取两个 compatibility change：

| DEFAULT_SCOPED_STORAGE | FORCE_ENABLE_SCOPED_STORAGE | 返回路径 |
|---:|---:|---|
| 1 | 1 | 直接 false，强制 scoped |
| 0 | 0 | 直接 true，强制 legacy |
| 1 | 0 | 查询 LEGACY op |
| 0 | 1 | 查询 LEGACY op |

DEFAULT change默认开启，FORCE被声明为默认关闭，所以通常落在“查 AppOp”的混合状态。FORCE单独为 true并不会强制 scoped，只有两者都 true才触发严格 enforced；两者都 false则绕过 AppOp直接报告 legacy。

这个 API不调用 StorageManagerService，也不核验当前进程 namespace。`isExternalStorageLegacy(File path)`在 r48实现里还完全不使用 path参数；它不是按卷裁决。于是 AppOp ALLOWED可能被双 true压成 false，AppOp非 ALLOWED也可能被双 false抬成 true，而 mount仍另受 READ/WRITE双门与 FUSE路径约束。

MediaProvider的 `LocalCallingIdentity`复制了同一组 change ID和两位公式，说明 compat是文件访问上层的并行政策输入；它并没有被 `getMountModeInternal()`读取。下一章再进入 FUSE/MediaProvider逐请求裁决。

### 练习 9：让 API结果、AppOp与 mount故意三者不同

先列四种 compat组合，再分别令 LEGACY=ALLOWED、READ denied与 FUSE开关变化。对每组写出 Environment返回、StorageManager mount mode和新进程绑定；不要把任一列当作另外两列的代理。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final long DEFAULT_SCOPED_STORAGE = 149924527L;' frameworks/base/core/java/android/os/Environment.java
grep -n -F 'private static final long FORCE_ENABLE_SCOPED_STORAGE = 132649864L;' frameworks/base/core/java/android/os/Environment.java
grep -n -F 'return defaultScopedStorage && forceEnableScopedStorage;' frameworks/base/core/java/android/os/Environment.java
grep -n -F 'return !defaultScopedStorage && !forceEnableScopedStorage;' frameworks/base/core/java/android/os/Environment.java
grep -n -F 'return appOps.checkOpNoThrow(AppOpsManager.OP_LEGACY_STORAGE,' frameworks/base/core/java/android/os/Environment.java
grep -n -F 'if (ENABLE_ISOLATED_STORAGE) {' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'for (ExternalStorageMountPolicy policy : mPolicies) {' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (!StorageManager.hasIsolatedStorage()) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final boolean hasRead = StorageManager.checkPermissionAndCheckOp' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (hasFull && hasWrite) {' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (hasLegacy && hasWrite) {' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mountExternal = storageManagerInternal.getExternalStorageMountMode(uid,' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'private static final boolean DEFAULT_FUSE_ENABLED = true;' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'BindMount(user_source, "/storage", fail_fn);' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F '"/mnt/runtime/write",   // MOUNT_EXTERNAL_LEGACY' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'case OP_LEGACY_STORAGE:' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (GetBoolProperty(android::vold::kPropFuse, false)) {' system/vold/VolumeManager.cpp
```

## 16. 用完成点矩阵收束排障，并把下一章边界留清楚

| 已观察到 | 可以证明 | 仍不能证明 |
|---|---|---|
| Manifest request或preserve为 true | 解析意图存在 | permission会 grant、LEGACY会 ALLOW |
| `mayGrantPermission()=true` | 这次 soft资格门允许继续 | permission已经写入、主 op已同步 |
| LEGACY raw为 ALLOWED | 某层 AppOps政策当前报告允许 | StorageManager缓存正确、READ/WRITE双门通过 |
| UID在 legacy反馈集合 | soft策略下一轮会看到 `L_u=true` | 当前 AppOps或 namespace仍为 legacy |
| `syncPackages()`返回 | 本轮候选 setter均返回 | AppOps XML提交、shared成员effective结果统一 |
| ProcessRecord记录 READ/WRITE | 启动时算出的 mount mode已保存 | FUSE逐请求访问一定通过 |
| Environment返回 true | compat/AppOps这条 API公式成立 | 实际 mount宽度或任意文件操作成功 |

排查时按“解析输入→真实 user与完整 UID→permission及 flags→三项策略结果→候选优先级与 raw遮蔽→AppOps内存→Storage反馈缓存→新/旧进程路径→compat API”逐层记录。尤其要保留包名：forced、preserve、flags和 package override都是包级，而最终 UID mode、WMS、request聚合与 mount namespace又跨成员共享。

第272章的核心结论是：soft-restricted存储策略维护的是迁移中的非对称建议，不是最终文件能力。permission grant、LEGACY政策、反馈缓存、mount和 compat API各有自己的时钟，shared UID与 r48的跨用户输入差异又会放大偏差。下一章进入 Android 11 scoped storage主链，继续拆 MediaProvider/FUSE、应用隔离目录、MediaStore授权与 `MANAGE_EXTERNAL_STORAGE`。
