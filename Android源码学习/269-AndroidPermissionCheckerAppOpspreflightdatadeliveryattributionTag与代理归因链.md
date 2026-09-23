# 269 Android PermissionChecker、AppOps preflight/data delivery、attributionTag 与代理归因链

基础 permission 已经返回 `GRANTED`，为什么位置、相机或麦克风仍可能不能交付？反过来，为什么监听器注册阶段可以接受 `MODE_FOREGROUND`，真正发送数据时却必须再次裁决？答案不在某一个布尔值里，而在 permission、AppOps mode、当前 UID 状态与访问记账这几份不同的账之间。

全文基于本地 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。这一版的 `android.content.PermissionChecker` 整类是 `@hide`，面向平台与系统代码；源码树里还没有后来版本的 `android.content.AttributionSource` 多跳对象，也没有链式的 data-delivery start/finish API。下文只解释 r48 已存在的 `attributionTag`、一次 `noteProxyOp` 和单跳 proxy/proxied 模型。

## 1. 先把四份账和五个完成点分开

PermissionChecker 的名字容易让人以为它只是在“多查一次权限”。真实链路至少涉及四份状态：

| 状态 | 代表字段或对象 | 它回答什么 |
|---|---|---|
| 基础授权 | runtime grant、install grant、`Context.checkPermission()` | 这个 pid/uid 是否持有 permission |
| 操作策略 | UID/package 的 AppOp mode、switch op | 这项操作的保存策略是什么 |
| 动态条件 | UID state、capability、user restriction | 此刻能否把保存策略评价为允许 |
| 审计记录 | `AttributedOp` access/reject、proxy info、active event | 访问是否被记录、记给谁、是否仍在持续 |

它们对应的完成点也不同：

1. 基础 permission 通过，只证明授权位这一层通过；
2. preflight 返回 `GRANTED`，只证明目标在注册或调度时“可能允许”，没有记录数据已交付；
3. delivery 的 AppOps 分支返回允许，才证明这次 note 的即时裁决通过；
4. note 成功写入的是瞬时访问，不能证明资源仍处于 active；
5. `startOp`成功且尚未 `finishOp`，才建立持续活动区间，但 r48 这套 start/finish 没有 proxy 版本。

最重要的阅读纪律是：每遇到一个 `GRANTED`，都问“它是哪一层的完成点”；每遇到一条 AppOps 记录，也问“是 proxy 侧、proxied 侧，还是 self active 侧”。

主要源码地图如下：

| 层次 | 文件 |
|---|---|
| 三值组合与入口包装 | `frameworks/base/core/java/android/content/PermissionChecker.java` |
| 客户端 AppOps API | `frameworks/base/core/java/android/app/AppOpsManager.java` |
| Binder 协议 | `frameworks/base/core/java/com/android/internal/app/IAppOpsService.aidl` |
| mode 评价与事件账 | `frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java` |
| tag 解析与组合校验 | `ParsedAttributionUtils.java`、`ParsedAttribution.java`、`ParsingPackageUtils.java` |
| 真实调用范例 | `ContentProvider.java`、`RecognitionService.java` |

## 2. preflight 与 data delivery 的差别是时机和副作用，不是强弱命名

PermissionChecker 提供两族入口：

| 入口族 | 适用时机 | AppOps 动作 | 对 `MODE_FOREGROUND` 的典型处理 |
|---|---|---|---|
| `...ForPreflight` | 注册 listener、安排工作、尚未发送受保护数据 | `unsafeCheckOpRawNoThrow()` | 保留 raw 值，允许“将来在合适状态下可用” |
| `...ForDataDelivery` | 紧邻一次真实数据交付 | `noteProxyOpNoThrow()` | 按当前状态评价，并产生该路径实际能到达的记录副作用 |

“delivery 更强”不是完整描述。preflight 的 raw check 在 r48 反而有一项 note 路径没有的 package-suspend 检查；delivery 的关键价值是使用当前动态状态、验证代理调用并记账。两者是不同协议，不能只按强弱排序，更不能用一次 preflight 结果永久授权后续回调。

三值常量不是三个 AppOps mode：

| PermissionChecker 结果 | 数值关系 | 最小可靠语义 |
|---|---|---|
| `PERMISSION_GRANTED` | 等于 PackageManager 的 grant | 当前分支允许继续 |
| `PERMISSION_HARD_DENIED` | 等于 PackageManager 的 deny | 当前分支按“硬拒绝”归类 |
| `PERMISSION_SOFT_DENIED` | PackageManager deny 再减一 | runtime 基础位已过，但 AppOps 阶段没有允许 |

`SOFT_DENIED`不保证失败是临时的、可恢复的或非安全问题。`MODE_IGNORED`、`MODE_ERRORED`、`MODE_DEFAULT`在 runtime 分支都会收敛到 soft；调用方仍不得交付完整受保护数据，只能按自身 API 合同选择空结果、延后、停止会话或错误回调。

### 练习 1：确认 API 族与三值并非同一套枚举

