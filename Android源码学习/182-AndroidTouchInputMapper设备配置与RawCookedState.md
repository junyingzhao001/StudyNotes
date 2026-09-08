# 182 Android TouchInputMapper：设备配置与 Raw/Cooked State

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`，`frameworks/base` 提交 `1d9b9ab57d`  
> 学习方式：macOS 静态只读，不要求编译、不要求连接设备  
> 前置章节：第 20、173、179、181 章

---

## 1. 先抓住故障现场：触点为什么会偏、转错、消失或从 MOVE 开始

触摸故障表面上常像一条公式写错：

- raw 坐标正常，App 中触点却整体偏移或旋转错误；
- 外接屏拔插后设备还在，MotionEvent 却消失；
- 打开或关闭触控板手势后，source、display 与路由突然变化；
- 重配后 Dispatcher 已取消旧流，Reader 下一笔却仍可能从 MOVE 开始；
- 蓝牙笔接入时首笔触摸延迟，压力更新又可能晚于位置。

真正的数据链不只是“ABS_X/Y 乘比例”：

```text
capability + input property + IDC
  → deviceType / gestureMode
  → runtime mode / source / viewport
  → natural surface 与 motion ranges
  → SYN_REPORT 形成 RawState
  → 外接笔融合门
  → affine + scale + rotation 形成 CookedState
  → direct / unscaled / navigation / pointer 分发
```

本章建立这条骨架，重点回答两件事：设备配置怎样定义坐标契约，Raw/Cooked 与 pending/current/last 为什么必须分账。多点 slot、trackingId 和稳定 pointer id 留到第 183 章，触控板手势状态机留到第 184 章。

读完后应能从 dump、IDC、viewport 和一包 ABS 事件判断问题停在“设备分类、surface 建立、cook，还是状态提交”，也能识别 r48 重配路径里几个不完整的 reset/generation 边界。

---

## 2. 源码地图与四层空间：先别急着把 raw 当屏幕坐标

核心文件：

```text
frameworks/native/services/inputflinger/reader/mapper/
├── TouchInputMapper.cpp/.h
├── SingleTouchInputMapper.cpp/.h
├── MultiTouchInputMapper.cpp/.h
└── accumulator/
    ├── SingleTouchMotionAccumulator.*
    ├── TouchButtonAccumulator.*
    ├── CursorButtonAccumulator.*
    └── CursorScrollAccumulator.*

frameworks/native/services/inputflinger/reader/
├── EventHub.cpp
├── InputDevice.cpp
└── InputReader.cpp

frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
frameworks/native/services/inputflinger/include/InputReaderBase.h
```

一次触摸至少跨四层空间：

| 空间 | 代表什么 | 典型证据 |
|---|---|---|
| raw axis | 驱动整数范围，min/max 都含端点 | `RawPointerAxes` |
| natural surface | Mapper 把 viewport 还原到设备自然方向后的映射面 | `mRawSurface*`、`mSurface*` |
| Reader cooked | affine、缩放、平移、旋转后的显示/设备逻辑坐标 | `PointerCoords` |
| App | Dispatcher 再按目标窗口 transform 后的坐标 | `MotionEvent.getX/Y()` |

DIRECT/POINTER 的 cooked X/Y 通常先处在显示逻辑空间；App 看到的 X/Y 通常已经是窗口局部坐标。UNSCALED/NAVIGATION 则主要处在设备自身的零基坐标面。还要特别区分：App 的 `MotionEvent.getRawX/Y()` 表示窗口变换前的 Motion 坐标，也不是 Linux `EV_ABS` raw。不能用 App 截图上的点直接反推 raw axis，也不能把 `DisplayViewport.logical*`、`physical*` 与 `deviceWidth/Height` 当成同一个矩形。

---

## 3. EventHub 先选协议子类，父类再统一配置与 cook

EventHub 对 multi-touch 的主要判据是同时存在：

```text
ABS_MT_POSITION_X + ABS_MT_POSITION_Y
```

并用 `BTN_TOUCH` 或“不是 gamepad buttons”排除一部分伪装成 ABS_MT 的手柄。旧式 single-touch 则要求：

```text
BTN_TOUCH + ABS_X + ABS_Y
```

`InputDevice` 优先为 `INPUT_DEVICE_CLASS_TOUCH_MT` 创建 `MultiTouchInputMapper`，否则为 touch class 创建 `SingleTouchInputMapper`。

这两个子类不是两套完整触摸栈：

| 职责 | Single | Multi | 公共父类 |
|---|---|---|---|
| 读取 axis 能力 | ABS_X/Y 等 | ABS_MT_* 等 | 保存到 `RawPointerAxes` |
| 收集协议状态 | 单点 accumulator | slot / Protocol A/B accumulator | — |
| 构造本包 raw pointer | `syncTouch()` | `syncTouch()` | 放进同一种 `RawState` |
| viewport、校准、cook、分发 | — | — | `TouchInputMapper` |

Single 始终用 pointer id 0；Multi 优先用 trackingId 维持 id，失败时再让父类按空间关系分配。这里只需抓住接口：子类在 SYN 边界把协议状态投影成统一 `RawPointerData`，后面的坐标与分发才可复用。

绝对轴 accumulator 是持续状态，不会像第 181 章的 REL_X/Y 一样每个 SYN 后清零；一个新 packet 未重报的绝对字段通常沿用前值。

---

## 4. 首次配置与热更新分工：IDC 和 axis 不是每次都重读

`TouchInputMapper::configure()` 每次先保存最新 `InputReaderConfiguration`，但只有 `changes == 0` 才执行：

```text
configureParameters()
configure scroll / touch-button accumulators
configureRawPointerAxes()
parseCalibration()
resolveCalibration()
```

所以 input property、`touch.deviceType`、`touch.gestureMode`、axis min/max 和大部分 IDC 校准在这个 Mapper 生命周期内按设备定义处理。要改变它们，通常需要 EventHub reopen / Mapper 重建，而不是普通配置刷新。

可热更新的分支更窄：

| change | 动作 | 自动 Mapper reset？ |
|---|---|---|
| `CHANGE_TOUCH_AFFINE_TRANSFORMATION` | 重新向 policy 取 affine | 否 |
| `CHANGE_POINTER_SPEED` | 重设 pointer 与两只 wheel VelocityControl | 否，但控制器历史被 `setParameters()` 清掉 |
| display / gesture enablement / showTouches / external stylus presence | 再跑 `configureSurface()` | 仅可能发 reset **通知**，不直接调用 Mapper `reset()` |

首次配置结束后，InputReader 的设备加入流程会统一调用真正的 `InputDevice::reset()`。因此“首次 `configureSurface()` 没显式 NotifyDeviceReset”不是漏掉初始化；它和动态重配是两条生命周期。

affine 热更新尤其值得警惕：代码只换矩阵，不 bump generation、不发 reset 通知。正在进行的同一触摸流可在下一样本直接使用新矩阵，坐标发生跳变却仍保留原 action 序列。

---

## 5. deviceType、runtime mode 与 source 是三份不同事实

`configureParameters()` 的默认 deviceType 顺序是：

```text
INPUT_PROP_DIRECT                → TOUCH_SCREEN
否则 INPUT_PROP_POINTER         → POINTER
否则存在 REL_X 或 REL_Y         → TOUCH_PAD
否则                            → POINTER
```

注意源码检查 REL_X **或** REL_Y，不要求两轴同时存在。IDC `touch.deviceType` 可在首次配置覆盖为 `touchScreen`、`touchPad`、`touchNavigation` 或 `pointer`。

`INPUT_PROP_SEMI_MT` 默认只把 gestureMode 设为 single-touch presentation；IDC 可改为 `single-touch` / `multi-touch`。它不会把 MultiTouchInputMapper 换成 SingleTouchInputMapper，协议子类早已由 EventHub capability 决定。

运行 mode/source 再结合全局配置计算：

| 条件 | mode | 基础 source |
|---|---|---|
| type=POINTER 且 gestures enabled | POINTER | MOUSE |
| type=TOUCH_SCREEN 且有关联显示 | DIRECT | TOUCHSCREEN |
| type=TOUCH_NAVIGATION | NAVIGATION | TOUCH_NAVIGATION |
| 其余 | UNSCALED | TOUCHPAD |

POINTER 可 OR 上内置 STYLUS，DIRECT 可 OR 上内置 STYLUS 与已连接的 BLUETOOTH_STYLUS。因此 source 是 bit 集合：查询“是否具备某能力”时通常看 mask，判断一条实现专门保留的精确 source 语义时仍可能需要等值比较；两种问题不能混用。

最后还要通过 raw X/Y 和 viewport 两道门；任一失败会把 mode 改为 DISABLED。此时有一个反直觉边界：`mSource` 已在前面按候选 mode 写好，早退只改 `mDeviceMode`。所以 disabled Mapper 的 `getSources()` 仍可能返回 MOUSE 或 TOUCHSCREEN；基类仍把该 source 加进设备信息，只是 `populateDeviceInfo()` 不再添加 motion ranges，输入也在 pending drain 处被丢弃。“source 由最终 mode 决定”并不成立。

---

## 6. 显示关联与 viewport 选择：有指定项时，失败未必继续回退

默认 `orientationAware` 只对 TOUCH_SCREEN 为 true，IDC 可覆盖。以下任一条件会令 `hasAssociatedDisplay=true`：

- orientationAware；
- deviceType 是 TOUCH_SCREEN 或 POINTER；
- InputDevice 通过 input port 建立了 display-port 关联。

`touch.displayId` 只在首次参数解析且 deviceType=TOUCH_SCREEN 时读入；pointer/touchPad/navigation 即使写了该 IDC 项，也不会进入这条赋值。有关联显示时，`findViewport()` 的实际顺序是：

1. 有 associated display port：直接返回该 port 的 viewport，空也不继续；
2. 当前 mode=POINTER：尝试 `defaultPointerDisplayId`，失败会继续；
3. IDC 指定 `touch.displayId` unique id：直接返回查询结果，空也不继续；
4. 外接 touchscreen 找 EXTERNAL，其他找 INTERNAL；
5. 只有 EXTERNAL type 找不到时，才回退 INTERNAL。

无关联显示则构造 non-display viewport，宽高来自 raw axis。它通常带 `ADISPLAY_ID_NONE`，并不因为“没有物理屏幕”自动 disabled。

`getAssociatedDisplayId()` 也按 mode 分叉：POINTER 从共享 PointerController 取 displayId，其他关联模式返回 `mViewport.displayId`；无关联返回 `nullopt`。实际 NotifyMotion 的 pointer 用法同样取 controller display，direct/unscaled 等使用 viewport 语义。

端口关联不是 EventHub 静态写进 Mapper 的属性，而是 InputDevice 用设备 `identifier.location` 匹配本轮 `portAssociations` 的结果。端口找不到 viewport 还有外层保护：`InputDevice::configure()` 会 `setEnabled(false)`，其 enable/disable 路径执行真正 reset、NotifyDeviceReset 和 generation bump。因此后文所说的 Mapper disabled 早退“漏收尾”，不能机械套到 port 缺失；unique-id/type/default-pointer 最终失败而外层仍 enabled 时，才更直接暴露 Mapper 自身边界。

另一个动态边界是：`hasAssociatedDisplay` 只在首次 `configureParameters()` 计算。一个原本不关联显示的 touchPad/navigation 若运行中才新增 port association，InputDevice 的 port 字段会更新，Mapper 这枚布尔值却不会随 `CHANGE_DISPLAY_INFO` 重算。

---

## 7. viewport 怎样还原为 natural surface

`DisplayViewport` 同时描述：

```text
logical rectangle   Framework 内容区域
physical rectangle  logical 内容在面板上的落点
deviceWidth/Height  完整显示设备尺寸
orientation         当前方向相对自然方向的旋转
```

raw touch sensor 绑定设备自然方向，viewport 却按当前显示方向给出。DIRECT/POINTER 因而先按 0/90/180/270 还原 natural logical、physical 和 device 尺寸，再计算：

```text
rawSurfaceWidth  = naturalLogicalWidth  * naturalDeviceWidth  / naturalPhysicalWidth
rawSurfaceHeight = naturalLogicalHeight * naturalDeviceHeight / naturalPhysicalHeight

