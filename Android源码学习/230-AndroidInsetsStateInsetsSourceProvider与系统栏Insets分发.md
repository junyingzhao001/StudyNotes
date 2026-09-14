# 230 Android InsetsState、InsetsSourceProvider与系统栏Insets分发

本文基于 `android-11.0.0_r48`，追踪状态栏、导航栏、手势区、刘海与 IME 如何从 system_server 中的窗口几何，变成某个应用窗口收到的 `InsetsState`，再由客户端折算为 `WindowInsets` 并进入 View 树。

这一章最重要的不变量是：**提供窗口已经布局、原始 Source 已更新、服务端决定广播、面向某个窗口的 State 已过滤、`IWindow` 调用已经返回、ViewRoot Handler 已处理、客户端 State 已接纳、一次 traversal 已安排、根 View 与子树收到 `WindowInsets`，是不同的完成点。**

版本锚点：

- `frameworks/base/core/java/android/view/InsetsSource.java`
- `frameworks/base/core/java/android/view/InsetsState.java`
- `frameworks/base/core/java/android/view/InsetsController.java`
- `frameworks/base/core/java/android/view/InsetsSourceConsumer.java`
- `frameworks/base/core/java/android/view/ViewRootImpl.java`
- `frameworks/base/core/java/android/view/ViewRootInsetsControllerHost.java`
- `frameworks/base/core/java/android/view/IWindow.aidl`
- `frameworks/base/core/java/android/view/IWindowSession.aidl`
- `frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java`
- `frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java`
- `frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java`
- `frameworks/base/services/core/java/com/android/server/wm/ImeInsetsSourceProvider.java`
- `frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java`
- `frameworks/base/services/core/java/com/android/server/wm/InsetsPolicy.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowState.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`

## 1. 先把一次Insets分发的完成点拆开

设想已注册的状态栏高度在布局中改变。下面是几何 post-layout 变化的一条代表路径，不是所有可见性请求与 transient 更新都必须经过的唯一路线：

```text
DisplayPolicy 早已为 Window 注册一个或多个 internal source
  → 本轮窗口布局
  → InsetsSourceProvider.onPostLayout()
  → 更新 serverVisible、frame、visibleFrame 与最终 visible
  → InsetsStateController.onPostLayout() 比较 raw InsetsState
  → DisplayContent 遍历可见 WindowState
  → InsetsPolicy.getInsetsForDispatch(target) 生成逐窗口视图
  → IWindow.insetsChanged(state)
  → ViewRootImpl 把消息切到客户端 Handler
  → InsetsController.onStateChanged(state)
  → requestLayout / scheduleTraversals
  → calculateInsets() 生成 WindowInsets
  → View.dispatchApplyWindowInsets()
```

同一条链上的完成点必须分开描述：

| 完成点 | 此时可以证明 | 此时仍不能证明 |
| --- | --- | --- |
| Provider 完成 post-layout | 本轮 frame、visibleFrame、serverVisible 已计算 | raw State 一定与上一轮不同 |
| 服务端决定广播 | 本轮 raw 比较变化，或另一入口直接要求通知 | 每个窗口最终收到的副本都相同 |
| 逐窗口 State 构造完 | 目标适用的 fixed-rotation early-return，或普通 provider/windowing/above-IME 分支已完成 | Binder 回调已经执行 |
| 服务端 `IWindow` 调用返回 | 远端 proxy 已提交 oneway；本地 W 回调已把 Handler 消息入队 | 客户端 Handler 已处理 |
| ViewRoot Handler 分支返回 | `InsetsController.onStateChanged()` 已执行 | View 已经重新布局 |
| traversal 已安排 | 将在合适时机重新计算 | 本帧已经提交或显示 |
| `dispatchApplyWindowInsets()` 执行 | 根 View 已开始向子树派发这份值 | 各子 View 一定采用而未消费它 |
| ViewGroup 派发返回 | 本轮兼容规则下的子树传播已结束 | 随后的 layout、draw 或物理 present 已完成 |

Insets 不是“一组系统 padding”。它是一组有类型、有矩形、有可见性、有目标窗口过滤规则的 source；`WindowInsets` 只是这些 source 相对某个窗口 frame 的一次派生结果。

上面的主链以默认 FULL 模式为准。NONE 模式下 `ViewRootImpl.notifyInsetsChanged()` 会直接返回，新 State 回调不会靠这条入口安排 apply-insets traversal；旧布局字段仍承担兼容职责。因此模式本身是第一份现场证据。

## 2. 四层对象与三种矩形先分清

完整链路至少有四层对象：

| 层级 | 代表对象 | 负责什么 |
| --- | --- | --- |
| 提供窗口 | 状态栏、导航栏、IME 的 `WindowState` | 拥有真实窗口 frame、policy 可见性与 surface |
| 单源状态 | `InsetsSource` | 保存 internal type、屏幕坐标 frame、visibleFrame、visible |
| display 聚合 | `InsetsState` | 用固定槽位保存多个 Source，并记录 display frame |
| 客户端派生 | `WindowInsets` | 把多个 internal source 聚合为 public type 的当前值、最大值和可见性 |

`InsetsSource.mFrame` 明确处于 screen coordinate space。传给 `calculateInsets(relativeFrame, ...)` 的 `relativeFrame` 也必须在同一坐标系；算法先求交集，再把交集贴在哪一条边折成 `Insets(left, top, right, bottom)`。结果是相对窗口内容使用的厚度，不再是一块屏幕矩形。

还要区分三个看起来相似的矩形：

- `WindowState.getFrameLw()` 是提供窗口布局后的外框。
- `InsetsSource.frame` 是 Provider 从窗口外框经过 `frameProvider` 或 given content insets 修饰后的占用区。
- `visibleFrame` 是给 legacy visible insets 使用的可见区域；为空引用表示退回 `frame`，空矩形则会让该 Source 被判为不可供用户动画控制。

