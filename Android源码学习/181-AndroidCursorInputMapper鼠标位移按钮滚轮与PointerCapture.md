# 181 Android CursorInputMapper：鼠标位移、按钮、滚轮与 Pointer Capture

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`，`frameworks/base` 提交 `1d9b9ab57d`  
> 学习方式：macOS 静态只读，不要求编译、不要求连接设备  
> 前置章节：第 20、173、174、179、180 章

---

## 1. 先抓住真实问题：同一包 REL/BTN 为何会变成三套语义

Linux 鼠标常把一次硬件报告写成相对位移、按钮和同步边界：

```text
EV_REL / REL_X / +10
EV_REL / REL_Y / +20
EV_KEY / BTN_LEFT / 1
EV_SYN / SYN_REPORT / 0
```

Android 11 却可把它解释为普通鼠标、Pointer Capture 相对鼠标或 navigation/trackball。三者不只 `source` 不同，连 X/Y、display、目标窗口和 App 入口也不同。调试时常见的误判正来自把这些层混在一起：

- 普通鼠标在 InputReader 产出显示逻辑坐标，App 的 `getX()/getY()` 通常已被 Dispatcher 变成窗口局部坐标；
- Capture 下 `getX()/getY()` 本身就是相对量，系统光标不由这个 Mapper 移动；
- 一枚侧键可同时形成 Motion button 与 KeyEvent，但两条事件走不同选窗规则；
- `NotifyDeviceReset` 不一定真的调用 Mapper 的 `reset()`，Capture 切换就是关键反例。

本章只追 r48 的 `CursorInputMapper` 主链：

```text
evdev packet
  → accumulator
  → CursorInputMapper::sync()
  → NotifyMotionArgs / NotifyKeyArgs
  → InputDispatcher 选窗与坐标变换
  → ViewRootImpl 的 mouse / trackball / captured-pointer 入口
```

读完应能仅凭一个 raw packet，推导事件条数、顺序、source、坐标、buttonState、displayId 和大致路由；也能解释 Capture 开关附近的断流、孤立 UP、旧按钮状态与回调竞态。

---

## 2. 源码地图与三个不能混用的坐标层

本章核心文件：

```text
frameworks/native/services/inputflinger/reader/mapper/
├── CursorInputMapper.cpp
├── CursorInputMapper.h
├── TouchCursorInputMapperCommon.h
└── accumulator/
    ├── CursorButtonAccumulator.cpp
    └── CursorScrollAccumulator.cpp

frameworks/native/services/inputflinger/reader/
├── InputDevice.cpp
└── InputReader.cpp

frameworks/native/libs/input/VelocityControl.cpp
frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp

frameworks/base/core/java/android/view/
├── View.java
├── ViewGroup.java
├── ViewRootImpl.java
└── IWindow.aidl

