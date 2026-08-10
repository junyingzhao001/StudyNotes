# 68 DisplayPowerController、自动亮度与屏幕电源状态链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：理解 PowerManagerService 如何生成 `DisplayPowerRequest`，DisplayPowerController 如何综合策略、距离传感器和亮度来源，自动亮度怎样从环境光得到目标值，以及状态、渐变和面板下发如何异步收敛。

---

## 1. 五层模型

```text
【意图层】PowerManagerService
wakefulness + 用户活动 + WakeLock + Dream/Doze + 设置
                ↓ DisplayPowerRequest
【决策层】DisplayPowerController
policy → screen state；亮度优先级；proximity 覆盖
                ↓
【执行层】DisplayPowerState
screenState + brightness + colorFadeLevel
                ↓
【平滑层】RampAnimator / ColorFade / WMS blocker
                ↓
【设备层】DisplayBlanker → DMS → LocalDisplayDevice
→ SurfaceFlinger/HWC/面板
```

一句话：**PMS 决定系统想要什么，DPC 决定怎样达到目标，DisplayPowerState 驱动当前值，显示设备层落实到硬件。**

---

## 2. 四种状态不能压成一个 screenOn

| 层次 | 典型值 | 含义 |
|---|---|---|
| PMS wakefulness | AWAKE/DREAMING/DOZING/ASLEEP | 整机交互阶段 |
| `DisplayPowerRequest.policy` | OFF/DOZE/DIM/BRIGHT/VR | PMS 对显示的策略意图 |
| `Display.STATE_*` | OFF/ON/DOZE/DOZE_SUSPEND/VR | 显示状态 |
| brightness | 0..1、off、invalid | 独立亮度量 |

`AWAKE` 不保证面板当前已亮；距离传感器可让它灭屏。`POLICY_DIM` 与 `POLICY_BRIGHT` 都映射到 `STATE_ON`，区别主要在亮度。`DOZE` 也不等于完全关闭。

---

## 3. 源码地图

```text
frameworks/base/services/core/java/com/android/server/power/
    PowerManagerService.java
    Notifier.java

frameworks/base/services/core/java/com/android/server/display/
    DisplayPowerController.java
    DisplayPowerState.java
    AutomaticBrightnessController.java
    BrightnessMappingStrategy.java
    RampAnimator.java
    DisplayManagerService.java
    LocalDisplayAdapter.java

frameworks/base/core/java/android/hardware/display/
    DisplayManagerInternal.java   // DisplayPowerRequest
```

协作者还有 SensorManager、WindowManagerPolicy、DreamManager、BatteryStats、SettingsProvider 和 SurfaceFlinger。

---

## 4. PMS 怎样生成 DisplayPowerRequest

`PowerManagerService.updatePowerStateLocked()` 按 dirty bits 重算各阶段，`updateDisplayPowerStateLocked()` 生成显示请求。

### 4.1 policy 优先级

`getDesiredScreenPolicyLocked()` 可概括为：

```text
ASLEEP 或 quiescent → OFF
DOZING 且有 DOZE wakelock → DOZE
VR 模式 → VR
亮屏 WakeLock / bright user activity / 未开机完成 / brightness boost → BRIGHT
否则 → DIM
```

用户活动超时通常先 DIM；继续超时导致 wakefulness 进入睡眠后才 OFF。

### 4.2 请求字段

```text
policy
screenBrightnessOverride
useAutoBrightness
useProximitySensor
boostScreenBrightness
lowPowerMode / screenLowPowerBrightnessFactor
dozeScreenState / dozeScreenBrightness
```

窗口亮度 override 优先于普通亮度设置。开机未完成时使用默认亮度，避免传感器尚未稳定导致闪烁。

### 4.3 request 是异步收敛协议

```java
mDisplayReady = mDisplayManagerInternal.requestPowerState(
        mDisplayPowerRequest, mRequestWaitForNegativeProximity);
```

返回 `false` 不是失败，而是显示仍在异步变化，例如等待窗口绘制、颜色渐隐或设备状态。DPC 完成关键步骤后用 `onStateChanged()` 通知 PMS 再次求值，直到 ready。

---