先找三值的数值来源，再列出通用、Self、Calling、CallingOrSelf 的 delivery/preflight 入口。手写一张表，注明哪些入口接收 package、tag、message，哪些只能由包装方法补全。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final int PERMISSION_GRANTED =  PackageManager.PERMISSION_GRANTED;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static final int PERMISSION_HARD_DENIED =  PackageManager.PERMISSION_DENIED;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static final int PERMISSION_SOFT_DENIED =  PackageManager.PERMISSION_DENIED - 1;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static final int PID_UNKNOWN = -1;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkPermissionForDataDelivery' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkPermissionForPreflight' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkSelfPermissionForDataDelivery' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkSelfPermissionForPreflight' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkCallingPermissionForDataDelivery' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkCallingPermissionForPreflight' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkCallingOrSelfPermissionForDataDelivery' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'public static int checkCallingOrSelfPermissionForPreflight' frameworks/base/core/java/android/content/PermissionChecker.java
```

## 3. Self、Calling 与 OrSelf 先决定身份，不能由方法名猜调用者

四种包装方式的真实输入如下：

| 入口 | pid/uid | package | delivery tag | self 边界 |
|---|---|---|---|---|
| 通用重载 | 调用方显式传入 | 显式或 null | 显式 | 调用方负责身份可信性 |
| Self | `Process.myPid/myUid` | Context package | Context tag | 文档要求当前 Binder callingUid 与 myUid 相同 |
| Calling | Binder calling pid/uid | 显式或 null | 显式 | callingPid 等于 myPid 时直接 HARD |
| CallingOrSelf | Binder calling pid/uid | self 时 Context package，其他情况固定 null | self 时 Context tag，其他情况用参数 | 允许当前 Binder identity 为 self |

“self”与“外部”描述的是当前线程的 Binder identity，不是 Java 方法是不是同进程调用。服务正在同步处理外部事务、尚未 clear identity 时，同线程 helper 仍可看到外部 caller；clear 后、离开事务或切到普通异步线程时，通常看到 self。

Calling 两个入口用 `callingPid == myPid`做防误用拒绝。OrSelf 没有 package 参数：外部分支固定把 null 交给 common，再按 UID 选第一个包。shared UID 场景若必须绑定真实业务包，应使用能够显式传 package 的 Calling 或通用重载，并在入口先验证 package 属于 callingUid；不能指望 OrSelf 接收一个它根本没有的参数。

Self 入口则无条件使用进程自身 pid/uid。正在处理外部 IPC 时用 Self，不会替 caller 做检查；Javadoc 明示它假定 `Binder.getCallingUid() == Process.myUid()`。Context 的 attribution tag 也只描述 proxy/self 这一端的功能，不会神奇变成远端客户端的 tag。

PID 的作用范围很窄：它只流入普通 permission、runtime 的第一道基础检查，以及 APPOP-flag 分支遇到 `MODE_DEFAULT`后的基础回退。AppOps raw/note 使用 uid、package、tag，不使用 pid。

### 练习 2：按当前 Binder identity 推演四种包装器

分别推演外部事务、同步 helper、clear 后调用和 Handler 异步调用。特别确认 OrSelf 外部分支不能显式带 package，而 Self 始终传本进程身份。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (Binder.getCallingPid() == Process.myPid()) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return checkPermissionForDataDelivery(context, permission, Binder.getCallingPid(),' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return checkPermissionForPreflight(context, permission, Binder.getCallingPid(),' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'String packageName = (Binder.getCallingPid() == Process.myPid())' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F '? context.getPackageName() : null;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F '? context.getAttributionTag() : attributionTag;' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return checkPermissionForDataDelivery(context, permission, Process.myPid(),' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return checkPermissionForPreflight(context, permission, Process.myPid(),' frameworks/base/core/java/android/content/PermissionChecker.java
```

## 4. common 先查定义、再补包，分类顺序决定整个真值表

`checkPermissionCommon()`每次先调用 `getPermissionInfo(permission, 0)`。找不到定义时立即 HARD；不会把未知名字降级成“普通 permission”继续查。这里没有 PermissionChecker 自己维护的定义 cache，所以性能分析不能只数 AppOps Binder 调用。

若 package 为 null，common 调用 `getPackagesForUid(uid)`，只取数组第一个元素；没有数组或数组为空就继续保留 null。这个选择有两层边界：

- shared UID 可对应多个合法包，第一项不等于真实业务发起包；
- 查询结果可能为空，之后 APPOP-flag 与 runtime 分支对 null package 的结果正好相反。

补包后先判断 `permissionInfo.isAppOp()`，再判断 `isRuntime()`，最后才是普通 permission。`isAppOp()`看的是 `PROTECTION_FLAG_APPOP`，`isRuntime()`看 dangerous base protection；即使某个定义同时满足二者，也会先进入 APPOP-flag 分支。返回类型、DEFAULT 语义和是否先查基础位都由这个顺序决定。

普通分支最简单：直接返回 `context.checkPermission(permission, pid, uid)`。它不映射 op、不读取 AppOps，也不留下访问记录。由于 PermissionChecker 的 GRANTED/HARD 数值复用 PackageManager 常量，这个直接返回仍符合三值合同。

### 练习 3：只沿 common 画出三个出口