surfaceLeft = naturalPhysicalLeft * naturalLogicalWidth / naturalPhysicalWidth
surfaceTop  = naturalPhysicalTop  * naturalLogicalHeight / naturalPhysicalHeight
surfaceRight  = surfaceLeft + naturalLogicalWidth
surfaceBottom = surfaceTop  + naturalLogicalHeight
```

这里使用整数运算，会发生截断。physical 宽或高为 0 时，源码记录错误并用 1 代替以避免除零；这是继续运行的容错，不是 viewport 已可信。

若 mode 是 UNSCALED/NAVIGATION，正常进入该分支时 geometry 使用 raw 宽高、left/top=0、orientation=0。`UNSCALED` 仍会在 cook 时减 raw min，所以它保存的是设备尺度，不是原始整数的绝对偏移。

r48 这段非 DIRECT/POINTER 分支没有给 `mSurfaceRight/mSurfaceBottom` 赋值，构造函数也未初始化它们；但 `consumeRawTouches()` 会在各 mode 的初始 touching、正式 cook 之前调用 `isPointInsideSurface()`，后者读取 right/bottom。于是 UNSCALED/NAVIGATION 的屏内判断可能使用未初始化值或上个 mode 留下的值，这是比名称误解更实质的实现缺口。

还有一个 r48 限定：上述 geometry 只在 `viewportChanged` 时重算，不是 `deviceModeChanged` 就一定重算。pointer-gesture 开关若改变 mode、两边却选中相等 viewport，ranges/reset/generation 会更新，surface geometry 仍沿用旧值。因此“UNSCALED 永远当场重建为 raw surface”只能描述正常首次配置，不能当动态切换的不变量。

---

## 8. 位置坐标的真实顺序：affine 在 raw 空间先做

raw axis 的 min/max 都是 inclusive：

```cpp
rawWidth = maxValue - minValue + 1;
```

随后：

```text
xScale     = rawSurfaceWidth / rawWidth
xTranslate = -surfaceLeft
xPrecision = 1 / xScale
```

`precision` 表示一个输出单位约对应多少 raw 单位，不是噪声 fuzz。90°/270° 后 oriented X/Y precision 会交换。

每个点的中心坐标按以下顺序处理：

```text
raw integer x/y
  → policy TouchAffineTransformation
  → 减 raw min
  → 乘 mXScale/mYScale
  → surface translate
  → 按 mSurfaceOrientation 交换/反向
  → Cooked PointerCoords X/Y
