# 73-statsd、StatsCompanionService 与 Atom 系统遥测链路

> 源码基线：Android 11（`android-11.0.0_r48`）  
> 本章目标：理解 Android 如何把分散在各模块的结构化事件变成可配置、可聚合、可拉取的系统指标。  
> 阅读方式：Mac 上只读源码，不要求编译或连接真机。

---

## 1. 先建立正确印象：statsd 不是“大号 Logcat”

Android Framework 中经常能看到：

```java
FrameworkStatsLog.write(
        FrameworkStatsLog.SCREEN_STATE_CHANGED,
        state);
```

如果只看这一行，很容易把它理解成“打印了一条日志”。实际上，它进入的是一套结构化统计系统：字段类型、Atom 编号、调用者身份和时间戳都有固定协议；`statsd` 根据下发的配置决定是否匹配、怎样分桶、按哪些维度切片、是否计算时长或阈值。

```text
业务模块写 Atom
      │
      ▼
生成的 StatsLog API / libstats
      │ Unix datagram: /dev/socket/statsdw
      ▼
statsd 原生进程
      ├─ 解析 LogEvent
      ├─ 按 StatsdConfig 匹配
      ├─ 更新 Count/Duration/Value/Gauge/Event 指标
      ├─ 按 UID、状态等维度切片并按时间分桶
      └─ 持久化或应请求输出 proto 报告

另一条方向：
statsd ──按计划触发 pull──> Framework/系统服务回调返回当前快照
```

先记住三个结论：

1. **Atom 是结构化事实，不是最终指标。**
2. **StatsdConfig 决定哪些事实被怎样计算。**
3. **statsd 通常保存聚合结果，而不是无限保存全部原始事件。**

---

## 2. 为什么已经有 logcat、BatteryStats，还需要 statsd

### 2.1 logcat 更适合人读的调试文本

`Log.d(TAG, "...")` 的主要价值是开发者阅读。文本格式可能改变，字段含义不够严格，大量长期保存也昂贵。

### 2.2 BatteryStats 是电池领域的专用账本

上一章的 BatteryStats 有专门的 on-battery 时间基准、WakeLock、CPU、网络与功耗模型。它回答的是耗电归因问题，并不是通用指标平台。

### 2.3 statsd 是配置驱动的通用统计引擎

它可以表达：

- 某事件发生了多少次；
- start/stop 之间持续多久；
- 某个数值在一小时内的总和、最小值、最大值或变化；
- 周期性抓取某系统状态；
- 按 UID、状态、标签等维度拆分；
- 满足阈值后触发订阅或告警。

它更像一个运行在设备上的小型流式指标引擎，而不是文本日志仓库。

---

## 3. 源码地图与进程边界

| 文件/目录 | 角色 | 重点 |
|---|---|---|
| `frameworks/base/cmds/statsd/src/atoms.proto` | Atom 总协议 | pushed/pulled oneof、字段注解 |
| `frameworks/base/cmds/statsd/src/statsd_config.proto` | 配置协议 | matcher、predicate、metric、alert |
| `frameworks/base/apex/statsd/framework/java/android/util/StatsLog.java` | Java 写事件入口 | JNI 与 `StatsEvent` |
| `system/core/libstats/` | Native 写事件库 | `statsdw` socket |
| `frameworks/base/cmds/statsd/src/main.cpp` | statsd 进程入口 | Binder、Looper、事件队列、socket listener |
| `.../socket/StatsSocketListener.cpp` | 接收 pushed Atom | 调用者凭据、解析、入队 |
| `.../StatsLogProcessor.cpp` | 事件处理核心 | 配置路由、UID 映射、报告与持久化 |
| `.../metrics/*MetricProducer.cpp` | 指标计算器 | event/count/duration/value/gauge |
| `.../external/StatsPullerManager.cpp` | pull 调度 | 缓存、超时、回调 |
| `.../config/ConfigManager.cpp` | 配置管理 | 加载、更新、删除 |
| `.../packages/UidMap.cpp` | UID 与包版本映射 | 多用户、升级、isolated UID |
| `frameworks/base/apex/statsd/service/.../StatsCompanionService.java` | Java 世界助手 | Alarm、UID/包信息、生命周期 |
| `.../StatsManagerService.java` | `StatsManager` Binder 后端 | 权限、配置、pull callback、重连缓存 |
| `.../android/app/StatsManager.java` | System API 客户端 | 加配置、取报告、注册 puller |

### 3.1 两个主要进程

