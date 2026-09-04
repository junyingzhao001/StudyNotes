# 139 Android JSS：schedule API 配额、权限与 Replacement

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 121、133、135、138 章

## 先看结论：同一个 jobId 再次 schedule，旧任务会怎样

应用为了“刷新上传条件”再次调用 `schedule()`，传入与旧任务相同的 jobId。常见误判有两个：一是把它当作无操作，二是看到 `RESULT_SUCCESS` 就认为旧任务已平滑更新、磁盘也已提交。Android 11 r48 的真实行为更严格：**普通 `schedule()` 会创建新的 `JobStatus`，撤销旧 Job 的顶层 ClipData 授权、移交 WorkItem，再替换 JobStore/Controller 状态；旧 Job 若正在运行，还会进入停止流程。**

只有 `enqueue()` 在“同 scheduling UID + 同 jobId + `JobInfo.equals()`”时走追加 work 的快路径，不替换 `JobStatus`。此外，请求在 replacement 前还要经过组件、权限、调用频率、后台启动模式、Job 数量和 URI grant 检查。

读完本章，你应当能回答：失败发生时旧 Job 是否仍在；第 250/251 次 persisted schedule 默认会怎样；100 Job 上限为何实际能出现第 101 个；`RESULT_SUCCESS` 到底承诺到哪个完成点。

本文不重讲 WorkItem 的领取/确认协议，也不重讲 jobs.xml 的 AtomicFile 细节，只追 schedule/enqueue 到内存替换与异步持久化交接。

## 1. 三个入口先分开

| 入口 | 面向谁 | persisted | 归因 |
|---|---|---|---|
| `schedule(JobInfo)` | 普通应用 | 允许，但需 `RECEIVE_BOOT_COMPLETED` | 调用应用自己 |
| `enqueue(JobInfo, JobWorkItem)` | 普通应用追加工作 | r48 明确禁止 persisted | 调用应用自己 |
| `scheduleAsPackage(...)` | 持 `UPDATE_DEVICE_STATS` 的系统组件 | 由内部条件处理 | 可指定 source package/user/tag |

公开客户端 `frameworks/base/apex/jobscheduler/framework/java/android/app/JobSchedulerImpl.java` 很薄：同步 Binder `RemoteException` 被转成 `RESULT_FAILURE`，服务端抛出的参数、安全或状态异常则沿 Binder 异常协议回到调用方。应用既要检查返回值，也不能忽略 Java 异常。

Binder Stub 在清除 calling identity 之前完成调用方验证，之后才以内核记录的 uid/pid/userId 参数进入内部方法，并在 `finally` 恢复身份。`clearCallingIdentity()` 的作用是避免后续 system_server 调用错误继承 App 身份，不是绕过权限。

## 2. 普通 schedule 的安全检查按什么顺序发生

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

`JobSchedulerStub.schedule()` 的顺序是：

```text
Binder.getCallingPid/Uid
→ enforceValidJobRequest(uid, job)
→ persisted 时 canPersistJobs(pid, uid)
→ validateJobFlags(job, uid)
→ clearCallingIdentity
→ 内部 scheduleAsPackage(job, null, uid, null, userId, null)
→ finally restoreCallingIdentity
```

`enforceValidJobRequest()` 通过 PackageManager 证明三件事：JobService 真实存在；它的 `ApplicationInfo.uid` 等于 calling UID（shared UID 也因此可用）；Service 必须声明 `android.permission.BIND_JOB_SERVICE`。最后一项是保护 JobService 不被普通 bind 的反向权限，不是应用要申请的常规 schedule 权限。

persisted Job 重启后仍可恢复，所以 r48 要求调用 UID 持有 `RECEIVE_BOOT_COMPLETED`；失败抛的是 `IllegalArgumentException`。结果缓存于 `mPersistCache`，键只有 UID，且本文件没有包/权限变化的失效路径，不能假设 system_server 生命周期内每次都会重新查权限。