从定义查询开始，标出未知定义、null 包补全、APPOP-flag、runtime 与普通分支。再给一个 shared UID 两包案例，说明“包属于同一 UID”为什么仍不能确定包级 mode 与 tag。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'permissionInfo = context.getPackageManager().getPermissionInfo(permission, 0);' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'catch (PackageManager.NameNotFoundException ignored) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'if (packageName == null) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'String[] packageNames = context.getPackageManager().getPackagesForUid(uid);' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'packageName = packageNames[0];' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'if (permissionInfo.isAppOp()) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'if (permissionInfo.isRuntime()) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return context.checkPermission(permission, pid, uid);' frameworks/base/core/java/android/content/PermissionChecker.java
```

## 5. 权威真值表：APPOP-flag 与 runtime 的 DEFAULT、null 包完全不同

先看 APPOP-flag 分支。它不先要求基础 permission 已 grant，而是先把 permission 映射为 op：

| op/package 与 mode | 结果 | 是否再查基础 permission |
|---|---|---|
| op 为 null或 package 为 null | HARD | 否 |
| `ALLOWED` | GRANTED | 否 |
| raw `FOREGROUND` | GRANTED | 否 |
| `DEFAULT` | 基础位 grant 则 GRANTED，否则 HARD | 是 |
| `IGNORED`、`ERRORED`及其他值 | HARD | 否 |

这意味着“APPOP permission 必须同时满足 permission 与 AppOp”并不准确：显式 ALLOWED 可以直接放行；只有 DEFAULT 才回退基础位。delivery 中还有一个更窄的完成点缺口：`noteProxyOperation()`要求 proxy mode 必须是 ALLOWED 才继续 proxied；若 proxy 返回 DEFAULT，它已经短路，PermissionChecker 却可能在目标基础位通过后返回 GRANTED。此时不能宣称目标侧已被 note。

runtime 分支的顺序相反：先查基础 permission，失败立即 HARD，根本不调用 AppOps；通过后才映射 op。

| runtime 基础位 | op/package 与 mode | 结果 |
|---|---|---|
| deny | 任意 | HARD，AppOps 未调用 |
| grant | op 为 null或 package 为 null | GRANTED，AppOps 未调用 |
| grant | `ALLOWED`或 raw `FOREGROUND` | GRANTED |
| grant | `DEFAULT`、`IGNORED`、`ERRORED`及其他值 | SOFT |

所以“op/package 缺失一定 HARD”只适用于 APPOP-flag 分支；runtime 恰好直接 GRANTED。也正因此，调用方不能故意用 null package 绕过包级 AppOps 后宣称归因完整。

标准 r48 delivery 的 `noteOperationUnchecked()`会把保存的 FOREGROUND 评价成 ALLOWED 或 IGNORED，正常不会把 FOREGROUND 原样返回；PermissionChecker 的 delivery switch 仍接受 FOREGROUND，可视为防御性兼容。preflight raw 才是 FOREGROUND 的主要可达路径。

### 练习 4：手算两个分支的完整 mode 表

给 APPOP-flag 与 runtime 各填一张 `op/package × mode` 表。重点比较 DEFAULT、null package、基础位拒绝，并指出哪些格子根本不会发起 AppOps 调用。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final String op = AppOpsManager.permissionToOp(permission);' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'if (op == null || packageName == null) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F '? appOpsManager.noteProxyOpNoThrow(op, packageName, uid, attributionTag, message)' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F ': appOpsManager.unsafeCheckOpRawNoThrow(op, uid, packageName);' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'case AppOpsManager.MODE_ALLOWED:' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'case AppOpsManager.MODE_FOREGROUND:' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'case AppOpsManager.MODE_DEFAULT: {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'if (context.checkPermission(permission, pid, uid) == PackageManager.PERMISSION_DENIED) {' frameworks/base/core/java/android/content/PermissionChecker.java
grep -n -F 'return PERMISSION_SOFT_DENIED;' frameworks/base/core/java/android/content/PermissionChecker.java
```

## 6. preflight raw 保留 FOREGROUND，但绝不是最终安全检查

`unsafeCheckOpRawNoThrow()`经 Binder 调到 `checkOperationRaw()`，后者把 `raw=true`交给共同实现。服务端仍会验证 op 范围、解析特殊 UID 的包名、尝试核对 package/uid、检查 package suspend 与用户 restriction，再把原始 op 映射到 switch op。只有最后读取 UID/package mode 时，raw 才决定“不调用 `evalMode()`”。

因此 raw 不等于“直接读 appops.xml 的一格”：

| 阶段 | raw 路径行为 |
|---|---|
| op code | 越界抛参数异常 |
| 特殊 UID package | root/shell/media/audio/camera/system 有规范化规则 |
| package/uid | `verifyAndGetBypass()`会尝试核对 |
| package suspend | 指定 op 可直接得到 IGNORED |
| user restriction | 可直接得到 IGNORED |
| switch | 按 switchCode 找 UID/package policy |
| FOREGROUND | 返回保存值，不按当前 UID state 转换 |

但“尝试核对 package/uid”不等于可以当安全门。`checkOperationUnchecked()`捕获核对失败的 `SecurityException`，返回 `opToDefaultMode(code)`；许多 op 的默认值恰好是 ALLOWED。它也不调用 `verifyIncomingUid(uid)`把目标 UID 绑定为 Binder caller。AppOpsManager 的文档因此明确称 quick check 不是 security check。

