# 153 Android PowerManagerService：wakefulness 状态机与亮灭屏转换

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 25、68、69、152 章

---

## 1. 为什么“已经 ASLEEP，屏幕却还没灭”不一定矛盾

排查亮灭屏问题时，经常会同时看到这些事实：

- PowerManagerService（PMS）的 wakefulness 已经是 `ASLEEP` 或 `DOZING`；
- `PowerManager.isInteractive()` 已经返回 `false`；
- DisplayPowerController 仍在执行渐变、近距或 AOD 状态切换；
- CPU 仍未进入 suspend；
- `SCREEN_OFF` 广播还没有发完。

它们可以同时成立，因为“设备意图睡眠”不是一个瞬间完成的布尔赋值，而是一组分层状态逐步收敛。

本章只回答一个问题：

> 一个唤醒、超时或睡眠请求，怎样经 wakefulness、用户活动、Dream/Doze、显示请求、通知和 SuspendBlocker，最终完成亮灭屏转换？

一句话结论：

> PMS 先改变设备级 wakefulness，再以 dirty bit 反复计算其他派生状态；只有显示请求 ready，且进入 Doze 时服务已经持有 DOZE WakeLock，才会结束这次 wakefulness transition。raw 状态、interactive、显示 policy、物理屏幕和 CPU suspend 因而不能互相替代。

读完应能解释上述“表面矛盾”，并能判断一次转换停在状态提交、Dream/Doze、Display ready、通知还是 blocker 哪一层。本章不展开 DisplayPowerController 内部如何把请求落实到物理屏幕，那是第 154 章的主线。

## 2. 先拆开六层状态

核心源码位置：

```text
frameworks/base/core/java/android/os/PowerManager.java
frameworks/base/core/java/android/os/PowerManagerInternal.java
frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
frameworks/base/services/core/java/com/android/server/power/Notifier.java
frameworks/base/services/core/java/com/android/server/dreams/DreamManagerService.java
frameworks/base/core/java/android/hardware/display/DisplayManagerInternal.java
```

六层各自回答不同问题：

| 层 | 代表字段或接口 | 回答的问题 |
|---|---|---|
| 设备意图 | `mWakefulnessRaw` | 系统整体处于醒、梦、Doze 还是睡眠 |
| 交互性 | `isInteractive()` | 输入、应用和策略是否按 interactive 对待设备 |
| 显示请求 | `DisplayPowerRequest` | PMS 希望显示进入 OFF、DOZE、DIM、BRIGHT 或 VR |
| 请求完成 | `mDisplayReady` | Display 子系统是否已完成当前请求所需的收敛 |
| 物理显示 | `Display.STATE_*` | 面板此刻实际处于什么状态 |
| CPU suspend | WakeLock summary、SuspendBlocker、HAL | kernel 当前是否允许 suspend |

因此需要记住三组反例：

- `AWAKE` 不保证面板一定发光：近距传感器可让屏幕灭，但设备仍 interactive。
- `DOZING` 不保证面板一定灭：AOD 可请求 `DOZE`，甚至请求 `ON`。
- `ASLEEP` 不保证 CPU 已 suspend：PARTIAL WakeLock、显示收敛或通知广播仍可持有 blocker。

## 3. 四态被折叠成二值 interactive

`PowerManagerInternal` 定义四个设备状态：

```java
WAKEFULNESS_ASLEEP   = 0;
WAKEFULNESS_AWAKE    = 1;
WAKEFULNESS_DREAMING = 2;
WAKEFULNESS_DOZING   = 3;
```

`isInteractive(int wakefulness)` 只把 `AWAKE` 与 `DREAMING` 映射为 `true`：

| wakefulness | interactive | 典型含义 |
|---|---:|---|
| `AWAKE` | true | 正常交互 |
| `DREAMING` | true | 屏保正在运行，但仍属交互态 |
| `DOZING` | false | Ambient Display / Doze 过渡 |
| `ASLEEP` | false | 设备级睡眠 |

这是一种有损折叠。ActivityManager 需要更细粒度，因此 Notifier 会把每次 wakefulness 变化异步告诉它；InputManager、输入法、BatteryStats 和常见 API 更关心 interactive 的二值边沿。

`PowerManager.isInteractive()` 还使用 `PropertyInvalidatedCache`。PMS 改 raw 状态时的顺序是：

