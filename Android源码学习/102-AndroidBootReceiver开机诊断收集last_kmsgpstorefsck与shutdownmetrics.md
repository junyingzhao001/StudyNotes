# 102 Android BootReceiver 开机诊断收集：last_kmsg、pstore、fsck 与 shutdown metrics

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 核心文件：`frameworks/base/core/java/com/android/server/BootReceiver.java`  
> 本章目标：理解系统启动后如何收集上一轮运行和关机遗留的诊断证据，如何去重、截断、分类写入 DropBox，并把文件系统与关机指标写入 statsd。  
> 环境：macOS 只读源码，不要求编译、刷机或连接设备。

---

## 1. BootReceiver 的名字容易误导

它确实继承 `BroadcastReceiver`，但主要工作不是向 App 宣告“手机开机了”，而是在系统完成启动后执行一批非关键、可能涉及磁盘 I/O 的诊断整理：

```text
读取上次 boot headers
处理 recovery aftermath
收集 last_kmsg / pstore
提取 SELinux audit
读取 fsck 结果
记录 mount/shutdown 指标
扫描 native tombstone
建立 FileObserver 监听后续 tombstone
清理远古 updater 下载记录
```

它更像“开机后的事故现场整理员”。

---

## 2. 类注释已经给出边界

源码注释：

```java
/**
 * Performs a number of miscellaneous, non-system-critical actions
 * after the system has finished booting.
 */
```

三个关键词：

- `miscellaneous`：汇聚多种来源，不是单一协议。
- `non-system-critical`：失败应记录，不能拖垮启动。
- `after ... booting`：将慢诊断工作移出启动关键阶段。

---

## 3. 源码地图

```text
frameworks/base/core/java/com/android/server/BootReceiver.java
frameworks/base/core/res/AndroidManifest.xml
frameworks/base/core/java/android/os/DropBoxManager.java
frameworks/base/services/core/java/com/android/server/DropBoxManagerService.java
frameworks/base/core/java/android/os/RecoverySystem.java
frameworks/base/core/java/android/os/FileUtils.java
frameworks/base/core/java/android/util/AtomicFile.java
frameworks/base/cmds/statsd/src/atoms.proto
system/core/init/reboot.cpp
system/core/fs_mgr/
```

本章以 BootReceiver 为中心，只在解释数据生产者时向 init、fs_mgr、recovery 等方向展开。

---

## 4. Manifest 入口

真实声明位于：

```text
frameworks/base/core/res/AndroidManifest.xml
```

可以搜索：

```bash
rg -n "com.android.server.BootReceiver" \
  frameworks/base/core/res/AndroidManifest.xml
```

BootReceiver 属于 Framework 系统包中的接收器，不是第三方应用注册的普通 BOOT_COMPLETED Receiver。

Android 11 清单还标记了 `android:systemUserOnly="true"`，并以较高优先级接收 `android.intent.action.BOOT_COMPLETED`。这说明它只在 system user 侧执行一次设备级整理，而不是为每个后来启动或解锁的用户重复收集同一批全局证据。

---

## 5. `onReceive()` 为什么立刻开线程

```java
public void onReceive(final Context context, Intent intent) {
    new Thread() {
        public void run() {
            logBootEvents(context);
            ...
        }
    }.start();
}
```

源码注释明确：避免 I/O 阻塞主线程。`logBootEvents()` 会读多个文件、解析 XML、写 DropBox、删除文件，绝不适合长时间占用 BroadcastReceiver 所在线程。

但直接 `new Thread()` 不是现代结构化并发；它没有 Future、统一超时或集中线程池治理。这里触发次数很少，历史实现选择了简单方案。

---

## 6. 异步线程改变完成语义

`onReceive()` 返回时：

```text
BootReceiver 已接到广播
后台线程可能刚创建
诊断文件可能尚未读取
DropBox Entry 可能尚未写入
stats Atom 可能尚未记录
tombstone observer 可能尚未启动
```

因此“开机广播已完成”不能作为“BootReceiver 诊断收集完成”的证明。

---

## 7. 总体流程图

```text
系统启动完成后触发 BootReceiver
              │
              ▼
        后台 Thread
              │
      getBootHeadersToLogAndUpdate
              │
      RecoverySystem.handleAftermath
              │
       read log-files.xml 去重表
              │
   ┌──────── first boot / runtime restart ────────┐
   │ SYSTEM_BOOT + last_kmsg/pstore/recovery/audit│
   │ 或 SYSTEM_RESTART                            │
   └──────────────────────────────────────────────┘
              │
      fs shutdown / mount / fsck metrics
              │
      shutdown-metrics → statsd
              │
      扫描 /data/tombstones
              │
      AtomicFile 写回去重表
              │
      FileObserver 持续监听新 tombstone
```