```text
system_server
  ├─ StatsCompanionService
  └─ StatsManagerService
          ⇅ Binder
独立 native statsd 进程
  ├─ StatsService（Binder 服务名 "stats"）
  ├─ StatsSocketListener
  ├─ StatsLogProcessor
  └─ MetricProducer / Puller / Storage
```

`StatsCompanionService` 虽然名字像主服务，实际上注释直接称它为 native statsd 的 helper。核心聚合引擎在独立的 C++ `statsd` 进程中。

### 3.2 为什么要拆成 native statsd 与 Java companion

statsd 需要高效接收大量事件并保持独立生命期；但 AlarmManager、PackageManager、UserManager、广播和 PendingIntent 属于 Framework Java 世界。companion 负责替 native 进程调用这些系统能力。

---

## 4. Atom：稳定字段定义的“最小事实”

### 4.1 `atoms.proto` 的 master oneof

Android 11 的注释说明，`Atom` 是所有原始 stats log 事件的 master message：

```proto
message Atom {
    oneof pushed {
        SensorStateChanged sensor_state_changed = 5;
        WakelockStateChanged wakelock_state_changed = 10;
        BatteryLevelChanged battery_level_changed = 30;
        // ...
    }

    oneof pulled {
        // 周期查询的快照类 Atom
    }
}
```

字段编号就是 atom id。它必须稳定：配置和解析端都用这个编号识别事件。

源码注释还提醒：`Atom` message 本身并不是每次完整序列化后写入；构建期间的 API 生成器根据协议产生常量和重载方法，运行时 statsd 再按照定义解析合成结构。

### 4.2 Atom 不是 Metric

例如 `SCREEN_STATE_CHANGED(state=OFF)` 只陈述一个状态变化。配置可以用同一个 Atom 构造不同指标：

- 关屏发生次数；
- 每次亮屏持续时间；
- 按用户或设备状态切片的亮屏时长；
- 只在省电模式成立时统计的亮屏事件。

因此：

```text
Atom = 输入事实
Matcher/Predicate = 选择和条件
Metric = 统计方法
Report = 输出结果
```

### 4.3 注解为什么重要

Atom 字段还可携带语义注解，例如 UID 字段、状态字段、primary field、truncate timestamp、module、log mode 等。这些注解会影响：

- 生成哪些调用 API；
- statsd 怎样识别 UID 并做映射；
- 状态跟踪如何取 key；
- 隐私字段如何处理；
- 哪个模块拥有该 Atom。

不能只看 Java `write(int, ...)` 的参数类型，还要回到 proto 看字段语义。

---

## 5. Push Atom：事件发生时立即写入

### 5.1 Java 调用链

常见调用点使用构建系统生成的 `FrameworkStatsLog` 或模块专用 StatsLog 类：

```java
FrameworkStatsLog.write(
        FrameworkStatsLog.CHARGING_STATE_CHANGED,
        status);
```

概念链路是：

```text
业务 Java 代码
  → 生成的 FrameworkStatsLog.write(...)
  → StatsEvent 编码
  → android.util.StatsLog.write()
  → JNI / libstats
  → /dev/socket/statsdw
```

`StatsLog.write(StatsEvent)` 最终调用 native `writeImpl()`；写完后会 `release()` 这个 `StatsEvent`，调用者不能继续使用它。

### 5.2 为什么 push 走 socket，不为每条事件走 Binder

事件写入要求低开销、异步、单向。Android 11 的 libstats 连接 Unix datagram socket：

```c
strcpy(un.sun_path, "/dev/socket/statsdw");
connect(sock, ...);
```

相比每条事件构造同步 Binder 请求，datagram 更适合高频 fire-and-forget 写入。但它不是可靠消息队列：缓冲区拥塞、statsd 未启动或事件队列溢出时都可能丢数据。

### 5.3 调用者身份不是只信 payload

`StatsSocketListener` 对 socket 开启 `SO_PASSCRED`，通过 `SCM_CREDENTIALS` 取得发送进程的真实 UID/PID：

```text
payload 中字段：Atom 自己携带的业务 UID（若有）
socket 凭据：真正写消息的进程 UID/PID
```

二者不是同一个概念。系统服务可能替应用上报一个带目标 UID 的 Atom；statsd 同时知道是哪个进程发来的。允许哪些 log source 写哪些 Atom仍受配置和安全规则约束。

### 5.4 接收后不是直接落盘

`StatsSocketListener`：

1. `recvmsg()` 接收 datagram 和凭据；
2. 跳过 stats event tag；
3. 构造 `LogEvent(uid, pid)`；
4. `parseBuffer()` 解析字段；
5. 推入有限的 `LogEventQueue`。

