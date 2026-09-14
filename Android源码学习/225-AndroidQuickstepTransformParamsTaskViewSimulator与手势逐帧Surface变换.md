# 225 Android Quickstep TransformParams、TaskViewSimulator与手势逐帧Surface变换

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章固定讨论旧版 Quickstep 的一条主线：前台 App 已交出 Recents animation leash，用户继续上滑，Launcher 怎样把手指位移变成同一帧中的 matrix、window crop、alpha 与 corner radius，并把这批属性交给 SurfaceFlinger。第 224 章已经解释 target 与输入怎样到达；第 226 章再处理抬手后的终态判定与异步状态机。

界面偶发“卡片边缘错一帧”“旋转时窗口跳一下”或“手势结束后 leash 迟迟不释放”时，只看一个 progress 值几乎找不到原因。这里实际有四套独立的账：手势标量、Recents 几何模拟、逐 target 属性分派，以及 RenderThread 消费/释放屏障。核心结论是：Quickstep 没有逐帧调用 WMS 的 `setProgress()`；它在 Launcher 中算几何，但 `Transaction.apply()` 只是在正常路径尝试跨进程交给 SurfaceFlinger，而且 Java 回调没有提供物理 present 证明。

## 1. 固定一次上滑场景，先给十五个观察点命名

固定场景 `G_app_to_overview`：手势导航；竖屏全屏 App；Recents animation 已 started；running target 的 `screenSpaceBounds` 与 `contentInsets` 有效；Launcher/Recents View 已 attach；没有 live-tile；用户从 App 连续上滑到 Overview，再选择回 Home。设备可能存在非零 `windowX/windowY`，这样旋转辅助方法的 r48 边界不会被默认零值掩盖。

| 点 | 源码侧含义 | 仍不能推出 |
|---|---|---|
| `D_oriented` | input consumer 按当前方向得到 displacement，并扣除起始 slop 位移 | shift 已更新 |
| `S_shift` | `updateDisplacement()` 反号、截下界并写 `mCurrentShift` | 值一定在 0—1 |
| `N_seek` | normal controller 已 seek，更新 fullscreen progress 与 Recents scale | 阻力段也已 seek |
| `R_seek` | 超过 1 时 resistance controller 已 seek | Task leash 已变 |
| `F_update` | `updateFinalShift()` 进入本次回调 | live-tile overlay 读到本次新几何 |
| `G_layout` | simulator 已重算 Task 尺寸、预览矩阵与逆矩阵 | scroll 插值已重算 |
| `G_scroll` | simulator 已重算 page center 与 curve scale | Surface 参数已构造 |
| `M_frame` | 本帧 matrix、crop、radius 已写入 simulator 临时对象 | 每个 target 都会使用它们 |
| `P_build` | `TransformParams` 已为全部 `unfilteredApps` 建好参数数组 | 数组已被 RT 消费 |
| `RT_take` | Launcher RT callback 已读取这批参数，或因 barrier 无效而丢弃它们 | 每个 target 都写入 transaction |
| `SF_enqueue` | 正常 IPC 中 SurfaceFlinger 已收到 transaction，并立即应用到 current state 或放入延迟队列 | 条件式 barrier 已满足 |
| `T_return` | Java `Transaction.apply()` 调用返回 | 延迟 transaction 已真正应用 |
| `SF_apply` | 本次 layer 状态已从立即/延迟路径应用 | 新 buffer 与属性已在同一合成周期汇合 |
| `L_latch` | 合成侧已 latch 对应 layer 状态 | HWC 已扫描到屏幕 |
| `P_present` | 对应 present fence 已 signal | 可由前面的 sequence 消息替代证明 |

正常 attached、barrier 有效、target Surface 有效且 native apply 无错误时，可以写出：

```text
D_oriented < S_shift < F_update < N_seek/R_seek
N_seek/R_seek < G_layout/G_scroll < M_frame < P_build < RT_take
RT_take < SF_enqueue ≤ T_return
SF_enqueue ≤ SF_apply < L_latch < P_present
```

其中 `SF_apply` 与 `T_return` 没有统一先后：不需等待时可在 Binder 返回前应用，进入延迟队列时则可在返回后才应用。这也不是所有分支的全序。`F_update` 先更新 live-tile overlay，后 seek window controller；若启用 live-tile，overlay 读到的是上一轮 simulator 几何。barrier 无效时仍会出现 `RT_take` 对应的 sequence 完成，却根本没有 `T_return`。单个 Surface 无效时又只跳过该 target。因此，诊断时首先要问“日志属于哪一个完成点”，而不是问“这一帧完成了吗”。

本章不重新解释 Recents controller 的 start/finish、input consumer 四道门或 WMS leash 回收；那些属于第 224 章。本章也不把 Java sequence 当作 SurfaceFlinger/HWC fence。

## 2. 四个 progress 与五个对象必须先拆开

逐帧数据面由五个核心对象协作：

