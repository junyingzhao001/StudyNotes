# 282 Android ContentProvider.Transport 执行链：读写权限、PathPermission、URI grant、CallingPackage、Attribution、AppOps、跨用户与 CRUD/Bulk

## 1. 先看结论：Transport 不是一扇权限门，而是一条逐接口收束链

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。上一章已经把 authority 找到 Provider、建立引用和处理死亡的链路收束到 `IContentProvider`；本章从这个 Binder 的服务端入口继续向里追，回答一次 query、insert、openFile 或 applyBatch 到底何时验证 URI、何时检查读写权限、何时记录 AppOps、何时设置调用者身份，以及拒绝后究竟是抛异常、返回空结果还是仍然执行一小段 Provider 代码。

先把主线压成九个完成点：

1. `ContentProviderNative.onTransact()` 解包 Binder 参数，再把请求交给 Provider 内部的 `Transport`；
2. Transport 先核对 URI 中的 user 与 authority，并折叠 path 中连续的 `/`；
3. 读写接口分别进入 `enforceReadPermissionInner()` 或 `enforceWritePermissionInner()`，按 same-app、exported/cross-user、顶层权限、PathPermission、具体 URI grant 的顺序求值；
4. Manifest permission 若关联 AppOp，会在权限分支中 note；Provider 通过 `setAppOps()` 配置的读写 op 又在外层 note，一次请求可能经过两层 AppOps；
5. 静态权限失败通常抛 `SecurityException`；AppOps 若返回可静默忽略的 mode，各接口再把它翻译成空 Cursor、0、null、false、dummy URI、`FileNotFoundException` 或 batch 异常，而 errored mode 会在 AppOps API 内直接抛异常；
6. 只有进入业务动态范围时，Transport 才把 `(callingPkg, attributionTag)` 放进当前线程的 `ThreadLocal`，退出时恢复嵌套调用原值；
7. query 的正常 Cursor 会在跨进程边界变成 BulkCursor，文件接口则转移文件描述符；两者的完成点和释放协议完全不同；
8. call、getType、getStreamTypes 与动态 `checkUriPermission` 是通用读写门之外的例外，Provider 必须理解各自的安全契约；
9. applyBatch 先逐项改写 URI 并预检，再一次进入 Provider；默认实现只是顺序循环，不承诺事务、回滚或消费 yield 标记。

因此，“已经拿到 Provider Binder”“Manifest 声明了 permission”“AppOps 被拒绝”“业务方法没有被调用”都不是可互换的结论。正确排查必须同时回答四个问题：请求落在哪个 user/authority/path，内层 permission 与 URI grant 得到什么结果，外层 AppOp 怎样翻译，目标接口把拒绝映射成什么可观察值。

## 2. 对象边界：手写 Binder 骨架负责传输，Transport 负责策略，ContentInterface 负责业务

r48 的 `IContentProvider` 不是由 AIDL 自动生成的 Stub。`ContentProviderNative` 是手写 Binder 骨架：`onTransact()` 按 transaction code 从 Parcel 读取 calling package、attribution、URI、Bundle、observer 与 cancellation Binder，再调用接口方法；客户端的 `ContentProviderProxy` 负责反向封包。异常仍通过 Binder/DatabaseUtils 协议返回，但安全语义并不在 native 层统一决定。

每个 `ContentProvider` 实例持有一个内部 `Transport extends ContentProviderNative`。Transport 保存 `AppOpsManager`、可选的 read/write AppOp 和 `mInterface`。通常 `mInterface` 指向外层 Provider 本身，因此 Transport 完成验证后才调用应用覆写的 `query()`、`insert()` 等方法；测试或日志包装可以替换这层接口，但不会绕过 Transport 已经执行的入口逻辑。

这三层要分开读：

| 层 | 主要职责 | 典型完成点 | 不负责什么 |
|---|---|---|---|
| `ContentProviderNative` / Proxy | Parcel 编解码、Cursor/FD 跨进程适配、异常传递 | reply 已写出或代理已解包 | 不统一决定每个接口的权限 |
| `ContentProvider.Transport` | URI 规范化、权限/AppOps、调用者动态范围、拒绝映射 | 已返回拒绝值或进入 `mInterface` | 不保证 Provider 数据层事务 |
| Provider / `ContentInterface` | 路由、查询、写入、文件与自定义方法 | 业务结果及资源已创建 | 不能假设入口替它完成所有细粒度授权 |

同进程调用也不能只凭“没有跨 Binder”推断安全层消失。客户端若持有的是 Transport 并通过 `IContentProvider` 入口调用，仍会走这些检查；`ContentResolver.wrap(ContentProvider)` 等直接包装 `ContentInterface` 的路径则可能直接落到业务接口。分析时应从实际对象和调用入口出发，而不是从进程位置猜测。

### 练习 1：确认手写 Binder、Transport 与 Cursor 适配层

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'class Transport extends ContentProviderNative {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'volatile ContentInterface mInterface = ContentProvider.this;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'public boolean onTransact(int code, Parcel data, Parcel reply, int flags)' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'case QUERY_TRANSACTION:' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'String callingPkg = data.readString();' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'String callingFeatureId = data.readString();' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'Cursor cursor = query(callingPkg, callingFeatureId, url, projection, queryArgs,' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'adaptor = new CursorToBulkCursorAdaptor(cursor, observer,' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'BulkCursorToCursorAdaptor adaptor = new BulkCursorToCursorAdaptor();' frameworks/base/core/java/android/content/ContentProviderNative.java
```

分别画出远端 query 和 `ContentResolver.wrap(ContentProvider)` 的调用图。标出哪条路径需要 Parcel、哪条路径把 Cursor 变成 BulkCursor、哪条路径仍经过 Transport；不要把“本地对象”与“Transport 本地调用”合并成同一种情况。

## 3. URI 第一关：user、authority 与 encoded path 会被校验，scheme 却没有在这里显式核对

绝大多数带 URI 的 Transport 方法第一句都是 `validateIncomingUri(uri)`。它先读取 authority；普通 Provider 若看到显式 userId，该 user 必须是 `USER_CURRENT` 或 Provider Context 所在 user，否则直接抛 `SecurityException`。singleUser Provider 跳过这项 user 一致性检查，因为它本来就可能服务多个调用用户。

随后 `validateIncomingAuthority()` 只把“去掉 user 前缀”的值用于比较，再与 `attachInfo()` 写入的一个或多个 authority 核对；它不会改写传入的 authority 字符串。多 authority 共享同一个 Transport，但不能借此访问该 Provider 未声明的名字。applyBatch 还会先验证外层 authority，再逐项验证 operation URI，二者不是互相替代关系。

authority-only 入口有自己的锐角。`getAuthorityWithoutUserId()`按最后一个 `@`截取后缀，并不验证前缀一定是数字；因此 `call()`只要后缀匹配即可通过 authority 比较，既不做普通 Provider 的 URI-user一致性检查，也会把原始 outer authority 继续传给业务。applyBatch 同样把原 outer authority 传给 Provider，不过其中每个 operation URI仍逐项执行完整 user/authority验证。业务若解释 outer authority 中的 user 信息或其他前缀，必须自行采用严格解析器。

最后，若 `encodedPath` 含连续的 `//`，框架用 `replaceAll("//+", "/")` 折叠空 path segment并记录 warning。权限匹配和业务调用看到的是规范化后的 URI，所以 Provider 不应把 `//admin` 与 `/admin` 当成两个安全域。这里操作的是 encoded path；query 参数与 fragment 不参与这次折叠。

