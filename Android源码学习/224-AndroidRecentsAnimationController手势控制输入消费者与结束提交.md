# 224 Android RecentsAnimationController手势控制、输入消费者与结束提交

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只讨论旧版 Quickstep Recents Animation：Launcher 怎样请求一段可持续操控的 Task 场景，system_server 怎样交付初始与动态 target，输入怎样从 App 窗口切到预注册 consumer，以及 finish、cancel、截图保活各自在哪一层结束。第 225 章再展开 Launcher 逐帧矩阵、圆角、裁剪与位移映射。

第 223 章的普通 Remote Animation 是 start/cancel/finished 三段协议；Recents Animation 则把一次动画扩成一段可交互会话。它既要让 Launcher 长时间持有多个 Task leash，又要容纳手势中启动的新 Task、输入路由切换和三种最终层级选择。理解这条链的关键不是背接口，而是始终分开：控制消息已返回、WMS 状态已改、Surface transaction 已提交、SurfaceFlinger 已 latch，以及像素已 present。

## 1. 固定一次上滑场景，先给十八个观察点命名

先固定场景 `G_app_to_overview`：设备使用手势导航；当前前台是普通 App；Quickstep 与 Home/Recents 同进程；目标 Activity 已预加载但当前不可见；启动时没有旧 Recents controller；默认 Display、默认 TaskDisplayArea；wallpaper 已 ready；初始 Task 都有 main window 和 Surface；runner 存活；用户上滑进入非 live-tile Overview，最终显式结束到 Recents。

| 点 | 源码侧含义 | 仍不能推出 |
|---|---|---|
| `C_down` | `TouchInteractionService` 的 gesture monitor 收到本次手势 | Recents input consumer 已可见 |
| `C_queue` | `TaskAnimationManager` 把 `startRecentsActivity()` 排到 `UI_HELPER_EXECUTOR` | system_server 已收到请求 |
| `C_init` | `GestureState` 写入 `STATE_RECENTS_ANIMATION_INITIALIZED` | runner 已收到 target |
| `A_enter` | ATMS 完成入口验权并保存请求 caller pid/uid | runner Binder 宿主已认证 |
| `A_stage` | `RecentsAnimation` 临时移动目标 stack、置 `mLaunchTaskBehind` | 最终层级已决定 |
| `W_oldCheck` | WMS 执行同步旧 controller 取消检查；固定场景为空操作 | 有旧会话时替换一定隔离 |
| `W_collect` | 新 controller 收集初始 Task 并启动 `SurfaceAnimator` | 每个 target 都能构造成功 |
| `L_taskCapture` | 初始 App Task adapter 截获 leash、finish callback、animation type 和几何 | Task setup transaction 已提交 |
| `S_taskSetup` | 后续 surface placement merge/close 初始 App Task setup | SurfaceFlinger 已 latch |
| `B_start` | 建 wallpaper targets 后发出 oneway `onAnimationStart()` | wallpaper setup 已提交，或 Launcher Binder Stub 已执行 |
| `C_wrap` | Launcher Binder 线程包装 targets 并构造客户端 controller | UI listener 已回调 |
| `C_started` | 主线程 listener 收到 `onRecentsAnimationStart()` | input consumer 已接管 |
| `I_enable` | 同步 controller 调用把服务端 boolean 置 true，并请求更新 input windows | consumer Surface 已提交或可命中 |
| `I_proxy` | Launcher 为目标终态安装 `InputConsumerProxy` listener | 旧触摸链一定迁移到 consumer |
| `F_localPost` | Launcher wrapper 已把 finished listeners 投到 MAIN | listener 已执行，或服务端 finish 请求已发出 |
| `F_server` | 同步 `IRecentsAnimationController.finish()` 在 system_server 返回 | WMS 层级恢复 transaction 已提交 |
| `S_finish` | Display pending transaction 被 merge 并提交给 SurfaceFlinger | 对应帧已 present |
| `P_present` | HWC present fence 对应的画面真正显示 | 可由前面的 Java callback 替代证明 |

固定成功路径的核心偏序是：

```text
C_down < C_queue < A_enter < A_stage < W_oldCheck < W_collect < L_taskCapture
C_queue < C_init                         （C_init 与 A_enter 无固定先后）
L_taskCapture < S_taskSetup < B_start < C_wrap < C_started
wallpaper setup pending-write < B_start；wallpaper setup submit 与 B_start 无固定偏序
C_started < I_enable；I_proxy 取决于 Launcher 选择的终态
F_localPost < enqueue helper finish
MAIN finished listener执行 与 helper同步finish调用/返回 无固定偏序
服务端reset写pending < S_finish < P_present
```

这不是全序。尤其 `C_init` 是客户端立即写的状态，可能早于 system_server 收到请求；`F_localPost` 只固定在 helper finish 入队之前，MAIN listener 真正执行会与 helper 竞跑。日志里看到 initialized 或 finished listener，都不能直接跨越到服务端或显示层完成点。

## 2. 三个控制面、两条 AIDL 与一枚可转交能力

Recents 会话至少跨三层对象：

| 层 | 关键对象 | 保存的事实 |
|---|---|---|
| ATMS 编排层 | `RecentsAnimation` | 目标 Activity type、临时 stack 位置、launch-behind Activity、恢复锚点、最终 reorder callback |
| WMS 控制层 | 服务端 `RecentsAnimationController` | runner、初始/动态 Task adapters、wallpaper adapters、输入开关、取消/截图/failsafe 状态 |
| Launcher 控制层 | `TaskAnimationManager`、`RecentsAnimationCallbacks`、客户端 `RecentsAnimationController` | 当前 listeners、targets、last appeared target、控制 Binder 包装与本地 Surface 引用 |

两条 AIDL 的方向和同步性相反：

```text
system_server -- oneway onAnimationStart(controller, targets...) --> Launcher
system_server -- oneway onAnimationCanceled(snapshot) -----------> Launcher
system_server -- oneway onTaskAppeared(target) -------------------> Launcher

Launcher -- 同步 screenshotTask / finish / input / bars / defer / cleanup / remove --> system_server
```

`IRecentsAnimationRunner` 整个接口声明为 `oneway`；`IRecentsAnimationController` 没有 `oneway`。因此 runner 通知的代理返回只说明 oneway 事务已交给 Binder 驱动，不说明远端 Stub 或主线程 listener 已执行。反向 controller 方法会等 Stub 返回，但“同步”只约束 Binder 方法本身，不自动等待 input transaction、Display pending transaction、SF latch 或 present。

controller Stub 没有逐方法重复 `MANAGE_ACTIVITY_STACKS` 验权。screenshot、finish、system bars、input 和 remove 等路径会清除调用身份；defer、cleanup screenshot 与 will-finish boolean 则直接在 global lock 下改 controller 状态。安全边界是：ATMS 接受获准 caller 提供的 runner Binder，再把这枚 controller 句柄回调给它。该句柄是一枚可转交能力；若持有者主动交给别的进程，Stub 不会重新证明对方就是最初 caller。

还要分开三种身份：

- `startRecentsActivity()` 的 Binder caller pid/uid：入口验权与 `WindowProcessController` 查找使用它；
- `IRecentsAnimationRunner` Binder 的真实宿主：这条路径没有用 caller pid/uid 反查它；
- Launcher 收到的 controller capability：后续控制调用凭持有 Binder 句柄进入，而不是凭每次调用者再次过入口权限。

所以“caller 进程正在跑 Recents animation”只描述请求 caller 的进程标志，不是 runner 所有权证明。

## 3. Launcher 发起请求：initialized、started 与代际空洞

`OtherActivityInputConsumer` 在手势开始阶段构造 handler。若 `TaskAnimationManager.isRecentsAnimationRunning()` 为 false，它走 `startRecentsAnimation()`；若已有 controller，则用 `continueRecentsAnimation()` 把新 `GestureState` 接到原会话。

