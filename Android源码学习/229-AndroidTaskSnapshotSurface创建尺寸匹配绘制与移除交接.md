# 229 Android TaskSnapshotSurface创建、尺寸匹配、绘制与移除交接

本文基于 `android-11.0.0_r48`，接续第 228 章已经得到的 `TaskSnapshot`，追踪它如何被选为启动占位、如何驱动 WMS 创建 `TYPE_APPLICATION_STARTING` 窗口、怎样处理 buffer 与当前窗口尺寸不一致，以及真实首窗进入 WMS drawn/show 链后为何仍不能立刻宣称快照图层已经销毁。

这一章最重要的不变量是：**快照被选中、WindowState 已加入、buffer 已入队、WMS 接受 finishDrawing、starting window 被标记为 drawn、ActivityRecord 清空引用、WMS 收到 remove、图层真正消失，是彼此不同的完成点。**

版本锚点：

- `frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java`
- `frameworks/base/services/core/java/com/android/server/wm/StartingData.java`
- `frameworks/base/services/core/java/com/android/server/wm/SnapshotStartingData.java`
- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`
- `frameworks/base/services/core/java/com/android/server/wm/Session.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowState.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java`
- `frameworks/base/core/java/android/view/Surface.java`
- `frameworks/base/core/java/android/view/SurfaceControl.java`
- `frameworks/base/core/jni/android_view_Surface.cpp`
- `frameworks/native/libs/gui/Surface.cpp`

## 1. 先把启动占位的八个完成点拆开

设想应用 A 的进程仍在，但 Activity 需要重新创建。上一章保存的快照还在 running cache，系统希望在 A 的真实窗口绘制前先显示旧画面。表面上是“把截图贴上去”，实际至少经过八个阶段：

| 阶段 | r48 中可以证明什么 | 仍不能证明什么 |
| --- | --- | --- |
| 选择 snapshot | ActivityRecord 通过入口条件与旋转兼容检查 | 当前隐私策略、尺寸和内容仍然适配 |
| `addToDisplay(GONE)` 成功 | WMS 已创建并登记 starting `WindowState` | 可见 client buffer layer 已有内容 |
| `relayout(VISIBLE)` 返回 | WMS 已给出 frame 与 client `SurfaceControl` | snapshot 已成功入队 |
| attach/queue 返回 | buffer 已交给目标 BufferQueue | SurfaceFlinger 已 latch 或屏幕已 present |
| `finishDrawing()` 返回 | WindowState 仍有效且处于 `DRAW_PENDING` 时，WMS 可进入提交阶段 | 图层已经显示 |
| `HAS_DRAWN` | WMS placement 已允许 starting window 显示 | 物理屏幕一定出现过这一帧 |
| ActivityRecord 清引用 | 逻辑上不再把它当作活动 starting window | `WindowState` 与图层已删除 |
| `removeImmediately()` | WMS 才执行实际 WindowState/Surface 清理 | 先前一帧何时退出物理扫描并无 present 回执 |

整条主链可以压缩为：

```text
ActivityRecord 选中 running snapshot
  → SnapshotStartingData
  → AnimationThread 执行 AddStartingWindow
  → addToDisplay(GONE) 建 WindowState
  → relayout(VISIBLE) 建 client buffer layer
  → queue snapshot / 必要时补背景
  → finishDrawing → traversal → HAS_DRAWN
  → 真实首窗进入 drawn/show 链
  → 清 ActivityRecord 引用并投递 remove
  → 可选 450ms 延迟 → session.remove
  → 可选 preview-exit animation → removeImmediately
```

因此，“启动图已画”“已显示”和“已移除”都必须带上具体层级与状态，不能只凭一次方法返回下结论。

## 2. TaskSnapshotSurface不是WindowState，也不是应用View

`TaskSnapshotSurface` 是 system_server 内实现 `WindowManagerPolicy.StartingSurface` 的包装与控制对象。它不是应用进程中的 `View`，也不等同于 WMS 的 `WindowState`。至少要分清下面几类对象：

| 对象 | 职责 |
| --- | --- |
| `TaskSnapshot` | 携带旧画面的 `GraphicBuffer`、ColorSpace、taskSize、insets 等元数据 |
| `TaskSnapshotSurface` | 保存绘制、几何、计时和 remove 所需状态 |
| 内部 `Window extends BaseIWindow` | 作为 `IWindow` client token 交给 `IWindowSession` |
| starting `WindowState` | WMS 在 `addWindow()` 内创建的窗口记录，类型来自 LayoutParams |
| WindowState container layer | `WindowState` 挂入容器树时建立的层级容器 |
| client buffer layer | `relayout(VISIBLE)` 创建并通过 out `SurfaceControl` 返回的缓冲层 |
| mismatch child layer | 仅尺寸不匹配时，挂在 client buffer layer 下面的额外缓冲层 |

近似层级如下：

```text
ActivityRecord
└─ starting WindowState / container layer
   └─ client buffer layer  ← TaskSnapshotSurface.mSurfaceControl
      └─ mismatch child    ← 仅 mSizeMismatch 时存在
