# 259 Android APK安装Verifier、PACKAGE_NEEDS_VERIFICATION广播、超时与最终放行链

本章源码基线是 Android 11 / API 30 / `android-11.0.0_r48`。第258章的终点只是 `ActiveInstallSession`交给 PMS；此时用户已经同意、来源 AppOp也可能已经放行，但 APK仍未进入最终安装流程。PMS还会并行启动 ordinary package verification与 App Integrity，并可选等待 rollback准备。三道完成门齐备只代表可以进入 `handleReturnCode()`；普通非 dry-run请求还须保持成功返回码，才会调用 `copyApk()`。

这一段最危险的误判，是把“Verifier说允许”“State显示完成”“InstallParams开始继续”“包最终安装成功”当成同一件事。r48恰好存在几处让这些事实分离的实现边界：required verifier缺失时主门默认已完成而 pending State不完整；副用户 sufficient verifier用 appId记账却以完整 Binder UID回调；普通超时在 Handler里取调用 UID；延期又不校验发起者是不是本次参与者。它们既是排障重点，也提醒我们不能只靠理想状态图读源码。

除专门说明的分支外，本章主线限定为 single-package、`SessionParams.isStaged=false`、非 APEX的普通 APK安装。这里的 Session staged事务状态与 `OriginInfo.staged`不是同一概念：普通 PackageInstaller Session交给 PMS时也可来自已写好的 stage目录，后者会让 `copyApk()`绑定现有路径而跳过物理复制。用户确认沿用第258章，APK签名与扫描事务由后续章节展开；域名 Intent Filter验证只在边界处对照，不混入本章状态机。

## 1. 三道完成门、两类判定和一个安装结果

`InstallParams.handleStartCopy()`完成位置预判并创建 `InstallArgs`后，先把三个完成位置为 true：

- `mVerificationCompleted`：ordinary verifier流程是否可以离开等待态；
- `mIntegrityVerificationCompleted`：App Integrity流程是否可以离开等待态；
- `mEnableRollbackCompleted`：请求 rollback时，Rollback Manager是否已回应或超时。

随后代码按实际请求把需要等待的位改回 false。`handleReturnCode()`只有在三位全真时才行动：dry run直接通知 observer并返回；其余请求若 `mRet`仍是 `INSTALL_SUCCEEDED`，才执行 `mArgs.copyApk()`，再无论成功或失败都把结果交给 `processPendingInstall()`。所以这些 boolean回答的是“异步门是否结束”，并不各自保存“是否通过”；拒绝结果写在另一本 `mRet`账上，而且 `setReturnCode()`只允许第一次失败覆盖原成功值。

ordinary verification内部又有 required与零个或多个 sufficient verifier；App Integrity则在 system_server中解析候选、组装元数据并求值。两者共享 `verificationId`和 `PackageVerificationState`，却不共享结果码或投票规则。rollback只是完成屏障，在 r48中启用失败并不把 `mRet`改成失败。

这三门也不替代其他检查：用户确认回答“是否接受本次安装”，APK签名回答“内容与升级身份是否合法”，域名验证回答“是否应默认处理 Web域名”。`ACTION_PACKAGE_VERIFIED`只是 ordinary相关路径使用的通知通道，携带值要结合正常响应、timeout或 Incremental事后分支解释，不能一概当作 State的聚合裁决。任何一个“ALLOW”都不是最终安装成功。

## 2. verificationId建起一张 State账，InstallParams另记主流水线账

当早期返回码仍成功时，PMS先递增 `mPendingVerificationToken`取得进程内 `verificationId`。只有 `!origin.existing`才创建 `PackageVerificationState`、放入 `mPendingVerification`，然后按源码顺序先发 Integrity请求、再处理 ordinary请求；move等 existing origin不会重新建立这次 APK验证事务。注意 token在 `origin.existing`判断之前已经消耗，因此“ID递增”本身不证明 State入表。

State保存 required UID、sufficient UID集合、两组 ordinary complete/passed位、一次延期位、Integrity complete位以及所属 `InstallParams`。它有三个不同问题：

| 查询 | 回答什么 | 不回答什么 |
|---|---|---|
| `isVerificationComplete()` | ordinary投票是否达到结束条件 | Integrity是否结束、安装是否继续 |
| `isInstallAllowed()` | ordinary聚合结果是否允许 | 其他门与后续安装是否成功 |
| `areAllVerificationsComplete()` | ordinary与Integrity的 State完成位是否都真 | rollback、`mRet`、复制与提交结果 |

`areAllVerificationsComplete()`主要决定何时删除 pending表项；真正控制 `handleReturnCode()`的是 `InstallParams`三位。正常路径两本账会汇合，异常边界却可以一边完成、一边残留。看到 pending项未删除时，不能直接断言复制一定没开始；看到 `mVerificationCompleted=true`也不能反推 State已完整聚合。

### 练习 1：推演 verification事务的创建条件

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mVerificationCompleted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIntegrityVerificationCompleted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mEnableRollbackCompleted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int verificationId = mPendingVerificationToken++;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!origin.existing) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPendingVerification.append(verificationId, verificationState);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'sendIntegrityVerificationRequest(verificationId, pkgLite, verificationState);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ret = sendPackageVerificationRequest(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (verificationState.areAllVerificationsComplete()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

分别推演 `mRet`成功或失败、`origin.existing`为真或假时：token是否消耗、State是否入表、两条验证是否启动、State是否立即移除。最后解释为什么 existing路径可能消耗 token，却没有任何可供回调命中的 pending State。

## 3. required verifier先在 system user选包名，再在目标用户解析实例

PMS初始化时用 `ACTION_PACKAGE_NEEDS_VERIFICATION`和 APK MIME查询 system user，并带 `MATCH_SYSTEM_ONLY`及 Direct Boot aware/unaware标志。匹配数的处理不是普通的“取第一个”：恰好一个时保存其 packageName，零个时记录错误并返回 null，多于一个直接抛 `RuntimeException`。方法名 `getRequiredButNotReallyRequiredVerifierLPr()`揭示了一个产品边界：没有 required verifier不阻止 PMS启动。

开机阶段只冻结包名。每次安装时，`verifierUser`通常取安装目标用户，`INSTALL_ALL_USERS`才折算为 system user；required UID用该用户上的 `getPackageUid()`求得。PMS又以当前用户状态查询 live receivers，再按保存的包名找 Component。因此下面三层不能合并：

1. system user开机时存在唯一 system receiver；
2. required包在本次 verifier user上有可用 UID；
3. 当前 action查询能找到该包的可用 Component。

