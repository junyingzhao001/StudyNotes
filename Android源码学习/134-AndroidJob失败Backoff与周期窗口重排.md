# 134 Job 失败 Backoff：重试为什么从现在算，周期任务为什么仍守原时间表？

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置知识：第 132 章 Job 执行状态机、第 133 章 WorkItem 未确认项重投

## 先给结论：这章解决什么问题

有一个每小时同步照片的 periodic Job，允许在每小时最后 20 分钟内运行。它在 10:50 执行失败，并请求重试：

```text
它会等到下一个小时窗口吗？
还是从 10:50 开始等 30 秒后重试？
重试成功后，下一周期从成功时间重新算一小时吗？
```

Android 11 的答案是：**失败时先走 backoff，从“本次结束时刻”计算一次重试；这次重试成功后，再走 periodic 窗口算法，尽量回到原周期锚点，而不是把整张时间表永久平移。**

可以把 periodic Job 想成一张列车时刻表，把失败 backoff 想成故障后的临时晚点：

```text
正常周期：沿时刻表安排下一班
本次失败：先临时等一段 backoff 再试
重试成功：尽量回到原时刻表
```

读完本章，你应该能：

- 从 `needsReschedule` 判断完成后走一次性结束、失败退避还是周期续期；
- 手算线性和指数 backoff，理解 10 秒、30 秒、5 小时分别是什么；
- 解释 periodic 的 earliest、latest 与 `originalLatest`；
- 手算“窗口内完成”“晚过窗口完成”后的下一窗口；
- 识别“时间到了”只代表时间约束满足，不代表 Job 立即获得执行槽；
- 理解 persisted Job 重启后能保留什么，又会丢掉什么。

本章只研究“一个执行实例结束后，新 JobStatus 的时间边界怎样产生”，不重复讨论网络、配额、并发槽和 App 线程执行细节。

---

## 一、总入口只有一个，结果却有三种

第 132 章的 `JobServiceContext` 清理完成后，会把旧 `JobStatus` 和 `needsReschedule` 交给：

```text
JobSchedulerService.onJobCompletedLocked(jobStatus, needsReschedule)
```

核心分叉可以压缩成：

```java
JobStatus retry = needsReschedule
        ? getRescheduleJobForFailureLocked(old) : null;

stopTrackingJobLocked(old, retry, ...);

if (retry != null) {
    startTrackingJobLocked(retry, old);
} else if (old.getJob().isPeriodic()) {
    startTrackingJobLocked(getRescheduleJobForPeriodic(old), old);
}
```

对应三类结果：

| Job 类型与结束结果 | 新记录 | 时间算法 |
|---|---|---|
| 一次性 Job，`needsReschedule=false` | 没有 | 真正结束 |
| 任意 Job，`needsReschedule=true` | 失败重试记录 | backoff |
| periodic Job，`needsReschedule=false` | 下一周期记录 | periodic 窗口重排 |

这里最值得记住的是优先级：**periodic Job 失败时也先走失败 backoff，不会直接跳到下一周期。**等它某次执行成功，才重新进入正常周期续期。

### needsReschedule 不等于“发生 Java 异常”

它是执行协议的最终决定，不是异常检测器。例如：

- `jobFinished(params, true)` 请求失败重调度；
- `onStopJob()` 返回 `true`，正常停止确认会带回重调度请求；
- App 进程意外断开，某些清理路径直接指定重调度；
- `onStartJob()` 返回 `false` 表示同步完成，不是失败；
- 绑定或启动阶段某些超时路径可能直接按不重试清理；
- 明确 `cancel(jobId)` 是取消，不是 backoff。

所以排查“为什么没重试”时，要沿具体停止原因追到 `closeAndCleanupJobLocked(reschedule, reason)`，不能只搜有没有异常栈。

---

## 二、为什么先创建新 JobStatus，再移除旧记录

完成分叉中有一个看似绕的顺序：先构造 `retry`，再 `stopTrackingJobLocked(old, retry, ...)`。

原因是旧记录里可能还有必须交接的状态：

- 第 133 章的 `executingWork + pendingWork`；
- ContentObserverController 尚未消费的 changed URI/authority；
- periodic Job 原来的时间锚点；
- source UID、约束、内部标志等 JobStatus 构造信息。

