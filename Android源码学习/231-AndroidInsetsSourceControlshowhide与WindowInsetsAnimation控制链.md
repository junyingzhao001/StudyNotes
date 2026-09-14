# 231 Android InsetsSourceControl、show/hide 与 WindowInsetsAnimation 控制链

本文基于 `android-11.0.0_r48`，从 system_server 选择控制目标开始，追踪 `InsetsSourceControl` 与 leash 的交付、客户端 `show()` / `hide()` 和用户控制请求、每帧 `SurfaceParams`、IME 延迟就绪，以及 `finish()` / cancel / control revoke 怎样回到稳定的 `InsetsState`。

这一章最重要的不变量是：**控制目标已选定、leash 已创建、WMS 已越过准备事务边界、客户端收到 Control、Consumer 接受 Control、动画回调 `onReady()`、一帧参数已提交、控制监听器收到 `onFinished()`、Controller 固化 requested visibility、服务端接纳终态、合成器物理显示，都是不同的完成点。**

版本锚点：

- `frameworks/base/services/core/java/com/android/server/wm/InsetsPolicy.java`
- `frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java`
- `frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowState.java`
- `frameworks/base/core/java/android/view/IWindow.aidl`
- `frameworks/base/core/java/android/view/ViewRootImpl.java`
- `frameworks/base/core/java/android/view/InsetsSourceControl.java`
- `frameworks/base/core/java/android/view/InsetsSourceConsumer.java`
- `frameworks/base/core/java/android/view/ImeInsetsSourceConsumer.java`
- `frameworks/base/core/java/android/view/InsetsController.java`
- `frameworks/base/core/java/android/view/InsetsAnimationControlRunner.java`
- `frameworks/base/core/java/android/view/InsetsAnimationControlImpl.java`
- `frameworks/base/core/java/android/view/InsetsAnimationThreadControlRunner.java`
- `frameworks/base/core/java/android/view/ViewRootInsetsControllerHost.java`

## 1. 先把控制链的完成点拆开

把“先获得状态栏控制权”和“随后执行一次普通 `hide(Type.statusBars())`”拼成一条完整生命周期，代表路径如下；目标选择与 leash 创建通常早于实际 hide 调用，不是每次 hide 都会重做：

```text
InsetsPolicy 选出 real control target
  → InsetsSourceProvider 归一化 target，并用 SurfaceAnimator 创建 leash/Control
  → InsetsStateController 再按 Provider 的实际 target 更新 type ↔ target 映射
  → WMS prepare-surfaces transaction 应用
  → IWindow.insetsControlChanged(state, controls)
  → ViewRoot Handler 先接纳 State，再交付 Controls
  → InsetsSourceConsumer 对齐 requested 与 local visibility
  → InsetsController 去重并收集可用 control 的副本
  → 普通 runner 构造中先派发 onPrepare，并登记 pre-draw
  → runner 入表，再用 showDirectly / hideDirectly 保存意图，并为有 Control 的成员切布局基线
      ↘ local visibility 有变化时，oneway 回报可并行进入 WMS，服务端甚至可能先于 finish 接纳
  → 下一次 pre-draw 后 onStart 与 listener.onReady
  → setInsetsAndAlpha 写 pending 值
  → applyChangeInsets 同时派生临时 State 与 SurfaceParams
  → finish(false) 安排终帧
  → InsetsController 移除 runner，并在需要时再次固化/回报最终选择
      ↘ 若最终选择改变，再发第二次 oneway 请求；后续 State 分发使双方收敛
```

这条链上至少有以下边界：

| 完成点 | 此时可以证明 | 此时仍不能证明 |
| --- | --- | --- |
| Policy 候选选定 | 策略已返回一个候选控制目标 | Provider 归一化后的实际 target 与 leash 已确定 |
| Provider 的 `updateControlForTarget()` 返回 | 成功的真实目标路径已捕获 leash、保存实际 target 并构造 Control | 创建 leash 的事务已越过 WMS 准备边界 |
| target 映射更新 | StateController 已按 Provider 的实际 target 记录双向关系 | leash 已可安全异步交付 |
| after-prepare 回调 | WMS 已调用并关闭本轮 pending transaction，Provider 可异步交付真实 leash | SurfaceFlinger 已 commit、latch 或 present；客户端 Handler 已处理 |
| `onControlsChanged()` 返回 | Consumer 已拿到、失去或替换 Control | 动画一定已经进入 `onReady()` |
| `listener.onReady()` | 调用者获得可驱动的 animation controller | 任意 `setInsetsAndAlpha()` 已提交 |
| `applySurfaceParams()` 返回 | 参数已交给同步 RT 应用器，或非硬件/异步路径的 transaction 已 `apply()` | SurfaceFlinger 已 present |
| control listener `onFinished()` | 控制协议不再接受普通逐帧修改 | 服务端 State 一定已回传 |
| `InsetsController.notifyFinished()` | runner 已移除，终态 requested visibility 已固化 | 新布局、绘制和物理显示已完成 |

因此，日志里只写“Insets 动画完成”没有诊断价值。至少要补充：是谁的回调、哪一线程、哪个 type mask、Control 是否仍在、local/requested/server 三份可见性分别是什么。

## 2. State、Control、leash 与 runner 是四种不同事实

四个对象回答四个问题：

| 对象 | 核心字段 | 回答的问题 |
| --- | --- | --- |
| `InsetsSource` / `InsetsState` | type、frame、visibleFrame、visible | 这个 inset 当前怎样参与布局与可见性计算 |
| `InsetsSourceControl` | internal type、可空 leash、surface position | 目标是否获得控制凭据，以及 surface 的基准位置 |
| `InsetsSourceConsumer` | requestedVisible、当前 Control、pending frame | 客户端想要什么，如何与服务端事实对齐 |
| animation runner | Control 副本、起止 Insets、fraction、alpha | 一段动画如何把请求变成逐帧 State 与 surface 参数 |

`InsetsSourceControl` 不是 `InsetsState` 的替代品。拿到 State 只说明可以计算布局；拿到无 leash 的 Control 只说明仍有一条意图通道；拿到非空 leash 才具备直接变换对应 surface 的能力。

这个类也不是深度不可变对象：

- 普通构造函数直接保存传入的 `Point`，`getSurfacePosition()` 也返回内部对象。
- copy constructor 会新建 `SurfaceControl` handle，并复制 `Point`。
- `release(Consumer)` 只把 leash 交给释放函数，不清空字段，也没有自己的防重复标记；标准 `SurfaceControl.release()` 会以 native pointer 是否为 0 防止第二次 native release。

所以所有权不能靠“Java 对象还在”判断。服务端交付的 Control、Consumer 持有的 Control、runner 为本段动画复制的 Control，是不同 handle 生命周期；包装类本身允许重复调用 release consumer，只是这里常用的 `SurfaceControl.release()` 会让第二次调用成为 no-op。