这解释了正确配对：preflight 可用来尽早拒绝明确不可能的工作；真正访问仍必须在 delivery 边界调用 note，或为持续资源调用 start。服务还必须自己从 Binder identity 得到可信 UID，并验证客户端自报 package，而不能把 raw check 的返回值当身份认证。

还有一个 r48 非对称：raw 路径显式调用 `isOpRestrictedDueToSuspend()`，`noteOperationUnchecked()`没有这一步，只检查 `isOpRestrictedLocked()`的用户 restriction。不能笼统声称 delivery 是 preflight 的严格超集；应按具体入口读所有早退门。

### 练习 5：证明 raw 既有政策门，也会不安全地回落默认值

沿客户端、Binder 服务和 unchecked 三层追踪。找到 package/uid 不匹配被转成 op 默认 mode 的位置，再确认 raw 仍执行 suspend、restriction 与 switch 映射。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return mService.checkOperationRaw(op, uid, packageName);' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'public int checkOperationRaw(int code, int uid, String packageName) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return checkOperationInternal(code, uid, packageName, true /*raw*/);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'bypass = verifyAndGetBypass(uid, packageName, null);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return AppOpsManager.opToDefaultMode(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (isOpRestrictedDueToSuspend(code, packageName, uid)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (isOpRestrictedLocked(uid, code, packageName, bypass)) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'code = AppOpsManager.opToSwitch(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return raw ? rawMode : uidState.evalMode(code, rawMode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return raw ? op.mode : op.evalMode();' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 7. delivery 把当前进程写成 proxy，把目标参数写成 proxied

进入 AppOps delivery 分支后，PermissionChecker 调用 `AppOpsManager.noteProxyOpNoThrow(op, targetPackage, targetUid, targetTag, message)`。AppOpsManager 不允许这个封装的调用者任意填写 proxy 身份：

| 角色 | UID | package | tag |
|---|---|---|---|
| proxied | PermissionChecker 传入的目标 UID | 目标 package | 目标 attributionTag |
| proxy | `Process.myUid()` | Context 的 op package | Context 的 attributionTag |

随后调用进入 AppOpsService。服务先 `verifyIncomingUid(proxyUid)`：proxy UID 等于当前 Binder caller 时通过；当前 `Binder.getCallingPid()`恰为 AppOpsService 所在进程 PID 时也通过；否则要求 caller 持有 `UPDATE_APP_OPS_STATS`。第二项判断的是当前 Binder identity，不是“Java 调用发生在同进程”就无条件成立。这道门验证的是 proxy 参数，也不会把最初业务 caller 的 UID自动跨远程 Binder hop 传进去。

`message`描述这次访问原因，可能参与 async-noted-op 收集或运行时访问提示，不参与 permission/mode 放行。target tag 与 Context tag也不能混用：前者是 proxied 包内功能，后者是 proxy 包内功能。

preflight 不走 proxy API，只查询 target uid/package 的 raw mode；它不会提前评价当前服务自身的 proxy mode。于是“注册成功”最多说明目标将来可能允许，不能保证 delivery 时代理端、目标端、restriction 与 UID state 都通过。

### 练习 6：逐参数标记 proxy 与 proxied 身份

从 AppOpsManager 的参数装配一路标到 AppOpsService，画出两端 uid/package/tag 的来源。再解释为何跨进程后服务端看到的 Binder caller 是 proxy 进程。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int myUid = Process.myUid();' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'int mode = mService.noteProxyOperation(op, proxiedUid, proxiedPackageName,' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'proxiedAttributionTag, myUid, mContext.getOpPackageName(),' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'mContext.getAttributionTag(), collectionMode == COLLECT_ASYNC, message,' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'verifyIncomingUid(proxyUid);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'verifyIncomingOp(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'String resolveProxyPackageName = resolvePackageName(proxyUid, proxyPackageName);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'final boolean isSelfBlame = Binder.getCallingUid() == proxiedUid;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (proxyMode != AppOpsManager.MODE_ALLOWED || Binder.getCallingUid() == proxiedUid) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'String resolveProxiedPackageName = resolvePackageName(proxiedUid, proxiedPackageName);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 8. noteProxy 是 proxy-first 的短路状态机，不保证“两边各记一次”

`noteProxyOperation()`的次序必须逐项读：

1. 验证 proxy UID 与 op；
2. 规范化 proxy package，无法解析则直接 IGNORED；
3. 计算 trusted/self-blame，并选择 proxy flag；
4. 对 proxy 调用 `noteOperationUnchecked()`；
5. proxy mode 不是 ALLOWED，立即返回该 mode；
6. 实际 Binder caller UID 等于 proxiedUid，也立即返回，避免同一主体双记；
7. 规范化 proxied package，失败返回 IGNORED；
8. 对 proxied 调用 `noteOperationUnchecked()`，返回目标 mode。

trusted 条件是三选一：proxy UID持有 `UPDATE_APP_OPS_STATS`；满足 r48 针对 `RECORD_AUDIO`的预装 voice service 特例；或实际 Binder caller UID 等于 proxiedUid。信任只选择 `TRUSTED_PROXY/TRUSTED_PROXIED`还是 `UNTRUSTED_PROXY/UNTRUSTED_PROXIED`，并影响异步收集；它不跳过 proxy mode，也不直接放行 target mode。

self-blame 的判断用实际 Binder caller UID，不是传入的 proxyUid。它在 proxy note 后短路，proxied package/tag 根本不会继续解析。标准 Self PermissionChecker 把 `context.getPackageName()`作为 target package，AppOpsManager却用 `mContext.getOpPackageName()`作为 proxy package；两者通常相关却并非 API 上保证相同。直接调用 noteProxy API 时更不能把这个分支理解成“已验证目标归因”。

记账效果要按失败点区分：

| 结束点 | proxy 侧 | proxied 侧 | 返回 |
|---|---|---|---|
| proxy package 无法解析 | 无 note | 无 | IGNORED |
| proxy 身份校验抛错 | 无 | 无 | 异常越过 no-throw 的 mode 合同 |
| proxy mode 拒绝 | 可能有 reject/通知，取决于具体早退 | 无 | proxy mode |
| self-blame | 一次 proxy 结果 | 无 | proxy mode |
| target package 无法解析 | proxy 已成功 access | 无 | IGNORED |
| target 身份核对失败 | proxy 已成功 access | 不产生 `rejected()` | ERRORED |
| 两侧 mode 都允许 | proxy access | proxied access，带 proxy info | target ALLOWED |
| target mode 拒绝 | proxy access | mode 分支写 reject；某些 restriction 早退只通知 | target mode |

因此 data delivery 不是“必然记一条”，也不是“必然把双方访问或拒绝都写完整”。它只保证按这台短路状态机执行；到底留下零条、一侧还是两侧证据，要结合结束点判断。

## 9. 最终 mode 先看 restriction，再按 switch、UID 与 package 求值

`noteOperationUnchecked()`先核对 uid/package/tag并取得 restriction bypass，再创建或取得 `Ops`与原始 `Op`。接着检查用户 restriction；只有未命中时才创建或取得 attribution bucket，并用 `opToSwitch(code)`决定控制开关。因此 restriction 早退可能已经建立 `Ops/Op`并安排写盘，却没有 attribution bucket 或 reject event。

mode 优先级是：

1. 如果 `UidState.opModes`含 switchCode，使用 UID mode；
2. 否则读取该 package 的 switch op mode；
3. 没有显式记录时，`Op`以该 op 的默认 mode 初始化；
4. note/start 用 `evalMode()`评价动态状态，raw check 则保留保存值。

控制粒度与记账粒度可以不同：策略读 switch op，`AttributedOp`却仍挂在原始 code 上。shared UID 的 UID mode可统一覆盖成员，而 package mode仍依赖 common 选中的具体包；这正是 null package 取“第一包”会改变结果和归因的原因。

mode 拒绝分支会调用 `AttributedOp.rejected()`，但更早的 package 核对失败、`getOpsLocked()`失败或 user restriction 分支并不都调用它。`scheduleOpNotedIfNeededLocked()`的观察者通知也不是等价的持久 reject event，排障时不能只凭“返回 IGNORED”反推账里一定有什么。

## 10. FOREGROUND 要经 UID state 与 capability，CAMERA/MIC 的 ALLOWED 也可能收缩

`UidState.evalMode()`不是简单判断“进程列表里有没有前台进程”。当保存 mode 是 FOREGROUND 时，visible AppWidget、pending-top 或 TOP state 会在检查 capability 之前直接 ALLOWED。只有落到“state 仍不劣于该 op 的首个不受限 UID state”这一分支时，位置、相机、麦克风才要求对应 capability；再差的状态直接 IGNORED。

当保存 mode 已是 ALLOWED 时，大多数 op直接保持 ALLOWED，但 r48 对 CAMERA 与 RECORD_AUDIO 再加一层 while-in-use 收缩：pending-top、临时 FGS while-in-use allowlist 或相应 capability 至少满足一项，否则仍变成 IGNORED。由此得到两个容易漏掉的结论：

- preflight raw FOREGROUND 返回 GRANTED，只表示未来状态可能满足；
- preflight raw ALLOWED 对 CAMERA/MIC 也不承诺 delivery 允许，因为 raw 跳过了这项特殊评价。

UID state/capability 会随前后台与进程生命周期变化。即使 registration 与第一次 delivery 只隔几毫秒，也必须把它们视作两个时点；note 后到真正 Binder/共享内存发送之间仍有竞态，持续资源还要靠 active 生命周期、mode 回调和停止协议继续收敛。

### 练习 7：手算 LOCATION、CAMERA、MIC 的动态评价

分别计算 raw FOREGROUND、evaluated FOREGROUND 与 raw ALLOWED。覆盖 widget、pending-top、TOP、普通前台、后台、capability 有无及临时 FGS allowlist。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int evalMode(int op, int mode) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (mode == MODE_FOREGROUND) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (appWidgetVisible) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mActivityManagerInternal.isPendingTopUid(uid)' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'state <= AppOpsManager.resolveFirstUnrestrictedUidState(op)' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'PROCESS_CAPABILITY_FOREGROUND_LOCATION' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'PROCESS_CAPABILITY_FOREGROUND_CAMERA' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'PROCESS_CAPABILITY_FOREGROUND_MICROPHONE' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mActivityManagerInternal.isTempAllowlistedForFgsWhileInUse(' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return MODE_IGNORED;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 11. attributionTag 是包内审计维度，不是身份或授权凭证

Context 的 `createAttributionContext(tag)`创建一个携带 tag 的 Context 视图；包、UID 与 LoadedApk 没有因此改变，创建动作也不会自动 note。Manifest 顶层 `<attribution>`为 tag 提供稳定声明和用户可读 label：

| 解析约束 | r48 行为 |
|---|---|
| tag | 必填，兼容读取旧 featureId，最长 50 字符 |
| label | 必填资源 |
| 数量 | 每包最多 1000 项 |
| 当前 tag | 包内不可重复 |
| inherit-from | 不能仍是当前声明 tag；同一旧 tag不能被多个新 tag重复继承 |

组合校验失败会让包解析以 bad manifest 结束，不是等 AppOps 第一次调用时才决定。`inherit-from`也不继承 permission 或 mode：AppOps mode仍在 UID/package/switch op 层。包替换时，AppOpsService 读取继承映射，把已移除 tag 的当前 access/reject event 合并到继任 tag；没有映射的旧 tag会并入 null/default bucket。正在运行的 start event 不会迁到新 bucket，`add()`会结束并释放它；这也没有把独立 HistoricalRegistry 承诺成同一份原子迁移。

运行时的 `verifyAndGetBypass(uid, package, tag)`通常会核对 package 属于 uid；但 root UID在方法开头直接返回，包与 tag 都跳过验证。其余普通包中，null tag有效，声明中的 tag有效；未声明 tag 时 r48 只写错误日志，不因此拒绝，后续 edit 路径仍可把它加入 `knownAttributionTags`并创建 bucket。其他没有常规包对象但能由固定名字解析的特殊 UID也允许任意 tag。

所以 tag 只能回答“这个包自报是哪项功能在使用”，不能回答“调用者是谁”或“是否有权访问”。服务不能因 tag 名为 `trusted_feature`就授予能力；真正的身份边界仍是 Binder UID、package 归属、permission 与 AppOps。

### 练习 8：区分安装期组合校验、运行时包核对与 tag 弱校验

列出 tag/label/数量/唯一性/继承约束，再追包替换时旧 bucket 的合并。最后比较“包不属于 UID”和“tag 未声明”的不同失败强度。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final int MAX_ATTRIBUTION_TAG_LEN = 50;' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttribution.java
grep -n -F 'private static final int MAX_NUM_ATTRIBUTIONS = 1000;' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttribution.java
grep -n -F 'return input.error("<attribution> does not specify android:tag");' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttributionUtils.java
grep -n -F 'return input.error("<attribution> does not specify android:label");' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttributionUtils.java
grep -n -F 'boolean wasAdded = attributionTags.add(attributions.get(attributionNum).tag);' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttribution.java
grep -n -F 'if (attributionTags.contains(inheritFrom)) {' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttribution.java
grep -n -F 'boolean wasAdded = inheritFromAttributionTags.add(inheritFrom);' frameworks/base/core/java/android/content/pm/parsing/component/ParsedAttribution.java
grep -n -F 'if (!ParsedAttribution.isCombinationValid(pkg.getAttributions())) {' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'if (!isAttributionTagValid) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'ops.knownAttributionTags.add(attributionTag);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'dstAttributionTags.put(attribution.inheritFrom.get(inheritFromNum),' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'newAttributedOp.add(op.mAttributions.valueAt(attributionNum));' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

## 12. access、reject、proxy info 与 watcher 是几种不同证据

允许的瞬时 note 调用 `AttributedOp.accessed()`：按 uidState 与 flags 组成 key，更新最近访问事件，并增加 HistoricalRegistry 的 access count。proxied 成功事件还带 `OpEventProxyInfo(proxyUid, proxyPackage, proxyTag)`；proxy 自身事件的 proxyUid 是 INVALID，不再嵌套另一层来源。

mode 拒绝调用 `rejected()`，更新最近拒绝事件并增加拒绝计数，但 r48 的 reject event明确不保存 proxy info。trusted/untrusted 的 proxy/proxied flags仍能区分记账角色，却不能从拒绝事件还原完整另一端三元组。

noted watcher、async noted-op 与持久/历史字段又是不同通道：

- `scheduleOpNotedIfNeededLocked()`通知有人尝试 note 及其 mode；
- `accessed/rejected`维护最近事件与 HistoricalRegistry 计数；
- async collection 可附带 message，必要时客户端生成调用栈说明；
- AppOpsManager 只在返回 ALLOWED 后做本地 SELF/SYNC 收集。proxy 的同步收集只认可 `UPDATE_APP_OPS_STATS`或 voice-service 特例，并没有复用服务端还包含 self-blame 的三路 trusted 谓词。

于是“隐私面板没有一条记录”不能直接推出没有检查，“有 watcher 回调”也不能推出已写成功访问；必须先确认控制流在哪个副作用点结束。

## 13. note 是瞬时事件；持续资源必须用 start/finish，但 r48 不支持 proxy active

`noteOp`适合一次离散访问。成功事件 duration 为 -1，不代表资源一直占用。`startOpNoThrow()`则把 AppOpsManager 的 clientId、op、uid、package、tag交给服务，在对应 `AttributedOp`内创建 `InProgressStartOpEvent`；只有同一 uid/package/op/tag bucket中的同一 clientId才用计数嵌套。active watcher又聚合到父 `Op`：跨 tag、跨 client 的第一个运行实例触发 true，最后一个结束才触发 false。

`finishOp()`必须使用相同 clientId、op、uid、package、tag。最终 finish 用 elapsedRealtime 计算 duration，并把 active 区间写成 access event；客户端 Binder 死亡时 death recipient把嵌套计数压到一次，再调用 finish 收尾。这是异常兜底，不替代调用方在正常路径的 finally 配对。

r48 的 AIDL只有单独的 `noteProxyOperation()`，而 `startOperation()/finishOperation()`是 self flags 模型；`AttributedOp.started/finished()`也明确使用 `OP_FLAG_SELF`。因此 PermissionChecker 的 delivery note 不能替长时相机、录音或定位会话建立 proxy active duration，后续版本的链式 start/finish 协议不能倒灌到本版。

还有一个完成点差别：start 成功时就增加访问次数，finish 才写 duration；服务进程返回 `MODE_ALLOWED`不等于资源已经打开，finish 返回也不等于底层设备关闭完成。AppOps只管理这份策略与审计账。

### 练习 9：对照单跳 note、SELF-flag start/finish 与真实调用点

先从 AIDL 证明 noteProxy 与 start/finish 是分开的接口，再追 clientId、嵌套计数和 Binder death。最后对照 ContentProvider 的瞬时 note 与 RecognitionService 的 delivery 检查。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int noteProxyOperation(int code, int proxiedUid, String proxiedPackageName,' frameworks/base/core/java/com/android/internal/app/IAppOpsService.aidl
grep -n -F 'int startOperation(IBinder clientId, int code, int uid, String packageName,' frameworks/base/core/java/com/android/internal/app/IAppOpsService.aidl
grep -n -F 'void finishOperation(IBinder clientId, int code, int uid, String packageName,' frameworks/base/core/java/com/android/internal/app/IAppOpsService.aidl
grep -n -F 'int mode = mService.startOperation(getClientId(), op, uid, packageName,' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'mService.finishOperation(getClientId(), op, uid, packageName, attributionTag);' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'attributedOp.started(clientId, uidState.state);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'event.numUnfinishedStarts++;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'attributedOp.finished(clientId);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'void onClientDeath(@NonNull IBinder clientId) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'deadEvent.numUnfinishedStarts = 1;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F '// startOp events don'"'"'t support proxy, hence use flags==SELF' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return noteProxyOp(callingPkg, attributionTag, mReadOp);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'PermissionChecker.checkCallingPermissionForDataDelivery(this,' frameworks/base/core/java/android/speech/RecognitionService.java
```

## 14. 两个真实范例说明“检查点”必须贴着业务阶段

ContentProvider transport 先做组件 permission、路径 permission、跨用户与 URI grant裁决；inner 返回 ALLOWED 后，若 provider 配有 read/write AppOp，再调用 `noteProxyOp(callingPkg, attributionTag, op)`。即使是 URI grant使对象级访问通过，外层仍会执行 configured AppOp note。这里展示的是“对象能力/组件门”和“操作策略/审计门”正交，而不是 PermissionChecker 包办一切。

ContentProvider 的 note 还把 `MODE_DEFAULT`转成 IGNORED；这是该调用点自己的合同，不能反推 PermissionChecker 的 APPOP-flag DEFAULT 也拒绝。不同上层 API必须显式解释 DEFAULT。

RecognitionService 展示了“时机由业务动作决定”：`startListening()`把即将代表 caller 使用录音能力视为 delivery，用显式 package/featureId 调 `checkCallingPermissionForDataDelivery()`；`stopListening()`与 `cancel()`不会开始新的敏感数据访问，改走 `checkCallingOrSelfPermissionForPreflight()`。后两者虽然接收 package/featureId，当前 helper却没有把它们传给 OrSelf preflight，外部 shared UID仍会落到第一包。调用点还要自己决定错误回调、会话状态与清理；PermissionChecker 只返回分类。

一个监听型服务可以采用同样的协议：注册时保存入口验证过的 uid/package/token；每次发送前重新做 delivery 检查；SOFT 时跳过或按合同降级，HARD 时拒绝/清理；长时资源再建立 start/finish。不能把注册时的 GRANTED 缓存在整个进程生命周期中。

## 15. 边界矩阵：最危险的错误都来自把一个完成点外推到另一层

下面这张表适合直接用于代码评审：

| 写法或现象 | 实际风险 | 应核对的边界 |
|---|---|---|
| 只查 `Context.checkPermission()` | legacy App可能基础位保留、AppOp 已禁用 | runtime grant 与 AppOp 分账 |
| preflight 成功后永久发送 | mode、UID state、capability 可变 | 每个 delivery 或受控会话重查 |
| 把 raw check 当身份认证 | UID不绑定 Binder caller；包不匹配可回默认 mode | 入口先固定 callingUid 并验证 package |
| null package 交给 common | shared UID 第一包歧义；runtime 甚至可跳 AppOps | 使用可信显式业务包 |
| `result != GRANTED`后丢弃类型 | HARD/SOFT 的上层合同无法区分 | 三值分支完整处理 |
| 把 SOFT 当可继续 | 仍可能是 ERRORED 或长期政策拒绝 | 不交付完整敏感数据 |
| 信任 attributionTag | tag 不是身份，r48 未声明只日志 | UID/package/permission 才是安全门 |
| 认为 trusted proxy 免查 | trusted 不跳过 proxy mode；非self才可能继续目标，self-blame 会短路 | proxy-first 状态机 |
| 看到 delivery 就认定双记录 | 早退、self-blame、DEFAULT 回退均可不完整 | 对照具体结束点 |
| note 一次代表持续 active | note duration 为瞬时；没有 active 生命周期 | start/finish 与异常收尾 |
| 认为 delivery 包含 suspend raw 门 | r48 note unchecked 缺该显式检查 | 分别读取 check/note 的 restriction |
| clear identity 后用 CallingOrSelf | 当前 Binder identity 已变 self | 清身份前保存并验证 caller |

排障可按五步收敛：

1. 先读 PermissionInfo，确认实际命中 APPOP-flag、runtime 还是普通分支；
2. 记录可信 pid/uid、显式 package、target tag、proxy Context package/tag；
3. 同时查看目标 raw mode、switchCode、UID mode、package mode、UID state/capability 与 restrictions；
4. delivery 失败时分别判断 proxy 是否已 note、是否进入 target、返回是哪一端 mode；
5. 对持续资源再查 active event、clientId、嵌套 start、finish 与 Binder death，而不是只看最近 note。

特别注意 APPOP-flag 的 DEFAULT：单个最终 GRANTED 可能只是 PermissionChecker 的基础 permission 回退成功，不证明 proxied note 已发生。也要注意 runtime 的 null package：基础位通过后直接 GRANTED，不证明任何 AppOps 门被执行。结果码相同，证据强度可以完全不同。

## 16. 用端到端链和完成点矩阵收束结论

三条 PermissionChecker 主链可以压缩为：

| 定义类型 | 主链 | 最终分类 |
|---|---|---|
| 普通 | definition → `Context.checkPermission(pid,uid)` | GRANTED/HARD |
| APPOP-flag | definition → package → raw/note mode；DEFAULT 才回基础 permission | GRANTED/HARD |
| runtime | definition → 基础 permission → package/op → raw/note mode | GRANTED/HARD/SOFT |

preflight 与 delivery 的端到端差别则是：

| 路径 | 实际执行 | 没有证明什么 |
|---|---|---|
| preflight AppOps | 只查 target raw；保留 FOREGROUND；仍可受 suspend/user restriction；不记 delivery | proxy 当前允许、UID动态评价通过、访问已发生、身份强校验完成 |
| delivery AppOps | AppOpsManager固定当前进程为 proxy；服务 proxy-first，允许且非self才进 target；按动态 mode写可达副作用 | 必然双记、必然记录所有拒绝原因、持续资源仍 active |
| start/finish | 单一目标 uid/package/tag 以 SELF flag 建立 clientId active 区间，finish或death关闭 | 单跳 proxy 责任链、底层资源开关完成 |

最后按“完成点”解释每种结果：

| 已到达的点 | 已经证明 | 仍未证明 |
|---|---|---|
| common 找到 PermissionInfo | 名字有当前定义 | 基础授权或 AppOp 允许 |
| 普通分支 GRANTED | 基础 pid/uid permission 通过 | AppOps、对象归属、数据交付 |
| preflight GRANTED | 该分支按 raw/基础规则可能允许 | delivery 时的 proxy/target 动态结果 |
| runtime HARD | 基础 runtime permission拒绝 | AppOps通常根本未调用 |
| runtime SOFT | 基础位通过、AppOps结果未允许 | 原因临时、可恢复或可忽略 |
| delivery GRANTED | 当前分支最终允许 | target 必有完整 proxy 记录；DEFAULT 特例尤其要单查 |
| 两侧 access event | proxy 与 proxied 的瞬时 note成功 | 资源持续占用、未来回调仍允许 |
| active event存在 | start/finish 账仍在运行 | r48 已表达 proxy 链、硬件资源仍打开 |

因此，最可靠的实现不是寻找一个“万能 permission API”，而是把身份固定、基础授权、AppOps policy、当前 UID 状态、归因与资源生命周期放在各自正确的边界：注册前只做不产生虚假访问的 preflight，真正交付前重新 note，持续资源明确 start/finish，并且始终把 package/tag 的归属与 Binder caller 分开验证。

下一章进入第 270 章，继续拆 `AppOpsService` 的 mode 存储、UID/package 优先级、watcher、历史持久化与用户 restriction 状态机。
