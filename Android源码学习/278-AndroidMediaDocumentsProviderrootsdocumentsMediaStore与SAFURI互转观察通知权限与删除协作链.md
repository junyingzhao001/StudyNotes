# 278 Android MediaDocumentsProvider roots/documents、MediaStore与SAF URI互转、观察通知、权限与删除协作链

## 1. 先看结论：这是分类索引、能力委托和跨Provider标识转换三条链

本文固定在 Android 11 android-11.0.0_r48：frameworks/base 位于 1d9b9ab57d844b18b3b1b4297725141e7788109b，packages/providers/MediaProvider 位于 47c141d93e93b25cc85c36f3579fda25a1695952。MediaDocumentsProvider 没有自己的媒体数据库，也不是公开 MediaStore.getDocumentUri()/getMediaUri() 的转换终点。它同时参与三件容易混淆的事：

1. 把 MediaStore 当前 external 视图翻译成 Images、Videos、Audio、Documents 四个 SAF root；
2. 在 DocumentsProvider 已检查组件权限或具体 URI grant 后，以 Provider self 身份代查、代开、代删底层 MediaStore 对象；
3. 在 row 插入、删除和底层 collection 通知之后，协助刷新 roots、撤分类 document grant。

同一张图片可能同时有三种 URI：

| 命名空间 | 示例 | 表达的关系 |
|---|---|---|
| MediaStore | content://media/external/images/media/123 | external 合成查询域中的媒体 row |
| 媒体分类 SAF | content://com.android.providers.media.documents/document/image%3A123 | type:id 形式的分类 leaf |
| 外部存储 SAF | content://com.android.externalstorage.documents/document/primary%3ADCIM%2Fa.jpg | root:path 形式的真实文件树节点 |

公开互转 API 连接的是第一种与第三种，不会返回第二种。即便三者暂时指向同一文件，row、物理路径、Cursor 快照、root EMPTY 标志和 URI grant 也仍是五份独立状态。

本文用六个完成点排查：incoming Documents URI 的权限门、docId 解析、MediaStore 查询、MatrixCursor/FD 返回、通知与 grant 回收、跨 Provider 路径互转。任一后段成功，都不能反证前面所有状态仍新鲜。

## 2. Manifest先建立组件门；ready和shell限制又是两种不同状态

MediaDocumentsProvider 与 MediaProvider 位于同一 APK，未另指定 process；前者的 authority 是 com.android.providers.media.documents，后者是 media。它们仍通过 ContentResolver 的 Provider 接口协作，而不是让分类 Provider 直接操作 external.db。

Manifest 把分类 Provider 声明为 exported、grantUriPermissions=true，并用 MANAGE_DOCUMENTS 同时保护读写。DocumentsProvider.attachInfo() 还在运行时重复验证 exported、grantUriPermissions、readPermission 和 writePermission；配置不符会在 attach 阶段抛 SecurityException。MANAGE_DOCUMENTS 让 DocumentsUI 等受信调用者枚举 roots，grantUriPermissions 则允许普通 App 在用户选择后持有窄 document capability。

sMediaStoreReady 是进程级 volatile boolean。queryRoots() 在它为 false 时只返回空 MatrixCursor，避免启动期触碰尚未准备好的 MediaStore；MediaProvider 把某个 external 卷的准备任务执行到 onMediaStoreReady() 后，将这个全局位设为 true 并通知 roots。volumeName 参数没有参与判断，detach 也不清零，所以它只表示“本进程至少完成过一次准备”，不是每卷 ready 表。这个位只挡 queryRoots()；知道docId而直接调用document、children、recent或search入口时，子类不再检查它。

roots 通知有两个独立触发源：MediaDocumentsProvider.onCreate() 发一次，某个卷的准备任务到达ready回调又发一次。后者跑在异步执行链上，不能把观察到的两次通知硬编号为“onCreate一定先、ready一定后”；任一次通知也都不是所有external卷已准备完成的屏障。

detach不清ready还有一个失败边界：synthetic external 在数据库路由时先落到 external_primary 并检查attached set。若primary也已detach，后续queryRoots()里的COUNT可能抛IllegalArgumentException，并不保证稳定返回四个带EMPTY的root。

多数 document 操作还调用 enforceShellRestrictions()：只有 caller appId 为 SHELL 且当前用户启用 DISALLOW_USB_FILE_TRANSFER 时才拒绝。queryRoots() 没有调用这个 helper。它是 shell/user restriction，不替代普通 read/write 或 URI grant 检查。

### 练习 1：核对两个authority、Manifest四门、全局ready与shell例外

在源码根目录运行；也可把源码根目录作为第一个参数，从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'android:name="com.android.providers.media.MediaDocumentsProvider"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'android:authorities="com.android.providers.media.documents"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'android:permission="android.permission.MANAGE_DOCUMENTS"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'android:grantUriPermissions="true"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'if (!info.exported) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'if (!info.grantUriPermissions) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'Manifest.permission.MANAGE_DOCUMENTS.equals(info.readPermission)' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'private static volatile boolean sMediaStoreReady = false;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static void onMediaStoreReady(Context context, String volumeName) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'sMediaStoreReady = true;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (sMediaStoreReady) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'resolveVolumeName(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!mAttachedVolumeNames.contains(volumeName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private void enforceShellRestrictions() {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'UserManager.DISALLOW_USB_FILE_TRANSFER' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

分别推演“进程刚启动”“primary准备完成”“随后secondary才完成”“全部卷detach”四个时刻的 sMediaStoreReady；说明为什么它不能回答某个具体卷是否可查询，以及最后一种状态为何可能在COUNT阶段失败。

## 3. 四个root是能力广告；EMPTY只是一次COUNT与四个进程内记忆位

Images、Videos、Documents root 广告 LOCAL_ONLY、SUPPORTS_RECENTS、SUPPORTS_SEARCH；Audio 没有 RECENTS。四者都不广告 CREATE 或 IS_CHILD。MIME 分别是 image/*、video/*、audio/* 加 application/ogg 与 application/x-flac，以及 Documents 的 */*。root ID 同时也是顶层 document ID。

