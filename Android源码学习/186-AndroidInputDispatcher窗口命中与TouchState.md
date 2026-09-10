# 186 Android InputDispatcher 窗口命中与 TouchState：一帧 Motion 怎样建立并延续目标

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`  
> `frameworks/native` 提交：`57b3d43492`；`frameworks/base` 提交：`1d9b9ab57d`  
> 学习方式：macOS 静态只读，不要求编译、不冒充真机实测  
> 前置章节：第 173—176、183—185 章

---

## 1. 先看故障现场：同一枚 POINTER_DOWN，为什么 A 收 MOVE、B 收 DOWN

屏幕上有两个并排窗口：

- A 已收到 pointer id 0 的 `DOWN`；
- A 支持 split touch；
- pointer id 1 随后落到 B；
- B 也支持 split touch。

InputReader 交给 Dispatcher 的第二帧只有一份原始 `MotionEntry`：

```text
action = POINTER_DOWN(index=1)
pointers = [id0@A, id1@B]
```

但 App 最终看到：

```text
A: MOVE, pointers=[id0]
B: DOWN, pointers=[id1]
```

这不是 InputReader 提前复制了事件，也不是 A、B 各自做了一次 hit-test。Dispatcher 先用旧 `TouchState` 知道 id0 已属于 A，再只为新落下的 id1 命中 B；随后针对每个 connection 裁出 pointer 子集，并把原 action 改写成该子集内部自洽的 action。

同一套机制还能解释几类看似无关的现象：

- 手指移出普通窗口，后续 `MOVE` 仍回原窗口；
- `FLAG_WATCH_OUTSIDE_TOUCH` 窗只在首个 `DOWN` 收一次 `OUTSIDE`；
- slippery 窗移交触摸时，旧窗收 `CANCEL`，新窗收 `DOWN`；
- wallpaper 能跟随整条触摸流，却不是 foreground target；
- target 查找失败，不一定意味着正式 `TouchState` 完全没变；
- `FLAG_SPLIT` 已存在，也不保证本帧一定调用 `splitMotionEvent()`。

本章只追一个问题：

```text
一帧 MotionEntry 怎样结合旧 TouchState，
生成本帧每个窗口的 InputTarget，
再决定哪些窗口—pointer 关系与临时 dispatch mode 要延续到下一帧？
```

回答时必须把三件事分开：

1. **命中**：此刻坐标在窗口栈里先遇到谁；
2. **路由记忆**：这一整条物理触摸已经归哪些窗口、各自拥有哪些 pointer id；
3. **本帧投影**：同一 `MotionEntry` 针对某个 connection 要裁哪些指、改成什么 action、用什么坐标变换。

---

## 2. 全链路不是“坐标找窗口”，而是 state → targets → per-connection event

### 从 MotionEntry 到 socket 前的主路径

```text
dispatchMotionLocked()
  ├─ 判断 source 是否属于 POINTER class
  ├─ findTouchedWindowTargetsLocked()
  │    old TouchState → tempTouchState
  │    hit-test / 延续旧目标 / 加旁路目标
  │    输出 TouchedWindow → InputTarget
  │    提交或拒绝 tempTouchState
  ├─ 成功后追加 global monitors
  ├─ conflictingPointerActions 时给所有 connection 合成 CANCEL
  └─ dispatchEventLocked()
       → prepareDispatchCycleLocked(connection, InputTarget)
           ├─ 必要时 splitMotionEvent()
           └─ enqueueDispatchEntriesLocked()
                → dispatch mode 改 action
                → connection.outboundQueue
                → publishMotionEvent()
