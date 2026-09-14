# 226 Android Quickstep手势判定、GestureState与MultiStateCallback异步状态机

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP  
> 核心路径：`TouchInteractionService → OtherActivityInputConsumer → BaseSwipeUpHandlerV2 → GestureState / MultiStateCallback`

## 1. 固定场景：抬手不是结束，而是提交一个异步事务

上一章已经看到 `TaskViewSimulator` 怎样把手指位移变成逐帧 `SurfaceControl.Transaction`。本章固定一个更长的场景：

1. 前台是普通 App，用户从导航区域按下；
2. 手势先越过“窗口可移动”门槛，再越过“Quickstep 接管输入”门槛；
3. 抬手时，Launcher 选择 `HOME`、`RECENTS`、`NEW_TASK` 或 `LAST_TASK`；
4. 终点动画、RecentsView 滚动、Launcher 首帧、截图替换和 Recents controller 回调各自在不同时间到达；
5. 只有规定的状态组合齐备，才能执行对应的收尾动作。

这里最容易产生的误解，是把 `ACTION_UP` 当作整个切换已经完成。源码中它只完成了“输入采样”这一段，并把 Handler 内部的 `STATE_GESTURE_COMPLETED` 置位。终点会在 UP 路径中同步算出；但发布 `END_TARGET_SET` 与真正启动终点动画可能要等 Recents targets 到达，后续还有两层状态机继续推进。

可以把整个过程写成偏序，而不是一条固定时间线：

```text
输入事件 ──> 手势开始 ──> ACTION_UP
                         │
Recents controller ─────┼──> 终点动画
Launcher create/start/draw ──> 截图替换
RecentsView scroll ──────────> settled
                         │
                         └──> finish / launch / resume / invalidate
```

本章的观察点有四个：

- `OtherActivityInputConsumer`：何时预热动画、移动窗口、pilfer 输入和结束采样；
- `calculateEndTarget()`：四个终点怎样由位移、速度、页面和导航模式共同决定；
- `GestureState`：跨对象保存的 Recents 生命周期与最终目标；
- `BaseSwipeUpHandlerV2.mStateCallback`：当前 Handler 的 16 个软件门闩。

这些状态位描述的是“客户端已经走到哪一步”，不自动等价于 Surface 已显示、system_server 已提交或 Binder 已成功。

## 2. 输入入口：同一事件先经过坐标变换、消费者选择和缓存分发

输入链从 SystemUI 请求的 gesture monitor 开始：

```text
SystemUiProxy.monitorGestureInput("swipe-up", displayId)
  → InputMonitorCompat.getInputReceiver(mainLooper, mainChoreographer, listener)
  → InputChannelCompat.InputEventReceiver.onInputEvent()
  → TouchInteractionService.onInputEvent()
  → mUncheckedConsumer.onMotionEvent()
  → OtherActivityInputConsumer.onMotionEvent()
```

`InputChannelCompat.InputEventReceiver` 先调用 listener，返回后才执行
`finishInputEvent(event, true)`。这里没有 `try/finally`，所以“总会确认 handled”不是这段代码能保证的性质。

`TouchInteractionService` 在 `ACTION_DOWN` 上先按显示旋转修正事件，再判断是否位于 swipe-up 区域。命中后它做了一件看似重复、实则用途不同的事：

```java
GestureState prevGestureState = new GestureState(mGestureState);
GestureState newGestureState = createGestureState(mGestureState);
mConsumer.onConsumerAboutToBeSwitched();
mGestureState = newGestureState;
mConsumer = newConsumer(prevGestureState, mGestureState, event);
```

`prevGestureState` 是一个临时副本，供消费者切换期间查询；它与旧对象共享
`MultiStateCallback` 和当时的 appeared-task 集合引用，其余字段只复制当下的值。
真正承载新手势的是
`createGestureState()` 创建的新对象，它拥有新的 callback。若
`TaskAnimationManager` 仍有 controller，新对象只转移 running task、last-started task id
和 appeared-task 集合，不继承旧手势的终点与整套生命周期状态。

到达 `OtherActivityInputConsumer` 后，事件还会先尝试分发给 RecentsView：

- 首次取得 RecentsView consumer 时，会补发一个临时的
  `ACTION_MOVE_ALLOW_EASY_FLING`；
- 代理分发期间临时加上 `EDGE_NAV_BAR`，随后恢复原 flags；
- 原事件恢复后才送进 `VelocityTracker`，因此 tracker 不会把临时 action/edge flag 当作输入事实。

这解释了为什么“Overview 横向翻页”和“Quickstep 自身的纵向位移判定”可以观察同一串触摸，却保留不同的解释层。

## 3. 两道 slop：一维窗口门槛与二维接管门槛

源码维护三个布尔量：

```text
mPassedWindowMoveSlop   窗口可以开始跟手
mPassedPilferInputSlop  客户端已越过或视为越过 pilfer 门槛
mPassedSlopOnThisGesture 本次触摸自身是否越过二维门槛
```

