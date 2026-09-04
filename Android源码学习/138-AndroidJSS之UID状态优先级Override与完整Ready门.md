# 138 Android JSS：UID 状态、优先级 Override 与完整 Ready 门

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或运行 system_server
>
> 前置阅读：第 121、129、130、131、136、137 章

## 先看结论：命令显示 `ready`，Job 为什么仍没进入 pending

排查 JobScheduler 时，一个很迷惑的现场是：`cmd jobscheduler get-job-state` 打印了 `ready`，网络、电量等约束也都满足，但 `dumpsys jobscheduler` 的 Pending queue 里没有它。继续重复 schedule 通常解决不了问题，因为这里的 `ready` 只来自 `JobStatus.isReady()`，还不是 JSS 的完整候选资格。

Android 11 r48 把完整判定拆成多道门：

```text
JobStatus 约束 ready
&& Job 仍注册
&& scheduling user 与 source user 都 started
&& source UID 不在全量 backup
&& 没有 JobRestriction
&& Job 尚未 pending/active
&& JobService 组件存在且应用不在 bad-process 状态
```

全部通过后，Job 只是进入 `mPendingJobs`；真正运行还要由 `JobConcurrencyManager` 分配槽位。UID 进程状态会形成另一份 priority override，影响 restriction 豁免和槽位竞争，却不会直接翻转 Job constraint。

读完本章，你应当能解释三种不同的“pending/ready”用词，逐项定位完整门失败的位置，并分清 UID priority override 与 shell constraint override。本文不重讲每个 Controller 如何更新具体 constraint，也不展开 JobService 的绑定与回调状态机。

## 1. 先分清四个容器和三种“ready”

JSS 不是用一个枚举表示 Job 生命周期，而是让同一 `JobStatus` 同时存在于多个集合维度：

| 状态容器 | 含义 |
|---|---|
| `mJobs` | registered Job 的权威内存表 |
| `mPendingJobs` | 已通过完整门、等待执行槽的候选 |
| `mActiveServices` | 固定 `JobServiceContext` 槽；非空槽持有 active Job |
| `mUidPriorityOverride` | source UID 当前进程重要性映射出的调度 priority |

周期 Job 运行时仍 registered，pending Job 也仍在 `mJobs`；这些不是互斥状态。

“ready”至少有三种语义：

1. `JobStatus.isReady()`：Job 自身约束层通过；
2. `isReadyToBeExecutedLocked()`：当前是否应当新加入 pending queue；已经 pending/active 时反而返回 false；
3. 通过完整门后仍可能因非 ACTIVE batching 没被放进 pending，或进了 pending 仍在等槽。

先确认讨论的是哪一层，才能避免把正常状态当成矛盾。

## 2. `JobStatus.isReady()` 只处理约束层

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java`

`isReady()` 先处理 quota/dynamic、NEVER bucket、Doze 与后台限制，再决定 deadline 或普通 required constraints 是否放行：

```java
if ((!mReadyWithinQuota && !mReadyDynamicSatisfied)
        || getEffectiveStandbyBucket() == NEVER_INDEX) {
    return false;
}
return mReadyNotDozing && mReadyNotRestrictedInBg
        && (mReadyDeadlineSatisfied
                || isConstraintsSatisfied(satisfiedConstraints));
```

这几行带来三个边界：

- within-quota 与 dynamic-satisfied 至少一个为真，NEVER bucket 无条件拒绝；
- 一次性 deadline 可以替代普通显式 constraints，却不能绕过 quota/dynamic、NEVER、Doze 或后台限制；
- periodic 的 latest time 是实现窗口，不会按一次性 override deadline 的方式无视普通 constraints。

这一层不查询用户是否 started、是否正在 backup、组件是否存在，也不看执行槽。因此 `job.isReady()==true` 只能证明第一道门。

## 3. 完整门怎样把约束结果变成 pending 候选

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

方法：`isReadyToBeExecutedLocked()`。前半段先验证 Job 自身和系统状态：

```java
final boolean jobReady = job.isReady();
if (!jobReady) {
    return false;
}

