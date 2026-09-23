# 276 Android MediaProvider query、SQLiteQueryBuilder、projection、owner权限过滤、pending/trashed匹配与canonical URI链

## 1. 先看结论：query不是一条SQL，而是五个层层叠加、不可互换的边界

本文只讨论 Android 11 `android-11.0.0_r48`：`frameworks/base` 位于 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/providers/MediaProvider` 位于 `47c141d93e93b25cc85c36f3579fda25a1695952`。这个版本的读取面不能概括为“URI 转成表名，再执行 selection”。一次普通 query 至少同时受五个边界约束：

1. framework `ContentProvider.Transport` 先校验 URI/userId，并执行 provider 级 read permission 与 AppOps；
2. MediaProvider 把 legacy 参数与 `Bundle` 参数归一化，再尝试恢复 canonical URI；
3. URI match、attached-volume gate 与 volume row filter 决定能进入哪个 helper、哪些 table/view 和哪些卷；
4. provider 自带的 `SQLiteQueryBuilder` 用 projection map、strict grammar 与双重编译限制调用者可表达的 SQL；
5. global、collection capability、shared-UID owner、pending/trashed/favorite 与 caller selection 被组合成最终 WHERE。

因此，下列结果不能互相代换：

| 观察结果 | 能直接证明什么 | 不能直接证明什么 |
|---|---|---|
| collection query 返回空 Cursor | 此身份在所有过滤叠加后没有可返回 row，或 Transport 已空化结果 | 数据库没有 row、URI 不合法、Provider 一定执行过 SQL |
| item query 返回一行 | 当前时刻、当前身份、当前 URI 与状态过滤可见该 row | 持有 write grant、能打开原始字节、canonical key 永久有效 |
| Cursor 带 notification URI | 远端普通 Cursor 已登记观察目标 | observer 已收到未来通知、控制 URI 也有该元数据 |
| extras 列出 honored arg | 该 key 进入了 Provider 的处理路径 | 调用者确实传过该 key、其值没有被覆盖、它匹配了 row |
| 未带canonical标记的 URI 经 `canonicalize()` 返回新 URI | 当前 row 可唯一读取且追加了辅助定位键 | 同名旧参数不存在时该键才是后续读取的有效hint；它也不保证全局唯一、跨卷恢复或新授权 |
| `safeUncanonicalize()` 返回原 URI | 输入未带canonical标记，或已标记但恢复没有给出替代 URI | 原 id 仍指向原对象；它也不捕获一般运行时异常 |

读取链的核心公式可先记成：**Transport gate → URI/volume/table → projection contract → trusted row policy → caller expression → Cursor metadata**。canonical URI 位于 URI 归一化阶段，它只是尽力重定位 item，既不是主键，也不是权限令牌。

## 2. Transport与两个query重载先统一入口，Bundle会被原地改写

跨进程调用先到 framework 的 `ContentProvider.Transport.query()`。它先 `validateIncomingUri()`、去掉 userId，再执行 `enforceReadPermission()`。AppOps 结果不允许时，如果 projection 非 null，Transport 直接返回同列顺序的零行 `MatrixCursor`，不会进入 MediaProvider；如果 projection 为 null，为了取得列名，它仍调用 Provider，然后只保留列名并把 row 清空。所以“空 Cursor”甚至不能证明 MediaProvider 的 query 主体一定执行过。

进入 MediaProvider 后，旧五参重载用 provider 自己的 `DatabaseUtils.createSqlQueryBundle()` 携带 selection、selectionArgs、sortOrder，再进入 Bundle 重载。`queryInternal()` 把 null 变成新 `Bundle`，删除只允许 Provider 内部使用的 `INCLUDED_DEFAULT_DIRECTORIES`，然后调用 `resolveQueryArgs()`。

这里不是纯函数转换：structured group/sort/limit 会写回同一个 Bundle 的 raw SQL key，旧 target 的兼容恢复也继续写回它。远端 Binder 参数通常是反序列化副本，但同进程直接调用者可能观察到 Bundle 被修改。分析日志时应同时保留“入参快照”和“归一化后快照”。

### 练习 1：核对Transport空化与Bundle统一入口

在源码根目录运行；也可把源码根目录作为第一个参数，从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public Cursor query(String callingPkg, @Nullable String attributionTag, Uri uri,' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'uri = validateIncomingUri(uri);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (projection != null) {' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return new MatrixCursor(projection, 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'return new MatrixCursor(cursor.getColumnNames(), 0);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'DatabaseUtils.createSqlQueryBundle(selection, selectionArgs, sortOrder)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'queryArgs = (queryArgs != null) ? queryArgs : new Bundle();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'queryArgs.remove(INCLUDED_DEFAULT_DIRECTORIES);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'DatabaseUtils.resolveQueryArgs(queryArgs, honoredArgs::add, this::ensureCustomCollator);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'queryArgs.putString(QUERY_ARG_SQL_GROUP_BY, groupBy);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'queryArgs.putString(QUERY_ARG_SQL_SORT_ORDER, sortOrder);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'queryArgs.putString(QUERY_ARG_SQL_LIMIT, limitString);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
```

分别推演 projection 为 `{"_id"}` 与 null、AppOps 不允许的两次调用：前者不进入 MediaProvider，后者可能为取得列名而进入，但最终都只有零行。

## 3. canonical恢复早于URI match；控制URI、attached gate与row卷过滤不能合并

参数归一化之后，`queryInternal()` 立即执行 `safeUncanonicalize(uri)`，然后才读取 volume、target SDK、hidden 能力并做 `matchUri()`。这意味着成功恢复出的新 item URI会参与后续 table、id、volume、notification URI 全链；失败则保留原 canonical URI，其旧 path id 继续参与查询。

`MEDIA_SCANNER`、`FS_ID`、`VERSION` 在数据库 helper 之前返回 `MatrixCursor`。它们绕过的不只是普通 table/projection policy，还包括 `getDatabaseForUri()`、builder、DB Cursor notification 与 honored extras。其他 URI 才进入 attached gate。

volume 有两个不同语义层：

- `getDatabaseForUri()` 先用 `resolveVolumeName()` 把 synthetic `external` 映成 `external_primary`，以它检查 `mAttachedVolumeNames`；internal 选 `mInternalDatabase`，其余选同一份 `mExternalDatabase`。
- builder 保留 URI 的原 volume。若它是 `external`，就把当前 `getExternalVolumeNames()` 展开为 `volume_name IN (...)`；若是 concrete volume，只绑定这个名字。

