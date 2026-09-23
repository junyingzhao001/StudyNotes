# 262 Android PermissionManager安装后权限更新、运行时权限继承、撤销、shared UID与GID变化链

## 1. 更新保留的不是旧权限表，而是对新请求仍有效的旧决定

第261章说明更新会保留`PackageSetting`与AppData。本章继续回答更敏感的问题：新代码为什么还能使用用户此前允许的相机权限，删掉的请求何时失效，新加的危险权限为何通常仍是拒绝，以及shared UID和Linux supplementary GID为什么会让“更新时杀过进程”不再是充分证明。

先拆开五本账：

| 账本 | 典型对象 | 更新时真正要问的问题 |
|---|---|---|
| 权限定义 | `BasePermission`、`<permission>` | 谁拥有，protection、group与GID是什么 |
| 解析后请求 | `pkg.getRequestedPermissions()` | 新包最终请求了什么，包括兼容补入项 |
| 授权状态 | `PermissionsState` | install/runtime grant与flags怎样投影 |
| 策略状态 | restricted exemption、AppOps | “已grant”之后是否仍受限制 |
| 运行进程 | UID、supplementary groups、FD | 内存中的旧能力何时退出 |

普通、非shared UID包替换的中心算法是“深复制旧state作为参考，reset当前state，再遍历解析后的新请求重建”。但这句话有三个限定：解析器可能把split或兼容权限补回请求列表；post-install可按安装flag显式预授；定义、group与storage范围还有跨包异步撤销。

因此至少要区分六个完成点：新定义已入表、本包基础权限state已恢复、全局Settings已写、每用户runtime文件已写、post-install白名单与预授已完成、旧进程已真正退出。任一前点都不能替代后点。

## 2. commit先发布定义，再在同一包锁内恢复本包权限状态

`installPackagesLI()`整体仍持`mInstallLock`；reconcile与`commitPackagesLocked()`另持PMS的`mLock`。对每个包，commit先调用`commitReconciledScanResultLocked()`：新`PackageSetting`、`AndroidPackage`、组件和AppsFilter入表，随后`addAllPermissionGroups()`与`addAllPermissions()`发布本包声明的定义。

若旧包或定义变化需要跨包审计，这一步只调用`AsyncTask.execute()`排后台工作。外层`mLock`尚未释放，后台任务即使开始，也不能把需要该锁的state变更插进当前同步提交。

`commitPackagesLocked()`接着在同一个外层`mLock`里调用`updateSettingsLI()`。它进入`updateSettingsInternalLI()`后执行`mPermissionManager.updatePermissions(pkgName, pkg)`，填安装结果并同步调用`mSettings.writeLPr()`。PermissionManager构造时直接令自己的`mLock = externalLock`，所以这里的“PMS锁”和“权限锁”在r48其实是同一对象的重入，而不是两把可任意交叉的锁。

这给包查询提供一个中心可见性边界：其他需要`mLock`的读者不会在“新Manifest已入表、本包基础state尚未恢复”的中间穿过。但它不覆盖已经排出的异步全局撤销、runtime文件落盘、post-install预授和进程退出。

### 练习 1：画出定义、状态、落盘与post-install的锁边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void commitPackagesLocked(final CommitRequest request) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'AndroidPackage pkg = commitReconciledScanResultLocked(reconciledPkg, request.mAllUsers);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.addAllPermissionGroups(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.addAllPermissions(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'AsyncTask.execute(() -> {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'updateSettingsLI(pkg, reconciledPkg.installArgs, request.mAllUsers, res);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.updatePermissions(pkgName, pkg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mSettings.writeLPr();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mLock = externalLock;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'executePostCommitSteps(commitRequest);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

把“持`mInstallLock`”“再持`mLock`”“后台任务已排队”“runtime文件已落盘”画成四条独立时间线。指出`mSettings.writeLPr()`返回时哪些仍未完成。

## 3. updatePermissions先修owner，再决定重建一个包还是全体包

