# 249 Android MultiTouchInputMapper、MT Slot与NotifyMotion坐标生产链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一帧多点报告怎样变成身份连续、坐标正确的Android动作

屏幕偶发串指、旋转后坐标跑偏、一次 `SYN_REPORT` 却看到多笔 `MotionEvent`，根因可能分别落在 slot 状态、pointerId 延续、Viewport 投影或动作展开，不能笼统归为“触摸驱动抖动”。本章只回答一条主问题：**由多条 evdev 事件组成的一帧多触点报告，怎样在同步边界后获得连续身份、显示坐标和合法 Android Action 序列？**

先分清四个状态层与完成点：

| 边界 | 此时保存的事实 | 仍不能证明 |
|---|---|---|
| Accumulator 处理一条 `EV_ABS` | 当前 slot 的一个字段已更新 | 一帧完整，其他轴属于同一时刻 |
| `SYN_REPORT → sync()` | 新 `RawState` 已封口并完成本帧 ID 分配 | 一定已向下游发 Motion；外接笔融合可暂停 |
| `cookAndDispatch()` 返回 | 该 RawState 已经过门控、坐标烹制和动作展开 | Dispatcher 已选窗口或 App 已处理 |
| `notifyMotion()` 返回 | 栈上参数已被 `QueuedInputListener` 深拷贝 | Reader 本轮已经 flush |
| Reader 锁外 `flush()` 返回 | 本轮排队输入已依次交给下一层 listener | Channel 发送、App 消费或 present 已完成 |

同一触点还有四种不能互换的身份：

| 名称 | 作用域 | 是否稳定 |
|---|---|---|
| MT slot | Protocol B 驱动状态表下标 | 可被下一次接触复用 |
| trackingId | 驱动给一次接触的跟踪值 | 合规驱动应在该接触期内稳定 |
| pointerId | Android 当前输入流使用的 0—31 身份 | Mapper 分配，可跨数组重排保持 |
| pointer index | 单笔 Motion 数组位置，也编码在 POINTER_DOWN/UP action 高位 | 每笔都可能改变 |

本章以 `DEVICE_MODE_DIRECT` 触摸屏为主。触摸板手势、虚拟键细则和 Bluetooth stylus 融合策略留到第 250 章；这里仅保留会改变主动作链完成点的门。

### 练习 1：在一笔事件里同时认出四种身份与五个完成点

Protocol B 的 slot 5 报 trackingId 81；Mapper 将它延续为 pointerId 2。同笔输出数组按 pointerId 升序装入 ID 0、2。问这个接触的 slot、trackingId、pointerId、pointer index 各是多少；驱动刚发 `SYN_REPORT` 时又能证明到表中哪一层？