第一道门槛只看从导航栏向屏幕中心的主轴位移：

```java
if (Math.abs(displacement) > mTouchSlop) {
    mPassedWindowMoveSlop = true;
    mStartDisplacement = Math.min(displacement, -mTouchSlop);
}
```

它使用严格的 `>`。正常从底边向上时 `displacement` 为负；一旦越界，
`Math.min(displacement, -mTouchSlop)` 通常保存的是这一帧的完整负位移。因此紧接着的
`updateDisplacement(displacement - mStartDisplacement)` 往往从 0 开始，而不是简单地“扣掉一个 touch slop”。

第二道门槛看二维平方距离：

```text
dx² + dy² >= ratio × touchSlop²
```

全手势模式的 `ratio` 是 2，其他非三键模式是 9；换算成欧氏距离分别是
`√2 × touchSlop` 和 `3 × touchSlop`，不是 2 倍和 9 倍距离。它也不是“方向 slop”：
是否禁用横滑是在二维门槛通过后，另用 `abs(dx) > abs(dy)` 检查。

两道门槛允许一种中间状态：窗口已经跟手，但 pilfer 请求还没有发出。即使
`mPassedPilferInputSlop` 变成 `true`，它也只是客户端门闩：普通路径先置位再调用
oneway `pilferPointers()`，续接路径更会在构造时直接预置，均不证明 InputDispatcher
已经处理接管。此时
`ACTION_UP` 仍会进入 Handler 的结束逻辑，只是 `mGestureStarted` 可能还是 `false`，所以不会被判成 fling。

## 4. 正常启动、延迟启动与续接不是同一路径

普通目标在 `ACTION_DOWN` 就调用 `startTouchTrackingForWindowAnimation(eventTime)`。这不是宣布手势成立，而是尽早：

1. 创建 `BaseSwipeUpHandler`；
2. 安装 gesture-end callback 与 motion-pause listener；
3. 调用 `initWhenReady(intent)`；
4. 向 `TaskAnimationManager` 请求 Recents animation。

延迟目标来自两个条件之一：

```text
Home 与 Overview 不是同一组件
或 activityInterface.deferStartingActivity(deviceState, event) 返回 true
```

正在续接 Recents animation 时，构造器会强制取消 deferred；否则它要等二维 pilfer
门槛通过的那一个 `ACTION_MOVE`，才用该 MOVE 的 `eventTime` 创建 Handler 并请求动画。
同一帧还会补齐 window slop、记录 start displacement，然后 pilfer。

续接路径更特殊。构造器一开始就把 window/pilfer 两个门闩置为 `true`，随后在
`ACTION_DOWN` 创建 Handler 时：

```text
continueRecentsAnimation(new GestureState)
  → addListener(handler)
  → notifyRecentsAnimationState(handler)
  → notifyGestureStarted(true)
```

所以它不等待新手势再次越过 slop。`mPassedSlopOnThisGesture` 仍从 `false` 开始，
用于区分“续接门闩已预置”和“本次触摸自身确实越过了二维 slop”。

`notifyGestureStarted()` 的顺序也值得保留：

```text
handler 非空
  → inputMonitor.pilferPointers()
  → closeOverlay()
  → closeSystemWindows()
  → handler.onGestureStarted(...)
```

`pilferPointers()` 返回只说明客户端调用已经发出，不能单凭这一行断言
InputDispatcher 已完成接管或目标窗口已经处理 `CANCEL`。

## 5. 多指、暂停与“可能切换任务”的三个边界

每个 `ACTION_POINTER_UP` 在进入 switch 之前都会执行：

```java
mVelocityTracker.clear();
mMotionPauseDetector.clear();
```

这与“抬起的是不是 active pointer”无关。若抬起 active pointer，源码再选 0/1 中的另一个
pointer，并根据上一帧 `mLastPos - mDownPos` 反推新的 down position，保持累计位移连续。
它保留的是“上一帧已记录位移”，不保证包含 pointer-up 事件中最新的坐标变化。

还有一个隐蔽后果：`MotionPauseDetector.clear()` 会清空 listener。若 clear 时 Handler
与 listener 已经创建，Consumer 不会在 pointer 切换后重新安装它，因此此后即使再次
停住，也不会把 pause 变化回调给 Handler；如果之前 shelf 已处于 peek，Handler 自己的
`mIsShelfPeeking` 还可能继续影响终点选择。反之，deferred 手势若在 Handler 创建前就发生
POINTER_UP，后来首次越过二维门槛时，`startTouchTrackingForWindowAnimation()` 仍会首次
安装 listener，不能把这一分支也算成永久丢失。

在 pilfer 之前，第二根手指若落在 swipe-up 区域外，`forceCancelGesture()` 会把当前
`MotionEvent` 临时改成 `ACTION_CANCEL`，调用本地结束逻辑，再恢复原 action。它不会凭这一动作自动 pilfer，也不能单独证明 App 已收到取消。