surface position 同样不是 Insets 厚度。它是 leash 在父层级中的基准坐标；每帧矩阵以它为起点，再叠加由 `shownInsets - pendingInsets` 算出的外移量。

## 3. 服务端先选目标，再为真实目标制造 leash

`InsetsPolicy` 决定状态栏和导航栏由谁控制。候选者可能是焦点窗口、顶部全屏窗口、通知栏、远程 Insets controller、dummy target，或者无人控制。transient bar 场景还会同时出现 real target 与 fake target：dummy target 真正操纵 surface，原应用只拿无 leash 的 fake control，用来表达“我想重新显示”的意图。fake target 只在 FULL Insets mode 生效，并且 Policy 只在 real target 是 dummy 时把焦点窗口选为 fake target。

`InsetsStateController.onBarControlTargetChanged()` 把 status、navigation、climate、extra-navigation 四个 internal type 分别送进 `onControlChanged()`。后者只为已存在且 `isControllable()` 的 Provider 工作，并维护两组关系：

```text
internal type → real/fake target
target → 它当前收到的 internal type 列表
```

IME 的目标选择不能用 z-order 上的 `mInputMethodTarget` 代替：DisplayContent 依据 input target 或 remote target 求 control target，Provider 对带 Window 的候选还会经 `WindowState.getImeControlTarget()` 做 host/fallback 归一化。目标为空时也不直接变成“无人控制”，而是转给 `mEmptyImeControlTarget`。注释给出的理由很直接——必须始终有人持有能隐藏 IME 的 leash，否则 IME surface 可能意外可见。

真正的 `InsetsSourceProvider.updateControlForTarget()` 依次处理：

1. seamless rotation 中暂不换控制。
2. 普通窗口目标先经 `getImeControlTarget()` 归一化；IME control target 可能不是传入窗口本身。
3. backing window 尚不存在时，只保存 `mPendingControlTarget`。
4. target 为空时取消现有 control animation，并把 client visibility 恢复为该 type 的默认值。
5. target 有效时创建 `ControlAdapter`，通过 `WindowState.startAnimation()` 让 `SurfaceAnimator` 建立 leash。
6. Provider 保存 target、更新 visibility，并用 captured leash 与窗口左上角创建 `InsetsSourceControl`。

这里的“animation”主要借用了 `SurfaceAnimator` 的 leash 与生命周期管理，不表示服务端正在替应用跑一个有时长的位移动画；`ControlAdapter.getDurationHint()` 返回 0，但 adapter 不调用传入的 finish callback，0 也不表示这份 ownership animation 会自动结束。它会持续到窗口动画被取消或替换。IME 的 leash 在创建事务中还会被设为 alpha 1 后隐藏，初始显隐由后续控制链接管。

强制重建 Control 时还有一层竞态保护：Provider 先把 `mAdapter` 指向新 adapter，`SurfaceAnimator` 启动新 animation 时会取消旧 adapter；旧 `onAnimationCancelled()` 只有在 `mAdapter == this` 时才能清 Provider 字段。因此 surface position 改变触发的新 leash，不会被迟到的旧取消回调误删。

## 4. leash 交付必须跨过 prepare-surfaces 屏障

创建 leash 后，Provider 立即把 `mIsLeashReadyForDispatching` 设为 false。原因不是对象还没构造好，而是创建、reparent、position 等服务端事务尚未越过应用边界；若客户端先提交 leash 操作，随后到达的服务端准备事务可能覆盖它。

Control 有同步和异步两类交付入口，范围不能混写。window add/relayout 的同步返回参数可以在屏障前带回 active controls；此时真实 target 得到的是同 type、真实 position、null leash 的占位 Control。target 变化触发的异步 `IWindow.insetsControlChanged()` 才按下面的 after-prepare 顺序交付真实可操作 leash。

`InsetsStateController.notifyPendingInsetsControlChanged()` 把通知挂到 `WindowAnimator.addAfterPrepareSurfacesRunnable()`。回调顺序是：

```text
遍历所有 Provider：onSurfaceTransactionApplied()
  → 每个 Provider 把 leash-ready 置 true
  → 遍历 pending target：notifyInsetsControlChanged()
  → WindowState 同包发送目标 State 与 Controls
```

这里的 after-runnable 只证明 WMS 已对本轮 pending transaction 调用 `apply()` / `close()`，不是 SurfaceFlinger commit、latch、present 或 fence 完成信号；它解决的是服务端与客户端 transaction 的提交次序。

屏障前若有人调用 `getControl(realTarget)`，Provider 不泄露刚建好的 leash，而是临时构造同 type、同真实 position、`leash == null` 的 Control。屏障后才返回真正的 `mControl`。fake target 无论何时都拿预建的 null-leash `mFakeControl`；它的 position 固定为构造时的 `(0,0)`，只是占位数据，不能当作来源窗口的几何基准。

控制变化通过 `IWindow.insetsControlChanged(InsetsState, InsetsSourceControl[])` 同包传递，而非只传 Control。客户端 `ViewRootImpl` 的 Handler 刻意先调用 `onStateChanged()`，再调用 `onControlsChanged()`：

- 获得控制时，Consumer 可以拿最新服务端 visibility 判断是否需要补动画。
- 失去控制时，Consumer 可以从 `mLastDispatchedState` 恢复服务端事实。

这也解释了为什么排查 control handoff 必须把同一消息里的 State 和 Controls 一起记录。只看后者，会错过客户端决策所使用的比较基准。

seamless rotation 是另一种延迟：`startSeamlessRotation()` 置标志并取消旧 animation；旋转期间 `updateControlForTarget()` 直接返回，既不更新 target，也不保存为 pending；`finishSeamlessRotation()` 只清标志并记录 frame number，并不会自行重建 Control。之后若其他流程再次创建 leash，且不是 timeout、窗口有 surface，才把来源 surface 与新 leash 的 transaction defer 到新方向首帧 barrier。可控 IME Provider 的 backing window 在 `WindowState` 中会跳过这条 seamless-rotate 操作。

## 5. Consumer 的 setControl 是一次状态归并，不是字段赋值

`InsetsController.onControlsChanged()` 先把数组按 internal type 放入临时表，然后做两轮遍历：第一轮覆盖已有 Consumer，包括为被撤销的 type 传入 null；第二轮为以前未见过的新 type 创建 Consumer。这样“数组没带某 type”才能被解释为撤权，而不是保持旧引用。

`InsetsSourceConsumer.setControl()` 只有在传入对象与当前对象引用相同时才直接返回。Parcelable 重建或 copy constructor 得到的新包装即使指向同一 surface，也仍会进入替换逻辑；后面只在决定是否直接同步 leash visibility 时用 `isSameSurface()` 比较底层 surface 身份。

获得控制的归并规则是：

