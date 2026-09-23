# Android权限组：AppPermissionGroup、前景/背景权限、组级 UI 与逐项状态链

## 1. 一个组级按钮为什么不能直接当成权限真相

本文固定在 AOSP `android-11.0.0_r48`：`frameworks/base` 对应提交 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/apps/PermissionController` 对应提交 `5a695c884d66c94d33b278dc626b4ed0c9ecd2ec`。第263章追过运行时请求，第264章解释了每用户权限文件，第265章又拆开 Default 与 Role 来源；本章补上中间那层：用户只点一次“允许”或“仅在使用中”，PermissionController 怎样把它投影为多项 permission、flags、AppOps 与一次性会话。

先给结论：Android 11 没有持久化一个“LOCATION 组当前等于 FOREGROUND_ONLY”的权威枚举。组是 UI 和策略聚合单位，底层仍按单项保存 grant 与 flags；有 AppOp 的权限还要叠加 UID 级 mode，前后台权限又组成有向关系。界面上的单选按钮只是这些状态的当前投影。

可以把一次选择拆成四个状态面：

| 状态面 | 典型内容 | 作用域 | 能证明什么 |
| --- | --- | --- | --- |
| PackageManager grant | 每项 permission 的 granted/denied | package 或 shared UID 的权限状态 | 是否具有基础权限资格 |
| permission flags | `USER_SET`、`USER_FIXED`、`ONE_TIME`、`DEFAULT`、`ROLE`、restriction 等 | 每用户、逐 permission | 决定来自谁、能否再问、是否受策略限制 |
| AppOps mode | `MODE_ALLOWED`、`MODE_FOREGROUND`、`MODE_IGNORED` | 本章路径调用 `setUidMode()`，按 UID 生效 | 把基础资格进一步收窄为前台、后台或不可用 |
| PermissionController 临时模型 | Java/Light 快照、`GroupState` | UI 进程 | 驱动当前交互，不是平台持久状态 |
| one-time session | `OneTimePermissionUserManager.PackageInactivityListener` | system_server，按用户、UID 跟踪 | 决定临时授权何时进入自动撤销链 |

因此“按钮变成已选中”“请求 Activity 返回”“PMS 内存已变”“两份文件均已落盘”是四个不同完成点。读源码时必须先问观察者读的是哪一层，再判断结论是否成立。

## 2. 请求弹窗与设置页使用两套模型

运行时 `requestPermissions()` 的弹窗主要走 Java 链：`GrantPermissionsActivity` 构造 `AppPermissions`，后者持有多个可变的 `AppPermissionGroup`，每组再持有多个 `Permission`。对象先保存 PackageManager 与 AppOps 的快照，grant/revoke 先改对象字段，再由 `persistChanges()`写回平台。

手持设备的单 App 权限设置页主要走 Kotlin 链：`PermStateLiveData` 产生逐项 `PermState`，`LightAppPermGroupLiveData` 组装不可变的 `LightPermission` 与 `LightAppPermGroup`，`AppPermissionViewModel` 把快照投影成按钮，`KotlinUtils` 直接调用 PackageManager 与 AppOps 并返回一个新的 Light 对象。LiveData 随后再观察平台通知并刷新。

| 场景 | 读取模型 | 修改入口 | 局部状态的寿命 |
| --- | --- | --- | --- |
| App 内运行时请求 | `Permission` / `AppPermissionGroup` / `AppPermissions` | Java `grantRuntimePermissions()`、`revokeRuntimePermissions()` | 当前 `GrantPermissionsActivity` |
| 手持设备设置页 | `PermState` / `LightPermission` / `LightAppPermGroup` | `AppPermissionViewModel.requestChange()` → `KotlinUtils` | 不可变快照，等下一次 LiveData 更新 |
| 电视、车载、Wear 与 legacy review | 仍可能复用 Java 或各自 UI | 各自 Fragment/handler | 不能拿手持页的顺序替它们背书 |

两套模型名称相似，读取合同却不相同。Java `Permission` 的 `isGrantedIncludingAppOp()`基于对象创建时读到的 raw AppOp 快照和后续局部字段修改，getter 本身不会再次查询 AppOps；r48 Light 对象的同名字段却只来自 grant 和 `REVOKED_COMPAT`。后文会专门解释这个最容易误诊的差异。

### 练习 1：先定位两套对象树

下面的命令只定位声明，不依赖行号。先把 Java 可变对象、Kotlin 不可变对象和两个 UI 控制器画成两列，再标出真正跨进程写入发生在哪一列。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public final class Permission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/Permission.java
grep -n -F 'public final class AppPermissionGroup' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'public final class AppPermissions' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'data class LightPermission(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/livedatatypes/LightPermission.kt
grep -n -F 'data class LightAppPermGroup(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/livedatatypes/LightAppPermGroup.kt
grep -n -F 'class PermStateLiveData private constructor(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/data/PermStateLiveData.kt
grep -n -F 'class LightAppPermGroupLiveData private constructor(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/data/LightAppPermGroupLiveData.kt
grep -n -F 'class GrantPermissionsActivity extends' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'class AppPermissionViewModel(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
```

完成标准：能说明 `GroupState`、Java `Permission`、Light `PermState` 与 system_server `PermissionsState` 不是同一个对象，也没有共同事务。

## 3. 平台分组来自 Utils，而不只来自 Manifest

权限的声明组与 PermissionController 的展示组必须分开看。r48 的 `AndroidManifest.xml` 虽然声明了 `android.permission-group.LOCATION`，但 FINE、COARSE、BACKGROUND LOCATION 三项自身的 `android:permissionGroup` 都是 `android.permission-group.UNDEFINED`。PermissionController 在 `Utils.PLATFORM_PERMISSIONS` 中把三项硬编码映射回 LOCATION，并反向构造 `PLATFORM_PERMISSION_GROUPS`。

`Utils.getGroupOfPermission()`先查询这张平台映射，只有不是平台表成员时才回退 `PermissionInfo.group`。`getPermissionInfosForGroup()`又把 PackageManager 的查询结果与平台表补出的权限合并，并在请求 `UNDEFINED` 时剔除已经有平台归组的条目。只看 Manifest 会错误地把位置权限当成无组权限。

第三方危险权限没有平台映射时，才以自己声明的 group 为准；没有 group 时，Java 模型把该 `PermissionInfo` 自身当成一个单项组。组名、label、图标与请求文案是展示元数据，不能替代逐项权限定义。

