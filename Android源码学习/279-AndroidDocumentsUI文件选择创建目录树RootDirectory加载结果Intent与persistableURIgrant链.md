# 279 Android DocumentsUI 文件选择、创建、目录树、Root/Directory 加载、结果 Intent 与 persistable URI grant 链

## 1. 先看结论：选择器交付的是 URI 能力，不是文件路径

本文固定在 Android 11 `android-11.0.0_r48`：`frameworks/base` 位于 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/apps/DocumentsUI` 位于 `50b9994d52e58514b8cb652dac2d76e0d6100e35`。这一章追踪四个公开入口：`ACTION_OPEN_DOCUMENT`、`ACTION_CREATE_DOCUMENT`、`ACTION_GET_CONTENT` 与 `ACTION_OPEN_DOCUMENT_TREE`。它们共用 `PickActivity`，却不是四个皮肤不同的相同操作。

最重要的结论可以先压缩成一条链：调用 App 发出 Intent，DocumentsUI 把它归一成 `State`，从多个 DocumentsProvider 的 root 与 document Cursor 中组装界面，用户选择后产生带 URI 和 flags 的结果 Intent；ActivityTaskManager 再校验 DocumentsUI 是否有资格转授，并在结果投递前给调用 App 安装临时 grant；只有调用 App 随后显式 `takePersistableUriPermission()`，可持久化的读写位才进入 system_server 的持久状态。

这条链至少有七个独立完成点：

1. Intent 是否被正确归一，MIME、multiple、openable、local-only 与调用者身份是否符合预期；
2. root 是否被发现、缓存并通过 action/MIME/profile 筛选；
3. 当前目录、Recents 或全局搜索是否完成 Provider 查询和客户端过滤；
4. leaf、当前目录、已有文件或新建 document 是否通过各自选择规则；
5. DocumentsUI 是否真正走到 `RESULT_OK`，并把一个 URI 放进 `data` 或把多个 URI 放进 `ClipData`；
6. system_server 是否接受 flags、校验每个 URI 并在结果目标上安装临时 grant；
7. App 是否及时 take，且请求的 read/write 位确实由同一个 exact 或 prefix grant 完整覆盖。

后段成功不能反证前段状态。例如，root 出现在侧栏不代表当前 document 可选；结果含 `FLAG_GRANT_WRITE_URI_PERMISSION` 不代表 Provider 实现了写操作；Intent 声明 `FLAG_GRANT_PERSISTABLE_URI_PERMISSION` 也不代表权限已跨重启保存。排查时应始终问“停在哪个完成点”，而不是笼统地说“SAF 失效”。

## 2. Manifest 只建立入口；默认结果、Provider 与 system_server 各有边界

Manifest 中 `.picker.PickActivity` 是 exported、对 instant app 可见的 Activity，四个公开 action 分别有 intent-filter。OPEN、CREATE、GET 的 filter 带 `CATEGORY_OPENABLE`，TREE 没有 MIME data；这只是解析入口，不会替调用 Intent 自动补 category，也不会让所有 Provider document 都变成可打开对象。

`BaseActivity.onCreate()` 在基础初始化末尾先把结果设为 `RESULT_CANCELED`。用户返回、Activity 被结束、异步工作未回调，都会保留取消结果；对DocumentsUI自产结果，只有 `ActionHandler.onPickFinished()` 最终调用三参数测试适配层的 `setResult(RESULT_OK, intent, 0)` 才覆盖它。因此“用户点过某行”与“调用方收到成功”之间仍隔着选择校验、last-access 写入、创建 IPC、结果组装和 Activity finish。GET_CONTENT选择外部handler时则把结果责任转给外部Activity。

四方职责不能混在一起：

| 参与者 | 负责什么 | 不负责什么 |
|---|---|---|
| DocumentsUI | root/document 展示、导航、选择策略、结果 URI 与请求 flags | 不替普通 App 持久化 grant，不把 URI 变成路径 |
| DocumentsProvider | roots、document、children/search Cursor 与 open/create 等能力 | 不决定调用 App 最终是否收到 Activity 结果 |
| system_server | 校验URI转授，把临时grant绑定到接收ActivityRecord的owner，并保存/恢复已take的持久grant | 不保证 Provider 的 write/delete/create 操作真的实现 |
| 调用 App | 构造规范 Intent、消费 data/ClipData、在需要时 take、实际读写 | 不能从结果 flags 推导底层文件所有权 |

`grantUriPermissions=true`、Provider 的读写权限门与 document flags 是 Provider 侧能力面；选择器的 `State` 和 `Config` 是 UI 策略面；UriGrantsManager 中的 `UriPermission` 是调用者能力面。三层状态可能同时不同步，源码阅读必须分别取证。

### 练习 1：核对四个入口、默认取消与最终成功覆盖

在源码根目录运行；也可把源码根目录作为第一个参数从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'android:name=".picker.PickActivity"' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'android:visibleToInstantApps="true"' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'android.intent.action.OPEN_DOCUMENT' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'android.intent.action.CREATE_DOCUMENT' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'android.intent.action.GET_CONTENT' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'android.intent.action.OPEN_DOCUMENT_TREE' packages/apps/DocumentsUI/AndroidManifest.xml
grep -n -F 'setResult(AppCompatActivity.RESULT_CANCELED);' packages/apps/DocumentsUI/src/com/android/documentsui/BaseActivity.java
grep -n -F 'void finishPicking(Uri... docs)' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'mActivity.setResult(FragmentActivity.RESULT_OK, intent, 0);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'mActivity.finish();' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
```

分别推演“按返回键”“点中 leaf 但 final share check 失败”“last-access 任务尚未回调”“正常完成”四种路径的 resultCode；指出哪一行才是从默认取消跨到成功的提交点。

## 3. Intent 归一成 State：action 相同之前，MIME 与选择模式已经分叉

`PickActivity.includeState()` 先取 `intent.getType()`，为空才用 `*/*` 作为默认接受类型，再交给 `State.initAcceptMimes()`。这里有一个容易忽略的优先级：只要 Intent **存在** `EXTRA_MIME_TYPES` 这个 key，就直接采用 `getStringArrayExtra()` 的结果，而不是把数组追加到主 MIME；r48 没在这里拒绝 null 或空数组。调用者应发送非空数组，并把主 `type` 设成合理的共同上界，不能依赖选择器替它修复畸形输入。

随后 action 被映射为内部常量：

| 公开 action | 内部 action | `EXTRA_ALLOW_MULTIPLE` | `CATEGORY_OPENABLE` 参与 leaf 规则 | 常规默认位置 |
|---|---|---:|---:|---|
| OPEN_DOCUMENT | ACTION_OPEN | 读取 | 读取 | Recents |
| GET_CONTENT | ACTION_GET_CONTENT | 读取 | 读取 | Recents |
| CREATE_DOCUMENT | ACTION_CREATE | 忽略 | 读取 | 配置的默认 root，r48 默认为 Downloads |
| OPEN_DOCUMENT_TREE | ACTION_OPEN_TREE | 忽略 | 不读取 | 设备 root |

multiple 只是让 OPEN/GET 的 selection manager 可以形成多个 URI；它不会改变 CREATE 或 TREE 的完成方式。openableOnly 只在 OPEN/GET/CREATE 被记录，主要用于拒绝 virtual document；它不是对 Provider `openFile()` 成功的运行时探测。

