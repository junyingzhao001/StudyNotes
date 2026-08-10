# 72-BatteryStatsService、BatteryStatsImpl 与 UID 耗电记账链路

> 源码基线：Android 11（`android-11.0.0_r48`）  
> 本章目标：弄清 Android 怎样把“谁做了什么、做了多久”记录下来，并估算成设置页里的 UID/应用耗电。  
> 阅读方式：本章只读源码，不要求在 Mac 上编译 AOSP。

---

## 1. 先说结论：Android 不是给每个 App 接了一只电流表

看到“某应用耗电 12%”，最容易产生的误解是：系统直接测到了这个应用流过多少电流。

实际过程更接近一本分层账簿：

1. Framework 服务上报事件：某 UID 获得 WakeLock、使用相机、启动传感器、发送网络数据；
2. `BatteryStatsImpl` 用计数器和计时器保存活动量，并用时间基准排除插电期间；
3. `BatteryExternalStatsWorker` 定期抓取 CPU、Wi-Fi、蓝牙、Modem、内核 WakeLock 等累计快照，计算两次快照的增量；
4. `PowerCalculator` 使用硬件能量报告或 `PowerProfile` 功耗模型，把活动量换算成 mAh；
5. `BatteryStatsHelper` 汇总为 `BatterySipper`，再形成用户看到的耗电占比。

```text
真实活动
  │
  ├─ Framework 主动 note 事件 ─────────────┐
  ├─ 内核 UID CPU/网络/WakeLock 累计值 ────┤
  └─ Wi-Fi/BT/Modem 控制器活动与能量 ──────┤
                                           ▼
                                  BatteryStatsImpl
                                “记录活动量和时间线”
                                           │
                                           ▼
                             PowerCalculator + PowerProfile
                                “活动量换算为估计 mAh”
                                           │
                                           ▼
                                  BatteryStatsHelper
                              UID/系统组件耗电与百分比
```

所以需要牢牢记住：

> BatteryStats 的核心是“统计与归因”，不是逐应用实时电流测量。

硬件可能报告某个控制器的总能量，但“这部分能量分别属于哪些 UID”仍经常需要按照活动时间、数据包或其他规则分摊。

---

## 2. 本章源码地图

| 文件 | 作用 | 建议重点 |
|---|---|---|
| `frameworks/base/services/core/java/com/android/server/am/BatteryStatsService.java` | `batterystats` Binder 服务入口 | `setBatteryState()`、各种 `note...()` |
| `frameworks/base/core/java/com/android/internal/os/BatteryStatsImpl.java` | 核心账本 | `TimeBase`、UID 统计、History、reset、持久化 |
| `frameworks/base/services/core/java/com/android/server/am/BatteryExternalStatsWorker.java` | 单线程抓取外部累计统计 | `scheduleSync()`、`updateExternalStatsLocked()` |
| `frameworks/base/core/java/android/os/BatteryStats.java` | 对外抽象和统计数据结构 | `Uid`、`Timer`、`Counter`、`HistoryItem` |
| `frameworks/base/core/java/com/android/internal/os/PowerProfile.java` | 读取设备功耗模型 | CPU、屏幕、无线控制器平均电流 |
| `frameworks/base/core/java/com/android/internal/os/*PowerCalculator.java` | 各硬件模块的估算器 | CPU、Wi-Fi、蜂窝、WakeLock、传感器等 |
| `frameworks/base/core/java/com/android/internal/os/BatteryStatsHelper.java` | 汇总耗电项目 | 创建 `BatterySipper`、计算总量和占比 |
| `frameworks/base/core/java/com/android/internal/os/BatterySipper.java` | 一项耗电结果 | UID 或 Screen、Cell、Idle 等系统项 |
| `frameworks/base/core/java/com/android/internal/app/IBatteryStats.aidl` | Binder 接口 | 调用边界与可用操作 |

### 2.1 运行进程与线程

`BatteryStatsService`、`BatteryStatsImpl` 和 `BatteryExternalStatsWorker` 都处在 `system_server` 进程，但不能因此认为所有代码都运行在主线程：

- 系统服务调用 `note...()` 时，可能从 Binder 线程或服务自己的线程进入；
- 修改 `BatteryStatsImpl` 共享数据时通常持有 `mStats` 锁；
- 外部统计采集使用单线程 worker，避免多个快照交叉；
- Wi-Fi、蓝牙和 Telephony 的活动信息还可能经过跨进程 Binder 回调；
- `batterystats.bin` 的异步写入避免长时间阻塞关键调用者。

