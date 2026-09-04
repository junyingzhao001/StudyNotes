# 147 Android AlarmManager：App Standby 配额与重排

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 122、123、145、146 章

---

## 1. 本章只回答一个问题

一个 `FREQUENT` 应用已经在一小时内成功收到两轮 Alarm。它现在又调用了一次 `setExact()`：

- API 为什么仍然接受请求？
- exact 为什么仍可能晚一小时？
- 应用升到 `ACTIVE` 或设备开始充电后，原时间为什么又可能恢复？

Android 11 的答案不是“配额满就拒绝登记”，而是：

```text
用 source package + creator user 的成功交付历史计算下一次有额度的时刻
  → 保留 expected 时间
  → 把实际 when/max 推迟到额度恢复点
  → bucket、充电或新交付改变事实后，移出旧 Batch 再重新插入
```

本章读完后，应能根据 bucket、历史时间点和当前容器，手算一枚 Alarm 的实际时间，并解释为什么“API 成功”“exact”“当前 dump 时间”可以同时成立。

## 2. 先把四套限制拆开

AlarmManager 中至少有四张不同的账：

| 机制 | 账户键 | 限制什么 | 命中后的动作 |
|---|---|---|---|
| 每 UID 登记上限 | calling UID | 当前登记的 Alarm 数 | `set()` 抛异常 |
| App Standby 配额 | source package + creator user | 成功发起的非豁免交付时间点 | 推迟实际时间 |
| AllowWhileIdle 频率门 | creator UID | 两次普通 AWI 成功交付的间隔 | 推迟到 `minTime` |
| background restriction | creator UID/source package 的当前状态 | 此刻是否允许后台 Alarm | 移入 pending-background |

同一枚 Alarm 可以连续经过多道门。普通 AWI 虽能穿过 Doze 的 idle 挂起门，仍可能先被 App Standby 改时间、到期时再被 AWI 频率门改一次。

本章核心文件：

```text
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/services/tests/mockingservicestests/src/com/android/server/AlarmManagerServiceTest.java
frameworks/base/core/java/android/app/usage/UsageStatsManager.java
frameworks/base/apex/jobscheduler/framework/java/com/android/server/usage/AppStandbyInternal.java
```

## 3. 默认配额和账户身份是什么

r48 的普通滚动窗口默认是一小时：

| bucket | 一小时内默认额度 |
|---|---:|
| `ACTIVE` | 720 |
| `WORKING_SET` | 10 |
| `FREQUENT` | 2 |
| `RARE` | 1 |
| `NEVER` | 0 |

`RESTRICTED` 是单独分支，默认一次/24 小时。

普通 bucket 的数值映射不是 `switch`，而是区间判断：

```text
<= ACTIVE       → ACTIVE
<= WORKING_SET  → WORKING_SET
<= FREQUENT     → FREQUENT
<  NEVER        → RARE
其他            → NEVER
```

`RESTRICTED` 的数值虽位于 `RARE` 与 `NEVER` 之间，`adjustDeliveryTimeBasedOnBucketLocked()` 会先识别它，所以不会落入普通数组的 RARE 档。

配额身份来自 `Alarm` 的两项：

```java
sourcePackage = operation != null
        ? operation.getCreatorPackage() : callingPackage;
creatorUid = operation != null
        ? operation.getCreatorUid() : callingUid;
```

随后账户键是：

```java
Pair.create(alarm.sourcePackage,
        UserHandle.getUserId(alarm.creatorUid))
```

这使代理携带别人创建的 `PendingIntent` 设置 Alarm 时，配额归到真正可能被唤醒的 source package/user，而不是只归到代理的 calling package。

## 4. `AppWakeupHistory` 记录的并非硬件唤醒次数

内存结构是：

```text
ArrayMap<Pair<package, user>, LongArrayQueue>
```

