# 232 Android IMMS、InputMethodService 与软键盘显示输入隐藏完整链

本文基于 `android-11.0.0_r48`。我们从用户点进一个 `EditText` 开始，依次追踪 App 侧焦点与 `InputConnection`、system_server 中的 `InputMethodManagerService`（IMMS）、输入法进程里的 `InputMethodService`（IMS）、IME 独立窗口，以及 Android 11 新 Insets 模式下的 show/hide 动画；最后再沿反方向看一个字符怎样写回 App。

本章最重要的不变量是：**View 获得焦点、它成为 served View、App 创建编辑协议、IMMS 接纳 start、IME Service 已绑定、Session 已创建、IME 收到 start、IMMS 安排 show、IMS 接受 show、IME 窗口完成布局、Insets 动画结束、Surface 事务提交、像素物理呈现，是彼此不同的完成点。**

版本锚点：

- `frameworks/base/core/java/android/view/ImeFocusController.java`
- `frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java`
- `frameworks/base/core/java/android/view/inputmethod/EditorInfo.java`
- `frameworks/base/core/java/com/android/internal/view/InputBindResult.java`
- `frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java`
- `frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java`
- `frameworks/base/core/java/com/android/internal/view/IInputMethod.aidl`
- `frameworks/base/core/java/com/android/internal/view/IInputContext.aidl`
- `frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java`
- `frameworks/base/core/java/android/inputmethodservice/InputMethodService.java`
- `frameworks/base/core/java/android/inputmethodservice/SoftInputWindow.java`
- `frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java`
- `frameworks/base/services/core/java/com/android/server/wm/ImeInsetsSourceProvider.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`
- `frameworks/base/core/java/android/view/ImeInsetsSourceConsumer.java`

## 1. 先把一次输入的完成点拆开

“点输入框、键盘出现、输入一个字、再隐藏”不是一个调用栈，而是两条相反方向的数据链加一条窗口链：

```text
建立与显示：
View/Window 焦点
  → IMM 在 View Looper 创建 EditorInfo 与 InputConnection
  → 同步 Binder 进入 IMMS 做用户、客户端、display、WMS 焦点和包名校验
  → 选择并异步绑定当前 IME，创建 WindowToken 与 Session
  → oneway IInputMethod.bindInput / startInput / showSoftInput
  → IMS 主线程准备输入 UI 和 TYPE_INPUT_METHOD 窗口
  → IME 请求 WMS 在布局后 show Insets
  → 控制目标执行 IME Insets 动画 → Surface 后续呈现

编辑回写：
IME 按键逻辑
  → 远端 InputConnection.commitText
  → oneway IInputContext
  → App 指定 Looper 上的 IInputConnectionWrapper
  → EditableInputConnection / BaseInputConnection
  → Editable、TextWatcher、布局、绘制

隐藏与回收：
App/系统 hide 请求
  → IMMS 策略记账并异步通知 IMS
  → IMS 清本地 UI 意图并请求 WMS hide Insets
  → 控制目标完成隐藏动画
  → 通知 IME 已隐藏并在独立路径移除残留 Surface
```

诊断时先问“卡在哪个完成点”，不要只问“键盘为什么没弹”：

| 观察 | 最多能证明 | 仍不能证明 |
| --- | --- | --- |
| `startInputInner()` 返回 `true` | 本轮没有在若干本地 abort/重投分支提前返回，且一次 IMMS 调用已返回或抛出的远端异常已被捕获 | IMMS 返回成功码、已拿到 Session、IME 已开始输入 |
| `SUCCESS_WAITING_IME_BINDING` | IMMS 已发起 Service 连接 | 已取得 `IInputMethod` |
| `SUCCESS_WITH_IME_SESSION` | 返回对象里的 Session、channel、sequence 可用 | IME 输入 View 或窗口可见 |
| `showSoftInput()` 返回 `true` | r48 已走到“有当前 IME 接口并安排 show”的分支 | oneway 调用成功交付、IME 接受、窗口绘制 |
| `ResultReceiver.RESULT_SHOWN` | IMS 的本地可见状态由 hidden 变为 shown | WMS post-layout、动画或物理 present 完成 |
| Insets 动画 `finish()` | 控制器逻辑到达终帧 | SurfaceFlinger 已在屏幕显示该帧 |

反向也要小心：r48 的 `showCurrentInputLocked()`先写 `mShowRequested`，再判断部分拒绝条件；Service 正在绑定时，本次调用可以返回 `false`，后来 `attachNewInputLocked()`仍可能重放保存的显示意图。`false` 并不总是“未来绝不会显示”。

## 2. 三个进程、六组 Binder 接口和同名字段

通常有三个安全域：目标 App、system_server、用户选中的 IME App。IME 窗口属于输入法进程，不是目标 Activity View 树的子 View。

| 角色 | 关键对象 | 责任 |
| --- | --- | --- |
| 目标 App | `ImeFocusController`、`InputMethodManager`、真实 `InputConnection` | 选择 served View，导出编辑协议，保存当前 Session |
| system_server | IMMS、WMS、`ImeInsetsSourceProvider` | 仲裁客户端和用户，绑定 IME，管理 token/Session，选择 Insets 控制目标 |
| IME App | `IInputMethodWrapper`、IMS、`SoftInputWindow`、远端 `InputConnectionWrapper` | 处理输入生命周期，绘制键盘，通过协议读写编辑器 |

接口方向与同步语义不能混在一起：

