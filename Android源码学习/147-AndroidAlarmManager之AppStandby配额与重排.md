# 147 Android AlarmManager：App Standby 配额与重排

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS只读源码，不要求编译、不要求连接设备  
> 前置章节：第122、123、145、146章

---

## 1. Alarm配额限制的是什么

Android 11 AlarmManager的App Standby配额不是“一个应用最多能登记多少Alarm”，也不是JobScheduler的执行时长预算。

它限制的是一个source package/user在滚动窗口内可以成功接收多少次非豁免Alarm交付。超过后，Alarm不会立即报错或删除，而是把实际
`whenElapsed/maxWhenElapsed`推到下一次可能恢复额度的时刻。

---

## 2. 本章目标

本章追：

```text
成功交付 → AppWakeupHistory
新set/旧Alarm到期 → 读取standby bucket与历史
额度用完 → expected时间保留、actual时间推后
bucket/充电/新交付变化 → 定向或全量重排
```

并重点区分Alarm Standby配额、AWI频率门、Battery Saver后台限制和每UID登记上限四套机制。

---

## 3. 源码地图

```text
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/services/tests/mockingservicestests/src/com/android/server/AlarmManagerServiceTest.java
frameworks/base/core/java/android/app/usage/UsageStatsManager.java
frameworks/base/services/core/java/com/android/server/usage/AppStandbyInternal.java
```

---

## 4. 四套限制先分开

| 机制 | 账户键 | 限制对象 | 超限结果 |
|---|---|---|---|
| 每UID Alarm上限 | calling uid | 当前登记数量 | set抛异常 |
| App Standby Alarm配额 | source package+user | 成功Alarm交付次数 | 推迟actual时间 |
| AllowWhileIdle频率 | creator uid | 成功AWI交付间隔 | 推迟到minTime |
| Background restriction | creator/source + AppStateTracker | 当前是否允许后台Alarm | 移入pending background |

同一Alarm可同时经过多道门。

---

## 5. 默认一小时配额

```text
ACTIVE      720 / 1小时
WORKING_SET 10  / 1小时
FREQUENT    2   / 1小时
RARE        1   / 1小时
NEVER       0   / 1小时
RESTRICTED  1   / 24小时（独立窗口）
```

这些是r48默认，不应套用其他Android版本的数字。

---

## 6. Bucket映射边界

`getQuotaForBucketLocked()`用数值区间映射：

```text
<= ACTIVE       → ACTIVE
<= WORKING_SET  → WORKING
<= FREQUENT     → FREQUENT
< NEVER         → RARE
else            → NEVER
```

RESTRICTED数值位于RARE和NEVER之间，却在adjust方法前置special-case，不走这张普通数组。

---

## 7. 账户为什么是source package + creator user

Alarm对象的 `sourcePackage` 对PendingIntent取creator package，user取creator UID的userId。

```java
Pair.create(alarm.sourcePackage,
    UserHandle.getUserId(alarm.creatorUid))
```

代理调用不能把目标PendingIntent产生的Alarm次数只记到代理calling package；真正可被启动/唤醒的来源承担standby政策。

---

## 8. AppWakeupHistory的数据结构

```text
ArrayMap<Pair<package,user>, LongArrayQueue>
```

每个账户保存elapsed realtime时间戳队列。它在system_server内存中，不是跨重启持久账本。

---

## 9. 同一毫秒会去重

```java
if (history.size() == 0 || history.peekLast() < nowElapsed) {
    history.addLast(nowElapsed);
}
```

同一package/user在同一个elapsed毫秒成功交付多个Alarm，只添加一个历史时间点。

因此配额更接近“交付批次/时间点次数”，不是严格逐Alarm对象计数。

---

## 10. 这个去重为何合理

AlarmManager本来就鼓励Batch coalescing。一个应用同一轮收到多个Alarm，如果逐对象扣额度，合并反而会惩罚应用；同毫秒只记一次更贴近“系统被该应用唤醒/交付一轮”的成本。

但即使Alarm都是non-wakeup，只要成功交付仍会写这份history；类名WakeupHistory不代表只统计WAKEUP类型。

---

## 11. History何时写入

DeliveryTracker成功完成 `operation.send()` / `listener.doAlarm()`并建立in-flight后，若不豁免才调用：

