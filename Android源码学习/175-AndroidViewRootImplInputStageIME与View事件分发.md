# 175 Android ViewRootImpl InputStage、IME 与 View 事件分发

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态只读源码，不要求编译或连接设备  
> 前置章节：第 19、20、52、163、173、174 章

---

## 1. App 收到事件，不等于 Button 立刻收到回调

第 174 章停在 App Looper 从 InputChannel 取出事件。现实排查中却经常出现这些现象：

- `InputDispatcher` 已把事件放进目标窗口的 `waitQueue`，目标 View 还没收到；
- 键盘事件先被输入法处理，过了约 2.5 秒才继续进入应用；
- 手指已经移到另一个 child 上，MOVE 仍发给最初的 child；
- 父容器开始拦截后，旧 child 收到的是 CANCEL，不是原来的 MOVE；
- `FINISHED(handled=true)` 已返回 system_server，`OnClickListener` 却稍后才执行。

一句话结论是：

```text
App 内输入分发不是一次函数调用，
而是 ViewRoot InputStage、可异步 IME、Window.Callback 和有状态 ViewGroup 的组合协议。
```

本章沿一笔事件追到 `ViewRootImpl.finishInputEvent()`，读完应能判断它卡在 IME、阶段链、Activity 回调还是某个 TouchTarget，并能说明 `handled`、CANCEL、click 和跨进程 FINISHED 各自完成了什么。

本章不展开 InputDispatcher 的选窗和 socket 协议，也不把输入完成误写成绘制或 present 完成；它们分别属于第 173、174、163 章。

---

## 2. 先把 handled 拆成三层事实

“返回 true”必须带上所在层级：

| 层级 | 典型结果 | 能证明什么 |
|---|---|---|
| View/子树 | `dispatchTouchEvent()==true` | 这一次树内调用被消费 |
| InputStage | `FINISH_HANDLED` | 后续 stage 不再执行普通处理，事件以 handled 收尾 |
| InputTransport | `FINISHED(seq, handled=1)` | App 的完成结果已沿 channel 返回 Dispatcher |

常见主路径是：

```text
View 返回 true
→ ViewPostImeInputStage 返回 FINISH_HANDLED
→ QueuedInputEvent 标记 FINISHED_HANDLED
→ ViewRootImpl.finishInputEvent
→ InputEventReceiver.finishInputEvent(event, true)
→ FINISHED(seq, handled=1)
```

但这不是唯一来源。IME、native InputQueue、按键 fallback 或 synthetic stage 也可能把事件标成 handled；安全过滤、窗口状态丢弃则可能以未处理结束。

还要分清“处理完成”和“业务完成”：</n+
```text
FINISHED = 输入协议不再等待这笔派发
handled  = 某一层声明消费
click    = View 可能稍后投递的高层动作
present  = 更晚的显示完成边界
```

所以 View 回调返回、Dispatcher 收到 FINISHED、`OnClickListener` 执行、画面上屏是四个不同时间点。

---

## 3. 源码地图与完整阶段链

ViewRoot 与 IME：

```text
frameworks/base/core/java/android/view/
├── ViewRootImpl.java
├── InputEventReceiver.java
├── ImeFocusController.java
└── inputmethod/InputMethodManager.java
```

Window 回调与 View 树：

```text
frameworks/base/core/java/
├── android/app/Activity.java
├── android/view/View.java
├── android/view/ViewGroup.java
├── android/view/MotionEvent.java
├── android/view/TouchDelegate.java
└── com/android/internal/policy/
    ├── DecorView.java
    └── PhoneWindow.java
```

窗口加入成功后，`ViewRootImpl.setView()` 从尾到头组装七个 stage：

```text
NativePreImeInputStage
→ ViewPreImeInputStage
→ ImeInputStage
→ EarlyPostImeInputStage
→ NativePostImeInputStage
→ ViewPostImeInputStage
→ SyntheticInputStage
→ ViewRootImpl.finishInputEvent
```

这是固定连接关系，不代表每笔事件都从第一段开始。普通 pointer MotionEvent 通常跳过前三段，从 `EarlyPostImeInputStage` 进入；合成或重新派发的事件还可以带 flag 选择别的入口。

