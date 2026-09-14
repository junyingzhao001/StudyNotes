# 252 Android PackageParser2、ScanRequest、Reconcile与PackageSetting提交链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题不是“扫描成功了吗”，而是“何时成为系统事实”

第 251 章已经把 PMS 启动看成旧账与磁盘候选的调和。本章把镜头推进到一枚 APK：它先被解析成 `ParsedPackage`，再进入 scan、appId、reconcile 与 commit；其中 appId 和 reconcile 的先后还随入口改变。看似是一条流水线，真正困难却在于每一段对“完成”的定义都不同。

| 层级 | 代表对象或动作 | 此时已经成立 | 此时仍不成立 |
|---|---|---|---|
| 解析完成 | `ParsedPackage` | Manifest、组件、代码路径等候选事实可读 | 旧身份可继承、签名获授权、appId 已分配 |
| scan 完成 | `ScanResult` | 策略、ABI、seinfo、候选 `PackageSetting` 等已计算 | 整批候选相容、全局表已替换 |
| reconcile 完成 | `ReconciledPackage` | 更新签名、shared UID、共享库与替换约束已检查或形成提交材料 | 提交必然无异常、普通读者已可见 |
| 锁内核心 commit 完成 | `commitReconciledScanResultLocked()`返回 | Settings 包项、`mPackages`、组件、AppsFilter 等核心索引已被逐项改写 | 普通安装的 `updateSettingsLI()`已结束、其他线程已观察到、磁盘账已耐久 |
| 外层解锁 | 离开调用者的 `synchronized (mLock)` | 遵守同一锁协议的并发查询可观察新世界 | 异步权限处理、安装后动作都已结束 |
| 持久化或后置完成 | Settings 写、`executePostCommitSteps()`等 | 对应那一次写或后置阶段完成 | 其他异步支线也都完成 |

所以本章的唯一主问题是：**Android 11 r48 中，一枚 APK 候选在哪个线程、哪把锁、哪个写入和哪个解锁点，才从可计算对象变成其他线程可查询的包事实；在那以前，又有哪些状态已经被提前改动？**

答案不会是某一个方法名。它是一条“可变候选 → 带别名的扫描请求 → appId 与批量相容性判定按入口采用不同顺序 → 非事务提交 → 外层解锁 → 持久化与后置动作”的完成梯。

## 2. 启动扫描与普通安装走两条生产路径，却共用同一提交核心

PMS 源码说明了两把关键锁。`mInstallLock`串行化安装、删除、ABI 和 `installd`一侧的重操作；`mLock`保护 Settings、`mPackages`、组件、shared user 等共享内存。`LI`、`LPr`、`LPw`和 `Locked`只是锁合同提示，不会替调用者执行 `synchronized`，判断真实边界必须沿调用链找显式加锁。

启动路径有两个并行面：PMS 构造器先长期持有 `mInstallLock → mLock`，随后才在这段双锁区内创建 `ParallelPackageParser`并向线程池提交 parse；SystemServer 主线程继续持锁，通过有界队列 `take()`收结果，再逐个进入 `addForInitLI()`完成 scan、reconcile 和 commit。worker 并行解析不等于主线程先释放了两把锁。队列按 worker 完成次序交付，也不承诺新包 appId 按文件名或目录枚举次序分配。此时公开 Binder 尚未注册，启动代码可先建立初始包世界。

普通安装则由 `PackageHandler`承接异步请求。生产调用在 `mInstallLock`下准备并扫描一批包；每个 scan 后先在 `mLock`临界区外乐观注册 appId，再进入 `mLock`做整批 reconcile 和 commit。离开 `mLock`以后才执行 `executePostCommitSteps()`，但这时仍处于外层 `mInstallLock`。因此“post-commit 在锁外”只能指局部离开了 package lock，不能推出已离开 install lock。

两条路径的共同核心是 `scanPackageOnlyLI()`、`reconcilePackagesLocked()`和 `commitReconciledScanResultLocked()`，但顺序并不完全相同，尤其 appId 注册时点要到第 13 节再比较。

### 练习 1：沿显式加锁与队列还原两条路径

