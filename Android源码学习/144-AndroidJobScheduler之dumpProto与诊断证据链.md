# 144 Android JobScheduler：dump、Proto 与诊断证据链

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 131、136、138、141、143 章

## 先看问题：`ready=true` 为什么仍不能证明 Job 应该立刻运行

Job 不运行可能卡在五层：JobStatus 约束、JSS 外部门、batch/pending、并发槽、JobServiceContext 与应用回调。`dumpsys jobscheduler` 把这些层放进一份输出，`cmd jobscheduler get-job-state` 又把部分状态压成几个短标签；如果把不同层的 “ready”“pending” 当成同一件事，很容易得出相反结论。

例如短命令的 `ready` 只调用 `JobStatus.isReady()`，没有检查用户、backup、thermal restriction、pending/active 去重或 AMS bad app；完整 dump 的 overall `Ready=false` 又可能仅仅表示 Job 已在 pending，属于正常防重复。

本章回答：**看到一个 Job 没跑时，怎样从 Registered 一直取证到 Active，并明确每个字段的快照、过滤和时钟边界？**

## 1. dump 入口是受权限保护的诊断面

Binder `dump()` 先调用 `DumpUtils.checkDumpAndUsageStatsPermission()`：调用者必须通过 `DUMP` 检查，同时通过 Usage Stats permission/AppOps；root、system、shell、incidentd 只在 usage 检查中有内部 UID 豁免。

权限通过并解析参数后才 `Binder.clearCallingIdentity()`，最后在 `finally` 恢复。内部查询以 system_server 身份执行，但入口授权已在清身份前完成。

参数只有：

```text
-h        显示帮助
-a        接受但忽略，因为本来就 dump all
--proto   改走二进制 Proto writer
package   第一个非 option 参数作为包过滤
```

未知 option 直接报错；包名之后的额外参数不会继续解释。`--proto` 与文本路径二选一，不能把输出当 UTF-8 文本直接读。

## 2. 这是 JSS 锁内快照，不是全系统原子快照

文本与 Proto 都先取：

```text
now        = wall clock currentTimeMillis
nowElapsed = elapsedRealtime
nowUptime  = uptimeMillis
```

然后在 `mLock` 内打印 Constants、Controller、JobStore、pending、active、JCM 与 PersistStats。JSS 自有集合在这段期间不会并发改动，因而相对一致。

但 PackageManager、AMS、网络与其他服务仍可变化，某些打印路径还会现场查询它们。持有 JSS 锁不等于冻结 Android；大 dump 反而可能延迟 schedule、Controller 更新和 Job 完成回调。

## 3. Registered、Pending、Active 是三种关系，不是三类互斥 Job

- Registered：对象存在于 JobStore，是权威登记集合。
- Pending：对象已通过完整候选门与 batch 政策，正在等待/竞争执行槽。
- Active：某个 JobServiceContext 正在处理该 scheduling UID+jobId。

Pending 与 Active 通常仍是 Registered 的子状态。`Registered N jobs` 不能回答有多少 Job 正等待；Pending 也不代表已经 bind 应用。

Registered 的 full dump 先展示两条身份轴：`callingUid+jobId` 是调度键，`sourceUid/sourceUser/sourcePackage` 负责 standby、quota、backup 和统计归因；代理调度时二者可能不同。Service component 是实际绑定目标。

同一详情里还要分开：

```text
JobInfo    应用声明：periodic、persisted、priority、network、latency、deadline、backoff…
JobStatus  系统派生：required/satisfied/dynamic、implicit ready、bucket、窗口、失败次数…
```

## 4. Required/Satisfied 只回答显式位，不是完整 ready

`JobStatus.isReady()` 还组合 quota/dynamic 顶层门、NEVER bucket、NOT_DOZING、后台限制、deadline override 与普通 constraints。full dump 的 Unsatisfied 行只计算：

```java
                    ((requiredConstraints | CONSTRAINT_WITHIN_QUOTA) & ~satisfiedConstraints));
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java`

它不会把所有 implicit 与 dynamic 失败折成一行。因此 Required 全部在 Satisfied 中，仍可能被 NOT_DOZING、background restriction、NEVER 或 dynamic/quota 逻辑挡住。

