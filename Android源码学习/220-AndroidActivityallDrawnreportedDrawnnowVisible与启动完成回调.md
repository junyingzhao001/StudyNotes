# 220 Android Activity allDrawn、reportedDrawn、nowVisible 与启动完成回调

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只做静态源码核对：可以证明 WMS 怎样聚合一个 Activity 的窗口、怎样产生 drawn/visible 边沿、怎样结束启动计时与两类 `WaitResult`；不能据此声称 SurfaceFlinger 已 latch、HWC 已 present 或用户已经看到某个像素。第 221 章再进入 `AppTransitionController` 的 opening/closing apps 与转场启动条件。

第 219 章停在单扇 `WindowState` 的 `READY_TO_SHOW`、`HAS_DRAWN` 与 SurfaceControl show 事务。本章把观察尺度抬到 `ActivityRecord`：**`allDrawn`、`reportedDrawn`、`mDrawn`、`reportedVisible`、`nowVisible` 为什么不是同一个“完成”，`am start -W` 又究竟等哪一张表？**

## 1. 固定一次启动，先给各完成点命名

先固定 `L_open`：普通 Activity 启动；真实主窗口可正常完成绘制；没有 relaunch、进程死亡、窗口转移或异常；旧版 AppTransition 最终开始；启动计时序列有效。特殊路径随后逐一放宽。

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `V_req` | `setVisibility(true)` 已写 `mVisibleRequested=true` | Activity 容器已经提交可见 |
| `C_allow` | `mClientVisible=true`，客户端可见性控制不再阻止 App 窗口生产；若字段发生变化才分发 | 一定发生过一次 true 回调，或客户端已经画完 |
| `W_ready` | 真实窗口已是 `READY_TO_SHOW` 或 `HAS_DRAWN`，`isDrawnLw()` 为 true | Activity 的窗口集合已经齐备 |
| `A_ready` | `ActivityRecord.allDrawn` 由窗口组装门置 true | `reportedDrawn` 已产生边沿 |
| `V_commit` | `commitVisibility(true, ...)` 已令 `mVisible=true` | Activity Surface transaction 已被显示系统消费 |
| `R_draw` | reported 聚合产生 drawn 正边沿，`onWindowsDrawn(true, ...)` 写 `mDrawn=true` | 整个 launch sequence 已完成 |
| `R_vis` | reported 聚合产生 visible 正边沿，`onWindowsVisible()` 写 `nowVisible=true` | 屏幕已经 present |
| `M_draw` | `ActivityMetricsLogger.notifyWindowsDrawn(r, ...)` 返回当前 transition 快照；delay由这次 r 到达更新，Activity元数据取序列的 latest record | transition 的另一扇门已经打开，或 snapshot元数据就是 r |
| `M_done` | transition-start 与 pending-Activity-empty 两门都满足 | 后台日志已经落盘 |
| `W_ret` | 某个同步 `WaitResult` 的循环退出 | 退出原因一定是目标 Activity 新绘制 |
| `P_real` | 可归因于目标内容的 present fence signal | Framework 中上述任一 boolean 可单独证明它 |

固定路线中可以写出这些局部关系：

```text
setVisibility(true)建立 V_req，并在等待opening draw前确保C_allow
若mClientVisible原本已true，C_allow可以早于V_req且不会重新分发
W_ready < A_ready < 额外layout/show机会

reported聚合的普通正边沿：R_draw < R_vis
R_draw < M_draw
transition-start 与 pending-empty（任意先后）共同决定 M_done
```

不能补写一条通用的 `A_ready < R_draw`。`updateReportedVisibilityLocked()`还有 relayout、移窗、首个真实窗口、转场提交和动画结束等入口；反过来，`allDrawn`也可能被 starting-window 转移复制，或被 closing-app 路径强制置 true。两套聚合共享窗口状态，却不是上下游固定流水线。

## 2. 九个近似字段分属七本账，两个 allDrawn 还同名不同物

| 账本 | 字段或对象 | 所有者 | 回答的问题 |
|---|---|---|---|
| 可见性请求 | `mVisibleRequested` | `ActivityRecord` | 系统当前想不想让这个 Activity 可见 |
| 客户端控制 | `mClientVisible` | `ActivityRecord` | 最近一次希望窗口客户端采用哪种 app visibility |
| 容器提交 | `mVisible` / `isVisible()` | `ActivityRecord` | Activity 容器的逻辑可见性是否已经 commit |
| 成组放行 | `allDrawn`、`mLastAllDrawn` | `ActivityRecord` | 真实窗口集合是否满足整体 show/unfreeze 的门，以及该门的上次值 |
| reported 边沿 | `reportedDrawn`、`reportedVisible` | `ActivityRecord` | 上一次已消费的窗口聚合结果是什么 |
| 上层状态 | `mDrawn`、`nowVisible` | `ActivityRecord` | metrics、同步等待和生命周期消费者当前看见什么 |
| 启动序列 | `TransitionInfo.mPendingDrawActivities` | `ActivityMetricsLogger` | 同一次 launch sequence 中还有哪些 ActivityRecord 欠 windows-drawn |

最容易混淆的是两个 `allDrawn`：