唯一答案是 slot=5、trackingId=81、pointerId=2、pointer index=1。`SYN_REPORT` 本身首先确立的是 RawState 封口；无等待时同一次调用可以继续 cook，但若外接笔融合正在等数据，`cookAndDispatch()` 就会暂缓，更不能据此证明 Queued listener 已 flush 或 App 已收到。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'void MultiTouchInputMapper::process(const RawEvent* rawEvent) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'TouchInputMapper::process(rawEvent);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mMultiTouchMotionAccumulator.process(rawEvent);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (rawEvent->type == EV_SYN && rawEvent->code == SYN_REPORT) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'sync(rawEvent->when);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F '#define MAX_POINTERS 16' frameworks/native/include/input/Input.h
grep -n -F '#define MAX_POINTER_ID 31' frameworks/native/include/input/Input.h
grep -n -F 'getListener()->notifyMotion(&args);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mArgsQueue.push_back(new NotifyMotionArgs(*args));' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'mQueuedListener->flush();' frameworks/native/services/inputflinger/reader/InputReader.cpp
```

## 2. 轴元数据先选择Protocol，slot容量与输出容量却是两道上限

`configureRawPointerAxes()` 读取 MT position、size、orientation、pressure、distance、trackingId 与 slot 的 absinfo。只有 trackingId 和 slot 两根轴都有效、slot 最小值恰为 0 且最大值大于 0，r48 才采用 slot-based Protocol B；最大值为 0 的单 slot 声明反而落入 Protocol A 回退。这个判断看的是配置期轴元数据，不是运行中是否偶尔收到 `ABS_MT_SLOT`。

Protocol B 状态表最多 32 个 slot，驱动声明更多会裁到 32；Android 一笔 Motion 最多 16 个 pointer。于是 32 个 active slot 并不等于能输出 32 点：`syncTouch()` 按 slot 下标遍历，收满前 16 个非 palm 点就直接 break，后面的 active slot 本帧不会进入 RawPointerData。32 是内部状态容量，16 才是输出 pointerCount 上限，pointerId 的 0—31 空间又是第三个维度。

## 3. Protocol B保存稀疏增量，负tracking只让slot退出使用

Protocol B 先用 `ABS_MT_SLOT=n` 选择槽，随后的 position、size、pressure、distance、orientation、tool type 与非负 trackingId 只更新该槽。大多数字段一写就把 `mInUse` 置 true；`TRACKING_ID < 0` 才把槽置为 unused，但故意保留旧字段。下一次接触复用这个槽时，驱动必须重新给出所需初值，否则 r48 会沿用旧坐标、旧尺寸甚至旧 tool type。

无效 slot 值会让当前槽保持越界；后续字段被忽略，直到新的合法 `ABS_MT_SLOT` 修正。最终 `SYN_REPORT` 并不只读取“本帧变过的槽”，而是遍历整张表，把 `isInUse()` 槽作为候选；随后还要跳过 palm，并受第2节所述 16 点上限约束。这正是稀疏增量能还原当前候选集合的原因。

### 练习 2：手算两个Protocol B帧后的完整slot表

初始全空。第一帧依次报告 slot0、tracking81、X=10、Y=20、slot1、tracking95、X=30、Y=40、`SYN_REPORT`。第二帧只报告 slot0、X=15、slot1、tracking=-1、`SYN_REPORT`。问两帧各自输出哪些槽和值。

唯一答案是第一帧输出 slot0=(81,10,20) 与 slot1=(95,30,40)；第二帧 slot0 仍是 in-use，沿用 tracking81/Y20 并把 X 改为15，slot1 退出，所以只输出 slot0。slot1 的旧字段仍留在对象里，但 unused 使它不进入 RawState。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static constexpr size_t MAX_SLOTS = 32;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (mRawPointerAxes.trackingId.valid && mRawPointerAxes.slot.valid &&' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mRawPointerAxes.slot.minValue == 0 && mRawPointerAxes.slot.maxValue > 0) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'size_t slotCount = mRawPointerAxes.slot.maxValue + 1;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (rawEvent->code == ABS_MT_SLOT) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mCurrentSlot = rawEvent->value;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (mUsingSlotsProtocol && rawEvent->value < 0) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'slot->mInUse = false;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'slot->mAbsMTTrackingId = rawEvent->value;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (outCount >= MAX_POINTERS) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mMultiTouchMotionAccumulator.getSlot(inIndex);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
```

## 4. Protocol A每帧借临时slot重建；reset也无法回读Protocol B全表

不满足 B 条件时，Accumulator 分配 16 个临时槽并令 `usingSlotsProtocol=false`。第一条 `EV_ABS` 会选择临时 slot0，每个 `SYN_MT_REPORT` 把 current slot 加一，`SYN_REPORT` 时遍历本帧临时槽；`finishSync()` 随即把它们全部清空并把 current slot 复位为 -1。所以下一帧的第一个接触又从 slot0 开始，临时下标没有跨帧身份含义。

负 trackingId 的“抬起”语义只写在 Protocol B 分支；Protocol A 收到负值反而会把临时槽标为 in-use、保存 -1，随后迫使整帧走距离 ID 回退。另一个实现边界是 `SYN_MT_REPORT` 的 current-slot++ 没有 `mUsingSlotsProtocol` 保护：合规 B 驱动不应发送它；若误发，r48 仍会暗中切到相邻槽，后续字段可能写错位置。

Mapper reset 时，Protocol A 直接清空；Protocol B 也只能清字段，再通过 ioctl 查询“当前选中的 slot 下标”，没有通用接口恢复各 slot 内容。查询时刻与 evdev 缓冲首事件的历史选择可能不同，因此源码接受短暂串槽/跳点来换取不留下 stuck touch。外层 reset 还会清 pointerId、pending/raw/cooked、hover、downTime 与 aborted 状态，它不是延续旧手势的边界。

### 练习 3：比较Protocol A清帧与误发SYN_MT_REPORT的Protocol B

Protocol A 一帧报告点 A 的 X/Y、`SYN_MT_REPORT`、点 B 的 X/Y、`SYN_REPORT`。问封口时临时槽与封口后状态。另有 B 设备已选 slot0，却误发一次 `SYN_MT_REPORT` 后直接报 X；没有新的 `ABS_MT_SLOT`，X 会写到哪里？