setter 会复制传入 `Rect`，但 `getFrame()`、`getVisibleFrame()` 和 `InsetsState.getDisplayFrame()` 都直接暴露内部可变对象。这里不是不可变值对象体系；共享引用、浅拷贝和原地修改必须结合调用点审计。

## 3. internal type不是public type的同义词

r48 的 `InsetsState` 有 20 个 internal 槽位，编号从 0 到 19。public `WindowInsets.Type` 则是位掩码 API；一个 public type 可以由多个窗口或多个方向的 Source 合并。

关键映射如下：

| public type 方法 | r48 中对应的 internal source |
| --- | --- |
| `Type.statusBars()` | status bar、climate bar |
| `Type.navigationBars()` | navigation bar、extra navigation bar |
| `Type.captionBar()` | caption bar |
| `Type.ime()` | IME |
| `Type.mandatorySystemGestures()` | top/bottom gestures，加四个 mandatory gesture source |
| `Type.systemGestures()` | left/right gestures；mandatory 结果还会再次并入这里 |
| `Type.tappableElement()` | top/bottom tappable element |
| `Type.displayCutout()` | left/top/right/bottom 四个 cutout source |

最容易读错的是 gesture：在这份源码中，`ITYPE_TOP_GESTURES` 与 `ITYPE_BOTTOM_GESTURES` 直接映射到 `MANDATORY_SYSTEM_GESTURES`，不是普通 `SYSTEM_GESTURES`。`processSource()` 随后又把 mandatory 的几何并入 system gestures，因此 public system gesture 区域是两阶段合并结果。

表里的大写名字是 framework 内部 bit 常量，应用公开入口是这些 `Type.xxx()` 方法。反向映射也不是上表的完全逆函数。`toInternalType(publicMask)` 只展开 status、navigation、caption、cutout 和 IME；它不展开 gesture、tappable，也不处理 package-private 的 `WINDOW_DECOR` bit。该函数主要服务 show/hide/control 请求，不能当作任意 public 类型到全部 internal source 的通用查询器。

默认可见性同样简单但影响很大：只有 IME 默认隐藏，其余 internal type 默认可见。因此 `getSource(type)` 的“读操作”会在缺失时创建一个空 frame 的 Source，并立即把它置为该类型的默认可见性；真正无副作用的查询是 `peekSource(type)`。

## 4. InsetsSource怎样把矩形折成单边厚度

普通 Source 的几何算法可以压缩为五步：

1. `ignoreVisibility` 为 false 且 Source 不可见，直接返回 `Insets.NONE`。
2. caption bar 不先求交，直接把 source frame 高度作为 top inset。
3. 其余类型先计算 source frame 与目标 frame 的交集；没有交集则返回 0。
4. IME 只要有交集，固定把交集高度报告为 bottom inset。
5. 普通类型只有在交集横跨目标全宽时才尝试 top/bottom，或在交集纵跨目标全高时才尝试 left/right；不贴边的内部悬浮矩形返回 0。

例如目标窗口为 `[0,0]-[1080,2400]`：

| Source frame | 类型 | 结果 | 原因 |
| --- | --- | --- | --- |
| `[0,0]-[1080,96]` | status | top=96 | 横跨全宽且贴上边 |
| `[0,2300]-[1080,2400]` | navigation | bottom=100 | 横跨全宽且贴下边 |
| `[0,1700]-[1080,2400]` | IME | bottom=700 | IME 固定走下边规则 |
| `[200,200]-[800,500]` | 普通类型 | 0 | 只在窗口内部相交，不占完整边 |
| `[0,0]-[30,2400]` | left gesture | left=30 | 纵跨全高且贴左边 |

四个边界值得单独记住：

- caption 规则发生在相交判断之前。即使 caption frame 与目标窗口不相交，也可能按其高度给出 top inset；这是拖动与缩放期间的布局补偿。
- `getIntersection()` 使用 `<=`，共边也算相交；交集可以是零宽或零高，最后仍通常折成 `Insets.NONE`。
- 普通 top/bottom 分支还有 `intersection.top == 0` 的兼容分支，用于把某些未贴目标上边、但位于屏幕顶部的 Source 仍解释为 top inset。
- 方法契约要求单个 Source 最终只占一边。若几何既不能唯一落到横向边，也不能落到纵向边，就返回 0，而不是生成四边包围盒。

“共边通常为 0”仍有退化 frame 例外：目标自身宽为 0 时，零宽交集仍可能满足“交集宽等于目标宽”，从而返回非零 top；目标高为 0 时同理可能返回非零 left。这里比较的是宽高相等，不是先要求目标面积为正。

`calculateVisibleInsets()` 只是优先用 `visibleFrame` 替代 `frame`，仍然尊重 `visible`。它也不会在 Source 内强制把 visibleFrame 裁进 frame；Provider 的常规构造会从同一窗口几何得到二者，但对象 API 本身允许 visibleFrame 超出 frame。所以“visibleFrame 非空”不等于“Source 可见”，“visibleFrame 为 null”也不等于“没有 visible insets”。

## 5. InsetsState聚合时几何取max，可见性却是覆盖

`calculateInsets()` 为 public type 建立 current、max 和 visibility 三组结果，然后按 internal type 从 0 到 19 遍历。单个 Source 先经上一节折成一边厚度，再映射到 public type。

同一 public type 有多个 Source 时，几何通过 `Insets.max(existing, insets)` 逐边取最大值，不做相加。例如顶部 80 的 status bar 与左侧 40 的 climate bar 都映射到 status bars，public 结果可以是 `(40,80,0,0)`；两个顶部来源分别为 80 与 30 时仍是 top=80，而不是 110。

