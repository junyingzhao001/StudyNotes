# 234 Android InputChannel、IInputMethodSession与IME输入事件分发完成回执链

本文基于 `android-11.0.0_r48`。第 233 章研究 IME怎样借 `IInputContext`修改文字；本章换到另一条常被混淆的路径：目标 App已经从窗口通道收到一枚 KeyEvent（硬件键只是典型来源）后，为什么还会把它交给当前IME过滤，并等待一份 handled回执。

核心不变量是：**原窗口事件、App发往IME的副本、IME专用Channel的 finished signal、ViewRoot恢复后的最终处理结果和屏幕变化，是不同对象与不同完成点。2500 ms只让App放弃等待这次IME判断，不会撤销已经发送的Channel事件。**

版本锚点：

- `frameworks/base/core/java/android/view/ViewRootImpl.java`
- `frameworks/base/core/java/android/view/ImeFocusController.java`
- `frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java`
- `frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java`
- `frameworks/base/core/java/com/android/internal/view/InputBindResult.java`
- `frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java`
- `frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java`
- `frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java`
- `frameworks/base/core/java/android/inputmethodservice/InputMethodService.java`
- `frameworks/base/core/java/android/view/InputChannel.java`
- `frameworks/base/core/java/android/view/InputEventSender.java`
- `frameworks/base/core/java/android/view/InputEventReceiver.java`
- `frameworks/base/core/jni/android_view_InputEventSender.cpp`
- `frameworks/base/core/jni/android_view_InputEventReceiver.cpp`
- `frameworks/native/libs/input/InputTransport.cpp`

## 1. 先分清三种 InputChannel与两条 Binder方向

本章主链只使用前两类 Channel，但排障时必须把第三类也分开：

| 通道/接口 | 方向 | 承载内容 | 谁完成它 |
| --- | --- | --- | --- |
| 目标窗口 `InputChannel` | InputDispatcher → App ViewRoot | 原始窗口输入事件 | App的窗口 `InputEventReceiver`最终 finish |
| IME Session专用 `InputChannel` | App IMM → IME | 交给当前IME过滤的 Key/Motion副本 | IME的 `ImeInputEventReceiver`回 finished signal |
| IME窗口自己的 `InputChannel` | InputDispatcher → IME窗口 ViewRoot | 用户直接触摸键盘窗口 | IME窗口自己的输入流水线 |
| `IInputContext` Binder | IME → App | commit/composing/delete/sendKeyEvent等编辑命令 | 由第233章的App连接链执行 |
| `IInputMethodSession` Binder | App → IME | selection、extracted text、cursor等状态/控制 | IME Session业务回调 |

一枚目标窗口 KeyEvent的过滤链是：

```text
InputDispatcher
  → 目标App Window Channel
  → ViewRoot pre-IME stages
  → App IMM / IME Session Channel publisher
  → IME Receiver / InputMethodSession
  → Session Channel finished signal
  → App IMM callback
  → ViewRoot finish，或继续 post-IME stages
  → App最终完成原Window Channel事件
```

system_server只负责建立绑定、创建Session Channel pair并交接端点，稳定期不逐枚读取和转发事件数据。IME也不是InputDispatcher之前的全局过滤器：事件先按目标窗口路由到App，才在该 ViewRoot内部经过IME stage。

## 2. ViewRoot怎样决定从 pre-IME还是 post-IME开始

r48在窗口 attach时构造固定 stage链：

```text
NativePreImeInputStage
→ ViewPreImeInputStage
→ ImeInputStage
→ EarlyPostImeInputStage
→ NativePostImeInputStage
→ ViewPostImeInputStage
→ SyntheticInputStage
```

`deliverInputEvent()`并不总从第一层开始。`QueuedInputEvent.shouldSkipIme()`遇到以下情况直接选择 `mFirstPostImeInputStage`：

- 已带 `FLAG_DELIVER_POST_IME`；
- MotionEvent属于 `SOURCE_CLASS_POINTER`；
- MotionEvent来自 `SOURCE_ROTARY_ENCODER`。

所以触屏、鼠标类 pointer和 rotary不仅绕过 `ImeInputStage`，也绕过两个 pre-IME stage。触摸若落在IME窗口，会由InputDispatcher送给IME窗口自己的 Channel；目标App窗口里的普通触摸不会再复制给IME过滤。

没有被跳过的事件先走两次前置机会。`NativePreImeInputStage`只在 App提供 `InputQueue`且事件为 KeyEvent时异步调用 native input queue；`ViewPreImeInputStage`只对 KeyEvent调用 `mView.dispatchKeyEventPreIme()`。任一返回 handled都会直接结束，不再进IME或普通View分发。trackball等非pointer Motion会被这两层直接 forward，然后进入IME stage。