final boolean jobExists = mJobs.containsJob(job);
final boolean userStarted = areUsersStartedLocked(job);
final boolean backingUp = mBackingUpUids.indexOfKey(job.getSourceUid()) >= 0;

if (!jobExists || !userStarted || backingUp) {
    return false;
}
if (checkIfRestricted(job) != null) {
    return false;
}
```

后半段避免重复入队/重复执行，并把昂贵的组件查询放到最后：

```java
final boolean jobPending = mPendingJobs.contains(job);
final boolean jobActive = isCurrentlyActiveLocked(job);
if (jobPending || jobActive) {
    return false;
}

return isComponentUsable(job);
```

方法名应读成“现在是否适合把这个 Job 新放入执行候选”，而不是“这个 Job 在抽象意义上是否处于 ready 状态”。这就是 pending/active Job 在 dump 中再次调用它时可能显示 false 的原因。

## 4. 两个 user 都必须 started，但不要求在这里直接 unlocked

代包调度可能同时涉及 scheduling/JobService 一侧的 user 与真实 source 包的 user。JSS 要求两边都已 started：

```java
private boolean areUsersStartedLocked(final JobStatus job) {
    boolean sourceStarted =
            ArrayUtils.contains(mStartedUsers, job.getSourceUserId());
    if (job.getUserId() == job.getSourceUserId()) {
        return sourceStarted;
    }
    return sourceStarted
            && ArrayUtils.contains(mStartedUsers, job.getUserId());
}
```

普通 App 两个 userId 通常相同；`scheduleAsPackage()` 场景可能不同。只启动一边时，即使 deadline、shell FULL 或普通 constraints 都通过，完整门仍拒绝。

`onStartUser()` 把 userId 加入 `mStartedUsers` 后 post `MSG_CHECK_JOB`；`onUnlockUser()` 不修改该数组，只主动要求重扫。因此这里的布尔门是 started，不是 CE 已 unlocked。解锁仍可能令组件或 Controller 条件变化，所以 JSS 选择再检查一次。

r48 的 `onStopUser()` 只从数组移除 userId，没有在这个方法里 post 检查消息。用户停止过程中还有其他系统清理/取消链，静态阅读这一个回调却不能证明它自身会立即清空 pending 并停止所有 active Job；最晚收敛还依赖后续调度检查。

## 5. Backup gate 按 source UID 拦，但恢复检查存在身份不对称

`mBackingUpUids` 实际被当作 int set 使用。完整门检查的是：

```java
mBackingUpUids.indexOfKey(job.getSourceUid()) >= 0
```

这是正确的归因轴：system UID 代包调度时，真正可能修改被备份数据的是 source 包。

`addBackingUpUid()` 只把 UID 放入 map，不 post 重扫，也不主动遍历 active context。源码注释假设 full backup 会由 ActivityManager 杀掉应用进程，使运行 Job 退出；这是一项跨服务前提，不是本方法完成的动作。

移除 backup 状态时才有条件触发恢复扫描：

```java
mBackingUpUids.delete(uid);
if (mJobs.countJobsForUid(uid) > 0) {
    mHandler.obtainMessage(MSG_CHECK_JOB).sendToTarget();
}
```

需要继续追到 `frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java` 的 `JobStore.JobSet.countJobsForUid()`：它按 scheduling UID 的 `mJobs` 索引查找，而且只统计 `job.getUid()==job.getSourceUid()` 的自调度 Job。于是某 source UID 如果只有别人代它 schedule 的 Job，backup gate 虽按 source UID 命中，`removeBackingUpUid(sourceUid)` 却可能计数为 0，不发送这次即时重扫。其他状态变化仍可让系统后来收敛；这是 r48 恢复触发的身份维度不对称，不能扩大成“Job 永远不再运行”。`clearAllBackingUpUids()` 只要 map 原来非空就无条件 post，没有这个逐 UID 计数条件。

Backup gate 不删除 Job，也不改 jobs.xml。它只影响当前是否能成为执行候选。

## 6. Restriction、重复状态和组件检查各负责什么

第 137 章的 `checkIfRestricted()` 位于 user/backup 之后、pending/active 之前。r48 中它会用 evaluated priority 先做统一豁免，再判断 SEVERE+ 的 required-network Job。Restriction 不改变 `JobStatus.isReady()`，所以约束 ready 与 overall ready 可以不同。

`mPendingJobs.contains(job)` 防止同一 `JobStatus` 引用重复入队；active 检查遍历 `mActiveServices`，用 `JobStatus.matches(uid, jobId)` 比较 scheduling UID + jobId。Job ID 的公开契约本来就是同一 UID 下唯一，source 身份不是这里的实例主键。

最后才执行组件检查，因为 PackageManager/ActivityManager 查询比前面的内存条件昂贵。下面省略 `RemoteException` 转换和 DEBUG 日志，只保留两个实际判断：

```java
service = AppGlobals.getPackageManager().getServiceInfo(
        job.getServiceComponent(),
        PackageManager.MATCH_DEBUG_TRIAGED_MISSING,
        job.getUserId());