```

换句话说，affine 的输入还是 raw 坐标。它按设备 descriptor 与 surface orientation 查询，可表达缩放、偏移和交叉轴修正；把它误放到最终显示坐标之后，结果会不同。

0° 时公式最直观：

```text
X = (affineX - rawMinX) * xScale - surfaceLeft
Y = (affineY - rawMinY) * yScale - surfaceTop
```

MotionRange 的 max 使用 `rawSurface + translate - 1`，表达 inclusive 输出范围。不过 90°/180°/270° 的反向公式用 `surfaceRight/Bottom - scaled`，没有再减 1：全屏情况下 raw 最小端可映到 width/height，恰好比声明 max 大 1。`+1/-1` 是总体计数约定，不足以证明每个旋转端点都无 off-by-one。

Mapper 这里不负责 App 窗口局部变换；DIRECT/POINTER 的 cooked 显示坐标之后仍会由 Dispatcher 按目标窗口处理。

---

## 9. pressure、size、orientation、tilt 与 coverage 是独立校准账

`parseCalibration()` 只解析 IDC 意图；`resolveCalibration()` 再按 axis 能力关闭不可能实现的模式：

| 轴/配置 | 默认 resolve | cook 结果要点 |
|---|---|---|
| touch/tool major | geometric | major/minor 可按几何尺度处理 |
| pressure | physical | `raw * pressureScale` |
| orientation | interpolated | raw orientation 乘弧度比例 |
| distance | scaled | 默认比例 1 或 IDC 比例 |
| coverage | none | 仅显式 box 才启用 |

没有 pressure axis 时，touching pointer 输出 1，hovering pointer 输出 0；它是兼容默认值，不表示真实力度达到最大。有 pressure 时公式直接是 `rawPressure * scale`，不会先减 axis min，也不会 clamp；自定义 scale 下声明 range 的 max 会按 `scale * rawMax` 计算。

size 有三组容易混淆的量：

- `touch.size.scale/bias` 应用于 TOUCH/TOOL major/minor，负值最后钳到 0；
- `AXIS_SIZE` 用另一份 `mSizeScale` 按 major axis max 归一化；
- `touch.size.isSummed` 在 touching count 大于 1 时，先把 major/minor 和 size 除以 touching count。

因此不能笼统说“scale/bias 也直接作用于归一化 AXIS_SIZE”。AREA 会对 major 开平方并令 minor=major；DIAMETER 直接令 minor=major；GEOMETRIC 才乘 surface 的平均几何比例。

若 tiltX/Y 都存在，Mapper 由两轴三角关系推导 tilt 与 orientation，orientation 范围变为 `[-π, π]`；否则才使用 raw orientation 的 interpolated/vector 校准。VECTOR 把一个字节的两个 4-bit 有符号分量送入 `atan2()/2`，置信度还会拉长 major、缩短 minor。

COVERAGE_BOX 把 toolMajor/toolMinor 的高低 16 位拆成边界，旋转缩放后写入 `GENERIC_1..4`。源码只对中心 X/Y 应用 affine，并留下 `TODO: Adjust coverage coords?`，所以 box 边界没有同步 affine 修正。

---

## 10. RawPointerData 与 CookedPointerData：id、index 和有效集合要分开

`RawPointerData::Pointer` 保存驱动侧整数：

```text
id, x/y, pressure,
touchMajor/minor, toolMajor/minor,
orientation, distance, tiltX/Y,
toolType, isHovering
```

集合层还有：

```text
pointerCount
touchingIdBits / hoveringIdBits
idToIndex[id]
```

id 是跨帧身份，数组 index 是本帧紧凑存放位置；数组可重排，必须经 `idToIndex` 找同一 pointer。

CookedPointerData 保持本轮相同 id 与 index，换成：

```text
PointerProperties = id + toolType
PointerCoords     = float X/Y、pressure、size、orientation、tilt、distance…
```

touching 与 hovering bit 分开，使 stylus 可在未接触时仍有坐标，又不会进入 DOWN 序列。`clear()` 只把 count 和有效 bit 清掉，并不擦除整个固定数组；后续代码必须以 count/bitset 为权威，不能从调试内存里的旧槽推断有效 pointer。

CookedState 还保存 finger/stylus/mouse id 分组和 buttonState。若当前 `pointerCount == 0`，cook 会强制 cooked buttonState=0，即使 raw accumulator 仍报告按钮；没有活动 pointer 的孤立按钮不会从这条通用 cooked button 状态机正常形成 Motion button。

---

## 11. 从 SYN_REPORT 到 last：pending/current/cooked/last 不是重复缓存

每枚真正交给 Mapper 的 SYN_REPORT 都先 `emplace_back()` 一个 RawState：

```text
when
touch buttons | cursor buttons
raw V/H scroll
子类 syncTouch() 生成 RawPointerData
必要时分配 pointer ids
```

scroll 是包内瞬时量，立刻 `finishSync()`；绝对触摸 accumulator 则继续保存轴状态。

四类状态的所有权是：

```text
mRawStatesPending      已收齐 packet，但可能仍等外接笔配对
mCurrentRawState       当前正被 cook/dispatch；稳定点上是最近已提交 raw
mCurrentCookedState    当前 raw 的 Android 表达
mLastRaw/CookedState   上一次完成 cookAndDispatch 后的差分基线
```

drain 一项时才执行：

```text
pending → current raw → cook current → dispatch → copy current to last
```

这保证等待外接 stylus 的新 packet 不会提前污染下游差分基线。`cookPointerData()` 明确保持 raw 与 cooked 的 ids/indices 对齐，后续逻辑才可在 raw tool 信息和 cooked 坐标之间交叉读取。

若 stylus 等待让队列里已有多帧，下一帧 id 分配参照的是前一项 pending，而不总是 current/last。Multi 对 Protocol A/B 都会尝试 trackingId；只要任一活动点无法取得可用 id，整帧就清 id bits，退回父类按相同 toolType 的距离匹配。

对 pending raw，若其 `when < mLastRawState.when`，代码只钳到相等，不强制 `last+1`。因此这条路径保证非递减，却允许多个输出同时间戳。

---

## 12. 外接 stylus 融合：72、20、10 ms 属于两种等待方向

外置笔可由独立 input device 提供 pressure/buttons，触摸屏提供位置。DIRECT 设备检测到外接 stylus 后，用 pending 队列配对两条时间线。

第一次从空 pointerCount 进入非空 raw state 时：

- 若最新 stylus pressure 非 0，把第一个 touching id 认作外接笔；
- 若没有笔压力，最多从该 raw state 时间起等 72 ms；
- 新 touch packet 可继续排队，但不越过这道门 dispatch；
- 超时则放弃本轮笔身份，按普通 touch 继续。

已有 fused stream 中，若新的 stylus 状态到达而暂时没有新 touch raw，另一条路径最多等 20 ms；超时会复制 `mLastRawState`，以：

```text
stylusState.when + 20ms - 10ms
```

作为 `cookAndDispatch()` 的事件时间，即相对 stylus 样本偏移 10 ms 后合成一次压力/按钮更新。72 ms 是“位置先到、等首份笔资料”，20 ms 是“笔资料先到、等触摸更新”，10 ms 是后一路的时间补偿，不能统称为“笔固定延迟 72 ms”。

还有一个时间边界：pending raw drain 会做 `current.when < last.when` 钳制；上述 stylus-only timeout 分支直接调用 `cookAndDispatch(computedWhen)`，没有复用这次比较。因此“TouchInputMapper 所有输出时间绝不倒退”比源码保证更强，只能说普通 pending raw 路径非递减。

外接笔压力为 0 而该 id 上轮仍 touching 时，cook 后的融合还会保留上轮 pressure；真正离开通常由 touch raw 的 id 消失驱动，不能只把笔设备的 pressure=0 当成本 Mapper 的 UP。

---

## 13. cook 后如何分流：先处理 raw 特例，再选择 pointer usage

`cookAndDispatch()` 的顺序能解释很多“为什么没看到触摸”：

```text
应用 external-stylus buttons
计算 first non-empty raw / buttonsPressed 与 WAKE
DIRECT 首个 pointer sample 时 fade 系统光标
consumeRawTouches() 处理虚拟键或屏外触点
cookPointerData()
应用 external-stylus pressure/toolType
合成 BACK/FORWARD Key DOWN
按 mode 分发 Motion
合成 BACK/FORWARD Key UP
清 current raw scroll
current raw/cooked → last
```

源码变量 `initialDown` 实际判断 `last.pointerCount==0 && current.pointerCount!=0`，包含 hover，不只 touching DOWN。若 `touch.wake=true`，首个 hover 也可触发 WAKE；DIRECT 还会在屏外/虚拟键消费之前 fade 共享系统光标。

屏外单指初次 touching 可命中 virtual-key hitbox，变成 `SOURCE_KEYBOARD` 的虚拟 KeyEvent并清 raw pointers，不再进入本轮 touch cook。离开 hitbox或加入第二指会发带 CANCELED 的 Key UP，再允许后续触摸流恢复。hit-test 使用 raw 坐标阶段，而且发生在 affine 之前；affine 较大时，“是否在 surface/虚拟键内”与最终 cooked 中心甚至可能不一致。

POINTER mode 会按工具优先级选择单一 usage：

```text
stylus / eraser > mouse > finger gestures（或主按钮 down）
```

usage 变化先 abort 旧解释，再进入 stylus、mouse 或 gesture 路径；这不是三种工具同时占用 PointerController。

DIRECT/UNSCALED/NAVIGATION 的通用顺序是：

```text
button release → hover exit → touches → hover enter/move → button press
```

touch id 集合变化时先逐个 UP，必要时为保留 pointer 补 MOVE，再逐个 DOWN；第一个 POINTER_DOWN 改写为 DOWN，最后一个 POINTER_UP 改写为 UP。详细 id/slot 规则留到下一章。

DIRECT 的 `showTouches` 会获取共享 PointerController、用 SPOT presentation 画调试点；它不改变 App 收到的触摸本身。动态关闭 showTouches 若 viewport/mode 不变，只清 Mapper 的强引用而没有在该分支显式 clearSpots/fade；共享 controller 仍被其他 Mapper 持有时，旧 spot 可能要等其他路径更新或真正 reset 才清理。

---

## 14. 重配最危险的误区：NotifyDeviceReset 不会清 Mapper 状态

`configureSurface()` 只有在：

```text
viewportChanged || deviceModeChanged
```

时重算 scales/ranges、设置 `outResetNeeded=true` 并 bump generation。若结果 mode 是 POINTER，还会 `abortPointerUsage()`；如果是离开 POINTER，则这条 abort 分支不会运行。

动态 `configure()` 收到 resetNeeded 后只是直接发 `NotifyDeviceReset`，并不调用 `TouchInputMapper::reset()`。Dispatcher 会按逻辑 deviceId 取消旧 connection state，Reader 的 pending/current/last、downTime、pointer usage 与大多数本地状态却仍保留。

可观察后果包括：

- DIRECT 触点按住时 viewport 改变：下游旧流被取消，Reader 下一样本仍可能按相同 id 发 MOVE，而不是新 DOWN；
- POINTER ↔ UNSCALED 切换：新的分发解释可继承旧 raw/cooked 差分基线；
- 从 Mapper-level DISABLED 恢复：disabled 数据路径只清 pending/current raw，last raw/cooked 未必清，除非外层另有真正 InputDevice reset；
- mode-only 变化还可能沿用旧 surface geometry，见第 7 节。

真正的 `TouchInputMapper::reset()` 才会清：

```text
button/scroll/touch accumulators
三只 velocity histories
pending/current/last raw 与 cooked
pointer usage、hover、id、aborted、downTime、virtual key
pointer gesture/simple 与 external stylus fusion
PointerController spots，并 gradual fade
```

协议 accumulator 的 reset 也不是简单同义词：Single 会从 kernel 重读 ABS_X/Y、pressure、tool width、distance、tilt；Multi Protocol B 无法重读每个 slot 的完整内容，只能清所有 slot并查询当前 slot index。源码明确承认起步时可能把两个 slot 短暂混淆，直到新的 `ABS_MT_SLOT` 到来，但这样至少避免 stuck touch。

另外两条 r48 漏口要单独记：

1. external-stylus presence 只改变 DIRECT 的 `mSource`，但 viewport/mode 不变时不会重写已有 MotionRange 的 source、不 bump 这个触摸设备的 generation、不发 reset。`InputDevice::configure()` 本轮仍会重新 OR mapper 的当前 source，所以新鲜构造的设备顶层 sources 可已变化，range 却仍标旧 source；外接笔自身增删也可能触发全局设备列表变化，但既有客户端不会据此把这台触摸设备判为 generation 已变。
2. raw X/Y 无效或 viewport 缺失会在计算 `deviceModeChanged` 前早退，留下已写的 source、旧 controller/ranges/last 状态，并绕过 Mapper 的 generation/reset 收尾。首次配置以及 display-port 禁用可能由外层 reset/generation 覆盖，但不能据此替 Mapper 路径补出不存在的保证。

这正是阅读输入重配代码时最重要的方法：看到名为 `resetNeeded` 的布尔量，要继续确认它最终“调用 reset”还是仅“发送 reset 通知”。

---

## 15. 九个 macOS 只读练习：逐层证明配置与状态契约

所有命令从 `/Users/ninebot/androidSource` 执行，均不修改源码。

### 练习一：从 capability 找到协议子类

```bash
sed -n '1338,1372p' frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '185,212p' frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