这正是阅读本链路时必须同时跟踪“进程、线程、锁、时间顺序”的原因。

---

## 3. 三类输入：事件、快照和电池状态

### 3.1 事件型输入：开始与结束

很多活动天然是区间：

```text
noteStartWakelock(uid, ...)
            │
            ├──── UID 持有时间 ────┤
            │                       │
            └────────── noteStopWakelock(uid, ...)
```

类似的输入还有：

- 传感器/GPS 开始、停止；
- 相机、闪光灯、音频、视频开始、停止；
- Wi-Fi scan、Bluetooth scan；
- Job、Sync、Alarm、进程状态等事件；
- 屏幕状态、亮度、设备空闲状态。

事件型输入适合用 `Timer`、嵌套计数和 History 状态位表示。

### 3.2 累计快照型输入：两次读数相减

CPU 或控制器经常提供的是“从某个起点累计到现在”的数值，而不是本次消耗：

```text
第一次读取：CPU UID 10001 累计 1200 ms
第二次读取：CPU UID 10001 累计 1550 ms
本统计段增量：1550 - 1200 = 350 ms
```

Wi-Fi、蓝牙、Modem 的 idle/rx/tx 时间以及网络流量也采用类似思想。系统必须保留上一快照，处理回绕、重启或无效数据，并把增量分配到正确 UID。

### 3.3 电池状态输入：账本的开关和边界

上一章的 `BatteryService` 从 Health HAL 获得插电类型、电量、温度、电压和电荷量，随后调用：

```java
mBatteryStats.setBatteryState(
        status, health, plugType, level, temp, volt,
        chargeUAh, chargeFullUAh, chargeTimeToFullSeconds);
```

它不仅补充 History 中的电池信息，更重要的是决定 on-battery 时间基准是否运行。

---

## 4. `BatteryStatsService`：系统统计账本的门面

### 4.1 为什么需要一个独立 Binder 服务

不同系统组件知道不同事实：PowerManager 知道 WakeLock，SensorService 知道传感器，AudioService 知道音频，NetworkStats 知道 UID 流量。它们需要把事实汇入同一本账。

`BatteryStatsService` 提供统一入口，并负责：

- 校验调用权限，普通应用不能随意伪造别人的活动；
- 把 UID、PID、名称、`WorkSource` 等归因信息交给 `BatteryStatsImpl`；
- 对核心共享状态加锁；
- 将较慢的外部统计采集放到 worker；
- 提供 dump、reset、write 等管理入口。

以 WakeLock 为例，入口结构很直接：

```java
public void noteStartWakelock(int uid, int pid, String name,
        String historyName, int type, boolean unimportantForLogging) {
    enforceCallingPermission();
    synchronized (mStats) {
        mStats.noteStartWakeLocked(uid, pid, null, name, historyName,
                type, unimportantForLogging,
                SystemClock.elapsedRealtime(), SystemClock.uptimeMillis());
    }
}
```

这里没有计算 mAh。它只是在可信边界内记录“某 UID 的某类 WakeLock 开始了”。

### 4.2 `WorkSource`：执行者不一定是责任者

假设应用请求系统定位：

```text
App UID 10086
    │ 请求位置
    ▼
system_server 中的位置服务实际操作 GNSS
```

只看执行线程，能耗似乎属于 system UID；但真正发起需求的是 UID 10086。`WorkSource`/`WorkChain` 就用于传递责任来源。

`BatteryStatsImpl.noteStartWakeFromSourceLocked()` 会遍历 `WorkSource`：

- 普通项使用 `ws.getUid(i)`；
- 链式归因使用 `WorkChain.getAttributionUid()`；
- 再为相应 UID 启动计时器。

这让系统服务“代办”的工作尽可能回到请求 UID。它仍不是万能的：共享 UID、复杂代理链、无法拆分的共享硬件活动都会限制精度。

---

## 5. 最关键的边界：为什么插拔电时必须先同步外部统计

`BatteryStatsService.setBatteryState()` 的核心代码如下：

```java
final boolean onBattery = BatteryStatsImpl.isOnBattery(plugType, status);
if (mStats.isOnBattery() == onBattery) {
    mStats.setBatteryStateLocked(...);
    return;
}

mWorker.scheduleSync("battery-state",
        BatteryExternalStatsWorker.UPDATE_ALL);
mWorker.scheduleRunnable(() -> {
    synchronized (mStats) {
        mStats.setBatteryStateLocked(...);
    }
});
```

