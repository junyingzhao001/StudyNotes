# 74-UsageStatsService、AppStandbyController 与应用使用/待机分桶链路

> 源码基线：Android 11（`android-11.0.0_r48`）  
> 本章目标：理解系统如何记录应用使用历史，并根据使用信号把应用放入不同 standby bucket，进而约束后台资源。  
> 阅读方式：Mac 上只读源码，不需要编译 AOSP。

---

## 1. 先拆开两个经常被混为一谈的问题

Android 的“应用使用情况”至少包含两套相关但不同的机制：

### 1.1 Usage Stats：历史上发生过什么

它回答：

- 某应用最近何时使用；
- 前台大约持续多久；
- 某时间范围有哪些 Activity resume/pause 等事件；
- 屏幕交互、配置变化和应用启动发生多少次；
- 日、周、月、年区间的聚合数据是什么。

### 1.2 App Standby：系统现在应该怎样对待它

它回答：

- 该应用目前属于 Active、Working Set、Frequent、Rare 还是 Restricted；
- 用户交互后应该提升到哪个 bucket、保持多久；
- 长期不用后何时降低 bucket；
- 系统应用、白名单、前台服务等是否可以豁免；
- JobScheduler、AlarmManager 应采用哪一级后台配额。

```text
Activity/通知/系统交互等事件
             │
             ├──────────────┐
             ▼              ▼
   UsageStatsService   AppStandbyController
   保存历史与聚合       更新活跃度与 bucket
             │              │
             ▼              ▼
 UsageStatsManager 查询   Job/Alarm/网络等策略消费者
```

最重要的第一句话是：

> UsageStats 是观察记录；standby bucket 是由多种信号和策略推导出的当前分类，不是简单查询“最近打开时间”得到的固定答案。

---

## 2. 源码地图

| 文件 | 作用 | 重点 |
|---|---|---|
| `frameworks/base/services/usage/java/com/android/server/usage/UsageStatsService.java` | 系统服务总入口 | 生命周期、事件排队、查询权限、Binder/LocalService |
| `.../UserUsageStatsService.java` | 单用户统计逻辑 | 时间转换、四级 interval、查询与 rollover |
| `.../UsageStatsDatabase.java` | 使用统计数据库 | 文件索引、读写、升级、裁剪、备份恢复 |
| `.../IntervalStats.java` | 一个时间区间的数据 | package stats、events、configuration stats |
| `frameworks/base/core/java/android/app/usage/UsageStatsManager.java` | 客户端 API 和常量 | query、bucket、reason |
| `frameworks/base/core/java/android/app/usage/UsageEvents.java` | 事件模型 | event type、迭代与字符串池 |
| `frameworks/base/apex/jobscheduler/service/java/com/android/server/usage/AppStandbyController.java` | 待机分桶策略 | usage signal、阈值、豁免、预测、监听器 |
| `.../AppIdleHistory.java` | bucket 历史持久化 | 当前 bucket、reason、时间轴 |
| `.../AppStandbyInternal.java` | system_server 内部接口 | Job/Alarm 等消费者接入 |
| `.../job/controllers/QuotaController.java` | Job 配额执行 | 根据 bucket 选择配额 |
| `frameworks/base/services/core/java/com/android/server/AlarmManagerService.java` | Alarm 延迟策略 | 根据 bucket 选择最小间隔 |

### 2.1 一个版本位置陷阱

Android 11 中：

- `UsageStatsService` 位于 `frameworks/base/services/usage`；
- `AppStandbyController` 位于 JobScheduler APEX 源码 `frameworks/base/apex/jobscheduler`。

两者 Java package 都是 `com.android.server.usage`，但源码物理目录和模块边界不同。只按 package 名搜索，容易忽略模块化迁移。

---

## 3. 运行进程、线程和锁

### 3.1 进程

核心 Java 服务运行在 `system_server`：

```text
system_server
  ├─ UsageStatsService
  ├─ UserUsageStatsService（每个已解锁用户一份）
  ├─ AppStandbyController
  ├─ JobSchedulerService / QuotaController
  └─ AlarmManagerService
```

App 通过 `IUsageStatsManager` Binder 查询；系统内部组件通过 `UsageStatsManagerInternal`、`AppStandbyInternal` 等 LocalServices 调用。

### 3.2 线程

`UsageStatsService.onStart()` 使用：

```java
mHandler = new H(BackgroundThread.get().getLooper());
```

大量事件先发给 BackgroundThread handler，再在服务锁下更新状态。这样 ActivityManager 等生产者不必同步等待磁盘处理。

`AppStandbyController` 也持有自己的 Handler/Looper，将检查 idle state、监听通知和写盘串行化。

### 3.3 两把不同的主要锁

- `UsageStatsService.mLock`：保护每用户服务、解锁状态、事件队列等；
- `AppStandbyController.mAppIdleLock`：保护 `AppIdleHistory` 和 bucket 状态。

