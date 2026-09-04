# 149 Android AlarmManager：取消、身份匹配与生命周期清理

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 145～148 章

---

## 1. 本章只回答一个问题

应用设置 Alarm 时使用了一个 `PendingIntent`，稍后重新构造了 Java 对象再调用 `cancel()`。它为什么能取消原来的 Alarm？反过来，为什么只改 extras 后设置“第二枚”Alarm，第一枚常常会消失？

表面 API 背后有两层身份：

```text
Intent/requestCode/type/user/flags 等形成 PendingIntentRecord.Key
  → Key 决定 system_server 是否复用 IIntentSender Binder token
  → AlarmManagerService 直接按 token 替换或取消等待 Alarm
```

生命周期清理则换用另外三套身份：

```text
Alarm.uid          → 谁调用 set、谁占登记数量
Alarm.sourcePackage → PendingIntent 代表哪个源包
Alarm.creatorUid    → PendingIntent 代表哪个用户/UID
```

一句话结论：

> 精确取消看 Binder token；UID、包和用户批量清理分别看登记者、source package 与 creator user。只有先分清身份和容器，才能判断一枚 Alarm 会不会被真正清掉。

## 2. 两种目标分别用什么 token

Alarm 必须二选一：

| 目标 | App 侧对象 | 服务端直接匹配身份 | repeating |
|---|---|---|---|
| `PendingIntent` | `PendingIntent` | `IIntentSender` Binder token | 可以 |
| direct listener | `OnAlarmListener` | `IAlarmListener` Binder token | 不可以 |

服务端拒绝“两者都空”或“两者都有”，但为兼容旧行为只记录日志并返回。

`Alarm` 同时保存四个容易混淆的字段：

| 字段 | PendingIntent Alarm | listener Alarm |
|---|---|---|
| `uid` | 调用 `set()` 的 UID | 调用 UID |
| `packageName` | 经 AppOps 验证的调用包 | 调用包 |
| `creatorUid` | PendingIntent 创建者 UID | 调用 UID |
| `sourcePackage` | PendingIntent 创建者包 | 调用包 |

假设 A 进程拿 B 创建的 PendingIntent 调用 `set()`：

```text
uid/packageName       = A
creatorUid/sourcePackage = B
```

于是每 UID 登记上限归 A，App Standby 与许多交付政策归 B；后面的批量清理也可能分别选中不同集合。

核心源码：

```text
frameworks/base/core/java/android/app/AlarmManager.java
frameworks/base/core/java/android/app/PendingIntent.java
frameworks/base/core/java/android/content/Intent.java
frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
frameworks/base/services/core/java/com/android/server/am/PendingIntentController.java
frameworks/base/services/core/java/com/android/server/am/PendingIntentRecord.java
```

## 3. 新 Java 对象为何仍能代表同一 PendingIntent

`PendingIntent` 不是一份普通 Intent 数据，它主要持有：

```java
private final IIntentSender mTarget;
```

system_server 用 `PendingIntentRecord.Key` 查找已有 record。Key 的主要字段包括：

```text
sender type（activity/broadcast/service/foreground-service/result）
packageName / featureId / userId
activity token / resultWho / requestCode
最后一个 Intent 的 filterEquals 身份
resolvedType
保留下来的 PendingIntent identity flags
```

`Intent.filterEquals()` 比较：

```text
action、data、MIME type、identifier、package、component、categories
```

extras 不参加比较。Key 相等时，`PendingIntentController` 通常复用同一个 `PendingIntentRecord`；两个 Java `PendingIntent` 对象最终可持有同一个 token。

删除阶段不再重新比较两份 Intent：

```java
return mTarget.asBinder().equals(other.mTarget.asBinder());
```

所以“重新构造对象还能取消”的真实条件不是 Java 引用相同，而是重新获取时形成相同 Key，并拿回同一个 Binder token。

## 4. extras、requestCode 和获取 flags 怎样改变身份

只改 extras 而保持其他 Key 字段相同，通常仍得到同一 token。这也是最常见的“第二枚 Alarm 替换第一枚”原因。

若确实需要多枚共存，应改变参与身份的字段，例如：

- `requestCode`；
- data URI；
- action；
- sender type；
- 必要时 component/category 等 `filterEquals` 字段。

获取 `PendingIntent` 时，三种动作 flag 会在构造 Key 前清除：

```text
FLAG_NO_CREATE
FLAG_CANCEL_CURRENT
FLAG_UPDATE_CURRENT
```

而 `FLAG_ONE_SHOT`、`FLAG_IMMUTABLE` 等描述 token 自身语义的 flag 会留在 Key 中。使用 `NO_CREATE` 重新查询 one-shot/immutable token 时，也要带上相应 identity flag。