```

`findTouchedWindowTargetsLocked()` 不直接向 App 写 socket。它的直接产物是 `std::vector<InputTarget>`；每项描述：

- 发往哪个 `InputChannel`；
- 是 foreground、split、zero-coords，还是 obscured；
- 本帧要 AS_IS、OUTSIDE、HOVER_ENTER/EXIT、SLIPPERY_ENTER/EXIT 中的哪几种投影；
- 使用全部 pointers，还是只使用一组 pointer id；
- 最终发布时采用哪组 frame offset、window scale 与 global scale。

### 三本账处在不同生命周期

| 账本 | 存活范围 | 主要内容 |
|---|---|---|
| `MotionEntry` | 一帧输入 | 原 action、所有 pointers、display/device/source、downTime |
| `TouchState` | 一条 display 级路由流 | down、split、设备身份、持久窗口集合、portal、gesture monitors |
| `InputTarget / DispatchEntry` | 本帧、每个接收 connection | pointer 子集、dispatch modes、坐标 offset/scale、resolved action/flags |

因此：

- `TouchState` 不保存每一帧坐标；
- `InputTarget` 不是“当前触摸状态”的长期真相；
- App 看到的 action 不一定等于 `MotionEntry.action`；
- 窗口成员可以保持不变，但本帧从窗口最新 `InputWindowInfo` 取得的 frame/scale 已变化。

### 本章源码地图

| 主题 | r48 文件 |
|---|---|
| pointer 事件入口、目标查找、提交 | `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp` |
| display 级触摸账与窗口合并 | `TouchState.h/.cpp`、`TouchedWindow.h` |
| 每个 connection 的 flags、pointer info | `InputTarget.h/.cpp` |
| touchable region、frame、trusted overlay、split capability | `frameworks/native/include/input/InputWindow.h`、`libs/input/InputWindow.cpp` |
| 下游连接一致性与取消依据 | `InputState.h/.cpp` |
| 窗口快照的生产链 | 第 173 章 |
| publish、FINISHED 与 ANR | 第 174—176、188—190 章 |

---

## 3. TouchState 是 display 级“路由记忆”，不是每指一台状态机

### 字段先按问题分组

```cpp
struct TouchState {
    bool down;
    bool split;
    int32_t deviceId;
    uint32_t source;
    int32_t displayId;
    std::vector<TouchedWindow> windows;
    std::vector<sp<InputWindowHandle>> portalWindows;
    std::vector<TouchedMonitor> gestureMonitors;
};
```

`mTouchStatesByDisplay` 以入口 `MotionEntry.displayId` 为 key。单个 state 记录：

- 当前是否存在按下流；
- 这条流是否进入过 split 模式；
- 当前流的 `deviceId/source/displayId`；
- 哪些窗口是 foreground、wallpaper 或临时投影目标；
- split 时每个窗口持有哪些 pointer id；
- 首 DOWN 以及后续 split POINTER_DOWN 穿过哪些 portal；其中 gesture monitor 仍只在首 DOWN 锁入；
- 首 DOWN 锁入了哪些 gesture monitor。

这带来第一个重要限制：

> r48 可以在不同 display 各维护一份 `TouchState`，但同一 display 的一份 state 只装一组 deviceId/source。

代码甚至保留了“测试多个同时输入流”的 TODO。不要把“map 按 display 分开”外推成“同一 display 可并行维护任意多台触摸设备”。

### TouchedWindow 的 pointerIds 只有 split 时才是所有权集合

```cpp
struct TouchedWindow {
    sp<InputWindowHandle> windowHandle;
    int32_t targetFlags;
    BitSet32 pointerIds; // zero unless FLAG_SPLIT
};
```

两种空集合含义必须区分：

| target 状态 | `pointerIds.isEmpty()` 的含义 |
|---|---|
| 没有 `FLAG_SPLIT` | 使用 default pointer info，接收原事件的全部 pointers |
| 有 `FLAG_SPLIT` | 正常持久目标不应为空；最后一指清除后窗口会被移除 |

`InputTarget::addPointers()` 明确把空 bitset 转成 `setDefaultPointerInfo()`。所以“空 pointerIds”在非 split 目标上是通配语义，不是“这个窗口没有触点”。

### split 是粘滞位

`TouchState::addOrUpdateWindow()` 只要看见一次 `FLAG_SPLIT` 就把 `split=true`。清理单个 pointer 或窗口时不会重新计算、也不会把它降回 false；只有 `reset()` 才清零。

这会影响后面的 slippery 边路：其他 split 窗口都已退场、只剩一指时，局部 `isSplit` 仍可能保持 true。

### FOREGROUND 不是键盘焦点

`InputTarget::FLAG_FOREGROUND` 在这里表示触摸主接收者：

- 它参与注入 UID 权限检查；
- `getFirstForegroundWindowHandle()` 用它找回退窗口；
- split 时可同时存在多个 foreground 触摸窗口。

它不等同于 `hasFocus`，也不保证是窗口栈最上层。一个带 `FLAG_NOT_FOCUSABLE` 的窗口仍可能在自身 touchable region 内成为触摸 foreground。

---

## 4. 每帧先复制 old state；“newGesture”只是算法分支名

### 副本顺序

函数开头先做：

```text
oldState = mTouchStatesByDisplay[entry.displayId]（若存在）
tempTouchState = copy(oldState)
isSplit = tempTouchState.split
```

后面的 hit-test、OUTSIDE、portal、wallpaper、pointer id 增删都先作用于 `tempTouchState`。直到函数尾部，才根据 permission、wrongDevice 与 action 决定是否写回 map。

这有事务味道，但不能简化成“成功才 commit”。第 8 节会给出真实提交矩阵。

### 哪些 action 重新建立本帧目标

`newGesture` 的 r48 定义是：

```cpp
DOWN || SCROLL || HOVER_MOVE || HOVER_ENTER || HOVER_EXIT
```

这个名字表示“正常到达该分支时，先 reset 临时 state，再做 Case 1 目标查找”，不等于每个 action 都会建立可持久的 App 手势。这里还有一道更早的设备冲突门：若旧 state 正处于 down、`SCROLL` 来自另一设备或 source，它会在 reset 与 Case 1 之前以 `FAILED + wrongDevice` 退出。

| action | 未被前置冲突门挡住时进入 Case 1 | `temp.down` | 尾部是否保存本次 temp |
|---|---:|---:|---:|
| `DOWN` | 是 | true | 通常保存 |
| `SCROLL` | 是 | false | **不保存** |
| `HOVER_ENTER/MOVE` | 是 | false | reset 后只保存设备身份，不保存窗口集合 |
| `HOVER_EXIT` | 是 | false | reset 并通常删除该 display state |
| split `POINTER_DOWN` | 是，但不是 newGesture | 保留 true | 保存新 pointer 归属 |
| 普通 `MOVE/UP/CANCEL` | 否 | 沿用旧值 | 使用旧目标后再做尾部清理 |
| 非 split `POINTER_DOWN` | 否 | true | 整帧沿用旧目标 |

`BUTTON_PRESS/RELEASE` 等不在 newGesture 集合里；它们若走 pointer 路由，会落入延续分支并受 `temp.down` 检查。

### 命中点先转成 int，而且 mouse 判断是精确相等

Case 1 取点：

```cpp
if (entry.source == AINPUT_SOURCE_MOUSE) {
    x = int32_t(entry.xCursorPosition);
    y = int32_t(entry.yCursorPosition);
} else {
    x = int32_t(entry.pointerCoords[actionIndex].getAxisValue(X));
    y = int32_t(entry.pointerCoords[actionIndex].getAxisValue(Y));
}
```

三个限定很重要：

1. `DOWN` 通常取 index 0，split `POINTER_DOWN` 取 action index 的新指；
2. float 到 `int32_t` 是向零截断，不是四舍五入或 floor；负小数尤其不同；
3. mouse 条件是 `source == AINPUT_SOURCE_MOUSE`，不是“只要带某个 mouse bit”。

而 slippery MOVE 另走一份代码，固定取 `pointerCoords[0].X/Y`，并不复用 cursor-position 分支。不能把“鼠标一律按系统光标 hit-test”写成全函数不变量。

### device/source 冲突不是一个统一 drop 规则

`switchedDevice` 比较旧 state 与本帧的 device、source、display。r48 分支并不对称：

- 另一设备的 `MOVE`：标记 `wrongDevice`，返回值甚至被设成 `PERMISSION_DENIED`，但正式 state 不变；
- down 流期间另一设备的 `SCROLL`：`FAILED` + `wrongDevice`，正式 state 不变；
- 新 `DOWN` 即使来自另一设备，也会重建 temp；若成功则报告 conflict；
- hover 可重建 temp，并在发现旧 state 正在 down 时报告 conflict；
- 不属于这些明确分支的异常 action 不能概括成“不同设备一律拒绝”。

`UP/CANCEL/POINTER_*` 没有同等的统一 early-return；异设备的这些异常 action 可能先沿旧 state 生成 targets，再在成功尾部标记 conflict。成功 conflict 由外层在真正派发当前事件前对**所有 connections**合成 pointer CANCEL，而不是只取消旧 `TouchState.windows`。

---

## 5. 真正的 hit-test：前到后、整数点、modal 或 Region，再决定是否穿 portal

### 普通窗口的判定顺序

`findTouchedWindowAtLocked(displayId, x, y, ...)` 按 `getWindowHandlesLocked(displayId)` 当前顺序从 front 到 back 遍历。Dispatcher 不在这里另做 Z 排序；第 173 章追过这份前到后快照怎样由上游产生。

每项依次检查：

```text
displayId 相等
  && visible
  && 没有 FLAG_NOT_TOUCHABLE
  && (isTouchModal || touchableRegion.contains(x, y))