| 接口 | 方向 | r48 AIDL 语义 |
| --- | --- | --- |
| `IInputMethodManager` | App → IMMS | start/show/hide 是同步 Binder 方法，返回的是 system_server 当下结果 |
| `IInputMethod` | IMMS → IME | 整个接口是 `oneway`；跨进程调用返回不等于 IME 已处理 |
| `IInputMethodClient` | IMMS → App | `oneway`；App Stub 再投递到 IMM 的 Handler |
| `IInputContext` | IME → App | `oneway`；编辑命令不带执行结果，查询另带 callback |
| `IInputMethodSession` | App/IMMS → IME Session | `oneway`；传递选择、光标、事件状态和收尾通知 |
| `IInputMethodPrivilegedOperations` | IME → IMMS | 受当前 IME token 约束的同步接口 |

同名字段尤其危险：

- App IMM 的 `mCurMethod` 是 `IInputMethodSession`；IMMS 的 `mCurMethod` 是顶层 `IInputMethod`。
- IMMS 的 `mShowRequested` 是服务端显示意图；IMS 的 `mShowInputRequested` 是输入法 UI 状态。
- IMMS 的 `mInputShown` 表示 show 已被安排给当前 IME，不是屏幕事实；IMS 的 `mIsInputViewShown` 也是本地 View 状态，不是 present fence。
- IMMS 的 `mImeWindowVis` 来自当前 IME 的状态上报，会与前述字段短暂分叉；WMS文档也明确它不保证与 WMS状态同步，r48 的 WMS实现只在该入口消费 back-key disposition，不靠这个布尔值驱动 Insets Source。

IMMS 的 `executeOrSendMessage()`也有一个反直觉分支：目标若是本地 `Binder`，它投递 `mCaller`；目标若是普通跨进程 `BinderProxy`，它在当前 IMMS 调用线程直接执行 `handleMessage()`，而其中的 `IInputMethod` 调用本身是 oneway。真正把收到的命令切到 IME 主线程的是 `IInputMethodWrapper` 的 `HandlerCaller`。所以不能把所有 show/start 都画成“先切 system_server 主线程”。

## 3. 焦点怎样把一个 View 变成 served editor

起点不是 `showSoftInput()`，而是 ViewRoot 对窗口焦点与 View 焦点的协调。

`ImeFocusController.onPreWindowFocus()`在窗口可接受 IME 且不是 local-focus 模式时，把当前 `ViewRootImpl` 交给 IMM。`onPostWindowFocus()`随后：

1. 取真正 focused View；没有时用根 View 作为窗口焦点代表。
2. `onViewFocusChanged(..., true)`只在 View 具有 IME focus 和 Window focus 时更新 `mNextServedView`。
3. 检查当前连接是否仍属于同一个 View，必要时强制新一轮 focus。
4. 调用 IMM delegate，把 WINDOW_GAINED_FOCUS 与可能的 start input 合并报告给 IMMS。

`checkFocus()`才把 next 提升为 served：

```text
mServedView == mNextServedView 且不强制 → 无变化
mNextServedView == null               → finishInput + 请求关闭当前 IME
存在新的 next                          → 更新 served，结束旧 composing，再 startInput
```

失焦事件不会一律把 next 立刻清空，因为触摸模式可能短暂清焦；detach 和 window dismissed 等明确边界另行收尾。这也是 `requestFocus()`之后立即 show 仍可能失败的原因：局部 View 焦点、Window 焦点、ViewRoot 当前身份和 served 状态尚未必收敛。

`InputMethodManager.showSoftInput(view, ...)`会先 `checkFocus()`，然后要求传入 View 就是 served View，或由 served View 声明它是合法的 input-connection proxy。`hideSoftInputFromWindow()`则要求 served View 存在且其 WindowToken 与参数相同。旧 Activity 保存的 token 不能直接代表新窗口。

`canStartInput()`通常要求 served View 有 Window focus，也给 Autofill UI 显示保留例外。它只回答是否适合启动输入，不等于 WMS 已把该窗口选成最终 IME target。

## 4. `startInputInner()`怎样安全创建编辑协议

IMM 先在 `mH` 锁内取得 served View，然后释放锁再调用 App 的 View 代码。原因不是形式上的“线程切换”，而是 `onCreateInputConnection()`可能复杂、重入并改变焦点；持锁调用会制造死锁和陈旧覆盖。

随后有三道门：

- View 没有 WindowToken：尚未 attach，终止本轮。
- `view.getHandler()` 为 `null`：状态已从脚下改变，IMM 调用 `closeCurrentInput()`尝试收起旧键盘。这个名字容易误导：它只向 IMMS发送 `HIDE_NOT_ALWAYS`，不在此处 clear/deactivate连接；若此前是 forced show，服务端还可以拒绝该 hide。
- 当前 Looper 不是 View Handler 的 Looper：把一个“重新读取届时 served View并重新 start”的 Runnable post 到 View 线程，然后本轮返回 `false`；源码不检查 `Handler.post()`的返回值。这里只说明这个分支已改道，其他多个 abort分支也会返回 `false`。

窗口获焦入口还有一层交错：完整 start若返回 `false`，原调用栈会继续用空 EditorInfo/InputContext向 IMMS报告一次 window focus。若这个 `false`来自跨 Looper重投，已经 post 的完整 start与原线程的 focus-only Binder调用会竞争 `mH`，谁先到 IMMS并无保证。因此“同一次获焦一定把窗口报告和完整编辑器原子交付”也不成立。

到达正确线程后，IMM 先填框架可确认的 `EditorInfo` 字段，再让 View 同时补齐描述并返回协议：

```text
EditorInfo：packageName、fieldId、inputType、imeOptions、初始选区、周围文本……
InputConnection：读取光标附近文本、组合、提交、删除、选区、editor action……
```

这里用 `getOpPackageName()`，因为 IMMS 后面要按 UID 核验包名，不只是给键盘显示标签。标准 `TextView` 仅在自己是启用的文本编辑器且文本可编辑时返回 `EditableInputConnection`；`onCreateInputConnection()`返回 `null` 时，窗口仍可报告焦点，IMMS 也可能收到非空 `EditorInfo`，但没有可供 IME 编辑的实际 `IInputContext`。

