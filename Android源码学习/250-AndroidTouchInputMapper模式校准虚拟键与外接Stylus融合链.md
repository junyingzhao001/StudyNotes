# 250 Android TouchInputMapper模式、校准、虚拟键与外接Stylus融合链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 先把问题边界画清：本章研究控制面与两条旁路

第249章已经把 `EV_ABS`、MT slot、pointer id、`PointerCoords` 和 `NotifyMotionArgs` 串成了坐标生产主链。本章只追那些会改变主链语义、却不负责生产 MT 坐标的机制：

- `.idc` 与内核能力怎样决定 `deviceType`、`DeviceMode`、`mSource` 和显示关联；
- `DisplayViewport` 怎样被选中，何时只改内存，何时还会 reset、bump generation；
- size、pressure、orientation、tilt、distance、coverage 怎样从原始整数变成 Motion 轴；
- sysfs virtual-key map 怎样把屏外触点改写为 `NotifyKeyArgs`；
- 一个只有压力、按键而没有 X/Y 的外笔，怎样借用触摸屏的坐标与 pointer id。

可以先记住四个相互独立的完成点：

1. `configure()` 返回：本地字段已经算完，不代表消费者已收到设备变化；
2. `bumpGeneration()`：设备描述具备被重枚举的条件；
3. `NotifyDeviceResetArgs`：下游应终止旧输入状态；
4. `notifyMotion()/notifyKey()`：某个具体输入事件才真正进入监听器。

把这四者混成“配置完成”，是阅读本章最常见的误区。

本章的总链可压缩为：EventHub 能力与配置 → `configureParameters()` → mode/source → `findViewport()` 与 `configureSurface()` → 校准与虚拟键命中框。运行期的精确次序是：touch `RawState` 排队 → external stylus id 等待/分配 → 合入外笔 buttons → virtual-key 分流 → cook 坐标与触屏轴 → 覆盖外笔 pressure/tool type → 普通 dispatch；“融合”不是一个单独调用点。

## 2. configure 的双点：`changes == 0` 不是不可重入保证

`TouchInputMapper::configure()` 每次都会调用基类，并把整个 `InputReaderConfiguration` 复制到 `mConfig`。但四组工作受不同条件控制：

| 工作 | 触发条件 | 主要结果 |
|---|---|---|
| 参数、accumulator、原始轴、parse/resolve calibration | `!changes` | 重建能力相关状态 |
| affine | `!changes` 或 affine change bit | 更新位置仿射矩阵 |
| pointer/wheel velocity | `!changes` 或 pointer-speed bit | 更新速度控制器参数 |
| surface/source/mode | `!changes` 或 display、gesture、show-touches、external-stylus bit | 可能重算 viewport、range、虚拟键 |

源码在 `if (!changes)` 后写着“first time only”，它表达设计意图，却不是 r48 调用图提供的强不变量。新增 EventHub 设备时，`createDeviceLocked()` 若找到相同非空 descriptor，会复用旧 `InputDevice`、先加入新子设备，再由 `addDeviceLocked()` 对全部 mapper 调 `configure(..., 0)` 和 `reset()`。组合设备删除一个 EventHub 子设备时，`removeDeviceLocked()` 也会保留仍有子设备的同一个 `InputDevice`，对它调用 `configure(..., 0)`，随后调用 `reset()`。因此：

- `InputDevice` 会清空并按当前子设备重新合并 `mConfiguration`；
- `TouchInputMapper` 的 parameters、原始轴与 calibration 也会重新跑；
- 旧引用若把 `changes == 0` 解释为“对象生命期恰好一次”，会漏掉组合设备扩张与收缩路径。

共享配置的冲突也没有稳定优先级契约。子设备保存在 `unordered_map`，`addAll()` 按迭代顺序把同名 property 替换为后加入值；若多个子设备声明同一 key，谁最后覆盖取决于无序容器遍历，不应把某个 EventHub id 或插入先后当成规则。新增/删除造成重建时，这类冲突尤其可能显形。

普通非零 change bit 又是另一条路：只有 `configureSurface()` 报告 `resetNeeded` 时，mapper 才直接发 `NotifyDeviceResetArgs`。零 changes 时它不在这里发 reset，因为新增路径由 reader 在 mapper 齐备后统一 reset，组合设备收缩也由移除路径紧接着执行 `device->reset()`。相同实参值，不等于只有一种调用语境。

### 练习 1：证明零 changes 存在第二种调用语境