可见性没有做 OR/AND 聚合。`typeVisibilityMap[index] = source.isVisible()` 是顺序覆盖，最后处理到的同 public type Source 获胜。由槽位顺序可得：climate bar 可以覆盖 status-bar public visibility，extra navigation bar 可以覆盖 navigation-bar public visibility。gesture 的三级顺序更微妙：top/bottom gestures（槽 3/4）先同时写 mandatory 与 system visibility，left/right gestures（槽 5/6）随后只覆盖 system，四个显式 mandatory source（槽 7—10）最后又同时覆盖 mandatory 与 system。几何与 visibility 因而可能来自不同的 internal source，排查时必须同时打印类型、槽位与遍历顺序。

`WindowInsets.isVisible(typeMask)` 对调用者给出的多个 public type 采用“全部为 true”语义；任意一个为 false，整体就是 false。visibility 只读聚合表，不检查 Source 是否与目标 frame 相交，所以 Source 可见而本窗口计算几何为 0 是合法组合。反过来，缺失的 internal Source 不参与 visibility 写入；只有没有任何映射到该 public type 的现存 Source 写过值时，槽才保持默认 false。`getSourceOrDefaultVisibility()` 的默认逻辑主要服务请求状态。

实现还有两个容易被数组名字遮住的事实：

- current 与 max 数组长度是 `WindowInsets.Type.SIZE`，这份源码为 9；visibility 数组却按 internal `InsetsState.SIZE` 分配，长度为 20。实际写入仍使用 public `indexOf()`。
- 缺失 Source 的分支只为 current map 补 `Insets.NONE`，不为 max map 补值；`WindowInsets` 构造端负责解释这些槽位。

max insets 对每个“未被模式跳过且非 IME”的 Source 以 `ignoreVisibility=true` 计算。IME 被明确排除，因为它的高度依赖当前 editor/target，不能承诺稳定最大值；调用 `WindowInsets.getInsetsIgnoringVisibility(mask)` 时，只要 mask 含 `Type.ime()`，API 会直接抛出 `IllegalArgumentException`，而不是返回 0 或一个猜测值。

`ignoreVisibility=true` 只绕过 visible 位，不保留历史几何。Provider 因 server 不可见而已把 frame 清空时，同一 State 的 max 仍是 0；只有传入另一个保留几何的 `ignoringVisibilityState` 才可能得到非零值。此时代码调用辅助 State 的 `getSource(type)`；缺少槽位会现场创建默认 Source，后面的 null 判断实际上不可达。这既是读结果，也是一次可观察的对象修改。

## 6. 三种新Insets模式与legacy兼容不是一把总开关

`persist.debug.new_insets` 在 `ViewRootImpl` 中对应三种模式：0 为 NONE，1 为只启用 IME 新链，2 为 FULL，默认值是 FULL。它同时影响计算、控制资格与 ViewRoot 是否因新 State 触发布局。

`InsetsState.calculateInsets()` 的跳过规则不是简单的“旧模式全跳过”：

| 当前模式 | 本轮最终跳过的 Source |
| --- | --- |
| FULL | 不因这组三态条件跳过 Source |
| IME | 所有非 IME source，只保留 IME |
| NONE | canonical status bar、canonical navigation bar、IME |

被跳过的 Source 仍会写 public visibility，但不写 current/max 几何。注意 `skipSystemBars` 只点名 canonical status/navigation；climate 与 extra-navigation 并未被该条件覆盖。因而不能把旧模式概括为“所有状态栏/导航栏同族 Source 都忽略”。

返回 `WindowInsets` 前还会构造 compat types：初始包含 `Type.systemBars()` 与 display cutout，其中 system bars 在 r48 是 status、navigation、caption 三者，不含 IME；soft-input adjust 为 `ADJUST_RESIZE` 时加入 IME；窗口带 `FLAG_FULLSCREEN` 时移除 status bars。这只决定已废弃的 `getSystemWindowInsets()` 等兼容结果，不会删除新 API `getInsets(Type.ime())`、`getInsets(Type.statusBars())` 的数据或 visibility。

FULL 模式且 legacy system-ui flags 含 `LAYOUT_STABLE` 时，`compatIgnoreVisibility` 为 true：legacy system insets 对非 IME 类型使用 ignoring-visibility/max 值，对 IME 仍使用当前值。它也不会改变新类型 API。因而“布局稳定”“当前栏隐藏”和“public visibility”为不同输入，不能由一个旧式 Rect 反推出全部状态。

`calculateVisibleInsets()` 另走一条 legacy 路径：非 FULL 模式只处理 IME；FULL 模式下只接受 system bars，以及 soft-input adjust 不是 `ADJUST_NOTHING` 时的 IME。gesture、tappable 与 cutout 不参与。它逐边取 max，并对每个 Source 尊重 visible、优先使用 visibleFrame。这里 IME 在 PAN/UNSPECIFIED 下可以进入 visibleInsets，而 deprecated system-window insets 只有 `ADJUST_RESIZE` 才把 IME 纳入 compat types。current insets、visible insets 与 ignoring-visibility max insets不是同一个概念，现场日志不能混为一列。

## 7. DisplayPolicy把一个系统窗口注册成多个Source

window-backed Source 的 Provider 创建入口是 `DisplayContent.setInsetProvider()`，最终由 `InsetsStateController.getSourceProvider(type)` 按 internal type 懒创建。IME 会得到专门的 `ImeInsetsSourceProvider`，其他经此入口的类型使用普通 `InsetsSourceProvider`。不能反推“每个 Source 都有 Provider”：cutout 由 Policy 直接写 raw State，caption 又主要由客户端组装。

状态栏 Window 加入时，一次注册三个 Source：

```text
TYPE_STATUS_BAR Window
├─ ITYPE_STATUS_BAR
├─ ITYPE_TOP_GESTURES
└─ ITYPE_TOP_TAPPABLE_ELEMENT
```

三者可以共享同一个 frame-provider：把 `rect.top` 固定为 0，把 `rect.bottom` 改为 policy 计算的状态栏高度。它们共享提供窗口，却拥有独立的 Source、可见性与 public 映射。

