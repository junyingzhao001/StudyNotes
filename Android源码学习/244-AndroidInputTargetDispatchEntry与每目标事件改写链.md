# 244 Android InputTarget、DispatchEntry与每目标事件改写链

本文以 `android-11.0.0_r48` 为唯一源码基线，沿 `InputTarget`、`splitMotionEvent()`、`createDispatchEntry()`、`InputState` 与 `InputPublisher` 追踪一份 Motion 输入如何变成接收者专属视图。这里不再讨论窗口为什么被命中，也不展开 socket 背压与 FINISHED 出队；前者属于第236、241、243章，后者留给第245章。

主线只问一件事：**一份已经选好接收者的 `MotionEntry`，怎样按每个目标的 pointer、mode、geometry、安全标志与完成责任，派生出应用最终看到的 action、pointer 集合、坐标、flags、eventId，以及用于回执的 seq？**

阅读时先记住一个反直觉事实：`DispatchEntry` 不是“原事件加一个窗口地址”。default-info 路径可以直接引用当前 `MotionEntry`；per-pointer 路径会为每个命中 mode 新建一份统一窗口几何的 combined entry。正常 split 目标属于后一类：临时 split entry 只提供筛选后的 pointer、action 与 ID，最终由 combined entry 承接；`DispatchEntry` 随后再独立固化 `resolvedAction`、`resolvedFlags`、`resolvedEventId` 与 `seq`。

## 1. 主问题：同一输入事实为什么不能直接复制给所有目标

假设一笔非 mouse 触摸分两步发生：指针 2 先以首个 `DOWN` 落在同时启用 wallpaper 且支持 split touch 的窗口 A；这一时点，上层观察窗 C 只收到 `OUTSIDE`，wallpaper W 与 gesture monitor G 也各有自己的视图。随后指针 7 以 `ACTION_POINTER_DOWN` 落到同样支持 split touch 的窗口 B，A 与 B 得到不同 pointer 子流。若在每个时点都把当笔 `MotionEntry` 原样交给它的全部目标，会同时破坏五类契约：

| 契约 | 每个目标可能不同的内容 | 原样复制的后果 |
|---|---|---|
| pointer 所有权 | A、B 只看自己的 pointer ID 子集 | App 看见不属于自己的手指 |
| action 序列 | B 的第一根指针必须从 `DOWN` 开始；A 可能只看见 `MOVE` | `InputState` 与客户端手势状态机失配 |
| 坐标 | 窗口 frame、window scale、portal offset 可不同 | 同一 channel 内多 pointer 无法用一组 offset/scale 正确表达 |
| 安全视图 | `OUTSIDE` 可清零坐标，遮挡位按目标添加 | 旁观者得到不该知道的位置，或敏感目标丢失风险证据 |
| 完成责任 | foreground 目标计入同步注入，观察目标通常不计入 | `WAIT_FOR_FINISHED` 的等待范围错误 |

r48 因而把工作分成四步：路由层先给每个接收连接写 `InputTarget`；必要时按 pointer 子集创建 split `MotionEntry`；再按 dispatch mode 创建一笔或多笔 `DispatchEntry`；最后在 publish 时应用剩余的坐标参数并写入 `InputMessage`。

四个完成点不能混称“事件已发送”：

| 完成点 | 源码事实 | 仍不能证明 |
|---|---|---|
| 目标计划完成 | `inputTargets` 已构造 | connection 仍存在、序列会被接受 |
| entry 入队完成 | `trackMotion()` 通过并进入 outbound queue | socket 已接纳、App 已读取 |
| publish 完成 | `sendMessage()` 成功并转入 wait queue | App 已处理、FINISHED 已返回 |
| connection 结账完成 | 对应 seq 的 entry 被释放 | `handled=true`；Motion 的完成不要求这一结论 |

本章的责任边界停在 publish 参数与完成责任已经确定；下一章再拆 outbound、wait、`WOULD_BLOCK`、ANR 与 FINISHED。

## 2. 三类对象与四种基数：计划、事件视图、投递凭据、连接

`InputTarget` 是一次 dispatch 决策的临时计划。它保存 `inputChannel`、内部 `flags`、`globalScaleFactor`、`pointerIds`，以及按 pointer ID 索引的 `PointerInfo`。它不保存 outbound/wait 状态，也没有自己的 timeout 字段。

`DispatchEntry` 是一笔目标化投递凭据。它保存一个 `EventEntry*`、内部 target flags、一组最终公共 offset/scale、三个 `resolved*` 字段和非零 `seq`。结构体本身没有 `Connection*`；它属于哪条连接，是由它被放进哪一个 `Connection::outboundQueue` 或 `waitQueue` 决定的。

`MotionEntry` 则有三种可能来源：

- 根 entry：Reader 或注入入口形成的完整输入事实；
- split entry：删去目标不拥有的 pointer，并可能改写 pointer action/index；
- combined entry：pointer 集合不再变化，只为同一 connection 的多套窗口几何重写 raw coords。

这些结构也服务于 Key 与 Focus，但本文后续所有 action、pointer、坐标、HMAC 和编号推演都限定为 Motion。Key 会在自己的分支填写 resolved 三元组；Focus 分支不依赖这组三元组，publish 直接读取 `FocusEntry.id/hasFocus`。不能把 Motion 的“显式 mode 换 resolved ID”外推到所有 `EventEntry::Type`。

因此对象基数不是简单的 1:1：

| 起点 | 终点 | 基数 | 原因 |
|---|---|---|---|
| 根 `MotionEntry` | `InputTarget` | 1:N | 前景、outside、wallpaper、monitor 或多个 split 窗口 |
| `InputTarget` | split `MotionEntry` | 1:0/1 | 只有 `SPLIT` 且目标 pointer 数不同才调用 split |
| 当前 `MotionEntry` | combined `MotionEntry` | 每个 mode 0/1 | 非 default pointer geometry 时，每次建 entry 都会归一化一次 |
| `InputTarget` | `DispatchEntry` | 0:6 | 六个 mode 逐位尝试；不含该位或 `InputState` 拒绝就没有入队项 |

构造 `DispatchEntry` 时立即取得 `seq` 并增加所引用 `EventEntry` 的引用计数；析构时释放引用。`resolvedEventId` 要在具体事件分支中写入，`timeoutTime` 也要到发送尝试前才写。即使 `deliveryTime` 的构造值是 0，源码注释仍把 delivery/timeout 的有效期限定在 send 阶段，诊断工具不应把一个尚在原始构造阶段的 0 当作真实交付时刻。

### 练习 1：画出一份目标计划的最大与实际展开数

固定一份 Motion 根 entry 和一个 `InputTarget`，其 flags 同时含 `HOVER_ENTER | AS_IS`，使用 default pointer info，connection 为 `NORMAL`。第一笔显式 `HOVER_ENTER` 被 `InputState` 接受，第二笔原始 `HOVER_MOVE` 也被接受。

