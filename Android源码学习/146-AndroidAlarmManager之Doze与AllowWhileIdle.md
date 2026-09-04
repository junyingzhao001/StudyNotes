# 146 Android AlarmManager：Doze 与 AllowWhileIdle

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 25、129、142、145 章

---

## 1. 本章只回答一个问题

假设应用设置了两枚相同时间到期的 Alarm：

- A 是普通 `setExact()`；
- B 是 `setExactAndAllowWhileIdle()`。

设备随后进入 deep Doze。A 为什么会被挂起，B 为什么仍可能运行，却又可能被推迟到九分钟以后？

答案不是“Doze 关闭了 Alarm”或“exact 一定准时”，而是一条完整政策链：

```text
DeviceIdleController 设置 IDLE_UNTIL
  → AlarmManagerService 将普通 Alarm 移出主调度队列
  → AWI 例外继续留在 Batch
  → 到期时再检查 creator UID 的最小交付间隔
  → 成功发起交付时附加短暂白名单并建立 InFlight
  → 所有交付完成后，AMS 反向通知 DIC 可以提前结束 maintenance
```

本章要建立的核心认识是：

> AllowWhileIdle 不是关闭 Doze，而是穿过 idle 挂起门后，仍受频率、身份和其他后台政策约束的一条窄通道。

## 2. 源码地图与四种 flag

核心文件：

```text
frameworks/base/core/java/android/app/AlarmManager.java
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
frameworks/base/apex/jobscheduler/framework/java/com/android/server/DeviceIdleInternal.java
```

先区分四个容易混淆的标记：

| 标记 | 谁能获得 | 对 idle 的意义 | 是否走普通 AWI 节流 |
|---|---|---|---|
| `FLAG_IDLE_UNTIL` | 仅 system UID 能保留 | 告诉 AMS 在此 Alarm 到期前挂起普通 Alarm | 否 |
| `FLAG_WAKE_FROM_IDLE` | Binder 入口不接受调用者伪造；服务端为 AlarmClock 添加 | 表示面向用户的提前唤醒点，可拉早 idle 边界 | 否 |
| `FLAG_ALLOW_WHILE_IDLE` | 普通应用可经 AWI API 请求 | 只允许这一枚 Alarm 在 idle 中运行，并不退出 idle | 是 |
| `FLAG_ALLOW_WHILE_IDLE_UNRESTRICTED` | 服务端为受信任调用者添加 | 在 idle 中按普通时序运行，不受 AWI 频率门限制 | 否 |

还要分清 `wakeup` 与 `WAKE_FROM_IDLE`：

- `RTC_WAKEUP` / `ELAPSED_REALTIME_WAKEUP` 表示 Alarm 到期时可唤醒 CPU；
- `FLAG_WAKE_FROM_IDLE` 表示它还是 Doze 边界要优先照顾的特殊 Alarm。

所以普通 `setExact(RTC_WAKEUP, ...)` 并不会自动获得 `WAKE_FROM_IDLE`。

## 3. deep Doze 怎样在 AMS 中建立挂起门

`DeviceIdleController.stepIdleStateLocked()` 进入 `STATE_IDLE` 时，会安排下一次 deep-idle 状态 Alarm：

```java
scheduleAlarmLocked(mNextIdleDelay, true);
```

`true` 最终走到隐藏 API：

```java
mAlarmManager.setIdleUntil(
        AlarmManager.ELAPSED_REALTIME_WAKEUP,
        mNextAlarmTime,
        "DeviceIdleController.deep",
        mDeepAlarmListener,
        mHandler);
```

`AlarmManager.setIdleUntil()` 创建的是 exact、wakeup、direct-listener Alarm，并带上 `FLAG_IDLE_UNTIL`。Binder 入口会清除非 system UID 请求的这个 flag，因此普通应用不能伪造 Doze 截止点。

AMS 插入这枚 Alarm 后，把它记为：

```java
mPendingIdleUntil = a;
```

这个非空引用是本章最重要的内部状态：它不是 PowerManager 所有 light/deep idle 状态的通用镜像，而是 AMS 当前的 deep-idle Alarm 挂起门。light idle 的阶段定时器使用普通 `set()`，不建立这道 `mPendingIdleUntil` 门。

## 4. 普通 Alarm 为什么离开主 Batch

