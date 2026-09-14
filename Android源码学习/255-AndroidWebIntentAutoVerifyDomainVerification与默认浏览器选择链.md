# 255 Android Web Intent、autoVerify、DomainVerification与默认浏览器选择链

## 1. 主问题：URL 去向由“语法、证明、策略、候选”四层共同决定

一次 `https://example.com/path` 点击可能直接进入 App、进入默认浏览器、显示 ResolverActivity、转发到工作资料，或出现 instant installer。把这些结果都归因于 `android:autoVerify`，会把四类性质不同的事实混在一起：

1. `IntentFilter` 语法：Activity 是否声明了当前 action、categories、scheme、host 与 path；
2. 网站证明：HTTPS 站点是否以 Digital Asset Links 声明信任这个包名与签名证书；
3. Settings 策略：旧版包级 main status 与当前用户的 ALWAYS、ASK、NEVER、ALWAYS_ASK、generation；
4. 查询处置：PMS 怎样把已匹配候选分桶，何时加入默认浏览器、跨 profile 与 instant installer，最后怎样排序和选择。

准确主线是：

`安装 prepare 排验证消息 → Handler 聚合包内 Web Filters/hosts → 显式广播到验证器 → HTTPS assetlinks 整批核验 → 写包级与每用户状态 → URL 查询先做普通 Filter 匹配 → 按包状态分桶并处理浏览器/profile/instant → resolve 再选单项`

本文固定在 Android 11 / API 30 / `android-11.0.0_r48`。这一版仍使用 `IntentFilterVerificationInfo` 与包级状态；Android 12 之后的新版 `DomainVerificationService`、逐域名状态和新 shell/API 不能反推本章。主要源码坐标为：

- `frameworks/base/core/java/android/content/Intent.java`
- `frameworks/base/core/java/android/content/IntentFilter.java`
- `frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java`
- `frameworks/base/services/core/java/com/android/server/pm/IntentFilterVerificationState.java`
- `frameworks/base/services/core/java/com/android/server/pm/Settings.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java`
- `frameworks/base/packages/StatementService/src/com/android/statementservice/`

本章的终点是 PMS 形成 URL 候选与 `resolveIntent()` 结果；ATMS 的 exported、permission、Intent Firewall、AppOps 与后台启动门仍是后续执行裁决。

## 2. 五个 Web 谓词并不等价，甚至 package 快照门还有一处命名陷阱

先把常用谓词逐一拆开：

| 谓词 | 当前实现真正检查的条件 |
| --- | --- |
| `Intent.hasWebURI()` | data 非空，scheme 非空且为 `http` 或 `https` |
| `Intent.isWebIntent()` | `ACTION_VIEW` 加 `hasWebURI()`；不检查 BROWSABLE |
| `IntentFilter.handlesWebUris(true)` | VIEW、BROWSABLE、至少一个 scheme，且所有 scheme 都只能是 http/https |
| `IntentFilter.needsVerification()` | autoVerify 位为真，加 `handlesWebUris(true)` |
| `IntentFilter.handleAllWebDataURI()` | 有 `CATEGORY_APP_BROWSER`，或者 `handlesWebUris(false)` 且没有 authority |

`handlesWebUris(false)` 允许 Filter 同时包含自定义 scheme，只要其中至少有 http/https；`true` 则遇到任一非 Web scheme 就失败。两者都不要求 authority，所以一个无 host 的泛化 Web Filter 也能满足 `needsVerification()`，后续却可能因空 host 请求而失败。

包解析结束时还计算 `pkg.isHasDomainUrls()`，它是安装验证入口的快照门。`hasDomainURLs()` 的实现连续检查 `hasAction(ACTION_VIEW)` 与 `hasAction(ACTION_DEFAULT)`，而 `ACTION_DEFAULT` 在 `Intent` 中只是 `ACTION_VIEW` 的别名；它没有按方法注释检查 `CATEGORY_DEFAULT`，也不检查 BROWSABLE 或 host。故 package 快照门、`needsVerification()` 与“有可验证域名”是三套口径。

通用查询侧又用 `intent.hasWebURI()` 判断是否进入 domain 分支，并不要求本次 action 是 VIEW。于是旧模型可能把包级 domain 状态用于一个能匹配 `ACTION_EDIT + https` 的候选；外层/base Intent 显式 component/package 时绕开这套分桶。selector-only package 是例外：PMS 在替换为 selector 前保存 base `pkgName`，所以 base 未限定时，selector 可先按自身 package 缩小召回范围，随后仍走 domain 分支。必须同时看调用点谓词、base/selector 与查询形态。

