# 258 Android PackageInstaller用户确认、InstallStart、未知来源授权与安装UI回传链

本章源码基线是 Android 11 / API 30 / `android-11.0.0_r48`。最容易误判的现场不是“安装按钮在哪”，而是安装器已经收到 `STATUS_PENDING_USER_ACTION`，用户又在 Settings 打开“允许来自此来源”，界面随后消失，却迟迟没有安装成功。这里至少有三本互不替代的账：来源应用有没有资格请求安装、用户是否同意当前 APK、PMS 是否最终提交包事实。

本章沿一次真实请求回答：已有 `PackageInstaller.Session`与直接打开 APK URI 为什么走不同入口；system_server为什么把确认 Intent装进状态回调而不直接弹窗；`InstallStart`怎样重建来源身份；用户限制、AppOp和 Settings开关怎样串联；点击同意后又由谁接住最终结果。读完应能只凭 sessionId、installId、public status、legacy status和 Activity result，判断请求停在哪一层。

除专门讨论的分支外，Session主线限定为 single-package、non-staged、非 APEX、文件已完整落入 stage的普通 APK。multi、staged与重启边界放在第15节；DataLoader的 `STATUS_PENDING_STREAMING`已在第257章完成，不把它硬塞进本章确认状态机。

## 1. 两个“允许”和一个“成功”为什么不能合并

先把三个问题分开：

1. 来源允许：对普通具名未知来源，发起者是否声明了 `REQUEST_INSTALL_PACKAGES`，用户限制是否放行，它的 `OP_REQUEST_INSTALL_PACKAGES`是否为 ALLOWED；trusted旁路与 anonymous兼容分支另算。
2. 本次确认：用户是否接受这一份已能解析身份的 APK；已有 Session用 `mPermissionsManuallyAccepted`记住本次回答。
3. 安装终局：PMS后续的 verifier、prepare、scan、reconcile和 commit是否成功。

`REQUEST_INSTALL_PACKAGES`不是 `INSTALL_PACKAGES`。前者是 app-op permission的请求资格，InstallStart读取的是声明关系，Settings开关改的是 AppOp；后者是 signature/privileged级系统安装能力，也是 r48 服务端 `setPermissionsResult()`的调用能力。来源 AppOp变成 ALLOWED不会让第三方安装器获得静默安装权，用户点击“安装”也不会一次性批准目标 APK声明的危险权限。

同样，`STATUS_PENDING_USER_ACTION=-1`不是负数意义上的失败，而是非终态：“调用方现在应在合适的前台时机启动 `Intent.EXTRA_INTENT`。”只有 `STATUS_SUCCESS`或某个 `STATUS_FAILURE_*`才是本次 PackageInstaller操作的公开终局。

## 2. 两条入口先分叉，身份、数据与结果也随之分叉

系统 PackageInstaller应用同时承接两类请求：

| 维度 | 已有 Session确认 | APK URI入口 |
|---|---|---|
| 到达 `InstallStart`前 | installer已创建、写入、seal并本地验证 Session | 调用方只有 `content:`或 `package:` URI |
| 外部动作 | framework经 status receiver交出 `ACTION_CONFIRM_INSTALL` | `ACTION_VIEW`或 `ACTION_INSTALL_PACKAGE` |
| 用户点同意 | `setPermissionsResult(sessionId, true)`，原 Session继续 | `file:`快照进入 `InstallInstalling`新建特权 Session；`package:`走已有包安装 |
| 字节所有权 | 原 Session stage | `content:`先复制到安装器私有临时文件，再复制进新 Session |
| 结果通道与可信度 | 派发时快照的 `mRemoteStatusReceiver`是 Session结果通道；满足前置条件且在快照前再次 `commit()`才可换 receiver | `content:`/内部 `file:`用 installId匹配的包限定广播；`package:`的权威 int被 r48 UI丢弃，随后 UI结果不可信 |

共有路线可以压缩为：来源身份整理 → 来源限制/AppOp → 当前 APK确认 →〔原 Session继续｜创建系统安装器 Session｜调用已有包恢复 API〕→ PMS结果。两类外部入口会经过同一个确认 Activity，但后半程实际有三支，不能因此把它们画成同一个 Session。

