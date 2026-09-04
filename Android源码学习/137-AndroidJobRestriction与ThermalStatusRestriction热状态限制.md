# 137 Android JobRestriction 与 ThermalStatusRestriction：热状态限制

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；未在真机上升温或执行 thermal override
>
> 前置阅读：第 70、121、131、136 章

## 先看结论：`job.isReady()==true`，为什么高温时仍不运行

设备持续拍摄、导航或充电时发热，后台上传 Job 可能从 pending queue 消失；如果只看网络 constraint，它仍是 satisfied，`JobStatus.isReady()` 甚至仍可能返回 true。此时很容易误判为 ConnectivityController 丢事件，或让应用不断重复 schedule。

Android 11 还在 constraint 之外设置了 `JobRestriction`。r48 唯一实现 `ThermalStatusRestriction` 的最终判定可以写成：

```text
overall restricted
= thermal status >= SEVERE
  && Job 声明了 connectivity constraint
  && evaluated priority < PRIORITY_FOREGROUND_APP(30)
```

命中后，未运行 Job 不再进入 pending；已经 active 的 Job 会按正常 stop 状态机收到 thermal stop 请求。温度恢复只会重新打开这一层门，不能保证任务立刻运行。

读完本章，你应当能分清 constraint、restriction 与 concurrency 三层；能解释热状态从 `ThermalManagerService` 到 JSS 重扫的异步链；也能从 dump 判断“raw thermal 命中”与“考虑优先级后的最终限制”为什么可能不同。

本章不重讲 Thermal HAL 的版本适配、硬件降频算法或各厂商温控参数，只追 Android 11 r48 的 JobScheduler 决策。

## 1. Constraint、Restriction、Concurrency 是三道不同的门

同一个“Job 没运行”现象，可能来自三个不同层级：

| 层级 | 回答的问题 | 主要数据/入口 |
|---|---|---|
| Constraint | Job 声明和系统动态条件是否满足 | `JobStatus.isReady()`、各 `StateController` |
| Restriction | 系统当前政策是否仍允许这个 ready Job 运行 | `JobSchedulerService.checkIfRestricted()` |
| Concurrency | 通过前两层后，有限槽位分给谁 | `JobConcurrencyManager` |

`ThermalStatusRestriction` 不继承 `StateController`，不跟踪每个 Job，也不修改 required/satisfied constraint bit。它只保存一个全局热限制布尔值；JSS 每次把具体 `JobStatus` 传进来即时判断。

因此这组状态完全合法：

```text
job.isReady() == true
checkIfRestricted(job) != null
isReadyToBeExecutedLocked(job) == false
```

这不是状态矛盾，而是前一道门通过、后一道门拒绝。

## 2. 抽象层的 boolean 注释在 r48 中写反了

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/restrictions/JobRestriction.java`

`JobRestriction` 把一项政策抽象为初始化、逐 Job 判断、固定 stop reason 和 dump。问题在于 `isJobRestricted()` 的 Javadoc 声称 false 表示“不应调度”，与方法名和真实调用方相反：

```java
/**
 * @param job to be checked
 * @return false if the {@link JobSchedulerService} should not schedule this job at the moment,
 * true - otherwise
 */
