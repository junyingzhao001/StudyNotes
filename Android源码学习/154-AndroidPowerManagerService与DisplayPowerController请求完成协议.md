# 154 Android PowerManagerService 与 DisplayPowerController：请求完成协议

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 68、69、152、153 章

---

## 1. `requestPowerState()` 返回 false，为什么不是失败

第 153 章停在这一行：

```java
mDisplayReady = mDisplayManagerInternal.requestPowerState(
        mDisplayPowerRequest, mRequestWaitForNegativeProximity);
```

这里的 `false` 很容易被误读成“设置显示失败”，`true` 又容易被误读成“面板、亮度和窗口都已彻底完成”。两种理解都不对。

本章只回答一个问题：

> PMS 提交一份显示目标后，DisplayPowerController（DPC）怎样异步推进它，并让 PMS 确认“当前最新请求”已经达到 ready？

协议主线是：

```text
PMS按最新全局事实构造request
  → DPC复制为pending快照并立即返回ready状态
  → Power线程异步消费最新快照
  → WindowManager / ColorFade / DisplayPowerState推进
  → DPC带SuspendBlocker通知“请重算”
  → PMS重新构造并再次提交当前请求
  → 直到相同的最新请求返回true
```

一句话结论：

> `false` 表示仍有影响 power-state ready 的异步工作，不是调用失败；`true` 只证明 DPC 对最新请求达到了本章定义的 ready，不保证亮度已到目标、更不等于硬件提供了完成 fence。

读完后应能定位一次亮灭屏握手卡在 pending 消费、开屏 unblock、ColorFade、DisplayPowerState clean、最新请求确认还是回调重算。本章只追到 `DisplayPowerState.waitUntilClean()` 的语义边界，具体下发链留给第 155 章。

## 2. 五个对象和三类执行上下文

核心源码：

```text
frameworks/base/core/java/android/hardware/display/DisplayManagerInternal.java
frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java
frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java
frameworks/base/services/core/java/com/android/server/display/DisplayPowerState.java
frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
```

各对象的职责：

| 对象 | 负责什么 | 不负责什么 |
|---|---|---|
| PMS | 汇总 wakefulness、活动、WakeLock、Doze，生成 request | 不直接驱动面板 |
| DMS LocalService | 在 `mSyncRoot` 下转交 request，提供 DisplayBlanker | 不在提交栈完成亮灭屏 |
| DPC | 保存最新请求，计算 state、亮度、近距与动画 | 不把每次请求当独立事务 |
| DisplayPowerState（DPS） | 保存待应用属性，协调 screen 与 ColorFade clean | 不证明窗口绘制完成 |
| PhoneWindowManager/WMS | 等 Keyguard 和关键窗口准备后回调开屏 | 不决定 PMS wakefulness |

线程关系需要精确描述。PMS 自己创建优先级为 `THREAD_PRIORITY_DISPLAY` 的 `ServiceThread`，并把其 Handler 交给 DMS：

```java
mDisplayManagerInternal.initPowerManagement(
        mDisplayPowerCallbacks, mHandler, sensorManager);
```

DPC 构造时使用 `handler.getLooper()` 创建自己的 async Handler。因此在 r48：

- 同步 `requestPowerState()` 可以来自当前持 PMS 锁的调用线程；
- DPC Handler 与 PMS 的 power Handler 是不同对象，但共享同一个 Power ServiceThread Looper；
- DPS 在 DPC 初始化时用当前 Looper 创建 Handler 和 Choreographer，也在这条 power 线程上推进；
- `PhotonicModulator` 才是专门新建的独立线程；
- WindowManager/Keyguard 的准备又可能跨到其各自 Handler，再回调 DPC。

“同进程、甚至同一个 Looper”都不等于同步完成：消息队列、锁外回调和独立下发线程仍构成明确的异步协议。

## 3. request 是声明式目标，不是一次硬件命令

`DisplayPowerRequest` 一次描述多个维度：

