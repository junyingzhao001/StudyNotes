# 185 Android 触摸校准与 Virtual Key：一帧 Raw Touch 怎样分成 Motion 与 Key

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> `frameworks/native` 提交：`57b3d43492`；`frameworks/base` 提交：`1d9b9ab57d`  
> 学习方式：macOS 静态只读，不要求编译、不冒充真机实测  
> 前置章节：第 182—184 章

---

## 1. 先看故障现场：同一块面板为什么一会儿发 Motion，一会儿发 Key

调试触摸设备时，几类现象很容易被误判为驱动随机：

- 没有 pressure axis，App 仍看到接触时 `PRESSURE=1`；
- `SIZE=0.5`，却不能推出 `TOUCH_MAJOR` 是屏幕对角线的一半；
- IDC 写了 `orientation=none`，旋转屏上的坐标里仍可能出现非零 orientation；
- 手指落在屏幕外，既没有越界 Motion，也可能暂时没有 KeyEvent；
- HOME 已经发出 `DOWN`，滑进屏幕时又在同一帧看到取消的 `UP` 与 Motion `DOWN`；
- 一枚虚拟键被 quiet window 抑制后，整段触摸仍然消失。

它们来自同一个执行顺序：

> `cookAndDispatch()` 先让 `consumeRawTouches()` 判断 raw touching 集合是否要被 Virtual Key 路径消费；只有留下来的 pointer data，才进入 `cookPointerData()` 做 size、pressure、orientation、distance 与 coverage 解释。

本章只追一个问题：

```text
一帧 RawPointerData 到达 cookAndDispatch() 后，
怎样决定它被改发成 KeyEvent、被静默吞掉，
还是继续变成带校准 axis 的触摸状态并最终参与 MotionEvent？
```

这不是严格的二选一。虚拟键滑出时，代码可先发一枚带 `CANCELED` 的 Key `UP`，再让同一帧 raw pointers 继续 cooking，形成新的 Motion `DOWN`。

辅助 axis 的公式在所有 `TouchInputMapper` mode 中都会先生成 Cooked State；但最终是否原样到达 App 还取决于后续 mode。DIRECT、UNSCALED、NAVIGATION 基本直接使用 cooked arrays；POINTER 下的 finger gesture 会重建鼠标式坐标，只保留 X/Y 与合成 pressure。本文讨论的是 InputReader 里的校准候选与分流边界，不把 motion range 宣告误当成每种下游事件都必然携带该 axis。

---

## 2. 唯一分叉点在 cooking 之前，但返回 true 不等于函数立即结束

### 完整调用链

```text
EV_SYN / SYN_REPORT
  → TouchInputMapper::sync()
      形成 RawState、分配 Android pointer id
  → processRawTouches()
  → cookAndDispatch()
      applyExternalStylusButtonState()
      计算 initialDown / policyFlags
      consumeRawTouches()
        ├─ 可直接发 Virtual Key DOWN / UP
        └─ true 时只清 rawPointerData
      cookPointerData()
      applyExternalStylusTouchState()
      按 device mode / pointer usage 分发
      保存 Current Raw/Cooked → Last Raw/Cooked
```

关键代码不是 `return`：

```cpp
if (consumeRawTouches(when, policyFlags)) {
    mCurrentRawState.rawPointerData.clear();
}
cookPointerData();
```

所以“消费”精确表示清掉 pointer count、touching/hovering id bits，使 cooking 和触摸分发看不到这些 pointers。`RawState.when`、button、scroll 等字段并未随之清空，后面的按键按钮与收尾代码仍会执行。

虚拟键取消分支甚至会返回 false：它先结束旧 Key，再让 current raw data 继续进入 cooking。这就是一帧内 `KEY_UP(CANCELED) → MOTION_DOWN` 或 `旧 KEY_UP(CANCELED) → 新 KEY_DOWN` 的基础。

### 必须同时看的五本账

| 账本 | 作用 | 常见误读 |
|---|---|---|
| `mRawPointerAxes` | 首次打开时记录 evdev axis 能力与 min/max/fuzz/resolution | 每帧 raw 值 |
| `mCalibration` 与 surface factors | IDC 意图、resolve 结果和实际比例 | IDC 文本本身就是最终行为 |
| Current/Last Raw 与 Cooked | 决定 0→非空、Motion 拓扑和上一帧坐标 | 被消费后 Last Raw 仍保存原触点 |
| `mCurrentVirtualKey` | 一枚活动 key 的 down/ignored/time/code | InputDispatcher 的全部 Key 状态 |
| InputReader `mDisableVirtualKeysTimeout` | 所有 mapper 共用的 quiet deadline | 每台设备独立 timer |

### 源码地图

| 主题 | r48 文件 |
|---|---|
| 主分叉、校准、cooking、Virtual Key 状态机 | `frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp` |
| Calibration、VirtualKey 与账本字段 | `frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h` |
| 单点/多点各自装载哪些 raw axes | `SingleTouchInputMapper.cpp`、`MultiTouchInputMapper.cpp` |
| sysfs map 加载与设备 class | `frameworks/native/services/inputflinger/reader/EventHub.cpp` |
| `virtualkeys.*` 文本解析 | `frameworks/native/libs/input/VirtualKeyMap.cpp` |
| 全局 quiet deadline 与 dump | `frameworks/native/services/inputflinger/reader/InputReader.cpp` |
| Java resource → JNI 配置 | `InputManagerService.java`、`com_android_server_input_InputManagerService.cpp` |
| 稀疏 axis 写入规则 | `frameworks/native/libs/input/Input.cpp` |

---

## 3. 配置不是结果：parse、resolve、configureSurface 是三层真相

### 第一层：首次配置才 parse

`configure()` 把下列步骤放在 `if (!changes)` 中：

```cpp
configureParameters();
configureRawPointerAxes();
parseCalibration();
resolveCalibration();
```

