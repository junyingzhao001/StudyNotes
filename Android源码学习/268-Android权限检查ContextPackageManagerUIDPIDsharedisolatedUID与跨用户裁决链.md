# 268 Android 权限检查的多路径裁决链：Context、PackageManager、UID/PID、shared/isolated UID 与跨用户边界

调用 `checkPermission()` 时，系统究竟在检查谁？答案不是一句“查 Manifest”能够概括的：入口先决定身份从哪里来，AMS 可能按 PID 收缩某个进程的能力，组件入口还要判断 owner 与 exported，PMS 才读取 UID 对应的授权状态；如果目标位于另一个用户，前面还必须有独立的跨用户裁决。

全文基于本地 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。Android 12 以后权限服务继续迁移，下文的缓存冲突、包可见性分支和窄边界只对这个 r48 基线负责。

## 1. 先拆成五道门：身份、进程、组件、授权状态与用户

一条权限检查至少可能回答五个不同问题：

| 维度 | 代表输入 | 主要裁决者 | 它回答什么 |
|---|---|---|---|
| 身份来源 | Binder caller、self、显式 pid/uid | `ContextImpl`、调用方 | 本次准备检查哪个主体 |
| 进程收缩 | pid、`deniedPermissions` | AMS | 这个具体进程是否主动放弃 `INTERNET` |
| 组件边界 | owningUid、exported、permission | `ActivityManager.checkComponentPermission()` | 此主体能否进入这个具体组件 |
| 授权状态 | 完整 UID、package、`PermissionsState` | `PermissionManagerService` | 该 UID 或目标包的状态中是否有此名字 |
| 用户与可见性 | userId、allowMode、callingUid | `UserController`、PMS、PMS 包过滤 | 能否选择目标用户、能否观察目标包 |

这五道门不是每个 API 都全部经过。最需要先纠正的是两条常被混在一起的链：

- `Context.checkPermission(permission, pid, uid)` 是全局 permission 查询。AMS 固定传 `owningUid=-1`、`exported=true`，所以组件 owner 与 exported 分支在这条链上没有实际作用。
- Activity、Service、Provider 等具体入口可把真实 owningUid 和 exported 交给组件 helper。此时 owner、exported 与 null permission 才是有效分支。

因此“Context 检查最终经过组件 helper”只是代码复用关系，不表示它已经验证某个真实组件。反过来，一个真实操作也可能在 permission 位通过后继续受 AppOps、对象归属、前后台状态或业务策略限制；本章的 `GRANTED` 只表示当前这条基础裁决链的完成点。

主要源码如下：

| 层次 | 文件 |
|---|---|
| Context 与客户端缓存 | `ContextImpl.java`、`ApplicationPackageManager.java`、`PermissionManager.java` |
| Binder 与 UID 编码 | `Binder.java`、`UserHandle.java` |
| AMS 与进程策略 | `ActivityManagerService.java`、`ActivityManager.java`、`ProcessRecord.java`、`ProcessList.java` |
| 包与权限状态 | `PackageManagerService.java`、`PermissionManagerService.java`、`PermissionsState.java` |
| Manifest 进程配置 | `ParsedProcessUtils.java`、`PackageInfoUtils.java` |
| 跨用户 | `UserController.java`、`PermissionManagerService.java` |

## 2. UID 是安全主体，PID 是一次进程实例，Binder identity 属于当前线程

r48 开启多用户时，完整 UID 由 userId 与 appId 组合。`PER_USER_RANGE` 是 100000，`getUid(userId, appId)`计算 `userId * 100000 + appId % 100000`；`getUserId(uid)`取商，`getAppId(uid)`取余。同一应用在 user 0 与 user 10 通常有相同 appId，却有不同完整 UID。

`UserHandle.isSameApp()`只比较 appId，刻意忽略 userId；`isSameUser()`才比较用户部分。这个区别解释了为什么组件 helper 的 same-app 放行不能单独承担跨用户保护：user 0 的 appId 10042 与 user 10 的 appId 10042 会被视为 same app，上层必须先保证目标用户合法。

PID 回答的是“当前哪一个 Linux 进程实例”。进程重启后 PID 会改变，也可能被复用；多个进程可以共享同一 UID，shared UID 的多个包也可以共享一个 UID。`PermissionsState`主要绑定 UID/包安全主体，而 r48 的 process deny 才把 PID 重新带进结果。

Binder identity 还要按线程理解：

| 当前执行状态 | `Binder.getCallingPid/Uid()`读到什么 |
|---|---|
| 正在处理未清身份的入站 Binder 事务 | 事务发送者 |
| 在该线程同步调用同进程 Java helper | 仍是原事务发送者 |
| 没有入站 Binder 事务 | 当前进程自身 |
| 调过 `clearCallingIdentity()`且尚未 restore | 当前进程自身 |
| Runnable 换到普通 Handler/Executor 线程 | 通常是执行进程自身 |
| 服务 B 再发 Binder 到另一个进程 C | C 默认看到 B，而非最初的 A |

所以“Java 本地调用总是 self”并不成立。只要同一线程仍在处理 A→B 的事务，B 的同步 helper 继续看见 A；变化发生在清身份、离开事务、换线程，或建立新的跨进程 Binder hop 时。

### 练习 1：把 userId、appId、PID 与 Binder 线程身份分开

