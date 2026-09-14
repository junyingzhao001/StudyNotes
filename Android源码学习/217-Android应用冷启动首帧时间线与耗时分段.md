# 217 Android 应用冷启动首帧时间线与耗时分段

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只能静态核对 AOSP：可以证明服务端计时、进程创建、客户端生命周期、WMS drawn 协议与 `am start -W` 怎样汇合；不能据此声称某台设备的实际毫秒数，也不能把 `Displayed` 文本出现的时刻当成 HWC present 时刻。

第 216 章已经解释一次 UI 帧如何从 Choreographer 进入 RenderThread。现在把镜头拉回应用启动：同一个“冷启动 300 ms”，可能指 shell 等了 300 ms、WMS 在 300 ms 认定窗口 drawn、App 在 300 ms 主动报告内容可用，甚至只是 300 ms 后 logcat 才打印一行文字。

本章只追一个问题：**一次 `am start -W` 发起的普通冷启动，怎样从 ActivityMetricsLogger 的起点穿过进程、Activity、Window 与首帧，并形成彼此不能相加的 `StartingWindowDelay`、`BindApplicationDelay`、`TransitionDelay`、`TotalTime` 与 fully-drawn 时间？**

## 1. 先固定一次冷启动，用二十六个完成点拆开“应用打开了”

不先锁定现场，cold/warm/hot、Splash/Snapshot、普通 BufferQueue/BLAST、单 Activity/跳板 Activity 会被拼成一条并不存在的总时序。本章主线 `L_cold` 固定如下。

| 维度 | 固定值或前提 |
|---|---|
| 入口 | 一个 `am start -W`，只有一个 waiter；显式目标可正常 resolve，返回 `START_SUCCESS` |
| 目标 | 默认 display 上的普通 standard Activity；目标进程与 `WindowProcessController` 均不存在，目标不是 `noDisplay`，此前不可见、未 drawn |
| 合并 | 没有跳板、重定向、同 display 活跃 launch、多 display 并发或另一笔 shell wait |
| 进程 | 普通 app Zygote 路线；为固定图关闭 USAP 分流，无 wrapper、isolated entry point、WebView Zygote 或 app Zygote |
| 生命周期 | attach、`bindApplication` 与 Activity transaction 均成功；无 Provider 异常、进程死亡、pause 抢占或配置 relaunch |
| Window | 系统创建并画出 Splash starting window；它让 app transition 在真实内容窗口 drawn 之前开始，无 transition timeout |
| 首帧 | 一个真实主窗口，无额外 interesting window；硬件渲染、非空 Skia 帧、`prepareTextures=true`，默认非 BLAST adapter；swap/queue 成功且 `didSwap=true` |
| 完成 | WMS 正常认定真实窗口 drawn；App 在此后稍晚只调用一次 `reportFullyDrawn()`，ActivityRecord 与 launch 账本仍可匹配 |

完成点是诊断坐标，不是 AOSP 内置 trace 名：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `S_shell` | system_server 的 shell-command 请求线程已用 `uptimeMillis()`记录本地调用前时刻 | ActivityStarter 已收到请求 |
| `S0` | `notifyActivityLaunching()`已保存 `elapsedRealtimeNanos()` | Intent 已 resolve |
| `P_post` | cold 分支已把 `startProcess` Message 放入 ATMS Handler | 新进程已经创建 |
| `C_cold` | `notifyActivityLaunched()`已建立 COLD `TransitionInfo` | Handler 已执行进程创建 |
| `P_req` | ProcessList 已向 Zygote 发送普通进程创建请求 | child 已 attach |
| `P_child` | child 已进入 `ActivityThread.main()` | Application 已创建 |
| `P_attach` | child 正在同步调用 AMS `attachApplication()` | bind 请求已发出 |
| `B_mark` | AMS 调 `preBindApplication()`，logger 已记 bind delay | App 已执行 `Application.onCreate()` |
| `B_submit` | AMS 已提交 oneway `bindApplication` | App 主线程已处理该 Message |
| `L_submit` | ATMS 已提交含 `LaunchActivityItem` 与最终 `ResumeActivityItem` 的 transaction | `onResume()` 已返回 |
| `A_create` | 客户端已完成目标 Activity `onCreate()` | ViewRoot 已 attach |
| `A_resume` | 客户端已完成 `onResume()`并进入 add-window 路线 | 首帧已 queue |
| `SW_drawn` | WMS 认为 Splash starting window `isDrawnLw()`，记 starting delay | Splash 已由 HWC 显示 |
| `T_start` | AppTransitionController 已通知 transition starting，记 reason/delay | 真实内容窗口已 drawn |
| `U_release` | RenderThread 完成同步并因 `prepareTextures=true`释放 UI 线程 | RT 的 draw/swap 已完成 |
| `Q_return` | 固定非空帧的 producer swap/queue 已成功返回 | SF 已 latch |
| `F_callback` | RT 已写 FrameCompleted 并调用 frame-complete callback | UI Handler runnable 已执行 |
| `W_finish` | WMS 已把主窗口 `DRAW_PENDING`改为 `COMMIT_DRAW_PENDING` | `isDrawnLw()`为 true |
| `W_ready` | surface placement 已把主窗口推进到 `READY_TO_SHOW` | HWC present fence 已 signal |
| `W_report` | reported-visibility 重算检测到 `nowDrawn`，采样时间戳并进入 `ActivityRecord.onWindowsDrawn(true)` | Metrics 聚合账已完成 |
| `M_drawn` | logger 已记 windows-drawn delay并移除该 Activity | 后台日志已打印 |
| `M_join` | transition-start 与 pending-draw 两道门均满足，`done(false)`进入 | shell 已返回 |
| `R_fill` | ActivityStackSupervisor 已填写并 `notifyAll()` 唤醒 WaitResult | logcat 已输出 `Displayed` |
| `S_return` | `startActivityAndWait()`已返回，shell 记录第二个 uptime | 输出格式化已完成 |
| `F_full` | system_server 已处理晚到的 `reportFullyDrawn()`并取 fully-drawn delay | 对应内容 Buffer 已 present |
| `P_signal` | 可靠 present fence 已 signal，接近该合成帧真正显示 | 用户已认为业务内容可用 |

