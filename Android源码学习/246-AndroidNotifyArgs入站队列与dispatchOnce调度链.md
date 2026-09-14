# 246 Android NotifyArgs、入站队列与dispatchOnce调度链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一笔 Reader 事件何时才算进入 Dispatcher

第 245 章从 `DispatchEntry` 已经进入 Connection 的时刻向 App 追到 FINISHED；本章补齐它的上游。这里至少有六个不能互换的完成点：

| 完成点 | 能证明什么 | 还不能证明什么 |
|---|---|---|
| Mapper 调用 `notify*` | 一笔解析结果已经形成 | 结果已经离开 Reader 锁、已经进 Dispatcher |
| `QueuedInputListener` 深拷贝 | 栈上参数已有延寿副本 | 下游已经看到、Looper 已被唤醒 |
| `flush()` 调到 Dispatcher | 同步跨过 Reader/Classifier 边界 | 已经构造 `EventEntry`；Filter 可能接走它 |
| `enqueueInboundEventLocked()` | `EventEntry` 已在 inbound 尾部 | Dispatcher 线程已经选中它 |
| inbound 队首提升为 `mPendingEvent` | 当前调度对象已确定 | 已找到窗口、已经发给 App |
| `dispatch*Locked()` 返回 `true` | Dispatcher 不再需要把它留作 pending | App 已处理、FINISHED 已回来、对象已析构 |

硬件 Key/Motion 的正常主线是：`InputMapper → NotifyArgs → QueuedInputListener → InputClassifier → InputDispatcher.notify* → EventEntry → mInboundQueue → mPendingEvent → InputTarget → DispatchEntry`。这条箭头不是一次复制，也不是一条线程上的无锁直通；中间有两次所有权交接、一次可选异步 Filter 分叉，以及 Dispatcher 线程的重新取件。

本章要回答的唯一主问题是：**一笔由 Reader 解析出的事件，怎样在不携带 Reader 锁的前提下跨入 Dispatcher，随后在 wake、pending、policy、drop 与 target 选择之间得到一个明确结局？**

## 2. 三种对象、三种身份：Args 不是 Entry，Entry 也不是 DispatchEntry

`NotifyArgs` 是 Reader 到 listener 的调用参数基类，公共字段只有 `id` 与 `eventTime`，再以虚函数把不同子类分派到对应的 `notify*`。`NotifyKeyArgs` 携带设备、source、display、policy、action、keyCode 等值，但没有 `repeatCount`；`NotifyMotionArgs` 还拥有固定容量的 pointer 数组和 `videoFrames`。

`QueuedInputListener` 保存的是 `NotifyArgs*` 深拷贝。Key 的标量逐项复制；Motion 构造函数循环 `pointerCount` 复制 `PointerProperties`、`PointerCoords`，并复制 `videoFrames` 向量。因此 Mapper 传入的临时对象退出作用域后，队列副本仍自足。这个类本身没有队列锁，raw pointer 加固定 `count` 的实现依赖 InputReader 单线程、非重入使用约定；它不是通用并发消息队列。

Dispatcher 接到回调后又构造自己的 `KeyEntry` 或 `MotionEntry`。这一层继续复制需要用于路由和发布的字段；r48 的 `MotionEntry` 没有 `videoFrames`，视频帧只服务于前面的分类路径，不随 inbound entry 继续向 App 传输。硬件事件的 Args `id` 会传给 EventEntry；合成重复键和 Focus 则由 Dispatcher 自己取新 id。

最后，一个 EventEntry 可以按多个 `InputTarget` 产生多个 `DispatchEntry`。每个 DispatchEntry 有独立 transport `seq`，构造时给共享 EventEntry 增加一份引用。于是三种身份应这样读：

| 身份 | 作用域 | 主要用途 |
|---|---|---|
| NotifyArgs `id` | Reader 解析结果 | 串起 Args 与其硬件 EventEntry |
| EventEntry `id` | Dispatcher 逻辑事件 | 日志、验证、多个目标共享的事件身份 |
| DispatchEntry `seq` | 单个 Connection 的传输账 | 正向发布与反向 FINISHED 配对 |

`flush()` 删除 Args 不会删除 EventEntry；pending 释放一份 EventEntry 引用，也不代表各 Connection 的 DispatchEntry 已释放。

### 练习 1：数清一笔双指 Motion 的三次存活边界

设 Mapper 在栈上构造一笔 source 恰为 TOUCHSCREEN 的双指 `NotifyMotionArgs`，其中含两组坐标和一帧 `videoFrames`；Queued listener 收下后，Mapper 立刻改写原栈对象。`mMotionClassifier` 实例已经存在，Dispatcher 最终只得到一个普通前台 AS_IS target 和一个 monitor AS_IS target，不存在 portal、wallpaper、outside、split 或其他 mode；二者对应不同 Connection，均存在且 NORMAL，InputState 接受该流。问：哪些值不受原栈对象改写影响，视频帧最远走到哪里，会出现几个可入队的 transport seq？

