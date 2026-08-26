# 106 Android ApplicationExitInfo 与 AppExitInfoTracker——进程死亡归因

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 核心文件：`frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java`  
> 本章目标：理解 AMS、zygote、LMKD 和真实 process death 的异步证据如何合并成 `ApplicationExitInfo`，以及历史记录的查询、持久化、状态摘要、ANR trace、权限和准确性边界。  
> 环境：macOS 只读源码，不要求编译或真机。

---

## 1. 先修正实现类名称

Android 11 当前源码中的实现类是：

```text
AppExitInfoTracker
```

路径：

```text
frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

不是某些后续版本资料中的 `ProcessExitInfoTracker`。学习平台源码时，版本和真实文件优先于网上类名。

---

## 2. 为什么进程死亡原因很难判断

进程退出时，系统可能从不同来源得到信息：

```text
AMS：我因 ANR/权限变化/用户请求等原因准备杀它
zygote SIGCHLD：子进程最终 exit status 或 signal
LMKD：这个 PID/UID 因低内存被杀
ProcessRecord death：Framework 已确认它从进程模型消失
native crash：AppErrors 已预先记 REASON_CRASH_NATIVE
应用自己 exit：zygote 看到正常 exit code
```

这些事件可能乱序到达。单看最后的 SIGKILL，无法知道是 LMKD、AMS 还是用户策略。

---

## 3. 完整合并图

```text
AMS kill intent ── scheduleNoteAppKill(reason/subreason/description) ─┐
                                                                     │
zygote SIGCHLD ── pid/uid/wait status ─→ external zygote cache ─────┤
                                                                     ├→ AppExitInfoTracker
LMKD report ───── pid/uid/LOW_MEMORY ─→ external lmkd cache ────────┤
                                                                     │
ProcessRecord died ─ raw pid/uid/packages/importance/PSS/RSS ───────┘
                          │
                          ▼
               ApplicationExitInfo history
                          ├─ AtomicFile proto
                          ├─ optional process state summary
                          ├─ optional gzipped ANR trace
                          └─ ActivityManager query API