```text
policy: OFF / DOZE / DIM / BRIGHT / VR
useProximitySensor
screenBrightnessOverride / useAutoBrightness
lowPowerMode / screenLowPowerBrightnessFactor
boostScreenBrightness
dozeScreenState / dozeScreenBrightness
```

PMS 每次重算都会重新填写自己长期复用的 `mDisplayPowerRequest`：

```java
mDisplayPowerRequest.policy = getDesiredScreenPolicyLocked();
mDisplayPowerRequest.screenBrightnessOverride = screenBrightnessOverride;
mDisplayPowerRequest.useAutoBrightness = autoBrightness;
mDisplayPowerRequest.useProximitySensor = shouldUseProximitySensorLocked();
mDisplayPowerRequest.boostScreenBrightness = shouldBoostScreenBrightness();
updatePowerRequestFromBatterySaverPolicy(mDisplayPowerRequest);
```

若 policy 为 DOZE，还会加入 DreamManager 提供的 state 和 brightness override。DPC 再把这份“希望显示怎样工作”的声明翻译为 state、亮度、遮罩、近距和策略通知。

字段存在不代表本版真实消费。r48 全树引用显示：

- `blockScreenOn` 只出现在 request 的默认值、复制、比较和打印中，DPC 的同名方法并不读取该字段；
- `screenAutoBrightnessAdjustmentOverride` 同样只在 request 数据类中出现。

所以这两个字段的变化仍会让 request 被判定不同，却没有可证明的 DPC 行为效果。阅读 Framework 字段时，必须从声明继续追到实际 read site。

## 4. 同步提交只建立快照边界

DMS 的 LocalService 没有 Binder 序列化：

```java
synchronized (mSyncRoot) {
    return mDisplayPowerController.requestPowerState(
            request, waitForNegativeProximity);
}
```

DPC 随即在自己的 `mLock` 下复制请求：

```java
if (mPendingRequestLocked == null) {
    mPendingRequestLocked = new DisplayPowerRequest(request);
    changed = true;
} else if (!mPendingRequestLocked.equals(request)) {
    mPendingRequestLocked.copyFrom(request);
    changed = true;
}
```

复制非常关键。PMS 返回后会继续修改同一个工作对象；若 DPC 保存原引用，异步处理可能读到一半旧、一半新的字段。快照把所有权分开：

```text
PMS可变工作对象 ──copyFrom──► DPC最新pending快照
```

`equals()` 决定是否真的产生新 request。它用专门的 `floatEquals()` 把两个 `NaN` 视为相等，因为这里 `NaN` 常表示“无 override”；否则连续提交同一份无覆盖请求会被误判为永远变化。

## 5. pending、active 与消息各是一本账

DPC 有三份容易混淆的状态：

| 字段 | 含义 |
|---|---|
| `mPendingRequestLocked` | 调用方最近提交的最新快照 |
| `mPowerRequest` | Handler 当前正在消费的 active 副本 |
| `mPendingUpdatePowerStateLocked` | 队列中是否已安排一次 update 消息 |

另一个 `mPendingRequestChangedLocked` 表示 pending 内容尚未转入 active。排消息时会合并：

```java
if (!mPendingUpdatePowerStateLocked) {
    mPendingUpdatePowerStateLocked = true;
    mHandler.sendMessage(mHandler.obtainMessage(MSG_UPDATE_POWER_STATE));
}
```

因此连续 A→B→C 不承诺执行三套完整动画。只要 Handler 尚未消费，pending 会被覆盖为 C，而 update 消息仍可能只有一条。这是“对最新目标收敛”，不是逐命令 FIFO。

Handler 开始工作时短暂持 `mLock`：首次创建 active，之后按 `mPendingRequestChangedLocked` 把最新 pending 复制到 active，然后清 flag；较重的传感器、动画和策略工作都在锁外执行。

## 6. 布尔返回值只回答“最新请求 ready 了吗”

