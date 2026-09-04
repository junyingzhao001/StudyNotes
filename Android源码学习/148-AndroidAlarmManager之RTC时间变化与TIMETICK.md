# 148 Android AlarmManager：RTC、系统时间变化与 TIME_TICK

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 145～147 章

---

## 1. 本章只回答一个问题

应用设置了一枚“今天 18:00”的 `RTC_WAKEUP` Alarm。17:00 时它在服务内部看起来是“elapsed 再过一小时”；随后用户把系统时间拨到 18:10：

- 为什么服务不能继续使用旧 deadline？
- 内核怎样告诉 Java 墙上时间跳了？
- 为什么 RTC Alarm 要重排，而“30 分钟后重试”的 ELAPSED Alarm 不动？
- `TIME_TICK` 和 `DATE_CHANGED` 又怎样重新对齐？

Android 11 的主线是：

```text
App 用 RTC 表达墙上时刻
  → AMS 投影成 elapsed deadline，与其他 Alarm 统一合批
墙上时间发生跳变
  → cancel-on-set timerfd 返回 ECANCELED
  → AlarmThread 过滤小幅通知并确认变化
  → 从 origWhen 重新投影主 Batch
  → 重建 TIME_TICK / DATE_CHANGED
  → 广播 TIME_SET 并检查已经到期的 Alarm
```

一句话结论：

> RTC 是需求的时间语言，elapsed 是 AlarmManagerService 的统一调度坐标；改钟后必须回到原始 RTC 目标重新投影，不能修补旧的 elapsed 结果。

## 2. 四种 type 与三根时钟先分清

| Alarm type | API 传入的基准 | 到点可唤醒 suspend 中的设备 |
|---|---|---|
| `RTC_WAKEUP` | `System.currentTimeMillis()` | 是 |
| `RTC` | `System.currentTimeMillis()` | 否 |
| `ELAPSED_REALTIME_WAKEUP` | `SystemClock.elapsedRealtime()` | 是 |
| `ELAPSED_REALTIME` | `SystemClock.elapsedRealtime()` | 否 |

三根常见时钟的区别是：

```text
currentTimeMillis：Unix Epoch 墙上时间，可被人工或校时服务跳变
elapsedRealtime：本次启动后经过时长，包含 deep sleep，不随改钟跳变
uptimeMillis：本次启动后 CPU 清醒时长，不包含 deep sleep
```

Alarm 主调度选择 elapsed，而不是 uptime，因为设备休眠的一小时也应该算进“距离 Alarm 到期还剩多久”。

选择 API 时先问需求：

```text
“30 分钟后重试”       → elapsed 语义
“2026-09-04 18:00”    → RTC/Epoch 语义
“每天当地 08:00”      → 保存日历规则，每次计算下一枚 RTC
```

这里的 `RTC` 是 Android API 对 wall-clock 时间基准的名称，不等于“每枚 App Alarm 都直接写一个硬件 RTC 闹钟”。

## 3. RTC 怎样投影到统一的 elapsed 时间轴

换算只有几行：

```java
private long convertToElapsed(long when, int type) {
    if (type == RTC || type == RTC_WAKEUP) {
        when -= currentTimeMillis() - elapsedRealtime();
    }
    return when;
}
```

写成公式更容易记：

```text
目标 elapsed
= 目标 RTC - (当前 wall - 当前 elapsed)
= 当前 elapsed + (目标 RTC - 当前 wall)
```

例如：

```text
当前 wall     = 1,700,000,000,000
当前 elapsed  =           500,000
目标 RTC      = 1,700,000,600,000

目标 elapsed  = 1,100,000
```

也就是从当前 elapsed 再过十分钟。

`Batch.start/end` 的注释明确说端点永远是 elapsed。这样 RTC 与 ELAPSED Alarm 才能在同一结构中比较顺序、求窗口交集、应用 Doze/App Standby 政策，并最终选出最近的 wakeup 与 non-wakeup deadline。

## 4. 为什么 `origWhen` 不能丢

`Alarm` 保留两类时间：

| 字段 | 含义 |
|---|---|
| `origWhen` | 当前 Alarm 对象/当前 repeat occurrence 的原始 type 基准时间 |
| `when` | 当前使用的原始基准时间，重加时先恢复为 `origWhen` |
| `expectedWhenElapsed` / `expectedMaxWhenElapsed` | 原始需求投影成 elapsed 后、App Standby 前的窗口 |
| `whenElapsed` / `maxWhenElapsed` | 当前政策修正后真正参与 Batch 的窗口 |