`UPDATE_CURRENT` 找到旧 record 后会替换它保存的 extras，并返回原 record：

```java
rec.key.requestIntent.replaceExtras(newIntent);
return rec;
```

token 没变，所以旧 Alarm 仍指向更新后的 capability；其他已经持有该 token 的组件，之后发送时也可能看到新 extras。即使 token 是 immutable，创建者仍可以通过 `UPDATE_CURRENT` 更新其基础 extras；immutable 主要约束发送者 fill-in。

`CANCEL_CURRENT` 则先把旧 record 标为 canceled、通知 AlarmManager 清旧 token，再从 token 表移除并创建新 record。Key 可以相同，代际 token 已经不同。

## 5. 为什么再次 `set()` 本身就是 replacement

服务端通过基本校验并构造新 `Alarm` 后，顺序是：

```java
removeLocked(operation, directReceiver);
incrementAlarmCount(newAlarm.uid);
setImplLocked(newAlarm, false, doValidate);
```

因此同 token 再次 set 的语义是：

```text
先删除所有等待中的旧 Alarm
  → 再按新 type、时间、窗口和政策插入一枚新 Alarm
```

是否 replacement 不看旧新 `type` 或 `triggerAtTime`，只看直接目标 token。公开 `AlarmManager.cancel(pi)` 调用同一删除入口，但不会作废 PendingIntent 本身；之后仍可以用这个 token 再 set 或直接 send。

`cancel(PendingIntent)` 的 null 行为有 target SDK 边界：N 及以上抛 NPE，旧 target 只记录错误并返回。`cancel(OnAlarmListener)` 传 null 则直接抛 NPE。

## 6. 精确 token 删除为什么要扫描四个容器

`removeLocked(PendingIntent, IAlarmListener)` 扫描：

| 容器 | 为什么 Alarm 会在那里 |
|---|---|
| `mAlarmBatches` | 正常等待 kernel deadline |
| `mPendingWhileIdleAlarms` | deep Doze 挂起 |
| `mPendingBackgroundAlarms` | 到期后受后台限制 |
| `mPendingNonWakeupAlarms` | non-wakeup 已到期但因非交互状态延迟 |

匹配函数按 Alarm 自身类型分支：

```java
return operation != null
        ? operation.equals(pi)
        : receiver != null
                && listener.asBinder().equals(receiver.asBinder());
```

主 Batch 删除会改变当前调度结构，因此 `Batch.remove()` 重算 `start/end/flags`，非重排删除还按 `alarm.uid` 递减 `mAlarmsPerUid`。如果删除了 AlarmClock，会标记 next-alarm-clock 需要更新；删除 TIME_TICK 会更新诊断时间。

暂存容器中的删除不设置 `didRemove`，因为这些项已不参与当前 Batch/kernel deadline；但仍必须逐项递减登记计数。

只要主 Batch 有匹配项，外层还会：

```text
检查并清 mPendingIdleUntil / mNextWakeFromIdle
  → rebatch 主调度结构
  → 如取消的是 idle-until，恢复 pending-while-idle
  → 更新 next alarm clock
```

这就是取消一枚特殊系统 Alarm 时，为什么不能只从 ArrayList 删一个对象。

## 7. `mAlarmsPerUid` 与 InFlight 是两本账

`mAlarmsPerUid` 按登记 `Alarm.uid` 统计尚在等待结构中的 Alarm，用于每 UID 最大登记数。精确删除每一项都应扣一次；防御代码在扣多时移除键并记录 `wtf`，不会让负数留在表中。

Alarm 到期后从等待结构取出。若是 repeating，服务先安排下一 occurrence，随后旧 occurrence 在 `deliverAlarmsLocked()` 末尾扣掉，因此正常总数仍保持一枚。

已经成功发起的交付换到另一组状态：

```text
mInFlight
mBroadcastRefCount
BroadcastStats / FilterStats nesting
共享 *alarm* WakeLock
```

显式 cancel 扫描等待容器，不扫描 `mInFlight` 来撤回已经发送的 PendingIntent 或已经 post 的 listener callback。它可以取消 repeating 的下一 occurrence，却不能把正在执行的这一次倒回去；当前交付仍由 finished、complete 或 timeout 收账。

## 8. listener 怎样取得、取消并随进程死亡

客户端把 `OnAlarmListener` 包装为：

```java
ListenerWrapper extends IAlarmListener.Stub implements Runnable
```