导航栏 Window 注册的 Source 更多：canonical navigation、bottom gestures、left gestures、right gestures、bottom tappable element。各自的 frame-provider 可以重写同一个窗口 frame：

- 手势导航且栏位于底部时，canonical navigation source 会缩到真正参与 layout 的高度。
- bottom gesture source 在窗口 frame 基础上再向上扩展额外手势区。
- left/right gesture source 直接构造贯穿显示高度的侧边条带。
- navigation window 不可触摸或允许触摸穿透时，bottom tappable source 被清为空矩形。

canonical navigation Provider 还保存一个仅向 IME 分发的 override frame。原因是手势导航下，普通应用用于布局的 nav frame 可以较小，而 IME 看到的 nav frame 需要保持常规窗口大小，避免键盘出现时客户端内容与导航栏重叠。

带 `providesInsetsTypes` 的替代系统栏必须通过 `STATUS_BAR_SERVICE` 权限检查；同类 singleton 也会被拒绝重复添加。权限只约束谁能声明提供者，不替代后续的布局、可见性与逐窗口过滤。

Display cutout 不依赖一个可见 Window。`DisplayPolicy.updateInsetsStateForDisplayCutout()` 以 unrestricted frame 与 safe frame 的差值构造四个方向 Source；cutout 为空时则移除四个槽位。

系统栏 Window 移除时也不是把其全部辅助 Provider 同步摘除。`removeWindowLw()` 显式清的是 canonical status/navigation 绑定；gesture/tappable Provider 可暂时仍指向旧 Window，随后因旧窗口不再具备显示条件而产出不可见、空 frame，新的系统栏加入时再重新绑定。不要把“一窗多源”的建立与销毁想象成一个原子数组替换。

## 8. Provider把frame、serverVisible与clientVisible合成事实

`InsetsSourceProvider.setWindow()` 负责把 Source 归属切到某个 `WindowState`。替换旧 Window 时，它会解除 controllable-provider 关系并取消旧动画；传入 null 时把 server visibility 设为 false、frame 清空、visibleFrame 设为 null。若 Source 可控制且新 Window 已存在，Provider 还会处理此前因“尚无 Window”而暂存的 control target。

`WindowState.computeFrame(displayFrames)` 先调用 `computeFrameLw()` 得到 `mWindowFrames.mFrame`，随后只会立即刷新它关联的单个 controllable Provider；一个系统栏的 gesture/tappable 辅助 Source 仍要等 `InsetsStateController.onPostLayout()` 遍历全部 Provider 才形成权威批次。因此“主 Source 已刷新”不代表同窗辅助 Source 已全部刷新。

每轮 `onPostLayout()` 先计算：

```text
serverVisible = wouldBeVisibleIfPolicyIgnored()
             && isVisibleByPolicy()
             && !mGivenInsetsPending
```

只有 `serverVisible` 为 true 才从 `WindowState.getFrameLw()` 产生有效 source frame；否则 frame 被清空。若有自定义 `frameProvider`，它原地改写临时 Rect；没有时则用 `mGivenContentInsets` 内缩。非零 `mGivenVisibleInsets` 会生成 visibleFrame，全零则回退为 null。

最终 Source 可见性通常是：

```text
source.visible = serverVisible && clientVisible
```

`serverVisible` 回答“WMS 是否认为提供窗口具备显示条件”，`clientVisible` 回答“当前控制方希望该 Source 是否可见”。二者任何一个为 false，普通 Source 都不可见。声明 `providesInsetsTypes` 且其中包含 IME 的 mirrored source 是例外：只要 server 可见，就绕过 clientVisible。这是源码中的定向兼容逻辑，不应泛化到所有多源窗口。

clientVisible 初值来自默认可见性，所以 IME 初始 false，其他类型初始 true。clientVisible 变化会请求 `LAYOUT_AND_ASSIGN_WINDOW_LAYERS_IF_NEEDED`，因为 Source visibility 不只是 surface alpha，还会改变别的窗口布局输入。

IME 的“请求显示”还要过一个 post-layout 门：`mIsImeLayoutDrawn` 已锁存时可直接继续；否则要求 IME 请求目标与 DisplayContent 认定的目标匹配、IME Window 已 drawn，且 `mGivenInsetsPending` 为 false。穿过门后 `ImeInsetsSourceProvider` 才调用 control target 的 `showInsets(Type.ime(), true)`。这个调用仍只是发起客户端显示链，不是键盘动画或呈现完成。

“可控制”也由模式和类型共同决定：FULL 模式下 status/navigation/climate/extra-navigation 可控制；IME 在 IME 或 FULL 模式下可控制；手势区、tappable、cutout 等普通 Provider 不可控制。是否有 Provider、Source 是否可见、是否可控制、当前是否已有 control target，是四个独立状态。

## 9. control target、请求可见性与真实State要分开

`InsetsStateController` 同时维护 raw State、Provider 表、real control target 映射、fake target 映射和待通知 control-target 集合。它不是简单的 `InsetsState` 容器。

当控制权交给目标时，Provider 会启动一次 `ANIMATION_TYPE_INSETS_CONTROL` 动画以取得 leash。新 leash 创建后并不会立即下发：对应 Surface transaction 尚未应用时，客户端更早操作 leash 可能被服务端事务覆盖。此时 `getControl()` 返回同类型与位置、但 leash 为 null 的 control。

目标也不总是当前焦点 Window。Provider 会把带 Window 的候选目标经 `getImeControlTarget()` 归一化到 IME host 或 fallback；没有 IME target 时，StateController 使用 empty IME target 持有隐藏 leash，并安排移除遗留 IME surface。这里的“empty”是系统兜底 control target，不是没有控制链。

`notifyPendingInsetsControlChanged()` 把通知放到 `addAfterPrepareSurfacesRunnable()`。该回调先对所有 Provider 调用 `onSurfaceTransactionApplied()`，再执行目标的 `notifyInsetsControlChanged()`。这个屏障证明“准备 leash 的事务已进入正确顺序”，不证明动画完成或画面已经 present。