如果用户把 17:00 直接拨到 18:10，目标 18:00 的旧 elapsed 投影还写着“再等一小时”，已经失真。正确做法不是给旧 deadline 猜一个偏移，而是：

```java
a.when = a.origWhen;
long whenElapsed = convertToElapsed(a.when, a.type);
```

RTC 类型用新的 wall/elapsed 差重新投影；ELAPSED 类型由 `convertToElapsed()` 原样返回，deadline 不随改钟移动。

`reAddAlarmLocked()` 随后重建 exact 或正窗口，并再次进入 Doze、App Standby 等政策。它不是“改钟后绕过其他限制”的快捷通道。

还有一个 r48 细节：heuristic window 在最初 `setImpl()` 时已经计算为正的固定宽度并保存。后续 rebatch 通常保留这个宽度，不会按新的 futurity 再计算一次 75% 窗口。

## 5. 内核怎样报告墙上时间变化

JNI 创建六个 timerfd：

```cpp
CLOCK_REALTIME_ALARM,
CLOCK_REALTIME,
CLOCK_BOOTTIME_ALARM,
CLOCK_BOOTTIME,
CLOCK_MONOTONIC,
CLOCK_REALTIME,       // 专门监听 RTC change
```

前五项保留历史 alarm type 槽位；第六项是额外的时间变化监听 fd。它不是“第六枚业务 Alarm”。

初始化时，第六个 fd 被设为：

```cpp
timerfd_settime(fd,
        TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
        &zeroSpec, nullptr);
```

`zeroSpec` 让 timer 保持 disarmed；r48 源码依赖的 timerfd 语义是：不需要真正的到期 deadline，只要将它配置为 cancelable，RTC 被离散设置时就能得到通知。

`epoll_wait()` 唤醒后读取 fd。若第六个 fd 返回 `ECANCELED`，JNI 设置 bit 16：

```cpp
result |= ANDROID_ALARM_TIME_CHANGE_MASK; // 1 << 16
```

其他 fd 则设置各自的 type bit。同一次 epoll 结果可以同时包含“时间改变”和“Alarm 到期”。Java `AlarmThread` 收到的是 bitmask，不是一个 Alarm 对象。

## 6. `AlarmThread` 怎样确认并处理一次改钟

内核可能报告很小的内部校正。Java 先根据两根不变量估算正常情况下的墙上时间：

```java
expectedClockTime = mLastTimeChangeClockTime
        + (nowElapsed - mLastTimeChangeRealtime);
```

判断条件是：

```java
lastTimeChangeClockTime == 0
        || nowRTC < expectedClockTime - 1000
        || nowRTC > expectedClockTime + 1000
```

所以 r48 的准确边界是：

- 第一次收到时间变化通知，无条件处理；
- 后续偏差要严格超过负一秒或正一秒；
- 恰好 `±1000 ms` 不满足实现中的不等式。

源码注释写“at least +/- 1000 ms”，与严格比较略有出入；精确分析应以条件表达式为准。

确认是真实变化后，顺序为：

```text
记录 WALL_CLOCK_TIME_SHIFTED 统计
  → 删除旧 TIME_TICK listener Alarm
  → 删除旧 DATE_CHANGED PendingIntent Alarm
  → rebatchAllAlarms()
  → 安排新的 TIME_TICK 与 DATE_CHANGED
  → 更新最后一次 wall/elapsed 快照与计数
  → 广播 ACTION_TIME_CHANGED（action 字符串为 TIME_SET）
  → result |= IS_WAKEUP_MASK，强制检查当前已到期 Alarm
```

先删 tick/date 是为了避免旧实例参与 rebatch 后，又新增一份按新时间计算的实例。

`rebatchAllAlarmsLocked()` 的“all”主要指主 `mAlarmBatches`：它拆出其中每枚 Alarm 再插入，并维护 idle 特殊引用；pending-background、pending-non-wakeup 等容器有自己的恢复时机，不能把 all 理解成所有 ArrayList 都逐项重算。

如果通知只有 `TIME_CHANGED_MASK`，却没有越过阈值，服务不 rebatch、不发广播，最后只重新设置 kernel deadline。若 bitmask 还含普通 Alarm bit，那些到期 Alarm 仍会进入正常检查。

## 7. 向前、向后拨钟分别发生什么

目标都是墙上时间 18:00。

向前拨：