## 5. 为什么 DPC 要复制 pending request

```text
PMS request
→ DPC.requestPowerState
→ 锁内复制到 mPendingRequestLocked
→ mDisplayReadyLocked=false
→ Handler MSG_UPDATE_POWER_STATE
→ updatePowerState()
```

复制避免调用方之后修改原对象影响已排队请求，也把 PMS 锁与 DPC Handler 状态机解耦。相同请求不重复排消息。

---

## 6. policy 到基础 state

```text
POLICY_OFF    → STATE_OFF
POLICY_DOZE   → dozeScreenState，未知时 STATE_DOZE
POLICY_VR     → STATE_VR
POLICY_DIM    → STATE_ON
POLICY_BRIGHT → STATE_ON
```

这只是第一轮结果，proximity、状态动画和 WMS blocker 还会覆盖或延迟它。

---

## 7. 距离传感器链路

通话贴耳时 policy 仍可能是 BRIGHT，但请求启用 proximity：

```text
SensorEvent raw distance
→ mPendingProximity
→ positive/negative debounce
→ mProximity
→ near 且没有 ignore
→ mScreenOffBecauseOfProximity=true
→ state 强制 STATE_OFF
→ callback PMS: onProximityPositive
```

### 7.1 为什么 debounce

边界处信号会抖动。单个 sample 立即亮灭会闪烁，因此近、远都要稳定一段时间，并且可配置不同延迟。

### 7.2 waitForNegativeProximity

屏幕因贴耳熄灭后，某些请求要求等到稳定 far 才重新亮屏：

```text
policy 想 ON + 当前仍 near + waitingForNegative
→ 继续监听并保持灭屏
→ far debounce 完成
→ onProximityNegative
→ PMS/DPC 重新求值
```

距离灭屏不是 `goToSleep()`：它只覆盖显示状态，整机 wakefulness、CPU suspend 和通话 WakeLock 另行计算。

---

## 8. 亮度来源的决策顺序

可用以下近似优先级阅读 `updatePowerState()`：

```text
屏幕 OFF                           → off brightness
VR                                 → VR brightness
窗口/系统 screenBrightnessOverride → override
临时亮度                           → temporary
brightness boost                   → maximum
自动亮度                           → ambient lux 映射结果
Doze 默认亮度                      → doze config
手动亮度                           → Settings 当前值
最后叠加 DIM、低电量比例和范围 clamp
```

源码用 `Float.NaN` 表示“亮度还没有被前面条件决定”，这是状态机哨兵，不是计算错误。

### 8.1 BrightnessReason

基础原因包括 SCREEN_OFF、VR、OVERRIDE、TEMPORARY、BOOST、AUTOMATIC、DOZE、MANUAL；修饰符包括 DIMMED 和 LOW_POWER。

排查“为什么暗”应同时看：

```text
基础 reason + modifiers + target + current
```

只看 Settings 滑块是不够的。

---

## 9. 自动亮度：lux 不是直接等于 brightness

完整链路：

```text
light sensor sample
→ AmbientLightRingBuffer 保存时间窗口
→ 过滤并计算稳定 ambientLux
→ brightening/darkening threshold
→ 对应 debounce
→ BrightnessMappingStrategy(lux)
→ automatic screen brightness
→ callback DPC
→ updatePowerState
```

### 9.1 为什么不用最新 lux

瞬时值会受手遮挡、屏幕反光和灯光闪烁影响。控制器使用时间窗口、迟滞阈值与稳定时间，避免在边界附近来回跳亮度。

### 9.2 变亮与变暗可不同

进入强光后需要尽快看清；短暂阴影不应立刻变暗，所以 brightening 与 darkening 使用不同 threshold/debounce。

### 9.3 BrightnessMappingStrategy

它把 lux 映射成设备适合的 nits/brightness 曲线。曲线与面板、传感器位置和厂商配置相关，不是全 Android 设备统一公式。

---

## 10. 自动模式下用户拖动滑块

它不只是永久改一个手动值，而可在当前 ambientLux 建立用户数据点：

```text
当前 ambientLux + 用户选择 brightness
→ addUserDataPoint
→ 修正短期亮度模型/adjustment
→ 相近环境复用用户偏好
```