这一层还决定哪些项能进入模型：目标权限必须已安装、未 removed 且 base protection 为 dangerous。Java 创建组时只收目标包实际请求的危险权限；targetSdk 不高于 L MR1 的 App 若遇非平台自定义组，会被过滤，因为 legacy App 没有现代 runtime grant 合同可供这种组级 UI 操作。

### 练习 2：验证声明组与展示组的分叉

先读 Manifest 的三项声明，再读 PermissionController 的映射和回退规则。不要凭 XML 中的 `UNDEFINED` 直接下结论。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F '<permission-group android:name="android.permission-group.LOCATION"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F '<permission android:name="android.permission.ACCESS_FINE_LOCATION"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F 'android:permissionGroup="android.permission-group.UNDEFINED"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F 'PLATFORM_PERMISSIONS.put(Manifest.permission.ACCESS_FINE_LOCATION, LOCATION);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'PLATFORM_PERMISSIONS.put(Manifest.permission.ACCESS_COARSE_LOCATION, LOCATION);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'PLATFORM_PERMISSIONS.put(Manifest.permission.ACCESS_BACKGROUND_LOCATION, LOCATION);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'String groupName = Utils.getGroupOfPlatformPermission(permission.name);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'groupName = permission.group;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'permissions.addAll(getPlatformPermissionsOfGroup(pm, group));' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
```

完成标准：能解释为什么 LOCATION 组真实存在、三项却写 `UNDEFINED`，以及平台表和第三方声明各在何时生效。

## 4. AppPermissions怎样建立主组、背景子组与三张索引

`AppPermissions.loadPermissionGroups()`按目标包 `requestedPermissions` 遍历。若某项还没有出现在 permission→group 缓存，就以它创建整个展示组；创建过程再扫描同组 `PermissionInfo`，但只把目标包实际请求的危险项装入 `allPermissions`。因此它不是“系统组定义里的所有权限”，也不是“本次 request 数组里的权限”，而是“目标包 Manifest 已请求的本组危险权限快照”。

对象装好后有三张索引：

| 容器 | 内容 | 关键用途 |
| --- | --- | --- |
| `mGroups` | 只列主组 | 排序、页面枚举、延迟批量提交 |
| `mGroupNameToGroup` | group name → 主组 | 按 LOCATION 等展示组查主组 |
| `mPermissionNameToGroup` | permission name → 主组或背景子组 | 把本次具体请求路由到正确 `GroupState` |

前后台关系不是凭名字猜出来的。先收齐目标包请求的所有 `Permission`，再从每个前景项的 `backgroundPermission` 名字查找同一个 `allPermissions` 中的背景对象；找到后，前景对象指向背景对象，背景对象也保存前景列表。只有成功建立反向列表，`Permission.isBackgroundPermission()`才为真，随后该对象才会进入独立的 `mBackgroundPermissions` 子组。

这个条件带来一个重要边界：仅声明背景项而没有声明任何指向它的前景项时，Java 模型不会建立背景子组。正常 API 合同要求背景位置同时具备前景声明，但源码分析仍要把“平台合同”与“对象链接能否建立”分开。

主组与背景子组共享包、用户、组名和文案，却各有自己的 `mPermissions`。`mGroupNameToGroup`仍指向主组；permission 索引则能让 `ACCESS_BACKGROUND_LOCATION` 指向背景子组。调用主组 grant/revoke 不会自动遍历背景子组，调用者必须显式组合两次操作。

### 练习 3：重建对象链接和索引

分别推演 Manifest 只声明 FINE、声明 FINE+BACKGROUND、只声明 BACKGROUND 三种输入。最后核对哪一种会创建背景子组。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'for (String requestedPerm : mPackageInfo.requestedPermissions)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'if (getGroupForPermission(requestedPerm) == null)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'mGroupNameToGroup.put(group.getName(), group);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'mPermissionNameToGroup.put(perms.get(permNum).getName(), group);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'backgroundPermission.addForegroundPermissions(permission);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'permission.setBackgroundPermission(backgroundPermission);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'if (permission.isBackgroundPermission()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'group.mBackgroundPermissions = new AppPermissionGroup' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'addAllPermissions(backgroundGroup);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
```

完成标准：能从一项 permission 反查主组或背景子组，并能说明为什么仅有背景声明时双向链接不完整。

## 5. Java 有效态、Light 快照与组级 any 是三种口径

Java `Permission` 同时保存 raw grant、AppOp 名称、`mAppOpAllowed` 和 flags。`isGranted()`只返回 grant；`isGrantedIncludingAppOp()`要求 grant 为真、相关 AppOp 可用，并且没有 `REVIEW_REQUIRED`。对没有关联 AppOp 的权限，第二个条件自然放行。

Java 构造快照时，对普通或前景 permission，把 raw `MODE_ALLOWED` 与 `MODE_FOREGROUND` 都记为 `appOpAllowed=true`：前景项处于 `MODE_FOREGROUND` 本来就是有效的“仅使用中”状态。背景对象则在建立链接后重新看对应前景 AppOp，只有 `MODE_ALLOWED` 才表示后台范围有效。于是同一个 raw mode 对前景和背景的含义不同。

设置页的 Light 链不能照搬这个公式。`PermStateLiveData`只读取 `PackageInfo.REQUESTED_PERMISSION_GRANTED` 与 permission flags，并把 `REVOKED_COMPAT` 排除；它没有调用 AppOps，也没有排除 `REVIEW_REQUIRED`。随后 `LightPermission` 把这个布尔值命名为 `isGrantedIncludingAppOp`。这个名字描述了理想合同，不是 r48 数据来源的证明。

正常路径里 AppOpsService 会尝试用 `REVOKED_COMPAT` 同步某些 AppOp 偏差，所以 Light 结果经常与功能态一致；但诊断时仍不能依赖这种间接同步。若 mode 由另一条路径改变、监听尚未收敛，或 review flag 独立存在，设置页可能展示与 Java 模型不同的结果。

更上一层的“组已授予”又是 `any` 聚合：

- Java `areRuntimePermissionsGranted()`只要过滤范围内一项 `isGrantedIncludingAppOp()`为真就返回 true；
- Light 前景、背景子组的 `isGranted` 也是 `permissions.any`；
- Java `getFlags()`把成员 flags 做按位 OR，`isUserFixed()`、`isPolicyFixed()`、`isSystemFixed()`同样只要任一项命中；
- 特殊位置 provider/controller 的组状态还会被位置总开关或包启用态覆盖。

所以组级 true 不能推出“全组每项都已授予”，一个 OR 出来的 flags 数值也不对应任何真实单项。它们适合保守地决定按钮，不适合充当逐项审计记录。

