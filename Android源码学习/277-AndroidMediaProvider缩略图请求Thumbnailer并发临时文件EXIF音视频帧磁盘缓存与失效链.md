# 277 Android MediaProvider缩略图请求、Thumbnailer并发临时文件、EXIF音视频帧、磁盘缓存与失效链

## 1. 先看结论：一次缩略图请求跨过六个完成点，拿到Bitmap不代表六层都正确

本文固定在 Android 11 `android-11.0.0_r48`：`frameworks/base` 位于 `1d9b9ab57d844b18b3b1b4297725141e7788109b`，`packages/providers/MediaProvider` 位于 `47c141d93e93b25cc85c36f3579fda25a1695952`。现代缩略图链不是“查thumbnail表后返回Bitmap”，而是六段不同协议：

1. `ContentResolver.loadThumbnail()` 把目标Size编码进typed-asset请求，并建立远程取消通道；
2. `ContentProvider.Transport` 校验URI/user并执行组件级read/AppOps门；
3. MediaProvider识别请求、恢复canonical URI，再按Audio/Video/Image/Files/Downloads分派；
4. Provider在URI match后、类型分支前切到self身份，三类Thumbnailer先尝试磁盘缓存，miss才读取源文件并生成Bitmap；
5. 临时文件经JPEG压缩和`rename()`发布，调用者拿到的是已打开inode的FD；
6. 客户端ImageDecoder再次采样并处理可选orientation；更新、删除、UUID和idle维护则在之后尽力清陈旧缓存。

这六段不能互相替代：

| 观察结果 | 能证明什么 | 不能证明什么 |
|---|---|---|
| `loadThumbnail()` 返回Bitmap | typed AFD可解码，客户端流程完成 | Bitmap尺寸精确等于请求Size、源row此刻仍存在 |
| Provider缓存文件存在 | 某次发布留下了`id.jpg` | 源mtime、MIME、volume归属或内容仍匹配 |
| `loadThumbnail()`因取消抛出 | 某个客户端或Provider检查点观到了signal | 生成线程绝未完成、缓存路径绝未被发布 |
| 源row已经删除 | 数据库删除已完成 | 后台invalidate已完成、在途生成者不会重新发布旧JPEG |
| `.database_uuid`匹配 | 目录marker等于当前DB xattr | 每个known-id JPEG内容健康、源文件没有变化 |
| idle报告清了0项 | 没发现数字id不在全库known集合的当前卷文件 | 没有错卷、错类型、损坏或mtime陈旧缓存 |

排查时应沿着：**client request → Transport gate → Provider dispatch/identity → generator → temp inode publication → invalidation/reconciliation**。尤其要记住：r48 typed-thumbnail分支留下明确的对象级授权缺口，不能用“最终能查到源文件”反推调用者本来就有row权限。

## 2. loadThumbnail请求的是typed AFD；Size只是提示，客户端还会再采样

公开入口先要求content interface、URI、Size非null，CancellationSignal可空。它把`Size`转成`Point`放入新Bundle的`ContentResolver.EXTRA_SIZE`，固定以`image/*`调用`openTypedAssetFile()`；跨进程返回的是`AssetFileDescriptor`，不是Bitmap。若signal在Binder调用前已取消，`openTypedAssetFileDescriptor()`先抛；否则它创建远程transport并把本地signal连接过去。

客户端随后用ImageDecoder读取AFD。header callback固定所选allocator，先做最后一次取消检查，再用原图宽高分别对请求宽高做**整数除法**，取较大sample；只有sample大于1才调用`setTargetSampleSize()`。这不是精确resize或center-crop：1.9倍会因整数除法得到1而不缩，小图也不放大，最终尺寸可以大于或小于请求框。`Size`构造器也不拒绝0或负数，loadThumbnail只验对象非null；宽或高为0会在这里触发ArithmeticException，而不是统一变成IOException。

AFD extras还可通过`DocumentsContract.EXTRA_ORIENTATION`传角度；decode完成后客户端围绕中心旋转。MediaProvider自己的缩略图AFD没有设置这项，Image路径是在Provider生成阶段处理orientation；这个side channel主要供其他typed provider使用，不能与EXIF本身混成同一层。

### 练习 1：核对Size、typed AFD、远程取消与客户端二次采样