新请求的顺序很容易被方法名误导：

1. 主线程建立新的 `RecentsAnimationCallbacks`，依次加入管理器 listener、`GestureState` 和交互 handler；
2. 把同步 `ActivityManagerWrapper.startRecentsActivity()` 排进 `UI_HELPER_EXECUTOR`；
3. 不等 helper，更不等 Binder start callback，立即写 `STATE_RECENTS_ANIMATION_INITIALIZED`；
4. system_server 之后 oneway 回调，Launcher Binder 线程先包装 targets、构造客户端 controller；
5. 只有 listeners 的分发被投回主线程；此时 `GestureState` 才能进入 started 语义。

`ActivityManagerWrapper` 会捕获任意 `Exception`。它仅在调用方传入非空 `resultCallback` 时报告 true/false；`TaskAnimationManager` 在 r48 传的是 null。因此权限错误、服务异常或参数异常可以被包装层吞掉，而 initialized 状态仍保留。诊断不能把 initialized 当作“请求已接受”，应继续寻找 ATMS 入口、server start、Launcher callback 三侧证据。

更棘手的是这份管理器没有 request generation：

- 旧请求还在路上、`mController == null` 时，新请求不会取消旧请求，却会覆盖 `mCallbacks`、`mLastGestureState` 等共享字段；迟到的旧 callback 可能撞进新一代状态；
- 旧 controller 已存在时，Studio build 直接抛 `IllegalArgumentException`，不会继续 force-finish；
- 非 Studio build 只记录错误，再调用 `finishRunningRecentsAnimation(false)`。该方法把旧 controller finish 投到 MAIN，立即做客户端清理；随后新 start 已可排入 UI helper。旧 finish 不是新 start 的屏障。

典型队列可写成：

```text
UI线程：post oldFinishToMain → localCleanupOld → enqueue newStartToHelper
MAIN稍后：old finish wrapper → enqueue oldBinderFinishToHelper
UI_HELPER：newStart 可能先于 oldBinderFinish
```

新 start 到达 system_server 时会同步取消 WMS 里的旧 controller，这能收敛多数交叠，却不能补出客户端缺失的代际检查。读竞态日志时要按 callback 所属 Binder 句柄和 targets，而不是只看 `TaskAnimationManager` 当前字段。

## 4. ATMS 入口、预加载与 caller 运行标志

`ActivityTaskManagerService.startRecentsActivity()` 先执行 `enforceCallerIsRecentsOrHasPermission(MANAGE_ACTIVITY_STACKS)`。与配置 Recents UID 具有相同 appId 的 caller 可进入；其他 caller 需要 signature 级栈管理权限。它在清身份前保存 pid/uid，随后在 global lock 内查 `WindowProcessController`，再创建 `RecentsAnimation`。

runner 为 null 时只走预加载：

- 已有目标且已 visible requested 或是 top running Activity，直接返回；
- 已有进程则刷新配置；没有 ActivityRecord 就用 background launch 创建；
- 目标未 attach 时可显式启动进程/Activity，但 `andResume=false`；
- 不处于 STOPPING/STOPPED 的目标被加入 stopping，让客户端仍有机会 traversal，而不是成为前台 resumed Activity。

background launch 并不沿用 Binder caller 身份。`ActivityStarter` 被显式写入配置的 `mRecentsUid`、Recents package/feature 与当前 userId；`execute()` 的结果没有被检查。若启动失败，后续重新查得的 target stack/Activity 可能为空，并在摆位或 launch-behind 写入处抛异常。外层只 log、rethrow、恢复 layout/trace，不会向 runner 补 cancel，也不对 running flag、已移动 stack 或 launch-behind 做统一回滚。

runner 非 null 才进入真实会话。`RecentsAnimation` 根据 intent component 决定目标 type：只有 component 等于配置的 Recents component 才是 `ACTIVITY_TYPE_RECENTS`，否则是 `ACTIVITY_TYPE_HOME`。类名叫 Recents 并不意味着目标永远是 Recents Activity。

入口找到 caller WPC 后，启动会在建 controller 前调用 `setRunningRecentsAnimation(true)`；finish 再写 false。该布尔与 remote-animation 布尔做 OR 后异步推动 AM 的进程状态更新。它既不是 runner callback 完成点，也不保证 OOM 调整已经执行。

这里存在一个同 caller 替换边界：新请求先写 true，但旧值本来已经为 true，于是什么也不发；随后同步取消旧 controller，旧 finish 写 false。新会话不会再次把它写回 true，于是 WMS 仍可继续控制新会话，而 caller 的 running-recents 标志已经为 false。反过来，若 `getProcessController()` 找不到 caller，整场动画仍可启动，只是没有这份进程标志。

异常路径同样需要单独看：running 标志在 stack staging 前写入，通用 `catch/finally` 只恢复 window layout 与 trace；后续异常没有统一回滚这枚布尔。不能把它当作服务端 controller 是否存在的权威状态。

## 5. 目标 Activity 先被临时摆位，ORIGINAL 也不是完整撤销

若目标 Activity 已存在，启动先保存“目标 stack 当时正上方的 stack”为 `mRestoreTargetBehindStack`。找不到上方 stack 时，服务端 oneway 通知 cancel(null) 并直接 return。这条早退位于 `Trace.traceBegin()` 之后、通用 `try/finally` 之前，所以本次 trace 没有配对的 `traceEnd()`；它是诊断瑕疵，不应被理解为 controller 已建立。

真实 staging 在 `deferWindowLayout()` 中完成：

1. 既有目标 stack 被移动到最底部可见 stack 后面；
2. 如果目标 Activity 所属 Task 不是该 stack 最上层 Task，再把该 Task 提到 stack 顶部；
3. 没有目标 Activity 时，以 `avoidMoveToFront`、`NEW_TASK | NO_ANIMATION` 等约束做 background launch，再把新 stack 放到底部；
4. 给精确的目标 Activity 置 `mLaunchTaskBehind=true`，替换 intent extras；
5. 同步取消旧 WMS controller，再初始化新 controller；
6. 捕获完可见 Task 后才 `ensureActivitiesVisible(..., PRESERVE_WINDOWS)`。

保存的恢复信息只有一枚 stack 锚点，没有保存目标 Task 在 stack 内的旧 index。因此开始阶段若执行了 `positionChildAtTop(task)`，`REORDER_MOVE_TO_ORIGINAL_POSITION` 也不会把 Task 恢复到原来的兄弟位置。新增目标 Activity 的路径根本没有旧锚点；`moveStackBehindStack()` 遇到 null、同一 stack 或不同 parent 会直接返回。

有旧 controller 时还会跨会话串写：新 `RecentsAnimation` 已把同一目标 Activity 置为 launch-behind，才调用 `cancelRecentsAnimation()`。旧 `RecentsAnimation.finishAnimation()` 随后可能用旧的 launched Activity 与 restore anchor 清 `mLaunchTaskBehind`、重排 stack；新会话不会在 cancel 返回后重新断言 launch-behind。WMS controller 虽被替换，Activity/stack 却是共享状态，不能把 `W_oldCheck` 当成代际隔离屏障。

还要注意 launch-behind 的清理条件：finish 先做 controller cleanup，再从当前目标 stack 查 `mLaunchedTargetActivity`。若该 Activity 已不在 stack，代码直接 return，来不及把 `mLaunchTaskBehind` 清成 false。名称里的 ORIGINAL 是“尝试恢复 stack 相对位置”，不是事务式回滚所有启动副作用。

`mDefaultTaskDisplayArea` 在 `RecentsAnimation` 构造时就固定为 Root 的默认 TDA。这一限制贯穿目标 stack 查找、listener 注册和重排；不能因为 controller 带着 `displayId` 就推断 r48 已支持任意 display/TDA。

