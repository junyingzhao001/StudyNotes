# 280 Android UriGrantsManagerService：GrantUri、UriPermissionOwner、Intent/ClipData递归授权、prefix、跨用户、撤销与持久恢复链

## 1. 先看结论：URI 能力要经过证明、计划、落账、回收与恢复

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。上一章停在 DocumentsUI 把结果 Intent 交给 ActivityTaskManager、调用 App 可能继续 `takePersistableUriPermission()`；本章向 system_server 内部再走一层，回答一个更窄的问题：一份 `content://` URI 能力怎样从发送者手中转给目标 UID，并在不同生命周期结束时准确收回或恢复？

先把主链压成六个完成点：

1. URI 被规范成“source user + 去 user-info 的 URI + exact/prefix”键；
2. system_server 证明目标是否需要 grant、Provider 是否允许转授、发送者是否有能力转授；
3. Intent 的 data 与 ClipData 被收集成 `NeededUriGrants`，此时通常还没有写 URI grant 表；
4. ActivityRecord、Service StartItem 或 ownerless 路径逐 URI 落账，read/write 才进入 `UriPermission`；
5. ContentProvider 真正处理 query/open/update 时，再以 direct permission、URI grant 与 AppOps 等规则决定本次调用；
6. owner 清理、显式 revoke、包生命周期或 App 的 take/release 改变内存账，只有已 take 的位才进入 `urigrants.xml` 并在 system ready 时恢复。

这六点不是一个事务。预检可以先改变包可见性或触发动态 Provider 检查；落账会逐 URI 进行，Provider 在两阶段之间消失时可只跳过一项；Activity grant 的安装也早于目标进程确认收到回调。因而日志中的“检查通过”“Needed 非空”“结果已排队”“owner 仍在”“XML 有记录”分别只证明一个完成点。

URI grant 仍不是文件所有权。它不复制字节、不改变 Provider 数据库 owner、不保证 document ID 永久存在，也不会替代 Provider 最终方法中的业务校验。最可靠的阅读方式，是始终同时问四个问题：谁在转授、URI 属于哪个 source user、能力授给哪个 target UID、由谁负责回收。

## 2. 服务与索引：四种身份落进 targetUid → GrantUri 两层表

`UriGrantsManagerService.Lifecycle.onStart()` 发布 `uri_grants` Binder 服务并注册 `UriGrantsManagerInternal`。`PHASE_SYSTEM_SERVICES_READY` 只取得 AMS 与 PackageManagerInternal 依赖；真正读取持久文件，是稍后 `ActivityManagerService.systemReady()` 调用 LocalService 的 `onSystemReady()`。公开 App 的 take/release/get 走 Binder 接口，AMS、ATMS、ActiveServices 等 system_server 组件主要走本地接口。

全局表是 `SparseArray<ArrayMap<GrantUri, UriPermission>>`：第一层 key 为 target UID，第二层 key 为 `GrantUri`。一次权限判断先定位目标 UID 的小表，不必扫描所有 App；查询某 Provider 发出的 outgoing grant、Provider 级撤销或包清理才会跨多个 target UID。

“source”在这条链上至少有四种身份，不能混写：

| 身份 | 来源 | 回答的问题 |
|---|---|---|
| `callingUid` | 当前发送、返回或显式转授请求 | 谁必须证明自己有转授能力 |
| `GrantUri.sourceUserId` | content user hint 或 URI 内嵌 user | 到哪个用户解析 authority 与 Provider |
| `ProviderInfo.applicationInfo.uid` / `sourcePkg` | PackageManager 当前解析结果 | 谁真正提供这份内容 |
| `targetPkg` / `targetUid` | 已解析目标组件或显式目标包 | 能力最终记到哪个 UID |

`sourcePkg` 是 Provider 包，不是发送方包。`targetUserId` 则由 target UID 导出。一个 `NeededUriGrants` 可以收集来自多个 source user 的 URI，却只能有一个 target UID。

第二层 key 不含 `targetPkg`。shared UID 下两个包若先后请求同一 `GrantUri`，会复用同一个 `UriPermission`，其中保存的 target package 保持首次创建值；运行时能力本来按 UID 共享，但按包名查询、定向清理与持久 XML 身份可能呈现“首个包名黏住”的边界，不能把表结构说成 package → URI。

`mLock` 标注保护这张表，但它不是全链路大锁。正常grant的直接/动态权限预检刻意在锁外跨AMS、PM或Provider进行；稍后还会看到 r48 的 `grantModes()` 在 find-or-create 的 synchronized 块结束后才修改对象。反过来，开机恢复和写盘路径又会在持有mLock时解析包或执行I/O。因而“Map 有锁”不等于预检、建对象、加mode、包可见性与持久化构成一个统一原子提交，也不能概括成所有跨服务调用都已移到锁外。

## 3. GrantUri：user、规范 URI 与 prefix 共同定义能力键

`GrantUri` 只有三个身份字段：`sourceUserId`、`uri` 和 `prefix`。hash/equals 同时比较三者，所以相同 authority/path 在个人用户与工作资料是两份能力，相同 URI 的 exact 与 prefix 也是两个不同 Map key。prefix 来自授权 flags，不是 URI 字符串自带属性；不能靠末尾斜杠猜测。

`GrantUri.resolve(defaultSourceUserHandle, uri, modeFlags)` 只对 `content:` URI做用户规范化：authority 若含 `10@authority`，内嵌 user 覆盖 default hint，然后保存移除 user-info 后的 URI。非 content scheme 原样保留并使用 default source user，但稍后的 grant 检查会返回“不建立 grant”。因此 resolve 能表示检查输入，不代表这种输入最终可授权。