固定主线中可以写出的关键偏序是：

```text
S_shell < S0 < P_post < C_cold
C_cold < P_req < P_child < P_attach < B_mark < B_submit < L_submit
L_submit < A_create < A_resume < U_release < Q_return < F_callback
F_callback < W_finish < W_ready < W_report < M_drawn < M_join
M_join < R_fill < S_return
W_report < F_full
SW_drawn < T_start < M_join
```

`SW_drawn`所在的 starting-window 分支与 `P_req … A_resume`所在的 app 进程分支没有通用全序；固定现场只额外保证 `SW_drawn < T_start < W_report`。`Q_return`之后，WMS draw-state路线与 SF latch/compose 路线分开；WMS没有读取 present fence，所以不能从 `W_report`或 `M_drawn`倒推出 `P_signal`。

## 2. 四个执行语境、三只时钟，先决定哪些数能相减

同一次 launch 至少跨四种执行语境：

| 执行语境 | 关键对象 | 本章责任 |
|---|---|---|
| 外部命令进程 | `/system/bin/am`包装脚本、原生`cmd`客户端 | 把`activity` shell command经 Binder交给AMS，并在服务端结束后接收返回；不在内部`WaitTime`区间 |
| system_server shell-command请求线程 | `ActivityManagerShellCommand`、`ActivityStarter`、`ActivityMetricsLogger`、ActivityStackSupervisor | 解析内部命令、记录`S_shell/S_return`、本地调用ATMS、resolve、分类、等待、聚合并格式化输出 |
| system_server 其他线程 | ATMS Handler、AMS attach Binder 线程、BackgroundThread | 真正启动进程、bind/attach、异步写日志 |
| App UI 与 RenderThread | `ActivityThread`、`ViewRootImpl`、HWUI | Application/Activity生命周期、首个 traversal、producer swap/queue、draw-finished 回报 |

三只常见单调时钟也不能只因单位都是纳秒或毫秒就混减：

| 时钟 | 本章出现位置 | suspend 时是否继续 | 合法用途 |
|---|---|---:|---|
| `elapsedRealtimeNanos()` | ActivityMetricsLogger 的 `S0`与各 metric 端点 | 是 | 同一 launch 账本内计算累计 delay |
| `uptimeMillis()` | shell `WaitTime`、部分 Activity/WMS 状态时间 | 否 | 同一 uptime 域内计算调用等待 |
| `System.nanoTime()` / MONOTONIC | Choreographer、HWUI FrameInfo | 否 | 同一渲染时钟域内分析一帧 |

因此 `TotalTime=180 ms`与 `WaitTime=188 ms`看起来可以相减，不代表源码授权把 8 ms命名为 Binder 开销。设备若在区间内 suspend，两者甚至没有同一计时口径；即使没有 suspend，也要先找到两个端点并排除异步输出、调度与别的 launch。

本章把“时间字段的值”与“打印这行字段的墙上时刻”分开。BackgroundThread 晚打印一行 `Displayed ... +180ms`，其中 180 ms仍来自更早的 `W_report`附近，不是打印线程运行到该行时重新计时。

## 3. `S0` 在 ActivityStarter 里很早，但不是整条 shell 命令的零点

固定的 shell 路线进入 `ActivityStarter.execute()`后，先拒绝带文件描述符的 Intent，再在 ATMS global lock 内调用：

```java
launchingState = mSupervisor.getActivityMetricsLogger()
        .notifyActivityLaunching(mRequest.intent, caller);

if (mRequest.activityInfo == null) {
    mRequest.resolveActivity(mSupervisor);
}

res = executeRequest(mRequest);
mSupervisor.getActivityMetricsLogger().notifyActivityLaunched(
        launchingState, res, mLastStartActivityRecord);
```

`notifyActivityLaunching()`立即取 `SystemClock.elapsedRealtimeNanos()`。所以固定路径的 `S0`：

- 晚于外部`am/cmd`分发与进入AMS的shell-command Binder调用，也晚于`ActivityManagerShellCommand`参数处理、`S_shell`和file-descriptor拒绝门；
- 早于尚未完成的 Intent resolve；
- 早于权限、Task、可见性、生命周期与进程决策；
- 不是 Zygote fork、`Application.onCreate()`或首帧开始时刻。

`LaunchingState`只表示“启动意图已进入度量”，并不证明会得到窗口。接口注释要求调用方随后走 `notifyActivityLaunched()`；固定普通路径满足这一约束。heavy-weight switcher早返回、异常出口等边缘并非都能闭合通知，不能把注释要求误当成所有实际出口的不变量；目标为空时的 `abort(null)`也不会顺手取消一个复用 `LaunchingState`所关联的旧账本。

还有一个容易忽略的调用顺序：cold 的 `startSpecificActivity()`在 `executeRequest()`深处先调用 `startProcessAsync()`，将工作投给 Handler；`executeRequest()`返回后才执行 `notifyActivityLaunched()`并分类。因此字面顺序是：

```text
S0 → executeRequest内P_post → executeRequest返回 → C_cold
```

它并不造成“进程已经存在所以被错分 warm”。该 Message 正是为避免持有 ATMS lock 回调 AMS而异步投递；`notifyActivityLaunched()`仍在这把锁内完成。Handler即使已经开始执行，也要在 `onProcessAdded()`把新 WPC写入ATMS进程表时取得同一 global lock；分类完成前看不到已注册的目标进程。`am start -W`随后 `wait()`释放 monitor，进程创建路线才可继续越过这道门。

## 4. cold/warm/hot 是分类；processSwitch、合并与 pending draw 是另外三把门

真正调用 `TransitionInfo.create()`时，它只接受 `START_SUCCESS`与 `START_TASK_TO_FRONT`。分类只看服务端进程记录与目标 Activity 是否 attach：

```java
if (processRunning) {
    transitionType = r.attachedToProcess()
            ? HOT_LAUNCH : WARM_LAUNCH;
} else {
    transitionType = COLD_LAUNCH;
}
```

由此可得：

| 现场 | r48 分类 | 不能用来替代它的判断 |
|---|---|---|
| 找不到目标进程的 `WindowProcessController` | COLD | Recents中是否还有 Task 卡片 |
| 进程存在，目标 ActivityRecord 未 attach | WARM | 进程是否因 Service/Provider 存活 |
| 进程存在，目标 ActivityRecord 已 attach | HOT | Activity是否刚执行过某个生命周期回调 |

