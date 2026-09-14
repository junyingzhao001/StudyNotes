# 257 Android DataLoader、Streaming Install、Incremental File System与按需安装链

本章源码基线是 Android 11 / API 30 / `android-11.0.0_r48`。这一版把“安装数据怎样进入 staging 目录”拆成三种模型：传统安装由安装器直接写普通文件；Streaming 由一个受绑定的 DataLoader 负责写普通 staging 文件；Incremental 则先建立可见但允许缺块的 IncFS 文件，读到缺块时再让 Loader 补块。下文把 DataLoader 简称 Loader，把 PackageManagerService 简称 PMS。三条路径最后都要回到 PackageInstallerSession 的 APK 校验和 PMS 安装链，但“文件存在”“安装可继续”“所有字节已到齐”“应用以后不再依赖 Loader”是四件不同的事。

本章沿一条真实提交链回答：一份只声明 location、文件名、大小、metadata 和可选 signature 的安装合同，怎样跨 Binder 绑定 DataLoader，怎样在 Streaming 与 Incremental 两条路上获得可读数据，哪个状态只表示阶段完成，以及原 Session listener 尚存时，安装后缺块怎样可能触发全用户卸载。

## 1. 先把问题说清：按需安装优化的是什么

传统安装的直觉是“先收完 APK，再验证，再安装”。它简单，却把首个可用时刻绑定到整包传输完成。DataLoader 把数据提供者从 PackageInstaller 客户端中抽出；IncFS 又把文件级等待缩小到块级等待，使系统可以先取得解析、签名检查和启动当前路径所需的块。

| 路径 | staging 中的对象 | 谁供数 | ready gate 的语义 | 之后能否按需补块 |
|---|---|---|---|---|
| 传统 | 普通完整文件 | Session 客户端经 `openWrite()`/`write()` | 没有 Loader 的 IMAGE_READY gate | 否 |
| Streaming | 普通文件 | 绑定的 Loader 经受限 callback 写入 | Loader 自报已准备；框架尚未核对声明长度与 APK 语义 | 否；Loader 随后被 destroy，缺少解析或验签实际需要的字节才会失败，声明 length本身没有另获满足证明 |
| Incremental | IncFS 逻辑文件与已写入块 | Loader 响应 pending read 或预取 | 允许 Session 继续解析、验签；这些读取本身仍可制造 pending read | 是；安装或运行路径尚未读取的块仍可缺失 |

所谓“流式”描述的是数据来源和时序，不等于 IncFS。Streaming 仍用普通文件，只是写文件的主体换成 Loader；Incremental 才具有逻辑 size 已建立、物理块尚未齐全、读取线程可因缺块阻塞的语义。

最值得保留的判断顺序是：

1. 当前 Session 是否配置了 `dataLoaderParams`。
2. type 是否精确为 STREAMING 或 INCREMENTAL。
3. 当前状态来自 Loader 生命周期还是 storage 健康度。
4. 当前完成点放行的是“开始加载”“准备安装”“PMS 提交”还是“全量装载”。

## 2. 一份安装字节经过哪些对象与进程

入口对象是 `PackageInstaller.SessionParams`。它保存 `DataLoaderParams`，后者只含 type、packageName、className 和自由格式 arguments。每次 `Session.addFile()` 再登记 location、name、length、metadata 与可选 signature。这里还没有打开源文件，也没有传入网络连接。

服务端把职责分成五层：

| 层 | 关键对象 | 只负责什么 |
|---|---|---|
| 安装合同 | `PackageInstallerSession` | 所有者、seal、文件清单、验证与最终安装调度 |
| 服务发现 | `DataLoaderManagerService` | 解析显式组件、bind、按 ID 保存连接 |
| Loader API | `DataLoaderService` / `IDataLoader` | create、start、prepare、stop、destroy |
| 增量存储 | `IncrementalFileStorages` / `IncrementalManager` | 建 storage、bind、placeholder、正式路径迁移 |
| native 数据面 | `IncrementalService` / `libdataloader` / IncFS | 三个控制 FD、缺块通知、写块和健康计时 |

先用一条总路线定位细节：安装合同 → bind/create/start →〔Streaming 写普通文件｜Incremental 建 placeholder、按缺块填充〕→ IMAGE_READY → Session 本地验证 → 用户确认与 PMS 安装 → Incremental 继续供给尚缺的运行期块。

三个近似词要预先拆开。`stageDir`或 staging 目录只是临时文件位置，不等于 `params.isStaged` 所说的可重启 staged session；调用 `commit()`只完成 seal并投递异步消息，`mCommitted=true`还要等 `streamValidateAndCommit()`越过 Loader gate与本地验证，两者都不等于 PMS 已完成内存和磁盘提交；`mDataLoaderFinished`只表示 Loader gate 已收口，也不能简称为“安装完成”。

Streaming 的 ID 直接使用 `sessionId` 调 `bindToDataLoader()`。Incremental 不沿用这个假设：这条创建路径把 root storage ID 设成与 mount ID 相同的数值，`DataLoaderStub`持有并用 mount ID 驱动 Manager 与 Loader，回调参数也按 mount ID核对。把所有回调 ID 都解释成 Session ID，会在排查 Incremental 日志时串错对象。

线程边界也不能省略。Session 的 Handler 串行执行 stream/validate/install 消息；ServiceConnection 默认回到 system_server 主线程；`IDataLoader`与`IDataLoaderStatusListener`都是 oneway，业务调用和状态可落在 Binder线程池；IncFS pending-read 与 page-read 又分别由 native Looper线程消费。status listener会在回调线程直接读写 `mDestroyed`、`mDataLoaderFinished`，只有重新验证和失败收口再投递 Session Handler。因此这不是一台单线程状态机，状态通知也不是调用栈上的同步完成证明。

