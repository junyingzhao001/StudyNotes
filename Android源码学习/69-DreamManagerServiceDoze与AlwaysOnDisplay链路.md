# 69 DreamManagerService、Doze 与 Always-On Display 链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译。  
> 本章目标：分清普通屏保 Dream、可 Doze 的 Dream、SystemUI AOD 状态机和设备空闲 Doze，追踪 PMS 如何启动 Dream、DreamManagerService 如何选择并绑定 DreamService、SystemUI 如何控制 AOD/pulse，以及 doze screen state、brightness 和 WakeLock 如何回到上一章的 DisplayPowerRequest。

---

## 1. 四个名字相近但不同的概念

| 名称 | 核心目的 | 主要实现 |
|---|---|---|
| 普通 Dream/屏保 | 充电、底座或用户 nap 后展示全屏内容 | `DreamManagerService + DreamController + DreamService` |
| Doze Dream | 在设备进入 DOZING 时承载低功耗环境显示服务 | 同一 Dream 框架，但 `canDoze=true` |
| AOD/Doze UI | 常显、抬手、通知 pulse、近距暂停等 UI 状态机 | SystemUI `DozeService + DozeMachine` |
| Device Idle Doze | 限制后台网络、Job、Alarm，降低待机功耗 | `DeviceIdleController` |

最重要结论：

> **AOD 使用 Dream 框架作为服务生命周期外壳，但它有自己的 SystemUI DozeMachine；DeviceIdleController 的 Doze 则是后台调度策略，和显示 Doze 不是一条状态机。**

---

## 2. 总体架构

```text
PowerManagerService
  wakefulness=DREAMING/DOZING
          ↓ DreamManagerInternal.startDream(doze)
DreamManagerService
  选择普通 dream 或 ambient display component
          ↓
DreamController
  bindServiceAsUser + timeout + death cleanup
          ↓ IDreamService.attach(token, canDoze,...)
DreamService
  普通屏保：自己的窗口和内容
  DozeService：组装 SystemUI DozeMachine
          ↓ startDozing(state, brightness)
DreamManagerService
          ↓ setDozeOverrideFromDreamManager
PowerManagerService.DisplayPowerRequest
          ↓
DisplayPowerController → DOZE/DOZE_SUSPEND/ON + brightness
```

这是一条控制链。AOD 图层的实际绘制仍经过 SystemUI、WMS、SurfaceFlinger。

---

## 3. 核心源码地图

```text
frameworks/base/services/core/java/com/android/server/dreams/
    DreamManagerService.java
    DreamController.java

frameworks/base/core/java/android/service/dreams/
    DreamService.java
    DreamManagerInternal.java
    IDreamManager.aidl
    IDreamService.aidl
    DreamActivity.java
    Sandman.java

frameworks/base/packages/SystemUI/src/com/android/systemui/doze/
    DozeService.java
    DozeFactory.java
    DozeMachine.java
    DozeUi.java
    DozeTriggers.java
    DozeScreenState.java
    DozeScreenBrightness.java
    DozePauser.java
    DozeSensors.java

frameworks/base/core/java/android/hardware/display/
    AmbientDisplayConfiguration.java

frameworks/base/services/core/java/com/android/server/power/
    PowerManagerService.java
```

用于对照但不属于本链：

```text
frameworks/base/apex/jobscheduler/service/java/com/android/server/DeviceIdleController.java
```

---

## 4. 普通 Dream 是什么

普通 Dream 可理解为系统管理的全屏屏保 Service。用户长时间无操作、设备充电/插入底座且设置允许时，PMS 可以进入 `WAKEFULNESS_DREAMING`。

典型用途：

- 时钟、照片屏保；
- 底座展示；
- 厂商定制的待机界面。

它不是普通 Activity：服务由 DreamManager 选择和绑定，再由 `DreamService` 建立 Dream 窗口/Activity 载体，并受电源状态统一管理。

### 4.1 requestDream 不直接 start Service

`DreamManagerService.requestDreamInternal()`：

```text
userActivity(noChangeLights=true)
→ PowerManager.nap(time)
→ PMS 判断是否真的能 dream
→ 合适时回调 DreamManagerInternal.startDream(false)
```

DreamManager 请求的是“请 PMS 进入 nap 并评估”，不是绕开 PMS 强开屏保。因为 Dream 是否允许取决于 wakefulness、用户活动、充电/配置和电源策略。