| 对象 | 职责 | 不负责什么 |
|---|---|---|
| `RemoteAnimationTargets` | 保存全部 App targets、按 `targetMode` 过滤的 `apps`、wallpapers 与 release checks | 不计算几何 |
| `AnimatorControllerWithResistance` | 把 shift 分成 normal 0—1 与超出 1 的 resistance 两段 | 不直接写 Surface |
| `TaskViewSimulator` | 模拟 TaskView、RecentsView、方向与滚动几何 | 不决定每类 target 的 alpha/layer |
| `TransformParams` | 保存公共 alpha/progress/radius，并按 target 类型调用不同 proxy | 不自动从 progress 推导矩阵 |
| `SurfaceTransactionApplier` | 在 Launcher RT frame callback 中消费参数并建立条件式 frame barrier | 不提供 present 回执 |

名称相似的四个标量方向并不相同：

```text
mCurrentShift
  0 = App 端，1 = Overview 端；完全手势模式可继续大于 1

TaskViewSimulator.fullScreenProgress
  1 = 全屏端，0 = Overview 卡片端；normal controller 内与 shift 反向

TransformParams.mProgress
  仅是给 BuilderProxy 读取的公共字段，不会自行驱动 simulator

RectFSpringAnim.mCurrentScaleProgress
  回 Home 收束阶段的尺寸 spring 值；不是 X/Y 两个物理动画的统一时间轴
```

`TransformParams` 初值是 progress 0、target alpha 1、corner radius -1。它的两个 `FloatProperty` setter 也只改字段；注释所说的自动 current rect/corner 插值并不存在于这个类的实现中。普通上滑的 `BaseSwipeUpHandler.applyWindowTransform()` 只 seek simulator 属性，然后 `apply(mTransformParams)`，并没有把本次 `mCurrentShift` 写进 `TransformParams.mProgress`。所以 Assistant 特殊 alpha 若沿这条普通路径读取 progress，不能默认它就是当前手势 shift；回 Home 创建 spring 前才有显式 `setProgress(startProgress)`。

`RemoteAnimationTargets.apps` 只保留 `mode == targetMode` 的项；`unfilteredApps` 保留服务端交来的全部 App targets。几何生成要覆盖 opening Home 等非主目标，所以 `TransformParams.createSurfaceParams()` 遍历的是后者。

## 3. 手指像素怎样进入 normal 与 resistance 两段

MOVE 首先不是直接拿屏幕 Y。`OtherActivityInputConsumer` 用方向处理器取得 displacement，超过 slop 后扣掉 `mStartDisplacement`；ACTION_UP 前还会补一次 displacement 更新。`SwipeUpAnimationLogic.updateDisplacement()` 再做一次符号与范围转换：

```text
d = -displacement
若 transitionDragLength > 0 且 d > length × factor：shift = factor
否则：shift = max(d, 0) / length
length == 0 时后一式直接给 0
```

因此字段旁的“范围 [0,1]”注释只适用于 normal 段。完全手势模式的 `factor = dp.heightPx / transitionDragLength`，通常可大于 1；双按钮模式固定为 `1 + 0.25 = 1.25`。`initTransitionEndpoints()` 在计算 factor 时本身没有对零长度做除零保护，只有稍后的 displacement 转换会在 `length == 0` 时令 shift 为 0；不能把这一处局部兜底扩大成整条初始化链都安全。

`AnimatorControllerWithResistance.setProgress()` 的行为是：

1. normal fraction 被限制到 0—1，驱动 `PendingAnimation`；
2. `maxProgress <= 1` 时直接返回，不会主动把旧 resistance fraction 清零；
3. 超过 1 时，用 `getProgress(progress, 1, maxProgress)` 算 resistance fraction；这里没有再次限制范围，依赖上游封顶；
4. 两段分别缓存上次 fraction，相同值不重复 seek。

normal animation 把 `fullScreenProgress` 从 1 变到 0，把 `recentsViewScale` 从 fullscreen scale 变到 1。shift 超过 1 后，前者停在 0；resistance animation继续修改 Recents scale，并在完全手势模式下修改 secondary translation。双按钮的额外 25% 使用线性 scale，而且根本不添加 secondary translation；它不等于完全手势模式的非线性阻力上移。

`mCurrentShift.updateValue()` 只有数值变化时才运行 callback。`BaseSwipeUpHandlerV2.updateFinalShift()` 还会更新阈值触感、system UI flags 与 Launcher transition；其中 system UI 控制可能调用 Recents controller Binder。准确说法应是“窗口几何没有逐帧 WMS/ATMS 控制 Binder”，而不是“同一次 update 的所有支路都不经过 Binder”。

## 4. Simulator为何分 layout cache 与 scroll cache

`TaskViewSimulator` 并不创建一个真实 `TaskView`。它复用相同的 sizing、orientation、thumbnail 与 curve 算法，用少量字段模拟真实 View 树会得到的几何，再把结果写到 live Task leash。

它把成本分成两级：

| 修改 | `mLayoutValid` | `mScrollValid` | 后续重算 |
|---|---:|---:|---|
| `setDp()` | false | 间接失效 | Task size、pivot、thumbnail matrix、inverse、page size |
| `setLayoutRotation()` | false | 间接失效 | 同上 |
| `setRecentsRotation()` | false | 间接失效 | 同上 |
| `setPreviewBounds()` | false | 间接失效 | 同上 |
| `setScroll()` 且数值变化 | 保持 | false | screen center、linear interpolation、curve scale |
| 只改动画 float | 保持 | 保持 | 本帧 insets、矩阵、crop 与 radius |