```text
原来 wall=17:00 → 旧投影为 elapsed+60min
突然 wall=18:10 → 新投影为 elapsed-10min
```

RTC deadline 已在过去。时间变化分支强制进入 `triggerAlarmsLocked()`，所以它可以在这一轮被取出；但 Doze、App Standby、background restriction 等仍可能继续推迟实际交付。

向后拨：

```text
原来 wall=17:50 → 旧投影为 elapsed+10min
突然 wall=16:50 → 新投影为 elapsed+70min
```

RTC Alarm 被推后。同一时刻设置的“elapsed+10min”Alarm 仍按启动后时间前进，不随墙钟改变。

Java 重排后也不会让每枚 RTC Alarm 独占内核 timerfd。`rescheduleKernelAlarmsLocked()` 仍只从 Batch 中选出最近的 wakeup 和 non-wakeup 主截止点，分别设置 `ELAPSED_REALTIME_WAKEUP` 与 `ELAPSED_REALTIME` 对应的内核入口。

## 8. `setTime()` 为什么不直接调用 rebatch

Binder 服务入口先要求 `android.permission.SET_TIME`，普通三方应用无法获得这项 signature/privileged 能力。

`setTimeImpl()` 的主体是：

```text
alarm driver 不存在 → 返回 false
否则持锁：
  记录旧 wall
  调 native 设置新 wall
  若新旧时刻跨过时区 offset/DST 边界，刷新 kernel timezone
  返回 true
```

它没有直接调用 `rebatchAllAlarms()`，也没有直接发送 `TIME_SET`。设置 `CLOCK_REALTIME` 后，第六个 cancel-on-set timerfd 唤醒 `AlarmThread`，再由上一节的统一链处理。这样无论时间变化来自这个 Binder API、网络校时还是其他系统来源，Alarm 重排都由同一个内核事实入口驱动。

公开 `AlarmManager.setTime()` 返回 `void`；AIDL/服务内部的 boolean 没有暴露给普通调用方。即使在服务内部，只要 alarm driver 存在，代码也忽略 native 结果并返回 `true`，因为注释指出 native 可能在 wall clock 已成功修改后仍因后续步骤失败返回 `-1`。

native `setTime()` 其实分两步：

```text
settimeofday() 改系统墙钟
  → 找到 /sys/class/rtc 中 hctosys 对应的 /dev/rtcN
  → gmtime_r 转 UTC
  → RTC_SET_TIME 写硬件 RTC
```

后半段失败时前半段可能已成功；反过来，服务端返回 true 也不能证明每个 native 步骤成功。JNI 还拒绝 `millis <= 0` 或秒数 `>= INT_MAX` 的输入。

服务启动时若当前 wall 早于以下三者的最大值，也会尝试把时间推进到该下界：

```text
ro.build.date.utc
根文件系统最后修改时间
Build.TIME
```

这只能避免明显过早的时钟，不代表已经完成可信网络校时，也不检查荒谬的未来时间。

## 9. 改时区为什么不是改时间

切换默认时区通常不改变 UTC Epoch 毫秒，只改变“这个瞬间在当地显示几点”。因此已经登记的普通 RTC Alarm 仍指向同一个 Epoch，不需要全量 rebatch。

服务端 `setTimeZone()` 要求 `SET_TIME_ZONE` 权限，并在清除 Binder calling identity 后执行：

```text
空字符串 → 返回
TimeZone.getTimeZone(tz)
  → 如 ID 变化，写 persist.sys.timezone
  → 按当前 wall 计算 GMT offset
  → 以 minutes west 的相反符号更新 kernel timezone
  → TimeZone.setDefault(null) 清 Java 默认时区缓存
  → 如 ID 确实变化，重排 DATE_CHANGED 并广播 TIMEZONE_CHANGED
```

Java offset 表达“当地相对 GMT 向东为正”；Linux `tz_minuteswest` 表达“位于 GMT 以西多少分钟”，所以 UTC+8 的 `+480` 要写成 `-480`。

客户端 `AlarmManager.setTimeZone()` 对 target SDK >= M 的调用方先验证 Olson/IANA ID；旧 target 可能落入 `TimeZone.getTimeZone()` 的兼容回退。不要把客户端版本校验误说成服务端对所有入口都严格拒绝未知字符串。

即使传入 ID 与 property 相同，服务端仍会刷新 kernel offset 和 Java 缓存；只有 ID 真变化时才重排午夜 Alarm 和发广播。