MOVE 阶段还计算：

```text
horizontalDist = abs(dx)
upDist = -displacement
likelyNewTask =
    （续接已成立但本次还没过二维 slop）
    或 horizontalDist > upDist
```

注意第二项不是 `horizontalDist > abs(upDist)`。向 App 方向下拉时 `upDist` 为负，
正的水平距离几乎必然大于它，因此也会暂时得到 `true`。全手势模式会先把这个结果用于
`setDisallowPause()`，再向 pause detector 添加位置，最后调用 Handler 的
`setIsLikelyToStartNewTask()`。前两步本身都可能同步改变 pause，并通过 listener 更早回调
Handler，所以这里的“最后”只限定 likely-new-task setter 的调用顺序。

## 6. ACTION_UP：速度坐标系、取消路径与延迟清理

UP 事件先进入 RecentsView 代理和 `VelocityTracker.addMovement()`，随后
`finishTouchTracking()` 才计算速度。符合 window slop 且 Handler 非空时：

```text
computeCurrentVelocity(1000, maxFlingVelocity)
velocityX / velocityY = 当前 active pointer 的 X/Y 速度
endVelocity =
  右边导航栏： velocityX
  左边导航栏：-velocityX
  底部导航栏： velocityY
最后一次 updateDisplacement()
handler.onGestureEnded(endVelocity, PointF(velocityX, velocityY), downPos)
```

`endVelocity` 的符号统一为“从导航栏朝屏幕中心为负”。但 `PointF` 保留经过
`TouchInteractionService` 旋转修正后的 X/Y 分量；后面的部分动画计算仍直接读
`velocity.y`，这一点在侧边导航栏上尤其重要。

`ACTION_CANCEL` 只在 window slop 已通过且 Handler 存在时调用
`onGestureCancelled()`。它先把 displacement 归零，再置
`STATE_GESTURE_COMPLETED`，最后以 `isCancel=true` 进入普通终点计算，因此落到
`LAST_TASK`。

若 window slop 未通过，则不会调用 Handler 的 ended/cancelled：

```text
onConsumerAboutToBeSwitched()
  → onInteractionGestureFinished()
  → 100 ms 后请求 cancelRecentsAnimation(restoreHomeStackPosition=true)
```

这个延迟任务没有 gesture generation 标识；它只是针对竞态的兜底。相反，window
slop 已通过的 UP 不会立刻让 detached consumer 完成，Consumer 会等 Handler 的
invalidation callback。

## 7. 四个终点：先算 goingToNewTask，再按导航模式决策

`GestureEndTarget` 不只是四个名字，还携带两个行为属性：

| 终点 | `isLauncher` | `recentsAttachedToAppWindow` | 业务含义 |
|---|---:|---:|---|
| `HOME` | true | false | 回 Workspace |
| `RECENTS` | true | true | 停在 Overview |
| `NEW_TASK` | false | true | 启动或切到另一任务 |
| `LAST_TASK` | false | true | 恢复应返回的任务 |

`goingToNewTask` 的计算先于 fling/non-fling 分支：

- `mRecentsView == null`：`false`；
- 有 view 但 `hasTargets() == false`：直接 `true`；
- 有 targets：`runningTaskIndex >= 0 && nextPage != runningTaskIndex`。

“没有 targets 就是新任务”是客户端的路径假设，可能意味着续接，也可能是 start 尚未到达、空集合或引用已清理，不能把它写成已证明的历史事实。

非 fling 决策可以压缩成：

```text
cancel                         → LAST_TASK
全手势 + shelf peeking         → RECENTS
全手势 + goingToNewTask        → NEW_TASK
全手势 + shift < 0.7           → LAST_TASK
全手势 + 其余                  → HOME
非全手势 + shift >= 0.7
          + gestureStarted     → RECENTS
非全手势 + goingToNewTask      → NEW_TASK
非全手势 + 其余                → LAST_TASK
```

fling 先用 `endVelocity < 0` 判断朝屏幕中心，再判断
`goingToNewTask && abs(velocity.x) > abs(endVelocity)`：

- 全手势、向上且 `willGoToNewTaskOnSwipeUp` 不成立：`HOME`；
- 全手势、向上、`willGoToNewTaskOnSwipeUp` 成立且 shelf 未 peek：`NEW_TASK`；
- 其他向上：未过 0.7 且 `willGoToNewTaskOnSwipeUp` 成立时为 `NEW_TASK`，否则 `RECENTS`；
- 向下：`goingToNewTask` 为 `true` 时是 `NEW_TASK`，否则 `LAST_TASK`。

最后，Overview 被策略禁用且候选是 `RECENTS` 或 `LAST_TASK` 时，结果统一为
`LAST_TASK`。这一步不会把 `HOME` 或 `NEW_TASK` 改掉。

## 8. 两个速度阈值与一个轴向缺口

`onGestureEnded()` 用资源 `quickstep_fling_threshold_velocity` 判断“是不是 fling”：