Android 11 的 `TextView` 会调用 `EditorInfo.setInitialSurroundingText()`；该方法对识别出的文本、Web 和数字密码 variation 清空这份初始周围文本，并把过长文本裁到 2048 个 UTF-16 code unit 左右。这个保护只约束初始快照：当前启用的 IME 仍处在敏感信任边界，活动 `InputConnection`并未因此整体不可查询，`FLAG_SECURE`也主要约束截屏而非 IME 协议。

View 回调返回后，IMM 重新加锁检查“served 仍是原 View”且 `mServedConnecting`仍为真。这不是数字代际检查；失败时只是不发布本轮结果，也不会替刚创建、尚未包装的原始 `InputConnection`调用 `closeConnection()`。成功才：

- 根据旧 `mCurrentTextBoxAttribute` 是否为空设置 `INITIAL_CONNECTION`；它不是“第一次显示键盘”。
- deactivate 旧的 `ControlledInputConnectionWrapper`。
- 优先采用具体 `InputConnection.getHandler()` 的 Looper，否则使用 View Looper。
- 计算 `missingMethods`，把兼容能力表交给远端。
- 用新的 wrapper 作为 `IInputContext`调用 IMMS。

`ControlledInputConnectionWrapper.isActive()`只检查客户端级 IMM active状态和自身是否 finished，不比较 served View、bind sequence或 start token。旧 wrapper在新 View的 `onCreateInputConnection()`执行期间仍可能 active；成功复核后才调用 deactivate，而专用 Handler不同时 deactivate也只是排入 close message。Binder句柄仍存在与真正退休完成不是同一个时刻。

## 5. IMMS 怎样拒绝冒名客户端、错用户和错屏

IMM 创建时已通过 `addClient()`登记 `IInputMethodClient`、一个客户端级 dummy `IInputContext`、UID、PID 和 self-reported display，并给 client Binder 注册 death recipient。后续入口不会只相信一次调用携带的数据。

`startInputOrWindowGainedFocus()`的主要校验顺序是：

1. WindowToken 必须非空；失败返回预定义且非 null 的 `InputBindResult.NULL`，其 result code 是 `ERROR_NULL`、sequence 是 -1。
2. 跨用户 `EditorInfo.targetInputMethodUser`要求 `INTERACT_ACROSS_USERS_FULL`，目标用户还必须正在运行。
3. `client.asBinder()`必须能在 `mClients`找到已登记的 `ClientState`。
4. WMS 从 WindowToken 得到的 display 必须等于客户端登记的 display。
5. `isInputMethodClientFocus(uid, pid, displayId)`必须认可调用进程确有当前 IME 焦点。
6. 调用用户必须是当前 profile；真正的用户切换则返回 waiting 状态。
7. 进入 `startInputUncheckedLocked()`后再核验 `EditorInfo.packageName`属于客户端 UID，以及 UID 仍被允许访问该 display。

因此有两层不同事实：App 内 `ImeFocusController`决定哪个 View 想当编辑器；system_server 让 WMS 判断该 UID/PID/display 是否确实拥有可服务的窗口焦点。后台 App 不能只靠自己的 View 状态抢走 IME。

显示位置也不是机械跟随客户端。`computeImeDisplayIdForTarget()`会把默认/无效 ID 归到 fallback display；某个显示不允许承载系统装饰或不满足安全条件时，也可能在 fallback display 显示 IME。跨显示且没有 ActivityView 坐标矩阵时，IMMS把 `REQUEST_CURSOR_UPDATES`记成缺失，避免把不可换算的光标坐标交给 IME。

`showSoftInput()`和 `hideSoftInput()`允许一个尚未成为 `mCurClient`、但已登记且被 WMS 认定有焦点的客户端发请求，以覆盖“Window 已聚焦而输入绑定尚未完成”的竞态。反过来，client 已经等于 `mCurClient`时走快路径，不重新查询一次 WMS焦点；IMMS这一层也不直接比较传入 WindowToken是否等于 `mCurFocusedWindow`，标准 App IMM此前的 served/token门承担了正常调用约束。

未知 client在 start入口实际抛 `IllegalArgumentException`，并不返回 `ERROR_INVALID_CLIENT`。不要因为 `InputBindResult.ResultCode`枚举里存在某个名字，就推断当前路径一定使用它。

## 6. `softInputMode`是焦点策略，不是无条件显示命令

当焦点窗口变化时，IMMS把 `softInputMode`、是否文本编辑器、是否前向导航、adjust 模式、屏幕大小和 target SDK 放在一起判断。先区分两个掩码：state 决定自动 show/hide 倾向，adjust 决定窗口如何适配 IME；`ADJUST_RESIZE`在这里还参与 `doAutoShow`，但不等于任何时刻都自动弹键盘。

| state | r48 的关键条件 |
| --- | --- |
| `UNSPECIFIED` | 非编辑器或不适合 auto-show 时可隐藏；编辑器 + resize/大屏 + forward navigation 才隐式显示 |
| `UNCHANGED` | 不主动改变可见意图 |
| `HIDDEN` | 只在 forward navigation 分支隐藏 |
| `ALWAYS_HIDDEN` | 新焦点窗口时隐藏 |
| `VISIBLE` | forward navigation 且 visible 请求被允许时显示 |
| `ALWAYS_VISIBLE` | visible 请求被允许且不是同一已聚焦窗口时显示 |

