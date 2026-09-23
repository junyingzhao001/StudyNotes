# 261 Android Package替换安装、PackageFreezer、旧进程终止、旧代码删除与用户数据保留链

## 1. 更新不是覆盖文件，而是让五个世界在不同完成点交接

第260章把候选APK送过prepare、scan、reconcile、commit与post-commit。本章只看replace：为什么账号、数据库和UID通常连续，旧进程与旧APK却要退出；也解释为什么安装器已经收到成功时，旧代码仍可能存在，广播接收者甚至还没运行。

先把五类对象拆开：旧进程由AMS管理；旧base/split/native/dex是磁盘资源；`PackageSetting`保存appId与每用户账；AppData保存应用业务数据；新`AndroidPackage`则是候选代码的解析模型。替换不是让五者同时翻面，而是按下列坐标逐步推进：

| 坐标 | 已经成立 | 仍不能推出 |
|---|---|---|
| `replace=true` | 请求获准覆盖一个现有身份 | 新签名能继承旧数据 |
| freezer构造返回 | 包名可能已冻结，kill请求可能已入AMS队列 | 旧进程已经死亡 |
| `DELETE_KEEP_DATA`删除计划成立 | 普通旧包允许按保数据方式撤下 | 旧注册或旧代码已经删除 |
| `commitPackagesLocked()`返回 | 活动包模型与Settings已经切到新版本 | AppData、profile和结果回程完成 |
| post-commit返回 | 资源准备链正常走完 | POST_INSTALL已经解冻 |
| 广播发送调用返回 | 对应发送任务已排入PMS Handler | AMS已接收或receiver已执行 |
| `doPostDeleteLI()`返回 | 旧资源清理被尝试 | 文件与dex一定消失 |
| install observer成功 | 安装结果已交付调用者 | no-kill旧代码已删除 |

本文的install observer特指`IPackageInstallObserver2`结果回调。post-commit较早调用的`notifyPackageChangeObserversOnUpdate()`是另一套包变化观察者，不能拿它替代安装结果完成点。

核心结论是：替换保留的是经签名允许的连续身份，不是旧世界的每个字节；它依靠预检查、冻结、锁内切换和锁外补偿收敛，也不是带通用undo log的ACID事务。

## 2. replace先由flag与活动包判定，静态共享库还有合成包名

`preparePackageLI()`完整解析候选后，先令`replace=false`。只有`INSTALL_REPLACE_EXISTING`存在，PMS才在`mLock`下查活动包：普通情况要求`mPackages`已有同名包；rename迁移情况还要求候选`originalPackages`包含Settings保存的旧名，并确实存在该旧名活动包，然后把候选包名改回旧名。

所以“同包名已存在”不等于替换获准。缺少replace flag时，后面的new-package分支会以`INSTALL_FAILED_ALREADY_EXISTS`拒绝；同样，flag存在但活动包表没有对应身份时仍是新装语义。prepare稍后又重新读取`oldPackage`和`PackageSetting`，因为Session校验后的包世界可能已经变化。

静态共享库先按`staticSharedLibVersion`改成合成包名。常规的新库版本因此是另一个包身份，不走replace；命中同一合成名后，prepare还要求package的`longVersionCode`相等，只有两条版本轴都相同的开发式覆盖才可能进入这条链。