每个队列保存 `elapsedRealtime` 时间戳，不跨 system_server 重启持久化。名字叫 `AppWakeupHistory`，但写入处并不检查 `alarm.wakeup`：RTC、ELAPSED、wakeup、non-wakeup 只要是非豁免 Alarm 且成功建立 InFlight，都会记账。

direct listener 也会记账。它没有 `PendingIntent` 时，`sourcePackage` 和 `creatorUid` 就回到 calling package/UID。

同一账户在同一个 elapsed 毫秒只加一个时间点：

```java
if (history.size() == 0 || history.peekLast() < nowElapsed) {
    history.addLast(nowElapsed);
}
```

`deliverAlarmsLocked()` 给同一轮 trigger list 传相同的 `nowElapsed`，因此同包同用户同一轮交付多枚 Alarm，通常只消耗一个历史时间点。这里的配额更接近“交付轮次”，不是严格的 Alarm 对象计数。

写历史发生在 `PendingIntent.send()` 或 `listener.doAlarm()` 没有立即失败、并已建立 InFlight 之后，但不等待 receiver finished、listener complete 或业务成功。系统已经给出一次交付机会就会记账；立即取消的 `PendingIntent` 或建立 InFlight 前抛错则不记。

## 5. 哪些 Alarm 不进入这张账

豁免公式只有三项：

```java
return a.alarmClock != null
        || UserHandle.isCore(a.creatorUid)
        || (a.flags & FLAG_ALLOW_WHILE_IDLE_UNRESTRICTED) != 0;
```

因此：

- `setAlarmClock()` 豁免；
- core creator UID 豁免；
- 服务端授予的 unrestricted AWI 豁免；
- 普通 `FLAG_ALLOW_WHILE_IDLE` 不豁免；
- exact 和 wakeup 也不自动豁免。

这再次说明几个维度彼此正交：exact 控制原始窗口，wakeup 控制设备 suspend，AWI 控制 idle 穿透，App Standby 则按 source 账户限制交付轮次。

## 6. 普通 bucket 怎样算下一次有额度的时刻

假设当前 bucket 额度为 `q`，历史中至少已有 `q` 个时间点。源码取第 `q` 个倒数时间 `t(q)`：

```java
long t = history.getNthLastWakeupForPackage(
        sourcePackage, sourceUserId, q);
long minElapsed = t + APP_STANDBY_WINDOW;
```

公式是：

```text
nextAllowed = 第 q 个倒数交付时间 + 滚动窗口
```

为什么是第 `q` 个倒数？因为它是当前占用这 `q` 个名额中最老的时间点。等它滑出窗口，才释放一个名额。

例如 `FREQUENT` 的 `q=2`，历史为第 10、40 分钟，窗口 60 分钟：

```text
第 2 个倒数 = 10
nextAllowed = 10 + 60 = 第 70 分钟
```

若新 Alarm 的 `expectedWhenElapsed < 70`，实际时间会推到 70；若应用原本就要求 80，则保持 80，政策不会把请求拉早。

边界比较值得单独记住：

```java
if (alarm.expectedWhenElapsed < minElapsed) { ... }
```

是严格小于，不是小于等于。因此恰好在 `t(q)+window` 的 Alarm 可以保留该时刻。

历史裁剪同样使用严格小于：

```java
while (first + window < last) {
    removeFirst();
}
```

所以恰好相隔一个窗口的两端会暂时同时保留。这是 r48 的闭边界实现细节；不要只用含糊的“过去一小时”替代手算。

## 7. `NEVER` 与 `RESTRICTED` 为什么单独看

`NEVER` 默认 `q=0`，没有“第 0 个倒数”可取。源码直接令：

```java
minElapsed = nowElapsed + MILLIS_IN_DAY;
```

若一天后仍是 `NEVER`，再次评估又会从新的 `now` 向后推一天。它接近“持续没有交付资格”，不是“冷却 24 小时后必定交付”。若应用原始 expected 本来更晚，仍不会被拉早。

