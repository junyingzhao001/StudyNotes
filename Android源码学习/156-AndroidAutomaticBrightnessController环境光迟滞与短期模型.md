# 156 Android AutomaticBrightnessController：环境光、迟滞与短期模型

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 68、154、155 章

---

## 1. 传感器已经报强光，屏幕为什么没有立刻变亮

把自动亮度理解成：

```text
最新lux → 查表 → 背光
```

会误判大量正常中间态。Android 11 的真实链路是：

```text
SensorEvent原始lux
  → 按时间保存并加权
  → 2秒fast + 10秒slow环境估计
  → ambient迟滞 + raw连续越界debounce
  → 接受新的nominal ambient lux
  → OEM曲线 + adjustment + 用户点 + App correction
  → screen-brightness迟滞
  → 回调DPC重算
  → RampAnimator与底层背光
```

本章只回答一个问题：

> 一笔光照变化必须穿过哪些门，才会真正改变 DPC 的目标亮度？

一句话结论：

> AutomaticBrightnessController（ABC）不信任单笔 lux。它要求 fast/slow 加权估计都跨过当前 ambient 阈值，原始样本又连续越界达到 debounce，才接受新的 `mAmbientLux`；映射结果还要跨第二层 screen-brightness 阈值。用户调整、短期模型和前台 App correction 改的是映射曲线，不等于跳过整个 DPC 电源状态机。

读完应能把“没变亮”定位到 enable、传感器、warm-up、加权、环境迟滞、debounce、映射、输出迟滞或 DPC/ramp 中的具体一层。本章不继续追背光实际下发，那已在第 155 章完成。

## 2. ABC 只负责算候选值，DPC 才决定最终来源

核心文件：

```text
frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java
frameworks/base/services/core/java/com/android/server/display/AutomaticBrightnessController.java
frameworks/base/services/core/java/com/android/server/display/BrightnessMappingStrategy.java
frameworks/base/services/core/java/com/android/server/display/HysteresisLevels.java
frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java
frameworks/base/core/java/android/hardware/display/BrightnessConfiguration.java
frameworks/base/core/res/res/values/config.xml
```

职责分层：

| 层 | 决定什么 |
|---|---|
| DPC | 是否允许自动亮度，以及 override、temporary、boost、doze default、dim、low-power 的最终优先级 |
| ABC | 采样、环境估计、两层迟滞、debounce、短期模型生命周期 |
| MappingStrategy | lux 到 normalized brightness 的曲线、gamma、用户点和可选 context correction |
| DMS | 每用户 BrightnessConfiguration 的校验、持久化与当前用户切换 |

DPC 创建 ABC 需要软件自动亮度配置开启且 `BrightnessMappingStrategy.create()` 成功。mapper 会优先创建 Physical 策略，物理映射不完整时才尝试 Simple；两者都无效就把 `mUseSoftwareAutoBrightnessConfig` 关掉。

但“ABC 对象存在”不保证光传感器存在。DPC 先按 `config_displayLightSensorType` 精确找 string type，失败后退到默认 `TYPE_LIGHT`，结果仍可能是 null。

## 3. enable 公式与亮度覆盖顺序不能混读

DPC 的 enable 条件是：

```java
autoBrightnessEnabled = request.useAutoBrightness
        && (state == STATE_ON || allowAutoInDoze && isDozeState(state))
        && Float.isNaN(brightnessState)
        && controller != null;
```

这里的 `brightnessState` 已经受 OFF/VR/WindowManager brightness override 等前置分支影响，但 temporary brightness 与 boost 是在 enable 计算之后才覆盖。

因此要精确区分：

- OFF、VR 或有效 WindowManager override 可让 ABC 本轮不 enable；
- temporary brightness 可以成为当前输出，但 ABC 仍保持采样；
- brightness boost 也故意不关 ALS，源码注释说明这样 boost 结束后可立即恢复自动结果；
- 后续 DIM 与 low-power 只修饰 ABC 的基础结果，不改变 ABC 如何估计 ambient lux。

ABC 目标变化只调用 `mCallbacks.updateBrightness()`；DPC 实现只是排一次 `updatePowerState`。ABC 不直接操作 DisplayPowerState 或背光。

## 4. Doze 的“自动亮度”不等于持续监听 ALS

