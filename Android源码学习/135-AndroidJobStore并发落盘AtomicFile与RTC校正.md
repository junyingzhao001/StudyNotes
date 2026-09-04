# 135 JobStore：schedule 成功后立刻掉电，persisted Job 一定还在吗？

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置知识：第 134 章 elapsed 时间门、periodic 窗口与失败 backoff

## 先给结论：这章解决什么问题

应用调用：

```java
jobScheduler.schedule(
        new JobInfo.Builder(90, service)
                .setPersisted(true)
                .setMinimumLatency(10 * 60_000)
                .build());
```

返回 `RESULT_SUCCESS` 后，设备如果马上掉电，这个 Job 重启后一定还在吗？如果写 `jobs.xml` 写到一半崩溃，会不会留下半个 XML，导致全部任务都读不回来？重启后的 `elapsedRealtime` 从 0 开始，原来的“再等 10 分钟”又怎样恢复？

一句话结论是：**`schedule()` 成功首先表示 Job 已进入 system_server 的内存 `JobSet`；persisted 变更会延迟约 2 秒形成快照，再由 IoThread 用 `AtomicFile` 写盘。因此返回成功不等于已经 fsync。AtomicFile 主要保护旧文件不被半写覆盖，而跨重启时间要经过 elapsed → wall clock → 新 elapsed 的换算。**

读完本章，你应该能：

- 区分内存 JobSet 与磁盘 persisted 子集；
- 解释 2 秒合并窗口为什么不会静默漏掉并发更新；
- 画出 `mLock`、`mWriteScheduleLock` 与单线程 IoThread 的分工；
- 说清 AtomicFile 保证“文件完整性”，但不保证“本次一定保存成功”；
- 手算重启前后的时间边界；
- 识别 Android 11 r48 中未调用 `failWrite()`、失败不自动重试等实现缺口。

本章讨论的是 JobScheduler 自己的调度状态，不是业务数据持久化。即使 JobInfo 成功写入 `jobs.xml`，上传到第几个分片、订单是否结算等业务事实仍应放在应用数据库中。

---

## 一、先分清三份状态

可以把 JobStore 想成“白板 + 定时拍照 + 保险文件”：

```text
内存 JobSet
  所有当前 Job 的白板，调度决策直接读它
        │
        │ 只复制 persisted Job
        ▼
List<JobStatus> storeCopy
  某一时刻的快照，之后脱离全局锁生成 XML
        │
        │ AtomicFile: jobs.xml.new → sync → rename
        ▼
/data/system/job/jobs.xml
  重启恢复用的磁盘文件
```

三者的完成点不同：

| 事件 | 已能在当前开机调度 | 已进入本轮快照 | 已提交到磁盘基准文件 |
|---|---:|---:|---:|
| `schedule()` 返回成功 | 是 | 不一定 | 不一定 |
| 写 runnable 完成内存复制 | 是 | 是 | 否 |
| `AtomicFile.finishWrite()` 返回 | 是 | 是 | 通常已走完 sync/rename；仍要留意内部失败日志 |

所以“API 返回成功”和“抗立即掉电”不是同一承诺。JobStore 选择异步合并写，是为了减少每次 schedule 都阻塞 Binder 调用并频繁刷盘的成本。

实际系统基准路径由 JobStore 构造时的 data directory 形成：

```text
/data/system/job/jobs.xml
```

测试可以传入临时 dataDir，因此读源码时不要把测试路径当真机路径。

---

## 二、JobSet 保存全部，XML 只保存 persisted

`JobStore` 同时维护按 calling UID 和 source UID 的索引，供不同查询与归因场景使用。这里先抓住持久化主线：内存里不只 persisted Job。

新增代码：

```java
boolean replaced = mJobSet.remove(jobStatus);
mJobSet.add(jobStatus);
if (jobStatus.isPersisted()) {
    maybeWriteStatusToDiskAsync();
}
```

普通非 persisted Job 也在 JobSet 中参与本次开机调度，只是不触发磁盘写入。写快照时又过滤一次：