---

## 5. PMS 何时启动 Dream

PMS 的 sandman 逻辑综合：

```text
当前 wakefulness
→ canDreamLocked() 或 canDozeLocked()
→ DreamManager.stopDream(false)
→ DreamManager.startDream(wakefulness == DOZING)
```

参数 `doze` 决定选择哪类组件：

```text
false → 当前用户设置的普通 Dream component
true  → ambient display component（通常是 SystemUI DozeService）
```

PMS 随后检查 Dream 是否实际启动。若启动失败或条件不再成立，整机可能继续走睡眠，而不是永远卡在 DREAMING/DOZING。

---

## 6. DreamManagerService 如何选择组件

`startDreamInternal(doze)`：

```text
当前 userId
→ chooseDreamForUser(doze, userId)
→ doze 时选择 ambient display component
→ 普通时选择用户启用/默认 dream
→ startDreamLocked(component, isTest=false, canDoze=doze, userId)
```

Dream 配置按用户管理。用户切换时服务会停止旧 Dream，避免旧用户的屏保/环境显示跨用户继续存在。

`testDream` 是设置界面的预览路径，`isTest=true` 且不能 Doze，不应拿它推导正式电源状态。

---

## 7. Dream token：会话身份而不是窗口内容

`startDreamLocked()` 为每次 Dream 创建新的 Binder token：

```java
Binder newToken = new Binder();
mCurrentDreamToken = newToken;
```

它用于：

- 标识当前 Dream 会话；
- 校验 `finishSelf/startDozing/stopDozing` 是否来自当前会话；
- 连接 Dream 窗口/Activity 与系统管理记录；
- 防止旧 Service 的迟到回调控制新 Dream。

即使组件相同，新一次启动也应视为新会话。

---

## 8. DreamController 绑定链路

```text
DreamManagerService.startDreamLocked
→ 先立即停止旧 Dream
→ 记录 token/name/canDoze/userId
→ 获取临时 PARTIAL_WAKE_LOCK("startDream")
→ Handler: DreamController.startDream
→ bindServiceAsUser(
     DreamService.SERVICE_INTERFACE,
     BIND_AUTO_CREATE | BIND_FOREGROUND_SERVICE)
→ ServiceConnection.onServiceConnected
→ IDreamService.attach(token, canDoze, callback)
→ DreamService.onDreamingStarted()
```

临时 WakeLock 保证绑定和 attach 阶段 CPU 不会中途休眠；Dream 成功接管或失败清理后释放。

### 8.1 三类故障兜底

DreamController 处理：

```text
连接超时 → stopDream("slow to connect")
温和退出超时 → stopDream("slow to finish")
Binder death / service disconnected → 立即清理
```

Dream 是可替换组件，system_server 不能无限信任它按时响应。

---

## 9. 普通 Dream 与 Doze Dream 的关键差别

| 维度 | 普通 Dream | Doze Dream |
|---|---|---|
| `canDoze` | false | true |
| PMS wakefulness | DREAMING | DOZING |
| 主要组件 | 用户选择的 DreamService | ambient display component |
| 能否调用 `startDozing()` | 不应/服务端不接受 | 可以 |
| 屏幕目标 | 通常正常 ON/DIM | OFF、DOZE、DOZE_SUSPEND、短时 ON |
| 目标 | 屏保展示 | 极低功耗常显和脉冲 |

DreamService 调 `startDozing()` 时，DreamManagerService 会校验：

```java
mCurrentDreamToken == token && mCurrentDreamCanDoze
```

因此普通 Dream 不能仅靠调用同名方法升级成 AOD 权限。

---

## 10. DreamService 的两个层次

### 10.1 通用 DreamService

它提供：

```text
onDreamingStarted / onDreamingStopped
setInteractive / setFullscreen
wakeUp / finish
startDozing / stopDozing
setDozeScreenState / setDozeScreenBrightness
```

DreamController 通过 `IDreamService.attach()` 交付 token。服务此后对系统的 doze/finish 操作都带这个 token。

### 10.2 SystemUI DozeService

Android 11 的 AOD component 是继承 DreamService 的 SystemUI 服务。它利用 Dream 框架获得生命周期和 doze 权限，但实际编排交给 `DozeMachine`：