Intent 路径的 default source user 来自每层 Intent 的 `contentUserHint`；值为 `USER_CURRENT` 时换成 `callingUid` 所属 user。直接的 `ContextImpl` API 则先用 `resolveUserId(uri)` 拆出 user，再把无 user-info URI送进 AMS。调用方若在跨 profile 场景过早丢掉 user-info，就可能把同样的 authority/path 指向错误 Provider 实例。

`NeededUriGrants` 保存一个 target package、target UID、一份 `flags` 和去重后的 `ArraySet<GrantUri>`。这里的 flags 是顶层 `Intent.getFlags()` 全值，Activity flags 等非 grant 位也会被原样保存；落账时 `UriPermission.grantModes()` 才掩码 read/write/persistable，而 prefix 已在构造每个 `GrantUri` 时固化进 key。

### 练习 1：验证身份四轴、user 规范化与两层索引

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private final SparseArray<ArrayMap<GrantUri, UriPermission>>' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mGrantedUriPermissions = new SparseArray<>();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'public final int sourceUserId;' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'public final boolean prefix;' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'this.prefix = (modeFlags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0;' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'return uri.equals(other.uri) && (sourceUserId == other.sourceUserId)' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'ContentProvider.getUserIdFromUri(uri, defaultSourceUserHandle),' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'ContentProvider.getUriWithoutUserId(uri), modeFlags);' frameworks/base/services/core/java/com/android/server/uri/GrantUri.java
grep -n -F 'final ArraySet<GrantUri> uris;' frameworks/base/services/core/java/com/android/server/uri/NeededUriGrants.java
grep -n -F 'this.targetUserId = UserHandle.getUserId(targetUid);' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
```

构造 `content://authority/a`、`content://10@authority/a`，并为前者各建 exact/prefix key；写出四个 hash/equals 组合是否相等。再解释一个 target UID为何能同时持有两个 source user 的同字符串 URI，而它们不能共用 key。

## 4. UriPermission 四本账：mode 缓存与 strength 检查在 r48 并不等价

一项 `UriPermission` 固定 source package、target package/UID 与 GrantUri，内部维护四本 read/write 账：

| 字段 | 含义 | 典型来源 | 是否写 XML |
|---|---|---|---|
| `ownedModeFlags` | 至少一个活跃 owner 支撑的位 | ActivityRecord、Service StartItem、外部 owner | 否 |
| `globalModeFlags` | 没有 owner 归属的位 | `Context.grantUriPermission()` 等 | 否 |
| `persistableModeFlags` | 允许目标 App take 的 offer | 带 PERSISTABLE 的 grant | 否 |
| `persistedModeFlags` | 已被 take 的长期位 | take 或开机恢复 | 是 |

`grantModes()` 先判断 PERSISTABLE，把本次 read/write OR进 offer；随后 owner为空就写 global，否则分别加入 read/write owner；最后更新 `modeFlags`。同一个 owner重复授同一位由 `ArraySet` 去重，没有引用计数。global更简单，只是按位OR，也没有“由几个发送者贡献”的归属账。

最容易误读的是 `modeFlags`。`updateModeFlags()` 明确只计算 `owned | global | persisted`，不含 persistable offer；源码注释也要求权限 enforcement 使用这个缓存。然而 r48 的实际 `checkUriPermissionLocked()` 并不读 `modeFlags`，而是对 exact/prefix 候选调用 `getStrength()`，后者第一优先检查 `persistableModeFlags`。因此不能笼统说“offer绝不参与访问”：当同一 permission 还被其他有效位留在 Map 中时，一个已失去 owned/global、但仍留在 offer 的单独模式，仍可能通过 strength 检查。

纯 offer 通常不会独立存活。`removeUriPermissionIfNeededLocked()` 以 `perm.modeFlags == 0` 为删除条件；常见 Activity owner把同一批read/write全部移除后，记录连同未take offer一起被删。只有各本账按不同位分叉、记录又被另一有效位保活时，上述实现背离才显现。

strength 还要求**同一本账完整覆盖整组请求位**。例如 owned只有READ、global只有WRITE时，`modeFlags` 看起来是READ|WRITE，但 `getStrength(READ|WRITE)` 不会跨两本账拼接，可能返回NONE。反过来，offer覆盖READ时，即使缓存的有效mode只剩WRITE，单独检查READ又可能返回PERSISTABLE。排查必须同时看五个字段，而不是只看汇总mode或strength其中一个。