客户端 show/hide 改的是本地 SourceConsumer 的 requested visibility。`InsetsController.updateRequestedState()` 只把当前拥有 control 的 Source 写入 `mRequestedState`，跳过客户端自行组装的 caption，再通过 `IWindowSession.insetsModified()` 回到 system_server：

```text
InsetsController.mRequestedState
  → ViewRootInsetsControllerHost.onInsetsModified()
  → IWindowSession.insetsModified()
  → Session.insetsModified()
  → WindowState.updateRequestedInsetsState()
  → InsetsPolicy.onInsetsModified()
  → InsetsStateController.onInsetsModified()
  → 对匹配 Provider 更新 clientVisible
```

服务端 Provider 只接受当前 real control target 的修改；非控制方提交的 Source 不会改变它的 clientVisible。即使调用者是真实控制方，`onInsetsModified()` 也只读取对应 Source 的 `isVisible()`，不会接受客户端提交的 frame 或 visibleFrame。`WindowState.updateRequestedInsetsState()` 只 add/replace 请求 State 中实际存在的 Source，既不会凭空补齐本次缺少的类型，也不会删除此前留下的槽位；最终仍由 Provider 的当前 real-target 检查挡住失效项。

详细的 leash 生命周期、show/hide 动画与 `InsetsSourceControl` 协议留到第 231 章；本章只需建立边界：State 是“看到什么”，requested State 是“希望什么”，control 是“能否驱动对应 surface”。三者可以暂时不一致。

## 10. raw State只在post-layout后比较，没变也可能通知

`InsetsStateController.onPostLayout()` 先把 display bounds 写入 raw State，再逐个调用 Provider 的 `onPostLayout()`。完成后用 `mLastState.equals(mState)` 判断全局事实是否改变；变化时深拷贝到 `mLastState` 并广播。

但 raw compare 不是每次 dispatch 的必经门。客户端请求经 `onInsetsModified()` 改变 Provider clientVisible 后，StateController 会直接 `notifyInsetsChanged()`；transient 收尾也可直接通知。`mLastState` 要到下一次 `onPostLayout()` 才追上 raw State，因此随后还可能由比较路径再广播一次。排查重复回调时，应同时找直接通知入口与 post-layout 比较入口。

Controller 与 Provider 本身没有另建互斥锁；这些布局更新、Session 回传与 Animator 帧通常由 WMS global lock 串行。在 `onPostLayout()` 的 global-difference 分支中，`mLastState` 会在逐窗口 oneway 回调前更新；某个 client 抛 `RemoteException` 不会回滚全局状态，也没有为该次广播建立自动重试或客户端确认。因此准确说法是“服务端已更新并尝试投递”，不是“所有客户端已接纳”。

深拷贝很关键。`new InsetsState(other)` 与 `set(other)` 默认只复制 Source 引用；若拿浅副本后原地修改某个 `InsetsSource`，原对象也会被改。`set(other, true)` 或 `new InsetsState(other, true)` 才逐个调用 Source 拷贝构造。

源码在逐窗口修饰中遵守一条 copy-on-write 纪律：

- 只 `removeSource()` 时，复制槽位数组即可，因为没有修改共享 Source 对象。
- 要改 IME frame、IME visibility 或 transient visibility 时，先复制具体 `InsetsSource`，再用 `addSource()` 替换当前 State 的槽位。
- `mLastState` 必须深拷贝，否则下一轮 Provider 原地更新 raw Source 后，last 与 current 会一起变化，差异检测失效。

全局 State 相等也不代表无需任何通知。`DisplayContent.mWinInsetsChanged` 保存那些自身条件改变、从而可能得到不同 dispatch State 的窗口，例如 z-order 影响“是否位于 IME 上方”。raw State 没变时，controller 仍单独通知这批窗口，之后清空列表。

`InsetsState.equals()` 还提供两个客户端专用忽略项：可以忽略 caption，因为 caption Source 在客户端组装；也可以在 IME 不可见时忽略其 frame，避免不可见键盘几何抖动触发无意义布局。普通 raw State 比较不启用这两个忽略项。

## 11. 发给每个Window前要经过一套过滤函数

`InsetsStateController.getInsetsForDispatch(target)` 不是返回 raw State 的 getter。它先检查目标 token 是否有 fixed-rotation InsetsState；有则优先使用旋转后的副本。否则根据目标是不是 Source 提供者、windowing mode、always-on-top 与 IME 层级构造个性化视图。

过滤规则按源码顺序发生：

1. 目标若关联一个 controllable Provider，只移除这个可控主 Source；同一 Window 生产的 gesture/tappable 辅助 Source 不会因此全部移除。
2. navigation/extra-navigation 提供者还移除 IME、status、climate、caption。
3. status/climate 提供者移除 caption。
4. 目标是 IME 时，具有 `imeFrameProvider` 的其他 Source 被复制并替换为 IME 专用 frame。
5. floating window，或 multi-window 且 always-on-top，移除 canonical status 与 navigation。
6. 位于 IME 上方的目标若看到可见 IME，则复制该 Source，把 visible 设为 false 并把 frame 清零。

这里再次出现“canonical 与同族替代 Source 不完全对称”：浮动窗口分支只移除 `ITYPE_STATUS_BAR` 与 `ITYPE_NAVIGATION_BAR`，没有顺手移除 climate/extra-navigation。现场若只看 public `statusBars()` 或 `navigationBars()`，很容易把残留贡献误判为过滤失败。

`isAboveIme()` 对 `WindowState` 使用 `needsRelativeLayeringToIme() || !mBehindIme`；它不是拿两个 frame 做几何比较。IME Source 的隐藏因此是层级语义，而非“矩形没有相交”的副产品。

