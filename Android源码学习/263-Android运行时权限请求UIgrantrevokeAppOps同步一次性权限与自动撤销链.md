# Android运行时权限请求：UI决策、grant/revoke、AppOps、一次性会话与自动撤销链

## 1. requestPermissions是一条多账本协议，不是一次grant调用

本文以 AOSP android-11.0.0_r48 为边界。第262章解释安装或更新后怎样重建权限状态；本章只看应用已经安装之后，用户发起权限请求、系统界面作出决定、system_server 改状态，以及两种自动撤销怎样结束授权。

一句话答案是：Activity.requestPermissions 只启动受信任的 PermissionController 界面；界面依据原始调用包和当前模型作决定，再通过有特权的 Binder 调用分别修改 runtime grant、permission flags 和 AppOps。PMS 的返回、应用回调、AppOps 收敛、进程终止、XML 落盘、一次性会话结束和长期不用扫描，是彼此不同的完成点。

需要同时盯住六本账：

| 账本 | 典型状态 | 直接回答的问题 |
| --- | --- | --- |
| runtime permission | granted / denied | 包在该用户下是否持有权限位 |
| permission flags | USER_SET、USER_FIXED、ONE_TIME、AUTO_REVOKED 等 | 这次状态为什么形成、以后能否再问 |
| raw AppOps | ALLOWED、FOREGROUND、IGNORED、DEFAULT | UID 或 package 当前保存的 op 模式 |
| effective AppOps | 由 raw mode、UID state、capability 等求值 | 此刻一次具体访问是否放行 |
| 会话与扫描状态 | one-time listener、alarm、periodic job | 未来由谁触发撤销 |
| 持久化文件 | packages.xml、runtime-permissions.xml、appops.xml | 重启后能恢复到什么状态 |

主源码集中在 Activity.java、PackageManager.java、GrantPermissionsActivity.java、AppPermissionGroup.java、PermissionManagerService.java、PermissionPolicyService.java、AppOpsService.java、OneTimePermissionUserManager.java、PermissionControllerServiceImpl.java 和 AutoRevokePermissions.kt。阅读时若把其中任何一次方法返回当成“六本账都完成”，后面的时序都会判断错。

## 2. Activity入口只为直接请求建立实例级闸门

直接调用 Activity.requestPermissions 时，负 requestCode 立即抛异常；permissions 的 null 或空数组由 PackageManager.buildRequestPermissionsIntent 拒绝。通过输入校验后，Activity 检查 mHasCurrentPermissionsRequest：同一个 Activity 实例已有一组直接请求时，新请求收到空数组回调，不再启动界面。

这个布尔值在发起后置 true，在权限专用结果分发前清 false，并随 Activity 的实例状态保存和恢复。因此它能覆盖旋转重建，却不是进程级、UID 级或系统级互斥锁。

还要保留一个 r48 特例：平台 android.app.Fragment 的 HostCallbacks 路径自行拼接 who 并直接 startActivityForResult，既不读取也不写入 mHasCurrentPermissionsRequest。同一 Activity 中，“直接 Activity 请求串行”并不能推出“所有 Fragment 请求也串行”；并发结果仍靠不同 who 和 requestCode 路由。

