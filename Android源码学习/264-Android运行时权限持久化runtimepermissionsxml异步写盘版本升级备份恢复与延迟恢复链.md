# Android运行时权限持久化：runtime-permissions.xml、版本升级、备份恢复与延迟恢复链

## 1. 先把三种文件和六个完成点分开

本文以 AOSP android-11.0.0_r48 为边界。第263章停在运行时权限、flags 与 AppOps 已经改变；本章继续回答两个问题：这些改变怎样跨重启保存，以及怎样经 BackupManager 搬到恢复后的用户环境。

源码里同时存在三种性质完全不同的权限数据：

| 数据 | 所在进程与典型位置 | 目的 | 是否完整镜像 |
| --- | --- | --- | --- |
| 本机 runtime 权限文件 | system_server 通过 Permission APEX System API 写入每用户 DE 目录 | 下次启动恢复本机 runtime grant 与 flags | 接近本机内存快照，但不含 install permission |
| BackupManager 权限负载 | PermissionController 选择性生成，交给备份传输 | 跨设备或重置后迁移用户有意义的选择 | 不是；只保留少数字段和状态 |
| delayed restore 文件 | PermissionController 每用户 DE 私有 files 目录 | 暂存“目标包尚未安装”的备份条目 | 只是尚未应用的备份子集 |

还要把六个完成点分开：内存已改、写消息已排队、快照已取得、文件提交已尝试、下次启动成功读回、备份恢复端重新解释成功。任何一个方法返回，都不能自动证明后面五项已完成。

本章的主线因而不是“找一个 XML”，而是追踪状态在哪个边界被压缩、谁确认了哪一步，以及失败后下一次重试是否仍然存在。

## 2. 内存是当前裁决源，写入口却按变更类型分流

运行中的授权裁决以 system_server 里的 PermissionsState 为准。普通 PackageSetting 持有自己的状态；shared UID 成员则经 PackageSetting.getPermissionsState 返回同一个 SharedUserSetting 状态。写 runtime 快照时，Settings 遍历全局已知包：非 shared-user 包按 package name 写，shared UID 成员包跳过，再按 shared-user name 写一份共享状态。这里没有按“该用户已安装”过滤，所以准确说法是“所有已知非共享包和所有 shared user”，不是“该用户当前安装包”。

入口也不是一条统一通道：

- 单独 grant 的 PermissionCallback 调用 writeSettings(true)，先调度整个 Settings 写；
- revoke 被视为关键变化，调用 writeSettings(false)，同步写整个 Settings；
- flags 或批量 runtime 状态更新走 writePermissionSettings，只处理指定用户的 runtime 权限文件，并由 sync 参数决定是否等待写尝试；
- Settings.writeLPr 在 packages.xml 等主设置写完后，又为所有用户调度 runtime 权限写。

因此 packages.xml 与 runtime-permissions.xml 是相关但不同的账。install permission 仍属于主包设置；每用户 runtime grant 和 flags 才进入本章的 runtime 文件。一次 UI 操作通常既改 grant 又改 flags，所以可能同时触发多条可合并的写路径。

### 练习 1：画出四种写入口和shared UID归属

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mRuntimePermissionsPersistence = new RuntimePermissionPersistence(mLock);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'if (packageSetting.sharedUser == null) {' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'sharedUserPermissions.put(sharedUserName, permissions);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mPackageManagerInt.writeSettings(true);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(false);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writePermissionSettings(userIds, !sync);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'writeAllRuntimePermissionsLPr();' frameworks/base/services/core/java/com/android/server/pm/Settings.java
```

分别为“孤立 grant、revoke、flags 更新、shared UID 中一个成员变化”标出最终写到哪一类节点。shared UID 的答案应落在 shared-user，而不是成员包节点。

## 3. Permission APEX文件位于每用户DE目录，schema保存grant与flags

Android 11 把持久化实现放进 com.android.permission APEX。system_server 直接调用 RuntimePermissionsPersistence System API；这是一条模块边界，不是一次 Binder 往返。接口明确声明 read、write、delete 都执行同步 I/O，所谓“异步写”是外层 Settings 把这次同步 I/O 放到 BackgroundThread Handler 上。

实现通过 ApexEnvironment 取得 Permission APEX 的每用户 Device Encrypted 数据目录，再拼接 runtime-permissions.xml。典型形态是：

`/data/misc_de/<userId>/apexdata/com.android.permission/runtime-permissions.xml`

DE 使它能在凭据解锁前读取；ApexEnvironment 还把这个目录纳入对应 APEX 的数据回滚语义。它不是 `/data/system/users/<id>/` 的旧文件，也不是 PermissionController 应用自己的 delayed 文件。

根节点保存 runtime 数据库 version 和可选 fingerprint。其下是 package 与 shared-user；每个 permission 只有 name、granted 和十六进制 flags。快照为每个已知非共享包、每个 shared user 都放入 map，所以没有 runtime permission 的对象也会形成空节点。空节点表示“快照知道它且列表为空”，缺节点则可能触发 missing 语义，两者不能混用。

denied 项仍需保存，因为 flags 可以表达 USER_SET、USER_FIXED、POLICY_FIXED、ONE_TIME、AUTO_REVOKED 等形成原因。只记 granted 项会丢掉“用户明确拒绝”和“尚未作决定”的区别。

### 练习 2：从路径构造走到XML字段

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return new RuntimePermissionsPersistenceImpl();' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistence.java
grep -n -F 'This will perform I/O operations synchronously.' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistence.java
grep -n -F 'Environment.getDataMiscDeDirectory(user.getIdentifier()), APEX_DATA,' frameworks/base/core/java/android/content/ApexEnvironment.java
grep -n -F 'File dataDirectory = apexEnvironment.getDeviceProtectedDataDirForUser(user);' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'private static final String RUNTIME_PERMISSIONS_FILE_NAME = "runtime-permissions.xml";' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'serializer.attribute(null, ATTRIBUTE_VERSION, Integer.toString(version));' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'serializer.attribute(null, ATTRIBUTE_GRANTED, Boolean.toString(' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'serializer.attribute(null, ATTRIBUTE_FLAGS, Integer.toHexString(' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
```

