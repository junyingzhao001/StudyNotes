# 223 Android RemoteAnimationAdapter、RemoteAnimationController与Launcher/Recents远程转场

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只追普通旧版 AppTransition 的 Remote Animation：Launcher/SystemUI 怎样取得临时 animation leash、远端怎样逐帧写 Surface 属性、system_server 又怎样收回控制权。交互式 Recents Animation 只在末尾划边界，第 224 章再展开输入接管与 Task 重排。

第 222 章的本地路径由 `SurfaceAnimationRunner` 解释传统 `Animation`；远程路径仍复用 `SurfaceAnimator` 建 leash，却让另一个进程决定 matrix、alpha、crop 与圆角。真正困难的不是“Binder 回调一次”，而是区分六类完成：服务端已建 leash、setup transaction 已提交、远端已收到 target、远端决定 finish、WMS 恢复层级的 pending transaction 已提交，以及像素最终 present。

## 1. 固定一次 Launcher 点图标链，先给十五个观察点命名

先固定场景 `R_launch`：集成 Quickstep 的 Launcher 具有 `CONTROL_REMOTE_APP_TRANSITION_ANIMATIONS`；用户从桌面点图标，Launcher 通过 `ActivityOptions.makeRemoteAnimation()` 启动普通 App；Display 已有 pending transition；没有 change、snapshot、单对象 start delay、取消或 finish defer；App target 有 Task 和 main window；runner 存活并正常设置 `AnimatorSet`、最终调用 finished callback。

| 点 | 源码侧含义 | 仍不能推出 |
|---|---|---|
| `I_pack` | Launcher 把 runner、duration、status-bar delay 装进 `ActivityOptions` | system_server 已接受权限 |
| `I_stamp` | `SafeActivityOptions` 验权，并给服务端 adapter 写入请求 caller 的 pid/uid | runner Binder 一定由该 pid 托管 |
| `O_remote` | `ActivityRecord.applyOptionsLocked()` 建立本批 `RemoteAnimationController` | 任一 leash 已存在 |
| `R_record` | 每个动画 target 建立一个 `RemoteAnimationRecord` 与主 wrapper | wrapper 已收到 `startAnimation()` |
| `L_capture` | `SurfaceAnimator` 建 leash，wrapper 写初态并截获 leash/finish callback/type | setup transaction 已提交 |
| `G_ready` | `AppTransition.goodToGo()` 调入 controller | 远端已经收到回调 |
| `T_build` | controller 把可用 record 转成 app targets，并另建 wallpaper targets | Binder 事务已经发送 |
| `S_submit` | `WindowAnimator` 将 Display pending transaction merge 后关闭本轮全局 transaction | SurfaceFlinger 已 latch |
| `B_enqueue` | after-prepare runnable 完成 death link，并发出 oneway `onAnimationStart()` | Launcher Binder Stub 已执行 |
| `U_create` | Launcher Binder 入口转换 target，再由 UI Handler 调 `onCreateAnimation()` | 任一远端 transaction 已 apply |
| `F_remote` | Launcher 某一 RT frame callback 对 leash 调用 `Transaction.apply()` | 该帧已经 present |
| `A_end` | Launcher `AnimatorSet` 结束，`AnimationResult.finish()` 发起同步 finished Binder | WMS 恢复 transaction 已提交 |
| `H_reset` | captured callback 通过 adapter 代际检查，清状态并把 reparent/remove 写入 Display pending transaction | pending 写入已经 merge |
| `W_submit` | 后续 `DisplayContent.prepareSurfaces()` merge pending，再关闭 WMS transaction | HWC 已 present |
| `P_present` | 对应 buffer 与层级状态被显示硬件实际呈现 | 可由前述 Java callback 单独证明 |

固定场景可证明的主干是：

```text
I_pack < I_stamp < O_remote < R_record < L_capture < G_ready < T_build
T_build < S_submit < B_enqueue < U_create
U_create < 某些F_remote
U_create < A_end < H_reset < finished Binder返回
H_reset < W_submit < P_present
```

最后三段只在固定的“不 defer、不失败”前提下成立。Animator 的终值更新可能只是排进 RT callback，所以不能把“最后一次 `F_remote` 已 apply”机械放在 `A_end` 之前；`P_present` 更需要 SurfaceFlinger/HWC fence，本文的 Java 点都不是它的替代品。

## 2. 七个对象、四条执行线与三类 transaction 必须分账

这条链中至少有七种不同载体：

| 对象 | 保存什么 | 不负责什么 |
|---|---|---|
| `RemoteAnimationAdapter` | runner Binder、duration、status-bar delay、change snapshot 请求及服务端补写的 caller 身份 | 不逐帧改 Surface |
| `RemoteAnimationDefinition` | 单个 transit 到 adapter+activity-type filter 的映射 | 不代表一次正在运行的动画 |
| `RemoteAnimationController` | 本批 records、wallpaper adapters、timeout、death link 与 finished stub | 不创建 Launcher 的 Animator |
| `RemoteAnimationRecord` | 一个 target 的主 wrapper、可选 thumbnail wrapper、start bounds 与输出 target | 不拥有真实窗口生命周期 |
| `RemoteAnimationAdapterWrapper` | 初始几何及截获的 leash、服务端 finish callback、animation type | 不调用本地 runner |
| `RemoteAnimationTarget` | 跨 Parcel 的几何/配置值和可操作的 SurfaceControl 句柄 | 不转移 Activity/Task 所有权 |
| Launcher runner/compat | Binder 适配、UI Animator、RT transaction 与客户端句柄释放 | 不负责服务端最终 reparent/remove |

执行线也不是简单的“WMS 线程对 Launcher 线程”：

1. Activity 启动 Binder 路径保存 caller 身份，在 ATMS/WMS global lock 下应用 options、选择 target并建立 wrapper；
2. `WindowAnimator` 使用 `AnimationThread` 上的 SF Choreographer，在锁内 prepare surfaces、merge/close setup transaction，随后执行 after-prepare runnable；
3. Launcher 的 Binder 线程先接收并包装 targets，`LauncherAnimationRunner` 再把动画创建投到 Launcher UI Handler；逐帧 Surface 写入可继续对齐 Launcher RenderThread frame；
4. Launcher UI 线程调用的 finished callback 是同步 Binder，system_server Binder 线程会取得 WMS lock做 controller 收尾，调用返回前不代表后续 Display pending transaction 已提交。