## 10. `TIME_TICK` 怎样始终寻找下一整分钟

系统在 `SYSTEM_SERVICES_READY` 阶段安排第一枚 tick。算法先在 wall clock 上找下一整分钟，再把剩余时长放到 elapsed 时间轴：

```java
long nextTime = 60000 * ((currentTimeMillis() / 60000) + 1);
long delay = nextTime - currentTimeMillis();

setImpl(ELAPSED_REALTIME,
        elapsedRealtime() + delay,
        0, 0,
        null, mTimeTickTrigger, "TIME_TICK",
        FLAG_STANDALONE, ...);
```

它不是公开 `setRepeating()`。每次 internal listener 到期后会：

```text
记录 mLastTickReceived
立即按此刻 wall/elapsed 计算下一整分钟 Alarm
Handler 异步发送 ACTION_TIME_TICK 给所有用户
调用 alarmComplete，为本次 InFlight 收账
```

重新计算而不是固定加 60 秒，可以在延迟或改钟后重新对齐墙上分钟。

`TIME_TICK` Intent 带有：

```text
FLAG_RECEIVER_REGISTERED_ONLY
FLAG_RECEIVER_FOREGROUND
FLAG_RECEIVER_VISIBLE_TO_INSTANT_APPS
```

它只能由运行时注册的 receiver 接收，不能靠 manifest receiver 做每分钟保活。

这枚 Alarm 使用 non-wakeup 的 `ELAPSED_REALTIME`。设备 suspend 时不会仅为 tick 唤醒，醒来后才可能处理过期项并计算下一分钟。因此它是 UI/系统时间刷新信号，不是可靠的后台每分钟调度器。

## 11. `DATE_CHANGED` 怎样对齐下一当地零点

午夜目标用 `Calendar` 计算：

```java
Calendar c = Calendar.getInstance();
c.setTimeInMillis(currentTimeMillis());
c.set(HOUR_OF_DAY, 0);
c.set(MINUTE, 0);
c.set(SECOND, 0);
c.set(MILLISECOND, 0);
c.add(DAY_OF_MONTH, 1);
```

然后设置一枚 exact、standalone、`RTC`、non-wakeup 的系统 `PendingIntent` Alarm。使用日历加一天而不是固定 `24h`，才能表达遇到时区/DST 规则时的“下一当地零点”。

`ClockReceiver` 收到 `ACTION_DATE_CHANGED` 后，会按当前日期的时区 offset 刷新 kernel timezone，再安排下一天。这是因为内核不会替 Java userspace 跟踪完整的夏令时规则。

TIME_TICK 与 DATE_CHANGED 的对照是：

| 项目 | `TIME_TICK` | `DATE_CHANGED` |
|---|---|---|
| 对齐目标 | 下一 wall 整分钟 | 下一当地零点 |
| 内部 type | `ELAPSED_REALTIME` | `RTC` |
| wakeup | 否 | 否 |
| 载体 | internal listener | 系统 PendingIntent |
| 续排时机 | listener 到期 | receiver 收到日期广播 |
| 改时间 | 删除后重建 | 删除后重建 |
| 改时区 | 没有专门重建 | 立即替换为新当地零点 |

`setTimeZoneImpl()` 再次调用 `setImpl()` 时，相同 `PendingIntent` identity 会先移除旧 DATE_CHANGED，因此不是简单追加第二枚。

## 12. 固定 24 小时 repeat 为什么不等于每天 08:00

重复 Alarm 到期后会根据原相位与漏过的 interval 算下一次：

```java
count += (nowElapsed - expectedWhenElapsed) / repeatInterval;
delta = count * repeatInterval;
nextElapsed = expectedWhenElapsed + delta;
nextWhen = alarm.when + delta;
```

它不是简单从“本次实际交付时刻”再加一个周期，因此普通延迟不会持续累积相位漂移。

但固定 `24h` 表达的是经过时长，不是日历规则。遇到夏令时的当地一天可能是 23 或 25 小时；改时区后原 Epoch 也不会自动变成新时区的 08:00。

真正表达“每天当地 08:00”的业务应保存规则，而不是只保存一次投影：

```text
按目标日期和时区算下一次 08:00 Epoch
  → 登记一次性 RTC Alarm
  → 正常触发后再算下一次
  → TIME_SET / TIMEZONE_CHANGED / BOOT_COMPLETED 后重新计算
```

是否选 wakeup、exact、AlarmClock 或普通窗口，是用户可见性与功耗政策的另一层决策。