只有新旧对象同时存在，旧状态才能有明确去向。例如 WorkItem 迁移需要把新对象作为 `incomingJob` 传入旧 JobStatus：

```text
old.executing + old.pending
          │
          └── stopTrackingJobLocked(incoming)
                         ↓
                 incoming.pending
```

如果旧 Job 已在执行期间被相同 jobId 的新 schedule 替换，完成回调时 `stopTrackingJobLocked()` 可能找不到旧记录。r48 会直接返回，不会用过时的完成结果把用户新提交的 Job“复活”。

这是一条重要的代际边界：**完成的是 old，不代表 old 仍然是 JobStore 中当前有效的那一代。**

---

## 三、失败 backoff 解决的不是“定时”，而是故障扩散

如果一次网络失败后立即无限重跑，会产生连锁问题：

```text
服务端故障
  → 大量设备立即重试
  → 网络与服务端负载继续升高
  → 再失败、再立即重试
```

Backoff 的目的，是让失败请求逐步拉开。它只规定“最早何时可再次尝试”，不是精准闹钟，也不是保证那个时刻一定执行。

还要坦诚一个限制：上面这段 r48 计算本身没有随机 jitter。如果大量设备以相同时间、相同次数失败，它们的理论退避点仍可能接近；指数增长能降低重试频率，但单靠这几行不能保证设备之间完全错峰。

### 1. 三个输入和一个输出

`getRescheduleJobForFailureLocked()` 读取：

```text
initialBackoffMillis：初始退避基数
backoffPolicy：LINEAR 或 EXPONENTIAL
old.numFailures：旧记录已累计的失败次数
```

先得到：

```java
int attempts = old.getNumFailures() + 1;
```

再创建新 JobStatus：

```java
new JobStatus(old,
        elapsedNow + delay,
        JobStatus.NO_LATEST_RUNTIME,
        attempts,
        old.getLastSuccessfulRunTime(),
        wallClockNow);
```

这几行确定五件事：

1. 第一次失败后的 `attempts=1`；
2. 新 earliest 是当前 `elapsedRealtime + delay`；
3. 失败重试没有 latest/deadline；
4. 新记录保存递增后的失败次数；
5. `lastFailedRunTime` 使用 wall clock 记录，和调度用 elapsed time 不是一套时钟。

### 2. 为什么调度用 elapsedRealtime

用户修改时区或手动校时，不应该让“再等 30 秒”突然变成已经到期或多等几小时。`elapsedRealtime` 是开机后的单调时钟，更适合表达相对延迟。

`lastSuccessfulRunTime`、`lastFailedRunTime` 更像给人和诊断工具看的历史时间，所以使用 wall clock。不要用这两个历史字段反推精确 backoff 剩余时间；真正的门槛是 `earliestRunTimeElapsedMillis`。

---

## 四、线性和指数公式怎样手算

### 1. 三个数字先分清

Android 11 r48 中：

| 数字 | 含义 |
|---|---|
| 30 秒 | 没有自定义时的默认 initial backoff |
| 10 秒 | `JobInfo.Builder` 接受的最小 initial backoff；服务端默认最小值也为 10 秒 |
| 5 小时 | 每一次算出的 delay 上限，不是总重试寿命 |

应用可以设置：

```java
builder.setBackoffCriteria(
        30_000,
        JobInfo.BACKOFF_POLICY_EXPONENTIAL);
```

Builder 会把小于 10 秒的请求抬到 10 秒。服务端计算时还会与可配置的 `MIN_LINEAR_BACKOFF_TIME` 或 `MIN_EXP_BACKOFF_TIME` 再取较大值。因此设备上的有效基数是：

```text
base = max(JobInfo 中的 initialBackoff, system_server 当前策略下限)
```

默认配置下两层下限都是 10 秒，但系统配置可能改变服务端那一层，排查真机时应看 dumpsys 中的常量，而不是只看 App 源码。

### 2. 线性退避

r48 公式：

```text
delay = base × attempts
```

若 base=30 秒：

| 连续失败次数 attempts | 本次 delay |
|---:|---:|
| 1 | 30 秒 |
| 2 | 60 秒 |
| 3 | 90 秒 |
| 4 | 120 秒 |