把IME放在 ViewRoot异步stage而非让InputDispatcher先送IME，才能结合当前窗口IME焦点、local-focus模式、App自己的 pre-IME处理以及当前绑定，并在IME不处理时恢复同一窗口的后续分发。

## 3. ImeFocusController与IMM的三种结果怎样控制队列

`ImeFocusController.onProcessImeInputStage()`先检查窗口是否拥有 `mHasImeFocus`、是否设置 `FLAG_LOCAL_FOCUS_MODE`，再取得 IMM；任一不满足都返回 `DISPATCH_NOT_HANDLED`。local-focus窗口自行管理局部焦点，不参加这条系统IME过滤链。

`mHasImeFocus`只是“该窗口有资格把事件交给IME”的窗口级条件，不等于当前一定存在文本编辑器、有效 `InputConnection`或可发送的Session Channel。是否真能发送，还要继续看 `mCurMethod`、`mCurChannel`与Sender状态。

IMM对外只有三种结果：

| 结果 | ViewRoot映射 | 含义 |
| --- | --- | --- |
| `DISPATCH_HANDLED` | `FINISH_HANDLED` | 本IME stage已同步处理，包括不进Channel的SYM特例 |
| `DISPATCH_NOT_HANDLED` | `FORWARD` | 资格门不满足、IMM/Session/Channel不可用或同步发送失败，继续post-IME |
| `DISPATCH_IN_PROGRESS` | `DEFER` | 保留这份 `QueuedInputEvent`，等待 callback再 finish/forward |

`mCurMethod`非 null时，第一次按下且 repeat为0的 `KEYCODE_SYM`是特殊同步分支：IMM显示输入法选择器并立即返回 handled，不写入专用Channel。没有 `mCurMethod`时，连这个特殊分支也不会进入，事件直接 not handled。

调用线程也会改变眼前的完成点。已经在 IMM主Looper时，`dispatchInputEvent()`可以立即尝试Channel发送，因而同步得到 not handled或 in progress；从其他线程调用时，它只投递 asynchronous `MSG_SEND_INPUT_EVENT`便返回 in progress。稍后即使发现 Channel为空或发送失败，也要通过 callback以 false恢复 ViewRoot，不能把最初的 in progress误读成“事件已到IME”。

`ImeInputStage`继承 `AsyncInputStage`，因此队列不是窗口级严格全序。deferred事件的callback可以乱序返回；真正向后续stage传播或完成时，只会被此前尚未传播且 `deviceId`相同的事件阻塞，不同设备的事件可以越过彼此。即使某项已经收到handled或timeout callback，也可能还要等待更早的同设备事件。这里维持的是同一异步stage内的同设备顺序，不是整个窗口的全局FIFO。

## 4. Session Channel pair怎样跨三进程分配所有权

IMMS为一个客户端请求Session时调用 `InputChannel.openInputChannelPair()`。按 `InputChannel`契约，第0端是 publisher/server端，第1端是 consumer/client端：

```text
channels[0] publisher
  → MethodCallback暂持
  → SessionState.channel留在system_server
  → attach时dup一份，经InputBindResult交给App IMM

channels[1] consumer
  → IInputMethod.createSession()
  → IME IInputMethodWrapper主线程
  → IInputMethodSessionWrapper / ImeInputEventReceiver
```

IME创建的是两件配套对象：本地 `InputMethodSession`处理事件与状态，`IInputMethodSessionWrapper`既作为 `IInputMethodSession.Stub`接 Binder状态消息，又持有 consumer端 Receiver。创建回调返回后，IMMS把 session Binder和 publisher端关联进 `SessionState`，App最终同时拿到 session Binder与Channel副本。

跨 Binder parcel的是 fd-backed句柄，不是把同一个Java对象搬过去。远程调用 `createSession()`后，system_server会 dispose自己不再使用的 consumer端本地句柄；返回 `InputBindResult`时又从 `SessionState.channel.dup()`产生用于App的副本。`dispose()`只释放当前持有者的引用，其他进程已复制的端点仍可存活，直到相关引用都关闭。

这也解释了为什么 system_server保留 `SessionState.channel`却不在逐事件热路径上：它用于生命周期与再次交付，真正的 payload由 App publisher直接写给IME consumer。

## 5. IMM Sender何时建立 PendingEvent与2500 ms等待账

App从 `InputBindResult`接收端点后，`setInputChannelLocked()`保存为 `mCurChannel`。第一次真正发送事件时才创建绑定到 IMM主Looper的 `ImeInputEventSender`；`InputEventSender.sendInputEvent()`必须在创建它的同一Looper线程调用。

IMM先取得一个 `PendingEvent`，保存：