`processSwitch = !processRunning || !processRecord.hasStartedActivity(launchedActivity)`是独立维度，而且是在 `notifyActivityLaunched()`分类时计算，不是 `S0`快照。r48 的 `hasStartedActivity(target)`排除目标本身，再检查其他 ActivityRecord 的 `stopped == false`；方法名里的 started不能机械等同于某个精确 `ActivityState.STARTED`枚举。它决定 MetricsLogger、statsd与 LaunchObserver 是否把事件当作值得记录的启动切换，不改变 cold/warm/hot。一个 launch仍可能跟踪到 windows drawn，却因为 `processSwitch=false`而不输出 `APP_START_OCCURRED`。

另外三条边界必须保留：

1. 若目标已经 `mDrawn && isVisible()`，logger直接终止，因为从新 `S0`等不到有意义的首次 draw。
2. 连续启动可按 caller Activity 或 calling UID关联到活跃 `LaunchingState`；同 display 时只更新 latest Activity，并把尚未 drawn的 Activity加入 pending list，不重建类型与原始 `TransitionInfo`起点。这个分支在调用 `TransitionInfo.create()`之前返回，因此也绕过它对 result code 的校验。
3. `setLatestLaunchedActivity()`只在 `!r.noDisplay && !r.mDrawn`时加入 pending draw。透明 Activity不等于 `noDisplay`；是否有窗口与主题是否透明不能互换。

合并还有一个精细后果：新的 `notifyActivityLaunching()`会更新 `LaunchingState.mCurrentTransitionStartTimeNs`，但已经构造的 `TransitionInfo.mTransitionStartTimeNs`是 final 字段，不会被后来的跳板改写。本章排除跳板，确保所有数字都属于同一目标和同一初始账本。

## 5. cold 进程段：先投递创建，再在 attach 中记录“准备调用 bind”的时刻

`ActivityStackSupervisor.startSpecificActivity()`先找目标 `WindowProcessController`。有进程且有 thread 时可直接 `realStartActivityLocked()`；固定 cold 现场没有，于是：

```text
startSpecificActivity
→ ActivityTaskManagerService.startProcessAsync
→ ATMS Handler: ActivityManagerInternal.startProcess
→ ProcessList / Process.start
→ Zygote socket
→ child ActivityThread.main
→ ActivityThread.attach
→ AMS.attachApplication
```

“Zygote 创建进程”也不是永远等于“现场 fork”。r48 `ZygoteProcess`先判断 `shouldAttemptUsapLaunch()`：USAP pool 已支持、已启用、策略允许且参数兼容时，会把参数交给预先 fork 的 unspecialized process，再由它执行 `specializeAppProcess()`；否则才走 Zygote connection 的 `forkAndSpecialize()`。固定图关闭 USAP，正文结论则保留两条分支。

App child 在 `ActivityThread.main()`准备主 Looper后调用同步 `mgr.attachApplication()`。AMS 的正常 attach 顺序是：

```text
记录 bindApplicationTimeMillis
→ mAtmInternal.preBindApplication(wpc)
→ ActivityMetricsLogger.notifyBindApplication(appInfo)
→ thread.bindApplication(...)
→ app.makeActive(...)
→ mAtmInternal.attachApplication(wpc)
→ RootWindowContainer.attachApplication
→ realStartActivityLocked
```

所以 `BindApplicationDelay` 的端点 `B_mark`是 system_server **即将调用客户端 `bindApplication`之前**，不是 Binder 返回、`handleBindApplication()`结束或 `Application.onCreate()`结束。logger按 `ApplicationInfo`引用身份扫描所有活跃匹配账本，重复通知还会覆写该累计值；它本身并不证明 Binder提交成功。这个 delay累计包含 `S0`之后的 resolve、启动决策、进程请求、Zygote/USAP、child bootstrap、同步 attach 回程以及此前 AMS 准备。

`IApplicationThread`整体声明为 `oneway`。`B_submit`只表示请求已提交；客户端稍后才在主线程处理 `BIND_APPLICATION`。同理，AMS 在 `makeActive()`后通过 ATMS attach桥接到 `realStartActivityLocked()`，提交 Activity transaction。这两笔提交的程序顺序是 bind 在前、Activity transaction在后。

## 6. 客户端不是收到 bind 就有首帧：Application、Activity、ViewRoot 各自有完成点

固定正常路径中，App 主 Looper先完整处理 `handleBindApplication()`，再处理 Activity transaction。bind阶段包含的关键工作是：

```text
创建 LoadedApk / class loader 等运行环境
→ makeApplication（构造 Application）
→ 安装启动所需 ContentProvider
→ Instrumentation.onCreate
→ Instrumentation.callApplicationOnCreate
```

这解释了为什么 Provider自动初始化、Application同步 I/O 或类加载都会推迟 Activity创建，却不能从 `B_mark`直接看出其中哪一项慢。`B_mark`是入口前的累计点，缺少“bind完成”的配对 metric。

服务端 transaction显式添加 `LaunchActivityItem`，最终 lifecycle request 是 `ResumeActivityItem`。客户端执行 callback时 `performLaunchActivity()`构造 Activity并调 `onCreate()`；TransactionExecutor为到达最终 RESUMED状态合成 `ON_START`，最后才由 Resume item进入 `onResume()`。不能把它描述成服务端显式发送了一个独立 `StartActivityItem`。

`handleResumeActivity()`随后取得（必要时才创建）Decor，再通过 WindowManager `addView()`建立 ViewRoot。若`onCreate()`里的`setContentView()`已触发`PhoneWindow.installDecor()`，此处取得的是既有Decor。再往后才有：

```text
首次 relayout / Surface
→ Choreographer traversal
→ measure / layout / draw
→ ThreadedRenderer同步DisplayList
→ RenderThread draw与producer swap/queue
```

`setContentView()`、`onCreate()`、`onResume()`都只是上游点。它们返回不证明 ViewRoot 已完成 traversal，更不证明 Buffer已被 SurfaceFlinger latch。

