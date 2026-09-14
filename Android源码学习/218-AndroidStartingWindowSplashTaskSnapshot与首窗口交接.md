# 218 Android Starting Window、Splash、Task Snapshot 与首窗口交接

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只做静态源码核对：可以证明系统何时选择、创建、登记、转移与移除 starting window，以及 WMS 的 drawn 账怎样参与 app transition；不能据此声称某台设备已经把对应像素交给 HWC。Android 12 以后公开 SplashScreen API 的统一图标与退出动画模型不在本章范围内。

第 217 章已经把 `StartingWindowDelay` 与真实窗口的 `WindowsDrawnDelay` 分开。本章继续追问：**同一个启动请求为什么有时看到主题 Splash、有时看到旧 Task 画面、有时什么过渡窗都没有；系统又怎样在真实窗口尚未物理显示时就开始清理这层代理画面？**

## 1. 先固定 Splash、Snapshot、转移三条路线，再定义交接点

不先固定现场，cold launch、Task 回前台、Home 解锁、跳板 Activity 与透明主题会被拼成一条不存在的总时序。本章并排使用三条路线。

| 路线 | 固定前提 | 预期选择 |
|---|---|---|
| `L_splash` | 新 Task 的普通 cold launch；非 overlay、非 scene transition；display 可用；无已显示主窗；主题不透明、不浮动、未禁用 preview；无可转移前窗 | 新建传统主题 Splash |
| `L_snapshot` | 已有 Task 被切回前台；进程有 thread，目标 Activity 服务端状态落在 STARTED—STOPPED；新 Intent 允许复用；running cache 有 rotation 兼容快照；非 Home | 新建 TaskSnapshotSurface |
| `L_transfer` | 同一 Task 的跳板链；目标通过前置与主题门；前 Activity 已有完整 window/surface 对，或仍持有 StartingData | 转移现有窗口，或偷走模型后重排创建 |

以下点是本章自己的诊断坐标，不是 AOSP 内置 trace 名。

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `E_call` | 上层调用目标 `showStartingWindow()` | 一定会创建 preview |
| `G_pass` | overlay、scene-transition、display、重复模型与已显示主窗等前置门已通过 | 类型一定是 Splash |
| `K_read` | 已用 `restoreFromDisk=false` 查询 running snapshot cache | cache 一定命中 |
| `T_pick` | `getStartingWindowType()` 返回 NONE/SNAPSHOT/SPLASH | 真实窗口已经加入 WMS |
| `H_pass` | 非 Snapshot 路线已通过 theme/wallpaper 过滤 | transfer 一定成功 |
| `X_move` | 目标已接管旧 window/surface 对或 StartingData | 旧像素已 present |
| `M_data` | 目标 `mStartingData` 已写入模型 | WMS 已有 WindowState |
| `Q_post` | `mAddStartingWindow` 已投到 AnimationThread 队首 | Runnable 已开始 |
| `Q_take` | Runnable 第一段锁内已取得 StartingData 引用 | 创建不会再被取消 |
| `C_open` | 已在 WMS global lock 外调用 `createStartingSurface()` | surface handle 已返回 |
| `W_bind` | WMS `addWindow()` 已把 starting `WindowState`写给 ActivityRecord | 内容已 draw |
| `B_queue` | Splash 或 Snapshot 的内容 Buffer 已进入 producer queue | SurfaceFlinger 已 latch |
| `F_draw` | starting client 已调用 `finishDrawing()`，WMS draw state 可从 DRAW_PENDING 前进 | `isDrawnLw()`已经为 true |
| `C_return` | `createStartingSurface()` 返回非空 StartingSurface 句柄 | ActivityRecord 已登记句柄 |
| `S_store` | Add Runnable 第二段锁内已写 `startingSurface` | `startingDisplayed=true` |
| `W_ready` | placement 已把 draw state 推到 READY_TO_SHOW | show transaction 已成功提交 |
| `S_drawn` | `updateDrawnWindowStates()` 认出 starting window `isDrawnLw()`并置 `startingDisplayed` | 对应像素已 present |
| `S_has` | `performShowLocked()` 的成功分支已把 draw state 置 HAS_DRAWN | HWC present fence 已 signal |
| `T_go` | 该 Activity 的 preview/all-drawn 门及 transition 其他门均满足 | 真实 App 窗口已 drawn |
| `P_start` | 可归因的 starting-window present fence 已 signal | App 内容已可用 |
| `A_ready` | 非 starting 的真实窗口以 READY/HAS 进入 `performShowLocked()`并调用 `onFirstWindowDrawn()` | 真实 Buffer 已 present |
| `R_clear` | ActivityRecord 已同步清掉 StartingData/surface/window/displayed 四项账 | WMS WindowState 已移除 |
| `R_call` | 异步 `StartingSurface.remove()` 已调用 | exit animation 已结束 |
| `R_gone` | starting WindowState 与底层 Surface 已真正退出层级 | 真实内容已物理显示 |
| `P_real` | 可归因的真实窗口 present fence 已 signal | 用户已认为业务完成 |

两类新建路线在 `C_open` 内部的顺序不同：

```text
L_splash:
E_call < G_pass < K_read < T_pick < H_pass < M_data < Q_post < Q_take
Q_take < C_open < W_bind < C_return < S_store < B_queue < F_draw

L_snapshot:
E_call < G_pass < K_read < T_pick < M_data < Q_post < Q_take
Q_take < C_open < W_bind < B_queue < F_draw < C_return < S_store
```

Snapshot 在 `create()` 返回前同步画旧 Buffer并报告 drawn；Splash 的 `addView()` 只先建 system_server 的 ViewRoot，首个 traversal 稍后才画。因此不能背一条统一的“surface 句柄登记先于 finishDrawing”。固定正常路线随后才有 `F_draw < W_ready < S_drawn`；`P_start`属于另一条 SF/HWC 链。