回答 MT 与 single 的判据，以及为何 InputDevice 优先创建 Multi。

### 练习二：画 deviceType → mode/source 决策树

```bash
sed -n '404,505p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '612,665p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

特别标出 REL_X/REL_Y 的 OR 与 disabled 后仍保留 source。

### 练习三：核对 viewport 的“直接返回”与回退

```bash
sed -n '560,611p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

找出 port、default pointer、unique id、EXTERNAL→INTERNAL 四种失败行为。

### 练习四：手算 natural surface

```bash
sed -n '667,756p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

任选 90° viewport，写出 natural physical left/top 与宽高交换。

### 练习五：验证 affine、scale、rotation 顺序

```bash
sed -n '2178,2210p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '3620,3660p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

用 raw min 非 0 的 0° 点代入一次。

### 练习六：比较 Raw/Cooked 的有效字段

```bash
sed -n '40,125p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h
sed -n '90,155p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

解释为什么数组残值不能越过 pointerCount/bitset 成为有效触点。

### 练习七：追 pending → current → last

```bash
sed -n '1410,1635p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

圈出队列写入、时间钳制、cookAndDispatch 和最后 copy。

### 练习八：区分三条 stylus 时间常量

```bash
sed -n '25,45p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1450,1520p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1640,1725p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

分别回答哪一方先到、何时合成，以及哪条路径没有时间钳制。

### 练习九：证明 reset 通知与真正 reset 不同

```bash
sed -n '340,395p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '750,790p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1355,1405p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