唯一答案是 A 在封口时从临时 slot0/1 读出两个点，随后所有临时槽清空且 current=-1；B 的 current slot 会被无条件加到1，若 slot1 合法，X 就写入 slot1，否则被忽略。不能因为设备已被判为 B 就假定 `SYN_MT_REPORT` 是 no-op。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mMultiTouchMotionAccumulator.configure(getDeviceContext(), MAX_POINTERS,' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'false /*usingSlotsProtocol*/);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F '} else if (mCurrentSlot < 0) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mCurrentSlot = 0;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F '} else if (rawEvent->type == EV_SYN && rawEvent->code == SYN_MT_REPORT) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mCurrentSlot += 1;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (!mUsingSlotsProtocol) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'status_t status = deviceContext.getAbsoluteAxisValue(ABS_MT_SLOT, &initialSlot);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'clearSlots(initialSlot);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mMultiTouchMotionAccumulator.reset(getDeviceContext());' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mPointerIdBits.clear();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mMultiTouchMotionAccumulator.finishSync();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
```

## 5. 子类先让base看事件；只有最终SYN_REPORT才封口RawState

`MultiTouchInputMapper::process()` 的调用顺序容易看反：它先让 `TouchInputMapper::process()` 更新 button/scroll，并在最终 `SYN_REPORT` 上调用 `sync()`，之后才让 MT Accumulator 处理同一 RawEvent。这没有漏掉最后一笔 MT 数据，因为所有先前 `EV_ABS` 已在各自调用末尾进入 Accumulator，而 Accumulator 对 `SYN_REPORT` 本来没有动作；`SYN_MT_REPORT` 不触发 base sync，却会在随后进入 Accumulator 推进临时槽。

每次最终同步都先在 `mRawStatesPending` 末尾新建并 clear 一个 `RawState`，写入 when、button、scroll，再调用虚函数 `syncTouch()` 扫整张 slot 表、分配 ID 并恰好执行一次 `finishSync()`。即使轴、ID 与 button 都没有变化，也照样产生一个 RawState；在未被 stylus 门延迟、raw 门消费或 palm 状态阻断的 DIRECT 主路径上，只要 touching ID 集合非空且前后相同，后面就会发 MOVE，不要求坐标先比较出变化。

## 6. Pending队列按硬件相邻帧续ID，外接笔门可让SYN暂时不下发

通常 pending 队列只有刚封口的一项，ID 回退拿 `mCurrentRawState` 作上一帧；若 Bluetooth stylus 融合把早先帧挡在队列里，新帧仍继续封口，身份参照改用 pending 倒数第二项。这样 pointerId 按相邻硬件帧延续，而不是跨过未交付帧直接对比最后一次下发状态。

`processRawTouches()` 只有在 `assignExternalStylusId()` 不要求等待时，才把 next 复制进 `mCurrentRawState` 并 cook/dispatch。等待会保留本项及其后各项并请求 Reader timeout；stylus 数据到达或超时后再继续 drain。因此 `SYN_REPORT` 是硬件帧完成点，不是 Motion 下发完成点。

准备下发时，若 next.when 小于 `mLastRawState.when`，r48 会把它夹到上一已处理 RawState 的时间；它只保证这条 Mapper 时间线不倒退，不恢复真实采样时刻。设备处于 `DEVICE_MODE_DISABLED` 时则直接 clear 当前与全部 pending，既不补发这些帧，也不靠稍后 enable 自动重放。

### 练习 4：两帧为何能先封口却暂时都没有Motion

最后已交付状态为 S0。触摸帧 S1 初次按下，但已连接的外接笔尚无压力数据；S2 随后移动并也到达 `SYN_REPORT`。问 S1、S2 是否都能形成 RawState，S2 的 ID 参照谁，何时才能 cook。

唯一答案是二者都已进入 pending；S1 等待 stylus，drain 在它处停止，S2 的 ID 以相邻 pending 帧 S1 为参照，而不是 S0。新 stylus 数据或 timeout 解除门后，才按 S1、S2 顺序复制到 current 并 cook。两次 `SYN_REPORT` 都不能单独证明已经发出 Motion。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mRawStatesPending.emplace_back();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'next.when = when;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'syncTouch(when, &next);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mRawStatesPending.size() == 1 ? mCurrentRawState : mRawStatesPending.rbegin()[1];' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'assignPointerIds(last, next);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'processRawTouches(false /*timeout*/);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (assignExternalStylusId(next, timeout)) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (mCurrentRawState.when < mLastRawState.when) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mCurrentRawState.when = mLastRawState.when;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'getContext()->requestTimeoutAtTime(mExternalStylusFusionTimeout);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mRawStatesPending.clear();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

## 7. slot先变Raw Pointer；tool、hover和palm在这里决定集合成员

`syncTouch()` 按 slot 下标复制 X/Y、pressure、touch/tool size、orientation、distance；MultiTouch 路径把每点 tiltX/tiltY 固定为 0。某个 slot 尚未收到 minor 值时，getter 回退对应 major，轴未声明只是其中一种情形。tool type 优先来自该 slot 的 `ABS_MT_TOOL_TYPE`，未知再查共享 TouchButtonAccumulator，仍未知就当 FINGER。

hover 判定也不是纯 per-slot：只有 TouchButtonAccumulator 的全局 tool type 不是 MOUSE 时，才允许“全局 button accumulator 正在 hover”或“pressure 轴有效且该 slot pressure<=0”把点放入 hovering bits；否则进入 touching bits。不能只看某个 `PointerProperties.toolType` 反推这道门。

PALM 槽自身不进入 raw pointer 数组。若此前已有 touching cooked state，`cancelTouch()` 会先用旧状态发 CANCEL 并置 `mCurrentMotionAborted`，同帧与后续非 palm 点虽继续 cook，却不再 dispatch，直到一次 cooked pointerCount 为 0 才解禁。若 palm 第一次出现时根本没有旧 touching state，`abortTouches()` 无对象可取消，也不会置 aborted；同一初始帧里的其他非 palm 点仍可能发 DOWN。这里实现的是“已有流被 palm 中止”，不是一枚 palm 永久封锁所有新触点。

### 练习 5：palm仍在场时为什么后来又能出现新DOWN

已有两个 finger 正在触摸。下一帧 slot0 变 PALM、slot1 仍为 finger；再下一帧 slot1 抬起，只剩会被跳过的 palm；随后 palm 仍在，slot1 又加入新 finger。问三帧各自的 Motion 结果与 aborted 状态。

唯一答案是第一帧先对旧双指流发一笔 CANCEL，并保持 aborted；第二帧输出 raw pointerCount=0，不发普通动作并清掉 aborted；第三帧遍历 palm 时没有旧 touching state 可取消，所以不会重新置 aborted，新 finger 可成为一笔新的 DOWN。若把 palm 本身算入 pointerCount，就会错误预测永不解禁。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'outPointer.tiltX = 0;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'outPointer.tiltY = 0;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'void TouchInputMapper::configureRawPointerAxes() {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mRawPointerAxes.clear();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'getAbsoluteAxisInfo(ABS_MT_POSITION_X, &mRawPointerAxes.x);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'getAbsoluteAxisInfo(ABS_MT_DISTANCE, &mRawPointerAxes.distance);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'outPointer.toolType = mTouchButtonAccumulator.getToolType();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'outPointer.toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mTouchButtonAccumulator.getToolType() != AMOTION_EVENT_TOOL_TYPE_MOUSE &&' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F '(mRawPointerAxes.pressure.valid && inSlot->getPressure() <= 0));' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (inSlot->getToolType() == AMOTION_EVENT_TOOL_TYPE_PALM) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'cancelTouch(when);' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mCurrentMotionAborted = true;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (!mCurrentMotionAborted) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (mCurrentCookedState.cookedPointerData.pointerCount == 0) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mCurrentMotionAborted = false;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

## 8. 合规trackingId优先续用pointerId，但重复值不会被框架拆开

每次 `syncTouch()` 先乐观令 `mHavePointerIds=true`。对非负 trackingId，它先在上一轮占用的 `mPointerIdBits` 中找相同映射；找不到便在这份 bitset 上标记第一个空 ID，并把 tracking 写入 map。由于上一帧尚未在本帧结束前从 bitset 移除，刚抬起点的 ID 会暂时保留，新 tracking 不会在同一帧立即抢走它。最后 `newPointerIdBits` 才替换为当前集合。

只要某个 active slot 的 trackingId 为负或无法取得 ID，本帧已写的所有 id bits 就一起清空，base 对全体 pointer 统一做距离回退；不会半帧信 tracking、半帧猜距离。这个回退只针对当前 RawState，下一帧仍会重新尝试 tracking 路径。

“同一时刻 trackingId 唯一”却完全交给驱动保证。若两个新 slot 都报 7，第一个先把 7 映到 ID0，第二个会在已被本次调用修改的 `mPointerIdBits` 中再次找到 ID0；代码既不检查 `newPointerIdBits` 已含该 ID，也不触发回退。Raw 数组可以有两项，但 touching bit 只有一位，后写的 `idToIndex[0]` 决定实际派发哪项。descriptor 能容错不等于触点身份层也能修复坏驱动。

### 练习 6：重复trackingId怎样把两个slot折叠成一个Android点

上一帧为空；slot0 在 (10,20)、slot1 在 (30,40)，两者都报 trackingId=7 且都 touching。问本帧 tracking 分配、raw pointerCount、touching bits 与最终可派发坐标。

唯一答案是 slot0 先取得 ID0，slot1 随后也命中 tracking7→ID0；`mHavePointerIds` 仍为 true，raw pointerCount 是2，但 touching bits 只有 {0}，`idToIndex[0]` 被 slot1 覆盖。动作数组按 bits 取一项，因此只会暴露 ID0 的后一组坐标 (30,40)，而不是自动拆成 ID0/ID1。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mHavePointerIds = true;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (trackingId >= 0) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (mPointerTrackingIdMap[n] == trackingId) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'id = mPointerIdBits.markFirstUnmarkedBit();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mPointerTrackingIdMap[id] = trackingId;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'if (id < 0) {' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'outState->rawPointerData.clearIdBits();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'newPointerIdBits.clear();' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'outState->rawPointerData.idToIndex[id] = outCount;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'mPointerIdBits = newPointerIdBits;' frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp
grep -n -F 'inline void markBit(uint32_t n) { markBit(value, n); }' system/core/libutils/include/utils/BitSet.h
```