`RESTRICTED` 默认一次/24 小时，读取独立的 quota 和 window：

```text
已有历史
  → 取 restrictedQuota 对应的倒数时间
  → 仅当 now - last < restrictedWindow 时仍视为窗口内
  → expected 早于 last + window 才推迟
```

它显式比较当前 `now`，是因为 history getter 本身不会按当前时刻清理陈旧项。默认 quota 为 1 时，最后一次时间就足够做判断。

## 8. `expected` 与实际时间为什么必须分账

`Alarm` 同时保存两组 elapsed 时间：

```text
expectedWhenElapsed / expectedMaxWhenElapsed
    App 原始需求经时钟转换、MIN_FUTURITY 和窗口计算后的结果

whenElapsed / maxWhenElapsed
    当前政策修正后真正参与 Batch 与 kernel deadline 的结果
```

额度不足时：

```java
alarm.whenElapsed = alarm.maxWhenElapsed = minElapsed;
```

政策把实际窗口压成一个点，但不会修改原 `windowLength`，也不会添加 `FLAG_STANDALONE`。因此 dump 中 `when == max` 不足以反推应用调用了 `setExact()`。

额度重新充足或处于 charging parole 时，源码恢复：

```java
alarm.whenElapsed = alarm.expectedWhenElapsed;
alarm.maxWhenElapsed = alarm.expectedMaxWhenElapsed;
```

这就是 bucket 升级后 Alarm 能回到原请求时间的基础。如果 expected 已经在过去，恢复不等于在 Handler 调用栈中同步交付；它只是重入近期调度，仍需 kernel/AlarmThread 与后续政策继续推进。

## 9. 哪些事件会重新计算已有 Alarm

第一次评估发生在普通 Alarm 插入主 Batch 前：

```text
setImplLocked()
  → adjustDeliveryTimeBasedOnBucketLocked()
  → insertAndBatchAlarmLocked()
```

如果历史已满，新 Alarm 从一开始就使用推迟后的实际时间，不必先设置一个无意义的早期 kernel deadline。

一轮正常 AlarmThread 交付还有闭环：

```text
从 triggerList 预收集所有非豁免 source package/user
  → deliverAlarmsLocked()：成功建立 InFlight 的 Alarm 写 history
  → 对预收集账户调用 reorderAlarmsBasedOnStandbyBuckets()
  → 重设 kernel Alarm 与 next alarm clock
```

候选账户是在发送前收集的，所以即使某一枚发送失败，该账户也可能被重算；只是失败项不会新增历史。

重排不能在原 Batch 中就地改时间。实现会倒序遍历 `mAlarmBatches`，将变化的 Alarm 从旧 Batch 移出，最后调用 `insertAndBatchAlarmLocked()` 重新插入。否则 Batch 的窗口交集、`start/end` 和全局时序都可能失真。

`targetPackages` 决定范围：

- 正常交付后，重排这轮涉及的账户；
- bucket 变化，重排一个 package/user；
- charging 状态变化，传 `null` 做全量重排。

## 10. bucket 与充电变化怎样影响时间

AMS 在 `SYSTEM_SERVICES_READY` 后注册 `AppIdleStateChangeListener`。bucket 回调不直接扫描，而是向 Alarm Handler 发消息；Handler 再持 AMS 锁定向重排并按需更新 kernel deadline。

r48 有一个容易漏看的消息合并边界：

```java
mHandler.removeMessages(APP_STANDBY_BUCKET_CHANGED);
mHandler.obtainMessage(... packageName ...).sendToTarget();
```

`removeMessages(what)` 会删除所有尚未处理的同类消息，不只删除相同包。若 A、B 两个包快速连续变化，B 可能取消 A 的定向重排。A 的真实 bucket 没丢；后续 set、恢复或其他重排会读取最新事实，但 A 已有的远期 Alarm 可能暂时保持旧 actual 时间。

charging receiver 也经 Handler 更新：