fixed-rotation State 被优先选中以后，`DisplayContent.notifyInsetsChanged()` 还会把 raw State 中现存 Source 的最新 visibility 同步进旋转副本；这里只同步可见性，不重算旋转几何。它解决的是启动固定旋转期间可见性不能冻结在旧值的问题。

## 12. InsetsPolicy为transient bar构造“看不见但可动画”的视图

`WindowState.getInsetsState()` 最终还要经过 `InsetsPolicy.getInsetsForDispatch()`。当某个 bar 正以 transient 方式显示时，Policy 检查逐窗口 State 中对应 Source；若它真实可见，就复制 State 与具体 Source，再把分发给应用的 visibility 改为 false。

这看似矛盾，却是在区分两种事实：系统 surface 可以临时出现在屏幕上，但应用布局仍应把 bar 当作隐藏，避免一次边缘滑动让内容区突然收缩。于是 raw Source、屏幕上的 transient surface、应用收到的 Source visibility 可以同时是“真、显示、假”。

真实 control target 可能被切到 `mDummyControlTarget` 来执行 transient 动画；原 focused window 同时成为 fake control target。fake control 没有 leash，但保留应用 show/hide 意图的观测通道：若 fake target 在 transient 期间请求把 bar 显示，Policy 会中止 transient 状态，而不是把这次请求丢掉。

因此四种身份不能混写：

| 身份 | 主要用途 |
| --- | --- |
| State recipient | 接收逐窗口几何与可见性 |
| real control target | 获得可操作 leash，可修改 Provider clientVisible |
| fake control target | 没有真实 leash，但让 Policy 观察原应用意图 |
| dummy target | 系统临时接管 leash，驱动 transient 动画或复位 |

控制权变化会单独走 `insetsControlChanged(state, controls)`；普通几何/可见性变化走 `insetsChanged(state)`。两类回调都带 State，但后者没有 control 数组，不能仅凭收到新 State 推断应用获得了控制权。

## 13. 服务端有同步返回和异步回调两条交付通道

窗口首次 `addToDisplay` 与以后 `relayout` 都通过 out 参数同步返回 `InsetsState` 和 active controls。WMS 调用 `outInsetsState.set(win.getInsetsState(), win.isClientLocal())`：同进程 client 需要深拷贝，跨进程则由 Parcel 提供对象隔离。controls 在离开 WM 锁前也会另建 `InsetsSourceControl`，避免原 leash 引用随后被释放。

运行中的变化通过 oneway `IWindow` 回调：

- `insetsChanged(InsetsState)` 只交付 State。
- `insetsControlChanged(InsetsState, InsetsSourceControl[])` 同时交付 State 与控制数组。
- `showInsets(types, fromIme)`、`hideInsets(types, fromIme)` 是 Policy/IME 发给客户端控制器的动作请求。

`DisplayContent.notifyInsetsChanged()` 从顶到下遍历所有窗口，但 `mDispatchInsetsChanged` 只对 `w.isVisible()` 的窗口调用 `notifyInsetsChanged()`。不可见 Window 不会因这次广播立即收到异步 State；它以后可在 add/relayout 或其他状态转换中重新同步。

异步 `insetsChanged` 与 `resized` 也不是一个原子状态包。在同一次 WMS surface-placement 中，Provider post-layout 与 Insets 回调先发生，surface transaction 关闭后才可能遍历 resizing windows 发送 resize；客户端分别排入 `MSG_INSETS_CHANGED` 与 `MSG_RESIZED`。因此新 State 可以暂时配着旧 window frame。非 NONE 模式下，`InsetsController.onFrameChanged()` 会再次请求 apply，最终依靠客户端主线程消息与 traversal 收敛；NONE 模式则由 resize 消息自身的 requestLayout 与 legacy 路径推进。同步 add/relayout 的返回路径先 `setFrame()`，再处理 out State 和 controls。

同进程 Binder 调用不会自动得到 Parcel 深拷贝，所以 `ViewRootImpl.dispatchInsetsChanged()` 与 `dispatchInsetsControlChanged()` 检查 calling pid；若服务端和客户端在同一进程，就显式深拷贝 State，control 回调还逐个复制 control。之后无论来自哪个进程，都只把消息投递到 ViewRoot Handler。

`MSG_INSETS_CONTROL_CHANGED` 的 Handler 处理顺序固定为先 `onStateChanged()`，再 `onControlsChanged()`。理由是获得控制时需要拿最新 server State 判断是否启动动画；失去控制时则要先把最近下发的 server State 恢复为当前依据。只有这个客户端 Handler 分支返回，才能证明 controller 已处理两个输入；服务端 oneway 调用返回并不能证明这一点，前者也仍不说明 surface 动画已经结束。

逐窗口过滤是布局语义隔离，不是内容保密机制。`InsetsState` 携带类型、矩形与可见性等元数据，不含窗口像素或输入文本；真正的画面保护仍属于 secure layer 等机制。反向请求还受三道边界约束：Session 用所属 `IWindow` 解析调用方自己的 `WindowState`，Provider 只接纳当前 real control target，并且只采用 visibility；能用 `providesInsetsTypes` 声明系统栏提供者的调用方另受 `STATUS_BAR_SERVICE` 权限保护。

## 14. 客户端三份State最终在traversal里变成WindowInsets

`InsetsController` 的三份 State 各有职责：

| 字段 | 含义 |
| --- | --- |
| `mLastDispatchedState` | 服务端最近一次原样下发的 State 深副本 |
| `mState` | 服务端 State 经各 SourceConsumer 与本地 visibility override 后的当前有效状态 |
| `mRequestedState` | 由受控 Source 增量填充、可能保留失控类型旧条目的请求账本 |