三类 transaction 分别是：WMS 开始阶段的 Display pending transaction、Launcher 每帧新建并 apply 的 transaction、WMS 完成阶段重新写入的 Display pending transaction。controller 虽在 finish 中打开/关闭 legacy global transaction，captured `SurfaceAnimator` callback 实际拿的是 animatable pending transaction；两者不能因代码嵌套在同一个 `try` 中就合并成一笔。

## 3. Adapter 是配置，Definition 是索引；两条 AIDL 的同步方向相反

`RemoteAnimationAdapter` 的四个 Parcel 字段是：

| 字段 | 服务端消费方式 |
|---|---|
| runner | `IRemoteAnimationRunner` 的 Binder 端点 |
| duration | wrapper 的近似 duration hint；WMS 不据此替远端插值 |
| status-bar delay | wrapper 被查询时返回 `uptimeMillis()+delay` |
| change-needs-snapshot | 有 start bounds 时是否额外建立 thumbnail wrapper |

`mCallingPid/mCallingUid` 没写进 Parcel。外部进程即使在发送前改过这两个字段，接收侧重建的 adapter 仍从默认值开始；可信入口必须用 Binder caller 重新补写。

`RemoteAnimationDefinition` 则是 `SparseArray<RemoteAnimationAdapterEntry>`。同一个 transit 再 add 一次会覆盖前一项，不会保留多个 filter 候选；`ACTIVITY_TYPE_UNDEFINED` 表示无过滤，否则只要本批 activity-type 集合包含指定类型就匹配。

协议方向尤其容易写反：

```text
system_server -- oneway onAnimationStart(apps, wallpapers, finished) --> runner
system_server -- oneway onAnimationCancelled() ----------------------> runner
Launcher      -- 同步 onAnimationFinished() -------------------------> system_server
```

所以 start/cancel 的代理返回只说明 Binder oneway 事务已提交，不说明 Launcher 方法已执行；而 Launcher 的 finished 调用会等服务端 Stub 返回。同步返回的上界仍只是 controller 方法结束，不能越级成为 WMS pending transaction submit、SF latch 或 HWC present 的证明。

duration、status-bar delay 与 controller timeout也是三本账。timeout 固定基数为 2000ms，再乘 `getCurrentAnimatorScale()`；它不读取 adapter duration。AppTransition 只从 top opening Activity 的 animating container读取 duration hint 与 status-bar time；若那里没有 adapter，就传 0 与当前 uptime，并仍使用固定 120ms 的状态栏 transition duration。status-bar delay 不随 animator scale缩放，也没有非负校验。

## 4. 五条入口的权限、身份与寿命并不等价

r48 的权限定义是 `signature|privileged`，AOSP 还通过 privapp allowlist授予 Launcher3。它不是普通三方 App 可用的公开窗口捕获能力。

| 入口 | 身份如何补写 | 作用域与尖角 |
|---|---|---|
| `ActivityOptions.makeRemoteAnimation()` | `SafeActivityOptions` 用创建 wrapper 时保存的 original/real Binder caller | 同 options 一起启动；还需 `ANIM_REMOTE_ANIMATION` 才会进入 `ActivityRecord` 的 remote case |
| `Activity.registerRemoteAnimations(definition)` | ATMS 验权后给 Definition 内所有 adapter 写当前 caller | 存到 token 对应 ActivityRecord；token 无效直接返回 |
| `registerRemoteAnimationsForDisplay()` | ATMS 验权并写 caller | 存到 Display 的 AppTransitionController；没有 ActivityRecord 那条长期 death link |
| `registerRemoteAnimationForNextActivityStart()` | ATMS 在登记时给 adapter 写 privileged registrar | 以 packageName 存 3 秒，命中 callingPackage 后一次性移除 |
| `IWindowManager.overridePendingAppTransitionRemote()` | WMS 只验权和 Display，未补 pid/uid | 外部 Parcel 又不携带身份，普通跨进程直调会把 pid=0 留到 controller 硬异常 |

next-start registry 在 `SafeActivityOptions` 处理之后注入。若原 options 为 null，它用 `makeRemoteAnimation()` 同时设置 adapter 与 animation type；若原 options 非 null，它只调用 `setRemoteAnimationAdapter()`，不会把既有 type 改成 remote。于是一个 basic/custom options 可消费掉登记项，却在 `ActivityRecord` switch 中不走 remote case。这不是权限绕过：登记 adapter 的权限与身份已在更早的 privileged 调用中完成。

`SafeActivityOptions` 的 system_server-pid 分支也不是拒绝请求：它只 `Slog.wtf()` 后 return，不抛异常、不终止 Activity start。若 adapter 没有旧身份，pid 仍可能是 0，稍后 `setRunningRemoteAnimation(true)` 才抛 `RuntimeException`。

最后，代码记录的是“发起注册/启动请求的 Binder caller”，并未反查 `runner.asBinder()` 的真实宿主。通常 Launcher 同时是 registrar 与 runner 进程，但若受信任 broker 转交另一个进程的 Binder，OOM 标志仍落在请求 caller 上，不能把 pid/uid 写成经过认证的 runner-owner 身份。

## 5. Definition 的选择发生在最终 transit 上，死亡清理却没有代际

AppTransitionController 先清参与者动画标志、调整 wallpaper，再把原 transit 改写为 translucent/wallpaper 版本；随后才收集 opening、closing、changing 的 activity types并选择 `animLpActivity`。因此 Definition 的 key 匹配的是这时的最终 transit，不是最早 `prepareAppTransition()` 传入值。

选择顺序可压成：

```text
在三组参与者中找“自身Definition匹配”的最高prefix Activity
  → 若没有，再找 fillsParent 且有main window的最高prefix Activity
  → 若仍没有，再找任意有main window的最高prefix Activity
得到 animLpActivity
  → 先查该Activity自身Definition
  → 未命中才查Display Definition
```

第一轮谓词不检查 Display Definition；而 `overrideWithRemoteAnimationIfSet()` 在 `animLpActivity==null` 时直接返回，所以“Display 级存储不依赖 ActivityRecord”不等于“没有 animLpActivity 也能应用”。源码注释称 remote animations always win，实际可依赖的是上述 filter 与最高 prefix 选择，不应再扩写成任意安全分支都可被覆盖。