frameworks/base/services/core/java/com/android/server/
├── input/InputManagerService.java
└── wm/InputManagerCallback.java
```

先固定三层词义：

| 层 | 普通鼠标 X/Y | Capture / trackball X/Y |
|---|---|---|
| evdev | 没有绝对位置，只有 `REL_X/Y` | 同左 |
| InputReader 输出 | PointerController 移动后的显示逻辑坐标 | 本包经缩放、旋转和速度控制后的 delta |
| App `MotionEvent.getX/Y()` | 通常经窗口 transform，成为窗口局部坐标 | 仍作为相对导航量读取 |

因此“普通鼠标向 App 报屏幕绝对坐标”并不严谨。绝对显示坐标是 Mapper/Dispatcher 入口处的事实，最终 App 坐标还要经过目标窗口变换。`AXIS_RELATIVE_X/Y` 则不应被套用普通 X/Y 的窗口平移。

责任边界也要分开：

- Mapper 解释设备、维护本地状态并生成通知；
- PointerController 保存并裁剪系统光标位置；
- Dispatcher 依据 source class、焦点、触摸区域和已有按压状态选窗；
- ViewRoot/View 决定应用回调及是否消费；
- 本章不把“已生成通知”写成“目标 App 一定收到并处理”。

---

## 3. Mapper 从何而来，以及三种 mode 的契约

EventHub 只有在节点同时具备 `BTN_MOUSE` 基准、`REL_X` 和 `REL_Y` 能力时，才赋予 `INPUT_DEVICE_CLASS_CURSOR`。`InputDevice::addEventHubDevice()` 再据此创建 `CursorInputMapper`。复合设备还可同时拥有 KeyboardInputMapper；InputDevice 会让相关 Mapper 按 raw event 顺序交错处理，不能假设“一个节点只对应一个 Mapper”。

三种运行模式可先记成一张表：

| mode | native source | X/Y | PointerController | event display | Dispatcher 主路由 |
|---|---|---|---|---|---|
| `MODE_POINTER` | `AINPUT_SOURCE_MOUSE` | 光标移动后的绝对显示坐标 | 获取、移动、同步按钮 | controller 的 displayId | pointer class，初始按位置命中 |
| `MODE_POINTER_RELATIVE` | `AINPUT_SOURCE_MOUSE_RELATIVE` | 本包 delta | 保留但本 Mapper 不移动 | `ADISPLAY_ID_NONE` | navigation class，按焦点 |
| `MODE_NAVIGATION` | `AINPUT_SOURCE_TRACKBALL` | `delta / 6` 后的相对量 | 不获取 | `ADISPLAY_ID_NONE` | navigation class，按焦点 |

`cursor.mode` 的 IDC 值只有：

```text
pointer | default | navigation
```

默认和 `pointer` 都进入普通鼠标；`navigation` 进入 trackball。IDC 不能把设备直接配置成 relative，relative 只由全局 Pointer Capture 从已建立的 pointer 模式动态切入。首次配置若意外看到 `MODE_POINTER_RELATIVE`，代码会记录错误并退回 `MODE_POINTER`，以保证先取得 PointerController。

Java 的 `SOURCE_MOUSE_RELATIVE` 带的是 `SOURCE_CLASS_TRACKBALL`/navigation class，而不是 pointer class。这不是命名细节：相对 X/Y 不能作为屏幕命中坐标，所以必须按焦点分发。

---

## 4. 首次配置：source、显示关联和速度参数各自何时确定

`configureParameters()` 先读取：

```text
cursor.mode
cursor.orientationAware
```

然后计算：

```cpp
hasAssociatedDisplay = mode == MODE_POINTER || orientationAware;
```

这意味着普通 pointer 总声明显示关联；默认 navigation 不关联；orientation-aware navigation 虽声明有关联语义，`getAssociatedDisplayId()` 却返回 `ADISPLAY_ID_NONE`，表示可向任意显示的焦点派发，而不是绑定一个具体屏幕。

首次配置再建立各模式常量：

```text
POINTER:
  source = MOUSE
  x/y scale = 1
  x/y precision = 1
  obtain shared PointerController

NAVIGATION:
  source = TRACKBALL
  x/y scale = 1/6
  x/y precision = 6
  no PointerController
```

若系统此时已经开启 Capture，后面的 Capture 分支会把新建 pointer Mapper 立刻切为 relative。该首次分支会 `bumpGeneration()`，但 Mapper 只在增量变化 `changes != 0` 时显式发送 `NotifyDeviceReset`；新设备本身另有 add/config/reset 生命周期，不能把两条路径合成一句“首次 Capture 必发 reset”。

另外两类变化独立处理：

- `CHANGE_POINTER_SPEED` 会给 pointer、横轮、纵轮三个 VelocityControl 调 `setParameters()`；该调用也清各自历史；
- `CHANGE_DISPLAY_INFO` 会从 **internal viewport** 读取方向，随后无条件 bump Mapper generation。

系统 pointer speed 的基础比例由 policy 计算为：

```cpp
scale = exp2f(pointerSpeed * 0.25f);
```

滚轮拿的是自己的参数；只是一次 pointer-speed 配置刷新会重新设置三只控制器，所以滚轮历史也会被清掉，并不表示滚轮使用 pointer 的比例公式。

---

## 5. accumulator：SYN_REPORT 才提交，但“累积”不等于求和

每笔 RawEvent 先依次交给三只 accumulator：

```cpp
mCursorButtonAccumulator.process(rawEvent);
mCursorMotionAccumulator.process(rawEvent);
mCursorScrollAccumulator.process(rawEvent);
```

只有遇到：

```text
EV_SYN / SYN_REPORT
```

才调用 `sync(when)`。提交后 motion 和 scroll 的瞬时轴清零，按钮状态跨包保留。

r48 的相对轴是覆盖赋值：

```cpp
case REL_X:     mRelX = rawEvent->value; break;
case REL_WHEEL: mRelWheel = rawEvent->value; break;
```

所以同一包：

```text
REL_X +3
REL_X +4
SYN_REPORT
```

只留下 `+4`，不是 `+7`。按钮也只保存最终布尔值：若前态未按，同包 `BTN_LEFT 1 → 0 → SYN_REPORT`，最终状态仍为 0，于是没有 DOWN、UP、WAKE 或 downTime 变化。

按钮的跨包语义与相对轴不同。`CursorButtonAccumulator::reset()` 不把所有按钮机械清零，而是用 `isKeyPressed()` 重新查询 kernel 的 LEFT、RIGHT、MIDDLE、BACK、SIDE、FORWARD、EXTRA、TASK 状态。这个快照只说明 reset 时硬件当前状态；何时能重新发成 Framework 事件，还取决于后续是否有一个真正交给 Mapper 的 `SYN_REPORT`。

### SYN_DROPPED 的精确恢复边界

InputDevice 收到 `SYN_DROPPED` 时会当场 `reset(when)` 并进入丢弃态。之后的 raw event 一直被丢到第一枚 `SYN_REPORT`；这枚“解除丢弃”的同步事件本身也不会传给 Mapper。

```text
SYN_DROPPED
  → InputDevice::reset()，button accumulator 查询 kernel
  → 丢弃中间事件
  → 第一枚 SYN_REPORT：只清 drop gate，仍被吞掉
  → 再有一个交给 Mapper 的 packet + SYN_REPORT：才可能提交按钮快照
