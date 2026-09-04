# 145 Android AlarmManagerService：调度总体链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 25、124、142、143 章

## 先看问题：应用设置一万个 Alarm，kernel 为什么通常只需要两个近期截止点

AlarmManagerService 把应用的 RTC/elapsed、exact/window/repeating 请求统一换算到 elapsed 时间轴，再经权限、Doze、App Standby、background restriction 和 non-wakeup 延迟政策，组织成可合并的 Batch。Java 内存可以保存大量逻辑 Alarm，写给 native 的通常只是最近 wakeup 与最近 non-wakeup 两个截止点。

timerfd 到点只表示 system_server 应重新检查；AlarmThread 还要取出 Batch、重新安排 repeating、执行政策延期、排序投递，并用 InFlight/WakeLock 等待 PendingIntent 或 listener 完成确认。

本章回答：**一个 `set*()` 请求从 API 时间语义到 kernel timerfd、再到应用回调和完成账，在哪些位置被转换、合并、推迟或失败？**

## 1. 四种类型包含两条独立语义轴

| 类型 | 输入时间轴 | 到点能否唤醒设备 |
|---|---|---|
| `RTC_WAKEUP` | wall clock | 是 |
| `RTC` | wall clock | 否 |
| `ELAPSED_REALTIME_WAKEUP` | elapsed realtime | 是 |
| `ELAPSED_REALTIME` | elapsed realtime | 否 |

RTC 表示日历时刻，会受手工校时或网络校时影响；elapsed 表示本次 boot 已经过的时间，包含设备休眠，不随 wall clock 改动。两者都不同于深睡不累计的 uptime。

“每天 8 点”通常需要 RTC 语义；“30 分钟后重试”通常应基于 elapsed。`WAKEUP` 只表示到点可唤醒 SoC，不保证绕过 Doze/standby、立即启动组件或让应用无限执行。

## 2. 所有公开 set 变体最终编码成同一组参数

`set()`、`setWindow()`、`setExact()`、`setRepeating()`、`setAndAllowWhileIdle()`、`setExactAndAllowWhileIdle()` 与 `setAlarmClock()` 最终进入 `AlarmManager.setImpl()`，再经 `IAlarmManager.set()` 传递：

```text
type + triggerAtMillis + windowMillis + intervalMillis + flags
PendingIntent 或 IAlarmListener
WorkSource + AlarmClockInfo + callingPackage
```

普通 `set()` 对 target SDK 19 及以上使用 `WINDOW_HEURISTIC=-1`；旧兼容应用可保持 exact。`setExact()` 使用 `WINDOW_EXACT=0`，正 window 是显式 `[trigger, trigger+window]`。

Android 11 的 repeating 只能使用 PendingIntent；Binder 入口若 `interval!=0 && directReceiver!=null` 直接抛 `IllegalArgumentException`。listener 若要周期行为，应在回调后重新设置 one-shot，而不是假设服务端 repeat。

## 3. Binder 边界先校验归因，再重写特权 flags

服务端从 Binder 取得真实 calling UID，并用 `mAppOps.checkPackage(callingUid, callingPackage)` 防止冒用包名。非空 WorkSource 要求 `UPDATE_DEVICE_STATS`，它改变电量/WakeLock 归因，不是普通应用可随意指定的字段。

外部调用者传入的 `WAKE_FROM_IDLE` 与 `ALLOW_WHILE_IDLE_UNRESTRICTED` 会先被清掉；非 system UID 的 `IDLE_UNTIL` 也被清掉。服务端再依据 alarm clock、core/SystemUI/白名单身份重新赋予可信 flags。

exact 请求还会自动成为 standalone：

```java
            // If this is an exact time alarm, then it can't be batched with other alarms.
            if (windowLength == AlarmManager.WINDOW_EXACT) {
                flags |= AlarmManager.FLAG_STANDALONE;
            }
```

源码路径：`frameworks/base/services/core/java/com/android/server/AlarmManagerService.java`

因此 exact 的成本不只是窗口宽度为零，还显式禁止与普通 Batch 合并。

## 4. PendingIntent 与 listener 是两种完成协议

内部要求 operation 与 directReceiver 恰好一个非空；都空或同时存在时只记警告并 return，这是为旧版本静默失败行为保留的兼容边界。

