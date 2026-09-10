# 189 Android InputState 与取消事件合成：Dispatcher 怎样替每条连接收尾

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`
> 核心问题：窗口失焦、设备 reset、手势被 pilfer 或触摸焦点转移时，Dispatcher 怎样判断某条 `InputChannel` 还欠哪些 `UP`、`CANCEL`、`HOVER_EXIT` 或补发的 `DOWN`？这些合成事件已经入队，是否就等于 App 一定收到了？

---

## 1. 从“App 一直以为还按着”开始

### 物理输入停了，接收端状态未必结束

假设 App 已收到触屏 `DOWN` 和若干 `MOVE`，随后窗口被移除。后续硬件 `UP` 即使仍由 InputReader 产生，也不会再按原路投给这个窗口。若系统什么都不做，App 侧的 View、手势识别器或游戏逻辑就可能永远认为手指还按着。

Key 也一样：旧焦点窗口看过 `KEY_DOWN`，焦点切走后，真实 `KEY_UP` 可能送到另一个窗口。旧窗口需要一条人为合成、带 `CANCELED` 的 `KEY_UP` 才能收账。

因此取消事件不是“把某个队列项删掉”的同义词。它是 Dispatcher 根据此前为某个 connection 保存的状态，新建一条正向输入事件，尝试通过原来的 `InputChannel` 告诉接收端：

> 你此前知道的这条活动输入流现在作废，请立即回到结束状态。

### 本章最重要的边界

必须同时记住两句话：

1. `Connection::inputState` 是 Dispatcher 对某条 connection **提前登记的接收者语义**；
2. 合成取消只是一次新的传输尝试，**不是 App 已收到取消的证明**。

之所以说“提前登记”，是因为 r48 在创建 `DispatchEntry` 后先调用 `trackKey()` 或 `trackMotion()`，随后才把 entry 放入 `outboundQueue`，再尝试 publish。状态可能领先于 socket，更可能领先于 App 的 Java 回调。

### 一个最小失败时间线

```text
原 Motion DOWN
  -> InputState 建立 MotionMemento
  -> outboundQueue
  -> publish 成功
  -> App 收到 DOWN

窗口被移除
  -> 根据 MotionMemento 新建 ACTION_CANCEL
  -> CANCEL 再次 track，先删除 MotionMemento
  -> CANCEL 排到 outboundQueue
  -> publish 发生硬错误
  -> connection BROKEN，App 没收到 CANCEL
```

终点是：

```text
Dispatcher 的 InputState：已无该 motion memento
App 的手势状态：仍可能停在 DOWN
```

这不是矛盾，而是两个进程之间没有事务提交。`InputState` 帮 Dispatcher 安排自洽序列，但不能把一次 socket 写入变成可靠交付协议。

### 本章要回答的五个问题

- Key 和 Motion 分别按什么身份找到旧状态？
- `CancelationOptions` 怎样筛选要结束的流？
- 合成事件保留哪些字段，又主动丢掉哪些字段？
- pilfer 与 transfer 为什么一个只需取消旧目标，另一个还要给新目标补 `DOWN`？
- r48 的普通 policy fallback 为什么没有真正进入预期的 `InputState` 账本？

源码主锚点：`InputState.h:27-128`、`InputState.cpp:27-446`、`CancelationOptions.h:24-48`、`InputDispatcher.cpp:2209-2425,2456-2613,2765-2922`。

## 2. 两本账：TouchState 决定路由，InputState 记 connection 语义

### 两者回答不同问题

| 状态 | 粒度 | 主要内容 | 回答的问题 |
|---|---|---|---|
| `mTouchStatesByDisplay` / `TouchState` | 每个 display 的当前触摸路由 | down、device/source、普通窗口、pointerIds、portal、gesture monitors | 下一条 pointer Motion 应送给谁？ |
| `Connection::inputState` / `InputState` | 每条已注册 connection | KeyMemento、MotionMemento、fallback map | 这条接收线路按 Dispatcher 的安排还处于哪些活动流？ |

前者属于路由平面，后者属于每接收线路的序列账本。一个 split gesture 可以让同一 display 的 `TouchState` 同时保存窗口 A 的 pointer id 0 和窗口 B 的 pointer id 1，而 A、B 各自的 `InputState` 只保存为该 connection 切分后的 pointer 数组。

### “接收者视角”不等于“实际已经收到”

旧表述常把 `InputState` 说成“App 已收到的事件状态”。源码时序不支持这么强的结论：

```text
create DispatchEntry
  -> 解析 resolvedAction / resolvedFlags
  -> connection.inputState.track...
  -> outboundQueue.push_back
  -> startDispatchCycle
  -> publish
```

所以更精确的定义是：

> `InputState` 记录 Dispatcher 已经接受、准备按该 connection 语义投递的活动状态。

如果 publish 因正常背压返回 `WOULD_BLOCK` 且已有 wait 项，entry 留在 outbound，账本仍领先于传输；若 publish 发生不可恢复错误，connection 被打成 `BROKEN` 并清发送队列，tracking 也不会回滚。

### InputState 不是这些东西

一条 memento 不是：

- EventHub 的物理按键或触点真相；
- `TouchState` 中的窗口命中记录；
- outbound/wait 中的 `DispatchEntry`；
- transport seq、ANR deadline 或 `handled` 状态；
- App 进程的 `MotionEvent`、`KeyEvent` 或 View 手势状态；
- 所有历史 `MOVE` 的轨迹缓存。

它没有 transport seq，也没有“已 publish”“已 finish”位。不能拿 memento 数量推导 socket 队列长度，更不能用 `isNeutral()` 证明 App 已经中性。

### 三类存储

`InputState` 内有三只容器：

| 容器 | 元素 | 用途 |
|---|---|---|
| `mKeyMementos` | `KeyMemento` | 保存仍需结束的 Key DOWN 状态 |
| `mMotionMementos` | `MotionMemento` | 保存非 hover 活动流、hover 流或 joystick 非中性状态 |
| `mFallbackKeys` | original keyCode → fallback keyCode | 记住 policy 对原键选择的 fallback 映射 |

`isNeutral()` 只检查前两个 vector 是否为空，不检查 fallback map。并且 r48 Dispatcher 生产代码没有调用 `isNeutral()`；它更像一个可供使用的状态查询，不是当前调度流程的门。

### clear 的真实边界

`clear()` 的实现只是：

```cpp
mKeyMementos.clear();
mMotionMementos.clear();
mFallbackKeys.clear();
```

它不合成事件、不 publish，也不等待 App 确认。更要紧的是，r48 的 production Dispatcher 中找不到对它的显式调用；不能说连接销毁或 broken 流程“会调用 `clear()`”。连接对象最终析构时，其成员容器自然销毁，是另一回事。

源码锚点：`Connection.h:35-69`、`InputState.h:79-127`、`InputState.cpp:27-39,353-357`、`TouchState.h:29-55`。

## 3. tracking 发生在 publish 之前

### 先处理 split 和 dispatch mode

`prepareDispatchCycleLocked()` 先确认 connection 是 `NORMAL`。Motion target 若带 `FLAG_SPLIT` 且只拥有部分 pointerIds，还会先构造新的 split `MotionEntry`。接下来 `enqueueDispatchEntriesLocked()` 按 target 上的 dispatch mode 依次尝试创建 entry。

因此某 connection 保存的 pointer 快照可以是为它切分后的数组，不一定等于原始 `MotionEntry` 的全部 pointers。

### resolvedAction 才是跟踪动作

`enqueueDispatchEntryLocked()` 创建 `DispatchEntry` 后，会解析该目标实际要看到的动作。例如同一原始 Motion 可被转换成：

- `OUTSIDE`；
- `HOVER_EXIT`；
- `HOVER_ENTER`；
- 原动作 `AS_IS`；
- slippery exit 对应的 `CANCEL`；
- slippery enter 对应的 `DOWN`。

`trackMotion()` 接收的是 `dispatchEntry->resolvedAction` 与 `resolvedFlags`，不是盲目照抄原始 action。首个 `HOVER_MOVE` 若该 connection 尚无 hover memento，还会先被改成 `HOVER_ENTER`。

### 真正的入队顺序

Key/Motion 的关键顺序是：

```text
createDispatchEntry
  -> 填 resolvedEventId / resolvedAction / resolvedFlags
  -> inputState.trackKey 或 trackMotion
  -> 若 false：直接跳过本次 DispatchEntry
  -> foreground injection pending++
  -> outboundQueue.push_back
