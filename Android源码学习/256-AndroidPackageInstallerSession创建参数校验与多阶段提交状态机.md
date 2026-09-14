# 256 Android PackageInstallerSession创建、参数校验与多阶段提交状态机

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 阅读方式：macOS 只读检索 AOSP，不编译、不启动设备

## 1. 先建立正确模型：Session 是安装事务的前半程

第 252 章从 PackageManagerService（PMS）内部解释了 prepare、scan、reconcile 与 commit。本章把入口侧补齐：安装器怎样提出请求，`PackageInstallerService` 怎样把请求净化成服务端事实，客户端怎样把文件送进受控 stage，`PackageInstallerSession` 又怎样在交给 PMS 之前完成封印、内容验证、用户确认和多包编排。

最容易形成的误解，是把 Session 当成“临时 APK 目录”。目录只是它的一本账。一个 Session 同时维护至少七类状态：

| 账本 | 代表字段或对象 | 回答的问题 |
|---|---|---|
| 身份账 | `mInstallerUid`、`InstallSource`、`userId` | 谁创建、谁拥有、替谁安装 |
| 策略账 | `SessionParams` | FULL/INHERIT、flags、位置、staged、DataLoader |
| 文件账 | `stageDir`、`mFiles`、resolved 集合 | 写入、删除、继承、规范化后的文件是什么 |
| 生命周期账 | `mPrepared`、`mSealed`、`mCommitted`、`mRelinquished`、`mDestroyed` | 还能做什么、控制权走到哪里 |
| 活跃账 | `mActiveCount` | 当前有多少客户端或内部提交引用 |
| 组合账 | parent/child session IDs | 多包关系与组内一致性 |
| 结果账 | `IntentSender`、final status、staged ready/applied/failed | 调用者应该等待哪个完成点 |

主链可压缩为：

> `createSession` 净化请求并登记元数据 → `openSession` 首次准备 stage → 客户端用受控 FD 写入 → `commit` 同步封印并阻塞持久化 → Handler 异步准备 DataLoader、解析和校验文件 → 必要时发起用户确认 → 生成 `ActiveInstallSession` 交给 PMS，或把 staged 会话交给 `StagingManager` → 经 Observer、IntentSender 和 staged 状态回报不同层次的结果。

这里存在三个不同的“提交”：Session 的 `mCommitted` 表示输入已验证并进入安装阶段；PMS 的安装事务提交表示包管理状态和文件切换已落定；staged 的 `applied` 才表示跨重启载荷实际生效。把三者写成一个“安装成功”，会让故障定位从一开始就错位。

本章重点跟踪这些文件：

- `frameworks/base/core/java/android/content/pm/PackageInstaller.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java`
- `frameworks/base/services/core/java/com/android/server/pm/StagingManager.java`

阅读时还要锁定版本边界：本文结论对应 r48。后续 Android 版本即使类名相同，权限、字段持久化、DataLoader 和 staged 行为也可能变化。

## 2. 创建入口先校验人和用户，不先碰文件

客户端 `PackageInstaller.createSession()` 只是把 `SessionParams`、自己的 installer package name 和目标 user ID 送过 Binder。服务端事实从 `PackageInstallerService.createSessionInternal()` 开始，其第一份可信身份不是参数里的包名，而是 `Binder.getCallingUid()`。

入口按以下次序建立边界：

1. 对 calling UID 和目标 `userId` 执行 `requireFull=true, checkShell=true` 的 cross-user 检查；仅仅能传入另一个整数，并不意味着能跨用户安装，shell 还受目标用户 `DISALLOW_DEBUGGING_FEATURES` 限制。
2. 若目标用户有 `UserManager.DISALLOW_INSTALL_APPS`，立即抛出 `SecurityException`。
3. 若参数带 `dataLoaderParams`，调用者必须拥有 `USE_INSTALLER_V2`。
4. 过长的 `appPackageName` 被置为 `null`，不是立刻拒绝；真实包名稍后可以从 APK 推导。
5. `appLabel` 经 `TextUtils.trimToSize()` 截到安全显示长度。

这些检查发生在 session ID 分配和 stage 路径计算之前。因此“创建被拒绝”通常没有 stage 目录需要清理，也没有可供稍后打开的 Session。入口会拒绝负 user ID，但这段 create 链没有单独证明每个非负 user ID 都真实存在；跨用户权限检查与用户存在性不能互相替代。Session 的 target user 也可以不同于 installer UID 所属 user，这会使 device/profile owner 的静默安装门直接返回 false。

`appPackageName = null` 也不能解释成“包名不重要”。如果调用者提供了合法长度的包名，后续 `assertApkConsistentLocked()` 会把它当成约束；若被清空或本来就没填，则校验阶段仍会以第一个 APK 的 manifest 建立 `mPackageName`。这两个分支的区别，是“预声明并受约束”与“从内容推导”，不是“检查”与“不检查”。

### 练习 1：定位创建入口的第一组信任边界