`requestPowerState()` 的核心收尾是：

```java
if (changed) {
    mDisplayReadyLocked = false;
}
if (changed && !mPendingRequestChangedLocked) {
    mPendingRequestChangedLocked = true;
    sendUpdatePowerStateLocked();
}
return mDisplayReadyLocked;
```

典型结果：

| 调用场景 | 返回值 | 含义 |
|---|---:|---|
| 首次 request | false | 需要首次异步处理 |
| 已 ready 后再次提交完全相同 request | true | 最新 request 已 ready |
| 上一轮尚未 ready，又提交相同 request | false | 仍在收敛 |
| 提交字段不同的新 request | false | ready 被撤回，需处理新目标 |

还有一个独立变化源：新的 `waitForNegativeProximity=true` 也会令 `changed=true`，即使 request 对象完全相同。

接口注释要求调用方在 false 时持有保护、等待 `onStateChanged()`，再重新请求直到收敛。这里没有“错误码”语义；真正的异常会走 Java 异常或服务故障路径。

## 7. wait-for-negative 是一次性命令，不是持久字段

PMS 提交后立即清掉：

```java
mRequestWaitForNegativeProximity = false;
```

但 DPC 已在锁内把它转移到 `mPendingWaitForNegativeProximityLocked`。Handler 消费时再转为 `mWaitingForNegativeProximity`：

```java
mWaitingForNegativeProximity |= mPendingWaitForNegativeProximityLocked;
mPendingWaitForNegativeProximityLocked = false;
```

这里用 `|=`，说明后续一次 `false` 不会取消已经建立的等待。等待应由 near→far、关闭近距功能或 ignore 状态等 DPC 逻辑结束。

这类“命令位”与 request 字段不同：它只需从生产者可靠移交给消费者一次，不代表长期目标值。

## 8. policy、近距和工作 state 分三步决定

DPC 先把 policy 映射为基础 state：

| policy | 基础 state |
|---|---|
| OFF | `Display.STATE_OFF` |
| DOZE | request 的 doze state；未知时用 `STATE_DOZE` |
| DIM / BRIGHT | `STATE_ON` |
| VR | `STATE_VR` |

随后近距传感器可覆盖它。request 即使仍为 BRIGHT，只要启用 proximity 且确认 near，DPC 会令 `mScreenOffBecauseOfProximity=true`，把目标 state 改为 OFF，并用带 blocker 的 callback 告诉 PMS。

```text
PMS policy=BRIGHT
+ proximity positive
→ DPC工作state=OFF
```

这不是矛盾。近距 OFF 也会绕过普通 `screenTurningOff()/screenTurnedOff()` 生命周期，避免贴耳这种短暂遮挡被 WindowManager 当成真正睡眠。

DPC 把目标交给状态动画后还会重新读取：

```java
animateScreenStateChange(state, ...);
state = mPowerState.getScreenState();
```

第二行得到的是 DPS 当前接受的工作 state，不是物理面板测量值。动画或开屏阻塞可能使它尚未等于最初目标。

## 9. 开屏可见性有一条独立握手

面板进入 ON 时，Keyguard 或应用关键窗口可能还没画好。若 ColorFade level 为 `0.0f`，DPC 会创建一个 `ScreenOnUnblocker`，保留黑色 ColorFade 遮罩，并调用：

```java
mWindowManagerPolicy.screenTurningOn(mPendingScreenOnUnblocker);
```

PhoneWindowManager 等 Keyguard 与窗口绘制；r48 的 keyguard timeout 为 boot 后 1 秒、boot 前 5 秒，`waitForAllWindowsDrawn` timeout 为 1 秒。这些是超时兜底，不是“无论系统怎样卡死都保证回调”。

listener 回来时不直接改 DPC 状态，而是携带自身对象投消息：

```java
if (mPendingScreenOnUnblocker == msg.obj) {
    unblockScreenOn();
    updatePowerState();
}
```