这表示 IDC calibration 与 raw axis 能力在 mapper 生命周期内按“不可变配置”处理。普通动态 reconfigure 不重读这些字段；要采用新的 IDC 或 capability，正常路径是设备 reopen，而不是等待一次 display change。

`parseCalibration()` 读取：

- `touch.size.calibration`：default / none / geometric / diameter / box / area；
- `touch.size.scale`、`touch.size.bias`、`touch.size.isSummed`；
- `touch.pressure.calibration`：default / none / physical / amplitude；
- `touch.pressure.scale`；
- `touch.orientation.calibration`：default / none / interpolated / vector；
- `touch.distance.calibration`：default / none / scaled；
- `touch.distance.scale`；
- `touch.coverage.calibration`：default / none / box。

不认识的枚举字符串只记 warning，枚举仍留 DEFAULT；float/bool 属性是否成功解析由对应 `have...` 标志记录。它们不会让整个触摸设备配置事务失败。

### 第二层：resolve 用 axis 能力裁决

| 项目 | DEFAULT 且能力存在 | 能力不存在 | 例外 |
|---|---|---|---|
| size | touchMajor 或 toolMajor 任一存在 → GEOMETRIC | 强制 NONE | 显式模式只要求“至少一种 major” |
| pressure | pressure 存在 → PHYSICAL | 强制 NONE | PHYSICAL 与 AMPLITUDE 在 cook 公式相同 |
| orientation | orientation 存在 → INTERPOLATED | 强制 NONE | 双 tilt 是稍后独立建立的路径 |
| distance | distance 存在 → SCALED | 强制 NONE | 无 |
| coverage | DEFAULT → NONE | 不参与判定 | 显式 BOX 完全不校验 tool axes |

“声明显式模式但缺 axis 就一定回 NONE”只适用于 size、pressure、orientation、distance 各自的 resolve 规则。`coverage=box` 是硬例外；双 tilt 也不受 orientation calibration 是否为 NONE 控制。

### 第三层：surface block 才计算有效 factor 与 range

`configureSurface()` 虽会因 display、pointer gesture、show touches、external stylus 等变化被调用，真正重算以下数据的条件却是：

```cpp
if (viewportChanged || deviceModeChanged) {
    configureVirtualKeys();
    // x/y, geometric, size, pressure, tilt,
    // orientation, distance, ranges...
    bumpGeneration();
}
```

非首配时，该 block 还会让外层发送 `NotifyDeviceReset`。仅仅调用 `configureSurface()` 不保证数值被重算。

Affine 是另一条动态链。`CHANGE_TOUCH_AFFINE_TRANSFORMATION` 只更新 `mAffineTransform`；不重建 factors/ranges、不 bump 本 mapper generation，也不发送这条 surface reset。

因此排障要分三问：

1. IDC 解析出了什么意图？
2. raw axis 能力把它 resolve 成什么枚举？
3. 当前 viewport/device mode 上次实际计算出的 factor、range 与 hit box 是什么？

`dumpsys input` 的 mapper 段能看到 resolved calibration 和格式化后的 factor/range，适合回答第 2、3 问；它不能证明一份刚修改但尚未 reopen 的 IDC 已被重新读取。

---

## 4. Single 与 Multi 装载的 axis 不同，range 又不等于 clamp

### stock r48 的两个子类并不对称

| Raw axis | SingleTouchInputMapper | MultiTouchInputMapper |
|---|---:|---:|
| position | `ABS_X/Y` | `ABS_MT_POSITION_X/Y` |
| pressure | `ABS_PRESSURE` | `ABS_MT_PRESSURE` |
| size/tool | axis metadata 只标 `ABS_TOOL_WIDTH`→toolMajor valid；逐帧 raw 同时写 toolMajor/toolMinor | MT touch/width major/minor |
| orientation | 不装载 | `ABS_MT_ORIENTATION` |
| tiltX/tiltY | `ABS_TILT_X/Y` | 不装载，Raw Pointer 写 0 |
| distance | `ABS_DISTANCE` | `ABS_MT_DISTANCE` |
| tracking/slot | 无 | `ABS_MT_TRACKING_ID/SLOT` |

所以“有 tilt 时优先于 orientation”在基类逻辑上成立；但 stock r48 常见到的是 Single 走 tilt、Multi 走 orientation，而不是同一 MultiTouch pointer 同时带两套 axis。

SingleTouch 把 `ABS_TOOL_WIDTH` 同时写进 raw `toolMajor/toolMinor`；它不凭空产生 touchMajor。后面的通用 size 代码才会在只有 tool 组时把 tool 值复制给 touch 组。

### MotionRange 是元数据，不是运行时裁剪器

`configureSurface()` 会登记：

- X/Y 的当前 oriented range 与 precision；
- pressure 始终存在的声明 range；
- 非 NONE size 的 SIZE、TOUCH/TOOL major/minor；
- 可用 orientation、tilt、distance；
- coverage BOX 的 GENERIC_1..4。

`PointerCoords::setAxisValue()` 不查这些 range，也不 clamp；非零值会写入稀疏 axis 数组，零值通常不占槽但读取仍返回 0。厂商给出负 scale、非有限值或不对称 raw range 时，payload 可以偏离宣告区间。

还要分清 Cooked 与最终事件：

| 后续路径 | 辅助 cooked axes |
|---|---|
| DIRECT / UNSCALED / NAVIGATION touch | 直接用 cooked arrays |
| POINTER stylus | 复制 cooked axes，PointerController 覆写 X/Y |
| POINTER mouse | 复制 cooked axes，覆写 X/Y 并重写 pressure |
| POINTER finger gesture | 重建鼠标式坐标，size/orientation/tilt/distance/coverage 不透传 |

因此 `getMotionRange()` 能证明 mapper 宣告了能力，不能证明某个 pointer usage 的每枚 MotionEvent 都携带原始 cooked 值。

---

## 5. Size 先选 raw 来源，再同时建立两条不同的输出链

### touch/tool 缺项怎样补

size calibration 非 NONE 时，代码先选原料：

