# 233 Android InputConnection、IInputContext与IME文本编辑命令回传链

本文基于 `android-11.0.0_r48`。第 232 章把“焦点、绑定、显示、写回”串成总链；本章把镜头固定在已经建立的编辑协议上，回答一笔 composing、commit、query、selection feedback、cursor geometry 或 rich content 怎样跨越进程与线程，以及每个返回值究竟停在哪个完成点。

本章最重要的不变量是：**IME 调用本地 `InputConnection`外观、oneway 事务进入 App、App Handler开始执行、真实编辑器返回、Editable变化、反向状态事务发出、IME Session处理、View重绘和像素 present，是彼此不同的完成点。**

版本锚点：

- `frameworks/base/core/java/android/view/inputmethod/InputConnection.java`
- `frameworks/base/core/java/com/android/internal/view/IInputContext.aidl`
- `frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java`
- `frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java`
- `frameworks/base/core/java/com/android/internal/inputmethod/CancellationGroup.java`
- `frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java`
- `frameworks/base/core/java/com/android/internal/widget/EditableInputConnection.java`
- `frameworks/base/core/java/android/widget/TextView.java`
- `frameworks/base/core/java/android/widget/Editor.java`
- `frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java`
- `frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java`
- `frameworks/base/core/java/android/inputmethodservice/IInputMethodSessionWrapper.java`
- `frameworks/base/core/java/android/inputmethodservice/InputMethodService.java`
- `frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java`
- `frameworks/base/services/core/java/com/android/server/inputmethod/InputContentUriTokenHandler.java`

## 1. 先画出两条直连数据面和九个完成点

IMMS负责选中用户、客户端、IME、Session和端点。稳定编辑期间，普通文字不再让 system_server逐字中转，而是走两条方向相反的直连链：

```text
IME → App 命令线：
IME UI
  → IME侧 InputConnectionWrapper
  → oneway IInputContext
  → App侧 IInputConnectionWrapper
  → 编辑器指定 Looper
  → EditableInputConnection / 自定义 InputConnection
  → Editable、TextWatcher、布局与绘制

App → IME 状态线：
Editor / 自定义编辑器
  → App侧 InputMethodManager
  → oneway IInputMethodSession
  → IME侧 IInputMethodSessionWrapper
  → IME主线程 Session
  → onUpdateSelection / onUpdateExtractedText / onUpdateCursorAnchorInfo
```

富内容是例外中的控制面插曲：IME在 `commitContent()`前可能经 privileged operations让 IMMS创建 URI token；真正的内容提交仍走 `IInputContext`直连 App。

诊断时把“成功”拆开：

| 观察 | 最多能证明 | 仍不能证明 |
| --- | --- | --- |
| 远端 `commitText()`返回 `true` | 调用 `IInputContext`未抛 `RemoteException` | App Handler已执行、真实连接接受、文字已绘制 |
| App真实 `commitText()`返回 `true` | 目标 Looper已调用到该实现并取得其本地返回值 | IME同步得知该结果、反向状态已处理、像素已 present |
| query返回非 null | callback最迟在 `await()`后的 `hasValue()`检查前提供一个值 | 值仍属于当前焦点或之后不会再变 |
| `requestCursorUpdates()`返回 `true` | 该具体App实现已执行并callback真 | `CursorAnchorInfo`已经构造或到达IME |
| `commitContent()`返回 `true` | App callback表示接受请求 | URI已经打开、内容加载或显示完成 |
| `onUpdateSelection()`到达 | IME处理了一份 App上报的逻辑坐标 | 它对应最后一笔命令或屏幕已经重绘 |

## 2. 三个 Wrapper 与两类 oneway 不要认错

源码里有三个近名对象：

| 类 | 常见所在侧 | 角色 |
| --- | --- | --- |
| `com.android.internal.view.InputConnectionWrapper` | IME进程 | 把远端 `IInputContext`包装成公开 `InputConnection`外观 |
| `com.android.internal.view.IInputConnectionWrapper` | App进程 | `IInputContext.Stub`，把 Binder入口转换成目标 Looper上的消息 |
| `android.view.inputmethod.InputConnectionWrapper` | App/库内 | 普通本地装饰器，与前两者不是同一跨进程桥 |

`IInputContext`整个接口声明为 `oneway`。写命令如 `commitText()`、`setComposingText()`、`setSelection()`、两类 delete、begin/end batch和 editor action没有结果 callback；IME侧 wrapper通常在 Binder调用没有抛异常后直接返回 `true`。

查询或需要接受结果的操作仍是 oneway，但携带另一个 callback Binder：

| 类型 | 例子 | callback值 |
| --- | --- | --- |
| 文本查询 | before/after/selected、extracted | `CharSequence`或 `ExtractedText` |
| 数值查询 | caps mode | `int` |
| 请求确认 | cursor updates、commit content | `1/0`转成 boolean |