在源码根目录运行；也可把源码根目录作为第一个参数，从任意目录运行。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public @NonNull Bitmap loadThumbnail(@NonNull Uri uri, @NonNull Size size,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'Objects.requireNonNull(content);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'Objects.requireNonNull(uri);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'Objects.requireNonNull(size);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'public Size(int width, int height) {' frameworks/base/core/java/android/util/Size.java
grep -n -F 'opts.putParcelable(EXTRA_SIZE, Point.convert(size));' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final AssetFileDescriptor afd = content.openTypedAssetFile(uri, "image/*", opts,' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'remoteCancellationSignal = unstableProvider.createCancellationSignal();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'cancellationSignal.setRemote(remoteCancellationSignal);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'cancellationSignal.setRemote(null);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'private ICancellationSignal mRemote;' frameworks/base/core/java/android/os/CancellationSignal.java
grep -n -F 'mRemote = remote;' frameworks/base/core/java/android/os/CancellationSignal.java
grep -n -F 'if (signal != null) signal.throwIfCanceled();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final int widthSample = info.getSize().getWidth() / size.getWidth();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'final int heightSample = info.getSize().getHeight() / size.getHeight();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'decoder.setTargetSampleSize(sample);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'orientation.value = (extras != null) ? extras.getInt(EXTRA_ORIENTATION, 0) : 0;' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'm.setRotate(orientation.value, width / 2, height / 2);' frameworks/base/core/java/android/content/ContentResolver.java
```

分别令返回图为`190×190`、`200×100`、`40×40`，请求`100×100`；按整数sample推导header阶段是否缩放，并说明结果为何不是“必为100×100”。

## 3. Transport只有组件级read门；r48的thumbnail对象级授权缺口是真实控制流

Transport先`validateIncomingUri()`、去掉userId，再以`"r"`调用`enforceFilePermission()`。一般Provider可在这里依靠顶层read permission、path permission、AppOps或已有URI grant拒绝访问；但r48 MediaProvider的manifest是exported，且这个Provider没有顶层read/write或path permission。`enforceReadPermissionInner()`因此走`allowDefaultRead`，MediaProvider也没有给Transport配置额外read AppOp。`forceUriPermissions=true`影响系统URI grant建账策略，并不会自动改写这条typed-file Transport门。

Transport通过后才设置calling package并进入`openTypedAssetFileCommon()`。方法在`safeUncanonicalize()`之后直接留下“enforce caller access”注释；只要opts含`EXTRA_SIZE`且MIME filter以`image/`开头，它就调用`ensureThumbnail()`。后者只在clear之前用caller决定能否匹配hidden URI，随后把MediaProvider的**本地授权身份**替换成self；album查询、Files MIME探测、`queryForDataFile()`和源文件读取都按Provider身份进行。

因此，不能再把这段解释成“外层已完整验证row，self只是读取lower filesystem”。对普通public item URI，未带canonical标记时safe层也不会先查row；缓存miss的源row查询发生在self身份下，cache hit甚至完全不查询数据库。静态控制流由此允许没有该row读取能力的调用者，用猜测的public item id请求派生图；已有孤儿`id.jpg`还可能在源row不存在时被打开。这是r48源码层面的对象级信息暴露边界，具体设备复现还应记录厂商修改、卷状态与调用方式。

若请求不满足thumbnail条件，Provider才回退`openFileCommon(uri,"r",...)`，那条路径另有row、路径、pending ownership与redaction检查。相同typed入口因为一个Bundle key和MIME filter选择不同分支，权限强度并不相同。

### 练习 2：把组件门、对象级空缺和self身份接成一条链

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public AssetFileDescriptor openTypedAssetFile(String callingPkg,' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'uri = validateIncomingUri(uri);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'enforceFilePermission(callingPkg, attributionTag, uri, "r", null);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'boolean allowDefaultRead = (componentPerm == null);' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'if (allowDefaultRead) return MODE_ALLOWED;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'volatile int mReadOp = AppOpsManager.OP_NONE;' frameworks/base/core/java/android/content/ContentProvider.java
grep -n -F 'android:forceUriPermissions="true"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'android:exported="true"' packages/providers/MediaProvider/AndroidManifest.xml
grep -n -F 'private AssetFileDescriptor openTypedAssetFileCommon(Uri uri, String mimeTypeFilter,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final boolean wantsThumb = (opts != null) && opts.containsKey(ContentResolver.EXTRA_SIZE)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final LocalCallingIdentity token = clearLocalCallingIdentity();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'restoreLocalCallingIdentity(token);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return new AssetFileDescriptor(openFileCommon(uri, "r", signal), 0,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

对同一个无权读取的public image item，分别推演“带EXTRA_SIZE且filter为image/*”和“不带key”两次typed open：指出哪条链切self、哪条链进入普通open权限与redaction逻辑。

## 4. Provider只看EXTRA_SIZE是否存在；分派、canonical与volume决定源和缓存落点

`openTypedAssetFileCommon()`并不读取`EXTRA_SIZE`的Point值，也不验证其类型；直接调用typed API的客户端只要放入同名key即可触发，实际生成始终使用Provider的固定`mThumbSize`。MIME条件是忽略大小写的`image/`前缀，不是检查源文件本身是否为图片；Audio和Video也通过`image/*`请求它们的可视封面。

方法先运行`safeUncanonicalize()`。成功恢复可能换id并清掉全部query参数，失败则保留原canonical URI；第276章已经说明同名参数遮蔽、旧id复用与exact grant失配边界。缩略图缓存名只取safe层最终URI的末段id，不把canonical hint、调用Size或query参数纳入key。

`ensureThumbnail()`在切self之前先match：支持Audio album item、Audio media item、Video item、Image item、Files item与Downloads item。album分支在self身份下按`album_id`查询Audio collection，没有sort且只`moveToFirst()`，所以代表歌曲不稳定，缓存复用该歌曲id而不是album id。Files/Downloads也在self身份下用`getType()`解析media type；非audio/video/image转成FileNotFoundException。

缓存路径先`resolveVolumeName()`：synthetic `external`被映成`external_primary`。若唯一id实际属于secondary volume，且该secondary仍在当前external-volume缓存中，源查询可从共享external DB的当前卷查询域找到它，JPEG却仍写到primary根。internal则没有可关联的单一volume path，`getVolumePath(internal)`会失败。URI的volume因此既影响数据库搜索域，也独立影响派生文件落点；不能概括成“总在源文件所在卷”。

`ensureThumbnail()`只把捕获到的IOException记录并转为只带message的FileNotFoundException；OperationCanceledException及NullPointerException等运行时异常不在这个catch内，仍会向外传播。

### 练习 3：验证触发条件、类型分派与synthetic external落点

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'uri = safeUncanonicalize(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '&& MimeUtils.startsWithIgnoreCase(mimeTypeFilter, "image/");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final ParcelFileDescriptor pfd = ensureThumbnail(uri, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case AUDIO_ALBUMS_ID: {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'MediaStore.Audio.Media.ALBUM_ID + "=" + albumId, null, null, signal)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (c.moveToFirst()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case AUDIO_MEDIA_ID:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case VIDEO_MEDIA_ID:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case IMAGES_MEDIA_ID:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case FILES_ID:' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final int mediaType = MimeUtils.resolveMediaType(getType(uri));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (MediaStore.VOLUME_EXTERNAL.equals(volumeName)) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return MediaStore.VOLUME_EXTERNAL_PRIMARY;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'case MediaStore.VOLUME_INTERNAL:' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
grep -n -F 'throw new FileNotFoundException(volumeName + " has no associated path");' packages/providers/MediaProvider/src/com/android/providers/media/util/FileUtils.java
```

构造一个synthetic external URI，其id对应仍在当前卷缓存中的secondary row：分别写出`queryForDataFile()`能搜索的row卷集合与`getThumbnailFile()`选择的物理根，解释两者为何可以不同。

## 5. 固定缓存只由volume映射、类型目录与row id寻址；fast hit不验证源row

Provider启动时取显示宽高较小边的一半，形成正方形`mThumbSize`。它在本次Provider生命周期内固定，不读取调用者Size，但只是生成器的粗略target；Resizer整数采样、原图不放大与媒体特定API都意味着缓存Bitmap的实际像素和长宽比未必固定。Audio、Video、Image子类只分别选择`Music`、`Movies`、`Pictures`；公共Thumbnailer把路径拼成：

`<resolved-volume-root>/<type-dir>/.thumbnails/<row-id>.jpg`

key没有请求宽高、源mtime、源size、MIME、generation、owner或内容hash。相同id在三类目录是三个可能文件；普通`files` row的类型改变在listener启用时会让维护链保守删除三份。多个具体external卷共用一套数据库id空间，但路径还有volume root；synthetic external例外地固定落到primary。

`Thumbnailer.ensureThumbnail()`第一步只是算出文件路径并用read-only打开。成功就立刻返回，不调用`queryForDataFile()`、不检查源row、状态、mtime、MIME、JPEG完整性，也不观察传入signal。直接Audio/Video/Image item会不经其他DB查询就进入此处；album URI在外层先查代表歌曲，Files/Downloads也先经`getType()`。删除与invalidate异步之间的窗口、物理删除失败留下的文件、错误卷中的known-id文件，都可能被这个fast path直接消费。

miss后才`mkdirs()`并生成；`mkdirs()`返回值没有检查，实际失败会在`createTempFile()`处暴露。缓存不是MediaStore源row，也没有独立LRU或多规格索引；应用若反复需要小图，仍应自行维护有边界的内存缓存。

## 6. 并发不是同id任务去重：每个线程生成自己的inode，最后rename者决定路径

cache miss时没有per-id锁、future表或“正在生成”标志。每个线程在目标目录建立唯一临时文件，并在生成前分别以write-only和read-only打开同一临时inode。Bitmap生成完成后，代码把它压成JPEG quality 90，再用`Os.rename(temp, final)`发布；返回的是预先打开的read FD之`dup()`。

预开read FD解决的是rename后的inode稳定性。两个线程T1/T2竞争时：

1. T1、T2各写自己的temp inode，旁观者看不到final半成品；
2. T1先rename，T1调用者的dup仍读T1 inode；
3. T2后rename覆盖路径，T2调用者读T2 inode；
4. 后续fast hit读路径上最后一次成功rename留下的inode。

这不是“所有并发请求共享同一个结果”。它避免通过final路径读到正在写的文件，却允许重复解码、重复压缩，也不保证各调用者像素完全相同。`finally`关闭本地FD并尝试删除temp路径；rename成功后那个路径通常已不存在。

发布协议还有两个缺口。第一，`Bitmap.compress()`的boolean返回值被忽略，false也继续rename，因而可以发布空或不完整文件；第二，这段Java代码没有显式flush/fsync/目录fsync，也没有在compress与rename之间重新检查取消。rename提供命名空间原子替换，不等于持久化事务或内容正确性证明。`deleteAndInvalidate()`自身也忽略`File.delete()`的boolean。

### 练习 4：逐行推演temp双FD、JPEG发布与两个竞争调用者

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public ParcelFileDescriptor ensureThumbnail(Uri uri, CancellationSignal signal)' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return FileUtils.openSafely(thumbFile,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final File thumbDir = thumbFile.getParentFile();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'thumbDir.mkdirs();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final File thumbTempFile = File.createTempFile("thumb", null, thumbDir);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'thumbWrite = FileUtils.openSafely(thumbTempFile,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'thumbRead = FileUtils.openSafely(thumbTempFile,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final Bitmap thumbnail = getThumbnailBitmap(uri, signal);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'thumbnail.compress(Bitmap.CompressFormat.JPEG, 90,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Os.rename(thumbTempFile.getAbsolutePath(), thumbFile.getAbsolutePath());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'return thumbRead.dup();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'FileUtils.closeQuietly(thumbWrite);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'deleteAndInvalidate(thumbTempFile);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'file.delete();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

画出T1/T2的temp1、temp2、final与两个dup FD；分别标出第一次rename、第二次rename和finally后每个FD仍指向哪个inode。

## 7. Audio先取embedded picture；旁搜受卷、目录层级、后缀和评分共同限制

AudioThumbnailer先通过self身份的`queryForDataFile()`取得源路径，再把固定`mThumbSize`交给`ThumbnailUtils.createAudioThumbnail()`。生成器起点检查取消，用MediaMetadataRetriever读embedded picture；有字节就交给ImageDecoder和Resizer。Retriever的RuntimeException被转成IOException，但embedded图的DecodeException属于IOException链，可直接外抛，不会再尝试旁搜。

只有“没有embedded picture”才考虑相邻文件。`Environment.getExternalStorageState(file)`为`MEDIA_UNKNOWN`时拒绝；直接父目录名大小写敏感地等于系统常量`Download`时拒绝，但`Download/sub/song.mp3`不会命中这条；位于外置根的顶层文件也拒绝。随后只枚举源文件父目录下大小写归一后以`.jpg`或`.png`结尾的条目，不递归、不看WebP，也不调用`isFile()`，所以同名目录也能成为最高分候选并在解码时失败。

评分从高到低是精确`albumart.jpg`、以albumart开头的JPG、包含albumart的JPG、其他JPG、PNG零分。stream的`max()`仍会在全是PNG时选出一个零分候选；相同分数时Comparator返回0，具体候选取决于`listFiles()`顺序，不能写成稳定字典序。枚举完成后再检查一次取消，最后用Resizer解码候选。

### 练习 5：验证Audio embedded、目录拒绝与PNG零分候选

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static @NonNull Bitmap createAudioThumbnail(@NonNull File file, @NonNull Size size,' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'retriever.setDataSource(file.getAbsolutePath());' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'final byte[] raw = retriever.getEmbeddedPicture();' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'throw new IOException("No embedded album art found");' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'parent.getName().equals(Environment.DIRECTORY_DOWNLOADS)' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'throw new IOException("No thumbnails in top-level directories");' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'return (lower.endsWith(".jpg") || lower.endsWith(".png"));' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'if (lower.equals("albumart.jpg")) return 4;' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'if (lower.endsWith(".jpg")) return 1;' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'return 0;' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'Arrays.asList(found).stream().max(bestScore).orElse(null);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'return ImageDecoder.decodeBitmap(ImageDecoder.createSource(bestFile), resizer);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'return ThumbnailUtils.createAudioThumbnail(queryForDataFile(uri, signal),' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

给出`cover.png`、`z.png`、`photo.jpg`、`myalbumart.jpg`四个候选，按score判断胜者；再删去两个JPG，说明为什么仍可能返回PNG而不是“No album art”。

## 8. Image有HEIF、EXIF thumbnail与全图三路；只有非full-decode结果可能手工旋转

Image生成器先检查取消，用文件名推导MIME；无法识别时`MediaFile.getMimeTypeForFile()`返回默认MIME而不是null。r48的`isExifMimeType()`为简化而把所有识别为`image/*`的类型都当作“可能有EXIF”，于是先构造ExifInterface，并只把`ROTATE_90/180/270`映成角度；flip、transpose、transverse等镜像orientation没有显式处理。ExifInterface构造若抛IOException会直接离开方法，也不会继续full ImageDecoder fallback。

HEIF/HEIC及sequence类型先用MediaMetadataRetriever的`getThumbnailImageAtIndex()`取图；RuntimeException转IOException。只要结果非null，就不会尝试EXIF bytes或全图。其他EXIF格式会尝试`getThumbnailBytes()`；embedded bytes解码失败只记录warning并继续。这里用Resizer缩小embedded图。

在全图fallback之前还有一个取消点。全图由ImageDecoder直接读File并使用Resizer，native decoder选择respect EXIF orientation，所以代码立即return，不再套手工旋转。只有HEIF retriever结果或EXIF embedded图会走末段Matrix旋转；HEIF native thumbnail路径还明确把自身rotation angle清零，交给Java的有限switch处理。这也解释了镜像orientation为何是特定分支的r48缺口，而不是所有全图解码都错误。

最终公共Thumbnailer无论源是透明PNG、广色域HEIF还是JPEG，都再压成不透明语义的JPEG 90缓存；原EXIF与alpha不会作为原格式元数据保留。

### 练习 6：区分HEIF、EXIF bytes、全图与orientation责任

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public static @NonNull Bitmap createImageThumbnail(@NonNull File file, @NonNull Size size,' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'final String mimeType = MediaFile.getMimeTypeForFile(file.getName());' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'return (mimeType != null) ? mimeType : MIME_TYPE_DEFAULT;' frameworks/base/media/java/android/media/MediaFile.java
grep -n -F 'return isImageMimeType(mimeType);' frameworks/base/media/java/android/media/MediaFile.java
grep -n -F 'if (MediaFile.isExifMimeType(mimeType)) {' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'case ExifInterface.ORIENTATION_ROTATE_90:' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'case ExifInterface.ORIENTATION_ROTATE_180:' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'case ExifInterface.ORIENTATION_ROTATE_270:' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'if (mimeType.equals("image/heif")' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'bitmap = retriever.getThumbnailImageAtIndex(-1,' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'final byte[] raw = exif.getThumbnailBytes();' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'bitmap = ImageDecoder.decodeBitmap(ImageDecoder.createSource(raw), resizer);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'bitmap = ImageDecoder.decodeBitmap(ImageDecoder.createSource(file), resizer);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F '// Use ImageDecoder to do full file decoding, we don'\''t need to handle the orientation' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'm.setRotate(orientation, width / 2, height / 2);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'Bitmap.createBitmap(bitmap, 0, 0, width, height, m, false);' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'videoFrame->mRotationAngle = 0;' frameworks/base/media/jni/android_media_MediaMetadataRetriever.cpp
grep -n -F 'SkAndroidCodec::ExifOrientationBehavior::kRespect' frameworks/base/native/graphics/jni/imagedecoder.cpp
```

分别为普通JPEG带embedded thumb、HEIC retriever返回图、EXIF bytes损坏后full decode三例标出谁处理orientation；再说明镜像orientation为何不能被三种rotate case覆盖。

## 9. Video优先embedded picture，否则取中点closest-sync帧；Provider与客户端是两次粗缩放

Video开头只检查一次取消。MediaMetadataRetriever有embedded picture时，用Resizer解码并立即返回；否则把preferred config设为ARGB_8888，解析video width、height与毫秒duration，把时长换成微秒后除以2。

若Provider固定请求的宽和高都大于原视频尺寸，代码用`getFrameAtTime()`返回native frame，不放大；只要任一维不满足，就调用`getScaledFrameAtTime()`并传入`mThumbSize`的正方形宽高，native scaled API仍保持源长宽比，并非强制得到正方形。它请求的是中点附近`OPTION_CLOSEST_SYNC`同步帧，不是精确中点帧；native还按frame rotation旋转并在90/270度交换宽高。metadata为null、不是数字或retriever返回null都会形成RuntimeException，再统一转成IOException。embedded bytes存在但ImageDecoder解码失败时，IOException会直接离开方法，不会继续取视频帧。

生成出来的Bitmap先被Provider压成JPEG；它使用固定rough target，却没有固定实际像素尺寸或长宽比。客户端再按自己请求Size做整数sample。较小请求可能得到二次downsample，较大请求不会把Provider缓存放大。两次都只是粗略缩小，没有任一层保证精确填满、裁剪到或匹配请求框的每个像素维度。

Resizer同时用于Audio embedded、旁搜图、Image embedded/full及Video embedded：header时检查取消、固定software allocator、用整数sample防御超大输入。Video frame fallback不经过ImageDecoder Resizer，依靠Retriever的native/scaled API。

## 10. 取消是分散检查点，不是回滚事务；IOException与运行时异常的外观也不同

客户端在发Binder前检查一次，把signal连接到远程transport，并在ImageDecoder header再检查。MediaProvider把signal传给Thumbnailer和生成器，但缓存fast open不检查；三个生成器的覆盖也不一致：Audio有起点、Resizer header及旁搜后检查，Image有起点、Resizer header及full fallback前检查，Video frame路径只有起点，Retriever本身没有接收signal。header callback是在header已经解析后才执行，也不能理解成逐像素解码都可抢占。

公共Thumbnailer在Bitmap返回后不再检查signal；JPEG compress、rename与dup也没有取消点。因此：

- 在已覆盖检查点取消，可抛OperationCanceledException，finally删temp，不发布；
- 在长时间Retriever、解码或compress期间取消，何时被观察取决于下一检查点；
- 若最后一个检查点已通过，调用者取消甚至停止等待后，线程仍可能压缩并发布可复用缓存；
- cache hit可能在Provider侧完全无取消检查，但客户端后续decode header仍有机会观察signal。

异常外观还取决于取消发生在哪个try内。Audio/Video的embedded ImageDecoder位于捕获RuntimeException的Retriever块中，其Resizer抛出的OperationCanceledException会先被包装成IOException，再被MediaProvider改成FileNotFoundException；Audio起点/旁搜后、Image各checkpoint及Video起点的OperationCanceledException则可作为运行时异常越过MediaProvider的IOException catch。压缩返回false又不是异常，会继续rename。上层若只记录统一失败文案，就会掩盖源/解码IO、被包装或未包装的取消、runtime bug或已发布坏JPEG之间的差异，不能只按一个错误码处理。

### 练习 7：标出每个取消点和最后可阻止rename的位置

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'cancellationSignal.throwIfCanceled();' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F 'cancellationSignal.setRemote(remoteCancellationSignal);' frameworks/base/core/java/android/content/ContentResolver.java
grep -n -F '// One last-ditch check to see if we'\''ve been canceled.' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'if (signal != null) signal.throwIfCanceled();' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'public static @NonNull Bitmap createVideoThumbnail(@NonNull File file, @NonNull Size size,' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'mmr.getScaledFrameAtTime(thumbnailTimeUs, OPTION_CLOSEST_SYNC,' frameworks/base/media/java/android/media/ThumbnailUtils.java
grep -n -F 'thumbnail.compress(Bitmap.CompressFormat.JPEG, 90,' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Os.rename(thumbTempFile.getAbsolutePath(), thumbFile.getAbsolutePath());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '} catch (IOException e) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'throw new FileNotFoundException(e.getMessage());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

对“取消发生在Video retriever取帧期间”和“取消发生在Bitmap已返回、compress进行中”两例，找下一取消检查点；若没有，说明为什么最终缓存仍可能出现。

## 11. legacy API只是现代loadThumbnail包装；sPending共享signal却没有引用计数

已废弃的Images/Video `getThumbnail()`先把kind映成Size，再调用同一`ContentResolver.loadThumbnail()`；传入的`BitmapFactory.Options`没有参与现代解码。Provider也把旧albumart、image/video thumbnail openFile URI重定向到`ensureThumbnail()`。所以“legacy API”不等于继续由legacy thumbnails表生成modern文件。

`InternalThumbnails.sPending`是客户端进程内的`Uri→CancellationSignal`表，不是Provider任务去重。相同URI并发调用会共享signal并各自进入Provider生成；共享的本地canceled位能让各调用后续客户端检查点失败，但一个CancellationSignal只有一个`mRemote`。第二个open会用R2覆盖R1，任一open返回时的`setRemote(null)`又可能清掉另一个仍在途请求的transport，所以一次cancel不能可靠扇出到所有Provider工作。

每个legacy调用finally还直接`remove(uri)`，没有引用计数或按value条件删除：先完成的调用会让仍运行调用失去公开cancel入口；更晚第三个调用放入新signal时，较早第二个调用的finally还可能把第三个映射删掉，形成ABA式竞态。

modern缓存是三类目录下的`id.jpg`，不依赖legacy `thumbnails`或`videothumbnails` row。那两表仍可由兼容URI直接读写，并在本章追踪的invalidate/prune中维护。r48另有可查询、插入的legacy `album_art` table，但这两个维护例程不处理它；不能把“三类modern生成缓存”、“thumbnails两表及_data文件”和`album_art`并成一套状态。

## 12. 失效触发点分同步与后台，也有明确不触发的窗口

`OnFilesChangeListener`由`files`表的insert/update/delete trigger驱动。listener启用时，普通`files` row的media type改变会后台删除旧id的三类缓存，delete也会后台invalidate并撤URI grant；这不是“任何表的任何row”。schema write lock由当前线程持有时，三个scalar callback又都会被抑制，所以schema upgrade/rebuild内的files变更不会走这条listener链。普通insert不清缓存，因为正常AUTOINCREMENT新id不应已有文件，但marker缺失或外部残留仍是反例。

update路径先决定`triggerInvalidate/triggerScan`并在mutation前快照id：

- 显式或movement计算出的`DATA`会令triggerInvalidate；
- 非self对被过滤的scanner控制列做修改，会令triggerScan；但若所有输入列都被移除，稍后的empty早退让扫描和失效都不执行；
- 非self请求包含`IS_PENDING`会令triggerScan，并清DATE_MODIFIED/SIZE强制发布扫描；
- 只要任一flag为true，update之后就为快照id后台invalidate；需要scan时还做blocking scan；
- self调用会把triggerScan重置为false，但不会清已经设置的triggerInvalidate。

非pending源文件通过Provider以write FD打开时，返回PFD被OnCloseListener包装；close回调无论远程writer是否报告异常，都先invalidate、再失效FUSE dentry并扫描。pending row不包装这个listener，因此pending期间重复写入不会在每次close清缓存；发布时的`IS_PENDING` update才走上面的invalidate/scan。若永不发布，已有缓存可继续陈旧。

所有后台触发都留下完成点窗口。DB update/delete返回，不代表`id.jpg`已经消失；而直接item的fast path不查row，删除后的旧图在invalidate执行前仍可能被打开。更隐蔽的是生成与invalidate之间无锁、也无generation校验：生成者取得旧Bitmap后，invalidate可以先删路径并返回，该生成者再把旧内容rename回final路径。所以即使invalidate完成也不是“没有在途旧图会后发”的屏障。scanner若以self身份只更新metadata且不含DATA，本段update逻辑也不会单独把它当失效理由；正确性依赖写close、path/type/delete及其他调用点覆盖。

## 13. 一次invalidate先删三类modern路径，再用事务清legacy row；两者并不原子

`invalidateThumbnailsInternal()`从URI解析数字id，依次对Audio、Video、Image Thumbnailer调用invalidate；也就是不先判断实际media type，保守尝试删除三个目录的同名`id.jpg`。每次`deleteAndInvalidate()`直接`File.delete()`并失效FUSE dentry，delete boolean未检查；三次调用共用一个外层IOException catch，所以前一项的路径/卷解析异常会跳过后两项。普通`File.delete()`返回false本身不会抛这个checked exception。

modern删除发生在数据库helper事务之前。随后它按同id union查询legacy `thumbnails._data`和`videothumbnails._data`，对每个物理路径调用`deleteIfAllowed()`，最后无条件删除两表row并提交。`deleteIfAllowed()`捕获全部异常而不阻止row删除，因此可能留下legacy文件却删掉索引。这里不查也不删`album_art`。反过来，卷helper不可用时，三类modern删除已尝试，方法却在legacy事务前return。

传入URI的volume也重要：`getThumbnailFile()`会把synthetic external映成primary；常规listener则传concrete Files URI，只删除对应具体根。secondary源row若曾通过synthetic external把JPEG写入primary，后续源update会拿secondary URI删secondary路径，primary的known-id旧图因而可躲过单项invalidate和idle。detached卷与手工残留同样不能指望任一单次invalidate自动遍历所有volume。

### 练习 8：核对三类modern删除、legacy物理文件与row事务边界

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private void invalidateThumbnailsInternal(Uri uri) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final long id = ContentUris.parseId(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mAudioThumbnailer.invalidateThumbnail(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mVideoThumbnailer.invalidateThumbnail(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'mImageThumbnailer.invalidateThumbnail(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'deleteAndInvalidate(getThumbnailFile(uri));' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper = getDatabaseForUri(uri);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'helper.runWithTransaction((db) -> {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'select _data from thumbnails where image_id=?' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '+ " union all select _data from videothumbnails where video_id=?",' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'deleteIfAllowed(uri, Bundle.EMPTY, path);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execSQL("delete from thumbnails where image_id=?", new String[] { idString });' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execSQL("delete from videothumbnails where video_id=?", new String[] { idString });' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'qb.setTables("album_art");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Log.e(TAG, "Couldn'\''t delete " + path, e);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
```

分别让“第一项Audio modern删除在路径/卷解析时抛IOException”和“legacy物理unlink失败”：列出三类文件、legacy row与事务最终可能留下的状态，证明invalidate不是单一原子提交。

## 14. database UUID只防数据库代际错配；缺marker、异步attach与删除失败都留缝

external DB文件用xattr `user.uuid`保存代际标识；缺失时随机生成并写回。每个当前具体卷的`Music/.thumbnails`、`Movies/.thumbnails`、`Pictures/.thumbnails`各有`.database_uuid`文本marker。

`ensureThumbnailsValid()`按目录比较：marker缺失时**直接信任现有缓存**并写当前UUID，不清目录；marker不同时遍历删除目录内容，再写新marker；相同时不做内容检查。三目录循环只有一个外层IOException catch，任一目录读/写marker失败都会跳过后续目录。缺marker且目录已有陈旧`id.jpg`，就会把它们收编进新代际。遍历与`File.delete()`又是best effort，即使某些文件删除失败，后续写新marker仍可能让残留在下次被视为同代。

卷attach把名字加入attached集合并发送通知后，将default folders与UUID校验任务提交到ForegroundThread，然后返回；返回不构成该任务的完成屏障。在任务完成前，public URI已经可能通过helper gate，旧缓存fast hit也可能先被读取。idle维护会先扫描各当前卷、再在事务中做UUID校验，最后统一prune；它是周期性收敛，不是attach返回时的同步屏障。

UUID只能发现“DB xattr与目录marker不同”。`createLatestSchema()`会在同一个SQLite文件上清表并让id从头编号，却不轮换这个文件的xattr；例如in-place downgrade/wipe可让旧marker继续匹配，新row复用旧id。源文件内容改变、MIME变化、JPEG损坏、错目录和错卷也都不会改变DB UUID；这些必须依靠事件invalidate或更细的reconciliation。

## 15. idle prune只问“数字basename是否属于全库known id”；因此有五类盲区

`pruneThumbnails()`先从external DB的整个`files`表收集所有id并排序，没有同时读取`volume_name`或`media_type`，目录与非媒体row的id也在集合内。然后只遍历**当前**external volumes的三个现代目录：`.database_uuid`永远跳过；其余条目先去扩展名，再把basename解析long。只要这个数字在全库known集合中就保留，否则非数字、未知id文件和目录都会经`deleteAndInvalidate()`尝试删除；非空目录、只读卷或普通unlink失败仍可能残留，因为delete boolean没有检查。

这个判据至少有五类盲区：

1. source row仍known但源mtime/bytes已经变化，旧JPEG保留；
2. known image id放在Music目录，仍保留，不校验media type；
3. secondary row的id文件放在primary目录，仍保留，不校验row volume；
4. `123.bad`只要123 known也保留，不要求`.jpg`或可解码；
5. detached/ejected卷不在current names中，目录根本不遍历。

所以idle不能修复known-id的损坏`id.jpg`；这类固定名缓存下次仍会fast hit，客户端decode反复失败，直到在途生成者结束后又有一次invalidate成功删除，或人工删除。`123.bad`虽然也能躲过prune，但fast path只会打开固定的`123.jpg`；UUID匹配对两种残留都无助。

最后两条SQL只删除legacy thumbnails/videothumbnails中source row已不存在的**数据库row**，没有读取`_data`并删除物理文件；legacy孤儿文件仍可能留在磁盘。这与单项invalidate的“先尝试删legacy物理路径、再删row”不同，两者都不处理`album_art`表。

### 练习 9：证明idle判据忽略volume、type、mtime和legacy物理路径

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private int pruneThumbnails(@NonNull SQLiteDatabase db, @NonNull CancellationSignal signal) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final LongArray knownIds = new LongArray();' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.query(true, "files", new String[] { BaseColumns._ID },' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'Arrays.sort(knownIdsRaw);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'for (String volumeName : getExternalVolumeNames()) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (Objects.equals(thumbFile.getName(), FILE_DATABASE_UUID)) continue;' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final String name = FileUtils.extractFileName(thumbFile.getName());' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'final long id = Long.parseLong(name);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'if (Arrays.binarySearch(knownIdsRaw, id) >= 0) {' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'deleteAndInvalidate(thumbFile);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execSQL("delete from thumbnails "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '+ "where image_id not in (select _id from images)");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'db.execSQL("delete from videothumbnails "' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F '+ "where video_id not in (select _id from video)");' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'ensureThumbnailsValid(volumeName, db);' packages/providers/MediaProvider/src/com/android/providers/media/MediaProvider.java
grep -n -F 'private void createLatestSchema(SQLiteDatabase db) {' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'makePristineSchema(db);' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'private static final String XATTR_UUID = "user.uuid";' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
grep -n -F 'return new String(Os.getxattr(db.getPath(), XATTR_UUID));' packages/providers/MediaProvider/src/com/android/providers/media/DatabaseHelper.java
```

把`7.jpg`放到primary Music、secondary Pictures两处，并令files表只有`id=7, volume=secondary, media_type=image`：推导idle会保留哪几个文件，再说明它为何无法判断任一JPEG是否新鲜。

## 16. 用诊断矩阵区分请求、授权、生成、发布与维护故障

定位缩略图异常时，至少记录caller uid/package、原URI及canonical/query参数、requested Size、typed opts/MIME filter、resolved volume、row id/type/path、cache路径、signal时间线、DB UUID/目录marker及最近一次mutation。再按下表判断：

| 症状 | 首查完成点 | r48关键反例 |
|---|---|---|
| 无媒体读取能力却拿到缩略图 | typed Transport→Provider identity | public item没有对象级enforce，随后切self；cache hit不查row |
| 请求大图却只得较小图 | Provider固定mThumbSize | 客户端只downsample，不负责放大 |
| 请求100×100却结果更大 | 客户端整数sample | 190/100得到sample 1 |
| album封面偶尔换图 | 无sort的首row选择 | cache按代表歌曲id，不按album id |
| 两个并发调用像素或FD不同 | temp inode竞争 | 两者各自生成，final路径最后rename者胜 |
| 取消后磁盘仍出现JPEG | 最后取消点→compress/rename | Video与公共发布尾段没有再次检查 |
| 同一id反复decode失败 | fast hit与idle known-id盲区 | 存在即返回，prune不验格式/内容/mtime |
| 删除后仍能读旧图 | DB完成→后台invalidate与在途发布 | 直接item fast hit不查源row；invalidate后还可被旧生成者rename回来 |
| DB重建后第一次仍命中旧图 | UUID marker缺失 | 缺marker选择收编，不清目录 |
| secondary源图的缓存出现在primary | synthetic external resolve | 查询域可覆盖当前多个具体卷，缓存根固定primary |
| legacy row消失但文件仍在 | idle legacy SQL | prune只删row；单项invalidate的unlink也可失败 |
| cancelThumbnail只影响部分并发 | 客户端sPending竞态 | 共享signal无引用计数，finally无条件remove |

整条链可压成一句话：**客户端Size与取消只是请求协议，Transport组件门没有补齐r48对象级thumbnail授权；Provider按self身份、固定rough target生成，temp inode与rename只解决可见性竞争，最终正确性仍依赖非原子的事件invalidate、UUID代际检查和只认known id的idle收敛。**

下一章进入MediaDocumentsProvider：从ready gate与四类roots开始，追documents/buckets虚拟层级、MediaStore与SAF URI互转、collection观察通知、MANAGE_DOCUMENTS/具体grant门，以及只读内容面与可删除协作之间的权限差异。