```

Tracker 是证据合并器，不是 Linux 进程死亡的制造者。

---

## 4. 源码地图

```text
frameworks/base/core/java/android/app/ApplicationExitInfo.java
frameworks/base/core/java/android/app/ApplicationExitInfo.aidl
frameworks/base/core/java/android/app/ActivityManager.java
frameworks/base/core/java/android/app/IActivityManager.aidl
frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
frameworks/base/services/core/java/com/android/server/am/ProcessList.java
frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
frameworks/base/services/core/java/com/android/server/am/AppErrors.java
frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
frameworks/base/core/proto/android/app/appexitinfo.proto
frameworks/base/core/proto/android/app/appexit_enums.proto
frameworks/base/services/tests/mockingservicestests/src/com/android/server/am/ApplicationExitInfoTest.java
```

---

## 5. ApplicationExitInfo 记录哪些事实

核心字段：

```text
pid
realUid / packageUid / definingUid
processName
reason / subReason / status
importance
last sampled PSS / RSS
wall-clock timestamp
human-readable description
connectionGroup
packageName / packageList
processStateSummary byte[]
optional trace retriever
```

它是多来源拼成的历史快照，不是完整 core dump 或 tombstone。

---

## 6. 三种 UID

### realUid

进程实际在 kernel 中运行的 UID。isolated process 时通常是临时 isolated UID。

### packageUid

Package 安装时分配的稳定应用 UID，用于把 isolated 进程历史归回所属 App。

### definingUid

external service/useAppZygote 场景中，服务提供者的定义 UID，可能与调用 App/package UID 不同。

只写“进程 UID”会丢失这些归属差异。

---

## 7. Reason 与 status 不是同一个字段

```text
reason：Framework 语义，如 CRASH、ANR、LOW_MEMORY
status：exit() 参数，或终止 signal 编号
```

例如 Java crash 可能：

```text
reason = REASON_CRASH
status = SIGKILL 或 0（取决于可获得证据与合并顺序）
```

Native crash 通常 `reason=CRASH_NATIVE`，zygote status 可补原 fatal signal。

---

## 8. Android 11 的主要 Reason

```text
UNKNOWN
EXIT_SELF
SIGNALED
LOW_MEMORY
CRASH
CRASH_NATIVE
ANR
INITIALIZATION_FAILURE
PERMISSION_CHANGE
EXCESSIVE_RESOURCE_USAGE
USER_REQUESTED
USER_STOPPED
DEPENDENCY_DIED
OTHER
```

Reason 是稳定大类；内部更细信息放 subreason 或 description。

---

## 9. SubReason 为什么存在

`REASON_OTHER` 太宽泛。Android 11 内部 subreason 可表达：

```text
TOO_MANY_CACHED / TOO_MANY_EMPTY / TRIM_EMPTY
LARGE_CACHED / MEMORY_PRESSURE
EXCESSIVE_CPU
SYSTEM_UPDATE_DONE
KILL_UID / KILL_PID
INVALID_START / INVALID_STATE
IMPERCEPTIBLE / REMOVE_LRU
ISOLATED_NOT_NEEDED
```

公开 App 通常以 Reason 为主；subreason 是 `@hide` 的平台诊断细节。

---

## 10. description 不是稳定协议

API 文档明确说明 description 只供人读，不保证跨设备和 Android 版本格式稳定。

不要：

```text
if (description.contains("cached")) { ... }
```

自动化应依据 reason/status 和正式字段，description 只辅助人工诊断。

---

## 11. Tracker 初始化

`init(AMS)`：

```text
创建后台 ServiceThread：AppExitInfoTracker:killHandler
创建 /data/system/procexitstore
设置 procexitinfo 持久文件
从资源读取每 package 最大历史数量
```

KillHandler 允许 I/O，避免进程死亡回调在 AMS 关键线程上完成所有合并/持久任务。

---

## 12. onSystemReady 的工作

```text
注册 user removal receiver
注册 package removal receiver
IoThread：持久化 LMKD reportkills 属性
IoThread：加载已有 process exit proto
```

加载完成前 `mAppExitInfoLoaded=false`，来自 `ProcessRecord` 的 death record 和 AMS 主动 kill record 会被跳过，避免磁盘旧数据与运行期写入并发覆盖。

但 LMKD/zygote 的外部通知入口本身没有这个 loaded gate：它们仍可进入专用 Handler，并尝试更新现有历史或暂存在 external-source cache。若此时还没有可合并的 `ProcessRecord` 记录，加载窗口内的最终归因仍可能不完整。不要把 `mAppExitInfoLoaded` 理解成四类消息共同的总闸门。

这是一项启动窗口取舍。

---

## 13. 四类 KillHandler 消息

```text
MSG_LMKD_PROC_KILLED
MSG_CHILD_PROC_DIED（zygote SIGCHLD）
MSG_PROC_DIED（ProcessRecord death）
MSG_APP_KILL（AMS 主动 kill 意图）
```

所有来源在专用 Looper 上串行进入 Tracker，再在 `mLock` 下更新共享数据。

来源不同，消息 payload 和可信语义也不同。

---

## 14. AMS 主动 kill 的记录时机

例如 AppErrors 已决定因 Java/native crash 处置进程时：

```java
ProcessList.noteAppKill(app, reason, subReason, msg)
 → AppExitInfoTracker.scheduleNoteAppKill(...)
```

这是“系统决定/知道原因”的时刻，可能早于 kernel 真正报告进程死亡。

预先记录可保留高层语义，避免最后只剩 SIGKILL。

---

## 15. 真正死亡通知

ProcessRecord 被确认死亡时：

```text
scheduleNoteProcessDied(app)
 → obtainRawRecordLocked(app)
 → MSG_PROC_DIED
 → handleNoteProcessDiedLocked