```

所以旧稿常说的“下一枚 SYN_REPORT 重建 DOWN”少了一层。这个 InputDevice reset 也不 bump generation；“状态重新建账”不等于“设备进入新 generation”。

---

## 6. `sync()` 先冻结状态快照，再决定是否输出

`sync(when)` 开头先计算：

```text
lastButtonState
currentButtonState
wasDown / down / downChanged
buttonsPressed / buttonsReleased
deltaX / deltaY / moved
vscroll / hscroll / scrolled
```

其中 `isPointerDown()` 只看：

```text
PRIMARY | SECONDARY | TERTIARY
```

BACK/FORWARD 不让 pointer 进入 down，pressure 仍为 0。右键或中键单独按下却足以产生主 `ACTION_DOWN`，并把 pressure 设为 1。

一次同步只有在以下任一条件成立时才进入 Motion 输出块：

```cpp
downChanged || moved || scrolled || buttonsChanged
```

因此纯 `SYN_REPORT` 不产生 Motion，也不会仅为刷新光标而 unfade。注意 `moved` 在旋转和 VelocityControl 之前，由基础 scale 后的 delta 判断；`scrolled` 也在 wheel VelocityControl 之前判断。

主 action 的选择是：

```text
无主按钮 → 有主按钮      DOWN
有主按钮 → 无主按钮      UP
状态不跨边界且当前 down   MOVE
状态不跨边界且非 MOUSE    MOVE
其余普通 MOUSE            HOVER_MOVE
```

所以纯滚轮也不是“只发 SCROLL”：它先触发一笔普通鼠标 `HOVER_MOVE`（按住主按钮则为 `MOVE`），relative/navigation 则先发 `MOVE`，之后才有 `ACTION_SCROLL`。

---

## 7. 位移变换链：基础 scale、方向、速度控制不能调换

X/Y 的实际顺序是：

```text
本包最后一个 REL_X/Y
  → mode 基础 scale（pointer=1；navigation=1/6）
  → 若 orientationAware，按 internal viewport 旋转
  → pointer VelocityControl
  → 移动 PointerController，或直接写入 X/Y