一个容易被常规 ContentResolver 路径掩盖的边界是：`validateIncomingUri()` 没有显式检查 scheme。正常 API 会围绕 `content://` 路由，但已经持有 Binder 的直接调用者可尝试提交另一种具有相同 authority 的分层 URI。Transport 仍会校验 user、authority 与 path，却不会在本方法因 scheme 本身拒绝。安全 Provider 应明确验证自己接受的 URI 形状，不能把“能到达此 Binder”当作 scheme 证明。

验证后，大多数接口调用 `maybeGetUriWithoutUserId()`：普通 Provider 去掉嵌入 user，singleUser 保留。少数接口使用无条件的 `getUriWithoutUserId()`，后文会单列这些不对称。业务方法因而通常不该把 authority 中的 `10@` 当作唯一用户来源；它应按该接口的真实传入形状与调用身份设计。

### 练习 2：重放 user、authority、scheme 与双斜杠四种输入

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!matchesOurAuthorities(getAuthorityWithoutUserId(authority))) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mInterface.call(authority, method, arg, extras);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'ContentProviderResult[] results = mInterface.applyBatch(authority,' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'String auth = uri.getAuthority();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'int userId = getUserIdFromAuthority(auth, UserHandle.USER_CURRENT);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (userId != UserHandle.USER_CURRENT && userId != mContext.getUserId()) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'validateIncomingAuthority(auth);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return auth.substring(end+1);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final String encodedPath = uri.getEncodedPath();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'encodedPath(encodedPath.replaceAll("//+", "/")).build();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'private Uri maybeGetUriWithoutUserId(Uri uri) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return getUriWithoutUserId(uri);' frameworks/base/core/java/android/content/ContentProvider.java
```

以 Context user 10 的普通 Provider 和 user 0 的 singleUser Provider 为两列，分别推演 `content://10@authority/a//b`、`content://11@authority/a`、相同 authority 的非 content scheme、错误 authority。记录在哪一步抛异常、业务最终看到哪个 URI，以及哪些输入仍需 Provider 自己拒绝。

## 4. 读写权限算法：same appId 快路之后，静态声明、跨用户与 URI grant 分层求值

`enforceReadPermissionInner()` 与 write 版本结构近似。在 URI 已通过前置验证、调用者也已取得 Binder 的前提下，第一条快路是 `UserHandle.isSameApp(callingUid, mMyUid)`，比较的是 appId，不是完整 UID。它让同 appId 调用跳过 exported、跨用户、Manifest permission、PathPermission 与 URI grant；共享 UID 或不同 user 下相同 appId 都可能命中，所以业务若还依赖账户、租户或 user 隔离，必须另行检查。普通 Provider 的显式异 user URI仍会更早被 `validateIncomingUri()`拒绝。该快路只结束 inner 层，Provider 配置的外层 AppOp 仍可能执行。

非 same-app 调用只有在 `mExported && checkUser(pid, uid, context)` 时才进入组件和路径权限分支。`checkUser()` 接受三种情况：调用者与 Provider Context 同 user、Provider 为 singleUser、调用者持有 `INTERACT_ACROSS_USERS` 或 `INTERACT_ACROSS_USERS_FULL`。这说明 exported 与跨用户资格是静态权限分支的前置条件，而不是整个算法的最终拒绝点。

静态分支先检查顶层 read/write permission。权限通过并且其关联 AppOp 也允许时立即返回；PathPermission 不再参与。若顶层 permission 为 null，则建立“默认开放”的候选；若顶层权限存在但拒绝，则记录缺失权限和最强拒绝 mode，继续寻找路径授权。

静态分支未允许时，算法在外层再检查具体 URI grant。这个“最后机会”位于 exported/checkUser 分支之外，因此足够具体且方向匹配的 grant 可以打开 non-exported Provider，也可以解决普通静态分支未通过的跨用户访问。反过来，AMS 在获取 Provider Binder 时做的是 authority 级“可能访问”预检；真正的 URI、读写方向和 path 仍以 Transport 此处为准。

若所有路径都失败，只有最强拒绝恰为 `MODE_IGNORED` 才把软拒绝交给接口翻译；其他情形抛 `SecurityException`。错误文案会提示 permission、grant 或 exported，并为 `MANAGE_DOCUMENTS` 给出专用建议，但文案不是完整策略证明，不能据它反推所有曾检查的分支。

## 5. PathPermission 的真实语义：允许路径是 OR，拒绝匹配会撤掉默认开放，顶层成功会短路

PathPermission 只匹配 `uri.getPath()`，不看 query 参数、fragment 或业务 Bundle。一个看似只读 `/items` 的 URI，若 `queryArgs` 能扩大范围、选择任意文件或注入另一条逻辑路径，Manifest path 规则不会自动约束那些参数。Provider 必须在业务层把所有会扩展数据范围的参数纳入授权。

算法遍历所有 PathPermission。只要某条匹配规则的对应 read/write permission 与关联 AppOp 允许，就立即成功；在各项检查都正常返回、没有中途异常的前提下，多条匹配规则具有“任一成功即可”的 OR 特性。若较早规则的关联 AppOp以 errored触发 `SecurityException`，后续允许规则和 URI grant都不会再检查。与此同时，如果顶层 permission 为 null，本来 `allowDefaultRead/Write=true`；任何一条“当前方向 permission非 null、path匹配、但未允许”的规则都会把该方向的默认开放撤掉。只有 writePermission 的匹配规则不会收紧默认 read，反之亦然；没有对应方向匹配规则时，顶层 null仍允许。

这形成三个容易写反的配置结果：

| 顶层权限 | 匹配路径规则 | 结果 |
|---|---|---|
| 已允许 | 更严格规则拒绝 | 顶层已提前返回，路径规则不能收紧 |
| null | 无匹配规则 | 保留默认开放 |
| null | 一条匹配规则正常返回拒绝、另一条匹配规则允许 | 允许规则提前成功；若前一检查抛异常则无法到达 |
| 拒绝 | 匹配规则允许 | 路径规则提供另一条允许路径 |

