# 270 Android AppOpsService：mode 存储、UID/package 优先级、watcher、历史持久化与用户限制状态机

一次 AppOp 返回 `MODE_IGNORED`，可能是保存的 mode 拒绝，也可能是 package suspend、用户限制或 UID 动态能力把它收缩；一个 mode 已经改回默认，磁盘、回调和运行中事件也未必同时收敛。若把 `AppOpsService` 看成一张 `package -> mode` 表，这些现象都会显得矛盾。

全文只讨论本地 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。这一版的状态仍集中在 `AppOpsService` 与 `HistoricalRegistry`，还没有后来拆出的独立 checking service。本文会把源码的正常协议与 r48 已存在的实现缺口同时标出来；“接口意图”不能替代“这一版实际执行的分支”。

## 1. 先分开五个状态平面与六个完成点

AppOps 至少有五个状态平面：

| 平面 | 主要对象或存储 | 回答的问题 |
|---|---|---|
| 保存政策 | `UidState.opModes`、package `Op.mode` | UID 或包对 switch op 配了什么 mode |
| 动态评价 | UID state、capability、widget、suspend、restriction | 这一刻保存政策能否变成允许 |
| 最近事件 | `AttributedOp` 的 access/reject/in-progress | 各 tag、UID state、flags 最近发生了什么 |
| 聚合历史 | `HistoricalRegistry` 的 current/pending/disk | 一段时间内发生了多少次、累计多久 |
| 观察与限制 | 四类 watcher、restriction token | 谁关心变化，谁临时施加额外门禁 |

它们对应的完成点也不同：setter 返回只表示内存路径走完；异步 mode callback 到达不表示文件已提交；`appops.xml` 提交不表示 history 同步提交；started callback 不表示进入 running；active=true 不表示每个嵌套 start 都各有一条边沿；restriction token 消失也不表示历史证据被清除。

主要源码地图如下：

| 职责 | 文件 |
|---|---|
| 当前政策、事件、watcher、restriction | `frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java` |
| 聚合历史与分层文件 | `frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java` |
| mode、UID state、历史对象和客户端包装 | `frameworks/base/core/java/android/app/AppOpsManager.java` |
| Binder 接口 | `frameworks/base/core/java/com/android/internal/app/IAppOpsService.aidl` 及四个 callback AIDL |
| shared UID permission flag 实体 | `PackageSetting.java`、`PermissionManagerService.java` |

阅读时先问“正在看哪个平面”，再问“代码已经越过哪个完成点”。同一个 `MODE_ALLOWED` 或同一次 callback，跨平面外推都会制造错误结论。

## 2. 对象树同时容纳政策、最近事件和运行态

顶层 `mUidStates` 以完整 Linux UID 为键，user 0 的 appId 10001 与 user 10 的同一 appId 是不同记录。`UidState` 不只是进程前后台状态，它同时保存：

- 已提交与待提交的 UID state、capability、widget 可见性；
- `opModes`：整个 UID 的显式 mode；
- `pkgOps`：`packageName -> Ops`；
- `foregroundOps` 与 `hasForegroundWatchers`：为动态 mode 通知维护的派生缓存。

`Ops extends SparseArray<Op>`，绑定 package、所属 `UidState`、restriction bypass 信息和已知 attribution tag。每个 `Op`含一个 package 级 `mode`，再以 `attributionTag -> AttributedOp`保存事件。`AttributedOp`内部有三组容器：

| 容器 | 键 | 内容 |
|---|---|---|
| `mAccessEvents` | `uidState + opFlags` | 每个键最后一次成功访问或完成的 start |
| `mRejectEvents` | `uidState + opFlags` | 每个键最后一次被记账的拒绝 |
| `mInProgressEvents` | client Binder | 尚未配平的 start 与嵌套计数 |

政策通常先归并到 switch op，事件却记在调用的原始 op 上。于是可以出现“一个开关控制一组细分动作，但每种动作仍分别留下 tag 和 flags 证据”。聚合次数与累计时长不在这棵 current 对象树里，而是送入 `HistoricalRegistry`；UID 的实时 state/capability、watcher 与 restriction 也不会写进 `appops.xml`。

### 练习 1：把三层对象与三组事件容器连起来