这些 callback接口本身也是 oneway。所谓“同步样式”来自 IME侧 wrapper在本地等待 `Completable`，不是一次双向 Binder调用栈。

反向的 `IInputMethodSession`同样是 oneway。App IMM已经持有当前 Session，所以 `updateSelection()`等直接调用它；IME侧 wrapper再用 `HandlerCaller`切到 Session所属线程。App的 oneway返回不表示 `InputMethodService.onUpdateSelection()`已经运行。

本地 Binder可能压缩部分进程边界，同 Looper也可能走直调快路；本章所说的异步完成点针对普通独立 App与IME进程，不能把“通常跨进程”误写成接口在任何部署下都必然排队。

## 3. query 怎样用 callback伪装同步，以及 2 秒后发生什么

以 `getTextBeforeCursor()`为例，IME侧 wrapper先检查绑定级 `CancellationGroup`，创建 `Completable.CharSequence`，把 callback交给 `IInputContext`，然后最多等待 2000 ms：

```text
调用线程创建 Completable
  → oneway query进入 App
  → App目标 Looper读取真实连接
  → callback.onResult(result)
  → IME Binder线程填值并 countDown
  → 原调用线程读取值
```

等待可由四件事结束：值到达、`cancelAll()`、线程中断或超时。wrapper在 `await()`返回后才检查 `hasValue()`；所以值即使刚好在超时之后、这个检查之前到达，仍可能被采用。只有检查时已有值才返回实际值，其余回落到 null/0/false。真实 API本来就可能合法返回 null，因此调用者仅凭 null无法区分“无选区”“inactive”“远端异常”“取消”和“超时”。

`CancellationGroup`在 r48 是**客户端 bind级**而非每个 editor start级：`IInputMethodWrapper.bindInput()`创建它，随后同一客户端的多个 `startInput()`共享它，`unbindInput()`在 Binder线程先 `cancelAll()`再把 unbind消息投到IME主线程。切换到同一 App里的另一个编辑器不一定触发这组取消；旧编辑器还要依靠 App wrapper的 active/finished门收口。

取消也不撤销已经发出的 Binder请求。callback可以在等待者返回默认值以后到达，`hasValue()`甚至可由 false变 true；只是这个私有 `Completable`已经没有同步调用者继续消费它。它更不是对 App Handler队列的删除操作。

r48还有一个只影响日志归因的反向命名。注册成功后，`value.await()`正常完成、等待中的取消唤醒或中断时返回真，真正超时返回假；若取消已经发生、`registerLatch()`失败，也返回假。代码却把这个结果命名为 `timedOut`并传给 `logInternal()`。因此在“无值”路径里，真正超时通常被打印成 canceled，等待中取消或中断反而打印成 2000 ms未响应；只有抢在注册前发生的取消可能碰巧得到正确文案。默认值行为不因此改变，排障时却不能把这条文案当成可靠分类。

## 4. App Stub怎样切 Looper，并在执行前挡住旧命令

IMM创建 `ControlledInputConnectionWrapper`时，优先采用真实 `InputConnection.getHandler()`的 Looper；方法缺失或返回 null时使用 served View的 Looper。`IInputConnectionWrapper`为它创建 Handler，Binder入口只封装消息：

```text
当前线程 == 目标 Looper → executeMessage()直接执行并 recycle
否则                      → mH.sendMessage()排队，返回值被忽略
```

因此 oneway调用可能已经对 IME返回 `true`，而 App消息还在队列里；若 Looper不再接收消息，发送结果也没有回传给 IME。多线程同时调用远端 `InputConnection`时，业务层也不应凭返回先后推断所有命令已有全局完成顺序。

执行时，大多数 case重新取得真实连接并检查 `isActive()`。r48 的受控实现只看客户端级 `IMM.mActive`与自身 `!isFinished()`，不比较 served View、bind sequence或某个 edit token；所以这是必要的晚门，却不是给每笔命令附加代际号。队列中排在 close之前的旧消息仍可能先执行。

两个清理 case故意不同：

- `finishComposingText()`绕过 active检查，允许 IME换客户端后清旧组合态；但 wrapper已经 finished时直接忽略。
- `closeConnection()`也绕过 active，若真实实现支持则调用其 close；无论底层是否支持或是否抛出，`finally`都会把内部连接置 null并令 `mFinished=true`。

但 close不是 IME发来的 `IInputContext`命令：r48 AIDL根本没有 `closeConnection()`，IME侧远端 wrapper的同名方法也是 no-op。真正入口是 App IMM在连接切换时本地调用 `ControlledInputConnectionWrapper.deactivate()`，再把 `DO_CLOSE_CONNECTION`送到该连接的目标 Looper。

查询在 inactive时仍尝试 callback null/0，通常能让 IME等待提前结束；纯写命令只记录日志并丢弃，没有“未执行”的反向回执。`commitText()`的 `true`于是连 App是否通过 active门都证明不了。

## 5. TextView怎样导出快照、真实连接和能力表