虽然都在 system_server，不能随意跨锁回调。源码常通过 Handler 通知监听器，避免锁顺序复杂化。

---

## 4. 事件从哪里来

Usage event 并不是应用自己随意声明“我被用户使用了”。可信系统组件通过内部接口上报。

常见来源包括：

- ActivityTaskManager：Activity resumed、paused、stopped、destroyed；
- 系统屏幕/锁屏状态：interactive、non-interactive、keyguard shown/hidden；
- NotificationManager：通知可见或被用户交互；
- ShortcutService：快捷方式调用；
- Chooser/分享面板：chooser action；
- 配置变化；
- 前台服务、系统交互、设备启动/关机；
- App standby bucket 自身变化事件。

### 4.1 Activity 事件不仅有包名

`reportEvent(ComponentName, userId, eventType, instanceId, taskRoot...)` 可以携带：

- 当前 Activity package/class；
- Activity instance id；
- task root package/class；
- elapsed realtime 时间戳。

Android 11 的 `mUsageSource` 可决定使用“当前 Activity”还是“任务根 Activity”作为使用来源。这是为了避免某些中间界面或透明 Activity 把使用归属完全抢走。

### 4.2 为什么事件先用 elapsed realtime

生产事件时常使用：

```java
new UsageEvents.Event(type, SystemClock.elapsedRealtime());
```

elapsed realtime 单调增长，不受用户修改时间影响，适合跨线程排队和计算先后。但使用历史查询 API 使用的是墙上 Unix 时间，所以单用户服务稍后需要转换。

---

## 5. `UsageStatsService`：总入口和用户路由器

### 5.1 `onStart()` 做了什么

关键工作包括：

1. 获取 AppOps、UserManager、PackageManager；
2. 创建 BackgroundThread Handler；
3. 通过 `AppStandbyInternal.newAppStandbyController()` 创建待机控制器；
4. 创建 AppTimeLimitController；
5. 注册 bucket 变化监听器；
6. 注册包和用户广播；
7. 发布 `usagestats` Binder 服务；
8. 发布 system_server 内部 LocalService。

这里能看出 UsageStats 与 AppStandby 的关系：由同一个顶层服务装配并互通事件，但数据实现仍分开。

### 5.2 每个用户独立

```text
UsageStatsService
  └─ mUserState
      ├─ user 0 → UserUsageStatsService
      ├─ user 10 → UserUsageStatsService
      └─ work profile 11 → UserUsageStatsService
```

同一包在两个用户中有独立使用历史和 bucket。查询时必须同时确定调用 UID、目标 userId 和跨用户权限。

### 5.3 用户未解锁时怎么办

Android 11 使用统计正式目录在：

```text
/data/system_ce/<userId>/usagestats
```

CE 数据只有用户凭据解锁后可用。开机后、用户解锁前到达的事件不能直接写 CE，因此服务：

- 暂存在内存 `mReportedEvents`；
- 必要时持久化到 `/data/system_de/<userId>/usagestats` 的 pending 区域；
- 用户解锁后加载 pending events；
- 初始化 CE 中的 `UserUsageStatsService`；
- 按顺序回放事件并写 `USER_UNLOCKED`；
- 删除临时 DE 数据。

```text
Direct Boot 阶段事件
   → 内存/DE pending
   → 用户解锁
   → 初始化 CE database
   → 回放 pending
   → 清理 DE pending
```

这条链是理解多用户与 FBE 的好例子：不能因为服务已经启动，就认为每个用户的私有历史已经可读。

---

## 6. 时间转换：最容易出错的一层

### 6.1 两份快照

`UserUsageStatsService` 保存：

```java
mRealTimeSnapshot = SystemClock.elapsedRealtime();
mSystemTimeSnapshot = System.currentTimeMillis();
```

事件携带 elapsed realtime，转换公式大致是：

```java
event.mTimeStamp = Math.max(0,
        event.mTimeStamp - mRealTimeSnapshot) + mSystemTimeSnapshot;
```

也就是用“事件相对 elapsed 快照的差值”投影到墙上时间。

### 6.2 用户改时间会发生什么

每次处理/查询前，服务估算当前墙上时间应该是：

```text
expectedSystemTime
  = 当前 elapsedRealtime - 旧 elapsed 快照 + 旧 systemTime 快照
```

如果真实 `currentTimeMillis()` 与 expected 相差超过 2 秒，认为墙上时间发生改变，并调用数据库的 time change 修正逻辑。

### 6.3 为什么不能只保存 elapsed realtime

elapsed realtime 重启后从头开始，无法直接回答“昨天下午 3 点用了什么应用”；墙上时间适合长期查询，但会被手工调整/NTP 校正。系统只能结合两者：事件排序阶段依赖单调时间，持久化与 API 查询依赖墙上时间。

