# 275 Android MediaProvider insert、update、delete、路径放置、rename事务、文件I/O与通知分发链

## 1. 先看结论：一次“写成功”至少有六个不同的完成点

本文以 Android 11 `android-11.0.0_r48` 为边界：`frameworks/base` 位于 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/providers/MediaProvider` 位于 `47c141d93e93b25cc85c36f3579fda25a1695952`。MediaProvider 的写路径不能简化成“改一行数据库并通知一下”。更准确的模型是六个可分别成功或失败的完成点：

1. **请求已通过政策检查**：URI、调用身份、列、MIME、volume 与目标目录组合合法。
2. **目标路径已确定**：`RELATIVE_PATH + DISPLAY_NAME + MIME_TYPE` 被清洗、补扩展名并映射为 `_data`；必要的父目录可能已经创建。
3. **数据库 row 已提交**：`files` 表 mutation 与 generation 提交，唯一键冲突已失败、upsert 或兼容 replace。
4. **文件实体已改变**：媒体字节被写入，或者 lower filesystem 的 rename/unlink 已发生。
5. **索引元数据已收敛**：写 FD 关闭或 pending 发布后，扫描器重新读取文件事实。
6. **通知与派生副作用已分发**：observer 通知、thumbnail 失效、quota、URI grant、SAF 与 DownloadManager 工作进入各自执行阶段。

SQLite 事务只能原子化数据库状态。`mkdirs()`、`Os.rename()`、`File.delete()`、写 FD 和扫描器都不属于同一提交域。因此，`insert()` 返回 URI、`update()` 返回 1、普通 row-backed `delete()` 返回 1、FUSE rename 返回 0，各自证明的范围都不同；`media_scanner` 等特殊控制 URI 的 delete 返回值不能按 row count 解释。

| 对外结果 | 能直接证明什么 | 不能直接证明什么 |
|---|---|---|
| 普通媒体 `insert()` 返回 item URI | row 已插入或受限 upsert 已完成 | 目标媒体文件已有字节、元数据已扫描 |
| placement `update()` 返回 1 | row update 已提交 | rename 与 row 从未出现过分叉窗口 |
| 普通 row-backed `delete()` 返回 1 | 累计删除了一行匹配的数据库记录 | `File.delete()` 确实返回 true、介质已安全擦除 |
| 受管 FUSE rename 返回 0 | lower rename 成功且数据库事务被标记成功 | 后续扫描已完整修好全部索引、观察者已消费通知 |
| 写 FD `close()` 返回 | 内核句柄关闭 | 后台 close listener 和扫描已经跑完 |

## 2. 两个写入平面共享政策，却不共享操作顺序

第一类入口来自 `ContentResolver`：`insertInternal()`、`updateInternal()`、`deleteInternal()`、`openFileCommon()` 从 URI、`ContentValues` 与 extras 出发。第二类来自 FUSE：native 的 create、unlink、rename 回调进入 Java 的 `insertFileIfNecessaryForFuse()`、`deleteFileForFuse()`、`renameForFuse()`。两类入口复用 `files` 表、路径推导、owner、权限与 trigger，却刻意采用不同顺序。

普通 collection insert 先确定路径、可能建父目录，再提交 row；目标媒体文件通常等调用者随后打开 URI 才创建。ContentProvider placement update 先 rename 文件，再提交 row。受管 FUSE rename 则在数据库事务内先尝试协调 row、再 rename lower 文件，成功后才提交；unchecked 兜底也可在没有 source-row update 时继续。FUSE 的 database-bypass 身份又会直接 rename lower 文件，不更新数据库。

源码地图如下：

- `MediaProvider.java`：四类 ContentProvider 入口、FUSE Java 回调、权限与后处理；
- `FileUtils.java`：现代列与 `_data` 双向计算、清洗、扩展名和唯一命名；
- `SQLiteQueryBuilder.java`：每次 mutation 强制进入 helper 事务；
- `DatabaseHelper.java`：事务状态、files trigger、通知聚合和三阶段任务；
- `FuseDaemon.cpp`、`MediaProviderWrapper.cpp`：native create/unlink/rename 顺序；
- `ModernMediaScanner.java`：写入后的元数据收敛。

`applyBatch()` 会为每个进入本批次前尚未活动的 helper 打开独立外层事务；若 helper 在进入批次前已有事务，它的每个 operation 只有允许异常才能 piggyback。同一 helper 的 `runWithTransaction()` 复用该事务。`super.applyBatch()` 正常返回后——允许异常的 operation 即使失败，也会把异常装进 `ContentProviderResult` 而不阻止此处继续——本次新开的 helper 才各自被标记 successful，然后在 `finally` 里依次 `endTransaction()`。因此 exception-allowed 失败不自动触发整批回滚，它早先产生的非 DB 副作用也没有单 operation 回滚保证。多个 helper 更不是分布式原子提交：较早 helper 可能已 commit 并启动后处理，较晚 helper 仍可能在 commit 或 post-commit blocking task 中失败；一次 `endTransaction()` 抛异常还会中断这个循环。

### 练习 1：画出两类入口与批事务边界

在源码根目录运行；也可把源码根目录作为第一个参数，从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private @Nullable Uri insertInternal(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private int updateInternal(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private int deleteInternal(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private ParcelFileDescriptor openFileCommon(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public int deleteFileForFuse(@NonNull String path, int uid)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public int renameForFuse(String oldPath, String newPath, int uid)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'static void pf_unlink(fuse_req_t req, fuse_ino_t parent, const char* name)' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'const int res = fuse->mp->Rename(old_child_path, new_child_path, req->ctx.uid);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'if (!helper.isTransactionActive()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final ContentProviderResult[] result = super.applyBatch(operations);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (mExceptionAllowed) {' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'return new ContentProviderResult(e);' frameworks/base/core/java/android/content/ContentProviderOperation.java
grep -n -F 'return helper.runWithTransaction((db) -> {' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
```

画图时不要把箭头合并：标出 ContentProvider movement 的 FS→DB、受管 FUSE rename 的未提交 DB→FS→commit，以及 database-bypass FUSE rename 的 FS-only。

## 3. insert先收回系统列，再决定raw path、download与owner

