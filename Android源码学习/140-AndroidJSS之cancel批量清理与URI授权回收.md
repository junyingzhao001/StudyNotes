# 140 Android JSS：cancel、批量清理与 URI 授权回收

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 133、135、138、139 章

## 先看问题：卸载一个包，为什么不等于删除这个 UID 的全部 Job

一个 Job 同时有两套身份：`JobStatus.getUid()` 表示 scheduling/calling UID，`getSourceUid()` 与 `getSourcePackageName()` 表示实际归因来源。普通 App 自己 schedule 时两套身份通常重合；系统通过 `scheduleAsPackage()` 代调度时则可能分离。

这使“取消”至少包含两个问题：先按哪套身份选中 Job，再怎样回收它散布在 JobStore、Controller、pending、active、jobs.xml 和 URI grant 中的状态。选错集合会漏删或多删；只从 JobStore 移除又不等于完整取消。

本章围绕一个主问题展开：**App 主动取消、包禁用/卸载/force-stop、用户删除和 UID disabled 分别会选中哪些 Job，选中后两类 URI 授权与运行态又在何时真正结束？**

## 1. 先把五种选择器放在一张表里

| 入口 | 第一层选择键 | 第二层过滤 | 特殊保护 |
|---|---|---|---|
| App `cancel(jobId)` | scheduling UID + jobId | 无 | 无批量保护 |
| App `cancelAll()` | scheduling UID | 无 | 拒绝整批取消 system UID |
| package cleanup | scheduling UID = 广播携带的 package UID | source package 相等 | 拒绝包名 `android` |
| user removed | source UID 所属 user | 无 | 无 |
| UID gone/idle 且 disabled | scheduling UID | 无 | 拒绝整批取消 system UID |

这里最容易混淆的是 `cancelJobsForPackageAndUid(pkg, uid)`：方法名看似“双条件全局查询”，实现却先从 `mJobs.getJobsByUid(uid)` 取 scheduling UID 桶，再过滤 source package。它没有从 `mJobsPerSourceUid` 反向找 source package。

所有批量查询拿到的都是新 `ArrayList` 快照，所以随后持锁逐项删除底层集合不会破坏迭代。

## 2. 公共 cancel 是同步 Binder 调用，但没有语义回执

客户端 `frameworks/base/apex/jobscheduler/framework/java/android/app/JobSchedulerImpl.java` 的 `cancel()` 返回 `void`，并吞掉 `RemoteException`。AIDL 调用不是 oneway，所以正常返回说明该次 Binder 事务已经返回；但调用方得不到“是否找到 Job”或“取消了几个”的结果，远端异常时甚至不会收到失败值。

服务端先保存真实 calling UID，再清除 Binder identity：

```java
        @Override
        public void cancel(int jobId) throws RemoteException {
            final int uid = Binder.getCallingUid();

            long ident = Binder.clearCallingIdentity();
            try {
                JobSchedulerService.this.cancelJob(uid, jobId, uid);
            } finally {
                Binder.restoreCallingIdentity(ident);
            }
        }
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

App 不能传入别的 UID，安全边界就是服务端捕获的 calling UID。单项键与 replacement 相同，都是 `(scheduling UID, jobId)`；找不到时内部返回 false，公开 API 不抛 not-found，因此重复取消表现为幂等。

`cancelAll()` 同样捕获 calling UID，却调用 `cancelJobsForUid()`；shared UID 的多个包共享这个命名空间，其中任一成员调用都可能清掉该 UID 桶中的所有 Job，而不按 source package 区分。

## 3. 一次完整取消按什么顺序收拢状态

真正的汇合点是 `cancelJobImplLocked(cancelled, incomingJob, reason)`。永久取消传入 `incomingJob=null`，顺序为：

```text
撤销旧 JobInfo ClipData grant
→ 清 pending/executing WorkItem，并逐项撤 grant
→ 从 JobStore 两张索引删除
→ 通知各 Controller 停止跟踪
→ 从内部 mPendingJobs 删除并闭合 PackageTracker pending
→ 若在 active context 中，发出 REASON_CANCELED 停止请求
→ 重新汇报 active 状态
```

对应 r48 原方法是：

```java
    private void cancelJobImplLocked(JobStatus cancelled, JobStatus incomingJob, String reason) {
        if (DEBUG) Slog.d(TAG, "CANCEL: " + cancelled.toShortString());
        cancelled.unprepareLocked();
        stopTrackingJobLocked(cancelled, incomingJob, true /* writeBack */);
        // Remove from pending queue.
        if (mPendingJobs.remove(cancelled)) {
            mJobPackageTracker.noteNonpending(cancelled);
        }
        // Cancel if running.
        stopJobOnServiceContextLocked(cancelled, JobParameters.REASON_CANCELED, reason);
        // If this is a replacement, bring in the new version of the job
        if (incomingJob != null) {
            if (DEBUG) Slog.i(TAG, "Tracking replacement job " + incomingJob.toShortString());
            startTrackingJobLocked(incomingJob, cancelled);
        }
        reportActiveLocked();
    }