CREATE 还有一条刻意保留的双轨：列表筛选使用 `state.acceptMimes`，但 `setupLayout()` 传给 `SaveFragment` 的新建 MIME 是原始 `intent.getType()`，保存时又从 fragment arguments 取回该值。换言之，`EXTRA_MIME_TYPES` 能改变可见/可选的已有 leaf，却不替换实际 `createDocument()` 的 MIME；主 type 为空时，新建链甚至可能把 null 继续传向 Provider。规范调用者不能只填 EXTRA 数组而省略主 type。

若用显式component绕开Manifest filter并传入未知action，映射链不会设置内部action，值保持0；后续没有可恢复位置而进入默认位置switch时会抛 `UnsupportedOperationException`。四个公开filter并不构成对显式启动输入的运行时校验。

### 练习 2：画出 action、MIME、multiple、openable 与 CREATE MIME 双轨

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'String defaultMimeType = (intent.getType() == null) ? "*/*" : intent.getType();' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.initAcceptMimes(intent, defaultMimeType);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'if (intent.hasExtra(Intent.EXTRA_MIME_TYPES)) {' packages/apps/DocumentsUI/src/com/android/documentsui/base/State.java
grep -n -F 'acceptMimes = intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES);' packages/apps/DocumentsUI/src/com/android/documentsui/base/State.java
grep -n -F 'if (Intent.ACTION_OPEN_DOCUMENT.equals(action)) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.action = ACTION_CREATE;' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.action = ACTION_GET_CONTENT;' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.action = ACTION_OPEN_TREE;' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.allowMultiple = intent.getBooleanExtra(' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'state.openableOnly = intent.hasCategory(Intent.CATEGORY_OPENABLE);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'final String mimeType = intent.getType();' packages/apps/DocumentsUI/src/com/android/documentsui/picker/PickActivity.java
grep -n -F 'final String mimeType = getArguments().getString(EXTRA_MIME_TYPE);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/SaveFragment.java
```

构造 `type=image/*` 且 `EXTRA_MIME_TYPES={image/png,image/jpeg}` 的 CREATE Intent，分别写出“已有 leaf 筛选”和“新建 document MIME”读取哪一个字段；再推演只放 EXTRA 数组、不设主 type 的风险。

## 4. 调用者身份、恢复 State 与跨 profile 是三条策略线

`BaseActivity.getState()` 只在没有 saved instance state 时创建新 `State`，读取 `EXTRA_LOCAL_ONLY`、excluded authorities、scoped-storage compat change，再调用 `includeState()`。旋转恢复时直接取 Parcelable State，不会按可能变化的 launch Intent 重新归一。更尖锐的是，r48 的 parcel字段没有写入/读回 `supportsCrossProfile`、`canShareAcrossProfile`、copy operation subtype与 `showHiddenFiles`；新State中的这些值回到默认值，`setupLayout()`也可能因supports为false而不再探测forwarder。已浏览工作profile的picker旋转后，因而可能失去跨profile交互，内部copy可丢子类型，隐藏文件显示策略也可回到false。调试重建不能把“恢复State”误写成“每个运行时策略都完整恢复”。

调用包默认来自 `Activity.getCallingPackage()`。只有真实调用者本身是 system app 或 updated system app 时，`Shared.getCallingPackageName()` 才允许非空 `Intent.EXTRA_PACKAGE_NAME` 覆盖。这个选定包同时用于 last-access key 和 `EXTRA_EXCLUDE_SELF`：后者枚举该包声明的所有 Provider authority，放进 `state.excludedAuthorities`，root 筛选阶段逐 authority 排除。它排除的是 root 提供者，不是把结果列表里“本 App 创建的文件”逐行删除。

`EXTRA_LOCAL_ONLY` 只要求 root 广告 `Root.FLAG_LOCAL_ONLY`。它是 Provider 的能力声明，不等于 POSIX 意义上的“文件就在某块本地磁盘”，也不检查网络实现是否偷偷访问远端。

Android 11 的跨 profile tabs 让 OPEN/GET/CREATE/TREE 支持 profile 切换，内部 copy destination 例外。`canShareAcrossProfile` 来自清掉component/package后的Intent能否解析到系统cross-profile forwarder，不是泛化为“某个调用App有跨用户权限”；DocumentsUI自己可枚举的profile集合又受设备能力、profile group与 `INTERACT_ACROSS_USERS` 约束。`State.canInteractWith(userId)` 本质上是当前用户或 `canShareAcrossProfile`；root 的 `supportsCrossProfile()` 也不是 Provider 新 flag，而是 DocumentsUI 根据 library、downloads、phone-storage 等 derived type 推导。普通目录的单 profile 路径会提前报告 quiet/no-permission；双 profile 文本搜索与全局聚合却可能吞掉 secondary 的 RemoteException，只显示部分结果。最终 `PickActivity.canShare()` 又只复核 `canInteractWith`，不重新查询 quiet mode。于是“标签可见”“root 可查”“leaf 可分享”并不是一个原子许可判断。

scoped-storage tree 限制由 compat change `141600225` 针对选定 calling package 计算，稍后与 document 的 `FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE` 联合决定确认按钮。它不是 root 发现开关，也不影响 OPEN_DOCUMENT 返回单个 document URI。

## 5. 初始位置有严格优先级；精确 root 定位会绕过普通 root 匹配

picker 的位置决策顺序不是“有 initial 就一定用 initial”，而是：

1. restored `DocumentStack` 已 initialized：恢复 root/directory，立即返回；
2. 内部 `ACTION_PICK_COPY_DESTINATION`：调用 `loadHomeDir()`，不恢复调用包上次位置；
3. feature 打开且 `EXTRA_INITIAL_URI` 是 root/document URI：尝试直接加载；
4. 从以 calling package 为 key 的 LastAccessedProvider 读取 stack；
5. initial异步加载失败仍先尝试last-access；该记录也不可用时才按action选择默认位置。

名称 `loadHomeDir()` 容易造成误判。r48 的 overlayable `default_root_uri` 实际是 `content://com.android.providers.downloads.documents/root/downloads`，所以默认 CREATE 与内部 copy destination 通常落在 Downloads；OEM overlay 可以改它。OPEN/GET 默认 Recents，TREE 默认 device root。`loadHomeDir()` 与 `loadDeviceRoot()` 也会经 `loadRoot()`、`LoadRootTask` 调到 `getRootOneshot()`，并不经过普通 matching-roots 筛选；精确root定位的旁路不只属于 initial URI，本文特别强调后者，是因为它由外部调用者控制。

initial root 走 `getRootOneshot()`，initial document 走 `LoadDocStackTask` 与 `findDocumentPath()`。只有 `/tree/<treeId>/document/<docId>` 这种 tree-document URI同时满足 `isDocumentUri()`，进入任务后才会取其中documentId、重建plain document URI再找完整路径；裸 `/tree/<treeId>` 只满足 `isTreeUri()`，不会被这里识别为initial document。得到的末项不是目录时会 pop 到父目录。DocumentsUI内部ArchivesProvider的URI不支持这条初始定位；原Provider中代表压缩包的普通document URI是另一回事。Provider不支持 `findDocumentPath()`、返回null或查询失败时，`onStackLoaded(null)` 会转入 `launchToDefaultLocation()`；picker在这里先异步重试last-access stack，只有该记录也不可用时才按action落到Recents、Downloads或device root。

公开DocumentsContract说明主要把 `EXTRA_INITIAL_URI` 配给OPEN、CREATE与TREE；r48实现本身没有按action挡住GET_CONTENT，所以GET也会实际尝试它，这是实现扩展而非可跨版本依赖的契约。initial识别还用当前PackageManager把URI authority与DocumentsProvider authority直接比对，随后固定传 `UserId.DEFAULT_USER`；这个常量是DocumentsUI进程 `CURRENT_USER` 的别名，并未从URI解析source user。带 `10@authority` 之类user-info的跨profile URI通常不能被识别/定位，不能靠这个extra跳到另一profile。

最关键的边界是：initial root/document 路径没有先调用 `getMatchingRoots()`。所以它能绕过 normal sidebar 对 local-only、MIME、exclude-self、EMPTY、CREATE-supports-create 的 root 筛选；精确root加载的 `onRootLoaded()` 只额外拒绝 TREE 中不支持 children 的 root。进入目录后，leaf 与当前目录仍受 `Config`、Save/PickFragment 规则约束，initial 不是自动选中，更不是授权。

`LoadDocStackTask.buildStack()` 还有一个 r48 异常路径：`getRootOneshot()` 返回 null 后，构造错误消息却解引用 `root.userId`，会先触发 NullPointerException；外层 catch 仍把它归为构建 stack 失败并回落。因此日志里的 NPE 不证明 Provider 自己抛了空指针。

last-access 恢复与 initial 不同：`DocumentStack.fromLastAccessedCursor()` 接收 `providers.getMatchingRootsBlocking(state)`，会服从普通 root 筛选。记录按 calling package 隔离；若完成时 stack root 属于非当前 profile，r48 写入的是 null stack，下次回默认位置，而不是跨 profile 自动恢复。

这里判断的是 **stack root**，不是结果leaf的user。若用户停在当前profile的合成Recents，却从跨profile聚合结果选中另一个profile的document，仍可能保存当前profile Recents stack；不能把“结果URI含user-info”直接等同于“last-access一定清空”。

### 练习 3：验证位置优先级、Downloads 默认值与 initial 绕过点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mState.stack.isInitialized()) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'if (launchHomeForCopyDestination(intent)) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'if (mFeatures.isLaunchToDocumentEnabled() && launchToInitialUri(intent)) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'initLoadLastAccessedStack();' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'Uri uri = intent.getParcelableExtra(DocumentsContract.EXTRA_INITIAL_URI);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'DocumentsContract.isRootUri(mActivity, uri)' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'DocumentsContract.buildDocumentUri(uris[0].getAuthority(), docId);' packages/apps/DocumentsUI/src/com/android/documentsui/LoadDocStackTask.java
grep -n -F 'final Path path = mDocs.findDocumentPath(docUri, mUserId);' packages/apps/DocumentsUI/src/com/android/documentsui/LoadDocStackTask.java
grep -n -F '"Failed to load root on user " + root.userId' packages/apps/DocumentsUI/src/com/android/documentsui/LoadDocStackTask.java
grep -n -F '<string name="default_root_uri" translatable="false">content://com.android.providers.downloads.documents/root/downloads</string>' packages/apps/DocumentsUI/res/values/config.xml
grep -n -F 'providers.getMatchingRootsBlocking(state), activity);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/LastAccessedStorage.java
grep -n -F 'values.put(Columns.STACK, (Byte) null);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/LastAccessedProvider.java
```

分别给出 restored stack、内部 copy、合法 initial root、失效 initial document、可恢复 last stack 五种输入，写出命中的第一条分支；再解释为何 initial 指向被 exclude-self 的 CREATE root 时，root 仍可能打开但保存按钮仍取决于当前目录能力。

## 6. ProvidersCache：发现、结构验证、两层缓存和停止态加载不是一次快照

`ProvidersCache.UpdateTask` 在 `UserIdManager.getUserIds()` 给出的profile inventory上逐一查询 `DocumentsContract.PROVIDER_INTERFACE`，并为每个user加一个DocumentsUI合成的Recents root。ProvidersCache自身没有State，也不在发现阶段调用 `canInteractWith`；交互筛选发生在后续UI/loader层。真实 Provider 必须有非空 authority，并在加载前通过四项结构检查：exported、grantUriPermissions、readPermission 为 MANAGE_DOCUMENTS、writePermission 也为 MANAGE_DOCUMENTS。通过后才注册 roots URI 的 descendant observer，并用 unstable `ContentProviderClient` 查询 roots Cursor。

这里有两层容易混称“缓存”的状态：

- `ContentResolver.getCache()/putCache()` 是长寿命 system cache。非 force 查询会优先采用它；普通 authority 返回零个 root 时不写这层缓存，MTP 与 Archives 是例外。注意“零行”不同于“返回一个带 `FLAG_EMPTY` 的 RootInfo”，后者是正常 root，照常缓存。
- `mRoots` 是 DocumentsUI 进程内这一轮 UpdateTask 的结果。即使 Provider 返回零行，本轮仍把该 authority 的零项结果提交进去；之后普通 `getMatchingRootsBlocking()` 不会仅因 system cache 为空便自动重查。要等新的 UpdateTask、roots/package/profile 等事件，或 exact `getRootOneshot()` miss 才可能再次查询。

正常 UpdateTask 跳过 `FLAG_STOPPED` 的 Provider，把 authority 记进 `mStoppedAuthorities`。名称容易误导：`getRootBlocking()`、`getRootsBlocking()`、`getMatchingRootsBlocking()` 会同步加载集合中**全部** stopped authorities；只有 `getRootsForAuthorityBlocking()` 做目标 authority 加载。`getRootOneshot()` 能直接查询一个 exact authority/root，但不等待 first-load latch，且查询时持有 `mLock`，一个慢 Binder 调用可阻住其他 cache 用户。

`waitForFirstLoad()` 的 15 秒只包住 `CountDownLatch.await()`，不是整个 Provider 查询的超时。即使 latch 超时返回，随后 `loadStoppedAuthorities()` 仍能无期限卡在某个 Provider；oneshot 本来也不走这 15 秒。因此不能把“15 秒”当 root 加载端到端 SLA。

roots observer 收到 authority URI 变化后解析 package，再启动一个 UpdateTask。该任务仍遍历所有 user/provider，只有目标 package 强制刷新，其他 authority可以复用 system cache；完成后整体替换 `mRoots`。多个 UpdateTask 都跑 THREAD_POOL，代码没有 generation 比较，因而较旧任务若最后完成，存在以旧快照覆盖新快照的竞态，这是由赋值顺序直接推得的实现风险。查询过程中异常会保留已收集的部分 roots、不写 system cache，但这些部分仍进入该任务的进程内结果。

### 练习 4：追踪 Provider 发现、空结果、stopped 集合与 15 秒边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final Intent intent = new Intent(DocumentsContract.PROVIDER_INTERFACE);' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'final List<ResolveInfo> providers = pm.queryIntentContentProviders(intent, 0);' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'if (!provider.exported) {' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'if (!provider.grantUriPermissions) {' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'android.Manifest.permission.MANAGE_DOCUMENTS.equals(provider.readPermission)' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'resolver.registerContentObserver(rootsUri, true,' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'final Bundle systemCache = resolver.getCache(rootsUri);' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'if (roots.isEmpty() && !PERMIT_EMPTY_CACHE.contains(authority)) {' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'success = mFirstLoad.await(15, TimeUnit.SECONDS);' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'for (UserAuthority userAuthority : mStoppedAuthorities) {' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'mRoots = mTaskRoots;' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
grep -n -F 'AsyncTask.THREAD_POOL_EXECUTOR' packages/apps/DocumentsUI/src/com/android/documentsui/roots/ProvidersCache.java
```

推演一个 Provider 依次经历“stopped、首次同步访问、返回零行、发 roots 通知、第二次返回两行”的状态；逐步写出 system cache、mRoots 与 mStoppedAuthorities 的变化，并标出哪一段不受 15 秒约束。

## 7. Root 筛选是 action 能力矩阵；精确定位是旁路

所有普通 sidebar、last-access 和 MultiRoot 入口最终以 `ProvidersAccess.getMatchingRoots()` 做 action 级筛选。顺序虽不是公开契约，但每个条件都值得单独检查：

| 条件 | 被排除的 root |
|---|---|
| CREATE / internal copy | 不支持 `Root.FLAG_SUPPORTS_CREATE` |
| OPEN_TREE | 不支持 `Root.FLAG_SUPPORTS_IS_CHILD`，以及合成 Recents |
| `EXTRA_LOCAL_ONLY=true` | 未广告 `FLAG_LOCAL_ONLY` |
| OPEN / GET_CONTENT | 带 `Root.FLAG_EMPTY` |
| action 不支持 cross profile | 非当前 user root |
| MIME | root 与请求 MIME 两个方向都不 overlap |
| `EXTRA_EXCLUDE_SELF=true` | authority 属于选定 calling package |

CREATE **不会**因为 `FLAG_EMPTY` 被排除；空但可创建的 root 正是合理目标。TREE 所谓 `supportsChildren()` 实际映射的是 `FLAG_SUPPORTS_IS_CHILD`，意味着 Provider承诺能验证 descendant，不能只因 queryChildDocuments 可用就显示为树授权根。上一章的 MediaDocumentsProvider 四个分类 root 没有该 flag，因此普通 TREE 侧栏不会列出它们。

MIME overlap 做双向匹配，是为了兼容 root 广告宽类型、请求具体类型，或反过来的组合；这只是 root 粒度预筛，document leaf仍会再比较实际 MIME。`FLAG_EMPTY` 同样只是一份 Provider root 快照：OPEN/GET 隐藏它，CREATE/TREE不按它过滤，不代表 Provider 查询在点击时一定仍为空或非空。

把这张矩阵和上一节精确root定位对照，才能理解一个看似矛盾的现象：某 root 不出现在侧栏，却可能被合法 initial URI或配置默认root直接打开。旁路只改变定位，不替调用 App 产生 URI grant，也不跳过进入目录后的 leaf/current-directory policy。

侧栏root被选中后也还没到children查询。非Recents路径先由 `BaseActivity.changeRoot()` 切换root，再通过 `GetRootDocumentTask`查询 `content://authority/document/<rootDocumentId>`；只有拿到有效root document才push进stack并开始目录加载，失败则以空stack刷新错误状态。该root-document查询使用null CancellationSignal，也没有first-load的15秒总时限。于是“RootInfo已发现”与“root document可进入”是两个完成点。

## 8. Recents 与 GlobalSearch 共用 MultiRoot，但失败、并发和过滤语义不同

`MultiRootDocumentsLoader` 先从 matching roots 中按 authority 分组，为每个 authority 建一个 `QueryTask`，再交给 authority executor。Semaphore 把同时运行的任务限制为普通设备 4 个、low-RAM 设备 2 个；这里限制的是 authority task，每个 task 内仍按 root 顺序查询。

首轮只等待 500ms。到点后收集已经完成的 Cursor，合并并在客户端排序，同时把 `DocumentsContract.EXTRA_LOADING` 设成 `!allDone`。迟到 task 完成时，在 first pass 已结束的条件下调用 `onContentChanged()`，触发下一轮合并。因此首屏为空或少文件可以只是部分结果，不能直接归因于 Provider 无数据。每次 raw Cursor 还会注册 observer；Provider 必须为 Cursor 设置 notification URI，后续数据变化才会唤醒。迟到完成通知与 Provider 数据通知是两条不同触发源。

不过framework对 `queryRecentDocuments()` 的契约明确不承诺change notifications。r48客户端即使给recent raw Cursor注册observer，也只能在具体Provider确实设置notification URI并发送匹配通知时获益，不能把这写成所有Recents都会实时刷新。

这套 QueryTask 把 `null` 作为 CancellationSignal 传给 `client.query()`，没有底层取消。loader reset 也只是把 close 动作排到同一个 authority executor；如果该 executor 正被挂起 query 占住，关闭会延迟。单个 root 查询异常只 log 并跳过，不会给 `DirectoryResult.exception` 赋值，所以聚合页更可能显示部分或空结果，而不是统一错误页。

Recents 的规则是：只选当前**所选 tab 的 `mUserId`**、LOCAL_ONLY、SUPPORTS_RECENTS roots；该 user 不可交互或 quiet 时才显式返回异常。每个 root 的 `RootCursorWrapper` 先最多暴露 64 行，MultiRoot随后才过滤 hidden、目录、请求 MIME 与 45 天以前的条目，最后做全局排序。因此某 root 的前 64 行大量被过滤时，不会回头补第 65 行以后；总结果也没有全局 64 上限。

GlobalSearch 只取 LOCAL_ONLY 与 SUPPORTS_SEARCH roots，并故意忽略 storage root以减少与 media roots 的重复。它向 Provider加入 `QUERY_ARG_EXCLUDE_MEDIA=true`，但 Provider是否 honour 是另一回事。默认 `getRejectMimes()` 为 null，所以它不像普通 `DirectoryLoader` 文本搜索那样统一隐藏目录。其跨profile条件要按源码原样读：当State支持跨profile、某root也被DocumentsUI判为支持跨profile、query又不含display-name时，非所选 `mUserId` 的该root被忽略；有文本条件时不触发这条排除。其余root还要经过MultiRoot的 `canInteractWith` 门，不能把这一条件简化成所有profile的一条通则。每root失败仍只是log+skip。

### 练习 5：观察 500ms 部分结果、每 root 64 条与全局搜索差异

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final int MAX_OUTSTANDING_TASK = 4;' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'private static final int MAX_OUTSTANDING_TASK_SVELTE = 2;' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'private static final int MAX_FIRST_PASS_WAIT_MILLIS = 500;' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'extras.putBoolean(DocumentsContract.EXTRA_LOADING, !allDone);' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'res[i] = client.query(uri, null, queryArgs, null);' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'if (mFirstPassDone) {' packages/apps/DocumentsUI/src/com/android/documentsui/MultiRootDocumentsLoader.java
grep -n -F 'private static final int MAX_DOCS_FROM_ROOT = 64;' packages/apps/DocumentsUI/src/com/android/documentsui/RecentsLoader.java
grep -n -F 'return !root.isLocalOnly() || !root.supportsRecents() || !mUserId.equals(root.userId);' packages/apps/DocumentsUI/src/com/android/documentsui/RecentsLoader.java
grep -n -F 'return System.currentTimeMillis() - REJECT_OLDER_THAN;' packages/apps/DocumentsUI/src/com/android/documentsui/RecentsLoader.java
grep -n -F 'if (!root.isLocalOnly() || !root.supportsSearch()) {' packages/apps/DocumentsUI/src/com/android/documentsui/GlobalSearchLoader.java
grep -n -F 'return root.isStorage();' packages/apps/DocumentsUI/src/com/android/documentsui/GlobalSearchLoader.java
grep -n -F 'queryArgs.putBoolean(DocumentsContract.QUERY_ARG_EXCLUDE_MEDIA, true);' packages/apps/DocumentsUI/src/com/android/documentsui/GlobalSearchLoader.java
```

设两个 authority：A 在 100ms 返回 10 行，B 在 800ms 返回 100 行且前 64 行有 60 个目录。分别推演 Recents 在首屏与迟到刷新后的可见数量上界；再说明同一数据放进 GlobalSearch 时为何“目录过滤”和“每 root 64 条”都不能照搬。

## 9. DirectoryLoader：同一根下的查询、过滤、排序、取消和观察要分层看

`AbstractActionHandler.LoaderBindings` 先做三分流：Recents 且未搜索用 `RecentsLoader`，Recents 且正在搜索用 `GlobalSearchLoader`，普通 root 才用 `DirectoryLoader`。最后一类再根据 search state 构造 search URI 或当前 document 的 children URI。因而“children 与 search 共用 DirectoryLoader”只对普通 root成立，不能覆盖 Recents。

DirectoryLoader 的 executor 由 `ProviderExecutor.forAuthority(mRoot.authority)` 决定，避免同 authority 的普通目录加载无序踩踏。它先把 SortModel 参数装进 Bundle，搜索时再合并 query args；只有同时满足 State支持跨 profile、当前 root支持跨 profile、query args含 display-name 时，才收集所有 `canInteractWith` user进行普通 root 的跨 profile文本搜索。否则只查询 root所属 user。

异常表现取决于 user 数量：

- 单 user 会先检查 `canInteractWith`、quiet mode 与 root document是否为空，分别放入明确的 `DirectoryResult.exception`；
- 多 user路径跳过这组统一预检，每个 user用自己的 ContentResolver查询；当前 user的 RemoteException继续抛出，secondary user的 RemoteException只记日志，剩余 Cursor仍可合并。

每次后台加载创建一个 `CancellationSignal`，同一对象传给 `ContentProviderClient.query()`；`cancelLoadInBackground()` 调用它的 `cancel()`。但 signal到达 framework `DocumentsProvider.final query()` 后，r48 只把它传给 recent callback；document、children与search分支调用的 Provider方法签名不含 signal。因此普通 DirectoryLoader 的 UI/transport 有取消请求，不等于目标 Provider 的 children/search SQL会合作中止。上一节 MultiRoot 更直接传 null，两条链不能混写。

查询成功后依次发生：在 raw Cursor注册 `LockingContentObserver`、用 `FilteringCursorWrapper`处理隐藏项、普通 search feature关闭时拒绝目录、photo picking只保留目录与图片，然后决定是否本地排序。排序判断有一个 r48 启发式缺陷：paging开启时只看 Cursor extras是否**直接含有** `QUERY_ARG_SORT_COLUMNS`，没有读取标准 `EXTRA_HONORED_ARGS`。Provider即使规范报告已处理排序，也可能被本地再排；反之只回显那个 key也可能错误跳过排序。MultiRoot则总在合并后本地排序。

observer 只有在 Provider 为 Cursor设置了匹配 notification URI后才有意义；注册本身不会监听所有数据库变化。`onStartLoading()` 重用旧 result前还会把 Cursor从头走到尾做 stale检查，代价是 O(n)，异常或无法走完整便强制重载。新结果替换时关闭旧 DirectoryResult；reset注销 observer并关闭当前结果。archive目录另有一条 client持有路径，不能由普通目录结论推导其远端生命周期。

### 练习 6：区分三种 loader、取消请求、客户端过滤与排序启发式

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'loader = new RecentsLoader(' packages/apps/DocumentsUI/src/com/android/documentsui/AbstractActionHandler.java
grep -n -F 'loader = new GlobalSearchLoader(' packages/apps/DocumentsUI/src/com/android/documentsui/AbstractActionHandler.java
grep -n -F 'return new DirectoryLoader(' packages/apps/DocumentsUI/src/com/android/documentsui/AbstractActionHandler.java
grep -n -F 'return ProviderExecutor.forAuthority(mRoot.authority);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'mSignal = new CancellationSignal();' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'Cursor c = userClient.query(mUri, /* projection= */null, queryArgs, mSignal);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'mSignal.cancel();' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'cursor.registerContentObserver(mObserver);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'cursor = new FilteringCursorWrapper(cursor, mState.showHiddenFiles);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'cursor = new FilteringCursorWrapper(cursor, null, SEARCH_REJECT_MIMES);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'cursor.getExtras().containsKey(ContentResolver.QUERY_ARG_SORT_COLUMNS)' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'cursor = mModel.sortCursor(cursor, mFileTypeLookup);' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
grep -n -F 'for (int pos = 0; pos < cursor.getCount(); ++pos) {' packages/apps/DocumentsUI/src/com/android/documentsui/DirectoryLoader.java
```

选择一个普通 root，分别推演 children、文本 search、跨 profile文本 search三个 query路径；再把 Provider设为“收到 cancellation但其 callback没有 signal参数”，说明 UI停止、Binder返回与底层SQL停止为何是三个时刻。

## 10. 用户点击不是统一的“选择”：目录、leaf、virtual、archive 与多选各有门

picker `Config.isDocumentEnabled()` 先把目录无条件标为 enabled，因为四种 action都需要导航。是否“可进入”与是否“可作为结果”随后分开：`canSelectType()` 对目录返回 false；TREE与内部 copy destination甚至让列表中的任何 item都不可直接选，用户必须进入目录后点底部确认当前目录。

leaf规则如下：

| action | leaf enabled 条件 | 点击结果 |
|---|---|---|
| OPEN / GET_CONTENT | MIME匹配；若 openableOnly 则不能是 VIRTUAL_DOCUMENT | 经过自产 leaf 的跨 profile final check后完成 |
| CREATE | 要求 `FLAG_SUPPORTS_WRITE`；若openableOnly则不能是VIRTUAL_DOCUMENT；最后MIME匹配 | 设为 replace target，等待保存/确认 |
| OPEN_TREE / internal copy | leaf不作为列表选择结果 | 当前目录由底部 PickFragment完成 |

这里没有直接拒绝 `FLAG_PARTIAL` 的分支，也没有在点击前试开文件。document flags是 Provider 对能力的声明；真实 `openFile()`、网络下载、认证或写入仍可稍后失败。`CATEGORY_OPENABLE` 只使 virtual leaf不可选，不是“所有 enabled leaf已经打开过”的证明。

`PickActivity.onDocumentPicked()` 对目录执行导航；对 OPEN/GET leaf调用 `canShare()`，它只检查每个 `DocumentInfo.userId` 是否满足 `mState.canInteractWith()`，失败时显示 action-not-allowed。这个 final gate只属于 DocumentsUI自产的 OPEN/GET单选和多选：TREE确认直接 finish，CREATE成功直接 callback，外部 GET_CONTENT handler也不会经过它。

archive是刻意的例外：在OPEN/GET中，MIME匹配的archive作为leaf可被返回，picker不把它内联打开，否则用户无法选择archive文件本身；注释同时明确不支持选择archive内部文件。这不表示每种action都可选择archive。`DocumentInfo.isContainer()` 在预览等路径可能比 `isDirectory()` 更宽，不能看到“容器”一词就假设点击一定push普通DocumentStack。

多选只存在于 OPEN/GET。selection manager交给 `onDocumentsPicked()` 的列表会逐项做相同 `canShare` 检查，再转成 URI数组；它不会把一个失败项静默丢掉后返回其余项。正常 UI会阻止空选择，但 `onPickFinished(Uri... uris)` 本身没有零长度拒绝，这个防线属于上游交互而不是结果组装函数。

## 11. CREATE 有三个提交点：创建对象、记录目录、返回 URI

CREATE 页面先用 `SaveFragment.prepareForDirectory()` 检查当前 document 的 `isCreateSupported()`，对应 `Document.FLAG_DIR_SUPPORTS_CREATE`；不支持时保存按钮禁用。任意已有leaf被点中时，它必须有 `FLAG_SUPPORTS_WRITE`、通过openableOnly/virtual门且MIME匹配，随后成为replace target，并把输入框改成该leaf名称。若用户再修改名称，replace target被清空，重新走创建链；反过来，仅手工输入一个碰巧已存在的名称不会自动识别为replace，仍调用Provider的createDocument。

真正创建由 `CreatePickedDocumentTask` 在当前 authority executor上运行：取 `stack.peek()` 为 parent，调用 `DocumentsAccess.createDocument()`；后者用 parent所属 user的 ContentResolver取得 unstable Provider client，再调用 `DocumentsContract.createDocument()`。这里没有 CancellationSignal，也没有事务把 UI、Provider对象与last-access行包在一起。

`CheckedTask` 在prepare前、后台run前和主线程finish前各检查一次旧Activity是否destroyed，但Provider IPC期间没有持续取消检查。若配置重建或返回使 owner销毁发生在 Provider IPC进行中，后台 `createDocument()` 仍可能提交，而 `onPostExecute`不再调用 finish；用户看不到结果，Provider里却已有对象。即使 Activity仍活着，创建成功后任务也先同步写 LastAccessedProvider，再返回 URI给 `onPickFinished()`；若这次记录抛 RuntimeException，child已经创建，结果回调却可能缺席。这是三个提交点非原子的直接后果。

Provider调用异常由 `DocumentsAccess.createDocument()` 捕获并变成 null。`CreatePickedDocumentTask.finish()` 对 null只显示长 Snackbar、恢复按钮状态，picker继续停留，不触发成功结果。对非 null URI则直接回调，没有重新 query document、校验 MIME或验证它真的位于parent下；Provider返回值的契约正确性仍由 Provider承担。

跨 profile创建时，另一个 user的 ContentResolver返回 URI通常不带 user-info，DocumentsAccess用 parent document URI的 encoded authority补回例如 `10@authority`，让最终授权指向正确 source user。这不是把新对象复制到当前 profile。

CREATE只创建一个 document对象，不写调用 App 的业务字节。Activity结果到达后，调用 App才用 resolver打开并写入；所以“CREATE result成功”与“文件内容写完”是两个完成点。反过来，replace也不调用 delete/create/truncate：确认后直接 `finishPicking(existingUri)`。调用 App随后以何种 mode打开、是否截断，是它自己的行为。

### 练习 7：定位 CREATE 的三个提交点、跨用户修饰与 replace边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'setSaveEnabled(cwd != null && cwd.isCreateSupported());' packages/apps/DocumentsUI/src/com/android/documentsui/picker/SaveFragment.java
grep -n -F 'new CreatePickedDocumentTask(' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F '.executeOnExecutor(getExecutorForCurrentDirectory());' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'Uri childUri = mDocs.createDocument(cwd, mMimeType, mDisplayName);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/CreatePickedDocumentTask.java
grep -n -F 'mLastAccessed.setLastAccessed(mOwner, mStack);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/CreatePickedDocumentTask.java
grep -n -F 'mCallback.accept(result);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/CreatePickedDocumentTask.java
grep -n -F 'Uri createUri = DocumentsContract.createDocument(' packages/apps/DocumentsUI/src/com/android/documentsui/DocumentsAccess.java
grep -n -F '? createUri : appendEncodedParentAuthority(parentDoc, createUri);' packages/apps/DocumentsUI/src/com/android/documentsui/DocumentsAccess.java
grep -n -F 'return null;' packages/apps/DocumentsUI/src/com/android/documentsui/DocumentsAccess.java
grep -n -F 'if (mCheck.stop()) {' packages/apps/DocumentsUI/src/com/android/documentsui/base/CheckedTask.java
grep -n -F 'finishPicking(replaceTarget.getDocumentUri());' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'mActions.finishPicking(mTarget.getDocumentUri());' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ConfirmFragment.java
```