对仍有代理账的路线，真实窗口交接固定到 `A_ready < R_clear`。只有清账时已有非空句柄，或Runnable已越过`Q_take`且最终返回非空句柄，随后才存在`R_clear < R_call`；在`Q_take`前取消则根本没有`R_call`。`R_gone`还可因退出动画或 Snapshot 的延迟移除更晚；WMS 不读取 `P_real`，所以源码没有给出这些点与真实 present fence 的通用全序。

## 2. Starting window 是系统代理窗；六本账分别回答六个问题

Android 11 定义三种选择结果：

```java
static final int STARTING_WINDOW_TYPE_NONE = 0;
static final int STARTING_WINDOW_TYPE_SNAPSHOT = 1;
static final int STARTING_WINDOW_TYPE_SPLASH_SCREEN = 2;
```

Splash 和 Snapshot 内容不同，但最终都以 `TYPE_APPLICATION_STARTING`、目标 Activity token 和 MATCH_PARENT 尺寸加入 WMS。它们由 system_server 侧代码创建，不是目标 Activity 的 Decor，也不会执行目标 App 的 `onCreate()`、点击逻辑或业务加载。

ActivityRecord 周围至少有六本不能互换的账：

| 账 | 写入含义 | 最常见误读 |
|---|---|---|
| `mStartingWindowState` | `showStartingWindow()`返回“请求或转移被接受”后写 SHOWN；`cancelInitializing()`可写 REMOVED | 枚举名 SHOWN 就等于像素已显示 |
| `mStartingData` | 尚可用于创建的 Splash/Snapshot 模型，也充当异步请求存在标记 | 它已经是 WindowState |
| `startingWindow` | WMS 已登记的 `WindowState` | Window client 已拿到移除句柄 |
| `startingSurface` | `StartingSurface.remove()`句柄 | 它就是 SurfaceControl 或已 present 的 Buffer |
| `startingDisplayed` | WMS 本轮统计时看到 starting `WindowState.isDrawnLw()` | SurfaceFlinger/HWC 已完成 |
| `startingMoved` | preview 所有权从该源 Activity 移走，可参与 transition 就绪判断 | 新目标已画好或旧画面已 present |

`mStartingWindowState`尤其容易骗人：`addStartingWindow()`只要创建了 StartingData并排队，或成功转移，就返回 true；实际异步创建可以尚未开始甚至稍后失败。真正的 WMS drawn 指示是另一字段 `startingDisplayed`，物理显示则还要继续追 SF/HWC。

WindowManagerPolicy 的 `StartingSurface`接口只承诺一个 `remove()`。Splash 实现包装 system_server 的 DecorView；Snapshot 实现持有 `IWindow`、`Surface`与 SurfaceControl。统一接口解决的是清理入口，不是证明两条创建链相同。

## 3. 上层先判断“值得尝试吗”，类型选择并不是第一道门

普通新 Activity 路线在 `ActivityStack.startActivityLocked()`准备 transition 后，才可能执行：

```java
ActivityRecord prev = r.getTask().topActivityWithStartingWindow();
r.showStartingWindow(prev, newTask,
        isTaskSwitch(r, focusedTopActivity));
```

这里的 `newTask` 是 `ActivityStarter.startActivityInner()` 查找可复用目标后得到的 `targetTask == null`，不能直接等同于 Intent 是否携带 `FLAG_ACTIVITY_NEW_TASK`。`taskSwitch` 则是目标 Activity 所属 Task 与当前 focused top Activity 所属 Task 不同；它描述 Task 身份变化，不描述进程冷热或窗口是否已经显示。

上层会排除 launch-task-behind、不可前移、scene transition 等不需要 preview 的情况；候选 `prev`来自同一 Task 中 `mStartingWindowState == SHOWN` 且可展示的 Activity，已经 `nowVisible`时又会被丢弃。另有 resume 失败、Task-to-front、用户切换等入口也会调用 `showStartingWindow()`，所以下面的布尔值不能反推唯一调用者。

`showStartingWindow()`自身还有两道早退：

- `mTaskOverlay`为 true：覆盖在 Task 上的特殊 Activity 不盖一张全屏代理图；
- pending animation 是 `ANIM_SCENE_TRANSITION`：共享元素要让真实 View 参与交接。

随后传给 `addStartingWindow()`的两个名字也要按实现解释：

| 参数 | r48 实际来源 | 它不是 |
|---|---|---|
| `processRunning` | `isProcessRunning()`找到 WPC 且 `hasThread()` | Linux pid 只要存在就算 true |
| `activityCreated` | 服务端 state ordinal 位于 STARTED 到 STOPPED，含两端 | 客户端对象一定仍健康、页面一定已画过 |

`addStartingWindow()`再依次拒绝 display 当前不能展示、目标已有 `mStartingData`、或主窗口已经 `getShown()`。只有这些门通过后才查询 snapshot cache并选择类型。防重因此有多层：ActivityRecord 的模型门防异步重复，WMS `addWindow()`还会拒绝同一 token 的第二个 starting WindowState。

这些返回值都是“无需尝试/已接受”的控制结果。`showStartingWindow()`返回后没有同步等待 AnimationThread、View traversal、Buffer queue或 present。

## 4. 类型决策有严格优先级；有快照也不等于优先 Snapshot

r48 选择器可以压成：

```java
if (newTask || !processRunning || (taskSwitch && !activityCreated)) {
    return SPLASH_SCREEN;
} else if (taskSwitch && allowTaskSnapshot) {
    if (isSnapshotCompatible(snapshot)) return SNAPSHOT;
    if (!isActivityTypeHome()) return SPLASH_SCREEN;
    return NONE;
}
return NONE;
```

按源码判断顺序展开：