```java
mInjector.invalidateIsInteractiveCaches();
mWakefulnessRaw = wakefulness;
mWakefulnessChanging = true;
mDirty |= DIRTY_WAKEFULNESS;
```

先失效缓存、后写 raw 状态，保证锁外查询不会继续命中旧 interactive 值。但此时 `mWakefulnessChanging` 仍为真，所以“API 已返回新值”不代表显示转换已经完成。

## 4. 外部事件先过身份、时间和旧事件门

公开的 Binder 入口包括 `wakeUp()`、`goToSleep()`、`nap()` 和 `userActivity()`。典型入口会：

1. 拒绝晚于当前 uptime 的 `eventTime`；
2. 校验 `DEVICE_POWER` 等权限；
3. 在清除 Binder calling identity 前保存真实调用 UID；
4. 进入带 `mLock` 的 internal/no-update 方法；
5. 状态确实变化后统一调用 `updatePowerStateLocked()`。

这里使用的是 `SystemClock.uptimeMillis()` 时间轴。它与 wall clock 无关，不能拿日志中的日期时间直接比较。

内部还用 `mLastWakeTime`、`mLastSleepTime` 防止旧事件倒灌。例如：

- `wakeUpNoUpdateLocked()` 拒绝早于 `mLastSleepTime` 的唤醒；
- `goToSleepNoUpdateLocked()` 拒绝早于 `mLastWakeTime` 的睡眠；
- `userActivityNoUpdateLocked()` 同时拒绝早于最近 wake 或 sleep 的活动。

所以一次 Binder 调用成功进入 system_server，不等于一定改变状态；“时间合法”也只是第一道门。

## 5. 四个入口分别提交什么状态

可以先用这张最小状态图记住主方向：

```text
                 nap / bedtime with dream
          ┌────────────────────────────────┐
          │                                ▼
ASLEEP ──wakeUp──► AWAKE ◄──dream ends── DREAMING
  ▲                   │                      │
  │                   │ goToSleep / timeout │ bedtime
  │                   ▼                      │
  └──really sleep── DOZING ◄─────────────────┘
          wakeUp ───────┘
```

### `wakeUpNoUpdateLocked()`：统一回到 AWAKE

它拒绝旧事件、已经 `AWAKE`、force suspend 或 system 尚未 ready 的情况。接受后：

```java
mLastWakeTime = eventTime;
mLastWakeReason = reason;
setWakefulnessLocked(WAKEFULNESS_AWAKE, reason, eventTime);
mNotifier.onWakeUp(...);
userActivityNoUpdateLocked(eventTime, USER_ACTIVITY_EVENT_OTHER, 0, reasonUid);
```

最后一步注入用户活动，让新一轮亮、暗、超时计时从唤醒时刻开始。

### `napNoUpdateLocked()`：只允许 AWAKE → DREAMING

它要求 boot/system ready、事件不旧且当前正是 `AWAKE`，然后设置 `mSandmanSummoned` 并提交 `DREAMING`。这一步只是召唤 Sandman，Dream 服务尚未必已经启动成功。

### `goToSleepNoUpdateLocked()`：名字叫 sleep，默认先进入 DOZING

源码注释明确说这是历史命名：

```java
mLastSleepTime = eventTime;
mLastSleepReason = reason;
mSandmanSummoned = true;
mDozeStartInProgress = true;
setWakefulnessLocked(WAKEFULNESS_DOZING, reason, eventTime);
```

默认路径进入 `DOZING`，让 Ambient Display 有机会接管。若带 `GO_TO_SLEEP_FLAG_NO_DOZE`，同一把锁内仍会先真实提交一次 `DOZING`，随后调用 `reallyGoToSleepNoUpdateLocked()` 进入 `ASLEEP`；它不是完全跳过中间状态。

方法会统计此时存在多少 screen-level WakeLock 并写 EventLog，但不会从 `mWakeLocks` 删除这些记录。其效果来自 wakefulness 对 summary 的后续修正，不是“睡眠顺手释放 App 的屏幕锁”。

### `reallyGoToSleepNoUpdateLocked()`：DOZING → ASLEEP

该方法最终调用：

```java
setWakefulnessLocked(
        WAKEFULNESS_ASLEEP,
        PowerManager.GO_TO_SLEEP_REASON_TIMEOUT,
        eventTime);
```