`apply()` 的入口只检查 `mDp != null` 与 `mThumbnailPosition` 非空；失败时直接返回，不会构造或提交任何参数。layout 重算会先调用 `getFullScreenScale()`，让 size strategy 写 `mTaskRect` 与 pivot；然后更新 preview helper，求逆矩阵并初始化横竖方向对应的 half page/screen size。求逆的 boolean 返回值没有检查。若源矩阵奇异，目标 inverse 会被忽略而保留旧内容，后续 crop/radius 便没有可靠保证。

scroll 重算只从方向处理器选择 primary 轴：`screenCenter = taskStart + scroll + halfPageSize`，再由 `ScrollState.updateInterpolation()` 得到 curve 插值，最后调用 `TaskView.getCurveScaleForInterpolation()`。这解释了为什么横屏/旋转后不能把 `scrollX` 机械代入同一公式。

`getCurrentMatrix()` 返回内部可变 `mMatrix`；`getCurrentCropRect()` 返回反复复用的 `mTempRectF`，还依赖最近一次 `apply()` 留下的 task rect、inverse 与 insets。两者都不是跨帧稳定快照。回 Home 路径对 crop 显式 `new RectF(...)`，正是因为调用者需要保留它。

## 5. PreviewPositionHelper模拟的是 live leash，不是真实快照对象

`setPreview(runningTarget)` 保存两组信息：

- `screenSpaceBounds` 与 `contentInsets` 交给 `setPreviewBounds()`；
- bounds 的 left/top 另存为 `mRunningTargetWindowPosition`，供末端转回 leash/window 坐标。

若调用者只用 `setPreviewBounds()`，第二组位置不会同步更新。这个差别在 bounds 原点不是 `(0,0)` 时才会暴露。

Simulator 内部新建一份 `ThumbnailData`，只写 insets、强制 fullscreen windowing mode，并在 layout 重算时把 rotation 写成当前 display rotation。它没有读取真实 `TaskSnapshot` 的 rotation 或 scale；默认 scale 仍为 1。真实 `TaskThumbnailView` 会消费 snapshot 数据，而 live leash 模拟链只是借用相同 helper，不能把两条路径混成“按快照旋转”。

`PreviewPositionHelper` 的关键选择是：

1. `delta = (thumbnailRotation - currentRotation + 4) % 4`；Simulator 中就是 display rotation 减 Recents activity rotation；
2. 只有非 multi-window 且 windowing mode 为 fullscreen 才允许方向旋转；
3. delta 为 90°/270°才交换 width/height 来计算 width-fit scale，180°虽会旋转却不交换；
4. delta 为 0 时，`thumbnailData.insets` 每边被 `dp.getInsets()` 限制；只要有 rotation delta，就直接用传入 insets；
5. 无旋转先平移负 insets；有旋转先做 90°整数倍旋转，再做尺寸与旋后 inset 补偿；最后统一乘 `thumbnailScale`。

真实 thumbnail 绘制还会读取 helper 的 `mClipBottom`，对缩放后高度不足的 bitmap 做额外 clip。Simulator 只读取 matrix 与 fullscreen insets，没有消费这个 bottom clip 字段。因此“复用同一个 helper”不表示真实缩略图与 live leash 的所有裁剪分支完全相同。

## 6. FullscreenDrawParams把全屏边缘还回窗口

normal controller 的两端容易写反：

| `fullScreenProgress` | 语义 | drawn insets | `mScale` | 基础 radius |
|---:|---|---|---|---|
| 0 | Overview 卡片 | 0 | 1 | Task card corner / parent scale |
| 1 | 全屏 App | helper 的 clipped insets | `width / (width + left + right)` | window corner / parent scale |

`TaskViewSimulator.apply()` 只在送进 `FullscreenDrawParams.setProgress()` 前把 fullscreen progress 限制到 0—1；`recentsViewScale.value`、secondary translation 等没有在这里统一限制。

fullscreen insets 按 progress 线性插值。左右边缘重新画回后，总宽度变大，所以还要用 `mScale` 缩回 task width；只有 `previewWidth > 0` 才更新该字段，否则它会保留之前的值。圆角从 `TaskCornerRadius` 插值到 fullscreen radius，再除以 `parentScale`，而 simulator 传入的 parent scale 就是 `recentsViewScale.value`。若它为 0，代码没有防止除零。

普通 fullscreen radius 来自设备资源与 rounded-corner 支持状态，不保证必为 0；multi-window 分支才明确把 fullscreen corner 设为 0。Overview 端的 Task card radius 在不支持 live rounded corners 的设备上仍可能取一个小的资源值。因此不能用“全屏必尖角、卡片必圆角”替代源码条件。

这里的 `mCurrentDrawnCornerRadius` 也不是最终屏幕 radius。它只补偿了 parent/Recents scale，此时尚未补偿 thumbnail scale；稍后 `getCurrentCornerRadius()` 会用 inverse preview 抵消这部分，而额外 `mScale`、curve scale 等仍会改变最终可见半径。

## 7. 一帧 matrix 按九层顺序合成