## 9. 距离回退按同toolType贪心配对，也会在同帧复用离场ID

没有上一帧时，fallback 按当前 raw 数组顺序分配 0、1、2……；前后恰好各一项且 toolType 相同，则无论跳多远都继承旧 ID。一般情形为每个“当前点×上一点”的同 toolType 组合计算 raw X/Y 平方距离，放进最小堆，再从近到远贪心选择双方都未匹配的 pair。它没有最大距离阈值，也不是求全局最小总代价；快速交叉、相等距离和同帧 lift+new 都可能交换或错误延续身份。

匹配发生在 affine、scale 与旋转之前，因此 Display 方向变化不会参与距离；代价是 raw 轴异常直接污染 ID。hover 与 touch 只要 toolType 相同也允许互相匹配，以便同一 stylus 在悬停/接触间续用身份。

最后给未匹配 current 分配 ID 时，`usedIdBits` 只包含“成功继承给当前点”的旧 ID，并没有预留未匹配的离场 ID。因此新点可在同一 RawState 复用刚消失点的 ID。若前后 ID 集合因此完全相等，后面的集合差看不出 replacement，只会发一笔 MOVE；toolType/property 可在这笔 MOVE 中改变。这是 r48 的身份近似边界，不是驱动真的报告了连续接触。