### 3. 指数退避

r48 公式：

```text
delay = base × 2^(attempts - 1)
```

同样 base=30 秒：

| 连续失败次数 attempts | 本次 delay |
|---:|---:|
| 1 | 30 秒 |
| 2 | 60 秒 |
| 3 | 120 秒 |
| 4 | 240 秒 |
| 5 | 480 秒 |

源码使用：

```java
delayMillis = (long) Math.scalb(backoff, attempts - 1);
delayMillis = Math.min(delayMillis,
        JobInfo.MAX_BACKOFF_DELAY_MILLIS);
```

超过 5 小时后，后续每次 delay 都封顶在 5 小时。它不表示“累计 5 小时后放弃”，r48 这里也没有用这个常量设置总重试次数。

还有一个只在恶意或极端参数下才值得关注的 r48 实现缺口：线性分支先做 `backoff * attempts`，再 `Math.min(..., 5小时)`；Builder 只钳最小值，没有把异常巨大的 initial 值先钳到上限。因此乘法理论上可先发生 `long` 溢出，5 小时 cap 并不是严格的饱和乘法。正常秒/分钟级配置不会碰到它，但做 Framework 审计时不能把“最后有 cap”误读成“前面的运算一定安全”。

### 4. 到期为什么仍可能不运行

Backoff 到期只把 timing delay 约束变成 satisfied。Job 仍可能等待：

```text
网络/充电/空闲等业务约束
App standby 与 quota
Doze/后台限制
并发槽与同 UID 调度竞争
```

r48 对 `numFailures>0` 的 ready Job 有一项有限优待：除 RESTRICTED bucket 外，它不再被普通“非活跃 Job 凑批”逻辑强制继续等待。但这不绕过约束、配额或执行槽，也不把 earliest 变成精确启动时刻。

---

## 五、失败重试为什么清掉原来的 deadline

失败重排创建新 JobStatus 时传入：

```java
newEarliest = elapsedNow + delay;
newLatest   = JobStatus.NO_LATEST_RUNTIME;
```

假设一次性 Job 原来有 11:00 override deadline，10:59 执行后失败。如果失败记录还保留旧 deadline，11:00 一到就可能立刻用 deadline 逻辑绕过部分普通约束，backoff 会失去意义。

所以失败实例只保留“不得早于 backoff 到期”的 earliest，不再给一个原 deadline 兜底。

这也解释了一个常见误判：

```text
原 Job deadline 到期运行
  ≠ 失败后的 retry 也继续拥有这个 deadline
```

periodic Job 的 latest 也会在失败实例中变成 `NO_LATEST_RUNTIME`，但它另存一份 `originalLatest` 作为正常周期时间表的锚点。两者职责不同。

---

## 六、periodic Job 不是“每隔 P 毫秒准时执行”

### 1. period 与 flex 组成一个窗口

```java
builder.setPeriodic(period, flex);
```

语义是：每个周期末尾有一个长度为 flex 的可执行窗口。

例如从 t0 安排 `period=60 分钟、flex=20 分钟`：

```text
t0                 t0+40                 t0+60
│--------------------│=====================│
                      earliest              latest
                      可运行窗口 20 分钟
```

这不是 40 分钟必须运行、60 分钟强制运行。对 periodic Job，latest 是窗口计算的边界；r48 明确禁止 periodic deadline 像一次性 override deadline 那样压过其他约束。网络、充电等不满足时，这个周期可以错过。

### 2. 参数会被钳位

Android 11 公共 Builder 的关键下限是：

```text
period >= 15 分钟
flex >= max(5 分钟, period 的 5%)
```

服务端构造窗口时还防御性地把 period 限在 `[15 分钟, 365 天]`，把 flex 限在 `[5 分钟, period]`。普通 App 通过 Builder 得到的对象已经经过前一层校验，服务端钳位用于守住内部算法边界。

只传一个参数：

```java
setPeriodic(period)
```

等价于 `flex=period`，即 full-flex：第一个窗口理论上覆盖从 t0 到 t0+period 的整段时间。不过其他约束和系统批处理仍决定实际何时执行。

### 3. periodic 不能混配一次性时间 API

`JobInfo.Builder.build()` 会拒绝 periodic 同时设置：