```java
isFling = mGestureStarted && abs(endVelocity) > flingThreshold;
```

进入 `handleNormalGestureEnd()` 后，又读取
`quickstep_fling_min_velocity`，决定是否启用基于速度的 duration/overshoot
计算。这是两道用途不同的阈值。当前资源值分别为 500 dp/s 和 250 dp/s，所以一旦已被
判成 fling，在长度大于 0 时也会通过第二道速度检查；不能把二者合并成一枚阈值，长度
guard 仍是独立条件。

非 fling 的普通进度动画根据剩余 shift 计算 duration，并限制在 350 ms 内。
`RECENTS` 使用 `OVERSHOOT_1_2`。fling 则先预测下一帧起点：

```java
startShift = boundToRange(
    currentShift - velocityPxPerMs.y * singleFrameMs / transitionDragLength,
    0,
    dragLengthFactor);
```

这里直接使用原始 `velocity.y`，且除法发生在 `transitionDragLength > 0` 检查之前。
长度为 0 时 Java 浮点运算会产生无穷或 NaN；`boundToRange` 不能把 NaN 修成有效
progress。普通 fling duration 同样用 `velocityPxPerMs.y` 作分母，而
`calculateEndTarget()` 用的却是已按导航栏方向归一化的 `endVelocity`。因此侧边导航栏
存在“终点按 X 决策、时长按 Y 计算”的轴向缺口。

非全手势 `RECENTS` 的高速分支用 `OvershootParams`；其他高速分支按
`2 × abs(distance / velocity.y)` 估时，`RECENTS` 仍可能选择
`OVERSHOOT_1_2`。所以不能把 overshoot 简化为“只属于双按钮”。

`HOME` 还要单独理解：公共代码虽算出 `endShift=1`，HOME 分支却不把 `endShift` 或
interpolator 交给几何动画。它先把 nominal duration 钳制为至少 120 ms，再把 duration
传给 `createHomeAnimationFactory(duration)`，而 `createWindowAnimationToHome()` 只接收
`startShift` 与 factory；leash 的几何终点动画实际是 `RectFSpringAnim`，没有把该
duration 直接设为 spring 时长。Launcher handler 在 Activity 已存在时返回按 accuracy
创建的 workspace controller，并另启 staggered animation；Activity 尚不存在时的空
controller 仍使用 duration。Fallback handler 也把 duration 用于 alpha/controller。
因此不能从剩余 shift 直接推导 HOME spring 必然更短。

## 9. 终点动画：目标先发布，完成位稍后发布

`animateToProgressInternal()` 先调用：

```java
mGestureState.setEndTarget(target, false);
```

它立即写 `mEndTarget` 并置 `STATE_END_TARGET_SET`，但不置
`STATE_END_TARGET_ANIMATION_FINISHED`。后者由 HOME spring 或普通
`ValueAnimator` 的 success listener 发布。

非 HOME 动画的 success listener 在 controller 仍非空且 `mRecentsView` 存在时，还会做一次目标纠正：

```text
原目标 NEW_TASK
  且 nextPage == lastAppearedTaskIndex
  且尚未调用过 startActivityFromRecents
    → 改成 LAST_TASK

原目标 LAST_TASK
  且 lastStartedTaskId != -1
    → 改成 NEW_TASK
```

`getLastAppearedTaskIndex()` 在 last-appeared id 为 `-1` 时才退回 running-task index；
若 id 存在但 `getTaskIndexForId()` 查不到，它会直接返回 `-1`，不会再次 fallback。
`hasStartedNewTask()` 只表示 last-started id 不是 `-1`，不证明启动成功或 task 已出现。
纠正发生在 Animator 的结束回调内，而不是动画播放结束之前。更细看一步，纠正调用的是
默认 `setEndTarget()`：它在改目标后会顺带置
`STATE_END_TARGET_ANIMATION_FINISHED`；listener 末尾又显式置一次同一位。因此纠正与
finished 发布嵌套在同一次结束回调中，后一次 set 只是幂等重复。

`setEndTarget(target)` 的默认参数会连续置
`END_TARGET_SET` 与 `END_TARGET_ANIMATION_FINISHED`。源码把这种用法称为 atomic，
含义只是调用者无需稍后再发布 finished；机械执行仍是“赋值 → setState（可触发回调）
→ 日志 → 再 setState”，不是不可插入观察的原子指令。若 `END_TARGET_SET` 的
run-once callback 已经消费过，后续纠正目标不会让它再次运行。

还有一个取消语义陷阱。普通 `Animator.cancel()` 会触发
`AnimationSuccessListener.onAnimationCancel()`，因而屏蔽 success；但
`RectFSpringAnim.cancel()` 只通知 update listener 的 `onCancel()`，然后调用
`end()`，最终向 Animator listener 发送 `onAnimationEnd(null)`。因此 HOME spring
的“cancel”仍可能走 success listener；回调内部还要用 controller 是否为空来挡住后续状态推进。