`TextView.onCreateInputConnection()`分两层判断：`onCheckIsTextEditor()`本身只认 `mEditor != null && inputType != TYPE_NULL`；onCreate再要求 View enabled，并在内部文本还是 `Editable`时才创建 `EditableInputConnection`。TextView填 `inputType`、`imeOptions`、private options、action、extras、hint locales/text、目标用户和焦点导航策略，再补初始选区、caps mode与 initial surrounding text；`packageName`、autofillId和 fieldId则是 IMM在调用 View之前填写。

这次调用一次交付两个不同对象：

- `EditorInfo`是 start时的描述快照，不会随每次文字改变自动更新。
- `InputConnection`是后续读写协议，`EditableInputConnection.getEditable()`每次指向 `TextView.getEditableText()`。

多行 input type会强制加入 `IME_FLAG_NO_ENTER_ACTION`，意图是保留换行键；普通 action则还会结合上下焦点和显式配置决定。`performEditorAction()`不应被简化为“发送 Enter”：`EditableInputConnection`直接调用 `TextView.onEditorAction()`，而 `BaseInputConnection`的默认实现才合成带 `FLAG_EDITOR_ACTION`的 Enter down/up。

只有 `mInputContentType`非 null时，TextView才先把 action交给 listener：listener返回 true就停止，返回 false再由内建逻辑处理 NEXT、PREVIOUS和 DONE。`mInputContentType`为 null，或 action没有被上述分支处理时，才落到 Enter兼容回退。因此 SEARCH/SEND若没有真正消费它的业务 listener，并不天然拥有搜索或发送实现。`TextView.onEditorAction()`本身是 void；`EditableInputConnection`调用后固定返回 true，所以这个 true无法区分 listener消费、内建处理和 Enter回退。

initial surrounding text也不是无界全文：`EditorInfo`会拒绝无效初始 selection，对密码 variation完全清空，并把过长内容裁到最多 2048个 UTF-16 code unit；selection本身超过1024时不携带该段，剩余额度大致按前后4:1分配，边界还避免切开 surrogate pair。它是隐私受限的启动快照，不代表活动连接以后不能查询上下文。

IMM通过 `InputConnectionInspector`计算兼容位图。它会反射旧自定义实现是否缺少 selected text、composing region、correction、cursor updates、code-point delete、handler、close和 content。IME侧 wrapper只对 selected text、composing region、code-point delete、cursor updates和 content正确短路；handler位在 App建链时消费，close位在 App本地关闭时消费，correction位在 r48还存在第13节所述错位。若对象继承 `BaseInputConnection`，Inspector直接返回 0，因为这些方法签名都已存在；这不代表每个默认实现有业务能力，例如 Base的 `commitContent()`仍返回 `false`。

标准 r48 TextView也不自动填写 `EditorInfo.contentMimeTypes`，其 `EditableInputConnection`没有覆盖 Base的 `commitContent()`。富内容需要编辑器或兼容库明确声明 MIME并实现接受逻辑，不能从“这是 TextView”推断天然支持。

## 6. composing text 是可替换状态，不只是下划线

`BaseInputConnection`用静态 `COMPOSING` marker定位主组合区，同时把相关样式 span标成 `SPAN_COMPOSING | SPAN_EXCLUSIVE_EXCLUSIVE`。显示样式只是结果；协议核心是下一次编辑能找到“仍由IME拥有的可整体替换范围”。

`setComposingText(text, pos)`调用 `replaceText(..., composing=true)`：

1. 有旧组合区时以它为替换区，并先移除旧 composing spans；否则使用 selection，负选区回落到 0并规范端点顺序。
2. 非 `Spannable`输入被包成 `SpannableStringBuilder`并加入默认组合样式；已有 spans则被统一带上 composing标志。
3. 新文本再带框架 `COMPOSING` marker，供下一次整体替换。

`commitText(text, pos)`选择相同的旧组合区或 selection，但不会为新文本添加框架 composing状态；随后 full-editor路径保留插入文字。`finishComposingText()`只移除 marker以及所有带 `SPAN_COMPOSING`的 span，不删除字符，也不应主动移动 selection。

`setComposingRegion(start, end)`处理已有文字：先清旧组合 spans，再排序端点并裁到 `0..length`；非空范围加入默认样式和 marker，零长度范围在常用 Spannable实现中不会留下 `EXCLUSIVE_EXCLUSIVE` marker，效果等同结束组合。它不改文字，也不改 selection。selection与 composing range可以交叠却是两本账；`setSelection()`也不会自动结束组合。

这里还有契约与 r48 TextView实现的分叉：接口注释说 `setComposingRegion()`因不改内容而不应触发 `updateSelection()`；但 Base实现仍包在 begin/end batch中，TextView最外层 `finishBatchEdit()`无条件尝试 `sendUpdateSelection()`，而 candidates范围已经变化，IMM去重后实际可能向IME回报新组合区。读行为应以这条具体实现链为准。

