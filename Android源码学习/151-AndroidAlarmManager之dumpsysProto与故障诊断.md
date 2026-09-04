# 151 Android AlarmManager：dumpsys、Proto 与故障诊断

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 145～150 章

---

## 1. 本章只解决一个问题

一枚 Alarm “没有响”，手里只有一份 `dumpsys alarm`。怎样判断它此刻停在：

- 正常 Batch；
- 后台限制或 Doze 暂存；
- 已到期但仍被合并的 non-wakeup 队列；
- Java 已计划但 kernel timer 异常；
- 已发出却没有完成的 InFlight？

诊断顺序应是：

```text
先找生命周期容器
  → 再统一时间基准
  → 再解释政策推迟
  → 再核对 Java 与 kernel deadline
  → 最后检查 InFlight 和完成账
```

一句话结论：

> `dumpsys alarm` 是一张持锁的 system_server 内存快照。容器决定 Alarm 在哪一阶段，时间和政策解释它为什么在那里，kernel timer 与 InFlight 才分别验证“是否已安排”和“是否已交付完成”。

## 2. 先确定这份快照怎样产生

核心源码：

```text
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/core/java/com/android/internal/util/DumpUtils.java
frameworks/base/core/java/android/util/TimeUtils.java
frameworks/base/core/proto/android/server/alarmmanagerservice.proto
frameworks/base/services/core/jni/com_android_server_AlarmManagerService.cpp
```

Binder dump 入口先同时检查 DUMP 与 Usage Stats 权限：

```java
if (!DumpUtils.checkDumpAndUsageStatsPermission(
        getContext(), TAG, pw)) return;

if (args.length > 0 && "--proto".equals(args[0])) {
    dumpProto(fd);
} else {
    dumpImpl(pw);
}
```

普通 UID 除 `android.permission.DUMP` 外，还要有 `PACKAGE_USAGE_STATS`，且 `OP_GET_USAGE_STATS` 为 allowed 或 default。root、system、shell、incidentd 只在 Usage Stats 这层直接放行，DUMP 检查仍先执行。Alarm tag、包名和触发模式会暴露使用行为，这也是权限较严的原因。

r48 只识别第一个参数是否为 `--proto`，没有服务端包过滤。`dumpsys alarm com.example` 仍走完整文本；事后 `grep` 只是在客户端裁剪，还可能丢掉 Batch 和容器上下文。

`dumpImpl()` 与 `dumpProto()` 都在 `synchronized (mLock)` 内收集状态。于是同一快照的核心容器大体一致，但巨大 dump 也会延长持锁时间，短暂挡住 set、remove、trigger 等路径。Proto 在退出锁后才 `flush()`。

## 3. 先把三根时钟放到同一张纸上

dump 开头固定采样：

```java
long nowELAPSED = mInjector.getElapsedRealtime();
long nowUPTIME = SystemClock.uptimeMillis();
long nowRTC = mInjector.getCurrentTimeMillis();
```

| 数轴 | 深睡时是否前进 | 手动改时是否跳变 | 主要用途 |
|---|---:|---:|---|
| wall / RTC | 是 | 是 | 人类日期、AlarmClock、TIME_TICK |
| elapsedRealtime | 是 | 否 | Alarm 调度、Batch、InFlight |
| uptimeMillis | 否 | 否 | 当前 runtime 实际清醒运行时间 |

`TimeUtils.formatDuration(target, now)` 展示的是 `target - now`：未来为正，过去为负。若源码先传入 `now - past` 的单个 duration，则“过去多久”反而显示正数。看符号前必须先看调用参数。

文本把 elapsed deadline 投影成日期：

```text
wall投影 = elapsed deadline + (nowRTC - nowELAPSED)
```

这只是抓取瞬间使用当前 offset 做出的解释；之后改 wall clock，不会改写旧快照。最近十次 TIME_TICK 历史也用抓取时 offset 回投，并未保存每次 tick 当时的 wall 值。

`Runtime uptime (elapsed) - Runtime uptime (uptime)` 可近似反映本轮 framework runtime 内的深睡时间。若出现 `(Runtime restarted)`，说明 system_server 不是本次 boot 的第一轮 runtime；Alarm 内存状态和本章讨论的累计统计都要从当前 runtime 重新解释。

## 4. 用容器还原 Alarm 生命周期

文本输出不是任意罗列，它对应一组互斥或相继的状态：