`main.cpp` 中队列上限为 2000。处理线程随后消费事件。如果队列满，源码记录 overflow；这再次说明 statsd 指标需要面对数据丢失和 guardrail，而不是假设原始事件百分之百可靠。

---

## 6. Pull Atom：statsd 到时间后主动问

### 6.1 为什么要 pull

有些数据本来就是累计快照，例如：

- 当前内存、进程或磁盘统计；
- 某服务维护的累计计数；
- 读取成本较高、不适合每次变化都 push 的状态；
- 需要固定时间桶边界采样的 gauge/value。

如果让数据源自行决定上报时刻，各设备采样周期不一致；statsd 主动 pull 可以让配置控制节奏。

### 6.2 pull 基本链路

```text
StatsdConfig 中某 Gauge/Value Metric 需要 pulled atom
        │
        ▼
StatsPullerManager 计算下一次拉取时间
        │
        ├─ 请求 StatsCompanionService 设置 AlarmManager alarm
        │
        ▼
alarm 到期，companion 通知 statsd
        │
        ▼
内置 Puller 或注册的 IPullAtomCallback
        │
        ▼
返回一组 StatsEvent → statsd 更新当前时间桶
```

### 6.3 `StatsManager.setPullAtomCallback()` 的角色

Android 11 已支持有权限的系统组件注册 pull callback。`StatsManagerService` 保存：

- 调用 UID + atomTag；
- cooldown；
- timeout；
- additive fields；
- `IPullAtomCallback`。

它总是先缓存注册信息；即使 statsd 正好死亡，等 statsd 恢复后也能重新注册。这是 system_server 代理层的重要价值。

默认常量为：

```java
DEFAULT_COOL_DOWN_MILLIS = 1_000L;
DEFAULT_TIMEOUT_MILLIS = 2_000L;
```

它们是 API 默认值，不等于所有 pull 必然每秒执行，也不表示 statsd 会无限等待。真正周期还取决于配置 bucket、puller 类型和 guardrail。

### 6.4 cooldown、timeout 与 additive field

- cooldown：短时间内多个配置请求同一 Atom 时，可复用结果，避免重复昂贵采集；
- timeout：回调太慢就跳过，避免拖死 statsd；
- additive field：某些累计字段适合对多个返回行或差值做加法语义处理。

回调返回 `PULL_SUCCESS` 或 `PULL_SKIP`。失败不是返回一组全零数据；跳过与真实零值在统计语义上不同。

---

## 7. `StatsdConfig`：把原始事实编排成指标

一份配置不是“列出几个 Atom ID”这么简单，它是一张由 ID 引用连接起来的图。

```text
AtomMatcher ──选择 what────────┐
                               ▼
Predicate ──控制 condition──> Metric ──bucket/dimensions/state──> Report
                               │
Activation / State / Alert ────┘
```

### 7.1 Matcher：什么事件算命中

`SimpleAtomMatcher` 至少指定 `atom_id`，还可用 `FieldValueMatcher` 限制字段：

```proto
message SimpleAtomMatcher {
  optional int32 atom_id = 1;
  repeated FieldValueMatcher field_value_matcher = 2;
}
```

比如同一个 WakeLock Atom，可以只匹配 `state=ACQUIRE`、特定类型或某字段范围。多个 matcher 还能通过 AND/OR/NOT 组合。

### 7.2 Predicate：某条件现在是否成立

`SimplePredicate` 用 start/stop matcher 建立状态区间：

```text
start 命中 ───────────── stop 命中
          Predicate=true
```

`count_nesting=true` 时会维护嵌套；这与上一章 WakeLock Timer 的嵌套思想相似。Predicate 可以作为 Metric 的 condition，例如“只统计设备正在充电时发生的事件”。

### 7.3 Metric：怎样计算

Android 11 主要有：

| Metric | 回答的问题 | 典型输入 |
|---|---|---|
| `EventMetric` | 哪些事件发生过 | push 事件明细 |
| `CountMetric` | 命中多少次 | push 事件 |
| `DurationMetric` | 某状态持续多久 | start/stop 或 predicate |
| `ValueMetric` | 数值的差、和、最值等 | 常见于累计 pulled 值 |
| `GaugeMetric` | 某时刻的样本是什么 | pulled 或触发采样 |

不要把 Value 和 Gauge 混为一谈：Gauge 偏向保留样本；Value 偏向对数值进行时间桶内聚合，尤其常用于累计量的差值。