---

## 8. Boot headers 包含什么

`getCurrentBootHeaders()` 组合：

```text
Build.FINGERPRINT
Build.BOARD
ro.revision
Build.BOOTLOADER
radio version
/proc/version 中的 kernel version
```

它们帮助诊断材料携带构建和硬件上下文，否则单独一段 kernel log 很难判断来自哪个版本。

---

## 9. 为什么记录“上一次”的 header

```java
final String oldHeaders = getPreviousBootHeaders();
final String newHeaders = getCurrentBootHeaders();
FileUtils.stringToFile(lastHeaderFile, newHeaders);
return oldHeaders == null
        ? "isPrevious: false\n" + newHeaders
        : "isPrevious: true\n" + oldHeaders;
```

本次开机读取的 last_kmsg、recovery log、shutdown metrics 多半描述上一轮运行或关机。给它们拼本次升级后的 build fingerprint 可能误导分析。

所以先取旧 header 作为诊断前缀，再把当前 header 保存给下一次启动使用。

---

## 10. 第一次没有旧 header 怎么办

若 `last-header.txt` 不存在或读取失败：

```text
isPrevious: false
+ 当前 headers
```

这不是断言日志一定来自当前构建，而是明确告诉分析者：系统没有可用的上一轮 header，只能用当前信息兜底。

可观测性中，标记证据质量比静默假设更重要。

---

## 11. Recovery aftermath

```java
String recovery = RecoverySystem.handleAftermath(ctx);
if (recovery != null && db != null) {
    db.addText("SYSTEM_RECOVERY_LOG", headers + recovery);
}
```

`RecoverySystem.handleAftermath()` 负责读取并处理 recovery 执行后的结果和残留。BootReceiver 只接收返回文本并送进 DropBox。

要理解 recovery 文件的生成与清理，需要继续追 `RecoverySystem`，不能把全部逻辑归给 BootReceiver。

---

## 12. boot reason 被放到 last_kmsg footer

```text
Boot info:
Last boot reason: <ro.boot.bootreason>
```

`ro.boot.bootreason` 通常由 bootloader/kernel/init 链提供，用来描述 watchdog、kernel panic、reboot 等启动原因。

它是一个线索，不一定是最终根因。真实诊断仍要与 kernel log、pstore 和 shutdown metrics 交叉验证。

---

## 13. `ro.runtime.firstboot` 区分什么

若属性为 0：

```text
设置为当前 wall time
写 SYSTEM_BOOT
收集 last_kmsg/pstore/recovery/audit
```

否则：

```text
写 SYSTEM_RESTART
```

这里的 restart 通常表示 Android runtime/system_server 重启，而设备未经历完整 kernel boot。

---

## 14. CryptKeeper bounce 是特殊分支

旧式加密启动中，系统可能先在临时数据环境启动以取得 PIN/密码，再在真实 `/data` 可用后再次启动。

`StorageManager.inCryptKeeperBounce()` 为 true 时，不设置 `ro.runtime.firstboot`，让真实数据挂载后再次执行首次启动收集。

这是 Android 11 仍保留的历史加密兼容逻辑，不能直接套到所有现代 FBE 设备。

---

## 15. SYSTEM_BOOT 与系统 BOOT_COMPLETED 不是一回事

`SYSTEM_BOOT` 是写入 DropBox 的 tag；`BOOT_COMPLETED` 是广播 action；`sys.boot_completed` 是系统属性；`PHASE_BOOT_COMPLETED` 是 SystemService 阶段。

四者名字相近，但生产者、消费者和完成语义不同。

第 99 章的原则仍适用：看到“boot complete”必须先确认具体信号。

---

## 16. last_kmsg 是什么

`/proc/last_kmsg` 是某些内核/设备保留上一轮 kernel log 的传统接口。它可能包含 panic、watchdog、关机末尾和驱动错误。

不是所有设备都提供它，因此 BootReceiver 同时尝试 pstore 路径。

文件不存在时 `lastModified() <= 0`，收集函数直接返回，不把缺失当致命错误。

---

## 17. pstore/ramoops 是什么

```text
/sys/fs/pstore/console-ramoops
/sys/fs/pstore/console-ramoops-0
```

ramoops 可把内核日志写入保留内存，重启后由 pstore 文件系统暴露。它比 `/proc/last_kmsg` 更常见于现代设备，但具体文件名由 kernel 配置和平台决定。

BootReceiver 逐个尝试，而不是假定唯一固定来源。

---

## 18. 为什么读取文件尾部

传给 `FileUtils.readTextFile()` 的大小为负数：

```java
-LASTK_LOG_SIZE
```

负值表示读取文件尾部。kernel panic、shutdown 完成和最后错误通常靠近末尾；在固定上传预算下，尾部比开头更有价值。