| dump 区域 | 主要内存结构 | 阶段 |
|---|---|---|
| Pending alarm batches | `mAlarmBatches` | 正常等待 kernel 近期 deadline |
| Pending user blocked background alarms | `mPendingBackgroundAlarms` | source App 被后台政策挡住 |
| Pending while idle | `mPendingWhileIdleAlarms` | deep Doze 暂存 |
| Past-due non-wakeup alarms | `mPendingNonWakeupAlarms` | 已到期，等待合并交付 |
| Outstanding deliveries | `mInFlight` | 已成功发起，等待完成信号 |

名义上的状态迁移是：

```text
set → 主Batch/idle暂存
主Batch到期 → 后台限制暂存 或 past-due non-wakeup 或 triggerList
triggerList成功发起 → InFlight
finished / alarmComplete / timeout → 离开InFlight
```

因此“主 Batch 搜不到”绝不等于 Alarm 被丢弃。必须继续搜其他三个等待容器和 InFlight；仍不存在时，才回查 replacement、cancel、包/用户生命周期清理和 set 入口是否真正成功。

`Pending alarms per uid` 的 `mAlarmsPerUid` 是等待登记账，不是在途数、wakeup 次数或 WakeLock 数。当前 occurrence 从等待结构取出后扣减；若成功发起，则由 `mBroadcastRefCount` 与 InFlight 接管执行阶段。

## 5. 在主 Batch 中怎样读一枚 Alarm

Batch 的时间域始终是 elapsed：

```text
Batch.start = 批内所有 whenElapsed 的最大值
Batch.end   = 批内所有 maxWhenElapsed 的最小值
```

`[start, end]` 是全部成员都可接受的共同交集，Batch 按 start 升序保存。某一项自身的 `whenElapsed` 早于 Batch.start，通常只是为合批等待，不等于漏触发。Batch flags 是成员 flags 的按位并集，也不是每一项都具有全部 bit。

单条文本同时给出：

```text
expectedWhenElapsed / expectedMaxWhenElapsed
whenElapsed / maxWhenElapsed
原时间域 when、window、repeat、count、flags
operation 或 listener
```

这里要收紧“expected”的含义：它们初始是 elapsed 归一化后的窗口，主要作为 App Standby 调整前的恢复基线；AWI 频率门再次排期时，会连 expected 一起重置。App Standby 延迟则只把实际 `whenElapsed/maxWhenElapsed` 推到 quota 恢复点。

所以：

```text
actual 明显晚于 expected
  → 是 App Standby 延迟的强证据
  → 对 system-only IDLE_UNTIL 还要考虑 wake-from-idle 修正
```

但 gap 为零不能排除 Doze 或后台限制，因为这两者主要把 Alarm 移入单独容器，不一定改写它的时间字段。不能把 `actual - expected` 笼统解释为“所有政策累计延迟”。

## 6. 三类暂存和两类辅助历史怎样读

后台限制区按 creator UID 保存列表，但判断限制使用 source UID/package。定位代理设置时，要把登记 caller、PendingIntent creator 和目标包分开记录。

Idle 区有三个不同概念：

- `Idling until`：system-only 的 idle-until 控制 Alarm；
- `Pending alarms`：被 deep Doze 门挡住的普通 Alarm；
- `Next wake from idle`：允许打破 idle 的近期候选，可能反过来拉早 idle-until。

Past-due non-wakeup 是已经从 Batch 取出、但因设备 non-interactive 且近期有过投递而延迟的队列。若 `Next non-wakeup delivery time` 仍在未来，这通常是设计内合并；当前列表的存在不能直接证明 AlarmThread 卡死。

累计指标要和当前列表分开：

| 字段 | 含义 |
|---|---|
| Number of delayed alarms | 本轮 runtime 中被延迟的 Alarm 条目总数 |
| total delay time | 每轮重叠延迟区间只累计一次 |
| max delay time | 已结算延迟轮次中的最大值 |
| max non-interactive time | 已结束的最长非交互区间 |

`App Alarm history` 按 source package/user 保存非豁免 Alarm 的成功发起时间，最多为每项打印最近 100 个值；它是 Standby quota 证据，不是全部 `set()` 历史。

AWI 区按 creator UID 显示最近成功 dispatch、下一允许时间及 short/long interval。`Next alarm clock information` 则按 user 显示 wall trigger；其中 `pendingSend:true` 表示 `ACTION_NEXT_ALARM_CLOCK_CHANGED` 的聚合通知待发，不表示 Alarm 本体尚未登记。

## 7. Java next 与 kernel remaining 为什么不能单独作证