第1层成立不保证后两层成立。`matchComponentForVerifier()`找不到时返回 null，而调用处仍执行 `verification.setComponent(requiredVerifierComponent)`；传 null会清掉显式 Component，使后续有序广播按剩余 Intent条件解析，而不是“必然显式发给 required包”。开机查询也只要求 system receiver匹配 action，不预检其应用持有 `PACKAGE_VERIFICATION_AGENT`；真正发送有序广播时才用 receiver permission过滤。唯一 Component即使找到，缺少该权限也收不到请求。与此同时，只要 `mRequiredVerifierPackage`非 null，代码仍安排 timeout并把 `mVerificationCompleted`置为 false。对副用户禁用、未安装、权限或 direct-boot状态差异的现场，必须同时核对 packageName、full UID和 Component。

required包本身为 null是另一种分支：required UID被记成 -1，但 ordinary enabled路径里没有 required广播、没有 ordinary timeout，也不会把 `mVerificationCompleted`改为 false。安装主流水线因而不会等待 ordinary票；State的 required complete却仍为 false。这不是“安装必然卡住”，而是两本账从一开始就分叉，第13节会推到终局。

## 4. ordinary是否发送由来源、策略、包状态与数据格式共同决定

r48的编译默认 `DEFAULT_VERIFY_ENABLE=true`，但 `isVerificationEnabled()`不是常量透传。ADB路径按以下优先级决策：

1. 目标用户受 `ENSURE_VERIFY_APPS`约束时强制开启；
2. 带 `INSTALL_DISABLE_VERIFICATION`时，新装仍开启；已有包更新只有待装 APK为 debuggable才跳过，release更新仍开启；
3. 其余情况读取 `PACKAGE_VERIFIER_INCLUDE_ADB`，默认值1。

非 ADB路径中，保留下来的 `INSTALL_DISABLE_VERIFICATION`直接关闭 ordinary验证；但调用者是否有权设置、服务端是否保留这个内部 flag属于更早的权限净化问题，不能把它当成普通应用可任意使用的开关。非 ADB Instant App还有一个窄旁路：instant installer的包名必须与 required verifier包相同，且 AppOps `checkPackage(installerUid, packageName)`确认 UID归属，才返回 false。

即便 `isVerificationEnabled()`返回 true，发送条件还要求“不是同时满足 Incremental与 V4签名”。只有这对组合跳过安装前 ordinary verifier；传统 stage、普通 streaming、Incremental但不是 V4、V4但不是 Incremental都不走这条特例。旁路分支调用 `setVerifierResponse(requiredUid, VERIFICATION_ALLOW)`，让 State ordinary侧完成；Integrity仍独立启动。

`origin.existing`位于更外层：它不创建 State，也不调用两条验证。因而诊断表至少需要 `origin.existing`、ADB、disable flag、fresh/update、debuggable、用户限制、instant身份、DataLoader type和签名方案九列，单看全局开关不够。