layout 与 scroll cache 就绪后，Simulator 每帧从 preview matrix 重新开始，不在上一帧矩阵上累乘：

| 顺序 | 源码操作 | 坐标意义 |
|---:|---|---|
| 1 | `set(positionHelper.matrix)` | 源窗口/截图内容适配到 Task 预览 |
| 2 | `postTranslate(drawnInsets.left, top)` | 把逐步还回的 inset 放进绘制区域 |
| 3 | `postScale(fullscreenDrawScale)` | 左右 inset 增宽后的回缩 |
| 4 | `postTranslate(taskRect.left, top)` | TaskView 在 Recents 中的位置 |
| 5 | `postScale(curveScale, task center)` | carousel 曲线缩放 |
| 6 | primary `MATRIX_POST_TRANSLATE(scroll)` | Recents page 滚动 |
| 7 | `postScale(recentsViewScale, pivot)` | Recents 整体从全屏尺度回到 1 |
| 8 | secondary post-translation | 阻力段的次轴位移 |
| 9 | window/home rotation helper | windowX/Y、display rotation、running target 原点补偿 |

Android `Matrix.postX()` 在这份源码的约定中形成 `M' = X × M`，所以一个点会按表中调用顺序经历这些变换。不要用其他图形库“post 等于右乘”的记忆反读。

末端 helper 又包含三步：

```text
postTranslate(windowX, windowY)
postDisplayRotation(recentsActivityRotation → displayRotation)
postTranslate(-runningTargetWindowPosition.x, -y)
```

`PreviewPositionHelper` 的 rotation 与这里的 display rotation 不能互相删掉。前者把源内容适配到模拟 Task 卡片，后者把 Launcher 布局坐标转回远端窗口/leash 所在坐标。在这条 Simulator 路径中二者取得的数值 delta 相同，但旋转方向以及用于补偿的坐标空间、尺寸不同。

标准 `onBuildTargetParams()` 不读取 `app` 的独立 bounds，而把同一个 `mMatrix`、`mTmpCropRect`、radius 写给每个匹配 targetMode 的普通 target。也就是说，running target 的 preview 几何是这一帧的统一模板；若 target 集合里多个 closing App 具有不同源几何，这个实现并未逐项重算。

## 8. Crop与圆角为什么只逆 preview matrix

window crop 属于被修改 leash 的局部/source 坐标，不属于最终屏幕坐标。Simulator 先在 Task 空间造矩形：

```text
[-drawnLeft, -drawnTop,
 taskWidth + drawnRight, taskHeight + drawnBottom]
```

然后只通过 `mInversePositionMatrix` 映回源窗口空间。`mapRect()` 得到旋转后四角的轴对齐包围盒，`roundOut()` 再向外取整为整数 `Rect`，避免向内取整无意切掉边缘像素。它刻意没有求整个最终 matrix 的逆；TaskView 位置、scroll、Recents scale 与 window rotation 是“把裁剪后的局部内容摆到哪里”，不应反算进 local crop。

圆角同理。`getCurrentCornerRadius()` 把 `(visibleRadius, 0)` 作为向量，通过 inverse preview matrix；`mapVectors()` 忽略平移，再取两个分量绝对值的最大值。当前 helper 只有统一缩放、90°整数倍旋转和平移，逆映射后理论上仍只有一个有效轴，所以 max 等于向量长度（浮点误差除外）。若以后加入任意角、skew 或非等比 scale，这个“免平方根”写法才会退化为近似，不能脱离当前矩阵集合泛化。

还要分清三个 radius：

- `FullscreenDrawParams` 的值是模拟绘制空间中的中间量；
- `TaskViewSimulator.getCurrentCornerRadius()` 是供 leash transaction 使用的 source-local 值；
- 回 Home factory 收到的 `mMatrix.mapRadius(cornerRadius)` 是两个映射后坐标轴向量长度的几何平均，只是一枚标量近似。

因此看到某一层的 radius 数值相等，仍不能证明两个 Surface 树的屏幕轮廓逐像素一致。

## 9. TransformParams按mode与activityType分派每个target

`createSurfaceParams(proxy)` 为 `unfilteredApps.length` 中每一项建一个 `SurfaceParams.Builder`，决策树是：

```text
app.mode == targetMode?
├─ 否：base proxy
└─ 是
   ├─ activityType == HOME：home proxy
   └─ 其他
      ├─ ASSISTANT && isNotInRecents：先写特殊 fade alpha
      └─ 普通：先写 targetAlpha
      然后两者都继续调用主 proxy
```

这里有四个常见误读：

1. home proxy 也受 mode 相等的前提约束。Recents targets 把 `targetMode` 固定为 CLOSING，常见 opening Home 实际走 base proxy；
2. Assistant fade 也只发生在 mode 相等时；
3. 特殊/普通 alpha 都写在主 proxy 之前，若自定义 proxy 再 `withAlpha()`，后写值会覆盖前值；
4. 默认 home/base proxy 只是写 alpha 1；它们不会自动继承 simulator matrix。

标准 TaskViewSimulator proxy 只写 matrix、crop、corner，所以先前 alpha 得以保留。回 Home `SpringAnimationRunner` 也写这三项，target alpha 则随其 scale spring progress 在 0—0.85 区间按 `ACCEL_1_5` 降到 0。