## 6. 初始 Task、leash 与 readiness：先提交 setup，不等于已显示

controller 初始化从指定 Display 的默认 TDA 取两组 Task：当前 visible tasks，以及目标 Home/Recents stack 下的全部 leaf Tasks；合并时去重。它跳过 floating tasks 和 split-screen primary，给其余 Task 建 `TaskAnimationAdapter`。`isNotInRecents` 来自 recent-task id 集合，与 Task 当前是否可见不是同一概念。

若筛选后没有 Task，或 runner 的 death link 建立失败，`initialize()` 会同步 cancel、回调 ATMS、令 WMS controller cleanup/null 后再 return。但外层 `RecentsAnimation.startRecentsActivity()` 不检查初始化是否仍成功，仍继续 ensure visible、记录 metrics，并在末尾注册 TaskDisplayArea stack-order listener。cleanup 的 unregister 发生在这次注册之前，因而删不到它；留下的 listener 以后虽会因 WMS controller 为 null 而快速返回，却仍是迟到注册的生命周期残留。

每个 adapter 在构造时快照 Task bounds、相对 parent 的 local bounds；`Task.startAnimation()` 随后借 `SurfaceAnimator` 建 leash，并把这些信息写入初始 transaction：

```text
setPosition(leash, localBounds.left, localBounds.top)
setWindowCrop(leash, localBounds translated to 0,0)
capture leash + finishCallback + ANIMATION_TYPE_RECENTS
```

Remote target 的创建门只检查 top visible Activity 的 main window。mode 也只有两种：top visible Activity type 等于目标 Home/Recents type 时为 OPENING，否则为 CLOSING；这条 Recents 路径不生成 CHANGING target。position、localBounds、screenSpaceBounds 是 adapter 构造阶段的值，不是 Launcher 每次读取时的实时查询。

一个隐含不变量是 adapter 必须先收到 `startAnimation()`：`createRemoteAnimationTarget()` 没检查 captured leash，`removeAnimation()` 又直接解引用 captured finish callback。Task Surface 不存在或 wrapper 未捕获时，坏 target 清理可能以空指针结束，而不是稳定地降级为 cancel。任一 Task adapter 后续被 `SurfaceAnimator` 取消，也会取消整场 Recents 会话并选择 ORIGINAL。

`task.commitPendingTransaction()` 这个名字比实际保证更强：`WindowContainer.commitPendingTransaction()` 在 r48 只调用 `scheduleAnimation()`。不过固定初始 App Task 路径外层仍处于 `RecentsAnimation.startRecentsActivity()` 的 window-layout defer 中；`continueWindowLayout()` 后的 surface placement 会 merge/close Task setup transaction，Root 再执行 `checkAnimationReady()`。在本章固定成功路径里，可以建立：

```text
initial App Task leash captured < Task setup submit < oneway onAnimationStart enqueue
```

这只保证初始 App Task 的提交次序，不证明 SF 已 apply、latch 或 present。wallpaper adapters 是 `startAnimation()` 内、ready check 之后才创建的；其 leash 操作写入 wallpaper pending transaction 后就紧接 oneway start，没有第二个 merge/close 屏障。因此 wallpaper setup submit 与 runner 收到 start 之间没有同样的偏序保证。

若目标可能成为 wallpaper target，ready 还要求 wallpaper target 非 null 且 `wallpaperTransitionReady()`。未绘制 wallpaper 会启动独立的 500ms draw timeout；超时把状态改成 TIMEOUT，直接调用 pending Recents controller 的 `startAnimation()`，随后强制 surface placement。它只防 wallpaper 阻塞，不是整场 controller 的通用超时。第 13 节的 1 秒 failsafe 还要由外部事件显式触发，也不能与这 500ms 混为一谈。start 时先构造 app targets、再建 wallpaper targets，把 `mPendingStart=false`，更新 layout/insets，最后 oneway 回调 runner。回调抛 `RemoteException` 只记录日志，不重试，也不自动取消；metrics 通知仍会继续。

传给 runner 的 content insets 优先取目标 Activity main window；窗口尚未建立时才退到指定 Display 的 stable insets。minimized-home bounds 也不是永远存在，只有目标 Activity 位于 split-screen secondary 时才传出。wallpaper targets 则通过 Root 全局遍历建立，没有在这条调用中按 controller display 过滤。

多显示边界因此更窄：初始 tasks 固定默认 TDA，Root readiness 读取默认 Display wallpaper，cleanup booster 也固定默认 Display。`displayId` plumbing 只覆盖部分数据，不等价于完整多 Display、多 TDA 支持。

## 7. Target 到达 Launcher 后，每帧主数据面不再走 controller Binder

`onAnimationStart()` 到达 Launcher 时，`ActivityManagerWrapper` 的 Binder Stub 先把 framework targets 包成 compat 对象；`RecentsAnimationCallbacks` 仍在 Binder 线程上创建 `RecentsAnimationTargets` 和客户端 `RecentsAnimationController`。只有遍历 listeners 的动作被 post 到 MAIN。因而以下顺序才准确：

```text
Binder Stub wrap targets
  → Binder线程构造 Targets/Controller
  → post MAIN
  → TaskAnimationManager保存mController/mTargets
  → GestureState与手势handler收到start
```

`RemoteAnimationTarget` 同时装着两类东西：跨 Parcel 复制的 taskId、bounds、insets、mode、windowConfiguration，以及 Launcher 可操作的 SurfaceControl 句柄。拿到句柄不意味着接管 Activity 生命周期；Launcher 只能在 leash 所界定的层级中写变换。

真正高频的数据面没有 `setProgress()` AIDL。Quickstep 在本地按手势进度计算 `SurfaceParams`；存在 `mSyncTransactionApplier` 时，`SurfaceTransactionApplier.scheduleApply()` 对齐 Launcher RenderThread frame，写 matrix、crop、alpha、corner radius 等属性并 `apply()`；applier 为 null 时，`TransformParams.applySurfaceParams()` 会在当前 caller 线程直接 apply transaction。所以 controller Binder 是低频控制面；逐帧性能问题应查 Launcher caller/UI、RT 与 Surface transaction，而不是查 system_server 每帧 Binder。

客户端 `RemoteAnimationTargets.release()` 释放的是 Launcher 这一侧的 SurfaceControl Java/native 引用。服务端 `SurfaceAnimator` reset/remove leash 是另一条生命周期，物理显示又是第三条。客户端 release 不能代替服务端 cleanup，服务端 cleanup 也不能证明某帧已 present。

终态还存在 live-tile 分支。普通 Home 或非 live-tile Overview 会最终调用 controller finish；启用 live tile 时，Overview 分支可只写 `STATE_CURRENT_TASK_FINISHED`，随后配置“下一次 transition 发生时 deferred cancel-with-screenshot”并保留 Recents controller；这次配置本身并不立即截图。下一次 `OtherActivityInputConsumer` 会 `continueRecentsAnimation()` 接回同一会话。日志里的“当前 Task 已完成”因此不能一概解释为服务端 Recents animation 已 finish。

## 8. 两条输入路径与四道接管门必须分开

Quickstep 同时使用两种不同输入设施：

| 设施 | 建立时机 | 作用 |
|---|---|---|
| `monitorGestureInput("swipe-up", displayId)` | 手势导航初始化 | 让 `TouchInteractionService` 观察导航手势并可 pilfer 当前 pointer stream |
| `recents_animation_input_consumer` | 用户解锁后预注册 | 手势进入动画后，在被控制 App 窗口附近建立一个可由 Launcher 接收的代理通道 |