### 练习 2：手算四本账、汇总 mode 与整组 strength

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'modeFlags = ownedModeFlags | globalModeFlags | persistedModeFlags;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'persistableModeFlags |= modeFlags;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'globalModeFlags |= modeFlags;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'addReadOwner(owner);' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'addWriteOwner(owner);' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'if ((persistableModeFlags & modeFlags) == modeFlags) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F '} else if ((globalModeFlags & modeFlags) == modeFlags) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F '} else if ((ownedModeFlags & modeFlags) == modeFlags) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'if (perm.modeFlags != 0) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'return STRENGTH_NONE;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
```

分别推演“owner提供READ、global提供WRITE”“offer提供READ、global提供WRITE”“offer与persisted都提供READ|WRITE、其余账为0”三项记录；计算 `modeFlags`，再计算READ、WRITE、READ|WRITE三种 `getStrength()`。指出哪两组结果证明汇总访问位与转授强度不能互相代替，并解释为何真实take/restore状态中的persisted位必须伴随offer，而strength没有单列persisted分支。

## 5. UriPermissionOwner：双向集合提供回收钩子，但不会自行过期

每个 `UriPermission` 持有 read owners 与 write owners；每个 `UriPermissionOwner` 又反向持有自己涉及的 read/write permissions。双向索引让 Activity结束时只遍历自身 grant，不必扫描所有target UID；它也让多个owner共享同一 URI 时可以逐个撤贡献：移除一个read owner后集合仍非空，owned READ继续有效，最后一个离开才清位。

owner 是生命周期**钩子**，不是计时器。`ExternalToken` 只是把 Binder token还原成本进程中的 Owner；`newUriPermissionOwner()` 没有为这个 token安装 `linkToDeath`。丢弃代理、客户端进程死亡或 owner对象“无人使用”，都不会自动清账。Activity、ActiveServices、拖放、输入法、通知、剪贴板等各自必须在正确完成点调用 owner removal；某条清理路径漏掉时，UriPermission与Owner的强引用可互相留存。

同owner、同permission、同mode的重复grant也没有计数。第二次 `ArraySet.add()`不增加“二份租约”，一次owner清理就移除该owner的整个mode贡献。这与global账类似但作用域不同：global连owner身份都没有，多个ownerless来源的相同位会直接折叠。

`UriPermissionOwner.removeUriPermission()` 可同时按GrantUri、mode、target package与target user过滤；GrantUri非空时使用完整equals，所以source user、URI与prefix必须全部一致。它只移除owner贡献，不主动清 persisted。每次从 permission一侧与owner反向集合两边同时摘除，随后服务仅在有效mode为零时删除Map项。

r48 还有一个纯诊断bug：`UriPermission.dump()`进入writeOwners分支后却遍历 `mReadOwners`。只有write owner时可能空指针，两类owner都有时会打印错集合；这不改变 enforcement，却会让 dumpsys 证据误导排查。

### 练习 3：验证多 owner、双向 removal 与外部 token 边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private ArraySet<UriPermissionOwner> mReadOwners;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'private ArraySet<UriPermissionOwner> mWriteOwners;' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'if (mReadOwners.add(owner)) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'owner.addReadPermission(this);' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'if (mReadOwners.size() == 0) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'static UriPermissionOwner fromExternalToken(IBinder token) {' frameworks/base/services/core/java/com/android/server/uri/UriPermissionOwner.java
grep -n -F 'if (token instanceof ExternalToken) {' frameworks/base/services/core/java/com/android/server/uri/UriPermissionOwner.java
grep -n -F 'perm.removeReadOwner(this);' frameworks/base/services/core/java/com/android/server/uri/UriPermissionOwner.java
grep -n -F 'mService.removeUriPermissionIfNeeded(perm);' frameworks/base/services/core/java/com/android/server/uri/UriPermissionOwner.java
grep -n -F 'for (UriPermissionOwner owner : mReadOwners) {' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
```

让Activity A与B共享READ，Service StartItem C只持WRITE；依次移除A、C、B，记录两个owner集合、owned flags和Map项。最后只看dump循环，说明为何writeOwners输出不能作为r48授权真相的唯一证据。

## 6. grant 预检先区分“无需新增”与“违反安全边界”

`checkGrantUriPermissionUnlocked()` 的返回值不是boolean。正target UID表示应该建立URI grant；`-1`表示这一项不需或不能建立新记录；安全边界被违反时抛 `SecurityException`。把后两者都翻译成“授权失败”会误判Activity是否仍可启动。

源码顺序先做四个短路：没有READ/WRITE access mode、非content scheme、SYSTEM/ROOT appId替别人直接签名且非两个Settings例外authority、source user下找不到Provider，均返回 `-1`。批量Intent入口还会在扫描URI前按显式target user解析target package；目标UID不存在时整次返回null。

随后检查目标是否已经**直接**持有Provider能力。这里看Provider self、exported、顶层permission、path-permission与动态门，不把目标已有的UriPermission当“直接能力”。basic read/write且目标已直接可访问时，不建UriPermission，但会立即调用 `grantImplicitAccess()`补包可见性；PERSISTABLE或PREFIX属于advanced grant，即使目标可直接访问，也继续走Provider grant-policy并建立细粒度记录。

`forceUriPermissions` 在这里有第一层作用：无条件把 `targetHoldsPermission` 改回false，禁止basic fast path。这个动作不依赖MediaProvider模块版本；真正是否发动态IPC，是后面另一层门。

Provider声明也要分成两组。`exported`、read/write permission与 `<path-permission>` 用于判断某UID是否直接访问；`grantUriPermissions` 与 `<grant-uri-permission>` 解析出的 `uriPermissionPatterns` 决定某路径能否被转授。patterns非空时，服务把grantAllowed重新设为false，只在至少一个pattern命中后打开。因此 `exported=false` 本身不是URI grant绝对否决：caller若已有足够强的UriPermission、Provider又允许grant，仍可能继续转授未导出的Provider URI。

## 7. source 能力：受限探测、grant policy 与完整证明按次序收束

源码在这里把source检查分成两次，次序不能压成一句“先看Provider、再看caller”。basic fast path之后，服务先为 `specialCrossUserGrant` 调一次 `checkHoldingPermissionsInternalUnlocked(..., false)` 受限探测，再判断Provider grant policy；policy放行后，才用 `checkHoldingPermissionsUnlocked()` 加“已有URI grant”完成最终source证明。前一次探测只决定跨用户例外能否放宽policy，不能替代最后一次证明。

完整检查若发现calling user与GrantUri source user不同，直接Provider能力分支先要求 `INTERACT_ACROSS_USERS`；同UID Provider self立即通过，否则Provider须exported，再分别计算请求的read/write。

顶层permission可满足对应一侧。若顶层未保护，默认允许该侧；但匹配的path-permission声明权限且UID没有它时，会取消这份默认开放。这里的path-permission是“直接访问规则”，不是上一节决定“可否转授”的uriPermissionPatterns。

直接能力不足并非立即拒绝。服务会在 `mLock` 下检查calling UID已有的 exact/prefix `UriPermission`；PERSISTABLE请求要求 `STRENGTH_PERSISTABLE`，普通请求最低OWNED。于是跨用户直接检查缺少 `INTERACT_ACROSS_USERS` 后，caller仍可能凭先前得到的URI grant继续转授；接收方本身也不必预先拥有跨用户permission才能收到能力。不能把那项manifest permission写成所有cross-user grant的总闸。