- `ActivityRecord.allDrawn`聚合该 Activity 下的 `WindowState`，用于窗口成组放行；
- `TransitionInfo.allDrawn()`只检查 `mPendingDrawActivities.isEmpty()`，聚合同一次启动序列里的 `ActivityRecord`。

同名不意味着同一集合、同一时刻或同一消费者。以后说“完成”时，至少要附上拥有者和集合元素类型。

## 3. requested、client 与 committed visibility 可以长期不相等

`setVisibility(true, deferHidingClient)`先从 opening/closing 集合移除自己，清 `waitingToShow`，再写 `mVisibleRequested=true`。变为可见时，即使已经设置 AppTransition、稍后才统一提交，代码也会先调用 `setClientVisible(true)`：否则系统一边等待 opening app 绘制，一边又不允许客户端生产窗口，协议会自锁。重复请求 invisible 另有提前返回分支，不能套用这段顺序。

`setClientVisible()`只有在值实际改变且隐藏未被 defer 时，才更新字段并递归执行 `sendAppVisibilityToClients()`。落到 `WindowState` 后，普通窗口通过 `IWindow.dispatchAppVisibility(clientVisible)`通知客户端；隐藏时还会先 detach 客户端附加的 child surfaces。starting window 是例外：当 client visibility 变 false 时，它不会随真实 App 窗口一起被这条调用隐藏。

r48 构造 `ActivityRecord`时显式令 `nowVisible=false`、`mDrawn=false`，却令 `mClientVisible=true`。因此第一次 `setClientVisible(true)`可能无操作；不能把字段初值解释成“已成功送达一次 true 回调”。它是控制状态，不是带确认的消息日志。

若转场已经设置且允许动画，`setVisibility()`把 Activity 放入 `mOpeningApps`或 `mClosingApps`后直接返回。此时常见状态是：

```text
mVisibleRequested = true
mClientVisible    = true
mVisible          = false
waitingToShow     = true
```

稍后 `commitVisibility()`才通知 child window 的 app visibility 变化、调用 `setVisible()`并同步 `mVisibleRequested`。`setVisible()`改变 `mVisible`并 schedule animation；`prepareSurfaces()`又在 `isVisible()`为 true，或 Activity自身/祖先正运行“排除 SCREEN_ROTATION 类型后的其他动画”时，向同步 transaction stage Activity SurfaceControl 的 show，否则 stage hide。这里 `PARENTS`包含容器自身与祖先，不是只查 parent；`isAnimatingExcluding`也不是“正在屏幕旋转”的判断。

所以三者分别是“请求”“客户端生产开关”“容器逻辑提交”。即使 `mVisible=true`，也只到 WMS transaction 准备侧，不能跨越 SurfaceFlinger latch、composition 与 HWC present。

## 4. 第一套聚合在 surface placement 中统计，但 evaluated 不是每轮位图

`RootWindowContainer.performSurfacePlacementNoTrace()`先递增全局 `mTransactionSequence`，再进入各 Display 的 surface changes。对每扇有 Surface 的窗口，`DisplayContent`先调用 `commitFinishDrawingLocked()`，随后才调用其 Activity 的 `updateDrawnWindowStates(w)`；后者返回 true 时，把 Activity 去重加入临时链表。窗口遍历结束后，链表中的 Activity 才执行 `updateAllDrawn()`。

只要没有命中 `allDrawn && !mFreezingScreen`的提前返回，每个新 transaction sequence 第一次碰到该 Activity 时会重置：

```java
mNumDrawnWindows = 0;
startingDisplayed = false;
mNumInterestingWindows = findMainWindow(false) != null ? 1 : 0;
```

主窗口被预占一个 interesting 名额，遍历到它时不会重复增加；其他满足条件的窗口才逐个增加。这个预占是“只要能找到非 starting main window就占一位”，并不先检查它此刻是否 `isInteresting()`。因此“GONE窗口不阻塞”只适合描述普通非主窗口；一个仍被 `findMainWindow(false)`找到、但不满足 interesting/drawn 的主窗口可以让基线那一位一直欠着。

更关键的是 `mDrawnStateEvaluated`。`updateDrawnWindowStates()`一进入就把当前 `WindowState`标为已评估，但这个 bit **不会随 `mTransactionSequence`每轮清零**；r48 只在 `WindowState.onParentChanged()`中清 false。它回答的是“这个新挂入当前 parent 的直接 child 是否至少被父级考虑过”，用来阻止刚加入、尚未遍历的 child 让集合提前闭合；每轮重新统计的是两个数字，不是这组 bit。

因此 dump 中 `drawnStateEvaluated=true`不能翻译成“本次 placement 已走到它”，`mNumDrawnWindows`也不能在 `allDrawn=true && !mFreezingScreen`的快速返回之后继续当成实时统计。

## 5. allDrawn 的筛选、starting window 与精确公式

第一套聚合依次经过两层判断：