有一个必须限定的 Base dummy模式：`fullEditor=false`时 Base维护临时 Editable；`setComposingText()`只缓冲，commit、finish或 `setComposingRegion()`会尝试把恰好一个 UTF-16 code unit变成普通 KeyEvent序列，其他文本变成带 characters的特殊 KeyEvent，然后清缓冲。标准 `EditableInputConnection`用 `fullEditor=true`，不走这条兼容文字主路。

## 7. `newCursorPosition`为何先放 selection、后 replace

这个参数是 Java UTF-16下标语义下的相对位置，不是文档绝对下标：

```text
newCursorPosition > 0  → 相对替换区末端 - 1
newCursorPosition <= 0 → 相对替换区起点
```

所以 `1`表示插入完成后位于整段新文本之后，`0`表示新文本之前。接口不允许稳定指定新文本内部某个位置，因为 `InputFilter`可能改写长度或内容。

Base实现的顺序看似反直觉：它先用旧内容的 `[a,b)`算出相对锚点并裁到旧 `content.length()`，先设置 Selection span，再执行 `content.replace(a,b,text)`。Spannable在替换时移动这个 span，从而让最终位置跟随真实过滤结果。不要拿“先设置时的整数”当作替换后的绝对位置。

例如旧替换区是 `[5,7)`，换入三个 UTF-16 code unit：

```text
pos = 1   → 锚在旧末端 7，替换后通常随新文本移动到 8
pos = 0   → 锚在旧起点 5，通常留在新文本之前
pos = -1  → 尝试在起点前一位，约 4
```

最终结果仍受 filter和 span边界规则影响。特别是 `[a,b)`为零宽纯插入时，pos为1与0在 r48 Base里都可能先把 POINT selection放到同一旧下标；具体 `Spannable`的插入边界亲和性可让 pos为0也跟随新文本，不能仅靠公式臆测最终坐标。越界 `setSelection(start,end)`在 Base实现里会不改内容却返回 `true`；若 META_SELECTING处于活动态且 start=end，它还会 extend而非简单重置，其他情况保留调用者给出的 start/end方向并不排序。替换和删除会自行排序反向 selection，不能把那条规则套到 setSelection。这再次说明 boolean不是状态回读。

## 8. surrounding delete 的三种“字符”与组合区保护

`deleteSurroundingText(before, after)`按 UTF-16 code unit计数；它以 selection两端为基准，若 composing范围伸到 selection外，就把基准扩成两者的最小包络区间。即使 selection与 composing彼此分离，中间的普通文字也被包进受保护间隙，before/after删除只落在整个包络之外；这个 API本身不是“删除当前 selection”。

实现先删前方，再用 `deleted`修正后方起点，因为第一次删除已令原下标左移。长度大于现有内容时裁到边界；负长度不会反向删除。

`deleteSurroundingTextInCodePoints()`改按 Unicode code point走 `findIndexBackward/Forward()`：

- 合法 surrogate pair作为一个 code point。
- 遍历遇到孤立、反序或半截 surrogate时返回 `INVALID_INDEX`，本轮两侧都不删。
- before/after负数先经 `Math.max(...,0)`变成 0。
- 即使无效编码导致没有修改，Base方法最终仍返回 `true`；跨进程 wrapper更早已经返回 `true`。

两版 delete在无有效 selection时还有一处本地差异：`Editable`为 null时都返回 false；若 Editable存在但 selection start/end为 -1，code-unit版在配对结束 batch后返回 false，code-point版跳过删除却最终返回 true。IME侧 oneway wrapper的提前 true仍会遮住这一区别。

它只保证不把 UTF-16代理对当两个 code point，不保证用户感知字素。肤色修饰、ZWJ家庭 emoji、国旗和组合音标都可能由多个 code point组成；删除一个完整 grapheme cluster仍需编辑器/IME更高层的分段策略。

普通文字应优先走 composing/commit，而非模拟 KeyEvent。`sendKeyEvent()`主要服务 `TYPE_NULL`或硬件键盘兼容语义；候选替换、组合 spans和相对光标无法用一串按键完整表达。

## 9. batch edit 聚合哪些副作用，又不承诺什么

IME可用嵌套 begin/end把“清旧组合、插候选、移动选区”等多步标成一个编辑批次。对 TextView而言，这会让 Editor累计 content/selection/cursor变化，并把部分 `updateAfterEdit()`、extracted-text报告、selection报告、cursor invalidate和手柄恢复推迟到最外层结束；它不是数据库事务，也不回滚失败步骤，更不能保证所有 `TextWatcher`或业务回调都沉默。

`EditableInputConnection`维护自己对 TextView嵌套账的贡献：

```text
mBatchEditNesting >= 0 → begin可进入 TextView并加一
mBatchEditNesting > 0  → end可进入 TextView并减一
closeConnection        → 补齐所有仍为正的 end，再置 -1
```