```

`new SurfaceControl()` 最初只是供 `relayout()` 填充的空 out-handle。它不是在 Java 构造语句处就创建了最终可绘制层。`mSurface.copyFrom(mSurfaceControl)` 又只是让 Java `Surface` 引用 client buffer layer 的 producer；`release()` 释放这一侧本地引用，不等同于删除 WMS 窗口或 SurfaceControl 层。

starting window 在同一 Activity token 内被明确排在其他应用窗口之上。它的任务是遮住尚未准备好的真实内容，真实窗口进入交接链后才请求移除。

## 3. 先过入口门，再从running cache取图

`ActivityRecord.addStartingWindow()` 一开始有三道短路：

- display 当前不适合显示，直接返回；
- `mStartingData` 已存在，不重复创建；
- 已有 main window 且其 animator 已 shown，不再增加占位。

通过后，它调用 `TaskSnapshotController.getSnapshot(taskId, userId, false, false)`。两个 `false` 分别意味着：**不允许磁盘恢复、请求高分辨率。** 所以这一条启动占位链只能消费当前 system_server 的 running cache；即使上一章的 proto 与 JPEG 完整存在，cache miss 时也不会为 starting window 现场读盘。

类型决策可以写成下面的顺序：

1. 新 Task、进程未运行，或 task switch 且 Activity 尚未 created：优先 splash。
2. 否则，只有 task switch 且允许 Task snapshot 时才检查 snapshot。
3. 旋转兼容则选择 snapshot。
4. 不兼容且不是 Home，回退 splash。
5. 不兼容的 Home 或其他情形，类型为 none。

“类型为 none”仍不必然代表最终没有 starting window。非 snapshot 分支会先检查主题约束，再尝试 `transferStartingWindow(transferFrom)`；已有 starting window 转移成功可以先于最后的 splash 类型判断返回 true。

转移本身又分两种状态：已经有 `startingWindow + startingSurface` 时，目标 Activity 接管 data、surface、displayed 状态和 WindowState，重写后者的 token/Activity 归属并搬动窗口层级；仍在 pending 阶段时，只偷走 `mStartingData` 并由目标重新排队创建。此后应由目标 Activity 的真实首窗完成移除交接。

已创建分支搬的是同一个 wrapper 引用，并不会用目标 Activity 重建 `TaskSnapshotSurface`。wrapper 内冻结的 `mTaskBounds`、`mActivityType`、`mOrientationOnCreation`、Painter 与 flags 仍来自原创建上下文；所以 450ms 的 Home 豁免、orientation 比较、crop 和栏外观都不会因 WindowState 归属转移而自动刷新。

反过来，选中 snapshot 后不会再检查 splash 主题的 translucent、floating、wallpaper 或 disable-preview 属性。snapshot 分支直接进入 `createSnapshot()`。

## 4. 兼容检查只回答旋转，Home还会先消费缓存

`isSnapshotCompatible()` 的名字容易让人高估它。r48 只做两件事：snapshot 非空，并且 snapshot rotation 等于目标 rotation。

目标 rotation 的来源是：

- display 能为不同方向 Activity 推导 rotation 时，使用 `rotationForActivityInDifferentOrientation(this)`；
- 否则使用当前 Task window configuration 的 rotation。

它没有检查 buffer 尺寸、taskSize、contentInsets、windowing mode、Activity component、捕获时间或当前安全策略。尺寸差异留给 `TaskSnapshotSurface`，其他差异多数根本没有消费端兼容门。

Home 还有一次性语义。只要类型已经判为 snapshot，代码就先从 running cache 删除 Home snapshot，再检查 transition 是否带 `TRANSIT_FLAG_KEYGUARD_GOING_AWAY_NO_ANIMATION`。flag 不满足时虽然返回 false、没有显示，这张缓存也已被消费。

创建阶段还会再比较一次：top fullscreen Activity 当前配置的 rotation 若不同于 snapshot rotation，就提前调用 `handleTopActivityLaunchingInDifferentOrientation()` 安装 fixed-rotation transform。这里比较的对象和兼容检查的目标来源并不完全相同，因此不是多余的同一判断。对应测试证明：snapshot 可以先按即将采用的 rotation 通过兼容检查，创建时仍因当前 top Activity 配置尚未切换而建立 fixed-rotation transform。

## 5. SnapshotStartingData把重活切到AnimationThread

`createSnapshot()` 只验证 snapshot 非空，创建 `SnapshotStartingData`，写入 `mStartingData`，然后安排 `mAddStartingWindow`。`SnapshotStartingData.createStartingSurface()` 再转到 Controller，最终调用 `TaskSnapshotSurface.create()`。

`scheduleAddStartingWindow()` 使用 WMS 的 `mAnimationHandler.postAtFrontOfQueue()`，并以 `hasCallbacks()` 去重。这个 Handler 绑定 `AnimationThread`，不是应用主线程，也不是 WMS 的 DisplayThread。把任务放在队首表达的是尽快处理，仍不构成同步执行或时限承诺。

调用方在 `addStartingWindow()` 返回 true 后还会把 `mStartingWindowState` 标成 `STARTING_WINDOW_SHOWN`。这个名字只表示请求已被接受并排队；此时可能仍只有 `mStartingData`，不能据此证明 `WindowState` 已加入，更不能证明 `startingDisplayed`。

`AddStartingWindow.run()` 的锁边界非常关键：

1. 在 WM global lock 内移除自己的 callback。
2. 若 `mStartingData` 已清空，认定请求取消并返回。
3. 把当前 `mStartingData` 复制到局部变量。
4. 释放 global lock，调用可能很重的 `createStartingSurface()`。
5. 捕获任意 `Exception`，把失败折叠成 `surface == null`。
6. 创建成功后重新进锁，发布 `startingSurface`，或在取消时清理并移除刚创建的 surface。

这符合 `StartingData` 和 `TaskSnapshotSurface` 的注释要求：调用创建逻辑时不能持有 WM global lock。代价是创建横跨一个可发生取消、替换与 Activity 状态变化的无锁窗口。

## 6. 两次null检查没有形成generation校验

第二次进锁只检查 `mStartingData == null`，没有检查它是否仍等于本次复制出的 `startingData`，也没有 generation token。于是代码存在如下代次交错窗口：

```text
run 复制 startingData=A，随后出锁创建 A
  → 另一条清理路径把 A 清空
  → 新请求安装 startingData=B
  → A 的 create 返回非空
  → 第二次检查看到“非 null”
  → 把 A 的 surface 发布到当前字段