注意顺序：

```text
旧状态（例如正在用电池）
       │
       ├─ ① 读取所有外部累计快照
       │      把最后一段增量记入旧状态
       │
       ├─ ② 切换 BatteryStats 的 on-battery 状态
       │
       ▼
新状态（例如已经插电）
```

如果先切换时间基准，再抓 CPU 快照，会发生什么？

```text
上次 CPU 快照 ──[实际发生在电池供电下的 CPU 活动]── 本次快照
                                                   ▲
                                 若这里已改成“插电”，增量可能进错区间
```

worker 是单线程执行器，因此先入队的 sync 会先于后入队的状态修改执行。源码注释明确说明，这个顺序用于避免相关数据以后无法正确收集。

### 5.1 怎样判断 on battery

Android 11 的判断并不是只看 status：

```java
public static boolean isOnBattery(int plugType, int status) {
    return plugType == BATTERY_PLUGGED_NONE
            && status != BatteryManager.BATTERY_STATUS_UNKNOWN;
}
```

即没有 AC/USB/无线充电输入，并且电池状态有效，才认为正在使用电池。

---

## 6. `TimeBase`：不是时钟，而是“有条件运行的累计时间轴”

这是全章最抽象、也最值得吃透的概念。

### 6.1 为什么不能直接用 `elapsedRealtime()` 相减

假设一个统计对象从 10:00 存在到 12:00，但设备 11:00 插上电：

```text
10:00               11:00               12:00
  |------ 电池供电 -----|------ 插电 -------|

墙上经过时间：2 小时
on-battery 有效时间：1 小时
```

耗电账本通常只想累计电池供电期间。每个计时器自己判断插电状态既重复又容易出错，于是大量 `Timer`/`Counter` 作为观察者挂在同一个 `TimeBase` 上。

### 6.2 两条主要时间基准

`BatteryStatsImpl` 维护：

- `mOnBatteryTimeBase`：仅在未插电时运行；
- `mOnBatteryScreenOffTimeBase`：仅在未插电且屏幕关闭时运行。

源码中的开关逻辑：

```java
final boolean screenOff = !isScreenOn(screenState);
mOnBatteryTimeBase.setRunning(unplugged, uptime, realtime);
mOnBatteryScreenOffTimeBase.setRunning(
        unplugged && screenOff, uptime, realtime);
```

屏幕关闭时间基准非常重要，因为内核 WakeLock、CPU suspend 阻止等问题在熄屏待机时更有诊断价值。

此外每个 UID 还可能有后台时间基准，用来回答“此活动发生时 UID 是否在后台”。因此一个计时器是否增长，可能同时依赖设备是否用电池、屏幕状态和 UID 前后台状态。

### 6.3 uptime 与 realtime 不要混为一谈

- `uptimeMillis()`：设备深度睡眠时不增长，适合看 CPU 实际清醒时间；
- `elapsedRealtime()`：包括深度睡眠，适合看从某事件到现在经过多久；
- `TimeBase`：在上述时钟之上再加“只在某条件成立时累计”的门控。

`computeBatteryUptime()` 和 `computeBatteryRealtime()` 分别通过 on-battery `TimeBase` 计算，而不是简单返回系统启动时长。

---

## 7. `Timer`、`Counter` 与嵌套计数

### 7.1 Timer 解决开始—结束区间

一个 UID 可能重复或嵌套获得同名资源：

```text
start A ───────────── stop A
       start A ─ stop A
```

如果第一次 `stop` 就关闭计时，会少记外层区间。因此计时器通常维护 nesting：

- nesting 从 0 变 1：真正开始；
- 1 变 2：只增加嵌套层数；
- 2 变 1：仍在运行；
- 1 变 0：真正停止并结算。

### 7.2 Counter 解决离散累计量

适合计数的量包括：

- 网络 bytes/packets；
- 某事件发生次数；
- 控制器 idle/rx/tx 毫秒数；
- 电荷计数器的放电差值；
- CPU 各频点累计时间。

Counter 同样能够依附 `TimeBase`，在统计基准运行时接收增量。

### 7.3 UID 是主要归因粒度

`BatteryStatsImpl` 内部通过 UID 统计对象保存：

```text
UID
 ├─ CPU user/system/frequency time
 ├─ network bytes/packets
 ├─ wakelock name -> Wakelock timers
 ├─ sensor handle -> Sensor timer
 ├─ process/package/service stats
 ├─ jobs/syncs/alarms
 └─ camera/audio/video/GPS/Wi-Fi/BT timers
```