| 现场 | 结果 | 关键原因 |
|---|---|---|
| 新 Task | Splash | 没有一张可代表此次新页面的既有 Task 语义 |
| 进程无 thread | Splash | cold 路线在第一层已结束判断，缓存命中也不改选 Snapshot |
| Task switch 且 Activity 未落在 created 区间 | Splash | 目标需要重建，旧图不被优先采用 |
| Task switch、允许快照、rotation 兼容 | Snapshot | 复用既有 Task 视觉状态 |
| 同条件但快照为空/rotation 不兼容，目标非 Home | Splash | 普通 Activity 回退主题窗 |
| 同条件但目标是 Home | None | Home 不走普通 Splash fallback |
| 非 Task switch 且未命中第一层 | None | 没有选择新代理窗的理由 |

注意 cache 查询发生在选择器之前，所以 `K_read`存在不表示结果会消费该对象。`newTask`或`!processRunning`一旦命中，snapshot参数只是被忽略。

Home 还有选择器之后的第二道特殊门。若先选到 Snapshot，代码立即清掉该 Task 的 running-cache entry，然后检查 `TRANSIT_FLAG_KEYGUARD_GOING_AWAY_NO_ANIMATION`；不是直接解锁的无动画路线就返回 false。也就是说，Home 快照可能“未被使用但已从 running cache 清除”，且这个早退不会再回头创建 Splash或尝试 transfer。

`isSnapshotCompatible()`只保证 snapshot rotation等于目标 Activity可能触发的 rotation，或 Task当前 rotation。它没有比较尺寸、Insets、资源限定符、业务数据、颜色模式或最终 present状态；后续 size-mismatch绘制正是为这些变化中的一部分兜底。

## 5. Snapshot 的“可用”横跨生产、缓存、Intent 与隐私四层

starting-window 选择读取：

```java
getSnapshot(taskId, userId,
        false /* restoreFromDisk */,
        false /* isLowResolution */);
```

因此本次快路径只读 running cache。磁盘上即使有持久化文件，也不会在持 WMS lock 的选择现场临时恢复；cache 被清后，本轮常见非 Home task-switch会因 snapshot为空而回退 Splash。

`allowTaskSnapshot()`还逐个检查 `newIntents`：null和 MAIN/LAUNCHER类 Intent可跳过；其他 Intent必须与最后一次或原 Intent `filterEquals()`，并且不能带 extras。`filterEquals()`本身不比较 extras，所以源码另设这一门，避免“带参数打开详情页”却先展示旧首页。

快照生产是另一条时间线：

```text
closing/hidden Task 或 screenTurningOff
→ 选择 snapshot mode
→ REAL: capture Task layers
   APP_THEME: 画不透明 TaskDescription 背景与system bars
→ running cache
→ 非临时Home快照再交给persister并通知Task
```

生产规则要注意四个边界：

1. Wear、TV、IoT上的 `shouldDisableSnapshots()`会让普通closing-app与screen-off入口跳过抓取；它不在`snapshotTasks()`本体内，不能外推成所有直接调用路径都被封死。
2. 普通生产只接受 standard/undefined 或 assistant Task；安全锁屏关屏时允许一条临时 Home REAL snapshot特例。
3. Task顶层 Activity设置的`mDisablePreviewScreenshots`，或该 Activity窗口子树中任一窗口`isSecureLocked()`，会把生产切到APP_THEME；后者既覆盖`FLAG_SECURE`，也覆盖DevicePolicy截图禁令，因此不会捕获这棵窗口子树的敏感像素。
4. APP_THEME画的是不透明 `TaskDescription` background color与系统栏，不是重新解析 Activity theme 的 `windowBackground`。

这里的 API 开关与主题属性 `windowDisablePreview`完全不同：前者控制快照是否可含真实像素，后者在消费时过滤传统 Splash/transfer 路线，不能合并成一个“禁用 preview”。

REAL snapshot用 `captureLayersExcluding()`捕获 Task layer并显式排除 IME。选择可见 Activity时要求其 surface showing、至少一个窗口 shown且 alpha大于0，但代码不等待“最新帧”的 reliable present fence，也没有显式从整棵 Task capture中排除 starting layer。它是已有图层状态的快照，可能陈旧，不能升级成业务最新性的证明。

夜间模式切换还有一个外部防线：UiModeManagerService在更新 uiMode configuration前调用 `clearSnapshotCache()`，注释就是让下一次使用 Splash而不是 screenshot。它只清 running cache；因为本章消费固定 `restoreFromDisk=false`，这已足以改变下一次选型。

## 6. Snapshot 早返；Theme 过滤、transfer 与新 Splash 的顺序不能交换

类型为 Snapshot 时，`addStartingWindow()`先处理 Home 特例，再 `createSnapshot(snapshot)`并直接返回。下面的 theme 检查、transfer和新建 Splash都不会执行。

非 Snapshot 路线才读取 Window styleable：

| Theme 条件 | r48 行为 | 视觉理由或边界 |
|---|---|---|
| `AttributeCache.Entry == null` | 返回 false | 不猜一个替代 App 主题 |
| `windowIsTranslucent=true` | 返回 false | 全屏不透明代理会遮错后方内容 |
| `windowIsFloating=true` | 返回 false | MATCH_PARENT代理与浮动窗口尺寸冲突 |
| `windowDisablePreview=true` | 返回 false | App theme明确拒绝传统 preview |
| `windowShowWallpaper=true`且当前无 wallpaper target | 给候选窗加`FLAG_SHOW_WALLPAPER` | 让壁纸与代理一起出现 |
| `windowShowWallpaper=true`且已有 wallpaper target | 返回 false | 避免不透明层破坏已有壁纸语义 |

然后顺序严格是：

```text
theme/wallpaper门
→ transferStartingWindow(transferFrom)
→ 若type不是SPLASH则返回false
→ 写SplashScreenStartingData并排队
```

由此得到三个不直观结论：

- NONE仍可能通过 transfer得到旧 preview，但前提是先通过 theme/wallpaper门；
- `windowDisablePreview`不仅阻止新 Splash，也会在 transfer调用之前返回；
- Snapshot已经在更早位置返回，所以该主题位挡不住选中的 Snapshot。