### 练习 1：把 Intent、Filter 与 package 三层谓词逐行对齐

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public boolean hasWebURI() {' frameworks/base/core/java/android/content/Intent.java
grep -n -F 'return scheme.equals(IntentFilter.SCHEME_HTTP) || scheme.equals(IntentFilter.SCHEME_HTTPS);' frameworks/base/core/java/android/content/Intent.java
grep -n -F 'public boolean isWebIntent() {' frameworks/base/core/java/android/content/Intent.java
grep -n -F 'public final boolean handleAllWebDataURI() {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F '(handlesWebUris(false) && countDataAuthorities() == 0);' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public final boolean handlesWebUris(boolean onlyWebSchemes) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (!hasAction(Intent.ACTION_VIEW)' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public final boolean needsVerification() {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'return getAutoVerify() && handlesWebUris(true);' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'pkg.setHasDomainUrls(hasDomainURLs(pkg));' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'private static boolean hasDomainURLs(ParsingPackage pkg) {' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'if (!aii.hasAction(Intent.ACTION_DEFAULT)) continue;' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'public static final String ACTION_DEFAULT = ACTION_VIEW;' frameworks/base/core/java/android/content/Intent.java
```

## 3. 安装只排入验证请求：commit 与网络证明不是同一事务

普通非 instant 安装在 `preparePackageLI()` 已完成代码目录 rename 与 fs-verity 设置后调用 `startIntentFilterVerifications()`，此时后面的 scan、reconcile、commit 尚未全部结束。该方法不联网，只把 packageName、`isHasDomainUrls`、Activity 列表、replacing、userId 与 verifierUid 封进 `IFVerificationParams`，向 PMS Handler 投递 `START_INTENT_FILTER_VERIFICATIONS`。

常规安装本身由 `processInstallRequestsAsync()` 投到同一个 Handler，并在该 Runnable 内持 `mInstallLock` 执行 `installPackagesLI()`。因此 prepare 中新投的验证消息通常要等当前安装 Runnable 让出 Looper 后才能处理：成功提交不是等待网站响应得出的，网络失败也不会回滚 APK。

这个时序仍不是原子保证。多包安装可在较早 package 的 prepare 阶段排入消息，随后另一 package 在 scan/reconcile 失败；当前 Runnable 结束后验证消息仍会处理。若目标没有进入 `mSettings.mPackages`，创建持久 `IntentFilterVerificationInfo` 会失败，但内存验证状态与外部请求未必在同一点消失。反过来，设备没有选出 `mIntentFilterVerifierComponent` 时，入口直接返回，连消息也不会排。

instant 安装明确跳过这条 App Link 验证。不要把“instant 以后也能参与 URL 解析”与“instant APK 在此安装点接受同一网络验证”混为一谈。

### 练习 2：确认验证消息位于 prepare 与 commit 之间

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void processInstallRequestsAsync(boolean success,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.post(() -> {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'installPackagesTracedLI(installRequests);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private PrepareResult preparePackageLI(InstallArgs args, PackageInstalledInfo res)' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'setUpFsVerityIfPossible(parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'startIntentFilterVerifications(args.user.getIdentifier(), replace, parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private void startIntentFilterVerifications(int userId, boolean replacing, AndroidPackage pkg) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Message msg = mHandler.obtainMessage(START_INTENT_FILTER_VERIFICATIONS);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case START_INTENT_FILTER_VERIFICATIONS: {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private void installPackagesLI(List<InstallRequest> requests) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'reconciledPackages = reconcilePackagesLocked(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'commitPackagesLocked(commitRequest);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 4. 批次构造：一个合格 autoVerify 触发，包内全部 Web Filter 共同投票

Handler 中的 `verifyIntentFiltersIfNeeded()` 先检查 Activity 列表和 package 快照门。新安装且 Settings 已存在恢复来的 `IntentFilterVerificationInfo` 时，会仅凭“对象存在”直接返回；这里不要求其 main status 已是成功。替换安装则继续比较旧账。

随后第一轮扫描寻找触发器。只有 `needsNetworkVerificationLPr(packageName)` 允许，且至少一个 Filter 的 `needsVerification()` 为真，`needToRunVerify` 才成立。main status 为 UNDEFINED、ALWAYS、ASK 时网络验证仍被允许；其余值走默认拒绝。

一旦触发，第二轮不是只收 autoVerify 所在 Filter，而是把包内所有 `handlesWebUris(false)` 的 Filter 加入同一 verificationId。每个 Filter 的 authorities 经 `getHostsList()` 汇入 `ArraySet`；path、port 与 MIME 不构成网站身份。Filter 可以同时声明非 Web scheme，而某个没有 authority 的 Web Filter会贡献零个 host。

替换安装用新 domains 与旧 `IntentFilterVerificationInfo.domains` 比较。在 main status 仍允许网络验证、并且仍有合格 autoVerify 触发器的前提下，旧集合包含新集合且当前用户策略为 ALWAYS 时，可以保留状态并跳过重验；新增 host 只会令 `keepCurState` 失效，随后才进入重验。若旧 main 已是 NEVER，或 XML/Parcel 异常载入了其他非 UNDEFINED/ALWAYS/ASK 值，`needsNetworkVerificationLPr()` 会先压掉触发器，代码不会发送新请求，而会落入“曾验证、现不运行验证”的清账分支。这里有两条 r48 实现边界：

- pending `IntentFilterVerificationState` 与 `mCurrentIntentFilterVerifications` 在计算 `keepCurState` 之前已经创建；早退没有撤销它们。由控制流可推得，后续另一个包调用 `startVerifications()` 时可能把这条本想跳过的旧请求一并发出；若没有后续调用，它会留在内存集合中。
- 更新后 `hasDomainUrls == false` 会在读取旧验证账之前返回，所以“完全移除 Web 入口”并不会从这条路径清掉旧 `IntentFilterVerificationInfo` 与用户状态。

这两点是当前实现的生命周期缺口，不应提升为平台设计意图。

### 练习 3：验证触发扫描、全包聚合与早退位置

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void verifyIntentFiltersIfNeeded(int userId, int verifierUid, boolean replacing,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!hasDomainUrls) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!replacing && previouslyVerified) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final boolean needsVerification = needsNetworkVerificationLPr(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (needsVerification && filter.needsVerification()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (needToRunVerify || previouslyVerified) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (filter.handlesWebUris(false /*onlyWebSchemes*/)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIntentFilterVerifier.addOneIntentFilterVerification(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'domains.addAll(filter.getHostsList());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'hostSetExpanded = !previouslyVerified' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (needToRunVerify && keepCurState) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mCurrentIntentFilterVerifications.add(verificationId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mCurrentIntentFilterVerifications.clear();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 5. 验证器发现与请求：组件、UID、token、白名单各解决不同问题

PMS 启动时以 `ACTION_INTENT_FILTER_NEEDS_VERIFICATION` 和 package-archive MIME 查询 system-only Receiver，再检查候选包持有 `INTENT_FILTER_VERIFICATION_AGENT`，取 Filter priority 最高者作为显式组件。发现组件解决“广播发给谁”；安装时记录 verifier UID，又解决“回调由谁提交”。两者不能互相替代。

r48 在这里还有一条可达的多用户断接。`USER_ALL` 会被映射到 system user，但其他安装值直接用于 `getPackageUid()`，所以 user 10 的单用户安装会记录 user 10 verifier 的完整 UID；请求却固定 `sendBroadcastAsUser(..., UserHandle.SYSTEM)`。AOSP StatementService 的 Receiver 没有 `singleUser`，其 Service 从 user 0 回调，而 `setVerifierResponse()` 比较的是完整 UID，不是 appId。结果是：非 0 用户的 user-specific 安装可收到网站检查结果，却因 UID 不同而拒绝结账，token 保持 PENDING；system user 或 `USER_ALL` 不触发这个错位。该路径并非纯理论，因为非 shell/root 安装器会被清掉 `INSTALL_ALL_USERS`，Session 随后使用具体 `userId`。

每个批次分配递增 `verificationId`，创建 `IntentFilterVerificationState(verifierUid, userId, packageName)`，置为 PENDING，并保存 Filter 与 host 集合。`getHostsString()` 用空格拼接，并把 `*.example.com` 去掉 `*.` 后按根域名请求；它不枚举子域名，也不保留 path。原始 `example.com` 与 wildcard 归一后可能形成重复文本，因为去重发生在归一化之前。

`startVerifications()` 先把 domains 写入或更新 Settings 中的 `IntentFilterVerificationInfo`，随后发送显式前台广播。四个 extra 是 verificationId、固定 `https` scheme、空格分隔 hosts、packageName。即使 manifest 只声明 HTTP，网站证明仍从 HTTPS 获取。

发送前，PMS 用 `getVerificationTimeout()` 的时长把验证器临时加入省电白名单，并把同一时长放进 `BroadcastOptions`。默认下限是 10 秒，配置只能加长。这个 timeout 在 App Link 路径中没有对应“到点按 verificationId 自动失败”的 Handler 消息；它限制后台启动机会，不是网络验证完成期限。

### 练习 4：钉住验证器筛选、token 与显式广播

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private @NonNull ComponentName getIntentFilterVerifierComponentNameLPr() {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final Intent intent = new Intent(Intent.ACTION_INTENT_FILTER_NEEDS_VERIFICATION);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'MATCH_SYSTEM_ONLY | MATCH_DIRECT_BOOT_AWARE | MATCH_DIRECT_BOOT_UNAWARE,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (best == null || cur.priority > best.priority) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final int verificationId = mIntentFilterVerificationToken++;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIntentFilterVerificationStates.append(verificationId, ivs);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'params.installFlags &= ~PackageManager.INSTALL_ALL_USERS;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'user = new UserHandle(userId);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '(userId == UserHandle.USER_ALL) ? UserHandle.USER_SYSTEM : userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private String getDefaultScheme() {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return IntentFilter.SCHEME_HTTPS;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'verificationIntent.setComponent(mIntentFilterVerifierComponent);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'options.setTemporaryAppWhitelistDuration(whitelistTimeout);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mContext.sendBroadcastAsUser(verificationIntent, UserHandle.SYSTEM,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (mRequiredVerifierUid == callerUid) {' frameworks/base/services/core/java/com/android/server/pm/IntentFilterVerificationState.java
grep -n -F 'android:name=".IntentFilterVerificationReceiver"' frameworks/base/packages/StatementService/AndroidManifest.xml
grep -n -F 'private static final long DEFAULT_VERIFICATION_TIMEOUT = 10 * 1000;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return Math.max(timeout, DEFAULT_VERIFICATION_TIMEOUT);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 6. 回调完成协议：permission、required UID 与 token 三关之后才写账

验证器调用 `verifyIntentFilter(id, code, failedDomains)` 时，Binder 入口先要求 `INTENT_FILTER_VERIFICATION_AGENT`，再捕获 `Binder.getCallingUid()` 并把响应排回 PMS Handler。Handler 查不到 id 会记录无效 token；找到后仍要由 `IntentFilterVerificationState.setVerifierResponse()` 比较 caller UID 与安装时保存的 required verifier UID。

UID 不同不会完成状态，内存 token 也不会删除。UID 相同时，code 为 SUCCESS 得到 SUCCESS state，为 FAILURE 得到 FAILURE state；其他整数仍会把 `mVerificationComplete` 置真，但内部 state 保持 UNDEFINED，随后按 `isVerified() == false` 进入失败处置。这表明 permission 是调用资格，UID 是批次绑定，code 才是结果内容。

接受响应后，`receiveVerificationResponse()` 先把每个内存 ParsedIntentInfo 设为 verified/unverified，再从 pending map 删除 token，然后更新持久账。`failedDomains` 只在 FAILURE 日志中输出，既不参与 Filter 子集更新，也不形成 `host → status` 表。

`IntentFilter.setVerified()` 在 r48 还有一个直接可见的不一致：setter 清/写 `STATE_VERIFIED`，getter 却在 checked 后读取 `STATE_NEED_VERIFY`。IntentResolver 这里只把 getter 用于诊断日志；URL 候选真正读取 Settings 的包/用户状态。故内存 Filter 位不能作为验证真相的唯一证据。

若正确 UID 永远不回调，当前路径既没有 token 专用 timeout，也没有别的清理分支；PENDING 可留在 `mIntentFilterVerificationStates`。token 也不绑定包版本：只要同一轮 system_server 中旧 id 仍在，迟到的正确 UID 回调就会按 packageName 查当时当前的 IVI；新旧批次重叠时，较晚被 Handler 处理的响应可以覆盖较早写入的 main status。广播白名单到期不会代替回调关闭这本账。

## 7. StatementService 先规范输入：十个 host 上限早于网络请求

AOSP 默认验证器只是协议的一种实现，设备厂商可以提供满足 system-only、权限和 priority 选择条件的其他 Receiver。默认 `IntentFilterVerificationReceiver` 在主线程解析四个 extra，构造固定 relation，并通过 `startService()` 把工作交给 `DirectStatementService`。

Receiver 用空格切 hosts，超过 10 个立即向 PMS 回整批 FAILURE，不发 HTTP。这个上限属于 AOSP StatementService，不是 manifest 或 PMS 的语法上限。PMS 已经剥过 wildcard 前缀，Receiver 又做一次同样处理；随后 `Patterns.DOMAIN_NAME` 校验每个 host，并只接受 http/https scheme。空 host、非法包名、`getPackageInfo()` 抛 `NameNotFoundException` 或 Web asset URL 构造失败都会进入显式 FAILURE 回调；证书数组为 null 等未被这两个 catch 覆盖的运行时异常则不保证回调。

`DirectStatementService` 在后台优先级 `HandlerThread` 上执行网络与 JSON 解析，避免 Receiver 主线程阻塞。它返回 `START_STICKY`，代码中没有按单个 startId 调 `stopSelf()`；这属于服务生命周期实现边界，不是验证 token 的完成信号。

目标 Android asset 由 packageName 与 PackageManager `GET_SIGNATURES` 返回证书的规范化 SHA-256 指纹列表组成。无签名轮换历史时，这通常是当前 signer；若包携带 signing lineage，r48 的兼容接口只返回 `pastSigningCertificates[0]`，即最旧 signer，而不是当前 signer。target matcher 要求 packageName 相同、网站声明与该接口实际返回的指纹集合至少有一个交集，并不要求两边列表全等。

### 练习 5：检查 host 数量、输入拒绝与后台线程

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final Integer MAX_HOSTS_PER_REQUEST = 10;' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'private static final String HANDLE_ALL_URLS_RELATION' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'String[] hostList = hosts.split(" ");' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'if (hostList.length > MAX_HOSTS_PER_REQUEST) {' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'if (host.startsWith("*.")) {' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'if (!Patterns.DOMAIN_NAME.matcher(host).matches()) {' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'context.startService(serviceIntent);' frameworks/base/packages/StatementService/src/com/android/statementservice/IntentFilterVerificationReceiver.java
grep -n -F 'mThread = new HandlerThread("DirectStatementService thread",' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F 'mHandler.post(new ExceptionLoggingFutureTask<Void>(' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F 'return START_STICKY;' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F 'PackageManager.GET_SIGNATURES).signatures;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/Utils.java
grep -n -F 'MessageDigest.getInstance("SHA-256");' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/Utils.java
grep -n -F 'pi.signatures[0] = signingDetails.pastSigningCertificates[0];' frameworks/base/core/java/android/content/pm/parsing/PackageInfoWithoutStateUtils.java
```

## 8. Digital Asset Links：网站是 source，安装包身份是 target

每个 source Web asset 最终读取 `https://host/.well-known/assetlinks.json`。验证方向是“网站声明将某种关系授予 Android App”，不是 App 自称拥有网站。固定 relation 是 `delegate_permission/common.handle_all_urls`；target matcher 同时匹配 namespace、packageName 与至少一个共同证书指纹。

网站文件中对应一条记录的概念结构是：relation 数组包含 handle-all-urls，target 的 namespace 为 `android_app`，并列出 package_name 与 `sha256_cert_fingerprints`。本章只说明字段契约，不用示例短指纹冒充可部署内容。

网络实现还有一组会改变失败语义的硬边界：

- 初始 URL 固定 well-known path，单响应内容上限 1 MiB；
- connect 与 read timeout 都是 5 秒；I/O 失败最多尝试 3 次，重试间隔 3 秒；
- HTTP 404/500 返回空内容且不按 I/O 异常重试；
- 代码先把 follow-redirects 设真又立即设假，实际最终不自动跟随 HTTP 重定向；
- JSON 可包含 include，递归层级上限为 1；不安全的非 HTTPS include 会被拒绝；
- Service 安装 1 MiB `HttpResponseCache`，但“有缓存”不表示证明永远有效。

Retriever 的结果还携带 HTTP 过期时间，但 `DirectStatementService` 只读取 statements，不按 `getExpireMillis()` 安排重新验证。缓存过期只影响未来抓取，不会自动撤销已经持久化的 ALWAYS。

这些失败最后大多折叠成“这个 source 没找到匹配 statement”，不会把 DNS、timeout、HTTP status、JSON 错误分别写入 PMS 状态。整份 JSON 列表无法解析时会得到空结果。单个元素只有在 JSON reader 已成功消费、语义构造阶段抛 `AssociationServiceException` 时才会被跳过，此后同一文件的有效记录仍可成功；字段类型、数组内容或 reader 引发的 `JSONException`/`IOException` 会逃出逐元素循环，使整份列表折为空结果。

### 练习 6：核对 URL、relation、证书与网络失败边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final String WELL_KNOWN_STATEMENT_PATH = "/.well-known/assetlinks.json";' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'private static final int HTTP_CONNECTION_TIMEOUT_MILLIS = 5000;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'private static final int HTTP_CONNECTION_BACKOFF_MILLIS = 3000;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'private static final int HTTP_CONNECTION_RETRY = 3;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'private static final long HTTP_CONTENT_SIZE_LIMIT_IN_BYTES = 1024 * 1024;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'private static final int MAX_INCLUDE_LEVEL = 1;' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'connection.setInstanceFollowRedirects(false);' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/URLFetcher.java
grep -n -F 'if (connection.getResponseCode() != HttpURLConnection.HTTP_OK) {' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/URLFetcher.java
grep -n -F 'if (relation.matches(statement.getRelation())' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F '&& target.matches(statement.getTarget())) {' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F 'Result statements = mStatementRetriever.retrieveStatements(source);' frameworks/base/packages/StatementService/src/com/android/statementservice/DirectStatementService.java
grep -n -F '} catch (AssociationServiceException e) {' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/StatementParser.java
grep -n -F 'The element in the array is well formatted Json but not a well-formed Statement.' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/StatementParser.java
grep -n -F '} catch (JSONException | IOException e) {' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'public long getExpireMillis() {' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/DirectStatementRetriever.java
grep -n -F 'public static boolean hasCommonString(List<String> list1, List<String> list2) {' frameworks/base/packages/StatementService/src/com/android/statementservice/retriever/Utils.java
```

## 9. 整批 AND 语义：failedSources 能诊断，却不能保存部分成功

`IsAssociatedCallable` 对 source 列表逐个调用 `verifyOneSource()`。任一 source 没有 relation + target 双重匹配，或检索抛出关联异常，就加入 `failedSources` 并把 `allSourcesVerified` 置假；循环仍会继续检查其余 source。最终只有所有 source 都成功，ResultReceiver 才向 PMS 回 SUCCESS。

传给 Service 的 target、relation 或 source descriptor 整体格式错误时走 `RESULT_FAIL`，Receiver 同样映射为 PMS FAILURE，但 failedDomains 为空；这不同于网站 `assetlinks.json` 解析失败所形成的单 source 不匹配。逐 host 失败则携带 source asset 的 JSON 字符串，而不是单纯 host 字符串；PMS 只把它们拼进调试日志。

所以 r48 默认链的真值模型是：

`每个 source 独立抓取/查 statement → 所有 source 做 AND → 一个 verificationId 只有一笔成功或失败 → 一个包级 main status`

一个暂时离线的 host 会拖累同批其他已正确部署的 host，成功 host 也没有独立持久位。把部署节奏不同的大量域名塞进同一包，会提高整批失败概率；这是旧包级模型的工程后果。

## 10. 两张持久账：packages.xml 的 main 与 user restrictions 的 policy

全局 `IntentFilterVerificationInfo` 挂在 `PackageSettingBase` 上，记录 packageName、domains 集合与 `mMainStatus`。它写在 `/data/system/packages.xml` 对应 `<package>` 内的 `<domain-verification>`，未知包的待恢复项则暂存在顶层 `<restored-ivi>`。它不是 `host → result` Map；`setStatus()` 只接受 UNDEFINED(0)、ASK(1)、ALWAYS(2)、NEVER(3)，不接受 ALWAYS_ASK(4)。但 `readFromXml()` 与 Parcel 构造直接给 `mMainStatus` 赋值，不经过 setter；恢复或损坏输入仍可能带入 4 或其他整数。

每用户 `PackageUserState` 另存 `domainVerificationStatus` 与 `appLinkGeneration`，写在 `/data/system/users/<userId>/package-restrictions.xml` 的 `<pkg>` 属性中。UNDEFINED 时 `domainVerificationStatus` 属性省略，但 generation 只要非零仍会写出；读取时两者也不做枚举或配对校验。PMS 内部查询用一个 packed long：高 32 位是 status，低 32 位是 generation。`getDomainVerificationStatusLPr()` 先读用户值；只有高位 UNDEFINED 才退到 main status，并把低位变成 0。公开 `getIntentVerificationStatus()` 则直接返回用户高位，不替调用者执行这次 fallback。

这解释了“shell 显示 undefined、URL 却按 always 处理”：前者可能看到裸用户状态，后者可回退到网络验证写下的 main ALWAYS。也解释了同包所有匹配 Activity 共享策略：查询只拿 `PackageSetting` 的包级/用户级值，不用 `IntentFilterVerificationInfo.domains` 再按当前 host 查一次。

generation 的存储有两个边角。设置 ALWAYS 时分配新的每用户序号；切到其他 status 时 `setDomainVerificationStatusForUser()` 不清旧 generation，清 status 的方法也只改高位。XML 仍可能保存陈旧低位，但 domain 分桶只在 status 为 ALWAYS 时把它写入 `ResolveInfo.preferredOrder`。重启读文件后，Settings 把 next 设为现有最大 generation + 1，下一次 ALWAYS 更新又先加 1，所以序号可以跳号；它只需保持相对新旧，不承诺连续或等于时间戳。

### 练习 7：确认 main、per-user、fallback 与 generation 的所有权

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private int mMainStatus;' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 'public void setStatus(int s) {' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 's <= INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_NEVER) {' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 'public void readFromXml(XmlPullParser parser)' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 'mMainStatus = status;' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 'mMainStatus = source.readInt();' frameworks/base/core/java/android/content/pm/IntentFilterVerificationInfo.java
grep -n -F 'IntentFilterVerificationInfo verificationInfo;' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'long getDomainVerificationStatusForUser(int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'state.domainVerificationStatus) << 32;' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'void setDomainVerificationStatusForUser(final int status, int generation, int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'state.appLinkGeneration = generation;' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'private long getDomainVerificationStatusLPr(PackageSetting ps, int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (result >> 32 == INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_UNDEFINED) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'alwaysGeneration = mNextAppLinkGeneration.get(userId) + 1;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mNextAppLinkGeneration.put(userId, maxAppLinkGeneration + 1);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final String ATTR_DOMAIN_VERIFICATON_STATE = "domainVerificationStatus";' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final String ATTR_APP_LINK_GENERATION = "app-link-generation";' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'writeDomainVerificationsLPr(serializer, pkg.verificationInfo);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'XmlUtils.writeIntAttribute(serializer, ATTR_APP_LINK_GENERATION,' frameworks/base/services/core/java/com/android/server/pm/Settings.java
```

## 11. 自动响应的状态矩阵：有 IVI 时先改 main，用户明确选择通常保留

正确 verifier UID 的响应到达、且 packageName 对应的 IVI 仍存在时，main status 按本次结果改写：成功为 ALWAYS，失败为 ASK，并安排 Settings 主文件写入。若安装失败等原因使 IVI 不存在，token 已被删除，但这一响应不会创建新账。然后仅当安装 userId 不是 `USER_ALL` 时，PMS 才处理该用户策略：

| 旧 per-user status | 验证成功 | 验证失败 |
| --- | --- | --- |
| UNDEFINED | 改 ALWAYS，分配 generation | 仍 UNDEFINED，但实现仍走一次 update |
| ASK | 改 ALWAYS，分配 generation | 保持 ASK，不 update |
| ALWAYS | 保持 ALWAYS | 普通包降 UNDEFINED；sysconfig linked app 保持 |
| NEVER | 保持 NEVER | 保持 NEVER |
| ALWAYS_ASK | 保持 ALWAYS_ASK | 保持 ALWAYS_ASK |

因此自动成功不会覆盖用户明确的 NEVER/ALWAYS_ASK，自动失败也不会直接写 NEVER。NEVER 表示用户/策略拒绝，不是网络暂时失败。`USER_ALL` 安装只改 main，并记录忽略 per-user autoVerify；不能把它写成对每个现有用户循环更新。

SystemConfig `<app-link>` 是另一条预置信任路径。`primeDomainVerificationsLPw()` 只接受已安装 system package，聚合 BROWSABLE + http/https Filter 的 hosts，把 main 留为 UNDEFINED，却把指定用户状态设成 ALWAYS。非 system 包、未知包或没有 host 的条目只在 prime 入口记录警告。自动验证失败时的“不降级旧 ALWAYS”豁免却只检查包名是否存在于 `SystemConfig.getLinkedApps()`，不再复核 system 身份；若配置误列非 system 包，而它又从其他路径取得 ALWAYS，失败后仍可能被保留。

## 12. 更新与恢复：host 集合影响是否重验，却不在查询时逐 host 裁决

新安装如果 Settings 已从备份恢复任何 `IntentFilterVerificationInfo`，入口因对象存在直接跳过网络；它不检查 main 是否 ALWAYS。备份 writer 虽接收 userId，却实际遍历所有 package、只写全局 IVI，不含该用户 policy/generation。现存包在恢复时立即替换 IVI；未知包先进入 `mRestoredIntentFilterVerifications`，以 `<restored-ivi>` 留在 packages.xml，等 `addPackageSettingLPw()` 再挂到包上。用户查询可在自己的 status 为 UNDEFINED 时回退恢复来的 main。这是一条迁移信任路径，不是本次安装现场完成 HTTPS 证明。

替换安装若 main status 允许网络验证、且仍有合格 autoVerify 触发器：

- 新 domains 是旧集合的子集，且当前用户为 ALWAYS：更新 domains 后早退；
- 新增任一 host，或当前策略不是 ALWAYS：不能走保留状态早退，并在至少收进一个 Web Filter 时发起整批重验；
- 重验请求发送时会更新 domains 集合，但旧 main/per-user status 不会先清零，直到响应才按矩阵变化。

最后一点与查询的包级 fallback 组合成重要窗口：一个原来 ALWAYS 的包升级新增 host 后，在重验尚未响应时，新的 host 候选也可能暂时继承旧 ALWAYS，因为 URL 分桶没有用 `ivi.domains` 对当前 host 做二次限定。

只要调用已经穿过“存在 verifier、非 instant、Activity 非空、package `hasDomainUrls` 为真”等前置门，包曾有 IVI 却不再形成合格 autoVerify 触发器时，代码就删除全局 IVI；只有旧 domains 覆盖新 domains 且用户本来 ALWAYS 才保留用户状态，否则一并重置。旧 main 为 NEVER（以及异常载入的其他非 UNDEFINED/ALWAYS/ASK 值）也会让网络门关闭并落入这条清账分支。反之，无 verifier、instant 安装、Activity 为空，或 `hasDomainUrls == false` 都会更早返回；“删除所有 Web Filters 必然清账”并不是 r48 实现事实。

清账的作用域也不对称：即便只为一个 user 调用 `removeIntentFilterVerificationLPw()`，它也会把 `PackageSetting` 上的全局 IVI 置空，从而改变其他用户的 main fallback；`alsoResetStatus` 只决定是否清当前用户的高位 status，旧 generation 仍保留。PackageSetting 更新副本又浅共享旧 IVI 与每用户状态，显式比较/清理前天然继承旧账。若单用户卸载后 PackageSetting 仍因其他用户、system app 或 keep-uninstalled 策略而保留，`markPackageUninstalledForUserLPw()` 会保留该用户 domain status、只把 generation 归零，并保留全包 IVI；普通非 system 包已无安装用户且不保留时会转入完整删除。完整删除也只有在不带 `DELETE_KEEP_DATA` 时才清所有用户验证状态并移除 PackageSetting。

### 练习 8：沿自动响应、sysconfig 与更新早退检查状态迁移

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final boolean verified = ivs.isVerified();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ivi.setStatus(INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_ALWAYS);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ivi.setStatus(INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_ASK);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_ALWAYS:' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_UNDEFINED:' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'case INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_ASK:' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'Slog.i(TAG, "autoVerify ignored when installing for all users");' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private void primeDomainVerificationsLPw(int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!pkg.isSystem()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ivi.setStatus(INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_UNDEFINED);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'clearIntentFilterVerificationsLPw(packageName, userId, !keepCurState);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ps.setIntentFilterVerificationInfo(null);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'boolean removeIntentFilterVerificationLPw(String packageName, int userId,' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'verificationInfo = orig.verificationInfo;' frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java
grep -n -F 'IntentFilterVerificationInfo ivi = mRestoredIntentFilterVerifications.get(p.name);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'void writeAllDomainVerificationsLPr(XmlSerializer serializer, int userId)' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'ps.readUserState(nextUserId).domainVerificationStatus,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '0 /*linkGeneration*/,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public abstract boolean updateIntentVerificationStatusAsUser(' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public boolean updateIntentVerificationStatus(String packageName, int status, int userId) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (!mUserManager.exists(nextUserId)) return;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'android:name="android.permission.SET_PREFERRED_APPLICATIONS"' frameworks/base/core/res/AndroidManifest.xml
```

## 13. 管理 API 与默认浏览器：一个改 App Link 策略，一个提供 browser role

客户端准确方法名是 `PackageManager.updateIntentVerificationStatusAsUser()`，下沉到 `IPackageManager/PMS.updateIntentVerificationStatus()`，再写 `Settings.updateIntentFilterVerificationStatusLPw()`；不要把 per-user API 与 `IntentFilterVerificationInfo.setStatus()` 的 main setter 混名。服务端要求保护级别为 `signature|installer|verifier` 的 `SET_PREFERRED_APPLICATIONS`，但没有额外的 cross-user、user-exists、status-range、has-Web-filter 或 no-op 检查。未知包或按 calling user 被 AppsFilter 隐藏时返回 false；其余路径即使 status 非法、值未变、包无 Web Filter或 userId 不存在也可返回 true。重复写 ALWAYS 每次都会分配新 generation；不存在的 user 还会先生成内存 `PackageUserState`，随后因写调度发现用户不存在而不落盘。非法 status 会被 getter 原样读出，而 URL 分桶没有对应分支。

`getIntentVerificationStatus()` 仅在跨用户读取时要求 `INTERACT_ACROSS_USERS_FULL`；instant caller、未知包和不可见包都统一得到 UNDEFINED，成功时也只返回裸 per-user 高位，不做 main fallback。可见性检查同样基于 calling user，不是目标 userId。另一个 `getIntentFilterVerifications()` 没有显式权限，只做 instant/AppsFilter 门；虽然公开注释称 packageName 为 null 应返回全部，r48 Settings 实现却直接返回空列表。

`pm set-app-link` 把 `undefined|ask|always|always-ask|never` 映射成五个整数，先确认包存在且 `PRIVATE_FLAG_HAS_DOMAIN_URLS` 已置位，再调用更新 API。设成 ALWAYS 会分配新 generation；其他状态不获得新的相对次序。这个 shell 入口是有权限的管理操作，不是第三方 App 可以任意把自己升级成 ALWAYS 的证明。

默认浏览器是另一套状态。Android 11 的 `RoleManagerService` 以 `ROLE_BROWSER` 持有者实现 `PermissionManagerServiceInternal.DefaultBrowserProvider`；PMS 查询 URL 时通过 PermissionManagerInternal 读取包名。旧 Settings 中的 default-browser 字段主要用于迁移/兼容，不应与 App Link status 或普通 PreferredActivity 合并。

同样，用户 preferred activity 保存 IntentFilter、候选集合与目标 Component；domain status 保存于 PackageSetting/PackageUserState，并在 Web 候选分桶阶段生效。三类“默认”虽都会影响最后选择，所有者与失效条件完全不同。

## 14. URL 候选分桶：ALWAYS 优先，ALWAYS_ASK 重新制造歧义

第254章的普通 `IntentFilter.match()`、用户态与初始排序先产出 candidates；domain policy 不会召回 manifest 根本不匹配的 Activity。在 base Intent 未限定 component/package 的通用查询中，PMS 看到当前解析 Intent 的 `hasWebURI()` 后才考虑这条分支；仅 selector 带 package 时仍属于这一路。没有 parent 候选、当前结果不超过一个且不准备加入 instant 时，会直接返回；有 parent domain 候选、当前结果为空且不加入 instant 时，也直接返回 parent forwarding。两者都不做当前 profile 分桶，单个 NEVER 候选也可能因此留在结果中。只有还需合并/筛选的候选集才按命中 `ResolveInfo` 分组：

- `handleAllWebDataURI == true` 进入 `matchAllList`，作为泛化浏览器；
- 非浏览器按包的有效 status 进入 `alwaysList`、`undefinedList`（UNDEFINED 与 ASK）、`alwaysAskList`、`neverList`；
- status 为 ALWAYS 时，把 packed low word 的 generation 写入 `preferredOrder`。

初始规则是：有 ALWAYS 就只加入 alwaysList；没有才加入 ASK/UNDEFINED，并可加入 parent-profile forwarding，同时开启 `includeBrowser`。只要存在 ALWAYS_ASK，又会把当前 result 的 `preferredOrder` 全清零、加入 alwaysAskList，并开启浏览器。它的实现效果是让该 App 参与候选并抹平 generation 优势，增加进入询问的可能；`chooseBestActivity()` 仍可能按当前排序字段直接收敛，状态名本身不保证 ResolverActivity 一定出现。

需要浏览器时，`MATCH_ALL` 直接加入全部 `matchAllList`。否则在 ROLE_BROWSER 包的匹配中选其最高 priority 条目；只有它存在、包名非空，且 priority 不低于 `max(0, 所有通用浏览器的最高 priority)`，才只加入这一条，否则加入全部浏览器。零下限意味着所有 browser priority 都为负时，即便默认浏览器并列最高也不能单独收敛。

最后若 result 仍为空，代码退回全部原 candidates 再删除 neverList。domain 过滤完成后外层令 `sortResult = true`，通常以六键 comparator 重排；Web instant installer 若随后加入，也发生在这次排序之前。post-resolution 的删除或动态 split 原位替换仍可能改变最终列表形状。

### 练习 9：手算五个桶、默认浏览器与 generation 排序

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (intent.hasWebURI()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (result.size() == 0 && !addInstant) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if (result.size() <= 1 && !addInstant) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ArrayList<ResolveInfo> alwaysList = new ArrayList<>();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ArrayList<ResolveInfo> alwaysAskList = new ArrayList<>();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (info.handleAllWebDataURI) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'info.preferredOrder = linkGeneration;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (alwaysList.size() > 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (alwaysAskList.size() > 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'i.preferredOrder = 0;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if ((matchFlags & MATCH_ALL) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.getDefaultBrowser(userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'int maxMatchPrio = 0;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& defaultBrowserMatch.priority >= maxMatchPrio' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'result.addAll(candidates);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'result.removeAll(neverList);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '& PackageManagerInternal.RESOLVE_NON_RESOLVER_ONLY) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'permissionManagerInternal.setDefaultBrowserProvider(new DefaultBrowserProvider());' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
grep -n -F 'RoleManager.ROLE_BROWSER));' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
```

## 15. 跨 profile、instant 与最终选择发生在不同阶段

当前 profile 查询后，PMS 在 domain 分桶之前就计算是否允许外部 instant resolution。Web Intent 需要非空 host，且 Web instant 功能未禁用。对非浏览器候选，只要某包有效状态为 ALWAYS 或 ALWAYS_ASK 就拒绝外部发现；是否已有匹配的 installed instant App 则在 browser 判断之外检查，所以任一这类候选都能阻断。ASK/UNDEFINED 与泛化浏览器的 status 本身不形成同样阻断。

只有存在 parent，且 `UserManager.hasUserRestriction(ALLOW_PARENT_PROFILE_APP_LINKING, sourceUserId)` 为真时，跨 profile domain 查询才在 parent user 对同一 Intent 做普通组件匹配。它忽略 `handleAllWebDataURI` 浏览器，聚合非浏览器候选的最佳 status，并只返回一个 `IntentForwarderActivity` ResolveInfo。`bestDomainVerificationStatus()` 把 NEVER 特判为最差，再对其他状态取数值较大者；不能直接按 0—4 的整数认为 NEVER 优于 ALWAYS。

若当前候选为空、parent domain 候选存在且不加入 instant，外层会在分桶前直接返回 forwarding。其余需要分桶的场景中，当前用户没有 ALWAYS 时，parent forwarding 可与 ASK/UNDEFINED 一起进入 result；当前已有 ALWAYS 时通常不加入。SKIP_CURRENT_PROFILE 是更早的 cross-profile 路径，也可能直接只返回 forwarding 结果，不能与 domain preferred 分支合并。

domain 筛选与必要排序之后才进入 post-resolution AppsFilter/instant 可见性清理。`queryIntentActivities()` 在列表处完成；`resolveIntent()` 还会调用 `chooseBestActivity()`：单项直接返回，多项可能由前三个选择字段、persistent/user preferred、已安装 instant 特判决定；仍无法收敛时，`RESOLVE_NON_RESOLVER_ONLY` 请求返回 null，其余才构造 ResolverActivity。App Link ALWAYS、默认浏览器 role 与最终 Resolver 选择是连续阶段，不是同一个“默认应用”字段。

## 16. 收束：按八个完成点定位 URL 为什么去了那里

遇到 URL 去向异常，按以下顺序取证最可靠：

1. 固定版本、userId、调用 UID、`resolveForStart`、Intent action/categories/data/flags，分开记录 base 与 selector 的 component/package；再判断 `hasWebURI()`、SKIP_CURRENT_PROFILE、单候选或仅 parent 早退是否真的让它走到旧 domain 分桶；
2. 用普通 Filter 规则确认 action、BROWSABLE/DEFAULT、scheme、host、port、path 是否真的匹配，并记录获胜 Filter 的 `handleAllWebDataURI`；
3. 安装侧区分 package `hasDomainUrls` 门、`needsVerification` 触发 Filter 与第二轮全包 Web Filter 集合；
4. 对 pending 请求记录 verificationId、required UID、userId、归一化前后 hosts、广播发出时间；白名单到期不等于 token 完成；
5. 网络侧逐 host 检查 HTTPS well-known URL、HTTP status、redirect、大小、timeout、JSON relation、packageName 与证书指纹，最后再看整批 AND；
6. 同时读取全局 IVI main/domains 与目标用户 status/generation；裸用户 UNDEFINED 仍可能在内部查询回退 main；
7. 更新场景记录旧/新 host 集、是否早退、重验是否在途，并警惕旧 ALWAYS 暂时覆盖新增 host 与 pending state 残留；
8. 查询侧按 browser、ALWAYS、ASK/UNDEFINED、ALWAYS_ASK、NEVER、parent、instant 分桶，再检查默认浏览器零下限、重排、post-filter 与 chooseBest。

对应的八个完成点也应分开：消息入队只说明“安装提出验证”；广播发送只说明“验证器获得请求”；每个 source 检查完成只说明“单站点有结果”；PMS 接受 required UID 回调才说明“批次关闭”；Settings 写入才说明“策略可恢复”；普通 Filter 匹配只说明“组件语法合格”；domain 分桶说明“本次解析允许保留哪些候选”；ATMS 后续授权通过才说明“Activity 启动可以继续”。

下一章转入 256：`PackageInstallerSession` 如何从创建与参数校验，经过文件写入、sealed/committed 状态和父子会话门，最终把一次安装请求交给 PMS 多阶段安装链。