### 练习 1：先画出三条互不混淆的路线

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void commit(@NonNull IntentSender statusReceiver, boolean forTransfer)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (hasParentSessionId()) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertCallerIsOwnerOrRootLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertNoWriteFileTransfersOpenLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F "Can't install packages while in secure FRP" frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'final Intent intent = new Intent(PackageInstaller.ACTION_CONFIRM_INSTALL);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '<activity android:name=".InstallStart"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F 'PackageInstaller.ACTION_CONFIRM_INSTALL.equals(intent.getAction());' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'nextActivity.setClass(this, InstallStaging.class);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'nextActivity.setClass(this, PackageInstallerActivity.class);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'mInstaller.setPermissionsResult(mSessionId, true);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'new PackageInstaller.SessionParams(' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'getPackageManager().installExistingPackage(appInfo.packageName);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
```

沿命中行前后阅读，分别画“已有 Session等待确认”“content URI”“package URI”三条线。每条都写明 Session owner、APK字节迁移和权威结果是否存在；`package:`没有新 Session或 APK复制，就明确写“无”。若画出 `InstallInstalling`继续原 Session，就说明入口已经串错。再把 status链启动前的同步异常画在 commit左侧，避免把“没有 callback”一律诊断成 pending丢失。

## 3. Session确认门位于本地验证之后、PMS交接之前

普通非 staged Session在 `streamValidateAndCommit()`返回 true后已有 `mPackageName`、`mSigningDetails`和 resolved base，随后 `handleInstall()`进入 `makeSessionActiveLocked()`。所以确认 UI面对的是已能解析出包身份的输入，不是任意未封口字节；但它仍早于 PMS的后半程，并不证明签名升级关系、空间、verifier和最终事务都会通过。

`needToAskForPermissionsLocked()`以当前 `mInstallerUid`计算静默资格：

- 有通用 `INSTALL_PACKAGES`即可；
- 有 `INSTALL_PACKAGE_UPDATES`且目标包 UID不是 -1，只覆盖更新；
- 有 `INSTALL_SELF_UPDATES`且目标包 UID等于 installer UID，只覆盖自更新；
- root、system或同用户 Device Owner/affiliated Profile Owner可以旁路；
- `INSTALL_FORCE_PERMISSION_PROMPT`无论上述资格如何都强制询问。

目标 UID按 Session的 target user查询。Device Owner分支还先要求 Session user等于 installer UID所属 user，因此另一个用户里的管理身份不能直接跨用户静默批准。判断不能缓存：Session transfer会改变 installer身份。

这道门并不覆盖所有 Session。`handleInstall()`先处理 staged Session并交给 `StagingManager`，APEX又只能走 staged；multi-package parent自身没有目标包，真正调用确认判断的是 child。r48的 multi细节还会制造部分提交边界，第15节再收口。

### 练习 2：手算静默资格矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private boolean needToAskForPermissionsLocked()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPm.checkUidPermission(android.Manifest.permission.INSTALL_PACKAGES,' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPm.checkUidPermission(android.Manifest.permission.INSTALL_SELF_UPDATES,' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPm.checkUidPermission(android.Manifest.permission.INSTALL_PACKAGE_UPDATES,' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'final int targetPackageUid = mPm.getPackageUid(mPackageName, 0, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'final boolean isInstallerRoot = (mInstallerUid == Process.ROOT_UID);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '(params.installFlags & PackageManager.INSTALL_FORCE_PERMISSION_PROMPT) != 0;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (userId != UserHandle.getUserId(mInstallerUid)) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'dpmi.canSilentlyInstallPackage(' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mStagingManager.commitSession(this);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

手算六例：普通新装、仅有 update权限的新装、该权限下的更新、同 UID自更新、同用户 Device Owner、带 FORCE_PROMPT的 system UID。每例写“需不需要问”及决定性布尔项，再解释 staged为何不进入同一判断位置。

## 4. pending回调是一只信封，不是 system_server直接弹窗

需要询问时，Session构造显式指向系统 package installer包的 `ACTION_CONFIRM_INSTALL`，只放入 sessionId。随后 `sendOnUserActionRequired()`再创建外层 fill-in Intent：

| 层 | 关键内容 | 谁消费 |
|---|---|---|
| 外层状态 | sessionId、`STATUS_PENDING_USER_ACTION` | 安装器提交时提供的 `IntentSender` |
| 内层动作 | `Intent.EXTRA_INTENT`中的显式包确认 Intent | 安装器选择合适时机启动 |

这样设计把“安装事务需要交互”与“何时抢占屏幕”分开。公开 API明确建议：用户正在使用安装器时可立即启动，否则先发通知把用户带回前台。system_server只送状态，不替应用决定后台拉起 UI。

发出 pending后，Session调用 `closeInternal(false)`释放 commit额外增加的 active引用，因此观察者可能看到它变 idle；stage仍 sealed，写入口不会重开。更窄的失败边界是 `IntentSender.SendIntentException`被吞掉：若 receiver已经失效，framework不会自动弹窗或生成终态，Session可能只剩一份等待外部处置的 sealed状态。

status receiver也不是第一次 `commit()`后冻结的字段。`markAsSealed()`先把参数写进 `mRemoteStatusReceiver`，再对已经 sealed的 Session快速返回；因此在 prepared、未 destroyed、无 open writer、FRP与 transfer检查均通过时，owner/root再次 `commit()`可替换尚未派发的 pending或终态去向，并重投异步消息。若 `mCommitted=true`，验证只会快速返回；若 Session已经 relinquished给 PMS，重投反而会在 `makeSessionActiveLocked()`失败。`dispatchSessionFinished()`又会先快照 receiver再排消息，所以快照后的 recommit改不了已派发结果。这是有阶段边界的恢复钩子，不是任意时刻安全换观察者。

### 练习 3：拆开外层状态与内层动作

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'intent.setPackage(mPm.getPackageInstallerPackageName());' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'intent.putExtra(PackageInstaller.EXTRA_SESSION_ID, sessionId);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'fillIn.putExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_PENDING_USER_ACTION);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'fillIn.putExtra(Intent.EXTRA_INTENT, intent);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'target.sendIntent(context, 0, fillIn, null, null);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '} catch (IntentSender.SendIntentException ignored) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'closeInternal(false);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mRemoteStatusReceiver = statusReceiver;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '// After updating the observer, we can skip re-sealing.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'statusReceiver = mRemoteStatusReceiver;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (mCommitted) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'public static final int STATUS_PENDING_USER_ACTION = -1;' frameworks/base/core/java/android/content/pm/PackageInstaller.java
```

写出 receiver存活与失效两种时序。前者标出谁真正调用 `startActivity()`；后者回答为什么“commit已返回、Session不活跃、没有失败回调”仍不能判成成功或自动取消。再给同一 sealed Session依次传 receiver A、B，分别令 B发生在结果快照前、快照后和 relinquished后，判断通知与失败去向。

## 5. 同意与拒绝回到 framework，但这是一项窄系统能力

确认 Activity调用 `PackageInstaller.setPermissionsResult(sessionId, accepted)`，经 `IPackageInstaller`进入 Service。Service只强制调用者拥有 `INSTALL_PACKAGES`，随后按 sessionId查表；它不再核对调用包就是确认 Intent显式指向的 package installer，也不检查 Session owner或 target user。AOSP PackageInstaller应用能调用，是因为它在 Manifest中持有这项平台权限。

Session侧的门也很窄：只要求 `mSealed`。没有独立的 `AWAITING_USER_ACTION`状态，也没有一次性 nonce。于是这不是“知道 sessionId就能批准”，而是“任何持有该高权限的系统主体都被信任”；对厂商系统组件做安全审计时必须把这个能力面算进去。

接受与拒绝完全不对称：

- true：锁内置 `mPermissionsManuallyAccepted=true`，向 Session Handler投递 `MSG_INSTALL`；第二轮再次走 `makeSessionActiveLocked()`，不重新解析 APK，确认门因该位直接放行。
- false：立即 `destroyInternal()`，以 `INSTALL_FAILED_ABORTED`和“User rejected permissions”派发终态。

true只表示排入第二轮，不表示 PMS已经接管。`makeSessionActiveLocked()`还会检查 relinquished/destroyed，随后处理继承文件和 native库，构造 `ActiveInstallSession`；只有它真正传给 `mPm.installStage()`才进入 PMS后半程。`mPermissionsManuallyAccepted`没有写入 Session XML，status receiver也不持久化，所以重启不能恢复原确认握手。

### 练习 4：核对批准能力与第二轮完成点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'void setPermissionsResult(int sessionId, boolean accepted);' frameworks/base/core/java/android/content/pm/IPackageInstaller.aidl
grep -n -F 'mContext.enforceCallingOrSelfPermission(android.Manifest.permission.INSTALL_PACKAGES, TAG);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'session.setPermissionsResult(accepted);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'if (!mSealed) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPermissionsManuallyAccepted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mHandler.obtainMessage(MSG_INSTALL).sendToTarget();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'dispatchSessionFinished(INSTALL_FAILED_ABORTED, "User rejected permissions", null);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (mRelinquished) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPm.installStage(installingSession);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'android:name="android.permission.INSTALL_PACKAGES"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F 'new ChildStatusIntentReceiver' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (installingChildSession != null) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'throw new PackageManagerException("No child sessions found!");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'dispatchSessionFinished(PackageManager.INSTALL_SUCCEEDED, "Session staged", null);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'you may commit the session again.' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F 'writeBooleanAttribute(out, ATTR_COMMITTED, isCommitted());' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mCommitted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

推演“未 sealed”“sealed但从未发 pending”“已 relinquished”“正常等待确认”四种 Session收到 true/false的结果。结论必须区分 Binder调用被接受、消息已排队、ActiveInstallSession已生成和 PMS已接手；再说明 multi全体等待与 staged success为何都不能套用普通单 Session终局。

## 6. InstallStart是唯一导出的安装导流边界

AOSP PackageInstaller Manifest把 `InstallStart`导出，注册 `ACTION_VIEW`、`ACTION_INSTALL_PACKAGE`和隐藏的 `ACTION_CONFIRM_INSTALL`；`InstallStaging`、`PackageInstallerActivity`、`InstallInstalling`及最终结果 Activity都不导出。外部输入先经过同一个 trampoline，内部页面才可以信任它整理出的 extras。

`InstallStart`只以 action是否等于 `ACTION_CONFIRM_INSTALL`识别 Session入口，不以 URI是否为空猜测。若通过 Session到达且 `getCallingPackage()`为空，它从 `SessionInfo.getInstallerPackageName()`补来源包；随后把 calling package、source ApplicationInfo和 originating UID显式写给内部 Activity。

它复制原 Intent并覆盖 flags为 `FLAG_ACTIVITY_FORWARD_RESULT | FLAG_GRANT_READ_URI_PERMISSION`。FORWARD_RESULT让下游最终 Activity result直接交还最初调用者；它不是 PackageInstaller Session的 status receiver，也不把 pending状态转成终态。导流 Activity启动下一个页面后立即 finish。

### 练习 5：验证导出面与路由规则

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F '<activity android:name=".InstallStart"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F '<activity android:name=".InstallStaging"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F '<activity android:name=".PackageInstallerActivity"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F '<activity android:name=".InstallInstalling"' frameworks/base/packages/PackageInstaller/AndroidManifest.xml
grep -n -F 'final int sessionId = (isSessionInstall' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'sessionInfo.getInstallerPackageName()' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'nextActivity.setFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'nextActivity.putExtra(PackageInstallerActivity.EXTRA_CALLING_PACKAGE, callingPackage);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'startActivity(nextActivity);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
```

按 Manifest命中行确认哪个组件可被外部显式触达；再为 ACTION_CONFIRM_INSTALL、content、package和其他 scheme写路由表。说明为何 FORWARD_RESULT不能替代 commit `IntentSender`。

## 7. 来源身份不是一个可随便相信的 Intent字符串

InstallStart先得到 calling package的 `ApplicationInfo`。能识别包时 originating UID取 `sourceInfo.uid`；否则向 ActivityManager查询该 Activity的 launched-from UID。调用者自己塞入的 `EXTRA_ORIGINATING_UID`默认不可信，只有两类中转者可以代传：持有 `MANAGE_DOCUMENTS`的文档管理器，或 authority为 `downloads`、ApplicationInfo确为 system app且 UID吻合的 Downloads Provider。

`EXTRA_NOT_UNKNOWN_SOURCE=true`也不是普通调用者的旁路。只有 sourceInfo带 `PRIVATE_FLAG_PRIVILEGED`时，InstallStart才把它解释成 trusted source；PackageInstallerActivity稍后还会用 calling package、sourceInfo和同一 extra再判一次。这个旁路只跳过“未知来源”声明、限制与 AppOp，不跳过最前面的 `DISALLOW_INSTALL_APPS`，也不会替用户点击当前 APK的确认按钮。

来源 UID可对应多个包。O及以上声明门先取该 UID所有包的最大 targetSdk；只要最大值至少26，就要求该 UID关联的某个包出现在 `REQUEST_INSTALL_PACKAGES`的 app-op permission package集合里。确认 Activity随后又从 `getPackagesForUid()`选择 AppOp包名：优先 mCallingPackage，否则取数组第一项。于是安全主体主要是 UID，展示与 AppOp键却仍含 packageName；shared UID现场要同时记录两者。

### 练习 6：对抗伪造来源与 shared UID

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'ActivityManager.getService()' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F '.getLaunchedFromUid(getActivityToken());' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'Manifest.permission.MANAGE_DOCUMENTS' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'if (isSystemDownloadsProvider(callingUid)) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'ApplicationInfo.PRIVATE_FLAG_PRIVILEGED' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'Intent.EXTRA_NOT_UNKNOWN_SOURCE' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'getMaxTargetSdkVersionForUid(this, originatingUid);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'declaresAppOpPermission(' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'String[] packagesForUid = mPm.getPackagesForUid(sourceUid);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'if (packageName.equals(mCallingPackage)) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
```

推演普通 App伪造 originating UID、普通 App伪造 trusted extra、Documents中转、假 downloads Provider和两个包共享 UID五例。每例写出最终 UID来自哪里、哪个包名成为 AppOp键、声明门是否可过。

## 8. 未知来源门是用户限制、可信旁路和 AppOp的有序矩阵

PackageInstallerActivity先查 `DISALLOW_INSTALL_APPS`。system基础限制显示不可用，管理员限制打开 Admin Support并结束；这道总门连 trusted source也不能绕过。

通过总门后，只有 `mAllowUnknownSources=true`或 privileged+NOT_UNKNOWN_SOURCE才直接进入当前 APK确认。其余请求依次检查 `DISALLOW_INSTALL_UNKNOWN_SOURCES`与 GLOBAL版本：system基础限制显示错误，管理员来源打开对应支持页。用户限制和 AppOp是两本账，ALLOWED不能压过限制。

没有来源包名时，r48显示 anonymous source警告；用户可以只对当前 Activity继续，系统没有 packageName可写 AppOp。能识别来源时才执行 `noteOpNoThrow(OP_REQUEST_INSTALL_PACKAGES, uid, package)`：

| mode | r48动作 | 能否进入当前 APK确认 |
|---|---|---|
| DEFAULT | 先写成 ERRORED，再落入 blocked Dialog | 否 |
| ERRORED | blocked Dialog，可去单包 Settings | 否 |
| ALLOWED | `initiateInstall()` | 是 |
| 其他值 | 记错误并 finish | 否 |

`PackageManager.canRequestPackageInstalls()`与 UI方向一致却不完全等价。它还要求调用 UID拥有所问 package、targetSdk至少26、非 instant、该 package自身声明权限；公开调用在未声明时抛 `SecurityException`，不是简单返回 false。PMS随后检查两项未知来源限制与 ExternalSourcesPolicy，但没有检查 UI最先处理的 `DISALLOW_INSTALL_APPS`，也无法表达没有可归因 package的 anonymous兼容分支。一个具名来源 App得到的 boolean，不能替代另一条实际 Activity状态机。

### 练习 7：手推来源门而不是只看一个 boolean

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'UserManager.DISALLOW_INSTALL_APPS, Process.myUserHandle());' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'if (mAllowUnknownSources || !isInstallRequestFromUnknownSource(getIntent())) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'UserManager.DISALLOW_INSTALL_UNKNOWN_SOURCES_GLOBALLY' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'if (mOriginatingPackage == null) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'mAppOpsManager.noteOpNoThrow(appOpCode,' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'case AppOpsManager.MODE_DEFAULT:' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'mOriginatingPackage, AppOpsManager.MODE_ERRORED);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'case AppOpsManager.MODE_ALLOWED:' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'mAllowUnknownSources = true;' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'mAppOpsManager.noteOpNoThrow(appOpCode, mOriginatingUid, mOriginatingPackage);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'if (info.targetSdkVersion < Build.VERSION_CODES.O) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (isInstantApp(packageName, userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'throw new SecurityException("Need to declare " + appOpPermission' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mExternalSourcesPolicy.getPackageTrustedToInstallApps(packageName, uid);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

建立“总安装限制 × unknown限制 × trusted/anonymous/known × AppOp mode”矩阵。分别说明 anonymous请求为何没有同一 package可供 `canRequestPackageInstalls()`比较，以及具名来源的 API结果为何仍不能覆盖 UI的总安装限制。

## 9. Settings开关只改每来源 AppOp，RESULT_OK也有前提

blocked Dialog构造 `ACTION_MANAGE_UNKNOWN_APP_SOURCES`并附 `package:<originatingPackage>`，AOSP Settings因此路由到单包 `ManageAppExternalSourcesActivity`和 `ExternalSourcesDetails`。列表/详情先用 `AppStateInstallAppsBridge`计算两项：包是否声明 app-op permission、当前 mode是什么。`isPotentialAppSource()`要求“mode非 DEFAULT或声明过”之一，否则开关禁用。

开关最终只执行：

`setMode(OP_REQUEST_INSTALL_PACKAGES, uid, packageName, ALLOWED或ERRORED)`。

它不 grant runtime permission，也不授予 `INSTALL_PACKAGES`。从允许切回拒绝时，Settings还会 kill非 core来源 UID，避免仍运行的来源进程沿用旧流程；core UID不杀。

Settings只有在开关实际变化、且承载组件正是单包 ManageAppExternalSourcesActivity时才调用 `setResult(ALLOWED ? RESULT_OK : RESULT_CANCELED)`。结果要等该 Activity结束才回到确认页。PackageInstallerActivity只在 request code匹配且 result为 OK时把内存位 `mAllowUnknownSources=true`、再 note一次 AppOp并进入确认；其他结果直接 finish。第二次 note是使用记录，不是第二次授权。`mAllowUnknownSources`只随 Activity saved state保存，跨请求资格仍以 AppOp为准。

因此“限制 → AppOp → 当前确认”只是首次检查顺序，不是持续不变量。Settings回传 OK后，`onActivityResult()`直接 `initiateInstall()`，既不重跑两类用户限制，也不检查第二次 note返回的 mode；管理员限制或 AppOp若在往返窗口内变化，本次 UI仍可能继续。开关实际从允许改成拒绝时的 `killUid()`也只结束来源进程，不会替它 abandon已存在的 Session，更不会主动关闭正在运行的系统确认页；单纯 Back或 RESULT_CANCELED不触发这项清理。

## 10. content与package输入先解决“字节在哪里”，再谈确认

APK URI入口不是一种数据模型：

- `content:`：InstallStart送往 `InstallStaging`。后台从 ContentResolver读取，复制到 PackageInstaller的 device-protected no-backup目录 `package*.apk`，再以内部 `file:` URI经过 `DeleteStagedFileOnResult`进入确认页。
- `package:`：表示设备上已有、可能只是在当前用户未安装的包；确认后 `InstallInstalling`调用 `installExistingPackage()`，没有 APK字节复制，也不创建普通安装 Session。r48这里还藏着一个假成功边界：API以 int返回多数失败码，`ApplicationPackageManager`只把 `INSTALL_FAILED_INVALID_URI`转换成 `NameNotFoundException`，而 Activity忽略返回值、未抛异常便直接 `launchSuccess()`。例如确认后策略发生竞态变化，PMS因新出现的 `DISALLOW_INSTALL_APPS`返回 `INSTALL_FAILED_USER_RESTRICTED`时，页面仍可能误报成功。
- 其他 scheme：InstallStart返回 `INSTALL_FAILED_INVALID_URI`，不会把内部确认页当通用 URI解析器。

content快照防止外部 Provider在 UI解析与真正写 Session之间换字节。确认流程回程后，`DeleteStagedFileOnResult`删除临时 APK；设备启动时 TemporaryFileManager还会删除本次 boot之前遗留的 no-backup文件。它不是 `/data/app`最终 code path，也不是 Session stage。

已有 Session确认不重新取第三方 URI。PackageInstallerActivity要求 `SessionInfo`存在、sealed且 `resolvedBaseCodePath`非空，再将内部路径包装成 file URI用于解析图标和包信息。UI可展示只说明输入足够解析，不等于 PMS最终验证成功。

### 练习 8：核对快照、内部文件与显示输入

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'ContentResolver.SCHEME_CONTENT' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStart.java
grep -n -F 'File.createTempFile("package", ".apk", context.getNoBackupFilesDir())' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/TemporaryFileManager.java
grep -n -F 'getContentResolver().openInputStream(packageUri)' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStaging.java
grep -n -F 'installIntent.setClass(InstallStaging.this, DeleteStagedFileOnResult.class);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallStaging.java
grep -n -F 'sourceFile.delete();' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/DeleteStagedFileOnResult.java
grep -n -F 'if (info == null || !info.sealed || info.resolvedBaseCodePath == null) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'packageUri = Uri.fromFile(new File(info.resolvedBaseCodePath));' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'mPkgInfo = PackageUtil.getPackageInfo(this, sourceFile,' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/PackageInstallerActivity.java
grep -n -F 'getPackageManager().installExistingPackage(appInfo.packageName);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'if (res == INSTALL_FAILED_INVALID_URI) {' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'return PackageManager.INSTALL_FAILED_USER_RESTRICTED;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

画出 content字节的 Provider → private snapshot → Session stage → final code path四个位置，给每条迁移标出所有者与清理者；再解释 package scheme为何不能画进这条复制链。最后设定“确认页初检后才新增 `DISALLOW_INSTALL_APPS`”的竞态，对照 int返回、异常捕获和 `launchSuccess()`说明假成功如何出现。

## 11. 确认页展示目标包，但来源包才是 AppOp主体

PackageInstallerActivity同时持有两组身份：`mOriginatingPackage/mOriginatingUid`表示谁发起安装，`mPkgInfo.packageName`表示正在安装谁。来源身份用于限制和 AppOp；目标身份用于新装/更新文案、图标和最终包规则。把两者混成“安装包名”会把授权记到错误对象。

file URI由 `PackageUtil.getPackageInfo()`解析；package URI查询现有 `PackageInfo`。`initiateInstall()`处理 canonical旧名，再以 `MATCH_UNINSTALLED_PACKAGES`查询目标：只有 ApplicationInfo带 `FLAG_INSTALLED`才按更新显示，system App又用单独警示文案。r48布局不列目标 APK的权限清单；注释里的“no permissions”不能当成 Manifest事实。

确认点击还有两层界面防护。Window添加 `SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS`，positive button启用 `setFilterTouchesWhenObscured(true)`；Activity进入 pause会禁用按钮，resume后按 `mEnableOk`恢复，非触摸模式默认焦点落在取消。它们减少覆盖点击风险，却不替代来源身份和 PMS验证。

## 12. 同一个界面关闭，服务端后果可能完全不同

通过来源门后，按钮才有明确的当前 APK语义：

| 动作 | 已有 Session | 直接 APK URI |
|---|---|---|
| positive | `setPermissionsResult(true)`并 finish | 启动 `InstallInstalling` |
| negative | `setPermissionsResult(false)`，Session ABORTED | Activity result取消，尚未创建/提交安装 Session |
| back | `setPermissionsResult(false)` | 只结束当前 UI链 |

但在进入这个确认面板之前，许多关闭只调用 `finish()`：InstallStart声明门失败、SessionInfo状态异常、用户限制 Dialog、Admin Support跳转、anonymous取消、blocked Dialog取消、Settings非 OK回程、解析错误和 Wear分支。它们未必替已有 Session调用 false。于是“PackageInstaller界面消失”既不能证明用户拒绝，也不能证明 stage已销毁；安装器必须继续看原 status receiver或显式 abandon。

直接 URI路径的 Activity result又不是 Session API status。只有请求携带 `EXTRA_RETURN_RESULT`时，最终成功/失败 Activity才明确返回 RESULT_OK/RESULT_FIRST_USER与 `EXTRA_INSTALL_RESULT`。已有 Session点击 positive只调用 `setPermissionsResult(true)`并 finish，没有设置 RESULT_OK，Activity默认结果仍是 RESULT_CANCELED；因此不能用启动内层确认页的 Activity result判断是否接受。它的权威终局送往 `dispatchSessionFinished()`快照的 `mRemoteStatusReceiver`；只有在该快照前成功 recommit才会替换首次 IntentSender。调试时先问入口，再选观察通道。

## 13. 直接 APK同意后，系统安装器才创建并写一个新 Session

`InstallInstalling`对 file URI创建 `MODE_FULL_INSTALL` Session，明确设非 instant、referrer/originating URI、originating UID、可选 installer package和 `INSTALL_REASON_USER`。Lite解析尽量提供目标包名、installLocation和预计安装大小；解析或大小计算失败只退回 file length，真正 framework验证仍可给出终局错误。

后台 `InstallingAsyncTask`打开该 Session，以固定 entry名 `PackageInstaller`调用 `openWrite()`，循环复制 private snapshot，更新 staging progress并 `fsync()`。复制阶段的 Cancel按钮会取消 task并 abandon Session；Back却只在取消按钮仍启用时执行普通 `onBackPressed()`，随后 `onDestroy()`虽取消并等待 task，却没有 abandon，可能留下未提交 Session等待系统过期清理。成功返回主线程才创建只限定到自身包的 foreground广播 PendingIntent，调用 `session.commit()`，禁用取消并禁止点窗口外结束。

此时新 Session的实际 installer UID是 AOSP PackageInstaller应用自身的 Binder UID，它持有 `INSTALL_PACKAGES`；用户已经在创建它之前完成确认，所以 framework静默资格通常放行，不再弹相同页面。传入的 installerPackageName用于归因，不会把 Session owner UID变回原来源应用，也不意味着这个应用 UID等于 `Process.SYSTEM_UID`。

commit仍不是成功。PMS结果通过 PendingIntent广播回来；这个 Activity注册的 `InstallSessionCallback.onProgressChanged()`只更新进度，`onFinished()`特意留空。公共 SessionCallback本身仍有“事务已成功或失败完成”的语义，只是此 UI选择从 IntentSender取得更详细的 public/legacy status与错误消息。

## 14. sessionId、installId、status与Activity result是四种账

`sessionId`由 PackageInstallerService分配，标识 stage与安装事务；`installId`由 PackageInstaller应用自己的 `EventResultPersister`递增生成，只把结果广播匹配到某个 UI observer。两者命名空间、owner和生命周期都不同。

public status是 SDK面向调用者的稳定、粗粒度类别，legacy status是 PMS内部 `INSTALL_*`细因及旧接口兼容码；Activity result则是旧 URI UI链在明确请求时的回程契约。三者可以描述同一次安装，却不能互相冒充，更不能用 UI result反推某个 Session的最终回调。

`InstallEventReceiver`在 Manifest中导出，但要求发送方有 `INSTALL_PACKAGES`。收到普通终态时，persister按 installId找在线 observer；没有 observer就把 public status、legacy status和 message写入 `AtomicFile`，Activity重建后重新注册可立即取走。写盘由 AsyncTask延后，返回调用栈不等于已经落盘。

pending user action是特例：它在读取 installId之前试图直接启动 `Intent.EXTRA_INTENT`，不进入最终结果表。AOSP InstallInstalling创建的 Session因 PackageInstaller应用自身 UID持有 `INSTALL_PACKAGES`且未设 FORCE_PROMPT，正常链不会走到这里；更不能把这段兜底当可靠代启动器。r48内层 Intent没有 `FLAG_ACTIVITY_NEW_TASK`，调用处又是 BroadcastReceiver的非 Activity Context，而 targetSdk≥P的 `ContextImpl.startActivity()`会拒绝这种调用。这是一条潜在异常路径，不是 system_server pending模型的保证。

这份持久账主要覆盖 Activity配置变化和短暂进程重建，不是跨 boot完成协议。`install_results.xml`与临时 APK同在 no-backup目录；BOOT_COMPLETED时 TemporaryFileManager遍历并删除 boot之前的文件。构造器遇到损坏 XML也会清空结果再异步写新状态。

最终 UI再做一层适配：请求 `EXTRA_RETURN_RESULT`时，InstallSuccess返回 RESULT_OK+`INSTALL_SUCCEEDED`，InstallFailed返回 RESULT_FIRST_USER+legacy code；否则显示“完成/打开”或失败页。但 r48正常广播链有一个丢参缺口：`launchFinishBasedOnResult()`失败时把 public status用于选择失败分支，却调用只接收 legacy status/message的 `launchFailure()`；后者启动 InstallFailed时没有放回 `EXTRA_STATUS`。所以 InstallFailed会读到默认通用失败，源码中 blocked/conflict/incompatible/invalid/storage的分类视图及 storage管理入口，不能由这条正常链的 public status触发。Activity result只是旧 Intent入口的回程包装，不改变 PMS已经产生、却在 UI跳转处丢失的 PackageInstaller status。

### 练习 9：从广播倒推四种 ID与完成点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'PackageInstaller.SessionParams.MODE_FULL_INSTALL' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'params.setInstallerPackageName(getIntent().getStringExtra(' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'mSessionId = getPackageManager().getPackageInstaller().createSession(params);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F '.openWrite("PackageInstaller", 0, sizeBytes)' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'session.fsync(out);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'broadcastIntent.setPackage(getPackageName());' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'session.commit(pendingIntent.getIntentSender());' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'mInstallId = InstallEventReceiver' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/EventResultPersister.java
grep -n -F 'context.startActivity(intent.getParcelableExtra(Intent.EXTRA_INTENT));' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/EventResultPersister.java
grep -n -F 'Calling startActivity() from outside of an Activity ' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'mResults.put(id, new EventResult(status, legacyStatus, statusMessage));' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/EventResultPersister.java
grep -n -F 'mResultsFile = new AtomicFile(resultFile);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/EventResultPersister.java
grep -n -F 'if (systemBootTime > fileOnBoot.lastModified()) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/TemporaryFileManager.java
grep -n -F 'public void onFinished(int sessionId, boolean success) {' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'getPackageManager().getPackageInstaller().abandonSession(mSessionId);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'int mResultCode = RESULT_CANCELED;' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'getBooleanExtra(Intent.EXTRA_RETURN_RESULT, false)' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallSuccess.java
grep -n -F 'launchFailure(legacyStatus, statusMessage);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'failureIntent.putExtra(PackageInstaller.EXTRA_LEGACY_STATUS, legacyStatus);' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallInstalling.java
grep -n -F 'getIntExtra(PackageInstaller.EXTRA_STATUS,' frameworks/base/packages/PackageInstaller/src/com/android/packageinstaller/InstallFailed.java
```

给定“Activity旋转时终态到达”“进程在异步写盘前退出”“终态已落盘后进程重建”“设备重启”“收到 pending”五个现场，写出 observer、AtomicFile和 UI各自会做什么；再说明 sessionId、installId、public status、legacy code和 Activity result分别回答哪一问。解释 pending直接启动为何可能违反 NEW_TASK规则、Cancel与 Back为何留下不同 Session状态；最后沿三个 launch-failure命中点说明 receiver明明拿到精确 public status，普通失败页为何仍只能落到默认分类。

## 15. multi、staged与重启边界决定了如何诊断卡住

已有 Session确认链还有四个 r48边界不能用正常 happy path覆盖。

第一，staged Session在 `handleInstall()`开头直接交给 StagingManager并向当前 receiver回报 `INSTALL_SUCCEEDED / "Session staged"`，不走普通 `makeSessionActiveLocked()`确认门。这个 success只表示已提交 staged流程，不表示包已安装；pre-reboot验证以 ready或 failed表达，重启恢复与激活后再以 applied或 failed收口，这些状态不会向原 commit receiver补发一次最终安装结果。看到 staged参数时，不应期待本章这只 confirmation Intent。

第二，multi child不能直接 commit；parent commit为每个 child安装 `ChildStatusIntentReceiver`，pending仍携带 child sessionId，全部成功或任一失败才改回 parent sessionId聚合给外部 receiver。parent自身跳过目标包判断，逐个 child调用 `makeSessionActiveLocked()`。若某 child需要确认，它返回 null并发 pending，但循环会把其他非 null child继续放进 `installingChildSessions`并调用 list版 `installStage()`；若所有 child都等待，空列表还会在 PMS触发“`No child sessions found!`”。用户后来对某 child点 true，又直接给该 child投 `MSG_INSTALL`。所以 r48不能假设“一个 child等待用户，整组必然原地不动”，排障必须按 child sessionId核对实际 PMS交接集合。

第三，确认位和回调都不是重启协议。XML schema含 committed/sealed和静态 SessionParams，却不保存 `mPermissionsManuallyAccepted`、`mRemoteStatusReceiver`以及本地验证派生出的 package/signing/resolved-file字段；第一次 commit会先同步落盘 sealed，随后异步才置 `mCommitted=true`，而这一步本身不保证再次写 Session XML，所以磁盘可能是 sealed=true、committed=false。重启读到 false后，recommit可重新验证并派生字段；若碰巧读到 true，`streamValidateAndCommit()`会快速返回，派生字段却仍为空，r48普通 APK后续不能可靠恢复。只有 APEX在 `onAfterSessionRead()`额外重建所需字段。公开 API虽允许 owner再次 commit并给新 receiver，这也只是恢复尝试，不等于旧回调或派生状态已经回来。

第四，UI关闭不等于服务端终态。最可靠的诊断顺序是：先辨入口；再记 Session owner/installer UID与 originating UID/package；检查 Session sealed/committed/relinquished/destroyed和是否 staged/multi child；确认最后 public status是否 pending；查看 AppOp与两类用户限制；最后分别等 PMS status与 Activity result。界面截图只能说明当前交互页，不足以代替这几本账。

## 16. 九类完成证据与第259章接口

下面是九类完成证据，不是一条让所有入口依次走完的统一时间线：

这份清单从“commit正常返回”才开始。child直接提交、非 owner调用、仍有 writer、Secure FRP或 transfer模式不匹配都可能在任何 status callback之前同步抛出；它们不是第1类证据之后的异步失败。

1. `[commit边界]` `Session.commit()`正常返回首先只证明没有同步抛异常；普通 single Session在 `markAsSealed()`成功后才完成 seal并投递异步验证。multi child sealing失败时该 void方法也可直接返回而不调用 `dispatchStreamValidateAndCommit()`，所以调用方不能仅凭返回值证明已调度，更不能证明本地验证完成。
2. `[任一普通 Session]` `streamValidateAndCommit()`返回 true：内容验证完成、`mCommitted=true`，不代表 PMS已接管；内部 file链是在第7类证据之后才到这里。
3. `[需要确认的已有 Session]` 收到 `STATUS_PENDING_USER_ACTION`：需要交互且外层 receiver存活，不是失败或 UI已启动。
4. `[确认 UI]` InstallStart通过来源声明与身份整理：能进入内部页面，不代表 AppOp或用户限制放行。
5. `[可选 Settings往返]` 来源 AppOp写成 ALLOWED：该 uid+package可请求，不代表当前 APK已获同意，而且回程存在不重查窗口。
6. `[确认 UI]` positive：已有 Session只是投递第二轮；内部 `file:`开始创建/写系统 Session；`package:`准备同步调用已有包恢复 API。
7. `[再入边界]` 已有 Session收到第二轮消息，或内部 `file:`路径执行新 Session的 `commit()`：都不代表 PMS已接手；`package:`跳过此类证据。
8. `[Session两支]` `ActiveInstallSession`真正传给 `mPm.installStage()`：PMS后半程已交接，不代表 verifier与事务成功；`package:`与 staged跳过这一形态。
9. `[non-staged Session终局]` 当前 commit receiver收到 `STATUS_SUCCESS`才是 PackageInstaller操作终局。只有内部 file链再把它翻译成 InstallSuccess或按请求返回 Activity result；已有 Session由自己的 receiver消费者处置。`package:`因忽略 int失败码甚至可能只有 UI假成功；staged的同名 success只表示“Session staged”，仍要另看 ready/applied/failed。

因此，已有 Session主线大致是 1→2→3→4→〔5〕→6→7→8→9；content转内部 file的路线是 4→〔5〕→6→7→2→8→9；`package:`只走 4→〔5〕→6后进入同步 API与可能失真的 UI结果。方括号中的第5类本来就是条件分支。

最简心智模型是：InstallStart守住外部入口和来源身份，PackageInstallerActivity组合用户限制、来源 AppOp与本次点击；在已有 Session路线，PackageInstallerSession保存本次确认并把工作交给 PMS，在内部 file路线则由系统安装器另建特权 Session。结果 receiver只负责把异步终局带回消费者。任何一层的“允许”都不能替后续层签字。

第259章从第8类证据继续：PMS接手后，required verifier、sufficient verifier、integrity verification、timeout与最终放行怎样共同决定安装能否进入真正的 prepare/scan/reconcile/commit。
