# 222 Android AppTransition动画加载、AnimationAdapter、SurfaceAnimator与动画Leash

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只讨论旧版 `AppTransition` 的本地 Surface 动画主线：可以由源码证明 `Animation` 如何被选中、怎样翻译成 leash 上的逐帧属性，以及完成、取消、转移如何恢复 Surface 树；不能据此证明事务已被 SurfaceFlinger latch、更不能证明 HWC 已 present。r48 这条主线没有后来独立的 `TransitionAnimation` 类。第 221 章已经完成参与者与 target 选择，第 223 章再处理跨 Binder 的远程 runner。

页面转场看起来只是一次淡入或位移，system_server 却要同时维护资源、几何、临时 Surface 父子关系、两层延迟协议和两套完成回调。最容易误诊的根源，是把“传统 `Animation` 结束”“本地 `ValueAnimator` 结束”“leash 被拆除”“Activity 完成收尾”“像素上屏”当成同一件事。本章把这些完成点逐个拆开。

## 1. 固定一条本地开页链，先命名十个观察点

先固定场景 `L_local`：默认 Display 上执行普通 `TRANSIT_ACTIVITY_OPEN`；target 可动画且未被 organizer 接管；没有 remote controller、change transition、冻结、单对象 start delay 或取消；theme 动画资源有效；动画没有请求延迟完成。后文再逐项放宽。

| 点 | 源码侧定义 | 仍不能推出 |
|---|---|---|
| `A_enter` | `WindowContainer.applyAnimation()` 通过 disable 与 `okToAnimate()` 检查 | 已经得到新 adapter |
| `A_pick` | `getAnimationAdapter()` 返回非空 `LocalAnimationAdapter` | adapter 已经开始执行 |
| `L_make` | `SurfaceAnimator` 创建 leash，并把真实 Surface reparent 到 leash | setup transaction 已提交 |
| `Q_put` | runner 将 `RunningAnimation` 放入 `mPendingAnimations` | `ValueAnimator` 已 start |
| `F_zero` | spec 用 play time 0 把初始 matrix/alpha/crop 写进调用者 transaction | 这笔属性已被 SurfaceFlinger 接收 |
| `V_start` | SF-vsync frame callback 把对象移入 running map，并启动 `ValueAnimator` | 屏幕已经显示第一帧 |
| `F_tick` | update listener 把某一帧写进共享 frame transaction | frame transaction 已 apply |
| `V_end` | `ValueAnimator.onAnimationEnd()` 从 running map 移除对象 | leash 已拆 |
| `C_finish` | AnimationThread 上的 finish runnable 进入 `SurfaceAnimator`，取得 WMS global lock | 完成一定不会被 defer |
| `H_reset` | 当前 adapter 再次通过代际校验，真实 Surface 回原 parent、leash 排入删除、容器 callback 执行 | 删除事务已经 present |

固定场景的源码顺序是：

```text
A_enter < A_pick < L_make < Q_put < F_zero
Q_put < V_start < F_tick < V_end < C_finish < H_reset
```

`Q_put < F_zero` 是 `SurfaceAnimationRunner.startAnimation()` 方法体内的真实顺序；二者通常都发生在统一启动 frame callback 之前。若启用任一延迟、发生替换/转移，或 finish 被 defer，这条线就会分叉。`Transaction.apply()` 之后仍有 SurfaceFlinger 与显示硬件阶段，本章的任何点都不是物理显示证明。

主线可以压成一张调用图：

```text
WindowContainer.applyAnimation
  → getAnimationAdapter
  → AppTransition.loadAnimation
  → WindowAnimationSpec
  → LocalAnimationAdapter
  → SurfaceAnimator.startAnimation
       → createAnimationLeash / reparent
       → LocalAnimationAdapter.startAnimation
            → SurfaceAnimationRunner.startAnimation
                 → 0ms Transformation 写 setup transaction
                 → SF VSync 驱动共享 frame transaction
                 → AnimationThread 投递 finish
       ← global lock 内校验 adapter 身份
       → reset / reparent / remove leash
       → WindowContainer.onAnimationFinished
```

## 2. 六个对象、三条线程与两类 transaction 不可混账

同一个“动画”在代码里至少有六种载体：

| 对象 | 保存什么 | 不负责什么 |
|---|---|---|
| `android.view.animation.Animation` | duration、start offset、interpolator，以及给定时间的 `Transformation` | 不认识 Surface 或 transaction |
| `AnimationSpec` | 给定 play time，怎样把状态写到 leash | 不管理 leash 所有权 |
| `WindowAnimationSpec` | 普通窗口动画的 matrix、alpha、crop、corner 与状态栏时间 | 不调度 VSync |
| `LocalAnimationAdapter` | 把 `AnimationSpec` 接到本地 runner，并保留 animation type/自身身份 | 不创建或拆除 leash |
| `SurfaceAnimationRunner` | pending/running map、SF VSync、共享逐帧 transaction、取消抑制 | 不恢复窗口层级 |
| `SurfaceAnimator` | 当前 adapter/type、leash、延迟、转移、代际校验与完成回调 | 不计算传统动画插值 |

经典路径还跨三条 system_server 执行线：

1. WMS 调用路径通常在 global lock 内完成 adapter 选择、leash 建立与 setup transaction 写入；
2. `SurfaceAnimationThread` 持有 SF `Choreographer`，启动和推进 `ValueAnimator`，在 traversal callback 应用逐帧 transaction；
3. `AnimationThread` 接收正常结束 runnable，随后 `SurfaceAnimator` 再取得 WMS global lock 做层级收尾。

transaction 也要分开看：调用者传入的 `t` 承载 leash 创建、reparent 和 0ms 初态；runner 持有的 `mFrameTransaction` 被所有本地动画逐帧复用，并在 SF Choreographer 的 `CALLBACK_TRAVERSAL` 阶段统一 `apply()`。正常 finish 又把 reparent/remove 写回 animatable 的 pending transaction。它们是两类写入通道，不是一笔从开始一直占有到结束的 transaction。

