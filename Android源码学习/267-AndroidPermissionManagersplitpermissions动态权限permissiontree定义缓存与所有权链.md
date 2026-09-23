# 267 Android PermissionManager、split permissions、动态权限、permission tree、定义缓存与所有权链

这一章不再问“某个 App 有没有拿到权限”，而是追问更靠前的问题：一个 permission 名字怎样进入系统、谁拥有它、旧应用为何会被解析器补出新名字、动态定义何时才算恢复完成，以及客户端为什么可能暂时看见旧检查结果。

全文基于本地 `android-11.0.0_r48`，`frameworks/base` 提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。Android 12 以后权限服务继续拆分，数据模型与持久化位置也有变化；下文所有异常和边界都只对这个 r48 基线负责。

## 1. 五本账：定义不等于请求，更不等于授权

读权限源码最容易犯的错误，是看见同一个字符串在不同对象里出现，就把它们当成同一份状态。实际上至少要分开五本账：

| 账本 | r48 的主要载体 | 回答的问题 | 不能推出什么 |
|---|---|---|---|
| 定义 | `mPermissions` 中的 `BasePermission` | 这个名字的来源、保护级别与元数据是什么 | 某包是否请求、某用户是否授权 |
| group | `mPermissionGroups` | 组名由谁声明、展示元数据是什么 | 组内权限已经授权 |
| tree | `mPermissionTrees` | 谁能扩展某个点号名字空间 | 树下叶子已经存在或可用 |
| 请求 | `ParsingPackageImpl` 的 requested/implicit 列表 | APK 显式或兼容性隐式请求了什么 | 请求已经获批 |
| 状态 | `PermissionsState` | 某包或 shared UID 在某用户下是否 grant、带哪些 flags | 定义仍存在、AppOp 也允许 |

此外还有 `mAppOpPermissionPackages`。它是“带 AppOp 的 permission → 请求包集合”的反向索引，不是 AppOp mode，更不是授权表。

一个名字的典型旅程是：Manifest 或系统配置提供材料，包解析得到 `ParsedPermission`，扫描注册得到 `BasePermission`，目标包的请求列表记录名字，权限恢复再写 `PermissionsState`。如果权限同时受 AppOps、组件导出状态或进程特例约束，grant 仍不等于最终操作一定成功。

因此要区分五个完成点：

1. Map 里已经有同名键；
2. `BasePermission.perm` 已绑定有效解析对象；
3. 解析对象带 `PermissionInfo.FLAG_INSTALLED`；
4. 某包某用户的 `PermissionsState` 已 grant；
5. 当前实际检查连同 AppOps 等后续门都通过。

只有第一个条件时，系统可能只是握着 XML 骨架或冲突留下的空壳。本文会反复使用“索引项”“已绑定定义”“grant 状态”这些精确词，而不把它们统称为“权限存在”。

本章主要沿下列源码走：

| 层次 | 文件 |
|---|---|
| App API 与客户端缓存 | `ApplicationPackageManager.java`、`PermissionManager.java` |
| 包解析 | `ParsingPackageUtils.java`、`PackageParser.java`、`ParsingPackageImpl.java` |
| 系统配置 | `SystemConfig.java`、`data/etc/platform.xml` |
| 权限服务 | `PermissionManagerService.java`、`PermissionSettings.java` |
| 定义对象 | `BasePermission.java`、`ParsedPermission.java`、`PermissionInfo.java` |
| 持久化与包提交 | `Settings.java`、`PackageManagerService.java` |

## 2. 客户端只是路由，四张全局索引才保存定义

公开入口散落在两个客户端类里。`PackageManager.getPermissionInfo()`、`queryPermissionsByGroup()`、`addPermission()` 等由 `ApplicationPackageManager` 实现，再经 `IPermissionManager` 进入 system_server；`android.permission.PermissionManager` 则提供 split 列表、权限检查缓存和若干系统 API。两个客户端都不是权威数据库。

服务端的 `PermissionSettings` 持有四张全局索引：

| Map | key | value | 生命周期要点 |
|---|---|---|---|
| `mPermissions` | permission 名 | `BasePermission` | 普通叶子、builtin 与 dynamic 都在这里 |
| `mPermissionTrees` | tree 名 | `BasePermission` | 独立于普通叶子 |
| `mPermissionGroups` | group 名 | `ParsedPermissionGroup` | 主要靠扫包重建，r48 运行期删除不对称 |
| `mAppOpPermissionPackages` | AppOp permission 名 | 请求包名集合 | 反向索引，不保存 mode |

它们按设计共享 `mLock`。不过一些名字带 `Locked` 的 helper 自己并不加锁，而是要求调用方已经持锁；不能把“由同一把锁保护”误读成“每个方法都会自行同步”。

`BasePermission` 包装一项全局定义，保存 `name`、`sourcePackageName`、`type`、`protectionLevel`、`uid`、GID、`perm`、`pendingPermissionInfo` 和定义变化标记。它没有“包 A 的用户 10 已允许”这种字段。

三个 `type` 也不要与保护级别混淆：

- `TYPE_NORMAL` 表示 Manifest 来源的普通 permission 或 permission tree；它可以承载 dangerous、signature 等任意保护基类。
- `TYPE_BUILTIN` 是配置先放入、以后可能被真实包声明接管的内建骨架。
- `TYPE_DYNAMIC` 是 tree 拥有者通过 API 添加的叶子定义。

对应地，`PROTECTION_NORMAL`、`PROTECTION_DANGEROUS`、`PROTECTION_SIGNATURE` 才描述授权模型。看见 `TYPE_NORMAL` 就说“普通权限自动授予”，会把来源类型与保护类型混成一件事。

### 练习 1：给五本账找到唯一归属