对象身份就是隐式代际 token。旧开屏回调晚到时，只要 pending 已清除或已换成新对象，就不能误解除新一代阻塞。

若开屏时 ColorFade 已非黑，DPC 不创建 blocker，传给 policy 的 listener 可以是 null；不能把每次 `screenTurningOn()` 都理解成一定要等待回调。

## 10. r48 关屏 blocker 并不等待异步回执

关屏代码名字很像对称协议：

```java
blockScreenOff();
mWindowManagerPolicy.screenTurningOff(mPendingScreenOffUnblocker);
unblockScreenOff();
```

但第三行紧接着就清掉 pending。正常 r48 主路径没有等待 `ScreenOffUnblocker.onScreenOff()` 后才继续；迟到回调会因对象身份不匹配而被忽略。

DPC 另有一份 reported-to-policy 状态：

```text
OFF → TURNING_ON → ON → TURNING_OFF → OFF
```

它表达“WindowManagerPolicy 已经收到哪一步”，不是 `Display.STATE_*` 的副本。若 TURNING_OFF 中途反向开屏，DPC 会先补齐 `screenTurnedOff()` 与 reported OFF，再开始 TURNING_ON，维持 policy 生命周期有序。

所以至少要分清：request policy、DPS 工作 state、reported policy state 和用户可见性四张账。

## 11. DisplayPowerState clean 究竟证明什么

DPC 调用 `mPowerState.setScreenState()` 或 `setScreenBrightness()` 只是修改 DPS 属性并安排异步更新。`waitUntilClean()` 的定义是：

```java
if (!mScreenReady || !mColorFadeReady) {
    mCleanListener = listener;
    return false;
}
mCleanListener = null;
return true;
```

两部分含义：

- `mColorFadeReady`：当前 ColorFade 绘制属性已应用；
- `mScreenReady`：对 state 变化而言，PhotonicModulator 已完成对应 `DisplayBlanker` 调用并回到稳定检查点。

`mScreenReady` 不能扩大解释成“真实面板提供了完成 fence”。PhotonicModulator 在锁内先更新自己的 `mActualState`，锁外同步调用 DisplayBlanker；调用返回后才在下一轮唤醒 DPS。底层请求最终代表什么，要继续读第 155 章。

亮度还有更窄的边界：brightness-only 变化会置 `mScreenReady=false`，但 `PhotonicModulator.setState()` 返回值只看 `mStateChangeInProgress`，不看 `mBacklightChangeInProgress`。因此它可能立刻把 `mScreenReady` 恢复为 true，而背光写仍在独立线程执行。

clean listener 只有一个，新 listener 覆盖旧 listener。DPC 不为每次请求保存 Promise；一旦属性 clean，只需重新运行最新状态机。

## 12. ready、finished 和显示完成不是同义词

DPC 的精确定义：

```java
final boolean ready = mPendingScreenOnUnblocker == null
        && (!mColorFadeEnabled
            || (!mColorFadeOnAnimator.isStarted()
                && !mColorFadeOffAnimator.isStarted()))
        && mPowerState.waitUntilClean(mCleanListener);

final boolean finished = ready
        && !mScreenBrightnessRampAnimator.isAnimating();
```

也就是：

```text
ready = 无开屏等待
     AND ColorFade on/off animator未运行
     AND DPS screen/color-fade属性clean

finished = ready AND brightness RampAnimator未运行
```

ready 有意不等 brightness ramp，因为 power state 已可用时，亮度可以继续平滑逼近，没必要拖长 wakefulness 关键路径。

finished 也不能叫“底层显示全部完成”。它只比 ready 多检查 RampAnimator；上一节所述 brightness-only Photonic 下发不在公式里，窗口之外的系统工作更不在其中。

当 ready 且工作 state 非 OFF、reported 状态为 TURNING_ON，DPC 才调用 `screenTurnedOn()`。所以 `screenTurningOn(listener)`、listener 返回、DPC ready 和 `screenTurnedOn()` 是四个相邻但不同的完成点。