## 3. applyAnimation 只做准入，返回值也不是“新动画已启动”

`WindowContainer.applyAnimation()` 先处理两个拒绝条件：

- `mDisableTransitionAnimation` 为 true 时，取消当前动画并返回 false；
- Display 不满足 `okToAnimate()` 时，也取消当前动画。

通过准入后，它调用 `applyAnimationUnchecked()`，最后返回的是当时的 `isAnimating()`。这个返回值只检查 `SurfaceAnimator.mAnimation != null`，所以它包含“先开启单对象 delay、随后已调用 `startAnimation()`，leash 已建但 adapter 尚未 start”的状态，也包含 finish 已到但仍在 defer 的状态。若只调用 `startDelayingAnimationStart()`、还没安装 adapter，返回值仍是 false。反过来，`getAnimationAdapter()` 本轮返回 null 时，`applyAnimationUnchecked()` 自身不会主动取消旧动画；因此不能把返回值机械解释为“本轮成功新建了动画”。

非空 adapter 路径先把 `sources` 加入 target 的 `mSurfaceAnimationSources`，再调用：

```java
startAnimation(getPendingTransaction(), adapter, !isVisible(),
        ANIMATION_TYPE_APP_TRANSITION);
```

这里的 `hidden` 是 leash 创建时的初始可见参数，不代表整段动画始终隐藏。若 adapter 要显示壁纸，代码设置 `FINISH_LAYOUT_REDO_WALLPAPER`。local change 只在 snapshot 实际存在时生成 thumbnail adapter并在同一 pending transaction 上启动；remote change 的失败尖角留到第 14 节单独核对。

第 221 章提升 target 后，`sources` 解决的是“高层容器播放一段动画，哪些 Activity 仍应收到完成通知”。正常 `WindowContainer.onAnimationFinished()` 会先遍历这些 source，随后清空集合，再通知 WMS 动画结束并清 `mNeedsZBoost`。这与 adapter 的直接 per-animation callback 是两本账。

sources 的加入发生在 `SurfaceAnimator.startAnimation()` 取消旧 adapter 之前；而 replacement 使用 `restarting=true`，不会触发旧动画的 finished callbacks。因此同一 target 被新动画替换时，旧批 sources 不会在替换点清空，`ArraySet` 会保留并合并新批 sources，等后续一次真正完成或显式 `WindowContainer.cancelAnimation()` 再统一消费。

## 4. adapter 分支先算坐标，而且 remote 严格早于 change 与 local

`getAnimationAdapter()` 先读取 `appStackClipMode`，再拆出三份几何：

| 值 | 形成方式 | 消费者 |
|---|---|---|
| `screenBounds` | `getAnimationBounds()` 的原始范围 | remote record 的 end/stack bounds |
| `mTmpPoint` | `getAnimationPosition()`；非层级模式改成 bounds 左上角 | remote position、普通 spec 的 post-translate |
| 归零后的 `mTmpRect` | 复制 bounds 后 `offsetTo(0, 0)` | 普通 spec 的 stack crop；其余分支会先再定位 |

Activity 的覆盖实现通常以 Task bounds 作为动画范围，并把 animation position 设为 `(0, 0)`，让 letterbox 一并进入动画；不同 `WindowContainer` 子类不能用同一坐标结论概括。关闭 hierarchical animations 时，基类明确改用全局左上角，因为 leash 可能被放进独立 app animation layer。

remote 分支会复制归零 rect，再把副本移到 `mTmpPoint` 形成 `localBounds`；change 分支则直接把 `mTmpRect` 移到该 point 作为 end bounds。普通 `WindowAnimationSpec` 才消费仍以 `(0, 0)` 为左上的 rect，并把位置留给 matrix。

随后分支顺序是：

```text
RemoteAnimationController存在 且 SurfaceAnimator未start-delayed
  → createRemoteAnimationRecord
否则，change transit && enter && 当前容器仍在changing集合
  → WindowChangeAnimationSpec（可带snapshot adapter）
否则
  → AppTransition.loadAnimation
  → WindowAnimationSpec + LocalAnimationAdapter
```

remote record 交给 SurfaceAnimator 的实际对象是 `RemoteAnimationController.RemoteAnimationAdapterWrapper`；`android.view.RemoteAnimationAdapter` 是 controller 保存的外部 runner 配置，二者不能因名字相近而互换。change participant 在 remote controller 存在时仍优先走 wrapper，freeze bounds 会作为 record 的 start bounds 输入。只有当前 `SurfaceAnimator` 已处于单对象 start delay，remote 分支才被挡住并继续考察 change/local。

`TRANSIT_TASK_CHANGE_WINDOWING_MODE` 也不等于无条件创建 `WindowChangeAnimationSpec`：只有 `enter && isChangingAppTransition()` 才进入 change 分支。未满足该谓词而落到普通 local 选择时，`AppTransition.loadAnimation()` 为这个 transit 构造 alpha 1→1 的定时占位动画。

`isOrganized()` 的短路位于 `WindowContainer.loadAnimation()` 内，而不是整个 `getAnimationAdapter()` 入口。它只让最后的普通本地资源加载返回 null，不能据此声称 organized 容器永远不可能先命中 remote 或 change 分支。

普通 local 分支在资源非空时才写 `mTransit` 与 flags，创建 `WindowAnimationSpec`。窗口圆角半径仅在非 multi-window 模式取 Display 配置值；`mNeedsZBoost` 则由传统动画的 `ZORDER_TOP` 或 closing transit 设置。`STACK_CLIP_AFTER_ANIM` 还会令 Activity 在 leash 外再建 animation-bounds layer，裁剪不是只发生在 spec 内。

## 5. AppTransition.loadAnimation 是有优先级的决策链，不是 theme 查表