### 练习 7：一个finger消失、一个stylus出现为何可能只有MOVE

tracking 路径已失败。上一帧只有 touching finger，ID0、raw(0,0)；当前帧只有 touching stylus，raw(1000,1000)。问单点快速路径、一般距离 heap、新 ID 和最终 Action。

唯一答案是 toolType 不同使 1→1 快路径不成立，heap 又没有同类型 pair；`usedIdBits` 为空，未匹配 stylus 立即取得 ID0。前后 touching bits 都是 {0}，`dispatchTouches()` 走集合相等分支并发 MOVE，数组里 ID0 的 toolType 从 finger 变为 stylus。这里没有距离阈值替框架判断“其实是新接触”。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'current.rawPointerData.clearIdBits();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (lastPointerCount == 0) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'uint32_t id = i;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (currentPointerCount == 1 && lastPointerCount == 1 &&' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'current.rawPointerData.pointers[0].toolType == last.rawPointerData.pointers[0].toolType) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'uint32_t id = last.rawPointerData.pointers[0].id;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (currentPointer.toolType == lastPointer.toolType) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'uint64_t distance = uint64_t(deltaX * deltaX + deltaY * deltaY);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'for (uint32_t i = min(currentPointerCount, lastPointerCount); heapSize > 0 && i > 0; i--) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'uint32_t id = usedIdBits.markFirstUnmarkedBit();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (currentIdBits == lastIdBits) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

## 10. Viewport先建立natural surface，再由Affine、scale和rotation产出Display坐标

DIRECT 模式先选关联 `DisplayViewport`。代码按 viewport orientation 还原 natural logical/physical/device 尺寸，并用 physical crop 算出 `mRawSurfaceWidth/Height` 与 `mSurfaceLeft/Top/Right/Bottom`。若 natural physical 宽或高为 0，r48 只记录错误并把对应除数改为 1，不会禁用 Mapper；因此“有 viewport”仍不证明映射合理。

raw 轴宽高采用闭区间：`max-min+1`。随后：

| 参数 | 公式 |
|---|---|
| `xScale` | rawSurfaceWidth / rawWidth |
| `yScale` | rawSurfaceHeight / rawHeight |
| `xTranslate` / `yTranslate` | -surfaceLeft / -surfaceTop |
| orientation 0 | x=xScaled+xTranslate；y=yScaled+yTranslate |
| orientation 90 | x=yScaled+yTranslate；y=surfaceRight-xScaled |
| orientation 180 | x=surfaceRight-xScaled；y=surfaceBottom-yScaled |
| orientation 270 | x=surfaceBottom-yScaled；y=xScaled+xTranslate |