`setImplLocked(Alarm)` 在 `mPendingIdleUntil != null` 时先检查例外 flag：

```java
if ((a.flags & (FLAG_ALLOW_WHILE_IDLE
        | FLAG_ALLOW_WHILE_IDLE_UNRESTRICTED
        | FLAG_WAKE_FROM_IDLE)) == 0) {
    mPendingWhileIdleAlarms.add(a);
    return;
}
```

由此得到容器模型：

```text
普通 Alarm                    → mPendingWhileIdleAlarms
ALLOW_WHILE_IDLE              → 继续进入 mAlarmBatches
ALLOW_WHILE_IDLE_UNRESTRICTED → 继续进入 mAlarmBatches
WAKE_FROM_IDLE                → 继续进入 mAlarmBatches
IDLE_UNTIL                    → 进入 Batch，并成为 mPendingIdleUntil
```

这个 `return` 发生在 App Standby 时间调整和 Batch 插入之前，挂起项暂时不会参与 kernel deadline 计算。

建立 `IDLE_UNTIL` 后，AMS 还会执行全量 rebatch。原来已经在 Batch 中的普通 Alarm 在 `reAddAlarmLocked()` 时同样看见非空的 `mPendingIdleUntil`，于是也转入挂起列表。因此 Doze 不只影响后来新设置的 Alarm。

这也解释了开头的 A：`setExact()` 虽然得到 `FLAG_STANDALONE`，但 standalone 只禁止与其他 Alarm 合批，不属于 idle 例外，A 仍会被挂起。

## 5. AlarmClock 怎样与 idle 边界协商

Binder 入口先清除调用者传来的 `WAKE_FROM_IDLE`；只有可信服务端逻辑可以重新授予。最典型的入口是：

```java
if (alarmClock != null) {
    flags |= FLAG_WAKE_FROM_IDLE | FLAG_STANDALONE;
}
```

`setAlarmClock()` 因此得到 exact、`RTC_WAKEUP`、standalone 和 `WAKE_FROM_IDLE` 四层语义。AMS 用 `mNextWakeFromIdle` 缓存最早的一枚候选。

若 DIC 计划的 `IDLE_UNTIL` 比它更晚，AMS 会先把 idle 截止点拉到该候选时刻，再从这个时刻随机减去一段 fuzz：

```java
if (mNextWakeFromIdle != null
        && idleUntil.whenElapsed > mNextWakeFromIdle.whenElapsed) {
    idleUntil.whenElapsed = mNextWakeFromIdle.whenElapsed;
}
idleUntil.whenElapsed -= randomDelta;
```

所以 fuzz 的确定事实是“状态 Alarm 可能比计算出的边界更早触发”；不要把它误套到普通应用的 exact Alarm 上，也不必从代码之外猜测其产品动机。

DIC 还有一层主动避让：

```java
nowElapsed + MIN_TIME_TO_ALARM >= getNextWakeFromIdleTime()
```

如果用户闹钟已经足够近，DIC 会延后进入 idle，或先恢复 active 再重新评估。AMS 的“拉早既有边界”和 DIC 的“避免新进 idle”共同保护用户闹钟。

AlarmClock 不是普通 AWI：它不进入 AWI 的 creator-UID 频率账本，也不因 AlarmClock 身份获得 AWI 的十秒 `BroadcastOptions`。

## 6. 普通 AWI 的 exact 到底保证什么

两个公开 API 都请求普通 `FLAG_ALLOW_WHILE_IDLE`，差别在初始窗口：

```text
setAndAllowWhileIdle()       → WINDOW_HEURISTIC
setExactAndAllowWhileIdle()  → WINDOW_EXACT
```

`WINDOW_EXACT` 让服务端设置 `FLAG_STANDALONE`，并令初始 `whenElapsed == maxWhenElapsed`。它没有绕过以下规则：

- 非 core 调用者的 `MIN_FUTURITY`；
- AWI 每 creator UID 最小交付间隔；
- App Standby 配额；
- 用户强制后台限制；
- `PendingIntent` 失效和实际交付失败。

因此 B 能穿过 `mPendingIdleUntil`，但“exact”只描述进入政策链前的单点窗口，不是硬实时承诺。`AlarmManager.java` 对 AWI 的定义也明确说：它允许这一枚 Alarm 在 idle 中执行，不会因此把设备带出 idle。