r48 的 `AppTransition.loadAnimation()` 按代码顺序首次命中即返回。这里同时读取 `transit` 与 `mNextAppTransitionType` 两个状态轴：前者描述本批窗口变化，后者描述一次 override 的动画形态。各 override setter 通常先 `clear()`，所以 custom、clip、scale、thumbnail 等 type 彼此替换，并不是长期并列候选；但更靠前的 voice、relaunch 等 transit 条件仍能遮住已记录的 type。remote 更不在这个方法的分支中，它已由 `getAnimationAdapter()` 提前选择。可把本方法的完整求值顺序压成下面这张表：

| 顺序 | 条件 | 结果 |
|---|---|---|
| 1 | Keyguard going-away 且 enter | no-animation flag 直接给 null；否则 policy 依据 wallpaper/to-shade/subtle 创建 |
| 2 | Keyguard occlude | null |
| 3 | Keyguard unocclude 且 exit | framework wallpaper-open-exit 资源 |
| 4 | crashing-activity-close | null |
| 5 | voice open/front | `voice_activity_open_*` |
| 6 | voice close/back | `voice_activity_close_*` |
| 7 | activity relaunch | 按 frame/insets 构造 relaunch 动画 |
| 8 | custom | 按 enter 选 package 的 enter/exit 资源，并按需挂 ended callback |
| 9 | custom-in-place | package 的单一 in-place 资源 |
| 10 | clip reveal | 按 start rect、frame、display frame 构造 |
| 11 | scale up | 按 start rect 与 frame 构造 |
| 12 | thumbnail scale up/down | 按 thumbnail state 构造 |
| 13 | aspect thumbnail up/down | 继续使用 uiMode、orientation、三类 insets 与 freeform 信息 |
| 14 | cross-profile 且 enter | framework cross-profile enter 资源 |
| 15 | task change windowing mode | alpha 1→1，时长取 change 默认值 |
| 16 | 其余进入 fallback | 只有 switch 已列出的 activity/task/wallpaper/open-behind transit 才映射 styleable；无映射则 null |

这里至少有三个边界。第一，Keyguard going-away 只有 enter 才命中第一项；不能把所有 Keyguard 两侧都称为 policy 动画。第二，cross-profile 专用分支也只处理 enter，另一侧会继续落到后续条件。第三，crash close 与 Keyguard occlude 返回 null 是显式策略，不是资源解析失败。

theme fallback 的 `LayoutParams` 来自第 221 章选出的 anim-LP owner，不保证是“新 Activity 的 theme”。`getCachedAnimations(lp)` 按 package、style resource 与 user 取得 `WindowAnimation` typed array；若资源 ID 高字节属于 framework 包，则强制使用 `android` package/context。starting-window 类型会把 style ID 换成 system 默认值，阻止应用定制 starting 动画，但外层仍先要求 `lp.windowAnimations != 0`。默认 translucent open-enter 与 close-exit 还会被替换成 translucent 专用资源，但任意直接 custom 资源不会被这一步无条件改写。

## 6. 资源非空后先限长、再初始化、最后缩放；三秒不是最终墙钟上限

`loadAnimationAttr()` 从缓存的 style 中取资源 ID，经 translucent 修正后才调用 `loadAnimationSafely()`。后者只捕获 `Resources.NotFoundException`、记录 warning 并返回 null；它不是包住任意运行时错误的万能隔离层。package 版 `loadAnimationRes()` 若拿不到对应 cache entry 也会返回 null。

`WindowContainer.loadAnimation()` 获得非空 `Animation` 后，顺序固定为：

```text
a.restrictDuration(MAX_APP_TRANSITION_DURATION)  // r48常量为3000ms
a.initialize(frame.width, frame.height, display.appWidth, display.appHeight)
a.scaleCurrentDuration(transitionAnimationScale)
```

`restrictDuration()` 会限制 start offset、单次 duration 与 repeat count，连无限 repeat 也会被收敛。但动画倍率在它之后才乘到 duration 与 start offset；若 scale 大于 1，最终 `computeDurationHint()` 仍可能超过 3000ms。因此准确说法是“缩放前按 3 秒限制传统动画参数”，而不是“实际播放永远不超过三秒”。

`initialize()` 的前两个参数来自 `getAnimationFrames()` 最终写出的 animation frame；它依运行时子类与窗口模式，可能来自 main window frame、Task/Stack bounds 或 containing frame，不能统称为内容区或容器自身 bounds。后两个实参是 Display 的 app width/height，也不是实际 Surface parent bounds。百分比 pivot、relative-to-parent 尺寸等传统动画语义依赖这两组尺寸，不能用一个 screen rect 代替。

在调用 `AppTransition.loadAnimation()` 之前还有一个 Activity 特例：`mLaunchTaskBehind` 会把本目标的 `enter` 强制改成 false，让短暂露出的 source 与继续留在屏幕上的另一个 opening Activity 使用相反两侧动画。日志里的原始 opening 身份不能替代这里实际传入的 enter 值。

若资源为 null，普通 local 分支返回两个 null adapter；可见性提交由上层转场流程继续推进，但本目标不会因此凭空得到 leash。还要记住第 3 节的边界：本次 null 不自动证明此前 adapter 已被清掉。

## 7. WindowAnimationSpec 每帧翻译五类状态，crop 与状态栏时间各有窄条件

`WindowAnimationSpec.apply()` 每帧先清当前线程的 `Transformation`，用 play time 调 `Animation.getTransformation()`，再把 `mPosition` 通过 `postTranslate()` 合进矩阵。写入顺序是 matrix、alpha、crop，满足条件时再写 corner radius。

crop 的规则不是“有 clip 才裁剪”：

| `mStackClipMode` | 本帧行为 |
|---|---|
| `STACK_CLIP_NONE` | 只有 `Transformation.hasClipRect()` 才把动画 clip 写到 leash |
| 任意非 NONE | 先复制 `mStackBounds`；有动画 clip 时做交集；随后总会写 `setWindowCrop()` |

圆角需要四个条件同时成立：本帧确实设置了 crop、传统 `Animation.hasRoundedCorners()` 为 true、配置半径大于 0，并且 local adapter 构造时没有因 multi-window 把半径归零。没有 crop 时，源码认为 corner radius 缺少参照范围，不写该属性。