隐藏 flag 还有各自权限门：`FLAG_WILL_BE_FOREGROUND` 要求 `CONNECTIVITY_INTERNAL`；`FLAG_EXEMPT_FROM_APP_STANDBY` 直接要求 system UID。普通应用不能靠 flag 自行穿透后台政策。

`enqueue()` 复用 Service 与 flag 验证，但在进入内部方法前拒绝 persisted Job 和 null work。`scheduleAsPackage()` 则要求非空 source package 与 `UPDATE_DEVICE_STATS`，不会复用普通入口的 Service 所属 UID 验证；这是受信任系统入口，不应按普通 App 安全边界理解。

## 3. API 调用配额只管“自调度 persisted schedule”

内部 `scheduleAsPackage()` 最先处理 CountQuotaTracker。下面分别摘录入口条件与分支后的记账行；两段之间的超额处理将在正文解释：

```java
        final String servicePkg = job.getService().getPackageName();
        if (job.isPersisted() && (packageName == null || packageName.equals(servicePkg))) {
            // Only limit schedule calls for persisted jobs scheduled by the app itself.
            final String pkg =
                    packageName == null ? job.getService().getPackageName() : packageName;
            if (!mQuotaTracker.isWithinQuota(userId, pkg,
                    QUOTA_TRACKER_SCHEDULE_PERSISTED_TAG)) {
```

超额分支结束后，请求若没有抛异常或提前返回，就执行原方法中的记账行：

```java
            mQuotaTracker.noteEvent(userId, pkg, QUOTA_TRACKER_SCHEDULE_PERSISTED_TAG);
        }
```

这两个代码块都是 r48 原文的连续行，只省略了中间分支。non-persisted 请求不计；`enqueue()` 已禁止 persisted；代其他 package 的 persisted Job 只有 source package 与 service package 相同时才进入这只计数器。它与 Job 执行时长 quota 完全不是一回事。

默认 limit 是滚动 60 秒内 250 次，CountQuotaTracker 用 elapsed realtime 记录事件，并以 `countInWindow < countLimit` 判为仍在额度内。JSS 是“先查、后记”，所以边界为：

```text
第 250 次：调用前 count=249，检查通过；note 后变 250，请求继续
第 251 次：调用前 count=250，进入超额分支
```

超额分支一定会请求 AppStandby 将该包标记为 forced-system buggy，并用另一只 1 次/分钟的 quota 限制 wtf 日志。默认 `API_QUOTA_SCHEDULE_THROW_EXCEPTION=true`，但只对 debuggable app 抛 `LimitExceededException`；默认 `API_QUOTA_SCHEDULE_RETURN_FAILURE_RESULT=false`，所以非 debuggable app 在触发副作用后仍会继续 schedule 并记下一次事件。不能把“250/分钟”简单写成“第 251 次统一返回失败”。

### r48 的 debuggable 缓存键缺口

普通入口把内部 `packageName` 参数传为 null。超额分支却用这个参数查询/写入 `mDebuggableApps`，实际查询 ApplicationInfo 时又使用正确的 `pkg`：

```java
if (!mDebuggableApps.containsKey(packageName)) {
    final ApplicationInfo appInfo = AppGlobals.getPackageManager()
            .getApplicationInfo(pkg, 0, userId);
    mDebuggableApps.put(packageName,
            (appInfo.flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0);
}
isDebuggable = mDebuggableApps.get(packageName);
```

于是普通 schedule 会共享 null key：第一个触发查询的包可影响之后其他普通包的 debuggable 判定。包移除清理也无法按真实包名命中这个 null 项。这是 Android 11 r48 实现缺口，不是应用可依赖的行为。

## 4. start-mode 与 100 Job 上限位于 replacement 之前