```

第一个满足者立即返回，后面的窗口不再看。

`isTouchModal` 不是独立字段：

```cpp
(flags & (FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCH_MODAL)) == 0
```

也就是说：

- 同时没有这两个 flag：touch-modal，**不要求点在 touchableRegion 内**；
- 有任意一个：non-modal，必须命中 touchableRegion；
- `FLAG_NOT_FOCUSABLE` 不只影响键盘焦点，它也让这条触摸命中改走 Region 门；
- `FLAG_NOT_TOUCHABLE` 则无论 modal 与否都不能成为 hit target。

一个 visible、touchable、modal 窗甚至可在空 Region 或 frame 外成为首个目标。排查“透明区域为什么仍挡住触摸”时，不能只画窗口 frame。

### touchableRegion 与 frame 各有职责

`InputWindowInfo::touchableRegionContainsPoint()` 调用 `Region::contains()`。Region 可由多个矩形组成；r48 实现对每个矩形使用半开区间：

```text
left <= x < right
top  <= y < bottom
```

`frame` 在本函数的普通命中门里不是替代品。它主要用于：

- 建立窗口局部坐标的 `-frameLeft/-frameTop`；
- `frameContainsPoint()` 判断当前点是否被上层窗遮住；
- `overlaps()` 判断窗口任意部分是否被上层 frame 覆盖；
- portal monitor 的 offset。

因此“触摸命中了 Region”与“App 收到怎样的局部 X/Y”是两个问题。

### WATCH_OUTSIDE 是未命中后的旁路收集

若 visible 窗走过普通“未命中”路径，且本次调用 `addOutsideTargets=true`、窗口带 `FLAG_WATCH_OUTSIDE_TOUCH`，它会作为 `DISPATCH_AS_OUTSIDE` 加入 temp，再继续向后遍历。portal 是关键例外：一旦该窗按 modal/Region 规则命中且指向另一 display，函数会立即返回递归结果；无论目的 display 是否命中，都不会再执行这个 portal 窗自己的 outside 收集，也不会继续扫描原 display 下方的 watcher。

它甚至可以同时带 `FLAG_NOT_TOUCHABLE`：不可触摸会阻止它成为 foreground，却不会阻止 outside 收集。反过来，一个 touch-modal watcher 会先命中自己，根本走不到 outside 收集语句。

调用者只在 masked action 为首个 `DOWN` 时把 `addOutsideTargets` 设为 true。普通有 foreground 的路径上，这意味着只收集 final hit 之前遍历过的 watcher；但 monitor-only 例外要到第 7 节再限定。

### portal 先截断原 display，再用同一 x/y 递归

若命中的窗口 `portalToDisplayId` 有效且不同于当前 display：

```text
记录 portal window（仅 addPortalWindows=true 时）
  → 用同一整数 x/y 递归扫描 portalToDisplayId
  → 直接返回递归结果
```

三点容易漏：

- portal 本身不是最终 foreground target；
- 子 display 没命中时，不会回到原 display 继续扫描 portal 下方窗口；
- 代码没有在 hit-test 前把 x/y 减 portal frame，也没有 visited-display 集合；拓扑必须由上游保证无循环。

`addOutsideTargets` 会原样传入递归，所以首 DOWN 可沿多个 display 栈累积 watcher。`addPortalWindows=false` 时仍会递归，只是不把 portal 路径记进 `TouchState`。

---

## 6. hit 之后的“三关”只属于 Case 1，而且失败不会继续找下一层

### 回退发生在准入检查之前

Case 1 得到 `newTouchedWindowHandle` 后，先决定 split：

1. 新窗支持 split：局部 `isSplit` 依据 mouse 规则更新；
2. 手势已 split、但新窗不支持：忽略该新窗；
3. 新窗为空：回退到 `tempTouchState.getFirstForegroundWindowHandle()`。

回退主要服务 split `POINTER_DOWN`：

- 新指落在空白处，可归给已有 first foreground；
- 已 split 时命中不支持 split 的新窗，也可退回已有 foreground。

“first”只是 `temp.windows` 的插入顺序中第一项带 `FOREGROUND` 的窗口，不会重新比较当前 Z-order。

### 然后才检查 paused、channel、responsive

对这个 Case 1 的新选或回退目标，代码依次检查：

```text
paused?
有注册 connection?
connection.responsive?
```

任一失败都把目标置空。关键是：

> 这三关失败后不会再回退 first foreground，也不会恢复窗口栈扫描去尝试更低一层。

所以“顶层命中窗暂停，就把 DOWN 发给下面的 App”不是 r48 行为。对已 split 的 `POINTER_DOWN`，命中一扇 paused/unresponsive 新窗也可让整帧失败，而不是自动把新指塞回旧窗。

### 这不是持续流的通用准入门

检查位置只覆盖：

- newGesture 的 Case 1；
- 已 split 的 `POINTER_DOWN`；
- 它们的 first-foreground fallback。

普通 `MOVE/UP` 不重新检查。slippery 新目标也不走这三关。若一个已写入 state 的新窗没有 channel，`addWindowTargetLocked()` 稍后只会跳过那个 `InputTarget`；它不会撤销刚建立的 state 成员，也不会自动选另一窗。

窗口在手势中途变得 unresponsive，并不会仅凭这里的 Case 1 代码从 `TouchState` 移除；后续等待、ANR 与取消由 Dispatcher 的连接状态机处理。

### 首 DOWN 的 gesture monitor 能救无窗口事件，global monitor 不能

首 `DOWN` 同时收集当前 display 与 portal 路径上的 gesture monitors，并剔除当时无 connection 或 unresponsive 的项。

若新窗最终为空：

- 至少还有一个 responsive gesture monitor：本帧可以继续成功；
- gesture monitor 也为空：直接 `FAILED`。

global monitors 此时还没加入。它们只在 `findTouchedWindowTargetsLocked()` 已经返回成功后，由 `dispatchMotionLocked()` 每帧追加，所以不能把“系统有 global monitor”当作无窗口 DOWN 的成功条件。

---

## 7. 首 DOWN 怎样收集四类可能的接收者：foreground、OUTSIDE、wallpaper、monitor

### 一次正常 hit-test 至多返回一个 foreground 新目标

新目标通过 Case 1 三关后，先得到：

```text
FOREGROUND | DISPATCH_AS_IS
```

若局部 `isSplit=true`，再加 `FLAG_SPLIT` 并只 mark action pointer id；否则 pointerIds 为空，代表接收完整原事件。

它还在加入 temp 时记录本次的 obscured 或 partially-obscured flag。普通持续 MOVE 不重新 hit-test，也不逐帧重算这些安全 flag。

### OUTSIDE 通常只来自 final foreground 之上的 watcher

普通 DOWN 有 final foreground 时，遍历在命中处 return，因此：

- 上方已遍历的 visible watcher 可收 `OUTSIDE`；
- final target 下方窗口不会被继续扫描；
- final target 本身不会同时作为 outside watcher。

随后 Dispatcher 取 first foreground 的 `ownerUid`，逐个比较 outside 窗：

```text
outside.ownerUid != foreground.ownerUid
  → 加 FLAG_ZERO_COORDS