| 判断 | r48 条件 | 作用 |
|---|---|---|
| `mightAffectAllDrawn()` | on-screen，或类型为 base/drawn application；且不在 exit animation、不 destroying | 决定是否值得影响成组 drawn，并参与“所有 child 已考虑”检查 |
| `isInteresting()` | 有 Activity、App 未死亡、未命中 Activity/window freezing 排除、client view 为 VISIBLE | 决定是否真计入 interesting/drawn 数字 |
| `isDrawnLw()` | 有 Surface、不 destroying，draw state 为 `READY_TO_SHOW`或`HAS_DRAWN` | 决定这一名额是否已经 drawn |

`COMMIT_DRAW_PENDING`仍不算 drawn；`READY_TO_SHOW`已经算。这和第 219 章一致：这里认证的是 WMS 可放行内容，不要求窗口已 `performShowLocked()`，更不要求 present。

starting window 单独处理。它不进入真实 Activity 的 interesting/drawn 数字；只有外层仍是 `!allDrawn`、该窗 `mightAffectAllDrawn()`且它自己 `isDrawnLw()`时，代码才调用 `notifyStartingWindowDrawn()`并令 `startingDisplayed=true`。这可以给 transition readiness 和 starting-window delay 提供信息，但不会凭空产生真实窗口的 `reportedDrawn`、结束 windows-drawn 等待或输出 `Displayed`。

最后的门是：

```java
numInteresting > 0
        && allDrawnStatesConsidered()
        && mNumDrawnWindows >= numInteresting
        && !isRelaunching()
```

四项分别防止空集合真值、刚挂入 child 未被考虑、drawn 名额不足以及 relaunch 期间沿用不完整集合。源码明确使用 `>=`而非 `==`；所以 dump 中 drawn大于interesting本身不挡门，但代码也没有提供“多出来的是哪扇窗”的业务承诺。

## 6. allDrawn 的结果是再给 show 一次机会，不是上屏回执

`updateAllDrawn()`置 true 后做两件事：给 Display 标记 `setLayoutNeeded()`，并向 WMS Handler 投递 `NOTIFY_ACTIVITY_DRAWN`。额外 layout 要求正常循环中的后续 placement 再给刚进入 `READY_TO_SHOW`的窗口一次 `performShowLocked()`机会，但它不是唯一 show 路径：下面的 animator检查可直接 `showAllWindowsLocked()`，opening app又由 AppTransition接管。

动画循环中的 `WindowAnimator`还会遍历 Display 调 `checkAppWindowsReadyToShow()`。该方法用 `mLastAllDrawn`做边沿记忆：

- false 边沿只更新记忆并返回；
- true 且 Activity 正冻结时，show all windows、停止 freeze，并请求 wallpaper layout；
- 普通 true 边沿请求 animation layout；若 Activity 不在 `mOpeningApps`且 `canShowWindows()`，立即 show all windows；
- `canShowWindows()`仍会挡住“Activity自身或祖先正在动画，且存在非默认色彩窗口”的情况；这里的 `PARENTS`同样包含自身。

若 Activity仍在 `mOpeningApps`，show由 AppTransition接管。r48 的 opening 处理顺序是 `commitVisibility(true, false)`、`updateReportedVisibilityLocked()`、清 `waitingToShow`，然后在 Surface transaction 中 `showAllWindowsLocked()`。这甚至允许无动画窗口先形成 reported visible 边沿、后调用 show，直接证明 `nowVisible`不是 SurfaceControl shown 回执。动画选择与 opening/closing readiness 留到第 221 章。

`performShowLocked()`内部还有一处更细的顺序：它先检查 `showToCurrentUser()`，失败便清 policy flag并返回。通过用户门后，当 draw state 为 READY/HAS 且不是 starting window 时，它先调用 `onFirstWindowDrawn()`；后者会移除 starting window并重算 reported。代码随后才检查 `mDrawState == READY_TO_SHOW && isReadyForDisplay()`，通过后才写 `HAS_DRAWN`。所以 reported重算甚至可以发生在本次 show readiness失败之前，不能拿 callback名字反推 show成功。

WMS Handler 的 `NOTIFY_ACTIVITY_DRAWN`也不是 metrics 的 `onWindowsDrawn`。它只携带 token、没有 generation；清掉 `allDrawn`也不会撤销已经排队的旧消息。消息异步进入 ATMS，按 token 找仍在栈中的 record，再调用 `ActivityStack.notifyActivityDrawnLocked()`；这里服务的是 translucent-Activity conversion 的 undrawn 集合及其独立 timeout。record 已不在栈中时就无事发生。

最后，`allDrawn=true`并非总是 draw 证据：closing-app 为了启动退出动画会强制写 true；starting-window 转移也会在源 Activity 已 true 时复制它。它会在重新显示隐藏/stopped Activity、非 transition-animation 下的窗口 draw-state reset、replacement、特定最后窗口移除、orientation 或 drag-resize 分支被清 false。

## 7. 第二套聚合由多条路径触发，筛选规则完全不同

`updateReportedVisibilityLocked()`复用一个 `UpdateReportedVisibilityResults`，先 reset，再让每个直接 child `WindowState`递归汇总。`WindowState.updateReportedVisibility()`先处理自己的 child windows，再判断自身；所以结果可以包含 attached child window，而不是只看主窗口。

