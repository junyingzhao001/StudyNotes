# 71 BatteryService、Health HAL 与电池状态分发链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：理解 Health HAL 如何把充电器、电量、电压、温度、电流和电池健康信息送进 system_server，BatteryService 如何派生插电类型、低电状态、广播与关机动作，BatteryManager 如何查询属性，并分清 BatteryService 的当前状态与 BatteryStats 的历史耗电记账。

---

## 1. 先建立三条链

```text
【当前状态链】
电池/充电 IC → kernel/healthd/vendor Health HAL
→ HealthInfo callback → BatteryService
→ 当前电量、插电、温度、健康状态

【系统分发链】
BatteryService.processValuesLocked
→ sticky BATTERY_CHANGED
→ POWER_CONNECTED/DISCONNECTED
→ BATTERY_LOW/OKAY
→ SystemUI、PMS、JobScheduler、App

【历史记账链】
BatteryService → BatteryStatsService.setBatteryState
→ BatteryStatsImpl
→ 充放电周期、UID 耗电、历史事件、batterystats
```

一句话：

> **BatteryService 是当前电池事实与状态变化的中心；BatteryStatsService 把事实放进时间轴，结合 UID 活动估算“谁耗了多少电”。**

---

## 2. 源码地图

```text
frameworks/base/services/core/java/com/android/server/
    BatteryService.java

frameworks/base/services/core/java/com/android/server/am/
    BatteryStatsService.java

frameworks/base/core/java/android/os/
    BatteryManager.java
    IBatteryPropertiesRegistrar.aidl
    BatteryManagerInternal.java

hardware/interfaces/health/
    2.0/
    2.1/

system/core/healthd/
    healthd.cpp
    BatteryMonitor.cpp
```

Health 2.1 信息会兼容嵌套旧版 `HealthInfo`；BatteryService 同时保存 legacy 与 2.1 扩展字段。

---

## 3. HealthInfo 中有什么

常见字段：

```text
chargerAcOnline / chargerUsbOnline / chargerWirelessOnline
maxChargingCurrent / maxChargingVoltage
batteryStatus / batteryHealth / batteryPresent
batteryLevel
batteryVoltage
batteryTemperature
batteryCurrent / batteryCurrentAverage
batteryChargeCounter / batteryFullCharge
batteryCycleCount / batteryTechnology
batteryCapacityLevel
batteryChargeTimeToFullNowSeconds
```

单位不要猜：

- `batteryLevel` 是 0..100 百分比；
- 广播 temperature 通常以 0.1°C 表示，例如 320 约为 32.0°C；
- voltage 通常 mV；
- charge counter 常为 µAh；
- current 的正负方向和单位应按 Health HAL 契约/设备实现核实。

App 不应把所有数值都假设为有效；HAL 版本、硬件能力和 unsupported sentinel 会影响字段。

---

## 4. BatteryService 启动

`onStart()`：

```text
registerHealthCallback()
→ publishBinderService("battery", BinderService)
→ publishBinderService("batteryproperties", BatteryPropertiesRegistrar)
→ publishLocalService(BatteryManagerInternal)
```

三个入口用途不同：

```text
"battery" → dumpsys/shell 与服务控制
"batteryproperties" → BatteryManager 属性查询
BatteryManagerInternal → system_server 内部快速访问/回调
```

启动时 BatteryService 会等待第一份 HealthInfo callback，确保后续系统服务读取时已有基础电池状态，而不是全是未初始化默认值。

---

## 5. HealthServiceWrapper 为什么存在

它负责发现和替换 `IHealth` 实例：

```text
优先 "default" vendor instance
→ 不可用时 "backup" healthd instance
→ 保存 mLastService
→ 向 hwservicemanager 注册 service notification
→ 新实例出现时切换并回调 BatteryService
```

实例优先级：

```java
["default", "backup"]
```

### 5.1 新 HAL 注册时

`HealthHalCallback.onRegistration(old,new,instance)`：

```text
旧服务 unregisterCallback
→ 新服务 registerCallback
→ 主动 newService.update()
```

`registerCallback()` 不保证立即回送状态，所以必须显式 `update()`，否则切换后可能长时间保留旧缓存。

### 5.2 为什么死亡时不立刻清空引用

Wrapper 注释说明：为避免 death notification 与新 service notification 的竞态，不立即清空最后服务引用。调用者必须处理 `RemoteException`。新实例通知在专用 `HealthServiceHwbinder` HandlerThread 串行切换。

