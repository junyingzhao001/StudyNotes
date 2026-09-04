# 155 Android DisplayPowerState、PhotonicModulator 与底层屏幕状态下发

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 67、68、154 章

---

## 1. ready 之后，屏幕到底“完成”到了哪里

第 154 章得到一个容易让人不安的结论：DPC 的 `ready` 不等待 brightness ramp，而 `finished` 也不等于硬件提供了完成凭证。那 `DisplayPowerState.mScreenReady` 究竟确认了什么？

本章沿这一条链继续下钻：

```text
DisplayPowerController
  → DisplayPowerState
  → PhotonicModulator
  → DisplayManagerService.DisplayBlanker
  → LocalDisplayDevice
  → SurfaceControl / LightsService
  → SurfaceFlinger / HWC 或 Lights HAL
```

只回答一个问题：

> 从 DPC 的 state/brightness 目标到 vendor 接口，哪一层只是记下目标，哪一层等待调用返回，哪一层有真实硬件完成证据？

一句话结论：

> DPS 与 DMS 都采用“锁内记最新目标、锁外异步执行”的结构；`mScreenReady` 只对 screen-state 变化等待 DisplayBlanker 调用返回，brightness-only 可以在背光仍在途时 ready。Photonic 的 `actual`、DMS/LocalDisplayDevice 的 state 也都是软件提交账，不是面板回读；r48 主链没有端到端硬件完成 fence。

读完后应能判断“Framework 字段都正确但屏仍黑”为什么不矛盾，并能把故障定位到 power mode、brightness、SurfaceFlinger/HWC 或 Lights HAL。本章不展开自动亮度如何产生目标值，那是第 156 章。

## 2. 先画出两次异步边界

核心文件：

```text
frameworks/base/services/core/java/com/android/server/display/DisplayPowerState.java
frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java
frameworks/base/services/core/java/com/android/server/display/LocalDisplayAdapter.java
frameworks/base/services/core/java/com/android/server/lights/LightsService.java
frameworks/base/core/java/android/view/SurfaceControl.java
frameworks/base/core/jni/android_view_SurfaceControl.cpp
frameworks/native/libs/gui/SurfaceComposerClient.cpp
frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
frameworks/native/services/surfaceflinger/DisplayHardware/HWComposer.cpp
frameworks/native/services/surfaceflinger/DisplayHardware/ComposerHal.cpp
```

两次“先记账、后执行”是主骨架：

```text
DPC power Looper
  └─ DPS写目标并失效，合并Handler任务
       └─ PhotonicModulator独立线程执行DisplayBlanker

DMS requestGlobalDisplayStateInternal
  └─ mSyncRoot内写global/device软件状态并收集Runnable
       └─ mSyncRoot外运行SurfaceControl或Backlight慢调用
```

随后 power mode 经同步 Binder 进入 SurfaceFlinger，并切到 SF 主线程；brightness 则可能走 SurfaceFlinger/HWC，也可能走 Lights HAL。软件同步返回只是某段调用链的完成点，不自动升级成面板物理完成。

## 3. DisplayPowerState 是可失效属性模型

DPS 类注释把自己类比为 View：先修改属性、标记 dirty，再在合适时机统一应用。它维护两组状态：

| 属性组 | 关键字段 | 推进机制 |
|---|---|---|
| screen | `mScreenState`、`mScreenBrightness`、`mScreenReady` | async Handler + Photonic thread |
| color fade | prepared、level、`mColorFadeReady` | Choreographer traversal callback |

构造时它先假设 screen 为 ON、brightness 为最大值并安排更新。注释明确说此时不知道 bootloader 留下的亮度，DPC 会在实际应用前尽快覆盖。初值是软件启动假设，不是硬件读取。

设置 state 只更新目标并失效：

```java
if (mScreenState != state) {
    mScreenState = state;
    mScreenReady = false;
    scheduleScreenUpdate();
}
```

设置 brightness 时，OFF 状态只保存新值，不安排下发；以后开屏会携带最新亮度。`scheduleScreenUpdate()` 也只允许一个 pending Runnable，任务执行时读取最新属性，因此连续中间值可以被合并。