它不是 `allDrawn`之后唯一的一步。relayout、窗口移除、首个真实窗口 drawn、opening/closing commit、窗口动画结束等路径都能调用它。方法名带 `Locked`但自身不获取锁；这些生产路径依靠 caller 已持有 WM/ATMS 共享 global lock。等待者也在同一把锁上 `wait()`，写字段并 `notifyAll()`之后，必须等生产者释放锁，等待线程才能重获锁并检查条件。

对每一扇递归到的窗口，以下任一条件成立就完全排除：

```java
mAppFreezing
|| mViewVisibility != View.VISIBLE
|| mAttrs.type == TYPE_APPLICATION_STARTING
|| mDestroying
```

通过筛选后先 `numInteresting++`，再按下表贡献：

| 单窗状态 | `numDrawn` | `numVisible` | 对 `nowGone` |
|---|---:|---:|---|
| `isDrawnLw()`且 `isAnimating(TRANSITION \| PARENTS)`为 false | +1 | +1 | 置 false |
| `isDrawnLw()`且上述检查为 true | +1 | +0 | 置 false |
| 未 drawn、但上述检查为 true | +0 | +0 | 置 false |
| 未 drawn、上述检查也为 false | +0 | +0 | 不改变 |
| 被筛选排除 | +0 | +0 | 不改变 |

这里的 `numVisible`没有调用 `isDisplayedLw()`，不检查 policy visibility、`performShowLocked()`、SurfaceController shown、SF latch 或 present。它只是“drawn 且没有指定动画”的计数名，必须按代码定义读，而不能按英文直觉升级。

## 8. raw 公式与 sticky 规则解释了抖动，也暴露了 nowGone 的反直觉

汇总后的原始候选值是：

```java
rawDrawn = numInteresting > 0 && numDrawn >= numInteresting;
rawVisible = numInteresting > 0
        && numVisible >= numInteresting
        && isVisible();
```

`rawVisible`比 `rawDrawn`多要求所有 interesting window 不在指定动画，并要求 Activity 的 `mVisible=true`。它仍不要求物理显示。

接着应用 sticky 规则：若 `nowGone=false`，任何 false 候选都恢复为旧的 `reportedDrawn/reportedVisible`；true 候选仍可以上升。这样，一扇仍 drawn 或仍动画的窗口在 replacement/transition 中不会让已经报告的 true 因瞬时计数不足而掉下去。

`nowGone`这个名字尤其危险。它初始为 true，却只会被“通过筛选且 drawn，或通过筛选且 animating”的窗口改成 false。因此：

- 存在一扇通过筛选、但未 drawn 且不动画的窗口时，`nowGone`仍可能为 true；
- 只剩 starting、freezing、destroying 或 client view非VISIBLE窗口时，它也可能为 true；
- 只要任意合格窗口 drawn/animating，它就变 false并启用 sticky。

所以它不是“Activity 已经没有 WindowState”的事实位，而是“本次聚合是否允许旧 reported true 回落”的控制量。当它保持 true 时，一扇仍存在但未 drawn、非动画的窗口足以让 reported 状态下降；sticky 不是永久 latch。

## 9. 两个边沿的写入顺序不对称，转移与死亡还能绕过边沿

r48 的核心顺序是：

```java
if (nowDrawn != reportedDrawn) {
    onWindowsDrawn(nowDrawn, SystemClock.elapsedRealtimeNanos());
    reportedDrawn = nowDrawn;
}
if (nowVisible != reportedVisible) {
    reportedVisible = nowVisible;
    if (nowVisible) onWindowsVisible();
    else onWindowsGone();
}
```

drawn 分支先调用 consumer：`onWindowsDrawn()`一进入就写 `mDrawn=drawn`，正边沿还会运行 metrics 与 waiter；返回后才写 `reportedDrawn`。因此这些同步 consumer 执行时可观察到“新 `mDrawn`、旧 `reportedDrawn`”。

visible 分支相反：先写 `reportedVisible`，再由 callback 写 `nowVisible`。在一次普通 false→true 汇总里，visible公式蕴含 drawn，且 drawn 分支排在前面，所以 `mDrawn=true`先于 `nowVisible=true`。这只是该方法的一次普通调用顺序，不是所有路径的全局不变量。

三类绕行必须单独记：

- `transferStartingWindow()`中“源已有 startingWindow 与 startingSurface”的分支复制 `reportedVisible`，并可复制 `allDrawn`、`firstWindowDrawn`、容器/请求/client visibility，却不复制 `reportedDrawn`、`mDrawn`或`nowVisible`；若目标 record 得到 `reportedVisible=true`而自身 `nowVisible=false`，下一次 raw visible 仍为 true时不会再产生 visible 边沿，二者可继续分离；仅搬运 pending starting data 的另一分支没有这组复制；
- Activity destroy 路径直接写 `nowVisible=false`，不要求先写 `reportedVisible`；
- 进程死亡但保留 dead window/record 时，`ActivityStack`直接令 `nowVisible=mVisibleRequested`，因此它可以在没有当前真实 draw 正边沿时为 true。

这些字段没有共同 generation id。看日志时必须先确认 ActivityRecord 实例、转移/重建路径和具体写点，不能只按字段名字拼一条时间线。

