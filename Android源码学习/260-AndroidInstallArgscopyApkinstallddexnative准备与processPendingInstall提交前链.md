# 260 Android InstallArgs、copyApk、installd、dex/native准备与processPendingInstall提交前链

## 1. 先把“安装文件准备完成”拆成七个坐标

第259章停在 ordinary verifier、App Integrity与 rollback三道异步门。本章继续追 Android 11 r48 的下一段：`InstallParams.handleReturnCode()`何时调用 `InstallArgs.copyApk()`，稳定候选目录怎样变成最终 code path，什么时候才真正修改包管理状态，以及 AppData、profile、dexopt和 Incremental native为何都在更晚的位置。

这条链最危险的口语是“APK已经复制好，所以安装完成”。源码里至少有七个互不等价的完成点：

| 坐标 | 已经成立 | 尚不能推出 |
|---|---|---|
| Session stage稳定 | PackageInstaller控制候选目录 | PMS已接受包 |
| `copyApk()`成功 | `InstallArgs`有可继续处理的路径 | 路径已经是最终名 |
| `doRename()`成功 | code path切到最终布局 | scan、reconcile或commit成功 |
| prepare/scan成功 | 单包模型与局部检查通过 | 多包组合世界可提交 |
| `commitPackagesLocked()`返回 | PMS内存与Settings提交阶段完成 | AppData、dexopt、native等待完成 |
| post-commit步骤返回 | 安装后资源准备链已跑完 | backup/observer回程已经完成 |
| install observer收到成功 | 本次公开安装结果已交付 | 后台优化永不再发生 |

主线可以压成一行：三门汇合 → 条件性 `copyApk()` → `processPendingInstall()` → Package Handler排队 → `mInstallLock`内 prepare/scan/reconcile/commit/post-commit → `doPostInstall()` → restore或 `POST_INSTALL` → install observer。这里所谓 post-commit“锁外”只指已经退出内层 `mLock`；整个核心安装与 post-commit仍在 `mInstallLock`保护下。

源码入口集中在 `PackageManagerService.java`，但文件动作会继续跨到 `PackageManagerServiceUtils.java`、`PackageInstallerSession.java`、`PackageAbiHelperImpl.java`、`NativeLibraryHelper.java`、`IncrementalManager.java`、`Installer.java`与 installd。排障时应记录“对象、路径、返回码、锁和阶段”五个维度，不能只记一条文件路径。

## 2. OriginInfo的两个布尔量正交，InstallArgs也不是不可变快照

`OriginInfo`保存 `file`、`staged`、`existing`、`resolvedPath`和 `resolvedFile`。其中 `staged`只表示下游不必再做防御性复制；`existing`只表示来源是已安装应用。二者回答不同问题，不能用一个推导另一个。构造器所谓 resolved path在本版本只是 `getAbsolutePath()`，并未做 canonical解析。

磁盘调用图比四个工厂方法的名字更重要：

| 工厂 | 本树中的实际生产调用 | `InstallArgs`结果 |
|---|---|---|
| `fromStagedFile()` | `ActiveInstallSession.getStagedDir()` | `move == null`，选择 `FileInstallArgs` |
| `fromExistingFile()` | `movePackageInternal()` | 私有卷完整移动有 `MoveInfo`，选择 `MoveInstallArgs`；主物理卷特殊分支没有 `MoveInfo`，反而选择 `FileInstallArgs` |
| `fromUntrustedFile()` | 只有定义，没有调用点 | 不应写成当前普通 APK安装的生产入口 |
| `fromNothing()` | 为既有 code/resource path构造清理参数 | 用于既有安装的删除/资源清理语境 |

`createInstallArgs()`只看 `params.move != null`：有 `MoveInfo`才构造 `MoveInstallArgs`，否则一律是 `FileInstallArgs`。所以 `origin.existing=true`不等于“必然调用 installd整体搬迁”；主物理卷分支恰好是 existing来源、无 `MoveInfo`、走非 staged文件复制。

`InstallArgs`更适合称为安装执行信封。它固化 origin、flags、InstallSource、volume、user、ABI override、签名、DataLoader type和observer等请求事实；但 `FileInstallArgs.codeFile/resourceFile`会在 copy与rename时改变，`instructionSets`也不是类级不可变事实。把它说成完全不可变快照，会掩盖失败清理究竟指向旧路径还是新路径这一关键问题。

### 练习 1：从工厂调用点还原真实来源矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static OriginInfo fromUntrustedFile(File file) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'static OriginInfo fromExistingFile(File file) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'static OriginInfo fromStagedFile(File file) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'origin = OriginInfo.fromStagedFile(activeInstallSession.getStagedDir());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final OriginInfo origin = OriginInfo.fromExistingFile(codeFile);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (params.move != null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

分别列出普通 Session、私有卷完整移动、主物理卷移动和既有包删除四例的 `staged/existing/move`。然后用全树搜索确认 `fromUntrustedFile()`没有生产调用，不能从“方法存在”跳到“普通安装正在用”。

## 3. 三道门只决定何时读粘滞返回码，dry run甚至不会进入copy

`HandlerParams.startCopy()`这个历史名称会先执行 `handleStartCopy()`，再执行 `handleReturnCode()`。`InstallParams.handleStartCopy()`完成位置策略、创建 `InstallArgs`，把 verification、Integrity和rollback三个完成位置为初始 true，再按条件发起异步工作。任何门开始等待时才把自己的完成位改为 false。