public abstract boolean isJobRestricted(JobStatus job);
```

实际消费者写得很明确：

```java
for (int i = mJobRestrictions.size() - 1; i >= 0; i--) {
    final JobRestriction restriction = mJobRestrictions.get(i);
    if (restriction.isJobRestricted(job)) {
        return restriction;
    }
}
return null;
```

所以 r48 的可执行语义是 **true = restricted**。审计抽象接口时不能只读注释，必须同时看至少一个实现和全部调用点。JSS 构造器在这个版本只加入一个 `ThermalStatusRestriction`；抽象列表虽可扩展，不能反推 r48 已有其他 restriction。

## 3. 热状态如何从传感器汇成 JSS 的一个布尔值

`ThermalManagerService` 缓存各热源的 `Temperature`，正常情况下取其中最高 status 作为全局值：

源码路径：`frameworks/base/services/core/java/com/android/server/power/ThermalManagerService.java`

```java
private void onTemperatureMapChangedLocked() {
    int newStatus = Temperature.THROTTLING_NONE;
    final int count = mTemperatureMap.size();
    for (int i = 0; i < count; i++) {
        Temperature t = mTemperatureMap.valueAt(i);
        if (t.getStatus() >= newStatus) {
            newStatus = t.getStatus();
        }
    }
    if (!mIsStatusOverride) {
        setStatusLocked(newStatus);
    }
}
```

这几行证明两点：任一已上报热源达到更高等级都可能抬高全局 status；shell override 生效时，传感器表更新不会覆盖被锁定的测试值。

JSS 在 `PHASE_SYSTEM_SERVICES_READY` 调用每个 restriction 的 `onSystemServicesReady()`。Thermal 实现取得 `PowerManager` 并注册 listener：

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/restrictions/ThermalStatusRestriction.java`

```java
mPowerManager.addThermalStatusListener(new OnThermalStatusChangedListener() {
    @Override
    public void onThermalStatusChanged(int status) {
        final boolean shouldBeActive =
                status >= PowerManager.THERMAL_STATUS_SEVERE;
        if (mIsThermalRestricted == shouldBeActive) {
            return;
        }
        mIsThermalRestricted = shouldBeActive;
        mService.onControllerStateChanged();
    }
});
```

状态共有 NONE、LIGHT、MODERATE、SEVERE、CRITICAL、EMERGENCY、SHUTDOWN 七档。Restriction 只保留“是否达到 SEVERE”的 `volatile boolean`：NONE～MODERATE 为 false，SEVERE～SHUTDOWN 为 true。同一侧内部变化，例如 SEVERE→CRITICAL，不会通知 JSS 重扫；只有跨过 SEVERE 边界才会。

这层自己没有温度阈值、时间 debounce 或迟滞回线。它依赖上游 HAL/TMS 给出的离散 status；源码不能证明具体设备在多少摄氏度进入 SEVERE。

## 4. 注册后的当前状态也是异步送达的

`PowerManager.addThermalStatusListener(listener)` 默认使用 `Context.getMainExecutor()`。服务端注册成功后，`ThermalManagerService.registerThermalStatusListener()` 会立即调用 `postStatusListener(listener)`，但这个“立即”指发起异步投递：服务端先 post 到 `FgThread`，PowerManager 的 listener wrapper 再提交给 MainExecutor。

完整顺序是：

```text
Thermal HAL/缓存变化
    → ThermalManagerService 计算全局最高 status
    → FgThread 投递 IThermalStatusListener 通知
    → PowerManager wrapper 清 Binder calling identity
    → Context MainExecutor 执行 ThermalStatusRestriction listener
    → 更新 volatile boolean
    → JobSchedulerService.onControllerStateChanged()
    → JSS Handler 收到 MSG_CHECK_JOB，在 mLock 下重扫
```

`IThermalStatusListener` 是 Binder 接口边界，但 JSS 与 ThermalManagerService 同在 `system_server` 时可能走本地 Binder 对象，不能武断地写成“一定切到 Binder 线程”。可以确定的异步边界是 TMS 的 FgThread post 与 PowerManager 的 Executor 提交。

注册返回到首次状态回调之间存在窗口，初值暂时是 false。如果回调到达时 JSS 尚未进入 `mReadyToRock`，Handler 可能丢掉那次早期重扫；但 volatile 状态已经保存，后续正式扫描仍会读取它。丢的是一条触发消息，不是热状态本身。

## 5. 真正公式为何只限制部分网络 Job

Thermal restriction 自己的 raw 公式只有两个条件：

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/restrictions/ThermalStatusRestriction.java`

```java
public ThermalStatusRestriction(JobSchedulerService service) {
    super(service, JobParameters.REASON_DEVICE_THERMAL);
}