`onStateChanged()` 先计算“忽略 caption、不忽略 invisible IME frame”的有效状态差异，并单独检查本地 caption 是否未变。只有这些都无变化且 `mLastDispatchedState` 也与新输入相等，才直接返回；即使有效状态不变，只要 server 原始输入不同，仍会深拷到 last-dispatched。随后 `updateState()` 逐个更新/创建 SourceConsumer，并移除服务端已不存在的 Source；若客户端 DecorView 有 caption 高度，它还会在 `mState` 中组装 caption Source。

应用正在控制某类 Insets 时，本地 requested visibility 可以覆盖刚收到的服务端 visibility；所以 `mState` 与 `mLastDispatchedState` 暂时不同并不必然是错误。若最终有效 State 变化，Controller 会调用 Host；在非 NONE 模式中，`ViewRootImpl.notifyInsetsChanged()` 才会标记 `mApplyInsetsRequested`、requestLayout，并在不处于 traversal 时安排新的 traversal。NONE 模式在 Host 入口直接返回。

真正构造 public 对象发生在 `ViewRootImpl.getWindowInsets(true)`：它把当前 window frame、round/cutout、softInputMode、window flags 与 system-ui flags 一起交给 `InsetsController.calculateInsets()`。同一处还计算 legacy visibleInsets，并把 public system/stable insets 回填到 `AttachInfo`。

`dispatchApplyInsets()` 可根据窗口 cutout 策略先 consume `DisplayCutout` 对象，再调用根 View 的 `dispatchApplyWindowInsets()`。这不会清掉 current/max/visibility map，所以 `getDisplayCutout()` 可以变为 null，而 `getInsets(Type.displayCutout())` 仍保留四个 cutout Source 的聚合值。

View 树传播还有 target-SDK 兼容分叉。`mode != FULL` 或应用 `targetSdk < R` 时，前一个 child 返回的 consumed 结果会串给下一个 child，已全部消费时可以提前停止；FULL 且 targetSdk 至少为 R 时，各 child 独立收到同一份输入，一个兄弟消费不会截断其他兄弟。因此调试“内容为什么下移”至少要保留四份证据：服务端 raw State、目标窗口 dispatch State、客户端 `mState`、根 View 实收 `WindowInsets`；只截其中一层会把过滤、本地覆盖或子树消费误认为上游计算错误。

对一次普通 State 更新，较准确的终点表述是：

```text
IWindow 回调已发出
  ≠ ViewRoot Handler 已处理
  ≠ InsetsController 已接纳为有效变化
  ≠ traversal 已执行
  ≠ 根 View 已收到
  ≠ 子 View 最终采用
  ≠ 对应 surface 已显示在物理屏幕
```

## 15. 九个只读练习：从类型映射追到View派发

下面命令只读取源码。默认源码根目录为 `/Users/ninebot/androidSource`，也可以把另一个源码根目录作为第一个参数传入。每段都兼容 macOS 自带 Bash 3.2 与 Zsh 5.9。

### 练习 1：确认20个internal槽位与public映射

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsState.java"
grep -n 'ITYPE_EXTRA_NAVIGATION_BAR = 19' "$FILE"
grep -n 'public static final int SIZE = LAST_TYPE + 1' "$FILE"
grep -n 'case ITYPE_TOP_GESTURES:' "$FILE"
grep -n 'return Type.MANDATORY_SYSTEM_GESTURES' "$FILE"
grep -n 'return Type.SYSTEM_GESTURES' "$FILE"
```

先验证 top gestures 的直接映射，再看 mandatory 如何额外并入 system gestures。不要从名字猜 public type。

### 练习 2：核对矩形折边的特殊次序

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsSource.java"
grep -n 'getType() == ITYPE_CAPTION_BAR' "$FILE"
grep -n 'getIntersection(frame, relativeFrame' "$FILE"
grep -n 'getType() == ITYPE_IME' "$FILE"
grep -n 'mTmpFrame.width() == relativeFrame.width()' "$FILE"
grep -n 'mTmpFrame.top == 0' "$FILE"
```

输出行号应显示 caption 在相交之前、IME 在相交之后、普通边判断最后发生。

### 练习 3：验证聚合是max而visibility是覆盖

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/core/java/android/view/InsetsState.java"
grep -n 'Insets.max(existing, insets)' "$FILE"
grep -n 'typeVisibilityMap\[index\] = source.isVisible()' "$FILE"
grep -n 'new boolean\[SIZE\]' "$FILE"
grep -n 'source.getType() != ITYPE_IME' "$FILE"
grep -n 'ignoringVisibilityState.getSource(type)' "$FILE"
```

把五行放在一起读：几何、visibility、数组尺寸、IME max 例外和辅助 State 的潜在补槽都能一次定位。

### 练习 4：枚举状态栏与导航栏提供的Source

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java"
grep -n 'setInsetProvider(ITYPE_STATUS_BAR' "$FILE"
grep -n 'setInsetProvider(ITYPE_TOP_GESTURES' "$FILE"
grep -n 'setInsetProvider(ITYPE_TOP_TAPPABLE_ELEMENT' "$FILE"
grep -n 'setInsetProvider(ITYPE_NAVIGATION_BAR' "$FILE"
grep -n 'setInsetProvider(ITYPE_BOTTOM_GESTURES' "$FILE"
grep -n 'setInsetProvider(ITYPE_LEFT_GESTURES' "$FILE"
grep -n 'setInsetProvider(ITYPE_RIGHT_GESTURES' "$FILE"
```

同一个 `WindowState` 出现在多次注册里，正是“一窗多 Source”的直接证据。

