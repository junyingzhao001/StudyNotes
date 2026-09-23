# 273 Android 11 scoped storage：MediaProvider/FUSE、应用隔离目录、MediaStore授权与MANAGE_EXTERNAL_STORAGE主链

## 1. 先看结论：这不是一堵权限墙，而是五个边界依次收敛

本文固定在 Android 11 `android-11.0.0_r48`：`frameworks/base`提交为 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/providers/MediaProvider`提交为 `47c141d93e93b25cc85c36f3579fda25a1695952`。这一版 scoped storage不能用“FUSE接管 `/sdcard`，MediaProvider查一下权限”概括，因为同一次访问至少跨过五类边界：

| 边界 | 权威状态 | 典型完成点 |
|---|---|---|
| 卷与挂载 | vold、StorageManagerService | vold认为卷已挂载 |
| 会话与进程 | StorageSessionController、ExternalStorageService | RemoteCallback返回或超时 |
| 进程视图 | Zygote/vold mount namespace | app看到upper，MediaProvider看到pass-through lower |
| 直接路径 | native libfuse、JNI、MediaProvider helper | 某个FUSE请求回复errno或成功 |
| 数据与授权 | MediaStore DB、URI grant、PermissionActivity | 行变更、文件变更、grant和Activity结果各自完成 |

正常主链是：vold挂整卷并把 `/dev/fuse` FD交给 system_server；system_server按用户绑定 MediaProvider包中的 `ExternalStorageServiceImpl`；该服务在MediaProvider进程内启动Java线程和native libfuse循环；内核请求再携带调用者UID/PID，经JNI进入MediaProvider的路径、行级权限与脱敏逻辑。

这条链最重要的阅读规则是：路径“看得见”、文件“能打开”、数据库“有行”、URI“有grant”、扫描“已完成”不是同一个事实。后文会用反例把这些状态拆开，尤其说明 `MANAGE_EXTERNAL_STORAGE`为什么不是其他应用 `Android/data`、`Android/obb` 的万能钥匙。

## 2. FUSE开关是启动快照；服务身份从system-only MediaStore反查

根 `init.rc`给 `persist.sys.fuse`写入true，并据其在zygote启动时选择把 `/mnt/user/0`还是旧 `/mnt/runtime/default`绑定到根命名空间的 `/storage`。`StorageManagerService`构造时读取一次属性到final的 `mIsFuseEnabled`；Settings侧标志与该快照不一致时，服务写回属性并执行硬重启，而不是在线替换已有卷会话。

真正的服务发现直到user 0解锁才发生。`StorageSessionController`先用 `MATCH_SYSTEM_ONLY`解析 `MediaStore.AUTHORITY`，取provider的包名和appId；再把 `ExternalStorageService.SERVICE_INTERFACE`限制到同一个包内解析，并要求service声明 `BIND_EXTERNAL_STORAGE_SERVICE`。这不是从所有可响应service中任选一个，也不是普通应用可替换的扩展点。

controller的 `shouldHandle()`还同时要求：FUSE已开启、当前不在reset、目标卷是emulated或可见public。private、stub和不可见public卷不会建立这里讨论的MediaProvider FUSE session。

### 练习 1：把启动快照、重启和服务发现分开

分别标出属性写入、构造快照、Settings比较和user 0解锁四个时间点；再验证provider包名如何约束service候选。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'setprop persist.sys.fuse true' system/core/rootdir/init.rc
grep -n -F 'mIsFuseEnabled = SystemProperties.getBoolean(PROP_FUSE, DEFAULT_FUSE_ENABLED);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (mIsFuseEnabled != settingsFuseFlag) {' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mContext.getSystemService(PowerManager.class).reboot("fuse_prop");' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'if (shouldHandle(null) && userId == 0) {' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'PackageManager.MATCH_SYSTEM_ONLY' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'intent.setPackage(mExternalStorageServicePackageName);' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'Manifest.permission.BIND_EXTERNAL_STORAGE_SERVICE' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'return mIsFuseEnabled && !mIsResetting' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
```

## 3. vold先搭整卷骨架，system_server再按用户连接、按卷建session

`StorageManagerService.mount()`调用 `mVold.mount()`并提供 `IVoldMountCallback`。vold的卷模型各自决定是否建立user FUSE：emulated路径要求FUSE与visible，public路径在这一处只检查FUSE；随后system_server的controller只处理所有emulated和visible public。满足各自条件时，vold打开 `/dev/fuse`、建立upper与pass-through view，再通过 `onVolumeChecking(fd, path, internalPath)`把FD与两条路径交回。只有callback返回ready，vold才继续完成挂载。

controller以 `vol.getMountUserId()`为键保存一个 `StorageUserConnection`，在连接内又以 `vol.getId()`为键保存多个 `Session`。因此准确模型是“每user一个Binder连接、每volume一个session”，而不是每个进程或每个文件一个daemon。连接使用 `bindServiceAsUser(..., UserHandle.of(mUserId))`，所以各用户绑定的是各自用户空间里的MediaProvider实例。

`startSession()`先把Session放进map，再发远端请求；远端请求携带 `FUSE | INDEXABLE`，并在finally关闭system_server持有的PFD。连接建立与callback共同受一次20秒 `CompletableFuture.get()`约束；system_server超时不会取消远端handler里已经排队或执行的工作，迟到callback仍可能发生。一个隐蔽结果是：start失败不会在该方法内撤回刚放入的Session；远端启动或绑定失败被包装成 `ExternalStorageServiceException`并回到mount callback时，StorageManagerService会安排10秒后的全局reset。这个10秒是延迟恢复，不是mount自身的总超时，其他异常也不必然走这条catch。