| 可用 major | touchMajor/minor | toolMajor/minor | SIZE 的 raw 基数 |
|---|---|---|---|
| touch 与 tool 都有 | 各用各组；缺 minor 就用本组 major | 各用各组 | touchMajor，或 touch major/minor 平均 |
| 只有 touch | touch 组 | 复制 touch 组 | touchMajor，或其 major/minor 平均 |
| 只有 tool | 复制 tool 组 | tool 组 | toolMajor，或其 major/minor 平均 |

这只是补齐输出结构，不证明硬件独立测过“接触椭圆”和“工具外形”。

从这里开始必须分开两条链：

```text
AXIS_SIZE
  = 选出的 raw major 或 raw major/minor 平均
  → 可选 isSummed 除数
  → × mSizeScale

TOUCH/TOOL_MAJOR/MINOR
  = 选出的四个 raw 值
  → 可选 isSummed 除数
  → mode 变换
  → IDC size.scale / size.bias
  → 下限截到 0
  → VECTOR orientation 还可能再改长短轴
```

`touch.size.scale/bias` 完全不作用于 `AXIS_SIZE`；GEOMETRIC、AREA 等模式也不改变 SIZE 的那一步公式。

### mSizeScale 不是严格的 min-max 归一化

自动比例的优先级是：

```text
touchMajor valid 且 max != 0 → 1 / touchMajor.max
否则 toolMajor valid 且 max != 0 → 1 / toolMajor.max
否则                            → 0
```

“缺 touchMajor 才 fallback”不够精确：touchMajor 存在但 `max==0` 时也会尝试 toolMajor。

计算不减 raw min，也不 clamp。只有常见的 `min=0`、上报值不超过 declared max 时，SIZE 才自然落在 [0,1]。源码虽然把 MotionRange 声明为 0..1，却没有在每帧强制这个承诺。

---

## 6. Size 的顺序不能交换：除数、mode、scale/bias、VECTOR 各管一段

### isSummed 先于模式变换

当 IDC 显式提供且设置 `touch.size.isSummed=true` 时：

```cpp
uint32_t touchingCount = touchingIdBits.count();
if (touchingCount > 1) {
    touchMajor /= touchingCount;
    touchMinor /= touchingCount;
    toolMajor /= touchingCount;
    toolMinor /= touchingCount;
    size /= touchingCount;
}
```

分母只数 touching ids，不数 hovering ids。不过这段代码位于“遍历所有 raw pointers”的循环里：若同帧有两枚 touching 加一枚 hover，hover pointer 自己的 size/major 也会除以 2，只是它没有给分母加一。

### 四种非 NONE 模式只改 major/minor

| mode | 对四个 major/minor 的变换 | 不发生什么 |
|---|---|---|
| GEOMETRIC | 全部乘 `avg(mXScale,mYScale)` | 不按方向分别乘 X/Y |
| DIAMETER | 每组 minor=major | 不改 SIZE 原料 |
| BOX | 没有专属数学变换 | 不是 coverage BOX |
| AREA | 正 major 开平方，minor=处理后的 major；非正 major→0 | 不给 SIZE 开平方 |

之后，四个 major/minor 各自执行：

```text
若 haveSizeScale：value *= sizeScale
若 haveSizeBias： value += sizeBias
若 value < 0：    value = 0
```

只有下限 0，没有上限、finite 或 motion-range clamp。AREA/DIAMETER 在这一刻令 major==minor；若稍后走非零 VECTOR orientation，major 还会乘椭圆比例、minor 会除比例，最终二者可再次分开。

### 手算一帧，才能看见两条链

假设：

```text
两枚 touching
raw touchMajor=25, touchMinor=9
raw toolMajor=36, toolMinor 不存在
touchMajor.max=100 → mSizeScale=0.01
mode=AREA, size.scale=2, size.bias=1
```

先补 toolMinor=36，再执行 isSummed：

```text
touch = 12.5 / 4.5
tool  = 18 / 18
SIZE raw basis = avg(25,9)/2 = 8.5
```

AREA 与 IDC 参数只进入 major/minor：

```text
TOUCH_MAJOR = TOUCH_MINOR = sqrt(12.5)*2+1 ≈ 8.071
TOOL_MAJOR  = TOOL_MINOR  = sqrt(18)*2+1   ≈ 9.485
SIZE = 8.5 * 0.01 = 0.085
```

所以 `SIZE=0.085` 与任一 major 的像素或厂商尺度都不是同一个量。若接着有 VECTOR 编码，四个 major/minor 还会发生最后一次长短轴调整，SIZE 保持 0.085。

---

## 7. Pressure 与 distance 都是 raw×scale，但默认、range 和最终覆写不同

### pressure 的三种实际结果

| resolved mode | per-frame 值 | 宣告 range |
|---|---|---|
| PHYSICAL / AMPLITUDE，有显式 scale | `rawPressure * configuredScale` | min=0，max=scale×rawMax |
| PHYSICAL / AMPLITUDE，无显式 scale | rawMax非0时 `raw/rawMax`，否则恒0 | 0..1 |
| NONE | touching=1，hovering=0 | 0..1 |

PHYSICAL 与 AMPLITUDE 在 r48 cooking 中没有公式差异。真正决定是否合成 0/1 的是 resolved calibration 为 NONE；“axis 有效但 rawMax=0”仍保留 PHYSICAL，`mPressureScale=0`，所以接触 pressure 也是 0，而不是合成 1。

和 SIZE 一样，pressure 不减 raw min、不 clamp。显式 scale 也没有正数或 finite 校验；负值、0 或异常浮点会原样进入 factor 与 range 计算。这是 IDC 作者的契约，不是框架替设备修复的输入。

### external stylus 还可在 cook 后改写

`cookPointerData()` 之后紧接着调用 `applyExternalStylusTouchState()`。DIRECT 设备若已把外接笔融合到某个 touching id：

- external pressure 覆盖面板刚算出的 pressure；
- external pressure 暂为 0、且该 id 上一帧仍 touching 时，沿用上一帧 cooked pressure；
- external toolType 不是 UNKNOWN 时，也覆盖 properties.toolType。