special cross-user只放宽Provider的grant-policy门。它要求target user不同于source user，并让caller以 `considerUidPermissions=false` 做一次internal direct check。false表示不采信顶层/path UID permission，但Provider self fast path仍成立、外部caller仍受exported和静态保护约束、动态force门也仍运行。若正常grantAllowed为false，special只允许basic grant继续，PREFIX/PERSISTABLE仍抛异常；之后caller最终能力检查照常执行。

动态门同时要求常量开关、`pi.forceUriPermissions` 与MediaProvider模块版本兼容判断。Provider/client在不同user时直接fail closed；同user才由AMS取得Provider并调用 `IContentProvider.checkUriPermission()`。动态结果与静态read/write是AND，不会把静态拒绝改成允许。Provider self在更早的UID相等分支已返回，不走回调。

函数名虽围绕MediaProvider版本，条件并不比较 `pi.packageName`；一旦全局版本门为true，任何声明force的Provider都会进入动态分支。r48产品树主要由MediaProvider使用：它按目标UID建立calling identity，WRITE优先走update策略，否则走query策略，复用正常row过滤；恰好一行才允许。没有有效row id的collection URI只在非prefix请求时有专门放行，prefix collection被拒。动态检查可能获取/启动Provider并发Binder，因此预检也不是纯计算。