```

raw record 捕获当时的 PID、UID、packages、importance、PSS/RSS 等 Framework 快照。

若此前已有主动 kill 记录，就更新/补全；没有则新建 UNKNOWN 记录再尝试外部归因。

---

## 16. zygote SIGCHLD 来源

zygote 作为 App 子进程的父进程接收 SIGCHLD，并把：

```text
pid
uid
wait status
```

交给 AMS/Tracker。

wait status 需用 `WIFEXITED/WEXITSTATUS`、`WIFSIGNALED/WTERMSIG` 解码，不能直接把原 int 当 exit code。

---

## 17. LMKD 来源

LMKD 杀进程时报告 pid+uid。Tracker 的 `mAppExitInfoSourceLmkd` 预设：

```text
REASON_LOW_MEMORY
```

LMKD 不一定提供完整 ProcessRecord/package/importance；Tracker 等待与 AMS death record 合并。

如果对应历史记录已经出现，外部通知也可反向更新它。

---

## 18. 事件顺序并不固定

可能顺序 A：

```text
AMS note kill → process died → zygote status
```

顺序 B：

```text
LMKD report → zygote status → AMS ProcessRecord cleanup
```

顺序 C：

```text
zygote status → 先缓存 → ProcessRecord died 后合并
```

ExternalSource 既能更新现有记录，也能缓存证据供稍后 claim。

---

## 19. 5 分钟 freshness

外部记录 freshness：

```java
APP_EXIT_INFO_FRESHNESS_MS = 300 * 1000;
```

原因是 PID 会复用。很久以前某 PID 的 LMKD/zygote 状态不能错误覆盖后来同 PID 的新进程。

5 分钟是防误配启发式，不是 Linux PID 唯一性的保证。

---

## 20. wall clock freshness 的边界

freshness 使用 `System.currentTimeMillis()`。用户/网络调时可能影响判断。

理想关联还可使用 process start time/generation，但 Android 11 此实现以 PID+UID+短时间窗口折中。

不要把 fresh 判定描述为严格事务 ID。

---

## 21. 合并优先级

`handleNoteProcessDiedLocked()`：

```text
先找已有 packageUid/package/pid record
移除并取得 zygote cache
移除并取得 lmkd cache
移除 isolated UID 映射
无记录则从 raw 添加
有 LMKD → reason 强制 LOW_MEMORY
否则有 zygote → 补 exit status/reason
```

LMKD 的明确低内存归因优先于只显示 signal 的 zygote status。

---

## 22. 主动 kill 信息为何覆盖

`handleNoteAppKillLocked()` 若已有记录：

```text
reason/subreason = AMS 提供值
status = 0
timestamp = now
description = AMS message
```

源码称该信息“more informational”。AMS 知道为什么选择杀进程，比最终 SIGKILL 更接近策略原因。

之后 native crash 的 zygote signal仍可补 status，而不会把 CRASH_NATIVE 改成普通 SIGNALED。

---

## 23. zygote status 更新规则

若正常 exit：

```text
reason = EXIT_SELF
status = exit code
```

若 signal：

```text
reason UNKNOWN → SIGNALED，status=signal
reason CRASH_NATIVE → 保持 CRASH_NATIVE，只补 status=signal
其他已有明确 reason → 通常不被泛化 signal 覆盖
```

大类原因优先，status 补低层终止形式。

---

## 24. 为什么 Java crash 不被 SIGKILL 覆盖

AppErrors 先记 `REASON_CRASH`，RuntimeInit finally 再 `killProcess()`。zygote 后来看到 SIGKILL，但已有原因更具体，Tracker 不应把它降级成 `REASON_SIGNALED`。

这正是多来源合并的价值。

否则所有 Java crash 看起来都只是 signal 9。

---

## 25. isolated UID 映射

Tracker 维护双向表：

```text
package app UID → isolated UIDs
isolated UID → package app UID
```

LMKD/zygote 报告 real isolated UID；历史查询应归入拥有它的 package UID。

进程死亡后移除映射，避免临时 UID 复用污染后续进程。

---

## 26. 一个进程可属于多个 package

raw record 有 `packageList`。`addExitInfoLocked()` 把同一 `ApplicationExitInfo` 加入每个 package 的容器。

因此共享进程死亡可从多个相关 package 查询到。

内部可能共享同一 info 对象引用，修改与清理要避免产生不一致。

---

## 27. mData 的层级

源码注释：

```text
packageName / packageUid
  → AppExitInfoContainer
      → pid / ApplicationExitInfo
