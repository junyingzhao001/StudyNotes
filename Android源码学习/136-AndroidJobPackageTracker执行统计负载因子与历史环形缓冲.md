# 136 Android JobPackageTracker：执行统计、负载因子与历史环形缓冲

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；本章没有在真机上制造调度竞争，也不提供未经测量的性能数字
>
> 前置阅读：第 121、131、132、135 章

## 先看结论：原始优先级不低，Job 为什么仍会失去调度优势

调试 Framework 内部 Job 时会遇到这样的现象：网络、充电与 quota 都没有阻塞，`JobInfo` 的原始 priority 也达到 30，本以为可以跳过 `JobRestriction`，最终却仍被热限制拦住；同一 scheduling UID 的新 Job 也可能没有按预期抢占旧任务。只盯原始 priority 无法解释，因为 **JSS 真正参与这些决策的是经过包级历史负载修正的 evaluated priority**。

Android 11 的 `JobPackageTracker` 会按工作真正归属的 source UID 与包名，累计这个包处于 pending 和普通 active 的时间。`JobSchedulerService` 再用最近两个统计批次计算 load factor；达到默认 0.5 或 0.9 时，分别把本轮 evaluated priority 下调 40 或 80。它还保留最近 100 条开始/停止事件，供 `dumpsys jobscheduler` 解释近况。

读完本章，你应当能做到三件事：

- 分清 registered、ready/pending、active 与 quota，不再把“已 schedule”误当作“正在排队”；
- 从源码手算某个包的 load factor，并判断它会不会丢失优先级或 `JobRestriction` 豁免；
- 正确阅读 package stats 与 event history，知道哪些字段只是易失诊断信息。

本章只讨论 `JobPackageTracker` 的负载反馈与诊断职责，不重讲 Job 约束求值、App Standby 配额算法或 JobService 回调状态机。

## 1. 先把它放回 JobScheduler 全链路

可以把 `JobPackageTracker` 理解成调度大厅里的“包级值班表”：它记录谁长期占着候选队列、谁长期占着执行槽，但它既不发放入场资格，也不直接规定每个包最多能跑几个 Job。

```text
应用 schedule/enqueue
        ↓
JobStore 保存 registered Job
        ↓ 约束、用户状态、备份状态等全部通过
mPendingJobs 加入 ready 候选 ── notePending()
        ↓
JobConcurrencyManager 计算 evaluated priority、分配槽位
        ↓ bindServiceAsUser() 成功
JobServiceContext 进入 active ── noteActive()
        ↓ 候选从 mPendingJobs 移除
结束 pending ── noteNonpending()
        ↓ 正常完成、超时、约束丢失或进程死亡统一清理
结束 active ── noteInactive()
```

这里最容易错读的是三个完成点：

| 名称 | 在 r48 中真正表示什么 | 不表示什么 |
|---|---|---|
| registered | Job 已进入 `JobStore` | 已满足约束 |
| pending | ready Job 已进入 `mPendingJobs` 等待分配 | 所有已 schedule 的 Job |
| active | `bindServiceAsUser()` 已返回成功，直到统一 cleanup | 应用的 `onStartJob()` 已回调或业务已开始 |

`JobPackageTracker` 位于 `system_server` 内部。虽然类声明为 `public final`，它不属于应用可调用的公开 SDK；应用只能通过公开的 `JobScheduler`/`JobInfo` API 间接触发这条内部链路。

## 2. 四个 note 钩子怎样与真实状态对齐

新 Job 只有在 `isReadyToBeExecutedLocked()` 为真、加入 `mPendingJobs` 时才会调用 `notePending()`。全量重建 ready 队列时，JSS 也会先为旧列表逐项 `noteNonpending()`，清空后再为新候选逐项 `notePending()`；否则 nesting 会悬空。

执行端的边界更值得注意。源码路径：

`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobServiceContext.java`

类/方法：`JobServiceContext.executeRunnableJob()`