### 练习 2：手算 ordinary verification旁路决策树

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final boolean DEFAULT_VERIFY_ENABLE = true;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((installFlags & PackageManager.INSTALL_FROM_ADB) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (isUserRestricted(userId, UserManager.ENSURE_VERIFY_APPS)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mSettings.mPackages.get(pkgInfoLite.packageName) == null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return !pkgInfoLite.debuggable;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Global.PACKAGE_VERIFIER_INCLUDE_ADB, 1) != 0;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((installFlags & PackageManager.INSTALL_DISABLE_VERIFICATION) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((installFlags & PackageManager.INSTALL_INSTANT_APP) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& mInstantAppInstallerActivity.packageName.equals(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '.checkPackage(installerUid, mRequiredVerifierPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '(mArgs.signingDetails.signatureSchemeVersion == SIGNING_BLOCK_V4);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '(mArgs.mDataLoaderType == DataLoaderType.INCREMENTAL);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& (!isIncrementalInstall || !isV4Signed)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

填写 ADB新装、debug更新、release更新、企业强制、非 ADB disable、可信 Instant installer、Incremental+V4七种输入的结果。每例分开写 `isVerificationEnabled()`返回值和最终是否发送 ordinary请求；后者还多一层 DataLoader与签名方案判断。

## 5. sufficient先发普通广播，required有序广播结束后才启动共同超时

ordinary请求的 data是内部 origin路径的 file URI，type为 APK MIME，并带 foreground与 read URI grant。extras包括 verificationId、install flags、packageName、短/长 versionCode、initiating installer包名，以及存在时的 originating URI、referrer、originating UID和 installer UID。这些字段给 Verifier提供上下文，却不因放进系统广播就自动变成同等可信；例如 App Integrity仍会校验 installer package是否属于 installer UID。

PMS先查询本次 verifier user中的 live receivers，匹配并逐个发送 sufficient显式普通广播；之后才处理 required。required Component若非 null，Intent显式指向它，并用要求接收者持有 `PACKAGE_VERIFICATION_AGENT`的 `sendOrderedBroadcastAsUser()`发送。最终 BroadcastReceiver返回后，PMS才排入 `CHECK_PENDING_VERIFICATION + getVerificationTimeout()`。

因此“10秒 timeout”不是从 `sendOrderedBroadcastAsUser()`调用瞬间开始，也不是每个 sufficient各有一只计时器。sufficient共享 required有序广播之后排出的同一个 ordinary timeout。required `onReceive()`执行时间位于计时起点之前，但仍受广播自身的调度约束；正确持有 `goAsync()`返回的 PendingResult时，有序链要等 `finish()`才继续。只有 Verifier先返回或先 `finish()`、再把未完成工作独立留在后台时，那部分工作才与后面的计时窗口竞争。

PMS还按 ordinary timeout时长给 verifier包临时 power-save whitelist，并把相同时长放进 `BroadcastOptions`。这是有限运行机会，不是保证响应一定到达。更要留意 null Component边界：此时 required Intent不再显式，虽然广播仍要求 signature/privileged级 agent权限，但能收到者与 State中 required UID未必相同；最终 timeout照样会排入。

只有进入 `mRequiredVerifierPackage != null`分支，代码才开始 ordinary trace并置 `mVerificationCompleted=false`。所以“已构造 verification Intent”或“已发 sufficient广播”都不等价于主流水线正在等待 ordinary完成。

### 练习 3：推演 required与 sufficient verifier的入场身份

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private @Nullable String getRequiredButNotReallyRequiredVerifierLPr() {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'MATCH_SYSTEM_ONLY | MATCH_DIRECT_BOOT_AWARE | MATCH_DIRECT_BOOT_UNAWARE,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (matches.size() == 1) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if (matches.size() == 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'throw new RuntimeException("There must be exactly one verifier; found " + matches);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final List<ResolveInfo> receivers = queryIntentReceiversInternal(verification,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ComponentName requiredVerifierComponent = matchComponentForVerifier(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'verification.setComponent(requiredVerifierComponent);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ComponentName comp = matchComponentForVerifier(verifierInfo.packageName,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'pkg.getSigningDetails().signatures.length != 1' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!Arrays.equals(actualPublicKey, expectedPublicKey)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'verificationState.addSufficientVerifier(verifierUid);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mContext.sendBroadcastAsUser(sufficientIntent, verifierUser,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mContext.sendOrderedBroadcastAsUser(verification, verifierUser,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

先推演开机匹配数为0、1、多个的结果，再推演安装时 required UID存在与否、Component找到与否，以及 sufficient的 Component、单签名、公钥三项认证。明确写出 required Component为 null时 Intent显式性怎样变化，不能用开机唯一匹配替代当前用户状态。

## 6. sufficient的信任根来自目标 Manifest，但 r48的副用户 UID记账不对称

`PackageInfoLite.verifiers`来自待安装 APK Manifest中的 `<package-verifier>`，每项给出 verifier包名与期望 public key。它不是系统全局配置，也不表示其中每个候选都能参与。PMS依次要求：

- 当前 verifier user的 live receiver查询里存在同包名 Component；
- 已安装包在全局 `mPackages`中存在；
- 当前签名数组恰好一个；
- 该签名公钥编码与 Manifest声明的公钥编码完全相同。

通过者被加入 sufficient Component列表与 UID集合。Manifest完全没有声明 verifier时，`matchVerifiers()`返回 null，语义是“没有额外要求”；声明非空却一个有效匹配都没有时返回空列表，PMS立即把本次 `ret`改为 `INSTALL_FAILED_VERIFICATION_FAILURE`。这两个空值不能混为一谈。

sufficient广播是显式普通广播，发送处没有 receiver permission；包名、Component与公钥匹配提供的是“目标 APK指定了这份已安装代码”这一信任链。回调 `verifyPendingInstall()`仍要求 `PACKAGE_VERIFICATION_AGENT`。因此一个公钥匹配但没有该高权限的 receiver可以收到广播，却不能提交有效回调，会成为等待集合中的沉默成员。Intent虽带 `FLAG_GRANT_READ_URI_PERMISSION`，data却是 `file:`而非 `content:`；Uri grant机制不会为 file URI创建授权，receiver能否真正打开路径仍由文件 DAC、SELinux与其进程权限决定。

r48还有一个副用户错配。required UID用 `getPackageUid(..., verifierUserId)`得到该用户的 full UID；sufficient的 `getUidForVerifier()`却返回 `pkg.getUid()`。`PackageImpl`源码明确说明这个字段是 appId，只有 system user时才等于 UID。广播发给 verifier user，进程回调携带的是 `UserHandle.getUid(userId, appId)`形式的完整 Binder UID，而 State按 int精确匹配。于是非 system user上的合法 sufficient响应通常找不到其 appId账项。

这个错配不会总以同一种表象结束：同一副用户中经该 helper登记的其他普通 app verifier也会遇到同类错配，不能假设“换一个 sufficient”自然收口；system-user路径或运行时 Binder UID恰与记录值相等的参与者才可正常命中。required若使用有效的 `ALLOW_WITHOUT_SUFFICIENT`可以清集合，普通 timeout也可能让 InstallParams继续；否则 State可能残留，甚至与延期缺口组合成永久等待。排障时必须同时打印 verifier user、记录值是 appId还是 full UID、回调 Binder UID，不能只比较包名。

UID还是投票键，不是 Component键。若 required与某个 sufficient共享同一 UID，`setVerifierResponse()`先命中 required分支，sufficient项不会被删除；若两个 sufficient共享 UID，`SparseBooleanArray.put()`会把它们折叠成一个键，首个响应就消费共同项。Manifest列了几个 Component不等于 State一定保存几张独立票，shared UID产品必须单独推演。

## 7. required必须允许，sufficient只需一个允许，但完成与通过仍是两步

State对投票的核心规则如下：

| 到达状态 | ordinary complete | ordinary allowed |
|---|---:|---:|
| required未响应 | 否 | 不应查询 |
| required ALLOW，且无 sufficient | 是 | 是 |
| required REJECT，且无 sufficient | 是 | 否 |
| required ALLOW，至少一个 sufficient ALLOW | 是 | 是 |
| required REJECT，至少一个 sufficient ALLOW | 是 | 否 |
| required ALLOW，所有 sufficient都已非 ALLOW响应 | 是 | 否 |
| required已响应，仍有 sufficient等待且尚无 ALLOW | 否 | 不应查询 |

required响应总会把 required complete置真；code为 `ALLOW`时 passed为真，`ALLOW_WITHOUT_SUFFICIENT`先清 UID集合再按 ALLOW处理，其他任意 code都按失败。sufficient只有精确 UID成员才能消费一票：code恰为 `ALLOW`时立刻把 sufficient complete/passed置真；其他 code移除该 UID，直到集合为空才 complete，passed仍假。因此 sufficient是“任一允许即可”。在正常回调聚合路径中，只要最终执行 `isInstallAllowed()`，required拒绝就不可放行；但它不会在还有 sufficient等待时让 `isVerificationComplete()`立即短路，而第9节的 timeout正会利用这段窗口绕过聚合判断。

`ALLOW_WITHOUT_SUFFICIENT`还有顺序依赖。若它到达时仍有 sufficient UID未响应，clear后 sufficient complete仍是假，`isInstallAllowed()`会跳过 sufficient passed检查，结果允许。若所有 sufficient早已拒绝，代码已经把 sufficient complete置真、passed留假；后到的 clear不会重置这两个 boolean，最终仍拒绝。它的准确语义不是无条件覆盖全部历史，而是清除尚存的 sufficient UID集合。

这也解释了为何必须先调用 `isVerificationComplete()`再问 `isInstallAllowed()`：后一方法不会自行验证所有票是否到齐。Handler正常回调路径遵守这个顺序，但第9节的 timeout分支没有用同一套聚合判断。

### 练习 4：手算 required与 sufficient投票真值表

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (uid == mRequiredVerifierUid) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'case PackageManager.VERIFICATION_ALLOW_WITHOUT_SUFFICIENT:' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'mSufficientVerifierUids.clear();' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'if (code == PackageManager.VERIFICATION_ALLOW) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'mSufficientVerifierUids.delete(uid);' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'if (mSufficientVerifierUids.size() == 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'if (!mRequiredVerificationComplete) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'if (!mRequiredVerificationPassed) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'return mSufficientVerificationPassed;' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
```

用两个 sufficient UID依次演算：都等待、先拒一个、先允一个、全部拒绝、required拒绝，以及 `ALLOW_WITHOUT_SUFFICIENT`分别在“仍有人等待”和“已经全部拒绝”之后到达。每一步记录 UID集合、四个 complete/passed位与最终 allowed，验证最后两种顺序为何不同。

## 8. Binder权限只准入 agent，caller UID才决定它能投哪一票

`verifyPendingInstall(id, code)`先强制调用者持有 `PACKAGE_VERIFICATION_AGENT`；该权限是 `signature|privileged`。PMS随后把 `Binder.getCallingUid()`与 code封装进消息，切到 PMS Handler再查 pending State。权限解决“是不是受信任 agent”，State成员检查解决“是不是本次 required或 sufficient参与者”，两者缺一不可。持有权限但 UID不在本次账本，会让 `setVerifierResponse()`返回 false，不能替别人改票。

Handler没有检查这个 boolean返回值。若 ordinary本来未完成，非成员消息通常到此停住；若 ordinary早已完成但 Integrity尚未结束，State还留在 pending表，后面的 `isVerificationComplete()`仍为真。此时非成员可触发重复收口：若 State允许，PMS会把非成员提供的 `response.code`原样塞进新的 `ACTION_PACKAGE_VERIFIED`，即使该 code与真实票面不一致；还会重复结束 trace并调用已经幂等的完成方法。它不能据此把 State从拒绝改成允许，但证明“无效投票”不等于“整条 Handler消息无副作用”。

unknown verificationId会记录警告并忽略。已从 pending表移除的事务也是 unknown；因此晚到消息不能重开已经继续的安装。State仍留在表中时，重复 required响应却能再次覆盖 required passed：第一次 ALLOW后若 Integrity尚未完成，第二次 REJECT可以把 `mRet`变成失败；第一次失败已经经 `setReturnCode()`写入后，后来的 ALLOW不能把它改回成功。这是“State可变、安装失败单向粘滞”的组合，不应把回调理解为天然 exactly-once。

普通回调只有在 `isVerificationComplete()`为真时才计算 `isInstallAllowed()`。允许时发送 `ACTION_PACKAGE_VERIFIED`；拒绝时把 `mRet`写成 `INSTALL_FAILED_VERIFICATION_FAILURE`。只有 ordinary与Integrity的 State完成位都真才删除 pending项，而 `handleVerificationFinished()`只把 InstallParams ordinary完成位置真。于是 ordinary已结束、Integrity仍运行期间，State保留是正常现象。

`ACTION_PACKAGE_VERIFIED`带 verificationId、ordinary result、候选 URI和 DataLoader type，并要求接收者持有 agent权限。它是 ordinary阶段通知，不是 `PackageInstaller.STATUS_SUCCESS`，也不代表 `copyApk()`、scan、reconcile或 commit已经发生。

### 练习 5：从 Binder回调推演 pending State清理

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'Only package verification agents can verify applications' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'verificationCode, Binder.getCallingUid());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mSufficientVerifierUids.get(uid)) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'Slog.w(TAG, "Verification with id " + verificationId' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'state.setVerifierResponse(response.callerUid, response.code);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (state.isVerificationComplete()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (state.isInstallAllowed()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'params.setReturnCode(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (state.areAllVerificationsComplete()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPendingVerification.remove(verificationId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'params.handleVerificationFinished();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

比较合法 required、合法 sufficient、持权限但 UID不在账本、unknown verificationId四种回调。再推演 required在 ordinary已完成但 Integrity未结束时依次发送 ALLOW→REJECT和 REJECT→ALLOW，说明 State与粘滞 `mRet`为何产生不同结果。

## 9. ordinary timeout能结束主门，r48的 UID与延期缺口却会改变语义

ordinary timeout从全局设置读取，但以10秒为下限；默认响应是 `VERIFICATION_ALLOW`。目标用户受 `ENSURE_VERIFY_APPS`约束时，默认响应改为 REJECT。超时允许分支试图用 `ALLOW_WITHOUT_SUFFICIENT`完成 required并清掉未决 sufficient，随后广播 ALLOW；拒绝分支广播 REJECT并把 `mRet`写成验证失败。

r48这里有一个关键身份缺口：`CHECK_PENDING_VERIFICATION`运行在 PMS Handler，代码却用该时刻的 `Binder.getCallingUid()`调用 `state.setVerifierResponse()`。它不再处于原 Verifier的 Binder事务里，通常得到 system_server的 system UID，而非 State记录的 required UID。代码也不检查这个调用是否真正改动 State，仍可能删除“若已全部完成”的项、结束 trace，并无条件调用 `params.handleVerificationFinished()`。

后果要按状态分支推：

- system UID不等于 required UID时，默认 ALLOW可能没有改动 State，但 InstallParams ordinary门仍结束，安装可继续，pending项残留；
- required曾 REJECT但因 sufficient仍等待而尚未 complete时，拒绝还没有写入 `mRet`；此时默认 ALLOW即使 UID不命中，也会直接结束主门而不再查 `isInstallAllowed()`，安装可能继续；
- required本身恰为 system UID时，timeout的 `ALLOW_WITHOUT_SUFFICIENT`能覆盖 required passed并清未决集合；若 sufficient已全部拒绝，State的 `isInstallAllowed()`仍会因遗留 complete/passed位返回 false，但 timeout路径根本不查询它，主流水线仍可能继续；
- 默认 REJECT分支先把 `mRet`写失败，所以即使 UID不命中，安装也会以验证失败收口。

这些是 r48实现行为，不是应推广到所有版本的协议语义。源码还定义了 `INSTALL_FAILED_VERIFICATION_TIMEOUT`，但这条 Handler超时拒绝实际写的是 `INSTALL_FAILED_VERIFICATION_FAILURE`；两者对外都会映射为 `STATUS_FAILURE_ABORTED`。

`extendVerificationTimeout()`再引入三项边界。它把 delay夹在0到1小时，只允许每个 State消费一次全局延期位；但只验 agent权限，不验 caller UID是不是本次参与者。它还在校验 `verificationCodeAtTimeout`之前就构造了含原 code与 caller UID的 response，后面对局部变量改成 REJECT不影响已封装对象。于是 required agent传入 API文档未允许的 code 2，延迟消息仍会按 `ALLOW_WITHOUT_SUFFICIENT`处理。

更严重的是，任意持权但非参与者可先抢到唯一延期：原始 CHECK到达时看到 `timeoutExtended()`便退出，延迟 `PACKAGE_VERIFIED`又因 caller UID不在 State中不能完成投票，代码不再安排后备 timeout，`mVerificationCompleted`可能永久为 false。任何延期结果若仍留下另一类票未完成，都有相同等待风险，例如 sufficient提交延期结果而 required沉默，或 required延迟 ALLOW而 sufficient沉默。排障必须记录“谁置了 extended”“延迟消息的 caller UID与 raw code”，不能只找原10秒消息。

延期只重排一条 delayed `PACKAGE_VERIFIED`，不会续期发送 ordinary请求时设置的 power-save whitelist或 `BroadcastOptions`；初始临时白名单仍按 ordinary timeout计算，默认10秒。因而“结果最多延后一小时”不等于 Verifier同时获得一小时后台运行豁免。

### 练习 6：推演 ordinary timeout与一次延期

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final long DEFAULT_VERIFICATION_TIMEOUT = 10 * 1000;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private static final int DEFAULT_VERIFICATION_RESPONSE = PackageManager.VERIFICATION_ALLOW;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return Math.max(timeout, DEFAULT_VERIFICATION_TIMEOUT);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendMessageDelayed(msg, getVerificationTimeout());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& !state.timeoutExtended()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'state.setVerifierResponse(Binder.getCallingUid(),' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mUserManager.hasUserRestriction(UserManager.ENSURE_VERIFY_APPS, user.getIdentifier())) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return PackageManager.VERIFICATION_REJECT;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'verificationCodeAtTimeout, Binder.getCallingUid());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (millisecondsToDelay > PackageManager.MAXIMUM_VERIFICATION_TIMEOUT) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((verificationCodeAtTimeout != PackageManager.VERIFICATION_ALLOW)' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((state != null) && !state.timeoutExtended()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendMessageDelayed(msg, millisecondsToDelay);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private boolean mExtendedTimeout;' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'params.handleVerificationFinished();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

画出 required有序广播返回、原始10秒消息、第一次延期、延迟响应四个时点。分别代入 required caller、合法 sufficient caller、持权但非成员 caller，并让 raw code取1、-1、2，判断 State、InstallParams和后备计时器的终态。

## 10. App Integrity在独立 Handler求值，timeout与临时白名单却不等长

PMS在 ordinary之前调用 `sendIntegrityVerificationRequest()`。r48默认开启且没有用户可配置开关；若编译配置关闭，State直接把 Integrity标为 complete，InstallParams的完成位因初始就是 true而无需再改。开启时，PMS构造 `ACTION_PACKAGE_NEEDS_INTEGRITY_VERIFICATION`，限定到 `android`包、system user与 registered receiver，带 foreground/read grant和与 ordinary相近的包、版本、安装器 extras。

这是有序广播，但没有 receiver permission；目标包限定为平台 `android`。`AppIntegrityManagerServiceImpl`在自己的 `AppIntegrityManagerServiceHandler`上注册动态 Receiver，`onReceive()`再把实际 `handleIntegrityVerification()`工作 post到同一 Handler。ordered链的最终 Receiver返回后，PMS才排至少30秒的 `CHECK_PENDING_INTEGRITY_VERIFICATION`。

r48有一处时长不对称：Integrity的临时 power whitelist用的是 ordinary `getVerificationTimeout()`，默认10秒；真正 Integrity timeout用 `getIntegrityVerificationTimeout()`，默认30秒。对 system_server内服务而言这未必直接制造休眠失败，但源码证据不能写成“两者都按30秒”。

超时默认不可配置，方法返回 ordinary命名空间的 `PackageManager.VERIFICATION_REJECT=-1`，而 internal Integrity REJECT常量是0。timeout分支只判断结果是否等于 internal ALLOW=1，State又根本不保存 code、只置 complete，所以 -1仍产生拒绝后果：`mRet`写验证失败，Integrity主门结束。晚到结果若 State已删会被忽略；若 ordinary还没结束而 State仍在，ALLOW也不能撤销已经粘滞的失败。

正常结果通过 `PackageManagerInternal.setIntegrityVerificationResult()`回到 PMS Handler。State仍只记录“完成”，真正的允许或拒绝由 Handler在写 State前后直接解释：ALLOW只记录日志，任何非 ALLOW把 `mRet`改为验证失败。不要从 `mIntegrityVerificationComplete=true`反推规则结果为允许。

### 练习 7：推演 Integrity广播、timeout与结果记账

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final long DEFAULT_INTEGRITY_VERIFICATION_TIMEOUT = 30 * 1000;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'new Intent(Intent.ACTION_PACKAGE_NEEDS_INTEGRITY_VERIFICATION);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'integrityVerification.setPackage("android");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final long idleDuration = getVerificationTimeout();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.obtainMessage(CHECK_PENDING_INTEGRITY_VERIFICATION);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendMessageDelayed(msg, getIntegrityVerificationTimeout());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return Math.max(timeout, DEFAULT_INTEGRITY_VERIFICATION_TIMEOUT);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private int getDefaultIntegrityVerificationResponse() {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public static final int VERIFICATION_REJECT = -1;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int INTEGRITY_VERIFICATION_REJECT = 0;' frameworks/base/services/core/java/android/content/pm/PackageManagerInternal.java
grep -n -F 'if (getDefaultIntegrityVerificationResponse()' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '== PackageManagerInternal.INTEGRITY_VERIFICATION_ALLOW) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case INTEGRITY_VERIFICATION_COMPLETE: {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'state.setIntegrityVerificationResult(response);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'void setIntegrityVerificationResult(int code) {' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'mIntegrityVerificationComplete = true;' frameworks/base/services/core/java/com/android/server/pm/PackageVerificationState.java
grep -n -F 'params.handleIntegrityVerificationFinished();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

比较实际 ALLOW、实际 REJECT、30秒 timeout三种路径，写出 State complete、`mRet`和 InstallParams完成位。再解释 -1与0虽不同，为什么本版本仍得到相同拒绝结果，以及10秒 whitelist与30秒 timeout为何不能互相代称。

## 11. App Integrity先重建安装器身份，再把 APK压成规则元数据

Integrity服务不直接信任 installer package extra。extra为 null时把来源记为字面量 `adb`；存在包名时要求 installer UID非负，且 `getPackagesForUid(uid)`确实包含该包，否则规范为未知安装器空字符串。若可信包名是 `com.android.packageinstaller`或 `com.google.android.packageinstaller`，服务把它视作中转者，转而读取 originating UID关联的包；shared UID有多个包时直接取列表第一个，所以来源归因并非一一映射。

副用户上的核身与证书取值又不完全对称：包名成员检查使用传入的完整 installer/originating UID；随后 `getInstallerCertificateFingerprint()`却在 system_server的 system-user Context上调用无 user参数的 `getPackageInfo()`。若来源包只在副用户安装，这一步可能得到 `NameNotFoundException`并返回空证书列表，于是规则输入仍有 installer name，却没有 installer certificates。

规则提供者充当本次 installer时还有默认旁路，不要求目标包就是 provider自身：若全局 `INTEGRITY_CHECK_INCLUDES_RULE_PROVIDER`不是1，且安装器被识别为允许的 system rule provider，服务直接回 ALLOW。纯 AOSP的 `config_integrityRuleProviderPackages`为空，产品通常通过 overlay提供名单。r48的 `isRuleProvider()`执行 `ruleProvider.matches(installerPackageName)`：它把已核准 installer包名当作正则，去匹配配置中的 provider字符串，而不是做 `equals()`。普通包名下通常表现得像相等比较，但源码语义仍应按这个参数方向描述。

未旁路时，服务重新解析候选路径，取得 active signing certificates与 Manifest metadata。它调用 `getSigningDetails(..., skipVerify=true)`，注释理由是签名已经在 PackageManager其他阶段验证，避免对大 APK重复耗时；这里提取证书供规则匹配，不承担完整 APK签名验真。解析返回 null时直接 ALLOW，而路径为空、scheme非 file、不可读等 `IllegalArgumentException`会落入拒绝分支，两类“读不到”后果不同。

元数据包括规范化目标包名、active signer证书 SHA-256、long versionCode、规范化安装器名及证书、目标当前是否为 system app、Manifest的 allowed-installers映射，以及 Source Stamp的 present/verified/trusted与证书摘要。目录路径会把 `Files.list()`列出的全部条目路径交给 `SourceStampVerifier`；verified便直接设 trusted。

包名规范化也有版本细节。注释说超过32 bytes就散列，实际条件却是 Java `packageName.length() <= 32`，即 UTF-16 code unit数量；只有超过该阈值才对 UTF-8 bytes做 SHA-256并输出 hex。对非 ASCII输入，不能按注释的 byte长度复现规则 key。

## 12. FORCE_ALLOW压过 DENY，无规则与内部故障多走 fail-open

`RuleEvaluationEngine`先从 `IntegrityFileManager`读规则。规则文件未初始化时返回空列表，读取异常也返回空列表；两者随后都会得到默认 ALLOW。`getRuleEvaluationEngine()`名义上提供 singleton，但 r48在静态字段为 null时直接返回新实例而没有回写字段，这不改变单次 service实例的求值，却说明不要凭方法名假设跨调用缓存已经建立。

`RuleEvaluator`先筛出所有 formula匹配当前 `AppInstallMetadata`的规则，再按固定优先级求值：

1. 只要有匹配的 `FORCE_ALLOW`，立即 ALLOW并记录这些规则；
2. 否则只要有匹配的 `DENY`，返回 DENY并记录拒绝规则；
3. 否则默认 ALLOW。

因此 FORCE_ALLOW与 DENY同时命中时不是冲突或“最后一条赢”，而是 FORCE_ALLOW明确优先。规则公式内部要求的 DNF与匹配语义由 `IntegrityFormula.matches()`实现；本层只聚合每条规则的 boolean结果。

服务外层异常策略不是统一 fail-open。`IllegalArgumentException`被解释为 PackageManager输入异常或欺骗，回 internal REJECT；其他 `Exception`被视为 Integrity实现故障，回 ALLOW。与此同时，`getPackageArchiveInfo()`把自身解析异常吞掉并返回 null，调用者又显式 ALLOW。因此要按异常在哪一层被转换来判断，不能只背最外层 catch顺序。

最终 effect经本地 `PackageManagerInternal`转成1或0回到 PMS；PMS才把非 ALLOW写入 `mRet`。Stats日志记录 effect、证书、安装器与规则原因，但日志不是安装放行源，State也没有保存 matched rule集合。

### 练习 8：推演 App Integrity规则决策与异常分支

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mHandler.post(() -> handleIntegrityVerification(intent));' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'String installerPackageName = getInstallerPackageName(intent);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'if (!getPackageListForUid(installerUid).contains(installer)) {' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'PackageInfo packageInfo = getPackageArchiveInfo(intent.getData());' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'if (packageInfo == null) {' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'builder.setPackageName(getPackageNameNormalized(packageName));' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'builder.setAppCertificates(appCertificates);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'builder.setInstallerName(getPackageNameNormalized(installerPackageName));' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'builder.setAllowedInstallersAndCert(getAllowedInstallers(packageInfo));' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'extractSourceStamp(intent.getData(), builder);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'IntegrityCheckResult result = mEvaluationEngine.evaluate(appInstallMetadata);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F '.filter(rule -> rule.getEffect() == FORCE_ALLOW)' frameworks/base/services/core/java/com/android/server/integrity/engine/RuleEvaluator.java
grep -n -F 'return IntegrityCheckResult.allow(matchedPowerAllowRules);' frameworks/base/services/core/java/com/android/server/integrity/engine/RuleEvaluator.java
grep -n -F '.filter(rule -> rule.getEffect() == DENY)' frameworks/base/services/core/java/com/android/server/integrity/engine/RuleEvaluator.java
grep -n -F 'return IntegrityCheckResult.deny(matchedDenyRules);' frameworks/base/services/core/java/com/android/server/integrity/engine/RuleEvaluator.java
grep -n -F 'return IntegrityCheckResult.allow();' frameworks/base/services/core/java/com/android/server/integrity/engine/RuleEvaluator.java
grep -n -F 'Slog.e(TAG, "Invalid input to integrity verification", e);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'Slog.e(TAG, "Error handling integrity verification", e);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'verificationId, PackageManagerInternal.INTEGRITY_VERIFICATION_REJECT);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
grep -n -F 'verificationId, PackageManagerInternal.INTEGRITY_VERIFICATION_ALLOW);' frameworks/base/services/core/java/com/android/server/integrity/AppIntegrityManagerServiceImpl.java
```

推演 installer UID/包名不匹配、rule provider默认旁路、APK解析为 null、FORCE_ALLOW与 DENY同时命中、只有 DENY、无命中、非法输入、内部异常八种路径。每例写明是在哪一层变成 ALLOW/REJECT，而不是只抄最外层 catch。

## 13. 两本账的正常汇合很简单，缺失身份与超时却会让它们分叉

正常 ordinary回调达到 complete后，Handler先按 `isInstallAllowed()`决定是否写失败，再在 Integrity也 complete时删 State，最后调用 `handleVerificationFinished()`。正常 Integrity回调则先解释 response、置 State complete、视两侧完成情况删 State，再调用 `handleIntegrityVerificationFinished()`。两个 InstallParams方法都只在自身完成位原先为 false时置真，并在另一验证位已真时尝试 `handleReturnCode()`；到达顺序不影响正常汇合。

但“State删表条件”与“安装主门条件”不是同一个公式：

| 现场 | State ordinary | InstallParams ordinary | 可能后果 |
|---|---|---|---|
| ordinary按配置旁路 | required被显式记 ALLOW | 初始 true | Integrity结束后正常汇合并删表 |
| required正常响应，sufficient满足 | complete且可判断 allowed | false→true | 等 Integrity后删表并继续 |
| required包名为 null | required永不 complete | 一直是初始 true | Integrity结束即可继续，pending残留 |
| required Component为 null | 响应 UID可能不命中 | 已置 false | 等 timeout；再受 Handler UID缺口影响 |
| 副用户只有 sufficient响应 | appId与 full UID可能不匹配 | 仍等待 required共同完成 | timeout、残留或延期卡住 |
| timeout默认 ALLOW但 UID不命中 | State可能仍不完整 | 被无条件置 true | 安装继续而 pending残留 |
| 非参与者抢延期 | State始终不完整 | 仍为 false | 没有后备 timeout，安装可永久等待 |

required包名为 null且 Manifest没有 sufficient时最能说明问题：Integrity结果若 ALLOW，`handleIntegrityVerificationFinished()`看到 ordinary完成位初始为 true，立即进入 `handleReturnCode()`；State却因 required UID=-1从未收到响应，`areAllVerificationsComplete()`永远为 false。若 Manifest声明 sufficient，广播甚至可能已经发出，但这些票不能补齐 required，也不阻止主流水线继续。

声明非空却零个有效 sufficient匹配是另一种分账反例：`sendPackageVerificationRequest()`先把局部返回值改成验证失败，State却没有任何 sufficient UID；required随后 ALLOW会令 State判断 ordinary allowed并广播 `ACTION_PACKAGE_VERIFIED(ALLOW)`，但 `mRet`已经在 `handleStartCopy()`末尾锁成 -22。Integrity与 rollback结束后仍会跳过 copy并报告安装失败。ordinary ALLOW通知与最终失败并不矛盾，它们来自不同账。

失败账具有单向性。ordinary拒绝、Integrity拒绝或更早位置错误只在 `mRet`仍成功时写入；后来的 ALLOW、duplicate或 rollback成功都不能恢复。反过来，boolean完成位不包含 pass信息，所以单看三位全真也可能只是“已经准备报告失败”。

## 14. rollback是第三道完成屏障，handleReturnCode再决定copy与报告分叉

带 `INSTALL_ENABLE_ROLLBACK`时，PMS向 system user发送 `ACTION_PACKAGE_ENABLE_ROLLBACK`有序广播，要求 `PACKAGE_ROLLBACK_AGENT`，把 `mEnableRollbackCompleted`改为 false。状态回调无论成功或失败都会移除 pending token并调用 `handleRollbackEnabled()`；失败只记录“继续安装”。默认10秒 timeout也移除 token、结束等待、发送取消 rollback广播，然后继续安装。r48在这里把“请求提供回滚能力”当作 best-effort屏障，而非安装必须成功的安全判定。

三位齐备后，`handleReturnCode()`先处理 dry run；普通路径中仅当 `mRet==INSTALL_SUCCEEDED`才调用 `mArgs.copyApk()`。如果 verifier、Integrity或早期位置检查已经失败，copy被跳过，但 `processPendingInstall(mArgs, mRet)`仍会把失败包装成 `PackageInstalledInfo`并排入统一结果回传链；它的 failure分支不会执行 `doPreInstall()`、真正安装或 `doPostInstall()`。若 copy自身失败，它的新返回码写回 `mRet`，同样由后者处理。即使调用到 `copyApk()`，`FileInstallArgs`遇到 `origin.staged`也只是把 code/resource file绑定到现有 stage并返回成功，不做第二次物理复制。

所以正确的完成序列是：异步门结束 → 读取粘滞 `mRet` → dry run单独返回或成功才进入 `copyApk()` → 统一处理结果。这里“进入 copy”是方法与状态机边界，不保证发生字节复制；普通 PackageInstaller stage常会命中 `origin.staged`快速路径。它不是“每道验证各自调用 copy”，也不是“State从 pending表删除就已经复制”。multi-package为每个 child创建自己的 InstallParams、verificationId和三门，parent要等全部 child回报后再以统一 completeStatus收口；不是整组共享一份 Verifier State。

staged也不是同一条一次性门。原 Session先向 commit receiver回 `INSTALL_SUCCEEDED / Session staged`；pre-reboot阶段抽取 APK内容创建临时非 staged Session，multi时只选择 APK children，随后只对这份 dry-run副本清 rollback并加 `INSTALL_DRY_RUN`，再进入本章验证入口。其中 ordinary仍服从启用矩阵、Integrity默认运行，dry run终点不 copy；原 staged事务的 rollback通知另有路径。重启后的实际 APK安装又创建 Session并加 `INSTALL_DISABLE_VERIFICATION`；这个 system_server创建的 Session走非 ADB disable分支，ordinary旁路，而默认开启的 Integrity再次运行，保留 rollback请求时还会再进入实际安装的 rollback门。APEX-only没有可抽取 APK，因而不进入这条 APK verifier链。ready、applied与failed才是 staged事务要继续观察的状态。

r48的 staged与 DataLoader组合还会更早分叉：`SessionParams.copy()`保留 `dataLoaderParams`，StagingManager抽取 APK时却向新 Session调用常规 `write()`；DataLoader Session的 `assertCanWrite()`会拒绝写普通文件。因此这类组合可能在 pre-reboot抽取阶段失败，尚未进入 ordinary或Integrity，不能误报成 verifier timeout。

ordinary或 Integrity造成的 `INSTALL_FAILED_VERIFICATION_FAILURE=-22`对外经 `installStatusToPublicStatus()`映射成 `PackageInstaller.STATUS_FAILURE_ABORTED`。公开状态刻意比 legacy原因粗；排障若只保存 public status，会知道安装被中止，却分不出是哪类 verification、哪个 verifier或哪一种 timeout边界。

## 15. Incremental与V4把 ordinary变成提交后的通知，不赋予事后否决权

只看 DataLoader与签名格式这一层特判：当 ordinary原本 enabled、origin也需要新 APK验证时，`mDataLoaderType==INCREMENTAL`且签名方案为 V4会跳过安装前 ordinary请求，State以 required ALLOW完成。它不是唯一旁路；disable flag、可信 Instant installer、ADB设置与 existing origin等已在第4节分别处理。App Integrity仍照常运行，rollback若请求也照常等待。V4数据块验证能力与 Incremental按需读取共同触发的是 ordinary时序特例，不是“整套验证关闭”，更不替代 Session阶段和后续 PMS阶段的签名、解析、scan、reconcile。

这项特判只在 Session真正交给 PMS之后讨论。上游 `streamAndValidateLocked()`若 `prepareDataLoaderLocked()`尚未完成会返回 false，请求仍停在 loader/Session上游；只有 loader unavailable或相关远端异常等分支才会另发 `STATUS_PENDING_STREAMING`，不能把每次 false都等同于这项 status。`DATA_LOADER_IMAGE_READY`才触发后续 validate/commit，multi又要等所有 children ready，之后才会调用 `mPm.installStage()`并创建本章 State。loader未 ready时 ordinary与Integrity广播都没发出，它们的 timeout自然也尚未起算。

只有安装事务已经成功 commit后，PMS在 `finally`中的 success分支遍历请求：筛出 Incremental+V4，读取已安装包的 base与 split code paths，另取一个递增 verificationId，计算 verification root hash string，然后发送 `ACTION_PACKAGE_VERIFIED`，result固定为 ALLOW，并附 root hash与 DataLoader type。失败事务不发送这份成功通知。

这个新 ID没有对应的 `mPendingVerification` State，也不是安装前被跳过 ID的恢复使用。广播发生在 commit成功之后，没有等待回调的代码，Verifier不能靠它否决已经提交的包；它的角色是把 root hash和成功事实告知 agent。看到 `ACTION_PACKAGE_VERIFIED + root hash`应先判断是否为这种事后通知，而不是在 pending表中盲找同 ID。

最后把相邻边界锁住：第258章的用户确认早于这里，只证明请求获准继续；APK signing details为这里的 V4判断和后续包身份检查提供事实，但 ordinary/App Integrity都不替代签名验证；`ACTION_INTENT_FILTER_NEEDS_VERIFICATION`管理 Web域名默认处理，有自己的 State、权限和结果，名字相似也不能用本章 verificationId追踪。

## 16. 九类证据能定位等待、残留、拒绝和事后通知

一次正常、非 Incremental特例、未请求 rollback的成功主线可压缩为下面九类证据。第3、4类请求顺序固定为 Integrity先、ordinary后；第5、6类的完成先后却可以互换：

1. `[入口]` `handleStartCopy()`早期返回码成功并创建 `InstallArgs`；这只说明 PMS验证阶段可启动。
2. `[建账]` token递增，非 existing origin的 `PackageVerificationState`进入 pending表；ID不是 sessionId。
3. `[Integrity请求]` 定向 `android`的有序广播发出，InstallParams Integrity位变 false；最终 Receiver才启动30秒窗口。
4. `[ordinary请求]` PMS先发送 sufficient显式普通请求，再发送 required有序请求，InstallParams ordinary位变 false；异步实际送达次序不由这两个调用保证，required链的最终 Receiver才启动10秒窗口。
5. `[ordinary聚合]` required允许且 sufficient条件满足；Handler经 ordinary通知通道发出触发消息的 raw code，并置 ordinary完成位，这仍不是安装成功，也不能把广播值脱离触发分支当成聚合对象。
6. `[Integrity聚合]` effect为 ALLOW，State Integrity complete、InstallParams Integrity完成；两侧 State都完成时 pending项删除。
7. `[rollback屏障]` 若未请求则初始已完成；若请求，成功、失败或 timeout都会结束等待，失败默认不阻断安装。
8. `[copy边界]` 三个完成位全真且 `mRet`仍成功，才调用 `copyApk()`；任何既有失败都跳过该方法，`origin.staged`则会进入方法但跳过物理复制。
9. `[安装终局]` 成功路径由 `processPendingInstall()`继续 prepare/scan/reconcile/commit，最终 status receiver收到结果；`STATUS_SUCCESS`才表示安装成功，各类 `STATUS_FAILURE_*`同样是终局，只是失败终局。

现场若偏离这条线，按“门、票、结果、清理”四列记录，而不是先猜组件：三项 InstallParams complete、required full UID与返回 code、每个 sufficient的记录 UID与 Binder UID、Integrity raw result、`mRet`首次变更点、extended标志、pending State是否删除、是否出现 copy、广播是否带 root hash。这样可以区分四种外观相似的问题：正常等待、安装已继续但 State泄漏、验证已拒绝正在报告、被无后备延期永久卡住。

### 练习 9：推演三门汇合、复制与 Incremental事后通知

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mVerificationCompleted' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& mIntegrityVerificationCompleted && mEnableRollbackCompleted) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mRet == PackageManager.INSTALL_SUCCEEDED) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mRet = mArgs.copyApk();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'processPendingInstall(mArgs, mRet);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case INSTALL_FAILED_VERIFICATION_FAILURE: return PackageInstaller.STATUS_FAILURE_ABORTED;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F '// For incremental installs, we bypass the verifier prior to install. Now' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (args.mDataLoaderType != DataLoaderType.INCREMENTAL) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (args.signingDetails.signatureSchemeVersion != SIGNING_BLOCK_V4) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final String rootHashString = PackageManagerServiceUtils' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'PackageManager.VERIFICATION_ALLOW, rootHashString,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

对 ordinary、Integrity、rollback三个完成位与成功/失败 `mRet`做组合推演：何时不动作、何时 copy、何时跳过 copy直接报告。再给一例 Incremental+V4成功安装，证明带 root hash的新 verificationId为何没有 pending State，也没有任何事后投票点。

本章最小心智模型是：State聚合 verifier事实并管理 pending寿命，InstallParams的完成位只管理异步汇合，`mRet`保存不可逆失败，三者共同决定能否 copy；最终安装成功还在它们之后。第260章从 `copyApk()`继续，进入 FileInstallArgs复制、stage重命名、Native Library与安装目录落位边界。