```text
原App InputEvent引用
ViewRoot的QueuedInputEvent token
调用 `dispatchInputEvent()`时的 `mCurId`快照（只用于日志）
FinishedInputEventCallback
callback所属Handler
最终handled值
```

主Looper上的关键顺序是：

```text
sender.sendInputEvent(appJavaSeq, event)
  ├─ false → 不进pending表，按not handled回调/返回
  └─ true  → mPendingEvents.put(appJavaSeq, p)
             → 安排2500ms asynchronous timeout
             → 返回DISPATCH_IN_PROGRESS
```

这里的 true只保证**整个事件已成功发布到Session Channel**，不保证IME Receiver已经读到、Session已经调用、handled为真或原窗口事件已经完成。

对带历史样本的 MotionEvent还有一个低层边界：native sender逐样本 publish，Channel若中途填满会整体返回 false，而且只在全部成功后才建立最终 published seq到Java seq的映射。由此可以推断，失败前的部分样本可能已经进入Channel，而App仍把原事件按 not handled继续；不能把 false解释成“IME绝对没有观察到任何字节或样本”。

## 6. 四套 sequence怎样把副本和两条Channel账重新对齐

同一用户动作在这条链上至少涉及四套编号：

| 编号 | 所属对象/通道 | 用途 |
| --- | --- | --- |
| 原Window transport seq | InputDispatcher ↔ App Window Channel | 最终关闭原窗口投递账 |
| App侧 `InputEvent#getSequenceNumber()` | ViewRoot/IMM | `mPendingEvents` key，也是 sender调用者 seq |
| Session Channel `publishedSeq` | native App publisher ↔ IME consumer | 每个实际发布消息的transport编号 |
| IME侧副本 `InputEvent#getSequenceNumber()` | IME Receiver/本地Session | `ImeInputEventReceiver.mPendingEvents`与 `EventCallback` key |

发送 KeyEvent时，native sender创建一个 `publishedSeq`并记录 `publishedSeq → App Java seq`。带历史的 MotionEvent可发布多个 seq；只有整单成功才把最终 published seq映射回这次App调用，consumer内部的 seq chain负责为合批样本补齐底层 finished。

IME native receiver把Session Channel的 `publishedSeq`（合批时是链末seq）连同新建的Java InputEvent交上来，`InputEventReceiver`再保存 `IME Java seq → publishedSeq`。本地Session回调携带IME Java seq；Wrapper用它找到那份IME事件，`finishInputEvent(event, handled)`再还原 published seq发回Channel。App native sender收到 finished后，最后由自己的映射还原 App Java seq，才能命中 IMM PendingEvent。

因此日志里的 seq必须先标所属层。对象地址、`InputEvent.getId()`、App Java seq、Session published seq和原Window transport seq都不能互相替代；尤其不能把Session Channel的 published seq写成原始InputDispatcher的窗口投递编号。

## 7. IME Receiver怎样调用Session，并要求每个事件闭账

`IInputMethodSessionWrapper`把 `ImeInputEventReceiver`绑定到 `context.getMainLooper()`，所以标准IME的Channel回调在IME主线程。收到事件后的顺序是：

1. 若 `mInputMethodSession == null`，立即 `finishInputEvent(event, false)`。
2. 否则以IME Java seq把事件放进 Receiver自己的pending表。
3. KeyEvent调用 `dispatchKeyEvent()`；trackball source调用 `dispatchTrackballEvent()`；其他能到达这里的 MotionEvent调用 `dispatchGenericMotionEvent()`。
4. `EventCallback.finishedEvent(seq, handled)`找回事件、移出pending，再完成底层Channel。

`AbstractInputMethodSessionImpl`默认同步调用 Service并立刻 finished，但接口允许自定义Session稍后回调，所以 Wrapper不能假设业务返回与Channel完成必在同一调用栈。未知或重复 seq会找不到pending项而被静默忽略。

`InputEventReceiver`文档要求每个事件最终调用 `finishInputEvent()`，并写明完成前不会收到新事件；但native consume循环与可容纳多项的pending映射允许多个事件在途，因此实现上不能依赖严格 single-flight。漏掉finish留下的是一笔未闭的transport账，它可能持续累积并最终形成背压或填满Channel，而不保证立刻挡住下一项。

若自定义Session异步处理，业务完成回调还必须切回 Receiver绑定的IME主Looper，再调用 `EventCallback.finishedEvent()`；`InputEventReceiver.finishInputEvent()`要求运行在创建Receiver的同一Looper。App的2500 ms超时只恢复原ViewRoot事件，**不会替IME闭合Session Channel的transport账**。

## 8. 标准InputMethodService会怎样处理Key与Motion

默认 `dispatchKeyEvent()`调用 `event.dispatch(AbstractInputMethodService.this, mDispatcherState, this)`，把 action分派到 Service的 `onKeyDown()`、`onKeyUp()`、`onKeyLongPress()`或 `onKeyMultiple()`。`DispatcherState`维持 tracking/long-press/canceled关系，Back不能只看孤立的 up。