```java
if (!binding) {
    mRunningJob = null;
    mRunningCallback = null;
    mParams = null;
    mExecutionStartTimeElapsed = 0L;
    mVerb = VERB_FINISHED;
    removeOpTimeOutLocked();
    return false;
}
mJobPackageTracker.noteActive(job);
```

这几行证明：bind 立即失败不会产生 active 记录；bind 成功就开始计 active，时间范围会覆盖后续的 BINDING、STARTING、EXECUTING 与 STOPPING，而不是只覆盖应用真正执行 `onStartJob()` 的那一小段。

离开执行槽最终汇入同一清理入口。源码路径仍是 `JobServiceContext.java`，方法是 `closeAndCleanupJobLocked()`：

```java
applyStoppedReasonLocked(reason);
completedJob = mRunningJob;
mJobPackageTracker.noteInactive(
        completedJob, mParams.getStopReason(), reason);
```

截取到这里已经足够证明调用方向：Tracker 先闭合 active 统计，statsd atom 是另一条记录链。正常 `jobFinished()`、stop ack、超时和宿主进程异常等路径虽然进入清理的原因不同，最终都由这里结束 active。

所有这些调用都依赖 JSS 的 `mLock` 串行化；`JobPackageTracker` 自己没有锁，也不是可从任意线程安全调用的通用统计器。它的 `dec*()` 也没有负数保护，成对调用是外部状态机必须维持的不变量。

## 3. 为什么按 source UID + package 记，而不是只看调用者

`scheduleAsPackage()` 允许系统组件代其他包安排工作。例如调度请求可能由 system UID 发起，真实工作却属于某个 SyncAdapter。若按 Binder calling UID 记账，大量工作都会错误归到 `system_server`。

因此四个 note 方法都使用：

```java
job.getSourceUid()
job.getSourcePackageName()
```

内部结构是：

```text
DataSet
└── SparseArray：sourceUid
    └── ArrayMap：sourcePackageName
        └── PackageEntry
```

完整 UID 已编码 userId，而同一 UID 下仍按包名拆分，所以 shared UID 的多个包不会在展示层被强行合并。事件历史另外保存 `batteryName` 与 jobId；`batteryName` 是诊断标签，不是统计主键。

这也解释了一个版本边界：`dumpsys jobscheduler` 的 UID 过滤会先取 `UserHandle.getAppId()`，Tracker 内部再次按 appId 比较。因此用某个完整 UID 过滤时，其他用户下相同 appId 的条目也可能出现；这里不是严格的“完整 UID 精确过滤”。

## 4. nesting 统计的是时间并集，不是每个 Job 时长相加

`PackageEntry` 为 pending、active、active-top 各保存一组 `pastTime/startTime/nesting/count`。nesting 的用途是把同包多个重叠 Job 合并为一段状态时间。

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobPackageTracker.java`

类/方法：`DataSet.incPending()`、`DataSet.decPending()`

```java
void incPending(int uid, String pkg, long now) {
    PackageEntry pe = getOrCreateEntry(uid, pkg);
    if (pe.pendingNesting == 0) {
        pe.pendingStartTime = now;
        pe.pendingCount++;
    }
    pe.pendingNesting++;
}

void decPending(int uid, String pkg, long now) {
    PackageEntry pe = getOrCreateEntry(uid, pkg);
    if (pe.pendingNesting == 1) {
        pe.pastPendingTime += now - pe.pendingStartTime;
    }
    pe.pendingNesting--;
}
```

只用一个例子就能看清：A 在 0～20 秒 pending，B 在 5～10 秒 pending，C 在 25～30 秒 pending。

- A 让 nesting 从 0 变 1，启动第一段秒表并令 count 加 1；
- B 只把 nesting 变成 2，不重复启动秒表；
- A、B 全部离开时结算 0～20 秒；
- C 再次把 0 变 1，形成第二段 episode。

最终 `pendingTime=25s`、`pendingCount=2`，不是三个 Job 时长相加得到 30s，也不是三个 Job 就得到 count 3。active 与 active-top 使用同一算法。

stop reason 的计数单位又不同：每次 `decActive()`/`decActiveTop()` 都会为该 Job 的 reason 加 1，即使多个 active Job 的持续时间被 nesting 合并。因此一行 dump 里同时存在三种口径：状态时间并集、episode 次数、逐 Job 停止原因次数。

## 5. active-top 为什么单独记，又为什么不进入负载因子

`JobConcurrencyManager` 在分配槽位前计算优先级，并把结果保存到 Job：

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobConcurrencyManager.java`