这里传给本次状态通知的 reason 固定为 `TIMEOUT`；此前 `goToSleep()` 保存的 `mLastSleepReason` 并未因此改写。诊断时必须区分“最近睡眠请求原因”和“这一次 raw state edge 携带的 reason”。

另外，`GO_TO_SLEEP_REASON_INATTENTIVE` 常量和字符串映射虽然存在，r48 的 PMS 自动 attentive 睡眠仍传 `TIMEOUT + NO_DOZE`。`QUIESCENT` 也是合法 reason，但 `sleepReasonToString()` 漏了对应 case，日志可能直接显示数字 `10`。

## 6. raw 状态提交与“转换完成”是两个时刻

`setWakefulnessLocked()` 做的是开始阶段：

- 更新 `mWakefulnessRaw`；
- 置 `mWakefulnessChanging=true` 和 `DIRTY_WAKEFULNESS`；
- 离开 `DOZING` 时清掉 `mDozeStartInProgress`；
- 调用 Notifier 的 early 通知；
- 通知 AttentionDetector。

真正完成由 `finishWakefulnessChangeIfNeededLocked()` 判断：

```java
if (mWakefulnessChanging && mDisplayReady) {
    if (wakefulness == WAKEFULNESS_DOZING
            && (mWakeLockSummary & WAKE_LOCK_DOZE) == 0) {
        return;
    }
    mWakefulnessChanging = false;
    mNotifier.onWakefulnessChangeFinished();
}
```

因此完成门是：

```text
Display request ready
AND
(当前不是 DOZING OR 已出现 WAKE_LOCK_DOZE)
```

`DOZING` 多出的第二道门防止 PMS 在 Ambient Display 尚未取得 DOZE WakeLock 时过早撤掉过渡保护。raw 状态先变是设计行为，不是半写入错误。

## 7. 所有输入在七个阶段重新收敛

`updatePowerStateLocked()` 是中心计算函数。它要求持有 `mLock`，无 dirty bit 时直接返回，阶段顺序是：

```text
0  更新供电、stay-on、亮度 boost 等基础事实
1  循环计算 WakeLock summary、用户活动、attentive、wakefulness
2  处理 profile timeout
3  生成并提交 DisplayPowerRequest
4  根据 display ready 调度 Dream/Doze
5  尝试完成 wakefulness 通知
6  最后更新 SuspendBlocker 与 HAL 模式
```

第 1 阶段必须循环，因为 wakefulness 变化又会改变 WakeLock summary 和用户活动 summary。第 6 阶段必须最后执行，因为它可能释放系统手里的最后一把 blocker；更早释放会让 CPU 在状态、显示或通知尚未提交完时 suspend。

Handler 的 user-activity timeout 和 attentive timeout 本身只做两件事：设置相应 dirty bit，再调用中心计算。真正的睡眠决策仍集中在 `updateWakefulnessLocked()`，不会散落在 timeout callback 中直接修改状态。

## 8. 用户活动不是一根 screen-off 定时器

PMS 先从多方限制合成四个时长：

```text
attentive = max(attentive setting, platform minimum)
sleep     = max(min(sleep setting, attentive), platform minimum)
screenOff = max(min(user setting, admin, WM override, sleep, attentive), platform minimum)
dim       = min(maximum dim duration, screenOff × maximum dim ratio)
```

小于等于零的 attentive/sleep setting 表示该层不启用。最终 screen-off 仍不能短于平台 minimum，所以管理员或 WindowManager 给出更小值时也会被向上钳位。

普通用户活动形成的时间线是：

```text
last activity
    │──── SCREEN_BRIGHT ────│── SCREEN_DIM ──│── SCREEN_DREAM ──► bedtime
                            screenOff-dim     screenOff          sleepTimeout
```

若 sleep timeout 未启用，亮/暗结束后 `SCREEN_DREAM` 可无限保留。这里的 `SCREEN_DREAM` 是用户活动 summary，表示已经适合 dream/sleep，并不证明 Dream 服务正在运行。

`USER_ACTIVITY_FLAG_NO_CHANGE_LIGHTS` 使用另一时间戳：只有普通 summary 已归零时，它才按当前 request 的 BRIGHT/VR 或 DIM 保持原亮度层级，而不是重新把暗屏强行提亮。

WindowManager 的 inactive override 会把 bright/dim 强制改为 `SCREEN_DREAM`，并保存原本将发生的 timeout 供统计。下一次被接受到统计阶段的活动会清除此 override。