截断时加入 `[[TRUNCATED]]`，让阅读者知道内容不完整。

---

## 19. userdebug 与 user 的大小不同

Android 11 基线：

```text
普通 LOG_SIZE：user 64 KiB，debuggable 96 KiB
LASTK_LOG_SIZE：user 64 KiB，debuggable 192 KiB
```

debuggable 构建允许更大的诊断样本。常量仍只是 BootReceiver 读取/提交的上限，不等于 DropBox 整体磁盘配额。

---

## 20. 为什么还考虑 GMSCore 二次截断

`addLastkToDropBox()` 计算 header、`[[TRUNCATED]]` 和 footer 占用，确保组合后的 last_kmsg 不超过后续消费者可能采用的 192 KiB 限制。

这说明数据链可能存在多个预算：

```text
原始文件大小
BootReceiver 截断预算
headers/footer 开销
DropBox 文件配额
下游消费者二次上传预算
```

只看一个常量无法判断最终保留多少正文。

---

## 21. recovery 文件来源

首次 kernel boot 分支还尝试：

```text
/cache/recovery/log          → SYSTEM_RECOVERY_LOG
/cache/recovery/last_kmsg    → SYSTEM_RECOVERY_KMSG
```

读取尾部，附加 headers，并利用时间戳表避免同一文件重复写入。

设备分区布局和 recovery 实现可能不同，文件不存在属于正常分支。

---

## 22. audit 不是上传完整 kernel log

`addAuditErrorsToDropBox()` 从 last_kmsg 或 pstore 中逐行筛选包含 `audit` 的行，再写入 `SYSTEM_AUDIT`。

```text
完整尾部样本 → SYSTEM_LAST_KMSG
筛选 audit 行 → SYSTEM_AUDIT
```

同一源文件可以产生不同用途的 DropBox 条目。

---

## 23. 去重表是什么

```text
/data/system/log-files.xml
```

逻辑结构：

```xml
<log-files>
  <log filename="/sys/fs/pstore/console-ramoops"
       timestamp="..." />
</log-files>
```

它只记录“某个键上次处理时的 lastModified”，不保存诊断正文。正文由 DropBoxManagerService 保存在自己的目录。

---

## 24. 去重算法

```java
long fileTime = file.lastModified();
if (fileTime <= 0) return;
if (timestamps.containsKey(filename)
        && timestamps.get(filename) == fileTime) return;
timestamps.put(filename, fileTime);
```

去重键通常是文件路径，值是 modification time。只要同一路径 mtime 未变化，就认为已经处理过。

这是一种轻量启发式，不是内容 hash 或强一致事件 ID。

---

## 25. mtime 去重的边界

可能出现：

- 内容变化但 mtime 被保留：误判重复。
- mtime 改变但内容相同：重复上传。
- 文件系统时间精度有限：快速覆盖难以区分。
- 时钟校准影响 wall time 语义。
- 路径复用：依赖 mtime 区分不同代文件。

对启动诊断这种低频文件，简单性可能比计算完整 hash 更重要，但不能把它描述成绝对去重。

---

## 26. audit 为什么用 tag 当去重键

audit 内容可能来自三个候选路径之一。代码使用 `timestamps[tag] = fileTime`，而非当前实际文件路径。

这样 `SYSTEM_AUDIT` 作为一个逻辑来源去重，不因 last_kmsg/pstore 路径切换而分别上传。

这也再次说明 map key 不总是文件名，虽 XML 属性仍叫 `filename`。

---

## 27. AtomicFile 如何保护去重 XML

```java
stream = sFile.startWrite();
... serialize XML ...
sFile.finishWrite(stream);
```

异常时：

```java
sFile.failWrite(stream);
```

`AtomicFile` 使更新尽量落为完整旧版或完整新版，避免普通覆盖写在进程崩溃时留下半截 XML。

它不让“DropBox 写入 + 去重表写入”成为一个跨文件原子事务。

---

## 28. 跨文件非原子窗口

典型顺序：

```text
timestamps.put
读取源文件
db.addText
稍后 writeTimestamps
```

若 DropBox 已写成功、去重 XML 写回前进程死亡，下次启动可能再次上传同一文件。

这偏向“至少一次”而非“恰好一次”。诊断证据允许偶尔重复，通常比永久漏报更可接受。

---

## 29. XML 解析失败策略

`readTimestamps()` 捕获文件不存在、I/O、XML、空值和非法状态。只要解析未成功：

```java
timestamps.clear();
```

结果是重新处理现存文件，可能重复写入，但不会因坏去重表永久跳过证据。

这是可用性优先的恢复策略。

---

## 30. 为什么 `synchronized (sFile)`

启动扫描线程和 tombstone FileObserver 都会读写同一个 `AtomicFile`。锁保证：

