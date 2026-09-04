# 158 Android DisplayWhiteBalanceController：环境色温与显示白平衡

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 155—157 章

---

## 1. 屏幕忽冷忽暖时，先问“哪一层在改颜色”

同一张白色图片，在暖灯和日光下会呈现不同的主观观感。Display White Balance（DWB）尝试估计外界光源色温，再改变显示白点，使“白色”更适应环境。

排查屏幕偏黄、偏蓝或色温跳变时，最容易把四件事混在一起：

- 自动亮度改变 backlight；
- DWB 根据环境色温改变白点；
- Night Display 按用户/时间策略加暖色；
- 第 157 章的内容采样只记录屏幕 HSV Value 直方图。

一句话结论是：

> Android 11 r48 的 DWB 不是把一次色温传感器读数直接写给屏幕，而是“两路传感器 → 两个时间滤波器 → CCT 映射与 lux bias → 节流 → DPC 顺序提交 → 颜色矩阵动画 → SurfaceFlinger”的异步链路。

读完本章，你应能判断一台设备是“功能不可用”“设置开启但未 active”“控制器未 enable”“候选被滤波/节流”，还是“矩阵已请求但没有硬件完成证据”。

本章不展开 Night Display 的日程与用户切换策略；它们放在第 159 章。

---

## 2. DWB 改的是白点，不是背光

先把职责钉死：

| 功能 | 主要输入 | 输出 |
|---|---|---|
| ABC 自动亮度 | 环境 lux、亮度曲线 | brightness/backlight |
| DWB | 环境 CCT + 环境 lux | 全局 4×4 RGB 颜色矩阵 |
| Night Display | 设置、时段、日落 | 暖色 tint 矩阵 |
| BrightnessTracker | 默认 ALS、系统上下文 | 历史事件与统计 |

DWB 使用 lux，不是为了调亮屏幕，而是为了给色温做低光/高光 bias：

```text
低光：颜色传感器噪声可能更大，向固定低光 CCT 靠拢
高光：某些目标白点会压低峰值亮度，向固定高光 CCT 靠拢
```

主源码：

```text
services/core/java/com/android/server/display/
├── DisplayPowerController.java
├── whitebalance/
│   ├── DisplayWhiteBalanceFactory.java
│   ├── DisplayWhiteBalanceSettings.java
│   ├── DisplayWhiteBalanceController.java
│   ├── DisplayWhiteBalanceThrottler.java
│   └── AmbientSensor.java
├── utils/
│   ├── AmbientFilter.java
│   └── RollingBuffer.java
└── color/
    ├── ColorDisplayService.java
    ├── DisplayWhiteBalanceTintController.java
    └── DisplayTransformManager.java
```

---

## 3. “可用、设置、active、屏幕 ON”是四道不同的门

DPC 构造时直接尝试创建设置与控制器：

```java
try {
    settings = new DisplayWhiteBalanceSettings(mContext, mHandler);
    controller = DisplayWhiteBalanceFactory.create(
            mHandler, mSensorManager, resources);
    settings.setCallbacks(this);
    controller.setCallbacks(this);
} catch (Exception e) {
    Slog.e(TAG, "failed to set up display white-balance: " + e);
}
```

失败只让两个字段保持 null，不阻止 DPC 构造。可能原因包括传感器缺失、采样率/滤波资源非法，或数组不匹配。

容易误判的是 `config_displayWhiteBalanceAvailable`。DPC 调 Factory 前没有检查它；该布尔值主要控制 `ColorDisplayService` 是否 setup DWB tint，以及 `ColorDisplayManager.isDisplayWhiteBalanceAvailable()` 的能力报告。

运行期还要通过四层状态：

```text
设备 available / tint setUp
        ↓
Secure setting enabled
        ↓
ColorDisplayService active
        ↓
DPC 当前 display state == ON
```

`DisplayWhiteBalanceSettings` 保存：

```java
public boolean isEnabled() {
    return mEnabled && mActive;
}
```

其中 `mEnabled` 是用户 Secure setting，`mActive` 是 ColorDisplayService 计算出的当前可激活状态。后者要求：

```java
settingEnabled
    && !nightDisplayActivated
    && !accessibilityEnabled
    && DisplayTransformManager.needsLinearColorMatrix()
```

无障碍条件包含反色或 Daltonizer；linear 条件由当前 display color 属性决定。即使用户开关保持 true，Night Display、无障碍颜色变换或不合适的颜色模式都可暂时压住 DWB。

