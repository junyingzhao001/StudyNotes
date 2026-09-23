# 283 Android ContentResolver.notifyChange 执行链：ContentObserver、ContentService 观察者树、自通知、Descendant、跨用户、延迟通知与同步调度

## 1. 先看结论：一次 notifyChange 不是一条回调，而是三个完成点不同的分支

本文固定在 Android 11 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。上一章追到 Provider 的 CRUD/Bulk 完成点；本章接住 Provider 常在写入后调用的 `ContentResolver.notifyChange()`，沿 `IContentService`、`ContentService.ObserverNode`、`ObserverCollector`、`IContentObserver`、App `ContentObserver`、cache 与 `SyncManager` 一直追到各自真正完成。

先把全链压成九个结论：

1. public 单 URI 重载先从 URI 提取 user、去掉嵌入的 user 前缀；public Collection则按`getUserIdFromUri(uri,mContext.getUserId())`得到的客户端userId聚类，无前缀时使用Context user，再为每组发起一次同步`IContentService.notifyChange()`；
2. system_server 先按 READ 或 WRITE URI grant 处理跨用户资格，再做 authority 级“可能访问”检查；这不是 Provider Transport 的最终行级读写授权，也不能证明数据真的发生过修改；
3. 观察者索引是一棵 `authority + decoded path segments` 树，scheme、query 与 fragment 不参与树键，注册时也不去重；
4. 注册点是通知 URI 的祖先时，只有 `notifyForDescendants=true` 才匹配；精确点与注册后代节点按 leaf 规则处理，而 `NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS` 会排除其中声明监听后代的 entry；
5. self 由服务端比较 observer Binder 身份计算，origin 的布尔意愿只决定这类匹配是否被保留；user 则按 hard user 与 `USER_ALL` 三路通配；
6. Collector 按 `(IContentObserver Java包装对象, 注册 uid, selfChange, flags, 目标 user)` 合批，却不对 URI 去重；同一entry若在重复输入的两次遍历中都存活，会在同一数组里追加重复值，重复/重叠注册还可能因包装对象不同而拆成多次回调；
7. 前台 UID 或带 `NOTIFY_NO_DELAY` 的 key 立即派发，其他 key 只是在 system_server `BackgroundThread` 上延迟至少 10 秒；它不跨 notify 调用合并，也没有取消句柄；
8. 远端 `IContentObserver.onChangeEtc()` 是 oneway，但本地 Binder 是普通 Java 直调；App 有 Handler 时还会再 post 一次，因此“服务已派发”不等于“业务 onChange 已完成”；
9. 服务端源码顺序是 observer dispatch、每个 `(authority,resolvedUser)` 的可选 local sync筛选/调度、该键对应的cache失效。三者没有共同事务，也不是绝对异常隔离。

排查时至少要同时记录四个完成点：调用者是否从 ContentService 返回、observer 任务是否已提交或执行、cache 是否已失效、sync 是否仅被调度。把任意一个完成点简称为“通知完成”，都会制造错误因果。

## 2. 调用边界：同步入口、通常异步的回调与后续副作用要分层观察

正常调用图如下：

`Provider/App → ContentResolver → IContentService.notifyChange → ContentService` 先完成一次同步 Binder RPC。ContentService 在仍持原调用身份时解析 user、检查跨用户资格、验证 authority 并收集观察者；随后清除 Binder identity，以 system_server 身份依次运行 `collector.dispatch()`、调度 sync、清 cache，最后恢复 identity并返回。

observer 分支又有两层调度。跨进程时，system_server 调用 oneway Proxy，只等待 Binder 驱动接收事务，不等待目标进程的 `onChange()`；目标进程的 Transport 收到后，如果构造 `ContentObserver` 时带 Handler，还会把业务回调再排进该 Looper。没有 Handler 时，回调就在收到 Transport 调用的当前线程执行。

这张边界表比“异步通知”四个字更准确：

| 边界 | 调用方返回时能保证什么 | 不能保证什么 |
|---|---|---|
| public Collection 的一个 user 组 | 该组的 ContentService RPC 已返回或抛异常 | 其余 user 组完成；App Handler 已消费 |
| 远端 oneway observer 调用 | ContentService已完成一次提交尝试；当场异常已按实现处理 | 事务一定入队；App业务成功；cache已先失效 |
| 本地 observer 直调 | 无 Handler 时业务回调已经返回 | 运行时异常一定被隔离 |
| `scheduleLocalSync()` 返回 | SyncManager的筛选/调度调用已返回 | 一定创建任务；网络已发生；服务端数据已上传 |
| cache invalidation 返回 | 匹配的 ContentService cache key 已删除 | Provider 自己的缓存或数据库已改变 |

`IContentService.notifyChange` 本身不是 oneway，所以参数验证、立即本地回调以及 sync/cache 路径上的异常都可能同步影响调用者。反过来，已经 post 的后台 observer 任务可能在原调用返回很久以后才运行。全链既不是数据库事务的延长，也不是一条有全局先后可见性的事件总线。

## 3. 客户端重载：默认会请求 sync，Collection 按 user 拆成多个 RPC

最短的双参数 `notifyChange(uri, observer)` 调用旧 boolean 重载，并显式传 `true`；后者把它映射为 `NOTIFY_SYNC_TO_NETWORK`。因此“没有写 flags”不等于“不请求同步”。若只想发观察者通知而不触发 local sync 意图，应使用 flags 重载并明确不带 bit 0。

public 单 URI flags 重载做两件事：用 `getUserIdFromUri()`读取嵌入 user，缺省为当前 Context user；再用 `getUriWithoutUserId()`把 user 从交给服务端的 URI 中移除。public 注册入口采用同样的拆分。于是正常 public 路径中，树键的 user 在独立参数里，URI 的 authority 不再带 `10@`。