```

这不是说每次重启都会发生，而是 r48 的两次检查无法从结构上排除它。若要证明某次现场没有跨代绑定，日志里必须同时记录 Activity、startingData 对象身份、surface 对象身份和两次锁区间。

失败语义也不对称：

- `createStartingSurface()` 返回 null 时，Runnable 只记日志，不清 `mStartingData`。
- 创建成功、返回后发现 `mStartingData == null` 时，会清字段并在锁外调用刚得到的 `surface.remove()`。
- `addToDisplay()` 已成功、但后续 relayout 或 draw 抛运行时异常时，外层只得到 null；WMS 可能已有 `startingWindow`，而 `startingSurface` 仍为空。
- 以后 `removeStartingWindow()` 遇到“startingWindow 非空、startingSurface 为空”会先清逻辑字段，再直接返回，无法通过该 wrapper 调用 `session.remove()`。

最后一种是部分创建失败窗口：真正的 `WindowState` 可能要依赖 token/window 的其他清理路径收尾。`TaskSnapshotSurface.create()` 自身没有用 `finally` 回滚已经成功的 add。

## 7. create锁内冻结的是多套对象，不是一份Activity快照

`TaskSnapshotSurface.create()` 先在锁外创建 LayoutParams、内部 Window、window session、空 SurfaceControl 与临时 out 参数，然后在 global lock 内收集创建所需状态。它要求：

- 目标 Activity 仍属于 Task；
- Task 存在 top fullscreen Activity；
- 目标 Activity 存在 main window；
- top fullscreen Activity 存在 top fullscreen opaque window。

收集字段时用了四个来源：

| 来源 | 贡献的数据 |
| --- | --- |
| 传入的目标 Activity | token、display、activity type、自己的 main window |
| 目标 Activity 的 main window | packageName、windowAnimations、dimAmount |
| Task 的 top fullscreen opaque window | flags、privateFlags、system UI、insets 配置、cutout、orientation、InsetsState |
| Task | taskId、当前 bounds、TaskDescription |
| 旧 snapshot | buffer format、rotation、旧 taskSize/contentInsets/ColorSpace |

这些选择器不保证指向同一个 Activity 或 Window。starting window 的包名和动画可能来自目标 main window，栏与 inset 策略却来自 Task 顶部的不透明窗口，像素又来自更早一次捕获。

旧 snapshot 并非所有元数据都会参与本类：buffer/format/ColorSpace、rotation、taskSize 与 contentInsets 会被读取；topActivity、snapshot orientation、windowing mode、旧 systemUiVisibility、real/translucent/low-resolution 标志不参与这里的匹配或绘制。栏外观采用的是创建时的当前窗口与 TaskDescription，而非旧 snapshot 保存的 system UI 字段。

大部分字段在锁内复制，但 add 时的 displayId 仍在锁外通过 `activity.getDisplayContent().getDisplayId()` 读取。因而不能笼统表述为“create 出锁后只使用冻结副本”。对象在创建间隙被重组时，早期校验也不是事务性承诺。

## 8. LayoutParams剔除显式输入flags与原窗口FLAG_SECURE

窗口类型固定为 `TYPE_APPLICATION_STARTING`，token 是 Activity token，宽高为 `MATCH_PARENT`，像素格式取旧 GraphicBuffer 的 format。最终 flags 先继承 top fullscreen opaque window，再排除一组可能影响副作用或布局的位，并强制加入 `FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCHABLE`。

排除集合包括 touch modal、IME focus、hardware accelerated、slippery、scaled、watch outside、split touch 等；源码中 `FLAG_NOT_FOCUSABLE` 写了两次，按位或结果不变。private flags 只继承 `PRIVATE_FLAG_FORCE_DRAW_BAR_BACKGROUNDS`。

`FLAG_SECURE` 也被明确排除。并且 `TaskSnapshotSurface.create()` 不会重新调用当前 Activity 的 `shouldUseAppThemeSnapshot()` 来验证传入 buffer；兼容检查仍只有 rotation。结合第 3、4 节，可得一个重要边界：

**生产新 snapshot 时的 secure → APP_THEME 决策，只保护那次新生产；策略变化前留在 running cache 的旧 REAL snapshot，消费时只经过 rotation 检查，而 inherited LayoutParams 不会继续携带原窗口的 `FLAG_SECURE`。**

即使只讨论新生产，也要收窄 APP_THEME 的覆盖结论：模式判断看 `task.getTopMostActivity()`，REAL 准备阶段可能另选 surface-ready Activity 收集元数据，最终 capture 的根又是整个 Task Surface。三者没有同一性保证；SurfaceFlinger 对实际 secure layer 的逐层黑化是独立兜底，不能把 topmost Activity 的模式决策扩写成“实际候选与整棵 Task 都通过了同一份安全检查”。

还要把 LayoutParams flag 与 native layer flag 分开：WMS 创建 client layer 时会调用 `WindowState.isSecureLocked()`，它除 attrs flag 外也考虑当前 DPM 状态；system_server Session 的内部窗口权限又会影响 DPM 判断。因而仅凭 `FLAG_SECURE` 被排除，不能断言最终 `SurfaceControl` 的 secure bit 必为 false。确定无疑的是：这里没有检查旧 buffer 的内容代次，也没有继承原应用窗口的显式 secure 位。

SurfaceFlinger 对捕获当时 secure layer 的黑化仍是生产端兜底，但它不会让已经保存的普通旧图在未来自动失效。若产品要求“当前切到 secure 后旧图绝不再显示或再次被捕获”，r48 这条消费链本身不足以证明该性质，需要额外失效、消费端内容复查或明确的 layer 安全策略。

输入方面还存在第二层事实：`addToDisplay()` 的 `outInputChannel` 传 null，WMS 不会为它打开客户端输入通道。即便如此，仍应把“无输入通道”和 LayoutParams 的 not-focusable/not-touchable 分开记录，它们是两层不同机制；`canBeImeTarget()` 还对 `TYPE_APPLICATION_STARTING` 有特例，不能仅凭这些 flag 扩大成“绝不参与 IME target 选择”。

## 9. add GONE与relayout VISIBLE是两个创建阶段

第一次 session 调用是 `addToDisplay(..., View.GONE, ...)`。成功后 WMS 创建 `WindowState`、attach、登记进 `mWindowMap`，把它写入 `ActivityRecord.startingWindow`，再加入 token 层级。负返回值会让 create 返回 null；`RemoteException` 则以“本地调用”为假设被吞掉并继续。

随后代码构造 `TaskSnapshotSurface`、把 wrapper 写回内部 Window，再调用 `relayout(..., View.VISIBLE, ...)`。这一阶段 WMS 才为可见窗口建立 client buffer layer，并把它复制到 out `SurfaceControl`。create 忽略 relayout 返回 flags；`RemoteException` 同样被吞掉。

系统栏 inset 的计算还要注意参数来源：

- frame 使用 relayout 返回的 `tmpFrame`；
- InsetsState 使用创建前从 top fullscreen opaque window 复制并合并 requested state 的 `insetsState`；
- relayout 返回的 `mTmpInsetsState` 没有用于 `getSystemBarInsets()`。

因此它是“当前 starting frame + 创建前复制的顶层窗口 inset 状态”的组合，不是简单采用 relayout 的全套输出。

add 成功也不是 draw 成功。若 relayout 没给出有效 out `SurfaceControl`，`drawSnapshot()` 开头的 `mSurface.copyFrom()` 就可能因空 native handle 抛 `NullPointerException`；若 copyFrom 已完成、parent `Surface` 仍无效并进入 mismatch 分支，才会命中其中的 `IllegalStateException`。attach/queue 或 Canvas 失败也会抛运行时异常，而 create 没有内部回滚。排障时应分别打点 add result、relayout result、out Surface validity、copyFrom、queue 和 finishDrawing。

## 10. sizeMismatch比较当前frame与buffer的精确像素

`setFrames()` 保存当前 window frame 与 system bar insets，然后按以下条件设置 `mSizeMismatch`：

```text
frame.width  != GraphicBuffer.width
或
frame.height != GraphicBuffer.height
```

这里没有比较 snapshot.taskSize，也没有容许 1 像素误差。只有进入尺寸不匹配分支后，才用宽高比差值 `> 0.01f` 再分两类。

| 情况 | buffer 放在哪里 | 几何策略 | 父 Surface 是否补背景 |
| --- | --- | --- | --- |
| 尺寸完全相等 | client buffer layer | 原 buffer 直接 attach/queue | 否 |
| 尺寸不同、宽高比差 `<= 0.01` | mismatch child | 全 buffer 以 `FILL` 映射到完整 frame | 否 |
| 尺寸不同、宽高比差 `> 0.01` | mismatch child | 先按旧 insets crop，再映射到计算 frame | 是 |

第二行不是 letterbox：`Matrix.ScaleToFit.FILL` 可以让两轴比例略有差异，只是源码认为 0.01 以内的形变肉眼不明显。第三行才会裁掉部分旧系统装饰，并由 parent Canvas 填充露出的区域。

计算没有验证 buffer 高度、frame 高度或 taskSize 分量是否为零，也不在 inset 后 clamp crop。正常生产对象应满足这些前提，但 Loader 或异常数据路径的健壮性不能由这里补足。

## 11. 尺寸相等路径转移buffer，不复制像素

相等路径先对 client `SurfaceControl` 调 `mSurface.copyFrom()`，再执行 `attachAndQueueBufferWithColorSpace(snapshotBuffer, colorSpace)`，最后 `mSurface.release()`。

API 注释把 attach/queue 定义为转移 buffer ownership，而不是重新绘制一份像素。Java 层把任意非零 native 错误转成 `RuntimeException`。native helper 依次执行：

1. 以 CPU API connect；
2. 暂存 producer 当前 dataspace；
3. 设置 snapshot 对应 dataspace；
4. attach GraphicBuffer；
5. 以 fence `-1` queueBuffer；
6. 恢复原 dataspace；
7. disconnect CPU API。

第 2 步的 `getBuffersDataSpace()` 只是取返回值，没有 status 失败分支；connect、两次 set dataspace、attach、queue 与 disconnect 都会返回错误。失败分支并不统一恢复已经改变的连接、dataspace 或 attachment 状态。尤其是 queue 已返回成功后，恢复 dataspace 或 disconnect 仍可能失败，Java 最终抛异常，但 buffer 已经入队。`-1` 也表示这里没有随 queue 传入新的 acquire fence；代码依赖传入的 snapshot buffer 已经可用。

ColorSpace 在此链中用来选择 dataspace 标签，不进行像素色彩转换。r48 JNI 只特殊识别 Display P3，其余 id 按 sRGB dataspace 处理。

成功 queue 仍只证明 producer 接受了 buffer。它不是 SurfaceFlinger latch、composition 或物理 present 的确认。`Surface.release()` 只释放 Java 本地 Surface 引用，也不是 remove window。

## 12. 尺寸不匹配会先提交child几何，再单独queue

不匹配路径先验证 parent `mSurface` 有效，然后新建 `SurfaceSession` 与 child `SurfaceControl`。child 的 buffer size、format 与旧 GraphicBuffer 完全一致，parent 指向 client buffer layer；字段 `mChildSurfaceControl` 保持强引用，避免局部对象结束后被 finalizer 销毁。

接着设置 child show、可选 crop、position 与 matrix，并调用 `mTransaction.apply()`。只有 Transaction 已提交后，代码才把 snapshot buffer attach/queue 到 child；aspect mismatch 时还会再经 parent Canvas `unlockCanvasAndPost()` 提交补边。几何、child buffer 与 parent buffer 没有共同的 Transaction 或 fence。若 attach 失败，child 的几何状态可能已经提交，而 `mHasDrawn`、`finishDrawing()` 和 `mSnapshot = null` 都尚未发生。

另一个很隐蔽的细节是 position 的最终写入者。aspect mismatch 分支先调用：

```text
setPosition(child, frame.left, frame.top)
```

随后 `Transaction.setMatrix(child, Matrix, float9)` 会提取 `MTRANS_X/Y` 并再次调用 `setPosition()`，覆盖前值。最终有效平移来自 `setRectToRect(sourceCrop, destinationFrame, FILL)` 生成的完整矩阵，不能简单记成 `frame.left/top`。

释放关系也必须按对象区分：

- size match：显式释放 parent Java `mSurface`；
- size mismatch 且比例接近：显式释放局部 child `surface`，parent `mSurface` 留到 wrapper 后续生命周期；
- size mismatch 且比例不同：先释放局部 child `surface`，Canvas 补边后再释放 parent `mSurface`。

最后一种不是对同一个 `mSurface` 连续释放两次。child `SurfaceControl` 和 `SurfaceSession` 没有在 wrapper remove 中显式 release/kill；WMS 最终移除 parent 会终止这棵可见层级，Java 本地引用则留给对象生命周期与回收机制。不能把这件事写成确定的立即 native 销毁。

## 13. crop、frame与补边混合了旧内容和当前窗口状态

aspect ratio mismatch 时，crop 从完整 buffer rect 开始。旧 snapshot 的 contentInsets 先分别乘以：

```text
scaleX = buffer.width  / snapshot.taskSize.x
scaleY = buffer.height / snapshot.taskSize.y
```

生产端的 contentInsets 是相对 Window frame 计算的，并且已经合入 letterboxInsets；它并不是已经规范化到 Task 坐标的一份纯 inset。消费端却按 `buffer/taskSize` 缩放这个复合字段，这是 r48 的坐标假设，不能改写成严格的坐标系契约。

随后代码裁 left、right、bottom。top 只有在 `taskBounds.top == 0 && frame.top == 0` 同时成立时完全保留，否则也按旧 top inset 裁掉。源码注释说这样保留屏幕顶部的状态栏，但实际保留的是整个复合 `contentInsets.top` 对应区域，其中也可能有 letterbox 等贡献。四边缩放后用 `(int)` 直接截断，没有 clamp；frame 反缩放时才用 `+0.5f` 近似四舍五入。

`calculateSnapshotFrame()` 再把 crop 宽高除以各自 scale，以 `+0.5f` 取近似四舍五入，并只按当前 `mSystemBarInsets.left` 把 frame 向右偏移，为左侧导航栏留位。最终矩阵负责把 snapshot 坐标的 crop 映射到窗口坐标的 frame。

这套计算同时使用：

- 旧 snapshot 的 buffer、taskSize、contentInsets；
- 创建时复制的当前 Task bounds；
- relayout 返回的当前 window frame；
- 当前顶层窗口策略推导出的 system bar insets。

所以它不是对一份同代几何的纯变换。旋转相同只能排除最明显的不兼容，无法保证自由窗 resize、栏位置或 inset 变化后完全无跳变。

补边只发生在 aspect ratio mismatch。parent Canvas 先用当前 TaskDescription background color 填右侧和底部，background 为 0 时回退白色，再由 `SystemBarBackgroundPainter` 画栏。右侧背景从何处开始却取决于原始 `TaskDescription.statusBarColor` 的 alpha：原始颜色完全不透明时从状态栏高度以下开始，否则从 y=0 开始。Painter 自己使用的是 `DecorView.calculateBarColor()` 计算后的最终颜色，这两个值不能混为一个。

因此可见画面可能是“旧 snapshot 像素 + 当前 TaskDescription 颜色 + 当前栏可见策略”的拼接，而不是历史画面的无损重放。

r48 的 `TaskSnapshotSurfaceTest` 主要覆盖 crop/frame helper、背景矩形与栏 Painter。它没有覆盖真实 add/relayout/draw、0.01 分支阈值、Transaction 与 queue 顺序、最终 matrix translation、release/ownership、attach 异常或 450ms 调度。因此这些行为不能只以现有单测通过作为端到端证明。

## 14. finishDrawing只是进入WMS显示状态机

`drawSnapshot()` 在具体绘制路径成功后，才在 global lock 内记录：

- `mShownTime = SystemClock.uptimeMillis()`；
- wrapper 私有的 `mHasDrawn = true`。

随后同步调用 `session.finishDrawing(mWindow, null)`，最后把 `mSnapshot` 置 null，避免 wrapper 长期持有对象。置 null 不会调用 `GraphicBuffer.destroy()`；BufferQueue 与 cache 中其他引用仍参与底层资源寿命。

finishDrawing 的 WMS 链为：

```text
Session.finishDrawing
  → WindowManagerService.finishDrawingWindow
  → WindowState.finishDrawing
  → WindowStateAnimator.finishDrawingLocked
  → DRAW_PENDING → COMMIT_DRAW_PENDING
  → requestTraversal
  → commitFinishDrawingLocked → READY_TO_SHOW
  → WindowState.performShowLocked
  → ActivityRecord.onStartingWindowDrawn
  → HAS_DRAWN + scheduleAnimation