```java
mJobSet.forEachJob(null, job -> {
    if (job.isPersisted()) {
        storeCopy.add(new JobStatus(job));
    }
});
```

因此：

```text
JobSet = 当前开机所有被跟踪 Job
jobs.xml = JobSet 在某一时刻的 persisted 子集
```

这也解释第 133 章的边界：`JobWorkItem` enqueue 被 r48 禁止搭配 persisted，它的 pending/executing 工作队列不会进入 jobs.xml。

---

## 三、2 秒不是“完成时限”，而是从首次变化开始的合并窗口

源码常量：

```java
private static final long JOB_PERSIST_DELAY = 2000L;
```

调度逻辑：

```java
synchronized (mWriteScheduleLock) {
    if (!mWriteScheduled) {
        mIoHandler.postDelayed(mWriteRunnable, JOB_PERSIST_DELAY);
        mWriteScheduled = mWriteInProgress = true;
    }
}
```

假设在 0ms、300ms、900ms 连续改变 3 个 persisted Job：第一笔在 IoThread 排一个约 2000ms 后的 runnable，后两笔看到 `mWriteScheduled=true`，不会各自再排一次。

严格说，它更像“从第一笔变化开始的固定延迟合并”，不是每来一笔就把计时器重新推后 2 秒的 trailing debounce。

而且 2 秒后只是 runnable 有资格进入 IoThread 消息队列：

```text
2 秒延迟到期
  ≠ runnable 已开始
  ≠ 快照已复制
  ≠ XML 已序列化
  ≠ fsync + rename 已完成
```

IoThread 若正忙，或磁盘 I/O 较慢，真正提交会更晚。

---

## 四、两把锁与一个串行线程怎样避免丢更新

### 1. 各自保护什么

| 机制 | 保护对象 |
|---|---|
| `mLock` | JobSet 和 JobScheduler 的调度状态一致性 |
| `mWriteScheduleLock` | 是否已排写、测试等待等写调度标志 |
| IoThread Handler | 写 runnable 按消息队列串行执行 |
| AtomicFile | 单次写的旧文件/新文件提交完整性 |

AtomicFile 自己明确不提供线程或进程互斥；不会替 JobStore 加锁。这里不会同时写同一个文件，关键是两个写 runnable 都在同一 IoThread Handler 上串行执行。

### 2. runnable 为什么先清 mWriteScheduled，再取快照

```java
synchronized (mWriteScheduleLock) {
    mWriteScheduled = false;
}
synchronized (mLock) {
    // 复制 persisted Job
}
```

一开始就允许排下一轮，正是为了闭合下面的竞态：

```text
第一轮清 mWriteScheduled=false
        │
        ├─ 若新 persisted 变更先拿到 mLock
        │     → 它会排第二轮
        │     → 第一轮快照可能含它，也可能不含；第二轮最终会含
        │
        └─ 若第一轮先拿到 mLock 完成快照
              → 后续变更一定发生在快照之后
              → 看到 scheduled=false，排第二轮
```

反过来，若等快照复制完才清 `mWriteScheduled`，就可能出现：新变更发生在快照之后，却仍看到 scheduled=true 而不排新写；随后旧 runnable 清零，更新既没进入旧快照，也没新 runnable 接手。

### 3. 变化发生在清零之前为什么也不丢

如果新变更发生时 `mWriteScheduled` 仍为 true，它不会排第二轮。但所有正常 JobStore 变更都在 `mLock` 保护下，而第一轮随后也要取得同一把 `mLock` 才复制。该变更在第一轮快照之前完成，所以会进入本轮。

可以总结成一句不变量：

```text
快照前的变更由本轮带走；快照后的变更能看到 scheduled=false，并排下一轮。
```

---

## 五、为什么持锁复制，锁外生成 XML

如果一边遍历活跃 JobSet，一边做 XML 序列化、内存扩容和磁盘写，`mLock` 会长时间占用，schedule、约束更新和完成回调都会被挡住。

r48 的选择是：