@Override
public boolean isJobRestricted(JobStatus job) {
    return mIsThermalRestricted && job.hasConnectivityConstraint();
}
```

它不检查网络 transport、估算字节数、是否充电、屏幕状态、periodic/persisted 属性，也不监控 JobService 实际打开的 socket。ANY、UNMETERED、NOT_ROAMING 或自定义 NetworkRequest 只要形成 required connectivity bit，SEVERE+ 时 raw 判断都一样；反过来，业务私自联网但没有声明 required network，Restriction 看不见。

为什么只挑声明网络的 Job？源码可以确定这是 r48 选中的粗粒度延后集合，却没有注释或测量数据足以证明具体功耗收益。本章只陈述机制，不编造节能百分比。

## 6. evaluated priority 是第三个条件

raw restriction 并不是最终结论。JSS 先用第 136 章的 evaluated priority 做统一豁免：

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

```java
private JobRestriction checkIfRestricted(JobStatus job) {
    if (evaluateJobPriorityLocked(job) >= JobInfo.PRIORITY_FOREGROUND_APP) {
        return null;
    }
    for (int i = mJobRestrictions.size() - 1; i >= 0; i--) {
        final JobRestriction restriction = mJobRestrictions.get(i);
        if (restriction.isJobRestricted(job)) {
            return restriction;
        }
    }
    return null;
}
```

`PRIORITY_FOREGROUND_APP` 在 r48 等于 30。`evaluateJobPriorityLocked()` 会综合 Job 原始 priority、source UID 的进程状态 override 与 `JobPackageTracker` 的负载 adjustment。

例如当前基础/override priority 为 35，package load 达到 moderate 后减 40，evaluated priority 变为 -5，于是失去 `>=30` 的统一豁免；若设备 SEVERE 且 Job 声明网络，最终就被限制。TOP_APP priority 40 则先跳过负载 adjustment，又在这里直接返回 null，所以不会被这项 Framework JobRestriction 阻止。

高优先级豁免只绕过 Framework 的 JobScheduler 政策，不能绕过内核、固件或 HAL 已实施的降频，更不能把 SEVERE 误解成设备已恢复正常性能。

## 7. SEVERE 到来后，pending 与 active 走两条路

listener 不直接操作 Job。它调用名字略显历史化的 `onControllerStateChanged()`，该方法只向 JSS Handler 投递 `MSG_CHECK_JOB`。Handler 取得 `mLock` 后，无论走 full queue 还是带 batching 的 maybe queue，都会先：

```text
1. 对旧 mPendingJobs 逐项 noteNonpending，再清空
2. 检查所有 active Job 是否仍可继续
3. 重扫 JobStore，只把通过全部门的 Job 放回 pending
4. 尝试把 pending 分配到空闲执行槽
```

因此被限制但尚未运行的 Job 只是退出 pending 候选，仍留在 `JobStore`；它不会收到 `onStopJob()`，也不会产生 thermal stop reason。第 136 章的 PackageTracker pending 时长会在清队列时闭合，不会把“因 thermal 留在 JobStore 等待”的时间继续算作 pending 压力。

已经 active 的 Job 则由 `stopNonReadyActiveJobsLocked()` 判断。纯 thermal 情况下，Job 自身 constraint 仍 ready，代码进入 restriction 分支：

```java
final JobRestriction restriction = checkIfRestricted(running);
if (restriction != null) {
    final int reason = restriction.getReason();
    serviceContext.cancelExecutingJobLocked(reason,
            "restricted due to "
                    + JobParameters.getReasonCodeDescription(reason));
}
```

Thermal restriction 构造时固定绑定 `REASON_DEVICE_THERMAL`。请求 stop 后，`JobServiceContext` 还要经历向应用发送 stop、等待 ack、超时或进程死亡，再进入 cleanup；**热状态变化、发出 stop、槽位释放是三个不同完成点**。应用从 `onStopJob()` 返回是否重试的语义不变，但温度仍为 SEVERE 时，新候选仍会被挡住。

## 8. 同时失败多个条件时，只选择一个主停止原因

运行中 Job 的停止顺序是：

1. 先判断 `running.isReady()`；
2. 若已不 ready，RESTRICTED bucket 的 dynamic constraint 不满足时使用 `REASON_RESTRICTED_BUCKET`，否则使用 `REASON_CONSTRAINTS_NOT_SATISFIED`；
3. 只有 Job 自身仍 ready，才调用 `checkIfRestricted()` 并使用第一项命中 restriction 的固定 reason。

所以“断网”和“进入 SEVERE”同时发生时，网络 Job 往往记录 constraints，而不是 thermal。系统没有把所有并发原因组成集合传给应用；这里是按 if/else 优先级选择一个主因。

API 边界也必须说清：r48 的 `JobParameters.REASON_DEVICE_THERMAL`、`getStopReason()` 与 `getDebugStopReason()` 都标记为 `@hide`。普通 SDK 应用可以实现公开的 `JobService.onStopJob()` 并决定是否重试，但不能把读取内部 stop reason 当作 Android 11 公开 API 合同。Framework、系统应用或诊断工具能在源码内部使用这些字段。

## 9. 温度恢复为什么不等于任务立即运行

SEVERE→MODERATE 会把布尔值从 true 变 false，再投递一次 `MSG_CHECK_JOB`。由于 restriction 没有修改每个 Job 的 constraint bit，也没有持有“被限制 Job 列表”，恢复时无需逐项翻位；下一轮即时判断自然放行。

但放行之后仍要通过：

- ConnectivityController 对网络能力与 UID policy 的判断；
- quota、Doze、后台限制、用户与组件状态；
- 非 ACTIVE Job 的 batching 策略；
- 执行槽容量与并发竞争。

因此解除 thermal 只打开一扇门。高温期间 Job 既不 pending 也不 active，旧 PackageTracker load 还可能随批次退出，使恢复后的 evaluated priority 与进入限制前不同；这是两个模块通过 JSS 状态机形成的间接反馈，不是 Thermal restriction 主动清空历史。

## 10. dumpsys 为什么可能同时出现 true 和 false

文本全局区域输出：

```text
In thermal throttling?: true
```

这个名字并不精确：字段实际表示 `status >= SEVERE`，LIGHT/MODERATE 虽然也是 PowerManager 定义的 throttling 等级，这里仍打印 false。更准确的读法是“Job thermal restriction 的全局开关是否开启”。

每个 registered Job 的文本 `Restricted due to:` 先调用 `checkIfRestricted()`，因此包含 priority 豁免。Proto 则同时写两层：

```text
RegisteredJob.IS_JOB_RESTRICTED
    = checkIfRestricted(job) != null        // 最终结果

