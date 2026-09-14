# 251 Android PackageManagerService启动、Settings账本与系统包扫描链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 先把问题改写成“调和”，而不是“找 APK”

PackageManagerService（下文简称 PMS）启动时，不是在空白内存里枚举几个目录。它要把四类事实合成一个可查询、可执行的新包世界：

| 事实 | r48 中的主要载体 | 能回答什么 | 不能单独回答什么 |
|---|---|---|---|
| 上次认可的全局身份 | `Settings`、`packages.xml`、`PackageSetting` | appId、shared UID、签名关系、旧路径、系统更新基线 | 当前 APK 的组件声明是否仍存在 |
| 本次磁盘内容 | APK、APEX、`ParsedPackage` | 当前 Manifest、版本与代码路径；签名信息的收集时点随候选路径变化 | 跨重启 appId、每用户安装态 |
| 每用户差异 | package restrictions、runtime permissions | installed、stopped、enabled、运行时授权 | 包代码是否真的还在磁盘 |
| 平台策略 | `SystemConfig`、分区 scan flag、权限与 overlay 配置 | system/vendor/product/privileged 等身份规则 | 某个候选最终一定会被采用 |

因此本章唯一主问题是：**SystemServer 主线程怎样读取旧账、扫描系统与数据候选、处理系统更新版和 OTA 分叉，最后把哪些完成事实分别交给 Binder、`systemReady()` 与第三方启动屏障？** 单个 APK 内部怎样经过 parse、scan、reconcile、commit，留到第 252 章。

r48 的主线程顺序可压成：前置服务 → `PackageManagerService.main()` → 构造期读账 → APEX/系统/data 扫描与调和 → 提交 app-data Future → 构造内调用主账写入 → 构造返回 → 用户类型白名单处理 → 依次注册 `package`、`package_native` → 稍后调用 PMS `systemReady()` → 更晚在 AMS callback 中 join app-data，并在已提交时 join WebView Future → 推进第三方服务 boot phase。

这里特意写“提交 Future”而不是“完成 Future”：它从构造中段就可能并行执行，只有后面的 join 位置固定。还要先记住九个不同观察点：

| 观察点 | 已能证明 | 仍不能证明 |
|---|---|---|
| `Settings.readLPw()` 返回 | 走到了某条读取返回路径 | 全局 XML 内容健康、当前代码仍存在 |
| `BOOT_PROGRESS_PMS_SCAN_END` | 主扫描段已走完 | 权限、app data、主账写入和构造已完成 |
| 构造内 `writeLPr()` 返回 | 这次无成功返回值的写调用已经结束 | 新 `packages.xml` 一定写成、派生文件与后续白名单变化都已耐久提交 |
| PMS 构造函数返回 | 构造尾部也已执行 | 公开 Binder 已注册 |
| `package` 注册 | `IPackageManager` Binder 端点可被发现 | `IPackageManagerNative`的 `package_native`端点已注册、`main()` 已返回 |
| `mSystemReady=true` | PMS 早期阶段门已打开 | `systemReady()` 方法体已走完 |
| PMS `systemReady()` 正常返回 | 同步方法体已走到清空 `mExistingPackages` | app-data Future、白名单延迟写与 staged 验证支线都已结束 |
| `waitForAppDataPrepared()` 返回 | 那一枚初始 Future 已终止且 join 成功 | 所有用户、所有卷、每个目录都准备成功 |
| `PHASE_THIRD_PARTY_APPS_CAN_START` | SystemService 收到相应 boot phase | 它是全系统唯一的“进程启动位” |

## 2. SystemServer 前置依赖与 `onlyCore` 已先改变输入集合

PMS 的入口、巨大构造函数、后来的 `systemReady()` 和 app-data join，都是 SystemServer 主线程上的同步调用。此时主 Looper 已 `prepare`，但 `Looper.loop()` 还没有开始；“主线程”不等于这些调用已被消息队列异步化。

`startBootstrapServices()` 先把 `SystemConfig.getInstance()`提交给 SystemServer 初始化线程池。PMS 构造稍后也会取同一单例；getter 在 `SystemConfig.class` 上同步，二者竞争成为唯一初始化者：后台若已持锁构造，主线程才等待；主线程若先取得锁，就自己创建实例，稍后的后台任务只复用它。它是预取，不是取消依赖。

前置对象不只有 `Installer`：PlatformCompat、ActivityManager、DataLoaderManager、Incremental Service、DisplayManager 等都先建立。默认显示 boot phase 先完成，PMS 才取 display metrics。`Installer.onStart()`若一时找不到 `installd` 会投递后台重连，因此“Java Installer 服务已启动”也不等于 native 连接永远已经成功。

`mOnlyCore` 来自加密中的最小 Framework 路径，不是 safe mode。它一方面让 `ParsingPackageUtils`拒绝 `coreApp=false` 的包，另一方面跳过 `/data/app` 和若干非核心准备；它改变 PMS 建出的包集合，而不只是推迟第三方进程启动。

首次 PMS 扫描很慢，SystemServer 用 `try/finally` 暂停并恢复 Watchdog 对“当前线程”的观察。构造内另起的 `PackageHandler`仍会以十分钟阈值加入 Watchdog。这两个观察对象不能混为一谈。

### 练习 1：标出同步入口、预取与最小包世界