if (service == null) {
    return false;
}
final boolean appIsBad =
        mActivityManagerInternal.isAppBad(service.applicationInfo);
return !appIsBad;
```

`ServiceInfo` 存在只证明组件可解析，`!isAppBad` 只排除 AMS 当前判定的 bad process；它们不证明应用进程已存在，也不证明后续 `bindServiceAsUser()` 一定成功。完整门通过后仍可能在绑定阶段失败。

## 7. 通过完整门只代表可以排队，不代表立即执行

新 schedule 的 Job 若立刻通过完整门，JSS 会 `notePending()`、按 comparator 插入 `mPendingJobs`，再调用 `maybeRunPendingJobsLocked()` 尝试分槽。`schedule()` 返回成功只证明请求被接受，并不承诺应用的 `onStartJob()` 已执行。

状态变化触发全量/条件扫描时，JSS 会先为旧 pending 全部 `noteNonpending()` 并清空，再从 `mJobs` 重建候选。这保证取消、用户、backup、restriction 和约束变化不会留下陈旧队列，也维持第 136 章 PackageTracker nesting 成对。

当 `mReportedActive` 为 true 时，`MSG_CHECK_JOB` 走 `queueReadyJobsForExecutionLocked()`；较空闲时走 `maybeQueueReadyJobsForExecutionLocked()`，后者还会按 standby bucket、失败次数、数量和最大 batching 延迟决定是否真的入队。因此一个 Job 通过完整门但暂未出现在 pending，在非 ACTIVE batching 下仍可能是合法状态。

pending queue 的 r48 comparator 也不是 evaluated priority 排序器：

```java
if (o1.overrideState != o2.overrideState) {
    return o2.overrideState - o1.overrideState;
}
if (o1.enqueueTime < o2.enqueueTime) {
    return -1;
}
return o1.enqueueTime > o2.enqueueTime ? 1 : 0;
```

它先比较 shell `overrideState`，再比较 `enqueueTime`。第 136 章的 evaluated priority 主要用于 restriction 豁免、同 scheduling UID 抢占和诊断，不能说成会全局重排 pending。

## 8. UID priority override 只改变调度优先级

AMS 的 UID observer 把 proc state 消息送到 JSS Handler；`updateUidState()` 将 source UID 的当前重要性投影为三档：

```java
if (procState == ActivityManager.PROCESS_STATE_TOP) {
    mUidPriorityOverride.put(uid, JobInfo.PRIORITY_TOP_APP);
} else if (procState <= ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE) {
    mUidPriorityOverride.put(uid, JobInfo.PRIORITY_FOREGROUND_SERVICE);
} else if (procState <= ActivityManager.PROCESS_STATE_BOUND_FOREGROUND_SERVICE) {
    mUidPriorityOverride.put(uid, JobInfo.PRIORITY_BOUND_FOREGROUND_SERVICE);
} else {
    mUidPriorityOverride.delete(uid);
}
```

只有“恰好 TOP”得到 40；更重要但并非 TOP 的 persistent 进程会落入后面的 `<= FOREGROUND_SERVICE` 分支，得到 35，而不是自动当 TOP。JSS 用 `job.getSourceUid()` 查这张表，代包 Job 随真实 source 的前台状态获得优先级。

`evaluateJobPriorityLocked()` 再把原始 priority、UID override 与 PackageTracker load adjustment 合成当前结果。`frameworks/base/apex/jobscheduler/framework/java/android/app/job/JobInfo.java` 中的 `JobInfo.Builder.setPriority()` 在 r48 标记为 `@hide`，普通 SDK 应用不能把内部 priority 数值当作公开可调旋钮。

UID priority override 不修改任何 satisfied constraint，也不写 jobs.xml。`MSG_UID_STATE_CHANGED` 更新 map 后，Handler 末尾只调用 `maybeRunPendingJobsLocked()` 重新分配已有 pending；它本身不调用 full/maybe queue 扫描。因此一个此前因 restriction 被排除、已不在 pending 的 Job，不能仅凭这条 UID 消息就保证立刻重新入队，还要等待后续触发完整扫描。

## 9. Shell override 是另一套状态，不要与 UID priority 混用

`JobStatus.overrideState` 服务于 `cmd jobscheduler run` 调试，和 `mUidPriorityOverride` 没有同一种语义：

| 命令模式 | 写入状态 | 对 r48 的直接影响 |
|---|---|---|
| 默认 `run` | `OVERRIDE_SOFT` | 假装充电、电量、存储、timing delay、idle 等 soft constraints 满足 |
| `run --force` | `OVERRIDE_FULL` | `isConstraintsSatisfied()` 直接为 true |
| `run --satisfied` | `OVERRIDE_SORTING` | 不伪造 constraint，只让 comparator 将它排在普通 Job 前；命令会先要求普通 required constraints 已满足 |

即使 FULL 让 `isConstraintsSatisfied()` 为 true，`JobStatus.isReady()` 外层仍检查 quota/dynamic、NEVER、Doze 与后台限制；完整门还会检查 users、backup、restriction、重复状态和组件。`--force` 不是“绕过 JSS 所有政策”。

命令成功后，r48 没有在 `executeRunCommand()` 尾部立即把当前 `JobStatus.overrideState` 清回 NONE；不能把 override 想成只在一个函数栈里临时生效。它属于 shell 调试接口，不是应用公开 API，也不应成为产品业务逻辑。

## 10. 三个诊断输出为什么会给出不同答案

| 入口 | `pending/ready` 实际含义 |
|---|---|
| `JobScheduler.getAllPendingJobs()` | 按公开文档返回调用应用所有已 schedule Job，包含正在运行与仍等待者；不是内部 `mPendingJobs` |
| `cmd jobscheduler get-job-state` 的 `ready` | 直接检查 `js.isReady()`；不包含 restriction 与 bad-app 判断 |
| `dumpsys jobscheduler` registered Job 的 `Ready:` | 调 `isReadyToBeExecutedLocked()`；已 pending/active 时也会为 false，并在括号中分解各门 |

公开方法的契约位于 `frameworks/base/apex/jobscheduler/framework/java/android/app/job/JobScheduler.java`。`get-job-state` 会另外打印 pending、active、user-stopped、source-user-stopped、backing-up、no-component，但不会报告 `checkIfRestricted()` 或 AMS bad-app 结果。把它的 `ready` 当作 overall ready，就会漏掉本章最关键的外层门。

诊断时建议固定记录六项：

```text
job.isReady
user/source-user started
backing-up
restricted
pending/active
component usable + evaluated priority
```

只看一个布尔值不足以还原决策。`enqueueTime` 是任务排队排序时间，`madePending` 是 PackageTracker 开始统计 pending 的 uptime，两者也不是同一字段。

## 11. 启动与版本边界

JSS 的系统级总开关是 `mReadyToRock`。在 `PHASE_THIRD_PARTY_APPS_CAN_START` 之前，persisted Job 可以已恢复进 `JobStore`，但 Handler 收到检查消息会直接 return；进入该 phase 后才创建执行 contexts、把 Job 交给 Controllers 跟踪，并主动发起首次检查。

本文结论限定为 Android 11 `android-11.0.0_r48`：

- `isReadyToBeExecutedLocked()`、两类 override、用户停止触发和 backup 恢复细节都是 Framework 实现，不是 SDK 契约；
- App 可公开调用 schedule/cancel/getAllPendingJobs，却不能读取 `mPendingJobs` 或控制 UID priority override；
- shell run/get-job-state 需要调试权限，只应作为诊断入口；
- 当前源码静态链能证明判定和消息顺序，不能证明特定设备上的应用进程一定何时启动。

## 12. 从源码复核一次完整门

在 Android 11 r48 源码根目录依次执行：

1. 先看 constraint 层：

   ```bash
   sed -n '1205,1330p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
   ```

2. 再看完整候选门与组件检查：

   ```bash
   sed -n '2210,2340p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 核对 started user、backup 与 UID priority 映射：

   ```bash
   sed -n '970,1005p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '1280,1305p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '2400,2445p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

4. 对照 shell override 与 comparator：

   ```bash
   sed -n '1288,1330p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
   sed -n '775,800p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '2860,2910p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