`RemoteAnimationTargets` 构造器允许 apps 参数为 null，并会得到空的过滤数组；但它仍原样把 null 保存为 `unfilteredApps`。`createSurfaceParams()`、`isAnimatingHome()` 与 `release()` 随后都直接遍历它。因此“构造成功”不代表这个对象的所有方法支持 null targets；正常 Recents 调用依赖非空数组约定。

## 10. SurfaceParams是值快照约定，不是类型级不可变

Builder 的每个 `withXxx()` 都设置一枚 flag。`applyTo()` 只写 flag 指定的字段；没设 alpha 不是“恢复为 1”，没设 matrix 也不是“把 Surface 重置为 identity”。它的调用顺序是 matrix、crop、alpha、absolute layer、corner、blur、visibility、relative layer；这是一次 transaction 的状态集合，不是八次屏幕绘制。

构造时会：

- 深拷贝传入的 `Matrix`；传入 null 时 Android `new Matrix(null)` 建 identity；
- 若 crop 非空，深拷贝 `Rect`；
- 复制数值与 boolean；
- 共享 `SurfaceControl surface` 与 `relativeTo` 引用。

但 `matrix` 和 `windowCrop` 虽是 `public final` 字段，指向的对象本身仍可修改；Surface 句柄也可能之后失效或被 release。`scheduleApply()` 又直接捕获传入的 varargs 数组，不复制数组。安全性来自“构建时复制输入值，并约定交给 applier 后不再修改数组/参数对象”，而不是 Java 类型强制的不可变性。

Builder 允许同时写 absolute layer 与 relative layer。`applyTo()` 最后调用 relative layer；native `setRelativeLayer()` 会清掉 absolute layer changed bit，最终采用 relative 规则，而不是两套 Z 规则同时成立。

每帧的主要分配也在这里出现：长度为 target 数的数组、每 target 一个 Builder/SurfaceParams、每份 Matrix/Rect 拷贝，以及随后 Transaction。Simulator 的临时 Matrix/Rect 可复用，不能据此推断整条每帧路径零分配。卡顿分析应同时记录 target 数、GC、UI callback、RT callback 与 SF transaction，而不是只盯矩阵乘法。

## 11. RT帧屏障究竟同步到哪里

没有 `SurfaceTransactionApplier` 时，`TransformParams` 在当前调用线程新建 `TransactionCompat`，按数组正序 `applyTo()`，然后立即 `apply()`。这仍是一批 transaction，但没有 Launcher View 的 RT frame number 约束；它也不会像专用 applier 那样先跳过无效 target Surface，已 release 的句柄可让 setter 在当前线程直接抛错。

有 applier 时，`scheduleApply()`：

1. 从保存的 `ViewRootImplCompat` 取 View；为 null 则直接返回；
2. sequence 自增并把 release gate 置 false；
3. 注册 RT frame callback，随后 `view.invalidate()` 请求一帧；
4. callback 若 barrier 为 null/invalid，只发 sequence 消息并丢弃整批参数；
5. barrier 有效时新建 transaction，反向遍历数组，跳过无效 target；
6. 对有效 target 建 `deferTransactionUntil(target, barrier, frame)`，再 `applyTo()`；
7. 调用 `t.apply()`，然后向构造时 Looper 的 Handler 发 sequence 消息。

反向遍历只决定 Java 往 transaction 写入多个 target 的顺序；除同一 Surface 的覆盖规则外，不能理解成 SurfaceFlinger 会依次显示数组尾到头。

`deferTransactionUntil` 是条件式“不早于”关系：目标 layer 已 detached 时合成侧可以不设置 deferral，barrier layer 被移除时 transaction 可无条件应用，负 frame number 在 Java 层也会忽略 deferral。它不保证两个 Surface 在同一次 latch，更不保证同一 present。

sequence handler 只有在消息序号等于最新 `mLastSequenceNumber` 时才把 release gate 置 true。旧 callback 的消息不会提前放行新参数；最新消息表示“这批 RT callback 已消费参数，或已选择丢弃，之后不再读取它们”。它不证明：

- 每个 target 有效并被写入；
- native `Transaction.apply()` 确实向 SF 发出 IPC；
- SF 已接受、latch 或合成；
- HWC 已 present。

正常成功路径下，Java `apply()` 经 JNI 进入 native `SurfaceComposerClient::Transaction::apply()`，并尝试调用 SurfaceFlinger 的 `setTransactionState()`；所以逐帧几何不走 WMS/ATMS，不等于数据全程留在 Launcher 进程。

## 12. ReleaseCheck防早释放，也可能永久关门

`RemoteAnimationTargets.release()` 是幂等的。它逐个看 `ReleaseCheck`，遇到第一个 false 就给它登记 `this::release` 并返回；该 check 转 true 后重新从第一项扫描。全部通过后才清 checks，并释放 apps/wallpapers target 的本地 leash 以及 target 自己持有的非空 `startLeash`。这不删除服务端 Task，也不代替 WMS 恢复 Surface 树。

两类 check 保护不同生产者：