## 4. ColorFade 同时影响画面与有效背光

DPS 提交给 Photonic 的有效亮度不是简单的 `mScreenBrightness`：

```java
float brightnessState = mScreenState != Display.STATE_OFF
        && mColorFadeLevel > 0f
        ? mScreenBrightness
        : PowerManager.BRIGHTNESS_OFF_FLOAT;
```

所以背光变成 off 至少有两条原因：

- screen state 本身是 OFF；
- state 非 OFF，但 ColorFade level 为 0。

第二条支撑了开屏黑幕：面板 power state 可以已 ON，但内容仍被黑色转场覆盖，有效背光也可被压到 off。修改 ColorFade level 时，非 OFF screen 属性会失效；若 fade 已 prepare，还会经 Choreographer 安排新一帧绘制。

`mScreenReady` 与 `mColorFadeReady` 因而是两本账，DPC 的 `waitUntilClean()` 需要两者同时成立。

## 5. PhotonicModulator 的 pending/actual 都是软件账

`PhotonicModulator` 是 DPS 构造时启动的永久 Java Thread，用 `wait()/notifyAll()` 驱动，而不是 HandlerThread。它保存：

```text
mPendingState / mPendingBacklight
mActualState  / mActualBacklight
mStateChangeInProgress
mBacklightChangeInProgress
```

`setState()` 仍只是提交：更新 pending，并用 OR 保留在途标记：

```java
mStateChangeInProgress = stateChanged || mStateChangeInProgress;
mBacklightChangeInProgress = backlightChanged
        || mBacklightChangeInProgress;
```

OR 很重要：旧调用尚未返回时，新目标不能把 in-progress 错误清零。若线程已经工作，也无需再次 `notifyAll()`；它完成当前调用后会重新读取最新 pending。

worker 在真正调用 DisplayBlanker 之前就执行：

```java
mActualState = state;
mActualBacklight = brightnessState;
```

所以这里的 actual 只表示“worker 已取走并准备/正在下发”，不是 vendor 或面板回读。下层若卡住，dump 仍可能显示 pending==actual。

## 6. 中间值为何可以消失

假设 A 正在下发，DPS 又提交 B、C：

```text
Photonic取出A，先把actual=A
  → 锁外调用DisplayBlanker(A)，可能阻塞
DPS把pending改成B，再改成C
  → 没有并行启动第二个worker
A返回后worker重新读取pending
  → 直接取C并下发，B可以从未到达底层
```

这不是丢失最终请求，而是刻意消除过时中间态。显示状态机追求“最终达到最新 level”，不承诺每个瞬态命令都落到硬件。

`mStateChangeInProgress` 只有在 worker 再次持锁、发现 pending state 已等于 actual 时才清除。若 pending 已更新，它会继续下一轮。

同理，`mBacklightChangeInProgress` 记录 worker 是否尚未追上最新背光；但它的完成不会独立回传成 DPC ready 门。

## 7. `mScreenReady` 为什么只等待 state

关键返回值是：

```java
return !mStateChangeInProgress;
```

DPS 的 screen-update Runnable 只有在这个返回 true 时才写 `mScreenReady=true`。因此三种情况是：

| Photonic 状态 | `setState()` | DPS 可否立刻 screen-ready |
|---|---:|---:|
| screen state 仍在途 | false | 否 |
| state 稳定、仅 backlight 在途 | true | 是 |
| 两者都无变化 | true | 是 |

state 变化时，worker 会在 DisplayBlanker 调用返回后的下一轮 post DPS 重检；新的 Runnable 再次调用 `setState()`，确认 state 已追平后才 ready。

brightness-only 的路径更弱：虽然 `setScreenBrightness()` 曾把 `mScreenReady=false`，但 Photonic 只要没有 state change 就会马上返回 true，即使独立线程还没完成背光调用。

所以 `mScreenReady=true` 精确证明的是“没有 DPS 正在等待的 screen-state change”，不证明：

- 最后一笔背光已经写完；
- SurfaceFlinger/HAL 返回了面板 fence；
- 像素已经稳定发光；
- WindowManager 内容已经绘制。