```text
一次 read 不与 write 交叉
两个 write 不同时更新 base/backup 文件
```

但 read-modify-write 整体并没有始终持同一把锁：各自读出 map、处理，再写回。因此并发事件仍可能发生最后写入覆盖较新 map 的理论窗口。

---

## 31. fs shutdown 指标从哪里找

候选：

```text
/sys/fs/pstore/console-ramoops
/proc/last_kmsg
```

只取第一个存在的文件，并读取最后 16 KiB，查找：

```text
powerctl_shutdown_time_ms:<duration>:<umount_status>
```

这是上一轮关机由 init/reboot 路径写入 kernel log 的摘要。

---

## 32. shutdown duration 与 umount status

匹配成功后写两个 stats 事件：

```text
SHUTDOWN_DURATION
SHUTDOWN_UMOUNT_STAT
```

一个是时长，一个是枚举/错误码。不能把第二个数字也当毫秒。

匹配不到时记录 `UMOUNT_STATUS_NOT_AVAILABLE=4`，而不是静默缺样本。

---

## 33. 匹配不到不等于关机没执行

源码注释指出：卸载之后若 kernel log 太多，目标字符串可能被挤出读取的最后 16 KiB。

所以“不存在指标”可能是：

```text
源文件不存在
格式不同
日志尾部过长
关机异常中断
生产者未写入
```

观测缺失不能直接推断业务事件未发生。

---

## 34. mount time 来自只读属性

依次读取：

```text
ro.boottime.init.mount_all.early
ro.boottime.init.mount_all.default
ro.boottime.init.mount_all.late
```

非零时映射到不同 `BOOT_TIME_EVENT_DURATION_REPORTED` 枚举并写 statsd。

属性由启动更早阶段的 init/fs_mgr 链产生，BootReceiver 是消费者和转录者。

---

## 35. `shutdown-metrics.txt`

路径：

```text
/data/system/shutdown-metrics.txt
```

内容是逗号分隔的 `key:value`，可能包括：

```text
reboot:y/n
reason:...
begin_shutdown:...
shutdown_system_server:<duration>
其他 shutdown_* 指标
```

BootReceiver 解析后写旧 MetricsLogger histogram 和 statsd shutdown Atom。

---

## 36. 解析不是通用 CSV

代码使用：

```java
metricsStr.split(",")
keyValueStr.split(":")
```

因此 value 若包含逗号或额外冒号会被判格式错误。生产者和消费者依赖一个简单私有协议，而非带转义的通用 CSV。

维护这类协议时，双方必须同步演进。

---

## 37. 哪些 key 进入 MetricsLogger

只有前缀为：

```text
shutdown_
```

的 key 会调用 `logTronShutdownMetric()`，且 value 必须能解析为非负 int。

`reboot`、`reason`、`begin_shutdown` 则用于组合结构化 `SHUTDOWN_SEQUENCE_REPORTED` Atom。

---

## 38. Statsd shutdown Atom 的默认值

缺失或解析失败时使用：

```text
reboot = false
reason = "<EMPTY>"
start = 0
duration = 0
```

同时打印错误日志，再照样写 Atom。这让数据管线保留一次记录，但分析时必须把默认占位与真实 0 区分开。

---

## 39. 为什么解析后删除 metrics 文件

```java
metricsFile.delete();
```

该文件描述上一轮关机。消费后删除，防止 system_server runtime restart 或下次启动重复记录同一轮指标。

即使内容为空或部分解析失败，代码最终也删除；这偏向避免重复，而可能牺牲失败后的再次解析机会。

---

## 40. fsck 日志路径

```text
/dev/fscklogs/log
```

BootReceiver 做两类工作：

1. 若包含 `FILE SYSTEM WAS MODIFIED`，决定是否把日志放入 DropBox。
2. 解析 `fs_stat,...,0x...`，修正误报位后写 statsd。

上传原始文本和记录结构化状态是两条独立链。

---

## 41. `fs_stat` 是位图

示例：

```text
fs_stat,/dev/block/.../userdata,0x5
```

最后字段是十六进制 flags。`0x400` 表示 `FS_STAT_FS_FIXED`，与 fs_mgr 中的定义保持一致。

位图可同时包含多个原因，不能把整个数字当单一枚举。

---

## 42. 为什么要修正 `FS_FIXED`

某些 e2fsck 输出会因为 extent tree 优化、配额信息更新或特定时间戳调整而设置“文件系统已修改”。这些变化不一定代表真实损坏修复。

若直接统计，会把正常优化误算成存储故障，污染设备健康指标。

`fixFsckFsStat()` 读取相邻日志上下文，判断是否应清除 `0x400`。

---

## 43. fsck pass 有语义