5. 最后看诊断命令如何定义 `ready`：

   ```bash
   sed -n '3010,3095p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

把每次 false 写到“constraint / user / backup / restriction / duplicate / component / batching / concurrency”之一；如果无法归类，说明调用链还没有追完。

## 13. 练习与参考答案

### 练习一：解释两个 ready

某 Job 的 `get-job-state` 打印 `ready`，但它的 source user 尚未 started。它能进入内部 pending queue 吗？

参考答案：不能。命令的 ready 只来自 `JobStatus.isReady()`；完整门还要求 source user 与 scheduling user 都在 `mStartedUsers`。

### 练习二：判断 FULL override 的边界

`cmd jobscheduler run --force` 已让普通 constraints 满足，但 source UID 正在 backup，设备也有命中的 thermal restriction。FULL 能否直接执行？

参考答案：不能。FULL 只影响 `isConstraintsSatisfied()`；backup 与 restriction 位于 `JobStatus.isReady()` 之后的完整门，仍会拒绝。

### 练习三：手算 UID priority

source UID 的 proc state 分别是 TOP、FOREGROUND_SERVICE、BOUND_FOREGROUND_SERVICE、CACHED_EMPTY 时，map 中的值是什么？

参考答案：依次为 40、35、30、删除映射。若 PackageTracker factor 达到 moderate/heavy，普通非 TOP 值随后还可能减 40/80；TOP 40 不做这次负调整。

### 练习四：找出 backup 恢复的不对称

system UID 只代 source UID X 调度了一个 Job；X 自己没有 schedule Job。`removeBackingUpUid(X)` 为什么可能不发 `MSG_CHECK_JOB`？

参考答案：gate 按 `sourceUid=X` 查询，但 `countJobsForUid(X)` 只在 scheduling UID 索引中统计 `getUid()==getSourceUid()` 的自调度 Job，此例返回 0。它只说明本次即时唤醒可能缺失，不代表以后永不重扫。

### 练习五：判断三个 pending

公开 `getAllPendingJobs()` 返回某 Job、`get-job-state` 没打印 pending、dumpsys Pending queue 也没有它，是否矛盾？

参考答案：不矛盾。公开方法名中的 pending 指“仍由系统持有的已 schedule Job”，包括运行中与等待中的 Job；后两个位置的 pending 才对应内部 `mPendingJobs` 候选集合。

## 本章带走什么

`JobStatus.isReady()` 只回答约束层，JSS 还要验证 Job 代际、两个 started user、source backup、restriction、重复 pending/active 和组件可用性；通过后也只是获得排队资格。UID proc state 只修正 evaluated priority，shell override 只按各自档位伪造部分 constraint 或排序优先，二者都不是万能通行证。遇到“ready 却没运行”，先统一术语，再沿完整门逐项排除，最后才看 batching 与并发槽。