### 6.4 启动/关机的缺口

重启期间没有连续 elapsed 时钟。初始化时，源码会在上次保存/事件末尾补 `DEVICE_SHUTDOWN`，并在当前时间加入 `DEVICE_STARTUP`，让消费者知道中间存在设备离线边界，而不是误把跨重启区间当成持续活动。

---

## 7. `UserUsageStatsService.reportEvent()` 的主流程

主线可以压缩为：

```text
接收 elapsed 时间事件
  → 检查墙上时间是否跳变
  → 转换为 wall-clock 时间
  → 到达日边界则 rollover
  → 将可公开事件加入 daily event list
  → 更新 daily/weekly/monthly/yearly 四套 IntervalStats
  → 标记 stats changed，延迟刷盘
```

### 7.1 为什么只在 daily 保存细粒度 events

源码将事件加入 `currentDailyStats.addEvent(event)`，而聚合 `UsageStats` 会更新所有 interval。细粒度事件数量大，长期按月/年完整复制代价太高；长期区间更适合保存聚合。

部分私有/内部事件不会直接加入公开 event list，例如：

- `SYSTEM_INTERACTION`；
- 内部 `FLUSH_TO_DISK`；
- 某些 `ACTIVITY_DESTROYED` 会丢弃或转换；
- `DEVICE_SHUTDOWN` 会在下次启动时补入。

“内部逻辑处理了事件”不等于查询 `queryEvents()` 一定能看到原始事件。

### 7.2 Activity 前台时间怎样累计

`IntervalStats.update()` 根据 resumed/paused/stopped 以及 instanceId 维护活动状态和时间。一个包可能有多个 Activity 实例，不能用单一 boolean 简单表示前台。

Android 11 事件命名以 `ACTIVITY_RESUMED/PAUSED/STOPPED` 为主；旧 API 中的 `MOVE_TO_FOREGROUND/BACKGROUND` 语义与版本兼容有关。阅读时应看当前源码 switch，而不是只记旧教程常量。

### 7.3 App launch count

当新的 `ACTIVITY_RESUMED` package 与 `mLastBackgroundedPackage` 不同，源码可能增加 launch count。它是 Framework 的统计规则，不等价于 Linux 进程新启动次数：Activity resume 时进程可能早已存在。

---

## 8. 四类 interval 与 rollover

`UserUsageStatsService` 同时维护：

| interval | 大致长度 | 用途 |
|---|---:|---|
| DAILY | 1 天 | 细粒度事件和日聚合 |
| WEEKLY | 1 周 | 周期聚合 |
| MONTHLY | 1 月 | 月度趋势 |
| YEARLY | 1 年 | 长期趋势 |

另有 `INTERVAL_BEST`：让数据库根据查询跨度选择最合适的已存 bucket。

### 8.1 这不是滚动窗口

这里的 interval 更像对齐日历的文件区间，不是“从当前时刻往前恰好 24 小时”的滑动窗口。

```text
日区间 A | 日区间 B | 当前日内存区间 C
```

跨过 daily expiry 时，`rolloverStats()`：

- 对仍处于前台等活跃状态的 package 执行区间结束处理；
- 把当前 stats 写盘；
- prune 过期文件；
- 加载/创建新一组 active stats；
- 将跨边界的持续状态续接到新 interval。

### 8.2 为什么四套都更新

每个事件到来时更新 daily/weekly/monthly/yearly 当前对象。这样查询长时间范围不必扫描并合并海量 daily 文件，代价是写入时维护多份不同粒度聚合。

---

## 9. 内存、磁盘和查询如何拼接

当前 interval 不会每来一个事件就写盘。默认 flush interval 是 20 分钟，用户停止、关机、显式 flush 或 rollover 也会持久化。

查询时 `queryStats()`：

1. 若 `INTERVAL_BEST`，先选择合适 interval；
2. 从数据库读取已结束区间；
3. 把查询 end 截到当前内存区间 begin 之前，避免重复；
4. 若时间范围覆盖当前 interval，再把内存 stats 合并进去。

```text
查询范围
|--------- 磁盘历史 ---------|---- 当前内存 ----|
           database query          combine copy
```

因此刚发生的使用事件即使尚未落盘，也可以在查询结果中出现。反过来，直接查看磁盘文件可能误以为最新数据丢了。

### 9.1 为什么 combine 当前对象时复制

当前 `IntervalStats` 仍会继续变化。返回查询结果时创建 `UsageStats` 副本，避免调用者持有内部可变对象或后续更新改变已返回结果。

---

## 10. `UsageStatsDatabase` 的文件和生命周期

每用户 CE 目录通常包含按 interval 分类的数据以及 mappings/version 等辅助文件。数据库负责：