代码识别：

```text
Pass 1 / 1E：inode/extent 检查
Pass 5：group summary/quota 相关检查
```

只有在预期 pass 中出现特定文本才按已知无害模式处理。相同字符串出现在错误阶段不能轻易忽略。

日志解析依赖工具输出格式，是脆弱但实用的兼容层。

---

## 44. extent tree + quota warning

已知模式大致为：

```text
Pass 1 出现 extent tree could be shorter
Pass 5 出现 [QUOTA WARNING]
随后 Update quota info
没有其他修复文本
```

这被视为优化而非真实修复，可以清除 `FS_FIXED` 位。

若只有 quota fix 而没有 tree optimization，则仍视为真实修改。

---

## 45. 特定时间戳调整

源码识别：

```text
Timestamp(s) on inode ... beyond 2310-04-04 are likely pre-1970
Fix? yes
```

在预期条件下将其视为可忽略调整。代码还跳过下一行 `Fix? yes`，避免把它误判为其他修复。

任何未识别的非空行都会趋向保留 `FS_FIXED`，属于保守策略。

---

## 46. 为什么只上报 data/userdata 的 fs_stat

`handleFsckFsStat()` 会为所有解析到的分区打印 Slog，但 stats Atom 只在 partition 为：

```text
userdata
data
```

时写入 data partition 对应枚举。

这不是说其他分区不重要，而是该 Atom 字段定义聚焦用户数据分区。

---

## 47. fsck 原始日志何时上传

条件：

```text
DropBox tag SYSTEM_FSCK 已启用
日志包含 FILE SYSTEM WAS MODIFIED
```

结构化 fs_stat 解析即使 tag 关闭仍会进行；`uploadEnabled` 与 `uploadNeeded` 分开维护。

这很好地展示“遥测指标”和“原始诊断附件”可有不同开关。

---

## 48. 为什么最后删除 fsck log

```java
file.delete();
```

源码注释：避免 runtime restart 时重新上传。fsck 日志是本次启动消费的一次性输入。

若 DropBox 写入失败，函数仍可能走到删除，因此这里不是可靠消息队列的确认协议；设计目标是诊断采样，不是不可丢业务数据。

---

## 49. tombstone 启动扫描

```java
File[] tombstoneFiles = TOMBSTONE_DIR.listFiles();
for (...) {
    if (file.isFile()) {
        addFileToDropBox(..., "SYSTEM_TOMBSTONE");
    }
}
```

先扫描已存在文件，覆盖 BootReceiver 开始监听之前产生的 native crash。

去重表防止旧 tombstone 在每次 runtime restart 都重复写入。

---

## 50. FileObserver 监听未来文件

随后对 `/data/tombstones` 注册 `FileObserver.CREATE`：

```text
新文件创建
 → 读取最新去重表
 → 名字以 tombstone_ 开头且为文件
 → 截断并写 SYSTEM_TOMBSTONE
 → 写回去重表
```

启动扫描解决“监听前”，FileObserver 解决“监听后”。二者共同缩小事件窗口。

---

## 51. 为什么 `sTombstoneObserver` 必须是静态强引用

源码注释说明：保留引用，避免 finalizer 禁用 observer。

若只创建局部对象，方法返回后可能被 GC，native watch 随之停止。回调注册成功不代表生命周期自动永久保持。

监听器设计要明确谁持有它。

---

## 52. 为什么这里的 CREATE 已经对应完整 tombstone

一般而言，`FileObserver.CREATE` 只说明目录项出现，不能单独证明生产者写完；但 r48 这条具体链路
还有生产者协议保证。`system/core/debuggerd/tombstoned/tombstoned.cpp` 先把输出 FD 指向匿名
`O_TMPFILE`；若文件系统不支持，则使用 `.temporary*` 名字。`crash_dump` 写完并发送
`kCompletedDump` 后，tombstoned 才 unlink 旧槽位，再用 `linkat()` 把已完成 inode 发布成
`tombstone_XX`，最后清理 `.temporary*`。

BootReceiver 又只处理 `file.getName().startsWith("tombstone_")`，所以匿名文件没有目录事件，
fallback 的 `.temporary*` CREATE 也会被过滤；最终 `tombstone_XX` 的 CREATE 才触发读取。对 r48
而言，这个最终命名事件已经位于完成通知之后。这个结论来自两端协议，不能泛化成“所有 CREATE
都代表写完”。

---

## 53. system_server native crash 特殊 tag

若 tombstone 内容包含：

```text
>>> system_server <<<
```

除普通 `SYSTEM_TOMBSTONE` 外，还额外写：

```text
system_server_native_crash
```

这样健康监控组件可以更容易单独识别 system_server 的 native 崩溃。

---

## 54. 同时写 stats Atom 与 EventLog