- `requestedVisible` 与本地 `mState` visibility 不同，说明要补动画。
- leash 非空，且有差异或以前因无 leash 留下 `mIsAnimationPending`，把对应 public type 累加进 `showTypes` 或 `hideTypes`。
- 有差异但 leash 为空，只记录 animation pending；无 surface 能力时不伪造动画。
- 不需动画时仍执行 local visibility override；若拿到新 leash，还用一次直接 transaction 同步 show/hide。
- 请求隐藏且没有待执行动画时，普通 Consumer 的 `removeSurface()` 是空操作，IME 子类会要求 IMM 清理 IME surface。

`showTypes` / `hideTypes` 是 public mask，但 Consumer 和 Control 都以 internal type 为单位。例如 status bar 与 climate bar 可共同折进 `statusBars()`；启动 runner 前仍以 internal type 收集各自 Control。

两轮归并后，Controller 会先调用 controllable-insets listeners。若应用在回调中已经为某些 type 启动 control/show/hide，`mLastStartedAnimTypes` 会把这些 type 从自动补偿的 `showTypes` / `hideTypes` 中剔除；这是“应用在控制权到达回调里接管动画”不会立刻又被系统补动画覆盖的保障。

失去控制的路径更重要：先通知 Controller 取消覆盖该 public type 的 runner，再把 local Source visibility 恢复为最后一次服务端 State 的值，更新 legacy 兼容位，最后释放旧 Control。客户端的 `requestedVisible` 意图不会因此自动改回服务端值；将来重新获得控制时，两者的差异正是补动画的依据。

## 6. show/hide 先去重，再把意图和动画能力分开

`show(types)` 与 `hide(types)` 先把 public mask 展开为受支持的 internal type，再按 requested visibility 与当前 animation type 去重。r48 的 `toInternalType()` 只展开 status、navigation、caption、display cutout 与 IME；gesture、tappable 和内部 window-decor bit 不会因出现在任意 public mask 中就自动获得 Consumer 控制链。

- 已请求显示且未动画，或已经在 SHOW，新的 show 是空操作。
- 已请求隐藏且未动画，或已经在 HIDE，新的 hide 是空操作。
- 来自 IME 的 show 遇到应用正在运行 USER animation，不会把用户控制抢走。

源码里的条件依赖 `&&` 高于 `||` 的 Java 运算符优先级。应读成 `(requested && NONE) || SHOW`，不是 `requested && (NONE || SHOW)`；hide 分支同理。

默认 show/hide 并不直接在入口调用 `Transaction.show()` / `hide()`。它们进入 `applyAnimation()`，建立 `InternalAnimationControlListener`，再走与用户控制共用的 `controlAnimationUnchecked()`。默认参数在 r48 中为：

| 场景 | 时长 | Insets 插值 | alpha 插值 |
| --- | ---: | --- | --- |
| 非 IME show | 275 ms | system-bars curve | 恒为 1 |
| 非 IME hide | 340 ms | system-bars curve | 恒为 1 |
| IME、有 View animation callback | 285 ms | sync-IME curve | 恒为 1 |
| IME、无 View animation callback | 200 ms | show 与 hide 使用不同曲线 | show 前半程淡入；hide 使用退出曲线 |

时长和插值器是这份版本的实现值，不是 API 永久保证。`applyAnimation()` 初次发起默认动画时传 display frame 计算 shown/hidden bounds，避免应用窗口自身 frame 使系统栏端点被裁错；用户控制初次请求传当前窗口 `mFrame`。IME 进入 pending 后的恢复例外见第 8 节。

duration/interpolator 的判断看整份 requested mask：只要混合请求包含 IME，整段默认动画就选择 IME 分支，而不是让系统栏继续使用自己的 275/340 ms。alpha 同样如此；只有无 View callback 的 IME 路径使用淡入淡出，系统栏以及有 callback 的 IME 都保持 alpha 1。

“直接”首先改变的是 requested visibility：SHOW 在 runner 建立后调用 `showDirectly(types)`，HIDE 调用 `hideDirectly(types, animationFinished=false, type)`。只有当前持有 Control 的 Consumer 才能通过 local visibility override 改本地 State；混合请求中无 Control 的成员只保存意图，不能据此宣称 View 已切到该成员的布局基线。对有 Control 的成员，View 先按基线布局，画面上的连续移动仍由 leash 完成。

还要区分三种名字相同的方向：应用主动调用 `InsetsController.show/hide`；WMS、DisplayPolicy 或 IME Provider 经 `WindowState.showInsets/hideInsets` 向控制目标发送 `IWindow.showInsets/hideInsets`；fake target 回报“想显示”供 Policy 中止 transient。`IWindow` 整个接口是 oneway：跨进程 Proxy 调用返回最多证明 Binder 消息已提交；同进程 local-interface 快路径可以直接进入 Stub，但 ViewRoot 仍把工作投到 Handler，所以返回最多证明消息已入队，不证明 Handler 已处理。服务端下发命令也不直接改 Provider 的 raw visibility，客户端仍需运行 Consumer/动画并经 `IWindowSession.insetsModified` 回报。

## 7. 用户控制要依次通过可控性、冲突与 Control 三道门

应用调用 `controlWindowInsetsAnimation()` 后，不会因为传入了 type 就必然收到非空 controller。

第一道门是窗口 frame。`calculateUncontrollableInsetsFromFrame(mFrame)` 只要与请求 mask 有交集，整个请求立即 `onCancelled(null)`；这里不是“过滤掉不可控部分继续”。

第二道门是 Source 的 user-controllable 状态。服务端新 State 若带来“非 null 且为空 Rect”的 visibleFrame，`InsetsController.updateState()` 会把对应 public type 加入 `mDisabledUserAnimationInsetsTypes`；visibleFrame 为 null 反而表示沿用 frame，仍可由用户控制。USER 请求会从 mask 中删掉禁用 type；删完为 0，仍是 `onCancelled(null)`。正在运行的 USER animation 遇到 Source 变得不可控时，Controller 会 post 一次 `show()`；该 show 执行时取消重叠 USER runner，并让布局回到可用状态。

第三道门是当前 Control。Controller 先取消所有与新 mask 有交集的旧 runner，再调用 `collectSourceControls()`：

- 每个非 null Control 包装先通过 copy constructor 放进本轮候选表；leash 是否为 null 不参与 `typesReady` 判定。只有 readiness 检查通过并真正构造 runner 后，这张表才成为 runner 私有表。
- 普通 show/hide 没拿到 Control，也会更新 requested visibility，等以后控制权到来再补动画。
- USER 请求没有 Control 时不会偷偷改成默认动画；若最终 `typesReady == 0`，回调 `onCancelled(null)`。
- 一个请求包含多个 public type 时，`typesReady` 以 public bit 为粒度：每收集到一个 internal Control，就 OR 入它映射的 public bit；同一 public family 里只拿到一个 internal Control，也可能把整枚 public bit 标 ready。listener 的 `onReady(controller, typesReady)` 告知的是这个 public mask，不是实际 Control internal type 的精确集合；IME 尚未 ready 是例外，会延迟整份原始请求。