## 13. 休眠、改钟、改时区和重启的边界

| 事件 | wall | elapsed | 已登记 RTC Alarm | 已登记 ELAPSED Alarm |
|---|---|---|---|---|
| deep sleep 一小时 | 正常前进 | 增长一小时 | 按 wakeup 属性处理 | 按 wakeup 属性处理 |
| 向前/向后改钟 | 跳变 | 不跳 | 从 `origWhen` 重投影 | 原 deadline 不变 |
| 改默认时区 | UTC 通常不跳 | 不跳 | 原 Epoch 不变 | 不变 |
| reboot | 重新建立 | 从零开始 | 普通登记丢失 | 普通登记丢失 |

四种普通应用 Alarm 都不会跨 reboot 自动恢复。持久化旧的 elapsed 数值尤其没有意义，因为它属于上一次 boot。业务应持久化“当地 08:00”“创建后 30 分钟”等规则和必要上下文，在合适的启动事件后重算。

若 native alarm driver 初始化失败，AMS 会退化到 Handler 调度，但 Handler 不能在 suspend 中提供等价的 wakeup，也没有 cancel-on-set timerfd。`setTimeImpl()` 此时直接返回 false。这是有限降级，不是完整等价实现。

## 14. 静态阅读与诊断方法

先追四条证据链：

```bash
rg -n "convertToElapsed|origWhen|reAddAlarmLocked|rebatchAllAlarms" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "TFD_TIMER_CANCEL_ON_SET|ECANCELED|TIME_CHANGE_MASK" \
  frameworks/base/services/core/jni/com_android_server_AlarmManagerService.cpp \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "setTimeImpl|setTimeZoneImpl|WALL_CLOCK_TIME_SHIFTED" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "scheduleTimeTickEvent|scheduleDateChangedEvent|mTimeTickTrigger" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

看到 RTC Alarm 时间异常时，依次回答：

```text
1. 业务表达的是绝对 Epoch、当地日历规则，还是经过时长？
2. origWhen、expectedWhenElapsed、actual whenElapsed 各是多少？
3. 最近是否确认过 wall-clock shift，mNumTimeChanged 是否变化？
4. 是改时间还是只改时区？
5. rebatch 后是否又被 Doze、App Standby 或 background restriction 推迟？
6. Alarm 是否位于不参加主 rebatch 的 pending 容器？
7. 设备只是 suspend，还是已经 reboot 丢失内存登记？
```

手算练习：当前是 `23:59:40.250`，下一 tick 的 delay 是约 `19.750s`；下一 DATE_CHANGED 是当前默认时区日历的次日 `00:00:00.000`。两者都是 non-wakeup，不能据此承诺设备会在物理边界瞬时唤醒。

## 15. 结论与下一章

完整模型可以压缩为：

```text
RTC 请求
  → 保存 origWhen(Epoch)
  → 用 wall-elapsed offset 投影
  → 与 ELAPSED Alarm 统一进入 elapsed Batch
  → Java 只选择最近 wakeup/non-wakeup deadline 给内核

CLOCK_REALTIME 被离散修改
  → cancel-on-set fd / ECANCELED / bit 16
  → 严格超过 ±1s 或首次事件才确认
  → 删除 tick/date、重投影主 Batch、重建 tick/date
  → TIME_SET、检查当前到期 Alarm

时区改变
  → Epoch 通常不动，不全量移动普通 RTC Alarm
  → 更新 property/kernel offset/cache
  → 替换下一当地零点并发 TIMEZONE_CHANGED
```

最终应记住六点：

1. RTC 是 wall-clock 表达，AMS 主 Batch 的统一坐标是 elapsed；
2. 改钟后从 `origWhen` 重投影，ELAPSED deadline 不随之移动；
3. `setTimeImpl()` 不直接 rebatch，统一由 cancel-on-set 事件驱动 AlarmThread 收敛；
4. TIME_TICK 与 DATE_CHANGED 都是 non-wakeup 自续排系统 Alarm，不能当硬实时边界；
5. 改时区改变当地日历解释，不自动把旧 Epoch 变成新时区的同一钟点；
6. reboot 会清空普通 Alarm 登记，业务应保存规则并重新计算，而不是复用旧 elapsed 值。

下一章继续处理生命周期问题：取消一个 `PendingIntent` 为什么能命中原 Alarm；listener Binder 死亡、force-stop、包卸载和用户停止又分别清理哪些容器与身份范围。