`getDuration()` 返回 `Animation.computeDurationHint()`，包含 start offset 与 repeat 次数的提示值。`getShowWallpaper()` 透传传统动画属性。`canSkipFirstFrame()` 与 `mIsAppAnimation` 则是在构造 spec 时捕获；后者让 `needsEarlyWakeup()` 返回 true，但 r48 的 `SurfaceAnimationRunner` 没有读取这个接口，不能把接口意图写成现有逐 spec 行为。

状态栏开始时间也只是启发式值。spec 只识别“动画本身是 `TranslateAnimation`”或“`AnimationSet` 的直接 child 是 Translate”，不会递归搜索更深层。找到后，它二分近似 interpolator 超过 0.99 的 fraction，再计算：

```text
uptimeNow + startOffset + duration × almostThere - 120ms
```

找不到 translate 就直接返回当前 uptime。`AppTransition.goodToGo()` 从 top opening Activity 的 animating container 取 adapter，把 duration hint 与这个状态栏时间交给 listeners；这不是本地动画真正首帧或末帧 fence。

## 8. SurfaceAnimator 先接管层级，再把 leash 交给 adapter

每次 `SurfaceAnimator.startAnimation()` 首行都会以 `restarting=true, forwardCancel=true` 取消旧动画。它不会给被替换动画发 normal-finish callbacks，但会把取消转给已启动 adapter，并移除旧 leash。随后才安装新的 adapter、type 与 per-animation callback。

若目标 Surface 为 null，新记录会立刻走 public cancel 路径并结束；不会继续建一个空 leash。若 `SurfaceFreezer` 能交出已有 leash，SurfaceAnimator 复用它；否则按下列拓扑创建 effect layer：

```text
开始前                    建立leash后
parent                    animationLeashParent
└── targetSurface         └── leash (EffectLayer)
                              └── targetSurface
```

创建 transaction 依次设置 leash 的 crop、位置、show 与 alpha，再把 target reparent 进去。r48 的这条 `SurfaceAnimator` 调用把 leash 的 x/y 传为 0；普通动画位置由 `WindowAnimationSpec` 的 matrix post-translate 表达。`hidden` 同时影响 builder 初态与 alpha，但 runner 启动时还会在 frame transaction 上把 leash alpha 设为 1，spec 每帧也会继续写 alpha。

新建 leash 后调用 `Animatable.onAnimationLeashCreated()`；复用 freezer leash 时不重复调用，因为 freeze 阶段已经通知过。两条路径都会调用 `onLeashAnimationStarting()`。基类 `WindowContainer` 的 created hook 会重排 layer 并把目标 Surface position 归零；`ActivityRecord` 覆盖该 hook，以动画层的 prefix-order/Z-boost 设置 leash layer，因此诊断时必须看运行时子类，不能把基类副作用套给所有对象。

leash 存在期间，`SurfaceAnimator.setLayer()`、`setRelativeLayer()` 与 `reparent()` 都把操作代理给 leash。基类 `WindowContainer.updateSurfacePosition()` 则在有 leash 时跳过真实 Surface 的外部 position 更新；布局账仍可变化，但承载视觉位置和层级的是临时 parent，不能描述成“目标 Surface 照常独立移动”。

正常拆除时，`removeLeash()` 若目标 Surface 非 null，会进入恢复分支；真正写入 reparent 还要求目标 Surface 自身 valid、parent 非 null且 valid。即使这些有效性检查失败，只要目标 Surface 非 null，`onAnimationLeashLost()` 仍会被调用。基类 hook 会 unfreeze、重排层级并恢复 Surface position；Activity 还会删除 animation-bounds layer并通知 animating registry。

## 9. 两种“延迟启动”位于不同层，0ms 初态也不同

源码里有两套容易同名误判的协议：

| 协议 | 状态保存处 | 延迟时已经发生 | 尚未发生 |
|---|---|---|---|
| `SurfaceAnimator.startDelayingAnimationStart()` | 单个 animatable 的 boolean | 仅置 delay flag；若随后调用 `startAnimation()`，才会安装 adapter/type、建立 leash并调用两个 hook | adapter `startAnimation()`、runner 入队、spec 0ms 初态 |
| `SurfaceAnimationRunner.deferStartingAnimations()` | runner 全局 boolean | Local adapter 已 start；对象已入 pending map；spec 0ms 已写调用者 transaction | pending→running、`ValueAnimator.start()`、逐帧共享 transaction |

单对象协议只有当前 `!isAnimating()` 时才能把 delay 设为 true。刚调用 `startDelayingAnimationStart()` 时还没有 adapter，故 `isAnimating()` 仍为 false；只有随后调用 `startAnimation()`，它才变为 true并进入“leash 已建、adapter 未 start”的 pending 状态。`endDelayingAnimationStart()` 清 flag，再用 animatable pending transaction 启动 adapter并 `commitPendingTransaction()`。若在这一 pending 状态 public cancel，delay flag 会挡住 adapter cancel，但代码仍执行 static/per-animation finished callbacks并删除 leash。

runner 协议明确不可嵌套。`AppTransitionController` 在统一给 opening/closing/changing 绑定动画前调用 `deferStartingAnimations()`，在 `finally` 中调用 `continueStartingAnimations()`；屏幕旋转的多段动画也采用同一对方法。它保证一批 adapter 先全部入 pending，再由同一个 SF frame callback 开始，而不是保证每个 target 的 setup transaction 已物理同时显示。

`continueStartingAnimations()` 只有在 pending map 非空时才 post frame callback。取消尚未启动的 pending 对象会直接从 map 删除，不向 Local adapter 的 finish runnable回调。

## 10. SurfaceAnimationRunner 用 SF VSync 驱动，但 setup 首帧先于 ValueAnimator

runner 构造时通过 `SurfaceAnimationThread` 的 handler 同步取得 `Choreographer.getSfInstance()`，并把 `AnimationHandler` provider 设为 `SfVsyncFrameCallbackProvider`。默认 `SfValueAnimator` 覆盖 `getAnimationHandler()`，因此时间推进跟随 SF-vsync，而不是应用 UI 线程的 Choreographer。