## 8. DisplayBlanker 用不对称顺序协调 PMS

DMS 为 DPC 提供的 DisplayBlanker：

```java
if (state == Display.STATE_OFF) {
    requestGlobalDisplayStateInternal(state, brightness);
}
callbacks.onDisplayStateChange(state);
if (state != Display.STATE_OFF) {
    requestGlobalDisplayStateInternal(state, brightness);
}
```

顺序是：

```text
OFF：先向显示设备下发OFF，再通知PMS
非OFF：先通知PMS，再向显示设备下发目标
```

`onDisplayStateChange()` 与第 154 章的 `onStateChanged()` 不是一回事。前者更新 PMS 的 `MODE_DISPLAY_INACTIVE`；在 legacy 耦合配置下，还按 OFF/非 OFF 调整 HAL interactive 与 autosuspend。后者才是“DPC 有进展，请重算 request”的 ready callback。

这个 legacy 顺序的目标是：关屏先让显示 OFF，再允许相关 suspend 配置变化；开屏则先退出可能的 autosuspend/非 interactive 配置，再操作面板。配置解耦时部分 HAL 操作会跳过，但调用顺序仍保留。

## 9. DMS 在核心锁内只收集工作

`requestGlobalDisplayStateInternal()` 先规范化输入：

- UNKNOWN state 转为 ON；
- OFF 强制 brightness 为 off；
- 非 off 的越界亮度转 invalid 或钳到最大值。

随后使用两层锁：

```text
mTempDisplayStateWorkQueue锁
  ├─ mSyncRoot内：
  │    更新mGlobalDisplayState/brightness
  │    遍历DisplayDevice
  │    调requestDisplayStateLocked并收集Runnable
  └─ mSyncRoot外、仍在workQueue锁内：
       顺序执行全部Runnable
```

设备 power 调用可能耗时数百毫秒，不能长期占用 DMS 的 `mSyncRoot`。临时 work queue 自身的锁则防止多个调用线程同时复用同一个 ArrayList，也把每轮收集与执行串行化。

这里叫 global，是因为 DPC 主要为默认显示计算策略，DMS 却把结果保存为 global state 并遍历所有 DisplayDevice。带 `FLAG_NEVER_BLANK` 的设备会跳过，所以“global”也不表示每个虚拟/逻辑显示无条件受控。

## 10. LocalDisplayDevice 先提交软件 state，再运行硬件动作

`requestDisplayStateLocked()` 在 `mSyncRoot` 内先比较并写入：

```java
final int oldState = mState;
if (stateChanged) {
    mState = state;
    updateDeviceInfoLocked();
}
if (brightnessChanged) {
    mBrightnessState = brightnessState;
}
return new Runnable() { ... };
```

因此 `DisplayDeviceInfo` 可能已经展示目标 state，而返回的 Runnable 尚未执行。`mState` 仍是提交账，不是硬件回读。

只有默认 LocalDisplayDevice 从 LightsManager 取得 `LIGHT_ID_BACKLIGHT`；非默认 local display 的 `mBacklight=null`，brightnessChanged 也因此为 false。全局 brightness 并非由这个对象逐个控制所有外接屏背光。

Runnable 捕获 `oldState`，以便决定安全转换顺序。由于外层 workQueue 锁覆盖 Runnable 执行，后续 global 请求不会与本轮共用临时队列并发穿插。

## 11. suspended、VR 与 Sidekick 决定动作顺序

LocalDisplayDevice 的 Runnable 大体遵循：

```text
必要时先退出旧suspended状态
  → 处理VR mode切换
  → 写brightness
  → 最后进入目标state（可能再次suspend）
```

如果旧状态为 `DOZE_SUSPEND` 或 `ON_SUSPEND`，硬件可能不适合直接改亮度。代码先转到 DOZE 或 ON 等可修改状态，应用 VR/brightness 后，再进入最终 suspended state。

从或向 VR 切换时，即使亮度数值没变，也会在 `setVrMode()` 后重写 brightness，因为 LightsService 的 low-persistence brightness mode 需要随 VR 改变。