| check | false 的区间 | true 最多证明 |
|---|---|---|
| `SurfaceTransactionApplier` | 最新参数可能仍被 RT callback 读取 | 最新 callback 已消费或丢弃参数 |
| `RectFSpringAnim` | X、Y、scale 三个物理动画尚未全部结束 | spring 不再生产新 rect 更新 |

所以 spring 结束不能替代 applier check；最后一帧参数仍可能在 RT 队列中。反过来，applier sequence 完成也不代表 gesture state machine 已允许 controller finish。

这套门没有 timeout，也有几个永久关闭边界：

- `ReleaseCheck.mCanRelease` 初值为 false；
- applier 构造不会先置 true；
- `scheduleApply()` 发现 View 为 null 时原样返回；
- `ViewRootImpl` 没有 threaded renderer 时，RT callback 可能没有注册；
- callback 中 defer、setter 或 apply 抛异常时，外层虽可记录异常，applier 没有 `finally` 补发 sequence。

barrier 在构造时只抓一次，之后 Surface 重建不会刷新；barrier 无效时会补发 sequence，通常可以重新打开 release gate，所以它不是永久卡门条件。它的风险是整批更新被丢弃，且不会自动切到 immediate fallback。

`SurfaceTransactionApplier.create()` 对已 attach 的判断也只要求存在 `ViewRootImpl`，不证明 render barrier 有效。尚未 attach 时它安装 listener，未来 attach 才回调；若 View 永远不 attach，callback 不发生。把 applier 加进 release checks 前，应理解这些生命周期前提，而不是把“create 被调用”当作可释放保证。

late add 也不会逆转已经完成的 release：`mReleased` 为 true 后再加入 check，并没有重新持有已释放句柄。这个对象依赖调用方在 release 前完成 check 注册。

## 13. 回Home收束不是normal progress继续播放

只有手势终态被判为 HOME，且 `runOnRecentsAnimationStart` 门已满足后，handler 才创建 Home factory 与 window spring。目标不保证是真实桌面图标：

- Launcher Activity 存在、能按 package/user 找到已 attach 的 workspace View 时，可建立 `FloatingIconView` 并使用图标位置；
- 找不到图标时使用 icon-size 的 fallback rect，位于可用区域主轴中心、次轴靠近 hotseat；
- Fallback Launcher 还可在 gesture contract 后动态取得新 rect/surface 并 retarget。

创建 spring 的顺序尤其重要：

1. `mCurrentShift.updateValue(startProgress)`；若值变化，会先触发一次完整 `updateFinalShift()`；
2. 又显式调用 `mTaskViewSimulator.apply(...)`，因此可能再排一批相同起点参数；
3. 复制当前 crop；
4. 新建 `homeToWindowPositionMap` 并调用 rotation helper；
5. 再用 simulator 当前 matrix 映射 crop 得到 start rect；
6. 求 home-to-window 的逆，把 start rect 转到 Launcher 空间；
7. 创建 `RectFSpringAnim`，让 factory 保存/接线该动画；
8. 构造 runner 时才快照 start radius，再注册 update 与 animator listeners。

它快照的是 crop、start rect 与 start radius，不是冻结 simulator matrix。runner 持有自己的 Matrix，每次 update 都用 `setRectToRect(crop, currentWindowRect, FILL)` 重建。

r48 在第 4、5 步之间有一个真实实现缺口：

```java
public void applyWindowToHomeRotation(Matrix matrix) {
    mMatrix.postTranslate(mDp.windowX, mDp.windowY); // 写的是成员
    postDisplayRotation(..., matrix);                // 写的是参数
    matrix.postTranslate(...);                       // 写的是参数
}
```

正常 `TaskViewSimulator.apply()` 传入的就是成员 `mMatrix`，所以三步落在同一对象。回 Home 路径传入新 Matrix：新 home-to-window map 漏掉 windowX/Y，而刚算好的 simulator matrix 被额外平移；随后 start rect 又由这个被改动的成员映射。`windowX/windowY == 0` 时现象被掩盖，非零窗口原点时两侧映射会不对称。阅读应以这份 r48 实现为准，不能按方法注释补成理想变换。

## 14. Spring的第二个参数只是尺寸进度

`RectFSpringAnim` 同时启动三个独立物理动画：

- center X：`FlingSpringAnim`；
- top 或 bottom Y：另一条 `FlingSpringAnim`；
- width/height 插值：目标为 1 的 `SpringAnimation`。

OnUpdateListener 的第二个参数是 `mCurrentScaleProgress`。任何 X/Y 更新也会带着“当时的尺寸 spring 值”回调；它不是三个动画共享的单调总时间。scale spring 只设最大值 1，没有设置最小值，且初速度可为负，所以也不应承诺始终严格位于 0—1。

每次回调：

1. current rect 的中心/纵向边来自 X/Y，宽高来自 scale progress；
2. Home animation playback controller seek 到 scale progress；
3. current rect 映到 window 空间；
4. `setRectToRect(..., FILL)` 生成 leash matrix；
5. local radius 从 start 插值到 `crop.width()/2`；
6. App alpha 在 progress 0—0.85 内按 `ACCEL_1_5` 下降，达到 0.85 后为 0；
7. 构建 matrix/crop/radius/alpha 并交给 applier；
8. factory 收到 Launcher rect、progress 与 `mapRadius()` 后的近似标量。