DPC 可在配置允许且当前 display state 属于 Doze 时令 `autoBrightnessEnabled=true`，但 ABC 的 `configure()` 还看 request policy：

```java
boolean dozing = displayPolicy == POLICY_DOZE;
changed |= setLightSensorEnabled(enable && !dozing);
```

所以进入 `POLICY_DOZE` 后普通 ALS 一定停用。原因是 AP 可能 suspend，非 wake-up light sensor 的事件不可靠。r48 没有在这里切换到 wake-up ALS。

停 sensor 时：

```java
mAmbientLuxValid = !mResetAmbientLuxAfterWarmUpConfig;
mScreenAutoBrightness = BRIGHTNESS_INVALID_FLOAT;
mAmbientLightRingBuffer.clear();
```

若 reset-after-warm-up 为 false，nominal ambient 仍有效，policy 变化会触发重新映射，getter 再乘 `mDozeScaleFactor`；若为 true，ambient 失效，DPC 通常转用 Doze 默认亮度。

因此至少有三件不同的事：允许 Doze 使用自动结果、Doze 中是否有有效旧 ambient、普通 ALS 是否继续注册。只看 `config_allowAutoBrightnessWhileDozing` 不能得出后二者。

## 5. 传感器生命周期的布尔值不是成功证明

启用时 ABC 先写内部状态，再注册：

```java
mLightSensorEnabled = true;
mLightSensorEnableTime = SystemClock.uptimeMillis();
mCurrentLightSensorRate = mInitialLightSensorRate;
registerForegroundAppUpdater();
mSensorManager.registerListener(
        mLightSensorListener, mLightSensor,
        mCurrentLightSensorRate * 1000, mHandler);
return true;
```

资源采样间隔单位为毫秒，SensorManager 接口使用微秒，所以乘 1000。事件通过 ABC 的 async Handler 投到与 DPC 相同的 power Looper；采样时间使用事件到达时的 `uptimeMillis()`，不是 `SensorEvent.timestamp`。

这里有一个诊断陷阱：ABC 忽略 `registerListener()` 的 boolean。传感器为 null 或底层注册失败时，SystemSensorManager 会返回 false，但 ABC 已把 `mLightSensorEnabled=true`，方法也返回 true。于是 dump 中 enabled 只证明 ABC 尝试过注册，不证明 ALS 正在产出事件；还要看 sensor 对象、注册日志和样本计数。

禁用分支完成清理后最终仍 `return false`。这个方法的返回值只被 `configure()` 当作“是否需要额外重算”的 changed bit，不是通用的启停成功状态。

## 6. initial rate、normal rate 与设备 overlay

传感器首次启用使用 initial interval；收到第一笔事件后切到 normal interval：

```java
if (mAmbientLightRingBuffer.size() == 0) {
    adjustLightSensorRate(mNormalLightSensorRate);
}
```

间隔数值越小，期望采样越快。设备可以配置更快的 initial rate，以便亮屏后尽快取得首样本；但 AOSP base 的 initial 为 `-1`，DPC 会直接替换成 normal，所以基线默认二者同为 250 ms。不能把“两阶段”写成所有设备一定先快后慢。

AOSP base 还给出 bright/dark debounce 4000/8000 ms、短期模型 timeout 300000 ms；真实设备 overlay 可全部覆盖。源码学习可用 base 数值举例，真机诊断必须解析最终资源。

## 7. 环形缓冲按持续时间解释样本

每笔事件执行：移除旧复核消息、必要时切 normal rate、prune、push，再立即尝试更新 ambient。ring buffer 保存 uptime 与 lux，并按旧→新排序。

初始容量约为：

```text
ceil(10秒horizon × 1.5 / normal interval)
```

满时容量翻倍，不会因为预估偏小而静默丢最新值。

`prune(horizonStart)` 至少保留一笔边界样本。原因是一些 ALS 只在光照变化时报告：窗口开始前的最后一笔读数，应被理解为持续有效到下一笔，而不是“窗口内没有光”。它会删除更老项，再把跨过 horizon 的最老保留项时间钳到窗口起点。

这是一种零阶保持。若按样本个数做普通平均，一笔持续 8 秒和一笔持续 100 ms 权重相同，会严重偏离真实时间占比。

## 8. fast 与 slow 是带时间权重的两个估计