## 10. mDrawn 与 nowVisible 的消费者不同，且都停在 Framework 语义层

`onWindowsDrawn(false, ...)`只把 `mDrawn`清 false并返回，不会发送“负的启动完成”。正边沿则按顺序查询 metrics、可能结束同步等待、停止 launch ticking，并把 Task 记为曾可见。

| consumer | 触发条件 | 结果 |
|---|---|---|
| `ActivityMetricsLogger.notifyWindowsDrawn` | 每个 reported drawn 正边沿都会尝试 | 更新序列delay并尝试移除首个匹配 pending Activity；可无匹配，也可能返回 null |
| `reportActivityLaunchedLocked` | metrics snapshot 有效，或该 record 正是 DisplayArea top-running | 收口全局 START_SUCCESS 等待表；fallback 可携带 invalid delay/state |
| `stopWaitingForActivityVisible` | 与上一项同一条件分支 | 收口匹配 Component 的 TASK_TO_FRONT visible wait |
| `finishLaunchTickingLocked` | drawn 正边沿 | 停止 Activity 的 launch tick |
| `Task.setHasBeenVisible(true)` | drawn 正边沿且有 Task | 记录 Task 曾达到该 Framework 阶段 |

`onWindowsVisible()`则无条件先尝试 `stopWaitingForActivityVisible(this)`，随后仅在 `nowVisible`原为 false 时写 true、记录 uptime 的 `lastVisibleTime`并安排 App GC；`onWindowsGone()`写 false。

`completeResumeLocked()`若发现 `nowVisible`已经 true，也会尝试停止**已经存在**的 component waiter。它不是“先可见、后注册”的补救：直接 `START_TASK_TO_FRONT`分支在注册前先检查 `nowVisible && RESUMED`，该检查才覆盖前置状态；注册和等待发生在 global lock 内。

另一个消费者是 `RootWindowContainer.allResumedActivitiesVisible()`：必须至少找到一个 resumed Activity，并要求每个 resumed record 的 `nowVisible=true`。动画结束后，这可让系统继续安排 stopping/finishing Activity，而不必等一个可能迟迟不到的 idle。这里仍只是在调度 Framework 生命周期工作。

## 11. Metrics 的 pending Activity 集合与 transition-start 构成双门

`TransitionInfo`用 `LinkedList<ActivityRecord> mPendingDrawActivities`记录同一 launch sequence 还欠 drawn 的 Activity。设置“最近启动 Activity”时，只有它不是连续重复的同一 record、不是 `noDisplay`且当时 `mDrawn=false`，才加入链表；已 drawn 与 no-display Activity 不欠这一门。

trampoline 可以让 A→B 等多个 Activity 合并进同一 transition。`TransitionInfo.allDrawn()`没有窗口统计，只返回链表是否为空。`notifyWindowsDrawn(r, timestamp)`找到 active info 后：

1. 用传入的 elapsed-realtime 时间计算并覆盖 transition 的 windows-drawn delay；
2. `removePendingDrawActivity(r)`尝试移除链表中首个匹配；若 r只是当前 `mLastLaunchedActivity`而不在pending中，这一步可以无操作；
3. 生成 transition snapshot；其 Activity元数据来自 `mLastLaunchedActivity`，不保证就是本次通知参数 r；
4. 若 transition-start 已记录且链表现在为空，才 `done(false, ...)`。

另一方向，`notifyTransitionStarting()`第一次记录 transition delay/reason；重复通知被忽略。若此时 pending 已空，也立即成功 `done`。所以两门顺序无关：

```text
pending activities empty ─┐
                          ├─> successful transition done
transition-start logged ──┘
```

一次成功的 `notifyWindowsDrawn`可以在整个序列 done 之前返回 transition snapshot，于是调用它的 `onWindowsDrawn`已经能够用该 snapshot 的 delay/launchState结束同步 waiter；`reportActivityLaunchedLocked()`收到的 record仍是调用方自身。A→B合并序列中若A先draw，delay可由A的timestamp更新，而 snapshot里的Activity元数据已经是latest B。这更不等于后台 `Displayed`日志已经输出。

链表本身也没有通用 identity 去重，只挡连续重复的 `mLastLaunchedActivity`。静态上若同一 record 以 A→B→A 非连续回到序列、两次加入时都仍 `mDrawn=false`，链表可出现两个 A；一次 `remove(r)`只移除首个匹配，而 reported drawn 正边沿通常只来一次。排查序列迟迟不 empty 时，这是一条值得核对的 r48 边界，而不是可以忽略的集合性质。

## 12. starting、取消、日志与 observer 各有自己的完成边界

`notifyStartingWindowDrawn()`只在首次命中时记录 `mStartingWindowDelayMs`，不从 pending draw 链表移除 Activity。starting preview 因此既不是 windows-drawn，也不会单独输出 `Displayed`。

新启动通知若发现目标 `mDrawn && isVisible()`，metrics 会认为无法测量 draw delay并 abort。启动中 visibility 变为不再 requested或 finishing 时，会先从 pending移除该 Activity；若它是序列最后启动者，还异步回到 global lock 检查 Task 中是否仍有 `mVisibleRequested && !mDrawn && !finishing`的 Activity，没有就 cancel/abort。清空 pending与成功完成不是同义词：成功仍要求 transition-start 门。