非周期 Job 的 deadline 满足可以绕过多数普通显式约束，却不能绕过 quota/dynamic 顶层门、NEVER、Doze 与后台限制；周期 Job 的 latest runtime 也不是同样的强制 deadline override。

`Tracking: BATTERY CONNECTIVITY ...` 只说明哪些 Controller 正负责维护该 Job，不说明对应约束已满足或缓存一定最新。事实要结合 Satisfied 与 Controller 自己的 tracker/alarm/cache 段。

## 5. overall Ready 要拆成七个分项看

JSS 文本对每个匹配的 Registered Job 打印：

```java
                    pw.print("    Ready: ");
                    pw.print(isReadyToBeExecutedLocked(job));
                    pw.print(" (job=");
                    pw.print(job.isReady());
                    pw.print(" user=");
                    pw.print(areUsersStartedLocked(job));
                    pw.print(" !restricted=");
                    pw.print(!isRestricted);
                    pw.print(" !pending=");
                    pw.print(!mPendingJobs.contains(job));
                    pw.print(" !active=");
                    pw.print(!isCurrentlyActiveLocked(job));
                    pw.print(" !backingup=");
                    pw.print(!(mBackingUpUids.indexOfKey(job.getSourceUid()) >= 0));
                    pw.print(" comp=");
                    pw.print(isComponentUsable(job));
                    pw.println(")");
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

解读顺序是：

1. `job`：JobStatus 内部 ready；
2. `user`：calling 与 source user 都 started；
3. `!restricted`：JSS 额外 restriction 未命中；
4. `!pending`：不是同一对象的已有候选；
5. `!active`：没有同 scheduling UID+jobId 正运行；
6. `!backingup`：source UID 不在 backup；
7. `comp`：ServiceInfo 可用且 AMS 未判 App bad。

overall false 不一定是故障。若 `!pending=false` 或 `!active=false`，通常只是防止重复加入。`comp=false` 又把组件缺失/禁用与 AMS bad app 合并成一个布尔，需回到 package/AMS 证据拆因。

ThermalStatusRestriction 不写 Required/Satisfied 位；它出现在 `Restricted due to` 与 `!restricted`。这正是“JobStatus ready”与“JSS overall ready”必须分层的原因。

## 6. Controller 与 PackageTracker 提供的是旁证

Controller 段回答“约束事实由谁维护”：Time 的下一 Alarm、Connectivity 的网络/UID 缓存、Quota 的账户窗口、Battery/Storage/Idle 的 tracker 状态。各 Controller 接受同一个 predicate，但内部输出粒度没有统一 schema。

PackageTracker 记录 source UID+package 的 pending/active 时长、峰值、停止计数与最多 100 条事件历史。它不是当前队列：没有历史可能是环形缓冲已覆盖或 system_server 重启；有 pending episode 也可能来自第 141 章的全量重建切分。

诊断要用 Controller/Tracker 解释 Registered 与队列状态，不能让统计反过来替代当前权威集合。

## 7. 已 Pending 时，应把注意力移到 JCM

Pending 段用 short JobStatus，另打印现场 `evaluateJobPriorityLocked()`、tag 和 madePending 相对时间。文本执行：

```java
                TimeUtils.formatDuration(job.madePending - nowUptime, pw);