任选一个普通包、一个 shared UID 和一个没有 runtime 权限的包，写出它们在 map 与 XML 中的归属。再解释为何“空节点”和“完全没有节点”会让读取端作出不同判断。

## 4. ONE_TIME在本机文件里被强制写成denied，但仍有提交窗口

本机 serializer 对 ONE_TIME 做了专门处理：只有内存 granted 且 flags 不含 FLAG_PERMISSION_ONE_TIME 时，磁盘 granted 才为 true；flags 本身仍原样写出。于是一次性授权成功提交后，重启读到的是“denied + ONE_TIME flag”，不会把已失去会话管理器的一次性 grant 当成永久授权复活。

这个保证必须带上“新快照已经成功提交”的前提。设磁盘旧文件保存永久 grant，用户刚把它改成 ONE_TIME，内存已经是临时授权，但异步写尚未落盘就掉电；下次启动仍可能读回旧的永久 grant。ONE_TIME 掩码修正的是新序列化结果，不会消除旧文件存在的崩溃窗口。

还要区分本机文件与备份负载：本机文件保留完整 flags，并特意把 ONE_TIME 的 granted 压成 false；PermissionController 的备份 schema 既不保存 ONE_TIME flag，也不采用这条掩码。两条链对同一状态的解释恰好不同，后文会看到它可在迁移时造成授权语义升级。

## 5. 10秒、200毫秒和2秒属于两层调度，不是落盘截止时间

Settings 的 runtime 写去抖按 userId 独立维护。第一次未写 mutation 记录 uptime 时刻 t0，向 BackgroundThread 投递 200 ms 后的消息，并标记该用户已有写任务。随后变化会移除旧消息，再投递 `min(200 ms, t0 + 2 s - now)`；记录的 t0 不随每次变化向后移动。因此连续高频变化最多把 Handler 的应执行时刻推到第一次未保存变化后的约 2 秒。

“约2秒”只约束消息投递时刻，不是磁盘完成上限。BackgroundThread 队列可能拥塞，Handler 取任务后还要取得全局锁、构造全量用户快照、序列化、sync 和 rename。任何一步都能继续延迟。

孤立 grant 还有上一层调度。onPermissionGranted 走 writeSettings(true)，PMS 首次 WRITE_SETTINGS 消息固定延迟 10 秒；Settings.writeLPr 完成主设置写后才为 runtime 文件再安排 200 ms/2 s 去抖。于是它可能先经历“10秒整库调度”，再进入 runtime 层。权限 UI 常伴随 flags 更新，后者会更早直接调度 runtime 文件，所以不能把所有 grant 都概括为至少等待 10 秒。

revoke 的 writeSettings(false) 会同步执行主 Settings 写，但 writeLPr 末尾的 writeAllRuntimePermissionsLPr 仍只调度各用户的 runtime 文件。因此 revoke 返回甚至不能证明 runtime XML 的写尝试已经结束；即便真正的同步 runtime 入口返回，APEX 实现也会吞掉写异常，只能证明尝试结束。