`TRANSIT_CRASHING_ACTIVITY_CLOSE` 只让这次 Definition 安装提前返回。它不会主动清掉此前由 ActivityOptions 创建的 remote controller；只要 controller 仍存在且 target 未 start-delayed，后面的 `WindowContainer` 仍优先走 remote。因此“crash 高于 Definition”不能改写成“crash 全局禁止一切 remote”。

长期死亡清理还有三条 r48 边界：

- ActivityRecord 保存 Definition 后，对其中 runner 逐个 `linkToDeath(this::unregisterRemoteAnimations)`；任一 link 抛异常时，Definition 仍已保存，后续 runner 也不会继续 link；
- 替换或显式 unregister 只是给字段重新赋值，没有 unlink，也没有 Definition generation；旧 runner 以后死亡，旧 recipient 仍可能把新 Definition 一并清空；
- Display Definition 只是字段赋值，没有上述长期 death link。真正一场动画会在 after-prepare 阶段由 `RemoteAnimationController` 再建立独立的当前场 death link。

## 6. ActivityOptions 可先建 controller，稍后的 Definition 仍能再覆盖它

ActivityOptions 的 remote case 最终调用 `AppTransition.overridePendingAppTransitionRemote()`。该方法只在 `isTransitionSet()` 时生效，然后 `clear()` 旧 override、把 type 设为 `NEXT_TRANSIT_TYPE_REMOTE` 并新建 controller；没有 pending transition 时，adapter 不会排队等待下一批。

普通 custom/thumbnail 等 override 都受 `canOverridePendingAppTransition()` 约束：一旦 type 已是 remote，它们不能覆盖。remote 方法本身却没有“已有 remote”门，所以稍后从 Activity/Display Definition 选出的 adapter 可以再次 clear 并替换 ActivityOptions controller。此时旧 controller 尚未收集 record是常态；不能把“remote 优先”误读成“第一只 remote adapter 永远赢”。

`AppTransition.clear()` 只把 controller 字段置 null，不等于取消 SurfaceAnimator。正常 ready 路径中，controller 已被 timeout、after-prepare runnable、death recipient 或 finished stub继续引用，所以 `goodToGo()` 后紧接的 clear 不会截断本场。反过来，transition 在 good-to-go 之前被 `freeze()` 时必须先显式 `cancelAnimation("freeze")`，再 clear，否则尚未安装 finished/timeout 的 records 可能失去收口入口。

AppTransition 对 listener 的 starting 通知先于 `RemoteAnimationController.goodToGo()`。它查询 top opening adapter的 duration/status-bar time后，controller 才启动自己的 timeout、构造 targets；状态栏计划时间不是 runner 实际收到 `onAnimationStart()` 的时间戳。

## 7. WindowContainer 先选 remote wrapper；change snapshot 请求还跨着一条空指针缝

第 222 章已经核准 `getAnimationAdapter()` 的优先级：

```text
controller存在 && 当前SurfaceAnimator未start-delayed
  → RemoteAnimationRecord/主wrapper
否则若 change && enter && 仍在changing集合
  → 本地WindowChangeAnimationSpec
否则
  → 普通本地Animation
```

所以单对象 start delay 与 remote 不兼容：flag 已置起时，不是“remote 晚点开始”，而是直接跳过 remote 分支、继续选择 change/local。

每次 `createRemoteAnimationRecord()` 都先建立主 wrapper并加入 controller pending list。只有传入 start bounds且 adapter 的 `getChangeNeedsSnapshot()` 为 true，record 才另建 thumbnail wrapper。但这个条件不核对 `SurfaceFreezer.mSnapshot`：`captureLayers()` 可能返回 null buffer，或得到宽/高不大于 1 的 buffer，从而根本不建 snapshot；`WindowContainer.applyAnimationUnchecked()` 却只看 thumbnail wrapper 非空，随后无 guard 解引用 `mSnapshot.startAnimation()`。当前组合会在 good-to-go 前触发空指针，而不是静默退化成无 thumbnail 的主 remote 动画。

两条 leash 的启动也不同：

| 对象 | 启动桥 | 生命周期拥有者 |
|---|---|---|
| 主结束态 Surface | `WindowContainer.startAnimation()` → `SurfaceAnimator` → remote wrapper | SurfaceAnimator 拥有主 leash 与代际 callback |
| 开始态 snapshot Surface | `SurfaceFreezer.Snapshot.startAnimation()` 直接调用 remote wrapper | Snapshot/SurfaceFreezer 持有 Surface 与取消/销毁 |

thumbnail 不会再套一层普通 `SurfaceAnimator`。平台 adapter 能请求 snapshot；r48 SystemUI shared 的 `RemoteAnimationAdapterCompat` 只调用三参构造，固定 `changeNeedsSnapshot=false`，Launcher 常用 compat 路径不会进入这条平台能力。

## 8. Wrapper 不播放帧，只写等待期初态并截获三件东西

`RemoteAnimationAdapterWrapper.startAnimation()` 不调用 `SurfaceAnimationRunner`。它先把远端接手前的 position/crop 写进调用者 transaction，再保存：

```text
mCapturedLeash
mCapturedFinishCallback
mAnimationType
```

初态分支必须按 record 而不是 wrapper 构造参数判断：

| record 状态 | 主 wrapper | thumbnail wrapper |
|---|---|---|
| `mStartBounds != null` | position=start left/top；crop=start width/height | 同样写 start left/top 与 start size |
| `mStartBounds == null` | position=`mPosition`；crop=`mStackBounds` size | 实际不会为普通 open/close 创建 thumbnail |

thumbnail 构造时确实传了 `Point(0,0)` 和归零的旧尺寸 rect，但只要共享 record 有 start bounds，`startAnimation()` 就不会读取这两个字段。只有 start bounds 本身从 `(0,0)` 开始时，测试现象才会碰巧像“thumbnail 固定从原点启动”。

主 wrapper 捕获的是 `SurfaceAnimator` inner finish callback；thumbnail wrapper 捕获的是 `Snapshot.startAnimation()` 传入的 callback，在本调用点只是空实现。正常主 leash reset 会触发 `onAnimationLeashLost()` → `unfreeze()`，后者取消并销毁仍存的 snapshot。因此 `target.leash` 与 `target.startLeash` 虽都交给远端，服务端清理账并不对称。