```

所以已等待 5 秒常显示 `-5s`；Proto 用 `nowUptime - madePending` 写正的 `pending_duration_ms`。负号描述过去时间点，不是异常。

一旦确认 Job 在 pending，下一步应查 JCM 的 screen/memory 配置、FG/BG 容量、active context、preferred UID、现场 priority 与抢占，而不是继续只盯 Battery/Network。

Pending 的 evaluated priority 是 dump 时重算值，可能读取当前 UID override 与 PackageTracker load；它不保证等于分配当时保存在 `lastEvaluatedPriority` 的快照。

## 8. Active 段是 16 个槽的快照

JSS 遍历固定的 16 个 JobServiceContext。running 槽显示运行时长、距 timeout、short JobStatus、现场优先级、madeActive 与此前 pending 时长。

空槽也会打印；若它最近停止过，则保留 `mStoppedTime/mStoppedReason`。这个 reason 属于该槽最近一代执行，不应凭位置配给任意 Registered Job，槽复用后尤其容易串因果。

Active 后很快消失时，可把 unique ID 与 PackageTracker history、失败次数、下一运行窗口连接起来；若要证明应用 `onStartJob()`/`onStopJob()` 做了什么，还需要应用日志或 Binder/ANR 等证据。一次 dump 主要是当前采样，不是完整事件日志。

## 9. package 参数不是全文过滤器

入口用 `MATCH_ANY_USER` 将包名解析为一个 UID，内部立即取 `UserHandle.getAppId(filterUid)`。predicate 匹配：

```text
appId(job.callingUid) == filterAppId
OR appId(job.sourceUid) == filterAppId
```

因此它跨 user 匹配同 appId，也能因代理 Job 的 calling/source 任一侧命中，并不等价于 `service.package == 输入包名`。

r48 文本 Registered 循环更先打印所有 Job 的一行 short title，之后才测试 predicate、跳过不匹配详情；顶部计数也仍是全局数。Controller、priority override、backup 与 PackageTracker 各自按 predicate/appId 过滤，但 Pending、Active、JCM、PersistStats 没有 package 判断，依然输出全局状态。

所以看到其他包并非参数完全失效。过滤是阅读便利功能，不是隐私边界；完整 dump 本就要求高权限，分享日志仍需对 UID、包、extras、URI 和 grant 信息脱敏。

## 10. Proto 不是文本输出的机械翻译

Proto 用强类型 duration、enum、oneof 与 repeated message，适合 incident/自动分析；文本包含人类标签与负方向相对时间。两边字段和过滤实现并不完全相同。

r48 Proto Registered 循环存在一个明确缺口：

```java
                    final long rjToken = proto.start(JobSchedulerServiceDumpProto.REGISTERED_JOBS);
                    job.writeToShortProto(proto, JobSchedulerServiceDumpProto.RegisteredJob.INFO);

                    // Skip printing details if the caller requested a filter
                    if (!predicate.test(job)) {
                        continue;
                    }
```

匹配分支最后才 `proto.end(rjToken)`；不匹配时 `continue`，没有显式 end。至少可以确认 token 配对不对称，因此带 package 过滤的 Proto 结构可靠性要谨慎对待，不能用文本路径的结果替它背书。

schema 中 reserved heartbeat/parole 等字段号用于兼容，不能证明 r48 仍有对应机制；schema 声明某字段，也要回到 Java writer 确认本版本实际是否写入。

## 11. `get-job-state` 是短摘要，不是 overall 判定

shell 命令先按 package+user 解析 scheduling UID，再以 `(uid, jobId)` 查 JobStore。可能输出：

```text
pending active user-stopped source-user-stopped backing-up no-component ready waiting
```

help 在 r48 漏列了实现会输出的 `source-user-stopped`。更重要的是各标签独立检查：

- `ready` 只来自 `js.isReady()`；
- user/source-user、backup、pending、active 另行打印；
- `no-component` 只查 ServiceInfo 是否存在，不含完整路径的 AMS `isAppBad()`；
- `waiting` 只是前面一个标签都没打印，不解释具体未满足 constraint。

所以 `user-stopped ready` 或 `backing-up ready` 并不矛盾；短命令的 ready 不是“现在可执行”。要找等待原因，仍需 Registered full dump 的 explicit/implicit 状态和 Controller 段。

`cmd jobscheduler run/cancel/reset-*` 会改变目标状态，不属于本章只读取证步骤。尤其不要为了观察原始失败原因先强制 run，导致 override 改写你要诊断的对象。

## 12. 一条可复用的七步证据链

```text
1. Registered
   对象存在吗？calling/source/service 身份对吗？
        ↓
2. JobInfo
   应用声明的约束、窗口、persisted、backoff 对吗？
        ↓
3. JobStatus
   required/satisfied/dynamic/implicit/quota/bucket 哪项失败？
        ↓
4. overall Ready 分项
   users、restriction、backup、component、pending/active 去重如何？
        ↓
5. Controller
   tracker、Alarm、网络或 quota 缓存是否支持上面的事实？
        ↓
6. Pending + JCM
   是否被 batching 延后，或因槽位/优先级/容量等待？
        ↓