还有一种受信任路径。Binder 入口会先清掉调用者传来的 unrestricted flag，然后仅在下列条件满足时由服务端重新添加：

```text
WorkSource == null
且调用 UID 是 core、SystemUI 或用户电源白名单
```

此时普通 `ALLOW_WHILE_IDLE` 会被清除，换成 `ALLOW_WHILE_IDLE_UNRESTRICTED`。`WorkSource == null` 的要求避免特权调用者在“代表别人工作”时把自己的无限制资格一起转移出去。

## 7. 到期后怎样计算 AWI 的下一次合法时间

普通 AWI 已经进入 Batch，但真正的频率检查发生在 `triggerAlarmsLocked()` 取出到期 Batch 时。

账本按 `alarm.creatorUid` 索引：

```java
last = mLastAllowWhileIdleDispatch.get(alarm.creatorUid, -1);
minTime = last + getWhileIdleMinIntervalLocked(alarm.creatorUid);
```

使用 `creatorUid` 而不是单纯使用向 AMS 发起 Binder 调用的 UID，意味着代理携带别人创建的 `PendingIntent` 时，频率仍归到真正创建者。

r48 的默认常量是：

```text
ALLOW_WHILE_IDLE_SHORT_TIME       = 5 秒
ALLOW_WHILE_IDLE_LONG_TIME        = 9 分钟
ALLOW_WHILE_IDLE_WHITELIST_DURATION = 10 秒
```

API 注释中的“大约一分钟”“例如十五分钟”是概念性或历史描述；分析这个 tag 的默认行为应以实现中的 5 秒和 9 分钟为准。

长短间隔的选择公式是：

```text
正在 deep Doze                         → long
不在 Doze，且未 force-all-apps-standby → short
仅 force-all-apps-standby：UID 近期前台 → short
仅 force-all-apps-standby：其他 UID     → long
```

没有历史记录时 `last == -1`，第一枚 AWI 通过频率门。这里的“第一枚”只是当前 system_server 内存账本中没有一次成功发起的交付，不是应用安装以来永久第一次。

若 `nowElapsed < minTime`，Alarm 不会被丢弃，而会被重排：

```java
alarm.expectedWhenElapsed = alarm.whenElapsed = minTime;
if (alarm.maxWhenElapsed < minTime) {
    alarm.maxWhenElapsed = minTime;
}
alarm.expectedMaxWhenElapsed = alarm.maxWhenElapsed;
setImplLocked(alarm, true, false);
```

原 exact Alarm 的 `maxWhenElapsed` 等于旧时间；推迟 `whenElapsed` 时必须至少同步抬高 max，才能保持合法窗口。这就是 B 可能被推迟到上次成功交付后九分钟的直接证据。

## 8. 何时记账，十秒临时白名单又是什么

到达节流门不等于已经消耗一次间隔。`DeliveryTracker.deliverLocked()` 只有在发送没有立即失败、Alarm 已建立 InFlight 后才记录：

```java
mLastAllowWhileIdleDispatch.put(alarm.creatorUid, nowELAPSED);
```

若 `PendingIntent.send()` 立即抛出 `CanceledException`，或 direct listener 调用在建立 InFlight 前失败，方法会提前返回，不更新这次成功交付时间。这里的“成功”仍只表示 AMS 成功发起并跟踪交付，不代表应用业务逻辑最终成功。

每次普通 AWI 交付还会根据 creator UID 当前是否在前台更新 `mUseAllowWhileIdleShortTime`。UID 进入前台时，AppStateTracker 回调也会把它设为 `true`；UID 被移除时，短间隔状态和 last-dispatch 账本都会清理。

十秒临时白名单来自 `BroadcastOptions`：

```java
opts.setTemporaryAppWhitelistDuration(
        ALLOW_WHILE_IDLE_WHITELIST_DURATION);
mIdleOptions = opts.toBundle();
```

普通 AWI 的 `PendingIntent.send()` 会携带 `mIdleOptions`。公开 AWI API本来就只接收 `PendingIntent`；direct-listener 分支不传这个 Bundle。unrestricted flag 也不满足代码里的普通 `allowWhileIdle` 布尔，因此既不使用这份 Bundle，也不更新普通 AWI 账本。