## 9. userActivity 返回 false 前也可能已经产生副作用

`userActivityNoUpdateLocked()` 的门不是一次性完成。顺序是：

1. 先拒绝旧时间或 system 未 ready；
2. 发送 interaction power hint；
3. 通知 BatteryStats/Policy 和 AttentionDetector；
4. 清除 WindowManager inactive override；
5. 再拒绝 `ASLEEP`、`DOZING` 或 `INDIRECT`；
6. 最后才更新时间戳和 dirty bit。

所以应精确描述：

> 因 ASLEEP、DOZING 或 INDIRECT 被拒绝的活动，可能已经被统计、通知 AttentionDetector，并清除 WM override，但不会刷新 PMS 的 last-user-activity 时间；因 stale 或 system-not-ready 被拒绝的活动则连这些副作用都没有。

这也是为什么只看方法布尔返回值，不能完整推断“系统什么都没做”。

## 10. 普通 keep-awake 与 attentive 硬上限不是同一套门

正常 bedtime 会被以下任一条件阻止：

- 充电保持唤醒 `mStayOn`；
- 近距为 positive；
- `WAKE_LOCK_STAY_AWAKE`；
- 用户活动 summary 仍为 bright/dim；
- 亮度 boost 正在进行。

attentive timeout 到期后，PMS 使用更窄的豁免集合：stay-on、近距、bright/dim 活动或 brightness boost；它故意不包含 `WAKE_LOCK_STAY_AWAKE`。因此 screen-level WakeLock 不能无限突破 attentive 上限。

显示“即将因不专注而睡眠”的 warning 门还更窄，只看 stay-on、boost、近距和 boot 状态，不等同于最终是否允许睡眠。

AttentionDetector 只在当前 summary 为 bright、且没有 `WAKE_LOCK_STAY_AWAKE` 时参与下一次 timeout 计算。检测到注意力后，它用 `USER_ACTIVITY_EVENT_ATTENTION` 回注 PMS，因而会推进 PMS 的 last activity，也会移动 attentive deadline；但 AttentionDetector 自己不会把这种合成事件当成新的真实用户活动，其 `config_attentionMaximumExtension` 会限制连续延长总量。

最后一个关键边界：PARTIAL WakeLock 只贡献 CPU 约束，不进入 `isBeingKeptAwakeLocked()` 的屏幕 bedtime 门。它可以让屏幕熄灭后的工作继续跑，却不能阻止 AWAKE 转向 Dream/Doze/ASLEEP。

## 11. 自动 bedtime 只从 AWAKE 发起

`updateWakefulnessLocked()` 只在当前为 `AWAKE` 且 `isItBedTimeYetLocked()` 成立时自动转换：

```java
if (isAttentiveTimeoutExpired(now)) {
    goToSleepNoUpdateLocked(now, TIMEOUT, NO_DOZE, SYSTEM_UID);
} else if (shouldNapAtBedTimeLocked()) {
    napNoUpdateLocked(now, SYSTEM_UID);
} else {
    goToSleepNoUpdateLocked(now, TIMEOUT, 0, SYSTEM_UID);
}
```

三条路径分别是：

- attentive 到期：`AWAKE → DOZING → ASLEEP`，同锁内使用 `NO_DOZE`；
- 配置允许屏保：`AWAKE → DREAMING`；
- 普通睡眠：`AWAKE → DOZING`，等待 Doze 或最终 sleep。

`DREAMING` 和 `DOZING` 的后续命运由 Sandman 处理，而不是这段自动 bedtime 逻辑直接处理。

## 12. Sandman 为什么必须锁外启动 Dream

PMS 不能持有自己的全局锁调用 DreamManager，否则跨服务回调会放大死锁和长时间占锁风险。因此它把工作投递到 PowerManager Handler：

```text
锁内：快照 wakefulness，判断 summoned + displayReady，清 summoned
锁外：必要时 stopDream → startDream(doze)，再查询 isDreaming
锁内：复核 summoned 和 wakefulness 代际，再决定继续或转换
```

如果锁外调用期间又发生唤醒或睡眠，复核发现 `mSandmanSummoned` 再次置位，或当前 wakefulness 与快照不同，就放弃本轮结论，等待下一次计算。这是异步调用的代际保护。

普通 Dream 的启动门包括：