因此上述公式是 `cookPointerData()` 的输出，不总是最终 `NotifyMotionArgs` 的值。

### distance 不做 0/1 合成

SCALED 的公式是：

```text
distance = rawDistance * mDistanceScale
```

未显式配 scale 时默认 1。MotionRange 的 min、max、fuzz 分别乘同一 scale，flat 与 resolution 写 0；NONE 则每帧输出 0 且不登记 distance range。

它同样不减 raw min、不 clamp，也不校验 scale。负 scale 不会自动交换 range min/max，还可能得到负 fuzz。正常设备应通过 IDC 与 axis metadata 保证这些量有意义。

---

## 8. Tilt 与 orientation 有三条路径，最后还要过一次 surface 旋转

### 双 tilt 完全压过 orientation calibration

只要 `tiltX`、`tiltY` 两条 raw axis 都 valid，`mHaveTilt=true`。此时不看 `touch.orientation.calibration`，用固定“raw 单位就是度”的公式：

```text
centerX = avg(rawTiltX.min, rawTiltX.max)
centerY = avg(rawTiltY.min, rawTiltY.max)
xAngle  = (rawTiltX-centerX) * π/180
yAngle  = (rawTiltY-centerY) * π/180

orientation = atan2(-sin(xAngle), sin(yAngle))
tilt        = acos(cos(xAngle) * cos(yAngle))
```

登记的 orientation range 为 [-π,π]，tilt 为 [0,π/2]。公式本身没有 clamp；异常 axis 跨度可能让 tilt 超出所声明上限。

stock r48 中 SingleTouch 装载双 tilt 而不装载独立 orientation；MultiTouch 装载 `ABS_MT_ORIENTATION` 而不装载 tilt。IDC 写 `orientation=none` 不能关闭 SingleTouch 的 tilt-derived orientation。

### INTERPOLATED 不是 min-max 仿射

无双 tilt 且 resolved 为 INTERPOLATED 时：

```text
rawMax > 0                    → scale =  π/2 / rawMax
rawMax <= 0 且 rawMin < 0     → scale = -π/2 / rawMin
否则                          → scale = 0

orientation = raw * scale
```

它以 raw 0 为中心，优先只用正侧 max；不执行 `(raw-min)/(max-min)`。若 raw range 不对称，另一端可能越出声明的 [-π/2,π/2]，又没有 clamp。

### VECTOR 把低八位拆成两个有符号 nybble

```text
c1 = signExtend4((rawOrientation & 0xf0) >> 4)
c2 = signExtend4(rawOrientation & 0x0f)
```

当二者不全为 0：

```text
orientation = atan2(c1,c2) / 2
confidence  = hypot(c1,c2)
shapeScale  = 1 + confidence/16

touchMajor *= shapeScale
touchMinor /= shapeScale
toolMajor  *= shapeScale
toolMinor  /= shapeScale
```

VECTOR 在 size 的 mode、IDC scale/bias 和下限处理之后运行；它不改 `AXIS_SIZE`。raw 的更高位被 mask 掉，四位符号范围是 -8..7。

例如 `rawOrientation=0x1f`：

```text
c1=1, c2=-1
orientation=atan2(1,-1)/2=3π/8
shapeScale=1+sqrt(2)/16≈1.0884
```

### surface rotation 的一个 r48 异常边界

cooking 随后无条件按 `mSurfaceOrientation` 调整 orientation：

| surface orientation | 调整 |
|---:|---|
| 0° | 不变 |
| 90° | 减 π/2 |
| 180° | 减 π |
| 270° | 加 π/2 |

若有 orientation range，代码只用一次严格 `< min` 或 `> max` 加减一个周期；它不是通用 modulo，异常大值一次后仍可能越界。

更反直觉的是：无 tilt 且 calibration=NONE 时，前面先令 orientation=0，但 90°/180°/270° 分支仍照样改成 `-π/2`、`-π`、`+π/2`。`haveOrientation=false` 只让代码不登记 range、不做 wrap，并不阻止非零值写入 `PointerCoords`。在 DIRECT 路径中，App 因而可能看到“设备没有 orientation range，事件却携带非零 orientation”的 r48 实现异常。

---

## 9. Coverage BOX 借用 packed tool 字段，但不会自动关闭 size

### 两个 int32 拆四条非负边

`coverage=box` 把：

```text
toolMinor 高16位 → rawLeft
toolMinor 低16位 → rawRight
toolMajor 高16位 → rawTop
toolMajor 低16位 → rawBottom
```

例如：

```text
toolMinor = 0x001e0050 → left=30, right=80
toolMajor = 0x00280064 → top=40,  bottom=100
```

拆位不做 16 位符号扩展，所以四个结果都按 0..65535 理解。resolve 也不检查 toolMajor/toolMinor 是否 valid；没有边序、范围或矩形合法性校验。

### 中心与 coverage 不走同一几何链

pointer 中心：

```text
raw X/Y → policy affine → rotateAndScale
```

coverage 四边：

```text
packed raw edges → 进入 0/90/180/270 四组硬编码公式
                 → 按分支使用 raw min/max、X/Y scale
                 → 只在源码明确写出的分量上加 translate
```

源码在 affine 之后直接留了 `TODO: Adjust coverage coords?`。所以 affine 可移动中心而不移动四边；coverage 并非完全不换算，但也没有复用统一的 `rotateAndScale()`。尤其 180° 的 left/right 不加 `mXTranslate`，270° 的 left/right 不加 `mYTranslate`，只能按四个分支的实际公式复算，不能概括成“每条边统一做 scale+translate+rotation”。

最终 payload 用：

| axis | 边 |
|---|---|
| GENERIC_1 | left |
| GENERIC_2 | top |
| GENERIC_3 | right |
| GENERIC_4 | bottom |

该分支不向当前 `PointerCoords` 写 TOOL_MAJOR/TOOL_MINOR，但这不等于 coverage 会禁用 size：