`startAnimation(spec, leash, t, finish)` 在 `mLock` 内完成：

1. 创建 `RunningAnimation`，以 leash 为 key 覆盖/写入 pending map；
2. 未批量延迟时 post `startAnimations` frame callback；
3. 无论是否延迟，都立刻执行 `spec.apply(t, leash, 0)`。

第三步解释了为何 move 动画的初始变换可以和 leash reparent 放进同一 setup transaction。它也给 `canSkipFirstFrame()` 划出精确边界：skip 为 true 时，runner 在 `ValueAnimator.start()` 后把 current play time 推到一个 frame interval，再手工 `doAnimationFrame()`；但此前 0ms Transformation 已经写过，所谓 skip 只影响第一张 ValueAnimator 驱动帧，不是“系统从未计算或写入第 0 帧”。

frame callback 会把所有 pending 项逆序启动后清空 map，并发出 `PowerHint.INTERACTION`。每个 animator 强制 `overrideDurationScale(1.0f)`，因为 WindowContainer 已经缩放传统 `Animation`，避免再次套用 animator duration scale。

update listener 把 play time 截到 duration 上界，在 `mCancelLock` 内确认未取消后调用 spec；所有本地动画共用 `mFrameTransaction`。它随后安排一次 `CALLBACK_TRAVERSAL`，回调中设置 `setAnimationTransaction()`、`apply()` 并清 scheduled flag。一次 traversal 可以合并多个动画的属性写入，但这仍只是提交给 SurfaceFlinger。

## 11. 正常结束要跨线程、两次验代，再先拆层级后通知容器

`ValueAnimator.onAnimationEnd()` 在 `mLock` 下先从 running map 删除对象，再在 `mCancelLock` 下确认没有取消。满足条件时，它只做一件跨线程动作：把 `RunningAnimation.mFinishCallback` post 到 `AnimationThread`。

Local adapter 的 runnable 把 type 与 `this` 重新封装为 `finishCallback.onAnimationFinished(type, this)`。到 `SurfaceAnimator` 后，顺序是：

```text
AnimationThread执行Local adapter finish runnable
  → SurfaceAnimator inner callback取得WMS global lock
  → 先查animationTransferMap
  → 第一次检查：回来的anim必须仍等于mAnimation
  → 构造resetAndInvokeFinish
  → Animatable可先请求defer；未请求才问AnimationAdapter
  → 真正执行runnable时第二次检查anim身份
  → reset(pendingTransaction, destroyLeash=true)
       → 清adapter/type/per-animation callback字段
       → target尝试reparent回parent
       → transaction remove leash
       → onAnimationLeashLost
  → static callback
  → 本次捕获的per-animation callback
```

两次身份检查防两类旧回调：adapter 已被新动画替换，以及 finish 曾被 defer、延迟 runnable 执行前又发生替换。字段在 callback 之前清空，所以 static callback 观察 `isAnimating()` 时，本对象已经不是动画中。

对 `WindowContainer`，static callback 就是 `this::onAnimationFinished`；它先通知 animation sources，再走 WMS 收尾。normal finish 的 per-animation callback排在它之后。此时 reparent/remove 只是写入 pending transaction，`scheduleAnimationLocked()` 负责请求后续动画/提交机会；callback 返回不等于 leash 已从合成树物理消失。

## 12. cancel、restart 与 normal finish 不是同一回调协议

SurfaceAnimator 的内部取消由两个 boolean 决定：`restarting` 控制是否发送 finished callbacks，`forwardCancel` 控制是否通知 adapter并最终删除 leash。四个常用场景如下：

| 场景 | adapter cancel | static/per finished | leash 处理 |
|---|---|---|---|
| public `cancelAnimation()` | `!mAnimationStartDelayed` 时发送 | 发送 | 回父节点并 remove，随后 commit pending transaction |
| 新动画替换旧动画 | `!mAnimationStartDelayed` 时发送 | 不发送 | 回父节点并 remove，复用调用者 transaction |
| start-delayed 后已安装、尚未进 adapter，随后 public cancel | 不发送 | 发送 | 回父节点并 remove |
| transfer 的 source 侧 | 不发送 | 发送 | source Surface 回原 parent，但 leash 不删 |

内部实现先 `reset(t, destroyLeash=false)`，再考虑 adapter 与 callbacks，最后在 `forwardCancel && leash != null` 时把 leash 写入 remove。因此 public cancel 和 normal finish 的源码顺序并不相同：normal finish 在 `reset(..., true)` 内先排入 remove，随后发 callbacks；public cancel 则先 reset/reparent、再 adapter cancel与 callbacks、最后排入 remove。

`!mAnimationStartDelayed` 是代码条件，不等价于“adapter 肯定执行过 start”。一个反例是 start 已安装 adapter、却发现 target Surface 为 null：它立即调用 public cancel，delay flag 为 false，于是仍会把 `onAnimationCancelled(null)` 发给 adapter，尽管该 adapter 从未收到 start。Local runner 对这个 null key没有 pending/running 项，最终只会无操作返回。

runner 自己也区分 pending 与 running。pending cancel 只是从 map 删除；running cancel 先移出 running map，在 `mCancelLock` 写 `mCancelled=true`，再 post 到 SurfaceAnimationThread 调 `ValueAnimator.cancel()` 并 flush 共享 frame transaction。`onAnimationEnd()` 仍可能因 cancel 被调用，但 cancelled 位会抑制 normal finish runnable。最终是否对 WindowContainer 发 finished，由上一层 SurfaceAnimator 的 `restarting` 语义决定。

这解释了两个常见现象：取消后看到容器完成 callback，并不代表 ValueAnimator 自然跑完；替换旧动画时没看到旧容器 normal-finish callback，也不代表旧 adapter 没收到 cancel。