UID 不总等于包名：

- 同一个应用在不同用户下 UID 不同；
- 多个包可能通过 `sharedUserId` 共用 UID；
- isolated UID 会通过 `mapUid()` 映射回宿主应用 UID；
- 系统共享工作可能只能归到 system、root 或某个系统组件。

所以设置页显示“应用”时，还需要 PackageManager 把 UID 映射为用户可读标签。统计底层首先相信的是 UID，不是包名字符串。

---

## 8. WakeLock 记账：时长不等于全部能耗

`noteStartWakeLocked()` 对 partial WakeLock 做几件事：

1. `mapUid(uid)` 处理 isolated UID；
2. 更新全局 WakeLock 嵌套状态；
3. 在 History 中设置 `STATE_WAKE_LOCK_FLAG`，必要时写入 tag；
4. 为目标 UID 的对应名称启动 Timer；
5. 熄屏用电池时请求更新 CPU 时间；
6. 同时写 `FrameworkStatsLog` 原子事件。

停止时反向执行，嵌套降到 0 才清除全局状态位。

### 8.1 为什么只重点统计 partial WakeLock

源码注释说明，耗电归因主要关心 partial WakeLock；屏幕相关 WakeLock 会在用户关闭屏幕时被取消，屏幕本身又有单独的亮度/显示功耗模型。

### 8.2 WakeLock 的耗电怎么算

Android 11 的 `WakelockPowerCalculator` 使用：

```java
mPowerWakelock = profile.getAveragePower(PowerProfile.POWER_CPU_IDLE);
app.wakeLockPowerMah =
        app.wakeLockTimeMs * mPowerWakelock / (1000 * 60 * 60);
```

公式本质是：

```text
mAh = 时间(ms) × 平均电流(mA) ÷ 3,600,000
```

但不要把它理解为“WakeLock 的完整代价”。WakeLock 的直接模型主要代表阻止 CPU 休眠时的基础清醒成本；WakeLock 期间真正执行的 CPU 指令会在 CPU 项中另算，网络、GPS 等也有各自项目。

因此：

- WakeLock 10 分钟不表示 CPU 满载 10 分钟；
- 没有长 WakeLock 也不表示应用没有通过 Job、Alarm 等产生耗电；
- WakeLock 时间很长但模型电流不高，仍可能是严重待机问题，因为它让其他后台活动有机会运行。

---

## 9. History：压缩的状态时间线，不是每毫秒采样表

### 9.1 History 记录什么

`BatteryStats.HistoryItem` 可承载：

- 电量、温度、电压、充电状态和插电类型；
- 屏幕、WakeLock、GPS、Wi-Fi、移动网络、Doze 等状态位；
- 带 tag/UID 的开始和结束事件；
- 当前墙上时间、reset、shutdown 等命令；
- 外部统计采集发生的时点。

### 9.2 为什么采用 delta 编码

如果每次都完整保存所有字段，历史很快膨胀。`BatteryStatsHistory`/`BatteryStatsImpl` 会比较当前项和上一项，只编码时间差及发生变化的字段；重复 tag 通过池化索引复用。

可以这样理解：

```text
完整状态 S0
  + 5 秒：只改 screen=off
  + 2 秒：只改 wakelock=on，tag=(UID, name)
  + 8 秒：只改 batteryLevel=79
```

读取时从前向后应用 delta，重建时间线。History 容量有限，它是面向诊断的紧凑事件轨迹，不是无限保存的数据库。

### 9.3 History 和累计统计互补

累计统计回答：

> UID 10086 一共持有 partial WakeLock 多久？

History 回答：

> 它在什么时间开始，是否恰好处于熄屏、蜂窝活跃或电量快速下降阶段？

只有累计量不容易判断因果时序；只有 History 又不方便快速求总量。因此 Android 同时维护两套视角。

### 9.4 Battery Historian 的数据从哪里来

Battery Historian 不是手机里负责记账的核心服务。它主要把 bugreport 中 BatteryStats 的 history/checkin 等文本或结构化数据解析为时间轴图表。

```text
手机运行时：BatteryStatsImpl 持续记账
            │
            ▼
bugreport / dumpsys batterystats
            │
            ▼
电脑侧 Battery Historian 解析和可视化
```

因此 Historian 图里没有某事件时，先区分：事件根本没被上报、History 被裁剪/重置、采集区间不含该事件，还是解析展示没有覆盖该字段。

