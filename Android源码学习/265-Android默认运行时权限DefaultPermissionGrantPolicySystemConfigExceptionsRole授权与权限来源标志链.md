# Android默认运行时权限：DefaultPermissionGrantPolicy、SystemConfig边界、exceptions、Role与来源标志链

## 1. 自动获得权限不是一条链，而是两台授权器改四本账

本文固定在 AOSP `android-11.0.0_r48`，源码提交边界为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`。第264章解释了运行时权限怎样写入 `runtime-permissions.xml`；本章向上追问：系统应用、默认处理者和角色持有者为什么会在没有普通授权弹窗时取得危险权限。

答案不能压成“系统替默认应用授权”。Android 11 至少有两台彼此独立的授权器：

| 授权器 | 所在进程 | 主要输入 | 主要产物 |
| --- | --- | --- | --- |
| `DefaultPermissionGrantPolicy` | system_server | 系统组件分类、默认处理者、动态 provider、各分区例外 XML | runtime grant、`GRANTED_BY_DEFAULT`、可选 `SYSTEM_FIXED`、restricted exemption |
| PermissionController 的 `Role` 模型 | PermissionController 进程 | `res/xml/roles.xml`、资格规则、当前 holder、default/fallback | runtime grant、`GRANTED_BY_ROLE`、AppOp、特殊白名单、preferred activity 等 |

两台授权器最终会碰到至少四个核心状态面：runtime permission grant、permission flags、AppOps mode、role holder 记录。前两者通常写进每用户 `runtime-permissions.xml`，AppOps 有自己的持久化，holder 与角色版本、包哈希则写进 `roles.xml`；Role 还可能改 preferred activity 与“None”偏好。一次回调成功不等于这些账都已持久化。

最重要的阅读原则是：`GRANTED_BY_DEFAULT` 和 `GRANTED_BY_ROLE` 是来源标记，不是 grant 位，也不是带引用计数的所有权记录。功能是否真正可用还可能受 AppOp、前后台配对、review 状态与固定策略约束。后文所有边界都从这句话展开。

## 2. systemReady、新用户、包变化和人工切换是四种入口

system_server 的默认策略有两条主入口。`PermissionManagerService.systemReady()` 遍历所有用户，只为 `isPermissionUpgradeNeeded(userId)` 为真的用户执行 `grantDefaultPermissions()`；若一个用户都不需要执行，才异步预读产品例外，减少以后创建用户时的磁盘读取。新用户则由 `onNewUserCreated()` 直接运行默认授权，然后再做一次 `updateAllPermissions()`。所以“平台升级”和“新建用户”不是同一事件，也不能把例外预读误当成授权。

Role 侧有另一套触发器。`RoleManagerService.onStartUser()` 同步检查组件状态哈希；包新增、移除或 enabled component 变化会通过广播进入节流后的异步检查。哈希变化时，system_server 请求对应用户的 RoleController 执行 `grantDefaultRoles`，只有控制器返回成功才把新哈希写入 `RoleUserState`。用户在设置页添加或移除 holder，则走单独的 add/remove Binder 操作，不必等待下一次默认角色扫描。

这四类入口的完成语义也不同：

- Default policy 返回，表示延迟缓存中的 PM 调用已逐项尝试；不表示 runtime 权限文件已提交。
- RoleController 的布尔结果，表示它走完了当前顺序流程；不表示相关持久化文件都落盘。
- `RoleUserState` 的 holder 变更先改内存并发监听，再延迟写文件。
- 包哈希只是“这份包组件快照已成功跑过角色初始化”的门闩，不证明之前每一次权限或 AppOp 写入都 durable。

### 练习 1：把四种入口与回调边界连起来

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mPackageManagerInt.isPermissionUpgradeNeeded(userId)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mDefaultPermissionGrantPolicy.grantDefaultPermissions(userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'public void onNewUserCreated(int userId) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'maybeGrantDefaultRolesSync(userId);' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
grep -n -F 'maybeGrantDefaultRolesAsync(userId);' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
grep -n -F 'getOrCreateController(userId).grantDefaultRoles(FgThread.getExecutor(),' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
grep -n -F 'userState.setPackagesHash(newPackagesHash);' frameworks/base/services/core/java/com/android/server/role/RoleManagerService.java
```

分别画出“升级启动”“新用户”“包变化”“设置页切换默认短信应用”的入口。每条线上标出调用返回、内存 holder 已变、权限 API 已返回、`roles.xml` 已提交四个点；源码没有把它们合成一个事务。

## 3. grant、来源、固定和restricted exemption是正交维度

一个 permission 的 runtime grant 回答“PackageManager 当前是否授予”；flags 回答“为什么、由谁控制以及有哪些附加限制”。以下位不能互相替代：

| 位 | 语义 | 单看它能否证明权限有效 |
| --- | --- | --- |
| `FLAG_PERMISSION_GRANTED_BY_DEFAULT` | 默认策略曾把该 permission 记为默认来源 | 不能 |
| `FLAG_PERMISSION_GRANTED_BY_ROLE` | Role 路径在特定条件下记过角色来源 | 不能 |
| `FLAG_PERMISSION_SYSTEM_FIXED` | 普通主体不能随意改变这一状态 | 不能，可能出现 fixed 但 denied |
| `FLAG_PERMISSION_POLICY_FIXED` | device/profile policy 固定 | 不能，但两台授权器都把它视为硬边界 |
| `USER_SET` / `USER_FIXED` | 用户已作选择或固定拒绝 | 不能 |
| `REVIEW_REQUIRED` | 即使 runtime 位为 granted，也仍需审核 | 常使“有效授权”失败 |
| restriction exemption 位 | 允许 hard/soft restricted 权限越过相应限制 | 本身不授予 |

Default 的正常授予门会保护 USER_SET、USER_FIXED、POLICY_FIXED、SYSTEM_FIXED；某些显式 override 路径可越过用户位和 system-fixed 的外层判断，但内层仍再次拒绝 POLICY_FIXED，`denied + SYSTEM_FIXED` 还可能被底层静默拒绝。Role 的 grant 同样始终保护 POLICY_FIXED 和 SYSTEM_FIXED，是否保护 USER_SET/USER_FIXED 由调用参数决定。两边名字相似，覆盖范围却不完全相同。

AppOp 又是独立平面。Role 代码把“permission 与 AppOp 整体有效”定义得比 PM grant 更严格：runtime 位必须允许、不能需要 review，普通 permission 的 AppOp 要是 ALLOWED，前景 permission 可为 FOREGROUND 或 ALLOWED，背景 permission 则要求至少一个前景 permission 的 AppOp 已到 ALLOWED。