RegisteredJob.RESTRICTIONS[].IS_RESTRICTING
    = restriction.isJobRestricted(job)      // raw 结果
```

于是 SEVERE 下的高优先级网络 Job 可以出现 raw thermal=true、overall restricted=false。诊断工具若只展示 raw 数组就会误报；应优先看 overall 字段，再结合 evaluated priority。

热状态不会写入 jobs.xml，Restriction 也没有历史账本。若要解释一次过去的 active stop，可以交叉看 JobPackageTracker 的 stop reason/event、BatteryStats 或其他系统遥测；未启动就被挡住的 Job 不会留下 inactive stop event。第 136 章还指出 r48 event history 的 STOP/STOP-P 标签选反，不能仅凭那个标签判断周期属性。

## 11. 版本与实现边界

- 以上结论只针对 Android 11 `android-11.0.0_r48`；后续版本可能增加 restriction、改变阈值或公开 stop reason API。
- `PowerManager.getCurrentThermalStatus()` 与 thermal listener 是应用可见的公开能力；`JobRestriction`、JSS 的豁免公式和 `JobParameters` stop reason 在 r48 属于 Framework 内部实现。
- TMS 给 restriction 的只有聚合 status，JSS 不知道是 CPU、GPU、skin 还是 battery 触发最高等级。
- Thermal HAL 无有效数据时，TMS 初始 status 为 NONE，restriction 会保持 false；“没有限制”可能表示没有达到 SEVERE，也可能表示平台没有提供有效状态，源码本身无法替设备证明传感器链健康。
- `adb shell cmd thermalservice override-status STATUS` 会锁定全局 status，能用于未来设备验证，但那是 shell 测试输入，不等于真实温度变化；结束后必须用 `reset` 恢复。本文没有执行该命令。
- 匿名 listener 没有保存在字段中，也没有 unregister 路径；这符合 JSS 与 system_server 同生命周期的使用方式，不应复制成可反复 start/stop 组件的通用模板。

## 12. 从源码验证热限制主线

在 Android 11 r48 源码根目录执行以下只读步骤：

1. 核对 r48 restriction 列表与启动时点：

   ```bash
   rg -n 'mJobRestrictions|onSystemServicesReady' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 核对 raw 公式与 SEVERE 边界：

   ```bash
   sed -n '35,75p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/restrictions/ThermalStatusRestriction.java
   ```

