# 141 Android JSS：Handler 消息、批处理与增量状态变化

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 131、136、138、140 章

## 先看问题：一个约束刚满足，为什么 Job 仍可能不进 pending

Controller 发现充电、网络或 deadline 状态变化时，并不直接调用应用。它先修改 `JobStatus` 的约束事实，再通过 `StateChangedListener` 给 `JobSchedulerService` 发消息。JSS Handler 随后决定是只插入一个 Job、贪婪地重建全部候选，还是应用“至少 5 个或最多等待约 31 分钟”的批处理政策。

因此“约束满足”到“进入 pending”之间，还有消息类型、消息处理时状态、`mReportedActive`、batch 计数和完整 ready 门。尤其在 Android 11 r48，31 分钟只是下一次扫描时比较的阈值，没有专门 Alarm；普通批处理路径异步投递的 `MSG_STOP_JOB` 还存在对象代际变陈旧的窗口。

本章只回答一个问题：**Controller 的一次状态变化，如何经 Handler 合并、全量或单项扫描与批处理，最终改变 pending，并在哪些时序下得到反直觉结果？**

## 1. 三层职责与线程边界

```text
Controller
  更新约束位，选择“普通变化”或“尽快运行”通知
        ↓ Message
JobSchedulerService.JobHandler（system_server 主 Looper）
  在 mLock 内选扫描策略、重建 pending、请求停止 active
        ↓
JobConcurrencyManager
  按槽位、前后台容量、优先级与同 UID 抢占分配 context
```

JSS 构造 `new JobHandler(context.getMainLooper())`。统一到主 Looper 不等于不需要锁：schedule/cancel 仍可从 Binder 线程同步进入，Controller 也可能来自其他回调线程，所以核心集合继续由 `mLock` 串行保护。

`handleMessage()` 获锁后先检查 `mReadyToRock`。第三方应用可启动阶段前的消息直接返回，不在 Handler 中延迟重放；后续 boot phase 会 attach 已加载 Job 到各 Controller，并发送一次检查消息，启动正确性依赖这次统一接管。

## 2. 三类扫描消息分别承诺什么

| 消息 | 携带对象 | 处理策略 | 同类消息合并 |
|---|---|---|---|
| `MSG_JOB_EXPIRED` | 特定 `JobStatus` 或 null | 对象仍 ready 时单项插入；否则贪婪全量扫描 | 不合并 |
| `MSG_CHECK_JOB` | 无 | `mReportedActive` 为 true 时贪婪扫描，否则走普通批处理扫描 | 处理时删除队列中其他 CHECK |
| `MSG_CHECK_JOB_GREEDY` | 无 | 贪婪全量扫描 | 不合并 |

`onControllerStateChanged()` 只投递 `MSG_CHECK_JOB`，不附带“哪些 Job 改了”的列表。所谓增量主要发生在 Controller 内部：它只更新受事件影响的 tracked Job；通知到了 JSS，候选政策仍从 JobStore 全表计算。

`removeMessages(MSG_CHECK_JOB)` 位于处理分支内，不是 post 前去重。已排队的多个普通变化可压成一次扫描，因为最新约束事实保存在 JobStatus 中；这减少重复工作，却不提供每个中间边沿都被观察的保证。

## 3. `run now` 既可能是单项提示，也可能是全量冲刷

`onRunJobNow(jobStatus)` 总是投递 `MSG_JOB_EXPIRED`。名字来自 deadline，但 Battery、Storage、Connectivity 等 Controller 也复用它，不能看到 `EXPIRED` 就断言一定是时间约束到期。

r48 分支原文为：

```java
                    case MSG_JOB_EXPIRED: {
                        JobStatus runNow = (JobStatus) message.obj;
                        // runNow can be null, which is a controller's way of indicating that its
                        // state is such that all ready jobs should be run immediately.
                        if (runNow != null && isReadyToBeExecutedLocked(runNow)) {
                            mJobPackageTracker.notePending(runNow);
                            addOrderedItem(mPendingJobs, runNow, sPendingJobComparator);
                        } else {
                            queueReadyJobsForExecutionLocked();
                        }
                    } break;
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

null 是“冲刷所有 ready Job”的协议。非 null 对象只有在处理消息时仍通过完整 ready 门，才按 `overrideState`、`enqueueTime` 排序插入。完整门会排除已 pending、已 active 或已不在 JobStore 的对象，所以不会重复 `notePending()`。

反直觉点在 `else`：一个 specific 对象若在排队期间被 cancel/replacement、约束再次失效或已进入 pending，JSS 不是忽略 stale 提示，而是退化为一次贪婪全量扫描。这可能让完全无关的 ready Job 越过普通 batch 门。

## 4. 全量扫描为何先把 pending 清空

`queueReadyJobsForExecutionLocked()` 与 `maybeQueueReadyJobsForExecutionLocked()` 都先执行：

```java
        noteJobsNonpending(mPendingJobs);
        mPendingJobs.clear();
        stopNonReadyActiveJobsLocked();