```java
mAppStandbyParole = charging;
reorderAlarmsBasedOnStandbyBuckets(null);
```

插电时跳过 App Standby 时间门并恢复 expected；拔电后重新按当前 bucket 和历史全量计算。

parole 不清空 history。`DeliveryTracker` 是否写历史只看 `isExemptFromAppStandby()`，不看 `mAppStandbyParole`，所以充电期间的非豁免交付仍然累积。测试中的结果也是：拔电后，这些充电期间的时间点会立即参与配额。

bucket 升降同样不换账本：

```text
升级 → 用更大的 q 重解释旧历史，可能恢复 expected
降级 → 用更小的 q 重解释旧历史，可能立即推迟
```

应用不能通过来回切 bucket 洗掉交付历史。

## 11. 不同 pending 容器的收敛时刻不同

`reorderAlarmsBasedOnStandbyBuckets()` 只扫描 `mAlarmBatches`，不会扫描三个 pending 容器。理解“为什么状态已变而时间没立刻恢复”时，要先确认 Alarm 在哪里。

| 容器 | 进入前是否已评估 App Standby | 离开时是否重评 |
|---|---|---|
| `mPendingWhileIdleAlarms` | 否；idle 分流发生得更早 | idle 结束后 `reAddAlarmLocked()` 会评估 |
| `mPendingBackgroundAlarms` | 是；它从到期的主 Batch 移入 | 限制解除时直接走 pending delivery，不先重评 |
| `mPendingNonWakeupAlarms` | 是；已通过到期与后台门 | 后续合并交付，不放回 Batch 先重评 |

这里还有一个值得诊断的非对称：pending-background 解除后若直接成功交付，会写 `AppWakeupHistory`，但 `deliverPendingBackgroundAlarmsLocked()` 本身不调用 standby reorder。主 Batch 中同账户的其他 Alarm 可能要等下一次 set、正常 AlarmThread 交付、bucket 或 charging 事件才根据这笔新历史调整。

pending non-wakeup 若在正常 AlarmThread 轮次被合并进 trigger list，则会进入该轮账户收集和随后重排；它等待期间本身不会被 bucket 消息直接扫描。

## 12. 配置热更新有哪些实现边界

`Settings.Global.ALARM_MANAGER_CONSTANTS` 可修改窗口和 quota，但解析完成后不主动调用 standby reorder。已有 Alarm 不会仅因常量字段变化就全量收敛；要等待下一次 set、正常交付、bucket 变化或 charging 变化。

普通窗口有上限，没有对称的非负下限：

```text
APP_STANDBY_WINDOW > 默认 1 小时 → 钳回 1 小时
APP_STANDBY_WINDOW < 默认值      → 接受并记警告
```

普通 quota 也直接接受显式配置值。WORKING 到 NEVER 的“默认回退值”会取前一档当前值与本档默认值的较小者，但显式写入更大值时，没有最终的单调递减校验；负 quota 会落入 `quota <= 0`，效果接近不断向后推一天。

Restricted 的校验更强一些：

```java
restrictedQuota = Math.max(1, parsedQuota);
restrictedWindow = Math.max(APP_STANDBY_WINDOW, parsedWindow);
```

还有一处结构失配：`mAppWakeupHistory` 在服务启动时用固定的默认一小时构造，之后没有随着 `APP_STANDBY_WINDOW` 热更新。getter 也不按当前 `now` 裁剪；只有记录新时间点时才按这份固定窗口裁剪。默认配置的主路径可由现有测试支持，但自定义短窗口、非默认 restricted quota 与 parole 交错时，不能只看 Constants 字段就推断 history 中一定保存了政策所需的完整窗口。

## 13. 用四个场景检验模型

场景一：`RARE` 第一次与第二次 Alarm。

```text
history 为空，q=1
第一枚按 expected 交付，在 T 记账
第二枚 expected < T+1h → actual 推到 T+1h
```