```

这不是跨组件原子事务。代码在 JSS `mLock` 内固定了内存操作顺序，但 active 停止、应用回调和 jobs.xml 写盘各有自己的完成点。

## 4. 两类 URI grant 在取消时都撤销，但入口不同

第一类来自 `JobInfo.setClipData()`。schedule 时 `prepareLocked()` 创建 `JobStatus.uriPerms`；取消第一步 `unprepareLocked()` 令 `prepared=false`，并调用 `uriPerms.revoke()`。

第二类来自每个 `JobWorkItem` 的 Intent data/ClipData。`enqueueWorkLocked()` 把 grant 放在 WorkItem 自身；永久取消走 `JobStatus.stopTrackingJobLocked(null)`：

```java
        } else {
            // We are completely stopping the job...  need to clean up work.
            ungrantWorkList(pendingWork);
            pendingWork = null;
            ungrantWorkList(executingWork);
            executingWork = null;
        }
        updateEstimatedNetworkBytesLocked();
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java`

已由 `completeWorkLocked()` 确认完成的 work 在移出 executingWork 时已经撤权；取消只处理仍在 pending/executing 列表里的项。replacement 则不同：它把旧 executingWork 放到新 pendingWork 前部，再接旧 pendingWork，保留 WorkItem 对象及其 grant。

撤权发生在请求应用停止之前。`GrantedUriPermissions.revoke()` 在 r48 通过 `UriGrantsManagerInternal` 的 LocalServices 接口执行；它是锁内的跨子系统调用，但不能写成必然发生一次 Binder 往返。

## 5. active Job 的“已取消”不等于进程已停止

JobStore 与 pending 状态先被移除，然后 `stopJobOnServiceContextLocked()` 才根据当前 context 状态请求停止。若 Job 正在执行，JobServiceContext 进入 STOPPING、设置超时并调用应用端 `stopJob()`；如果还在 bind/start 握手，走相应的延迟取消分支。

所以 `cancel()` 正常返回最多说明服务端已处理取消请求并发起所需的停止流程，不证明：

- 应用的 `onStopJob()` 已经返回；
- 进程中的业务线程、网络请求或数据库事务已停止；
- wakelock/context cleanup 已完成；
- persisted 快照已落盘。

应用确认 stop 时可以返回“需要重排”，但显式取消不会因此复活旧 Job。原因是旧对象此前已从 JobStore 删除；稍后 `onJobCompletedLocked()` 再调用 `stopTrackingJobLocked(old, ...)` 会发现对象不存在并直接返回，不把为失败重排构造的新对象重新注册。

`onStopJob()` 因而是中断通知与资源收尾点，不是事务回滚钩子。业务写入是否需要幂等、补偿或单独的 durable checkpoint，仍由应用负责。

## 6. 包广播：禁用、更新、卸载与 force-stop 并不相同

`mBroadcastReceiver` 在 `PHASE_SYSTEM_SERVICES_READY` 注册。r48 的分支可以概括为：

| 广播 | r48 行为 |
|---|---|
| `PACKAGE_CHANGED` | 只有 changed component 列表含裸包名且整个应用为 disabled/disabled-user 时批量取消；随后对该 UID 重评 Controller |
| `PACKAGE_REMOVED` + `EXTRA_REPLACING=true` | 跳过取消和 Controller/缓存清理 |
| `PACKAGE_REMOVED` + 非 replacing | 按 package+UID 取消，再调用各 Controller `onAppRemovedLocked`，删除 `mDebuggableApps[pkg]` |
| `QUERY_PACKAGE_RESTART` | 只查询是否存在 source package 匹配的 Job，并设置广播结果；不取消 |
| `PACKAGE_RESTARTED` | 按 package+UID 取消，表示 force-stop |

文件头注释声称“即使稍后 replacement 也清理”，但实际 `PACKAGE_REMOVED` 明确在 `EXTRA_REPLACING` 为 true 时跳过。这是注释与实现漂移；分析 r48 行为应以分支为准，不能据注释写成“应用更新一定清空 Job”。

单个 JobService 组件被禁用不走整包取消，只触发 Controller 重评；完整 ready 门中的组件可用性还会影响候选和运行。

## 7. package cleanup 的代理 Job 缺口与 shared UID 边界

普通 App 的 scheduling UID、source UID 与 package UID 一致，package cleanup 能命中。但系统代包调度时可能是：

```text
scheduling UID = system UID
source UID/package = 被卸载应用的 UID/package
```

卸载广播把被卸载应用的 UID 传给 `cancelJobsForPackageAndUid()`。方法先查这个 UID 的 scheduling 桶，因此看不到存放在 system UID 桶中的代理 Job；后面的 source package 过滤根本没有机会执行。JobStore 明明维护 `mJobsPerSourceUid`，这条入口却未使用它。

这是 r48 的源码边界，不应泛化为所有 Android 版本都漏清。用户删除路径按 source user 查找，反而能命中这种代理 Job。

shared UID 呈现相反风险：package cleanup 先锁定共享 scheduling UID，再按 source package 精确过滤，通常只取消目标包；但随后 `ConnectivityController.onAppRemovedLocked(pkg, uid)` 直接 `mTrackedJobs.delete(uid)`，是 UID 粒度。其他 Controller 可能按 user+package 清理。Controller 销户粒度并不统一，需要逐个审计，不能把“主 Job 选择精确”外推到所有附属状态。

## 8. 用户删除与启动期清理使用不同口径

`cancelJobsForUser(userId)` 调用 `JobStore.getJobsByUser()`；后者遍历 `mJobsPerSourceUid`，选择 source UID 属于该 user 的 Job。因此它会清掉“其他 calling UID 代已删除用户调度”的 Job，然后逐个 Controller 调用 `onUserRemovedLocked(userId)`。

反向情况是：calling UID 属于已删除用户、source user 却仍存在。实时 user-removed 入口只按 source user，理论上不会命中这种受信任代理组合。

JSS 在 `PHASE_SYSTEM_SERVICES_READY` 还调用 `cancelJobsForNonExistentUsers()`。JobStore 的谓词只要 source user 或 calling user 任一不存在就直接从两张索引 `removeIf`。这发生在启动期 Controller attach、pending 和 active 建立之前，因此无需停止运行实例；但它没有走 `cancelJobImplLocked()`，也没有调用 `maybeWriteStatusToDiskAsync()`。

结果是内存会排除孤儿 Job，而 jobs.xml 中的旧记录可能继续留到下一次其他写盘重建快照；若一直没有写盘，下次启动仍可能再次读入后删除。这里应称为“持久化清扫延后”，不能推导成运行期 URI grant 泄漏：从磁盘恢复的 JobWorkItem 不存在，且可持久化 Job 不携带这类临时 grant 状态。

## 9. UID observer 与 shell 为什么还会扩大选择范围

AMS 的 UID observer 把状态投递到 JSS Handler：

- `MSG_UID_GONE` 总会清掉 UID priority override 并标记 idle；只有 `disabled=true` 才 `cancelJobsForUid(uid)`。
- `MSG_UID_IDLE` 也只有 `disabled=true` 才整 UID 取消。
- Handler 每条消息末尾调用 `maybeRunPendingJobsLocked()`，但 active cleanup 仍可能尚未完成。

这里按 scheduling UID 全清，并受 system UID 批量保护。普通进程死亡本身不等于永久取消 registered Job；只有 AMS 同时判定 disabled 才走这一支。

shell `cmd jobscheduler cancel PACKAGE [JOB_ID]` 先把 package+user 解析为 UID。指定 jobId 时调用 `(UID, jobId)` 单项取消；未指定时调用 `cancelJobsForUid(UID)`。因此 shared UID 下的“取消某包全部”实际会覆盖同 UID 的其他包，而 system UID 整批取消会被拒绝。单项 `cancelJob()` 本身没有 system UID 批量保护。

## 10. JobStore、Controller 与 jobs.xml 的完成点

`JobSet.remove()` 同时从 scheduling UID 的 `mJobs` 和 source UID 的 `mJobsPerSourceUid` 删除同一对象；两边结果不一致会记录 wtf。之后各 Controller 收到 `maybeStopTrackingJobLocked(old, incoming, false)`，自行清理 alarm、网络、quota、content observer 等附属状态。

对于 persisted Job，`JobStore.remove(old, true)` 只安排延迟写。默认 `JOB_PERSIST_DELAY=2000ms`，`mWriteScheduled` 已为 true 时，多次删除会合并到后续整表快照。non-persisted 删除不触发 jobs.xml 写。

因此崩溃窗口要分开：

| 时刻 | 内存 | 磁盘 |
|---|---|---|
| `cancelJobImplLocked()` 返回后 | 旧 Job 已从权威 JobStore 删除 | 可能仍有旧 persisted 记录 |
| 异步快照成功后 | 仍已删除 | 新 jobs.xml 不再包含旧 Job |
| 写盘前 system_server/设备异常终止 | 运行态随进程消失 | 下次启动可能从旧快照恢复 |

正常的有序关机、系统其他写盘和后续调度可能缩短窗口，但公共 `cancel()` 没有 durable ACK。若应用业务绝不能在重启后再看到任务，仅依赖一次 void cancel 并不足以构成端到端事务协议。

另有两个 r48 缓存边界：真正卸载会按真实包名删除 `mDebuggableApps`，却清不到第 139 章普通 schedule 写入的 null key；`JobSchedulerStub.mPersistCache` 按 UID 缓存 persisted 权限，本文件的卸载路径没有清理它。两者都是实现观察，不是应用可依赖的契约。

## 11. 公开契约与 r48 实现要分开

- `JobScheduler.cancel()`、`cancelAll()` 的公开契约可用于删除本 UID 的 Job，但 API 不返回命中数，也不等待应用清理或磁盘提交。
- `cancelJobsForPackageAndUid()`、UID observer、shell 命令、双索引、Controller 清理与缓存行为都是系统内部实现。
- `REASON_CANCELED` 在 Android 11 源码中参与内部 `JobParameters`，相关 stop reason 读取接口在 r48 不是普通 SDK 应用可稳定依赖的公开面。
- 代理 Job 漏选、shared UID 粗清、启动期不写盘和注释漂移都必须带上 `android-11.0.0_r48` 版本限定。
- 静态阅读可以证明调用顺序和集合口径，不能给出设备上撤权、进程停止或 I/O 完成的实际耗时。

## 12. 用源码复核一次取消

在 Android 11 r48 源码根目录只读执行：

1. 看客户端 void API 与服务端 Binder identity：

   ```bash
   sed -n '35,85p' \
     frameworks/base/apex/jobscheduler/framework/java/android/app/JobSchedulerImpl.java
   sed -n '2710,2760p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 比较单项、UID、package、user 四类选择器及完整取消：

   ```bash
   sed -n '1175,1300p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 追包/用户广播和 UID Handler：

   ```bash
   sed -n '805,930p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '1935,1980p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