public Collection 不是一次原子批量调用。它遍历输入，为每条 URI 计算 user，并放进 `SparseArray<ArrayList<Uri>>`；列表保存去 user 后的 URI。随后按 SparseArray 数值升序逐组调用隐藏的 `Uri[]` 入口。空 Collection不产生 Binder调用。Collection不逐项判空：两个 ContentProvider helper对 null分别返回默认user和null URI，所以null会进入某个user组，通常到ContentService直接读取`uri.getAuthority()`时才失败。它不按 authority聚类，也不保留user首次出现顺序。

每个 user组是独立同步 RPC。这意味着user 0组可能已经提交或排队observer任务、完成一次sync筛选/调度尝试并清除cache，user 10组才因权限错误抛出；它仍不保证App `onChange()`已执行，也不保证SyncManager最终创建任务。只能说“同一个ContentService RPC在dispatch前处理完它接收的URI验证循环”，不能把这个保证扩展到public Collection的全部user组。

隐藏的显式 user 单 URI与数组重载不会替调用者去掉 URI 内嵌 user；它们把 URI 原样送入服务端。因此“URI 总会先归一化”只适用于 public 入口。最终调用还携带 origin Transport、当下 `deliverSelfNotifications()`结果、原 flags、目标 user、`mTargetSdkVersion` 和 Context package name；这些普通参数不等于服务端已把每一个值都当身份凭证验证。

### 练习 1：重放默认 sync、URI user 拆分与多 user 分组

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'notifyChange(uri, observer, true /* sync to network */);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'syncToNetwork ? NOTIFY_SYNC_TO_NETWORK : 0);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'ContentProvider.getUriWithoutUserId(uri),' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'ContentProvider.getUserIdFromUri(uri, mContext.getUserId()));' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final SparseArray<ArrayList<Uri>> clusteredByUser = new SparseArray<>();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final int userId = ContentProvider.getUserIdFromUri(uri, mContext.getUserId());' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'list.add(ContentProvider.getUriWithoutUserId(uri));' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final int userId = clusteredByUser.keyAt(i);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'notifyChange(list.toArray(new Uri[list.size()]), observer, flags, userId);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'observer != null && observer.deliverSelfNotifications(), flags,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'userHandle, mTargetSdkVersion, mContext.getPackageName());' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'if (uri == null) return defaultUserId;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (uri == null) return null;' frameworks/base/core/java/android/content/ContentProvider.java
```

准备 user 10 Context 下的三条 URI：无 user、`0@`、`10@`，并故意交错、重复。画出SparseArray的键、每组保留的URI顺序和RPC次序；Collection按调用者传入的flags推演，再单独用单URI双参数入口验证默认置bit 0。最后让第二组验证失败，标出第一组哪些副作用已经不可撤销。

## 4. 服务端准入：READ/WRITE grant 处理跨 user，authority 检查只证明“可能访问”

注册与通知都先取真实 Binder pid/uid。注册把 `FLAG_GRANT_READ_URI_PERMISSION`交给 `handleIncomingUser()`，通知使用 WRITE；这两个 grant 只在目标 hard user 不同于 calling user 时，作为跨用户权限的替代路径检查。同 user 根本不会在这个 helper 中检查 URI grant，所以 notify 通过并不证明调用者持有 WRITE grant，更不证明 Provider 数据确实被写过。

`USER_CURRENT`先解析成 ActivityManager 当前用户。`USER_ALL`要求 `INTERACT_ACROSS_USERS_FULL`，但返回值仍是 -1。其他负数直接非法。目标是不同 hard user 且具体 URI grant 未通过时，注册和通知都因 `allowNonFull=true`而接受 FULL 或普通 `INTERACT_ACROSS_USERS`。接口注释只强调 FULL，实际实现还接受后者，源码顺序应高于注释概括。

接着 `ActivityManagerInternal.checkContentProviderAccess(authority,user)`做 authority 级“possible chance”检查。AMS 自己的注释明确最终权限仍在 ContentProvider；这里可能因顶层 read/write permission、某个 PathPermission 或 authority grant而允许，却不会执行上一章 Transport 对本次具体 URI/方向的最终检查。notify 也无需先调用 CRUD，更不会验证 INSERT/UPDATE/DELETE flags 与真实数据库动作一致。

目标 SDK 决定拒绝兼容。O 及以上收到任何 access message 都抛 `SecurityException`。O 以前若只是找不到 Provider，注册或通知仍可进入一棵“幽灵 authority”树；其他 access 拒绝在注册时直接返回，通知时跳过当前 URI。正常 ContentResolver 提交真实 target SDK，但 AIDL 参数没有绑定到 Binder UID；直接隐藏 Binder 调用者可伪造较低值，得到缺失 Provider 的兼容行为。这不是数据访问授权，因为根本没有一个真实 Provider 因此被开放。

通知在一个 RPC 内以 `(authority,resolvedUserId)`缓存验证结果和 Provider package。相同键只验证一次；不同 user 或 authority 分开。若 O+ 的后续 URI 在循环中抛出，代码尚未调用 `collector.dispatch()`，因此该 RPC 之前收集的 URI也未派发，sync/cache也未开始。O 前的可忽略拒绝则只 continue 当前 URI。

### 练习 2：区分跨 user 资格、authority 预检与 target SDK 兼容

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'Intent.FLAG_GRANT_READ_URI_PERMISSION, true, userHandle);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'Intent.FLAG_GRANT_WRITE_URI_PERMISSION, true, userId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (userId == UserHandle.USER_CURRENT) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'userId = ActivityManager.getCurrentUser();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (userId == UserHandle.USER_ALL) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (checkUriPermission(uri, pid, uid, modeFlags,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'Manifest.permission.INTERACT_ACROSS_USERS)' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F '.checkContentProviderAccess(uri.getAuthority(), resolvedUserId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (targetSdkVersion >= Build.VERSION_CODES.O) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (msg.startsWith("Failed to find provider")) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final Pair<String, Integer> provider = Pair.create(uri.getAuthority(), resolvedUserId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final ArrayMap<Pair<String, Integer>, String> validatedProviders = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'Check if the calling UID has a possible chance at accessing the provider' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'Final permission checking is always done' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

分别推演 same user、不同 hard user、`USER_CURRENT`、`USER_ALL`；为每种情况标出grant、FULL、普通跨用户权限和authority预检的角色。再用两个authority做单RPC：第二个先设为不存在的Provider，再设为Provider存在但访问拒绝；分别比较target R与target N的continue、异常及副作用完成点。

## 5. 观察者树键：authority 是第 0 段，后面只接 decoded path segments

`ContentService`持有一个名字为空的 root。`ObserverNode.countUriSegments(uri)`返回 path segment 数加 1；`getUriSegment(uri,0)`取 authority，后续索引取 `getPathSegments().get(index-1)`。注册递归创建或复用相同名字的 child，到叶节点才把 `ObserverEntry`加入 `mObservers`。

因此树键不含 scheme、query、fragment。`content://books/items?owner=a`与相同 authority/path 的另一 query 落在同一节点；隐藏入口若交入另一分层 scheme但 authority/path 相同，也会落在同一树枝，而回调仍携带原通知 URI。观察者树不是 URI 安全验证器，Provider 不能依赖 query 区分权限域。