整条触摸主链可压缩为：

```text
WindowInputEventReceiver
→ ViewRoot pending queue
→ EarlyPostIme
→ NativePostIme（若存在 native InputQueue）
→ ViewPostIme
→ View.dispatchPointerEvent
→ DecorView → Activity → PhoneWindow → DecorView.super
→ ViewGroup / child
→ SyntheticInputStage（前面都未处理时）
→ ViewRoot finish → InputTransport FINISHED
```

DecorView 出现两次不是递归错误：第一次把事件交给 `Window.Callback`，第二次的 `superDispatchTouchEvent()` 进入继承自 ViewGroup 的普通树分发。

---

## 4. QueuedInputEvent 把接收队列和阶段状态分开

ViewRoot 用轻量对象包装输入：

```java
InputEvent mEvent;
InputEventReceiver mReceiver;
int mFlags;
QueuedInputEvent mNext;
```

关键 flag 的职责是：

| flag | 含义 |
|---|---|
| `DELIVER_POST_IME` | 从 post-IME 入口开始 |
| `DEFERRED` | 当前 async stage 尚未完成 |
| `FINISHED` | 结果已经确定，后续 stage 只转发 |
| `FINISHED_HANDLED` | 最终 handled=true |
| `RESYNTHESIZED` | 已进入 synthetic 处理 |
| `UNHANDLED` | 请求从未处理输入合成新事件 |
| `MODIFIED_FOR_COMPATIBILITY` | 结束前还要走兼容还原 |

`mReceiver != null` 才表示链尾要对原始 channel 事件发送 FINISHED。App 或 IME 在本地合成并塞回 ViewRoot 的事件可以没有 receiver；链尾只回收对象，不能伪造一个不存在的 Dispatcher 派发序号。

### 4.1 pending queue 按入队顺序，不按 eventTime 重排

`enqueueInputEvent()` 总是先连到 `mPendingInputEventHead/Tail`。`processImmediately=true` 只是立即 drain，同样经过队列计数；时间戳可能来自 App/IME 注入，源码明确不信任它严格单调。

```text
入队顺序 = ViewRoot 的处理顺序基线
eventTime = 事件时间信息，不是此队列的排序键
```

`doProcessInputEvents()` 会把 pending 节点逐个摘下后送入 stage。某一笔若 DEFER，外层仍可继续取后续节点；异步 stage 自己负责必要的次序约束。

### 4.2 一个 stage 只有四种决策

```text
FORWARD            → 交给下一 stage
FINISH_HANDLED     → 标记完成、handled=true
FINISH_NOT_HANDLED → 标记完成、handled=false
DEFER              → AsyncInputStage 暂存，等待回调
```

`finish()` 不是立即跳到函数栈外，而是设置 `FLAG_FINISHED` 再 forward。后续 stage 的 `deliver()` 看到该 flag 就跳过 `onProcess()`，一路传到链尾。

---

## 5. 窗口状态变化会丢事件，但终止事件有例外

每个 stage 在处理前都调用 `shouldDropInputEvent()`。r48 的主要条件包括：

```text
根 View 已移除
窗口无焦点且事件不是 pointer，并且 Autofill UI 未显示
ViewRoot 已 stopped
ambient mode 下事件不是 button source
transition pause 中且事件不是 BACK
```

这里有两个不同分支，不能合成一句“UP/CANCEL 永远不会丢”：

1. `mView == null || !mAdded` 时直接返回 true，终止事件也会被丢弃并以未处理结束；
2. 其他 focus/stopped/ambient/transition 条件下，非终止事件被丢弃，终止事件则先 `event.cancel()`，再继续向下游分发。

第二种设计避免旧手势只丢 UP/CANCEL 后让 View 永久保留 pressed、drag 或 gesture 状态。下游看到的是已取消的终止事件，不是原动作一定原样到达。

`ViewPostImeInputStage.processKeyEvent()` 在 View、shortcut、fallback 等关键尝试之间还会重新检查 drop 条件，因为事件在前一次回调期间可能失焦或 stopped。状态门不是只在链首拍一次快照。

