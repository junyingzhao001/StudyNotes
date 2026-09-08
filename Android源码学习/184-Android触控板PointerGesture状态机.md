# 184 Android 触控板 Pointer Gesture：手指数怎样变成鼠标式事件流

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> `frameworks/native` 提交：`57b3d43492`；`frameworks/base` 提交：`1d9b9ab57d`  
> 学习方式：macOS 静态只读，不要求编译、不冒充真机实测  
> 前置章节：第 181—183 章

---

## 1. 先看故障现场：两根手指，为什么 App 只收到一个 pointer

调试一块触控板时，常会遇到看似矛盾的现象：

- 一根手指已经接触硬件，App 却只收到 `HOVER_MOVE`，没有 `DOWN`；
- 两根手指落下后，App 的 `pointerCount` 仍是 1；
- 两指同向移动收到的是普通 `MOVE`，不是 `ACTION_SCROLL`；
- 两指稍后张开，旧单指流突然 `CANCEL`，再重新出现多指 `DOWN / POINTER_DOWN`；
- 多指结束后，最后一枚 `UP` 与紧随其后的 `HOVER_MOVE` 坐标可能不相同；
- 轻点已经物理抬手，`UP` 却过一段时间才到。

一句话结论是：

> 在 `DEVICE_MODE_POINTER` 下，`TouchInputMapper` 不把手指数直接翻译成 MotionEvent 指针数；它先把当前 finger 集合解释为 HOVER、TAP、BUTTON、PRESS、SWIPE、FREEFORM 或 QUIET，再由第二层代码把“手势状态”展开成鼠标式事件流。

这章的唯一主问题是：

```text
一份 Cooked finger 集合，怎样经过状态判断、坐标换算和事件展开，
最终成为 App 所见的 hover、单指 down 流或自由多指流？
```

读完后，应能仅凭 raw/cooked 集合、按钮状态、时间和位移，手算下一状态、`finish/cancel`、事件 action、gesture id 与坐标来源。

本章不讨论现代桌面系统上层怎样把两指 `MOVE` 再解释成滚动、缩放或快捷手势，也不讨论直接触屏的 DOWN/MOVE/UP 展开；后者已经在第 182、183 章建立模型。

---

## 2. 先分清五本账：状态机不是手指数到 action 的查表

### 从事件包到通知的完整主线

核心调用链不是旧稿容易写成的“`sync()` 从 Cooked State 选择 usage”：

```text
SYN_REPORT
  → TouchInputMapper::sync()
      组 RawState、同步触点/按钮/滚轮、分配 Android pointer id
  → processRawTouches()
  → cookAndDispatch()
      构造 CookedState，按 tool 分类 finger / stylus / mouse
      选择 PointerUsage
  → dispatchPointerUsage()
  → dispatchPointerGestures()
      preparePointerGestures()：判 mode、坐标、finish/cancel
      视觉更新
      展开 NotifyMotionArgs
```

因此至少要同时维护五本账：

| 账本 | 代表什么 | 容易误认成什么 |
|---|---|---|
| `fingerIdBits` | 本帧被归为 finger 的 Android pointer id 集合 | slot 或 trackingId |
| `activeTouchId` | 状态机当前选来跟随的一个 Raw/Cooked pointer id | 驱动直接给的“物理 id” |
| `current/lastGestureMode` | 当前与上一帧的手势假设 | App action |
| `current/lastGestureIdBits` | current 是本帧候选输出 id；last 只持久保存上一帧 down 流 id | finger id 集合 |
| `PointerController` | 屏幕主光标、display id、button state 与可选 spots | MotionEvent 坐标本身 |

第 183 章已经把 slot/trackingId 映射为 `RawPointerData::Pointer.id`。本章的 `activeTouchId` 取的正是这个 Android pointer id；FREEFORM 又会在其上建立 `touch id → gesture id` 的第二层映射。

### 九个 mode 不等于九种 action

`TouchInputMapper::PointerGesture::Mode` 有九个值：

| mode | 物理意图假设 | 对外 down 拓扑 |
|---|---|---|
| `NEUTRAL` | 无手指、无有效按键 | 无 |
| `HOVER` | 一指移动鼠标光标 | 无；发 `HOVER_MOVE` |
| `TAP` | 抬手后才确认的一次轻点 | 单 id，pressure=1 |
| `TAP_DRAG` | 第二次落指续接尚未结束的 TAP | 沿用单 id |
| `BUTTON_CLICK_OR_DRAG` | primary/secondary/tertiary 按键点击或拖动 | 单 id |
| `PRESS` | 至少两指但意图尚未判明 | 单个静止合成 id |
| `SWIPE` | 两指已被判成大致同向 | 单个移动合成 id |
| `FREEFORM` | 自由两指或更多指 | 每根 finger 一个 gesture id |
| `QUIET` | 多指或按钮退场后的静默期 | 无 |

`PRESS`、`SWIPE` 都来自多根物理手指，却只输出一个 gesture pointer；`HOVER` 内部有一份当前 gesture 坐标，却不把它保存成 down id。mode、finger 数与 App pointer 数不是一一对应关系。

### 源码地图

```text
frameworks/native/services/inputflinger/
├── include/InputReaderBase.h
├── reader/InputReader.cpp
├── reader/Macros.h
└── reader/mapper/
    ├── MultiTouchInputMapper.cpp
    ├── TouchCursorInputMapperCommon.h
    └── TouchInputMapper.cpp/.h

frameworks/base/
├── core/java/android/view/ViewConfiguration.java
└── services/core/jni/com_android_server_input_InputManagerService.cpp
```

`TouchInputMapper.cpp:1506—1589` 负责 Cooked 分类与 usage 选择；`2317—3298` 覆盖 prepare、dispatch 及九态主逻辑。行号只帮助定位当前提交，不是跨版本 API。

---

## 3. 这条路径何时成立：device mode、source 与 usage 是三层门

### 先成为 POINTER mode

设备通常因 `INPUT_PROP_POINTER` 或配置 `touch.deviceType=pointer` 被识别为 `DEVICE_TYPE_POINTER`。只有全局 `pointerGesturesEnabled` 为真，`configureSurface()` 才选择：

```cpp
mSource = AINPUT_SOURCE_MOUSE;
mDeviceMode = DEVICE_MODE_POINTER;
if (hasStylus()) {
    mSource |= AINPUT_SOURCE_STYLUS;
}
```

所以 finger gesture 的 source 以 MOUSE 为基础，但带笔能力的复合设备可能得到 `MOUSE | STYLUS` 的组合 source；不要把它写成永远“精确等于 MOUSE”。状态机生成的 `PointerProperties.toolType` 仍常是 FINGER。

关闭 pointer gesture 后，这类设备不会继续走同一状态机；r48 的兜底是 `DEVICE_MODE_UNSCALED + AINPUT_SOURCE_TOUCHPAD`。