六个 dispatch mode 位给出结构上限：若 flags 六位全含且起始协议状态与原 action 兼容，单个 target 最多可有 6 次构造尝试、6 笔成功入队的 `DispatchEntry`。本题实际答案是：1 个根 entry、1 个 target、0 个 split entry、0 个 combined entry、2 笔成功入队的 `DispatchEntry`、2 个不同 seq；两笔都引用同一根 entry，使它相对增加 2 份存活引用。题设已经固定两次 `trackMotion()` 均成功，不能再把第二笔改写成拒绝分支。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'struct InputTarget {' frameworks/native/services/inputflinger/dispatcher/InputTarget.h
grep -n -F 'struct DispatchEntry {' frameworks/native/services/inputflinger/dispatcher/Entry.h
grep -n -F 'const uint32_t seq; // unique sequence number, never 0' frameworks/native/services/inputflinger/dispatcher/Entry.h
grep -n -F 'EventEntry* eventEntry; // the event to dispatch' frameworks/native/services/inputflinger/dispatcher/Entry.h
grep -n -F 'for (const InputTarget& inputTarget : inputTargets)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'prepareDispatchCycleLocked(currentTime, connection, eventEntry, inputTarget);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_back(dispatchEntry.release());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'eventEntry->refCount += 1;' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
grep -n -F 'eventEntry->release();' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
```

## 3. InputTarget 的三组 flags 与两种 pointer 账

`InputTarget::flags` 是 Dispatcher 内部控制面，不等于应用最终收到的 `MotionEvent.flags`。r48 把它分为三组：

| 组别 | flags | 作用阶段 |
|---|---|---|
| 角色与披露 | `FOREGROUND`、`SPLIT`、`ZERO_COORDS` | 完成计数、pointer 过滤、publish 清零 |
| 安全视图 | `WINDOW_IS_OBSCURED`、`WINDOW_IS_PARTIALLY_OBSCURED` | 转成公开 Motion flags |
| dispatch mode | `AS_IS`、`OUTSIDE`、`HOVER_ENTER/EXIT`、`SLIPPERY_ENTER/EXIT` | 展开 entry 并决定 resolved action |

`FOREGROUND` 不表示“Z 序最高”。它表示该目标承担本次输入的主要处理责任，并在存在 `InjectionState` 时计入 `pendingForegroundDispatches`。split 手势可同时拥有多个 foreground 窗口；一次 slippery 切换也可能让旧窗的 CANCEL 和新窗的 DOWN 都成为 foreground entry。

pointer 几何有两种编码：

- `pointerIds` 为空：`pointerInfos[0]` 是 default 槽，当前 `MotionEntry` 的全部 pointer 共用它；这里的 0 不是 pointer ID 0。
- `pointerIds` 非空：位图描述目标 pointer 子集，`pointerInfos[pointerId]` 按 ID 取几何；绝不能拿 pointer array index 去索引。

`InputTarget::addPointers()` 收到空集合时会调用 `setDefaultPointerInfo()`，而该函数会清空现有 `pointerIds`。收到非空集合时则要求它与旧集合完全不重叠，再做位并集。正常路由依靠上游不混用 default 与 per-pointer 两种形态；这不是一个可随意交错调用的容错合并 API。

另一个容易漏掉的边界是 `FLAG_SPLIT` 与非空位图的配对。`prepareDispatchCycleLocked()` 在需要真正 split 时会进入 `splitMotionEvent()`，后者直接断言位图非零；正确性依赖目标构造阶段始终给 split 目标登记至少一个 pointer。

r48 还留下一个值得单独审计的数组边界：事件校验允许 pointer ID 到 `MAX_POINTER_ID=31`，同时限制并发 pointer 数不超过 `MAX_POINTERS=16`；但 `InputTarget::pointerInfos` 的长度只有 16，却直接用 pointer ID 作下标，`addPointers()` 与 `createDispatchEntry()` 在本地都没有再次限制 `<16`。这几行能证明静态的尺寸错配，不能单凭本文断言任意生产输入一定可达或已造成何种运行后果。练习统一使用 0—15 内的 ID；若维护 r48 分支，应把 ID 16—31 的注入与 split 路径列为专项边界测试。

## 4. 从 TouchedWindow 到唯一 channel target：合并的是 pointer，不是任意 flags

`TouchState::addOrUpdateWindow()` 先以**同一个 `InputWindowHandle`**为键维护手势路由账。它会 OR `targetFlags` 和 `pointerIds`；加入 `SLIPPERY_EXIT` 时还会清掉 `AS_IS`。这是 hover 进入可同时含 `HOVER_ENTER | AS_IS`、同一窗口可积累多个 split pointer 的来源。

输出 `inputTargets` 时，`addWindowTargetLocked()` 改用 **connection token** 查重。这一层支持多个 window handle 指向同一个客户端 channel，例如同 token 的多个输入区域分别拥有不同 frame/scale。第一次遇到 token 时，它先查注册 `InputChannel`：channel 已注销就跳过这个窗口，不会构造一个空壳 target。

找到已有 target 后，代码并不再次 OR flags，而是断言：

- 已有 `flags` 必须与当前 `targetFlags` 完全相等；
- 已有 `globalScaleFactor` 必须与当前窗口完全相等；
- 新 pointer ID 集合必须与旧集合不重叠。

只有这些不变量成立，才把 `xOffset=-frameLeft`、`yOffset=-frameTop` 和窗口快照中的 `windowXScale/windowYScale` 写入相应 pointer ID 槽。也就是说，**TouchState 层负责把同一 handle 的角色与模式聚合好，InputTarget 层只允许同一 token 的兼容几何片段汇合**。

monitor 是另一条构造路径。`addMonitoringTargetLocked()` 每次新建 target，只放 `DISPATCH_AS_IS`，将调用者提供的 monitor offset 写为 default pointer info，窗口 scale 固定为 1。global monitor 的常见 offset 是 display 基准；跨 portal 的 gesture monitor 可以带路由累计出的偏移，不能一概写成 `(0,0)`。

### 练习 2：判定同 token 的四次加入

初始 target 列表为空，token T 已有注册完成的 `InputChannel`，四个 handle 都指向 T 并能解析到这条 channel。W1 的 flags 为 `FOREGROUND|SPLIT|AS_IS`、global scale 为 1、pointerIds 为 `{2}`；W2 的相应值相同、pointerIds 为 `{7}`；W3 把 mode 改成 `OUTSIDE`；W4 保持 W1 的 flags，但又登记 `{2}`。

唯一答案是：W1 创建 target；W2 通过两项相等断言并合并 pointer 7，此时位图为 `{2,7}`，两根 pointer 可保存不同 frame/scale；W3 触发 flags 相等断言；若跳过 W3 单独加入 W4，则触发 pointer 集合不重叠断言。`addWindowTargetLocked()` 不会把 W3 的 mode 自动 OR 进已有 target，也不会把 W4 当作幂等重复。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'return inputTarget.inputChannel->getConnectionToken() ==' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<InputChannel> inputChannel = getInputChannelLocked(windowHandle->getToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'ALOG_ASSERT(it->flags == targetFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'ALOG_ASSERT(it->globalScaleFactor == windowInfo->globalScaleFactor);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'it->addPointers(pointerIds, -windowInfo->frameLeft, -windowInfo->frameTop,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newPointerIds.isEmpty()) {' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F 'setDefaultPointerInfo(xOffset, yOffset, windowXScale, windowYScale);' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F 'ALOG_ASSERT((pointerIds & newPointerIds) == 0);' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F 'pointerIds |= newPointerIds;' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F '#define MAX_POINTERS 16' frameworks/native/include/input/Input.h
grep -n -F '#define MAX_POINTER_ID 31' frameworks/native/include/input/Input.h
grep -n -F 'PointerInfo pointerInfos[MAX_POINTERS];' frameworks/native/services/inputflinger/dispatcher/InputTarget.h
grep -n -F 'pointerInfos[pointerId].xOffset = xOffset;' frameworks/native/services/inputflinger/dispatcher/InputTarget.cpp
grep -n -F 'if (id < 0 || id > MAX_POINTER_ID) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'target.flags = InputTarget::FLAG_DISPATCH_AS_IS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 5. dispatchEvent 与 prepare：失败默认只截断当前目标

`dispatchEventLocked()` 要求根 entry 已经 `dispatchInProgress=true`，先为整笔事件安排一次 user activity，再遍历 `inputTargets`。每个 target 都用 channel token 重新查 `Connection`；目标计划形成后 connection 若已注销，只丢这一目标，循环继续处理其他 target。

找到 connection 后，`prepareDispatchCycleLocked()` 再执行两道门：

1. connection 必须仍为 `STATUS_NORMAL`；broken 或 zombie 不再增加新 outbound entry。
2. `FLAG_SPLIT` 只能用于 Motion；给 Key 等类型带这个 flag 会触发 fatal invariant。

之后才决定是否创建 split entry。若不需要 split，就直接把当前 entry 交给 mode 展开。这里的“当前 entry”可能是根 Motion，也可能是其他内部路径合成的事件；下游只依赖它此刻携带的 pointer、action、id 与 injection 引用。

失败作用域应按层次读：connection 缺失或非 NORMAL 只截断当前 target；split 失败只截断当前 connection 的这次 target；某个 mode 被 `InputState` 拒绝只跳过那一笔 entry。它们都不会自动回滚已经为先前 target 或先前 mode 成功入队的内容。

对注入还有更窄的返回值边界。`dispatchMotionLocked()` 在得到路由成功结果后，先调用 `setInjectionResult()`，之后才补 global/portal monitors、处理冲突取消并进入逐 target dispatch。因此后半段遇到 connection 消失、非 NORMAL、split 失败或 track 拒绝，不会把已经写成 succeeded 的 injection result 改回失败；若最终没有 foreground entry 入队，pending 仍是 0，`WAIT_FOR_RESULT` 甚至 `WAIT_FOR_FINISHED` 都可能返回成功。这些同步模式证明的是既定状态机条件，不是“每个候选 target 均已 publish”。

`enqueueDispatchEntriesLocked()` 记录调用前 outbound 是否为空。六个 mode 尝试结束后，只有原来为空且现在至少成功加入一笔时，才立即启动发送周期。若所有 mode 都不在 flags 中，或所有构造项都被协议门拒绝，便不会启动空队列。

本章主线从 `dispatchEventLocked()` 进入，但并非所有 Motion 都经过这一个入口。connection 的 `InputState` 还能合成 CANCEL/HOVER_EXIT，建立非 foreground、AS_IS、default-info target 后直接调用 `enqueueDispatchEntryLocked()`；这类收口事件拥有新的 Dispatcher ID，也可以发给 monitor。诊断合成取消时应从 connection memento 反查，不能强求它出现在本轮 `inputTargets` 遍历中。

## 6. splitMotionEvent：按 ID 保留 pointer，再重写动作语义

真正 split 的触发条件不是“看见 `FLAG_SPLIT` 就复制”，而是 `pointerIds.count() != originalMotionEntry.pointerCount`。若数量相等，r48 只是在 prepare 阶段不创建 split entry，而是把当前 entry 继续交给 mode 展开；它不逐个验证位图成员。这依赖上游保证位图确实是当前 pointer 集合。若人为构造一个“数量相同但 ID 不同”的坏 target，这个快捷路径不会替你发现它。还要注意，跳过 split 不等于最终 `DispatchEntry` 一定引用原对象：只要 `pointerIds` 非空，后面的 `createDispatchEntry()` 仍会为每个命中 mode 创建一份同 ID、同 action 的 combined entry。

数量不等时，`splitMotionEvent()` 先断言请求位图非空，再按原 pointer array 的遍历顺序复制命中的 properties 与 coords。因此输出数组保持被选 pointer 的原相对顺序，而不是按 pointer ID 数值排序。若最终复制数不等于位图计数，说明请求中至少有 ID 不在当前事件内，函数返回空，当前目标投递被丢弃。

只有原 action 为 `POINTER_DOWN` 或 `POINTER_UP` 时需要重写：

| action pointer 与目标的关系 | split 后 pointer 数 | 输出 action |
|---|---:|---|
| 属于目标 | 1 | `POINTER_DOWN → DOWN`；`POINTER_UP → UP` |
| 属于目标 | 大于 1 | 保留 POINTER 动作，action index 改为 split 数组中的新 index |
| 不属于目标 | 任意合法数量 | `MOVE` |

其他 action 原样保留。例如子集上的 `MOVE` 仍是 `MOVE`，子集上的 `CANCEL` 仍是 `CANCEL`。只要函数确实被调用，无论 action 是否改变，都会从 `mIdGenerator` 取得一个新 Motion ID；事件时间、downTime、设备、source、display 与其余字段则从当前 entry 复制。

若当前 entry 带 `InjectionState`，split entry 共享同一个对象并增加其引用计数。它不是一次新的注入请求；后续多目标 foreground entry 仍在同一本 `pendingForegroundDispatches` 账上增减。

### 练习 3：手算三根 pointer 的两个目标子流

原数组顺序为 `[id7, id2, id9]`，action 是 `POINTER_DOWN(index=2)`，即本次变化的是 id9。目标 A 请求 `{7,9}`，目标 B 请求 `{2}`，两者数量都小于原数组数量；目标 A、B、后述 C 与全量目标都明确带 `SPLIT|AS_IS`。

唯一答案是：A 的输出数组为 `[id7,id9]`，id9 在新数组的 index 为 1，所以 action 为 `POINTER_DOWN(index=1)`；B 的输出数组为 `[id2]`，变化 pointer 不属于 B，所以 action 为 `MOVE`。A、B 各调用一次 split，各有不同的新 split ID。若目标 C 请求 `{2,5}`，只复制到 id2，计数不符，因此 C 的 split 返回空且只丢 C。若某目标请求 `{7,2,9}`，数量相等，prepare 阶段不创建 split，当前 ID 仍为原 ID；但非空 pointerIds 仍使每个命中 mode 走 per-pointer 路径并新建同 ID 的 combined entry，最终 `DispatchEntry` 不直接引用原对象。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (inputTarget.flags & InputTarget::FLAG_SPLIT) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (inputTarget.pointerIds.count() != originalMotionEntry.pointerCount) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'ALOG_ASSERT(pointerIds.value != 0);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerIds.hasBit(pointerId)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (splitPointerCount != pointerIds.count()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerIds.count() == 1) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '? AMOTION_EVENT_ACTION_DOWN' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'while (pointerId != uint32_t(splitPointerProperties[splitPointerIndex].id)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '(splitPointerIndex << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'action = AMOTION_EVENT_ACTION_MOVE;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'int32_t newId = mIdGenerator.nextId();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 7. 四本编号账：root ID、derived ID、resolved ID 与 seq

同一条链上至少有四种编号，作用域完全不同：

| 编号 | 何时取得 | 代表什么 | 是否交给 App 作为 `InputEvent.getId()` |
|---|---|---|---|
| root Motion ID | Reader/注入入口 | 初始输入事实身份 | 仅在后续没有替换时 |
| split Motion ID | `splitMotionEvent()` 真正执行时 | pointer 子集视图身份 | AS_IS 时会成为 resolved ID |
| resolved event ID | mode 解析通过后 | 该 DispatchEntry 对外 action 视图身份 | 是 |
| `DispatchEntry::seq` | entry 构造一开始 | transport 投递与 FINISHED 关联凭据 | 否；Consumer 另行返回 seq |

combined entry 不取得新 Motion ID。`createDispatchEntry()` 为归一坐标创建 `MotionEntry` 时显式沿用传入 entry 的 id；它改变表达坐标的载体，不改变这一层逻辑事件身份。

resolved ID 的规则也不是“action 字面变化就换 ID”：

- `AS_IS` 先写当前 Motion ID；通常 action 也保持当前 action。
- 五种显式 transmute mode 先保留哨兵；`trackMotion()` 成功后才取一个新 ID。
- AS_IS 的 `HOVER_MOVE` 若因 connection 尚未 hovering 而被修成 `HOVER_ENTER`，action 改了，但 resolved ID 仍是当前 Motion ID。这是必须单独记住的例外。

`seq` 使用一个静态原子计数器，跨 connection 取得非零值；回绕取得 0 时继续递增。它在 `DispatchEntry` 构造列表的最前面取得，所以后续 `trackMotion()` 拒绝仍会留下 seq 缺口。相比之下，显式 mode 的新 resolved ID 在 track 成功后才分配：同一拒绝会消耗 seq，却不会消耗这一步的 `mIdGenerator` ID。

App 侧 `InputConsumer` 把 message 的 `eventId` 写进 `MotionEvent`，同时通过独立的 `outSeq` 返回 transport seq。后续批处理还可能把多个 seq 串成一次客户端完成动作；因此日志不能拿 `MotionEvent.getId()` 去匹配 Dispatcher wait queue。

### 练习 4：给五条派生路径填编号

五行按表序发生在同一进程中，但各使用彼此隔离的 `Connection/InputState`，且每行的当前根 Motion ID 都记作 `I0`。共享的 `mIdGenerator.nextId()` 依次给 `I1、I2…`，共享的 seq 分配器依次给 `S1、S2…`；前四行接受，第五行拒绝。由此推导：

| 场景 | 当前 entry ID | resolved action | App event ID | transport seq |
|---|---|---|---|---|
| SPLIT 位图恰含全部 pointer，AS_IS | `I0` | 原 action | `I0` | `S1` |
| 真正 split 后 AS_IS | `I1` | split 后 action | `I1` | `S2` |
| 真正 split 后 OUTSIDE | split 先取 `I2` | `OUTSIDE` | mode 再取 `I3` | `S3` |
| 未 hovering 的 AS_IS HOVER_MOVE | `I0` | `HOVER_ENTER` | `I0` | `S4` |
| 显式 HOVER_EXIT 但 track 拒绝 | `I0` | `HOVER_EXIT` | 无 message；不分配新 resolved ID | `S5` 已消耗 |

五行不是同一次目标展开，但两个全局编号器不重置；所以第三行必须承接第二行已经消耗的 `I1`。第五行则只在构造时消耗 `S5`，显式 mode 的新 resolved ID 要等 track 成功后才分配。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'new MotionEntry(newId, originalMotionEntry.eventTime' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'new MotionEntry(motionEntry.id, motionEntry.eventTime' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F ': seq(nextSeq()),' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
grep -n -F 'seq = android_atomic_inc(&sNextSeqAtomic);' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
grep -n -F '} while (!seq);' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
grep -n -F 'dispatchEntry->resolvedEventId = motionEntry.id;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '? mIdGenerator.nextId()' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F ': motionEntry.id;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'event->initialize(msg->body.motion.eventId' frameworks/native/libs/input/InputTransport.cpp
grep -n -F '*outSeq = mMsg.body.motion.seq;' frameworks/native/libs/input/InputTransport.cpp
```