3. 追 overall priority gate 与 active stop 原因：

   ```bash
   sed -n '1985,2050p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

4. 对照 TMS 聚合、注册后首次异步通知和 PowerManager MainExecutor wrapper：

   ```bash
   sed -n '155,225p' frameworks/base/services/core/java/com/android/server/power/ThermalManagerService.java
   sed -n '400,415p' frameworks/base/services/core/java/com/android/server/power/ThermalManagerService.java
   sed -n '1920,1970p' frameworks/base/core/java/android/os/PowerManager.java
   ```

5. 最后查看文本与 Proto dump 的生成代码，分别标出 raw 与 overall：

   ```bash
   sed -n '3180,3210p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   sed -n '3370,3410p' frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

预期证明的是调用方向和 r48 语义；这些静态命令不能证明某台设备的温度阈值、HAL 上报质量或实际节能效果。

## 13. 练习与参考答案

### 练习一：手算最终限制

分别判断以下 Job 是否被 thermal restriction 阻止：

1. status=MODERATE，network=true，evaluated priority=0；
2. status=SEVERE，network=false，evaluated priority=0；
3. status=SEVERE，network=true，evaluated priority=20；
4. status=CRITICAL，network=true，evaluated priority=40。

参考答案：只有第 3 个被阻止。第 1 个没跨 SEVERE；第 2 个没有 required connectivity；第 4 个通过 `>=30` 的外层优先级豁免。

### 练习二：判断停止原因

一个 active 的 required-network Job 同时断网并进入 SEVERE，`running.isReady()` 已因网络 constraint 变成 false。本次优先使用 thermal 还是 constraints reason？

参考答案：使用 `REASON_CONSTRAINTS_NOT_SATISFIED`；若同时命中 RESTRICTED bucket dynamic constraint 分支，则该分支更先使用 `REASON_RESTRICTED_BUCKET`。只有 `running.isReady()` 仍为 true 时才检查 thermal restriction。

### 练习三：解释 Proto 的“矛盾”

SEVERE 下，一个 required-network Job 的 raw `RESTRICTIONS[].IS_RESTRICTING=true`，但 overall `IS_JOB_RESTRICTED=false`。最可能缺少哪项信息？

参考答案：evaluated priority。raw 只执行 `thermal && connectivity`；overall 先检查 priority，若 `>=30` 就直接豁免。

### 练习四：指出公开 API 边界

普通 Android 11 SDK 应用能否依赖 `JobParameters.getStopReason()==REASON_DEVICE_THERMAL` 来处理重试？

参考答案：不能把它当公开 SDK 合同；r48 的 reason 常量和 getter 都是 `@hide`。应用可以通过公开的 `onStopJob()` 返回值请求重试，也可以使用公开 PowerManager thermal API观察总体状态，但两者不等于读取 JSS 内部主停止原因。

## 本章带走什么

Android 11 的 thermal JobRestriction 不是温度 constraint bit，而是 Job 自身 ready 之后的额外政策门：TMS 把各热源汇总为全局 status，SEVERE+ 打开一个布尔开关；JSS 再结合 required connectivity 与动态计算的 evaluated priority 得出最终结果。命中时，pending 候选被撤下，active Job 走正常 stop/ack/cleanup；解除时也只是重新允许竞争。把 raw 热命中、overall restriction 和最终槽位分配分开看，才不会把高温下的“不运行”误诊为网络约束故障。