### 再由当前 tool 集合选择 usage

`cookAndDispatch()` 把 touching raw pointer 分为 stylus、mouse、finger，hovering 集合只把 stylus/eraser 纳入 stylus usage。优先级固定为：

```text
有 stylus / eraser → POINTER_USAGE_STYLUS
否则有 mouse     → POINTER_USAGE_MOUSE
否则有 finger，或有效 pointer button down
                  → POINTER_USAGE_GESTURES
否则             → 保持上一 usage
```

“空集合保持上一 usage”不是泄漏，而是收尾协议的一部分：

- gesture usage 仍需看到 0 指帧，才能确认 TAP 或结束旧流；
- stylus/mouse simple usage 也需看到空帧，才能发 UP/HOVER_EXIT；
- 已请求的 TAP timeout 到达时，只有设备仍是 POINTER 且 usage 仍为 GESTURES，才重新进入 gesture timeout 分支。

usage 改变时，`dispatchPointerUsage()` 先 `abortPointerUsage()`，再切换解释器。旧 GESTURES 若有 down gesture id 会发 CANCEL；旧 STYLUS/MOUSE 走 `abortPointerSimple()`，通常是正常 UP 或 HOVER_EXIT，不能把三条路径统称为 CANCEL。

PALM 更早在 `MultiTouchInputMapper::syncTouch()` 被排除并可能触发 `cancelTouch()`；第 183 章已证明，r48 的 `mCurrentMotionAborted` 只抑制/清除非 POINTER 分支，因此剩余 non-palm finger 仍可能同帧重新进入 pointer gesture。

---

## 4. 决策顺序比状态图更重要：QUIET 甚至压在按钮之前

`preparePointerGestures()` 先处理 timeout；普通输入帧再更新速度、tap 资格、active touch 与 quiet 判断，最后按以下优先级只进入一条主分支：

```text
isTimeout?
  └─ 只处理 lastMode == TAP

普通帧：
  1. isQuietTime                  → QUIET
  2. primary/secondary/tertiary   → BUTTON_CLICK_OR_DRAG
  3. currentFingerCount == 0      → TAP 或 NEUTRAL
  4. currentFingerCount == 1      → HOVER 或 TAP_DRAG
  5. currentFingerCount >= 2      → PRESS / SWIPE / FREEFORM
```

这棵决策树修正了两个常见口号：

1. “物理按钮绝对优先”不成立。QUIET 已成立时，即使按钮在本帧按下，也先留在 QUIET；
2. “三指直接 FREEFORM”不成立。初入至少两指先建立 PRESS；只有至少两指累计移动越过门槛后，手指数才参与 FREEFORM 判定。

主干是 0 指的 TAP/NEUTRAL、一指的 HOVER/TAP_DRAG、有效按钮的 BUTTON，以及 2+ 指的 PRESS→SWIPE/FREEFORM。第三指在 settle 内加入时会先取消并重建 PRESS，同一次调用随后仍可能再分类；settle 后的 SWIPE 加第三指则直接 CANCEL→FREEFORM。

---

## 5. 阈值从哪里来：150ms、10px 只是构造基线

### 构造值与正常运行值要分开

`InputReaderConfiguration` 的 C++ 构造值是：

| 参数 | 构造基线 | 用途 |
|---|---:|---|
| `pointerGestureQuietInterval` | 100ms | 多指/按钮退场后的静默 |
| `pointerGestureDragMinSwitchSpeed` | 50px/s | BUTTON 切换 active finger |
| `pointerGestureTapInterval` | 150ms | 首次落指至抬指的 tap 窗口 |
| `pointerGestureTapDragInterval` | 150ms | TAP 后等待第二次落指 |
| `pointerGestureTapSlop` | 10px | X、Y 分轴的光标容差 |
| `pointerGestureMultitouchSettleInterval` | 100ms | 加指时是否重建多指参考 |
| `pointerGestureMultitouchMinDistance` | 15px | PRESS 分类运动门槛 |
| `pointerGestureSwipeTransitionAngleCosine` | 0.2588 | 约为 cos 75° |
| `pointerGestureSwipeMaxWidthRatio` | 0.25 | swipe 最大两指 raw 间距比例 |
| `pointerGestureMovementSpeedRatio` | 0.8 | 整体平移相对显示对角线比例 |
| `pointerGestureZoomSpeedRatio` | 0.3 | FREEFORM 相对展开比例 |

但正常 system_server 会从 Java policy 回填三项：

```text
tapInterval     = getHoverTapTimeout()
tapDragInterval = max(min(longPressTimeout - 100, doubleTapTimeout),
                      hoverTapTimeout)
tapSlop         = getHoverTapSlop()
```

r48 AOSP 默认的 hover tap、long press、double tap、hover slop 分别是 150ms、400ms、300ms、20px，因此默认运行结果通常为：

```text
tapInterval = 150ms
tapDragInterval = max(min(400 - 100, 300), 150) = 300ms
tapSlop = 20px
```

long-press timeout 还能受系统设置影响，所以“普通 tap 固定 150ms 后发 UP”和“slop 固定 10px”都不成立。应以目标设备 `dumpsys input` 中的有效配置为准。

### movement 与 zoom 是两套空间

surface 几何建立时计算：

```text
rawDiagonal     = hypot(rawWidth, rawHeight)
displayDiagonal = hypot(rawSurfaceWidth, rawSurfaceHeight)

movementScale = 0.8 * displayDiagonal / rawDiagonal
zoomScale     = 0.3 * displayDiagonal / rawDiagonal
maxSwipeWidth = 0.25 * rawDiagonal
```

- `movementScale` 用于 HOVER/BUTTON 的光标移动，以及 SWIPE/FREEFORM 的整体群组平移；
- `zoomScale` 用于 PRESS 运动门槛与 FREEFORM 相对 `referenceTouch` 的展开；
- `maxSwipeWidth` 仍在 raw 触控板空间比较。

movement delta 后面还会经过 `mPointerVelocityControl`，其 scale 受用户 pointer speed 设置影响。所以 0.8 是无加速时的几何基线，不是最终手感的唯一参数。

### 十个边界运算符值得单独记

| 判定 | r48 运算符 | 正好等于阈值时 |
|---|---|---|
| tap 持续时间 | `<=` | 仍可算 TAP |
| tap X/Y slop | `<=` | 仍在方框内 |
| tap-drag 再落指 | `<=` | 仍可续接 |
| timeout 结束 TAP | `when > deadline` | 等于时重约，不结束 |
| quiet 是否仍有效 | `when < deadline` | 等于时已退出 |
| settle 是否完成 | `when >= deadline` | 等于时已 settled |
| PRESS 每指运动门槛 | 外层实际要求 `>` | 等于时仍留在 PRESS |
| swipe 最大宽度 | `mutualDistance > max` 才拒绝 | 等于时继续判角度 |
| swipe 方向 | `cosine >= threshold` | 等于时判 SWIPE |
| BUTTON 换指速度 | `speed > bestSpeed` | 等于不换 |

