# 274 Android MediaProvider数据库、volume attach、ModernMediaScanner、文件与row一致性及过期清理链

## 1. 先看结论：一致性不是一个瞬间，而是五个可分别失败的完成点

本文以 Android 11 `android-11.0.0_r48` 为边界：`frameworks/base` 位于 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/providers/MediaProvider` 位于 `47c141d93e93b25cc85c36f3579fda25a1695952`。这一版的 MediaProvider 不能用“磁盘和数据库最终总会一样”概括；更准确的模型是五个依次推进、却没有共同原子提交的完成点：

1. **卷可见**：`StorageManager` 报告卷处于 mounted 或 mounted-read-only，缓存能解析卷名、根目录和扫描路径。
2. **卷已 attach**：具体卷名进入 `mAttachedVolumeNames`，对应 URI 才能路由到数据库 helper。
3. **文件已遍历**：扫描器访问目录树，判断隐藏性、MIME、媒体类型和是否需要更新。
4. **row 已提交**：批量 `insert`/`update`/`delete` 真正经过 MediaProvider 和 SQLite；排队计数不等于提交计数。
5. **旧 row 已对账**：扫描快照之外的候选行经过 generation、pending 等条件过滤后被清理；这一步只要求删除数据库行，不要求删除磁盘文件。

日常维护又在这些完成点之上运行：它重新扫描当前卷、核对缩略图、清 owner、删除到期项、遗忘长期不再出现的卷，最后才记录指标。任何一个广播、返回值或计数都只覆盖其中一段。

| 观察 | 能证明什么 | 不能证明什么 |
|---|---|---|
| `attachVolume()` 返回 | 卷名已进入 attached 集合，通知已发出 | 默认目录、缩略图 UUID、Documents roots 已完成 |
| `MEDIA_SCANNER_FINISHED` | external 扫描的外层 `finally` 已执行 | 所有路径扫描成功、scanner 状态已清空 |
| scan 指标中的 insert/update/delete | 操作曾加入 pending 队列 | 每项都成功提交 |
| reconcile 的 `PARAM_DELETE_DATA=false` | 不走普通物理文件删除分支 | 没有通知、授权撤销或缩略图失效副作用 |
| idle 的 expired 数 | 查询游标命中的候选行数 | 同样数量的磁盘文件已删除 |

## 2. 数据库不是“一卷一库”：两个helper、共享external.db与事务epoch

`MediaProvider.onCreate()` 构造一个 `ModernMediaScanner`，随后只构造 `internal.db` 和 `external.db` 两个 `DatabaseHelper`。所有具体外部卷的 row 都进入同一个 `external.db`，由 `volume_name` 区分。`isMediaDatabaseName()` 仍接受 `external-*.db`，这是兼容或迁移识别范围，不能反推当前 attach 会为每个卷新建数据库。

`files` 是宽表，`_data` 使用 `UNIQUE COLLATE NOCASE`；目录也能拥有 row，只是目录的 MIME 为 `null`。因此 `getItemCount()` 明确只统计 `mime_type IS NOT NULL` 的真实媒体项，而不是简单统计整张表。

`DatabaseHelper` 故意禁用普通的 `getReadableDatabase()` 与 `getWritableDatabase()` 调用。业务操作必须经过 `runWithTransaction()` 或 `runWithoutTransaction()`，这样 schema 读锁、SQLite 事务和提交后的通知任务才处在同一套纪律中。WAL 打开并不取消这套锁语义。

最容易误读的是 generation：`beginTransactionInternal()` 在 SQLite 事务开始后立即把 `local_metadata.generation` 加一。事务若回滚，这次加一也回滚；事务若成功，即使没有 row 改动，generation 仍可能前进。`runWithTransaction()` 在同一线程已有事务时直接复用它，所以一组嵌套 helper 调用共用同一个 epoch；直接再次 `beginTransaction()` 才会抛异常。它是“成功事务序号”，不是 row 数、文件数或扫描次数。

### 练习 1：证明数据库拓扑与generation的真实粒度

在源码根目录运行；也可把源码根目录作为第一个参数，从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mMediaScanner = new ModernMediaScanner(context);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mInternalDatabase = new DatabaseHelper(context, INTERNAL_DATABASE_NAME,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mExternalDatabase = new DatabaseHelper(context, EXTERNAL_DATABASE_NAME,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'static final String INTERNAL_DATABASE_NAME = "internal.db";' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'static final String EXTERNAL_DATABASE_NAME = "external.db";' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'if (name.startsWith("external-") && name.endsWith(".db")) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public SQLiteDatabase getReadableDatabase() {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'public SQLiteDatabase getWritableDatabase() {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'setWriteAheadLoggingEnabled(true);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F '_data TEXT UNIQUE COLLATE NOCASE' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'SELECT COUNT(_id) FROM files WHERE ' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'db.execSQL("UPDATE local_metadata SET generation=generation+1;");' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'if (mTransactionState.get() != null) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'return op.apply(db);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
```

读完应能回答：两个具体 SD 卡是否共享 generation？答案是共享，因为它们都路由到同一个 external helper；一次成功的只读式 `runWithTransaction()` 是否可能让 generation 前进？答案也是可能。

## 3. 卷名同时参与路由与过滤，但两条链不是同一件事