对 target SDK P 及以上，`VISIBLE`与 `ALWAYS_VISIBLE`还要求 start flags 同时含 `VIEW_HAS_FOCUS` 和 `IS_TEXT_EDITOR`；老目标版本保留兼容放行。`IS_TEXT_EDITOR`来自 `onCheckIsTextEditor()`，它与稍后创建出的 `InputConnection`是否非空仍是两个观察。

需要 auto-show 的 IMMS分支会先 `startInputUncheckedLocked()`，再 `showCurrentInputLocked()`。源码这样排序是为了让键盘在显示前先知道 EditorInfo 和编辑目标；需要换窗口并隐藏旧 IME 时，则先处理旧可见状态，避免新编辑器初始化挡住旧窗口消失。它不是所有 API交错的全局保证：App显式 `showSoftInput()`调用 `checkFocus()`时，若完整 start被重投到另一个 Looper，show仍可先进入 IMMS。

同一窗口已聚焦且仍是文本编辑器时有早返回路径：有 EditorInfo 就直接重新 start，只有 focus 报告则返回 `SUCCESS_REPORT_WINDOW_FOCUS_ONLY`。不要假设每次 `startInputOrWindowGainedFocus()`都会重新执行完整 state switch。

## 7. 选择、绑定 IME 与三类 token

`mCurMethodId`来自当前用户的输入法设置，指向实现 `android.view.InputMethod` Service 的组件。若当前 IME/显示不能复用，IMMS：

1. 清理旧 method 与 WindowToken。
2. 创建 `Intent(InputMethod.SERVICE_INTERFACE)`并指定组件。
3. `bindServiceAsUser()`发起主连接。
4. 绑定请求成功后记录 `mCurId`，创建 `mCurToken`并让 WMS登记 `TYPE_INPUT_METHOD` WindowToken。
5. 先返回 `SUCCESS_WAITING_IME_BINDING`，等待 `onServiceConnected()`。

绑定成功发起不等于 Service 已连接；WindowToken 登记也不等于 IME 窗口已绘制。Service 的 `onCreate()`会构造 `SoftInputWindow`和根布局，真正收到 `initializeInternal()`后才设置 token、更新 display，并把 decor 设为 `INVISIBLE`后调用 `Dialog.show()`把窗口先加进 WMS。这样 Insets controllable listener 可以在真正可见前建立；输入 View 和候选 View 仍可按需创建。

三类 token 不要合并：

| token | 生命周期与用途 | 不提供的保证 |
| --- | --- | --- |
| IME WindowToken `mCurToken` | 一次当前 IME/显示绑定；授权 `TYPE_INPUT_METHOD` 窗口，并校验 privileged operations 来自当前 IME | 某个 App show 请求仍是最新 |
| `startInputToken` | 每次 attach 创建，弱映射到当时 focused window；IME 在处理 start 前报告回来，供 IMMS更新 `mLastImeTargetWindow` | 不与 latest sequence 比较 |
| show/hide input token | 每次可见请求创建，弱映射到请求 App WindowToken；IMS原样带回 `applyImeVisibility()` | 不验证“这是最后一笔请求”，map miss 也没有独立错误返回 |

后两类 token 是 provenance key，不是代际闩锁。`applyImeVisibility()`先用 IME WindowToken确认调用者仍是当前 IME，再以 show/hide token 查原始窗口；show 的 WMS post-layout 还有目标一致性检查，而 hide 最终可作用于该 display 当前 control target。把它们写成“自动拒绝一切过期请求”会高估 r48 的保护。

## 8. Session 的异步闭环与 `InputBindResult`

相同 IME ID 和显示下有三种常见状态：

| IMMS 状态 | 同步返回 | 后续动作 |
| --- | --- | --- |
| 当前客户端已有 `curSession` | `SUCCESS_WITH_IME_SESSION` | 立即 attach 新输入 |
| 已有顶层 `IInputMethod`，尚无 Session | `SUCCESS_WAITING_IME_SESSION` | 开一对 `InputChannel`并请求 `createSession()` |
| 主 Service 已 bind，但尚未得到接口 | `SUCCESS_WAITING_IME_BINDING` | 3秒窗口内等 `onServiceConnected()`；超过后才落入重连 |

`onServiceConnected()`只证明拿到顶层 `IInputMethod`。IMMS先发 `initializeInternal(WindowToken, displayId, privilegedOps)`，再为当前客户端请求 Session。`requestClientSessionLocked()`用 `InputChannel.openInputChannelPair()`创建两端，并用 `sessionRequested`避免同一 ClientState 重复申请。

Session callback 有一个容易读错的所有权边界：`MethodCallback`只捕获 `mMethod`和 system_server 端 channel，没有捕获发起请求时的 `ClientState`。`onSessionCreated()`只核对返回的 method Binder仍是当前 IME；若回调时 `mCurClient`已经换成另一个客户端，它会清理并把该 Session装到**回调当下的当前客户端**，不是必然废弃。只有当前 method 不匹配、没有当前客户端或用户切换等分支才会丢弃并 dispose channel。

`attachNewInputLocked()`随后按当前状态完成：

```text
尚未 bind 到当前客户端 → oneway bindInput(InputBinding)
每次 attach              → 新建 startInputToken 并记录 target
                         → oneway startInput(actual IInputContext, EditorInfo, restarting)
仍有 mShowRequested       → 再安排 show
                         → 返回 Session、dup channel、IME id、mCurSeq
```

`InputChannel`主要承载发给 IME Session 的输入事件及完成通知；文本的 `commitText()`不走这里，而走 `IInputContext`。App 收到新的 channel 会替换并 dispose旧端；system_server/远程 IME各自也有明确的 dup/dispose 边界。

IMMS 每次 start 都递增正数 `mCurSeq`。App 的 `MSG_BIND`只有在 `res.sequence == mBindSequence`时才安装异步返回的 Session；不匹配时会 dispose不属于当前 channel 的返回端。sequence保护的是绑定回调，不会给每一笔 `IInputContext.commitText()`自动加代际标签。