```text
onCreate → DozeFactory.assembleMachine(this)
onDreamingStarted
→ request INITIALIZED
→ DreamService.startDozing()
onDreamingStopped
→ request FINISH
```

所以 `DreamService.startDozing()` 和 `DozeMachine.State.DOZE` 不是同一层：前者向 system_server 宣告开始使用 doze 能力，后者是 SystemUI 内部 UI/传感器状态。

---

## 11. DozeMachine 的状态

常见状态：

```text
UNINITIALIZED
INITIALIZED
DOZE
DOZE_AOD
DOZE_AOD_PAUSING
DOZE_AOD_PAUSED
DOZE_REQUEST_PULSE
DOZE_PULSING
DOZE_PULSING_BRIGHT
DOZE_PULSE_DONE
DOZE_AOD_DOCKED
FINISH
```

理解分组：

```text
DOZE           → 屏幕通常关闭，等待触发
DOZE_AOD       → 常显低功耗内容
AOD_PAUSING/PAUSED → 因近距等条件暂停 AOD
REQUEST_PULSE  → 已接受一次短时唤显请求
PULSING        → 正在展示通知/手势唤起内容
PULSING_BRIGHT → 需要更亮的特殊 pulse
PULSE_DONE     → pulse 结束，回到基础 DOZE/AOD
FINISH         → 整个 Doze Dream 结束
```

---

## 12. 状态请求为何排队而不是递归执行

DozeMachine 的 Part 在处理 transition 时可能再次 `requestState()`。Machine 把请求加入 `mQueuedRequests`：

```text
request state
→ acquire transition WakeLock
→ 依次处理 queue
→ transitionPolicy
→ validateTransition
→ 所有 Part.transitionTo(old,new)
→ update state WakeLock
→ resolve intermediate state
→ queue 清空后 release transition WakeLock
```

这避免 Part 回调递归打乱状态顺序，并确保状态转换期间 CPU 保持运行。

非法转换会抛异常，例如未初始化只能先进入 INITIALIZED，PULSING 必须从 REQUEST_PULSE 进入。

---

## 13. INITIALIZED 后为什么还要自动选下一状态

`resolveIntermediateState()` 根据环境选择：

```text
已经 waking/awake（pulse 完成时） → FINISH
已 dock 且可显示 → DOZE_AOD_DOCKED
已 dock 但隐藏 → DOZE
用户启用 always-on → DOZE_AOD
否则 → DOZE
```

INITIALIZED 和 PULSE_DONE 是过渡态，不应长期停留。AOD 是否启用由 `AmbientDisplayConfiguration` 综合设置、资源能力和用户状态判断。

Battery Saver 可以把请求的 DOZE_AOD 降级为 DOZE；SystemUI host 也可 suppress AOD。因而“设置已打开”不保证此刻机器一定在 DOZE_AOD。

---

## 14. DozeMachine 不是一个巨型类：Parts 分工

`DozeFactory` 组装多个 Part：

| Part | 职责 |
|---|---|
| `DozeScreenState` | 将 Machine state 映射为 Display state |
| `DozeScreenBrightness` | 环境传感器/默认值到 Doze 亮度 |
| `DozeTriggers` | 通知、传感器、广播、pulse 请求 |
| `DozeUi` | 与 StatusBar/DozeHost 协作，time tick、pulse UI |
| `DozePauser` | 近距等条件下从 pausing 到 paused |
| `DozeDockHandler` | 底座状态 |
| `DozeWallpaperState` | AOD 壁纸状态 |
| `DozeAuthRemover` | 进入 Doze 后处理认证状态 |

Machine 只维护合法状态和统一广播 transition；各 Part 执行副作用。这比在一个 switch 中控制全部传感器、亮度、UI 更容易隔离。

---

## 15. Machine state 怎样变成 Display state

Android 11 `DozeMachine.State.screenState()` 的典型映射：

```text
DOZE / DOZE_AOD_PAUSED → Display.STATE_OFF
DOZE_AOD / AOD_PAUSING → Display.STATE_DOZE_SUSPEND
PULSING / PULSING_BRIGHT / AOD_DOCKED → Display.STATE_ON
初始化/请求 pulse → 依据设备 screen-off 控制能力选择 ON/OFF
```