### 7.4 ConfigKey 不是只有 config id

native 端使用：

```text
ConfigKey = (配置拥有者 UID, config id)
```

所以两个不同 UID 可以使用相同的 long id 而不冲突。读取和删除时也必须带正确调用 UID，防止跨调用者窃取或删除统计配置。

---

## 8. Dimensions：同一指标按谁拆账

### 8.1 不切维度会发生什么

若配置只统计 `WAKELOCK_STATE_CHANGED` 的次数，不指定维度：

```text
所有 UID + 所有 tag + 所有类型 → 一个总数
```

指定 UID 和 tag 作为 `dimensions_in_what` 后：

```text
(uid=10001, tag=A) → 27
(uid=10001, tag=B) → 4
(uid=10086, tag=C) → 11
```

### 8.2 FieldMatcher 是字段路径

维度不只可以选顶层字段，还可以通过 child 选择嵌套字段；repeated 字段还有 FIRST/LAST/ANY/ALL 等位置语义。

### 8.3 维度爆炸

如果把高基数字符串、时间戳或几乎唯一的 ID 当维度，每个事件都可能建立一个新切片：

```text
指标内存 ≈ 时间桶数 × 维度组合数 × 状态组合数
```

statsd 具有 guardrail，会限制配置、bucket、维度和内存；超过限制可能丢弃数据或让配置无效。设计配置时应问：这个维度是否真的需要单独分析？

---

## 9. Bucket：为什么结果按时间分段

Count、Duration、Value 等指标通常按固定窗口聚合：5 分钟、10 分钟、1 小时、1 天等。

```text
10:00        10:05        10:10
  | bucket 1   | bucket 2   |
       8 次          3 次
```

桶的价值：

- 控制内存；
- 保留趋势而非所有原始事件；
- 便于比较不同时间段；
- 为告警提供窗口。

`ONE_MINUTE` 在 proto 中有注释：除 shell/root 等情况外会受 guardrail 提升到至少 5 分钟。因此“配置写了 1 分钟”不一定表示普通生产配置真的按一分钟执行。

### 9.1 当前未完成桶

取报告时要区分已完成 bucket 和 current partial bucket。源码在不同 API/选项下可能不包含当前桶，或在 dump 时 flush partial bucket。做实验时若刚写完事件立即读取却看不到结果，先检查这一点。

---

## 10. `StatsLogProcessor`：一个事件如何穿过配置图

`StatsSocketListener` 只负责接收，真正处理发生在 `StatsLogProcessor::OnLogEvent()` 附近。

概念步骤：

1. 校验/修正事件时间；
2. 处理特殊系统 Atom，例如 isolated UID 映射变化；
3. 更新全局 StateManager；
4. 遍历对该 Atom 感兴趣的配置；
5. `MetricsManager` 更新 matcher/predicate/state；
6. 命中的 `MetricProducer` 更新对应维度和 bucket；
7. 检查 anomaly、activation、内存和持久化条件。

```text
LogEvent(atomId=10)
  │
  ├─ Config A：matcher 命中 → DurationMetric 更新 UID 10001
  ├─ Config B：字段条件不符 → 忽略
  └─ Config C：matcher 命中，但 predicate=false → 不计入
```

所以“Atom 确实写进 statsd”不等于“某配置必然产生数据”。还要检查来源白名单、matcher、condition、activation、时间戳、维度与 guardrail。

---

## 11. UID、包名和版本为什么需要单独的 `UidMap`

### 11.1 UID 会复用，包会升级

只保存数字 UID 会产生历史歧义：

- 应用卸载后 UID 可能被另一个包复用；
- 同一包升级后版本发生变化；
- 多用户中的应用 UID 不同；
- isolated UID 生命周期短，需要映射到宿主 UID。

`UidMap` 保存 UID 与 package、version、installer 等关系，并记录更新时间。报告可以附带相关映射，使离线分析知道事件发生时 UID 代表谁。

### 11.2 全量同步

statsd 准备好后，`StatsCompanionService.informAllUids()`：

1. 创建 pipe；
2. 遍历所有用户/profile；
3. 通过 PackageManager 读取已安装和部分已卸载包信息；
4. 写成 `UidData` proto；
5. 通过 fd 让 statsd 读取。

使用 pipe 避免把巨大的包列表塞进一次 Binder byte array。

### 11.3 增量更新

companion 注册 package added/replaced/removed 和 user 变化广播，将单包变化通知 statsd。`StatsLogProcessor` 还专门处理 isolated UID created/removed Atom，让临时 UID 的事件归到宿主。