## 10. GestureState：生命周期账本不等于 controller 是否存在

`GestureState` 有九个状态位：

```text
END_TARGET_SET
END_TARGET_ANIMATION_FINISHED
RECENTS_ANIMATION_INITIALIZED
RECENTS_ANIMATION_STARTED
RECENTS_ANIMATION_CANCELED
RECENTS_ANIMATION_FINISHED
RECENTS_ANIMATION_ENDED
OVERSCROLL_WINDOW_CREATED
RECENTS_SCROLLING_FINISHED
```

`INITIALIZED` 表示客户端已经把“请求启动 Recents animation”的本地工作排入队列，
不是 Binder 已发送，更不是 controller 已返回。启动异常若没有走到 end 回调，
这个状态账本可能长期保留“running”的判断。

两个同名问题有不同答案：

```text
GestureState.isRecentsAnimationRunning()
  = INITIALIZED 已置 && ENDED 未置

TaskAnimationManager.isRecentsAnimationRunning()
  = 当前 controller != null
```

前者是历史状态位推导，后者是当前引用检查；在请求已排队但 controller 未到、取消处理中、
或本地 finish 已先清理引用时，它们可以不一致。

平台 cancel 的 Binder 回调先被调度到 main thread；进入
`BaseSwipeUpHandlerV2.onRecentsAnimationCanceled()` 后，如果本来就在主线程，
`setStateOnUiThread(CANCELLED | HANDLER_INVALIDATED)` 会同步执行相关 callbacks，
然后 `super` 才清 controller 与 targets。这个顺序是刻意保留的局部时序，不应再把
Handler 内的 set 描述成“一定异步”。

GestureState 自己则在 cancel 时依次置 `CANCELED`、`ENDED`，正常 finish 时依次置
`FINISHED`、`ENDED`。`ENDED` 是统一的终止门闩，前两个位记录原因。

## 11. MultiStateCallback：位掩码、一次性队列与重入顺序

`MultiStateCallback` 的核心判断只有一条：

```text
(currentState & stateMask) == stateMask
```

`runOnceAtState(mask, callback)` 若条件已满足就立刻运行；否则把 callback 放进该 mask
对应的 `LinkedList`。同一 mask 按 FIFO 排空，不同 mask 由 `SparseArray` 的数值 key
顺序扫描。一次性是“同一个 MultiStateCallback 实例中的这个 callback 只从队列取出一次”，
并不意味着同名业务动作在对象重建后永不再发生。

`setState(flag)` 的准确顺序是：

1. 保存 `oldState`；
2. `mState |= flag`；
3. 扫描并排空所有已满足的一次性 callback；
4. 最后才通知持久 change listeners。

这个实现没有队列化重入。callback 内再次 `setState()` 会深度优先推进内层状态；
外层随后仍用自己捕获的旧 `oldState` 对最终 `mState` 发通知，可能让 listener 观察到
重复或不直观的 true。若 callback 在执行中 `clearState(mask)`，当前 while 循环也不会
重新检查 mask，剩余同 mask callbacks 仍会继续运行。

它也没有 `try/finally`：某个 callback 抛异常时，状态位已经写入，但后续 callbacks 与
listener 通知会中断。这说明它是一个轻量协调器，不是带回滚、隔离与持久化的事务框架。

`setStateOnUiThread()` 只在调用线程不是 main looper 时才 post；调用者要区分
“跨线程入队”与“主线程同步重入”这两种执行形态。

## 12. Handler 的 16 位门闩：用合取条件表达偏序

`BaseSwipeUpHandlerV2` 的 callback 使用另一套 16 位状态：

| 分组 | 状态 |
|---|---|
| Launcher UI | `LAUNCHER_PRESENT`、`LAUNCHER_STARTED`、`LAUNCHER_DRAWN` |
| controller | `APP_CONTROLLER_RECEIVED` |
| 终点缩放 | `SCALED_CONTROLLER_HOME`、`SCALED_CONTROLLER_RECENTS` |
| 手势 | `HANDLER_INVALIDATED`、`GESTURE_STARTED`、`GESTURE_CANCELLED`、`GESTURE_COMPLETED` |
| 截图 | `CAPTURE_SCREENSHOT`、`SCREENSHOT_CAPTURED`、`SCREENSHOT_VIEW_SHOWN` |
| 收尾动作 | `RESUME_LAST_TASK`、`START_NEW_TASK`、`CURRENT_TASK_FINISHED` |

关键 callback 可以画成合取门：