注释里常写 “less than” 或 “at least”，真正复现边界时要以这些运算符为准。

---

## 6. 一指主线：activeTouchId 怎样把 raw 差值变成 HOVER

### 普通选取先保留 active，需补位时取最低 Android pointer id

普通帧先挑 active touch：

```cpp
if (activeTouchId < 0 && !fingerIdBits.isEmpty()) {
    activeTouchId = fingerIdBits.firstMarkedBit();
    firstTouchTime = when;
} else if (!fingerIdBits.hasBit(activeTouchId)) {
    activeTouchId = fingerIdBits.isEmpty()
            ? -1 : fingerIdBits.firstMarkedBit();
}
```

实现并未真的比较“刚刚 down 的 id”；它直接取当前 bitset 的最低 marked id。在这一步，已有 active 仍在便保留，缺位时取当前最低 id；BUTTON 分支随后仍可能按速度改选，见第 9 节。

还有一个不对称点：active 从 -1 变为有效时会写 `firstTouchTime`；active 消失但仍有其他手指接任时，不重置该时间。settle 内重建 PRESS 也不重置。只有全抬起后下一轮重新选择，或整体 reset/abort，才开启新的 first-touch 计时。

`activeTouchId` 主要供 HOVER、TAP_DRAG、BUTTON 跟随。PRESS/SWIPE 用群组向量；FREEFORM 用 touch→gesture 映射，不能把 active finger 当所有模式的坐标源。

### HOVER 使用相对位移，不投影绝对触控板位置

一指、无按钮、非 TAP_DRAG 时进入 HOVER。若同一 active id 在上一帧也存在：

```cpp
deltaX = (current.x - last.x) * mPointerXMovementScale;
deltaY = (current.y - last.y) * mPointerYMovementScale;
rotateDelta(mSurfaceOrientation, &deltaX, &deltaY);
mPointerVelocityControl.move(when, &deltaX, &deltaY);
mPointerController->move(deltaX, deltaY);
```

关键条件不是“刚接任”，而是新 active id 是否存在于上一 finger 集合：若存在，即使它刚从旧 active 手中接任，也会按自己的 current-last delta 移动；只有不在上一集合的新 id 才重置速度控制且本帧不移动。最终 gesture 坐标取 `PointerController::getPosition()`，pressure=0。

这里要区分三种位置：

1. raw pointer 的 x/y：用于算相邻帧差值；
2. PointerController 的显示逻辑坐标：累计、旋转、加速并被显示边界约束；
3. App 最终 `MotionEvent.getX/Y()`：InputDispatcher 后续还会转成窗口局部坐标。

所以“触控板某个绝对角落等于 App 某个绝对点”不是这条路径的契约。

HOVER 每个普通输入帧都会直接发 `HOVER_MOVE`，即使坐标未变；down 流的 `MOVE` 才受第 14 节的变化比较约束。另有一份 `PointerGesture.velocityTracker` 收集 `raw * movementScale`、不先旋转，只为 BUTTON 挑最快 finger；实际 delta 加速由独立的 `mPointerVelocityControl` 完成。

---

## 7. TAP 是抬手才确认：物理 UP 反而产生 gesture DOWN

### 落指阶段只记候选

非持有 TAP 的普通 0→1 指帧先按 HOVER 处理，随后记录；若上一 mode 正是尚可续接的 TAP，则这里会优先进入下一节的 TAP_DRAG：

```cpp
mPointerGesture.resetTap();
mPointerGesture.tapDownTime = when;
mPointerGesture.tapX = x;
mPointerGesture.tapY = y;
```

`x/y` 是当时 PointerController 的位置，不是 raw contact 坐标。

抬手进入 0 指分支时，只有以下条件全部成立才确认 TAP：

1. 上一 mode 是 HOVER 或 TAP_DRAG；
2. 上一帧恰有一根 finger；
3. `when <= tapDownTime + tapInterval`；
4. 当前 PointerController 相对 `tapX/tapY` 的 X、Y 偏移分别 `<= tapSlop`。

判定区域是显示逻辑坐标里的轴对齐方框，不是 raw 空间的半径圆。若起点靠显示边缘，PointerController 的边界裁剪还可能让大量 raw 运动表现成较小的光标偏移。

### TAP 的坐标和 downTime 都不是物理抬手现场的直觉值

识别成功后，current mode 变 TAP，gesture id0、pressure=1，坐标写的是首次落指时保存的 `tapX/tapY`，不是抬手帧当前 cursor。

dispatch 在首个 gesture id 真正加入空集合时才执行：

```cpp
if (dispatchedGestureIdBits.count() == 1) {
    mPointerGesture.downTime = when;
}
```

所以简单 tap 的 MotionEvent `downTime` 接近“物理抬起并识别 TAP”的时刻，而不是 `tapDownTime`。这正是两套时间账。

普通轻点的第一段事件因此是：

```text
物理 finger DOWN → HOVER_MOVE，记录 tapDownTime/tapX/tapY
小幅移动           → HOVER_MOVE ...
物理 finger UP     → 识别 TAP，向 App 发 ACTION_DOWN
```

### 为什么先发 DOWN，却暂不发 UP

确认 TAP 时还会请求：

```cpp
requestTimeoutAtTime(tapUpTime + pointerGestureTapDragInterval);
```

状态机暂时把 id0 保持为 down，是为了让窗口内的第二次落指无缝续成 TAP_DRAG。没有后续输入时，通常由 timeout 正常结束旧 TAP。

但“UP 一定等 timeout”也过强。截止前如果出现新普通输入帧，状态机会立即重新分类：

- 又来一枚 0 指 `SYN_REPORT`：0 指分支直接 finish TAP；
- 来一指但已超时或超 slop：finish TAP，再进入 HOVER；
- 来有效按钮或至少两指：finish TAP，再开启 BUTTON 或 PRESS。

timeout 是无新输入时的保底完成机制，不是唯一完成路径。

### 为什么截止点必须严格越过

timeout 分支只处理 `lastGestureMode == TAP`：

```cpp
if (when <= tapUpTime + tapDragInterval) {
    requestTimeoutAtTime(tapUpTime + tapDragInterval);
    return false;
}
finishPreviousGesture = true; // when > deadline
```

`InputReader::loopOnce()` 在调用 `timeoutExpiredLocked(now)` 前，已经把全局 `mNextTimeout` 重置成 `LLONG_MAX`；所以精确等于 deadline 时重新请求同一 deadline 能重新成为下一 timeout，不会因“与旧值相等”被永久忽略。下一次时钟严格越界才发 UP。

正常结束后，只要没有当前新 down，dispatch 还会以 PointerController 当前坐标补一枚合成 `HOVER_MOVE`。它与 TAP DOWN 保存的 `tapX/tapY` 允许存在 slop 范围内的小跳变。