`DozeScreenState` 接收 transition，必要时延迟应用，然后调用：

```text
DozeMachine.Service.setDozeScreenState(state)
→ DozeService
→ DreamService.setDozeScreenState
```

设备能力不支持某些状态时，Factory 还会包装 preventing adapter，把不可用的 DOZE_SUSPEND 等降级成安全状态。

因此 Machine 状态和 Display 状态不是同一个枚举，也不是一一同名映射。

---

## 16. Doze 亮度链路

`DozeScreenBrightness` 可使用专门传感器档位到亮度数组的映射，也有默认 doze brightness：

```text
Doze state 改变/传感器值
→ 选择 sensor bucket
→ 查 brightness 配置
→ clamp 到用户亮度设置
→ setDozeScreenBrightness(int)
→ DreamService
→ IDreamManager.startDozing(token, state, brightness)
→ DreamManagerService
→ PMS.setDozeOverrideFromDreamManager
→ DisplayPowerRequest.dozeScreenBrightness
→ DPC
```

SystemUI host 还可同步接收亮度，用于 scrim/界面配合。亮度和 screen state 是两个独立 override，但通过 `startDozing` 一起送给服务端。

---

## 17. startDozing：连接 SystemUI 与 PMS 的桥

精确调用链是：

```text
DreamService.startDozing()
→ IDreamManager.startDozing(token, screenState, screenBrightness)
→ DreamManagerService.startDozingInternal(...)
```

DreamManagerService 校验 token/canDoze 后：

```java
mPowerManagerInternal.setDozeOverrideFromDreamManager(
        screenState, screenBrightness);
```

并在第一次真正开始 dozing 时获取 `mDozeWakeLock`。

PMS 下一轮构造：

```text
DisplayPowerRequest.policy = POLICY_DOZE
dozeScreenState = override
dozeScreenBrightness = override
```

随后进入第 68 章的 DPC 状态机。

这里存在两类 WakeLock：

- DreamManagerService 的 Doze WakeLock：表示当前 Dream 正在 dozing，参与 PMS doze 流程；
- SystemUI DozeMachine WakeLock：仅在状态转换或 pulse 等必须运行阶段保持 CPU。

不要把它们当成同一把锁。

停止时则是：

```text
DreamService.stopDozing()
→ IDreamManager.stopDozing(token)
→ DreamManagerService.stopDozingInternal(token)
→ 释放 mDozeWakeLock
→ setDozeOverrideFromDreamManager(STATE_UNKNOWN, BRIGHTNESS_DEFAULT)
```

清空 override 后，PMS 下一次会根据自身 wakefulness 和其他条件重新生成显示请求，旧 Doze state/brightness 不再有效。

---

## 18. AOD pulse 完整链路

以通知到来为例：

```text
通知/传感器触发
→ DozeTriggers 判断当前 state.canPulse()
→ 检查 proximity、host blocking、power save 等
→ requestState(DOZE_REQUEST_PULSE, reason)
→ DozeUi 请求 DozeHost.pulseWhileDozing
→ UI 准备完成
→ DOZE_PULSING（或 BRIGHT）
→ DozeScreenState 设置 Display.STATE_ON
→ DozeMachine 持有 state WakeLock
→ SystemUI 展示通知/时钟
→ pulse timeout/完成
→ DOZE_PULSE_DONE
→ resolve 到 DOZE_AOD 或 DOZE
→ Display 回到 DOZE_SUSPEND 或 OFF
→ 释放 pulse WakeLock
```

pulse 是短暂允许更完整显示管线工作的窗口，不等同于整机完全 wake up。若用户真正解锁/唤醒，Machine 进入 FINISH，PMS 转为 AWAKE。

---

## 19. 近距传感器为什么暂停 AOD

手机放入口袋时持续 AOD 浪费功耗且无意义：

```text
DozeTriggers proximity near
→ DOZE_AOD_PAUSING
→ DozePauser 设置超时
→ DOZE_AOD_PAUSED
→ screenState OFF

proximity far
→ 回到 DOZE_AOD
→ screenState DOZE_SUSPEND
```

这与通话贴耳的 DPC proximity 灭屏有相似传感器，但控制层次不同：

- 通话 proximity：DPC 直接覆盖显示 state；
- AOD proximity：SystemUI DozeMachine 改状态，再通过 Dream override 影响 DPC。