先只标出线程、队列、两把锁和普通安装的后置动作。不要凭方法后缀推测持锁状态。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "Internally there are two important locks:" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "// CHECKSTYLE:OFF IndentationCheck" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "} // synchronized (mLock)" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "} // synchronized (mInstallLock)" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "return ConcurrentUtils.newFixedThreadPool(MAX_THREADS, \"package-parsing-thread\"," "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "mQueue.put(pr);" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "return mQueue.take();" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "addForInitLI(parseResult.parsedPackage, parseFlags, scanFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "private void processInstallRequestsAsync(boolean success," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "installPackagesTracedLI(installRequests);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "executePostCommitSteps(commitRequest);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

应画出两张图，而不是把所有箭头塞进一条线：启动是“构造器取得双锁 → 锁内提交并行 parse → worker 解析、主线程持锁收件 → 逐包提交”；普通安装是“PackageHandler → install lock → 批量扫描与 appId 预占 → package lock 内调和和提交 → 离开 package lock → 后置动作”。两者最终写同一组全局结构，所以锁协议比线程名更能决定可见性。

## 3. `PackageParser2`复用线程局部工作台，缓存只保存解析候选

`PackageParser2`标为 `@AnyThread`，但它没有为每次调用新建全部辅助对象。`mSharedAppInfo`和 `mSharedResult`是 `ThreadLocal`；同一工作线程会复用自己的 `ApplicationInfo`和 `ParseTypeImpl`。后者同时充当输入与结果容器，`reset()`清理结果、错误和延迟错误表，却不是“把对象每个字段恢复出厂值”：包名、target SDK 等上下文字段另有更新时点。

这解释了两个边界。第一，跨线程没有共享同一 `ParseTypeImpl`，同线程连续任务却必须遵守 reset 协议。第二，`close()`只对调用它的当前线程执行 `ThreadLocal.remove()`；它不是远程清空线程池中所有工作线程的槽位。

缓存由 `PackageCacher`负责。r48 entry key 精确使用 APK 文件的 basename 加 parse flags，不含完整路径；命中 freshness 只比较 `packageFile`的修改时间是否早于 cache 文件。缓存目录本身再用 build fingerprint 与两个存储属性形成版本命名空间。目录命名空间负责系统版本级失效，单 entry 的 mtime 负责文件级失效，二者不能互相替代。

`useCaches`只控制是否尝试读取；只要 `PackageParser2`配置了 `mCacher`，成功解析后仍会写入结果。启动并行解析显式传 `true`，另一些直接扫描入口使用无 cacher 的 parser 并传 `false`。缓存反序列化得到的是 `PackageImpl`解析模型，不包含 Settings 里的 appId、每用户状态、权限或上一次签名授权结论。因此“cache hit”最多等于省去 Manifest 解析，绝不等于包已获安装许可。

### 练习 2：把线程复用、读缓存和写缓存分开

根据命中点回答三个问题：谁在每线程复用？`useCaches=false`是否必然禁止写缓存？并行启动解析为何不能从命中缓存推出 appId 已存在？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "@AnyThread" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "private ThreadLocal<ApplicationInfo> mSharedAppInfo =" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "private ThreadLocal<ParseTypeImpl> mSharedResult;" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "if (useCaches && mCacher != null) {" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "ParsedPackage parsed = mCacher.getCachedResult(packageFile, flags);" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "mCacher.cacheResult(packageFile, flags, parsed);" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "mSharedResult.remove();" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageParser2.java"
grep -n -F "return pkg.st_mtime < cache.st_mtime;" "frameworks/base/services/core/java/com/android/server/pm/parsing/PackageCacher.java"
grep -n -F "return mPackageParser.parsePackage(scanFile, parseFlags, true);" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "parsedPackage = pp.parsePackage(scanFile, parseFlags, false);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

正确结论是：ThreadLocal 隔离工作台而非制造不可变结果；`useCaches=false`只跳过读取，写入还取决于 parser 是否配置 cacher；缓存内容也只是后续 scan 的输入候选。r48 的共享缓存目录只按构建指纹与存储属性分代，并未按 APK 所在目录再分命名空间；两个候选若具有相同 basename 和 flags，就会映射到同一 entry，mtime freshness 也不核对反序列化对象的 codePath。这是需要保留的碰撞风险，不能把 basename key 当成完整路径身份。

## 4. `ParsedPackage`与`AndroidPackage`在 r48 是同一可变对象的两种类型视图

`PackageImpl`同时实现 `ParsedPackage`和 `AndroidPackage`。`hideAsParsed()`与 `hideAsFinal()`都直接 `return this`；“hide”只收窄调用者看到的接口，不复制、不冻结对象，也不建立发布屏障。

对象初建时 `uid=-1`。直到 `commitReconciledScanResultLocked()`中 appId 已经确定，代码才执行 `parsedPackage.setUid(pkgSetting.appId)`，随后取得 `AndroidPackage`视图。这一字段在 PMS 语境实际承载 appId；完整 Linux UID仍需与 Android userId 组合。

更强的证据来自提交阶段：获得 `hideAsFinal()`视图后，后续代码仍会调整包内对象，例如设置 instrumentation 的 package name。若干 List、Map、Bundle getter 也返回原引用。于是“final”应读作服务内部面向消费者的接口边界，不能读作 Java 不可变性。

这会直接影响快照推理。一个早先保存的 `AndroidPackage oldPkg`可能仍指向某个历史对象，但同一 `PackageImpl`在 scan 和 commit 间持续被填充 ABI、seinfo、UID 等字段。要证明某字段稳定，必须找到最后写点、持锁范围和是否复制，而不能只看静态类型名。

### 练习 3：证明两个接口视图没有对象隔离

顺着实现类、初始 UID、两个 hide 方法和提交写点，判断引用身份与字段变化的先后。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "class PackageImpl extends ParsingPackageImpl implements" "frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/PackageImpl.java"
grep -n -F "private int uid = -1;" "frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/PackageImpl.java"
grep -n -F "public ParsedPackage hideAsParsed() {" "frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/PackageImpl.java"
grep -n -F "public AndroidPackage hideAsFinal() {" "frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/PackageImpl.java"
grep -n -F "ParsedPackage setUid(int uid);" "frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/ParsedPackage.java"
grep -n -F "parsedPackage.setUid(pkgSetting.appId);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final AndroidPackage pkg = parsedPackage.hideAsFinal();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "a.setPackageName(pkg.getPackageName());" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

引用关系应写成 `ParsedPackage view ─┐`、`AndroidPackage view ─┴→ same PackageImpl`。类型视图能限制某段代码可调用的方法，却不能把已发布引用背后的对象图自动变成 immutable。

## 5. 证书材料与身份预检不是同一关，两条入口的时序也不同

启动扫描的 `addForInitLI()`会在 parse 后调用 `collectCertificatesLI()`。若旧 `PackageSetting`的代码路径、时间戳、签名数据库状态等条件允许复用，代码可以新建一份旧 `SigningDetails`给候选；否则从 APK重收。这里的“收集成功”只证明有了比较材料，不证明它与旧身份、shared UID 或升级 keyset相容；启动路径把这项授权留给后面的 reconcile。

普通安装不走这一段复用协议。`preparePackageLI()`优先采用 `args.signingDetails`；若调用方没有提供，则用 `ParsingPackageUtils.getSigningDetails(..., false)`从暂存 APK取材。随后它在 `mLock`下对已有包先做一次 upgrade-keyset 或 `verifySignatures()`快速检查，目的是在权限重定义等更深准备动作前尽早失败；兼容签名路径甚至可能在这里写回 live `PackageSignatures`。后面的 batch reconcile还会在整批候选与当前包世界中再次裁决。因此不能给两条入口套上“证书收集之后才首次认证”的同一时间线。

对启动路径，r48 的强制重收还有一个容易被注释带偏的事实：系统分区候选在系统升级时 `forceCollect=true`；数据分区调用 `PackageManagerServiceUtils.isApkVerificationForced(pkgSetting)`，而该 helper 在这一版本无条件返回 `false`。`forceCollect=false`只表示允许在严格条件满足时复用旧材料，不等于一定不收证书；路径、时间戳或签名数据库条件不合仍会重收。不要把面向未来的分支写成 r48 已启用的数据分区强制策略。

系统分区候选的 `skipVerify`恒为 true；stock r48 的数据分区路径则因 force 分支不可达而保持 false。unsafe 路径调用 `unsafeGetCertsWithoutVerification(..., JAR)`，跳过完整内容验证并抽取证书，且允许回退到 JAR/v1；base 与 split 仍须证书一致。这不等于“已经验证了全部 APK 内容”，也不宜扩大成只验证某一种签名块的承诺。可信只读分区是允许这样取材的前提，候选能否继承既有身份仍由 reconcile 的 keyset、签名 lineage 和 shared-user 规则决定。

`getOriginalPackageLocked()`也不能替代签名关口。它用于 original-package 名称迁移和 shared-user 形状筛选；启动候选面向本次材料的兼容判定在 reconcile，普通安装则已有 prepare 预检并在 reconcile中复核。

### 练习 4：对照启动与普通安装的取材、预检和调和

找出启动旧签名复制、force/skip 条件、普通安装的调用方材料/现场抽取与快速预检，再定位 reconcile中的签名比较。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "new PackageParser.SigningDetails(ps.signatures.mSigningDetails));" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final boolean forceCollect = scanSystemPartition ? mIsUpgrade" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F ": PackageManagerServiceUtils.isApkVerificationForced(pkgSetting);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final boolean skipVerify = scanSystemPartition" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "collectCertificatesLI(pkgSetting, parsedPackage, forceCollect, skipVerify);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (args.signingDetails != PackageParser.SigningDetails.UNKNOWN) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ParsingPackageUtils.getSigningDetails(parsedPackage, false /* skipVerify */));" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "Quick sanity check that we're signed correctly if updating;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final boolean compatMatch = verifySignatures(signatureCheckPs, null," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final boolean compatMatch = verifySignatures(signatureCheckPs," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "static boolean isApkVerificationForced(@Nullable PackageSetting ps) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java"
grep -n -F "re-enable." "frameworks/base/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java"
```

完成点至少要分三格：签名材料可用、普通安装 prepare 的早期身份预检、整批 reconcile 的最终相容性检查。材料真实与身份获准是两件事；早期通过也不能替整批候选作最终担保。

## 6. updated-system 候选可能先预扫描，再决定是否值得收证书和主扫描

`addForInitLI()`处理系统分区候选时，会先在 `mLock`下查现有 setting、original setting 与 disabled-system setting。若存在 disabled-system setting，说明 `/data`上曾有更新版，当前系统分区 APK 是其基线候选。

此时 r48 有一段反直觉顺序：代码先用 live disabled setting 构造 `ScanRequest`，调用一次 `scanPackageOnlyLI()`，若使用了候选外壳，再通过 request 中的别名把结果 `updateFrom()`回那份 disabled setting；离开这段锁后，才比较路径和 versionCode，计算 `isSystemPkgBetter`。这次预扫描发生在本次 `collectCertificatesLI()`之前，而且已经可能改旧系统基线，不能视为一次可随手丢弃的探测。

分叉如下：

| 条件 | 后续动作 | 这一候选的结果 |
|---|---|---|
| 系统候选路径改变且 versionCode 更高 | 清理 `/data`更新资源、启用系统包 | 继续收证书，再走主 `scanPackageNewLI()` |
| disabled setting 存在，但系统候选不更好 | 直接抛 `PackageManagerException` | 停在收证书与主扫描之前，继续采用稍后扫描的 `/data`版 |
| 不属于 updated-system 分叉 | 不走这次预扫描 | 正常收证书和主扫描 |

因此只能说“更好的系统基线可能经历预扫描和主扫描两次”；不能说每个 updated-system 包都完整扫描两次，也不能说第一次扫描已经通过签名授权。第一次的作用更像用旧系统 setting推导候选状态，为版本竞争与切换准备材料。

若系统分区出现同名但 Settings 中只有普通 data 包，又会进入另一组签名和版本竞争：签名不相容可删除 data 包；系统版本更新可回切；系统版本较旧则隐藏系统候选，等待 data 版重新加入。名称相同从来不是身份相同的充分条件。

### 练习 5：定位预扫描、版本闸门与主扫描

按源码位置给命中点编号。特别观察：哪条调用早于证书收集，哪一分支会在证书收集之前抛出，哪一条才进入主 scan/reconcile/commit。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (isSystemPkgUpdated) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanPackageOnlyLI(request, mInjector, mFactoryTest, -1L);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanResult.request.pkgSetting.updateFrom(scanResult.pkgSetting);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final boolean isSystemPkgBetter = scanSystemPartition && isSystemPkgUpdated" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (scanSystemPartition && isSystemPkgUpdated && !isSystemPkgBetter) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "collectCertificatesLI(pkgSetting, parsedPackage, forceCollect, skipVerify);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final ScanResult scanResult = scanPackageNewLI(parsedPackage, parseFlags, scanFlags" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

若只画“parse → cert → scan”，这段 r48 特例就会消失。更可靠的画法是把 `addForInitLI()`视为候选仲裁器：它可在证书收集前做一次受限扫描，也可在版本闸门处终止该系统候选。

## 7. `ScanRequest`是引用束，不是时间冻结的请求快照

`ScanRequest`把 `parsedPackage`、`oldPkg`、`pkgSetting`、`disabledPkgSetting`、`originalPkgSetting`、shared user、flags 和 user 等引用绑在一起。字段声明为 `final`只保证引用槽不再改指向，不保证被指向对象不变。

最容易误判的是 `oldPkgSetting`。构造函数确实执行 `new PackageSetting(pkgSetting)`，但 r48 的复制合同只是 one-level-deep cloning。`PackageSettingBase`复制时共享 `keySetData`、`signatures`与 verification info；用户状态虽然新建 SparseArray，却把每个 `PackageUserState`值对象原样放入；`PackageSetting`还共享 `pkg`与 `sharedUser`。权限状态、静态库数组和 mime groups 又各有自己的复制策略。所以这是一张混合复制图，不是递归深拷贝。

| 字段 | 外壳是否新建 | 典型嵌套对象是否共享 | 可否作为稳定历史证据 |
|---|---:|---:|---|
| `oldPkgSetting`本体 | 是 | 是 | 只可按具体字段逐项判断 |
| `oldPkg` | 否 | 直接引用旧 AndroidPackage | 取决于旧对象后续是否再变 |
| `parsedPackage` | 否 | 同一 PackageImpl | scan/commit 期间持续可变 |
| `sharedUserSetting` | 否 | 共享全局对象 | reconcile 甚至会直接改签名态 |
| flags、user 引用槽 | 值或 final 引用 | 语义较稳定 | 仍不是整个请求的快照证明 |

源码在 `scanPackageOnlyLI()`现有包分支把复制称为避免修改现存系统状态，但底层复制实现比这句话更弱。分析异常回滚时必须以赋值和对象身份为准：外壳字段改动可能隔离，嵌套对象改动仍可能泄漏到 live state。

### 练习 6：审计“复制”究竟复制到哪一层

对照 request 构造与两个 setting 类的复制构造。若稍后修改 `oldPkgSetting.signatures.mSigningDetails`或某个共享的 user-state 值，能否只影响快照？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "static class ScanRequest {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "this.pkgSetting = pkgSetting;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "this.oldPkgSetting = pkgSetting == null ? null : new PackageSetting(pkgSetting);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "New instance of PackageSetting with one-level-deep cloning." "frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java"
grep -n -F "keySetData = orig.keySetData;" "frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java"
grep -n -F "signatures = orig.signatures;" "frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java"
grep -n -F "mUserState.put(orig.mUserState.keyAt(i), orig.mUserState.valueAt(i));" "frameworks/base/services/core/java/com/android/server/pm/PackageSettingBase.java"
grep -n -F "pkg = orig.pkg;" "frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java"
grep -n -F "sharedUser = orig.sharedUser;" "frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java"
```