配额处理之后，JSS 询问 AMS 的 `isAppStartModeDisabled()`；为 true 时返回 `RESULT_FAILURE`，还没有碰旧 Job。接口 `RemoteException` 被忽略，是因为 AMS 与 JSS 同在 system_server；这不等于业务上的 disabled 被忽略。

进入 `mLock` 后先按 `(scheduling UID, jobId)` 找 `toCancel`，再处理 Job 数量：

```java
if (ENFORCE_MAX_JOBS && packageName == null) {
    if (mJobs.countJobsForUid(uId) > MAX_JOBS_PER_APP) {
        throw new IllegalStateException("Apps may not schedule more than "
                + MAX_JOBS_PER_APP + " distinct jobs");
    }
}
```

`MAX_JOBS_PER_APP=100`，但比较是 `> 100`。当已有 100 个自调度 Job 时，第 101 个仍可加入；到已有 101 个时，下次普通 schedule 才抛异常。检查没有排除 replacement，因此处于 101 个的边界状态时，普通 `schedule()` 即使只想替换现有 jobId 也会先失败。

异常发生在新 `JobStatus.prepareLocked()` 与旧 Job 取消之前，所以旧 Job 不受伤害。`scheduleAsPackage` 的 `packageName != null` 跳过这项普通 App 上限。

## 5. 为什么 enqueue 有快路径，而 schedule 一定替换

找到旧 Job 后，只有 `work != null && old.getJob().equals(newJobInfo)` 才直接追加：

```java
if (work != null && toCancel != null) {
    if (toCancel.getJob().equals(job)) {
        toCancel.enqueueWorkLocked(work);
        toCancel.maybeAddForegroundExemption(mIsUidActivePredicate);
        return JobScheduler.RESULT_SUCCESS;
    }
}
```

`enqueueWorkLocked()` 会分配递增 workId；Intent 带 URI grant flags 时立即建立 `GrantedUriPermissions`，再把 work 放入 pendingWork。快路径保留原 `JobStatus`、enqueueTime、Controller 跟踪和正在运行状态，也绕过后面的 100 Job 检查。

普通 `schedule()` 的 work 永远为 null，因此没有同样的 no-op/快路径；即使新旧 `JobInfo.equals()`，也继续创建新 `JobStatus` 并 replacement。`enqueue()` 若 JobInfo 不相等，也走慢路径 replacement，正在运行的旧实例会被请求停止。

所以应用若只是不断投递同一规格的小工作，应理解 `enqueue()` 的队列语义；反复 `schedule()` 不是廉价的“刷新提示”。但具体选型还要考虑 WorkItem 不持久化等公开 API 限制，不能只为避开替换而机械改接口。

## 6. replacement 为什么先 prepare 新对象，再撤销旧对象

慢路径先创建新 `JobStatus`，根据 source UID 当前活跃状态补前台豁免，然后执行：

```java
jobStatus.prepareLocked();

if (toCancel != null) {
    cancelJobImplLocked(toCancel, jobStatus, "job rescheduled by app");
} else {
    startTrackingJobLocked(jobStatus, null);
}

if (work != null) {
    jobStatus.enqueueWorkLocked(work);
}
```

`prepareLocked()` 为新 JobInfo 的 ClipData 建立顶层 URI grant，可能抛 `SecurityException`。把它放在旧 Job 取消之前，意味着新授权失败时旧任务仍留在原状态。

replacement 进入 `cancelJobImplLocked(old, new, ...)` 后的关键顺序是：

1. `old.unprepareLocked()` 撤销旧顶层 ClipData grant；
2. `old.stopTrackingJobLocked(new)` 把 executingWork 放到新 pendingWork 前部，再接旧 pendingWork，保留 next workId；WorkItem 自己持有的 URI grant 随对象迁移，不在 replacement 时撤销；
3. 从 JobStore/Controllers 移除旧对象；
4. 从 `mPendingJobs` 移除旧候选并闭合 PackageTracker pending；
5. 若旧对象 active，向 `JobServiceContext` 发取消；
6. `startTrackingJobLocked(new, old)` 把新对象加入 JobStore/Controllers；
7. `enqueue()` 本次新 work 最后追加。