```

`performShowLocked()` 在 starting window 分支调用 `onStartingWindowDrawn()`，把 Task 标记为曾经可见。`ActivityRecord.startingDisplayed` 是后续 `updateDrawnWindowStates()` 遍历发现 starting window 已 drawn 时才设为 true；它不是 wrapper 写 `mHasDrawn` 时同步设置。

`startingDisplayed` 也不是单调、每次成功显示都必经的一次性回执。新的 WMS transaction sequence 会先把它重置为 false，只有当前还需计算 all-drawn、窗口会影响该账本且 starting window 已 drawn 等条件满足时才重新置 true。第 1 节的阶段表描述正常成功链，不能把这一布尔账本当作硬件显示代际号。

这形成至少四个名称相似但不同的状态：wrapper 的 `mHasDrawn`、WindowStateAnimator 的 draw state、ActivityRecord 的 `startingDisplayed`、SurfaceFlinger/显示设备的实际 present。前三者都不能单独证明第四者。

内部 Window 收到 `resized(..., reportDraw=true)` 时，会经静态 main-looper Handler 再调用 `reportDrawn()`；它只检查 wrapper 的 `mHasDrawn`。如果 animator 已不在 `DRAW_PENDING`，重复 finishDrawing 通常不会再次推进 draw state。

这条 callback 不会重新调用 `setFrames()`，也不会更新 system bar insets 或重画 snapshot。它只在 merged `Configuration.orientation` 与创建时枚举不同的情况下向 main Handler 投递 remove，因此同为 portrait 的 0°→180°旋转、同方向 resize 或 inset 变化不会由此条件触发重算。initial relayout 得到的是一次性几何快照。

此外，`WindowState.requestDrawIfNeeded()` 看到 Activity 已有 active starting window 时，不把真实 main window加入同一等待集合。starting window 能解除某些“等待所有窗口绘制”的阻塞，不能据此推导真实内容也已经显示。

## 15. 真实首窗、450ms与最终销毁是三段交接

真实应用侧通常从 `ViewRootImpl.pendingDrawFinished() → reportDrawFinished() → IWindowSession.finishDrawing()` 把本次绘制完成报告给 WMS。WMS 再推进 animator draw state 与 placement；这不是 `Activity.onResume()`，也不只是某次 `View.onDraw()` 返回。

非 starting 的真实窗口进入 `performShowLocked()` 时，只要 draw state 是 `READY_TO_SHOW` 或 `HAS_DRAWN`，就调用 `ActivityRecord.onFirstWindowDrawn()`。该方法设置 `firstWindowDrawn`、清 dead windows；若仍有 starting window，还取消真实首窗自己的动画，然后调用 `removeStartingWindow()`。

`removeStartingWindow()` 先在 ActivityRecord 语义上处理状态：

- 尚未 add、只有 `mStartingData`：清空数据，取消待创建请求；
- Window 与 surface 都存在：先清 `mStartingData`、`startingSurface`、`startingWindow`、`startingDisplayed`，再把捕获的 surface remove 投递到 AnimationThread；
- Window 存在但 surface 为空：清字段后返回，暴露第 6 节的部分创建缺口；
- Window 存在而 `mStartingData` 已空：记录日志并返回。

因此 ActivityRecord 引用清空早于真正执行 `TaskSnapshotSurface.remove()`。

wrapper 的 450ms 条件只在“尺寸不匹配且不是 Home”时生效。计时起点 `mShownTime` 位于本地 queue/Canvas 成功之后、`finishDrawing()` 之前；若当前 uptime 尚未到 `mShownTime + 450`，则用绑定 WMS `mH` Looper 的实例 Handler 以绝对时间再次调用 remove。WMS 本身构造于 DisplayThread，所以这里的延迟任务运行在 DisplayThread，不是前面 add/remove wrapper 所用的 AnimationThread。Home 和 size-match 均不延迟。

这 450ms 不能叫“屏上最短展示时间”：

- 起点早于 WMS `HAS_DRAWN`，更早于未知的 latch/present；
- Handler 只能保证不早于目标 uptime 执行，不能保证精确执行；
- 重复 remove 可排入多个同一目标时刻的递归任务，类内没有 removed 标志或 callback 去重；
- 源码日志把已经经过的时间填进“defer in %dms”，不是剩余时间；
- 到点也只是调用 `session.remove()`。

更准确的说法是：“若 remove 过早到达，就把本 wrapper 的 remove 请求安排在 `mShownTime + 450ms`，而 Handler 实际执行还可以更晚。”至于设计动机是减少 mismatch 快速切换造成的视觉闪动，只能作为合理推断，r48 注释没有给出这一完整因果。

`session.remove()` 又只进入 WMS `removeWindow() → WindowState.removeIfPossible()`。若 starting window 可见且允许动画，WMS 可以应用 `TRANSIT_PREVIEW_DONE`，设置 `mAnimatingExit/mRemoveOnExit`，等 exit animation 结束后再隐藏、销毁 surface 并 `removeImmediately()`。最后一个 starting window、无有效动画等条件也可能直接删除。

`TaskSnapshotSurface` 自己没有“到某个最大时限就必删”的 watchdog。系统其他 transition timeout 可以推动更大的状态机继续运行，但不是这个 wrapper 的直接最终删除回执。

所以最终要记录三段时间：ActivityRecord 何时逻辑解绑、wrapper 何时发出 remove、WindowState 何时 `removeImmediately`。真实首窗触发交接依据的是 WMS draw/show 状态，不是物理 present fence；450ms 也既不是物理可见时长的下界，也不是最终销毁时长的上界。

现场排障可先按症状定位：

| 症状 | 优先检查 |
| --- | --- |
| 完全没有 snapshot starting window | 三道入口短路、running cache、type/rotation、Home flag |
| 有 startingWindow 但黑屏或未显示 | relayout out Surface、queue 异常、finishDrawing draw state |
| 尺寸跳变或栏错位 | frame/buffer 精确差、0.01 分支、旧 insets 与当前 frame |
| 首窗已来但占位久留 | Activity 字段、AnimationThread 投递、450 条件、preview exit animation |
| 安全策略开启后仍见旧内容 | running cache 旧 REAL 图、消费端仅 rotation 检查、旧显式 FLAG_SECURE 未继承、layer 安全位另查 |
| WindowState 残留但 wrapper 为空 | add 后 draw 异常、`startingSurface == null` 的清理缺口 |

## 16. 用九个只读练习复原完整交接链

以下命令固定读取 `android-11.0.0_r48`，不修改工作区。每个练习都先标出对象、线程和完成点，再判断方法返回能证明什么。

### 练习 1：重建snapshot、splash与transfer选择顺序

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '1732,1853p;1917,1951p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '3384,3477p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/SnapshotStartingData.java | nl -ba | sed -n '23,41p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '128,150p;282,325p;525,542p'
```