`done(false, ...)`才进入 `logAppTransitionFinished()`；它先做 snapshot，在序列值得记录时投递 transition log，并总是把 `logAppDisplayed()`任务投到 `BackgroundThread`。r48 的 `logAppDisplayed()`只接受 WARM/COLD，HOT transition 进入后台任务后直接返回。因此同步 `WaitResult`已返回、observer已排队、甚至 transition已 done，都不能保证 logcat 有一行 `Displayed`。

Launch observer 注册表也明确把所有回调按顺序投到后台 Handler，避免在 WM critical section 中运行未知 observer。它保持 observer 事件的异步顺序，但调用者不会等 observer 实际消费后才继续。

`mLastTransitionInfo`还有一处所有权边界：r48 只在新建 `TransitionInfo`时把当时的 launched record放入映射；随后 trampoline 的 `setLatestLaunchedActivity()`不为新 record补同类映射。因而不能假定合并进序列的每个后续 Activity 都能以自己为 key 找到 fully-drawn info。

## 13. START_SUCCESS 等的是无目标身份的全局表，不是目标专属 Future

`ActivityStarter.waitForResult()`收到 `START_SUCCESS`时，把调用者的 `WaitResult`加入 `mWaitingActivityLaunched`，然后在 ATMS global lock 上循环等待，条件为：result尚未变成 `START_TASK_TO_FRONT`、未 timeout、`who==null`。`wait()`释放锁；被唤醒后重获同一锁，再重新判断，因而能抵抗无关 `notifyAll()`。

但这张表没有 Activity/component key。`reportActivityLaunchedLocked()`从尾到头移除**全部**条目，把同一个 Activity、time与launchState写给所有 `who==null`的 waiter，然后 `notifyAll()`；它刻意不改原 result。

`reportWaitingActivityLaunchedIfNeeded()`遇到嵌套启动结果 `START_DELIVERED_TO_TOP`或`START_TASK_TO_FRONT`时也移除整张表：

| 收口源 | `result` | `who` | 时间/状态 |
|---|---|---|---|
| 有效 windows-drawn snapshot | 保持原值 | 报告该 record 的 Component | snapshot delay；COLD/WARM/HOT |
| top-running fallback | 保持原值 | top record Component | invalid delay，launchState=-1 |
| idle timeout | 保持原值 | timeout关联 record（若非空） | timeout=true、invalid delay、-1 |
| 嵌套 DELIVERED_TO_TOP | 改为该 result | 填当前 Component | 不由这里补完整计时 |
| 嵌套 TASK_TO_FRONT | 改为该 result | 可仍为 null | 循环仅凭 result 已可退出 |

这意味着并发 `startActivityAndWait`/`am start -W`等待者可能被同一次完成广播一起收口，不能解释成每项严格绑定最初目标。`InterruptedException`也只被吞掉后继续按条件循环，不是一个完成结果。

直接 `START_DELIVERED_TO_TOP`根本不进表：立即写 `who`、`totalTime=0`；其 `launchState`可能保留默认值。正常 snapshot能给三种 launch state；降级路径可给 `-1`，shell层应准备把未知值显示为 UNKNOWN，而不是强行归类。

## 14. TASK_TO_FRONT 另用 Component 表，而且没有有效的本地 timeout

直接 `START_TASK_TO_FRONT`先只按 `attachedToProcess()`写 HOT或COLD，没有 WARM。若 Activity已 `nowVisible && RESUMED`，立即写 Component与 `totalTime=0`；否则注册 `WaitInfo(component, result)`到 `mWaitingForActivityVisible`，再在 global lock 上等待。

源码注释直说这里的 timeout变量当前不会被设置。因此这张表没有独立有效超时；`activityIdleInternal(fromTimeout=true)`调用的只是 `reportActivityLaunchedLocked()`，只能清上一节的全局 launched 表，不能清 component-visible 表。

component waiter 的收口路径是：

- `onWindowsVisible()`总会尝试停止；
- `onWindowsDrawn()`仅在 metrics snapshot有效或 record为top-running时停止；
- `completeResumeLocked()`在 waiter已存在且 `nowVisible=true`时停止；
- `cleanupActivity()`用 `INVALID_DELAY`停止，保证 record清理时协议能退出。

`stopWaitingForActivityVisible()`按 `ComponentName`从尾到头匹配并完成所有命中项，不按 `ActivityRecord`实例、task、启动 generation 或 FIFO绑定。同一 Component 的多实例/并发等待可以被一个 record 一起收口。默认无显式 time 的调用还会读取 `getLastDrawnDelayMs(r)`，它可能是 invalid，也可能来自保留的 last-transition 映射。

所以两张表必须分开排障：

| 表 | 注册入口 | 匹配粒度 | idle timeout是否收口 |
|---|---|---|---|
| `mWaitingActivityLaunched` | START_SUCCESS | 无目标身份；全表广播 | 是 |
| `mWaitingForActivityVisible` | 直接 TASK_TO_FRONT 且尚未 visible/resumed | ComponentName；完成所有匹配项 | 否 |