### 练习 1：验证直接Activity闸门与Fragment旁路

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public final void requestPermissions(@NonNull String[] permissions, int requestCode) {' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'if (mHasCurrentPermissionsRequest) {' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'startActivityForResult(REQUEST_PERMISSIONS_WHO_PREFIX, intent, requestCode, null);' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'mHasCurrentPermissionsRequest = true;' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'storeHasCurrentPermissionRequest(outState);' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'restoreHasCurrentPermissionRequest(icicle);' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'onRequestPermissionsFromFragment(Fragment fragment, String[] permissions,' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'String who = REQUEST_PERMISSIONS_WHO_PREFIX + fragment.mWho;' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'mHasCurrentPermissionsRequest = false;' frameworks/base/core/java/android/app/Activity.java
```

先画两条时间线：Activity 直接请求会经过 bool 闸门；平台 Fragment 请求不经过。再模拟旋转，确认保存的是“已有直接请求”这一事实，而不是 PermissionController 当前页面的完整状态。

## 3. 显式Intent限定处理者，resultTo链确认原始调用包

PackageManager.buildRequestPermissionsIntent 创建 ACTION_REQUEST_PERMISSIONS Intent，放入原始权限数组，并把 package 限定为系统选出的 PermissionController 包。这能防止普通应用抢占隐式 Intent，但 setPackage 只限定目标包，不是调用者身份凭据，也没有固定到某个组件。

原始 App 的身份来自 ActivityTaskManager 的 resultTo 关系。PermissionController 的 GrantPermissionsActivity 在 onCreate 缓存 getCallingPackage；源码注释指出以后再读可能得不到它。GrantPermissionsActivity 随后重新读取这个包的 PackageInfo、requestedPermissions、targetSdk 和 UID，而不是相信 Intent 自报的包名。

这里有两层身份，不能混为一谈：

| 层次 | 身份 | 用途 |
| --- | --- | --- |
| UI 请求来源 | ATMS 的 resultTo 所指 Activity 包 | 决定为哪个原 App 展示和计算结果 |
| PMS Binder caller | PermissionController UID | 接受 GRANT/REVOKE 特权与跨用户检查 |

PermissionController Manifest 自身持有 GRANT_RUNTIME_PERMISSIONS 和 REVOKE_RUNTIME_PERMISSIONS。普通 App 不会因为启动了这个界面而获得这些 Binder 权限；它只是成为受信任 UI 选定的目标 package。

入口还叠加两类防遮挡：主题打开 filterTouchesWhenObscured，Activity 窗口设置 HIDE_NON_SYSTEM_OVERLAY_WINDOWS。setFinishOnTouchOutside(false) 只说明触摸窗口外不结束流程，不能替代前两项安全控制。

### 练习 2：分离Intent目标、UI来源与Binder调用者

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public Intent buildRequestPermissionsIntent(@NonNull String[] permissions) {' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'Intent intent = new Intent(ACTION_REQUEST_PERMISSIONS);' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'intent.setPackage(getPermissionControllerPackageName());' frameworks/base/core/java/android/content/pm/PackageManager.java
grep -n -F 'return r != null ? r.info.packageName : null;' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
grep -n -F 'return r.resultTo;' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
grep -n -F 'mCallingPackage = getCallingPackage();' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F '<uses-permission android:name="android.permission.GRANT_RUNTIME_PERMISSIONS" />' packages/apps/PermissionController/AndroidManifest.xml
grep -n -F '<uses-permission android:name="android.permission.REVOKE_RUNTIME_PERMISSIONS" />' packages/apps/PermissionController/AndroidManifest.xml
grep -n -F 'android:theme="@style/GrantPermissions.FilterTouches"' packages/apps/PermissionController/AndroidManifest.xml
grep -n -F '<item name="android:filterTouchesWhenObscured">true</item>' packages/apps/PermissionController/res/values/themes.xml
grep -n -F 'getWindow().addSystemFlags(SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
```

把“谁发起 UI”“谁调用 PMS”“谁是被修改目标”分别写在纸上。若三处都写成原 App UID，就能立刻发现身份模型被压扁了。

## 4. 原始数组、Manifest权限和UI group是三种不同集合

GrantPermissionsActivity 只把“整个数组为 null”归一成空数组；null 元素在遍历 UI 时被跳过，重复元素也不会从原始数组删除。它随后重读调用包 Manifest：包不存在、没有 requestedPermissions，或 targetSdk 小于 M，都会以空或当前结果尽早结束。pre-M 包因此不进入现代请求按钮链；Java/Kotlin 模型中为 legacy App 处理 AppOp 的代码属于设置页、审查或其他消费者。

真正进入 UI 的 affectedPermissions 还会扩张：

- split permission 可把一个旧权限扩为多个新权限；
- targetSdk 不高于 N_MR1 时，一个请求可扩为旧式 group 内多个权限；
- 相同 affected permission 会在内部集合去重；
- group 是展示与批量决策单位，runtime grant 仍以单个 permission 为状态键。

foreground 与 background 会成为同组的两个 GroupState。targetSdk 至少为 R 时，background permission 必须单独请求；r48 用原始 mRequestedPermissions.length 大于 1 判断，所以“background + 重复项、null 或无效项”也会命中限制。代码调用 finish 后没有 return，后续初始化仍可能继续，但 mResultSet 会保住第一次构造的 canceled 结果。

按钮结果不是一个布尔值：

| UI决定 | foreground | background | flags侧重点 |
| --- | --- | --- | --- |
| 始终允许 | grant | grant（若流程允许） | 清 ONE_TIME |
| 仅在使用中 | grant | revoke | 清 ONE_TIME |
| 仅限这一次 | grant | revoke | 非 background 项置 ONE_TIME |
| 拒绝 | revoke | revoke | 清 ONE_TIME |
| 拒绝且不再询问 | revoke | revoke | 置 USER_FIXED，清 ONE_TIME |

DevicePolicy 的自动允许或拒绝还能通过同一 Java 模型设置 POLICY_FIXED。按钮是否展示受设备形态、targetSdk、foreground 已有状态、固定 flags、restricted policy 等共同限制，不能从 switch 分支反推“每个应用必有这些按钮”。

### 练习 3：从原始数组推导affected集合和按钮结果

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mRequestedPermissions = getIntent().getStringArrayExtra(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (requestedPermission == null) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (callingPackageInfo.applicationInfo.targetSdkVersion < Build.VERSION_CODES.M) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'ArrayList<String> affectedPermissions =' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'extendedBySplitPerms.addAll(splitPerm.getNewPermissions());' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (requestingAppTargetSDK <= Build.VERSION_CODES.N_MR1) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F '>= Build.VERSION_CODES.R && mRequestedPermissions.length > 1' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'case GRANTED_ONE_TIME:' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'case DENIED_DO_NOT_ASK_AGAIN :' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'groupState.mGroup.setOneTime(true);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (!mResultSet) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'int numRequestedPermissions = mRequestedPermissions.length;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'grantResults[i] = pm.checkPermission(mRequestedPermissions[i], mCallingPackage);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'setResultIfNeeded(RESULT_CANCELED);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'setResultIfNeeded(RESULT_OK);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'dispatchRequestPermissionsResult(requestCode, data);' frameworks/base/core/java/android/app/Activity.java
grep -n -F 'onRequestPermissionsResult(requestCode, permissions, grantResults);' frameworks/base/core/java/android/app/Activity.java
```

构造一个含五项的数组：CAMERA 重复两次、一个 null、一个无效名字、一个 R 应用的 background location。分别记录原始回调数组、affectedPermissions 和 GroupState，三列不会相同。

## 5. 请求UI走Java模型，而且一次选择会产生多次立即持久化

GrantPermissionsActivity 构造 AppPermissions 时传入 delayChanges=false；四参构造器最终也把 false 交给完整构造器。因此按钮路径使用 Java AppPermissionGroup 的立即持久化，不是 KotlinUtils 的轻量模型，也不是先缓存全部变化后一次提交。

“仅限这一次”的真实顺序是：

1. setOneTime(true) 遍历组内所有非 background permission，修改模型并立即 persistChanges(false)；
2. grantRuntimePermissions 只用 affectedPermissions 过滤本次 grant，再次触发持久化；
3. 每次 persistChanges 又逐 permission 发出 grant/revoke、updatePermissionFlags 和 AppOp 调用；
4. 遍历完成后才根据组快照决定 start 或 stop one-time session。

普通允许会先独立清 ONE_TIME，再 grant；拒绝会先 revoke，再独立清 ONE_TIME。setOneTime 的组范围还可能大于本次 affectedPermissions 范围。这里既没有数据库事务，也没有 compare-and-set：进程异常、另一个管理入口并发修改或 shared UID 的另一个包参与时，都可能观察到或放大中间状态。

Java mask 明确包含 POLICY_FIXED、排除 SYSTEM_FIXED，并总把 AUTO_REVOKED 放进 mask 而不放进 values，从而在普通用户决策时清除自动撤销标记。system-fixed permission 不由这个模型改 grant/revoke；policy-fixed 则能由策略分支主动设置。KotlinUtils 的 changed flag mask 是另一套实现，不能拿它解释请求 UI。

AppPermissionGroup 的 grant/revoke 返回 boolean，但 GrantPermissionsActivity 不据此回滚整组；多 permission 操作可能部分成功。最终界面只能重新检查每个原始项，无法把“一次点击”升级为全有或全无的承诺。

### 练习 4：按源码顺序展开一次one-time点击

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mAppPermissions = new AppPermissions(this, callingPackageInfo, false,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'this(context, packageInfo, sortGroups, false, onErrorCallback);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissions.java
grep -n -F 'groupState.mGroup.setOneTime(true);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'groupState.mGroup.grantRuntimePermissions(true, doNotAskAgain,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/ui/GrantPermissionsActivity.java
grep -n -F 'if (!permission.isBackgroundPermission()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'if (!mDelayChanges) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'public void persistChanges(boolean mayKillBecauseOfAppOpsChange, String revokeReason) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mPackageManager.grantRuntimePermission(mPackageInfo.packageName,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mPackageManager.revokeRuntimePermission(mPackageInfo.packageName,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'mPackageManager.updatePermissionFlags(permission.getName(),' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F '| PackageManager.FLAG_PERMISSION_POLICY_FIXED' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F '| PackageManager.FLAG_PERMISSION_AUTO_REVOKED, // clear auto revoke' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'shouldKillApp |= allowAppOp(permission, uid);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'shouldKillApp |= disallowAppOp(permission, uid);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F 'if (isOneTime() && areRuntimePermissionsGranted()) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
grep -n -F '.startOneTimePermissionSession(packageName,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/model/AppPermissionGroup.java
```

不要只列方法名。把第一次 setOneTime 的 flags/AppOp/session、第二次 grant 的 permission/flags/AppOp/session分别列出，便能看到中间态为什么真实存在。

## 6. PMS重新校验特权、用户、目标包、权限定义与fixed状态

ApplicationPackageManager 的 grant 在 r48 仍经 IPackageManager 包装转发，revoke 已直接调用 IPermissionManager；二者最终进入 PermissionManagerService 的内部实现。服务端不信任 UI 已做完的判断，而会重新检查调用者持有 GRANT 或 REVOKE_RUNTIME_PERMISSIONS、跨用户权限、用户是否存在、包可见性、权限定义、目标是否声明或由 shared 权限状态承载、runtime/development 类型、legacy、instant app、restricted 和 fixed flags。

“校验失败”没有单一返回形态。未知 permission 可抛 IllegalArgumentException；某些 fixed revoke 抛 SecurityException；grant 遇 system-fixed、无 override 的 policy-fixed 或 restricted 常记录后直接返回；未知用户或包也常返回。调用方不能只靠“没有异常”推断发生了状态变化。

成功 grant 修改 PermissionsState；若 GID 变化，callback 会安排杀 UID，随后触发 permission listener、设置写入和 runtime state 通知。成功 revoke 也先改内存状态，再调用 callback 和 runtime state 通知。默认 callback 的关键差异是：

- grant 使用 writeSettings(true)，主设置写入走延迟路径；
- revoke 使用 writeSettings(false)，packages.xml 主写同步执行，但 writeLPr 末尾的 runtime-permissions 文件仍另行调度；
- revoke kill 投递到 PMS 的 ServiceThread；
- PermissionPolicy 同步走 FgThread，两条队列没有顺序保证。

所以 Binder 返回可说明这次同步服务调用已走完或提前返回，却不能说明 kill 已执行、AppOps 已收敛或各 XML 已 durable。

### 练习 5：为PMS每一种拒绝建立结果表

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void grantRuntimePermission(String packageName, String permName, final int userId) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'android.Manifest.permission.GRANT_RUNTIME_PERMISSIONS,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'android.Manifest.permission.REVOKE_RUNTIME_PERMISSIONS,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (!mUserManagerInt.exists(userId)) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (pkg == null || ps == null) {' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'bp.enforceDeclaredUsedAndRuntimeOrDevelopment(pkg, ps);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (pkg.getTargetSdkVersion() < Build.VERSION_CODES.M' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'if (bp.isHardRestricted()' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'Cannot grant system fixed permission ' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'throw new SecurityException("Cannot revoke policy fixed permission "' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'final int result = permissionsState.grantRuntimePermission(bp, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'permissionsState.revokeRuntimePermission(bp, userId)' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'callback.onPermissionGranted(uid, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'callback.onPermissionRevoked(UserHandle.getUid(userId,' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(true);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mPackageManagerInt.writeSettings(false);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'mHandler.post(() -> killUid(appId, userId, KILL_APP_REASON_PERMISSIONS_REVOKED));' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
```

为未知 user、未知 package、未知 permission、legacy、system-fixed、policy-fixed、hard restricted 和成功变化分别填“返回、日志、异常、callback、kill、写入”。这比笼统记“PMS 会校验”更可执行。

## 7. 应用回调只返回原始数组的permission-check视图

GrantPermissionsActivity 完成时按原始 mRequestedPermissions 的长度创建 grantResults，并逐项调用 PackageManager.checkPermission。原始顺序、重复项和 null 都保留；null 得到 denied。split 扩展项、旧目标 group 扩展项不会额外出现在回调里。

这个检查只反映当时内存中的 permission grant 视图，不携带 ONE_TIME、USER_SET、USER_FIXED 或 AUTO_REVOKED，也不读取 AppOps raw/effective mode，更不等待 PermissionPolicy、kill 或磁盘写入。checkPermission 还受 FULLER_PERMISSION_MAP 等兼容规则影响，例如更强权限可能让较弱权限的检查成立。PERMISSION_GRANTED 因而不是“完整能力已经可永久使用”。

Activity.dispatchActivityResult 识别 REQUEST_PERMISSIONS_WHO_PREFIX 后，完全丢弃 resultCode，只把 requestCode 和 data 交给权限专用分发。App 观察不到权限 Activity 的 RESULT_OK 或 RESULT_CANCELED。正常结果数组也可混合 granted/denied；空数组则可能来自并发闸门、pre-M 退出、界面中断或 controller 崩溃，不能唯一反推原因。

设备 Back 的差异进一步说明 resultCode 不可靠：手持实现可把 Back 映射为 UI 的 CANCELED，随后 setResultAndFinish 又写 RESULT_OK；TV/Wear 常把 Back 映射为 DENIED，实际执行 revoke/flags 路径。应用唯一稳定入口仍是 onRequestPermissionsResult 的数组，并在真正使用资源时处理后续 AppOp 或状态变化。

## 8. Java模型会直接改AppOps，PermissionPolicy随后再收敛

runtime permission 与 AppOp 解决不同问题。permission bit 表示持有资格，AppOp 表示某类实际操作当前以何种模式运行。位置的“仅在使用中”典型地是 permission 已 grant，而对应 op 保存为 MODE_FOREGROUND；两者不能合成一个 granted 布尔值。

请求 UI 的 Java AppPermissionGroup 在逐项 persist 时会直接 allowAppOp 或 disallowAppOp，使用户决定尽快反映到操作层。随后 PMS 的 runtime permission listener 又触发 PermissionPolicyService，根据全包、shared UID、foreground/background 和 restricted 状态重新计算。第二次写不是第二次授权，而是把局部 UI 快照收敛到系统级不变量。

顺序仍非原子。一个 group 内是 permission A 的 grant、flags、AppOp，再到 permission B；setOneTime 和 grant 又是两轮。任意观察者都可能在其中读到中间组合。后续同步通常会收敛，但不能把暂态从模型中删除，也不能承诺进程异常后一定已有机会执行补偿。

AppOps 的变化还可能反向影响 REVOKED_COMPAT flag：普通外部 setUidMode 在进入持锁修改前调用 updatePermissionRevokedCompat，而 PermissionPolicy 带 callback 的专用写路径会跳过这一步。所谓“双向同步”不是 AppOps 直接 grant/revoke runtime bit，而是部分兼容 flags 与 policy 重算之间的协作。

### 练习 6：证明runtime通知至少晚两个FgThread阶段

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'notifyRuntimePermissionStateChanged(packageName, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'FgThread.getHandler().sendMessage(PooledLambda.obtainMessage' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'listeners.get(i).onRuntimePermissionStateChanged(packageName, userId);' frameworks/base/services/core/java/com/android/server/pm/permission/PermissionManagerService.java
grep -n -F 'this::synchronizePackagePermissionsAndAppOpsAsyncForUser' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mIsPackageSyncsScheduled.add(new Pair<>(packageName, changedUserId))' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F '::synchronizePackagePermissionsAndAppOpsForUser,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'mAppOpsManagerInternal.setUidModeFromPermissionPolicy(opCode, uid, mode,' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (permissionPolicyCallback == null) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'updatePermissionRevokedCompat(uid, code, mode);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

从 PMS 的 post、listener 执行、PPS 再 post 到 syncPackages 画消息队列。Binder 返回点放在第一条 post 之后，便不会误以为应用回调等待了 AppOps 收敛。

## 9. PermissionPolicy由三类事件驱动，并按package/user去重

PermissionPolicyService 启动后观察 package add/change/remove、runtime permission state change 和相关 AppOps mode change。包变化可同步包并清理 UID 下已无人请求的 AppOp permission；permission 或 AppOps 变化则进入异步去重。

去重键是 packageName 与 userId 的 Pair。第一次事件把任务投到 FgThread，后续相同键只合并；真正执行时先从 scheduled 集合移除，再读取当前状态。因此它是最终状态型收敛，而不是逐事件日志回放。不同用户不能共享同一键，shared UID 的其他包则在执行阶段扩展。

PMS 的 runtime listener 本身已经由 FgThread 异步通知；PPS listener 再投一次同一 FgThread。没有 future、join 或回调把这两跳连接到 grant/revoke 调用者。队列中还可能夹入 package、AppOps 和其他前台线程任务，源码只保证最终有机会按新快照重算。

## 10. shared UID先扩展已安装成员，再按最宽候选写raw UID mode

synchronizePackagePermissionsAndAppOpsForUser 先取得目标 PackageInfo，再加入目标用户中已安装的 shared UID 成员，最后统一 syncPackages。权限状态本身也由 PackageSetting 委托给 SharedUserSetting；因此 Binder 参数中的 packageName 不是 shared UID 内一份隔离账本。

每个包/permission 会产生候选：

- grant 且不带 REVOKED_COMPAT，并通过 restricted policy：通常进入 ALLOWED；
- 有 background permission 的 foreground 权限：background 也允许时进入 ALLOWED，否则进入 FOREGROUND；
- 不满足 grant 条件：进入 IGNORED；
- 某些 soft-restricted/legacy storage 进入 IGNORE_IF_NOT_ALLOWED。

syncPackages 固定按 ALLOWED、FOREGROUND、IGNORED、IGNORE_IF_NOT_ALLOWED 的顺序遍历。ALLOWED 循环会处理全部候选并标记 uid+switch-op；从 FOREGROUND 开始，后续候选才会因同一键已被标记而跳过。因此跨桶仍是“最宽 raw 候选优先”，但不能说同桶只写一次。IGNORE_IF_NOT_ALLOWED 也不是第四种更窄 mode：它先读当前 raw mode，若已经 ALLOWED 就保留，只在当前不是 ALLOWED 时考虑写 IGNORED。

REVIEW_REQUIRED 只让那一个 permission 不产生候选；同 switch op 的另一 permission 或 shared 包仍可能提供候选。请求 UI 直接写 AppOp 时只掌握当前包模型，所以 shared UID 可先出现过宽或过窄 raw mode，等 PPS 扩展成员后再收敛。

### 练习 7：手算shared UID的候选桶与第一写入者

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F '? sharedUser.getPermissionsState()' frameworks/base/services/core/java/com/android/server/pm/PackageSetting.java
grep -n -F 'if (ps.getInstalled(userId)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'getSharedUserPackagesForPackage(' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'LongSparseLongArray alreadySetAppOps = new LongSparseLongArray();' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'setUidModeAllowed(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'setUidModeForeground(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'setUidModeIgnored(op.code, op.uid, op.packageName);' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (alreadySetAppOps.indexOfKey(IntPair.of(op.uid, op.code)) >= 0) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'if (currentMode != MODE_ALLOWED) {' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'Note: Called with the package lock held. Do <u>not</u> call into app-op manager.' frameworks/base/services/core/java/com/android/server/policy/PermissionPolicyService.java
grep -n -F 'int evalMode(int op, int mode) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'if (mode == MODE_FOREGROUND) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F '} else if (mode == MODE_ALLOWED) {' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'PROCESS_CAPABILITY_FOREGROUND_CAMERA' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'PROCESS_CAPABILITY_FOREGROUND_MICROPHONE' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'callbackSpecs.remove(mModeWatchers.get(callbackToIgnore.asBinder()));' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHandler.postDelayed(mWriteRunner, WRITE_DELAY);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
grep -n -F 'mHandler.postDelayed(mWriteRunner, 10*1000);' frameworks/base/services/core/java/com/android/server/appop/AppOpsService.java
```

设 A 与 B 同 UID、同 switch op：A 给出 FOREGROUND，B 给出 ALLOWED，再加入一个 REVIEW_REQUIRED 的 C。先按源码收集，再按桶序写，答案应是 ALLOWED；C 的跳过不会冻结整个 op。

## 11. raw mode不是最终访问结果，内存更新也不是appops.xml落盘

PPS 比较和写入的主要是 raw UID mode。真正 check/note/start op 时，AppOpsService 的 UidState.evalMode 还会考虑 pending-top、可见 widget、UID state 与 capability。raw MODE_FOREGROUND 可能变成 ALLOWED 或 IGNORED；camera/microphone 的 raw MODE_ALLOWED 在特定前台能力约束下也存在被求值为 IGNORED 的分支。因而“ALLOWED 大于 FOREGROUND 大于 IGNORED”只描述同步器候选优先级，不是所有时刻的访问真值表。

PPS 写 UID mode 后用 unsafeCheckOpRaw 再查一次。若观察值仍不等于目标，它会把当前 package-specific mode 复位到默认值。源码把常见原因描述为错误设置的 package mode，但 raw 检查还受 suspend/user restriction 等条件影响，不能把所有不相等都归因于 package override。

PermissionPolicy 写 mode 时传入 mAppOpsCallback。AppOpsService 发送 watcher 通知前，用 callbackToIgnore 明确从集合移除该 callback；这才是避免 PPS 对自身写入立即自反馈的直接机制。旧值比较和 package/user 去重仍负责减少额外工作。

持久化另有完成点：UID mode 内存改变后，appops.xml 默认延迟 30 分钟写；复位 package mode 的 fast write 默认延迟 10 秒。崩溃前内存已正确，不等于磁盘文件已包含该结果。

## 12. one-time不是固定寿命，而是flag、UID监听和回调撤销的组合

一次性权限至少包含三部分：非 background permission 的 ONE_TIME flag、system_server 中按 UID 维护的 inactivity listener、PermissionController 在超时回调中执行的实际 revoke。任何一部分单独存在都不是完整会话。

AppPermissionGroup.persistChanges 在组为 one-time 且仍有 runtime grant 时调用 startOneTimePermissionSession；若包已没有任何 one-time 权限则 stop。默认超时由 PermissionController Utils 给出 1 分钟，并可被 DeviceConfig 覆盖。这一分钟是 UID 处于特定“不活跃但仍存活”区间的累计门槛，不是从用户点击起算的固定墙上时间。

system_server 的计时模块不直接撤销 permission。它判定 session 结束后调用 PermissionControllerManager.notifyOneTimePermissionSessionTimeout；PermissionControllerServiceImpl 重新读取包和请求权限，收集仍标记 one-time 的 group，撤销其中仍授予的 runtime permission，清 USER_SET，再 persistChanges 并记录原因。

如果回调时包已经不存在，r48 实现把 NameNotFoundException 包成 RuntimeException；listener 此前已经结束，没有内建重试。这个边界说明“发出超时通知”也不等于“撤销已经成功完成”。

## 13. importance状态机混合Handler延时、墙上时钟与RTC alarm

OneTimePermissionUserManager 为每个 Android user 建实例，内部 SparseArray 却只以 UID 为键。下表比较的是 importance 数值；数值越小，进程语义上越重要：

| UID的importance数值 | 行为 |
| --- | --- |
| importance 不高于 reset 阈值 | 清 timerStart，取消或不启动累计 |
| 高于 reset、但仍不高于 keep-alive 阈值 | 记录 System.currentTimeMillis 起点，暂不设 alarm |
| 高于 keep-alive、但 UID 仍存在 | 以 timerStart+timeout 设置精确 RTC_WAKEUP |
| 高于 IMPORTANCE_CACHED，即 UID gone | 主线程 Handler 延迟默认 5 秒，重查仍 gone 才结束 |

活跃累计使用墙上时钟和 RTC_WAKEUP，手工改时钟会改变到期判断；gone 的 5 秒由 Handler 延时，深度睡眠可延长实际等待。两条路径不能合称一个统一单调计时器。

同 UID 已有 listener 时，新 start 不更新 packageName、timeout 或阈值。shared UID 的后一个包会复用第一个包的会话；任一同 UID 包调用 stop，又能按 UID 停掉该 listener。超时回调最终只携带最初保存的 packageName，这与 shared permission state 组合后必须谨慎推演。

r48 还暴露两个并发边界：

- start/stop 先持 mLock，再在构造或 cancel 中进入 mInnerLock；alarm 到期路径持 mInnerLock 后在 onPackageInactiveLocked 进入 mLock，形成相反锁序；
- ACTION_UID_REMOVED receiver 直接访问被标注由 mLock 保护的 mListeners，没有取得外锁。

这两点是源码级竞态/死锁风险，不应包装成设计保证。cancel 没有移除 token callback，而 delayed gone callback 又未按方法注解持 mInnerLock；mIsFinished 只能表达实现意图，不能当作并发 no-op 的完成保证。

### 练习 8：同时验证计时、shared UID与锁顺序

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final long DEFAULT_KILLED_DELAY_MILLIS = 5000;' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'private final SparseArray<PackageInactivityListener> mListeners = new SparseArray<>();' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'PackageInactivityListener listener = mListeners.get(uid);' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mTimerStart = System.currentTimeMillis();' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mAlarmManager.setExact(AlarmManager.RTC_WAKEUP, revokeTime, LOG_TAG, this,' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mHandler.postDelayed(() -> {' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'synchronized (mInnerLock) {' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'synchronized (mLock) {' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'mPermissionControllerManager.notifyOneTimePermissionSessionTimeout(' frameworks/base/services/core/java/com/android/server/pm/permission/OneTimePermissionUserManager.java
grep -n -F 'public static final long ONE_TIME_PERMISSIONS_TIMEOUT_MILLIS = 1 * 60 * 1000;' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/Utils.java
grep -n -F 'public void onOneTimePermissionSessionTimeout(@NonNull String packageName) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
grep -n -F 'group.revokeRuntimePermissions(false);' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/PermissionControllerServiceImpl.java
```

分别画存活但后台、UID gone、shared UID 第二包 start、任一 sibling stop、alarm 与 stop 并发五条路径。锁图要标出 outer→inner 与 inner→outer，而不只是列 synchronized 次数。

## 14. 长期不用自动撤销先由周期Job筛出候选包

长期不用自动撤销与 one-time 不是同一个计时器。PermissionController 的 boot receiver 默认每 15 天安排 periodic Job，默认未使用阈值 90 天；两者都可由配置覆盖。Automotive 不调度，profile 自身不调度而由 parent 处理 profile group。JobInfo 只设置 periodic，没有附加充电或 idle 条件。

SKIP_NEXT_RUN 是进程内 static 变量，用来绕过调度后的首次运行；进程死亡会丢失它，所以不是持久的“首轮必跳过”协议。firstBootTime 存在 SharedPreferences，并用 apply 异步写；崩溃也可能使保护时间重新初始化得更晚。

候选时间以 System.currentTimeMillis 为 now，对每个包计算：

1. 同 UID 所有包 UsageStats.lastTimeVisible 的最大值；
2. 与该包 firstInstallTime 取最大；
3. 与 PermissionController 保存的 firstBootTime 取最大；
4. 若包允许跨 profile，再与 profile group 中同包可见时间取最大；
5. 只有 now-lastTimeVisible 严格大于 threshold 才算 unused。

因此 shared UID 任一 sibling 最近可见都会保护该 UID 下所有包不进入候选；墙上时钟跳变会影响分类。没有 UsageStats 的 user 被移出，未解锁 user 被跳过；扫描的数据范围是当前 profile group 的 LiveData，不等于设备上每个独立用户都由这一进程处理。

## 15. 豁免按包判断、变更按group执行，而取消可能管不住子任务

进入 unused 集合后仍有两层包级豁免。永久豁免包括提供特定 bound service、disabled/work profile 和 carrier privileged。用户可覆盖豁免由 OP_AUTO_REVOKE_PERMISSIONS_IF_UNUSED 表达：MODE_DEFAULT 下 targetSdk 不高于 Q 通常默认豁免；MODE_ALLOWED 表示允许自动撤销；其他显式非默认 mode 表示豁免。

每个 group 只有在 foreground/background 都不 fixed、至少一个非 ACTIVITY_RECOGNITION permission 的 isGrantedIncludingAppOp 为真、不是 default/role grant 且 user sensitive 时才触发。ACTIVITY_RECOGNITION 只从“是否触发 group”测试中排除；group 一旦被其他权限触发，revocablePermissions 却取全部 keys，它仍可能进入实际列表。

执行顺序还存在四个完成性陷阱：

- stats 在 importance 检查之前逐 permission 记录，所以最后因 App 正在运行而跳过，日志也可能已经写成自动撤销事件；
- getPackageImportance 只检查一次，检查后到 revoke 之间 App 可以变活跃；
- anyPermsRevoked 在调用 revoke 前置 true，不根据各调用返回值确认真实变化；
- 自动撤销先 revoke background，再 revoke foreground，最后逐项置 AUTO_REVOKED 并清 USER_SET，也不是单事务。

更深的并发问题来自 forEachInParallel：默认 scope 是 GlobalScope，每项用 scope.async(Main) 启动后再 await。AutoRevokeService.onStopJob 取消的是外层 job，这些脱离父 Job 的子任务未必随之取消；它们可在 jobFinished、失败处理或重调度后继续修改权限，甚至与下一轮重叠。

shared UID 又把“使用时间”和“撤销资格”拆成不同粒度：last visible 先按 UID 取并集，但永久/用户豁免、group eligibility 和 mutation 按 package 单独执行；底层 PermissionsState 却可能属于 shared user。A 包允许自动撤销、B 包因旧 target 或其他原因豁免时，处理 A 仍可能改变 B 共同依赖的 shared 权限状态。源码没有在撤销前把 sibling 的豁免做 UID 级并集。

### 练习 9：构造长期不用扫描的三个反例

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private var SKIP_NEXT_RUN = false' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'private val DEFAULT_UNUSED_THRESHOLD_MS =' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'private val DEFAULT_CHECK_FREQUENCY_MS = DAYS.toMillis(15)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F '.setPeriodic(getCheckFrequencyMs(context))' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'pkgs.groupBy { pkg -> pkg.uid }' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'now - lastTimeVisible > getUnusedThresholdMs(context)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'val revocablePermissions = group.permissions.keys.toList()' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'PermissionControllerStatsLog.write(' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'if (packageImportance > IMPORTANCE_TOP_SLEEPING) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'if (isPackageAutoRevokePermanentlyExempt(pkg, user)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'if (isPackageAutoRevokeExempt(context, pkg)) {' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'anyPermsRevoked.compareAndSet(false, true)' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'FLAG_PERMISSION_AUTO_REVOKED to true,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'job?.cancel()' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/service/AutoRevokePermissions.kt
grep -n -F 'scope: CoroutineScope = GlobalScope,' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
grep -n -F 'map { scope.async(context) { transform(it) } }.map { it.await() }' packages/apps/PermissionController/src/com/android/permissioncontroller/permission/utils/KotlinUtils.kt
```

反例一：日志已写但 importance 为 TOP；反例二：Job 被 stop 后 GlobalScope child 继续；反例三：同 UID 的 A 可撤、B 豁免。每例分别标“候选、实际 grant、flags、通知、jobFinished”，不要用一个 revoked 布尔值代替。

## 16. 用完成点矩阵判断这条链真正走到了哪里

把整章压成一张完成点矩阵：

| 观察事件 | 已能确认 | 仍不能确认 |
| --- | --- | --- |
| 用户点击按钮 | UI 已选择分支 | 整组原子成功 |
| PMS Binder 返回 | 同步校验已执行；内存变化可能完成，也可能因校验提前返回 | AppOps 收敛、kill、XML durable |
| onRequestPermissionsResult | 原始数组的当前 permission-check 视图 | ONE_TIME/flags、effective AppOp、长期稳定性 |
| PPS syncPackages 结束 | 当前快照对应的 raw mode 已尝试收敛 | 每次实际访问必放行、appops.xml 已写 |
| one-time alarm 到期，或 UID gone 后 5 秒复查仍 gone | system_server 已决定会话结束 | Controller 已成功撤销 |
| auto-revoke jobFinished | 外层协程走到结束回调 | GlobalScope 子任务必已停止 |
| 收到自动撤销通知 | revokedApps 列表非空 | 每个底层 revoke 都真实改变过 grant |

排查“用户允许了却仍不能访问”时，依次检查 permission bit、flags、raw AppOp、UidState.evalMode 的有效结果以及 shared UID sibling；排查“为什么后来被撤销”时，再区分 one-time flag/listener 与 auto-revoke job。排查“重启后为什么不同”时，最后核对 runtime-permissions.xml 和 appops.xml 的延迟写入，而不是只看 UI 回调。

最重要的结论有四个。第一，调用包身份来自 ATMS resultTo，而执行特权属于 PermissionController。第二，一次按钮选择在 Java 模型中会拆成多次 Binder/AppOps/session 操作，不具备事务原子性。第三，PMS 返回、应用回调、PPS raw mode 收敛和磁盘持久化是不同完成点。第四，one-time 与长期不用自动撤销都带有 UID/package 粒度错位、时钟和取消边界，shared UID 下尤其不能按单包直觉推断。