```text
PRESENT ∧ GESTURE_STARTED
  → 配置 Recents UI 与终点监听

DRAWN ∧ GESTURE_STARTED
  → 创建 Launcher animation controller

RESUME_LAST_TASK ∧ APP_CONTROLLER_RECEIVED
  → resumeLastTask()

START_NEW_TASK ∧ SCREENSHOT_CAPTURED
  → startNewTask()

PRESENT ∧ APP_CONTROLLER_RECEIVED ∧ DRAWN ∧ CAPTURE_SCREENSHOT
  → switchToScreenshot()

SCREENSHOT_CAPTURED ∧ GESTURE_COMPLETED ∧ SCALED_CONTROLLER_RECENTS
  → finishCurrentTransitionToRecents()

END_TARGET_ANIMATION_FINISHED ∧ RECENTS_SCROLLING_FINISHED
  → onSettledOnEndTarget()
```

最大的 RECENTS 收尾门还要求七项同时成立：

```text
LAUNCHER_PRESENT
∧ APP_CONTROLLER_RECEIVED
∧ LAUNCHER_DRAWN
∧ SCALED_CONTROLLER_RECENTS
∧ CURRENT_TASK_FINISHED
∧ GESTURE_COMPLETED
∧ GESTURE_STARTED
  → setupLauncherUiAfterSwipeUpToRecentsAnimation()
```

这些都是只增不减的正向门闩。`HANDLER_INVALIDATED` 只会触发清理 callback，其他门没有
自动附带“且未 invalidated”的负条件。因此取消后到达的晚事件仍可能补齐旧组合；源码主要靠
controller/null 检查、对象引用与具体 callback 内的 guard 限制影响，并非状态机天然拒绝所有晚回调。

## 13. SCREENSHOT_CAPTURED 和 APP_CONTROLLER_RECEIVED 都只是软件事实

`SCREENSHOT_CAPTURED` 的名字很强，但路径语义更接近“截图阶段可以放行”：

- live tile：有 controller 才尝试 screenshot/update thumbnail，随后无条件置位；
- 没有 targets：直接置位；
- controller 为空：没有 post-draw，也会走立即置位；
- HOME：可以调用 `screenshotTask()`，但不更新 TaskView，随后立即置位；
- 普通 task：若拿到 TaskView 且未 cancel，`ViewUtils.postDraw()` 延后两个
  `onPostDraw` 回调再置位；
- `screenshotTask()` 失败时 wrapper 可返回空 `ThumbnailData`，位仍可继续推进。

因此该位不证明像素有效，也不证明 SurfaceFlinger 已 present。两次 post-draw 只是 View
绘制时序栅栏。

`APP_CONTROLLER_RECEIVED` 同样是单调的“曾经收到过 controller”。cancel 后父类会清
`mRecentsAnimationController`，但不会清这个位。截图四门虽包含它，函数内部仍检查
controller 是否为空；反过来，`resumeLastTask()` 直接解引用 controller，依赖的是正常
生命周期顺序，而不是该状态位能证明引用此刻有效。

这给阅读状态名提供一条通用规则：

```text
状态名 = 某条客户端路径已经宣布可继续
状态名 ≠ 其英文名所暗示的所有外部效果均已完成
```

## 14. settled 后的四条收尾路径与 Binder 边界

终点动画 finished 与 RecentsView scrolling finished 同时成立后，
`onSettledOnEndTarget()` 才把目标翻译成 Handler 状态：

| 终点 | 新增 Handler 状态 | 后续动作 |
|---|---|---|
| `HOME` | `SCALED_CONTROLLER_HOME` + `CAPTURE_SCREENSHOT` | 截图放行后 finish-to-home |
| `RECENTS` | `SCALED_CONTROLLER_RECENTS` + `CAPTURE_SCREENSHOT` + `SCREENSHOT_VIEW_SHOWN` | 截图与手势完成后 finish-to-recents |
| `NEW_TASK` | `START_NEW_TASK` + `CAPTURE_SCREENSHOT` | 截图放行后启动所选 task |
| `LAST_TASK` | `RESUME_LAST_TASK` | controller 到达后 finish(false) |

其中 HOME 分支还会在 `onSettledOnEndTarget()` 内立即调用
`notifySwipeToHomeFinished()`；这个通知早于截图门和 controller finish。

四条路径的 controller 语义并不统一：

- `LAST_TASK`：`finish(false, null)` 后立即日志并 `reset()`，不等待回调；
- `RECENTS`：普通模式 `finish(true, callback)`；live tile 不 finish，只把
  `CURRENT_TASK_FINISHED` 置位，并在完成后保留 controller、设置 defer-cancel；
- `HOME`：Launcher handler 调 `finish(true, callback, sendUserLeaveHint=true)`；
  Fallback handler 调 `finish(false, callback, sendUserLeaveHint=true)`；
- `NEW_TASK`：live tile 先 `finish(true)` 再启动。普通模式若 task 是本次首次
  appeared，匹配的 `onTaskAppeared` 先让 V2 handler `reset()`，父层随后
  `finish(false)`；若该 task 已在本手势中 appeared，launch-success callback 会先走父层
  `onRestartPreviouslyAppearedTask()` 的 `finish(false)`，V2 override 返回后再 `reset()`。