`sWrappers` 是 `WeakHashMap<OnAlarmListener, WeakReference<ListenerWrapper>>`。同一个仍可查到的 listener 对象通常复用 wrapper；`cancel(listener)` 先按 Java listener 查 wrapper，再让 wrapper 调 `mService.remove(null, this)`。

因此不能创建一个“代码相同”的新 lambda 取消旧 listener，必须保留原 listener identity。若 weak map 已找不到活 wrapper，客户端只记录 `Unrecognized alarm listener`，不会向服务发 remove。

服务端登记 listener 前先：

```java
directReceiver.asBinder().linkToDeath(mListenerDeathRecipient, 0);
```

若此时 Binder 已不可达，`linkToDeath` 抛异常，Alarm 不登记、计数也不增加。App 进程后来死亡时，death recipient 把 Binder 转回 `IAlarmListener`，再复用精确四容器删除入口。

这与 PendingIntent 不同：PendingIntent record 在 system_server，Alarm 对 token 的强引用可跨普通 App 进程死亡继续存在；listener 的 Binder Stub、Handler 和 Java callback 都属于 App 进程，进程死亡后没有继续保留的执行主体。

r48 的 `AlarmManagerService` 可见 `linkToDeath()`，没有对应的显式 `unlinkToDeath()`。这是本 tag 的实现观察，不应推广成所有版本的契约。也不要把同一个 listener 对象当作多个并行 Alarm 的独立 identity：wrapper 的 Handler 和 completion 字段可变，重复复用会共享状态。

## 9. PendingIntent 自己失效时为什么还要异步清 Alarm

显式 `PendingIntent.cancel()`、相同 Key 的 `FLAG_CANCEL_CURRENT`、one-shot token 成功 send，以及包/用户生命周期清理，都可能让 `PendingIntentRecord` 失效。

`PendingIntentController.makeIntentSenderCanceled()` 在自身锁下先：

```java
rec.canceled = true;
```

随后通过 `AlarmManagerInternal` 通知：

```text
PendingIntentController
  → AlarmManagerInternal.remove(pi)
  → AlarmManager Handler 投递 REMOVE_FOR_CANCELED
  → Handler 持 AMS mLock 按 token 清四容器
```

LocalService 只 post 消息，不在 PendingIntentController 的锁内直接进入复杂 Alarm 清理，降低两套锁的顺序耦合。代价是“token 已 canceled”与“等待 Alarm 已删除”之间存在短暂窗口。

如果 Alarm 在窗口内先到期，`PendingIntent.send()` 会返回 canceled/抛 `CanceledException`。一次性 Alarm 已离开等待结构；repeating 在到期处理中已安排下一 occurrence，所以异常分支会再按 token remove，确保下一代被清掉。

listener 的 `doAlarm()` 若在建立 InFlight 前抛异常，则不发送 timeout、不建立等待中的 repeating 下一代，只增加 listener finish 诊断计数后返回。

## 10. 四种批量清理到底按谁匹配

精确 token 清理覆盖最全；生命周期方法有不同谓词：

| 方法 | 主身份 | Batch | idle pending | background pending | non-wakeup pending |
|---|---|---:|---:|---:|---:|
| `removeLocked(token)` | PI/listener token | ✓ | ✓ | ✓ | ✓ |
| `removeLocked(uid)` | `Alarm.uid` 登记者 | ✓ | ✓ | 逐项查 `Alarm.uid` | ✗ |
| `removeLocked(package)` | `sourcePackage` | ✓ | ✓ | 逐项查 source | ✗ |
| `removeForStoppedLocked(uid)` | 见下文 | ✓ | ✓ | 按外层 creator UID key 整组 | ✗ |
| `removeUserLocked(user)` | creator user | ✓ | ✓ | 按外层 creator UID user 整组 | ✗ |

`removeLocked(uid)` 对 system UID 直接返回，避免一次 force-stop 粗粒度清掉共享 system UID 的关键 Alarm。它会显式清相同登记 UID 的 `mNextWakeFromIdle`，并防御性检查 `mPendingIdleUntil`。

`removeLocked(package)` 的 `Alarm.matches(package)` 只比较 `sourcePackage`，还会在主 Batch 谓词中记录是否删到了 `mNextWakeFromIdle`。

`removeUserLocked(user)` 对主 Batch 和 idle pending 检查 `creatorUid` 的 user；background map 本来就以 `creatorUid` 为 key。删除 background 组时仍逐 Alarm 按各自登记 `uid` 扣计数。

这些差异不是命名风格，而是代理 PendingIntent、多用户和 shared UID 场景中真实不同的集合。

## 11. stopped 清理为什么存在额外不对称