```

UID 包含 userId，因此同 package 的不同 Android 用户自然分开。

容器按配置限制每 package 历史条数。

---

## 28. PID 作为容器 key 的后果

同一 package 新进程复用旧 PID 时，`SparseArray.put/append` 可能替换该 PID 旧记录。

容器的目标是有限近期历史，不是永不冲突的全局审计日志。

时间、容量和 PID 复用共同限定保留精度。

---

## 29. 容量满时如何裁剪

当容器达到 `mMaxCapacity`：

```text
遍历找 timestamp 最旧记录
删除其 trace 文件
移除记录
加入新记录
```

不是按 PID 大小或插入 index 简单删除。

元数据与关联 trace 必须一起清理。

---

## 30. PSS/RSS 不是死亡瞬间值

API 文档明确：

```text
last sampled PSS/RSS
不是死亡前精确内存
若来不及采样可能为 0
```

低内存杀记录的 RSS=0 不代表进程没有占内存；只能说明 Tracker 没有样本。

---

## 31. importance 也是最后已知状态

importance 由 ProcessRecord procState 转换，反映死亡前 Framework 最后掌握的进程重要性。

并发状态转换可能使它稍滞后；它适合诊断“前台/服务/缓存大类”，不是纳秒级 kernel 状态。

---

## 32. process state summary

App 可调用：

```java
ActivityManager.setProcessStateSummary(byte[] state)
```

Tracker 按 calling UID/PID 暂存。进程死亡记录创建时 claim 到 `ApplicationExitInfo`。

它让 App 写入少量业务上下文，例如当前页面/任务阶段，但不是任意大 crash dump。

---

## 33. state summary 大小限制

AMS 检查：

```text
state.length <= MAX_STATE_DATA_SIZE
```

Android 11 的 `ActivityManagerService.MAX_STATE_DATA_SIZE` 是 `128`，单位是字节。这个容量只适合保存短小、可版本化的诊断标签，例如“页面编号 + 任务阶段 + 少量标志位”，不适合保存堆栈、日志或完整 JSON。

超限抛 `IllegalArgumentException`。调用身份直接来自 Binder callingUid/Pid，App 不能为另一个进程设置摘要。

byte[] 内容由 App定义，读取方必须自带版本和校验。

---

## 34. summary 的生命周期

```text
active UID/PID summary
 → process dies
 → findAndRemove
 → 归入 exit record
```

若 App 多次设置，以当前 map 中最新值为准。若进程从未死亡或记录未建立，active 数据会在 package/user cleanup 中移除。

它不是实时遥测流。

---

## 35. ANR trace 的 claim 模型

ANR 发生但进程尚未死时，Tracker 记录 active trace 文件路径。后来 exit record 创建：

```text
findAndRemove active trace
 → info.setTraceFile
 → info.setAppTraceRetriever
```

因此进程可能先 ANR 后恢复，再因其他 reason 死亡；该退出记录仍可能附带较早 ANR trace。

trace 存在不等于最终 reason 一定为 ANR。

---

## 36. trace 为什么 gzip

存储文件后缀 `.gz`，`getTraceInputStream()` 使用 `GZIPInputStream` 包装从 system_server 取得的只读 FD。

压缩降低长期历史 traces 的磁盘占用；调用者得到解压后的 InputStream。

必须关闭 stream，底层 `AutoCloseInputStream` 才关闭 ParcelFileDescriptor。

---

## 37. trace File 不跨 Binder

`ApplicationExitInfo.mTraceFile` 是 system_server 内部字段，不随普通对象直接暴露文件路径。

跨进程对象携带 `IAppTraceRetriever` Binder；调用 `getTraceInputStream()` 时再向 system_server 请求 FD。

这允许读取时重新做权限检查，并避免暴露受保护路径。

---

## 38. AppTraceRetriever 权限链

```text
拒绝 isolated caller
校验 package 非空
handleIncomingUser 检查跨用户
enforceDumpPermissionForPackage 检查调用者对 package 的访问
按 package/uid/pid 找 record
clearCallingIdentity 后只读 open trace
返回 ParcelFileDescriptor
```

拿到历史元数据不自动拥有任意 trace 文件访问权。

---

## 39. 查询 API

App 侧：

```java
ActivityManager.getHistoricalProcessExitReasons(
        packageName, pid, maxNum)