```

`InputState.h` 的注释写着事件“just been published”，但实现明显是在 publish 前。读这类状态机应以调用位置为准，而不是把注释当作时序保证。

### false 表示这条目标投递被跳过

`trackMotion()` 判断不一致并返回 false 时：

- 该 `DispatchEntry` 不进入 outbound；
- 不增加 foreground pending；
- 不会为它分配一轮 publish/wait；
- 也不会自动为 App 修复此前状态。

而 Motion 的 injection result 在目标查找结束后、真正逐 connection enqueue 前已经可被设为成功。极端情况下，路由层认为注入成功，但某目标的 entry 因 connection 本地状态不一致而被跳过；“injection succeeded”不能替代逐 connection 的传输证明。

`trackKey()` 在 r48 的实际各分支都返回 true，所以“Key 因 trackKey false 被跳过”只是接口允许的概念，不是本版本会发生的行为。

### publisher 才做坐标变换

`MotionMemento` 保存的是传给 tracking 的 `MotionEntry` pointer 数组；窗口 offset、window scale、global scale 与 `FLAG_ZERO_COORDS` 等目标变换主要在 publish 阶段应用。

因此 memento 不是 App 最终看到的坐标字节副本。后来合成取消时，Dispatcher 会重新为当前窗口构造 target 变换；窗口位置或 scale 已改变时，合成包的坐标映射也可能与最初的包不同。

### hard failure 不回滚 InputState

publish 成功后，entry 才从 outbound 迁到 wait。不可恢复错误或“wait 为空却 `WOULD_BLOCK`”会调用 broken abort，drain connection 的 outbound/wait 并置 `BROKEN`。这个路径不调用 `InputState::clear()`，也不逆向恢复此前的 tracking。

所以排障时要允许这种组合：

```text
status = BROKEN
outboundQueue = empty
waitQueue = empty
InputState = 仍可能有 memento
```

源码锚点：`InputDispatcher.cpp:2209-2263,2265-2425,2456-2613,2655-2678`。

## 4. KeyMemento：宽松的 Key 生命周期

### 唯一匹配键有五个字段

`findKeyMemento()` 同时比较：

```text
(deviceId, source, displayId, keyCode, scanCode)
```

所以同一 keyCode 来自不同设备、source、display 或 scanCode 时，可以各有自己的 memento。metaState、flags、downTime 和 policyFlags 是要保存的状态，不参与查找身份。

### DOWN 是替换，不是报错

收到 resolved `KEY_DOWN` 时：

1. 查找同五元组的旧 memento；
2. 若存在，先删除；
3. 用当前 entry 与 resolved flags 新建一条。

重复 DOWN 因而刷新整条 KeyMemento，包括 downTime、metaState、flags 和 policyFlags。它不会因为“已有 DOWN”就拒绝派发。

### UP 找不到 DOWN 仍然放行

resolved `KEY_UP` 命中 memento 时会删掉它。找不到时，源码保留了一段 FIXME：按严格一致性本想 drop，但弹出的窗口可能在按键按住期间才出现，没有见过原 DOWN，却仍需要 UP 来关闭。

最终代码明确 `return true`。这说明 Key 一致性采用兼容性优先：

```text
有匹配 DOWN  -> 删除 memento，允许 UP
无匹配 DOWN  -> 不删任何状态，仍允许 UP
```

不要把 Motion 的严格收尾规则类推到 Key。

### 其他 Key action 也直接允许

除 DOWN、UP 外的 action 走 default，直接返回 true，也不修改 KeyMemento。综合所有分支，r48 的 `trackKey()` 实际不会返回 false。

### fallback UP 的反向清表

如果传入 `trackKey()` 的原始 `entry.flags` 带 `AKEY_EVENT_FLAG_FALLBACK`，且 action 是 UP，它会先遍历 `mFallbackKeys`，删除所有 value 等于当前 fallback keyCode 的映射：

```text
original A -> fallback X
original B -> fallback X
收到 fallback X 的 UP
=> 两条映射都可被移除
```

这里按 value 反查，不是按 original keyCode 删除。之后才按当前 fallback Key 的五元组查 KeyMemento。

但这段机制只有在 fallback UP 真正再次经过 `trackKey()` 时才生效。第 15 节会看到，r48 普通 policy fallback restart 恰好绕过了这条入口。

### KeyMemento 能重建什么

它保存 device/source/display、keyCode/scanCode/metaState、resolved flags、downTime 和 policyFlags。取消时可由这些字段生成带 `CANCELED` 的 UP；它不保存原始事件的 transport seq、handled 结果或完整重复历史。

源码锚点：`InputState.cpp:41-89,195-205,218-230`、`InputState.h:80-90`。

## 5. MotionMemento：身份、指针快照与严格收尾

### Motion 身份不是 pointerIds

`findMotionMemento()` 的匹配键是：

```text
(deviceId, source, displayId, hovering)
```

pointerId 集合不参与查找。同一 device/source/display 可以同时有一条非 hover memento 和一条 hover memento；普通非 hover 多指手势则共用一条 memento。

### DOWN 建立或重置非 hover 流

resolved `ACTION_DOWN` 查找 `hovering=false` 的 memento：

- 已存在：删除旧项；
- 然后：保存当前字段与整个 pointer 数组；
- 返回 true。

和 Key DOWN 一样，重复 Motion DOWN 是替换，不是拒绝。

### UP 与 CANCEL 必须有前态

resolved `ACTION_UP` 或 `ACTION_CANCEL` 必须命中非 hover memento：

```text
命中 -> 先删除 memento，再返回 true
未命中 -> 返回 false，DispatchEntry 被跳过
```

这里的“先删除”仍发生在 outbound 入队和 publish 之前。即使这次 UP/CANCEL 后来写 socket 失败，InputState 也已认为该流结束。

### MOVE 与 POINTER action 只更新数组

排除 navigation 与 joystick 特例后，`MOVE`、`POINTER_DOWN`、`POINTER_UP` 要求：

1. 找到非 hover memento；
2. `firstNewPointerIdx < 0`。

满足后调用 `MotionMemento::setPointers()`。这个函数只复制：

- `pointerCount`；
- 全部 `PointerProperties`；
- 全部 `PointerCoords`。

它不会刷新 flags、x/y precision、cursor position、downTime 或 policyFlags。这些固定字段仍来自最初建立该 memento 的 DOWN；只有 pointer 数组是最近一次被接受的数组。

因此“MotionMemento 保存最后一条 Motion 的所有字段”是错误表述。

### POINTER_UP 后仍可能暂存离去 pointer

Android 的 `ACTION_POINTER_UP` 事件本身仍携带即将离开的 pointer。`setPointers()` 原样保存整个数组，不主动删 action index 对应项。因此：

```text
POINTER_UP 已被 track
  -> memento 仍含该事件携带的全部 pointers
  -> 下一条 MOVE 通常才带收缩后的数组