重叠取消期间用 `mTypesBeingCancelled` 防重入。旧 listener 的取消回调若同步尝试为同一 type 再开动画，会抛 `IllegalStateException`，而不是在列表遍历中悄悄制造第二个 runner。

Control 的复制是所有权边界：Consumer 保留原 Control，ready 后的 runner 操作自己的 handle。动画结束或取消释放 runner 副本，不应顺手释放 Consumer 仍需用于下一段动画的凭据。但 IME delayed 是 r48 的例外缺口：若混合请求已复制其他 type，随后发现 IME 未 ready，方法直接保存不含 controls 的 `PendingControlRequest` 并返回；本轮候选副本没有显式 release，恢复时又会重新复制，只能依赖这些失去引用的 `SurfaceControl` 之后由 GC/finalizer 释放 native 引用。

## 8. IME show 是一次带应答的两阶段请求

普通栏的 `requestShow()` 总是立即可运行；IME 必须先让 `InputMethodManager` 与输入法进程准备窗口。因此 `ImeInsetsSourceConsumer.requestShow(fromIme)` 有三个结果：

| 结果 | 条件 | Controller 行为 |
| --- | --- | --- |
| `SHOW_IMMEDIATELY` | 请求来自 IME；或 IME Source 已 visible 且已有 Control | 继续收集 Control |
| `IME_SHOW_DELAYED` | `requestImeShow(windowToken)` 接受请求 | 保存整份 pending control request |
| `IME_SHOW_FAILED` | IMM 拒绝，例如没有可服务的 editor | 跳过 IME，其他可运行 type 仍可继续 |

若调用时还没有 Control，IME Consumer 同时置 `mIsRequestedVisibleAwaitingControl`。它与普通 `requestedVisible` 做或运算，使 Control 稍后到达时仍能触发 show，而不会因为旧 local State 是隐藏就丢失意图。

这个字段在 r48 不是一次性 latch：代码只在窗口失焦时清 false，获得 Control、SHOW_FAILED 或后续 hide 都不会消费它。因而一次“无 Control 时请求显示”可能持续影响同一焦点周期内后来到达的 Control；排查时要记录它何时置位以及是否发生过 focus loss。

延迟请求保存 types、listener、duration、interpolator、animation type、layout mode、CancellationSignal 和是否使用 animation thread，并设置 2000 ms 超时；它不保存最初用于计算端点的 frame。IME 回调恢复请求时固定传当前 `mFrame`，所以默认 IME show 最初以 display frame 发起、随后进入 pending 时，恢复路径会改用当时的 window frame。至少五类退出路径要分清：

1. IME 随后以 `fromIme=true` 调用 show：取出 pending 请求，移除超时，重新进入 `controlAnimationUnchecked()`。
2. CancellationSignal 先取消：只要保存的仍是同一请求，就 `onCancelled(null)` 并清空。
3. control 被撤销：IME Consumer 通知 Controller，pending 请求同样以 null controller 取消。
4. 2 秒到期：Handler 执行同一个 abort 方法。
5. 新的重叠 IME 请求、显式 `cancelExistingAnimations()` 或另一个 delayed 请求取代旧请求：`cancelExistingControllers()` / 保存新请求前的 abort 会先取消旧 listener。

“IME 已预渲染”是另一条较早的兼容逻辑。`onPreRendered()` 只在 `mShowOnNextImeRender` 已置位、focused 与 pre-rendered `EditorInfo` 被判相似时调用 `applyImeVisibility(true)`。在 r48 当前文件里，没有可达代码把该标志置 true；注释也明确把自动置位列为尚未接上的工作，所以不能把这条分支描述成稳定的 show 主路径。

这份版本的 `areEditorsSimilar()` 还保留明显瑕疵：private options 的 null 比较不对称；extras null 条件重复了同一方向，反方向可能继续解引用 null；`info1.extras.equals(info1)` 比错对象；两个 `Parcel` 未回收。它们适合作为版本审计证据，不应被概括成 EditorInfo 的正确通用相等算法。

IME hide 的通知时点取决于来源。应用发起、`fromIme=false` 的 hide 在 `collectSourceControls()` 阶段就先调用一次 `notifyImeHidden(token)`；若动画正常到达终帧，`ImeInsetsSourceConsumer.hide(animationFinished=true, ...)` 会再次通知并调用 `removeImeSurface(token)`。来自 IME 的 hide 跳过前置通知，正常终帧才通知和清理；若根本没有 Control，应用路径可能只有前置通知而没有动画终帧。因此“请求被收集”“Insets 切到隐藏布局”“输入法收到一次或再次隐藏通知”“IME surface 请求清理”必须按 `fromIme` 与 Control 是否存在分别记录。

## 9. onPrepare 先到，布局基线在 pre-draw 前切换

普通 runner 的精确顺序比“runner 创建后开始回调”更细：`InsetsAnimationControlImpl` 构造器内部先调用 Controller 的 `startAnimation()`，它立即派发 prepare 并登记 pre-draw；构造器返回后，Controller 才把 runner 加入 running 列表、安装 CancellationSignal listener，并根据 `layoutInsetsDuringAnimation` 保存 requested visibility；其中有 Control 的成员同时切换本地布局基线。

```text
构造 InsetsAnimationControlImpl
  → dispatchWindowInsetsAnimationPrepare(animation)
  → 登记 pre-draw runnable
  → 构造返回，runner 加入 running 列表
  → 安装 CancellationSignal listener
  → showDirectly / hideDirectly 切布局基线
  → 下一次 pre-draw：dispatchWindowInsetsAnimationStart(animation, bounds)
  → 标记 startDispatched 与 readyDispatched
  → listener.onReady(controller, typesReady)
```

系统发起的 SHOW 固定选 SHOWN，HIDE 固定选 HIDDEN；用户控制默认选择“当前 requested 状态的反面”，若多个已有 Consumer 中任意一个当前请求隐藏，则整体选 SHOWN，否则选 HIDDEN。这不是把 surface 瞬移到终点，而是让 View 树先拿到本段动画采用的稳定布局基线。默认 SHOW/HIDE 的基线就是最终端；USER animation 的基线只是临时反面状态，调用者稍后仍可用 `finish(true/false)` 选择最终状态。

`onPrepare()` 与 `onStart()` 之间由此保留一次布局机会，应用可以在 prepare 时记录旧几何，在 start 时读取新几何，再用 animation progress 把内容从旧位置过渡到新位置。

如果 pre-draw 前已取消，runnable 只退出：不会派发 start，也不会调用 onReady；control listener 收到的是 `onCancelled(null)`。`null` 的含义不是“没有发生取消”，而是调用者从未获得一个 ready controller。