处理 tombstone 时：

```text
FrameworkStatsLog.TOMB_STONE_OCCURRED
```

复制任意文件到 DropBox 时：

```text
EventLog DROPBOX_FILE_COPY(filename, maxSize, tag)
```

DropBox 保存正文，statsd 记录结构化发生事件，EventLog 记录本地搬运行为。三者用途不同。

---

## 55. 文件内容为什么转成 text

`addFileWithFootersToDropBox()` 使用 `FileUtils.readTextFile()`，拼 headers/footer，再调用 `db.addText()`。

因此 BootReceiver 并未把原文件以 FD 原样交给 DropBox，而是创建一个有大小上限的文本快照。

好处是容易附加上下文；代价是二进制数据不适合、内存会暂存字符串。

---

## 56. “addFileToDropBox” 名字也会误导

方法名看似把 File 直接传入，实际流程是：

```text
FileUtils.readTextFile
 → String 拼接
 → DropBoxManager.addText
```

源码阅读不能只凭 helper 名字判断数据传输方式。

---

## 57. DropBox tag disabled 时的行为

入口先检查：

```java
if (db == null || !db.isTagEnabled(tag)) return;
```

关闭某 tag 后，源文件通常不会读取和加入 timestamp map。但 fsck 是例外：仍解析 stats，只禁用原始日志上传。

这是因为结构化 boot health 指标与 DropBox 内容开关不是同一条产品策略。

---

## 58. DropBox 不可用时

`getSystemService(Context.DROPBOX_SERVICE)` 可能得到 null，代码多数 helper 会直接返回，但后续 mount/shutdown stats 仍可独立记录。

这体现降级设计：诊断仓库不可用不应阻止整个 BootReceiver 的其他遥测工作。

---

## 59. 异常隔离边界

后台线程外层：

```java
try { logBootEvents(context); }
catch (Exception e) { Slog.e(...); }
```

如果 `logBootEvents()` 中间某处抛出未处理异常，后续步骤可能全部跳过。例如在 fsck 前异常，tombstone observer 就不会注册。

整个流程不是每个来源完全隔离的独立任务。这是阅读时值得记录的可靠性风险。

---

## 60. 旧 updater 清理

诊断之后，线程查询 PackageManager 的 `isOnlyCoreApps()`。非 only-core 模式下，删除远古：

```text
com.google.android.systemupdater
```

对应的下载记录。

这段兼容 Froyo 以前 updater 的历史代码解释了类注释中的 `miscellaneous`：BootReceiver 还承担遗留升级清理。

---

## 61. 为什么 only-core 模式不清理

only-core 通常表示系统处于受限启动状态，只加载核心应用。此时不执行依赖完整包环境的旧下载清理。

PackageManager Binder 调用失败时捕获 `RemoteException`，默认 `onlyCore=false`，随后可能继续清理；外层还有异常捕获避免线程崩溃扩散。

---

## 62. 线程与数据关系图

```text
系统广播线程
  └─ new BootReceiver worker
       ├─ RecoverySystem / 文件 I/O
       ├─ DropBox Binder 调用
       ├─ statsd 写入
       ├─ AtomicFile read/write
       └─ 注册 FileObserver

FileObserver singleton thread
  └─ onEvent
       ├─ AtomicFile read
       ├─ 文件 I/O
       ├─ DropBox Binder 调用
       └─ AtomicFile write
```

FileObserver 回调也做了较多同步 I/O；事件频率低是该设计能够工作的隐含前提。

---

## 63. 一条 kernel panic 证据链

```text
kernel panic
 → ramoops 把 console 写入保留内存
 → 设备重启
 → pstore 暴露 console-ramoops
 → BootReceiver 读取尾部
 → 拼上一轮 build headers + bootreason
 → DropBoxManager.addText(SYSTEM_LAST_KMSG)
 → DropBoxManagerService 写入 /data/system/dropbox
 → 特权诊断消费者查询
```

任一环节缺失都可能导致证据不完整。

---

## 64. 一条正常关机指标链

```text
system_server 发起 shutdown
 → 写 /data/system/shutdown-metrics.txt
init 执行服务停止和文件系统卸载
 → kernel log 写 powerctl_shutdown_time_ms
设备启动
 → BootReceiver 读 metrics 文件与 pstore/last_kmsg
 → SHUTDOWN_SEQUENCE_REPORTED
 → BOOT_TIME_EVENT_DURATION / ERROR_CODE
 → 删除一次性 metrics 文件
```

Framework 和 init 各自产生不同部分，BootReceiver 在下一次启动汇合。

---

## 65. 一条 fsck 指标链

