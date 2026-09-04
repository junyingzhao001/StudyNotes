# 150 Android AlarmManager：投递、InFlight、WakeLock 与完成协议

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 145～149 章

---

## 1. 本章只回答一个问题

一枚 broadcast PendingIntent 和一枚 `OnAlarmListener` 几乎同时到期。AlarmManagerService 怎样保证：

- 目标确实发出去后才持 WakeLock；
- 两枚 Alarm 共享一把锁，却各自完成一次账；
- listener 卡住五秒后不把锁永久占住；
- 某一枚先完成时，不会误释放仍保护另一枚的锁？

核心链是：

```text
triggerList
  → PendingIntent.send(OnFinished) 或 IAlarmListener.doAlarm(completion)
  → 成功发起后建立 InFlight、共享 ref++
  → 第一个 InFlight 获取 PARTIAL_WAKE_LOCK
  → finished / alarmComplete / listener timeout 删除一个 InFlight
  → ref--，最后一个完成才 release
```

一句话结论：

> AlarmManager 的 WakeLock 保护的是一次受控“交付协议”，不是应用全部业务生命周期；只有成功进入 InFlight 的投递才记账，并由目标类型对应的完成信号销账。

## 2. 先分清三类计数

| 计数 | 代表什么 | 生命周期 |
|---|---|---|
| `mAlarmsPerUid` | 尚在 Batch/pending 结构中的登记数 | set 到到期取出或取消 |
| `mBroadcastRefCount` | 已成功发起、尚未完成的全部 InFlight 数 | 建立 InFlight 到完成/超时 |
| `BroadcastStats/FilterStats.nesting` | 同账户/同 tag 至少一项在飞的嵌套层数 | 第一项开始到最后一项结束 |

`mBroadcastRefCount` 的名字具有历史色彩：它也统计 activity/service PendingIntent 和 direct listener，不只统计广播。

Alarm 从等待态进入执行态时，第一张账减少，后两张账增加。`cancel()` 管等待项，finished/complete/timeout 管执行项，不能拿一个 counter 解释另一阶段。

核心源码：

```text
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/services/core/java/com/android/server/AlarmManagerInternal.java
frameworks/base/core/java/android/app/AlarmManager.java
frameworks/base/core/java/android/app/PendingIntent.java
frameworks/base/core/java/android/app/IAlarmListener.aidl
frameworks/base/core/java/android/app/IAlarmCompleteListener.aidl
frameworks/base/services/core/java/com/android/server/am/PendingIntentRecord.java
frameworks/base/services/core/java/com/android/server/am/BroadcastDispatcher.java
```

## 3. 外层投递先结束哪张等待账

`deliverAlarmsLocked()` 遍历已经通过到期与政策门的 `triggerList`：

```java
for (Alarm alarm : triggerList) {
    try {
        ActivityManager.noteAlarmStart(...);
        mDeliveryTracker.deliverLocked(alarm, nowElapsed, allowWhileIdle);
    } catch (RuntimeException e) {
        Slog.w(TAG, "Failure sending alarm.", e);
    }
    decrementAlarmCount(alarm.uid, 1);
}
```

无论目标成功、PendingIntent 已取消，还是内部出现 RuntimeException，这一 occurrence 已从等待结构取出，最后都按登记 `alarm.uid` 扣 `mAlarmsPerUid`。

repeating Alarm 的下一 occurrence 已在 `triggerAlarmsLocked()` 中提前重新登记。它拥有自己的一份等待计数；当前 occurrence 的完成协议不会替它销账。

`DeliveryTracker` 同时实现两个回调接口：

```java
IAlarmCompleteListener.Stub
PendingIntent.OnFinished
```

两种目标最终都汇入同一套 InFlight 与共享 WakeLock 逻辑。

## 4. PendingIntent 路径怎样发起并附带完成回调

发送参数的核心是：

```java
alarm.operation.send(
        context,
        0,
        mBackgroundIntent.putExtra(EXTRA_ALARM_COUNT, alarm.count),
        mDeliveryTracker,
        mHandler,
        null,
        allowWhileIdle ? mIdleOptions : null);
```