- 扫描并按 beginTime 建索引；
- 读写 `IntervalStats`；
- XML/proto 版本升级；
- package token 映射和混淆；
- prune 过旧区间；
- package 卸载时清理关联数据；
- backup/restore 和系统升级处理。

### 10.1 为什么要 package mapping/obfuscation

Android 11 在写盘时可将包名等字符串映射为 token，减少重复和控制敏感信息暴露。读取时需要相应 mapping 还原。包卸载、恢复和首次启动的 mapping 更新顺序因此很重要。

### 10.2 多久保留不是 API 契约

不同 interval 有不同 prune 周期，厂商/版本实现也可能调整。应用不应把 UsageStats 当作永久审计数据库。查询过去很久的数据返回空，并不一定代表应用从未使用。

---

## 11. 查询 API 与安全边界

### 11.1 为什么使用记录敏感

使用历史能推断用户习惯、工作时间、健康和社交行为。因此普通应用不能任意查询所有包。

`UsageStatsService.BinderService` 通常综合检查：

- `PACKAGE_USAGE_STATS`；
- AppOps `GET_USAGE_STATS`；
- 调用 package 是否属于 calling UID；
- 目标 userId 和跨用户权限；
- instant app、shortcut、locus、notification 等额外可见性规则。

即使 manifest 声明 `PACKAGE_USAGE_STATS`，用户仍需在“使用情况访问权限”设置中授权对应 AppOp。

### 11.2 查询聚合与查询事件

- `queryUsageStats()`：返回每包聚合值；
- `queryEvents()`：返回时间范围内事件流；
- `queryEventsForSelf()`/package 变体：只看调用者自身，权限规则不同；
- `queryConfigurationStats()`：配置使用情况；
- `queryEventStats()`：屏幕/锁屏等事件聚合。

事件查询会根据调用者能力：

- 隐藏 shortcut invocation；
- 隐藏 locus id；
- 混淆 notification event；
- 混淆 instant app 包信息；
- 决定是否包含 task root。

权限检查不是只有“整份允许/拒绝”，还包括字段级降级与混淆。

### 11.3 查询时间使用墙上时间

客户端传入 `beginTime/endTime` 是 Unix epoch 毫秒，并采用半开区间思想：事件时间通常需满足 `begin <= t < end`。若 end 不大于 begin，或 begin 在未来，源码会判定 range 无效。

---

## 12. standby bucket 的数值和语义

Android 11 常量：

| bucket | 值 | 直观含义 |
|---|---:|---|
| EXEMPTED | 5 | 豁免待机限制的特殊分类 |
| ACTIVE | 10 | 正在或最近明显被用户使用 |
| WORKING_SET | 20 | 经常使用但不是当前活跃 |
| FREQUENT | 30 | 有规律、频率较低 |
| RARE | 40 | 很少使用 |
| RESTRICTED | 45 | 因后台资源行为/策略受到更强限制 |
| NEVER | 50 | 未使用/未知初始状态等 |

数值越大通常限制越强，但不要写 `bucket++` 推进：数值不是连续枚举，且 EXEMPTED 是更小的特殊值。

### 12.1 Rare 以上何时算 idle

`AppIdleHistory` 的判断本质上使用 cutoff；`AppStandbyController` 通知 listener 时：

```java
final boolean idle = bucket >= STANDBY_BUCKET_RARE;
```

所以某些旧接口只返回 boolean app idle，会把 Rare、Restricted、Never 折叠成同一类；新 bucket API 才保留分级信息。

### 12.2 Restricted 在 Android 11 的版本边界

Android 11 源码已定义 Restricted，但 `mAllowRestrictedBucket` 可以控制是否真正允许。禁用时会降为 Rare。不能只因常量存在，就断言所有 Android 11 设备一定对外启用了相同行为。

---

## 13. bucket reason：不仅要知道结果，还要知道原因

reason 高 8 位是 main reason，低 8 位是 sub reason：

```text
reason = REASON_MAIN_* | REASON_SUB_*
```

主要原因包括：

- DEFAULT：默认/应用更新；
- TIMEOUT：长时间未使用而自然下降；
- USAGE：用户或系统使用信号带来提升；
- FORCED_BY_USER：shell/用户强制；
- PREDICTED：预测器给出的 bucket；
- FORCED_BY_SYSTEM：系统因异常资源使用等强制。

usage 子原因又包括：

- system interaction；
- notification seen；
- user interaction；
- move to foreground/background；
- sync adapter；
- foreground service start；
- slice pinned 等。

两个应用都在 Active，一个可能是刚被用户打开，另一个可能是系统豁免/强制。只看 bucket 不看 reason，会丢失策略诊断最关键的信息。

---

## 14. `AppIdleHistory` 的时间为什么又不完全等于 elapsed realtime

待机降级关注“设备有机会被用户使用但应用一直没被使用多久”。设备关机、长期深睡或屏幕关闭是否应全部算入，需要策略定义。