```

然后遍历 JobStore，重新用 `isReadyToBeExecutedLocked()` 构造候选。这样不会在旧队列上做复杂的增删补丁，所有 user、backup、restriction、组件和代际条件都按当前快照重算；不再 ready 的对象也自然退出。

代价是统计语义：原 pending Job 先 `noteNonpending`，同一轮若再次入队又 `notePending`。包级 pending 总时长通常只出现极短断点，但 nesting 回到零时会切分 episode 并增加计数。因此 PackageTracker 的 pending episode 不是 schedule 调用次数，也不等于业务任务数。

全量重建最后统一排序；specific `run now` 则用 `binarySearch` 插入已有有序队列。两条路径使用同一 comparator：先看 shell override，再看 enqueueTime，不按 evaluated priority 全局排序。

## 5. 扫候选前，active Job 只过一组较窄的停止门

`stopNonReadyActiveJobsLocked()` 先检查运行对象的 `JobStatus.isReady()`；不 ready 时以 constraints reason 停止，若 RESTRICTED bucket 的动态约束未满足则优先使用 restricted-bucket reason。仍 ready 时再检查 `JobRestriction`，例如第 137 章的 thermal restriction。

这里没有调用第 138 章的完整 `isReadyToBeExecutedLocked()`。用户是否 started、source UID 是否 backup、组件是否仍可用等外部门不在这一个函数内统一处理，可能由各自生命周期入口或其他逻辑负责。不能把“全量候选门”直接套成“active 统一停止门”。

停止仍是异步协议：扫描只向 JobServiceContext 请求 cancel，槽位要等 stop ACK、超时或连接清理后才真正可用。

## 6. 贪婪扫描与普通批处理的唯一区别

贪婪 functor 把每个通过完整 ready 门的 Job 都加入新列表，不看 standby bucket、失败次数和 batch 阈值；未通过的 Job 会调用 `evaluateControllerStatesLocked()`，让少数实现该钩子的 Controller更新派生状态。

普通 `MaybeReadyJobQueueFunctor` 对每个 ready Job 分类：

```text
RESTRICTED bucket                         → force-batched
否则，numFailures > 0                    → unbatched
否则，minReady > 1、非 ACTIVE、等待未超时 → force-batched
其他                                     → unbatched
```

postProcess 不是挑选部分 Job，而是决定整批 runnableJobs 是否全部放行：

```java
            if (unbatchedCount > 0
                    || forceBatchedCount >= mConstants.MIN_READY_NON_ACTIVE_JOBS_COUNT) {
```

只要存在一个 unbatched Job，所有本轮 ready Job 都一起入 pending；或者 force-batched 数达到门槛，也整批放行。于是一个失败重试或 ACTIVE Job 能让 RESTRICTED/非 ACTIVE Job“搭车”。当两项都不满足时，本轮一个都不入队。

默认 `MIN_READY_NON_ACTIVE_JOBS_COUNT=5`。例如 4 个 RARE 首次 ready 时先等待；第 5 个在某次 CHECK 中也 ready，五个一起放行。若再出现一个 ACTIVE Job，即使 force-batched 仍不足 5，因为 `unbatchedCount>0`，整批也一起放行。

## 7. 31 分钟是比较阈值，不是唤醒保证

默认 `MAX_NON_ACTIVE_JOB_BATCH_DELAY_MS=31 * MINUTE_IN_MILLIS`。一个 Job 第一次被判为 force-batched 时，JSS 才写 `firstForceBatchedTimeElapsed`。后续扫描满足：

```text
nowElapsed - firstForceBatchedTimeElapsed >= 31 分钟
```

它就改归 unbatched，并带动整批放行。计时使用 elapsed realtime，包含设备休眠时间。

关键限制是这段逻辑没有为 31 分钟阈值单独设 Alarm。时间静默流逝不会自行进入 Handler；必须由新的 Controller 消息、调度/完成事件或其他检查触发下一次扫描。因此“最多等待 31 分钟”不是 r48 的严格墙钟延迟保证。

时间戳也不会因为 Job 暂时不 ready 而清零。同一 `JobStatus` 再次 ready 时可能沿用旧起点；replacement 创建新代际时通常回到 0。`firstForceBatchedTimeElapsed` 表示首次被判强制聚批，不表示此后一直满足约束。

## 8. `mReportedActive` 为什么会改变下一条 CHECK 的政策

`reportActiveLocked()` 在 pending 非空时直接把 active 判为 true；pending 为空时，只有正在运行且不属于 foreground、doze whitelist 或 active-UID 豁免的 Job 才算 active。它不是“至少一个 JobServiceContext 正在执行”的同义词。

所以普通批处理扫描一旦放入 pending，末尾 `maybeRunPendingJobsLocked()` 即使暂时没有槽位，也会令 `mReportedActive=true`。下一条普通 CHECK 随即选择贪婪全量扫描，从而让其他 ready Job 不再过 batch 数量门。

这是一种系统级忙碌反馈，不是 per-package 公平性。pending 对象在没有槽位时通常仍留队；真正是否启动由 JobConcurrencyManager 决定，可能受 FG/BG 容量、内存压力、优先级和抢占规则限制。

## 9. start-mode 检查只在普通批处理路径，且 STOP 消息有代际窗口

`MaybeReadyJobQueueFunctor.accept()` 在完整 ready 后额外问 AMS：`isAppStartModeDisabled(job.getUid(), servicePkg)`。若 disabled，它不在当前循环直接取消，而是投递：

```java
                        mHandler.obtainMessage(MSG_STOP_JOB, job).sendToTarget();
                        return;
```

贪婪 functor与 specific `MSG_JOB_EXPIRED` 路径没有这次二次检查。schedule 入口虽然也检查 start mode，但注册后状态可以变化，所以这是 r48 路径不对称，不能说所有入 pending 路径都受同一个 start-mode gate。

异步 STOP 还制造了代际窗口：当前 Handler turn 释放 `mLock` 后，Binder 线程可能用同 UID+jobId replacement。下一 turn 的 `MSG_STOP_JOB` 仍携带旧 `JobStatus`，并直接调用 `cancelJobImplLocked(old, null)`，没有先确认旧对象仍在 JobStore。

由于 JobStore/`mPendingJobs` 使用对象身份，旧对象通常删不到新代际；旧对象若已在 replacement 中 unprepare，这里还会记录一次重复 unprepare 的 wtf。但 `stopJobOnServiceContextLocked()` 以 `(scheduling UID, jobId)` 匹配 active context。若新代际已经运行，stale STOP 可能请求停止新实例。它不会简单地“把新 Job 从 JobStore 删除”，影响集中在逻辑键匹配的 active 停止与后续回调/重排链。

这是 Android 11 r48 的静态时序缺口；是否命中取决于消息、replacement 和分槽顺序，不能仅凭源码推出发生概率。

## 10. 每条有效消息末尾都会尝试分槽

除 `mReadyToRock=false` 的早退外，switch 结束后统一调用 `maybeRunPendingJobsLocked()`，内部先让 JobConcurrencyManager 分配 context，再 `reportActiveLocked()`。即使 UID 状态消息或 STOP 消息没有新建 pending，也会触发一次分配尝试。

`maybeRunPendingJobsLocked()` 不再逐个复查完整 ready；它信任前面的 pending 构造与 Controller 通知。约束在 pending 后转负时，需要相应消息促使队列重建；这也是 Controller 通知正确性属于系统内部契约的原因。

抢占也不是“同一轮立即换人”：并发管理器可能先向旧 context 发 stop，待 cleanup 完成后的新检查才能真正启动候选。deadline/run-now 表示绕过普通 batch 倾向，不表示绕过 user、backup、restriction、component 或槽位限制。

## 11. Controller 为什么选择不同通知

| 来源 | 典型通知 | 意图 |
|---|---|---|
| Time deadline 满足且 Job ready | `onRunJobNow(job)` | specific 尽快入队 |
| Connectivity 发现某 Job 可用 active network | `onRunJobNow(job)` | specific 尽快入队 |
| Battery 条件转正 | `onRunJobNow(null)` | 冲刷所有 ready Job |
| Storage 转为 not-low | `onRunJobNow(null)` | 冲刷所有 ready Job |
| Battery/Storage 负向变化 | `onControllerStateChanged()` | 让 JSS 重建并停止不再 ready 的 active |
| 退出 Doze、restricted bucket 改变 | `MSG_CHECK_JOB` | 重新按当前政策扫描 |

正负边沿策略并不统一成一个抽象规则，应回到各 Controller 的调用点确认。一次 null flush 或 stale specific 消息可能触发全量 greedy，这正是消息语义会跨 Job 扩散的地方。

## 12. API、实现与版本边界

- 应用可观察的是 Job 是否被调用、停止或稍后重投；`MSG_*`、batch 常量、`mReportedActive` 和 functor 都是内部实现。
- 5 个与 31 分钟是 r48 默认值，可被 Settings 常量覆盖，也可能随版本改变。
- `getAllPendingJobs()` 的 public “pending”仍指已注册 JobInfo 列表，不等于本文内部 `mPendingJobs` 候选队列。
- Handler 消息名不是原因或优先级契约；`MSG_JOB_EXPIRED` 可由非时间 Controller 发送。
- 静态阅读能证明分支和可能的竞态窗口，不能证明设备上消息延迟、扫描耗时或竞态命中率。

## 13. 从源码验证消息到 pending

在 Android 11 r48 源码根目录只读执行：

1. 读 Listener、消息分支和末尾统一分槽：

   ```bash
   sed -n '1880,1985p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 比较 active 停止、greedy functor 与 maybe functor：

   ```bash
   sed -n '2010,2190p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 核对默认 5/31 分钟及配置是否有 clamp：

   ```bash
   sed -n '490,680p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

4. 追四类 Controller 的通知选择：

   ```bash
   rg -n 'onRunJobNow|onControllerStateChanged' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers
   ```

5. 验证 stale STOP 的对象删除与逻辑键停止差异：

   ```bash
   sed -n '1260,1290p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '1610,1660p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

## 14. 练习与参考答案

### 练习一：手算 batch

同一轮有 4 个首次 ready 的 RARE Job、1 个首次 ready 的 RESTRICTED Job，无失败、无 ACTIVE，默认常量下是否入队？

参考答案：会。五个都计入 forceBatchedCount，达到默认门槛 5，postProcess 将整批 runnableJobs 放入 pending。

### 练习二：ACTIVE 搭车

3 个 RARE 与 1 个 ACTIVE 同轮 ready，结果如何？

参考答案：ACTIVE 计入 unbatchedCount，使条件立即成立；四个 runnable Job 全部入队，不只放行 ACTIVE。

### 练习三：静默 40 分钟

一个 RARE Job 首次被 force-batched 后没有任何新消息，40 分钟后是否必然自动入队？

参考答案：不必然。31 分钟没有专用唤醒；只有下一次扫描才计算 delay expired。

### 练习四：stale specific

TimeController 发出某 Job 的 run-now，处理前它被 replacement。消息到达后怎样？

参考答案：旧对象因 `mJobs.containsJob(old)==false` 不能走单项插入，分支退化为 greedy 全量重建，新代际若满足完整门可能作为全表候选进入。

### 练习五：stale STOP

普通 maybe 扫描为旧对象投递 STOP，随后同键 replacement 并启动新代际。STOP 能否从 JobStore 删除新对象，能否停止新 active？

参考答案：前者通常不能，因为删除使用旧对象身份；后者可能，因为 active context 的匹配只看 scheduling UID+jobId。

## 本章带走什么

Controller 通知不是“立即执行”命令，而是把事实变化交给 JSS Handler 重新解释。specific run-now 只有对象在处理时仍 ready 才单项插入；null 或 stale specific 都会触发 greedy 全量扫描。普通 CHECK 在空闲态使用 5 个/31 分钟聚批，但 31 分钟不会自行唤醒，且任一 unbatched Job 会带整批一起进入 pending。

读这条链时还要保留三个边界：全量重建会切分 pending 统计 episode；active 停止门比完整候选门窄；start-mode 只在 maybe 路径二次检查，异步 stale STOP 甚至可能按逻辑键停止 replacement 后的新 active 代际。最后，进入 pending 只是交给并发管理器争槽，不是应用已开始执行。