默认Back链是：

```text
DOWN：先给可见ExtractEditText的ActionMode
      → 若handleBack(false)认为IME UI可收起，startTracking并handled=true

UP：再次给ExtractEditText
    → 仅在isTracking且未canceled时调用handleBack(true)真正收起
```

`setBackDisposition()`只上报系统应展示的Back语义，r48默认 `onKeyDown()`并不会自动读取它；自定义IME必须让声明与实际回调一致。

可见且活动的全屏 `ExtractEditText`是另一特例：MovementMethod先尝试移动抽取区光标，而四个DPAD方向键无论 movement是否消费都会被IME吞掉，避免底层App同时换焦点。普通非Back、非全屏特殊键默认多为 false。

Session Channel里的 `SOURCE_CLASS_TRACKBALL`走 `onTrackballEvent()`，其他未被ViewRoot提前跳过的 Motion走 `onGenericMotionEvent()`；两者默认都返回 false。pointer与rotary早在目标App的 `shouldSkipIme()`处离开主链，不能从这两个IME回调的存在推断所有Motion都会到达。

## 9. `finishInputEvent()`怎样穿过JNI回到App Sender

IME本地Session给出 handled后，Receiver的完成仍有多个阶段：

```text
EventCallback.finishedEvent(imeJavaSeq, handled)
→ ImeInputEventReceiver.mPendingEvents用imeJavaSeq找回event
→ InputEventReceiver.mSeqMap以event还原Session publishedSeq
→ native InputConsumer.sendFinishedSignal(publishedSeq, handled)
→ App native InputPublisher.receiveFinishedSignal()
→ publishedSeq映射回App Java seq
→ ImeInputEventSender.onInputEventFinished(appJavaSeq, handled)
→ IMM.finishedInputEvent()
```

`InputEventReceiver.finishInputEvent()`必须在Receiver所绑Looper调用；它无论映射是否有效都会在末尾 `recycleIfNeededAfterDispatch()`。native发送 finished若遇到 `WOULD_BLOCK`，会把信号放进 `mFinishQueue`并监听可写事件后再发，所以“Java finish方法返回”还不等于App端已经收到信号。

正常回到IMM后才移除 App PendingEvent、取消以该 `PendingEvent`对象为标识的timeout，并把 handled交给指定 callback Handler。callback若已在目标Looper就直接运行，否则用 asynchronous Message切过去；运行完才清字段并把容量20的包装对象放回池。

handled只回答“IME是否消费这份过滤副本”。它既不是文字提交结果，也不是布局、绘制、Surface提交或物理present的完成栅栏。

## 10. 超时、迟到finished与换Channel分别怎样收口

r48的 `INPUT_METHOD_NOT_RESPONDING_TIMEOUT`是2500 ms。这个数是 `sendMessageDelayed()`为timeout消息设置的到期时间，不是硬实时完成点：IMM主Looper忙碌时，消息实际执行可以更晚；callback若属于另一个Handler，还要再异步切回 ViewRoot所在Looper。只有 callback真正运行，原事件才从IME stage恢复。timeout处理调用 `finishedInputEvent(seq, false, true)`：从 App pending表移除事件，用 `PendingEvent`中“调用dispatch时的 `mCurId`快照”打印warning，再向 ViewRoot callback false。这个id只是诊断字段，不是Session代数或路由保护。

这个动作不会向IME发送取消，不会替它调用 `finishInputEvent()`，也不会从IME Receiver的pending表删除事件。超时后原ViewRoot可能已在App产生副作用；稍后IME即使发回 handled=true，App表里也找不到seq，直接按 spurious/already finished or timed out忽略，不能撤回App行为。

正常finished则删除与该 `PendingEvent`对象匹配的timeout Message，避免只按 message what误删其他事件。2500 ms是App侧IME过滤等待门，不是所有IME/InputDispatcher ANR的统一判定时刻；它也不会闭合卡在IME侧的Session Channel transport账。

绑定清除或端点变化走另一条路径。`setInputChannelLocked()`以Java对象引用比较 `mCurChannel != channel`，不是比较fd或token值；变化时只为已经成功发布、已经进入 `mPendingEvents`的项逐一投递 asynchronous `MSG_FLUSH_INPUT_EVENT`，再dispose旧Sender和旧Channel。flush消息稍后同样以 handled=false执行，callback不在 IMM锁内重入 ViewRoot。真实finished若先命中，会让对应flush随后自然找不到seq；每个PendingEvent仍只完成一次。