下面命令既可在源码根目录直接运行，也可从任意目录把源码根作为第一个参数传入。逐条命中后，按“可信身份 → 用户边界 → 参数降权”的顺序解释结果。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private int createSessionInternal' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'final int callingUid = Binder.getCallingUid();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'mPermissionManager.enforceCrossUserPermission(' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'UserManager.DISALLOW_INSTALL_APPS' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'Manifest.permission.USE_INSTALLER_V2' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.appPackageName = null;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.appLabel = TextUtils.trimToSize' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'String requestedInstallerPackageName' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'return mInstaller.createSession(params, mInstallerPackageName, mUserId);' frameworks/base/core/java/android/content/pm/PackageInstaller.java
```

验收时要能回答：哪个值来自 Binder，哪个值只是调用方声明，以及拒绝发生时为何还不存在 stage。

## 3. SessionParams 会被净化，installer 也不是单一名字

`SessionParams` 是请求，不是最终策略。创建逻辑会依据 calling UID 改写 flags，并拆出三种来源身份：

- initiating package：直接发起这次安装链的包；shell/root 的 ADB 路径可使这里为空。
- originating package：只有 `originatingUid` 非 unknown 且不同于 calling UID 时才尝试反查；同 calling UID、unknown 或查不到包时为 null，共享 UID 有多个包时 r48 只取数组第一个。数值 UID 仍留在 params/XML/verification 信息中，并没有进入 `InstallSource` 的包名字段。
- installer package：最终被记录为安装来源的包，可以由有权调用者指定。

普通应用首先要通过 `AppOpsManager.checkPackage(callingUid, installerPackageName)`，证明传入包名属于该 UID。若 requested installer 与直接调用者不同，没有 `INSTALL_PACKAGES` 时还要证明 requested installer 同样属于 calling UID。换句话说，改 installer 并不自动代表可冒充第三方。

shell/root 路径会加 `INSTALL_FROM_ADB`，并把 initiating installer 置空；普通调用者则会清掉 `INSTALL_FROM_ADB`、`INSTALL_ALL_USERS` 和 `INSTALL_ALLOW_TEST`，强制加入 `INSTALL_REPLACE_EXISTING`。`INSTALL_VIRTUAL_PRELOAD` 只对 verifier 保留。

downgrade 也不是一个由应用任意开启的开关。debuggable build，或 system/shell/root 调用者，会得到 `INSTALL_ALLOW_DOWNGRADE`；其他调用者的 allow/request downgrade 位都会被清除。禁用验证更窄：system 可以保留，或者命中 r48 定义的 ADB 开发组合；否则 `INSTALL_DISABLE_VERIFICATION` 被清掉。

staged 或 APEX 要求 `INSTALL_PACKAGES`。APEX 还要求设备支持 APEX 且必须使用 staged。非 system/root/shell 的 staged installer 还要在系统白名单内；一次性 bypass 是服务端状态，不是 SessionParams 自带通行证。非多包会话若请求自动授予运行时权限，还要额外拥有 `INSTALL_GRANT_RUNTIME_PERMISSIONS`。

参数净化还存在几条容易漏掉的限定。requested installer 只有在非空且长度严格小于最大包名长度时才采用，边界与 `appPackageName` 的“超过最大长度才清空”并不对称。icon 缩放、mode 拒绝、运行时权限自动授予检查以及 volume 解析都位于 `!params.isMultiPackage` 分支，父 Session 因而不在这里建立自己的载荷位置。restricted permissions 白名单还会被原地过滤，只保留真实的 hard/soft restricted permissions。若 DataLoader component 指向 system DataLoader package，构造器另有限定 shell/root/system 的门；但 r48 用字符串引用 `==` 判断这个包名，不能把它表述成可靠的内容相等门。DataLoader 与 APEX 的组合也在构造期被拒绝。

有两处值得专门记成审计结论。其一，`forceQueryableOverride` 只对 shell/root 保留，system UID 也不在例外中。其二，r48 的 `areHiddenOptionsSet()` 实现用 `(installFlags & hiddenMask) != installFlags`；只要 flags 还含 mask 外的常规位就很容易返回 true。`transfer()` 却只在它返回 false 时拒绝，并给出“只允许 public options”的文案。普通创建又常加入 `REPLACE_EXISTING`/`INTERNAL`，使这道门几乎总能通过。这里应记录源码条件与意图的反向张力，不能按方法名替实现补出正确限制。

### 练习 2：还原 flags 和安装来源的净化矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mAppOps.checkPackage(callingUid, installerPackageName);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'requestedInstallerPackageName, installerPackageName' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.installFlags |= PackageManager.INSTALL_FROM_ADB;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.installFlags &= ~PackageManager.INSTALL_ALL_USERS;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.installFlags |= PackageManager.INSTALL_REPLACE_EXISTING;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'originatingPackageName = packages[0];' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'PackageManager.INSTALL_ALLOW_DOWNGRADE' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'PackageManager.INSTALL_DISABLE_VERIFICATION' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'PackageManager.INSTALL_APEX' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'isStagedInstallerAllowed(requestedInstallerPackageName)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'android.permission.INSTALL_GRANT_RUNTIME_PERMISSIONS permission' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'params.forceQueryableOverride = false;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'if (!params.areHiddenOptionsSet())' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

不要只列 flags；应说明每个改写由哪种调用身份触发，以及 `InstallSource` 三个名字为何不能互换。

## 4. 存储、配额、ID 与 create 的真实完成点

非多包 Session 只接受 `MODE_FULL_INSTALL` 或 `MODE_INHERIT_EXISTING`。显式内部安装先用 `PackageHelper.fitsOnInternal()` 做容量判断；force volume 会从 flag 角度被视为 internal；未显式指定时则在清除 calling identity 后调用 `resolveInstallVolume()` 决定 volume UUID。这里的容量检查只是创建时快照，后续写入、继承和 PMS 安装仍可能因空间变化失败。

服务还按 calling UID 限制创建时归属于该 UID 的当前 Session 数：有 `INSTALL_PACKAGES` 时上限 1024，没有时上限 50；历史会话上限是 1,048,576。`getSessionCount()` 实际遍历 `mSessions` 并比较当前 installer UID，成功 staged Session 因仍留在集合也可能继续计数，transfer 还会改变归属。这一“active sessions”配额不等同于 Session 内部 `mActiveCount` 的打开引用数。两处都用了 active 一词，却回答完全不同的问题。

`allocateSessionIdLocked()` 从正整数空间随机取值，避开本次启动期间 `mAllocatedSessions` 见过的 ID。其 `do/while (n++ < 32)` 从 n=0 开始，最多检查 33 个候选，也就是首次加 32 次重试。因此 ID 是不透明句柄，不应被调用者解释成时间、顺序或持久唯一编号。

创建阶段只计算 stage 位置：内部普通 Session 对应 `vmdl<ID>.tmp`，staged 会话对应 `session_<ID>`；multi-package 父会话没有自己的 stageDir/stageCid。真正建目录发生在第一次 `open()`。随后构造 `PackageInstallerSession`、放入 `mSessions`，staged 还登记到 `StagingManager`；除 dry run 外发送 created callback，并把 `install_sessions.xml` 的异步写任务交给 I/O 线程。

所以 create 的完成点是“服务端对象和 ID 已登记，可被后续 open”，不是“目录已建立”，更不是“APK 已经可安装”。如果异步元数据持久化尚未发生 system_server 就异常退出，调用者刚拿到的 ID 也可能无法在恢复后重建。

### 练习 3：证明 create 没有创建 stage 目录

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'case SessionParams.MODE_FULL_INSTALL:' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'PackageHelper.fitsOnInternal(mContext, params)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'PackageHelper.resolveInstallVolume(mContext, params)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'MAX_ACTIVE_SESSIONS_WITH_PERMISSION = 1024' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'MAX_ACTIVE_SESSIONS_NO_PERMISSION = 50' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'MAX_HISTORICAL_SESSIONS = 1048576' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'sessionId = allocateSessionIdLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'stageDir = buildSessionDir(sessionId, params);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'mSessions.put(sessionId, session);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'mStagingManager.createSession(session);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'mCallbacks.notifySessionCreated' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'writeSessionsAsync();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'private int allocateSessionIdLocked()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'private File buildSessionDir' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
```

把“路径计算”“对象登记”“异步持久化”“物理建目录”分别标在时间线上，才能得到正确的 create 完成点。

## 5. open 才 prepare；活跃引用和服务配额必须分开