wrapper 的 `getDurationHint()` 只是返回 adapter duration；`getStatusBarTransitionsStartTime()` 每次查询都以“当前 uptime + delay”重算。wrapper cancellation 则先把 record 中对应字段置 null，只有主与 thumbnail 都为 null才移除 record，全局 pending app record 为空才取消整场。

## 9. Target 创建门只检查主 wrapper，失败收口依赖更强的不变量

`RemoteAnimationRecord.createRemoteAnimationTarget()` 只要求：主 wrapper 仍存在、主 captured finish callback 非 null、主 captured leash 非 null。随后让具体 `WindowContainer` 创建 target；ActivityRecord 还要求 Task 与 main window，Task 则委托 top Activity。基类或不支持的容器可以返回 null。

mode 的判断顺序是：top Activity 属于 opening 就是 `MODE_OPENING`；否则当前容器在 changing 集合才是 `MODE_CHANGING`；再否则兜底为 `MODE_CLOSING`。opening 因此遮住同一对象可能同时满足的 changing 条件；closing 是参与者收集后的兜底，不是任意陌生容器的自动分类。

`createAppAnimations()` 逆序扫描 records。target 为 null 时，它会回放已经捕获的 callbacks并移除该 record，而不是让 SurfaceAnimator 永久等待；全部 record 都被剔除后，controller 不会启动 wallpaper-only remote animation。

但这套 helper 依赖“有 thumbnail 时主 wrapper 不会先独立消失”的更强不变量。record 只有双 null才移除，而 target gate只认主 wrapper；若状态变成“主 null、thumbnail 仍存”，无论 invalid-target 清理还是 controller finish，源码都会为 thumbnail 读取 `wrappers.mAdapter.mAnimationType`，从而空指针。常规 WindowContainer 主取消会经 leash-lost/unfreeze 先取消 snapshot，通常维持该耦合；controller 自身并没有把不变量编码成安全 null guard，不能把单 wrapper cancellation描述成完全对称、任意顺序都可靠的状态机。

wallpaper cancellation 是另一套策略：它只从 pending wallpaper list移除对应 adapter，并明确让 app remote animation继续。App target 数组顺序来自 pending list逆序，不是 Z-order 契约；远端需要使用 `prefixOrderIndex`，不能用数组下标猜层级。

## 10. RemoteAnimationTarget 混合了坐标值与 Surface 句柄，wallpaper 又是特殊形态

普通 Activity/Task target 的字段来源如下：

| 字段 | r48 来源/边界 |
|---|---|
| `taskId` | top Activity 所属 Task；不是 Surface ID 或 Activity token |
| `mode` | opening/changing/closing 角色；不是 Activity lifecycle state |
| `leash` / `startLeash` | 主动画 leash / 可选 snapshot Surface句柄 |
| `isTranslucent` | `!fillsParent()` 的容器语义；不是逐像素 alpha 检测 |
| `clipRect` | main window animator 的 last clip hint |
| `contentInsets` | main window content insets再加 letterbox insets |
| `prefixOrderIndex` | 窗口树前序提示；不是 SurfaceFlinger 绝对 layer |
| `position` | API 注释沿用旧的屏幕坐标说法；r48 默认 hierarchical producer 实际给相对 parent 的位置，Activity 固定为 `(0,0)` |
| `localBounds` | 相对 animation parent 的 bounds，适合 leash 定位 |
| `screenSpaceBounds` | 屏幕空间结束 bounds；旧 `sourceContainerBounds` 复制同值 |
| `startBounds` | change 的屏幕空间开始 bounds，否则为 null |
| `windowConfiguration` | windowing mode、activity type等配置；compat 常只派生 activity type |
| `isNotInRecents` | 普通 ActivityRecord 路径固定 false；不是“是否由 Recents 控制” |

`RemoteAnimationTarget` 构造时复制 Rect/Point；跨 Binder 后客户端拿到的是自己的可变值对象，称为“Parcel 时刻的值快照”更准确，不是共享只读内存。server 构造器对 null Rect 会得到空 Rect，只有 `startBounds` 保持 nullable。

这个 `position` 差异来自 producer 而不是 Parcel：`WindowContainer` 默认调用 `getAnimationPosition()`，基类返回相对 parent 的位置，ActivityRecord 为了把 letterbox 纳入动画而固定返回零；只有关闭 hierarchical animations 时才改写成全局 bounds 左上角。调试 r48 不能只照字段的历史注释把它当成无条件屏幕坐标。

SurfaceControl 则是同一个服务端 Surface 节点的跨进程引用。target 用 flags=0 写 `leash` 与 `startLeash`；`SurfaceControl.writeToParcel()` 只有收到 `PARCELABLE_WRITE_RETURN_VALUE` 才释放发送端，所以这里 server 保留自己的引用，client 获得独立本地引用。client 的 `RemoteAnimationTargetCompat.release()` 只释放自己的引用，不会删除服务端节点；服务端 remove leash 与 client release 是两种动作。

wallpaper targets 使用同一个类，却单独放在 `wallpaperTargets[]`：taskId=-1、mode=-1、空几何 rect、isNotInRecents=true。controller 从 WMS root遍历 wallpaper windows，只加入其所在 Display 当前被判定可见的项；不能对 wallpaper 数组套用三个 App mode 或普通 taskId 语义。

## 11. goodToGo 先装 timeout、再删坏 target；process 标志更早于远端 start

controller 的 `goodToGo()` 顺序是：

```text
pending app records为空或已canceled
  → 直接onAnimationFinished，不调用runner
否则
  → post 2000ms × current Animator duration scale timeout
  → 创建FinishedCallback
  → createAppAnimations，剔除坏record并回放已捕获callback
  → appTargets长度为0：立即finish，不创建wallpaper targets
  → 为可见wallpaper建立adapter/leash/targets
  → 登记afterPrepareSurfaces runnable
  → setRunningRemoteAnimation(true)
```

wallpaper adapters 因而不是在 `AppTransition.goodToGo()` 之前就全部安装；它们正是在 controller good-to-go 内创建。app wrappers 已捕获是入口屏障，wallpaper 是屏障内的后续步骤。