画出 `createDocument → setLastAccessed → onPickFinished` 三节点时序，分别在每个节点后注入失败；写出 Provider对象、last-access行、resultCode三份状态。再说明 overwrite确认路径为何没有第一个节点。

## 12. OPEN_TREE 选当前目录；blocked、hidden、prefix 是三种不同限制

普通 root先要广告 `FLAG_SUPPORTS_IS_CHILD` 才进入 TREE侧栏。用户点击列表目录只是导航；`PickFragment` 的目标持续更新为当前工作目录，点“使用此文件夹”后弹 `ConfirmFragment`，由 `DocumentInfo.getTreeDocumentUri()` 构造 `/tree/<documentId>` 结果。这与 OPEN返回 `/document/<documentId>` 不同，也不是返回当前 children Cursor URI。

Android 11 scoped-storage限制由两个条件联合生效：当前 document带 `FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE`，并且 calling package的 compat restriction为 true，确认按钮才 disabled。ExternalStorageProvider的 `shouldBlockFromTree()` 对非USB存储阻止卷root、顶层 Download、顶层 Android；removable USB root明确例外。FileSystemProvider在生成目录行时把该判断变成 document flag。

这不能与“隐藏目录”混为一谈。FileSystemProvider另以正则从普通 children/search结果隐藏精确的 `Android/data`、`Android/obb`、`Android/sandbox` 路径。顶层 Android本身可以在界面中出现但被禁止作为TREE目标，三个敏感子目录则可能根本不在普通列表；blocked selection与hidden discovery是两层机制。