## 9. `bindInput`、`startInput`和输入 View 是三层生命周期

一个 IME Service 可服务多个先后出现的 App 客户端；一个客户端又可在多个编辑器间切换。因此：

- `bindInput(InputBinding)`是客户端级。IMMS切换当前客户端时先对旧端 `unbindInput()`，再对新端 bind。`InputBinding`包含客户端级连接 Binder、UID 和 PID。
- `startInput(actual IInputContext, EditorInfo, restarting)`是编辑器级。同一 App 从搜索框切到消息框，可以不重建 Service，却发生新的 start。
- `onStartInputView()`是 UI 级。只有输入 View真正开始服务当前编辑器时才调用。

IME 侧 `IInputMethodWrapper`收到 oneway Binder 调用后，统一交给 IME 主线程。处理 start 时，它把 App 的 `IInputContext`包装成远端 `InputConnectionWrapper`，应用 `missingMethods`兼容信息，并先让 IMS通过 privileged operations报告 `startInputToken`对应的目标，然后才进入 `startInput()`或 `restartInput()`。

`InputMethodService.doStartInput()`的顺序是：非 restart 时先结束上一输入；设置 `mStartedInputConnection`和 EditorInfo；调用 `onStartInput()`；若 decor 已可见且正在显示输入 View，再调用 `onStartInputView()`。若尚不可见但启用了实验性的 pre-render 条件，则可能预先构建和绘制不可见窗口。

`getCurrentInputConnection()`优先返回编辑器级 `mStartedInputConnection`；没有时才退到 `bindInput()`留下的客户端级 `mInputConnection`。后者通常源自 IMM 登记时的 dummy context，不能把它和当前 EditText 的连接视作同一个对象。

典型 hidden-to-shown 路径中，start 先于 show，因此 `onStartInput()`先于 `onStartInputView()`；但重启时窗口可能已显示，配置变化也会重建 UI。正确不变量是生命周期层次和局部调用顺序，而不是“进程一生只调用一次”的全局序列。

## 10. `showSoftInput()`的布尔值与四套状态账

App IMM 先确认 served View，再同步调用 IMMS。IMMS若发现 client 不是 `mCurClient`，会回查登记并让 WMS确认它当前有焦点，以允许“焦点已到、start 尚未完成”的合法窗口。

`showCurrentInputLocked()`的实际顺序很重要：

1. 立即令 `mShowRequested = true`。
2. 无障碍策略要求不显示时返回 `false`。
3. 根据 App flags 更新 `mShowExplicitlyRequested`和 `mShowForced`。
4. system 未 ready 时返回 `false`。
5. 有 `mCurMethod`时创建 show token、安排 `MSG_SHOW_SOFT_INPUT`、令 `mInputShown = true`、增加 visible bind，并返回 `true`。
6. 只有连接卡住超过阈值才强制 unbind/rebind；其余尚在连接的情况返回 `false`，但保留 show 意图。

这带来三条非对称语义：

- `true`只是 IMMS进入安排分支。跨进程 `IInputMethod`是 oneway；即使发送处抛出 `RemoteException`，`handleMessage()`也吞掉异常，而调用分支仍可把 `mInputShown`设为真并返回真。
- `false`可能已修改 `mShowRequested`，甚至部分 flag 账；等 Session attach 时仍可能重放。
- `mVisibleConnection`只是额外的可见优先级绑定，不创建第二个 IMS，也不证明窗口可见。

App 的 `SHOW_FORCED`/`SHOW_IMPLICIT`不会原封不动交给 IMS。IMMS先把它们记成服务端历史，再由 `getImeShowFlags()`转换为 `InputMethod.SHOW_EXPLICIT`和 `InputMethod.SHOW_FORCED`。因此排查 hide policy 应看 IMMS账，而不是只看 IME最后收到的整数。

## 11. IMS 怎样接受 show、准备 UI 和回复 `ResultReceiver`

跨进程 show 到达 `IInputMethodWrapper`后被投递到 IME 主线程。wrapper 调 `showSoftInputWithToken()`，临时设置 `mSystemCallingShowSoftInput`和 `mCurShowInputToken`，在同步处理完成后立即清掉；target SDK R 及以上的 IME若绕开系统路径直接调用自己的 `InputMethodImpl.showSoftInput()`，会被要求改用 `requestShowSelf()`。

IMS 先记录 `wasVisible`，再执行 `dispatchOnShowInputRequested(flags, false)`。默认策略会拒绝不适合的隐式请求，例如实体键盘场景或会突兀进入 fullscreen 的隐式显示；IME作者也可覆写该策略。只有返回真才执行：

```text
showWindow(true)
  → 防重入
  → prepareWindow：decor 逻辑可见、初始化、fullscreen、input frame、懒建 View
  → startViews：必要时 onStartInputView
  → 上报本地 IME window status
  → onWindowShown，更新 pre-render/window 标志
  → 必要时 mWindow.show() 请求窗口绘制
applyVisibilityInInsetsConsumerIfNecessary(true)
  → 新 Insets 模式才把 show token 带回 system_server
```

`SoftInputWindow`对象和根布局在 Service `onCreate()`已有，WindowToken到达时又把 invisible decor 预先加进 WMS；`onCreateInputView()`与 `onCreateCandidatesView()`仍按需发生。因此“Service connected”“Window object exists”“Window added invisible”“键盘子 View 已创建”“Window visible”是五个状态。