答案不是笼统的“浅”或“深”，而是一张 alias map。只有新外壳自己的标量与后来被整体替换的字段天然隔离；共享的 signatures、keyset、user-state 值、pkg 和 shared user 都要求额外证明。

## 8. `scanPackageNewLI()`先补政策身份，再校验候选，还可能创建 shared user

`scanPackageNewLI()`先解析 renamed/original/installed/disabled setting，再由 `adjustScanFlags()`把旧账里的系统分区身份补回 updated-system 候选，包括 system、privileged、vendor、product 等。随后在 `mLock`里执行 `applyPolicy()`和 `assertPackageIsValid()`；解析成功的包仍可能因 core-app、重复库名、静态共享库规则、代码路径或特权约束在这里失败。

shared UID 的 privileged 推导有一处细节：当候选自身还未标 privileged、声明 shared UID、且不处于旧 vendor 跳过条件时，代码查询该 shared user。若 shared user 已 privileged，非平台签名候选通常被补上 `SCAN_AS_PRIVILEGED`；平台签名候选不补，但后面的 privileged 校验豁免会接受它。老 vendor 跳过路径则可能让候选到后续校验时被拒绝。不能把这段缩成“共享 privileged UID 一律拒绝”或“一律提权”。

通过校验后，声明 shared UID 的候选会调用 `getSharedUserLPw(..., true)`。源码明确承认这里可能分配一个新 `SharedUserSetting`；Settings 实现还会为它注册 appId并放进 `mSharedUsers`。也就是说，方法名里的 scan 尚未返回，Settings 世界就可能已有 shared-user 记录和 UID 槽。后续普通 appId cleanup不会删除这个 map 项，也不会恢复其签名 lineage，不能依赖“scan 是纯计算”的直觉。