---

## 6. 哪些事件绕过 IME，native stage 又何时存在

`deliverInputEvent()` 的入口选择是：

```java
stage = q.shouldSkipIme()
        ? mFirstPostImeInputStage
        : mFirstInputStage;
```

r48 中 `shouldSkipIme()` 对以下事件返回 true：

```text
带 FLAG_DELIVER_POST_IME
MotionEvent 且 source 属于 SOURCE_CLASS_POINTER
MotionEvent 且 source 是 SOURCE_ROTARY_ENCODER
```

因此触摸、鼠标 pointer 与旋转编码器通常不会走 `NativePreIme → ViewPreIme → Ime`；普通键盘 KeyEvent 才是 IME 路径的典型对象。IME 主动回注的 key 带 `DELIVER_POST_IME`，也不会再次送回 IME 形成循环。

### 6.1 ViewPreIme 沿焦点路径先给 View 一次机会

`ViewPreImeInputStage` 只处理 KeyEvent：

```java
if (mView.dispatchKeyEventPreIme(event)) {
    return FINISH_HANDLED;
}
```

ViewGroup 会沿当前 focused child 继续派发，普通 View 最终进入 `onKeyPreIme()`。它适合在 IME 之前观察 BACK 等按键，但这个“之前”只指 App 内 IME stage 之前；系统早已选窗并把事件送进目标进程。

### 6.2 NativePre/PostIme 依赖 RootViewSurfaceTaker

只有根 View 提供 `InputQueue` 时，这两段才把事件异步交给 native：

```text
NativePreIme：仅 KeyEvent，IME 前
NativePostIme：尚未完成的各类输入，IME 后
```

native 回调 `handled=true` 就 finish；false 才继续 Java 链。普通 Java Activity 通常没有 `mInputQueue`，这两段只是 FORWARD，不能把所有 Activity 输入都画成 Java→native→Java。

---

## 7. IME 是嵌套的异步派发，2.5 秒后会放行

`ImeFocusController` 先检查：

```text
ViewRoot 是否有 IME focus
窗口是否设置 FLAG_LOCAL_FOCUS_MODE
能否取得 InputMethodManager
```

不满足时返回 `DISPATCH_NOT_HANDLED`，事件直接继续 post-IME。满足时，IMM 的结果有三种：

| 返回值 | ImeInputStage 行为 |
|---|---|
| `DISPATCH_HANDLED` | `FINISH_HANDLED` |
| `DISPATCH_NOT_HANDLED` | `FORWARD` |
| `DISPATCH_IN_PROGRESS` | `DEFER`，等异步 callback |

这是一条嵌套链：

```text
InputDispatcher → 目标 App
目标 App 的 IMM → 当前 IME input channel
IME FINISHED → App 的 ImeInputStage callback
App ViewRoot FINISHED → InputDispatcher
```

### 7.1 2500ms 超时不是系统输入 ANR

IMM 成功 `sendInputEvent()` 后，以 Java event sequence 为 key 放入 `mPendingEvents`，并在主 Looper 安排 2500ms 异步超时消息。超时时：

```text
从 mPendingEvents 移除
记录 IME timeout 日志
以 handled=false 回调 ImeInputStage
原事件继续 EarlyPostIme / View 树
```

迟到的 IME FINISHED 再按 seq 查找时已经找不到 pending 项，会被当作 spurious 直接忽略，不会二次完成原事件。

第 174 章的目标窗口默认约 5 秒 InputDispatcher 超时仍在外层计时。IME 的 2.5 秒保护不是 ANR，也不会重置外层 deadline；它已消耗掉 App 完成预算的一大段。

### 7.2 AsyncInputStage 只保证同 deviceId 不越序

DEFER 的事件进入当前 stage 自己的 queue。某事件准备 forward 时会扫描更早条目：

```text
前面存在同 deviceId 事件 → 当前继续排队
只存在不同 deviceId 事件 → 当前可以向下游前进
更早事件完成 → 释放同 deviceId 中已不再 deferred 的后继
```