列出动态重配保留、真正 reset 才清掉的状态。

---

## 16. 三个推演、十四条复读清单与下一章

### 推演一：raw min 不为 0

假设 raw X 是 100..1099，`rawWidth=1000`，raw surface 宽 2000，`surfaceLeft=100`，affine 恒等、方向 0°：

```text
xScale = 2
xTranslate = -100
raw x = 600
cooked X = (600 - 100) * 2 - 100 = 900
```

若直接算 `600*2`，就同时漏掉 raw min 与 surface crop。

### 推演二：外接笔位置先到

```text
t0：DIRECT 从空变为一个 raw pointer，笔 pressure 尚为 0
    → RawState 留在 pending，安排 t0+72ms
t0+8ms：笔 pressure 到达
    → 重新 drain，指定 touching id，cook/dispatch
    → current/last 此时才前移
```

等待期间再来的 touch samples 可排队，但不能提前成为 last。

### 推演三：按住触点旋转显示

```text
旧 DIRECT 流已有 DOWN(id0)
DISPLAY_INFO 改 viewport
configureSurface：重算 range、bump、NotifyDeviceReset
Mapper 没执行 reset：last 仍含 id0
下一 SYN 仍是 id0 → Reader 可能发 MOVE；Dispatcher 已取消旧流
```