- size calibration 若非 NONE，前面的 size 代码仍可能把同一 packed tool 值解释成 SIZE 与 TOUCH_MAJOR/MINOR；
- `populateDeviceInfo()` 也可能继续登记 TOOL_MAJOR/MINOR range；
- coverage 另行登记四个基于 X/Y 的 GENERIC range。

因此部署 BOX 协议时必须成套配置 size 语义。只设置 `touch.coverage.calibration=box`，框架不会替厂商消除两种解释的冲突。

---

## 10. cook 保留 pointer 身份，只重解释数值；但它不是最终分发层

### Raw 与 Cooked 的结构不变量

`cookPointerData()` 开头复制：

```text
pointerCount
hoveringIdBits
touchingIdBits
```

随后按相同数组索引逐 pointer 计算 axis，最后写：

```cpp
properties.id = in.id;
properties.toolType = in.toolType;
idToIndex[id] = i;
```

校准不会重分配第 183 章建立的 Android pointer id，也不会改变 touching/hovering 身份。它改变的是同一个 pointer 的坐标解释和辅助轴。

X/Y 本身在这一层走：

```text
raw center
  → mAffineTransform.applyTo()
  → rotateAndScale()
  → cooked X/Y
```

Virtual Key 的 inside/hit test 发生在这之前，直接看 raw X/Y；affine、surface rotation 和 cooked 坐标都不会反过来改变本帧是否命中 key。

### 零值、range 与 App 可见值是三回事

每个 `PointerCoords` 先 `clear()`，再逐 axis `setAxisValue()`。零值通常不在稀疏 bitset 占槽，`getAxisValue()` 仍返回 0；非零值则不检查 MotionRange。

之后仍有两层变化：

1. external stylus fusion 可覆写 pressure/toolType；
2. device mode / pointer usage 决定直接透传 cooked arrays，还是重建鼠标式 gesture coords。

排查辅助轴时应同时记录 raw axis、resolved calibration、surface factors、last cooked dump 和最终 event；只看任意一层，都无法证明另一层相同。

---

## 11. Virtual Key 的定义链：sysfs 给几何，KCM/KL 给 Android keyCode

### EventHub 只在设备打开时读取 map

对 `INPUT_DEVICE_CLASS_TOUCH` 设备，EventHub 尝试读取：

```text
/sys/board_properties/virtualkeys.<device canonical name>
```

文件不存在不会阻止普通触摸设备打开。成功解析出 map 后，EventHub 给该节点加 `INPUT_DEVICE_CLASS_KEYBOARD`，随后加载 key map；这样虚拟 scanCode 才有机会映射为 Android keyCode。

sysfs map 随 EventHub 设备打开加载。后面的 display/viewport reconfigure 只重新使用内存里的 definitions 计算 hit boxes，并不会重新打开这个文件；修改它通常需要设备 reopen。

### 解析器是“语法全有或全无”

每项格式是：

```text
0x01:scanCode:centerX:centerY:width:height
```

同一行多项之间也由冒号继续连接：

```text
0x01:102:540:1950:160:100:0x01:158:900:1950:160:100
```

不同项也可以各占一行；一行第一个非空白字符是 `#` 时整行视为注释。类型不是 `0x01`、少字段、非法整数或字段后残留非预期文本，会让整份 `VirtualKeyMap::load()` 返回 null，不是只跳过坏项。

语法成功并不代表几何合理：解析器不验证 width/height 为正、不验证矩形在 surface 外、不查重叠，也不限制 center 与尺寸组合。

### mapKey 与 hit box 是后续两步

`configureVirtualKeys()` 对每项调用：

```cpp
mapKey(scanCode, usageCode=0, metaState=0, ...)
```

r48 的 EventHub 先问 combined KCM，未命中再问 KL。KCM 命中时 flags 从 0 开始；KL 命中时返回其 policy flags。单项 mapKey 失败只丢该 definition，不会清掉其他有效 key。

映射成功后保存：

```text
scanCode
keyCode
flags
raw-coordinate hit rectangle
```

其中 `flags` 后面有一个重要断点：`dispatchVirtualKey()` 没有读取该字段，见第 15 节。

### display definition 怎样换回 raw hit box

以 X 为例：

```text
rawWidth = rawX.max - rawX.min + 1
halfWidth = definition.width / 2

hitLeft  = (centerX-halfWidth) * rawWidth / mRawSurfaceWidth + rawX.min
hitRight = (centerX+halfWidth) * rawWidth / mRawSurfaceWidth + rawX.min
```

Y 同理。运算使用 int32，除法截断；奇数 width/height 的一半也先截断。命中判断四边都是闭区间：

```text
x >= left && x <= right && y >= top && y <= bottom
```

定义坐标按 natural display/surface 语义换到 raw touch 坐标，运行时直接拿 raw X/Y 命中；不经过 affine。多个矩形重叠时，`findVirtualKeyHit()` 返回 vector 中第一个命中的项，文件顺序因此会影响结果。

---

## 12. Virtual Key 的入场门只看 touching 集合和最低 Android pointer id

### 精确资格不是“完整 RawState 从空到非空”

新 key/off-screen 检查的条件是：

```cpp
last.rawPointerData.touchingIdBits.isEmpty()
        && !current.rawPointerData.touchingIdBits.isEmpty()
```

hovering pointer 不参与。RawState 即使还保存 when、button、scroll 或 hover，也不影响这道门；反过来，只要 last/current 都各有 touching，即使发生“旧触点全换成新触点”，也不会取得 Virtual Key。

`count()==1` 只表示一枚 touching pointer，没有 finger toolType 门。MultiTouch palm 在更早的 `syncTouch()` 已被过滤并触发 cancel，但 touching stylus，乃至被归为 touching 的 mouse，都可到达这段通用逻辑。

### 多指只拿最低 id 的点判断 inside

入场逻辑等价于：