`AppIdleHistory` 维护：

- elapsed duration；
- screen-on duration；
- 每包 last used elapsed/screen time；
- currentBucket、bucketingReason；
- prediction 和 job 运行等时间。

它会把跨重启的累计 duration 写盘，并用本次启动的 elapsedRealtime 接续。因此这里的“elapsed time adjusted”是 AppIdleHistory 自己构造的持久化时间轴，不等同于直接把 `SystemClock.elapsedRealtime()` 写进文件后跨重启比较。

### 14.1 为什么同时看 elapsed 和 screen time

如果备用机放抽屉一周、屏幕从未点亮，仅按墙上时间可能让所有应用迅速降级。结合 screen-on threshold，可以要求设备确实经历了一定用户使用机会后再判定某 App 被冷落。

具体阈值可由 DeviceConfig/Settings 调整，不应把某组小时/天数当成永久 AOSP 契约。

---

## 15. 使用事件怎样提升 bucket

`AppStandbyController.reportEvent()` 获取包名、event type 和 elapsed time，在锁内调用 `reportEventLocked()`。

不同事件的提升强度和保持时长不同。概念上：

```text
强用户交互 / Activity resumed
       → ACTIVE，并设 active timeout

通知被看见等较弱信号
       → 至少 WORKING_SET 或短期提升

前台服务启动、sync 等系统信号
       → 根据规则提升或延长
```

源码不是简单执行 `currentBucket = ACTIVE`。`AppIdleHistory.reportUsage()` 会：

- 写 last used 时间；
- 判断是否允许从 Restricted 提升；
- 只在新 bucket 更活跃时提升；
- 设置 `REASON_MAIN_USAGE | subReason`；
- 保存 bucket expiry，避免一个瞬时信号永久保持 Active。

### 15.1 Restricted 的特殊性

若应用因系统强制原因进入 Restricted，普通非用户 usage 信号可能不能把它轻易救出；真实用户交互通常具有更高权重。这防止一个行为异常的应用通过自发后台工作不断“刷活跃”。

### 15.2 通知并不等于用户打开应用

“通知出现”“通知被看见”“用户点击通知”是不同强度的信号。策略可能给较弱提升或不同 timeout，不能笼统地说“发通知就永远变 Active”。

---

## 16. 长期不用怎样降级

`checkAndUpdateStandbyState()` 是核心降级方法之一。简化流程：

```text
读取 package 当前 AppUsageHistory
  → 计算该包可达到的最活跃 minBucket（豁免边界）
  → 检查强制 reason / prediction 是否仍有效
  → 比较 last used elapsed/screen time 与各级 threshold
  → 得出 ACTIVE / WORKING_SET / FREQUENT / RARE / RESTRICTED
  → 若变化，写 AppIdleHistory 并通知 listeners
```

### 16.1 minBucket 是什么

有些应用不能被降到普通 Rare：

- 系统/平台关键包；
- device owner/profile owner；
- active admin；
- 默认拨号器、短信等角色；
- widget、VPN、载具模式等特定系统关系；
- 电池优化白名单或其他豁免；
- 当前前台/活跃 UID。

`getAppMinBucket()` 返回它允许达到的最受限边界。最终算法不能把应用降得比该边界更严格。

### 16.2 预测 bucket

系统智能组件可以提供 predicted bucket。控制器记录 predicted time/bucket，但预测有有效期；过期后回到基于真实使用时间的 timeout 逻辑。

预测也受实际 usage expiry 限制：刚刚真实使用过的应用不能被一个更差预测立刻推到 Rare。

---

## 17. bucket 怎样传给执行者

`AppStandbyController` 不是自己暂停 Job 或延后 Alarm。它通过 `AppIdleStateChangeListener` 发布变化：

```text
bucket 改变
  ├─ UsageStatsService 记录 STANDBY_BUCKET_CHANGED event
  ├─ JobSchedulerService / QuotaController 更新 job 配额
  ├─ AlarmManagerService 重新评估 alarm 延迟
  └─ 其他 AppStandbyInternal 消费者更新策略
```

这体现“策略状态源”和“执行器”分离：AppStandbyController 判断分类，各资源管理器按自己的规则解释分类。

### 17.1 JobScheduler

`JobSchedulerService` 把 bucket 映射为内部 standby index；`QuotaController` 为不同 index 设置执行时间、job 次数、session 数等配额。

一般趋势：

```text
ACTIVE → 配额最宽
WORKING_SET
FREQUENT
RARE → 配额很窄
RESTRICTED/NEVER → 更严格或特殊处理
```

但运行中的前台 UID、用户交互、charging、特权 job 等还可能影响“effective bucket”和实际约束。

### 17.2 AlarmManager

AlarmManager 根据 bucket 选择 standby delay/最小投递间隔。越不活跃的 App，普通后台 alarm 越可能被推迟和批处理。