`removeForStoppedLocked(uid)` 的主 Batch 谓词要求：

```java
a.uid == uid
        && ActivityManager.isAppStartModeDisabled(uid, a.packageName)
```

即先按登记者，再确认该调用包当前确实被禁止启动。idle pending 却只按 `a.uid == uid` 粗删。

background pending 更特殊。这个 map 的外层 key 在 Alarm 到期受限时使用的是 `alarm.creatorUid`，但 stopped 清理直接：

```java
if (mPendingBackgroundAlarms.keyAt(i) == uid) {
    decrementAlarmCount(uid, group.size());
    removeGroup();
}
```

正常 self-scheduled PendingIntent 中 creator UID 与登记 UID 相同，所以看不出问题。代理场景中它可能：

- 停登记者 A 时遗漏位于 creator B 分组的 Alarm；
- 停 creator B 时整组删除由其他 UID 代为登记的 Alarm；
- 用 B 扣 `mAlarmsPerUid`，而条目真实登记计数可能属于 A。

这是 Android 11 r48 的身份/计数不对称，应作为实现边界记录，不能泛化成预期 API 契约。

## 12. 生命周期广播各走哪条路径

`UninstallReceiver` 监听包、用户与 UID 事件，实际动作如下：

| 事件 | r48 动作 |
|---|---|
| `QUERY_PACKAGE_RESTART` | 只查询，不删除 |
| `PACKAGE_REMOVED` + `EXTRA_REPLACING=true` | 直接返回，更新期间保留状态 |
| `PACKAGE_REMOVED` 非替换 / `PACKAGE_RESTARTED` | 有有效 UID 时：清该 pkg/user history，再 `removeLocked(uid)` |
| `EXTERNAL_APPLICATIONS_UNAVAILABLE` | 通常无单 UID，逐 source package 清理 |
| `USER_STOPPED` | `removeUserLocked(user)`，再清该 user 的 App wakeup history |
| `UID_REMOVED` | 只清两张 AWI UID 辅助表，不调用 `removeLocked(uid)` |

有 UID 的包事件最终按登记 UID 清等待 Alarm，而不是按 source package 清。因此 shared UID 中一个包 restarted，可能删除该 UID 登记的 sibling 包 Alarm；wakeup history 却只清事件中的 package/user。

`QUERY_PACKAGE_RESTART` 调用的 `lookForPackageLocked()` 只看主 Batch 和 idle pending，不看 background/non-wakeup pending。它回答的是一个有限可见范围，不是“AMS 任意容器是否还有该包”。

用户停止按 creator user 清 Alarm，而不是等用户真正 removed。`USER_SYSTEM` 传给 `removeUserLocked()` 会直接返回，但 receiver 随后仍无条件清 user 0 的 App wakeup history：等待 Alarm 与配额历史出现不同结果。

`removeUserLocked()` 只删除该 user 的 `mLastAllowWhileIdleDispatch`，没有同步删除 `mUseAllowWhileIdleShortTime`；`UID_REMOVED` 才会按具体 UID 清两者。

包清理还移除 `mPriorities[pkg]` 和各 UID 下的 `BroadcastStats[pkg]`。所以生命周期动作不只改变队列，也有选择地清政策历史和诊断状态。

## 13. r48 的两个容器/缓存缺口

第一处是 pending non-wakeup：

```text
removeLocked(uid)
removeLocked(package)
removeForStoppedLocked(uid)
removeUserLocked(user)
```

四个批量方法都不扫描 `mPendingNonWakeupAlarms`；只有精确 token remove 明确扫描它。条目已到过 nominal deadline，只是因设备非交互而被延迟，因此生命周期事件后可能暂时残留，登记计数也延后到该列表以后被取出才收账。

实际后果要分层：

- PendingIntentController 往往还会取消对应 token，并异步触发精确清理；
- 即使等待条目抢先进入发送，canceled token 仍会拒绝交付；
- listener 进程死亡也有 Binder death 的精确清理；
- 所以不能从“批量方法漏扫”直接断言卸载应用一定会成功执行。

第二处是 `mNextWakeFromIdle`：UID/package remove 会显式使最早引用失效，再借 rebatch 重选；stopped/user remove 没有相同处理。若它们从主 Batch 删除的正是当前最早 AlarmClock，rebatch 开始时旧引用仍非空，可能继续成为 `getNextWakeFromIdleTime()` 和 idle-until 拉早逻辑的 stale 依据。

这表示缓存引用可能残留，不表示旧 Alarm 仍在 Batch 等待正常交付。两种结论必须分开。

## 14. 用三个场景检验模型