- 当前为 `DREAMING`，设备支持且设置启用；
- Display request 为 BRIGHT/DIM，而非 VR；
- user activity summary 至少含 bright、dim 或 dream；
- boot 已完成；
- 未被其他 keep-awake 条件保持时，还要满足供电和最低电量配置。

Dream 正常运行且门仍成立就继续；Dream 结束或条件失效后，若仍是 bedtime 则转向 `DOZING/ASLEEP`，否则重新 `wakeUp()` 回 `AWAKE`。电池下降超过配置阈值也可结束普通 Dream。

Doze 的资格门反而很简单：`canDozeLocked()` 只检查当前是否为 `DOZING`。若 Doze dream 未运行或结束，Sandman 调用 `reallyGoToSleepNoUpdateLocked()` 进入 `ASLEEP`。

## 13. Doze 有两道互补握手

进入 `DOZING` 时存在一个危险窗口：PMS 已变成 non-interactive，但 Ambient Display 还没有机会取得自己的 DOZE WakeLock。r48 用两道保护闭合它。

第一道是 PMS 的 `mDozeStartInProgress`。只要处于 `DOZING` 且该标志仍为真，`needDisplaySuspendBlockerLocked()` 就继续持有 display blocker。Sandman 尝试启动或确认不启动 Dream 后才清该标志。

第二道在 DreamManagerService。合法的 doze dream 调用 `startDozing()` 时：

```java
mPowerManagerInternal.setDozeOverrideFromDreamManager(screenState, brightness);
if (!mCurrentDreamIsDozing) {
    mCurrentDreamIsDozing = true;
    mDozeWakeLock.acquire();
}
```

DOZE WakeLock 进入 PMS summary 后，`finishWakefulnessChangeIfNeededLocked()` 才允许完成 `DOZING` transition。停止时 DreamManagerService 先释放 DOZE WakeLock，再把显示 override 清为 unknown/default；dream cleanup 也会兜底释放。

这两道门分工不同：`mDozeStartInProgress` 保护“服务尚未来得及接棒”的短窗口；DOZE WakeLock 表示服务已接棒并为持续 doze 提供约束。

## 14. wakefulness 怎样变成 DisplayPowerRequest

PMS 每轮重新构造显示请求：policy、亮度 override、自动亮度、近距、boost、省电策略，以及 DreamManager 提供的 doze state/brightness。

`getDesiredScreenPolicyLocked()` 的关键优先级是：

```text
ASLEEP 或 boot quiescent                 → OFF
DOZING 且已有 WAKE_LOCK_DOZE             → DOZE
DOZING、无 DOZE bit、dozeAfterScreenOff  → OFF
其他 DOZING 临时情况                     → 继续落到 VR/BRIGHT/DIM 选择
VR                                       → VR
bright WakeLock / bright activity / ...  → BRIGHT
其他                                     → DIM
```

DOZING 尚未出现 DOZE bit、且 `mDozeAfterScreenOff=false` 时，代码故意不立即返回 OFF，而是暂时沿后续分支保留亮/暗请求，以跳过一次先关屏再点亮 AOD 的闪烁。这解释了“wakefulness 已 DOZING，请求却暂时仍 DIM/BRIGHT”。

若 policy 为 DOZE，PMS 填入 DreamManager 的 screen state 和 brightness。持有 `WAKE_LOCK_DRAW` 时，还会把 `DOZE_SUSPEND → DOZE`、`ON_SUSPEND → ON`，除非 Sidekick override 接管；DRAW 的作用是要求可绘制状态，不等于普通 App 能直接控制 AOD。

最后：

```java
mDisplayReady = mDisplayManagerInternal.requestPowerState(
        mDisplayPowerRequest, mRequestWaitForNegativeProximity);
```

这个返回值表示当前请求是否完成收敛，不是“物理屏幕现在为 ON”。OFF、DOZE、近距灭屏都可能对应 ready；请求变化后也可能先返回 false，稍后由 display callback 设置 dirty bit 再算一轮。

## 15. Notifier 把 early、late 与广播顺序分开

`onWakefulnessChangeStarted()` 每次都把完整四态异步通知 ActivityManager。只有 interactive 二值真的翻转时，才处理 InputManager、输入法、BatteryStats 和策略的早晚阶段。

唤醒路径：

```text
early：Input/IME interactive=true
       policy.startedWakingUp()
       排队 ACTION_SCREEN_ON ordered broadcast
display ready 后
late：policy.finishedWakingUp()
```