TREE结果请求 READ、WRITE、PERSISTABLE、PREFIX。system_server中的 prefix匹配只是 `Uri.isPathPrefixMatch()`：比较 scheme、authority和原子path segments，不理解 Provider内部 documentId层级。后续拿 tree URI访问 `/tree/<rootId>/document/<childId>` 时，DocumentsProvider的 `enforceTree()` 再调用 `isChildDocument(parent, child)`。因此安全性依赖两个门：URI grant命中和Provider descendant验证；`FLAG_SUPPORTS_IS_CHILD` 正是 Provider对第二个门的承诺。

TREE并非 `MANAGE_EXTERNAL_STORAGE`。它只给目标 App相应 tree URI范围内、Provider认可后代的能力；Provider仍可按 document flags或方法实现拒绝write/delete/create。TREE确认也没有 OPEN/GET自产 leaf的 `canShare()` final loop，跨 profile可用性依赖此前UI状态与后续system_server检查，不能把那条 final check泛化到这里。

## 13. 结果 Intent：data、ClipData、flags、last-access 与外部 handler 必须一起读

DocumentsUI自产结果先构造空 Intent。恰好一个 URI只放 `intent.data`；多于一个才创建 `ClipData`，其 MIME description取 `mState.acceptMimes`，每个 item只放 URI，data保持 null。零个 URI时两者都为空，但函数仍会继续统计、加flags并设 `RESULT_OK`；正常选择UI承担“不以空数组调用”的上游约束。§3所述畸形 `EXTRA_MIME_TYPES=null` 在单选data路径未必当场暴露，多选却会把null数组交给 `ClipDescription` 构造器并立即抛出 `NullPointerException`；空数组与null不是同一条失败边。