预注册是为避免动画中途新建 consumer 导致既有 MotionEvent 链被取消。成功路径会让 WMS 创建 server/client `InputChannel`、向 InputManager 注册 server 端、建立 Input Consumer Surface，并把 client channel 传回 Launcher。默认 `registerInputConsumer()` 使用普通 `Choreographer.getInstance()`；只有显式传 true 才选择 SF Choreographer。r48 还把 create/destroy 硬编码到 `DEFAULT_DISPLAY`。

注册失败路径也不完整：`createInputConsumer()` 抛 `RemoteException` 后，wrapper 只记日志，不 return，仍尝试用未初始化的 `InputChannel` 构造 receiver。JNI `nativeInit()` 会再抛未被这里捕获的 `RuntimeException`，所以字段不会赋值，也不会通知 `registered=true`。客户端 registered 不是假阳性，但原始远端失败会升级成本地注册调用异常。

“已注册”距离“事件到达具体手势处理器”至少还有四道门：

1. **Channel 门**：Launcher 的 `InputEventReceiver` 与 WMS server channel 已存在；
2. **Controller 门**：`mInputConsumerEnabled=true`，且当前 Activity 不是 Home/Recents target、却属于被动画控制的 Task；
3. **InputWindow 门**：InputMonitor 的异步遍历找到首个匹配 App window，把 consumer show 在它上方一层并提交 InputWindow 信息；
4. **Listener 门**：`InputConsumerProxy.enable()` 已把本次终态对应的 listener 装到客户端 controller。

`BaseSwipeUpHandlerV2` 只有同时达到 APP_CONTROLLER_RECEIVED 与 GESTURE_STARTED 才调用 `enableInputConsumer()`。客户端先在 UI helper 隐藏 IME，再同步设置服务端开关。服务端 `updateInputWindowsLw(true)` 只是向 AnimationHandler post 更新；遍历产生的 input transaction 还要 merge 到 Display pending transaction并 schedule animation。同步 Binder 返回不保证 consumer Surface 已 show、InputDispatcher 已安装快照或新事件已经路由过来。

InputMonitor 每轮先 hide/reset 全部 consumers，再按窗口从顶到底遍历。对第一个满足条件的 App window，它复制该窗口的 focus 状态，却把 consumer touchable region 设为目标 Home/Recents main window bounds，而不是当前 App bounds；Surface relative layer 是匹配 App window 的 `+1`。没有目标 main window就不会 show。

Listener 门也不能省略。预注册 receiver 即便 listener 为 null，`InputConsumerController` 仍会结束输入事件；`InputConsumerProxy.enable()` 通常要等手势选择 Launcher 终态才安装真正代理。runner death 会额外销毁 WMS 侧 consumer，而普通动画 cleanup 只令当前 WMS引用失效并通过 input update 隐藏；客户端 receiver 的本地 registered 状态可能与服务端对象错开，后续需要重新注册才能恢复一致。

## 9. Controller 方法都是同步 Binder，调用线程与完成含义却各不相同

`IRecentsAnimationController` 没有 oneway 方法，但 Launcher wrapper 并未把所有调用统一搬到后台：

| 操作 | r48 Launcher 常见调用线 | Stub 返回最多证明什么 |
|---|---|---|
| `screenshotTask(taskId)` | MAIN 直接同步 Binder | 服务端已尝试为受控 Task 取 snapshot，并返回对象或 null |
| `setDeferCancelUntilNextTransition()` | MAIN 直接同步 Binder | 两个 defer boolean 已写入 controller |
| `removeTask(taskId)` | `onTaskAppeared` 的 MAIN listener 直接同步 Binder | 已返回本次 `isOnTop` 检查与移除结果 |
| `setAnimationTargetsBehindSystemBars()` | `UI_HELPER_EXECUTOR` | Task 标志已改并 request traversal |
| `hideCurrentInputMethod()`、`setInputConsumerEnabled()` | 同一个 helper task 依次同步调用 | hide 请求已交给本地 IME 服务；输入开关已写并排了窗口更新 |
| `cleanupScreenshot()` | `UI_HELPER_EXECUTOR` | screenshot animator 已被请求 cancel；后续 finish callback 可能已重入 |
| `finish()` | `UI_HELPER_EXECUTOR` | 服务端重排方法已返回，或 compat 已吞下 RemoteException |
| `setWillFinishToHome()` | 手势 MAIN 经 compat 直接同步调用 | failsafe 的目标 boolean 已写入 |

因此 screenshot、defer 和动态 target 移除都可能阻塞 Launcher 主线程。看到“controller 是同步接口”也不能反推所有动作都在 MAIN；必须读具体 wrapper。

截图接口只遍历 `mPendingAnimations`，不接受任意 taskId。匹配后先 `snapshotTasks()`，把 Task 加入 skip-closing 集合，再调用 `getSnapshot(taskId, 0, ...)`；这里 userId 硬编码为 0。服务端的 null 可以表示 canceled、task 不受控或 snapshot 不存在；compat 又把 null 与 `RemoteException` 都折叠成空 `ThumbnailData`。上层若只检查非 null，就无法区分这些失败来源。

系统栏方法也有一个命名反向：Launcher 的 `setUseLauncherSystemBarFlags(true)` 会向服务端传 `behindSystemBars=false`。服务端遍历非目标类型 Task，设置 `setCanAffectSystemUiFlags(behindSystemBars)` 并请求 traversal；cleanup 对每个 adapter 无条件恢复 true，而不是恢复进入会话前逐 Task 的旧值。

`setWillFinishToHome()` 只影响 failsafe 触发时选择 TOP 还是 ORIGINAL，不会直接 finish，也不会改变正常手势的终态。控制方法名只表示请求意图；想证明视觉结果，仍要继续追 traversal、pending merge、SF commit 和 present fence。

## 10. 动态 Task 走 onTaskAppeared，但建立顺序弱于初始 App Task

Recents controller 存在时，`Task.applyAnimationUnchecked()` 截获传统 AppTransition 动画。只有 `enter=true` 才调用 `addTaskToTargets()`；`enter=false` 也不会回落到 superclass。这意味着会话中 opening Task 可变成动态 remote target，而 closing Task 的普通动画也可能被抑制。

动态 opening 的服务端链是：

```text
Task.applyAnimationUnchecked(enter=true)
  → addTaskToTargets(task, transitionFinishedCallback)
  → addAnimation(hidden=true)
  → mPendingNewTaskTargets.add(taskId)
  → createRemoteAnimationTarget()
  → oneway runner.onTaskAppeared(target)
```

`hidden=true` 让新 target 在 Launcher 决定如何呈现前保持隐藏。问题是 `commitPendingTransaction()` 仍只 schedule animation；与初始 App Task 批次不同，这里没有固定的 after-placement 边界把“setup 已提交”放到 `onTaskAppeared` 之前。runner 可能先拿到 leash 句柄，相关 show/reparent/crop 还留在 Display pending transaction。能操作句柄不等于输入画面已经形成。

另一个不对称是 `mPendingNewTaskTargets.add(taskId)` 发生在 target null 检查之前。若 Task 没有 top visible main window，`createRemoteAnimationTarget()` 返回 null，runner 收不到 appeared，但 id 已进入“新 target”集合；没有就地 remove。若 oneway appeared 因 `RemoteException` 失败，adapter 也继续由 server 控制，直到全局收尾。

`removeTask()` 只在目标 Task 当前 `isOnTop()` 时成功。Launcher 的 `TaskAnimationManager` 只保留最后一个 appeared target；新 appeared 到来时，它尝试移除前一个，却忽略 boolean 返回值。失败 target 仍在 server pending 列表里，正常 finish 会再遍历新 id 尝试移除；仍不满足 isOnTop 的项最后依赖全局 cleanup。