`openSessionInternal()` 先从 `mSessions` 找对象并校验 owner，然后调用 `session.open()`。`open()` 用 `mActiveCount.getAndIncrement()` 增加客户端引用；从 0 变 1 时发 active callback。第一次打开才在 Session 锁内调用 `prepareStageDir(stageDir)` 并设置 `mPrepared = true`；multi-package 父会话因为没有载荷目录，直接视为可以 prepare。首次准备后再发 `onSessionPrepared()`，服务异步写 XML。

一个 Session 可以被同一 owner 多次打开，所以 active 是引用计数而非布尔生命周期。每次客户端 `close()` 都会减一，归零才发 idle callback。提交验证成功后，`streamValidateAndCommit()` 还会额外加一个内部引用，让客户端即使关闭句柄，正在进行的安装也保持 active。若稍后需要用户确认，`makeSessionActiveLocked()` 用 `closeInternal(false)` 释放的正是这份提交引用，使等待 UI 的会话看起来 idle；用户接受后进入 `MSG_INSTALL`，并不会重新跑文件验证。普通最终 Observer、destroy 和 finish 路径没有与这份内部 increment 对应的 decrement；普通会话随即被移出集合，staged success 则可能仍留下正计数。因此该字段更适合观察回调活跃性，不能当资源引用的严格证明。

这段代码隐含调用配对要求：错误地重复 close 会把计数减到负数，当前方法本身没有做饱和保护。`open()` 又在 prepare 之前先 increment；若 `prepareStageDir()` 抛出异常，源码没有回滚这次增加，而客户端也没有成功拿到 Binder 包装去正常 close。公开 API 正常使用应由一份打开的 `Session` 对应一次 close；调试 active 异常时，应核对客户端句柄、prepare 失败和内部 commit 引用，而不是去看 50/1024 的创建配额。`open()` 本身也不检查 sealed/committed/destroyed，限制主要落在之后的具体读写操作。

stage 元数据与 stage 内容并非同一个原子文件：`install_sessions.xml` 保存 Session 状态，真实 APK 位于 stage。开机恢复还会对实际 stage 目录做 reconcile，清走没有有效 Session 对应的孤儿目录。