```

`rotateDelta()` 的映射为：

| display orientation | `(x, y)` 变成 |
|---|---|
| 0° | `(x, y)` |
| 90° | `(y, -x)` |
| 180° | `(-x, -y)` |
| 270° | `(-y, x)` |

方向来自 internal viewport，不一定等于共享 PointerController 当前所在 display 的方向；多显示场景不能凭 event displayId 反推这里使用的 viewport。

navigation 的常量是：

```cpp
TRACKBALL_MOVEMENT_THRESHOLD = 6;
mXScale = mYScale = 1.0f / 6;
mXPrecision = mYPrecision = 6;
```

raw `REL_X=3` 因而先成为 `0.5`。它不是 Mapper 等累计满 6 才离散成方向键；若之后出现 DPAD 合成，那是 ViewRoot `SyntheticTrackballHandler` 的另一段逻辑。

VelocityControl 维护的是“控制器输入位置”及 VelocityTracker。navigation 在进入它以前已经除以 6，不能称为累计 raw position。停止至少 500 ms 后的下一次非零移动会先清速度历史，再按基础 scale、阈值和 acceleration 计算输出，避免把很久以前的速度接续到新一笔。

pointer 是一只二维控制器，横滚和纵滚各有一只一维控制器，三者速度历史互不串扰。

---

## 8. 普通鼠标：共享 PointerController、显示坐标与粘住的按压流

普通 `SOURCE_MOUSE` 分支在 moved、scrolled 或 buttonsChanged 时依次：

```text
setPresentation(POINTER)
若 moved：move(deltaX, deltaY)
若 buttonsChanged：setButtonState(currentButtonState)
unfade(IMMEDIATE)
```

随后读取 controller 的位置和显示：

```text
AXIS_X / AXIS_Y                 = 裁剪后的显示逻辑位置
AXIS_RELATIVE_X / RELATIVE_Y   = 本包变换后的 delta
xCursorPosition / yCursorPosition = 同一显示位置
displayId                      = controller displayId
```

在屏幕边界继续移动时，X/Y 可因裁剪保持不变，RELATIVE_X/Y 仍保留移动意图。反过来，`populateDeviceInfo()` 会声明普通鼠标的 X/Y range，却没有为它声明 RELATIVE_X/Y range；运行时存在轴值不等于 `InputDeviceInfo` 一定为该轴给出 range。

InputReader 只保存一份 PointerController 弱引用；需要 controller 的多个 cursor/touch Mapper 可持有同一个强引用。两只鼠标因此移动同一系统光标。`getPointerControllerLocked(deviceId)` 虽把 deviceId 传给 policy，但 r48 的 `NativeInputManager::obtainPointerController(int32_t /* deviceId */)` 明确忽略该参数，不能把 controller 归属于最先创建它的设备。

显示选择来自 `defaultPointerDisplayId`：找不到指定 viewport 时回退默认显示，默认也不存在才记录错误并跳过更新。

Dispatcher 对 pointer-class 的 hover、scroll 和新 DOWN 按坐标命中窗口；DOWN 一旦建立，后续按压序列粘在既有目标，不是每一笔 MOVE 都重新 hit-test。再经目标窗口 transform 后，App `getX()/getY()` 通常成为窗口局部坐标。

---

## 9. relative 与 navigation：X/Y 就是 delta，按焦点进入 ViewRoot

非普通鼠标分支直接写：

```cpp
pointerCoords.setAxisValue(AXIS_X, deltaX);
pointerCoords.setAxisValue(AXIS_Y, deltaY);
displayId = ADISPLAY_ID_NONE;
```

同时保留：

```text
xCursorPosition = INVALID
yCursorPosition = INVALID
```

r48 不在这里设置 `AXIS_RELATIVE_X/Y`。因此 Capture API 应从 `MotionEvent.getX()/getY()` 读取 delta，不能照普通 mouse 只查 `AXIS_RELATIVE_X/Y`。

`ADISPLAY_ID_NONE` 进入 Dispatcher 后由当前 focused display 的窗口焦点承接；它只在选目标时用 focused display，交给 App 的 MotionEvent `displayId` 仍是 `NONE`。navigation-class 事件不拿 delta 去碰 touchable region，也不对 relative X/Y 套普通 pointer 的窗口 offset/scale。普通 mouse 的侧键 Motion 仍属于 pointer 路由，两者不能混写成“所有鼠标都发给焦点窗口”。

ViewRoot 的 trackball 阶段对 relative mouse 有一层门：

```java
if (event.isFromSource(SOURCE_MOUSE_RELATIVE)) {
    if (!hasPointerCapture() || mView.dispatchCapturedPointerEvent(event)) {
        return FINISH_HANDLED;
    }
}
if (mView.dispatchTrackballEvent(event)) {
    return FINISH_HANDLED;
}
return FORWARD;
```

结果分三种：

- Reader 已切 relative，但客户端 `hasPointerCapture()` 尚未变 true：事件静默结束；
- 客户端已 capture，`dispatchCapturedPointerEvent()` 返回 true：正常消费；
- 客户端已 capture但回调返回 false：继续尝试 `dispatchTrackballEvent()`，之后还可能进入 `SyntheticTrackballHandler`。

把第一条理解为避免相对量误落到普通 trackball 的保护效果是合理推论，但源码没有跨进程握手来保证“客户端先确认，Reader 后切 mode”。

---

## 10. 按钮状态：别名、BTN_TASK 缺口与共享 downTime

Linux 按钮到 Android Motion button 的映射为：

| Linux code | Android bit |
|---|---|
| `BTN_LEFT` | `BUTTON_PRIMARY` |
| `BTN_RIGHT` | `BUTTON_SECONDARY` |
| `BTN_MIDDLE` | `BUTTON_TERTIARY` |
| `BTN_BACK` 或 `BTN_SIDE` | `BUTTON_BACK` |
| `BTN_FORWARD` 或 `BTN_EXTRA` | `BUTTON_FORWARD` |

BACK/SIDE 是 OR 到同一个 bit，FORWARD/EXTRA 也是。两个别名同时按住时，第二次按下不会再改变 Framework state；释放其中一个也不会释放该 bit，直到另一枚也释放。

`BTN_TASK` 是一个明显的 r48 边界：accumulator 会构造、reset、process 它，却没有在 `getButtonState()` 中映射到任何 Android bit。因此只变 `BTN_TASK` 不会让本 Mapper 输出 Motion/Key，也不会凭它触发外接设备 WAKE。不过 `getScanCodeState()` 接受 `[BTN_MOUSE, BTN_JOYSTICK)`，直接查询 scan state 时仍可能看见 BTN_TASK。

`mDownTime` 属于 PRIMARY/SECONDARY/TERTIARY 这一整组：

- 从“全未按”到“至少一枚主按钮按下”时写为当前 `when`；
- 组内再按或释放一枚不会改；
- 最后一枚释放时不会清零；
- reset 才清为 0，下一轮 DOWN 会覆盖。

因此 UP、UP 后额外 HOVER，以及随后纯 hover/scroll 都可能继续携带上一轮按压的旧 downTime。这不表示它们仍在 down；应同时看 action、pressure 和 buttonState。

---

## 11. 一个 SYN_REPORT 可输出很多事件，且按钮按较大 mask 先处理

`sync()` 的固定输出顺序是：

```text
1. 新按下 BACK/FORWARD 对应的 Key DOWN
2. 每个 ACTION_BUTTON_RELEASE
3. 一笔主 DOWN / UP / MOVE / HOVER_MOVE
4. 每个 ACTION_BUTTON_PRESS
5. 普通 mouse 主 UP 后额外一笔 HOVER_MOVE
6. 若滚轮非零，再发 ACTION_SCROLL
7. 新释放 BACK/FORWARD 对应的 Key UP
8. 清本包 motion / scroll accumulator
```

release 在主事件前，press 在主事件后。主事件总携带最终 `currentButtonState`；BUTTON_RELEASE/PRESS 则用局部 state 一步步减少或增加。

多按钮循环是：

```cpp
BitSet32 bits(changedButtons);
actionButton = BitSet32::valueForBit(bits.clearFirstMarkedBit());
```

`BitSet32` 的 bit index 0 对应 `0x80000000`，`clearFirstMarkedBit()` 从最高有效数值位开始；Android button mask 又位于低位。所以实际顺序是**较大的 Android button mask 先**，不是“最低数值 bit 先”。同时按 SECONDARY=`0x2` 与 TERTIARY=`0x4` 时：

```text
主 DOWN：buttonState = SECONDARY | TERTIARY
BUTTON_PRESS：actionButton = TERTIARY，state = TERTIARY
BUTTON_PRESS：actionButton = SECONDARY，state = TERTIARY | SECONDARY
```

同时“释放旧键、按下新键”的一包更能看清两种口径：release 事件从旧 state 逐步清；主 MOVE/UP/DOWN 已携最终 state；press 事件再从清完后的中间 state 逐步加。

最后一枚普通主按钮释放的典型序列是：

```text
BUTTON_RELEASE → ACTION_UP → ACTION_HOVER_MOVE
```

若仍有另一枚主按钮按着，则是 `BUTTON_RELEASE → ACTION_MOVE`，没有 UP 后 hover。

---

## 12. 侧键、滚轮、meta、WAKE 和查询接口的交叉边界

BACK/FORWARD 先作为 Motion button 参加前述状态机，公共 helper 又分别合成 `KEYCODE_BACK` / `KEYCODE_FORWARD`：

```text
Key DOWN → Motion 主/按键事件 …… Motion 释放事件 → Key UP
```

两类事件不保证到同一窗口：KeyEvent 走 focused-key target，普通 mouse Motion 走光标位置/已有流的目标；policy 还可能消费 Key，只留下 Motion。应用若恰好收到两路，也要避免把它们再次互相合成造成双触发。

侧键 helper 构造的 Key 有几个特殊点：

```text
scanCode = 0
flags = 0
eventTime = when
downTime = when（DOWN、UP 各自都用本次 when）
source / displayId / policyFlags 沿用 cursor 包
```

UP 的 downTime 因而不是原 DOWN 时间。它也没有 `DISABLE_KEY_REPEAT`：如果 Key DOWN 被 policy 放行并一直没有对应 UP，Dispatcher 的普通重复机制可能生成 repeat/long-press；这和 Motion button 事件是两套状态账。

能力查询也并不对称：Cursor mapper 实现 scan-code state，却没有像 KeyboardInputMapper 那样实现 key-code state 或 supported-key 标记。运行时能合成 BACK/FORWARD，不代表“支持哪些 keyCode”的查询一定报告它们。

滚轮只读取 `REL_WHEEL` / `REL_HWHEEL`，r48 不处理 high-resolution wheel code。`mVWheelScale`、`mHWheelScale` 被设为 1 并出现在 dump，却没有在 `sync()` 数据路径中消费；真实变换来自两只 wheel VelocityControl。

`populateDeviceInfo()` 为 VSCROLL/HSCROLL 以及 relative/navigation X/Y 声明 nominal `[-1, 1]`，但 `sync()` 没有 clamp。它只能证明声明范围，不能单独证明运行值一定落在范围内，更不能由此推导全部 API 语义。

每次 Motion 输出从 InputReaderContext 读取全局 meta，所以按住外接键盘 Ctrl 再滚轮，SCROLL 可带 Ctrl。外接 cursor 只有在：

```text
buttonsPressed || moved || scrolled
```

时添加 `POLICY_FLAG_WAKE`。纯 release 不自动 WAKE；内部 cursor 不走这条自动唤醒；BTN_TASK 因不形成 buttonState 变化也不贡献这里的 `buttonsPressed`。

---

## 13. Pointer Capture 控制面：焦点授权、回调与 native 刷新不是一次原子提交

请求链是：

```text
View.requestPointerCapture()
  → ViewRootImpl.requestPointerCapture(true)
  → InputManagerService.requestPointerCapture(token, true)
  → WMS InputManagerCallback 校验当前 focused IWindow
  → IWindow.dispatchPointerCaptureChanged(true)
  → NativeInputManager::setPointerCapture(true)
  → InputReader 请求 CHANGE_POINTER_CAPTURE