置为 -1后，迟到的 begin/end都返回 false，不再改变 TextView的批次账。close先调用 `BaseInputConnection.closeConnection()`清 composing，再平衡遗留 nesting；Java内置锁可重入，循环里的 `endBatchEdit()`不会因此死锁。

r48存在一处本地返回语义偏差：接口约定 `endBatchEdit()`应仅在结束后仍有嵌套时返回 true；`EditableInputConnection`却只要调用前 nesting大于 0就返回 true，包括 1降到0。跨进程 IME侧 wrapper又在 oneway发出后固定返回 true。因此任何一层的这个 boolean都不适合作为远端精确嵌套计数。

漏掉 end会让部分状态报告长期被抑制；连接正常 close可补齐本连接贡献，但这不是让IME忽略配对责任的理由。若 close消息还排在队列后面，先入队的普通命令仍可能在补账前运行。

## 10. selection 与 ExtractedText 怎样反向直达 IME

Editor的 selection span watcher和最外层 batch收尾会调用 `sendUpdateSelection()`。它读取 selection start/end，再从 `COMPOSING` marker取得 candidates start/end，交给 App IMM。

IMM会先 `checkFocus()`并确认 View仍被当前输入法服务、EditorInfo和 Session存在，然后比较四个缓存值。发生变化时，它在调用 Session前就更新缓存，再发：

```text
oldSelStart / oldSelEnd
newSelStart / newSelEnd
candidatesStart / candidatesEnd
```

提前更新可避免 IME在 `onUpdateSelection()`中反向改字但未移动光标时形成相同值回环；代价是 Binder发送若抛异常，缓存不会回滚，同值也不会因这次失败自动重发。`IInputMethodSession`是 oneway，IME wrapper还要排入自己的 Handler，所以回调可以落后于更多编辑命令，且没有与某笔 `commitText()`绑定的 command id。

`ExtractedText`是另一份状态。全屏编辑器可在 `getExtractedText(..., GET_EXTRACTED_TEXT_MONITOR)`时登记 request；Editor随后累计 changed start/end/delta，并在适当的最外层 batch收尾或 draw检查里构造部分/全量快照，经 `IMM.updateExtractedText()`直达同一 Session。request token用于匹配这类提取请求，不是全局编辑代际。

反向 Session到达IME后还要通过该 Session的 enabled门。端点虽然由 App直连使用，生命周期仍由 IMMS建立和结束；`finishSession()`到 IME侧实际处理时才清内部 Session并 dispose InputEventReceiver/InputChannel，而且同样没有完成 ACK。旧 Session被禁用或 finish后，消息可被丢弃；App侧成功发出仍不等于 `InputMethodService.onUpdate...()`已执行。

## 11. CursorAnchorInfo 从请求模式走到几何快照

`requestCursorUpdates(mode)`也采用 int callback并最多等2秒。Base默认直接返回 false；`EditableInputConnection`还要求 `mIMM`非 null，只认识 `CURSOR_UPDATE_IMMEDIATE`与 `CURSOR_UPDATE_MONITOR`，任一未知位会整单返回 false且不覆盖旧模式。其完整模式是：

这张表还有一个到达 App前的前置门：若客户端与IME落在不同 display，且 IMMS取不到 ActivityView到IME screen的变换矩阵，它会强行把 `REQUEST_CURSOR_UPDATES`加入 missing-method位图。IME侧 wrapper于是直接返回 false，根本不会调用下面的 `EditableInputConnection`实现。

| mode | r48 `EditableInputConnection`行为 |
| --- | --- |
| `0` | 合法；记录0并关闭持续监听 |
| `IMMEDIATE` | 记录模式，并在可行时 `requestLayout()`促成一次回报 |
| `MONITOR` | 记录持续订阅，本次调用不强制 layout |
| `IMMEDIATE | MONITOR` | 尽快回报一次并继续监听 |
| 含未知位或 `mIMM == null` | 返回 false，不完成模式更新 |

两种有效位的时序含义是：

- `IMMEDIATE`要求尽快给一次；TextView不在 layout中时调用 `requestLayout()`，正在 layout中则依赖本轮结束后的既有位置通知。
- `MONITOR`保留持续订阅，后续位置、滚动或 composing变化可继续产生快照。
- 返回 true只说明模式被接受，不说明 layout已发生、几何可用或 Binder回报已完成。

`Editor.CursorAnchorInfoNotifier`还要通过 batch nesting为0、IMM active、模式已启用和 Layout非 null等门，才构建：

```text
selection范围
TextView local matrix + getLocationOnScreen平移
composing文本与每字符 bounds
插入标记 top / baseline / bottom
可见、不可见与 RTL flags
```

插入标记 top/bottom分别检查，所以局部被裁时 `HAS_VISIBLE_REGION`与 `HAS_INVISIBLE_REGION`可以同时为真。跨显示所需的 ActivityView parent matrix可得时，IMM在发送前再变换；它缓存的仍是原始 `CursorAnchorInfo`。