### 练习 4：按顺序验证 target、Provider、source 与动态门

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!Intent.isAccessUriMode(modeFlags)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!ContentResolver.SCHEME_CONTENT.equals(grantUri.uri.getScheme())) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (pi.forceUriPermissions) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (basicGrant && targetHoldsPermission) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!ArrayUtils.isEmpty(pi.uriPermissionPatterns)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'boolean specialCrossUserGrant = targetUid >= 0' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (UserHandle.getUserId(uid) != grantUri.sourceUserId) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (Thread.holdsLock(mLock)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '&& isDynamicPermissionEnabledInMP()) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mAmInternal.checkContentProviderUriPermission(grantUri.uri,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'packageInfo.getLongVersionCode() >= MIN_DYNAMIC_PERMISSIONS_MP_VERSION;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'isDynamicPermissionEnabledInMP = true;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
```

为同一URI分别设置“target已有basic直接权限”“target已有直接权限但请求PREFIX”“Provider禁止grant但满足special basic”“caller只有owned grant却请求PERSISTABLE”“force Provider跨user动态检查”五组条件，写出返回-1、正UID或SecurityException，并标出包可见性何时已变化。

## 8. Intent/ClipData 遍历：所谓递归有 URI 优先级与外层 flags

LocalService从最外层Intent读取一次 `intent.getFlags()` 作为mode。递归函数先看当前Intent的data，再遍历ClipData items。每个URI都用当前这一层Intent自己的 `contentUserHint` 解析source user；但递归进入item Intent时继续传最外层mode，嵌套Intent自己的grant flags不会扩大或缩小授权位。

ClipData.Item 的分支是 `if (item.uri != null) ... else ...`。因此item只有URI时检查URI，URI为空且Intent非空时才递归；一个同时携带URI与Intent的合法复杂item只检查URI，内嵌Intent在这条实现中被忽略。text、HTML、普通extras也不参与收集。外层data与ClipData则可以同时被扫描。

检查返回正UID的GrantUri加入 `ArraySet`，完整的source user、规范URI与prefix相同才去重。一个URI抛SecurityException会中止整次收集，不返回之前积累的Needed；不过之前某个“target已有basic直接能力”的item可能已经调用 `grantImplicitAccess()`，动态Provider调用也已经发生，所以失败不代表预检副作用回滚。

r48 的target UID局部变量还有一个跨用户锐角。批量入口起初按显式target user解析UID，但每个URI都会把 `checkGrantUriPermissionUnlocked()` 返回值重新赋给该变量。若前一项因https、Provider缺失或target已有basic能力而返回-1，后一项会在单URI helper里按 `callingUid` 的user重新解析target package，而不是最初target user。若Needed尚为空，可能用这个重解析UID新建计划；若Needed已有内容，后项检查可能用另一user的UID，却仍加进原计划。本文只记录代码级不一致，不据此推断可利用性。

另一个source-user锐角出现在显式 `Context.grantUriPermission()`。ContextImpl已剥离URI user-info并把解析的userId传给AMS；AMS也构造了相应GrantUri，却只在错误文本中使用它，随后把无user-info URI放入一个默认contentUserHint的新Intent。UGMS于是回退calling UID的user。普通同用户调用无差异，跨用户URI或跨用户Context不能假设这条r48实现完整保留了ContextImpl传来的source user。

### 练习 5：重放 data、复杂 item、嵌套 hint 与 targetUid 重解析

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final int mode = (intent != null) ? intent.getFlags() : 0;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'Uri data = intent.getData();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'ClipData clip = intent.getClipData();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'int contentUserHint = intent.getContentUserHint();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'GrantUri grantUri = GrantUri.resolve(contentUserHint, data, mode);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'Uri uri = clip.getItemAt(i).getUri();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'Intent clipIntent = clip.getItemAt(i).getIntent();' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'callingUid, targetPkg, clipIntent, mode, needed, targetUserId);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'needed.uris.add(grantUri);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'UserHandle.getUserId(callingUid));' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'ContentProvider.getUriWithoutUserId(uri), modeFlags, resolveUserId(uri));' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'final Intent intent = new Intent();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

构造外层https data、一个同时含URI和Intent的ClipData.Item、再加一个只有嵌套Intent的item；目标在user 10，发送者在user 0。逐步记录每项采用的mode、contentUserHint、targetUid与是否进入ArraySet，并指出哪一个内嵌URI不会被遍历。

## 9. Needed 是延迟落账计划，不是纯只读检查或全有全无事务

`NeededUriGrants` 只冻结target package/UID、顶层完整flags和GrantUri集合，不保存ProviderInfo、source package或检查时的权限快照。它把安全预检与后续落账、owner选择分成两个阶段；Activity启动路径还特意在WM锁外收集，但started Service与显式Context grant仍可在AMS锁内做检查，不能推广成“所有组件拓扑锁都已避开”。两阶段设计同样不承诺第二阶段世界状态完全不变。

`grantUriPermissionUncheckedFromIntent()` 按ArraySet逐项调用unchecked helper。每一项会按GrantUri source user重新解析当前Provider；Provider此刻不存在就warning并跳过该项，后续项继续，没有批次回滚。Provider存在时，当前 `pi.packageName` 作为targetUid+GrantUri find-or-create的source参数：新建项保存它，已存在项的final `sourcePkg` 则继续保留首次创建值；随后写mode并补implicit package visibility。Needed没有携带第一次解析出的sourcePkg，也不会在第二阶段重做grant policy和caller能力检查。

这带来三个完成点。第一阶段SecurityException意味着不会拿到计划，但不回滚visibility/dynamic-check副作用；第二阶段开始后可能只落一部分URI；第三，落账完成也只表示system_server内存能力存在，目标组件尚可能没有执行任何应用代码。

锁边界同样不是一个事务。r48在 `synchronized (mLock)` 内完成find-or-create，退出后才调用 `perm.grantModes(modeFlags, owner)`；包可见性调用更在其后。源码明确禁止持本地锁执行动态权限检查是合理的跨服务避锁，但读者不能由此反推每个permission对象的所有变更都受mLock完整覆盖。这是一处实现审计边界，不等于本文已经证明可复现竞态。

## 10. Activity：新启动、复用与结果都绑定接收 ActivityRecord

ActivityStarter在解析目标Activity后、不持WM全局锁时收集Needed。新Activity路径先完成“是否允许启动”、复用task、加入或重挂ActivityRecord等决策，随后把grant装到 `mStartActivity.getUriPermissionsLocked()`；这一点晚于若干拒绝分支，却早于目标进程确认收到生命周期事务。若后续client启动失败，owner会随服务端ActivityRecord继续存在，直到record真正移出history。

singleTop等复用已有Activity时，`deliverNewIntentLocked()` 先向该ActivityRecord已有owner加grant，再尝试调度 `onNewIntent()`；远程调用失败可把Intent留作稍后投递，grant并不会等待App ack。多批new Intent因而会在同一个record owner中累积。

Activity result同样归**接收方**。ATMS以返回方Binder UID作为callingUid、以 `resultTo` 的package/user作为target做collect；`finishActivityResults()` 先对 `resultTo.getUriPermissionsLocked()` grant，再把Result加入接收record队列。返回方DocumentsUI不是临时grant owner，回调到达后才授权也不存在竞态窗口。

Activity owner的结束点不是Java对象 `onDestroy()`、配置重建回调或进程死亡本身，而是服务端 `ActivityRecord.removeFromHistory()` 调用 `removeUriPermissionsLocked()`。进程死后record若仍在任务历史中，grant仍按target UID存在，重建进程可继续使用；只有record移出history才清owned贡献。若同URI还有其他Activity owner、global或persisted来源，访问仍可继续。

## 11. started Service、ownerless global 与外部 owner 是三种不同寿命

`startService()` 在服务策略检查后收集Needed，先把它保存在 `ServiceRecord.StartItem` 的pending队列；真正把item移到delivered并发送start args前，才按需创建该StartItem自己的owner并落账。`bindService()` 没有这条Needed/StartItem路径，所以不能把“Intent带grant flags”泛化为所有Service交付。

普通非persistent service的进程死亡并准备redelivery时，ActiveServices会先对仍在delivered列表中的StartItem移除owner，再把item放回pending；下次发送重新grant。persistent service走另一条restart分支，不能套用这份清理结论。`stopServiceToken` 丢弃已完成范围时也显式remove。可是r48处理 `START_STICKY`、`START_STICKY_COMPATIBILITY` 与 `START_NOT_STICKY` 返回值时，只调用 `findDeliveredStart(startId, false, true)` 从列表移除item，没有调用该item的 `removeUriPermissionsLocked()`。Owner与UriPermission双向强引用仍可保留该StartItem，因此不能说 `onStartCommand()` 返回后权限一定撤销；这是一个具体的retention边界。`START_REDELIVER_INTENT` 则有意把item留到显式stop或重投递路径。`START_TASK_REMOVED_COMPLETE` 虽也只移除taskRemoved item，但其正常构造明确令 `neededGrants=null`，不能据此当作新的URI-owner保留证据。

`Context.grantUriPermission()` 采用owner=null，read/write进入global账。它在一次Binder调用中先check Needed再立即unchecked落账，不等待组件启动；同一targetUid+GrantUri上的多个ownerless来源没有集合或引用计数，重复授予只OR同一bit，一次符合范围的revoke就可能清掉共享global位。

system_server其他子系统可通过 `newUriPermissionOwner()` 建外部owner token，再由 `grantUriPermissionFromOwner()` 绑定。token必须能还原为真实 `ExternalToken`；fromUid与Binder caller不同时，只有与system_server相同的system UID调用者可代签，这个门比较UID而不是PID或进程对象；target user先过 `handleIncomingUser(...ALLOW_FULL_ONLY)`。但Owner类本身不监听token死亡，拖放等子系统若要随外部token死亡回收，必须自己linkToDeath并显式revoke。

通用broadcast发送链在r48没有Activity/started-Service式的Needed hook。个别系统功能会在发送前显式建立ownerless grant，那是调用点自己负责，不能仅凭broadcast Intent带READ/WRITE flag就推断UGMS自动递归并安装能力。

### 练习 6：比较 ActivityRecord、StartItem、global 与外部 owner

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'intentGrants = supervisor.mService.mUgmInternal.checkGrantUriPermissionFromIntent(' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
grep -n -F 'mStartActivity.getUriPermissionsLocked());' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
grep -n -F 'resultTo.getUriPermissionsLocked());' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
grep -n -F 'removeUriPermissionsLocked();' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
grep -n -F 'NeededUriGrants neededGrants = mAm.mUgmInternal.checkGrantUriPermissionFromIntent(' frameworks/base/services/core/java/com/android/server/am/ActiveServices.java
grep -n -F 'r.pendingStarts.add(new ServiceRecord.StartItem(r, false, r.makeNextStartId(),' frameworks/base/services/core/java/com/android/server/am/ActiveServices.java
grep -n -F 'si.getUriPermissionsLocked());' frameworks/base/services/core/java/com/android/server/am/ActiveServices.java
grep -n -F 'r.findDeliveredStart(startId, false, true);' frameworks/base/services/core/java/com/android/server/am/ActiveServices.java
grep -n -F 'uriPermissions = new UriPermissionOwner(sr.ams.mUgmInternal, this);' frameworks/base/services/core/java/com/android/server/am/ServiceRecord.java
grep -n -F 'mUgmInternal.grantUriPermissionUncheckedFromIntent(needed, null);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'UriPermissionOwner owner = UriPermissionOwner.fromExternalToken(token);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'targetUserId = mAmInternal.handleIncomingUser(Binder.getCallingPid(),' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
```

为同一READ URI安排Activity A、两次START_STICKY、一次Context global grant和一个拖放external owner；分别触发process death、ActivityRecord移出history、Service返回、显式global revoke与external token丢失，写出哪些动作真会调用remove，哪些只改变Java/组件表面生命周期。

## 12. exact/prefix：路径匹配会看 segment，却在 r48 fallback 漏看 source user

运行时 `checkUriPermissionLocked()` 先在target UID小表用完整GrantUri找exact。ContentProvider正常检查只携带READ/WRITE，所以请求key通常是prefix=false；exact失败后再遍历小表，寻找 `perm.uri.prefix`、足够strength且请求URI对permission URI满足 `isPathPrefixMatch()` 的项。

`Uri.isPathPrefixMatch(prefix)` 比较scheme、authority和解码后的原子path segments。`/foo`不会命中`/foobar`，连续斜杠归到segment语义；query与fragment不参与比较，因此同路径不同query/fragment可被同一prefix覆盖。这不是Provider documentId后代算法，DocumentsProvider tree还需要自己的 `enforceTree()/isChildDocument()` 门。

PERSISTABLE决定最低strength从OWNED抬到PERSISTABLE；PREFIX本身不改变最低等级，却改变请求GrantUri key，并要求caller已有prefix候选或直接Provider能力，普通exact grant不能悄悄升级成prefix。take/release又是另一套查找：它们只构造**传入同URI**的exact与prefix两个key，不遍历祖先，所以tree要take返回的tree URI本身。

r48 fallback循环有一个必须保留的代码疑点：exact key包含sourceUserId，但prefix遍历只比较candidate.prefix、URI path-prefix与strength，没有显式比较candidate的sourceUserId。由于GrantUri.resolve已把authority里的user-info移除，个人user与工作profile可留下相同规范URI。相比之下，revoke路径明确比较source user。本文只确认缺失比较及其候选范围，不在没有完整调用环境与补丁史证据时宣称可利用结果。

root UID还有一条独立fast path：`checkUriPermissionLocked()` 对uid 0直接true。它不能与前面“system/root不能直接代签”混为一谈；前者回答root能否访问，后者防止高权限进程以模糊来源把能力授给第三方。

### 练习 7：验证exact、segment prefix、strength与source-user缺口

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final UriPermission exactPerm = perms.get(grantUri);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (exactPerm != null && exactPerm.getStrength(modeFlags) >= minStrength) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (perm.uri.prefix && grantUri.uri.isPathPrefixMatch(perm.uri.uri)' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'final int minStrength = persistable ? UriPermission.STRENGTH_PERSISTABLE' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (uid == 0) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!Objects.equals(getScheme(), prefix.getScheme())) return false;' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'if (!Objects.equals(getAuthority(), prefix.getAuthority())) return false;' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'List<String> seg = getPathSegments();' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'if (!Objects.equals(seg.get(i), prefixSeg.get(i))) {' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'if (perm.uri.sourceUserId == grantUri.sourceUserId' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
```

为user 0与user 10建立同authority/path的prefix候选，再检查user 10后代；只按函数逐行说明哪些字段被比较。另用 `/foo`、`/foobar`、`/foo/a?q=1#x` 验证segment、query与fragment边界，并比较普通访问检查与take时prefix查找为何不同。

## 13. revoke：调用者是否直接持权决定扫描范围，targetPackage 决定是否碰 owner

`Context.revokeUriPermission()` 到AMS后先拒绝null URI、没有READ/WRITE的mode或找不到Provider；UGMS再解析Provider，并用direct Provider规则判断caller是否直接持有请求能力。注意这里不把caller已有UriPermission算作direct。

caller没有direct能力时，只扫描 `mGrantedUriPermissions[callingUid]`，也就是它自己作为target收到的记录；它可清global与persisted/offer，但 `includingOwners=false`，所以不能用这条路径撤Activity/StartItem owner贡献，更不能扫描其他UID。caller有direct能力时则扫描所有target UID，常用于Provider在对象删除后撤自己发出的能力。

两条分支都要求permission的sourceUserId与撤销GrantUri一致，然后执行 `perm.uri.uri.isPathPrefixMatch(grantUri.uri)`。方向是“已授权URI是否位于传入撤销URI之下”，所以撤父路径会覆盖exact与prefix后代项；这份广度与调用时有没有传PREFIX flag无关。query/fragment同样不参与path match。

实际 `revokeModes()` 收到 `modeFlags | PERSISTABLE`，请求read/write对应的global、persistable offer与persisted位会一起清。只有在direct caller扫描全表且 `targetPackage == null` 时 `includingOwners=true`；指定目标包的定向撤销反而保留owner贡献。于是“revoke已返回”不等于所有生命周期来源都消失，必须看调用者direct能力、targetPackage和其他mode。

若persisted位发生变化才安排磁盘写；只清临时位不需要改XML。已经打开的Cursor或文件描述符也不会被这张表主动关闭，revoke主要影响后续权限检查，不能把“撤账完成”写成“目标手里所有I/O瞬间终止”。

## 14. 包与用户清理：persistable开关、target-user过滤与shared UID都会改语义

`removeUriPermissionsForPackageLocked(packageName, userHandle, persistable, targetOnly)` 先按**targetUid所属user**过滤外层Map，再在项内匹配sourcePkg或targetPkg。`targetOnly=true`只看接收包；false也看Provider包。package与user不能同时完全无限定，防止一条误调用抹掉整表。

这个顺序使“清某user中由某包发出或收到的grant”并非完全对称：即使匹配sourcePkg，userHandle仍比较target user。source user X发给target user Y的cross-user outgoing项，在用X作为userHandle清理时不会进入外层分支；反过来，以Y清同包名时可能扫描到来自另一source user的项。调用者若要覆盖所有target users必须显式使用USER_ALL，不能从参数名推断source-user范围。

`persistable=false` 调用 `revokeModes(~PERSISTABLE, true)`：清临时global/owner而保留已take位；true则以全位清除，连persisted一起删并在变化时安排写盘。AMS force-stop和停止user的路径使用false，清数据（未要求keep state）与full uninstall使用true。user stop把packageName传null，因此只会按该target user清临时项；r48的user removal路径没有一条显式UGMS全量持久grant清理调用，不能宣称“删user立即重写所有持久项”。

还有一个不应包装成正常保证的r48审计缝隙：`forceStopPackageLocked(..., doit)` 对瞬态URI grant的清理位于外围若干 `if (doit)` 之外。部分 `doit=false` 探测会因现存service/provider提前返回，但执行若继续走到清理语句，查询式调用也可能发生真实变更；判断force-stop副作用时必须沿具体返回路径核对，不能只看 `doit` 参数名。

Downloads authority在persistable=false时整个permission被跳过，这是源码注明的兼容hack，避免为了重新grant立刻拉起DownloadManager。它不表示下载URI永久不撤；持久清理、Provider revoke等其他路径仍可处理。

Provider进程死亡本身也不调用UGMS清理。URI grant按UID与URI存在，不依附Provider进程实例；后续访问会因Provider不可用失败，进程重启后则可能恢复服务。full uninstall另走package生命周期清理，不能把“Binder provider死了”和“authority归属被移除”当成一个事件。

shared UID再次放大包名边界：同一targetUid+GrantUri只有一项，targetPkg保持首次创建值。另一个shared-UID包能以UID身份实际使用，却可能在incoming按包过滤时看不到，或因首个包被清理而一起失去项。这是身份记账与Linux UID共享模型的交界，不是第二个包得到独立租约。

### 练习 8：比较Provider广域撤销、定向撤销、force-stop与卸载

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final boolean callerHoldsPermissions = checkHoldingPermissionsUnlocked(pi, grantUri,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'final ArrayMap<GrantUri, UriPermission> perms = mGrantedUriPermissions.get(callingUid);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'perm.uri.uri.isPathPrefixMatch(grantUri.uri)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'modeFlags | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'targetPackage == null);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (userHandle == UserHandle.USER_ALL' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '|| userHandle == UserHandle.getUserId(targetUid)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '|| perm.targetPkg.equals(packageName)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '&& !persistable) continue;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '? ~0 : ~Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION, true);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mUgmInternal.removeUriPermissionsForPackage(packageName, userId, false, false);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mUgmInternal.removeUriPermissionsForPackage(ssp, userId,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

建立source user 0→target user 10、同target shared UID两个包、同时含owner/global/persisted三种来源的记录；分别执行定向revoke、targetPackage=null的Provider revoke、user 0 force-stop、user 10 stop与full uninstall，逐项标出扫描范围、includingOwners和是否schedule写盘。

## 15. take/release 与 512：同 URI 双 key、整组覆盖和内存时间要分开

公开take/release只接受READ/WRITE位，isolated caller被拒。ContentResolver在Binder前解析URI embedded user并移除user-info，所以服务收到普通URI与source user两个参数；跨profile调用若擅自删掉user-info，往往会在错误source user的小表里查找。

take只在target UID小表中构造传入**同URI**的exact key与prefix key，不遍历祖先prefix。请求的全部read/write必须由某一个候选的 `persistableModeFlags`完整覆盖；exact只读加prefix只写不能拼成一次READ|WRITE。若exact和prefix各自都完整覆盖，r48会对两项都take，因此同URI可产生两份persisted记录。

`takePersistableModes()` 把offer交集OR进persisted，并在持久位非零时每次更新 `persistedCreateTime`；返回值只表示mode bits是否变化。重复take相同mode会刷新**内存**时间，却不单独触发schedule；它仍会改变本次开机内的prune排序，若system_server在下一次有效写盘前重启，磁盘旧时间又会回来。

512上限按target UID的persisted `UriPermission` 项计数，不按package，也不按去重后的URI字符串；同URI exact/prefix都持久化会算两项。helper先用整个permission map大小做快捷判断，达到512便可能扫描，但真正收集和排序的只有 `persistedModeFlags != 0` 项；只有超过512才释放最旧项。大量临时grant会增加扫描机会，却不占持久配额。这是每次take后的修剪目标，不是XML reader的硬拒绝上限：读取路径不调用prune，旧文件或异常输入可先恢复出超过512项，直到该UID后续一次take才重新修到512。

release也查传入同URI的exact/prefix key，两项存在就分别清请求位；普通caller两项都不存在才抛SecurityException。已有key但对应位从未persisted只是no-op，owner/global仍可继续提供访问；带特权toPackage的内部调用即使没项也不走普通caller那条异常。GET_CONTENT的新结果不提供PERSISTABLE，但若UID以前已有同URI/source user的offer，take仍可能成功，不能只从本次Intent flags推断历史Map。

## 16. urigrants.xml：延迟合并、带锁 I/O 与有条件恢复构成最后完成点

只有persisted位变化或真实prune才调用 `schedulePersistUriGrants()`。若Handler没有同类消息，服务排一个10秒后的IoThread任务；窗口内后续变化合并进同一批且不重置期限。这是leading-edge延迟合并，不是每次变更都重新计时。system_server在窗口内异常终止，最新内存变化可能尚未落盘。

写入只快照 `persistedModeFlags != 0` 的项，XML保存source/target user、source/target package、规范URI、prefix、persisted modes与createdTime；owned、global和未take offer不保存。AtomicFile保证单个文件替换可回滚，却不能把Provider数据库、Activity交付与grant XML变成跨服务事务。

r48锁实现还有一处反直觉：Handler先 `synchronized(mLock)` 再调用 `writeGrantedUriPermissionsLocked()`，所以虽然函数内部注释说先做snapshot以便不持锁持久化，后面的AtomicFile I/O实际上仍处在外层mLock范围；内部又 `synchronized(this)` 取snapshot，但没有释放mLock。慢存储可能拉长grant表操作等待，不能按注释想象成“复制后完全锁外写”。

AMS进入system ready后触发读取。旧格式只有userHandle时同时赋给source/target；缺createdTime用本次读取的now。每项按source user重新解析authority，要求Provider存在且仍属于sourcePkg；targetPkg则用 `MATCH_UNINSTALLED_PACKAGES` 在target user解析UID，找不到才跳过。恢复不会重查Provider当前的exported、grantUriPermissions、path或grant pattern。后来真正访问时，ContentProvider Transport仍会重新组合direct permission、AppOps与URI-grant检查，但不会倒回去审查当初这份grant是否符合现时的可签发策略。

恢复只调用 `initPersistedModes()`，把XML modes同时放进persistable与persisted，owner/global保持空。若原offer是READ|WRITE而重启前只take READ，XML只保存READ，未take的WRITE offer不会跨重启回来。恢复后同一已持久位仍能release或再次take。

缺文件视为正常；IO/XML解析异常只记录，读取过程没有把已插入Map的前半部分回滚。source归属不匹配会warning，target UID解析失败静默跳过；这些无效XML行不会立即触发重写，直到以后一次有效持久状态写盘，新snapshot才自然把它们排除。成功恢复还会补implicit package visibility。

遇到问题可按下面的矩阵缩小范围：

| 症状 | 第一检查点 | 容易忽略的反例 |
|---|---|---|
| Intent已发出却无能力 | 外层READ/WRITE、data/ClipData优先级、Needed是否为空 | item同时有URI与Intent时不递归Intent；返回-1不一定阻止组件 |
| 多URI只部分可用 | 第二阶段Provider重解析、每项是否落账 | unchecked逐项提交，没有批次回滚 |
| Activity进程死后仍可访问 | ActivityRecord是否还在history、其他owner/global/persisted | owner跟record而非进程 |
| Service返回后grant仍在 | StartItem是否仍在delivered、sticky分支是否只remove列表项 | r48某些完成分支未调用owner cleanup |
| revoke后仍能读 | targetPackage是否非null、owner是否保留、FD是否已打开 | 定向revoke不含owner；已有FD不被强关 |
| profile间身份异常 | GrantUri source user、targetUid循环重解析、Context显式grant | prefix fallback未比较source user |
| 重启后grant丢失 | take是否改位、10秒写盘、source归属、target UID解析 | 重复take只改内存时间时不schedule |
| dumpsys owner异常 | 直接核对read/write双向集合 | r48 writeOwners循环读错集合 |

### 练习 9：闭合take、prune、延迟写盘与启动恢复

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'UriPermission exactPerm = findUriPermissionLocked(uid,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'UriPermission prefixPerm = findUriPermissionLocked(uid,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!(exactValid || prefixValid)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'persistedCreateTime = System.currentTimeMillis();' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'private static final int MAX_PERSISTED_URI_GRANTS = 512;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (perm.persistedModeFlags != 0) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'Collections.sort(persisted, new UriPermission.PersistedTimeComparator());' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!mH.hasMessages(PERSIST_URI_GRANTS_MSG)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '10 * DateUtils.SECOND_IN_MILLIS);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mGrantFile.startWrite(startTime);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'sourcePkg.equals(pi.packageName)' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'targetPkg, MATCH_UNINSTALLED_PACKAGES, targetUserId);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'perm.initPersistedModes(modeFlags, createdTime);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mUgmInternal.onSystemReady();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

令同URI exact与prefix各自offer READ|WRITE，依次执行take READ、重复take READ、take WRITE、release READ，并再创建511项触发prune；记录每步的persisted位、内存时间、是否排写消息、项数与XML内容。最后模拟source authority换包和target package无法解析，指出哪些XML项恢复、哪些跳过。

本章真正要记住的不是一串flags，而是五个互不替代的完成点：direct/既有能力证明、Needed计划、owner/global落账、生命周期撤销、persisted文件恢复。下一章进入 Android ContentProvider发布、ProviderMap、ContentProviderRecord/Connection引用计数、stable/unstable client、死亡清理与ANR协作链，继续追URI能力最终依赖的Provider进程怎样被取得与维持。