```

ViewRoot 只用本地 `mPointerCapture` 做“是否无需请求”的快速判断；真正授权在 WMS callback：请求 token 必须与**全局 focused display 上的 focused window** Binder 相同。这里校验的是窗口 token，不是发起请求的子 View 是否持有 View focus；同一 ViewRoot 中任一已附着 View 都可发请求，Capture 状态属于整个窗口。窗口焦点或全局 focused display 切换会先向旧窗口派发 `false`，并请求 native 关闭 Capture；子 View 间焦点移动本身不会释放。

WMS 的 `dispatchPointerCaptureChanged()` 先更新 `mFocusedWindowHasCapture`，再调用 oneway `IWindow` 回调；`RemoteException` 被忽略，方法仍返回需要刷新配置。于是即使客户端回调发送失败，WMS 仍可认为已 capture，IMS 仍会调用 native。

客户端 Binder stub 收到回调后还要向 ViewRoot 主 Handler 投递 `MSG_POINTER_CAPTURE_CHANGED`，才更新 `mPointerCapture` 并通知 View。`ViewGroup` 会把状态变化广播给孩子，而 captured event 沿当前 focused child 的输入路径下发，两者范围也不相同。另一边 native 只是更新全局 bool，再异步请求 InputReader 刷新配置。

因此这两条异步支路没有握手：

```text
窗口回调支路：WMS → oneway IWindow → ViewRoot Handler
Reader 支路：   IMS → native bool → requestRefreshConfiguration
```

观察时可能先看到客户端 capture=true，也可能先看到 relative source；请求方法返回更不是“下一笔输入已完成切换”的确认点。

r48 的 native `pointerCapture` 是全局 bool，不按 display、window 或 device 保存。窗口层只允许焦点窗口请求，但 Reader 配置变化会广播给所有 CursorInputMapper。

---

## 14. Pointer Capture 数据面：切 mode 不等于 reset Mapper

进入 Capture 时，当前 pointer Mapper 做：

```text
MODE_POINTER → MODE_POINTER_RELATIVE
SOURCE_MOUSE → SOURCE_MOUSE_RELATIVE
fade(IMMEDIATE)
保留 PointerController 引用和其中的位置
```

Capture 期间此 CursorInputMapper 不调用 controller `move()`。退出时只把 mode/source 切回普通 mouse，**没有**在配置分支中 `unfade()`；要等之后 moved、scrolled 或 buttonsChanged，普通 mouse 数据路径才 immediate unfade。TouchInputMapper 不读取这个全局 Capture 开关，仍可能移动或显示共享 controller，所以“Capture 冻结所有指针设备”也不成立。

“退出后回到原位置”只能作条件性描述：本 Mapper 在 relative 期间不改共享 controller，因而通常从保留位置继续；但其他共享它的鼠标/触控板路径或显示配置仍可能改变 controller。

动态 Capture 分支无论成功切换还是因当前 mode 不合适而只记错误，都会继续：

```text
bumpGeneration()
NotifyDeviceReset（changes != 0）
```

对 `MODE_NAVIGATION` 设备也是如此：source/mode 没变，仍会 bump 并发 reset 通知。

最容易误读的是 `NotifyDeviceReset`。这里直接构造通知交给 listener，**没有调用** `CursorInputMapper::reset()`，所以以下状态都会保留：

- `mButtonState` 与 `mDownTime`；
- 三只 VelocityControl 的历史；
- accumulator 中尚未被 `finishSync()` 清掉的位移/滚轮；
- button accumulator 的当前布尔状态。

Dispatcher 会据 reset 通知按逻辑 deviceId 对各 connection 做 `CANCEL_ALL_EVENTS`，复合设备同一 id 下的键盘等其他 source 也会受影响；Reader 自己却不会重新建 DOWN。若主按钮跨切换一直按着，切换后的下一包可能直接成为新 source 的 MOVE；随后释放可出现 BUTTON_RELEASE → UP，却没有同 source 的新 DOWN，并继续使用旧 downTime。第一笔 relative delta 也可能继承 Capture 前的速度历史。这是通知隔离下游状态的效果，不是两端原子重置。

共享 PointerController 还有一个可见边界：relative 分支不会 `setButtonState()`。Capture 期间发生的按/放不会更新 controller；退出后若第一包没有新的 `buttonsChanged`，controller 内部按钮图可能继续是旧值，虽然 Mapper 发出的 Motion 使用自己的当前 `mButtonState`。

真正的 `CursorInputMapper::reset()` 才会清本地 button/downTime、三只速度历史、相对与滚轮瞬时值，并让 button accumulator 查询 kernel。它与 Capture 配置分支的同名“device reset 通知”必须分开阅读。

---

## 15. 九个 macOS 只读练习：把结论变成可复查证据

所有命令都从 AOSP 工作树 `/Users/ninebot/androidSource` 执行，不修改源码。

### 练习一：定位创建条件与 Mapper 装配

```bash
rg -n "INPUT_DEVICE_CLASS_CURSOR|BTN_MOUSE|REL_X|REL_Y" \
  frameworks/native/services/inputflinger/reader/EventHub.cpp \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