只有三个完成位同时为 true，`handleReturnCode()`才继续。接下来还有两层分路：

1. `INSTALL_DRY_RUN`直接轻量解析包名并回 observer，不调用 `copyApk()`，也不调用 `processPendingInstall()`；
2. 非 dry run仅当粘滞 `mRet == INSTALL_SUCCEEDED`时调用 `mArgs.copyApk()`，随后无论成功或失败都把当前码交给 `processPendingInstall()`。

`setReturnCode()`只在旧值仍成功时写入，因此 verifier拒绝、Integrity拒绝、位置错误或更早失败不会被后来成功覆盖。`copyApk()`自己的失败码也写回同一本账。三个位全真只表示“异步门已经结束”，不表示判定结果是允许。

`origin.existing`在 `handleStartCopy()`外层就跳过 ordinary与Integrity State，而不是只跳过广播；rollback若没有请求也保持初始完成。因此移动请求通常很快到 `copyApk()`，但它仍经过相同的返回码汇合方法。

### 练习 2：标出copyApk的四重条件

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mVerificationCompleted' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& mIntegrityVerificationCompleted && mEnableRollbackCompleted) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((installFlags & PackageManager.INSTALL_DRY_RUN) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mRet == PackageManager.INSTALL_SUCCEEDED) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mRet = mArgs.copyApk();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'processPendingInstall(mArgs, mRet);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

手算“ordinary晚到、Integrity先拒绝”“三门允许但copy失败”“dry run已拒绝”“existing move”四例。每例分别写完成位、进入copy与否、最终交给哪个回程，避免把门状态和安装结果合成一个布尔量。

## 4. origin.staged不是SessionParams.isStaged，Session激活也不是native终点

`InstallParams(ActiveInstallSession)`确实无条件调用 `OriginInfo.fromStagedFile(activeInstallSession.getStagedDir())`，但原始跨重启 Session不会直接进入这个构造器：`PackageInstallerSession.install()`在 `params.isStaged`分支把事务交给 StagingManager后返回；重启应用APK时，`createAndWriteApkSession()`复制参数，显式改成 `params.isStaged=false`，同时加入 `INSTALL_STAGED` flag，再由这个合成 Session进入 `makeSessionActiveLocked()`和 `InstallParams`。因此普通 Session与合成 APK Session都会得到 `origin.staged=true`，它只描述“目录已经受控、可被下游接管”，不能反推原始事务是否跨重启。

因此常规 Session的 `FileInstallArgs.doCopyApk()`只把 `codeFile`与 `resourceFile`指向 `origin.file`并返回成功。它完成的是路径接管，不是第二次字节复制。上游 Session已 seal，提交前验证过 base/split结构和签名，并在 `makeSessionActiveLocked()`中处理继承文件；这才是下游敢复用目录的上下文。

native库在交给 PMS前已经有第一处动作。对单 APK Session或 multi的每个非父 child，`makeSessionActiveLocked()`在创建 `ActiveInstallSession`之前调用 `extractNativeLibraries(stageDir, abiOverride, mayInheritNativeLibs())`。普通文件系统在这里同步抽取；Incremental路径会向 Incremental Service配置 native文件，真实数据抽取可继续异步。multi parent本身不代表一个 APK，所以跳过这一段，children各自处理。

但这还不是本章可以宣称的“native最终完成”。prepare阶段的 ABI推导会再次调用 native helper，Incremental还要在 commit后的组末等待。看到 Session激活成功，只能说第一处 native准备调用已经返回。