## 8. createDispatchEntry 的几何归一化：让每根 pointer 保住自己的窗口坐标

default pointer info 路径最直接：`DispatchEntry` 引用当前 entry，带走 default offset、window scale 与 global scale，不创建 combined entry。

per-pointer 路径面对一个表示难题：同一客户端 channel 可能对应多个 handle，pointer 2 属于 frame/scale A，pointer 7 属于 frame/scale B；但一条 `InputMessage::MOTION` 只能携带一组公共 `xScale/yScale/xOffset/yOffset`。r48 选择 target 位图中**最低 marked pointer ID**的 `PointerInfo` 作为规范基准。它不是当前 pointer array 的 index 0，也不保证是最早落下的手指。

对每根保留下来的 pointer，X 轴重写可写成：

`normalizedRawX = (rawX + ownOffsetX) × (ownWindowScaleX / baseWindowScaleX) - baseOffsetX`

Y 轴同理。随后 combined `MotionEntry` 把自身构造 offset 设为 0，而 `DispatchEntry` 保存 base pointer 的 offset 与 scale。publish 发送的公共 X 参数为：

`xScale = baseWindowScaleX`，`xOffset = baseOffsetX × xScale`

客户端 `getX()` 最终计算 `normalizedRawX × xScale + xOffset`，代入后正好得到：