`theme == 0`时整段属性过滤被跳过，随后仍可 transfer；失败且类型为 Splash时才新建模型。不要把“选型返回 Splash”与“最终一定建出 Splash”合并，theme、wallpaper、transfer和异步创建仍能改变结果。

## 7. AddStartingWindow 是两段锁加一次锁外慢调用，不是事务提交

新建模型后，`scheduleAddStartingWindow()`先用 Runnable实例防止重复 callback，再把它 `postAtFrontOfQueue()`到 WMS `mAnimationHandler`。该 Handler绑定 system_server 的 `AnimationThread` Looper；队首只表示优先于当时普通消息，不表示在当前持锁调用返回前同步执行。

Runnable 的骨架是：

```text
第一段global lock
  removeCallbacks(this)
  若mStartingData==null则退出
  捕获startingData引用
解锁
  surface = startingData.createStartingSurface(activity)
  捕获Exception并保留surface=null
若surface非空，第二段global lock
  若当前mStartingData==null：清startingWindow，标abort
  否则：startingSurface = surface
解锁
  abort时surface.remove()
```

第一段与第二段之间的锁外调用会读资源、建 ViewRoot、进入 WMS、relayout甚至画 Buffer。`StartingData`类注释明确禁止调用者持 WMS lock，避免递归回调和长时间扩大临界区。

第二段只检查“当前 `mStartingData`是否为 null”，没有比较它与第一段捕获对象的身份，也没有 generation id。因此应称取消检查，而不是完整代际校验。r48依靠同一 Activity的模型防重与转移规则，让这个较弱检查足够覆盖常见竞态。

三种结束必须分账：

| 现场 | Runnable结果 | 残留状态 |
|---|---|---|
| 开始前已取消 | 第一锁区看到null直接退出 | 不创建真实窗；callback本次被消费 |
| 锁外create期间取消 | create可先把WindowState加入WMS；第二锁区看到null，随后锁外remove返回句柄 | ActivityRecord引用可先清，WMS实体异步退出 |
| create抛异常或返回null | 只记录日志，不进入第二锁区 | `mStartingData`不会由这里清除，可能继续阻挡add，直到别的remove/cleanup |

这也说明 `mStartingWindowState=SHOWN`、`mStartingData!=null`、`startingWindow!=null`与`startingSurface!=null`可以短暂组合成多种中间态，不能用单字段判断创建完成。

## 8. Splash 在 system_server 创建 PhoneWindow；App Context 只提供资源

`SplashScreenStartingData.createStartingSurface()`调用 WMS policy；r48默认进入 `PhoneWindowManager.addSplashScreen()`，仍在 system_server 的 AnimationThread执行。

Context选择分三层：

1. 根据 displayId取正确 Display Context；非默认 Display不存在就返回null，不偷放到默认屏。
2. theme或label需要时，用`createPackageContextAsUser(..., CONTEXT_RESTRICTED, user)`取得 App包资源并设主题。
3. 有 merged override configuration时创建 configuration context；只有该配置下`windowBackground`资源和Drawable都可用才采用，避免换到一个无背景Context。

随后创建的是系统代理 `PhoneWindow`：

```java
PhoneWindow win = new PhoneWindow(context);
win.setIsStartingWindow(true);
win.setType(TYPE_APPLICATION_STARTING);
win.setLayout(MATCH_PARENT, MATCH_PARENT);
```

policy强制 `FLAG_NOT_TOUCHABLE | FLAG_NOT_FOCUSABLE | FLAG_ALT_FOCUSABLE_IM`，设置目标 Activity token、package、window animation和标题，并加 `PRIVATE_FLAG_FAKE_HARDWARE_ACCELERATED`。这些字段让WMS正确分层与过渡；它们不证明目标App已创建自己的ViewRoot、Renderer或input connection。

Android 11已经会读取 `Window_windowSplashscreenContent`：有Drawable时包进一个View作为内容；没有时，PhoneWindow/Decor仍按 launch theme 的window背景形成传统占位。它只是r48的主题内容能力，不等于后续公开 SplashScreen API 的 icon animation或exit listener。

`wm.addView(decor, params)`会在当前 AnimationThread创建 system_server 的 ViewRoot，并同步调用WMS `addWindow()`；WMS可在此写下`startingWindow`。但 addView返回时首次 traversal通常只是已调度，未完成draw。只有Decor已有parent才返回`SplashScreenSurface`，Add Runnable随后才登记`startingSurface`。

固定成功 Splash因而是：

```text
system_server addView
→ WMS写startingWindow
→ addView返回并包装SplashScreenSurface
→ ActivityRecord写startingSurface
→ AnimationThread后续traversal/draw
→ IWindowSession.finishDrawing
```

BadToken、包资源异常或其他 RuntimeException会被policy捕获；未成功attach的View在finally里`removeViewImmediate()`，返回null。正常移除句柄则用View Context拿 WindowManager并`removeView()`，仍走普通ViewRoot/WMS移窗链。

## 9. Snapshot 在 create 返回前画 Buffer；尺寸不匹配又分两支

`TaskSnapshotSurface.create()`先短暂取得 WMS global lock，核对 Task、目标main window和Task顶部不透明window，并复制 window animation、dim、system UI、Insets、cutout与少量flags。它随后释放锁，再以本进程 `IWindowSession`完成：

```text
addToDisplay(View.GONE)
→ WMS写startingWindow
→ 创建TaskSnapshotSurface并setOuter
→ relayout(View.VISIBLE)
→ setFrames
→ drawSnapshot
→ finishDrawing
→ 返回StartingSurface句柄
```