---

## 8. 第二次落指：TAP_DRAG 与 double-tap 共享同一条 held-down 流

### TAP_DRAG 不发第二个 DOWN

上一 mode 是 TAP 时，窗口内再次出现恰好一指；若时间与 X/Y slop 仍合格，current mode 变为 TAP_DRAG。上一 TAP 的 gesture id0 仍在 `lastGestureIdBits`，所以新触点不会生成第二枚 DOWN。

第二次落指帧本身不会移动 PointerController，因为上一 finger 集合为空；从后续仍保持该 active id 的帧起，才按相对 delta 移动。随后也只有坐标或属性真的变化才发 MOVE，因此落指帧可能没有任何 MotionEvent，不能把“进入 TAP_DRAG”写成“必然收到 MOVE”。

同时，因为上一帧 finger count 为 0，分支末尾会重新记录第二次触摸的 `tapDownTime/tapX/tapY`。这是第二次抬手还能被重新识别成 TAP 的计时基线。

### 双击的真实 action 时间线

假设两次接触都短且小移：

| 物理时刻 | mode 结果 | App 所见 |
|---|---|---|
| 第一次落指 | HOVER | HOVER_MOVE |
| 第一次抬指 | TAP | 第一个 ACTION_DOWN |
| 第二次落指 | TAP_DRAG | 沿用 id/downTime；坐标不变时可无事件 |
| 第二次抬指 | 再次识别 TAP，且 `finish=true` | 同一 `when` 先第一个 ACTION_UP，再发第二个 ACTION_DOWN |
| 第二个窗口严格超时 | NEUTRAL | 第二个 ACTION_UP，再补 HOVER_MOVE |

第二次抬指帧之所以既 UP 又 DOWN，是因为 0 指分支先把上一非 NEUTRAL 流标为 finish，随后又把 current 构造成新的 TAP id0。dispatch 正常结束旧流后，发现当前 id0 是新的 down，再重设 `downTime` 并发送 DOWN。

### 续接失败会怎样

第二次落指若晚于窗口或光标超出 slop，current 保持 HOVER。因为上一 mode 不是 HOVER，prepare 同时置 `finishPreviousGesture=true`：

```text
旧 TAP ACTION_UP
→ 当前一指 HOVER_MOVE
```

不是 CANCEL，也不是把旧 id 强改成 hover。持续 TAP_DRAG 则不再每帧复核最初 tap-drag 窗口；它一直保持到抬手、按钮、多指或其他结束条件介入。

---

## 9. 按钮先于手指数，但没有先于 QUIET

### 哪些 button 才把 pointer 视为 down

公共辅助函数 `isPointerDown()` 只检查：

```cpp
AMOTION_EVENT_BUTTON_PRIMARY
| AMOTION_EVENT_BUTTON_SECONDARY
| AMOTION_EVENT_BUTTON_TERTIARY
```

BACK、FORWARD 与 stylus button 单独变化不会令状态机进入 BUTTON。BACK/FORWARD 还有独立的合成 KeyEvent 路径，不能因 `buttonState != 0` 就断言当前是点击拖动。

而且普通状态选择先检查 QUIET。quiet 窗口内按下 primary，当前帧仍保持 QUIET；只有后续输入帧到达且 `when >= quietTime + quietInterval`，按钮分支才有机会接管。

### BUTTON 可以在零根手指时成立

只要有效按钮 down 且未被 QUIET 压住，手指数为 0 也能进入 `BUTTON_CLICK_OR_DRAG`。输出 gesture id0、pressure=1，坐标就是当前 PointerController 位置。

若有 active finger 且它在上一 finger 集合中，状态机按 HOVER 相同的 relative→scale→rotate→velocity-control 管线移动 PointerController。只有 active id 不在上一集合时，本帧 delta 才为 0 并重置速度控制；若它只是从上一集合中接任，则仍可立即使用自己的 current-last delta。

### 多指按键拖动为何会换 active finger

集成式 button-pad 常由一根手指施力、另一根手指拖动。BUTTON 每帧可遍历所有 finger 的 VelocityTracker 速度：

```text
bestSpeed 初值 = dragMinSwitchSpeed
只在 speed > bestSpeed 时记录 bestId
最终 bestId 有效且不同于 activeTouchId 才切换
```

默认构造门槛为 50px/s。遍历按低 pointer id 进行；相等速度不替换当前 best，因此严格大于和 id 顺序共同决定平局结果。选中的新 finger 若上一帧已存在，可立即按它自己的 raw delta 移动；若刚落下，则本帧仍不移动。

### 进入 BUTTON 是正常结束旧流，不是语义取消

只要上一 mode 不是 BUTTON，代码就设 `finishPreviousGesture=true` 并把 active gesture id 设为 0。若旧 TAP/TAP_DRAG/PRESS/SWIPE/FREEFORM 有 down id，dispatch 会：

```text
旧流 ACTION_POINTER_UP / ACTION_UP
→ 当前 BUTTON 的 ACTION_DOWN
```

两组流即使都使用数值 id0，也因中间 UP 与新的 `downTime` 而属于不同生命周期。它不是 CANCEL。

按钮释放时：

- 仍有至少两指：从 BUTTON 优先进入 QUIET；
- 只剩一指：正常 finish BUTTON，再发当前 HOVER_MOVE；
- 零指：ACTION_UP 后进入 NEUTRAL，并补一枚合成 HOVER_MOVE；不会从 BUTTON 误判 TAP，因为非 HOVER/TAP/TAP_DRAG mode 已清掉 tap 资格。

buttonState 变化确实能强制 down 流发 MOVE，但只在“无 finish/cancel、旧/新均有共同 gesture id”的连续流中成立；它不是一条绕过全部前置条件的全局规则。

---

## 10. 多指先承诺 PRESS：settle 不是延迟分类定时器

### 为什么不能等意图明确后再发第一个事件

刚出现两根或更多 finger 时，框架还不知道用户要同向移动、张合旋转，还是只想长按当前位置。若完全等到移动越过阈值才发 DOWN，按压反馈和 long-press 起点都会被推迟。

所以首次进入多指分支时，无论当前是两指还是三指以上，都先：

1. `finishPreviousGesture=true`；
2. current mode 设为 PRESS；
3. active gesture id 固定为 0；
4. 清 reference id；
5. 重置 pointer velocity control；
6. 以当前 touching pointers 的 raw centroid 为 `referenceTouchX/Y`；
7. 以当前 PointerController 位置为 `referenceGestureX/Y`。

PRESS 随即向下游承诺一条 pressure=1 的单 pointer DOWN 流。PRESS 期间输出锚点保持静止，但每根手指的 raw 位移仍在内部累计。

### 两套 reference 不在同一坐标系