`FILL` 可做非等比 scale，终点 `crop.width()/2` 只是源码所说的“足够圆”，不是物理 window corner 的精确恢复公式。Floating icon 自身的 shape reveal 又在更靠后的独立区间开始，不能把 App 的 0.85 alpha 阈值当作图标轮廓阈值。

真实 workspace/hotseat 也不是由这个 playback fraction统一 seek。Launcher factory 创建的 Activity animation 可不包含 StateHandler 属性；workspace/hotseat 常由另行启动的 atomic/staggered animation驱动。Fallback 分支的 home alpha、home scale 与 gesture-contract icon Surface 又有各自 Animator/Transaction。只能说这些动画在同一次终态流程中协作，不能说它们共享一根 spring 时间轴。

`mTargetRect` 保存调用者传入的 RectF 引用而不是副本。图标重新布局时可以修改同一对象，再调用 `onTargetPositionChanged()` retarget X/Y；width/height 也在每次 update 从可变 target 读取。`cancel()` 先通知 update listeners 的 `onCancel()`，再调用 `end()`；它没有派发标准 `AnimatorListener.onAnimationCancel()`，最终仍会派发 `onAnimationEnd()`。因此 factory cancel 与 `AnimationSuccessListener` 的结果语义要按具体实现核对，不能套普通 Animator 直觉。

spring success 最多令 handler 写 `STATE_END_TARGET_ANIMATION_FINISHED`。它还要等待 Recents scrolling、截图与多状态门，之后才可能 finish Recents controller。Home 收束到目标 rect不等于 WMS 已收回 leash，更不等于最后一帧 present。

## 15. 九组macOS只读练习

下面命令只读源码，不需要编译 AOSP。每条 `rg` 都使用独立且已存在的锚点；路径基于本章同一份 r48 工作树。

### 练习 1：确认shift可以超过1

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/SwipeUpAnimationLogic.java"
rg -n -F 'mDragLengthFactor = (float) dp.heightPx / mTransitionDragLength;' "$F"
rg -n -F 'mDragLengthFactor = 1 + AnimatorControllerWithResistance.TWO_BUTTON_EXTRA_DRAG_FACTOR;' "$F"
rg -n -F 'shift = mDragLengthFactor;' "$F"
rg -n -F 'mCurrentShift.updateValue(shift);' "$F"
```

观察：字段注释写 0—1，但两种导航都允许 `factor > 1`；真正上限来自 displacement 分支。

### 练习 2：拆开normal与resistance

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/src/com/android/quickstep/util/AnimatorControllerWithResistance.java"
rg -n -F 'float normalProgress = Utilities.boundToRange(progress, 0, 1);' "$F"
rg -n -F 'if (maxProgress <= 1) {' "$F"
rg -n -F 'float resistProgress = progress <= 1 ? 0 : Utilities.getProgress(progress, 1, maxProgress);' "$F"
rg -n -F 'mResistanceController.setPlayFraction(resistProgress);' "$F"
```

观察：只有 normal 明确 clamp；resistance 依赖上游把 progress 封在 maxProgress。

### 练习 3：核对两级缓存与提前返回

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/TaskViewSimulator.java"
rg -n -F 'if (mDp == null || mThumbnailPosition.isEmpty()) {' "$F"
rg -n -F 'if (!mLayoutValid) {' "$F"
rg -n -F 'if (!mScrollValid) {' "$F"
rg -n -F 'mScrollValid = false;' "$F"
rg -n -F 'mLayoutValid = false;' "$F"
```

观察：layout 失效会在重算后连带 scroll 失效；单独滚动不必重建 preview inverse。

### 练习 4：确认live leash没有读取snapshot rotation

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/TaskViewSimulator.java"
rg -n -F 'private final ThumbnailData mThumbnailData = new ThumbnailData();' "$F"
rg -n -F 'mThumbnailData.insets.set(insets);' "$F"
rg -n -F 'mThumbnailData.windowingMode = WINDOWING_MODE_FULLSCREEN;' "$F"
rg -n -F 'mThumbnailData.rotation = mOrientationState.getDisplayRotation();' "$F"
```

观察：Simulator 合成自己的 ThumbnailData；真实截图对象的 rotation/scale 不在这条赋值链里。

### 练习 5：按源码顺序标出九层matrix

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/TaskViewSimulator.java"
sed -n '257,287p' "$F"
rg -n -F 'mMatrix.set(mPositionHelper.getMatrix());' "$F"
rg -n -F 'mMatrix.postScale(mCurveScale, mCurveScale, taskWidth / 2, taskHeight / 2);' "$F"
rg -n -F 'applyWindowToHomeRotation(mMatrix);' "$F"
```

观察：每帧先 `set`，所以不会把上一帧 matrix 累乘进来；crop 在最终 matrix 之后另算。

### 练习 6：证明crop只逆preview matrix

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/TaskViewSimulator.java"
rg -n -F 'mPositionHelper.getMatrix().invert(mInversePositionMatrix);' "$F"
rg -n -F 'mInversePositionMatrix.mapRect(mTempRectF);' "$F"
rg -n -F 'mTempRectF.roundOut(mTmpCropRect);' "$F"
rg -n -F 'mInversePositionMatrix.mapVectors(mTempPoint);' "$F"
```