```

真正 publish 时，`FLAG_ZERO_COORDS` 调用 `PointerCoords::clear()`，清掉的不是只有 X/Y，而是该 pointer 的全部 axis 数据；`xCursorPosition/yCursorPosition` 两个独立字段没有在这段代码中同步改写。相同 UID 的 outside 目标则仍使用自己的 frame offset/scale。

### monitor-only 是 OUTSIDE 通则的 r48 边路

若扫描完没有 foreground，但 responsive gesture monitor 存在，函数仍可成功。此时有两个反直觉结果：

1. 扫描可能已经把沿途所有 visible WATCH 窗放入 temp，而不再是“某个 final target 上方”；
2. ZERO_COORDS 代码只在 first foreground 非空时运行，所以这条 monitor-only 路径不做 foreground UID 比较。

无 foreground、也无 gesture monitor 时则在输出 targets 前失败，一个 `OUTSIDE` 都不会发。

所以安全分析要写成：

> “普通 foreground DOWN 的跨 UID OUTSIDE 会清 PointerCoords”，而不是“所有 OUTSIDE 无条件跨 UID 清零”。

### wallpaper 只由首 DOWN 锁入

通过 permission 检查后，若 first foreground 的 `hasWallpaper=true`，Dispatcher 遍历**入口 displayId** 的窗口列表，把每个 `TYPE_WALLPAPER` 加入：

```text
DISPATCH_AS_IS
| WINDOW_IS_OBSCURED
| WINDOW_IS_PARTIALLY_OBSCURED
```

这段代码不再检查 wallpaper 的 visible、touchable、paused、responsive，也不给它 `FOREGROUND` 或 `FLAG_SPLIT`。其 pointerIds 为空，因此即使主手势分给多个 App，wallpaper 仍按非 split 通配语义接收整份 pointer 流。

wallpaper 只在首 `DOWN` 收集并随 state 锁住；`HOVER_MOVE` 与 `SCROLL` 明确不收集。若 foreground 是 portal 后的子 display 窗，这段枚举仍使用原始 `entry.displayId`，不能擅自改写成“最终目标所在 display”。

### obscured 看 frame 与进程边界，不看 touchableRegion

一个上层窗只有同时满足下列条件才可遮挡目标：

- token 不同；同 token 的 cloned layer 不算；
- visible；
- `ownerPid` 不同；
- 不是 r48 硬编码类型集合里的 trusted overlay；
- displayId 相同。

然后：

| 计算 | 几何 | 普通新目标结果 |
|---|---|---|
| `isWindowObscuredAtPointLocked` | 上层 `frameContainsPoint(x,y)` | 加 `WINDOW_IS_OBSCURED` |
| `isWindowObscuredLocked` | 上层 frame 与目标 frame overlap | 点未挡但任意部分重叠时加 `PARTIALLY_OBSCURED` |

普通路径在**单次 targetFlags 计算**里使用 `if ... else if`，本次新增的两项互斥；这不保证 `TouchedWindow` 生命周期里的最终 bit 永远互斥。后续 split `POINTER_DOWN` 若再次命中同一 handle，`addOrUpdateWindow()` 会把新 flags 与旧 flags 做 OR，于是它可能先拿到 partial、后又拿到 obscured，最终两项并存；下游也会分别映射两项。same UID 但不同 PID 并不会自动豁免；判断是 ownerPid。slippery 新窗只算 point-obscured、不补 partial；wallpaper 则被强制同时加两项。

这些 target flags 最终转成 `AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED` 与 `...PARTIALLY_OBSCURED`。它们不是点击穿透规则，也不是对上层 alpha 的通用可见性计算。

---

## 8. temp 不是“成功才提交”：permission、wrongDevice、failure 要分四层

### 成功路径先输出，再过滤临时窗口

正常成功顺序：

```text
permission 检查通过
  → 加 wallpaper
  → temp.windows / gestureMonitors 转成本帧 InputTargets
  → filterNonAsIsTouchWindows()
  → 按 action 做 POINTER_UP / UP / CANCEL 等尾部变化
  → 写回 mTouchStatesByDisplay