目标包写入入口只要传入非null的package，就固定带`UPDATE_PERMISSIONS_REPLACE_PKG`，首次安装也不例外；删除入口传null，并额外带`UPDATE_PERMISSIONS_ALL`。这里的replace位是PermissionManager内部“重建目标状态”的范围，不是`INSTALL_REPLACE_EXISTING`，更不能单靠它判断这是不是APK替换。

内部先执行`updatePermissionTreeSourcePackage()`与`updatePermissionSourcePackage()`。若tree或permission的source package可能变化，就补上`UPDATE_PERMISSIONS_ALL`，因为signature授权依赖定义者，不能只看正在更新的包。这个返回值是保守dirty信号：更新包只要命中某个当前source就可能置true，不要求先证明owner字段真的不同。随后才进入`restorePermissionState()`。

全量循环会跳过changing package，先处理其他包，最后单独处理changing package。其他包只有`UPDATE_PERMISSIONS_REPLACE_ALL`存在且volume相等时才以replace方式重建；普通单包更新没有这个bit。changing package的volume UUID由同一个`pkg`同时生成比较两端，标准单包路径会相等；volume条件真正有区分力的是全量、挂载和SDK升级类调用。

background→foreground permission映射在首次调用时缓存一次。源码依据是“background permission只由system定义”，这是Android 11的实现假设，不应外推成后续模块化版本的永久契约。

### 练习 2：拆开replace、all与volume三个位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final int UPDATE_PERMISSIONS_ALL = 1 << 0;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static final int UPDATE_PERMISSIONS_REPLACE_PKG = 1 << 1;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static final int UPDATE_PERMISSIONS_REPLACE_ALL = 1 << 2;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '(pkg == null ? UPDATE_PERMISSIONS_ALL | UPDATE_PERMISSIONS_REPLACE_PKG' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean permissionTreesSourcePackageChanged = updatePermissionTreeSourcePackage(' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean permissionSourcePackageChanged = updatePermissionSourcePackage(changingPkgName,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'flags |= UPDATE_PERMISSIONS_ALL;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'cacheBackgroundToForegoundPermissionMapping();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (pkg == changingPkg) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final boolean replace = replaceAll && Objects.equals(replaceVolumeUuid, volumeUuid);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '&& Objects.equals(replaceVolumeUuid, volumeUuid);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return StorageManager.UUID_PRIMARY_PHYSICAL;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

分别推演普通更新、包删除、`updateAllPermissions(..., false)`与SDK更新。不要把“遍历了全部包”自动等同于“全部包都reset”。

## 4. PermissionsState同时保存grant、flags、用户维度和GID输入

`PermissionsState`用permission name定位`PermissionData`。同一个名字可以有`USER_ALL`上的install state，也可以有各userId上的runtime state；每个state又把`granted`和flags分开。未grant但`USER_FIXED`、已grant且`POLICY_FIXED`、已grant但`APPLY_RESTRICTION`是三种不同事实。

normal与合法signature通常是全用户共享的install permission；现代应用的dangerous权限按用户记录runtime state。legacy应用的dangerous权限在r48也常用每用户runtime grant配合`FLAG_PERMISSION_REVIEW_REQUIRED`、`FLAG_PERMISSION_REVOKED_COMPAT`表达兼容语义，不能机械照抄方法头那段已经漂移的旧注释。

`new PermissionsState(old)`会逐个构造新的`PermissionData`，复制global GIDs、missing与review集合，是深状态快照，不是Map浅别名。`reset()`则清`mPermissions`、global GIDs、missing与review账。随后`setGlobalGids()`再装回系统配置的全局GID基线。

包使用shared UID时，`PackageSetting.getPermissionsState()`会委托`SharedUserSetting`；物理上每个`SettingBase`都有字段，但权限判定的权威对象是共享state。这一区别决定了下一节能否reset。

## 5. 普通replace把旧state当参考，用解析后请求构造新结果

`restorePermissionState()`先处理missing用户，再令`origPermissions = permissionsState`。replace时清`installPermissionsFixed`；普通包随后深复制旧state并reset当前对象，shared UID则不做这两步。

重建白名单不是XML中肉眼可见的`<uses-permission>`集合，而是解析完成后的`pkg.getRequestedPermissions()`。`ParsingPackageUtils.convertNewPermissions()`与`convertSplitPermissions()`可能按targetSdk把兼容权限同时加入requested与implicit列表。因此开发者删掉一个显式声明，不代表解析后的列表一定没有这个名字。