### 练习 4：画出 prepared 与 active 的引用变化

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private IPackageInstallerSession openSessionInternal' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'session.open();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'public void open() throws IOException' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mActiveCount.getAndIncrement() == 0' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'prepareStageDir(stageDir);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPrepared = true;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mCallback.onSessionPrepared(this);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'activeCount = mActiveCount.decrementAndGet();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mCallback.onSessionActiveChanged(this, false);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mActiveCount.incrementAndGet();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'closeInternal(false);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'reconcileStagesLocked(StorageManager.UUID_PRIVATE_INTERNAL);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
```

建议手算三条路径：open→close、open→commit→close→安装回调，以及 open→commit→close→等待确认。

## 6. 写入面：受控 FD、断点偏移与不统一的截断语义

普通 Session 的客户端 `openWrite(name, offset, length)` 经 Binder 进入 `doWriteInternal()`。写入前必须满足 owner/root、prepared、未 sealed；DataLoader Session 禁止走普通文件 API。隐藏的 `Session.write(..., incomingFd)` 反向写入入口还把调用者限制给 shell/root/system。DataLoader 的 `FileSystemConnector.writeData()` 也接收 incoming FD，却直接进入 `doWriteInternal()`，不经过这道调用者检查；它属于封印后由 DataLoader 供数的另一条受控路径。

`doWriteInternal()` 的真实顺序比“先验文件名”更危险：它先在 `mLock` 内把新的 revocable FD 或 bridge sentinel 加进集合，释放锁后才检查 `FileUtils.isValidExtFilename()`、open 和 allocate。普通 `openWrite()` 若在这些后续步骤抛异常，没有通用 finally 移除 sentinel，后续 commit 可能一直得到 `Files still open`，直到 abandon/destroy 强制回收。`assertCanWrite()` 与登记 sentinel 又是两个分开的锁区，中间如果并发 commit 抢先 seal，`doWriteInternal()` 不会复查 sealed，仍可登记并继续写。这是 r48 的锁间隙，不是推荐用法。

服务端打开目标文件使用 `O_CREAT | O_WRONLY`，再设 0644。这里没有 `O_TRUNC`，但 `length` 路径不能简单概括成“永不截断”：

- `offsetBytes > 0` 时执行 `lseek`，适合在调用者正确管理长度时续写。
- `offsetBytes == 0` 自身不会清掉已有文件尾部；当没有随后发生截断时，对同名较长旧文件重写较短内容会留下尾巴，并可能在 APK 解析或签名验证时暴露。
- `lengthBytes > 0` 调 `StorageManager.allocateBytes()`。常见 `posix_fallocate()` 快路径只分配空间，不缩短较长文件；不支持时的 fallback 却调用 `ftruncate(bytes)`，会把它裁到请求长度。它仍不代表这些字节已经写成有效 APK 内容。
- 因此 `offset=0` 或填写 `length` 都不是跨存储实现一致的“覆盖写并截断”保证；调用者应使用新的唯一文件名，或理解所在分支的最终长度语义。

普通客户端得到的并不是任意可长期持有的内部路径能力。r48 根据 `PackageInstaller.ENABLE_REVOCABLE_FD` 返回 revocable FD，或用 `FileBridge` 暴露 socket 端；Session 同时把相应对象记录到 `mFds`/`mBridges`。`commit()` 会逐一确认 FD 已 revoked、bridge 已 closed，只检查写传输，不因只读 FD 仍开着而拒绝。

客户端 `Session.fsync(OutputStream)` 只接受由 `openWrite()` 产生的两种流类型：revocable FD 分支调用 `Os.fsync()`，bridge 分支发送 bridge 协议的 fsync。默认 bridge 的 `CMD_CLOSE` 自身也会先对 target `fsync` 再关闭；revocable FD 的普通 close 没有同样的显式同步。commit 只检查 FD revoked/bridge closed，不另查调用者是否做过 fsync。稳健且不依赖分支的流程仍是 write → fsync → close → commit。反向同步 copy 的 finally 会关闭两端并移除临时 tracker，但没有额外显式 fsync。

`removeSplit(splitName)` 创建 `<split>.removed` 的零权限 marker。它禁止 DataLoader，并要求 `params.appPackageName` 非空；但 r48 调用的是 `assertPreparedAndNotCommittedOrDestroyedLocked()`，没有显式检查 sealed。这形成一个窄而真实的边界：seal 后、committed 前，marker 路径的防变更门禁比 `openWrite()` 弱。本文只记录源码事实，不把它推广成可依赖的 API 契约。

`getNames()` 与 `openRead()` 同样检查未 committed/destroyed，而没有把 sealed 列为拒绝条件；它们是观察面，不等于获得继续写入的能力。

### 练习 5：核对写入门禁与无截断语义

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'Cannot write regular files in a data loader installation session.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private ParcelFileDescriptor doWriteInternal' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertPreparedAndNotSealedLocked("assertCanWrite")' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mFds.add(fd);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'O_CREAT | O_WRONLY, 0644' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'allocateBytes(' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Os.posix_fallocate(fd, 0, bytes);' frameworks/base/core/java/android/os/storage/StorageManager.java
grep -n -F 'Os.ftruncate(fd, bytes);' frameworks/base/core/java/android/os/storage/StorageManager.java
grep -n -F 'Os.lseek(targetPfd.getFileDescriptor(), offsetBytes' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'PackageInstaller.ENABLE_REVOCABLE_FD' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'public void fsync(@NonNull OutputStream out)' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F 'Os.fsync(((ParcelFileDescriptor.AutoCloseOutputStream) out).getFD());' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F '((FileBridge.FileBridgeOutputStream) out).fsync();' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F 'Os.fsync(mTarget.getFileDescriptor());' frameworks/base/core/java/android/os/FileBridge.java
grep -n -F 'public void removeSplit(String splitName)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertPreparedAndNotCommittedOrDestroyedLocked("removeSplit")' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'REMOVE_MARKER_EXTENSION' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'public ParcelFileDescriptor openRead(String name)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private void assertNoWriteFileTransfersOpenLocked()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

额外做两个纸面实验：原文件 100 字节，以 `lengthBytes <= 0` 从 offset 0 写 60 字节，旧尾为何确定保留；再令 `lengthBytes=60`，比较 `posix_fallocate` 与 `ftruncate` fallback 的不同结果。这样才能看见“open flags 无截断”与“allocate fallback 可截断”同时成立。

## 7. 状态不是单向 enum：先学会每个布尔值拒绝什么

r48 没有用一个枚举表达 Session 生命周期，而是组合多个字段：

| 状态 | 何时置位 | 主要含义 | 不能误读为 |
|---|---|---|---|
| `mPrepared` | 首次 `open()` 准备 stage 后 | 载荷位置可使用 | 文件已经完整 |
| `mSealed` | `sealLocked()` 很早置位 | 普通写 FD 与正常 mutation 门关闭 | 内容已经验证成功；也不覆盖 r48 的 remove marker 窄窗 |
| `mCommitted` | stream/DataLoader 准备和 Session 内容验证成功后 | 可进入安装阶段，内部 active 引用已持有 | PMS 已安装成功 |
| `mRelinquished` | 构造 `ActiveInstallSession` 返回前 | Session 本地越过不可回退门，准备交接 | 该对象已经实际传入 PMS |
| `mDestroyed` | `destroyInternal()` 的条件分支或 staged abandon 设置 | 对普通/终态 staged 表示销毁 | 每次调用 destroyInternal 都必置位，或 staged 目录一定已删 |
| `mPermissionsManuallyAccepted` | 确认接受后 | 当前内存对象后续安装不再询问 | 安装获得额外权限，或重启后仍记得确认 |

这些布尔值会形成短暂组合。例如 `sealed=true, committed=false` 是同步封印完成、异步校验尚未完成；也可能是 DataLoader 暂不可用，等待调用者再次 commit。`committed=true, relinquished=false` 可以表示正在等待用户确认。staged 的 `destroyInternal()` 在未到终态时会 seal，但故意不一定把 `mDestroyed` 设为 true，也不删除 stage，因为重启后还要读取载荷。

`abandon()` 也按这些轴分叉。child 不能直接 abandon；一般只允许当前 owner/root，只有“staged 且已经 destroyed”的安全收尾分支额外接受 system。applied/failed 的 staged 终态直接 no-op 并保留历史；已 committed 的 staged 会先请求 `StagingManager` 中止，若当前还不安全，只记录 staged changed 后返回，安全时才清 stage；已经 relinquished 的普通安装忽略 abandon；其余路径调用 `destroyInternal()`，撤销 FD、强关 bridge、删除非 staged 的 stage，并回报 `INSTALL_FAILED_ABORTED`。普通 multi-package 父的这条 abandon 路径不会递归 destroy children，不能据父退出推断子对象也已销毁。

Session 状态与 staged 子状态又是两层：ready、applied、failed 三者至多一个为真。普通 `INSTALL_SUCCEEDED` 没有 ready/applied 的第二阶段；staged 则必须继续观察这组三态。

诊断时不要问“Session 是什么状态”这么宽泛的问题，而要逐一打印：prepared、sealed、committed、relinquished、destroyed、active count、parent/children、staged 三态、final status。一个词无法覆盖所有轴。

## 8. commit 的同步半程：所有权、开放写流、FRP、transfer 与永久 seal

child Session 不能直接 commit；入口先拒绝有 parent 的 child。父或普通 Session 随后进入 `markAsSealed(statusReceiver, forTransfer)`：取 children 列表时刻意不持有当前 Session 锁，避免跨 Session 取锁死锁；进入锁后再检查 owner/root、prepared、未 destroyed、没有开放写传输。

Secure FRP 开启时，系统 package installer 即便持有常规安装权限也被特意挡住；其他调用者至少要有 `INSTALL_PACKAGES`。这是一道 commit 时门禁，不能由 create 已成功推出一定可提交。

transfer 分支区分两个 API 方向：

- `forTransfer=true` 要求调用者持有 `INSTALL_PACKAGES`，并要求当前 owner 已不同于 original owner。
- 普通 commit 要求当前 owner 仍等于 original owner。
- `transfer(packageName)` 先按 Session 的目标 user 查找目标包并要求其持有 `INSTALL_PACKAGES`；随后才在锁内要求 Binder 调用者是当前 owner/root，Session prepared、未 committed/destroyed/sealed，且 `sealLocked()` 再确认没有开放 writer。通过后把 installer UID/InstallSource 改为新 owner。
- 内容验证还要求 transferred Session 的实际包名等于 original installer package；这里的 original package 是创建时已归一化的 requested installer-of-record，不是 initiating package，因而限制 UID 确实变化的 transfer 只能更新这个原 installer 自身。

“是否 transfer”完全用 UID 是否变化判断。转给共享同 UID 的另一个包时，`mInstallerUid` 仍等于 original：`commitTransferred()` 会说尚未 transfer，普通 `commit()` 反而可以继续，包名约束也不会触发。旧 owner 已打开的句柄在不同 UID transfer 后连普通 close 都会因 owner 检查失败，且 transfer 没有要求 active count 为零；这再次说明 active 不是严谨的 RAII 生命周期。

`mRemoteStatusReceiver` 在判断“是否已经 sealed”之前更新。因此对一个已 sealed、但允许重试的 Session 再调用正确方向的 commit，可以换一个接收结果的 receiver。它不会重新打开写窗口。反过来，owner/prepared/writer/FRP/transfer 的前置异常发生在 receiver 保存和 `sealLocked()` 之前，只同步抛给调用者，不会由这条路径 destroy 或派发最终失败；真正进入 `sealLocked()` 后的异常才经 `onSessionVerificationFailure()` 收束。

`sealLocked()` 先把 `mSealed = true`，再检查 multi-package 的 staged/rollback 一致性；注释也明确说明，即使封印过程失败，Session 仍被封印。失败经 `onSessionVerificationFailure()` 进入 destroy 和结果派发。成功后，代码退出 `mLock`，再调用 `onSessionSealedBlocking()` 阻塞写 XML。这一顺序同时满足两件事：不在 Session 锁内反向拿 Service 的会话锁；异步验证开始前，磁盘已经记住不可再写，避免重启后把已建立的硬链接重新暴露给修改。

### 练习 6：沿同步 commit 找到不可逆边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void commit(@NonNull IntentSender statusReceiver, boolean forTransfer)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'may not be committed directly.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private boolean markAsSealed' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertNoWriteFileTransfersOpenLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Secure.SECURE_FRP_MODE' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mRemoteStatusReceiver = statusReceiver;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (mSealed) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mCallback.onSessionSealedBlocking(this);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mSealed = true;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertMultiPackageConsistencyLocked(childSessions);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'MSG_STREAM_VALIDATE_AND_COMMIT' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void onSessionSealedBlocking' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'writeSessionsLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'public void transfer(String packageName)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

沿同步调用栈列出 commit 保证完成的事实，再列出它没有等待的 Handler 工作。消息在 Binder 方法返回前已经入队，独立 Handler 可能已经开始甚至完成，不能把“调用者刚返回”当成它仍排队的 happens-before 证据。

## 9. commit 的异步半程：可重入验证、暂态等待与失败终止

封印成功后才投递 `MSG_STREAM_VALIDATE_AND_COMMIT`。Handler 调 `streamValidateAndCommit()`：若已经 `mCommitted`，直接返回 true；否则准备 DataLoader 并验证内容，完成后把 client progress 推到 1、增加内部 active 引用、设置 `mCommitted = true`。

DataLoader 的 `prepareDataLoaderLocked()` 可以返回 false，表示数据源暂未准备好。此时会话已经 sealed，却没有 committed，Handler 不进入 install；当 DataLoader image ready 时会再次投递父或自身的 stream-validate 消息。`DATA_LOADER_UNAVAILABLE` 和某些远程异常走 pending streaming 通知，允许调用者再次 commit。相反，解析错误、不可恢复的数据源错误或一致性错误会调用 `onSessionVerificationFailure()`：销毁资源、发送最终失败，staged 还清理对应 stage。

因此 false 与 exception 必须分开：false 是“这轮尚未 ready”，exception 是“不可恢复”。代码注释所说“可以多次调用”，并不表示所有失败都能重试。

进度也有明确分工：客户端写入占 80%，内部准备占 20%。commit 验证完成把 client 部分设到 1，但这仍不是 PMS 安装进度的完整等价物。只看 100% 进度条不能推出包已可用。

## 10. APK 校验从文件发现开始，再建立内容基线

非 multi-package、非 APEX 会进入 `validateApkInstallLocked()`。每次验证先清空 resolved 状态和包身份，然后读取目标 user 下已有包信息。INHERIT 没有已安装 base 时立即失败；通常的 FULL 自带新 base，不为继承而依赖旧包，但若 FULL Session 也写入 `.removed`，后面的 remove-list 分支仍要求旧包和对应旧 split 存在。`removeSplit()` 本身没有 mode 门，不能把 FULL 概括成无条件不查旧包。

stage 文件发现故意分类：added APK filter 会排除目录、`.removed`、dex metadata 和 fs-verity signature；因此 `.dm` 不是被当作一个 APK 送进 `ApkLite` 循环，而是在处理对应 APK 时由 `DexMetadataHelper.findDexMetadataForFile()` 单独发现和规范化。这里仅做松散配对、外部名检查和改名，没有调用 `validateDexMetadataFile()`；孤立 `.dm` 甚至会被 added APK filter 忽略，而不在 Session 这一步单独拒绝。其 ZIP 内容合法性要到 PMS 完整解析后的 `AndroidPackageUtils.validatePackageDexMetadata(parsedPackage)`。把“.dm 在 added APK 列表里一起解析”或“Session 已完整验证 .dm”写进心智模型，都会错误解释责任边界。

对每个 added file，`parseApkLite(..., PARSE_COLLECT_CERTIFICATES)` 读取轻量 manifest 和签名信息。有 added APK 时，第一份建立 `mPackageName`、version code 和 signing details 基线；以后每一份必须满足：

- split name 不重复；
- package name 相同；
- 若 params 声明 app package name，还要与声明相同；
- long version code 相同；
- 签名精确匹配。

文件再按内容规范化：base 变成 `base.apk`，split 变成 `split_<name>.apk`。fs-verity 签名要求形成“全有或全无”集合；INHERIT 默认会参考旧 base 是否启用 fs-verity。这里验证的是 Session 内部文件集合自洽，不做 PMS 的完整升级兼容、shared UID、权限或系统包策略判断；源码注释直接把 upgrade compatibility 留给 `PackageManagerService`。

若没有 added APK、只有 remove marker，包名和版本基线改从旧 `PackageInfo` 取得，签名则用 `unsafeGetCertsWithoutVerification(..., JAR)` 从旧 base 读取；两类输入都为空才报 `No packages staged`。APEX 又是并列分支：`validateApexInstallLocked()` 要求 added file 恰好一个，必要时补 `.apex` 后缀并规范化，再用 `PackageParser.parseApkLite(...PARSE_COLLECT_CERTIFICATES)` 重建 package/version；它不进入 FULL/INHERIT 的 base/split 集合算法。

### 练习 7：从过滤器追到 ApkLite 一致性基线

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final FileFilter sAddedApkFilter' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'DexMetadataHelper.isDexMetadataFile(file)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'VerityUtils.isFsveritySignatureFile(file)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private void validateApkInstallLocked()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private void validateApexInstallLocked()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Too many files for apex install' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'sourceName.endsWith(APEX_FILE_EXTENSION)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Missing existing base package' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'final List<File> addedFiles = getAddedApksLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'ApkLiteParseUtils.parseApkLite' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'stagedSplits.add(apk.splitName)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertApkConsistentLocked(String.valueOf(addedFile), apk);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'targetName = "base" + APK_FILE_EXTENSION;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'DexMetadataHelper.findDexMetadataForFile(addedFile)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'AndroidPackageUtils.validatePackageDexMetadata(parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'DexMetadataHelper.validateDexMetadataFile(dexMetadata);' frameworks/base/services/core/java/com/android/server/pm/parsing/pkg/AndroidPackageUtils.java
grep -n -F 'private void assertApkConsistentLocked' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'signaturesMatchExactly(apk.signingDetails)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'upgrade compatibility is still performed by' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
```