```

若两者之间立即需要合成 CANCEL，CANCEL 可能仍包含刚刚在 POINTER_UP 中标记离开的 pointer。代码没有根据 action index 自己重算数组。

### 它不是完整的 pointer 序列校验器

默认分支没有检查：

- pointerId 是否重复；
- POINTER_DOWN/UP 的 action index 是否有效；
- pointerCount 应增加还是减少；
- 新旧 pointerId 集合是否连续；
- source 是否真的属于 POINTER class。

它主要检查“是否有对应流”与“是否尚有待补 DOWN 的 pointer”，再保存调用者给的数组。这里是局部账本，不是独立的输入验证防火墙。

源码锚点：`InputState.cpp:91-193,207-216,232-255`、`InputState.h:92-112`。

## 6. Navigation、Joystick 与 Hover 三条特殊路径

### navigation 的早退只覆盖三类 action

当 action 是 `MOVE`、`POINTER_DOWN` 或 `POINTER_UP`，且 source 带 `AINPUT_SOURCE_CLASS_NAVIGATION`，函数直接返回 true，不查找也不更新 MotionMemento。这样 trackball 一类相对运动不需要为了相对位移合成取消。

但不能概括成“navigation 永远不追踪”。分支顺序决定：

- navigation `DOWN` 仍走通用 DOWN，建立非 hover memento；
- navigation `UP/CANCEL` 仍走通用严格结束；
- navigation `HOVER_*` 仍走 hover 分支；
- 只有上述 MOVE/POINTER 三类 action 提前放行。

### joystick 的 neutral 是结构判定

同一组三类 action 中，navigation 早退之后才进入 joystick 逻辑。Joystick 允许没有 DOWN/UP 的连续 MOVE：

| 已有 memento | `pointerCoords[0].isEmpty()` | 结果 |
|---|---:|---|
| 是 | true | 删除 memento，视为回到 neutral |
| 是 | false | 只更新 pointer 数组 |
| 否 | true | 不建立状态 |
| 否 | false | 新建非 hover memento |

所有分支最终都返回 true。

这里的 `isEmpty()` 检查 `PointerCoords.bits` 是否为空，不是在运行时逐轴比较“所有数值是否等于 0”。`setAxisValue()` 对一个尚不存在且值为 0 的轴不会建立 bit，因此正常构造往往让“所有轴回零”表现为空；但语义依据仍是稀疏坐标结构，不应改写成浮点全零比较。

### joystick 更新也只刷新 pointers

已有 joystick memento 且非空时同样只调用 `setPointers()`。后来的 flags、precision、cursor、downTime 与 policyFlags 不会随 MOVE 更新。

### hover 有独立生命周期

`HOVER_ENTER` 和 `HOVER_MOVE` 会：

1. 查找同 device/source/display 的 `hovering=true` memento；
2. 删除旧项；
3. 完整新建一条 hover memento。

因此 hover 的每次 ENTER/MOVE 都会刷新全部固定字段与 pointer 快照，不同于普通手势 MOVE 只更新 pointers。

`HOVER_EXIT` 则要求已有 hover memento，找到后删除；找不到就返回 false。

### 缺失 ENTER 的修复在 Dispatcher，不在 InputState

如果某 connection 没有 hover 状态却准备接收 `HOVER_MOVE`，`enqueueDispatchEntryLocked()` 会先用 `isHovering()` 检查，再把 resolved action 改成 `HOVER_ENTER`。随后 `trackMotion()` 才按 ENTER 建立状态。

所以“InputState 自动把 MOVE 变 ENTER”不准确；动作改写属于 Dispatcher 创建投递的步骤。

### hover 窗口消失不必然走 removed-touched-window cancel

窗口列表刷新发现 `mLastHoverWindowHandle` 不再存在时，r48 直接把该 handle 清空。普通 hover 路由还会在触摸目标计算中 reset 临时 TouchState；因此不能用“移除 touched window”的 pointer cancel 循环证明 hover 一定收到 `HOVER_EXIT`。

`InputState` 确实能在 ALL/POINTER 等命中该 hover memento 时合成 `HOVER_EXIT`，但“有生成能力”与“这个特定窗口移除路径必然调用”是两件事。

源码锚点：`InputState.cpp:91-193`、`InputDispatcher.cpp:2362-2371,1719-1817,1934-1949,3706-3721`、`Input.h:315-334`、`Input.cpp:187-217`。

## 7. CancelationOptions：模式与过滤条件要分两层读

### 源码确实只拼一个 l

r48 类型名是 `CancelationOptions`，函数名是 `synthesizeCancelationEvents`。讨论业务概念时可以写“cancellation”，引用符号时应保留源码的既有拼写。

### 四种 mode 的选择矩阵

| mode | KeyMemento | pointer-class Motion | non-pointer Motion |
|---|---:|---:|---:|
| `CANCEL_ALL_EVENTS` | 是 | 是 | 是 |
| `CANCEL_POINTER_EVENTS` | 否 | 是 | 否 |
| `CANCEL_NON_POINTER_EVENTS` | 是 | 否 | 是 |
| `CANCEL_FALLBACK_EVENTS` | 仅 stored flags 带 `FALLBACK` | 否 | 否 |

Pointer 与 non-pointer 的区分只看：

```cpp
memento.source & AINPUT_SOURCE_CLASS_POINTER
```

它不看 event 名称、toolType 或当前 TouchState。Mouse、touchscreen、stylus 等只要 source class 是 pointer，就落入 pointer 侧；Key 永远属于 non-pointer/all/fallback 的 Key 选择逻辑。

### 三个 optional filter

`CancelationOptions` 还可以设置：

- `keyCode`；
- `deviceId`；
- `displayId`。

Key 的筛选顺序会检查三者；Motion 只检查 deviceId 与 displayId，因为 Motion 没有 keyCode。

### 最容易踩的 keyCode 陷阱

如果构造：

```text
mode = CANCEL_ALL_EVENTS
keyCode = X
deviceId/displayId 未设置
```

结果不是“所有事件都按 X 缩小”。真实含义是：

- Key：只取消 keyCode X；
- Motion：仍取消所有 device/display 匹配的 motion，因为 motion 选择函数根本不读 keyCode。

调用方若想只处理某个 Key，应该选与 Key 语义相符的 mode，而不能把 keyCode 当作跨事件类型的通用过滤器。

### reason 不进入事件

`reason` 是描述性 C 字符串。wrapper 在调试日志中打印它，合成的 `KeyEntry`/`MotionEntry` 不携带 reason，App 收到的 `KeyEvent`/`MotionEvent` 也无法从事件字段知道“因为窗口失焦”还是“因为设备 reset”。

### FALLBACK 看 memento flags，不查映射

`CANCEL_FALLBACK_EVENTS` 的 Key 判定只是：

```cpp
return memento.flags & AKEY_EVENT_FLAG_FALLBACK;
```

它不会遍历 original→fallback map，也不会因为 map 中存在某个 value 就把普通 KeyMemento 当作 fallback。这个细节正是第 15 节实现缺口的核心。

源码锚点：`CancelationOptions.h:24-48`、`InputState.cpp:402-446`。

## 8. 合成出的 Key UP、Motion CANCEL 与 HOVER_EXIT

### Key 取消的字段

每个命中的 KeyMemento 生成一条新的 `KeyEntry`：

| 字段 | 值 |
|---|---|
| id | `IdGenerator.nextId()` |
| eventTime | wrapper 传入的 currentTime |
| action | `AKEY_EVENT_ACTION_UP` |
| flags | memento flags `| AKEY_EVENT_FLAG_CANCELED` |
| repeatCount | 0 |
| downTime | 原 memento downTime |
| device/source/display | 原 memento |
| keyCode/scanCode/metaState | 原 memento |
| policyFlags | 原 memento |

因此它是“以当前时间结束原 DOWN 生命周期”的合成 UP，不是假装还原一条真实硬件 UP。

### Motion 根据 hovering 选动作

每个命中的 MotionMemento 只生成一条事件：

```text
hovering = false -> ACTION_CANCEL
hovering = true  -> ACTION_HOVER_EXIT
```

多指非 hover 手势不会逐 pointer 合成 `POINTER_UP`；同一 memento 的完整 pointer 数组一次性放进一条 `ACTION_CANCEL`，表示整条 gesture 立即失效。

### Motion 保留与重置的字段

保留：

- 新 id、当前 eventTime；
- memento 的 device/source/display/policyFlags；
- memento 的 flags；
- x/y precision、cursor position、downTime；
- pointerCount、properties 与 coords。

显式重置：

| 字段 | 合成值 |
|---|---|
| actionButton | 0 |
| metaState | `AMETA_NONE` |
| buttonState | 0 |
| classification | `NONE` |
| edgeFlags | `NONE` |
| xOffset/yOffset | 0 |

Motion 代码没有像 Key 那样额外 OR 一个 canceled flag；取消语义来自 `ACTION_CANCEL`，hover 收尾则来自 `HOVER_EXIT`。

### “最新坐标”要加限定

普通手势在每次被接受的 MOVE/POINTER action 上更新整个 pointer 数组，所以 CANCEL 通常携带最近一次 tracking 的 pointer 快照。但 flags、precision、cursor、downTime 与 policyFlags 仍可能来自 DOWN。

Joystick 同样只更新 pointer 数组；hover ENTER/MOVE 因为整条 memento 替换，才会刷新全部保存字段。

### 返回顺序是 Key 在前、Motion 在后

实现先遍历 `mKeyMementos`，再遍历 `mMotionMementos`。返回 vector 中所有合成 Key UP 在前，所有 Motion CANCEL/HOVER_EXIT 在后。每个 vector 内沿现有 memento 顺序。

这只是同一 connection 后续入队的构造顺序，不建立多 connection App 处理之间的全局顺序。

### synthesize 本身不 reset

`InputState.h` 注释称 `synthesizeCancelationEvents()` 会“resets the tracked state”，但 `InputState.cpp` 的实现只遍历并 new EventEntry，完全没有 erase 或 clear。

因此直接调用后：

```text
返回了 cancel EventEntry vector
InputState 原 mementos 仍然存在
```

正常 Dispatcher wrapper 会把这些合成事件再送进 `enqueueDispatchEntryLocked()`，由合成 Key UP、Motion CANCEL 或 HOVER_EXIT 的 tracking 间接删除对应 memento。状态重置发生在“重新入队”阶段，而不是 synthesize 函数内，更不是 App 确认之后。

源码锚点：`InputState.cpp:268-298`、`InputState.h:53-55`。

## 9. Dispatcher wrapper：取消入队与 App 收到之间没有事务

### helper 的四层入口

Dispatcher 为不同调用范围提供四层包装：

```text
ForAllConnections
  -> 遍历 mConnectionsByFd