7. Active + JSC + history
   bind、回调、timeout、stop reason、失败重排发生到哪一步？
```

这条链的价值是把“为什么没跑”拆成可证伪的小问题。单看某个 ready、waiting 或 stopped reason，都不足以跳到“系统 Bug”。

## 13. 时钟与持久化边界

| 字段 | 时间基准 | 解读 |
|---|---|---|
| enqueue/runtime window | elapsed realtime | 本次 boot 单调轴，包含休眠流逝 |
| madePending/madeActive | uptime | 深睡不累计，用于活动阶段相对时间 |
| running/timeout | elapsed realtime | 运行与超时预算 |
| last success/failure | wall clock | 可映射日期，但受 RTC 校正 |
| PackageTracker batch | uptime/elapsed/wall 快照 | 分别服务时长、事件与人类时间 |

PersistStats 只说明 JobStore 读写数量等统计，不等于 jobs.xml 当前逐字内容，也不证明最近一次异步写已经 durable。验证重启恢复必须结合 JobStore 写入状态及重启后的 Registered 事实。

## 14. API、实现与版本边界

- dumpsys、Proto schema、JSS 内部 pending/JCM 都不是普通应用 API。
- public `getAllPendingJobs()` 返回该 UID 已注册的 JobInfo，不等于 dump 的内部 Pending queue。
- package predicate、Proto token 缺口、help 漏项和字段集合都必须限定在 `android-11.0.0_r48`。
- 一次 dump 不能证明过去因果；有限 history、logcat/statsd/应用日志或连续快照可补足不同时间段。
- 持锁快照提高 JSS 内部一致性，但可能扰动被观察系统，也不能冻结外部服务。

## 15. 从源码核对诊断口径

在 Android 11 r48 源码根目录只读执行：

1. 看 dump 权限、参数、identity：

   ```bash
   sed -n '2750,2820p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 按输出顺序审计文本过滤范围：

   ```bash
   sed -n '3135,3330p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 对照 Proto token、Pending/Active duration：

   ```bash
   sed -n '3330,3505p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

4. 读 JobStatus full/short、显式与 implicit 字段：

   ```bash
   sed -n '1680,1780p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
   ```

5. 比较短命令实现与 help：

   ```bash
   sed -n '3000,3100p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '470,495p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerShellCommand.java
   ```

## 16. 练习与参考答案

### 练习一：overall false

一行显示 `job=true user=true !restricted=true !pending=false`。应继续查约束还是 JCM？

参考答案：查 Pending/JCM。`!pending=false` 表示对象已在候选队列，overall false 是防重复。

### 练习二：短命令组合

`get-job-state` 同时输出 `source-user-stopped ready` 是否自相矛盾？

参考答案：不矛盾。ready 只表示 JobStatus 内部约束满足，source user 外部门仍阻止执行。

### 练习三：过滤范围

带 package 参数后仍看见其他包的 Pending 与 Active，能否证明过滤失败？

参考答案：不能。r48 这两个段落本来就不应用 predicate，Registered 也保留所有 short title。

### 练习四：时间符号

文本 `Enq: -5s` 与 Proto `pending_duration_ms=5000` 是否冲突？

参考答案：不冲突。前者是 madePending 相对 now 的过去时间点，后者是正持续时长。

### 练习五：active 消失

inactive slot 显示 stopped reason，能否直接归因给同编号 Registered Job？

参考答案：不能。reason 属于该槽最近执行代际，槽会复用；要用 unique ID、时间和 history 交叉关联。

## 本章带走什么

可靠的 JobScheduler 诊断不是寻找一行“最终原因”，而是沿 Registered→声明→内部约束→外部门→Controller→Pending/JCM→Active/history 逐层缩小范围。Required 全满足不等于 JobStatus ready，短命令 ready 不等于 overall ready，overall false 又可能只因已经 pending/active。

同时要记住观测工具自身的边界：dump 是 JSS 锁内而非全系统快照；package 参数按 appId 和 calling/source 过滤部分段落，不覆盖全文；Proto 与文本字段、时间符号和过滤实现不同，r48 甚至有 token 未闭合分支。先确定字段属于哪一层、哪种时钟和哪个采样时刻，再把证据串成因果链。