验收输出应区分“发现某类文件”“解析 APK”“建立基线”“把文件改成规范名”四个动作。

## 11. FULL 与 INHERIT 的差异发生在集合构造，不只是一个 mode 值

FULL 必须在本次 `stagedSplits` 中包含 null，也就是提供 base。INHERIT 则先解析旧 `PackageLite` 和旧 base，再用同一套包名、版本、签名规则检查旧 base；本次没有覆盖 base 时把旧 base 纳入 inherited 集合。

旧 split 的处理是集合差：本次 staged 的同名 split 覆盖旧文件，`.removed` 指定的 split 被排除，剩余旧 split 与对应 dex metadata 被继承。每个 remove marker 必须指向真实存在的旧 split。继承的 oat 文件按合法 ISA 子目录收集；只有 `DONT_KILL_APP`、INHERIT、系统属性允许且 remove list 为空时，才把旧 native libraries 加入 inherited 集合，避免删 split 后误继承其 so。真正准备 `ActiveInstallSession` 前，同设备优先 hard link，跨设备或不能 link 时 copy；native 库也在 Session 层抽取或整理。

还有三个窄边界：

1. `baseApk.useEmbeddedDex` 时，只审计 resolved staged APK 的 dex 是否未压缩且对齐。
2. `baseApk.isSplitRequired` 的 r48 判断只看当前 `stagedSplits.size() <= 1`，没有把继承文件计入这个计数。因此 INHERIT 已有 split 也不能简单推出该检查必然通过。
3. Incremental + shell 的读取日志会依据 base 是否 debuggable/profilable 调整；这是安装数据读取可观测性，不是 APK 一致性放宽。