其中：

- fill-in Intent 带 `FLAG_FROM_BACKGROUND`；
- `EXTRA_ALARM_COUNT` 是这枚 repeating Alarm 合并的 occurrence 数，通常一次性 Alarm 为 1；
- `mDeliveryTracker` 是 `OnFinished`；
- finished callback 明确投到 Alarm Handler；
- 普通 AWI 才带上一章所述临时白名单 options。

若 token 已 canceled，`PendingIntent.send()` 抛 `CanceledException`：

```text
mSendCount++
  → send 失败
  → repeating 则按 token 删除已安排的下一 occurrence
  → mSendFinishCount++
  → 不建 InFlight、不拿 WakeLock
```

`mSendCount - mSendFinishCount` 因此是 PendingIntent 未完成交付的诊断近似值，而不是当前等待 Alarm 数。

## 5. PendingIntent 的“完成”因目标类型而异

`PendingIntent.FinishedDispatcher` 是 `IIntentReceiver.Stub`。收到 `performReceive()` 后，它把 `onSendFinished()` post 到 AMS 传入的 Handler。

`PendingIntentRecord.sendInner()` 的具体完成点取决于 sender type：

| PendingIntent 类型 | AlarmManager 何时通常收到 OnFinished |
|---|---|
| broadcast | 成功进入 serialized broadcast 流后，等广播链最终完成 |
| activity | 启动请求交接后立即回调 |
| service / foreground service | 启动请求交接后立即回调 |
| activity result | result 请求交接后立即回调 |

broadcast 分支在 `broadcastIntentInPackage()` 返回成功时把 `sendFinish=false`，由 BroadcastQueue 以后回调；若未成功入队但结果也不是 canceled，则走立即 finished。

因此：

```text
Alarm WakeLock release
≠ Activity 生命周期结束
≠ Service.onStartCommand() 或异步业务完成
```

broadcast 的完成更接近 receiver 链收口，但具体 receiver timeout/ANR 由 BroadcastQueue 负责；AlarmManagerService 没有给所有 PendingIntent 再统一设置一个五秒 timeout。

## 6. listener 路径为什么需要 completion 加 timeout

`IAlarmListener.aidl` 声明为 `oneway`。服务端调用：

```java
mListenerCount++;
alarm.listener.doAlarm(this);
mHandler.sendMessageDelayed(
        obtainMessage(LISTENER_TIMEOUT, listenerBinder),
        mConstants.LISTENER_TIMEOUT);
```

`this` 是 `DeliveryTracker` 的 `IAlarmCompleteListener`。oneway 调用返回只表示异步事务已提交，不表示 App 的 `onAlarm()` 已运行完。

客户端 `ListenerWrapper` 收到后保存 completion，并把自身 post 到目标 Handler：

```java
public void run() {
    try {
        mListener.onAlarm();
    } finally {
        mCompletion.alarmComplete(this);
    }
}
```

即使 `onAlarm()` 抛异常，`finally` 仍尝试完成回报，然后异常可以继续使 App 崩溃。

r48 默认 listener timeout 为五秒，可由 `listener_timeout` 修改。计时从 oneway 提交成功后开始，覆盖 App Binder 排队、目标 Handler 排队和 callback 执行；主线程堵塞时，可能在 `onAlarm()` 真正开始前就超时。

若 `doAlarm()` 本身在提交阶段抛异常，服务只把 listener send/finish 计数就地闭合，不安排 timeout、不建立 InFlight。

## 7. 成功后怎样建立共享 InFlight

两条发送分支成功返回后才执行：

```java
if (mBroadcastRefCount == 0) {
    setWakelockWorkSource(...);
    mWakeLock.acquire();
    postAlarmsActive(true);
}
InFlight f = new InFlight(..., nowElapsed);
mInFlight.add(f);
mBroadcastRefCount++;
```

WakeLock 创建为：

```java
newWakeLock(PARTIAL_WAKE_LOCK, "*alarm*")
```