```text
若 last touching 为空、current touching 非空：
  id = current touching 集合的最低 marked id
  只读取这个 id 的 raw X/Y

  若这个点 inside surface：
      不查 Virtual Key，整组继续 cooking

  若这个点 outside surface：
      若 touching count == 1：
          查第一个命中矩形；命中则取得 key
      无论是否命中、也无论 count 是否为1：
          consume 整组 pointer data
```

所以“多指不查 key”还不够：

- 最低 id 在外：整组被吞，即使另一指已经在屏内；
- 最低 id 在内：整组进入 cooking，即使另一指在屏外；
- 进入 cooking 且 Last Cooked 为空时，同一 `when` 可展开 `DOWN → POINTER_DOWN`。

这不是几何上的“任一点/所有点”判定，而是 r48 对 `firstMarkedBit()` 的实现依赖。

### 为什么屏幕外会反复重试

off-screen 帧返回 true 后，调用者清 `mCurrentRawState.rawPointerData`。帧末复制给 `mLastRawState` 的 touching 集合也是空，于是下一份仍在屏幕外的报告又满足资格门：

```text
屏幕外未命中
  → 本帧 consume
  → saved last touching 仍为空
  → 下帧重新判断 first id / inside / hit
```

它不是显式的 `OFFSCREEN_RETRY` enum，而是清账本造成的效果。后续测试点进入 surface 时可直接成为一条新 Motion 流；后续仍 outside 但进入某个 key box 时也可中途取得 key。

正常从 surface 内开始的 touch 会把 non-empty touching 保存进 Last Raw。之后滑出 surface 仍不经过这道 0→非空门，通常继续作为原 Motion 流；它不会因为坐标碰到 key box 就半路改键。

### “key 必须在屏幕外”不是配置约束

只有“取得新 key 时所测试的那枚最低 id pointer”必须 outside。代码从未校验整个 hit rectangle 位于 surface 外。错误配置的矩形可以跨进显示区；这会影响 active-key 保持行为。

---

## 13. 持键状态机先处理旧 key，再决定本帧是吞、换键还是转 touch

### active key 只锁存逻辑 key，不锁存 touch id

`mCurrentVirtualKey` 保存：

```text
down
ignored
downTime
keyCode
scanCode
```

它不保存 Android pointer id，也不保存“最初命中的矩形索引”。active 分支每帧先：

1. 若 touching 为空，正常结束；
2. 若恰好一枚 touching，取它的 raw 点做 `findVirtualKeyHit()`；
3. 只要命中项的 `keyCode == current keyCode`，继续保持并消费；
4. 否则取消旧 key，再继续执行本帧的新入场检查。

因此同帧旧指被新指替换，只要最终仍是一枚 touching 且命中同 keyCode，逻辑 key 会不断开。两个不同 scanCode 映射到同一 keyCode 时，跨矩形也不切键；最终 UP 仍携带最初锁存的 scanCode 与 downTime。

active 保持也不再次调用 `isPointInsideSurface()`。若错配 hit box 跨入显示区，指针已经 inside 但仍命中相同 keyCode 时，代码会继续吞作 key。只有进入 surface 且不再命中相同 keyCode，才走常见的“取消 Key → 新 Motion”。

### 一张事件表覆盖所有主分支

| old state / current touching | mapper 动作 | 是否消费 current pointers |
|---|---|---|
| 无 key；单 touching、测试点 outside、命中 | 锁存 key；accepted 发 DOWN，ignored 不发 | 是 |
| 无 key；测试点 outside、未命中或多 touching | 不发 Key | 是 |
| active；touching 为空 | accepted 发普通 UP；ignored 静默 | 是 |
| active；单 touching 命中相同 keyCode | 不重复发 DOWN | 是 |
| active；其余情况 | accepted 先发 CANCELED UP；ignored 静默结束 | 暂不决定，继续新入场门 |

最后一行会继续分叉：

- 单 touching outside 且命中新 keyCode：同一 eventTime 旧 `CANCELED UP → 新 DOWN`；
- 测试点 inside：当前 pointers 进入 cooking，可形成 Motion DOWN；
- 测试点 outside 且未命中/多指：当前整组继续被吞。

### 几条容易写错的时间线

正常 key：

```text
t0  KEY_DOWN(eventTime=t0, downTime=t0)
t1  同 keyCode 内移动：mapper 无新 NotifyKey
tn  KEY_UP(eventTime=tn, downTime=t0)
```

切到不同 keyCode：

```text
t0  HOME DOWN
t1  HOME UP|CANCELED, downTime=t0
t1  BACK DOWN,         downTime=t1
```

前提是 t1 仍为单 touching、测试点 outside 且命中 BACK。若 HOME/BACK 的 scanCode 最终映射成同一个 keyCode，则 t1 没有切换。

滑进 surface：

```text
t0  KEY DOWN
t1  KEY UP|CANCELED
t1  MOTION DOWN
```

该序列要求 t1 已不再命中相同 keyCode。若同时变成两指，还要看最低 pointer id：它 inside 才产生多指 Motion；它 outside 时整组仍会被吞。

活动 key 加第二指也总会先脱离“恰一指且同 keyCode”保持条件。accepted key 先发取消 UP，然后才按最低 id 的 inside 结果决定本帧是 `DOWN→POINTER_DOWN` 还是无 Motion。

### “mapper 不重复 DOWN”不等于 App 没有 repeat

保持区内 `TouchInputMapper` 不再发送 `NotifyKeyArgs(DOWN)`。但 InputDispatcher 会代表所有上游 notifier 维护 key repeat；Virtual Key 进入 Dispatcher 后也可能得到合成 repeat，除非 policy 设置 `POLICY_FLAG_DISABLE_KEY_REPEAT` 或上层拦截。两层不要混写。

---

## 14. Quiet window 是一项全局、锁存式抑制，不是每个 key 的 timer

### 配置链与默认值

`frameworks/base` 的 `config_virtualKeyQuietTimeMillis` 经：

```text
Resources
  → InputManagerService.getVirtualKeyQuietTimeMillis()
  → NativeInputManager JNI
  → InputReaderConfiguration.virtualKeyQuietTime
```