`rescheduleKernelAlarmsLocked()` 只选两个近期目标：

```java
if (firstWakeup != null) {
    mNextWakeup = firstWakeup.start;
    setLocked(ELAPSED_REALTIME_WAKEUP, firstWakeup.start);
}
if (nextNonWakeup != 0) {
    mNextNonWakeup = nextNonWakeup;
    setLocked(ELAPSED_REALTIME, nextNonWakeup);
}
```

文本中的 Java next 同时包含相对值、raw elapsed、wall 投影和 `set at`。这里的 `set at` 是最近写 kernel deadline 的 elapsed 时刻，不是 App 原始目标。

另两行 `Next kernel ... alarm` 经 JNI 调用 `timerfd_gettime()`，得到的是 timerfd 当前 `it_value` 剩余 duration：

```text
Java next raw = elapsed绝对deadline
kernel next   = 从dump时刻起还剩多久
```

二者不能直接拿 raw 数字相减，且抓取本身存在小量采样误差。

r48 还有一个关键边界：`rescheduleKernelAlarmsLocked()` 在找不到新目标时，不把 `mNextWakeup/mNextNonWakeup` 清零，也不显式 disarm 原来的 timerfd。于是：

- Java next 可保留上次计划值；
- 最近 Alarm 被提前取消后，旧 kernel timer 也可继续倒计时，直到旧 deadline 返回一次；
- 单看一个过去的 Java next 或一个无对应 Batch 的 kernel remaining，都不能证明当前仍有业务 Alarm。

可信判断至少要把当前 Batch、Java next、kernel remaining 三者并列。只有“Batch 仍有稳定可复现的近期目标，但对应 kernel timer 长期为 0 或明显不符”才真正指向 reschedule、`setLocked()` 或 native timerfd 链。

若系统没有 alarm driver，`setLocked()` 退回 Handler 消息，这时 kernel timer 两行不能代表等价的 wakeup 能力；第 145 章已经说明该 fallback 不具备完整硬件语义。

## 8. 三个 `Last` 字段最容易被名字误导

AlarmThread 的关键顺序是：

```java
int result = mInjector.waitForAlarm();
long nowELAPSED = mInjector.getElapsedRealtime();
mLastWakeup = nowELAPSED;
...
mLastTrigger = nowELAPSED;
boolean hasWakeup = triggerAlarmsLocked(triggerList, nowELAPSED);
```

`mLastWakeup` 在 result flag 解析前就更新，准确含义是“最近一次 native wait 返回”，原因可以是 wakeup timer、non-wakeup timer、time change，甚至异常的 result 0；它既不证明 CPU 是由 wakeup Alarm 唤醒，更不表示屏幕点亮。

`mLastTrigger` 在扫描 Batch 前更新，triggerList 可以为空。它代表最近一次进入到期检查，不代表 App 成功收到 Alarm。

`mLastAlarmDeliveryTime` 又在 `deliverAlarmsLocked()` 遍历前更新。AlarmThread 能用空 triggerList 调用该方法，因此文本 `Time since last dispatch` 更接近“距离最近一次进入交付阶段”，不是最近一次成功 send。

non-interactive 区同样要看条件：文本始终打印 Max wakeup delay、last dispatch 与 next non-wakeup delivery，只在非交互时多打印 `Time since non-interactive`；Proto 却把这四个字段全部包在 `if (!mInteractive)` 中。文本在 interactive 状态下打印的 fuzz 依赖旧 non-interactive 起点，不宜当当前政策值使用。

## 9. 怎样识别“已发出但没有完成”

最直接的证据是：

```text
Broadcast ref count
Outstanding deliveries
PendingIntent send/finish
Listener send/finish
```

正常情况下：

```text
mBroadcastRefCount == mInFlight.size()
ref ≈ (PI send - PI finish) + (listener send - listener finish)
```

每个 InFlight 文本包含 PendingIntent、开始 elapsed、WorkSource、登记 uid、creatorUid、tag、type 和两级 stats。根据目标类型继续追：

| InFlight 类型 | 应等待的 AlarmManager 完成信号 |
|---|---|
| broadcast PendingIntent | serialized broadcast 链最终回调 OnFinished |
| activity/service PendingIntent | 启动请求交接后的 OnFinished，通常很快 |
| direct listener | `alarmComplete()` 或默认五秒 timeout |

共享 `*alarm*` WakeLock 在第一项 InFlight 时 acquire，最后一项完成时 release。AlarmManagerService WakeLock 长期持有，应先从 Outstanding deliveries 找未闭合项，而不是盯等待 Batch。