睡眠路径：

```text
early：Input/IME interactive=false
       policy.startedGoingToSleep()
display ready（Doze 还需 DOZE lock）后
late：取消待处理 user activity
      policy.finishedGoingToSleep()
      排队 ACTION_SCREEN_OFF ordered broadcast
```

若上一次 interactive transition 尚未 late 完成就反向变化，Notifier 会先补做旧方向的 late 行为，再开始新方向，避免状态机永久悬空。

两个 Intent 都带 `REGISTERED_ONLY | FOREGROUND | VISIBLE_TO_INSTANT_APPS`，通过 `sendOrderedBroadcastAsUser(..., UserHandle.ALL, ...)` 发送。它们不是任意 manifest receiver 都能依赖的普通广播。

Notifier 维护“目标 interactive 状态、已经广播的状态、待发 ON/OFF”三份账，以交替补齐有序边沿。整个 ordered-broadcast 序列开始前取得专用 SuspendBlocker，最后一个 receiver 完成并确认没有下一条后才释放。因此 `SCREEN_OFF` 已排队但 CPU 尚未 suspend，可能只是广播完成账还没闭合。

## 16. 用四个场景建立诊断顺序

### 场景一：按电源键后系统已 AWAKE，屏幕仍黑

先不要把 `AWAKE` 当作“面板已经亮”。依次检查：

1. `mWakefulnessChanging` 是否仍为 true；
2. `mDisplayPowerRequest.policy` 是否为 BRIGHT/DIM；
3. `mDisplayReady` 是否为 false；
4. 是否仍在等待 negative proximity；
5. DisplayPowerController 的 request 是否已经收敛；
6. Notifier 是否只完成了 `startedWakingUp`，尚未 `finishedWakingUp`。

### 场景二：超时后停在 DOZING

检查两条接棒链：

```text
mSandmanSummoned → MSG_SANDMAN → DreamManager.startDream(true)
startDozing → doze override → DOZE WakeLock → WAKE_LOCK_DOZE summary
```

若没有 DOZE bit，wakefulness finish 门会继续等待；若 Dream 根本没运行，Sandman 应继续推进到 `ASLEEP`。

### 场景三：屏幕灭了，CPU 还在运行

这并不要求 wakefulness 回到 AWAKE。检查：

- App 或系统的 PARTIAL WakeLock；
- display request 尚未 ready；
- proximity 配置是否要求保留 display blocker；
- `mDozeStartInProgress`；
- AOD 是否明确请求 `Display.STATE_ON`；
- brightness boost；
- Notifier 的 ordered broadcast 是否仍在进行。

### 场景四：拿着 screen WakeLock 仍因超时睡眠

先区分普通 screen-off timeout 和 attentive timeout。普通门包含 `WAKE_LOCK_STAY_AWAKE`；attentive 到期门故意不包含它，并会走 `TIMEOUT + NO_DOZE`。若拿的是 PARTIAL WakeLock，则两种屏幕 bedtime 本来都不受它阻止。

静态源码练习可直接运行：

```bash
rg -n "WAKEFULNESS_|isInteractive" \
  frameworks/base/core/java/android/os/PowerManagerInternal.java

rg -n "wakeUpNoUpdateLocked|goToSleepNoUpdateLocked|napNoUpdateLocked|reallyGoToSleep" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java

rg -n "updatePowerStateLocked|finishWakefulnessChangeIfNeededLocked|handleSandman" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java

rg -n "handleEarlyInteractiveChange|handleLateInteractiveChange|sendNextBroadcast" \
  frameworks/base/services/core/java/com/android/server/power/Notifier.java
```

最终应能自己画出这条因果链：

```text
事件/timeout/WakeLock变化
  → dirty bit
  → wakefulness raw state
  → user activity与WakeLock summary重算
  → DisplayPowerRequest
  → display ready + Doze接棒
  → Notifier late完成
  → 最后更新SuspendBlocker/HAL autosuspend
```

本章最重要的判断原则是：看到“亮屏、灭屏、睡眠”时，必须追问它说的是哪一层，以及该层是刚提交目标、已经 ready，还是物理动作与异步通知都已结束。

下一章进入 PMS 与 DisplayPowerController 的请求完成协议：`requestPowerState()` 为什么能先返回 false，哪些 callback 让 PMS 重算，以及 request ready 与面板实际状态之间还隔着哪些步骤。