| 字段 | 坐标系 | 用途 |
|---|---|---|
| `referenceTouchX/Y` | raw surface 单位 | 初始取物理质心，随后由 common vector 推进的相对偏移基准 |
| `referenceGestureX/Y` | 显示逻辑像素 | 合成 gesture pointer 的锚点 |
| `referenceDeltas[id]` | raw 增量 | PRESS 分类与共同运动提取 |
| PointerController position | 显示逻辑像素且受边界约束 | 主光标与收尾 hover |

第一次建 PRESS 时，`referenceTouch` 取 raw touching centroid，两套 reference 在语义上对齐，却不数值相等。后续它只按逐轴最小共同分量推进，不保证仍等于当前物理质心；FREEFORM 使用的是“显示锚点 + raw 相对这份动态 referenceTouch 经 zoom/旋转后的偏移”，并非触控板到屏幕的绝对投影。

### settle 真正控制的是“窗口内加指是否推翻旧参考”

源码先计算：

```cpp
settled = when >= firstTouchTime + multitouchSettleInterval;
```

但随后无论 `settled` 是否为真，都继续累计 delta 并尝试 PRESS 分类。settle 不会强制 PRESS 至少保持 100ms。

它只在以下条件同时成立时生效：

```text
上一 mode 已经是 PRESS / SWIPE / FREEFORM
&& 尚未 settled
&& currentFingerCount > lastFingerCount
```

此时先设 CANCEL，把 mode 置回 PRESS，并按新集合重建 centroid/gesture reference。窗口内新手指加入会推翻已经交给 App 的旧多指解释；首次从 HOVER/TAP 进入多指则用 finish，不是这个 settle CANCEL。重建后同一次调用仍会累计 continuing fingers 的本帧 delta 并执行分类：通常保持 PRESS，若至少两枚旧 finger 的单帧位移已严格过门，也可能立刻再到 FREEFORM。

还有三个边界：

- `firstTouchTime` 从本轮第一根 active finger 出现时开始，不从第二根落下时开始；
- active finger 被剩余手指接替或重建 PRESS，都不重置它；
- 同一帧一根旧 finger 消失、一根新 finger 加入而总数不变，`currentCount > lastCount` 为假，不会触发 settle CANCEL；新 id 只获得清零后的 reference delta。

因此 settle 是“早期新增数量的参考重建窗口”，不是完整的 pointer membership 代际检测器。

---

## 11. PRESS 怎样分类：先累计两根，再看手指数、宽度和夹角

### reference delta 是跨帧累计量

每帧只对 current 与 last 都存在的 finger 累加：

```cpp
delta[id].dx += currentRaw.x - lastRaw.x;
delta[id].dy += currentRaw.y - lastRaw.y;
```

新纳入 `referenceIdBits` 的 finger 先把 delta 清零。PRESS 不消费共同移动，所以微小的多帧位移可以积累到分类门槛，而不是要求单帧跳过 15px。

### common vector 不是平均值

`calculateCommonVector(a, b)` 逐轴工作：

```text
两值都正 → 取较小正值
两值都负 → 取绝对值较小、较接近零的负值
异号或任一为零 → 0
```

这里只折叠 `commonIdBits = lastFingerIds ∩ currentFingerIds`：新指的 delta 虽会清零，但它在落指当帧不参与 common 折叠。对共同集合而言，如果该轴所有累计量都严格同号，就得到最小共同幅度；只要有一根为零或反向，该轴共同分量便为 0。它表达“所有持续手指都可贡献的整体平移”，不是质心平均速度。

在 PRESS 中共同量只参与计算，不移动 reference。进入 SWIPE/FREEFORM 后，只要 X 或 Y 任一共同量非零，源码会：

1. 把所有 reference finger 的 dx、dy 两轴一起清零；
2. raw 共同量加入 `referenceTouch`；
3. 乘 movement scale、旋转、经过 velocity control；
4. 加入 `referenceGesture`。

所以即使只有共同 X 非零，也会同时清掉各指累计的 Y delta；不要假定它逐轴独立消费历史。

### 分类顺序是一棵有短路的树

先为每个 reference finger 计算：

```text
dist[id] = hypot(delta.dx * zoomScaleX,
                 delta.dy * zoomScaleY)
```

必须至少两根 finger 的 `dist > multitouchMinDistance`，才继续：

```text
至少两根严格越过运动门槛
  ├─ currentFingerCount > 2
  │    └─ CANCEL PRESS → FREEFORM
  └─ 恰好 2 指
       ├─ raw mutualDistance > maxSwipeWidth
       │    └─ CANCEL PRESS → FREEFORM
       └─ 宽度合格
            ├─ cosine >= 0.2588 → SWIPE
            └─ cosine <  0.2588 → CANCEL PRESS → FREEFORM
```

这解释了几个反直觉结果：

- 三指落下后若只有一指移动，仍可长时间保持 PRESS；
- 恰好等于 15px 仍过不了外层严格 `>` 门；
- 两指很宽也不会在落下瞬间 FREEFORM，要先有两根越过运动门；
- cosine 约束很宽松：夹角不超过约 75° 都可判 SWIPE；
- X/Y zoom scale 在当前实现相同，但判断仍写成二维向量。

### 一旦分类，方向结果具有粘性

SWIPE 保持恰好两指时，不会重新检查宽度或夹角。之后两指改成反向、拉得更宽，仍保持 SWIPE。它唯一显式的后续分类转换是手指数大于 2 时转 FREEFORM。

在无有效按钮且 usage 不变的正常多指分支内，FREEFORM 不会因方向改变而降回 SWIPE/PRESS。按钮分支、stylus/mouse usage 抢占、abort/reset、settle 内加指重建，以及手指数掉到 0/1，仍可令它离开。

第三指加入 SWIPE 的结果依时间而异：

- 尚在 settle 窗口：前面的加指规则先 CANCEL 并重建 PRESS；本次随后仍分类，通常保持 PRESS，也可能立即 FREEFORM；
- 已 settled：继续到 SWIPE 分支，直接 CANCEL→FREEFORM。

PRESS 在 settled 后增加第三指不会仅因“3”立即转换；它继续 PRESS，直到至少两根累计位移严格过门。

---

## 12. SWIPE 与 FREEFORM：一边是假单指，一边才重建多指拓扑

### PRESS→SWIPE 为什么可以连续

PRESS 与 SWIPE 都只把 `currentGestureIdBits` 设为一个 `activeGestureId`，properties 是 FINGER、pressure=1，坐标是 `referenceGestureX/Y`。两者拓扑相同：

```text
PRESS：单 pointer DOWN，锚点暂不动
SWIPE：同一 pointer 后续 MOVE，锚点跟随共同分量
```

因此 PRESS→SWIPE 不设 CANCEL。App 只看到同一条单 pointer down 流从静止变为移动。

SWIPE 不是 `ACTION_SCROLL`，`MotionClassification` 仍是 NONE，也不是两个 pointer。其 source 以 MOUSE 为基础，事件 action 仍是 DOWN/MOVE/UP；上层若要把它解释为滚动，是另一层策略。