ABC 用线性权重函数：

```text
y = x + weightingIntercept
integral(x) = x × (x/2 + weightingIntercept)
```

`x` 是相对 now 的负时间，intercept 为 10 秒，因此窗口内越新的时间片权重越大。每笔 lux 乘它所覆盖区间的积分权重，再除以总权重。

最新样本还被假定延续到 now 之后 100 ms：

```java
AMBIENT_LIGHT_PREDICTION_TIME_MILLIS = 100;
```

这给最新值非零权重，并避免只有一笔样本时总权重为零；它不是预测下一笔 lux 数值。

两个固定 horizon：

```text
fast = 最近约2秒的加权lux
slow = 最近约10秒的加权lux
```

fast 表示新环境大致到了哪里，真正被接受时用它写 `mAmbientLux`；slow 用长期证据抑制尖峰。两者都必须跨阈值。

## 9. 首个 nominal ambient 只过 warm-up 门

当 `mAmbientLuxValid=false`，ABC 等到：

```text
mLightSensorEnableTime + mLightSensorWarmUpTimeConfig
```

然后直接用 2 秒窗口建立首值：

```java
setAmbientLux(calculateAmbientLux(time, 2000));
mAmbientLuxValid = true;
updateAutoBrightness(true, false);
```

首次建立不要求常规 bright/dark debounce；warm-up 是独立的初始化门。

但 warm-up 消息只会在至少一笔 SensorEvent 调用 `updateAmbientLux()` 后安排。完全没有事件时，时间到了也不会凭空生成 ambient lux。结合上一节可得：`mLightSensorEnabled=true`、warm-up 已过、`mAmbientLuxValid=false` 可能只是注册失败或没有首样本。

初始化后同一次函数还会继续计算常规 fast/slow 和下次 deadline，不是建立首值后立即永久退出。

## 10. 接受环境变化要同时过三道门

当前 nominal `mAmbientLux` 决定迟滞阈值：

```text
bright threshold = ambient × (1 + bright ratio)
dark threshold   = ambient × (1 - dark ratio)
```

AOSP base 数组给出 10%/20%；设备可按 `thresholdLevels` 为不同 lux 区间提供不同 ratio。数组长度要求 bright 与 dark 相等，且比 threshold levels 多一项。

常规更新的完整门：

```text
增亮：slow >= brightThreshold
  AND fast >= brightThreshold
  AND raw lux连续 > brightThreshold 达到bright debounce

变暗：slow <= darkThreshold
  AND fast <= darkThreshold
  AND raw lux连续 < darkThreshold 达到dark debounce
```

raw 连续性通过从最新样本向旧扫描得到：遇到一笔 `<= brightThreshold` 就中断增亮段；遇到一笔 `>= darkThreshold` 就中断变暗段。于是 raw 恰等阈值会中断连续计时，而加权值恰等阈值允许通过。

三道门分别过滤幅度不足、短时尖峰和长期估计尚未跟上。满足后接受 fast 为新的 nominal ambient，并以新锚点重算下一轮阈值。

## 11. debounce deadline 只是一次重新检查预约

变化型传感器可能在读数稳定后不再发事件。ABC 因而计算：

```text
连续越界最早样本时间 + bright/dark debounce
```

并安排 `MSG_UPDATE_AMBIENT_LUX`。到点后利用零阶保持语义重新计算，即使没有新 SensorEvent 也能完成转换。

若 raw 连续时间已经满足，但 fast/slow 加权值仍未跨门，算出的 deadline 可能早于 now。源码不会 Handler 自旋，而把下一次检查推到：

```java
time + mNormalLightSensorRate
```

新 SensorEvent 到来会先移除旧消息，再根据最新 ring 重新安排。

r48 有一条会误导诊断的日志：接受新值后先 `setAmbientLux(fastAmbientLux)`，再比较 `fastAmbientLux > mAmbientLux`，两者已经相等，所以方向字符串总会落到 `Darkened`。它同时打印的是更新后的阈值，不是刚刚跨过的旧阈值。判定逻辑发生在此前，不受这个日志问题影响。

## 12. Physical 与 Simple mapper 的能力不同

`BrightnessMappingStrategy.create()` 优先选择 Physical：

```text
ambient lux → target nits
panel backlight ↔ nits
```