```text
setMinimumLatency()
setOverrideDeadline()
addTriggerContentUri()
```

原因不是语法洁癖，而是这些 API 各有自己的时间/触发语义，混在同一个 JobInfo 中会让“下一周期从哪里算”变得矛盾。

---

## 七、为什么需要 originalLatest 这根时间锚

初次创建 periodic JobStatus 时：

```java
latest = elapsedNow + period;
earliest = latest - effectiveFlex;
```

JobStatus 构造器同时令：

```java
mOriginalLatestRunTimeElapsedMillis = latest;
```

假设原窗口为 `[10:40, 11:00]`。Job 在 10:50 失败，第一次指数 backoff 产生一个临时失败实例：

```text
retry earliest = 10:50:30
retry latest   = none
originalLatest = 11:00   ← 原时间表锚点仍保留
```

如果重试在 10:51 成功，正常周期续期仍能根据 11:00 算出下一窗口 `[11:40, 12:00]`。如果没有 originalLatest，只能从 10:51 重新加一小时，整张时间表会因一次短暂故障永久漂移到 11:51。

因此三个时间不能混用：

| 字段 | 回答的问题 |
|---|---|
| retry earliest | 这次失败重试最早何时可以再尝试 |
| retry latest | r48 失败实例中为 none |
| originalLatest | 原 periodic 时刻表的窗口终点锚 |

失败重试连续失败时，`originalLatest` 继续传给下一代；某次成功后，周期算法才消费它并计算下一窗口。

---

## 八、周期成功后，下一窗口怎样算

源码入口：

```text
JobSchedulerService.getRescheduleJobForPeriodic()
```

输入可以简化成：

```text
now：本次完成时刻（elapsed realtime）
L：originalLatest，原窗口终点
P：effective period
F：effective flex
```

输出：

```text
newLatest
newEarliest = newLatest - min(F, P - rescheduleBuffer)
```

算法先分成“在原窗口终点前完成”和“在终点后完成”。

### 1. 在 originalLatest 之前完成

```java
newLatest = L + P;
```

我们的照片同步在 10:51 重试成功，L=11:00、P=60 分钟、F=20 分钟：

```text
newLatest   = 12:00
newEarliest = 11:40
```

重点是从 11:00 这个锚点续期，而不是 `10:51 + 60 分钟 = 11:51`。

### 2. 在 originalLatest 之后完成

r48 先计算跨过了多少个 period：

```java
long diff = now - L;
long skipped = (diff / P) + 1;
newLatest = L + P * skipped;
```

这里的 `+1` 把原窗口也算进去。错过多个周期时，系统只生成一个未来窗口，不补跑多个历史实例。

例如 full-flex Job 的 L=11:00、P=60 分钟，在 13:10 才完成：

```text
diff=130 分钟
skipped=130/60 + 1 = 3
newLatest=11:00 + 3小时 = 14:00
```

它不会因为错过 12:00、13:00 而立即连续运行两次。这里只保证算出的 `newLatest=14:00` 在未来；full-flex 的 `newEarliest=13:00` 此时已经过去，所以新记录在时间维度上可能立刻 ready。Periodic API 的语义是周期性机会，不是逐条补齐历史账单。

---

## 九、两个“别贴得太近”的保护规则

周期算法还要避免刚执行完，下一窗口马上又允许执行。r48 有两条容易混淆的保护。

### 1. 提前完成：收窄下一窗口起点

当 `now <= L`，且完成时间离 L 非常近：

```java
if (diff < 30min && diff < P / 6) {
    buffer = min(30min, P / 6 - diff);
}
```

随后：

```text
newEarliest = newLatest - min(F, P - buffer)
```

用 full-flex 的一小时 Job 手算：原 L=11:00，在 10:55 完成。

```text
P/6 = 10分钟
diff = 5分钟
buffer = 5分钟
newLatest = 12:00
newEarliest = 12:00 - min(60, 55) = 11:05
```

若没有 buffer，full-flex 下一窗口会从 11:00 立刻开启，离刚完成只有 5 分钟；保护后最早 11:05，形成约 10 分钟的完成间隔。

这里的 30 分钟只是 buffer 上限之一，不代表所有 periodic Job 都至少间隔 30 分钟。条件和比较都是源码里的具体公式。