还有一个容易漏掉的窗口：非主线程调用刚排入的 `MSG_SEND_INPUT_EVENT`尚未真正发送，因而还不在pending表，不属于这轮flush。若Channel在该消息执行前已经变化，发送逻辑会使用届时的新 `mCurChannel`/Sender；`PendingEvent.mInputMethodId`仍是最初dispatch调用时的旧id快照。这再次说明该id只供日志使用，不能充当路由或代际校验。

## 11. handled怎样回到ViewRoot并形成第二层finished

IME callback回到 `ImeInputStage.onFinishedInputEvent()`后分两路：

```text
handled=true
  → finish(q, true)
  → 目标View不再收到普通dispatchKeyEvent
  → ViewRoot最终完成原Window事件

handled=false / timeout / send失败 / 无Session
  → forward(q)
  → EarlyPostIme → NativePostIme → ViewPostIme → Synthetic
  → 事件只是获得继续分发的机会；后续stage仍可能因窗口停止、View脱离等门槛丢弃
  → App后续可能消费，也可能最终not handled
  → ViewRoot最终完成原Window事件
```

第一层finished属于IME Session Channel，只释放IME过滤阶段并携带该阶段的handled。第二层属于目标窗口原Channel：仍由App的Window InputEventReceiver在全部stage结束后回给InputDispatcher。IME从未取得原Window端点的完成权。

所以 IME false不是“丢事件”，App仍可能把最终窗口结果变成 handled；IME true也只是让该原事件停止进入目标View。原窗口完成之后若处理触发动画或新帧，仍要另看UI线程、RenderThread、SurfaceFlinger与显示时序。

## 12. IInputMethodSession状态面与Channel数据面并非同一队列

`IInputMethodSession.aidl`是 oneway，列出 selection、extracted text、cursor、completion、private command和 `finishSession()`等方法，却没有 raw `dispatchKeyEvent()`。这些 Binder消息由 Wrapper的 `HandlerCaller`投到IME主Looper；Session Channel的 fd事件也在标准实现中落到同一主Looper，但两者来自不同消息源，不能凭调用墙钟假定跨队列的绝对先后。

`AbstractInputMethodSessionImpl`维护 enabled/revoked：revoke会永久禁止再enable；标准 `InputMethodSessionImpl`的 selection、extracted text、cursor等多种状态回调会检查 `isEnabled()`。但基类的 raw `dispatchKeyEvent/Trackball/GenericMotion`没有这个检查，disabled本身不是Channel硬防火墙。安全还依赖IMMS只交当前端点以及切换时flush/close/finish。

`finishSession()`本身是 oneway，Wrapper收到后仍要经 `HandlerCaller`排到IME主线程；在 `DO_FINISH_SESSION`真正执行前，后续到达Stub的调用仍可能继续入队，只有执行时看到本地Session已为 null的消息才会被丢弃。实际执行会把wrapper内的本地Session置 null，再dispose Receiver；Receiver已经持有同一个consumer端 `InputChannel`，随后wrapper对 `mChannel.dispose()`的第二次本地关闭是安全且幂等的清理。

这个过程不会逐项把 Receiver里尚未完成的事件回为 false。异步Session若迟到回调，Wrapper会先从自己的pending映射移除事件，再尝试在已dispose的Receiver上finish，只能警告且无法发出有效transport signal；若它永不回调，那些映射项也不会被显式逐项清空。App只能靠自身flush或timeout让原ViewRoot前进。

system_server的 `finishSessionLocked()`按程序顺序发出这个 oneway调用、清 `SessionState.session`并dispose自己持有的Channel引用，却不会等待IME主线程跑完 `doFinishSession()`。App在解绑/换Session时独立清理自己的Sender、Channel和pending。三方本地清理相互配合，却没有一个“所有进程都已释放”的同步ACK。native sender观察到HANGUP本身也不会自动向IMM合成一次 false callback；App侧仍要等绑定更新触发flush，或等timeout。

## 13. IME反向 `sendKeyEvent()`为何不会再次进入IME

IME主动产生KeyEvent时调用当前 `InputConnection.sendKeyEvent()`。远端连接经 `IInputContext`到App编辑器Looper；App自定义 `InputConnection`可以覆盖其语义，只有采用 `BaseInputConnection`默认实现时，才会继续调用 `IMM.dispatchKeyEventFromInputMethod()`，选中目标或served ViewRoot并投递 `MSG_DISPATCH_KEY_FROM_IME`。这个本地默认实现返回false，但跨进程 `IInputContext`调用是 oneway，IME侧包装层不会得到并转交这个真实boolean。

ViewRoot在处理该消息时做两件关键事：