回答：哪些情况强制 splash；为什么 none 仍可能 transfer 成功；已创建与 pending transfer 各搬哪些状态；starting window 为什么不能从磁盘恢复；Home 在哪个检查前被移出 cache。

### 练习 2：验证创建跨代与部分失败窗口

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/StartingData.java | nl -ba | sed -n '21,41p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '1845,2000p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '154,280p'
```

画出两次 global-lock 区间，证明第二次只判 null、没有比较对象身份；再构造 add 成功而 draw 抛异常时 `startingWindow` 与 `startingSurface` 的组合。

### 练习 3：拆开wrapper、WindowState与三层SurfaceControl

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '90,180p;251,304p;366,388p;516,544p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '1535,1656p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '2270,2310p;2518,2550p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowContainer.java | nl -ba | sed -n '358,418p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowStateAnimator.java | nl -ba | sed -n '456,535p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowSurfaceController.java | nl -ba | sed -n '96,140p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '3284,3324p'
```

标出 IWindow client、starting WindowState、container、client buffer layer 和 mismatch child；再说明为何 starting window 位于 Activity 其他窗口之上。

### 练习 4：审计flags、输入与旧REAL图的隐私边界

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '97,118p;180,254p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '1574,1583p;1630,1655p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '1752,1769p;1917,1951p;4365,4390p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '247,364p;456,464p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowStateAnimator.java | nl -ba | sed -n '456,482p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowState.java | nl -ba | sed -n '1778,1784p;2395,2450p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/Session.java | nl -ba | sed -n '96,107p'
git -C frameworks/base show android-11.0.0_r48:services/devicepolicy/java/com/android/server/devicepolicy/DevicePolicyCacheImpl.java | nl -ba | sed -n '53,57p'
```

列出继承与排除的 flag，确认 outInputChannel 为 null 的效果，并回答旧 buffer 内容、显式 secure flag、DPM layer 检查各发生在哪一层；再解释 starting-window 的 IME-target 特例为何不同于拥有客户端输入通道。

### 练习 5：手算三个尺寸分支与最终matrix平移

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '328,469p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/SurfaceControl.java | nl -ba | sed -n '2570,2608p'
git -C frameworks/base show android-11.0.0_r48:services/tests/wmtests/src/com/android/server/wm/TaskSnapshotSurfaceTest.java | nl -ba | sed -n '160,215p'
```