## 13. 新 request 怎样阻止旧一代误报 ready

设 Handler 正在锁外处理 active=A，此时 PMS 又提交 B：

```text
1. A从pending复制到active，pendingChanged=false
2. Handler锁外处理A
3. B覆盖pending，pendingChanged=true，lockedReady=false
4. A局部计算达到ready
5. Handler重新持锁，发现pendingChanged=true
6. 不写mDisplayReadyLocked=true
7. 仍可发onStateChanged，提示PMS重检
8. 下一轮把B转成active并继续收敛
```

对应源码门：

```java
if (ready && mustNotify) {
    synchronized (mLock) {
        if (!mPendingRequestChangedLocked) {
            mDisplayReadyLocked = true;
        }
    }
    sendOnStateChangedWithWakelock();
}
```

因此需要区分：

- 局部变量 `ready`：当前 active 这一轮达到公式；
- `mDisplayReadyLocked`：没有更新 pending 时，最新请求才可对调用方返回 true；
- `onStateChanged()`：只表示出现了值得重新检查的进展，不是携带 generation 的完成凭证。

这套设计没有显式 request ID，却用 pending-changed 门防止旧结果覆盖新目标。

## 14. 回调把 PMS 带回 level-triggered 重算

DPC 不把 request 或完成代际作为参数返回给 PMS。它先取得 blocker 引用，把 Runnable 投到 power Looper：

```java
mCallbacks.acquireSuspendBlocker();
mHandler.post(mOnStateChangedRunnable);
```

Runnable 调用 PMS 后再释放引用。PMS 的 callback 只做：

```java
synchronized (mLock) {
    mDirty |= DIRTY_ACTUAL_DISPLAY_POWER_STATE_UPDATED;
    updatePowerStateLocked();
}
```

PMS 于是基于此刻最新 wakefulness、用户活动、WakeLock、Doze 和近距重新生成 request，再读 DPC 返回值。这是 level-triggered reconciliation：事件只负责唤醒重算，当前状态才是真相。

PMS 还计算 `displayBecameReady = mDisplayReady && !oldDisplayReady`，只把 false→true 上升沿交给 Dream 更新。持续 true 不会反复制造 became-ready；但 `mDisplayReady` 本身仍用于 wakefulness finish 与 suspend 判断。

第 153 章的完成门在这里闭合：普通 wakefulness change 等 `mDisplayReady`；`DOZING` 还要同时出现 `WAKE_LOCK_DOZE`。

## 15. 多种未完成理由共享一把引用计数 blocker

旧代码容易让人以为“PMS 有一把 display blocker，DPC 又有一把”。r48 实际映射是：

```java
public void acquireSuspendBlocker() {
    mDisplaySuspendBlocker.acquire();
}
public void releaseSuspendBlocker() {
    mDisplaySuspendBlocker.release();
}
```

DPC callbacks 最终都调用 PMS 创建的同一个 `PowerManagerService.Display` SuspendBlocker。`SuspendBlockerImpl` 内部有 `mReferenceCount`，0→1 才调用 native acquire，1→0 才 native release。

共享对象上可能同时存在多份逻辑引用：

| 持有理由 | 谁记录自己的那一份账 |
|---|---|
| request 尚未 ready、亮屏/Doze 等仍需保护 | PMS 的 `mHoldingDisplaySuspendBlocker` |
| DPC 尚未 `finished`，常见为亮度 ramp | DPC 的 `mUnfinishedBusiness` |
| `onStateChanged` / proximity callback 尚未执行完 | 每次投递前 acquire、Runnable 后 release |
| proximity debounce 尚未结束 | pending debounce 时间建立/清除时 acquire/release |

DPC 的 unfinished 逻辑是：

```java
if (!finished && !mUnfinishedBusiness) acquire();
if (finished && mUnfinishedBusiness) release();
```