---

## 6. HAL 回调主链

```text
IHealth.update / 电池属性变化
→ IHealthInfoCallback.healthInfoChanged_2_1(info)
→ BatteryService.update(info)
→ Trace 电量计数/电流
→ synchronized(mLock)
→ mHealthInfo / mHealthInfo2p1 替换
→ processValuesLocked(false)
→ notifyAll（唤醒等待首次 info 的线程）
```

若是 Health 2.0 callback，BatteryService 会构造 2.1 容器，并把 capacity level、time-to-full 标为 unsupported 后走同一处理函数。

---

## 7. processValuesLocked 第一步：派生状态

### 7.1 critical level

```text
status != UNKNOWN && level <= config_criticalBatteryWarningLevel
→ mBatteryLevelCritical=true
```

### 7.2 plug type

按优先级：

```text
AC online       → BATTERY_PLUGGED_AC
否则 USB        → BATTERY_PLUGGED_USB
否则 wireless   → BATTERY_PLUGGED_WIRELESS
否则            → NONE
```

这意味着 `plugType` 是 BatteryService 根据三个 online boolean 派生的单值，不是 HAL 直接传入的最终枚举。

### 7.3 先通知 BatteryStats

```text
mBatteryStats.setBatteryState(
  status, health, plugType, level, temp, voltage,
  chargeCounter, fullCharge, chargeTimeToFull)
```

BatteryStats 需要尽早知道 plugged/unplugged 和电量变化，才能划分充放电历史与统计周期。

---

## 8. BatteryService 与 BatteryStatsService 的边界

| BatteryService | BatteryStatsService |
|---|---|
| 保存最新 HealthInfo | 保存长期历史与 UID 活动统计 |
| 判断当前是否插电、低电、过温 | 统计 wakelock、CPU、网络、传感器等耗电关联 |
| 发送电池广播 | 提供 `dumpsys batterystats`、历史、估算 |
| 当前状态安全关机 | 电量周期/放电区间记账 |
| `dumpsys battery` 可模拟状态 | batterystats reset/charged 等统计控制 |

“某 App 为什么耗电高”应从 BatteryStats/PowerStats 找证据；“现在电量是多少、是否充电”看 BatteryService/HealthInfo。

BatteryStats 也不直接测出每个 App 的独立电流，它综合硬件计数、power profile、控制器 activity 和 UID 使用时间估算/归因。

---

## 9. 何时认为状态真正变化

BatteryService 比较当前值与一组 `mLast*`：

```text
status / health / present / level / plugType
voltage / temperature
max charging current/voltage
charge counter / invalid charger
```

有任一变化或 `force=true` 才进入广播、日志、LED 和 last state 更新。

不是 HAL 每次 callback 都向全系统重复广播相同状态，这能减少无意义唤醒和 Binder/广播开销。

---

## 10. ACTION_BATTERY_CHANGED

BatteryService 在 `sendBatteryChangedIntentLocked()` 中构造：

```text
Intent.ACTION_BATTERY_CHANGED
FLAG_RECEIVER_REGISTERED_ONLY
FLAG_RECEIVER_REPLACE_PENDING
```

常见 extras：

```text
sequence
status / health / present
level / scale
battery_low
plugged
voltage / temperature / technology
invalid_charger
max_charging_current / voltage
charge_counter
```

随后调用：

```java
ActivityManager.broadcastStickyIntent(intent, USER_ALL)
```

### 10.1 sticky 的意义

新注册的动态 Receiver 可以立即拿到最近一份电池状态，不必等待下一次电量变化：

```java
Intent current = context.registerReceiver(null,
        new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
```

### 10.2 registered-only 的意义

它主要发给运行时注册的接收者，不按普通隐式广播方式拉起所有 Manifest Receiver。电池值可能频繁变化，不能每次唤醒大量 App。

---

## 11. 插拔电广播

plug type 从 NONE → 非 NONE：

```text
ACTION_POWER_CONNECTED
```

非 NONE → NONE：

```text
ACTION_POWER_DISCONNECTED
```

它们与 sticky BATTERY_CHANGED 分开，因为部分系统/应用只关心电源连接边沿，而不想处理每次 level/temperature 变化。

插拔时还记录：

- 充电起始时间/电量；
- 放电起始时间/电量；
- 充电持续时间；
- 放电周期异常值。