timeout 乘的是全局 Animator duration scale，不是 transition animation scale，也不是 adapter duration。scale=0 会投递 0 延迟消息，但“尽快进 Handler”仍不是与 AnimationThread after-prepare runnable 的全序，下一节会看到由此暴露的竞态。

`setRunningRemoteAnimation(true)` 发生在远端 start 之前。pid=0 会直接抛 `RuntimeException`；找不到 `(pid,uid)` 对应的 `WindowProcessController` 只 warning并继续。找到时，WPC 先改本地 boolean，再向 ATMS Handler异步投递 AM 重新计算。只有没先命中 fixed/persistent、top-app 等更高优先级分支，真正落入 `runningRemoteAnimation` 分支时，才写 `VISIBLE_APP_ADJ` 与 `SCHED_GROUP_TOP_APP`；不能把 controller 调用返回无条件解释成 AM 已完成“升权”。

pid=0 的异常点尤其晚：timeout、FinishedCallback、target 数组和 after-prepare runnable 都已经建立，`goodToGo()` 没有回滚，异常还会跳过调用方随后执行的 `AppTransition.clear()`。已排队 runnable 仍可迟到发 start；timeout 再收尾时，末尾的 `setRunningRemoteAnimation(false)` 又会因 pid=0 抛错，使 cancel 通知也可能到不了。

该标志也是 caller 进程上的非计数 boolean。同一 pid 若并行控制两个 Display/transition，一个 controller 先 finish 就可能写 false，而另一个仍在运行；它不具备引用计数语义。

## 12. after-prepare 保证 setup 先提交，却没有把“已取消”封成终态

`WindowAnimator` 在 AnimationThread 的一帧内遍历 Display、调用 `prepareSurfaces()`，把 Display pending transaction merge进 global transaction，随后 `closeSurfaceTransaction("WindowAnimator")`，最后才按登记顺序执行 after-prepare runnables。这个边界让 leash/reparent/初始 crop先提交，再向远端发 target；close 仍不等于 SF latch/present。

runnable 内先 `linkToDeathOfRunner()`，再调用 oneway `onAnimationStart(appTargets, wallpaperTargets, finishedCallback)`。link 或 transact 抛 `RemoteException` 时直接 `onAnimationFinished()`，不再等 timeout，也不额外调用远端 cancel。link 成功后 runner死亡会从 Binder 线程进入 `binderDied()` → controller cancel。

这里存在一条不能被状态图抹平的 r48 竞态：after-prepare runnable既没有可移除句柄，也不检查 `mCanceled`。如果 0-delay timeout、全部 app adapter取消，或其他 cancel 在 runnable 执行前已经：

```text
清pending records → release FinishedCallback外层引用 → set process flag false
```

旧 runnable 之后仍会重新 link death，并发送 `onAnimationStart(..., mFinishedCallback)`；字段此时可能已经是 null。于是“cancel/timeout 已完成”不能推出“远端绝不会再收到 start”。若新 death link建立后 runner仍活，controller 的 `mCanceled` 又会让后续 `binderDied()` 直接返回，这条 link也没有正常 unlink机会。

远端真正收到 start后，WMS 不提供逐帧时钟。target leash后续怎样变化，取决于 Launcher/系统 UI 自己的 Animator、RT callback 与 `SurfaceControl.Transaction`；server timeout只负责收口，不替远端补帧。

## 13. finished 先回放服务端 callback，reparent/remove 仍要等下一次 pending 提交

正常 finished 从 Launcher 发起同步 Binder。服务端 Stub 先 `Binder.clearCallingIdentity()`，再执行 controller 收尾，finally恢复身份。controller 的主要顺序是：

```text
remove timeout
取得WMS global lock
unlink当前场runner death
release FinishedCallback对controller的外层引用
open legacy global Surface transaction
  → 逐个回放app主/thumbnail captured callback
  → 逐个回放wallpaper captured callback
close legacy global Surface transaction
setRunningRemoteAnimation(false)
返回finished Binder
```

主 captured callback 会先做 transfer-map/adapter 身份检查，再运行 `SurfaceAnimator.reset(mAnimatable.getPendingTransaction(), true)`。在有 Display 的普通 target 上，这通常是 Display pending transaction：状态字段此时会清掉，reparent/remove只是写入 pending；controller 的 legacy close不会替这笔显式 transaction执行 `apply()`。后续 `DisplayContent.prepareSurfaces()` 才 merge，WindowAnimator 再 close提交。因此精确完成链是：

```text
Launcher决定finish < 服务端SurfaceAnimator状态reset及pending写入
服务端reset及pending写入 < finished同步Binder返回
服务端reset及pending写入 < WMS merge/close < SF latch < HWC present
finished同步Binder返回 与 WMS merge/close 无固定先后
```

原因是 controller 在 global lock内写 pending，释放该锁后还要清 process flag才返回 Binder；AnimationThread 可以在这段间隙取得同一锁并提交 pending。因此上面的“WMS merge”只相对 reset是后续，未必相对 Binder reply是后续。若 Animatable 请求 defer，controller 更可以先移除 record、清 process flag并返回 Binder，而 `SurfaceAnimator` 仍保留 adapter/leash；此时连 `H_reset` 都尚未发生。远程 wrapper自身不 defer，但 ActivityRecord 的 `AnimatingActivityRegistry` 仍可能这样做。

cancel 路径先在锁下把 `mCanceled=true`，然后调用同一 `onAnimationFinished()` 做服务端清理，最后才 oneway通知 runner `onAnimationCancelled()`。所以客户端收到 cancel时，服务端状态通常已收尾或至少已写 pending，但仍不能推导恢复层级已提交/present。timeout、runner death、freeze、全部 app wrappers取消都走这条序列；启动 transact 的 RemoteException则只 finish、不发 cancel。

FinishedCallback 完成一次收尾后会把 `mOuter` 置 null，timeout释放后到达或顺序重复调用通常变成无操作；controller 的 pending lists也在 WMS lock内逐项移除。不过 `mOuter` 的检查与清空没有同步/原子 once 门，两个真正并发的 Binder 调用仍可能同时观察到非 null，源码不保证严格并发幂等。不能把常见的顺序去重扩大成任意竞态安全。