## 7. Splash 与 app transition 是一条旁路，不是 App 内容首帧

starting window由系统侧为启动过渡提供即时反馈。固定现场使用 Splash；Snapshot是另一种 reason，starting window被转移则还有 `startingMoved`分支。

在 surface placement 中，`ActivityRecord.updateDrawnWindowStates()`遇到 starting window且 `isDrawnLw()`时：

```text
ActivityMetricsLogger.notifyStartingWindowDrawn(activity)
→ mStartingWindowDelayMs = nowElapsed - S0
→ startingDisplayed = true
```

它没有把 starting window计入真实 app window的 drawn 数量。`StartingWindowDelay`只说明 WMS draw-state已到 READY/HAS；它不读取该 Splash 的 present fence，也不代表目标 Activity内容已出现。

`AppTransitionController.transitionGoodToGo()`对 opening Activity接受三类就绪条件：

```text
ActivityRecord.allDrawn && !isRelaunching()
或 startingDisplayed
或 startingMoved
```

若由固定 Splash放行，reason是 `APP_TRANSITION_SPLASH_SCREEN`；snapshot则是 `APP_TRANSITION_SNAPSHOT`；真实窗口全 drawn可记 `APP_TRANSITION_WINDOWS_DRAWN`。`TransitionInfo.mReason`虽然初值是`APP_TRANSITION_TIMEOUT`，但controller的timeout shortcut会跳过逐Activity填reason map；随后用空map通知并不会替任何账本打开transition-start门，不能把默认字段值当成真实的`T_start`。正常的非空通知才让logger写入`TransitionDelay = T_start - S0`，且同一账本只记第一次。

固定场景有 `SW_drawn < T_start < W_report`。一般场景不能背这个顺序：没有 starting window时可能由真实窗口 allDrawn放行；代码还特意允许 `notifyWindowsDrawn()`先到、transition starting后到，再由第二道门完成聚合。

## 8. 首帧从 producer queue 到 WMS drawn：固定顺序明确，present 边界仍在外面

首次需要报告的硬件 draw中，ViewRoot令 `mReportNextDraw=true`并注册 frame-complete callback。固定的非空成功路径是：

```text
UI ThreadedRenderer.draw()
→ DrawFrameTask同步树状态
→ prepareTextures=true，先释放UI线程
→ RenderThread CanvasContext.draw()
→ pipeline swapBuffers / producer queue返回
→ FrameInfo.markFrameCompleted()
→ RT调用frame-complete callback
→ callback向UI Handler队首post Runnable
→ pendingDrawFinished()
→ reportDrawFinished()
→ IWindowSession.finishDrawing()
```

因此在本章严格前提下，`Q_return < F_callback < W_finish`。但这不是“任意 FrameCompleted 都有新 Buffer”的定理：dirty为空且允许 skip-empty时，CanvasContext会不 swap也调用 callback；`canDrawThisFrame=false`、swap失败、软件渲染、SurfaceHolder异步重绘又各有不同门。

默认 `wm_use_blast_adapter=false`时，本章走普通 BufferQueue。若启用 WMS-requested BLAST sync，RT transaction可先合入 `mSurfaceChangedTransaction`，再随 `finishDrawing`交给WMS；不能把默认路径的 producer/WMS关系照搬到该分支。

服务端 draw-state协议还要经过：

```text
ViewRoot reportDrawFinished
→ Session.finishDrawing
→ WMS.finishDrawingWindow
→ WindowStateAnimator: DRAW_PENDING → COMMIT_DRAW_PENDING
→ 下一次surface placement: READY_TO_SHOW
→ 条件允许时performShowLocked / HAS_DRAWN
```

`WindowState.isDrawnLw()`只认 `READY_TO_SHOW`或 `HAS_DRAWN`，不认刚进入的 `COMMIT_DRAW_PENDING`。第一个非 starting window进入 `performShowLocked()`时会调用 `onFirstWindowDrawn()`，移除 starting window并更新 reported visibility；实际 Surface show、transaction提交、SF latch、compose与HWC present仍在后续图形管线。

两条结论可以同时成立：

- 固定普通成功帧中，producer queue 返回严格早于 WMS接受 `finishDrawing`；
- `W_finish/W_ready/W_report`均不等待或读取 SF/HWC present fence，因此 `Displayed`不能当作物理显示时间戳。

## 9. 三套名字相近的 drawn 账本，不能互相代称

r48同一 Activity周围至少有三套“都画好了吗”的判断：

| 账本 | 核心状态 | 统计规则 | 主要消费者 |
|---|---|---|---|
| transition/show 准备 | `ActivityRecord.allDrawn`、`mNumInterestingWindows/mNumDrawnWindows` | `updateDrawnWindowStates()`经 `mightAffectAllDrawn()`、`isInteresting()`统计；还要求所有 child 已评估、非 relaunch | AppTransitionController、布局/show与freeze处理 |
| Activity reported visibility | `reportedDrawn`与 `mDrawn` | 每次重算局部 `UpdateReportedVisibilityResults`；Window排除 freezing、非VISIBLE、STARTING、destroying，drawn数达标后调 `onWindowsDrawn()` | 启动计时、WaitResult、Activity drawn状态 |
| Metrics多 Activity聚合 | `TransitionInfo.mPendingDrawActivities` | 每个被纳入launch的 Activity完成或不再需要时从链表移除；`allDrawn()`只是链表为空 | ActivityMetricsLogger 的完成门 |

第一套的 `allDrawn`不等于第二套的 `reportedDrawn`。特别是第二套 `WindowState.updateReportedVisibility()`并不调用第一套的 `WindowState.isInteresting()`；两者排除规则相似但不是同一个计数器。

第二套中：

```java
nowDrawn = numInteresting > 0 && numDrawn >= numInteresting;
nowVisible = numInteresting > 0
        && numVisible >= numInteresting && isVisible();
```

drawn只要求相关窗口达到 `isDrawnLw()`；visible还要求窗口不在 transition/parent animation并满足 Activity可见性。于是 `onWindowsDrawn()`可以早于 `onWindowsVisible()`，`Displayed`也不要求 `nowVisible=true`。