DPC 最后再要求：

```java
if (state == Display.STATE_ON
        && mDisplayWhiteBalanceSettings.isEnabled()) {
    controller.setEnabled(true);
    controller.updateDisplayColorTemperature();
} else {
    controller.setEnabled(false);
}
```

所以 Doze/AOD 不在 r48 的 DWB enable 范围内。

---

## 4. 为什么这条链分属两个线程域

DPC 把自己的 Handler 传给 Factory。两个 AmbientSensor 的回调、两个 filter、Controller 和 Throttler 因而运行在 DPC 所在的 PowerManagerService 专用 ServiceThread Looper。

颜色矩阵则由 `ColorDisplayService` 的 `DisplayThread` Handler 负责：

```text
DPC ServiceThread
  sensor callback
  → estimate
  → pending CCT
  → updateWhiteBalance()
  → sendUpdatePowerState()
  → consume pending
  → ColorDisplayServiceInternal

DisplayThread
  MSG_APPLY_DISPLAY_WHITE_BALANCE
  → cancel/创建动画
  → 每帧 DisplayTransformManager
  → SurfaceFlinger Binder
```

两边都在 system_server 中，通过 `LocalServices` 交接；应用侧 Binder 不是这段内部主链的必经步骤。

这种拆分的价值是让传感器变化先回到 DPC 的 `updatePowerState()` 顺序中提交，再由颜色服务串行处理所有 tint。代价是 CCT、激活状态、动画矩阵之间是 latest-state 异步收敛，不是一笔跨线程原子事务。

---

## 5. 两个环境传感器怎样选，注册成功又怎样判断

Factory 创建两路 Sensor：

| 路径 | 选择方式 | 默认 rate |
|---|---|---|
| ambient brightness | `getDefaultSensor(TYPE_LIGHT)` | 250 ms |
| ambient color temperature | 遍历 `TYPE_ALL`，比较 `getStringType()` | 250 ms |

色温资源虽叫 `config_displayWhiteBalanceColorTemperatureSensorName`，代码比较的不是 `Sensor.getName()`，而是 string type；AOSP 默认值为 `com.google.sensor.color`。

资源 rate 的单位是毫秒，注册时乘 1000 变成 SensorManager 要求的微秒：

```java
mSensorManager.registerListener(
        mListener, mSensor, mRate * 1000, mHandler);
```

DWB 的 lux 也是独立监听。至此显示链中至少有三套容易混淆的光照值：

```text
ABC：可由 OEM 指定 ALS，形成 raw/fast/slow/accepted lux
BrightnessTracker：默认 TYPE_LIGHT 的历史 raw lux
DWB：另一次默认 TYPE_LIGHT 注册，经自己的 AmbientFilter
```

即使后两者选到同一个物理 sensor，它们仍是不同监听器、采样请求和数据结构。

### software enabled 不证明 SensorManager 注册成功

`AmbientSensor.enable()` 先写 `mEnabled = true`，再调用 `registerListener()`，且不检查 boolean 返回值。Controller 也忽略两路 `setEnabled(true)` 的返回值。因此 dump 中 enabled=true 只能证明软件路径发起过注册。

disable 时先把 `mEnabled` 置 false，再 unregister。若已有事件排在 Handler 队列里，`handleNewEvent()` 会看到 false 并丢弃它；这道门防的是注销与已排队回调的竞态。

---

## 6. 两个 10 秒滤波器怎样计算估计值

每一路事件先写入自己的 `AmbientFilter`，随后无条件触发一次总估计：

```java
onAmbientBrightnessChanged(value) {
    brightnessFilter.addValue(now, value);
    updateAmbientColorTemperature();
}

onAmbientColorTemperatureChanged(value) {
    colorTemperatureFilter.addValue(now, value);
    updateAmbientColorTemperature();
}
```

因此不是“色温 sensor 才驱动 CCT 更新”。任一路新事件都会用两只 filter 的当前内容重新计算；刚 enable 时必须等两路至少各有可用值，否则其中一只 estimate 为 `-1`，不会形成候选。

AOSP 默认两只 filter 都保留 10 秒，并采用 `WeightedMovingAverageAmbientFilter`。它不是对样本做简单算术平均，而是把每笔值视为从它到下一笔之间持续有效，权重为：

```text
对 y = x + intercept 的时间积分面积
```