### 练习 5：追Provider的frame与可见性合成

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java"
grep -n 'wouldBeVisibleIfPolicyIgnored()' "$FILE"
grep -n '!mWin.mGivenInsetsPending' "$FILE"
grep -n 'mFrameProvider.accept' "$FILE"
grep -n 'mTmpRect.inset(mWin.mGivenContentInsets)' "$FILE"
grep -n 'mServerVisible && (isMirroredSource() || mClientVisible)' "$FILE"
```

前两行决定 serverVisible，接着两行展示 frame 的二选一计算，最后一行才是 Source 的最终 visible。

### 练习 6：定位逐窗口过滤的六类分支

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
FILE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java"
grep -n 'getFixedRotationTransformInsetsState' "$FILE"
grep -n 'state.removeSource(type)' "$FILE"
grep -n 'otherProvider.overridesImeFrame()' "$FILE"
grep -n 'WindowConfiguration.isFloating(windowingMode)' "$FILE"
grep -n 'if (aboveIme)' "$FILE"
grep -n 'imeSource.setFrame(0, 0, 0, 0)' "$FILE"
```

这些位置共同说明 dispatch State 是按目标生成的视图，不是 raw State 的无条件广播。

### 练习 7：验证transient与fake target的分工

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
POLICY="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsPolicy.java"
STATE="$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java"
grep -n 'source.setVisible(false)' "$POLICY"
grep -n 'getFakeControlTarget' "$POLICY"
grep -n 'mDummyControlTarget' "$POLICY"
grep -n 'onControlFakeTargetChanged' "$STATE"
grep -n 'new InsetsSourceControl(source.getType(), null' "$ROOT/frameworks/base/services/core/java/com/android/server/wm/InsetsSourceProvider.java"
```

最后一行确认 fake control 没有 leash；它传递的是意图通道，不是 surface 操作能力。

### 练习 8：串起服务端到ViewRoot的异步回调

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
WINDOW="$ROOT/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
VIEWROOT="$ROOT/frameworks/base/core/java/android/view/ViewRootImpl.java"
AIDL="$ROOT/frameworks/base/core/java/android/view/IWindow.aidl"
grep -n 'void insetsChanged' "$AIDL"
grep -n 'mClient.insetsChanged(getInsetsState())' "$WINDOW"
grep -n 'MSG_INSETS_CHANGED' "$VIEWROOT"
grep -n 'mInsetsController.onStateChanged' "$VIEWROOT"
grep -n 'dispatchApplyWindowInsets(insets)' "$VIEWROOT"
```

这条练习刻意保留 Binder callback、Handler message、controller 接纳和 View 派发四个节点。

### 练习 9：确认客户端三份State与反向请求链

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
CONTROLLER="$ROOT/frameworks/base/core/java/android/view/InsetsController.java"
HOST="$ROOT/frameworks/base/core/java/android/view/ViewRootInsetsControllerHost.java"
SESSION="$ROOT/frameworks/base/services/core/java/com/android/server/wm/Session.java"
grep -n 'mState = new InsetsState()' "$CONTROLLER"
grep -n 'mLastDispatchedState = new InsetsState()' "$CONTROLLER"
grep -n 'mRequestedState = new InsetsState()' "$CONTROLLER"
grep -n 'mHost.onInsetsModified(mRequestedState)' "$CONTROLLER"
grep -n 'mWindowSession.insetsModified' "$HOST"
grep -n 'windowState.updateRequestedInsetsState(state)' "$SESSION"
```

若只看 `mState`，就会漏掉“服务端最后说了什么”和“客户端准备请求什么”这两个比较基准。

## 16. 用四份State和分层完成点收束排查

读这条链时，可以先画四列：

| 证据列 | 首选观察点 | 典型问题 |
| --- | --- | --- |
| raw | `InsetsStateController.getRawInsetsState()` / dump | 提供窗口 frame、server/client visible 是否正确 |
| dispatch | `WindowState.getInsetsState()` | fixed rotation、provider 自排除、floating、above-IME、transient 是否改变目标视图 |
| client | `InsetsController.mLastDispatchedState` 与 `mState` | consumer、本地 requested visibility、caption 是否造成差异 |
| View | `dispatchApplyInsets()` 的最终 `WindowInsets` | cutout consume、View 传播与消费是否改变结果 |

最后保留十条精确结论：

1. Source frame 是屏幕坐标矩形，WindowInsets 是它相对目标 frame 折出的单边厚度。
2. 20 个 internal type 会多对一映射到 public type；top/bottom gestures 在 r48 直接属于 mandatory gestures。
3. 同 public type 的几何逐边取 max，不相加；visibility 按遍历顺序覆盖，不做集合逻辑。
4. caption 在求交前报告顶部高度，IME 在求交后固定报告底部高度，普通悬浮交集通常为 0。
5. `getSource()` 会补建槽位；`peekSource()` 才是无副作用查询；默认只有 IME 不可见。
6. Provider 最终 visibility 通常由 serverVisible 与 clientVisible 相与；只要 backing Window 的 `providesInsetsTypes` 含 IME，它背后的 Source 都会走 mirrored 例外。
7. 逐窗口结果不保证内容相同；无需修饰的普通窗口在 system_server 内甚至可沿用同一个 raw State，命中过滤时才 copy-on-write，之后再由 Parcel 或同进程显式深拷贝隔离。
8. State recipient、real control target、fake target 与 dummy target 是四个角色；收到 State 不等于拿到 leash。
9. `IWindow` 调用与 ViewRoot Handler 处理是两个完成点；非 NONE 模式还要安排 traversal、计算 WindowInsets 并向 View 树派发。
10. “Insets 已更新”必须带层级和完成点，否则无法区分上游生产错误、逐窗口过滤、本地覆盖或 View 消费。

建议的现场顺序是：先确认源码模式与 internal 槽位，再看 Provider 的窗口、frame、server/client visibility；随后对比 raw 与目标 dispatch State；最后进入客户端同时检查 last-dispatched、current、requested 和根 View 实收值。这样可以在第一次出现分叉的层级停下，而不是在应用 padding 处反推整个 WMS。

下一章进入 `InsetsSourceControl`、show/hide 与 `WindowInsetsAnimation` 控制链，重点拆开 control 获取、leash 可用、请求可见性回传、每帧 surface 参数、动画 finish/cancel 和服务端最终 State 收敛这些完成点。