### 2. 晚于终点：自定义 flex 可能额外跳过一窗

当 `now > L` 且 `P != F`，r48 还判断：

```java
if (diff > min(30min, (P - F) / 2)) {
    skipped++;
}
```

仍用 `P=60 分钟、F=20 分钟`：原 L=11:00，在 11:25 完成。

```text
diff = 25分钟
普通 skipped = 1
阈值 = min(30, (60-20)/2) = 20分钟
25 > 20，所以 skipped 再加 1

newLatest = 13:00
newEarliest = 12:40
```

也就是说 `[11:40, 12:00]` 这一窗被跳过，避免本次迟到完成与下一次可运行区间过近。

这条额外跳窗只适用于 custom flex。full-flex 的 `P==F` 不走这个分支。

### 3. 边界比较要看清

源码一边用严格小于 `<`，另一边用严格大于 `>`：

- 提前完成恰好离 L 为 `P/6`：不加 buffer；
- 晚完成 diff 恰好等于阈值：不额外跳窗。

手算边界测试时，差 1 毫秒就可能进入不同分支。

---

## 十、周期窗口不是执行承诺

最容易形成错误直觉的是变量名 `latestRunTimeElapsedMillis`。对一次性 override deadline 和 periodic window，它的业务含义不同。

JobStatus 的源码注释和 ready 逻辑明确区分：

```java
mReadyDeadlineSatisfied = !job.isPeriodic()
        && hasDeadlineConstraint() && state;
```

也就是说：

```text
一次性 Job 的 override deadline 到期
  → 可在一定范围内压过普通约束

periodic Job 的窗口 latest 到达
  → 不获得这种 deadline override
  → 仍需正常约束全部满足
```

所以 periodic Job 可能跳过某个窗口，也可能在 intended window 之后才执行。算法随后把迟到算进一个未来窗口，而不是承诺补跑。

如果业务要求“每天 00:00 必须做一条不可遗漏的结算”，不能只依赖 periodic Job 的每窗一次直觉。更可靠的做法是持久化“尚未结算的业务日期”，每次获得运行机会后补业务账；调度机会与业务完整性是两层问题。

---

## 十一、失败、成功和重新 schedule 怎样影响计数

### 1. 连续失败会增加 numFailures

失败新记录取：

```text
new.numFailures = old.numFailures + 1
```

它决定下一次 backoff 的 attempts。

### 2. 正常周期续期把计数清零

`getRescheduleJobForPeriodic()` 创建新 JobStatus 时传入：

```java
0 /* backoffAttempt */
```

因此 periodic Job 一旦成功完成，下一周期的失败链从头开始。

### 3. App 主动重新 schedule 是新一代定义

应用再次 `schedule()` 相同 jobId 会走 replacement，而不是沿旧失败 JobStatus 继续 `numFailures+1`。新 `JobStatus.createFromJobInfo()` 的失败次数从 0 开始。

这不意味着可以靠高频 schedule 规避系统策略；频繁调度本身有成本和配额限制。这里仅说明对象代际语义。

### 4. 不同停止原因不保证同样进入 backoff

约束丢失、抢占、执行超时、App 崩溃等最终都可能产生 `needsReschedule=true`，但它们到达这个布尔值的路径不同；cancel、某些启动失败则可能不重试。

因此日志分析应记录：

```text
stop reason
needsReschedule
numFailures
earliest/latest/original latest
```

只看到 `Num failures: 2`，还不能知道最初是什么原因导致两次重调度。

---

## 十二、persisted Job：重启能恢复时间门，但不能无损恢复失败代际

这部分只适用于 `setPersisted(true)` 的 Job。第 133 章的 JobWorkItem Job 在 r48 不能 persisted。

### 1. jobs.xml 保存什么

`JobStore` 会保存：

- JobInfo 的 period/flex 或一次性参数；
- 当前 JobStatus 的 delay/deadline 时间边界，转换成 wall clock；
- 非默认 backoff policy 与 initial backoff；
- `lastSuccessfulRunTime`、`lastFailedRunTime`。

因此，一个 persisted Job 已经进入“本次 backoff 等到 11:10”的状态时，当前 earliest 边界可以经 XML 在重启后近似恢复。