越新的区间位于更大的 x，通常权重越高；intercept 越大，新旧差异越弱。最后一笔还被预测延续到 `now + 100 ms`，避免刚到样本因持续时间为零而几乎没有权重。

`RollingBuffer.truncate(minTime)` 也不简单丢掉全部过期值。它保留 cutoff 前最后一笔值，并把该笔的时间改写为 minTime，以表示窗口左边界仍由它覆盖。

### 这里使用 wall clock，不是 SensorEvent 时间

回调写入 filter 的 timestamp 来自 `System.currentTimeMillis()`，Throttler 也使用同一 wall clock。系统时间向前或向后跳变，会影响：

- horizon 裁剪；
- 积分区间；
- debounce 的 earliest time。

这与 ABC 常用 uptime/elapsed 时间轴不同。

### 输入防御只排除了负数

`AmbientFilter.addValue()` 只有：

```java
if (value < 0.0f) {
    return false;
}
```

`NaN` 与正无穷不会被该条件拒绝。更细的两个后果是：

- NaN 可传播进加权结果、Spline 与 Throttler；Java 把 NaN 强转 int 时得到 0，随后 tint controller 会把它钳到最小 CCT。
- 负值虽未入 buffer，sensor callback 仍继续调用总估计，可能用旧 buffer 在新的 wall-clock 时刻重新算出候选。

这不是建议 OEM 依赖的行为，而是 r48 缺少完整 finite 校验的失败边界。

---

## 7. 从 ambient CCT 到目标 display CCT 的精确顺序

`updateAmbientColorTemperature()` 的流水线是：

```text
color filter estimate
  → 可选 ambient-CCT → display-CCT spline
  → brightness filter estimate
  → 可选 low-light bias
  → 可选 high-light bias
  → debug override
  → throttler
  → pending CCT
```

先映射再 bias 很重要。若 OEM 配置：

```text
ambient 5000 K → display 5400 K
```

low/high bias 混合的是 5400 K 与固定端点，而不是原始 5000 K。

低光公式：

```text
target = bias × mappedCct
       + (1 - bias) × lowLightFixedCct
```

因此 bias=0 完全使用低光固定 CCT，bias=1 完全保留 mapped CCT。

高光公式方向相反：

```text
target = (1 - bias) × previousTarget
       + bias × highLightFixedCct
```

bias=0 不干预，bias=1 完全使用高光固定 CCT。若两条 spline 都存在，low-light 结果会继续进入 high-light 混合；构造函数要求两者定义域不相交，避免同时强烈介入。

### AOSP 默认 low-light 是台阶，不是平滑曲线

r48 默认资源为：

```text
brightnesses = [10, 10]
biases       = [0, 1]
```

`Spline.LinearSpline` 的公开构造器没有验证 x 严格递增；边界判断使结果成为：

```text
lux <= 10 → bias 0
lux > 10  → bias 1
```

也就是 10 lux 处的台阶。默认 high-light 和 ambient→display 数组为空，相关 spline 为 null；而 available 默认又是 false。真正量产行为取决于 OEM overlay。

### spline 校验不等于配置完全安全

Controller 对 low/high spline 只确认 0 lux 插值得到 0、正无穷插值得到 1，并检查两段 domain 不相交。它没有逐项验证中间 bias 都在 `[0,1]`，也没有强制 y 单调。

---

## 8. Throttler 同时设时间门和相对变化门

首个候选在 `mLastTime == -1` 时直接接受；这里甚至没有额外的 finite 校验。之后一个候选只要“太早”或“太近”任一成立就被丢弃：

```java
if (mLastTime != -1 && (tooSoon(value) || tooClose(value))) {
    return true;
}
```

相对阈值以最近一次被接受的值 X 为锚：

```text
升高：new < X × (1 + increaseRatio) 时太近
降低：new > X × (1 - decreaseRatio) 时太近
```

等于阈值时会通过；时间恰好等于 earliest time 也会通过。只有接受后才更新 last value、last time 与下一组上下阈值。

被 throttle 的候选不会保存在“到期队列”里，也没有 Handler deadline。debounce 到期本身不会唤醒重评；必须等下一笔任一路 sensor event。

### r48 有三处“注释/命名不能直接信”的地方