### 练习 2：画出FD、连接和session的所有者变化

从vold打开FD开始，记录它何时进入callback、何时交给远端、何时在Java侧 `detachFd()`；同时标出失败后Session仍留在哪张表里。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mVold.mount(vol.id, vol.mountFlags, vol.mountUserId' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'callback->onVolumeChecking(std::move(fd), getPath(), getInternalPath(), &is_ready);' system/vold/model/EmulatedVolume.cpp
grep -n -F 'callback->onVolumeChecking(std::move(fd), getPath(), getInternalPath(), &is_ready);' system/vold/model/PublicVolume.cpp
grep -n -F 'res = MountUserFuse(user_id, getInternalPath(), label, &fd);' system/vold/model/EmulatedVolume.cpp
grep -n -F 'if (isFuse) {' system/vold/model/PublicVolume.cpp
grep -n -F 'fuse_fd->reset(open("/dev/fuse", O_RDWR | O_CLOEXEC));' system/vold/Utils.cpp
grep -n -F 'connection.startSession(sessionId, deviceFd' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'mConnections.put(userId, connection);' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'mSessions.put(sessionId, session);' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'FLAG_SESSION_TYPE_FUSE | FLAG_SESSION_ATTRIBUTE_INDEXABLE' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'DEFAULT_REMOTE_TIMEOUT_SECONDS = 20' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F ').get(DEFAULT_REMOTE_TIMEOUT_SECONDS, TimeUnit.SECONDS);' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'mContext.bindServiceAsUser' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'mFuseDeviceFd = Objects.requireNonNull(fd).detachFd();' packages/providers/MediaProvider/src/com/android/providers/media/fuse/FuseDaemon.java
grep -n -F 'FAILED_MOUNT_RESET_TIMEOUT_SECONDS = 10' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
```

## 4. FuseDaemon是MediaProvider进程内线程；ready、退出和reset各有不同屏障

framework的 `ExternalStorageService` Binder stub把start、state-change、end都投递到该进程的 `BackgroundThread`，所以这三类生命周期回调在同一handler上串行。`ExternalStorageServiceImpl`取得同进程的本地 `MediaProvider`，为session创建一个 `FuseDaemon extends Thread`；进程局部的static map按sessionId保存daemon，它不是跨用户的系统全局表，更不是vold fork出的独立Linux守护进程。

Java `start()`先启动线程，再以1秒间隔最多轮询5次。线程创建native对象后进入阻塞的 `native_start()`；native libfuse的 `pf_init()`把atomic `active`置true，Java轮询到它才把start视为ready。之后 `fuse_session_loop_mt()`持续处理内核请求。也就是说start callback证明libfuse已经完成init，不证明卷扫描、默认目录维护或任意未来I/O已经完成。

显式end先从daemon map移除；只有找到daemon时才 `join(5000ms)`，daemon已自然移除或从未成功入map则告警后直接返回。daemon自己不能凭end调用拆掉挂载，必须等FUSE connection因卸载、关闭或错误而让native loop返回。全局reset会对每个仍被system_server跟踪的session先 `vold.unmount()`，再尝试远端end；存在daemon时等待，若无法确认退出就kill对应用户的MediaProvider，最后关闭连接并执行vold reset。

5秒start轮询与5秒exit join都发生在对应的20秒远端等待内部，不应相加成独立的外层deadline。更棘手的是，Java `FuseDaemon.start()`轮询超时只抛异常，不会主动停止刚启动的线程；它可能稍后进入ready，但由于 `sFuseDaemons.put()`位于start返回之后，这个线程不会被map跟踪。

r48还有一个必须按实码保留的例外：正常 `onVolumeUnmount()`先调用 `onVolumeRemove()`，后者已经从连接map移除Session；随后 `removeSessionAndWait()`第二次remove得到null并直接返回。因此普通显式unmount虽先由vold拆挂载、native loop通常会自然退出，却不会发送远端 `endSession()`，也不会执行5秒join。注释里的“等待退出”不能覆盖这个双重remove行为。

user stop也不是完整的连接销毁：`onUserStopping()`只清该连接的Session map，没有从controller移除连接，也不主动close/unbind；后续 `onCleanupUser()`才通知vold/storaged停止用户，而vold这里只销毁、卸载该用户的emulated volumes，不能据此断言public volume等其他session也同时被拆除。daemon自然退出会清MediaProvider进程里的daemon map，却不会反向删除system_server仍跟踪的Session。两侧map必须分别观察。

另一条结束路径来自service death：连接断开或binding死亡时，`ActiveConnection`会close连接、取消connection future与outstanding operations，再调用 `resetUserSessions()`；只要system_server仍跟踪session，r48最后投递的是全局 `H_RESET`，而非只修复当前user。

### 练习 3：验证三种结束路径而不是只看方法名

分别模拟自然退出、普通unmount和global reset，记录谁移除两侧map、谁真正调用end、谁会kill进程。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mHandler.post(() -> {' frameworks/base/core/java/android/service/storage/ExternalStorageService.java
grep -n -F 'private static final Map<String, FuseDaemon> sFuseDaemons' packages/providers/MediaProvider/src/com/android/providers/media/fuse/ExternalStorageServiceImpl.java
grep -n -F 'private static final int POLL_COUNT = 5;' packages/providers/MediaProvider/src/com/android/providers/media/fuse/FuseDaemon.java
grep -n -F 'if (mPtr != 0 && native_is_started(mPtr)) {' packages/providers/MediaProvider/src/com/android/providers/media/fuse/FuseDaemon.java
grep -n -F 'fuse->active->store(true, std::memory_order_release);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'fuse_session_loop_mt(se, &config);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'StorageUserConnection connection = onVolumeRemove(vol);' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'connection.removeSession(sessionId);' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'connection.removeSessionAndWait(sessionId);' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'Session session = removeSession(sessionId);' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'if (session == null) {' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'mVold.unmount(vol.id);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mService.onExitSession(getName());' packages/providers/MediaProvider/src/com/android/providers/media/fuse/FuseDaemon.java
grep -n -F 'destroyEmulatedVolumesForUser(userId);' system/vold/VolumeManager.cpp
grep -n -F 'op.cancel(true);' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'resetUserSessions();' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'but for now, reset everything.' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'killExternalStorageService(connections.keyAt(i));' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'join(waitMs);' packages/providers/MediaProvider/src/com/android/providers/media/fuse/FuseDaemon.java
grep -n -F 'if (!mSessions.containsKey(sessionId)) {' frameworks/base/services/core/java/com/android/server/storage/StorageUserConnection.java
grep -n -F 'No available storage user connection' frameworks/base/services/core/java/com/android/server/storage/StorageSessionController.java
grep -n -F 'mStorageSessionController.notifyVolumeStateChanged(vol);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mCallbacks.notifyVolumeStateChanged(vol, oldState, newState);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'ForegroundThread.getExecutor().execute(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 5. volume-state callback挡在广播前，却不是“一切扫描完成”的承诺

对 `shouldHandle(vol)`为true、对应user connection存在且session仍被跟踪的卷，StorageManagerService会在安排监听者通知和广播之前同步等待 `notifyVolumeStateChanged()`的RemoteCallback；否则controller直接返回或只记录日志，后续通知仍继续。20秒仍未完成或远端回传异常时同样只记录错误，代码随后仍继续通知。因此这个顺序提供的是一次有上限的前置尝试，不是“失败就永不广播”。

MediaProvider实现对mounted调用 `attachVolume()`，对unmounted、ejecting、removed和bad-removal调用 `detachVolume()`，最后 `updateVolumes()`。`attachVolume()`先把卷名加入集合并发通知，然后把 `ensureDefaultFolders()`与 `ensureThumbnailsValid()`投到 `ForegroundThread`。它没有在本次callback内等待这项异步维护；实现里也没有在此处调用导入的 `REASON_MOUNTED`进行整卷扫描。

所以成功的远端callback最多能证明同步的attach/detach/updateVolumes调用已经返回。它不能证明默认目录维护完成、缩略图检查完成、磁盘上每个文件已有DB行，也不能保证接收广播的应用观察不到后续收敛。调试“收到MOUNTED但首查不完整”时，应同时记录remote callback、attached-volume集合、异步维护和scanner四个时间点。

## 6. 新FUSE与AppFuse只共享底层技术，控制面完全不同

MediaProvider scoped-storage FUSE挂的是整卷：生命周期键是user与volume，策略依据是请求UID、路径、数据库行、AppOp与redaction，处理者是MediaProvider进程里的libfuse线程池。`AppFuseBridge`服务的是 `StorageManager.openProxyFileDescriptor()`：生命周期键是calling UID、mountId与fileId，挂载点形如 `/mnt/appfuse/<uid>_<mountId>`，I/O最终回调应用提供的 `ProxyFileDescriptorCallback`。

两者都会让vold打开 `/dev/fuse`，但这不构成同一条Java桥。AppFuse不受 `persist.sys.fuse`开关控制，mount选项包含 `default_permissions`；MediaProvider整卷FUSE则刻意省略该选项，让请求先到daemon再做策略判断。AppFuse在system_server维护 `MountScope`，`waitForMount()`直接 `CountDownLatch.await()`而没有这里的20秒remote callback合同；它也不读取MediaStore owner、pending或 `ACCESS_MEDIA_LOCATION`。反过来，`StorageSessionController`不创建AppFuse mountId，也不参与单个proxy FD的生命周期。

### 练习 4：用键、挂载点和等待方式区分两套FUSE

不要凭类名判断。为两条链分别写出“谁持有mount表、谁响应read、等待什么事件、失败由谁回收”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public class AppFuseBridge implements Runnable' frameworks/base/services/core/java/com/android/server/storage/AppFuseBridge.java
grep -n -F 'APPFUSE_MOUNT_NAME_TEMPLATE' frameworks/base/services/core/java/com/android/server/storage/AppFuseBridge.java
grep -n -F 'private final SparseArray<MountScope> mScopes' frameworks/base/services/core/java/com/android/server/storage/AppFuseBridge.java
grep -n -F 'mMounted.await();' frameworks/base/services/core/java/com/android/server/storage/AppFuseBridge.java
grep -n -F 'final AppFuseMount mount = mStorageManager.mountProxyFileDescriptorBridge();' frameworks/base/core/java/android/os/storage/StorageManager.java
grep -n -F 'device_fd->reset(open("/dev/fuse", O_RDWR));' system/vold/AppFuseUtil.cpp
grep -n -F '"default_permissions,"' system/vold/AppFuseUtil.cpp
grep -n -F 'permission checks before routing to FUSE daemon' system/vold/Utils.cpp
grep -n -F 'SetupMessageSockets(&proxyFd)' frameworks/base/services/core/jni/com_android_server_storage_AppFuseBridge.cpp
grep -n -F 'FLAG_SESSION_TYPE_FUSE = 1 << 0' frameworks/base/core/java/android/service/storage/ExternalStorageService.java
grep -n -F 'class FuseDaemon final' packages/providers/MediaProvider/jni/FuseDaemon.h
```

## 7. Zygote决定谁看upper、谁看lower；data/obb隔离还多一层namespace

对mount mode非NONE且不属于pass-through、installer等特殊模式的普通应用，Zygote在私有mount namespace里把 `/mnt/user/<userId>`绑定到 `/storage`，因此直接路径进入MediaProvider FUSE upper视图；NONE模式创建所需namespace后直接返回，不提供该storage bind。StorageManagerService若识别UID承载 `ExternalStorageService`，则返回 `MOUNT_EXTERNAL_PASS_THROUGH`；Zygote改把 `/mnt/pass_through/<userId>`绑定到该进程的 `/storage`。于是 `ExternalStorageServiceImpl`虽收到名义上的upper path，在MediaProvider自己的namespace中，同一 `/storage/...`名称实际指向绕过MediaProvider FUSE的pass-through backing view；启用sdcardfs时该view来自 `/mnt/runtime/full`，否则才直接来自absolute lower path。r48实现因此明确忽略传入的lower path。

应用专属目录隔离是另一开关。`mVoldAppDataIsolationEnabled`要求FUSE开启且 `persist.sys.vold_app_data_isolation_enabled`为true；源码读取这个persistent property时默认false，产品、vendor或外部属性配置可把它设为true。启用后，普通app启动时Zygote在它的namespace把 `Android/data`与 `Android/obb`盖成tmpfs，再只把该UID可见的包目录从绕过FUSE的backing view bind进来。shared UID会把同UID包集合一起准备，而特殊的installer、pass-through、android-writable模式跳过此步骤。

若进程启动时目录尚未准备好，ProcessList把 `bindMountPending`置true；对应emulated volume随后进入 `STATE_MOUNTED`时，StorageManagerService收集该user的pending进程，并让vold进入既有进程namespace补做bind mount。由此可见，data/obb隐身同时依赖namespace布局与FUSE策略；只看MediaProvider一个Java if不足以说明设备上的最终路径视图。

### 练习 5：在同一路径名下比较三个进程视图

选择普通app、MediaProvider和DownloadProvider，分别追 `mountMode`、`/storage`来源以及是否需要data/obb tmpfs覆盖。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return Zygote.MOUNT_EXTERNAL_PASS_THROUGH;' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'return Zygote.MOUNT_EXTERNAL_ANDROID_WRITABLE;' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'BindMount(pass_through_source, "/storage", fail_fn);' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'BindMount(user_source, "/storage", fail_fn);' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'if (mount_mode == MOUNT_EXTERNAL_NONE) {' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'We only use the upperFileSystemPath' packages/providers/MediaProvider/src/com/android/providers/media/fuse/ExternalStorageServiceImpl.java
grep -n -F 'StringPrintf("/mnt/runtime/full/%s", relative_upper_path.c_str())' system/vold/Utils.cpp
grep -n -F 'Bind mounting " << absolute_lower_path' system/vold/Utils.cpp
grep -n -F 'mVoldAppDataIsolationEnabled = mIsFuseEnabled' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'MountAppDataTmpFs(androidObbDir, fail_fn);' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'BindMountStorageToLowerFs(user_id, uid, "Android/data"' frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
grep -n -F 'app.bindMountPending = true;' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
grep -n -F 'mFuseMountedUser.add(userId);' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'getProcessesWithPendingBindMounts' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
grep -n -F 'mVold.remountAppStorageDirs' frameworks/base/services/core/java/com/android/server/StorageManagerService.java
```

## 8. FUSE只有UID，shared UID的“主包”和“成员集合”承担不同职责

Binder调用可从provider获得调用包名，`LocalCallingIdentity.getPackageNameInternal()`再用 `AppOpsManager.checkPackage(uid, packageName)`验证归属。FUSE请求只有内核提供的UID/PID；`fromExternal(uid)`查询 `getPackagesForUid()`，把数组第一个元素选为 `packageNameUnchecked`，同时保存全部成员名。

这造成两个不同的主体：首包名负责package级AppOp、targetSdk以及FUSE新建行的 `OWNER_PACKAGE_NAME`；全部shared包名负责匹配 `Android/{data,media,obb,sandbox}/<package>`路径归属和数据库 `OWNER_PACKAGE_NAME IN (...)`。只有 `isCallingIdentitySharedPackageName()`这类路径/shared-member比较明确忽略大小写；SQL `IN`与最终的 `Objects.equals()` owner检查不能一概而论。若A/B共享UID且PM返回A在首位，B触发FUSE create时新行owner仍是A；A/B一般都能通过shared-owner过滤，但某些最终只比较首包名的owner检查仍可能拒绝owner=B的行。

FUSE identity按UID缓存，没有TTL。`fromExternal(uid)`立即取得首包候选与全部shared成员；首包归属验证、targetSdk和permission/AppOp位才惰性解析，owned-row与deleted-path映射则是运行中可变状态。MediaProvider监听storage、location、legacy、manager、gallery和测试后门等mode变化，并在包增删或owner行变化时尝试失效。这里的缓存是性能优化，不是授权快照的持久真相；特别是包卸载后 `getPackageUid()`失败会被吞掉，不能断言remove广播总能清掉旧UID缓存。

### 练习 6：制造一个shared UID首包反例

让A/B具有不同package AppOp，PM依次返回A、B；分别预测manager判断、新建row owner、shared-owner查询和非FUSE pending owner检查。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'sharedPackageNames[0]' packages/providers/MediaProvider/src/com/android/providers/media/LocalCallingIdentity.java
grep -n -F 'checkPackage(uid, packageNameUnchecked);' packages/providers/MediaProvider/src/com/android/providers/media/LocalCallingIdentity.java
grep -n -F 'getPackagesForUid(uid);' packages/providers/MediaProvider/src/com/android/providers/media/LocalCallingIdentity.java
grep -n -F 'private final SparseArray<LocalCallingIdentity> mCachedCallingIdentityForFuse' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'values.put(FileColumns.OWNER_PACKAGE_NAME, getCallingPackageOrSelf());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'for (String sharedPkgName : mCallingIdentity.get().getSharedPackageNames()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'startWatchingMode(AppOpsManager.OPSTR_MANAGE_EXTERNAL_STORAGE' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'invalidateLocalCallingIdentityCache(packageName' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mCachedCallingIdentityForFuse.remove(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'getPackageUid(packageName, 0));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 9. bypass有严格顺序：MANAGE也先撞上其他包private-path拒绝

native `is_app_accessible_path()`先允许非app UID，再按owned-path正则校验包目录；对普通app，包名不属于该UID便用类似ENOENT的方式隐藏。进入Java helper后，`isPrivatePackagePathNotOwnedByCaller()`仍在 `shouldBypassFuseRestrictions()`之前执行。它把 `Android/data`、`Android/obb`与 `Android/sandbox`的其他包目录视为private，却明确排除 `Android/media`，因为后者是会被扫描、共享的媒体区域。

因此 `MANAGE_EXTERNAL_STORAGE`扩大普通共享区与MediaStore行级能力，但不能跨过other-app data/obb/sandbox的前置拒绝。精确的 `Android/data`、`Android/obb`根目录在opendir/readdir链也先被拒绝；installer或 `MOUNT_EXTERNAL_ANDROID_WRITABLE`之所以能处理这些目录，是因为它们可能直接看到lowerfs bind mount，而不是manager穿透了同一个FUSE判断。

通过private-path门后，`shouldBypassFuseRestrictions(forWrite, path)`依次接受：具备相应READ/WRITE storage权限的legacy身份、manager身份、shared UID自己的包目录，以及具备图片/视频写AppOp且文件类型匹配的system gallery能力。普通 `READ_EXTERNAL_STORAGE`并不直接走这条捷径，而会进入数据库可见行检查。一个被判为legacy却缺相应storage权限的调用，也不会自动退回现代owner模型，而会在后续分支显式拒绝。

`checkPermissionManager()`还兼容隐藏的 `OPSTR_NO_ISOLATED_STORAGE`，这是测试/开发后门，不应被当作公开的第二个“all files”权限。

root、shell和其他系统UID也不能合并成一个bypass。native wrapper让root与shell绕过open、目录枚举和redaction等检查，但create、unlink、rename只让root直接碰lowerfs；shell仍回调Java，以便维护数据库。其他低于 `AID_APP_START` 的UID只是在native package-path前置门放行，后续Java helper是否放行仍由各自身份决定。

## 10. lookup、stat、readdir和open回答的是四个不同问题

在共享路径上，native lookup主要检查父目录的package归属与用户号；getattr也只套package-path门，不查询MediaStore行级权限。`pf_access()`对 `F_OK`明确直接查lowerfs，其他mode才走open准入。因此“已知路径的 `exists()`或stat有结果”不等于可读文件内容，这是一个可观察的元数据侧信道边界。

open才调用 `isOpenAllowedForFuse()`：other-private先返回ENOENT；bypass直接成功；无足够权限的legacy返回EACCES；现代调用者必须查到可见DB行，并通过owner、对应媒体能力、global能力或exact URI grant的行级检查。这里有两级门：FUSE先以collection URI加 `_data=?`查询可见行，exact item grant不会扩大这次collection query；只有行已由其他条件可见，拼出的item URI才会进入 `checkAccess()`并命中exact grant。由Provider生成路径的pending文件还会额外要求owner；FUSE-created pending没有隐藏pending文件名模式，因而走特例。

目录枚举又有第三种语义。opendir先做目录准入，第一次readdir才向Java取列表，并把结果缓存到当前dirhandle。Java可返回 `['']`表达拒绝、`['/']`要求native读取完整lowerfs，或返回DB可见的文件名；native无论文件列表是否为空，都会再从lowerfs加入目录名。所以scoped storage可以隐藏文件名，却不保证隐藏共享树中的目录名；权限或DB在handle打开后改变，也不会刷新这个handle已经缓存的列表。

### 练习 7：用同一个victim路径分别调用四类操作

让现代app没有读权限但知道完整路径，逐个预测lookup、`access(F_OK)`、open和父目录readdir；答案必须区分属性、内容与名称枚举。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static bool is_app_accessible_path' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'We should always allow lookups on the root' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'exists() checks are always allowed.' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'static void pf_open' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'public int isOpenAllowedForFuse' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean callerHasUriPermission' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (h->next_off == 0) {' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'add directory names from lower file system.' packages/providers/MediaProvider/jni/MediaProviderWrapper.cpp
grep -n -F 'return new String[] {"/"};' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return new String[] {""};' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'const std::regex PATTERN_OWNED_PATH(' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'if (shouldBypassDatabaseForFuse(uid)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'static void pf_mknod' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'deleteIfAllowed(uri, extras, data);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'file.delete();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'daemon.shouldOpenWithFuse(filePath, true /* forRead */' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'bool direct_io = ri->isRedactionNeeded() || is_file_locked(fd, path);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'e->entry_timeout = get_timeout(fuse, path, should_inval);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'e->attr_timeout = is_package_owned_path(path, fuse->path) || should_inval' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'if (!name.empty()) {' packages/providers/MediaProvider/jni/FuseDaemon.cpp
```

## 11. create、delete和rename各自选择不同的DB/文件提交顺序

标准 `open(O_CREAT)`进入 `pf_create()`：native先查父路径，Java `insertFileIfNecessaryForFuse()`再校验private目录、文件名、bypass与placement。对没有database-bypass、且由DB管理的普通共享路径，现代scoped调用通常先插入 `IS_PENDING=1`且owner为FUSE首包的数据库行，随后native才 `open()` lowerfs；manager与legacy system gallery可直接跳过DB，app-private等restriction-bypass分支则只在缺row时尝试插入且吞掉异常。lowerfs创建失败时会调用delete尝试回滚已建行。创建成功后的 `onFileCreatedForFuse()`只异步更新quota type，不等于媒体扫描完成。另一个反例是 `pf_mknod()`直接落lowerfs且不建DB行；它是否可从应用到达仍受syscall、seccomp、SELinux和mount约束，不能据这一处代码宣布可利用。

文件unlink通过 `deleteFileForFuse()`：普通路径进入MediaProvider delete；对匹配row逐个先调用 `deleteIfAllowed()`尝试删除lower文件，其中异常会被吞掉，`deleteAndInvalidate()`也不检查 `File.delete()`的boolean，随后仍删除DB行。因此数据库事务不能把两种介质动作变成原子提交，delete count也不是物理文件已消失的证明。manager与legacy system gallery命中database-bypass后直接尝试删lowerfs；其他restriction-bypass身份仅在Provider没删到row时再直接删lowerfs，root则在native wrapper中直接unlink。目录rmdir不同，它只问目录策略后调用lowerfs，不为目录逐行维护数据库。

rename最复杂。普通scoped路径先禁止其他包private目录、非法名字、storage根目标和大多数 `Android/*`目标；文件要求源row可写且MIME适合目标collection，目录则检查树内已索引文件。checked流程把DB更新与lowerfs rename编排在事务中；manager与legacy system gallery命中database-bypass后直接操作lowerfs，只有未命中database-bypass、但旧新路径都命中restriction-bypass的分支才尝试同步DB，root也可直接走native rename。`Android/media`不是private，普通路径允许以它作为目标区域，却没有在所有分支验证目标package等于调用包；目录创建/删除策略也没有按media下的package名重新校验owner。因此该目录名不是owner证明，最终结果还会受到父目录、非空目录、SELinux与mount条件约束。

可执行反查时，先从 `pf_create()`、`pf_mknod()`、`deleteFileForFuse()`与rename的database-bypass分支分别落笔，避免拿一种提交顺序解释四种操作。

## 12. VFS、lower FD和数据库缓存靠失效与锁协调，而非天然强一致

FUSE启用writeback cache后，upper FUSE fd与MediaProvider直接打开的lower fd可能看到不同page cache。`shouldOpenWithFuse()`的native接口能按参数尝试读锁或写锁，但r48 MediaProvider主调用即便以write mode打开也固定传 `true /* forRead */`，实际尝试的是共享读锁；附近关于升级成WR_LOCK的注释与这个实参不一致，不能把注释当运行事实。如果无法可靠加锁，调用会选择从FUSE upper打开。redaction存在或文件已被lower fd锁住时，native handle启用 `direct_io`，避免缓存让无位置权限的调用者读到前一调用者留下的原始字节，或让上下层读写互相看不到。

相关Provider路径修改lowerfs后会尝试调用 `invalidateFuseDentryCache()`；FUSE线程本身跳过该动作，因为正在处理的lower变更已经在本次FUSE请求里反映，而且在FUSE线程上发invalidation可能崩溃。失效还要求daemon active、node tracker能找到目标，且路径不是受保护的精确mountpoint `/Android`、`/Android/data`、`/Android/obb`；这些匹配不含尾随斜杠，并非每次尝试都真正让内核缓存失效。普通路径entry/attr timeout近似无限；package-owned data/obb/sandbox两者为0；`Android/media`只把entry timeout设为0，attr不必然为0。大小写冲突lookup的某些失效另在线程中异步执行。entry/attr timeout、失效、单个dirhandle的readdir缓存与MediaStore事务因此构成多种短暂窗口，不能承诺任意两个API在同一瞬间强一致。

这一节也解释了为什么 `ContentResolver.openFileDescriptor()`有时返回lower fd、有时又回到upper FUSE：选择的目标不是重复做权限检查，而是避免同一文件的缓存与锁状态分裂。

## 13. MediaStore查询用SQL缩小行集；READ不是Files与Downloads的通票

外部数据库的核心记录在 `files`表，images、video、audio、downloads等查询通过视图或受限builder组织。`getQueryBuilder()`把权限变成SQL条件，而不是对每个无权限collection query统一抛异常：无图片/视频集合读能力时通常只返回 `OWNER_PACKAGE_NAME IN (calling UID全部shared包)`的行；音频还保留ringtone、alarm、notification兼容可见性。

`Files` URI不会因为拥有某一种媒体读权限就暴露所有非媒体文件，它只按获准媒体类型和owner收敛；`Downloads`对现代普通调用者也主要按owner限制。legacy read、system/self/shell、manager和exact item URI grant是其他独立路径，但exact grant只在以该item URI操作时生效，不会让它自动出现在collection query中。own row即使没有广域READ也可通过row-level读写门，但实际操作仍受mutable-column、placement、pending open等后续检查；owner为NULL也不等于任何人都拥有它。

普通远端insert不能自行指定 `OWNER_PACKAGE_NAME`，Provider会移除传入值并强制为调用包；self/shell与delegator有单独规则。包数据被清理或卸载后，orphan逻辑可把owner置NULL。这里的owner既影响query/update/delete行集，也影响pending open与redaction，绝不是只供展示的元数据。

### 练习 8：用五行数据验证SQL可见性

准备owner=A、owner=B、owner=NULL、ringtone和non-media download五行，再分别给A无权限、图片读权限、legacy read、exact item grant与manager；对grant场景分别测试item URI和collection URI，写出实际可见集合。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final boolean allowGlobal = checkCallingPermissionGlobal(uri, forWrite);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String matchSharedPackagesClause = FileColumns.OWNER_PACKAGE_NAME + " IN "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, matchSharedPackagesClause);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'OR is_ringtone=1 OR is_alarm=1 OR is_notification=1' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Remote callers have no direct control over owner column' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.remove(FileColumns.OWNER_PACKAGE_NAME);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'values.putNull(FileColumns.OWNER_PACKAGE_NAME);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (mCallingIdentity.get().isOwned(id)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'getContext().checkUriPermission(uri, mCallingIdentity.get().pid' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Files pending from FUSE will not have pending file pattern.' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean allowMovement = extras.getBoolean' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 14. pending、trashed、favorite与RELATIVE_PATH分别控制发布、回收、标记和放置

collection query默认排除pending与trashed，却默认不按favorite过滤；`QUERY_ARG_MATCH_PENDING/TRASHED/FAVORITE`可指定INCLUDE、EXCLUDE、ONLY。强类型单条item URI会把pending/trashed设为INCLUDE，但这只改变状态过滤；行仍须由owner、相应媒体能力、exact item URI grant或global能力之一放行，音频item另有ringtone、alarm、notification兼容可见性。默认MATCH_EXCLUDE中会特别保留owner的FUSE-created pending；普通Provider-created pending仍默认排除。open普通Provider-created pending时，若 `OWNER_PACKAGE_NAME`非NULL，还要求调用包与owner精确匹配；NULL owner不会被该helper拒绝。

trash不是delete。普通外部update在启用movement时设置 `IS_TRASHED`，会计算到期时间并把文件改成隐藏的trashed物理名，untrash再恢复；PermissionActivity明确启用movement。由Provider生成路径的pending通常使用独立隐藏名，而FUSE-created pending不使用该命名。r48默认pending约7天、trash约30天，外部调用者不能把 `DATE_EXPIRES`当任意可写字段。favorite只是一列标记，不默认改变query可见性。

`RELATIVE_PATH`也不是随意标签。insert/update会结合collection、MIME、display name计算 `_data`，必要时创建父目录或真实移动文件。图片、视频、音频、downloads、generic files各有允许的顶层目录；例外包括原目录不变、合法related URI、自己的 `Android/media/<package>`、manager和受控system-gallery。普通scoped app传入 `_data`会被忽略，manager可在insert时提供；update没有同等的 `_data`直改捷径，移动应走 `RELATIVE_PATH`与 `DISPLAY_NAME`，且不能跨volume或跨路径表达的package owner。

## 15. 四种批量请求与单项RecoverableSecurityException不是同一种恢复协议

`createWriteRequest()`、`createTrashRequest()`、`createFavoriteRequest()`、`createDeleteRequest()`只接受MediaStore authority下强类型的image/audio/video单条ID；collection、`Files/<id>`和 `Downloads/<id>`都会在创建PendingIntent时被拒绝。创建阶段只校验URI形状与请求允许的列，不验证row是否存在或调用者当前能否访问，也不会剔除已经可写的item。write批准后仅授予exact URI的临时read+write grant，调用者仍需重试原始写操作；trash、favorite与delete批准后则由PermissionActivity直接applyBatch执行对应变更。

r48的favorite/unfavorite暂时自动批准而不显示确认框。批量update/delete为每项设置 `withExceptionAllowed(true)`，返回结果未逐项检查；外围异常也被catch后继续 `RESULT_OK`。所以 `RESULT_OK`只证明请求走完批准分支（favorite/unfavorite为自动批准）且后台尝试已经结束，不足以证明每个URI都成功。grant又明确不是persistable或prefix，其有效期还受发起Activity生命周期约束。

`RecoverableSecurityException`只出现在更窄的单项写链：目标是强类型image/audio/video item，调用者没有直接写权、却已有直接读权，因而可获得一个 `createWriteRequest()` action；这既覆盖update/delete，也覆盖以write或rw模式打开单项 `openFileDescriptor()`。无读权会得到普通 `SecurityException`；Files、Downloads和collection不提供这条recoverable action；collection update/delete还可能只是被SQL过滤成影响0行。获批后得到grant，原失败写操作依然必须重试。

### 练习 9：区分“grant后重试”与“批准后直接执行”

为四种request和单项RSE各写一条状态机，明确PendingIntent创建、用户决定、grant或batch、Activity结果与原操作重试的先后关系。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'All requested items must be referenced by specific ID' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public static @NonNull PendingIntent createWriteRequest' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'grantUriPermission(getCallingPackage(), uri,' packages/providers/MediaProvider/src/com/android/providers/media/PermissionActivity.java
grep -n -F '.withExceptionAllowed(true)' packages/providers/MediaProvider/src/com/android/providers/media/PermissionActivity.java
grep -n -F 'Favorite-related requests are automatically granted for now' packages/providers/MediaProvider/src/com/android/providers/media/PermissionActivity.java
grep -n -F 'setResult(Activity.RESULT_OK);' packages/providers/MediaProvider/src/com/android/providers/media/PermissionActivity.java
grep -n -F 'We only allow the user to grant access to specific media items' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new RecoverableSecurityException' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'FLAG_GRANT_PERSISTABLE_URI_PERMISSION' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'including those for which you already hold write access.' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F '.withExtra(MediaStore.QUERY_ARG_ALLOW_MOVEMENT, true)' packages/providers/MediaProvider/src/com/android/providers/media/PermissionActivity.java
grep -n -F 'Caller must hold ACCESS_MEDIA_LOCATION permission to access original' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'op.withValue(MediaColumns.XMP, xmp.getRedactedXmp());' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F '// We no longer track location metadata' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public static boolean checkPermissionManager' packages/providers/MediaProvider/src/com/android/providers/media/util/PermissionUtils.java
grep -n -F '<permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F 'return MediaStore.Files.getContentUri(extractVolumeName(path));' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'final boolean hasOwner = (itemOwner != null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 16. 用完成点矩阵收束MANAGE、redaction与整条排障链

Android 11 r48没有 `android.permission.MANAGE_MEDIA`；`MANAGE_MEDIA_PROJECTION`管理的是投屏会话，与本链无关。本文的manager始终指 `checkPermissionManager()`返回true：它检查 `MANAGE_EXTERNAL_STORAGE`对应权限/AppOp，并保留 `OPSTR_NO_ISOLATED_STORAGE`后门兼容。

`ACCESS_MEDIA_LOCATION`不授予媒体行读取权；调用者必须先能打开目标。Content URI路径中，非owner缺少permission或其AppOp时，Provider计算EXIF GPS、ISO location box与相关XMP的字节范围并脱敏；`setRequireOriginal()`只添加要求原始字节的参数，不能升级权限，无法满足时抛 `UnsupportedOperationException`。数据库中的 `LATITUDE/LONGITUDE`在r48已不再可靠维护，scanner写入 `MediaColumns.XMP`的也是redacted XMP；该权限约束的是已有访问权之后的原文件字节，不是“解锁未脱敏数据库列”。

direct-path FUSE仍会调用redaction helper，但在相应legacy-read、manager、own package-specific路径或匹配MIME的system-gallery能力下会提前返回空范围；否则helper按路径重建 `Files/<id>` URI，只有匹配这个URI的write grant才能免脱敏，read grant不够。`createWriteRequest()`只接受并授权强类型image/audio/video item URI，因此不能假定它授予的grant也匹配这里的 `Files/<id>` URI。shared UID还要留意另一个不对称：这里的owner免脱敏用首包名做精确 `Objects.equals()`，不是shared-owner集合。`MANAGE_EXTERNAL_STORAGE`在Content URI入口不替代 `ACCESS_MEDIA_LOCATION`；两种入口不可互相推导。

最后把常见观察量放回各自边界：

| 已观察到 | 可以证明 | 仍不能证明 |
|---|---|---|
| FUSE start callback成功 | libfuse已init，session start已返回 | 卷扫描、默认目录维护完成 |
| 成功的远端volume-state callback返回 | 同步attach/detach/update调用返回 | 异步维护或所有DB行完成 |
| lookup/stat成功 | 路径与属性在当前门上可见 | open内容会被允许 |
| FUSE create回复成功 | lower文件已打开，行通常已先插入 | quota更新或媒体扫描完成 |
| URI query返回一行 | 当前SQL可见性允许观察该行 | write、原始位置字节或direct path均允许 |
| exact write grant存在 | 以同一获grant的item URI操作时row-level global write可放行 | collection query自动看见该项、FUSE按路径的collection查询能先找到该行、强类型grant同时匹配重建的 `Files/<id>` URI、Provider-created pending owner门或Content URI原始位置字节放行 |
| `checkPermissionManager()`返回true | 普通共享区和DB row-level能力扩大 | 其他包data/obb/sandbox、Content URI入口的 `ACCESS_MEDIA_LOCATION`及pending/trashed默认过滤全部消失 |
| PermissionActivity返回OK | write的grant循环或trash/favorite/delete的batch尝试已经结束 | 每个grant或batch item必然成功；调用者原失败写操作已被自动重试 |

排障时按“FUSE启动快照→vold卷mount→user connection与volume session→Zygote进程视图→native操作类型→FUSE UID身份缓存→private-path前置门→DB owner与状态过滤→URI grant→redaction→扫描/失效异步任务”逐层记录。第273章的核心不是背一张权限表，而是承认这些完成点有不同的时钟；只有把具体入口、UID、路径、row、mode和观察时刻放在一起，才能解释Android 11 scoped storage看似矛盾的结果。

下一章转入MediaProvider数据库、volume attach与ModernMediaScanner，继续追“磁盘已有文件”和“可查询row已经收敛”之间的扫描、reconcile与过期清理边界。