方法：`assignJobsToContextsLocked()`

```java
final int priority = mService.evaluateJobPriorityLocked(pending);
pending.lastEvaluatedPriority = priority;

mJobCountTracker.incrementPendingJobCount(isFgJob(pending));
```

Tracker 随后以 `lastEvaluatedPriority >= JobInfo.PRIORITY_TOP_APP` 为界，把执行时间放进 active-top 或普通 active。这里的“top”是一次内部调度优先级快照，不等于应用此刻可见，也不等于 JobService 调用了 `startForeground()`。

active-top 有独立的时长、nesting 与 count，能在 dump 中观察，却被明确排除在 load factor 之外。由代码结构可以推断，这避免把用户当前交互带来的 TOP 紧迫工作直接计入后台公平性惩罚；这是设计意图推断，不是公开 API 承诺。

还要纠正一个容易顺手推过头的结论：负载降权不会把 TOP Job 降为后台 Job，因为 `adjustJobPriority()` 对 `PRIORITY_TOP_APP` 及以上根本不做调整。load factor 会参与普通 Job 的 evaluated priority 计算，并影响下文所述的 restriction 豁免与同 scheduling UID 抢占，但不能据此宣称它会改变 r48 的 TOP/非 TOP 并发分类。

## 6. 30 分钟批次为何不是准点窗口

Tracker 同时使用三只时钟，不能把它们统称为“当前时间”：

| 时钟 | 用途 | 深睡时是否前进 |
|---|---|---|
| uptime | pending/active 时长、批次长度、load factor 分母 | 否 |
| elapsed realtime | 100 条事件的时间和“多久以前” | 是 |
| wall clock | dump 中批次起始日期标签 | 会受改时间影响 |

因此设备深睡两小时，不会自动给负载分母增加两小时；但事件 history 的“多久以前”会包含这段真实经过时间。

批次名义长度是 30 分钟 uptime，真正轮转却只发生在四个 note 状态钩子附近，而且条件是严格大于 30 分钟。

源码路径：`JobPackageTracker.java`

方法：`rebatchIfNeeded()`

```java
void rebatchIfNeeded(long now) {
    long totalTime = mCurDataSet.getTotalTime(now);
    if (totalTime > BATCHING_TIME) {
        DataSet last = mCurDataSet;
        last.mSummedTime = totalTime;
        mCurDataSet = new DataSet();
        last.finish(mCurDataSet, now);
        System.arraycopy(mLastDataSets, 0, mLastDataSets, 1,
                mLastDataSets.length-1);
        mLastDataSets[0] = last;
    }
}
```

没有 Handler 定时器，所以长时间没有状态转换时，当前批可以远长于 30 分钟。`dump()` 与 `getLoadFactor()` 也不会主动轮转。

`finish()` 会把仍在 pending/active 的 nesting 与新的 startTime 迁到下一批，并在旧批结算到切点；它不增加 count，因为这仍是同一个连续 episode。最多保留 5 个已结束批次，但不能把它简单说成严格的“最近 2.5 小时”或“最近 3 小时”墙钟窗口。

顺序也有意义：进入状态的 `notePending()`/`noteActive()` 先轮转再 inc；离开状态的 `noteNonpending()`/`noteInactive()` 先 dec 再轮转。这样跨过批次边界后结束的 episode 会先在旧批正确闭合。

## 7. load factor 怎样反馈到优先级

对同一 source UID + package，r48 只取当前批与最近一个已结束批：

```text
time   = current(pending + ordinary active)
       + previous(pending + ordinary active)

period = current batch uptime + previous batch uptime

load factor = time / period
```