`external_primary`、UUID 形式的卷名是**具体卷**；`external` 是合成卷，代表当前外部卷的合并视图。数据库路由先把合成 `external` 解析成 `external_primary`，再检查这个具体名是否 attached，最后选择 internal 或 external helper。于是合成 external 查询仍以 primary attached 为入口条件。

查询行集则走另一条链：`getQueryBuilder()` 遇到合成 external，会把 `includeVolumes` 展开为当前缓存中的所有外部卷名；具体卷则只绑定自身。Images、Video、Audio media、Files、Downloads 等普通集合在运行时追加 `volume_name IN (...)`。

不要把这个事实泛化成“所有 SQL view 都内置卷过滤”。常规 media view 本身没有当前卷谓词；只有 `audio_artists`、`audio_albums`、`audio_genres` 这类聚合 view 把 `mFilterVolumeNames` 烘焙进定义。`updateVolumes()` 先同步刷新静态卷名、路径、扫描路径和 path-to-id 缓存，再把聚合 view 的重建异步投递给 `ForegroundThread`。因此普通 row 查询可能已经看到新缓存，而聚合 view 仍在等待重建。

`MediaStore.getExternalVolumeNames()` 只收集 mounted 与 mounted-read-only 且具有 MediaStore 卷名的卷。它回答“当前卷”，不是 attached 集合，也不是 recent 历史集合。