---

## 10. `BatteryExternalStatsWorker`：把外部世界拉进账本

### 10.1 为什么不能所有数据都靠 note

CPU 时间来自内核，网络字节来自 NetworkStats，控制器活动来自 Wi-Fi/蓝牙/Telephony。它们不是每次活动都同步调用 `note...()`，而是提供累计读数。

worker 的职责是：

- 串行化同步任务；
- 异步请求控制器活动信息；
- 在超时范围内等待回调；
- 读取 CPU、内核 WakeLock、内存带宽、RPM/rail 等数据；
- 将有效 delta 交给 `BatteryStatsImpl`；
- 在 History 中写入 `EVENT_COLLECT_EXTERNAL_STATS`，便于诊断。

### 10.2 一次 `UPDATE_ALL` 大致做什么

```text
BatteryExternalStatsWorker 单线程
  ├─ 请求 WifiActivityEnergyInfo
  ├─ 请求 BluetoothActivityEnergyInfo
  ├─ 请求 ModemActivityInfo
  ├─ 等待控制器结果（有超时）
  ├─ updateCpuTimeLocked()
  ├─ updateKernelWakelocksLocked()
  ├─ updateKernelMemoryBandwidthLocked()
  ├─ updateBluetoothStateLocked()
  ├─ updateWifiState(delta)
  └─ updateMobileRadioState(info)
```

Wi-Fi 和 Modem 更新会在 `mStats` 大锁外先取网络统计，再在内部按需加锁，避免持锁进行较慢操作。

### 10.3 为什么不是每个事件都立即全量同步

读取多个服务、内核节点和控制器会产生 CPU、Binder 与 I/O 成本。为了统计耗电而频繁唤醒系统，本身就会制造耗电。

Android 在准确度和开销之间折中：

- 插拔电这样的统计边界立即全量同步；
- 电量变化可以延迟收集；
- WakeLock/屏幕变化只同步必要类别；
- 写盘前抓取较完整快照；
- 某些数据按周期或 dump 需求更新。

这也意味着短时间观察时，`dumpsys` 结果可能稍后才反映某些硬件增量。

---

## 11. CPU、网络和无线控制器怎样归到 UID

### 11.1 CPU

CPU 统计通常包含 UID 的 user/system 时间、频点驻留时间、cluster 活动等。基本模型可写成：

```text
CPU mAh ≈ Σ(某 UID 在某 cluster/频点的时间 × 对应平均电流)
```

实际 `CpuPowerCalculator` 还要处理进程 CPU、active time、cluster time 等数据与设备 PowerProfile。频率越高通常功耗系数越大，所以“同样 1 秒 CPU 时间”不一定消耗相同。

必须注意：调度器和内核统计提供的是归因依据，电流系数来自设备配置；配置不准确会直接影响估算。

### 11.2 网络字节不是能耗本身

UID 流量能回答谁收发了多少 bytes/packets，但无线能耗还取决于：

- Wi-Fi 或蜂窝网络；
- 信号质量、调制和重传；
- 控制器 idle/rx/tx 时间；
- 蜂窝 radio tail，即数据传输结束后仍保持高功耗状态；
- 多个 UID 的传输是否重叠。

所以不能用“每 MB 固定多少 mAh”精确解释所有场景。

### 11.3 Wi-Fi 控制器

Android 11 的 `WifiPowerCalculator` 对 UID 活动使用 idle/rx/tx 时间乘相应平均电流：

```java
app.wifiPowerMah =
        (idleTime * idleCurrentMa
       + txTime * txCurrentMa
       + rxTime * rxCurrentMa) / (1000 * 60 * 60);
```

控制器总项若提供 power counter，会优先利用它；若为 0，则用同样的时间模型估算。已分给 UID 的部分从控制器总量中扣除，剩余部分形成系统/未归属项。

### 11.4 蜂窝与蓝牙

思路相似，但分摊因子不同：

- Modem 可报告 sleep/idle/rx/不同 tx power level 时间；
- 移动 radio active 时间可能按 UID 数据包等依据分配；
- 蓝牙控制器提供 idle/rx/tx 与能量信息，并结合 UID 扫描/流量活动归因。

“控制器测得总量”与“UID 分配规则”是两个层次，阅读时要分开。

---

## 12. 从活动量到 mAh：`PowerProfile` 和计算器

### 12.1 `PowerProfile` 是设备模型