starting window在前两套都被排除：第一套单独记 `startingDisplayed`，第二套看到 `TYPE_APPLICATION_STARTING`直接返回。Splash再快也不会替真实内容 Activity清空 Metrics pending list。

## 10. ActivityMetricsLogger 有两道完成门；WaitResult 与后台日志再从这里分叉

reported-visibility重算检测到`nowDrawn`后，会先采样timestamp并进入`ActivityRecord.onWindowsDrawn(true, timestamp)`；该回调先调用logger：

```text
找到活跃TransitionInfo
→ mWindowsDrawnDelayMs = timestamp - S0
→ 从mPendingDrawActivities移除当前Activity
→ 创建TransitionInfoSnapshot
→ 若mLoggedTransitionStarting && pending为空，则done(false)
```

完成条件不是单个名为 allDrawn 的布尔值，而是：

```text
Gate A: notifyTransitionStarting 已发生
Gate B: TransitionInfo pending Activity list 已为空
M_join = A && B
```

两门谁后到，谁调用 `done(false)`。固定 Splash路径中 A先到，所以 `notifyWindowsDrawn()`完成B并进入done；源码也允许B先到，再由后续非空`notifyTransitionStarting()`完成A。timeout本身不是这种正常反序：它可绕过reason-map填充而让A保持未满足，账本还需其他事件或取消路径收口。

`done(false)`停止launch trace；interesting launch会先把finished事件投给`LaunchObserverRegistryImpl`的Handler，真正observer回调异步执行，不能与后续日志、waiter建立全序。随后调用`logAppTransitionFinished()`，制作一份为后台记录复制主要标量与字符串的浅快照；其中仍持有`ApplicationInfo`与`WindowProcessController`引用，不能泛称完全不可变。快照还复制`ActivityInfo.launchToken`并清空源字段；这个instant-app度量字符串不是fully-drawn调用使用的Activity Binder token。接着：

- 仅 interesting launch向 BackgroundThread投递 `logAppTransition()`，其中写 MetricsLogger与 `APP_START_OCCURRED`；
- 所有正常完成的 transition都投递 `logAppDisplayed()`，但该方法只为 COLD/WARM打印 `WM_ACTIVITY_LAUNCH_TIME`与 `Displayed`；
- 若有缓存的 fully-drawn请求，同步运行其 Runnable；
- 最后active list会被清理；用于晚报fully drawn的`mLastTransitionInfo`项在正常done时不删除，但同一`ActivityRecord`后续被接受的launch可用`put()`覆盖它，否则保留到Activity移除。

logger返回 snapshot后，`ActivityRecord`才调用 `ActivityStackSupervisor.reportActivityLaunchedLocked()`写 `who/totalTime/launchState`并 `notifyAll()`。所以 Metrics本身不直接唤醒 shell。

固定场景是 `M_join → 日志任务入队 → R_fill`；BackgroundThread何时真正打印与 `R_fill/S_return/P_signal`没有全序。若 B先于A，ActivityRecord甚至会先 `R_fill`，以后 `notifyTransitionStarting()`才完成join并投递日志。合并 launch有多个 pending Activity时，较早一个 `onWindowsDrawn()`也可能先填掉全局 WaitResult，而 `Displayed`要等最后一个 pending被移除并与transition-start汇合。

字段赋值还晚一步：`onWindowsDrawn()`返回后，`updateReportedVisibilityLocked()`才执行`reportedDrawn = nowDrawn`。所以本章的`W_report`特指“检测并进入回调”的计时点，而不是字段已经翻转；固定路径实际还有`M_drawn < M_join < R_fill < reportedDrawn字段翻转`。另有visibility检查会在移除pending后异步确认并写`APP_START_CANCELED`再abort；普通abort不走正常`Displayed`，也不直接唤醒全局`START_SUCCESS` waiter，后者可能最终由idle timeout解除。

## 11. 五个启动 delay 都从 `S0`累计，绝不能直接求和

ActivityMetricsLogger 的核心字段可写成：

| 字段 | 起点 | 终点 | 可缺省 | 它没有证明什么 |
|---|---|---|---:|---|
| `StartingWindowDelay` | `S0` | WMS第一次确认starting window drawn | 是，初值 `-1` | starting像素已present |
| `BindApplicationDelay` | `S0` | AMS准备调用 `bindApplication`之前 | 是，初值 `-1` | `Application.onCreate()`已完成 |
| `TransitionDelay` | `S0` | AppTransitionController通知starting | 正常账本会写 | 真实内容窗口已drawn |
| `WindowsDrawnDelay` / 正常 `TotalTime` | `S0` | Activity reported-drawn时间戳 | 正常完成会写 | SurfaceFlinger/HWC已present |
| fully-drawn delay | `S0` | system_server处理App报告；早报时钳到windows drawn | 是 | App选择的内容有统一客观定义 |

它们是平行的累计读数：

```text
D_starting = SW_drawn - S0
D_bind     = B_mark   - S0
D_trans    = T_start  - S0
D_drawn    = W_report - S0
D_full     = F_full   - S0     // 晚报
```

因此 `D_starting + D_bind + D_trans + D_drawn`会重复计算同一前缀四次。只有 trace已证明两个端点属于同一 launch、同一 clock且先后明确时，才可派生区间。

例如某次固定现场得到：

```text
StartingWindowDelay = 34 ms
TransitionDelay     = 39 ms
BindApplicationDelay= 86 ms
WindowsDrawnDelay   = 182 ms
FullyDrawnDelay     = 310 ms
```

这五项不能相加。因为该次证据另外证明 `T_start < B_mark < W_report < F_full`，才可手算：transition→bind为47 ms、bind→windows-drawn为96 ms、windows-drawn→fully-drawn为128 ms。换一个 starting/attach交错现场，前两个端点可能倒置，公式必须重画。

多 Activity合并时还要更谨慎：每次 `notifyWindowsDrawn()`都会覆写 `mWindowsDrawnDelayMs`，最终保留最后一个 pending Activity完成的累计值。它不是某个固定 Activity类名天然拥有的属性。

## 12. `reportFullyDrawn()` 是 App 语义完成点，而且 Activity 端至多尝试一次

`Activity.reportFullyDrawn()`用于 App声明“首屏重要数据与 UI 已达到可用状态”。r48没有从 View树自动推导业务可用；调用位置属于产品语义，应覆盖首启真正重要的同步与异步内容，又不应故意拖延以粉饰数字。