```text
启动挂载前 fs_mgr/e2fsck 检查文件系统
 → /dev/fscklogs/log
 → fs_stat 位图 + 人类可读修复文本
 → BootReceiver 解析上下文并修正误报 FS_FIXED
 → data partition Atom
 → 若真正 modified 且 tag 开启，原始日志进 DropBox
 → 删除源日志
```

原始证据与聚合指标同时存在，但保留策略不同。

---

## 66. 第 101 章与本章如何衔接

上一章解释 DropBoxManagerService 如何保存 Entry。本章解释一个重要生产者如何生成 Entry。

```text
BootReceiver：选择、截断、加 header、去重、提交
DropBoxManagerService：压缩、落盘、建索引、配额、通知、读取授权
```

不要把源文件去重表与 DropBox 内部 Entry 索引混为一套数据结构。

---

## 67. 两套去重/保留机制

| 机制 | 所属 | 解决问题 |
|---|---|---|
| `log-files.xml` 路径→mtime | BootReceiver | 同一源文件别重复搬运 |
| DropBox Entry 索引与 quota | DropBoxManagerService | 已搬运条目如何枚举和裁剪 |

即使 BootReceiver 不重复提交，DropBox 仍可能因配额删除正文；即使 DropBox 尚有空间，坏去重表也可能造成重复 Entry。

---

## 68. 设计取舍：为什么不用数据库

BootReceiver 的输入低频、来源少、状态只需路径和 mtime。XML + AtomicFile：

- 易读。
- 易恢复。
- 不依赖数据库初始化。
- 坏文件可清空重建。

代价是查询、并发更新和 schema 演进能力弱。选型应匹配数据规模与一致性要求。

---

## 69. 设计取舍：为什么偏向重复而非漏报

去重表损坏就清空；DropBox 成功后若 XML 写失败，下次可能重复。

对于崩溃诊断：

```text
重复一份日志 → 浪费少量空间，可由时间/tag识别
永久漏掉 panic → 可能失去唯一根因证据
```

因此至少一次倾向是合理的，但仍受 DropBox quota 限制。

---

## 70. 设计风险：一条异常中断整个收集链

`logBootEvents()` 是长串顺序调用。虽然多个 helper 内部会自行返回，未捕获的 IOException 仍可跳过后半段。

现代化方向可以把每个来源包装为独立 collector：

```text
CollectorResult(source, success, bytes, error)
```

逐个隔离异常，最后统一写健康指标。不过会增加代码和测试成本。

---

## 71. 生产者协议消除了半文件窗口，但仍有扫描—监听窗口

r48 已验证 tombstoned 在完成通知后才以 `linkat()` 发布最终 `tombstone_XX`，因此 BootReceiver
不会因 `.temporary*` 的早期 CREATE 读到半文件。真正仍需注意的是：BootReceiver 先扫描目录、
写去重表，之后才安装 FileObserver；恰好在“扫描结束—watch 生效”之间发布的 tombstone 可能本轮
收不到事件，只能等后续扫描机会。扫描加监听缩小了窗口，但没有形成无缝的事件交接协议。

---

## 72. 设计风险：高基数和隐私

DropBox 内容可能包括堆栈、进程名、路径、SELinux audit 和设备环境。访问受特权权限控制，上传端还应做额外隐私审查与采样。

statsd Atom 更适合稳定枚举和数值，不能把完整 reason/文本无约束地作为高基数维度。

---

## 73. macOS 只读练习一：入口与线程

```bash
cd /Users/ninebot/androidSource

sed -n '70,190p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java

rg -n "com.android.server.BootReceiver" \
  frameworks/base/core/res/AndroidManifest.xml
```

画出广播线程、BootReceiver worker 和 FileObserver thread，标注每个线程执行的 I/O/Binder 操作。

---

## 74. macOS 只读练习二：首次启动分支

```bash
cd /Users/ninebot/androidSource

sed -n '190,290p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java

rg -n "ro.runtime.firstboot|inCryptKeeperBounce" \
  frameworks/base system/core | head -100
```

回答 kernel boot、runtime restart、CryptKeeper bounce 三条路径分别写哪个 DropBox tag、收集哪些源文件。

---

## 75. macOS 只读练习三：去重事务窗口

```bash
cd /Users/ninebot/androidSource

sed -n '300,380p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java

sed -n '710,805p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java
```

逐行标出 `timestamps.put`、`db.addText`、`finishWrite`。假设每两个点之间进程死亡，判断下次会漏报、重复还是正常跳过。

---

## 76. macOS 只读练习四：fsck 状态机

```bash
cd /Users/ninebot/androidSource

sed -n '380,455p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java

sed -n '598,710p' \
  frameworks/base/core/java/com/android/server/BootReceiver.java
```

用四段虚构 e2fsck 文本测试：只有 tree optimization、tree+quota、只有 quota、额外 inode 修复。手算最终 `0x400` 是否保留。