`mReadyDispatched` 在 `onStart()` 之后、listener `onReady()` 之前置 true。因而 onReady 内同步触发 CancellationSignal 时，`onCancelled()` 会收到 controller 本身；这个细节可以区分“就绪前被撤销”和“应用拿到控制后取消”。

无 View animation callback 的默认 show/hide 不走这套 View 回调：它使用 `InsetsAnimationThreadControlRunner`，在专用 Handler 上直接调用内部 listener 的 `onReady()`。这个 onReady 在线程 runner 构造器内被 post，主线程要等构造返回才把 runner 入表并切布局基线；两线程之间不能从源码推出绝对先后。该 runner 也没有把内部 `mReadyDispatched` 置 true，所以即使异步 listener 已收到 onReady，随后 cancel 仍会把 null 传给它的 `onCancelled()`；“ready 后取消一定带 controller”只对上面的普通路径成立。用户自定义控制始终使用普通 runner，因为应用自己就是逐帧驱动者。

## 10. hidden、shown、current 与 side map 在构造期一次确定

`InsetsAnimationControlImpl` 构造时深拷贝当前 `InsetsState`，并计算三组 Insets：

- `current`：按构造瞬间的 visibility 计算。
- `hidden`：把所有受控 internal Source 临时置为不可见后计算。
- `shown`：把它们置为可见后计算，同时填写 `typeSideMap`。

调用顺序有个容易忽略的结果：同一个 `mInitialInsetsState` 先算 hidden、后算 shown，所以构造结束时这份内部 State 的受控 Source visibility 已是 shown；它保存的 frame 仍是动画几何基线，但并非一份 visibility 完全不变的“原始快照”。每帧是否可见会写到传入的临时 State，而不是再依赖这份对象的 visibility。

`typeSideMap` 把 internal type 映射到 left/top/right/bottom；`sideSourceMap` 再反向把同一边上的一个或多个 Control 分组。每帧输入只有四边聚合 Insets，因此同边多个 Source 会收到相同的边位移。源码直接标出“一个 inset 跨多个 type”的行为仍未完整实现，所以不能假设聚合值能独立还原每个 Source 的厚度。

zero-insets IME 是特殊情况：若控制 IME 且 shown.bottom 仍为 0，就强制把 IME 映到 bottom。默认动画把 hidden bottom 替换为 `-80dp`（经 host 转 px），从屏幕外产生位移，即使布局意义上的 IME Insets 端点都是 0。

特殊情况也改变输入约束：普通 controller 会把传入 Insets 逐边 clamp 到 hidden 与 shown 之间；zero-insets IME 直接接受 Insets，不做这层 clamp。alpha 与 fraction 无论哪种情况都压到 `[0, 1]`，Insets 为 null 则沿用 current。

## 11. setInsetsAndAlpha 只写 pending，apply 才形成一帧

对普通路径，应用调用：

```java
controller.setInsetsAndAlpha(nextInsets, nextAlpha, fraction);
```

方法先拒绝 finished/cancelled controller，再 sanitize 三个输入，写入 `mPendingInsets`、`mPendingAlpha`、`mPendingFraction`，最后请求 Controller 调度 apply。getter 返回的 `current` 只在 `applyChangeInsets()` 后更新；所以 pending 与 current 是两个完成点。

逐帧位移的核心公式是：

```text
offset = shownInsets - pendingInsets

LEFT   : baseX - offset.left
TOP    : baseY - offset.top
RIGHT  : baseX + offset.right
BOTTOM : baseY + offset.bottom
```

例如底部栏 shown.bottom=120、pending.bottom=45、control position=(0,2280)，则 `offset.bottom=75`，矩阵位置变为 `(0,2355)`；同时临时 Source frame 也向下偏移 75。这样 surface 与回调中用于计算 `WindowInsets` 的几何指向同一视觉位置。

每个有 leash 的 Source 生成一个 `SurfaceParams`：surface、alpha、translation matrix、visibility。leash 为 null 时不生成 surface 参数，但临时 State 仍会更新；这正是 fake/system-owned control 能参与请求语义、却不能由客户端移动真实 surface 的边界。

普通 Source 的逐帧 visibility 由该边 `inset != 0` 决定，与 alpha 无关。zero-insets IME 则在 SHOW 全程 visible；其他 animation type 在 finish 前 visible、finish 后 hidden。这个分支只显式识别 SHOW，意味着 r48 的 USER zero-insets IME 即使 `finish(true)`，终帧临时 State 也会按“非 SHOW 且已 finished”算成 hidden；这是该版本的实现边界，排障时应直接核对最终服务端 State，不能从 `finish(true)` 名字反推这一帧临时 visibility。

perceptible 是单独的反馈信号：实际公式使用刚写入的 pending Insets 与 pending alpha，四边都满足 `100 * pending >= 5 * (shown - hidden)`，且 alpha 至少 0.5，才报告 true；状态只在首次计算或翻转时回报。它在 `setInsetsAndAlpha()` 中发生于 schedule/apply 调用之后，但 USER 的 schedule 是同步的、默认帧 schedule 可以只是入队，因此不能统一声称它发生在 surface apply 的前或后。它不是 SurfaceFlinger 的可见性判定，也不是动画完成条件。

## 12. 有无 View callback 决定调度器，不改变请求协议

普通 runner 的 `scheduleApplyChangeInsets()` 有三种行为：

| 场景 | apply 时机 | View animation callback |
| --- | --- | --- |
| 正在派发 `onReady()` 的首帧 | 同步运行 `mAnimCallback` | 有 |
| USER animation | 每次调用同步运行 `mAnimCallback` | 有 |
| 默认 SHOW/HIDE 的后续帧 | 投递到 `CALLBACK_INSETS_ANIMATION` 阶段并合并 | 有 |

`mAnimCallback` 先复制当前 `mState`，再让所有普通 `InsetsAnimationControlImpl` 把 pending 值应用到同一临时 State。它在 apply 前保存已经 start-dispatched 的 animation 列表，即使某个 control 在本次 apply 中 finish，本次 `onProgress()` 仍包含它；progress 之后才对 finished control 派发 `onEnd()`。

多动画聚合的意义是：一次 View callback 看到同一时刻所有运行中的 Insets，而不是 status bar 和 IME 各自发送一份互相覆盖的 `WindowInsets`。surface 参数则仍按每个 runner 各自提交。

这里有两套不能混叫“listener”的回调。`WindowInsetsAnimationControlListener` 只管理一次控制请求的 ready/finished/cancelled；`WindowInsetsAnimation.Callback` 则沿 View 层级接收 prepare/start/progress/end。后者的 `onStart()` 可返回缩小后的 Bounds，`onProgress()` 可返回变换后的 WindowInsets，供 descendants 继续处理；`DISPATCH_MODE_STOP` 会截断该 View 的子树。`ViewGroup` 把父节点返回值作为同一输入传给每个直接 child，但忽略某个 child 的返回值，所以一个 sibling 的变换不会串给下一个 sibling。