```

Binder 侧还带 userId。返回 `ParceledListSlice<ApplicationExitInfo>`，应对列表 Parcel 传输。

结果通常按 timestamp 从新到旧。

---

## 40. packageName 为空

AMS 使用 callingUid 作为 filter UID，调用者查询自己的历史。

packageName 非空时，调用 `enforceDumpPermissionForPackage()`，只有有权查看目标 package 才返回。

接口不是任意 App 的全设备死亡历史浏览器。

---

## 41. userId 限制

AMS 明确不支持：

```text
USER_ALL
USER_CURRENT
```

要求具体 userId，再通过 `handleIncomingUser()` 做跨用户校验。

具体用户参数避免“current”在异步查询期间变化造成歧义。

---

## 42. pid 过滤

```text
pid = 0：不过滤 PID
pid > 0：只返回该 PID 的历史记录
maxNum > 0：限制数量
```

由于 PID 可复用，PID 过滤仍需结合 timestamp、UID、processName 判断是否是目标实例。

---

## 43. 查询时 clearCallingIdentity

Tracker 在完成 AMS 外层授权后清除 Binder identity，读取内部数据时使用 system_server 身份，finally 恢复。

授权必须在清除前完成；内部 map 查询本身不应被调用者身份意外影响。

---

## 44. 持久化目录

```text
/data/system/procexitstore/
  procexitinfo
  <关联 ANR trace>.gz
```

实际 system dir 来自 `SystemServiceManager.ensureSystemDir()`。

元数据使用 proto + AtomicFile，trace 是独立文件，因此不是跨文件原子事务。

---

## 45. 30 分钟批量落盘

```java
APP_EXIT_INFO_PERSIST_INTERVAL = 30 minutes
```

每次加入记录只安排 persist task；已有 task 时通常不重复排队。减少频繁死亡下的磁盘写放大。

代价是 system_server/设备突然掉电时，最近尚未持久化历史可能丢失。

---

## 46. 立即持久化

`schedulePersistProcessExitInfo(boolean immediately)` 可移除原延迟 task 并以 0 delay 重新 post。

通常在用户/package 清理等需要尽快反映持久状态的边界使用。

“post 0”仍是异步 IoThread 任务，不等于调用返回时磁盘已完成。

---

## 47. AtomicFile 保护什么

```text
procexitinfo 单文件写半失败
```

它不能保证：

```text
proto 与多个 trace 文件原子一致
内存刚加的记录必已落盘
LMKD/zygote/AMS 三个来源 exactly-once
```

加载后会 prune 无引用 trace，修复部分跨文件残留。

---

## 48. Proto 内容

层级近似：

```text
last update timestamp
packages[]
  package name
  users/UID containers[]
    ApplicationExitInfo[]