以下 alarm 可能有豁免或另一套规则：

- alarm clock；
- foreground-related；
- 系统 UID；
- `allowWhileIdle`（仍受其自身限频）；
- 电池优化白名单等。

因此 bucket=Rare 不表示“所有 Alarm 一个都不执行”。

### 17.3 网络限制不是 Controller 直接断网

文档/API 说明 Frequent 以上可能受到网络限制，但真实执行还要结合 NetworkPolicy、Data Saver、Doze、前后台状态和厂商策略。standby bucket 是输入之一，不是唯一总开关。

---

## 18. App Standby、Doze、Background Restriction 的区别

| 机制 | 主要粒度 | 核心问题 |
|---|---|---|
| App Standby Bucket | 每用户、每应用 | 用户最近多常使用这个 App |
| Device Idle/Doze | 整台设备 | 设备长时间未使用时怎样进入维护窗口 |
| Battery Saver | 整机模式 + 各服务策略 | 低电/用户开启省电后降低总体功耗 |
| Background Restriction | 每应用强约束 | 用户/系统是否禁止其后台活动 |
| Data Saver | 网络/UID | 计量网络下是否允许后台数据 |

它们会叠加。例如 Rare 应用在深度 Doze 中安排普通 Job，同时受到 bucket 配额和 Doze 窗口限制。不能看到一次延迟就只归因于 standby bucket。

---

## 19. bucket 变化为何也写回 UsageEvents

`UsageStatsService` 注册 standby listener：

```java
public void onAppIdleStateChanged(..., int bucket, int reason) {
    Event event = new Event(Event.STANDBY_BUCKET_CHANGED,
            SystemClock.elapsedRealtime());
    event.mBucketAndReason = (bucket << 16) | (reason & 0xFFFF);
    event.mPackage = packageName;
    reportEventOrAddToQueue(userId, event);
}
```

高 16 位放 bucket，低 16 位放 reason，进入使用事件时间线。这样诊断者不仅知道当前 bucket，还能看到何时为何发生变化。

但要注意方向：

```text
usage event → 可能改变 standby bucket
standby bucket changed → 又记录一个 UsageEvent 供历史查询
```

这是反馈记录，不代表无限递归；`STANDBY_BUCKET_CHANGED` 并不会按普通用户交互规则再次提升自己。

---

## 20. 包安装、卸载、更新与多用户

### 20.1 新安装应用

没有历史时常处于 NEVER/default 状态，系统安装器、首次启动或系统默认初始化可能给予特定 bucket。不能假设“安装完成立即 Rare”。

### 20.2 应用更新

更新会影响 package mapping、版本数据和 default reason。系统更新后的系统应用还可能被初始化到较活跃 bucket，避免 OTA 后关键组件因缺少近期使用记录被错误限制。

### 20.3 卸载

UsageStatsDatabase 删除/裁剪包数据和 token mapping；AppIdleHistory 也需移除状态。若保留数据安装等标志存在，具体清理路径会不同。

### 20.4 Cross-profile

工作资料与个人资料同包默认是不同 userId、不同历史。Android 11 的 controller 有 cross-profile sharing 配置：允许某些跨 profile 应用共享 standby 活跃信号。但这不是把两边 UsageStats 数据库合并。

---

## 21. 常见命令与“只读学习”方式

本机 Mac 无需真正执行 Android 命令，只需理解它们对应的源码入口。

```bash
adb shell dumpsys usagestats
adb shell am get-standby-bucket PACKAGE
adb shell am set-standby-bucket PACKAGE rare
```

不同版本 shell 子命令可能变化，源码中应搜索 `BinderService.dump()`、`onShellCommand()`、ActivityManager shell 和 `setAppStandbyBucket()`。

shell 强制设置产生 `REASON_MAIN_FORCED_BY_USER` 或相应 forced reason，不等同于等待真实 timeout 自然降级。实验结束后也要恢复状态，否则后续观察会被人为 bucket 污染。

---

## 22. 源码阅读练习路线

### 第一轮：事件到数据库

```bash
rg -n "reportEventOrAddToQueue|MSG_REPORT_EVENT|reportEvent\(" \
  frameworks/base/services/usage/java/com/android/server/usage/UsageStatsService.java \
  frameworks/base/services/usage/java/com/android/server/usage/UserUsageStatsService.java
```

回答：时间在哪一步从 elapsed 变成 wall clock？

### 第二轮：查询的磁盘/内存拼接

```bash
sed -n '400,540p' \
  frameworks/base/services/usage/java/com/android/server/usage/UserUsageStatsService.java
```

画出 truncatedEndTime，并说明为什么不会重复当前 interval。

### 第三轮：usage signal 到 bucket