硬件加速的 ViewRoot Host 把 `SurfaceParams` 交给 `SyncRtSurfaceTransactionApplier`，使 surface transaction 与 View 渲染帧同步；非硬件加速时则直接建立 `SurfaceControl.Transaction` 并以 frame `-1` 应用，源码明确标为没有同步。两种方法返回都不等于屏幕已经扫描显示。

当 Host 没有任何 View animation callback 时，默认动画改用 `InsetsAnimationThreadControlRunner`：

- `ValueAnimator` 在 `InsetsAnimationThread` 上运行，并使用 SurfaceFlinger vsync provider。
- 每次 apply 直接创建 `SurfaceControl.Transaction`、写参数、`apply()` 后 close。
- perceptible 与最终 `notifyFinished()` 再 post 回主线程。
- 内部临时 `InsetsState` 只为 surface 几何服务，不向不存在的 View callback 派发 progress。

两条路径改变线程和提交方式，不改变 show/hide 请求、Control 收集、IME ready 与最终 requested State 协议。分析竞态时必须先确认 `Host.hasAnimationCallbacks()`，否则会把动画线程日志误认为主线程乱序。

r48 的 ViewRoot Host 对 prepare、start、progress 都检查根 View 是否仍存在，`dispatchWindowInsetsAnimationEnd()` 却直接解引用 `mViewRoot.mView`；`applySurfaceParams()` 在 View 不存在时也会抛异常。若窗口在动画中 detach，不能先验假设所有尾回调都只是安静丢弃，应把 detach 时点与 end/apply 的堆栈一起保留。

## 13. finish 有控制协议完成、runner 收口和服务端收敛三层

`WindowInsetsAnimationController.finish(shown)` 首先做的是本地协议收口：记录 `mShownOnFinish`、置 `mFinished=true`，把 pending Insets 设为 hidden 或 shown、alpha 设 1、fraction 设 1，并安排一次最终 apply。随后调用 control listener 的 `onFinished(controller)`。

但 requested visibility 不是等到这里才第一次变化。runner 创建后、`onReady()` 之前，layout mode 已经触发一次 `showDirectly()` 或 `hideDirectly()`；只要 local Source visibility 改变，`notifyVisibilityChanged()` 就会立即构造客户端 requested State，并经 oneway `IWindowSession.insetsModified` 发给 WMS。默认 SHOW/HIDE 的 layout endpoint 与最终 `finish(show)` 参数相同，finish 时的第二次调用通常对 visibility 幂等；USER controller 可以选择与初始 layout endpoint 相反的 `shown`，这时 finish 才会翻转并再次回报。

最终 apply 到达 `applyChangeInsets()` 时，才调用外层 `InsetsController.notifyFinished(runner, shown)`。外层再完成：

1. 以“不重复调用 cancel callback”的方式从 `mRunningAnimations` 移除 runner。
2. 解冻各 Consumer 在动画期间暂存的新 frame。
3. `shown=true` 时 `showDirectly(types)`；false 时以 `animationFinished=true` 执行 hide。
4. 若最终选择改变 local visibility，通过 `mRequestedState` 与 `Host.onInsetsModified()` 再回报 WMS；默认动画通常早已回报同一端点。
5. 对已经 start-dispatched 的普通 runner，在 progress 后派发 `onEnd()`。

回调相对顺序不能脱离调度路径概括。USER animation 的 schedule 是同步的，外层 runner 收口甚至可能在 `finish()` 内部调用 control listener `onFinished()` 之前完成；默认有 View callback 的后续帧通常先把终帧排入 animation callback，再从 `finish()` 返回。异步 runner 又会先在线程侧结束并 post 主线程收口。

客户端 `mRequestedState` 与服务端 `WindowState.mRequestedInsetsState` 都带增量、粘性语义：客户端只刷新自己当前有 Control 的 Source；服务端只合并这次参数中出现的 Source，缺失项不会被删除。`Session.insetsModified()` 在 global lock 内完成 merge，再交给 Policy 与 StateController。

Provider 只接受 caller 正是当前真实 `mControlTarget` 的 visibility 修改；fake target 不能改 `mClientVisible`，Policy 只观察它的 visible 请求来中止 transient。真实修改若改变 client visibility，Provider 立即按 `mServerVisible && (mirrored || mClientVisible)` 更新 raw Source，并另投递 layout/layer 消息；`InsetsStateController.onInsetsModified()` 在同次处理里立即 `notifyInsetsChanged()`，不必等下一轮 post-layout。若 `mServerVisible=false`，即使请求显示，raw visibility 仍可能是 false。

`IWindowSession.insetsModified` 的 AIDL 契约是 oneway：常见跨进程 Proxy 路径没有“WMS 已接纳”的同步应答；local-interface 快路径即使直接进入 Stub，也不应被外推成跨进程确认语义。随后的 `IWindow.insetsChanged` 同样要区分远端 Binder 投递与同进程直接 Stub/Handler 入队。方法返回、Session 在锁内接纳、服务端发出新 State、客户端 Handler 接纳，是四个完成点。

因此以下关系必须保留：

```text
control listener onFinished
  ≠ 最终 SurfaceParams 已物理显示
  ≠ runner 已在主线程移除（异步路径尤其如此）
  ≠ WMS 已接纳 clientVisible
  ≠ 新 InsetsState 已回传
  ≠ View 已完成终态 layout/draw
```

`finish(false)` 把 alpha 固定为 1 并不矛盾：普通 Source 通过终点 Insets 为 0 得到 `visible=false`；alpha 只是同一组 surface 参数中的另一个维度。不要把 alpha 0 当作 hide 的必要定义。

## 14. cancel、revoke、frame 冻结与释放要分开审计

取消不是 `finish(startState)`。`CancellationSignal`、重叠新动画、Source 变得不可控、control revoke 都会进入 `cancelAnimation(runner, invokeCallback=true)`。普通 `InsetsAnimationControlImpl.cancel()` 同步标记 cancelled、通知 listener、释放副本，然后 Controller 移除 running 记录并解冻 pending frame；线程 runner 的 `cancel()` 只向动画线程 post 内部 cancel，主线程会先继续移除/解冻，listener 与释放反而可能稍后发生。只有已派发 start 的普通路径才发送 animation end。取消不会合成一帧最终 progress；若发生在 pre-draw 前，应用可能已经收到 prepare，却不会收到 start 或 end。

它不会统一替所有场景恢复“动画开始前 requested visibility”。runner 创建时已经按 layout mode 调过 `showDirectly()` 或 `hideDirectly()`，取消后该意图通常保留。以“取消后回起点”作为不变量会直接读错测试；正确做法是记录创建 runner 前后 Consumer 的 requested 值。