这些计数不是可独立宣判泄漏的事务账本。r48 的 unmatched PI finished 会在找不到 InFlight 时仍无条件 ref--；同 listener 重叠又可能一次移除全部同 token timeout，却只完成第一项。先检查 ref/list 一致性和具体条目，再把 send-finish 差当异常提示。

文本 InFlight 没有直接打印 listener Binder；它只表现为 `pendingIntent=null` 加 tag 等信息。Proto 还缺 creatorUid 和 listener 标识，代理或 listener 卡住时证据更弱。

## 10. Top Alarms 和 Alarm Stats 能证明什么

`FilterStats` 按 `aggregateTime` 降序选前十。它统计的是 Alarm 完成协议的 elapsed 在途区间，不是 set 次数、CPU 时间或电量：

```text
首次 nesting 0→1：记录 startTime
重叠项开始：nesting++
逐项完成：nesting--
最后 1→0：aggregate += now - startTime
```

同 tag 两项并发五秒，aggregate 通常增加的是“至少一项在飞”的并集，而不是十秒。当前仍 `*ACTIVE*` 的区间尚未结算进 aggregate，所以一个刚卡住很久的 tag 可能不在 Top 10；长挂诊断应优先看 InFlight。

`count` 是成功建立统计的交付次数，`numWakeup` 是其中 wakeup type 次数，`lastTime` 是该 filter 最近一项开始的 elapsed 时刻。包级 BroadcastStats 与 tag 级 FilterStats 都会 nesting，但文本包头不打印 count，Proto 会写。

这些对象只存在于 system_server 内存，包清理或 runtime 重启都能让统计消失。对 activity/service PendingIntent，完成窗口又只覆盖启动交接，不覆盖组件后续业务。因此 Top 第一名最多说明“本轮 runtime 中该 tag 的 Alarm 交付完成窗口累计较长”，不能直接改写成耗电元凶。

文本末尾的 StatLogger 是 AMS 内部 rebatch/reorder 等操作的次数和耗时，不是 App callback 时长。`Recent problems` 是容量有限的 LocalLog，也不是完整 logcat。

## 11. 为什么现场最好同时保存文本和 Proto

Proto 类型稳定、适合 bugreport 与程序解析，但 r48 的 producer 只挑选部分状态，并不是文本的一比一编码。

| 关键证据 | 文本 | Proto |
|---|---:|---:|
| App Standby Parole | 有 | 无 |
| runtime start/restarted | 有 | 无 |
| TIME_TICK 四节点与十条历史 | 有 | 无 |
| Java raw next 与 wall 投影 | 有 | 仅相对值 |
| kernel timerfd remaining | 有 | 无 |
| Last trigger | 有 | 无 |
| Pending alarms per UID | 有 | 无 |
| App Alarm history | 有 | 无 |
| StatLogger | 有 | 无 |
| 完整 quota/restricted Constants | 有 | 无 |

`AlarmProto` 只有 `whenElapsed-nowElapsed`、window/repeat/count/flags、AlarmClock、PI/listener 等；它缺 expected、maxWhenElapsed、原始 when、身份和 WorkSource，无法独立计算 Standby 推迟量。`BatchProto.start/end` 是 raw elapsed 绝对值，要减顶层 `elapsed_realtime` 才得到相对时间。

r48 还存在两个明确的 schema/producer 边界：

```java
// 字段名是 time_until...
proto.write(TIME_UNTIL_NEXT_NON_WAKEUP_DELIVERY_MS,
        nowElapsed - mNextNonWakeupDeliveryTime);
```

未来五秒的 delivery 会写成 `-5000`，实际符号与 “time until” 相反。另一个字段 `device_idle_user_whitelist_app_ids` 虽在 schema 声明，`dumpProto()` 却从未写入；空数组不能证明白名单为空。

此外 `RECORD_DEVICE_IDLE_ALARMS=false`、`WAKEUP_STATS=false`，所以源码中 Allow-while-idle dispatch history 与 Recent Wakeup History 默认不会出现在文本或 Proto。不要把编译期未记录误判成“历史确实为空”。

## 12. 一套可复用的“没响”诊断树