`(rawX + ownOffsetX) × ownWindowScaleX`

所以每根 pointer 都落到自己的窗口局部坐标，而 message 仍只需要一组公共参数。combined entry 沿用当前 Motion ID，共享同一个 `InjectionState`；每个包含该几何 target 的 mode 都会各建一份 combined entry，因为 `createDispatchEntry()` 位于逐 mode 调用内部。

publish 还有两个限定。第一，只有 source 带 `AINPUT_SOURCE_CLASS_POINTER` 且 target 未设 `ZERO_COORDS` 时，才使用这组窗口 offset/scale。第二，`globalScaleFactor != 1` 时调用的是 `PointerCoords::scale(global, 1, 1)`：X/Y 不在这里再次缩放，变化的是 touch/tool major/minor；r48 认为 X/Y 所需的全局兼容缩放已包含在 window scale 中。

归一化代码直接计算 `ownScale / baseScale`，没有在本函数检查 base window scale 是否为 0。正常窗口快照应提供可用 scale；若追查畸形窗口信息或移植分支，必须把零值、NaN 与 Infinity 当作上游不变量单独验证，不能把公式本身当作数值防线。

### 练习 5：手算不同 frame 与 scale 的同 channel 两指

这是 pointer-class source，target 不带 `ZERO_COORDS`，`globalScaleFactor=1`，并进入正常 Motion publish 分支。target 位图为 `{2,7}`，所以 id2 是 base，即使事件数组顺序是 `[id7,id2]`。id2 的 `frameLeft=100`、window scale 为 `0.5`，屏幕 raw X 为 140；id7 的 `frameLeft=300`、window scale 为 `2`，屏幕 raw X 为 320。offset 分别为 -100 与 -300。