服务端 `ControlAdapter.onAnimationCancelled()` 在自己仍是当前 adapter 时，清掉 Provider 的 control、target 与 adapter，把 client visibility 恢复默认，并让 StateController 从映射中移除旧关系；这个 revoke 方法本身既不把旧 target 加入 pending，也不直接通知客户端。正常 target 切换的外层 `onControlChanged()` 才会把 previous target 加入 pending，随后异步交付更新。直接的窗口 cancel 或 seamless-rotation cancel 因而不保证旧客户端立刻收到撤权消息。

真正到达客户端时，撤权通常表现为该 internal type 从 active-controls 数组缺失；最后一个 type 消失时整个数组还可能是 null，并非服务端一定发送一个值为 null 的 Control 元素。`InsetsController` 对已有 Consumer 主动传 null，才把“缺失”翻译成 `setControl(null)`；Consumer 随后取消相关 runner，并以同包 State 恢复 local visibility。服务端 ownership cancel 与客户端 animation cancel 是相邻但不同的生命周期。

动画期间收到新 Source frame 时，Consumer 不立即替换几何：它保存 `mPendingFrame` / `mPendingVisibleFrame`，把新 Source 的 frame 改回旧值后放进 local State。finish 或 cancel 移除 runner 时，`notifyAnimationFinished()` 才把 pending 几何写回。这样同一动画的 shown/hidden bounds 与每帧基线不会半途跳变。

释放审计至少列三类 handle：

| 持有者 | 正常释放点 | 不应影响谁 |
| --- | --- | --- |
| Consumer 的当前 Control | Control 被替换或撤销 | 新 Control 与正在使用的 runner 副本 |
| 普通 runner 的 Control 副本 | `applyChangeInsets()` 处理 finish，或 `cancel()` | Consumer 原 Control |
| animation-thread runner 的副本 | 线程侧 finish/cancel | 服务端 leash 与 Consumer 原 Control |
| IME delayed 前的候选副本 | r48 无显式释放；失去引用后依赖 GC/finalizer | pending 请求恢复时新建的下一批副本 |

r48 的异步 finish 源码值得专门标记：线程 runner 的内部 `notifyFinished()` 先遍历 controls 直接 release，返回 `InsetsAnimationControlImpl.applyChangeInsets()` 后又执行 `releaseLeashes()`。这是可由控制流直接推出的两次 release 调用与生命周期冗余；两条路径最终都使用 `SurfaceControl.release()`，其 native pointer 清零保护会让第二次成为 no-op，不能据此声称发生了两次 native release 或已经证明会崩溃。

最终可用下面的五列现场表收束：

| 列 | 关键证据 |
| --- | --- |
| authority | real/fake target、Control type、leash 身份、position、ready barrier |
| intent | Consumer requestedVisible、pending IME request、animation type |
| geometry | last-dispatched/local/requested State、current/pending Insets、pending frame |
| scheduling | main/animation/RT 线程、prepare/start/progress/end、listener callbacks |
| lifetime | Consumer handle、runner 副本、revoke/cancel/finish 的 release 次数 |

## 15. 九个只读练习：从 leash 屏障追到终态

下面命令只读取源码。默认源码根目录为 `/Users/ninebot/androidSource`，也可以把另一个源码根目录作为第一个参数传入。每段都兼容 macOS 自带 Bash 3.2 与 Zsh 5.9。

### 练习 1：确认真实 Control 与 fake Control 的 leash 差异

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java"
grep -n 'mFakeControl = new InsetsSourceControl' "$FILE"
grep -n 'null /\* leash \*/' "$FILE"
grep -n 'mWin.startAnimation(t, mAdapter' "$FILE"
grep -n 'mCapturedLeash = animationLeash' "$FILE"
grep -n 'mControl = new InsetsSourceControl' "$FILE"
```

前两行定位无 surface 能力的 fake control，后三行串起真实 leash 的创建、捕获与包装。

### 练习 2：验证 prepare-surfaces 屏障与同包交付

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
PROVIDER="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java"
STATE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java"
WINDOW="$ROOT/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
SESSION_AIDL="$ROOT/frameworks/base/core/java/android/view/IWindowSession.aidl"
WMS="$ROOT/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
ANIMATOR="$ROOT/frameworks/base/services/core/java/com/android/server/wm/WindowAnimator.java"
grep -n 'out InsetsState insetsState, out InsetsSourceControl\[\] activeControls' "$SESSION_AIDL"
grep -n 'getInsetsSourceControls(win, outActiveControls)' "$WMS"
grep -n 'mIsLeashReadyForDispatching = false' "$PROVIDER"
grep -n 'mergeToGlobalTransaction(mTransaction)' "$ANIMATOR"
grep -n 'addAfterPrepareSurfacesRunnable' "$STATE"
grep -n 'provider.onSurfaceTransactionApplied()' "$STATE"
grep -n 'controlTarget.notifyInsetsControlChanged()' "$STATE"
grep -n 'mClient.insetsControlChanged(getInsetsState()' "$WINDOW"
```

前两项证明 add/relayout 有同步返回 Control 的出口；后续命中说明异步真实 leash 交付为什么要跨 transaction 边界。

### 练习 3：核对客户端先 State、后 Control 的顺序

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/ViewRootImpl.java"
grep -n 'case MSG_INSETS_CONTROL_CHANGED' "$FILE"
grep -n 'mInsetsController.onStateChanged((InsetsState) args.arg1)' "$FILE"
grep -n 'mInsetsController.onControlsChanged((InsetsSourceControl\[\]) args.arg2)' "$FILE"
grep -n 'controller can compare with server state' "$FILE"
grep -n 'restore server state by taking last' "$FILE"
```

两条调用的相邻行号与注释共同给出 Consumer 获权、失权时的比较基准。

### 练习 4：追 setControl 的补动画与撤权恢复

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsSourceConsumer.java"
grep -n 'mController.notifyControlRevoked(this)' "$FILE"
grep -n 'getLastDispatchedState().getSourceOrDefaultVisibility' "$FILE"
grep -n 'needAnimation = requestedVisible' "$FILE"
grep -n 'mIsAnimationPending = true' "$FILE"
grep -n 'lastControl.release(SurfaceControl::release)' "$FILE"
```

同一方法同时处理 runner 取消、State 恢复、无 leash 延迟与旧 handle 释放。