---

## 12. 低电量判断与迟滞

### 12.1 warning level

来自：

```text
Settings.Global.LOW_POWER_MODE_TRIGGER_LEVEL
```

0 时回退资源默认值，且不得低于 critical level。

恢复阈值：

```text
mLowBatteryCloseWarningLevel
= warningLevel + config_lowBatteryCloseWarningBump
```

这形成 hysteresis，避免电量在 warning 边界波动时 LOW/OKAY 来回广播。

### 12.2 BATTERY_LOW 发送条件

对应源码方法是 `shouldSendBatteryLowLocked()`：

```text
当前未插电
且 status != UNKNOWN
且 level <= warning
且（刚拔电，或从 warning 上方跌到边界内）
```

不是每下降 1% 都重发 LOW。

### 12.3 BATTERY_OKAY

之前已经发过 LOW，且 level 回升到 close warning level，发送 `ACTION_BATTERY_OKAY`。插电会让内部 `mBatteryLevelLow=false`，但广播状态还按代码条件与后续更新管理。

---

## 13. 低电量自动关机

判断集中在 `shouldShutdownLocked()`。

Android 11 优先看 Health 2.1：

```text
batteryCapacityLevel == CRITICAL
→ shouldShutdown=true
```

若该字段 unsupported，回退：

```text
level <= 0
且 batteryPresent
且 status != CHARGING
→ shutdown
```

无电池设备不会因为 level 默认 0 就关机。处于充电状态时也不会用旧规则立即关机。

满足后 Handler 启动 `ACTION_REQUEST_SHUTDOWN` Activity，reason=`low_battery`，并等待 system ready，进行有序关机而不是直接杀电源。

---

## 14. 电池过温关机

```text
batteryTemperature > config_shutdownBatteryTemperature
→ ACTION_REQUEST_SHUTDOWN
→ reason=battery_thermal_state
```

Android 11 默认注释示例约为 68.0°C，但真正阈值来自设备资源，不能硬编码。

它和第 70 章 ThermalManagerService 的 battery `THROTTLING_SHUTDOWN` 是两条可能的安全入口：

- BatteryService 直接比较 HealthInfo 电池温度与资源阈值；
- ThermalManagerService 根据 Thermal HAL severity 判断。

设备也应有 PMIC/充电 IC/内核级独立过温保护，Framework 关机不是唯一防线。

---

## 15. 电池状态、健康状态和充电状态

### 15.1 status

```text
UNKNOWN / CHARGING / DISCHARGING / NOT_CHARGING / FULL
```

`plugged != NONE` 不必然等于 `status=CHARGING`：电池过热、充电策略、已满或电源能力不足时可能插着电但 NOT_CHARGING。

### 15.2 health

```text
UNKNOWN / GOOD / OVERHEAT / DEAD
OVER_VOLTAGE / UNSPECIFIED_FAILURE / COLD
```

health 是电池健康/异常分类，不是长期“健康度百分比”。

### 15.3 present

表示设备是否有电池。固定供电电视/车机等可没有电池，因此低电关机逻辑必须检查 present。

---

## 16. BatteryManager 属性查询链

App 可通过：

```java
BatteryManager bm = context.getSystemService(BatteryManager.class);
int capacity = bm.getIntProperty(BATTERY_PROPERTY_CAPACITY);
long charge = bm.getLongProperty(BATTERY_PROPERTY_CHARGE_COUNTER);
```

链路：

```text
BatteryManager
→ IBatteryPropertiesRegistrar("batteryproperties")
→ BatteryService.BatteryPropertiesRegistrar
→ HealthServiceWrapper.getLastService()
→ IHealth.getCapacity/getChargeCounter/...（按属性）
→ BatteryProperty 返回
```

这是按需查询 HAL 的属性通道，与 ACTION_BATTERY_CHANGED 的缓存/广播通道不同。调用要处理 unsupported/error sentinel，不能把任意负数或 `Long.MIN_VALUE` 当真实电量。

---

## 17. sequence 有什么用

每次有意义的状态批次：

```text
mSequence++
```

同一批 POWER_CONNECTED、BATTERY_LOW、BATTERY_CHANGED 等携带相同/对应 sequence，消费者可以判断事件先后和状态是否属于更新批次。

异步 Handler post 可能让广播到达业务代码时又有新状态，稳妥消费者应把广播 extras 当该 sequence 的快照，必要时重新查询当前状态。