它先从当前 BrightnessConfiguration 得到 nits，再按此面板的 nits→normalized-backlight spline 转换。同一个 200 nits 目标可以在不同面板得到不同驱动值，也支持动态配置与 nits 回转。

物理信息不完整时，若存在合法的 `lux → backlight int` 数组，就创建 Simple。Simple 直接使用 normalized backlight，不接受动态 BrightnessConfiguration、不支持 nits 转换，也没有 package/category correction。

通用 mapping 校验要求：数组非空且等长；lux 非负并严格递增；输出非负且单调不降；不能含 NaN。`config_autoBrightnessLevels` 的 0 lux 点由代码隐式补出，所以输出数组要多一项。

DMS 对用户/管理员设置的 BrightnessConfiguration 还逐点检查最低亮度 spline；任一点太暗就抛 `IllegalArgumentException`。合法配置按 user serial 持久化，只在当前 user 时下发 DPC。

## 13. adjustment 与用户控制点怎样改曲线

全局 auto-brightness adjustment 位于 `[-1,1]`，不是固定加减值：

```text
gamma = maxGamma ^ (-adjustment)
adjustedBrightness = brightness ^ gamma
```

正 adjustment 通常使曲线更亮，负值更暗。

用户拖动亮度时，DPC 先区分 ABC 自己写回 setting 的回声和外部新值，再把 `userChangedBrightness=true` 交给 ABC。只有 `mAmbientLuxValid` 时才能建立 `(ambientLux, desiredBrightness)`，因为偏好必须绑定当时环境。

mapper 当前只保留一个用户点。加入它时：

1. 求该 lux 的未调整基础 brightness；
2. 反推出接近目标所需的 global gamma adjustment；
3. 对基础曲线应用 gamma；
4. 把用户点精确插入；
5. 向两侧平滑，保持单调并限制斜率；
6. 重建 spline。

所以一个点会影响邻近曲线，不是只改一个离散 lux。

`configure()` 对任何 user-initiated change——brightness 或 auto adjustment——都会令本次 `updateAutoBrightness(..., isManuallySet=true)` 绕过 screen-brightness hysteresis。旧理解若只说“插入用户点时绕过”，会漏掉直接调整 adjustment 的路径。

## 14. 短期模型 timeout 是待复核标志，不是停用开关

写入用户点后：

```java
mShortTermModelValid = true;
mShortTermModelAnchor = mAmbientLux;
```

anchor 是用户表达偏好时的环境 lux，不是目标 brightness。display policy 从 BRIGHT/DIM/VR 这类 interactive 转成 OFF/DOZE 时，才开始 short-term timeout；在 timeout 前回到 interactive 会移除消息。

timeout 回调只做：

```java
mShortTermModelValid = false;
```

它没有清 mapper 的用户点，mapper 求亮度时也不检查这个 flag。因此 invalid 并不表示偏好停止生效；它表示“下一次 accepted ambient 到来时需要复核”。

`setAmbientLux()` 随后比较 anchor 区间。默认上下 multiplier 均为 0.6：

```text
保留区间：(anchor × 0.4, anchor × 1.6]
```

落在区间内重新标 valid；越界才 `resetShortTermModel()`，清用户点、把由该点推断的 adjustment 归零并将 anchor 设为 -1。anchor=0 时区间为 `(0,0]`，没有任何值满足，下一次复核必然 reset。

若 invalidation 已经执行，快速重新点亮只会移除尚存消息，不能把 flag 自动改回 true；用户点仍继续生效，直到下一次 accepted ambient 完成复核。配置变化或 user switch 则立即 reset。

## 15. 前台 App correction 与输出迟滞是最后两道门

Physical 策略可从 BrightnessConfiguration 读取 package-name correction；没有 package 命中时再查 app category。只要存在用户点，代码完全跳过 package/category correction，优先尊重用户刚表达的偏好。

ALS 启用时 ABC 注册 TaskStackListener。任务变化先通知 ABC Handler，再把 `getFocusedStackInfo()` 和应用 category 查询投到 BackgroundThread，结果回到 power Handler 应用并重算。这样避免竞争 ATMS 锁时阻塞 DPC Looper。查询失败或 top activity 为空时会保留旧 context，不能把 correction 当成强一致的前台真相。

mapper 得到候选 brightness 后，ABC 还有第二层迟滞：