`@GuardedBy`同样不是运行期自动锁。r48 的 wrapper、静态 `scanPackageOnlyLI()`标注和部分调用链并不完全对称；生产路径的安全性来自外层显式持锁与启动期不可公开，而不是注解替 Java monitor 执行同步。

## 9. `scanPackageOnlyLI()`主要构造候选，但并非无副作用

`scanPackageOnlyLI()`把 request 解包，决定复用还是新建 `PackageSetting`，填充代码/资源路径、版本、flags、每用户 instant/full 状态、seinfo、ABI、native library 路径、shared-user ABI 等。它还不断改写同一个 `parsedPackage`，所以即使 setting 完全隔离，候选对象本身也不是只读输入。

新包用 `Settings.createNewSetting()`建立候选 setting；现有包先 `new PackageSetting(pkgSetting)`再 `Settings.updatePackageSetting()`。结合上一节的 alias map，这个新外壳不能保证嵌套状态完全隔离。源码自己的方法注释也承认“无副作用”并不完全真实。

至少要追踪三类提前变化：

- 对显式 user，现有 setting 候选会经过 `setInstantAppForUser()`改变对应安装形态；共享 user-state 值让隔离边界需要逐字段验证；
- ABI 推导会改 `ParsedPackage`，`applyAdjustedAbiToSharedUser()`还可改相关 shared-user 包与 setting，并返回受影响代码路径；
- 第 8 节中的 shared-user 创建发生在 wrapper 里，早于本方法返回，属于更外层的 scan 副作用。