---

## 18. LED、SystemUI 与其他消费者

BatteryService 自身更新电池 LED：

```text
低电未充电 → 低电颜色/闪烁
充电且接近满/已满 → 绿色
充电中 → 中间颜色
其他 → 关闭
```

SystemUI 电池图标、充电动画和低电提示通常接收 battery controller 封装后的广播状态。PMS 可根据 `BatteryManagerInternal.isPowered()` 处理保持唤醒/充电行为；JobScheduler BatteryController 根据充电和 battery-not-low 约束更新任务。

BatteryService 负责事实分发，不负责每一种 UI 的具体样式。

---

## 19. dumpsys battery 模拟模式

Shell 可执行类似：

```text
dumpsys battery set level 15
dumpsys battery set ac 1
dumpsys battery unplug
dumpsys battery reset
```

进入模拟时 `mUpdatesStopped=true`，真实 HAL update 不再覆盖当前模拟值，而是保存到 `mLastHealthInfo`；reset 后恢复真实信息并重新处理。

适合测试：

- SystemUI 图标和低电提示；
- Job/充电约束；
- App Receiver；
- BatteryService 广播逻辑。

但它不会真的改变电池电量、充电 IC、电流或底层 BatteryStats 物理耗电，不能当真实续航测试。

---

## 20. 一次电量从 21% 到 20%

假设 warning=20：

```text
fuel gauge/Health HAL：level 21 → 20
→ HealthInfo callback
→ BatteryService.update
→ processValuesLocked
→ BatteryStats.setBatteryState(level=20)
→ 未插电且从 warning 上方跨入
→ mBatteryLevelLow=true
→ sequence++
→ ACTION_BATTERY_LOW
→ sticky ACTION_BATTERY_CHANGED(level=20, battery_low=true)
→ ACTION_BATTERY_LEVEL_CHANGED 队列
→ LED/SystemUI/JobScheduler 更新
→ last fields=当前值
```

下一次 20→19 不会再次满足“从 warning 上方跨入”，所以 LOW 不应每 1% 重复发送。

---

## 21. 一次插入充电器

```text
charger online 属性改变
→ HAL callback
→ mPlugType: NONE → AC/USB/WIRELESS
→ BatteryStats 结束放电/开始充电记账
→ 记录 discharge duration 与 level
→ ACTION_POWER_CONNECTED
→ 更新 low-battery 内部状态
→ sticky ACTION_BATTERY_CHANGED(plugged=...)
→ LED/SystemUI/PMS/JobScheduler
```

如果插电但 status 暂时 NOT_CHARGING，POWER_CONNECTED 仍可发送，因为它观察 plug edge，不要求已经进入 CHARGING。

---

## 22. 常见误解修正

1. BatteryService 不直接读取 `/sys`；Android 11 主链通过 Health HAL callback。
2. plugType 不是充电状态，插电可能 NOT_CHARGING。
3. battery health 不是容量健康百分比。
4. BATTERY_CHANGED 是 sticky 且 registered-only，不会按普通广播拉起所有 App。
5. BATTERY_LOW 不是每降低 1% 都发送。
6. close warning threshold 提供迟滞，避免 LOW/OKAY 抖动。
7. BatteryService 当前状态不等于 BatteryStats 历史耗电归因。
8. BatteryManager property 查询与 sticky 广播是不同数据通道。
9. level=0 不必然关机，还检查 capacity level、present 和 charging。
10. dumpsys battery 模拟不改变真实硬件电池。

---

## 23. 故障排查

### 23.1 电池图标不更新

```text
Health HAL 是否 callback
→ BatteryService mHealthInfo 是否更新
→ 是否 mUpdatesStopped 模拟未 reset
→ processValues 是否检测到变化
→ BATTERY_CHANGED sequence/extras
→ SystemUI BatteryController/主线程
```

### 23.2 插电但显示未充电

同时检查 charger online、mPlugType 和 batteryStatus。可能线已连接但充电策略因温度、满电保护或功率不足返回 NOT_CHARGING。

### 23.3 低电广播不发送

检查未插电、status 非 UNKNOWN、warning setting、上一次 level 是否在阈值上方、是否已经发过 LOW，以及 force/模拟状态。

### 23.4 BATTERY_OKAY 不发送

需要先发过 LOW，且 level 回升到 close warning level。仅回到 warning 本身可能仍未达到恢复阈值。