### 练习 3：手算两层去抖的时间线

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static final int WRITE_SETTINGS_DELAY = 10*1000;  // 10 seconds' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendEmptyMessageDelayed(WRITE_SETTINGS, WRITE_SETTINGS_DELAY);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private static final long WRITE_PERMISSIONS_DELAY_MILLIS = 200;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final long MAX_WRITE_PERMISSIONS_DELAY_MILLIS = 2000;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mLastNotWrittenMutationTimesMillis.put(userId, currentTimeMillis);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'final long writeDelayMillis = Math.min(WRITE_PERMISSIONS_DELAY_MILLIS,' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mHandler.sendMessageDelayed(message, writeDelayMillis);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'super(BackgroundThread.getHandler().getLooper());' frameworks/base/services/core/java/com/android/server/pm/Settings.java
```

令同一用户在 t=0、150、310、900、1900 ms 连续改 flags，逐次算出消息的新时刻。然后另画一条孤立 grant 时间线；不要把消息时刻写成 fsync 完成时刻。

## 6. 快照在锁内，I/O在锁外，AtomicFile却没有并发互斥

RuntimePermissionPersistence 构造时收到的 mPersistenceLock 就是 PMS 的全局 mLock。writePermissionsSync 先在该锁内删除 scheduled 标记，复制 version、fingerprint、全部普通包和 shared-user 的 runtime 状态，构造一个脱离 live PermissionsState 的快照；退出锁后才调用 APEX 实现的 writeForUser。异步 Handler 因而不会在慢 I/O 期间一直占住全局包锁。

同步入口有一个反直觉差异：PackageManagerInternal.writePermissionSettings 外层已经 synchronized(mLock)，再进入上述方法。Java 监视器可重入，所以快照阶段正常；但外层锁要等整个调用返回，实际同步 I/O 仍持有全局锁。

这两者组合出 r48 的并发缺口：

1. 旧 async 任务在锁内取得快照 A，退出锁并开始写文件；
2. 新 mutation 形成快照 B，sync 调用取得 mLock；
3. sync 写 B 时，不会等待已经离开 mLock 的 async I/O；
4. 两次 writeForUser 各自 new AtomicFile，却指向同一 base 与同一 `.new`。

AtomicFile 只保证单写者通过新文件、sync 和 rename 避免半文件提交，源码明确说它不提供文件锁，并要求调用者建立互斥。这里不能保证 B 最后胜出，可能发生覆盖、丢失或损坏；具体结果取决于 truncate、两个文件描述符写入和 rename 的时序，不能简化成固定的“旧快照必然覆盖新快照”。

错误契约也比方法名弱：writeForUser 捕获所有 Exception，记录 wtf，调用 failWrite，却不把失败传播给 Settings；async 在 I/O 前已清 scheduled，失败没有专门的自动重试。delete 直接 File.delete 并忽略返回值。即便同步调用返回，也只能证明写尝试结束，不能证明调用者收到 durable commit acknowledgment。

### 练习 4：证明AtomicFile不能修补两个writer

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'synchronized (mPersistenceLock) {' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mWriteScheduled.delete(userId);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mPersistence.writeForUser(runtimePermissions, UserHandle.of(userId));' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mSettings.writeRuntimePermissionsForUserLPr(userId, !async);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'AtomicFile atomicFile = new AtomicFile(file);' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'Atomic file does not confer any file locking semantics.' frameworks/base/core/java/android/util/AtomicFile.java
grep -n -F 'atomicFile.finishWrite(outputStream);' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'atomicFile.failWrite(outputStream);' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'getFile(user).delete();' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
```

画出 A、B 两个快照的锁区间和 I/O 区间，至少列举两种 `.new` 交错结果。关键结论不是猜谁最后写，而是证明源码没有建立单 writer 不变量。

## 7. 启动只在新文件缺失时回退旧格式，运行中重读还有锁缺口

正常启动先同步读 APEX DE 文件。只有 readForUser 因文件不存在返回 null，Settings 才尝试旧路径 `/data/system/users/<userId>/runtime-permissions.xml`，随后异步安排一次新格式写。迁移路径没有删除旧文件；若新格式写持续失败，后续启动仍可能反复从旧文件恢复。调试开关若只删除新文件，也可能让残留旧文件重新注入状态。

新文件存在但损坏则不是回退条件。XML 或普通 I/O 异常被包装为 IllegalStateException；version、flags 的整数转换还可能抛出未被该 catch 捕获的运行时异常。一个坏的新文件因此会阻断读取，而不是自动选旧副本。

成功解析后，reader 以当前 mPackages 与 mSharedUsers 为主表查同名节点：文件中的孤儿包不会创建 PackageSetting，未知权限被警告并跳过。当前对象缺节点时，非“升级到 R”路径会把其 PermissionsState 标成 missing；升级到 R 时暂不这样做。文件内明确存在的空节点则返回空列表，不等于 missing。

r48 还有一条运行中重读边界。pre-created user 转正式用户时，PMS 用 `synchronized (mPackages)` 锁住自己的包映射，却调用一个按注解应由 Settings.mLock 保护、会修改 Settings 包映射与权限状态的读取方法；两个 mPackages 不是同一张 map，这里确实拿错了监视器。非首次写期间 base 与 `.new` 并存时，AtomicFile.openRead 会把 `.new` 当作过期文件删除并读取旧 base，在途 writer 随后的 finishWrite 便可能失去提交对象；首次写若只有 `.new` 而没有 base，openRead 不删除 `.new`，却会因 base 不存在而返回 null 并转入 legacy 路径。即便把这里改成 mLock，也挡不住已经离开 mLock 的 async I/O，仍需要独立的文件互斥。读取 granted=false 时又只恢复 flags、不主动 revoke 已有内存 grant，因此这条方法不能被理解成带正确互斥的“完整替换式 reload”。