ForMonitors
  -> 遍历所有 display 的 global monitors
  -> 遍历所有 display 的 gesture monitors

ForInputChannel
  -> 先按 channel token 找 Connection

ForConnection
  -> 真正 synthesize、enqueue、start cycle
```

`CancelationOptions` 决定“在每个 InputState 里选什么”；选择哪一层 wrapper 决定“扫描哪些 connections”。这两个维度必须分开。

### BROKEN 与 ZOMBIE 的细微区别

`synthesizeCancelationEventsForConnectionLocked()` 开头只显式排除：

```cpp
if (connection->status == Connection::STATUS_BROKEN) {
    return;
}
```

它没有同样排除 `STATUS_ZOMBIE`。通常 zombie 已从全局 connection 表移除，常规入口不会再找到它；但 policy 回调会临时解锁，调用方手里保留的 `sp<Connection>` 可能在重入后已变成 zombie。

此时 helper 仍可能生成并 enqueue 条目，而 `startDispatchCycleLocked()` 的 while 条件只允许 `STATUS_NORMAL`，所以不会 publish。不能把“非 BROKEN”简化为“必然可通信”。

这里还有一个资源边界：unregister 是先 drain 当时的 outbound/wait，再把 connection 置为 ZOMBIE；若 policy 重入后保留的 `sp<Connection>` 又经本 helper 追加 raw-pointer `DispatchEntry`，从这条 r48 调用链看已没有后续 start 或 drain。`Connection` 析构函数本身为空，因而该条目不只是永远无法送达，还存在随 retained zombie 被遗弃、引用未正常 release 的风险。

### target 不重新 hit-test

helper 不把合成事件丢回普通窗口目标查找。它直接建立只含当前 connection channel 的 `InputTarget`，并设置 `FLAG_DISPATCH_AS_IS`。

如果仍能通过 connection token 找到 window handle，就从**当前**窗口信息取：

- `-frameLeft / -frameTop`；
- `windowXScale / windowYScale`；
- `globalScaleFactor`。

找不到 window handle 时，保留 target 的默认 offset/scale。它不会因为窗口已经不在窗口表里就改投另一窗口。

### 一条取消的实际顺序

对每个生成的 EventEntry：

```text
synthesizeCancelationEvents
  -> enqueueDispatchEntryLocked
       -> create DispatchEntry
       -> 再次 track
       -> 删除对应 memento
       -> push outbound
  -> release 本地 EventEntry 引用

全部处理完
  -> startDispatchCycleLocked
  -> 尝试 publish
  -> 成功才移入 waitQueue
```

所以“synthesize 不 erase”并不等于正常调用后状态一直保留；normal wrapper 会通过回灌事件间接消账。但这个消账点仍早于 publish。

### 新 cancel 排在已有 outbound 后

helper 使用普通的 `outboundQueue.push_back`。如果 connection 之前已有发送积压，CANCEL 会排在它们后面；它不是高优先级控制消息，也不会抢占或删除先前 entry。

这使语义顺序在同一 connection 上仍然自洽：

```text
已排定 MOVE 1
已排定 MOVE 2
窗口移除后合成 CANCEL

outbound: MOVE 1 -> MOVE 2 -> CANCEL
```

但若前两条持续阻塞，App 也无法及时得到 CANCEL。

### publish、wait 与 FINISHED

只有 publish 成功，取消 entry 才从 outbound 进入 wait，并像普通 Key/Motion 一样等待 App 的 `FINISHED`。在 normal connection 与正常 memento 不变量下，wrapper 会同步把已生成的匹配 UP/CANCEL/HOVER_EXIT enqueue 到 outbound；真正的不确定边界从 outbound 以后开始：

- synthesize 后经 wrapper enqueue：表示 Dispatcher 已排定取消并提前消账；
- 入 outbound：不等于 publish 成功；
- publish 成功：不等于 Java 已处理；
- 进入 wait：不等于 App 已 finish。

取消的成功标准若是“App 已经真正结束手势”，仍要观察正向传输与完成回执，而不能只看 InputState 已无 memento。

### 失败不会回滚路由或 memento

很多调用方会先修改全局路由，或在 cancel enqueue 时提前删 memento。后续 publish 失败没有统一回滚：

- removed touched window 仍会从 TouchState 删除；
- pilfer 仍会清普通窗口路由；
- transfer 已经把路由从 source 改到 target；
- 合成取消的 tracking 已经删掉 source memento。

这种实现选择偏向让 Dispatcher 的未来决策前进，而不是提供跨进程 exactly-once 收尾。

源码锚点：`InputDispatcher.cpp:2456-2613,2765-2864,4379-4401`、`Connection.cpp:23-31`。

## 10. 全局取消调用点：一次 drop 可能结束许多无关流

### 先看调用点总表

| 场景 | wrapper 范围 | mode | 过滤条件 | 重要边界 |
|---|---|---|---|---|
| inbound Key 被 drop | 所有 connections | NON_POINTER | 无 | 清所有 Key 与非 pointer Motion，不只当前 Key |
| inbound pointer Motion 被 drop | 所有 connections | POINTER | 无 | 跨 device/display 的 pointer memento 都可能命中 |
| inbound non-pointer Motion 被 drop | 所有 connections | NON_POINTER | 无 | Key 也会一起取消 |
| DeviceResetEntry | 所有 connections | ALL | deviceId | 不按 display 限定 |
| Motion 目标查找/注入失败 | 所有 monitors | POINTER 或 NON_POINTER | 无 | permission denied 特例不取消 |
| conflicting pointer actions | 所有 connections | POINTER | 无 | 先 cancel，再 dispatch 当前 Motion |
| 单 connection ANR | 该 connection | ALL | 无 | 不 drain、不 break，且不保证 cancel 能越过积压 |
| disable/filter reset | 所有 connections | ALL | 无 | 后续清全局状态，但不 drain connection 队列 |

这张表最反直觉的地方是“触发事件的身份”通常没有自动写入 options。调用者不设置 deviceId/displayId，就会宽范围取消。

### dropInboundEventLocked 的范围

因 POLICY、DISABLED、APP_SWITCH、BLOCKED 或 STALE 被丢弃时：

- 当前是 Key：对所有 connections 做 NON_POINTER；
- 当前是 pointer-class Motion：对所有 connections 做 POINTER；
- 当前是 non-pointer Motion：对所有 connections 做 NON_POINTER。

options 不带当前 entry 的 deviceId、displayId 或 keyCode。于是丢一条 Key 不只是结束这一个 keyCode；它还会结束所有连接中的全部 Key 和非 pointer Motion。丢一条 pointer Motion 也可能取消其他设备、其他 display 的 pointer 流。

这是 r48 的实际止损粒度，不能按“当前事件”想当然缩小。

### Device reset 只加 deviceId

`dispatchDeviceResetLocked()` 使用 ALL，并设置：

```cpp
options.deviceId = entry->deviceId;
```

它会扫描所有 connections 中该设备的 Key、pointer/non-pointer Motion 和 hover 状态，其他设备保留。合成事件的 eventTime 是 wrapper 执行时的 `now()`，不是 `DeviceResetEntry.eventTime`。

该函数本身不清 `mTouchStatesByDisplay`，也不调用 `resetKeyRepeatLocked()`；不要把它和后面的全局 reset helper 混在一起。

### Motion 注入失败只取消 monitors

Motion 目标查找结果若：

- `PENDING`：继续等待，不取消；
- `PERMISSION_DENIED`：直接结束本次处理，不取消 monitor；
- 其他非 success：按 source class 选择 POINTER/NON_POINTER，只对 monitors 合成取消。

`synthesizeCancelationEventsForMonitorsLocked()` 会遍历**所有 display** 的 global 与 gesture monitor map，而 options 没有 device/display filter。因此一个 display 上的失败，可以结束另一 display monitor connection 中的同类活动状态。

Key 目标查找失败没有一段对称的 monitor cancel 逻辑。

### conflictingPointerActions 是全连接宽取消

冲突可能来自新 DOWN 与已有 down、down 中出现 hover，或某些 device/source 切换状态；但 wrong-device 路径可先作为失败返回，并非所有“设备不同”都会走到 conflict cancel。

一旦 `conflictingPointerActions` 最终为 true，Dispatcher 会：

1. 对所有 connections 做无 device/display 过滤的 POINTER cancel；
2. 然后把当前 Motion 投给新算出的 targets。

同一 connection 上，cancel enqueue 发生在当前 event enqueue 之前；不同 connection 的 socket 和 App 主线程之间没有全局处理顺序。

### ANR cancel 是追加，不是清场

connection ANR 后，policy 若给正数 extension，只延长超时；非正结果回锁后重新查 connection，仍存在时才调用 `cancelEventsForAnrLocked()`。只有 status 为 `NORMAL` 才合成 ALL。

这个 helper 明确不 break connection，也不 drain 原 outbound/wait。CANCEL 只是追加到 outbound。若前面的未完成事件就是 App 卡住的根源，cancel 也可能排在它们之后。

`startDispatchCycleLocked()` 不以 `responsive` 作为 publish 门，所以 status 仍为 NORMAL 时会继续尝试发送；但 connection 已被标成 unresponsive，新的成功 publish 项不会再插入 `mAnrTracker`。entry 自身仍会写入 `timeoutTime`，只是该 deadline 不会注册进 tracker，也就不会靠它再触发一轮普通 connection ANR。

no-focused-window ANR 没有具体 connection token；该分支的非正 policy 结果不会因此找到某条 connection 来合成取消。

### resetAndDropEverything 也不 drain connection queues

禁用 dispatch 或切换 input filter enable 状态时，`resetAndDropEverythingLocked()`：

1. 先对所有 connections 合成 ALL cancel；
2. 清 key repeat、pending/inbound、无焦点 timeout；
3. 清 ANR tracker、TouchState、last hover 与 replaced keys。

它没有清 connection outbound/wait，也没有直接调用 `InputState::clear()`。合成 CANCEL 仍排在同一 connection 的既有 outbound 后。

### 通道失败路径反而不合成 cancel

正向 publish hard error、HUP/unregister 等断链路径会 drain 发送队列、把 connection 变成 BROKEN 或 ZOMBIE；它们不会先尝试向同一条已坏通道发送 CANCEL。此时 App 是否收尾只能依赖进程/窗口生命周期的更高层清理。

源码锚点：`InputDispatcher.cpp:598-680,864-923,1065-1074,1193-1207,1235-1305,1364-1376,2574-2599,3895-3939,4026-4043,4650-4699`。

## 11. 焦点、focused display 与窗口移除：三个范围不同的收尾

### 窗口焦点改变：先 non-pointer cancel，再 Focus(false)

`setInputWindowsLocked()` 比较旧、新 focused window 的 token。只有 token 真正不同，旧焦点窗口存在且仍能找到 channel 时，才：

1. 对旧 channel 合成 `CANCEL_NON_POINTER_EVENTS`；
2. enqueue `FocusEntry(hasFocus=false)`；
3. 从旧焦点表移除；
4. 再为新窗口 enqueue `FocusEntry(hasFocus=true)`。

NON_POINTER 会结束 Key 和 non-pointer Motion，但不结束 mouse/stylus/touch 等 pointer-class motion 或 hover。

这里没有设置 displayId filter。若同一 connection 异常保存其他 display 的 non-pointer memento，也可能一并命中。

### setFocusedApplication 本身不取消

`setFocusedApplication()` 更新 focused application 与无焦点等待状态，但它本身没有调用 cancel synthesis。不要因为名字含 “focused” 就把所有焦点 API 归为同一收尾路径。

### focused display 改变：只结束 display-unspecified

`setFocusedDisplay(newId)` 针对旧 focused display 的旧 focused window，合成 NON_POINTER 时显式设置：

```cpp
options.displayId = ADISPLAY_ID_NONE;
```

这只结束当初未指定 display、靠 focused display 路由过去的状态。显式带某 displayId 的 Key/non-pointer Motion 不受这次筛选影响。

该函数不会为旧、新窗口 enqueue Focus(false/true)；它更新 `mFocusedDisplayId`，再调用 `onFocusChangedLocked(oldFocusedWindowHandle, newFocusedWindowHandle)`，把两个窗口 token 通过 command 异步交给 policy 的 `notifyFocusChanged()`。

### touched window 移除：只选 channel，不缩 device/display

窗口列表刷新后，Dispatcher 遍历该 display 的 `TouchState.windows`。某个 TouchedWindow 已不在窗口表中时：

1. 若还能找到 channel，对它合成 POINTER cancel；
2. 无论 channel 是否存在、cancel 是否能发出，都从 `state.windows` 删除该记录。

options 没设置 deviceId、displayId 或 pointerIds。虽然触发检查发生在某个 display 的 TouchState 内，真正扫描 `InputState` 时并不会自动继承这个 display：

```text
在 display 0 发现 window 被移除
  !=