唯一答案是：Queued 的整份 Args 副本均不受原栈对象改写影响；分类器再为 HAL 队列复制一次 Args，Dispatcher 的 MotionEntry 复制坐标与 classification、但不保存视频帧；两个 target 各建一个 DispatchEntry，所以有两个不同 seq，却共享同一 EventEntry id。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'struct NotifyArgs {' frameworks/native/services/inputflinger/include/InputListener.h
grep -n -F 'virtual void notify(const sp<InputListenerInterface>& listener) const = 0;' frameworks/native/services/inputflinger/include/InputListener.h
grep -n -F 'std::vector<NotifyArgs*> mArgsQueue;' frameworks/native/services/inputflinger/include/InputListener.h
grep -n -F 'mArgsQueue.push_back(new NotifyMotionArgs(*args));' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'videoFrames(other.videoFrames)' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'struct MotionEntry : EventEntry {' frameworks/native/services/inputflinger/dispatcher/Entry.h
grep -n -F 'const uint32_t seq;' frameworks/native/services/inputflinger/dispatcher/Entry.h
grep -n -F 'eventEntry->refCount += 1;' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
```

## 3. Reader 的两段锁区：getEvents 在中间，设备列表通知又在 flush 前

`InputReader::loopOnce()` 不是“持锁读设备，然后统一解锁”。准确时间线有五段：

1. 第一段 Reader 锁内读取 generation、配置刷新请求与下一次 timeout，算出 `timeoutMillis`。
2. 释放 Reader 锁，在 `EventHub::getEvents()` 中等待并取 RawEvent。
3. 第二段 Reader 锁内广播 alive、处理 RawEvent、处理 mapper timeout，并在 generation 改变时复制设备列表快照。
4. 再次释放 Reader 锁；若设备列表改变，先同步调用 policy 的 `notifyInputDevicesChanged()`。
5. 最后才调用 `mQueuedListener->flush()`。

Mapper 在第二段锁区里可以连续生成多个 Args。Queued listener 以 `push_back` 保持它们的生成顺序；`flush()` 先快照队列长度，再从下标 0 逐一同步调用 inner listener，每回调一笔便 `delete` 那个 Args，末尾 `clear()`。

“保持顺序”的边界只覆盖 `mArgsQueue` 内部。设备列表回调不在该队列中，所以同一轮里 policy 可能先获知新设备快照，Dispatcher 随后才收到该轮积累的 Configuration、DeviceReset、Key 或 Motion。不要把两个通道画成同一个全局 FIFO。

flush 仍由 InputReader 线程直接执行，没有切换到 Dispatcher 线程。把它放到锁外，是因为 listener 最终可能经 Dispatcher、WindowManager 再查询 Reader；锁内回调会形成 `Reader → Dispatcher/Policy → Reader` 的闭环。锁外解决的是 Reader 的锁环，不意味着所有下游组件都不持自己的锁。

### 练习 2：给一轮设备变化与两笔事件排唯一时间线

设第一段锁区得到 `timeoutMillis=20`；`getEvents()` 返回后，第二段锁区依次产生 Key K、Motion M，并使 generation 改变。问设备列表回调、K、M 的可观察顺序和执行线程。

唯一答案是：第一段锁释放 → `getEvents()` → 第二段锁内排入 K、M 并复制设备列表 → 第二段锁释放 → `notifyInputDevicesChanged()` → flush K → flush M；后三个调用都仍在 InputReader 线程，K 与 M 的 Dispatcher 回调不持 Reader 锁。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'size_t count = mEventHub->getEvents(timeoutMillis, mEventBuffer, EVENT_BUFFER_SIZE);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'processEventsLocked(mEventBuffer, count);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'getInputDevicesLocked(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mPolicy->notifyInputDevicesChanged(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mQueuedListener->flush();' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'size_t count = mArgsQueue.size();' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'args->notify(mInnerListener);' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'delete args;' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'mArgsQueue.clear();' frameworks/native/services/inputflinger/InputListener.cpp
```

## 4. Classifier 的异步只在 HAL 侧；下游 notify 仍同步且带着 classifier 锁

`InputManager` 的实际装配顺序是 Dispatcher 先被 Classifier 包住，Reader 再把 Classifier 当 listener：`Reader → InputClassifier → InputDispatcher`。Configuration、Key、Switch 直接透传；Motion 与 DeviceReset 有额外状态处理。

只有 source **恰好等于** `AINPUT_SOURCE_TOUCHPAD` 或 `AINPUT_SOURCE_TOUCHSCREEN` 的 Motion 才进入 `MotionClassifier`，不是任意带 pointer source class 位的事件。分类开启时，`InputClassifier::notifyMotion()` 持有自己的 `mLock`，复制 Args，调用 `classify()`，把返回值写入副本，再同步调用 Dispatcher。

`classify()` 对 DOWN 先把该 device 的缓存 classification 重置为 NONE，并记住 `downTime`；随后把 Args 副本压进容量为 5 的 HAL 工作队列，却立即返回当前缓存。HAL 线程稍后得到本事件的分类并更新缓存，通常影响后续 1—2 笔事件，而不是让当前事件停下来等待。若结果的 `eventTime` 早于该设备最新一次 `downTime`，它属于旧手势，会被丢弃；队列已满则清队并排入 HAL reset。

这里有两把不同的锁：MotionClassifier 的缓存锁保护分类表，外层 InputClassifier 的锁保护 classifier 对象。Reader 锁在 flush 前已经释放，Dispatcher 的 `notifyMotion()` 在 beforeQueueing 与 filter 回调时也会避免持有 Dispatcher 锁；但外层 `InputClassifier::mLock` 仍跨过整个下游 `mListener->notifyMotion()`。DeviceReset 同样持该锁：它立即删除该 device 尚未送 HAL 的队列项并排入 reset 请求，然后同步透传给 Dispatcher；HAL 真正执行 `resetDevice` 和清 classification 缓存可以更晚。Key 透传则不拿 classifier 锁。

### 练习 3：判断当前帧和下一帧各带什么 classification

设 device 7 进入新手势，DOWN 已把缓存清为 NONE；紧随其后的 MOVE A 入 HAL 队列时缓存仍为 NONE。A 已带 NONE 进入 Dispatcher 后，HAL 才对 A 返回 DEEP_PRESS，并在 MOVE B 到来前完成。问 A、B 进入 Dispatcher 时的 classification，以及 A 的 Dispatcher 回调持有哪些关键锁。