这里还要区分更外层的 `WindowContainer.cancelAnimation()`：它先用当前 type/adapter 主动 `doAnimationFinished()` 并清 sources，随后才调用 `SurfaceAnimator.cancelAnimation()` 做 reset、条件式 adapter cancel、static/per callbacks与 leash remove，最后再 `SurfaceFreezer.unfreeze()`。因此显式容器取消时，source 完成通知甚至早于 leash 恢复；不能套用第 11 节 normal finish 的顺序。

## 13. transfer 改的是 leash 所有者；defer finish 改的是拆除时机

`transferAnimation(target ← from)` 不重启 `ValueAnimator`。target 先结束自身的 start delay并取消自己的旧动画，然后复制 source 的 leash、adapter、type 与 per-animation callback。source 以 `restarting=false, forwardCancel=false` 取消：它恢复自己的 Surface、执行自己的 finished callbacks，却既不取消 runner，也不删除 leash。随后 target Surface 被 reparent 进原 leash，leash 被移到 target parent，target 收到 created hook。

关键一步是：

```java
mService.mAnimationTransferMap.put(mAnimation, this);
```

runner 结束时持有的仍是创建于 source 的 inner callback。该 callback在 global lock 内先从 transfer map 取出 target，若命中便转调 target 的 inner callback；target 才完成当前 adapter 身份检查与拆 leash。转移不是“把 callback对象改写成 target”，而是用 adapter 身份建立一次路由。

finish defer 是另一条轴。inner callback用短路或依次询问：

```text
Animatable.shouldDeferAnimationFinish(resetRunnable)
  为false才询问
AnimationAdapter.shouldDeferAnimationFinish(resetRunnable)
```

只要一方返回 true，`SurfaceAnimator` 就保留 `mAnimation` 与 leash，由该方稍后执行 runnable；若 Animatable 返回 true，Adapter 连询问机会都没有。`ActivityRecord` 可借 `AnimatingActivityRegistry` 协调同一批 Activity 的结束。

defer 期间 `ValueAnimator` 已结束、runner running map 已无此项，但 `SurfaceAnimator.isAnimating()` 仍为 true。若先 cancel/replace，再执行旧 deferred runnable，第二次 adapter 身份检查会让它无操作返回，不会误拆新动画。这正是排查“runner 已空但 WindowContainer 仍 animating”时应看的分界。

## 14. change 动画复用 freezer leash；snapshot、圆角早唤醒又是三本账

changing container 在准备阶段由 `SurfaceFreezer.freeze()` 保存 start bounds，创建一条 leash并把目标 Surface 放进去。这里创建时传入的 animation type 是 `ANIMATION_TYPE_SCREEN_ROTATION`，但真正启动 change adapter 时，`WindowContainer` 仍以 `ANIMATION_TYPE_APP_TRANSITION` 交给 SurfaceAnimator；不要从 freeze 建 leash 的参数反推最终运行 type。

freezer 可对 `getFreezeSnapshotTarget()` 调 `captureLayers()`。只有得到宽高都大于 1 的 buffer 才创建 snapshot Surface；snapshot 与真实 target 同为 freezer leash 的 child，layer 被设为 `Integer.MAX_VALUE`，因此盖在目标内容之上。local change 只有在 `mSnapshot != null` 时才构造 thumbnail adapter，后面的启动代码据此直接解引用同一 snapshot，没有第二次 null 分支。SurfaceAnimator 启动时 `takeLeashForAnimation()` 清掉 freezer 对 leash 的所有权，避免双方重复拆除。

remote change 的保护并不对称：只要传入了 start bounds且外部 `RemoteAnimationAdapter.getChangeNeedsSnapshot()` 为 true，controller 就创建 thumbnail wrapper，并不核对 `SurfaceFreezer.mSnapshot`。而 `captureLayers()` 可能因空 buffer 或尺寸不合格而留下 null；`applyAnimationUnchecked()` 看到非空 wrapper 后仍直接调用 `mSnapshot.startAnimation()`。所以这条组合会在当前源码中触发空指针，而不是“没有缩略图但主 remote 动画照常启动”。这是实现边界，不能被 local change 的 null guard掩盖。

`WindowChangeAnimationSpec` 有两条输出：

- 真实目标按 start/end bounds 生成 scale、translate、clip；裁剪矩形先用 matrix 的缩放向量求逆，让 crop 表现为“缩放之后”的裁剪；
- snapshot adapter 做 alpha fade与固定逆比例 scale，是否推迟 fade 取决于源码判定的 growing/shrinking。

snapshot 动画直接把 snapshot Surface 当 animation leash交给 Local adapter，不再套一层 `SurfaceAnimator`。runner 自然结束只调用传入的空 finish callback；snapshot Surface 的销毁与字段清理由后续 `unfreeze()` 负责。正常主 leash 的 `onAnimationLeashLost()` 会触发 unfreeze：取消尚存 snapshot adapter、移除 snapshot；若 freezer leash已经被 take，它不会再次拆主 leash。

早唤醒也不能并到这套 snapshot 账。`WindowAnimationSpec` 与 `WindowChangeAnimationSpec` 都可让 `needsEarlyWakeup()` 对 app animation 返回 true，但本版本 runner未消费。实际可见实现位于 `WindowAnimator`：它以 `TRANSITION | CHILDREN` 遍历 root，并请求 app-transition、screen-rotation、recents 三类 Surface 动画；从 false 到 true 时暂停 TaskSnapshot 持久化并 `setEarlyWakeupStart()`，反向边沿则恢复并 `setEarlyWakeupEnd()`。还要注意 `WindowContainer.isSelfAnimating()` 的第二分支：只要带 `TRANSITION` flag 且对象仍 `isWaitingForTransitionStart()`，即使尚无匹配 type 的 SurfaceAnimator，也能使谓词为 true。这是全局树谓词，不是逐 spec 开关。

## 15. 九个只读练习把每个结论钉回 r48

以下命令只读源码，均可直接在 macOS 自带 Bash 3.2 或 Zsh 中执行。每个 `rg -e` 都是独立证据点；不要只看某个命中行的名字，要连同所在分支与前后顺序解释。