```bash
rg -n "reportEventLocked|reportUsage|checkAndUpdateStandbyState|getAppMinBucket" \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/usage/AppStandbyController.java \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/usage/AppIdleHistory.java
```

记录每类事件提升到哪里、reason 是什么、expiry 多久；阈值从当前配置读取，不照抄网络文章。

### 第四轮：bucket 到执行器

```bash
rg -n "AppIdleStateChangeListener|STANDBY_BUCKET" \
  frameworks/base/apex/jobscheduler/service/java/com/android/server/job \
  frameworks/base/services/core/java/com/android/server/AlarmManagerService.java
```

区分“原始 bucket”和 JobScheduler 计算的 effective standby bucket。

---

## 23. 一个完整案例：很久没打开的天气 App 被用户重新打开

初始状态：

```text
Weather App user 0
  bucket = RARE
  reason = TIMEOUT
```

### 23.1 用户点击图标

```text
Launcher/ATMS 启动 Activity
  → Activity resumed
  → UsageStatsManagerInternal.reportEvent()
```

### 23.2 UsageStats 分支

```text
UsageStatsService handler
  → 路由到 user 0
  → elapsed timestamp 转 wall clock
  → daily events 加 ACTIVITY_RESUMED
  → 四套 IntervalStats 更新 lastTimeUsed/foreground time/launch count
  → 标脏，稍后落盘
```

### 23.3 AppStandby 分支

```text
AppStandbyController.reportEventLocked()
  → 识别 MOVE_TO_FOREGROUND/USER_INTERACTION
  → AppIdleHistory.reportUsage()
  → bucket 从 RARE 提升到 ACTIVE
  → reason = USAGE | MOVE_TO_FOREGROUND
  → 写 active timeout
  → listener 通知 bucket changed
```

### 23.4 消费者分支

```text
QuotaController 收到变化
  → 重新评估该 UID jobs

AlarmManagerService 收到变化
  → 重新排序/检查受 standby 影响的 alarms

UsageStatsService listener
  → 再记录 STANDBY_BUCKET_CHANGED 历史事件
```

### 23.5 后续再次长期不用

Active timeout 过期后，定期 idle check 比较 elapsed/screen thresholds，应用逐步进入 Working Set、Frequent、Rare；具体速度受配置、预测、通知、同步、前台服务、系统角色和白名单影响。

---

## 24. 常见误区纠正

### 误区 1：UsageStatsService 和 AppStandbyController 是同一个数据库

不对。前者保存 CE 中的历史/聚合；后者通过 AppIdleHistory 保存 bucket、reason 和活跃时间。

### 误区 2：最近使用时间超过固定天数就一定进入 Rare

不对。阈值可配置，还需 screen time、minBucket、预测、expiry、豁免和 forced reason。

### 误区 3：应用进程活着就说明它处于 Active bucket

不对。进程状态和 standby bucket 是不同维度；缓存进程可以属于 Rare，用户当前使用的 UID 会在执行策略中得到临时活跃处理。

### 误区 4：Rare 表示应用完全不能运行

不对。它通常意味着更小配额和更长延迟；系统窗口、豁免、用户交互和特殊任务仍可执行。

### 误区 5：前台服务永久把应用保持 Active

不对。它是一种 usage signal/豁免因素，具体提升级别和期限由版本策略决定，也受到后台限制等其他机制约束。

### 误区 6：`queryUsageStats()` 返回的是精确秒表

不对。它依赖生命周期事件完整性、崩溃/重启补偿、interval 聚合和时间修正，是系统级使用估计。

### 误区 7：`INTERVAL_DAILY` 就是最近 24 小时

不对。它是按日历边界组织的统计粒度，查询范围仍由 begin/end 决定。

### 误区 8：manifest 有 PACKAGE_USAGE_STATS 就能读取

不对。还需要用户授予 usage access 对应 AppOp，并通过包/UID和用户检查。

### 误区 9：修改系统时间只会影响显示

不对。UsageStatsDatabase 以墙上时间组织文件，服务必须检测并修正时间跳变。

### 误区 10：bucket 直接控制所有网络、Job、Alarm

不对。bucket 是策略输入，各执行器结合前台、白名单、Doze、Data Saver 等计算最终行为。

---

## 25. 排查“后台任务为什么没执行”的分层方法

### 第一层：确认任务自身

- Job/Alarm 是否成功注册？
- 约束是否满足？
- 是否被应用取消或进程更新清除？

### 第二层：确认应用当前身份

- 正确 package、UID、userId？
- 工作资料是否处于 quiet mode？
- 包是否 stopped/suspended？

### 第三层：确认 standby

- 当前 bucket 和 reason？
- 是自然 timeout、预测还是 forced？
- 是否存在 expiry/minBucket/豁免？

### 第四层：确认执行器

- JobScheduler effective bucket 与 quota？
- Alarm standby delay？
- UID 当前 active/foreground 是否暂时放宽？