因此它既不是原对象就地修改，也不是“先删旧、再尝试准备新”的危险顺序。旧 active 的 stop 与新 Job 重新过 ready 门可以在时间上重叠；JobServiceContext 的 cleanup 仍是另一个完成点。

## 7. 内存替换与 jobs.xml 提交不是一个事务

`startTrackingJobLocked()` 会重置新对象的 `enqueueTime`，然后 `mJobs.add(jobStatus)`；Controller 收到 old/new 交接。新对象若已经通过第 138 章的完整门，可立刻进入 pending 并尝试分槽，否则等待后续状态变化。

JobStore 的 `add()` 对 persisted Job 只调用 `maybeWriteStatusToDiskAsync()`；旧 persisted Job 的 remove 也只是安排异步写。AtomicFile 保护的是稍后的单次文件发布，不会把下面这些动作合并成跨层事务：

```text
URI grant 切换
WorkItem 内存迁移
旧 active stop
JobStore/Controller 替换
jobs.xml 异步落盘
应用回调
```

如果 persisted→non-persisted，旧记录移除会安排写盘；non-persisted→persisted，新记录加入会安排写盘；两边都是 persisted 时可能合并为后续快照。`RESULT_SUCCESS` 返回时，内存注册/替换已经完成，立即 ready 的 Job 也已尝试入队和分槽，但它不承诺 jobs.xml 已耐久提交、旧 active 已 cleanup，或新 `onStartJob()` 已执行。

客户端若收到 `RESULT_FAILURE` 是因为 Binder `RemoteException`，还存在经典的不确定提交窗口：服务端可能已完成内存变更，只是 reply 没送回。使用稳定 jobId 的幂等替换有助于恢复查询，但不能把通信失败等同为服务端一定没做事。

## 8. 失败点决定旧 Job 是否受影响

| 失败点 | 典型表现 | 旧 Job |
|---|---|---|
| Service/UID/BIND_JOB_SERVICE 校验 | `IllegalArgumentException` | 未进入内部 replacement，不变 |
| persisted 权限或 enqueue 参数 | 参数异常 | 不变 |
| 隐藏 flag 权限 | `SecurityException` | 不变 |
| debug app 调用配额超额 | `LimitExceededException`，并已触发 buggy 标记 | 不变 |
| start mode disabled | `RESULT_FAILURE` | 不变 |
| 101 边界后的 max-job 检查 | `IllegalStateException` | 不变 |
| 新 ClipData `prepareLocked()` | `SecurityException` | 不变 |
| 进入 `cancelJobImplLocked()` 之后 | 旧授权/队列/运行态开始切换 | replacement 已产生可见副作用 |
| Binder reply 丢失 | 客户端得到 `RESULT_FAILURE` | 服务端是否已替换不确定，需查询 |

配额超额的非 debug 默认分支尤其特殊：它会产生“标记 buggy”副作用，却仍可能继续并最终返回 success。返回码不能概括所有政策副作用。

## 9. API、内部实现与版本边界

- `JobScheduler.schedule/enqueue/getAllPendingJobs`、`JobInfo.Builder` 的常规约束配置是公开 API；`scheduleAsPackage`、内部 priority/flag、JobStore 与 URI grant 迁移属于 Framework 实现。
- `schedule()` 返回 `RESULT_SUCCESS` 的公开意义是调度请求被接受，不是开始/完成/持久化 ACK。
- r48 的 persisted API quota、debug-only exception、null cache key 和 `>100` 比较都可能在后续版本变化，不能作为跨版本业务规则。
- `mPersistCache` 与 `mDebuggableApps` 的缓存缺口是源码观察；真实影响还取决于包权限变化、UID 分配与请求顺序。
- 本章没有设备数据，不能声称 250 次调用造成多少功耗或具体多久后一定解除 AppStandby 限制。