临时白名单和 AlarmManager 的 `*alarm*` WakeLock 是两件事：

- 白名单给目标应用一小段受限政策例外时间；
- WakeLock 由 AMS 按所有 InFlight 交付的引用计数持有，直到 callback、complete 或 timeout 收账。

默认时长相近不代表所有者、用途或结束条件相同。

三个 AWI 常量都能从 `Settings.Global.ALARM_MANAGER_CONSTANTS` 热更新，解析处没有建立“非负”或 `long >= short` 的跨字段约束。更新白名单时长会重建后续发送使用的 Bundle；更新最小间隔不会主动重排已经按旧 `minTime` 放回 Batch 的 Alarm。

## 9. 穿过 Doze 后还有哪些政策门

普通 AWI 并不豁免 App Standby 配额。`isExemptFromAppStandby()` 只认：

```text
AlarmClock
core creator UID
FLAG_ALLOW_WHILE_IDLE_UNRESTRICTED
```

普通 `FLAG_ALLOW_WHILE_IDLE` 不在其中。因此它设置时仍会按 `sourcePackage + creatorUserId` 调整交付时间，成功交付后也会写 App wakeup history。它可能同时受“App Standby 配额门”和“AWI creator-UID 时间门”。下一章会专门展开前一扇门。

到期取出后还有 `isBackgroundRestricted()`。普通 AWI 会把 `isExemptOnBatterySaver=true` 传给 AppStateTracker，这可绕过 force-all-apps-standby 这一层，但不能绕过用户通过 `RUN_ANY_IN_BACKGROUND` 施加的强制限制。AlarmClock、启动 Activity 的 PendingIntent 另有直接豁免；foreground-service PendingIntent 也会改变传给政策层的参数。

因此“AWI 不受后台限制”过于宽泛。更准确的说法是：它改变若干政策门的输入，但没有删除整条政策链。

## 10. idle 结束时，挂起 Alarm 怎样回来

当 `mPendingIdleUntil` 自己进入触发列表时，AMS 依次执行：

```java
mPendingIdleUntil = null;
rebatchAllAlarmsLocked(false);
restorePendingWhileIdleAlarmsLocked();
```

`restorePendingWhileIdleAlarmsLocked()` 逐项调用 `reAddAlarmLocked()`，然后重设 kernel Alarm 和 next alarm clock。它只是把挂起项恢复到正常调度结构，并不在当前调用栈里同步发送所有 `PendingIntent`。未来时间、App Standby、后台限制以及后续到期检查仍然有效。

显式取消当前 `IDLE_UNTIL` 时也会清引用、rebatch 并 restore；全量 rebatch 若发现原来的 idle-until 对象意外丢失，也有防御性恢复。否则挂起列表可能失去释放入口。

`mNextWakeFromIdle` 被触发或取消后会清空并 rebatch，剩余 `WAKE_FROM_IDLE` Alarm 在重加时重新选出最早者。这个字段是缓存的最早引用，不是完整候选列表。

## 11. Alarm 交付怎样反馈给 maintenance

第一枚 Alarm 成功建立 InFlight 时：

```text
mBroadcastRefCount: 0 → 1
  → 获取 AMS 的 *alarm* WakeLock
  → Handler 投递 REPORT_ALARMS_ACTIVE(1)
  → DeviceIdleInternal.setAlarmsActive(true)
```

每个 `PendingIntent` finished、listener complete 或 listener timeout 都会减少引用。最后一枚完成时：

```text
mBroadcastRefCount: 1 → 0
  → 释放 WakeLock
  → Handler 投递 REPORT_ALARMS_ACTIVE(0)
  → DIC 尝试 exitMaintenanceEarlyIfNeededLocked()
```

通过 Handler 反向通知，避免 AMS 持有自身锁时直接进入 DIC monitor。代价是 DIC 看到的是异步、全局布尔状态，不是每枚 Alarm 的精确计数。

DIC 只有在以下条件同时成立时才会提前结束 deep/light maintenance：

```java
mActiveIdleOpCount <= 0 && !mJobsActive && !mAlarmsActive
```

所以 `alarmsActive` 只阻止 early exit，不负责打开 maintenance，也不会取消状态机已经安排的预算上界。它与按 UID 通知广播 pending/complete 的 `InFlightListener` 也不是同一条反馈链。