listener 在客户端由 `ListenerWrapper extends IAlarmListener.Stub` 包装。`IAlarmListener.doAlarm()` 是 oneway Binder；wrapper 收到后 post 到指定 Handler，未指定时使用应用主线程 Handler。用户 `onAlarm()` 在 `finally` 中调用 `IAlarmCompleteListener.alarmComplete()`，即使回调抛异常也尽量回执 system_server。

服务端对 listener binder 注册 death recipient；无法 link 或投递时直接放弃。PendingIntent 则通过 `send(..., OnFinished, handler, ...)` 获得发送完成回调。

两条路径都不是“AlarmThread 直接执行应用 Java 方法”。

## 5. 输入时间怎样固定成 elapsed 窗口

`setImpl()` 先做归一化：

- window 超过半天被视为可疑并钳到 1 小时；
- repeating interval 钳到 `MIN_INTERVAL..MAX_INTERVAL`；
- 非法 type 抛异常；
- 负 trigger 先改成 0；
- 非 core UID 的最终 trigger 至少为 `nowElapsed + MIN_FUTURITY`，r48 默认 5 秒。

RTC 类型通过当时 `wallNow-elapsedNow` 偏移换成 `nominalTrigger`，随后所有 Batch 端点都使用 elapsed。窗口分三类：

```text
window = 0   → maxElapsed = triggerElapsed
window > 0   → maxElapsed = triggerElapsed + window
window < 0   → maxTriggerTime() 计算一次 heuristic 上界并固定窗口
```

heuristic 对“距触发还有多久”或 repeat interval 取 75%，低于 10 秒时不 fuzz。窗口在设置时固定，时间接近时不会不断收缩。

## 6. 每 UID 上限与 replacement 有一个顺序陷阱

r48 默认每 UID 最多 500 个并发已登记 Alarm，配置不能调得更低。外层在进入 `setImplLocked(type,...)` 前检查：

```java
            if (mAlarmsPerUid.get(callingUid, 0) >= mConstants.MAX_ALARMS_PER_UID) {
```

而按相同 PendingIntent/listener 删除旧 Alarm 的 `removeLocked()` 位于更深一层、start-mode 检查之后。因此 UID 已达到 500 时，即使请求本意只是 replacement，也会先抛 `IllegalStateException`，旧 Alarm 保持原状。

未达到上限时，AMS 先问 `isAppStartModeDisabled()`；disabled 直接 return，也不会移除旧 Alarm。只有这些检查通过后，才按相同目标删除旧记录、增加计数并插入新 Alarm。

所以“相同 PendingIntent 的 set 会替换旧 Alarm”成立，但不是无条件、无失败窗口的原子更新承诺。

## 7. Batch 表示窗口交集

每个 Alarm 有 `[whenElapsed, maxWhenElapsed]`。Batch 可接纳新 Alarm 的条件是区间有交集：

```java
        boolean canHold(long whenElapsed, long maxWhen) {
            return (end >= whenElapsed) && (start <= maxWhen);
        }
```

加入后：

```text
batch.start = max(所有 whenElapsed)
batch.end   = min(所有 maxWhenElapsed)
```

`start` 是所有成员都已经合法触发的最早共同点，所以 Batch 到 start 就能整批交付。若加入新成员使 start 后移，Batch 会从按 start 排序的 `mAlarmBatches` 中移除再插入。

例如 A=[10,30]、B=[20,40] 可合并成 Batch=[20,30]；C=[31,50] 与该 Batch 无交集，另建一批。standalone Alarm 直接跳过 coalesce，每个自成 Batch。

## 8. 政策容器位于 Batch 前后不同位置

`setImplLocked(Alarm)` 若存在 `mPendingIdleUntil`，且新 Alarm 没有 allow-while-idle/wake-from-idle 等豁免，会直接放入 `mPendingWhileIdleAlarms`，暂不进入 Batch。

允许进入正常集合的 Alarm 先按 App Standby bucket 调整 `whenElapsed/maxWhenElapsed`，再 `insertAndBatchAlarmLocked()`。因此 Batch 看到的可能已经不是应用原始窗口；Alarm 另保留 expected 时间用于 quota/parole 恢复。

Batch 到点后 `triggerAlarmsLocked()` 仍可能二次延期：