因此 reset 通知能隔离下游旧状态，却不能保证 Reader 自动补发新 DOWN。

### 十四条复读清单

1. EventHub capability 决定 Single/Multi 子类，IDC 的 gestureMode 不换协议实现。
2. input property、IDC 基础参数、axis 与校准通常只在首次配置读取。
3. deviceType、runtime mode、source 和 enabled 是四份不同事实。
4. disabled 早退后 source 仍可能非 0且没有 ranges，外层 port 禁用另有 reset/generation。
5. port 与 unique id 查询为空都不继续普通 viewport fallback；default pointer 查询为空会继续。
6. DIRECT/POINTER 的 viewport 要先还原到 natural surface；UNSCALED 也会减 raw min。
7. surface geometry 只因 viewportChanged 重算，modeChanged 本身不充分；非显示分支的 right/bottom 还可能未初始化。
8. 中心位置先做 raw-space affine，再做 min/scale/translate/rotation；屏内/虚拟键判定没有 affine。
9. App 的 X/Y 通常还会从 Reader 显示坐标变为窗口局部坐标。
10. 无 pressure axis 时 touch=1、hover=0；size scale/bias 不等于 AXIS_SIZE 的归一化比例。
11. Raw/Cooked 同轮 id/index 对齐，但 count 与 bitset 才定义有效成员。
12. pending raw 时间会钳成非递减；stylus-only timeout 合成没有复用该钳制。
13. 首个非空 raw pointer 包含 hover，也可 fade pointer/触发配置允许的 WAKE。
14. 动态 NotifyDeviceReset 不调用 Mapper reset；source-only 和 disabled 早退又可能绕过 generation/range 收尾。

### 一句话模型

```text
TouchInputMapper 先把 capability、input property 与首次 IDC 解释成协议子类和 deviceType，再结合动态手势/显示配置建立 mode、source、viewport 与 natural surface；每个 SYN_REPORT 形成 RawState，经外接笔 pending 门后才成为 current，中心点按 affine→min/scale/translate→rotation 形成同 id/index 的 CookedState并按 mode 分发，最后复制为 next baseline；而 r48 的动态 reset 多数只是下游通知，本地状态、source-only 更新、disabled 早退与 mode-only geometry 都必须另行审计。
```

### 下一章

第 183 章深入 MultiTouch Protocol A/B、slot、trackingId、pointer id 稳定分配、palm cancel 与多指 action index。