`insertInternal()` 先处理并非普通 files row 的 URI：`media_scanner` 修改扫描状态，`volumes` 调 `attachVolume()`，playlist member 写入会持久化到真实 playlist 文件。只有普通 image、video、audio、playlist、downloads、files 与 thumbnail/album-art 分支才继续走各自插入路径。

普通插入会移除调用者给出的 `_id`；`DATE_EXPIRES` 也先被删除，再依据本次 `IS_PENDING` 或 `IS_TRASHED` 请求按系统时钟重算。这里还有 raw path 反推的覆盖层：获准传入的 `.pending-<秒>-名字` 或 `.trashed-<秒>-名字` 会被 `computeValuesFromData()` 再解析，文件名编码的 expiry 可覆盖刚才的派生值；FUSE 普通物理名的 pending 则会保留 pending、却清掉 expiry。

非 self 调用者提供的 `IS_DOWNLOAD` 被移除。Images、Video、Audio、Files 等分支在路径生成**之前**调用 `maybeMarkAsDownload()`：只有此刻已经获准携带 raw `_data`，函数才能按 Download 路径写 1；只有现代列、`_data` 尚空时，即使稍后生成到 `Download/`，这里也不会回头重算。Downloads collection 才无条件强制写 1。经纬度被写成 SQL `NULL`，target Q 及以下残留的旧目录列被移除。

raw path 权限要按操作区分。insert 中，self、legacy write 和 manager 可以保留 `sDataColumns`；普通调用者的 raw `_data` 等列被丢弃，随后从现代放置列生成路径。即使 manager 的 raw path 被接受，`assertFileColumnsSane()` 仍 canonicalize 路径并要求它属于目标 volume 的 scan roots，不能把某个卷 URI 指向另一个卷或系统任意路径。canonical file 只用于这次包含关系检查，源码没有把 canonical path 写回 values；获准的原始字符串仍可能进入 `_data`。

owner 是 MediaStore 对象所有权，不是 inode 的 Linux uid。self 与 shell 可显式提供 owner，缺失时只从**此刻已有的 incoming raw path** 猜；现代列输入尚未生成 `_data`，后面生成到 `Android/media/<package>` 并不会再补猜 owner。delegator 可代表目标包，缺失时回退 Binder package；普通远端调用者不能直接控制 `OWNER_PACKAGE_NAME`，Provider 强制使用真实 calling package。这个 owner 后续参与 item 权限、pending 发布、upsert 与 replace 决策。

### 练习 2：核对insert的系统列与owner矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (match == MEDIA_SCANNER) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (match == VOLUMES) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.remove(MediaColumns._ID);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'FileUtils.computeDateExpires(initialValues);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (isCallingPackageSelf() || isCallingPackageLegacyWrite()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (isCallingPackageManager()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.remove(FileColumns.IS_DOWNLOAD);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean isDownload = maybeMarkAsDownload(initialValues);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.put(FileColumns.IS_DOWNLOAD, 1);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.putNull(ImageColumns.LATITUDE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ownerPackageName = extractPathOwnerPackageName(path);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (isCallingPackageDelegator()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.remove(FileColumns.OWNER_PACKAGE_NAME);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ownerPackageName = getCallingPackageOrSelf();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final Collection<File> allowed = getVolumeScanPaths(volumeName);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '.getCanonicalFile();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!FileUtils.contains(allowed, actual)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

分别推演普通 R 应用、legacy writer、manager、scanner 与 delegator 给出 `_id`、`DATE_EXPIRES`、`IS_DOWNLOAD`、raw `_data`、owner 时，哪些值会保留、重算或移除。

## 4. collection URI是一套放置类型系统，不只是SQL表名

`ensureFileColumns()` 先由 URI match 选默认 MIME、默认 media type、默认 primary directory 与 allowed-primary 集合：

| collection | 默认MIME | 默认目录 | 常规允许的顶级目录 |
|---|---|---|---|
| Audio | `audio/mpeg` | `Music/` | Alarms、Audiobooks、Music、Notifications、Podcasts、Ringtones |
| Video | `video/mp4` | `Movies/` | DCIM、Movies、Pictures |
| Images | `image/jpeg` | `Pictures/` | DCIM、Pictures |
| Playlists | `audio/mpegurl` | `Music/` | Music、Movies |
| Downloads | `application/octet-stream` | `Download/` | Download |
| Files | `application/octet-stream` | `Download/` | Download、Documents；playlist/subtitle 再扩展 Music、Movies |

MIME 缺失时，target R 及以上先从 `DISPLAY_NAME` 后缀推导，推不出才用 collection 默认值；旧 target 的具体媒体 collection 保留默认 MIME 行为。具体 collection 收到无受支持扩展映射的 MIME 时，会尝试使用文件名推导出的同类 MIME；仍不成立时，R+ 抛异常，旧 target 回退默认值。最终 media type 必须与 Images、Video、Audio 等 collection 一致。

Files 的初始 `MEDIA_TYPE_NONE` 让它能接受一般文件，但 playlist 与 subtitle 会扩展默认目录政策。这里的“generic”不是免除路径、owner、volume 与权限检查。若 `_data` 为空且目标是 internal volume，Provider 直接拒绝创建路径；internal 数据库用于索引系统媒体，并不是普通应用的写入目标。

### 练习 3：从URI还原默认MIME与目录矩阵

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'String defaultMimeType = ClipDescription.MIMETYPE_UNKNOWN;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'String defaultPrimary = Environment.DIRECTORY_DOWNLOADS;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'defaultMimeType = "audio/mpeg";' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Environment.DIRECTORY_AUDIOBOOKS,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'defaultMimeType = "video/mp4";' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'defaultMimeType = "image/jpeg";' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'defaultMimeType = "audio/mpegurl";' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case FileColumns.MEDIA_TYPE_PLAYLIST:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case FileColumns.MEDIA_TYPE_SUBTITLE:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String mimeTypeFromExt = TextUtils.isEmpty(displayName) ? null :' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new IllegalArgumentException("Unsupported MIME type " + mimeType);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (defaultMediaType != actualMediaType) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Writing to internal storage is not supported.' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

尝试解释 `photo.mp3 + image/jpeg` 与 `photo.jpg + image/*`：前者的名字后缀不一定被保留，后者在 R+ 可由扩展名纠正为受支持的 image MIME。

## 5. 现代放置列先形成候选路径，唯一命名只看当前文件系统