唯一答案是：id2 的 normalized raw X 为 `140`；id7 为 `(320-300)×(2/0.5)-(-100)=180`。message 公共参数为 `xScale=0.5`、`xOffset=-50`。客户端 id2 的 `getX()` 为 `140×0.5-50=20`，id7 为 `180×0.5-50=40`，分别等于 `(140-100)×0.5` 与 `(320-300)×2`。数组 index 0 虽是 id7，基准仍由最低 marked ID 2 决定。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'inputTarget.pointerIds.firstMarkedBit()' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const PointerInfo& currPointerInfo = inputTarget.pointerInfos[pointerId];' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'currPointerInfo.windowXScale / firstPointerInfo.windowXScale;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].applyOffset(currPointerInfo.xOffset, currPointerInfo.yOffset);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].scale(1, scaleXDiff, scaleYDiff);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerCoords[pointerIndex].applyOffset(-firstPointerInfo.xOffset,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'firstPointerInfo.yOffset, inputTarget.globalScaleFactor,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'xOffset = dispatchEntry->xOffset * xScale;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'float x = windowInfo->windowXScale * (point.x - windowInfo->frameLeft);' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'scaledCoords[i].scale(globalScaleFactor, 1 /* windowXScale */,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 9. 六种 mode 按固定顺序展开，每笔 entry 只保留一个 mode

`enqueueDispatchEntriesLocked()` 不遍历一个无序 bitset，而是固定尝试六次：

| 次序 | mode | 目标化 action |
|---:|---|---|
| 1 | `HOVER_EXIT` | `ACTION_HOVER_EXIT` |
| 2 | `OUTSIDE` | `ACTION_OUTSIDE` |
| 3 | `HOVER_ENTER` | `ACTION_HOVER_ENTER` |
| 4 | `AS_IS` | 当前 Motion action |
| 5 | `SLIPPERY_EXIT` | `ACTION_CANCEL` |
| 6 | `SLIPPERY_ENTER` | `ACTION_DOWN` |

每次调用先检查 target 是否含当前 mode；不含便立即返回。含有时则清掉整个 `DISPATCH_MASK`，只放回当前 mode，再构造 `DispatchEntry`。因此 target 可以同时表达多个待展开动作，但队列中的每一笔 entry 都只有一个明确 mode。

固定顺序不只是让日志好看，因为每笔成功项都会立即修改该 connection 的 `InputState`，下一 mode 会看到前一 mode 建立的状态。最清楚的生产例子是一个新 hover 目标：正常命中先给它 `FOREGROUND|AS_IS`，hover 目标变化又 OR 入 `HOVER_ENTER`。展开时先送显式 ENTER 建立 hovering memento，再处理 AS_IS 的原 `HOVER_MOVE`，于是第二笔不再触发缺失 ENTER 修复。

slippery 的常见目标则不是简单的“同 target 同时 AS_IS+CANCEL”。旧窗口加入 `SLIPPERY_EXIT` 时，`TouchState::addOrUpdateWindow()` 会清掉它的 `AS_IS`；新窗口以 `SLIPPERY_ENTER` 建立，当前轮收到 DOWN，轮末保存到后续手势状态时再被收敛为 AS_IS。这个细节解释了为什么固定 mode 顺序存在，但不能凭顺序表断言每个目标都会收到六笔。

## 10. resolvedAction 与 hover 修复：两种 ENTER 拥有不同 ID 语义

Motion 分支先把 `resolvedEventId` 设成 InputReader 不会产生的 OTHER-source 哨兵。五种显式改写 mode 只设置 action，哨兵继续保留；AS_IS 分支同时设置当前 action 和当前 Motion ID。

随后是一道独立的 connection 状态修复：若此时 resolved action 是 `HOVER_MOVE`，但 `InputState::isHovering(deviceId, source, displayId)` 为 false，Dispatcher 把 action 改为 `HOVER_ENTER`。由于这只能从 AS_IS 的 HOVER_MOVE 到达，resolved ID 已经是当前 Motion ID。后面的三元表达式只在 ID 仍是哨兵时取新 ID，所以这条修复不会换 ID。

因此日志中的两种 `HOVER_ENTER` 必须分开：

| 来源 | action 如何得到 | resolved ID |
|---|---|---|
| target 明确含 `DISPATCH_AS_HOVER_ENTER` | mode 直接改写 | `trackMotion()` 成功后取新 ID |
| AS_IS HOVER_MOVE 但 connection 尚无 hover memento | 状态修复改写 | 保留当前 Motion ID |

global/gesture monitor 只有 AS_IS mode，第一次中途看见 HOVER_MOVE 时可能走第二种路径；新普通 hover 窗口通常同时有显式 ENTER 和 AS_IS，固定展开顺序让第一笔先建立状态，第二笔保持 HOVER_MOVE。

### 练习 6：推演三种 hover 与一组 slippery

四个场景彼此独立，所有 `trackMotion()` 都成功：

| 场景 | entry 数 / 同一 target 内顺序 | action | ID |
|---|---|---|---|
| 新普通 hover 窗口，target 含 `HOVER_ENTER|AS_IS`，原 action 为 HOVER_MOVE | 2 | `HOVER_ENTER`，再 `HOVER_MOVE` | 新 resolved ID，再原/当前 ID |
| 新 monitor 只有 AS_IS，自己的 InputState 尚未 hovering | 1 | 修复成 `HOVER_ENTER` | 保留原/当前 ID |
| 已 hovering 的 monitor 收 AS_IS HOVER_MOVE | 1 | `HOVER_MOVE` | 保留原/当前 ID |
| 单指从 slippery A 滑入 B | A、B 各 1；跨 Connection 相对顺序未定 | A 得 `CANCEL`，B 得 `DOWN` | 两笔显式 mode 各取新 resolved ID |

最后一行的两个 target 在 `inputTargets` 向量中的相对位置由上游目标列表决定；六 mode 固定顺序只保证**同一个 target 内**的尝试顺序，不能据此宣称不同 connection 一定谁先 publish。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_HOVER_EXIT);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_OUTSIDE);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_HOVER_ENTER);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_IS);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_SLIPPERY_EXIT);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_SLIPPERY_ENTER);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'inputTargetFlags = (inputTargetFlags & ~InputTarget::FLAG_DISPATCH_MASK) | dispatchMode;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_OUTSIDE;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_CANCEL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_DOWN;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction == AMOTION_EVENT_ACTION_HOVER_MOVE' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_HOVER_ENTER;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 11. resolvedFlags 与 HMAC：签的是目标化投影，不是整条 MotionEvent