回答：capability 判类和 Mapper 构造分别在哪一层？

### 练习二：对照三种 mode

```bash
sed -n '112,238p' \
  frameworks/native/services/inputflinger/reader/mapper/CursorInputMapper.cpp
```

分别圈出 source、scale、PointerController、Capture 切换和 generation。

### 练习三：验证覆盖而非求和

```bash
sed -n '20,62p' \
  frameworks/native/services/inputflinger/reader/mapper/CursorInputMapper.cpp
sed -n '20,75p' \
  frameworks/native/services/inputflinger/reader/mapper/accumulator/CursorScrollAccumulator.cpp
```

找出 `=`，手算同包 `REL_X 3 → REL_X 4`。

### 练习四：逐行标出 `sync()` 输出顺序

```bash
sed -n '265,450p' \
  frameworks/native/services/inputflinger/reader/mapper/CursorInputMapper.cpp
```

写出 Key DOWN、release、主 Motion、press、hover、scroll、Key UP 的次序。

### 练习五：证明多按钮从较大 mask 开始

```bash
sed -n '35,105p' system/core/libutils/include/utils/BitSet.h
sed -n '3405,3465p' \
  frameworks/native/services/inputflinger/tests/InputReader_test.cpp
```

用 SECONDARY=`0x2`、TERTIARY=`0x4` 验证哪一个先发。