r48 默认资源是 0，即关闭；注释建议启用时不超过 250ms。设备/产品 overlay 可以改它。

### 更新时间的真实条件

`consumeRawTouches()` 只有走到函数尾、没有在前面 return，并满足：

```cpp
virtualKeyQuietTime > 0 && !current.touchingIdBits.isEmpty()
```

才执行：

```text
globalDeadline = when + quietInterval
```

这里没有再次检查 `isPointInsideSurface()`，也没有 device-mode 门。因此：

- 正常被接纳的 touch 后来滑出 surface，仍可继续覆盖 deadline；
- 任一 `TouchInputMapper` 的未消费 touching 帧都可能更新全局值；
- hover、UP、active-key hold、off-screen consume 帧不会更新。

`disableVirtualKeysUntilLocked()` 是直接赋值，不是 `max(old,new)`。在单调 eventTime 的常规触摸流里，它表现成“每帧向后滑动”；跨设备报告若按较旧时间随后处理，源码本身没有防止 deadline 回退。

该值位于 `InputReader`，不在 mapper。一个设备写入后，另一个设备取得 Virtual Key 也读取同一值；device reset 不会清它。

### drop 判定只有一个严格边界

```text
now < globalDeadline  → ignored=true
now == globalDeadline → 允许
now > globalDeadline  → 允许
```

没有为 deadline 请求 timeout。时间到只改变以后新 key 的判定，不会主动发事件。

### ignored 在取得 key 时锁存

取得 key 的当帧只调用一次 `shouldDropVirtualKey()`：

- ignored=true：仍设置 `down=true` 并消费；不发 DOWN；
- deadline 后来过期：不会补发迟到 DOWN；
- accepted key 按住后，别的设备再推进 deadline：不会追溯取消；
- ignored key 进入 surface 且已不再命中相同 keyCode：结束 latch，可形成 Motion DOWN，但不发 CANCELED Key UP；
- ignored key 移到另一枚 outside key box、且 first-match keyCode 不同：旧 latch 结束，新 key 重新采样当前 deadline；若新测试点 inside，则转 touch 而不取得新 key。

若 ignored key 一直留在相同 keyCode 到抬起，整段既无 KeyEvent 也无 MotionEvent。它之所以消失，是 Virtual Key 状态机持续消费，不是 quiet 判断每帧重新执行。

---

## 15. NotifyKey、reset 与 dumpsys：三个完成点不能互相替代

### dispatchVirtualKey 实际写出的字段

`dispatchVirtualKey()` 构造：

| 字段 | 值 |
|---|---|
| source | 固定 `AINPUT_SOURCE_KEYBOARD` |
| displayId | 源码字面取 `mViewport.displayId` |
| policyFlags | 继承本帧 flags，再 OR `POLICY_FLAG_VIRTUAL` |
| action | DOWN 或 UP |
| event flags | FROM_SYSTEM、VIRTUAL_HARD_KEY，取消时再加 CANCELED |
| keyCode / scanCode | active latch |
| metaState | InputReader 全局 meta |
| downTime | 首次取得该 key 的 when |
| eventTime | 当前帧 when |

initial raw down 若设备 `touch.wake=true` 或按默认规则需要唤醒，传入的 policyFlags 还可能已有 WAKE。反过来，`configureVirtualKeys()` 保存的 `virtualKey.flags` 从未在这个函数读取：KL 的 WAKE/FUNCTION 等 map flags 不能仅凭字段存在就断言已进入这枚 KeyEvent。

`TouchInputMapper` 查询 key/scan state 时只检查 `mCurrentVirtualKey.down`；ignored latch 也会报告 `AKEY_STATE_VIRTUAL`，尽管它从未向 Dispatcher 发 DOWN。`sourceMask` 在这三个 virtual-key 查询实现中也没有参与筛选。

### 真 reset 与动态 NotifyDeviceReset 不是一件事

真正的 `TouchInputMapper::reset()`：

```text
mCurrentVirtualKey.down = false
```

它不补普通 UP 或 CANCELED UP，也不清 InputReader 全局 quiet deadline。`InputDevice::reset()` 在 mapper reset 后发送 `NotifyDeviceReset`，InputDispatcher 再按 deviceId 给各连接合成取消；这不是一枚正常 KeyEvent UP。

动态 viewport/device-mode reconfigure 更微妙：

- surface block 会清并重建 `mVirtualKeys` hit boxes；
- 会发 `NotifyDeviceReset`；
- 但不会调用 mapper `reset()`，所以 `mCurrentVirtualKey.down` 仍可保留。

由源码可推得：下游可能已经因 DeviceReset 取消旧 key，mapper 却继续按新 hit boxes 检查旧 latch；若仍命中相同 keyCode，它不会补新 DOWN，后来却可能产生 UP。`cancelTouch()` 也只取消 touch/pointer usage，不清 virtual-key latch。诊断 reconfigure 竞态时必须把 mapper latch 与 Dispatcher state 分账。

### stock dump 能证明什么

`dumpsys input` 能看到：

- InputReader 配置中的 `VirtualKeyQuietTime`；
- mapper 的 resolved calibration、format 后 factors；
- MotionRanges；
- virtual key 的 scanCode/keyCode/raw hit rectangle；
- Last Raw 与 Last Cooked pointer state。

它看不到：

- `mCurrentVirtualKey.down/ignored/downTime`；
- InputReader 的 `mDisableVirtualKeysTimeout` 当前 deadline；
- 哪个重叠 hit box 刚刚 first-match；
- 本帧是否走了 cancel→重新入场。

`DEBUG_VIRTUAL_KEYS` 在 stock r48 为 0。quiet drop 本身会留一条 InputReader INFO 日志，其余逐帧路径若要确认，需要调试构建打开受控日志或插桩。只看 App 日志，无法区分“map 未加载”“点未命中”“off-screen 帧被吞”和“命中但 ignored”。

### 最小排查证据集

按顺序收集：