```text
mLock 内：筛选 persisted + new JobStatus(job)
mLock 外：构造整份 XML + AtomicFile 写入
```

这是一张时间点快照，不是“写文件期间自动跟着内存变化”。快照之后的变更由前一节的第二轮调度处理。

`new JobStatus(job)` 主要复制调度记录，底层不可变 `JobInfo` 仍被引用；写 extras 前，JobStore 又用 `deepCopyBundle(..., 10)` 制作最多 10 层的持久化副本。这样避免 XML 序列化期间直接遍历一个可能被外部代码异常修改的 Bundle。

快照不会把所有运行态写进 XML。r48 中尤其要记住：

- `numFailures` 不落盘；
- WorkItem pending/executing 不落盘；
- JobInfo 的 estimated network bytes 没有 XML 往返字段；
- 动态 controller 的瞬时 satisfied 状态不应被当作恢复事实；
- `lastSuccessfulRunTime`、`lastFailedRunTime` 会保存。

所以 jobs.xml 是“足以重建可调度定义和时间边界的子集”，不是 JobStatus 内存镜像。

---

## 六、AtomicFile 到底保护了什么

### 1. 正常提交链

JobStore 先在内存中生成完整 XML 字节，再开始文件事务：

```java
FileOutputStream out = mJobsFile.startWrite(startTime);
out.write(baos.toByteArray());
mJobsFile.finishWrite(out);
```

Android 11 的 AtomicFile 使用：

```text
jobs.xml.new  ← 写新内容
      ↓ sync + close
rename(jobs.xml.new, jobs.xml)  ← 原子替换基准文件
```

写 `.new` 期间，原 `jobs.xml` 仍保持旧的完整版本。只有完成 sync/close 后才 rename 替换，所以进程在半写时崩溃，通常不会把旧基准文件变成半截 XML。

旧实现遗留的 `.bak` 仍有兼容处理，但 r48 新写路径不是每次都先创建 `.bak`。

### 2. openRead 怎样处理残留 .new

如果基准文件和 `.new` 同时存在，`openRead()` 会把 `.new` 当作未完成写并删除，然后读取基准文件。它恢复的是上一份已提交状态，不会猜测半写的新文件是否可用。

### 3. 它不保证什么

AtomicFile 不保证：

- 调用者一定会调用 `finishWrite()`；
- I/O 一定成功；
- 写失败后自动重试；
- 多线程同时 startWrite 仍安全；
- `schedule()` 返回时磁盘已经提交；
- 业务数据与 JobInfo 跨文件原子提交。

它解决的是单个文件的替换完整性，不是整个持久化系统的可靠性。

---

## 七、r48 写入与清理路径还有哪些实现缺口

### 1. catch 里没有 failWrite

AtomicFile 契约要求：成功调用 `finishWrite(out)`，失败调用 `failWrite(out)`。但 r48 的 JobStore 把流定义在 try 内，catch 只记录日志：

```java
try {
    FileOutputStream out = mJobsFile.startWrite(startTime);
    out.write(bytes);
    mJobsFile.finishWrite(out);
} catch (IOException e) {
    // 只记日志
}
```

如果 `startWrite()` 已成功、后续 write 抛异常，这里没有显式 close、sync、删除 `.new`。已有基准文件时，下次 `openRead()` 通常会删掉残留 `.new` 并保留旧文件，但这仍不是正确完成 AtomicFile 失败协议；还可能暂时遗留流和临时文件。

这不是说旧 `jobs.xml` 一定损坏。更准确的结论是：AtomicFile 的“保旧文件”设计仍有价值，但 JobStore 没有完整执行它要求的失败清理。

### 2. 写失败不自动重试

`writeJobsMapImpl()` catch 后，外层 runnable 仍会把本轮标记为结束。代码没有因为 IOException 自动再次 post。只有后来又发生 persisted 状态变化，才可能触发下一轮写。

因此一次瞬时 I/O 失败可能让磁盘状态落后于内存，直到后续变化或 system_server 重启。`mPersistInfo.countAllJobsSaved` 也在 finally 中按已经遍历序列化的数量更新，不能把它当作 `finishWrite()` 成功提交的硬证据。