因此若目标是“Provider 大体开放，但 `/private` 必须持权限”，匹配 `/private` 的 path permission 可以撤掉该路径的默认开放；若同时配置一个已被调用者持有的顶层 permission，则顶层快路会让 `/private` 的更严格规则失效。安全评审应按源码顺序枚举，而不是把 XML 看成由宽到窄的层叠样式表。

### 练习 3：枚举顶层权限与多条 PathPermission 的组合

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (UserHandle.isSameApp(uid, mMyUid)) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (mExported && checkUser(pid, uid, context)) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final String componentPerm = getReadPermission();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'boolean allowDefaultRead = (componentPerm == null);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final PathPermission[] pps = getPathPermissions();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (pathPerm != null && pp.match(path)) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'allowDefaultRead = false;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (allowDefaultRead) return MODE_ALLOWED;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final String componentPerm = getWritePermission();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'boolean allowDefaultWrite = (componentPerm == null);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (allowDefaultWrite) return MODE_ALLOWED;' frameworks/base/core/java/android/content/ContentProvider.java
```

构造四组调用者：无任何权限、只有顶层权限、只有一条 path 权限、同时命中一条允许和一条拒绝规则。分别对 `/public` 与 `/private` 推演 read/write，写出每个提前返回点，并说明为何增加一条顶层权限有时反而扩大了受保护路径的访问面。

## 6. URI grant 是最后机会，但 singleUser 的 read 与 write 在 r48 并不对称

read 静态分支失败后，Transport 取得 calling userId。若 Provider 为 singleUser 且调用 UID 与 Provider UID 不同 user，它通过 `maybeAddUserId()`对“尚无 user且 scheme为 content”的 URI尝试补 caller user，再交给 `Context.checkUriPermission()`；已有显式 user或非-content URI原样参与 grant检查。否则也使用当前 URI。在普通直达 Binder 路径中，部署在 user 0 的 singleUser Provider 因而可以对无 user的 content URI按调用用户名下 read grant判定。`openContentUri` 这类带 callerToken 的 AMS代理路径是例外：补 user 的决定早于 token 替换，依据的是表面 Binder UID，后面的权限回调才可能恢复原 pid/uid。

write 版本没有对应补偿：它直接拿业务侧 URI 检查 write grant。由于 singleUser 的 `maybeGetUriWithoutUserId()`会保留嵌入 user，这在常规显式 user URI 上可能仍然正确；但对于没有嵌入 user、只从 Binder UID 才知道调用用户的场景，read 会构造 caller-user URI，write 不会。这是 r48 的实现不对称，不能概括成“singleUser 总会自动补 user”。

grant 还严格区分方向。read grant 不能满足 write，write grant 也不等于算法会同时赋予 read。`callerToken` 只在特定代理路径参与 `Context.checkPermission()` 与 `checkUriPermission()`；它不是可离线保存或跨线程复用的通用 capability。

缺少静态 permission 时，helper 记录 `MODE_ERRORED`并继续寻找 PathPermission或 URI grant；permission 具备、其关联 AppOp 正常返回 ignored/default→ignored 时，也可继续到具体 grant。grant 成功即可结束 inner 层，这是有意保留的具体授权逃生路径。若关联 AppOp 返回 errored，`noteProxyOp()`会当场抛 `SecurityException`，没有机会继续 grant。Provider 自己配置的外层 `mReadOp/mWriteOp` 在 inner 成功之后执行，因此 grant 和 same-app 快路都不能绕开它；外层 errored 同样直接抛，只有正常返回的 ignored 才交给接口翻译。

## 7. AppOps 有两处 note：permission 关联 op 属于分支，setAppOps 属于最终外层

`checkPermissionAndAppOp()` 先调用 `Context.checkPermission(permission,pid,uid,callerToken)`；权限不具备时返回 `MODE_ERRORED`。权限具备时再用 `permissionToOpCode()` 找到关联 AppOp，并通过 `noteProxyOp()`以 calling package、Binder calling UID 与 attribution tag 记账。若权限没有关联 op，则 `OP_NONE` 直接视为允许。

Provider 还可调用 `setAppOps(readOp,writeOp)`配置独立读写 op。`enforceReadPermission()` 只有在 inner 层允许后才 note `mReadOp`，write 同理。这导致一次成功路径可能 note 两次：一次源于 Manifest permission 对应的 op，一次源于 Provider 显式配置的 op；它们代表不同策略层，不能在审计日志里见到两条就判断重复调用。

内部 helper 把 `MODE_DEFAULT` 转成 `MODE_IGNORED`。但拒绝如何呈现取决于 AppOps API 是否正常返回：`AppOpsManager.noteProxyOp()`若得到 `MODE_ERRORED`会当场抛 `SecurityException`，Transport 收不到这个 mode；正常返回的非 allowed 结果在 r48 主要是 `MODE_IGNORED`，才会进入具体接口的软拒绝分支。permission/path 分支中的普通缺权限同样走硬异常。因此不能把“AppOps 拒绝”写成统一的空结果，也不能假设所有 mode 都能返回给 Transport。

测试入口 `attachInfoForTesting()` 会设置 `mNoPerms`，使 `setAppOps()`不写 Transport 的读写 op；这是直接构造 Provider 的测试语境，不应推广成生产 Provider 可以选择忽略 AppOps。正式 `attachInfo()`还把 Manifest permission、PathPermission、exported、singleUser、authority 和 AppOpsManager 一次装入运行态。

### 练习 4：分开 permission-associated op 与 Provider-configured op

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (getContext().checkPermission(permission, Binder.getCallingPid(), Binder.getCallingUid(),' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'AppOpsManager.permissionToOpCode(permission));' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final int mode = enforceReadPermissionInner(uri, callingPkg, attributionTag,' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return noteProxyOp(callingPkg, attributionTag, mReadOp);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return noteProxyOp(callingPkg, attributionTag, mWriteOp);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'int mode = mAppOpsManager.noteProxyOp(op, callingPkg, Binder.getCallingUid(),' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mode == MODE_DEFAULT ? MODE_IGNORED : mode;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (mode == MODE_ERRORED) {' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'throw new SecurityException("Proxy package " + mContext.getOpPackageName()' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'public final void setAppOps(int readOp, int writeOp) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (!mNoPerms) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'mTransport.mReadOp = readOp;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'mTransport.mWriteOp = writeOp;' frameworks/base/core/java/android/content/ContentProvider.java
```

为四种路径列出 note 次数：same-app 且无自定义 op、顶层 permission 关联 op、URI grant 绕过被忽略的关联 op、顶层 permission 加自定义 op。再分别让 AppOps 返回 ignored 与 errored：先标出 errored 在 `noteProxyOp()`内部抛出，再按 query、update、openFile 写出只有 ignored 能到达的接口翻译结果。