更关键的是，SWIPE 只更新 `referenceGestureX/Y`，没有调用 `PointerController::move()`。所以：

- MotionEvent 的合成 pointer 坐标可以移动；
- 可视主光标与通知中的 `cursorPosition` 可停在原锚点；
- 合成坐标不经过 PointerController 的显示边界裁剪；
- 结束后的合成 HOVER_MOVE 又使用 PointerController 坐标，可能与最后一枚 UP 明显跳变。

“SWIPE 让光标移动”因而不准确；它让 gesture 坐标移动，主光标轨道保持独立。

### PRESS→FREEFORM 为什么必须 CANCEL

PRESS 已向下游承诺“单 pointer id0、锚点语义”。FREEFORM 要改成“每根 finger 各有一个 gesture id、围绕动态 referenceTouch 的多 pointer 语义”。若直接沿用，App 会看到一个 pointer 突然裂成多个且坐标定义改变。

所以正常 PRESS→FREEFORM 会：

```text
旧 PRESS ACTION_CANCEL
→ 新 FREEFORM ACTION_DOWN
→ 新 FREEFORM ACTION_POINTER_DOWN ...
```

旧 id0 即使立刻又被新流分配，也因 CANCEL 与新的 downTime 隔开，不具连续身份。

### FREEFORM 怎样分配并保留 gesture id

典型 PRESS/SWIPE→FREEFORM 的 CANCEL 路径会先把 active gesture id 置为 -1；状态机按当前 touch id 从低到高遍历，依次取 bitset 中第一个未占用 gesture id，建立：

```text
freeformTouchToGestureIdMap[touchId] = gestureId
```

继续 FREEFORM 时：

1. current∩last 的 touch 沿用旧映射；
2. 上一帧全部 `lastGestureIdBits` 先作为 used 集合；
3. 消失 touch 对应的 active gesture 若离场，active 暂置 -1；
4. 新 touch 再取首个未占用 id；
5. active 为空时，选当前 gesture id 集合中的最低 id。

第 2 步带来一个精确边界：同一帧旧 touch 抬起、新 touch 落下，新 touch 不会立即复用刚释放的 gesture id，因为那个 id 仍在本帧的 used 集合里；到下一帧后才可能重新可用。

正常延续 FREEFORM 时，触点增删会展开普通 POINTER_UP/POINTER_DOWN，而非一律 CANCEL。settle 窗口内“手指数增加”是例外：它会先取消整条流、重建 PRESS，再在同次调用继续分类。

还有一个更窄的例外：若上一态本来就是 FREEFORM，settle 内加指后同帧又分类回 FREEFORM，映射代码仍走 continuation 分支，surviving touch 可复用旧的数值 id，active 也可能保留；但 dispatch 已用 CANCEL 和新的 downTime 切断前后两条事件流的身份连续性。

### FREEFORM 坐标围绕锚点展开

每根输出 pointer 的核心公式是：

```text
deltaRaw = physicalTouch - referenceTouch
deltaPx  = rotate(deltaRaw * zoomScale)
gesture  = referenceGesture + deltaPx
```

非 PRESS 状态提取到共同群组运动时，`referenceTouch` 与 `referenceGesture` 一起移动，使整体平移与局部形状变化分离。FREEFORM 本身也不调用 PointerController move，坐标不受主光标边界裁剪。

这套 gesture id 与坐标只服务当前 FREEFORM 生命周期；它不修改第 183 章 Raw/Cooked pointer id 的身份账。

---

## 13. QUIET 没有自己的 timeout：它丢的是过渡帧，不是触点

### 哪些退场会进入 QUIET

普通帧先更新 active touch，再判断 quiet：

- 上一 mode 为 PRESS/SWIPE/FREEFORM，当前少于两指但仍有 active finger；
- 或上一 mode 为 BUTTON，按钮已释放且当前仍有至少两指。

满足时记录 `quietTime=when`。第一次进入 QUIET 还会 `finishPreviousGesture=true`，然后：

```text
activeGestureId = -1
currentGestureMode = QUIET
currentGestureIdBits = empty
reset pointer velocity control
```

若 PRESS/SWIPE/FREEFORM 中的手指一次全部抬光，activeTouchId 会先变 -1 并 `resetQuietTime()`；这几种上一 mode 已令 tap 资格失效，所以随后直接进入 NEUTRAL，不进 QUIET、也不生成 TAP。QUIET 主要保护“仍残留手指”的退场。

### 进入 QUIET 的第一帧仍可能有两个事件

上一 PRESS/SWIPE/FREEFORM，或“按钮释放且仍有至少两指”的 BUTTON，都是 down 流。QUIET 的 finish 会先正常发送旧 UP；因为当前不是 HOVER、当前 down 集合又为空，dispatch 接着用 PointerController 坐标合成一枚 `HOVER_MOVE`。

```text
零到多枚 POINTER_UP，最后一枚 ACTION_UP
→ 合成 HOVER_MOVE（PointerController 坐标）
→ QUIET 中间帧通常无 MotionEvent
```

所以“进入 QUIET 后完全没有事件”不准确。它阻止的是剩余 finger 立即控制主光标，不是禁止收尾 hover 协议。

这也放大了 SWIPE/FREEFORM 的双坐标轨道：最后 UP 可位于已移动的 gesture 坐标，随后 HOVER_MOVE 回到未移动的主光标位置。

### 100ms 后不会自动醒来

quiet 判断是：

```cpp
isQuietTime = when < quietTime + pointerGestureQuietInterval;
```

源码没有为 quiet deadline 调用 `requestTimeoutAtTime()`。必须等下一份输入到来才重判：

- deadline 前的新包仍被 QUIET 吸收；
- 精确等于或晚于 deadline 的新包走按钮/0/1/多指正常分支；
- 全部抬起的新包会立即清 quiet 并进 NEUTRAL。

如果设备在 quiet 内持续上报位移，每个包虽不产生 gesture MotionEvent，Raw/Last State 仍向前推进，结束时通常只看最后一小段 delta。由这套账本更新可以推断：若设备长期完全不发包，下一包可能包含自上个状态以来较大的 raw 差值；QUIET 不是“冻结并清空所有未来运动”的硬保证。

quiet 内按钮状态仍在 prepare 末尾写给 PointerController，但按钮 mode 被优先级压住。窗口过期也没有异步唤醒；还要再来一个包，BUTTON 才可能发 DOWN。

---

## 14. prepare 之后还有一台事件机：finish、cancel 与 id 差集

### 先确定 current，再处理 old

`preparePointerGestures()` 计算并持久化 current gesture state、finish/cancel 标志与本帧发送条件；过程中还可能移动 PointerController、更新速度或请求 TAP timeout。它本身尚不把 gesture 状态展开成 MotionEvent，`dispatchPointerGestures()` 才把新旧两份状态变成 action。若 finish 与 cancel 同时为真，调用者先强制 `cancel=false`；正常结束优先于语义作废。