它保证 CPU 可运行，不点亮屏幕。第一个 InFlight 从 0→1 时 acquire；并发项只增加服务自己的 ref/list，不再重复 acquire。最后一个完成才 release。

`InFlight` 保存完成阶段所需快照：

```text
PendingIntent 或 listener Binder
投递开始 nowElapsed
WorkSource
登记 uid / creatorUid
stats tag、Alarm type
BroadcastStats / FilterStats 引用
```

其中 `mWhenElapsed` 是建立 InFlight 的实际投递时刻，不是 App 请求的 deadline。

## 8. 为什么“先 send、后 acquire”没有正常抢跑

顺序看似危险：目标请求先发出，InFlight 和 WakeLock 后建立。但正常完成回调仍不能先把账销掉：

- PendingIntent 的 `FinishedDispatcher` 被要求 post 到 Alarm Handler；
- listener 的 `doAlarm()` 是 oneway，App 还会再 post 到自己的 Handler；
- `deliverLocked()` 全程在 AMS `mLock` 下，反向 `alarmComplete()` 也必须先取得同一把锁。

目标进程可能已经开始调度，但“完成销账”会在 InFlight 建立并退出临界区后进行。安全来自异步回调与同一把锁的组合，不是来自“App 绝不可能提前运行”。

listener timeout 消息虽然也在建 InFlight 前入队，但默认延迟五秒，且处理时同样要拿 `mLock`。

## 9. 交付归因和统计在何时建立

目标调用外围先设置线程局部归因：

```text
有非空 WorkSource → WorkSource attribution UID
否则              → creatorUid
```

`finally` 中恢复，避免污染 system_server 线程之后的工作。这与 WakeLock 自身的 WorkSource 是两层账。

共享 WakeLock 第一次 acquire 时优先使用 Alarm 的完整 WorkSource，否则使用 creator UID。某项完成但仍有其他 InFlight 时，服务用 `mInFlight[0]` 的 WorkSource/creator UID 重新归因；其他项仍被同一把锁保护，只是瞬时电量 blame 由队首代表。

统计账户也按目标类型区分：

```text
PendingIntent → creator UID + creator package
listener      → Alarm.uid + Alarm.packageName
```

`BroadcastStats` 和 tag 级 `FilterStats` 开始时 `count++/nesting++`，第一层记录 startTime；完成时 `nesting--`，只有归零才把 `now-startTime` 加入 aggregateTime。因此重叠交付记录的是“至少一项在飞”的并集时长，不是每项时长简单求和。

成功建立 InFlight 后还会记录：

- 普通 AWI 的 creator-UID last dispatch；
- 非豁免 App Standby 的 source package/user history；
- wakeup type 的 wakeup 统计与 `noteWakeupAlarm()`。

这些记录点代表“成功发起并纳入跟踪”，不等待应用业务成功。

## 10. 两种完成入口怎样找到一个 InFlight

PendingIntent 回调：

```java
if (inflight.mPendingIntent == pi) {
    return mInFlight.remove(i);
}
```

这里用 Java 引用 `==`，不是第 149 章等待阶段的 token `equals()`。`FinishedDispatcher` 保存并回传的正是本次 `operation.send()` 使用的 PendingIntent 对象，因此正常协议可命中。

listener 回调：

```java
if (inflight.mListener == who) {
    return mInFlight.remove(i);
}
```

它按 ListenerWrapper Binder 对象找第一个匹配项。

PendingIntent `onSendFinished()` 不根据 resultCode 判断业务成败；回调到达就表示 AlarmManager 的交付协议结束。listener `alarmComplete()` 则先移除同 token 的 timeout 消息，再查 InFlight；若已经超时，找不到就按 late completion 忽略，不会二次销账。

timeout 路径同样只有找到 InFlight 才调用统一销账。r48 留有 `TODO: implement ANR policy for the target`，所以这里的五秒超时只终止 AlarmManager 跟踪，不直接产生 ANR，也不会强行停止稍后才运行或仍在运行的 App callback。

## 11. `updateTrackingLocked()` 怎样维护共享不变量