## 8. CallingPackage 与 Attribution：Pair 是线程动态范围，package 校验是惰性的

Transport 不把 calling package 作为 Provider 永久字段。进入绝大多数业务方法前，它调用 `setCallingPackage(new Pair<>(callingPkg, attributionTag))`，保存线程原值；业务 `try/finally` 再恢复原 Pair。这让同一 Provider 实例在同一线程被同步重入时能暂存外层身份，也让并发 Binder 线程互不覆盖。`onCallingPackageChanged()`在正常进入、退出和 `clearCallingIdentity()`时收到回调，Provider 可以据此清理按 caller 缓存。不同 Provider 实例各有自己的 ThreadLocal，B 的设置不会覆盖 A 的槽位。

不过入口的 `setCallingPackage()`位于业务 `try`之前，而且它先写 ThreadLocal、再调用可覆写的 `onCallingPackageChanged()`。若回调抛异常，恢复用的 finally 尚未建立，Pair 会残留；`clearCallingIdentity()`也可能在 Binder identity 已清除后因同一回调抛出而无法把 CallingIdentity token 返回给调用者。覆写这个回调时必须保证不抛异常，并把它限制为轻量的缓存失效动作。

`getCallingPackage()`读取 Pair 后，才调用 `AppOpsManager.checkPackage(Binder.getCallingUid(), pkg.first)`验证包名属于当前 Binder UID。因此校验是惰性的：Provider 不调用 verified getter，就不能声称仅因 Transport 存了字符串便已验证。即使验证通过，共享 UID 下也只能证明该包属于这个 UID，不能证明究竟是哪一个 sibling package 发起 Binder 调用。`getCallingPackageUnchecked()`明确不验证；`getCallingAttributionTag()`也直接返回 Pair 的第二项，package check不会顺带验证 tag。r48 的 AppOpsService 遇到未在 Manifest 声明的 attribution tag只记录错误日志，尚不强制拒绝。若业务使用 attribution，应先验证 calling package，并仍把 tag 仅当作归因标签。

身份清理必须同时处理两套状态。只调用 `Binder.clearCallingIdentity()`会把 Binder caller 改成宿主进程，却留下 ThreadLocal Pair：unchecked package 与 attribution 仍可读到旧值；verified getter 通常会拿宿主 UID 校验旧包名并抛 `SecurityException`，而不是可靠地返回旧 caller。`ContentProvider.clearCallingIdentity()`同时清 Binder 与 Pair，返回组合 token。

r48 的恢复还有一个细节：`clearCallingIdentity()`通过 `setCallingPackage(null)`触发回调；`restoreCallingIdentity()`却直接 `mCallingPackage.set(identity.callingPackage)`，没有调用 `onCallingPackageChanged()`。Provider 若依赖回调维护安全缓存，不能假设 clear/restore 对称通知。

ThreadLocal 和 Binder identity 都只在当前线程的动态调用范围有效。把 Runnable 投递到线程池后，新线程不会继承这份 caller；正常进入业务 `try`且外层 finally 完成后，原线程 Pair才恢复。Provider clear 后若漏 restore，只影响本次调用剩余动态范围；正常 Transport 外层 finally仍会恢复 Pair，不能把它写成必然污染未来顶层 Binder transaction。需要审计记录时，应在入口同步复制经过验证的 UID/package等最小不可变信息，把 tag单独视作归因标签，不能把 getter 或 CallingIdentity token 留给异步任务继续推断原调用者。