更改 power mode 前若 Sidekick 正控制显示，先 `endDisplayControl()` 收回控制权；进入非 OFF 的 suspended state 后，才尝试 `startDisplayControl(state)` 交还。Sidekick 是低功耗显示控制协作者，不是普通亮度通路。

## 12. power state 怎样进入 SurfaceFlinger 和 HWC

Framework state 的映射：

| Display state | SurfaceControl power mode |
|---|---|
| OFF | OFF |
| DOZE | DOZE |
| DOZE_SUSPEND | DOZE_SUSPEND |
| ON_SUSPEND | ON_SUSPEND |
| ON、VR 等其他状态 | NORMAL |

VR 没有单独 HWC power mode，差异主要由 VR/low-persistence brightness mode 等实现。

调用链是：

```text
LocalDisplayDevice.setDisplayState
  → SurfaceControl.setDisplayPowerMode(displayToken, mode)
  → JNI nativeSetDisplayPowerMode
  → SurfaceComposerClient::setDisplayPowerMode
  → ISurfaceComposer Binder
  → SurfaceFlinger::setPowerMode
  → schedule(MAIN_THREAD).wait()
  → setPowerModeInternal
  → HWComposer::setPowerMode
  → Composer HAL / vendor HWC
```

Photonic thread 会同步等待 Binder 和 SF 主线程任务返回。JNI 若整段超过 100 ms 会打印 `Excessive delay in setPowerMode()`。

SF 的状态机还联动合成：OFF 前禁用硬件 VSync、通知 Scheduler released；从 OFF 唤醒时恢复 HWC mode、VSync/Scheduler 并全量 repaint；进入/离开 DOZE_SUSPEND 也会 release/acquire 并重同步 VSync。

## 13. 同步 power-mode 返回仍不是硬件 fence

`SurfaceFlinger::setPowerMode()` 的 `.wait()` 证明 SF 主线程已执行对应函数并调用到 HWC 接口。它不证明：

- 面板电压或背光已经物理稳定；
- 第一帧已完成扫描；
- 用户已看到窗口内容；
- vendor 返回了可传回 DPS 的完成 fence。

而且 `HWComposer::setPowerMode()` 对多种 HWC 错误主要写日志，最后仍返回 `NO_ERROR`。上层同步调用返回不能当作端到端成功事务。

DOZE 还有能力降级：HWC `supportsDoze()` 为 false 时，把传给 vendor 的 DOZE/DOZE_SUSPEND 改为 ON。SurfaceFlinger 自身的软件 DisplayDevice 已在此前记录上层请求 mode，因此 dump 可显示 DOZE，而 vendor 实际收到 ON；排查高功耗必须核对 capability 和 HWC 日志。

## 14. brightness 有 HWC 与 Lights 两条路径

LocalDisplayDevice 可先通过 DisplayDeviceConfig 的两段 spline 做空间转换：

```text
Framework brightness → nits → HAL brightness
```

随后默认屏的 Backlight 对象进入 LightsService。构造时它查询默认 internal display 的 HWC brightness capability，并读取系统最大 brightness setting。

满足以下条件时走新路径：

```text
brightnessMode == USER
AND 非VR low-persistence
AND HWC声明brightness capability
AND maximum screen brightness setting恰为255
```

实际代码用 `mSurfaceControlMaximumBrightness == 255` 表达后两项。源码 TODO 说明 `255` 是 Android 11 迁移期兼容条件，不应外推成永久契约。

新路径：

```text
LightsService
  → SurfaceControl.setDisplayBrightness（返回boolean）
  → ISurfaceComposer同步Binder
  → SF主线程
  → HWComposer future
  → Composer 2.3 setDisplayBrightness
```

Composer client 低于 2.3 会返回 `UNSUPPORTED`。SurfaceControl 能把 status 转成 boolean，但 LightsService 没有检查这个返回值，所以失败不会自动撤回 DPC ready。

不满足条件时走旧路径：float 转 8-bit 灰度 ARGB，再由 `setLightLocked()` 去重，最后优先调用 VINTF AIDL Lights HAL；不存在时通过 JNI 走兼容 HIDL Lights HAL。