```java
mAppWakeupHistory.recordAlarmForPackage(
    sourcePackage, sourceUserId, nowElapsed);
```

PendingIntent canceled或Listener发送失败不入账。

---

## 12. 记账发生在完成之前

写history发生在成功发起交付、建立in-flight之后，不等待接收器finished。

配额衡量“系统已经给出一次交付机会”，不是“应用业务成功完成”。即使之后广播超时，这次机会仍已计入。

---

## 13. 哪些Alarm豁免

```java
return alarmClock != null
    || UserHandle.isCore(creatorUid)
    || (flags & ALLOW_WHILE_IDLE_UNRESTRICTED) != 0;
```

普通AWI不在豁免公式中，因此仍受standby配额；这正是第146章复读纠正的重点。

---

## 14. Exact与Wakeup都不自动豁免

普通App的 `setExact()`、`RTC_WAKEUP`、`ELAPSED_REALTIME_WAKEUP`依然经过配额。

Exact只影响窗口/Batch，wakeup只影响设备suspend；两者都不是App Standby特权。

---

## 15. AlarmClock为何豁免

面向用户的闹钟需要按可见时间触发，还会改变DIC进入Doze的计划。若它和普通后台Alarm共用RARE/NEVER配额，会破坏用户可见功能。

代价是AlarmClock权限语义必须只用于真正闹钟，不能当后台配额逃生口。

---

## 16. 普通窗口的滚动公式

假设quota为q，当前history至少q项。取第q个倒数时间 `t(q)`：

```text
nextAllowed = t(q) + APP_STANDBY_WINDOW
```

若Alarm原始expectedWhen早于nextAllowed，就把actual when/max都设为nextAllowed。

---

## 17. 为什么取第q个倒数

要让窗口内最多保留q次，下一次必须等当前最老的那一个额度事件滚出窗口。

例如FREQUENT q=2，历史为10、20分钟，窗口1小时；新Alarm最早到70分钟，此时10分钟事件到边界外，留下20和新交付两次。

---

## 18. 边界使用什么比较

History清理：

```java
while (first + window < last) removeFirst();
```

严格 `<`意味着刚好相差一个window时旧事件仍保留。调整出的 `nextAllowed=t+window` 在恰好边界可被安排，但记录新时间后snapToWindow会因
old+window < new为false继续保留边界两端，形成闭区间式计数边界。

这是需要结合测试验证的精确细节，不要只用“过去一小时”口语化带过。

---

## 19. quota=0怎样处理

NEVER默认0，无法取第0倒数。因此源码简单设置：

```java
minElapsed = nowElapsed + 1 day;
```

每次重新评估仍不允许，就再向后推一天，直到bucket/parole改变。它不是一个真正固定的“明天必发”承诺。

---

## 20. RESTRICTED为何单独处理

RESTRICTED默认1次/24小时，窗口不同于普通1小时，且配置保证quota至少1。

若有历史，取restricted quota对应倒数时间，再判断 `now-last < restrictedWindow`，必要时推到last+window。

---

## 21. Restricted判断还比较expected

只在 `alarm.expectedWhenElapsed < minElapsed` 时推迟。若应用本来就把Alarm设在更晚处，不会为了配额把它拉早或改成minElapsed。

政策永远不能把应用原计划提前。

---

## 22. expected与actual是本章核心

```text
expectedWhen/Max：App API、RTC转换与原始window形成的需求
when/max：standby政策后真正参与Batch的时间
```

额度不足时actual变为单点minElapsed；额度恢复时可把actual还原为expected。

---

## 23. 为什么推迟后窗口变成单点

源码设置：

```java
alarm.whenElapsed = alarm.maxWhenElapsed = minElapsed;
```

不保留原窗口宽度，确保下一次就在额度恢复点重新评估/触发。它可能使原inexact Alarm在政策推迟后表现为一个standalone以外的零宽窗口；
零宽actual不自动添加Standalone flag。

---

## 24. 零宽actual不等于API exact

Standalone flag是在Binder入口依据原 `windowLength==0`添加。standby把actual when/max压成同一点，并不会修改原windowLength或flags。

因此“dump看到actual区间为0”不能反推应用调用了setExact。

---

## 25. 没有defer时主动恢复

若当前额度充足：