短期模型可因超时、环境显著变化或配置切换而重置，防止一次室内调整永久扭曲所有光照区间。这是“自动亮度学习偏好”的本地曲线机制。

---

## 11. DIM 和 Battery Saver

### 11.1 DIM

policy 为 DIM 时，DPC 在基础亮度上降低至少一定量，同时受 dim config 与最小亮度限制。它仍是 `STATE_ON`，用来提示即将灭屏。

### 11.2 Low Power

```text
基础 brightness × screenLowPowerBrightnessFactor
→ 不低于 minimum
→ 添加 MODIFIER_LOW_POWER
```

所以自动亮度本身正确时，Battery Saver 仍可能让最终画面更暗。

---

## 12. RampAnimator：target 与 current 分离

DPC 调用：

```java
animateScreenBrightness(target, rate);
```

RampAnimator 保存 current、target、rate，按帧逐步逼近：

```text
0.20 → 0.26 → 0.32 → ... → 0.80
```

环境光稳定变化通常使用慢速，避免视觉跳变；用户主动调整、dim/low-power 状态改变或特殊状态切换可能快速变化或跳过 ramp。

因此设置值已经改变而面板仍在过渡是正常现象。

---

## 13. ColorFade 与 WMS 绘制握手

若面板先亮而 Activity 尚未画好，用户会看到旧帧或闪烁。DPC 与 WindowManagerPolicy 协作：

```text
目标 ON
→ screenTurningOn(unblocker)
→ 黑色 ColorFade surface / blockScreenOn
→ WMS 等关键窗口绘制完成
→ unblocker
→ 移除遮挡，完成 screen on
```

灭屏也有 `screenTurningOff` 和可能的 fade 动画。

DPC 维护报告给 policy 的阶段：

```text
SCREEN_OFF
SCREEN_TURNING_ON
SCREEN_ON
SCREEN_TURNING_OFF
```

它与 `Display.STATE_ON/OFF` 不是同一状态：前者是 WMS 生命周期握手，后者是显示设备状态。

---

## 14. DisplayPowerState 的职责

它把三个执行属性收口：

```text
mScreenState
mScreenBrightness
mColorFadeLevel
```

属性改变后标 dirty，并由 PhotonicModulator/ColorFade 工作推进。视觉亮度还会受 colorFadeLevel 影响，所以黑屏不一定是逻辑 brightness=0。

设备状态通过：

```text
DisplayPowerState
→ DisplayBlanker.requestDisplayState(state, brightness)
```

接入 DMS。

---

## 15. 从 DPC 到物理面板

```text
DisplayPowerState / PhotonicModulator
→ DisplayBlanker
→ DisplayManagerService.requestGlobalDisplayStateInternal
→ 遍历 DisplayDevice
→ device.requestDisplayStateLocked(state, brightness)
→ 锁外执行 Runnable
→ LocalDisplayDevice
→ SurfaceControl power mode / brightness
→ SurfaceFlinger / HWC / display HAL
→ 面板与背光
```

Device 返回 Runnable 是为了把潜在阻塞的 SurfaceFlinger/硬件操作移出 DMS `SyncRoot`，降低长锁和死锁风险。

某些硬件状态不能一步切换，例如 ON 到 DOZE_SUSPEND 可能先经过 DOZE。LocalDisplayAdapter 隐藏这些顺序约束。

---

## 16. Doze 与 AOD

```text
PMS wakefulness=DOZING
→ POLICY_DOZE
→ DreamManager 提供 dozeScreenState/dozeBrightness
→ DPC 选择 DOZE、DOZE_SUSPEND、ON 等
→ Doze/AOD 服务绘制受限内容
```

`DOZE_SUSPEND` 通常允许显示硬件维持低功耗内容，同时 AP 有机会 suspend；具体能力取决于设备。

若存在 draw WakeLock，PMS 可把 DOZE_SUSPEND 临时提升为 DOZE，让显示管线完成绘制。Doze 中是否允许自动亮度由设备配置决定；否则使用 doze override/default。

这里的显示 Doze 与 DeviceIdleController 的网络/后台限制 Doze 不是同一状态机，尽管产品语言都叫 Doze。