```java
if (old不是NaN && !isManuallySet
        && new > screenDarkeningThreshold
        && new < screenBrighteningThreshold) {
    return;
}
```

也就是只有落在严格开区间内部才忽略；恰等 dark/bright threshold 会接受。ambient 迟滞过滤输入噪声，screen 迟滞过滤映射后肉眼意义很小的输出变化。

真正变化后 ABC 回调 DPC。DPC 获取自动值，写回尚未应用 DIM/low-power 的基础 brightness setting，再施加这些 modifier 并交给 RampAnimator。写 setting 前先同步更新 `mCurrentScreenBrightnessSetting`，ContentObserver 回来看到同值就不把它误判为用户输入。

环境自适应只有在上一轮已应用自动亮度且 adjustment 本轮未变时才倾向 slow ramp；首次启用、用户 adjustment、DIM/low-power 边沿等会强制更快。ambient debounce、screen hysteresis 与 ramp 是三种职责完全不同的“慢”。

## 16. 用四层证据诊断“自动亮度没反应”

先分层，不要只看 `mLastObservedLux`：

```text
1. 资格与采样
   ├─ mapper/ABC是否存在
   ├─ request.useAutoBrightness与display state/policy
   ├─ override是否在enable前挡住ABC
   ├─ mLightSensorEnabled是否只是尝试注册
   └─ sensor对象、register日志、mRecentLightSamples是否有证据

2. ambient接受
   ├─ warm-up与mAmbientLuxValid
   ├─ ring buffer时间覆盖
   ├─ fast/slow是否都跨阈值
   └─ raw连续越界是否达到bright/dark debounce

3. 映射与输出
   ├─ Physical/Simple、当前config与spline
   ├─ adjustment、userLux/userBrightness
   ├─ foreground package/category correction
   └─ 候选是否仍在screen hysteresis内

4. DPC与背光
   ├─ updateBrightness callback是否排队
   ├─ temporary/boost/doze default/dim/low-power谁赢
   ├─ slow/fast ramp
   └─ 第155章的state/backlight下发链
```

一个从暗室走到室外的正确推演是：raw 先跳高；fast 很快上升但 slow 仍追赶；两者跨阈值后，还要 raw 连续高于阈值满 bright debounce；接受 fast 为新 ambient 后，mapper 输出仍可能落在 screen hysteresis 内；真正跨出后 DPC 才渐变。

短暂手遮传感器通常会被 slow 窗口、dark debounce 或后续亮样本中断，不能只用“一笔 raw 很低”断言应该变暗。

用户在 50 lux 把基础 0.25 拖到 0.40，则创建 anchor=50 的控制点、推断 gamma 并立即绕过输出迟滞。默认保留区间是 `(20,80]`；timeout 后这个点仍生效，下一次 accepted ambient 落在区间内就重新 valid，落到 500 lux 才真正清除。

静态验证命令：

```bash
rg -n "autoBrightnessEnabled|configure\(" \
  frameworks/base/services/core/java/com/android/server/display/DisplayPowerController.java

rg -n "setLightSensorEnabled|updateAmbientLux|calculateAmbientLux" \
  frameworks/base/services/core/java/com/android/server/display/AutomaticBrightnessController.java

rg -n "nextAmbientLightBrighteningTransition|nextAmbientLightDarkeningTransition" \
  frameworks/base/services/core/java/com/android/server/display/AutomaticBrightnessController.java

rg -n "addUserDataPoint|shouldResetShortTermModel|class PhysicalMappingStrategy|class SimpleMappingStrategy" \
  frameworks/base/services/core/java/com/android/server/display/BrightnessMappingStrategy.java
```

最终必须能区分五个量：

| 量 | 含义 |
|---|---|
| raw lux | 单笔传感器读数 |
| fast lux | 2 秒加权候选 |
| slow lux | 10 秒长期证据 |
| `mAmbientLux` | 已穿过 ambient 迟滞与 debounce 的 nominal 环境值 |
| `mScreenAutoBrightness` | 曲线、用户/context 修正与 screen 迟滞后的 ABC 输出 |

下一章继续追 `BrightnessTracker`：自动亮度之外，系统如何采集用户亮度事件、环境样本、电池与前台应用上下文，并把它们保存为训练和诊断数据。