场景一：extras 不同，但其他 PendingIntent Key 相同。

```text
requestCode 相同、type 相同、filterEquals 相同
  → 取得同一 token
  → UPDATE_CURRENT 可更新旧 token extras
  → 再 set 时先删除旧等待 Alarm
```

若需要两枚共存，改变 requestCode 或 data URI，而不是只改业务 extra。

场景二：Alarm 已经进入 InFlight 后调用 `AlarmManager.cancel(pi)`。

```text
等待容器中没有当前 occurrence
  → cancel 不撤回已发送组件
  → 若 repeating，可删除已经安排的下一 occurrence
  → 当前交付仍靠 finished/complete/timeout 释放 WakeLock
```

场景三：A 替 B 的 PendingIntent 登记 Alarm，随后发生生命周期事件。

```text
force-stop/按 UID 清 A → 以 Alarm.uid 为主，通常选中登记项
按 source package 清 B → 以 sourcePackage 选中
停止 B 所在 user       → 以 creator user 选中
App Standby history     → 归 B package/user
```

如果 Alarm 已在 background pending，还要应用上一节的 creator-key/stopped 不对称，不能只凭表面包名下结论。

## 15. 静态阅读与诊断方法

先定位 token 与容器清理：

```bash
rg -n "class Key|filterEquals|FLAG_UPDATE_CURRENT|FLAG_CANCEL_CURRENT" \
  frameworks/base/services/core/java/com/android/server/am/PendingIntentRecord.java \
  frameworks/base/services/core/java/com/android/server/am/PendingIntentController.java \
  frameworks/base/core/java/android/content/Intent.java

rg -n "void removeLocked|removeForStoppedLocked|removeUserLocked|lookForPackageLocked" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "linkToDeath|mListenerDeathRecipient|REMOVE_FOR_CANCELED" \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java

rg -n "makeIntentSenderCanceled|AlarmManagerInternal" \
  frameworks/base/services/core/java/com/android/server/am/PendingIntentController.java
```

诊断 cancel 后“仍有 Alarm”时，依次确认：

```text
1. 使用的是原 token，还是只构造了看似相同的 Intent？
2. requestCode、sender type、user、resolvedType 和 identity flags 是否一致？
3. 目标是 PendingIntent token 还是 listener token？
4. 条目在 Batch、idle pending、background pending、non-wakeup pending 还是 InFlight？
5. 生命周期方法按 Alarm.uid、sourcePackage 还是 creator user 选集合？
6. PendingIntent token 是否已经 canceled，但 AMS Handler 尚未处理？
7. 是否命中了 r48 bulk remove 漏扫 non-wakeup 或 stale next-wake 边界？
8. mAlarmsPerUid 是等待计数，还是把它误当成 InFlight 计数？
```

现有测试能证明 token cancel、listener death、若干 UID/package/user 计数与精确 pending-non-wakeup 删除；测试存在不代表每个 bulk remove 都覆盖四容器，仍应逐方法对照矩阵。

## 16. 结论与下一章

完整模型可以压缩为：

```text
PendingIntent 参数
  → PendingIntentRecord.Key
  → 复用或创建 IIntentSender token
  → Alarm 直接按 token replacement/cancel

OnAlarmListener 对象
  → ListenerWrapper Binder token
  → 精确取消或 Binder death 清理

精确 token remove
  → Batch + idle/background/non-wakeup 三类 pending

生命周期 bulk remove
  → uid / source package / creator user 各自选集合
  → r48 不统一扫描 non-wakeup，stopped/user 不统一清 next-wake

已进入 InFlight
  → cancel 不撤回，完成协议另行收账
```

最终应记住六点：

1. extras 不参与 PendingIntent identity；重新取得相同系统 token 才能精确取消；
2. `UPDATE_CURRENT` 复用 token，`CANCEL_CURRENT` 使旧 token 失效并创建新代际；
3. 同 token 再 set 会先清旧等待项，AlarmManager cancel 不会取消 PendingIntent capability；
4. listener 依赖 App Binder Stub，进程死亡会触发精确四容器清理；
5. `Alarm.uid`、`sourcePackage`、`creatorUid` 分别服务不同政策，代理/shared UID 时不能互换；
6. bulk remove 漏扫 non-wakeup、stopped background creator-key 和 stale next-wake 是 r48 的实现边界，实际影响还要结合 PendingIntent 取消与发送兜底判断。

下一章进入已经无法“取消回去”的阶段：PendingIntent 与 listener 怎样建立 InFlight，谁持有共享 WakeLock，finished/complete/timeout 又怎样保证最后一个交付完成时准确归零。