只取消该 channel 中 display 0 的 pointer memento
```

如果同一 channel 中还有其他 display/device 的 pointer-class memento，它们也可能被合成结束。

### hover 是一个常见误诊

`mLastHoverWindowHandle` 在窗口消失时可以只被置空，而不经过上述 TouchedWindow 移除循环。故“hover 窗口移除必然收到 HOVER_EXIT”不是 r48 保证。

如果后来有 ALL/POINTER 取消命中该 connection 的 hover memento，合成器会正确选择 HOVER_EXIT；但排障时必须先确认实际走到哪个 call site。

### 三种场景对照

| 变化 | 取消对象 | display filter | 后续路由变化 |
|---|---|---|---|
| focused window token 改变 | 旧焦点 channel 的 non-pointer | 无 | 更新窗口焦点，并 enqueue Focus |
| focused display 改变 | 旧 display 焦点 channel 的 non-pointer | 只限 `ADISPLAY_ID_NONE` | 更新 focused display |
| touched window 被移除 | 被移除窗口 channel 的 pointer | 无 | 从该 display TouchState 删除窗口 |

源码锚点：`InputDispatcher.cpp:3684-3793,3795-3824,3826-3875`。

## 12. pilferPointers：monitor 抢的是未来路由，不是传输承诺

### 三个必要条件

`pilferPointers(token)` 要成功，至少要求：

1. token 仍能在 gesture-monitor 注册表中找到 display；
2. 该 display 存在当前 `TouchState`；
3. token 已出现在当前 state 的 `gestureMonitors`，且 `state.down == true`。

只注册过 monitor 不够；它必须已经加入当前 ongoing pointer stream。任一条件失败返回 `BAD_VALUE`。

函数没有额外检查发起 monitor connection 是否 responsive 或 NORMAL。若某个 monitor 尚在注册表和 TouchState，却已发生异常状态，前置验证仍可能通过。

### 对普通窗口的取消有精确 device/display 过滤

验证后从当前 TouchState 取：

- `deviceId = state.deviceId`；
- `displayId = monitor 所属 display`。

然后遍历所有 `state.windows`，对仍能找到 channel 的普通窗口，用 POINTER mode 加上述两个 filter 合成取消。这比窗口移除路径窄得多，但“精确”只精确到 device 与 display：

```text
命中该 window connection 中
deviceId == 当前 TouchState.deviceId
displayId == 当前 monitor display
且 source 属于 POINTER class 的全部 MotionMemento
```

options 没有 source、hovering 或每个 TouchedWindow pointerIds filter。因此同 device/display 下若异常并存另一个 pointer source 或 hover memento，也会命中；非 hover 生成整条 CANCEL，hover 生成 HOVER_EXIT。

### filterNonMonitors 清了什么

取消循环结束后，无条件调用 `state.filterNonMonitors()`。实现只清：

- `windows`；
- `portalWindows`。

它保留：

- `down/split/deviceId/source/displayId`；
- 完整的 `gestureMonitors` 列表。

因此 pilfer 后不是“只剩发起 pilfer 的那一个 monitor”。当前流中的所有 gesture monitors 都继续保留；global monitor 也会在后续每次 dispatch 目标组装时按普通机制加入。

### 为什么不给 monitor 补 DOWN

能通过验证的 gesture monitor 已经从手势开始被捕获进 `TouchState.gestureMonitors`，本来就在接收流。它不是中途第一次成为接收者，所以不需要重放 DOWN。

这与 `transferTouchFocus(from,to)` 不同：transfer 的目标窗口此前可能完全没见过 source 持有的 pointers，必须先构造 DOWN/POINTER_DOWN 序列。

### cancel 失败也不回滚 pilfer

某个 touched window：

- channel 已不存在：跳过 cancel；
- connection 是 BROKEN：helper 跳过；
- cancel 进入 outbound 后 publish 失败：App 仍可能没收到。

无论哪种情况，`filterNonMonitors()` 都照常清路由，函数最终仍返回 `OK`。所以 OK 的含义是“Dispatcher 接受了抢流并改变未来路由”，不是“所有旧窗口均确认收到 CANCEL”。

### 一个细小但重要的测试边界

r48 测试覆盖了窗口 channel 在手势中途移除后，gesture monitor 仍能 pilfer 并继续收到后续 UP 的情形。这说明 pilfer 的核心前提是 monitor 与 TouchState 中的 ongoing stream，不是所有原窗口 channel 都必须仍存活。

源码锚点：`InputDispatcher.cpp:4429-4488`、`TouchState.cpp:119-122`、`InputDispatcher_test.cpp` 的 pilfer 相关测试。

## 13. transferTouchFocus：先改 TouchState，再处理两端 InputState

### 同 token 是不验证的快速成功

`transferTouchFocus(fromToken, toToken)` 一开头先判断：

```cpp
if (fromToken == toToken) {
    return true;
}
```

这发生在加锁和窗口查找之前。所以两个相同但无效的 token、相同 token 但当前没有触摸，也都会返回 true。讨论“transfer 必须满足窗口存在、同 display”时，必须注明这是**非同 token** 分支的条件。

### 非同 token 的前置门

正常分支要求：

1. 能分别按 token 找到 from/to `InputWindowHandle`；
2. 两个 handle 声明的 displayId 相同；
3. 在某个 `mTouchStatesByDisplay` 的 `state.windows` 中按 handle 对象找到 from。

函数遍历所有 display 的 TouchState 查 from handle，并不显式检查命中的 `state.down`。找不到 from touched record 才返回 false。

### 路由修改发生在 connection 查找之前

命中 from 的 `TouchedWindow` 后，函数立即：

1. 保存其 `targetFlags` 与 `pointerIds`；
2. 从 `state.windows` 删除 from；
3. 为 to 调用 `addOrUpdateWindow()`；
4. 然后才按 token 查 from/to `Connection`。

给 to 保留的 flags 仅为：

```text
FOREGROUND | SPLIT | DISPATCH_AS_IS
```

其他 dispatch mode 不转移；pointerIds 原样带过去。如果 to 已经是 touched window，`addOrUpdateWindow()` 会 OR flags 与 pointerIds，而不是新建重复项。

### connection 缺失仍返回 true

只有 fromConnection 与 toConnection 都非空，才执行：

```text
from.inputState.mergePointerStateTo(to.inputState)
-> 给 from 合成 POINTER cancel
-> 给 to 合成 DOWN / POINTER_DOWN
```

任一 connection 找不到时，这三步全部跳过，但此前 TouchState 的 from→to 修改不回滚，函数最后仍 wake 并返回 true。

所以返回 true 只能说明路由层接受了 transfer；它不证明：

- 两条 connection 都存在；
- source 收到 CANCEL；
- target 收到补 DOWN；
- App 两端已经按该顺序处理。

### BROKEN 也存在部分提交

两条 connection 都能查到但其中一条已是 BROKEN 时，`mergePointerStateTo()` 仍先执行。随后 cancel/down wrapper 各自在入口看到 BROKEN 才返回。

可能出现：

```text
TouchState 已转移
target InputState 已 merge
source CANCEL 未生成或 target DOWN 未生成
函数仍返回 true
```

这是典型的“先改本地未来路由，通知尽力而为”，不是跨 connection 事务。

### source cancel 的范围比路由转移更宽

TouchState 只转移命中的那条 TouchedWindow 的 pointerIds。可后面的 source cancel options 只有 POINTER mode，没有本次 transfer 的 deviceId、displayId 或 pointerIds filter。

再加上 merge 本身也遍历 source 的所有 pointer-class MotionMemento，正常正确性依赖上层保证一条窗口 connection 的当前 pointer 状态与被转移的路由相符。若异常地混有其他 display/device/hover 状态，transfer helper 不会主动缩小。

源码锚点：`InputDispatcher.cpp:3947-4023`、`TouchState.cpp:54-77`。

## 14. mergePointerStateTo 与补 DOWN：依赖上层不变量的数组拼接

### merge 选择所有 pointer-class memento

source `InputState` 遍历自己的 `mMotionMementos`，只用：

```cpp
if (memento.source & AINPUT_SOURCE_CLASS_POINTER)
```

作为参与条件。它没有按本次 TouchState 的：

- deviceId；
- displayId；
- pointerIds；
- `hovering=false`

继续过滤。Key、fallback map 和 non-pointer Motion 不转移，但 pointer-class hover memento 也会进入算法。

### 目标匹配甚至不比较 hovering

寻找 target memento 时只比较：

```text
(deviceId, source, displayId)
```

这里与正常 `findMotionMemento()` 不同，少了 `hovering`。如果 target 恰有相同三元组但 hover 类型不同，代码也会把 pointer 数组拼到一起。

正常业务路径显然依赖更高层不让这种组合出现；文章不能把“不会混 hover”说成函数自身已经验证的保证。

### 有目标状态：在尾部追加

假设 target 原来知道两个 pointers：

```text
target: [id 2, id 5]
source: [id 7, id 9]
```

第一次追加前设置：

```text
firstNewPointerIdx = target.pointerCount = 2
```

然后逐个复制 source：

```text
target: [id 2, id 5, id 7, id 9]
firstNewPointerIdx = 2
```

下标 0、1 是 target 已知的前缀；下标 2、3 是必须补发 DOWN 的新部分。

### 目标无对应状态：复制并从 0 开始

若 target 找不到匹配项，代码把 source memento 自身的 `firstNewPointerIdx` 设为 0，再把整条复制进 target：

```text
target: [source 的全部 pointers]
firstNewPointerIdx = 0
```

这表示 target 一个 pointer 都不知道，需要从 `ACTION_DOWN` 开始重建。注意 source memento 也被原地写入 0；正常随后给 source 合成 CANCEL 并删除它，因此这个副作用通常短暂存在。若 source 已 BROKEN、cancel helper 跳过，标记可以留在 source InputState。

固定字段的来源也随分支不同：追加到既有 target memento 时保留 target 原来的 flags、precision、cursor、downTime 与 policyFlags，只追加 source pointers；直接复制时则把 source 的这些固定字段一起带到 target。merge 不根据目标窗口重新计算它们，窗口 offset/scale 仍要等 synthetic DOWN publish 时由 target 变换另行应用。

### merge 不删除 source

`mergePointerStateTo()` 是复制/追加，不是 move：

```text
source MotionMemento 仍在
target MotionMemento 得到合并状态
```

source 的状态靠下一步 synthetic CANCEL 再次 tracking 才删除。若取消没有入队，merge 本身不会替它清账。

### 没有去重和容量保护

`MotionMemento` 的 arrays 是固定 `MAX_POINTERS`，追加循环却没有：

- pointerId 重复检查；
- 合并后 count 上限检查；
- action index 合法性检查。

正确调用依赖 split routing 保证 source/target pointerIds 互斥，且总数不超过系统上限。把它当通用集合 merge API 使用会有越界或重复 id 风险。

### firstNewPointerIdx 怎样挡住普通 MOVE

普通 MOVE/POINTER_DOWN/POINTER_UP tracking 只有在 `firstNewPointerIdx < 0` 时才接受。merge 后它为非负，表示 target 的账本里已有一些“计划交给它、但还没补出 DOWN 序列”的 pointers。

这道门防止后续普通 Motion 直接把陌生 pointer 的 MOVE 当成合法更新。不过正常 `transferTouchFocus()` 在同一把 Dispatcher lock 下马上调用 down synthesis，不存在对外可见的长等待期；它是短暂的内部构造标记，不是 App 级交付确认。

### synthesizePointerDownEvents 的构造

对每条 pointer-class 且 `firstNewPointerIdx >= 0` 的 target memento：

1. 先把 target 已知前缀复制进临时数组；
2. 从 `firstNewPointerIdx` 开始逐个加入新 pointer；
3. 加入后总数为 1：生成 `ACTION_DOWN`；
4. 否则生成 `ACTION_POINTER_DOWN`，action index 使用当前 memento 数组下标 `i`；
5. 为每一步 new 一个 MotionEntry；
6. 全部生成后把该 memento 的 `firstNewPointerIdx` 重置为 -1。

`i` 在这里是数组 index，不是 pointerId。因为临时数组按同一位置复制，当前事件的 count 恰好增长到 `i + 1`，所以 action index 指向刚加入的 pointer。

### 两个具体补发序列

Target 原本为空，source 带 `[id 4, id 8]`：

```text
firstNewPointerIdx = 0
事件 1: ACTION_DOWN,         pointers [4]
事件 2: ACTION_POINTER_DOWN, pointers [4,8], actionIndex=1
```

Target 已知道 `[id 2]`，source 带 `[id 7,id 9]`：

```text
firstNewPointerIdx = 1
事件 1: ACTION_POINTER_DOWN, pointers [2,7],   actionIndex=1
事件 2: ACTION_POINTER_DOWN, pointers [2,7,9], actionIndex=2
```

第二种不需要重发 id 2 的 DOWN，因为 target 已经有对应 memento。

### 合成 DOWN 的字段

DOWN 序列保留 memento 中的 device/source/display、policyFlags、flags、precision、cursor、downTime 与当前 pointer 快照。它把 actionButton、metaState、buttonState、classification、edgeFlags、offset 设为中性值，和取消 Motion 的构造风格一致。

它不添加 `CANCELED`，因为目标正在开始或扩展一条可继续处理的新 gesture。

### reset marker 仍早于 publish

`synthesizePointerDownEvents()` 在返回 EventEntry vector 前已经把 `firstNewPointerIdx` 设回 -1。wrapper 随后才逐条 enqueue，再 start dispatch。

若 target 原本为空，第一条 synthetic DOWN 的 tracking 会先把合并 memento 替换为单 pointer 状态，后续 POINTER_DOWN 再逐步扩展。若 publish 失败，InputState 仍可能显示这条重建序列已经完成。

### 两条 socket 没有全局顺序

Dispatcher 的函数调用顺序是：

```text
source merge to target
source cancel enqueue/start
target down enqueue/start
```

但 source 与 target 是不同 connections。即使两边 publish 都成功，也不能推出两个 App 线程一定先处理 source CANCEL、再处理 target DOWN。保证的是各自 connection 内部的队列顺序，不是跨进程原子切换。

源码锚点：`InputState.cpp:257-265,300-381`、`InputDispatcher.cpp:2866-2922,4005-4014`。

## 15. r48 policy fallback：映射存在，但普通 restart 没有重新 tracking

### 先说预期模型

从类接口看，似乎可以这样理解 fallback：

```text
App 不处理 original key
  -> policy 选择 fallback key
  -> InputState 记 original -> fallback
  -> fallback DOWN 建立带 FALLBACK flag 的 KeyMemento
  -> 需要撤销时 CANCEL_FALLBACK_EVENTS 合成 canceled fallback UP