### 练习 4：对比三种 granted

给同组两项设置不同 grant、AppOp 和 flags，分别手算 Java 单项、Light 单项和组级结果。特别测试 `grant=true + MODE_IGNORED + 无 REVOKED_COMPAT`。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return mGranted && (!affectsAppOp() || isAppOpAllowed()) && !isReviewRequired();' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/Permission.java
grep -n -F 'appOpsMode == MODE_ALLOWED || appOpsMode == MODE_FOREGROUND' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'permission.getAppOp(), packageInfo.applicationInfo.uid' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'val granted = packageFlags and PackageInfo.REQUESTED_PERMISSION_GRANTED != 0' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/data/PermStateLiveData.kt
grep -n -F 'permFlags and PackageManager.FLAG_PERMISSION_REVOKED_COMPAT == 0' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/data/PermStateLiveData.kt
grep -n -F 'this(pkgInfo, permInfo, permState.granted, permState.permFlags, foregroundPerms)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/livedatatypes/LightPermission.kt
grep -n -F 'if (permission.isGrantedIncludingAppOp()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'val isGranted = specialLocationGrant ?: permissions.any { it.value.isGrantedIncludingAppOp }' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/livedatatypes/LightAppPermGroup.kt
grep -n -F 'flags |= permission.getFlags();' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
```

完成标准：能指出哪个值包含真实 AppOp，哪个只依赖 compat flag，以及 any/OR 在哪里丢失成员精度。

## 6. 设置页按钮只是状态投影，特殊组还会改写文案

`AppPermissionViewModel`先把 Light 前景、背景子组投影为按钮，再叠加 fixed、目标 SDK、特殊能力和产品配置。常规位置组的核心矩阵如下：

| 设置页表象 | 前景 grant | 背景 grant | `ONE_TIME` | 正常一致态下的前景 AppOp |
| --- | --- | --- | --- | --- |
| 始终允许 | 至少一项 true | true | false | `MODE_ALLOWED` |
| 仅在使用中 | 至少一项 true | false | false | `MODE_FOREGROUND` |
| 本次会话仍有效 | true | false | true | `MODE_FOREGROUND` |
| 每次询问 | false | false | true | `MODE_IGNORED` |
| 拒绝 | false | false | false | `MODE_IGNORED` |

“每次询问”不是资源 API 每次访问都弹窗。它是 `denied + ONE_TIME` 的稳定态，App 下一次再调用 `requestPermissions()`时仍可进入询问；“仅本次”则是 `granted + ONE_TIME` 并有活跃 one-time session。设置页为后一种状态显示一个只读的 `ASK_ONCE` 选项，源码明确不为它安装点击动作。

fixed 也按子组聚合。前景和背景都 fixed 时按钮全禁；只固定背景 grant 时，用户可切换前景开关但不能降为普通 foreground-only；只固定前景 grant 时，用户只能在背景开关之间选择。这里的 fixed 是 any 结果，若组内混合，必须进入单项页面才能定位真正的成员。

还有三类不能机械套表：

- Camera 与 Microphone 在 r48 没有 Location 那样的背景 modifier，ViewModel 却把它们显示成“仅在使用中”；有 assistant、carrier、voice interaction、sound trigger 等前台外能力的包还会改成“始终允许”文案。普通 grant 可把 raw op 设为 `MODE_ALLOWED`，`AppOpsService.UidState.evalMode()`仍会按 pending-top、临时 FGS allowlist 或 CAMERA/MICROPHONE capability 动态折成 allowed/ignored。这是 UI 文案加动态求值，不是新建背景 permission，也不能套用 Location 的 raw `MODE_FOREGROUND`公式。
- Storage 对非 legacy scoped-storage App 把 runtime 媒体权限与 `OPSTR_MANAGE_EXTERNAL_STORAGE` 拼成三态；“所有文件”是独立 special access AppOp，拒绝使用 `MODE_ERRORED`，不是背景子组。
- `config_permissionsIndividuallyControlled` 打开时，SMS、PHONE、CONTACTS 中列出的权限可进入单项页；AOSP 默认是 false。即使关闭，历史、策略和其他 API 仍可能留下混合态。

### 练习 5：从 Light 快照手算按钮

对 LOCATION 填五行状态表，再对 CAMERA 和 STORAGE 标出表中哪些列不适用。命令只定位投影公式和产品开关。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (group.hasPermWithBackgroundMode) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'allowedAlwaysState.isChecked = group.background.isGranted' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'allowedForegroundState.isChecked = group.foreground.isGranted' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'askState.isChecked = !group.foreground.isGranted && group.isOneTime' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'askOneTimeState.isChecked = group.foreground.isGranted && group.isOneTime' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'private fun applyFixToForegroundBackground(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'private fun isForegroundGroupSpecialCase' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'OPSTR_MANAGE_EXTERNAL_STORAGE' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F '<bool name="config_permissionsIndividuallyControlled">false</bool>' frameworks/base/core/res/res/values/config.xml
grep -n -F 'Manifest.permission_group.SMS.equals(group)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
```

完成标准：能从四层状态推导按钮，也能说明一个相同文案为何不保证相同底层 mode。

## 7. 请求数组先扩成 affected permissions，再聚合成 GroupState

`GrantPermissionsActivity`先固定真实 calling package，读取 `EXTRA_REQUEST_PERMISSIONS_NAMES` 和该包的 `PackageInfo`。空数组、包不存在、Manifest 没有权限或 targetSdk 小于 M 都会提前结束；窗口同时隐藏非系统 overlay 并禁止点外部关闭，降低遮挡与误触风险。

每个原始请求项还要经过 `computeAffectedPermissions()`：若旧权限在目标 SDK 之后发生 split，就把适用的新权限加入；targetSdk 不高于 N MR1 时，再扩成这些权限所在主组或背景子组的全部成员，以维持旧版“组内一项带动整组”的兼容语义。较新的 App 通常只改原始项与 split 扩展项。

扩展后的每项通过 `mPermissionNameToGroup` 找到主组或背景子组，并按 `(groupName, isBackgroundGroup)`合并进 `GroupState.affectedPermissions`。restricted 项在模型中缺失时记为 ignored；无法映射的项也只记 ignored，不会因为 UI 使用组名就凭空得到权限。

在展示前还有两类自动决策：

- Device Policy 为 `AUTO_GRANT` 时，按当前 permission filter grant 后再设 `POLICY_FIXED`；`AUTO_DENY` 只设 policy fixed 并记拒绝；
- 默认 policy 下，只要该子组的 `areRuntimePermissionsGranted()`为 true，新请求项就会被自动 grant 并跳过 UI。