```

`filterNonAsIsTouchWindows()`：

- 保留有 `DISPATCH_AS_IS` 的窗口；
- 也保留 `SLIPPERY_ENTER`，但把 dispatch mode 统一改成 `AS_IS`；
- 删除纯 OUTSIDE、HOVER_EXIT、SLIPPERY_EXIT 等临时项。

因此临时目标先参与本帧 `InputTarget` 生成，再从下一帧 state 中消失。

### 真实提交矩阵

| 结果/条件 | 输出正常 targets | 写回 temp | 额外行为 |
|---|---:|---:|---|
| 成功且 permission granted | 是 | 是，SCROLL 除外 | conflict 时先全连接 CANCEL |
| `injectionPermission=DENIED` | 否 | **否** | 立即返回 |
| `wrongDevice=true` | 否 | **否** | 保留旧 state |
| 其他 FAILED、但 permission granted | 否 | **仍可能写回** | 仍执行 action 尾部清理 |
| SCROLL | 成功才输出 | **永不写回本次 temp** | 仍可能更新全局 hover handle |

permission 检查分两种：

- 有 foreground 时，逐个 foreground 调 `checkInjectionPermission(window, injectionState)`；
- 提前失败且权限仍 UNKNOWN 时，用 null window 做一次最终检查。

物理事件的 `injectionState==nullptr` 会通过这道检查。注入事件若没有全局注入权限，在 null-window 失败边路即使本来可能拥有某个 App UID，也无法借一个不存在的目标提交状态。

还有一个 monitor-only 例外：没有 foreground、但有 gesture monitor 时，foreground 循环为空，存在性检查由 monitor 满足，代码直接把 permission 标成 granted。不能把“无权注入的 DOWN 一定无法提交 state”写成无条件结论。

### 无目标 DOWN 失败，state 甚至未必为空

首 DOWN 在 Case 1 开头已经：

```text
temp.reset()
temp.down=true
写入 device/source/display
```

若后来因为无目标、paused、无 channel 或 unresponsive 而 `goto Failed`，物理事件仍可能在尾部保存这个 down state。

而且 hit-test 可能早已收集 OUTSIDE 或 portal。失败发生在成功路径的 `filterNonAsIsTouchWindows()` 之前，所以写回的 `windows` **未必为空**，临时 OUTSIDE 项也可能残留在正式 state 中。它们没有生成本帧 target；后续通常继续因没有 foreground/gesture monitor 失败，直到获准提交的 `UP/CANCEL` 清账。

不要给这种行为附会“特意保存物理序列一致性”的设计目的；源码只证明执行顺序和结果。

### injectionResult 与实际收件人不是同一个量

- `injectionPermission` 未获 GRANTED 一定不提交 temp；foreign MOVE 虽也返回 `INPUT_EVENT_INJECTION_PERMISSION_DENIED`，实际走的是另一个 `wrongDevice` 分支；
- `FAILED` 不等于不提交；
- `SUCCEEDED` 也不保证每个 temp window 都产生 `InputTarget`：`addWindowTargetLocked()` 遇到已注销 channel 会跳过该项；
- gesture monitor 可令没有 foreground 的 DOWN 成功；
- global monitor 只有成功后才追加，不能反向改变结果。

外层 `dispatchMotionLocked()` 对失败与权限拒绝又不同：

- permission denied：记录并直接丢；
- 其他失败：给 monitors 合成 pointer CANCEL；
- 成功 conflict：给所有 connections 合成 pointer CANCEL，再派发当前事件。

这就是为什么排障不能只看一个 injection result 常量。

---

## 9. split 在首个目标处开启；后续只为新 pointer 重新命中

### 首 DOWN 决定普通手势能否分窗

Case 1 对新命中窗执行：

```cpp
if (newWindow != nullptr && newWindow->supportsSplitTouch()) {
    isSplit = !isFromMouse;
} else if (isSplit) {
    newWindow = nullptr;
}
```

首 DOWN 时旧 state 已 reset，`isSplit=false`。因此普通 touchscreen 流：

| 首目标 | 首 DOWN 后状态 | 第二指落下 |
|---|---|---|
| 支持 `FLAG_SPLIT_TOUCH` | `split=true`，首窗只 mark 首 pointer id | 进入 Case 1，按新指坐标重新 hit-test |
| 不支持 | `split=false`，首窗 pointerIds 为空 | 进入 Case 2，不 hit-test，完整事件仍给首窗 |

所以不是“每个新 pointer 都先问自己脚下的窗口”。只有 state 已 split，且：

```cpp
isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN
```

才会为新指进入 Case 1。判断必须用 masked action；完整 action 还带 pointer index bits。

### array index 只是这一帧的位置，pointerId 才是持久所有权

代码先用 action index 找本帧变化项，再读：

```cpp
pointerId = entry.pointerProperties[pointerIndex].id;
```

写入 `TouchedWindow.pointerIds` 的是 id。下一帧 pointer 数组可以重排，`splitMotionEvent()` 仍按 id 扫描并重建目标子集；不能把“数组第 1 项”长期当作“pointer id 1”。

### split POINTER_DOWN 的四种去向

假设 id0 已属于 A：

| id1 几何命中 | 当前局部结果 |
|---|---|
| 支持 split 的 B | B 获得 id1 |
| 不支持 split 的 B | B 被忽略，回退 first foreground A，A 获得 id1 |
| 空白 | 回退 A，A 获得 id1 |
| B 支持 split、但随后 paused/channel/responsive 失败 | **不再回退 A**，本帧失败 |

后续 `POINTER_DOWN` 即便穿过一条新 portal 路径，也只会把 portal handle 加到 state；`newGestureMonitors` 的收集条件仍是 `maskedAction==DOWN`，不会为这个第二指补锁该目的 display 的 gesture monitor。

### “mouse 永不 split”只在 Case 1 的能力裁决成立

`isFromMouse` 用精确的 `source == AINPUT_SOURCE_MOUSE` 判断。在上述 Case 1，只要是 mouse，新窗即使支持 split 也把局部 `isSplit` 设为 false。

但 r48 的 slippery MOVE 进入新窗时没有 mouse guard：

```cpp
if (newWindow->supportsSplitTouch()) {
    isSplit = true;
}
```

所以鼠标按下流经过 slippery 移交到支持 split 的窗口时，state/target 也可能带 `FLAG_SPLIT`，只是正常鼠标仍只有一枚 pointer，通常不会真的裁成多窗口子集。准确结论是：

> Case 1 禁止 exact-mouse 开启 split；slippery 是 r48 例外。

---

## 10. TouchedWindow 保存所有权，InputTarget 才装本帧 connection 与坐标

### 同一 handle 再加入时只做 OR 与并集

`TouchState::addOrUpdateWindow()` 以 `windowHandle` 对象相等查找：

```text
targetFlags |= newFlags
pointerIds  |= newPointerIds
```

特殊规则只有一条：新 flags 含 `SLIPPERY_EXIT` 时清掉旧 `DISPATCH_AS_IS`，防止旧窗同时收到原样 `MOVE` 与转换后的 `CANCEL`。

这意味着 target flags 是累计快照。普通 MOVE 不会因窗口的 Region、Z-order 或遮挡变化重新构建它们。

### 输出 InputTarget 时按 connection token 合并

成功路径遍历 `temp.windows`，调用：

```cpp
addWindowTargetLocked(windowHandle, targetFlags, pointerIds, inputTargets)
```

`InputTarget` 的查重键不是 handle，而是该窗口 token 对应的 connection token。同一 connection 若由多个 window handle 贡献 pointer，可在一个 target 里为不同 id 保存不同 `PointerInfo`；代码同时断言这些贡献的 target flags 与 global scale 一致。

没有注册 channel 时，这一步只记录 warning 并返回。`TouchState` 里的窗口不会在这里被移除，函数的 `injectionResult` 也不会因此回滚。

### 每个 pointer info 的局部坐标参数

窗口 target 每帧从当前 `InputWindowInfo` 读取：

```text
xOffset = -frameLeft
yOffset = -frameTop
windowXScale
windowYScale
globalScaleFactor
```

非 split target 的空 pointerIds 走 default info。split target 则按 pointer id 保存 info。

若同一 connection 内不同 pointers 的 info 不同，`createDispatchEntry()`：

1. 选 target 中最低 marked id 的 info 作为统一基准；
2. 每指先加自己的 offset；
3. 按自己的 window scale 与基准 scale 之比换算；
4. 再减基准 offset；
5. 最终让 `DispatchEntry` 携带基准 offset/scale 发布。

这一步是“把一个 connection 的所有 pointer 坐标归一到一套 publish 参数”，不是重新判断 pointer 属于哪个窗口。

### globalScale 与 windowScale 也不是同一层

publish 前：

- `globalScaleFactor` 会直接缩放 `PointerCoords` 中的 TOUCH_MAJOR/MINOR 与 TOOL_MAJOR/MINOR；它不缩放已归一化的 PRESSURE、SIZE 或 ORIENTATION；
- `windowXScale/windowYScale` 与 frame offset 作为发布参数传给客户端坐标转换；
- `FLAG_ZERO_COORDS` 会绕过这些 pointer transform 并清空坐标 axes。

第 173 章的窗口快照决定这些值从哪里来；本章只强调：`TouchState` 锁的是目标身份，`InputTarget` 每帧重取坐标参数。

---

## 11. 每个 connection 是否真的 split，要先过一个“数量捷径”

### FLAG_SPLIT 不等于必调 splitMotionEvent

`prepareDispatchCycleLocked()` 的条件是：

```cpp
if (target has FLAG_SPLIT) {
    if (target.pointerIds.count() != original.pointerCount) {
        splitMotionEvent(...);
    }
}
```

当数量相等时，代码不调用 `splitMotionEvent()`，而把当前 entry 继续交给通用入队链：

- 不创建“pointer 子集”副本；
- 不因 split 重写 action 或 event id；
- 不检查具体 pointer id 集合是否相同；
- 但 `createDispatchEntry()` 仍可能为 per-pointer 坐标归一构造一份 `combinedMotionEntry`，其 pointer 数量、action 与 id 保持不变。

因此“FLAG_SPLIT 会校验每帧 id 完整性”是错误结论。只有数量不同、真正进入 `splitMotionEvent()` 时，才执行子集提取与缺失检测。

### 真正 split 时按 id 提取

函数遍历原 pointer 数组：

```text
若 target.pointerIds 含该 pointerProperties.id
  → 复制 properties 与 coords 到子数组