4. 对照 WorkItem 撤权、双索引与启动期直接清理：

   ```bash
   sed -n '620,725p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
   sed -n '1070,1175p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
   ```

5. 检查至少两个 Controller 的 package 清理粒度：

   ```bash
   sed -n '300,320p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/ConnectivityController.java
   sed -n '575,605p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/QuotaController.java
   ```

## 13. 练习与参考答案

### 练习一：代包卸载

system UID 用 `scheduleAsPackage()` 为 user 10 的包 A 调度一个 Job。A 卸载时，package cleanup 能否靠 package+UID 入口命中？user 10 删除时呢？

参考答案：前者通常不能，因为 Job 在 system scheduling UID 桶中，而入口先查 A 的 UID 桶；后者能，因为 `getJobsByUser(10)` 按 source UID 的 user 查询。

### 练习二：shared UID

共享 UID 的包 A、B 各有 Job。A 调用 `cancelAll()` 与 A 被卸载分别会怎样？

参考答案：`cancelAll()` 按 scheduling UID 全清，可同时取消 A、B；卸载 A 的主选择器还会按 source package 过滤，通常只取消 A，但 Controller 的 `onAppRemovedLocked` 可能仍按 UID 粗粒度销户。

### 练习三：取消 active Job

`cancel(jobId)` 已正常返回，但应用的工作线程仍在写文件。这是否违反 JSS 语义？