所以 `external` 不是“逐卷打开多个 SQLite 数据库”。它先借 `external_primary` 通过 helper gate，再在同一 external DB 内用 row 的 `volume_name` 聚合当前具体卷。普通 image/audio/video/files/downloads 会追加这一卷过滤；thumbnail 与若干 audio 派生分支没有同一套 state/volume 条件，不能把普通 collection 的结论外推给所有 match。

public matcher 先匹配；只有 self 才能令 `allowHidden=true`。命中 hidden matcher 而调用者无资格时会抛 `IllegalStateException`，不是把它当普通 unknown table 静默查询。

### 练习 2：拆开canonical、控制URI、helper与row卷名

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'uri = safeUncanonicalize(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String volumeName = getVolumeName(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final int table = matchUri(uri, allowHidden);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (table == MEDIA_SCANNER) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (table == FS_ID) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (table == VERSION) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final DatabaseHelper helper = getDatabaseForUri(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private static @NonNull String resolveVolumeName(@NonNull Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return MediaStore.VOLUME_EXTERNAL_PRIMARY;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!mAttachedVolumeNames.contains(volumeName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return mExternalDatabase;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'includeVolumes = bindList(getExternalVolumeNames().toArray());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, FileColumns.VOLUME_NAME + " IN " + includeVolumes);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final int publicMatch = mPublic.match(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new IllegalStateException("Unknown URL: " + uri + " is hidden API");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return isCallingPackageSelf();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

画出 synthetic `external` 的两条线：helper 线只检查 `external_primary` 是否 attached，row 线则展开当前 concrete external volume 集合；不要画成多个 database helper。

## 4. structured参数覆盖raw键，honored数组只是“走过的处理分支”

`resolveQueryArgs()` 优先使用高层参数：非空 `QUERY_ARG_GROUP_COLUMNS` 拼成 SQL group；非空 sort columns 可叠加 locale/collation 与 direction；存在整数 limit 时再拼可选 offset。随后它们写回 `QUERY_ARG_SQL_GROUP_BY`、`QUERY_ARG_SQL_SORT_ORDER`、`QUERY_ARG_SQL_LIMIT`，覆盖同 Bundle 中原有 raw 值。offset 没有 limit 时不会单独生效。

locale 会经 `ensureCustomCollator()` 生成只保留ASCII字母并加 `custom_` 前缀的名字；该名字第一次出现时，Provider才在internal/external helper注册 ICU collation，strict grammar又只接受 `custom_[a-zA-Z]+`。这套净化不是一一映射：`en-US` 与 `en_US` 都成为 `custom_enUS`，后来的locale会复用先注册者；若一个被ULocale接受的输入净化后没有任何字母，返回的 `custom_` 又无法通过外部strict grammar。PRIMARY/SECONDARY 映成 `NOCASE`，IDENTICAL 不追加 SQL suffix。

`EXTRA_HONORED_ARGS` 在 r48 不能严格解释成“调用方确实提交且最终生效的键”：

- SQL selection 与 selectionArgs 无条件加入，即使 Bundle 没有它们；
- 没有 structured group/sort/limit 时，对应 raw SQL key 也无条件加入；
- structured sort 未给 locale 时，默认 IDENTICAL 仍会把 sort-collation key 加入；
- builder 会读取并执行 raw SQL having，但 `resolveQueryArgs()` 从未把 having 加入；
- 普通媒体分支会无条件加入三个 MATCH key，随后 item 分支还可能覆盖其值；
- Provider 只设置 honored 数组，不设置 total-count extra。

target R 以前，sort 尾部和 URI 参数中的 legacy limit 会被提取；target Q 以前，selection 尾部的 group、首 projection 的 `DISTINCT ` 与特定 thumbnail 查询还会被兼容。builder 在这些恢复前已经创建，但最后读取的是同一个被改写的 Bundle，所以恢复仍影响执行。

thumbnail兼容是更强的例外：Images/Video thumbnails collection只要selection完整匹配 `image_id=数字` 或 `video_id=数字`，就直接用请求projection构造 `MatrixCursor`并返回。虽然helper gate与builder构造已经发生，但 `qb.query()` 没有执行，因此projection map计算、strict grammar、caller selection执行和source-owner trusted子查询都没有成为结果门；未知projection名也可成为Cursor列，只是四个已知列之外的值保持null。该分支不验证源row存在或可见，也不设置普通DB Cursor的notification/extras。

### 练习 3：验证参数优先级、honored缺口与旧target恢复

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'honored.accept(QUERY_ARG_SQL_SELECTION);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'honored.accept(QUERY_ARG_SQL_SELECTION_ARGS);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final String[] columns = queryArgs.getStringArray(QUERY_ARG_GROUP_COLUMNS);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'honored.accept(QUERY_ARG_SQL_GROUP_BY);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final String[] columns = queryArgs.getStringArray(QUERY_ARG_SORT_COLUMNS);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final int collation = queryArgs.getInt(' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'sortOrder += " COLLATE NOCASE";' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final int limit = queryArgs.getInt(QUERY_ARG_LIMIT, Integer.MIN_VALUE);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final int offset = queryArgs.getInt(QUERY_ARG_OFFSET, Integer.MIN_VALUE);' packages/providers/MediaProvider/src/com/android/providers/media/util/DatabaseUtils.java
grep -n -F 'final String having = queryArgs.getString(QUERY_ARG_SQL_HAVING);' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'final String collationName = "custom_" + locale.replaceAll("[^a-zA-Z]", "");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!mCustomCollators.contains(collationName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execPerConnectionSQL("SELECT icu_load_collation(?, ?);",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mCustomCollators.add(collationName);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '"(?i)custom_[a-zA-Z]+");' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'DatabaseUtils.recoverAbusiveSortOrder(queryArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'DatabaseUtils.recoverAbusiveLimit(uri, queryArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'DatabaseUtils.recoverAbusiveSelection(queryArgs);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'projection[0] = projection[0].substring("DISTINCT ".length());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'static final Pattern PATTERN_SELECTION_ID = Pattern.compile(' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final Matcher matcher = PATTERN_SELECTION_ID.matcher(selection);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final MatrixCursor cursor = new MatrixCursor(projection);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'cursor.newRow().add(MediaColumns._ID, null)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'extras.putStringArray(ContentResolver.EXTRA_HONORED_ARGS,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

构造同时含 structured sort columns、raw sort order、offset、但没有 limit 的 Bundle：structured sort 胜出，offset 不进入 SQL；再对照 extras，区分“被列名报告”和“请求值真正改变 SQL”。

## 5. MediaProvider用的是本地builder；projection map对self也没有消失

这里必须先认准类：`MediaProvider.java` import 的是 `com.android.providers.media.util.SQLiteQueryBuilder`，不是 framework 的 `android.database.sqlite.SQLiteQueryBuilder`。两者实现不能混读。

每个受支持 URI match 在返回 builder 前都必须设置 projection map，否则 Provider 主动抛错。`DatabaseHelper.getProjectionMap()` 反射 MediaStore contract class 的 public fields，只收入字段名为 `_ID` 或带 `@Column` 注解的列，并按 class 缓存、按分支合并。provider builder 再把 map key 和来访列名都转为小写查找，因此公开列匹配在这一层不区分大小写。join、thumbnail 等分支还可把公开列映成带限定符或安全 alias 的表达式。

projection 为 null **或空数组**时，只展开 projection map 的 values，并跳过 `_count`；不是直接 `SELECT` 底表所有私有列。权限探针传 `new String[0]` 虽然调用方只消费 `moveToFirst()`，SQL 仍会展开公开映射列，不是零列或 `SELECT 1`。

所有调用者都先 `setStrict(true)`。self 只跳过额外的 strict-columns 与 strict-grammar 两个 flag；强制 projection map、`computeProjection()` 和 strict-parentheses 仍然存在。由于 strict flag 已非零，self 也不能依靠关闭 strict-columns 获得任意 `column AS alias`。源码注释所说的“any columns”必须按实际实现收窄理解。

### 练习 4：证明本地builder、公开列反射与self边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'import com.android.providers.media.util.SQLiteQueryBuilder;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public ArrayMap<String, String> getProjectionMap(Class<?>... clazzes) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'if (Objects.equals(field.getName(), "_ID")' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F '|| field.isAnnotationPresent(mColumnAnnotation)) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'map.put(column, column);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'mProjectionMap.put(entry.getKey().toLowerCase(Locale.ROOT), entry.getValue());' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'column = mProjectionMap.get(userColumn.toLowerCase(Locale.ROOT));' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'if (qb.getProjectionMap() == null) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new IllegalStateException("All queries must have a projection map");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public @Nullable String[] computeProjection(@Nullable String[] projectionIn) {' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'if (projectionIn != null && projectionIn.length > 0) {' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'if (entry.getKey().equals(BaseColumns._COUNT)) {' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'qb.setStrict(true);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setStrictColumns(true);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setStrictGrammar(true);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (mStrictFlags == 0 &&' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'try (Cursor c = qb.query(helper, new String[0],' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

分别用 null、空数组、`{"_id"}`、`{"COUNT(_id)"}` 与未知列手算 `computeProjection()`；前两者行为相同，未知列在生成 SQL 前失败。

## 6. 聚合只包裹可映射列；旧target灰名单不是aggregation开关

provider builder 只识别 `AVG/COUNT/MAX/MIN/SUM/TOTAL/GROUP_CONCAT/UNICODE` 八种外层函数。匹配后，它仍把括号内的**整个字符串**拿到 projection map 查找；所以现代 target 的 `COUNT(_id)` 可成立，而 `COUNT(*)` 通常因 `*` 不在 map 中失败。聚合外壳不会让 `COUNT(secret_column)` 穿透公开列契约。

target Q 以前，MediaProvider 才配置窄 projection greylist，兼容 `COUNT(*)`、部分 alias、少数 case 表达式和历史列写法。灰名单命中只放行这一条 projection 表达式；trusted owner/state/volume WHERE 仍会进入查询，计数不会因此越过 row policy。

framework builder 中名为 `setProjectionAggregationAllowed()` 的方法在这个 tag 已是 deprecated 空实现，getter 恒为 true；provider 本地 builder根本没有这个开关。因而不能写成“MediaProvider 打开 framework aggregation 开关”。实际控制点是本地 builder 的聚合 regex、projection map 与旧 target greylist。

alias 也不能泛化为任意开放：直接 map、受控聚合或 greylist 三条路都失败时，普通 `column AS alias` 只有 `mStrictFlags==0` 才放行；MediaProvider 已无条件开启 strict parentheses，所以这条宽松路对它的 self builder 也不可用。

## 7. trusted WHERE与caller selection永远AND；strict grammar再阻断跨clause跳转

Provider 产生的 id、media type、owner、state、volume 等条件先经 `DatabaseUtils.bindSelection()` 固化为可信 SQL，再由 `appendWhereStandalone()` 把每段单独加括号并以 `AND` 相连。最终 `computeWhere()` 又形成 `(internalPolicy) AND (callerSelection)`。调用者写 `1=1 OR ...` 最多让自己的括号恒真，不能取消前面的 trusted policy。

非 self 还要对 selection、group、having、sort、limit 全部 tokenize。合法 map column/table、受控 custom collator、已知 SQLite function/type 与未被特别封锁的 keyword可通过；`SELECT/FROM/WHERE/GROUP/HAVING/WINDOW/VALUES/ORDER/LIMIT` 被显式拒绝，分号也由 tokenizer拒绝。target R 前只有三条精确 token pattern 作为兼容口。

执行时先用原始 selection/having 构造 unwrapped SQL，再做 strict-column 与 grammar 检查。strict-parentheses 模式先 `validateSql(unwrappedSql, cancellationSignal)`，然后重新构造把 selection/having 再包一层括号的 SQL并真正 `rawQueryWithFactory()`；CancellationSignal 同时传给验证和执行。这是“两种形态都能编译，实际只跑 wrapped 形态”，不是执行两次查询。

query 经 `helper.runWithoutTransaction()`：当前线程已有 helper transaction 时直接复用；否则只在 `rawQueryWithFactory()` 构造 Cursor 期间持 schema read lock。SQLiteCursor 后续填充 window 与遍历可以是惰性的，此时锁已释放；它也不会为了每个读取新开一个只读 SQLite transaction。

### 练习 5：沿着WHERE隔离、grammar与双编译走一遍

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'qb.appendWhereStandalone(DatabaseUtils.bindSelection(selection, selectionArgs));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mWhereClause.append(" AND ");' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'mWhereClause.append('\''('\'').append(inWhere).append('\'')'\'');' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'where.append(" AND ");' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'where.append('\''('\'').append(selection).append('\'')'\'');' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'void enforceStrictGrammar(@Nullable String selection, @Nullable String groupBy,' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'SQLiteTokenizer.tokenize(limit, SQLiteTokenizer.OPTION_NONE,' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'case "SELECT":' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'case "LIMIT":' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'throw genException("Semicolon is not allowed", sql);' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteTokenizer.java
grep -n -F 'final String unwrappedSql = buildQuery(' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'db.validateSql(unwrappedSql, cancellationSignal);' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'final String wrappedSql = buildQuery(projectionIn, wrap(selection), groupBy,' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'return db.rawQueryWithFactory(' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'return helper.runWithoutTransaction((db) -> {' packages/providers/MediaProvider/src/com/android/providers/media/util/SQLiteQueryBuilder.java
grep -n -F 'if (mTransactionState.get() != null) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'mSchemaLock.readLock().lock();' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
```

把 caller selection 设为 `_id=7 OR 1=1`，再假设 internal policy 是 owner 与 volume 两段：写出最终括号结构，证明 caller 的 OR 无法逃出自己的表达式。

## 8. global是免owner收窄的快路，不是跳过state、volume或Transport

`checkCallingPermissionGlobal(uri, forWrite)` 有三类快路：self/shell、manager，以及 URI相关能力。URI相关能力又分两种：五类特定 item id可命中 LocalCallingIdentity 的 owned-id cache；除此以外，Context 对**当前传入URI**持有对应 read/write grant也会放行，这个检查写在 item switch之外，并未限定为item。

owned cache 只用于 Audio/Video/Images/Files/Downloads 五种 item match，不含 playlist、playlist member、thumbnail与派生 URI；其数据结构只是 `LongArray` 中的 id，没有 volume 或 table 维度。它是依靠维护/invalidation正确性的性能快路，不应描述成一次实时数据库 owner 证明，更不能把同一个数字 id 在不同命名空间中的含义混在一起。

URI grant严格按 `forWrite` 选择 read 或 write flag。TYPE_QUERY 用 read；TYPE_UPDATE/DELETE 用 write。read grant 不会自动让 write builder获得 global。反过来，global 为 true 只让各 collection 跳过 owner/capability收窄；普通 collection 后面的 pending/trashed/favorite 与 `volume_name IN` 仍照常追加。Transport 的 provider read/write permission与 AppOps 也仍是外层边界。

这里存在一条不能按“grant只授具体row”简化的 r48 交叉链。MediaProvider声明 `grantUriPermissions=true` 与 `forceUriPermissions=true`；后者让framework即使认为目标已有普通provider权限，也继续建立实际grant账。本文成对基线的 APEX清单版本 `300000000` 低于动态回调门槛 `301400000`，所以模块存在时，UriGrantsManager校验grant发起者是否持有源 URI时**不会**回调MediaProvider的row级 `checkUriPermission()`。Provider又是exported且没有顶层read/write permission，静态检查便把任意匹配到该Provider的URI视作发起者已持有。于是代码路径可为猜测的item、collection，乃至带 prefix flag 的collection建立grant；这是一条需要真机复现边界条件的静态控制流结论。

`forceUriPermissions`在这里强制生成grant账，却没有反过来强制旧APEX执行动态源row校验。同一份framework在找不到MediaProvider模块时反而假定动态能力已存在；若仍配本文这份Provider实现，才会回调它的 `checkUriPermission()`。真实升级到门槛以上的模块必须按对应版本源码复核，不能假定仍保留r48行为。

接收者随后以同source user与足够mode访问时，Context grant账的exact查找可命中同一URI；prefix grant还会按 path-prefix 命中collection本身与descendants。MediaProvider把任一结果作为`allowGlobal`，从而跳过owner/capability条件。普通Images/Audio/Video/Playlists/Files/Downloads仍会叠加state/favorite/volume；但audio派生、playlist-members与image/video thumbnails没有统一builder state/requested-volume公式，grant还会移除它们的`AND (0)`或source-owner子查询。external helper的artists/albums/genres SQL view另烘焙异步收敛的current-volume集合，internal view另限internal，但都不是请求URI的具体卷过滤。风险因此不只是一条collection注释冲突，而是旧APEX配对下grant发起者的row资格没有进入动态核验。已经建立的exact/prefix grant如何被消费可由源码确定；具体设备上还有哪些产品策略限制发起者、目标或传递方式，仍需复现确认。

owner SQL 使用 `OWNER_PACKAGE_NAME IN getSharedPackages()`，即把同 UID 下所有 package 名纳入；audio/video/images capability却由当前 LocalCallingIdentity 的 calling package/attribution参与 AppOps 检查。shared UID 的 owner 合并与 collection capability不是同一个判断。

legacy read 只在 Files/Downloads 的 options 构造中有特殊意义：query 时可跳过 owner options，但仍受 state/volume；legacy write 构造 write builder时只是 options 中的 `external_primary` 候选，不等于全卷 global。

## 9. typed collection按读写能力分流，Audio和派生表有刻意的不对称

同一个 URI 工厂同时服务 query 与 mutation。TYPE_QUERY 令 `forWrite=false`；insert/update/delete 为 true。Images、Video、Audio、Playlist 的 query通常选 SQL view，write 选 `files` 并补 media-type；projection contract仍按各自公开类生成。

| URI族 | 无global/对应能力时的trusted条件 | 额外边界 |
|---|---|---|
| Images / Video collection或item | shared-UID owner | 再叠加 state、favorite、volume |
| Audio media | owner **或** ringtone/alarm/notification | 这组 OR 也出现在 update/delete builder，不只读路径 |
| Audio Playlists | shared-UID owner | query view、write files+playlist type |
| genres/artists/albums/album-art等派生数据 | 没有 audio read/write能力就追加常量 `0` | external helper的artists/albums/genres view另带异步收敛的current-volume快照；internal view按internal过滤；它们仍无请求URI具体卷与统一state公式 |
| playlist members | 没有 global 且 `checkCallingPermissionAudio(false)` 失败就追加 `0` | write builder也固定用 `false`；Provider静态update/delete链把 audio read能力视作足够 |
| image/video thumbnail表 | source id 必须落在 owner可见的 images/video 子查询中 | 不追加普通媒体的 pending/trashed/favorite/volume公式 |

Audio 的 ringtone 兼容 OR 在调用 `checkCallingPermissionAudio(forWrite, ...)` 失败后无条件构造，因此不能写成“只给无读权限应用看系统铃声”；它也会进入 TYPE_UPDATE/DELETE builder。item mutation的 `enforceCallingPermission(..., true)` 本身仍用 TYPE_UPDATE builder探测，delete 的 `deleteIfAllowed()` 又经同类write检查，所以这些item probe不会消除该 OR；collection update/delete更没有item probe。可修改列、路径与文件IO仍是后续约束，但不能把它们误当成row授权收窄。

同样，playlist member 的 `checkCallingPermissionAudio(false, ...)` 在 query与write分支外共同执行。update/delete 的 `resolvePlaylistIndex()` 明确构造 TYPE_DELETE members builder，随后 `queryForDataFile(playlistUri)` 只走 TYPE_QUERY；移动、移除与替换 audio id 的 helper没有再强制playlist write。因而就这份 Provider 的静态路径，只有 audio read能力也足以通过 member update/delete 的row与文件定位检查，最终仍可能因文件IO失败。insert则不同：它显式要求playlist write，并另验audio read。

### 练习 6：还原typed、Audio例外与派生表空结果

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'case TYPE_QUERY: forWrite = false; break;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("images");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("video");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("audio");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, FileColumns.MEDIA_TYPE + "=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!allowGlobal && !checkCallingPermissionImages(forWrite, callingPackage)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!allowGlobal && !checkCallingPermissionVideo(forWrite, callingPackage)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!allowGlobal && !checkCallingPermissionAudio(forWrite, callingPackage)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '+ " OR is_ringtone=1 OR is_alarm=1 OR is_notification=1"));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("audio_playlists");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("audio_playlists_map, audio");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!allowGlobal && !checkCallingPermissionAudio(false, callingPackage)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, "0");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '"image_id IN (SELECT _id FROM images WHERE "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '"video_id IN (SELECT _id FROM video WHERE " +' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return mCallingIdentity.get().hasPermission(PERMISSION_READ_AUDIO)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '|| mCallingIdentity.get().hasPermission(PERMISSION_WRITE_AUDIO);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb = getQueryBuilder(TYPE_DELETE, AUDIO_PLAYLISTS_ID_MEMBERS,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final File playlistFile = queryForDataFile(playlistUri, null);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case FILES: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case DOWNLOADS: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (!allowGlobal && !allowLegacyRead) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'options.add(DatabaseUtils.bindSelection("volume_name=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '+ " AND media_type=0 AND mime_type LIKE '\''audio/%'\''");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, TextUtils.join(" OR ", options));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, FileColumns.IS_DOWNLOAD + "=1");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execSQL("CREATE VIEW audio_artists AS SELECT "' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F '+ " WHERE is_music=1 AND volume_name IN " + filterVolumeNames' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'db.execSQL("CREATE VIEW audio_genres AS SELECT "' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
```

假设 caller没有global：分别为“无image能力的other-owner image”“无audio能力的other-owner ringtone”“无audio能力的same-owner普通audio”“无audio能力的artist”“只有audio read能力的playlist-member write builder”“无image能力且source为other-owner的thumbnail”列出新增trusted条件并判断结果；不要给派生表套用普通owner近似。

## 10. Files与Downloads把多种能力做OR，但最终仍与state、volume做AND

Files 是最宽的 projection，却不是“拥有任一媒体权限就看全表”。当 caller既非 global、也非 legacy-read 时，builder构造一个 options OR：

- shared-UID owner永远是第一项；
- write场景若是 legacy writer，再加入 `volume_name=external_primary`；
- audio能力加入 AUDIO、PLAYLIST、SUBTITLE media type，并允许 owner名匹配且 `media_type=0 AND mime_type LIKE 'audio/%'` 的隐藏类型；
- video能力加入 VIDEO、SUBTITLE，并只为 owner名匹配的 media_type 0 video MIME补口；
- image能力加入 IMAGE，并只为 owner名匹配的 media_type 0 image MIME补口；
- Provider内部可再加入默认目录的 relative-path候选。

整组 OR 之后仍与 filter、pending、trashed、favorite、volume 条件逐段 AND。audio权限让标准 AUDIO row跨 owner可见，却不会让任意 other-owner `media_type=0` audio MIME可见；后者仍含 owner条件。legacy-read query则完全不建这个 options OR，所以 synthetic `external` 上可见当前 includeVolumes 中的 row，而不是只限 primary。

Downloads没有 audio/video/image collection能力分支：非 global、非 legacy-read时只取 owner；write legacy再加 external-primary候选。它随后同样叠加三种 state与volume。

thumbnail/derived分支是另一个模型：image/video thumbnail只用source-owner子查询，没有普通state/volume；audio派生表在无整体audio能力时直接 `AND (0)`。external helper中的artists/albums/genres view会按 current external names 重建，internal helper的同名view按internal过滤；而 `updateVolumes()` 更新内存集合后是经前台executor异步触发外部view重建，所以这里的“current”只是最终收敛的快照，挂载变化后存在旧定义短窗。album-art、playlist-members等没有这层；即使有volume快照过滤，也不是按请求的concrete URI卷过滤。排查泄漏或“指定卷却混入其他当前卷元数据”时，必须先确认URI match落在哪个case，不能只看files主分支。

## 11. 四种MATCH不是同义布尔值；FUSE read还有内部可写可见模式

公开 `MATCH_DEFAULT/INCLUDE/EXCLUDE/ONLY` 的实际 WHERE转换为：

| match | 普通转换 |
|---|---|
| INCLUDE | 不追加这一列的条件，0和1都可继续参与后续过滤 |
| ONLY | 追加 `column=1` |
| EXCLUDE | trash/favorite为 `column=0`；pending另有FUSE-owned例外 |
| DEFAULT | 先按线程/操作/列解析，再进入以上分支 |

非 FUSE 的 pending 与 trashed默认 EXCLUDE，favorite默认 INCLUDE。legacy URI 的 include-pending 参数在默认解析前强制把 pending改为 INCLUDE，甚至覆盖同次 Bundle 显式给出的 EXCLUDE/ONLY。

pending 的普通 EXCLUDE 不是简单 `is_pending=0`。它还允许 `is_pending=1`、物理 `_data` 不匹配隐藏 pending 文件名 pattern、且 owner属于 shared-UID packages 的 row；这就是“owned pending from FUSE”例外。trash/favorite EXCLUDE没有这条例外。

FUSE thread 的 write默认 pending/trash INCLUDE；FUSE read默认内部值 `MATCH_VISIBLE_FOR_FILEPATH`。该模式先问 legacy-write或**write-global**，后者固定调用 `checkCallingPermissionGlobal(uri, true)`；外层TYPE_QUERY仅靠read grant得到的global并不足以省掉这层状态条件。有legacy-write/write-global时不追加状态条件。否则再按URI族分流：Images、Audio、Video、Playlist持有对应write能力时也可省略整列条件；Downloads没有这种能力快路；Files的audio/video/image write能力只把相应media type加入options，并不会省略整列条件。余下情况形成 `column=0 OR (column=1 AND options)`：options至少含owner，pending额外允许FUSE物理名，trash不加这项。这个模式表达的是“从路径访问角度可写的隐藏row也可见”，不是公开第五种Match API。

### 练习 7：把MATCH、legacy覆盖与FUSE例外落实到WHERE

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final int MATCH_VISIBLE_FOR_FILEPATH = 32;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private static final String MATCH_PENDING_FROM_FUSE = String.format("lower(%s) NOT REGEXP '\''%s'\''",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case MATCH_INCLUDE:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case MATCH_EXCLUDE:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case MATCH_ONLY:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'appendWhereStandalone(qb, column + "=?", 1);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case MATCH_VISIBLE_FOR_FILEPATH:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return String.format("%s=0 OR (%s=1 AND %s AND %s)", column, column,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return column + "=0";' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (MediaStore.getIncludePending(uri)) matchPending = MATCH_INCLUDE;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'defaultMatchForPendingAndTrashed =' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'forWrite ? MATCH_INCLUDE : MATCH_VISIBLE_FOR_FILEPATH;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (matchFavorite == MATCH_DEFAULT) matchFavorite = MATCH_INCLUDE;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (isCallingPackageLegacyWrite() || checkCallingPermissionGlobal(uri, /*forWrite*/ true)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'options.add(DatabaseUtils.bindSelection(matchSharedPackagesClause));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'options.add(MATCH_PENDING_FROM_FUSE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String matchWritableRowsClause = String.format("%s=0 OR (%s=1 AND %s)", column,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

为普通query、FUSE read、FUSE write各写一次pending/trashed默认值；再对typed Images与Files分别加入对应write能力，验证前者可省略整列条件、后者只扩充options，并观察FUSE物理名为何只影响pending。

## 12. item URI强制include pending/trashed，但不移除owner、favorite或volume门

Images/Audio/Video/Playlist/Files/Downloads 的 item-id case先追加精确 `_id=?`，然后把 pending与trashed无条件改为 INCLUDE再 fall through到 collection逻辑。这发生在读取 Bundle、legacy include-pending和默认值解析之后，所以 item URI甚至会覆盖调用者明确要求的 pending/trash ONLY或EXCLUDE。favorite不被覆盖，仍按请求或默认 INCLUDE。

“item包含隐藏状态”并不等于“知道 id 就能访问”。fall-through后仍会追加 global/collection capability或owner条件、favorite条件与volume条件；只有 state的两项被拿掉。thumbnail item、playlist member item与多数派生 item走不同 case，也不能套用这条规则。

适用 collection仍无条件把三个 MATCH key加入 honored数组，即使调用者没传、item又覆盖了pending/trash。这再次证明 extras是处理路径说明，而非入参审计或效果回执。

这一点也影响canonical恢复：exact-id查询可因 item override看到 pending/trash row；如果 id已变化，fallback退回 collection查询时又恢复默认 EXCLUDE。trash因此被排除；pending通常也被排除，但 shared-owner 且物理名满足 `MATCH_PENDING_FROM_FUSE` 的row仍是例外。于是 API式 `.pending-*`、非owner pending或trashed对象可能“旧 id路径看得到，稳定键搜索却找不到”。这是由两段源码组合得出的行为边界，不是canonical key本身丢失。

## 13. Cursor元数据、权限探针与getType各自回答不同的问题

普通 DB query完成后，只有 caller PID不同于 Provider进程且当前不是 FUSE thread，才对**经过 safeUncanonicalize后的 URI**调用 `setNotificationUri()`。然后 Provider用一个新 Bundle设置 honored数组。scanner/fs-id/version控制 Cursor与 target-Q 前的 thumbnail兼容 Cursor都在这段之前返回，因而没有这套 notification/extras。

`queryForSingleItem()` 复用正常 query，要求 Cursor恰好一行且能 moveToFirst；零行、多行、null或移动失败都转成 `FileNotFoundException`并关闭 Cursor。canonical、路径转 item URI和多处内部读取都依赖这个“恰好一行”契约。

权限探针有三种不同形态：

1. `enforceCallingPermission()` 先试 global；write时用 TYPE_UPDATE builder探测可写 row，再用 TYPE_QUERY探测可读 row，强类型 image/audio/video item只读可见而请求写时可构造用户授权恢复动作。传入空 projection仍展开公开列，只是调用方仅消费存在性。
2. MediaProvider自己的 `checkUriPermission(uri, uid, modeFlags)` 临时切到目标 uid，write flag选 TYPE_UPDATE，否则 TYPE_QUERY，并要求 query count恰为1才立即 GRANTED。它不调用 `safeUncanonicalize()`，canonical hint不会参与重定位。若末段不是有效 id且未请求 prefix，它随后也返回GRANTED；count不等于1的prefix collection则DENIED，但count恰为1仍在prefix检查之前GRANTED。必须把这个Provider override与UriGrantsManager的grant账检查分开：成对r48因动态门关闭，创建grant时根本不调用前者，prefix grant一旦建成也由后者的path-prefix循环消费。
3. framework明确 `getType()` 不要求 provider read/write permission，其 Transport也没有 calling package；MediaProvider对 item临时 `clearLocalCallingIdentity()` 后查询 MIME。它是 permission-free、Provider自身份下的窄类型探针，不代表原调用者获得 row或文件访问权。

`allowGlobal`、permission probe、canonical可读、MIME可解析因此是四个不同结论。安全分析必须记录调用的是哪一种 API，而不能只记“Provider查到了一行”。

### 练习 8：区分普通Cursor、single-item、grant探针与MIME探针

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final Cursor c = qb.query(helper, projection, queryArgs, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean callerIsRemote = mCallingIdentity.get().pid != android.os.Process.myPid();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (callerIsRemote && !isFuseThread()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'c.setNotificationUri(getContext().getContentResolver(), uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'c.setExtras(extras);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Cursor queryForSingleItem(Uri uri, String[] projection, String selection,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (c.getCount() < 1) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} else if (c.getCount() > 1) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public int checkUriPermission(@NonNull Uri uri, int uid,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'LocalCallingIdentity.fromExternal(getContext(), uid));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'type = TYPE_UPDATE;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (c.getCount() == 1) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if ((modeFlags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) == 0) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'android:grantUriPermissions="true"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'android:forceUriPermissions="true"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'private static final long MIN_DYNAMIC_PERMISSIONS_MP_VERSION = 301400000L;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F '"version": 300000000' packages/providers/MediaProvider/apex/apex_manifest.json
grep -n -F '&& isDynamicPermissionEnabledInMP()) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'forceMet = true;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'return readMet && writeMet && forceMet;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (pi.forceUriPermissions) {' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'targetHoldsPermission = false;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'boolean grantAllowed = pi.grantUriPermissions;' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'public void grantUriPermission(String toPackage, Uri uri, int modeFlags) {' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'final NeededUriGrants needed = mUgmInternal.checkGrantUriPermissionFromIntent(intent,' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
grep -n -F 'targetUid = checkGrantUriPermissionUnlocked(callingUid, targetPkg, grantUri, mode,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'needed.uris.add(grantUri);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'forceMet = (mAmInternal.checkContentProviderUriPermission(grantUri.uri,' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'res = checkUriPermissionLocked(grantUri, callingUid, modeFlags);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'perm = findOrCreateUriPermissionLocked(pi.packageName, targetPkg, targetUid, grantUri);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'perm.grantModes(modeFlags, owner);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'final UriPermission exactPerm = perms.get(grantUri);' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'if (perm.uri.prefix && grantUri.uri.isPathPrefixMatch(perm.uri.uri)' frameworks/base/services/core/java/com/android/server/uri/UriGrantsManagerService.java
grep -n -F 'return ActivityManager.getService().checkUriPermission(' frameworks/base/core/java/android/app/ContextImpl.java
grep -n -F 'if (getContext().checkUriPermission(uri, mCallingIdentity.get().pid,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '// getCallingPackage() isn'\''t available in getType(), as the javadoc states.' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'public String getType(Uri url) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final LocalCallingIdentity token = clearLocalCallingIdentity();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'new String[] { MediaColumns.MIME_TYPE }, null, null, null)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

先对MediaProvider override推演“collection恰有0/1/2条可见row”与prefix flag，再单独走一遍成对r48动态门关闭时的grant创建和exact/prefix账消费；最后说明为什么 `getType()` 成功不能作为 query授权证据。

## 14. canonicalize只为三种typed item生成辅助键，已经canonical的URI跳过row复核

framework Transport 对 `canonicalize()` 先做 read permission/AppOps检查，保留 calling package身份进入 Provider。MediaProvider会调用 URI matcher；若 query中已有 `canonical=1`，就原样返回，不核对 matcher是否命中受支持case，也不核对 row、title/`document_id`或清理额外参数。真正unknown的公开path会得到 `NO_MATCH`，却仍可从这条快路原样返回；hidden path无权限时仍会在 matcher内抛错。因此它只经过了 Transport并执行过match动作，不是受支持URI证明，更不是签名验证。

首次 canonicalize调用 `queryForSingleItem(uri, null, ...)`，仍受当前 caller 的正常 query、owner、state与volume约束。只支持三种 item：

- Audio item读取 `getDefaultTitleFromCursor()`；有 `title_resource_uri` 时尝试取 `Locale.US` 的默认资源标题，解析异常或结果为null时才回退row的 `title`；成功得到空串不会回退，后续因空值返回null。非空时才附加 `title=<值>&canonical=1`。
- Image/Video item读取 `document_id`；非空才附加 `document_id=<值>&canonical=1`。
- 其他match即使查询恰好一行，switch也不生成 URI，最终返回 null。源码只捕获 single-item产生的 `FileNotFoundException`；其他运行时错误并不承诺转换成 null。

辅助参数通过 `buildUpon().appendQueryParameter()` 附加在原 item URI上，原有 query参数与旧数值 id都保留。调用结果因此是“旧定位+恢复提示”，不是用 title/`document_id`替换主键。title可重复，`document_id`也只在当前数据库与政策约束下被搜索；后续恢复仍要求恰好一行。

“保留”还有一个反直觉后果：append不会替换同名旧参数，而 `Uri.getQueryParameter()` 读取第一个同名值。输入若已有错误title/`document_id`，Provider追加的真实hint排在后面，uncanonicalize仍读旧值；若输入先带 `canonical=0` 再由canonicalize追加 `canonical=1`，后续读取仍先得到0，甚至不会进入恢复分支。因此返回URI里“出现了新键”不等于该值会被后续代码采用。

Transport 会把传入 URI 的 userId在调用 Provider前拆掉，并在返回 URI上补回；这解决跨用户 URI表示，不等于canonical搜索跨 user执行。

## 15. uncanonicalize先试旧id再搜辅助键；r48的Image/Video精确分支比较错了字段

`uncanonicalize()` 先 match URI；没有 `canonical=1` 就原样返回。有标记时，它先取出 `title` 与 `document_id`，再用 `clearQuery()` 清掉**所有** query参数以避免递归，包括 canonical辅助键之外的 distinct、filter、limit等参数。

Audio恢复分两步：先按清过query的原 item URI做 single-item query，并比较保存title与当前 `getDefaultTitleFromCursor()`；相等就保留旧id。否则去掉末段id，在同一 collection/volume上按数据库 `title=?`搜索，恰好一行才返回其id。这里有一个可观察的语义缝：canonicalize可能保存资源的美式默认标题，fallback selection却直接比较数据库 `title`列；如果二者不同，稳定键搜索可能失败。

Image/Video也先查旧id，但 r48 exact分支调用的是 `Objects.equals(title, getDefaultTitleFromCursor(c))`。canonicalize为这两类保存的是 `document_id`，通常没有保存title，所以 exact分支并未比较`document_id`：row title非null时通常会进入fallback；row title恰为null时又可能直接接受旧id，而没有核对`document_id`。fallback才按 `document_id=?` 在同一 collection/volume查找，并同样要求恰好一行。

恢复还保留三组边界：

- 外层API先经过各自的Transport gate；Provider内部的exact与fallback不会重新进入Transport，但仍用当前calling identity建立builder，不绕过owner/capability；
- concrete volume仍限该卷；synthetic `external`则可在当前所有具体外置卷间搜索，重复hint会因多行而失败，但它不会进入internal或已不在当前集合的卷；
- exact item会强制include pending/trash，fallback collection恢复默认 EXCLUDE；trash被排除，pending只有 shared-owner 且物理名满足 FUSE pending公式时仍可能命中，所以id改变的其他隐藏状态row可在fallback中消失。

canonical与exact URI grant的组合还有一层不连续：外层Transport先在带canonical参数的原URI上验grant；uncanonicalize随后清空query，内部exact/fallback使用的URI通常不再与该grant精确相等。foreign row若只靠这条grant可读，恢复可能失败，safe层再退回完整原URI，主builder才重新命中grant并按旧id查询。换句话说，grant不会随hint迁到新id；旧id复用时却仍可能授权访问错误对象。

清空query还会改变非查询API的操作语义。`deleteInternal()` 在 safe恢复后才读取 `MediaStore.PARAM_DELETE_DATA`：调用者原本传 `deletedata=false` 只删数据库记录，成功恢复却会丢掉该参数并回到“尝试删除物理文件”的默认。`openFileCommon()` 也在 safe恢复后读取 `MediaStore.getRequireOriginal(uri)`；`requireOriginal` 被清掉后，原本应在缺少定位权限时抛错的请求可能改为返回redacted FD。这不是只有distinct/include-pending不同，而是恢复结果影响mutation与原始字节契约。

三种返回应分开看：

| `uncanonicalize()`结果 | `safeUncanonicalize()`交给外层的URI |
|---|---|
| 带标记但不属于Audio/Image/Video item | 在未抛其他异常时，已清掉全部query参数的原path URI |
| 支持的item exact或fallback成功 | 在未抛其他异常时，已清掉全部query参数、可能换成新id的URI |
| 支持的item fallback为零行或多行 | `uncanonicalize()`返回null，safe层恢复完整原canonical URI |

于是成功恢复会丢掉全部 URI query参数，失败却保留它们，include-pending、distinct、deletedata与requireOriginal等参数的后续效果也可能随分支不同。`safeUncanonicalize()` 只把null换回原URI，并不捕获一般异常；exact/fallback也只捕获 `FileNotFoundException`，match、数据库或其他运行时错误仍能中断外层操作。query、update、delete、openFile与typed-open都调用这层，所以“恢复失败”不会统一表现为立刻报错：它可能返回零行、在后续权限/文件链失败，甚至在旧id已复用且新row对caller可见时命中错误对象。这不是用SQL逃出builder policy，而是URI身份与授权绑定发生混淆，确实可能改变最终获准访问的对象；hint并非对象身份断言。

### 练习 9：逐行验证canonical生成、字段错配与失败回退

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public Uri canonicalize(Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if ("1".equals(uri.getQueryParameter(CANONICAL))) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'try (Cursor c = queryForSingleItem(uri, null, null, null, null)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'builder.appendQueryParameter(AudioColumns.TITLE, title);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '.getString(c.getColumnIndexOrThrow(MediaColumns.DOCUMENT_ID));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'builder.appendQueryParameter(MediaColumns.DOCUMENT_ID, documentId);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public Uri uncanonicalize(Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String title = uri.getQueryParameter(AudioColumns.TITLE);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String documentId = uri.getQueryParameter(MediaColumns.DOCUMENT_ID);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'uri = uri.buildUpon().clearQuery().build();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (Objects.equals(title, getDefaultTitleFromCursor(c))) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'AudioColumns.TITLE + "=?", new String[] { title }, null)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaColumns.DOCUMENT_ID + "=?", new String[] { documentId }, null)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private Uri safeUncanonicalize(Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Uri newUri = uncanonicalize(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (newUri != null) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return newUri;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'title = getDefaultTitle(titleResourceUri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'configuration.setLocale(Locale.US);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'uri = safeUncanonicalize(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'public Builder appendQueryParameter(String key, String value) {' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'query = Part.fromEncoded(oldQuery + "&" + encodedParameter);' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'Searches the query string for the first value with the given key.' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'public String getQueryParameter(String key) {' frameworks/base/core/java/android/net/Uri.java
grep -n -F 'String deleteparam = uri.getQueryParameter(MediaStore.PARAM_DELETE_DATA);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (redactionNeeded && MediaStore.getRequireOriginal(uri)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

构造四例：Audio旧id仍对、Audio标题重复、Image旧id已复用给另一`document_id`、pending Image改id。分别记录exact比较字段、fallback selection、row-count约束与 `safeUncanonicalize()` 最终交给主操作的 URI。

## 16. 用诊断矩阵找出“查不到、查错了、恢复错了”发生在哪一层

面对 MediaStore 读取异常，先采集 calling package/uid、target SDK、URI及全部 query参数、Bundle入参与归一化值、是否FUSE thread、projection、selectionArgs、volume attached/current集合，再按下表定位：

| 症状 | 首查层 | 关键反例 |
|---|---|---|
| 空 Cursor且无Provider SQL日志 | Transport permission/AppOps | projection非null时可直接空化 |
| concrete卷报不存在 | helper attached gate | external DB里有历史row也无济于事 |
| synthetic external漏掉旧卷 | current-volume row filter | 同一external DB不等于全部历史卷可见 |
| 普通DB query的unknown projection报错 | projection map | self也没有任意公开映射外列；target-Q前thumbnail早退除外 |
| `COUNT(*)`只在旧应用工作 | target-Q前greylist | 不是aggregation开关变化 |
| selection的OR仍看不到other-owner | trusted WHERE括号 | caller只能继续收窄 |
| Audio能见铃声却不能见普通other-owner音频 | Audio兼容OR | 不要套用Images/Video公式 |
| Files权限看见标准media type但看不见other-owner hidden MIME | options内部仍有owner项 | media_type 0补口不是全局能力 |
| 普通media/files收到item、collection或prefix grant后看见other-owner row | r48静态源检查→grant账exact/prefix命中→allowGlobal | 旧APEX不动态核验发起者row资格；派生/thumbnail没有统一builder过滤，部分audio view另限volume快照 |
| item能见pending、collection搜不到 | item覆盖MATCH | fallback collection恢复默认EXCLUDE；shared-owner FUSE pending例外 |
| honored列出没传的key | 路径声明语义 | 不是入参或效果回执 |
| canonical Image常落入fallback，或null title时反而接受旧id | exact分支比较title | 保存的辅助键其实是`document_id` |
| canonical恢复失败后仍继续操作 | safe fallback保留原URI | 后续仍可按旧id命中别的row |
| `getType()`成功但query/open失败 | 内部身份MIME探针 | MIME解析不是访问授权 |

整条读取链可压成一句话：**Transport先决定请求是否进入Provider，URI与volume决定查询域；在普通DB路径上，本地builder把公开列和调用者SQL限制在契约内，trusted WHERE把身份、owner、状态与卷直接下推到结果集；canonical只在这套政策之内用title或`document_id`尽力换回当前id，失败时还会保留旧URI继续走主链。**

下一章进入缩略图链：从 `ContentResolver.loadThumbnail()` 的 size与CancellationSignal开始，追到 typed-asset分派、Thumbnailer临时文件与并发发布、Audio封面/Image EXIF/Video帧提取，再闭合三类缓存失效、数据库UUID代际清理与idle孤儿回收。