### 2. jobs.xml 没保存什么

r48 没有单独写入：

```text
numFailures
periodic failure 实例的 originalLatest
```

从 XML 恢复 JobStatus 的构造路径把失败次数设回 0。结果是：

- 当前已经算好的 backoff 时间门可以保留；
- 但下一次再失败时，指数级数可能重新从第一次失败计算；
- 正常周期窗口的 latest 能作为恢复后的新 anchor；
- 如果设备恰在 periodic failure 实例期间重启，该实例 latest 为 none，内存中的 originalLatest 又没有独立落盘，后续成功时可能触发“非法 anchor”防御分支并从当前时刻重新定相。

最后一点是 Android 11 r48 的实现边界，不应包装成 periodic API 的稳定承诺。它说明“Job persisted”不等于 JobStatus 的所有内部代际字段都逐字持久化。

### 3. 为什么 periodic 完成时移除旧记录不立刻删磁盘

`onJobCompletedLocked()` 对 periodic Job 调用 stop tracking 时，`removeFromPersisted=false`。随后新周期记录 `startTrackingJobLocked()` 会更新持久化状态。

设计意图是缩小这个窗口：

```text
旧周期记录从内存移除
        ↓
新周期记录尚未加入
```

如果中间 system_server 出问题，磁盘上至少还保留旧 periodic 定义，不至于因为一次换代瞬间完全丢失。它仍不是跨文件事务，也不保证所有运行态字段无损保存。

---

## 十三、怎样用 dumpsys 诊断“为什么还没重跑”

在设备上查看 JobStatus 时，按下面顺序读，不要只盯一个时间：

| 字段 | 解释 |
|---|---|
| `Num failures` | 当前内存代际的连续失败次数 |
| `earliest` | backoff 或周期窗口起点是否已到 |
| `latest` | 失败实例可能为 none；periodic 时是窗口终点 |
| `original latest` | periodic 原时间表锚点 |
| satisfied/unsatisfied constraints | 时间到了后还卡在哪项约束 |
| standby bucket / quota | 是否仍受后台和配额限制 |
| pending/running 状态 | 是否在等调度或已占槽 |
| stop reason / last failed | 最近一次为什么结束、何时结束 |

一个实用判断顺序是：

```text
先问：这次完成真的 needsReschedule=true 吗？
  ↓
再问：earliest 到了吗？
  ↓
再问：其他约束、quota 和动态限制满足吗？
  ↓
最后问：是否有可用执行槽？
```

本章只在 macOS 静态核对源码，没有声称已在真机执行 dumpsys。不同 Android 版本的输出字段和调度策略可能变化，设备问题应以对应分支为准。

---

## 十四、容易翻车的判断

### “onStopJob 返回 true，会马上再调用 onStartJob”

不对。true 只是请求失败重调度；新记录先等 backoff earliest，再等其他约束和执行槽。

### “第一次指数退避是 60 秒”

若默认 base=30 秒，第一次 attempts=1，公式是 `30×2^0=30 秒`。第二次才是 60 秒。

### “5 小时后系统就放弃这个 Job”

不对。5 小时是单次 delay 的上限，不是总寿命或最大累计重试时间。

### “原 override deadline 会让失败重试提前运行”

不对。失败 JobStatus 的 latest 被设为 `NO_LATEST_RUNTIME`。

### “periodic 每次都从实际完成时刻加 period”

不对。正常情况沿 `originalLatest` 续期，目的是保持原时间表相位。

### “错过三个周期，系统会补跑三次”

不对。算法只生成一个未来窗口，不为每个错过的窗口创建补跑实例。

### “periodic latest 到期会无视网络约束强制执行”

不对。r48 的 deadline-ready 特权明确排除 periodic Job。

### “persisted 会保存指数退避的所有失败次数”

不对。当前时间边界可落盘，但 r48 jobs.xml 不保存 `numFailures`。

---

## 十五、macOS 静态练习：把公式变成证据

以下命令都在 `/Users/ninebot/androidSource` 下只读执行。

### 练习 1：画出完成分叉表

```bash
sed -n '1832,1872p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
```

记录 `needsReschedule`、`isPeriodic()` 和创建函数。