```java
alarm.whenElapsed = alarm.expectedWhenElapsed;
alarm.maxWhenElapsed = alarm.expectedMaxWhenElapsed;
```

这使bucket升级、充电parole或历史滚出后，之前被推迟的Alarm能回到原始要求，而不是永远黏在旧minElapsed。

---

## 26. 新set时第一次评估

Alarm进入主Batch前调用 `adjustDeliveryTimeBasedOnBucketLocked()`。如果历史已满，新Alarm从一开始就以推迟后的actual加入Batch。

这避免先arm一个过早kernel deadline，到点后才发现额度不足。

---

## 27. 到期后为何还要重排

一轮triggerList成功交付后，history新增时间。AMS收集所有非豁免trigger package/user，然后调用：

```java
reorderAlarmsBasedOnStandbyBuckets(triggerPackages)
```

同账户后续Alarm可能因刚消耗额度而必须立刻后移。

---

## 28. 同一批多Alarm只记一次的连锁

同账户同毫秒多个Alarm成功发送，history只增一个时间点；triggerPackages也是ArraySet，只针对该账户重排一次。

这形成一致的“一个交付时间点，一次额度变化，一轮定向重排”模型。

---

## 29. reorder怎样保持Batch正确

它倒序遍历所有Batch/Alarm，只处理target账户；若adjust改变时间：

```text
从旧Batch remove（reOrdering=true，不减每UID计数）
收集到rescheduledAlarms
最后重新insertAndBatch
```

直接在Batch内改when会破坏窗口交集和start排序，因此必须移出再插入。

---

## 30. targetPackages=null表示全量

充电/parole变化影响所有非豁免账户，所以传null全表重算；bucket变化只构造一个package/user集合，定向重算。

重排只扫描 `mAlarmBatches`，不扫描pending-while-idle、pending-background或pending-non-wakeup。不同容器有各自恢复/交付时再评估的边界。

---

## 31. Bucket变化怎样到达AMS

SYSTEM_SERVICES_READY时AMS注册 `AppStandbyInternal.AppIdleStateChangeListener`。

监听回调不直接持AMS锁扫描，而是去重同类Handler消息并post package+user；Handler再持锁定向reorder并重设kernel Alarm/next alarm clock。

---

## 32. removeMessages会合并不同包变化

```java
mHandler.removeMessages(APP_STANDBY_BUCKET_CHANGED);
send(package,user)
```

它移除所有尚未处理的同what消息，而不是只移除同一package。若短时间A、B连续变化，B可能取消A的定向重排。

后续set/到期会重新按当前bucket评估，但A已有远期Alarm可能暂时保持旧actual时间。这是r48值得审计的事件合并边界。

---

## 33. 充电被当作parole

ChargingReceiver监听 `ACTION_CHARGING/DISCHARGING`，Handler直接：

```java
mAppStandbyParole = charging;
reorderAlarmsBasedOnStandbyBuckets(null);
```

充电时跳过standby配额，断电后重新应用。字段名parole在这里的实际输入就是charging广播，不是独立UsageStats parole callback。

---

## 34. Parole不会清历史

充电时adjust把被推迟Alarm恢复expected，但 `mAppWakeupHistory`仍保留旧时间，并继续记录充电期间成功交付的非豁免Alarm。

断电后这些记录立即参与配额，后续Alarm可能再次被推迟。Parole是暂时不执行门，不是清零账户。

---

## 35. 充电期间为何仍记history

DeliveryTracker的记账条件只看 `isExemptFromAppStandby()`，不看 `mAppStandbyParole`。

所以parole允许交付，但成本历史继续积累。测试也验证断电后按充电期间交付时间重新推迟。

---

## 36. Bucket升级

FREQUENT已满而被推迟的Alarm，升级到ACTIVE后定向reorder。新quota足够则deferred=false，actual恢复expected；若expected已在过去，kernel会安排近期处理。

升级不是新建Alarm，也不改变origWhen/windowLength。

---

## 37. Bucket降级

WORKING中尚未到期的Alarm，在降到RARE后可能立即被推到“第1倒数交付+1小时”。

已经成功交付的历史不按旧bucket分类；同一队列在新bucket quota下重新解释。

---

## 38. Quota切换不能洗历史

账户历史与bucket分开保存，切bucket只改变q。应用不能通过ACTIVE→RARE→ACTIVE过程制造一张新账本。