```

加载时重建 `ProcessMap<AppExitInfoContainer>`。读取异常会记录 warning，最后仍将 loaded 标记 true，让新运行期记录继续工作。

---

## 49. 为什么加载前跳过记录

若磁盘加载和新的 ProcessRecord/AMS kill record 同时修改 `mData`，旧文件内容可能覆盖或与新对象重复。

简单策略是在 loaded 前跳过 `scheduleNoteProcessDied()` 和两种 `scheduleNoteAppKill()`。LMKD/zygote 消息仍可到达 external-source 合并器，但缺少最终 raw record 时也无法保证留下完整历史。代价是早期启动期间的进程死亡可能没有可查询记录。

这是数据一致性优先于短窗口完整性的明确取舍。

---

## 50. package removal

监听 `ACTION_PACKAGE_REMOVED`：

```text
若 EXTRA_REPLACING=true（升级）→ 不删除
真正卸载 → 按 package/uid/allUsers 清历史和 trace
```

应用升级不应自动抹去此前退出历史；卸载则需删除隐私数据和 UID 归属。

---

## 51. user removal

用户删除时清除：

- 该 user 的 package exit containers。
- isolated UID 映射。
- zygote/LMKD external cache。
- active state summary 与 traces。
- 对应 trace 文件。

多用户数据生命周期不能只清主 map。

---

## 52. raw record pool

`SynchronizedPool<ApplicationExitInfo>` 最大 8 个，用于 AMS 高频 death/kill 消息的临时对象复用。

消息处理后 `recycleRawRecordLocked()` 清理并归还池。

池中 raw 不是历史对象；`addExitInfoLocked()` 会复制成新 `ApplicationExitInfo`，避免回收后篡改历史。

---

## 53. 为什么需要复制 raw

```java
final ApplicationExitInfo info = new ApplicationExitInfo(raw);
```

raw 对象很快回池复用。如果容器直接持 raw，下一次 death 会覆盖旧历史。

对象池与长期存储之间必须有 ownership 转换。

---

## 54. KillHandler 是 async Handler

构造：

```java
super(looper, null, true)
```

消息标记 asynchronous，可越过 Looper sync barrier。进程死亡记账不应被 UI 等同步屏障无谓阻塞。

但仍与同一 KillHandler 队列中的其他消息串行。

---

## 55. mLock 与 AMS 锁

Tracker 有独立 `mLock`，不把所有历史操作塞进 AMS 大锁。

调用 PID map 时还需 `mPidsSelfLocked`。锁顺序必须谨慎；查询外部 Package/User 权限通常在进入 Tracker 锁前完成。

专用锁降低耦合，但没有自动消除死锁可能。

---

## 56. 排序比较器的潜在细节

源码部分位置使用：

```java
(int) (b.getTimestamp() - a.getTimestamp())
```

long 差值强转 int 理论上可能溢出，导致跨很长时间记录排序不严格。历史容量和常见时间接近使风险降低，但这是代码审查值得发现的边界。

不要把所有 comparator 都默认视作数学安全。

---

## 57. LOW_MEMORY 与内存压力 subreason

```text
REASON_LOW_MEMORY：LMKD 明确报告 kill
REASON_OTHER + SUBREASON_MEMORY_PRESSURE/LARGE_CACHED：AMS 因自身策略杀
```

两者都与内存有关，但执行者与证据来源不同。

只按 description 搜“memory”会混淆系统策略与 LMKD kill。

---

## 58. EXIT_SELF 与 USER_REQUESTED

```text
EXIT_SELF：进程调用 exit 正常退出，zygote wait status 推断
USER_REQUESTED：用户/系统 API 明确要求停止该 App
```

用户点 Force Stop 最终进程可能也收到 signal，但高层原因应保留 USER_REQUESTED。

---

## 59. SIGNALED 与 CRASH_NATIVE

```text
SIGNALED：只有低层 signal 证据，没有更具体已知原因
CRASH_NATIVE：debuggerd/AppErrors 已确认 native crash
```

两者 status 都可能是 SIGSEGV。Reason 表达归因置信度和上层语义，而 status 表达 Linux 终止形式。

---

## 60. ANR reason 与 trace

ANR 不一定杀进程：用户可选择等待，App 可恢复。因此：

- 有 ANR trace，最终 reason 可能不是 ANR。
- reason=ANR 时通常意味着系统最终因 ANR 杀它。
- trace 也可能已因容量/清理而不可用。

元数据和附件不是一一必然关系。

---

## 61. 一个 Java crash 合并时序

```text
RuntimeInit → AMS AppErrors
 → noteAppKill(REASON_CRASH)
 → Tracker 建/更新 record
RuntimeInit finally SIGKILL
 → zygote SIGCHLD(status=SIGKILL)
ProcessRecord appDied
 → raw state/PSS/RSS/packages
Tracker 合并
 → reason 保持 CRASH
```

若只看最后 signal，会误判为普通外部 kill。

---

## 62. 一个 native crash 合并时序

```text
debuggerd 生成 tombstone
NativeCrashListener → AppErrors
 → noteAppKill(CRASH_NATIVE)
恢复原 SIGSEGV
 → zygote status=SIGSEGV
ProcessRecord died
 → Tracker 合并
最终 reason=CRASH_NATIVE, status=SIGSEGV
```

这比单独 tombstone 或单独 exit status 更便于 App 查询历史。

---

## 63. 一个 LMKD kill 时序

```text
LMKD kill(pid, uid)
 → MSG_LMKD_PROC_KILLED 缓存 LOW_MEMORY
zygote SIGCHLD(SIGKILL)
 → status cache/更新
AMS appDied raw
 → claim LMKD record
最终 reason=LOW_MEMORY
```

事件乱序时 ExternalSource 的 add-or-update 逻辑完成同样归因。

---

## 64. 一个 AMS cached trim 时序

```text
OomAdjuster/ProcessList 决定移除 cached process
 → noteAppKill(REASON_OTHER, SUBREASON_TOO_MANY_CACHED/...)
 → kill
 → zygote SIGKILL
 → process died