1. 若事件带 `FLAG_FROM_SYSTEM`，创建去掉该位的副本，防止第三方IME冒充系统硬件来源。
2. 以 `FLAG_DELIVER_POST_IME`入队，让 `shouldSkipIme()`直接选 post-IME起点，避免 `IME → App → IME → App`递归。这里的 `FLAG_DELIVER_POST_IME`是 `ViewRootImpl.QueuedInputEvent`内部标志，不是 `KeyEvent` flag；前一项的 `FLAG_FROM_SYSTEM`才属于 `KeyEvent`。

这份IME主动生成的新事件以 receiver=null进入App本地队列，不是原InputDispatcher正在等待的硬件事件，也没有原Window Channel的那份finished账。若IME先消费一枚原硬件键、又主动send一枚新键，两者是“原事件完成 + 新事件本地分发”两笔独立事实，sequence不能混用。

`InputMethodService.sendDownUpKeyEvents()`生成的事件带 `FLAG_SOFT_KEYBOARD | FLAG_KEEP_TOUCH_MODE`。它适合 `TYPE_NULL`、控制键或硬件键兼容语义；普通文字仍应使用 composing/commit，因为KeyEvent无法完整表达中文候选、组合span、富文本和相对光标协议。

## 14. 从现象反推第一处分歧

| 现象 | 第一组检查 | 不应直接得出的结论 |
| --- | --- | --- |
| App按键总延迟约2.5秒 | IME Receiver是否收到、Session是否finished、主Looper是否堵塞、Channel是否旧 | 系统已经对IME弹出ANR |
| IME说handled但View仍收到 | 是否超时后才finished、看的是否同一seq、是否为IME主动生成的新KeyEvent | handled可事后撤回App处理 |
| pointer从不进IME callback | `shouldSkipIme()`的pointer/rotary门、事件实际目标窗口 | IME Session失效 |
| trackball进App不进IME | IME focus/local-focus、mCurMethod/Channel、send结果、Session enabled只是软门 | 所有Motion都应走相同路径 |
| Channel send返回false | buffer/端点、是否为带历史样本Motion、随后ViewRoot callback | IME必然一个样本也没看见 |
| 换IME后旧调用继续 | 先区分已发布pending与尚在 `MSG_SEND_INPUT_EVENT`中的待发送项，再核对实际Channel、flush、旧consumer是否已读及迟到finished | enabled位能撤销已发fd数据 |
| `sendKeyEvent()`没有再次进IME | `FLAG_DELIVER_POST_IME`、目标ViewRoot、IInputContext active门 | 事件被InputDispatcher吞掉 |
| finished后界面尚未变化 | 这是Session finished还是原Window finished、后续layout/draw/present | handled就是显示完成 |

排查时固定记录五个维度：**事件来源与source class、当前窗口/Session身份、所属Looper、sequence命名空间、观察点只完成到哪一层。** 只看到一条“send true”“onKeyDown”“timeout”或“finished”日志，都不足以单独还原整条链。

## 15. 九组 macOS 只读源码练习

以下代码块应整段作为脚本运行，只读源码，可在 macOS自带 Bash 3.2和 Zsh 5.9执行；保存后也可把另一个源码根作为第一个参数传入。

### 练习 1：重建 ViewRoot stage与跳过条件

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'InputStage imeStage = new ImeInputStage(earlyPostImeStage,' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mSyntheticInputStage = new SyntheticInputStage();' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'InputStage viewPostImeStage = new ViewPostImeInputStage(mSyntheticInputStage);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'InputStage nativePostImeStage = new NativePostImeInputStage(viewPostImeStage,' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'InputStage earlyPostImeStage = new EarlyPostImeInputStage(nativePostImeStage);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'InputStage viewPreImeStage = new ViewPreImeInputStage(imeStage);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'InputStage nativePreImeStage = new NativePreImeInputStage(viewPreImeStage,' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mFirstInputStage = nativePreImeStage;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mFirstPostImeInputStage = earlyPostImeStage;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'stage = q.shouldSkipIme() ? mFirstPostImeInputStage : mFirstInputStage;' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if ((mFlags & FLAG_DELIVER_POST_IME) != 0) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mEvent.isFromSource(InputDevice.SOURCE_CLASS_POINTER)' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'mEvent.isFromSource(InputDevice.SOURCE_ROTARY_ENCODER));' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if (mView.dispatchKeyEventPreIme(event)) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'final int deviceId = q.mEvent.getDeviceId();' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if (!blocked && deviceId == curr.mEvent.getDeviceId()) {' frameworks/base/core/java/android/view/ViewRootImpl.java
```

分别写出 KeyEvent、pointer Motion、rotary与trackball从哪一层起步，并说明 pre-IME handled后的终点。

### 练习 2：核对IME资格与三返回值

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (!mHasImeFocus || isInLocalFocusMode(windowAttribute)) {' frameworks/base/core/java/android/view/ImeFocusController.java
grep -n -F 'return imm.dispatchInputEvent(event, token, callback, mViewRootImpl.mHandler);' frameworks/base/core/java/android/view/ImeFocusController.java
grep -n -F 'public static final int DISPATCH_IN_PROGRESS = -1;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'public static final int DISPATCH_NOT_HANDLED = 0;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'public static final int DISPATCH_HANDLED = 1;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F '&& keyEvent.getKeyCode() == KeyEvent.KEYCODE_SYM' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'Message msg = mH.obtainMessage(MSG_SEND_INPUT_EVENT, p);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'case InputMethodManager.DISPATCH_IN_PROGRESS:' frameworks/base/core/java/android/view/ViewRootImpl.java
```