### 练习六：找出 BTN_TASK 与查询接口的不对称

```bash
sed -n '20,115p' \
  frameworks/native/services/inputflinger/reader/mapper/accumulator/CursorButtonAccumulator.cpp
sed -n '445,470p' \
  frameworks/native/services/inputflinger/reader/mapper/CursorInputMapper.cpp
```

比较 `process()`、`getButtonState()` 与 `getScanCodeState()`。

### 练习七：核对 SYN_DROPPED 的两枚同步边界

```bash
sed -n '320,370p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

确认解除 drop gate 的那枚 SYN_REPORT 是否传给 Mapper。

### 练习八：追完 Capture 的两条异步支路

```bash
rg -n "requestPointerCapture|dispatchPointerCaptureChanged|nativeSetPointerCapture|CHANGE_POINTER_CAPTURE" \
  frameworks/base/core/java/android/view \
  frameworks/base/services/core/java/com/android/server/input \
  frameworks/base/services/core/java/com/android/server/wm \
  frameworks/base/services/core/jni \
  frameworks/native/services/inputflinger
```

分别标出授权点、客户端状态更新点、native bool 和 Reader 配置点。

### 练习九：验证 ViewRoot 的 relative fallback

```bash
sed -n '6048,6070p' frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n "SOURCE_MOUSE_RELATIVE|SyntheticTrackballHandler" \
  frameworks/base/core/java/android/view/ViewRootImpl.java