每点先对 raw X/Y 应用 descriptor 与 orientation 对应的 affine，再减轴 min、缩放并按 surface orientation 旋转。结果没有额外 clamp，校准或 crop 可以产生公开 range 外的坐标。这里得到的是关联 Display 坐标；Dispatcher 后面还会按 InputTarget 做窗口 offset/scale，不能把它叫作 View local。

### 练习 8：手算闭区间宽度与0/90度输出

设 X 轴 [100,1099]、Y 轴 [200,699]，affine 为 identity；rawSurfaceWidth=500、rawSurfaceHeight=1000、surfaceLeft=10、surfaceTop=20，因而 surfaceRight=510、surfaceBottom=1020。raw 点 (600,300) 在 orientation 0 与 90 时各输出什么？

唯一答案是 rawWidth=1000、rawHeight=500，xScaled=250、yScaled=200，translate=(-10,-20)。orientation 0 得 (240,180)；orientation 90 得 (180,260)。若误把 max 当宽度，或把 translate 再加进 90 度的 Y，都会得到不同错误结果。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'inline int32_t getRawWidth() const { return x.maxValue - x.minValue + 1; }' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h
grep -n -F 'inline int32_t getRawHeight() const { return y.maxValue - y.minValue + 1; }' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h
grep -n -F 'naturalPhysicalHeight = naturalPhysicalHeight == 0 ? 1 : naturalPhysicalHeight;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'naturalPhysicalWidth = naturalPhysicalWidth == 0 ? 1 : naturalPhysicalWidth;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mRawSurfaceWidth = naturalLogicalWidth * naturalDeviceWidth / naturalPhysicalWidth;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mSurfaceLeft = naturalPhysicalLeft * naturalLogicalWidth / naturalPhysicalWidth;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mXScale = float(mRawSurfaceWidth) / rawWidth;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mXTranslate = -mSurfaceLeft;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mAffineTransform.applyTo(xTransformed, yTransformed);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'rotateAndScale(xTransformed, yTransformed);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'y = mSurfaceRight - xScaled;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'x = mSurfaceBottom - yScaled;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
```

## 11. X/Y并非所有轴的统一模板，MultiTouch甚至不提供tilt输入

`cookPointerData()` 保持 pointerId 与 raw 数组索引，只重写坐标与轴语义。size 可按 GEOMETRIC、AREA、DIAMETER 或 BOX 解释：AREA 对 major 开平方并令 minor 相等，DIAMETER 直接令 minor=major，GEOMETRIC 乘平均 XY scale；`sizeIsSummed` 在 touching 数大于1时按点数分摊。pressure 的 PHYSICAL/AMPLITUDE 路径乘 scale，默认路径则把 hover 写0、touch 写1；这些结果没有统一钳到 0—1。

orientation 的 INTERPOLATED 路径线性换算，VECTOR 路径解码两个 4-bit 有符号分量并反向调整 major/minor。Display 旋转还会改写 orientation 范围。distance 只在 SCALED 校准下乘比例。与旧描述不同，r48 的 `MultiTouchInputMapper::configureRawPointerAxes()` 没有读取 `ABS_TILT_X/Y`，`syncTouch()` 又把每点 tiltX/Y 固定为0，所以真正的 tilt 角计算不会在这条 MT 路径活跃；不能因为 base 类有通用代码就宣称多点设备会输出硬件 tilt。

COVERAGE_BOX 更特殊：它从 toolMajor/toolMinor 的高低 16 位解包 raw left/top/right/bottom，再单独缩放、旋转到 GENERIC_1..4。位置 X/Y 会经过 affine，coverage 四边却没有套同一 affine，源码也明确留下这一缺口。由此，中心点校准正确不保证 coverage box 与它严格重合。

## 12. Raw门控发生在坐标烹制前，Cooked状态才进入动作差分

`cookAndDispatch()` 先把 Bluetooth stylus button 合入 raw buttonState，并在 initial down 或新 button press 时决定 WAKE；接着 `consumeRawTouches()` 用未经 affine 的 raw X/Y 做 surface/virtual-key 命中。若首次 touching 集合的最小 pointerId 落在 surface 外，即使没命中虚拟键，整帧也会被消费；命中键则走 Key 路，不再同时产生屏幕 Motion。affine 修好了显示坐标，并不会反向修正这次 raw 命中。

未被消费的 RawPointerData 才进入 cook。touching/hovering bits 与 ID 原样保留，PointerProperties 写 id/toolType，PointerCoords 写 X/Y、pressure、size、orientation 等。外接笔 pressure/toolType 在 cook 后再覆盖指定 ID；DIRECT 的 showTouches 读取 cooked spots，只是系统可视化，不是第二个 App 目标。

最后动作派发基于 `mLastCookedState` 与 current；本轮结束后才把 current raw/cooked 复制为 last，并清一次性 scroll。于是 last 表示已经走过完整 cook/dispatch 周期的状态，而非 Accumulator 最新值。reset 会同时清 pending/current/last，避免新配置后的第一帧与旧坐标系或旧手势做差。

## 13. touching ID集合差把一个硬件帧展开为UP、MOVE、DOWN

`dispatchTouches()` 只比较 cooked touching bits。前后集合完全相同且非空时，无条件发一笔 MOVE，即便坐标、properties 与 buttonState 都没变；下游 listener 才负责可能的 MOVE batching。集合不同时则计算：

| 集合 | 含义 |
|---|---|
| `up = last - current` | 本帧离开的 ID |
| `down = current - last` | 本帧新加入的 ID |
| `move = last ∩ current` | 本帧幸存的 ID |
| `dispatched = last` | 展开动作时当前对外仍按下的集合 |

源码先用 current properties/coords 更新 last 中的幸存点，再按 ID 升序逐个发 POINTER_UP。因此 UP 数组保留正在抬起点的旧数据，却已携带其他幸存点的本帧新位置。每发一笔 UP 就从 dispatched 删除该 ID。若幸存点真的变了，或 buttonState 变化，再用纯幸存集合补一笔 MOVE；最后按 ID 升序把新点逐个加入 dispatched 并发 POINTER_DOWN。一个 `SYN_REPORT` 因而可以展开为多个 UP、一笔 MOVE、多个 DOWN，而不是一帧等于一笔 Motion。

若第9节的 fallback 在同帧复用了离场 ID，集合差会把 replacement 吞成“ID 集合相同”的 MOVE，这说明动作层只能解释 Mapper 已分配的身份，不能重新识别物理接触。

## 14. pointerId升序决定数组与action index，downTime则跟随整条touch流

`dispatchMotion()` 从 BitSet32 反复取最小 ID，按 pointerId 升序复制 properties/coords；changedId 所在的输出位置才编码进 action 高位。因此 slot 次序、RawPointerData 数组次序和最终 pointer index 都可能不同。POINTER_UP 的数组仍包含 changedId，清除发生在下一笔；POINTER_DOWN 则已包含刚加入的 changedId。

输出 pointerCount 为1时，首个 POINTER_DOWN 归一化为 DOWN，最后一个 POINTER_UP 归一化为 UP。所有旧点已经展开抬起、随后第一个新点使 dispatched count 变成1时，`mDownTime` 重置为本帧 when；只要还有一个 survivor，新 DOWN 就沿用原手势 downTime。

DIRECT 且未 aborted 时，Motion 分支固定先发 BUTTON_RELEASE，再发必要的 HOVER_EXIT，然后执行 touch actions，再发 HOVER_ENTER/HOVER_MOVE，最后发 BUTTON_PRESS。第一次 hover 会连续产生 ENTER 与 MOVE；开始 touch 前会先 EXIT。这个顺序与同一时刻共享无关：每笔 Notify 都是独立事件，各有自己的 Reader event id。

## 15. NotifyMotion已是Display输入事实；视频帧却只归第一个展开动作

`dispatchMotion()` 最终填入 Reader event id、eventTime、logical deviceId、source、displayId、policyFlags、action/actionButton、flags、meta/button、edge、pointer 数组、precision、cursor position 与 downTime。Mapper 把 classification 初值写为 NONE；InputClassifier 可在 Reader 之后补分类。DIRECT 模式 cursor position 通常为 invalid，坐标已经面向 Display，但还没经过 Dispatcher 的目标窗口变换。

每次调用还会从 EventHub 取 video frames、按 surface orientation 旋转后塞进 Args。这个读取是 consume：`TouchVideoDevice::consumeFrames()` move 出整个队列并立即清空。因此同一 `SYN_REPORT` 展开多笔 Motion 时，只有第一笔调用 `dispatchMotion()` 能拿到此前积累的视频帧；后续 UP/MOVE/DOWN 不会复制同一批。第一笔可能是 button release、hover exit 或最小 ID 的 UP，并不保证是 MOVE。

`NotifyMotionArgs` 构造时复制本笔 pointer 数组与 frame vector，随后 `QueuedInputListener::notifyMotion()` 再复制堆对象；栈上 Args 返回后仍安全。真正跨出 Reader 锁的是本轮末尾的 flush。即便 flush 完成，也只表示下层 listener 已收到，不能越级证明 Dispatcher 命中窗口、socket publish、App FINISHED 或画面反馈。

### 练习 9：展开一帧动作、数组index、downTime与视频归属

last touching IDs={0,2}，current={1,2}；ID2 坐标改变，无 button/hover 变化，旧流 downTime=D，本帧 when=T，EventHub 恰有一批视频帧。问动作次序、每笔 ID 数组、changed index、downTime 和视频归属。

唯一答案是先 `POINTER_UP(index0)`，数组 [0,2]，其中 ID2 已是新坐标；再 `MOVE`，数组 [2]；最后 `POINTER_DOWN(index0)`，数组按 ID 升序为 [1,2]。ID2 始终幸存，所以三笔 downTime 都为 D，不重置成 T。三笔 eventTime 都为 T、Reader event id 各不相同；视频队列在第一笔 UP 被 consume，MOVE 与 DOWN 得到空 vector。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'BitSet32 upIdBits(lastIdBits.value & ~currentIdBits.value);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'BitSet32 downIdBits(currentIdBits.value & ~lastIdBits.value);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'BitSet32 moveIdBits(lastIdBits.value & currentIdBits.value);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'updateMovedPointers(mCurrentCookedState.cookedPointerData.pointerProperties,' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'while (!upIdBits.isEmpty()) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchedIdBits.clearBit(upId);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (moveNeeded && !moveIdBits.isEmpty()) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'while (!downIdBits.isEmpty()) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchedIdBits.markBit(downId);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (dispatchedIdBits.count() == 1) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'mDownTime = when;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'uint32_t id = idBits.clearFirstMarkedBit();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'action |= pointerCount << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'if (changedId >= 0 && pointerCount == 1) {' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'action = AMOTION_EVENT_ACTION_DOWN;' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchButtonRelease(when, policyFlags);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchHoverExit(when, policyFlags);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchHoverEnterAndMove(when, policyFlags);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'dispatchButtonPress(when, policyFlags);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'std::vector<TouchVideoFrame> frames = getDeviceContext().getVideoFrames();' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'frame.rotate(this->mSurfaceOrientation);' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'NotifyMotionArgs args(getContext()->getNextId(), when, deviceId, source, displayId, policyFlags,' frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp
grep -n -F 'return device->videoDevice->consumeFrames();' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'std::vector<TouchVideoFrame> frames = std::move(mFrames);' frameworks/native/services/inputflinger/reader/TouchVideoDevice.cpp
grep -n -F 'mFrames = {};' frameworks/native/services/inputflinger/reader/TouchVideoDevice.cpp
grep -n -F 'mArgsQueue.push_back(new NotifyMotionArgs(*args));' frameworks/native/services/inputflinger/InputListener.cpp
```