1. Factory 把 `config_displayWhiteBalanceDecreaseDebounce` 读进 increase 变量，把 increase 资源读进 decrease 变量。默认两者都是 5000 ms，所以 AOSP 默认看不出交换；OEM 配成不同值时方向会反。
2. `getHighestIndexBefore()` 实际返回第一个 `base[i] >= value` 的索引，是 ceiling 档，不是名字暗示的 floor 档；配置注释示例按 floor 描述。
3. `isValidMapping()` 检查 x 非负且递增，却只检查 y 非 NaN，没有落实注释所说的 y 非负。负 ratio 或大于 1 的 ratio 都可能通过并产生反向/越界阈值。

调参时应以 r48 实现为准，而不是把资源名和注释当成执行语义。

---

## 9. pending、current、last 解释了开关后的“恢复旧色温”

Controller 维护三个 CCT：

| 字段 | 含义 |
|---|---|
| `mPendingAmbientColorTemperature` | 已通过 Throttler、等待 DPC 消费的候选 |
| `mAmbientColorTemperature` | 本轮当前已提交给 ColorDisplayService 的值 |
| `mLastAmbientColorTemperature` | 最近一次成功走到提交末尾的值 |

Throttler 接受候选后只写 pending，并通过 callback 让 DPC 再跑 `updatePowerState()`。DPC 调 `updateDisplayColorTemperature()` 时：

```text
刚 enable 且 current/pending 都为 -1 → 尝试 last
否则 pending 有效且不同于 current → 消费 pending
都没有 → 返回
```

提交顺序是 current ← candidate、pending 清空、写 50 项诊断 History、把 float CCT 截断成 int 后交给 ColorDisplayService，最后 last ← current。

disable 会：

- 注销两路 sensor；
- 清两只 filter 与 Throttler；
- 把 current/pending 设为 `-1`；
- 要求颜色服务把 DWB CCT reset 到默认值；
- 保留 last 和 History。

因此重新 enable 时可以先恢复 last，再等新传感器数据收敛。这不是 reset 失效，而是 last 被有意保留。

### debug override 并不会因一次 DPC 重跑立即变成候选

`setAmbientColorTemperatureOverride()` 只写字段。DPC 的 shell 路径随后调用 `sendUpdatePowerState()`，但该循环执行的是 `updateDisplayColorTemperature()`，不会重新跑包含 override 的 `updateAmbientColorTemperature()`。

所以在已启用且没有新 sensor event 时，单靠这次重跑通常不会把新 override 变成 pending；仍需后续估计触发。这与附近“let's make it”注释表达的立即意图并不闭合。

---

## 10. CCT 怎样变成可下发的 4×4 颜色矩阵

`DisplayWhiteBalanceTintController.setMatrix(int cct)` 先把输入钳到 OEM 配置的最小/最大 CCT，默认范围为 4000—8000 K。Controller 上游的 float 在调用前直接强转 int，因此小数部分被截断。

矩阵计算顺序：

```text
目标 CCT
  → ColorSpace.cctToXyz()
  → 目标 XYZ 白点
  → Bradford chromatic adaptation
  → adaptation × display RGB-to-XYZ
  → display XYZ-to-RGB × 上一步
  → RGB 空间 3×3
```

显示 RGB 色彩空间优先取 `SurfaceControl.getDisplayNativePrimaries()`；失败才读取 `config_displayWhiteBalanceDisplayPrimaries`。用于 adaptation 的 nominal white 则来自单独的 `config_displayWhiteBalanceDisplayNominalWhite`。

接着计算变换后白色的 R/G/B 响应，并用三者最大值归一化：

```java
denum = max(adaptedMaxR, adaptedMaxG, adaptedMaxB);
result[i] /= denum;
```

目的不是让所有通道都保持 1，而是把峰值通道压到 1，减少超范围/裁剪风险。最后将 3×3 填入 column-major 4×4 identity 的左上颜色部分，alpha/齐次分量保持 identity。

### 计算失败没有上行错误协议

`setMatrix()` 返回 void。它在验证归一化后的系数之前已经：

```text
mCurrentColorTemperature = cct
Matrix.setIdentityM(mMatrixDisplayWhiteBalance)
```

若之后发现 NaN/Infinity，会记录错误并 return，留下 identity 矩阵；上层 `setDisplayWhiteBalanceColorTemperature()` 仍可能因为 tint active 而返回 true、排入应用消息。

因此“LocalService 返回 true”只表示 active 且已安排 apply，不证明目标 CCT 成功算出了非 identity 矩阵。

---

## 11. 3 秒动画怎样接续最新目标