### 第五层：确认叠加策略

- Doze/maintenance window；
- Battery Saver；
- Background Restriction；
- Data Saver/NetworkPolicy；
- 厂商自定义电源策略。

这样能避免看到 Rare 后就停止调查。

---

## 26. 练习题

### 题 1

为什么事件入口使用 elapsed realtime，而查询接口使用 Unix wall time？

### 题 2

用户尚未解锁时 Activity 使用事件如何避免丢失？

### 题 3

为什么查询需要同时合并磁盘历史和当前内存 interval？

### 题 4

`INTERVAL_DAILY` 与 standby bucket 的 ACTIVE/RARE 是同一类 bucket 吗？

### 题 5

一个应用 currentBucket=Rare，但 `getAppMinBucket()` 返回 Working Set，下一次检查应怎样处理？

### 题 6

为什么系统要保存 bucket reason，而不是只保存数字 40？

### 题 7

用户刚打开 Rare 应用后，为什么其普通 Job 可能仍不一定立刻执行？

### 参考答案

1. elapsed 单调、适合事件排序；wall time 可跨重启并符合用户查询时间。服务用双快照转换并检测时间跳变。
2. 先进入内存/DE pending，解锁后初始化 CE database，再顺序回放并清理 pending。
3. 当前区间为减少 I/O 尚未落盘；只查磁盘会漏最新数据，只查内存会漏历史。
4. 完全不是。前者是数据库时间粒度，后者是应用活跃等级。
5. 不能比 minBucket 更受限，应提升/夹到 Working Set 边界并通知变化。
6. 40 可能来自 timeout、预测或强制；不同原因决定后续什么信号能覆盖以及如何诊断。
7. 还可能有其他 Job 约束、Doze、quota 重新计算/调度时机、网络要求或后台限制。

---

## 27. 复读后的易混淆点补强

从初学者视角复读后，最容易卡住的是四组“时间”和三组“bucket”。

### 27.1 四种时间放在一起

| 时间 | 用途 |
|---|---|
| `elapsedRealtime` | 本次启动内事件排序，含深睡且不受改时钟影响 |
| `currentTimeMillis` | 对外查询和日历文件边界 |
| AppIdleHistory adjusted elapsed | 跨重启累计的待机判断时间轴 |
| screen-on duration | 衡量设备真正提供了多少用户使用机会 |

它们不会互相替代。

### 27.2 三种 bucket 放在一起

| 名称 | 含义 |
|---|---|
| usage interval bucket | daily/weekly/monthly/yearly 数据存储粒度 |
| standby bucket | Active/Working Set/Frequent/Rare 等活跃分类 |
| JobScheduler internal/effective bucket | Job 执行器结合 UID active、豁免等得到的实际配额索引 |

看到源码变量名 `bucket`，必须先问它属于哪一套坐标系。

### 27.3 “使用过”不是单一 boolean

打开 Activity、看到通知、点击通知、运行前台服务、系统交互、sync 都可成为不同强度信号。Controller 保存 reason 和 expiry，正是为了表达“怎样使用过”和“影响保持多久”。

### 27.4 分类与执行不是同一步

AppStandbyController 像信用评级机构，只给出等级；JobScheduler、AlarmManager、NetworkPolicy 像不同业务部门，各自根据等级和现场条件决定额度。Rare 是重要输入，但不是一条直接 kill/deny 命令。

---

## 28. 本章总结

完整链路：

```text
ATMS/通知/系统组件报告可信 usage event
  → UsageStatsService 在 BackgroundThread 按 user 路由
  → 未解锁先存 DE pending，解锁后进入 CE
  → UserUsageStatsService 将 elapsed 转 wall time
  → 更新 daily events 与四级 IntervalStats
  → UsageStatsDatabase 延迟持久化、rollover、prune
  → UsageStatsManager 在权限/AppOps/用户边界下查询

同一 usage signal
  → AppStandbyController.reportEventLocked
  → AppIdleHistory 更新 last used、expiry、bucket、reason
  → timeout/prediction/minBucket/forced reason 共同决定分类
  → listener 通知 JobScheduler、AlarmManager 等
  → 各执行器叠加 quota、Doze、前台与豁免形成最终行为
```

本章最该记住的不是各 bucket 的某组固定小时数，而是六个边界：

1. 历史统计与策略分类的边界；
2. elapsed time 与 wall time 的边界；
3. CE 正式历史与 DE pending 的边界；
4. usage interval bucket 与 standby bucket 的边界；
5. bucket 和 reason 的边界；
6. 分类状态与 Job/Alarm 实际执行的边界。

下一章将学习 `AppTimeLimitController、UsageObserver 与 Digital Wellbeing 使用时长限制链路`，继续沿 UsageStats 体系理解应用计时观察者、session、回调和跨重启状态管理。