### 练习 2：沿着external同时追路由门和行集过滤

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static @NonNull String resolveVolumeName(@NonNull Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return MediaStore.VOLUME_EXTERNAL_PRIMARY;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!mAttachedVolumeNames.contains(volumeName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return mExternalDatabase;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'includeVolumes = bindList(getExternalVolumeNames().toArray());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, FileColumns.VOLUME_NAME + " IN " + includeVolumes);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mExternalDatabase.setFilterVolumeNames(getExternalVolumeNames());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'filterVolumeNames = bindList(mFilterVolumeNames.toArray());' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'WHERE is_music=1 AND volume_name IN ' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'WHERE volume_name IN ' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'case Environment.MEDIA_MOUNTED_READ_ONLY: {' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
grep -n -F 'res.add(volumeName);' packages/providers/MediaProvider/apex/framework/java/android/provider/MediaStore.java
```

排障时应把四种状态分别记录：StorageManager 当前卷缓存、attached 集合、运行时 query filter、聚合 view filter。它们通常趋同，却不由一个同步临界区一次性切换。

## 4. attach先开路由门，再异步准备目录、缩略图与Documents roots

进程启动时，MediaProvider 注册 `StorageVolumeCallback`，调用 `updateVolumes()`，然后 attach internal 和当时所有当前外部卷。后续状态变化先触发缓存刷新；真正的 attach 也可由 `MediaService` 在卷扫描前调用。

`attachVolume()` 只允许本进程身份调用，校验卷名；`validate=true` 时还要能解析物理路径。它先把卷名加入 `mAttachedVolumeNames`，再通知具体卷 URI；外部卷还通知合成 external URI。到这里方法就已经拥有可返回的 URI。

这里也没有“已经 attach 就直接返回”的幂等快路：`ArraySet.add()` 的返回值被忽略，重复调用仍会重复 notify；重复 external attach 还会再次投递后续准备任务。

外部卷的后续准备被投递到前台执行器：在 external 数据库事务中调用 `ensureDefaultFolders()` 和 `ensureThumbnailsValid()`，事务结束后再调用 `MediaDocumentsProvider.onMediaStoreReady()`。所以 attach 返回不是这些工作的屏障。

默认目录只在偏好键未置位时主动创建：primary 使用 `created_default_folders`，其他卷使用带卷名的键。用户日后手动删除目录，不会因为同一键仍在就每次 attach 都重建。实现没有检查 `mkdirs()` 的返回值，目录已存在时也不会在这一步补 row，最后仍可能提交偏好键；因此这是 best-effort 初始化，后续扫描仍承担收敛职责。缩略图则用数据库文件 `user.uuid` xattr 与卷上 `.database_uuid` 配对；所有具体外部卷共享 external.db 的 UUID。缺少标记时写入当前 UUID，不匹配时尝试清空缩略图树再改写标记。

Documents 的 ready 更要收窄理解：Android 11 这里是一个进程级静态布尔值，传入的 `volumeName` 并未形成逐卷 readiness；它表示底层 provider 已经可以回答 roots，既不表示该卷扫描完成，也不表示所有卷准备完成。

MediaService 在 attach 返回后就能继续写 scanner 状态并遍历目录，不等待这项异步准备；detach 也不会撤销已经排队或正在运行的准备任务。detach 自身的顺序是：通知扫描器取消该卷当前 signal，移除 attached 名，再通知具体卷与合成 external。它不关闭共享 external.db，也不在这里刷新四组 volume 缓存或重建聚合 view；这些由独立的 volume-state/update 链推进，`updateVolumes()` 本身也不会替代 attach/detach。

### 练习 3：给attach、异步准备和detach分别找完成点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mStorageManager.registerStorageVolumeCallback(context.getMainExecutor(),' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'updateVolumes();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'attachVolume(MediaStore.VOLUME_INTERNAL, /* validate */ false);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public Uri attachVolume(String volume, boolean validate) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mCallingIdentity.get().pid != android.os.Process.myPid()' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'getVolumePath(volume);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mAttachedVolumeNames.add(volume);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'resolver.notifyChange(getBaseContentUri(MediaStore.VOLUME_EXTERNAL), null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ForegroundThread.getExecutor().execute(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ensureDefaultFolders(volume, db);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ensureThumbnailsValid(volume, db);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaDocumentsProvider.onMediaStoreReady(getContext(), volume);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'key = "created_default_folders_" + vol.getMediaStoreVolumeName();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'else if (!Objects.equals(uuidFromDatabase, uuidFromDisk.get())) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mMediaScanner.onDetachVolume(volume);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mAttachedVolumeNames.remove(volume);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private static volatile boolean sMediaStoreReady = false;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
grep -n -F 'sMediaStoreReady = true;' packages/providers/MediaProvider/src/com/android/providers/media/MediaDocumentsProvider.java
```

## 5. MediaService编排扫描；状态URI与广播都不是成功事务

`MediaService` 是 `JobIntentService`，处理按需单文件扫描和卷挂载扫描。外部卷扫描在发 STARTED 广播前，先递归扫描 internal，再确保默认铃声；因此广播并不包住全部准备工作。它还提前解析一次 broadcast URI，以便卷在中途弹出时仍能发对应事件。

随后服务取得本地 MediaProvider、以 `validate=true` attach 目标卷，向 `media_scanner` URI insert 当前卷名。这个 URI 没有写一张持久表：MediaProvider 只是把名称放进单个 `mMediaScannerVolume` 字段、记录 helper 的 start time；query 返回的是 `MatrixCursor`。正常路径遍历每个 scan path 后 delete 这个 URI，MediaProvider 才记录 stop time 并清空字段。

这个状态入口没有 per-scan token、卷名匹配或引用计数，只是单槽 best-effort 活动指示。若两个调用重叠，B 的 insert 会覆盖 A；A 随后的 delete 会按槽中当前卷记录 stop 并清掉 B，B 尚在工作时 query 就可能返回 null，B 最后的 delete 再返回 0。共享 external helper 上的 start/stop time 也不是逐卷并发账本。

异常边界决定了不能把它叫作事务：`resolver.delete(scanUri, ...)` 位于 `try` 正常路径，不在 `finally`。扫描、批处理之后若抛出未被内部吞掉的异常，状态字段可能残留；外层 `finally` 仍会发 external 的 FINISHED 广播。甚至 attach、状态 insert 或 STARTED 发送阶段的异常，只要已经进入这一层 `try`，也会走 FINISHED。FINISHED 因而表示编排作用域退出，不表示扫描成功。

按需 `ACTION_MEDIA_SCANNER_SCAN_FILE` 更短：canonicalize 文件后直接调用 `provider.scanFile()`，不走卷状态 URI，也没有卷 STARTED/FINISHED 广播。

### 练习 4：制造“FINISHED已发、scanner状态未清”的反例

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public class MediaService extends JobIntentService {' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'case Intent.ACTION_MEDIA_SCANNER_SCAN_FILE: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'case Intent.ACTION_MEDIA_MOUNTED: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'onScanVolume(context, MediaStore.VOLUME_INTERNAL, reason);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'RingtoneManager.ensureDefaultRingtones(context);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'provider.attachVolume(volumeName, /* validate */ true);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'values.put(MediaStore.MEDIA_SCANNER_VOLUME, volumeName);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'Uri scanUri = resolver.insert(MediaStore.getMediaScannerUri(), values);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'new Intent(Intent.ACTION_MEDIA_SCANNER_STARTED, broadcastUri)' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'for (File dir : FileUtils.getVolumeScanPaths(context, volumeName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'resolver.delete(scanUri, null, null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'new Intent(Intent.ACTION_MEDIA_SCANNER_FINISHED, broadcastUri)' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
grep -n -F 'mMediaScannerVolume = initialValues.getAsString(MediaStore.MEDIA_SCANNER_VOLUME);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.mScanStartTime = SystemClock.elapsedRealtime();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.mScanStopTime = SystemClock.elapsedRealtime();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mMediaScannerVolume = null;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'c.addRow(new String[] {mMediaScannerVolume});' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return provider.scanFile(file, REASON_DEMAND);' packages/providers/MediaProvider/src/com/android/providers/media/MediaService.java
```

## 6. 一个Scan抓取起始快照；取消与目录锁都只是协作式边界

每次 API 调用创建一个 `Scan` 对象，持有本地 provider client、root、卷名、Files URI、owner、`mStartGeneration` 与 `mSingleFile`。后者来自 `mRoot.isFile()`，不是来自“调用的方法名”：把目录传给 `scanFile()` 仍会采用目录扫描语义。只有 `mSingleFile && mScannedIds.size() == 1` 才跳过 reconcile。

同一卷的多个 Scan 从 `mSignals` 取得同一个 `CancellationSignal`。正常扫描结束不会把它移除；detach 才 remove 并 cancel 当时的 signal。若 detach 发生在 Scan 构造之前，它不是一个会永久拒绝未来扫描的闩锁。扫描在遍历、查询和主要阶段显式检查 signal，但 metadata retriever、Exif 解析、`applyBatch()` 等内部过程没有逐项取消点；detach 只是请求尽快停止，不会 join 到扫描线程退出。

scanner 的公开 `scanDirectory()`/`scanFile()` 会吞掉 `OperationCanceledException`，分别安静返回或返回 null。对卷编排而言，这次取消可能因此表现成一次正常方法返回，最后 MediaService 仍可能清状态并发 FINISHED。更微妙的是 detach 先从 map 移除旧 signal 再 cancel；若编排还有后续 scan path，新建 Scan 会取得 fresh signal，不会继承旧 signal 的 canceled 状态，能否继续则再受 attached gate 与后续访问约束。取消既不是持久闩锁，“流程正常收尾”也不等于目录完整扫描。

并发控制按**精确目录 Path** 建立引用计数锁。单文件扫描锁 parent；目录遍历在 `preVisitDirectory()` 加锁，在 `postVisitDirectory()` 先 flush 该目录相关 pending，再解锁。它不会把整卷串行化，也不覆盖之后的 reconcile 与 playlist 阶段。

还有一个异常清理陷阱：`Scan.close()` 先检查 pending 是否为空；若非空会立即抛异常，后面的遗留锁释放与 client close 根本不会执行。只有 pending 已排空时，close 才能补释放异常路径遗留的锁。不能把 `try-with-resources` 写成无条件清理保证。

### 练习 5：标出取消、锁和close都没有覆盖的区间

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'try (Scan scan = new Scan(file, reason, /*ownerPackage*/ null)) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'public void onDetachVolume(String volumeName) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'final CancellationSignal signal = mSignals.remove(volumeName);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'signal.cancel();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mSignal = getOrCreateSignal(mVolumeName);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mStartGeneration = MediaStore.getGeneration(mResolver, mVolumeName);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mSingleFile = mRoot.isFile();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (mSingleFile && mScannedIds.size() == 1) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'acquireDirectoryLock(mRoot.getParentFile().toPath());' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'lock = mDirectoryLocks.get(dir);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'applyPending();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'releaseDirectoryLock(dir);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (!mPending.isEmpty()) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'for (Path dir : new ArraySet<>(mAcquiredDirectoryLocks)) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mClient.close();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
```

## 7. 路径策略分“跳过子树”和“隐藏媒体”，普通.nomedia不等于不遍历

扫描开始先沿 root 的父链调用 `shouldScanPathAndIsPathHidden()`，同时计算“可扫描”和“祖先是否隐藏”。两组正则提供硬边界：存储根及 sandbox 根是强制可见点，会尝试删除异常 `.nomedia`；`Android/data`、`Android/obb` 以及 Movies/Music/Pictures 下的 `.thumbnails` 是强制不可扫描点，会尝试创建 `.nomedia` 并 `SKIP_SUBTREE`。

普通目录中的 `.nomedia` 走另一套语义。`FileUtils.isDirectoryHidden()` 把点目录或含 `.nomedia` 的目录标成 hidden；遍历仍会进入它，`mHiddenDirCount` 随层级增减，文件被归类为 `MEDIA_TYPE_NONE`。`.nomedia` 文件自身被 `scanItem()` 忽略，但其他文件仍可留在 `files` 表中。顶层默认媒体目录和 `DCIM/Camera` 又是强制可见例外，会删除其中的 `.nomedia`。

目录本身也经过 `visitFile()`，以 MIME `null` 建立 parent row。之后遇到现有目录 row 时，无论 mtime/size 是否变化，代码都会在记下 scanned id 后跳过 metadata update。这解释了为什么“目录存在于 files 表”和“目录是一条媒体项”不是同一句话。

## 8. 单文件决策先记seen，再决定skip或upsert

`visitFile()` 先区分目录与文件、解析 MIME；DRM MIME 会向 `DrmManagerClient` 询问原始类型。媒体类型同时受路径、MIME 和 `mHiddenDirCount` 影响。随后用精确 `_data=?` 查询现有 row，并显式包含 pending、trashed、favorite，避免默认过滤让已存在项看起来像新文件。

如果找到 row，扫描器会**先**把 id 放进 `mScannedIds` 并可能设为 first result，再比较 mtime、size、MIME（忽略大小写）和 media type。四项相同且不是 FUSE pending 才算 unchanged；目录则直接跳过。这个顺序意味着后续 update 即使失败，旧 id 仍被当作本次见过，不会在同次 reconcile 中删除。mtime 也并非所有根都直接取文件属性：位于 `Environment.getStorageDirectory()` 之外的只读分区使用 `Build.TIME`。

FUSE pending 的识别是组合条件：物理文件名不匹配 `.pending|trashed-时间戳-原名`，同时 DB 的 `is_pending` 非零。它会强制走 update，但是否发布还取决于调用线程：常规 MediaService、idle 或 Binder 需求扫描经过 `computeValuesFromData(..., false)`，会把没有隐藏命名模式的 pending 清零；FUSE 目录 rename 后也会同步触发扫描，此时 `isFuseThread()` 仍可为 true，显式 pending 会保留。只有非 FUSE 路径的 update 真正提交成功，才能说完成发布。

新项走 insert，旧项走带 `_id` 的 update。只有“新建、非目录、调用方提供 owner”才补 `OWNER_PACKAGE_NAME`；DRM 再强制写 `IS_DRM=1`。generic 层先把一批常见字段清空；audio/video 再走 retriever 与 XMP，image 则走 Exif 与 XMP。这能降低旧 metadata 残留，却不是清空整张宽表的证明：例如部分图片专有字段和 TRACK 只在 Optional 有值时覆盖，缺失时仍需逐列审计。

### 练习 6：用一个隐藏FUSE pending文件手推visitFile

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final Pattern PATTERN_VISIBLE = Pattern.compile(' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'private static final Pattern PATTERN_INVISIBLE = Pattern.compile(' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'nomedia.delete();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'nomedia.createNewFile();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'isPathHidden = isPathHidden || FileUtils.isDirectoryHidden(dir);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (attrs.isDirectory()) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'actualMimeType = mDrmClient.getOriginalMimeType(realFile.getPath());' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F '/*isHidden*/ mHiddenDirCount > 0);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'queryArgs.putInt(MediaStore.QUERY_ARG_MATCH_PENDING, MediaStore.MATCH_INCLUDE);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mScannedIds.add(existingId);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'final boolean sameTime = (lastModifiedTime(realFile, attrs) == dateModified);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F '&& !isPendingFromFuse;' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'op = scanItem(existingId, realFile, attrs, actualMimeType, actualMediaType,' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (op.build().isInsert() && !attrs.isDirectory() && mOwnerPackage != null) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'op.withValue(MediaColumns.IS_DRM, 1);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'op.withValue(MediaColumns.DATE_TAKEN, null);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'final ExifInterface exif = new ExifInterface(is);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'withRetrieverValues(op, mmr, mimeType);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (FileUtils.contains(Environment.getStorageDirectory(), file)) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (PATTERN_EXPIRES_FILE.matcher(name).matches()) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'FileUtils.computeValuesFromData(values, isFuseThread());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 9. batch是吞吐边界，不是“全批成功”边界

`BATCH_SIZE` 为 32，但 flush 条件是 `mPending.size() > BATCH_SIZE`，所以自然积累的一批通常能到 33。`addPending()` 在执行前就增加 insert/update/delete 指标，指标描述排队意图，不是数据库确认结果。

扫描 upsert 由 `newUpsert()` 创建，并设置 `withExceptionAllowed(true)`。因此 applyBatch 返回后可以逐项查看 `ContentProviderResult.exception`；只有带成功 `result.uri` 的 insert 才把新 id 加入 scanned IDs。既有 row 的 id 早在 update 排队前已加入，不能用 scanned IDs 反推 update 成功。

reconcile 的 delete 则没有设置 `withExceptionAllowed(true)`。某个 clean delete 失败可能让整个 provider batch 事务回滚并抛 `OperationApplicationException`；外层只记录错误，`finally` 仍无条件清空 pending。upsert 的单项容错不能推广给 clean delete。

`mFirstId` 同样不是提交收据：既有 row 在比较阶段即可占据它；成功 insert 的 URI 也可设置它。`getFirstResult()` 再查询 media type，映射为 audio/video/image/playlist URI，最坏返回 generic Files URI。若业务需要证明内容已落库，应在 scan 返回后重新 query 所需字段，而不是只检查非空 URI。

## 10. reconcile用start generation保护新row，但不封住所有并发竞争

目录扫描完成后，扫描器先复制并排序 `mScannedIds`，再查询 root 本身及其子路径。候选排除没有磁盘文件的 abstract playlist，默认排除 pending，同时包含 trashed 与 favorite；最关键的条件是 `generation_added <= mStartGeneration`。

这个 generation 条件只保护“扫描开始后新插入的 row”：它们的 `generation_added` 更大，不会因未出现在本次目录快照而被清理。它不保护扫描开始前已有、期间被 update 或 move 的 row，因为 update 只改 `generation_modified`；也不是文件系统事务日志。目录锁此时已经释放，更不能替代并发协议。

查询 unknown IDs 与删除分成两阶段，使分页或游标不会被边查边删扰动。两阶段之间不会重新 `stat`：若一个旧 row 对应的文件恰在 query 后重新出现，row 仍可能被删，留给下一次 scan 再补。`visitFileFailed()` 也只是记录错误并继续，所以瞬时不可访问的文件没有进入 seen 集合时，后续对账可能清掉它的 row。

clean URI追加 `PARAM_DELETE_DATA=false`，因此 MediaProvider 跳过普通物理文件删除分支，只删除 row；但 files delete trigger/listener 仍可安排通知、URI 授权撤销和缩略图失效。所谓“row-only”只限定 unlink，不等于零副作用。

playlist 解析最后按整个卷查询 `generation_modified > mStartGeneration`，没有 root 谓词。它可能处理本次 root 之外、由并发事务修改的 playlist，不能称为“只解析这次遍历到的播放列表”。

### 练习 7：分别证明seen、committed与cleaned

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final int BATCH_SIZE = 32;' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (mPending.size() > BATCH_SIZE) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (op.isInsert()) mInsertCount++;' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'ContentProviderResult[] results = mResolver.applyBatch(AUTHORITY, mPending);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (result.exception != null) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (uri != null) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'catch (RemoteException | OperationApplicationException e) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'mPending.clear();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'final long[] scannedIds = mScannedIds.toArray();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'final String generationClause = FileColumns.GENERATION_ADDED + " <= "' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'queryArgs.putInt(MediaStore.QUERY_ARG_MATCH_PENDING, MediaStore.MATCH_EXCLUDE);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'queryArgs.putInt(MediaStore.QUERY_ARG_MATCH_TRASHED, MediaStore.MATCH_INCLUDE);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'if (Arrays.binarySearch(scannedIds, id) < 0) {' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F '.appendQueryParameter(MediaStore.PARAM_DELETE_DATA, "false")' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'addPending(ContentProviderOperation.newDelete(uri).build());' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'FileColumns.GENERATION_MODIFIED + " > " + mStartGeneration);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F '.withExceptionAllowed(true);' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'public FileVisitResult visitFileFailed(Path file, IOException exc)' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'helper.beginTransaction();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!op.isExceptionAllowed()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.setTransactionSuccessful();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (deleteparam == null || ! deleteparam.equals("false")) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 11. 用四格矩阵看文件与row，而不是假设单一真相源

MediaStore 查询以数据库为索引真相，文件 I/O 以磁盘为字节真相。扫描和维护只是在两个真相之间不断缩短窗口：

| 磁盘文件 | 数据库row | 典型原因 | 后续动作与边界 |
|---|---|---|---|
| 有 | 有 | 正常、隐藏、pending、trashed，或 metadata 已陈旧 | scan 可能 skip 或 update；隐藏项可仍在 `files` |
| 有 | 无 | 文件刚由路径创建、insert/batch 失败、普通 delete 的 unlink 失败后 row 已删 | 后续 scan 尝试 insert；FINISHED 不能证明已经补齐 |
| 无 | 有 | 外部删除、卷离线留下历史索引、扫描期间竞争 | reconcile 可 clean row；pending、新 generation 或 recent volume 策略会延后处理 |
| 无 | 无 | 从未索引或已完成删除 | 对当前查询是稳定终态，但不代表所有通知消费者已处理 |

普通 MediaProvider delete 的实际顺序也不是强一致事务：先通过 `deleteIfAllowed()` 尝试 `file.delete()` 并失效 FUSE dentry，再删数据库 row。`deleteIfAllowed()` 吞掉异常，`deleteAndInvalidate()` 又忽略 `File.delete()` 的 boolean 返回值；所以 row 可以成功消失而字节仍留在磁盘。反方向上，scanner clean 明确跳过物理删除，却仍删 row。

受管 FUSE create 的常规顺序则是 row 在前：native 先调用 `InsertFile()`，Provider 插入 `IS_PENDING=1` 的 row，之后才真正 `open()` lower 文件；open 失败会回调删除先前 row。创建成功后的 `OnFileCreated()` 在 Java 侧只异步更新 quota 类型，`pf_release()` 也只关闭句柄，没有自动启动 metadata scan。于是“FUSE close 后媒体必已发布”不是成立的完成点；显式需求扫描、卷扫描或 idle 扫描仍承担 metadata 与 pending 的后续收敛。

判断一致性应至少分别观测：目标路径是否存在、精确 `_data` row 是否存在、row 的 pending/trashed/generation 状态、扫描状态字段是否清空，以及相关通知是否已经派发。单看广播或 Metrics 会把“尝试”误当成“完成”。

## 12. pending与trash把生命周期编码进row，也可能编码进物理文件名

`PATTERN_EXPIRES_FILE` 识别 `.<pending|trashed>-<秒级过期时间>-<原名>`。Provider API 创建 pending 时默认保留 7 天，trash 默认 30 天。`computeDateExpires()` 先移除调用者提供的 `DATE_EXPIRES`，仅当本次 values 明确包含 pending 或 trashed 标志时，才按系统时钟重新计算或清空；外部调用者不能任意指定过期点。

非 FUSE 的 pending 会把 `_data` 改成隐藏 pending 名；trash 无论该布尔参数如何都会选择隐藏 trashed 名。虽然这些名字以点开头，`isFileHidden()` 会对 expiration pattern 特判为非隐藏，真正的可见性仍由 pending/trashed query 条件控制。FUSE 通过 filepath 建 pending 时则不改物理名，所以数据库状态与文件名暂时不对称。反向的 `computeValuesFromData()` 能从隐藏名恢复 volume、relative path、display name、状态和过期时间；普通非隐藏名在非 FUSE 路径会清 pending/trashed/expiry，而 FUSE 路径故意保留显式 pending。

这正是 scanner 强制重扫 FUSE pending 的原因：文件名没有 expiration pattern，但 row pending 非零时不能按“mtime 与 size 没变”跳过。常规服务、idle 或 Binder 需求扫描在非 FUSE 线程成功提交 update 时，普通反推路径会清掉 pending，发布原名文件；由 FUSE rename 同步触发的扫描仍可保留 pending，batch 失败也不会完成发布。

过期清理只选择 `DATE_EXPIRES BETWEEN now-7days AND now`。它是防系统时钟巨幅跳变的窗口，不是“所有小于 now 的记录”；早于窗口的异常旧值不会被这条查询命中。普通非 FUSE 的集合查询默认排除 pending/trashed，但 item URI、FUSE 路径查询和显式 `QUERY_ARG_MATCH_*` 都有例外；因此“某次应用查询查不到”不等于 row 或文件不存在。

### 练习 8：比较Provider pending、FUSE pending与trash的物理名

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static final Pattern PATTERN_EXPIRES_FILE = Pattern.compile(' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static final long DEFAULT_DURATION_PENDING = 7 * DateUtils.DAY_IN_MILLIS;' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static final long DEFAULT_DURATION_TRASHED = 30 * DateUtils.DAY_IN_MILLIS;' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'values.remove(MediaColumns.DATE_EXPIRES);' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F '(System.currentTimeMillis() + DEFAULT_DURATION_PENDING) / 1000);' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F '(System.currentTimeMillis() + DEFAULT_DURATION_TRASHED) / 1000);' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static void computeValuesFromData(@NonNull ContentValues values, boolean isForFuse) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'if (isForFuse) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'values.put(MediaColumns.IS_PENDING, 0);' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'public static void computeDataFromValues(@NonNull ContentValues values,' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'if (!isForFuse && getAsBoolean(values, MediaColumns.IS_PENDING, false)) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F '} else if (getAsBoolean(values, MediaColumns.IS_TRASHED, false)) {' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'final Matcher matcher = FileUtils.PATTERN_EXPIRES_FILE.matcher(realFile.getName());' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'boolean isPendingFromFuse = !matcher.matches();' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'isPendingFromFuse &= c.getInt(5) != 0;' packages/providers/MediaProvider/src/com/android/providers/media/scan/ModernMediaScanner.java
grep -n -F 'scanRenamedDirectoryForFuse(oldPath, newPath);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return FuseDaemon.native_is_fuse_thread();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'int mp_return_code = fuse->mp->InsertFile(child_path.c_str(), req->ctx.uid);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'values.put(FileColumns.IS_PENDING, 1);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'fuse->mp->OnFileCreated(child_path);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'public void onFileCreatedForFuse(String path) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'fuse->fadviser.Close(h->fd);' packages/providers/MediaProvider/jni/FuseDaemon.cpp
grep -n -F 'FileColumns.DATE_EXPIRES + " BETWEEN " + from + " AND " + to' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 13. IdleService的24小时只是调度约束，维护本体有固定先后顺序

开机接收器会安排 `IdleService`；任务 ID 为 `-200`，只有不存在 pending job 时才创建。JobInfo 的 period 是 24 小时，同时要求充电和设备 idle，所以它不是墙钟上的精确每日闹钟。`onStartJob()` 新建线程运行维护；`onStopJob()` cancel signal 并返回 false，表示本次停止后不要求自动 reschedule。

`onIdleMaintenance()` 的源码顺序不可随意改写：

1. trim 持久日志；
2. 对每个**当前外部卷**调用 `MediaService.onScanVolume(REASON_IDLE)`，随后核对该卷缩略图 UUID；每个 external 扫描又会先递归扫描 internal；
3. prune 当前卷上的 stale thumbnails；
4. 找出未知 owner package 并把 owner 置空；
5. 查询最近一周内到期的 row，逐个走普通 delete；
6. 用 known volume 减 recent volume，raw delete 长期陈旧卷的 row；
7. 清 directory cache；
8. 统计 MIME 非空的媒体项并记录 maintenance metrics。

扫描阶段只捕获 `IOException`，其他运行时异常可以中断后续维护。取消检查位于每卷扫描前、带 signal 的数据库查询及每个缩略图目录前，并非每个 mutation 前都有检查；取消后已完成的早期事务不会自动回滚。

IdleService 的 job signal 也没有传给 `MediaService.onScanVolume()` 或 `ModernMediaScanner`；它与 detach 取消的按卷 scanner signal 是两套对象。因此 `onStopJob()` 发生在某卷扫描期间时，不能靠 job signal 打断这次扫描，只能等扫描返回后在下一个 maintenance 检查点生效。worker 又只捕获 `OperationCanceledException`，而 `jobFinished(params, false)` 不在 `finally`：其他运行时异常不但截断后续阶段，还会绕过这次 JobScheduler 完成回执。

### 练习 9：按源码顺序审计一次idle维护

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final int IDLE_JOB_ID = -200;' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'new Thread(() -> {' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'jobFinished(params, false);' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'mSignal.cancel();' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'return false;' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'if (scheduler.getPendingJob(IDLE_JOB_ID) == null) {' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F '.setPeriodic(TimeUnit.HOURS.toMillis(24))' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F '.setRequiresCharging(true)' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F '.setRequiresDeviceIdle(true)' packages/providers/MediaProvider/src/com/android/providers/media/IdleService.java
grep -n -F 'Logging.trimPersistent();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'for (String volumeName : getExternalVolumeNames()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaService.onScanVolume(getContext(), volumeName, REASON_IDLE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return pruneThumbnails(db, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!isPackageKnown(packageName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'delete(Files.getContentUri(volumeName, id), null, null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '.getRecentExternalVolumeNames(getContext());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'staleVolumeNames.removeAll(recentVolumeNames);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final int num = db.delete("files", FileColumns.VOLUME_NAME + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mDirectoryCache.clear();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return DatabaseHelper.getItemCount(db);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'pm.getPackageInfo(packageName, PackageManager.MATCH_UNINSTALLED_PACKAGES);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'for (SessionInfo si : pm.getPackageInstaller().getAllSessions()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'file.delete();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return c.getCount();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

## 14. thumbnail、owner、expiry与recent volume各清不同对象

thumbnail prune 先取 external.db 中所有已知 `_id`，再遍历当前卷的缩略图目录。文件名能解析为已知 id 就保留，否则调用 `deleteAndInvalidate()` 并把 `prunedCount` 加一。由于底层忽略 `File.delete()` 的 boolean，这个数字是尝试处理的 stale 候选数，不是已成功 unlink 的证明；legacy thumbnail 表另用 SQL 清孤儿 row。

owner 清理也不删文件或 row。一个包只要能以 `MATCH_UNINSTALLED_PACKAGES` 查到，或仍出现在任意 `PackageInstaller` session 中，就算 known；否则 `onPackageOrphaned()` 仅把该包所有 row 的 `owner_package_name` 置为 null。

expiry 查询直接扫共享 external.db，可能命中当前未 attached 卷的历史 row。随后普通 `delete(Files.getContentUri(volumeName,id))` 又必须通过 attached gate；这种候选可能抛 volume 异常并中断所在事务和后续维护。所有候选又处在同一个 external helper 事务里，嵌套 delete 复用它：若前几项已成功 unlink，后面的 detached-volume 或 SQLite 异常令外层事务回滚，早先 row 可以恢复，已经删除的文件却无法恢复，反而形成“无文件、有 row”。即使整体返回，`expiredMedia` 也是 cursor count，物理删除失败仍可能伴随 row 删除，不能把日志里的 Deleted N 当成 N 个字节对象已消失。

stale volume 清理使用另一套保留线：`getRecentStorageVolumes()` 的契约包含“当前和最近可用”的卷，并带 real-state、invisible、recent flags。代码计算 `known - recent`，对结果直接执行 `db.delete("files", volume_name=?)`；卷本就不可用，所以这里只删历史 row，不碰文件。current 是当前扫描集合，recent 是历史保留集合，两者不可互换。

## 15. generation适合增量水位，但必须与version和删除检测配套

自定义 `SQLiteQueryBuilder` 会移除调用者提供的 `generation_added`、`generation_modified`。insert 把两列都写成当前 generation；update 只改 `generation_modified`；delete 没有 row 可盖章，但包裹它的成功事务仍会推进全局 generation。files trigger 调用 `_INSERT/_UPDATE/_DELETE` listener 安排通知、授权和缩略图等副作用，不负责 generation 赋值。

还要识别绕过自定义 builder 的内部维护：例如 orphan owner 使用原生 `db.update()`，事务 generation 会前进，却不会给受影响 row 重写 `generation_modified`；stale-volume raw delete 也没有 tombstone。于是“水位前进”与“能从 generation 列拉到每个变化”并不等价。

`MediaStore.getVersion(volume)` 在 provider 内返回 `db.getVersion() + ":" + DatabaseHelper.getOrCreateUuid(db)`。数据库被删除、重建或 UUID 改变时，version 能迫使客户端全量同步。对多个外部卷而言，version 和 current generation 都来自同一个 external.db，是**数据库级**而非逐卷计数；其他卷的事务甚至无 row 改动的成功事务，都可让某个具体卷观察到水位前进而查不到 delta。

稳健的增量同步可保存 `(version, generation)`：下次先比较 version，改变则全量重建；未变时要在**查询前**采样 `highWater=getGeneration()`，再按目标具体卷拉取 `old < generation_added|modified <= highWater`，成功处理后只保存这个预采样 highWater，并再次核对 version。不能在查询结束后才读取并保存最新 generation，否则夹在 query 与末次取水位之间的提交会被跨过去。generation 比 wall-clock 字段稳健，但它没有 delete tombstone，单靠这两列发现不了已删除 id；删除仍需 observer、周期性全量集合对账或业务自己的 tombstone 机制。

对扫描器而言，start generation 也只是并发插入保护线。出现空 delta、跳号或多个 row 共用同一 generation 都是合法现象；把它解释成连续 row 序号会制造错误恢复逻辑。

## 16. 用完成点矩阵收束排障，并把下一章边界留给写入事务

遇到“相册没出现”“文件删不掉”“扫描明明结束却状态异常”，按下面顺序定位：

| 问题 | 直接证据 | 常见误判 |
|---|---|---|
| 卷是否可解析 | current volume 名、path 与 scan paths 缓存 | mounted 就必然 attached |
| URI 是否可路由 | `mAttachedVolumeNames` 与具体卷名 | external 合成名本身是一块物理卷 |
| 是否真正遍历 | root、隐藏父链、不可扫描 pattern、取消点 | `.nomedia` 总会跳过整棵子树 |
| row 是否写成 | 精确 `_data` query、result exception、generation 字段 | pending 队列计数就是成功数 |
| 旧 row 是否对账 | root predicate、scanned IDs、start generation、pending 条件 | generation 能封住所有 rename/update 竞争 |
| 文件是否真删除 | 删除后重新 `stat` 路径 | row 消失或 expired 日志即可证明 unlink |
| 维护是否走到底 | 每一阶段的最后证据与异常日志 | 24 小时 period 等于每天固定时刻完整执行 |

整条链可以压成一句话：**StorageManager 给出当前卷，attach 打开数据库路由，scanner 把文件事实转成 row 操作，reconcile 清理旧索引，idle 再按不同保留线修剪派生物与历史状态；这些步骤通过 generation、锁、取消和通知相互约束，却没有跨文件系统与 SQLite 的总事务。**

下一章进入 MediaProvider 的写路径：`insert/update/delete` 如何计算放置路径，rename 怎样协调文件与数据库事务，文件 I/O 的失败窗口在哪里，以及 files trigger 最终如何把变化分发为通知。