flags矩阵是：

| 公开 action / 内部 copy 分支 | DocumentsUI自产结果 flags |
|---|---|
| GET_CONTENT | READ |
| OPEN_DOCUMENT | READ、WRITE、PERSISTABLE |
| CREATE_DOCUMENT | READ、WRITE、PERSISTABLE |
| OPEN_DOCUMENT_TREE | READ、WRITE、PERSISTABLE、PREFIX |
| internal copy destination | data仍携目标URI；不加URI grant flags，另附DocumentStack和operation type extras |

这些是向system_server请求授予的模式，不是“每个操作已验证可用”。尤其OPEN也请求WRITE，但被选leaf可能没有 Provider写能力；调用 App应按自己的操作、结果 flags和Provider行为处理失败，不能无条件以 `rw` 打开。

除CREATE任务自己写last-access外，普通 `finishPicking()` 先启动 `SetLastAccessedStackTask`，等它在当前 authority executor写完后才调用 `onPickFinished()`。于是用户点击与结果交付之间存在数据库关键路径；旧 Activity被销毁或写入异常，都可能保留默认取消。TREE和replace也走这条路径。导航、Back与取消本身不写last-access。

GET_CONTENT还有一条完全不同的“其他 App”路径。RootsFragment可以展示可处理原 Intent的外部 Activity；用户选择后，ActionHandler复制原 Intent，清除 READ、WRITE、PERSISTABLE、PREFIX四类URI flags，加入 `FLAG_ACTIVITY_FORWARD_RESULT` 和 `FLAG_ACTIVITY_PREVIOUS_IS_TOP`，再启动外部组件并结束DocumentsUI。外部 Activity直接成为原调用者的结果生产者：DocumentsUI不再组装data/ClipData、不做自产结果的read-only限制、不写普通finishPicking的last-access，也不审查外部最终URI。故“GET只返回READ”必须限定为DocumentsUI自产结果。