这和 BatteryStats 的 `mapUid()` 思路相通，但 statsd 还要保存包版本历史，以服务长期报告解释。

---

## 12. `StatsCompanionService` 到底“陪伴”什么

### 12.1 Alarm 代理

native statsd 不能直接像 Java 系统服务那样使用 AlarmManager。companion 提供：

- anomaly alarm；
- pulling alarm；
- periodic/subscriber alarm。

它用 `ELAPSED_REALTIME` 和 exact alarm，在触发后通过 Binder 通知 statsd。这里使用 elapsed realtime，是为了不受用户修改墙上时间影响。

### 12.2 包与用户信息代理

它访问 PackageManager/UserManager，为 `UidMap` 提供完整和增量数据。

### 12.3 生命周期握手

statsd 启动后调用 `sayHiToStatsCompanion()`，companion 的 `statsdReady()` 再完成：

- 获取并保存新的 `IStatsd`；
- 建立 Binder death recipient；
- 通知 `StatsManagerService` statsd 已恢复；
- 注册所需广播；
- 回放 pull callbacks 和 PendingIntent 注册；
- 通知 boot completed（若系统已进入该阶段）；
- 全量发送 UID map。

这是双向握手，不是 system_server 假设 native 进程永不死亡。

### 12.4 死亡恢复与坏配置防护

companion 监听 statsd Binder 死亡，清理旧 receiver 和引用。源码还记录一定时间窗口内的死亡次数；若频繁达到阈值，会处理 `/data/misc/stats-service` 中可能导致反复崩溃的配置。

这是一种可用性保护：错误配置不应让 statsd 永久 crash loop。它也提醒我们，配置是会影响 native 引擎行为的受控输入，而非普通文本。

---

## 13. `StatsManagerService`：权限边界和重连缓存

### 13.1 为什么 App 不直接拿 `IStatsd`

`StatsManager` 的公共/System API 先进入 system_server 的 `StatsManagerService`。它负责：

- 校验 `DUMP`、`PACKAGE_USAGE_STATS`、AppOps 或专用 pull 权限；
- 使用真实 `Binder.getCallingUid()` 形成 ConfigKey；
- 清除/恢复 Binder identity 后调用 native statsd；
- 缓存 PendingIntent 和 pull callback；
- statsd 重启后重新注册。

普通第三方应用并不能随意安装配置并读取全系统遥测数据。

### 13.2 配置所有权

客户端调用：

```java
statsManager.addConfig(configKey, configBytes);
```

Java 层把 package name 用于权限/AppOps 校验，native 调用还带 calling UID。真正唯一键是 `(callingUid, configKey)`。

### 13.3 报告通常是 proto bytes

客户端通过 `getReports(configKey)` 一类接口取得序列化报告。它不是面向人直接阅读的字符串，需要相匹配的 proto schema 解析。

读取报告还可能具有“取走/清除已报告数据”的语义。命令行 `dump-report` 提供 `--keep_data`，明确说明不希望 dump 后擦除时必须指定。做重复实验时一定关注是否消费了数据。

---

## 14. 五类 Metric 用生活化例子理解

假设系统产生门禁事件：进入、离开、当前人数、电表累计读数。

### 14.1 EventMetric：保留关键事件

```text
10:01 张三进入
10:03 李四进入
```

适合需要事件字段本身的低频数据，不宜无节制记录高频明细。

### 14.2 CountMetric：数了多少次

```text
10:00~10:05 进入 8 次
```

只关心次数，不保存每一条完整明细。

### 14.3 DurationMetric：状态持续多久

```text
张三进入(start) ───── 张三离开(stop)
                    12 分钟
```

需要正确匹配 start/stop 和维度 key；丢失 stop、嵌套或跨桶都会让实现更复杂。

### 14.4 GaugeMetric：定时拍照

```text
10:00 当前人数 3
10:05 当前人数 7
```

保留某时点样本，不必做前后差值。

### 14.5 ValueMetric：累计表做差或聚合数值

```text
10:00 电表 1000
10:05 电表 1035
该桶增量 35
```

如果累计源重启回零、pull 失败或 bucket 边界缺样本，实现需要按规则处理；不能简单认为任何相邻值都可直接相减。

---

## 15. State、Condition 和 Dimension 的区别

这三个词最容易混淆。

### 15.1 Condition：要不要计

```text
只在 charging=true 时统计网络事件
```

condition 为 false 时，metric 不累计。

### 15.2 Dimension：分别记到哪本子账

```text
按 uid 分开统计网络事件
```