先只定位字段，不追调用。手画 `mUidStates -> UidState -> pkgOps -> Ops -> Op -> AttributedOp`，再把 UID mode、package mode、recent event 与 pending history 放到正确节点。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final SparseArray<UidState> mUidStates = new SparseArray<>();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public ArrayMap<String, Ops> pkgOps;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public SparseIntArray opModes;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final ArrayMap<String, AttributedOp> mAttributions = new ArrayMap<>(1);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private @Nullable LongSparseArray<NoteOpEvent> mAccessEvents;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private @Nullable LongSparseArray<NoteOpEvent> mRejectEvents;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private @Nullable ArrayMap<IBinder, InProgressStartOpEvent> mInProgressEvents;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private @NonNull LinkedList<HistoricalOps> mPendingWrites = new LinkedList<>();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
```

## 3. check、note 与 start 不是同一条裁决流水线

三类入口共享一些结构，却有不同的早返回、动态求值和副作用。最可靠的对照是：

| 阶段 | `checkOperation` | `noteOperation` | `startOperation` |
|---|---|---|---|
| package 解析失败 | 返回 IGNORED | 返回 IGNORED，无 noted callback | 返回 IGNORED，无 started callback |
| UID/package 校验失败 | 捕获后返回 op 默认 mode | 返回 ERRORED，无 noted callback | 返回 ERRORED，无 started callback |
| package suspend | 对指定的一组 op 返回 IGNORED | 没有这道显式检查 | 没有这道显式检查 |
| user restriction | 返回 IGNORED | callback 报 IGNORED，但不写 reject | callback 报 IGNORED，但不写 reject |
| mode 拒绝 | 只返回结果 | 写 recent/history reject 并发 noted | 写 recent/history reject 并发 started |
| mode 允许 | 只返回结果 | 写瞬时 access 并发 noted | 发 started，建立 in-progress，首个实例再发 active |

因此 check 不建事件、不增加历史、不触发 noted/started/active。note/start 也不是“所有尝试均可观察”：包解析、身份失败，以及 start 的 hotword 前置失败都在事件 watcher 之前返回。更细的一个 r48 不一致是：`getOpsLocked()`失败时 noted/started callback 被安排为 IGNORED，方法返回值却是 ERRORED。

switch code 的使用也不完全相同：

- check 在读取 UID/package policy 前直接把局部 `code`替换为 switch code，所以两层动态求值都看 switch code；
- note/start 保留原始 `code`，UID 覆盖分支用原始 code 求值，package 分支则对 switch `Op`求值；
- restriction 在三条路径中都用原始 code；suspend 也在 check 归并 switch 前看原始 code。

对 r48 当前 op 表，这种差别经常落到相同能力族，但它仍是不能抹平的实现边界。维护或移植时，不应写一份抽象伪代码替代三条真实路径。

`checkOperationRaw()`也只跳过 `evalMode()`，并不跳过 package 解析、身份校验、suspend 或 user restriction。身份不匹配还会回落 op 默认 mode，所以 raw API 不是身份认证接口。缺少显式 `Op`时直接返回默认值，也不会再经过 CAMERA/MIC 对 ALLOWED 的特殊动态收缩。

root UID 是包校验规则的特例：`resolvePackageName()`把它规范成 `"root"`，`verifyAndGetBypass()`也直接返回 unrestricted bypass，不走普通 package/UID 归属核对。因而上表描述的是普通应用身份失败路径，不能外推成所有 UID 都使用同一 package 校验。

### 练习 2：逐行证明三条路径的分叉

把下列命中按 check、note、start 三列排列。特别标出 suspend 的唯一调用点、restriction 与创建 `AttributedOp`的相对位置，以及 check 何时覆盖局部 `code`。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return checkOperationInternal(code, uid, packageName, false /*raw*/);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (isOpRestrictedDueToSuspend(code, packageName, uid)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'code = AppOpsManager.opToSwitch(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return raw ? op.mode : op.evalMode();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private int noteOperationImpl(int code, int uid, @Nullable String packageName,' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final int switchCode = AppOpsManager.opToSwitch(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public int startOperation(IBinder clientId, int code, int uid, String packageName,' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'scheduleOpStartedIfNeededLocked(code, uid, packageName, AppOpsManager.MODE_ALLOWED);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'scheduleOpNotedIfNeededLocked(code, uid, packageName, AppOpsManager.MODE_ALLOWED);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 4. UID 覆盖优先，FOREGROUND 与部分 ALLOWED 还要动态求值

只要 `UidState.opModes`含该 switch key，UID 层就遮蔽 package 层；删除 UID 覆盖后，原有 package mode 才重新显现。遮蔽不会删除 package 记录。若两层都没有显式项，则使用 `opToDefaultMode()`。

这里有三个容易混为一谈的“默认”：表中没有 key、某个 op 的 `opToDefaultMode(op)`、常量 `MODE_DEFAULT`。setter 只在目标 mode 等于该 op 的真实默认值时删除覆盖；若某 op 的真实默认不是 `MODE_DEFAULT`，显式设置 `MODE_DEFAULT`反而会留下一个优先级很高的 UID/package 记录，不能自动回落下层。

UID state 数值越小越重要。从更差状态升到更重要状态会立即提交；跨入可能前台的边界也立即提交；同一 state 但 capability 改变同样立即提交。向更差状态移动则按来源状态使用 settle time，r48 默认 TOP 5 秒、foreground-service 5 秒、background 1 秒，避免短暂切换造成能力抖动。

`MODE_FOREGROUND`的评价顺序是：widget 可见、pending-top、state 不差于 TOP可直接 ALLOWED；否则若 state 仍处在该 op 的首个不受限范围，位置、相机、录音还要各自 capability，其他 op 可 ALLOWED；再差则 IGNORED。

`MODE_ALLOWED`也不是所有 op 的恒等返回。CAMERA 与 RECORD_AUDIO 仍要求 pending-top、临时 while-in-use allowlist 或对应 capability，缺少时收缩为 IGNORED。普通 op 的 ALLOWED 才原样返回。这个动态结果既会随 UID 状态改变，也可能因 check/package 默认早返回而根本没有经过同一段评价代码。

运行中的 `AttributedOp`在 UID state 改变时会内部执行一次不触发 active 边沿的 finish/start，把累计 duration 切到旧 state，再在新 state 下继续。这意味着 historical access count 不是“业务 API 调用次数”的绝对同义词：一次长会话跨 UID state，内部重段也会再次增加 access count。

## 5. setUidMode 的状态转移、兼容 permission flag 与通知边界

`setUidMode()`先执行管理授权、校验 op、归并 switch code。它不接收 package，也不调用 `verifyIncomingUid()`；只要调用者通过 `enforceManageAppOpsModes()`，就可直接为目标 UID 创建 `UidState`。同进程调用直接通过，当前 user 的 profile owner 可改本 user 的目标 UID，其余调用者需要 `MANAGE_APP_OPS_MODES`。

非 PermissionPolicy 内部回调发起时，方法在进入 AppOps 主锁前先执行 `updatePermissionRevokedCompat()`。这带来两个结论：

1. 后面即使发现相同 mode 并提前返回，兼容 permission flag 联动也已经执行；
2. permission flag 更新与 AppOps mode 写入不是同一把锁保护的事务。

兼容 flag 步骤完成后，UID mode 的 AppOps 侧核心分支如下：

| 旧状态 | 目标状态 | r48 行为 |
|---|---|---|
| 无 `UidState` | op 真实默认 | 直接返回，不通知、不安排写 |
| 无 `UidState` | 非默认 | 创建 root 与 `opModes`，普通延迟写 |
| 有 root、`opModes == null` | 真实默认 | 不写盘，但仍重算并通知 |
| `opModes`已有同 key 同值 | 任意 | 直接返回 |
| 数组非空但缺该 key | 任意 | `previousMode = get(code)`得到整数 0，即 MODE_ALLOWED |
| 有显式 key | 恢复真实默认 | 删除 key；数组空后置 null，普通延迟写 |

最后一种“缺 key 时 previous=0”不是此前有效策略的可靠描述；如果 op 的默认值不是 ALLOWED，传给 `StorageManagerInternal`的 previous 值尤其会失真。

兼容 permission 联动遍历该 switch 控制的权限，只处理当前已授予的 runtime permission。存在 background permission 时，FOREGROUND 会把后台权限标为 compat-revoked、保留前台权限；现代 targetSdk 只会触发“不应以 mode 代替真正 revoke”的警告，代码仍继续更新 flag。

源码确实把 `getPackagesForUid(uid)[0]`作为 PackageManager API 的 package 参数，但不能据此得出“shared UID 只有第一个包收到 flag”。`PackageSetting.getPermissionsState()`在 shared UID 下返回共同的 `SharedUserSetting` permission state，更新的是共享实体；第一个包主要承担调用参数、请求权限查找与通知归因角色。

内存处理后，服务为 UID 的全部 package 合并 op/package watcher 并异步通知，再同步调用 `StorageManagerInternal.onAppOpsChanged(code, uid, null, mode, previousMode)`。异步观察者和同步本地消费者不是一个完成点。

### 练习 3：对照两个 setter 的写盘与 previousMode

为 UID setter 和 package setter 各画一个“无 root、空数组、缺 key、同值、改值、恢复默认”表。再沿 shared UID permission state 确认“第一包参数”不等于“第一包私有 flag”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void setUidMode(int code, int uid, int mode,' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'previousMode = uidState.opModes.get(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private void updatePermissionRevokedCompat(int uid, int switchCode, int mode) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'String packageName = packageNames[0];' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return (sharedUser != null)' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'private void setMode(int code, int uid, @NonNull String packageName, int mode,' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'scheduleFastWriteLocked();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'notifyOpChangedSync(code, uid, packageName, mode, previousMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 6. setMode 的 no-op、fast write 与 running-only prune 陷阱

`setMode()`也先授权、校验并归并 switch code。普通 UID 必须通过 `verifyAndGetBypass(uid, packageName, null)`确认 UID/package 关系，失败只记录并返回；root UID 为兼容旧行为会跳过 packageName 校验，因而不能把这一步当作 root 输入的真实性证明。进入锁后，它先取旧 `UidState`引用，再用 `getOpLocked(..., edit=true)`找或创建 package `Op`。

“edit=true”本身就会调用 `scheduleWriteLocked()`，无论 Op 是新建还是已存在；只有当前尚无已安排写任务时，它才新发一个 30 分钟 runner。因此 no-op 并非无副作用：

- 对一个从未存在的 package/op 设置其真实默认值，会创建空 `Ops/Op`并请求普通写；因为 mode 没变化，代码不会进入 prune；
- 对已有相同非默认 mode 再设置一次，也会请求普通写；
- mode 相等分支本身不安排 mode-change callback，却仍无条件同步通知 StorageManager；局部 `previousMode`没有在 no-op 中更新，仍是 `MODE_DEFAULT`。不过前置 `getUidStateLocked(uid, false)`可能顺带提交已到期的 pending UID state并安排 foreground callback，不能据“setter 参数同值”断言整个调用绝无异步通知。

真正发生 mode 改变时才会重算 foreground 缓存、收集 op 与 package watcher、安排 10 秒 fast write。这里还有一个顺序边界：局部 `uidState`在 `getOpLocked(edit=true)`之前取得；若 root 原先不存在，后者新建 root，但局部变量仍为 null，本次首次写入即便是 FOREGROUND也不会调用 `evalForegroundOps()`。

恢复真实默认时会调用 `pruneOpLocked()`，但“有价值事件”的判断只看 access/reject：`AttributedOp.hasAnyTime()`完全不看 `mInProgressEvents`。所以一个刚 start、尚无已完成 access/reject 的 running-only attribution 可被删除，父 `Op`甚至可从 map 裁掉。之后按 UID/package/tag 执行显式 finish 可能找到新建的另一个 Op，却找不到旧 attribution；running 查询和 active 边沿也可能失配。旧对象可因 client death recipient 暂时存活，但这不是可依赖的完成协议。

UID mode 只遮蔽 package mode，不会阻止上述 package 记录被创建或裁剪。排查 setter 时应分别记录“有效结果是否改变”“存储对象是否改变”“是否安排普通/快速写”“哪类 callback 被触发”。

## 7. resetAllModes 的名字比实际范围更宽，参数约束却比想象更窄

正常意图是把允许 reset 的 UID/package mode 恢复到各自真实默认值。它不清 recent event、不清 historical count、不撤销 restriction token，也不解除 package suspend。package `Op`恢复默认后只移除没有 access/reject 的 attribution；这也继承了 running-only prune 问题。

package 分支会按 `reqUserId`和 `reqPackageName`过滤，并对 `opAllowsReset()`为真、当前非默认的记录改值。若 `DevicePolicyManagerInternal.supportsResetOp(op)`为真，代码直接同步调用 `dpmi.resetOp(op, reqPackageName, reqUserId)`并跳过本地处理；“defer”只是方法名，不代表异步排队，而且判断发生在本地 mode 是否非默认之前。

UID 分支在 r48 有四个必须单独记录的缺口：

| 缺口 | 直接后果 |
|---|---|
| 只判断 `uid == reqUid || reqUid == -1`，不判断 `reqUserId` | `resetAllModes(user10, null)`可清所有用户的可重置 UID mode |
| 指定不存在的 package 时 UID 查询仍保留 `reqUid == -1` | 看似单包 reset 可退化成全 UID-mode 扫描 |
| 删除 UID key 不把 `changed`或`uidChanged`置 true | 仅 UID 变化时不安排 fast write，也不重算 foreground 缓存 |
| `pkgOps == null`后直接 continue | 删除最后一个 UID mode 后，空 `UidState`可留在内存 |

若同一次 reset 还改了任一 package mode，`changed=true`会让完整 current 快照最终包含 UID 删除；但这只是搭便车，不能补成 UID 分支自己的持久化保证。

通知也有边界。锁内收集 ChangeRec，锁外先把 Handler 消息入队，再同步通知 StorageManager。`addChange()`判重只比较 op 与 package，不比较 UID；`USER_ALL`场景中不同用户的同名 package 可被错误合并，后一个 UID 的 callback/同步通知记录可能消失。

### 练习 4：用三个输入证明 reset 的范围漏洞

分别推演 `(user10, null)`、`(user10, 存在包)`、`(user10, 不存在包)`。在纸上给 UID-mode 循环和 package-mode 循环各标一次过滤条件，不要用方法入口的参数名替代循环内条件。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void resetAllModes(int reqUserId, String reqPackageName) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'int reqUid = -1;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (opModes != null && (uidState.uid == reqUid || reqUid == -1)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (reqUserId != UserHandle.USER_ALL' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (AppOpsManager.opAllowsReset(code)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (shouldDeferResetOpToDpm(curOp.op)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (changed) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (report.op == op && report.pkg.equals(packageName)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 8. mode watcher 的两个索引是并集，不是 `(op, package)`交集

mode watcher 有三张表：binder 到唯一 `ModeCallback`、switch op 到 callback 集合、package 到 callback 集合。注册同时给出 op 与 package 时，同一对象被放进两种索引；通知端从命中的集合取并集。因此它表达的是“这个 op 在任意包变化，或这个包的任意受索引变化”，不是只观察这个精确二元组。

组合的实际可达性如下：

| 注册参数 | 加入的索引 | 可收到什么 |
|---|---|---|
| 具体 op，package null | switch-op 索引 | 该 switch 的变化，跨 package |
| `OP_NONE`，具体 package | package 索引 | 该 package 经 setter/reset 进入此索引的变化 |
| 具体 op，具体 package | 两个索引 | 两类事件的并集，通知集合再去重 |
| `OP_NONE`，package null | 无分发索引 | binder 表里有对象，但没有通知来源 |

没有 `CALL_BACK_ON_SWITCHED_OP`时，具体 op watcher 记住原始 op；`OP_NONE`记为 `ALL_OPS`，通知时把变化的 switch 展开成受控成员。带该 flag 时记住 switch op；`OP_NONE`则在 callback 时只回传触发的 code。这里的“all”只有 package 索引提供触发源，不能把 `OP_NONE + null`理解成全局订阅。

r48 服务端没有执行 `WATCH_APPOPS`权限检查，`watchedUid`固定为 -1；源码旁边也承认需要特权保护。客户端 API 的注解或文档不能补上服务端缺失的强制检查。

同一 callback binder 重复注册时，服务复用第一次创建的 `ModeCallback`。后续调用只增加 op/package 索引，不更新第一次的 flags、`mWatchedOpCode`、calling uid/pid。因此“先注册 A、再用同一 listener 注册 B”可能仍按 A 的 code 与 flags 回报。`WATCH_FOREGROUND_CHANGES`也受首次 flags 黏连影响。

索引并集还会影响 code 的解释：一个同时注册具体 op 与 package 的 callback，可因该 package 的另一个 op 变化而从 package 索引被唤醒，但 `notifyOpChanged()`仍可能按首次保存的具体 `mWatchedOpCode`回报。callback 参数因此不是“这个二元组刚发生精确变更”的事务证据。

并非所有有效资格变化都走同一索引：UID state/capability 的 foreground 变化依赖 op 索引；restriction 变化只取 `mOpModeWatchers.get(code)`并以 `UID_ANY/null package`通知；package-only watcher收不到它们。suspend receiver同样只取 op 索引，但随后在 receiver 线程直接调用通知方法，不经 Handler。

### 练习 5：画出 mode watcher 的索引并集

用同一个 callback 依次注册 `(CAMERA, null)`与 `(OP_NONE, pkg)`，追踪 binder 表只创建一次、两个索引怎样增加、最终 `mWatchedOpCode`为何仍来自首次注册。再验证 restriction 通知没有查 package 索引。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final ArrayMap<IBinder, ModeCallback> mModeWatchers = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final SparseArray<ArraySet<ModeCallback>> mOpModeWatchers = new SparseArray<>();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final ArrayMap<String, ArraySet<ModeCallback>> mPackageModeWatchers = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'int watchedUid = -1;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if ((flags & CALL_BACK_ON_SWITCHED_OP) == 0) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'ModeCallback cb = mModeWatchers.get(callback.asBinder());' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (callback.mWatchedOpCode == ALL_OPS) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'ArraySet<ModeCallback> callbacks = mOpModeWatchers.get(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'evalAllForegroundOpsLocked();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 9. active、started、noted 分别观察边沿、start 结果与 note 结果

另外三类 watcher 都按原始 op code 建索引，不经过 switch 归并。无 `WATCH_APPOPS`时，服务把 `mWatchingUid`固定为 callingUid；有权限时使用负值表示不限 UID，分发时再次过滤。

三者的语义是：

| watcher | 触发单位 | 被拒是否可见 | 关键边界 |
|---|---|---|---|
| active | 父 `Op`从无 running 到有、从有到无 | 否 | 多 tag、多 client 的总体 0↔1 边沿 |
| started | 到达 `scheduleOpStartedIfNeededLocked()`的一次 start 裁决 | 是 | 更早返回不可见；callback result 有一个 getOps 异常分支不等于返回值 |
| noted | 到达 `scheduleOpNotedIfNeededLocked()`的一次 note 裁决 | 是 | restriction 拒绝可见但不一定有 reject 账 |

同一 client Binder 对同一 attribution 重复 start，只增加 `numUnfinishedStarts`；相同次数的 finish 才归零。父 Op 内第一个成功 start 发 active=true，最后一个 running event 结束才发 false。client death 会把该 token 的未完成计数压成 1，再走统一 finish，保证一次清理结束全部嵌套层。

注册 API 还有两个 r48 边界。`startWatchingActive()`允许 `ops == null`通过前置范围检查，却随后执行 `for (int op : ops)`，原始 Binder 调用会 NPE；started/noted 则明确拒绝 null 或空数组。公共 Java wrapper通常会挡掉 active 的 null，但服务端缺口仍然存在。

其次，active/started/noted 对同一 binder 的每次注册都会新建 wrapper 并 `linkToDeath`，再按 op 覆盖 SparseArray。重复覆盖同一个 op 时不会先 destroy 旧 wrapper，显式 stop 只能遍历当前 map 值；旧 death recipient 可一直保留到 binder 死亡。一次注册把同一 wrapper 放入多个 op 时，stop 又会对同一对象重复 unlink。它们没有 mode watcher 的“首个 wrapper 完全复用”，却有另一种重复注册生命周期瑕疵。

### 练习 6：证明 callback 可见性不等于记账可见性

分别从 package 解析失败、restriction 拒绝、UID mode 拒绝、允许四个点进入 note/start，记录返回值、started/noted、recent reject、history reject、active 五列。最后验证 active 的 null 数组为何在循环处失败。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void startWatchingActive(int[] ops, IAppOpsActiveCallback callback) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'for (int op : ops) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'Preconditions.checkArgument(!ArrayUtils.isEmpty(ops), "Ops cannot be null or empty");' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public void startWatchingNoted(@NonNull int[] ops, @NonNull IAppOpsNotedCallback callback) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (triggerCallbackIfNeeded && !parent.isRunning()) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'event.numUnfinishedStarts++;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'deadEvent.numUnfinishedStarts = 1;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'scheduleOpStartedIfNeededLocked(code, uid, packageName, uidMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'scheduleOpNotedIfNeededLocked(code, uid, packageName, uidMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 10. 回调、recent event 与 history counter 是三种不同证据

一次允许 note 会覆盖对应 `uidState+flags`的最后 access，并增加 historical access count；mode 拒绝会覆盖最后 reject 并增加 reject count。restriction 拒绝虽然能发 noted/started，却发生在创建 `AttributedOp`之前，所以不写 recent/history reject。check 更是三者都不写。

成功 start 先安排 started(ALLOWED)，随后创建或递增 in-progress event，并增加 access count；第一次让父 Op running 时再安排 active=true。finish 到零后以 `elapsedRealtime`计算 duration，把开始 wall time 和持续时长写入 recent access，再增加 historical duration。started 与 active 的消息通常按这一调用顺序入 Handler 队列，但 callback 是异步观察，不是 setter/start 的事务提交凭证。

大多数 mode、active、started、noted 通知在锁内复制集合或安排消息，真正 callback 时 `clearCallingIdentity()`，避免 system_server 内消费者继承触发者身份。存在两个重要例外：package suspend receiver复制集合后在 receiver 路径直接执行 mode callback；async-noted 使用另一套 `RemoteCallbackList`直接广播。两者内部仍处理 Binder identity，但“所有外部通知都经同一个 Handler”不成立。

StorageManager 的 mode 通知又是第三种路径：它是锁外同步的 LocalServices 调用，setter no-op 也可能触发，并且不经过 mode watcher 的去重与 Handler。排查“回调先后”时至少要分清远端 watcher、本地同步 consumer、async-noted 应用侧消息。

## 11. appops.xml 的普通写、快速写与 AtomicFile 只保护一份 current 文件

ActivityManagerService把 `/data/system/appops.xml`传给 `AppOpsService`构造器，服务以 `AtomicFile`管理。普通写延迟默认 30 分钟；快速写固定 10 秒。`scheduleWriteLocked()`已有任务时不重复发；`scheduleFastWriteLocked()`首次提速时设置普通与快速标志、移除旧 runner，再发 10 秒任务。

runner 在 AppOps 主锁内先把两个 scheduled 标志清零，再把 `writeState()`交给 `AsyncTask.THREAD_POOL_EXECUTOR`。因此快照写入期间的新变化可以重新安排下一轮，不必等当前 I/O 完成。shutdown 只在当时 `mWriteScheduled`为真时同步写 current：尚未触发的 fast write也设置该标志，因而会被补写；若 runner 已清标志并把 I/O交给 AsyncTask，shutdown既不等待这次在途写，也不会仅因它在途而另补一次。原 Handler callback也没有在 shutdown 中显式移除，正常依赖进程退出结束生命周期。

note/start/finish 会调用 `getOpLocked(..., edit=true)`，而这个 helper 即使只取到已有 Op 也安排普通写。因此 recent event最终有机会落到 current XML；“30 分钟后才允许”是错误的，内存裁决立即生效，延迟的只是提交。

AtomicFile 的成功路径是 `startWrite -> finishWrite`，`IOException`走 `failWrite`恢复文件级备份。它能防止这一份 XML 的典型半写，却不能保证：

- 内存 policy 与 watcher callback 同时提交；
- package/event 快照与 UID-mode 快照来自同一时刻；
- `appops.xml`与 history 目录跨存储原子提交；
- 运行中的 Binder 生命周期能在重启后恢复。

### 练习 7：区分调度标志、快照与文件提交

沿 `getOpLocked(edit)`、普通调度、快速调度、runner、`writeState()`顺序画时间线。把“内存已变”“runner 已入队”“快照已取”“AtomicFile 已提交”标成四个不同完成点。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static final long WRITE_DELAY = DEBUG ? 1000 : 30*60*1000;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mFile = new AtomicFile(storagePath, "appops");' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mWriteScheduled = false;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHandler.postDelayed(mWriteRunner, WRITE_DELAY);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHandler.postDelayed(mWriteRunner, 10*1000);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'stream = mFile.startWrite();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'List<AppOpsManager.PackageOps> allOps = getPackagesForOps(null);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mFile.finishWrite(stream);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mFile.failWrite(stream);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 12. current XML 能恢复政策与最后事件，不能恢复 active 会话

writer 先通过 `getPackagesForOps(null)`在主锁内取得 package/op/event 条目，随后另一次进入主锁克隆 UID `opModes`。序列化在锁外进行，所以两份快照各自一致，却不是单一全局时刻。

根节点是 `<app-ops v="1">`。顶层 `<uid n>`里的 `<op n m>`保存 UID 非默认覆盖；package 路径是 `<pkg n> -> <uid n> -> <op n [m]> -> <st>`。package mode 等于真实默认时省略 `m`，但只要 Op 对象仍在，空 `<op>`或带事件的 `<op>`都可写出。

`<st>`字段可按下表读：

| 属性 | 含义 |
|---|---|
| `id` | attribution tag，可省略 |
| `n` | UID state 与 op flags 的组合 key |
| `t` / `r` | 最后 access / reject 的 wall time |
| `d` | 最后完成持续时间，仅大于 0 才写 |
| `pp` / `pc` / `pu` | 最后 access 的 proxy package/tag/uid |

reject 从一开始就没有 proxy info。若 `t/r/d/proxy`均无可写值，整个 `st`跳过。零 duration 不写 `d`，所以缺少 `d`不能证明从未 start。

生成快照时，running event会被复制成“开始时间 + 到当前的 duration” access event；`isRunning`虽存在于内存 `AttributedOpEntry`，writer不序列化它。重启读取后只调用 `accessed(time,duration,...)`，不会重建 client token 或 active 状态。因此运行中快照在 XML 里看起来像一条截至快照时刻的已完成 recent access，这个区间也不会自动延续。

`readState()`先锁 `mFile`再锁服务。成功打开文件后立即清空 `mUidStates`；任何受捕获的解析错误都会再清空整棵树，不保留前半文件。一个容易漏掉的分支是：`openRead()`报文件不存在时在 clear 之前直接返回。首次构造时内存本来为空；但 `reloadNonHistoricalState()`等已有内存场景下，缺文件不会把旧内存清空。

无 version 的旧文件会把 RUN_IN_BACKGROUND 的非默认 UID/package mode复制到 RUN_ANY_IN_BACKGROUND，并安排 fast write；package 事件不会跟着复制。顶层 UID XML又通过 `setUidMode()`读取，可能触发 compat permission flag、foreground 评价、写调度和内部通知，所以反序列化并非纯粹的无副作用 put。

## 13. HistoricalRegistry 的 mode 不是 add/query 的统一总闸

默认参数是 ACTIVE、基础区间 15 分钟、压缩倍率 10。构造阶段 SettingsProvider 尚未就绪，`systemReady()`才注册 `APPOP_HISTORY_PARAMETERS`观察者、应用配置并初始化 `Persistence`。普通自动上报在此前会记录“persistence 未初始化”并丢弃。

三种 mode 的最小可靠语义是：

| mode | 自动 access/reject/duration | 进入该 mode 时的动作 | 显式 add/query |
|---|---|---|---|
| ACTIVE | 记录 | 无额外清理 | persistence 可用时可用 |
| PASSIVE | 不记录 | 无额外清理 | 可显式注入，主要供测试 |
| DISABLED | 不记录 | 仅在 mode 真正变化时清一次 current/pending/disk | 没有被 mode 本身统一阻断 |

这里的表描述的是请求已经进入 `HistoricalRegistry`之后的 mode 行为，不表示任意 caller 都能抵达它：服务层普通 history query先把 caller限定为 system、受 instrumentation 的 UID或 permission controller，再要求 `GET_APP_OPS_STATS`；`addHistoricalOps()`要求 `MANAGE_APPOPS`。通过这些入口检查后，`isApiEnabled()`只看调用身份/DeviceConfig而不看 `mMode`，registry 内的 add/query也只检查 persistence 是否已初始化。因此从 ACTIVE/PASSIVE 切到 DISABLED 后，获准的显式 add 仍能重新写入，获准的 query 仍能读出；再次设置相同 DISABLED 且 interval/multiplier 也不变时，不会再次触发清理。不能把它描述成封闭且自洽的三态机。

反向还有初始化缺口：设备若以 DISABLED 且默认 interval/multiplier 启动，`systemReady()`跳过创建 persistence。之后只把 mode 改回 ACTIVE/PASSIVE、两个数值不变，`setHistoryParameters()`既不重采样也不补建 persistence；自动采集继续因未初始化而丢弃。若此前是已初始化状态再切 DISABLED，clear 不会把 persistence 置 null，行为又不同。

设置解析也要按分支描述。缺少三项之一或数值解析失败，会记录警告并保留当前参数，日志文字声称 reset 但没有真的调用 reset。未知 mode 字符串却被 `parseHistoricalMode()`解释为 DISABLED；只要两个数字合法，整套设置会真实应用并可能清历史。interval 与 multiplier 在这条内部路径也没有正值校验。

## 14. current、pending、disk 的合并存在丢批次竞态与锁序反转

聚合历史内部以“距现在多久”的相对时间组织 current batch。达到区间边界并不会由独立定时器准点触发；下一次 increment、query 等调用进入 `getUpdatedPendingHistoricalOpsMLocked()`时才 rollover，把完整 batch放入 `mPendingWrites`并给 BackgroundThread 发消息。空闲设备可以越过边界而没有准点磁盘写。

磁盘位于 `/data/system/appops/history`，用 `AtomicDirectory`切换整个目录版本。第 0 层保留最近且细的窗口，更老层级按倍率扩大间隔并合并统计；持久化的是 UID、package、tag、op、`uidState+flags`下的 access count、reject count、duration。它适合趋势统计，不是逐事件日志。读取或版本异常会清掉不可用 history，权限裁决仍可继续，但审计证据会出现空洞。

正常跨旧区间 query 会合并 current，并把 pending 强制持久化后读取 disk；raw-disk API则只读磁盘。可 r48 的普通 query 有一个真实丢失窗口：它在主锁内无条件复制并清空 `mPendingWrites`，只有当查询区间需要 disk 时才调用 `persistPendingHistory(localCopy)`。若一个只覆盖 current 的查询抢在后台 writer 前执行，`collectOpsFromDisk=false`，局部副本既不合并也不持久化；后台消息随后看到的全局队列已经为空。

锁方面，类注释规定涉及两边时必须 `mOnDiskLock -> mInMemoryLock`，而 `mInMemoryLock`就是 `AppOpsService.this`。query遵守 disk→memory；但 `AppOpsService.packageRemoved()`先持有服务锁，再调用 `HistoricalRegistry.clearHistory(uid, package)`去拿 disk 锁，形成 memory→disk。并发时可出现：删除线程持 memory 等 disk，查询线程持 disk 等 memory。这不是抽象风险，而是 r48 可见的 ABBA 环。

锁持有范围还越过结果回调：普通 query在持有 disk 锁时执行 `RemoteCallback.sendResult()`，raw-disk query更在 disk 与 memory 两把锁都未释放时发送。远端回调的耗时或重入会延长锁占用，不能把“已经构造 Bundle”当作历史锁已经释放。

调整 interval/multiplier 的“重采样”也只覆盖先读出的磁盘历史。`offsetHistory()`先 `readHistoryDLocked()`，随后 `clearHistory()`会清 current、pending 与 disk，再把旧磁盘副本按新参数写回；尚未落盘的近期统计被删除，不是一起重采样。

wall clock用于窗口与重启偏移，单次 start duration仍由 `elapsedRealtime`产生。自动发现 wall clock 回拨时，`getUpdatedPendingHistoricalOpsMLocked()`只记录正的 `mPendingHistoryOffsetMillis`，下一次持久化再据此重采样磁盘历史；它不会直接调用 `pruneFutureOps()`。裁去落到未来的历史只发生在显式 `offsetHistory(offsetMillis)`收到负 offset 的路径。两者都不能恢复真实绝对时间，审计工具仍应把系统改时与重启证据一起记录。

### 练习 8：构造 pending 丢失与 ABBA 两条时序

第一条令 rollover 产生 pending，再让 current-only query先于后台消息；第二条让 package removal持服务锁、query持 disk 锁。只按 acquire/clear/persist 的源码顺序画箭头，不假定 AtomicDirectory能跨锁修复内存竞态。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private final Object mOnDiskLock = new Object();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'mPendingWrites = new LinkedList<>();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'mInMemoryLock = lock;' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'mHistoricalRegistry = new HistoricalRegistry(this);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'pendingWrites = new ArrayList<>(mPendingWrites);' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'mPendingWrites.clear();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'collectOpsFromDisk = inMemoryAdjEndTimeMillis > currentOps.getEndTimeMillis();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'if (collectOpsFromDisk) {' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'mPendingWrites.offerFirst(ops);' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'final File newBaseDir = sHistoricalAppOpsDir.startWrite();' frameworks/base/services/core/java/com/android/server/appop/HistoricalRegistry.java
grep -n -F 'public void packageRemoved(int uid, String packageName) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHistoricalRegistry.clearHistory(uid, packageName);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 15. restriction 由 Binder token 持有，removeUser 只做即时内存删除

`mOpUserRestrictions`以客户端 Binder token映射 `ClientRestrictionState`。每个 state为各 user保存一组 op boolean和一组 excluded package；任意 token命中 user/op且目标包不在该 token例外中，就构成 restriction。另一个 token 的例外不会成为全局白名单。

`setUserRestriction()`对外要求 `MANAGE_APP_OPS_RESTRICTIONS`；跨 user 接受 `INTERACT_ACROSS_USERS_FULL`或 `INTERACT_ACROSS_USERS`。批量 `setUserRestrictions(Bundle,...)`只允许 system UID。`USER_ALL`不是持久通配符，而是在调用当刻通过 `UserManager.getUsers(false)`展开已创建用户快照；该参数不会排除 dying/removing 用户，同时默认排除 partial 与 pre-created 用户，未来新增 user不会自动继承。

少数 op可由 `opAllowSystemBypassRestriction()`声明 bypass 类别，但还要目标 package 的 `RestrictionBypass`具备对应 privileged 或录音特例属性；“system UID 总能绕过”不成立。restriction 只在内存，不写 current XML或 history，依赖上层持有 token并在需要时重新下发。

状态变化只通知 op-indexed mode watcher，package 参数为 null、UID 为 `UID_ANY`。取消某 user最后一个 true 时会先删 restriction 数组并把局部变量置 null，后面的 exclusion 清理块因此跳过；若同一 token还有其他 user，已无意义的 exclusion entry会残留，但不影响 `hasRestriction()`结果。

token死亡会从全局 map移除整份 state，并对死亡时仍为 true 的每个 user/op安排通知；同一 op在多个 user为 true 时可重复发 `UID_ANY`。显式取消最后限制则由 setter发现 default、移除 state并 unlink death。

`removeUser()`只允许 system UID，但这一入口的保证很窄：它从每个 token删除该 user数组，再从内存 `mUidStates`删相应 UID。它没有安排 current 写盘、没有清 HistoricalRegistry、没有 finish running event、没有通知 watcher，也没有把因此变成 default 的 restriction state从 token map移除。后续其他系统流程或无关写盘可能收敛部分状态，不能把那些外部效果算作此方法自己的完成点，更不能由这段代码保证 userId未来复用绝无旧 XML/history 影响。

### 练习 9：把 token 生命周期与 user 生命周期分开

分别模拟“同一 token 两个 user”“两个 token 同一 user”“USER_ALL 后新增 user”“removeUser 后立即重启”。标出 restriction 命中、例外、watcher、current 文件和 history各自是否变化。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private final ArrayMap<IBinder, ClientRestrictionState> mOpUserRestrictions = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public void setUserRestriction(int code, boolean restricted, IBinder token, int userHandle,' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (userId == UserHandle.USER_ALL) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'List<UserInfo> liveUsers = UserManager.get(mContext).getUsers(false);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return !ArrayUtils.contains(perUserExclusions, packageName);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (restrictionState.isDefault()) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'public void removeUser(int userHandle) throws RemoteException {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'opRestrictions.removeUser(userHandle);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'removeUidsForUserLocked(userHandle);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'private void removeUidsForUserLocked(int userHandle) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 16. 用两张边界表收束一次 AppOp 的诊断

先按入口选择真实裁决链：

| 问题 | 首先查看 | 不应直接推断 |
|---|---|---|
| check 为何拒绝 | package 身份、原始 op suspend/restriction、switch UID/package policy、动态评价 | 一定已有 reject 或 watcher 证据 |
| note 为何拒绝 | package/身份早退、restriction、UID 原始-code评价、package switch评价 | 一定检查过 suspend；callback 一定等于 recent/history |
| start 为何没 active | 更早返回、started result、client token、父 Op是否已 running | 每次 start 都有 active=true |
| setter 后为何仍拒绝 | UID 覆盖、真实默认、FOREGROUND/capability、restriction/suspend | package mode 单独决定有效结果 |

再按证据选择账本：

| 想回答的问题 | 权威位置 | 主要损失边界 |
|---|---|---|
| 当前保存政策 | 内存 `opModes` / `Op.mode` | XML 有延迟，两层快照非同一时刻 |
| 最近一次可记事件 | `AttributedOp` / `appops.xml` | restriction 拒绝不记；每个 key只留最后一次 |
| 当前是否 active | `mInProgressEvents` | 不持久化；running-only prune 可断开查找 |
| 区间次数与时长 | HistoricalRegistry | 压缩、损坏清理、pending query竞态 |
| 临时用户门禁 | restriction token | 内存态；death/removeUser通知与清理并不对称 |

最后按完成点审查状态变化：内存 mode已改，不等于异步 watcher已到；watcher已到，不等于 Storage consumer看到相同 previous 值；current 文件已提交，不等于 history已提交；AtomicFile/AtomicDirectory各自成功，不等于两者共同事务；removeUser内存已删，也不等于旧文件和历史已清。

本章最值得保留的 r48 结论不是一条漂亮的“总状态机”，而是几条明确的不对称：check独有 suspend；note/start的 restriction callback与 reject账分离；UID/package动态求值使用 code 的方式不同；setter no-op仍可能创建、写盘或同步通知；reset 的 UID 范围漏掉 user；mode watcher是索引并集且首注册黏连；current-only history query可丢 pending；历史锁序存在反转；removeUser只完成即时内存删除。

下一章进入 `PermissionPolicyService`，继续追 runtime permission、AppOps mode、角色与 one-time permission怎样在每用户启动和变更回调中尝试收敛，并检验这种收敛在哪些完成点仍可能出现短暂或永久偏差。