Activity端逻辑是：

```java
if (mDoReportFullyDrawn) {
    mDoReportFullyDrawn = false;
    try {
        ActivityTaskManager.getService()
                .reportActivityFullyDrawn(mToken, mRestoredFromBundle);
        VMRuntime.getRuntime().notifyStartupCompleted();
    } catch (RemoteException e) {
    }
}
```

标志在 Binder前就清零，pause与stop也会清零。因此准确说法是“Activity至多尝试一次”，不是“保证服务端成功收到一次”。若同步 Binder抛 `RemoteException`，同一 Activity不重试，且同一 `try`后面的 VMRuntime通知也不会执行。

服务端先用 Activity token找 `ActivityRecord`，再用它从 `mLastTransitionInfo`找账本：

- 找不到 ActivityRecord或映射时，服务端不产生对应 fully-drawn metric；客户端的一次机会仍已消耗。
- 本章无跳板，映射稳定；同 display跳板只更新 latest Activity而不必为新 Activity补一条 map，不能把 token到launch账本说成无条件一一对应。
- 若 Metrics pending Activity仍非空且尚未缓存请求，保存一个 Runnable；正常 `done(false)`时再执行，并把 fully-drawn delay钳为 `mWindowsDrawnDelayMs`。
- 若窗口已drawn，直接用当前 elapsed时间减 `S0`；它甚至可以在“windows已drawn但transition-start门仍未到”的短窗口内先记录。
- abort、记录已移除或无法匹配不保证替缓存请求补记结果。

无论早报还是晚报，它都不等待该业务状态对应 Buffer的 HWC present fence。TTID/TTFD可作为现代性能术语帮助交流，但分析 r48时必须落回这里真实存在的 windows-drawn与App主动报告实现，不能把后续平台的 FrameTimeline字段反向套进本版本。

## 13. `am start -W` 有三种等待分支，`TotalTime` 与 `WaitTime`不是同一个量

system_server里的`ActivityManagerShellCommand`在本地同步调用周围记录：

```java
long startTime = SystemClock.uptimeMillis();
result = startActivityAndWait(...);
long endTime = SystemClock.uptimeMillis();
```

`WaitTime = endTime - startTime`，整个区间都在system_server的shell-command请求线程。它包括`S_shell`之后的options构造、对本进程ATMS对象的Java调用、WaitResult等待以及结果填好后返回到`endTime`的尾部；不包括外部`am/cmd → AMS.onShellCommand`的Binder请求与回复、不包括内部命令解析和`S_shell`之前的输出，也不包括`endTime`之后的结果格式化。不能把这段本地`startActivityAndWait()`说成又跨了一次AMS Binder。

ActivityStarter按 start result走三种分支：

| start result | 等待对象 | `totalTime` | `launchState` |
|---|---|---:|---|
| `START_SUCCESS` | 加入全局 `mWaitingActivityLaunched`，在 global lock上wait，直到 who/timeout或结果被改成task-to-front | 正常为 Metrics windows-drawn delay；idle timeout为 `-1` | 正常 snapshot给 COLD/WARM/HOT；timeout可为 `-1` |
| `START_DELIVERED_TO_TOP` | 不等后续draw，立即填 who | `0` | 没有赋值，Java默认0，shell打印 `UNKNOWN (0)` |
| `START_TASK_TO_FRONT` | 已visible且RESUMED则立即完成；否则进入按component匹配的 `mWaitingForActivityVisible` | 立即为0；后续为最近drawn delay或 `-1` | attached为HOT，否则COLD；这里没有WARM |

代码注释还说明 task-to-front的 visible wait当前不会设置 timeout变量。普通 success的 idle timeout则会通过 `reportActivityLaunchedLocked(true, ..., INVALID_DELAY, -1)`解除等待。

两类 waiter的数据结构不同：

- `mWaitingActivityLaunched`是全局列表；`reportActivityLaunchedLocked()`与跳板修正会倒序移除所有尚未完成项，并不按component匹配。本章只允许一个 shell waiter。
- `mWaitingForActivityVisible`保存 `WaitInfo(ComponentName, WaitResult)`，用 `w.matches()`按component完成。

`mGlobalLock.wait()`阻塞的是 system_server正在服务该同步请求的线程，并释放 monitor让启动继续；它不阻塞 App主线程。r48 `WaitResult`只有 `result/timeout/who/totalTime/launchState`，没有一些旧资料常写的 `thisTime`字段。`totalTime < 0`时shell不会打印 `TotalTime`行。

## 14. 诊断时先找第一个分叉点，不要拿一个数字包办整条链

静态源码给出的最好方法不是先猜“Application太重”，而是按完成点寻找第一处分叉：

| 现象 | 已知边界 | 下一步证据重点 |
|---|---|---|
| `StartingWindowDelay`已高 | 连系统占位反馈都晚 | `S0→SW_drawn`间的启动决策、WMS/系统进程调度与starting-surface路线 |
| starting快、`BindApplicationDelay`高 | 占位反馈正常，App进程尚未到pre-bind门 | ProcessList/Zygote或USAP、child bootstrap、attach与AMS准备 |
| bind较早、`WindowsDrawnDelay`高 | 已准备向App发bind，但真实窗口晚 | handleBind、Provider/Application、Activity生命周期、主线程、traversal、RT与finishDrawing |
| `Displayed` duration小，肉眼内容仍晚 | WMS reported-drawn早 | SF latch、GPU/acquire、composition、HWC present、显示扫描；同时检查首帧是否只是空壳内容 |
| windows-drawn快、fully-drawn高 | 框架首窗早，App业务完成声明晚 | 数据加载、首屏状态转换及报告点语义 |
| `WaitTime`与`TotalTime`差异异常 | 两字段本就跨 uptime/elapsed clock | suspend、Binder前后、waiter分支、并发launch与返回尾部；禁止直接给差值命名 |

`Displayed`文字早或晚出现都只是日志线程调度现象；真正有价值的是文字携带的 duration及其服务端来源。反过来，producer queue、HWUI `FrameCompleted`也只到图形提交附近；要证明物理显示，应追 SF/HWC `presentAndGetFrameFences()`及其 fence，并注意 HAL可声明 `PRESENT_FENCE_IS_NOT_RELIABLE`。