---

## 17. 电源键灭屏完整链路

```text
电源键 → PhoneWindowManager/PMS goToSleep
→ wakefulness 改变，dirty bits
→ getDesiredScreenPolicyLocked = OFF
→ DisplayPowerRequest(POLICY_OFF)
→ DPC requestPowerState：not ready
→ updatePowerState：STATE_OFF + off brightness
→ WindowManagerPolicy.screenTurningOff
→ ColorFade/状态动画
→ DisplayPowerState.setScreenState(OFF)
→ DisplayBlanker → DMS → LocalDisplayDevice
→ SurfaceFlinger power mode OFF
→ DPC onStateChanged → PMS 再次收敛
→ ready，Notifier 完成相关通知
```

输入线程没有直接操作面板，而是改变系统意图后由异步状态机执行。

---

## 18. 自动亮度变亮完整链路

```text
室内走到阳光下
→ light sensor 连续 sample
→ RingBuffer 过滤
→ ambientLux 越过 brightening threshold
→ brightening debounce 完成
→ MappingStrategy 得到更高 target
→ AutomaticBrightnessController callback
→ DPC BrightnessReason=AUTOMATIC
→ RampAnimator 提高 current brightness
→ DisplayPowerState/Blanker/DMS/LocalDisplayDevice
→ 面板背光提高
```

诊断时至少记录四个量：raw lux、stable ambientLux、target brightness、current/ramped brightness。

---

## 19. ready 不等于所有画面业务都完成

DPC ready 表示关键显示状态、blocker、动画和 proximity 条件已收敛。但用户感知仍可能受 App 首帧、SystemUI 动画和面板响应影响。

反过来，肉眼已看到画面时，brightness ramp 或 policy 通知也可能仍在完成。性能分析要给“亮屏完成”定义明确时间点。

---

## 20. 线程与锁边界

```text
PMS mLock：计算 DisplayPowerRequest
→ DisplayManagerInternal 本地接口
DPC mLock：复制 pending request
→ DPC Handler：串行 updatePowerState
Sensor event：Handler/debounce 后触发更新
DMS mSyncRoot：更新设备期望
→ 锁外 Runnable：SurfaceFlinger/硬件操作
```

三个 `mLock/SyncRoot` 不是同一把锁。源码中的 `Locked` 必须结合所属类理解。

---

## 21. 常见误解

1. `AWAKE` 不保证屏幕此刻可见，proximity/WMS blocker 仍可阻止亮屏。
2. `POLICY_DIM` 不对应不存在的 `STATE_DIM`，它仍是 ON 加亮度修饰。
3. 自动亮度不是最新 lux 直接查表，还包括过滤、迟滞、阈值和 debounce。
4. 设置亮度不等于面板 current 立即到达 target，RampAnimator 会渐变。
5. 距离灭屏不等于整机睡眠。
6. DOZE 不等于 OFF，也不等于设备空闲 Doze。
7. DPC 不直接调用显示 HAL，而经 DisplayPowerState、DMS、Device 和 SurfaceFlinger。
8. `requestPowerState=false` 表示尚未 ready，不是请求失败。
9. 黑屏不一定亮度为 0，也可能是 OFF、ColorFade、WMS blocker 或设备故障。

---

## 22. 分层故障排查

### 22.1 亮屏慢

```text
PMS wakefulness/policy
→ DPC target state
→ proximity near/waiting
→ screen-on unblocker/WMS 绘制
→ ColorFade
→ DisplayPowerState clean
→ LocalDisplayDevice/SF power mode
→ App 首帧
```

### 22.2 自动亮度跳变

检查 raw lux、反光/遮挡、ring buffer、threshold、hysteresis、debounce、mapping 曲线、用户 data point 和 ramp rate。

### 22.3 自动亮度完全不变

检查 `useAutoBrightness`、当前 state 是否允许、是否被 override/temporary/boost 抢占、sensor 是否 enabled、ambientLux 是否 valid、映射结果是否有效。

### 22.4 离耳仍不亮

检查 raw/pending/debounced proximity、negative debounce、waiting/ignore flag 和 sensor 注册状态。