源码路径：`JobPackageTracker.java`

方法：`getLoadFactor()`（以下是方法中计算窗口的连续关键片段）

```java
final long now = sUptimeMillisClock.millis();
long time = 0;
if (cur != null) {
    time += cur.getActiveTime(now) + cur.getPendingTime(now);
}
long period = mCurDataSet.getTotalTime(now);
if (last != null) {
    time += last.getActiveTime(now) + last.getPendingTime(now);
    period += mLastDataSets[0].getTotalTime(now);
}
return time / (float)period;
```

active 与 pending 是两本独立账，可以同时为正。例如同包 A 已 active、B 一直 pending，两个比例会直接相加，不扣交集，所以 factor 可以大于 1。它衡量的是“占槽压力 + 等槽需求”，不是 CPU 使用率、耗电比例或互斥饼图。

默认阈值与反馈位于 `JobSchedulerService.adjustJobPriority()`：

```java
if (curPriority < JobInfo.PRIORITY_TOP_APP) {
    float factor = mJobPackageTracker.getLoadFactor(job);
    if (factor >= mConstants.HEAVY_USE_FACTOR) {
        curPriority += JobInfo.PRIORITY_ADJ_ALWAYS_RUNNING;
    } else if (factor >= mConstants.MODERATE_USE_FACTOR) {
        curPriority += JobInfo.PRIORITY_ADJ_OFTEN_RUNNING;
    }
}
return curPriority;
```

r48 默认 `MODERATE_USE_FACTOR=0.5`、`HEAVY_USE_FACTOR=0.9`，对应的 adjustment 是 `-40` 与 `-80`；比较包含等号。阈值能由 JobScheduler 的 settings constants 覆盖，它们不是稳定的公开 API 合同。

假设观察期为 60 分钟，普通 active 为 24 分钟、pending 为 18 分钟、active-top 为 10 分钟：

```text
factor = (24 + 18) / 60 = 0.70
```

active-top 不参与，默认进入 moderate 档，当前用于计算的 priority 减 40。若 active 20 分钟、pending 35 分钟，即使两者重叠 15 分钟，源码仍得到 `(20+35)/60≈0.917`，进入 heavy 档。

### 降权究竟影响哪里

全树检索 `evaluateJobPriorityLocked()` 后，r48 中与决策直接相关的消费者主要是：

- `checkIfRestricted()`：只有 evaluated priority 至少为 `PRIORITY_FOREGROUND_APP` 才跳过 `JobRestriction`；降权可能让普通高优先级 Job 失去这层豁免；
- `JobConcurrencyManager`：保存 pending Job 的 `lastEvaluatedPriority`，并在同 scheduling UID（`JobStatus.getUid()`）的运行中 Job 抢占比较里使用优先级；
- dump/proto：展示当时重新计算的 evaluated priority。

它不会直接取消 Job，也不会修改 `QuotaController` 的 quota constraint，更不会改写原始 `JobInfo.priority` 或 jobs.xml。`mPendingJobs` 的全局排序在 r48 只比较 override state 与 enqueueTime，不能把 load factor 说成“直接按优先级重排整个 pending 队列”。

## 8. 它与 QuotaController 是两套政策

| 问题 | JobPackageTracker | QuotaController |
|---|---|---|
| 观察什么 | 包级 pending + 普通 active 比例 | standby bucket 对应滚动窗口中的执行账 |
| 产出什么 | evaluated priority 的负调整与 dump 统计 | quota 约束是否满足、恢复调度 |
| TOP/前台处理 | active-top 单列且不进 factor | 按 TOP-started、UID 前台、充电、bucket 等自己的规则计费 |
| 保存多久 | 内存 current + 5 个历史批次 | 自己的 session/统计窗口 |
| 重启恢复 | 不恢复 | 与 Tracker 不是同一账本或恢复路径 |