### 23.5 开机卡在等待 HealthInfo

检查 IHealth default/backup 实例、manifest、hwservicemanager notification、callback 注册、新服务 `update()` 和 Health HAL 日志。

### 23.6 App property 返回异常值

检查设备是否支持该属性、HAL transaction、BatteryManager 的 unsupported sentinel 和 App 是否错误做了 int/long 单位转换。

### 23.7 异常关机

查看 capacityLevel、level/status/present、batteryTemperature、shutdown threshold、模拟模式，以及 ThermalManagerService 是否也报告 battery SHUTDOWN。

---

## 24. macOS 只读练习

1. 从 `registerHealthCallback()` 追 default/backup IHealth 选择与首次等待。
2. 追新 Health HAL 注册后的 unregister/register/update。
3. 列出 HealthInfo 主要字段、单位与 unsupported 风险。
4. 从 `healthInfoChanged_2_1()` 追到 `processValuesLocked()`。
5. 证明 plugType 是由三个 online boolean 派生。
6. 对比 BatteryService 与 BatteryStatsService 的输入和输出。
7. 追 BATTERY_CHANGED flags、sticky 分发与 extras。
8. 用 warning=20、close=25 模拟 26→20→19→25 的 LOW/OKAY。
9. 追低电和过温两条 shutdown reason。
10. 从 BatteryManager property 追到 IHealth。
11. 阅读 dumpsys battery set/reset，解释 mUpdatesStopped 和真实 HAL update。

---

## 25. 推荐阅读顺序

```text
1. HealthInfo + BatteryManager 常量
2. BatteryService onStart/registerHealthCallback
3. HealthServiceWrapper + HealthHalCallback
4. update/processValuesLocked 前半
5. 广播、低电、关机和 last fields
6. BatteryPropertiesRegistrar
7. BatteryStatsService.setBatteryState
8. shell 模拟与 dump
```

每读一处固定问：

```text
这是 HAL 原始值还是 BatteryService 派生值？
这是当前状态、边沿事件还是历史记账？
单位是什么？unsupported 怎样表达？
这次变化会触发哪个广播，为什么？
```

---

## 26. 完整心智模型

```text
电池/充电器硬件
→ kernel fuel gauge/charger driver
→ vendor IHealth(default) 或 healthd(backup)
→ IHealthInfoCallback
→ BatteryService.update/processValuesLocked
   ├→ 派生 plugType/critical/low
   ├→ BatteryStatsService 历史记账
   ├→ 低电/过温有序关机
   ├→ POWER_CONNECTED/DISCONNECTED
   ├→ BATTERY_LOW/OKAY
   ├→ sticky BATTERY_CHANGED
   ├→ LED/SystemUI/PMS/JobScheduler
   └→ 缓存 last fields

App 按需属性：
BatteryManager → BatteryPropertiesRegistrar → IHealth query
```

总结：**Health HAL 提供硬件事实，BatteryService 将其转换成 Android 当前状态和事件，BatteryStatsService 把状态放进长期耗电时间轴；广播、查询和统计是三种不同用途的数据通道。**

---

## 27. 自测题

1. Health HAL、BatteryService、BatteryStatsService 分别负责什么？
2. HealthServiceWrapper 为什么有 default 和 backup 实例？
3. 新 IHealth 服务注册后为什么主动调用 update？
4. plugType 与 batteryStatus 有何区别？
5. BATTERY_CHANGED 为什么 sticky 且 registered-only？
6. BATTERY_LOW 的边沿条件是什么？
7. close warning level 为什么高于 warning level？
8. level=0 时为什么不一定关机？
9. BatteryService 与 ThermalManagerService 的电池过温入口有何不同？
10. BatteryManager property 查询与广播有什么不同？
11. dumpsys battery 能测试什么、不能测试什么？
12. 为什么 BatteryStats 不是每个 App 的独立电流表？

能画出当前状态、系统分发和历史记账三条链，并完整解释插电与跨入低电阈值，本章就掌握了。

---

## 28. 下一章预告

```text
72 BatteryStatsService、BatteryStatsImpl 与 UID 耗电记账链路
```

下一章将深入本章只点到的历史统计：on-battery 时间基准、HistoryItem、WakeLock/CPU/网络/传感器记账、UID 映射、PowerProfile/PowerCalculator、充电周期重置、checkin/proto 与 Battery Historian 数据来源。