被视为 down 的 mode 只有 TAP、TAP_DRAG、BUTTON、PRESS、SWIPE、FREEFORM。NEUTRAL、HOVER、QUIET 在本轮结束时都会清 `lastGestureIdBits`。

### 三种事件展开不能混成一张线性图

#### finish 帧

```text
对全部旧 ids：低 id 顺序 POINTER_UP / 最后一指 UP
→ 不发 MOVE
→ 若 current 是新 down 流：低 id 顺序 DOWN / POINTER_DOWN
→ 否则按条件补 HOVER_MOVE
```

#### cancel 帧

```text
对全部旧 ids：一枚 ACTION_CANCEL
→ 清旧 dispatched 集合
→ 不发 MOVE
→ 若 current 是新 down 流：DOWN / POINTER_DOWN
```

#### 无 finish/cancel 的普通拓扑变化

```text
旧集合 - 新集合：依次 POINTER_UP / UP
→ 共同 ids 的属性或坐标变化时 MOVE
→ 新集合 - 已派发集合：依次 DOWN / POINTER_DOWN
```

因此 `{gesture0, gesture1} → {gesture1, gesture2}` 的正常 FREEFORM 帧可以是：

```text
POINTER_UP(id0)
→ 可选 MOVE(id1)
→ POINTER_DOWN(id2)
```

但 PRESS→FREEFORM 是 CANCEL→DOWN/POINTER_DOWN，不含 MOVE；TAP→BUTTON 是 UP→DOWN，也不含 MOVE。旧流程图把 finish/cancel 后仍连到 MOVE，是错误的。

普通拓扑帧还有一条坐标快照规则：在真正发 UP 之前，`updateMovedPointers()` 会把 current∩last 的 properties/coords 覆盖进 `lastGesture*`。因此 POINTER_UP 包里的 surviving ids 已是本帧坐标，removed id 仍用上一帧坐标；后续 MOVE/POINTER_DOWN 再使用 current 数组。finish/cancel 明确跳过这次覆盖，所以整组旧 UP/CANCEL 都使用 last 快照。

### moveNeeded 有四层前提

down MOVE 只有在以下条件全部成立时才计算：

1. current mode 属于 down；
2. 没有 cancel；
3. 没有 finish；
4. last/current gesture id 集合都非空。

它只比较两边共同 id 的 properties 与 coords。单纯新增/删除 id 不自动令 MOVE 成立；但在已有共同 id 的连续 down 流中，buttonState 改变会强制 `moveNeeded=true`。

HOVER 不走这套去重，每个 HOVER 帧直接发 `HOVER_MOVE`。本 gesture mapper 也不显式发 HOVER_ENTER/HOVER_EXIT；后续 hover target 语义不应从这段函数凭空补写。

### downTime 在“集合从空变为一个 id”时开始

添加 down ids 的循环每加入一枚 id，就看当前已派发数量。数量刚变为 1 时把 `downTime=when`：

- TAP：通常是物理抬指确认时；
- PRESS：首次多指产生 PRESS DOWN 时；
- CANCEL 后重建 FREEFORM：新 FREEFORM 第一枚 DOWN 时；
- FREEFORM 保留一枚旧 id 再加新 pointer：集合从未归零，downTime 沿用；若普通换指令旧集合全部 UP，再加首枚新 id，也会重新计时。

gesture HOVER_MOVE（包括收尾合成项）仍携带当前 `mPointerGesture.downTime`。正常 finish 不主动清它，所以 hover 可暂带刚结束 down 流的旧值；reset/abort 才把字段清为 0，不能用 hover 的 downTime 证明当前仍有 down 生命周期。

`dispatchMotion()` 最后按 gesture id 从低到高重新打包数组，把 `changedId` 所在数组位置编码进 action index；单 pointer 的 POINTER_DOWN/UP 会改写成 DOWN/UP。gesture id、数组 index 与 action index 仍是三种概念。

### 收尾 HOVER_MOVE 的触发条件

若 current mode 就是 HOVER，函数总发当前 HOVER_MOVE。否则，只要本帧展开完以后 dispatched down 集合为空、上一帧 down id 非空，就用：

```text
id=0, toolType=FINGER, pressure=0
x/y = PointerController position
```

直接构造 `NotifyMotionArgs(HOVER_MOVE)`。普通 TAP timeout、down 流进 QUIET/NEUTRAL 都可能触发；CANCEL 后立即建立 FREEFORM 时集合不为空，不会补 hover。

---

## 15. 视觉、abort、reset 与 dump：四个不能由 App action 反推的边界

### single/multi gestureMode 主要控制呈现

`INPUT_PROP_SEMI_MT` 默认令 `gestureMode=SINGLE_TOUCH`，否则默认 MULTI_TOUCH；IDC 的 `touch.gestureMode` 可覆盖。

`preparePointerGestures()` 不读取这个参数。两种 mode 共用同一九态分类与 MotionEvent 拓扑，所以 “single-touch 配置绝不会产生多 pointer FREEFORM”不是 r48 代码保证。

差别主要发生在 PointerController：

| 场景 | MULTI_TOUCH 呈现 | SINGLE_TOUCH 呈现 |
|---|---|---|
| FREEFORM | 设置各 gesture spots，主光标 gradual fade | 不设 spots，主光标 immediate unfade |
| finish/cancel | 清 spots | 无 spots 可清 |
| FREEFORM→QUIET/NEUTRAL | 若 last 是 FREEFORM，主光标 gradual unfade | 常规逻辑 |
| 转到其他活动 mode | 主光标 immediate unfade | 主光标 immediate unfade |

`abortPointerGestures()` 则无论当前呈现如何都 gradual fade 并 clear spots。不能把“FREEFORM 结束”笼统写成主光标总会渐显。

### abort 只根据 down gesture id 发 CANCEL

usage 切换，或 viewport/deviceMode 改变后仍配置成 POINTER 的 surface 重配，可调用 `abortPointerGestures()`。离开 POINTER 时这段 mapper abort 不执行；configure 会标记 resetNeeded，随后以 `NotifyDeviceReset` 让 Dispatcher 取消旧连接状态。进入 gesture abort 后，只有 `lastGestureIdBits` 非空才发送 ACTION_CANCEL，然后 reset gesture、reset velocity、fade/clear spots。

HOVER 在每帧结束时会清 last gesture ids，所以从 HOVER abort 时，这里不发 gesture-domain HOVER_EXIT。视觉渐隐与事件协议不是同一完成点。

`PointerGesture::reset()` 清模式、关键时间、active id、current/last bitset、downTime 与 VelocityTracker；它没有逐项清数组、reference delta 和映射表。安全性来自 bitset 与重新建立 reference 后不再读取无效项，而不是所有内存字节都归零。

### Mapper reset 本身不发 MotionEvent