### 练习 2：证明来源位不是grant位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final int FLAG_PERMISSION_POLICY_FIXED =  1 << 2;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int FLAG_PERMISSION_SYSTEM_FIXED =  1 << 4;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int FLAG_PERMISSION_GRANTED_BY_DEFAULT =  1 << 5;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'public static final int FLAG_PERMISSION_GRANTED_BY_ROLE =  1 << 15;' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'if (!isPermissionGrantedWithoutCheckingAppOp(packageName, permission, context)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'if (isPermissionReviewRequired(packageName, permission, context)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'return appOpMode == AppOpsManager.MODE_FOREGROUND' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
```

构造三种状态：`granted + ROLE + AppOp ignored`、`denied + DEFAULT + SYSTEM_FIXED`、`granted + POLICY_FIXED`。分别回答功能是否有效、默认策略能否覆盖、Role 能否覆盖；不要用任一来源位代替 grant 查询。

## 4. DefaultPermissionGrantPolicy先计算三阶段，再集中apply

`grantDefaultPermissions(userId)` 的结构很短，却决定了默认来源的优先顺序：

1. 为系统组件与满足条件的 privileged persistent 平台签名应用授予其请求的 runtime 权限；
2. 为安装器、验证器、相机、Dialer、SMS、Calendar、Contacts、Location 等默认系统处理者授予预定义权限集合；
3. 读取各分区 `etc/default-permissions/*.xml`，执行产品例外；
4. 调用 `DelayingPackageManagerCache.apply()`，把前面累计的状态逐项交给真实 PackageManager。

延迟缓存的目的不是提供数据库事务，而是让同一轮中的后续判断看见前面拟议的 grant/flags，并减少反复查询。它按“目标用户计算出的 uid + permission”合并 `PermissionState`；PackageInfo、PermissionInfo 与 user Context 也分别缓存。多个阶段命中同一 uid/permission 时，后写逻辑看到的是缓存的新状态。

apply 时先 `corkPackageInfoCache()`，再逐 uid、逐 permission 调用 `PermissionState.apply()`，最后 uncork。单项 apply 的正确顺序是：先删除会阻止 grant/revoke 的 flags，再加 restriction exemption，再改 grant，最后补其余 flags。这只是操作排序；没有撤销日志，也没有把所有 permission 包进原子提交。

### 练习 3：验证三阶段与延迟状态的粒度

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'DelayingPackageManagerCache pm = new DelayingPackageManagerCache();' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'grantPermissionsToSysComponentsAndPrivApps(pm, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'grantDefaultSystemHandlerPermissions(pm, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'grantDefaultPermissionExceptions(pm, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'pm.apply();' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'private SparseArray<ArrayMap<String, PermissionState>> mDelayedPermissionState =' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'PackageManager.corkPackageInfoCache();' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'mDelayedPermissionState.valueAt(uidIdx).valueAt(permIdx).apply();' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
```

让同一包的同一 permission 依次被第一阶段 fixed grant、第二阶段 non-fixed grant、第三阶段 exception 命中，标出缓存中只有几个 `PermissionState`，以及哪些变化要等 apply 才进入真实服务。

## 5. 第一阶段的“系统组件”与第二阶段的“系统包”互斥

第一阶段并非给所有预装应用全授权。`isSysComponentOrPersistentPlatformSignedPrivApp()` 接受两类对象：

- appId 小于 `FIRST_APPLICATION_UID` 的核心 UID；
- privileged、persistent 且 platform-signed 的应用。

源码注释希望 updated system app 的 persistent 条件优先看被禁用的工厂包；但 r48 的实际调用并没有完成这件事。内部服务只返回 disabled package 的包名，`PackageManagerWrapper.getSystemPackageInfo(name)` 随后仍用普通 `getPackageInfo` 和 `DEFAULT_PACKAGE_INFO_QUERY_FLAGS` 查询，没有 `MATCH_FACTORY_ONLY`。常见的同包名更新因此又解析到 data 分区当前包，persistent 核验实际退化成检查更新版本。必须把“注释的安全意图”和“当前查询结果”分开。

通过实际分类、targetSdk 支持 runtime 权限且声明列表非空后，策略才把其请求的所有 runtime permission 作为 system-fixed 默认授权。枚举固定查询 system user，而且 flags 还包含 uninstalled、hidden-until-installed 与 disabled-until-used 包；真正 grant 却使用传入的目标 userId。因此这不是“目标用户当前安装包集合”，策略可能先为目标用户预置状态。

随后还有一条兼容通道：所有 privileged、支持 runtime 权限且请求 `READ_PRIVILEGED_PHONE_STATE` 的包，会尝试额外获得 system-fixed 的 `READ_PHONE_STATE`；通用 Manifest 交集仍要求它也声明 READ_PHONE_STATE。这条循环没有再次要求前一类“核心组件或 persistent 平台签名应用”，范围更宽，不能并回第一类资格定义。

第二、三阶段的大多数 handler helper 与所有 exception 使用 `isSystemPackage()`：它要求 system app，同时排除刚才的核心组件或 persistent 平台签名 privileged app。两类通常互斥；但 known-package Browser 分支会直接走 `grantPermissionsToPackage()`，仅在 Intent fallback 时额外检查 `isSystemPackage`，所以不能把它写成第二阶段的绝对总门。即使核心组件从 Browser 分支再次命中，第一阶段通常也已经处理过它。

默认处理者来源也不统一。有的从 known-package 常量取第一项，有的解析 activity/service/provider，有的从在锁内复制出的动态 provider 取得包数组。provider 调用发生在锁外，避免把外部查询塞进 `mLock`；Dialer 和 SMS 在 provider 返回 null 时退回 Intent 解析，而 SIM call manager、open Wi-Fi 等 provider 为空时会直接跳过。RoleManagerService 另向 PermissionManagerService 安装 browser、dialer、home holder provider，供其他默认应用 API 查询 Role 状态；这是一座桥，但不会把两套来源账本合成一套。

Telecom 安装的 provider 还有多用户窄边界。SMS 与 Dialer lambda 虽收到 userId，却分别用 system_server 的 `mContext` 调 `SmsApplication.getDefaultSmsApplication` 和 `DefaultDialerManager.getDefaultDialerApplication`，没有把 userId 传入查询；SIM call manager 的正常路径会传 userId，但 Telecom 尚未连接时只积压整数 userId，连接后先做一次无 userId 的 `getSimCallManager()`，再把同一 package 授给所有积压用户。多用户设备不能假设这三条查询都天然按目标用户隔离。

sync-adapter provider 的 null 语义又不同：调用方允许 provider 缺失或返回 null，却把结果直接交给 `getHeadlessSyncAdapterPackages()`，后者立即 foreach，没有空值保护。正常产品往往会及时注册 provider，但依赖未就绪就可能以 NullPointerException 中止整轮默认授权，而不是只跳过 sync adapter。

## 6. Manifest交集、工厂APK和split决定“候选”能否真正授予

权限组常量只是候选集合。`grantRuntimePermissions()` 先取得传入 PackageInfo 的 `requestedPermissions`，再与当前安装版本的请求列表相交；未被当前版本声明的项会变成 null 并被过滤。因此任何候选最终都必须出现在当前 Manifest。

r48 这里有一个值得单独记录的副作用：局部变量直接引用 `pkg.requestedPermissions`，交集循环把数组元素原地写为 null。如果传入的是缓存的 PackageInfo，后续对同一个对象的读取可能看见被改写的请求数组。它不是纯函数式过滤。

对 updated system app，普通 Default 路径的注释与分支都试图读取 disabled factory package：若工厂 APK 不声明任何权限就退出，否则应以工厂请求列表为遍历基线，并用当前 Manifest 限制可授予项。可 r48 仍通过上述普通 wrapper 查询同一个包名，没有 `MATCH_FACTORY_ONLY`。在常见同包名更新场景，`disabledPkg` 实际还是当前版本，数组通常相等，所谓工厂基线就没有建立；data 分区更新新增的 Manifest 权限可能进入默认授权候选。Role 的工具类确实提供 `getFactoryPackageInfo(...MATCH_FACTORY_ONLY)`，但 `Role.grant()` 传入 `overrideDisabledSystemPackage=true`，本来就跳过该限制。

`ignoreSystemPackage=true` 会直接绕开 Default 的这段基线分支。r48 类内实际使用只有动态 `grantDefaultPermissionsToDefaultUseOpenWifiApp()` 调用的 helper，以及产品例外把 `permissionGrant.whitelisted` 直接传进该参数；Dialer/SMS 仍走普通 system-package helper。代码旁关于 Phone/SMS 的注释不能替代调用点事实。即便修正普通查询，这两个 override 来源仍会按当前实现绕过工厂基线。

候选集合会按 split-permission 表扩张：目标 SDK 早于 split 的应用若命中旧权限，新增权限一并进入候选。之后代码构造了“前景 permission 在前、背景 permission 在后”的 `sortedRequestedPermissions`，但真正循环仍读取原始 `requestedPermissions`。所以 r48 Default 路径的排序结果没有生效；Role 路径的相似排序才确实被遍历。

因此 Default 路径没有源码承诺的“前景先行”顺序；但 apply 后来又按 SparseArray/ArrayMap 的缓存顺序遍历，PMS 也不会单纯因为背景项先到就拒绝 grant。这里能确认的是排序数组为死代码和中间观察顺序缺乏保证，不能进一步推出最终必然漏 grant，或断言最终结果由 Manifest 声明次序决定。

## 7. r48延迟缓存有两处位运算缺口，apply也不是失败原子

正常 Default grant 先读取旧 flags。若没有 fixed/user-set，或者调用方要求 override，或者正在重新设置已有 system-fixed 状态，就进入修改；无论 override 如何，POLICY_FIXED 都在内层再次阻断。若准备从 fixed 来源降级为 non-fixed，代码还会请求清除 SYSTEM_FIXED。

但 `DelayingPackageManagerCache.updatePermissionFlags()` 的实现不是标准掩码替换，而是：

`state.newFlags |= flagValues & flagMask`

它只会 OR 入位，从不清除 mask 中值为 0 的位。因此本轮要求清 SYSTEM_FIXED 时，缓存仍保留旧位；“先移除 fixed，再 grant”的 apply 分支也收不到应有的 `flagsToRemove`。对 `denied + SYSTEM_FIXED`，PMS 的 grant 会记录错误并直接 return，而不是抛异常；apply 仍会继续并可能补上 DEFAULT，最终留下 `denied + SYSTEM_FIXED + DEFAULT`。

另一个缺口在 `grantRuntimePermissions()`：`newFlags` 在 permission 循环外创建。每遇到一个 permission，它会 OR 入该项已有的 restriction exemption；这些位不会在下一项前复位。于是较早 permission 的 exemption 可能被带进较晚 permission 的更新掩码和值。虽然真正新增 system exemption 另有精确调用，这个跨项累积仍破坏了“每项只保留自己的旧 exemption”。范围要收紧：它影响一次多 permission 调用中的后项；XML 每个条目传 singleton set，不会在不同 XML permission 条目之间按这条路径串位。

apply 也没有全局恢复保证。它只捕获每项的 `IllegalArgumentException`，uncork 不在 `finally` 中；SecurityException 或其他运行时异常会跳过余下 permission，也可能跳过 `uncorkPackageInfoCache()`。已经执行的 flag、grant 与 exemption 不会回滚。即使全部调用成功，权限持久化仍由后续写调度承担。

### 练习 4：验证工厂查询、provider与缓存缺口

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'String[] requestedPermissions = pkg.requestedPermissions;' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'requestedPermissions[i] = null;' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'private static final int DEFAULT_PACKAGE_INFO_QUERY_FLAGS =' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'mContext.getPackageManager().getPackageInfo(pkg,' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'mServiceInternal.getDisabledSystemPackageName(pkg.packageName));' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'return getPackageInfo(packageName, PackageManager.MATCH_FACTORY_ONLY, context);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'DefaultDialerManager.getDefaultDialerApplication(mContext);' frameworks/base/services/core/java/com/android/server/telecom/TelecomLoaderService.java
grep -n -F 'PhoneAccountHandle phoneAccount = telecomManager.getSimCallManager();' frameworks/base/services/core/java/com/android/server/telecom/TelecomLoaderService.java
grep -n -F 'PhoneAccountHandle phoneAccount = telecomManager.getSimCallManager(userId);' frameworks/base/services/core/java/com/android/server/telecom/TelecomLoaderService.java
grep -n -F 'for (String syncAdapterPackageName : syncAdapterPackageNames) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'final String[] sortedRequestedPermissions = new String[numRequestedPermissions];' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'String permission = requestedPermissions[requestedPermissionNum];' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'int newFlags = PackageManager.FLAG_PERMISSION_GRANTED_BY_DEFAULT;' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'newFlags |= (flags & PackageManager.FLAGS_PERMISSION_RESTRICTION_ANY_EXEMPT);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'state.newFlags |= flagValues & flagMask;' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'if ((flags & PackageManager.FLAG_PERMISSION_SYSTEM_FIXED) != 0) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Log.e(TAG, "Cannot grant system fixed permission "' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F '} catch (IllegalArgumentException e) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'PackageManager.uncorkPackageInfoCache();' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
```

先证明 Default wrapper 缺少 `MATCH_FACTORY_ONLY`，再比较 Telecom 的无 userId 与带 userId 查询，并说明 sync-adapter 数组为 null 时会停在哪里。最后用 P1=SYSTEM_FIXED denied、P2=带一项 restriction exemption、P3=普通权限推演缓存：写出预期与实际 `newFlags`，并先验证 P1 的底层 grant 是静默 return；再假设某个无关的底层 PM 调用抛 RuntimeException，判断后续项与 package-info cache 的完成状态。

## 8. 产品例外XML有分区、资格和危险权限三道门，但没有证书校验

第三阶段由 `DefaultPermissionGrantPolicy` 自己按固定分区收集 `etc/default-permissions`：system、vendor、odm、product、system_ext；只有设备声明 `FEATURE_EMBEDDED` 时才额外读 OEM。它不经过 `SystemConfig`；后者负责的主要是 `etc/sysconfig` 与 `etc/permissions`，所以把这批文件叫成“SystemConfig exceptions”会找错入口。

策略只处理可读的 `.xml` 文件。每个目录直接使用 `listFiles()` 返回顺序，既没有显式排序，也没有对 listFiles 返回 null 单独防护；同包同权限的重复项会按实际遍历顺序依次加入列表和执行。

一个 `<exception package="…">` 首次出现时必须满足：

- 当前可找到 system-image PackageInfo；
- `isSystemPackage(packageInfo)` 为真，也就是 system app 但不是第一阶段那类系统组件；
- targetSdk 大于 Lollipop MR1，支持 runtime 权限模型。

每个 permission 到执行阶段还要是 dangerous。XML schema 虽声明了 `sha256-cert-digest`，r48 的 Java parser 既不读取也不验证它；本类的直接门只有上面的系统包分类，普通 system update 的签名兼容性仍由更早的 PMS 安装链负责。不能把 XSD 字段当成这里已执行的逐例外证书核验。

parser 还有一个宽松判断：它使用 `TAG_PERMISSION.contains(parser.getName())`，不是相等比较。只要标签名是字符串 `permission` 的子串就会进入解析分支。缺 name 会跳过；`fixed` 与 `whitelisted` 都按布尔属性读取。schema 与运行时还存在反向错位：XSD 允许 exception 的 `brand` 与证书摘要，却不声明 permission 的 `whitelisted`；Java 不读前两者，却会读后者。schema 校验通过和运行时语义正确是两件事。

两个属性的真实传参尤其反直觉：

| XML 属性 | 实际进入的参数 | 后果 |
| --- | --- | --- |
| `fixed=true` | `systemFixed=true` | 进入授权分支后，请求写入 DEFAULT 与 SYSTEM_FIXED |
| `whitelisted=true` | `ignoreSystemPackage=true` | 绕开 updated-system 基线分支，并越过 USER_SET、USER_FIXED、SYSTEM_FIXED 的外层门；POLICY_FIXED 仍阻断 |
| 无论 `whitelisted` 值 | `whitelistRestrictedPermissions=true` | 进入授权分支且目标 permission restricted 时，请求 SYSTEM_EXEMPT |

所以 `whitelisted` 在这里并不只是“给 restricted permission 加白名单”。变量名掩盖了更宽的 override 行为；反过来，属性为 false 也不意味着 restricted exemption 不会添加。越过外层 SYSTEM_FIXED 判断也不等于底层 grant 必然成功：`denied + SYSTEM_FIXED` 仍可能被 PermissionManager 拒绝，而延迟缓存的清位缺口又让恢复更困难。

例外表第一次读取后存进 `mGrantExceptions`，供所有用户复用；没有热重载。单个文件解析到中途出错时，catch 只跳过后续读取，之前已加入全局 map 的部分项仍可能被保留并缓存。systemReady 没有实际授予用户时可先异步预读，真正授权前会移除那条消息，并在缓存仍为空时同步读取。此后修复文件、后装包、删除 XML 或删除某个 permission 都不会触发差异撤销；旧 grant、DEFAULT、SYSTEM_FIXED 与 exemption 可能继续存在。

首次同步解析还在 Default policy 的 `mLock` 内做文件 I/O 和 PackageManager 查询；provider setter 则可能先持有 PMS `mLock` 再进入这把锁。两条路径形成结构上的反向锁序可能性，虽然正常启动时多数 provider 会更早注册而降低复现概率。这里应当写“潜在锁序风险”，不能写成已观察到的必现死锁。

重复条目的顺序也不能简单解释成“后文件覆盖前文件”。延迟 flag 缓存大量使用 OR，任一 `fixed=true` 一旦进入就可能粘住；任一能越过门槛的 whitelisted 项也可能先把 grant 改成 true。枚举无序主要破坏可推理性，并在解析中断或 apply 中途异常时决定已经累计、已经应用到哪一项。

### 练习 5：从分区文件追到真实override参数

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'File dir = new File(Environment.getRootDirectory(), "etc/default-permissions");' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'if (mContext.getPackageManager().hasSystemFeature(PackageManager.FEATURE_EMBEDDED, 0)) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'Collections.addAll(ret, dir.listFiles());' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'if (!file.getPath().endsWith(".xml")) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'if (!pm.isSystemPackage(packageInfo)) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'mGrantExceptions = readDefaultPermissionExceptionsLocked(pm);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F '} catch (XmlPullParserException | IOException e) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'if (TAG_PERMISSION.contains(parser.getName())) {' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'final boolean whitelisted = XmlUtils.readBooleanAttribute(parser, ATTR_WHITELISTED);' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F 'grantRuntimePermissions(pm, pkg, permissions, permissionGrant.fixed,' frameworks/base/services/core/java/com/android/server/pm/permission/DefaultPermissionGrantPolicy.java
grep -n -F '<xs:attribute name="sha256-cert-digest" type="xs:string"/>' frameworks/base/services/core/xsd/default-permissions.xsd
```

设计两份文件对同包同权限给出相反的 fixed/whitelisted 组合。仅凭源码列出哪些结果受文件枚举顺序影响，并解释为何填写证书摘要不能证明 r48 运行时做了证书核验。

## 9. Default撤销只认自己的标记，却不会保护Role来源

`revokeRuntimePermissions()` 先取当前 system package，只考虑它仍声明的 permission。目标必须带 `GRANTED_BY_DEFAULT`；POLICY_FIXED 永远跳过，SYSTEM_FIXED 只有调用者也以 fixed 方式撤销时才允许继续。通过后，它先 revoke 实际 permission，再只清 DEFAULT 位。源码刻意让 SYSTEM_FIXED 保持粘滞，restriction exemption 也没有清理，因此可能形成 `denied + SYSTEM_FIXED + exemption`。

这里没有检查 `GRANTED_BY_ROLE`。如果 Role 先有效授予并写 ROLE，后来 Default 又叠加 DEFAULT，那么 Default revoke 仍会撤实际 grant，只留下 ROLE 来源位。反方向也一样危险：Role revoke 只要求 ROLE，不会因 DEFAULT 仍在而保留实际 grant。

顺序还造成另一种不对称：

| 先后顺序 | 叠加后的常见来源位 | 移除后果 |
| --- | --- | --- |
| Default 先使权限有效，Role 后到 | Role 发现整体已经有效，通常不补 ROLE；保留 DEFAULT | 移除 Role 时权限路径通常不动，因为没有 ROLE |
| Role 先使权限有效，Default 后到 | ROLE 与 DEFAULT 可同时存在 | 任一授权器按自己的位撤销，都可能撤掉另一来源仍需要的实际 grant |
| runtime 位已有，但 review 或 AppOp 使整体无效，Role 后到 | Role 会把“先前不有效”视为新角色来源并补 ROLE | 不能仅按旧 runtime 位推断是否有 ROLE |

这不是偶发持久化问题，而是数据模型本身没有来源引用计数。诊断时必须同时记录“谁先执行、当时整体是否有效、后来谁撤销”，不能只打印最终 flags。

## 10. Role资格由通用最小门、required component和行为覆写共同决定

PermissionController 从 `res/xml/roles.xml` 构造 Role：一个角色可声明是否 exclusive、是否 system-only、默认 holder、required component、runtime permissions、AppOp permissions、裸 AppOps 与 preferred activities。角色能力远多于一组权限。

所有候选先经过最小资格：

- 包名不能是 `android`；
- 目标用户必须能取得 enabled 的 ApplicationInfo；
- instant app 被拒绝；
- system-only 角色要求 `FLAG_SYSTEM`；
- 声明 shared library 的应用被拒绝。

然后 RoleBehavior 可以给出非 null 结果，直接覆盖通用 required-component 检查；只有 behavior 返回 null 才要求每个 required component 都找到合格组件。因此“XML 中列了组件门”不保证所有角色都走同一算法。

Browser behavior 就自己查询 HTTP + BROWSABLE activity，并加 MATCH_ALL 与 direct-boot flags，最后要求 `ResolveInfo.handleAllWebDataURI`。Dialer 角色只有设备 `isVoiceCapable()` 时可用。SMS 在 work profile 与 restricted profile 不可用；通常还要求 `isSmsCapable()`，但有配置 default holder 时保留车机例外。SMS fallback 优先取配置 default 的第一项，否则取 qualifying packages 的第一项；源码自己警告后一条可能让第三方应用突然成为默认短信应用并得到权限。

“角色可用”“包有资格”“包是当前 holder”是三层状态。资格检查本身不要求 APK 声明角色所列的每一项 permission；Manifest 交集要到 grant 阶段才发生。因此包可以合法成为 holder，却因为没有请求相应权限而一项 runtime grant 都拿不到。角色不可用也不表示旧权限一定已经撤销，后文的初始化缺口正会打破这种直觉。

### 练习 6：比较Browser、Dialer和SMS资格

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!isPackageMinimallyQualifiedAsUser(packageName, Process.myUserHandle(), context)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Role.java
grep -n -F 'Boolean isPackageQualified = mBehavior.isPackageQualified(this, packageName, context);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Role.java
grep -n -F 'if (Objects.equals(packageName, PACKAGE_NAME_ANDROID_SYSTEM)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Role.java
grep -n -F 'if (!userPackageManager.getDeclaredSharedLibraries(packageName, 0).isEmpty()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Role.java
grep -n -F 'if (!resolveInfo.handleAllWebDataURI) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/BrowserRoleBehavior.java
grep -n -F 'return telephonyManager.isVoiceCapable();' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/DialerRoleBehavior.java
grep -n -F 'if (UserUtils.isWorkProfile(user, context)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/SmsRoleBehavior.java
grep -n -F 'return CollectionUtils.firstOrNull(qualifyingPackageNames);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/SmsRoleBehavior.java
```

为同一个第三方浏览器、系统 Dialer 和 work-profile SMS 包依次跑“角色可用→最小资格→behavior→required component”。明确指出 behavior 非 null 时哪一步不会执行。

## 11. Role.grant会真正使用前景排序，并把permission与AppOp合成有效状态

`Role.grant()` 顺序执行五类动作：`Permissions.grant`、AppOp-permission grant、裸 AppOp grant、preferred activity 配置、RoleBehavior 的附加动作。它们没有统一 rollback。

`Permissions.grant` 先展开旧 targetSdk 对应的 split permissions，再与当前安装包 Manifest 相交。Role 调用时 `overrideDisabledSystemPackage=true`，因此即使目标是 updated system app，也不受 factory APK 请求列表限制。设计上这与普通 Default 初始授权不同；在 r48 中，普通 Default 的查询缺口又让两边实际结果可能意外趋同。

Role 路径确实遍历 `sortedPermissionsToGrant`：有 background permission 的前景项先处理，其他项后处理。授背景 permission 前，还必须至少有一个对应前景 permission 已经“整体有效”。SMS 与 Call Log 组中的 restricted permission 会先进入 system whitelist，然后才执行 fixed 检查与 `grantSingle()`；所以 fixed-denied 阻止 grant 时，白名单仍可能已经改变。

“整体有效”的判定是本章最容易读错的点：

- runtime permission 必须 granted；
- `REVIEW_REQUIRED` 必须未设置；
- 没有 AppOp 的普通 permission 到这里即可；
- 普通 AppOp 必须 ALLOWED；
- 前景 permission 的 AppOp 可为 FOREGROUND 或 ALLOWED；
- 背景 permission 要找到至少一个前景 AppOp 为 ALLOWED。

Role 是否补 `GRANTED_BY_ROLE` 看的是授予前这个整体判定，而不是只看 runtime 位。旧 runtime 位已经 granted，但 AppOp 为 ignored 或仍需 review 时，`wasPermissionOrAppOpGranted` 仍为 false。只有它随后没有被 POLICY_FIXED/SYSTEM_FIXED 拦住，并按调用参数越过用户位进入修改分支，才会补 ROLE、把 USER_SET/USER_FIXED 纳入清理 mask。若授予前已经整体有效，则不会补 ROLE，以免角色移除时撤走原本由用户或 Default 提供的授权。

固定门也要按参数读。Role grant 传 `overrideSystemFixed=false`，所以 POLICY_FIXED 与 SYSTEM_FIXED 始终阻断无效状态的修复；USER_SET/USER_FIXED 是否阻断取决于 `overrideUserSetAndFixedPermissions`。重新确认已有合格 holder 时传 false，默认、fallback 与显式添加 holder 时传 true。这个参数只放开入口，不代表每次调用都必然清用户位。

### 练习 7：推演前景、背景、AppOp与ROLE位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'CollectionUtils.retainAll(permissionsToGrant, packageInfo.requestedPermissions);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'permissionsToGrant.addAll(splitPermission.getNewPermissions());' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'String permission = sortedPermissionsToGrant[i];' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'if (!isAnyForegroundPermissionGranted) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'boolean wasPermissionOrAppOpGranted = isPermissionAndAppOpGranted(packageName, permission,' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'newFlags |= PackageManager.FLAG_PERMISSION_GRANTED_BY_ROLE;' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'newMask |= PackageManager.FLAG_PERMISSION_USER_FIXED' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'fixedFlags |= PackageManager.FLAG_PERMISSION_SYSTEM_FIXED;' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'fixedFlags |= PackageManager.FLAG_PERMISSION_USER_FIXED' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'ArraySet<String> permissionsToRevoke = new ArraySet<>(permissions);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
grep -n -F 'CollectionUtils.retainAll(permissionsToRevoke, packageInfo.requestedPermissions);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Permissions.java
```

把前景 location 与 background location 假设为一个产品自定义 Role 的权限集合；r48 原生 `roles.xml` 没有引用这组背景权限。分别从 AppOp ignored、MODE_FOREGROUND、MODE_ALLOWED 起步，计算授予前的“整体有效”、是否补 ROLE、是否能授背景项。再用一个会发生 split 的旧 target 权限比较 grant/revoke 集合，说明为何 Default 的无效排序和 Role 的 split 残留不能混为一个问题。

## 12. changed返回值会漏报，显式kill因此不能当成完整变更证明

`grantPermissionAndAppOp()` 先记录 runtime grant 是否改变，再处理 AppOp。r48 有两处覆盖错误：

- permission 没有关联 AppOp 时直接 `return false`，丢掉刚发生的 runtime grant change；
- 非背景 permission 有 AppOp 时，用 `permissionOrAppOpChanged = setAppOpUidMode(...)` 覆盖原值，而不是 OR；若 runtime 位刚改变但 AppOp 原本已是目标 mode，最终仍返回 false。

revoke 的普通 AppOp 分支使用 OR，但无 AppOp 时也直接返回 false，仍会漏掉刚发生的 runtime revoke。上层 `Permissions.grant/revoke` 汇总这些返回值，`Role.grant/revoke` 又只用该汇总决定自己的显式 kill；AppOp-permission、裸 AppOp、preferred activity 与 behavior 的 changed 都没有并入。

grant 只在“不要求 dont-kill、汇总声称有变化、目标应用不支持 runtime permission”时显式 kill，也就是主要照顾 legacy target。revoke 在不要求 dont-kill 且汇总有变化时对所有 target 显式 kill。普通 PM/AppOps API 自己可能另有进程或缓存影响，但这不修正 Role 这一本地 changed 语义：看到 Role 没有显式 kill，不能反推所有能力都没变。

另一个细节是排他角色切换。调用方可对“新 holder 的添加”传 `DONT_KILL_APP`，但控制器移除旧 holder 时硬编码 `dontKillApp=false`；这个 flag 没有传给旧 holder。因此一次“不杀应用”的切换请求仍可能杀掉原 holder。最终调用还是 `ActivityManager.killUid()`，在 shared UID 场景影响的是整个 UID，不只是传入包名。

## 13. Role.revoke先减去其他角色，却仍有来源、split和preferred残留

撤销角色前，`Role.revoke()` 查询该包仍持有的其他角色，并从待撤 permission、AppOp-permission 与裸 AppOp 集合中减去其他角色所需项。这是角色之间的集合保护，但只覆盖仍存在于当前 Role map 的角色定义；它不是对 Default 或用户来源的通用引用计数。若旧持久化状态尚未来得及被 `setRoleNames()` 清理，或竞态窗口中 holder 记录含有当前定义表不存在的角色名，代码直接解引用 `roles.get(roleName)`，存在空对象风险；正常初始化会先删除这类角色，不能把它当成常态。

`Permissions.revoke` 先与当前 Manifest 相交，再按“背景先、非背景后”执行。与 grant 不同，revoke 不展开 split permission：旧权限在 grant 时派生出的新 permission 若不在 Role 原始列表中，角色移除后可能仍带 ROLE 和实际 grant。

单项撤销的次序是先验证本项带 ROLE，然后立刻清 ROLE 位，之后才检查 fixed 与前后台约束。Role 路径在这里不允许覆盖 USER_SET、USER_FIXED、POLICY_FIXED 或 SYSTEM_FIXED；任一 fixed 位挡住有效授权的 revoke 时，实际 grant 会保留，ROLE 却已经丢失。它也不检查 DEFAULT；ROLE 先授、DEFAULT 后叠加且没有 fixed/背景门阻断时，移除角色会清 ROLE 并撤实际 grant，留下 `denied + DEFAULT`。

背景项先撤，是为了把对应前景 AppOp 从 ALLOWED 降到 FOREGROUND；前景项若仍有整体有效的背景 permission 则暂不撤。restricted system whitelist 在循环尾只要发现该项不再带 DEFAULT 就尝试移除，不要求本次 `revokeSingle()` 真正撤销成功。

三类能力的 Manifest 规则也不统一：runtime permission grant/revoke 都与当前请求列表求交；`AppOpPermissions.grant()` 检查 Manifest，而 revoke 不检查；裸 AppOp 完全不依赖 Manifest。裸 AppOp 自己的 `maxTargetSdkVersion` 门同时用于 grant 与 revoke，包持有角色期间若把 targetSdk 升过门槛，旧 allowed mode 可能因为撤销也被门挡住而残留。

preferred activity 在 r48 明确不做 revoke，因为多数相关角色有 fallback，清理还可能产生误导性的 preferred-activity 变化通知。

因此“holder 已移除”甚至不能证明控制器实际执行了角色特权回收：`removeRoleHolderInternal()` 取不到 ApplicationInfo 时会跳过 `Role.revoke()`，仍继续删除 holder。即使 revoke 已调用，也不保证没有 split、裸 AppOp 或 preferred activity 残留，更不保证 Default 或用户来源得到正确保护。

## 14. RoleController初始化、排他切换与fallback都是有序但非事务的

`onGrantDefaultRoles()` 先收集当前用户可用角色，记录哪些角色第一次出现，然后调用 `setRoleNamesFromController()`。r48 明确没有先清理由于角色变为 unavailable 而即将删除的 holder。system_server 的 `RoleUserState.setRoleNames()` 会直接删掉旧角色及 holder 记录；对应 permission、AppOps 与 preferred activity 没有经过 `Role.revoke()`，可形成“记录消失、特权残留”。

对仍可用的角色，控制器逐个检查现有 holder：

- 仍合格：重做 grant，但不覆盖 USER_SET/USER_FIXED；
- 不再合格：有 ApplicationInfo 时先尝试回收特权，再删 holder；取不到时直接跳过回收；
- 已无 holder：新增加的角色先取 default，其他情况或 default 列表为空时才取 fallback；
- default/fallback grant 允许越过 USER_SET/USER_FIXED；真正进入修改分支时才清相应用户位；
- exclusive 角色有多个 holder 时保留查询结果中的第一个，移除其余；源码没有更强的优先判据。

这里还有一个兜底空洞：`Role.getDefaultHolders()` 会先用较弱的门排除不存在或非 system 的配置包，却不会预先执行完整 Role 资格。只要弱过滤后的 default 列表非空，就不会先验证 required component/behavior 再决定是否调用 fallback。若配置包通过弱门、随后完整资格失败，循环只会跳过它，本轮不会再尝试原本可用的 fallback；方法最后仍返回 true，system_server 仍可更新 packages hash，直到包组件状态再次变化前都不会自动重跑。

显式添加 exclusive holder 时，控制器先移除所有旧 holder，再为新包 grant，最后把新 holder 写入 RoleManager。中途失败没有恢复旧 holder。显式移除也不先验证目标确为 holder：只要包信息存在就先调用 revoke，随后 `RoleUserState.removeRoleHolder()` 只要 role name 存在便返回 true，即使集合根本未改变。一个“移除非 holder”的请求因此也可能重置 AppOp-permission、裸 AppOp 或 behavior 状态，并报告成功。

正常显式移除在删记录后还要尝试 fallback；fallback 失败会让整个调用返回 false，但原 holder 已经移除，false 不是“什么都没发生”。remove/clear 的 DONT_KILL_APP 也没有传给 fallback 添加所用的三参数 helper，fallback 内部会以 `dontKillApp=false` 执行。

`addRoleHolderInternal` 的顺序始终是“先授权限/AppOps/偏好，再记 holder”；`removeRoleHolderInternal` 则是“包信息存在时先尝试回收，再删 holder”。如果第二步返回失败、包信息缺失或进程在两步之间退出，记录与特权可以分叉。`Role.grant/revoke` 自身都是 void，静态 permission 的 changed 只影响显式 kill，其他能力的返回还被忽略；所以 holder 回调为 true 也不证明每一类特权都操作成功。

### 练习 8：给三条非事务路径标故障点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mRoleManager.setRoleNamesFromController(roleNames);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'if (!roleNames.contains(roleName)) {' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'mRoles.removeAt(i);' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'addRoleHolderInternal(role, packageName, false, false, true);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'removeRoleHolderInternal(role, packageName, false);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'addRoleHolderInternal(role, packageName, true);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'boolean removed = removeRoleHolderInternal(role, currentPackageName, false);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'role.grant(packageName, dontKillApp, overrideUserSetAndFixedPermissions, this);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'added = mRoleManager.addRoleHolderFromController(roleName, packageName);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'role.revoke(packageName, dontKillApp, false, this);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'if (applicationInfo != null) {' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'boolean removed = mRoleManager.removeRoleHolderFromController(roleName, packageName);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
grep -n -F 'boolean fallbackSuccessful = addFallbackRoleHolderMaybe(role);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/service/RoleControllerServiceImpl.java
```

分别在“旧 exclusive holder 已删、新 holder 未加”“新包特权已授、holder 未记”“原 holder 已删、fallback 失败”处终止流程。写出 API 返回、holder 记录和权限/AppOps 三列，不要把 false 解释成回滚。

## 15. role操作至少跨四个持久化时钟

Role holder 记录由 system_server 的 `RoleUserState` 管理。add/remove 在 `mLock` 下修改集合；只要 role name 存在就返回 true，即使 holder 原本已存在或已经不在集合中。只有集合真正变化时才安排写并通知 callback。因此这个布尔值连“holder 集合发生变化”都不表示，更不表示落盘成功。

写任务使用 BackgroundThread Handler，以 200 ms `sendMessageDelayed` 投递；这不是落盘上限，队列、锁竞争、快照和 I/O 都会继续拉长时间。任务取锁生成 `RolesState` 快照，出锁后调用 Permission APEX 的 `RolesPersistence.writeForUser()`。新实现路径位于每用户 Permission APEX 的 device-protected 目录，文件名同样是 `roles.xml`；旧的 `/data/system/users/<id>/roles.xml` 只用于新文件不存在时迁移读取。

底层使用 `AtomicFile` 的 start/finish/fail 写法，但捕获所有 Exception 后只记录 wtf，不把失败返回给 `RoleUserState`。写任务在 I/O 前已经把 `mWriteScheduled` 清为 false，失败也没有专门重排。AtomicFile 能保护单次替换免于留下半文件，不能让跨文件操作成为事务。

RoleControllerManager 对单个远端请求还设置 15 秒超时。超时可让调用侧收到失败，却不会给已经在 PermissionController worker 上执行的多步授权提供取消或回滚；后续步骤仍可能改变状态。这是第四个“返回与实际副作用分离”的边界。

一次 Role grant 至少涉及四个主要持久化时钟：

| 状态 | 内存改变位置 | 持久化路径 | RoleController 返回时是否保证提交 |
| --- | --- | --- | --- |
| runtime grant 与 permission flags | system_server PermissionManager | 每用户 `runtime-permissions.xml` | 否 |
| permission 关联 AppOp、AppOp-permission、裸 AppOp | system_server AppOpsService | AppOps 自己的状态文件 | 否 |
| role holder、role version、packages hash | system_server `RoleUserState` | 每用户 `roles.xml` | 否 |
| preferred activity | system_server PackageManager | 每用户 `package-restrictions.xml`，通常再按 10 秒消息调度 | 否 |

`RoleUserState` 内部的 holder-change callback 在集合改变后立即调用，它不等待磁盘；通常早于 200 ms 写任务，但并没有与后台 I/O 建立严格完成屏障。它也不是 RoleController API 最终 callback。默认角色成功后设置 packages hash 仍只是再安排一次写。设备若在四个时钟之间崩溃，重启可能看到 holder 与能力不一致；包哈希是否已经提交又决定初始化会不会主动重跑修复。

若 UI 允许选择“None”，该选择还写到 PermissionController 的 device-protected SharedPreferences，并使用异步 `apply()`，构成可选的第五个时钟。它不属于 `roles.xml` holder 集合。

Default policy 没有 role holder 这本账，但同样只是同步完成一串权限 API 调用；runtime 文件仍走第264章的独立写调度。因此“不涉及 Role”也不等于拥有单文件事务完成点。

### 练习 9：区分内存成功、回调成功与多文件提交

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final long WRITE_DELAY_MILLIS = 200;' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'private static final long REQUEST_TIMEOUT_MILLIS = 15 * 1000;' frameworks/base/core/java/android/app/role/RoleControllerManager.java
grep -n -F 'changed = roleHolders.add(packageName);' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'mCallback.onRoleHoldersChanged(roleName, mUserId, null, packageName);' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'mWriteHandler.sendMessageDelayed(PooledLambda.obtainMessage(RoleUserState::writeFile,' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'mPersistence.writeForUser(roles, UserHandle.of(mUserId));' frameworks/base/services/core/java/com/android/server/role/RoleUserState.java
grep -n -F 'ApexEnvironment apexEnvironment = ApexEnvironment.getApexEnvironment(APEX_MODULE_NAME);' frameworks/base/apex/permission/service/java/com/android/role/persistence/RolesPersistenceImpl.java
grep -n -F 'AtomicFile atomicFile = new AtomicFile(file);' frameworks/base/apex/permission/service/java/com/android/role/persistence/RolesPersistenceImpl.java
grep -n -F 'atomicFile.finishWrite(outputStream);' frameworks/base/apex/permission/service/java/com/android/role/persistence/RolesPersistenceImpl.java
grep -n -F 'atomicFile.failWrite(outputStream);' frameworks/base/apex/permission/service/java/com/android/role/persistence/RolesPersistenceImpl.java
grep -n -F 'packageManager.replacePreferredActivity(intentFilter, match, set, packageActivity);' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/PreferredActivity.java
grep -n -F 'static final int WRITE_SETTINGS_DELAY = 10*1000;  // 10 seconds' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mHandler.sendEmptyMessageDelayed(WRITE_PACKAGE_RESTRICTIONS, WRITE_SETTINGS_DELAY);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '.putBoolean(Constants.IS_NONE_ROLE_HOLDER_SELECTED_KEY + mName, true)' packages/apps/PermissionController/src/com/android/permissioncontroller/role/model/Role.java
```

在 holder 内存改变后、runtime 文件提交后、AppOps 文件提交后、preferred activity 已改而 package restrictions 未写、roles 文件失败后分别模拟掉电。对每个场景写出重启可见状态，并指出哪个公开返回值能确认到哪一步。

## 16. 用来源转换矩阵收尾，调试时按状态而不是方法名取证

一条可靠诊断记录至少要同时包含：userId、package/uid、permission grant、全部 permission flags、相关 AppOp mode、当前 held roles、包是否 updated system app、工厂 APK 是否声明该 permission、默认例外条目及角色资格。只搜索 `grantRuntimePermission` 会漏掉候选过滤、override、AppOp 和 holder 写入；只看 `dumpsys package` 也会漏掉 Role 与 AppOps。

可以用下面的转换矩阵检查最危险的场景：

| 初态与动作 | 预期观察重点 | r48 特有风险 |
| --- | --- | --- |
| 用户 fixed deny，普通 Default 扫描 | grant 不变，用户位保留 | 产品例外 `whitelisted=true` 可越过用户门 |
| SYSTEM_FIXED denied，再要求 fixed grant | 应先清 fixed 再 grant 再补 fixed | 延迟缓存只 OR，清位请求可能丢失 |
| P1 有 exemption，随后处理 P2 | 每项保留自己的 exemption | 循环外 `newFlags` 可能把 P1 的位带给 P2 |
| updated system app 新增 Manifest 权限 | 注释意图要求工厂与当前 Manifest 双重声明 | Default 普通查询缺 `MATCH_FACTORY_ONLY`，实际可能把新增项放进候选；Role/override 也不受基线限制 |
| Default 有效后再成为 Role | 通常只有 DEFAULT | Role 移除不能据此证明没有别的角色能力 |
| Role 有效后再叠加 Default | ROLE + DEFAULT | 任一方撤销都可能撤实际 grant，不保护另一来源 |
| Role grant 展开 split，随后移除 | 原始项与派生项都应核对 | revoke 不展开 split，派生项可能残留 |
| 角色从 available 变 unavailable | holder 与所有特权都应收敛 | 初始化先删角色记录，未调用 revoke |
| holder API 返回 true | 检查集合是否真变、各文件是否提交 | true 只保证 role name 存在 |

排查顺序建议保持固定：

1. 先确认触发入口和 userId，避免把升级、新用户、包变化与人工切换混在一起；
2. 再确认资格与候选集合，包括 current/factory Manifest、split 与 restricted 条件；
3. 记录授权前的完整 grant/flags/AppOp/roles 快照；
4. 按 Default 或 Role 的真实操作顺序逐项推演；
5. 最后分别验证 runtime、AppOps、roles 三个持久化完成点与重启结果。

至此，第263章的运行时授权状态、第264章的持久化链和本章的自动来源链已经接上。下一章转向 PermissionController 的 `Permission`、`AppPermissionGroup` 与轻量 LiveData 模型，解释用户看到的组级“允许/仅使用时/始终/拒绝”怎样拆成逐 permission grant、背景 modifier、flags 与 AppOp 状态。