从以下命中点回答：SystemConfig 后台任务没结束时，PMS 能否绕过它？Watchdog 的恢复是否依赖 PMS 成功返回？`onlyCore` 是在注册完成后隐藏包，还是解析入口就拒绝包？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "Looper.prepareMainLooper();" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "SystemServerInitThreadPool.submit(SystemConfig::getInstance, TAG_SYSTEM_CONFIG);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "SystemConfig systemConfig = SystemConfig.getInstance();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "synchronized (SystemConfig.class) {" "frameworks/base/core/java/com/android/server/SystemConfig.java"
grep -n -F "Installer installer = mSystemServiceManager.startService(Installer.class);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "mSystemServiceManager.startBootPhase(t, SystemService.PHASE_WAIT_FOR_DEFAULT_DISPLAY);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "Watchdog.getInstance().pauseWatchingCurrentThread(\"packagemanagermain\");" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "mPackageManagerService = PackageManagerService.main(mSystemContext, installer," "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "Watchdog.getInstance().resumeWatchingCurrentThread(\"packagemanagermain\");" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "if (mOnlyCoreApps && !lite.coreApp) {" "frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java"
```

答案是：PMS 仍需取得同一 `SystemConfig` 单例；`finally` 负责恢复当前线程观察，即使 `main()`抛异常也会执行；`onlyCore` 在 lite parse 后就返回 `ONLY_COREAPP_ALLOWED` 错误，并非先注册再过滤。

## 3. `main()`装配共享锁；LocalServices 与公开 Binder 分属两条边界

`PackageManagerService.main()`先创建 `lock` 和 `installLock`，再由 Injector 分别传给协作者。Settings、UserManager、PermissionManager、ComponentResolver 与 PMS 共享 `mLock`这把状态锁；`installLock`则交给 PMS、UserDataPreparer 等安装/data 侧协作者。两把锁的职责不能概括成所有对象都共同持有：

- `mLock`保护解析后的包状态、Settings、组件与 shared-user 等高争用内存，常规路径应短持有；
- 锁协议规定常规 `LI`路径用 `mInstallLock`串行化 `installd`访问和重磁盘工作；不得持 `mLock` 再取它，可以持 `mInstallLock` 时短暂进入 `mLock`；本章后面的 r48 `fixupAppData()` worker 是实际例外；
- `LI`、`LIF`、`LPr`、`LPw`分别表示 install lock、install lock+冻结包、package lock 读、package lock 写。这是命名合同，不是 Java 类型系统检查。

启动构造按 `mInstallLock → mLock`长期持有两把锁，完成读账、扫描、调和和主写。公开 Binder 尚不可见，使这段特殊启动策略可建立单一初始世界；它不是普通安装可照搬的长锁模板。

构造很早就注册 `PackageManagerInternal`，随后才读取 Settings。LocalServices 可发现只说明内部对象已有入口，不说明包世界已经完整。`PackageHandler`也在双锁区早期启动并加入 Watchdog。

公开边界在构造返回之后：`main()`先注册兼容性 listener，再调用 `installWhitelistedSystemPackages()`，然后顺序注册 `package` 和 `package_native`。后者由 system_server 中的 Java 内部类 `PackageManagerNative extends IPackageManagerNative.Stub`实现，是第二个 Binder 端点，不是 native 进程。白名单处理若改变用户安装态，只安排十秒延迟的 Settings/restrictions 写；因此 Binder 首次可见时，内存已含白名单结果，磁盘却可能仍是构造内那次写入。

### 练习 2：区分内部可发现、内存完成与公开可查询

按源码顺序排列共享锁、LocalServices、读账、构造返回后的白名单和两个 Binder。再解释为何 `package` 已注册时不能推出 `package_native` 已注册。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "final Object lock = new Object();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "final Object installLock = new Object();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "LocalServices.addService(PackageManagerInternal.class, mPmInternal);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "// CHECKSTYLE:OFF IndentationCheck" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mFirstBoot = !mSettings.readLPw(mInjector.getUserManagerInternal().getUsers(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "m.installWhitelistedSystemPackages();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ServiceManager.addService(\"package\", m);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ServiceManager.addService(\"package_native\", pmn);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "static final int WRITE_SETTINGS_DELAY = 10*1000;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

完整顺序是：创建两把锁 → 构造早期注册 LocalServices → 进入启动期双锁 → `readLPw()` → 构造返回 → 应用用户类型白名单 → 注册 `package` → 注册 `package_native`。两个 `addService()`是相邻但独立的调用；第一条完成到第二条完成之间，`IPackageManager`端点可以已被发现，而 `IPackageManagerNative`端点尚不可见，`PackageManagerService.main()`也尚未返回。

## 4. Settings 是连续身份账，不是 Manifest 副本

生产设备上的相关持久化不能只画成一个 `packages.xml`：

| 文件或输出 | 主要内容 | 启动时角色 |
|---|---|---|
| `/data/system/packages.xml` | package setting、appId/shared UID、路径、签名/keyset、安装权限、版本指纹、renamed/disabled-system 记录 | 全局旧账输入 |
| `/data/system/packages-backup.xml` | 上一次未被新提交确认替代的全局副本 | 主账恢复候选 |
| `users/<id>/package-restrictions.xml` | installed/stopped/hidden/suspended/enabled、组件覆盖、preferred/default/cross-profile 等 | 每包、每用户差异输入 |
| permission APEX 每用户 device-protected `runtime-permissions.xml` | 包或 shared user 的运行时授权与 flags | r48 当前运行时权限输入 |
| `/data/system/users/<id>/runtime-permissions.xml` | 旧位置的运行时权限 | 当前文件缺失时的迁移回退 |
| `/data/system/packages.list` | appId、user-0 data path、seinfo、GID 等扁平派生信息 | 写出给 native 消费者；PMS 启动不从它恢复包世界 |
| kernel mapping | appId 与 excluded-user 等内核映射 | 写给内核接口；不是 PMS 启动恢复源 |

后文说“五本账”时，指五个逻辑责任域：主 Settings、用户 restrictions、runtime permissions、`packages.list`和 kernel mapping。main/backup 属于同一主账的恢复文件，permission APEX 当前文件与旧路径文件也属于同一 runtime-permission 账；物理文件行数不能直接当作账本数量。

`packages.xml`不保存 Activity、Service、Receiver、Provider 和 IntentFilter 的现行完整模型；那些声明必须从本次 APK 解析。反过来，只解析目录也恢复不了稳定 appId、shared UID、签名演进和每用户安装态。正确模型始终是：旧账保持连续性，当前文件给出现行代码事实，scan/reconcile 决定二者能否继续绑定。

## 5. `readLPw()`的 backup 优先不是“校验失败后再回退”

全局读取先看 backup。只要 backup 成功 `open`，就立即调用 `delete()`清理同时存在的普通主文件，而且不检查删除返回值，然后解析 backup；若 backup 内容后来解析失败，控制流不会回头再试 main。只有 backup 自身打不开时，才会继续尝试普通文件。

主、备都不存在时，代码把 internal 与 primary-physical 两份 VersionInfo 强制到当前 SDK、数据库版本和 fingerprint，并返回 `false`。PMS 用 `mFirstBoot = !readLPw(...)`得到 first boot。这是一个具体返回协议，不是“Settings 健康度”布尔值。

还有两个不对称失败点：

- 文件存在但找不到任何起始标签时也返回 `false`，却没有执行 `forceCurrent()`；新 Settings 实例的 `getInternalVersion()`只做 map lookup，随后 PMS 对 `ver.fingerprint`的直接访问可因 `ver == null`立刻抛 `NullPointerException`；
- 进入解析后的 `XmlPullParserException`或 `IOException`会被记录，随后仍继续连接 pending shared user、读用户态和 runtime permissions，最后可能返回 `true`。读取没有事务回滚，已经装入的部分状态可以留下。

所以“有解析错误就自动当首次开机重建”是错误模型；“backup 总能救主文件”也过强。

### 练习 3：推演四种 Settings 磁盘状态

分别推演：仅 main、backup 与 main 同时存在、两者都不存在、仅 main 但文件无起始标签。记录选了哪个文件、是否删除另一个、是否 `forceCurrent()`以及返回值。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (mBackupSettingsFilename.exists()) {" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "str = new FileInputStream(mBackupSettingsFilename);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mSettingsFilename.delete();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "findOrCreateVersion(StorageManager.UUID_PRIVATE_INTERNAL).forceCurrent();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "No start tag found in package manager settings" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "catch (XmlPullParserException e) {" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mReadMessages.append(\"Read completed successfully: \"" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mFirstBoot = !mSettings.readLPw(mInjector.getUserManagerInternal().getUsers(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

四种结果可直接列成表：

| 磁盘状态 | 读取与删除 | 版本补全 | `readLPw()`结果 |
|---|---|---|---:|
| 仅 main、内容正常 | 读 main，不删另一个 | 不 `forceCurrent()` | true |
| backup + main、backup 可打开且正常 | 读 backup，并调用删除 main | 不 `forceCurrent()` | true |
| 两者都不存在 | 无文件可读 | internal 与 primary-physical 都 `forceCurrent()` | false |
| 仅 main、无起始标签 | 读 main，不删另一个 | 不 `forceCurrent()` | false，随后可能在 `ver.fingerprint`解引用处崩溃 |

backup+main 的关键不是比较新旧时间，而是 backup 能否打开；“没有文件”和“有文件却无起始标签”虽都返回 false，却绝不安全等价。

## 6. XML 中的 `userId`其实是 appId；多用户状态在另一张账

`packages.xml`有一个极易误读的历史命名：普通包的 `<package userId="...">`写的是 `pkg.appId`，共享身份的包写 `<package sharedUserId="...">`，也仍是 appId。完整 Linux UID 要到具体 Android user 下再由 `UserHandle.getUid(userId, appId)`组合，不能把 XML 属性直接当成 user 10、user 11 这样的用户号。

PMS 在读账前先放入 system、phone、shell、networkstack 等内置 shared-user 记录。共享包反序列化时仍先进入 `mPendingPackages`；读完整个 XML 后，代码按 shared appId 查询 `SharedUserSetting`，命中才设置 `p.sharedUser`、复用其 appId 并加入 Settings。命中普通 setting 或完全缺失都会记录坏账，不会静默分配一个新 shared UID。

shared UID 共享的是 Linux 身份和 shared-user 权限态，不会把两个包合成同一份每用户开关。每个 `PackageSetting`仍分别保存各 user 的 installed、stopped、enabled 等状态。

全局节点和引用连接完后，Settings 才读用户账：若旧式 stopped 文件存在，只迁移 user 0；否则逐用户读 restrictions。某个 restrictions 文件完全缺失时，当前 `mPackages`中的所有包都会被显式设为 `installed=true`、`stopped=false`等默认值，不区分 system/data；文件中的未知包名只被跳过，不能凭一条用户记录复活全局不存在的包。

runtime permissions 也由 Settings 内部的 persistence 接口同步读取。r48 首选 permission APEX 的每用户 device-protected 文件并用 `AtomicFile`；主文件不存在才读 `/data/system/users/<id>` 的旧文件，随后安排迁移写。它不是由 `packages.xml`一次性恢复的字段。

### 练习 4：把 appId、shared user 与 Android user 拆开

查明普通包、共享包各写哪个 XML 属性；再找到 pending 引用连接、restrictions 和 permission APEX 文件。回答：两个 shared-UID 包是否必然拥有相同的 `installed(userId)`？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "serializer.attribute(null, \"userId\", Integer.toString(pkg.appId));" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "serializer.attribute(null, \"sharedUserId\", Integer.toString(pkg.appId));" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mPendingPackages.add(packageSetting);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "p.appId = sharedUser.userId;" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "readPackageRestrictionsLPr(user.id);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "true  /*installed*/," "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mRuntimePermissionsPersistence.readStateForUserSyncLPr(user.id);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "ApexEnvironment.getApexEnvironment(APEX_MODULE_NAME);" "frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java"
grep -n -F "return new File(dataDirectory, RUNTIME_PERMISSIONS_FILE_NAME);" "frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java"
```

答案是否定的。shared user 连接决定共享 appId/权限载体；installed 等仍从各自 `PackageSetting`的每用户状态读取。

## 7. first boot、真实 OTA、SDK 变化与 mock upgrade 是四个开关

读账之后，PMS 从 internal VersionInfo 取旧 fingerprint：`mIsUpgrade`只比较它与当前 `Build.FINGERPRINT`。`sdkUpdated`则比较旧 SDK 与当前 SDK，用于权限更新；同 API 级 OTA 可以 fingerprint 变化而 SDK 不变。

正常磁盘状态可先用三行表理解：

| 场景 | `mFirstBoot` | `mIsUpgrade` |
|---|---:|---:|
| 主、备都不存在且版本被 `forceCurrent()` | true | false |
| 健康旧账、fingerprint 不同 | false | true |
| 健康旧账、fingerprint 相同 | false | false |

畸形文件不保证落入这张安全表，因为第 5 节的返回值与 VersionInfo 建立并不总是绑定。

四个开关的直接消费者要分开看：

| 开关 | 来源 | 本章关键消费者 |
|---|---|---|
| `mFirstBoot` | `!readLPw()` | `SCAN_FIRST_BOOT_OR_UPGRADE`；非 only-core 时初始化默认 preferred/domain |
| `mIsUpgrade` | 旧 fingerprint 与当前值不同 | 同一 scan flag、pre-N/pre-M 等迁移分支、非 only-core 的全量 code-cache 清理 |
| `sdkUpdated` | 旧 SDK 与当前 SDK 不同 | `updateAllPermissions()`的 SDK 变化参数 |
| `isDeviceUpgrading()` | `mIsUpgrade`或 `persist.pm.mock-upgrade` | 扫描前包名快照/用户类型白名单、版本变化包的 profile 清理 |

所以 mock-upgrade 不只影响快照和白名单，也会让 `maybeClearProfilesForUpgradesLI()`清除版本变化包的 profiles；但它不会自动触发所有真实 OTA 分支：`SCAN_FIRST_BOOT_OR_UPGRADE`只看 `mIsUpgrade || mFirstBoot`，全量 code-cache 清理只看 `mIsUpgrade && !mOnlyCore`。

`mExistingPackages`也不是扫描调和的分类器。它仅在 `isDeviceUpgrading()`为真时，于扫描前复制 Settings 中的包名；直接消费者是构造返回后的 `installWhitelistedSystemPackages()`。OTA 应用用户类型白名单时，它避免把升级前已经存在的系统包当作“本次新增、可以卸载”。这份集合一直保留到 `systemReady()`末尾才清空。

## 8. 系统分区和活动 APEX 先被投影成带身份的扫描目录

r48 的静态 `PackagePartitions`按 increasing specificity 排列：

| 顺序 | 根 | priv-app | overlay | 额外 scan flag |
|---:|---|---:|---:|---|
| 1 | `/system` | 有 | 无 | 无分区附加位 |
| 2 | `/vendor` | 有 | 有 | `SCAN_AS_VENDOR` |
| 3 | `/odm` | 有 | 有 | `SCAN_AS_ODM` |
| 4 | `/oem` | 无 | 有 | `SCAN_AS_OEM` |
| 5 | `/product` | 有 | 有 | `SCAN_AS_PRODUCT` |
| 6 | `/system_ext` | 有 | 有 | `SCAN_AS_SYSTEM_EXT` |

PMS 先把这些静态项放入 `mDirsToScanAsSystem`，再追加活动 APEX 对应的扫描项，而不是按继承分区插回静态优先级位置。updatable APEX cache 由 `ArraySet`转成 `ArrayList`，多个活动 APEX 的相对次序也不应解释成稳定优先级。活动挂载路径本身不决定 vendor/product 身份：`resolveApexToScanPartition()`回看预安装 APEX 路径，继承匹配静态分区的目录能力与 scan flag，再加 `SCAN_AS_APK_IN_APEX`。

这里要保留一个 r48 实现边界：匹配使用绝对路径字符串 `startsWith()`并取静态列表中的第一个命中，不是带路径分隔符的 canonical containment。静态 `/system`又排在 `/system_ext`之前，所以 `/system_ext/apex/...`字符串会先命中 `/system`，存在丢失 `SCAN_AS_SYSTEM_EXT`的实现风险。诊断 APEX 身份时应查看最终 `ScanPartition`，不能仅凭设计意图推断。

APEX 还有两层不同扫描。在 `ApexManagerImpl`这条可更新 APEX 实现中，`scanApexPackagesTraced()`先解析 APEX 容器元数据；flattened APEX 实现的同名方法则是 no-op，活动目录由 `/apex`枚举。随后活动 APEX 内的 APK 才作为追加的 system scan partition 进入 overlay/priv-app/app 阶段。可更新实现的 only-core 容器 parse 中，只有 `ONLY_COREAPP_ALLOWED`错误会作为非 core APEX 被跳过，其他 parse 错误仍是致命异常。容器包与 APK-in-APEX 不应混叫成同一候选。

## 9. 目录阶段严格串行；overlay 的逆序不能套到所有扫描

双锁区建立 `SCAN_BOOTING | SCAN_INITIAL`，first boot 或真实 fingerprint upgrade 再加 `SCAN_FIRST_BOOT_OR_UPGRADE`。system candidate 另有 `PARSE_IS_SYSTEM_DIR`和 `SCAN_AS_SYSTEM`；parse flag 描述解析来源，scan flag 控制提交期身份，不能互换。

实际阶段顺序如下：

1. 调用 APEX 元数据扫描；可更新实现用共享 executor 解析并全部取回，flattened 实现为 no-op；
2. 对 `mDirsToScanAsSystem`倒序扫描各自 overlay 目录；
3. 扫 `/system/framework`，附加 `SCAN_NO_DEX | SCAN_AS_PRIVILEGED`；
4. 若包名 `android`仍不存在，立即抛致命异常；
5. 对同一目录表正序遍历，每个 partition 先 `priv-app`、再 `app`；
6. 基于已发现的系统包初始化 `OverlayConfig`；
7. `!mOnlyCore`时整理旧系统记录、updated-system 基线和 stub 候选；
8. 同一 `!mOnlyCore`分支再扫描 `/data/app`。

“overlay 倒序”只精确描述那一条循环。普通 system app 循环是正序；活动 APEX partitions 又追加在静态项之后，所以不要把所有目录阶段概括成“统一按高优先级到低优先级”。每次 `scanDirLI()`会先收齐本目录结果再返回，下一目录不会与它跨阶段并发解析。

### 练习 5：从调用顺序还原完整扫描阶段

把下列命中按实际执行顺序编号，并指出哪一条循环倒序、哪一条循环正序、`/data/app`受哪个条件控制。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "mApexManager.scanApexPackagesTraced(packageParser, executorService);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "for (int i = mDirsToScanAsSystem.size() - 1; i >= 0; i--) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(partition.getOverlayFolder(), systemParseFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(frameworkDir, systemParseFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (!mPackages.containsKey(\"android\")) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "for (int i = 0, size = mDirsToScanAsSystem.size(); i < size; i++) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(partition.getPrivAppFolder(), systemParseFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(partition.getAppFolder(), systemParseFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mOverlayConfig = OverlayConfig.initializeSystemInstance(" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(sAppInstallDir, 0, scanFlags | SCAN_REQUIRE_KNOWN, 0," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

命中顺序就是：APEX 元数据入口（可更新实现解析容器，flattened 实现 no-op）→ 倒序 overlay 循环及其目录调用 → framework → 必需的 `android`检查 → 正序 partition 循环中的 priv-app、app → `OverlayConfig` → `/data/app`。只有 overlay 循环倒序，普通 system 目录循环正序；列表中的旧系统整理与 `/data/app`都受 `!mOnlyCore`控制。每个目录内部可并行 parse，但调用线程取满该目录结果后才进入下一阶段。

## 10. ParallelPackageParser 并行的是 parse，不是全局提交

`scanDirLI()`先 `listFiles()`，过滤 APK/目录并排除 staging 名称，然后把本目录候选提交给一个最多四线程、foreground priority 的 executor。每个工作项只构造 `ParseResult`并放入容量 30 的完成队列。30 限制的是已完成但尚未被调用线程消费的 result 数，不是 executor 的待执行任务总量。

调用线程按完成顺序 `take()`，所以体积小的后提交包可以先返回；目录枚举顺序也没有被当作承诺。每个成功 result 随后在调用线程串行进入 `addForInitLI()`。`mPackages`、Settings、组件索引、签名和 shared UID 并不是四条 parse 线程同时提交。合法镜像不应在同一目录制造重复包；若人为制造 duplicate，先完成者会先改变全局状态，不能再声称冲突结果与调度顺序无关。

普通 APK 目录还有一条容易被“并行解析”遮住的边界：这里的 parse flags 没有要求 worker 收集证书，`addForInitLI()`稍后在调用线程执行 `collectCertificatesLI()`。可更新 APEX 实现的容器元数据扫描则显式传 `PARSE_COLLECT_CERTIFICATES`，可在它自己的 parse worker 内收集；flattened 实现没有这一步。不能把 APEX 的例外反推给所有 APK。

失败语义分三层：

- `PackageParserException`成为该候选的安装错误；
- parse 工作线程出现其他 `Throwable`，调用线程抛 `IllegalStateException`；
- 非 system 扫描的候选只要 parse/scan 失败，`removeCodePathLI()`就可能删除无效 data code path；只读 system 镜像不会走这条删除分支。

所有目录完成后，PMS 关闭 parser 并 `shutdownNow()`共享 executor；返回的 queued、尚未开始任务列表非空会被视为启动一致性错误。空列表本身不证明没有 running task；真正的完成边界来自 APEX 与每个 `scanDirLI()`此前都按各自提交数取满结果。共享 executor 的生命周期长，不等于目录之间形成跨阶段 pipeline。

### 练习 6：证明“最多四线程”没有变成“四路提交”

找出 result queue、submit、take、`addForInitLI()`和 data 删除点。回答：日志的完成顺序为何可以变化，Settings 的提交线程为何仍是调用 `scanDirLI()`的线程？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "private static final int QUEUE_CAPACITY = 30;" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "private static final int MAX_THREADS = 4;" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "private final BlockingQueue<ParseResult> mQueue = new ArrayBlockingQueue<>(QUEUE_CAPACITY);" "frameworks/base/services/core/java/com/android/server/pm/ParallelPackageParser.java"
grep -n -F "parallelPackageParser.submit(file, parseFlags);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ParallelPackageParser.ParseResult parseResult = parallelPackageParser.take();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "addForInitLI(parseResult.parsedPackage, parseFlags, scanFlags," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "throw new IllegalStateException(\"Unexpected exception occurred while parsing \"" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "removeCodePathLI(parseResult.scanFile);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "List<Runnable> unfinishedTasks = executorService.shutdownNow();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

完成队列决定 result 被取出的先后，不授予 worker 修改全局包世界的职责；串行 `addForInitLI()`才是本章讨论的认定入口。

## 11. `/data/app`必须先通过旧路径账，updated system app 才有例外

system 扫描结束后，PMS 已知道哪些基础系统包仍存在、哪些候选形成了 system/data 双版本关系。`mExpectingBetter`不只覆盖“用户曾更新预装包”：它也覆盖 OTA 新增 system package 与此前普通 `/data`同名包相遇、签名能力允许且 data 版本不低于 system 的路径。`addForInitLI()`会隐藏新 system candidate、建立 disabled-system 基线；稍后的系统账遍历再把基础路径放进 `mExpectingBetter`。既有 updated-system 关系也只在 system candidate 没有胜过 data 版、仍应等待 data 时进入这张表；system 版本更高则先清理 data 代码并转入采用 system 的后续 scan/reconcile/commit 路径，后段仍可能失败，不能提前宣告最终胜出。

随后 `/data/app`使用 `SCAN_REQUIRE_KNOWN`。普通候选必须已在 Settings 中存在，并且本次 code path 同时等于账本的 code/resource path；陌生包或路径漂移不是靠开机扫目录自动安装。唯一显式放宽是包名已在 `mExpectingBetter`中，此时跳过 known-path 要求，让 data 更新版有机会接替系统基线；后面的签名、版本和 scan/reconcile 检查并没有因此全部取消。

若 data 更新版成功进入 `mPackages`，它成为活动版本，disabled-system 记录继续保存可恢复的 system 基线。若 data parse/scan 失败，非 system 分支可先删除坏 code path；最终 `mExpectingBetter`发现包仍未出现，就反向查基础路径原属哪个 partition、是否 priv-app，重建完整 parse/scan flags，enable system setting 并重扫基础 APK。

这个回退也不是无条件成功：基础路径不落在已知 `app/priv-app`时会被忽略，重扫本身也可能失败。`mExpectingBetter`表达“安排一条恢复尝试”，不是最终包必然存在的证明。

### 练习 7：推演“系统基线 + 坏 data 更新版”

沿着 expecting、data known-path、无效路径删除和 system rescan 四段回答：最终恢复时为何必须重新找 partition，而不能只补一个 `SCAN_AS_SYSTEM`？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "mExpectingBetter.put(ps.name, ps.codePath);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (scanSystemPartition && !isSystemPkgUpdated && pkgAlreadyExists" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "shouldHideSystemApp = true;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSettings.disableSystemPackageLPw(parsedPackage.getPackageName(), true);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanDirTracedLI(sAppInstallDir, 0, scanFlags | SCAN_REQUIRE_KNOWN, 0," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "if (mExpectingBetter.containsKey(pkg.getPackageName())) {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "throw new PackageManagerException(INSTALL_FAILED_PACKAGE_CHANGED," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "removeCodePathLI(parseResult.scanFile);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "Expected better " "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSettings.enableSystemPackageLPw(packageName);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "rescanFlags = systemScanFlags | SCAN_AS_PRIVILEGED" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "scanPackageTracedLI(scanFile, reparseFlags, rescanFlags, 0, null);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
```

因为 fallback 后的 vendor/product/system_ext 与 privileged 身份会影响权限和策略；仅标 system 会丢掉基础版本原来的分区语义。

## 12. OTA 删除基础包与 stub 是另一组收尾分支

旧账中的 system package 在新镜像完全消失时，要区分有没有 disabled-system 基线：

- 没有 disabled 记录，说明普通旧 system 包已被镜像删除；PMS移除 setting、撤掉残余权限，代码和数据由后续调和清理；
- 有 disabled 记录但基线 code path/pkg 已消失，包名进入 `possiblyDeletedUpdatedSystemApps`。data 版若仍存在，PMS先删 disabled 记录，再移除已按旧 system 身份扫描的包，随后以普通 data flags 重扫，从而撤销 system 特权；data 版也不存在则删除剩余包数据。

这和 `mExpectingBetter`的方向相反：后者是“system 基线还在，data 更新没来就退回 system”；前者是“system 基线已经没了，data 版若在就降为普通包”。把二者都叫“updated system app 回退”会丢掉安全含义。

stub system app 又在这两类关系确定后最后处理。stub 是随镜像提供的精简占位包，启动调用受 `!mOnlyCore`控制，并没有 `mFirstBoot`条件，尽管方法说明以首次开机描述用途。三种失败/恢复位置不能合成一套统一回滚：

| 位置 | 活动 system stub | 当前启动的处理 | 后续含义 |
|---|---|---|---|
| 解压在 `disableSystemPackageLPw()`前失败 | 仍在 | user 0 被设为普通 `DISABLED` | 预过滤只跳 `DISABLED_USER`，下次仍可能尝试 |
| 已 disable/remove，data 包扫描再失败 | 已从活动集合移除 | 删除坏 data path 并记失败 | 启动 helper 不在该分支显式重装 system stub |
| 交互式 `enableCompressedPackage()`失败 | 走另一条控制流 | 有恢复 stub 的专门代码 | 不能反推启动 helper 也会同样恢复 |

源码要求 stub 最后处理，但“发现 stub”与“完整实现可用”之间仍隔着多阶段提交。

最后 `mExpectingBetter.clear()`只表示这张构造期临时表不再需要；真正最终事实要看 `mPackages`、Settings 和后续写入。

## 13. 扫描结束后仍要重算库、权限、shared UID 与 app data

候选集合稳定后，PMS 才为所有客户端更新 shared-library path，并逐个修正 SharedUserSetting 的 ABI、dex 与 SEInfo。usage/compiler stats 是依附信息，不是包存在事实。`BOOT_PROGRESS_PMS_SCAN_END`在这之后记录，但权限更新、app-data、主账写和构造尾部还没结束。

权限更新使用独立的 `sdkUpdated = oldSdk != currentSdk`，不能拿 fingerprint upgrade 代替。非 only-core 且 first boot 或 pre-M upgrade 时，才会建立默认 preferred apps/domain verification。

app data 先同步处理 internal private volume、system user：文件级加密（FBE）设备只准备 device-encrypted（DE）存储，非 FBE 同时准备 DE 与 credential-encrypted（CE）存储。这里传给 `reconcileAppsDataLI()`的局部参数固定为 `onlyCoreApps=true`，不是全局 `mOnlyCore`；正常启动也用它让同步阶段先处理 core app，并返回已扫描但延后的非 core 包名。随后 PMS 在仍持 `mInstallLock → mLock`时，把一枚 `mPrepareAppDataFuture`提交给 SystemServer init pool，而且这发生在清 code cache、`writeLPr()`和 `PMS_READY`之前。

这枚 Future 是一条偏序支线：

- worker 的 `fixupAppData()`没有先获取 Java `mInstallLock`，可能在构造尚未返回时就执行；
- 这次 fixup 无条件传 DE|CE flags，而逐包同步/延后准备在 FBE 设备上使用前面算出的 DE-only `storageFlags`；
- 逐包阶段先短取 `mLock`读 setting，再单独取 `mInstallLock`准备目录，通常要等构造释放大锁；
- deferred 列表为空时，它可能很早完成；不为空时也可能在 Binder 发布前或后完成；
- 范围只是 internal volume、system user 的这批初始工作，不覆盖未来用户或后挂载卷；
- `fixupAppData`与单包 `createAppData`的若干 Installer 失败会记录后继续或尝试恢复，因此 Future 成功返回不等于每个目录操作都成功。

真实 fingerprint upgrade 且非 only-core 时还会清 app code cache，却保留 ART profiles。这个条件仍是 `mIsUpgrade`，不是 mock-upgrade 或单纯 SDK 变化。

## 14. `writeLPr()`只原子化自己的主文件协议，不原子化整个包世界

构造内 `Settings.writeLPr()`不用 `AtomicFile`写全局主账，而是手工维护 backup：

1. 有 main 且无 backup：把 main rename 成 backup；rename 失败就直接返回，不写新主账；
2. main 与旧 backup 同时存在：保留更老 backup，删除当前 main；
3. 写新 main，`flush()`并对文件 `FileUtils.sync()`；
4. 成功后删除 backup，再设置权限；主 XML 写入发生 `IOException`时删除部分 main，若旧主账此前已成功改名则 backup 仍保留；
5. 主账成功之后，才分别写 kernel mapping、JournaledFile 保护的 `packages.list`、所有用户 restrictions，并为 runtime permissions 安排异步写。

`writeLPr()`本身返回 `void`，rename 失败和被内部捕获的 `IOException`都不会给调用者一个“本轮成功”的布尔值；所以调用者看到它返回，只能证明这次调用结束。主文件的恢复协议很清楚，跨文件事务却不存在。主 backup 删除后，派生输出或某个用户文件仍可能失败；runtime-permission 写更只是 nominal 200ms 合并、从首个未写 mutation 起最多 2s 的调度窗口，`writeLPr()`不会等待它落盘。

`packages.list`还是一个有损派生视图：无 parsed package、无 data path、path 含空格的项会被跳过；它写 appId、user-0 data path、seinfo、活跃用户聚合 GID、profileable 与 version，不检查 user 0 当前是否 installed，也不逐用户列行。它缺一项不能反推 `packages.xml`中没有该 setting，PMS 启动也不读它恢复状态。

每用户 restrictions 有自己的 main/backup 协议。当前 runtime-permission 主实现则在 permission APEX 中用 `AtomicFile`；“Settings 只有一种写盘器”同样不成立。

### 练习 8：给四类文件分别画提交线

找到 main rename/sync/删 backup、三类后继文件协议，以及 kernel mapping 副作用。回答：`writeLPr()`返回前哪些调用在当前线程执行，哪一类只排队？`packages.list`为何不能与主 XML 共用同一提交点？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (!mSettingsFilename.renameTo(mBackupSettingsFilename)) {" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "FileUtils.sync(fstr);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mBackupSettingsFilename.delete();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "writeKernelMappingLPr();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "writePackageListLPr();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "writeAllUsersPackageRestrictionsLPr();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "writeAllRuntimePermissionsLPr();" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "JournaledFile journal = new JournaledFile(mPackageListFilename, tempFile);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "mRuntimePermissionsPersistence.writePermissionsForUserAsyncLPr(userId);" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
grep -n -F "private static final long WRITE_PERMISSIONS_DELAY_MILLIS = 200;" "frameworks/base/services/core/java/com/android/server/pm/Settings.java"
```

主 XML 成功后，kernel mapping、`packages.list`和每用户 restrictions 都由当前 `writeLPr()`调用同步进入各自写函数；runtime permissions 只逐用户安排异步写，当前调用不等其落盘。主 XML、每用户 XML、JournaledFile 与 permission APEX AtomicFile 是四套文件提交协议，kernel mapping 另是内核副作用；调用顺序能建立先后，不能把它们提升成一次跨文件原子提交。

## 15. `PMS_READY`、Binder、`systemReady()`与第三方 phase 不是同一个 ready

构造内 `Settings.writeLPr()`调用返回后立刻记录 `BOOT_PROGRESS_PMS_READY`；由于该 API 没有成功返回值，这个 event 连“新主账一定落盘”也不能证明。构造随后还确定 installer/verifier/permission-controller 等角色包名，建立 InstantApp 与 PackageInstaller 对象，加载 DexManager，退出双锁，创建 ModuleInfoProvider，uncork package-info cache 并 GC。这个 event 不是构造函数返回通知。

构造返回后，白名单处理可改变每用户内存态并排十秒延迟写，然后 `package`与`package_native`被顺序注册。普通 Java Binder 客户端看到 `package`时，启动扫描和构造确实结束、白名单内存调整也已运行；但第二个 `IPackageManagerNative` Binder 端点、延迟磁盘写以及 `main()`返回仍各有自己的下一行。

SystemServer 启动更多服务后直接调用 PMS `systemReady()`。方法先把 volatile `mSystemReady`置 true，之后才注册 instant-app settings observer、处理 carrier/SKU app、清理失效 preferred activity、更新权限、注册 storage listener、调和 stale users 与孤儿 `/data/app`代码路径、通知子组件，并把 staged-session restore 放在末尾。标志不回滚；即使后段仍运行或抛错，其他线程也可能已经观察到 true。只有正常走到最后的 `mExistingPackages = null`，这次同步方法才完整返回；staged restore 还可能排队或恢复 pre-reboot verification，而实际处理受 `BOOT_COMPLETED`后的 ready 门控制，故这个返回仍不代表所有 staged 工作闭合。

初始 app-data Future 早已在构造中提交。更晚进入 AMS `systemReady()`时，AMS 自己的 `mSystemReady=true`、`mProcessesReady=true`都先于 `goingCallback.run()`，所以 app-data join 不是这两个 AMS 标志的前置。callback 又先推进 `PHASE_ACTIVITY_MANAGER_READY`、启动 SystemUI 并通知多项网络服务 ready，才无超时地 `Future.get()` join app-data；中断或 Future 异常会向上抛，成功返回后才把引用清空。许多 Installer 失败已在任务内部被记录，所以 join 证明“任务终止”，不证明“每项成功”。若 `!mOnlyCore`且 WebViewUpdateService 存在，callback 还会 join 已提交的 WebView preparation，随后才发送 `PHASE_THIRD_PARTY_APPS_CAN_START`；这个 phase 后仍要启动 NetworkStack、Tethering 等，callback 返回后 AMS 才继续初始用户/应用。因此 phase、callback 返回和 AMS 两个 ready 标志都不是同一个点。

### 练习 9：把所有 ready 放到一条偏序线上

分别标出主账 event、白名单、两个 Binder、PMS 的早置位与正常返回、staged 收尾、AMS 两个 ready 标志、app-data join、条件式 WebView join 和第三方 phase。再回答：哪两个点之间存在“`IPackageManager`已可查、`IPackageManagerNative`尚不可查”的窗口？哪一个 Future 可能在 Binder 前就完成？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "EventLog.writeEvent(EventLogTags.BOOT_PROGRESS_PMS_READY," "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "m.installWhitelistedSystemPackages();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ServiceManager.addService(\"package\", m);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "ServiceManager.addService(\"package_native\", pmn);" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mPrepareAppDataFuture = SystemServerInitThreadPool.submit(() -> {" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSystemReady = true;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mInstallerService.restoreAndApplyStagedSessionIfNeeded();" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mExistingPackages = null;" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mSystemReady = true;" "frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java"
grep -n -F "mProcessesReady = true;" "frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java"
grep -n -F "if (goingCallback != null) goingCallback.run();" "frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java"
grep -n -F "mSystemServiceManager.startBootPhase(t, SystemService.PHASE_ACTIVITY_MANAGER_READY);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "ConcurrentUtils.waitForFutureNoInterrupt(mPrepareAppDataFuture, \"wait for prepareAppData\");" "frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java"
grep -n -F "mPackageManagerService.waitForAppDataPrepared();" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "if (!mOnlyCore && mWebViewUpdateService != null) {" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "ConcurrentUtils.waitForFutureNoInterrupt(webviewPrep, WEBVIEW_PREPARATION);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "mSystemServiceManager.startBootPhase(t, SystemService.PHASE_THIRD_PARTY_APPS_CAN_START);" "frameworks/base/services/java/com/android/server/SystemServer.java"
grep -n -F "NetworkStackClient.getInstance().start();" "frameworks/base/services/java/com/android/server/SystemServer.java"
```

第一问的窗口位于两次 `ServiceManager.addService()`之间；第二问是构造中段提交的 `mPrepareAppDataFuture`。后来的 wait 只固定 join 点，不固定实际完成发生在 Binder 前还是后。

## 16. 用五本账和九个完成点排障，再交给第 252 章

遇到“包丢了、UID 变了、系统更新回退、开机卡在 ready”时，按责任线取证：

| 现象 | 第一检查点 | 第二检查点 | 不应直接下的结论 |
|---|---|---|---|
| 包在磁盘却未注册 | parse result 与 `addForInitLI()`错误 | data 包的 known path、only-core、签名/调和 | 目录存在就应自动安装 |
| appId 或 shared UID 异常 | `packages.xml`的 `userId/sharedUserId` | pending shared-user 连接与 appId 冲突 | XML `userId`是 Android 多用户号 |
| data 系统更新消失 | disabled-system 与 `mExpectingBetter` | data 删除日志、fallback partition flags | 任意失败都保留 data 版特权 |
| OTA 后基础包消失 | `possiblyDeletedUpdatedSystemApps` | data 版是否按普通 flags 重扫 | 与 expecting-better 是同一方向 |
| `packages.list`缺项 | parsed metadata、data path 与 JournaledFile | 主 Settings 和 `mPackages` | 派生表缺项等于包不存在 |
| Binder 可查但状态尚未耐久 | 构造内 `writeLPr()` | 白名单后的十秒延迟写 | Binder 注册是全文件提交点 |
| `mSystemReady`为 true 仍有工作 | `systemReady()`当前行 | staged/permission/storage 后置动作 | volatile 标志是方法完成 Future |
| PMS `systemReady()`已返回仍有支线 | 末行 `mExistingPackages=null` | app-data join、白名单延迟写、staged 验证队列/异步执行 | 同步返回等于所有包相关工作闭合 |
| 第三方 phase 尚未推进 | app-data Future | WebView Future 与 AMS callback | PMS Binder 可用即可启动所有代码 |

一条包的启动认定可最后压成：旧 `PackageSetting`提供连续身份 → 当前 system/APEX/data 文件产生 `ParsedPackage` → 调用线程按分区与启动 flags 进入 `addForInitLI()` → updated-system/OTA/stub 规则选择活动版本 → shared library、ABI、SEInfo、权限与用户态收敛 → 主账及派生账分别写出 → Binder 暴露当前内存世界。任何单点都不是整条链的替代品。

本章停在“全局启动世界已经建立到哪些边界”。第 252 章进入一枚候选内部，继续追 `PackageParser2`、`ScanRequest`、`ReconcileRequest`、签名/shared UID 裁决和最终 `PackageSetting`、组件索引提交。