因此 `ScanResult.success=true`不能等价于“此前没有触碰 live state”，失败也不能自动等价于“所有变化已撤销”。必须按写点分类：候选外壳、共享嵌套对象、全局 Settings、文件系统动作，各自的清理机制不同。

### 练习 7：找出 scan 阶段已经越过的副作用边界

把注释承认的副作用、setting 外壳复制、用户态调整、shared-user ABI 调整和成功返回放在一条线上。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "SIDE EFFECTS; may potentially allocate a new shared user" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "true /*create*/);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "Not entirely true at the moment. There is still one side effect" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "pkgSetting = new PackageSetting(pkgSetting);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "setInstantAppForUser(injector, pkgSetting, userId, instantApp, fullApp);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "changedAbiCodePath = applyAdjustedAbiToSharedUser(pkgSetting.sharedUser, parsedPackage," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "return new ScanResult(request, true, pkgSetting, changedAbiCodePath," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

建议把每个写点标成 C、A、G、F：candidate-only、aliased nested state、global state、filesystem。只有 C 类天然可随候选丢弃；A/G/F 都必须找到显式恢复或接受其提前生效。

## 10. `ScanResult`是一包提交材料，不是已经安装的包

`ScanResult`携带原始 request、success、候选 `pkgSetting`、`existingSettingCopied`、ABI 变化路径和静态/动态 shared-library 描述。它最重要的语义是“足够让后续调和与提交继续”，不是“全局查询已经成功”。

`existingSettingCopied=true`表示 scan 最终在一个复制出的 setting 外壳上计算；commit 时会取 request 中的 live `pkgSetting`并执行 `updateFrom(result.pkgSetting)`。为 false 时，结果 setting本身将进入 Settings。它不简单等于“进入 scan 时存在旧 setting”：若旧 setting 的 shared user 与本次不符，局部变量会被清空并重新创建。这个布尔量控制提交合并策略，也决定普通 appId 是否需要注册，并不证明内部对象无别名。

`success`字段在这条生产返回路径通常为 true；scan 失败主要通过异常离开，不能把它想成一个完整的错误枚举。成功结果仍需通过整批 reconcile：两个单独合法的候选可能冲突于同一 shared library、签名关系或替换计划。

还要注意 `changedAbiCodePath`只报告共享 UID ABI 调整涉及的路径。直到 commit，代码才尝试对这些路径做 `rmdex`；即便 `InstallerException`被忽略，包提交仍继续。于是这个 list 既不是回滚日志，也不是“所有副作用”清单。

## 11. `ReconcileRequest`构造的是同一锁下的批次视图，不是冻结世界

普通安装会先为一批 `InstallRequest`逐个得到 `ScanResult`，然后一次性创建 `ReconcileRequest`。它把 scannedPackages、installArgs、installResults、prepareResults、现有 shared-library source、allPackages、versionInfo 和静态库旧 setting 绑在一起。启动单包路径则用 singleton maps 与空安装材料复用同一调和器。

普通路径传入的 allPackages 是 `Collections.unmodifiableMap(mPackages)`：只读 wrapper 阻止经该引用写 map，却不会复制底层内容，也不会冻结其中的 `AndroidPackage`对象。批次一致性来自整个 reconcile 在 `mLock`下执行；若脱离这把锁保存 request，它就不是历史快照。

调和器先复制当前 allPackages 到本地 `combinedPackages`，再让每个 incoming parsed package按包名覆盖旧项。这样共享库解析能看到“现有世界 + 本批全部候选”，而不是只看到提交顺序中更早的候选。它还先收集 incoming shared libraries，并为普通替换预计算 `DeletePackageAction`。库冲突规则需细分：static library按同名同 version判重，non-static library同名也会重复；dynamic library又受 system provider与旧 provider策略约束。启动/system-dir 的 required dependency校验还会延后到全局库更新，不能概括成一次统一的静态库判重。

这解释了为什么 reconcile 必须是批次关口：scan 回答“这枚包在给定 setting 下怎样成形”，reconcile 回答“这些候选同时进入现有世界是否相容”。逐包 scan 成功无法替代后一个问题。

## 12. `reconcilePackagesLocked()`会直接改 shared-user 签名态，并非纯验证器

签名调和先选择 `signatureCheckPs`：静态共享库更新可能使用上一版本 setting，否则使用 scan 结果 setting。若 KeySetManager 要求 upgrade keyset，就验证 upgrade key；初始扫描、`SCAN_INITIAL`与 shared UID等情况不会一概走这条捷径。否则调用 `verifySignatures()`比较旧包、disabled-system 包与本次 `SigningDetails`，必要时标记稍后移除旧 keyset 数据；兼容签名路径本身也可能把迁移后的 signing details写回被检查的 `PackageSignatures`。

shared UID让“验证”越过纯函数边界。正常 lineage 合并时，代码取得 `signatureCheckPs.sharedUser.signatures.mSigningDetails`，调用 `mergeLineageWith()`，结果对象不同就立刻写回 shared user；`signaturesChanged`为 null 时也立刻写成 false。系统包 OTA 的允许变更分支还会直接把本次 signing details 写入 shared user，并把 `signaturesChanged`写成 true。

这些赋值发生在 `ReconciledPackage`创建之前。若同一批后面的另一个包再失败，不能假设 reconcile 从未改 live shared-user 状态。外层持有 `mLock`能阻止普通并发读者看到中间值，却不提供事务回滚。

系统包的签名异常还有版本边界：某些 OTA 情况允许本批第一个 shared-user 系统包建立新基线，但同一启动批后续包必须一致；较新首发 API 设备可能把不一致升级为 `IllegalStateException`。这不是“系统包永远忽略签名失败”。

### 练习 8：把批次合并、替换计划和签名写点串起来

先找本地 combined map，再找 incoming library、删除计划、keyset 分支与 shared-user 的直接赋值。判断异常发生时哪些只是局部变量，哪些已经改了共享对象。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "private static class ReconcileRequest {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "combinedPackages.putAll(request.allPackages);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "combinedPackages.put(scanResult.pkgSetting.name, scanResult.request.parsedPackage);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final Map<String, LongSparseArray<SharedLibraryInfo>> incomingSharedLibraries =" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "deletePackageAction = mayDeletePackageLocked(res.removedInfo," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (ksms.shouldCheckUpgradeKeySetLocked(signatureCheckPs, scanFlags)) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "SigningDetails mergedDetails = sharedSigningDetails.mergeLineageWith(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "signatureCheckPs.sharedUser.signatures.mSigningDetails = mergedDetails;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "signatureCheckPs.sharedUser.signaturesChanged = Boolean.TRUE;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

一张准确的副作用表至少应把 `combinedPackages`和 `incomingSharedLibraries`标为局部容器，把 `DeletePackageAction`标为计划材料，把 shared-user signing details 与 flag 标为锁内全局写。方法名里的 reconcile 不能替代对象身份分析。

## 13. appId 是为提交消除一个失败点的乐观预占，两条路径顺序相反

`optimisticallyRegisterAppId()`只在 `existingSettingCopied=false`时调用 `mSettings.registerAppIdLPw(result.pkgSetting)`。新普通包的 setting因此获得新 appId；已存在包复制 setting时沿用旧 appId。shared UID 包的 appId本来属于 `SharedUserSetting`，它可能已在第 8 节的 shared-user 创建中注册；此处通常不会把 `PackageSetting`变成那个 appId 的所有者。

“optimistic”表示在完整提交前先改 Settings 的 appId registry。真正可能因空间不足抛错的是 `registerAppIdLPw()`；后面的 `parsedPackage.setUid(pkgSetting.appId)`只是赋值，不会再分配 UID。它本身已经是全局写，不是候选字段计算。失败清理仅在注册函数返回确实创建了 appId时调用 `cleanUpAppIdCreation()`，并通过 `removeAppIdLPw()`释放槽位。

两条生产路径的相对顺序不同：

| 路径 | appId 与 reconcile 的顺序 | 锁与失败清理含义 |
|---|---|---|
| 启动 `addForInitLI()` | 先 reconcile，后为该包乐观注册 appId，再 commit | 全程位于构造期外层 `mLock`；reconcile 失败前尚未预占 |
| 普通批量安装 | 每个 scan 成功后先乐观注册 appId，整批之后才 reconcile | 预占位于 `mInstallLock`内却在整批 `mLock`临界区外；调和失败要清理此前多个预占 |

源码没有把两种顺序抽象成统一事务协议，可靠结论只应停在实际调用次序：启动是 reconcile → appId，普通安装是逐包 appId → batch reconcile。由于普通 UID 查询持 `mLock`读取 appId registry，而这次写不在同一锁下，系统级或未被过滤的查询存在提前看到有限 UID 记录的窗口；这不是完整包发布，甚至不是一份由共同 monitor保证一致的数据视图。

清理边界仍然有限：释放 appId不等于撤销 scan 的每个 alias 写、shared-user 创建、reconcile 的签名变更或文件系统准备。遇到失败要逐类核对，不能把 `cleanUpAppIdCreation()`称为整笔事务回滚。

## 14. commit 是按顺序改写多张内存表的过程，不具备原子回滚

`commitReconciledScanResultLocked()`先把 scan 结果合并回 live setting或采用新 setting，连接 shared user，设置安装来源；再把确定的 appId写进同一 `PackageImpl`并取得 `AndroidPackage`视图。它还调用 `writeUserRestrictionsLPw()`、处理 shared library更新、keyset、签名、权限继承和 ABI dex 清理，最后进入 `commitPackageSettings()`。对已有包，用户 restrictions调用可能在最终包索引发布之前就真实落盘；新包尚未插入 Settings时则会直接返回。

后者继续按顺序改全局世界：先注册候选提供的 shared-library info并更新 library clients，再做 frozen诊断和必要的 client kill；随后把 setting插入 Settings、把 package放入 `mPackages`、登记 APEX/keyset、向 ComponentResolver加入组件、更新 AppsFilter、permission groups与permissions，最后登记 instrumentation、protected broadcasts并按条件投递异步权限撤销。顺序不是事务日志，异常不会自动倒序恢复。

源码警告明确承认 `commitReconciledScanResultLocked()`可能在提交中途抛异常并留下不一致状态。更具体地说，`commitPackageSettings()`在 `checkPackageFrozen(pkgName)`之前就可能调用 `commitSharedLibraryInfoLocked(info)`。而 r48 的 frozen 检查只是未冻结时执行 `Slog.wtf()`的诊断性断言，不会显式拒绝提交；它既不是共享库写之前的检查，也不是事务总闸门。任何更后的未检查异常仍可能面对已经改变的库 registry。

`mPackages.put()`也不是唯一发布点。查询包名可能依赖它，组件解析依赖 ComponentResolver，可查询性依赖 AppsFilter，签名和权限查询又依赖 Settings/PermissionManager。一次锁内 commit 的原子可见性来自读者共同持 `mLock`，不是这些容器在某一条语句同时切换。

## 15. 提交返回、并发可见、磁盘耐久与安装结束是四个时间点

启动路径在 `addForInitLI()`内 commit 后仍处于 PMS 构造函数的外层 `mLock`。其他遵守锁协议的线程要等构造期在后面释放 `mLock`才可观察；公开 `package` Binder又要等整个构造返回、白名单步骤之后才注册。故“启动包已 put 入 `mPackages`”早于“普通并发读者可见”，也早于“外部客户端能发现 Binder”。并且该包仍可能被后续 `/data`扫描、`mExpectingBetter`、stub 或 updated-system 仲裁替换，单包 commit 不是整轮启动扫描的最终裁决。

普通安装的 reconcile 与逐包 commit 同样位于一个外层 `synchronized (mLock)`中。每个 commit 返回时，整批仍可能继续，受同一锁保护的标准包名、组件与权限查询不能穿过锁看到半批完整结果；离开该 block后，它们才可看到整批内存状态。此前在锁外预占的 appId/shared user却可能给 UID 类查询留下有限的提前窗口。随后 `executePostCommitSteps()`在 package lock外、install lock内准备 app data、profile并执行 dexopt；关闭 freezer、运行时权限、广播与 observer 回调还在更后的 post-install 阶段。

持久化也不是 commit 的同义词。普通安装的 `commitPackagesLocked()`逐包执行 `commitReconciledScanResultLocked()`后，立即调用 `updateSettingsLI()`；后者在同一外层可重入 `mLock`内逐包执行 `mSettings.writeLPr()`。所以多包请求即使宣称整体失败，前面成员也可能已经改内存并落盘，后续 finally主要只释放由本次 optimistic 注册的普通 appId、关闭 freezer并修正结果码，不会恢复全部状态。启动则在全局扫描和若干修复完成后、仍持构造期锁时统一写主 Settings。某些权限撤销通过 `AsyncTask.execute()`投递，能晚于 commit 返回乃至外层解锁。

还要修正“锁外 kill”这一常见说法：若某 helper离开了自己的内层 `synchronized (mLock)`再 kill library clients，而生产调用者外面仍持有可重入的 `mLock`，实际线程并未释放最外层 monitor。必须把调用栈上所有同一把锁都数完。

### 练习 9：建立 appId、提交、解锁、持久化和 Binder 的完成梯

用下列锚点分别标出启动顺序、普通安装顺序、非事务警告、诊断性 frozen 检查、核心容器写、异步分支和最终外部入口。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "final Map<String, ReconciledPackage> reconcileResult = reconcilePackagesLocked(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "appIdCreated = optimisticallyRegisterAppId(scanResult);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "createdAppId.put(packageName, optimisticallyRegisterAppId(result));" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "reconciledPackages = reconcilePackagesLocked(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "WARNING:</em> The method may throw an excpetion in the middle" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "commitSharedLibraryInfoLocked(info);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "checkPackageFrozen(pkgName);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSettings.insertPackageSettingLPw(pkgSetting, pkg);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mPackages.put(pkg.getPackageName(), pkg);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mComponentResolver.addAllComponents(pkg, chatty);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mAppsFilter.addPackage(pkgSetting, isReplace);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "AsyncTask.execute(() -> {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "executePostCommitSteps(commitRequest);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSettings.writeLPr();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ServiceManager.addService(\"package\", m);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

最小可信结论是：appId 预占只消除一个将来失败点，并可早于完整包视图形成有限 UID 窗口；锁内多个容器按序更新；外层 `mLock`释放才是标准包/组件读者看到整批新世界的边界；Settings 写返回只证明对应那一次持久化调用结束；Binder 注册只是外部入口出现；后置和异步任务各有自己的终点。

## 16. 用“对象身份 × 锁 × 失败点 × 观察者”诊断整条链

阅读这段源码时，可对任何“已经完成”的说法连续追问四次：

1. **对象身份**：这是新对象、one-level copy、read-only view，还是同一 live 对象的别名？
2. **锁边界**：当前线程真正持有哪些 monitor？外层是否又持有同一把可重入锁？读者是否遵守同一协议？
3. **失败点**：下一条可能抛异常的语句之前已经改了哪些 candidate、aliased、global 或 filesystem 状态？有逐项恢复吗？
4. **观察者**：结论面向当前线程、锁内协作者、并发 Binder 客户端、重启后的 Settings，还是异步后置任务？

把本章关键节点放进诊断矩阵：

| 节点 | 对象身份 | 主要锁语境 | 失败后的核心风险 | 合法观察者结论 |
|---|---|---|---|---|
| parser cache hit | 反序列化 PackageImpl | 解析线程局部 | 缓存损坏会删除并重解析 | 只证明得到解析候选 |
| `hideAsFinal()` | 同一 PackageImpl | 尚在提交链 | 后续仍可改对象图 | 只证明接口视图收窄 |
| `ScanRequest` | 多个 live 引用 + 一层 setting 外壳 | wrapper 通常位于生产锁路径 | 嵌套 alias 可泄漏 | 不能当历史快照 |
| `ScanResult` | 候选 setting + 原 request | install/package 锁语境依路径而异 | scan 已可能有副作用 | 只证明可进入调和 |
| reconcile 返回 | 批次材料 | `mLock` | shared-user 签名态可能已改 | 只证明批次约束已走完 |
| optimistic appId | Settings 全局槽位 | 启动持 `mLock`；普通安装仅持 `mInstallLock` | 可提前形成有限 UID 记录；只清槽位不足以恢复其他写 | 证明 UID 材料已准备，不证明完整包已发布 |
| commit 返回 | 多张 live 表已顺序改写 | 调用者仍可能持外层锁 | 中途异常无事务回滚 | 当前线程可继续，读者未必可见 |
| 外层 `mLock`释放 | 同一批共享状态 | monitor 发布边界 | 异步与磁盘仍可能滞后 | 守约读者可见内存世界 |
| Settings 写返回 | 持久化调用完成 | 依具体调用路径 | 派生账与异步仍各自独立 | 对应写调用已结束 |

最终可以把一枚新普通包的主干压成：handler 请求 → `mInstallLock`下 prepare解析、签名预检与scan → 候选 setting与 parsed object继续可变 → package lock外乐观预占 appId → `mLock`下整批 reconcile → 逐包非事务 commit与 Settings写 → 释放 `mLock`后对标准包/组件读者发布 → package lock外准备 app data与 dexopt → 更晚结束 post-install。启动路径则是构造器先取得双锁 → 在锁内提交并行 parse、主线程持锁收件 → `addForInitLI()`先 reconcile、后 appId、再 commit → 构造后段仍在锁内统一写账 → 释放构造期锁 → 更晚注册 Binder。

第 253 章将转向查询侧：`PackageInfo`与`ApplicationInfo`怎样由已提交包模型叠加 `PackageUserState`生成，并由 `AppsFilter`决定调用者最终能看见哪些包与组件。