每次 include root 都以 self 身份对对应 synthetic external collection 查询 COUNT(_id)。结果为零时加 Root.FLAG_EMPTY，并把 sReturnedImagesEmpty 等静态位设为 true。这些 boolean 不是精确计数器，而是“本进程曾报告空、且尚未被对应insert回调清除”的粗提示；后续非空queryRoots()不会主动把旧位改回false。

onMediaStoreInsert() 只处理非 internal row；只有匹配 media_type 且对应 sReturned*Empty 已为 true，才清位并 notify roots。因此从未有人见过 EMPTY 时，首个 insert 不需要额外 roots 通知。files trigger 在 schema write lock 由当前线程持有时会抑制 listener；若这种路径漏掉 insert 回调，后续 root query 本身仍会重新 COUNT 并显示内容，但非空分支不会主动清旧 boolean，下一次普通 insert 可能多发一次通知。

onMediaStoreDelete() 对 external image/video/audio/document leaf 撤分类 document URI grant，并且每删一项都 notify roots，而不是只在最后一项删除时通知。下一次 queryRoots 才重新 COUNT 并决定 EMPTY。internal row 与 MEDIA_TYPE_NONE 等未列类型不进入这段分类回调。

roots刷新并不覆盖每种可见性变化：ready、命中旧EMPTY位的insert、上述四种delete会通知roots；普通UPDATE、pending/trashed切换、media_type改类和detach没有对应的roots-authority通知。客户端缓存的FLAG_EMPTY因此可以滞后，主动重查时才重新COUNT。

Documents root 有一个 r48 不一致：isEmpty(Files.EXTERNAL_CONTENT_URI) 统计当前external卷中、默认排除pending/trashed后的整个Files collection，包括非document媒体与目录row；children、recent和search却追加MEDIA_TYPE_DOCUMENT。因此“库里只有图片”时 Documents root 仍可能不带EMPTY，点进去却没有document leaf。首次准备还会在ready前尝试创建默认目录并插入目录row，所以即使没有媒体文件，这个root也常常已不是EMPTY。

Audio 也有另一种口径分叉：EMPTY 和 search 直接查 Audio.Media，而 root children 先查内含 `is_music=1` 的 audio_artists view。只有录音、播客等非 music audio 时，Audio root 可不带 EMPTY、search 也可命中，导航的artist列表却为空。album children 又只按 album_id 查 Audio.Media，没有再加 `is_music=1`，共用album_id的非music row还可能混入已发现的album。