LayoutParams仍是 `TYPE_APPLICATION_STARTING`与目标 token。继承flags时先排除focus、touch、secure、scaled、hardware-accelerated等副作用位，再强制NOT_FOCUSABLE/NOT_TOUCHABLE；private flags只继承绘制系统栏所需的一小部分。这是一张图的代理窗，不是旧App运行行为的克隆。

绘制按“窗口frame尺寸是否等于snapshot Buffer尺寸”分支：

| 分支 | Buffer路径 | 背景与system bars |
|---|---|---|
| size match | 父Surface直接`attachAndQueueBufferWithColorSpace()` | 不另画 |
| size mismatch、宽高比差不超过0.01 | 建精确Buffer尺寸child Surface，matrix FILL到新frame | child覆盖目标，不另锁Canvas补洞 |
| size mismatch且宽高比差超过0.01 | child Surface做crop、position、matrix；父Surface锁Canvas | 用TaskDescription背景填空洞并画system bars |

crop把snapshot content Insets按原Task尺寸缩放到Buffer坐标；只有Task与window都顶到屏幕顶部时保留top方向，否则也裁掉top装饰。这里解决的是旧图适配新frame，不证明业务内容、Insets或系统栏一定与即将到来的真实页面相同。

`drawSnapshot()`在queue后、`finishDrawing()`前用 uptime记录`mShownTime`并置`mHasDrawn=true`。这个名字不是物理显示时间。size-mismatch、非Home的`remove()`若离该点不足450 ms，会把真正的session remove推迟到阈值；所以450 ms只是“延迟调用remove”的门，不保证用户至少看到450 ms。

`resized()`若发现orientation不同，会经system_server主Looper尽快remove；若收到`reportDraw`，也会在`mHasDrawn`后补一次`finishDrawing`。这不是重新捕获或重画一张新快照。

## 10. WMS 把 starting 窗单独计账；drawn、show 与 present 仍是三层

`WMS.addWindow()`对 `TYPE_APPLICATION_STARTING`保留服务端硬门：token必须解析成仍在层级中的 ActivityRecord，同一Activity不能已有startingWindow。通过后先attach WindowState并写入window map，再执行：

```java
tokenActivity.startingWindow = win;
win.mToken.addWindow(win);
```

ActivityRecord的z-order比较还把starting window排在同token其他应用窗之上。它能遮住尚未接管的真实窗，但“在层级上方”仍不是“已有Buffer被显示”。

draw-state路线是：

```text
client finishDrawing
→ DRAW_PENDING → COMMIT_DRAW_PENDING
→ placement commitFinishDrawingLocked
→ READY_TO_SHOW
→ performShowLocked尝试
→ 成功时HAS_DRAWN并安排surface transaction
```

在placement同一窗口处理里，`commitFinishDrawingLocked()`先设READY并调用`performShowLocked()`；稍后`ActivityRecord.updateDrawnWindowStates()`才统计。starting window走专支：若`isDrawnLw()`为true，记StartingWindowDelay并设`startingDisplayed=true`，但不增加真实窗口的`mNumInterestingWindows/mNumDrawnWindows`。

`performShowLocked()`对starting window调用`onStartingWindowDrawn()`，后者只把Task标成曾可见；对非starting窗才调用`onFirstWindowDrawn()`。若`isReadyForDisplay()`失败，window可仍停在READY，而`isDrawnLw()`已经接受READY；因此`startingDisplayed`连“成功走完show分支”都不应无条件替代，更不能替代present fence。

AppTransitionController在非timeout路径对每个opening Activity检查：

```text
(allDrawn && !isRelaunching()) || startingDisplayed || startingMoved
```

这只是per-app门；rotation animation、remote animation specs、unknown-app visibility、wallpaper，以及opening/changing集合的其他对象也必须就绪。timeout会整体绕过这段reason-map填充。

非allDrawn时，reason按当前`mStartingData instanceof SplashScreenStartingData`记Splash，否则记Snapshot。它是模型分类而非像素证据：源Activity转走preview后`mStartingData`已清、`startingMoved=true`，即使被转走的是Splash，else分支也可能得到Snapshot reason。

## 11. Transfer 有“完整对迁移”和“模型迁移”，都不复制像素

ActivityStack寻找候选prev时用的是`mStartingWindowState==SHOWN`，并不要求`startingDisplayed`。目标进入`addStartingWindow()`后，还要先走theme/wallpaper门；Snapshot早返路线则根本不会尝试transfer。

第一种 transfer条件是源 Activity同时有 `startingWindow`和`startingSurface`。它只说明window/handle完整登记，`startingDisplayed`仍可为false。WMS global lock内会：

- 把 StartingData、surface handle、displayed值交给目标；
- 清源 Activity三项对象引用，并把源`startingMoved=true`；
- 改同一个 WindowState 的token与ActivityRecord，先从源容器removeChild，再add到目标；
- 传播部分allDrawn、firstWindowDrawn、visible/clientVisible状态；
- 必要时迁移animation与fixed-rotation transform；
- 置DisplayContent的`mSkipAppTransitionAnimation=true`，避免再叠一轮普通opening动画。

这条路没有复制Buffer、重建Decor或重画Snapshot，只是把同一窗口实体换账本和容器。代码也没有在这里比较两个Activity的package/theme是否相同；上层仅把候选限定在同一Task并排除已nowVisible项。

第二种分支在完整window/surface对不成立、但源`mStartingData!=null`时触发。目标偷走模型，源清data并置`startingMoved=true`，目标重新`scheduleAddStartingWindow()`。它不只覆盖“尚未开始create”：锁外create可能已经让WMS写了source.startingWindow，却还没返回surface给Runnable登记，此时仍会落入这一分支。

若源旧Runnable随后返回，它在第二锁区看到源data已为null，会remove自己刚建出的句柄；目标Runnable则按转来的模型另建。这里可能短暂经历两次WMS add/remove尝试，但模型所有权只有一个。