### 3. mWriteInProgress 只是测试/等待信号，不是提交日志

第一轮 runnable 开头已允许排第二轮，并把 `mWriteInProgress` 留为 true；但第二轮排队后，第一轮结束仍会直接设 false。此时第二轮可能还在 IoThread 队列中。

所以 `waitForWriteToCompleteForTesting()` 观察的布尔状态存在代际表达不足：它不携带“第几轮写”的计数。实际文件写仍由 IoThread 串行，不会因这个标志同时执行；受影响的是等待者可能过早认为所有排队轮次都结束。

这个测试辅助方法还有一个独立的小问题：循环里传给 `wait()` 的是 `now - start + maxWaitMillis`，而常见的“等待剩余时间”应是 `end - now`。当前写法可能一次等待得比剩余预算更久；外层下一轮虽会检查 deadline，但等待本身不精确。

### 4. 删除无效用户的批量入口没有主动排写

r48 的：

```java
removeJobsOfNonUsers(whitelist)
```

只调用 `mJobSet.removeJobsOfNonUsers()`，没有像 `clear()` 那样调用 `maybeWriteStatusToDiskAsync()`。如果其中删除了 persisted Job，而之后没有其他持久化变化触发全量写，磁盘文件可能暂时仍保留旧记录。这是源码实现缺口，不是 AtomicFile 能补救的事情。

---

## 八、为什么 elapsedRealtime 不能原样写进 jobs.xml

假设旧开机过程：

```text
旧 elapsed now = 5小时
Job earliest   = 5小时10分
```

如果把 `5小时10分` 原样落盘，重启后 elapsed 从 0 重新开始，系统会错误地再等 5 小时10分。

JobStore 写盘时把相对边界转换成 wall clock：

```text
deadlineWall = wallNow + (deadlineElapsed - elapsedNow)
delayWall    = wallNow + (earliestElapsed - elapsedNow)
```

例如写盘时：

```text
wallNow=10:00
还需等待10分钟
→ XML delay=10:10
```

重启后：

```text
new wallNow=10:04
new elapsedNow=2分钟
剩余=6分钟
→ new earliest elapsed=8分钟
```

源码换算为：

```java
earliest = nowElapsed + Math.max(delayWall - nowWall, 0);
```

若 10:10 已过，就用 0 作为剩余时间，让时间约束立即满足；不会生成负的 elapsed deadline。

`NO_EARLIEST_RUNTIME` 和 `NO_LATEST_RUNTIME` 是哨兵值，写读时要单独保留，不能拿它们当普通时间做减法。

---

## 九、开机 RTC 不可信时，为什么先加载再校正

### 1. 怎样判断时钟可疑

JobStore 读取 `jobs.xml` 的最后修改时间：

```java
mXmlTimestamp = mJobsFile.getLastModifiedTime();
mRtcGood = (systemClockNow > mXmlTimestamp);
```

若当前 wall clock 比文件修改时间还早，系统怀疑开机 RTC 尚未校准。文件不存在时 mtime 通常为 0，正常当前时间大于 0，所以通常视为可信的空状态。

注意构造时使用严格 `>`；后来判断“现在是否终于可校正”使用 `>=`。这是 r48 的具体边界。

### 2. 为什么不暂停所有 persisted Job

完全不加载会让充电、网络维护等任务在校时前全部消失。r48 选择先用当前 wall clock 临时换算，同时把 XML 中的原 UTC pair 保存在：

```text
JobStatus.mPersistedUtcTimes
```

这样系统可以先恢复 Job，又保留将来重算所需的原始数据。

### 3. 校时后不是改字段，而是 replacement

JSS 注册 `ACTION_TIME_CHANGED`。当当前 wall clock 达到文件时间的 sanity 条件后：

```text
BroadcastReceiver 注销自己
  → FgThread 执行 mJobTimeUpdater
  → mLock 内找出带 mPersistedUtcTimes 的旧 JobStatus
  → 按正确时钟创建 new JobStatus
  → cancelJobImplLocked(old, new, "deferred rtc calculation")
```