正常 finished 也不会把 controller 的 `mCanceled` 置 true。`removeCallbacks()` 与 `unlinkToDeath()` 不能召回已出队的 timeout 或已在途的 death callback；它们若随后进入 `cancelAnimation()`，仍会通过 canceled 门，再做一次通常已无 record 的收尾并 oneway发送迟到 cancel。于是“finished 已返回”也不是“runner 此后绝不会收到 cancel”的严格终态。

`onAnimationFinished()` 的 callback loop还有异常边界：captured callback若抛异常，这一条 finish/cancel 路径只在 `finally` 保证 legacy transaction close，然后继续向外抛；`setRunningRemoteAnimation(false)` 会被跳过，若调用来自 cancel，后面的远端 cancel通知也到不了。正常内部 callback通常不应抛，但源码并没有把这条失败变成完整的 finally 清账。

更早的坏 target 剔除又是另一处回放点：`createAppAnimations()` 在 controller 尚未打开上述 legacy transaction时直接调用 captured callback。这里若抛异常，会中断 `goodToGo()`；timeout与 FinishedCallback 已安装，出错 record还可能未移除，但 wallpaper、after-prepare 与 running=true 尚未执行。finish-loop 的 `finally` 对这条分支没有任何保护。

## 14. Launcher compat 把 Binder 切到 UI/RT，但普通 remote 仍不是 Recents 手势协议

r48 SystemUI shared 的 `RemoteAnimationAdapterCompat` 创建匿名 `IRemoteAnimationRunner.Stub`。Binder 线程先把平台 arrays转换成 compat arrays，再把同步 finished接口包装成 Runnable，随后调用 `LauncherAnimationRunner.onAnimationStart()`；后者才按配置用普通 async callback或 front-of-queue async callback投到 Launcher Handler。

这层 compat 有意缩窄了平台协议：它只调用三参 `RemoteAnimationAdapter`，所以 change snapshot固定关闭；`RemoteAnimationTargetCompat` 不公开 `startBounds`，只把 `startLeash` 私下保存用于 release，并把完整 `WindowConfiguration` 压成 `activityType`。不能用这套封装声称 Launcher3 已实现平台 remote-change 双 leash动画。

Launcher3 的两条普通 remote 实例是：

- 点图标/点最近任务卡片时，`getActivityLaunchOptions()` 用 `ActivityOptionsCompat.makeRemoteAnimation()` 建 AppLaunch runner；其中方法名 `composeRecentsLaunchAnimator()` 只表示“从最近任务 UI 启动 App”的视觉分支，仍走普通 `IRemoteAnimationRunner`；
- 回 Launcher 时，`registerRemoteAnimations()` 给 `TRANSIT_WALLPAPER_OPEN + ACTIVITY_TYPE_STANDARD` 登记 Definition，并可另登记 Keyguard-going-away runner。

`LauncherAnimationRunner.AnimationResult` 是一次性服务端协议门：null Animator立即 finish；已经 finish后才拿到 Animator会 start后立刻end；正常 AnimatorSet结束时只发送一次 finished。start后主动把 play time推进 `min(oneFrame,totalDuration)` 是 Launcher减少静止首帧的策略，不是 Remote Animation AIDL规则。

`finishExistingAnimation()` 这个名字也不能按字面读成停止旧 Animator：它只调用旧 `AnimationResult.finish()` 把旧场协议交还 WMS，没有对 `mAnimator` 调 `cancel()` 或 `end()`。旧 Animator仍可自然推进，并继续尝试向“协议上已交还、服务端节点可能尚待 remove”的旧 leash写帧；这些写入不再有协议效果保证，而句柄是否释放还要看后续 client release。其 end listener因 result 已 finished而不会第二次通知服务端。

`WrappedLauncherAnimationRunner` 本身不是 Binder Stub。真正 Stub由 compat 匿名类持有，并强引用这个 wrapper；wrapper只用 `WeakReference` 指向具体 Launcher实现。若实现已被回收，`onCreateAnimation()` 什么也不做，`AnimationResult` 既未 set也未 finish，最终依赖 server timeout/cancel促使 Handler上的 `finishExistingAnimation()` 清掉。

Quickstep 的真实逐帧路径会构造 `SurfaceParams`，交给 `SurfaceTransactionApplier.scheduleApply()`：它注册 Launcher RT frame callback，使用 Launcher render Surface作 barrier，逐项 `deferTransactionUntil()` 后 `Transaction.apply()`。`RemoteAnimationTargets.release()` 还可等待最新 apply sequence安全后，再释放 client SurfaceControl引用。apply/release仍不是服务端 remove或物理 present。

真正交互式 Recents Animation 使用 `IRecentsAnimationRunner`、`RecentsAnimationController` 与控制 Binder，支持 `onTaskAppeared`、输入消费者、截图以及 finish时 Task重排。普通 remote即便视觉函数名含 Recents，也只有 start/cancel/finished三段式协议；下一章才讨论手势期间的动态控制面。

## 15. 九组只读练习把跨进程完成点钉回 r48

以下命令只读源码，均可在 macOS Bash 3.2 或 Zsh 5.9 执行。每个 `rg -e` 都是独立证据点；命中名字以后，还要读相邻分支确认顺序与作用域。

### 练习 1：核对 Adapter Parcel 与 AIDL 的两个方向

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'dest.writeStrongInterface(mRunner);' \
  -e 'dest.writeBoolean(mChangeNeedsSnapshot);' \
  frameworks/base/core/java/android/view/RemoteAnimationAdapter.java
rg -n -F \
  -e 'oneway interface IRemoteAnimationRunner {' \
  -e 'interface IRemoteAnimationFinishedCallback {' \
  frameworks/base/core/java/android/view/IRemoteAnimationRunner.aidl \
  frameworks/base/core/java/android/view/IRemoteAnimationFinishedCallback.aidl