最终保留 AMS 具体 subreason
```

它不应被误标为 LMKD LOW_MEMORY，除非 LMKD 也提供更新且符合合并规则。

---

## 65. PID 先复用的防护

Tracker 同时比较：

```text
pid
real/package uid
package name
record timestamp freshness
isolated UID mapping
```

比单 PID 安全，但仍是启发式。系统级绝对进程 identity 更适合 pidfd/starttime/generation，Android 11 这里未完全采用。

---

## 66. API 使用示意

```java
ActivityManager am = context.getSystemService(ActivityManager.class);
List<ApplicationExitInfo> exits =
        am.getHistoricalProcessExitReasons(null, 0, 10);
for (ApplicationExitInfo info : exits) {
    Log.i(TAG, "reason=" + info.getReason()
            + " status=" + info.getStatus()
            + " time=" + info.getTimestamp());
}
```

调用方要处理空列表、字段为 0、trace 为 null 和版本差异。

---

## 67. App 如何写业务摘要

```java
byte[] state = encodeVersionedState("sync_upload", 3);
am.setProcessStateSummary(state);
```

建议格式包含：

```text
schema version
短状态码
非敏感计数/阶段
校验或长度
```

不要放 token、密码、用户正文或无界 JSON。

---

## 68. state summary 不是 crash callback

App 在正常运行中主动更新状态。系统不会在 crash 瞬间回调 App 再请求摘要，因为那时进程可能失效。

```text
提前 checkpoint 小状态
死亡时 claim 最新 snapshot
```

这与黑匣子持续记录关键状态的思想类似。

---

## 69. trace InputStream 使用

```java
try (InputStream in = info.getTraceInputStream()) {
    if (in != null) {
        // 有界读取并在后台解析
    }
}
```

不要在主线程无界读取；历史记录可能在查询后被容量裁剪，FD 请求返回 null 属于正常竞态。

---

## 70. 持久化并非实时审计日志

限制包括：

- 每 package 有容量上限。
- 30 分钟批量落盘。
- system_server crash 可丢最近内存记录。
- PID 复用可替换同 key。
- 卸载/用户删除主动清理。
- trace 单独裁剪。

它服务近期诊断，不适合合规级永久审计。

---

## 71. macOS 只读练习一：四来源消息

```bash
cd /Users/ninebot/androidSource

rg -n "MSG_LMKD|MSG_CHILD|MSG_PROC_DIED|MSG_APP_KILL|handleMessage" \
  frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

给每种消息写出生产者、payload、它能证明什么、它不能证明什么。

---

## 72. macOS 只读练习二：合并规则

```bash
cd /Users/ninebot/androidSource

sed -n '350,500p' \
  frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

分别模拟 CRASH+SIGKILL、CRASH_NATIVE+SIGSEGV、UNKNOWN+exit(7)、LMKD+SIGKILL，写出最终 reason/status。

---

## 73. macOS 只读练习三：记录结构

```bash
cd /Users/ninebot/androidSource

sed -n '330,700p' \
  frameworks/base/core/java/android/app/ApplicationExitInfo.java

rg -n "obtainRawRecordLocked|addExitInfoInnerLocked|AppExitInfoContainer" \
  frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

画出 realUid/packageUid/definingUid、packageList 和每 package container 的关系。

---

## 74. macOS 只读练习四：持久化窗口

```bash
cd /Users/ninebot/androidSource

sed -n '620,760p' \
  frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

假设 record 加入后 5 秒、20 分钟、31 分钟 system_server 分别崩溃，判断磁盘是否必有最新数据。

---

## 75. macOS 只读练习五：查询权限

```bash
cd /Users/ninebot/androidSource

sed -n '10410,10450p' \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java

rg -n "getTraceFileDescriptor|enforceDumpPermissionForPackage|handleIncomingUser" \
  frameworks/base/services/core/java/com/android/server/am/AppExitInfoTracker.java
```

对照“查询自己”“指定其他 package”“跨 user”“isolated caller”“读取 trace”五种场景。

---

## 76. macOS 只读练习六：调用点分类

```bash
cd /Users/ninebot/androidSource

rg -n "noteAppKill\(|\.kill\(" \
  frameworks/base/services/core/java/com/android/server/am | head -180
