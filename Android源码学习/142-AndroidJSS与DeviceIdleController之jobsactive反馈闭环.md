# 142 Android JSS 与 DeviceIdleController：jobs-active 反馈闭环

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 129、131、138、141 章

## 先看问题：维护窗口打开后，谁决定它可以提前关闭

Doze 对 JobScheduler 不只是单向限制。DeviceIdleController 退出 idle、打开 maintenance 后，JSS 会反向报告“还有没有需要窗口承载的 Job 工作”。DIC 把这个布尔量与 Alarm 活动、维护广播尚未完成的操作数合并，三者都空闲时才允许提前回到 idle。

但 `jobs-active` 不是正在运行的 Job 数：一个等待槽位的 pending 就能置 true；某些前台、白名单或活跃 UID 的 running Job 反而不计。它也不是无限延长窗口的租约，只阻止 early exit，状态机原定的 maintenance Alarm 仍可推进。

本章回答：**JSS 怎样计算这一个全局布尔，DIC 怎样把它并入 maintenance 退出门，以及 3 秒后台恢复、最短等待、预算 Alarm 与 WakeLock 为什么是四个不同完成点？**

## 1. 三个 active 先分清

| 名称 | 所属模块 | 含义 |
|---|---|---|
| UID active | AMS / AppStateTracker | source UID 处于前台活跃状态 |
| JSS `mReportedActive` | JobSchedulerService | 上一次准备报告给 DIC 的全局 Job 活动快照 |
| DIC `mJobsActive` | DeviceIdleController | DIC 最近一次收到的 JSS 布尔信号 |

两边经 `DeviceIdleInternal.setJobsActive(boolean)` 连接。接口由 LocalServices 在 system_server 内同步调用，不经过 Binder 驱动；这意味着没有 Parcel 或 calling identity 边界，但两端各自仍有锁和状态机。

JSS 到 `PHASE_THIRD_PARTY_APPS_CAN_START` 才取得 `mLocalDeviceIdleController`。更早调用 `reportActiveLocked()` 时接口可为 null，只更新本地缓存；启动后首次 Controller attach 与 CHECK 会重新计算。

## 2. JSS 的报告公式：pending 优先，running 有三类例外

r48 的核心公式来自 `JobSchedulerService.reportActiveLocked()`：

```java
        // active is true if pending queue contains jobs OR some job is running.
        boolean active = mPendingJobs.size() > 0;
        if (mPendingJobs.size() <= 0) {
            for (int i=0; i<mActiveServices.size(); i++) {
                final JobServiceContext jsc = mActiveServices.get(i);
                final JobStatus job = jsc.getRunningJobLocked();
                if (job != null
                        && (job.getJob().getFlags() & JobInfo.FLAG_WILL_BE_FOREGROUND) == 0
                        && !job.dozeWhitelisted
                        && !job.uidActive) {
                    // We will report active if we have a job running and it is not an exception
                    // due to being in the foreground or whitelisted.
                    active = true;
                    break;
                }
            }
        }
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

公式可压缩成：

```text
jobsActive = pending 非空
          OR 存在普通后台 running Job

普通后台 running = 非 WILL_BE_FOREGROUND
                  AND 非永久 Doze whitelist
                  AND source UID 非 active
```

pending 不套这三项过滤。它表示 Job 已通过完整 ready 门、正在等或争执行槽；若窗口立即关闭，NOT_DOZING 可能再次转负，所以即使尚未运行也先保守报告 true。等它从 pending 进入 running 后，若恰属三个例外之一，下次报告可能转回 false。

`mReportedActive` 只在真假边沿变化时调用 `setJobsActive()`，不是 Job 数计数器。schedule、cancel/replacement、Job 完成，以及每条有效 JSS Handler 消息末尾的并发分配后，都会在相应路径重新计算。

## 3. DIC 收到 true 与 false 时行为不对称

`DeviceIdleController` 的本地服务最终进入：

```java
    void setJobsActive(boolean active) {
        synchronized (this) {
            mJobsActive = active;
            if (!active) {
                exitMaintenanceEarlyIfNeededLocked();
            }
        }
    }
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java`

true 只更新占用信号，不会主动开启 maintenance、延长 Alarm 或获取 WakeLock；false 才立即检查是否可以 early exit。若 false 到达时仍有 Alarm 活动或 active idle op，状态不变；等其他计数随后归零，它们自己的回调会再次检查。

DIC 的共同空闲门是：

```java
    boolean isOpsInactiveLocked() {
        return mActiveIdleOpCount <= 0 && !mJobsActive && !mAlarmsActive;
    }