这是局部设备顺序，不是整个窗口的全局串行。于是 ViewRoot pending queue 已空，IME/native async queue 仍可能持有事件；不同设备的 FINISHED 也不必严格按 Dispatcher waitQueue 队头返回。

---

## 8. post-IME 先做模式修正，再进入 Window 回调

`EarlyPostImeInputStage` 不只是过路节点。对 pointer，它会：

- 按兼容 translator 把屏幕坐标转到 App window；
- DOWN/SCROLL 时进入 touch mode；
- DOWN 时隐藏 Autofill UI 与 tooltip；
- 按 `mCurScrollY` 修正位置；
- 保存最近 raw touch point 与 source，供拖拽等逻辑使用。

所以 ViewGroup 看到的坐标已经经历多层处理：

```text
Dispatcher 的窗口 offset/scale
→ MotionEvent 的窗口局部坐标
→ ViewRoot compatibility / scroll 修正
→ ViewGroup 的 child offset / inverse matrix
```

`ViewPostImeInputStage` 再按类型/source 分流：

```text
KeyEvent                  → processKeyEvent
SOURCE_CLASS_POINTER      → processPointerEvent
SOURCE_CLASS_TRACKBALL    → processTrackballEvent
其他 MotionEvent          → processGenericMotionEvent
```

`View.dispatchPointerEvent()` 还会按 `event.isTouchEvent()` 二分：touch-like pointer 进入 `dispatchTouchEvent()`，hover/scroll 等进入 `dispatchGenericMotionEvent()`。设备是鼠标，不代表所有鼠标事件都走 touch。

### 8.1 Activity 与 DecorView 的回环

触摸的标准调用顺序是：

```text
ViewRootImpl
→ DecorView.dispatchTouchEvent
→ Window.Callback（通常 Activity）.dispatchTouchEvent
→ Activity.onUserInteraction（仅 DOWN）
→ PhoneWindow.superDispatchTouchEvent
→ DecorView.superDispatchTouchEvent
→ ViewGroup.dispatchTouchEvent
```

若 View 树返回 false，`Activity.dispatchTouchEvent()` 最后还调用 `Activity.onTouchEvent()`。因此 Activity override 若不调用 super，可以在普通树分发前截断整条路径；而 DecorView 的第二次入口使用 `super`，不会再次回调 Activity。

按键链还包含 ActionBar、Window callback、unhandled-key listener、shortcut、`FallbackEventHandler` 与焦点导航。定位“EditText 没收到 key”时，只看 `EditText.onKeyDown()` 远远不够。

---

## 9. ViewGroup 在 DOWN 建立 TouchTarget，而不是每次重做命中

每个新 `ACTION_DOWN` 一开始都执行：

```java
cancelAndClearTouchTargets(ev);
resetTouchState();
```

它防御上一手势因 App 切换、ANR 或事件丢失而残留的 child target、disallow-intercept 等状态。新的 DOWN 是新一代手势账本。

父容器未拦截时，ViewGroup 按触摸绘制顺序从视觉前层向后扫描 child：

```text
child 可接收 pointer（VISIBLE 或仍有 animation）
+ 触点经 child 逆矩阵后落在其 bounds
→ dispatchTransformedTouchEvent(DOWN, child)
→ child 返回 true：建立 TouchTarget，停止扫描
→ child 返回 false：继续尝试下一个候选
```

几何命中只产生候选；DOWN 的返回值才决定普通手势归属。`TouchTarget` 记录的是：

```text
child 引用 + 归属该 child 的 pointerIdBits + next
```

### 9.1 后续 MOVE 对已选目标是“粘住”的

建立 `mFirstTouchTarget` 后，普通 MOVE 不再按当前位置换 child，而是沿 target 链分发。手指移出按钮边界不会自动把整段 gesture 转给旁边按钮；原 child 可以依据坐标改变 pressed 状态，但身份仍是原 target。

若 DOWN 没有任何 child 返回 true，`mFirstTouchTarget` 为空，ViewGroup 才把事件当作普通 View 发给自己。此后非 DOWN 且无 target 时，源码直接令 `intercepted=true`，不会在中途重新寻找一个 child。