UID 10001 与 10002 形成不同切片。

### 15.3 State：同一本子账再按系统状态切片

```text
同一 UID 的 CPU 时间，按前台/后台状态拆分
```

可以简化成：

```text
Condition = 开关
Dimension = 主键/分组
State     = 随时间变化的分类标签
```

它们还需要通过 link 指明 metric 输入字段和 condition/state 中哪个字段对应。例如一边的 UID 必须与另一边的 UID 关联，否则可能把 A 应用的状态误用于 B 应用。

---

## 16. 持久化：配置、数据和元数据不是一个目录

Android 11 中可以看到多个目录：

| 目录 | 主要内容 |
|---|---|
| `/data/misc/stats-service` | 配置文件 |
| `/data/misc/stats-data` | 刷盘的报告数据 |
| `/data/misc/stats-active-metric` | 活跃 metric/activation 状态 |
| `/data/misc/stats-metadata` | 配置与指标相关元数据 |

`StorageManager` 负责命名、读取、删除和 trim。数据文件名中包含时间、UID、config id 等信息。

### 16.1 为什么聚合结果也要刷盘

- statsd 有独立崩溃/重启风险；
- 设备重启前要保留重要数据；
- 内存需要 guardrail；
- 报告可能长时间后才被客户端读取。

### 16.2 持久化不等于永不丢失

目录容量有限，会 trim；配置可能删除或 TTL 到期；取报告可能消费数据；坏配置防护也可能移除文件。因此设计者必须接受遥测系统的有界存储语义。

---

## 17. Guardrail：统计系统也必须防止自己拖垮设备

statsd 面临一个悖论：想观察系统，但观察本身不能消耗过多 CPU、内存、磁盘和电量。

常见保护包括：

- socket 与内部队列有上限；
- 记录 dropped event/queue overflow；
- pull 有 cooldown 和 timeout；
- 时间桶有最小粒度限制；
- 维度切片数量受控；
- config 数量、metric 数量和内存受控；
- 数据目录按容量裁剪；
- 高频 Atom 可截断时间戳或限制来源；
- statsd 频繁崩溃时隔离可疑配置。

这意味着“没有数据”可能是业务没发生，也可能是配置没匹配、pull 跳过、事件丢失、guardrail 丢弃或报告已被消费。排查必须分层。

---

## 18. 一条完整示例：统计每个 UID 的 WakeLock 时长

这不是可直接下发的完整 proto，只用于理解关系。

```text
matcher 101：WAKELOCK_STATE_CHANGED 且 state=ACQUIRE
matcher 102：WAKELOCK_STATE_CHANGED 且 state=RELEASE

predicate 201：
  start = 101
  stop  = 102
  dimensions = uid + wakelock tag

duration_metric 301：
  what = predicate 201
  bucket = ONE_HOUR
  dimensions = uid + wakelock tag
```

运行时：

```text
PowerManager/BatteryStats 相关代码
  → FrameworkStatsLog.write(WAKELOCK_STATE_CHANGED, uid, tag, ACQUIRE)
  → statsdw socket
  → LogEvent(uid-of-writer, atom fields...)
  → matcher 101 命中
  → (uid=10086, tag=Location) 的 predicate 进入 true
  → 一段时间后 RELEASE 命中 matcher 102
  → DurationMetric 结算时长并放入当前一小时 bucket
  → getReports/dump-report 输出聚合 proto
```

### 18.1 和 BatteryStats 有何不同

同一个 WakeLock 可能同时进入：

- BatteryStats：专门用于耗电记账、History、UID WakeLock Timer；
- statsd：通用 Atom，只有存在相关 StatsdConfig 才形成对应 metric；
- logcat：若模块另有调试 Log，供人排查文本。

它们不是互相覆盖的重复实现，而是面向不同消费者的观察通道。

---

## 19. statsd 启动与恢复链路

`main.cpp` 的顺序很清楚：

1. 准备 native Looper；
2. 启动 NDK Binder 线程池，最大线程数设置为 9；
3. 建立上限 2000 的 `LogEventQueue`；
4. 创建 `StatsService`，以服务名 `stats` 注册；
5. 注册信号处理；
6. 向 companion 打招呼；
7. `Startup()` 加载配置/元数据并启动处理；
8. 启动 `StatsSocketListener`；
9. Looper 持续处理报告和 alarm 任务。

注意源码末尾旧注释提到 Binder pool one thread，但实际本版本代码调用 `ABinderProcess_setThreadPoolMaxThreadCount(9)`。阅读时应以执行代码为准，并意识到注释可能没有同步更新。