这条动态链需要同时观察四本账：runner 已收到哪些 target、server pending adapters、pending-new ids、Launcher last-appeared 引用。任意一本都不能单独代表完整集合。客户端 `RemoteAnimationTargets.release()` 只显式 release 初始 `unfilteredApps` 与 wallpapers；替换旧 appeared 时只请求 server remove 后覆盖引用，管理器 cleanup 也只 release `mTargets`，再把 `mLastAppearedTaskTarget` 清 null。动态 compat target 没有显式 `release()`，其本地 SurfaceControl 句柄依赖引用消失后的 finalization；`GestureState` 还可能延长引用寿命。server remove/cleanup 不能替代这侧 release。

## 11. 正常 finish 先通知本地，再向服务端提交三种选择

客户端 `finishController(toRecents, callback, sendUserLeaveHint)` 的顺序是：

```text
mOnFinishedListener.accept(this)
  → RecentsAnimationCallbacks 把 finished listeners 投到 MAIN
UI_HELPER_EXECUTOR.execute(
  同步 compat.finish(toRecents, sendUserLeaveHint)
  → 若有callback，再投回MAIN)
```

也就是说，“客户端 finished 通知已发起”先于 Binder finish 排队。MAIN 上的 listener 与 helper 上的 Binder 请求随后并发推进，本地 `TaskAnimationManager.cleanUpRecentsAnimation()` 可以先释放 targets、移除 listeners、清当前字段。它不是服务端 cleanup 的确认。

可选 `onFinishComplete` 只在 compat `finish()` 返回后投 MAIN；但 compat 捕获 `RemoteException` 后也正常返回。因此它最多说明“客户端调用尝试已经返回”，不是 server callback、Surface cleanup 或 present 的不可失败确认。

server Stub 先在 WMS lock 下检查 `mCanceled` 并尝试移除所有 dynamic-new targets，然后释放该锁，才调用 `RecentsAnimation.onAnimationFinished()`。这段检查没有设置 one-shot finishing 标志：两个并发 finish 都可能先通过门，finish 与 cancel 也可能在门外竞跑。最终由先进入 `RecentsAnimation.finishAnimation()`、且仍看到 WMS controller 非 null 的调用完成 cleanup 与 reorder；后到者可能只看到 controller 已清而返回。Binder 入口先到不等于其 reorder mode 必胜。

获胜且无异常的 callback 会在 ATMS/WMS global lock 下注销 stack-order listener、清 caller running flag，并在 `inSurfaceTransaction()` 块中执行 controller cleanup 与 Task 重排。对这一固定成功调用，同步 Binder 保证这些 Java 路径在返回前已走到结尾；它仍没有把所有 Display pending transaction 自动变成已显示画面，失败或竞争败者的返回也不提供同样含义。

`toRecents` 只被映射成两个模式：true 为 TOP，false 为 ORIGINAL。KEEP 不是普通 Launcher finish 的第三个布尔值，而是 server cancel 路径使用的独立 reorder mode。

前一节提到的 live-tile Overview 是例外：它可以不调用 finish，只把当前 Task 的 UI 状态收束，再保留 controller 供下一次手势继续。必须结合 feature flag、end target 和 server controller 是否仍存在，才能解释一次手势结束后的会话寿命。

## 12. TOP、ORIGINAL、KEEP 改的是层级意图，不是物理呈现完成

三种模式的服务端结果并不对称：

| 模式 | hierarchy 动作 | visibility/transition 动作 | 特殊边界 |
|---|---|---|---|
| `REORDER_MOVE_TO_TOP` | 将目标 Task 或目标 stack 移到前台 | `TRANSIT_NONE`、ensure visible、resume、execute transition | `sendUserLeaveHint=true` 时设置 user-leaving，并走 `moveTaskToFront()` 以允许前 App 进入 PiP |
| `REORDER_MOVE_TO_ORIGINAL_POSITION` | 尝试把目标 stack 放回保存锚点之后 | 同样执行无动画 transition、可见性与 resume | 不恢复目标 Task 的旧 index；锚点无效时可 no-op |
| `REORDER_KEEP_IN_PLACE` | 不再移动 stack | 必要时只刷新 target 可见性，随后提前 return | 不执行后面的 transition/resume；常用于 stack-order cancel |

所有模式先调用 `WMS.cleanupRecentsAnimation()`：WMS 把全局 controller 字段置 null，controller 遍历 Task/wallpaper adapters，调用 captured SurfaceAnimator finish callback，并清 death link、failsafe、runner、输入窗口与状态栏通知。之后才查当前 target stack 和精确 Activity，再清 `mLaunchTaskBehind` 并执行模式分支。

顺序上的细节很重要：若 target Activity 已不在目标 stack，cleanup 已发生，但方法会在清 launch-behind 前 return。KEEP 若目标有效，则会清 launch-behind；当没有 deferred screenshot 且目标 stack 不是 focused stack 时，它可局部 ensure visible，然后直接结束，不进入公共 transition 逻辑。

Surface 侧至少要保留这条层级：

```text
captured finish callback
  → SurfaceAnimator reset/reparent/remove 写入 WindowContainer pending transaction
  → DisplayContent.prepareSurfaces merge pending
  → WMS关闭/应用本轮 global transaction，提交给SurfaceFlinger
  → SF apply/commit layer hierarchy
  → composition与HWC present fence
```

`inSurfaceTransaction()` 的词义不能吞掉中间层。adapter 回调拿到的仍可能是 Task/Display pending transaction。finally 先调用 `continueWindowLayout()`：`continueLayout()` 在有 layout changes 或 deferred requests 时会同步 `performSurfacePlacement()`；随后若 Root 仍 `isLayoutNeeded()`，代码还会再显式 placement。若两处都没有执行 placement，pending 只依赖已 schedule 的 animation traversal。因此同步 finish 返回可以证明获胜 callback 与 Java 层重排已走过，却不能普遍证明 pending 已 merge。

本章的 Recents Task/Wallpaper adapter 不请求 animation-finish defer，所以固定 cleanup 中 reset 写 pending 发生在 Binder 返回前；pending submit 与 Binder reply 谁先被外部观察到则没有通用保证，present 更晚。`SurfaceAnimator` 的通用机制允许 Animatable/AnimationAdapter 请求 defer，但那不是此处用来削弱偏序的默认条件。诊断画面残留时，要找 `prepareSurfaces`、transaction id 或 layer trace，而不是只找 finish log。

## 13. cancel、defer 与 1 秒 failsafe 是三套机制

非截图 cancel 的固定服务端顺序是：拿 WMS lock，若已 canceled 则 return；移除 failsafe；置 `mCanceled=true`；oneway 通知 runner `onAnimationCanceled(null)`；仍在锁内调用 ATMS finish callback，完成 controller cleanup 与指定 reorder。由于 runner 回调是 oneway，Launcher 实际处理 cancel 可以早于或晚于 server cleanup；跨进程没有“先看完 cancel UI，再拆 leash”的等待关系。

触发源则很多：初始化无 target、runner death-link 失败、Task adapter 被取消、新 start 替换旧 controller、runner death、显式 WMS cancel、stack order 变化和 failsafe。它们选择的 reorder mode 不完全相同，排障时不能只按 callback 名称归类。

stack-order 路径可由 Launcher 请求 defer：服务端先标记“下次 transition start/cancel 时继续”，仍处于注册状态的 AppTransition listener 命中后再调用普通 cancel 或截图 cancel。该 listener 在任意一次 transition starting/cancelled 回调开头都会先 unregister，然后才检查 `mCancelOnNextTransitionStart`；如果更早的无关 transition 已把它一次性消费，之后 stack-order change 只设 boolean、不会重新注册 listener，deferred cancel 可能再也等不到回调。defer boolean 只是推迟触发点，不会建立无限期自动恢复，也不等同于动画 timeout。