`ColorDisplayServiceInternal.setDisplayWhiteBalanceColorTemperature()` 先同步更新目标矩阵；若 tint 当前 active，再向 DisplayThread 发送 `MSG_APPLY_DISPLAY_WHITE_BALANCE`。

`applyTint()` 会：

1. cancel 旧 animator；
2. 从 `DisplayTransformManager` 读取 level 125 当前矩阵；
3. 读取 tint controller 的最新目标矩阵；
4. 创建 3000 ms、`fast_out_slow_in` 的矩阵插值动画；
5. 每帧更新 level 125；
6. 未取消时在结尾再写一次精确目标。

新目标到来时不是从旧动画原始起点重放，而是从 DTM 当前保存的中间矩阵接续。被 cancel 的旧动画不会在 `onAnimationEnd()` 强写旧终点。

当 tint 变 inactive 时，`getMatrix()` 返回 identity，ColorDisplayService 同样发 apply 消息，于是 level 125 从当前矩阵动画回 identity，而不是立即删除颜色层。

诊断日志还有一个无功能影响的小缺口：`TintValueAnimator` 的 max 数组若用 `Float.MIN_VALUE` 初始化，它代表最小正数，不是最负数；一个系数全程为负时，打印的 max 可能不准确，但实际插值与下发不受此日志统计影响。

---

## 12. level 125 到 SurfaceFlinger，完成语义到哪里为止

`DisplayTransformManager` 按 level 升序保存并合成矩阵：

```text
Night Display       level 100
DWB                 level 125
global saturation   level 150
grayscale           level 200
invert              level 300
```

DWB 与 Night Display 在 ColorDisplayService 策略上互斥，但 DTM 本身仍是通用多层矩阵合成器。每次动画帧的 level 125 变化都会重算当前全部矩阵，然后调用 SurfaceFlinger transaction 1015。

```java
sFlinger.transact(
        SURFACE_FLINGER_TRANSACTION_COLOR_MATRIX,
        data, null, 0);
```

两个边界值得保留：

- `setColorMatrix()` 持有 `mColorMatrix` 锁完成重算并执行 Binder transact，其他颜色矩阵更新会等待这段临界区。
- 代码捕获 `RemoteException`，却不检查 `transact()` 的 boolean 返回值，也没有 SurfaceFlinger 合成完成或面板刷新 fence。

因此一条合理的完成层级是：

```text
候选通过 Throttler
< DPC 已消费 pending
< DisplayThread 已排/推进动画
< Binder transact 已调用返回
< SurfaceFlinger 后续合成与物理显示
```

r48 这条 Java 链只能直接证明前四层中的软件动作，不能把 Binder 返回写成“屏幕已完成刷新”。

---

## 13. 设置 API 返回 true 为什么仍不代表正在生效

`ColorDisplayManager.setDisplayWhiteBalanceEnabled()` 受：

```text
android.permission.CONTROL_DISPLAY_COLOR_TRANSFORMS
```

保护；该权限在 r48 为 `signature|privileged`，不是普通三方应用权限。Binder 服务清除调用身份后写当前用户的 `Secure.DISPLAY_WHITE_BALANCE_ENABLED`。

`isDisplayWhiteBalanceEnabled()` 读取的也是 setting，不是最终 active。源码注释明确说明：enabled 时仍可能因为更高优先级 transform 而不 active。

诊断时至少同时看：

```text
ColorDisplayService:
  available / mSetUp / Activated / current matrix

DisplayWhiteBalanceSettings:
  mEnabled / mActive

DisplayWhiteBalanceController:
  mEnabled
  两路 sensor enabled/events
  两只 filter buffer
  Throttler lastTime/lastValue/threshold
  pending/current/last CCT
```

Controller 和两路 sensor 各自只保存 50 项 `History` 供 dump；这些历史不落盘，重启后要靠新事件重建。另一个噪声点是 `updateDisplayColorTemperature()` 的 “Display cct” 使用无条件 `Slog.d`，即使专用 logging flag 为 false 也可能出现。

---

## 14. OEM overlay 最容易在哪些地方把链路配断

一台设备要真正工作，不能只 overlay `available=true`。至少要一起核对：