这也是为什么“MOVE 已落在 B 上，B 却没回调”通常不是 hit-test bug：应先查 DOWN 时谁成为 target。

---

## 10. 中途拦截的当前事件只以 CANCEL 送给旧 child

ViewGroup 只有在下面任一条件成立时才询问 `onInterceptTouchEvent()`：

```text
当前是 ACTION_DOWN
或者 mFirstTouchTarget != null
```

没有 target 的后续事件直接视为 intercepted。调用 intercept 前保存原 action，返回后用 `ev.setAction(action)` 恢复，避免自定义实现意外改 action 污染后续框架逻辑。

若父容器在某个 MOVE 上第一次返回 true，遍历旧 target 时：

```text
cancelChild = intercepted
→ dispatchTransformedTouchEvent(ev, cancel=true, child)
→ 临时把当前事件 action 改成 ACTION_CANCEL
→ child.dispatchTouchEvent(CANCEL)
→ 恢复原 action
→ 移除并回收 TouchTarget
```

关键边界是：这一次原始 MOVE 不会在同一个 `dispatchTouchEvent()` 调用中再送给父容器的 `onTouchEvent()`。该轮只用它给旧 child 发 CANCEL；到下一笔事件，因为 target 已空，父容器才按自己的普通 View 路径接收。

因此日志可以合法地长这样：

```text
child: DOWN, MOVE, CANCEL
parent: 下一笔 MOVE, UP
```

### 10.1 disallow intercept 不是永久授权

child 调用 `requestDisallowInterceptTouchEvent(true)` 后，标志会向祖先传播；状态不变的重复请求直接返回。只要标志仍在，本手势后续检查会跳过 `onInterceptTouchEvent()`。

但新 DOWN 先执行 `resetTouchState()`，其中会清除 disallow 标志。所以它：

- 保护当前 gesture 的后续事件；
- 不禁止祖先处理新的 DOWN；
- 也不阻止框架因 CANCEL、detach 或新手势进行状态清理。

把它理解成“子 View 永久拥有触摸权”会产生错误预期。

---

## 11. 多指 split 会重写 pointer 集合和 action

targetSdk 较新的应用中，ViewGroup 默认可启用 `FLAG_SPLIT_MOTION_EVENTS`；鼠标事件明确不走这套 split。新 pointer DOWN 时，ViewGroup 可为该 pointer 再找一个 child，并把对应 id bit 加进目标账本。

假设：

```text
pointer 0 → child A
pointer 1 → child B
```

向 A 分发时只保留 id 0，向 B 分发时只保留 id 1。`MotionEvent.split(desiredPointerIdBits)` 会根据剩余 pointer 改写 action：某 child 眼中的 `POINTER_DOWN/UP` 可能变成自己的 `DOWN/UP`，也可能在动作 pointer 不属于它时变成 MOVE。

若新 pointer 没找到新 child，但已有 target，源码把它分给“最早加入”的 target：链表是头插法，代码走到 `next == null` 的 least recently added 节点再 OR id bit。

### 11.1 child 坐标不是简单减 left/top

`dispatchTransformedTouchEvent()` 有两条路径：

```text
pointer 集合相同且 child matrix 为 identity
→ 原 event 临时 offset，派发后再 offset 回去

需要裁 pointer 或 child 有非 identity matrix
→ split/复制 event
→ offset(parent scroll - child left/top)
→ transform(child.getInverseMatrix())
→ 派发并 recycle 副本
```

所以旋转、缩放后的 child 命中与坐标都依赖逆矩阵。抓日志时要注明坐标空间；把 Dispatcher 的 window-local 坐标直接和深层 View 的 `event.getX/Y()` 比较，可能差了多次变换。

CANCEL 是特殊路径：重点是动作而非 pointer 内容，源码临时改 action 后直接派发，不执行普通 pointer 过滤与坐标变换。

---

## 12. View 内部还有安全、监听器、代理和默认行为四道门

普通 `View.dispatchTouchEvent()` 的核心顺序是：