观察：crop 与 radius 都只回到 source-local；最终 Recents/window 变换没有参与求逆。

### 练习 7：还原target分派

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/TransformParams.java"
rg -n -F 'new SurfaceParams[targets.unfilteredApps.length]' "$F"
rg -n -F 'if (app.mode == targets.targetMode) {' "$F"
rg -n -F 'mHomeBuilderProxy.onBuildTargetParams(builder, app, this);' "$F"
rg -n -F 'mBaseBuilderProxy.onBuildTargetParams(builder, app, this);' "$F"
rg -n -F 'proxy.onBuildTargetParams(builder, app, this);' "$F"
```

观察：home、Assistant 都先受 mode 门约束；主 proxy 在普通/Assistant 分支都会继续执行。

### 练习 8：检查SurfaceParams的copy与flags

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/frameworks/base/packages/SystemUI/shared/src/com/android/systemui/shared/system/SyncRtSurfaceTransactionApplierCompat.java"
rg -n -F 'this.matrix = new Matrix(matrix);' "$F"
rg -n -F 'this.windowCrop = windowCrop != null ? new Rect(windowCrop) : null;' "$F"
rg -n -F 'flags |= FLAG_MATRIX;' "$F"
rg -n -F 'if ((flags & FLAG_MATRIX) != 0) {' "$F"
rg -n -F 't.setRelativeLayer(surface, relativeTo, relativeLayer);' "$F"
```

观察：构造会复制 Matrix/Rect，但 public final 只固定引用；字段是否应用由 flags 决定。

### 练习 9：验证sequence只是参数消费门

```bash
set -eu
SRC=/Users/ninebot/androidSource
F="$SRC/packages/apps/Launcher3/quickstep/recents_ui_overrides/src/com/android/quickstep/util/SurfaceTransactionApplier.java"
rg -n -F 'setCanRelease(msg.arg1 == mLastSequenceNumber);' "$F"
rg -n -F 'if (mBarrierSurfaceControl == null || !mBarrierSurfaceControl.isValid()) {' "$F"
rg -n -F 'if (surfaceParams.surface.isValid()) {' "$F"
rg -n -F 't.apply();' "$F"
rg -n -F 'view.invalidate();' "$F"
```

观察：barrier 无效分支没有 `t.apply()`，仍会发 sequence 消息；所以 release gate 不是 transaction/present 成功证明。

## 16. 把逐帧链压缩成一张排查表

一次正常 App→Overview 帧可以压缩为：

```text
方向化 displacement
→ shift（normal 0—1，之后可进入 resistance）
→ AnimatedFloat 更新 simulator 属性
→ layout/scroll cache
→ preview → drawn insets → Task/curve/scroll → Recents → window 的 matrix
→ inverse preview 得 crop 与 source-local radius
→ TransformParams 按 mode/activityType 为全部 App target 分派
→ SurfaceParams 的值复制与 flags
→ Launcher RT callback 条件式 defer
→ Transaction.apply 在正常路径尝试跨进程交给 SurfaceFlinger
→ latch / composition / present（本章 Java 层没有回执）
```

遇到问题时按现象选入口：

| 现象 | 先查 | 不要先下的结论 |
|---|---|---|
| 只在横竖屏切换跳动 | 两段 rotation、running target origin、windowX/Y | 一定是 WMS bounds 错 |
| 卡片跟 scroll 错位 | scroll cache、curve scale、RT barrier | 一定是手势 shift 算错 |
| crop 露边/切边 | drawn insets、inverse 是否成功、roundOut | 应该逆整个 final matrix |
| Home 收束起点偏移 | r48 helper 的成员/参数不对称 | 新 home-to-window map 含完整平移 |
| Assistant alpha 不跟手 | `TransformParams.mProgress` 的真实写点 | 它等于 `mCurrentShift` |
| finish 后引用未释放 | spring check、applier latest sequence、RT callback 注册 | spring end 就足够 |
| sequence 已完成仍没画面 | invalid barrier/target、native apply、SF/HWC | sequence 就是 present fence |

最终应保留七条结论：

1. shift、fullscreen progress、TransformParams progress 与 spring scale progress 是四套状态；
2. Simulator 模拟 live leash 几何，并未读取真实 TaskSnapshot rotation/scale；
3. matrix 决定“摆到哪里”，crop/radius 要回到 source-local，不能逆最终全局矩阵；
4. TransformParams 先按 mode/activity type 分派，再由 proxy 写具体字段；
5. SurfaceParams 依赖 copy + 不再修改约定，不是类型级不可变；
6. applier sequence 只关闭客户端参数读取窗口，既可能伴随 apply，也可能代表丢弃；
7. 回 Home 是独立的三物理动画与多状态门，既不是 normal progress 延长，也不是 controller finish/present 完成点。

第 226 章将沿着抬手后的另一条主线继续：`GestureState`、`MultiStateCallback`、终态选择、截图、Recents scroll 与 Launcher Activity readiness 怎样在异步到达时只触发一次正确动作。