---

## 20. 普通 Dream 的退出与 gentle wake

`stopDream(immediate=false)`：

```text
标记 waking gently
→ IDreamService.wakeUp()
→ DreamService 有机会执行退出动画并 finishSelf
→ 超时仍未结束
→ DREAM_FINISH_TIMEOUT
→ 强制 stop/detach/unbind
```

`immediate=true` 则直接 detach/unbind。停止时还会：

- 移除连接/退出 timeout；
- unlink Binder death；
- 释放启动 WakeLock；
- 发送 dreaming stopped；
- 清除 token/name/canDoze；
- 若正在 dozing，释放 Doze WakeLock 并清除 PMS override。

温和退出不是无限等待，Dream 组件卡住不能阻塞系统唤醒。

---

## 21. 三套状态机如何协作

```text
PMS wakefulness
  AWAKE / DREAMING / DOZING / ASLEEP
              ↓ 决定是否启动/停止 Dream

DreamManager current dream
  token / component / canDoze / isDozing / waking
              ↓ 提供 Service 生命周期和 doze override

SystemUI DozeMachine
  DOZE / AOD / PAUSED / PULSE / FINISH
              ↓ 映射 screenState/brightness

DPC display state
  OFF / DOZE / DOZE_SUSPEND / ON
```

状态改变通常从上到下，但 callback、用户唤醒和失败也会反向触发重新求值。它们不应合并为一个超大枚举。

---

## 22. Device Idle Doze 为什么完全不同

`DeviceIdleController` 关心：

```text
设备静止、未充电、屏幕关闭
→ inactive / sensing / locating / idle / maintenance
→ 网络、Job、Alarm、WakeLock 白名单与限制
```

显示 AOD 关心：

```text
屏幕是否 OFF/DOZE_SUSPEND/短时 ON
时钟、通知 pulse、近距和环境显示
```

二者都为了省电，也会参考屏幕/充电状态，但没有 `DozeMachine.State.DOZE_AOD` 直接驱动 DeviceIdleController 状态。阅读日志时必须看 tag/类名。

---

## 23. 常见误解修正

1. Dream 不只是图片屏保；DozeService 也借用 Dream 生命周期。
2. `requestDream()` 不直接绑定 Service，而是请求 PMS nap 后评估。
3. `canDoze=true` 是系统授予当前 Dream 的能力，不是 Service 自己声明就可信。
4. `startDozing()` 不是把 CPU 永久锁醒，而是提交显示 override 并进入受控 doze。
5. DozeMachine state 与 Display.STATE 不是一套枚举。
6. AOD 设置开启不保证当前一定 AOD，Battery Saver、suppression、proximity、dock 都会改写状态。
7. pulse 不等于完整唤醒，只是低功耗状态中的短时显示窗口。
8. AOD 近距暂停与通话 proximity 灭屏不是同一控制层。
9. Dream gentle wake 仍有超时，不能阻塞系统。
10. 显示 Doze 与 Device Idle Doze 不是同一机制。

---

## 24. 故障排查

### 24.1 普通屏保不启动

```text
设置/当前用户 dream component
→ PMS canDreamLocked 条件
→ nap/wakefulness
→ chooseDreamForUser
→ bindServiceAsUser
→ attach 是否在 timeout 内完成
```

### 24.2 AOD 开关已开但黑屏

```text
ambient display component 是否正确
→ PMS 是否 DOZING/启动 canDoze Dream
→ DozeMachine 当前是否 DOZE_AOD
→ power save/suppressed/docked
→ proximity 是否 PAUSED
→ screenState override 是否送达 PMS
→ DPC 最终 state/brightness
```

### 24.3 通知来了但不 pulse

检查当前 state.canPulse、DozeTriggers 是否收到、通知是否允许、proximity、host blocking、Battery Saver、pulse pending 和超时。

### 24.4 AOD 功耗高

```text
是否长期停在 PULSING/STATE_ON
→ DozeMachine state WakeLock 是否释放
→ time tick/传感器是否过频
→ DOZE_SUSPEND 是否被设备 adapter 降级
→ draw WakeLock 是否长期阻止 suspend
→ 面板是否真正支持低功耗模式
```

### 24.5 唤醒后 Dream 残留