path 取自 Android `Uri.getPathSegments()`：它按 encoded `/`切分，忽略开头、结尾与重复斜杠形成的空段，再逐段 decode。于是 `/a//b/`与 `/a/b`具有相同 segment 序列；`/a%2Fb`则先作为一个 encoded segment，随后解码为名字 `a/b`，不同于 `/a/b`的两个节点。树节点存 decoded 字符串，而不是完整 URI 文本。

user 也不在节点名字里，而在每个 entry 的 `userHandle`字段中。正常 public 客户端已把 URI 内嵌 user拆出；隐藏入口可能保留特殊 authority 文本，从而创建不同树名，这是调用路径差异，不能概括成系统总会替隐藏调用者规范化。

注册没有重复检测。同一个 Transport 在同一节点注册两次，会得到两个 entry和两个 death recipient；在祖先与叶子同时注册，则两个节点各有 entry。后续树匹配不会把这种重叠视作同一逻辑订阅。至于 Collector是否并进一个 key，还取决于 entry保存的 `IContentObserver` Java包装对象是否 `Objects.equals()`，而不是只看底层 Binder是否相同。

一个只属于旧 target兼容面的哨兵碰撞值得单列：pre-O App若通知非null对象 `Uri.EMPTY`，并先通过跨用户检查，null authority会被“找不到 Provider”的兼容分支放行。它没有path段；收集时root的第0段也是null，而递归用`segment == null`表示“通知路径已走完、遍历全部children”，结果会走遍所有authority子树，随后仍逐entry应用self、user与leaf上的SKIP过滤。注册`Uri.EMPTY`反而会在add阶段因null segment抛异常。若沿默认双参数入口还带着sync bit，null authority继续传给SyncManager；其`requestedAuthority == null`语义是不再按authority收窄，但仍只在calling user的运行账户、可用adapter、syncable与设置条件内筛选。正常有效的`content://authority/...`不会触发这一边界；Collection中的真正null元素也会更早失败，不会走到这条全树链。