## 8. 四种version各管一层，fingerprint只是跨重启触发账

源码中至少有四种容易混淆的版本：

| 名称 | r48中的典型值 | 用途 |
| --- | --- | --- |
| runtime permission database version | 缺失映射为 -1，新用户从 0，最新为 8 | 决定 PermissionController 依次执行哪些数据迁移 |
| packages.xml 内部 SDK/database version | 随平台包设置演进 | 判断系统包设置与平台升级背景 |
| PermissionBackupHelper STATE_VERSION | 1 | BlobBackupHelper 自身的备份状态与压缩负载版本 |
| 备份 XML platform version | r48 仍写 Q，即 29 | restore 时判断 split permission 是否需要展开 |

extended fingerprint 也不是应用签名或文件摘要，而是 `Build.FINGERPRINT + "?pc_version=" + PermissionController versionCode`。启动扫描 PermissionController 后，Settings 将磁盘 fingerprint 与当前扩展值比较，填充每用户 mPermissionUpgradeNeeded；没有条目的查询默认 true。

更新 fingerprint 只把新字符串放进 mFingerprints 并安排异步写，不会把当前进程的 mPermissionUpgradeNeeded[user] 改成 false。PermissionPolicyService 停止用户时又只删 mIsStarted，所以同一 system_server 生命周期内 stop/start 会再次进入升级闸门。此时 version 往往已是 8，不再执行 0→8 的结构版本步骤，却仍会重新加载升级数据、为预装应用重放受限权限 upgrade whitelist，再启动 user-sensitive 更新并再次写 fingerprint；PMS 只在 systemReady 阶段做的默认授权不会因此重跑。重启后重新比较已落盘 fingerprint，才可能算出 false。

因此 fingerprint 是一种跨重启、至少一次式的触发账，不是本进程 latch，更不是“全部权限策略和 fsync 均成功”的提交标记。