### 练习 1：从flag、rename与活动表还原replace

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'boolean replace = false;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((installFlags & PackageManager.INSTALL_REPLACE_EXISTING) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'String oldName = mSettings.getRenamedPackageLPr(pkgName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& mPackages.containsKey(oldName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if (mPackages.containsKey(pkgName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'PackageSetting ps = mSettings.mPackages.get(pkgName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.origUsers = ps.queryInstalledUsers(mUserManager.getUserIds(), true);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'renameStaticSharedLibraryPackage(parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'existingPkg.getLongVersionCode()' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

分别手算“同名但无replace flag”“rename映射命中”“有flag但活动表无包”和“静态库新版本”四例。标出最终包名、replace值和失败位置，不能把Settings里存在历史记录等同于当前活动版本。

## 3. prepare内两轮身份复核后，reconcile提交前还会再检查

保留AppData意味着新代码能以原appId读取旧数据库、密钥材料与私有文件，因此签名检查不是形式步骤。prepare早期先针对现有`PackageSetting`做upgrade keyset或`verifySignatures()`快速检查，避免候选在权限重定义等后续步骤走得过远；freezer建立后，replace分支再取一次旧`AndroidPackage`与Setting，执行面向替换的完整复核。

第二轮优先检查upgrade keyset。否则，新签名必须对旧签名具有`INSTALLED_DATA`能力，或旧签名对新签名具有`ROLLBACK`能力；这支持受控证书轮换和回滚，却不等于任意同作者声明。系统包若保存`restrictUpdateHash`，还会按base、split顺序计算SHA-512并要求完全相同。

scan完成后，`reconcilePackagesLocked()`还会以scan得到的Setting、disabled system基线和批次版本信息第三次执行upgrade-keyset或`verifySignatures()`，并决定是否带着keyset/SigningDetails更新进入commit。因此前两次通过不是提交授权的永久缓存。

另外两道边界不能由签名代替：新旧`sharedUserId`必须相同；已有full app不能对受影响用户降成instant app。通过这些门只能证明候选可以继承旧身份，不能证明旧数据内容适配新schema。

### 练习 2：核对旧数据继承的身份门槛

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'oldPackage = mPackages.get(pkgName11);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ps = mSettings.mPackages.get(pkgName11);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (ksms.shouldCheckUpgradeKeySetLocked(ps, scanFlags)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'SigningDetails.CertCapabilities.INSTALLED_DATA)' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'SigningDetails.CertCapabilities.ROLLBACK)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!Objects.equals(oldPackage.getSharedUserId(),' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'installedUsers = ps.queryInstalledUsers(allUsers, true);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'uninstalledUsers = ps.queryInstalledUsers(allUsers, false);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.removedInfo.installReasons = new SparseArray<>(installedUsers.length);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.removedInfo.uninstallReasons = new SparseArray<>(uninstalledUsers.length);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final PackageSetting signatureCheckPs =' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final boolean compatMatch = verifySignatures(signatureCheckPs,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

画出upgrade keyset、正向`INSTALLED_DATA`、反向`ROLLBACK`和全部失败四格。再解释：为什么“APK验签成功”与“获准继承当前设备上的旧数据”不是同一个命题。

## 4. 每用户账先快照，提交后却不是逐位原样恢复

prepare较早把旧Setting查询到的installed用户写入`res.origUsers`；post-install用它和`res.newUsers`划分“原有用户更新”与“本次新增”。replace第二轮则在`mLock`内重新取得全部userId，把旧Setting分成installed与uninstalled两组，并另写`removedInfo.origUsers`；两个`SparseArray`分别快照旧installed用户的installReason与旧uninstalled用户的uninstallReason。包级`firstInstallTime`在commit继承，`lastUpdateTime`刷新。

`DELETE_KEEP_DATA`保留Setting和appId，使普通替换沿用Linux appId、权限状态与大部分`PackageUserState`。但“保留”不是逐位封印：

- 指定单用户安装会对该用户执行`setInstalled(true)`和`setEnabled(DEFAULT)`。
- system更新会把受请求覆盖的原有用户设为DEFAULT，并为所有用户清component label/icon override。
- 旧install/uninstall reason先回填；随后所有当前installed用户的uninstall reason改为UNKNOWN。
- `USER_ALL`分支只把旧installed用户加入`previousUserIds`，却给其他所有用户写本次installReason；源码旁边也质疑这些用户是否真的新装。因此旧uninstalled用户可能仍未安装，但installReason已经变化。

应用业务数据是否可读还取决于新版本自身的数据库迁移、加密协议和启动逻辑。`DELETE_KEEP_DATA`的正常撤旧分支不主动完整删除业务数据目录，不等于替应用完成schema升级；post-commit的system恢复例外见第13节。

## 5. 候选先rename，freezer随后才保护活动包手术窗口

prepare的真实顺序是ABI/native处理、`doRename()`、fs-verity、调用`startIntentFilterVerifications()`把App Links验证任务排入同一PMS Handler，然后才`freezePackageForInstall()`。当前安装Runnable没有让出Handler前，验证任务还不会实际执行。候选在最终随机code path出现并不说明旧包已被冻结，更不说明查询已切到新版本；此时它仍不是活动包。

freezer也不只服务replace。prepare对新装和更新都创建它：若Settings还没有新包，真实freezer仍会把包名加入`mFrozenPackages`，只是找不到appId所以不请求kill。这能让稍后的普通commit遵守同一“重大包手术不得启动”协议。

`mFrozenPackages`影响`checkPackageStartable()`，还让其他move路径拒绝同包并发。commit侧`checkPackageFrozen()`却只是`Slog.wtf`诊断；它不是安装授权，也不会在集合缺项时抛出并中止commit。boot、`SCAN_DONT_KILL_APP`和`SCAN_IGNORE_FROZEN`会跳过这项协议检查。

## 6. PackageFreezer是一枚可关闭的Set所有权令牌，不是引用计数

真实`PackageFreezer`在`mLock`下执行`mFrozenPackages.add(name)`，把返回值保存为`mWeFroze`，再根据Setting决定是否请求kill。`close()`用`AtomicBoolean`保证对象自身只关一次；只有`mWeFroze=true`的对象才从Set移除。`CloseGuard`与finalizer能报警并尝试补关，但时间不可预测。

这套设计是“第一次加入者拥有remove权”，不是嵌套计数。若两个真实freezer异常重叠，第二个`add()`返回false；第一个先close就会移除唯一Set项，即使第二个仍存活。正常安装力求只让一个freezer贯穿主链，但数据结构本身没有证明这个不变量。

`INSTALL_DONT_KILL_APP`返回空壳freezer：包名为null、`mWeFroze=false`，既不冻结目标也不请求kill，但仍打开CloseGuard，调用者必须正常close。这个flag放弃的是目标包的两层保护，不是“只不发signal、仍禁止启动”。

### 练习 3：拆解freezer的Set所有权

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private class PackageFreezer implements AutoCloseable {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mWeFroze = mFrozenPackages.add(mPackageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'killApplication(ps.name, ps.appId, userId, killReason);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private final AtomicBoolean mClosed = new AtomicBoolean();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mClosed.compareAndSet(false, true)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mWeFroze) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mFrozenPackages.remove(mPackageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mFrozenPackages.contains(packageName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'is currently frozen!' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private void checkPackageFrozen(String packageName) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Message msg = mHandler.obtainMessage(START_INTENT_FILTER_VERIFICATIONS);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendMessage(msg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

手算两个freezer依次构造、第一枚先close、第二枚再close的Set状态。再比较空壳对象，说明对象可重复close为什么不等于包名具有引用计数。

## 7. killApplication返回只表示AMS消息入队，persistent进程还是反例

PMS清除外层Binder身份后调用`IActivityManager.killApplication()`，最后恢复身份；同在system_server并不保证调用必然跨进程，但接口边界仍把进程管理交给AMS。AMS service可以为null，PMS也吞掉`RemoteException`，没有把kill请求失败映射成安装失败。

AMS只允许system appId调用，然后把`KILL_APPLICATION_MSG`排入自己的Handler。消息稍后才在AMS锁下进入`forceStopPackageLocked()`；该调用传`evenPersistent=false`，`ProcessList`会跳过persistent进程，而且即便选中普通进程也没有向PMS返回“内核已确认死亡”的ACK。因此真实freezer构造返回只证明冻结项已建立且PMS尝试调用AMS；只有AMS正常受理时才能推出kill消息已入队，仍不能推出旧PID已终止。

这与persistent更新形成真实反例：prepare允许带`INSTALL_STAGED` flag的persistent app更新，AMS这条kill路径却不杀persistent进程。冻结集合能阻止新的startability检查通过，却不会抹掉已经存活的persistent进程。

`INSTALL_DONT_KILL_APP`也只约束目标包。commit更新共享库时会另行遍历依赖者并无条件`killApplication(..., "update lib")`；不能从目标no-kill推导所有相关进程都不受影响。

### 练习 4：从PMS请求追到AMS异步force-stop

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if ((installFlags & PackageManager.INSTALL_DONT_KILL_APP) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return new PackageFreezer();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final long token = Binder.clearCallingIdentity();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'am.killApplication(pkgName, appId, userId, reason);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Binder.restoreCallingIdentity(token);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public void killApplication(String pkg, int appId, int userId, String reason) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (UserHandle.getAppId(callerUid) == SYSTEM_UID) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'Message msg = mHandler.obtainMessage(KILL_APPLICATION_MSG);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mHandler.sendMessage(msg);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'case KILL_APPLICATION_MSG: {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'forceStopPackageLocked(pkg, appId, false, false, true, false,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (app.isPersistent() && !evenPersistent) {' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
```

分别标出PMS返回、AMS Handler取消息、ProcessList选中进程和实际进程死亡四个坐标。说明哪一处有可见ACK，哪三处不能互相替代。

## 8. freezer跨四阶段和rollback等待，commit后异常却可能漏掉显式close

创建freezer后，prepare用`shouldCloseFreezerBeforeReturn`管理所有权。异常返回前由finally关闭；成功则把对象写进`res.freezer`并把关闭责任转交主安装链。scan或reconcile受控失败时，`installPackagesLI()`的failure finally统一关闭整组freezer，并把仍标成功的请求改成`INSTALL_UNKNOWN`。

commit成功不会立即解冻。更新不走普通Backup Manager restore，但带`INSTALL_ENABLE_ROLLBACK`或`INSTALL_REQUEST_DOWNGRADE`时，Rollback Manager可异步接管所有已安装用户的数据snapshot/restore；`mRunningInstalls`继续强引用`PostInstallData`与freezer，直到`finishPackageInstall()`重新投递POST_INSTALL。token没被取走时对象根本不满足不可达条件，不能靠finalizer解冻，只能等回调、system_server重启等外部收口。

锁也要分层：`processInstallRequestsAsync()`在`mInstallLock`内调用整个`installPackagesLI()`，所以prepare、scan、reconcile、commit和`executePostCommitSteps()`都仍持安装锁；reconcile/commit另持`mLock`，所谓post-commit只是离开`mLock`。后来的POST_INSTALL不持续持`mInstallLock`，只有同步或延迟旧代码cleanup再单独取得它。

正常POST_INSTALL取出账本后先`freezer.close()`，再处理权限、广播、旧代码和observer。这让新版本组件可以在广播阶段启动。

还有一个第260章已经遇到的破口：`installPackagesLI.success`在commit返回时就置true，`executePostCommitSteps()`随后若抛未捕获运行时异常，finally会走success分支而不关闭freezer；外层Runnable也到不了`doPostInstall()`、`restoreAndPostInstall()`和POST_INSTALL。此时新包已提交，freezer只能等非正常兜底。

### 练习 5：比较prepare失败、整组失败和正常POST_INSTALL

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'freezePackageForInstall(pkgName, installFlags, "installPackageLI");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'boolean shouldCloseFreezerBeforeReturn = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'shouldCloseFreezerBeforeReturn = false;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.freezer = freezer;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (shouldCloseFreezerBeforeReturn) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'cleanUpAppIdCreation(result);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (request.installResult.freezer != null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'request.installResult.freezer.close();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'request.installResult.returnCode = PackageManager.INSTALL_UNKNOWN;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'request.args.doPostInstall(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (status != PackageManager.INSTALL_SUCCEEDED) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'data.res.freezer.close();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'installPackagesTracedLI(installRequests);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'success = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'executePostCommitSteps(commitRequest);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

画三条所有权线并给每次close标明持有者。再加入“commit后post-commit运行时异常”，解释为什么它不走整组failure finally的关闭分支。

## 9. reconcile只预裁决普通旧包，DeletePackageAction还没有执行删除

对non-system replace，reconcile从scan flags推导`killApp`，构造`DELETE_KEEP_DATA | DELETE_DONT_KILL_APP?`，再调用`mayDeletePackageLocked()`。得到的`DeletePackageAction`封装旧Setting、disabled system基线、`PackageRemovedInfo`、flags与用户范围，只是一份可以在commit执行的计划。

`mayDeletePackageLocked()`若判定旧包不可合法撤下，reconcile以`INSTALL_FAILED_REPLACE_COULDNT_DELETE`让整组在commit前失败。system replace不走这份通用计划，因为它必须保留只读工厂版与disabled-system账；静态共享库虽是特殊包形态，精确同版本覆盖仍可能在non-system条件下取得普通删除计划。

`killApp`来自`SCAN_DONT_KILL_APP`，后者由install flag传播。reconcile仍把no-kill意图编码进`DeletePackageAction`，但replace commit直接调用的`executeDeletePackageLIF()`及其下游不再读取`DELETE_DONT_KILL_APP`；目标包到底是否请求kill，实际由更早的`freezePackageForInstall()`决定。真正是否已有旧进程退出，仍要回到上一节的AMS异步账。

### 练习 6：区分删除裁决与commit执行

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (isInstall && prepareResult.replace && !prepareResult.system) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final boolean killApp = (scanResult.request.scanFlags & SCAN_DONT_KILL_APP) == 0;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int deleteFlags = PackageManager.DELETE_KEEP_DATA' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '| (killApp ? 0 : PackageManager.DELETE_DONT_KILL_APP);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'deletePackageAction = mayDeletePackageLocked(res.removedInfo,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'PackageManager.INSTALL_FAILED_REPLACE_COULDNT_DELETE,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'executeDeletePackageLIF(reconciledPkg.deletePackageAction, packageName,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'commitReconciledScanResultLocked(reconciledPkg, request.mAllUsers);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

在reconcile与commit之间画一条硬线。指出`DeletePackageAction`中哪些只是输入事实，以及null如何阻止新`AndroidPackage`成为活动版本。

## 10. DELETE_KEEP_DATA撤下活动注册，但不走完整卸载的身份销毁分支

commit执行普通delete计划时，`removePackageDataLIF()`无条件先`removePackageLI()`：旧`AndroidPackage`从`mPackages`、组件解析器、共享库和permission定义等活动结构撤下。`DELETE_KEEP_DATA`并不保留旧活动注册。

它控制的是随后两个大分支：

| 资源或账本 | replace撤旧阶段 | 完整卸载阶段 |
|---|---|---|
| DE/CE/external AppData | 不调用`destroyAppDataLIF()` | 主动销毁 |
| 旧ART profiles | 此阶段不调用`destroyAppProfilesLIF()` | 主动销毁 |
| PackageSetting与appId | 保留 | `removePackageLPw()`并释放 |
| keyset、AppsFilter与null-package权限重算 | 不走完整卸载分支 | 移除或重算 |
| 旧活动包、组件、共享库与permission定义 | 从活动结构移除 | 从活动结构移除 |
| 旧code/resource | 仍创建延后清理args | 同样可清理 |

因此UID连续来自Setting/appId没有被完整卸载，不是commit重新分配出“碰巧相同”的值。权限与组件随后还会按新Manifest重新协调，保留身份不等于旧声明永久有效。

KEEP_DATA也不是“包外状态原封不动”：若旧包在某用户持有`SUSPEND_APPS`，delete尾段仍会解除它施加的package suspension，并清掉该用户所有包的distraction restrictions。这一步位于普通与system删除分支汇合之后，不受KEEP_DATA分支保护。

`deleteInstalledPackageLIF()`即使带KEEP_DATA，仍用旧code/resource path与instruction sets构造`removedInfo.args`。r48的`createInstallArgsForExisting()`固定返回`FileInstallArgs`，并没有按历史容器类型动态选择多种清理实现。

## 11. commit完成新旧模型切换，mOldCodePaths却在常见路径立即被清空

普通replace先执行旧包删除计划，再由`commitReconciledScanResultLocked()`把扫描结果写回活动Setting与`mPackages`，随后`updateSettingsLI()`恢复用户账、更新权限、写Settings，并把结果中的pkg、uid和成功码填好。`firstInstallTime`来自旧Setting，`lastUpdateTime`取当前时间。

r48在撤旧后还试图把旧base/split写入`ps1.mOldCodePaths`，注释说它服务“不重启升级”时给活动classloader补新APK。但这段账有两层实现断裂：

1. 条件检查的是值为`0x1`的`PackageManager.DONT_KILL_APP`，不是值为`0x1000`的`INSTALL_DONT_KILL_APP`。
2. scan为现有Setting构造`new PackageSetting(pkgSetting)`时，copy明确跳过`mOldCodePaths`；commit紧接着`pkgSetting.updateFrom(result.pkgSetting)`，若live字段非null而副本字段为null，就把live字段清成null。

所以在`existingSettingCopied`的普通replace路径里，这份刚写入的旧路径集合并不是稳定队列，不能拿它证明旧代码何时删除。真正清理所有权仍在`removedInfo.args`与post-install。

### 练习 7：证明数据、appId连续并复现mOldCodePaths断链

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'removePackageLI(deletedPs.name, (flags & PackageManager.DELETE_CHATTY) != 0);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & PackageManager.DELETE_KEEP_DATA) == 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'destroyAppDataLIF(resolvedPkg, UserHandle.USER_ALL,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'destroyAppProfilesLIF(resolvedPkg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mSettings.mKeySetManagerService.removeAppKeySetDataLPw(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'removedAppId = mSettings.removePackageLPw(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.updatePermissions(deletedPs.name, null);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (removedAppId != -1) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'reconciledPkg.pkgSetting.firstInstallTime = deletedPkgSetting.firstInstallTime;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int[] installedForUsers = res.origUsers;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ps.setInstallReason(previousInstallReason, previousUserId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ps.setUninstallReason(previousReason, previousUserId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((reconciledPkg.installArgs.installFlags & PackageManager.DONT_KILL_APP)' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public static final int INSTALL_DONT_KILL_APP = 0x00001000;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int DONT_KILL_APP = 0x00000001;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'ps1.mOldCodePaths = new ArraySet<>();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'pkgSetting.updateFrom(result.pkgSetting);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Intentionally skip mOldCodePaths; it' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'mOldCodePaths = null;' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'if (reconciledPkg.prepareResult.clearCodeCache) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((flags & Installer.FLAG_CLEAR_APP_DATA_KEEP_ART_PROFILES) == 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'clearAppProfilesLIF(pkg, UserHandle.USER_ALL);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'destroyAppDataLeafLIF(pkg, userId, flags);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Not entirely true at the moment. There is still one side effect' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mUserState.put(orig.mUserState.keyAt(i), orig.mUserState.valueAt(i));' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'setInstantAppForUser(injector, pkgSetting, userId, instantApp, fullApp);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ksms.removeAppKeySetDataLPw(parsedPackage.getPackageName());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sourcePackageSetting.signatures.mSigningDetails =' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (hadSuspendAppsPermission.get(affectedUserId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'unsuspendForSuspendingPackage(packageName, affectedUserId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'removeAllDistractingPackageRestrictions(affectedUserId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'resolveUserIds(reconciledPkg.installArgs.user.getIdentifier()),' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'The method may throw an excpetion in the middle' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'of committing the package, leaving the system in an inconsistent state.' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

先圈出KEEP_DATA跳过的完整卸载区，再按“live Setting → scan副本 → 临时写旧路径 → updateFrom”画对象图。解释为什么字段注释描述设计意图，却不能覆盖r48的实际赋值顺序。

## 12. 系统包与静态共享库各自绕开普通替换的一部分

system replace不在reconcile构造通用`DeletePackageAction`。commit先`removePackageLI(oldPackage)`，再调用`disableSystemPackageLPw()`：

- 第一次覆盖只读工厂版时，disable成功并把工厂基线放进disabled-system账，`removedInfo.args=null`；工厂APK本来也不能由数据分区清理器删除。
- 再次覆盖时，当前活动包已是数据分区更新版，disable返回false；PMS才用旧更新版路径创建`removedInfo.args`，稍后清它。

这不是“卸载系统更新并恢复工厂版”的`deleteSystemPackageLIF()`方向。安装更新会提交新数据分区版本，同时继承system、privileged、vendor/product等来源属性；卸载更新才重新启用与扫描工厂包。

静态共享库则先按version合成包名，只有同一版本的精确覆盖才可能replace。`PackageRemovedInfo`的`isStaticSharedLib`会让REMOVED直接返回；新包侧也跳过ADDED、REPLACED和MY_PACKAGE_REPLACED，只可能向library consumers发送PACKAGE_CHANGED。标准四广播序列不能套给它。

## 13. KEEP_DATA不等于AppData和profile在post-commit中完全不动

commit之后，`prepareAppDataAfterInstallLIF()`只为installed且正在运行、非dying的用户调用installd `createAppData()`：已解锁用户准备DE与CE，运行但未解锁用户只准备DE。第三方包失败只记日志，避免擅自wipe业务数据；system包失败却会先`destroyAppDataLeafLIF()`再重建。因此KEEP_DATA只保证“撤旧阶段不主动完整删除”，不能保证系统包恢复分支永远不触碰目录。

每个replace的`PrepareResult.clearCodeCache=true`。post-commit调用`clearAppDataLIF(... FLAG_CLEAR_CODE_CACHE_ONLY)`请求清理DE、CE与external code cache；由于flags没有`FLAG_CLEAR_APP_DATA_KEEP_ART_PROFILES`，wrapper随后请求对所有用户执行`clearAppProfilesLIF()`。这纠正了“KEEP_DATA整体保留旧profile”的常见误读。

本链先请求清除所有用户的code cache与该包profiles；随后replace通知DexManager code paths已更新，并仅为`installArgs.user`解析出的本次用户请求`prepareAppProfiles(..., true)`，再进入条件dexopt。profile清理与准备内部遇到`InstallerException`都只记日志，不改变安装成功码。因此单用户更新会请求清其他用户旧profile，却不在这条调用中替他们重建；源码也不保证请求后的最终profile状态。

## 14. POST_INSTALL先解冻；四类更新广播只是按顺序再次入队

普通更新不走首次安装的Backup restore；rollback/downgrade数据工作若未接管，`restoreAndPostInstall()`把POST_INSTALL消息排入PMS Handler。Handler先从`mRunningInstalls`移除token并close freezer，再调用`handlePackagePostInstall()`。

若成功包在这段间隙已被另一路移除，`pkgSetting==null`会把结果改为`INSTALL_FAILED_PACKAGE_CHANGED`并直接通知install observer；本链不发送更新广播，也不会消费`removedInfo.args`。这是一条旧代码清理可失去正常入口的竞态。

正常普通replace的逻辑调用顺序是：

1. REMOVED，`DATA_REMOVED=false`、`REPLACING=true`，`DONT_KILL_APP`按请求填写；
2. 处理restricted whitelist与请求的runtime grants；
3. 对原来没有而现在有的用户发首次ADDED；对update users发`ADDED(REPLACING)`；
4. 向update users及特定installer/verifier发REPLACED，再向新包自身定向MY_PACKAGE_REPLACED。

但`sendPackageBroadcast()`本身只是`mHandler.post()`。由于当前正处理POST_INSTALL，这些调用只是把实际AMS发送任务排到当前消息之后；顺序是PMS入队顺序，不是前一广播全部receiver执行完成的屏障。static shared library还会跳过上述标准序列。

### 练习 8：还原解冻、广播入队与static例外

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'AndroidPackage pkg = commitReconciledScanResultLocked(reconciledPkg, request.mAllUsers);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'updateSettingsLI(pkg, reconciledPkg.installArgs, request.mAllUsers, res);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'data.res.freezer.close();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.removedInfo.sendPackageRemovedBroadcasts(killApp);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'extras.putBoolean(Intent.EXTRA_DATA_REMOVED, dataRemoved);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'extras.putBoolean(Intent.EXTRA_DONT_KILL_APP, !killApp);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'extras.putBoolean(Intent.EXTRA_REPLACING, true);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendPackageBroadcast(Intent.ACTION_PACKAGE_ADDED, packageName,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendPackageBroadcast(Intent.ACTION_PACKAGE_REPLACED,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendPackageBroadcast(Intent.ACTION_MY_PACKAGE_REPLACED,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (dataRemoved && !isRemovedPackageSystemUpdate) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendPackageBroadcast(Intent.ACTION_PACKAGE_FULLY_REMOVED,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public void sendPackageBroadcast(final String action, final String pkg, final Bundle extras,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.post(() -> {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (isStaticSharedLib) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (res.pkg.getStaticSharedLibName() == null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mInjector.getActivityManagerInternal().broadcastIntent(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'else if (!ArrayUtils.isEmpty(res.libraryConsumers)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendPackageChangedBroadcast(pkg.getPackageName(), false /* dontKillApp */,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'notifyPackageChangeObserversOnUpdate(reconciledPkg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

把“调用send”“PMS Handler实际调用AMS”“AMS完成发送决策”“receiver执行”画成四个坐标。再分别推导普通更新、static shared library覆盖与完整卸载的广播集合。

## 15. 标准更新广播在kill清理之后才真正发送，no-kill另有两条延迟账

以下先限定为没有未捕获异常的正常控制流，并只比较`handlePackagePostInstall()`里REMOVED、ADDED、REPLACED、MY_PACKAGE_REPLACED这组标准生命周期广播。旧external包的UNAVAILABLE在commit阶段已经另行入队，新external包的AVAILABLE也单独入队；它们不能拿来证明下表里的标准广播顺序。

| 路径 | 当前POST_INSTALL消息内 | 当前消息返回后 |
|---|---|---|
| kill且`removedInfo.args != null` | 依次排标准广播任务；同步持`mInstallLock`调用`doPostDeleteLI(true)`；直接回install observer | PMS才处理标准广播任务并调用AMS |
| no-kill且`removedInfo.args != null` | 依次排标准广播任务；排3秒旧代码清理；按包名登记install observer并排500ms后备 | 标准广播先有机会真正发送；有效内部确认或500ms后备可回observer；3秒后尝试清旧代码 |
| `removedInfo.args == null` | 标准广播仍按包形态入队，但只请求并发GC；kill直接回observer，no-kill仍登记500ms后备 | 不存在3秒物理清理任务；GC不提供删除完成证明 |

因此就这四类标准广播而言，当`removedInfo.args != null`时，kill路径会先尝试清旧APK，再让PMS Handler真正调用AMS。但这次清理只排在PMS较早的kill调用尝试之后，并不等待AMS处理`KILL_APPLICATION_MSG`或内核确认PID死亡；persistent进程甚至不会被选中。

no-kill也没有等待所有manifest receiver。ProcessList会向全部有thread的运行进程异步`dispatchPackageBroadcast(PACKAGE_REPLACED)`，但`foundProcess`只检查是否存在“以目标包为主包”的进程；没找到时直接通知PMS。有进程时，首个通过可见性过滤的ActivityThread回调就能移除唯一observer映射。这是首个有效回调或无主进程时的捷径，不是所有进程Resources已刷新ACK。static shared library没有REPLACED内部确认，所以其no-kill更新只能走500ms后备。

即使普通包也只有在“同包名没有重叠更新”时，才能把内部确认或500ms理解为本次安装的两条回程。r48用`packageName`而非安装token向`mNoKillInstallObservers`写一个Pair；下一次同包no-kill更新会覆盖前一个Pair，旧REPLACED回调或旧500ms消息又可能移除并通知新的Pair。因此它不保证逐安装关联，旧install observer也可能永远收不到结果。

旧资源清理本身是best-effort。`createInstallArgsForExisting()`固定给`FileInstallArgs`；它先尽力parse旧PackageLite，再`cleanUp()`和`rmdex`。parse失败仍能删存在的主code目录；codeFile已不存在则提前返回，外置resourceFile也不单独删。目录删除的`InstallerException`只记日志，普通文件`delete()`返回值被忽略，`rmdex`异常被吞，`doPostDeleteLI()`仍恒定返回true。安装成功与清理函数返回都不能证明磁盘已干净。反过来，Incremental旧路径的`closeStorage()`若抛未捕获运行时异常，在kill路径会于标准广播任务入队后截断当前POST_INSTALL并阻断install observer；no-kill路径则到独立的3秒清理消息才触发，500ms observer后备通常已经先到。“best-effort”不等于所有异常都被吞，也不能把两条路径的异常窗口混为一谈。

`removedInfo.args`为null常见于首次覆盖只读工厂APK；此时并发GC既不等待也不是文件删除协议。3秒也只是Handler的最早调度点，线程拥塞或`mInstallLock`竞争还会更晚；它没有逐进程mmap、Resources或ClassLoader引用确认。

### 练习 9：验证物理清理、REPLACED确认与observer边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'outInfo.args = createInstallArgsForExisting(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'res.removedInfo.args = createInstallArgsForExisting(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private InstallArgs createInstallArgsForExisting(String codePath,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return new FileInstallArgs(codePath, resourcePath, instructionSets);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'InstallArgs args = res.removedInfo != null ? res.removedInfo.args : null;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!killApp) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'scheduleDeferredNoKillPostDelete(args);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'args.doPostDeleteLI(true);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'static final int DEFERRED_NO_KILL_POST_DELETE_DELAY_MS = 3 * 1000;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'static final int DEFERRED_NO_KILL_INSTALL_OBSERVER_DELAY_MS = 500;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'getPackageManager().notifyPackagesReplacedReceived(' frameworks/base/core/java/android/app/ActivityThread.java
grep -n -F 'AppGlobals.getPackageManager().notifyPackagesReplacedReceived(packages);' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'Pair<PackageInstalledInfo, IPackageInstallObserver2> pair =' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mNoKillInstallObservers.put(packageName, Pair.create(info, observer));' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Message message = mHandler.obtainMessage(DEFERRED_NO_KILL_INSTALL_OBSERVER, packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'boolean foundProcess = false;' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'r.thread.dispatchPackageBroadcast(cmd, packages);' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'VMRuntime.getRuntime().requestConcurrentGC();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendResourcesChangedBroadcast(false, true, pkgList, uidArray, null);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendResourcesChangedBroadcast(true, true, pkgList, uidArray, null);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIncrementalManager.closeStorage(codePath);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'List<String> allCodePaths = Collections.EMPTY_LIST;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (codeFile == null || !codeFile.exists()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final PackageLite pkg = PackageParser.parsePackageLite(codeFile, 0);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'cleanUp();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'removeCodePathLI(codeFile);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'removeDexFiles(allCodePaths, instructionSets);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

分别推演kill、no-kill有运行进程、no-kill无运行进程和旧codeFile已丢失四例。对每例写出广播任务、observer、3秒消息与物理删除的最早可证完成点。

## 16. 用失败矩阵排查“新包已生效，旧世界却没收口”

“commit是唯一系统状态修改点”只能理解成新活动包的中心发布边界，不能扩大成commit前绝无副作用。r48至少有三类反例：prepare的兼容签名路径可提前移除keyset数据或更新permission owner的SigningDetails；scan虽声称无副作用，源码紧接着自我否定，而且`PackageSetting` copy把`PackageUserState`对象引用直接放入副本，instant/full切换可能改到live用户态；乐观appId也在reconcile前注册，再靠failure finally补偿。

commit自身也不是原子区。`commitReconciledScanResultLocked()`的源码注释明确警告，它可能在提交中途抛异常并留下不一致状态；外层failure finally只关闭freezer并回收乐观创建的appId，不会把已经完成的旧包撤下、Setting改写或注册表修改逐项逆转。

因此受控prepare/scan/reconcile失败能保证“新`AndroidPackage`没有作为活动版本提交”，却不能证明每份Settings辅助状态按位回滚；commit中途异常连这一保证也不具备。rename、fs-verity、App Links验证任务入队和这些辅助修改都要分别审计。

| 现场 | 最可能的阶段 | 重点证据 |
|---|---|---|
| 新随机目录存在，旧包仍可查询 | rename后、commit前 | freezer、prepare/scan错误、failure cleanup |
| 新包可查询但一直提示frozen | commit后post-commit异常或rollback回调未归还 | Handler异常、`mRunningInstalls`、CloseGuard |
| 成功但system AppData丢失 | `createAppData`失败恢复 | critical日志与destroy/retry |
| REMOVED已调用但receiver未运行 | 广播只在PMS Handler排队 | PMS与AMS两层队列 |
| kill路径observer成功但广播尚未实际发送 | 当前POST_INSTALL仍未返回 | Handler消息顺序 |
| no-kill observer成功但旧APK还在 | REPLACED内部确认或500ms后备早于3秒清理 | 两个延迟消息与cleanup日志 |
| 前一次no-kill observer沉默，后一次被旧消息提前回调 | 同包名更新重叠 | `mNoKillInstallObservers`单Pair与旧ACK/timeout |
| 新旧注册表只改了一部分且整组报失败 | commit中途异常 | commit警告、failure finally没有undo |
| 新包已提交、无更新广播/install observer且仍冻结 | post-commit运行时异常 | commit结果、Incremental/AppData/dex日志 |
| `INSTALL_FAILED_PACKAGE_CHANGED`且旧代码残留 | POST_INSTALL前包被另一路移除 | `pkgSetting==null`早退与`removedInfo.args` |

最后自检九问：replace为什么需要flag；签名能力为什么等同数据访问授权；freezer返回为何不等进程死亡；persistent为何是反例；KEEP_DATA跳过哪些动作又不跳过什么；`mOldCodePaths`为何会被立即清空；系统首次与再次覆盖的旧代码去向有何不同；kill与no-kill的广播、install observer、清理顺序为何相反；static shared library为什么没有标准四广播。

第262章将继续沿`updateSettingsLI()`之后的权限链，区分permission定义、Manifest请求、Package/SharedUser权限状态和每用户runtime账，解释替换时哪些授权继承、撤销或触发GID变化。