实践中可把证据分三层对齐：

```text
服务端：ActivityStarter / ActivityMetricsLogger / WMS trace与事件
客户端：ActivityThread / Choreographer / ViewRoot / HWUI slice
显示端：BufferQueue或BLAST / SF latch / composition / HWC present fence
```

先问“第一个比健康样本晚的完成点是哪一个”，再检查它与前一点之间的线程、锁、Binder或队列；不要因 `onResume()`耗时看起来短，就跳过它之前的bind与之后的首帧。

## 15. 九组 macOS 只读源码练习

以下命令都从 AOSP 根目录执行，只读文件，不要求编译或设备。每组先定位调用点，再按正文中的完成点手画偏序；不要把 `rg`命中行号本身当成运行时先后。

### 练习 1：钉住命令入口、system_server的两只钟与输出字段

```bash
test -f frameworks/base/cmds/am/am
rg -n -F -e 'cmd activity "$@"' frameworks/base/cmds/am/am
rg -n -F -e 'IBinder::shellCommand' frameworks/native/cmds/cmd/cmd.cpp
rg -n -F -e 'void onShellCommand' -e 'new ActivityManagerShellCommand' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
test -f frameworks/base/services/core/java/com/android/server/am/ActivityManagerShellCommand.java
rg -n -F -e 'mWaitOption = true' -e 'final long startTime = SystemClock.uptimeMillis()' -e 'startActivityAndWait' -e 'LaunchState:' -e 'TotalTime:' -e 'WaitTime:' frameworks/base/services/core/java/com/android/server/am/ActivityManagerShellCommand.java
rg -n -F -e 'public long totalTime' -e 'public @LaunchState int launchState' -e 'launchStateToString' frameworks/base/core/java/android/app/WaitResult.java
```

回答：外部`am/cmd`怎样进入system_server？`S_shell/S_return`各在哪一行？为什么`TotalTime`与`WaitTime`不能直接相减？确认r48是否存在`thisTime`。

### 练习 2：证明 `S0`、resolve、cold Message 与分类的字面顺序

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
rg -n -F -e 'int execute()' -e 'notifyActivityLaunching' -e 'mRequest.resolveActivity' -e 'executeRequest' -e 'notifyActivityLaunched' -e 'waitForResult' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
rg -n -F -e 'startSpecificActivity' -e 'hasThread()' -e 'startProcessAsync' frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
rg -n -F -e 'ActivityManagerInternal::startProcess' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
rg -n -F -e 'mService.mAtmInternal.onProcessAdded' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
rg -n -F -e 'void onProcessAdded' -e 'synchronized (mGlobalLockWithoutBoost)' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
```

回答：为何已经投递的Handler不能让分类看到已注册的WPC，且固定路线的`P_req`仍在global lock释放之后？列出`S0`明确不包含的命令前缀。

### 练习 3：独立验证 launch type、processSwitch、合并与 pending draw

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
rg -n -F -e 'elapsedRealtimeNanos()' -e 'static TransitionInfo create' -e 'processRunning' -e 'attachedToProcess()' -e 'hasStartedActivity' -e 'mTransitionStartTimeNs' frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
rg -n -F -e 'mCurrentTransitionStartTimeNs' -e 'mAssociatedTransitionInfo' -e 'setLatestLaunchedActivity' -e '!r.noDisplay && !r.mDrawn' -e 'launched activity already visible' frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
```

回答：Task存在为何仍可能COLD？`processSwitch`为何不是第四种 launch type？合并后哪个起点会变、哪个不会变？

### 练习 4：区分普通 Zygote fork 与 USAP specialization

```bash
test -f frameworks/base/core/java/android/os/ZygoteProcess.java
rg -n -F -e 'usesWebviewZygote()' -e 'usesAppZygote()' -e 'Process.start' frameworks/base/services/core/java/com/android/server/am/ProcessList.java
rg -n -F -e 'shouldAttemptUsapLaunch' -e 'attemptUsapSendArgsAndGetResult' -e 'attemptZygoteSendArgsAndGetResult' frameworks/base/core/java/android/os/ZygoteProcess.java
rg -n -F -e 'processOneCommand' -e 'forkAndSpecialize' -e 'handleChildProc' frameworks/base/core/java/com/android/internal/os/ZygoteConnection.java
rg -n -F -e 'usapMain' -e 'specializeAppProcess' frameworks/base/core/java/com/android/internal/os/Zygote.java
```

回答：哪些条件才尝试USAP？为什么“每次冷启动现场都由Zygote新fork”在r48过强？

### 练习 5：闭合 attach、pre-bind 与 Activity transaction

```bash
test -f frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
rg -n -F -e 'attachApplicationLocked' -e 'mAtmInternal.preBindApplication' -e 'thread.bindApplication' -e 'app.makeActive' -e 'mAtmInternal.attachApplication' frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
rg -n -F -e 'notifyBindApplication' -e 'mBindApplicationDelayMs' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
rg -n -F -e 'oneway interface IApplicationThread' -e 'void bindApplication' frameworks/base/core/java/android/app/IApplicationThread.aidl
rg -n -F -e 'sendMessage(H.BIND_APPLICATION' -e 'handleBindApplication' -e 'makeApplication' -e 'installContentProviders' -e 'mInstrumentation.onCreate' -e 'callApplicationOnCreate' frameworks/base/core/java/android/app/ActivityThread.java
rg -n -F -e 'realStartActivityLocked' -e 'LaunchActivityItem.obtain' -e 'ResumeActivityItem.obtain' frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
rg -n -F -e 'handleStartActivity' -e 'handleResumeActivity' frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java
```

回答：`B_mark`为何不等于 `Application.onCreate()`结束？`ON_START`由谁补齐？服务端显式final lifecycle item是哪一个？

### 练习 6：从 frame-complete callback 追到 WMS draw-state