```

回答 capture=false、captured callback=true、callback=false 三种结果。

---

## 16. 三个手算包、复读清单与下一章

### 包一：普通 mouse 在边界继续右移

假设光标已在显示最右边，忽略 acceleration，本包 `REL_X=+10`：

```text
主 action         = HOVER_MOVE（无主按钮）
Mapper AXIS_X     = 仍是边界位置
AXIS_RELATIVE_X   = +10
cursorPosition    = 边界位置
App getX          = 再经目标窗口 transform，通常是局部坐标
```

这同时证明“绝对位置未变”不等于“设备没有移动意图”。

### 包二：同时按右键与中键

前态无按钮，本包最终为 SECONDARY|TERTIARY：

```text
ACTION_DOWN(final state = SECONDARY | TERTIARY)
ACTION_BUTTON_PRESS(actionButton = TERTIARY, state = TERTIARY)
ACTION_BUTTON_PRESS(actionButton = SECONDARY, state = 两者)
```

若下一包释放两者，则较大 mask 的 TERTIARY release 也先出现，最后才是主 UP 和额外 HOVER。

### 包三：按钮按住时切入 Capture

```text
普通 MOUSE 已有 DOWN，mDownTime=t0
CHANGE_POINTER_CAPTURE：切 source、bump、通知 Dispatcher reset；Mapper 状态不清
下一包 relative delta：ACTION_MOVE，buttonState 仍按下，downTime=t0
随后释放：BUTTON_RELEASE → ACTION_UP，仍可能带 t0
```

新 source 没有由 Mapper 补一笔 DOWN；这正是“通知下游 reset”与“执行 Mapper reset”不同的可观察后果。

### 十四条复读清单

1. accumulator 等 SYN_REPORT，但 r48 的同轴值与按钮布尔值只保留包内最后状态。
2. SYN_DROPPED 后用于解除 drop 的首枚 SYN_REPORT 被吞，最早要后续同步才提交 kernel 按钮快照。
3. pointer 的 Mapper X/Y 是显示逻辑坐标；App X/Y 通常已变成窗口局部坐标。
4. relative/navigation 的 X/Y 是 delta，cursorPosition 无效，r48 不填 RELATIVE_X/Y。
5. navigation 在进入 VelocityControl 前已除以 6。
6. orientation 使用 internal viewport，不必等于 pointer 当前 display。
7. PointerController 被多个 Mapper 共享，native policy 还忽略传入的 deviceId。
8. pointer 的初始目标按位置命中；已建立的 DOWN 流不会逐笔重新命中。
9. PRIMARY/SECONDARY/TERTIARY 任一都建立 down；BACK/FORWARD 不建立。
10. 多 button 的 release/press 按较大 Android mask 先处理。
11. 侧键 Motion 与合成 Key 路由不同，接收者和消费结果可能不同。
12. 纯滚轮先有主 HOVER/MOVE，再有 SCROLL；nominal range 不是 clamp 证据。
13. Capture 的窗口回调与 Reader 切 mode 是无握手的异步支路。
14. Capture 的 `NotifyDeviceReset` 不调用 Mapper reset，退出分支也不会主动 unfade。

### 一句话模型

```text
CursorInputMapper 以真正交给它的 SYN_REPORT 为提交边界，把包内最后一组相对轴和持久按钮快照依次做 mode scale、方向与速度变换；普通 mouse 借共享 PointerController 形成显示坐标并按位置建立目标，relative/trackball 则把 X/Y 当 delta 按焦点路由；一个包再严格按侧键 Key DOWN、button release、主 Motion、button press、UP 后 hover、scroll、侧键 Key UP 输出，而 Capture 只异步切 source/mode、bump generation 并通知下游 reset，并不会清 Mapper 自己的状态。
```

### 下一章

第 182 章进入 `TouchInputMapper` 的设备配置与 raw/cooked state：先建立 single-touch、multi-touch、touchpad、direct/indirect 模式与坐标校准框架，再为 slot 跟踪和手势状态机打基础。