`PowerProfile` 读取设备配置的典型平均电流，例如：

- CPU 每 cluster/频点；
- 屏幕开启与不同亮度；
- Wi-Fi/蓝牙/Modem 不同状态；
- GPS、相机、闪光灯；
- CPU idle 等。

它是机型相关数据。AOSP 默认值只能提供框架，厂商应根据具体硬件校准 overlay/XML 数据。

### 12.2 通用换算公式

若平均电流是 `I` mA、持续时间是 `t` ms：

```text
耗电量(mAh) = I(mA) × t(ms) ÷ 3,600,000
```

例如某模型状态平均 180 mA，持续 20 秒：

```text
180 × 20,000 ÷ 3,600,000 = 1 mAh
```

这只是教学示例，不代表任何真实设备参数。

### 12.3 测量值、模型值和分摊值

建议把结果分成三层理解：

| 层次 | 例子 | 局限 |
|---|---|---|
| 硬件/驱动累计值 | 电荷计数、控制器 energy used、rail energy | 常是整机或模块总量，不天然带 UID |
| 活动统计 | UID CPU 时间、流量、WakeLock、扫描时长 | 能说明活动量，不等于电能 |
| 模型与分摊 | 平均电流×时间、按数据包分控制器能耗 | 依赖配置与归因假设 |

设置页结果通常是三层信息结合，而不是纯粹属于其中一层。

---

## 13. `BatteryStatsHelper` 与 `BatterySipper`：形成用户看到的结果

`BatteryStatsHelper` 依次运行 CPU、WakeLock、移动网络、Wi-Fi、蓝牙、传感器、相机等 calculator，把结果写入 `BatterySipper`。

一个 `BatterySipper` 可以表示：

- 某个 UID；
- Screen；
- Cell；
- Wi-Fi；
- Bluetooth；
- Idle；
- Android OS 或其他系统项。

内部会保存诸如：

```text
cpuPowerMah
wakeLockPowerMah
mobileRadioPowerMah
wifiPowerMah
sensorPowerMah
cameraPowerMah
totalPowerMah
```

最终百分比不是简单“该 UID mAh ÷ 标称电池容量”。展示层还会考虑本次统计的总估算耗电、最小排除阈值、系统项，以及实测放电量与模型总量之间的差异。

### 13.1 unaccounted 与 overcounted

模型估算总量可能与由电池电量/容量推断的真实放电范围不一致：

- 模型总量偏低：可能出现未计入/未归属部分；
- 模型总量偏高：可能出现过度计算部分；
- 电池百分比粒度、容量老化、温度和计量误差都会影响比较。

这不是简单的一处加法 bug，而是“硬件总量、软件活动、功耗模型”无法完全重合的自然结果。

---

## 14. 统计周期、自动重置与持久化

### 14.1 “自上次充满”不是一句绝对字面描述

`BatteryStats` 仍定义：

```java
STATS_SINCE_CHARGED = 0;
STATS_CURRENT = 1;          // Android Q 起已弃用
STATS_SINCE_UNPLUGGED = 2; // Android Q 起已弃用
```

Android 11 的注释指出，Q 起实际支持重点是 `STATS_SINCE_CHARGED`。但它的 reset 逻辑并不是机械地只在 100% 执行。

从插电切到用电池时，如果允许自动重置，并满足下面之一，可能清空旧统计：

- 之前状态为 FULL；
- 当前电量至少 90%；
- 曾低于 20%，后来充到至少 80%。

这是为了形成有意义的充放电统计周期，同时兼容用户未必每次充到 100% 的现实。

### 14.2 为什么 reset 前可能保存 checkin

如果旧周期有足够放电量，源码会在 reset 前把摘要写入 checkin 文件，避免一段有分析价值的完整周期直接丢失。随后：

- 清累计统计；
- 重新初始化 History；
- 重设放电起点和 step tracker；
- 开启新的 on-battery 时间基准。

### 14.3 `batterystats.bin`

构造 `BatteryStatsImpl` 时，系统目录下创建：

```java
mStatsFile = new AtomicFile(new File(systemDir, "batterystats.bin"));
```

典型位置是 `/data/system/batterystats.bin`。使用 `AtomicFile` 是为了让写入中断时能恢复到有效版本。系统会异步写入，例如发生重要状态变化或距离上次写入超过一定时间。

不要手工编辑这个二进制文件。源码学习和诊断优先使用 `dumpsys batterystats`/bugreport 输出。

---

## 15. 一条完整案例：应用后台定位为何出现在耗电榜