## 10. 从源码验证一次 replacement

在 Android 11 r48 源码根目录按这个顺序只读：

1. 比较三个 Binder 入口、权限检查和 identity 边界：

   ```bash
   sed -n '2550,2720p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 读内部配额、start mode、快路径、上限和慢路径：

   ```bash
   sed -n '1005,1165p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 对照 CountQuotaTracker 的严格小于和 noteEvent：

   ```bash
   sed -n '175,220p' frameworks/base/services/core/java/com/android/server/utils/quota/CountQuotaTracker.java
   sed -n '355,370p' frameworks/base/services/core/java/com/android/server/utils/quota/CountQuotaTracker.java
   ```

4. 追 URI grant 与 WorkItem 移交：

   ```bash
   sed -n '590,725p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
   sed -n '1260,1290p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

5. 最后回到 JobStore，确认 add/remove 只安排异步写：

   ```bash
   sed -n '190,270p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
   ```

每个可能的 return/throw 都记录“是否已 note quota、是否已 restrictApp、是否已 prepare 新授权、是否已 cancel 旧对象”，就能判断失败是否仍是无副作用。

## 11. 练习与参考答案

### 练习一：手算配额边界

一个 debuggable App 在滚动 60 秒内连续调用 persisted `schedule()`，其他检查都通过。默认设置下第 250 与第 251 次怎样？

参考答案：第 250 次在调用前仍是 249，先通过再记成 250；第 251 次调用前已不在 quota，先触发日志限频与 buggy restriction，再因 debug app 抛 `LimitExceededException`，旧 Job 尚未 replacement。

### 练习二：手算 Job 数边界

普通 UID 已有 100 个自调度 Job，再 schedule 一个全新 jobId；随后已有 101 个时只替换其中一个旧 jobId。结果分别是什么？

参考答案：第 101 个仍通过，因为条件是 `count > 100`；已有 101 个时连 replacement 也先抛 `IllegalStateException`。若是 `enqueue()` 且 JobInfo 完全相等，可在上限检查之前走快路径。

### 练习三：判断旧 Job 是否安全

replacement 的新 JobInfo 含无权访问的 ClipData URI，`prepareLocked()` 抛 `SecurityException`。旧 Job 的授权、pending 或 active 状态是否已被撤销？

参考答案：没有。新对象先 prepare，成功后才调用 `cancelJobImplLocked(old,new)`；异常发生时旧对象尚未进入取消链。

### 练习四：还原 WorkItem 顺序

旧 Job 有 executing work A、B 和 pending work C，本次慢路径 enqueue 新 work D。新 Job 的 pendingWork 顺序是什么？

参考答案：A、B、C、D。replacement 先把 executing 列表作为新 pending 前部，再追加旧 pending，最后才 enqueue 本次 work；对应 WorkItem grant 随原对象迁移。

### 练习五：解释 success 的边界

`schedule()` 返回 `RESULT_SUCCESS`，能否证明 jobs.xml 已写完、旧 Job 已停止、新 Job 已执行 `onStartJob()`？

参考答案：三项都不能。success 证明服务端已接受并完成内存注册/替换流程；持久化是异步快照，旧 active cleanup 与新应用回调各有自己的完成协议。

## 本章带走什么

Android 11 的 `schedule()` 是一条“先验权与限流、再准备新资源、最后替换旧代际”的状态转换，不是简单向队列追加。相同 jobId 的普通 schedule 仍会 replacement；只有相同 JobInfo 的 enqueue 能追加 WorkItem 而保留原实例。判断一次失败是否安全，关键是看它发生在 `prepareLocked()`、`cancelJobImplLocked()` 还是 Binder reply 之后；判断一次 success 保证了什么，则必须把内存注册、pending/active、应用回调与 jobs.xml 耐久提交分开。