r48 的 1 秒 failsafe 也不是每场 Recents 初始化时自动启动的 watchdog。真实入口是 PhoneWindowManager 处理未被消费、非长按的 power-key up，向 WMS 发 `ANIMATION_FAILSAFE` 消息；WMS 发现当前 controller 后才调用 `scheduleFailsafe()`。一秒后根据 `mWillFinishToHome` 选择 TOP 或 ORIGINAL。若从未触发这条外部消息，就没有这只定时器。

Launcher 里的 `RecentsAnimationCallbacks.mCancelled` 还要与 server `mCanceled` 分开。前者只由本地 `notifyAnimationCanceled()` 设置；真正收到 server `onAnimationCanceled()` 时不会写它。它只用于“本地主动取消发生后，迟到 start callback 应立即 finish-to-app”的客户端门，不能泛化成所有 server cancel-before-start 的代际保护。

runner death 通常走 ORIGINAL cancel，随后销毁关联 input consumer。但第 14 节的成功截图 hold 已提前把 `mCanceled` 置 true；此时 death 回调里的 cancel 会直接 return，只剩 input consumer 销毁，不能完成 hold。

## 14. deferred screenshot 是一份必须显式归还的保活协议

截图取消不是“发回一张图然后照常 finish”，而是把 finish 拆成两阶段。AppTransition listener 到点后，服务端先从 `mPendingAnimations.get(0)` 取 Task；这里没有 empty guard，也没有证明它就是产品语义里的“当前卡片”。随后同步抓 snapshot，并建立 `TaskScreenshotAnimatable`：创建 screenshot Surface、挂入 buffer；旧式 `SurfaceControl.setMatrix()` 写 legacy global transaction，`show()` 写 Task pending transaction；独立 `SurfaceAnimator.transferAnimation()` 再接管原 Task animation。

这些 Surface 操作仍可能只在 pending transaction 中。服务端随后就 oneway 回调 `onAnimationCanceled(snapshot)`，所以 runner 收图不证明 screenshot Surface 已提交、latch 或替换了真实 Task 像素。AIDL 注释中的“已替换”应按协议目标理解，不能当成物理时间戳。

两条结果完全不同：

```text
snapshot == null
  → oneway cancel(null)
  → 同步 server finish(KEEP)

snapshot != null
  → oneway cancel(snapshot)
  → server保持controller与screenshot animator
  → runner调用cleanupScreenshot()
  → cancel screenshot animator
  → animator finish callback触发server finish(KEEP)
```

进入第二条路径前，server 已置 `mCanceled=true` 并移除 failsafe。此后普通 controller `finish()` 会因 canceled 而 no-op；runner 必须调用 `cleanupScreenshot()` 才能推进。若发送 snapshot 时发生 `RemoteException`、runner 随后死亡，或 runner 永远不 cleanup，本地没有新的 timeout 自动解套：death cancel 与再次 failsafe 都会被 canceled gate 吞掉。

非空 snapshot 也不证明 animation transfer 成功。source animator 没有 leash 时，`transferAnimation()` 直接 return；destination screenshot Surface 或 parent 为空时，它 cancel 尚未装入 animation 的 destination 后 return。`screenshotRecentTask()` 对这两种结果都不检查，仍把 snapshot 回给 runner。之后即使 runner 调 `cleanupScreenshot()`，空 animator 的 cancel 也不会触发 static finish callback，server 仍可能停在 hold。

客户端收到非空 snapshot 后，`TaskAnimationManager` 让 `BaseActivityInterface.switchRunningTaskViewToScreenshot()` 在 UI 真正切图完成后运行 cleanup runnable；该 runnable 才会做客户端清理并把 `cleanupScreenshot()` 排到 helper。若此时 `getCreatedActivity()==null`，r48 的实现直接 return，连传入 runnable 都不执行，于是客户端与 server 都可能停在 screenshot hold。

`cleanupScreenshot()` 内部的 `SurfaceAnimator.cancelAnimation()` 还有一处重入顺序：cancel 先 reset，再调用 animation finish callbacks，之后才把 leash remove 写入传入 transaction。screenshot animator 的 finish callback 会在这个中间点触发 Recents finish，所以“controller 已清理”甚至可能早于外层 cancel 记录 leash remove。

成功 cleanup 的服务端顺序应写成：client screenshot View 已切换；cleanup Binder 进入；reset 先记录 screenshot Surface 清理；animator finish callback 重入 Recents controller cleanup；外层 cancel 再记录 screenshot leash remove并 schedule pending transaction；最后 Stub 才能退出。释放 global lock 后，Binder reply 与 AnimationThread 的 merge/submit 没有固定的外部观察顺序；present 更晚。任何一个早期 callback 都不能独自证明最后一层。

## 15. 九组只读练习把会话完成点钉回 r48

以下命令只读源码，均可在 macOS Bash 3.2 或 Zsh 5.9 执行。每个 `rg -e` 都是独立证据点；命中后还要阅读相邻锁、线程切换与失败分支，不能只凭方法名下结论。

### 练习 1：核对两条 AIDL、入口权限与 capability 边界

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'oneway interface IRecentsAnimationRunner {' \
  -e 'void onAnimationStart(in IRecentsAnimationController controller,' \
  frameworks/base/core/java/android/view/IRecentsAnimationRunner.aidl
rg -n -F \
  -e 'interface IRecentsAnimationController {' \
  -e 'ActivityManager.TaskSnapshot screenshotTask(int taskId);' \
  -e 'boolean removeTask(int taskId);' \
  frameworks/base/core/java/android/view/IRecentsAnimationController.aidl
rg -n -F \
  -e 'enforceCallerIsRecentsOrHasPermission(MANAGE_ACTIVITY_STACKS, "startRecentsActivity()");' \
  -e 'final int callingPid = Binder.getCallingPid();' \
  -e 'final WindowProcessController caller = getProcessController(callingPid, callingUid);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
rg -n -F \
  -e 'return UserHandle.isSameApp(callingUid, mRecentsUid);' \
  frameworks/base/services/core/java/com/android/server/wm/RecentTasks.java
```

解释 runner 三个回调为什么不等远端执行、controller 调用为何会等待 Stub；再说明后续 Stub 没有逐方法权限检查，为什么仍不能把最初 caller pid 当成 runner Binder 宿主证明。

### 练习 2：重建 Launcher start 队列与无代际窗口

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (FeatureFlags.IS_STUDIO_BUILD) {' \
  -e 'finishRunningRecentsAnimation(false /* toHome */);' \
  -e '.startRecentsActivity(intent, null, mCallbacks, null, null));' \
  -e 'gestureState.setState(STATE_RECENTS_ANIMATION_INITIALIZED);' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/TaskAnimationManager.java
rg -n -F \
  -e 'mController = new RecentsAnimationController(animationController,' \
  -e 'Utilities.postAsyncCallback(MAIN_EXECUTOR.getHandler(), () -> {' \
  -e 'public void notifyAnimationCanceled() {' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/RecentsAnimationCallbacks.java
rg -n -F \
  -e 'ActivityTaskManager.getService().startRecentsActivity(intent, receiver, runner);' \
  -e 'if (resultCallback != null) {' \
  frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/ActivityManagerWrapper.java
```

画出 old controller 已存在时 MAIN 与 UI helper 的入队次序；再推演旧请求尚未回调、`mController==null` 时连续发两个 start，指出 initialized 为什么不能排除静默失败。

### 练习 3：验证目标 type、临时摆位与不完整恢复

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mTargetActivityType = targetIntent.getComponent() != null' \
  -e 'mRestoreTargetBehindStack = getStackAbove(targetStack);' \
  -e 'mDefaultTaskDisplayArea.moveStackBehindBottomMostVisibleStack(targetStack);' \
  -e 'targetStack.positionChildAtTop(task);' \
  -e 'targetActivity.mLaunchTaskBehind = true;' \
  -e 'mService.mRootWindowContainer.ensureActivitiesVisible(null, 0, PRESERVE_WINDOWS);' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimation.java