### 练习 8：验证结果容器、四类URI grant flags与 GET_CONTENT 转发

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (uris.length == 1) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'intent.setData(uris[0]);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F '} else if (uris.length > 1) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'intent.setClipData(clipData);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'if (mState.action == ACTION_GET_CONTENT) {' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F '| Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'intent.putExtra(Shared.EXTRA_STACK, (Parcelable) mState.stack);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'new SetLastAccessedStackTask(' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'final int flagsRemoved = Intent.FLAG_GRANT_READ_URI_PERMISSION' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'intent.setFlags(intent.getFlags() & ~flagsRemoved);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'intent.addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
grep -n -F 'mActivity.startActivity(intent);' packages/apps/DocumentsUI/src/com/android/documentsui/picker/ActionHandler.java
```

分别给 `onPickFinished` 传0、1、2个URI，记录data、ClipData与resultCode；再比较DocumentsUI自产GET和外部handler GET，指出谁决定最终flags、谁负责last-access、谁接受system_server的转授校验。

## 14. ActivityTaskManager 在结果投递前安装临时 grant，但不验证业务操作

DocumentsUI本身不调用 `grantUriPermission()`。当它 `finish()` 时，`ActivityTaskManagerService.finishActivity()` 先拒绝结果 Intent携带file descriptors，解析目标 `r.resultTo`，随后在不持全局锁时调用 `collectGrants(resultData, target)`。UriGrantsManager递归检查顶层 `data`、`ClipData`每个item的 URI；item没有URI但含嵌套Intent时继续递归，并沿用顶层result Intent的mode flags，而不是改用嵌套Intent自己的flags。普通 extras不会被扫描，所以 `EXTRA_INITIAL_URI` 和internal copy的 stack extras不会凭空变成grant。

检查使用finish调用者UID，也就是DocumentsUI进程身份，解析目标UID与source user，并读取Provider元数据：`exported`、顶层权限与 `<path-permission>` 参与判断目标或转授者是否已直接持权，`grantUriPermissions` 与 `<grant-uri-permission>` 生成的 `uriPermissionPatterns` 决定该URI是否允许转授；`exported=false` 本身不是URI grant的绝对否决。非content URI、找不到Provider，或目标对basic grant已能直接访问时，检查通常返回“不需新增grant”，结果仍可能交付；除basic cross-user特例外，Provider禁止转授，或DocumentsUI既无直接权限也无足够强度的既有URI grant，才会抛出 `SecurityException` 中止正常finish路径。任何URI能进入结果字符串，都不等于调用方最终获得了可用能力。

`ActivityRecord.finishActivityResults()` 的顺序很关键：先调用 `grantUriPermissionUncheckedFromIntent(resultGrants, resultTo.getUriPermissionsLocked())`，再 `resultTo.addResultLocked(...)`。也就是说，正常路径在Activity结果排队给调用方之前已把grant绑定到接收ActivityRecord的 `UriPermissionOwner`。这解释了调用方回调一到即可用URI，而不是回调后另有一个竞态授权窗口。

这份 owned grant仍是临时能力。接收ActivityRecord离开history时，其 owner会被移除并撤相应模式；进程/Activity生命周期与具体owner关系将在下一章细拆。若结果提供PERSISTABLE，调用 App可在临时grant仍有效时显式take，把允许的位提升为持久状态；单纯保存URI字符串没有这种效果。

结果 flags只决定授权模式候选，system_server不调用 `queryDocument()` 检查 `Document.COLUMN_FLAGS`，也不试执行 `openFile/create/delete`。所以权限链成功只回答“这个UID能否尝试该URI的read/write”，不回答对象存在、网络在线、Provider无bug或具体操作受支持。

对TREE还要再区分 grant匹配与Provider校验：UriGrantsManager按原子URI path segment做prefix命中；真正调用DocumentsProvider时，`enforceTree()` 对不同parent/child documentId调用 `isChildDocument()`。前者不是文件路径递归算法，后者也不能弥补一个被授错authority/source-user的grant。

## 15. takePersistableUriPermission：exact/prefix 不拼权限，重复 take 也未必写盘

调用 App通常从结果 flags中只保留 READ/WRITE：

`val takeFlags = result.flags and (FLAG_GRANT_READ_URI_PERMISSION or FLAG_GRANT_WRITE_URI_PERMISSION)`

随后对需要跨重启保存的OPEN/CREATE/TREE结果调用 `ContentResolver.takePersistableUriPermission(uri, takeFlags)`。DocumentsUI自产GET结果本身不新增PERSISTABLE offer；若该UID事先没有同一规范URI与source user下可持久化的exact/prefix permission，随后以READ/WRITE take会抛 `SecurityException`。

ContentResolver先从可能带 `10@authority` 的URI解析source user，再移除embedded user-info，把普通URI与userId分别交给UriGrantsManager。对跨profile URI，调用者不能先删掉 `10@` 再take；否则 `resolveUserId()` 退回resolver所属user，通常与原grant的source user不同而查找失败。isolated进程禁止take；公开入口只接受READ/WRITE两位，夹带PREFIX、PERSISTABLE或其他flag会在参数检查时报错。

服务在目标UID的map中分别找同URI的exact key和prefix key。这里的prefix lookup仍用传入URI构造带prefix位的**同URI key**，不会像一般访问检查那样遍历祖先prefix grant；TREE应对返回的 `/tree/<id>` 本身take，不能拿后代 `/tree/.../document/...` 代替。请求的所有read/write位必须被某一个候选的 `persistableModeFlags` 完整覆盖，`exact只读 + prefix只写`不会拼成一次READ|WRITE成功。如果exact和prefix各自都完整覆盖，r48会同时对两份permission执行take，而不是只选一个。

`UriPermission.takePersistableModes()` 只把已offer的交集OR进 `persistedModeFlags`。只要持久位非零，它每次都会把 `persistedCreateTime` 更新为当前时间；返回值却只表示mode bits是否变化。于是重复take相同mode会更新内存时间，但如果没有mode变化、也没有prune，服务不会调用schedule，新的时间可能在重启前从未写到磁盘。不能把“touch时间”直接等同于“立即刷新持久文件”。

每UID最多保留512份persisted grants。`maybePrunePersistedUriGrantsLocked()` 先以整个permission map大小做快捷判断，真正收集的却只有 `persistedModeFlags != 0` 的项目；超过512才按时间排序、释放最旧项。map很大只会更早进入扫描，不会把未持久临时grant算进512或误删它们。mode变化或真实prune发生后，服务会在没有待处理写消息时排一个10秒后的任务；窗口内后续变更合并写入且不重置期限，最终使用AtomicFile。system_server若在这段延迟窗口内重启或异常终止，最新变更尚未落盘。

`releasePersistableUriPermission()` 只清持久部分，文档明确其他non-persistent grant保留；它检查传入同URI的exact/prefix key，不遍历祖先prefix。`getPersistedUriPermissions()`只返回授给调用包的incoming且已taken项目，不等于当前所有临时URI能力，也不含该包作为Provider发出的outgoing列表。持久记录跨重启恢复时会核对authority对应Provider仍属于source package，并确认target package仍能解析出UID；恢复后的实际使用仍可能因user未解锁、Provider或包移除、对象删除或Provider运行时拒绝而失败。

### 练习 9：验证 user-info、exact/prefix、512上限与10秒写盘

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'ContentProvider.getUriWithoutUserId(uri), modeFlags, /* toPackage= */ null,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'resolveUserId(uri));' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'enforceNotIsolatedCaller("takePersistableUriPermission");' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'Preconditions.checkFlagsArgument(modeFlags,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'UriPermission exactPerm = findUriPermissionLocked(uid,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'UriPermission prefixPerm = findUriPermissionLocked(uid,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (!(exactValid || prefixValid)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'persistedModeFlags |= (persistableModeFlags & modeFlags);' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'persistedCreateTime = System.currentTimeMillis();' frameworks/base/services/core/java/com/android/server/uri/UriPermission.java
grep -n -F 'private static final int MAX_PERSISTED_URI_GRANTS = 512;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (perm.persistedModeFlags != 0) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mH.sendMessageDelayed(mH.obtainMessage(PERSIST_URI_GRANTS_MSG),' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '10 * DateUtils.SECOND_IN_MILLIS);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mGrantFile.startWrite(startTime);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'mPackageName, true /* incoming */, true /* persistedOnly */' frameworks/base/core/java/android/content/ContentResolver.java
```

构造exact只读、prefix只写两份offer，证明一次READ|WRITE为何失败；再令两者都提供READ|WRITE，推演服务会修改几份permission。最后比较首次take、重复相同take、超过512触发prune三种情况下的内存时间、schedule与磁盘状态。

## 16. 用完成点矩阵排错，并把 grant owner 深入留给下一章

遇到“选择器里看不见、点不了、返回失败、重启失效”，可按下面的最短矩阵定位：

| 症状 | 第一检查点 | 关键反例 |
|---|---|---|
| root不出现 | ProvidersCache结构门、system/in-process cache、matching roots | initial URI仍可能直接打开；`FLAG_EMPTY`不是零个root |
| 进入root却无内容 | root document query、Directory/Recents/GlobalSearch分流、Provider Cursor | 500ms首屏可为partial；聚合异常可只表现为空 |
| Recents少结果 | 每root先截64、45天窗口、目录/MIME/hidden过滤 | 截断发生在过滤前，合并后没有全局64上限 |
| 搜索少结果 | 普通root还是GlobalSearch、display-name跨profile条件、root能力与storage排除 | GlobalSearch无每root64限制且不统一滤目录，secondary失败可被吞 |
| leaf灰掉 | MIME、VIRTUAL+OPENABLE、CREATE的SUPPORTS_WRITE | enabled不保证真实open/write成功 |
| TREE按钮灰掉 | compat restriction与BLOCKS flag | hidden敏感目录与blocked当前目录不是同一规则 |
| CREATE后无回调 | Provider create、last-access写入、旧Activity是否destroyed | 对象可能已创建；replace根本不创建或截断 |
| 收到RESULT_OK却URI打不开 | 自产还是外部GET结果、data/ClipData flags、owner存活、source user与Provider对象 | 正常自产结果在投递前已过ATMS检查，但flags不验证Provider业务方法 |
| 重启后失效 | 是否offer PERSISTABLE、是否及时take、请求mode覆盖 | URI字符串、重复take内存touch都不是磁盘提交证明 |

再补三个容易被日志误导的边界。第一，普通root点击后还要查询root document：`BaseActivity.changeRoot()` 之后的 `GetRootDocumentTask` 用 document URI取真实行，成功才push/open；root出现在侧栏不代表这一步必成，且该查询没有CancellationSignal。第二，roots observer与directory Cursor observer是两条刷新链；前者更新package snapshot并广播侧栏，后者依赖Cursor notification URI重启内容loader。第三，RootsMonitor判断当前root是否还存在时用不编码user的root URI，另一profile同authority/rootId可能掩盖当前profile root消失；不要把它当跨profile状态的强一致仲裁器。

本章的核心不是背四组flags，而是守住分层：Intent/State决定候选，Provider Cursor决定可见快照，Config决定UI可选性，Provider方法决定对象操作，ActivityTaskManager决定临时能力，显式take才决定持久位。任一层的“支持”都不能替下一层作保证。

下一章进入 `UriGrantsManagerService` 内部，继续追 `GrantUri`、`UriPermissionOwner`、Activity与ClipData递归授权、prefix匹配、跨用户校验、撤销以及持久文件恢复，把本章停在“结果已交付/调用方已take”的边界继续向system_server收束。