### 练习 5：拆开version、fingerprint与内存闸门

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final int NO_VERSION = -1;' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsState.java
grep -n -F 'version = UPGRADE_VERSION;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'return mVersions.get(userId, INITIAL_VERSION);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'return Build.FINGERPRINT + "?pc_version=" + version;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'return mPermissionUpgradeNeeded.get(userId, true);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mPermissionUpgradeNeeded.put(userId,' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mFingerprints.put(userId, mExtendedFingerprint);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mIsStarted.delete(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'private static final int STATE_VERSION = 1;' frameworks/base/core/java/com/android/server/backup/PermissionBackupHelper.java
```

构造“version 已为8、fingerprint 已在内存更新、用户 stop 后再 start、system_server 尚未重启”的场景，解释为何仍通过 upgrade-needed 分支，以及哪些工作会重复。

## 9. 升级是有序但非事务的迁移，回调也不是最终提交

PermissionPolicyService 启动用户时先检查 upgrade-needed。PMS 的 DefaultPermissionGrantPolicy 已先为需要升级的用户补默认授权；名字相似的 PermissionController `performDefaultPermissionGrants()` 在 r48 实际为空，Controller 这一段的核心是 runtime 数据库迁移。

Policy 创建 future，请 PermissionController 执行 grantOrUpgrade，然后直接 `future.get()`。这里没有传超时时间；远端连接设施可能用自己的超时或失败结束请求，但本地代码注释所说的“we time out”并没有对应的 get(timeout)。Controller 则在 `GlobalScope.launch(IPC)` 中读取当前 version、加载一次升级所需数据，顺序应用 whitelist，再顺序应用 grants，最后才把 version 设为 8 并调用 onComplete。

迁移严格逐级前进：

| 区间 | 动作 |
| --- | --- |
| -1 → 0 | 标记从 P 升级 |
| 0 → 1 | 为 SMS 与 Call Log 的受限权限建立 upgrade whitelist |
| 1 → 2、2 → 3 | 保留的空迁移槽，实际逻辑移到后续阶段 |
| 3 → 4 | whitelist background location，并在内存模型中模拟 exemption 后续计算 |
| 4 → 5 | 保留槽 |
| 5 → 6 | whitelist storage 受限权限 |
| 6 → 7 | 仅 P 升级场景，在前台已授予且后台无 USER_SET、SYSTEM_FIXED、POLICY_FIXED、USER_FIXED 时扩展 background location |
| 7 → 8 | 仅非新用户，按 USER_SET、fixed 与现有 grant 门槛扩展 ACCESS_MEDIA_LOCATION |

它不是事务。某个 whitelist 或 grant 已应用后抛异常，不会回滚；version 可能未推进，onComplete 也没有 finally 兜底，随后可能由连接失败、进程崩溃或重启触发再跑。version setter 自身也只是更新内存并异步安排 runtime XML。

Controller 成功回调后，Policy 调用 updateUserSensitive；该方法只 postAsync，失败只记日志。Policy 不等待它便立刻更新 fingerprint，而 fingerprint 又异步落盘。所以“用户启动继续”证明的是 Controller grant/upgrade 回调成功，不证明 user-sensitive 完成，也不证明 version/fingerprint 已持久化。

随后 onStartUser 先把 mIsStarted 置为 true，再执行该用户的 Permission/AppOps 全量同步，最后才通知 onInitialized observer。于是 upgrade gate 返回、isStarted、AppOps 同步完成和 observer 初始化仍是四个顺序相邻但不同的完成点。

### 练习 6：验证迁移顺序和多个不同完成点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (packageManagerInternal.isPermissionUpgradeNeeded(userId)) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'future.get();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'private val LATEST_VERSION = 8' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/RuntimePermissionsUpgradeController.kt
grep -n -F 'GlobalScope.launch(IPC) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/RuntimePermissionsUpgradeController.kt
grep -n -F 'for (whitelisting in (preinstalledAppWhitelistings union upgradeWhitelistings)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/RuntimePermissionsUpgradeController.kt
grep -n -F 'for (grant in grants) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/RuntimePermissionsUpgradeController.kt
grep -n -F 'permissionManager!!.runtimePermissionsVersion = LATEST_VERSION' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/RuntimePermissionsUpgradeController.kt
grep -n -F 'permissionControllerManager.updateUserSensitive();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'packageManagerInternal.updateRuntimePermissionsFingerprint(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsStarted.put(userId, true);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'synchronizePermissionsAndAppOpsForUser(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'callback.onInitialized(userId);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
```

分别在第一个 whitelist 后、最后一个 grant 后、version 内存更新后注入异常，写出下次启动的可见状态。再标出 Policy 的 future、user-sensitive future 和 XML 写任务，三者不能合并成一个完成点。

## 10. BackupManager通过pipe取负载，30秒与两只60秒各有边界

SystemBackupAgent 以 `permissions` 键注册 PermissionBackupHelper；外层 BlobBackupHelper 的 STATE_VERSION 为 1。真正 XML 由 PermissionController 生成，system_server 的 PermissionManagerService 通过 PermissionControllerManager 和 pipe 跨进程取回 byte[]。

这条链有三只计时器和一处无超时等待。RemoteStream 为管道读写设置 30 秒 timeout；PermissionControllerManager 的 ServiceConnector request timeout 是 60 秒，覆盖它自己的连接与 job；PMS 又对外层 CompletableFuture 做一次独立的 60 秒 get。它们起算位置和完成对象不同，不能合并成一个“60秒”。服务端 Binder 方法则创建 CountDownLatch 后直接 await，没有自己的 timeout 参数，InterruptedException 日志仍写成 timed out。PermissionControllerServiceImpl 再把 XML 工作投到 AsyncTask，结束时 countDown。外层超时不会自动回滚服务端已做工作，也不能把“PMS 返回”解释为远端线程必然停止。

失败值也会改变备份语义：Manager 遇到接收错误会给回调 EmptyArray.BYTE，PMS 的 60 秒 future 可能因此以空数组正常完成；BlobBackupHelper 会给非 null 的空数组添加版本头并压缩，形成一个实体。若 PMS 最终返回 null，且 checksum 相对旧状态变化，BlobBackupHelper 会写 `dataSize=-1` 的删除实体。两种失败都不等于“保留云端上一份好数据”。服务端序列化异常又在实现内被捕获，普通 pipe 可能只交回空或部分字节。

恢复入口更弱：stageAndApplyRuntimePermissionsBackup 是 fire-and-forget 的发送 API，没有成功结果回传给 BackupManager 调用栈。在同一存活连接、同一 IPermissionController Binder 对象上，ServiceConnector 按提交顺序发请求，oneway transaction 又按序 dispatch；stage 的 Stub 在 transaction 内用 latch 等到 AsyncTask 连同 delayed 写结束，才允许后续 oneway retry 进入。这个顺序屏障不改变本地 API 的完成语义：方法返回只代表任务已投递或 pipe 发送链启动，不代表 XML 已解析、包状态已改、delayed 文件已写，更不代表 system_server runtime 文件已落盘。

### 练习 7：给30秒、两只60秒和latch分别命名

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'addHelper(PERMISSION_HELPER, new PermissionBackupHelper(mUserId));' frameworks/base/services/core/java/com/android/server/backup/SystemBackupAgent.java
grep -n -F 'return mPermissionManager.backupRuntimePermissions(mUser);' frameworks/base/core/java/com/android/server/backup/PermissionBackupHelper.java
grep -n -F 'return backup.get(BACKUP_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static final long REQUEST_TIMEOUT_MILLIS = 60000;' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'RemoteStream.receiveBytes(remotePipe -> {' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'callback.accept(EmptyArray.BYTE);' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'orTimeout(30, SECONDS);' frameworks/base/core/java/com/android/internal/infra/RemoteStream.java
grep -n -F 'CountDownLatch latch = new CountDownLatch(1);' frameworks/base/core/java/android/permission/PermissionControllerService.java
grep -n -F 'latch.await();' frameworks/base/core/java/android/permission/PermissionControllerService.java
grep -n -F 'AsyncTask.execute(() -> {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
```

把四个等待主体写成表：谁计时、从何时起算、超时后谁完成、远端工作是否被取消。随后比较空 byte[] 与 null 进入 BlobBackupHelper 后对旧备份实体的不同影响。

## 11. 备份XML只挑用户有意义的偏离，并按effective grant判断

备份 XML 根是 `perm-grant-backup`，内部 `rt-grants` 按 package 保存 permission。每项只有 name、g、set、fixed、was-reviewed；这里没有本机 runtime 文件的任意 flags 位图，也没有应用签名、版本、installer 或 shared-user 身份。恢复层首先按 packageName 匹配，不能把这份负载当成安全的本机文件复制品。

选择规则按“是否值得迁移”裁剪：

- POLICY_FIXED 或 SYSTEM_FIXED 直接排除，避免把设备政策和系统不可变决定搬走；
- `!USER_SET && GRANTED_BY_DEFAULT` 在其他判断前直接排除；极端的 default 标记仍在、USER_SET=false、USER_FIXED=true 的拒绝也可能因此漏掉；
- targetSdk 至少 M 的现代应用，以 effective grant 或 user-set/fixed 为非默认状态；
- pre-M legacy 应用方向相反：默认视为有效授予，只有有效拒绝、用户 flags 或已完成 review 才值得记录；
- effective grant 要同时看 runtime bit 与关联 AppOp 是否允许，但这里刻意不把 review-required 纳入该 helper 的 grant 计算；
- background permissions 不在顶层 group 列表里，代码显式再遍历 background subgroup；
- 没有任何入选 permission 的包完全不写入。

这是一种稀疏“用户选择负载”，不是源设备最终状态的全量声明。目标系统的默认授权、政策、包清单和 permission 定义会参与重新解释。

## 12. ONE_TIME和ROLE在备份边界丢失来源，临时授权可能永久化

r48 的筛选 mask 只有 POLICY_FIXED 与 SYSTEM_FIXED。活跃的一次性权限在内存里通常是 runtime granted、AppOp 有效，因而对现代应用会生成 `g=true`；BackupPermissionState 没有 ONE_TIME 字段，writeAsXml 也只输出 g/set/fixed/was-reviewed。

在一台新恢复目标上，AppPermissions 初始模型通常没有 ONE_TIME。restore 看到 g=true 后调用 group.grantRuntimePermissions，却没有重建一次性 session，也没有调用 setOneTime；最终 persistChanges 从目标模型重新合成 flags，于是该授权成为普通持久 grant。也就是说，这不是“少恢复一个标志”而已，而是临时授权被提升为永久授权的 r48 语义漏洞。

GRANTED_BY_ROLE 有相同的来源坍缩。它不在排除 mask，也不在 XML 字段里；只由 role 授予、又没有 GRANTED_BY_DEFAULT 的有效 grant 可以被保存成 g=true。若目标侧没有另一轮独立 role reconcile，恢复结果就是不带 role 来源的普通 grant，日后按 role bit 撤销的逻辑也识别不到它。AUTO_REVOKED、restriction exemption 等其他来源位同样不会逐位往返。

这也解释了为何本机文件和备份负载不能互换：前者保存 flags 且对 ONE_TIME 强制 denied，后者主动压缩策略，却在 r48 没有为一次性和角色授权建立安全排除或来源字段。

### 练习 8：用同一个one-time状态穿过两种serializer

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F '& PackageManager.FLAG_PERMISSION_ONE_TIME) == 0));' frameworks/base/apex/permission/service/java/com/android/permission/persistence/RuntimePermissionsPersistenceImpl.java
grep -n -F 'private static final String ATTR_WAS_REVIEWED = "was-reviewed";' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'private static final int SYSTEM_RUNTIME_GRANT_MASK = FLAG_PERMISSION_POLICY_FIXED' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'isNotInDefaultGrantState = isPermGrantedIncludingAppOp(perm);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'return new BackupPermissionState(perm.getName(), isPermGrantedIncludingAppOp(perm),' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'group.grantRuntimePermissions(false, mIsUserFixed,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'permission.isOneTime() ? PackageManager.FLAG_PERMISSION_ONE_TIME : 0' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'permission.setOneTime(isOneTime);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
```

从“grant=true、ONE_TIME=true、AppOp allowed”出发，分别列出本机 XML 与备份 XML 的字段，再在新目标上推演 restore。对 ROLE grant 重做一次，观察丢失的是授权值还是授权来源。

## 13. restore先按平台版本扩展split，再让目标当前USER_SET获胜

备份根携带 platform version，解析 permission 时遍历当前 split permission 表：如果备份平台版本小于 split 的 targetSdk 且名字命中旧 permission，就把同一备份状态复制到所有新 permission。expandedPermissions 没有额外去重。

r48 有一个前向兼容边界：只要 BuildCompat.isAtLeastQ，就固定写 `Build.VERSION_CODES.Q`，即 29，而不是实际 R 的 30。r48 当时已有 split 的 target 通常不高于 29，所以同版本恢复未必立即误扩；未来若出现 targetSdk=30 的 split，一份实际来自 R 的备份会被误判为 pre-30 并展开。

恢复已安装包时，BackupPackageState 创建 AppPermissions 快照，先处理 foreground，再处理 background，以免 pre-M 应用形成“后台授予但前台拒绝”的不可表达组合。单 permission 的顺序是：找到 group 和 permission；若 was-reviewed 则先清 REVIEW_REQUIRED；随后固定组直接返回；若目标快照中的 permission 已 USER_SET，则不 grant/revoke，否则按 g 和 fixed 修改并设置备份的 USER_SET。

“目标用户决定优先”只能限定为“创建该 AppPermissions 快照时看到 USER_SET 就跳过”。检查与最终 persistChanges 之间没有 CAS，也没有覆盖整段工作的 system_server 锁；并发的新用户操作仍可能被旧快照写回覆盖。was-reviewed 又在 fixed 与当前 USER_SET 两道保护之前清除，所以这两类目标状态仍可能发生 REVIEW_REQUIRED 变化。

## 14. 每包恢复会清扫整组USER_FIXED，并以整包快照持久化

处理完备份列出的 permissions 后，restore 并不会只收尾命中的项。它遍历 AppPermissions 中每个 foreground group 及 background group；只要 `areRuntimePermissionsGranted()` 为 true，就调用 group.setUserFixed(false)。这个 granted 判断是“组内至少一个 permission effective-granted”，而 setUserFixed(false) 会清该组所有 permission 的 USER_FIXED。

所以副作用范围可能大于备份负载：同组未被备份甚至处于 denied 的 sibling，也可能因为组内另一个 permission 已授予而失去 USER_FIXED。最后 `appPerms.persistChanges(true)` 按整包快照重放 runtime bit、AppOp 和多种 flags；其更新 mask 包含 AUTO_REVOKED，重建值却从不带该位，因此连未命中备份的权限也可能被清掉 AUTO_REVOKED。并发变化同样可能被旧快照覆盖。

恢复也不具备 per-package 事务隔离。初始 full restore 对已安装包逐个调用 restore；某包抛异常会中断后面的循环，之前已经发生内存或平台状态变化的包不回滚，而本次 packagesToRestoreLater 还可能根本没写。一次“restore 调用结束”可能同时包含已变化的局部包、未处理包和未可靠保存的延迟集合。

## 15. 未安装包进普通DE文件，按包重试的true和false并不对称

初始 restore 解析所有 BackupPackageState。目标已安装的包立即应用；NameNotFound 的包放入 packagesToRestoreLater。最后在静态 sLock 下，用 PermissionController 用户上下文的 `openFileOutput(name, MODE_PRIVATE)` 整体重写 delayed_restore_permissions.xml。PermissionController manifest 默认使用 Device Protected Storage，所以典型位置属于 `/data/user_de/<userId>/<permission-controller-package>/files/`。它是普通 app-private DE 文件，不是 Permission APEX 文件，也不是 AtomicFile；即使列表为空也写一份合法空 XML，不删除文件。

第二次 full restore 会整表覆盖上一份 pending，不做 merge。sLock 只串行化 PermissionController 进程内的 delayed 文件读改写，而且跨用户共用同一个静态锁；它不提供 restore generation。r48 服务工作还因 AsyncTask 默认串行而通常不会逐包交错，但后执行的一代仍会整体替换前一代 delayed 集合，这不是文件协议自身的 merge 或事务保证。

包安装后，PMS 请求 applyStaged。restoreDelayedState 在 sLock 内读全表，找到第一个同名条目，restore 成功返回后才 remove，再整体重写；若 group/permission 不存在或 fixed 导致跳过，包条目仍会被消费。写失败在 helper 内只记录日志，返回的 hasMore 仍按内存 list 计算，不能证明新文件提交成功。

返回布尔的失败语义尤其危险：

| 情况 | 回调值 | PMS解释 |
| --- | --- | --- |
| 文件不存在、XML损坏或本地 delayed restore 异常 | false | 缓存“该用户没有剩余备份”，以后安装可直接跳过 |
| Manager/远端请求错误 | true | 保守认为仍有内容，未来继续重试 |
| 正常处理后列表非空/为空 | true / false | 分别继续或关闭重试 |

若某 delayed 包 restore 抛异常，条目尚未 remove，但 ServiceImpl 捕获后返回 false，PMS 反而可能把仍在文件里的条目永久屏蔽到下一次 full restore 或进程重启。

正常的单次 `stage → applyStaged` 不应被描述成“apply 能越过尚未写完的 stage”：同一存活 Binder 对象上的顺序与 stage latch 已构成屏障。可证的竞态发生在不同代际：apply 的 Binder 方法只把工作交给 AsyncTask 便返回，而 r48 的 AsyncTask.execute 默认使用进程级 SERIAL_EXECUTOR。若提交顺序是 old apply、new full restore/stage，旧任务会先依据旧文件或空列表算出 false 并完成远端 future，新 stage 任务随后才写入新 pending；但 old false 的处理还要异步投到 system_server main executor，它可以在新 full restore 已删除缓存、甚至新 stage 已写完之后才执行，再把 mHasNoDelayedPermBackup 置 true。新 stage 没有成功回调去清这个值，缓存又没有 generation token，于是新一代 pending 可能被旧一代结果屏蔽。竞态发生在迟到的跨进程回调，不是两个 PermissionController 文件任务并行写。

### 练习 9：构造延迟恢复的失败矩阵和代际竞态

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'DELAYED_RESTORE_PERMISSIONS_FILE, MODE_PRIVATE' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'mContext.openFileInput(DELAYED_RESTORE_PERMISSIONS_FILE)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'packagesToRestoreLater.remove(i);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'return packagesToRestoreLater.size() > 0;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/BackupHelper.java
grep -n -F 'android:defaultToDeviceProtectedStorage="true"' packages/apps/PermissionController/AndroidManifest.xml
grep -n -F 'callback.accept(true);' frameworks/base/core/java/android/permission/PermissionControllerManager.java
grep -n -F 'mHasNoDelayedPermBackup.delete(user.getIdentifier());' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mHasNoDelayedPermBackup.put(user.getIdentifier(), true);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'oneway interface IPermissionController' frameworks/base/core/java/android/permission/IPermissionController.aidl
grep -n -F 'private static volatile Executor sDefaultExecutor = SERIAL_EXECUTOR;' frameworks/base/core/java/android/os/AsyncTask.java
```

先画正常 stage、服务端 latch、delayed 写、apply 的有序链；再画 old apply(false)、new full restore、late callback 三者交错。最后把“文件错误返回 false”和“传输错误返回 true”放入同一表，检查哪类失败会过早停止重试。

## 16. 用完成点矩阵判断权限状态究竟保存到了哪里

把整章压成一张判定表：

| 观察到的事件 | 可以确认 | 不能确认 |
| --- | --- | --- |
| Permission API 返回 | 内存 mutation 已走完对应同步路径 | runtime XML 已写、AppOps 或备份已收敛 |
| runtime async 消息已投递 | 某用户未来有一次快照任务 | 2秒内一定完成 I/O |
| writePermissionSettings(sync) 返回 | 同步写尝试已经结束 | AtomicFile 没有并发 writer、失败已向上传播、下次启动必能读回 |
| Controller upgrade 回调成功 | whitelist/grant 循环走到回调 | user-sensitive 完成、version/fingerprint 已落盘 |
| mIsStarted 已置 true | upgrade gate 已返回 | Permission/AppOps 全量同步和 onInitialized 已完成 |
| backupRuntimePermissions 返回 byte[] | pipe 层交付了某个字节数组 | 内容一定完整；空数组也可能是传输失败 |
| stageAndApply 本地返回 | 任务已提交到远端连接链 | 包 mutation、delayed 文件或 runtime XML 完成 |
| applyStaged 回调 false | 实现声称无剩余，或本地失败被压成 false | delayed 文件一定为空且可读 |
| 下次启动读回新 APEX XML | 某次已提交文件可解析 | 云端备份、角色来源和一次性会话可恢复 |

排查本机丢权限，先区分内存值、scheduled 标志、APEX base/`.new`、旧 legacy 文件和启动异常；排查换机偏差，则检查备份筛选、effective AppOp、ONE_TIME/ROLE 来源、platform version 与目标 USER_SET；排查未安装包恢复，最后再看 delayed 普通文件、hasMore 的真假语义和 PMS 缓存代际。

最重要的结论只有三个：本机 XML 是每用户 runtime 状态快照，不是 packages.xml 的别名；备份 XML 是有意稀疏的策略负载，却在 r48 存在 ONE_TIME 与 ROLE 的语义坍缩；所有调度、回调和版本标记都只覆盖自己的阶段，真正的完成必须落到“谁成功提交了什么、谁还能重试、下次读取会选哪份数据”。下一章将回到默认运行时授权，拆解 DefaultPermissionGrantPolicy、SystemConfig exceptions、Role 授权与权限来源 flags 如何共同决定初始状态。