`RecentsAnimationController.finishController()` 先在调用线程执行本地
on-finished listener，再把真正的 compat `finish()` 排到 UI helper executor。
compat 层捕获并吞掉 `RemoteException`；其后的 callback 只表示这次 Binder 调用已经返回
或失败被吞掉，不是 WindowManager 已完成提交、更不是新画面已经 present。

于是 `CURRENT_TASK_FINISHED` 也只是本地门闩：没有 targets、controller 为空或 live tile
分支都能立即置位；正常 callback 也至多说明 finish 尝试返回。GestureState 的
`FINISHED/ENDED`、targets release 和 Handler 清理可以早于真正的远端可见结果。

`NEW_TASK` 还有失败边界：若待启动 TaskView 为空或 Handler 已 cancel，底层启动函数可能
不调用结果 callback；这条路径不能靠“最终一定会 success=false”来保证复位。

## 15. 旋转重建、取消与失败注入：状态机会在哪些地方重新武装

Launcher Activity 因旋转而重建时，Handler 不复用原来的
`mStateCallback`。`onActivityInit()` 会：

```text
oldState = 原状态去掉 PRESENT / STARTED / DRAWN
initStateCallbacks() 创建新 MultiStateCallback 并重新注册所有 run-once
newCallback.setState(oldState) 回放非 UI 位
再绑定新 Activity，等待新的 present/start/draw
```

所以同一业务动作在新 callback 实例上可以重新武装。先前已经满足的非 UI 组合会在
`setState(oldState)` 时再次触发相应动作，而且回放发生在 `mActivity`、`mRecentsView`
换成新实例之前，callback 还可能读到旧引用；“run once”不能跨实例去重。另一个细节是
`onRecentsAnimationStart()` 动态注册的 enable-input-consumer callback 不在
`initStateCallbacks()` 的静态表中，重建不会自动重新注册它。

取消路径也可能主动结束 window animation：

```text
onRecentsAnimationCanceled()
  → 同步置 GESTURE_CANCELLED | HANDLER_INVALIDATED
  → invalidateHandler()
  → endRunningWindowAnim(false)
  → 再由 super 清 controller / targets
```

`endRunningWindowAnim(false)` 会走 animation end；success listener 会运行，但它看到
controller 仍非空，可能继续置终点完成位。Activity restart 也可能 cancel HOME spring，
而该 spring 的 cancel 如前所述仍以 end 结束。正确的测试不能只覆盖理想顺序，还应注入：

- controller 先到、Launcher 后 draw，反过来也测；
- UP 先于 targets，targets 先于 UP；
- animation end 与 cancel 交错；
- 旋转重建发生在截图门之前或之后；
- callback 重入、抛异常与 late task appeared。

判断是否安全的重点不是“所有事件按预想顺序来”，而是每个合取门在任意合法顺序下是否只产生可接受的副作用。

## 16. 九个源码练习与结论

下面的命令都只读取 Android 11 源码。每个练习先把路径放进变量，便于从仓库根目录执行。

### 练习 1：还原输入接收链

```bash
set -eu
TIS=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/TouchInteractionService.java
ICC=frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/InputChannelCompat.java
rg -n -F 'monitorGestureInput("swipe-up"' "$TIS"
rg -n -F 'this::onInputEvent' "$TIS"
rg -n -F 'mUncheckedConsumer.onMotionEvent(event)' "$TIS"
rg -n -F 'listener.onInputEvent(event)' "$ICC"
rg -n -F 'finishInputEvent(event, true' "$ICC"
```

检查 listener 与 `finishInputEvent` 的先后，并确认消费者分发发生在 service 内。

### 练习 2：比较两道 slop

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/inputconsumers/OtherActivityInputConsumer.java
rg -n -F 'Math.abs(displacement) > mTouchSlop' "$F"
rg -n -F 'mStartDisplacement = Math.min(displacement, -mTouchSlop)' "$F"
rg -n -F 'squaredHypot(displacementX, displacementY)' "$F"
rg -n -F 'QUICKSTEP_TOUCH_SLOP_RATIO_GESTURAL = 2' "$F"
rg -n -F 'QUICKSTEP_TOUCH_SLOP_RATIO_TWO_BUTTON = 9' "$F"
```

分别标注一维严格大于、二维大于等于，以及平方倍率对应的实际距离。

### 练习 3：对照 deferred 与 continuation

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/inputconsumers/OtherActivityInputConsumer.java
T=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/TouchInteractionService.java
rg -n -F 'mIsDeferredDownTarget = !continuingPreviousGesture' "$F"
rg -n -F 'startTouchTrackingForWindowAnimation(ev.getEventTime())' "$F"
rg -n -F 'continueRecentsAnimation(mGestureState)' "$F"
rg -n -F 'gestureState.getActivityInterface().deferStartingActivity' "$T"
```

注意 start 函数在 DOWN 与越过门槛的 MOVE 中都可能出现，必须结合分支上下文阅读。