PMS 则在 `!mDisplayReady` 时必需自己的引用；BRIGHT/DIM、某些 AOD/近距/boost、Doze 启动窗口也可继续需要。两套布尔账会重叠，但不会互相冒充：共享 blocker 的引用计数让最后一个理由结束后才真正允许 native suspend。

因此 dump 中只看到 blocker ref count，并不能立刻知道是哪一种理由；必须同时看 PMS 与 DPC 状态字段。

## 16. 用两条时间线和一棵树诊断

### 正常唤醒

```text
PMS: ASLEEP→AWAKE，提交BRIGHT，得到false
  → PMS保留display blocker引用，wakefulness仍changing
DPC: pending→active，计算STATE_ON
  → 必要时screenTurningOn(unblocker)，等待关键内容
DPS/Photonic: 推进state与ColorFade clean
Policy: listener回到正确代际
DPC: ready；无新pending才写lockedReady=true；带blocker回调
PMS: 重算并再次提交BRIGHT，得到true；允许wakefulness finish
DPC: brightness ramp可继续；finished后撤销unfinished引用
```

### 中途反转

```text
A=BRIGHT仍在开屏握手
  → B=OFF覆盖pending
  → A即使局部ready也因pendingChanged不能提交lockedReady
  → 旧listener因对象身份不匹配不能解除新状态
  → Handler消费B，并按最新OFF收敛
```

排查 `mDisplayReady=false` 时按顺序看：

```text
1. pending 是否尚未转 active？
   └─ mPendingUpdatePowerStateLocked / mPendingRequestChangedLocked
2. 是否卡在开屏可见性？
   └─ mPendingScreenOnUnblocker、PhoneWindowManager、Keyguard、draw timeout
3. ColorFade animator 是否仍 started？
4. DPS 是否不 clean？
   └─ mScreenReady / mColorFadeReady / Photonic state progress
5. ready 已出现却未提交 latest-ready？
   └─ 是否有更新 pending 覆盖旧 active
6. callback 是否尚未运行？
   └─ power Handler 队列、PMS mLock、display blocker ref count
```

建议同时取证：

```text
dumpsys power:
  mWakefulness, mWakefulnessChanging, mDisplayReady,
  mDisplayPowerRequest, mRequestWaitForNegativeProximity,
  mProximityPositive, mHoldingDisplaySuspendBlocker

dumpsys display:
  mPendingRequestLocked, mPendingRequestChangedLocked,
  mDisplayReadyLocked, mPowerRequest,
  mPendingScreenOnUnblocker, mReportedScreenStateToPolicy,
  mWaitingForNegativeProximity, mScreenOffBecauseOfProximity,
  mUnfinishedBusiness, mScreenReady, mColorFadeReady,
  PhotonicModulator pending/actual与in-progress字段
```

只看一侧可能正好截到不同代际。`ready=true + rampAnimating=true + mUnfinishedBusiness=true` 也可以是健康中间态，而不是卡死。

用下面的静态练习验证主线：

```bash
rg -n "requestPowerState|mPendingRequestLocked|mDisplayReadyLocked" \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java

rg -n "final boolean ready|final boolean finished|mUnfinishedBusiness" \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java

rg -n "waitUntilClean|mScreenReady|mBacklightChangeInProgress" \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerState.java

rg -n "sendOnStateChangedWithWakelock|onStateChanged|mDisplaySuspendBlocker" \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
```

本章最终应记住四个不等式：

```text
request被同步接收 ≠ Handler已处理
DPC局部ready ≠ 最新pending可返回true
requestPowerState返回true ≠ brightness ramp结束
finished ≠ 物理面板提供了硬件完成凭证
```

下一章继续沿 `DisplayPowerState → PhotonicModulator → DisplayBlanker → LocalDisplayDevice → SurfaceControl/Lights` 下钻，核准 screen ready、modulator actual 和底层 power/brightness 调用各自能证明到哪里。