```bash
test -f frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'setFrameCompleteCallback' -e 'pendingDrawFinished' -e 'reportDrawFinished' -e 'mWindowSession.finishDrawing' frameworks/base/core/java/android/view/ViewRootImpl.java
rg -n -F -e 'swapBuffers' -e 'markFrameCompleted' -e 'mFrameCompleteCallbacks' frameworks/base/libs/hwui/renderthread/CanvasContext.cpp
rg -n -F -e 'public void finishDrawing' -e 'mService.finishDrawingWindow' frameworks/base/services/core/java/com/android/server/wm/Session.java
rg -n -F -e 'void finishDrawingWindow' -e 'win.finishDrawing(postDrawTransaction)' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'finishDrawingLocked' -e 'COMMIT_DRAW_PENDING' -e 'READY_TO_SHOW' -e 'HAS_DRAWN' frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
rg -n -F -e 'isDrawnLw' -e 'performShowLocked' -e 'onFirstWindowDrawn' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'WM_USE_BLAST_ADAPTER_FLAG' -e 'ADD_FLAG_USE_BLAST' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

回答：在本章固定条件下证明 `Q_return < W_finish < W_ready`。再列出至少两个不能把该顺序推广到所有draw的例外。

### 练习 7：把三套 drawn 账本与两道 Metrics 门分别圈出

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'updateDrawnWindowStates' -e 'updateAllDrawn' -e 'allDrawnStatesConsidered' -e 'mNumInterestingWindows' -e 'startingDisplayed' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'updateReportedVisibilityLocked' -e 'reportedDrawn' -e 'onWindowsDrawn' -e 'onWindowsVisible' -e 'reportActivityLaunchedLocked' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'TYPE_APPLICATION_STARTING' -e 'results.numInteresting' -e 'results.numDrawn' -e 'results.numVisible' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F -e 'mPendingDrawActivities' -e 'removePendingDrawActivity' -e 'boolean allDrawn()' -e 'mLoggedTransitionStarting' -e 'notifyWindowsDrawn' -e 'notifyTransitionStarting' -e 'done(false' frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
```

回答：哪一套服务 transition/show，哪一套触发 `ActivityRecord.onWindowsDrawn()`，哪一套只判断 pending Activity list为空？

### 练习 8：验证 fully drawn 的一次尝试、早报钳位与映射边界

```bash
test -f frameworks/base/core/java/android/app/Activity.java
rg -n -F -e 'mDoReportFullyDrawn' -e 'reportFullyDrawn()' -e 'reportActivityFullyDrawn' -e 'notifyStartupCompleted' -e 'performPause()' -e 'performStop(' frameworks/base/core/java/android/app/Activity.java
rg -n -F -e 'reportActivityFullyDrawn' -e 'reportFullyDrawnLocked' frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'mLastTransitionInfo' -e 'mPendingFullyDrawn' -e 'mWindowsDrawnDelayMs' -e 'logAppTransitionReportedDrawn' -e 'windowsFullyDrawnDelayMs' -e 'logAppFullyDrawn' -e 'launchedActivityLaunchToken' -e 'launchToken = null' frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
```

回答：远端异常后为什么不会重试？早报在哪个正常完成点被执行？Activity token为何不保证总能找到 launch账本？

### 练习 9：核对 WaitResult 三分支、日志异步与 present 边界

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
rg -n -F -e 'case START_SUCCESS' -e 'mWaitingActivityLaunched.add' -e 'mGlobalLock.wait()' -e 'case START_DELIVERED_TO_TOP' -e 'case START_TASK_TO_FRONT' -e 'LAUNCH_STATE_HOT : LAUNCH_STATE_COLD' frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
rg -n -F -e 'mWaitingActivityLaunched' -e 'mWaitingForActivityVisible' -e 'w.matches' -e 'result.totalTime = totalTime' -e 'reportActivityLaunchedLocked' frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
rg -n -F -e 'logAppTransitionFinished' -e 'BackgroundThread.getHandler().post' -e 'APP_START_OCCURRED' -e 'WM_ACTIVITY_LAUNCH_TIME' -e 'Displayed ' frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
rg -n -F -e 'presentAndGetFrameFences' -e 'getPresentFence' frameworks/native/services/surfaceflinger/CompositionEngine/src/Display.cpp frameworks/native/services/surfaceflinger/CompositionEngine/src/Output.cpp
rg -n -F -e 'PRESENT_FENCE_IS_NOT_RELIABLE' hardware/interfaces/graphics/composer/2.1/IComposer.hal
```

回答：为什么 delivered-to-top显示 `UNKNOWN (0)`？全局waiter与按component waiter如何区分？`Displayed`日志内容、日志实际打印与可靠 present fence各对应哪个完成点？

## 16. 把本章压成一张可复核的冷启动账本

先记住四条不变量：

1. `S0`是ActivityStarter内部尽早的elapsed起点，早于常见resolve，却晚于外部`am/cmd`分发、shell-command Binder入口与system_server内部命令前缀。
2. cold/warm/hot只由进程存在与 Activity attach状态分类；`processSwitch`、同display合并与 pending draw各有独立用途。
3. WMS至少有 transition `allDrawn`、Activity `reportedDrawn`、Metrics pending-list三套账；`Displayed`与正常 `WaitResult.totalTime`落在第二套触发的时间戳上。
4. starting、bind、transition、windows-drawn与fully-drawn全是从同一 `S0`出发的累计读数，不能相加；shell `WaitTime`还属于另一只时钟。

固定主线可压缩为：

```text
shell uptime起点
→ ActivityMetrics elapsed S0
→ cold进程Message先投递、TransitionInfo随后分类
→ Zygote fork或USAP specialization
→ child同步attach
→ preBind记累计点
→ oneway bind + Activity transaction
→ Application/Provider/Activity/ViewRoot
→ producer queue
→ WMS finish/READY，检测nowDrawn并进入onWindowsDrawn
→ Metrics与transition-start双门汇合
→ WaitResult填写；日志异步打印
→ App按业务语义reportFullyDrawn
```

看到“首帧慢”时，先问具体是哪一个完成点慢；看到“Displayed很快”时，再问证据有没有走到 SF/HWC；看到多个毫秒字段时，先画端点与时钟，再做减法。这样才能把一个启动数字还原成可验证、可定位、不会跨层越界的时间线。

下一章继续放大starting-window支线：Android Starting Window、Splash、Task Snapshot与首窗口交接。