statsd 重启后不是自动回到完全相同内存状态：

- 从磁盘恢复可持久化配置/活跃状态/元数据；
- companion 重新发送 UID map；
- StatsManagerService 回放 pull callback 和 PendingIntent；
- 中断期间未送达的 push 事件通常无法补回。

---

## 20. 安全与隐私边界

系统遥测可能包含包、UID、使用行为、设备状态甚至敏感字段，因此不能成为普通应用的任意查询数据库。

### 20.1 写入侧

- Atom API 往往是隐藏/生成 API；
- socket 附带真实 UID/PID；
- config 可限制 `allowed_log_source`；
- 某些字段使用特殊 log mode 或隐私处理；
- 不能仅靠 payload 伪装可信调用者。

### 20.2 配置和读取侧

- `StatsManager` API 需要 `DUMP` 与 `PACKAGE_USAGE_STATS` 等权限；
- AppOps 还会核对 package 与 UID；
- ConfigKey 带拥有者 UID；
- pull callback 注册需要专用权限；
- PendingIntent 由 system_server 包装，防止 native 端任意伪造调用身份。

### 20.3 “结构化”不代表“可以随便采集”

新增 Atom 时仍要考虑最小化、字段稳定性、基数、敏感性、保留周期和访问者。技术上能加一个字段，不代表隐私和兼容性上应该加。

---

## 21. 只读源码学习路线

### 第一轮：只追 push 数据面

```bash
rg -n "statsdw|SO_PASSCRED|parseBuffer|LogEventQueue" \
  system/core/libstats \
  frameworks/base/cmds/statsd/src/socket \
  frameworks/base/cmds/statsd/src/main.cpp
```

画出：生成 API → JNI/libstats → socket → listener → queue → processor。

### 第二轮：只读配置 schema

```bash
rg -n "message (AtomMatcher|Predicate|CountMetric|DurationMetric|ValueMetric|GaugeMetric|StatsdConfig)" \
  frameworks/base/cmds/statsd/src/statsd_config.proto
```

不要马上看全部字段，先写出 ID 引用关系。

### 第三轮：只追 pull

```bash
rg -n "registerPullAtomCallback|setPullingAlarm|informPollAlarmFired|Puller" \
  frameworks/base/apex/statsd \
  frameworks/base/cmds/statsd/src/external
```

区分谁决定时间、谁设置 Alarm、谁真正生产样本。

### 第四轮：只追重启恢复

```bash
rg -n "statsdReady|linkToDeath|LoadActiveConfigsFromDisk|SaveActiveConfigsToDisk|informAllUids" \
  frameworks/base/apex/statsd \
  frameworks/base/cmds/statsd/src
```

列出“磁盘恢复”和“system_server 回放”各自负责什么。

---

## 22. 常见误区纠正

### 误区 1：调用 `StatsLog.write()` 后，事件一定永久保存

不对。socket、队列和存储都有上限；没有配置感兴趣时也不会形成目标 metric。

### 误区 2：Atom 就是最终报表中的一行

不对。Atom 是输入；配置可能把许多 Atom 聚合成一个 bucket，也可能完全忽略它。

### 误区 3：push 比 pull 更高级或更准确

不对。二者适合不同数据形态。状态变化适合 push，昂贵累计快照适合 pull。

### 误区 4：StatsCompanionService 执行核心指标计算

不对。它主要提供 Java Framework 能力；核心 matcher/metric/report 引擎在 native statsd。

### 误区 5：Dimension 和 Condition 都是过滤

不对。Condition 决定计不计，Dimension 决定计入哪个分组。

### 误区 6：读取一次报告不会改变后续结果

不一定。部分读取/dump 操作会消费数据；需要重复读取时检查 keep-data 语义。

### 误区 7：UID 可以永久唯一标识一个包

不对。UID 会复用，包会升级，还有多用户和 isolated UID；需要 UidMap 的时序信息。

### 误区 8：没有指标就是业务事件没有发生

不对。还可能是来源不允许、matcher 不匹配、condition 为 false、配置未激活、pull 超时、队列溢出、guardrail 或报告已消费。

---

## 23. 排查“为什么没有 statsd 数据”的七层法

```text
① 生产层：业务代码真的调用 write 了吗？
② 传输层：statsdw 是否可用，是否发生 drop/overflow？
③ 解析层：Atom id、字段类型和时间戳是否合法？
④ 配置层：配置属于正确 UID，且成功加载了吗？
⑤ 匹配层：matcher、condition、activation、link 是否成立？
⑥ 聚合层：维度是否爆炸，bucket 是否尚未结束，pull 是否成功？
⑦ 导出层：是否取错 ConfigKey，数据是否已被读取/清除或裁剪？
```