检查 PMS 是否调用 stopDream、gentle wake 是否超时、Service 是否响应 `wakeUp/finishSelf`、Binder 是否断开，以及 token 是否仍是当前 token。

---

## 25. macOS 只读源码练习

1. 从 `requestDreamInternal()` 追到 PMS `nap()`，解释为什么不直接绑定。
2. 在 PMS sandman 路径找 `canDreamLocked/canDozeLocked/startDream`。
3. 比较 `chooseDreamForUser(false)` 与 `chooseDreamForUser(true)`。
4. 从 `startDreamLocked()` 追 token、临时 WakeLock、Controller bind 和 attach。
5. 找连接超时、温和退出超时、Binder death 三类兜底。
6. 从 DozeService `onDreamingStarted()` 追到 DozeMachine INITIALIZED 和 `startDozing()`。
7. 把所有 DozeMachine state 按基础、AOD、暂停、pulse、结束分组。
8. 从 `DozeScreenState.transitionTo()` 追到 PMS doze override 和 DPC。
9. 从 `DozeScreenBrightness` 追传感器 bucket、clamp 和 brightness override。
10. 画通知 pulse 的 REQUEST→PULSING→DONE→基础状态链。
11. 对比 SystemUI DozeMachine 与 DeviceIdleController 的输入和输出。

---

## 26. 推荐阅读顺序

```text
1. PMS sandman + DreamManagerService.startDreamInternal
2. DreamManagerService start/stop + DreamController bind/attach
3. DreamService attach/startDozing/finish
4. DozeService + DozeFactory + DozeMachine states
5. DozeScreenState/DozeScreenBrightness
6. DozeTriggers/DozeUi/DozePauser
7. 回接 PMS DisplayPowerRequest 和 DPC
```

第一次先建立三套状态机，再读具体传感器，否则很容易把同名 DOZE 串错。

---

## 27. 完整心智模型

```text
用户无操作/电源状态
→ PMS 决定 DREAMING 或 DOZING
→ DreamManagerService 选择 component，创建会话 token
→ DreamController bind + attach DreamService

普通 Dream：全屏内容 → gentle/immediate stop

Doze Dream（SystemUI DozeService）
→ DozeMachine
   ├─ AmbientDisplayConfiguration 决定 AOD 基础态
   ├─ Triggers 决定 pulse/近距/广播
   ├─ ScreenState 映射 Display.STATE
   ├─ ScreenBrightness 决定低功耗亮度
   └─ WakeLock 保护 transition/pulse
→ DreamService.startDozing(token,state,brightness)
→ DreamManagerService 校验 token/canDoze
→ PMS doze override
→ DisplayPowerRequest(POLICY_DOZE)
→ DPC → 面板 OFF/DOZE_SUSPEND/短时 ON
```

总结：**Dream 框架负责选择、绑定和监管待机显示 Service；SystemUI DozeMachine 负责 AOD 的 UI、传感器和 pulse 状态；PMS/DPC 负责把合法 override 落实成显示电源状态。**

---

## 28. 自测题

1. 普通 Dream、Doze Dream、AOD 和 Device Idle Doze 有何区别？
2. requestDream 为什么先调用 PMS.nap？
3. Dream token 解决什么问题？
4. DreamController 如何处理连接慢、退出慢和 Binder death？
5. 普通 Dream 为什么不能直接 startDozing？
6. DreamService.startDozing 与 DozeMachine.State.DOZE 有何区别？
7. INITIALIZED 后怎样选择 DOZE 或 DOZE_AOD？
8. Battery Saver 和 proximity 怎样影响 AOD？
9. Machine state 怎样变成 Display state 和 brightness？
10. pulse 为什么不等于完整唤醒？
11. 两类 Doze WakeLock 分别保护什么？
12. gentle wake 为什么仍需要强制超时？

能画出 PMS、DreamManager、DozeMachine、DPC 四层状态图，并讲清 AOD pulse，本章就掌握了。

---

## 29. 下一章预告

```text
70 Android 热管理、ThermalManagerService 与性能降频链路
```

下一章将追踪 Thermal HAL 温度/节流事件、ThermalManagerService、thermal status/headroom、PowerManager thermal shutdown、DisplayModeDirector 刷新率限制以及 CPU/GPU/充电等设备侧降频，分清温度采集、策略通知和真实硬件节流。