`resolvedFlags` 先复制当前 `MotionEntry.flags`，再按这个 target 的内部安全位追加公开的 `AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED` 与 `AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED`。`FOREGROUND`、`SPLIT`、`ZERO_COORDS` 和 dispatch mode 本身不会作为同名公开位塞进 `MotionEvent.flags`。

这意味着同一根 Motion 对不同 target 可以有不同 resolved flags。第243章解释了遮挡位如何产生；本章只负责说明它们在 entry 级别固化，并同时供 `InputState`、HMAC 与 publish 使用。

“HMAC 按最终视图签”仍需再收窄。r48 的 Motion 签名只在**resolved masked action 为 DOWN 或 UP**时计算；MOVE、OUTSIDE、HOVER 与 CANCEL 直接返回 `INVALID_HMAC`。签名函数从当前 `MotionEntry` 构造 `VerifiedMotionEvent`，再用 `DispatchEntry` 覆盖 actionMasked 与允许验证的 flags。

实际签名投影包含：type、deviceId、eventTime、source、displayId、第一根 pointer 的 raw X/Y、resolved masked action、downTime、两种允许验证的遮挡 flags、metaState 与 buttonState。它不包含 resolved event ID、transport seq、完整 pointer 数组、action index、classification、edge flags 或窗口局部 offset/scale。

“`DispatchEntry` 所引用的 `MotionEntry`”也很重要：default-info 路径引用根/当前 entry；per-pointer 路径引用 combined entry。真正 split 时，临时 split 决定 combined 的 pointer 子集与数组顺序，combined 再完成几何归一化，所以签名读取的是 combined `pointerCoords[0]` 的归一化 raw X/Y，而不是裸 split entry。publish 为 global scale 临时复制的 `usingCoords` 并没有传进 `getSignature()`；不过该调用对 X/Y 使用 window scale 1，只调整 touch/tool major/minor，而这些尺寸字段本来也不在 Verified 投影中。`ZERO_COORDS` 的正常生产用途是 OUTSIDE，而 OUTSIDE 不签名。

### 练习 7：区分公开 flags、坐标披露与签名投影

同一原始 DOWN 的 flags **仅含** `IS_GENERATED_GESTURE`，没有其他位。目标 A 为 AS_IS foreground 且 target 带 `WINDOW_IS_OBSCURED`；目标 B 为跨 UID OUTSIDE，只带 `ZERO_COORDS`。另有一笔 flags 为 0 的原始 MOVE，对不带任何安全目标位的 slippery 新窗 C 以 `SLIPPERY_ENTER` 投递。

唯一答案是：A 的 resolved flags 同时保留 generated-gesture 位并加入公开 obscured 位，但 HMAC 的 flags 投影只含 obscured；A 因 resolved DOWN 计算签名。B 的 resolved flags 仍只有 generated-gesture，resolved action 是 OUTSIDE，publish 清空全部 PointerCoords，HMAC 为 invalid sentinel。C 的 resolved flags 为 0，resolved action 是 DOWN，因此会按 C 当前 entry 的 raw X/Y 与 resolved DOWN 计算签名；C 的新 resolved ID 和 seq 都不在签名投影里。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'dispatchEntry->resolvedFlags = motionEntry.flags;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedFlags |= AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedFlags |= AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::array<uint8_t, 32> hmac = getSignature(*motionEntry, *dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if ((actionMasked == AMOTION_EVENT_ACTION_UP) || (actionMasked == AMOTION_EVENT_ACTION_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.actionMasked = actionMasked;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'verifiedEvent.flags = dispatchEntry.resolvedFlags & VERIFIED_MOTION_EVENT_FLAGS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const float rawX = entry.pointerCoords[0].getAxisValue(AMOTION_EVENT_AXIS_X);' frameworks/native/services/inputflinger/dispatcher/Entry.cpp
grep -n -F 'AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED | AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED;' frameworks/native/include/input/Input.h
grep -n -F 'return INVALID_HMAC;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 12. InputState 是入队前的接收者协议门，不是 App 已处理证明

action、候选 ID 语义和 flags 确定后，Dispatcher 调用该 connection 独有的 `InputState::trackMotion()`。传入的是 `DispatchEntry` 所引用的根/default 或 combined `MotionEntry`，以及 resolved action/flags；正常 split 路径在这里已经由 combined 承接。因此 slippery 新窗用 DOWN 建 memento，旧窗用 CANCEL 清 memento，而不是都拿原始 MOVE 跟踪。

主要规则是：

| resolved action | connection 状态要求与变化 |
|---|---|
| `DOWN` | 删除同 device/source/display 的旧 non-hover memento，再建立新 memento |
| `MOVE`、`POINTER_DOWN/UP` | 一般必须已有 non-hover memento且没有 transfer 遗留的未知 pointer；navigation/joystick 有专门例外 |
| `UP`、`CANCEL` | 必须找到 non-hover memento，找到后删除；否则拒绝 |
| `HOVER_ENTER/MOVE` | 替换或建立 hover memento |
| `HOVER_EXIT` | 必须找到 hover memento，找到后删除；否则拒绝 |
| `OUTSIDE` 等其他 action | 默认返回 true，不建立普通 down memento |

若返回 false，局部 `unique_ptr<DispatchEntry>` 立即析构：EventEntry 引用归还，已取得的 seq 留下缺口，foreground 计数尚未增加，entry 不进 outbound。显式 mode 的新 resolved ID 也尚未分配。

源码头文件把 `trackMotion()` 描述为记录“刚刚 published”的事件，但 r48 实际调用点位于 outbound 入队甚至 socket publish 之前。诊断时应以调用顺序为准：这里能证明 Dispatcher 已把该视图纳入 connection 的预期协议，不能证明客户端已收到。

track 成功并补定 resolved ID 后，`dispatchPointerDownOutsideFocus()` 检查 resolved action。source 必须属于 POINTER 类，masked action 必须为 DOWN，target token 必须仍能映射到 window handle，且它不是当前 focused window，才异步 post policy 命令。这里取的是 `mFocusedDisplayId` 上的焦点窗口，不是按 Motion 自身 `displayId` 查焦点；`SLIPPERY_ENTER` 改出的 DOWN、split 后 singleton DOWN，甚至有效 wallpaper window target 都可能进入判断，monitor token 因找不到 window handle而不会命中。这个通知不重选目标，也不是 focus 已经改变的完成确认。

## 13. FOREGROUND、引用计数与 WAIT_FOR_FINISHED：计的是 entry 责任

只有 `trackMotion()` 成功后，代码才检查这笔 `DispatchEntry` 是否带 `FOREGROUND`。若同时存在共享的 `InjectionState`，便把 `pendingForegroundDispatches` 加 1，然后将 entry 压入 outbound queue。

计数单位是**成功入队的 foreground DispatchEntry**，不是根事件、target 或 connection：