- allow-while-idle 距上次交付太近，改到最小间隔后重新 set；
- background restricted，移入 `mPendingBackgroundAlarms`；
- 屏灭且本轮没有 wakeup，non-wakeup 可并入 `mPendingNonWakeupAlarms` 延迟交付。

“kernel timer 到点”因此不是应用回调的最终保证，只是政策管线再次运行的触发。

## 9. 大量 Java Alarm 怎样压成两个近期 kernel 时间

`rescheduleKernelAlarmsLocked()` 找：

1. 最早包含 wakeup 的 Batch.start，写 `ELAPSED_REALTIME_WAKEUP`；
2. 若全局首 Batch 不是该 wakeup Batch，再把首个 non-wakeup start 写 `ELAPSED_REALTIME`；
3. 若已有延期 non-wakeup，取它与上述 non-wakeup 的更早值。

所以 Java 维护全部逻辑 Alarm/Batch，kernel 通常只知道“下一 wakeup”和“下一 non-wakeup”。native 仍创建多个 timerfd 映射 Android alarm 类型，并额外使用 cancel-on-set fd 观察墙钟变化；Java 正常调度路径已经统一到两个 elapsed 类型。

native 使用绝对 `timerfd_settime(..., TFD_TIMER_ABSTIME, ...)`。由于 timerfd 把全零当作 disarm，源码把 0 截止点替换成 1ns：

```cpp
    if (!ts->tv_nsec && !ts->tv_sec) {
        ts->tv_nsec = 1;
    }
```

源码路径：`frameworks/base/services/core/jni/com_android_server_AlarmManagerService.cpp`

`epoll_wait()` 返回的多个 fd 被合成为 bit mask；time-change fd 因 `ECANCELED` 置专用标志。AlarmThread 检测 wall clock 与 expected 相差至少约 1 秒时，重建 RTC Batch、重设 TIME_TICK/DATE_CHANGED 并广播时间变化。

## 10. AlarmThread 取出 Batch 后怎样处理 repeat

AlarmThread 长期阻塞于 native `waitForAlarm()`。返回后持 `mLock` 从按 start 排序的头部连续取出 `start<=nowElapsed` 的 Batch，再逐 Alarm 执行 allow-while-idle/background 检查。

repeating Alarm 若设备睡过多个 interval，会计算：

```text
count = 1 + floor((now - expectedWhenElapsed) / repeatInterval)
next  = expectedWhenElapsed + count * repeatInterval
```

本次 PendingIntent 收到 `EXTRA_ALARM_COUNT=count`，服务端同时按原 expected 相位安排下一次，而不是简单从“实际交付时刻+interval”开始。这减少长期相位漂移。

触发列表按 delivery generation 排序：TIME_TICK 优先，其次 wakeup，最后普通 non-wakeup；同一优先级再按 nominal delivery time。一个包在同代有多项时使用其中最高优先级作为 package class。

## 11. WakeLock 覆盖投递确认，而非应用任意后台工作

`deliverAlarmsLocked()` 仍持 AMS `mLock`，逐项调用 DeliveryTracker。只有 PendingIntent send 或 listener `doAlarm()` 成功发出后，才建立 InFlight、增加 `mBroadcastRefCount`；第一项成功投递获取 `*alarm*` partial WakeLock，并向 DeviceIdleInternal 报 alarms active。

无法发送的 canceled PendingIntent 或不可达 listener 在建立 InFlight 前 return，不增加引用，也不等待不存在的完成回调。

完成路径是：

```text
PendingIntent OnFinished
或 listener alarmComplete
或 listener timeout（默认 5 秒）
        ↓
移除对应 InFlight、更新统计、refCount--
        ↓ refCount==0
报告 alarms inactive、释放 WakeLock
```

还有 InFlight 时，WakeLock WorkSource 重归因给队头。WakeLock 保证 system_server/交付握手不在中途睡眠，不代表接收方随后启动的任意异步线程都由它持续保护。

这也是第 142 章 DIC 三项空闲门中的 `mAlarmsActive` 来源：它与 jobs-active、active idle ops 并列，只影响 maintenance early exit，不等同于 Alarm 数量。

## 12. 无 timerfd 时的 Handler fallback 不是等价替代

若 native alarm driver 初始化失败，AMS 不启动 AlarmThread，而用 `Handler.sendMessageAtTime(ALARM_EVENT, when)`。Handler 时间基准是 uptime，且不能像 wakeup timerfd 那样从 suspend 唤醒设备。