`startingMoved`写在源 Activity上，作用是让其transition不再等一扇已经转走的窗；它不说明目标窗已drawn。r48没有后续平台那种Splash内容View复制/回调握手，本章的transfer只指 WindowState/StartingSurface所有权迁移或 StartingData重新执行。

## 12. 真实窗口到 READY 就可触发清账；实际移除与物理交接更晚

当非starting窗口以 READY_TO_SHOW或HAS_DRAWN进入`WindowState.performShowLocked()`时，它先调用`ActivityRecord.onFirstWindowDrawn()`，然后才检查`isReadyForDisplay()`并在成功分支把draw state改成HAS_DRAWN。方法名与日志里的“shown”不能覆盖这段源码顺序。

`onFirstWindowDrawn()`同步执行：

```text
firstWindowDrawn = true
→ removeDeadWindows
→ 若仍有startingWindow，取消本次真实window自己的animation
→ removeStartingWindow
→ updateReportedVisibilityLocked
```

`removeStartingWindow()`又分两种：

| 当时账本 | 同步动作 | 异步动作 |
|---|---|---|
| `startingWindow==null && mStartingData!=null` | 只把data清null，取消请求 | `Q_take`前取消时Runnable醒来直接退出；`Q_take`后取消时creator仍可建出短命窗，若返回非空句柄则在第二锁区abort并remove |
| window/data均存在，但`startingSurface==null` | 清data/surface/window/displayed后直接返回 | 若这是仍在锁外执行的creator且它返回非空句柄，第二锁区会发现data为null并自行remove；create失败或返回null则没有这次句柄调用 |
| window/data/surface三者均存在 | 捕获surface handle，清data/surface/window/displayed | post到AnimationThread调用`surface.remove()` |

所以`R_clear`发生时，真正的WMS WindowState可能仍在层级。Splash句柄会`removeView()`；WMS对 starting window的退出可使用`TRANSIT_PREVIEW_DONE`动画。Snapshot句柄直接`mSession.remove(mWindow)`，但size-mismatch且非Home时可能再把实际remove转投WMS主Handler，等到`mShownTime+450ms`。

“用与add相同线程remove”的注释对ActivityRecord统一调度入口和Splash View hierarchy成立；不能扩写成所有底层remove最终都在AnimationThread，因为Snapshot的450 ms分支明确改用`mService.mH` Looper。

即使真实window已触发`onFirstWindowDrawn()`，该方法也没有读取它的present fence；starting surface移除同样不等待`P_real`。WMS可以借同批Surface transaction与z-order实现视觉交接，但静态Java状态只能证明draw-ready、清账和移除请求，不能证明扫描线上完全无黑帧或色差。

其他清理入口也要保留：`cancelInitializing()`把独立的`mStartingWindowState`置STARTING_WINDOW_REMOVED并移除孤儿preview，它没有把Activity生命周期state改成REMOVED；WindowStateAnimator销毁仍被ActivityRecord引用的starting surface时会清`startingDisplayed`；窗口/Activity退出路径还会做last-window cleanup。

## 13. 失败与竞态要按“模型、WindowState、句柄、像素”分层恢复

starting window是体验优化，许多失败选择继续等真实App而不让system_server崩溃；这不等于失败会把每本账自动回滚得像事务。

| 失败或竞态 | 可见源码结果 | 后续应观察 |
|---|---|---|
| display frozen/无目标display | 选择前或policy层返回false/null | 是否直接等待真实窗 |
| theme entry、包资源或Drawable失败 | Splash不建或create返回null | `mStartingData`是否仍残留、真实窗何时来 |
| token不是Activity、Activity已退出层级 | WMS返回BAD/EXITING类错误 | policy finally或Snapshot outer catch怎样清理 |
| 第二个starting WindowState | WMS返回`ADD_DUPLICATE_ADD` | ActivityRecord防重是否被异步窗口穿透 |
| Snapshot无Task/main/top opaque window | `TaskSnapshotSurface.create()`返回null | 模型仍可能存在，但无可移除句柄 |
| Snapshot add/relayout/draw异常 | 外层Add Runnable捕获Exception | 是否已有短命startingWindow、cleanup是否到达 |
| create过程中目标先出真实窗 | data被清；返回句柄后走abort remove | WindowState从加入到移除的短窗口 |
| orientation在Snapshot创建后改变 | `resized()`把remove投给主Looper | 真实新方向窗口与旧图的交接 |
| transfer发生在源create途中 | 目标重排；源返回后自删句柄 | 两个Runnable与唯一模型所有权 |

一个特别容易漏的状态是“create返回null”。Add Runnable不会因此清`mStartingData`，所以后续同Activity的`addStartingWindow()`仍可能被模型防重挡住；最终通常依靠真实窗口的remove、Activity清理或其他路径收口。诊断时只查WMS有没有WindowState，会错过这个模型残留。

反过来，`startingWindow!=null && startingSurface==null`也可能只是正常in-flight：WMS add发生在create内部，Runnable还没返回登记句柄。若creator最终返回非空句柄，`removeStartingWindow()`先清ActivityRecord账后，creator会在第二锁区看到data为null，再用刚得到的句柄做真正remove；若create失败而没有句柄，则不能套用这个收口顺序。

本章固定路线排除进程死亡、display移除、资源卸载和并发第二笔launch。现实故障若落在这些分支，应先标出四本对象账的现场值，再判断是等待、取消、清理还是重建，不能从一条白屏录像倒推唯一代码路径。

## 14. 白屏、旧图与闪烁要找第一处分叉，而不是只盯 `onCreate()`