先核对 UID 算法，再核对 Binder 对“当前事务”和“无事务”的定义。为六种执行状态各写一行 callingPid/callingUid 来源，不要用“本地调用”作为唯一判断条件。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final int PER_USER_RANGE = 100000;' frameworks/base/core/java/android/os/UserHandle.java
grep -n -F 'return uid / PER_USER_RANGE;' frameworks/base/core/java/android/os/UserHandle.java
grep -n -F 'return userId * PER_USER_RANGE + (appId % PER_USER_RANGE);' frameworks/base/core/java/android/os/UserHandle.java
grep -n -F 'return getAppId(uid1) == getAppId(uid2);' frameworks/base/core/java/android/os/UserHandle.java
grep -n -F 'public static final native int getCallingPid();' frameworks/base/core/java/android/os/Binder.java
grep -n -F 'public static final native int getCallingUid();' frameworks/base/core/java/android/os/Binder.java
grep -n -F 'public static final native boolean isHandlingTransaction();' frameworks/base/core/java/android/os/Binder.java
grep -n -F 'public static final native long clearCallingIdentity();' frameworks/base/core/java/android/os/Binder.java
grep -n -F 'public static final native void restoreCallingIdentity(long token);' frameworks/base/core/java/android/os/Binder.java
```

## 3. Context 四种 API 的差别，是身份来源与 self 失败策略

`ContextImpl`提供四种基础检查。对非 null permission，它们最终都返回 `PERMISSION_GRANTED` 或 `PERMISSION_DENIED`，但构造 pid/uid 的方式不同；四个入口遇到 null 都先抛 `IllegalArgumentException`。AMS Binder global 入口的 null→`DENIED`是更下层、且 Context 普通调用到不了的合同。

| API | pid/uid 来源 | 外部 Binder 事务 | 无事务或 clear 后 | 适合的合同 |
|---|---|---|---|---|
| `checkPermission(p,pid,uid)` | 调用者显式提供 | 不自动改写 | 不自动改写 | 替一个可信、已解析的主体查询 |
| `checkSelfPermission(p)` | `Process.myPid/myUid` | 仍固定 self | self | 查询当前进程自身 |
| `checkCallingPermission(p)` | Binder calling pid/uid | 查外部 caller | callingPid 等于 myPid 时直接拒绝 | 只接受真正外部 caller 的入口 |
| `checkCallingOrSelfPermission(p)` | Binder calling pid/uid | 查外部 caller | 查 self | 明确允许 caller 或本服务自身 |

`checkCallingPermission()`的 self-deny 是有意的防误用设计。它先读取 callingPid，只有 `pid != Process.myPid()`才继续查 callingUid；若当前身份是 self，直接 `DENIED`。但同步 Java helper 若仍承载外部 Binder identity，callingPid 仍是外部 PID，不会因为“方法调用发生在同进程”而被拒绝。

`checkCallingOrSelfPermission()`没有这层拒绝。它适合确实允许内部调用与外部有权 caller 共用的路径，也意味着 system_server 清身份后再调用它，会用 system_server 的 pid/uid。由于 AMS 先对自己的 PID 无条件放行，非 null permission 在这种场景会直接通过，而不只是“system UID 大概率有权限”。

`enforcePermission()`、`enforceCallingPermission()`和 `enforceCallingOrSelfPermission()`没有另一个更强的权限模型；它们调用对应 check，失败时统一抛 `SecurityException`。异常字符串主要包含 uid、permission 与调用方给的 message，并不是完整审计记录，也不包含所有失败原因。

还有一个隐藏重载 `checkPermission(permission,pid,uid,callerToken)`。它直接调用 AMS 的 `checkPermissionWithToken()`，不经过 `PermissionManager`的普通客户端 cache。第 14 节再解释 token 只在哪个受控同步窗口内有意义。

### 练习 2：逐行推演四种 Context 检查

分别代入“外部事务”“同线程同步 helper”“无事务”“clear 后”四种状态，记录每个入口使用的 pid/uid，以及 check 与 enforce 的唯一差别。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public int checkPermission(String permission, int pid, int uid) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'return PermissionManager.checkPermission(permission, pid, uid);' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'return ActivityManager.getService().checkPermissionWithToken(' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'public int checkCallingPermission(String permission) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'if (pid != Process.myPid()) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'public int checkCallingOrSelfPermission(String permission) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'return checkPermission(permission, Binder.getCallingPid(),' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'public int checkSelfPermission(String permission) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'return checkPermission(permission, Process.myPid(), Process.myUid());' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'if (resultOfCheck != PERMISSION_GRANTED) {' frameworks/base/core/java/android/app/ContextImpl.java
```

## 4. 两张 16 项客户端缓存共享失效代际，却缓存了不同的问题

普通 Context 链先进入 `PermissionManager.sPermissionCache`。cache miss 时，`checkPermissionUncached()`取得 AMS：AMS 不存在的极早启动或测试场景，root/system appId 被假定 `GRANTED`，其他 UID 被保守地判为 `DENIED`；正常场景则调用 `IActivityManager.checkPermission(permission,pid,uid)`。

`PermissionQuery`虽然保存 permission、pid、uid，`recompute()`也把三者传给 AMS，但 `equals()`与 `hashCode()`只比较 permission+uid。源码注释认为“实际安全检查只按 UID”，所以忽略 PID 可以少 miss；第 5、6 节会看到 r48 的 AMS 明明还有两项 PID 语义，这个假设并不完整。

另一张 `sPackageNamePermissionCache`服务 `PackageManager.checkPermission(permission, packageName)`，key 是 permission+packageName+第三个整数。r48 把第三个字段命名为 `uid`，但 `ApplicationPackageManager`实际传的是 `getUserId()`，服务端 AIDL 语义也是 userId。读缓存 dump 时应把它当 userId，不能按完整 UID 解码。

两张 cache 都最多 16 项，并共享 `CACHE_KEY_PACKAGE_INFO` 的 PropertyInvalidatedCache 代际。常规 grant、revoke、reset、flags 更新以及包/delegate 变化路径会触发 `PackageManager.invalidatePackageInfoCache()`；这是一套跨进程代际失效机制，不是逐个找到所有客户端 Java Map 清空，也不能推成任意 `PermissionsState`内部赋值都自动失效。为了避免 system_server 内部递归与陈旧结果，PMS 构造时还禁用本进程的 permission 与 package-name 两张本地 cache。