任选一组非零 left/top crop，计算 sourceRect 到 destinationRect 的矩阵，验证最后的 position 来自 `MTRANS_X/Y`，而不是先前那次显式 setPosition。

### 练习 6：区分parent与child的queue、Canvas和release

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '337,423p;471,487p;550,640p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/Surface.java | nl -ba | sed -n '282,298p;536,568p'
git -C frameworks/base show android-11.0.0_r48:services/tests/wmtests/src/com/android/server/wm/TaskSnapshotSurfaceTest.java | nl -ba | sed -n '109,158p;217,293p'
```

为 size match、比例接近 mismatch、比例不同 mismatch 各列一行：buffer queue 到谁、Canvas 画到谁、显式 release 哪个 Java Surface。核对现有测试没有覆盖哪些真实绘制行为。

### 练习 7：追踪buffer ownership、dataspace与非原子提交

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/Surface.java | nl -ba | sed -n '735,755p'
git -C frameworks/base show android-11.0.0_r48:core/jni/android_view_Surface.cpp | nl -ba | sed -n '69,85p;392,400p'
git -C frameworks/native show android-11.0.0_r48:libs/gui/Surface.cpp | nl -ba | sed -n '2202,2231p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/SurfaceControl.java | nl -ba | sed -n '2314,2341p'
```