若允许的 raw `_data` 已存在，`computeValuesFromData()` 以路径为准，重算 volume、relative path、display name、bucket、pending、trashed 与 expiry；调用者同时给出的矛盾现代列不会胜出。若 `_data` 为空，Provider 补默认 `DISPLAY_NAME` 与 `RELATIVE_PATH`，清洗 FAT 非法字符和路径段，再由 `computeDataFromValues()` 组合 volume root、relative path 与物理文件名。

非 FUSE pending 使用 `.<pending-prefix>-<expiry>-<display-name>` 物理名；trash 同样使用带过期时间的隐藏物理名。row 中保存的是逻辑 `DISPLAY_NAME`：会去掉 pending/trash 物理前缀，也可能已被清洗、补扩展名或加唯一后缀，不能笼统地说它恒等于最初输入。FUSE pending 不改物理名，数据库状态与文件名可以暂时不对称。

`splitFileName()` 检查后缀与 MIME 是否一致；不一致时，原 display name 会成为 basename 的一部分，并追加 MIME 的默认扩展名。`buildUniqueFile()` 再检查真实文件是否存在：一般名字尝试原名和最多 31 个括号编号；DCIM 中的严格 DCF 名递增四位序号，日期式相机名使用 `~N`。

这里有一个重要的双命名空间边界：唯一命名只调用 `File.exists()`，不会先查询 `_data UNIQUE`。如果先前 insert 只预留了 row、文件尚未创建，第二次同名 insert 仍会得到同一候选路径，随后才在 SQLite 唯一键处进入 owner 受限 upsert 或失败。