- split 手势的多个前景窗口可以分别加 1；
- 新 hover 窗口的显式 ENTER 与 AS_IS MOVE 都带 foreground 时可以加两次；
- slippery 旧窗 CANCEL 与新窗 DOWN 可以各加一次；
- outside、wallpaper、monitor 通常不带 foreground，不增加这本同步注入账；
- 没有 `InjectionState` 的硬件事件，即使带 foreground 也没有该计数对象。

`WAIT_FOR_FINISHED` 先等 injection result 不再 pending；若结果成功，再等共享计数归零。entry 最终由 `releaseDispatchEntry()` 释放时，只要仍带 foreground，就在 delete 前递减计数；归零会唤醒注入等待者。正常 FINISHED、broken connection drain 等路径怎样汇入 release，是第245章的主题。

这里的完成不要求 `handled=true`。Motion 的 handled 值不会像 Key fallback 那样改变本章的目标化过程；只要对应 entry 生命周期被合法收口，foreground 债就能减少。反过来，非 foreground entry 虽不拖住注入线程，仍需要自己的 transport 回执和队列清理，不能理解为“无需 FINISHED”。

这句话只适用于本文的 Motion 主线。foreground Key 若首次回报 `handled=false`，policy 可能生成 fallback 并把同一 `DispatchEntry`、同一 seq 放回 outbound；那一轮不会释放 entry，也不会立刻减少 pending。第245章按事件类型展开 FINISHED 时会把这个例外单独结账。

引用计数与 foreground 计数也不能合并。前者保护 `EventEntry`/`InjectionState` 的内存生命周期；后者定义同步注入还欠多少前景投递。split 与 combined entry 会显式共享并增加 InjectionState 引用，但只有成功入队且带 foreground 的 DispatchEntry 才增加 pending 数。

### 练习 8：计算一笔 injected slippery MOVE 的完成债

一笔带共享 `InjectionState` 的单指 MOVE 从 foreground slippery 窗口 A 进入 foreground 窗口 B。A 形成 `SLIPPERY_EXIT`，B 形成 `SLIPPERY_ENTER`；另有 wallpaper W 和 monitor G 各形成 AS_IS 非 foreground entry。题设预置 A 已有匹配的 non-hover memento、B 尚无，四笔都通过 `trackMotion()`。