### 练习 2：验证root flags、四个EMPTY记忆位与Documents计数错位

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final String IMAGE_MIME_TYPES = joinNewline("image/*");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F '"audio/*", "application/ogg", "application/x-flac");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'private static final String DOCUMENT_MIME_TYPES = joinNewline("*/*");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_RECENTS | Root.FLAG_SUPPORTS_SEARCH' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'int flags = Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_SEARCH;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'private static volatile boolean sReturnedImagesEmpty = false;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'new String[] { "COUNT(_id)" }, null, null, null' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (isEmpty(Files.EXTERNAL_CONTENT_URI)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static void onMediaStoreInsert(Context context, String volumeName, int type, long id) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (MediaStore.VOLUME_INTERNAL.equals(volumeName)) return;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static void onMediaStoreDelete(Context context, String volumeName, int type, long id) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'private static void notifyRootsChanged(Context context) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'retSelection += FileColumns.MEDIA_TYPE + "=?";' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'retSelectionArgs.add("" + FileColumns.MEDIA_TYPE_DOCUMENT);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'cursor = resolver.query(Audio.Artists.EXTERNAL_CONTENT_URI,' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'WHERE is_music=1 AND volume_name IN ' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'SongQuery.PROJECTION, AudioColumns.ALBUM_ID + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

构造只含一张 external 图片、没有 MEDIA_TYPE_DOCUMENT 的数据库，分别推导四个 root 的 EMPTY；再删除这张图片，写出 roots 通知与下一次 COUNT 的关系。

## 4. type:id描述虚拟分类，不是稳定文件路径；bucket还是32位路径hash

getIdentForDocId() 只在第一个冒号处分割。没有冒号的 root 得到 id=-1；有冒号则用 Long.parseLong() 解析全部后缀。非法数字抛 NumberFormatException，未知 type 抛 UnsupportedOperationException；它们不统一表现为“空文档”。

分类层级如下：

| root | 虚拟中间节点 | leaf | leaf底层collection |
|---|---|---|---|
| images_root | images_bucket:<bucketId> | image:<rowId> | Images external |
| videos_root | videos_bucket:<bucketId> | video:<rowId> | Video external |
| audio_root | artist:<artistId> → album:<albumId> | audio:<rowId> | Audio external |
| documents_root | documents_bucket:<bucketId> | document:<rowId> | 发现链为Files external + MEDIA_TYPE_DOCUMENT |

root、bucket、artist、album 是 SQL 投影出来的目录语义，不是可写的物理目录 row。DocumentsProvider 允许同一 document 出现在多个父视图；album docId 只带 albumId，即使它从不同 artist 查询路径中出现，也没有把父 artist 编入 ID。

四类 leaf 都指向 synthetic external URI。rowId 在共享 external.db 中唯一，而查询 builder 再按当前 external volume 集合过滤；docId 没有保存具体 volume。卷detach后，同一个 leaf ID可以仍存在于数据库却暂时查不到；重新attach后又可能出现。

MEDIA_TYPE_DOCUMENT 只在documents root的children/recent/search发现链追加。queryDocument("document:<id>")只对 Files external 做 `_id=?`，getUriForDocumentId()也直接构造Files item，都不重验media_type。因此人为构造的`document:<image-id>`是同一row的generic alias：拿到这个**精确alias URI**的grant后，query/open/delete可作用到真实Files row。普通发现链不会生成这种alias，caller也不能只凭猜id创造AMS grant；这是type→row不受约束的别名边界，不是普遍无权读。

BUCKET_ID 来自小写绝对父路径的 Java String.hashCode()，是32位路径派生值，不是目录 row 主键。bucket docId 又不带 volume 或原路径，查询只按 BUCKET_ID；两个不同目录一旦hash碰撞，分类视图会把它们合并。列表按 BUCKET_ID、DATE_MODIFIED DESC 排序，Java 只在 id 变化时输出一行，所以代表名称、volume 和mtime取该hash组最新row。正常的“不同卷同相对路径”因绝对父路径不同通常不会天然相等，但仍有普通32位hash碰撞风险。

### 练习 3：从docId追到external collection，并证明bucket不是目录主键

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final int split = docId.indexOf(' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ident.id = Long.parseLong(docId.substring(split + 1));' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'return type + ":" + id;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static final String TYPE_IMAGES_BUCKET = "images_bucket";' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static final String TYPE_ARTIST = "artist";' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'static final String TYPE_ALBUM = "album";' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'return ContentUris.withAppendedId(' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Images.Media.EXTERNAL_CONTENT_URI, ident.id);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Files.EXTERNAL_CONTENT_URI, DocumentQuery.PROJECTION,' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'FileColumns._ID + "=?", queryArgs, null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'values.put(MediaColumns.BUCKET_ID, parent.hashCode());' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'final String SORT_ORDER = ImageColumns.BUCKET_ID + ", " + ImageColumns.DATE_MODIFIED' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (lastId != id) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ImageColumns.BUCKET_ID + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Artists.Albums.getContentUri("external", ident.id)' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

任选两个不同绝对父路径，说明只有hash相等才会被同一 images_bucket 合并；再指出此时bucket显示名和缩略图代表row由哪条排序决定。

## 5. query复制成MatrixCursor；通知只要求重查，sort和missing都有边界

queryDocument() 与 queryChildDocuments() 先过 DocumentsProvider/ContentProvider 外层读门，进入子类后检查 shell，再 clear Binder identity，以 Provider self 查询 MediaStore。这样具体 SAF grant 能被安全地转换成底层代读能力；self 不是外部 caller 的 row owner，也不会把 MediaStore grant反向授予 caller。

queryDocument 对 root 直接合成一行；bucket、artist、album、leaf 以 _id、bucket_id、album_id 等条件查询。其中`document:<id>`只按Files `_id` 查，没有MEDIA_TYPE_DOCUMENT条件，因而会呈现上节的generic alias。格式合法但row已消失时，底层 cursor 的 moveToFirst() 为false，方法返回零行 MatrixCursor，而不是主动抛 FileNotFoundException。相反，DocumentsProvider dispatcher 捕获子类抛出的 FileNotFoundException 时会记录并返回 null；NumberFormatException 与 UnsupportedOperationException 不在这个catch里。调用者不能把 null、零行和异常当成同一种not-found。

DocumentsProvider先把Bundle中的SQL/结构化排序参数折成sortOrder再调用子类；MediaDocumentsProvider虽然收到sortOrder，却完全不用它，LIMIT等其他child参数也不会应用或报告honored。images/videos/documents root采用固定bucket-id + mtime顺序去重，bucket内leaf与audio层级多传null sort；所以调用者排序对这里没有效果。transport收到的CancellationSignal也没有传进document、children或search子类签名。

子类把底层 row 复制到独立 MatrixCursor。DATE_MODIFIED 从MediaStore秒乘1000变成DocumentsContract毫秒，projection为null时使用六列默认document projection。复制完成后，旧MatrixCursor不会随数据库变化自动改行。

对发生底层查询的结果，MatrixCursor notification URI被设为Images、Video、Audio或Files external collection。MediaProvider普通row listener主要通知具体卷与synthetic external的扩展item URI；客户端对collection注册的descendant observer可以因此被唤醒。notifyChange只表示“需要重新query”，不是把新行推入现有Cursor。root document本身没有collection notification URI；ready/EMPTY变化走独立roots URI通知。

### 练习 4：区分零行、null、异常、固定排序与notification URI

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public Cursor queryDocument(String docId, String[] projection)' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'final MatrixCursor result = new MatrixCursor(resolveDocumentProjection(projection));' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'final long token = Binder.clearCallingIdentity();' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (cursor.moveToFirst()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'result.setNotificationUri(resolver, Images.Media.EXTERNAL_CONTENT_URI);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'public Cursor queryChildDocuments(String docId, String[] projection, String sortOrder)' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ImageQuery.PROJECTION, ImageColumns.BUCKET_ID + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'return queryChildDocuments(getDocumentId(uri), projection, queryArgs);' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'parentDocumentId, projection, getSortClause(queryArgs));' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'catch (FileNotFoundException e) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'return null;' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'cursor.getLong(ImageQuery.DATE_MODIFIED) * DateUtils.SECOND_IN_MILLIS' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

对 image:999 不存在、image:not-a-number、unknown:1 三个docId分别推演结果；再说明为何收到collection通知后继续读旧MatrixCursor不会看到新数据。

## 6. recent只截复制数量；search的MIME预筛、LIKE与honored并非完整契约

queryRecentDocuments() 只支持 Images、Videos、Documents；Audio root没有 RECENTS flag，强行调用会抛UnsupportedOperationException。默认最多复制64行，显式 QUERY_ARG_LIMIT 才记入 EXTRA_HONORED_ARGS。limit没有下推SQL，底层resolver.query仍按 DATE_MODIFIED DESC 发起无SQL LIMIT的查询，再靠 result.getCount()<limit 停止复制；SQLite Cursor可以惰性步进，不能据此断言全量结果已经物化。

recent虽然收到CancellationSignal，却调用不带signal的resolver.query重载。已取消的signal仍可能在客户端发IPC前被ContentResolver拦截；一旦调用到达DocumentsProvider，document、child、search分支不下传signal，recent子类也不检查或下传，所以不能把上层取消理解成底层SQL已经中止。

search没有limit，也没有CancellationSignal参数；它把全部匹配row复制进MatrixCursor。四个广告参数是display name、size over、last modified after与MIME types：

- display name用 LIKE %输入%，没有escape百分号或下划线，所以输入仍带SQL LIKE通配语义；
- size与mtime都是严格大于，mtime毫秒先除1000再直接拼进selection，亚秒精度丢失；
- wildcard MIME变成 LIKE type/%，具体MIME进入参数化IN，两组以OR组合；
- Documents root最后再与 MEDIA_TYPE_DOCUMENT 做AND，所以MIME参数为null或具体类型时不会纳入图片、视频或音频row。

上一句只适用MIME参数为null或具体类型的正常路径。Documents root收到显式`*/*`时没有特殊处理：buildSearchSelection()把任何以`/*`结尾的值都改成LIKE，于是`*/*`变成`mime_type LIKE '*/%'`，通常把本应通配的搜索清空。

Images/Video/Audio先用 shouldFilterMimeType() 判断请求是否与固定 image/*、video/*、audio/* 相容。Audio root虽广告 application/ogg 和 application/x-flac，但MIME数组若只含前者或只含后者，都不匹配audio/*预筛，必定直接得到空结果且不query collection；若数组同时含audio/*或*/*，则改走无MIME限制路径。

search固定按DATE_MODIFIED DESC返回，忽略调用者排序，也没有实现父类建议的relevance次序。getHandledQueryArguments()对五种键都只检查“是否存在”：空displayName、-1 size/time或null MIME即使没有形成有效predicate也可被报honored；QUERY_ARG_EXCLUDE_MEDIA更彻底——搜索从未读取其值，root的COLUMN_QUERY_ARGS也没有广告它。这是“响应声明已处理”和实际selection分叉的r48边界。

通知也有两个窄边界：MIME预筛直接跳过底层query时，空search MatrixCursor没有notification URI；recent实现虽设置了collection notification URI，父类契约却明确说recent不支持change notifications，因此只能当作r48实现细节，不能当稳定API保证。

### 练习 5：验证recent的客户端截断与search的四个窄边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public Cursor queryRecentDocuments(String rootId, String[] projection,' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'limit = 64;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ContentResolver.EXTRA_HONORED_ARGS' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'while (cursor.moveToNext() && result.getCount() < limit) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ImageQuery.PROJECTION, null, null, ImageColumns.DATE_MODIFIED + " DESC");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'public Cursor querySearchDocuments(String rootId, String[] projection, Bundle queryArgs)' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'selectionArgs.add("%" + displayName + "%");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'selection.append(columnLastModified + " > " + lastModifiedAfter / 1000);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'selection.append(columnFileSize + " > " + fileSizeOver);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'mimeType.endsWith("/*")' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'shouldFilterMimeType(mimeTypes, "audio/*",' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (queryArgs.keySet().contains(QUERY_ARG_EXCLUDE_MEDIA)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'args.add(QUERY_ARG_EXCLUDE_MEDIA);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'private static final String SUPPORTED_QUERY_ARGS = joinNewline(' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

分别用 QUERY_ARG_LIMIT=3、displayName=a%、Audio MIME=application/ogg、Documents MIME=*/*、EXCLUDE_MEDIA=true 推导SQL是否执行、返回行数在哪里截断、哪些参数真的改变selection。

## 7. 普通App靠窄URI grant进入；分类tree却不能给后代建立前缀语义

ContentProvider.Transport.query() 对incoming URI执行read permission，open/openTyped执行与mode匹配的file permission。组件MANAGE_DOCUMENTS通过时直接进入；普通App通常没有这个signature|documenter级权限，只能依靠AMS中的具体URI grant。列出roots通常因此属于DocumentsUI等受信组件，得到一个leaf grant并不允许枚举所有roots或collection。

在交付Provider binder前，AMS只做一次粗筛：caller有任一组件read/write permission，或在该authority下持有任一URI grant，就有机会取得binder；checkAuthorityGrants不比对本次路径和操作mode。真正的对象级read/write必须由Transport或call分支再做。完全没有MANAGE_DOCUMENTS、同authority grant或既有handle的冷启动App，会先在获取Provider时被拒绝；这与metadata的authority内横向缺口必须同时记住。

查询结果只是能力发现，不会自动给每个返回的 image:id 授权。用户在ACTION_OPEN_DOCUMENT流程确认某个leaf后，系统才把相应read/write/persistable grant交给目标App。Document.COLUMN_FLAGS又只描述Provider实现能力，不等同于调用者当前持有哪些mode。

tree URI还要过DocumentsProvider.enforceTree()：tree root与document id相同可通过；否则必须由isChildDocument(parent, child)确认。MediaDocumentsProvider没有override，父类默认恒false，root也不广告 SUPPORTS_IS_CHILD/CREATE。因此分类层级虽然能被特权DocumentsUI浏览，却不是可用ACTION_OPEN_DOCUMENT_TREE grant导航的真实目录树。

clearCallingIdentity发生在子类入口权限门之后。这里调的是raw Binder.clearCallingIdentity()：它切Binder UID身份，不是主动清ContentProvider的calling-package ThreadLocal；后续嵌套ContentResolver进入MediaProvider时，transport再临时设置Provider自身package。这使窄SAF grant能驱动代读，但不会把caller变成MediaStore owner或给caller新增media URI grant。若某个子类入口漏掉匹配的外层read/write检查，clear identity反而会放大为confused-deputy风险，metadata正是下一节要单独处理的例外。

通用 ContentProvider.call() 不知道方法是读还是写，因此Transport本身不自动enforce。DocumentsProvider对delete等call分支显式补write门；不能由“组件有permission字段”推断每个call分支都安全。

### 练习 6：把Transport门、URI grant、tree验证和clear identity接起来

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (enforceReadPermission(callingPkg, attributionTag, uri, null)' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'enforceFilePermission(callingPkg, attributionTag, uri, mode, callerToken);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'enforceFilePermission(callingPkg, attributionTag, uri, "r", null);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'public Bundle call(String callingPkg, @Nullable String attributionTag, String authority,' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return mInterface.call(authority, method, arg, extras);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'private void enforceTree(@Nullable Uri documentUri) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'if (Objects.equals(parent, child)) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'public boolean isChildDocument(String parentDocumentId, String documentId) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'return false;' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'enforceWritePermissionInner(documentUri, getCallingPackage(),' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'final long token = Binder.clearCallingIdentity();' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'mUgmInternal.checkAuthorityGrants(callingUid, cpi, userId, checkUser)' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'private boolean checkAuthorityGrantsLocked(int callingUid, ProviderInfo cpi, int userId,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (matchesProvider(grantUri.uri, cpi)) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
```

比较MANAGE_DOCUMENTS caller、只持image:7 read grant的App、只持images_root tree grant的App：分别能否query roots、query image:7、query图片bucket children和open leaf。

## 8. 内容FD严格只读；thumbnail保留size和部分cancel，却复用第277章缓存语义

openDocument() 先把docId映成四类leaf MediaStore item URI：image、video、audio和document。root、bucket、artist、album均UnsupportedOperationException。其中document直接映到Files item，不重验media_type，所以精确获授权的generic alias也会打开同一row。mode必须精确等于 r；w、rw、rwt即使caller持write grant也会IllegalArgumentException。write grant在这里主要能授权delete，不等于可写FD能力。

实现clear identity后调用不带CancellationSignal的 openFileDescriptor(target, mode)，所以传入openDocument的signal被丢弃。底层MediaProvider仍执行普通openFileCommon读取、row/path/pending/redaction规则，只是caller变成受信Provider self。

openDocumentThumbnail() 只接受image/video leaf及两类bucket。bucket先以 BUCKET_ID=<long> 查询 DATE_MODIFIED DESC 的第一row，代表查询没有signal；leaf则直接用rowId。随后把Point写入EXTRA_SIZE，以 image/* 调MediaStore openTypedAssetFile，并把signal传给typed阶段。

这条委托首先由分类Provider incoming URI的read门授权，随后才以self进入第277章的MediaProvider thumbnail链。因此它不是第277章普通direct MediaStore item对象级授权缺口的同一入口；授权能力来自已经检查过的SAF document URI。

sizeHint在两层Bundle中保留，但MediaProvider只看EXTRA_SIZE键是否存在，仍使用固定mThumbSize粗略生成和id.jpg缓存；最终客户端ImageDecoder再按原请求整数sample。bucket也没有独立缓存key，它最终选一个leaf row id。取消不能中断bucket代表查询，进入typed链后才按第277章那些不均匀检查点生效，compress/rename尾段仍可能后发。

### 练习 7：验证四类leaf、只读mode、丢失的open signal与bucket缩略图

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private Uri getUriForDocumentId(String docId) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Files.EXTERNAL_CONTENT_URI, ident.id);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'if (!"r".equals(mode)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'return getContext().getContentResolver().openFileDescriptor(target, mode);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'public AssetFileDescriptor openDocumentThumbnail(' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'final long id = getImageForBucketCleared(ident.id);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ImageColumns.BUCKET_ID + "=" + bucketId,' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'ImageColumns.DATE_MODIFIED + " DESC");' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'opts.putParcelable(EXTRA_SIZE, size);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'openTypedAssetFile(uri, "image/*", opts, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Document.FLAG_SUPPORTS_THUMBNAIL' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'Document.FLAG_SUPPORTS_DELETE' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

对videos_bucket:9的thumbnail请求标出最后一个不能被signal中止的前置步骤；再解释为什么bucketId=9不会产生9.jpg缓存。

## 9. metadata只查索引且会截窄long；r48的call分支缺对象级read门

只有image、video、audio leaf广告 SUPPORTS_METADATA；普通document和虚拟节点不支持。子类按类型选择索引列：

| leaf | 子Bundle | 索引字段 |
|---|---|---|
| image | METADATA_EXIF | width、height、date_taken |
| video | android.media.metadata.video | duration、height、width、date_taken |
| audio | android.media.metadata.audio | artist、composer、album、year、duration |

它clear identity后按 _id 查询MediaStore collection，不重新打开原文件。结果因此取决于scanner索引新鲜度，不是完整EXIF或媒体metadata快照。图片date_taken用getLong后按当前Locale格式成String；其他 INTEGER统一cursor.getInt()再putInt，视频date_taken等64位值会截成32位。NULL/BLOB只记日志并跳过；无row或不支持type抛FileNotFoundException。

权限上有一个必须单列的r48缺口。DocumentsContract metadata走通用call；ContentProvider.Transport.call()只验证authority并设置calling package，不推断读写。DocumentsProvider.callUnchecked()对delete、isChild等分支显式enforce，但 METHOD_GET_DOCUMENT_METADATA 直接调用 getDocumentMetadata(documentId)，没有enforceReadPermissionInner。MediaDocumentsProvider自身只做shell限制，随后clear identity查询。

这不是“零关系App匿名读全库”：既无组件权限、在该authority下也无任何grant的冷启动App，会先在获取Provider binder的AMS粗门被拒。但普通App一旦已持有同authority下任一文档grant，例如audio:7，这个authority级粗查就允许它取得binder；之后它可对普通、非tree形式的image:42发metadata call，分支再也没有要求持有image:42 read grant。已持有Provider handle的调用也不会重走获取粗门。

因此应定性为**同authority内的横向对象授权缺口**：可以返回上述少量索引字段并形成id存在性oracle，却不等于能open内容、query leaf或delete；那些仍走read/write门。tree URI还会先受enforceTree约束。设备结论应记录厂商改动与后续版本，因为这里描述的是r48源码边界。

### 练习 8：证明metadata的数据来源、getInt截窄和缺失的read enforce

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public @Nullable Bundle getDocumentMetadata(String docId)' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'columnMap = IMAGE_COLUMN_MAP;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'query = Video.Media.EXTERNAL_CONTENT_URI;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'query = Audio.Media.EXTERNAL_CONTENT_URI;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'final long token = Binder.clearCallingIdentity();' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'BaseColumns._ID + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'long date = cursor.getLong(index);' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'metadata.putInt(bundleTag, cursor.getInt(index));' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F '} else if (METHOD_GET_DOCUMENT_METADATA.equals(method)) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'return getDocumentMetadata(documentId);' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'enforceReadPermissionInner(documentUri, getCallingPackage(),' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'Document.FLAG_SUPPORTS_METADATA' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

对已持audio:7 grant但没有image:42 grant的普通App，分别推演queryDocument、openDocument、getDocumentMetadata(image:42)三条入口；再与一个对该authority完全无grant且无provider handle的App对比。

## 10. delete需要write capability，但“返回成功、row删除、文件消失、grant清零”并非原子

DocumentsContract.deleteDocument()进入父类call分支后，先对分类document URI执行 enforceWritePermissionInner，再调用子类。MediaDocumentsProvider只接受四类leaf，clear identity后对唯一MediaStore item执行 resolver.delete(target,null,null)；`document:<id>`这里同样不重验media_type。它忽略返回count：row已不存在时仍可正常返回，父类随后照样revokeDocumentPermission()，同时撤同docId普通document URI与tree URI。

若MediaStore确实删除files row，普通listener会提交collection通知，并把撤MediaStore expanded grants、清缩略图、调用MediaDocumentsProvider.onMediaStoreDelete()排到background。后一个回调又撤分类leaf URI并notify roots，也覆盖从非SAF入口删除媒体的场景。父类revoke与background回调可竞速，不应写成严格的跨线程总序。

这仍不是强删除事务。第275章已经说明MediaProvider可出现物理unlink失败但row删除；第277章又说明在途thumbnail生成者可在invalidate后重发旧图。delete返回也不能证明观察者已经重query、roots已刷新或所有grant cleanup完成。相反，如果底层delete count为0，父类分类grant可已撤，而MediaProvider listener根本没有delete事件。

media_type update没有 onMediaStoreUpdate 回调。MediaStore collection会通知旧/新分类查询重查，但旧type的分类document grant不会由这一条路径显式revoke；强类型query通常已找不到row，grant账却可能暂时残留。schema write lock抑制files trigger时，insert/delete回调同样不会执行。

删除回调只按row的真实media_type构造image/video/audio/document中一个URI。若曾经存在`document:<image-id>` alias grant，从Images URI或MediaStore其他入口删row时不会撤这个alias；只有正好通过alias自身调delete，父类才会按alias docId撤它。这是grant清理与type alias不封闭的长期风险，但files._id使用AUTOINCREMENT，普通删除后的新insert不会立即复用该id；实际影响取决于能否先获得alias grant、当前row的后续类型变化，或数据库重建等非常规id重用。

revoke只清URI grant账，不会追踪并关闭已交付的ParcelFileDescriptor。所以“grant已撤”能阻止后续重新open，不证明已打开FD立即失效。

只读与delete并不矛盾：openDocument不支持写FD，而leaf flags可广告DELETE；write URI grant只是delete call的权限前提。create、rename、copy、move、remove均未override且flags不广告，父类默认实现抛UnsupportedOperationException。

## 11. getDocumentUri先验证MediaStore row，再把DATA交给ExternalStorageProvider

MediaStore.getDocumentUri(context, mediaUri) 的公开包装先读取调用者 getPersistedUriPermissions()，把mediaUri和这个快照放进Bundle，然后调用media authority。它不会枚举当前仍有效但尚未persist的临时grant。

MediaProvider的通用call transport没有自动read门，所以 GET_DOCUMENT_URI_CALL 分支显式调用 enforceCallingPermission(mediaUri,extras,false)。全局媒体能力、owner、manager或对该MediaStore item的适用grant必须让自定义read探针通过；随后才clear LocalCallingIdentity，用queryForDataFile()取得唯一row的DATA。

路径被包装为内部 file:// URI，再由MediaProvider self调用固定authority com.android.externalstorage.documents。调用者不会直接收到裸DATA，ExternalStorageProvider也要求调用者持WRITE_MEDIA_STORAGE，确保普通App不能绕开MediaProvider直接调用它的隐藏转换方法。

ExternalStorageProvider.getDocIdForFile()本身不检查file.exists()：它对absolute path与各root path做词法startsWith，既未canonicalize，也没有额外的路径分隔符边界，再截出root:path docId。这一步甚至比“规范路径确实被root包含”更弱；正常MediaStore DATA通常已受自己的路径规则约束，但不能把ESP这个helper单独写成canonical containment检查。

文件存在性要到grant选择时分叉。普通exact document grant只比较candidate docId字符串，可以在文件已消失时仍选出URI；tree grant会调用isChildDocument()，该实现先把parent与target docId都解析成必须存在的File再做canonical containment，missing target会在选择阶段失败。因此“正向不验文件”只适用于没有先触发tree验证的exact路径；无论返回与否，URI字符串都不是字节仍可打开的证明。

这条链的目标从始至终是ExternalStorageProvider root:path文档，不是MediaDocumentsProvider image:id分类文档。原因是persisted SAF grant描述真实文件树capability，而媒体分类docId没有携带真实父路径。

## 12. 正向转换用序列化grant快照挑URI；循环提前break让“tree优先”并不稳定

ExternalStorageProvider先从file path算出自己的docId，然后只检查传入UriPermission列表中authority等于自身者：

- 普通document grant要求candidate documentId与目标完全相等；
- tree grant取treeDocumentId，并用isChildDocument()验证目标位于其下；
- read/write都具备称为full，只有一个mode称为partial。

扫描过程中遇到第一个匹配且full的permission就break。扫描后选择顺序写成full tree、full exact、partial tree、partial exact，但提前break改变了全局效果：如果列表先遇到full exact，后面的full tree根本不会看；若没有full，则保存到各变量的是最后一个匹配项，再偏好tree。返回形态因此会受persisted列表顺序影响，不能简单宣称“永远tree优先”。

更重要的是，这里检查的是客户端在API调用开始时序列化的UriPermission对象，不是ExternalStorageProvider此刻向AMS重新check grant。快照之后发生revoke，甚至直接调用隐藏协议时传入不可信列表，都可能让算法仍算出一个URI字符串。安全边界在于转换不调用grantUriPermission：一个过期或构造出的描述不会恢复真实capability，后续query/open仍要过当前permission门。

因此公开API的正确使用前提仍是：caller既能读输入MediaStore对象，又已有覆盖同一路径的persisted ExternalStorageProvider grant。这里的snapshot边界只说明返回URI不等于“grant在返回瞬间仍有效”。

## 13. getMediaUri反向先查当前grant；persisted列表未使用，authority也未绑定ESP

MediaStore.getMediaUri(context, documentUri) 的包装同样读取并传入persisted UriPermission列表，但r48反向分支完全不使用它。MediaProvider先对调用者提交的documentUri执行 enforceCallingUriPermission(...READ...)；普通App可用当前临时或persisted read grant通过，不要求它已经persist。

之后MediaProvider以内部权限调用ExternalStorageProvider。隐藏方法名get_media_uri不以`android:`开头，所以DocumentsProvider.call()直接交给子类，不走其platform call的tree与authority校验。ESP从传入URI取documentId，按root tag加词法suffix执行getFileForDocId(docId,true)，要求tag对应root与拼出的目标此刻存在，再返回内部file:// path。虽然调用传了visible=true，r48 buildFile()实际忽略该形参，总是优先root.visiblePath、没有时才用root.path；它直接new File(rootPath,suffix)，也没有canonicalize/contains检查。

因此r48这两层既未把documentUri.authority限定为com.android.externalstorage.documents，也没有把suffix重新证明为canonical child。一个其他DocumentsProvider URI若恰有primary:path一类docId且caller对它有read grant，也会被ESP按自己的root tag和词法suffix解释。最终MediaProvider仍只拿这个absolute path做Files.DATA精确查询，转换也不新增权限；这是命名空间/路径解释边界，不等于caller取得了目标文件能力。

MediaProvider随后clear self identity，用FileUtils.getVolumeName()选择具体volume的 Files.getContentUri(volumeName)，再以 DATA=? 调queryForSingleItem。必须恰好一row，零行与多行都FileNotFoundException；集合查询默认排除pending与trashed，所以物理文件存在也可能暂时没有可转换的公开row。成功结果通常是 content://media/<具体卷>/file/<id>，不是Images/Video/Audio强类型URI。

反向链因此要求“incoming document read capability、ESP所选root路径上的现存文件、当前MediaStore唯一公开row”三者同时成立。persisted列表只是公开包装多传的未消费数据。

## 14. 互转只关联标识，不复制grant，也不具备canonical或跨状态原子性

getDocumentUri与getMediaUri都声明不授予新权限。正向结果随后能否访问，只取决于AMS当时是否存在覆盖该URI的组件权限或read/write grant；它可以是原persisted grant，也可以是仍有效的临时grant或后来新获的grant。反向得到Files URI后，原SAF grant不会自动变成media URI grant。App随后访问结果时，各Provider按当前账本重新判断。

它们不是MediaProvider canonicalize/uncanonicalize这组API，输出也不是canonical URI。canonical URI仍在media authority内，用title/document_id等hint恢复row；这里用DATA/root path跨越media与ExternalStorageProvider两个authority。不过正向queryForDataFile()最终进入MediaProvider.queryInternal()，会先对输入media URI执行safeUncanonicalize再取DATA；反向则新建具体卷Files item URI。路径rename、扫描、卷detach和row更新都能让两种标识在不同时间失配。

两条链还有不对称的存在性检查：正向先要求唯一MediaStore row，ESP把path变docId本身不验文件，但tree候选会在canonical child检查时要求文件存在；反向先由ESP要求所选root路径上的file存在，再要求MediaStore当前公开集合中DATA唯一。因此：

| 状态 | 正向media→document | 反向document→media |
|---|---|---|
| row存在、文件刚消失、仅有exact persisted grant | 可返回doc URI字符串 | ESP先因missing file失败 |
| row存在、文件刚消失、只有tree grant匹配 | isChild解析target时失败 | ESP先因missing file失败 |
| 文件存在、scanner尚未建row | 没有输入media row | MediaProvider因零row失败 |
| row为pending/trashed且路径存在 | item输入可能被自定义访问探针接受 | 默认Files collection search排除而失败 |
| 同DATA异常命中多row | 输入item仍可取自己的DATA | queryForSingleItem拒绝multiple |
| grant在正向快照后撤销 | 仍可能算出URI字符串 | 无其他覆盖权限时，后续使用结果会被拒绝 |

源码把底层FileNotFoundException多包装为IllegalArgumentException或IllegalStateException，公开方法本身只捕获RemoteException；不要假设所有“无等价项”都会温和返回null。排障要记录原URI authority/docId、media volume/id、DATA、file existence、pending/trashed、persisted snapshot与调用后的当前grant。

### 练习 9：画出两向桥接并找出snapshot、authority与存在性的不对称

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static @Nullable Uri getDocumentUri(@NonNull Context context, @NonNull Uri mediaUri)' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'final List<UriPermission> uriPermissions = resolver.getPersistedUriPermissions();' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'in.putParcelableArrayList(EXTRA_URI_PERMISSIONS, new ArrayList<>(uriPermissions));' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'case MediaStore.GET_DOCUMENT_URI_CALL: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'enforceCallingPermission(mediaUri, extras, false);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'fileUri = Uri.fromFile(queryForDataFile(mediaUri, null));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaStore.EXTERNAL_STORAGE_PROVIDER_AUTHORITY' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'android.Manifest.permission.WRITE_MEDIA_STORAGE, TAG' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'final String docId = getDocIdForFile(doc);' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'if (path.startsWith(rootPath)' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'if (isChildDocument(parentDocId, docId)) {' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'return FileUtils.contains(parent, doc);' frameworks/base/core/java/com/android/internal/content/FileSystemProvider.java
grep -n -F 'if (matchesRequestedDoc && allowsBothReadAndWrite(uriPermission)) {' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'return DocumentsContract.buildDocumentUriUsingTree(treeUriPermission.getUri(), docId);' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'case MediaStore.GET_MEDIA_URI_CALL: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'getContext().enforceCallingUriPermission(documentUri,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!method.startsWith("android:")) {' frameworks/base/core/java/android/provider/DocumentsProvider.java
grep -n -F 'final String docId = DocumentsContract.getDocumentId(documentUri);' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'final Uri uri = Uri.fromFile(getFileForDocId(docId, true));' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'File target = root.visiblePath != null ? root.visiblePath : root.path;' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'target = new File(target, path);' frameworks/base/packages/ExternalStorageProvider/src/com/android/externalstorage/ExternalStorageProvider.java
grep -n -F 'final Uri uri = Files.getContentUri(volumeName);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'uri = safeUncanonicalize(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaColumns.DATA + "=?", new String[] { file.getAbsolutePath() }, signal' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new FileNotFoundException("Multiple items at " + uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

分别推演“只有临时document grant的正向调用”“只有临时grant的反向调用”“其他authority但docId为primary:DCIM/a.jpg的反向调用”；明确返回URI与新增权限是两回事。

## 15. 通知、Cursor、row、文件和grant是五个状态平面，没有单一提交点

把常见事件放到时间线上更容易看出边界：

| 事件 | 同步可见 | 延后或独立状态 |
|---|---|---|
| Provider onCreate | roots URI收到notify | queryRoots仍可能因ready=false返回0 root |
| external准备任务完成 | 全局ready=true并notify roots | 其他卷未必ready |
| 普通files insert | row事务与扩展item通知可唤醒collection observer | background onMediaStoreInsert只在曾报EMPTY时notify roots |
| update metadata/path | collection observer可被item通知唤醒后重查 | 没有专门分类grant迁移；旧MatrixCursor不变，roots也不通知 |
| update media_type | 相应媒体查询可收到变化 | 旧type grant不显式撤，roots EMPTY也可能滞后 |
| deleteDocument返回 | 父类分类document/tree revoke已尝试 | MediaProvider background清grant/thumbnail并notify roots可仍在途 |
| 非SAF delete | row可先消失 | 分类leaf grant与roots通知由background回调收尾 |
| URI互转返回 | 得到另一命名空间字符串 | 当前grant、row与文件仍由后续操作重验 |

item/collection notification、roots notification与revoke各解决不同问题：前者提示已有查询重读内容，roots通知刷新ready/EMPTY能力广告，revoke回收访问票据。任何一个都不直接修改另两个状态；并且不是每种row可见性变化都会发roots通知。

因此“文件选择器看不到媒体”应依次查ready、当前external volume、media_type、root EMPTY误差、bucket/artist/album聚合、search MIME预筛与Cursor是否重查；“能query不能open”则查authority、leaf类型、read grant、精确r mode、底层DATA/pending/trashed与shell限制；“删除后仍可见”要区分旧MatrixCursor、后台grant账、物理字节与缩略图回写。

schema rebuild、进程重启、卷detach/reattach还会改变这些状态的恢复方式：静态ready/EMPTY位随进程消失，persisted URI grant在系统账本，external.db row与物理文件各自持久，MatrixCursor只活在客户端内存。不要把某次UI刷新当成全链持久一致性证明。

## 16. 用完成点矩阵诊断，并带着三条边界进入DocumentsUI

| 症状 | 首查完成点 | r48关键反例 |
|---|---|---|
| roots第一次为空 | readiness | onCreate先notify，ready=false仍返回空Cursor |
| Documents root非EMPTY却无child | root COUNT与child selection | COUNT整个Files，child只取MEDIA_TYPE_DOCUMENT |
| 同一bucket混入无关目录 | docId/grouping | 32位父路径hash碰撞，docId不含volume/path |
| 请求排序未生效 | queryChild | sortOrder收到但未使用 |
| recent设limit仍查询很慢 | DB query→Matrix复制 | limit只限制复制，不下推SQL |
| Audio OGG搜索为空 | MIME预筛 | application/ogg虽被root广告却不匹配audio/*门 |
| EXCLUDE_MEDIA显示honored却无效果 | result extras | key被报告，selection从未读取 |
| write grant可删但不能写FD | capability分层 | delete需write门；open仍只接受r |
| 已有一个grant却能查别id metadata | provider粗门→call对象门 | authority粗查可获binder，r48 metadata缺enforceReadPermissionInner |
| document:id打开了图片row | type alias映射 | direct query/open未重验MEDIA_TYPE_DOCUMENT |
| Documents搜索*/*反而为空 | MIME selection | 通配符被改成mime_type LIKE '*/%' |
| delete成功但文件仍在 | row→filesystem | MediaProvider删除不是物理强证明 |
| 正向转换有临时grant却失败 | grant输入 | 只传persisted列表 |
| 正向返回doc URI但随后打不开 | snapshot→当前grant | 返回字符串不创建或恢复AMS grant |
| 反向返回Files而非Images URI | path→row | queryForMediaUri固定Files concrete volume |
| 其他authority被当成ESP docId | reverse namespace | 先查提交URI grant，却未绑定authority再解释docId |
| 通知后Cursor内容不变 | snapshot/observer | MatrixCursor必须重新query |

整条链可压成一句话：MediaDocumentsProvider用MANAGE_DOCUMENTS或窄SAF grant守住分类入口，再以self把type:id投影到MediaStore；它提供的是只读内容加独立delete能力，通知与revoke只做分层收敛。公开MediaStore互转则借DATA/visible path连接ExternalStorageProvider，grant快照只帮助选择已有URI形态，绝不随转换复制权限。

下一章进入DocumentsUI：从ACTION_OPEN_DOCUMENT、CREATE_DOCUMENT、GET_CONTENT与OPEN_DOCUMENT_TREE怎样归一成State开始，继续追ProvidersCache、Root/Directory加载、选择结果Intent、临时grant与takePersistableUriPermission的两阶段完成点。