| 现象 | 已知边界 | 第一轮证据重点 |
|---|---|---|
| 白色很快出现但停很久 | Splash可能已正常drawn，只是launch theme背景为白 | `T_pick/H_pass/S_drawn`与真实`A_ready`之间的Provider、Application、Activity、ViewRoot、RT |
| 点击Recents先见旧业务数据 | 很可能选中REAL snapshot；旧图本就来自较早capture | cache对象的id/rotation/isReal、newIntents门、真实首窗交接时刻 |
| 安全页只显示纯色+系统栏 | APP_THEME snapshot可能按设计隐藏真实像素 | `mDisablePreviewScreenshots`、secure window、TaskDescription颜色 |
| 没有任何preview | 可能是NONE，也可能前置/theme/policy/create失败 | overlay/scene/display/mainWin、类型、theme、WindowState与句柄四层 |
| 过渡图尺寸或bar闪变 | size-mismatch、crop、TaskDescription与真实Insets不一致 | frame/buffer/taskSize、aspectRatioMismatch、systemBar painter |
| 跳板间闪两次 | pending transfer、旧create abort、新create重排可能交错 | 源/目标data/window/surface/moved与两个Runnable |
| Snapshot似乎“不到450ms就没了” | 阈值从queue后的mShownTime算，不是present算 | sizeMismatch、Home特例、remove第一次调用与实际session remove |
| `startingDisplayed=true`但肉眼未确认 | 这里只到READY/HAS的WMS统计 | show transaction、SF latch/compose、可靠present fence |
| 真实首窗已ready仍短暂见preview | ActivityRecord先清账，真实remove可异步/动画/延迟 | `R_clear/R_call/R_gone`三点，不只看字段null |

把证据按层排列更稳：

```text
决策层：showStartingWindow参数、cache、type、theme、transfer
对象层：mStartingData / startingWindow / startingSurface
WMS层：DRAW_PENDING / COMMIT / READY / HAS、startingDisplayed、startingMoved
清理层：onFirstWindowDrawn / R_clear / StartingSurface.remove / R_gone
显示层：starting与real各自的queue、latch、compose、present fence
```

Splash颜色与真实首帧背景不一致，即使所有duration都很小也会闪；Snapshot像素再漂亮，也可能业务过期。性能优化和视觉连续性是两项验收：前者缩短完成点间隔，后者让两侧内容、system bars与Insets在交接处一致。

## 15. 九组 macOS 只读源码练习

以下命令从 AOSP 根目录执行，只读文件，不要求编译或设备。每个`rg -e`备选都应独立命中；命中行号只负责定位，运行时顺序仍要按调用关系手画。

### 练习 1：定位上层入口与五个前置门

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'void showStartingWindow' -e 'mTaskOverlay' -e 'ANIM_SCENE_TRANSITION' -e 'boolean addStartingWindow' -e '!okToDisplay()' -e 'mainWin.mWinAnimator.getShown()' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'topActivityWithStartingWindow' -e 'showStartingWindow(prev' frameworks/base/services/core/java/com/android/server/wm/ActivityStack.java frameworks/base/services/core/java/com/android/server/wm/Task.java
```

回答：哪些门在cache查询前？`mStartingWindowState=SHOWN`为何不证明AnimationThread已经运行？`activityCreated`的服务端state范围是什么？

### 练习 2：手推类型表与Home第二道门

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'false /* restoreFromDisk */' -e 'getStartingWindowType' -e 'newTask || !processRunning' -e 'taskSwitch && allowTaskSnapshot' -e 'isSnapshotCompatible' -e 'rotationForActivityInDifferentOrientation' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'removeSnapshotCache' -e 'TRANSIT_FLAG_KEYGUARD_GOING_AWAY_NO_ANIMATION' -e 'return createSnapshot(snapshot)' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

回答：有cache时cold launch为何仍选Splash？普通Task rotation不兼容怎样回退？Home候选拒用时cache是否仍保留？

### 练习 3：区分Intent许可、真实像素与APP_THEME生产

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotController.java
rg -n -F -e 'private boolean allowTaskSnapshot()' -e 'ActivityRecord.isMainIntent(intent)' -e 'filterEquals(intent)' -e 'intent.getExtras() != null' -e 'shouldUseAppThemeSnapshot' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'SNAPSHOT_MODE_APP_THEME' -e 'drawAppThemeSnapshot' -e 'captureLayersExcluding' -e 'mCache.putSnapshot' -e 'persistSnapshot' -e 'false /* isRealSnapshot */' frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotController.java
rg -n -F -e 'setDisablePreviewScreenshots' -e 'mDisablePreviewScreenshots' -e 'WindowState::isSecureLocked' frameworks/base/core/java/android/app/Activity.java frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

回答：`windowDisablePreview`与`setDisablePreviewScreenshots()`分别影响消费还是生产？APP_THEME画的是什么？REAL capture显式排除了哪一层？

### 练习 4：证明Theme、transfer与新Splash的真实顺序

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'Window_windowIsTranslucent' -e 'Window_windowIsFloating' -e 'Window_windowShowWallpaper' -e 'Window_windowDisablePreview' -e 'transferStartingWindow(transferFrom)' -e 'type != STARTING_WINDOW_TYPE_SPLASH_SCREEN' -e 'new SplashScreenStartingData' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'if (type == STARTING_WINDOW_TYPE_SNAPSHOT)' -e 'return createSnapshot(snapshot)' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

回答：Snapshot为何不受这些theme位过滤？NONE何时仍能得到旧preview？为什么`windowDisablePreview`也会挡住NONE transfer？

### 练习 5：画出两段锁、锁外create与三对象中间态

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/StartingData.java
rg -n -F -e 'DO NOT HOLD THE WINDOW MANAGER LOCK' -e 'abstract StartingSurface createStartingSurface' frameworks/base/services/core/java/com/android/server/wm/StartingData.java
rg -n -F -e 'postAtFrontOfQueue(mAddStartingWindow)' -e 'startingData = mStartingData;' -e 'surface = startingData.createStartingSurface(ActivityRecord.this);' -e 'if (mStartingData == null) {' -e 'startingSurface = surface;' -e 'surface.remove();' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'mStartingData' -e 'WindowState startingWindow' -e 'StartingSurface startingSurface' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

回答：第二锁区验证了null还是对象身份？create返回null会清data吗？取消发生在锁外create期间时谁负责移除已建实体？

### 练习 6：证明Splash属于system_server代理ViewRoot

```bash
test -f frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
rg -n -F -e 'StartingSurface addSplashScreen' -e 'getDisplayContext' -e 'createPackageContextAsUser' -e 'createConfigurationContext' -e 'new PhoneWindow(context)' -e 'setIsStartingWindow(true)' -e 'TYPE_APPLICATION_STARTING' -e 'FLAG_NOT_TOUCHABLE' -e 'PRIVATE_FLAG_FAKE_HARDWARE_ACCELERATED' -e 'Window_windowSplashscreenContent' -e 'wm.addView(view, params)' -e 'view.getParent() != null' frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
rg -n -F -e 'class SplashScreenSurface' -e 'wm.removeView(mView)' frameworks/base/services/core/java/com/android/server/policy/SplashScreenSurface.java
rg -n -F -e 'new Handler(AnimationThread.getHandler().getLooper())' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