Session 层校验通过仍可能在 PMS 失败。签名升级能力、版本策略、shared user、权限迁移、系统包约束、存储提交等更完整规则属于后续链。正确完成点是“形成可交给 PMS 的自洽载荷集合”，而不是“升级一定合法”。

## 12. multi-package 是组编排，不是通用磁盘/数据库原子事务

multi-package 父 Session 没有自己的 stage；每个 child 承载一个普通安装单元。child 只要仍带有效 parent ID，就不能直接 commit 或 abandon。父 commit 先封印自己；父的 group consistency 失败会直接返回，根本不进入 child 循环。进入循环后，只有某 child 的 `sealLocked()` 验证失败被转成 `markAsSealed()==false` 时才记下失败并继续；owner、prepared、开放 writer、FRP、transfer 方向等前置门抛出的异常没有被循环捕获，会立即中断，并可能留下父和部分 child 已 sealed、其余尚未 sealed 的组合。

父子组装本身也有 r48 缺口。`addChildSessionId()` 没有显式确认当前 Session 真是 multi-package，也不检查 child owner 与 parent owner 相同；`removeChildSessionId()` 没有 owner/sealed/committed 门，而且在确认该 ID 属于当前 parent 之前就会把可找到 child 的 parent ID 清掉。最终 commit 对每个 child 再跑 owner/prepared/writer/方向门，只能在较晚阶段暴露一部分异常关系，不能把 add/remove API 当成已经建立完整事务约束。

异步验证也遍历父和全部 children。`allSessionsReady &= child.streamValidateAndCommit()` 使用非短路的位与赋值，确保前面一个 child 尚未 ready 时，后续 child 仍得到准备/验证机会。不可恢复失败会记录第一份异常，并让父与其他未失败 child 走同一失败清理。只要某个 DataLoader 还未 ready 而没有永久错误，就返回等待，不应伪装成最终失败。

组内创建/封印阶段显式检查的核心一致性只有 `isStaged` 和 enable rollback 两项。到安装阶段，父为每个 child 调 `makeSessionActiveLocked()`：抛出 `PackageManagerException` 才把整轮标为失败；若某个 child 因等待用户确认返回 null，循环只是略过它，并仍可能把其余非 null 子集交给 `mPm.installStage(list)`。若最终列表为空，PMS 的 multi-package 参数构造才会拒绝。这个 r48 行为意味着“有 child pending 就绝不启动其他 child”并非源码保证，不能把非空子集提交改写成全部成功才提交。`ChildStatusIntentReceiver` 收到 pending user action 时原样上送并保留 child session ID，确认组件才能对具体 child 回填结果；首个 failure 或全部 success 才把 ID 改成 parent。child 接受后由自己的 `MSG_INSTALL` 单独进入 PMS，正好闭合前述子集提交行为。

这提供了“组共同推进和统一结果”的语义，但不能无限外推成一个跨所有副作用的数据库事务。真正安装还经历 PMS 的 verification、prepare、scan、reconcile 和 commit；崩溃、staged 激活与外部组件都有各自恢复协议。最准确的表述是：multi-package 在 PackageInstaller/PMS 支持的安装边界内协调一组 Session，不是对任意文件、广播或外部服务提供通用 ACID 保证。

### 练习 8：区分父子继续遍历与异常中断

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final IntentSender childIntentSender' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'boolean sealFailed = false;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'allSessionsReady &= session.streamValidateAndCommit();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'onSessionVerificationFailure(unrecoverableFailure);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'assertMultiPackageConsistencyLocked' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'inconsistent staged settings' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'inconsistent rollback settings' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'class ChildStatusIntentReceiver' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mChildSessionsRemaining.removeAt' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'public void addChildSessionId' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'childSession.setParentSessionId(this.sessionId);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mPm.installStage(installingChildSessions);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void installStage(List<ActiveInstallSession> children)' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

先用三个 child 推演异步验证：A ready、B DataLoader unavailable、C 内容损坏，说明为何 B 的暂态不会阻止检查 C。再把 B 换成 owner 不匹配，解释为何封印循环会抛异常而不是继续到 C。

## 13. 用户确认位于内容验证之后、PMS 交接之前

`handleInstall()` 对普通非 staged Session 调 `installNonStagedLocked()`，再由 `makeSessionActiveLocked()` 判断是否需要用户确认。到这里 Session 已 sealed、已完成 `ApkLite` 集合验证并 `mCommitted=true`，所以确认 UI 可以基于已解析出的 `mPackageName`，而不是信任客户端声明。

无需询问的条件包括：安装器拥有通用 `INSTALL_PACKAGES`；或拥有 update 权限且目标包已存在；或拥有 self-update 权限且目标包 UID 就是 installer UID；或 installer 是 root/system；或设备所有者/关联 profile owner 被策略服务允许静默安装。`INSTALL_FORCE_PERMISSION_PROMPT` 可以强制走确认。注意，判断依据是当前 `mInstallerUid`，所以 transfer 后必须重新按新 owner 身份评估，不能缓存创建时结果。