1. canonical device name 与 sysfs map 是否成功解析；
2. KCM/KL 最终 keyCode、definition 顺序；
3. raw X/Y、touching id bits 与最低 id；
4. raw axis min/max、surface bounds 与 raw hit rectangle；
5. quiet 配置、最近其他触摸设备活动；
6. calibration、factors、Last Raw/Cooked；
7. NotifyKey/NotifyMotion 或临时状态日志。

这样才能回答“这帧为何没有到 App”，而不是把所有无事件都归因于 quiet time。

---

## 16. 用九个只读练习把“校准还是改道”变成可复算模型

以下命令均在 Android 11 r48 源码根目录执行。每题都有明确产物，不要求 macOS 上编译 AOSP。

### 练习一：证明消费发生在 cooking 前

```bash
sed -n '340,390p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1506,1545p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

产物：画出 `configure(first only)` 与 `cookAndDispatch` 两条顺序线，圈出 `consumeRawTouches()==true` 后只清 `rawPointerData`、函数仍继续执行的位置。

### 练习二：做一张“IDC×raw axis”resolve 表

```bash
sed -n '1110,1245p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '65,84p' \
  frameworks/native/services/inputflinger/reader/mapper/SingleTouchInputMapper.cpp
sed -n '333,352p' \
  frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
```

至少推演四例：显式 pressure 但无 axis、DEFAULT size 且只有 toolMajor、orientation=none 但 Single 有双 tilt、coverage=box 但无 tool axes。说明为什么后两例不能套“缺轴必回 NONE”。

### 练习三：复算 SIZE 与 major/minor 的两条链

```bash
sed -n '790,870p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2018,2128p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
awk 'BEGIN {
  printf("SIZE=%.3f\n", 8.5 / 100);
  printf("TOUCH=%.3f TOOL=%.3f\n", sqrt(12.5) * 2 + 1, sqrt(18) * 2 + 1);
}'
```

用第 6 节的两指 AREA 参数核对输出，再指出 `touch.size.scale/bias`、AREA、VECTOR 中哪些步骤会改 SIZE，哪些只改 major/minor。

### 练习四：区分 pressure、distance 与外接笔覆写

```bash
sed -n '847,951p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2114,2175p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1632,1658p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

分别计算 raw pressure min=10/max=110/value=10 的默认结果，以及 distance scale=-2 时的 range。确认代码为何既不减 min，也不会替负 distance range 重排端点。

### 练习五：解码 VECTOR，并找到 orientation=NONE 的旋转异常

```bash
sed -n '868,932p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2128,2255p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '190,220p' \
  frameworks/native/libs/input/Input.cpp
awk 'BEGIN {
  pi = atan2(0, -1);
  printf("orientation=%.6f shapeScale=%.6f\n",
         atan2(1, -1) / 2, 1 + sqrt(2) / 16);
  printf("3pi/8=%.6f\n", 3 * pi / 8);
}'
```

产物：解释 `0x1f` 的两个 nybble、VECTOR 为什么晚于 size scale/bias，以及 calibration NONE 在 90° surface 上为何仍能把非零 orientation 写入稀疏 coords。

### 练习六：把 coverage packed words 展开成 GENERIC_1..4

```bash
sed -n '179,235p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2175,2268p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

手算 `toolMinor=0x001e0050`、`toolMajor=0x00280064`，再标出 center affine 与 coverage 四边分叉的位置。解释为什么 payload 不写 TOOL_MAJOR/MINOR，却仍可能在 DeviceInfo 看到对应 range。

### 练习七：闭合 sysfs→parser→KCM/KL→raw hit box

```bash
sed -n '1395,1416p;1632,1645p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '45,158p' \
  frameworks/native/libs/input/VirtualKeyMap.cpp
sed -n '545,586p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '1045,1095p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

产物：写出 canonical filename、同行两项的完整分隔格式、KCM/KL 优先级、单项 map 失败与整份 parse 失败的不同结果，并手算一个闭区间 hit box。

### 练习八：逐帧推演 key、off-screen 与 quiet

```bash
sed -n '1725,1842p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '397,410p;736,748p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
sed -n '3656,3682p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

至少写出五条序列：正常按键、不同 keyCode 换键、相同 keyCode/不同 scanCode、active key 加第二指且最低 id 在外、ignored key 过期后抬起。对 deadline-1、deadline、deadline+1 分别判定 drop。

### 练习九：核对 reset、可见性与下游 repeat

```bash
sed -n '340,392p;755,790p;1018,1028p;1359,1392p;3618,3622p;3868,3912p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '327,333p;460,463p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '397,410p;650,670p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
sed -n '24,36p' \
  frameworks/native/services/inputflinger/reader/Macros.h
sed -n '1008,1048p;1065,1075p;1111,1136p;3088,3105p;3254,3272p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2008,2020p' \
  frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '1495,1505p' \
  frameworks/base/core/res/res/values/config.xml
```

产物：分开写 mapper reset、dynamic NotifyDeviceReset、global quiet deadline、mapper 单次 DOWN 与 Dispatcher repeat 五本账；再列出 stock dumpsys 能见与不能见的字段。

### 检查题

1. 为什么 `SIZE=0.5` 不能推出 TOUCH_MAJOR 是 surface 对角线的一半？
2. 为什么 IDC 写 `orientation=none`，SingleTouch 的双 tilt 仍可产生 orientation？
3. 两指初始帧中，一指 inside、一指 outside 时，哪枚 id 决定整组是否被吞？
4. ignored key 的 deadline 已过，为什么不会补发 DOWN？
5. 同一 keyCode 的不同 scanCode 之间移动，最终 UP 为什么仍带旧 scanCode？
6. 为什么 `NotifyDeviceReset` 已取消下游 key，不能推出 mapper 的 virtual-key latch 已清？

### 下一章

第 186 章进入 InputDispatcher 窗口命中与 `TouchState`：从 display 窗口栈、touchable region、split touch 与 wallpaper 复制，追一条 MotionEntry 怎样建立、粘住并更新触摸目标。