### 练习 1：建立入口与对象地图

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void setDataLoaderParams' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F 'public void addFile(@FileLocation int location' frameworks/base/core/java/android/content/pm/PackageInstaller.java
grep -n -F 'public class DataLoaderManagerService' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'public abstract class DataLoaderService extends Service' frameworks/base/core/java/android/service/dataloader/DataLoaderService.java
grep -n -F 'public final class IncrementalFileStorages' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'bool IncrementalService::startLoading' frameworks/base/services/incremental/IncrementalService.cpp
```

把六个命中点按“声明合同—绑定服务—建立存储—启动供数”排序。再写下每层不能证明的事，例如 Manager bind 成功不能证明 `onCreate()` 成功，storage 创建成功不能证明 APK 可安装。

## 3. 合同入口：DataLoaderParams 与文件清单

`DataLoaderParams.forStreaming()` 和 `forIncremental()`只是设置不同 type；ComponentName 不是现成 Binder，arguments 也完全由 Loader 解释。SDK 标注 `setDataLoaderParams()` 需要 INSTALL_PACKAGES 与 USE_INSTALLER_V2，但服务端针对非空 DataLoader 参数新增的专门检查只有 USE_INSTALLER_V2；常规安装资格由 createSession 的其他门处理，不能把注解当成这一个分支里的 Binder enforce。

Session 构造时还拒绝 DataLoader APEX；Incremental 额外要求设备 feature 开启且 `incremental.allowed` 为真。源码也写了“不允许既有 system 或 updated-system App”的 helper，但构造器此时把尚未由 APK 解析建立的 `mPackageName`传进去；它仍是 null，查询不到 PackageSetting便返回 true。因此 r48 这道门表达了意图，却不能按当前调用点实际拒绝这类更新。当前实现也没有统一拒绝 NONE 或未知 type：只要参数非空就成为 DataLoader Session，`manualStartAndDestroy = !isIncrementalInstallation()` 又会把非 INCREMENTAL 值送入手工生命周期。因此 type 合法性不能只靠调用方约定来推断。

`addFile()`的真实检查顺序是：

- 调用方持有 USE_INSTALLER_V2；
- Session 配置了 DataLoader；
- 仅精确 STREAMING 会当场要求 `LOCATION_DATA_APP`；
- metadata 只要求非 `null`，空数组仍能登记；
- name 通过外部文件名校验；
- 锁内要求 owner/root、prepared 且未 sealed；
- `FileEntry` 的比较键使同一 `(location, finalName)` 不能重复。

length 不在这里校验为正数，signature 也允许为空。Incremental 的其他 location 直到 `IncrementalFileStorages.initialize()` 才被拒绝；空 metadata 在 Java 门能通过，但当显式 fileId 也为空时，后面的原生建文件可能失败。这里登记的是合同，不是成功建盘的承诺。

`removeFile()`是另一种合同。它要求 DataLoader Session 和非空 `appPackageName`，把 name 变成 `<name>.removed`，以 length=-1、metadata/signature=null 加入内存清单。它不创建物理 marker，也不当场验证 location；后续 Session 从后缀识别删除请求。

### 练习 2：逐项核对文件合同

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void addFile(int location, String name' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (isStreamingInstallation())' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'DataLoader installation requires valid metadata' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'new FileEntry(mFiles.size()' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'return (mFile.getLocation() == rhs.mFile.getLocation()) && TextUtils.equals(' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'public void removeFile(int location, String name)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'new InstallationFile(location, getRemoveMarkerName(name), -1, null, null)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Unknown file location: ' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'if (params.metadata.empty()) {' frameworks/base/services/incremental/BinderIncrementalService.cpp
grep -n -F 'return {EINVAL, {}, {}};' frameworks/base/services/incremental/BinderIncrementalService.cpp
```

沿命中行前后阅读，分别构造“STREAMING+MEDIA_DATA”“INCREMENTAL+MEDIA_DATA”“空 metadata”“null metadata”“同名不同 location”五个纸面用例，写出失败发生在登记期、初始化期还是更深的建文件期；再比较 `removeFile()`为何能登记 null metadata，却不进入 added-file 的 IncFS 建文件分支。

## 4. Session 建模：关闭普通写入口，保留受控供数口

一旦 `dataLoaderParams != null`，Session 的 `openWrite()`、`write()`与反向写入口都会经过 `assertCanWrite()`并抛错，`openRead()`也明确拒绝。安装器不能一边声明 Loader 合同，一边绕回普通 staging I/O；`getNames()`则改从有序 `mFiles` 生成，而不是枚举目录。

这不表示 seal 后没有任何写通道。Streaming 在 BOUND 状态创建专用 `FileSystemConnector`，它只收集 added 文件的 name 集合。callback 只检查 incomingFd 非空、name 在 allowlist，然后直接进入 `doWriteInternal()`；它不重跑 owner、USE_INSTALLER_V2、INSTALL_PACKAGES、prepared 或 sealed 检查，也不把本次 offset/length 与登记的 length 做一致性校验。正因为这是授予 Loader 的窄能力，Session sealed 后它仍能完成供数。

能力边界是“只能写已登记名字”，不是“写入内容自动可信”。`doWriteInternal()`仍校验最终文件名，目标以 `O_CREAT|O_WRONLY`打开而没有 `O_TRUNC`；正 offset 才 seek，负 offset实际留在 0。length 大于 0时用于预分配，但不会与登记 size比较，也不统一截断，因此重复短写可留下旧尾、越过声明 size也没有这层阻挡。

Java 文档还允许 `lengthBytes=-1`表达未知长度，但 callback把 -1原样交给 `FileUtils.copy(..., count)`；当前 userspace分支会构造负长度 `SizedInputStream`，其他快速复制分支也没有把它规范成“复制到 EOF”。所以不能把 -1写成这条实现上可靠的全量复制模式。Loader 返回 ready 后，Session必须重新解析、验签并检查 split/包名等 APK语义。

持久化又是另一张账。Session XML 保存 DataLoader type、组件、arguments，以及每个文件的 location、name、length、metadata、signature，重启后能重建合同；`mDataLoaderFinished`、`mIncrementalFileStorages`、status receiver 和 Manager 连接表是内存状态。普通非 staged Session 在开机读取后只会恢复对象并重新 seal，不会自动 recommit。进入 PMS正式路径迁移前，Incremental只有 temporary bind，收养会卸载它且在没有有效 permanent bind时删除 storage；但 `renameCodePath()`一旦成功就已创建 permanent bind，时间点早于后续 fs-verity、scan、reconcile和 PMS commit，崩溃窗口里可能出现“尚未安装成功却可收养”的 mount。于是 XML 恢复既不是自动续跑，也不能单独证明已下载块会透明续接。

删除请求最终由 Session 的 `.removed` 过滤器和 `validateApkInstallLocked()`消费。不能因为它曾被放入 `prepareImage()`参数，就推断 Java Loader 在 r48 真正看到了它；JNI 桥的丢参边界将在第6节单独处理。

## 5. 跨进程绑定：显式组件、调用用户与连接竞态

`DataLoaderManagerService`用 params 构造 ComponentName，再建立带 `ACTION_LOAD_DATA`且已 `setComponent()` 的 Intent。容易误读的地方在于：PMS 对显式组件走 `getServiceInfo()`分支，不做 action/filter 匹配。因此类存在却没声明对应 intent-filter，并不会仅因这个原因解析失败。

显式分支最多返回那个组件的一项。Manager 代码虽然用列表并“返回第一项”，但当前调用链不是从同一 package 的多个隐式候选里选择；把这段循环解释成不确定的多服务择一并不成立。真正要检查的是组件在指定 user 下能否取得 ServiceInfo，以及随后的显式 bind 能否成功。

Manager 没有接收 install target user 参数。resolve 与 bind 都使用 `UserHandle.getCallingUserId()`；这表示绑定用户来自该 Binder 调用时的身份，而不是 Session 中的目标 user 字段。常见调用发生在 system_server 内部，但审计时仍应看实际 identity，不能机械写成“永远绑定目标用户”或“永远 user 0”。

连接表是 `SparseArray<dataLoaderId, DataLoaderServiceConnection>`，但新连接直到 `onServiceConnected()`才 append。由此有两个边界：

1. bind 尚未回调的窗口中，同 ID 的第二次 bind 看不到第一条连接，两次都可能发起；回调时先 append 的获胜，另一条自行 unbind。
2. 表里已有 ID 时，`bindToDataLoader()`直接返回 true，不替换 listener，也不补发 BOUND。

第二点直接影响所谓重试。UNAVAILABLE 后再次 commit 并不保证重新走绑定；若旧连接仍在表里，Session 只得到一个 true，却没有新的状态事件。只有旧 Loader 后续主动报告、或 Service disconnect/died/null binding 触发 destroy/remove 后，新的 bind 才有机会建立。Manager 的断连日志虽写“尝试恢复”，当前类本身并没有自动 rebind 逻辑。

还有一个窄失败点：`bindServiceAsUser()`返回 false 后，代码立即对这条尚未成功绑定的 connection 调 `unbindService()`，该调用也没有局部 catch。阅读异常现场时不能只停在 boolean false。

### 练习 3：证明解析与绑定的真实边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'intent.setComponent(componentName);' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'queryIntentServicesAsUser(intent, 0, UserHandle.getCallingUserId())' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'if (mServiceConnections.get(dataLoaderId) != null)' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'return true;' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'UserHandle.of(UserHandle.getCallingUserId()))' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'if (!append())' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'mServiceConnections.append(mId, this);' frameworks/base/services/core/java/com/android/server/pm/DataLoaderManagerService.java
grep -n -F 'final ServiceInfo si = getServiceInfo(comp, flags, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

画出“第一次 bind 尚未连接、第二次 bind 到来、两个 onServiceConnected 先后返回”的时序。再说明为什么 existing-map 快路能防重复连接，却也会让没有新状态事件的 recommit 静默等待。

## 6. Java/native 双实现：生命周期是谁推进的

`DataLoaderService`表面只有两个实现者回调：`onCreate()`与`onPrepareImage()`。Binder 的 create/start/stop/destroy/prepare 全部先进入 native `libdataloader`。native 先尝试进程注册的自定义 factory；若它不接收该 type，才回退到 `ManagedDataLoader`，把调用转回 Java 接口。

系统 shell Loader 正好展示两条实现：STREAMING 由 Java `PackageManagerShellCommandDataLoader.DataLoader`处理普通文件；INCREMENTAL 由注册到 native factory 的 `PMSCDataLoader`处理 pending reads、page reads 和写块。不能从同一个 Service 类名推断两种 type 都走相同 Java 回调。

生命周期状态主要由框架合成：

- native create 成功后报告 CREATED；
- 选中 Loader 的 `onStart()`成功；control 中若有有效 pending/log FD再注册它们，随后报告 STARTED；
- stop、destroy 分别报告 STOPPED、DESTROYED；
- `onPrepareImage()`布尔结果被翻译成 IMAGE_READY 或 IMAGE_NOT_READY；
- create/start 失败可报告 UNAVAILABLE。

自定义 native Loader 能主动上报的公开枚举范围在当前头文件里只有 UNRECOVERABLE。也就是说业务 Loader 不能随意伪造 BOUND、CREATED 或 IMAGE_READY；这些阶段由 Manager 与桥接层拥有。

r48 的桥还有两个必须显式保留的缺口。`DataLoaderService_OnPrepareImage()`接受 `removedFiles`参数却完全不使用；NDK 接口只有 addedFiles，`ManagedDataLoader`再调用 Java 时传入 `null`，尽管 Java 参数标了 `@NonNull`。同时 native 转换 added file 只保留 location、name、size、metadata，Managed 层重建 `InstallationFile`时把 signature 设为 null。Session 自己仍保存并消费删除标记；Incremental 建 placeholder 时也已把 signature 交给 storage，但 Java Loader 不能据此认为自己收到了这两项。

create 完成后，`DataLoaderService`会关闭传入 Java 的 cmd、pendingReads、log PFD；native 在此之前已 `dup` 成自己的 control，所以关闭的是跨 Binder 传来的 Java 端副本，不是立即切断 native Loader。

### 练习 4：追一遍 Java/native 适配

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'nativeCreateDataLoader(id, control, params, listener)' frameworks/base/core/java/android/service/dataloader/DataLoaderService.java
grep -n -F 'dataLoader.prepareImage(' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'void prepareImage(int id, in InstallationFileParcel[] addedFiles, in @utf8InCpp String[] removedFiles);' frameworks/base/core/java/android/content/pm/IDataLoader.aidl
grep -n -F 'managedDataLoaderFactory' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'reportStatusViaCallback(env, listener, storageId, jni.constants.DATA_LOADER_CREATED)' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'bool DataLoaderService_OnPrepareImage' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'jobjectArray removedFiles' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'files.emplace_back(location, std::move(name), size, std::move(metadata));' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'bool (*onPrepareImage)(struct DataLoader* self, const DataLoaderInstallationFile addedFiles[],' system/incremental_delivery/libdataloader/include/dataloader_ndk.h
grep -n -F 'name, size, metadata, nullptr);' system/incremental_delivery/libdataloader/ManagedDataLoader.cpp
grep -n -F 'jni.dataLoaderOnPrepareImage, jaddedFiles, nullptr' system/incremental_delivery/libdataloader/ManagedDataLoader.cpp
grep -n -F 'DATA_LOADER_FIRST_STATUS = DATA_LOADER_UNRECOVERABLE' system/incremental_delivery/libdataloader/include/dataloader_ndk.h
```

把 prepare 参数在 Session、IDataLoader、JNI、NDK、Managed Java 五层逐项列出。最后指出 removedFiles 和 signature 分别在哪一层消失，以及它们仍由哪个系统组件消费。

## 7. Streaming 主线：普通 staging 文件怎样被填满

Session seal 后，`streamAndValidateLocked()`先调用 `prepareDataLoaderLocked()`。对 Streaming，精确序列如下：

| 事件 | 发起者 | Session 动作 |
|---|---|---|
| bind(sessionId) | Session | 等待异步连接 |
| BOUND | Manager | 取 `IDataLoader`，create，并传 callback 型 FileSystemControlParcel |
| CREATED | native bridge | 调 start |
| STARTED | native bridge | 调 prepareImage(added, removed) |
| IMAGE_READY | native bridge | 置 `mDataLoaderFinished`，重投 parent/self 验证，再调 Loader destroy |
| IMAGE_NOT_READY | native bridge | 置 `mDataLoaderFinished`，派发 verification failure，再 destroy |

BOUND 只证明 ServiceConnection 已得到 Binder；CREATED 才证明 factory、Java/native `onCreate()`与 connector 建立成功；STARTED 只证明 Loader 已进入可供数状态；IMAGE_READY 则只证明 Loader 的 `onPrepareImage()`返回 true。此时 Session 还没有完成 `validateApkInstallLocked()`，更没有进入 PMS commit。

Streaming 的 `FileSystemControlParcel`只设置 callback，没有 Incremental control。Java Loader 可从 stdin、本地 FD、网络缓存或其他来源读数据，再调用 `FileSystemConnector.writeData()`。服务端 allowlist 以 name 控制目标，写入仍是普通 staging 文件；没有 placeholder、pending read 或 4 KiB 按页恢复语义。

`onPrepareImage()`可以多次调用 `writeData()`填不同区间，但登记 length 与调用 length 不构成框架自动核对。写成功也不证明内容属于声明包，只有 Session 后续解析出的 package、split、version、签名与继承关系才是安装裁决依据。

当 IMAGE_READY 回调到达 multi-package child，child 不直接安装，而是唤醒 parent 的 stream/validate 消息。parent 会重新遍历所有 child；只有每个 child 都越过 Loader gate、完成本地 APK验证并让 `streamValidateAndCommit()`返回 true，才发送 MSG_INSTALL。IMAGE_READY只是重新检查的门铃，不是“全组已验证”或绕过组原子性的许可。

### 练习 5：把 Streaming 五个状态写成状态表

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final boolean manualStartAndDestroy = !isIncrementalInstallation();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'control.callback = new FileSystemConnector(addedFiles);' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'case IDataLoaderStatusListener.DATA_LOADER_BOUND' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'case IDataLoaderStatusListener.DATA_LOADER_CREATED' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'case IDataLoaderStatusListener.DATA_LOADER_STARTED' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'case IDataLoaderStatusListener.DATA_LOADER_IMAGE_READY' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'case IDataLoaderStatusListener.DATA_LOADER_IMAGE_NOT_READY' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'validateApkInstallLocked();' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'mCommitted = true;' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'private final class FileSystemConnector extends' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'File name is not in the list of added files.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'O_CREAT | O_WRONLY' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (offsetBytes > 0)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'FileUtils.copy(incomingFd.getFileDescriptor(), targetPfd.getFileDescriptor(),' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (mLength <= 0) {' frameworks/base/core/java/com/android/internal/util/SizedInputStream.java
```

沿命中行前后阅读，给每个状态写三列：“已经证明”“尚未证明”“下一动作”。特别比较 IMAGE_READY 与 `validateApkInstallLocked()`通过；再用最后五个命中点解释 callback 的名字能力边界、负 offset、无 `O_TRUNC`及 length=-1，为何都不能证明供数完整。

## 8. Streaming 状态门：可重试通知与终态失败

Session对九个 DataLoader状态并非同等处理。下表描述的是 STOPPED/DESTROYED早返回，或其余状态成功从 Manager取到 `IDataLoader`之后的主分支：

| 状态 | 当前 Session 处理 | 是否设置 `mDataLoaderFinished` |
|---|---|---|
| STOPPED / DESTROYED | 直接忽略 | 否 |
| BOUND / CREATED / STARTED | 推进手工状态机 | 否 |
| IMAGE_READY | 重新调度验证 | 是 |
| IMAGE_NOT_READY | 安装失败并清理 | 是 |
| UNAVAILABLE | 回传 STATUS_PENDING_STREAMING | 否 |
| UNRECOVERABLE | 安装失败 | 是 |

当 Session既未 `mDestroyed`也未 `mDataLoaderFinished`时，任何其他状态都会先从 Manager取 `IDataLoader`；若连接表中取不到对象，会立即把本轮视为 MEDIA_UNAVAILABLE失败。RemoteException则被降为 pending streaming，保留再次 commit的表面机会。这两个失败看似相近，生命周期后果并不相同。`mDestroyed`或 `mDataLoaderFinished`置位后，则只对迟到的 UNRECOVERABLE调用 storage-unhealthy路径，其余状态都忽略。

`sendPendingStreaming()`只向当前 `mRemoteStatusReceiver`发送 sessionId、STATUS_PENDING_STREAMING 和原因；receiver 缺失时只记日志。它不自行排重试计时器，不复位 Manager 连接，也不回滚 seal。调用方需要把 pending 当成“尚无终局”，而不是成功或永久失败。

更危险的是重试实现缺口。create 失败时 native 可报告 UNAVAILABLE 并移除自己的 connector，但 Manager 的 ServiceConnection 仍可能留在 map；Session 在 ready/not-ready 调的是远端 `IDataLoader.destroy(id)`，也不是 Manager 的 `unbindFromDataLoader()`。下一次 bind 因 ID 已存在直接 true，却没有 BOUND 重放。于是“允许 recommit”是 API 意图，不是每种旧连接状态下都能自动恢复的保证。

Service disconnect、binding died 或 null binding 会由 Manager 发 DESTROYED、destroy 并 remove；Session 却忽略 DESTROYED。等到调用方再次 commit 且 map 已清，才可能真正新建连接。诊断卡住的 PENDING_STREAMING 时，必须同时看 Session 的 `mDataLoaderFinished`、Manager map 与 native connector 三张账。

## 9. Incremental 建盘：storage、临时 bind 与 placeholder

Incremental 先过可用性与属性门：内核/用户空间 IncFS feature 可用，系统属性允许。构造器随后调用 system/updated-system 检查，但传入尚未解析出来的 null `mPackageName`，helper 因查不到 PackageSetting而放行；这不是一条有效的目标包限制。构造器对系统 Loader package 的 shell/system 身份检查还错误地用了 Java 字符串 `==`，其命中依赖对象同一性；健康宽容判断也有同样缺口。

普通路径由 `IncrementalFileStorages`调用 `IncrementalManager.createStorage(stageDir, CREATE|TEMPORARY_BIND, autoStart=false, ...)`。`IncrementalService`要求 mount point 是绝对空目录，建立 backing/mount、从 vold 取得 cmd、pendingReads、log 三个 FD，分配 mount ID，创建默认 storage 与临时 bind，再构造 `DataLoaderStub`。

另有 packageName 等于 `"local"` 的内部路径：arguments 必须给出已有 Incremental 路径，Manager `openStorage()`后把它 bind 到 stageDir。这是当前实现约定，不是任意应用都应依赖的稳定 SDK 模式。

接着 initialize 遍历 addedFiles。这里只有 `LOCATION_DATA_APP`可接受；目标名不存在时调用 `makeFile(name, size, null, metadata, signature)`，已存在则跳过，不复核既有对象的 size、metadata 或 signature 是否与新合同一致。placeholder 此时有目录项、逻辑长度和文件身份，但数据块可以为空。

所有 placeholder 建好后才 `startLoading()`。initialize 随即把 storage wrapper保存在 Session 内并返回；`prepareDataLoaderLocked()`返回 false，让外层停在 stream gate。后续 IMAGE_READY 回调再唤醒验证。commit 重试若 wrapper 仍在，只调用 `startLoading()`，不会重建 storage 或文件。还有一个非事务窗口：构造器完成后，added-file循环或 `startLoading()`抛错并不经过构造器内部的 `cleanUp()`，而 Session 要到 initialize 成功返回后才持有 wrapper；残留 temporary storage/bind只能等待其他清理路径或重启收养阶段处理。

### 练习 6：核对建盘顺序与延迟校验

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'Incremental installation not allowed.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Incremental installation of this package is not allowed.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (!isIncrementalInstallationAllowed(mPackageName))' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (existingPkgSetting == null || existingPkgSetting.pkg == null)' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'CREATE_MODE_TEMPORARY_BIND' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'getPackageName().equals("local")' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'result.addApkFile(file);' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'if (!targetFile.exists())' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'mDefaultStorage.makeFile(apkName, apk.size, null, apk.metadata, apk.signature);' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'result.startLoading();' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'Mounting over existing non-empty directory is not supported' frameworks/base/services/incremental/IncrementalService.cpp
```

沿命中行前后阅读，按 temporary bind → added-file循环 → `addApkFile()`/`makeFile()` → `startLoading()`重建顺序。对一个 location 非 DATA_APP 的文件标出每步可达性；再回答同名 placeholder为何不能证明合同一致，以及循环或 start 抛错时谁还持有可清理 wrapper。

## 10. IncFS 数据面：4 KiB 块、三个 FD 与按需供给

IncFS 把“文件逻辑存在”与“块已写入 backing store”拆开。当前用户空间常量 `INCFS_DATA_FILE_BLOCK_SIZE`为 4096；文件可先报告完整 size，读取命中缺块时，内核把请求放进 pendingReads。挂载默认 `read_timeout_ms`是 10000，因此读取可能阻塞但不是无限等；Loader按 fileId 和 block index找数据，再填充目标块。

Installer 路径没有显式 UUID，于是 BinderIncrementalService 在 `fileId`为空时从 metadata派生 ID。metadata长度不超过 16字节时直接复制并以零补满，超过 16字节时取 SHA-1结果的前16字节；这是确定性映射，不是无碰撞承诺。metadata因而同时承担 Loader路由与文件身份种子的角色，但它不是系统统一规定的 URL格式。系统 shell Loader把 mode和 file index编进 metadata；其他 Loader可以定义自己的编码，只要建文件和供数侧一致。

Java `addFile()`只拒绝 null metadata，原生转换却要求 fileId与metadata不能同时为空；空数组最终返回 EINVAL。内核接口还把 file attribute限制为 512字节、signature限制为8096字节，负 file size在 `IncFs_MakeFile()`返回 ERANGE。这些都属于登记之后才显现的边界。

三个 FD 不应合并理解：

| FD | 方向与作用 | 正确性地位 |
|---|---|---|
| cmd/control | 为目标文件取得 permit-fill 能力并打开 special-ops FD | 授权供数所需，不承载块 payload |
| pendingReads | 内核报告尚未满足的读请求 | 按需恢复所需 |
| log | 报告已经发生的 page reads | 预取、trace 与优化观察 |

块 payload的真实落点是 per-file special-ops FD：`openForSpecialOps()`先借 cmd FD发 `INCFS_IOC_PERMIT_FILL`，随后每个 `IncFsDataBlock.fileFd`指向获准的文件 FD，`writeBlocks()`对它执行 `INCFS_IOC_FILL_BLOCKS`。data block与 hash block都走这条 per-file ioctl路径，不能简写成“把数据写入 cmd FD”。

IncrementalService把三个 control FD的副本装进 `FileSystemControlParcel`交给 Loader进程；`libdataloader`直接读取 pendingReads/log，不由 IncrementalService逐条转发。服务端为了健康度另开 pendingReads control，只观察最老欠读时间。目标进程里，pendingReads与 log各有 Looper线程；STARTED前后注册有效 FD，stop先令 `mRunning=false`、移除 FD，再等待两个回调临界区退出。pending回调每次最多准备256个 `ReadInfo`槽，循环取完当前可见请求；page log走另一缓冲与回调。

系统 native shell Loader在 `onPendingReads()`中第一次看见某个 fileId时先发 PREFETCH，并对每个 pending block再发 BLOCK_MISSING；接收线程解析服务端块头，按 file index打开 special-ops FD，批量 `writeBlocks()`。这条链说明“缺块唤醒 Loader”不等于“缺块立刻到达”，网络、adb对端、块头解析与 IncFS写入都可能成为等待点。

read log 可以动态开关，也可被平台永久关闭；pending read不能因此消失。前者泄露访问模式并服务优化，后者是满足阻塞读取的控制面。关闭 log不会把 Incremental降级成 Streaming，更不会证明所有块已齐。

### 练习 7：把一次缺块画成闭环

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'constexpr int kBlockSize = INCFS_DATA_FILE_BLOCK_SIZE;' system/incremental_delivery/libdataloader/include/dataloader.h
grep -n -F '#define INCFS_DATA_FILE_BLOCK_SIZE 4096' system/incremental_delivery/incfs/kernel-headers/linux/incrementalfs.h
grep -n -F 'INCFS_DEFAULT_READ_TIMEOUT_MS = 10000' system/incremental_delivery/incfs/include/incfs_ndk.h
grep -n -F 'static constexpr auto kPendingReadsBufferSize = 256;' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'waitForPendingReads(mControl, 0ms, &pendingReads)' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'mDataLoader->onPendingReads' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'mDataLoader->onPageReads' system/incremental_delivery/libdataloader/DataLoaderConnector.cpp
grep -n -F 'if (params.fileId.empty())' frameworks/base/services/incremental/BinderIncrementalService.cpp
grep -n -F 'id = IncrementalService::idFromMetadata(params.metadata);' frameworks/base/services/incremental/BinderIncrementalService.cpp
grep -n -F 'if (size_t(metadata.size) <= sizeof(id))' system/incremental_delivery/incfs/incfs.cpp
grep -n -F 'SHA1_Update(&ctx, metadata.data, metadata.size);' system/incremental_delivery/incfs/incfs.cpp
grep -n -F 'INCFS_IOC_PERMIT_FILL' system/incremental_delivery/incfs/incfs.cpp
grep -n -F 'INCFS_IOC_FILL_BLOCKS' system/incremental_delivery/incfs/incfs.cpp
grep -n -F 'sendRequest(mOutFd, BLOCK_MISSING, fileIdx, blockIdx);' frameworks/base/services/core/jni/com_android_server_pm_PackageManagerShellCommandDataLoader.cpp
grep -n -F 'auto read = ::read(incomingFd.get(), buffer->data() + size, toRead);' frameworks/base/services/core/jni/com_android_server_pm_PackageManagerShellCommandDataLoader.cpp
grep -n -F 'mIfs->writeBlocks(instructions)' frameworks/base/services/core/jni/com_android_server_pm_PackageManagerShellCommandDataLoader.cpp
grep -n -F 'blocks[i].kind == INCFS_BLOCK_KIND_HASH' system/incremental_delivery/incfs/incfs.cpp
grep -n -F 'if (v4signatureBytes == null || v4signatureBytes.length == 0)' frameworks/base/core/java/android/os/incremental/IncrementalStorage.java
grep -n -F 'read_id_sig_headers' system/core/adb/client/incremental_utils.cpp
grep -n -F 'return verifySigner(signingInfo, signedData);' frameworks/base/core/java/android/util/apk/ApkSignatureSchemeV4Verifier.java
```

从应用线程读取一个未到块开始，写出 kernel、pending FD、native Looper、Loader数据源、writeBlocks、原读取返回六个节点。然后把 log FD从图中删掉，验证正确性闭环仍然成立；最后标出 V4 header、Merkle tree hash block和 signer密码学校验各落在哪一层。

## 11. V4 与 Merkle 边界：块可验证不等于包可安装

`signature`在 `addFile()`是可选项，`IncrementalStorage.validateV4Signature()`遇到 null或空数组会直接返回，libincfs也允许 unverified file。只有提供非空字节时，r48才检查它可解析为 version 2的 V4 header，并限制 SHA-256、log2 block size=12、空 salt、32字节 rawRootHash和 additionalData最大128字节。

这段方法名容易造成两个误判。第一，它验证的是序列化结构与 IncFS 可接受参数，不在这里执行 signingInfo 的公钥签名校验；第二，`rawRootHash`在当前类注释中是首个 Merkle tree page 的 salted digest，不宜直接改写为“整个 APK 的裸根哈希”。

`.idsig`也不能与 `InstallationFile.signature`混成同一字节块。adb侧文件由 version、hashingInfo、signingInfo、treeSize和后续 Merkle tree组成；传给 `addFile()`/`makeFile()`的 signature只是前部 V4 header，树本体由 Loader另以 `INCFS_BLOCK_KIND_HASH`块填入。显式 `adb install --incremental`会要求配套 `.idsig`，不代表底层 Session API也把 signature设为必填。

真正的 APK身份验证在后面的 `ApkSignatureVerifier`。V4路径会提取并验证 V4 signer，再取得 V2/V3证书与 digest，核对证书和 APK digest一致。若 V4不存在且最低版本允许，验证器会退回 V3、V2或更老方案。Incremental+V4只是在安装前跳过外部 package verifier广播，成功提交后再带 root hash发已验证通知；它没有跳过 APK签名、包名、升级证书兼容或 PMS reconcile。

因此应分三层说安全：

- IncFS/Merkle：已提供 hash 信息时，保护独立块与树的对应关系；
- APK V4/V2/V3：证明 APK 内容与 signer，并交叉核对 V4和V2/V3；
- PackageInstaller/PMS：判断这组 APK 是否构成合法安装、是否能更新现有包。

没有 signature 不等于跳过后两层；有 signature 也不等于 `makeFile()`时已经得到可安装结论。Streaming 的 Java Loader又因 JNI适配丢失 signature，更不能把它当作 Loader 业务参数。

## 12. Incremental 状态机：startLoading 与 IMAGE_READY

Incremental 不让 Session 手工 create/start/destroy。`IncrementalService::DataLoaderStub`保存 currentStatus 与 targetStatus：目标 CREATED 时从 DESTROYED/UNAVAILABLE 去 bind，从 BOUND 去 create；目标 STARTED 时从 CREATED/STOPPED 去 start，必要时先沿 CREATED 路径补齐前序。`IncrementalService::startLoading()`只调用 `requestStart()`并立即返回 true，连该调用的 boolean结果也没有向上传递；requestStart只是改目标并运行一步 FSM，更不会等待所有异步状态到齐。

Manager 报 BOUND 后，Stub继续 create；native报 CREATED后继续 start；native报 STARTED后，Session listener调 `prepareImage()`。native shell Loader会按 metadata模式复制本地数据、V4 header或树块；而 metadata 内部的 STREAMING mode（外层安装 type仍为 INCREMENTAL）可以只完成 OKAY握手、启动 receiver线程便返回 true。桥随即报告 IMAGE_READY，后面的 PackageParser或验签读取仍可能首次制造 pending read。

IMAGE_READY 的 AIDL注释是“已流入继续安装所需的一切”，不是“文件 fully loaded”。Java wrapper虽暴露 `isFileFullyLoaded()`与 range 查询，但 r48 的 `BinderIncrementalService::isFileRangeLoaded()`固定写回 false；这条公开包装链不能给出“已全量”的正面证明，Session ready gate也没有调用它。若要证明全量，只能另核对底层已填充范围或数据源事实；包安装成功后，正式 code path 中的其他块仍可由相同 mount 的 DataLoader按需补齐。

UNAVAILABLE 到达 Stub 时，它在同一把锁内先更新 current、再把 target改为 DESTROYED；解锁后才通知 Session，随后 `fsmStep()`执行 destroy。Session收到通知只发 pending；再次 commit可调用 `startLoading()`，Stub才有机会从 reset/destroyed态重新绑定。这个增量恢复链由 Stub拥有，不能套用 Streaming 直接 bind(sessionId) 的状态图。

Stub 初始化还会立刻计算 health；一旦 pending read出现，健康逻辑会把 target设为 STARTED并尝试推进。这使真实应用读取也能唤醒已停止或尚未完成启动的 Loader。生命周期状态回答“Loader能否工作”，健康状态回答“最老缺块等了多久”，两张表互相触发但不能合并。

### 练习 8：手推 DataLoaderStub 的目标状态

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'dataLoaderStub->requestStart();' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'bool IncrementalService::startLoading(StorageId storage) const' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'return setTargetStatus(IDataLoaderStatusListener::DATA_LOADER_STARTED);' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'bool IncrementalService::DataLoaderStub::fsmStep()' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'case IDataLoaderStatusListener::DATA_LOADER_STARTED' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'case IDataLoaderStatusListener::DATA_LOADER_CREATED' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'return start();' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'return bind();' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'return create();' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'mCurrentStatus = newStatus;' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'if (mCurrentStatus == IDataLoaderStatusListener::DATA_LOADER_UNAVAILABLE)' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'setTargetStatusLocked(IDataLoaderStatusListener::DATA_LOADER_DESTROYED)' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'listener->onStatusChanged(mountId, newStatus);' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'fsmStep();' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'setTargetStatusLocked(IDataLoaderStatusListener::DATA_LOADER_STARTED)' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'public boolean isFileFullyLoaded' frameworks/base/core/java/android/os/incremental/IncrementalStorage.java
grep -n -F 'binder::Status BinderIncrementalService::isFileRangeLoaded' frameworks/base/services/incremental/BinderIncrementalService.cpp
grep -n -F '*_aidl_return = false;' frameworks/base/services/incremental/BinderIncrementalService.cpp
```

沿命中行前后阅读，分别从 DESTROYED、UNAVAILABLE、BOUND、CREATED、STOPPED 五个 currentStatus 出发，把 target 设为 STARTED，写出下一步调用。指出 `startLoading()`哪一步同步返回、哪些转换仍要等回调；再解释为何 Java 的 fully-loaded方法在 r48不能返回 true作为完成证据。

## 13. 健康状态机：缺块计时怎样改变安装结局

健康检查重新打开一份 pendingReads control，取所有当前请求中最老的 kernel boot-clock 时间戳，并用首次 epoll 回调建立用户态基线。没有 pending read或基线无效时报告 OK并重新监听；存在请求时确保 Loader目标为 STARTED，再按等待年龄分级。

Session 给出的阈值是 blocked 2000 ms、unhealthy 7000 ms、unhealthy 后每 60000 ms继续监控；native 逻辑还有 500 ms调度容差。四态含义是：

| health | 含义 | 仅由时间能证明什么 |
|---|---|---|
| OK | 当前没有可见 pending read | 此刻无欠块请求，不证明全文件齐 |
| READS_PENDING | 有请求，`delta + 500ms`仍未到 blocked 门 | 读取正在等 |
| BLOCKED | 进入 blocked 判定窗口 | 实际年龄可能比 blocked阈值短最多约 500ms，仍未进 unhealthy窗口 |
| UNHEALTHY | 进入 unhealthy 判定窗口 | 实际年龄可能比 unhealthy阈值短最多约 500ms |

Session 的策略并不对所有 Loader一视同仁。在 IMAGE_READY 前且 `mDataLoaderFinished=false`时，普通 Loader遇到 READS_PENDING 或 BLOCKED会直接 fall through到失败处理，也就是首次观察到欠块就能使本轮安装失败；预期的系统 adb Loader才容忍前两态，等到 UNHEALTHY再失败。但 `systemDataLoader`同样用 packageName的 `==`比较，宽容分支不具备可靠值语义保证。IMAGE_READY 后即使 PMS尚未安装完，PENDING与BLOCKED也已被 finished早退忽略。

一旦 `mDataLoaderFinished`或 Session destroyed，OK、PENDING、BLOCKED都被忽略，只有 UNHEALTHY调 `onStorageUnhealthy()`；迟到的 UNRECOVERABLE也会走同一路径。该方法并不另查这次新版本是否已提交，只检查 `mPackageName`是否为空；非空便由 Handler调用 `deletePackageX(package, VERSION_CODE_HIGHEST, USER_SYSTEM, DELETE_ALL_USERS)`。源码注释把该分支称作“App已安装”，但 finished也会在 NOT_READY、UNRECOVERABLE和安装前健康失败时置 true，真正保护删除动作的窄门只是包名有没有在解析中建立。user参数是 USER_SYSTEM，但 flag明确要求全用户删除，不能写成只卸载 user 0。

这解释了为什么安装成功不是 storage 生命周期终点，但卸载结论必须收窄：只有原 Session listener仍存活的同一 system_server生命周期里，欠读进入 UNHEALTHY（或收到上述迟到状态）且 `mPackageName`非空，才会触发删除。Loader断连但没有 pending read时 health仍可为 OK，不会仅因“失联”卸载；重启后收养出来的 Stub又没有恢复 Session status/health listener，并会禁用 advanced health，因此这不是跨 system_server重启的持久卸载保证。

## 14. 正式路径迁移、read log 与长期依赖

普通 FileInstallArgs 用 `rename(2)`把 stage 目录移到最终 code path；Incremental 不能把 bind mount当普通目录重命名。`IncrementalManager.renameCodePath()`先打开 stage 对应 storage，在最终父目录创建与原 mount链接的新 storage并使用 CREATE|PERMANENT_BIND，再递归建立目录和文件 link，最后解除旧 stage bind。

这里的 link 没有复制所有文件字节。新 code path和旧 storage共享同一 IncFS mount中的文件对象与已到块，永久 bind 的 metadata使服务重启后可以收养挂载关系。收养会重建无 Session listener 的 DataLoaderStub；它保留供数可能性，却不恢复上一节那套 Session卸载回调。失败时会解除新 target bind，但这仍不是跨所有文件与外部状态的通用事务。

Session结束清理 `IncrementalFileStorages`时只尝试 unbind stageDir并清 wrapper引用，不删除整个 storage。正式 permanent bind和 DataLoaderStub才因此能在安装后继续服务。把 `cleanUp()`理解成下载取消或 backing store删除，会与运行期按需读取相冲突。

read log是单独的隐私/优化面。若 installer UID 是 shell、安装为 Incremental，且 base APK既非 debuggable也非 profilableByShell，Session在 APK验证阶段永久关闭 read logs。IncrementalService保存关闭标记并更新 mount option；pendingReads与块供给仍继续。系统 native shell Loader还可随 adb trace tag动态申请日志，但永久关闭后的服务端策略是更高门。

PMS 对 Incremental+V4 的 verifier顺序也有差异：安装前跳过外部 package verification请求，PMS完成解析、scan、reconcile、commit后才广播 allow与 root hash。这是减少全量读取依赖的流程调整，不是绕过内部签名与安装事务。

## 15. 多包、重启与失败诊断

multi-package parent 自身跳过 DataLoader与 APK验证，逐个调用 child 的 `streamValidateAndCommit()`。任何 child返回 false都会令整组停下；在这次同步遍历中，child抛 `PackageManagerException`才会让 parent与其他未失败 child一起走 verification failure；只有全部 child 都完成 Loader gate与本地验证、方法返回 true，才发 MSG_INSTALL。异步状态不是同一条传播链：child 的 IMAGE_READY会唤醒 parent重新扫描，但 IMAGE_NOT_READY/UNRECOVERABLE只向 child派发失败；后续 ChildStatusIntentReceiver把失败改成 parent sessionId转发并清空跟踪，并不执行同一组级 parent/sibling destroy。因此“多包失败必然立即清理全组”在 r48并不成立。

Session XML保存 params与文件合同，却不保存 `mDataLoaderFinished`、live listener、status receiver、Manager map或 Java/native connector。普通 Session开机读取后不会自动 recommit；收养逻辑会卸载 PMS正式路径迁移前残留的 temporary bind，没有 valid permanent bind就删除 storage。已经成功执行 `renameCodePath()`的 permanent bind可被收养，但它既可能来自已安装包，也可能落在 rename之后、后续安装事务完成之前的崩溃窗口；新 Stub同样不带旧 Session status/health listener。恢复问题必须拆成“合同可重建”“permanent mount可收养”“Loader可重绑”“包事实是否已提交”“谁会重新发起安装”五问，不能只看到 XML里有 arguments就宣称断点完整恢复。

遇到 pending或卡住，建议按以下证据顺序定位：

1. Session：是否 DataLoader、type、sealed、`mDataLoaderFinished`、parent/child。
2. Manager：该 ID是否已在 map，Binder是否非空，listener是否仍是本次 Session。
3. 生命周期：最后状态是 BOUND、CREATED、STARTED、UNAVAILABLE还是 DESTROYED。
4. Incremental：stage path是否真在 IncFS、storage/mount ID与 Session ID是否被混用。
5. 数据面：是 pending read无响应、块写失败，还是 page log被关闭。
6. 健康面：最老请求年龄落在哪个阈值，system Loader值比较是否命中。
7. 安装面：IMAGE_READY之后究竟失败在 APK parse/signature、split校验、PMS scan还是 reconcile。

版本风险同样要落在结论里：本章对应 Android 11 / API 30 / `android-11.0.0_r48`。这一代接口带有 SystemApi实验警告，字符串引用比较、remove参数丢失、signature不传 Java、已有连接不重放状态等都是当前源码事实，不应外推为后续 Android 的固定契约。

### 练习 9：完成一次对抗式故障归因

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'sendPendingStreaming("DataLoader unavailable")' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Missing receiver for pending streaming status.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'params.getComponentName().getPackageName() == SYSTEM_DATA_LOADER_PACKAGE' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'Image is missing pages required for installation.' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'if (mDestroyed || mDataLoaderFinished) {' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'PackageManager.DELETE_ALL_USERS' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'IncrementalManager.CREATE_MODE_PERMANENT_BIND' frameworks/base/core/java/android/os/incremental/IncrementalManager.java
grep -n -F 'if (!args.doRename(res.returnCode, parsedPackage))' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'setUpFsVerityIfPossible(parsedPackage);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mIncrementalManager.renameCodePath(beforeCodeFile, afterCodeFile);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mDefaultStorage.unBind(mStageDir.getAbsolutePath())' frameworks/base/core/java/android/os/incremental/IncrementalFileStorages.java
grep -n -F 'writeByteArrayAttribute(out, ATTR_SIGNATURE, file.getSignature())' frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java
grep -n -F 'those are probably temporary binds' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'deleteStorage(*ifs);' frameworks/base/services/incremental/IncrementalService.cpp
grep -n -F 'mHealthCheckParams.blockedTimeoutMs = -1;' frameworks/base/services/incremental/IncrementalService.cpp
```

沿命中行前后阅读，设定三个现场：A 为 Streaming反复 PENDING且 Service仍连接；B 为 Incremental首次 pending read，分别填 IMAGE_READY前/后 × 普通/system Loader的矩阵；C 为原 Session listener仍存活时，安装成功后最老欠读进入 unhealthy窗口。写出是否可仅靠 recommit恢复、最终是否失败或卸载。最后比较 XML合同、temporary bind、permanent bind和重启收养后的 health listener，说明哪些状态不能续接。

## 16. 九个完成点与第258章接口

读完整条链后，应把以下完成点分开：

1. `createSession()`返回：服务端接受 Session参数，不代表组件可绑定。
2. `addFile()`返回：文件合同已入内存，不代表 placeholder或普通文件已建立。
3. Manager bind返回 true：绑定请求已接受或 ID已存在，不代表会出现新的 BOUND。
4. BOUND：Binder连接可取，不代表 Loader `onCreate()`成功。
5. CREATED：Loader对象初始化成功，不代表开始接收缺块。
6. STARTED：供数能力已启动，不代表安装必要数据已到。
7. IMAGE_READY：Loader声称可继续安装，不代表 APK验证通过，也不代表全文件齐。
8. `streamValidateAndCommit()`返回 true、`mCommitted=true`：APK集合满足 Session规则，不代表 PMS内存提交和磁盘后处理完成。
9. 安装后半程完成：非 staged要看 PMS成功发布，staged还要区分 ready与 applied；任何一种都不证明 Incremental剩余块已装载或 Loader可退出。

最简心智模型是：DataLoader只决定“字节怎样来”，IncFS只决定“缺块怎样等待与校验”，PackageInstallerSession决定“何时具备继续验证的条件”，PMS才决定“这组包能否成为已安装事实”。状态回调连接这些层，却没有任何一个单独覆盖全链完成。r48还没有能从这条 Java→Binder fully-loaded包装链取得的正完成点：range查询固定返回 false；“不再依赖 Loader”必须由底层填充范围或数据源事实另行证明，不能由 IMAGE_READY推出。

第258章将从这里进入用户确认链：当 Session数据已经准备并完成本地验证后，`STATUS_PENDING_USER_ACTION`怎样携带确认 Intent进入 PackageInstaller应用，未知来源授权、REQUEST_INSTALL_PACKAGES/AppOps、Session owner与接受/拒绝回传如何决定安装是否真正继续。