```

三项是 AND 条件：维护广播/最短等待操作已结束、JSS 不再需要窗口、AlarmManager 也无活动。`mJobsActive=false` 单独不能关窗。

## 4. maintenance 打开时为什么先放一个 active idle op

deep 或 light idle 进入 maintenance 时都会先令 `mActiveIdleOpCount=1` 并获取 `mActiveIdleWakeLock`，然后关闭 idle mode、发送 `MSG_REPORT_IDLE_OFF`。

Handler 发送 deep/light idle-mode-changed ordered broadcast 时，每发一条就 `incActiveIdleOps()`；发送动作本身完成后 `decActiveIdleOps()`，而每条 ordered broadcast 的最终 receiver 不会立即减计数，而是再延迟：

```java
            if (PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED.equals(intent.getAction())) {
                mHandler.sendEmptyMessageDelayed(MSG_FINISH_IDLE_OP,
                        mConstants.MIN_DEEP_MAINTENANCE_TIME);
            } else {
                mHandler.sendEmptyMessageDelayed(MSG_FINISH_IDLE_OP,
                        mConstants.MIN_LIGHT_MAINTENANCE_TIME);
            }
```

默认最短等待是 deep 30 秒、light 5 秒（`COMPRESS_TIME=false`），可由 device-idle constants 改写。它的用途是给 JobScheduler、AlarmManager 等接收 idle-off 通知并发布活动信号的时间，不等于整个 maintenance 窗口长度。

当最后一个 active op 归零，DIC 会检查 early exit，然后释放 `mActiveIdleWakeLock`。即使 `mJobsActive=true` 阻止了状态机提前退出，这把 DIC WakeLock 也会释放；jobs-active 自身不拥有它。正在运行的 Job 由 JobServiceContext 等机制管理自己的执行资源，二者不能混为同一把锁。

## 5. 3 秒后台恢复与“乐观 true”怎样配合

DeviceIdleJobsController 观察到离开 Doze 后：

1. 立即更新 foreground source UID 的 Job；
2. 对其他后台 Job 延迟 `BACKGROUND_JOBS_DELAY=3000ms` 再更新 NOT_DOZING；
3. 立刻通知 JSS `onDeviceIdleStateChanged(false)`。

JSS 此时若已 ready 且 `mReportedActive=false`，会先写 true 并同步调用 DIC，再投递 `MSG_CHECK_JOB`：

```java
                    if (mLocalDeviceIdleController != null) {
                        if (!mReportedActive) {
                            mReportedActive = true;
                            mLocalDeviceIdleController.setJobsActive(true);
                        }
                    }
                    mHandler.obtainMessage(MSG_CHECK_JOB).sendToTarget();
```

这是一个乐观占位：第一轮 CHECK 可能发生在后台 Job 的 3 秒延迟之前，看不到它们已解除 NOT_DOZING；先报 true 可避免 DIC 因暂时“无工作”过早关窗。3 秒后后台约束更新若发生变化，会再发 Controller CHECK，真正构造 pending。

默认 light 5 秒、deep 30 秒的 minimum 大于 3 秒，为这段传播留下余量；但两类值都可配置，源码没有把关系写成不可破坏的不变量。后续 JCM 分配并调用 `reportActiveLocked()` 后，如果确实没有 pending 或普通后台 running，乐观 true 会被纠正为 false。

## 6. jobs-active 只挡提前退出，不覆盖窗口预算

deep maintenance 开始时按 `mNextIdlePendingDelay` 安排状态机 Alarm；light maintenance 则把 `mCurLightIdleBudget` clamp 到最小/最大预算间并据此设 Alarm。Alarm 到达会推进状态机回 idle，并不先要求 `mJobsActive=false`。

因此：

```text
minimum maintenance time
  = ordered broadcast 完成后仍保留 active op 的最短响应时间