包卸载/强停相关清理和user移除才会删除对应history；普通bucket变化不会。

---

## 39. History删除边界

```text
ACTION_USER_STOPPED → removeForUser
PACKAGE_REMOVED/RESTARTED → removeForPackage（随后还remove alarms）
package update REPLACING=true → 不清Alarm/历史
```

避免应用更新时意外丢失调度；真正卸载则清账户，防新安装继承旧历史。

---

## 40. History只在新记录时裁剪

`snapToWindow()`仅由 `recordAlarmForPackage()`调用；getter不会以当前now主动删除陈旧时间。

普通公式用第q倒数+t窗口，旧事件即使留在queue也可能产生已过去的minElapsed，随后expected比较/当前触发自然放行；Restricted分支还显式用
`now-last < window`避免陈旧历史误限。

---

## 41. 配置改变后的history window陷阱

AppWakeupHistory构造时固定使用默认1小时，Constants更新 `APP_STANDBY_WINDOW` 时没有同步修改history的 `mWindowSize`。

因此测试把窗口调短时，策略计算用新window，但history裁剪仍以默认1小时保留更多时间戳。getter返回队列总数，可能影响“是否已满”的判断；
这是r48配置热更新不完全同步的边界。

---

## 42. Window只能不大于默认一小时

解析后：大于1小时钳回1小时；小于1小时允许但日志提示只建议测试。没有看到非负下限钳位。

负window会让普通minElapsed落在历史之前，并使history/策略窗口失配更严重，属于错误配置风险。

---

## 43. 普通quota的默认递减约束

ACTIVE直接读配置；WORKING到NEVER每项的“默认回退”取：

```java
min(前一项当前值, 本bucket默认值)
```

但若配置显式提供一个更大的值，parser会直接接受，并没有最终强制 `working >= frequent >= rare >= never`。

---

## 44. 负quota没有统一钳位

普通bucket quota用 `getInt()`直接赋值。负数会满足 `wakeupsInWindow >= quota`，随后走 `quota<=0`，等同不断推迟一天。

Restricted quota则 `Math.max(1, parsed)`，至少为1。两类配置校验不对称。

---

## 45. Restricted window的钳位

```java
APP_STANDBY_RESTRICTED_WINDOW =
    max(APP_STANDBY_WINDOW, parsedRestrictedWindow);
```

它保证restricted窗口不短于普通窗口；但如果普通window本身负数，restricted仍可能由parsed/default决定。

---

## 46. 常量更新不主动reorder

AlarmManager Constants Observer解析配额后只更新字段与AWI options，没有调用
`reorderAlarmsBasedOnStandbyBuckets()`。

所以改quota/window不会立即修正所有已有Alarm；要等下一次set、交付、bucket变化或charging变化。与第143/146章热更新原则一致。

---

## 47. 非wakeup也消耗历史

`recordAlarmForPackage()`不检查alarm.wakeup。只要非豁免Alarm成功建立in-flight，无论RTC/ELAPSED、wakeup/non-wakeup都会入账。

因此“App Alarm history”比“硬件唤醒次数”宽；命名不能替代消费者公式。

---

## 48. Listener也会消耗历史

成功的direct listener Alarm同样进入in-flight并记账，尽管没有PendingIntent。账户的sourcePackage就是calling package，creatorUid就是calling uid。

配额针对Alarm交付机会，不限广播。

---

## 49. Pending while idle何时评估standby

普通Alarm在Doze中早于standby adjust就进入pending-while-idle。退出idle恢复时 `reAddAlarmLocked()`重建expected，再走
`setImplLocked()`，此时才应用最新standby配额。

所以挂起期间bucket/history变化不会直接扫描该容器，但恢复时会收敛。

---

## 50. Pending background何时评估

一个Alarm先通过standby adjust进入主Batch，真正到期时可能因AppStateTracker background restriction移入pending-background。

限制解除时专用发送函数直接deliver pending项，未再次调用standby adjust。它们原先的actual已经按当时配额决定；等待期间history/bucket变化可能不会在释放前重评，
是另一处跨政策容器边界。

---

## 51. Pending non-wakeup的边界

它们已从Batch取出并通过standby/后台检查，只因屏灭合并交付而等待。期间配额变化不会把它们送回Batch；真正deliver时记账，随后triggerPackages重排剩余主Batch。