### 练习 5：验证惰性 package 校验、嵌套恢复与 clear/restore 不对称

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mCallingPackage = new ThreadLocal<>();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final Pair<String, String> original = mCallingPackage.get();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'mCallingPackage.set(callingPackage);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'onCallingPackageChanged();' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'mTransport.mAppOpsManager.checkPackage(Binder.getCallingUid(), pkg.first);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'public final @Nullable String getCallingPackageUnchecked() {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return pkg.second;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'Slog.e(TAG, "attributionTag " + attributionTag + " not declared in manifest of "' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'return new CallingIdentity(Binder.clearCallingIdentity(), setCallingPackage(null));' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'Binder.restoreCallingIdentity(identity.binderToken);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'mCallingPackage.set(identity.callingPackage);' frameworks/base/core/java/android/content/ContentProvider.java
```

先画出同一 Provider 实例的 A 请求同步重入自身、内层返回、A clear、A restore、A 返回的 Pair 与 Binder UID 时间线；再画一张 A Provider 调用另一个 B Provider 的双 ThreadLocal 图。分别在 clear 后调用 verified、unchecked 与 attribution getter，说明三者为何不能写成同一个结果；最后让回调在入口抛异常，标出为什么恢复 finally 尚未生效。

## 9. 各接口拒绝矩阵：硬失败与软失败必须分开观察

所有接口都可能在 URI/user/authority 预检时抛 `SecurityException`。读写 inner 层没有可用 permission/grant 时也通常硬抛。下表的“软拒绝”只描述权限函数正常返回非 `MODE_ALLOWED` 的情况，最常见来源是 AppOps；它不是另一次授权成功，也不代表所有业务代码都绝对没有运行。

| 接口 | 通用门 | 软拒绝对调用者的结果 | 是否进入业务 | cancellation 参数 |
|---|---|---|---|---|
| `query` | read | Transport 看到非 null projection 时返回 0 行 MatrixCursor；看到 null 时先执行真实 query，业务返回 null则返回 null，否则按列名构造 0 行 Cursor | 后一种会调用 query | 有 |
| `insert` | write | 调用 `rejectInsert()`，默认返回 path 追加 `0` 的 URI | 进入拒绝钩子 | 无 |
| `bulkInsert` | write | `0` | 否 | 无 |
| `update` / `delete` | write | `0` | 否 | 无 |
| `openFile` / `openAssetFile` | mode 决定 read 或 write | `FileNotFoundException("App op not allowed")` | 否 | 有 |
| `openTypedAssetFile` | 固定 read | 同上 | 否 | 有 |
| `canonicalize` / `uncanonicalize` | read | `null` | 否 | 无 |
| `refresh` | read | `false` | 否 | 有 |
| `applyBatch` | 逐项分类 | 预检抛 `OperationApplicationException` | 整批不进入业务 | 无 |
| `call` / `getType` / `getStreamTypes` | 无通用 read/write 门 | 没有这一软拒绝分支 | 直接按各自契约进入 | 无 |

“有 cancellation 参数”只表示协议能携带 signal，不证明基类或底层 I/O 会响应；客户端通常先调用 Transport 的 `createCancellationSignal()`工厂，再把所得 Binder随请求送回同一 Provider。具体失效与解绑边界见第 10—12 节。

query 的“非 null”还要以 Transport 服务端实际收到的值为准。跨进程 Proxy 把 null 与长度为 0 的 projection 都编码成整数 0，服务端 `onTransact()`只在长度大于 0 时重建数组；所以客户端传空数组时，远端 Transport 看到的仍是 null，会执行真实 query取列名并继承其资源边界。本地直接调用 Transport 才能保留一个非 null 的零长度数组。

`insert` 的拒绝钩子尤其容易误判。Transport 已经完成 URI 校验和普通 Provider 的 user 前缀剥离，随后设置 caller Pair 并调用 `rejectInsert(uri, values)`；它直接返回钩子结果，不执行 `maybeAddUserId()`。只有允许分支的真实 insert 结果才补回输入 userId。因此显式跨 user URI 的 dummy/自定义拒绝 URI 不保证保留原 user 前缀。

`bulkInsert` 默认实现逐项调用 Provider 的业务 `insert()`并最终返回输入数组长度；它不检查每个返回 URI，也不自动开事务。若 Provider或数据层没有在外部事务中包围这些单项调用，中途异常时此前副作用不会由这段基类循环回滚。这里说的是已经通过 Transport 门后的 Provider 默认实现，不能与 Transport 软拒绝直接返回 0 混成同一层。

### 练习 6：从源码生成硬拒绝、软拒绝与业务执行表

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return new MatrixCursor(projection, 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return new MatrixCursor(cursor.getColumnNames(), 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return rejectInsert(uri, initialValues);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return maybeAddUserId(mInterface.insert(uri, initialValues, extras), userId);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mInterface.bulkInsert(uri, initialValues);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mInterface.delete(uri, extras);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mInterface.update(uri, values, extras);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'throw new FileNotFoundException("App op not allowed");' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'throw new OperationApplicationException("App op not allowed", 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'insert(uri, values[i]);' frameworks/base/core/java/android/content/ContentProvider.java
```

为每个返回值写一个不会误报成功的调用端判断：空 Cursor、0、null、false、dummy URI 与 FileNotFoundException 分别意味着什么。然后加入一条 Provider 日志，验证 projection-null query 和 rejectInsert 为什么能在“拒绝”时仍产生业务侧可见行为。

## 10. Query 的特殊完成点：空结果可能先执行真实查询，跨进程 Cursor 才进入 BulkCursor 生命周期

正常 query 在读门允许后设置 caller Pair，把 Transport cancellation Binder 转成 `CancellationSignal`，调用业务 query，并把 Cursor 返回给 `ContentProviderNative`。跨进程时，native 层创建 `CursorToBulkCursorAdaptor`，读取 descriptor 并把 Cursor 所有权交给 adaptor；客户端 Proxy 创建 `BulkCursorToCursorAdaptor`。成功后的 close 通过远端 BulkCursor 协议结束；observer 的 death recipient 注册成功后，客户端后续死亡也会触发服务端关闭。若注册当时对端已死，构造器吞掉 `RemoteException`且不会立刻 dispose，不能把死亡关闭写成无条件保证。

业务 `query()`返回还不是初始 Binder事务完成点。`getBulkCursorDescriptor()`会在 reply 写出前同步读取列名、`wantsAllOnMoveCalls`、总 count 和初始 CursorWindow；惰性 Cursor可能在 `getCount()`或取首窗时才真正执行昂贵查询。客户端初始化后，`getCount()`只读 descriptor 缓存；移动超出当前窗口才通过 `mBulkCursor.getWindow()`再次跨 Binder。诊断首包慢与后续滚动慢时，必须把这两个阶段分开。

身份动态范围却更早结束：Transport 在业务 `query()`返回时已于 finally恢复 caller Pair，之后 native层才构造 adaptor并调用 Cursor 的列名、count和window，后续窗口更是直接进入 BulkCursor adaptor。普通顶层调用中，这些惰性 Cursor方法无法再从 Provider的 ThreadLocal取得原 calling package/tag。对象级授权必须在 query动态范围内完成并固化到返回 Cursor的查询计划，不能延迟到 `getCount()`或 `fillWindow()`再调用身份 getter。

服务端构造 descriptor 失败时，native 层的 finally 会关闭已经创建的 adaptor或尚未移交的原 Cursor。客户端若在 transact、反序列化或初始化阶段异常，Proxy也调用本地 adaptor.close；但只有 descriptor 已初始化、`mBulkCursor`非 null 时，这个 close 才能向服务端发送远端 close。若客户端在 initialize前失败，本地空 adaptor关闭不能证明服务端 Cursor 已释放，仍要依赖已注册 observer 的死亡通知或进程结束等收束点。

AppOps 软拒绝时存在两个分支。Transport 实际收到非 null projection，便直接用投影列名返回 0 行 MatrixCursor，完全不调用业务。收到 null 时，调用者可能按列下标读取，框架不知道列集合；Transport 因而仍设置 caller Pair并执行真实 query。业务返回 null则 Transport直接返回 null；业务返回非 null Cursor才读取 `getColumnNames()`并返回另一个 0 行 MatrixCursor。跨进程的零长度 projection因编码塌缩也进入后一支。r48 没有关闭这个非 null 原业务 Cursor，也没有把它交给 native adaptor。它不在正常远端 close 链里，既可能泄漏资源，也可能让本应“软拒绝”的请求产生查询副作用。

Cancellation 也不是硬中断保证。正常路径由客户端向当前 Provider调用 `createCancellationSignal()`，该 Binder回到创建它的同一进程时才还原为本地 Transport；`CancellationSignal.fromTransport()`只在参数确为本进程私有 `Transport`实例时返回 signal，否则返回 null。ContentResolver 在 unstable Provider发生 DeadObject后会取得新的 stable Provider，却把旧进程创建的 remote signal原样传入重试；新 Provider看到的不是自己的本地 Transport，业务侧因而得到 null，重试段失去合作取消。即便 signal有效，ContentProvider 默认重载仍可回落到忽略 signal 的旧 query；实现只有主动检查 signal或让底层查询响应取消，调用端 cancel 才能及时停止。返回后的窗口读取和 Cursor 生命周期也不是这次方法级 signal 自动覆盖的阶段。

### 练习 7：追踪正常 Cursor、构造失败与 projection-null 拒绝的三条关闭链

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'CancellationSignal.fromTransport(cancellationSignal));' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'cursor = mInterface.query(' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return new MatrixCursor(cursor.getColumnNames(), 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (cursor == null) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'int length = 0;' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'if (num > 0) {' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'if (transport instanceof Transport) {' frameworks/base/core/java/android/os/CancellationSignal.java
grep -n -F 'return ((Transport)transport).mCancellationSignal;' frameworks/base/core/java/android/os/CancellationSignal.java
grep -n -F 'qCursor = stableProvider.query(mPackageName, mAttributionTag, uri, projection,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'adaptor = new CursorToBulkCursorAdaptor(cursor, observer,' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'BulkCursorDescriptor d = adaptor.getBulkCursorDescriptor();' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'd.columnNames = mCursor.getColumnNames();' frameworks/base/core/java/android/database/CursorToBulkCursorAdaptor.java
grep -n -F 'd.count = mCursor.getCount();' frameworks/base/core/java/android/database/CursorToBulkCursorAdaptor.java
grep -n -F 'd.window = mCursor.getWindow();' frameworks/base/core/java/android/database/CursorToBulkCursorAdaptor.java
grep -n -F 'return mCount;' frameworks/base/core/java/android/database/BulkCursorToCursorAdaptor.java
grep -n -F 'setWindow(mBulkCursor.getWindow(newPosition));' frameworks/base/core/java/android/database/BulkCursorToCursorAdaptor.java
grep -n -F 'mBulkCursor = d.cursor;' frameworks/base/core/java/android/database/BulkCursorToCursorAdaptor.java
grep -n -F 'if (mBulkCursor != null) {' frameworks/base/core/java/android/database/BulkCursorToCursorAdaptor.java
grep -n -F 'remoteObserver.asBinder().linkToDeath(recipient, 0);' frameworks/base/core/java/android/database/CursorToBulkCursorAdaptor.java
grep -n -F 'if (adaptor != null) {' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'if (cursor != null) {' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'adaptor.initialize(d);' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'adaptor.close();' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'return query(uri, projection, selection, selectionArgs, sortOrder);' frameworks/base/core/java/android/content/ContentProvider.java
```

分别让业务 query 返回正常 SQLiteCursor、返回 null、在 descriptor 构造时失败、在 projection-null 软拒绝时返回 Cursor。标出每条路径谁拥有 Cursor、谁调用 close、哪个分支在 r48 没有所有者接管。

## 11. 文件接口：mode 只选一扇门，callerToken 支撑 openContentUri 身份代理，取消仍取决于实现

`openFile()`和 `openAssetFile()`用一个简单规则选权限：mode 字符串只要包含 `w` 就检查 write，否则检查 read。`rw`不会同时要求 read 与 write；Provider 若认为“可写不等于可读”，必须在业务层核对模式和自身策略。typed open 固定按 read 检查。静态硬拒绝仍是 `SecurityException`，outer AppOp 软拒绝被包装成 `FileNotFoundException`，故“文件不存在”日志可能实际是隐私降级。

Transport 把 cancellation Binder 交给 `fromTransport()`后再传业务的带 signal 重载，这同样受上一节“必须是当前 Provider进程创建的本地 Transport实例”约束。ContentResolver 的文件 DeadObject重试也会把旧 Provider创建的 remote signal原样交给新 stable Provider，重试端业务可能只得到 null。即使 signal有效，基类 `openFile(uri,mode,signal)`、`openAssetFile` 和 `openTypedAssetFile` 默认也都直接调用不带 signal 的旧重载；Provider 未覆写相应入口时，signal 在框架表面存在却不会中断阻塞 I/O。取消只覆盖方法执行阶段，也不会自动撤销返回后对 FD 的读写。

`ActivityManagerService.openContentUri()`展示了 `callerToken` 的用途。AMS 用 external handle 取得 Provider，保存原 Binder pid/uid，再创建一个新 Binder token放进 system_server 的静态 ThreadLocal；随后以 `callingPkg=null`、AMS Binder 身份调用 Provider 的 `openFile(...,token)`。Provider 权限检查回到 AMS 的 `checkPermissionWithToken()`或 `checkUriPermission()`时，只有 token 与同一线程保存对象精确相等，才把 pid/uid替换回最初调用者。finally 删除 ThreadLocal并释放 external handle。

所以 token 不是调用者可长期持有的授权票据：对象、线程与动态范围共同约束它。它只在 AMS 的 `checkPermissionWithToken()`与 `checkUriPermission()`精确命中时替换 pid/uid；不会改写 Transport 先前取得、用于 sameApp、checkUser和错误文案的 Binder身份，不会改写 `noteProxyOp()`读取的 Binder UID，也不会补 calling package、attribution tag或 URI userId。singleUser read 是否给 URI补 caller user，同样按表面的 Binder UID决定。对 `openContentUri` 而言，outer AppOp仍按 `callingPkg=null`和 system_server Binder UID求值，AppOpsManager把 null package记到 `android`；token并未跳过 Manifest/URI-grant门，却也没有恢复原应用的 AppOps身份，不能据此声称原 caller 的 op 已被检查。

文件描述符不经过 BulkCursor。远程 open 成功回复时，native 层以 `PARCELABLE_WRITE_RETURN_VALUE`写 PFD/AFD；AFD把 flags原样传给内部 PFD，PFD 在副本写入 Parcel 后关闭服务端 wrapper，客户端持有反序列化得到的句柄。这个自动关闭结论以 copy 写入成功为边界，不能扩大成异常写包也一定完成；本地直接 Transport 调用没有 Parcel步骤，返回的仍是同一 Java descriptor，由调用方按本地契约关闭。

### 练习 8：验证 mode、默认取消回退与 callerToken 代理身份

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (mode != null && mode.indexOf('w') != -1) {" frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'throw new FileNotFoundException("App op not allowed");' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return openFile(uri, mode);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return openAssetFile(uri, mode);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return openTypedAssetFile(uri, mimeTypeFilter, opts);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'cancellationSignal.setRemote(remoteCancellationSignal);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'return provider.refresh(mPackageName, mAttributionTag, url, extras,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'Binder token = new Binder();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'sCallerIdentity.set(new Identity(' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'pfd = cph.provider.openFile(null, null, uri, "r", null, token);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'if (tlsIdentity != null && tlsIdentity.token == callerToken) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'sCallerIdentity.remove();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'noted for the "android" package' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'fd.writeToParcel(reply,' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'Parcelable.PARCELABLE_WRITE_RETURN_VALUE);' frameworks/base/core/java/android/content/ContentProviderNative.java
grep -n -F 'mFd.writeToParcel(out, flags);' frameworks/base/core/java/android/content/res/AssetFileDescriptor.java
grep -n -F 'if ((flags & PARCELABLE_WRITE_RETURN_VALUE) != 0 && !mClosed) {' frameworks/base/core/java/android/os/ParcelFileDescriptor.java
grep -n -F 'closeWithStatus(Status.SILENCE, null);' frameworks/base/core/java/android/os/ParcelFileDescriptor.java
```

推演普通 `r`、`w`、`rw`、typed open 与 AMS `openContentUri` 五种请求。为每种标出检查方向、Binder calling UID、token 是否非空、软拒绝异常和 signal 最终是否到达 Provider 自己覆写的可取消实现；再比较 query/open 正常解绑与 refresh返回后保留 remote signal 的差异。

## 12. 通用门之外的接口：call 自行鉴权，type 不提供 caller Pair，动态 URI 钩子应默认最小授权

`call()`只验证外层 authority，把 extras 设为 defusable，设置 caller Pair后直接调用 Provider。框架不知道 method 是读、写还是管理操作，所以没有通用 read/write、PathPermission、URI grant 或 Provider read/write AppOp 门。Provider 必须按 method 白名单校验 calling UID/package、所需 permission、AppOp、目标 user 和 extras 中的资源标识。`Bundle.setDefusable()`只降低恶意 Parcelable 反序列化把进程直接击穿的风险，不限制 Bundle 大小、键、类型或业务权限。

`getType()`与 `getStreamTypes()`验证 URI并做 user 处理，但不走 read permission，也不设置 caller Pair。对普通顶层事务，业务看到的 calling package/tag应为 null；这是为了允许 MIME 探测，不是读取数据的授权。实现代码没有在进入这两个方法前主动清空 ThreadLocal：若 Provider 正在同一线程处理另一个请求并同步重入自己的 Transport type 方法，可能仍看到外层 Pair。该实现锐角不能作为可靠身份来源，业务应遵循接口契约，不在 type 方法依赖 caller Pair。

ContentResolver 对 type 的 3 秒/远端总 23 秒异步等待是特定客户端包装，不是所有 Transport CRUD 的统一执行超时；canonicalize 也有 3 秒包装。Transport 本身的同步 query、insert、open 等没有同一把总时钟，阻塞与 ANR 仍要结合上一章的 Provider 引用和 ANR 协作链分析。

refresh 的 cancellation 生命周期还有一个客户端不对称。ContentResolver 创建 remote signal并绑定到调用方 CancellationSignal，Transport 再把它传给业务；但 r48 的 `refresh()` finally只释放 Provider，没有像 query和文件路径那样执行 `setRemote(null)`。所以 refresh 已返回后，客户端 signal仍可能指向旧 transport，之后 cancel还会送到一个已经结束的服务端 signal。这个迟到调用不证明 refresh仍在执行；而 ContentProvider基类本就直接返回 false并忽略 signal，真正的合作取消仍取决于覆写实现。

Transport 的 `checkUriPermission(uri,uid,modeFlags)`同样不经过通用 read/write 门，而是设置 caller Pair后把“被检查的目标 uid”交给 Provider 动态钩子。系统真实使用链由 URI grants 服务进入 AMS，AMS external-acquire Provider、清 Binder identity，再以 `callingPkg=null`调用此钩子。Transport 自身没有强制只有 system_server 才能调用，也没有验证传入 uid 必须等于 Binder caller；已经持有 Binder 的主体可直接发起请求。因此 Provider 覆写该钩子时应默认拒绝、只返回最小必要能力，不能把传入 uid 或 callingPkg 当成可信调用权限证明。

## 13. 跨用户返回值：正常 insert/canonical 尝试回补 user，拒绝 insert 与 refresh 却不遵循同一规则

普通 query、update、delete、open 等通常经 `maybeGetUriWithoutUserId()`把 user 前缀从业务 URI 去掉，返回类型本身也无需重新嵌入 user。会返回 URI 的接口必须显式处理这一维度：正常 insert 在调用前保存 `getUserIdFromUri()`，业务成功后调用 `maybeAddUserId()`尝试回补；canonicalize 与 uncanonicalize 也保存 user，再无条件剥离后对结果尝试回补。helper 只处理非 null、scheme 为 content且尚无 userId 的 URI；Provider 返回非-content URI或自带 userId时，结果原样保留。

三处不对称需要单独记：

1. insert 的软拒绝直接返回 `rejectInsert()`结果，不补回原 user；
2. canonicalize/uncanonicalize 使用无条件 `getUriWithoutUserId()`，即使 singleUser 也把业务输入的 user 前缀去掉，随后只在返回 URI 上恢复；
3. refresh 也无条件去掉 user，却没有 URI 返回值可回补，singleUser Provider 业务侧看不到嵌入 user。

applyBatch 先为每个 operation 记录原 userId，必要时构造替换 URI 的新 operation，并在 Provider 返回后用 `ContentProviderResult(result,userId)`只给 result.uri 补 user；count、extras、exception 原样保留。若 Provider 违反契约返回比 operation 更多的结果，回补循环会索引越界；返回更短数组则只处理已有项。安全实现应维持一项 operation 对应一项 result，而不是依赖 Transport修正异常长度。

这些规则说明 userId 不是一种统一、透明的 URI 装饰。排查跨用户差异时必须记录“输入 URI 的嵌入 user”“Binder calling user”“Provider Context user”“业务实际 URI”“返回 URI”五列；只看日志中某一个 URI 很容易把框架剥离或回补误认为 Provider 自己改写。

## 14. ApplyBatch：外层与逐项双校验、TYPE_CALL 空洞、原地改写和非事务默认实现

Transport 先验证 applyBatch 的 outer authority，再遍历所有 operation。每项先保存 userId、验证自己的 URI、按接口规则剥离 user；若 URI 变化，创建新的 `ContentProviderOperation`并写回传入的 ArrayList。两次验证不比较“每项 authority 必须等于 outer authority”：同一 Provider 声明 A、B 两个别名时，outer=A、operation=B仍可通过。原始 outer authority也不被规范化，最后原样传给 `mInterface.applyBatch()`。跨进程调用时 operation列表来自 Parcel副本，客户端对象不受影响；本地直接调用 Transport 时可能修改调用方同一个列表，而且后续项预检失败时，前半段已经被替换。

权限分类由 operation 自己回答：insert/update/delete 是 write，assert query 是 read，TYPE_CALL 两者都不是。因此 batch 中的 call 不经过通用读写预检，仍必须由 `ContentProvider.call()`按 method 自行鉴权。任何被分类项得到软 AppOps 拒绝，Transport 在进入 Provider 前抛 `OperationApplicationException("App op not allowed",0)`；整批业务尚未开始，但列表改写可能已经发生。

全部预检通过后，Transport 只设置一次 caller Pair并调用 `mInterface.applyBatch(authority,operations)`。基类默认实现按顺序执行 `operation.apply(provider,results,i)`，没有 beginTransaction、rollback 或自动 yield。若 Provider或数据层没有额外事务/补偿，中途异常时先前 insert/update/delete 的副作用会保留；expected count 是在写操作完成后比较，因而“计数不符”也可能发生在数据已改变之后，但外层事务仍可能回滚它。

back reference 从前序 `ContentProviderResult`解析 URI id、count 或 extras。`exceptionAllowed`会把单项异常包装成 result.exception并继续后续操作，这扩大了“部分完成”的合法集合；`yieldAllowed`字段不会被基类简单循环消费。

TYPE_CALL 还有一个 null 边界：普通 `ContentProvider.call()`契约允许返回 null，基类默认也返回 null；operation执行却把结果交给要求 Bundle 非 null 的 `ContentProviderResult(Bundle)`构造器。因此 batch call 返回 null 会触发 `NullPointerException`。没有 `exceptionAllowed`时它终止默认循环，基类不会替此前操作回滚；启用后则被包装进 result.exception，后续项继续。Provider 若承诺原子批处理，必须覆写 applyBatch，在自己的存储事务、锁与失败策略内执行，并明确 call、null、异常允许和通知发布的时点。

默认 bulkInsert 同样只是循环 `insert()`后返回数组长度，无事务、无回滚，也不根据每个 insert 返回 null 修正计数。批量 API 的名字不构成原子性证明。

### 练习 9：验证 batch 预检、TYPE_CALL、回补、异常允许与默认顺序执行

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'validateIncomingAuthority(authority);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'final int[] userIds = new int[numOperations];' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'operation = new ContentProviderOperation(operation, uri);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'operations.set(i, operation);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (operation.isReadOperation()) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (operation.isWriteOperation()) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'results[i] = new ContentProviderResult(results[i], userIds[i]);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (userId != UserHandle.USER_CURRENT' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F '&& ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (!uriHasUserId(uri)) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mType == TYPE_DELETE || mType == TYPE_INSERT || mType == TYPE_UPDATE;' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'return mType == TYPE_ASSERT;' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'if (mType == TYPE_CALL) {' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'return new ContentProviderResult(res);' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'this(null, null, Objects.requireNonNull(extras), null);' frameworks/base/core/java/android/content/ContentProviderResult.java
grep -n -F 'return call(method, arg, extras);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (mExceptionAllowed) {' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'if (mExpectedCount != null && mExpectedCount != numRows) {' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'results[i] = operations.get(i).apply(this, results, i);' frameworks/base/core/java/android/content/ContentProvider.java
```

设计一批“insert → 带 back reference 的 update → call → expected-count失败 → exceptionAllowed delete”。分别在基类默认实现和一个显式数据库事务实现中推演结果数组、数据副作用、异常、回滚与通知；再让第三项预检被 AppOps 软拒绝，区分业务副作用与 operation 列表改写。

## 15. Provider 实现检查单：把身份、权限、参数、资源与并发放在同一张图上

ContentProvider 方法可被多个 Binder 线程并发调用。Transport 的 ThreadLocal 只隔离 caller Pair，不替业务对象、数据库事务、Cursor、缓存或文件句柄加锁。Provider 还可能发起嵌套调用、清身份、回调客户端或把任务投递到别的线程；任何跨动态范围共享的可变状态都要有自己的并发协议。

一个面向 r48 的安全实现至少应逐项回答：

- URI：是否只接受预期 scheme、authority、segment 数量和编码形式，是否拒绝 query 参数扩大路径权限范围；
- 身份：是否在同步入口调用 verified `getCallingPackage()`，是否把 UID/user 与 package/tag 分开记录，是否避免异步任务或惰性 Cursor方法再读取 ThreadLocal；
- 权限：Manifest 顶层和 PathPermission 是否按源码的短路/OR语义配置，same-app/shared-UID是否仍需业务租户隔离；
- grant/AppOps：是否理解具体 URI grant 是 inner 最后机会，而自定义 read/write op 是 outer 最终门；
- 例外接口：call 与 batch TYPE_CALL 是否逐 method 鉴权，getType/getStreamTypes 是否只泄露必要元数据，动态 URI 钩子是否默认拒绝；
- 软拒绝：调用端是否明白空 Cursor、0、null、false、dummy URI 和 FileNotFoundException都不是成功或已授权证明，并按具体接口判断业务是否曾进入；
- 资源：projection-null 拒绝是否可能执行昂贵查询，Provider 的 Cursor/FD由谁关闭，默认 signal 重载是否真的响应取消；
- 批处理：是否需要事务，expected count失败如何回滚，exceptionAllowed 与通知何时对外可见；
- 跨用户：业务实际 URI是否保留 user，返回 URI是否需回补，singleUser read/write grant不对称是否影响策略。

性能诊断也必须服从这些边界。query 卡住可能在权限 AppOp、业务 query、BulkCursor descriptor 的首个 window，或后续窗口读取；open 返回后慢读不是 Transport 方法仍在执行；ContentResolver 的 type 超时不适用于 update。先定位完成点，再谈锁、Binder 饱和、数据库或 ANR，能避免把不同阶段堆成一个“Provider慢”。

## 16. 诊断顺序与下一章：从可观察结果反推到精确分支

遇到 `SecurityException`，先核对规范化后的 URI/user/authority，再检查 same-app、exported/checkUser、顶层 permission、所有匹配 PathPermission 和具体方向 grant；最后区分异常来自 permission-inner、authority/user校验还是业务自己抛出。不要只根据 exception message 省略 AppOps 和 URI grant 的旁路。

遇到 query 成功但永远 0 行，先记录客户端 projection及 Transport实际收到的形状，再查 read permission关联 op与自定义 read op。服务端非 null projection 的软拒绝不会进业务；null或跨进程空数组的软拒绝会进业务取列名，并可能遗留 Cursor。用 Provider 日志和 AppOps 记录共同判断，不能把“业务 query 出现过”直接当成已授权。

遇到 bulk/update/delete 返回 0 或 insert 返回 `/0`等 dummy URI，按写接口矩阵判断是否是软拒绝；insert 返回 null还可能来自 Provider正常实现或自定义 reject hook，需结合 AppOps与日志区分，并检查 userId是否丢失。refresh 的 false与 canonical/query 的 null属于读侧各自契约，不能混进 CRUD写矩阵。遇到文件“不存在”，把 `FileNotFoundException("App op not allowed")`与真实路径不存在分开。遇到 batch 部分写入，先区分 Transport 预检失败、Provider 默认顺序执行失败、expected count事后失败和 exceptionAllowed继续四种完成点。

最后保留四条不会因日志简化而消失的原则：拿到 Binder 不等于具体 URI 获权；AppOps拒绝不等于统一异常；caller Pair不等于已验证身份；Bulk或Batch不等于事务。只有把入口形状、决策顺序、业务动态范围和资源完成点连接起来，才能解释调用者看到的每一种结果。

下一章转向 `ContentResolver.notifyChange()` 与 `ContentService`：观察者怎样按 URI 树注册，self-change、descendant 与 user 过滤怎样共同决定派发，延迟通知与同步调度又在哪些时点完成。