正常完成一个 InFlight 时：

```text
更新 stats nesting 和可选 noteAlarmFinish
  → mBroadcastRefCount--
  ├─ 仍大于 0：按 mInFlight[0] 重设 WakeLock 归因
  └─ 等于 0：报告 alarms inactive，释放 WakeLock
```

假设 A、B、C 依次成功，B、A、C 依次完成：

```text
A 开始：ref 0→1，list[A]，acquire
B 开始：ref 1→2，list[A,B]
C 开始：ref 2→3，list[A,B,C]
B 完成：ref 3→2，list[A,C]，归因 A
A 完成：ref 2→1，list[C]，归因 C
C 完成：ref 1→0，list[]，release
```

正常不变量是：

```text
mBroadcastRefCount == mInFlight.size()
```

若 ref 已归零而 list 仍有项，r48 会记录错误、释放锁并清空残留，优先避免永久 WakeLock。若 ref 仍非零但 list 为空，只记录问题并把 WorkSource 清回 OS；它不会擅自把 ref 改零。

## 12. 广播和 Doze 为什么需要 InFlight 边沿通知

只有 `PendingIntent.isBroadcast()` 的 InFlight 会通知 `AlarmManagerInternal.InFlightListener`：

```text
broadcastAlarmPending(uid)
broadcastAlarmComplete(uid)
```

`BroadcastDispatcher` 按 UID 维护 `mAlarmUids`。当某 UID 正在接收 Alarm 广播时，它会把该 UID 已被慢 receiver 策略延迟的广播临时放进 `mAlarmBroadcasts` 快速队列；全部 Alarm 广播完成后再回普通 deferral。

接口参数名叫 `recipientUid`，但 r48 实际传的是 `alarm.uid`，即登记 calling UID，不一定是 PendingIntent creator/真实 receiver UID。普通自调度两者相同；代理设置场景中 fast-track 归属可能落在登记者。这是实现身份边界，不是理想化命名。

所有类型的共享 ref 从 0→1、1→0 时还会经 Alarm Handler 向 `DeviceIdleController` 报告 `setAlarmsActive(true/false)`。DIC 只在 alarms、jobs 和 active idle ops 都空闲时尝试提前结束 maintenance；这组反馈不等同于 broadcast UID 通知。

## 13. r48 最值得警惕的三类协议边界

第一类是 unmatched PendingIntent callback：

```java
updateTrackingLocked(removeLocked(pi, intent));
```

`removeLocked()` 返回 null 时，`updateTrackingLocked(null)` 仍会无条件 `mBroadcastRefCount--`。重复或不匹配 callback 会破坏 ref/list 不变量，甚至使 ref 变成负数，影响后续 0→1 acquire 判断。正常 finishedReceiver 由 system_server 自己构造并在受控协议中传递，代码依赖这一可信前提。

listener 更防御：late completion、spurious timeout 或 null token 都不会在找不到 InFlight 时减 ref。

第二类是同 token 重叠：等待队列中同 listener 会 replacement，但旧 occurrence 已 InFlight 后仍可再 set 并触发下一次。`alarmComplete(who)` 会移除所有 `LISTENER_TIMEOUT` 且 `obj == who` 的消息，却只删除第一个匹配 InFlight；另一代可能失去 timeout。客户端 wrapper 还共享可变 `mHandler` 和 `mCompletion`。同一个 listener 对象不适合作为多个并行未完成代际。

同一个 PendingIntent 对象也可有多个 InFlight；callback 没有 generation ID，服务删除第一个 `== pi` 的项。次数平衡时总 ref 能闭合，但单项投递时刻与完成的代际配对可能不精确。

第三类是外部记账失败：外层在尝试发送前已调用 `ActivityManager.noteAlarmStart()`，而 `noteAlarmFinish()` 只在找到 InFlight 的完成路径执行。CanceledException 或 listener 同步失败不建 InFlight，可能留下 BatteryStats active-event start/finish 不对称；这不应直接夸大成“必然多记一段具体耗电时长”，但它是 r48 的诊断边界。