`JobStatus` 的 earliest/latest 是构造期字段，不适合原地修改。replacement 还能让各 StateController 以统一方式停止跟踪旧对象、开始跟踪新对象。

代价是：若某个受影响 Job 恰好正在运行，replacement 可能触发停止和重新调度。时钟校正不只是改显示时间，而是一次调度对象换代。

### 4. r48 的状态小缺口

接收器达到 sanity 后会注销并执行校正，但这段路径没有把 JobStore 的 `mRtcGood` 字段显式设回 true。正常运行依靠接收器只处理一次和所有受影响对象已 replacement；如果把 `jobTimesInflatedValid()` 当成永久实时状态查询，就会得到容易误解的结果。

---

## 十、读取失败时会恢复多少

开机读取从 `AtomicFile.openRead()` 开始：

- 文件不存在：视为没有 persisted Job，是正常空状态；
- 根标签或 schema version 无效：放弃整份文件；
- 单个 `<job>` 命中已处理的字段/构建校验错误并返回 null：该项跳过，其余项可继续；若错误升级成 XML/IO 异常，外层会中止本次读取，不能笼统认为所有单项损坏都可隔离；
- JobStatus 恢复后要 `prepareLocked()`，重新建立 URI 等运行所需资源；
- 读取到的 Job 直接加入启动期 JobSet，不走普通 add 的再次异步写。

r48 当前 `JOBS_FILE_VERSION` 为 0。版本不匹配直接中止读取，说明 schema version 是整文件兼容门，而不是“尽量猜字段”。

还有一个实现质量边界：`FileInputStream` 在正常路径末尾手动 close，但没有用 try-with-resources/finally 包住。解析/IO 中途异常会进入对应 catch，而 `prepareLocked()` 的运行时异常甚至不在该 catch 列表内；两种情况下，正常路径末尾的 close 都可能被跳过。

---

## 十一、jobs.xml 不是业务数据库

即使所有 Framework 路径都正常，仍有两个事务无法自动合并：

```text
应用数据库写入“照片待上传”
JobScheduler jobs.xml 写入 persisted JobInfo
```

它们属于不同进程、不同文件、不同提交协议。可能出现：

- 数据库已提交，Job 尚未持久化；
- Job 已持久化，业务记录尚未提交；
- Job 被执行，但业务状态在崩溃前没更新。

可靠设计通常让业务数据库成为事实来源：

```text
数据库保存待处理记录与幂等状态
persisted Job 只是“叫醒处理器”的恢复触发器
Job 运行后重新扫描数据库
```

这样 Job 重复运行不会重复结算，Job 偶尔丢失也能由下次启动、广播或健康检查重新 schedule。

---

## 十二、容易翻车的判断

### “schedule 返回成功，jobs.xml 已经更新”

不对。内存先成功，persisted 变化通常约 2 秒后才开始异步快照与写盘。

### “2 秒内一直有变化，计时器会一直往后顺延”

不对。r48 从第一笔变化 post 一次固定延迟 runnable，后续变化合并进去，不重置那次延迟。

### “AtomicFile 自动解决并发写”

不对。AtomicFile 文档明确要求调用者自己互斥；JobStore 靠单 IoThread 串行写。

### “catch IOException 就等于回滚完成”

不对。应调用 `failWrite()`；r48 JobStore catch 没有这样做。

### “countAllJobsSaved=5 证明磁盘提交了 5 个 Job”

不对。计数在 finally 更新，可能只证明序列化循环走过 5 项，不能替代 finishWrite 成功证据。

### “persisted Job 会保存 JobStatus 的所有字段”

不对。它保存可恢复子集，`numFailures`、WorkItem 队列和 r48 estimated network bytes 等不完整往返。

### “elapsedRealtime 可直接跨重启比较”

不对。它每次开机重新计时，必须借 wall clock 中转。

### “RTC 不可信时系统完全不加载 Job”

不对。r48 先临时换算并保存原 UTC pair，时钟可信后再 replacement 校正。