IMM在 IMMEDIATE置位时即使内容相同也发送；`mCurMethod.updateCursorAnchorInfo()`没有抛 `RemoteException`后才缓存对象并清 IMMEDIATE。这个点只是 oneway事务已发出，MONITOR仍保留，更不等于候选窗已经重新布局或显示。

## 12. `commitContent()`怎样把接受结果与 URI权限拆开

富内容对象包含 `content://` URI、`ClipDescription`和可选 http/https link URI。公开构造器会先验证这些字段，跨进程到达后 App侧 `IInputConnectionWrapper`还会再次调用 `validate()`；IME侧远端 wrapper只检查取消与 missing bit、可选 expose后便转发，并不再次验证对象。协议要求编辑器用 `EditorInfo.contentMimeTypes`声明能力并自行核对 MIME，这几层结构校验都不替业务判断 MIME匹配。

IME设置 `INPUT_CONTENT_GRANT_READ_URI_PERMISSION`时，远端 wrapper先调用 `InputMethodService.exposeContent()`：只有传入的 wrapper仍严格等于当前 `InputConnection`，IMS才拿当前 EditorInfo包名向 IMMS请求 token。这个检查失败是静默 return；之后的 `commitContent()`仍可能继续，只是对象没有可 take的 token。

IMMS创建 token时核对：参数非 null、URI scheme为 content、调用 token仍是当前 IME WindowToken、包名等于当前 `mCurAttribute.packageName`。它还把 Binder calling UID固定为源IME UID，并记录源/目标 user。**此时尚未真正授予，也没有预先证明IME能转授该URI。**

App接受对象后调用 `InputContentInfo.requestPermission()`，才进入 `InputContentUriTokenHandler.take()`：创建 permission owner，并以记录的源IME UID调用 URI grants服务授予临时只读权限；IME若无权转授会在这里触发安全失败。token一旦构造并交给 App，就不再要求调用 take时仍是当前输入会话；App只要继续持有该 `InputContentInfo`，即使随后切换IME也可再请求权限。成功后的重复 take和重复 release分别 no-op，release撤销 owner权限；但 take在真正 grant前已保存 owner token，grant若抛安全异常不会自动复位，直接重试只会 no-op，至少要先 release清状态。finalizer只是非确定性兜底，不能代替显式释放。

最后，`IInputContext.commitContent()`本身带 int callback。IME最多等2秒得到“编辑器接受/拒绝”；即使返回 true，App仍可后台加载，读取也可能因没 take、token缺失、授权失败或过早 release而失败，图片显示更在后面。

## 13. 能力降级与 r48 两个诊断陷阱

missing-method位图解决“旧类根本没有后来新增方法”，不等于特性协商的最终答案：

- `BaseInputConnection`被视为签名齐全，即使某些默认实现只返回 false。
- `getHandler`位只决定 App Stub采用真实连接 Looper还是 View Looper；IME侧远端 wrapper自己的 `getHandler()`固定返回 null。
- cursor updates与 commit content即便方法存在，仍可因 mode、MIME或编辑器策略返回 false。
- `closeConnection`位缺失时 App Stub跳过底层 close，但仍在 finally清 wrapper引用并标 finished。

r48的 IME侧 `InputConnectionWrapper.commitCompletion()`还误查了 `MissingMethodFlags.COMMIT_CORRECTION`。Inspector本就没有 `COMMIT_COMPLETION`位，因为 completion是早期接口；结果一方面是被判缺少 correction的旧自定义连接即使能处理 completion，也被远端 wrapper提前返回 false；另一方面，紧邻的 `commitCorrection()`反而没有检查该缺失位，命令仍会进入 App并可能在旧实现处触发 `AbstractMethodError`。这是一组本版实现错位，不是 API通则。

另一个陷阱是第3节的 `timedOut`日志反向。二者都提醒我们：诊断必须同时看“能力位、实际分支、callback/默认值”，不能只信方法名或一条日志文案。

补充两类命令也要守住完成点：`performPrivateCommand()`的 action应带拥有者包名前缀，远端 true不证明 App识别；`TextView.onEditorAction()`把 listener的消费结果留在内部且自身不返回值，`EditableInputConnection.performEditorAction()`调用后固定返回 true，跨进程 wrapper又更早返回。因此“动作调用为真”不是业务已经消费。

## 14. 用故障现象反推第一处分歧