### 练习 3：区分路径接管、Session抽取与prepare再抽取

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (origin.staged) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'codeFile = origin.file;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'params.isStaged = false;' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'params.installFlags |= PackageManager.INSTALL_STAGED;' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'extractNativeLibraries(stageDir, params.abiOverride, mayInheritNativeLibs());' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'final boolean extractNativeLibs = !AndroidPackageUtils.isLibrary(parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'derivedAbi = mInjector.getAbiHelper().derivePackageAbi(parsedPackage,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'copyRet = NativeLibraryHelper.copyNativeBinariesForSupportedAbi(handle,' frameworks/base/services/core/java/com/android/server/pm/PackageAbiHelperImpl.java
```

画四列时间线：普通 Session、原始跨重启 Session及其合成 APK Session、Incremental Session、multi parent加两个 children。明确哪些动作是采用路径、同步复制、异步配置、ABI选择和最终等待；不要用一个“extract完成”覆盖全部节点。

## 5. 非staged FileInstallArgs在当前树中服务主物理卷移动，不是普通APK入口

`FileInstallArgs.doCopyApk()`确实保留一条通用的非 staged防御性复制实现：分配新 stage、复制 APK集合、准备 native库。但在这份 r48源码中，`OriginInfo.fromUntrustedFile()`只有定义没有调用。把这条实现描述成“普通 legacy APK安装的主要路径”，会把能力边界误当成当前调用图。

可证的生产入口来自 `movePackageInternal()`。目标是内部私有卷或 adoptable private volume时，`moveCompleteApp=true`并构造 `MoveInfo`；目标是 `StorageManager.UUID_PRIMARY_PHYSICAL`时，`moveCompleteApp=false`、`move=null`。两者都把旧 code path包装成 `OriginInfo.fromExistingFile(codeFile)`，但后者因为没有 `MoveInfo`而落入 `FileInstallArgs`。

这条特殊路径同时证明两个布尔量确实正交：`existing=true`让 package verification外层整体旁路，`staged=false`又让 `doCopyApk()`实际复制。它不是从不可信下载目录装一个新包，而是把已安装包的代码重建到主物理卷的受控 stage。

非 staged分支调用已废弃标记的 `allocateStageDirLegacy(volumeUuid, isEphemeral)`。PackageInstallerService分配随机 legacy sessionId，在相应 volume的 app目录创建 `vmdl<id>.tmp`，mode为0775并做一次 restorecon。分配中任何 `IOException`都被压成 `INSTALL_FAILED_INSUFFICIENT_STORAGE`，即使真实原因不是容量不足。

随后 `copyPackage()`复制 APK，`NativeLibraryHelper.Handle.create(codeFile)`重新打开目标集合，再由 `copyNativeBinariesWithOverride()`准备 `lib/<isa>`。Handle在 finally中关闭；APK复制成功并不保证整个 `copyApk()`成功，native返回码仍能把结果改为失败。

## 6. copyPackage只复制base与split，fresh-stage假设承担了文件事务安全

`PackageManagerServiceUtils.copyPackage()`不是递归目录复制。它先重新解析 `PackageLite`，只取 `baseCodePath`、`splitNames`与 `splitCodePaths`：base落成 `base.apk`，split落成 `split_<name>.apk`。来源目录里的 oat、旧 lib、杂项文件乃至未列入 PackageLite的内容都不会随手带过去；native随后从目标 APK重新构建。

目标名经过 `FileUtils.isValidExtFilename()`。不过这个检查抛 `IllegalArgumentException`，外层只捕获 `PackageParserException | IOException | ErrnoException`；若真触发，它不会被映射成空间错误，而会越出 helper。常规 base名固定且 split名此前已经解析，正常输入依赖上游约束不触发这个分支。

文件级实现还有两个必须按源码记录的假设：目标用 `Os.open(..., O_RDWR | O_CREAT, 0644)`打开，没有 `O_EXCL`或 `O_TRUNC`；finally只关闭 source `FileInputStream`，没有把 raw target `FileDescriptor`交给可关闭包装对象，也没有调用 `Os.close()`。libcore的 `FileDescriptor`注释明确由创建者负责关闭，因此这是该 helper保留分支中的FD泄漏疑点；普通GC不能假定为它代关，只有进程退出时内核才兜底。新建且空的 stage使“目标此前不存在”成为常态，也掩盖了覆盖旧文件时尾部残留。不能把它描述为自足、可重放的原子复制 helper。

映射也很粗：解析、普通IO与 errno异常统一变成 `INSTALL_FAILED_INSUFFICIENT_STORAGE`，应结合 `Failed to copy package at ...`日志保留原异常类别。反过来，前述未捕获的运行时异常不会获得这个状态码。

清理边界同样反直觉。若 `copyApk()`已经分配 legacy stage后返回失败，`processInstallRequestsAsync(success=false)`会跳过全部 `doPreInstall()`与 `doPostInstall()`，所以 `FileInstallArgs`自身的 failure cleanup在这条路径根本没有执行。现代 Session通常会在最终 observer回调中销毁自身 stage；对独立 legacy stage，源码可见的延迟兜底是 `PackageInstallerService.reconcileStagesLocked()`：`systemReady()`只对内部私有卷调用，私有卷挂载时再按该卷调用。这不是本次失败回程的即时删除，也不能据此断言每个主物理卷目标都一定被覆盖。

删除侧还有一处历史不对称：`FileInstallArgs.doPostDeleteLI(boolean delete)`无论参数真假都调用 `cleanUpResourcesLI()`。后者先尽力解析 code paths，再调用 `cleanUp()`，最后按 instruction sets调用 installd `rmdex`。PackageLite解析失败只会让 dex路径枚举为空；只要 `codeFile`还存在，目录清理仍会继续。反过来，`codeFile`缺失会让 `cleanUp()`立即返回，连位于其外部的 `resourceFile`也不会单独尝试删除。参数名不能替代实现证据。

### 练习 4：审计copyFile的输入边界与FD所有权

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final PackageParser.PackageLite pkg = PackageParser.parsePackageLite(packageFile, 0);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F 'copyFile(pkg.baseCodePath, targetDir, "base.apk");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F '"split_" + pkg.splitNames[i] + ".apk");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F 'if (!FileUtils.isValidExtFilename(targetName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F 'O_RDWR | O_CREAT, 0644);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F 'FileUtils.copy(source.getFD(), targetFd);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
grep -n -F 'IoUtils.closeQuietly(source);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java
```

假设目标是全新空目录，再假设同名目标已有更长旧文件，分别推导结果。列出 source FD、target FD各由谁关闭，并说明为什么“当前调用通常安全”与“helper本身具备覆盖事务语义”是两种结论。

## 7. MoveInstallArgs让installd先复制完整应用，PMS提交后才删源端

`movePackageInternal()`按目标位置选择完整搬迁：目标是内部私有存储或已挂载可写的private volume时，`moveCompleteApp=true`并使用 `MoveInstallArgs`；源端也可能是此前位于 `PRIMARY_PHYSICAL`的代码，不能把“目标为private”误读成两端都必须是private。方法先在 `mLock`下检查包、volume、设备管理员与冻结状态，取得已安装用户，冻结包；若文件级加密启用，还要求所有已安装用户的key已解锁。它测量 code+data并确认目标空间后，才构造 `MoveInfo`进入 `INIT_COPY`。

`MoveInstallArgs.copyApk()`这个名字并不准确描述动作范围。它在 `synchronized(mInstaller)`中调用 `Installer.moveCompleteApp()`；installd端复制 code tree并对目标恢复SELinux标签。对 `get_known_users(from_uuid)`返回的每个用户，它先检查CE源目录：CE不存在就跳过整个用户，存在才创建目标并依次复制DE、CE，再做AppData restorecon。源端此时仍保留，C++注释明确把“框架先扫描、持久化目标，再删除源”作为断电恢复顺序。

installd内部任一步失败会尽力删除已经复制到目标的 code、DE和CE目录，再把错误返回 system_server；各个删除失败只记录warning，所以“回滚目标端”是意图而非绝对保证。成功返回后，`MoveInstallArgs`以原 `fromCodePath`的最后一级名字构造目标 `codeFile`；`doRename()`直接返回 true，因为整体复制已经把目标路径建立好，不再生成另一组随机名。

框架侧的补偿矩阵是：核心安装成功，`doPostInstall()`清理 `fromUuid`；失败则清理 `toUuid`。清理遍历所有 userId，调用 `destroyAppData(DE|CE|KEEP_ART_PROFILES)`并删除 code path。`destroyAppData()`异常会被逐用户记录后继续，底层删除也可能失败，所以成功后仍可能残留源端、失败后也可能残留目标端。profiles刻意保留，因为它们没有一起搬移，误删会丢掉唯一副本。这里依靠“先复制、提交、后删源”恢复一致性，不是跨 Binder、文件系统和Settings的一次数据库事务。

主物理卷分支没有 `MoveInfo`，所以不能把这一套 complete-app补偿套给它。它走上一节的 `FileInstallArgs`代码复制，移动进度所计容量也只有 `stats.codeSize`，不是 code+data。

### 练习 5：推导完整移动的源端与目标端补偿

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mInstaller.moveCompleteApp(move.fromUuid, move.toUuid, move.packageName,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'codeFile = new File(Environment.getDataAppDirectory(move.toUuid), toPathName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'cleanUp(move.fromUuid);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'cleanUp(move.toUuid);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int flags = FLAG_STORAGE_DE | FLAG_STORAGE_CE' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mInstaller.destroyAppData(volumeUuid, move.packageName, userId, flags, 0);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'copy_directory_recursive(fromCodePath.c_str(), to_app_package_path_parent.c_str());' frameworks/native/cmds/installd/InstalldNativeService.cpp
```

分别在 code复制后失败、第三个用户CE复制后失败、PMS reconcile失败和最终成功四个时间点画源/目标矩阵。区分 installd函数内部回滚与 `MoveInstallArgs.doPostInstall()`补偿，不能把两层清理当成同一次调用。

## 8. processPendingInstall是状态汇合桥，Async只是向同一Package Handler再排一条消息

单包进入 `processPendingInstall()`时，PMS新建 `PackageInstalledInfo`，只写当前 returnCode，并把 uid初始化为 -1、pkg与removedInfo置空。包名、UID、更新信息和freezer都要在后续 prepare/scan/commit中逐步填入。

multi-package不会让第一个成功 child先安装。每个 child把 `InstallArgs -> status`写入 parent的 `mCurrentState`；Map大小未达到 child数就返回，任何值仍是 `INSTALL_UNKNOWN`也返回。这个返回不会安排timer或主动重试，必须由同一个 `InstallArgs`之后再次回调并覆盖状态才能推进。全部可判定后，遍历中遇到的第一个非成功码成为 `completeStatus`，再用这个同一状态为每个 child重建 `PackageInstalledInfo`。因此只有进入核心前的汇合失败会给全组同一个码；它保证“整组是否进入核心”一致，却不会保留各child的原始错误码。

`processInstallRequestsAsync()`的 Async不表示另开并行worker。它调用 `mHandler.post()`，仍落在 PMS自己的 background-priority `PackageHandler`/`ServiceThread`。这样把耗时核心与当前 verifier或rollback消息栈断开，却仍按同一Looper串行执行。

传入 `success=true`时，Runnable按 `doPreInstall()` → `synchronized(mInstallLock)`中的 `installPackagesTracedLI()` → `doPostInstall()`顺序执行。两个钩子的返回值都被忽略，当前子类主要靠共享 `PackageInstalledInfo.returnCode`和清理副作用协作。传入 false时，pre、核心与post三段全部跳过，直接为每个请求调用 `restoreAndPostInstall()`。

正常返回下，无论安装成功或普通状态码失败都会进入 restore/`POST_INSTALL`统一回程；新装、允许backup且成功时可能先做restore round-trip。这里必须保留“正常返回”限定：Runnable没有包住核心的 catch/finally，未捕获运行时异常可以截断 `doPostInstall()`和install observer链，第15节给出 commit后的具体入口。

### 练习 6：手算multi汇合与Handler上的四段分路

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'args.mMultiPackageInstallParams.tryProcessInstallRequest(args, currentStatus);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mCurrentState.put(args, currentStatus);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mCurrentState.size() != mChildParams.size()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (status == PackageManager.INSTALL_UNKNOWN) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'completeStatus = status;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.post(() -> {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'request.args.doPreInstall(request.installResult.returnCode);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'installPackagesTracedLI(installRequests);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'request.args.doPostInstall(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'restoreAndPostInstall(request.args.user.getIdentifier(), request.installResult,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

给三个 children依次输入成功、UNKNOWN、-2，再让 UNKNOWN更新为-22。写出何时开始核心、所有child最终看到哪个码，以及在 `success=false`时哪些清理钩子实际没有运行。

## 9. 四阶段只把可预测错误赶到commit前，post-commit仍持有mInstallLock

`installPackagesLI()`源码注释把事务分成 prepare、scan、reconcile、commit四阶段：prepare解析并做初始验证；scan把候选放进扫描模型；reconcile把所有候选与当前系统状态一起检查；commit才发布扫描结果并更新系统状态。prepare、scan或reconcile通过 catch+return表达的受控失败会让整组退出，随后外层为全部请求执行 failure `doPostInstall()`；但结果码并不总相同：prepare或scan只给触发失败的请求写具体错误，finally把仍为成功的兄弟改成 `INSTALL_UNKNOWN`，只有reconcile失败显式给所有请求写同一个错误。commit或其他未捕获运行时异常则会继续传播，不能套用这条正常补偿结论。

真实锁层次如下：

| 区域 | `mInstallLock` | `mLock` | 主要工作 |
|---|---:|---:|---|
| copy与`processPendingInstall()`前 | 否 | 局部读取 | stage/move文件准备、异步门 |
| `installPackagesTracedLI()`整体 | 是 | 分段 | prepare、scan、reconcile、commit、post-commit |
| reconcile与commit代码块 | 是 | 是 | 组合校验、包表与Settings提交 |
| `executePostCommitSteps()` | 是 | 否（内部短暂再取） | installd、AppData、profile、dexopt、IncFS native等待 |
| `doPostInstall()`与结果回程 | 否 | 视子路径而定 | 清理、restore、observer |

所以源码所说“commit后释放 package lock”指退出 `mLock`，不等于退出 `mInstallLock`。AppData、dexopt和 Incremental native wait依然串行占住安装锁；这既避免多个安装互相踩文件状态，也意味着慢 post-commit会推迟后续安装。

prepare对每个请求运行后才scan该请求；scan时还会拒绝同一 multi中解析出重复包名，并可能乐观注册 appId。reconcile与commit统一在 `mLock`内执行。若 commit前退出，finally会清理由乐观扫描创建的 appId、关闭已有 freezer，并把仍标成功的请求改为 `INSTALL_UNKNOWN`，防止其被误报为成功。

“原子安装”主要约束最终包状态发布，不是文件系统零副作用。native复制与 `doRename()`已经在 prepare中发生，远早于全组reconcile；失败后依赖 `doPostInstall()`逐项补偿。断电、未捕获异常或实现缺口都要求磁盘对账，不能拿四阶段注释证明 ACID回滚。

### 练习 7：用锁与方法顺序重建四阶段

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void installPackagesLI(List<InstallRequest> requests) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'preparePackageLI(request.args, request.installResult);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ScanResult result = scanPackageTracedLI(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'reconciledPackages = reconcilePackagesLocked(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'commitPackagesLocked(commitRequest);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'executePostCommitSteps(commitRequest);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'synchronized (mInstallLock) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!args.doRename(res.returnCode, parsedPackage)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

在每个命中点标出 `mInstallLock/mLock`持有情况。再假设第二个child在scan失败，列出第一个child此前已经发生的 native、rename、appId动作和最终由哪条路径清理。

## 10. prepare重新建立完整包事实，并把rename放在scan之前

`preparePackageLI()`从 `args.getCodePath()`重新做完整 `PackageParser2.parsePackage()`，随后验证 dex metadata。上游 `PackageLite`只够位置、base/split和轻量策略；组件、权限、库、完整 Manifest与ABI需要 `ParsedPackage`。若 Session已经提供 `SigningDetails`就注入，否则重新读取签名；这不是 verifier投票的替代，而是包身份链的正式输入。

prepare在破坏性发布前设置多层硬门：instant不能位于外部volume、targetSdk至少O、不能有sharedUserId且签名方案至少v2；testOnly需要 `INSTALL_ALLOW_TEST`；static shared library只能在内部存储。更新还会处理 renamed package、禁止 targetSdk从runtime-permission模型退回旧模型、限制没有 `PackageManager.INSTALL_STAGED` flag的 persistent app更新，并先用upgrade keyset或签名能力检查快速失败。这里检查的是install flag，不是 `origin.staged`。

权限重定义也在这里处理。候选若以不兼容签名重声明其他非 `android`包拥有的permission，会得到 `INSTALL_FAILED_DUPLICATE_PERMISSION`；重声明平台permission则移除候选声明并记录警告。随后replace分支还会再次核对签名、restrict-update hash、sharedUserId与 full/instant转换，说明“前面验过签名”从不等于后续世界无需复核。

`SCAN_NO_DEX`加入 move和普通安装的 scan flags，只表示旧scan路径不要顺手dexopt；它不是“包没有dex”，也不是禁用稍后的 install-time dexopt。Move复用 PackageSetting里的 primary/secondary ABI；File路径调用 `derivePackageAbi()`。

关键时序是：完整parse与策略检查 → ABI/native处理 → `args.doRename()` → fs-verity → 异步启动 App Links验证 → `freezePackageForInstall()` → 构造 `PrepareResult` → scan。也就是说最终形状的随机目录在scan、reconcile和commit之前就可能存在；看到 `/data/app/~~...`只能证明 prepare推进过，不能证明包已经发布。

## 11. native有Session、copy、derive和组末wait四个触点

本版本不能用“三类来源各抽取一次”概括 native。对非 library APK，至少要区分四个触点：

1. PackageInstaller Session激活时调用 `extractNativeLibraries()`；普通文件同步复制，Incremental配置异步抽取；
2. 非 staged `FileInstallArgs.doCopyApk()`在复制 APK后调用 `copyNativeBinariesWithOverride()`；
3. `preparePackageLI()`调用 `PackageAbiHelperImpl.derivePackageAbi(..., extractLibs=true)`，实现会再次 `copyNativeBinariesForSupportedAbi()`，同时计算 primary/secondary ABI与最终 native paths；
4. `executePostCommitSteps()`收集所有 IncrementalStorage，处理完所有包后统一 `waitForNativeBinariesExtraction()`。

普通 Session会经历1和3；当前非 staged File分支会经历2和3；Incremental会在1、3重复配置并在4等待。`MoveInstallArgs`是例外：它搬的是完整已安装世界，prepare直接复用旧 PackageSetting的ABI，不走 File路径的derive。

第3处尤其容易被方法名误导。ABI推导不是纯函数：multiArch分别处理32/64位，普通包按 override或设备ABI选择；只要 `extractLibs`为真就会实际复制或配置 native。更新时若既有 `PackageSetting.cpuAbiOverrideString`非空，它优先于本次 `args.abiOverride`，否则才采用请求值。只有 `AndroidPackageUtils.isLibrary(parsedPackage)`使安装路径传 false，未更新的system包也会在 helper内关闭提取。

错误语义也不是单一 hard fail。multiArch分支显式容忍 `NO_NATIVE_LIBRARIES`与 `INSTALL_FAILED_NO_MATCHING_ABIS`；单ABI分支容忍前者，其余负码才抛 `PackageManagerException`。这些异常到 prepare外层统一映射成 `INSTALL_FAILED_INTERNAL_ERROR`；但 `PackageAbiHelperImpl.derivePackageAbi()`自己的 `IOException` catch只记录日志并继续返回当前ABI/path结果。诊断时要区分“被容忍的无库/不匹配”“helper失败码”“IOException被吞并”和“Incremental异步结果”。

最后的 wait只遍历storage并调用布尔返回的方法，却不检查返回值。因此“wait调用返回”不严格等于“所有抽取都成功”；false不会在这里改写 installResult。只有明确的状态、文件与服务日志合起来，才能判断 native是否可用。

## 12. doRename建立最终路径，但restorecon与IncFS异常暴露两个清理缺口

`FileInstallArgs.doRename()`以当前 `codeFile.getParentFile()`为 targetDir，通过 `getNextCodePath()`选择 `${targetDir}/~~<randomA>/<packageName>-<randomB>`。两段随机值各用16字节随机数做URL-safe Base64；常见内部存储因此呈现 `/data/app/~~.../...`，其他volume应以实际 targetDir为准。

`getNextCodePath()`只挑一个尚不存在的第一层目录，不创建它。`doRename()`先 `makeDirRecursive(parent, 0775)`；普通文件系统用 `Os.rename(before, after)`，Incremental则由 `IncrementalManager.renameCodePath()`创建 permanent bind storage、递归链接文件并解绑旧stage。后者不是字节复制，也不能用普通rename替代。

普通路径rename后才执行 `SELinux.restoreconRecursive(afterCodeFile)`；Incremental在 r48明确跳过这一递归步骤。通过后，代码才把 `codeFile/resourceFile`改成 after path，并重写 `ParsedPackage.codePath/baseCodePath/splitCodePaths`。canonical path失败发生在字段更新之后，因此常规 failure `doPostInstall()`能指向after路径清理。

restorecon失败却发生在字段更新之前：磁盘rename已经成功，`codeFile`仍指旧stage；后续 `cleanUp()`看到旧路径不存在便直接返回，最终随机目录可能遗留。这不是抽象上的可能性，而是本版本字段赋值顺序直接造成的补偿缺口。

Incremental还有不同的异常边界。`renameCodePath()`声明并可能抛 `IllegalArgumentException`，例如旧路径无法打开为 IncrementalStorage；`doRename()`只捕获 `IOException | ErrnoException`。这类未捕获异常不会被正常转换成“rename失败/空间不足”，还可能越过 `doPostInstall()`与结果回程。看到无observer结果且 Handler异常时，应检查这一分支，而不是只搜索 `INSTALL_FAILED_INSUFFICIENT_STORAGE`。

如果普通rename本身失败，刚创建的随机第一层父目录也可能留下；旧stage仍由 failure cleanup处理。文件切换由多步组成，任何“rename是原子操作”的结论都只能描述单次同文件系统 `Os.rename()`，不能覆盖父目录创建、restorecon与模型重写。

### 练习 8：推演普通rename与Incremental永久bind的失败点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final File afterCodeFile = getNextCodePath(targetDir, parsedPackage.getPackageName());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIncrementalManager.renameCodePath(beforeCodeFile, afterCodeFile);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Os.rename(beforeCodeFile.getAbsolutePath(), afterCodeFile.getAbsolutePath());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!onIncremental && !SELinux.restoreconRecursive(afterCodeFile)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'codeFile = afterCodeFile;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'parsedPackage.setCodePath(afterCodeFile.getCanonicalPath());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'String dirName = RANDOM_DIR_PREFIX' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'throw new IllegalArgumentException("Not an Incremental path: " + beforeCodeAbsolute);' frameworks/base/core/java/android/os/incremental/IncrementalManager.java
```

分别让父目录创建、Os.rename、restorecon、canonical path和IncFS open失败。每例写磁盘上的 before/after、`codeFile`字段、异常类型、是否进入普通failure cleanup，以及可能遗留什么。

## 13. fs-verity、App Links与PackageFreezer都在rename之后，且freezer可以是空壳

rename之后，prepare调用 `setUpFsVerityIfPossible(parsedPackage)`。两个模式都关闭时直接返回；standard模式只在相应 `.fsv_sig`存在且目标尚未启用时设置，所以“没有签名文件”是可选跳过，不是失败。legacy模式只为已有 privileged PackageSetting的 APK候选准备数据。

返回码不能笼统写成内部错误。standard设置时的IOException以及legacy显式失败会在 helper内部抛 `PrepareFailure(INSTALL_FAILED_BAD_SIGNATURE)`；外围只把 `InstallerException/IOException/DigestException/NoSuchAlgorithmException`转换成 `INSTALL_FAILED_INTERNAL_ERROR`。同叫“verity失败”，进入的异常层不同，对外码也不同。

非instant包随后调用 `startIntentFilterVerifications()`。这是 App Links域名默认处理的异步起点，不等待结果，也不是第259章阻塞 copy的 package verifier。它发生在 freezer之前，更不能拿“域名验证已启动”推断候选已经commit。

`freezePackageForInstall()`再保护更新窗口，但有明确例外：install flags带 `INSTALL_DONT_KILL_APP`时返回无目标的 `new PackageFreezer()`，不会把包加入 frozen set，也不会杀旧进程。普通分支才冻结包并把freezer交给 `PackageInstalledInfo`延后关闭。因而“所有更新都由freezer阻止旧进程活动”是过度概括。

prepare成功返回后才进入scan、reconcile与commit。`commitPackagesLocked()`是本链首次可以称为包状态发布的节点；rename、verity、App Links启动与freezer都只是它的前置副作用。commit前普通失败会由 installPackages finally与外层 `doPostInstall(failure)`补偿，但第12节的字段顺序缺口仍需单独对账。

## 14. AppData按installed与用户运行态创建，失败策略不会回滚已commit包

`commitPackagesLocked()`返回并退出 `mLock`后，`executePostCommitSteps()`先为每个包调用 `prepareAppDataAfterInstallLIF()`。此时仍持有 `mInstallLock`。方法先从Settings取 `PackageSetting`并写 kernel mapping，然后遍历 `mUserManager.getUsers(false /*excludeDying*/)`：参数为 false意味着不排除正在移除的用户；实现仍排除partial与pre-created用户。旧结论“只遍历非dying用户”正好相反。

对每个用户还要先满足 `ps.getInstalled(userId)`。目录flags由运行态决定：`isUserUnlockingOrUnlocked()`为true时准备 DE+CE；仅running但未解锁只准备 DE；未运行直接跳过。前一种判断还特意把STOPPING/SHUTDOWN但key仍解锁的用户算作true，也只有这组用户会在内部AppData之后再调用 `StorageManagerInternal.prepareAppDataAfterInstall()`处理外部存储/OBB相关状态。

`prepareAppDataLeafLIF()`计算 appId与 `seInfo + seInfoUser`，通过 `Installer.createAppData(volumeUuid, packageName, userId, flags, appId, seInfo, targetSdk)`进入 installd并取得CE inode。Linux UID只是隔离的一部分，volume、user、DE/CE和SELinux输入共同决定目录语义。

失败是 post-commit best-effort策略。system app首次创建失败会记录critical信息，销毁对应数据后重试；第三方包只记错误，避免擅自wipe用户数据。两类失败都没有把本次 `PackageInstalledInfo.returnCode`改回失败，所以此时包状态可能已成功发布，而某用户数据目录仍有问题。

若请求CE且inode有效，代码把值写回对应user的 PackageSetting；源码只在此处更新内存字段，没有就地展示一次专门的持久化调度。随后还会准备AppData内容。对非system user、系统升级或首次开机，leaf方法内部会以 `updateReferenceProfileContent=false`准备profile；安装主链稍后还会以 true针对安装用户再准备一次。两处触发用户集合和 `updateReferenceProfileContent`参数不同、集合也可能重叠，不能简单去重成一处。

## 15. profile先于条件dexopt，Incremental native wait的布尔结果却被忽略

每个包的 post-commit顺序是：必要时打开 IncrementalStorage → AppData → 可选清 code cache → 仅replace时更新DexManager → `prepareAppProfiles(..., true)` → 条件性layout编译与dexopt → 通知 BackgroundDexOptService和包变化观察者。全部包完成这些步骤后，才对收集到的 IncrementalStorage做组末 native等待。

profile准备无论是否dexopt都会调用，而且先于dexopt；`ArtManagerService.prepareAppProfiles()`对false结果和 `InstallerException`都只记日志，所以这个顺序是优化输入依赖，不是安装成功硬门。install-time dexopt只有三个外层条件同时成立才调用optimizer：包不是instant，或全局开关允许instant优化；包不 debuggable；code path不在 Incremental File System。进入optimizer后，无code包或不需处理的单个路径仍可得到 `DEX_OPT_SKIPPED`。layout预编译还要额外由系统属性开启。dexopt reason是 `REASON_INSTALL`，device restore/setup会再加 restore flag。

代码直接调用 `mPackageDexOptimizer.performDexOpt()`并忽略返回值，旁边注释要求不要因dexopt失败码否决安装。因此编译失败仍可依靠解释执行、JIT或以后后台优化；但这个保证针对正常返回的dexopt结果，不应扩大成“任意未捕获运行时异常都不会影响回程”。成功走到末尾还会通知 BackgroundDexOptService，使更新包从历史编译失败黑名单中重新评估。

Incremental等待也忽略服务返回的 boolean：false不会改安装status。C++实现用带谓词的 `mJobCondition.wait()`，没有超时；只要服务仍running且该mount仍有pending或queued job，它就会在 `mInstallLock`内持续等。更尖锐的边界发生在等待之前：`openStorage(pkg.getCodePath())`返回 null会在 commit之后抛 `IllegalArgumentException`。此时 `installPackagesLI.success`已经是 true，finally不会走失败清理；它甚至仍会为 Incremental+V4发送第259章所述事后 `ACTION_PACKAGE_VERIFIED`。异常随后越过没有 finally保护的 `processInstallRequestsAsync()`，跳过整组 `doPostInstall()`、`restoreAndPostInstall()`与install observer；异常前已处理child的package-change observer却可能已经收到通知。freezer的正常显式关闭路径也被绕过，只能等待其finalizer等更晚的兜底，形成“包已commit、外部结果未交付”的分叉。

这一具体边界也解释了为什么终局诊断必须同时看包表、Settings、code path、Handler异常和observer，不能只等一个 callback。组末wait无超时且仍处于 `mInstallLock`内；某个Incremental native job不结束，就会串行拖住后续安装，即使前面的包已经commit。

### 练习 9：建立post-commit资源与异常矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'prepareAppDataAfterInstallLIF(pkg);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags = StorageManager.FLAG_STORAGE_DE | StorageManager.FLAG_STORAGE_CE;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'flags = StorageManager.FLAG_STORAGE_DE;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ceDataInode = mInstaller.createAppData(volumeUuid, packageName, userId, flags,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mArtManagerService.prepareAppProfiles(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final boolean performDexopt =' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& !pkg.isDebuggable()' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& (!onIncremental);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPackageDexOptimizer.performDexOpt(pkg, realPkgSetting,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'NativeLibraryHelper.waitForNativeBinariesExtraction(incrementalStorages);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'throw new IllegalArgumentException(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

手算普通release、debuggable、instant默认、instant开关开启、Incremental五例的 AppData/profile/dexopt/native动作。再分别让 `createAppData`失败、dexopt返回失败、native wait返回false、Incremental openStorage返回null，写出是否已commit、是否改returnCode、是否还能到observer。

## 16. 用证据矩阵定位“有文件、没结果”和“结果成功、资源未齐”

本章最可靠的排障方法不是问“安装到哪一步”，而是逐项回答：

| 证据 | 能证明 | 仍需继续查 |
|---|---|---|
| Session seal与stage目录 | 候选字节进入受控区 | `copyApk()`是否接管 |
| `copyApk` trace/返回码 | 路径准备方法已经返回 | 是否发生字节复制、是否清理失败stage |
| `/data/app/~~...`或volume对应随机路径 | rename/bind至少部分成功 | restorecon、ParsedPackage重写、scan与commit |
| prepare/scan/reconcile日志 | 可预测检查推进情况 | `commitPackagesLocked()`是否返回 |
| Settings/包查询可见 | 包状态已经发布 | post-commit与observer是否完整 |
| DE/CE目录与seInfo日志 | 某用户AppData状态 | 其他用户、external data、CE inode |
| profile/dexopt日志 | 优化准备状态 | native异步抽取和公开结果 |
| Session callback或install observer | 结果回程已交付 | 后台dexopt、以后用户解锁准备 |

失败恢复也应按阶段写：验证或位置失败不调用copy；copy普通返回失败仍走统一结果，但跳过pre/post钩子；prepare、scan或reconcile以状态码正常退出时，外层post钩子清理 File路径或按Move矩阵保源；restorecon失败可能因 `codeFile`仍指旧路径而留下after目录；commit后的未捕获异常则不能再假装“整组从未安装”，还可能截断observer。

九个自检问题可以验证是否真正拆清边界：

1. 为什么 `origin.staged=true`不能证明 `SessionParams.isStaged=true`？
2. 为什么 `origin.existing=true`仍可能执行 `copyPackage()`？
3. 为什么当前树中 `fromUntrustedFile()`方法存在却不能算生产入口？
4. copy失败后为什么 `doPreInstall(failure)`未必运行？
5. native为何会在 Session、File copy、ABI derive和组末wait四处出现？
6. 进入 `executePostCommitSteps()`时哪把锁已经释放，又仍持哪把锁？
7. restorecon失败为何会让cleanup看错路径？
8. AppData或dexopt失败为什么可能不改已经commit的安装码？
9. 哪个具体异常能造成“commit成功但install observer没收到结果”？

Android 11 r48之后，PackageManager拆分类、ART Service、Incremental、staging与 `/data/app`布局持续演进。迁移结论时应重新搜索调用点和锁范围，而不是照搬方法名。第261章将沿着replace分支继续追 PackageFreezer、旧进程终止、旧 code path删除与用户数据保留，回答新旧包怎样真正交接。