```

说明为什么 pid/uid 不能由客户端 Parcel认证，并分别写出 start代理返回、finished代理返回的最强含义。

### 练习 2：比较五条入口的权限、身份与 next-start 类型尖角

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -A 1 -F \
  -e '<permission android:name="android.permission.CONTROL_REMOTE_APP_TRANSITION_ANIMATIONS"' \
  frameworks/base/core/res/AndroidManifest.xml
rg -n -F \
  -e '<permission name="android.permission.CONTROL_REMOTE_APP_TRANSITION_ANIMATIONS"/>' \
  frameworks/base/data/etc/com.android.launcher3.xml
rg -n -F \
  -e 'definition.setCallingPidUid(Binder.getCallingPid(), Binder.getCallingUid());' \
  -e 'adapter.setCallingPidUid(Binder.getCallingPid(), Binder.getCallingUid());' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
rg -n -F \
  -e 'adapter.setCallingPidUid(callingPid, callingUid);' \
  -e 'if (callingPid == Process.myPid()) {' \
  frameworks/base/services/core/java/com/android/server/wm/SafeActivityOptions.java
rg -n -F \
  -e 'private static final long TIMEOUT_MS = 3000;' \
  -e 'options.setRemoteAnimationAdapter(entry.adapter);' \
  frameworks/base/services/core/java/com/android/server/wm/PendingRemoteAnimationRegistry.java
```

给 non-null basic options 注入 registry adapter，解释为什么 entry 已消费却可能没有 remote controller；再指出 direct WMS入口还缺哪一步。