```text
1. 在主Batch找到目标？
   ├─ 是：读 expected/actual、Batch.start/end
   │      ├─ actual被推迟：查Standby quota/history
   │      └─ deadline合理：并列核对Java next和kernel remaining
   └─ 否：继续找三个等待容器
          ├─ background：查source身份与AppStateTracker
          ├─ while-idle：查idle-until/wake-from-idle
          └─ past-due：查interactive和next delivery

2. 等待容器都没有？
   ├─ InFlight有：按PI类型/listener追完成协议
   └─ InFlight无：查replacement、cancel、生命周期清理和set结果

3. 判断异常前再排除
   ├─ Java next旧缓存或旧kernel timer
   ├─ Last字段只是阶段入口时间
   ├─ 累计统计跨多次事件
   └─ Proto字段缺失或producer偏差
```

一次快照只能证明“抓取时内存里有什么”。要回答谁在何时 set/remove、timerfd 何时重设、receiver 是否 `goAsync()` 后忘记 finish、system_server 是否刚重启，仍需要连续快照、logcat、Perfetto、BatteryStats 或 bugreport 时间线。

## 13. 用四个场景校准结论

场景一：目标在 Batch，`actual` 比 `expected` 晚一小时。

先看 source package/user 的 bucket、parole、quota 和 App Alarm history。若 actual 正好落在 quota 恢复点，这是 Standby 重排证据；不应先怀疑 timerfd。Doze 则更可能表现为目标在 pending-while-idle，而不是一小时 gap。

场景二：目标在 Past-due non-wakeup，next delivery 还有三分钟。

如果设备 non-interactive，说明 Alarm 已到期但正在合并。三分钟内未交付并非故障；若 next delivery 长期过去，再联合 kernel non-wakeup remaining、last wait return 和 Recent problems。

场景三：Batch 已空，但 Java next 为过去值，kernel 还剩二十秒。

这可以由 r48 不清 next、不撤旧 timerfd 解释。等旧 timer 返回后再抓一份，或观察是否有新 Batch；不能把它直接解释成仍有一枚隐形 Alarm。

场景四：ref 为 1，listener InFlight 已持续十秒。

默认五秒 timeout 本应收口。检查 Alarm Handler 是否被阻塞、同 listener 是否有重叠代际导致 timeout 被一起移除，以及 ref/list 是否已失配。timeout 不会停止 App Runnable，也不会在 r48 直接触发 ANR。

## 14. macOS 静态验证清单

先定位快照骨架与时间字段：

```bash
rg -n "protected void dump|void dumpImpl|void dumpProto" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "mLastWakeup|mLastTrigger|mLastAlarmDeliveryTime" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

预期确认：两个 dump 都持 `mLock`；三个 Last 字段都在真正结果验证或遍历完成前写入。

再验证 next 的双重残留边界：

```bash
rg -n "mNextWakeup =|mNextNonWakeup =|rescheduleKernelAlarmsLocked" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

sed -n '3043,3070p' \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

预期确认：字段只在启动时归零、找到新目标时赋值；无目标分支没有清缓存，也没有向 native 发 disarm。

最后比对 schema 与 producer：

```bash
rg -n "time_until_next_non_wakeup_delivery|device_idle_user_whitelist" \
  frameworks/base/core/proto/android/server/alarmmanagerservice.proto \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "BROADCAST_REF_COUNT|OUTSTANDING_DELIVERIES|TOP_ALARMS" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

预期确认：time-until 字段 producer 使用 `now-target`；whitelist 只有 schema 声明；InFlight 和 Top Stats 是两类不同证据。

建议每次现场记录固定七项：目标 tag/身份、抓取三时钟、所在容器、expected/actual、Batch/Java/kernel deadline、政策状态、ref/InFlight/send-finish。这样下一份快照才能逐项比较，而不是只保存一段被 grep 过的孤立文本。

## 15. 结论与下一章

把一份 `dumpsys alarm` 还原成状态机，只需守住六条规则：

1. 先找 Batch、background、idle、past-due、InFlight，不先猜原因；
2. wall 负责日历解释，调度比较统一回到 elapsed；
3. expected/actual 主要证明 Standby 时间改写，Doze 与后台限制还要看容器；
4. Java next 和 timerfd remaining 都可能残留，必须以当前容器交叉验证；
5. 三个 Last 字段记录阶段入口，不严格证明唤醒、触发或成功交付；
6. Top/计数/Proto 都是辅助证据，不能替代 InFlight 明细与时间线。

下一章进入 PowerManagerService：一次 WakeLock acquire 怎样经过 Binder token、权限与 WorkSource 建立服务端记录，又怎样在 release、timeout 或 Binder death 时撤销并触发电源状态重算。