### 练习 3：手算 authority、encoded path、decoded segment 与重复注册

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final class ObserverNode {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'return uri.getAuthority();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'return uri.getPathSegments().get(index - 1);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'return uri.getPathSegments().size() + 1;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (index == countUriSegments(uri)) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'mObservers.add(new ObserverEntry(observer, notifyForDescendants, observersLock,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'ObserverNode node = new ObserverNode(segment);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'while ((current = path.indexOf('\''/'\'', previous)) > -1) {' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'if (previous < current) {' frameworks/base/core/java/android/net/Uri.java
grep -n -F '= decode(path.substring(previous, current));' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'segmentBuilder.add(decode(path.substring(previous)));' frameworks/base/core/java/android/net/Uri.java
```

为 `/a//b/`、`/a/b`、`/a%2Fb`、带 query 的 `/a/b`画树。再让同一 observer 在 `/a`重复注册两次、在 `/a/b`注册一次，记录节点、entry和 death recipient 数；不要提前用对象身份去重。

## 6. 匹配算法：祖先看 descendants，精确点与注册后代都走 leaf 规则

收集从 root 向下递归。通知路径尚未走完时，当前节点以 `leaf=false`检查自己的 observers，只有 `notifyForDescendants=true`的 entry可通过；因此注册 URI是通知 URI祖先时，descendants开关决定是否接收。

当递归索引到达通知 URI末尾，当前节点以 `leaf=true`收集全部 observers。此时通常不再检查 descendants布尔，所以精确注册无论 true/false 都接收。随后 `segment`保持 null，递归会进入当前节点的所有 children；这些更深注册节点也以 leaf=true收集。也就是说，通知祖先 URI会命中其注册后代，即使该 entry 的 `notifyForDescendants=false`。

但“更深注册无条件接收”仍然过强。每个 entry都必须先通过 self和user过滤；而 leaf分支还受 `NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS`约束：只要该 flag存在且 entry 的 descendants=true，精确节点和所有注册后代节点都会被跳过。SKIP不作用于通知路径尚未结束的非叶祖先，因为那里本就只接受 descendants=true。

把三种位置写成真值表：

| 注册点相对通知 URI | `leaf` | descendants=false | descendants=true | 带 SKIP 时 |
|---|---:|---:|---:|---|
| 注册点是祖先 | false | 不收 | 收 | 仍收 |
| 精确相等 | true | 收 | 收 | false收，true跳过 |
| 注册点是后代 | true | 收 | 收 | false收，true跳过 |

这个设计支持 Provider先发一个“X下有变化”的概括 URI并带 SKIP，再发具体子 URI：监听 X及后代的 observer只接具体通知，而只精确监听某个子项的 observer仍可接收概括通知。它不是常见的单向前缀订阅，诊断时必须同时标出注册点和通知点。

### 练习 4：用三种相对位置验证 leaf 与 SKIP

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void collectMyObserversLocked(Uri uri, boolean leaf, IContentObserver observer,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (leaf) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if ((flags&ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) != 0' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F '&& entry.notifyForDescendants) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (!entry.notifyForDescendants) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (index >= segmentCount) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'collectMyObserversLocked(uri, true, observer, observerWantsSelfNotifications,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'collectMyObserversLocked(uri, false, observer, observerWantsSelfNotifications,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (segment == null || node.mName.equals(segment)) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'node.collectObserversLocked(uri, segmentCount, index + 1, observer,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'public static final int NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS = 1<<1;' frameworks/base/core/java/android/content/ContentResolver.java
```

注册 `/a`、`/a/b`、`/a/b/c`，每处各放 descendants true/false 两个 entry。分别通知三个 URI并切换 SKIP，逐格写出结果；再叠加后文的 self=false和不同 user，确认“树位置命中”只是第一层条件。

## 7. self 与 user：Binder 身份决定 selfChange，目标 user 决定 Collector 事件域

self不是 Java `ContentObserver`对象相等，也不是 uid、package或进程相等。ContentResolver把 origin observer 的 Transport传给服务端；每个 entry用 `entry.observer.asBinder() == origin.asBinder()`做精确 Binder 身份比较。命中者的 `selfChange=true`，其余 observer为 false。origin是 null时所有回调都是 false；同进程但不同 ContentObserver也不是 self。

`observerWantsSelfNotifications`由发起 notify 的 origin当下 `deliverSelfNotifications()`产生。服务端信任这个布尔值：self且值为 false就彻底跳过；值为 true才把该 URI收进 Collector。它不改变其他 observer，也不会验证布尔值必然来自那个 Binder对象。直接 AIDL调用者可以提交不一致参数，但 selfChange仍由服务端的 Binder比较计算。

user条件是三路 OR：目标为 `USER_ALL`、entry注册为 `USER_ALL`、或 hard user精确相等。Collector收到的是 `targetUserHandle`，不是 entry的注册 user。因此 user 10通知会同时命中 user 10 entry与 USER_ALL entry，并且两者的 callback userId都为10；USER_ALL通知会命中各 hard user entry，但每个 callback userId仍为-1，并不展开成一条条 hard-user事件。

如果同一个 Binder在 user 10与 USER_ALL都注册同一节点，两项都可能命中同一次 user 10通知，回调都携带 userId 10；USER_ALL通知多个 hard-user注册时，回调都携带-1。但这不保证它们聚进同一 key：Collector对 `IContentObserver`用 `Objects.equals()`，self/unregister才显式比较 `asBinder()`。远端 Binder在不同注册事务中通常被解包为不同 Proxy对象，因而常形成多个 key与多次 callback；本地调用复用同一 Stub/包装对象时才可能合并，并在数组中出现重复 URI。无论是哪一种，user通配都不提供事件去重。

`USER_ALL`的通配只属于观察者匹配。AMS检查该 authority时会要求 FULL并暂用 calling user解析 Provider；ContentService后续 cache直接查 -1桶，sync仍传 calling user。它不是“替每个用户分别执行 notify/cache/sync”的广播循环。

### 练习 5：验证 Binder self、user通配与重复 URI

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'IBinder observerBinder = observer == null ? null : observer.asBinder();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'boolean selfChange = (entry.observer.asBinder() == observerBinder);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (selfChange && !observerWantsSelfNotifications) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (targetUserHandle == UserHandle.USER_ALL' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F '|| entry.userHandle == UserHandle.USER_ALL' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F '|| targetUserHandle == entry.userHandle) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'collector.collect(entry.observer, entry.uid, selfChange, uri, flags,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'targetUserHandle);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'userId = UserHandle.getCallingUserId();' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'invalidateCacheLocked(resolvedUserId, packageName, uri);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'syncManager.scheduleLocalSync(null /* all accounts */, callingUserId,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
```

让同一 Binder分别注册 user 10、user 11与 USER_ALL，并另放一个不同 Binder。分别按远端多次解包的 Proxy对象、本地复用同一 Stub两种情况，通知 user 10和 USER_ALL，列出每个 Collector key的 callback userId、selfChange和 URI次数；再说明为何不能据 userId=-1假装已经完成每个用户的 cache失效。

## 8. 注册生命周期：一个 ContentObserver 复用 Transport，注销与死亡都不是无竞态屏障

App侧 `getContentObserver()`在 `mLock`内惰性创建 Transport；同一个 ContentObserver重复注册通常复用同一 Binder。`releaseContentObserver()`把 Transport内部的 `mContentObserver`置 null，再清掉自身引用并返回旧 Transport给 ContentResolver注销。以后重新注册会创建新 Binder。

服务端每次注册都新建 ObserverEntry并调用全局 `BinderDeathDispatcher.linkToDeath()`。Dispatcher以 `target.asBinder()`为键，再用 `ArraySet`保存多个 entry recipient；对远端 Binder，它只建立一条底层 death link并将死亡事件扇出。本地 `Binder.linkToDeath()`实现是 no-op，因此本地observer没有这条进程死亡清理保障，未显式注销的entry可一直留到system_server生命周期结束。达到1000个recipient时仅对该UID首次打印严重告警，并不拒绝本次注册。

显式 unregister从 root递归。每个节点只删除第一个 Binder相同的 entry便 break，所以同一节点的重复注册会残留 N-1项；不过它会遍历所有节点，因此不同节点上的首项都能删除，并在递归返回时剪掉空 child。App第二次 unregister时自身 `mTransport`已为 null，不会再次请求服务端；同节点残留项通常只能等待进程死亡，其旧 Transport又已断开 App对象，造成静默工作与结构泄漏。

远端活 Binder死亡时 Dispatcher复制全部 recipients并逐个调用。这里的 `ObserverEntry`是注册节点的非静态内部类；未限定的 `removeObserverLocked(observer)`从该 entry所属节点开始，不是从 root开始。它能删除该节点及其子树中同 Binder项，但返回的 empty没有父调用者接住，所以常见叶节点死亡会暂留从 root到叶的空节点骨架。以后任意一次从root开始的显式unregister递归都可顺手剪掉空child，同URI新注册也会复用既有骨架；这不是永久不可达泄漏。

还有一个构造顺序锐角：`mObservers.add(new ObserverEntry(...))`先执行构造器。若 `linkToDeath()`返回 -1，构造器立即调用 `binderDied()`，此时新 entry尚未加入列表；移除可能什么也找不到，甚至删掉一个旧的同 Binder项。构造完成后外层 add仍会插入这个已死 entry，且未来没有 death回调。死亡清理是兜底，不是形式证明。

注销也不是 callback排空屏障。服务端 Collector已经快照或 BackgroundThread已经排队的任务不会被撤销；Transport的 `onChangeEtc()`先把 `mContentObserver`复制到局部变量，release与这次读取没有同一锁或 volatile约束；App Handler里已排队的 Runnable同样保留对象引用。对本次真正删除的entry，注销返回后才开始的树收集不再命中；同节点重复注册遗留的N-1项仍可被收集，只是旧Transport通常因字段已置null而丢弃转发，并且该字段竞态仍不提供可见性证明。因此unregister不能承诺返回后业务再无onChange。

### 练习 6：复现重复注册、死亡清理空骨架与注销竞态

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mTransport == null) {' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'mTransport = new Transport(this);' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'oldTransport.releaseContentObserver();' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'mTransport = null;' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'final int entries = sObserverDeathDispatcher.linkToDeath(observer, this);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if (entries == -1) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F '} else if (entries == TOO_MANY_OBSERVERS_THRESHOLD) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'removeObserverLocked(observer);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'sObserverDeathDispatcher.unlinkToDeath(observer, entry);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'break;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'info.mRecipients.add(recipient);' frameworks/base/core/java/com/android/internal/os/BinderDeathDispatcher.java
grep -n -F 'copy.valueAt(i).binderDied();' frameworks/base/core/java/com/android/internal/os/BinderDeathDispatcher.java
grep -n -F 'ContentObserver contentObserver = mContentObserver;' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'mContentObserver = null;' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'Local implementation is a no-op.' frameworks/base/core/java/android/os/Binder.java
```

对同一节点注册三次并注销一次，逐步记录 mObservers、Dispatcher recipient和 App Transport；再分别模拟活 Binder死亡与 linkToDeath发现已死，画出 entry和空节点是否残留。最后安排 Transport读取、release、Handler post交错，说明注销返回后仍可能发生哪类回调。

## 9. Collector：五元组聚合但不去重，批量收集也不是锁住整批 URI

`ObserverCollector.Key`包含 `IContentObserver` Java包装对象、注册 uid、selfChange、flags、目标 userId。equals对 observer调用 `Objects.equals()`，hashCode也直接纳入该对象，并没有改用 `observer.asBinder()`；任一项不同就拆成另一次 callback。flags不会被掩码，未知bit会原样进入key并传播到App；类级key语义会区分不同flags，不过当前正常notify的一个RPC只传一份flags，因而不会仅靠未知bit在该RPC内部拆组。INSERT/UPDATE/DELETE只是Provider提供的变化提示，ContentService不查数据库来验证它们。

同一 key第一次出现时创建 `ArrayList`，之后只执行 `value.add(uri)`，没有contains或集合去重。若两次遍历时同一entry仍在树中且都通过self/user/SKIP过滤，重复输入URI会把相同值追加多次；两条URI之间的注销或死亡也可能让它只追加一次。多个注册若在本地复用同一observer包装对象，也可能合并后追加重复值。远端同一Binder在每次AIDL解包时通常产生新的`IContentObserver.Stub.Proxy`接口包装，所以更可能拆成多个key、收到多次callback；底层`IBinder/BinderProxy`仍可相同，故self、unregister和death的`asBinder()`比较仍能汇合。

dispatch对本地和远端都会先执行 `value.toArray()`。本地直调也收到一个新数组，但其中Uri元素仍是system_server对象引用，Transport随后再包装成List；跨进程时数组与Uri还会经过Parcel在App重建。因此任何路径都不是把Collector内部List本身交给业务。

每次 ContentService RPC创建一个新 Collector，因此10秒窗口内的多次 notify不会跨调用合并或去抖。一个 public Collection又可能按 user制造多个 Collector。所谓“批量通知”只是在一个 key内减少 Binder事务数，不是全局事件合并器。

服务端对每条 URI单独进入 `synchronized(mRootNode)`，收集后立即释放，并非整个数组持锁。注册或注销可夹在两条 URI之间：已经加入 Collector的快照不会撤回，新注册则可能只看到后面的 URI。dispatch在整个验证/收集循环之后且位于树锁之外，避免回调期间持有观察者树锁，却也明确了快照与实时树状态可以分离。

### 练习 7：验证五元组、URI重复与逐 URI 锁窗口

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final IContentObserver observer;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final boolean selfChange;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final int flags;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final int userId;' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'return Objects.equals(observer, other.observer)' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'return Objects.hash(observer, uid, selfChange, flags, userId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final Key key = new Key(observer, uid, selfChange, flags, userId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'value.add(uri);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'value.toArray(new Uri[value.size()]), key.flags, key.userId);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'final ObserverCollector collector = new ObserverCollector();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'synchronized (mRootNode) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'mRootNode.collectObserversLocked(uri, segmentCount, 0, observer,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'collector.dispatch();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
```

构造两个相同 URI、同 Binder的祖先/精确重复注册，再分别按“远端不同Proxy包装”和“本地同一Stub对象”计算 key；然后改变 flags、self与目标 user。先算出每个 key的数组，再在两条 URI收集之间插入 unregister，判断已收集项和第二条 URI各自如何变化。

## 10. 延迟派发：procState 只采样一次，10 秒是最小排队延迟而非交付期限

dispatch逐 key建立 Runnable。它只捕获当次 URI值列表和 key；不在执行时重新查树、user或进程状态。服务端在 dispatch时查询注册 uid的 process state：不差于 `PROCESS_STATE_IMPORTANT_FOREGROUND`，或 flags含隐藏的 `NOTIFY_NO_DELAY`，立即 `task.run()`；其余投递到 `BackgroundThread`，延迟常量为10秒。

这里的10秒基于 Handler调度，只是最早可运行时间，不是 deadline。设备深睡、system_server队列拥塞或Looper状态都可能让实际时间更晚；`postDelayed()`返回值也未检查，队列拒收时没有补偿。UID在排队后转前台不会重新采样或提前任务，转后台也不会推迟已经立即执行的 key。

`NOTIFY_NO_DELAY`虽是 hidden flag，却没有服务端 permission或uid门；能直接发 AIDL或以其他方式设置 bit 15的调用者可绕过后台10秒策略。hidden API限制不是安全边界。相反，INSERT/UPDATE/DELETE位也只被透传，不影响延迟判断、树匹配、cache或sync，只有 bit 0、bit 1、bit 15在这条链中具有相应框架语义。

后台任务没有保存 token，unregister不会找到它，`cancelSync()`也只影响另一个子系统的同步请求。多次 notify各自产生任务，不会因 URI相同在10秒内折叠。ContentService只包围一次oneway提交尝试，提交当场出现的`RemoteException`会被忽略，而且异常不只代表死亡；若事务正常入队后目标才死亡，发送端不会因此收到异常，App业务仍可能没有执行。树的死亡清理由独立death recipient负责。

异常边界尤其重要。Runnable只捕获 `RemoteException`。远端 Proxy的业务异常不会沿 oneway回到 ContentService；但同进程 IContentObserver是普通 Java调用，本地无 Handler的 observer若抛 `RuntimeException`，立即路径会中断剩余 Collector keys，并阻止本次后续 sync/cache。延迟本地异常则发生在原 notify返回以后，此时 sync/cache通常已完成，但可能伤害 BackgroundThread消息处理语境。三分支没有共同事务，不代表所有异常天然隔离。

## 11. App 侧交付：Handler 再切一次线程，R 用 compat change 拆解旧 int 语义

Transport收到 `onChangeEtc(self, Uri[], flags,userId)`后先把 `mContentObserver`读到局部变量；非 null才调用 `dispatchChange()`。无 Handler时直接进入隐藏的四参数 Collection `onChange`；有 Handler时把同一批值捕获进 Runnable再 post。post返回值未检查，退出中的 Looper可能静默丢回调。

跨进程远端 observer通常先在 App Binder线程执行 Transport；无 Handler就继续在该 Binder线程跑业务，有 Handler才转入指定 Looper。本地立即 observer则可能在 ContentService当前线程执行；本地后台 observer可能在 system_server BackgroundThread执行。因而回调中观察 `Binder.getCallingUid()`不能可靠获得最初 notify调用者：清身份后的本地直调看到system_server语境，App Handler任务通常已回到自身线程语境。

历史隐藏API是单URI `onChange(boolean, Uri, int userId)`；R把同一Java签名公开为最后一个int表示flags，并新增Collection入口，参数名变化无法产生另一份重载。Transport始终进入隐藏四参数Collection入口；其中compat change `150939131`在target R及以上、且进程不是SYSTEM_UID时启用，把flags传给现行三参数Collection入口；旧target或SYSTEM_UID则把userId送进这个Collection入口，默认实现再逐URI调用单URI同签名方法，使旧override继续把int解释成userId。

这项 compat在目标 App进程中用 `Process.myUid()`和 compat配置求值，不由 ContentService收到的 targetSdk参数代算。SYSTEM_UID无论 target多新，都保留旧 int=userId路径，除非实现直接覆写隐藏四参数入口。为了跨版本稳健，自定义 observer应理解自己覆写了哪个 overload，不要假设所有三参数回调中的 int都天然同义。

### 练习 8：跟踪 oneway、Handler 与 R 三参数兼容分流

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'oneway void onChangeEtc(boolean selfUpdate, in Uri[] uri, int flags, int userId);' frameworks/base/core/java/android/database/IContentObserver.aidl
grep -n -F 'ContentObserver contentObserver = mContentObserver;' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'contentObserver.dispatchChange(selfChange, Arrays.asList(uris), flags, userId);' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'if (mHandler == null) {' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'onChange(selfChange, uris, flags, userId);' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'mHandler.post(() -> {' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'private static final long ADD_CONTENT_OBSERVER_FLAGS = 150939131L;' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F '@EnabledAfter(targetSdkVersion=android.os.Build.VERSION_CODES.Q)' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'if (!CompatChanges.isChangeEnabled(ADD_CONTENT_OBSERVER_FLAGS)' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F '|| android.os.Process.myUid() == android.os.Process.SYSTEM_UID) {' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'onChange(selfChange, uris, userId);' frameworks/base/core/java/android/database/ContentObserver.java
grep -n -F 'onChange(selfChange, uris, flags);' frameworks/base/core/java/android/database/ContentObserver.java
```

为 target Q普通 App、target R普通 App与 SYSTEM_UID各画一次调用栈，标出三参数 Collection入口的 int实际是什么。再分别配置 null Handler和主线程 Handler，说明 Binder事务入队、Transport执行、Handler入队与业务完成四个时刻。

## 12. Cache 分支：这是受 CACHE_CONTENT 保护的服务缓存，失效规则是字符串前缀

ContentService cache不是所有 `ContentResolver.query()`的通用透明缓存。只有显式 `putCache/getCache`入口参与，调用者需 `CACHE_CONTENT`、通过跨用户检查，并由 AppOps `checkPackage()`确认 client package属于 calling UID。存储形状是 `userId → providerPackageName → Pair(clientPackageName,Uri) → Bundle`。

notify为每个已经验证的 `(authority,resolvedUserId)`取得 hosting provider package，然后遍历原始 `uris`数组，挑出 authority相等的变化 URI调用 `invalidateCacheLocked()`。flags不控制这一步；即使没有 sync bit或没有 observer，只要流程到达这里仍会失效。外层 authority比较只筛选变化 URI，进入同一个 provider package缓存后，并不会再次核对被缓存 key的 authority。

真正删除时并不做 path segment关系判断，而是 `cachedUri.toString().startsWith(changedUri.toString())`，所以既可能多清，也可能漏清。通知 `/foo`会清 `/foobar`；若同一 package承载 authority `a`与`ab`，通知 `content://a`还可能前缀命中缓存的 `content://ab`。反过来，观察者树忽略 query、空尾段并比较decoded segment，cache字符串却保留这些差异：通知带不同query、尾斜杠或不同编码文本时，树可以命中而cache前缀不命中。`invalidateCacheLocked(...,null)`供包事件等整包清理；notify正常成功路径传具体URI，数组中的null元素会在此前解引用失败。

`USER_ALL`不会循环各 hard-user cache。resolvedUserId仍是-1，helper只查 `mCache.get(-1)`；AMS为 authority检查临时解析 calling user也不会改回这个值。Provider package解析在 -1上的结果还可能为 null，而 map允许把它当键查询。观察者通配成功不能据此推出实际用户缓存已逐一失效。

cache清理发生在 collector.dispatch之后、同一 Provider key的可选 sync调度之后。后台 observer只是已排队，所以通常 cache会先清；远端前台 oneway可能已经开始。本地Transport调用确定发生在sync/cache之前，但只有ContentObserver无Handler时业务`onChange()`也同步先执行；有Handler时这里只完成post，业务仍可晚于cache。不能向 observer承诺“收到时 cache必定已清”。

## 13. Sync 分支：按 authority 与 resolved user 去重，却把 calling user交给 SyncManager

sync仅在 flags含 `NOTIFY_SYNC_TO_NETWORK`时运行。服务端遍历 `validatedProviders`，所以一次 ContentService RPC对每个唯一 `(authority,resolvedUserId)`最多调用一次 `scheduleLocalSync()`；不是对每个 URI一次，也不是跨所有 RPC全局每个 authority一次。public Collection按 user拆分后，各 RPC仍可分别调度。

调用参数有一个刻意的不对称：去重键和 cache使用 resolved target user，`scheduleLocalSync`的 user参数却是原始 `callingUserId`。它还携带原 callingUid、callingPid、callingPackage与计算出的 exemption；此时 Binder identity虽已清成system_server，这些显式值仍保存来源语境。notify路径并未像 cache API那样对 callingPackage执行 `checkPackage()`，因此它在这里是调度归因元数据，不是访问 authority的授权依据。

SyncManager收到 `requestedAccount=null`后从`mRunningAccounts`开始筛选，但仍受传入user、adapter、syncable状态、账户访问和同步设置约束；无账户时直接返回，不匹配时也会continue。它在extras写入`SYNC_EXTRAS_UPLOAD=true`，再进入这些筛选与Job调度链。`LOCAL_SYNC_DELAY`默认30秒，可由`sync.local_sync_delay`系统属性覆盖；注释说至少等待该时长以聚合本地变更。`scheduleLocalSync()`返回只证明筛选/调度调用已返回，请求可能当场丢弃，不保证任务已创建，更不表示联网或上传完成。

通常 authority是已经验证的非空名称；前文提到的 pre-O `Uri.EMPTY`例外会把 null传成 `requestedAuthority`。SyncManager把 null解释为“不按authority收窄”，于是 malformed通知可能从观察者全树扩展到calling user运行账户下所有满足adapter、upload、syncable与设置条件的authority；它不是所有用户或所有系统authority的必然同步。这个结果依赖旧target的缺失Provider兼容、默认sync bit、SyncManager可用以及observer分支未以本地异常中断，任一缺失都不会形成完整放大链。

`getSyncManager()`可能不可用，此时 callback与cache仍可继续。反过来，源码没有围绕 `scheduleLocalSync()`捕获任意运行时异常；若它异常退出，当前 Provider key的cache以及后续 key都可能没执行。所谓三分支应描述为“顺序执行、无共同事务且通常异步解耦”，不能描述成强异常隔离。

### 练习 9：验证 dispatch、sync与cache的源码顺序及不同 user参数

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final long token = clearCallingIdentity();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'collector.dispatch();' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'for (int i = 0; i < validatedProviders.size(); i++) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'if ((flags & ContentResolver.NOTIFY_SYNC_TO_NETWORK) != 0) {' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'syncManager.scheduleLocalSync(null /* all accounts */, callingUserId,' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'invalidateCacheLocked(resolvedUserId, packageName, uri);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'key.second.toString().startsWith(uri.toString())' frameworks/base/services/core/java/com/android/server/content/ContentService.java
grep -n -F 'LOCAL_SYNC_DELAY =' frameworks/base/services/core/java/com/android/server/content/SyncManager.java
grep -n -F 'SystemProperties.getLong("sync.local_sync_delay", 30 * 1000 /* 30 seconds */);' frameworks/base/services/core/java/com/android/server/content/SyncManager.java
grep -n -F 'extras.putBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD, true);' frameworks/base/services/core/java/com/android/server/content/SyncManager.java
grep -n -F 'AuthorityInfo.UNDEFINED, LOCAL_SYNC_DELAY, true /* checkIfAccountReady */,' frameworks/base/services/core/java/com/android/server/content/SyncManager.java
grep -n -F 'if (requestedAuthority != null) {' frameworks/base/services/core/java/com/android/server/content/SyncManager.java
grep -n -F 'Binder.restoreCallingIdentity(token);' frameworks/base/services/core/java/com/android/server/content/ContentService.java
```

在 calling user 0通知target user 10的两条同 authority URI，再加一条第二 authority URI。计算 validatedProviders、sync次数、传入SyncManager的user与cache桶；随后分别让前台本地 observer和 scheduleLocalSync抛运行时异常，标出哪一步之后的副作用还能发生。

## 14. 失败与顺序矩阵：先区分单个 RPC，再讨论 public 多 user 调用

一个 ContentService RPC的验证/收集循环在 dispatch之前。O+ provider access异常、非法 user或跨用户拒绝会直接退出，所以该 RPC尚没有 callback、sync或cache副作用；pre-O普通拒绝只跳过相应 URI，其他合法 URI仍可进入 Collector与validatedProviders。树收集本身不调用 App代码，Collector snapshot在随后清身份后才派发。

进入副作用区后的顺序与失败面如下：

| 位置 | 同步可见失败 | 已完成或可能完成 | 尚未保证 |
|---|---|---|---|
| user/access验证中 | 参数、权限或SecurityException | 同一 RPC仅有内存收集快照 | callback、sync、cache |
| 本地、立即派发且无Handler的observer | 未捕获运行时异常 | 更早Collector key可能已回调 | 后续key、全部sync/cache |
| 远端 oneway提交 | RemoteException被忽略 | 更早任务已提交 | App业务成功及其相对cache顺序 |
| local sync调度 | 未捕获运行时异常 | `collector.dispatch()`已返回；更早key可能已推进 | 远端/延迟/Handler业务回调完成；当前cache与后续provider key |
| cache失效 | 本实现未包成跨key事务 | 该key之前步骤 | 后续key |
| 后台/App Handler任务 | 与原RPC解耦 | sync/cache通常已经推进 | 回调一定执行；注销能够撤回任务 |

public Collection还在外层按user做多个RPC。前一组已经从服务端返回后，后一组才发起；后一组失败不回滚前组。若需要应用层“先验证所有目标，再让任何用户看到变化”，notify API本身不提供这种跨用户事务，应在数据与授权设计中另建协议。

同一个 RPC也没有把整组 URI锁成树快照：注册/注销能夹在两次收集之间；但后续 hard异常仍会阻止整个 Collector dispatch。这是“已收集到内存”与“已对外派发”两个完成点，日志必须分开记。

最后，不要用 observer顺序作为 cache一致性协议。ArrayMap与树遍历顺序是实现细节；远端 oneway、BackgroundThread与App Handler又各自引入调度。业务若必须在读取前看到某个提交，应依赖数据库事务、版本号或显式确认，而不是等待一次 onChange的表面时序。

## 15. 实现与诊断清单：把 URI、Binder、user、flags 与四个完成点同时记下来

Provider侧发送通知时，先选择足够精确的 URI。不要把 query参数当观察者树的安全边界；若先发概括 URI再发具体 URI，明确是否需要 SKIP减少 descendants observer重复。INSERT/UPDATE/DELETE只用于表达变化类型，不能替代事务提交，也不应伪造。若不希望同步，避免沿用默认双参数重载；若使用 NO_DELAY，应把它当性能例外审计。

Observer侧应把回调视为失效提示而非变更日志：数组可重复、可合批、可延迟，也可能只收到祖先 URI。回调中重新查询并用版本/主键去重；Handler要绑定有效 Looper，重工作移出 Binder线程。注销前可先设置自己的关闭状态，回调第一句检查；若业务需要严格排空，再在自己的串行执行器上建立 barrier，不能只信 unregister返回。

跨用户代码要同时记录 requested user、resolved user、calling user与callback user。特别标记 `USER_CURRENT`解析时刻和`USER_ALL=-1`，不要把observer通配误写成逐用户cache/sync。审计授权时，把跨用户资格、AMS authority possible-access与Provider Transport最终读写权限分成三列；notify本身不证明调用者真的改过数据。

遇到“没回调”，按以下顺序缩小范围：

1. public客户端是否把 URI分到了预期user组，前一组是否已部分完成；
2. targetSdk provider access分支是抛异常、continue还是允许缺失 authority；
3. 树键的 authority与decoded segments是否相同，注册点相对通知点属于哪一类；
4. self意愿、hard user/USER_ALL与SKIP是否过滤 entry；
5. 是否因重复注册进入不同或相同 Collector key，数组中是否已有重复 URI；
6. UID在dispatch采样时是否后台，NO_DELAY是否存在，system_server Handler是否仍接收任务；
7. Transport是否已release，App Handler是否存活，目标override在该target下把int解释成flags还是userId；
8. 是否只看到了sync/cache结果，却误以为observer业务也已完成。

遇到“数据还是旧的”，则反向查数据库提交完成点、observer是否先于cache失效执行、cache key是否真的满足字符串前缀、App是否另有缓存，以及sync是否仅在30秒起步的调度阶段。notify是失效与调度信号，不是数据库可见性或网络同步的提交证明。

## 16. 收束：第 283 章的核心是拒绝把“通知”压扁成单一事件

`notifyChange()`把一组看似简单的参数送进了五套不同语义：ContentResolver按user拆RPC，ContentService验证跨用户与authority，ObserverNode按authority/path树匹配，Collector按五元组合批并按进程状态派发，最后再顺序触发sync与字符串前缀cache失效。每层都保留自己的user、身份、延迟、异常与完成点。

最值得带走的判断式是：

`观察命中 = 树位置规则 ∧ self规则 ∧ user规则 ∧ leaf上的SKIP规则`

`一次服务调用的后半段 = dispatch observers → [按provider key可选schedule local sync] → invalidate matching cache`

这两个式子仍需带限定：Collector不去重，远端oneway不等待业务，本地调用可能同步抛异常，public跨user Collection不是单个服务调用，`USER_ALL`也不展开cache/sync。只有把这些限定连同完成点一起写进设计，才能解释“为什么重复回调”“为什么注销后还有一条”“为什么回调先于cache”“为什么通知了user10却调度calling user0”等现象。

下一章进入 `SQLiteDatabase`、`SQLiteOpenHelper`、`SQLiteConnectionPool/Session`、事务、WAL、并发连接与损坏恢复链，继续追 Provider业务方法内部一次数据库操作如何选连接、怎样嵌套事务，以及损坏处置在哪一层真正完成。