```

若最终复制数量不等于 target bitset 的 count，说明预期 id 缺失；函数返回 null，**只跳过这个 connection 的本次 split 事件**。目标查找阶段已经可能报告成功，其他 connection 的投影也不会因此自动回滚。

### POINTER action 的改写表

当原 action 是 `POINTER_DOWN` 或 `POINTER_UP`：

| 变化 pointer 是否属于目标 | 目标子集大小 | 目标 action |
|---|---:|---|
| 否 | 任意 | `MOVE` |
| 是 | 1 | `DOWN` 或 `UP` |
| 是 | 大于 1 | 保留 `POINTER_DOWN/UP`，重算子集 action index |

其他原 action 保持不变。

真正创建 split 副本时：

- 生成新的 event id；
- 保留 eventTime、device/source/display、downTime、flags 等；
- 共享并增加原 `InjectionState` 引用；
- 当前 `MotionEntry` 没有 history 数组；第 187 章再追客户端 batching。

### 两窗两指逐帧账

设 A、B 都支持 split：

| 物理帧 | A 的 bitset / action | B 的 bitset / action |
|---|---|---|
| id0 DOWN@A | `{0} / DOWN`，数量相等故不裁 pointer 子集；坐标归一仍可建同 ID combined entry | 无 |
| id1 POINTER_DOWN@B | `{0} / MOVE` | `{1} / DOWN` |
| 两指 MOVE | `{0} / MOVE` | `{1} / MOVE` |
| id1 POINTER_UP | `{0} / MOVE` | `{1} / UP` |
| id0 UP | `{0} / UP` | 已从 state 移除 |

wallpaper 没有 `FLAG_SPLIT` 且 pointerIds 为空，所以同一手势里它看到的是完整原事件：第二帧仍是含 id0、id1 的 `POINTER_DOWN`，不是 A/B 的局部 action。

---

## 12. 先用旧所有权生成终帧，再清 pointer 与临时 dispatch mode

### POINTER_UP 的清理晚于 target 输出

成功路径先把当前 `tempTouchState.windows` 全部转成 `InputTarget`，然后尾部才处理 `POINTER_UP`：

1. 用 action index 找变化 pointer id；
2. 只遍历带 `FLAG_SPLIT` 的窗口；
3. 从各自 bitset 清掉该 id；
4. bitset 为空就删除该 `TouchedWindow`。

所以最后拥有该 pointer 的窗口能先收到 `UP`。若提前清 state，它反而会失去终止事件。

非 split 目标 pointerIds 本来为空，不在这个循环中逐指删；完整 `UP/CANCEL` 才 reset 整个 state。

### UP 与 CANCEL 也不是无条件清 map

masked action 为 `UP/CANCEL` 时，允许走到提交尾部才：

```text
temp.reset()
displayId = NONE
erase mTouchStatesByDisplay[entry.displayId]
```

但这段整体受两道外门保护：

- injection permission 必须 granted；
- `wrongDevice` 必须为 false。

因此不能把“收到任何一个名为 CANCEL 的 entry 就必然清 TouchState”当作独立于权限和设备冲突的规则。

### dispatch mode 在 connection 层才变 action

`enqueueDispatchEntriesLocked()` 按固定顺序尝试六种 mode：

```text
HOVER_EXIT
OUTSIDE
HOVER_ENTER
AS_IS
SLIPPERY_EXIT
SLIPPERY_ENTER
```

`enqueueDispatchEntryLocked()` 再解析：

| mode | resolved action |
|---|---|
| OUTSIDE | `ACTION_OUTSIDE` |
| HOVER_EXIT | `ACTION_HOVER_EXIT` |
| HOVER_ENTER | `ACTION_HOVER_ENTER` |
| SLIPPERY_EXIT | `ACTION_CANCEL` |
| SLIPPERY_ENTER | `ACTION_DOWN` |
| AS_IS | 原 action |

dispatch-mode 转换会取得 Dispatcher 新 id；AS_IS 保留它收到的 `MotionEntry.id`。若此前已经生成 split 副本，这个 entry 本身已有新 id，不能再说它等于最初的物理事件 id。然后 `Connection::InputState` 检查这条局部 action 是否与该 connection 先前收到的状态一致，不一致仍可能在此处单独跳过。

### cancellation 账与 TouchState 账要分开

App 收到合成 `CANCEL`，只证明某个 `Connection::InputState` 的 motion memento 被结束；它不自动证明 `mTouchStatesByDisplay` 已 erase。

反方向也一样：`TouchState` 删除一扇窗，仍可能需要先依据 connection memento 合成取消。第 189 章会专门拆这两本账，本章只保留这个边界。

---

## 13. slippery 不是“越界就转移”，而是满足五个条件才 CANCEL → DOWN

### state 先要求恰好一个 slippery foreground

`TouchState::isSlippery()` 顺序扫描 windows：

- 必须恰好遇到一个 `FOREGROUND`；
- 该窗口当前 `layoutParamsFlags` 必须含 `FLAG_SLIPPERY`；
- 第二个 foreground 出现就返回 false。

调用处还要求：

```text
maskedAction == MOVE
pointerCount == 1
```

所以多指不 slippery；split 手势只剩一指且其余 foreground 已移除后，才可能重新满足。

### 旧窗仍 modal，根本不会“滑出”

slippery MOVE 会重新调用 hit-test。但 hit-test 仍遵循 modal：

- 旧窗仍是前到后的首个 touch-modal 窗时，即使点已离开它的 Region/frame，也会再次命中旧窗；
- `oldWindow == newWindow`，不转移。

要让 `FLAG_SLIPPERY` 真正生效，上游窗口组合还必须允许 hit-test 找到另一扇窗。

### 只有 old、new 都存在且不同才转移

| 重新命中结果 | 行为 |
|---|---|
| 还是旧窗 | 旧窗继续收 AS_IS MOVE |
| 空白，没有新窗 | **不 CANCEL**，旧窗继续收 AS_IS MOVE |
| 新窗存在且不同 | 旧窗 SLIPPERY_EXIT，新窗 SLIPPERY_ENTER |

下游把同一原始 MOVE 变成：

```text
旧 connection: CANCEL
新 connection: DOWN
```

它不是旧窗 `UP`。新 `DOWN` 的 `downTime` 仍来自原 `MotionEntry` 的物理手势 downTime，并没有在 slippery 转移处改成当前 eventTime。

### slippery 新窗绕过了 Case 1 的多项规则

这条路径：

- 固定用 `pointerCoords[0]`，exact mouse 也不改用 cursor position；
- 不检查新窗 paused、connection、responsive；
- hit-test 可以递归 portal，但调用默认 `addPortalWindows=false`，不记录新 portal；
- 不新增 OUTSIDE，不重新收集 gesture monitors 或 wallpaper；从 old state 复制来的既有 monitor、wallpaper 仍参与本帧并继续保留；
- 只计算 point-obscured，不计算 partial；
- 新窗支持 split 时可把 false 改 true；不支持时**不会**把已有 true 清 false；
- 没有 mouse split guard。

这些差异不绕过后面的共享检查：所有 foreground 仍要通过注入 permission，且最终必须存在 foreground 或已锁的 gesture monitor。旧窗加入 `SLIPPERY_EXIT` 时 `addOrUpdateWindow()` 清其 AS_IS；新窗以 `FOREGROUND|SLIPPERY_ENTER` 加入。它们先成为本帧 targets，随后 filter 删除旧 foreground、把新窗 mode 归一为 AS_IS；下一帧 foreground 只向新窗延续，既有 wallpaper 与 gesture monitors 不受这句话排除。

---

## 14. hover、scroll 与 portal monitor 各有一套临时生命周期

### hover 每帧 hit-test，却不用 TouchState.windows 记住目标

`HOVER_ENTER/MOVE/EXIT` 都被列为 newGesture，先 reset temp 并重新命中。目标变化时：

- 旧 `mLastHoverWindowHandle` 加 `HOVER_EXIT`；
- 新窗先已有 `FOREGROUND|AS_IS`，再 OR `HOVER_ENTER`。

若原事件是 `HOVER_MOVE` 且换窗，新窗的同一 connection 会按固定 mode 顺序先排 enter、再排 AS_IS move；旧窗则只有 exit。六种 mode 的固定顺序只约束单个 target 的投影，不能据此宣称两个不同 channel 上 exit 与 enter 的 App 可见先后。即使只有孤立 AS_IS `HOVER_MOVE`，`Connection::InputState` 发现尚未 hovering 时也会把它改成 `HOVER_ENTER`。

尾部又 reset temp；`HOVER_ENTER/MOVE` 只把 device/source/display 身份写回，`HOVER_EXIT` 通常 erase display state。真正记住目标的是单一成员 `mLastHoverWindowHandle`。

它不是按 display 的 map。窗口列表更新只要在所更新 display 中找不到该 handle，就可清掉这一个全局 hover handle；不要从 `mTouchStatesByDisplay` 的多显示隔离能力推导出多 hover 隔离。

### SCROLL 目标是一次性的

`SCROLL` 在通过前置设备冲突门后会 reset temp、重新 hit-test，但尾部明确不保存本次 temp。若旧 state 正在 down 且这笔 scroll 换了设备或 source，它会在 reset 前以 `wrongDevice` 失败。找到有效窗口时，代码把 `newHoverWindowHandle` 设回旧值，以保持 hover 归属。

这个保持不是无条件的：若 Case 1 失败，`newHoverWindowHandle` 仍可能是 null；permission 允许走尾部时，统一赋值语句可清掉旧 hover handle。SCROLL 也不收 wallpaper 或 gesture monitor。

### gesture monitor 与 global monitor 的时间点不同

| 接收者 | 加入时机 | 能否救无窗口 DOWN | 是否进入 TouchState |
|---|---|---:|---:|
| gesture monitor | 仅首 `ACTION_DOWN`，origin + 当时 portal destinations | 是 | 是 |
| global monitor | target 查找成功后，每帧追加 | 否 | 否 |
| WATCH_OUTSIDE 窗 | 首 DOWN 的窗口栈扫描中 | 否；但可搭 monitor-only 成功 | 临时加入 windows |

portal gesture monitor offset 是对应 portal window 的 `-frameLeft/-frameTop`。多层 portal 的循环按每个 handle 分别添加，并没有在这段代码里把 offsets 累加成一条总变换。

### portal 终帧有一个顺序陷阱

初始 DOWN 的 portal path 写入 origin display 的 `TouchState`。成功返回后，外层每帧：

1. 先给 entry/origin display 加 global monitors；
2. 再读取已提交 state.portalWindows；
3. 给各 `portalToDisplayId` 加 global monitors。

但 `UP/CANCEL` 已在 `findTouchedWindowTargetsLocked()` 尾部 reset 并 erase state，外层到第 2 步时已找不到 portal path。因此：

- 锁入 `TouchState.gestureMonitors` 的 portal monitor 已先成为本帧 target，仍可收终帧；
- 只靠这条 portal-state 追加的 destination global monitor，不会由该逻辑收到终帧；
- origin display 的 global monitor 仍按第 1 步追加。

这不是“所有 monitor 都复制整条流”的统一模型。

---

## 15. “目标粘住”不等于窗口属性冻结；动态更新与取消各改不同账

### 同 id+token 的窗口更新会复用 handle

`updateWindowHandlesForDisplayLocked()` 为旧列表按 window id 建表。新快照若：

```text
id 相同 && token 相同
```

就复用旧 `InputWindowHandle` 对象，并用 `updateFrom()` 覆盖其 `InputWindowInfo`。这是因为 `TouchState` 多处按 handle 指针比较，存量路由需要保持对象身份。

于是一次普通 MOVE 中：

| 内容 | 是否沿用 |
|---|---|
| TouchedWindow 成员与 pointerIds | 沿用，不因新 Z/Region 自动重命中 |
| targetFlags 中 obscured/partial/split | 沿用命中/加入时快照 |
| frame、windowScale、globalScale | `addWindowTargetLocked()` 每帧从更新后的 info 重取 |
| `FLAG_SLIPPERY` | `isSlippery()` 每次读当前 info |

所以窗口移动后，触摸所有权可仍属原窗，而 App 局部坐标变换已经采用新 frame/scale。

### 窗口真正移除时只检查同 display key 的 TouchState

`setInputWindowsLocked()` 更新列表后只查 `mTouchStatesByDisplay[displayId]`。若这个 state 中的某个 touched handle 已不在全局窗口表：

1. 若 channel 还在，给它合成 pointer CANCEL；
2. 从 `state.windows` erase 该项。

portal 会打破“目标 display 更新即可清理”的直觉：事件以 origin display 为 key 保存 `TouchState`，其中的 foreground 却可以是递归命中的 destination handle。若 destination 窗口被移除，destination 的 `setInputWindowsLocked()` 只检查 destination-keyed state，可能看不到这条 origin-keyed 路由；它会暂时漏掉 CANCEL 与 erase，直到 origin display 后续也更新窗口列表并检查自己的 state，或另一路径清账。

它不会因此自动：

- 把 `state.down` 设 false；
- 把粘滞 `state.split` 降回 false；
- 清 `portalWindows`；
- 清 `gestureMonitors`；
- 让幸存 pointers 按新 Z-order/Region 重命中。

若移除的是唯一 foreground，state 可暂时剩下 `down=true` 但无 foreground；只有在 state 也没有已锁 gesture monitor 时，后续普通 MOVE 才会在“至少一个 foreground 或 gesture monitor”门失败。已有 gesture monitor 则仍可单独维持成功路由。

### 还有三种显式改路由入口

普通输入帧里 slippery 不是唯一会改 target 的机制：

| 入口 | 对路由的大意 | 后续章节 |
|---|---|---|
| `transferTouchFocus(from,to)` | state 从旧窗移到新窗；两端 connection 都存在时，旧端 CANCEL、新端合成 DOWN 序列 | 第 189—190 章 |
| gesture monitor `pilferPointers()` | CANCEL 当前 touched windows，再 `filterNonMonitors()`；保留 envelope 与 gesture monitors | 第 191 章 |
| `resetAndDropEverythingLocked()` | 全连接 CANCEL，并清整个 touch-state map 与 hover handle | 第 178、189 章 |

`filterNonMonitors()` 只清 windows 与 portal，不 reset down/split/device/source/display，也不清 gestureMonitors。pilfer 表示抢走当前 App routes，不等于把手势 envelope 重置。

### DeviceReset 的 CANCEL 不单独清 TouchState

`dispatchDeviceResetLocked()` 只按 deviceId 给所有 connections 合成 `CANCEL_ALL_EVENTS`。它没有修改 `mTouchStatesByDisplay`。

因此：

> “App 因 DeviceReset 收到 CANCEL”不能单独证明 Dispatcher 的 display 级 route state 已清。

在 r48 触摸 reset 主链里，`InputDevice::reset()` 先调用 mapper reset 清内部状态，再发 `NotifyDeviceReset`；`TouchInputMapper::reset()` 本身不另发 Motion CANCEL，App 侧取消正是 Dispatcher 的这段 connection 合成逻辑。也正因为如此，不能再凭空假定另有一枚普通 ACTION_CANCEL 会替它清掉 TouchState map。

### dumpsys 能看到什么

`dumpsys input` 的 Dispatcher 段会打印：

- `TouchStatesByDisplay`：down、split、deviceId、source；
- 每个 touched window 的 name、pointerIds、十六进制 targetFlags；
- portal window 名称；
- 各 display 当前窗口顺序、portalToDisplayId、paused/focus/wallpaper/visible；
- layout flags/type、frame、scale、touchableRegion、ownerPid/Uid；
- 注册的 global/gesture monitor 列表。

但它不直接打印：

- state 内究竟锁住了哪组 `gestureMonitors`；
- `mLastHoverWindowHandle`；
- target flags 的文字解码；
- 一次失败在 filter 前留下 OUTSIDE 的历史原因。

常用 flag 十六进制可从 `InputTarget.h` 对照：

| flag | 值 |
|---|---:|
| FOREGROUND / OBSCURED / SPLIT / ZERO_COORDS | `0x1 / 0x2 / 0x4 / 0x8` |
| AS_IS / OUTSIDE / HOVER_ENTER / HOVER_EXIT | `0x100 / 0x200 / 0x400 / 0x800` |
| SLIPPERY_EXIT / SLIPPERY_ENTER / PARTIAL | `0x1000 / 0x2000 / 0x4000` |

排障最小证据集应同时包含窗口栈、当前 TouchState、原 Motion action/pointer ids、目标 connection 状态和 App 实际 action；只截一张窗口布局图无法复原路由。

---

## 16. 用九个只读练习把一帧路由完整复算出来

以下命令均在 Android 11 r48 源码根目录执行。每题都要求留下状态表或判断矩阵，不要求 macOS 上编译 AOSP。

### 练习一：做出 hit-test 真值表

```bash
sed -n '802,847p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '31,66p' \
  frameworks/native/libs/input/InputWindow.cpp