show 处理末尾总会按本地状态调用 `setImeWindowStatus()`，再比较 `wasVisible`与 `isVisible`决定 ResultReceiver 的四种结果。这个结果可能在 WMS尚未通过 post-layout门、Insets尚未拿到 control时发出；`SoftInputWindow.show()`还会吞掉过期 token 导致的 `BadTokenException`并停止后续重试，所以本地 shown 结果更不是窗口添加或物理显示的硬证明。IMMS前置拒绝、oneway投递失败或 IME死亡时，ResultReceiver也没有必达保证。

## 12. 新 Insets 模式怎样把 IME 的 show 变成动画

当 `ViewRootImpl.sNewInsetsMode > NEW_INSETS_MODE_NONE`，IMS不靠 `mWindow.show()`单独决定最终可见性，而是用 privileged operation 把 show token带回 IMMS。r48 的 mode 1仅把 IME迁到新机制，mode 2使用完整新 Insets，系统属性缺省值是 mode 2；两种非零模式都进入这里：

```text
IMS applyImeVisibility(showToken, true)
  → IMMS 校验当前 IME WindowToken
  → mShowRequestWindowMap[showToken] 找原请求 WindowToken
  → WMS showImePostLayout(windowToken)
  → WindowState.getImeControlTarget()
  → ImeInsetsSourceProvider.scheduleShowImePostLayout(controlTarget)
  → 等 IME window 已 drawn、givenInsets 不 pending，且 target 条件满足
  → DisplayContent.mInputMethodControlTarget.showInsets(ime, fromIme=true)
  → App或远程控制目标进入 InsetsController 动画
```

Provider 的 post-layout不是固定延时。它检查 IMMS认为的来源 target 与 DisplayContent 的 IME target/control target关系，并等待 IME窗口已经 layout/draw；同一 Activity 内 target变化还有一次重新检查分支。条件未满足时 runner保留到后续 traversal。新的 hide 只有在 token 映射出的请求窗口仍能被 WMS解析时，才会在由该窗口归一化出的 display中止 pending show；映射或窗口已失效时没有这一步。

`fromIme=true`让 `ImeInsetsSourceConsumer.requestShow()`无条件返回 `SHOW_IMMEDIATELY`；随后 `collectSourceControls()`才决定已有 control时复制它并建立动画 runner，还是在 control为空时只更新 requested visibility、等待 control。如果客户端自己从 `WindowInsetsController.show(ime())`发起，consumer则可能先调用 `InputMethodManager.requestImeShow()`，并在没有 control时记录 awaiting-control，等待服务端完成上述链路。

最终动画使用的 leash、requested visibility、server/client `InsetsState`、Surface transaction 和物理 present都由第 230—231 章描述的分层协议完成。本章这里只增加一个关键入口：IMS本地 UI准备在先，WMS post-layout批准在后；`mImeWindowVis`或 ResultReceiver先变化，不会把后面的边界压成同步调用。

## 13. hide 的标志、状态清理和 Surface 收尾

App `hideSoftInputFromWindow()`先校验 served WindowToken。IMMS再校验当前/聚焦客户端，并在真正派发前执行历史 flag policy：

| hide flag | 拒绝条件 |
| --- | --- |
| `HIDE_IMPLICIT_ONLY` | 此前是 explicit 或 forced show |
| `HIDE_NOT_ALWAYS` | 此前是 forced show |
| `0` | 不受这两道历史门限制 |

通过 flag 门后，r48只要有当前 method，且 `mInputShown`为真或 `mImeWindowVis`含 `IME_ACTIVE`，就创建 hide token并安排 oneway hide。这是对 Eclair以来行为的兼容：show 已安排而 IME状态上报尚未到时，App仍应能立刻撤销。原始 hide flags只在 IMMS用于判断，真正发给 `IInputMethod.hideSoftInput()`的 flags固定为 `0`。

随后 IMMS不等待 IME：撤销 visible bind，并清 `mInputShown`、`mShowRequested`、`mShowExplicitlyRequested`、`mShowForced`。若没达到 should-hide 条件，它返回 `false`但仍执行这组清理；只有前两道 flag policy早返回时才保留原账。

IMS 主线程收到 hide 后先在新模式调用 `applyImeVisibility(false)`。有效当前 IME且 `mCurClient`仍存在时，IMMS才把请求交给 WMS：

```text
hide token → IMMS 映射原请求窗口
  → WMS.hideIme(window, current-client display)
  → 若 window 可解析：按其 control target归一化 display，并中止 pending show
  → 若该 display/current control target存在：hideInsets(ime, fromIme=true)
  → 若该 display存在：Provider.mImeShowing = false
```

非 pre-render 路径随后清本地 show flags并 `doHideWindow()`。旧 Insets 模式会直接 `mWindow.hide()`；新模式的 `hideWindow()`只结束输入/候选 View生命周期、更新逻辑可见状态，并把 input View派发为 `GONE`，让 leash动画保留 Surface。

隐藏动画完成后，`ImeInsetsSourceConsumer`还有两个独立动作：`notifyImeHidden()`经当前 Session让 IMS `requestHideSelf(0)`同步服务端账；`removeImeSurfaceFromWindow()`经 IMMS核对当前 focused window和 enabled Session，再让 IMS在 `!mShowInputRequested && !mWindowVisible`时调用 `mWindow.hide()`把 decor 置为 `GONE`，触发后续窗口与 Surface隐藏回收。这个调用返回仍不证明 Surface已经移除或该状态已经 present；hide API返回、IMS本地 hidden、Insets终帧和 Surface收尾因此不能互换。

IMMS 的 hide flag policy若早返回，请求不会派发到 IMS：API同步返回 `false`，这个 `ResultReceiver`也不会由 IMS回调。请求真正到达 IMS后，结果码只比较处理前后的本地可见状态；状态本来就 hidden时可能返回 unchanged，Surface动画是否结束不参与计算。它不是 SurfaceFlinger present fence。