假设 UID 10123 在熄屏时通过系统位置服务持续定位：

```text
App UID 10123
  │ 发起定位请求，责任通过 WorkSource 传递
  ▼
Location/GNSS 相关服务
  ├─ noteStartGps / sensor timer → UID 10123
  ├─ 可能持有 partial WakeLock → UID 10123
  ├─ CPU 内核统计记录 UID 执行时间
  └─ 网络统计记录辅助定位/上传流量
                 │
                 ▼
BatteryStatsImpl
  ├─ on-battery + screen-off 时间基准正在运行
  ├─ UID timer/counter 增长
  └─ History 记录 GPS、WakeLock、网络状态时序
                 │
                 ▼
外部快照同步
  ├─ CPU delta
  ├─ 网络/Modem delta
  └─ 控制器活动与能量
                 │
                 ▼
各 PowerCalculator
  ├─ GPS/传感器模型
  ├─ CPU 模型
  ├─ WakeLock 基础清醒成本
  └─ 移动网络模型/分摊
                 │
                 ▼
BatterySipper(UID 10123) → 设置页应用标签与百分比
```

这里不存在一个“定位耗电”数字从 GNSS HAL 原封不动传到设置页。多个维度在 UID 下汇总，而且系统服务必须正确传递 `WorkSource`，否则部分工作可能留在 system UID。

---

## 16. 只读源码时怎样验证理解

你在 Mac 上不需要编译，可以按以下顺序阅读。

### 16.1 第一轮：只追电池边界

```bash
rg -n "setBatteryState|isOnBattery|setOnBatteryLocked|updateTimeBasesLocked" \
  frameworks/base/services/core/java/com/android/server/am/BatteryStatsService.java \
  frameworks/base/core/java/com/android/internal/os/BatteryStatsImpl.java
```

回答：为什么插拔电时 external sync 必须排在时间基准切换前？

### 16.2 第二轮：只追一个事件

选择 partial WakeLock：

```bash
rg -n "noteStartWakelock|noteStartWakeLocked|noteStopWakeLocked" \
  frameworks/base/services/core/java/com/android/server/am/BatteryStatsService.java \
  frameworks/base/core/java/com/android/internal/os/BatteryStatsImpl.java
```

画出：服务入口 → 权限 → `mStats` 锁 → UID 映射 → History → UID Timer。

### 16.3 第三轮：只追一个计算器

```bash
sed -n '1,220p' \
  frameworks/base/core/java/com/android/internal/os/WifiPowerCalculator.java
```

区分 `calculateApp()` 和 `calculateRemaining()`：前者给 UID 算活动，后者处理控制器总量减去 UID 已分配量后的剩余。

### 16.4 第四轮：对比三个名词

在笔记中自己写一句定义：

- History：状态和事件的时间线；
- Timer/Counter：统计周期内的累计活动量；
- PowerCalculator：把活动量换算为估计能耗。

如果能不看本章解释清楚三者差异，主线就已经掌握。

---

## 17. 常见误区逐条纠正

### 误区 1：耗电百分比就是硬件直接测出的应用电流

不对。硬件可能提供整机、rail 或控制器总量，UID 结果还需活动统计、模型和分摊。

### 误区 2：`noteStart...()` 一调用就算出 mAh

不对。note 阶段主要记录事件/时间；mAh 通常由后续 calculator 计算。

### 误区 3：WakeLock 时间就是 CPU 运行时间

不对。WakeLock 表示阻止挂起的责任区间，CPU 可能空闲，也可能执行其他 UID；CPU 时间单独统计。

### 误区 4：流量最多的应用一定最耗无线电

不一定。信号、重传、控制器状态、radio tail 和并发传输都会改变代价。

### 误区 5：同一包名永远对应同一个统计对象

不对。底层主要按 UID；多用户、shared UID、isolated UID 都会影响映射。

### 误区 6：插电时 Timer 还在，统计就一定继续增长

不对。Timer 对象可以存在，但它依赖的 on-battery `TimeBase` 已停止。

### 误区 7：History 是完整且永久的原始采样

不对。它是容量有限、delta 压缩的状态/事件轨迹，会重置或轮转。

### 误区 8：统计总量与电池真实下降必须完全相等

不对。电量百分比、电荷计量、容量老化、模型误差和无法归因的共享耗电都会造成差异。

---

## 18. 排查耗电问题的思考框架

看到一个 UID 异常时，不要只盯总百分比。按四层问：