下面只读命令同时验证四张索引、请求列表、授权状态和扫描调用顺序。执行后给每个命中写一句“谁写、何时写、是否代表 grant”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final ArrayMap<String, BasePermission> mPermissions =' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'final ArrayMap<String, BasePermission> mPermissionTrees =' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'final ArrayMap<String, ParsedPermissionGroup> mPermissionGroups =' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'final ArrayMap<String, ArraySet<String>> mAppOpPermissionPackages = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'List<String> getRequestedPermissions();' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageRead.java
grep -n -F 'List<String> getImplicitPermissions();' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageRead.java
grep -n -F 'private ArrayMap<String, PermissionData> mPermissions;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'mPermissionManager.addAllPermissionGroups(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.addAllPermissions(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 3. 扫包先收 group，再把 tree 与 leaf 送进 BasePermission

包提交阶段先调用 `addAllPermissionGroups()`，再调用 `addAllPermissions()`。这个顺序让现代包的 permission 在注册时能把自己的 raw group 名链接到当前 `ParsedPermissionGroup`。

group 的冲突规则很窄：Map 尚无同名项，或者已有项的 `packageName` 与新声明相同，才执行覆盖；另一个包的同名声明只记录警告并忽略。这里没有“system package 自动优先抢 group”的分支。

r48 更值得注意的是恢复与删除都不对称。group 没有写入 `packages.xml`，主要靠启动扫包重建；运行期的 `removeAllPermissions()`遍历 `pkg.getPermissions()` 和 requested permissions，处理普通定义、tree 绑定以及 AppOp 请求索引，却不遍历 `pkg.getPermissionGroups()`，`PermissionSettings` 也没有对应的 group remove helper。于是来源包卸载、或升级后删掉 group 声明时，旧 `ParsedPermissionGroup` 与旧 owner 可能继续留在本次 system_server 生命周期里：

- `getPermissionGroupInfo()` 仍可能查到旧元数据；
- 另一个包的同名 group 仍会被当作冲突；
- 冷启动重新构造服务并扫包后，Map 才通常按当前安装集合重建。

permission 与 group 的绑定也有版本门。targetSdk 大于 L MR1 才从 `mPermissionGroups` 查链接；旧包故意跳过这一步，避免早期没有现代组授权语义时出现意外组级行为。现代包写了一个当前未知的非空 raw group 名，服务只在调试条件下记录信息，叶子定义仍可继续注册。

这带来两个查询边界：未知 group 不等于未知 permission；raw group 非空却找不到 Map 项，也不等于 raw group 为 null。第 11 节会看到，`queryPermissionsByGroup(null)`只选后一类，不会把前一类当作“无组”。

随后 `addAllPermissions()`遍历 permission 与 permission-tree。每项先清掉解析对象上的 `FLAG_INSTALLED`，再按 `p.isTree()`选择 `mPermissionTrees` 或 `mPermissions`，交给 `BasePermission.createOrUpdate()`仲裁。只有解析定义真正绑定时才重新加 `FLAG_INSTALLED`。

所以 `FLAG_INSTALLED` 表示“这条解析定义被当前全局索引接受”，不是 APK 安装整体成功，更不是某个请求方已经 grant。甚至 Map 中存在同名 `BasePermission` 也不足以证明该 flag 已设置，因为 r48 会留下 `perm == null` 的骨架。

### 练习 2：核对 group 的先后顺序与缺失的删除对称性

先按 PackageManagerService 的调用顺序读，再比较 group 的 put 与 `removeAllPermissions()`真正遍历的集合。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mPermissionManager.addAllPermissionGroups(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.addAllPermissions(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'private void addAllPermissionGroups(AndroidPackage pkg, boolean chatty) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.mPermissionGroups.put(pg.getName(), pg);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private void removeAllPermissions(AndroidPackage pkg, boolean chatty) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (pkg.getTargetSdkVersion() > Build.VERSION_CODES.LOLLIPOP_MR1) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'p.setParsedPermissionGroup(mSettings.mPermissionGroups.get(p.getGroup()));' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'p.setFlags(p.getFlags() & ~PermissionInfo.FLAG_INSTALLED);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.putPermissionTreeLocked(p.getName(), bp);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.putPermissionLocked(p.getName(), bp);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

## 4. 同名定义按 owner 仲裁，敏感变化把撤销推到异步尾部

`BasePermission.createOrUpdate()`处理静态 permission 与 tree 的同名仲裁。先把两个维度拆开：

- `sourcePackageName` 是定义来源；
- `perm` 是否为 null 表示完整解析对象是否已经绑定。

若已有定义属于另一个包，普通第三方声明不能夺权。系统包可以接管尚未绑定的 `TYPE_BUILTIN`，也可以覆盖已绑定的非 system owner。还有一条更宽的空壳边界：只要 `bp.perm == null`，代码就把 `currentOwnerIsSystem`视为 false；因此另一 system 包也能覆盖这个 skeleton，即便其 `sourcePackageName`字符串恰好指向 system 包。只有已绑定的 system owner 才不会仅因后来者“也是 system”而被覆盖。

若不存在旧对象，方法先创建 `TYPE_NORMAL` 的 `BasePermission`。只有来源匹配、且没有落进 foreign tree，才绑定传入的 `ParsedPermission`、写 UID、更新 source，并给解析对象加 `FLAG_INSTALLED`。这意味着“创建包装对象”早于“确认解析定义可安装”，也是下一节 foreign-tree 空壳问题的根源。

当本轮绑定后的定义是 runtime permission，且发生 owner 替换，或绑定前的 `BasePermission`被视为非 runtime，`mPermissionDefinitionChanged` 会被置位。新建对象默认 protection 是 signature，所以首次加入的 runtime 定义也满足后一条件；这不只表示“旧定义从 non-runtime 改成 runtime”。`addAllPermissions()`把名字汇总，包提交后才在 `AsyncTask` 中做全局撤销，避免回调、kill 与 `mPackages` 锁形成死锁。

撤销遍历包快照与所有用户，但它不是无条件清空：

- 当前定义必须仍是 runtime；
- UID 小于 `Process.FIRST_APPLICATION_UID` 的主体跳过；
- 只有当前已 grant 且未命中保护 mask 才撤销；
- mask 是 `SYSTEM_FIXED | POLICY_FIXED | GRANTED_BY_DEFAULT | GRANTED_BY_ROLE`。

`USER_SET` 不在这个 mask 里。原因是用户曾经同意的是旧 owner 或旧语义，不能把旧同意无条件转给变成 dangerous 的新定义。每项处理完成后才把 definition-changed 标记清掉。

完成点也因此分裂：包和定义可以已经提交、`FLAG_INSTALLED` 已经可见，而历史 runtime grant 的撤销任务仍在异步队列里。包安装 API 返回不能被解释成“所有定义变化引发的跨包撤销已经结束”。

## 5. permission tree 用点号边界和 appId 管名字空间，但 r48 会留下 foreign 空壳

Manifest 的 `<permission-tree>`声明可扩展名字空间。解析器要求名字至少有三段，例如 `com.example.feature`；`com.example` 只有两段，不能成为 tree。tree 自身不是用户可授权的叶子。

叶子匹配不是普通 `startsWith`。`findPermissionTree()`还要求候选更长，且 tree 名之后的第一个字符是点：

| tree | 候选 | 结果 |
|---|---|---|
| `com.example.feature` | `com.example.feature` | 不匹配，候选没有更长 |
| `com.example.feature` | `com.example.feature.read` | 匹配 |
| `com.example.feature` | `com.example.features.read` | 不匹配，边界不是点 |

管理权按身份而非字符串包名判断。`enforcePermissionTree()`比较 `tree.uid` 与 `UserHandle.getAppId(callingUid)`，所以跨用户的同一 appId、以及 shared UID 的其他成员可能通过这道门。安全审计要查共享 UID 关系，不能只看哪个 APK 声明了 tree。

集合遍历遇到第一个匹配 tree 就返回，并不选择最长前缀。若索引被异常的嵌套 tree 污染，迭代顺序可能影响命中的 owner；代码没有为“最具体 tree 优先”提供保证。

更重要的是，r48 对 foreign Manifest 声明的处理不能概括为“完全忽略”。控制流是：

1. 旧 Map 没有同名项时，先创建 `TYPE_NORMAL` 的 `BasePermission`；
2. 找到 foreign tree 后，拒绝把传入的 `ParsedPermission` 绑定给它；
3. `createOrUpdate()`仍返回这个 `perm == null` 的对象；
4. `addAllPermissions()`无条件把返回对象放进普通 permission 或 tree Map。

于是 tree 阻止的是 foreign 解析定义成为有效定义，却没阻止名字索引被占位。普通叶子空壳会让合法 tree owner 随后的 `addPermission()`看到“已有且非 dynamic”而抛错；foreign 的 nested tree 空壳还可能影响 first-match 查找。只要 skeleton 的 source package 仍非 null，它还可被 `BasePermission.writeLPr()`写进 Settings，并不只污染当前进程。这是 r48 的实现缺口，不是 permission-tree 合同提供的能力。

判断现场时至少同时检查：Map 是否有 key、`bp.perm` 是否非 null、解析对象是否有 `FLAG_INSTALLED`、source 与 tree owner 是否一致。只 dump 出名字不足以证明名字空间没有被空壳污染。

### 练习 3：手算 tree 边界并找到 foreign 空壳

对 `com.demo.sensor`、`com.demo.sensor.read`、`com.demo.sensors.read` 手算匹配结果，再沿“先 new、后拒绝绑定、最后无条件 put”的顺序解释占位是怎样发生的。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return input.error("<permission-tree> name has less than three segments: "' frameworks/base/core/java/android/content/pm/parsing/component/ParsedPermissionUtils.java
grep -n -F 'private static BasePermission findPermissionTree(' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F "permName.charAt(bp.name.length()) == '.')" frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'if (bp.uid == UserHandle.getAppId(callingUid)) {' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'bp = new BasePermission(p.getName(), p.getPackageName(), TYPE_NORMAL);' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'final BasePermission tree = findPermissionTree(permissionTrees, p.getName());' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'mSettings.putPermissionTreeLocked(p.getName(), bp);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.putPermissionLocked(p.getName(), bp);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '} else if (!bp.isDynamic()) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

## 6. addPermission 先改内存，true/false 只区分新增与更新

`ApplicationPackageManager`最终把动态添加交给 `PermissionManagerService.addPermission(info, async)`。服务端依次做这些门控：

1. instant app 一律拒绝；
2. `labelRes` 与 `nonLocalizedLabel` 至少有一个；
3. 名字必须落在调用 appId 拥有的 tree 下；
4. 同名项不存在时检查容量并创建 `TYPE_DYNAMIC`；
5. 同名项存在时必须已经是 dynamic，不能借 API 改静态定义；
6. 调用 `addToTree()`，必要时写 Settings。

新建项的 source 不取调用者提交的 `PermissionInfo.packageName`，而取 tree 的 `sourcePackageName`。这防止调用者靠 Parcelable 字段伪造新定义来源。重定义已有 dynamic 时，`addToTree()`只更新解析对象与 UID，不刷新既有 `sourcePackageName`；所有权门本身仍是 appId 级，shared UID 成员可共享管理能力。

返回值只回答“这次是否新建”：`added = bp == null`，最后直接 `return added`。因此：

| 返回 | 含义 | 不代表 |
|---|---|---|
| `true` | 之前没有同名项，本次走新增分支 | 已经安全落盘 |
| `false` | 之前已有 dynamic，本次走更新分支 | 失败、无变化、没有安排写盘 |

是否实际变化由另一个局部量 `changed` 决定。无论同步还是 async，内存对象都先在锁内更新；只有 `changed` 为 true 才调用 `writeSettings(async)`。所以 async 异步的是 Settings 写入安排，不是 Map 变更。

`PermissionInfo.fixProtectionLevel()`也不是通用合法性验证器。r48 只做两件事：把弃用的 `SIGNATURE_OR_SYSTEM` 转成 `SIGNATURE | PRIVILEGED`，以及在没有 `PRIVILEGED` 时剥掉孤立的 `VENDOR_PRIVILEGED`。其他 bit 组合原样返回。不能把它描述成 Manifest parser 那套完整的保护位合法性审查。

同步写失败也没有围绕内存变更做事务回滚；async 返回时则还存在进程异常退出前未持久化的窗口。调用成功、当前进程可查询、`packages.xml` 已 fsync 是三个不同完成点。

## 7. r48 addToTree 丢掉 leaf info，容量与 async 又制造三条边界

按 API 直觉，`addToTree()`应复制调用者的 leaf `PermissionInfo`，规范 protection 后，用它与 tree owner 拼成叶子 `ParsedPermission`。r48 实际代码却在复制后丢掉了副本：

1. 先用当前 `perm`、tree 和传入的 `info`计算 `changed`；
2. 把 `info`复制并修改其 protection；
3. 没有再消费这个副本；
4. 直接执行 `perm = new ParsedPermission(tree.perm)`；
5. 把 `uid`设为 tree UID。

`BasePermission.name` 仍是叶子名，但 `generatePermissionInfo()`在 `perm != null` 时从 `perm`生成结果。因此刚添加后，查询可能继承 tree 解析对象的名字、图标、label、protection 等字段，而不是提交的叶子信息；tree 的 group 通常仍是 null。比较阶段又拿 tree copy 与 leaf info 比 name，后续更新很容易一直得到 `changed == true`。

这还让“参与校验的对象”与“最终保存的对象”错位。`comparePermissionInfos()`考虑 icon、logo、protection、name、非资源 label 和 packageName；group、description、资源 label 等字段本来就没有纳入一致性比较。复制对象被丢弃后，即便参与比较的 leaf 字段看起来完整，也不能据此推断它们进入了 `ParsedPermission`。

容量上限 `MAX_PERMISSION_TREE_FOOTPRINT` 是 32768，但要加四个限定：

- 只在新增分支检查，更新分支不再检查；
- system UID 的 tree 免检；
- 当前量遍历整个 `mPermissions`，按与 tree 相同 UID 汇总，不只统计本 tree 前缀，也不只统计 dynamic；
- `BasePermission.calculateFootprint()`只在两者 UID 相等时计入，再加 BasePermission name 与已绑定 `perm.calculateFootprint()`。

`PermissionInfo.calculateFootprint()`本身只数 name、`nonLocalizedLabel`、`nonLocalizedDescription` 的字符长度。由于 r48 实存的是 tree copy，不能声称“更新 leaf 的长 label 就会把已存 footprint 撑大”；更精确的结论是：新增时校验提交对象，当前量统计已存解析对象，而最终又保存 tree copy，三处数据模型不一致。

普通运行期里，同 UID 的多棵树共享预算；shared UID 成员也共享。第 9 节还会看到，对受 cap 约束且 tree UID 非 0 的 owner（典型是第三方 appId），重启读出的 dynamic 骨架 UID 为 0，旧项会暂时漏出统计，从而让 32768 不是跨重启稳定执行的累计上限。

写入时机是另一条边界。`async=false` 走同步 Settings 写；`async=true` 进入 `scheduleWriteSettingsLocked()`，r48 的调度延迟常量是 10 秒。该异步消息自身延迟约十秒，也可能被同类请求合并；其他同步 Settings 写则可能更早把当前状态一并落盘。十秒是安排策略，不是持久化 SLA。

### 练习 4：把新增、更新、存储对象和容量对象分开

读完后画四列：调用者 `info`、用于比较的对象、写进 `bp.perm` 的对象、用于当前 footprint 的对象。再说明 `true`、`false` 与 `changed` 的关系。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public boolean addPermission(PermissionInfo info, boolean async) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (info.labelRes == 0 && info.nonLocalizedLabel == null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'added = bp == null;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'enforcePermissionCapLocked(info, tree);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'changed = bp.addToTree(fixedLevel, info, tree);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'info = new PermissionInfo(info);' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'perm = new ParsedPermission(tree.perm);' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'return perm.name.length() + perm.perm.calculateFootprint();' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'if (curTreeSize + info.calculateFootprint() > MAX_PERMISSION_TREE_FOOTPRINT) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(async);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'return added;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'static final int WRITE_SETTINGS_DELAY = 10*1000;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 8. removePermission 的类型判断反向，返回也不等于能力已撤

`removePermission()`先拒绝 instant app，再按 calling appId 强制校验 tree。查不到同名普通 permission 时直接返回。公开合同意在删除动态叶子，照理应在“不是 dynamic”时拒绝。

r48 却写成：当 `bp.isDynamic()` 为 true 时打印“Not allowed to modify non-dynamic permission”的 `wtf`，既没有抛异常，条件与文案也相反；接下来不论类型都从 `mPermissions`删除并同步写 Settings。只要调用者拥有覆盖该名字的 tree，这条路径甚至可能移除树下静态叶子的定义索引。

同步写只缩短磁盘窗口，不补齐授权清理。这个方法没有：

- 遍历包与用户撤销 runtime grant；
- 清 install grant；
- 调用 permission listener；
- 进入 `updatePermissionSourcePackage()`那套逐包 grant 状态清理与 callback 流程。

因此 remove 的即时效果只是“定义 Map 已删，并发起同步 Settings 写”，不等于即时 revoke。普通非 instant 包的 `checkSinglePermissionInternal()`先查 `PermissionsState.hasPermission(name, userId)`，命中后直接返回 true，并不回查 `mPermissions`里是否仍有定义。旧 grant 在本次开机内可能继续通过权限检查；若同名定义又被添加，按名字保留的状态还可能重新对应到它。

instant app 是例外：状态命中后还会查询定义是否带 INSTANT 属性，定义已删时无法通过那一步。冷启动也不同：读取 install 或 runtime 状态时若找不到全局 `BasePermission`，Settings 会把它作为 unknown permission 跳过。所以不能把内存残留外推成必然跨重启保留。

把三个删除路径分开看会更清楚：

| 路径 | 删定义 | 直接逐包撤销 | 写盘 | 主要风险 |
|---|---:|---:|---|---|
| 动态 API `removePermission()` | 是 | 否 | 同步 | 当前 boot 内状态残留 |
| source package 更新/删除 | 是 | runtime 会遍历；install 会清状态 | 由更新链处理 | legacy runtime 包还有跳过条件 |
| 冷启动读状态遇到未知定义 | 定义本来就无 | 不把未知记录载入 | 随后新状态再写 | 不是即时清理证据 |

### 练习 5：证明“定义已删”与“检查已拒绝”不是同一时刻

沿 remove、普通检查、状态 Map、冷启动两种 XML 读取依次核对。不要只看警告文案，要看它后面有没有 `throw`。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void removePermission(String permName) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Slog.wtf(TAG, "Not allowed to modify non-dynamic permission "' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.removePermissionLocked(permName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(false);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private boolean checkSinglePermissionInternal(int uid,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!permissionsState.hasPermission(permissionName, UserHandle.getUserId(uid))) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'PermissionData permissionData = mPermissions.get(name);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
grep -n -F 'void readInstallPermissionsLPr(XmlPullParser parser,' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'Slog.w(PackageManagerService.TAG, "Unknown permission: " + name);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'Slog.w(PackageManagerService.TAG, "Unknown permission:" + name);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
```

## 9. packages.xml 只保存骨架，普通 boot 不保证 dynamic 重新绑定

`Settings.writeLPr()`在 `packages.xml` 顶层分别写 `<permission-trees>` 与 `<permissions>`，二者最终都调用 `BasePermission.writeLPr()`输出 `<item>`。这份 XML 保存的是全局定义骨架，不是每用户 runtime grant；同一文件中其他 permissions 节点还可能保存 install 状态，不能仅凭标签同名就混为一层。

普通 `BasePermission` item 的核心字段是 name、source package，以及 protection 非 normal 时的数值。dynamic 项在有 `perm` 或 pending 信息时再写 `type="dynamic"`，并可附 icon、非资源 label。group、资源 label、description 等并没有形成完整 `PermissionInfo` 快照。

r48 的 `addToTree()`问题会继续传到磁盘：叶子 item 的 name 来自 `BasePermission.name`，但 icon 与 label 优先从 `bp.perm`取；此时 `perm`是 tree copy，所以 XML 可能出现“叶子 key + tree 展示字段”的混合记录。调用者提交但没有进入 `perm` 的 leaf 元数据无法靠这次写入找回。

读取 dynamic item 时，`readLPw()`创建 `TYPE_DYNAMIC` 的 `BasePermission`，把 name、source 与 protection 放进骨架，把 icon/label 放入 `pendingPermissionInfo`。这时 `perm == null`、`uid == 0`。查询仍可能成功，因为 `generatePermissionInfo()`会合成一个最小对象，但只放：

- `name = BasePermission.name`；
- `packageName = sourcePackageName`；
- `nonLocalizedLabel = name`；
- 当前 `protectionLevel`。

这个 fallback 不读取 pending icon/label。因此“`getPermissionInfo()`非 null”仍不能证明 dynamic 已重建。

设计上的重建入口是 `updateDynamicPermission()`：找到覆盖叶子的有效 tree 后，以 tree 的 `ParsedPermission`为底，结合 pending 信息创建叶子对象并恢复 tree UID。不过构造器只从 pending 覆盖 flags、description resource、background permission、group、request resource 和 protection；icon 与 `nonLocalizedLabel`仍留用 tree 的值。XML 即便读到了这两个 pending 字段，最终绑定也没有消费它们。

更关键的是普通 boot 控制流有早退。`updateAllPermissions()`调用 `updatePermissions(null, null, volumeUuid, flags, callback)`；随后 `updatePermissionSourcePackage()`见 `packageName == null`立即返回 true，循环里的 `bp.updateDynamicPermission(...)`根本没有执行。所以 r48 不能承诺“完成全量开机权限更新后，所有 dynamic 骨架都已绑定”。

后续任意一次 package-scoped update 会先遍历每个 dynamic 项并尝试重建，然后才按 source package 过滤；只有 pending 非 null、且此时能匹配到已绑定 tree 的项才可能补上绑定。这个时机依赖后续包事件，不是普通 boot 的固定完成点。

容量漏洞也由此产生。对受 cap 约束且 tree UID 非 0 的 owner，重启后的旧 dynamic 骨架 UID 为 0，而 footprint 只在 `tree.uid == perm.uid`时计入旧项。若 owner 在任何 package-scoped 重建发生前再新增一批叶子，旧批次不会占用本次 32768 预算；反复重启并在这个窗口添加，可以累计突破设计上看似全局的上限。SYSTEM_UID 1000 才是 cap 豁免者，不能与 root UID 0 混称。

换句话说，`packages.xml` 证明的是“骨架曾被持久化”，不是“完整 leaf 元数据已恢复”，也不是“容量在跨重启期间持续正确累计”。

### 练习 6：沿 XML 读写走到普通 boot 的 early return

先列出写入字段，再从 `pendingPermissionInfo`追到重建入口，最后证明全量更新为何到不了它。额外比较 UID 条件，解释容量漏算。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'serializer.startTag(null, "permission-trees");' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mPermissions.writePermissionTrees(serializer);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'mPermissions.writePermissions(serializer);' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'final boolean dynamic = "dynamic".equals(ptype);' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'bp.pendingPermissionInfo = pi;' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'serializer.attribute(null, "type", "dynamic");' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'int icon = perm != null ? perm.getIcon() : pendingPermissionInfo.icon;' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'permissionInfo.nonLocalizedLabel = name;' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'perm = new ParsedPermission(tree.perm, pendingPermissionInfo,' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'this.flags = pendingPermissionInfo.flags;' frameworks/base/core/java/android/content/pm/parsing/component/ParsedPermission.java
grep -n -F 'this.protectionLevel = pendingPermissionInfo.protectionLevel;' frameworks/base/core/java/android/content/pm/parsing/component/ParsedPermission.java
grep -n -F 'updatePermissions(null, null, volumeUuid, flags, callback);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private boolean updatePermissionSourcePackage' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (packageName == null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'bp.updateDynamicPermission(mSettings.mPermissionTrees.values());' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (uid == perm.uid) {' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
```

## 10. owner 更新、adopt 与悬空清理是三条不同路径

正常新建的 dynamic 叶子，其 `sourcePackageName`取 tree owner；已有 dynamic 更新不刷新 source，因此异常旧对象还可能保留别的值。以下讨论正常新建项：package-scoped update 处理该 owner 时，服务先尝试重建所有 dynamic，再对 source 与变化包相同的定义调用 `hasPermission(pkg, bp.getName())`。这个 helper 精确遍历新 APK 的 Manifest permission 列表。

API 创建的叶子通常只在运行时存在，并不会同时写回 APK Manifest。于是 owner 升级时，即使 tree 仍声明着，叶子名也常常不在新包的静态列表里，随后被视为“不再声明”，进入撤销与移除分支。tree 存在只是 dynamic 存活的必要条件，并非充分条件。

顺序还会让行为看起来不稳定：无关包的 package-scoped update 能先把 XML 骨架重建，因为 dynamic 更新发生在 source 过滤前；但 source 不匹配时不会删除。轮到 tree owner 自己更新，同一叶子又可能因 Manifest 精确检查被删。“能跨一次重启”不能外推成“能跨 owner 升级”。

普通 source 删除走的是更完整的定义清理：runtime 定义会遍历包与用户尝试 revoke，非 runtime 定义会清 install 状态，再移除 Map 项。即便如此也有范围条件，例如 runtime helper 会跳过 targetSdk 小于 M 的 legacy App。因此这条路径也不能简单表述成对所有历史状态无条件清空。

tree 自己有另一套 `updatePermissionTreeSourcePackage()`。第一轮在变化包不再声明 tree 时可直接 `iterator.remove()`；第二轮检查悬空 source 时却调用 `removePermissionLocked(bp.getName())`，删的是普通 permission Map，而不是 `removePermissionTreeLocked()`。多数匹配变化包的正常场景已经在第一轮移除，但这个错误调用意味着不能保证所有悬空 tree 都由第二轮可靠清掉，还可能误删同名普通索引。

`<adopt-permissions>`又不是上述更新的别名。r48 的门槛是：

1. 非 system package 在扫描策略阶段会被清空 adopt 列表；
2. 旧 `PackageSetting` 必须存在；
3. `verifyPackageUpdateLPr()`要求旧包曾带 `FLAG_SYSTEM`；
4. 旧包此刻不能仍在 `mPackages`。

这个 helper 本身没有验证新旧包签名或 signing lineage。可信边界主要来自 system image 与整个扫描/安装链，不能把一个只检查“旧包为 system 且已消失”的函数称为完整签名更新证明。

通过后，`PermissionSettings.transferPermissions()`只循环 `mPermissionTrees` 与 `mPermissions`。对匹配 source 的 `BasePermission`，它改成新包名，清 `perm`、UID、GID，并修改 pending 的 packageName；它不迁移 `mPermissionGroups`，也不搬运请求包的 `PermissionsState` grant 表。

新 owner 还必须在自己的 APK 中重新声明相同 permission/tree 名，后续扫描才会把 `ParsedPermission` 与 UID 绑定回来；adopt 标签本身不合成声明。未重新声明的骨架仍可能被 source 清理删除。与此同时，旧 group 在当前 system_server 中可能继续归旧包，阻塞新包的同名 group，通常要等冷启动重建 Map 才有机会纠正。

可以把三条路径记成：

| 路径 | 触发 | 主要操作 | 不保证 |
|---|---|---|---|
| source update/delete | 定义来源包变化 | 校验声明、撤销部分状态、移除定义 | 修复所有 tree 悬空异常 |
| adopt | 新 system 包声明接管旧包 | 改 definition/tree source 并清绑定字段 | 迁 group、迁 grant、合成新声明 |
| dynamic API remove | owner 主动删叶子 | 直接删普通定义并写盘 | 即时撤销现存 grant |

### 练习 7：区分 owner 升级、tree 清理与 adopt

给一个 API 创建的 leaf 推演三次事件：无关包更新、owner 更新、新 system 包 adopt。每次都写出 source、`perm`、UID、group owner 和 grant 是否改变。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'bp.updateDynamicPermission(mSettings.mPermissionTrees.values());' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (pkg == null || !hasPermission(pkg, bp.getName())) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static boolean hasPermission(AndroidPackage pkg, String permName) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mSettings.removePermissionLocked(bp.getName());' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!pkg.getAdoptPermissions().isEmpty()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (verifyPackageUpdateLPr(orig, pkg)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mSettings.mPermissions.transferPermissions(origName, pkg.getPackageName());' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'public void transferPermissions(String origPackageName, String newPackageName) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'i == 0 ? mPermissionTrees : mPermissions;' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionSettings.java
grep -n -F 'public void transfer(@NonNull String origPackageName, @NonNull String newPackageName) {' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F 'pendingPermissionInfo.packageName = newPackageName;' frameworks/base/services/core/java/com/android/server/pm/permission/BasePermission.java
grep -n -F '.clearAdoptPermissions();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 11. PermissionInfo 查询可返回最小定义，group 查询还有 null/empty 分界

`getPermissionInfo(name, flags)`经 `ApplicationPackageManager`带上调用包名进入服务端。instant caller 直接得到 null，客户端公开 API 再把缺失结果转成 `NameNotFoundException`；普通调用者则从 `mPermissions`找 `BasePermission`。Map 没有名字时返回 null，Map 有骨架但 `perm == null`时会返回上一节的最小 `PermissionInfo`。

所以诊断动态恢复时，应把结果分成三类：

| 服务端状态 | 查询结果 | 可下的结论 |
|---|---|---|
| Map 无 key | null | 当前没有普通定义索引 |
| Map 有 key，`perm == null` | 最小对象 | 只有骨架，完整元数据未绑定 |
| Map 有 key，`perm != null` | 从解析对象生成 | 已绑定，但仍需检查 r48 tree-copy 异常 |

服务端还会按调用身份调整 protection flags。signature 基类始终返回完整位；system/root/shell 看完整位；合法的 pre-O 查询包只看 base bits。反直觉的是，packageName 找不到、PackageSetting 缺失或 appId 不匹配的若干分支会直接返回原始完整 `protectionLevel`，并非一律遮蔽或拒绝。这里应描述控制流，不应把 packageName 当成严格隐私鉴权凭据。

`queryPermissionsByGroup(groupName, flags)`有三种返回语义：

- 非 null 名字不在 `mPermissionGroups`：服务端返回 null，公开 API 表现为组不存在；
- 名字存在但没有成员：返回空列表；
- `groupName == null`：遍历普通定义，只选 `perm == null` 或 raw `perm.getGroup() == null`。

最后一条尤其容易误读。某 permission 的 raw group 是非空字符串，即使该 group 当前不在 Map，它也不会被 null 查询当成 ungrouped；反过来，任意 `perm == null` 的骨架都会进入 null 结果。group stale 又会影响第一道“组是否存在”门，因此查询结果同时暴露定义绑定状态和 group Map 生命周期，而不只是业务分组。

这些定义查询不经过第 15 节的两张 permission-check cache。看见 `PermissionManager`里有缓存，不能推断 `getPermissionInfo()`或 `queryPermissionsByGroup()`也复用了它们。

## 12. split 表来自 SystemConfig，阈值是小于而不是小于等于

split permission 与 dynamic permission 只是名字里都有 permission，机制完全不同：前者是一张平台兼容迁移表，后者是 tree owner 运行时创建定义。split 表不现场向 `mPermissions`创建新定义，也不直接写 grant。

`SystemConfig`从允许的分区 XML 读取 `<split-permission>`。system、product、system_ext 可按各自 allow flags 提供相关配置；vendor/odm 只有 `FIRST_SDK_INT <= O_MR1` 时为兼容性加上 `ALLOW_PERMISSIONS`，OEM 的 allow 集合不含这类 permissions 配置。分析厂商镜像不能只搜到 XML 标签，还要确认该目录的 `permissionFlag`允许它生效。

每条记录有旧 permission、若干 new permissions 与 targetSdk 阈值。读取规则是：

- 缺 name：跳过；
- targetSdk 缺失：使用 `CUR_DEVELOPMENT + 1`；
- targetSdk 不是整数：跳过整条；
- new-permission 空名：忽略该子项；
- 最终 new 列表为空：不加入运行时表。

AOSP r48 主表里有这些直观例子：

| old | new | threshold | 生效目标 |
|---|---|---:|---|
| `READ_CONTACTS` | `READ_CALL_LOG` | 16 | target 15 及以下 |
| `ACCESS_FINE_LOCATION` | `ACCESS_BACKGROUND_LOCATION` | 29 | target 28 及以下 |
| `ACCESS_COARSE_LOCATION` | `ACCESS_BACKGROUND_LOCATION` | 29 | target 28 及以下 |
| `READ_EXTERNAL_STORAGE` | `ACCESS_MEDIA_LOCATION` | 29 | target 28 及以下 |
| `WRITE_EXTERNAL_STORAGE` | `READ_EXTERNAL_STORAGE` | 默认值 | 当前正常 target 基本都满足 |

parser 的判断是 `targetSdk >= threshold`就跳过，等价于只在 `targetSdk < threshold`时应用。threshold 为 29 时，target 28 会补，target 29 不会；不是 `<= 29`。

`SystemConfig`是 boot 时构建的 singleton，其 getter 直接返回内部可变 `ArrayList`；PermissionManagerService 每次 Binder 查询再把当下内容转换成 Parcelable 列表。配置表基本以 system_server boot 生命周期为边界；客户端是否缓存，以及 parser 是否走这层缓存，是另一回事。

## 13. parser 只补 requested/implicit，且 live 列表允许级联

新解析路径在包解析收尾先调用 `convertNewPermissions(pkg)`，紧接着调用 `convertSplitPermissions(pkg)`。legacy `PackageParser.Package`也保留独立的 `requestedPermissions`、`implicitPermissions`和对应转换；r48 正处在两套解析模型并存的阶段，定位日志时要先确认走了哪套对象。

`convertNewPermissions()`读取静态 `PackageParser.NEW_PERMISSIONS`。它处理“平台后来新增 permission”的兼容项；r48 包含 target 低于 DONUT 时补 `WRITE_EXTERNAL_STORAGE` 与 `READ_PHONE_STATE`。这张 Java 表不是 SystemConfig split XML。

随后 `convertSplitPermissions()`直接调用 `ActivityThread.getPermissionManager().getSplitPermissions()`取 Binder 列表，没有经过 `PermissionManager`实例方法及其 `mSplitPermissionInfos`缓存。每次解析都重新走这条 raw service 路径，不能用客户端实例缓存解释 parser 的性能或一致性。

对每条 split，parser 要同时满足：当前 target 小于阈值、requested 列表已经含 old。每个尚未存在的 new 名字会同时加入 requested 与 implicit；若 new 本来就是开发者显式请求，代码因已存在而不再加 implicit 标记。多个 old 指向同一 new 时也由 contains 去重。

注意它读取的是正在增长的 live requested 列表，不限于原始 Manifest。兼容步骤的先后与 `mSplitPermissions`的插入/遍历顺序因此允许级联；跨目录的普通 XML 顺序本身不应被当作统一稳定合同。以 stock `platform.xml`中的当前次序和极老 target 为例：

1. `convertNewPermissions()`先补 `WRITE_EXTERNAL_STORAGE`；
2. split 表由 WRITE 补 `READ_EXTERNAL_STORAGE`；
3. 后面的 split 又由 READ 补 `ACCESS_MEDIA_LOCATION`。

因此“Manifest 必须显式写 old permission”不是完整前提；前一个兼容规则补出的 old 也能触发后一个 split。

转换只修改内存包模型，APK 中的二进制 Manifest 没有变化。此刻也没有弹权限 UI、没有写 `PermissionsState`、没有直接 grant。requested 回答“系统把它视为请求”，implicit 额外回答“这不是开发者显式写入”；真正的历史状态迁移发生在权限恢复阶段。

## 14. grant 继承与 REVOKE_WHEN_REQUESTED 在 restore 阶段完成

`restorePermissionState()`遍历当前 requested permissions，在旧状态中未曾请求、且当前属于 implicit 的名字加入 `newImplicitPermissions`。ACTIVITY_RECOGNITION 还有独立升级兼容：即使当前不再 implicit，只要设备 split 表把它列为 new、且旧 source 曾是 install grant，也可能被加入。stock r48 的 `platform.xml`并没有这条 AR split，所以该分支通常要由允许分区上的设备附加配置触发。

`setInitialGrantForNewImplicitPermissionsLocked()`先把 split 表反转成“new → 全部 source 集合”。只有能找到 `sourcePerms`、且 new 当前不是 install permission，才进入逐用户 marker/继承逻辑。普通 new permission 会先设置 `FLAG_PERMISSION_REVOKE_WHEN_REQUESTED`，然后才判断有没有状态可继承。故这个 marker 表示“系统自动补入、将来显式请求时需重新裁决”的来源，不证明当下已经 grant。

继承有三个核心分支：

| source 状态 | new 的 grant | 继承函数额外并入的 flags |
|---|---|---|
| 任一 source 已 runtime/install grant | grant new | 所有已 grant source 的 flags 按位合并 |
| 无 source grant，但存在旧 source 状态 | 不 grant | denied source 的 flags 按位合并 |
| 旧状态没请求任何 source，当前也没有 source install grant | 不继承 | 只有此前写入的兼容 marker 等变化 |

ACTIVITY_RECOGNITION 不走普通 marker 设置。继承方法的注释还明确说它不处理 foreground/background permission 关系，所以不能仅凭通用 split 继承函数推断所有前后台联动都已解决。

当 App 升级后某名字不再 implicit，`revokePermissionsNoLongerImplicitLocked()`尝试退出兼容状态，但它不是“一变显式就一定撤销”：

1. 它遍历 `ps.getPermissions(userId)`，也就是当前 grant 的 install/runtime 名字；只有 denied 状态且带 marker 的项不会被访问，marker 可能残留。
2. install permission 不走 runtime revoke。
3. runtime grant 必须带 `FLAG_PERMISSION_REVOKE_WHEN_REQUESTED`。
4. 没有 `SYSTEM_FIXED | POLICY_FIXED | GRANTED_BY_DEFAULT`，且 targetSdk 已支持 runtime permissions，才真正 revoke。
5. 对进入“非 install 且带 marker”分支的 grant，无论是否因固定来源或 legacy target 而保留，都会清 marker；真正 revoke 时还会清用户设置/固定类 flags。install 状态不会进入这段 marker 清理。

这里的 blocking mask 不含 `GRANTED_BY_ROLE`，与第 4 节“定义变化撤销”的 mask 不同。相似的 revoke 场景不能共享一张 flags 速记表。

权限恢复结束后，callback 的 `onPermissionUpdated(...)`负责安排相关写入；runtime permission listener 则由后续独立的 `notifyRuntimePermissionStateChanged(...)`调用触发，不能合并成一个 callback。普通路径多为异步 runtime 写，shared-UID 的特定 revoke 路径才传同步标志。整个 parser→restore→write 链不是一个跨文件事务；解析模型已有 implicit、新 grant 已写内存、runtime 文件已落盘、客户端 cache 已观察到新结果，仍是四个时间点。

### 练习 8：从 split XML 推演到 grant 与退出 implicit

任选 `READ_EXTERNAL_STORAGE → ACCESS_MEDIA_LOCATION`，分别推演 target 28/29、old grant/deny、new 已显式请求/未请求。最后指出 marker 在 grant 判断前设置，以及哪些条件会阻止真正 revoke。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int targetSdk = Build.VERSION_CODES.CUR_DEVELOPMENT + 1;' frameworks/base/core/java/com/android/server/SystemConfig.java
grep -n -F 'final boolean allowPermissions = (permissionFlag & ALLOW_PERMISSIONS) != 0;' frameworks/base/core/java/com/android/server/SystemConfig.java
grep -n -F 'if (Build.VERSION.FIRST_SDK_INT <= Build.VERSION_CODES.O_MR1) {' frameworks/base/core/java/com/android/server/SystemConfig.java
grep -n -F 'case "split-permission": {' frameworks/base/core/java/com/android/server/SystemConfig.java
grep -n -F 'if (!newPermissions.isEmpty()) {' frameworks/base/core/java/com/android/server/SystemConfig.java
grep -n -F '<split-permission name="android.permission.READ_EXTERNAL_STORAGE"' frameworks/base/data/etc/platform.xml
grep -n -F '<new-permission name="android.permission.ACCESS_MEDIA_LOCATION" />' frameworks/base/data/etc/platform.xml
grep -n -F 'convertNewPermissions(pkg);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'convertSplitPermissions(pkg);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'splitPermissions = ActivityThread.getPermissionManager().getSplitPermissions();' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'if (pkg.getTargetSdkVersion() >= spi.getTargetSdk()' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'pkg.addRequestedPermission(perm)' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F '.addImplicitPermission(perm);' frameworks/base/core/java/android/content/pm/parsing/ParsingPackageUtils.java
grep -n -F 'newImplicitPermissions.add(permName);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'ArrayMap<String, ArraySet<String>> newToSplitPerms = new ArrayMap<>();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!newPerm.equals(Manifest.permission.ACTIVITY_RECOGNITION)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FLAG_PERMISSION_REVOKE_WHEN_REQUESTED,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!origPs.hasRequestedPermission(sourcePerms)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'inheritPermissionStateToNewImplicitPermissionLocked(sourcePerms,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'for (String permission : ps.getPermissions(userId)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '&& supportsRuntimePermissions) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static final int BLOCKING_PERMISSION_FLAGS = FLAG_PERMISSION_SYSTEM_FIXED' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (hasRuntimePermission(permission, userId)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionsState.java
```

## 15. split 列表缓存与两张 permission-check cache 的生命周期完全不同

`PermissionManager`里至少有三类看似相近、实则边界不同的缓存：

| 数据 | 位置与 key | 填充/失效 | 主要陷阱 |
|---|---|---|---|
| split 列表 | 每个 `PermissionManager`实例的 `mSplitPermissionInfos` | 首次成功 Binder 调用后保存，无显式失效 | 返回同一个可变列表，parser 不走它 |
| UID 检查 | 静态 16 项 `PropertyInvalidatedCache`，equals key 为 permission+uid | 共享 package-info property 失效 | 对象保存 pid，重算使用 pid，但相等性忽略 pid |
| 包名检查 | 静态 16 项，permission+package+源码字段 `uid` | 同一 property 失效 | 这个字段实际承载 `userId`，不是完整 UID |

split 实例缓存首调成功后把 Parcelable 列表转成新的 `ArrayList`，后续直接返回同一对象，没有同步、没有防御性 copy、没有失效入口。`SplitPermissionInfo.getNewPermissions()`也直接返回内部 Parcelable 的 list。调用者修改它们会污染同一 `PermissionManager`实例的后续观察。若首次 Binder 调用抛 `RemoteException`，方法返回空列表但不写字段，下次仍会重试。

这份缓存与 parser 无关。第 13 节已经看到，`ParsingPackageUtils`和 legacy `PackageParser`都直接拿 raw Binder 服务；它们不会命中某个 Context 持有的 `PermissionManager.mSplitPermissionInfos`。

UID 权限检查缓存的 `PermissionQuery`保存 permission、pid、uid，miss 时也把 pid 传给 AMS；但 `equals()`与 `hashCode()`明确忽略 pid。源码注释把它解释为实际安全检查按 uid，而 r48 后端却仍有 PID 语义：`ActivityManagerService.checkComponentPermission()`对 `pid == MY_PID`特殊放行，也会按 pid 查 `ProcessInfo.deniedPermissions`。因此同 permission+uid、不同 pid 的 uncached 结果并非理论上绝不可能不同，cache key 与后端输入之间存在真实缝隙。

PermissionManagerService 构造时先触发共享 package-info property 的系统级失效，随后只在 system_server 本进程禁用 permission cache 与 package-name cache。这能避免权威服务被自身旧值遮住，却不能证明其他客户端进程中忽略 pid 的键没有碰撞。

包名检查缓存的类字段叫 `uid`，但 `ApplicationPackageManager`传入 `getUserId()`，AIDL 与 PMS 参数也叫 `userId`。准确的 key 是 permission + packageName + userId；把第三项写成 calling UID 会与上一张缓存混淆。

两张检查缓存共享 `cache_key.package_info`，ApplicationInfo 与 PackageInfo cache 也使用这项 property。`PackageManager.invalidatePackageInfoCache()`通过 `AutoCorker`合并频繁失效；默认 2000ms cork 期间 property 处于 `NONCE_UNSET`，查询绕过缓存直接 recompute，最后 uncork 才建立新 generation。两秒不是“继续返回旧值的 TTL”。

所以观察权限变化时应问：调用走哪张 cache、key 是否包含所有后端语义、当前是否处于 cork bypass、哪个进程禁用了本地 cache。单凭“第二次调用很快”不能断定没有 Binder，也不能把 split 快照的无失效生命周期套到 permission checks 上。

### 练习 9：对照 split、UID 与 package-name 三种 cache

为每张 cache 写出作用域、key、miss 时的后端参数和失效方式；再用 AMS 的两个 PID 分支反驳“忽略 pid 必然安全”的过强结论。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mSplitPermissionInfos != null) {' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'mSplitPermissionInfos = splitPermissionInfoListToNonParcelableList(parcelableList);' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return mSplitPermissionInfoParcelable.getNewPermissions();' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'private static int checkPermissionUncached(@Nullable String permission, int pid, int uid) {' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'private static final PropertyInvalidatedCache<PermissionQuery, Integer> sPermissionCache =' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F "N.B. pid doesn't count toward equality!" frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'return uid == other.uid' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'new PropertyInvalidatedCache<PackageNamePermissionQuery, Integer>' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F '.checkPackageNamePermission(permName, pkgName, getUserId());' frameworks/base/core/java/android/app/ApplicationPackageManager.java
grep -n -F 'int checkPermission(String permName, String pkgName, int userId);' frameworks/base/core/java/android/permission/IPermissionManager.aidl
grep -n -F 'public static final String CACHE_KEY_PACKAGE_INFO = "cache_key.package_info";' frameworks/base/core/java/android/permission/PermissionManager.java
grep -n -F 'if (pid == MY_PID) {' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'ProcessInfo procInfo = sActiveProcessInfoSelfLocked.get(pid);' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'mInjector.disablePermissionCache();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mInjector.disablePackageNamePermissionCache();' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'sCacheAutoCorker.autoCork();' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int DEFAULT_AUTO_CORK_DELAY_MS = 2000;' frameworks/base/core/java/android/app/PropertyInvalidatedCache.java
grep -n -F 'if (currentNonce == NONCE_DISABLED || currentNonce == NONCE_UNSET) {' frameworks/base/core/java/android/app/PropertyInvalidatedCache.java
```

## 16. 用完成点矩阵收束定义、split、grant 与缓存

先把三条主线压成最短因果链。

**静态定义链**

Manifest 解析 → group 先注册 → tree/leaf 进入 `createOrUpdate()` → owner 与 tree 仲裁 → `perm`绑定且设置 `FLAG_INSTALLED`。此后分成并行语义支路：definition-change 撤销投递到 `AsyncTask`，包级权限更新/恢复继续推进，Settings 写入与客户端失效也各有完成点；源码不保证撤销与恢复形成一条严格全序链。

**动态定义链**

有效 tree 已绑定 → calling appId 通过所有权校验 → label/类型/新增容量检查 → Map 立即变化 → 同步执行写入或异步安排一次 `packages.xml` 写入 → 重启只读成 pending 骨架 → 普通全量 update 不保证重绑 → 某次 package-scoped update 可能重绑，也可能在 owner 更新时因 Manifest 没有叶子而删除。

**split 迁移链**

SystemConfig 读表 → parser 在 target 小于阈值时补 requested+implicit → `restorePermissionState()`发现新 implicit → 先写兼容 marker → 依据全部 source 的旧 grant/flags 决定继承 → 将来不再 implicit 时按 target 与 blocking flags 条件清理。

把常见日志或 API 返回放回完成点矩阵，就不容易越界推理：

| 观察 | 已经证明 | 尚未证明 |
|---|---|---|
| `mPermissions`有名字 | 全局索引有 `BasePermission` | `perm`已绑定、定义有效、有人 grant |
| 静态扫描对象的 `FLAG_INSTALLED`已置位 | 本次解析定义被接受 | 历史 grant 清理异步任务已完成 |
| `addPermission()`返回 true | 本次新建而非更新 | fsync 已完成、leaf 元数据正确 |
| `addPermission()`返回 false | 走已有 dynamic 更新分支 | 更新失败或没有 changed |
| 调用前同名 BP 存在且 `removePermission()`正常返回 | 定义已从 `mPermissions`删除，并完成同步 Settings 写调用 | 普通包残留 grant 已撤销 |
| `getPermissionInfo()`非 null | 至少有定义索引 | 不是最小骨架、不是 tree copy |
| parser 有 implicit | 兼容请求来源已记录 | 已 grant、已弹 UI、磁盘已写 |
| marker 存在 | 属于自动兼容来源 | 当前一定 grant |
| `PermissionsState` grant | 权限状态账允许 | 定义仍完整、AppOp 与其他门允许 |
| cache invalidation 入口已调用 | 若 cache 启用，则进入 nonce 变更或 cork bypass 流程 | 各客户端已在下一次查询观察到变化 |

r48 主线中最需要单独记住的源码异常有八个；group stale、pending 展示字段不被消费、adopt 不迁 group 等生命周期边界还应结合对应章节一起核对：

1. foreign tree 拒绝绑定后仍可能留下 `perm == null` 的名字空壳；
2. tree 查找取首个前缀匹配，而非最长匹配；
3. `addToTree()`复制 leaf info 后丢弃，实际保存 tree copy；
4. 新增 cap 的校验对象、当前统计对象和最终存储对象不一致；
5. `removePermission()`类型判断反向且没有阻止删除，也不直接撤 grant；
6. 普通 boot 的 null package early return 绕过 dynamic 重绑定，并产生跨重启容量漏算窗口；
7. owner 包更新会把未写进 Manifest 的 API dynamic leaf 当作不再声明；
8. tree 悬空清理第二阶段误调用普通 permission Map 的 remove。

现场排查一个名字时，按这个顺序最省时间：

1. 它在 `mPermissions`还是 `mPermissionTrees`，对象的 `perm`与 UID 是否已绑定？
2. raw group、当前 group Map 与 stale owner 是否一致？
3. 名字是否落入某个 tree，first-match 的 owner appId 是谁？
4. 目标包是 Manifest 显式请求、NEW_PERMISSIONS 补入，还是 split implicit？
5. 对应用户或 shared UID 的 `PermissionsState`是否 grant，marker/fixed flags 是什么？
6. 这次观察发生在 Map 修改、异步撤销、磁盘写入、dynamic 重绑和 cache uncork 的哪一个时间点？
7. 权限检查之后是否还有 AppOps、组件导出、进程 PID 特例等门？

至此，第267章得到的核心结论不是“permission 是一个字符串”，而是：同一个名字沿定义、所有权、请求、授权、持久化和缓存拥有多份生命周期不同的状态；只有把完成点说清，才能解释 r48 中那些看似矛盾的查询与检查结果。

下一章进入第268章，沿 `Context`、`PackageManager`、AMS 与 PMS 追踪 UID、shared UID、instant/isolated UID 和跨用户权限检查的完整裁决链。