---

## 77. macOS 只读练习五：关机生产者

```bash
cd /Users/ninebot/androidSource

rg -n "shutdown-metrics.txt|powerctl_shutdown_time_ms|begin_shutdown|shutdown_system_server" \
  frameworks/base system/core | head -160
```

分别找到 Framework 写 metrics 文件、init 写 kernel log marker 的位置，再回到 BootReceiver 对应消费者。

---

## 78. macOS 只读练习六：tombstone 生产链

```bash
cd /Users/ninebot/androidSource

rg -n "TOMBSTONE_DIR|/data/tombstones|tombstone_" \
  frameworks/base system/core | head -160
```

确认 Android 11 当前 checkout 中 tombstone 文件由谁创建、采用何种临时文件/rename 协议，再判断 FileObserver.CREATE 是否可能读到半文件。

---

## 79. 第一遍复盘问题

1. 为什么诊断 header 优先使用上一轮构建信息？
2. SYSTEM_BOOT 与 SYSTEM_RESTART 如何区分？
3. 为什么 last_kmsg 读尾部？
4. `log-files.xml` 保存正文吗？
5. AtomicFile 能否保证 DropBox 与去重表恰好一次？
6. fsck 为什么既写 Atom 又可能写 DropBox？
7. tombstone 为什么先扫描再监听？
8. FileObserver CREATE 是否代表文件写完？

---

## 80. 第二遍复读：易混点一——本次启动与上次事故

BootReceiver 在本次启动运行，但 last_kmsg、关机 metrics、recovery 结果大多描述上一轮。执行时间与证据归属时间不同。

`last-header.txt` 正是为了给上一轮证据配上一轮构建上下文。

---

## 81. 易混点二——pstore 与 DropBox

pstore 是 kernel 侧跨重启保留机制；DropBox 是 Android Framework 诊断仓库。

BootReceiver 把 pstore 的一个截断文本快照复制进 DropBox，二者不是同一存储层。

---

## 82. 易混点三——source file 与 DropBox Entry

删除 `/dev/fscklogs/log` 或 `shutdown-metrics.txt` 不等于删除已经提交的 DropBox Entry/Atom。

源文件是一次性输入，DropBox/Stats 是消费后的输出。

---

## 83. 易混点四——文件去重与事件去重

路径+mtime 只能判断“这个文件版本看起来处理过”，不能证明其中每个逻辑事件唯一，也不能对抗内容相同但 mtime 不同。

它是搬运去重，不是全系统 exactly-once 事件协议。

---

## 84. 易混点五——fsck modified 与损坏

文件系统发生修改不一定代表真实损坏：extent tree 优化、配额更新和特殊时间戳修正可能触发位图。

BootReceiver 解析上下文是为了减少健康指标误报，而不是否认 fsck 做过写操作。

---

## 85. 易混点六——缺指标与数值为零

解析失败时 shutdown Atom 可能填 0；目标 kernel marker 不存在时写 NOT_AVAILABLE 枚举。

分析系统必须保留 missing/invalid/zero 的区别，否则会把数据管线故障解释成“瞬间完成”。

---

## 86. 易混点七——广播完成与诊断收集完成

BootReceiver 自己在 `onReceive()` 内启动裸线程。广播调度框架不知道该线程何时结束，也没有通过 `goAsync()` 的 PendingResult 表达其生命周期。

因此不能用 Receiver 返回作为收集完成门槛。

---

## 87. “事故现场整理员”类比

```text
pstore/last_kmsg       = 黑匣子中的上一轮录音
recovery/fsck 文件     = 维修人员留下的工单
shutdown-metrics       = 关机流程计时表
tombstone              = native crash 尸检报告
Boot headers           = 事故设备型号和软件版本
log-files.xml          = 已经装袋过的证物清单
BootReceiver           = 现场整理员
DropBox                = 证物仓库
statsd Atom            = 结构化事故统计表
```

证物清单损坏会导致重新装袋，而不是把仓库里的证物自动删除。

---

## 88. 本章结论

BootReceiver 把多个阶段遗留的零散证据统一转录到可查询、可统计的系统：

```text
上一轮构建上下文 → last-header.txt
源文件搬运去重   → AtomicFile log-files.xml
原始诊断样本     → DropBox
启动/关机数值    → FrameworkStatsLog / MetricsLogger
持续 native crash→ FileObserver
```

它最重要的工程思想是：本次启动既是新运行的开始，也是上一轮运行的结算点。跨重启证据必须明确生产时间、消费时间、去重依据、截断预算和缺失语义。

下一章将精读 `tombstoned` 与 debuggerd：native crash 从信号、ptrace、进程内 dump 到 `/data/tombstones` 的真实生成链，再与本章 FileObserver 接上。