package-name cache 的 key 结构上不含服务端 visibility 使用的 callingUid。不过在 r48 标准部署中，普通进程向远端 permission 服务发起新 Binder 调用时，对端看见的是查询进程自身 UID，而不是该进程正在服务的上游 caller；system_server 的本地路径又已禁用这张 cache。因此不能直接宣称普通多-caller 服务必然发生可见性污染。这个缺维风险应限定在自定义同进程部署、测试替身或 cache 未禁用的非常规路径；它仍说明失效代际只能处理“状态变了”，不能补回 key 从未编码的上下文。

### 练习 3：找出 cache key 中缺失的两个维度

对 UID cache 标出 PID 如何参加 recompute 却不参加 equality；对 package-name cache 标出 userId 的误命名和 callingUid 的缺席。最后确认 PMS 为何禁用自己的两张本地 cache。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static int checkPermissionUncached(@Nullable String permission, int pid, int uid) {' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'final IActivityManager am = ActivityManager.getService();' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return uid == other.uid' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F '&& Objects.equals(permission, other.permission);' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F '16, CACHE_KEY_PACKAGE_INFO) {' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return sPermissionCache.query(new PermissionQuery(permission, pid, uid));' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'PackageNamePermissionQuery(@Nullable String permName, @Nullable String pkgName, int uid) {' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return Objects.hash(permName, pkgName, uid);' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return Objects.equals(permName, other.permName)' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F '&& Objects.equals(pkgName, other.pkgName)' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return sPackageNamePermissionCache.query(' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F '.checkPackageNamePermission(permName, pkgName, getUserId());' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'mInjector.disablePermissionCache();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mInjector.disablePackageNamePermissionCache();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'public static void invalidatePackageInfoCache() {' frameworks/base/core/java/android/content/pm/PackageManager.java
```

## 5. Context 的全局链在 AMS 先看 system_server PID，再看进程拒绝

AMS 的公开 `checkPermission()`先把 null permission 判为 `DENIED`，随后固定调用 `checkComponentPermission(permission,pid,uid,-1,true)`。这组实参关闭了真实组件的 owner/exported 分支，但保留 AMS 外层的 PID 规则和 `ActivityManager` helper 的 core/isolated/UID 规则。

精确顺序是：

1. 若 `pid == MY_PID`，立即 `GRANTED`；
2. permission 非 null 时，以 pid 查活动 `ProcessInfo`，命中该进程的 `deniedPermissions` 就 `DENIED`；
3. 再进入 `ActivityManager.checkComponentPermission()`；
4. global 链因 owningUid=-1、exported=true，不会在 owner/exported 分支结束；
5. root/system appId 通过，isolated UID 拒绝，普通主体最终进入 `IPackageManager.checkUidPermission()`。

第一步只认 PID，不核对同时传入的 UID 是否真属于该 PID。后面的 process deny 也按 pid 查配置，而最终授权按显式 uid 查状态。换言之，`checkPermission(pid,uid)`是两个维度的查询，不是 AMS 帮调用方认证一对 pid/uid；把客户端自报整数原样送进来，客户端既可伪造特权 UID，也可填 system_server PID 命中第一步。

`MY_PID`分支也意味着 system_server 中的 `checkSelfPermission()`以及 clear 后的 OrSelf，对任意非 null permission 都在这里通过，甚至不读取权限定义。鉴权必须在 clear 之前完成，除非业务合同明确就是授权 system_server 自身。

非 system_server 的 root/system-appId 受管进程则不同：它们先经过 process deny，再到 helper 的 core 放行。因此它们可以通过进程配置主动放弃 `INTERNET`；system_server 自己已经在第一步返回，不会被这张 per-process deny 表拒绝。

## 6. `<processes>` 只收缩 INTERNET，并在 Java 检查与 Linux GID 两处生效

r48 解析 Manifest 的 `<processes>`时，外层 `<deny-permission>`/`<allow-permission>`维护默认集合；每个 `<process>`先复制默认集合，再用自己的子项增加或移除。重复 process 名会成为解析错误。

虽然集合类型能装任意字符串，`parseDenyPermission()`与 `parseAllowPermission()`都只接受 `android.permission.INTERNET`。别的名字不会变成通用的“按进程撤销任意 Android 权限”机制。它与设置页中的 runtime permission flags 也不是同一份状态。

这份配置有两个独立生效点：

- `ProcessRecord`按 `_uid`取得进程表，再按 `_processName`取 `ProcessInfo`。AMS `addPidLocked()`把非空配置登记到 pid Map，移除进程时删掉，供 Java permission check 的第二步使用。
- `ProcessList`启动非 isolated 进程时，先取得包的 permission GIDs；对 denied permission 取得对应 GID 并从数组移除，再把结果传给 Zygote。即使某次 Java 查询绕开 AMS 包装层，已经启动的进程也不会凭空拿回被删的 Linux 附加组。

这又暴露了 UID cache 的 r48 冲突。同一检查进程替同一 UID 的两个 PID 查询 `INTERNET`时，一个进程可以 deny、另一个不 deny，但 key 只含 permission+uid；先填入的结果可能污染后一个。忽略 pid 还会遮蔽 `pid == MY_PID` 快路，而且 `addPidLocked()`/`removePidLocked()`本身并不失效 package-info cache。

普通应用的两个进程各有各的地址空间，若都只查 self，cache 不跨进程共享，所以问题不容易出现。风险集中在同一进程替多个目标 PID 查询的服务或测试工具。system_server 中 PMS 已禁用本地 permission cache，缓解了主路径；这个缓解不改变 key 设计本身。

### 练习 4：把解析、活动 PID 表与 Zygote GID 串起来

先证明默认集合会复制给具体 process、且 r48 只识别 INTERNET；再从 `ProcessRecord`追到活动 PID Map和启动 GID 移除。最后手算同 UID 的 P1/P2 只有 P2 deny 时，哪一个先填 cache 会造成哪种错误。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (perm != null && perm.equals(android.Manifest.permission.INTERNET)) {' frameworks/base/core/java/android/content/pm/parsing/component/ParsedProcessUtils.java
grep -n -F 'proc.deniedPermissions = new ArraySet<>(perms);' frameworks/base/core/java/android/content/pm/parsing/component/ParsedProcessUtils.java
grep -n -F 'ParseResult<Set<String>> denyResult = parseDenyPermission(deniedPerms, res,' frameworks/base/core/java/android/content/pm/parsing/component/ParsedProcessUtils.java
grep -n -F '_service.mPackageManagerInt.getProcessesForUid(_uid);' frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
grep -n -F 'procInfo = processes.get(_processName);' frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
grep -n -F 'sActiveProcessInfoSelfLocked.put(app.pid, app.processInfo);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'sActiveProcessInfoSelfLocked.remove(app.pid);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'ProcessInfo procInfo = sActiveProcessInfoSelfLocked.get(pid);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'permGids = pm.getPackageGids(app.info.packageName,' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'mService.mPackageManagerInt.getPermissionGids(' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'permGids = ArrayUtils.removeInt(permGids, gid);' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
```

## 7. 真实组件 helper 有六个结束点，global check 只可能在其中三个结束

AMS 外层没有提前结束时，`ActivityManager.checkComponentPermission(permission,uid,owningUid,exported)`按以下固定顺序裁决：

| 顺序 | 条件 | 结果 | 关键边界 |
|---:|---|---|---|
| 1 | appId 是 root 或 system | `GRANTED` | AMS 的 per-process deny 已发生在它之前 |
| 2 | UID 是 isolated | `DENIED` | 不继承宿主 App 的 grant |
| 3 | owningUid 有效且 `isSameApp` | `GRANTED` | 比 appId，不比 userId；shared UID 成员也命中 |
| 4 | `exported == false` | `DENIED` | 持有再强的 permission 也不能覆盖 |
| 5 | permission 为 null | `GRANTED` | 这里只表示导出组件没有具名门 |
| 6 | 其余 | `checkUidPermission` | 才进入 PMS 状态检查 |

对 Context global check，owningUid 固定 -1，第三步永不命中；exported 固定 true，第四步永不命中；permission 在 AMS 公开入口已保证非 null，第五步也不会命中。它实际只利用 core、isolated 与最终 UID 查询，外加 AMS 更早的 self PID/process deny。

真实组件入口的结论不同。包 A 与包 B 若共享 UID，B 访问 A 的非导出组件会在 same-app owner 处通过；Linux 权限主体无法用 UID 区分这两个包。即使不 shared UID，同一 appId 的不同用户也会被 `isSameApp()`视为相同，所以调用组件 helper 前的上层跨用户门是安全前提，而不是可选补丁。

null 也有两种语义：AMS 公开 global `checkPermission(null,...)`直接拒绝；真实组件 helper 中 null 是“没有额外声明 permission”，只要此前 core/isolated/owner/exported 已裁决完，就允许进入。把这两个 null 分支画成一个节点会得到相反结论。

组件 helper 只返回 permission 层结果。Provider 可能随后检查 URI grant 与 AppOps，Service/Activity 启动还有解析、用户、后台启动与业务政策；“same-app 已放行”也不等于整个操作的所有门都结束。

### 练习 5：用同一 helper 手算 global 与真实组件两张表

先按源码顺序推演 root、isolated、same-app 非导出、外部有权但非导出、导出无 permission 五个组件案例；再把 owningUid=-1/exported=true 代入，删去 global 链永远不可能命中的分支。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (pid == MY_PID) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F '&& procInfo.deniedPermissions.contains(permission)) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'return checkComponentPermission(permission, pid, uid, -1, true);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (appId == Process.ROOT_UID || appId == Process.SYSTEM_UID) {' frameworks/base/core/java/android/app/ActivityManager.java
grep -n -F 'if (UserHandle.isIsolated(uid)) {' frameworks/base/core/java/android/app/ActivityManager.java
grep -n -F 'if (owningUid >= 0 && UserHandle.isSameApp(uid, owningUid)) {' frameworks/base/core/java/android/app/ActivityManager.java
grep -n -F 'if (!exported) {' frameworks/base/core/java/android/app/ActivityManager.java
grep -n -F 'if (permission == null) {' frameworks/base/core/java/android/app/ActivityManager.java
grep -n -F '.checkUidPermission(permission, uid);' frameworks/base/core/java/android/app/ActivityManager.java
```

## 8. PMS 的 package-name 路径与 UID 路径不是同一种查询

`PermissionManagerService`有两个基础入口：

| 项目 | `checkPermission(perm,pkg,userId)` | `checkUidPermission(perm,uid)` |
|---|---|---|
| 目标输入 | 明确包名与 userId | 完整 UID |
| 首个用户门 | user 必须存在 | 从 UID 取出的 user 必须存在 |
| 找目标 | 全局 `mPackages.get(name)` | `getPackagesForUidInternal(uid,SYSTEM_UID)`后取首个包 |
| 安装状态 | 没有等价的 `getInstalled(userId)`前置门 | 只返回该 user 已安装成员 |
| 可见性 | 对显式目标包做过滤 | 普通 UID 做；shared UID 走特殊分支 |
| 无包时 | `DENIED` | 查 `mSystemPermissions[uid]` |

package-name 入口检查的是“指定目标包在指定用户的状态”，不是查询者自身是否有 permission。`ApplicationPackageManager`用当前 Context 的 `getUserId()`构造目标 user；服务端再用到达 PMS 的直接 Binder callingUid做 package visibility 过滤，代理调用时这个身份是代理进程而非更上游的业务发起者。null、用户不存在、包不存在、不可见、没有 grant、instant 不适用，最后都可能只是同一个 `DENIED`，调用者不能靠结果区分目标是否存在。

这里有一个重要的 user 安装边界。`getPackage(packageName)`直接从全局包表取 `AndroidPackage`，后续 `filterAppAccess()`不是目标安装状态检查；`PermissionsState`对 install permission 又会把 userId 归一到 `USER_ALL`。因此在一个真实存在的 user 上，即使目标包没有安装给该 user，只要调用者能看见目标且全局 install grant 存在，显式包名检查仍可能 `GRANTED`。

UID 路径不会这样选代表包。`getPackagesForUidInternal()`从完整 UID 提取 userId，对普通 `PackageSetting`要求 `ps.getInstalled(userId)`；shared UID 也只收集该 user 已安装的成员。没有成员时才落到 SystemConfig UID 集合。于是不能把 package-name 查询与 UID 查询互相替代，更不能把前者的 `GRANTED`解释成“目标包已安装给这个用户”。

两个入口都会先读取 `mCheckPermissionDelegate`。r48 的实际使用者之一是 shell 为 instrumentation 建立的 permission identity delegate；它是 system_server 内部受控机制，并非普通应用可注册的全局 hook。delegate 安装、移除或改变目标权限时会使 package-info cache 失效。

### 练习 6：对照普通包、shared UID 与用户安装完成点

沿 package-name 与 UID 两条找包路径对照 `getInstalled(userId)`，再证明成员的 `PackageSetting`会转到 shared state，并读 install permission 对 `USER_ALL` 的归一。先构造“user 10 存在但普通包只装在 user 0”；再为 shared UID 分别推演“user 10 没有任何成员”和“目标成员未装、但另一个成员已装在 user 10”两个子案例，预测 package-name 与 UID API 会在哪个分支结束。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return checkPermissionInternal(pkg, true, permName, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (isPackageExplicit || pkg.getSharedUserId() == null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.filterAppAccess(pkg, callingUid, userId)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final AndroidPackage pkg = mPackageManagerInt.getPackage(uid);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return checkUidPermissionInternal(pkg, uid, permName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return mPackages.get(packageName);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final String[] packageNames = getPackagesForUidInternal(uid, Process.SYSTEM_UID);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final SharedUserSetting sus = (SharedUserSetting) obj;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (ps.getInstalled(userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'return (sharedUser != null)' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'if (isInstallPermission()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'userId = UserHandle.USER_ALL;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
```

## 9. shared UID 共享一份 PermissionsState，代表包只负责把 UID 带进来

普通包的 `PackageSetting.getPermissionsState()`返回自己的状态；shared UID 成员的 setting 指向 `SharedUserSetting`的共享状态。权限恢复阶段按成员请求并集维护它，基础 check 到这里只问这份共同 state 是否有名字，并不会“分别检查每个包再取交集”。

UID 路径先从该 user 已安装的 shared 成员中选择第一个能在 `mPackages`找到的代表包。代表包不是权限独占者：`checkPermissionInternal(pkg,false,...)`取得它的 `PackageSetting`后，读到的仍是 shared `PermissionsState`。本路径的 grant 结果不应因选中哪个已安装成员而改变；更基本的边界是 UID 只能回答共同安全主体能力，不能唯一回答“是哪一个 APK 在做事”。

可见性也专门分叉：

- 显式 package-name 查询总按指定成员做 `filterAppAccess()`，随后读取 shared state。
- UID 查询若代表包不是 shared UID，仍按该包过滤。
- UID 查询若代表包属于 shared UID，不拿一个任意成员代表整个 UID 做过滤；但若查询者是 instant app，直接拒绝观察该 shared UID。

shared UID 同时削弱了包级归因和组件隔离。成员共享权限位，`isSameApp()`也把成员视为同一 owner；设计上必须把 shared UID 当作共同信任域。若业务策略需要区分包名，还要验证所报 packageName 确实属于 callingUid，并结合签名、AppOps package mode 或受控 token，而不是把字符串当身份。

用户维度仍然存在。runtime permission 存在 shared state 的每用户槽位；user 0 的 grant 不自动复制到 user 10。install permission 则用 `USER_ALL`状态，但 UID 代表包选择仍要求至少一个 shared 成员已安装给目标 user。正是这两个层次的组合，让“共享状态是全局的”与“任意用户都可查询到包”成为两件事。

## 10. instant、isolated、SystemConfig 与 fuller 都会改变最后一跳

PMS 取得 `PermissionsState`后，`checkSinglePermissionInternal()`先按名字调用 `hasPermission(permissionName,userId)`。普通目标有该状态便返回 true；目标是 instant app 时，还要求当前权限定义带 instant 能力，否则拒绝。这里要分清查询者 instant 与目标 instant：前者影响可见性/shared UID 观察，后者限制已授名字是否可实际通过。

直接名字失败后才查 `FULLER_PERMISSION_MAP`：

| 请求的较弱 permission | 可替代的较强 permission |
|---|---|
| `ACCESS_COARSE_LOCATION` | `ACCESS_FINE_LOCATION` |
| `INTERACT_ACROSS_USERS` | `INTERACT_ACROSS_USERS_FULL` |

方向只从强到弱。fuller 检查再次调用同一个 single helper，所以目标 instant app 仍要求较强 permission 定义也允许 instant。它不改 requested 列表、不补 grant，也不是上一章的 split permission；它只是每次查询时的蕴含规则。

若 UID 找不到包，PMS 在 `mSystemPermissions`中按完整 uid 查字符串集合，然后也尝试 fuller 映射。这条路服务 native daemon 等没有 APK `PackageSetting`的主体。package-name API 没有对应包时只会拒绝，不会凭一个包名映射到 SystemConfig。

isolated UID 在 global Context 链已经被 `ActivityManager` helper 明确拒绝。若某个内部调用直接进入 PMS UID API，它通常找不到包，也没有 SystemConfig 项，所以仍会拒绝；但这是“没有命中状态”的结果，不是 PMS 重复执行 isolated 硬门。入口差异必须保留。

root/system 豁免也同样属于入口，而非 `PermissionsState`的普遍定律。ActivityManager helper 直接放行 core appId，PermissionManager 在 AMS 缺失时也放行；显式 PMS package/UID 查询则走自己的包状态或 SystemConfig 逻辑。具体业务服务还可以继续要求用户限制、对象归属或 AppOps。

r48 还有一个反直觉的定义边界：普通目标的 single helper 不先查询 `mPermissions`里是否仍有 `BasePermission`，只按同名 state 判断。因此未知名字通常是 `DENIED`，但定义刚删除而 grant state 尚未清理的窗口中，残留名字仍可能 `GRANTED`。目标 instant app 会额外调用 `isPermissionInstant()`，缺失定义通常在这一步失败。需要判断“定义是否存在”时必须另查 `getPermissionInfo()`；基础 check 的结果不能替代定义查询。

### 练习 7：画出 ordinary、instant、isolated 与 native UID 的末端矩阵

分别跟踪直接 permission、fuller permission、无包 SystemConfig 和残留 state。特别标出哪条链检查定义、哪条只检查字符串状态，以及 isolated 硬拒绝究竟发生在哪一层。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!permissionsState.hasPermission(permissionName, UserHandle.getUserId(uid))) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (mPackageManagerInt.getInstantAppPackageName(uid) != null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return mSettings.isPermissionInstant(permissionName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FULLER_PERMISSION_MAP.put(Manifest.permission.ACCESS_COARSE_LOCATION,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FULLER_PERMISSION_MAP.put(Manifest.permission.INTERACT_ACROSS_USERS,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (checkSingleUidPermissionInternal(uid, permissionName)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'ArraySet<String> permissions = mSystemPermissions.get(uid);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return permissions != null && permissions.contains(permissionName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (UserHandle.isIsolated(uid)) {' frameworks/base/core/java/android/app/ActivityManager.java
```

## 11. `DENIED` 会折叠原因，`GRANTED` 也只冻结检查时刻

两条 PMS 路径都先确认 user 存在，但之后大量原因折叠到同一个整数：null 参数、目标包不存在、包对 caller 不可见、没有同名 state、instant 定义不适用，都会表现为 `DENIED`。UID 在该用户没有已安装成员时还会尝试同一完整 UID 的 SystemConfig direct/fuller 项；这也没有命中才拒绝。这种折叠有意避免权限查询变成包枚举旁路，也意味着异常消息和单次返回值都不足以做根因审计。

反过来，`GRANTED`只冻结检查时刻的这一层状态。package-name 结果不证明目标 user 已安装该包，component owner 放行不证明存在 grant，shared UID 结果不能唯一归因到成员，残留 state 也不必证明定义仍在；AppOps、URI grant、传感器隐私与业务对象归属更是后续门。

常规 grant、revoke、reset、updateFlags 及包或 delegate 更新路径会失效客户端 cache，但不能推成任意内部状态赋值都会自动失效；检查与使用之间也仍有竞态。对一次同步 Binder 操作，常见做法是在入口检查并立即完成受保护动作；长事务、持续资源与异步工作还需要会话 token、死亡监听、AppOps active/finish 或重新验证。旧 pid 可能复用，旧 uid 也可能在包卸载后重分配，不能把保存数小时的整数当永久授权票。

诊断时至少记录 API、pid/uid 来源、当前 Binder identity、目标 user、shared/instant/isolated 状态、process deny、delegate 与 cache 代际；只看 `SecurityException`一句话会丢失大多数分支。

## 12. `handleIncomingUser()` 是参数归一化加授权，不是“用户存在”证明

许多 AMS 入口用 `UserController.handleIncomingUser(callingPid,callingUid,userId,allowAll,allowMode,name,callerPackage)`处理目标 user。它的顺序本身就是合同：

1. raw userId 已等于 callingUserId 时立即返回；
2. `USER_CURRENT`或 `USER_CURRENT_OR_SELF`转换为当前用户快照；
3. 非 root/system caller 才进入 Recents、FULL 与 allowMode 权限树；
4. 未获允许且原参数是 `USER_CURRENT_OR_SELF`时，降级到 callingUserId；其他请求抛 `SecurityException`；
5. `allowAll=false`时拒绝仍为负数的特殊 user；
6. shell 访问非负目标且该用户禁止调试功能时，再抛 `SecurityException`；
7. 返回归一后的 targetUserId。

第一步早于特殊值校验与 shell restriction。raw userId 等于 calling user 时必然是普通非负值，所以不会让 `USER_ALL`漏过；但 shell 访问自己的 user 会直接返回，不执行末尾的 `DISALLOW_DEBUGGING_FEATURES`检查。这与 PMS helper 的顺序不同。

第二步只是读取 current user 的一个快照，源码也承认它与用户切换存在竞态。`USER_CURRENT_OR_SELF`的“OR SELF”只在 caller 没有当前用户访问资格时生效：它不是一开始就固定 self，也不是静默吞掉所有非法 user。

非 root/system caller 的允许树如下：

| 优先级 | 条件 | 是否还受 allowMode/profile 限制 |
|---:|---|---|
| 1 | 受信任 Recents 且同 profile group | 直接允许 |
| 2 | `INTERACT_ACROSS_USERS_FULL` | 直接允许 |
| 3 | `ALLOW_FULL_ONLY` | 没有 FULL 就失败 |
| 4 | 特定 mode、同 profile、`INTERACT_ACROSS_PROFILES` preflight | 允许 |
| 5 | 没有 `INTERACT_ACROSS_USERS` | 失败 |
| 6 | `ALLOW_NON_FULL` | 普通跨用户 permission 足够 |
| 7 | 两种 profile-only mode | 还必须同 profile group |

root/system 只跳过第 3 步的普通权限树，之后仍受 `allowAll`的特殊值校验；shell 是 UID 2000，不属于这个跳过分支。返回一个非负 userId 也不表示它真实存在：`handleIncomingUser()`本身没有统一调用 `mUserManagerInt.exists()`，具体业务入口仍需按自己的对象查询与存在性合同处理。

## 13. PMS 跨用户 helper 与 UserController 有六处不可互换的差别

权限授予、撤销与 flags API 常用 PMS 自己的 `enforceCrossUserPermission()`或 `enforceCrossUserOrProfilePermission()`。把它与 `handleIncomingUser()`对照，差异比名字看起来更大：

| 维度 | `UserController.handleIncomingUser` | PMS helpers |
|---|---|---|
| 特殊 user | 识别 CURRENT/CURRENT_OR_SELF，可由 allowAll 接受部分特殊值 | 一开始拒绝任何负 userId |
| 同 user | raw 相同立即返回 | 可用 `requirePermissionWhenSameUser`强制仍需特权 |
| shell restriction | 位于末尾，同 user 早退会跳过 | 可选检查位于同-user shortcut 之前 |
| 受信任例外 | Recents + 同 profile | 没有 Recents 分支 |
| 非 FULL 策略 | 由多种 allowMode 控制 | `hasCrossUserPermission()`由 boolean 选择 FULL-only 或 FULL/普通 |
| profile permission | 真实 callingPid、callerPackage 做 preflight | `PID_UNKNOWN`加 callingUid 的代表包名 |

`hasCrossUserPermission()`在 `requirePermissionWhenSameUser=false`且 user 相同时先通过，然后放行 root/system；需要 FULL 时只收 `INTERACT_ACROSS_USERS_FULL`，否则 FULL 或普通权限均可。这个 boolean 只控制该 helper，不会关闭下一段 cross-profile fallback。它也不负责把 CURRENT 解析为具体用户，更不单独证明正 userId 存在，调用它的具体 API通常在别处完成存在性检查。

cross-profile 版本先尝试上述跨用户权限；失败后，即使参数要求 FULL，代码结构上仍会在同 profile group 时调用 `PermissionChecker.checkPermissionForPreflight(INTERACT_ACROSS_PROFILES,PID_UNKNOWN,callingUid,packageName)`。r48 的两个实际调用点都传 `requireFullPermission=false`，但不能把这一事实误写成 helper 自身有对应 guard。为查 profile group，helper 临时 clear identity，并在 finally restore；这段清身份只包围 UserManager 查询，不改变前面保存的 callingUid。

r48 的窄边界是 packageName 直接来自 `mPackageManagerInt.getPackage(callingUid).getPackageName()`，没有 null 检查。无法映射包的 isolated/native UID 若越过上层限制并走到这条 fallback，可能触发空对象异常，而不是整齐地返回拒绝。它不应被复制成通用安全模板。

### 练习 8：对照两套跨用户状态机

推演四个案例：普通 caller 请求 CURRENT_OR_SELF、同 profile caller 只有跨 profile 权限、shell 访问受限制的同 user、无法映射包的 UID 进入 profile fallback。每例写出最早结束的源码分支。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (callingUserId == userId) {' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'int targetUserId = unsafeConvertIncomingUser(userId);' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'if (userId == UserHandle.USER_CURRENT_OR_SELF) {' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'if (!allowAll) {' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'if (callingUid == Process.SHELL_UID && targetUserId >= UserHandle.USER_SYSTEM) {' frameworks/base/services/core/java/com/android/server/am/UserController.java
grep -n -F 'if (userId < 0) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'PackageManagerServiceUtils.enforceShellRestriction(mUserManagerInt,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!requirePermissionWhenSameUser && userId == callingUserId) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'PermissionChecker.PID_UNKNOWN,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.getPackage(callingUid).getPackageName())' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final long identity = Binder.clearCallingIdentity();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Binder.restoreCallingIdentity(identity);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

## 14. 受控的高级边界：clear/restore 改线程身份，callerToken 只覆盖局部参数

`Binder.clearCallingIdentity()`重置当前线程的入站身份并返回 opaque long token；`restoreCallingIdentity()`恢复它。安全的基本次序是：在原 identity 下取得 callingUid/pid，完成 permission、package 归属、target user 与对象能力检查；保存已经验证的内部参数；必要时 clear，以服务身份调用内部组件；无论成功或异常都在 finally restore。

这套 token 与 `checkPermissionWithToken()`的 `IBinder callerToken`不是一种东西：

| token | 类型 | 作用 |
|---|---|---|
| clear 返回值 | opaque `long` | 恢复当前 Binder 线程 identity |
| indirect caller token | `IBinder`对象 | 在 AMS 特定同步代理窗口中匹配保存的 pid/uid |

`openContentUri()`展示了第二种机制。AMS 在代表外部 App 同步调用 ContentProvider 前，新建 Binder token，把 token 与原 calling pid/uid放入 `ThreadLocal<Identity>`，并把 token 传给 provider 的 `openFile()`；finally 中移除 ThreadLocal。Provider 做 permission/URI check 时把 token 带回 AMS，只有当前嵌套调用线程仍有 Identity 且 `tlsIdentity.token == callerToken`，AMS 才用保存值覆盖这一次检查的局部 pid/uid。

它没有调用 `restoreCallingIdentity()`，没有永久改变 Binder caller，也不是通用委托票。不匹配时，AMS不会自动改用当前 Binder caller，而是原样保留 API 显式传入的 pid/uid；所以显式参数仍必须来自可信框架代码。匹配只防止随便造一个 Binder 对象认领 ThreadLocal 身份，生命周期与同步调用栈共同限制可用窗口。

ContentProvider 的这条路径随后还可能调用 `noteProxyOp()`或检查 URI grant。受控 token 解决的是“AMS 代表 App 发起同步 provider 调用时，基础 check 不要误看成 AMS 自己”；它没有把基础 permission、AppOps 与 URI 能力合成一个永不过期的结论。

异步任务也不能靠这两种 token 自动继承原 caller。Runnable 换线程后应只接收入口阶段验证过的内部数据；若执行时授权仍可能改变，重新校验或绑定一个有生命周期的 capability，而不是在异步线程重新调用 `getCallingUid()`。

### 练习 9：区分两种 token，并验证不匹配时的真实回退

沿 `openContentUri()`记录 ThreadLocal 的 set、同步传递与 finally remove，再读 `checkPermissionWithToken()`。回答：匹配时改了什么局部变量，不匹配时保留什么，并确认该窗口只有 ThreadLocal set/remove与局部赋值，没有调用 clear/restore 来改 Binder identity。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final ThreadLocal<Identity> sCallerIdentity = new ThreadLocal<Identity>();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'Binder token = new Binder();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'sCallerIdentity.set(new Identity(' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'pfd = cph.provider.openFile(null, null, uri, "r", null, token);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'sCallerIdentity.remove();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'Identity tlsIdentity = sCallerIdentity.get();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (tlsIdentity != null && tlsIdentity.token == callerToken) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'uid = tlsIdentity.uid;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'pid = tlsIdentity.pid;' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'enforceFilePermission(callingPkg, attributionTag, uri, mode, callerToken);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mTransport.noteProxyOp(callingPkg, attributionTag,' frameworks/base/core/java/android/content/ContentProvider.java
```

## 15. 安全 Binder 入口要同时证明主体、对象、用户与执行时机

一个稳健的服务入口通常按以下顺序组织：

1. 在原入站事务中立即读取可信 callingPid/callingUid；
2. 用 Calling 或显式保存的可信身份检查业务 permission；
3. 若接收 packageName，验证它属于 callingUid；shared UID 下明确允许哪一个成员语义；
4. 用正确的 UserController/PMS helper 解析并授权 target user，再按业务需要确认用户真实存在；
5. 验证组件 owner、Binder token、资源 ID 或其他对象归属；
6. 只有这些门通过后才 clear identity，且 finally restore；
7. 异步阶段只传已验证参数，并为可撤销能力选择重查或生命周期 token；
8. 执行具体操作需要时继续做 AppOps、URI grant、前后台与设备政策检查。

三个反例尤其值得在评审中搜索。

其一，入口先 clear，再 `enforceCallingOrSelfPermission()`。system_server 会以 `MY_PID`在 AMS 第一分支无条件通过非 null permission，外部 caller 的授权完全丢失。

其二，入口接收 `clientPid/clientUid`并直接调用显式 `checkPermission()`。首要漏洞是整数可伪造；clear identity 本身不会把这两个显式参数改成 system_server，但会改变同一流程里依赖 Binder caller 的 package visibility、delegate 或审计判断。伪造参数、错误清身份、缺少跨用户门是三项独立问题，不能只修其中一项。

其三，代码先按 permission 放行，再直接使用 caller 提供的 packageName、userId 或对象 token。permission 表示一类能力，不证明参数属于 caller；拥有 `MANAGE_*`一类权限也不自动允许所有特殊 user、非导出组件或任意其他用户对象。

## 16. 用四条端到端链和完成点矩阵收束结论

四条路径的最短精确地图如下：

| 路径 | 实际主链 | 这条链不负责什么 |
|---|---|---|
| Context global | cache hit 直接返回；miss 后进 AMS，依次可能在 MY_PID、process deny、core、isolated 结束；其余才到 PMS delegate/正常 UID 实现，后者有包走 state direct/fuller+instant，无包走 SystemConfig direct/fuller | 真实 owner/exported、AppOps |
| PackageManager 包名 | cache hit 直接返回；miss 后由 PMS确认 user，再经 delegate 或正常 package 实现；正常实现才做全局找包、直接 caller visibility、普通/shared state+instant/fuller | process deny、目标 user 安装保证 |
| 经 AMS wrapper 的 Activity/Service/Provider 等主路径 | 上层 user/解析 → MY_PID/process deny → core → isolated → owner → exported → null/具名 permission | 直接调用四参数 static helper 的路径不含 PID 两步；业务政策仍在外层 |
| 跨用户 | UserController：raw same-user立即返回；否则 CURRENT 转换→权限树/OR_SELF fallback→allowAll special-user gate→末尾 shell。PMS 两 helper 都先负值拒绝→可选 shell→same-user/core→所需跨用户 permission；只有 OrProfile 失败后再走同-profile fallback，且 `requireFullPermission`不关闭它 | 两套家族没有一条可互换的固定顺序，也都不单独证明正 user 存在 |

最终完成点如下：

| 已到达的点 | 已经证明 | 仍未证明 |
|---|---|---|
| 已取得 callingUid/pid | 当前线程读到一对 identity 数字 | 它是外部 caller、参数包名/对象属于它 |
| AMS global `GRANTED` | 本次基础 global 链放行 | 真实组件导出、跨用户、AppOps |
| component helper `GRANTED` | 命中 core、same-app、exported+null 或最终 UID 查询中的某个最早放行分支 | 未执行的后续门、上层用户与业务对象合法 |
| PMS package `GRANTED` | 正常实现的目标 state/instant/fuller 通过，或当前 delegate 返回通过 | 包安装给目标 user、原目标 state 必然通过、查询者有该能力 |
| PMS UID `GRANTED` | 正常实现的 UID/shared/SystemConfig 分支通过，或当前 delegate 返回通过 | 原目标 state、唯一包归因、实际操作政策通过 |
| `handleIncomingUser`返回 | 参数已按该入口归一并获跨用户资格 | 正 userId 必然存在 |
| cache 命中 | 某代际同 key 曾算出结果 | key 未遗漏 PID/callingUid 等维度 |
| 正处于 clear—restore 区间 | 内部动作以服务身份运行 | 此时能从线程重新读取原 caller；restore 后才恢复先前 identity |

最可靠的阅读方法不是问“这个 App 有没有权限”，而是依次问：身份从哪来、pid/uid 是否可信成对、当前检查属于 global 还是具体组件、目标 user 如何获得、包名还是 UID 路径、shared/instant/isolated 如何分叉、cache key 是否覆盖所有结果维度，以及基础 grant 后还有哪道操作门。

下一章进入第 269 章，继续追 `PermissionChecker`、AppOps 的 preflight/data delivery、`AttributionSource` 链，以及“permission 已 grant 但实际操作仍被拒绝”的完成点。