### 第一层：统计区间是否可信

- 是否刚 reset？
- 是否跨过插拔电或重启？
- bugreport 是否覆盖问题时段？
- 观察的是 since charged 还是短期 History？

### 第二层：哪种活动异常

- CPU 时间/高频驻留？
- partial WakeLock？
- 移动网络或 Wi-Fi？
- GPS/传感器/相机？
- Job、Alarm、Sync 是否过密？

### 第三层：归因是否正确

- 系统代理工作有没有 `WorkSource`？
- 是否 shared UID？
- isolated UID 是否映射回宿主？
- 多用户下是否看错 UID？

### 第四层：模型是否可信

- 厂商 `PowerProfile` 是否校准？
- 控制器是否真的支持 enhanced power reporting？
- 硬件报告的是总量还是 UID 量？
- 是否存在较大的 unaccounted/overcounted？

这种分层方式能避免把“应用确实活动异常”和“模型/归因偏差”混成一个问题。

---

## 19. 本章源码阅读题

### 题 1

设备从电池供电切到 USB 插电。为什么不能先调用 `setBatteryStateLocked()`，稍后再同步 CPU？

提示：画出两个累计快照之间的增量属于哪一侧时间基准。

### 题 2

一个 partial WakeLock 嵌套获取两次，只释放一次，Timer 是否应停止？为什么？

### 题 3

Wi-Fi 控制器报告了总 power counter，为什么仍需要 UID 的 rx/tx/idle 活动信息？

### 题 4

应用通过 system_server 中的服务使用硬件，缺少 `WorkSource` 会造成什么现象？

### 题 5

为什么 `mOnBatteryScreenOffTimeBase` 的条件是 `unplugged && screenOff`，而不是只有 `screenOff`？

### 参考答案

1. CPU 累计增量实际发生在旧状态；先切换会让增量跨越边界并可能记错统计区间。
2. 不应停止；嵌套从 2 降到 1，资源仍处于持有状态，只有降到 0 才结算停止。
3. 总 power counter 通常不含 UID 归属；UID 活动用于把控制器总量分摊或计算已归属部分。
4. 责任可能落到 system UID，使请求应用看起来耗电偏低、Android OS/系统项偏高。
5. 该时间基准描述“电池供电且熄屏”的交集；插电熄屏不应进入电池放电统计。

---

## 20. 复读后的易混淆点补强

本章写完后，从第一次接触源码的视角复读，最可能卡住的是下面四处，因此再换一种说法总结。

### 20.1 TimeBase 到底是什么

它不是第三种系统时钟，而像一个总闸：底层仍读取 uptime/realtime；总闸打开时，挂在它上面的统计对象才把时间差计入“有效时间”。插电就是关闭 on-battery 总闸。

### 20.2 同步快照为何强调“先后顺序”

累计读数只告诉你 A 到 B 增加了多少，不告诉你增量发生时属于插电还是放电。必须在状态边界处先封账，再翻到新的一页。

### 20.3 “硬件测量”为什么仍不是“应用测量”

控制器好比大楼总电表，UID 是各房间。即使大楼总表完全准确，如果没有每个房间的独立表，仍需按各房间活动证据分摊。

### 20.4 百分比为什么会变

百分比既受分子影响，也受分母、统计周期和新快照影响。某应用估算 mAh 没明显增加，其他系统项补齐后，它的占比也可能下降。

---

## 21. 本章小结

把整章压缩成一条链：

```text
BatteryService 提供插电/电量边界
  → BatteryStatsService 校验并串行安排同步
  → 边界前先抓外部累计快照
  → BatteryStatsImpl 切换 on-battery TimeBase
  → Framework note 事件与内核/控制器 delta 进入 UID Timer/Counter
  → History 保存压缩时序
  → PowerCalculator 用控制器能量或 PowerProfile 模型换算 mAh
  → BatteryStatsHelper 汇总 BatterySipper 和展示百分比
```

阅读这套源码时最重要的不是记住所有字段，而是始终区分四件事：

1. **事件记账**：谁做了什么；
2. **时间门控**：这段活动是否属于电池供电统计；
3. **活动归因**：共享硬件活动怎样分给 UID；
4. **功耗估算**：活动量怎样通过测量值或模型换算成 mAh。

下一章将继续阅读 Android 的系统遥测链路：`statsd`、`StatsCompanionService`、Atom 上报、pull/push 数据与统计配置，理解 BatteryStats 之外系统如何进行结构化指标采集。