先分别定位 mapper 与 device 中的“first time only”分支，再沿新增、删除两条路径找零 changes 重配和随后的 reset。回答：新加入或被移除的子设备贡献了某个 `.idc` 属性时，mapper 下一次读取的是旧合并表还是重建后的表？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (!changes) { // first time only" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (!changes) { // first time only" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
grep -n -F "device = deviceIt->second;" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "device->configure(when, &mConfig, 0);" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "device->reset(when);" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "mConfiguration.clear();" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
```

答案是重建后的表：`InputDevice::configure(..., 0)` 先 clear，再从当前 `mDevices` 的各 `InputDeviceContext` 合并属性；新增子设备已经进入集合，被删子设备已经离开集合。

## 3. configureParameters：先由能力推断，再让有效 idc 覆盖

`configureParameters()` 的决策顺序很重要。

先别越过对象创建边界：EventHub 必须先把设备分到 `INPUT_DEVICE_CLASS_TOUCH` 或 `TOUCH_MT`，`InputDevice::addEventHubDevice()` 才会创建 Single/MultiTouchInputMapper。下面的 idc 只配置已存在的 mapper，不能把一台没有触摸 class 的任意设备凭空变成触屏。

`gestureMode` 先看 `INPUT_PROP_SEMI_MT`：有该属性默认 single-touch，否则默认 multi-touch；`touch.gestureMode` 可写 `single-touch`、`multi-touch` 或 `default` 覆盖/保留。这个参数说的是 pointer gesture 的呈现能力，不会把 Protocol A 驱动改造成 Protocol B。

`deviceType` 的能力推断按互斥顺序执行：

1. 有 `INPUT_PROP_DIRECT` → `touchScreen`；
2. 否则有 `INPUT_PROP_POINTER` → `pointer`；
3. 否则有 `REL_X` 或 `REL_Y` → `touchPad`，意图是别让附着在 cursor device 上的 pad 默认移动指针；
4. 都没有 → `pointer`。

随后，合法的 `touch.deviceType` 可覆盖为 `touchScreen`、`touchPad`、`touchNavigation` 或 `pointer`；`default` 保留推断值，其他字符串只告警。`orientationAware` 默认只对 touchScreen 为真，但 `touch.orientationAware` 可覆盖。

显示关联不是 `orientationAware` 的同义词。满足以下任一条件就把 `hasAssociatedDisplay` 设为真：orientation-aware、deviceType 是 touchScreen、deviceType 是 pointer，或者 `InputDeviceContext` 有 associated display port。只有 touchScreen 分支会读取 `touch.displayId`，并用设备 external 属性初始化 `associatedDisplayIsExternal`。`touch.wake` 则默认等于设备是否 external，之后也可被 idc 覆盖。

一个实用反例是：把 touchPad 的 `touch.orientationAware` 配成 true，会让它进入“有关联显示”的 viewport 路径，但后面的 mode 仍可能是 UNSCALED；参数之间不是一枚总开关。

## 4. DeviceMode 与 source：矩阵有优先级，也有降级分支

`configureSurface()` 每次先调用 `resolveExternalStylusPresence()`，再按以下顺序选 mode/source：

| 条件 | `mDeviceMode` | 基础 `mSource` | 可附加 source |
|---|---|---|---|
| deviceType=pointer 且 pointer gestures 开启 | POINTER | MOUSE | 本机 stylus |
| deviceType=touchScreen 且有关联显示 | DIRECT | TOUCHSCREEN | 本机 STYLUS、全局 BLUETOOTH_STYLUS |
| deviceType=touchNavigation | NAVIGATION | TOUCH_NAVIGATION | 无 |
| 其余 | UNSCALED | TOUCHPAD | 无 |

这张表有三个容易反向理解的点。

第一，pointer type 在 `pointerGesturesEnabled=false` 时不会保持 POINTER，它落入最后的 UNSCALED/TOUCHPAD。第二，`hasStylus()` 是触摸 mapper 自己从 MT tool type 或 touch buttons 看出的能力；`hasExternalStylus()` 只是 reader 中是否存在任意未忽略的 external-stylus device，两者不是同一个发现链。第三，external stylus source 只附加给 DIRECT touchscreen；它不会让 POINTER、NAVIGATION 或 UNSCALED 进入融合状态机。

mode/source 在验证 X/Y 和 viewport 之前就已写入。缺轴或找不到 viewport 时，代码只把 mode 改成 DISABLED 并提前返回；此前写入的 source 不会同步清零，旧 viewport、pointer controller、virtual keys 等缓存也没有在这两个分支统一清掉。`populateDeviceInfo()` 又会照常 addSource。因此“mode disabled”既不能推出“`getSources()==0`”，也不能推出所有旧描述字段已失效。

### 练习 2：手算四组输入的 mode/source

分别推导：DIRECT 属性的 MT 屏；POINTER 属性且 gestures 关闭的 pad；idc 强制 touchNavigation 的设备；有本机笔能力且连接了外接笔的 direct 屏。然后用分支顺序核对，别按枚举名字猜结果。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_SCREEN;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_PAD;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_NAVIGATION;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mConfig.pointerGesturesEnabled" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSource = AINPUT_SOURCE_MOUSE;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSource = AINPUT_SOURCE_TOUCHSCREEN;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSource |= AINPUT_SOURCE_BLUETOOTH_STYLUS;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSource = AINPUT_SOURCE_TOUCHPAD;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
```

DIRECT 屏是 TOUCHSCREEN；gestures 关闭的 pointer 是 UNSCALED/TOUCHPAD；强制 navigation 是 NAVIGATION/TOUCH_NAVIGATION；最后一组是 DIRECT，source 同时含 TOUCHSCREEN、STYLUS 与 BLUETOOTH_STYLUS。

## 5. findViewport：几个“立即返回”决定了 fallback 是否存在

当 `hasAssociatedDisplay` 为真，`findViewport()` 采用严格次序：

1. 若有 associated display port，立即返回 `getAssociatedViewport()`；未匹配时得到空值，不再尝试其他 viewport。
2. 若 mode 是 POINTER，先找 `defaultPointerDisplayId`；找到就返回，找不到只告警，继续向后降级。
3. 若 idc 给了非空 `uniqueDisplayId`，立即按 unique id 返回；未匹配同样不会降级。
4. 否则按 external/internal 类型选 viewport；external 缺失时单向退到 internal，internal 缺失时没有反向退路。

当 `hasAssociatedDisplay` 为假，函数不查配置中的显示列表，而是用原始 X/Y 宽高构造 non-display viewport。

所以不能把选择规则简化为“port → id → type，任何一步失败都继续”。port 与 unique-id 分支是强约束，pointer display 的失败才会继续；external→internal 又是仅此一处的单向兜底。各个 lookup 只按 port/id/type 找对象，不在这里检查 `DisplayViewport::isValid()` 或 active；“找到了”也不等于几何合法。

更高一层还有一扇门：`InputDevice::configure()` 在 display-info 变化时会按 input port 查 viewport，查不到便 `setEnabled(false)`。mapper 内 `findViewport()` 的 DISABLED 与 device 层真正关闭 EventHub fd 是两种不同状态，排障时要同时看。

POINTER 还有两位消费者：TouchInputMapper 找不到 `defaultPointerDisplayId` 时会继续 unique-id/type fallback；全局 `updatePointerDisplayLocked()` 给 PointerController 选 viewport 时却只退到 display 0，仍找不到就保留 controller 的旧 viewport。于是 mapper surface 与光标 controller 可以在异常配置下分叉，不能用一边的 dump 替另一边作证。

### 练习 3：画出三个缺失 viewport 的不同结果

比较 port 已配置但对应 viewport 缺失、unique id 已配置但找不到、external 类型 viewport 缺失但 internal 存在。分别回答是否继续 fallback、mapper 是否可能 DISABLED，以及 device 层是否可能直接 disable。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "const std::optional<uint8_t> displayPort = getDeviceContext().getAssociatedDisplayPort();" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "return getDeviceContext().getAssociatedViewport();" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mConfig.defaultPointerDisplayId" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "return mConfig.getDisplayViewportByUniqueId(mParameters.uniqueDisplayId);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "viewportTypeToUse == ViewportType::VIEWPORT_EXTERNAL" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "newViewport.setNonDisplayViewport(rawWidth, rawHeight);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mAssociatedViewport = config->getDisplayViewportByPort(*mAssociatedDisplayPort);" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
```

port 缺失与 unique-id 缺失都让 mapper 得到空 viewport；只有 port 情形还会在 device 层触发 enable 状态收敛。external 类型缺失可以退到 internal，不必因此禁用。

## 6. configureSurface：重算、reset、generation 不是同一步

拿到非空 viewport optional 后，代码才比较 `viewportChanged`。而且只有 viewport 对象真的变化，DIRECT/POINTER 才把旋转 viewport 还原到 natural surface，计算 logical/physical/device 尺寸、surface 边界和 `mSurfaceOrientation`；其他 mode 才改用 raw X/Y 宽高。physical 宽或高为零时只把分母修成 1 继续算，并没有拒绝这个 viewport。

随后才比较 `deviceModeChanged`。mode 变化先清空 oriented ranges；POINTER 或 DIRECT+showTouches 才需要 pointer controller。`viewportChanged || deviceModeChanged` 为真时，后半段会：

- 重算 X/Y scale、translate、precision 与所有 oriented ranges；
- 重新生成 virtual-key hit boxes；
- 更新 affine，并在 POINTER 中重算 gesture 参数、abort 旧 pointer usage；
- 置 `*outResetNeeded=true` 并 `bumpGeneration()`。

这里有一处比 source 缺口更早的 mode 交叉边界：surface 几何只受 `viewportChanged` 保护，后半段 scale/range 却受 viewport 或 mode 任一变化保护。pointer device 切换 `pointerGesturesEnabled` 可在同一个 viewport 上发生 POINTER↔UNSCALED；此时 mode 已变、viewport 对象未变，后半段会拿“旧 mode 留下的 `mRawSurfaceWidth/Height` 等字段”重算 scale/range。也就是说，mode change 会 reset/generation，不保证 mode-dependent surface geometry 已按新 mode 重建。

这产生一个 r48 可观察缺口：external stylus presence change 会重跑 `configureSurface()`，也可能只让 DIRECT 屏的 `mSource` 增减 `AINPUT_SOURCE_BLUETOOTH_STYLUS`。若 viewport 和 mode 都没变，重算大块不进入，mapper 不 bump generation，也不要求 reset；`InputDevice::configure()` 却仍会从 mapper 重新 OR 出 `mSources`。因此目标触屏的 source 已变，而它自己的 `InputDevice.mGeneration` 没变。

实际 presence 变化来自 external device 的 add/remove 时，那条拓扑路径会另外 bump reader 全局 generation，所以本轮末尾仍可发布整份设备列表，不能说“系统一定看不到 source”。精确结论是：通知由拓扑变化兜底，目标触屏自身的 generation/reset 并没有与 source 变化联动。

另两个提前返回也在大块之前：X/Y 无效、viewport 为空都会把 mode 设为 DISABLED，却不在 mapper 内设置 resetNeeded 或 bump generation。device 层 enable/disable 可能另行产生 reset/generation，不能把它当成该提前返回自身的保证。

### 练习 4：构造“source 变、目标设备 generation 不变”

固定同一个 DIRECT touchscreen 与同一个 viewport，仅添加一个 external-stylus device。沿 presence change → configureSurface → source OR → viewport/mode 比较追踪。指出哪一行改变触屏 source，哪一个条件挡住触屏自身 bump；再找出为何 reader 全局 generation 仍会因设备新增而变化。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "resolveExternalStylusPresence();" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSource |= AINPUT_SOURCE_BLUETOOTH_STYLUS;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "bool viewportChanged = mViewport != *newViewport;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (viewportChanged) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "bool deviceModeChanged = mDeviceMode != oldDeviceMode;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (viewportChanged || deviceModeChanged) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "*outResetNeeded = true;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mSources |= mapper.getSources();" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
grep -n -F "bumpGenerationLocked();" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
```

触屏改变发生在 source OR；阻挡点是最后的 viewport/mode 二选一条件。reader 的另一次变化来自新增设备路径的 `bumpGenerationLocked()`。这个例子说明 target-device generation、reader generation、source 与 reset 是四条状态线。

## 7. calibration 是两阶段决策，affine 是另一条动态配置线

`parseCalibration()` 只负责把字符串和数值装入 `Calibration`，未知字符串保留 DEFAULT 并告警。`resolveCalibration()` 再结合原始轴把 DEFAULT 收敛成可执行模式：

| 类别 | 可配置模式 | 默认解析 |
|---|---|---|
| size | none/geometric/diameter/box/area | 有 touchMajor 或 toolMajor → geometric，否则 none |
| pressure | none/physical/amplitude | 有 pressure → physical，否则 none |
| orientation | none/interpolated/vector | 有 orientation → interpolated，否则 none |
| distance | none/scaled | 有 distance → scaled，否则 none |
| coverage | none/box | DEFAULT 永远落到 none |

显式模式也受能力裁决。例如没有 pressure 轴时，即使字符串写 physical，resolve 仍改成 none；没有 major 轴时 size 也会被改成 none。coverage 不检查独立 coverage 轴，因为 box 数据借用了 toolMajor/toolMinor 的位段，这正是它危险的地方。

这些 idc calibration 参数在 `!changes` 分支解析；普通配置 change 不会任意重读它们。位置 affine 则不属于 `Calibration`：`updateAffineTransformation()` 在初始/零 changes、affine change bit，以及 viewport/mode 大块重算时更新。把“校准”作为单个生命周期会错过这条动态线。

## 8. size 与 pressure：公式不减 min，也不统一 clamp

size cooking 先选择原料。若 touch/tool major 都有，各用各的 major/minor；只存在一组时，就把同一组复制给 touch 与 tool。minor 缺失便复制 major；归一化 `SIZE` 使用 touch 或 tool 的 major/minor 平均值。

之后按模式处理：

- geometric：四个 major/minor 乘 `mGeometricScale=(mXScale+mYScale)/2`；
- area：正 major 开平方，minor 被直接设成开方后的 major；
- diameter：minor 被设成 major；
- box：不做上述三种形状变换；
- 若 `touch.size.isSummed=true` 且 touching pointer 超过一个，先把四个尺寸与 size 都除以 touching count。循环中的 hover pointer 也会被这个 touching count 除，尽管 hover 本身不计入分母。

`touch.size.scale/bias` 只作用于四个 major/minor，不作用于归一化 `AMOTION_EVENT_AXIS_SIZE`。后者始终再乘 `mSizeScale`；这个 scale 优先取 `1 / touchMajor.maxValue`，touchMajor 无效或 max 为 0 时才尝试 max 非零的 toolMajor，同样不减 min。area 模式开方的是 major/minor，但独立 `SIZE` 仍用开方前的平均原料归一化。

pressure 的 physical 与 amplitude 在这段 r48 cook 代码里执行同一个 `rawPressure * mPressureScale`。有显式 `touch.pressure.scale` 就直接使用，并把声明 range max 设为 `scale * rawMax`；否则 raw max 非零时取倒数。没有可用 pressure calibration 时，hover 输出 0，非 hover 输出 1。MultiTouch 是否 hover 的更早判定仍可使用“pressure 轴有效且 raw pressure<=0”，不会因为 idc 把 pressure calibration 配成 none 就停止参考该 raw 轴。

不要自行补上源码没有的性质：这些公式不减 axis min，也没有把输出统一 clamp 到 range。四个 major/minor 在应用 size scale、bias 后只有一条下限保护，负值会被钳到 0；独立 `SIZE`、pressure、distance 等没有共享这条保护。`SIZE` 虽声明 [0,1]，负 raw、非零 min 或越界 sample 都可能让实际值越界；major/minor 声明上限虽是 surface diagonal，scale/bias 以及后续 vector 拉伸也可能超过它。physical/amplitude 且没有显式 `touch.pressure.scale` 时，raw max 为 0 的有效 pressure 轴会留下 `mPressureScale=0`，触摸压力也成为 0；显式 scale 仍照用，calibration=none 则由 touching/hover 合成 1/0。正向越界或过大的 scale/bias 仍能让实际值越过声明范围。

### 练习 5：算出一组反直觉的 size/pressure

设 touchMajor=80、touchMinor=20、touchMajor.max=100、两指 touching、`size.isSummed=true`、size mode=area；另设 pressure raw=80、raw max=100、显式 pressure scale=0.02。忽略 size scale/bias。分别算 major/minor、SIZE、pressure 与声明的 pressure max。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "mSizeScale = 1.0f / mRawPointerAxes.touchMajor.maxValue;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "touchMajor /= touchingCount;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "touchMajor = touchMajor > 0 ? sqrtf(touchMajor) : 0;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (*outSize < 0) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h"
grep -n -F "size *= mSizeScale;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "pressure = in.pressure * mPressureScale;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "pressureMax = mPressureScale * mRawPointerAxes.pressure.maxValue;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "pressure = in.isHovering ? 0 : 1;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "orientation -= M_PI_2;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "rawLeft = (in.toolMinor & 0xffff0000) >> 16;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "left = float(mRawPointerAxes.x.maxValue - rawRight) * mXScale;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "left = float(mRawPointerAxes.y.maxValue - rawBottom) * mYScale;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
```

除以两指后 major/minor 原料为 40/10，area 输出 major=minor=√40；`SIZE=((80+20)/2)/2/100=0.25`。pressure=1.6，声明 max=2.0。两组输出并不共享同一种归一化语义。

## 9. orientation、tilt、distance、coverage：同名轴并不走同一路

只要 raw tiltX 与 tiltY 同时有效，tilt 路径就压过 orientation calibration。中心取各自 min/max 平均，原始数值按“度”乘 `π/180`；输出 orientation 是 `atan2(-sin(tiltX), sin(tiltY))`，tilt 是 `acos(cos(tiltX) * cos(tiltY))`。range 声明为 orientation [-π, π]、tilt [0, π/2]，计算后没有额外 clamp。

没有成对 tilt 轴时：

- interpolated 根据 orientation max>0 或 min<0 选择比例，把一侧极值映到 ±π/2；
- vector 把一个字节拆成两个带符号 4-bit 分量，以 `atan2(c1,c2)/2` 求方向，并按 confidence 拉长 major、压短 minor；
- none 在 calibration 分支先把 orientation 置 0。

这个 0 还不是最终值。后面的公共 surface-orientation 分支会无条件对 orientation 做 −π/2、−π 或 +π/2；只有 `mOrientedRanges.haveOrientation=true` 才进入区间回绕。因此 calibration=none 的旋转屏仍可能把非零 ORIENTATION 写进 `PointerCoords`，同时又不对外声明 orientation range。读事件值与读 device range 必须双向核对。

这里存在 SingleTouch 与 MultiTouch 的能力边界：SingleTouch 的原始轴配置会读取 `ABS_TILT_X/Y`；MultiTouch 的 `configureRawPointerAxes()` 只读 `ABS_MT_ORIENTATION`、pressure、distance 等，不给 `mRawPointerAxes.tiltX/tiltY` 赋值。因此通用基类虽然支持 tilt，r48 的 MultiTouch 路径并不会从 MT slot 产出这两个字段。

distance 只是 `rawDistance * distanceScale`，默认 scale=1；range 的 min/max/fuzz 也直接乘这个 scale，所以负 scale 能得到倒序 range 与负 fuzz。

coverage box 把 toolMinor 的高/低 16 位解出 left/right，把 toolMajor 解出 top/bottom；掩码拆位不做 16-bit 符号扩展，四项按 0..65535 使用。它再以自己的分支按 surface orientation 换算到 GENERIC_1..4，而不是调用点坐标的 `rotateAndScale()`。点坐标 X/Y 会先经过 affine，coverage 四边明确没有套同一 affine；180° 的 left/right 又没加 `mXTranslate`，270° 的 left/right 没加 `mYTranslate`，非零 surface offset 下也不会与点完全同变换。四边没有 clamp 或次序校验。

显式 coverage=box 不检查 WIDTH major/minor 是否有效；MultiTouch minor 缺失时 slot getter 又会回退到 major，左右/上下可能复用同一 packed 字。进入 box 后输出占用 GENERIC_1..4，不再写 TOOL_MAJOR/MINOR，而且解包直接读原始 `in.tool*`，不受 size scale/bias 影响。

## 10. virtual-key map：整文件解析、逐键映射、整数命中框

EventHub 只对 touch class 尝试读取 `/sys/board_properties/virtualkeys.<canonicalName>`。文件可读且整份 `VirtualKeyMap::Parser::parse()` 成功，设备才保存 map，并额外带上 KEYBOARD class；文件不存在只是没有虚拟键。

每条定义格式是 type `0x01` 加五个冒号分隔整数：scanCode、centerX、centerY、width、height。type 必须是字面量 `0x01`；整数用 `strtol(..., base=0)`，所以符号、十六进制和八进制都可被接受，却不校验 errno、int32 范围或宽高必须为正。`#` 只在一行开头的有效内容之前表示整行注释，尾随注释会成为行尾残留。未知 type、字段缺失、非整数或行尾残留都会返回错误，`load()` 直接返回空指针；不是“保留此前正确项、跳过坏项”。

空文件或纯注释文件反而会 parse 成功；EventHub 仍把它视为“成功加载 map”并添加 KEYBOARD class，只是 definitions 为空。virtual-key sysfs 文件只在设备打开分类时加载，普通 reader reconfigure 不会重读，修改后需要走 EventHub reopen 才能生效。

`configureVirtualKeys()` 再做第二层过滤：每个 scan code 必须能经 device context 的 `mapKey()` 得到 keyCode，否则只丢这一项。EventHub 实现会先查 KeyCharacterMap，再查 KeyLayout，并在成功后通过 KCM 处理 meta；因此把它简称为“只查 key layout”并不准确。成功项按文件顺序进入 `mVirtualKeys`。映射返回的 flags 被存入 `VirtualKey.flags`，但本链 dispatch 没读取它；最终 KeyEvent 的 source、policy flag 与 event flags 由 `dispatchVirtualKey()` 固定组装。

命中框从显示坐标反算到 raw touch 坐标。width/height 先做整数除 2，各边再执行整数乘除，所以奇数尺寸和不能整除的比例会截断；矩形不裁剪、不去重，负尺寸可形成反向空区，极值乘法也没有溢出保护。`VirtualKey::isHit()` 四边都用 `>=/<=`，边界包含在内；重叠区域中 `findVirtualKeyHit()` 返回 vector 里的第一项。命中框不会经过 affine，这与它在 raw touch 坐标上、且 `consumeRawTouches()` 早于 `cookPointerData()` 的位置一致。

前面 physical 宽高为零时改 1，只保护 viewport 换算的分母；logical/device 尺寸并未在此验证，仍可能把 `mRawSurfaceWidth/Height` 算成 0。若此时又存在 virtual-key definitions，命中框反算会出现整数除零。optional 中“有 viewport”远弱于几何安全。

## 11. virtual-key 状态机：按键、触摸和取消之间怎样切换

虚拟键只在一次“raw 上一帧无 touching、当前帧有 touching”的新按下中检查，而且首个触点必须位于 surface 外。一个触点命中时，`mCurrentVirtualKey.down=true`；是否真正发 KEY_DOWN 由 reader 的全局 quiet-time 门决定，但即使 ignored，整条虚拟键 stroke 仍被消费。

后续帧有三类结果：

- 仍只有一个触点，且命中同 keyCode：继续消费，不重复发 KEY_DOWN；
- 所有触点抬起：若未 ignored，发 KEY_UP，然后消费这一帧；
- 不再命中同 keyCode，或出现第二指：清除当前虚拟键；只有原键未被 quiet time 标成 ignored 时，才先发带 CANCELED 的 KEY_UP，再继续处理当前帧。

“继续处理”很关键。此前虚拟键帧在 `cookAndDispatch()` 中被 clear 后才复制到 `mLastRawState`，所以 last raw 看起来没有触点。若当前点已滑回 surface 内、并且不再被 `findVirtualKeyHit()` 判为同 keyCode，DIRECT 模式会从普通 Motion 的全新 DOWN 开始；POINTER 模式则进入 pointer usage，普通单指通常先是 HOVER_MOVE，不能套用 DIRECT 的 action。若仍在屏外并命中另一虚拟键，也可能同帧取消旧键并按下新键。它不是从原按键无缝转换出的 MOVE。

活动虚拟键检查早于 `isPointInsideSurface()`，所以“进入 surface”本身不是取消条件。配置错误可让 virtual-key hit box 与 surface 重叠；点即使已经在屏内，只要第一命中项仍是同 keyCode，就继续被消费。判断只比 keyCode，不比 scanCode 或 definition。反过来，重叠键按文件顺序返回第一项：当前键的矩形仍覆盖该点，也可能因为更早条目映射到不同 keyCode 而被取消。

surface 与 hit box 都含边界，但初始和持有阶段的优先级相反：初始 DOWN 先要求点在 surface 之外，共享边界归普通 surface；虚拟键已经激活后先查 hit box，共享边界若仍首命中同 keyCode，就继续归虚拟键。

若新 stroke 的首点在 surface 外、没有命中键，或者一开始就是多指屏外，`consumeRawTouches()` 直接消费该帧。之后只要 last raw 仍因消费而为空，仍在屏外的帧可能反复按“新按下”检查。

### 练习 6：推演从 BACK 区滑入屏内

假设 mapper 是 DIRECT、BACK hit box 不与 surface 重叠，并且全局 quiet time 未把这次按键标成 ignored。依次给出屏外 BACK down、仍在 BACK、滑到屏内、屏内 move、up。写出 Key 与 Motion action，并说明 Motion 为什么从 DOWN 而不是 MOVE 开始。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "if (mCurrentVirtualKey.down) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "virtualKey && virtualKey->keyCode == mCurrentVirtualKey.keyCode" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "AKEY_EVENT_FLAG_CANCELED" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (!isPointInsideSurface(pointer.x, pointer.y)) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mCurrentVirtualKey.ignored =" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mCurrentRawState.rawPointerData.clear();" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mLastRawState.copyFrom(mCurrentRawState);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
```

序列是 KEY_DOWN、无新事件、KEY_UP|CANCELED 加 Motion DOWN、Motion MOVE、Motion UP。clear 后保存的 last raw 为空，正是新 Motion stream 的边界。

## 12. quiet time 是 reader 全局时钟；reset 靠 DeviceReset 收口

只要一帧未被 virtual-key/off-screen 分支消费、当前仍有 touching，且 `virtualKeyQuietTime>0`，TouchInputMapper 就调用 `disableVirtualKeysUntil(when + quietTime)`。这个 deadline 存在 `InputReader`，不是某个触屏或某个 key 的成员。因此一个屏内触摸可以压制另一块设备随后发生的虚拟键。

`disableVirtualKeysUntilLocked()` 是直接赋值，不取 max。reader 通常按时间顺序处理 raw events，所以 deadline 通常向前推进；但函数自身并不保证单调。`shouldDropVirtualKeyLocked()` 使用严格 `now < deadline`，恰好等于 deadline 时允许按键。

quiet-time 判断发生在新虚拟键 acquisition 中、普通触摸更新 deadline 之前，结果被锁存在 `mCurrentVirtualKey.ignored`。被判 ignored 的 virtual key 会记录 down 状态并吞掉 stroke，只是不发 DOWN/UP；状态查询仍会因为 `down=true` 报 `AKEY_STATE_VIRTUAL`。旧 deadline 到期不会补发 DOWN，新的 quiet window 也不会追溯取消已经发出的 held key。它不会在命中本帧反过来延长 quiet time。

`TouchInputMapper::reset()` 只是把 `mCurrentVirtualKey.down=false`，不会单独合成 KEY_UP/CANCEL。正常 `InputDevice::reset()` 会在所有 mapper reset 后通过 `notifyReset(when)` 发设备 reset，Dispatcher 应以设备级 reset 清理旧状态。审计 trace 时，找不到成对 KeyEvent 不一定是泄漏；要继续找 DeviceReset。

但增量 surface 重配是另一条路径：`TouchInputMapper::configure()` 可直接发 `NotifyDeviceResetArgs`，却不调用自身 `reset()`。若此刻虚拟键仍 down，mapper 内状态会保留；下游先收到 DeviceReset，未来物理抬起时 mapper 仍可能再发 KEY_UP。不能从通知名字反推出 mapper 已清空。

全局 `mDisableVirtualKeysTimeout` 也不在 mapper/device reset 或 device removal 中清除。设备拓扑已经变化，旧屏内触摸留下的 quiet deadline 仍可短暂影响新设备。

### 练习 7：区分全局 deadline 与单设备按键状态

令设备 A 在 t=100ms 产生屏内触摸，quiet time=50ms；设备 B 分别用两条独立 stroke（中间完整抬起）在 t=149、150ms 尝试虚拟键。再让 mapper 在一次未抬键时 reset。判断两次是否发 KEY_DOWN，以及 reset 是否直接发 KEY_UP。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "getContext()->disableVirtualKeysUntil(when + mConfig.virtualKeyQuietTime);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mDisableVirtualKeysTimeout = time;" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "if (now < mDisableVirtualKeysTimeout) {" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "mCurrentVirtualKey.down = false;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "for_each_mapper([when](InputMapper& mapper) { mapper.reset(when); });" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
grep -n -F "notifyReset(when);" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
```

t=149 的独立 stroke 被压制，t=150 的下一条独立 stroke 通过严格边界；若 149ms 后一直不抬起，deadline 到点不会给 ignored stroke 补 KEY_DOWN。mapper reset 本身不发 KEY_UP，设备级 reset 才是下游收口信号。

## 13. ExternalStylusInputMapper：只生产状态，不生产坐标

EventHub 把一种特殊设备归为 `INPUT_DEVICE_CLASS_EXTERNAL_STYLUS`：前面的 MT 与单点坐标分类都未命中，它有 `ABS_PRESSURE` 或 `BTN_TOUCH`，并且没有成对的 `ABS_X`、`ABS_Y`。因此“没有 X/Y”仍不是充分条件：设备若同时具备成对 `ABS_MT_POSITION_X/Y`，会先被 MT touch 分支截走，根本不会走 external-stylus 分支。命中该分支后还会移除 KEYBOARD class，以免笔按钮先被键盘 mapper 占走。这是能力分类，不是根据蓝牙 transport 名称判断。

`InputDevice::addEventHubDevice()` 为该 class 创建 `ExternalStylusInputMapper`。它宣称的 source 是 STYLUS，device info 只添加 [0,1] pressure range；每个 `SYN_REPORT` 清空并重建 `StylusState`：

- `when` 取这个 pen SYN 的时间；
- tool type 取 `TouchButtonAccumulator`，UNKNOWN 会改成 STYLUS；
- 有 raw pressure axis 时，pressure 直接是 `raw / rawMax`；
- 没有 pressure axis 但 tool active 时是 1，否则 0；这里的 active 包含 `BTN_TOOL_PEN` 一类 proximity 状态，不只 `BTN_TOUCH`；
- buttons 取 accumulator 当前 button state。

这段归一化不减 raw min、不 clamp，也没有检查 `rawMax==0`。所以“device info 声明 0..1”不是运行值安全落在 0..1 的证明。有有效 pressure 轴时也不再检查 tool active。`configure()` 每次都重读 pressure axis、配置 button accumulator，它没有用 change mask 缩小工作。

最后，mapper 不发 Motion；它只调用 `dispatchExternalStylusState()`。坐标必须来自另一台 TouchInputMapper，这就是“外接笔融合”而非一台完整坐标笔设备。触屏会先按自己的 calibration cook pressure，融合阶段再用 external pressure 直接覆盖，外笔数值不会重新走触屏的 pressure scale。

## 14. 广播与配对：全局 presence、全局 state、最低 touching id

external-stylus device 添加或移除时，`notifyExternalStylusPresenceChanged()` 触发全 reader 配置刷新。`getExternalStylusDevicesLocked()` 只要找到任意未忽略 external stylus，所有 TouchInputMapper 的 `mExternalStylusConnected` 就为真；没有按 display、port、descriptor 或物理邻近关系配对。
运行状态也是全局广播。`StylusState` 只有 when、pressure、buttons、toolType，没有来源 device id。`InputReader::dispatchExternalStylusState()` 遍历 `mDevices`，对每个 `InputDevice` 调 `updateExternalStylusState()`；device 又把同一个 state 交给自己的全部 mapper。真正消费它的 TouchInputMapper 仍要求 DIRECT 且 external presence 为真。若同时连接多支外笔，它们写的是同一份 mapper 缓存，语义是全局 last-writer-wins，而非逐笔隔离。

初始融合也不做几何匹配：当上一 raw state 的 `pointerCount==0`、当前不为 0，且缓存 stylus pressure 非零时，直接取 `touchingIdBits.firstMarkedBit()` 作为 `mExternalStylusId`，也就是最低 Android touching id。缓存 state 没有“必须足够新”的年龄判断；正常设备依赖 pen-up state 把 pressure 归零。

这里还有组合设备放大效应。`InputReader::mDevices` 以 EventHub id 为键，同一个合并后的 `shared_ptr<InputDevice>` 可出现多次。全局广播按 map entry 遍历，因而同一逻辑设备可能收到重复 update；第一次 update 若刚释放了 72ms 待处理 DOWN，第二次相同 update 已看到 active stylus id，可能再建立 20ms data-pending timeout，最终产生一次额外的合成刷新。这是从容器结构和状态机共同推出的边界，不是“一支笔只定向通知一块屏”。

### 练习 8：验证广播为何不是一对一

找出 presence 枚举、state 广播、device 内 mapper 广播和初始 id 选择四层。回答：两块 direct 屏同时有新触点时，源码在哪一步选择目标屏？组合设备为什么可能多收一次相同 state？

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "device->classes |= INPUT_DEVICE_CLASS_EXTERNAL_STYLUS;" "frameworks/native/services/inputflinger/reader/EventHub.cpp"
grep -n -F "notifyExternalStylusPresenceChanged();" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "device->getClasses() & INPUT_DEVICE_CLASS_EXTERNAL_STYLUS" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "void InputReader::dispatchExternalStylusState(const StylusState& state)" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "std::unordered_map<int32_t /*eventHubId*/, std::shared_ptr<InputDevice>> mDevices;" "frameworks/native/services/inputflinger/reader/include/InputReader.h"
grep -n -F "devicePair.second->getDescriptor() == identifier.descriptor" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "device = deviceIt->second;" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "device->updateExternalStylusState(state);" "frameworks/native/services/inputflinger/reader/InputReader.cpp"
grep -n -F "for_each_mapper([state](InputMapper& mapper) { mapper.updateExternalStylusState(state); });" "frameworks/native/services/inputflinger/reader/InputDevice.cpp"
grep -n -F "mExternalStylusId = state.rawPointerData.touchingIdBits.firstMarkedBit();" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
```

源码没有目标屏选择步骤；所有候选 mapper 都收广播，各自在新触摸处取最低 touching id。重复来自 `mDevices` 的 EventHub-id entry 可共享同一 `InputDevice`。

## 15. 72ms、20ms、10ms：同一个 timeout 字段承载两种等待

`mExternalStylusFusionTimeout` 初始为 `LLONG_MAX`，但它有两种语义。

第一种是“触摸先到，等笔数据”。DIRECT 屏已知存在 external stylus，新 raw state 构成 initial down，而缓存 pressure 为 0 时，`assignExternalStylusId()` 把 deadline 设为 `touchWhen+72ms`，把 raw state 留在 `mRawStatesPending`。pressure 非零的笔 state 只要在 reader 执行 timeout callback 之前被处理，就会立即重进 `processRawTouches()`、选 id、清 deadline、正常 dispatch；代码不拿 stylus sample 的 when 与 72ms deadline 做硬截止比较。`InputReader` 同一轮又是先处理 raw events、后检查 timeout，所以一个时间戳已略晚于 deadline 的 sample 仍可能抢先完成融合。若到达的是 pressure=0，仍等原 deadline，不会延长。timeout 路径会 `resetExternalStylus()`，再把这帧当普通触摸发出。

第二种是“笔数据先更新，等触摸坐标”。已有 active stylus id，或者正在等前述初始 DOWN 时，`updateExternalStylusState()` 都会把 `mExternalStylusDataPending=true` 并处理队列。只有 active stream 没有新 touch frame 消化该 state、且 timeout 当前为 `LLONG_MAX`，才把 deadline 设为 `stylusWhen+20ms`。初始 DOWN 等待期已经持有 72ms deadline，不会切换成 20ms：非零 pressure 可直接释放 pending touch，零 pressure 则继续等原 deadline。

20ms 到期后复制 `mLastRawState`，以 `deadline-10ms` 作为事件时间再 cook/dispatch，等价于给第一份 stylus sample 人工加 10ms，复用最后坐标产生 pressure、tool type 或 button 更新。对 touching id 集未变的 DIRECT 屏，这会形成一次 Motion MOVE，而不会凭空创建 DOWN。

三个关键边界如下：

- deadline 只在当前为 `LLONG_MAX` 时建立；20ms 窗口内后来的多份 stylus state 会覆盖缓存，但不延长第一次 sample 的 deadline，合成时用最新 state、却仍用第一次 deadline 推导时间；
- `timeoutExpired()` 判断 `mExternalStylusFusionTimeout < when`，恰好相等时只再次请求同一 deadline，需下一次更晚回调才真正处理；
- active id 上收到 pressure=0，而上帧该 id 仍 touching 时，`applyExternalStylusTouchState()` 会保留上帧 cooked pressure；零值不是进行中 stroke 的立即归零。

按钮还有一个不同于 pressure 的滞留边界：external buttons 通过按位 OR 写入当前 raw state。20ms 合成又从已经含旧 external button bit 的 `mLastRawState` 复制；若只有 pen button-release、没有新 touch frame，新的零 bit 无法用 OR 清掉旧 bit，通常要下一帧真实触摸状态重建 buttonState 后才体现释放。

还有一个输入形状陷阱：initialDown 用 `pointerCount` 判断“从 0 到非 0”，选 id 却从 `touchingIdBits` 取第一位。若某实现产生纯 hover raw pointer，同时缓存 external pressure 非零，这两个集合的假设会分裂；代码没有在 `firstMarkedBit()` 前再次验证 touching 非空。

### 练习 9：在一条时间线上放置三只钟

情形 A：touch DOWN 在 0ms，pen pressure 在 50ms；情形 B：active stroke 中 pen samples 在 100ms 和 115ms，此间没有 touch frame。推导 dispatch 时刻、使用哪份 state、deadline 是否移动，并判断 timeout callback 恰好到 deadline 时会不会处理。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "EXTERNAL_STYLUS_DATA_TIMEOUT = ms2ns(72);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "TOUCH_DATA_TIMEOUT = ms2ns(20);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "STYLUS_DATA_LATENCY = ms2ns(10);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mExternalStylusFusionTimeout = state.when + EXTERNAL_STYLUS_DATA_TIMEOUT;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "mExternalStylusFusionTimeout = mExternalStylusState.when + TOUCH_DATA_TIMEOUT;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "nsecs_t when = mExternalStylusFusionTimeout - STYLUS_DATA_LATENCY;" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "if (mExternalStylusFusionTimeout < when) {" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
grep -n -F "pressure = coords.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE);" "frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp"
```

A 在约 50ms 的实际处理时刻收到 pen state 后立即释放 pending DOWN，而不是等满 72ms；但发出的 Motion `when` 仍取等待中的 touch 时间 0ms（只会为不早于上一 raw state 而被抬高），不能把交付时刻和事件时间混为一谈。B 的 deadline 固定为 120ms，115ms state 覆盖缓存但不延期；真正超时后以 110ms 为合成事件时间使用 115ms 的最新 state。回调时间恰好 120ms 时严格小于不成立，只会重约，稍晚回调才处理。

## 16. 用状态线排障，并把责任交给第251章

面对“触摸没反应、虚拟键误触、蓝牙笔延迟或 source 不对”，建议按状态线取证，而不是从最终 Motion 倒猜：

| 现象 | 第一检查点 | 第二检查点 | 容易误判之处 |
|---|---|---|---|
| 设备完全无 Motion | raw X/Y valid、device enable | mode、viewport 是否 DISABLED | source 非零不代表 mapper 可运行 |
| 旋转/尺寸异常 | viewport natural/physical/logical | affine 与 calibration 分开看 | coverage 不跟随 point affine |
| virtual key 不发 | 整份 map 是否解析成功、scanCode 是否映射 | surface 外命中、全局 quiet deadline | ignored stroke 仍会被消费 |
| 笔 DOWN 慢约 72ms | presence 与缓存 pressure | pending raw state、timeout | 这是触摸等笔，不是固定事件延迟 |
| 笔压力刷新约在首份笔状态后 20ms 调度 | active stylus id、dataPending | 是否缺同期间 touch frame | 严格 `<` 与调度抖动可让实际交付晚于 20ms；10ms 只是合成 eventTime 偏移 |
| source 已变化而目标 generation 未变 | `InputDevice.mSources` | mapper/device/reader generation | topology 通知可兜底，但四者不原子联动 |

源码证据入口可固定为：

- mode、surface、calibration、virtual key、fusion：`frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp` 与 `TouchInputMapper.h`；
- MT 能力边界：`MultiTouchInputMapper.cpp`，单点 tilt 对照：`SingleTouchInputMapper.cpp`；
- external stylus 状态生产：`ExternalStylusInputMapper.cpp`；
- 全局 presence/state/quiet time：`InputReader.cpp`；
- 组合设备聚合与 reset：`InputDevice.cpp`；
- class 与 virtual-key sysfs 加载：`EventHub.cpp`、`frameworks/native/libs/input/VirtualKeyMap.cpp`。

最终应能给出一句精确结论：`TouchInputMapper` 不是单纯的坐标缩放器；它把静态能力、可变显示配置、非统一校准、屏外按键旁路和无坐标外接笔广播收敛到同一条触摸输出链，而这些子链的完成点并不相同。

下一章转入 `Android PackageManagerService启动、Settings账本与系统包扫描链`，继续沿“磁盘事实、持久账本、内存状态与对外 ready 不是同一完成点”的方法，拆解 PMS 启动。