## 16. 诊断顺序：先查帧、身份和坐标，再追下游完成点

面对跳点、串指、偏移或手势不断，按下面顺序取证：

1. **先确定协议选择。** 看 tracking/slot axis 是否有效、slot min/max、B 的 slotCount 与 A 的 `SYN_MT_REPORT`；运行中见到 SLOT 不等于配置期一定选 B。
2. **还原 Accumulator。** 逐条记录 current slot、inUse 与稀疏字段，特别检查新接触是否重报必需值、非法 slot 和混入 B 流的 `SYN_MT_REPORT`。
3. **在 SYN 边界画完整 RawState。** 列 slot order、前16个非 palm 点、tool/hover、trackingId、raw 数组和 touching/hovering bits；别从单条 ABS 直接推 Motion。
4. **单独验证身份。** 先查 tracking 是否唯一稳定；一旦本帧回退，再按上一/pending 帧、同 toolType、raw 距离和 usedIdBits 手算，警惕无阈值匹配与离场 ID 即时复用。
5. **再算坐标。** 核对 inclusive raw range、natural viewport/crop、affine、scale/translate 和 rotation；X/Y 正确不代表 coverage 也过了 affine，Display 坐标也不是 Window local。
6. **用集合差展开动作。** 写出 up/move/down/dispatched，按 ID 升序演算每笔数组、action index 与 downTime；ID 集合相同会无条件生成 MOVE。
7. **最后追交付。** 检查 pending stylus 门、时间夹取、第一笔 Motion 对视频帧的 consume、Queued copy 与 Reader flush，再继续沿 Dispatcher/Channel/App 回执确认真正完成点。

稳定模型可以压成一句话：**slot 保存驱动的稀疏状态，SYN_REPORT 封口 RawState，tracking 或距离近似生成 pointerId，Viewport 与校准生成 Display 坐标，touching ID 集合差再展开 Android Action；任何一层的“完成”都不能代替下一层。** 第 250 章继续补齐 TouchInputMapper 控制面：模式选择、校准配置、虚拟键与外接 Stylus 融合怎样改变这条主链。