推演“无IME焦点、主线程send失败、异线程先返回in progress、SYM首按”四种路径。

### 练习 3：追踪Channel pair三方所有权

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'InputChannel[] channels = InputChannel.openInputChannelPair(cs.toString());' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'MSG_CREATE_SESSION, mCurMethod, channels[1],' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'new MethodCallback(this, mCurMethod, channels[0])));' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'session.channel != null ? session.channel.dup() : null' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (channel != null && Binder.isProxy(method)) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'sessionState.channel.dispose();' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'new IInputMethodSessionWrapper(mContext, session, mChannel);' frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java
grep -n -F 'new ImeInputEventReceiver(channel, context.getMainLooper());' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mParentIMMS.onSessionCreated(mMethod, session, mChannel);' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (res.channel != null && Binder.isProxy(client)) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'setInputChannelLocked(res.channel);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'server channel and should be used to publish input events.' frameworks/base/core/java/android/view/InputChannel.java
grep -n -F 'is designated as the client channel and should be used to consume input events.' frameworks/base/core/java/android/view/InputChannel.java
```

画出 publisher、consumer、system_server保留句柄与App副本，解释为什么局部dispose不等于立刻关闭所有端点。

### 练习 4：验证send成功才建立等待账

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'static final long INPUT_METHOD_NOT_RESPONDING_TIMEOUT = 2500;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mCurSender = new ImeInputEventSender(mCurChannel, mH.getLooper());' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'if (mCurSender.sendInputEvent(seq, event)) {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mPendingEvents.put(seq, p);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mH.sendMessageDelayed(msg, INPUT_METHOD_NOT_RESPONDING_TIMEOUT);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'if the input channel buffer filled before all samples were dispatched.' frameworks/base/core/java/android/view/InputEventSender.java
grep -n -F 'for (size_t i = 0; i <= event->getHistorySize(); i++) {' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'Failed to send motion event sample on channel' frameworks/base/core/jni/android_view_InputEventSender.cpp
```

区分“完整发布、部分Motion样本可能已发布、进入pending、IME处理、finished到达”五个观察点。

### 练习 5：手推timeout、迟到回执与flush

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'int index = mPendingEvents.indexOfKey(seq);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'return; // spurious, event already finished or timed out' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'INPUT_METHOD_NOT_RESPONDING_TIMEOUT + " ms: " + p.mInputMethodId' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mH.removeMessages(MSG_TIMEOUT_INPUT_EVENT, p);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'finishedInputEvent(msg.arg1, false, true);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'finishedInputEvent(msg.arg1, false, false);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mPendingEvents.removeAt(index);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'Message msg = mH.obtainMessage(MSG_FLUSH_INPUT_EVENT, seq, 0);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'private void flushPendingEventsLocked() {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'sendInputEventAndReportResultOnMainLooper((PendingEvent)msg.obj);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'if (mCurChannel != channel) {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'flushPendingEventsLocked();' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'mCurSender.dispose();' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'case MSG_FLUSH_INPUT_EVENT: {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'invokeFinishedInputEventCallback(p, handled);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

分别推演20 ms true、20 ms false、3000 ms true与换Channel四种结局，确认每个 App PendingEvent最多回调一次。