答案：`needsReschedule=true` 总是优先创建 failure reschedule；false 且 periodic 才创建正常下一周期；false 且非 periodic 不创建新记录。

### 练习 2：手算两种 backoff

```bash
sed -n '1682,1724p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
```

设 base=20 秒，连续失败四次：

```text
LINEAR：20、40、60、80 秒
EXPONENTIAL：20、40、80、160 秒
```

再回答：第 20 次指数结果能无限增长吗？不能，最终被单次 5 小时上限截断。

### 练习 3：证明 Builder 与服务端有两层下限

```bash
rg -n "MIN_BACKOFF_MILLIS|setBackoffCriteria" \
  frameworks/base/apex/jobscheduler/framework/java/android/app/job/JobInfo.java

rg -n "MIN_LINEAR_BACKOFF_TIME|MIN_EXP_BACKOFF_TIME" \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
```

预期观察：Builder 先把请求抬到 10 秒；真正计算又与服务端动态常量取较大值。

### 练习 4：手算照片同步的下一窗口

```bash
sed -n '1752,1820p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
```

给定 `L=11:00、P=60 分钟、F=20 分钟`：

1. 10:51 完成：下一窗口 `[11:40,12:00]`；
2. 11:10 完成：diff=10，未严格大于 20 分钟阈值，下一窗口仍 `[11:40,12:00]`；
3. 11:25 完成：diff=25，大于阈值，额外跳窗，下一窗口 `[12:40,13:00]`。

### 练习 5：证明 periodic deadline 不覆盖其他约束

```bash
sed -n '1053,1064p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java

sed -n '1272,1289p' \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
```

预期观察：`mReadyDeadlineSatisfied` 带 `!job.isPeriodic()`；ready 注释也明确 periodic deadline 只是实现细节。

### 练习 6：证明失败次数没有落盘

```bash
rg -n "numFailures|lastSuccessfulRunTime|lastFailedRunTime|initial-backoff" \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobStore.java \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java
```

预期观察：JobStatus 有 `numFailures`，但 JobStore XML 只找到成功/失败时间和 backoff 配置，没有失败次数属性；从 XML 恢复的构造路径传入 0。

---

## 阅读检查题与答案

### 1. periodic Job 在 10:50 失败并请求重试，下一步先算什么？

先用 failure reschedule 算 `10:50 + backoff delay`，不是直接算下一周期窗口。

### 2. 重试在 10:51 成功，为什么下一窗口不从 10:51 加一小时？

失败实例保留了原 `originalLatest=11:00`。成功后的 periodic 算法从该锚点续期，所以得到 latest=12:00。

### 3. backoff earliest 已经过了，Job 为什么仍不执行？

earliest 只是一项时间约束；网络、充电、quota、Doze、后台限制和执行槽仍可能阻塞。

### 4. 一次性 Job 原有 override deadline，失败重试还保留吗？

不保留。失败新记录的 latest 为 `NO_LATEST_RUNTIME`，否则 deadline 可能破坏退避。

### 5. periodic Job 错过两个窗口，恢复后一定补跑两次吗？

不一定，也不是 API 的设计。r48 只计算并创建一个未来窗口，不按错过数量生成多个补跑实例。

### 6. persisted Job 重启后，numFailures 还在吗？

r48 不在 jobs.xml 中保存它。当前已计算的 earliest/deadline 可恢复，但失败级数会重置；periodic failure 的原 anchor 也可能丢失。

---

## 本章 takeaway

看到 Job 完成后的时间问题，先画这条分叉：

```text
onJobCompletedLocked(old, needsReschedule)
        │
        ├─ needsReschedule=true
        │      → now + backoff
        │      → earliest 有值，latest=none
        │      → numFailures + 1
        │
        ├─ false + periodic
        │      → 沿 originalLatest 计算未来窗口
        │      → numFailures 清零
        │
        └─ false + one-off
               → 不再创建新 JobStatus
```

最后始终补一句边界：**算出 earliest/latest 只是在时间维度上安排候选机会；是否真正执行，还要经过完整约束、配额、后台策略和并发槽。**

下一章继续追持久化边界：第 135 章分析 `JobStore` 怎样并发落盘、借助 `AtomicFile` 避免半写文件，并在重启时校正 RTC 与 elapsed realtime。