不要把每个pending容器都当成“尚未通过所有政策”。

---

## 52. 与AWI双门手算

一个FREQUENT普通AWI：

1. set时先按standby q=2/小时调整actual；
2. 到期时再检查creator UID距上次AWI是否至少long/short；
3. 成功后同时更新AWI last-dispatch与standby history；
4. 然后重排同package/user后续Alarm。

两张账本键不同，恢复时刻取决于更晚的那道门。

---

## 53. 与JobScheduler quota的区别

JobScheduler第122章按package运行墙钟段、Job/Session次数和rate limit判断Job约束；Alarm这里只保存成功交付时间点。

两者standby bucket名字相同，但账本、窗口、免费条件、恢复动作完全不同，不能相互套公式。

---

## 54. 测试覆盖什么

`AlarmManagerServiceTest`覆盖ACTIVE/WORKING/FREQUENT/RARE的set时推迟、到期后推迟与不推迟，Restricted 1/day，bucket升级/降级和charging parole。

这些测试验证主要策略；同毫秒去重、Constants window与History固定窗口失配、bucket消息全局remove等边界需从实现审计，不能说测试已覆盖。

---

## 55. 场景一：RARE第一次Alarm

history为空，q=1，第一枚按expected交付并记T。紧接第二枚set时count>=1，取第1倒数T，推到T+1小时。

这是滚动窗口，不是每个整点重置。

---

## 56. 场景二：FREQUENT同毫秒两枚

q=2，两枚在同一elapsed毫秒成功交付，history只记一个T。之后第三枚仍看到count=1，尚未用满2个“时间点”额度。

若两枚相差1ms，则记两个时间点，第三枚可能推迟。

---

## 57. 场景三：NEVER

q=0，新Alarmactual被推到now+1天。一天后bucket仍NEVER，重新评估仍会再推一天。

因此NEVER的语义接近持续不可交付，而不是固定24小时冷却一次。

---

## 58. 场景四：Restricted

第一枚成功T；第二枚原计划T+1小时，默认被推到T+24小时。若原计划本来是T+30小时，则保持T+30小时，不会拉早到24小时。

---

## 59. 场景五：插上电源

working配额已满，Alarm从expected T推到T+1小时。收到CHARGING后全量reorder，parole=true，actual恢复expected；若expected已过，近期触发。

充电期间交付继续记history。

---

## 60. 场景六：拔掉电源

parole=false，全量重排。充电期间新交付的时间点仍在history，未到期Alarm可能立刻被再次推后。

这不是“充电白送并清零”，而是“充电期间暂不执行门”。

---

## 61. 场景七：连续A/B bucket变化

A变化post消息；B变化先remove所有同what再post B。若A无其他事件触发，其已有Alarm可能暂用旧时间。

这不会修改A真实UsageStats bucket，只是AMS缺少一次定向重排通知；以后再评估时会读取当前事实。

---

## 62. macOS只读练习一：手算q-th last

FREQUENT q=2，window=60分钟，历史 `[10, 40]`，计算在45分钟set的新Alarmactual；再计算70、71分钟分别会怎样。

明确边界比较是 `<` 还是 `<=`。

---

## 63. macOS只读练习二：追记账点

```bash
rg -n "recordAlarmForPackage|isExemptFromAppStandby" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

画出PendingIntent canceled、Listener异常、成功in-flight三条是否入账的分支。

---

## 64. macOS只读练习三：审计所有重排触发

```bash
rg -n "reorderAlarmsBasedOnStandbyBuckets" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

区分成功交付后的定向集合、bucket单账户消息、charging全量消息，记录每条是否重设kernel Alarm。

---

## 65. macOS只读练习四：验证配置不对称

```bash
sed -n '500,555p' \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

手算普通quota=-1、restricted quota=-1、window=2小时、window=-1分别得到什么字段值。

---

## 66. macOS只读练习五：对照测试

```bash
sed -n '490,770p' \
  frameworks/base/services/tests/mockingservicestests/src/com/android/server/AlarmManagerServiceTest.java