## 12. 用三个场景检验模型

场景一：deep Doze 中有普通 exact 和 AWI exact。

```text
普通 exact：standalone，但无 idle 例外 → pending-while-idle
AWI exact：穿过 idle 门 → 进入 Batch → 到期时再查 creator UID 间隔
```

场景二：同一 creator UID 的第一枚 AWI 在 T 成功发起交付，第二枚在 T+1 分钟到期。设备仍在 deep Doze，默认 long 为 9 分钟。

```text
第二枚的合法下限 = T + 9 分钟
T + 1 分钟到期检查 → 重排到 T + 9 分钟
```

如果第一枚的 `PendingIntent` 已取消、发送立即失败，没有建立 InFlight，则 T 不会写入 last-dispatch，第二枚不能基于这次失败计算九分钟。

场景三：用户 AlarmClock 早于 DIC 原计划的 deep-idle 截止点。

```text
AlarmClock → 服务端添加 WAKE_FROM_IDLE
  → 成为 mNextWakeFromIdle
  → AMS 拉早 IDLE_UNTIL
  → DIC 也通过 upcoming-alarm 检查避免临近时进入 idle
```

这三例分别验证了：exact 与 idle 权限正交、AWI 是推迟而非丢弃、AlarmClock 与普通 AWI 走不同通道。

## 13. 静态阅读与诊断清单

先用只读命令定位四条链：

```bash
rg -n "setIdleUntil|mPendingIdleUntil|mPendingWhileIdleAlarms" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java

rg -n "mNextWakeFromIdle|getNextWakeFromIdleTime|isUpcomingAlarmClock" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java

rg -n "mLastAllowWhileIdleDispatch|getWhileIdleMinIntervalLocked|mIdleOptions" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "REPORT_ALARMS_ACTIVE|setAlarmsActive|isOpsInactiveLocked" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
```

阅读 `dumpsys alarm` 对应实现时，优先建立这组证据：

```text
Idling until / Pending alarms
Next wake from idle
Last allow while idle dispatch times / Next allowed
mUseAllowWhileIdleShortTime
short / long / whitelist constants
Alarm expectedWhenElapsed 与实际 whenElapsed
Broadcast ref count / Outstanding deliveries
```

只看到“Pending”还不够：要先判断它位于主 Batch、idle 挂起、background 挂起还是 non-wakeup 延迟队列。只看到 AWI flag 也不够：还要计算 creator UID 的 next allowed，并检查 App Standby 与用户强制后台限制。

## 14. 结论与下一章

把开头问题压缩成一张决策图：

```text
Alarm 设置
  ├─ IDLE_UNTIL：system-only，建立 deep-idle 挂起门
  └─ 其他 Alarm
       ├─ 无 AWI / unrestricted / wake-from-idle → idle 中挂起
       └─ 有例外 → 留在主 Batch
                    ├─ 普通 AWI → App Standby + creator UID 频率门
                    ├─ unrestricted → 不走普通 AWI 频率与临时白名单路径
                    └─ AlarmClock → WAKE_FROM_IDLE，参与 idle 边界协商

成功发起交付
  → 普通 AWI PendingIntent 获得默认 10 秒临时白名单
  → InFlight 引用与 WakeLock 开始
  → DIC 收到 alarms-active
全部完成
  → 引用归零、WakeLock 释放
  → DIC 可在 jobs 与 active-op 也空闲时提前收回 maintenance
```

最终应记住五点：

1. exact、wakeup、allow-while-idle、wake-from-idle 是彼此独立的维度；
2. `mPendingIdleUntil` 通过容器分流挂起普通 Alarm，而不是给每枚 Alarm 增加 constraint bit；
3. 普通 AWI 按 `creatorUid` 在到期时节流，r48 默认 deep Doze 最小间隔为 9 分钟；
4. 十秒临时白名单只附加在普通 AWI 的 `PendingIntent` 发送路径，不等于 AMS WakeLock；
5. Alarm 的 InFlight 聚合状态只影响 maintenance 能否提前结束，不会无限延长维护窗口。

下一章继续追问：普通 AWI 已穿过 Doze 门后，为什么还可能被 App Standby 配额再次改写时间？第 147 章将沿 `sourcePackage + creatorUserId` 的滚动唤醒历史、bucket 配额和重排触发条件展开。