这比一上来怀疑 `StatsLog.write()` 更高效，因为数据链跨越了写端、socket、native 引擎、配置图和报告五个系统。

---

## 24. 练习题

### 题 1

同一个 `BatteryLevelChanged` Atom 已成功写入 statsd，为什么 Config A 有结果而 Config B 没有？至少列出四种可能。

### 题 2

为什么 statsd 接收 socket 时还要读取 `SCM_CREDENTIALS`，不能只相信 Atom 内的 UID 字段？

### 题 3

一个 pulled Atom 的回调超时，为什么应返回/处理为 skip，而不是伪造值 0？

### 题 4

解释 `(uid, configId)` 为何比只用 `configId` 更安全。

### 题 5

按随机 requestId 做 dimension 会出现什么问题？

### 题 6

statsd 崩溃重启后，哪些信息可以恢复，哪些 push 事件通常无法恢复？

### 参考答案

1. B 的 matcher 字段不符、condition 为 false、activation 未激活、来源不允许、bucket 未完成、guardrail 丢弃或数据已消费等。
2. payload 是业务数据，发送者可能填写；内核提供的 socket 凭据用于识别真实写入进程和执行安全判断。
3. 0 是有效业务样本，会错误改变聚合；skip 表示本次没有可信样本。
4. 不同调用者可用相同 id；拥有者 UID 防止一方读取、覆盖或删除另一方配置。
5. 形成近乎每事件一个切片的高基数，导致内存膨胀并触发 guardrail。
6. 磁盘配置、部分活跃状态/元数据和已刷盘报告可恢复，system_server 可回放 callback/PendingIntent/UID map；死亡期间丢失的 datagram push 通常不可补回。

---

## 25. 复读后最容易卡住的地方

写完后按第一次读 statsd 的视角复读，下面五点最容易混乱，换一种比喻再解释。

### 25.1 Atom 与 Metric

Atom 像超市每次扫码产生的小票事实；Metric 像“按小时统计某类商品销量”。没有统计规则，小票不会自动变成你想要的报表。

### 25.2 Push 与 Pull

Push 是门铃：事情发生时通知你；Pull 是抄表：到固定时间主动读取累计值。门铃适合边沿事件，抄表适合当前快照。

### 25.3 Matcher、Predicate 与 Metric

- Matcher 像识别某类门铃；
- Predicate 像根据进入/离开维持“房间有人”的状态；
- Metric 像统计进门次数或房间有人总时长。

### 25.4 StatsManagerService 与 StatsCompanionService

两者都在 system_server，但前者面向客户端 API、权限与重连缓存；后者面向 native statsd，代办 Alarm、包/用户信息和生命周期协作。源码中它们会互相引用，但职责不同。

### 25.5 statsd 和 BatteryStats

BatteryStats 是带电池时间基准和功耗模型的专科账本；statsd 是配置驱动的通用统计平台。某个系统事件可以同时喂给二者，但最终问题不同：前者问“耗多少电、归谁”，后者问“按配置应怎样形成指标”。

---

## 26. 本章总结

把完整链路压缩为：

```text
atoms.proto 定义稳定结构
  → 构建生成各模块 StatsLog API
  → push 事件经 libstats/statsdw 到 statsd
  → pull 数据由 statsd 调度、companion 设 Alarm、系统回调生产
  → StatsLogProcessor 按 StatsdConfig 的 matcher/predicate/state 路由
  → MetricProducer 按 dimensions 和 bucket 计算
  → UidMap 补充包/版本时序归属
  → 结果受 guardrail 控制并按需刷入 stats-data
  → StatsManager/getReports 或 dumpsys/cmd 导出 proto
```

学完本章，应能明确回答：

1. Atom 为什么不是日志字符串；
2. push 和 pull 分别适合什么；
3. Matcher、Predicate、Condition、State、Dimension、Metric 如何组成配置图；
4. 为什么核心统计在 native statsd，而 system_server 仍需两个 Java 服务；
5. 为什么数据可能丢失、被聚合、被消费或被 guardrail 裁剪；
6. statsd 与 BatteryStats、logcat 的职责边界。

下一章将学习 `UsageStatsService、AppStandbyController 与应用使用/待机分桶链路`，继续理解系统如何依据前后台行为形成应用活跃度记录，并把它用于后台资源限制。