唯一答案是：A 的 CANCEL 与 B 的 DOWN 各使 pending foreground 加 1，总债为 2；W、G 虽各有 seq、引用与队列项，但不增加该计数。A 若先以 handled=false 完成，计数降为 1；B 随后完成才归零唤醒 WAIT_FOR_FINISHED。若 G 仍未完成，注入等待仍可结束，但 G 的 connection 账尚未结束。另作单一改动，若移除 A 预置的 memento，A 的 CANCEL 会被 track 拒绝：A 的 seq 已消耗但不增 pending，B 的 DOWN 仍接受，此时总债只有 B 的 1。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (!connection->inputState.trackMotion(motionEntry, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchPointerDownOutsideFocus(motionEntry.source, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (dispatchEntry->hasForegroundTarget()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'incrementPendingForegroundDispatches(newEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'while (injectionState->pendingForegroundDispatches != 0) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionState->pendingForegroundDispatches += 1;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'decrementPendingForegroundDispatches(dispatchEntry->eventEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionState->pendingForegroundDispatches -= 1;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInjectionSyncFinished.notify_all();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'delete dispatchEntry;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 14. publish 才应用最后一层坐标，并把 eventId 与 seq 分开装箱

entry 到达 outbound 队首时，`startDispatchCycleLocked()` 先写 `deliveryTime=currentTime`，再按 connection timeout 写 `timeoutTime`，随后调用 publisher。严格说，这两个字段在 **send 尝试前**就写入，而不是 send 成功后才写。若 socket 返回 `WOULD_BLOCK` 且 wait queue 非空，entry 留在 outbound 等既有回执释放空间，下一次尝试会重写两字段；若 wait queue 为空，同一返回值会被视为异常并触发 broken-cycle drain，不能概括成普通背压重试。只有 publish 成功并进入 wait queue 后，这些字段才进入稳定的在途语义；ANR 索引还要另看 connection 是否 responsive。

Motion publish 的最终字段来源如下：

| `InputMessage` 字段 | 来源 |
|---|---|
| `seq` | `DispatchEntry::seq` |
| `eventId` | `resolvedEventId` |
| `action`、`flags` | `resolvedAction`、`resolvedFlags` |
| pointer properties/coords | `DispatchEntry` 引用的根/default 或 combined `MotionEntry` |
| x/y scale、offset | pointer source 且未 ZERO 时取 DispatchEntry；offset 先乘 window scale |
| HMAC | 当前 MotionEntry 与 resolved action/可验证 flags 的投影 |

若是 pointer source 且未设 `ZERO_COORDS`，publisher 参数使用 `windowXScale/windowYScale`，offset 为 `DispatchEntry offset × 对应 scale`。若 global scale 不为 1，先复制全部 coords，只对 touch/tool major/minor 应用 global factor。若 target 带 `ZERO_COORDS`，则逐 pointer `clear()`，同时保持默认 scale 1、offset 0；不是只把 X/Y 写零。若 source 不属于 POINTER 类且没有 ZERO，窗口几何也不应用，coords 原样传递。

`getSignature()` 的调用位于这些临时 coords 准备之后，但参数仍是存储的 `motionEntry`，不是 `usingCoords` 指针。接着 `InputPublisher::publishMotionEvent()` 拒绝 seq 0 或非法 pointerCount，把各字段复制进 `InputMessage` 并调用 channel `sendMessage()`。

客户端消费时，message eventId 进入 `MotionEvent::mId`；seq 则作为独立返回值交给 receiver，最终用于 finished signal。这正是“App 能看见的事件身份”与“Dispatcher 要结的传输账”不能互换的原因。

publish 成功后，当前 entry 一定从 outbound 移到 wait queue；只有 connection 仍为 responsive 时，代码才把它的 deadline 插入 `mAnrTracker`。`WOULD_BLOCK`、批处理 seq chain、乱序 FINISHED 与异常 drain 的细节全部留到第245章。本章只需守住边界：**resolved 字段在入队前定案，最终坐标参数在发送尝试时装箱，send 成功才进入等待回执阶段。**

## 15. 七类接收者视图与失败阶梯：从现象反推第一处分歧

把常见目标并排后，可以看到 InputTarget 的价值不是“保存收件地址”，而是允许同源事实形成不同协议视图：

| 接收者/场景 | pointer 与 action | 坐标 | flags / ID / foreground |
|---|---|---|---|
| 普通命中前景窗 | 全部或本窗 split 子集，通常 AS_IS | 本窗局部 | 当前/派生 ID；可带遮挡位；foreground |
| split 中未发生变化的旧窗 | 本窗子集；其他窗 pointer 变化时见 MOVE | 本窗局部 | split 新 ID；foreground |
| 新 split 窗口 | 第一根本窗 pointer 见 DOWN | 本窗局部 | split 新 ID；foreground |
| cross-UID outside 窗 | 通常原数组但 action 为 OUTSIDE | 全部 coords 清空 | mode 新 ID；非 foreground；不签 Motion HMAC |
| wallpaper | AS_IS、default pointer info | wallpaper frame 对应局部 | 根/当前 ID；强制两遮挡位；通常非 foreground |
| gesture/global monitor | AS_IS，可能补 HOVER_ENTER | display/portal monitor 坐标 | 修复 hover 可保留 ID；非 foreground |
| slippery 旧窗/新窗 | CANCEL / DOWN | 各自窗口局部 | 两个 mode 新 ID；两者都可 foreground |

三类特殊目标还有各自的上游尖角。OUTSIDE 只在首个 DOWN 选取；跨 UID 时 `ZERO_COORDS` 清的是 pointer coords，独立传输的 cursor-position 字段并未在该分支清零。wallpaper 也是首 DOWN 才因前景窗 `hasWallpaper` 加入；该段扫描只检查 display 与 wallpaper type，不重新检查 wallpaper 的 visible/paused，而且发生在“只对 foreground 窗做注入权限检查”之后。slippery 新窗路径没有重走新 DOWN 分支对 paused、connection 存在与 responsive 的预检；若它后来因 channel 缺失而跳过，旧窗的 foreground CANCEL 仍可能已经派生，整体路由结果也仍是 succeeded。这里记录的是 r48 调用顺序，不表示这些边界在任意设备上必然形成用户可见故障。

排障时按下面阶梯找第一处分歧，不要从 App 回调缺失直接猜 socket：

1. `InputTarget` 是否存在：上游是否选出目标，window channel 是否已注册。
2. connection 是否还能按 token 找到，状态是否 `NORMAL`。
3. SPLIT 位图数量与成员是否满足当前 Motion；是否因缺 ID 返回空。
4. 每个 mode 是否真的在 flags 中；构造后拿到了哪个 seq。
5. resolved action/flags 是否被 `InputState` 接受；拒绝时不会入队和增加 foreground 债。
6. entry 是否进入 outbound；publish 使用了哪个 EventEntry、eventId、坐标参数与 HMAC。
7. `sendMessage()` 是否成功；只有成功才进入 wait queue，后续才谈 FINISHED。

### 练习 9：为一次多目标 DOWN 填最终视图与完成点

一笔带共享 `InjectionState` 的 injected 根事件 `id=I0`，其初始 `pendingForegroundDispatches=0`、Motion flags 为 0、单 pointer DOWN、屏幕坐标 `(80,60)`。A 是 frame 左上 `(20,10)`、scale `(1,1)` 的正常 foreground 窗且部分遮挡；B 是跨 UID outside 窗并带 ZERO；W 是 frame 左上 `(0,0)`、scale `(1,1)` 的 wallpaper；G 是 offset `(5,-3)`、scale 1 的 monitor。A/B/W/G 属于四条不同的 NORMAL Connection，InputState 均接受，四个目标的几何都使用 default info；其 outbound/wait 初始为空，publisher 均立即成功。

唯一答案：

| 目标 | action / App ID | App 可见坐标 | 公开安全 flags | pending foreground |
|---|---|---|---|---:|
| A | DOWN / `I0` | `(60,50)` | PARTIALLY_OBSCURED | +1 |
| B | OUTSIDE / 新 resolved ID | 全部 PointerCoords 清零 | 0 | +0 |
| W | DOWN / `I0` | `(80,60)` | OBSCURED 与 PARTIALLY_OBSCURED | +0 |
| G | DOWN / `I0` | `(85,57)` | 0 | +0 |

四个目标各有独立 seq。题设明确 send 成功，所以四笔都已从各自 outbound 进入 wait；四笔 default-info `DispatchEntry` 共同让根 EventEntry 相对增加 4 份存活引用，但只有 A 让共享注入完成债从 0 增至 1。若只知道 `dispatchEventLocked()` 已遍历结束，则不能提前填写“都在 wait”，因为 send 可能阻塞或失败。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'dispatchEntry->deliveryTime = currentTime;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->timeoutTime = currentTime + timeout;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if ((motionEntry->source & AINPUT_SOURCE_CLASS_POINTER) &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '!(dispatchEntry->targetFlags & InputTarget::FLAG_ZERO_COORDS)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'scaledCoords[i].clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '.publishMotionEvent(dispatchEntry->seq,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedEventId,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.push_back(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'msg.body.motion.seq = seq;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'msg.body.motion.eventId = eventId;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'return mChannel->sendMessage(&msg);' frameworks/native/libs/input/InputTransport.cpp
```

## 16. r48 诊断顺序、结论与第245章接口

面对“同一触摸在不同接收者上 action、坐标或 ID 不一样”，最短的静态核对顺序是：

1. 记录当前 Motion 的 `id/action/pointerProperties`，明确 pointer ID 与 array index。
2. 对每个 target 记录 connection token、三组 flags、pointerIds 和每 ID 的 PointerInfo。
3. 判断 split 是否真正调用；若调用，写出保留数组、action/index 和 split ID。
4. 判断 default 或 per-pointer geometry；若为后者，找最低 marked ID 并手算 combined raw coords。
5. 按固定次序列出实际 mode；每笔分别记录 seq、resolved action/flags/ID。
6. 结合该 connection 的 InputState 判断接受、修复或拒绝，不把 memento 当成 App 已消费证据。
7. 区分 foreground pending、EventEntry/InjectionState 引用与 connection 队列三本账。
8. 到 publish 点再核对 scale、offset、ZERO、HMAC 投影、message eventId 与 transport seq。

r48 中最容易误判的边界可收束为十条：

- 空 `pointerIds` 是 default/all-pointer 语义，不是“发送零根手指”。
- 同 token target 合并 pointer geometry，但要求 flags/global scale 完全一致。
- SPLIT 位图与原 pointer 数相等时只是不建 split；非空 pointerIds 仍令各 mode 建 combined entry。
- split 真正执行就换 Motion ID，即使 action 最后没有改变。
- coordinate-combined entry 改 raw 表达但沿用当前 Motion ID。
- 显式 mode 通常换 resolved ID；缺失 hover-enter 修复改 action 却保留当前 ID。
- seq 在 track 前取得，拒绝造成缺口；它不是 `MotionEvent.getId()`。
- `InputState` 在 publish 前更新，是 connection 预期协议账，不是客户端完成账。
- HMAC 只覆盖 Verified 投影；它不认证 event ID、seq 或完整 pointer 数组。
- foreground 以成功入队 entry 计数，可一份根事件多笔，也不要求 handled 为 true。

到这里，第244章已经完成“事件事实 → 接收者计划 → pointer/action/geometry 派生 → resolved 字段 → publish 参数”的闭环。接下来的问题不再是“App 应该看到什么”，而是“这笔已经定型的 `DispatchEntry` 为什么仍可能停在 outbound、何时进入 wait、socket 满时谁重试、FINISHED 怎样按 seq 释放、ANR 与 broken drain 怎样收口”。这正是第245章 `Connection` 队列、socket 背压与 FINISHED 释放链的入口。