rg -n -F \
  -e 'taskDisplayArea.moveStackBehindStack(targetStack,' \
  -e 'targetActivity.mLaunchTaskBehind = false;' \
  -e 'if (targetActivity == null) {' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimation.java
rg -n -F \
  -e '.setCallingUid(mRecentsUid)' \
  -e '.setCallingPackage(mRecentsComponent.getPackageName())' \
  -e '.setUserId(mUserId)' \
  -e '.execute();' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimation.java
```

分别推演既有目标 Task 被提到 stack 顶、新建目标没有 restore anchor、finish 时目标 Activity 已移走三种情况，列出 ORIGINAL 能恢复和不能恢复的状态。

### 练习 4：追初始 App Task setup 与 wallpaper readiness

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'final ArrayList<Task> visibleTasks = mDisplayContent.getDefaultTaskDisplayArea()' \
  -e 'targetStack.forAllLeafTasks(c, true /* traverseTopToBottom */);' \
  -e 'if (config.tasksAreFloating()' \
  -e 'task.startAnimation(task.getPendingTransaction(), taskAdapter, hidden,' \
  -e 'task.commitPendingTransaction();' \
  -e 'mService.mWindowPlacerLocked.performSurfacePlacement();' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'mCapturedLeash = animationLeash;' \
  -e 'mCapturedFinishCallback = finishCallback;' \
  -e 'final int mode = topApp.getActivityType() == mTargetActivityType' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'wallpaperController.getWallpaperTarget() != null' \
  -e 'mPendingStart = false;' \
  -e 'mRunner.onAnimationStart(mController, appTargets, wallpaperTargets, contentInsets,' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'WALLPAPER_DRAW_PENDING_TIMEOUT_DURATION = 500;' \
  -e 'mService.getRecentsAnimationController().startAnimation();' \
  frameworks/base/services/core/java/com/android/server/wm/WallpaperController.java
rg -n -F \
  -e 'cancelAnimation(REORDER_MOVE_TO_ORIGINAL_POSITION, "initialize-noVisibleTasks");' \
  -e 'cancelAnimation(REORDER_MOVE_TO_ORIGINAL_POSITION, "initialize-failedToLinkToDeath");' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'mDefaultTaskDisplayArea.registerStackOrderChangedListener(this);' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimation.java
```

把 App Task adapter 捕获、schedule、surface placement submit、ready check、500ms wallpaper timeout、wallpaper adapter 创建、oneway start 七点排序；再说明 wallpaper setup 为何没有同样的 submit-before-callback 屏障，以及 initialize 内同步 cancel 为什么仍会留下迟到注册的 stack listener。

### 练习 5：逐道打开输入 consumer 的四扇门

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'monitorGestureInput("swipe-up",' \
  -e 'mInputConsumer.registerInputConsumer();' \
  packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/TouchInteractionService.java
rg -n -F \
  -e 'registerInputConsumer(false);' \
  -e 'mWindowManager.createInputConsumer(mToken, mName, DEFAULT_DISPLAY, inputChannel);' \
  -e 'mInputEventReceiver = new InputEventReceiver(inputChannel, Looper.myLooper(),' \
  -e 'mWindowManager.destroyInputConsumer(mName, DEFAULT_DISPLAY);' \
  -e 'withSfVsync ? Choreographer.getSfInstance() : Choreographer.getInstance());' \
  frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/InputConsumerController.java
rg -n -F \
  -e 'jniThrowRuntimeException(env, "InputChannel is not initialized.");' \
  frameworks/base/core/jni/android_view_InputEventReceiver.cpp
rg -n -F \
  -e 'mController.hideCurrentInputMethod();' \
  -e 'mController.setInputConsumerEnabled(true);' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/RecentsAnimationController.java
rg -n -F \
  -e 'mHandler.post(mUpdateInputWindows);' \
  -e 'mDisplayContent.getPendingTransaction().merge(mInputTransaction);' \
  -e 'mRecentsAnimationInputConsumer.show(mInputTransaction, w);' \
  frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java
rg -n -F \
  -e 't.setRelativeLayer(mInputSurface, w.getSurfaceControl(), 1);' \
  frameworks/base/services/core/java/com/android/server/wm/InputConsumerImpl.java
rg -n -F \
  -e 'mInputConsumerController.setInputListener(this::onInputConsumerEvent);' \
  packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/InputConsumerProxy.java
```

分别标记 gesture monitor、client registered、server channel、server enabled、InputWindow submitted、proxy listener installed；解释 create 抛 RemoteException 后为何会继续走到 JNI RuntimeException，以及同步 enable 返回时后面哪三层仍可能没完成。

### 练习 6：建立 controller 调用线程与结果折叠矩阵

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'return mController.screenshotTask(taskId);' \
  -e 'mController.setDeferCancelUntilNextTransition(defer, screenshot);' \
  -e 'return mController.removeTask(target.taskId);' \
  -e 'mController.setAnimationTargetsBehindSystemBars(!useLauncherSysBarFlags);' \
  -e 'UI_HELPER_EXECUTOR.execute(() -> mController.cleanupScreenshot());' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/RecentsAnimationController.java
rg -n -F \
  -e 'return snapshot != null ? new ThumbnailData(snapshot) : new ThumbnailData();' \
  -e 'return new ThumbnailData();' \
  -e 'return false;' \
  frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RecentsAnimationControllerCompat.java
rg -n -F \
  -e 'return snapshotController.getSnapshot(taskId, 0 /* userId */,' \
  -e 'task.setCanAffectSystemUiFlags(behindSystemBars);' \
  -e 'taskAdapter.mTask.setCanAffectSystemUiFlags(true);' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
```

给每个 wrapper 标出调用线程，再列出 screenshot 空结果、RemoteException 和 remove false 在 compat 层如何折叠；说明 system-bar cleanup 为什么不是逐 Task 旧值恢复。

### 练习 7：推演 dynamic target 的添加、移除与客户端持有

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'final RecentsAnimationController control = mWmService.getRecentsAnimationController();' \
  -e 'if (control != null) {' \
  -e 'if (enter) {' \
  -e 'control.addTaskToTargets(this, (type, anim) -> {' \
  frameworks/base/services/core/java/com/android/server/wm/Task.java
rg -n -F \
  -e 'true /* hidden */, finishedCallback);' \
  -e 'mPendingNewTaskTargets.add(task.mTaskId);' \
  -e 'mRunner.onTaskAppeared(target);' \
  -e 'target.mTask.isOnTop()' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'mController.removeTaskTarget(mLastAppearedTaskTarget);' \
  -e 'mLastAppearedTaskTarget = appearedTaskTarget;' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/TaskAnimationManager.java
rg -n -F \
  -e 'for (RemoteAnimationTargetCompat target : unfilteredApps) {' \
  -e 'for (RemoteAnimationTargetCompat target : wallpapers) {' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/RemoteAnimationTargets.java