任一等待返回都只证明它的退出条件被某条路径满足；cleanup、fallback、嵌套 result 都可能在没有新像素 present 的情况下终止协议。

## 15. reportFullyDrawn 是业务可用声明，既非 reportedDrawn 也非 present

App 调用 `Activity.reportFullyDrawn()`时，客户端先把 `mDoReportFullyDrawn=false`，再同步调用 ATMS，随后在同一个 try 块里执行 `VMRuntime.notifyStartupCompleted()`。普通重复调用被 one-shot flag忽略；Activity pause/stop也会把该 flag清 false。若 Binder 抛 `RemoteException`，同一 try 块后面的 VMRuntime通知也不会执行。

ATMS在 global lock 内按 token找 record；不存在就返回。存在时，`ActivityMetricsLogger.logAppTransitionReportedDrawn()`用 `mLastTransitionInfo.get(r)`找启动信息：没有映射就忽略。若该 transition 的 pending draw Activity 还未空且尚未保存 callback，它只存一个 `mPendingFullyDrawn` Runnable并返回 null。

这个 deferred Runnable只在**成功**的 `logAppTransitionFinished()`路径运行；abort分支不会运行它。还有一个反直觉边界：pending尚未空、但第一个 Runnable 已经存在时，对同一映射 record 的第二次 server-side 调用不会再进入保存分支，而会继续记录。正常 `Activity` API 的 one-shot会抑制这种重复；合并序列中的后续 record通常又没有自己的 `mLastTransitionInfo`映射，因此这主要说明 server guard不是通用去重，不能据此虚构一个常规“第二 Activity复用 callback”的路径。成功后递归计算 deferred fully-drawn 时，代码采用 `mWindowsDrawnDelayMs`作为 startup time，而不是 App 最早调用 Binder 的时刻。这把过早的业务声明下移到系统 windows-drawn 下界。

客户端却不会等待后台 metrics 真正写出：只要 ATMS Binder方法正常返回，即使服务端刚把 Runnable存起来，它就继续通知 VMRuntime。只有某次直接 `ActivityRecord.reportFullyDrawnLocked()`调用从 logger拿到非空 snapshot时，wrapper才会调用 `reportActivityLaunchedLocked()`，从而可能用 fully-drawn delay收口仍存在的全局 START_SUCCESS waiter。日后由成功 transition运行的 deferred Runnable只递归进入 logger并丢弃返回值，不会再经过这个 wrapper，也就不会自行收口 waiter。

因此三个概念必须分开：

```text
reportedDrawn / mDrawn：WMS窗口统计边沿
reportFullyDrawn：App声明重要数据与UI已可用，并受metrics规则校正
present：显示系统和硬件的物理完成
```

## 16. 九组只读练习、排障顺序与下一章边界

下面命令只搜索或打印源码，不修改工作树。路径与模式均针对本章的 r48 基线。

### 练习 1：确认字段所有者与初值

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'private boolean mVisible;' \
  -e 'boolean nowVisible;' \
  -e 'boolean mDrawn;' \
  -e 'boolean mVisibleRequested;' \
  -e 'private boolean reportedDrawn;' \
  -e 'boolean reportedVisible;' \
  -e 'mClientVisible = true;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

分别标注“请求、client control、容器提交、边沿记忆、上层状态”，并解释为什么构造时 client true不等于已送达一次回调。