需要确认时，Session 构造只发给系统 package installer 的 `ACTION_CONFIRM_INSTALL` Intent，通过状态 receiver 返回 `STATUS_PENDING_USER_ACTION`，然后释放 commit 额外持有的 active 引用并返回 null。这不是安装失败，也没有把写窗口重新打开。

正常流程由系统确认组件通过 Service 的 `setPermissionsResult()` 回来；这条 Binder 入口只强制调用者拥有 `INSTALL_PACKAGES`，Session 侧只检查 sealed，没有再核对调用者就是显式 Intent 指向的 package installer，也没有独立的 awaiting-user-action 状态门。接受时设置 `mPermissionsManuallyAccepted` 并直接投递 `MSG_INSTALL`；拒绝则 destroy，并以 `INSTALL_FAILED_ABORTED` 完成。接受后的第二轮不会再 parse 一遍 APK，也不会重新设置 `mCommitted`，而是从安装调度点继续。该接受位完全没有写入 XML，system_server 重启后的恢复对象会回到 false，重新推进时可能再次确认。

确认之后，INHERIT 才真正 link/copy 旧文件并提取 native libraries。`makeSessionActiveLocked()` 创建 Observer，把未来 PMS 回调接回 `destroyInternal()` 与 `dispatchSessionFinished()`，设置 `mRelinquished = true`，返回 `ActiveInstallSession`。这个位表示当前 Session 已越过本地不可回退门，所以 abandon 会被忽略；只有返回对象随后确实传给 `installStage()`，才能说 PMS 已接管。multi parent 的返回对象会被丢弃；child 转换中途抛异常时，较早转换成功的 child 也可能已 relinquished 却从未传入 PMS。

## 14. ActiveInstallSession 不是终点：PMS 仍有验证与事务提交

`ActiveInstallSession` 携带 package name、stageDir、observer、session ID、params、installer UID、InstallSource、目标 user 和 signing details。单包调用 `mPm.installStage(installingSession)`，多包调用 list 版本。之后进入 PMS 自己的 verification 与 install request 链，再经过前面章节讲过的 prepare、scan、reconcile、commit。

这解释了两类看似矛盾的日志：

- Session 已 `committed=true`，但最终得到 `INSTALL_FAILED_*`：前者只表示输入流和 Session 级内容校验成功，PMS 后半程仍可拒绝。
- Session `mRelinquished=true` 后 abandon 无效：它已经越过本地不可回退门；通常结果应由 PMS Observer 收束，但 multi 组装中途失败是“已置位却未实际提交”的例外。

实际交给 PMS 的 single/child，其 Observer 在 `onPackageInstalled()` 无论成功失败都会销毁该 Session 资源并派发最终结果。`dispatchSessionFinished()` 先记录 legacy return code/message，再异步把结果转换给 `IntentSender`；Service callback 通知注册监听者，把这些普通已结束 Session 从 `mSessions` 移到历史集合，并同步写 XML。新安装成功还可能向默认 launcher 发 session commit broadcast；dry run 会抑制一部分外部事件。

multi parent 是特殊清账缺口：`ChildStatusIntentReceiver` 对 pending 原样转发 child ID，只把首个 failure/全部 success 改成 parent ID；无论哪条都没有对 parent 调 `destroyInternal()`、`dispatchSessionFinished()` 或 Service `onSessionFinished()`。因此正常聚合出最终 Intent 也不会自动把 parent 从 `mSessions` 移除；它往往只能等非 staged 的 3 天开机清理。收到 parent 结果和 parent 对象已完成生命周期，在 r48 并不是同一件事。

public status 与 legacy install return code 不是同一命名空间。安装器应读取 `PackageInstaller.EXTRA_STATUS`、message、package name、session ID，遇到 pending user action 走确认分支；诊断 system_server 时才同时追 legacy code。不能把任意非零 legacy 值直接当 public API status。

对 Incremental + V4 的验证优化也应窄读：它可能跳过普通 pre-install verifier 路径，不等于跳过 Session 的 ApkLite 一致性、PMS 所有验证或最终事务提交。第 257 章会专门展开 DataLoader、Streaming 与 IncFS，这里只保留交界线。

## 15. staged、持久化与重启恢复：三个源码缺口决定排障方式

staged Session 的 `handleInstall()` 不构造普通 PMS 安装请求，而是调用 `mStagingManager.commitSession(this)`，随后 `destroyInternal()` 和 `dispatchSessionFinished(INSTALL_SUCCEEDED, "Session staged", null)`。由于未到 staged 终态，destroy 不一定设 `mDestroyed`，stageDir 也保留。这个 success 的准确含义是“载荷已交给 staged 管线”，不是“包已经应用”。

后续 `StagingManager` 经四个阶段推进，而不是让 staged APK 永远绕开 PMS：

1. `commitSession()` 记录会话并启动 pre-reboot verification。
2. 重启前，APEX 分支向 apexd submit 并核对签名；APK 分支从原 staged 目录抽出临时非 staged Session，以 dry-run 真正走一次 PackageInstaller/PMS 校验。checkpoint/rollback 准备完成后，`handlePreRebootVerification_End()` 先把会话置 ready，再通知 apexd ready。
3. 开机恢复由 Service 把 staged Session 交给 `restoreSession()`；未 committed 忽略，failed/applied 不再推进，destroyed 尝试安全 abandon，未 ready 的重启 pre-reboot verification，ready 的才进入 `resumeSession()`。
4. `resumeSession()` 核对 checkpoint 和 apexd 的激活/失败状态，再为 staged APK 创建临时非 staged Session 做实际 commit；所有 APK 完成后设置 applied，APEX 的 successful 还要按 checkpoint 条件立即或延后报告。

在这条链上，三个位的含义是：

- ready：已准备，等待重启应用；这是非终态；
- applied：已生效；
- failed：验证、激活或回滚流程失败。

每次三态变化都会 `markUpdated()` 并异步写 XML；只有 Service 已允许广播且 Session 未 destroyed 时才发 session updated broadcast，boot restore 阶段不会因此保证有广播。`isStagedAndInTerminalState()` 只把 applied/failed 算作终态，ready 仍要跨重启继续；applied/failed setter 清 stage。

“staged failure”还分两种清账路径。若 Session 前半程用 `dispatchSessionFinished(..., success=false)` 结束，Service 的 finish callback 会 abort 并从 `mSessions` 移除；若 pre-reboot/激活阶段只是 `setStagedSessionFailed()`，则它清 stage、写 failed 状态并走 staged-changed callback，不触发 `onSessionFinished()`，对象仍保留，等 terminal + 7 天的开机规则淘汰。相对地，首次 staged success 也不会立刻从当前集合移除，因为恢复和状态查询仍需要它。