唯一答案是：A 携带 NONE，B 读取缓存后携带 DEEP_PRESS；两次回调都不持 Reader 锁，A 进入 Dispatcher 时仍持 InputClassifier 的 `mLock`，而 HAL 计算发生在另一线程。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mDispatcher = createInputDispatcher(dispatcherPolicy);' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mClassifier = new InputClassifier(mDispatcher);' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mReader = createInputReader(readerPolicy, mClassifier);' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'static constexpr size_t MAX_EVENTS = 5;' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'return args.source == AINPUT_SOURCE_TOUCHPAD || args.source == AINPUT_SOURCE_TOUCHSCREEN;' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'newArgs.classification = mMotionClassifier->classify(newArgs);' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'mListener->notifyMotion(&newArgs);' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'enqueueEvent(std::move(event));' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'return getClassification(args.deviceId);' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'if (eventTime < lastDownTime) {' frameworks/native/services/inputflinger/InputClassifier.cpp
```

## 5. 五种 NotifyArgs 在 Dispatcher 处分成四条路，Focus 来自第五个入口

不能用一条“所有 NotifyArgs 都进 inbound”概括接口：

| 输入 | Dispatcher 入口后的路径 | 是否构造 inbound EventEntry |
|---|---|---|
| ConfigurationChanged | 锁内构造 `ConfigurationChangedEntry` 并普通入队 | 是 |
| Key | validate、规范化、beforeQueueing、可选 Filter，允许后构造 `KeyEntry` | 有条件 |
| Motion | validate、beforeQueueing、可选 Filter，允许后构造 `MotionEntry` | 有条件 |
| Switch | 加 TRUSTED 后直接调用 policy `notifySwitch` | 否 |
| DeviceReset | Classifier 先处理设备状态，Dispatcher 构造 `DeviceResetEntry` | 是 |

Focus 不在 `InputListenerInterface`，也不存在 `NotifyFocusArgs`。它由 WindowManager 提交的新窗口快照触发，`setInputWindowsLocked()` 发现焦点 token 变化后直接构造 `FocusEntry`，走专用的前插函数。

Key/Motion 的校验失败会在 EventEntry 出现前返回；安装的 Filter 接走原事件时也不会入队。Switch 虽然经过 Queued listener 保序到达 Classifier/Dispatcher，却在 Dispatcher 处立即转给 policy，不占 inbound。因而“flush 已完成”无法推出“inbound 长度增加了相同数量”。

Motion 坐标也不能一概称为最终 display 坐标。带 viewport 的触摸/指针 Mapper 通常已产出逻辑 display 空间坐标，但 Joystick、Rotary 等 Motion 可以使用 `ADISPLAY_ID_NONE`；目标 display、窗口 offset、scale、split 都在后续路由与 DispatchEntry 阶段继续解析。

### 练习 4：计算一次 flush 后 inbound 到底增加几笔

设队列依次含 Configuration、Switch、非法 action 的 Key、被已安装 Filter 接走的 Motion、DeviceReset，Dispatcher 原先无 inbound；整个观察窗口内 Dispatcher 线程不消费，Filter 也尚未回送。问这五次原始 notify 直接造成的 inbound 新增顺序，以及 Switch、非法 Key、原 Motion 的去向。

唯一答案是：inbound 只新增 `[ConfigurationChangedEntry, DeviceResetEntry]`；Switch 已同步交给 policy，非法 Key 在 validate 处消失，原 Motion 被异步交给 Filter 且没有构造 MotionEntry。Filter 以后若回送事件，会作为另一条注入路径再入队。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'void InputDispatcher::notifyConfigurationChanged(const NotifyConfigurationChangedArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::notifyKey(const NotifyKeyArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::notifyMotion(const NotifyMotionArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::notifySwitch(const NotifySwitchArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::notifyDeviceReset(const NotifyDeviceResetArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->notifySwitch(args->eventTime, args->switchValues, args->switchMask, policyFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'new ConfigurationChangedEntry(args->id, args->eventTime);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'new DeviceResetEntry(args->id, args->eventTime, args->deviceId);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'FocusEntry* focusEntry =' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'virtual void notifyDeviceReset(const NotifyDeviceResetArgs* args) = 0;' frameworks/native/services/inputflinger/include/InputListener.h
```

## 6. Key 在入队前会改写身份；repeatCount 却要等到派发预处理

`notifyKey()` 先验证 action 只能是 DOWN 或 UP。随后发生四类规范化：

- policy 的 VIRTUAL 位与 Key flag 的 `VIRTUAL_HARD_KEY` 任一存在，另一侧也会被补齐。
- `POLICY_FLAG_FUNCTION` 会转成 `AMETA_FUNCTION_ON`。
- Reader 来源无条件补 `POLICY_FLAG_TRUSTED`；软件注入是否 trusted 由调用 uid/权限决定，不走这条无条件规则。
- Meta+Backspace 可改成 BACK，Meta+Enter 可改成 HOME；DOWN 时保存替换关系，UP 即使 Meta 已先松开，也按 device+原 keyCode 找回同一替换，保持上下行成对。

Dispatcher 用规范化后的字段先构造临时 native `KeyEvent`，同步调用 `interceptKeyBeforeQueueing()`。policy 通过增删 `PASS_TO_USER` 等 policyFlags 决定后续是否应到 App；这个回调名里的 “BeforeQueueing” 指 EventEntry 入队之前，而非 Mapper 队列之前。

`NotifyKeyArgs` 没有 repeatCount。硬件 notify 路径以局部常量 0 创建 KeyEntry。等该 entry 成为 pending，`dispatchKeyLocked()` 首次预处理才保存 repeat 状态：连续两个相同 keyCode 的 DOWN 被认作驱动重复，这里不再匹配 deviceId 或 source；第二笔改成上一笔 `repeatCount+1`，并关闭框架自己的重复计时。`repeatCount==1` 时补 LONG_PRESS。若超时由 Dispatcher 合成重复，则新 id、eventTime、repeatCount 会在 `synthesizeKeyRepeatLocked()` 中产生。

### 练习 5：连续两个硬件 DOWN 的 repeatCount 在哪一层变化

设两个可信、同 keyCode、初始 flags 不含 LONG_PRESS、policyFlags 不含 `POLICY_FLAG_DISABLE_KEY_REPEAT` 的 DOWN 连续到达，没有 UP，第二个在框架合成重复超时前成为 pending。列出两笔 NotifyKeyArgs、刚构造的 KeyEntry、第二笔完成派发预处理后的 repeatCount。

唯一答案是：NotifyKeyArgs 层根本没有该字段；两个新 KeyEntry 都以 0 构造；第一笔预处理后仍为 0，第二笔识别驱动重复后变为 1、补 LONG_PRESS，并把框架下一次重复时间置为无限远。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!validateKeyEvent(args->action)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr int32_t repeatCount = 0;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'policyFlags |= POLICY_FLAG_VIRTUAL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'metaState |= AMETA_FUNCTION_ON;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'policyFlags |= POLICY_FLAG_TRUSTED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'accelerateMetaShortcuts(args->deviceId, args->action, keyCode, metaState);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->interceptKeyBeforeQueueing(&event, /*byref*/ policyFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'entry->repeatCount = mKeyRepeatState.lastKeyEntry->repeatCount + 1;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'entry->flags |= AKEY_EVENT_FLAG_LONG_PRESS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mKeyRepeatState.nextRepeatTime = LONG_LONG_MAX;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 7. InputFilter 是异步改道，不是 notify 栈上的布尔变换器

Key/Motion 完成 early policy 后，Dispatcher 短暂加 `mLock` 检查 `mInputFilterEnabled`。若启用，它先释放 Dispatcher 锁，再调用 policy 的 `filterInputEvent()`。在常规已安装 Java Filter 的情形中，IMS 把事件经 `oneway IInputFilter` 发出，`InputFilter.filterInputEvent()` 又投递到指定 Handler；IMS 随即向 native 返回 `false`。于是原 Reader 调用栈直接结束：不构造 EventEntry、不进 inbound、也不 wake。

Filter 的 `onInputEvent()` 可以丢弃、拆分或改写流；默认实现会调用 host 把同一事件送回。host 使用 pid/uid 0、ASYNC 模式调用 `nativeInjectInputEvent()`，并强制 OR `FLAG_FILTERED`。这是另行发生的独立注入调用，甚至可与原 notify 尾部并发，不是原调用栈恢复执行。默认原样回送会保留 event id，但 native 注入构造 Key/Motion Entry 时把 deviceId 改成 `VIRTUAL_KEYBOARD_ID`，并补 INJECTED；uid 0 又使它带 TRUSTED。

`POLICY_FLAG_FILTERED` 在 r48 注入路径中的可见作用，是跳过第二次 `interceptKeyBeforeQueueing()` 或 `interceptMotionBeforeQueueing()`；注入路径本来就不会再调用 InputFilter。因此不能把这个位解释成“避免 Filter 递归”的条件判断。它还影响异步注入结果日志，但不把 injected 身份变回硬件身份。

Filter 安装状态切换也不是无缝改一枚 bool。`setInputFilterEnabled()` 在 Dispatcher 锁内调用 `resetAndDropEverythingLocked()`：向已有连接合成 CANCEL、释放 pending、排空 inbound、清 key repeat、无焦点等待、ANR tracker 和触摸状态，解锁后 wake。这是在逻辑流上划一条 CANCEL 边界，却不会 drain 各 Connection 已有的 outbound/wait；旧传输债仍可继续完成，而已经清空的 ANR 索引也不能再被误称为 waitQueue 镜像。

### 练习 6：跟踪 Filter 默认回送的两条调用栈

设 Java Filter 已安装，收到一笔合法硬件 Key，early beforeQueueing 保留 PASS_TO_USER；默认 `onInputEvent()` 原样回送，且回注入队时 inbound 为空、没有并发生产者。问 early beforeQueueing 执行几次，原事件与回送事件各自是否建 Entry、是否 wake。

唯一答案是：early beforeQueueing 只在原 Reader notify 栈执行一次；原事件被 Filter 接走，不建 Entry、不因它 wake。Handler 另行经 host 发起带 FILTERED 的异步注入，注入路径跳过第二次 beforeQueueing，构造 injected KeyEntry；id 保留，deviceId 变为 VIRTUAL_KEYBOARD_ID，policy flags 含 FILTERED、INJECTED、TRUSTED 及原有 PASS。因固定回注时 inbound 为空，这次注入得到 needWake=true，并在解锁后 wake；两条调用栈不靠“原 notify 已返回”建立顺序。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'oneway interface IInputFilter {' frameworks/base/core/java/android/view/IInputFilter.aidl
grep -n -F 'mH.obtainMessage(MSG_INPUT_EVENT, policyFlags, 0, event).sendToTarget();' frameworks/base/core/java/android/view/InputFilter.java
grep -n -F 'if (!mPolicy->filterInputEvent(&event, policyFlags)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return; // event was consumed by the filter' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInputFilter.filterInputEvent(event, policyFlags);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'policyFlags | WindowManagerPolicy.FLAG_FILTERED' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'if (!(policyFlags & POLICY_FLAG_FILTERED)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'resetAndDropEverythingLocked("input filter is being enabled or disabled");' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'drainInboundQueueLocked();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 8. 入队只做尾插；needWake 是三个源码条件，不是“应该醒”的猜测

`enqueueInboundEventLocked()` 在 Dispatcher 锁内先计算 `needWake = mInboundQueue.empty()`，再把 entry `push_back`。这个初值只看**入队前 inbound 是否为空**，不看 `mPendingEvent`：即使已有 pending 正在等 policy、焦点或 timeout，只要 inbound 当时为空，新 entry 仍会得到 true。这可能是一次冗余 wake，却让生产者不必推断消费线程当前睡眠状态。

随后只有两类优化能把 false 强制改为 true：

1. 可信、PASS_TO_USER、未取消的 HOME/ENDCALL/APP_SWITCH key UP，在此前见过任一 app-switch DOWN 后，把 due time 设为该 UP 的 `eventTime+500ms`。
2. 新 pointer DOWN 满足 prune 条件时，把它记为 `mNextUnblockedEvent`。

普通 Key、Configuration、DeviceReset 在非空 inbound 后追加时不会额外强制 wake。pointer DOWN 若只是把 `mKeyIsWaitingForEventsTimeout` 改成 `now()`，该子分支自身也不把 needWake 置 true；是否醒仍取决于 inbound 原本为空或别的条件。

调用者离开 Dispatcher 锁后才执行 `mLooper->wake()`。这样 wake 是“状态已经可见”的门铃，不是装载事件的容器。Focus 不走这个函数；`setInputWindows()` 完成整批窗口/焦点更新并解锁后无条件 wake。

### 练习 7：四种入队分别会不会 wake

分别判断：A. inbound 空但已有 pending；B. inbound 非空，追加普通 Key；C. inbound 非空，已见 app-switch DOWN 后追加合格 UP；D. inbound 非空，追加满足 prune 的 pointer DOWN。

唯一答案是：A=true，因为初值只看 inbound；B=false；C=true，由 app-switch 分支强制；D=true，由 prune 分支强制。四者都会先在锁内完成尾插，再由调用者于锁外决定是否敲 Looper 门铃。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'bool needWake = mInboundQueue.empty();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInboundQueue.push_back(entry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAppSwitchDueTime = keyEntry.eventTime + APP_SWITCH_TIMEOUT;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mNextUnblockedEvent = entry;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mKeyIsWaitingForEventsTimeout = now();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'needWake = true;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'needWake = enqueueInboundEventLocked(newEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLooper->wake();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'enqueueFocusEventLocked(*newFocusedWindowHandle, true /*hasFocus*/);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 9. dispatchOnce 外壳先看 command，再做 ANR，最后才 poll

Dispatcher 线程每轮调用 `dispatchOnce()`，以 `nextWakeupTime=LONG_LONG_MAX` 开始并持有 `mLock`。它的顺序不是单纯“取一个 inbound”：

1. 若进入本轮时 command queue 非空，跳过 `dispatchOnceInnerLocked()`；否则内层可以选择或推进一笔 pending，并可能新增 command。
2. `runCommandsLockedInterruptible()` 排空 command queue。函数名中的 Locked 表示调用前后持锁；具体 command 可以临时 unlock，同步调 policy，再 lock 回来。
3. 只要执行过 command，便把下一次 wakeup 设为 `LONG_LONG_MIN`，促成立即进入下一轮。
4. 无论内层是否运行，都调用 `processAnrsLocked()`，把无焦点窗口与 Connection wait 的最早超时合入 wakeup；它若此时新 post ANR command，排空 command 的位置已经过去，该命令留给立即到来的下一轮。
5. 只要 `nextWakeupTime` 最终仍为 `LONG_LONG_MAX` 就通知 idle；这里没有再次检查 pending/inbound。尤其 frozen 让内层直接返回时，即使队列尚有内容也可能满足该条件，所以 idle 更准确地表示“本轮没有计划一个有限唤醒时刻”，不是所有容器必为空。解锁后再把绝对时间换成毫秒并进入 `mLooper->pollOnce()`。

所以 command 对“本轮选择新事件”有优先权，但执行后强制立即再转一轮；它不是另起的 worker，也不能据此推出事件永久饥饿。

`mDispatchFrozen` 只让 `dispatchOnceInnerLocked()` 提前返回：app-switch、stale、key repeat、pending 路由等内层进度暂停。外层已有 command 仍会运行，`processAnrsLocked()` 仍会检查无焦点与 waitQueue ANR，Looper 也仍可处理 fd/wake 回调。源码注释中的“不处理 timeout”不能扩写成 Dispatcher 世界的所有超时都停止。

## 10. 从 inbound 到 pending：一次只提升一笔，false 会让它跨轮存活

只有 `mPendingEvent==nullptr` 时，内层才从 inbound 队首 pop 一笔。若 inbound 为空但 key repeat 状态到期，则可能合成一笔重复键直接成为 pending；若尚未到期，只缩短 nextWakeupTime。

新 pending 带 `PASS_TO_USER` 时会调用 `pokeUserActivityLocked()`，但不保证产生 command：Focus、已取消 Key、Motion CANCEL、或 focused window 禁用 user activity 都会早退；符合条件时才 post。接着初始化 `done=false` 和 `dropReason=NOT_DROPPED`，根据 policy、dispatch mode 与事件类型继续推进。处理函数返回 false 时，pending 指针原样保留，下一轮不会再从 inbound 取新事件；这正是顺序屏障。

false 常见于三类等待：

- Key 首次进入 `dispatchKeyLocked()`，需要执行 `interceptKeyBeforeDispatching` command。
- policy 返回正 delay，尚未到 `interceptKeyWakeupTime`。
- 目标选择返回 `INPUT_EVENT_INJECTION_PENDING`，例如 focused application 已有但 focused window 尚未出现、窗口 paused，或 Key 发现 `mAnrTracker` 中仍登记着全局已 publish 账。这项检查不按 FOREGROUND、display、window、事件类型或与当前 Key 的先后关系过滤；记录常见于 Motion，也可以来自 Key、Focus 或 monitor。未登记、已因 ANR 按 token 擦除的 wait 欠账不会让这个谓词保持 true。

三种 PENDING 的定时保障也不同：无 focused window 会写自己的 ANR deadline；Key 因 tracker 非空而等待时使用一个 500ms timer；paused window 只是返回 PENDING，本分支不设置 nextWakeupTime，要依靠窗口状态更新等外部 wake。pending 每轮都会重新计算 dropReason，所以等待期间新出现的 app-switch due、事件变 stale 或 unblocked marker 也可能改变它的结局。

当函数返回 true，含义只是这笔 pending 在 Dispatcher 的“选目标/决定丢弃”阶段已经结束。若目标解析成功，`dispatchEventLocked()` 会尝试为每个仍存在且 NORMAL 的目标 Connection 准备 DispatchEntry；目标在此刻消失或 Connection 已非 NORMAL 时可以一笔也没有。随后 pending 自己的引用可以释放。App 的处理完成点属于第 245 章的 wait/FINISHED 链，不能由 `done=true` 代替。

## 11. dropReason 是有优先级的单选结果，特殊事件会覆盖为不丢

Key/Motion 的初始丢弃判定先看 `PASS_TO_USER`，没有则是 POLICY；否则若 dispatch disabled，则是 DISABLED。后续只有仍为 NOT_DROPPED 才能写入较低优先级原因，得到严格顺序：

`POLICY > DISABLED > APP_SWITCH > STALE > BLOCKED`。这是 `dispatchOnceInnerLocked()` 在进入 Key/Motion 处理前的判定优先级；Key 后来从 beforeDispatch 得到 SKIP，只会在原 dropReason 仍为 NOT_DROPPED 时补 POLICY，不会反过来覆盖已确定的 DISABLED、APP_SWITCH、STALE 或 BLOCKED。

Key 的 beforeDispatch policy 若返回负 delay，会把 intercept 结果设为 SKIP；若此前还没有别的 dropReason，再转成 POLICY。被 POLICY 丢掉的注入事件，其 injection result 记作 SUCCEEDED，表示 policy 已合法消费；DISABLED、APP_SWITCH、STALE、BLOCKED 则记作 FAILED。硬件事件没有 InjectionState，这项写入是 no-op。

dropReason 先算出，也不保证 Key 立刻执行 drop。`dispatchKeyLocked()` 的顺序是先做 repeat/LONG_PRESS 预处理，再处理 beforeDispatch command，最后才进入“若要丢弃”的清理分支。只要它仍带 PASS_TO_USER，DISABLED、APP_SWITCH、STALE、BLOCKED 的 Key 都可能先经历一次 policy command 或正 delay；Motion 没有这层延迟。

丢弃并非简单 delete。`dropInboundEventLocked()` 对 Key 请求全局 non-pointer CANCEL；对 pointer Motion 请求 pointer CANCEL，对其他 Motion 请求 non-pointer CANCEL，以闭合已经送达连接的输入状态。但“全局请求”不等于每条连接必有一笔 CANCEL：遍历会跳过 BROKEN Connection，且每条 Connection 只有在 InputState 尚存匹配的已下发状态时才合成。合成项直接进入该 Connection 的 DispatchEntry/发送周期，绕过 inbound、drop 与 recent，publish 仍可能失败。

drop 也不重置 key repeat：一笔可信且未带 `POLICY_FLAG_DISABLE_KEY_REPEAT` 的 DOWN，即使随后因 POLICY、APP_SWITCH、STALE 或 BLOCKED 被丢，也可能已经在预处理里留下 `lastKeyEntry` 与 repeat timer，之后仍有机会合成重复键，直到 UP、禁用、配置变化或其他 reset 路径清理。CANCEL、pending release 与 repeat state 是三本账。

Filter 接走的原事件从未形成 EventEntry，因此不会来到这个函数；Java Filter 文档要求 Filter 自己维护流一致性并在需要时回送 CANCEL。

ConfigurationChanged、DeviceReset、Focus 在 type switch 里都把 dropReason 强制改回 NOT_DROPPED。Configuration 会重置 key repeat 并投递 policy command；DeviceReset 会按 device 对全部连接合成 CANCEL；Focus 走目标 token 的专用派发。它们不会因为 `PASS_TO_USER` 或 disabled 被普通 drop 流吞掉。由 enabled 切到 disabled 时，已有 pending/inbound 会先被 `resetAndDropEverythingLocked()` 清掉；DISABLED 原因主要评价切换后新到达或仍被选中的输入，而不是让切换前旧积压逐笔自然落入该分支。

还要把 target lookup 失败与五种 dropReason 分开。Key 找不到目标、权限拒绝或无焦点超时，会在写 injection result 后结束，却不调用全连接 `dropInboundEventLocked()`；Motion 权限拒绝也直接结束，其他非成功 target result 只向 monitors 合成对应 CANCEL。只有调用点明确带着非 NOT_DROPPED 原因，才走前述全连接取消矩阵。

## 12. APP_SWITCH、STALE 与 BLOCKED 都在清旧账，但时间轴不同

APP_SWITCH 优化只认未取消、TRUSTED、PASS_TO_USER 的 HOME、ENDCALL、APP_SWITCH。合格 DOWN 只置一个 bool；随后任一合格 app-switch UP 都可把 due 设为自己的 `eventTime+500ms`，这里不按 keyCode、deviceId 或 source 保存成对状态。由于使用事件时间而不是入队时刻，一笔滞留很久的 UP 可能刚入队就已经 due。

due 后，队首普通 Key/Motion 会被标 APP_SWITCH 并逐笔丢弃；当 app-switch Key 自己成为 pending，timer 会以 handled=true 重置，但这只清 due，不保证该 Key 最终送达：它仍可能保留此前的 DISABLED，随后命中 STALE、BLOCKED，或被 beforeDispatch SKIP。只有在 `mPendingEvent==nullptr` 且 inbound 已空时，due 才会被视作等不到切换键并以 handled=false 放弃。

`resetAndDropEverythingLocked()` 并不清 `mAppSwitchSawKeyDown` 或 `mAppSwitchDueTime`，这是 r48 的独立残留状态；不能因为 pending/inbound 被排空，就推断 app-switch 识别账也归零。

STALE 独立比较 `currentTime-entry.eventTime >= 10s`，只在没有更高原因时生效。它衡量事件从发生到当前调度的年龄，不是 inbound 停留时间，也不是 App delivery wait。Dispatcher 没有把 `eventTime+10s` 登记进 nextWakeupTime；只有因 command、fd、其他 deadline 或显式 wake 再进入 inner 时才检查，因此 paused/frozen 或缺少外部唤醒时不会仅为 stale 阈值自行醒来。

BLOCKED 来自 `mNextUnblockedEvent`。当 Dispatcher 正等某 focused application、而用户的新 pointer DOWN 命中的 window applicationToken 与 awaited token 不同（命中 token 为 null 也算不同），或仍有 responsive gesture monitor 能接手时，新 DOWN 被尾插并成为 marker。所有在它前面的 Key/Motion 到达 pending 时，只要没有更高原因，都会以 BLOCKED 丢弃；Configuration、DeviceReset、Focus 不受此普通分支影响。marker 自己被提升为 pending 后，代码先清指针再检查 type，因此它不会把自己丢掉。release/reset 也会清理指针。后来的合格 DOWN 会直接覆盖 marker，于是旧 marker 也可能变成新 marker 之前的 BLOCKED 对象。

若只触发“Key 因 tracker 非空而等待”优化，pointer DOWN 会把 `mKeyIsWaitingForEventsTimeout` 提前到 now，却不会成为 marker。一个是让 pending Key 尽快继续，另一个是清掉 marker 前的 Key/Motion，不能混称为 prune。

### 练习 8：按优先级推演 marker 前后的四笔事件

设 `mNextUnblockedEvent=M2`，处理顺序为：带 PASS 但年龄 11 秒的 K1、Configuration C、同时无 PASS 的 M1、marker M2；dispatch enabled，app-switch 未 due。K1 已是 `dispatchInProgress=true` 且 intercept 结果为 CONTINUE；M2 带 PASS、fresh，能成功找到目标且 `conflictingPointerActions=false`。问各自结局。

唯一答案是：K1 先命中 STALE，而不是 BLOCKED；C 强制 NOT_DROPPED 并执行配置路径；M1 命中更高优先级 POLICY，而不是 BLOCKED；M2 提升为 pending 时先清 marker，因此不因 BLOCKED 被丢，正常进入 Motion 目标选择。三笔接受普通 drop 判定的输入是 K1、M1、M2，其中实际被丢的是 K1、M1；C 是控制事件，另走强制不丢分支。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr nsecs_t STALE_EVENT_TIMEOUT = 10000 * 1000000LL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (mDispatchFrozen) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DropReason dropReason = DropReason::NOT_DROPPED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropReason = DropReason::POLICY;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropReason = DropReason::DISABLED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropReason = DropReason::APP_SWITCH;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropReason = DropReason::STALE;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropReason = DropReason::BLOCKED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (mNextUnblockedEvent == mPendingEvent) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dropInboundEventLocked(*mPendingEvent, dropReason);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'addRecentEventLocked(entry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 13. Key 的 dispatchInProgress 只防止重复预处理，policy command 后必须重查

Key 首次进入 `dispatchKeyLocked()` 时处理 repeat/LONG_PRESS，随后把 `dispatchInProgress=true`。若 `interceptKeyResult` 还是 UNKNOWN 且 PASS_TO_USER，它保存当时 focused window 的 channel、给 KeyEntry 增加一份引用、post `doInterceptKeyBeforeDispatchingLockedInterruptible`，然后返回 false。

外层在同一 `dispatchOnce()` 里排空 command。command 从 KeyEntry 构造 KeyEvent，释放 Dispatcher 锁，同步调用 policy，再重新加锁，把结果归为：负数 SKIP、0 CONTINUE、正数 TRY_AGAIN_LATER，并以 `now()+delay` 保存绝对唤醒时刻，最后释放 command 持有的引用。因为执行过 command，外层强制下一轮立即醒来；若解锁期间 reset 已把原 pending 移走，command 的额外引用只保证内存安全，下一轮会依据最新队列状态，而不会强行恢复旧 pending。

若解锁期间没有 reset 或其他状态更新把它移走，下一轮仍是同一 pending。`dispatchInProgress` 避免 repeat 状态、日志与首次初始化再跑；它不表示 target 已找到。如果结果是 TRY_AGAIN_LATER 且时间未到，函数继续返回 false，并把 nextWakeupTime 缩到 policy deadline；到点后把结果清回 UNKNOWN，再投一条新的 policy command。这允许 policy 多次延期。

CONTINUE 后才调用 `findFocusedWindowTargetsLocked()`。无 focused window 但有 focused application 时，pending 等待并建立 no-focused-window timeout；窗口 paused 也等待；Key 还会在第一次发现 `mAnrTracker` 中仍有登记项时设置一个 `currentTime+500ms` 的绝对 deadline，之后复用它。线程若迟到，实际墙钟等待可超过 500ms；检查对象也不按 FOREGROUND、display、window、类型或因果先后筛选，Motion、Key、Focus、monitor 的登记都可能让 tracker 非空。最终 result 非 PENDING 才写 InjectionState。本条 CONTINUE 路径的 SUCCEEDED 表示目标选择成功，实际 DispatchEntry、publish、wait 与 FINISHED 仍在后面。

## 14. Motion 没有 beforeDispatch command；done、result 与释放仍是三回事

Motion 首次进入只把 `dispatchInProgress` 置 true。pointer 类事件走 touched-window 解析，非 pointer Motion 走 focused-window 解析；PENDING 同样保留当前 pending。权限拒绝、无目标、无焦点超时等会结束这一级，但各自的 cancel 范围并不完全相同。

目标解析成功后，Dispatcher 增加全局 monitor；pointer 事件还可能加 portal display 的 monitor。若检测到冲突 pointer action，会先向所有 Connection 合成 pointer CANCEL，再派发当前 Motion。`dispatchEventLocked()` 逐 target 找 Connection，调用 `prepareDispatchCycleLocked()`；不存在或非 NORMAL 的 Connection 可以不产生 DispatchEntry，函数仍可结束当前 pending。

因此三个量必须分开：

| 量 | 真正含义 |
|---|---|
| `dispatchInProgress` | 此 EventEntry 的一次性派发预处理已经开始 |
| `injectionResult=SUCCEEDED` | policy 合法消费，或 target-selection 成功；两者都不证明已建 DispatchEntry、publish 或 FINISHED |
| `done=true` | 当前 EventEntry 不必继续占着 `mPendingEvent` |

done 后 `releasePendingEventLocked()` 会调用统一的 `releaseInboundEventLocked()`：尚为 PENDING 的注入状态改 FAILED，若它正是 unblocked marker 则清指针，把 entry 加入最多 10 笔的 recent queue，再释放 pending 引用。recent 与各 DispatchEntry 的引用都可能让 EventEntry 继续存活；“离开 pending”不是“对象析构”。

## 15. Configuration、DeviceReset 与 Focus：三个永不普通丢弃的控制事件

Configuration Entry 成为 pending 后重置 key repeat，post 一个锁外 policy notification command，然后 done。DeviceReset 按 deviceId 向所有 Connection 合成 `CANCEL_ALL_EVENTS`，然后 done。两者来自 Reader 的 NotifyArgs，并以普通尾插进入 inbound。

Focus 完全不同。窗口快照改变时，旧焦点若仍有 channel，先合成 non-pointer CANCEL，再排一笔 `hasFocus=false`；新焦点随后排 `hasFocus=true`。`enqueueFocusEventLocked()` 若发现已有 pending，会先把 pending 压回 inbound 前端并清空 pending；随后把 Focus 插在所有既有 Focus 后面、其他事件前面。连续失焦/得焦的顺序得以保持，而新焦点可先于被退回的普通 pending 生效。

Focus 被选中后按 connection token 找 channel；channel 已消失便直接结束，不产生 DispatchEntry。存在 channel 时只建立 `DISPATCH_AS_IS` target，不带 FOREGROUND。若变化发生在当前 focused display，Dispatcher 还会 post 一条 `notifyFocusChanged(old,new)` policy command；command queue 的外层优先级通常使这条 policy 回调先于客户端 FocusEntry 派发。

正常发布到客户端后，native receiver 创建 native FocusEvent，直接调用 Java `onFocusEvent(hasFocus,inTouchMode)`，不构造 Java InputEvent、不进 `mSeqMap` 和 ViewRoot InputStage。ViewRoot 回调只更新 upcoming 字段并投递 `MSG_WINDOW_FOCUS_CHANGED`；JNI 随后立即以 handled=true 调 finish helper，所以服务端 FINISHED 可能早于 Handler 真正执行 `handleWindowFocusChanged()`。

若反向 socket WOULD_BLOCK，通用 helper 会尝试把 `(seq,true)` add 到 `mFinishQueue`，并在队列 size 为 1 时监听可写；它没有检查 add 的返回值。其他 send error 会记录 warning 并返回。Focus 分支没有依据这个返回值改变已经完成的 Java 回调流程。即使 `onFocusEvent()` 留下 Java 异常，C++ 源码也会继续走紧随其后的 finish 调用。

### 练习 9：Focus 抢到 pending 前面后，哪个完成点最早出现

设 inbound 原为 `[X]`、pending 为普通事件 P；当前 focused display 的一次窗口更新依次产生旧焦点 F−、新焦点 F+。两个 channel 都存在，更新前 command queue 为空；观察期间对应 Connection 一直保持 NORMAL 且不被移除。客户端后来收到 F−，`onFocusEvent()` 正常投递 ViewRoot Handler 消息，首次 FINISHED 写返回 WOULD_BLOCK，且 `mFinishQueue.add()` 成功。问新的 inbound 顺序、下一轮谁先执行，以及此时 ViewRoot 消息、native finish queue、服务端 wait 的状态。

唯一答案是：第一次专用前插先退回 P，再插 F−；第二次把 F+ 插在 F− 后，因此队列为 `[F−, F+, P, X]`。同时已有 `notifyFocusChanged` command，下一轮先执行它而不取 F−。客户端回调后 Handler 消息已排队但可尚未执行；`(seq,true)` 已进入 native `mFinishQueue`；服务端 wait 仍保留该 DispatchEntry，直到可写回调真正发出 FINISHED 并由 Dispatcher 收到。Java `mSeqMap` 从未参与 Focus。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mInboundQueue.push_front(mPendingEvent);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPendingEvent = nullptr;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'event->type == EventEntry::Type::FOCUS' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInboundQueue.insert(it.base(), focusEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (channel == nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'onFocusChangedLocked(oldFocusedWindowHandle, newFocusedWindowHandle);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!haveCommandsLocked()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'target.flags = InputTarget::FLAG_DISPATCH_AS_IS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'case AINPUT_EVENT_TYPE_FOCUS: {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'finishInputEvent(seq, true /* handled */);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'mFinishQueue.add(finish);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'windowFocusChanged(hasFocus, inTouchMode);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'msg.what = MSG_WINDOW_FOCUS_CHANGED;' frameworks/base/core/java/android/view/ViewRootImpl.java
```

## 16. 诊断顺序：先辨线程与对象，再问卡在哪个完成点

遇到“Reader 已读到事件，App 为什么没收到”，按下面顺序查，能避免跨层猜测：

1. 先确认 Mapper 是否形成合法 Args；所有类型记录 Reader id、eventTime，只有具备相应字段的 Key/Motion/Switch 再记录 source、display 或 policyFlags，不能把子类字段套给 NotifyArgs 基类。
2. 看 Args 是否只在 Queued listener 等 flush；把 `notifyInputDevicesChanged` 与队列 FIFO 分开。
3. 对 Motion 检查是否进入 classifier、当前携带的是哪一笔缓存分类；同时标出 Reader 锁已释放、classifier 锁仍可能持有。
4. 在 Dispatcher notify 处分流：validate return、early policy、Filter 接走、Switch 直达 policy，还是已经构造 EventEntry。
5. 若走 Filter，分别追原硬件调用与稍后的 FILTERED 注入，不能把两条调用栈拼成同步返回。
6. 已入队则按源码三条件判断 needWake；wake 只证明门铃被敲，不证明 entry 已成为 pending。
7. 看 command queue 是否使内层本轮跳过，以及 frozen 只冻结了哪些内层工作。
8. pending 不动时看 `dispatchInProgress`、intercept result、no-focused-window、paused，以及 Key 因 `mAnrTracker` 仍有全局已 publish 登记而使用的 500ms deadline。
9. 被丢时按 `POLICY > DISABLED > APP_SWITCH > STALE > BLOCKED` 找第一个原因，并检查合成 CANCEL；控制事件另算。
10. 最后区分 target 成功、pending 释放、Connection publish、App FINISHED 与 EventEntry 最后一份引用释放。

r48 的上游闭环可以压成一句话：**Reader 在锁内把 Mapper 结果深拷贝排队，锁外仍在原线程同步 flush；Key/Motion 经校验、policy 与可选异步 Filter 后才拥有 EventEntry，入队与 wake 分离，Dispatcher 再用 pending 把命令、等待、丢弃与目标选择串行化，而 done 只结束这一层调度，不替代 App 完成。**

至此，第 246 章已经接上第 245 章的入口：成功解析出的 target 只有在对应 Connection 仍存在且 NORMAL 时，才进入 prepare；此后还要经过 split、mode 与 InputState 条件，才可能形成 DispatchEntry 并进入 Connection。下一章转向 `InputReader` 与 `InputDispatcher` 两条线程本身，统一解释 EventHub、Looper、wake、Command 与跨 Java 回调之间的锁顺序并发链。