sed -n '247,260p' \
  frameworks/native/libs/ui/Region.cpp
```

至少推演：invisible、NOT_TOUCHABLE+WATCH、touchable modal+空 Region、NOT_FOCUSABLE+命中 Region、NOT_TOUCH_MODAL+右边界点五例。产物中明确区分 frame、Region 与向零截断后的整数点。

### 练习二：重建 action 分类与提交矩阵

```bash
sed -n '1585,1638p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '1911,1996p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

分别写出 `DOWN`、`SCROLL`、`HOVER_MOVE`、foreign `MOVE`、`POINTER_UP`、`CANCEL` 对 temp、map、hover handle 的影响。标出 permission 与 wrongDevice 两道外门。

### 练习三：证明 OUTSIDE 有 foreground 与 monitor-only 两条结果

```bash
sed -n '802,861p;1651,1706p;1819,1909p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2496,2532p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

画三层 WATCH 窗口栈，分别推演底层 foreground 成功、无 foreground 但有 gesture monitor、两者都没有。说明哪些 watcher 被输出、何时加 ZERO_COORDS、实际清了哪些 axis。

### 练习四：逐帧维护两窗 split bitset

```bash
sed -n '1637,1736p;1949,1989p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '43,77p;124,148p' \
  frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
```

按 id0 DOWN@A、id7 POINTER_DOWN@B、MOVE 数组重排、id7 POINTER_UP、id0 UP 五帧填写 temp.windows。故意使用 id7，避免把 action index 1 与 pointer id 1 混为一谈。

### 练习五：核对数量捷径与 action 改写

```bash
sed -n '2237,2263p;2298,2395p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2925,3017p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