```text
accessibility focus 目标检查
→ ACTION_DOWN 时停止旧 nested scroll
→ onFilterTouchEventForSecurity
→ enabled 时 scrollbar dragging
→ enabled 时 OnTouchListener.onTouch
→ 若仍未处理，onTouchEvent
→ UP/CANCEL 或未处理 DOWN 时停止 nested scroll
```

### 12.1 obscured 过滤只检查完全遮挡 flag

当 `FILTER_TOUCHES_WHEN_OBSCURED` 开启时，r48 默认安全过滤检查：

```java
event.getFlags() & MotionEvent.FLAG_WINDOW_IS_OBSCURED
```

它没有在这段默认实现中同时检查 `FLAG_WINDOW_IS_PARTIALLY_OBSCURED`。若过滤失败，listener 和 `onTouchEvent()` 都不执行，调用返回 false。ViewGroup 自己也是 View，因此进入其 child 路由前同样先经过这道过滤。

### 12.2 OnTouchListener 只在 enabled 时先于 onTouchEvent

监听器存在、View enabled 且 `onTouch()` 返回 true，就不再调用 `onTouchEvent()`。若 listener 返回 false，才继续默认行为。

disabled View 不调用 listener；但 `onTouchEvent()` 对仍标为 clickable/long-clickable/context-clickable 的 disabled View 会返回 true，只是不产生正常交互动作。这让 disabled 控件仍可阻止触摸穿透，不能用“disabled 必然返回 false”推断路由。

### 12.3 TouchDelegate 在 onTouchEvent 内部

`mTouchDelegate.onTouchEvent()` 位于 enabled 检查之后、普通 clickable 状态机之前。它不是 `dispatchTouchEvent()` 最外层的前置过滤器，也不会让 disabled View 进入代理逻辑。

最终消费优先级可记为：

```text
安全过滤
→ enabled OnTouchListener
→ onTouchEvent 内的 disabled 快路径
→ enabled TouchDelegate
→ clickable / tooltip 默认状态机
```

---

## 13. UP 被 handled，不代表 OnClick 已执行

clickable View 在有效 UP 上不会总是同步调用 `performClickInternal()`。正常路径创建 `PerformClick` 并 `post()`：

```java
if (!post(mPerformClick)) {
    performClickInternal();
}
```

异步 post 的理由是先让 pressed 等视觉状态更新。于是可能出现：

```text
UP → View.onTouchEvent 返回 true
→ ViewRoot InputStage finish handled
→ FINISHED 返回 Dispatcher
→ 主 Looper 稍后运行 PerformClick
→ OnClickListener 执行业务
```

只有 `post()` 失败才同步 fallback。因此输入系统的“App 已处理完 UP”不等待点击业务，更不等待点击触发的网络、数据库、绘制或 Surface present。

### 13.1 cancelPendingInputEvents 不是 ACTION_CANCEL

`View.cancelPendingInputEvents()` 的目标是清除已经 post 的高层输入动作：默认 `onCancelPendingInputEvents()` 移除 pending click 并取消 long press，ViewGroup 还会向 child 传播。

它不会凭空向正在进行的 touch target 派发一个 MotionEvent CANCEL，也不是 InputDispatcher 的取消协议。反过来，收到 ACTION_CANCEL 会清理触摸状态，但不能由方法名推断所有自定义 Handler callback 都自动删除；自定义 View 应 override 并移除自己投递的高层任务。

这两个“cancel”解决不同问题：

```text
ACTION_CANCEL             → 结束低层 gesture 状态
cancelPendingInputEvents  → 撤销已排队的高层输入动作
```

---

## 14. 未处理事件、synthetic 与 unbuffered 的边界

若 `ViewPostImeInputStage` 没有消费，事件继续到 `SyntheticInputStage`。r48 会对这些来源合成行为：

```text
trackball         → SyntheticTrackballHandler
joystick          → SyntheticJoystickHandler
touch navigation  → SyntheticTouchNavigationHandler
FLAG_UNHANDLED key→ SyntheticKeyboardHandler
```

它们进入 synthetic 后被标记 `RESYNTHESIZED`，相应处理器可生成新的 key。普通未处理 touchscreen MotionEvent 不会被 synthetic 神奇地转成点击；它最终以 handled=false 完成。

还要区分两层 key fallback：