```

选择十个调用点，把它们映射到 Reason/SubReason，并判断 reason 是“决定杀的原因”还是“事后观察到的结果”。

---

## 77. 第一遍复盘问题

1. 为什么单看 SIGKILL 无法归因？
2. AppExitInfoTracker 合并哪四类消息？
3. reason 与 status 有何区别？
4. LMKD 与 AMS memory-pressure kill 如何区分？
5. freshness 为什么存在，又为何不严格？
6. realUid/packageUid/definingUid 各是什么？
7. PSS/RSS 为什么可能为 0？
8. 有 ANR trace 是否代表 reason=ANR？
9. AtomicFile 能否保证 trace 与 proto 原子？
10. packageName 为空时查询范围是什么？

---

## 78. 第二遍复读：易混点一——kill intent 与 process died

AMS `noteAppKill` 是“我准备/决定因某理由结束它”；`scheduleNoteProcessDied` 是“Framework 已确认它死了”。

前者保存语义，后者补进程快照。两者不是重复事件。

---

## 79. 易混点二——reason 与 signal

reason 是上层归因，signal 是 status 的一种。Java crash reason 可以是 CRASH，而 signal 最终是 SIGKILL；native crash reason 是 CRASH_NATIVE，signal 可能 SIGSEGV。

不能把 reason 当 Linux signal enum。

---

## 80. 易混点三——LOW_MEMORY 与所有内存相关 kill

LOW_MEMORY 主要由 LMKD 明确报告；AMS 自己裁剪 cached process 常为 OTHER + 细 subreason。

两个都可能在低内存背景发生，但归因生产者不同。

---

## 81. 易混点四——timestamp 与 event ordering

记录 timestamp 可能在主动 kill 更新时重写为当前 wall time；外部 cache 也有自己的时间。它不是所有底层事件的单一原始发生时间。

精细时序仍需 EventLog/Perfetto/LMKD log。

---

## 82. 易混点五——历史记录与死亡证明

ApplicationExitInfo 是 Framework best-effort 记录。加载窗口、异常、容量、掉电和 PID 复用均可能造成缺失或近似。

列表为空不能证明进程从未退出。

---

## 83. 易混点六——PSS/RSS 与峰值/死亡值

它们是最后一次采样，不是 peak，也不是 kernel 在死亡瞬间测量。0 常表示无样本，不表示真实 0 KiB。

要分析内存趋势需结合 meminfo、stats、LMKD 和采样历史。

---

## 84. 易混点七——trace 与最终原因

trace 是此前系统抓到的 ANR 附件，可被后来 exit record claim。最终 reason 可能 crash、user requested 或其他。

附件描述一段历史，不必等同终止触发点。

---

## 85. 易混点八——state summary 与系统状态

summary 是 App 自报的不透明 bytes；importance/PSS/reason 是系统生成字段。前者不能作为安全事实，必须版本化和不信任解析。

它用于业务线索，不用于权限判定。

---

## 86. “拼事故报告”类比

```text
AMS noteAppKill       = 调度中心写下为何下令停车
zygote wait status    = 车辆最终以何机械状态停下
LMKD report           = 内存管理中心承认由它执行
ProcessRecord died    = 车队管理系统确认车辆离线并附最后状态
freshness             = 防止旧车牌记录错贴到复用车牌
ApplicationExitInfo   = 合并后的事故卡片
state summary         = 车辆提前写的小型黑匣子状态
ANR trace             = 此前一次卡顿检查报告
AtomicFile proto      = 定期归档卡片
```

事故卡片是多方证词的最佳合并，不是摄像机逐帧真相。

---

## 87. 本章结论

Android 11 的进程退出历史依靠“先记录高层意图，再用低层死亡证据补全”的模型：

```text
AMS reason/subreason 保留策略语义
zygote status 保留 exit code/signal
LMKD 提供明确低内存归因
ProcessRecord 提供 package/UID/importance/PSS/RSS 快照
isolated UID map 把临时身份归回 App
5 分钟 freshness 降低 PID 复用误配
state summary 和 trace 补业务/ANR 上下文
per-package 容量、AtomicFile proto 和清理 receiver 管历史生命周期
查询与 trace FD 在 system_server 重新鉴权
```

最重要的结论是：`ApplicationExitInfo` 应被当作有明确证据边界的近期诊断记录。Reason 比单一 signal 更有业务意义，但仍是 best effort；PSS/RSS、timestamp、trace 和 description 都有各自的采样、时序或稳定性限制。

下一章将继续深入 LMKD 协议与 PSI/内存压力：Framework 的 OOM adj 如何送入 lmkd，lmkd 如何选择 victim、上报 kill，并最终形成本章的 `REASON_LOW_MEMORY`。