### 练习 5：拆开 show/hide、Control 复制与 requested State 回报

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsController.java"
HOST="$ROOT/frameworks/base/core/java/android/view/ViewRootInsetsControllerHost.java"
SESSION="$ROOT/frameworks/base/services/core/java/com/android/server/wm/Session.java"
STATE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java"
grep -n 'animationType == ANIMATION_TYPE_SHOW' "$FILE"
grep -n 'animationType == ANIMATION_TYPE_HIDE' "$FILE"
grep -n 'cancelExistingControllers(types)' "$FILE"
grep -n 'new InsetsSourceControl(control)' "$FILE"
grep -n 'showDirectly(types)' "$FILE"
grep -n 'hideDirectly(types, false' "$FILE"
grep -n 'mHost.onInsetsModified(mRequestedState)' "$FILE"
grep -n 'mWindowSession.insetsModified' "$HOST"
grep -n 'windowState.updateRequestedInsetsState(state)' "$SESSION"
grep -n 'changed |= provider.onInsetsModified' "$STATE"
```

前六项覆盖去重、复制与布局基线；后四项把 requested State 追进 system_server。前两种 animation type 有多处命中，应结合所在方法判断其角色。

### 练习 6：验证 IME pending 请求的多类出口

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
CONTROLLER="$ROOT/frameworks/base/core/java/android/view/InsetsController.java"
CONSUMER="$ROOT/frameworks/base/core/java/android/view/ImeInsetsSourceConsumer.java"
grep -n 'requestImeShow(mController.getHost().getWindowToken())' "$CONSUMER"
grep -n 'mPendingImeControlRequest = request' "$CONTROLLER"
grep -n 'postDelayed(mPendingControlTimeout' "$CONTROLLER"
grep -n 'abortPendingImeControlRequest()' "$CONTROLLER"
grep -n 'if (fromIme && mPendingImeControlRequest != null)' "$CONTROLLER"
```

再结合 CancellationSignal listener、control revoke 与 timeout 三个调用点，区分“等待”与“失败”。

### 练习 7：确认 pre-draw 回调序与 apply 调度分叉

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsController.java"
IMPL="$ROOT/frameworks/base/core/java/android/view/InsetsAnimationControlImpl.java"
grep -n 'mController.startAnimation(this, listener, types' "$IMPL"
grep -n 'dispatchWindowInsetsAnimationPrepare(animation)' "$FILE"
grep -n 'mHost.addOnPreDrawRunnable' "$FILE"
grep -n 'mRunningAnimations.add(new RunningAnimation' "$FILE"
grep -n 'showDirectly(types)' "$FILE"
grep -n 'dispatchWindowInsetsAnimationStart(animation, bounds)' "$FILE"
grep -n 'listener.onReady(controller, types)' "$FILE"
grep -n 'runner.getAnimationType() == ANIMATION_TYPE_USER' "$FILE"
grep -n 'postInsetsAnimationCallback(mAnimCallback)' "$FILE"
```

把 constructor 的 `startAnimation`、Controller 入表和 direct visibility 三处放在一起，才能还原 prepare → 布局基线 → pre-draw start/ready；最后两行区分同步 USER 与帧回调合并。

### 练习 8：手算一帧 bottom leash 位移

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsAnimationControlImpl.java"
grep -n 'Insets.subtract(mShownInsets, mPendingInsets)' "$FILE"
grep -n 'updateLeashesForSide(ISIDE_BOTTOM' "$FILE"
grep -n 'mTmpMatrix.setTranslate(control.getSurfacePosition().x' "$FILE"
grep -n 'm.postTranslate(0, inset)' "$FILE"
grep -n 'state.getSource(source.getType()).setFrame(mTmpFrame)' "$FILE"
grep -n 'withVisibility(visible)' "$FILE"
```

把 shown=120、pending=45 代入，验证 surface matrix 与临时 Source frame 都向下偏移 75。

### 练习 9：区分 finish、cancel 与异步释放

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
IMPL="$ROOT/frameworks/base/core/java/android/view/InsetsAnimationControlImpl.java"
ASYNC="$ROOT/frameworks/base/core/java/android/view/InsetsAnimationThreadControlRunner.java"
CONTROLLER="$ROOT/frameworks/base/core/java/android/view/InsetsController.java"
grep -n 'mListener.onFinished(this)' "$IMPL"
grep -n 'mController.notifyFinished(this, mShownOnFinish)' "$IMPL"
grep -n 'mListener.onCancelled(mReadyDispatched ? this : null)' "$IMPL"
grep -n 'releaseControls(mControl.getControls())' "$ASYNC"
grep -n 'releaseLeashes()' "$IMPL"
grep -n 'cancelAnimation(runner, false' "$CONTROLLER"
```

这些命中把控制监听器、外层 runner 收口、null-controller 取消与两处释放路径分开。

## 16. 用五条时间线收束一次 Insets 动画排查

读这套实现时，最省时间的方法不是从 `ValueAnimator` 开始，而是按五条时间线找第一次分叉：

1. **authority**：谁是 real target，谁是假目标；leash 是 null、旧 handle 还是新 handle；是否越过 after-prepare 屏障。
2. **intent**：requestedVisible 何时改变；请求来自应用还是 IME；是否因无 Control、IME 未 ready 或 user-controllable gate 而延迟。
3. **geometry**：动画端点基于 display frame 还是 window frame；current 与 pending 是否一致；Source 新 frame 是否被冻结。
4. **scheduling**：普通主线程 runner、animation thread、RT transaction 各走哪条；prepare/start/ready/progress/end 与 listener 回调先后如何。
5. **lifetime**：Consumer 原 Control 与 runner 副本分别由谁释放；finish、cancel、revoke 是否经过不同出口。

最后保留十二条精确结论：

1. State 负责布局事实，Control 负责权限，leash 负责 surface 操作；三者不能互相替代。
2. real target 可拿非空 leash，fake target 的 null-leash Control 只保留请求意图。
3. 异步通知中的真实 leash 要等 WMS 越过准备事务边界；同步 add/relayout 在此之前最多得到 null-leash 占位包装。
4. control-change 消息同时携带 State，ViewRoot 必须先交 State、再交 Control。
5. Consumer 获权时以 requested 与 local State 差异决定补动画，失权时以 last-dispatched State 恢复事实。
6. show/hide 先去重；没有 Control 仍可保存意图，不能据此宣称动画已开始。
7. USER 请求可能因窗口 frame、Source user-controllable、无 Control 三道门而以 `onCancelled(null)` 结束。
8. IME show 有 IMM 应答与 2 秒 pending 阶段；`fromIme` 回调只是继续请求，不是物理显示完成。
9. prepare 先于布局基线切换；View 按该基线布局后，普通路径才在 pre-draw 发 start 与 onReady，USER 最终状态仍由 finish 选择。
10. `setInsetsAndAlpha()` 写 pending；apply 才同步推进临时 State、surface 参数与 current getter。
11. finish、cancel 与 revoke 不是同一终态操作；control listener、runner 收口、WMS 接纳与物理 present 也不是同一完成点。
12. r48 的 zero-insets USER visibility、休眠的 EditorInfo 比较、IME delayed 候选副本缺少显式释放，以及异步 runner 的冗余 release 调用都具有版本特异性，结论不可无条件外推到其他 Android 版本。

下一章进入 `InputMethodManagerService`、`InputMethodService` 与软键盘显示/输入/隐藏完整链，重点追踪焦点与 served editor、客户端 show 请求、IME 进程绑定、窗口可见性、输入连接、结果回调和隐藏清理之间的完成点。