- App 内 `FallbackEventHandler` / `SyntheticKeyboardHandler`；
- 第 174 章中 Dispatcher 收到未处理 key 后，向 policy 请求 fallback key 的系统层路径。

两者不是同一个对象，也不应把普通 Motion 的 false 类推为系统必然重派。

### 14.1 requestUnbufferedDispatch 影响后续取 batch 节奏

View 在处理 pointer 时可请求 unbuffered dispatch。`ViewPostImeInputStage` 在当前回调之后才观察 `mUnbufferedDispatchRequested`，设置 ViewRoot 状态并把已安排的 batched consume 改为立即消费；手势终止时再恢复普通 batched 调度。

因此它不会让当前 MotionEvent 重新派发一次，也不保证硬件每个 sample 变成独立 Java 对象。它改变的是后续 batch 消费调度，代价是更高消息/回调频率。

hover 还会触发 pointer icon 与 tooltip 更新；touch exploration 开启时 tooltip hover 会被跳过。辅助功能目标 flag 也会优先尝试 accessibility focused host，但失败后可清 flag 并回到普通 child 扫描。它们都是主链上的条件分支，不是另一套独立 InputChannel。

---

## 15. 用一笔手势建立诊断树，并完成九组静态练习

假设 child A 在 DOWN 返回 true，父容器在第二个 MOVE 开始拦截，A 的 CANCEL 返回 true，父容器随后处理 UP：

```text
1. DOWN 进入 ViewRoot，pointer 跳过 IME
2. ViewGroup reset，命中 A；A 返回 true，建立 TouchTarget(A)
3. MOVE1 不重做普通 hit-test，继续发 A
4. MOVE2 的 onInterceptTouchEvent 返回 true
5. MOVE2 被临时改成 CANCEL 发 A，随后移除 target
6. MOVE2 不在同轮再发父 onTouchEvent
7. UP 到来时无 target，ViewGroup 按自身路径处理
8. stage 链得出 handled，ViewRoot 对原 seq finish
```

现场可先按症状分层：

| 现象 | 优先检查 | 不要先下的结论 |
|---|---|---|
| Dispatcher wait 有超龄 key | App Looper、IME pending、async stage queue | 一定卡在目标 View |
| touch 完全没进 child | DOWN 的 security/intercept/hit-test/返回值 | MOVE 坐标命中就应切 target |
| child 收到 CANCEL | 父中途 intercept、窗口/手势状态变化 | Dispatcher 一定主动发了 CANCEL |
| `onTouch` 有日志、`onTouchEvent` 无日志 | listener 是否返回 true、View 是否 enabled | ViewRoot 没派发 |
| FINISHED 后才看到 click | `PerformClick` 是否 post | 输入回执顺序错误 |
| ViewRoot pending=0 仍未完成 | async stage queue、receiver finish queue | App 内已无输入账 |

以下命令只读当前 `android-11.0.0_r48` 工作树。

### 练习 1：复原七段链与两个入口

```bash
sed -n '1138,1172p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '7868,7910p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '8050,8090p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

画出完整顺序，并列出 pointer、rotary、IME 回注 key 从哪里开始。

### 练习 2：证明 pending queue 不按时间戳排序

```bash
sed -n '7965,8045p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

找出尾插、`processImmediately` 与 drain，说明 eventTime 只更新 frame info 而不参与排序。

### 练习 3：区分两类 drop 与 terminal cancel