对每个解析后请求，PermissionManager先查`BasePermission`及其source setting。定义不存在就跳过；普通包已经reset，所以旧同名state不会被写回。新列表里完全没有的名字也不会被遍历。最终效果是“以新解析模型投影旧决定”，而不是旧表上增量打补丁。

这不是通用数据库事务。reset后的内存变更在外层`mLock`内完成，但跨文件持久化、异步审计与进程状态仍有各自完成点；commit中途异常也没有逐字段undo。

### 练习 3：验证深复制、reset与解析后白名单

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'PermissionsState origPermissions = permissionsState;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'ps.setInstallPermissionsFixed(false);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'origPermissions = new PermissionsState(permissionsState);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permissionsState.reset();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'updatedUserIds = revokeUnusedSharedUserPermissionsLocked(' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permissionsState.setGlobalGids(mGlobalGids);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final int N = pkg.getRequestedPermissions().size();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (bp == null || getSourcePackageSetting(bp) == null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPermissions.put(name, new PermissionData(permissionData));' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'mPermissions = null;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'convertNewPermissions(pkg);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'convertSplitPermissions(pkg);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
```

用四个输入推演最终state：旧CAMERA已grant且仍请求、旧CAMERA已拒绝且带USER_FIXED、显式删除但split规则补回、新增未知权限。每一步都区分解析请求、grant和flags。

## 6. normal、runtime与signature走三套grant判定

重建循环先判permission种类：

| 定义与请求 | 主要分支 | replace后的典型结果 |
|---|---|---|
| unknown或source setting缺失 | 跳过 | 普通包旧state不回填 |
| normal | `GRANT_INSTALL` | 作为install permission自动grant |
| dangerous/runtime | `GRANT_RUNTIME`或`GRANT_UPGRADE` | 继承旧决定或迁移旧表示 |
| signature | `grantSignaturePermission()` | 通过签名、privileged/OEM等规则才grant |
| runtime-only + legacy target | 入口拒绝 | 不用legacy兼容自动grant兜底 |

`installPermissionsFixed`主要阻止已有非system包在非replace重扫时随便捡到新的非runtime能力。replace已先把它清为false，所以更新后的normal权限可以重新按新Manifest裁决；signature允许仍必须经过自己的授权函数。循环末再把选择固定。

`GRANT_INSTALL`还会检查旧state中是否存在同名runtime表示：若有，先从旧参考state撤销runtime并清flags，再在新state授install。这处理的是permission表示类型变化，不等于安装器允许应用任意降低targetSdk。

### 练习 4：建立grant类型决策表

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (bp.isRuntimeOnly() && !appSupportsRuntimePermissions) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (bp.isNormal()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '} else if (bp.isRuntime()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'origPermissions.hasInstallPermission(bp.getName())' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '} else if (bp.isSignature()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'allowedSig = grantSignaturePermission(perm, pkg, ps, bp, origPermissions);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!ps.isSystem() && ps.areInstallPermissionsFixed() && !bp.isRuntime()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!allowedSig && !origPermissions.hasInstallPermission(perm)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'case GRANT_INSTALL: {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'case GRANT_RUNTIME: {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'case GRANT_UPGRADE: {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'ps.setInstallPermissionsFixed(true);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

为新增normal、新增dangerous、合法signature、非法signature、runtime-only legacy五例写出grant枚举和最终state。不要把`allowedSig`等同于“同包名更新”。

## 7. modern dangerous继承旧决定，legacy与GRANT_UPGRADE是另外两条路

现代应用走`GRANT_RUNTIME`时，从旧快照按用户取得`PermissionState`。旧state已grant且hard restriction未阻断，就向新state重新grant；旧拒绝不会翻成允许。旧flags经过review、compat和restriction调整后，以`MASK_PERMISSION_FLAGS_ALL`写入新state，所以USER_FIXED之类的拒绝决定可以连续。

新危险权限若没有旧state，基础重建不会凭“这是一次更新”自动grant。但它可能是split产生的implicit权限，稍后继承source；也可能在post-install被受权安装器显式预授。这两个例外不应反向改写基础规则。

源码中“重新grant返回`PERMISSION_OPERATION_FAILURE`时才令`wasChanged=true`”不是成功判断写反。`updatedUserIds`记录的是最终state相对旧持久状态是否变化：重新grant成功只是重建出磁盘已有的旧grant；失败才把“旧grant”变成“新拒绝”，需要写回和通知。

legacy target会把可兼容的runtime权限按用户grant，并设置review/revoked-compat或restriction flags。`GRANT_UPGRADE`的直接条件则是旧state仍把当前runtime permission保存为install grant，或命中Activity Recognition迁移特例；它先撤旧install表示，再向各用户建立runtime表示。它不等同于“任意pre-M应用这次把targetSdk升到M+”。

## 8. restricted裁决依赖用户、策略就绪和三类exemption

restore最先遍历`getUserIdsIncludingPreCreated()`。若某用户state标记missing，它根据解析请求补合理默认：平台runtime restricted权限得到upgrade exemption；legacy target还会grant并加review/revoked-compat。源码只证明“state缺失时修复”，不能把缺失原因固定写成rollback。

正式`GRANT_RUNTIME`又按用户计算四个输入：PermissionPolicy是否initialized、hard/soft属性、system/upgrade/installer任一exemption、旧`APPLY_RESTRICTION`。结果不是一个boolean：

- modern + hard restricted + 已初始化 + 无exemption：不把旧grant回填，并设置APPLY_RESTRICTION；由于新state已reset，代码里的revoke通常只是空state上的no-op。
- modern + soft restricted + 无exemption：旧grant可以回填，但加APPLY_RESTRICTION，最终使用还要看policy与AppOps。
- policy未初始化：暂不做最终restricted剥夺，等待后续重评。
- 已解除restriction或已有exemption：清APPLY_RESTRICTION；legacy target还可能重新要求review。

installer whitelist在POST_INSTALL才写installer-exempt flag，并以`replace=false`再跑一次restore。whitelist提供“可授/可不受限制”的条件，本身不是grant。

### 练习 5：推演missing、hard、soft与exemption

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (permissionsState.isMissing(userId)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return UserManagerService.getInstance().getUserIdsIncludingPreCreated();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FLAG_PERMISSION_RESTRICTION_UPGRADE_EXEMPT' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'targetSdkVersion = Math.min(targetSdkVersion,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final boolean permissionPolicyInitialized =' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean hardRestricted = bp.isHardRestricted();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'boolean softRestricted = bp.isSoftRestricted();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FLAGS_PERMISSION_RESTRICTION_ANY_EXEMPT' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'flags |= FLAG_PERMISSION_APPLY_RESTRICTION;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'flags &= ~FLAG_PERMISSION_APPLY_RESTRICTION;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (permissionsState.grantRuntimePermission(bp, userId)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'callback.onPermissionUpdated(updatedUserIds, runtimePermissionsRevoked);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

为两个用户分别设置policy未初始化、hard无豁免、soft无豁免和installer exemption，写出grant、APPLY_RESTRICTION、review与是否需要持久化，不只写“允许/拒绝”。

## 9. shared UID不reset，而是按所有成员的解析请求并集裁剪

shared UID的`PackageSetting.getPermissionsState()`委托同一个`SharedUserSetting`。更新成员A时若reset，会连成员B持有的能力一起抹掉，因此replace分支保留共享对象，只调用`revokeUnusedSharedUserPermissionsLocked()`。

helper遍历`SharedUserSetting.getPackages()`的所有成员，把定义仍存在的解析后requested permission加入`usedPermissions`。它不按某用户是否安装该成员过滤：B只装在user 0，只要仍是sharedUser成员，它的请求也会阻止user 10的同名共享state被裁。

随后全局install state与每用户runtime state分别倒序扫描。只有名字不在used集合且当前`BasePermission`非null时才撤grant并清全部flags；unknown或定义已经被移除的残留项不会由这个helper删除。若A删CAMERA而B仍请求，整个UID继续持有；只有所有成员都不请求，才按各用户裁掉。

missing修复同样按全部成员请求并集，并取成员最小targetSdk；但这个最小值只服务missing用户的默认修复，不是整段grant算法的统一targetSdk。正常主循环仍以当前正在restore的`pkg.getTargetSdkVersion()`判断runtime支持。runtime扫描只要裁掉一个unused `PermissionState`就把该user加入同步集合，即使该项本来是denied但还带flags；所以`runtimePermissionsRevoked`在这里更准确地表示“共享runtime状态项被裁”，不只表示已grant位从true变false。callback据此触发本章唯一明确的定向同步runtime文件写。

### 练习 6：手算shared UID的成员并集与用户范围

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return (sharedUser != null)' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'final List<AndroidPackage> pkgList = suSetting.getPackages();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final ArraySet<String> usedPermissions = new ArraySet<>();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'usedPermissions.add(permission);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!usedPermissions.contains(permissionState.getName())) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permissionsState.revokeInstallPermission(bp);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permissionsState.revokeRuntimePermission(bp, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'runtimePermissionsRevoked = true;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'List<AndroidPackage> packages = ps.getSharedUser().getPackages();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'targetSdkVersion = Math.min(targetSdkVersion,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'sharedUserPermissions.put(sharedUserName, permissions);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
```

构造A、B共享UID，B只安装在user 0。分别推演“A删除CAMERA、B仍请求”“A与B都删除”“定义已不存在”三例在user 0与user 10的结果。

## 10. split permission先改请求集合，随后才继承旧state

解析器在restore之前已经执行`convertNewPermissions()`与`convertSplitPermissions()`：targetSdk低于split门槛且请求source permission时，新名字会同时进入requested与implicit列表。这就是为什么“删掉新权限的显式声明”仍可能保留一个隐式请求。

restore先收集旧state从未见过的新implicit permission，尾部再建立new→source集合。若任一source原本以runtime或install形式granted，新权限获得runtime grant；flags按源码的宽松合并规则继承。大多数新implicit权限还加`FLAG_PERMISSION_REVOKE_WHEN_REQUESTED`：应用日后显式请求、且没有blocking flags时，系统可撤掉兼容自动grant，让它回到正常请求流程。

Activity Recognition是单独兼容分支：即使不再属于parser意义的implicit列表，只要旧split source仍以install形式存在，也可进入迁移。通用继承函数明确不处理foreground/background permission；后台位置不能靠这段逻辑直接类推。

另一个名字相近但不同的尾段是`checkIfLegacyStorageOpsNeedToBeUpdated()`。replace包请求legacy external storage并包含READ/WRITE时，它把全部用户标为updated，推动PermissionPolicy同步`OP_LEGACY_STORAGE`；这不是新增一个permission grant。

### 练习 7：把解析补入、implicit继承与AppOp分开

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'pkg.addRequestedPermission(npi.name)' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'pkg.addRequestedPermission(perm)' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F '.addImplicitPermission(perm);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'newImplicitPermissions.add(permName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!origPs.hasRequestedPermission(sourcePerms)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FLAG_PERMISSION_REVOKE_WHEN_REQUESTED' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'ps.grantRuntimePermission(mSettings.getPermissionLocked(newPerm), userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Warning: This does not handle foreground / background permissions' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permName.equals(Manifest.permission.ACTIVITY_RECOGNITION)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (replace && pkg.isRequestLegacyExternalStorage() && (' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return getAllUserIds();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

从一条split配置出发，写出source、新permission、targetSdk、requested列表、implicit列表、旧grant、继承flags和AppOp通知八列，避免把解析兼容与授权混成一步。

## 11. packages.xml先写，runtime文件另有同步与延迟两种路径

install permission随package或shared-user条目写入全局`packages.xml`。`updateSettingsInternalLI()`在包锁内同步调用`mSettings.writeLPr()`；这个函数写完全局文件后，还调用`writeAllRuntimePermissionsLPr()`，但这里只是为当前created users逐个安排runtime权限异步写。

runtime持久层按用户生成两张Map：非shared包按packageName，shared UID按sharedUserName只写一次；每项保存name、granted和flags，并带version/fingerprint。异步调度默认200ms，连续修改可防抖，但从首个未写mutation起最多约2000ms。

`restorePermissionState()`把updated users和`runtimePermissionsRevoked`交给default callback。普通变化走异步定向写；shared UID unused prune把sync设为true，`writePermissionSettings(userIds, false)`最终进入`writePermissionsForUserSyncLPr()`，先移除该用户待处理消息，再在当前调用中写runtime快照。

显式runtime revoke的保证更弱于原稿常见说法：`onPermissionRevoked()`同步调用`writeSettings(false)`，这会同步写packages.xml，却仍只异步调度所有用户runtime文件；kill又排在PermissionManager自己的Handler。runtime写Handler与kill Handler之间没有完成顺序，不能把“回调返回”解释成撤销已同步落入runtime文件。

## 12. GID变化影响下一次exec，restore本身却不发GID callback

`BasePermission`可带system config提供的GID；`perUser=true`时先用`UserHandle.getUid(userId, gid)`变换。`PermissionsState.computeGids()`从global GIDs出发，把该用户全部已grant permission的GID用set-like append合并。PMS的`getPackageGids()`只提供permission-derived GIDs；ProcessList还会按`deniedPermissions`删项，并加入sharedApp、cacheApp、user、mount等GID，再把最终groups交给Zygote。因此`computeGids()`不是进程最终Linux groups的完整快照。

`PermissionsState.grantPermission()`与`revokePermission()`只在当前permission含GID时比较前后数组长度。这里的长度比较不是集合等价检查写漏：单次grant只能从old集合做并集，单次revoke只能做子集差，`ArrayUtils.appendInt`还去重；在串行不变量下，长度不变就表示有效GID集合没变。

显式runtime grant若收到`PERMISSION_OPERATION_SUCCESS_GIDS_CHANGED`，default callback把`killUid`排到PermissionManager Handler；显式revoke不转发这个返回值，但总会走`onPermissionRevoked()`并排kill。随后`killUidForPermissionChange()`按appId、userId且`packageName=null`选择整个UID，`evenPersistent=true`，比第261章的package-specific freezer kill更强。返回仍不等于内核已确认所有PID死亡。

本章的restore循环却不把内部grant/revoke返回值转发为`onGidsChanged()`，尾部只有`onPermissionUpdated()`。普通replace通常靠PackageFreezer覆盖目标包；但freezer构造只是向AMS发出package-specific kill请求并阻止匹配进程新启动，不是restore开始前“内核已确认旧PID死亡”的屏障。`INSTALL_DONT_KILL_APP`则会直接留下旧supplementary groups、FD和缓存能力窗口。

shared UID还有更尖的反例：更新A并裁掉UID级GID权限时，freezer调用的是packageName=A、shared appId。ProcessList的package-specific筛选还要求ProcessRecord的`pkgList`包含A或依赖A；只运行B的同shared-UID进程可能幸存，而restore又没有GID callback。逻辑权限state已经收缩，不等于该UID的所有旧Linux进程都换了groups。

风险还不限于shared UID。定义owner变化可补上`UPDATE_PERMISSIONS_ALL`并同步重评其他消费者；这些第三方包不在owner包的freezer覆盖范围内。若它们的install permission被撤销且该permission带GID，这条restore/定义清理路径同样没有把grant/revoke返回值转成UID级kill；异步definition审计又只处理runtime permission。于是owner更新结束时，第三方内存state可以已收缩，而既有进程仍暂时保留旧supplementary group。

### 练习 8：核对落盘Handler、GID集合和两种kill范围

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'writeAllRuntimePermissionsLPr();' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final long WRITE_PERMISSIONS_DELAY_MILLIS = 200;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final long MAX_WRITE_PERMISSIONS_DELAY_MILLIS = 2000;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'writePermissionsForUserSyncLPr(int userId)' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'packagePermissions.put(packageName, permissions);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'sharedUserPermissions.put(sharedUserName, permissions);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'public int[] computeGids(int userId) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'if (oldGids.length != newGids.length) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'return appendInt(cur, val, false);' frameworks/base/core/java/com/android/internal/util/ArrayUtils.java
grep -n -F 'mHandler.post(() -> killUid(appId, userId, KILL_APP_REASON_GIDS_CHANGED));' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'am.killUidForPermissionChange(appId, userId, reason);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'true /* evenPersistent */,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'permGids = pm.getPackageGids(app.info.packageName,' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'if (app.processInfo != null && app.processInfo.deniedPermissions != null) {' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'gids = computeGidsForProcess(mountExternal, uid, permGids);' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'killApplication(ps.name, ps.appId, userId, killReason);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Message msg = mHandler.obtainMessage(KILL_APPLICATION_MSG);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'pkgList.put(_info.packageName, new ProcessStats.ProcessStateHolder(_info.longVersionCode));' frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
grep -n -F 'if (!app.pkgList.containsKey(packageName) && !isDep) {' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
```

分别推演新增GID权限、撤销GID权限、普通no-kill更新、shared A更新但仅B进程存活。为每例标出Java权限检查、runtime文件、supplementary groups和PID四个完成点。

## 13. 更新权限定义者时，定义删除与使用者state恢复是同步主链

包自身声明`<permission>`时，`addAllPermissions()`通过`BasePermission.createOrUpdate()`处理owner与protection；从non-runtime变成runtime或owner被system包取代时，`mPermissionDefinitionChanged`会置真，供稍后的异步安全审计使用。

同步`updatePermissions()`还会扫描旧source package拥有的定义。若新包已不再声明某名字，runtime定义会立即遍历created users和当前包集合，尝试从使用者撤销；non-runtime定义则从各PackageSetting撤install state，随后把定义从表中移除。这个动作发生在本包基础restore之前、仍在外层`mLock`内。

runtime helper会对targetSdk低于M的使用者提前返回。因此“定义者删除权限会同步清干净所有使用者state”并不成立。定义从表中消失后，常规permission check已无法再把它当有效定义，但legacy残留state与旧进程是另一份清理账。

source/tree变化返回true时还会把restore范围扩大到所有包。其他包在这次循环中通常以replace=false重评；它们不是全部clone/reset。

## 14. group、definition与storage安全审计是无join的后台任务

commit在新定义入表后快照`mPackages.keySet()`，并向`AsyncTask`提交三类工作：

1. 旧包存在时，比较它声明的dangerous permission group。新group非null且与旧group不同，遍历快照包名和created users撤销持有者。
2. `mPermissionDefinitionChanged`非空时，对变成runtime或owner变化的permission遍历使用者；UID低于FIRST_APPLICATION_UID，或带SYSTEM_FIXED、POLICY_FIXED、GRANTED_BY_DEFAULT、GRANTED_BY_ROLE的grant会保留。
3. 新包新请求legacy storage或targetSdk从Q以上降到Q以下时，逐用户尝试撤销新包请求的storage runtime permissions；它只检查新包，legacy与fixed policy仍可能使撤销跳过或失败。

这三路没有Future、token或安装完成join。快照只保存包名，不保存PackageSetting代际；任务真正运行时再解析live package与state，迟到任务可能遇到卸载、重装或下一次更新。created user集合也与同步restore使用的including-pre-created集合不同。

失败处理并不一致。definition审计每个revoke捕获`Exception`；storage审计捕获`IllegalStateException | SecurityException`；group审计却只捕获`IllegalArgumentException`。若遇POLICY_FIXED导致`SecurityException`，group任务可在中途退出，后续包与用户没有完成证明。

group从旧值移到null不会触发这条撤销，但这不是明显的“漏判bug”：代码要防的是进入一个新group后借联动授权扩大能力；解除分组不会获得新group联动。读版本边界时应记录条件，而不是仅凭非对称就判错。

### 练习 9：联合追定义删除、异步撤销与post-install覆盖

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (pkg == null || !hasPermission(pkg, bp.getName())) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'revokePermissionFromPackageForUser(p.getPackageName(),' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'appInfo.targetSdkVersion < Build.VERSION_CODES.M' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (bp.isRuntime() && (ownerChanged || wasNonRuntime)) {' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'final ArrayList<String> allPackageNames = new ArrayList<>(mPackages.keySet());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.revokeRuntimePermissionsIfGroupChanged(pkg, oldPkg,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.revokeStoragePermissionsIfScopeExpanded(pkg, oldPkg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.revokeRuntimePermissionsIfPermissionDefinitionChanged(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (newPermissionGroupName != null' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'catch (IllegalArgumentException e) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'catch (IllegalStateException | SecurityException e) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FLAG_PERMISSION_GRANTED_BY_ROLE;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPermissionManager.setWhitelistedRestrictedPermissions(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.grantRequestedRuntimePermissions(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

分别构造group A→B、group A→null、normal→runtime定义变化、POLICY_FIXED持有者、storage scope扩大五例。写出同步基础restore、后台审计、runtime落盘和kill各自最早可证的时刻。

## 15. POST_INSTALL的whitelist与预授是第二层覆盖，不属于基础restore

基础restore完成并不等于安装时权限工作结束。`handlePackagePostInstall()`在标准PACKAGE_ADDED之前，先写installer restricted whitelist，再按`INSTALL_GRANT_RUNTIME_PERMISSIONS`派生的`grantPermissions`决定是否调用`grantRequestedRuntimePermissions()`。

whitelist实现只遍历解析后requested permission，修改system/upgrade/installer exemption flags。只要flags实际变化，就以`replace=false`重跑restore；若此前已grant的restricted权限因此消失，再调用revocation callback排kill。它不会仅因进入白名单就自动grant。

预授同样只遍历解析请求，并可再受`grantedPermissions`名单收窄。目标还必须是runtime或development；instant包只能获instant允许权限，legacy不能获runtime-only，modern的SYSTEM_FIXED或POLICY_FIXED不允许安装器改写，底层仍复查hard/soft restriction。legacy“全部预授”主要是清review/revoked-compat flags。

这些步骤位于后来的POST_INSTALL消息，不在前面持`mInstallLock`的提交临界区。广播调用排在其后，所以正常控制流下receiver看到的是覆盖后的内存state；但runtime文件可能仍在200ms防抖窗口，后台definition/group/storage审计也可能尚未收敛。广播、install observer或包可查询都不是“权限世界最终完成”的ACK。

## 16. 用状态矩阵识别r48的真实边界

| 现场 | 应先检查 | 不能下的结论 |
|---|---|---|
| 新增normal立即可用 | GRANT_INSTALL与定义有效性 | 所有新增权限都会自动grant |
| 旧CAMERA拒绝仍保留USER_FIXED | 旧PermissionState与flags回填 | reset把用户决定清空了 |
| 删显式权限后名字仍在state | parser split/new compat补入 | restore忽略了新Manifest |
| shared A删权限但UID仍持有 | B是否仍是成员并请求 | revoke失效 |
| B未装某用户却阻止该用户裁剪 | shared used集合不看per-user installed | 权限是逐包拥有 |
| 内存已撤销但runtime文件仍旧 | 200ms/2s writer与跨Handler时序 | onPermissionRevoked已同步写runtime文件 |
| shared GID已裁但B进程仍活 | package-specific freezer的pkgList过滤 | shared appId保证全UID被杀 |
| group审计只处理了前几个包 | POLICY_FIXED SecurityException与catch范围 | AsyncTask完成了全部快照 |
| group移到null未撤销 | `newPermissionGroupName != null` | 单凭不对称即可认定安全bug |

最后校正三个容易误判的r48细节。第一，modern旧grant重建时`== PERMISSION_OPERATION_FAILURE`才标updated，是差异记账，不是成功条件倒置。第二，单次GID集合只会单调加或减，长度比较在该不变量内足够。第三，group→null没有新组联动风险，真实风险是异步任务无join、只按包名快照及异常处理不对称。

本章的最终答案是：普通包以解析后的新请求重建权限state，shared UID以全部成员请求并集裁剪；基础state、全局Settings、每用户runtime文件、AppOps、后台安全审计与进程groups分别完成。第263章将继续进入日常runtime权限入口，追PermissionController UI、Binder验权、fixed flags、一次性权限、自动撤销和AppOps同步。