### 练习 1：确认准入、返回值与 source 归属

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (mWmService.mDisableTransitionAnimation) {' \
  -e 'if (okToAnimate()) {' \
  -e 'return isAnimating();' \
  -e 'mSurfaceAnimationSources.addAll(sources);' \
  -e 'startAnimation(getPendingTransaction(), adapter, !isVisible(),' \
  -e 'if (adapter.getShowWallpaper()) {' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
```

分别解释“本轮 adapter 为 null但旧动画仍在”和“adapter 已安装但单对象 start-delayed”时返回值可能是什么。

### 练习 2：还原几何与三分支优先级

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'final Rect screenBounds = getAnimationBounds(appStackClipMode);' \
  -e 'getAnimationPosition(mTmpPoint);' \
  -e 'mTmpPoint.set(mTmpRect.left, mTmpRect.top);' \
  -e 'mTmpRect.offsetTo(0, 0);' \
  -e 'if (controller != null && !mSurfaceAnimator.isAnimationStartDelayed()) {' \
  -e '} else if (isChanging) {' \
  -e 'new WindowAnimationSpec(a, mTmpPoint, mTmpRect,' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
rg -n -F \
  -e 'class RemoteAnimationAdapterWrapper implements AnimationAdapter {' \
  -e 'if (mRemoteAnimationAdapter.getChangeNeedsSnapshot()) {' \
  frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
```

给一个 changing target 同时安装 remote controller，再分别令单对象 delay 为 false/true，手推实际分支。

### 练习 3：按源码顺序列出 Animation 的选择链

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (isKeyguardGoingAwayTransit(transit) && enter) {' \
  -e '} else if (transit == TRANSIT_KEYGUARD_OCCLUDE) {' \
  -e '} else if (transit == TRANSIT_CRASHING_ACTIVITY_CLOSE) {' \
  -e '} else if (isVoiceInteraction && (transit == TRANSIT_ACTIVITY_OPEN' \
  -e '} else if (transit == TRANSIT_ACTIVITY_RELAUNCH) {' \
  -e '} else if (mNextAppTransitionType == NEXT_TRANSIT_TYPE_CUSTOM) {' \
  -e '} else if (mNextAppTransitionType == NEXT_TRANSIT_TYPE_CLIP_REVEAL) {' \
  -e '} else if (transit == TRANSIT_TASK_CHANGE_WINDOWING_MODE) {' \
  -e 'a = animAttr != 0 ? loadAnimationAttr(lp, animAttr, transit) : null;' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
```

构造 voice+custom、relaunch+custom、crash-close+custom 三组输入，说明为什么 override 类型不等于最终分支。

### 练习 4：验证资源失败与限长、初始化、缩放顺序

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'return AnimationUtils.loadAnimation(context, resId);' \
  -e '} catch (NotFoundException e) {' \
  -e 'return null;' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
rg -n -F \
  -e 'a.restrictDuration(MAX_APP_TRANSITION_DURATION);' \
  -e 'a.initialize(containingWidth, containingHeight, width, height);' \
  -e 'a.scaleCurrentDuration(mWmService.getTransitionAnimationScaleLocked());' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
rg -n -F \
  -e 'static final int MAX_APP_TRANSITION_DURATION = 3 * 1000;' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
```

令 scale=2，解释为什么“3000ms常量存在”仍不足以证明最终 duration hint 小于等于 3000ms。

### 练习 5：逐项核对 spec 的每帧属性与状态栏时间

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'tmp.transformation.clear();' \
  -e 'tmp.transformation.getMatrix().postTranslate(mPosition.x, mPosition.y);' \
  -e 't.setMatrix(leash, tmp.transformation.getMatrix(), tmp.floats);' \
  -e 't.setAlpha(leash, tmp.transformation.getAlpha());' \
  -e 'mTmpRect.intersect(tmp.transformation.getClipRect());' \
  -e 't.setCornerRadius(leash, mWindowCornerRadius);' \
  -e 'TranslateAnimation openTranslateAnimation = findTranslateAnimation(mAnimation);' \
  -e 'return SystemClock.uptimeMillis();' \
  frameworks/base/services/core/java/com/android/server/wm/WindowAnimationSpec.java
```

分别推演 `STACK_CLIP_NONE` 无 clip、非 NONE 无 clip、非 NONE 且动画 clip 更小三种 crop 结果。

### 练习 6：比较两套 start delay 与 0ms 初态

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (mAnimationStartDelayed) {' \
  -e 'mAnimation.startAnimation(mLeash, mAnimatable.getPendingTransaction(),' \
  -e 'mAnimatable.commitPendingTransaction();' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
rg -n -F \
  -e 'mAnimationStartDeferred = true;' \
  -e 'mPendingAnimations.put(animationLeash, runningAnim);' \
  -e 'applyTransformation(runningAnim, t, 0 /* currentPlayTime */);' \
  -e 'mChoreographer.postFrameCallback(this::startAnimations);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimationRunner.java
```

画出两个 delay 各自取消时，adapter 是否见过 start、runner map 是否已有对象、0ms 状态是否已写。

### 练习 7：追 SF VSync、共享 transaction 与正常 finish 线程

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mSurfaceAnimationHandler.runWithScissors(() -> mChoreographer = getSfInstance(),' \
  -e 'new SfVsyncFrameCallbackProvider(mChoreographer)' \
  -e 'anim.overrideDurationScale(1.0f);' \
  -e 'anim.setCurrentPlayTime(mChoreographer.getFrameIntervalNanos() / NANOS_PER_MS);' \
  -e 'anim.doAnimationFrame(mChoreographer.getFrameTime());' \
  -e 'mChoreographer.postCallback(CALLBACK_TRAVERSAL, mApplyTransactionRunnable,' \
  -e 'mFrameTransaction.setAnimationTransaction();' \
  -e 'mAnimationThreadHandler.post(a.mFinishCallback);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimationRunner.java
```

标出 setup transaction、frame transaction、SurfaceAnimationThread 与 AnimationThread 的交界，不把 `apply()` 标成 present。

### 练习 8：验证 normal finish、cancel、defer 与 transfer 的差异

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'final SurfaceAnimator target = mService.mAnimationTransferMap.remove(anim);' \
  -e 'if (anim != mAnimation) {' \
  -e 'mAnimatable.shouldDeferAnimationFinish(resetAndInvokeFinish)' \
  -e 'reset(mAnimatable.getPendingTransaction(), true /* destroyLeash */);' \
  -e 'cancelAnimation(t, true /* restarting */, true /* forwardCancel */);' \
  -e 'if (!mAnimationStartDelayed && forwardCancel) {' \
  -e 'from.cancelAnimation(t, false /* restarting */, false /* forwardCancel */);' \
  -e 'mService.mAnimationTransferMap.put(mAnimation, this);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
```

模拟 A→B 转移后 A 的旧 callback 到达，再模拟 B 被 C 替换后旧 deferred runnable 到达，写出两次路由/代际判断结果。

### 练习 9：核对 freezer、snapshot 与全局 early-wakeup

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mLeash = SurfaceAnimator.createAnimationLeash(mAnimatable, mAnimatable.getSurfaceControl(),' \
  -e 'SurfaceControl freezeTarget = mAnimatable.getFreezeSnapshotTarget();' \
  -e 'if (buffer == null || buffer.getWidth() <= 1 || buffer.getHeight() <= 1) {' \
  -e 't.setLayer(mSurfaceControl, Integer.MAX_VALUE);' \
  -e 'SurfaceControl takeLeashForAnimation() {' \
  -e 'mSnapshot.cancelAnimation(t, false /* restarting */);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceFreezer.java
rg -n -F \
  -e 'ANIMATION_TYPE_APP_TRANSITION | ANIMATION_TYPE_SCREEN_ROTATION' \
  -e 'mService.mTaskSnapshotController.setPersisterPaused(true);' \
  -e 'mTransaction.setEarlyWakeupStart();' \
  -e 'mTransaction.setEarlyWakeupEnd();' \
  frameworks/base/services/core/java/com/android/server/wm/WindowAnimator.java
rg -n -F \
  -e 'if ((flags & TRANSITION) != 0 && isWaitingForTransitionStart()) {' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
```

分别说明 spec 接口返回 true、root 昂贵动画谓词翻转、snapshot Surface 被销毁是哪三个独立事件，并补上 transition waiter 尚未绑定 SurfaceAnimator 的一例。

## 16. 排障矩阵、源码地图与下一章边界

| 现象 | 第一证据点 | 常见误判 | 下一步 |
|---|---|---|---|
| `applyAnimation()` 返回 false | disable、`okToAnimate()`、adapter 是否为空 | 一定是 XML 缺失 | 先分准入、remote/change/local 分支 |
| local adapter 为空 | `isOrganized()` 与 `loadAnimation()` 首个命中分支 | organizer 是全局禁动画开关 | 确认是否早已走 remote/change |
| 动画起点跳一下 | setup transaction 中的 0ms spec、position 与 crop | skip-first-frame 会删除所有 0ms 写入 | 分开初态写入与首张 ValueAnimator 帧 |
| 多个 target 起跑不齐 | runner pending map与批量 defer | adapter start调用必须同一 Java 栈帧 | 检查 `continueStartingAnimations()` 和 SF frame callback |
| 窗口带着内容跑出边界 | stack clip mode、animation-bounds layer、leash crop | 只有传统 Animation clip 生效 | 同时核对 spec crop与外层 bounds layer |
| runner map 已空仍 animating | `shouldDeferAnimationFinish()` | ValueAnimator 没结束 | 查 Animatable优先级与保存的 end runnable |
| 新动画被旧 finish 拆掉 | 两次 `anim != mAnimation` 检查 | callback 只靠时间顺序安全 | 核对 adapter 实例身份与替换路径 |
| transfer 后 source leash 消失 | `forwardCancel=false` 与 transfer map | source cancel一定remove leash | 看 target reparent与最终路由 |
| cancel 后仍收到容器完成 | SurfaceAnimator public cancel 的 callback分支 | runner 发了自然结束 | 区分 runner抑制与上层主动通知 |
| change结束仍残留截图 | `onAnimationLeashLost()`→`unfreeze()` | snapshot runner finish会自行destroy | 查看 freezer snapshot字段与 pending transaction |
| 动画结束但画面仍未稳定 | finish callback、pending remove与显示fence | `H_reset` 等于present | 继续抓 SurfaceFlinger transaction/latch/present 证据 |

源码导航：

```text
frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
frameworks/base/services/core/java/com/android/server/wm/AnimationAdapter.java
frameworks/base/services/core/java/com/android/server/wm/LocalAnimationAdapter.java
frameworks/base/services/core/java/com/android/server/wm/WindowAnimationSpec.java
frameworks/base/services/core/java/com/android/server/wm/WindowChangeAnimationSpec.java
frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimationRunner.java
frameworks/base/services/core/java/com/android/server/wm/SurfaceFreezer.java
frameworks/base/services/core/java/com/android/server/wm/WindowAnimator.java
frameworks/base/services/tests/wmtests/src/com/android/server/wm/SurfaceAnimatorTest.java
frameworks/base/services/tests/wmtests/src/com/android/server/wm/SurfaceAnimationRunnerTest.java
frameworks/base/services/tests/wmtests/src/com/android/server/wm/WindowAnimationSpecTest.java
```

本章最小心智模型是：`AppTransition` 只负责选出传统动画；`WindowAnimationSpec` 把时间映射为合成属性；`LocalAnimationAdapter` 保留桥接身份；runner 负责 SF VSync 与逐帧 transaction；`SurfaceAnimator` 才拥有 leash 和完成代际。0ms 初态、ValueAnimator 终点、leash reset、WindowContainer callback 与物理 present 是五个不同观察点。

下一章进入第 223 章“Android RemoteAnimationAdapter、RemoteAnimationController与Launcher/Recents远程转场”，追 target/leash 怎样跨 Binder 交给外部 runner，以及启动、取消、超时、进程死亡和 finish 如何重新接回 WMS。