### 练习 4：验证多指清理顺序

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/inputconsumers/OtherActivityInputConsumer.java
M=packages/apps/Launcher3/quickstep/src/com/android/quickstep/util/MotionPauseDetector.java
rg -n -F 'ev.getActionMasked() == ACTION_POINTER_UP' "$F"
rg -n -F 'mVelocityTracker.clear()' "$F"
rg -n -F 'mMotionPauseDetector.clear()' "$F"
rg -n -F 'setOnMotionPauseListener(null);' "$M"
rg -n -F 'ptrId == mActivePointerId' "$F"
```

确认 tracker/pause 清理位于 active-pointer 判断之前，再查 listener 是否被清空。

### 练习 5：追踪 UP 的速度坐标

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/inputconsumers/OtherActivityInputConsumer.java
rg -n -F 'computeCurrentVelocity(1000' "$F"
rg -n -F 'mNavBarPosition.isRightEdge()' "$F"
rg -n -F '? -velocityX' "$F"
rg -n -F 'new PointF(velocityX, velocityY)' "$F"
rg -n -F 'postDelayed(mCancelRecentsAnimationRunnable, 100)' "$F"
```

写出底、左、右三种导航栏下 `endVelocity` 的符号，并区分原始 `PointF`。

### 练习 6：手算四终点决策

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/BaseSwipeUpHandlerV2.java
rg -n -F 'private GestureEndTarget calculateEndTarget' "$F"
rg -n -F 'goingToNewTask = true' "$F"
rg -n -F 'mIsShelfPeeking' "$F"
rg -n -F 'willGoToNewTaskOnSwipeUp' "$F"
rg -n -F 'isOverviewDisabled()' "$F"
```

至少手算：全手势慢上拉、全手势斜向快速滑、非全手势过 0.7、cancel 四组输入。

### 练习 7：定位 duration 的轴向风险

```bash
set -eu
F=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/BaseSwipeUpHandlerV2.java
rg -n -F 'quickstep_fling_threshold_velocity' "$F"
rg -n -F 'quickstep_fling_min_velocity' "$F"
rg -n -F 'velocityPxPerMs.y' "$F"
rg -n -F 'mTransitionDragLength > 0' "$F"
rg -n -F 'createWindowAnimationToHome(start, homeAnimFactory)' "$F"
```

观察除法与长度 guard 的相对位置，并比较 target 决策和 duration 使用的速度轴。

### 练习 8：审计 GestureState 与 MultiStateCallback

```bash
set -eu
G=packages/apps/Launcher3/quickstep/src/com/android/quickstep/GestureState.java
M=packages/apps/Launcher3/quickstep/src/com/android/quickstep/MultiStateCallback.java
rg -n -F 'mStateCallback = other.mStateCallback' "$G"
rg -n -F 'STATE_RECENTS_ANIMATION_INITIALIZED' "$G"
rg -n -F '!mStateCallback.hasStates(STATE_RECENTS_ANIMATION_ENDED)' "$G"
rg -n -F 'final int oldState = mState' "$M"
rg -n -F 'callbacks.pollFirst().run()' "$M"
rg -n -F 'notifyStateChangeListeners(oldState)' "$M"
```

用源码顺序说明 callback 重入时为什么会先推进内层，再返回外层 listener 通知。

### 练习 9：核对截图与 finish 的含义

```bash
set -eu
H=packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/BaseSwipeUpHandlerV2.java
C=packages/apps/Launcher3/quickstep/src/com/android/quickstep/RecentsAnimationController.java
R=frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/RecentsAnimationControllerCompat.java
rg -n -F 'STATE_SCREENSHOT_CAPTURED' "$H"
rg -n -F 'ViewUtils.postDraw(taskView' "$H"
rg -n -F 'STATE_CURRENT_TASK_FINISHED' "$H"
rg -n -F 'mOnFinishedListener.accept(this)' "$C"
rg -n -F 'UI_HELPER_EXECUTOR.execute' "$C"
rg -n -F 'catch (RemoteException e)' "$R"
```

把每个命中点分类为本地位、View draw 栅栏、本地 listener、executor 排队或 Binder 异常边界。

本章最终可留下五条结论：

1. Quickstep 有“一维窗口移动”和“二维输入接管”两道不同门槛；
2. `ACTION_UP` 只结束采样，并不表示目标、远端 finish 或画面呈现已完成；
3. 四终点由导航模式、位移、速度、页面与 shelf 状态共同决定，且存在原始 Y 速度参与时长计算的轴向缺口；
4. `GestureState` 记录跨组件生命周期，Handler 的 16 位状态记录本地偏序，两者都不能替代当前引用与外部系统事实；
5. `MultiStateCallback` 是可同步重入、无回滚的一次性位掩码协调器，必须连同 callback 副作用与重建路径一起审计。

下一章转向 `RecentTasks`、`TaskKey` 与 `ThumbnailData`，继续追踪 Overview 中“任务元数据”和“任务像素”分别怎样加载、缓存与失效。