区分 Java Surface 引用、GraphicBuffer ownership、dataspace 标签和 Transaction；找出 transaction apply 与 attach/queue 的先后，以及每个 early return 可能留下的状态。

### 练习 8：从finishDrawing走到startingDisplayed

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '337,358p;489,543p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '2569,2589p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowStateAnimator.java | nl -ba | sed -n '325,378p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowState.java | nl -ba | sed -n '1915,1945p;4425,4459p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/DisplayContent.java | nl -ba | sed -n '885,910p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '5504,5568p'
```

把 wrapper `mHasDrawn`、`COMMIT_DRAW_PENDING`、`READY_TO_SHOW`、`HAS_DRAWN` 和 `startingDisplayed` 排成时序；指出哪一步仍没有 SurfaceFlinger present 回执。

### 练习 9：区分首窗交接、450ms门槛与最终删除

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '1953,2000p;5324,5340p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/ViewRootImpl.java | nl -ba | sed -n '3739,3773p;3854,3885p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotSurface.java | nl -ba | sed -n '282,326p;337,358p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/Session.java | nl -ba | sed -n '191,194p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '729,741p;1116,1124p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowManagerService.java | nl -ba | sed -n '1896,1907p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowState.java | nl -ba | sed -n '2161,2208p;2212,2356p;3238,3267p;4719,4782p'
```

分别标出逻辑解绑、AnimationThread 投递、DisplayThread 延迟、remove request、preview exit 与 `removeImmediately()`。最后解释为何 450ms 既不能保证屏上至少显示 450ms，也不能保证 450ms 时已经销毁。

整章可用一句话收束：**TaskSnapshotSurface 用旧 buffer 尽快构造一个 WMS starting window，但它不重新证明快照仍安全或几何仍新鲜；绘制成功只启动 WMS 显示状态机，真实首窗只启动移除状态机，物理可见与最终销毁都在更后的边界。**