所以“quota 没超额”与“历史负载没有降权”是两个独立命题。排查 ready Job 不运行时，需要把约束、restriction、批处理、并发槽与优先级分别核对，而不是看到某一层通过就推断整个系统一定会立即执行。

## 9. package stats 与 100 条 history 各回答什么

Tracker 有两套彼此独立的数据：

- `DataSet`：回答某包在窗口内 pending/active 了多久、形成几段 episode、以哪些标准原因结束，以及系统并发峰值；
- 100 槽 event ring：回答最近哪些 Job 按什么顺序开始或停止。

`dump()` 会把 `mLastDataSets[0] + mCurDataSet` 合成 `Current stats`，而 `mLastDataSets[1..4]` 才逐批打印为 `Historical stats`。这与 load factor 的两批观察窗口对齐，但更早的历史批只用于诊断，不参与当前 factor。

并发峰值 `mMaxTotalActive/mMaxFgActive` 属于整个 DataSet，不属于某个包。它由 `JobConcurrencyManager` 完成本轮槽位规划后调用 `noteConcurrency()` 更新，是调度器的并发计数投影，不是对应用线程或 CPU 核心并行度的采样。

事件 ring 使用 `RingBufferIndices` 与多组并行数组保存 command、elapsed time、source UID、battery tag、jobId 和 debug reason；第 101 条会覆盖最老事件。文本 dump 按从旧到新输出，最新事件在底部；文本时间显示为负偏移，Proto 写正的 `TIME_SINCE_EVENT_MS`。

文本停止事件如果有 `debugReason` 会优先显示内部字符串；Proto 不保存该字符串，只写结构化 stop reason。因此两种输出不是字段完全等价的编码。

### r48 必须特别标出的 STOP 标签缺口

源码路径：`JobPackageTracker.java`

方法：`noteInactive()`

```java
if (job.lastEvaluatedPriority >= JobInfo.PRIORITY_TOP_APP) {
    mCurDataSet.decActiveTop(job.getSourceUid(), job.getSourcePackageName(), now,
            stopReason);
} else {
    mCurDataSet.decActive(job.getSourceUid(), job.getSourcePackageName(), now, stopReason);
}
rebatchIfNeeded(now);
addEvent(job.getJob().isPeriodic() ? EVENT_STOP_JOB : EVENT_STOP_PERIODIC_JOB,
        job.getSourceUid(), job.getBatteryName(), job.getJobId(), stopReason, debugReason);
```

start 路径使用“周期 → `START-P`，一次性 → `START`”，而这段 stop 三元表达式反了：在 Android 11 r48 中，周期 Job 会记为 `STOP`，一次性 Job 会记为 `STOP-P`。这个缺口只影响事件 command/标签，不影响 PackageEntry 的 active 时长与 stop reason 聚合。排障时必须以版本和源码为准，不能凭标签名称反推 Job 类型。

## 10. 诊断边界：这些数据不能证明什么

| 看到的现象 | 可以说明 | 不能直接说明 |
|---|---|---|
| `30% 4x pending` | 窗口内至少一个同包 Job pending 的并集约占 30%，形成 4 段 episode | 一共有 4 个 Job，或 CPU 用了 30% |
| `(active)` | 生成合并快照时仍有 active 活动 | 整个窗口始终 active |
| stop reason 计数 | 已 active Job 离开时的标准原因次数 | 所有未运行 Job 的失败原因 |
| 最近 100 条 history | 近况中的 start/stop 顺序 | 完整审计、计费或跨重启事实 |
| package entry 仍存在 | 观察窗口内出现过该 source 包 | 包当前仍安装或仍有 registered Job |

Tracker 没有 AtomicFile、XML 或数据库持久化。`system_server` 重启后，批次、ring history 与负载记忆全部清零；persisted Job 从 jobs.xml 恢复并不意味着这套统计也恢复。包卸载后也没有专门删除历史 entry，数据只会随批次淘汰或事件覆盖自然消失。