回答：App包Context改变了资源来源还是执行进程？addView返回证明到哪一层？哪几个flag阻止代理窗接管输入？

### 练习 7：比较Snapshot的size match、同宽高比缩放与crop补洞

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java
rg -n -F -e 'session.addToDisplay' -e 'session.relayout' -e 'drawSnapshot()' -e 'attachAndQueueBufferWithColorSpace' -e 'drawSizeMismatchSnapshot' -e 'aspectRatioMismatch' -e 'calculateSnapshotCrop' -e 'setWindowCrop' -e 'setMatrix' -e 'drawBackgroundAndBars' -e 'mSession.finishDrawing' frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java
rg -n -F -e 'SIZE_MISMATCH_MINIMUM_TIME_MS' -e 'mShownTime = SystemClock.uptimeMillis()' -e 'mHandler.postAtTime(this::remove' -e 'mActivityType != ACTIVITY_TYPE_HOME' frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java
```

回答：哪条分支才锁Canvas补背景？450 ms从什么点起算？为什么它不是“至少present 450 ms”？

### 练习 8：区分WMS登记、drawn、show与transition reason

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'activity.startingWindow != null' -e 'tokenActivity.startingWindow = win' -e 'ADD_DUPLICATE_ADD' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
rg -n -F -e 'finishDrawingLocked' -e 'COMMIT_DRAW_PENDING' -e 'commitFinishDrawingLocked' -e 'READY_TO_SHOW' frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java
rg -n -F -e 'w != startingWindow' -e 'notifyStartingWindowDrawn' -e 'startingDisplayed = true' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'startingDisplayed' -e 'startingMoved' -e 'APP_TRANSITION_SPLASH_SCREEN' -e 'APP_TRANSITION_SNAPSHOT' frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

回答：starting窗为何不增加真实allDrawn计数？reason为何只是模型分类？`startingDisplayed`离可靠present还缺哪一层证据？

### 练习 9：闭合transfer、真实首窗与异步remove

```bash
test -f frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'tStartingWindow != null && fromActivity.startingSurface != null' -e 'mSkipAppTransitionAnimation = true' -e 'startingDisplayed = fromActivity.startingDisplayed' -e 'fromActivity.startingMoved = true' -e 'fromActivity.mStartingData != null' -e 'scheduleAddStartingWindow()' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'void onFirstWindowDrawn' -e 'win.cancelAnimation()' -e 'void removeStartingWindow' -e 'mWmService.mAnimationHandler.post' -e 'surface.remove()' -e 'mStartingWindowState = STARTING_WINDOW_REMOVED' frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F -e 'mHandler.postAtTime(this::remove' -e 'mSession.remove(mWindow)' frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java
rg -n -F -e 'TRANSIT_PREVIEW_DONE' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

回答：完整pair为何仍不证明drawn？pending/in-flight transfer怎样区分？`R_clear`后哪两类机制还能延后`R_gone`？

## 16. 把选择、创建与交接压成一张不会越界的检查表

先记住六条不变量：

1. cold、新Task，或Task-switch且服务端state不在STARTED—STOPPED闭区间时优先Splash；只有process有thread、state落在该闭区间、Task switch、Intent允许且running cache rotation兼容时才选Snapshot。
2. Snapshot在theme过滤前早返；NONE transfer与新Splash都必须先过theme/wallpaper门。
3. StartingData、startingWindow、startingSurface、startingDisplayed是模型、WMS实体、移除句柄与drawn统计四本账；`mStartingWindowState=SHOWN`只是接受结果。
4. Splash先返回句柄再异步draw，Snapshot先queue/finishDrawing再返回句柄；不能统一强排。
5. transfer迁移的是同一WindowState/句柄或未完成模型，`startingMoved`不等于drawn/present。
6. 真实窗口进入`onFirstWindowDrawn()`便可同步清代理账，但真正remove、exit animation、Snapshot 450 ms门和两类present仍各有自己的完成点。

遇到启动白屏或旧图，按下面顺序问：

```text
前置门是否允许preview？
→ cache/Intent/rotation选了哪一类？
→ theme是否挡住非Snapshot路线？
→ 是transfer还是新建StartingData？
→ AnimationThread是否建出WindowState与surface句柄？
→ starting窗是否finish/READY/startingDisplayed？
→ transition其他门是否满足？
→ 真实窗何时进入onFirstWindowDrawn？
→ ActivityRecord何时清账，底层何时真正remove？
→ starting与real各自何时取得可靠present证据？
```

这条检查链把“系统给了即时反馈”“WMS认定代理窗drawn”“真实窗口开始接管”和“像素真正显示”分成四个问题。只有先找到第一处分叉，theme调整、启动性能优化与窗口状态诊断才不会互相代替。

下一章继续追真实首窗的反向回报：Android ViewRootImpl `reportNextDraw`、`finishDrawing`与WMS draw-state状态机。