### 练习 3：还原 Definition 查找、覆盖与死亡生命周期

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mTransitionAnimationMap.put(transition,' \
  -e 'entry.activityTypeFilter == ACTIVITY_TYPE_UNDEFINED' \
  -e '.linkToDeath(deathRecipient, 0 /* flags */);' \
  frameworks/base/core/java/android/view/RemoteAnimationDefinition.java
rg -n -F \
  -e 'definition.linkToDeath(this::unregisterRemoteAnimations);' \
  -e 'mRemoteAnimationDefinition = null;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F \
  -e 'if (animLpActivity == null) {' \
  -e 'if (transit == TRANSIT_CRASHING_ACTIVITY_CLOSE) {' \
  -e 'return mRemoteAnimationDefinition.getAdapter(transit, activityTypes);' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

推演同 transit重复 add、旧 Definition被新 Definition替换后旧 runner死亡、以及只有 Display rule但 animLpActivity为 null 三种结果。

### 练习 4：验证 remote 分支与 change snapshot 的失败缝

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (controller != null && !mSurfaceAnimator.isAnimationStartDelayed()) {' \
  -e 'controller.createRemoteAnimationRecord(this, mTmpPoint, localBounds,' \
  -e 'mSurfaceFreezer.mSnapshot.startAnimation(getPendingTransaction(),' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
rg -n -F \
  -e 'if (mRemoteAnimationAdapter.getChangeNeedsSnapshot()) {' \
  frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
rg -n -F \
  -e 'if (buffer == null || buffer.getWidth() <= 1 || buffer.getHeight() <= 1) {' \
  -e 'mAnimation.startAnimation(mSurfaceControl, t, type, animationFinishedCallback);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceFreezer.java
```

分别推演 single-object delay=true、snapshot capture=null、snapshot有效三条路径，指出哪一条在 good-to-go 前就失败。

### 练习 5：核对 Wrapper 初态、target 门与 mode

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (mRecord.mStartBounds != null) {' \
  -e 't.setPosition(animationLeash, mRecord.mStartBounds.left, mRecord.mStartBounds.top);' \
  -e '|| mAdapter.mCapturedLeash == null) {' \
  -e 'if (dc.mOpeningApps.contains(topActivity)) {' \
  -e '} else if (dc.mChangingContainers.contains(mWindowContainer)) {' \
  -e 'onAnimationFinished(wrappers.mAdapter.mAnimationType,' \
  frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
rg -n -F \
  -e 'return activity != null ? activity.createRemoteAnimationTarget(record) : null;' \
  frameworks/base/services/core/java/com/android/server/wm/Task.java
```

令非零 startBounds 同时启动主/thumbnail wrapper，再构造“主 null、thumbnail 非 null”，说明为何 `(0,0)` 与完全对称取消都是错误模型。

### 练习 6：追 target 字段、Surface Parcel 引用与 client release

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'record.mAdapter.mCapturedLeash, !fillsParent(),' \
  -e 'InsetUtils.addInsets(insets, getLetterboxInsets());' \
  -e 'record.mThumbnailAdapter != null ? record.mThumbnailAdapter.mCapturedLeash : null,' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F \
  -e 'getAnimationPosition(mTmpPoint);' \
  -e 'if (!sHierarchicalAnimations) {' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
rg -n -F \
  -e 'outPosition.set(0, 0);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F \
  -e 'dest.writeParcelable(leash, 0 /* flags */);' \
  -e 'dest.writeParcelable(startLeash, 0 /* flags */);' \
  frameworks/base/core/java/android/view/RemoteAnimationTarget.java
rg -n -F \
  -e 'if ((flags & Parcelable.PARCELABLE_WRITE_RETURN_VALUE) != 0) {' \
  frameworks/base/core/java/android/view/SurfaceControl.java
rg -n -F \
  -e 'mStartLeash = app.startLeash;' \
  -e 'leash.mSurfaceControl.release();' \
  frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RemoteAnimationTargetCompat.java
```

解释“server remove节点”“client release本地引用”“Rect跨Parcel复制”为什么是三件事。

### 练习 7：画出 good-to-go、setup submit 与 oneway start 的偏序

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mHandler.postDelayed(mTimeoutRunnable,' \
  -e 'final RemoteAnimationTarget[] appTargets = createAppAnimations();' \
  -e 'mService.mAnimator.addAfterPrepareSurfacesRunnable(() -> {' \
  -e 'linkToDeathOfRunner();' \
  -e 'setRunningRemoteAnimation(true);' \
  frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
rg -n -F \
  -e 'mService.closeSurfaceTransaction("WindowAnimator");' \
  -e 'executeAfterPrepareSurfacesRunnables();' \
  frameworks/base/services/core/java/com/android/server/wm/WindowAnimator.java
rg -n -F \
  -e 'mAtm.mH.sendMessage(PooledLambda.obtainMessage(' \
  frameworks/base/services/core/java/com/android/server/wm/WindowProcessController.java
rg -n -F \
  -e '} else if (app.runningRemoteAnimation) {' \
  frameworks/base/services/core/java/com/android/server/am/OomAdjuster.java
```

把 process flag本地写、AM异步处理、setup close、onAnimationStart入队与Launcher Stub执行放到一张偏序图，并补出 cancel先于after-prepare的分叉。

### 练习 8：区分 controller finish、SurfaceAnimator reset 与下一次提交

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mHandler.removeCallbacks(mTimeoutRunnable);' \
  -e 'releaseFinishedCallback();' \
  -e 'mService.closeSurfaceTransaction("RemoteAnimationController#finished");' \
  -e 'setRunningRemoteAnimation(false);' \
  -e 'onAnimationFinished();' \
  -e 'invokeAnimationCancelled();' \
  frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
rg -n -F \
  -e 'reset(mAnimatable.getPendingTransaction(), true /* destroyLeash */);' \
  frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
rg -n -F \
  -e 'SurfaceControl.mergeToGlobalTransaction(transaction);' \
  frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

分别给“finished同步返回”“Display pending已merge”“SF latch”“HWC present”写出所需证据，不允许用前一个点替代后一个点。

### 练习 9：核对 Launcher 普通 remote 与 Recents controller 的分界

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'ActivityOptionsCompat.makeRemoteAnimation(new RemoteAnimationAdapterCompat(' \
  -e 'definition.addRemoteAnimation(WindowManagerWrapper.TRANSIT_WALLPAPER_OPEN,' \
  -e 'composeRecentsLaunchAnimator(anim, mV, appTargets, wallpaperTargets,' \
  -e 'surfaceApplier.scheduleApply(params);' \
  frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RemoteAnimationAdapterCompat.java \
  packages/apps/Launcher3/quickstep/src/com/android/launcher3/QuickstepAppTransitionManagerImpl.java
rg -n -F \
  -e 'finishExistingAnimation();' \
  -e 'mAnimationResult.finish();' \
  -e 'mAnimator.setCurrentPlayTime(' \
  packages/apps/Launcher3/quickstep/src/com/android/launcher3/LauncherAnimationRunner.java
rg -n -F \
  -e 'R animationRunnerImpl = mImpl.get();' \
  packages/apps/Launcher3/quickstep/src/com/android/launcher3/WrappedLauncherAnimationRunner.java
rg -n -F \
  -e 'mRunner.onAnimationStart(mController, appTargets, wallpaperTargets, contentInsets,' \
  -e 'mRunner.onTaskAppeared(target);' \
  -e 'cancelAnimation(REORDER_MOVE_TO_ORIGINAL_POSITION, "taskAnimationAdapterCanceled");' \
  frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
```

解释 `composeRecentsLaunchAnimator` 为什么仍是普通 remote；再列出真正 Recents 多出的动态 target、输入与最终重排能力。

## 16. 用排障矩阵收口，并把手势控制留给第 224 章

| 现象 | 第一证据点 | 常见误判 | 下一步 |
|---|---|---|---|
| remote options存在却走本地动画 | animation type、`isTransitionSet()`、后续 Definition覆盖 | 有 adapter字段就必然remote | 追 options type与两次 override时序 |
| direct WMS override在 ready处异常 | adapter calling pid | 权限通过就完成身份绑定 | 区分验权与server stamp |
| Definition突然失效 | Activity长期 death recipient | 一定是当前runner死亡 | 查旧Definition未unlink与Display无长期link |
| change在target发送前崩溃 | thumbnail wrapper与实际snapshot | `changeNeedsSnapshot`会保证截图存在 | 查capture buffer与WC null guard |
| thumbnail起点重复偏移 | record start bounds | wrapper构造传Point(0,0)就会用0 | 看startAnimation先走哪一分支 |
| cancel后仍收到start | after-prepare runnable与mCanceled | canceled是不可逆终态 | 对齐WMS Handler与AnimationThread竞态 |
| runner收到start但进程未升权 | WPC异步post与caller pid | controller boolean等于AM OOM状态 | 查ATMS Handler和ProcessRecord |
| finished已返回但leash仍在 | defer、Display pending transaction | legacy close已经apply reset | 查SurfaceAnimator与下一次prepare/merge |
| 新remote开始后旧Animator仍写帧 | `finishExistingAnimation()` | finish旧协议等于cancel旧Animator | 查旧Animator update listener |
| target数组有leash却无法做change | SystemUI shared compat字段 | 平台能力等于Launcher compat能力 | 核对change flag、startBounds可见性 |
| 动画结束但画面尚未稳定 | WMS submit与显示fence | Binder/Transaction callback就是present | 继续抓SF latch/present证据 |

源码导航：

```text
frameworks/base/core/java/android/app/ActivityOptions.java
frameworks/base/core/java/android/view/RemoteAnimationAdapter.java
frameworks/base/core/java/android/view/RemoteAnimationDefinition.java
frameworks/base/core/java/android/view/RemoteAnimationTarget.java
frameworks/base/core/java/android/view/IRemoteAnimationRunner.aidl
frameworks/base/core/java/android/view/IRemoteAnimationFinishedCallback.aidl
frameworks/base/services/core/java/com/android/server/wm/SafeActivityOptions.java
frameworks/base/services/core/java/com/android/server/wm/PendingRemoteAnimationRegistry.java
frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimator.java
frameworks/base/services/core/java/com/android/server/wm/SurfaceFreezer.java
frameworks/base/services/core/java/com/android/server/wm/WallpaperAnimationAdapter.java
frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RemoteAnimationAdapterCompat.java
frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RemoteAnimationTargetCompat.java
packages/apps/Launcher3/quickstep/src/com/android/launcher3/LauncherAnimationRunner.java
packages/apps/Launcher3/quickstep/src/com/android/launcher3/WrappedLauncherAnimationRunner.java
packages/apps/Launcher3/quickstep/src/com/android/launcher3/QuickstepAppTransitionManagerImpl.java
packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/SurfaceTransactionApplier.java
frameworks/base/services/core/java/com/android/server/wm/RecentsAnimationController.java
```

本章的最小模型是：受信任入口只授予一场视觉控制；SurfaceAnimator/Freezer仍拥有服务端层级；wrapper把 leash与完成能力转成 targets；after-prepare只建立 setup submit先于oneway start；Launcher用自己的UI/RT时钟写帧；同步 finished只把控制交还controller；SurfaceAnimator pending reset、下一次WMS submit与物理present仍要分别结账。

下一章进入第 224 章“Android RecentsAnimationController手势控制、输入消费者与结束提交”，从 Quickstep 发起、`IRecentsAnimationRunner` target交付与输入消费者开始，追手势过程中动态 Task、截图、系统栏和 finish-to-home/finish-to-app 怎样闭合。