## 14. 用三个场景检验模型

场景一：service PendingIntent 成功启动。

```text
send 请求交接 → FinishedDispatcher post OnFinished
  → 建立 InFlight/WakeLock
  → Handler 销账并可能很快 release
  → Service 后续业务仍按组件规则继续
```

不能依赖 Alarm WakeLock 覆盖 Service 的全部异步工作。

场景二：listener 主线程堵塞六秒。

```text
oneway 事务提交成功 → 建 InFlight，启动五秒 timeout
  → timeout 找到 InFlight，ref--/release
  → 主线程以后执行 onAlarm 并回 completion
  → completion 找不到，按 late 忽略
```

timeout 不是取消 App Runnable，也不是 ANR 判定。

场景三：broadcast A 和 listener B 同时在飞。

```text
A：ref 0→1，acquire；另通知 BroadcastDispatcher UID pending
B：ref 1→2，不重复 acquire
B timeout：ref 2→1，WakeLock 归因给 A
A broadcast 完成：UID complete，ref 1→0，release
```

广播 UID 计数、全局 ref 和两类 send/finish counter 是相关但不同的账。

## 15. 静态阅读与诊断方法

先定位发送、完成和归因：

```bash
rg -n "deliverAlarmsLocked|class DeliveryTracker|updateTrackingLocked" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "FinishedDispatcher|sendFinish|finishedReceiver.performReceive" \
  frameworks/base/core/java/android/app/PendingIntent.java \
  frameworks/base/services/core/java/com/android/server/am/PendingIntentRecord.java

rg -n "LISTENER_TIMEOUT|alarmComplete|alarmTimedOut" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "broadcastAlarmPending|mAlarmUids|mAlarmBroadcasts" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java \
  frameworks/base/services/core/java/com/android/server/am/BroadcastDispatcher.java
```

`dumpsys alarm` 中优先建立这组不变量：

```text
Broadcast ref count ≈ Outstanding deliveries 数量
PendingIntent send - finish ≈ 未完成 PI InFlight
Listener send - finish ≈ 未完成 listener InFlight
InFlight.when = 实际发起时间，不是请求 deadline
WakeLock WorkSource = 当前队首代表，不是所有并发项逐项展开
```

若 PI 差值长期不归零，先看 target type：broadcast 要继续追 BroadcastQueue；service/activity 只应等待请求 handoff。若 listener 差值持续超过 timeout，检查 Handler 是否处理 timeout、token 是否重叠，以及 ref/list 是否已经失配。

## 16. 结论与下一章

完整闭环可以压缩为：

```text
等待 Alarm 到期
  → 尝试 PI send 或 listener oneway
  ├─ 同步失败：finish counter 就地闭合，不建 InFlight
  └─ 成功：第一个 acquire，共享 ref/list/stats 建账

PI 完成
  → broadcast 等广播链；其他类型通常等请求交接
Listener 完成
  → alarmComplete 或默认五秒 timeout

每完成一个
  → 移除一个 InFlight、stats nesting--、ref--
  ├─ ref > 0：归因给剩余队首
  └─ ref = 0：release，并报告 alarms inactive
```

最终应记住六点：

1. `mAlarmsPerUid` 是等待账，`mBroadcastRefCount`/InFlight 是执行账；
2. 目标成功发起后才建立 InFlight 和 WakeLock，发送失败只闭合尝试计数；
3. broadcast、activity/service 和 listener 的完成点不同，Alarm WakeLock 不覆盖所有业务生命周期；
4. listener timeout 只结束 AlarmManager 跟踪，不停止 App 代码，也不在 r48 直接触发 ANR；
5. 共享 WakeLock 按 ref 保护全部 InFlight，却只能用队首近似归因；
6. unmatched PI callback、同 listener 重叠 timeout、同步失败的 noteAlarm 账是 r48 的关键健壮性边界。

下一章把这些状态变成诊断证据：怎样联合等待 Batch、四类 pending、Java/kernel deadline、InFlight、send/finish 差值和 Proto 输出，定位一枚 Alarm 究竟卡在哪一层。