第二条使用的是 any 语义。也就是说，同组旧成员已有效，可能让一个新请求成员不经新对话就被授予；这是 r48 明确实现的组兼容策略，不是“用户已经逐项确认”的证明。反过来，`group.isUserFixed()`也是 any：另一个成员带 USER_FIXED 时，整个待请求组可能被保守挡住。policy fixed 的条件要按源码布尔式读：`group.isPolicyFixed() && !group.areRuntimePermissionsGranted() || permission.isPolicyFixed()`。当前 permission 自身 fixed 会直接淘汰；组级 any fixed 只有在该子组没有任何 runtime grant 时才淘汰，不能概括成“组内有一项 policy fixed 就永远挡住整组”。

## 8. target R 的背景请求会转设置页，失败路径仍有副作用

前后台权限先受依赖门约束：背景 `GroupState` 只有在主组已有效，或同一批次中存在前景 `GroupState` 时才保留；否则它被标为 skipped 并记 ignored。这是 UI 的第二道防线，不替代 PMS 对声明、`SYSTEM_FIXED` 与 restriction 等服务端条件的复核；`POLICY_FIXED` 对特权调用者还要另看 `overridePolicy`。

targetSdk 至少为 R 时，若原请求数组长度大于 1 且扩展项落到背景子组，Activity 记录错误并调用 `finish()`：背景权限必须在已有前景基础后单独请求。这个判断看的是整个 `mRequestedPermissions.length`，不是同组项数量。

随后展示逻辑区分三种状态：

| target R+ 状态 | r48 行为 |
| --- | --- |
| 只缺前景 | 通常显示运行时弹窗，只提供 foreground 方向；具有前台外能力的特殊包转设置页 |
| 前景已有，只缺背景 | `sendToSettings()` 打开 `ACTION_MANAGE_APP_PERMISSION` |
| 前景、背景都缺 | 正常 API 合同下不应到达；普通包返回 false，特殊能力包转设置页 |

target 小于 R 的普通 App 仍保留兼容文案：前后景都缺时可在同一 UI 里选 foreground-only；只缺背景且 `mCouldHaveFgCapabilities=false` 时，主按钮隐藏 allow、deny 与 one-time，只留下 `NO_UPGRADE` 类选项。这些按钮保持 foreground-only 能力、不授予 background，但仍会把本次背景请求记录为普通拒绝或固定拒绝，写入 `USER_SET` 或 `USER_FIXED`；真正的升级入口是 detail 文案里的 Settings 链接。具有前台外能力的特殊包会更早进入另一分支，显示普通 deny 或 deny-and-don't-ask-again 加 Settings detail，同样不在弹窗里直接授予背景能力。版本分流发生在 PermissionController，不意味着底层多出一个“按钮状态”字段。

这里有两个需要按控制流理解的窄边界。第一，多项 R 请求调用 `finish()`后没有立刻 `return`，循环仍会执行 `addRequestedPermissions()`；`finish()`已经通过本类 override 采样结果，后面的 Device Policy 自动处理等仍可能改变状态，返回数组与稍后状态可出现代际错位。第二，从设置页返回却没有 `EXTRA_RESULT_PERMISSION_INTERACTED` 时，回调会把当前整个子组从未设置推进到 USER_SET、从 USER_SET 推进到 USER_FIXED，再把组加入 skip；“没有选择”仍被计入防骚扰状态，而且不是 affected-permission 粒度。

### 练习 6：画出请求扩展与 R 分流