## 14. 一个字符怎样回写，以及连接怎样失效

IME按键通常调用 `getCurrentInputConnection().setComposingText()`、`commitText()`或 `performEditorAction()`。这是 IME进程里的 `com.android.internal.view.InputConnectionWrapper`，内部持有 App 的 `IInputContext` Binder，而不是 `EditText`对象。

写操作链如下：

```text
IME InputConnectionWrapper.commitText("中", 1)
  → oneway IInputContext.commitText
  → App IInputConnectionWrapper.dispatchMessage(DO_COMMIT_TEXT)
  → 若调用线程不是目标 Looper，则排队；同 Looper可直接执行
  → 再取真实 InputConnection并检查 isActive
  → EditableInputConnection → BaseInputConnection.replaceText
  → 移除/设置 composing span，替换选区，计算新光标
  → TextWatcher、布局、绘制在后续发生
```

IME侧 `commitText()`在 oneway Binder调用未抛 `RemoteException`时就返回 `true`，没有 App编辑结果 callback。App handler稍后可能发现连接 inactive而丢弃，所以这个布尔值连“Editable已修改”都不能证明，更不证明字符已显示。

查询操作不同。`getTextBeforeCursor()`等仍先发 oneway 请求，但附带结果 callback；IME端 wrapper用 `CancellationGroup.Completable`最多等待 2000 ms，把它包装成同步样式 API。超时返回 null/0，`unbindInput()`会 `cancelAll()`唤醒等待者。所谓“异步查询”应理解为传输协议是 callback，而不是调用方一定立即返回。

连接切换时 App IMM的旧 wrapper执行 `closeConnection()`：在目标 Handler上尝试调用真实连接支持的 close；即使底层没有实现该方法，wrapper最终也会将内部连接置空并标 finished。普通编辑请求要求 active；`finishComposingText()`特意允许在 inactive阶段继续清组合态，直到 finished。若具体 InputConnection选择了不同 Handler，已排在 close前的旧消息仍可能先执行；sequence并不标记这些编辑命令。

标准 TextView中，`setComposingText()`保留 `SPAN_COMPOSING`，`commitText()`替换当前组合区或选区并移除组合态；`performEditorAction()`进入 `TextView.onEditorAction()`，不等同于无条件发送 Enter。现代软键盘的文本输入主路是编辑协议，不是一串模拟 KeyEvent。

进程死亡也分层收尾：

- App client Binder死亡：IMMS移除 ClientState、清它的 Session/channel；若它是当前客户端则向 IME发 `unbindInput()`并清当前 client，但不等同于注销用户选择的 IME Service。
- IME Service意外断开：IMMS清所有 client Session和当前 method，但保留主 binding、Intent与 IME WindowToken；它记录新的 bind时刻，把 `mShowRequested`重置为当时的 `mInputShown`，再令 `mInputShown=false`并解绑当前客户端。系统重启已绑定 Service后，App收到 matching unbind且仍 active时也会重新 start。
- 异步 Session晚到：只按当前 method和当前 client规则处理，不能靠“它最初为谁申请”来推断归属。

## 15. 九组 macOS 只读源码练习

以下脚本只读源码。每段都可在 macOS 自带 Bash 3.2或 Zsh 5.9运行；可把另一个源码根作为第一个参数传入。

### 练习 1：还原 focus 到 start 的入口

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'void onPostWindowFocus(View focusedView' frameworks/base/core/java/android/view/ImeFocusController.java
grep -n -F 'public boolean checkFocus(boolean forceNewFocus, boolean startInput)' frameworks/base/core/java/android/view/ImeFocusController.java
grep -n -F 'boolean startInputInner(@StartInputReason int startInputReason,' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'vh.post(() -> mDelegate.startInput' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

把四个命中连起来，并解释为何“post后返回 false”不代表永久失败。

### 练习 2：区分描述快照和编辑协议

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'tba.packageName = view.getContext().getOpPackageName();' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'InputConnection ic = view.onCreateInputConnection(tba);' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
grep -n -F 'outAttrs.setInitialSurroundingText(mText);' frameworks/base/core/java/android/widget/TextView.java
grep -n -F 'InputConnection ic = new EditableInputConnection(this);' frameworks/base/core/java/android/widget/TextView.java
grep -n -F 'if (isPasswordInputType(inputType)) {' frameworks/base/core/java/android/view/inputmethod/EditorInfo.java
```

分别标出 IMM、TextView和 EditorInfo负责的字段，并说明密码保护只覆盖哪份数据。

### 练习 3：逐层列出 IMMS 的信任校验

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (windowToken == null) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'final ClientState cs = mClients.get(client.asBinder());' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (cs.selfReportedDisplayId != windowDisplayId) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (!mWindowManagerInternal.isInputMethodClientFocus(' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (!InputMethodUtils.checkIfPackageBelongsToUid(' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

按“身份、窗口事实、编辑器声明”三列整理命中，指出哪一步仍使用 Binder调用身份。

### 练习 4：手推 `softInputMode` 决策

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'final boolean doAutoShow =' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'case LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED:' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'case LayoutParams.SOFT_INPUT_STATE_VISIBLE:' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'case LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE:' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'static boolean isSoftInputModeStateVisibleAllowed' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodUtils.java
grep -n -F 'if (targetSdkVersion < Build.VERSION_CODES.P) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodUtils.java
```

构造“P+、非文本编辑器、forward navigation、ADJUST_RESIZE”和“P+、文本编辑器、无 forward navigation”两例，写出 `VISIBLE`分支是否 show。