```bash
sed -n '5287,5415p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

回答 root removed 为什么没有 terminal 例外，以及 stopped 条件下 UP 怎样继续下游。

### 练习 4：闭合 IME 异步与迟到回执

```bash
sed -n '218,250p' frameworks/base/core/java/android/view/ImeFocusController.java
sed -n '2580,2718p' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
sed -n '5560,5685p' frameworks/base/core/java/android/view/ViewRootImpl.java
```

标出 `IN_PROGRESS → DEFER → timeout handled=false → forward`，以及迟到 seq 为何被忽略。

### 练习 5：解释 Activity/DecorView 回环

```bash
sed -n '435,495p' frameworks/base/core/java/com/android/internal/policy/DecorView.java
sed -n '1864,1878p' frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java
sed -n '4110,4135p' frameworks/base/core/java/android/app/Activity.java
```

指出哪一次调用进入 Window.Callback，哪一次 `super` 真正进入 ViewGroup。

### 练习 6：手算 DOWN 与中途拦截

```bash
sed -n '2633,2835p' frameworks/base/core/java/android/view/ViewGroup.java
```

用 DOWN、MOVE1、MOVE2(intercept)、UP 推演 `mFirstTouchTarget` 和 child/parent 实际收到的 action。

### 练习 7：核对 split 与坐标变换

```bash
sed -n '2700,2785p' frameworks/base/core/java/android/view/ViewGroup.java
sed -n '3076,3145p' frameworks/base/core/java/android/view/ViewGroup.java
rg -n 'split\(' frameworks/base/core/java/android/view/MotionEvent.java
```

说明新 pointer 找不到 child 时归谁，并验证 inverse matrix 在哪条路径应用。

### 练习 8：写出 View 内精确消费顺序

```bash
sed -n '14275,14345p' frameworks/base/core/java/android/view/View.java
sed -n '15660,15745p' frameworks/base/core/java/android/view/View.java
```

比较 obscured、disabled、OnTouchListener、TouchDelegate、clickable 五个条件的先后。

### 练习 9：证明 FINISHED 可以早于 click

```bash
sed -n '15700,15750p' frameworks/base/core/java/android/view/View.java
sed -n '8090,8125p' frameworks/base/core/java/android/view/ViewRootImpl.java
sed -n '20570,20635p' frameworks/base/core/java/android/view/View.java
```

分别标出 `post(mPerformClick)`、receiver finish 与 pending high-level callback 清理，写出三个完成边界。

---

## 16. 本章结论与自检

最终模型可以压缩成四本账：

```text
ViewRoot 接收账
= pending queue + QueuedInputEvent flags + receiver

异步阶段账
= native/IME DEFER queue + 同 deviceId 顺序

ViewGroup 手势账
= DOWN 选中的 TouchTarget + pointerIdBits + intercept/disallow 状态

跨进程完成账
= ViewRoot finish → InputEventReceiver → FINISHED(seq, handled)
```

最值得记住的边界是：

- pointer/rotary 通常从 post-IME 开始，普通 key 才典型地经过 pre-IME 与 IME；
- IME 2.5 秒超时会以未处理放行，迟到回执被忽略，外层输入 ANR 预算并未重置；
- async stage 只阻止同 deviceId 越过更早事件；
- root removed 会连终止事件一起丢，其他部分状态门则把终止事件 cancel 后继续；
- ViewGroup 在 DOWN 建目标，MOVE 不按当前位置普通换 child；
- 父中途拦截时，当前 Motion 只以 CANCEL 送旧 child，不在同轮再送父；
- disallow intercept 只约束当前手势，并在新 DOWN 重置；
- disabled clickable View 仍可消费，TouchDelegate 位于 `onTouchEvent()` 内；
- 正常 click 通过 post 延后，FINISHED 不等待 click、业务任务、绘制或 present。

自检时应能回答：

1. 七个 InputStage 的真实顺序与两种入口是什么？
2. `FINISH_HANDLED` 为什么仍要穿过后续 stage？
3. ViewRoot pending queue 已空时，事件还能藏在哪里？
4. IME 超时为何既不是丢事件，也不是重置 Dispatcher deadline？
5. stopped 时的 MOVE 和 UP 为什么可能走不同结果？
6. child 为什么必须在 DOWN 返回 true 才能获得普通后续 gesture？
7. 父在 MOVE2 拦截时，父与 child 各收到什么？
8. split 后某 child 看到的 action 为什么可能改变？
9. OnTouchListener、TouchDelegate 与 `onTouchEvent()` 的顺序是什么？
10. 为什么 handled=true、OnClick 已执行和画面 present 不能画成同一个点？

下一章进入 **InputDispatcher 的 ANR 发现、policy 回调、系统与应用现场收集**，把第 174—175 章的 wait entry 和 App 完成链接到超时处置。