所以 fallback 可保住设备清醒时的基本调度，却不能被描述为 wakeup/elapsed 语义完全等价。源码也明确记录 “Failed to open alarm driver. Falling back to a handler.”

## 13. 启动、重启与选型边界

AlarmManagerService 启动时初始化 native fd、Handler、WakeLock 和系统 Alarm；有 driver 才启动 AlarmThread。在 `PHASE_SYSTEM_SERVICES_READY` 才启动常量观察、接入 DeviceIdle/AppStandby/AppStateTracker，并安排 TIME_TICK/DATE_CHANGED。

普通应用 Alarm 是 system_server 内存状态，不像 persisted Job 那样写 JobStore；设备重启后不会自动恢复。应用若需要跨重启工作，通常要在 BOOT_COMPLETED 后重建 Alarm，或使用满足业务语义的 persisted Job。

Alarm 适合具体时刻、低延迟或用户可见 alarm clock；JobScheduler 适合约束驱动、可批处理的后台任务。二者都受系统政策影响，不应只按“哪个更准”选择。

## 14. 从源码验证端到端链路

在 Android 11 r48 源码根目录只读执行：

1. 从 API 汇聚到 Binder flag 重写：

   ```bash
   rg -n 'setImpl\(|WINDOW_EXACT|WINDOW_HEURISTIC' \
     frameworks/base/core/java/android/app/AlarmManager.java
   sed -n '2070,2150p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   ```

2. 读时间归一化、上限与 replacement 顺序：

   ```bash
   sed -n '1680,1850p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   ```

3. 手算 Batch 交集与 kernel 两个近期值：

   ```bash
   sed -n '665,725p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   sed -n '940,1000p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   sed -n '3035,3075p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   ```

4. 追 timerfd、epoll 与 time-change bit：

   ```bash
   sed -n '95,130p' \
     frameworks/base/services/core/jni/com_android_server_AlarmManagerService.cpp
   sed -n '175,210p' \
     frameworks/base/services/core/jni/com_android_server_AlarmManagerService.cpp
   ```

5. 追触发、repeat 与 InFlight 完成：

   ```bash
   sed -n '3490,3620p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   sed -n '4535,4820p' \
     frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
   ```

## 15. 练习与参考答案

### 练习一：Batch 交集

A=[10,30]、B=[20,40]、C=[25,27] 能否同批，最终 start/end 是多少？

参考答案：可以，交集为 [25,27]；到 25 时三项都已到各自最早合法时刻。

### 练习二：replacement 上限

UID 已有 500 个 Alarm，再用相同 PendingIntent set 新时间。旧项会先被删除吗？

参考答案：不会。上限检查早于 `removeLocked()`，请求先抛异常，旧 Alarm 保持。

### 练习三：sleep 后 repeat

expected=100、interval=10，系统到 135 才处理。本次 count 与 next 是多少？

参考答案：`count=1+floor(35/10)=4`，`next=100+4×10=140`，保持原相位。

### 练习四：kernel 到点

wakeup timerfd 已触发，是否证明应用回调马上执行？

参考答案：不能。Alarm 仍可能被 allow-while-idle 频率、background restriction 或交付失败等分支推迟/丢弃。

### 练习五：listener 不 complete

direct listener 收到 oneway 回调后不回 `alarmComplete()`，alarm WakeLock 是否永久持有？

参考答案：不会；r48 为 listener 安排默认 5 秒 timeout，超时移除 InFlight 并递减引用。PendingIntent 使用自己的 OnFinished 协议。

## 本章带走什么

AlarmManager 的端到端模型是：API 把时钟、窗口和目标编码进 Binder 请求；服务端验证身份、重写 flags、统一为 elapsed 窗口，经过政策调整后用窗口交集形成 Batch；Java 再把大量 Batch 压成近期 wakeup/non-wakeup 截止点交给 timerfd。

kernel 到点后，AlarmThread 只是重新开始政策与投递阶段：allow-while-idle、background 和 non-wakeup 仍可延期；repeat 按 expected 相位补 count 并安排下一代；成功发出的 PendingIntent/listener 才进入 InFlight，由完成回调或 timeout 结束 WakeLock 引用。每一层都解决不同问题，任何单个“set 成功”“timer 到点”或“回调已发出”都不是整条链的最终完成。