---

## 十三、macOS 静态练习：验证每一个完成点

以下命令都在 `/Users/ninebot/androidSource` 下只读执行。

### 练习 1：证明 2 秒只排一次

```bash
sed -n '324,338p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
```

记录 `mWriteScheduled` 在 post 时和 runnable 开始时分别怎样变化。

答案：首次变化设 true 并 postDelayed；后续不重复 post；runnable 开始先清 false，使快照之后的新变化能排下一轮。

### 练习 2：手画并发更新

```bash
sed -n '388,422p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
```

分别把新变更放在“清 scheduled 前”“清后取快照前”“快照后”。预期结论：前一种进入本轮；后两种可以排第二轮，不会既错过本轮又没有后续写。

### 练习 3：核对 AtomicFile 契约

```bash
sed -n '92,181p' frameworks/base/core/java/android/util/AtomicFile.java
```

检查 `startWrite`、`finishWrite`、`failWrite`，再回 JobStore 搜 `failWrite`。

答案：AtomicFile 要求成功/失败二选一收尾；JobStore 当前文件没有调用 `failWrite`。

### 练习 4：列出真正写入的字段

```bash
sed -n '480,612p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
```

做一张 `JobStatus field → XML attribute/tag` 表。若搜不到 `numFailures` 或 estimated network bytes，不要凭 JobInfo/JobStatus 中存在字段就认为已持久化。

### 练习 5：手算跨重启时间

```bash
sed -n '614,631p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
```

写盘时 wall=20:00、elapsed=3小时、earliest elapsed=3小时15分；重启后 wall=20:05、elapsed=1分钟。

答案：XML delay=20:15；剩余10分钟；新 earliest elapsed=11分钟。

### 练习 6：追 RTC 二阶段校正

```bash
sed -n '1463,1510p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java

sed -n '148,195p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
```

预期链：mtime sanity → TIME_CHANGED → FgThread → 收集 old/new 平行列表 → `cancelJobImplLocked` replacement。

---

## 阅读检查题与答案

### 1. `RESULT_SUCCESS` 到底先保证什么？

保证本次请求已被系统服务接受并进入当前内存调度状态；不保证异步 jobs.xml 已 fsync。

### 2. 为什么 runnable 要在取快照前把 mWriteScheduled 清 false？

让快照之后发生的变更能排下一轮；快照之前的变更则受同一 mLock 排序并进入本轮。

### 3. AtomicFile 写一半崩溃，为什么通常还能读旧文件？

新内容先写 `.new`，旧 base 在 finishWrite rename 前不被替换；下次读取会丢弃 base 旁的残留 `.new`。

### 4. 为什么说 AtomicFile 不等于写入必成功？

I/O、sync、rename 仍可能失败，调用者还必须正确调用 failWrite，并自行决定重试。r48 JobStore 在这些方面并不完美。

### 5. 为什么时间要借 wall clock 中转？

elapsedRealtime 在重启后归零，不能跨 boot 直接比较；wall clock 能把“目标绝对时刻”带到下一次开机，再换算成新 elapsed 边界。

### 6. RTC 错误时为什么保存 mPersistedUtcTimes？

当前 wall clock 算出的 elapsed 只是临时值；保存原 XML UTC pair 后，校时完成才能从原始数据重新计算，而不是在错误结果上二次换算。

---

## 本章 takeaway

分析 JobStore 问题时，按四个完成点逐层问：

```text
1. 内存 JobSet 是否已更新？
2. 这次变化是否进入某一轮 storeCopy？
3. AtomicFile 是否真正 finishWrite？
4. 重启读取后，RTC 时间边界是否已正确换算/校正？
```

再记住边界：

```text
persisted Job = 调度定义与部分时间状态可恢复
             ≠ API 返回时已经落盘
             ≠ JobStatus 全字段镜像
             ≠ 应用业务事务
```

下一章将接着看“调度历史怎样被统计”：第 136 章分析 `JobPackageTracker` 的 pending/active 计时、负载因子与历史环形缓冲。