jobs-active / alarms-active
  = minimum 操作归零后，是否允许提前退出

maintenance budget / state alarm
  = 本轮窗口的上限方向
```

light 模式还会根据实际 maintenance duration 增减 reserve：少于 minimum budget 的剩余额度加回，超出则扣减，再限制在下一次窗口的 min/max 范围。deep 路径没有相同的 light reserve 公式。

这解释了一个常见误判：Job 没结束，不代表窗口永不关闭。jobs-active 可以延后 early exit，却不能取消状态机预算 Alarm；Job 到窗口结束时仍可能因 NOT_DOZING 变负而被停止或等待下次机会。

## 7. 进入 Doze 的停止口径比报告公式更粗

`onDeviceIdleStateChanged(true)` 中，JSS 遍历 active contexts，只豁免 `FLAG_WILL_BE_FOREGROUND`，其余 running Job 都请求以 `REASON_DEVICE_IDLE` 停止。它没有复用 report 公式中的 `dozeWhitelisted` 与 `uidActive` 两项过滤。

与此同时，DeviceIdleJobsController 会按永久 whitelist 或 `IMPORTANT_WHILE_FOREGROUND` + foreground/temp-whitelist 计算 NOT_DOZING。两条路径的口径并非同一个布尔表达式；某个被即时停止的 Job 仍可能在后续 CHECK 中因豁免条件 ready，再参与调度。

这属于 r48 的实现边界，不能把“running 不计 jobs-active”推导成“进入 Doze 时一定不停止”。报告窗口占用和执行资格是不同职责。

## 8. 几个时间轴场景

### 场景一：空 maintenance

idle-off ordered broadcast 结束后仍等待 minimum；最后一个 active op 归零，若 jobs 与 alarms 都 false，立即 early exit。窗口可接近 minimum，但不是由 minimum 单独定义。

### 场景二：一个普通后台 Job 等槽

它进入内部 pending 后，JSS 报 true。minimum 到点时 active op 归零，但 jobs-active 阻止 early exit；若状态机预算 Alarm 先到，窗口仍结束。

### 场景三：只有白名单 running Job

pending 已空，running Job 的 `dozeWhitelisted=true`，JSS 可报 false。minimum 与 alarms 也结束后，DIC 可 early exit；该 Job 是否继续由 NOT_DOZING 白名单与其他约束决定，而不是靠窗口反馈。

### 场景四：没有立即可见的后台 Job

JSS 先乐观报 true，第一轮扫描可能纠正 false；只要 DIC minimum active op 尚未归零，窗口仍保留到 3 秒后台更新有机会触发下一轮。若配置把 minimum 改得短于恢复延迟，保护关系会变弱。

## 9. 诊断时怎样判断两边布尔为何不一致

JSS 的 `mReportedActive` 与 DIC 的 `mJobsActive` 是跨本地服务调用两端的缓存，正常边沿调用是同步的，但 boot 早期接口为 null、转场中正在重算或 dump 时刻不同，都可能造成观察差异。

排查应同时看：

- JSS 内部 `mPendingJobs` 与 active Job 的三项例外；
- `mReportedActive`；
- DeviceIdle deep/light state、`mJobsActive`、`mAlarmsActive`、`mActiveIdleOpCount`；
- 下一次 deep/light state Alarm 与 light budget；
- DeviceIdleJobsController 的 `mDeviceIdleMode` 和 3 秒延迟消息。

不要用 PowerManager 的 idle 布尔替代完整 DIC 状态机，也不要用 `mJobsActive` 推断有多少 registered Job 或多少 context 正在执行。

## 10. API、内部实现与版本边界

- `DeviceIdleInternal`、`mReportedActive`、`mJobsActive`、active-op 与 maintenance budget 都是系统内部实现，普通 App 无 API 直接读写。
- 默认 3 秒、light 5 秒、deep 30 秒及 light 预算都属于 r48 默认配置，设备/测试设置可以改写其中一部分。
- jobs-active 是全局布尔，没有 package、UID 或 Job 数粒度，也不替代 App Standby quota、thermal restriction、batching 或并发槽位。
- maintenance 打开只令 NOT_DOZING 有机会转正，不保证所有 Job ready，更不保证立即执行。
- 静态源码只能确认状态转换顺序，不能给出具体设备的窗口长度、功耗或调度延迟。

## 11. 从源码验证反馈闭环

在 Android 11 r48 源码根目录只读执行：

1. 看 JSS 报告公式与退出 Doze 的乐观占位：

   ```bash
   sed -n '1295,1390p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 看 DIC 三项空闲门与 early exit：

   ```bash
   sed -n '3250,3300p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   sed -n '3415,3450p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   ```