| 现象 | 第一组检查 | 不应直接得出的结论 |
| --- | --- | --- |
| `commitText()`为真但没字 | IInputContext Binder、App Handler、active/finished、真实连接、selection/filter | Editable已经改过又被UI吞掉 |
| query为 null/0 | missing bit、CancellationGroup、远端异常、App active、2秒等待、真实返回 | App一定卡满2秒 |
| composing替换范围错 | COMPOSING marker、传入 spans、selection、旧连接晚消息 | 下划线样式本身就是协议 |
| emoji删半个 | 调的是 code-unit还是 code-point版本、代理对是否合法、字素边界 | code-point API保证完整字素 |
| batch后无状态更新 | begin/end配对、连接 close、Editor nesting、Session enabled | batch提供事务回滚 |
| selection回调落后 | App缓存、oneway Session、IME Handler、后续命令竞态 | 回调必对应最近一笔 commit |
| cursor请求为真但无几何 | layout、active、monitor mode、CursorAnchorInfo门、旧Session | request返回就是一次位置回报 |
| content接受但打不开 | MIME、current connection、URI token、take/release、源UID授权 | callback true包含加载与展示 |

一条连接没有给每笔 edit附 sequence。安全来自多层、不同粒度的门：IME bind级取消等待，App wrapper的 active/finished，IMM的 served View与当前 Session，IME Session的 enabled，以及 URI token的当前IME/包名/源UID约束。它们减少旧状态伤害，却不能把所有异步消息变成一个全局线性事务。

## 15. 九组 macOS 只读源码练习

以下脚本只读源码。每段都可在 macOS自带 Bash 3.2或 Zsh 5.9运行；也可把另一个源码根作为第一个参数传入。