这是滚动窗口，不在整点清零。

场景二：`FREQUENT` 同毫秒两枚 Alarm。

```text
q=2
同账户两枚都在 T 成功建立 InFlight
history 只新增一个 T
下一枚看到的 count 仍是 1
```

若两次相差 1 ms，则会保存两个点并用满默认额度。

场景三：`RESTRICTED` 的原请求晚于恢复点。

```text
上次交付 T，默认恢复点 T+24h
新 Alarm expected=T+30h
```

结果仍是 T+30h，配额只推迟，不提前。

场景四：`WORKING_SET` 已满时插电再拔电。

```text
插电 → parole=true，全量重排，被推迟项恢复 expected
充电期间交付 → history 继续增加
拔电 → parole=false，全量重排，用包含充电期间记录的历史重新计算
```

parole 是暂时跳过门，不是免费清账。

## 14. 静态阅读和诊断方法

先定位历史、公式、重排和事件入口：

```bash
rg -n "AppWakeupHistory|recordAlarmForPackage|getNthLastWakeup" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "adjustDeliveryTimeBasedOnBucketLocked|isExemptFromAppStandby" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "reorderAlarmsBasedOnStandbyBuckets|APP_STANDBY_BUCKET_CHANGED|CHARGING_STATUS_CHANGED" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "test.*Quota|testRestricted|testCharging" \
  frameworks/base/services/tests/mockingservicestests/src/com/android/server/AlarmManagerServiceTest.java
```

诊断一枚被推迟的 Alarm 时按以下顺序记证据：

```text
1. sourcePackage 与 creator user 是谁？
2. 是否属于 AlarmClock/core/unrestricted 豁免？
3. 当前 bucket、q、普通/restricted window 是多少？
4. history 至少 q 个点时，第 q 个倒数是什么？
5. expectedWhen/Max 与实际 when/max 分别是多少？
6. Alarm 当前在 Batch、pending-idle、pending-background 还是 pending-non-wakeup？
7. 最近是否发生 bucket、charging 或正常交付重排？
8. 是否还叠加普通 AWI 的 creator-UID next allowed？
```

现有单元测试覆盖普通四档的 set 时推迟、到期后重排与不推迟，另覆盖 Restricted、bucket 升降和 charging。测试没有自动替我们证明同毫秒边界、消息全局合并、全部 pending 容器和自定义配置失配；这些结论要分别回到实现核对。

## 15. 结论与下一章

完整决策链可以压缩为：

```text
非豁免 Alarm 准备进入 Batch
  → 查 source package + creator user 的 bucket/history
  ├─ 额度足：actual = expected
  └─ 额度满：actual when=max=nextAllowed

成功建立 InFlight
  → 同账户同 elapsed 毫秒最多新增一个 history 点
  → 正常 AlarmThread 路径重排同账户剩余 Batch

bucket 改变 → Handler 定向重排
charging 改变 → parole 切换并全量重排
其他 pending 容器 → 按各自恢复路径收敛，时机并不统一
```

最终应记住六点：

1. App Standby 配额限制成功交付时间点，不限制登记数量；
2. 默认普通窗口是一小时，额度为 720/10/2/1/0，Restricted 默认一次/24 小时；
3. 普通 AWI、exact、wakeup 都不自动豁免，账户键是 source package + creator user；
4. expected 保存请求，actual 保存政策结果，actual 零宽不等于 API exact；
5. 新 set、正常交付、bucket 和 charging 会触发不同范围的重排，但三个 pending 容器并不都被扫描；
6. 配置字段更新、history 固定窗口和 Handler 消息合并构成 r48 的重要收敛边界。

下一章转向时间本身：RTC Alarm 遇到用户改时钟、时区或 kernel `CANCEL_ON_SET` 时，怎样重新投影到 elapsed 时间轴；`TIME_TICK` 与 `DATE_CHANGED` 又怎样自我续排。