3. 追 idle-off ordered broadcast、minimum delay 和 active op：

   ```bash
   sed -n '655,685p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   sed -n '1445,1515p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   ```

4. 比较 deep/light maintenance 的预算 Alarm：

   ```bash
   sed -n '2995,3080p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   sed -n '3195,3245p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
   ```

5. 验证前台立即、后台延迟 3 秒的 NOT_DOZING 更新：

   ```bash
   sed -n '130,180p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/controllers/DeviceIdleJobsController.java
   ```

## 12. 练习与参考答案

### 练习一：手算报告值

内部 pending 为空；正在运行三个 Job，分别是 WILL_BE_FOREGROUND、永久 Doze 白名单、普通后台 Job。`mReportedActive` 应为何值？

参考答案：true。前两项不计，但第三项同时满足三个普通后台条件，足以令全局布尔为 true。

### 练习二：minimum 到点

最后一个 active idle op 归零，此时 jobs-active=true、alarms-active=false。能否 early exit？

参考答案：不能，三项共同空闲门仍被 jobs-active 挡住；但 maintenance 的状态机 Alarm 以后仍可强制推进。

### 练习三：true 是否开窗

设备不在 maintenance，JSS 调用 `setJobsActive(true)`，DIC 是否立即打开窗口并获取 active-idle WakeLock？

参考答案：不会。该方法只保存布尔；窗口进入与 WakeLock 获取由 deep/light 状态机负责。

### 练习四：3 秒恢复

退出 Doze 后第一轮 CHECK 没看到后台 Job ready，为什么不能立刻认定本轮 maintenance 无 Job？

参考答案：后台 NOT_DOZING 更新故意延迟 3 秒；JSS 的乐观 true 与 DIC minimum active op 给它传播机会，稍后约束变化会触发新扫描。

### 练习五：Job 一直不结束

普通后台 Job 始终运行并持续令 jobs-active=true，是否保证 maintenance 永不结束？

参考答案：不保证。它阻止的是 early exit；deep/light 已安排的状态机预算 Alarm 仍能推进回 idle。

## 本章带走什么

JSS→DIC 的 jobs-active 是一个保守的全局反馈：pending 一律计入，running 只计算真正依赖普通后台 maintenance 的 Job。DIC 不把它当开窗命令或 WakeLock 引用，只把它与 alarms-active、active-idle-op 一起用于 early-exit 判断。

完整时间关系是：idle-off 广播提供通知，minimum op 防止接收方尚未反应就关窗，JSS 乐观 true 覆盖后台约束延迟，真实 pending/running 随后纠正反馈，而 deep/light budget Alarm 给窗口设置上限方向。把这些完成点分开，才能解释“Job 仍 active，但 maintenance 仍结束”或“没有 running Job，窗口却暂时保持”的现象。