```

为每个测试写出它证明的公式，以及它没有证明的容器/配置/同毫秒边界。

---

## 67. macOS只读练习六：画容器政策状态

为同一Alarm依次经过：

```text
pending-while-idle → main Batch → pending-background
→ pending-non-wakeup → in-flight
```

标出哪次进入会重新评估standby，哪次只保留旧actual，哪次才写history。

---

## 68. 常见误解纠正

- 误解：只统计wakeup Alarm。纠正：所有非豁免成功交付都记账。
- 误解：一批N个Alarm扣N次。纠正：同package/user同毫秒去重。
- 误解：Exact自动豁免。纠正：只有AlarmClock/core/unrestricted。
- 误解：AWI豁免standby配额。纠正：普通AWI仍走配额，再叠加AWI频率门。
- 误解：quota满会拒绝set。纠正：通常推迟actual时间。
- 误解：充电会清空历史。纠正：只临时parole，仍记账。
- 误解：NEVER一天后必发。纠正：仍NEVER会再推一天。
- 误解：bucket切换重置账本。纠正：同一history按新q解释。
- 误解：配置更新立即重排。纠正：Observer没有触发reorder。

---

## 69. 面试式自测

1. Alarm standby配额的账户键和记账时点是什么？
2. 为什么同毫秒多Alarm只扣一次？
3. q-th last公式怎样保证滚动窗口上限？
4. quota=0为何不取history？
5. Restricted和RARE有什么不同？
6. expected与actual时间分别表示什么？
7. actual零宽为何不等于API exact？
8. 成功交付后为何必须重排同账户剩余Alarm？
9. charging parole为何仍保留/增加history？
10. 哪些Alarm豁免，普通AWI是否豁免？
11. bucket消息合并有什么r48边界？
12. Constants window与History window为何可能失配？

---

## 70. 本章结论

1. Alarm App Standby限制成功交付时间点，不限制登记数量；
2. 账户是source package+creator user；
3. 默认普通窗口1小时，配额720/10/2/1/0；
4. Restricted独立为默认1次/24小时；
5. 同账户同elapsed毫秒多个交付只记一次；
6. 发送失败不入账，成功in-flight即入账而不等finished；
7. AlarmClock、core creator、unrestricted豁免，普通AWI不豁免；
8. exact、wakeup本身不构成豁免；
9. q-th last + window决定下一额度恢复点；
10. quota 0每次评估向后推一天；
11. expected保留原需求，actual参与政策后Batch；
12. defer把actual压成单点，但不自动变API Standalone exact；
13. 成功交付、bucket变化、charging变化会触发不同范围reorder；
14. 充电parole恢复expected但不清history，期间仍记账；
15. bucket变化不洗历史，升级/降级用新q重解释；
16. r48存在bucket消息全局合并、history固定默认window和常量更新不主动reorder等边界。

一句话记忆：

> Alarm Standby配额不是把Alarm拒之门外，而是用source账户的滚动交付历史不断改写“实际何时有资格进入下一轮Batch”，并在事实变化时把已有Alarm重新排队。

---

## 71. 生成后复读修订

初稿后重新核对adjust/reorder、DeliveryTracker、AppWakeupHistory、Constants和测试，重点补强：

1. 明确配额记的是所有非豁免成功交付而非仅wakeup；
2. 发现同package/user同毫秒只记一次；
3. 区分成功发起与finished，失败不入账；
4. 逐项确认普通AWI不豁免、unrestricted才豁免；
5. 用q-th last解释滚动恢复时刻；
6. 补出严格小于的窗口边界；
7. 区分RESTRICTED独立1/day与RARE 1/hour；
8. 分开expected/actual和原window/Standalone flag；
9. 说明充电parole期间仍记history；
10. 限定reorder只扫描主Batch；
11. 发现bucket listener removeMessages会合并不同包消息；
12. 发现AppWakeupHistory window构造后不随Constants更新；
13. 审计普通quota负值/递减关系与restricted钳位不对称；
14. 补出常量更新不主动重排；
15. 用测试限定已证实范围，不夸大容器/配置边界覆盖；
16. 全部练习限定为macOS只读推演，不执行Alarm或bucket修改命令。

---

## 72. 下一章

第148章继续AlarmManager时间体系：深入RTC/ELAPSED转换、setTime/setTimeZone权限、timerfd cancel-on-set、TIME_CHANGED过滤、全量rebatch、
TIME_TICK与DATE_CHANGED自调度，以及墙钟跳变、DST和设备重启下的语义边界。