```

这个模型只有“fallback KeyEntry 作为一条普通新投递重新经过 enqueue/track”时才成立。r48 的标准 policy fallback 不是这样实现的。

### 第一轮：原 Key 正常建立 memento

原始 Key DOWN 创建新 DispatchEntry 时：

```text
dispatchEntry.resolvedAction = original action
dispatchEntry.resolvedFlags  = original flags
trackKey(original)
  -> KeyMemento 保存 original keyCode
outbound -> publish -> wait
```

App 回 `handled=false` 后，完成命令进入 `afterKeyEventLockedInterruptible()`。

### policy 回调与映射

对 foreground、非 fallback 的未处理 Key，Dispatcher 调 policy 的 `dispatchUnhandledKey()`。首次 DOWN 根据 policy 结果锁定：

```text
mFallbackKeys[originalKeyCode] = fallbackKeyCode
```

若 policy 不提供 fallback，value 使用 `AKEYCODE_UNKNOWN`。这张 map 维持同一 original lifecycle 后续 repeat/UP 的 policy 选择，但它不是 KeyMemento。

### restart 原地改写同一个 KeyEntry

policy 选择 fallback 后，r48 没有 new 一个独立 EventEntry/DispatchEntry，而是原地修改当前 `keyEntry`：

- eventTime、device/source/display；
- flags 加 `AKEY_EVENT_FLAG_FALLBACK`；
- keyCode 改成 fallbackKeyCode；
- scanCode/metaState/repeat/downTime；
- `syntheticRepeat=false`。

函数返回 `true` 表示 restart。完成命令把同一个 DispatchEntry 从 wait 擦除后，直接：

```cpp
connection->outboundQueue.push_front(dispatchEntry);
```

它没有调用 `enqueueDispatchEntryLocked()`。

### 第一个断裂：没有 fallback KeyMemento

因为 restart 不重新经过 `trackKey()`：

```text
InputState 仍保留 original-key KeyMemento
InputState 没建立 fallback-key KeyMemento
```

内部 KeyEntry 已变成 fallback key，并不等于 InputState 中 vector 的值同步改变；memento 是此前按值复制出的独立结构。

这会带来一个可见的收尾差异：fallback DOWN 已尝试送给 App 后若焦点切换，NON_POINTER cancel 扫到的仍可能是 original memento，于是合成 original key 的 canceled UP，而不是 fallback key 的 canceled UP。

### 第二个断裂：DispatchEntry 的 resolvedFlags 没刷新

restart 同样没有重算：

- `dispatchEntry->resolvedAction`；
- `dispatchEntry->resolvedFlags`；
- `dispatchEntry->resolvedEventId`。

Key publisher 第二轮取 keyCode 等字段自已被改写的 `keyEntry`，但 flags 取：

```cpp
dispatchEntry->resolvedFlags
```

它仍是第一轮 original flags。于是 r48 这条标准 restart 的 App wire leg 不会因为内部 `keyEntry->flags` 新增了 `AKEY_EVENT_FLAG_FALLBACK` 而自动带上该 flag。

“没有重算”不代表三个字段都已出现值冲突：此 restart 并未修改 `keyEntry.action` 或 `keyEntry.id`，所以现有源码只直接证明 `resolvedFlags` 因缺少新加的 FALLBACK 位而陈旧；resolvedAction/resolvedEventId 只是继续沿用第一轮值。

内部完成处理仍检查 `keyEntry->flags & FALLBACK`，所以 system_server 知道第二轮是 fallback；App 收到的 packet flags 与内部判断却不是同一本值。

### 第三个断裂：CANCEL_FALLBACK_EVENTS 通常匹配不到

r48 有两个 call site 构造：

```text
mode = CANCEL_FALLBACK_EVENTS
keyCode = fallbackKeyCode
```

一个用于 original 后来被处理或已非 foreground，另一个用于 policy 改变/撤销 fallback。可 `shouldCancelKey()` 只扫描：

```text
KeyMemento.keyCode == fallbackKeyCode
&& KeyMemento.flags 带 FALLBACK
```

标准 restart 留下的是 original-key、original-flags memento，两项都不满足。fallback map 也不会被这个筛选函数读取。因此这两个调用对标准 policy-restart leg 通常合成不出预期的 fallback UP。

### fallback UP 的反向清理也被绕开

普通独立 enqueue 的 fallback UP 经过 `trackKey()` 时，可以按 fallback value 清 `mFallbackKeys`。标准 restart 的 fallback UP 同样直接把原 DispatchEntry 推回 outbound，不走 tracking，因此这段反向清理也不会执行。

原始 Key UP 在 `afterKeyEventLockedInterruptible()` 开头会按 original keyCode 主动 `removeFallbackKey()`，所以正常 original lifecycle 仍有另一条 map 清理路径；两者不能混为一谈。

### CANCEL_FALLBACK_EVENTS 不是完全死代码

如果某条本来就带 `AKEY_EVENT_FLAG_FALLBACK` 的 KeyEntry 通过普通 enqueue 路径进入 connection，`trackKey()` 会建立真正的 fallback KeyMemento。此时 FALLBACK mode 可以命中。

准确结论是：

> r48 的筛选与合成能力本身能处理被正常 tracking 的 fallback key；标准 policy restart 路径却没有建立这种 memento，也没有刷新 DispatchEntry resolved flags，二者发生了实现断接。

### 用一条时间线核账

```text
1. original DOWN 入队
   InputState: original DOWN memento
   DispatchEntry.resolvedFlags: original flags