因此 `SurfaceControl` 不是 r48 背光的唯一必经路径，看到 `setLight_native` 也不能断言设备一定用了 HIDL 分支。

## 15. `finished`、actual 与错误传播的边界

从第 154 章带下来的公式是：

```text
DPC finished = ready AND brightness RampAnimator未运行
```

它只说明 RampAnimator 不再生成中间亮度。由于 `mScreenReady` 不等待 `mBacklightChangeInProgress`，最后一次 Photonic brightness 调用仍可能在途。

各层字段能证明的最强事实：

| 证据 | 最强可证事实 | 仍不能证明 |
|---|---|---|
| DPS `mScreenReady=true` | 无待等待的 state change | 最后背光已完成 |
| Photonic pending==actual | worker 已取走最新值 | blanker 已返回 |
| LocalDisplayDevice `mState` | 软件目标已提交 | SurfaceControl 已执行 |
| power Binder 返回 | SF 主线程/HWC 调用链已返回 | 面板稳定、首帧可见 |
| DPC `finished=true` | ready 且 ramp 不再动画 | brightness-only 下发或硬件 ACK |

Lights AIDL/HIDL 与 HWC 的一些失败只记日志；新 brightness 路径的 boolean 还被 LightsService 忽略。于是“Framework 状态已经收敛但物理屏异常”完全可能，需要结合 SurfaceFlinger、HWC、Lights、kernel/backlight 或厂商驱动证据。

## 16. 从 dump 和 trace 逐层排查

先看状态关系：

| 表象 | 优先检查 |
|---|---|
| `mScreenReady=false` 长时间不变 | Photonic state in progress、DMS/SF power-mode 调用 |
| Photonic pending≠actual | worker 是否被前一笔 blanker 调用阻塞 |
| pending==actual 但 DPS 未重检 | worker 当前调用、post Runnable、power Handler 拥堵 |
| ready=true 但屏黑 | ColorFade/窗口可见性、power mode/HWC、背光路径 |
| state 正确但亮度错 | 映射 spline、新旧 Backlight 分支、Lights/HWC 日志 |
| DOZE 请求但功耗高 | `supportsDoze()` 与 vendor 是否实际退化到 ON |

关键 dump 字段：

```text
DisplayPowerState:
  mScreenState, mScreenBrightness, mScreenReady,
  mScreenUpdatePending, mColorFadeReady, mColorFadeLevel

PhotonicModulator:
  mPendingState/Backlight, mActualState/Backlight,
  mStateChangeInProgress, mBacklightChangeInProgress

DisplayManagerService / LocalDisplayDevice:
  mGlobalDisplayState/Brightness, mState, mBrightnessState,
  mBacklight, mSidekickActive
```

Trace 可搜索：

```text
requestGlobalDisplayState(...)
setDisplayState(...)
setDisplayBrightness(...)
setLightState(...)
ScreenState / DisplayPowerMode / ScreenBrightness counters
```

静态练习：

```bash
sed -n '300,445p' \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerState.java

sed -n '555,605p' \
  frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java

sed -n '585,760p' \
  frameworks/base/services/core/java/com/android/server/display/LocalDisplayAdapter.java

sed -n '260,340p' \
  frameworks/base/services/core/java/com/android/server/lights/LightsService.java

rg -n "setPowerModeInternal|setDisplayBrightness" \
  frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp \
  frameworks/native/services/surfaceflinger/DisplayHardware
```

分别推演 `ON→OFF`、`DOZE_SUSPEND→ON`、`ON→DOZE_SUSPEND` 和 brightness-only。每一步记录“目标写入、worker 取走、慢调用返回、state ready、brightness 是否仍在途”，就能验证本章最重要的边界：

```text
软件字段名叫actual ≠ 硬件读回
同步函数返回 ≠ 面板物理完成
state ready ≠ backlight ready
最新目标最终收敛 ≠ 每个中间值都执行
```

下一章回到亮度目标的上游，精读 AutomaticBrightnessController 怎样把环境光样本、时间权重、迟滞、debounce、映射曲线和短期用户模型合成为 DPC 使用的亮度值。