此外，`period==0` 没有显式保护：系统刚创建 Tracker、同一 uptime tick 内就查询时，`0/0` 可得到 NaN；与阈值的 `>=` 比较都会为 false。生产路径通常很快跨过该边界，但静态阅读不能把它说成数学上永远有合法分母。

## 11. 从源码验证一次“原始优先级不低却仍被限制”

当前目录是 Android 11 r48 源码，以下命令只读，不需要在 Mac 上编译：

1. 找到四个状态钩子及调用者：

   ```bash
   rg -n 'notePending|noteNonpending|noteActive|noteInactive' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job
   ```

2. 从 `JobSchedulerService.isReadyToBeExecutedLocked()` 区分 ready 条件与 `checkIfRestricted()`，再追 `maybeRunPendingJobsLocked()` 到 `JobConcurrencyManager.assignJobsToContextsLocked()`。

3. 在 `JobConcurrencyManager` 里记录 `lastEvaluatedPriority` 的写入点、空闲槽判定与同 UID 抢占比较；不要假设 pending 列表按它全局排序。

4. 在 `JobPackageTracker.getLoadFactor()` 手算 current + last[0]，并回到 `adjustJobPriority()` 对照运行时的 moderate/heavy constants。

5. 最后才用 package stats、pending queue、active contexts 与 history 交叉验证。若未来有设备，可执行 `adb shell dumpsys jobscheduler` 采集现场；本章没有声称已经跑过该设备验证。

这条顺序能排除四类不同原因：还没 ready、因 evaluated priority 降低而进入 restriction 检查、进入 pending 但没有槽、已有槽却卡在 bind/回调/cleanup。Tracker 只解释其中与历史负载和近况有关的一部分。

## 12. 练习与参考答案

### 练习一：手算 nesting

A 在 0～20 秒 pending，B 在 5～10 秒 pending，C 在 25～30 秒 pending。求 `pendingTime` 与 `pendingCount`。

参考答案：`pendingTime=25s`，`pendingCount=2`。A、B 的时间重叠，只形成 0～20 秒这一段；C 形成第二段。count 统计 0→1 的 episode，不统计 Job 个数。

### 练习二：手算负载反馈

current + previous 总 period 为 60 分钟，普通 active 为 20 分钟、pending 为 35 分钟、active-top 为 12 分钟，active 与 pending 重叠 15 分钟。使用默认阈值时 factor 和 adjustment 是多少？

参考答案：源码不扣交集，也不计 active-top，所以 `factor=(20+35)/60≈0.917`；达到 heavy 的 0.9，adjustment 为 `-80`。

### 练习三：判断三个说法

1. “Job 出现在 JobStore，就已经被记为 pending。”
2. “负载降权会把 TOP_APP Job 降成普通后台 Job。”
3. “r48 history 中的 STOP-P 能证明它是周期 Job。”

参考答案：三个都错。只有 ready 候选进入 `mPendingJobs` 才 notePending；TOP_APP 及以上跳过负载 adjustment；r48 的 `noteInactive()` 恰好把周期/一次性 STOP command 选反。

### 练习四：核对版本缺口

执行下面的只读命令，把 start 与 stop 的三元表达式并排比较：

```bash
sed -n '467,492p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobPackageTracker.java
```

参考答案：start 是 `isPeriodic ? EVENT_START_PERIODIC_JOB : EVENT_START_JOB`；stop 却是 `isPeriodic ? EVENT_STOP_JOB : EVENT_STOP_PERIODIC_JOB`。这证明 STOP 标签反向是 r48 源码事实，而不是从输出名称做的猜测。

## 本章带走什么

`JobPackageTracker` 不是 CPU 计量器，也不是另一套配额控制器。它用 source 包的“普通 Job 占槽时间 + ready Job 排队时间”形成最近两批的调度压力，再把压力反馈为普通 Job 的 evaluated priority；与此同时，它用易失的批次统计和 100 条事件帮助解释近况。读 dump 时始终把约束、quota、restriction、并发槽、优先级和应用回调分成不同完成点，才能准确回答“为什么这个 Job 的原始 priority 不低，真正参与决策时却不再占优势”。