### 练习 5：跟踪 binding、WindowToken 和 Session

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mCurToken = new Binder();' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'mIWindowManager.addWindowToken(mCurToken, LayoutParams.TYPE_INPUT_METHOD,' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'void requestClientSessionLocked(ClientState cs) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'InputChannel[] channels = InputChannel.openInputChannelPair' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'void onSessionCreated(IInputMethod method, IInputMethodSession session,' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'InputBindResult attachNewInputLocked' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

指出 Session callback携带了什么、没携带什么，并解释客户端切换时为什么不能假定它必然被废弃。

### 练习 6：核对 Binder 方向和等待语义

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'oneway interface IInputMethod {' frameworks/base/core/java/com/android/internal/view/IInputMethod.aidl
grep -n -F 'oneway interface IInputContext {' frameworks/base/core/java/com/android/internal/view/IInputContext.aidl
grep -n -F 'oneway interface IInputMethodClient {' frameworks/base/core/java/com/android/internal/view/IInputMethodClient.aidl
grep -n -F 'private static final int MAX_WAIT_TIME_MILLIS = 2000;' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'int SUCCESS_WAITING_IME_BINDING = 2;' frameworks/base/core/java/com/android/internal/view/InputBindResult.java
grep -n -F 'if (mBindSequence < 0 || mBindSequence != res.sequence) {' frameworks/base/core/java/android/view/inputmethod/InputMethodManager.java
```

为每个命中写出调用者能观察到的完成点；特别区分 oneway edit与 callback + 2秒等待的 query。

### 练习 7：审计 show 的非对称返回值

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mShowRequested = true;' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (mAccessibilityRequestingNoSoftKeyboard) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'mShowRequestWindowMap.put(showInputToken, windowToken);' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'MSG_SHOW_SOFT_INPUT, getImeShowFlags(), reason' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'mInputShown = true;' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'if (mShowRequested) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

分别推演“无障碍拒绝”“Service绑定中”“已有 mCurMethod”三条路径，记录返回值和被修改的字段。

### 练习 8：找到 show/hide 进入 Insets 的真正门

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public void showSoftInputWithToken' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'applyVisibilityInInsetsConsumerIfNecessary(true' frameworks/base/core/java/android/inputmethodservice/InputMethodService.java
grep -n -F 'public void showImePostLayout' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'void scheduleShowImePostLayout(InsetsControlTarget imeTarget)' frameworks/base/services/core/java/com/android/server/wm/ImeInsetsSourceProvider.java
grep -n -F 'target.showInsets(WindowInsets.Type.ime(), true' frameworks/base/services/core/java/com/android/server/wm/ImeInsetsSourceProvider.java
grep -n -F 'dc.mInputMethodControlTarget.hideInsets(' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

画出 show等待 layout/draw而 hide会中止 pending show的分叉，并在图中单列 ResultReceiver时刻。

### 练习 9：验证字符回写和失活收尾

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'mIInputContext.commitText(text, newCursorPosition);' frameworks/base/core/java/com/android/internal/view/InputConnectionWrapper.java
grep -n -F 'case DO_COMMIT_TEXT: {' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'ic.commitText((CharSequence)msg.obj, msg.arg1);' frameworks/base/core/java/com/android/internal/view/IInputConnectionWrapper.java
grep -n -F 'content.replace(a, b, text);' frameworks/base/core/java/android/view/inputmethod/BaseInputConnection.java
grep -n -F 'mCancellationGroup.cancelAll();' frameworks/base/core/java/android/inputmethodservice/IInputMethodWrapper.java
grep -n -F 'public void onServiceDisconnected(ComponentName name)' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

说明 `commitText()`在哪一行已经对 IME返回 true，哪一行才可能改 Editable，以及连接失效时 query与 edit分别靠什么收口。

## 16. 用六条时间线收束排查

把本章压成六条互不替代的时序：

```text
焦点线：
Window 可用 IME → current ViewRoot → next served → served → startInputInner

协议线：
EditorInfo 快照 + InputConnection → IInputContext → IMMS校验 → IME远端 wrapper

绑定线：
bindService → IInputMethod → initialize/WindowToken → createSession
→ attach(bindInput, startInput) → InputBindResult/onBindMethod

显示线：
IMMS mShowRequested → oneway show → IMS本地 UI/status/ResultReceiver
→ showImePostLayout → control target → Insets动画 → Surface present

编辑线：
IME commit/composing/query → IInputContext → App目标 Looper
→ active检查 → Editable → View绘制

隐藏线：
IMMS flag门与提前清账 → oneway hide → IMS本地 hidden
→ WMS abort pending show/hideInsets → 动画终帧 → notify/remove Surface
```

最后用这张表定位现象：

| 现象 | 第一组检查 | 不应直接得出的结论 |
| --- | --- | --- |
| `showSoftInput()` 为 `false` | served View、WMS焦点、绑定状态、无障碍策略、`mShowRequested` | 键盘之后一定不会出现 |
| 有 `SUCCESS_WAITING_*` | ServiceConnection、Session callback、sequence | 当前 Session 已可用 |
| `onStartInput()`到了但无键盘 | show flag、`onShowInputRequested()`、WindowToken、post-layout门 | InputConnection创建失败 |
| ResultReceiver为 `RESULT_SHOWN` | IME本地状态、WMS target、Insets control和动画 | 像素已 present |
| IME的 `commitText()`为 `true`但没字 | App wrapper active/finished、目标 Handler、旧连接排队 | App Editable已经成功修改 |
| hide返回后仍短暂可见 | IMS本地状态、pending show、Insets动画、Surface移除 | hide请求未被处理 |

真正掌握这条链，不是记住“IMM调用IMMS、IMMS调用IMS”，而是能随时回答四个问题：**当前字段属于哪个进程和对象；这次 Binder是同步、oneway还是 callback等待；token只证明来源还是也证明最新代际；眼前的完成点停在意图、绑定、UI、Insets、Surface提交还是物理呈现。**