`TouchInputMapper::reset()` 清 Raw/Cooked、usage、gesture/simple、速度与视觉状态，本身不调用 dispatchMotion 发 CANCEL。`InputDevice::reset()` 随后发送 `NotifyDeviceReset`，InputDispatcher 再按 deviceId 为各连接合成取消事件。

`SYN_DROPPED` 正走这条设备 reset 链，并丢弃到且包括第一枚恢复 `SYN_REPORT`。旧 TAP timeout 没有取消句柄；回调到达时，只有 mapper 仍处 POINTER、usage 仍为 GESTURES 才进入 gesture timeout，而 reset 后的非 TAP 状态通常直接忽略它。

PALM 的 `cancelTouch()` 又不同：它先 abort 当前 pointer usage，再 `abortTouches()`。旧 down gesture 与旧 Raw/Cooked touching 集合都存在时，mapper 层可能发出两份基于不同 id/坐标账本的 CANCEL；随后 POINTER 分支既不受 `mCurrentMotionAborted` 门抑制，也不在全抬起时清该 latch。由此可推断，后续 PALM 会被 guard 挡住、可能不再触发 cancel，直到 mapper reset 或切到会清 latch 的非 POINTER 分支。这是 r48 实现缺口，不是理想 Palm 策略。

### stock dumpsys 能看配置，却看不到当前 mode

`dumpsys input` 的 InputReader 段会打印当前配置与派生量的格式化值：

- quiet、drag、tap、tap-drag、slop、settle、distance、cosine、ratio；
- 每个 POINTER mapper 的 movement/zoom scale 与 max swipe width；
- 最近 Raw/Cooked pointer 状态等通用字段。

其中 cosine、ratio 等字段可能只保留一位小数，mapper scale 只保留三位；dump 适合确认运行时采用了哪组配置，不足以精确复算临界值。

它不打印 current/last gesture mode、activeTouchId、finish/cancel 或 FREEFORM 映射。源码里的详细 `ALOGD("Gestures: ...")` 又受 `Macros.h` 的 `DEBUG_GESTURES` 控制，stock r48 默认值是 0。

因此只读分析可以静态推演这些内部量；真机若要逐帧确认，需要调试构建打开日志或添加受控插桩，不能声称一份默认 dumpsys 已直接证明 mode 转换。

---

## 16. 用九个只读练习，把状态机变成可复算模型

以下命令均在 Android 11 r48 源码根目录执行。每题都给出明确产物，不要求 macOS 上编译 AOSP。

### 练习一：修正完整调用链

```bash
sed -n '1415,1506p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '1506,1592p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2277,2330p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

产物：画出 `sync → processRawTouches → cookAndDispatch → dispatchPointerUsage → dispatchPointerGestures`，并在图上标明 Raw 组装、Cooked 分类、usage 仲裁、prepare、dispatch 五个位置。

### 练习二：证明按钮门与 mode 枚举

```bash
sed -n '535,665p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h
sed -n '48,62p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchCursorInputMapperCommon.h
```

预期答案：九个 mode 全部列出；单独 BACK/FORWARD 不满足 `isPointerDown()`，不能进入 BUTTON。

### 练习三：算出正常 AOSP 的 300ms 与 20px

```bash
sed -n '255,272p' \
  frameworks/native/services/inputflinger/include/InputReaderBase.h
sed -n '500,528p' \
  frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
sed -n '72,82p;130,162p;590,605p' \
  frameworks/base/core/java/android/view/ViewConfiguration.java
```

产物：分别写出 C++ 构造基线与 Java policy 后的有效默认值，手算 `max(min(400-100,300),150)=300ms`。

### 练习四：复算所有时间与距离边界

```bash
rg -n "when <=|when <|when >=|fabs\\(|pointerGestureTapSlop|dist.*>|mutualDistance >|cosine >=" \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

分别判断 tap 在 149/150/151ms、slop 在 20/20.1px、quiet 在 99/100ms、dist 在 15/15.1px 时的结果。注意 20px 是上题算出的正常 AOSP 值，不是 C++ 构造的 10px。

### 练习五：手写完整 double-tap action 时间线

```bash
sed -n '2546,2580p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2760,2922p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '2380,2485p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

预期序列：第一次物理 UP→DOWN；第二次物理 DOWN 续接且可能无事件；第二次物理 UP→旧 UP、同 `when` 新 DOWN；第二 timeout→UP、HOVER_MOVE。

### 练习六：比较 settle 内外的第三指

```bash
sed -n '2930,3112p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

构造 SWIPE 后加第三指：settle 内先 CANCEL 并重建 PRESS，本帧若两枚 continuing finger 都未严格越门则保持 PRESS，若都以大单帧 delta 越门则可立即 FREEFORM；settle 后直接 CANCEL→FREEFORM。再构造三指初落但只有一指超过门槛，证明它仍是 PRESS。

### 练习七：用两个向量算 SWIPE/FREEFORM

仍读上题代码，假设宽度合格、位移已经是 zoom 后像素：

```text
(20, 0) 与 (20, 0)  → cosine = 1 → SWIPE
(20, 0) 与 (0, 20)  → cosine = 0 → FREEFORM
```

再说明一旦得到 SWIPE，为何后续两指反向也不会重新分类。

### 练习八：推演 FREEFORM 同帧换指与 dispatch 顺序

```bash
sed -n '2380,2522p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '3170,3265p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

令一枚旧 touch 抬起、同帧加入新 touch。证明刚释放的 gesture id 仍被 `usedGestureIdBits` 占用；再写出普通拓扑变化的 POINTER_UP→可选 MOVE→POINTER_DOWN。

### 练习九：证明 quiet 无 timer、默认日志也不可见 mode

```bash
sed -n '2628,2675p' \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
rg -n "requestTimeoutAtTime|quietTime" \
  frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
sed -n '680,706p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
sed -n '34,44p' \
  frameworks/native/services/inputflinger/reader/Macros.h
```

预期答案：quiet 只有时间比较，没有 deadline 请求；dump 打印配置而非当前 mode；`DEBUG_GESTURES` 默认为 0。

### 检查题

1. TAP 的 `tapDownTime` 与 MotionEvent `downTime` 为什么不是同一时刻？
2. 精确到 tap-drag deadline 的 timeout 为什么不结束 TAP？
3. settle 为什么不能解释成“先等 100ms 再分类”？
4. PRESS→SWIPE 与 PRESS→FREEFORM 的事件拓扑差在哪里？
5. 为什么 SWIPE 的最后 UP 与下一枚 HOVER_MOVE 可能坐标跳变？
6. 默认 `dumpsys input` 能证明哪些配置，又不能证明哪个当前状态？

### 下一章

第 185 章转向触摸校准与 VirtualKey：继续沿 `TouchInputMapper` 追尺寸、压力、方向、距离的校准，以及屏幕外虚拟键的命中、quiet time 与 KeyEvent 合成。