列出 target count 等于/小于原 pointerCount 两条路径；再对“变化 id 属于目标且子集 1 指”“属于且多指”“不属于”计算 resolved action、event id 是否重建。

### 练习六：找出 slippery 的所有非通则

```bash
sed -n '134,148p' \
  frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
sed -n '1751,1793p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

分别推演旧窗 modal、移到空白、新窗 paused、新窗支持 split 的 mouse、历史 split=true 进入非 split 新窗。产物应说明为什么 `FLAG_SLIPPERY` 单独不足以保证转移。

### 练习七：跟踪 portal 与两类 monitor 的终帧

```bash
sed -n '802,861p;1278,1296p;1651,1659p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '1934,1994p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

画 origin→portal→destination。对 DOWN、第二指经新 portal、UP 三帧，分别写 gesture monitor、origin global monitor、destination global monitor 是否加入以及 offset 来源。

### 练习八：证明“粘住 identity，不冻结 geometry”

```bash
sed -n '261,327p;1999,2030p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '3614,3670p;3756,3779p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

假设窗口 id/token 不变，frameLeft 从 100 移到 140。写出 TouchState 哪些字段不变、下一帧 InputTarget 的 xOffset 怎样变；再推演 token 改变导致旧 handle 被移除的取消路径。

### 练习九：对齐 CANCEL、route state 与 dumpsys

```bash
sed -n '1065,1075p;4026,4043p;4092,4178p;4429,4475p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '105,123p' \
  frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
```

做一张四行表：raw ACTION_CANCEL、DeviceReset、resetAndDropEverything、pilferPointers。分别写 connection memento、state.windows、down/split、portal、gestureMonitors 是否清除，并标出 dumpsys 能直接证明哪些结论。

### 检查题

1. 为什么 touchableRegion 不含点，touch-modal 窗仍可能成为 foreground？
2. 为什么“鼠标永不 split”不适用于 r48 slippery 路径？
3. split POINTER_DOWN 命中 paused 新窗后，为什么不会再 fallback 旧 foreground？
4. target 带 FLAG_SPLIT 时，什么情况下仍原样发送原 MotionEntry？
5. 跨 UID OUTSIDE 为什么通常清全部 PointerCoords，却在 monitor-only 边路没有这一步？
6. POINTER_UP 为什么必须先生成 targets，再从 state 清变化 id？
7. 窗口移动后不重新 hit-test，App 局部坐标为什么仍可能改变？
8. DeviceReset 已让 App 收到 CANCEL，为什么还不能推出 TouchState map 已清？
9. destination global monitor 为什么可能收不到 portal 流的终帧？

### 下一章

第 187 章进入输入事件批处理与触摸重采样：从独立 MotionEntry、InputChannel 消息、InputConsumer 的兼容条件、history、Choreographer 输入阶段与 resampling，追一帧 UI 怎样消费多次硬件采样。