| 配置 | 常见失败 |
|---|---|
| color sensor string type | 与 `getStringType()` 不匹配，Factory 抛异常 |
| 两路 rate | 小于等于 0，AmbientSensor 构造失败 |
| filter horizon/intercept | horizon 非正或 intercept 缺失/NaN，Factory 失败 |
| threshold 数组 | 长度不同、base 非递增，Throttler 构造失败 |
| debounce | r48 increase/decrease 资源在 Factory 中交叉读取 |
| low/high bias | 中间值越界仍可能通过有限校验 |
| ambient→display CCT | 数组空表示不映射；非法时捕获并禁用 spline |
| primaries/nominal white | 长度或数值错误可使 tint setup/矩阵计算失败 |
| CCT min/max/default | min≤0 或 max<min 使 tint setup 失败 |
| color mode | 非 linear matrix 模式会令 active=false |

默认配置本身是“功能关闭 + 示例/占位参数”的组合，不能把 AOSP `config.xml` 当成某台量产机的真实曲线。

---

## 15. 从“设置开着但屏幕不变”建立排查树

```text
1. config_displayWhiteBalanceAvailable 为 true？
   否 → ColorDisplayService 不 setup tint
   是
    ↓
2. DPC 的 Settings 与 Controller 是否都构造成功？
   否 → 检查两路 sensor 和资源异常
   是
    ↓
3. setting=true 且 active=true？
   否 → 查 Night Display、a11y、color mode
   是
    ↓
4. display state == ON？
   否 → DPC disable controller
   是
    ↓
5. 两路 sensor 是否真有 event，而非只有 enabled=true？
   否 → 无法形成双 filter estimate
   是
    ↓
6. candidate 是否被 time/ratio Throttler 拒绝？
   是 → 等下一笔 sensor event，不是只等时钟到期
   否
    ↓
7. pending 是否被 DPC 消费并交给 ColorDisplayService？
    ↓
8. tint setUp 是否有效，level 125 是否在动画？
    ↓
9. SurfaceFlinger transact 返回不等于面板刷新完成
```

### 练习一：手算 bias 与阈值

假设 mapped CCT=5000 K、low fixed=6500 K、low bias=0.4、high bias 不启用：

```text
target = 0.4×5000 + 0.6×6500 = 5900 K
```

若最近接受值 X=5900、increase ratio=0.1，则新值低于 6490 K 会被 tooClose；恰好 6490 K 不会因严格小于比较而被该门挡住，但仍要满足 debounce。

### 练习二：证明 debounce 没有到期消息

从 `DisplayWhiteBalanceThrottler.throttle()` 搜索全部调用，再搜索 `postAtTime`、`sendMessageAtTime` 或 Alarm。预期只看到 sensor event 触发重评，没有为 suppressed candidate 保存 deadline 的代码。

### 练习三：追矩阵完成边界

沿：

```text
DisplayWhiteBalanceController.updateDisplayColorTemperature()
→ ColorDisplayServiceInternal.setDisplayWhiteBalanceColorTemperature()
→ MSG_APPLY_DISPLAY_WHITE_BALANCE
→ applyTint()
→ DisplayTransformManager.setColorMatrix(125)
→ transaction 1015
```

记录每层是直接调用、Handler 消息还是 Binder，并标出源码中没有出现的“SurfaceFlinger 已完成合成”和“面板已刷新”确认。

---

## 16. 本章结论

DWB 的唯一主线可以压缩为：

```text
默认 ALS + OEM CCT sensor
→ 各自 wall-clock 加权滤波
→ CCT 映射与 lux bias
→ debounce + 相对阈值
→ pending
→ DPC 在 ON/active 时消费
→ CCT 转 Bradford/RGB 4×4
→ 3 秒 level-125 动画
→ SurfaceFlinger 全局矩阵
```

最后记住七条边界：

1. available、setting、active、DPC state ON 是不同门；
2. DWB 的 lux 是第三套独立采集，不是 ABC 或 Tracker 的 lux；
3. software enabled 不证明 SensorManager 注册成功；
4. filter 与 Throttler 使用 wall clock，NaN/Infinity 防御不完整；
5. r48 debounce 资源方向交叉，threshold 档位实现也与命名/注释不一致；
6. disable 清 current/pending 却保留 last，重启控制器可先恢复旧 CCT；
7. LocalService true、动画完成或 Binder 返回都不是物理显示 fence。

最重要的诊断习惯是：

> 先判断候选有没有形成，再判断 DPC 有没有消费，最后判断矩阵有没有进入合成；不要用“设置是开着的”替代这三层证据。

下一章继续阅读 `ColorDisplayService`：Night Display、颜色模式、矩阵层级和用户切换怎样决定多个颜色策略的互斥与合成。