2. App 回 handled=false
   mFallbackKeys: original -> fallback
   KeyEntry: 原地改成 fallback + internal FALLBACK

3. 同 DispatchEntry 直接 wait -> outbound
   没有 trackKey
   resolvedFlags 仍是 original flags

4. fallback DOWN publish
   wire keyCode: fallback
   wire flags: stale resolvedFlags
   InputState: 仍是 original memento

5. 尝试 CANCEL_FALLBACK_EVENTS(fallback keyCode)
   找不到 fallback-key + FALLBACK-flag memento
```

这不是根据接口命名猜出的风险，而是 r48 静态调用链的直接结果。以后阅读新版本时，应检查 restart 是否改成新 entry、是否重新 tracking、resolved 字段是否重算；不能把本章结论无条件外推到其他 Android 版本。

源码锚点：`InputDispatcher.cpp:2323-2337,2478-2492,4751-4806,4809-4983`、`InputState.cpp:41-89,384-423`。

## 16. 排障清单、九道练习与下一章

### dumpsys 能看到什么

r48 `dumpsys input` 会显示：

- 每 display TouchState 的 down/split/device/source；
- touched windows 的 pointerIds 与 targetFlags；
- portal windows；
- 已注册 global/gesture monitor 名称；
- connection outbound/wait queue 中仍存活的事件。

它不会直接显示每个 connection 的：

- KeyMementos；
- MotionMementos；
- fallback map；
- hovering；
- `firstNewPointerIdx`；
- 当前 TouchState 已捕获的 gestureMonitors 明细。

所以“dumpsys 没看到 InputState 异常”通常只是因为没有 dump 这个私有账本。合成事件尚在 outbound/wait 时可从队列间接观察；完成并释放后没有专门历史。

### 先按四层排障

遇到“窗口还像按住”“新窗口只见 MOVE”“pilfer 后旧 App 仍响应”时，依次问：

1. **路由层**：TouchState 当前把 future Motion 给谁？pointerIds 是否 split？
2. **connection 账本**：应有哪条 Key/Motion memento？它是何时被 eager tracking 建立或删除的？
3. **合成选择层**：调用了哪个 wrapper、mode 和 optional filters？是否意外宽取消或根本未命中？
4. **传输层**：合成 entry 在 outbound、wait，还是 connection 已 BROKEN/ZOMBIE？App 是否回 FINISHED？

不要从第 4 层的一条 Java 日志直接反推前 3 层，也不要用 InputState 已中性代替 App 状态已中性。

### 症状与优先假设

| 症状 | 先查什么 | 不要先下的结论 |
|---|---|---|
| App 看过 DOWN，没看见 CANCEL | connection status、旧 outbound 积压、publish error | “合成器一定没运行” |
| InputState 没有 motion，但 App 仍按住 | CANCEL tracking 是否早于 hard publish failure | “App 一定已收 CANCEL” |
| 新窗口 transfer 后先见 MOVE | 两端 connection 是否存在/BROKEN、补 DOWN 是否真正 publish | “firstNewPointerIdx 永远能保证跨进程顺序” |
| pilfer 返回 OK，旧窗口未收 CANCEL | 旧 channel 是否存在、BROKEN、队列是否阻塞 | “OK 就是全部 App 已确认” |
| 一次丢包后其他 display 也被取消 | call site 是否未设 device/display | “InputState 串 display 查错了” |
| fallback cancel 没生成 UP | 是否走标准 restart、是否存在真实 fallback memento | “只要 fallback map 有值就能命中” |

### 四个完整推演

场景 A：设备 12 的两指触摸 reset。

```text
options = ALL + deviceId 12
-> 扫所有 connections
-> 每条命中的非 hover memento 生成一条完整 ACTION_CANCEL
-> 设备 13 的状态保留
-> TouchState 不因 dispatchDeviceResetLocked 本身自动清空
```

场景 B：旧焦点窗口有 Key 和 mouse hover。

```text
focused window token 改变
-> NON_POINTER
-> Key 合成 canceled UP
-> mouse hover 属于 pointer class，不被这次 mode 命中
-> 然后 enqueue Focus(false)
```

场景 C：monitor pilfer 当前 display/device 的 split touch。

```text
所有普通 touched windows:
  按同 device/display 合成整条 pointer CANCEL