### 练习 1：把命令、查询和反向状态分成三类

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oneway interface IInputContext {' frameworks/base/core/java/com/android/internal/view/IInputContext.aidl
grep -n -F 'void getTextBeforeCursor(int length, int flags, ICharSequenceResultCallback callback);' frameworks/base/core/java/com/android/internal/view/IInputContext.aidl
grep -n -F 'void commitText(CharSequence text, int newCursorPosition);' frameworks/base/core/java/com/android/internal/view/IInputContext.aidl
grep -n -F 'oneway interface IInputMethodSession {' frameworks/base/core/java/com/android/internal/view/IInputMethodSession.aidl
grep -n -F 'void updateSelection(int oldSelStart, int oldSelEnd,' frameworks/base/core/java/com/android/internal/view/IInputMethodSession.aidl
```

说明哪些方法只有发送结果、哪些另带 callback、为什么反向状态不经 IMMS逐字中转。

### 练习 2：验证 2 秒等待与绑定级取消

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'private static final int MAX_WAIT_TIME_MILLIS = 2000;' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'final boolean timedOut = value.await(MAX_WAIT_TIME_MILLIS' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'if (value.hasValue()) {' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'return mLatch.await(timeout, timeUnit);' frameworks/base/core/java/com/android/internal/inputmethod/CancellationGroup.java
grep -n -F 'mCancellationGroup = new CancellationGroup();' frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java
grep -n -F 'mCancellationGroup.cancelAll();' frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java
```

分别推演“正常值、unbind取消、真正超时”，并核对 `timedOut`日志为什么可能反向。

### 练习 3：找到 App Looper 与旧命令晚门

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (Looper.myLooper() == mMainLooper) {' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'mH.sendMessage(msg);' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'case DO_COMMIT_TEXT: {' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'if (ic == null || !isActive()) {' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'case DO_FINISH_COMPOSING_TEXT: {' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'mInputConnection = null;' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'mFinished = true;' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'public void closeConnection() {' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'void deactivate() {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

解释为何 write没有失败回调，而 inactive query仍能尽量回默认值；再标出哪两类清理绕过 active。

### 练习 4：核对 TextView建连和兼容表

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public InputConnection onCreateInputConnection(EditorInfo outAttrs) {' frameworks/base/core/java/android/widget/TextView.java
grep -n -F 'InputConnection ic = new EditableInputConnection(this);' frameworks/base/core/java/android/widget/TextView.java
grep -n -F 'outAttrs.setInitialSurroundingText(mText);' frameworks/base/core/java/android/widget/TextView.java
grep -n -F 'missingMethodFlags = InputConnectionInspector.getMissingMethodFlags(ic);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'icHandler = ic.getHandler();' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'if (ic instanceof BaseInputConnection) {' frameworks/base/core/java/android/view/inputmethod/InputConnectionInspector.java
grep -n -F 'public boolean commitContent(InputContentInfo inputContentInfo, int flags, Bundle opts) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'MEMORY_EFFICIENT_TEXT_LENGTH = 2048' frameworks/base/core/java/android/view/inputmethod/EditorInfo.java
grep -n -F "For privacy protection reason, we don't carry password inputs to IMEs." frameworks/base/core/java/android/view/inputmethod/EditorInfo.java
grep -n -F 'isMethodMissing(MissingMethodFlags.COMMIT_CORRECTION)' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
```

把 EditorInfo快照、真实协议、Looper选择和“签名存在但默认 false”分成四列。

### 练习 5：手推 composing与相对光标

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public static void setComposingSpans(Spannable text, int start, int end) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'private void replaceText(CharSequence text, int newCursorPosition,' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'newCursorPosition += b - 1;' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'Selection.setSelection(content, newCursorPosition);' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'content.replace(a, b, text);' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'removeComposingSpans(content);' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'public boolean setComposingRegion(int start, int end) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'sendUpdateSelection();' frameworks/base/core/java/android/widget/Editor.java
```

用 `ab[XY]cd`把 `XY`视为组合区，依次计算 composing `zhong`、commit `中`以及 pos为1/0时的 marker和光标。

### 练习 6：比较 code unit、code point与字素

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public boolean deleteSurroundingText(int beforeLength, int afterLength) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'private static int findIndexBackward(final CharSequence cs, final int from,' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'private static int findIndexForward(final CharSequence cs, final int from,' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'Math.max(beforeLength, 0)' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'if (start != INVALID_INDEX) {' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
```

用 `A😀B`和一个 ZWJ家庭 emoji解释两种 API各保证到哪层，并观察非法 surrogate时为何仍可能返回 true。

### 练习 7：闭合 batch与 selection反馈

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mBatchEditNesting++;' frameworks/base/core/java/com/android/internal/widget/EditableInputConnection.java
grep -n -F 'while (mBatchEditNesting' frameworks/base/core/java/com/android/internal/widget/EditableInputConnection.java
grep -n -F 'void finishBatchEdit(final InputMethodState ims) {' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'private void sendUpdateSelection() {' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'imm.updateSelection(mTextView,' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'mCurMethod.updateSelection(oldSelStart, oldSelEnd,' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

说明嵌套从2降到0时哪些动作只在最外层发生、EIC怎样补自己的账，以及 Session返回为何不是IME已处理的证明。

### 练习 8：从 cursor请求走到几何快照

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'final int unknownFlags = cursorUpdateMode & ~KNOWN_FLAGS_MASK;' frameworks/base/core/java/com/android/internal/widget/EditableInputConnection.java
grep -n -F 'mIMM.setUpdateCursorAnchorInfoMode(cursorUpdateMode);' frameworks/base/core/java/com/android/internal/widget/EditableInputConnection.java
grep -n -F 'final CursorAnchorInfo.Builder builder = mSelectionInfoBuilder;' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'builder.setMatrix(mViewToScreenMatrix);' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'builder.setInsertionMarkerLocation(insertionMarkerX, insertionMarkerTop,' frameworks/base/core/java/android/widget/Editor.java
grep -n -F 'mCurMethod.updateCursorAnchorInfo(' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F '~InputConnection.CURSOR_UPDATE_IMMEDIATE;' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

列出“接受模式、请求 layout、构造 info、发 oneway、IME处理、候选窗重排”六个完成点。

### 练习 9：验证 rich content权限的真正授予点

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'inputMethodService.exposeContent(inputContentInfo, this);' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'if (getCurrentInputConnection() != inputConnection) {' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'if (mCurToken != token) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (!TextUtils.equals(mCurAttribute.packageName, packageName)) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'mPermissionOwnerToken = LocalServices.getService(UriGrantsManagerInternal.class)' frameworks/base/services/core/java/com/android/server/inputmethod/InputContentUriTokenHandler.java
grep -n -F 'grantUriPermissionFromOwner(' frameworks/base/services/core/java/com/android/server/inputmethod/InputContentUriTokenHandler.java
grep -n -F 'mIInputContext.commitContent(inputContentInfo, flags, opts, ResultCallbacks.of(value));' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'client is able to request a temporary read-only access even after the current IME is switched' frameworks/base/core/java/android/view/inputmethod/InputConnection.java
```

把“token构造、App take、grant校验、编辑器callback、后台加载”分别标时刻，并说明哪个阶段才验证源IME的转授权资格。

## 16. 用六条时间线收束协议边界

```text
写命令线：
IME wrapper发起oneway → Binder接纳/调用返回 → IME wrapper返回
                                  ↘ App Stub → 目标 Looper → active门 → Editable变化

查询线：
oneway请求 → App读取 → oneway callback → Completable有值
             ↘ cancel / interrupt / 2秒 → 默认值

组合线：
selection或旧COMPOSING → 移除旧标记 → 相对光标锚点
→ replace/filter → 新composing或最终文字 → 反向坐标

批次线：
嵌套begin → 累计局部变化 → 最外层end → 部分UI更新/extracted/selection
→ App Session发送 → IME Handler处理

光标几何线：
callback接受mode → layout/position门 → CursorAnchorInfo → Session oneway
→ IME回调 → 候选UI后续变化

富内容线：
声明MIME → 可选token构造 → App进入commitContent
                         ├→ 同步take/grant → 方法返回 → callback
                         └→ 方法先返回 → callback → 异步take/grant
两支随后都可能继续后台读取/解码/显示，最终显式release
```

真正掌握 `InputConnection`，不是背完几十个方法，而是随时回答五个问题：**这是只有发送结果的命令，还是 callback + 本地等待；真实执行属于哪个 Looper；范围按 UTF-16、code point还是字素；旧消息靠哪个粒度的门失效；眼前的 true/null/回调只证明到了哪一层。**