分别模拟 target 25、29、30；输入 FINE、FINE+BACKGROUND、BACKGROUND+另一个组权限。记录扩展集合、主/背景 `GroupState`、弹窗或设置页，以及返回值在哪一刻采样。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'ArrayList<String> affectedPermissions =' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'extendedBySplitPerms.addAll(splitPerm.getNewPermissions());' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (requestingAppTargetSDK <= Build.VERSION_CODES.N_MR1)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (group.areRuntimePermissionsGranted()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F '>= Build.VERSION_CODES.R && mRequestedPermissions.length > 1' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'addRequestedPermissions(group, affectedPermissions.get(i), icicle == null);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'boolean foregroundGroupAlreadyGranted =' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'sendToSettings(groupState);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (groupState.mGroup.isUserSet()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'grantResults[i] = pm.checkPermission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
```

完成标准：能指出 R 多项请求的 `finish()`为何不是立即停止执行，并能区分 skipped、返回数组和实际平台状态。

## 9. 请求页按钮先改 GroupState，但不会形成一笔原子提交

`showNextPermissionGroupGrantRequest()`只把支持 one-time 的组排到前面；其余顺序没有额外的业务优先级。当前第一个 `STATE_UNKNOWN` 组显示，前景和背景属于同一展示组时只出现一次对话框，按钮再被翻译回两个 `GroupState`。

请求页的执行顺序是：

| 按钮结果 | 前景 state | 背景 state | 调用顺序 |
| --- | --- | --- | --- |
| `GRANTED_ALWAYS` | 先尝试清 one-time，再 grant | 先调用 `setOneTime(false)`但后台 ONE_TIME 不变，再 grant | 前景后背景 |
| `GRANTED_FOREGROUND_ONLY` | 先尝试清 one-time，再 grant | revoke，随后调用 `setOneTime(false)`但后台 ONE_TIME 不变 | 前景后背景 |
| `GRANTED_ONE_TIME` | 先设 one-time，再 grant | revoke，随后调用 `setOneTime(false)`但后台 ONE_TIME 不变 | 前景后背景 |
| `DENIED` | revoke，再清 one-time | revoke，再调用 `setOneTime(false)`但后台 ONE_TIME 不变 | 前景后背景 |
| `DENIED_DO_NOT_ASK_AGAIN` | revoke，设 user-fixed，再清 one-time | revoke、设 user-fixed，再调用 setter 但后台 ONE_TIME 不变 | 前景后背景 |

第一次普通拒绝通常得到 `USER_SET=1, USER_FIXED=0`；再次展示时按钮换成“不再询问”，对应 `USER_SET=0, USER_FIXED=1`。这不是单一计数器，而是 UI 根据当前 flags 选择下一种动作。

这里不要把设置页的顺序套过来。请求页完全拒绝时先撤前景、再撤背景，会短暂留下“背景 grant 仍在、前景基础已撤”的组合；设置页则有意先撤背景。两条路径最后通常收敛到同一稳定态，但中间状态和异常窗口不同。

`GrantPermissionsActivity`用四参数 `AppPermissions` 构造器，实际 `delayChanges=false`。因此 `group.setOneTime()`本身就会立即调用一次 `persistChanges()`，接下来的 filtered grant/revoke 又调用一次。选择 one-time 时，可能先给整个主/前景子组写入 `ONE_TIME`，并按每一项原有的 grant 快照持久化；本次尚未授予的 affected 项随后才 grant。它不是把整个子组先统一改成 denied，也不是只在方法末尾一次性落账。

另外，Java `setOneTime()`没有 affected filter：它遍历当前子组所有成员，只跳过被识别为 background 的对象。对 targetSdk 高于 N MR1 的单项请求，同一主子组中未参与本次请求的 permission 也可能被设或清 `ONE_TIME`。在背景子组上，无论调用 `setOneTime(true)`还是 `setOneTime(false)`，每一项都会被跳过；随后的 `persistChanges()`只会按旧快照重放整个背景子组，并不会清掉既有的后台 `ONE_TIME`。若 Kotlin 确认路径此前给后台项写入该 flag，后来再走 Java 的 always、foreground-only 或 deny 分支也不能靠这个 setter 清理。

`onPermissionGrantResultSingleState()`忽略 grant/revoke 的 boolean 返回值，调用结束就把 `GroupState`标成 allowed 或 denied。最终交给 App 的数组会重新用 `PackageManager.checkPermission()`读取原始 grant，所以 UI state 与返回数组还有一次分叉；两者都不包含 AppOp 有效性，也不等待文件提交。

## 10. Java filter 只约束模型循环，persist 却重放整个子组

Java grant/revoke 先遍历 `mPermissions`。`filterPermissions`只决定哪些 `Permission` 对象在这一轮被改；不在 filter 的成员保持构造时快照。对现代 App，grant 会把模型 grant 置 true、调整 user flags，并把 `appOpAllowed`置 true；revoke 做相反处理。对 legacy App，runtime grant 本身保持不变，主要切 AppOp 与 `REVOKED_COMPAT`，必要时 kill UID。

遇到 `SYSTEM_FIXED` 时循环设置失败结果并 `break`，而不是跳过该项继续。若前面的成员已经修改，后面的 fixed 才出现，方法不会回滚前项；非 delay 模式仍会继续执行 `persistChanges()`。返回 false 只说明没有“全部通过循环”，不是“零修改”。

更隐蔽的边界在提交层。`persistChanges()`不接收 filter，而是再次遍历当前子组的每一项，按以下真实顺序调用平台：

1. 非 system-fixed 项先 `grantRuntimePermission()`；若模型为 denied，则仅在当前 `checkPermission()`仍为 granted 时调用 revoke；
2. 用固定 mask 更新 `USER_SET`、`USER_FIXED`、`REVOKED_COMPAT`、`POLICY_FIXED`、可清的 `REVIEW_REQUIRED`、`ONE_TIME` 与 `AUTO_REVOKED`；
3. 非 system-fixed 且有关联能力时，最后调用 `allowAppOp()`或 `disallowAppOp()`；
4. 再处理可选 kill、LocationAccessCheck 与 one-time session。

这意味着 filtered 请求仍会对未命中成员重放快照：grant/revoke 通常是幂等的，但 flags 和 AppOps 仍会被提交。特别是 mask 总含 `AUTO_REVOKED`，构造出的 `flags`值却从不带该位，所以当前子组所有成员都会被清掉 AUTO_REVOKED，不只是 affected permission。若对象创建后平台状态并发改变，全子组重放还可能把较新的 flags 或 AppOp 拉回旧快照。

`REVIEW_REQUIRED` 的 mask 是单向的：模型当前不需要 review 时把该位放入 mask 并清除；当前需要 review 时反而不把它放入 mask，所以普通 persist 不会主动新设 review。现代 App 的 Java grant 分支也不调用 `unsetReviewRequired()`；只有 legacy grant 分支显式清它。普通“允许”清 one-time 是请求 Activity 额外调用 setter 的结果，不是 Java grant 方法自身保证。

PackageManager 的 grant/revoke 接口返回 void，但两类失败不能合并。grant 遇 `SYSTEM_FIXED`、未被 override 的 `POLICY_FIXED`，以及未获豁免的 hard restriction 或策略不允许的 soft restriction 时，会记录错误后返回；状态无须改变则直接成为不记错误日志的 no-op。PermissionController 自身声明了 `ADJUST_RUNTIME_PERMISSIONS_POLICY`，所以它的 grant/revoke 会以 `overridePolicy=true` 越过 `POLICY_FIXED` 检查。revoke 遇非 system UID 的 `SYSTEM_FIXED`，或未 override 的 `POLICY_FIXED`，通常抛 `SecurityException`，并没有与 grant 对称的 restriction 拦截入口。Java 方法没有逐项 acknowledgment；在 grant 被限制条件静默挡回且自己的模型循环未失败时，它仍可能返回 true，调用者又忽略该 boolean，完成语义就更弱。

### 练习 7：证明 filter 与提交范围不同

选一个含两个成员的主组，只把一项放进 filter，并假设另一项带 AUTO_REVOKED。沿模型循环和 persist 循环各走一遍，写出最终 flags。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public boolean grantRuntimePermissions(boolean setByTheUser, boolean fixedByTheUser,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F '&& !ArrayUtils.contains(filterPermissions, permission.getName())) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'wasAllGranted = false;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'wasAllRevoked = false;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'public void persistChanges(boolean mayKillBecauseOfAppOpsChange, String revokeReason)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mPackageManager.grantRuntimePermission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mPackageManager.updatePermissionFlags(permission.getName(),' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'PackageManager.FLAG_PERMISSION_AUTO_REVOKED, // clear auto revoke' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'shouldKillApp |= allowAppOp(permission, uid);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'public void setOneTime(boolean isOneTime)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'if (!permission.isBackgroundPermission()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
```

完成标准：能明确说出 filter 过滤的是第一次模型循环，而非后续 Binder 调用集合。

## 11. Kotlin 设置路径直接逐项写，但返回的新对象仍不是确认回执

`KotlinUtils`不维护 Java `Permission` 的延迟状态。它按 Light map 中的 filter 遍历，每项直接调用平台，再构造一个新 `LightAppPermGroup`供 ViewModel 连续计算。这里的实际 grant 顺序同样不是“AppOp 先于 permission”：

- grant：若 Light 认为尚未有效，现代 App 先调用 PackageManager grant，再调用 `allowAppOp()`；之后清 compat/review/user-fixed/one-time/auto-revoked、设 user-set，并更新 flags；
- revoke：若 Light 认为当前有效，现代 App 先调用 PackageManager revoke，再调用 `disallowAppOp()`；之后根据参数设置 user-fixed，普通拒绝设 user-set，ask 清 user-set 并设 one-time，最后清 auto-revoked；
- legacy App 不撤 install-time grant，而用 AppOp 和 `REVOKED_COMPAT`表达关闭；一旦进入有关联 AppOp 的 grant/revoke 分支就标记需要 kill，并不等待 `setUidMode()`报告真实变化。正常一致态下二者通常同时发生。

`PERMISSION_CONTROLLER_CHANGED_FLAG_MASK`只覆盖 `USER_SET`、`USER_FIXED`、`ONE_TIME`、`REVOKED_COMPAT`、`REVIEW_REQUIRED` 与 `AUTO_REVOKED`；`ONE_TIME`在表达式中重复一次但按位 OR 没有额外效果。它不覆盖 `DEFAULT`、`ROLE`、`SYSTEM_FIXED`、`POLICY_FIXED` 或 restriction exemption，所以用户操作会保留这些来源与策略位。

单项 helper 自己只硬挡 `SYSTEM_FIXED`，不检查 `POLICY_FIXED`。正常手持页由 ViewModel 先计算 `isForegroundFixed/isBackgroundFixed`，从 `ChangeRequest`中消掉相应方向；PMS 会重查 `SYSTEM_FIXED`，grant 还会重查 restriction。可是 PermissionController 持有 policy override 权限，PMS 的 `POLICY_FIXED` 条件对它可以绕过，因此 ViewModel 才是这条策略边界的关键防线。若其他内部路径绕过 ViewModel，不能把 Kotlin helper 或 PMS 笼统视为无条件 policy-fixed 保险。

这里还有一个与 void API 相同的完成问题：PMS 的 grant 可能因 hard/soft restriction 或 `SYSTEM_FIXED` 记录后返回，Kotlin 随后仍会把局部 `isGranted`设 true、继续调 AppOp并返回新 Light 对象；revoke 的 fixed 失败则通常以异常中断，而不是返回 false。正常设置页已过滤 restricted、预先挡 fixed，风险主要出现在过期快照或非常规调用；无论如何，新对象只代表函数推演结果，不是服务端逐项确认。

`AppPermissionViewModel.requestChange()`在一个按钮内固定执行：撤背景 → 撤前景 → 授前景 → 授背景。这个顺序令降级先移除更强能力、升级先建立基础资格；但每一步仍是独立 Binder 调用，中途异常或进程终止不会回滚已完成的前半段。

### 练习 8：比较 Java 与 Kotlin 的真实写序

不要读注释推断顺序，直接按调用语句排列 permission、AppOp、flags 和 kill。再确认 ViewModel 如何组合四类操作。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private const val PERMISSION_CONTROLLER_CHANGED_FLAG_MASK' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'app.packageManager.grantRuntimePermission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'allowAppOp(app, perm, group)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'app.packageManager.revokeRuntimePermission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'disallowAppOp(app, perm, group)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'app.packageManager.updatePermissionFlags(perm.name' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'if (shouldRevokeBackground && group.hasBackgroundGroup' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'if (shouldRevokeForeground && (wasForegroundGranted' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'if (shouldGrantForeground) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'if (shouldGrantBackground && group.hasBackgroundGroup)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/model/AppPermissionViewModel.kt
grep -n -F 'wasChanged = wasChanged || setOpMode' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'private static int[] sOpToSwitch = new int[] {' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'OP_COARSE_LOCATION,                 // FINE_LOCATION' frameworks/base/core/java/android/app/AppOpsManager.java
grep -n -F 'code = AppOpsManager.opToSwitch(code);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

完成标准：能写出两条真实跨服务顺序，并说明为何新 Light 对象不能替代重新读取平台。

## 12. 前后台范围由 AppOp 拼接，Kotlin 短路在 r48 仍命中同一 switch

对有关联 AppOp 的普通 permission，允许通常设 `MODE_ALLOWED`，拒绝设 `MODE_IGNORED`。对有 background modifier 的前景 permission，背景未有效时允许设 `MODE_FOREGROUND`，背景有效时提升为 `MODE_ALLOWED`。操作背景 permission 自身时，代码不依赖它有独立 AppOp，而是遍历反向关联的前景项：授背景提升前景 ops，撤背景把它们降回 `MODE_FOREGROUND`。

但“遍历已授予前景项”在两套模型中的真实谓词不同。Java 看 `foregroundPermission.isAppOpAllowed()`，它是构造或局部修改得到的 AppOp 布尔，不直接验证 grant；Kotlin 看 Light 的 `isGrantedIncludingAppOp`，而 r48 该值实际来自 grant 与 `REVOKED_COMPAT`。正常一致态下二者近似“有效前景”，遇到跨服务偏差时则可能选出不同集合。

Kotlin 背景 allow/disallow 循环确实写成 `wasChanged = wasChanged || setOpMode(...)`：一旦较早的前景 op 真正变化，`||`会跳过后续调用。但 r48 当前有 background modifier 的平台组只有 LOCATION，而 `AppOpsManager.sOpToSwitch`把 FINE、COARSE 乃至 GPS 都映射到 `OP_COARSE_LOCATION`；`AppOpsService.setUidMode()`在写 UID mode 前也会先执行 `opToSwitch()`归一化。因此第一次调用已经改动同一个 UID switch，第二次被跳过不会留下另一份独立 mode。它是值得警惕的代码形状与未来扩展缝隙，不是 r48 已证实的状态错误；Java 对应代码用 `|=`，右侧始终执行。

两条路径都调用 `AppOpsManager.setUidMode()`，不是 package mode；相关 AppOp 会影响共享该 UID 的其他包。它们在需要时调用的 `ActivityManager.killUid()`也同样是 shared-UID 粒度。界面以 package 为标题，不等于所有副作用都只落在该包。

`setUidMode()`也不是纯 AppOps 写。在普通调用中，AppOpsService 会先执行 `updatePermissionRevokedCompat()`，把目标 mode 反投影为相关 runtime permission 的 `REVOKED_COMPAT`，随后才改 UID mode；该 helper 从 `getPackagesForUid()`结果取第一个包名。于是一次 AppOp 修改可能额外触发 permission flags 写，shared UID 下还带“UID 级 mode、首包 flag 更新”的不对称。

这里不存在跨 PMS、AppOps 与 ActivityManager 的事务：grant 与 mode 之间有可观察窗口，AppOps 写失败不会撤回 permission，后续 flags 写失败也不会恢复之前两步。源码注释中“先启 AppOp 再 grant”的安全意图与 r48 实际调用顺序相反，诊断应以 `persistChanges()`和 `KotlinUtils`调用点为准。

## 13. ONE_TIME 同时跨 flags、grant 与 UID 会话

r48 只有 LOCATION、CAMERA、MICROPHONE 三个展示组支持 one-time。`ONE_TIME` 本身只是逐 permission flag；当前能否访问仍取决于 grant 与 AppOp，何时自动收回则由 system_server 的 `OneTimePermissionUserManager` 跟踪 UID importance。

请求页选择“仅本次”时，Java 路径先对整个前景子组设 `ONE_TIME` 并立即 persist，再 grant affected 前景项。`persistChanges()`末尾看到当前子组有 one-time 且组内任一权限有效，才调用 `startOneTimePermissionSession()`；如果当前组不满足且全包已没有 one-time permission，才调用 stop。会话启动的 void 返回不代表未来撤销已经完成。

会话管理器用 UID 作为 `mListeners` key。已有 listener 时，再以同一 UID 的另一个 packageName 启动不会替换原对象、超时或阈值；listener 保存第一次创建时的 packageName，超时再通知 PermissionController 按该包枚举并撤销 one-time 组。shared UID 因而同时存在“按 UID 观察、按首个包名回调”的粒度错位。

设置页的 Kotlin grant/revoke 只改 permission、flags 与 AppOps，不调用 start/stop one-time session。用户在活跃的 one-time 会话中改为永久允许或拒绝，旧 listener 可以继续存活到超时；超时回调会重新读取 flags，已不再是 one-time 的组不会被这次回调撤销，但“按钮完成”仍不等于 listener 已立即清理。

跨重启还要再降一级承诺。第264章已核准：runtime permission 持久化会把带 `ONE_TIME` 的临时 grant 写成 denied，避免设备重启后恢复临时能力。文件能恢复 flag/denied 组合，却不会恢复旧进程活跃度或原 listener 的计时现场。

位置还有独立的后续动作：Java/Kotlin 在 FINE 与 BACKGROUND 首次共同变为有效时安排 `LocationAccessCheck`，未来提醒用户复核后台位置。它既不是 grant 的回滚器，也不是当前按钮的 durable acknowledgment；单独 COARSE、缺 FINE 或重复提交不满足同一触发条件。

## 14. DEFAULT、ROLE、restricted 与 legacy review 都会改写普通按钮语义

权限 UI 的 flag mask 不清 `GRANTED_BY_DEFAULT` 或 `GRANTED_BY_ROLE`。用户撤销一个自动授予项后，可以得到 `denied + DEFAULT/ROLE + USER_SET`；来源位不带引用计数，也不等于当前 grant。后续 Default/Role 是否重授还要重新检查用户 flags 和各自资格，不能从来源位单独推断。

手持设置页在撤销 DEFAULT、legacy App 权限或 install→runtime split 时显示警告；它不因 ROLE 单独显示同类警告。确认只授权当前这次 ViewModel 操作，不会清来源位。`onDenyAnyWay()`在 r48 还有三个精确缺口：

- 先把 background 的 DEFAULT 记忆 OR 进局部变量，处理 foreground 时却用赋值覆盖；若只有背景带 DEFAULT，`hasConfirmedRevoke`可能最终变回 false；
- “每次询问”的确认把 `oneTime=true`也传给 background Kotlin revoke，于是背景 permission 会被写入 ONE_TIME，区别于普通 `requestChange()`先以默认 false 撤背景；
- 弹警告的条件包含 `hasInstallToRuntimeSplit`，确认后的 `hasConfirmedRevoke`条件却没有它，split-only 场景可能再次要求确认。

restricted 又展示了两套模型的差异。Light 对象保留 `allPermissions`，但公开 `permissions`会过滤 hard restricted 非豁免项和策略要求隐藏的 soft restricted 项，因此设置页的 Always 等按钮可能直接消失。Java 创建路径对主/前景项做 restriction 过滤，已成功链接的背景对象却先走 background 分支加入子组；“进入 Java 背景模型”仍不等于可 grant，PMS 最终会重查 hard/soft restriction。

legacy `REVIEW_REQUIRED` 不是普通请求弹窗。手持 `ReviewPermissionsFragment`用 `delayChanges=true` 构造 `AppPermissions`：用户点继续时，未触碰但以“已授予”展示的 review 组会在内存中补 grant，主组与背景组统一 `persistChanges(true)`；随后又遍历包的所有 requested permissions，单独清 `REVIEW_REQUIRED`，连 restricted 或没有模型对象的项也覆盖。取消则不调用这条提交链，局部模型变化随页面销毁。

Wear 的 review 实现不同：它使用非 delay Java 模型，但切换 Switch 本身只改变 Preference；点继续后才遍历主组，此时每个 grant/revoke 及随后的 `unsetReviewRequired()`各自立即 persist。它不遍历 background subgroup，也没有手持版“对所有 requested permission 手工清 review flag”的兜底；继续前取消不会产生这些模型写入。形态相同的“继续”按钮在手持与 Wear 上没有相同批处理边界，分析时必须先确认实际 Fragment。

## 15. UI 回调、服务端内存和两份文件有不同完成点

一次点击跨越多个独立提交时钟：

| 可观察事件 | 已经能证明 | 仍不能证明 |
| --- | --- | --- |
| Java/Light 对象改变 | PermissionController 已算出期望状态 | PMS 或 AppOps 接受了写入 |
| PackageManager Binder 返回 | 对应同步调用已结束，通常内存态已处理 | runtime XML 已提交；后续 AppOp 已成功 |
| AppOps `setUidMode()`返回 | UID mode 内存态已处理，远端 watcher 通知已排队 | watcher 已实际收到回调；`appops.xml` 已持久化；permission 与 flags 同步 |
| `GroupState=ALLOWED/DENIED` | 请求 UI 已推进队列 | grant/revoke boolean 成功；有效态与落盘 |
| 请求 Activity result | 原请求数组已用 `checkPermission()`采样 raw grant | AppOp、review、隐式 affected 项、文件耐久 |
| 设置页 result extra | 某按钮交互已被记录 | 确认框被接受；状态真的改变 |
| LiveData 刷新 | permission listener 触发后又读到一版 grant/flags | 纯 AppOp 变化一定被观察；跨服务原子快照 |

PMS 的 grant callback 通知 listener 后调用 `writeSettings(true)`，是可丢的异步持久化请求；revoke callback 调 `writeSettings(false)`并异步 post kill，但同步写 package settings 仍不等于每用户 runtime XML 已落盘；flags 更新走 `writePermissionSettings(userIds, true)`式异步调度。普通 runtime permission writer 以 200 毫秒合并变化，并用 2 秒窗口限制连续推迟，这两者都是消息调度边界，不是磁盘完成期限。AppOps `setUidMode()`则调用自己的 `scheduleWriteLocked()`，非 debug 的普通延迟常量是 30 分钟；后续 fast write 或显式 flush 还会改变实际时刻。

设置页还有一个更早的回执：`AppPermissionFragment`在 `requestChange()`之后立刻 `setResult()`；而 `requestChange()`可能只弹出 DEFAULT/legacy/split 确认框便返回。即使用户取消确认，先前写入 Activity 的 result extra 也没有被清除。所以 `DENIED_DO_NOT_ASK_AGAIN`在这里是按钮枚举，既不表示 Kotlin revoke 设置了 USER_FIXED——常规设置页实际传 false——也不表示确认已经发生。

手持 review 的 `executeCallback(true)`同样只发生在一串 Binder 调用之后，不等待 runtime-permissions.xml 与 appops.xml 同时 durable。整个系统没有一个把 permission、flags、AppOps、one-time listener、kill 和 UI result 一起提交的事务 ID。

### 练习 9：为一次点击标出全部完成点

选择“始终允许位置”或“拒绝默认授予位置”，从 UI result 一直追到 PMS、AppOps 和 review 分支。给每个回调标注“内存”“调度写”“文件提交”之一。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'grantResults[i] = pm.checkPermission' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'setResult(DENIED_DO_NOT_ASK_AGAIN);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/handheld/AppPermissionFragment.java
grep -n -F 'new AppPermissions(activity, packageInfo, false, true,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/handheld/ReviewPermissionsFragment.java
grep -n -F 'mAppPermissions.persistChanges(true);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/handheld/ReviewPermissionsFragment.java
grep -n -F 'pm.updatePermissionFlags(perm, pkg.packageName, FLAG_PERMISSION_REVIEW_REQUIRED,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/handheld/ReviewPermissionsFragment.java
grep -n -F 'mPackageManagerInt.writeSettings(true);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(false);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writePermissionSettings(userIds, !sync);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'android.permission.ADJUST_RUNTIME_PERMISSIONS_POLICY' packages/apps/PermissionController/AndroidManifest.xml
grep -n -F 'final boolean overridePolicy =' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!overridePolicy && (flags & PackageManager.FLAG_PERMISSION_POLICY_FIXED) != 0) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Non-System UID cannot revoke system fixed permission' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'private static final long WRITE_PERMISSIONS_DELAY_MILLIS = 200;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'private static final long MAX_WRITE_PERMISSIONS_DELAY_MILLIS = 2000;' frameworks/base/services/core/java/com/android/server/pm/Settings.java
grep -n -F 'static final long WRITE_DELAY = DEBUG ? 1000 : 30*60*1000;' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHandler.postDelayed(mWriteRunner, WRITE_DELAY);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F '.startOneTimePermissionSession(packageName,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'private final SparseArray<PackageInactivityListener> mListeners' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
```

完成标准：能解释为什么“Activity 已返回 granted”不是 permission 功能态或双文件耐久态的证据。

## 16. 用逐项矩阵收束，而不是继续猜组按钮

定位权限异常时，最有效的最小记录不是“LOCATION 显示允许”，而是逐项写出：

| permission | raw grant | flags | raw AppOp | 模型口径 | UI 来源 |
| --- | --- | --- | --- | --- | --- |
| `ACCESS_FINE_LOCATION` | true/false | user/fixed/source/one-time/restriction | allowed/foreground/ignored | Java 或 Light | request/review/settings |
| `ACCESS_COARSE_LOCATION` | true/false | 同上 | allowed/foreground/ignored | Java 或 Light | 同上 |
| `ACCESS_BACKGROUND_LOCATION` | true/false | 同上 | 看关联前景 ops | 主组还是背景子组 | 同上 |

然后按症状回溯：

- UI 显示允许但访问失败：先确认当前走 Light 还是 Java，再核对单项 raw grant、`REVIEW_REQUIRED`、`REVOKED_COMPAT`和真实 AppOp；
- 同组未请求项的 flags 变化：检查 Java `setOneTime()`的全子组范围、filtered 操作后的全量 `persistChanges()`与 AUTO_REVOKED 清除；
- 看到 Kotlin 的 `||`短路：先查 `opToSwitch()`；r48 的 FINE/COARSE 共享 `OP_COARSE_LOCATION`，不能报告成第二份 mode 漏写；
- 请求页 state 与返回数组冲突：检查 Java system-fixed break、PMS grant 的记录后返回、revoke fixed 异常、R 多项 `finish()`后的继续执行，以及 raw `checkPermission()`采样时刻；
- 另一个包也受影响：先确认 shared UID，因为 `setUidMode()`、`killUid()`和 one-time listener 都不是纯 package 粒度；
- 重启后临时授权消失：这是 one-time 序列化边界，不应拿重启前 UI 快照反推恢复结果；
- legacy review 行为不同：确认是手持 delayed 模型还是 Wear immediate 模型。

最后再划清一个同名陷阱：`AppPermissionUsage.GroupUsage`里的 foreground/background time 与 count 来自 AppOps 的访问历史，描述“操作发生时 UID 在前台还是后台”；它们不是 `AppPermissionGroup` 的前景 permission 与背景 modifier 子组。`PermissionUsages`只从主组 permissions 收集 AppOp 名，通过 `getOpsForPackage()`或`getPackagesForOps()`读取当前记录，再以 `HistoricalOpsRequest(OP_FLAGS_ALL_TRUSTED)`读取历史记录；对应调试 UI 明示 `INTERNAL ONLY`且数据可能不准确，这条链也不写 grant、flags 或 AppOp mode。授权结构和使用统计可以关联，但不能相互替代。

本章的主线至此闭合：平台映射决定展示组，目标包请求决定成员，双向链接拆出背景子组；Java 与 Light 用不同口径读快照，组级 any/OR 把多项状态压成按钮；请求页、设置页和 review 页再以各自顺序改 permission、flags、AppOps 与 one-time 会话。任何可靠结论最终都必须回到单项状态和明确完成点。下一章进入第267章：`PermissionManager`、split permissions、动态权限、permission tree、定义缓存与所有权链。