### 练习 2：追 requested 到 commit 的分叉

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mVisibleRequested = visible;' \
  -e 'setClientVisible(true);' \
  -e 'appTransition.isTransitionSet()) {' \
  -e 'commitVisibility(visible, true /* performLayout */);' \
  -e 'void commitVisibility(boolean visible, boolean performLayout)' \
  -e 'setVisible(visible);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

画出“无转场立即commit”和“加入opening/closing后return”两条路线。

### 练习 3：定位第一套聚合的 placement 入口

```bash
cd /Users/ninebot/androidSource
rg -n -F -e 'mWmService.mTransactionSequence++;' \
  frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
rg -n -F \
  -e 'winAnimator.commitFinishDrawingLocked();' \
  -e 'activity.updateDrawnWindowStates(w);' \
  -e 'activity.updateAllDrawn();' \
  frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

按源码行号确认单窗commit、逐窗计数、遍历后Activity闭门的顺序。

### 练习 4：证明 evaluated 不随 transaction sequence 重置

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'setDrawnStateEvaluated(false /*evaluated*/);' \
  -e 'boolean getDrawnStateEvaluated()' \
  -e 'boolean mightAffectAllDrawn()' \
  frameworks/base/services/core/java/com/android/server/wm/WindowState.java
rg -n -F \
  -e 'mLastTransactionSequence != mWmService.mTransactionSequence' \
  -e 'allDrawnStatesConsidered()' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

把 parent-change bit与每轮数字清零分别写成一句话。

### 练习 5：为 reported 单窗贡献制作真值表

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'void updateReportedVisibility(UpdateReportedVisibilityResults results)' \
  -e 'mAttrs.type == TYPE_APPLICATION_STARTING' \
  -e 'results.numInteresting++;' \
  -e 'results.numDrawn++;' \
  -e 'results.numVisible++;' \
  -e 'results.nowGone = false;' \
  frameworks/base/services/core/java/com/android/server/wm/WindowState.java
```

特别写出“存在、合格、未drawn、非动画”时为什么 `nowGone`仍不被清 false。

### 练习 6：核对 raw、sticky 与不对称写序

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'boolean nowDrawn = numInteresting > 0 && numDrawn >= numInteresting;' \
  -e 'boolean nowVisible = numInteresting > 0 && numVisible >= numInteresting && isVisible();' \
  -e 'nowDrawn = reportedDrawn;' \
  -e 'nowVisible = reportedVisible;' \
  -e 'onWindowsDrawn(nowDrawn, SystemClock.elapsedRealtimeNanos());' \
  -e 'reportedDrawn = nowDrawn;' \
  -e 'reportedVisible = nowVisible;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

说明 consumer分别能看见哪一边的新旧值，并给出 normal false→true 的局部顺序。

### 练习 7：找出强制、复制与直接覆盖

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'app.allDrawn = true;' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
rg -n -F \
  -e 'reportedVisible = fromActivity.reportedVisible;' \
  -e 'if (fromActivity.allDrawn) {' \
  -e 'nowVisible = false;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'r.nowVisible = r.mVisibleRequested;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityStack.java
```

逐项回答它是否携带窗口draw证据、是否触发 callback、是否复制 consumer字段。

### 练习 8：验证 Metrics 双门和异步输出

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mPendingDrawActivities.add(r);' \
  -e 'return mPendingDrawActivities.isEmpty();' \
  -e 'info.removePendingDrawActivity(r);' \
  -e 'info.mLoggedTransitionStarting && info.allDrawn()' \
  -e 'if (info.allDrawn()) {' \
  -e 'BackgroundThread.getHandler().post(() -> logAppDisplayed(infoSnapshot));' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
```

分别模拟“先draw后transition-start”和“先transition-start后draw”，确认只成功done一次。

### 练习 9：对照两张 waiter 表与 fully-drawn

```bash
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mWaitingActivityLaunched.add(mRequest.waitResult);' \
  -e 'r.nowVisible && r.isState(RESUMED)' \
  -e 'waitActivityVisible(r.mActivityComponent, mRequest.waitResult);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
rg -n -F \
  -e 'WaitResult w = mWaitingActivityLaunched.remove(i);' \
  -e 'if (w.matches(r.mActivityComponent)) {' \
  -e 'stopWaitingForActivityVisible(r, WaitResult.INVALID_DELAY);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
rg -n -F \
  -e 'public void reportFullyDrawn()' \
  -e 'mDoReportFullyDrawn = false;' \
  -e 'VMRuntime.getRuntime().notifyStartupCompleted();' \
  frameworks/base/core/java/android/app/Activity.java
```

为每个返回点注明“draw正边沿、visible正边沿、直接结果、timeout、cleanup或业务声明”，不要只写“启动完成”。

排障时按这个顺序最省力：

1. 先用 `ActivityRecord`实例与 token把日志归属分开，确认是否发生 starting-window transfer、relaunch、destroy或进程死亡保留；
2. 若卡在成组show，检查 main-window预占、`mNumInterestingWindows/mNumDrawnWindows`、`allDrawnStatesConsidered()`与 opening-app归属；
3. 若 `allDrawn=true`但上层未动，单独重建 reported 的筛选、raw值、`nowGone`和sticky结果，不假设两套聚合相连；
4. 若 `mDrawn=true`但启动序列未done，检查 `mPendingDrawActivities`是否还有别的 record或重复项，以及 transition-start门；
5. 若同步命令不返回，先辨认它在全局 launched 表还是 Component表；后者没有本地有效timeout；
6. 若只缺 `Displayed`，先确认是否HOT，再考虑后台Handler时序；
7. 若问题是“画面何时真的出现”，离开这些boolean，转向 SurfaceFlinger transaction、Buffer latch、composition与present fence证据。

源码导航：

```text
frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
frameworks/base/services/core/java/com/android/server/wm/WindowState.java
frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
frameworks/base/services/core/java/com/android/server/wm/LaunchObserverRegistryImpl.java
frameworks/base/services/core/java/com/android/server/wm/ActivityStarter.java
frameworks/base/services/core/java/com/android/server/wm/ActivityStack.java
frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java
frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java
frameworks/base/core/java/android/app/Activity.java
frameworks/base/core/java/android/app/WaitResult.java
```

本章的最小心智模型是：`allDrawn`关“WindowState集合能否成组放行”；reported字段关“重算结果是否形成新边沿”；`mDrawn/nowVisible`关“Framework消费者现在采用什么状态”；Metrics的同名 `allDrawn()`关“launch sequence还有没有Activity欠账”；两张 `WaitResult`表又各自定义同步调用何时退出。它们都不是 present fence。

下一章进入第 221 章“Android AppTransitionController opening/closing apps 与转场启动条件”，继续追 `allDrawn`、`startingDisplayed`、`startingMoved`怎样参与 readiness，opening/closing 集合怎样选动画目标并提交可见性。