### 练习 4：证明物理名字、显示名字与唯一键不是同一层

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static void computeValuesFromData(@NonNull ContentValues values, boolean isForFuse) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'final Matcher matcher = FileUtils.PATTERN_EXPIRES_FILE.matcher(displayName);' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'values.put(MediaColumns.DISPLAY_NAME, matcher.group(3));' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static void computeDataFromValues(@NonNull ContentValues values,' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'if (!isForFuse && getAsBoolean(values, MediaColumns.IS_PENDING, false)) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F '} else if (getAsBoolean(values, MediaColumns.IS_TRASHED, false)) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static void sanitizeValues(@NonNull ContentValues values,' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static String[] splitFileName(String mimeType, String displayName) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static File buildUniqueFile(File parent, String mimeType, String displayName)' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'if (!file.exists()) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'private static final Pattern PATTERN_DCF_STRICT = Pattern' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'private static final Pattern PATTERN_DCF_RELAXED = Pattern' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'return i < 32;' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F '_data TEXT UNIQUE COLLATE NOCASE' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
```

用“row 已存在但 file 不存在”和“file 已存在但 row 不存在”两种初态手算同名插入；两者分别在数据库唯一键和物理唯一命名层分流。

## 6. 路径准入按五层扩展；mkdir早于row事务

生成候选路径后，准入依次扩展，而不是只看一个存储权限：

1. update 时若新旧父目录相同，允许原目录内改名；insert 没有 `currentPath`，不走这一条。
2. 顶级目录属于当前 collection 的 `allowedPrimary`。
3. insert extras 给出 related URI，且关联对象与新对象的 MIME 主类型、`RELATIVE_PATH` 都完全相同；这里没有额外比较 volume。关联项可见但两项不匹配会立即抛错，查询不到关联项才记录后继续尝试后面的放行层。update 会主动移除 related URI，不能借它搬动既有对象。
4. 路径位于调用者 shared package 自己的 `Android/media` 目录。
5. manager 在这个 ContentProvider 生成路径分支直接把 `validPath` 置 true；不要把 FUSE create 对其他包 private path 的拒绝错误移植到这里。否则，只有未请求 legacy storage、且具备最终路径所对应 image/video 写能力的调用身份才进入这层放行；它可使用已有目录或在已有顶级目录下创建子目录，但不能借此创建不存在的非默认顶级目录。

全部规则失败才抛 placement 异常。raw `_data` 不走这套生成分支，但仍走 volume scan-root 校验。

路径通过后，`res.getParentFile().mkdirs()` 会在数据库 mutation 前执行，只检查最终父目录是否存在，不记录哪些层级是本次创建的。insert 会在 row 事务前走到这里；ContentProvider movement 的 non-unique probe 和 unique 目标计算也都复用同一段逻辑，因而可在 `Os.rename()` 之前建父目录。若后续 `_data` constraint、权限 builder、rename 或进程失败，空目录不会被 SQLite 回滚。

### 练习 5：逐层验证placement准入与mkdir窗口

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final String currentDir = (currentPath != null)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'boolean validPath = res.getParent().equals(currentDir);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'validPath = allowedPrimary.contains(primary);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final Uri relatedUri = extras.getParcelable(QUERY_ARG_RELATED_URI);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!Objects.equals(expectedType, actualType)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!Objects.equals(expectedPath, actualPath)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String pathOwnerPackage = extractPathOwnerPackageName(res.getAbsolutePath());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'validPath = isExternalMediaDirectory(res.getAbsolutePath()) &&' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'validPath = isCallingPackageManager();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean createNonDefaultTopLevelDir = primary != null &&' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'canAccessMediaFile(res.getAbsolutePath(), /*allowLegacy*/ false);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'res.getParentFile().mkdirs();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!res.getParentFile().exists()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'extras.remove(QUERY_ARG_RELATED_URI);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

为每一层各造一个允许样例，再造一个 `Pictures/` collection 指向不匹配顶级目录的拒绝样例；最后标出目录已建、row 未提交的失败点。

## 7. insert提交的是row预留；受限upsert解决双入口竞态

`insertFile()` 先确保最终 path，`computeValuesFromData()` 由路径回填 bucket 等列；`DATE_ADDED` 来自当前时钟，`TITLE` 仅在未给出时才由 path 兜底，format、MIME 与 media type 还有目录、已提供值和调用身份分支。若 path 已存在，Provider 总会用磁盘 mtime 覆写 date-modified，只有 size 是缺失时才补。目录使用 association format、MIME 为 null，并写入 `mDirectoryCache`；普通媒体 insert 不 touch 目标文件，调用者要用返回 URI 打开并写字节。还有一个 r48 实现细节：普通 `insertFile()` 分支只给 `newUri` 赋值，`insertInternal()` 的局部 `rowId` 仍是 -1，末尾 `setOwned(rowId, true)` 因而没有把真实新 id 放进这条快速 owned-id cache；row 中的 owner 值本身不受影响。

事务内若 `PARENT` 未给出，`getParent()` 查找或建立父目录 row；显式非 null 的 parent 会跳过推导。随后先做普通 `qb.insert()`；若 `_data` 唯一键冲突，Provider 用 generic Files update builder 查同路径 row，并要求 owner 属于 calling shared packages，或属于 delegator 被允许代表的 owner。owner allowlist 只是必要条件：Files 的 TYPE_UPDATE builder 仍保留 volume 与行权限等过滤，但 `getQueryBuilderForUpsert()` 对 pending 和 trashed 明确设为 `MATCH_INCLUDE`。查到 id 且定点 update 恰好影响 1 行才保留原 id，否则重抛 constraint。R+ 调用者最终看到异常；旧 target 的顶层 `insert()` 兼容包装会把未解决的 constraint 变成 null。

parent 与 directory cache 也不是纯数据库状态：`getParent()` 在同一 transaction 中可递归插祖先 row，但目录 path→id cache 会立即写入内存。最终 insert/upsert 回滚时，SQLite 能撤销祖先 row，cache 没有相应的 transaction rollback 钩子，因而存在陈旧 parent-id cache 窗口。

这不是无条件覆盖。它专门处理“应用先从直接文件路径创建，FUSE 已插 row；随后又用 ContentResolver insert”以及“前一次只预留 row”的重复入口。owner 不匹配时仍失败，阻止调用者选择受害者路径覆盖其索引。

playlist 是字节层的特殊例外：远端创建 playlist row 后，Provider 再通过返回 URI 打开并关闭输出流，尝试 touch 空文件；I/O 异常被忽略，所以 playlist 的 URI 返回也不是实体创建成功证明。

### 练习 6：追踪row、parent、upsert与playlist实体

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'values.put(MediaStore.MediaColumns.DATE_ADDED, System.currentTimeMillis() / 1000);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (path != null && new File(path).isDirectory()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (file.exists()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'rowId = insertAllowingUpsert(qb, helper, values, path);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Long parent = values.getAsLong(FileColumns.PARENT);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final long parentId = getParent(db, path);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return qb.insert(helper, values);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String packages = getAllowedPackagesForUpsert(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final long rowId = getIdIfPathOwnedByPackages(qbForUpsert, helper, path, packages);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qbForUpsert.update(helper, values, "_id=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw e;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return ContentUris.withAppendedId(uri, rowId);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mDirectoryCache.put(parentPath, id);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mCallingIdentity.get().setOwned(rowId, true);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (getCallingPackageTargetSdkVersion() >= Build.VERSION_CODES.R) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '.openOutputStream(newUri)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} catch (IOException ignored) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

观察同一物理名字先后由同 owner 与不同 owner 插入时的分歧，并说明为什么 `buildUniqueFile()` 没有提前消除“只有 row、没有 file”的冲突。

## 8. update先过滤列；pending发布把磁盘重新设为元数据真相

`updateInternal()` 同样移除 `_id`、重算 expiry、清经纬度。raw data 权限比 insert 更窄：只有 self 与 legacy write 明确保留 `sDataColumns`，没有 manager 特例。manager 仍可通过现代 placement 列进入受控 movement，但不能据此推断任意 raw `_data` update 会生效。

owner transfer 也独立检查：self/shell 可改；delegator 只在当前 owner 为空或属于自己的 shared packages 时可转给 proposed owner；其他调用者给出的 owner 被移除。对象可写权并不自动附带 owner 转移权。

对非 self 调用者，`sMutableColumns` 中的 placement、favorite、部分 bookmark/tags/category、playlist、download 来源等列才正常保留。scanner 控制的 duration、width、height、title 等列在已发布对象上被忽略，并把 `triggerScan` 置 true，因为可信值应来自磁盘；但若请求里只有这些被移除的列，随后 `initialValues.isEmpty()` 会直接返回 0，已经置位的 `triggerScan` 也不会执行。至少还有一个可写列留下时，后面的扫描路径才会到达。

pending 放宽也不是任意 URI 的通则：它调用旧的 `isPending(uri)`，只识别 Audio、Video、Images 的 typed item ID。Files/Downloads item 或 selection 批量 update 不会靠这个方法获得同样的非 mutable 列放宽。

对非 self 调用者，只要 values **出现** `IS_PENDING` 键，不论它是不是一次 1→0 发布，Provider 都要求 scan，并把 date-modified 与 size 置 null，打破扫描器的 mtime/size no-op 快路。发布后扫描重新读取 EXIF、retriever 等磁盘事实。self/scanner 不进这段过滤与置空逻辑，并会在后面强制取消其他路径设置的 `triggerScan`，避免扫描写 row 再触发自身。

## 9. ContentProvider movement是“先FS、后DB”，ENOENT也不阻止row前进

placement 集合包含 `_data`、relative path、display name、MIME、pending、trashed 与 expiry。只有未直接提供 `_data`、非 thumbnail、`allowMovement=true` 时才进入受控移动；默认值对外部调用者为 true，对 self 为 false。直接 raw `_data` update 只是重指 row，不执行这段 `Os.rename()`。

movement 只接受明确的单 item media/files/downloads URI。Provider 以内置身份查询当前 placement 列，把调用者未提供的字段融合进去；先用 non-unique 路径计算 probe。路径未变就不移动；volume 改变或从路径提取出的 package owner 域改变则拒绝。确认真的移动后才用 unique 模式生成最终目的地。

关键顺序是：两次路径计算都可先 `mkdirs()` → 尝试 `Os.rename(before, after)`。只有 rename 成功才接着失效两端 FUSE dentry；`ENOENT` 会跳过这两次失效，但仍把 after path 写回 values，再进入 `updateAllowingReplace()` 的数据库事务，因而 row 仍可能提交到新路径；其他 errno 才终止。反方向上，rename 已成功而数据库 constraint、进程终止或后续异常发生，文件可在新路径、row 仍在旧路径。unique probe 只看当时的 `File.exists()`；若目的路径只有数据库 row，或另一进程在 probe 后创建目标文件，rename 甚至可能先写入或覆盖目标，随后 DB 的 owner/constraint 检查才失败。扫描器承担恢复，不存在跨 VFS 与 SQLite 的总回滚。

授权时序还有更窄的边界：Audio、Video、Images typed item 在 movement 前显式 `enforceCallingPermission()`；Files、Downloads 与 playlist item 没有这项前置 item 检查。旧 placement 的融合查询又在清除调用身份后执行，于是从这段 Java 链自身看，后几类 URI 可能先完成 `Os.rename()`，最终 TYPE_UPDATE builder 才用调用者身份把 row mutation 过滤成 count 0。此时 API 可返回 0、文件已移动、row 仍指旧路径，affected-id 快照也可能为空而没有补扫。不能把“最终数据库写权限过滤”当作“文件移动前授权屏障”。

## 10. update的兼容replace与后处理顺序必须按事务上下文理解

`updateAllowingReplace()` 在 `_data` constraint 时对 R+ 直接重抛。旧 target 只有在目标 path 当前存在、冲突 row 属于 calling shared packages 且可删除时，才在同一数据库事务内删冲突 row 并重试 update。对 placement movement 而言，文件已先 rename 到目标，所以“目标存在”可能只是刚移动来的源文件，不能证明冲突 row 原先拥有另一份实体。

路径或 scanner metadata 变化会在 mutation 前快照 affected IDs，因为 update 后原 query builder 可能不再匹配。row 提交后，每个 id 安排 thumbnail 失效；需要 scan 时再查询更新后的 `_data` 并调用 scanner。

`postBlocking` 的名称不能脱离上下文解释。独立 update 中，内部 row 事务已经在 `updateAllowingReplace()` 返回时结束，因此随后调用 `postBlocking()` 会在当前线程立即扫描；此前 transaction notification 只是已投递到 foreground executor，两者没有一个共同队列屏障。`applyBatch()` 在对应 helper 上仍有外层事务，此时 blocking scan 才真正排队；该 helper 成功结束后先逐项执行，再统一投递通知，最后提交 background tasks。若 batch 跨多个 helper，各 helper 在 `finally` 中顺序结束，前一个可以已经 commit 并启动后处理，而后一个的结束才失败。两种情况下，publish update 通常都在向调用者返回前完成 blocking scan，但通知观察者何时消费不是返回屏障。

### 练习 7：比较update过滤、movement与两种后处理上下文

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'extras.remove(QUERY_ARG_RELATED_URI);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (sMutableColumns.contains(column)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (isPending.get()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (initialValues.isEmpty()) return 0;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private boolean isPending(Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.putNull(MediaColumns.DATE_MODIFIED);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'initialValues.putNull(MediaColumns.SIZE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean allowMovement = extras.getBoolean(MediaStore.QUERY_ARG_ALLOW_MOVEMENT,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ensureNonUniqueFileColumns(match, uri, extras, initialValues, beforePath);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'try (Cursor c = queryForSingleItem(genericUri,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new IllegalArgumentException("Changing volume from " + beforePath + " to "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new IllegalArgumentException("Changing ownership from " + beforePath + " to "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Os.rename(beforePath, afterPath);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (e.errno == OsConstants.ENOENT) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'count = updateAllowingReplace(qb, helper, values, userWhere, userWhereArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.postBackground(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.postBlocking(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (getCallingPackageTargetSdkVersion() >= Build.VERSION_CODES.R) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return qb.update(helper, values, userWhere, userWhereArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

分别画独立 `update()` 和处于 `applyBatch()` 外层事务中的 update；特别标出 notify 已提交给 executor 与 observer 已处理并不是同一件事。

## 11. delete先尝试实体、再删row；count只计数据库变化

普通 files mutation 的 query builder 实际指向 `files` 表。Audio、Video、Images typed item delete 会先显式执行可写权限检查，让缺权调用者有机会走具体 URI 的用户授权升级；Files、Downloads 等分支没有同样的前置升级动作，而由 query builder 的可写行过滤决定是否命中。

随后 delete 查询 media type、`_data`、id、download 标志与 MIME，对每个匹配项先清 calling identity 的 owned-id cache，再调用 `deleteIfAllowed()`，最后按 id 执行 `qb.delete()`。`deleteIfAllowed()` 恢复调用者权限检查，但捕获所有异常；`deleteAndInvalidate()` 又无视 `File.delete()` 的 boolean，只继续失效 dentry。owned-id 的提前清除不会被后续数据库回滚恢复，通常造成保守的 cache miss，不是错误授权。

因此 row delete 可以提交，而文件因权限、I/O、忙碌或其他原因仍存在。普通 row-backed delete 的返回 count 累计的是 `qb.delete()` row 数，不是成功 unlink 数，更不是安全擦除证明。后续扫描可能把孤儿文件重新插入；反过来，如果 unlink 已成功而 row transaction 随后失败，便得到“无文件、有 row”。多 row 的独立 delete 还可能为每个 `qb.delete()` 各开一次事务，前面的 row 已提交后，后面的异常不会自动回滚整次调用；只有同一 helper 的外层 batch 等事务会包住它们，物理删除仍不可回滚。

URI 参数 `deletedata=false` 明确跳过实体遍历，直接删索引，扫描对账用它清理磁盘上已不存在的旧 row。它不表示“安静删除”：row trigger 仍可产生通知、授权撤销与 thumbnail 失效。

## 12. parent循环、playlist、Downloads与FUSE delete各有独立边界

`deleteRecursive()` 会在数据库事务内先清空整个 directory cache，再重复相同 delete 直到返回 0。事务回滚不会还原这个内存 cache，通常只导致后续重查。它的设计说明配合 `_id NOT IN (SELECT parent...)` 可逐层剥离叶子与父目录；但不能把设计说明误写成 r48 所有 files delete 的实际保证。

在普通实体分支里，源码先遍历 cursor、逐 id 删除 row，之后才向 query builder 追加 `ID_NOT_PARENT_CLAUSE`，再调用 `deleteRecursive()`；这些前置逐 id 删除并未受 parent 谓词保护。`deletedata=false` 又完全跳过追加谓词。因而排障时要看具体 call site 和 selection，不能仅凭 helper 名称断言“父 row 一定最后删”。image/video thumbnail 表走专门的“先尝试删文件、再 recursive row delete”分支；audio album-art 的 builder 指向 `album_art`，在这个方法里落入默认 row delete，没有复用该实体删除分支。

删除 external audio row 后，Provider 查询 `audio_playlists_map` 的受影响 playlist 并从 playlist 文件重新解析成员。download id 与 MIME 来自删除前 cursor：只要 `IS_DOWNLOAD=1` 就加入集合，即使该 id 的 `qb.delete()` 返回 0。循环后才把它们交给 DownloadManager。独立 delete 此时已无活动 helper transaction，`postBackground()` 直接提交任务，可与先前逐 row 事务投递的 foreground notification 竞速；只有收集在同一 `TransactionState` 的任务才受分阶段顺序约束。row trigger 还可安排 URI revoke、thumbnail 失效与 SAF 删除回调。

FUSE unlink 先按身份与 row 命中分流。native wrapper 的 ROOT 直接 `unlink()`；Java 的 database-bypass caller 直接 `deleteFileUnchecked()`，两者都不改 row。普通受管路径调 Provider `delete()`，row count 大于 0 即向 FUSE 返回成功，即使内部 `File.delete()` 返回 false；若 count 为 0 但 caller 可 bypass FUSE restrictions，又回退到一次 FS-only 删除。只有受管 row-delete 分支共享 files trigger 与通知链。如果 Provider 在物理删除后抛出 Java 异常，JNI 把它转为 `EFAULT`，native 不调 `SetDeleted()`，此时 lower file 却可能已不在。另外，FUSE 路径删 row 后，同 UID 再按 item URI delete，cached deleted-row id 会让第二次调用静默返回 0；这表示没有新 row 被删。

目录 `rmdir` 是另一条 native 路径：先问 Java 是否允许，再直接对 lower path 调 `rmdir()`。它不经过逐 row delete 链，也不自行请求 scan、row mutation 或 observer notification；只有未来因其他原因触发的扫描才可能收敛目录索引。

### 练习 8：用反例审计delete、open与FUSE I/O

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'removeDeletedRowId(Long.parseLong(uri.getLastPathSegment()))' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (deleteparam == null || ! deleteparam.equals("false")) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'deleteIfAllowed(uri, extras, data);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'count += qb.delete(helper, BaseColumns._ID + "=" + id, null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, ID_NOT_PARENT_CLAUSE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'n = qb.delete(helper, userWhere, userWhereArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'file.delete();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private void deleteIfAllowed(Uri uri, Bundle extras, String path) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public int deleteFileForFuse(@NonNull String path, int uid) throws IOException {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return deleteFileUnchecked(path);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (shouldBypass) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (uid == ROOT_UID) {' packages/providers/MediaProvider/jni/MediaProviderWrapper.cpp
grep -n -F 'int MediaProviderWrapper::Rename(const string& old_path, const string& new_path, uid_t uid) {' packages/providers/MediaProvider/jni/MediaProviderWrapper.cpp
grep -n -F 'if (delete(contentUri, where, whereArgs) == 0) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'int status = fuse->mp->DeleteFile(child_path, ctx->uid);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'child_node->SetDeleted();' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'int status = fuse->mp->IsDeletingDirAllowed(child_path, req->ctx.uid);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'if (rmdir(child_path.c_str()) < 0) {' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'private ParcelFileDescriptor openFileAndEnforcePathPermissionsHelper(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'checkAccess(uri, Bundle.EMPTY, file, forWrite);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (isPending && !isPendingFromFuse(file)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ParcelFileDescriptor lowerFsFd = FileUtils.openSafely(file, modeBits);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'daemon.shouldOpenWithFuse(filePath, true /* forRead */, lowerFsFd.getFd());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'fl.l_type = for_read ? F_RDLCK : F_WRLCK;' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'return ParcelFileDescriptor.wrap(pfd, BackgroundThread.getHandler(), listener);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final ParcelFileDescriptor pfd = ensureThumbnail(uri, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (shouldBypassDatabaseForFuse(uid)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.beginTransaction();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!updateDatabaseForFuseRename(helper, oldPath, newPath,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (hasFullAccessToNewPath && hasFullAccessToOldPath) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'int errno = renameInLowerFs(oldPath, newPath);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.setTransactionSuccessful();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.endTransaction();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'values.put(FileColumns.OWNER_PACKAGE_NAME, "null");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'scanRenamedDirectoryForFuse(oldPath, newPath);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (CheckForJniException(env)) {' packages/providers/MediaProvider/jni/MediaProviderWrapper.cpp
grep -n -F 'if (res == 0) {' packages/providers/MediaProvider/jni/FuseDaemon.cpp
```

构造 `File.delete()` 返回 false、权限检查抛异常、`deletedata=false`、有 child 的目录 row、FUSE 重复 delete 五种情况，分别记录 lower file、row、count、dentry cache 与通知。

## 13. 普通openFile从row解析路径；thumbnail分支会提前短路

`openFileCommon()` 的四个 legacy thumbnail redirect 直接进入 `ensureThumbnail()`，普通 item 才进入 `openFileAndEnforcePathPermissionsHelper()`。在普通路径中，Provider 暂时清调用身份查询 `_data`、owner 与 pending，把 `_data` canonicalize 成 `File`；恢复身份后才执行 `checkAccess()`。内部能查到 row 与外部有权打开文件是两个不同结论。framework transport 仍有 URI-level file-permission enforcement，但不能把它等同于 MediaProvider 内部的 row/path `checkAccess()` 链。

write-only 模式会补上 read-write 位，使后续 `shouldOpenWithFuse(..., true /* forRead */, ...)` 能取得 native 的 `F_RDLCK`；它不是额外取得 write lock。非 FUSE 物理名的 pending row 还要求 item ownership；所谓“FUSE pending”并没有单独 provenance 列，而是由文件名不匹配 expiration pattern 推断，因此该分支仍要依赖前面的 URI/path 权限检查。

非 owner 且缺少原始位置访问能力时，Provider 计算 EXIF/XMP redaction ranges；URI 要求 original 则拒绝。启用新 FUSE 时通过 upper path 让 FUSE handler 脱敏，旧实现使用 redacting descriptor。

无需脱敏时，Provider 先用转换后的 open flags 安全打开 lower FD，这一步的 create/truncate 已可能改变 lower file；然后才询问对应 daemon 是否已有必须保持一致的 upper VFS cache。需要时改开 upper FD 并关闭 lower；否则保留 lower，且 writable lower FD 会在返给调用者前 invalidate dentry。所以 invalidation 早于调用者后续写入，却不一定早于 open-time create/truncate；这是缓存一致性选择，不是额外授予读权限。

`openTypedAssetFileCommon()` 的 underlying-file fallback 最终进入 `openFileCommon()`，会执行普通权限链；但它请求 thumbnail 时直接进入 `ensureThumbnail()`，r48 源码本身标记 URI access enforcement 尚未补齐。`openFileCommon()` 的四个 legacy thumbnail redirect 也同样不进入 path helper。不能用 fallback 的内部校验替这些 thumbnail 分支作保证。

## 14. close后的扫描与FUSE rename都不是一个单一同步完成点

非 pending 写 FD 会用 `ParcelFileDescriptor.wrap()` 挂到 BackgroundThread handler。close listener 无论远端 writer 是否报告异常，都会失效 thumbnail 与 dentry；普通媒体按需扫描，legacy thumbnail 则直接更新宽高。因为 listener 运行在后台 handler，调用者的 `close()` 不是 metadata scan 完成屏障。

pending 写 FD 不挂 listener，避免生产阶段每次 close 都扫描；发布时由 update 的 blocking scan 收敛。pending 是 open 时的快照：若 FD 尚未关闭就先发布，发布 scan 可能读到半成品，而这个早先打开的 FD 关闭时也没有 listener 再补扫。只读 open 同样不挂写后扫描。

排除 native 层已拒绝/短路的 flags、parent node、路径可访问性和 no-op，以及 ROOT 在 wrapper 中直接 lower rename 的 FS-only 快路后，进入 Java `renameForFuse()` 的非 ROOT 请求才共同先过两组检查：新旧端不能进入其他包私有路径，且新路径必须等于清洗后的绝对路径。manager 等 database-bypass 身份随后就直接 lower rename，跳过数据库更新，也跳过后面默认目录、存储根与 `Android` 目录约束；这些 FS-only 调用本身不安排扫描或通知，只有其他原因触发的未来扫描才可能收敛数据库。

非 database-bypass 中，新旧两端都具备 FUSE restriction bypass 的调用者立即走 unchecked 数据库协调，同样位于后续目录约束之前。普通 checked 路径先按调用身份拒绝 legacy caller，再由 old/new relative path 分别检查默认顶级目录不可改名、目标不可位于存储根；随后执行 `Android` 区域限制。该区域在 r48 以 `Environment.getExternalStorageDirectory()` 的 primary external root 构造，不能外推为对所有可移除卷的同等检查。checked file rename 还验证新路径支持 MIME。checked directory rename 的 full-access 快路按 relative-path 字符串 `startsWith(defaultDir)` 判断，并非目录段边界检查，因而默认目录的同前缀路径也可命中；命中后直接取所有已索引且 MIME 非 null 的文件。只有慢路才比较全部数与 TYPE_UPDATE 可见数，并逐项验证新路径 MIME。两条都不会审计未索引实体或 MIME 为 null 的目录 row，但 lower directory rename 会把整棵真实树一起移动。

受管 rename 的共同框架是：开启 helper 事务 → 尝试协调 row → lower `Os.rename()` → 成功才 `setTransactionSuccessful()` → 结束事务。checked 单文件必须成功更新 source row；directory 必须成功更新列表中的每个 indexed file；unchecked 单文件则可在 source update 失败后走下一段兜底。constraint 处理会尝试删除 caller 可写的冲突 row 并重试。lower rename 失败会回滚 row；但 lower 已成功而进程在 commit 前终止时，文件仍可能移动而 row 回滚。

unchecked 单文件还有一个不能并入上述成功路径的分支：只要 `updateDatabaseForFuseRename()` 返回 false——可以是 constraint 无法解决，也可以是源 path 没有命中一行——bypass caller 就转而调用 `maybeRemoveOwnerPackageForFuseRename()`。若 caller-filtered query 能看见目标 other-owner row，该 helper 尝试把 owner 写成字面字符串 `"null"`；查询未命中——包括 row 不存在、对 caller 不可见或 owner 已是该字符串——以及 row 本就属于 caller 时，helper 也可直接返回 true。之后仍执行 lower rename，不会重试把源 row 更新到新 path。这里的 `"null"` 不是 SQL `NULL`。目录 unchecked 没有同样的 owner-clear 兜底，任一 indexed file 更新失败就返回权限错误。

目录事务只批量改已索引文件 row，不改目录 row；commit 后同步扫描 old/new path，清旧目录索引并重建 hidden/media-type 状态。单文件涉及 `.nomedia` 时则在 commit 后同步扫描对应父目录。这些扫描位于 Java rename 返回之前；若它们或其他 commit 后 Java 代码抛异常，JNI 会转为 `EFAULT`，此时 lower FS 与 DB 可能都已成功，native node 却因非零结果没有执行 `Rename()`。因此非零不能反推 lower 未移动或 DB 已回滚；反过来，返回 0 也不能证明每个扫描候选 mutation 都已完全对账。

## 15. files trigger在事务内触发；只有部分副作用受commit门保护

`files_insert`、`files_update`、`files_delete` 是 SQLite `AFTER` trigger，调用 `_INSERT/_UPDATE/_DELETE` 自定义函数。函数在执行 mutation 的线程、数据库事务尚未结束时进入 `mFilesListener`。这使所有 builder、scanner 与受管 FUSE rename 共用同一分发入口。

listener 里的动作要再分两类：

- `handleInsertedRowForFuse()`、`handleUpdatedRowForFuse()`、`handleDeletedRowForFuse()` 与 owner cache invalidation 当场执行。若 lower rename 随后失败、SQLite 回滚，这些内存 cache 变化不会随数据库回滚。
- `notifyInsert/Update/Delete()` 在存在显式 helper `TransactionState` 时只写入 `notifyChanges`；quota、URI revoke、thumbnail、SAF 等经 `postBackground()` 收集。事务未成功时，这两组都随 transaction state 丢弃。“commit-gated”指的是这种显式事务状态，不是任意 SQLite 隐式语句事务。

成功结束时，`endTransactionInternal()` 先移除 transaction state、结束 SQLite transaction 并释放 schema read lock，再同步逐项运行 blocking tasks。只有它们全部正常返回，才投递 foreground runnable；该 runnable 又要先完成所有 `notifyChange()`，才把 background tasks 逐个提交给 BackgroundThread。这里保证的是**无异常路径上的阶段顺序**，不是“DB 已提交就必然完成全部分发”：blocking task 抛错可留下已提交 row，却截断剩余 blocking task 并阻止通知/background runnable 投递；foreground 通知循环抛错则会阻止其后通知与 background submission。它更不保证远端 observer 已在后台任务开始前消费通知。

URI 会扩展：audio/image/video 的 typed item URI、generic Files item URI、必要时的 Downloads item URI，以及具体外部卷对应的 synthetic external item URI。audio 还使 genre、playlist、artist、album 聚合 collection URI 失效。update 改变 media type 时，旧类型与新类型都通知；同一事务、同一 flag 下用 `ArraySet` 去重。

独立于 helper 事务调用 `notifyChange()` 时，只向 ForegroundThread 投递单 URI；独立 `postBlocking()` 立即运行，独立 `postBackground()` 立即提交后台。legacy FUSE ownership transfer 是重要例外：`updateOwnerForPath()` 通过 `runWithoutTransaction()` 直接 `db.update()`，trigger 执行时没有 `TransactionState`，通知与 background work 会立即投递，可能早于该隐式语句事务提交，回滚也无法撤回。因此分析顺序前必须先问“当前线程此刻是否持有 helper transaction”。

### 练习 9：证明trigger、回滚与三阶段分发的精确边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'db.execSQL("CREATE TRIGGER files_insert AFTER INSERT ON files"' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'db.execSQL("CREATE TRIGGER files_update AFTER UPDATE ON files"' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'db.execSQL("CREATE TRIGGER files_delete AFTER DELETE ON files"' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'mFilesListener.onUpdate(DatabaseHelper.this, volumeName, oldId,' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'handleUpdatedRowForFuse(oldPath, oldOwnerPackage, oldId, newId);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return helper.runWithoutTransaction((db) -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'acceptWithExpansion(helper::notifyDelete, volumeName, id, mediaType, isDownload);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final TransactionState state = mTransactionState.get();' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'state.notifyChanges.put(flags, set);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'state.blockingTasks.add(command);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'state.backgroundTasks.add(command);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'db.endTransaction();' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'if (state.successful) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'state.blockingTasks.get(i).run();' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'ForegroundThread.getExecutor().execute(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'notifyChangeInternal(state.notifyChanges.valueAt(i),' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'BackgroundThread.getExecutor().execute(state.backgroundTasks.get(i));' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'acceptWithExpansion(consumer, MediaStore.VOLUME_EXTERNAL,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (newMediaType != oldMediaType) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

为 commit 与 rollback 各画一次时序：FUSE identity cache、SQLite row、observer URI、quota、thumbnail 与 URI revoke 分别在哪一刻生效或被丢弃。

## 16. 用操作矩阵定位裂缝，再把query边界留给下一章

排查“返回成功但相册不对”“改名后出现双路径”“删除后文件又回来”，先用这张矩阵，不要从单一返回值倒推全链：

| 操作 | 文件系统先发生什么 | 数据库何时提交 | 元数据收敛 | 主要裂缝 |
|---|---|---|---|---|
| 普通 insert | 可能先 `mkdirs()`；通常无目标字节 | path/parent row 事务 | 后续写 close 或 publish scan | 空目录已建但 row 失败；row 有而 file 尚无 |
| ContentProvider movement | 路径计算可先 `mkdirs()`，再 `Os.rename()`；ENOENT 可继续 | rename 之后 update 事务 | 按 affected id 扫描 | 空目录残留；FS 新、DB 旧；或 DB 新、file 缺失 |
| 普通 delete | 先 best-effort `File.delete()` | 常见为逐 id row transaction | 孤儿由后续 scan 收敛 | unlink 失败仍删 row；unlink 成功后 row 失败 |
| FUSE unlink | ROOT/database-bypass 可 FS-only；受管路径调 Provider delete；零行时可再 FS-only 兜底 | 只有受管 row-delete 改 DB | 这个调用无统一扫描阶段 | file 仍在却报成功；或 file 已无而 row 未改 |
| 受管 FUSE rename | 先尝试协调 row，再 lower rename | lower 成功才标记事务成功 | directory 或 `.nomedia` 可在 commit 后同步扫描 | unchecked 可无 source-row update；commit 后扫描报错又可返回非零 |
| ROOT/native 或 Java database-bypass rename | 只改 lower filesystem | 本次不改 row | 本调用不安排扫描 | 立即出现 path/row 分叉 |
| native `rmdir` | 授权后直接删 lower 目录 | 本次不改 row | 本调用不安排扫描 | 目录实体与索引可分叉 |
| 非pending URI写入 | open 时选择 upper/lower，调用者写字节 | 本次 open 不保证 row metadata 更新 | close listener 在后台扫描 | close 已返回，metadata/通知仍在路上 |

建议采集的证据也分层：

1. 调用 URI、calling package/uid、target SDK、是否 FUSE thread、是否处于 outer batch transaction；
2. 请求与最终 `DISPLAY_NAME`、`RELATIVE_PATH`、MIME、owner、pending/trashed、canonical `_data`；
3. rename/delete 前后两端 `stat`，不能只看 ContentProvider count；
4. 精确 id 与 `_data` row、generation、media type、download flag；
5. scanner 的需求原因与完成日志，写 close 和 publish 要分别观察；
6. 具体卷与 synthetic external 的 observer URI，以及 thumbnail、quota、SAF、grant、DownloadManager 后台结果。

整条链可以压成一句话：**URI collection 和调用身份决定放置政策，现代列生成候选路径，显式 helper 事务提交 row 并在无异常路径上按阶段分发副作用；真实文件却由 mkdir、FD、unlink、两种不同顺序的 rename 单独推进，扫描器负责缩短而不是消灭它们之间的窗口。**

下一章转向读取面：MediaProvider query 如何选择 SQLiteQueryBuilder、校验 projection/selection、按 owner 与权限过滤 pending/trashed，canonical URI 又怎样影响 item 定位与授权。