### 22.5 Battery Saver 后太暗

查看基础 BrightnessReason 以及 `MODIFIER_LOW_POWER`、`MODIFIER_DIMMED`，不要先改自动亮度曲线。

### 22.6 AOD 不亮或功耗高

检查 POLICY_DOZE、dozeScreenState、dozeBrightness、draw WakeLock 是否阻止 suspend，以及面板是否真正支持低功耗显示。

---

## 23. macOS 只读练习

1. 阅读 `getDesiredScreenPolicyLocked()`，为五个 policy 各写一个场景。
2. 在 `updateDisplayPowerStateLocked()` 标出 DisplayPowerRequest 各字段来源。
3. 追 `requestPowerState()` 的请求复制、Handler、ready false 和 callback。
4. 画 policy→state 表，再加 proximity near 覆盖线。
5. 从 SensorEvent 追到 AmbientLightRingBuffer、ambientLux 和 MappingStrategy。
6. 搜索 `addUserDataPoint`、`hasUserDataPoints`、`resetShortTermModel`。
7. 按真实代码给 brightnessState 的各来源标优先级。
8. 从 `animateScreenBrightness()` 追进 `RampAnimator.animateTo()`。
9. 从 `setScreenState(ON)` 追 `screenTurningOn(unblocker)`。
10. 从 DisplayPowerState 追到 LocalDisplayDevice 和 SurfaceControl。
11. 比较 OFF、DOZE、DOZE_SUSPEND 及 draw WakeLock 的影响。

---

## 24. 推荐阅读顺序

```text
1. PMS getDesiredScreenPolicyLocked/updateDisplayPowerStateLocked
2. DPC requestPowerState/updatePowerState 的 state 与 proximity
3. updatePowerState 的 brightness 决策
4. AutomaticBrightnessController/BrightnessMappingStrategy
5. RampAnimator/DisplayPowerState
6. DisplayBlanker/DMS/LocalDisplayAdapter
```

第一次不要试图顺读完整 DPC；它接近两千行。先按这六段建立骨架。

---

## 25. 完整心智模型

```text
电源键/用户活动/WakeLock/Dream/设置/Battery Saver
→ PMS wakefulness + DisplayPowerRequest
→ DPC Handler
   ├─ policy → state
   ├─ proximity → 可强制 OFF
   ├─ override/temporary/boost
   ├─ light sensor → ambientLux → auto target
   ├─ doze/manual fallback
   └─ dim/low-power/clamp
→ WindowManagerPolicy blocker + ColorFade + RampAnimator
→ DisplayPowerState current state/brightness
→ DisplayBlanker → DMS → LocalDisplayDevice
→ SurfaceFlinger/HWC/面板
```

总结：**屏幕电源不是布尔开关，而是 PMS 意图、DPC 异步状态机、传感器和亮度策略、WMS 绘制握手、动画以及硬件状态共同收敛的结果。**

---

## 26. 自测题

1. wakefulness、policy、display state、brightness 有何区别？
2. POLICY_DIM 为什么映射为 STATE_ON？
3. requestPowerState 返回 false 表示什么？
4. BRIGHT 为什么仍可能被 proximity 变为 OFF？
5. 自动亮度为什么不能使用最新 lux？
6. MappingStrategy 与 RampAnimator 分别负责什么？
7. 自动模式下拖动滑块怎样改变短期模型？
8. DIM 和 low-power 怎样叠加？
9. policy reported state 与 Display.STATE 有何区别？
10. 亮屏为什么等待 WMS unblocker？
11. 状态怎样到达物理面板？
12. OFF、DOZE、DOZE_SUSPEND 有何区别？

能画出五层模型，并讲清“贴耳灭屏”和“阳光下自动变亮”，本章就掌握了。

---

## 27. 下一章预告

```text
69 DreamManagerService、Doze 与 Always-On Display 链路
```

下一章会承接 POLICY_DOZE，追踪 DreamManager、DreamController、DreamService、SystemUI DozeMachine、DozeService、AmbientDisplayConfiguration 和 Doze WakeLock，并分清屏保 Dream、AOD Doze 与设备空闲 Doze。