TouchState:
  windows/portalWindows 清空
  所有 gestureMonitors 保留
monitor:
  不补 DOWN，因为本就在当前流
```

场景 D：A→B transfer，B 已知 id 3，A 持有 id 8。

```text
TouchState: A 的 pointerIds 转给 B
B InputState: [3,8], firstNewPointerIdx=1
A: 合成整条 CANCEL
B: 合成 POINTER_DOWN [3,8], actionIndex=1
随后 B 的普通 MOVE 才继续更新
```

这些都是 Dispatcher 的计划顺序；每次推演最后还要补一句：“若 channel 不可通信，App 观察结果可以停在更早状态。”

### 练习 1：标出 eager tracking

```bash
sed -n '2209,2425p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '2456,2613p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

在纸上标出 `trackMotion`、outbound push、publish、wait push。解释为什么 memento 不是“App 已收到”的证据。

### 练习 2：穷举 Key 状态机

```bash
sed -n '41,90p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

分别推演重复 DOWN、匹配 UP、缺失 DOWN 的 UP、其他 action。证明 r48 `trackKey()` 没有实际 false 分支。

### 练习 3：找出 Motion 固定字段

```bash
sed -n '91,255p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

列出 `addMotionMemento()` 保存的字段，再圈出 `setPointers()` 真正更新的字段。解释为什么 MOVE 后合成 CANCEL 不能称为“完整复制最后一条 MOVE”。

### 练习 4：手算取消矩阵

```bash
sed -n '24,49p' frameworks/native/services/inputflinger/dispatcher/CancelationOptions.h
sed -n '402,446p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

给同一 connection 放入 Key、touch、mouse hover、joystick 四类 memento，分别套 ALL、POINTER、NON_POINTER、FALLBACK，并再加 `keyCode` filter，写出命中集合。

### 练习 5：证明 synthesize 不 reset

```bash
sed -n '41,193p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
sed -n '268,357p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
sed -n '2799,2864p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

找出真正 erase memento 的位置。再假设 CANCEL publish hard error，分别写出 InputState、outbound/wait 与 App 的最终状态。

### 练习 6：比较三个焦点相关入口

```bash
sed -n '3691,3793p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '3795,3875p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

比较 focused window、focused application、focused display：谁发送 cancel、谁设置 display filter、谁 enqueue FocusEvent。

### 练习 7：推演 pilfer

```bash
sed -n '4429,4476p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '89,123p' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
```

画出两个 ordinary windows、两个 gesture monitors 和一个 global monitor。pilfer 后分别说明哪些路由记录还在，以及 cancel 失败时函数返回值是什么。

### 练习 8：手算 transfer 的数组

```bash
sed -n '257,381p' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
sed -n '3947,4024p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

让 target 已知 `[1,4]`、source 为 `[7,9]`，写出 `firstNewPointerIdx`、每条 synthetic POINTER_DOWN 的 pointerCount 与 action index。再加入重复 pointerId，指出源码缺哪两类保护。

### 练习 9：复现 fallback 账本断裂

```bash
sed -n '4751,4807p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4809,4984p' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
rg -n 'resolvedFlags =' frameworks/native/services/inputflinger/dispatcher
```

从 original DOWN 画到 fallback DOWN 第二次 publish。分别记录 KeyEntry.keyCode/flags、DispatchEntry.resolvedFlags、KeyMemento 和 fallback map，解释 `CANCEL_FALLBACK_EVENTS` 为什么通常匹配不到标准 restart leg。

### 最终不变量清单

- TouchState 决定未来 pointer 路由；InputState 保存每 connection 的提前语义账本。
- tracking 在 outbound push 与 publish 前；memento 不能证明 App 已收到。
- Key UP 缺 memento 仍允许；Motion UP/CANCEL 与 HOVER_EXIT 缺 memento 会被跳过。
- 普通 Motion MOVE/POINTER action 只刷新 pointer 数组，不刷新所有 memento 字段。
- navigation 只在特定 action 分支早退；joystick neutral 看 `PointerCoords.bits` 是否为空。
- mode 选事件类别，wrapper 选 connections，optional filter 再缩 identity；三层不能混读。
- `synthesizeCancelationEvents()` 自身不 erase；normal wrapper 借合成事件重新 tracking 提前消账。
- synthetic cancel 是普通 outbound 投递，不能保证 publish、App 处理或 FINISHED。
- pilfer 保留所有 gesture monitors，不给 monitor 补 DOWN；transfer 为新窗口补 DOWN。
- transfer 先改 TouchState，再查 connections；merge、两边通知和返回值不是事务。
- merge 不比 hovering、不去重、不检查 MAX_POINTERS，依赖上层不变量。
- r48 标准 policy fallback restart 绕过 enqueue/track，fallback map 不等于 fallback memento。

### 下一章

下一章继续读 **190 Android InputChannel 与窗口生命周期**：从 channel pair、token、`InputWindowHandle`、`Connection` 注册，到 BROKEN、ZOMBIE、HUP、unregister 与队列 drain，解释为什么“窗口消失”“通道断开”和“Connection 销毁”不能合并成一个时刻。