### 练习 6：核对IME Receiver的事件分类与背压

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'channel, context.getMainLooper());' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'if (mInputMethodSession == null) {' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'finishInputEvent(event, false);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mPendingEvents.put(seq, event);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mInputMethodSession.dispatchKeyEvent(seq, keyEvent, this);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mInputMethodSession.dispatchTrackballEvent(seq, motionEvent, this);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mInputMethodSession.dispatchGenericMotionEvent(seq, motionEvent, this);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'public void finishedEvent(int seq, boolean handled) {' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'InputEvent event = mPendingEvents.valueAt(index);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'finishInputEvent(event, handled);' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'No new input events will be received' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'status_t NativeInputEventReceiver::consumeEvents(JNIEnv* env,' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'for (;;) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'gInputEventReceiverClassInfo.dispatchInputEvent, seq, inputEventObj' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
```

解释 custom Session漏调finished时，为何App timeout与IME transport账闭合是两件事，并说明不能把文档描述扩成实现上的严格single-flight保证。

### 练习 7：验证两端sequence翻译

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mSeqMap.put(event.getSequenceNumber(), seq);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'int seq = mSeqMap.valueAt(index);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'uint32_t publishedSeq = mNextPublishedSeq++;' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'mPublishedSeqMap.emplace(publishedSeq, seq);' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'status_t status = mInputPublisher.receiveFinishedSignal(&publishedSeq, &handled);' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'auto it = mPublishedSeqMap.find(publishedSeq);' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'status_t status = mInputConsumer.sendFinishedSignal(seq, handled);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'nativeFinishInputEvent(mReceiverPtr, seq, handled);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'uint32_t seq = it->second;' frameworks/base/core/jni/android_view_InputEventSender.cpp
grep -n -F 'gInputEventSenderClassInfo.dispatchInputEventFinished,' frameworks/base/core/jni/android_view_InputEventSender.cpp
```

给每个 seq标注“原Window transport、App Java、Session Channel `publishedSeq`或IME Java”，不要用一个编号贯穿所有层。

### 练习 8：比较Binder状态门与raw事件门

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oneway interface IInputMethodSession {' frameworks/base/core/java/com/android/internal/view/IInputMethodSession.aidl
grep -n -F 'if (!isEnabled()) {' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'boolean handled = event.dispatch(AbstractInputMethodService.this,' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'public void setEnabled(boolean enabled) {' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'if (!mRevoked) {' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'public void revokeSelf() {' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'mRevoked = true;' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'mEnabled = false;' frameworks/base/core/java/android/inputmethodservice/AbstractInputMethodService.java
grep -n -F 'mInputMethodSession = null;' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mReceiver.dispose();' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'mChannel.dispose();' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'case DO_FINISH_SESSION: {' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'doFinishSession();' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'private void doFinishSession() {' frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java
grep -n -F 'sessionState.session.finishSession();' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

说明disabled、revoked、finish消息已发和IME主线程实际dispose四个状态为什么不能互换。

### 练习 9：验证反向KeyEvent防环与去信任标志

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mIMM.dispatchKeyEventFromInputMethod(mTargetView, event);' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'ic.sendKeyEvent((KeyEvent)msg.obj);' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'viewRootImpl.dispatchKeyFromIme(event);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'event = KeyEvent.changeFlags(event,' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'event.getFlags() & ~KeyEvent.FLAG_FROM_SYSTEM);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'enqueueInputEvent(event, null, QueuedInputEvent.FLAG_DELIVER_POST_IME, true);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'if (q.mReceiver != null) {' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'q.mEvent.recycleIfNeededAfterDispatch();' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'KeyEvent.FLAG_SOFT_KEYBOARD|KeyEvent.FLAG_KEEP_TOUCH_MODE));' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
```

解释去掉 `FROM_SYSTEM`解决信任问题，而 `DELIVER_POST_IME`解决递归问题；再说明 receiver=null意味着缺少哪一笔原窗口完成账。

## 16. 用三条时间线收束完成边界

```text
正常handled=true：
原Window事件到App → pre-IME → Session Channel完整发布 → App pending+timeout
→ IME Java回调 → native finished直接写出或先进入mFinishQueue
→ App publisher收到 → publishedSeq还原为App Java seq
→ IMM移除pending并取消timeout → callback在目标Handler实际运行
→ ImeInputStage finish(q, true)，并受同device前序事件排序约束
→ 原Window finished → 后续画面另行完成

handled=false或发送失败：
Channel不可用/整单发送失败 → 不建立App pending → 同Looper同步NOT_HANDLED或异线程稍后callback false
IME返回false → 已建立App pending → finished到达 → 删除pending与timeout → callback false
→ 同步返回或callback实际运行后，ImeInputStage取得forward机会
→ 若无更早同device事件阻塞，再进入post-IME与App常规分发
→ App最终handled/not handled → 原Window finished

timeout消息被IMM实际处理（2500 ms只是最早到期点）：
App pending移除并安排callback false → callback运行后原事件具备向post-IME推进的条件
→ 仍可能受更早的同device事件阻塞
已发布的Channel事件不会被App timeout取消；若IME Receiver已经接收，其pending也不会被清除
→ 若通道仍存活，迟到finished可关闭对应Session transport账、缓解在途债
→ App已无对应pending，因此迟到handled不能改写timeout已确定的false决策
→ 原事件何时实际推进，仍取决于callback执行和同device排序
```

真正掌握这条链，要能随时回答六个问题：**事件最初属于哪个窗口；当前走的是Window Channel、Session Channel还是 Binder；为什么它从某个stage起步；眼前的seq属于哪一层；谁还欠哪一份finished；这个handled或timeout最多证明到哪个完成点。**