参考答案：不违反。服务端先删除注册/候选状态并发送停止请求，应用回调和进程内业务停止是后续完成点；应用必须在 `onStopJob()` 等处主动终止或隔离自己的工作。

### 练习四：两类 grant

JobInfo ClipData 与 executing WorkItem Intent 都带 URI 授权。永久取消和 replacement 各如何处理？

参考答案：永久取消先撤 JobInfo grant，再逐项撤全部剩余 WorkItem grant；replacement 撤旧 JobInfo grant并为新 JobInfo预先建 grant，但迁移 WorkItem 对象及其 grant，不把它们当作已完成 work 回收。

### 练习五：取消后立刻断电

persisted Job 从内存删除后、异步快照前设备异常终止，下次启动一定不会恢复它吗？

参考答案：不能保证。void cancel 没有磁盘提交 ACK，旧 jobs.xml 仍可能包含记录；这与本次运行中 JobStore 已删除并不矛盾。

## 本章带走什么

理解取消要先问“用 scheduling 身份还是 source 身份选集合”，再问“内存、授权、运行回调和磁盘各结束到哪一步”。普通单项 cancel 用 scheduling UID+jobId；整 UID 取消可能扩大到 shared UID；package cleanup 的双条件在 r48 反而可能漏掉 system 代包 Job；user removal 按 source user，能覆盖另一部分代理场景。

选中 Job 后，完整取消会先撤 JobInfo 与 WorkItem 两层 URI grant，再清 JobStore、Controller 与 pending，并向 active context 发停止请求。这个顺序能证明旧任务已退出系统调度集合，却不能把应用收尾、进程停止和 jobs.xml 耐久提交压缩成一个同步完成点。