```

构造 target 创建返回 null、appeared oneway 失败、旧 appeared Task 不在 top 三条路径，分别更新 server adapter、new-id、runner seen、client last-target 四本账；再检查动态 target 是否进入初始数组的 release 循环。

### 练习 8：区分客户端 finish、三种重排与 Surface 提交

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mOnFinishedListener.accept(this);' \
  -e 'mController.finish(toRecents, sendUserLeaveHint);' \
  -e 'MAIN_EXECUTOR.execute(callback);' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/RecentsAnimationController.java
rg -n -F \
  -e 'mCallbacks.onAnimationFinished(moveHomeToTop' \
  -e '? REORDER_MOVE_TO_TOP' \
  -e ': REORDER_MOVE_TO_ORIGINAL_POSITION, sendUserLeaveHint);' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'mWindowManager.inSurfaceTransaction(() -> {' \
  -e 'mWindowManager.cleanupRecentsAnimation(reorderMode);' \
  -e 'targetStack.moveTaskToFront(targetActivity.getTask(),' \
  -e 'taskDisplayArea.moveStackBehindStack(targetStack,' \
  -e 'if (mWindowManager.mRoot.isLayoutNeeded()) {' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimation.java
rg -n -F \
  -e 'reset(mAnimatable.getPendingTransaction(), true /* destroyLeash */);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
rg -n -F \
  -e 'SurfaceControl.mergeToGlobalTransaction(transaction);' \
  frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
rg -n -F \
  -e 'if (hasChanges || mDeferredRequests > 0) {' \
  -e 'performSurfacePlacement();' \
  frameworks/base/services/core/java/com/android/server/wm/WindowSurfacePlacer.java
rg -n -F \
  -e 'commitTransaction();' \
  frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
rg -n -F \
  -e 'result.presentFence = hwc.getPresentFence(*mId);' \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/Display.cpp
```

为“本地 finished 已投递”“同步 Binder 已返回”“reset 已写 pending”“pending 已 merge”“SF 已 commit”“HWC 已 present”各写一个独立观察条件，并解释为何前三者不能替代后三者。

### 练习 9：闭合 cancel、power failsafe 与 screenshot hold

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'final Task task = mPendingAnimations.get(0).mTask;' \
  -e 'mRunner.onAnimationCanceled(taskSnapshot);' \
  -e 'mRecentScreenshotAnimator.transferAnimation(task.mSurfaceAnimator);' \
  -e 'mRecentScreenshotAnimator.cancelAnimation();' \
  -e 'cancelAnimation(REORDER_MOVE_TO_ORIGINAL_POSITION, "binderDied");' \
  -e 'mService.mH.removeCallbacks(mFailsafeRunnable);' \
  -e 'mDisplayContent.mAppTransition.unregisterListener(this);' \
  -e 'if (mCancelOnNextTransitionStart) {' \
  -e 'mCancelOnNextTransitionStart = true;' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
rg -n -F \
  -e 'mHandler.post(mWindowManagerFuncs::triggerAnimationFailsafe);' \
  frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
rg -n -F \
  -e 'mRecentsAnimationController.scheduleFailsafe();' \
  frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F \
  -e 'activityInterface.switchRunningTaskViewToScreenshot(thumbnailData,' \
  -e 'mController.cleanupScreenshot();' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/TaskAnimationManager.java
rg -n -F \
  -e 'ACTIVITY_TYPE activity = getCreatedActivity();' \
  -e 'if (activity == null) {' \
  -e 'recentsView.switchToScreenshot(thumbnailData, runnable);' \
  packages/apps/Launcher3/quickstep/src/com/android/quickstep/BaseActivityInterface.java
rg -n -F \
  -e 'if (from.mLeash == null) {' \
  -e 'if (surface == null || parent == null) {' \
  -e 'if (animation != null) {' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
```

先说明无关 transition 如何提前消费一次性 listener；再推演 snapshot null、snapshot non-null 后正常 cleanup、transfer 早退、回调发送失败、runner 随后死亡、Launcher Activity 为 null 六条路径，对每条标明谁还能触发最终 server finish，以及哪条没有本地 timeout。

## 16. 用会话矩阵收口，并把逐帧几何留给第 225 章

| 现象 | 第一证据点 | 常见误判 | 下一步 |
|---|---|---|---|
| 手势状态长期只有 initialized | ATMS 是否收到 start、runner 是否有 callback | initialized 等于 server 已接受 | 查 UI helper 异常与 null result callback |
| 新手势拿到旧 targets | 两次 start 的 Binder 句柄与 callback 时刻 | manager 字段天然有 generation | 按请求代际重建 MAIN/helper 队列 |
| 新会话运行但 caller 没有 Recents 标志 | WPC boolean 的 true/false 更新 | 每个 controller 都独立引用计数 | 查同 caller 替换时旧 cleanup 写 false |
| ORIGINAL 后 Task 顺序变化 | stack restore anchor 与 Task index | ORIGINAL 是完整回滚 | 查启动阶段 `positionChildAtTop()` |
| runner 收到 start，首帧却仍不对 | target 类型、setup submit 与 SF latch | 所有初始 target 都有 callback 前提交屏障 | 分开查 App Task 与 wallpaper transaction |
| 输入 consumer 显示 registered 却无事件 | 四道门、目标 main window、listener | 注册就等于接管 | 分别查 server enabled、InputWindow 与 proxy |
| enable 返回后首个事件仍给 App | InputMonitor Handler 与 transaction submit | 同步 Binder 会同步刷新 InputDispatcher | 对齐 input-window 更新帧和既有 touch focus |
| 动态 Task callback 后黑一下 | hidden leash setup 是否 merge | appeared 保证 Surface 已 show | 查 dynamic add 的 pending transaction |
| 旧动态 target 未移除 | `isOnTop()` 返回值与 server pending list | client 调 remove 就一定成功 | 记录 boolean，并等全局 cleanup |
| 客户端 finished 后 server 仍 active | local listener 与 helper Binder 队列 | 本地 cleanup 是远端确认 | 查 compat RemoteException 与 ATMS finish |
| finish 返回但 leash 仍在 trace 中 | SurfaceAnimator pending 与下一次 placement | 同步返回等于 remove 已提交 | 查 pending merge 和 SF hierarchy commit |
| power key 后才出现 1 秒 cancel | `ANIMATION_FAILSAFE` 消息 | controller 初始化自带 timeout | 还原 power-key up 触发条件 |
| cancel(null) UI 与 server cleanup 顺序漂移 | runner oneway 与 cancel 锁内 callback | runner 一定先收尾 UI | 分别采 Launcher Binder 与 WMS trace |
| 非空 snapshot 后永久卡住 | canceled、screenshot animator、cleanup call | runner death 会自动 finish | 查 Activity-null runnable、Binder death 和 failsafe gate |
| client Surface 引用数量增长 | 初始数组 release 与 appeared target 引用 | server remove 会释放 client handle | 单独审计动态 target 的 release 所有权 |

推荐按六条执行线读 trace：Launcher MAIN 负责 listener、手势状态、本地清理与部分直接 apply；UI helper 负责 start、finish 与部分控制调用；Launcher Binder 线程负责 target 包装；有 applier 时 Launcher RT 负责对齐帧的 transaction apply；system_server Binder/global-lock 路径负责 controller 与 Task 层级；WMS AnimationThread、SurfaceFlinger 和 HWC 负责 pending merge 到 present。把这些线折成一条“动画线程”，几乎一定会把因果关系写反。

本章最终可保留五条不变量：

1. initialized、started、controller finished、server cleaned、frame presented 是五个不同事实；
2. runner 通知是 oneway，controller 控制是同步 Binder，但同步返回不越过异步 transaction；
3. 初始 App Task 有固定成功路径的 setup-before-callback；wallpaper 与 dynamic target 都没有同样强的提交边界；
4. TOP、ORIGINAL、KEEP 选择 Task/stack 收口策略，ORIGINAL 不承诺还原所有临时改动；
5. 非空 screenshot cancel 把 liveness 责任交给 runner，`cleanupScreenshot()` 是协议必需项，不是可选释放优化。

第 225 章将在这套控制边界之上继续追 `TransformParams`、`TaskViewSimulator`、matrix/crop/corner-radius、orientation 与 `SurfaceTransactionApplier`：那里回答的是“Launcher 怎样计算并提交每一帧”，而本章回答的是“哪些 Surface 可控、输入何时改道、会话怎样真正结束”。