XML 保存身份、created/updated time、stage、prepared/committed/destroyed/sealed、多包关系、staged 三态、若干 SessionParams、DataLoader 描述和文件列表。构造恢复对象时，磁盘 `sealed` 先进入 `mShouldBeSealed`，并未直接设置 `mSealed`；所有 Session 读入集合后再调用 `onAfterSessionRead()`，这样父 Session 检查 children 时可避免因加载顺序误判。恢复封印的 APEX 还会再次轻量验证以重建包名等运行时字段。

r48 有三个必须如实记录的恢复缺口：

1. `readFromXml()` 读取 `updatedMillis`，但构造函数没有该参数，也没有回填；构造函数直接令 `updatedMillis = createdMillis`。因此 staged 终态按“距最近状态变化 7 天”清理的意图，在重启后可能退化为按创建时间计算。
2. XML 并未覆盖所有 `SessionParams`。例如 `requiredInstalledVersionCode`、`forceQueryableOverride`、`rollbackDataPolicy` 没有在这份 r48 读写链中恢复。不能把内存态 `SessionInfo` 字段齐全误读为持久化也齐全。
3. 非 staged Session 的 3 天年龄检查只在 `readSessionsLocked()` 的开机恢复发生；源码常量和注释不能推出后台存在周期性定时清扫。staged 只有到 terminal state 且距 update 达 7 天才在这条恢复链判过期，但又受到第一项时间戳缺口影响。

transfer 的恢复还有一处独立缺口：XML 写下的是当前 installer UID 与当前 `InstallSource`，没有单独持久化 `mOriginalInstallerUid`/`mOriginalInstallerPackageName`。读回时，构造器会把当前 installer 再当成 original。由此直接推得，跨 system_server 重启后 `commitTransferred` 可能被判为“尚未 transfer”，普通 commit 反而按 owner 未变化继续，针对 original installer 的包名约束也随原始身份丢失。multi-package transfer 也不会递归改 children；目标 UID 真正改变时，父和每个 child 必须分别走匹配的 transfer，否则 child 封印会在 owner 或 transfer 方向门失败。

开机时有效 Session 被放回 `mSessions`，无效者进历史并保留 ID 为已分配；随后 stage reconcile 会清理孤儿目录。恢复是“XML 元数据 + stage 现实 + 状态重建”的交叉校验，不是只要 XML 能解析就继续安装。

### 练习 9：区分 staged 接收成功、应用终态与恢复缺口

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mStagingManager.commitSession(this);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F '"Session staged"' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void setStagedSessionReady()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void setStagedSessionApplied()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void setStagedSessionFailed' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private void handlePreRebootVerification_End' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'private void verifyApksInSession' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'private void checkStateAndResume' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'private void resumeSession' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'private void installApksInSession' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
grep -n -F 'this.updatedMillis = System.currentTimeMillis();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'writeLongAttribute(out, ATTR_UPDATED_MILLIS, updatedMillis);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'long updatedMillis = readLongAttribute(in, ATTR_UPDATED_MILLIS);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'this.updatedMillis = createdMillis;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'this.mShouldBeSealed = sealed;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void onAfterSessionRead()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'MAX_AGE_MILLIS = 3 * DateUtils.DAY_IN_MILLIS' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'MAX_TIME_SINCE_UPDATE_MILLIS = 7 * DateUtils.DAY_IN_MILLIS' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'session.isStagedAndInTerminalState()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'if (!session.isStaged() || !success)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerService.java
grep -n -F 'void restoreSession(@NonNull PackageInstallerSession session' frameworks/base/services/core/java/com/android/server/pm/StagingManager.java
```

特别核对 `updatedMillis` 局部变量的去向：读到不等于恢复到对象。

## 16. 故障树、完成点与下一章接口

遇到安装器报告“提交失败”，按最早失败边界定位，比从最终错误码倒猜更可靠：

| 观察 | 先查 | 关键证据 |
|---|---|---|
| create 直接抛异常 | calling UID、目标 user、限制、AppOps、flags 权限 | 尚无 session ID/stage |
| create 成功，open 失败 | owner、Session 是否仍在集合、prepareStageDir、空间 | ID 已登记，目录可能刚创建失败 |
| commit 同步抛异常 | child 身份、owner、prepared、开放 writer、FRP、transfer 方向 | Handler 可能尚未收到消息 |
| sealed 但不 committed | DataLoader 暂态或异步验证等待 | 写窗口不会重新打开 |
| 立刻最终失败 | ApkLite、base/split、包名、版本、签名、verity | 多数会 destroy 并派发失败 |
| pending user action | 当前 installer 的静默安装资格 | 已验证内容，尚未 relinquish |
| committed/relinquished 后失败 | PMS verification/prepare/scan/reconcile/commit | Session 成功不代表 PMS 成功 |
| staged 首次 success 后包未生效 | ready/applied/failed、重启、StagingManager | success 只是 staged 管线接收，ready 也非终态 |
| 重启后会话异常过期 | XML 字段、created/updated 恢复、stage reconcile | 注意 r48 时间戳回填缺口 |

五个完成点要分别命名：

1. create 完成：对象进 `mSessions`，调用者得到 ID。
2. write 完成：每个输出流已 fsync/close，没有开放写传输。
3. Session commit 完成：sealed 已同步持久化，内容验证成功，`mCommitted=true`。
4. 普通安装完成：PMS Observer 回来并向 `IntentSender` 发最终 public status。
5. staged 安装完成：`applied=true`；首次 `Session staged` 成功不算这个点。

自测时应能回答以下问题：为什么 create 成功却没有目录；为何 offset 0 重写可能保留旧尾；为什么 sealed 可以为真而 committed 为假；为什么 pending confirmation 不是失败；为何 child 不能单独 commit；Session 校验具体不替 PMS 做什么；为什么 multi-package 不应被描述成任意副作用的通用事务；staged 的 success、ready、applied 分别是什么；重启后 updated time 为何可能偏离磁盘值。

下一章进入第 257 章，沿本章 `prepareDataLoaderLocked()` 返回 false 的分支继续：DataLoader 如何 bind/create/start/prepareImage，Streaming 怎样通过文件系统 connector 供数，Incremental File System 如何建立按需页、健康监测与失败回调，以及这些异步事件怎样重新唤醒已经 sealed 的 Session。到那里，本文的核心接口是：**sealed 关闭普通 installer mutation，但预声明 DataLoader 文件仍可由 connector 物化；DataLoader ready 才允许内容验证推进，暂态不可用可以等待，不可恢复错误才结束事务。** r48 的 remove marker 与写入锁间隙仍是前文已经标出的例外。
