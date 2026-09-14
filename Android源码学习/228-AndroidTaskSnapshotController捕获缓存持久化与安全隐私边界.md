# 228 Android TaskSnapshotController捕获、缓存、持久化与安全隐私边界

本文基于 `android-11.0.0_r48`，沿着一张最近任务缩略图的完整生命线，回答四个必须分开的问题：系统何时决定截图，抓到的究竟是真实 Surface 还是主题占位图，结果何时只存在于内存、何时才可能落盘，以及安全策略在哪一层阻止真实内容进入快照。

这里的核心不是记住几个方法名，而是建立一套能处理竞态与失败的判断框架：**捕获返回、缓存可见、写任务执行、重启后可恢复，是四个不同的完成点。**

版本锚点：

- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotController.java`
- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotCache.java`
- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotPersister.java`
- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotLoader.java`
- `frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java`
- `frameworks/base/services/core/java/com/android/server/wm/WindowState.java`
- `frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java`
- `frameworks/base/services/core/java/com/android/server/wm/RecentTasks.java`
- `frameworks/base/services/core/java/com/android/server/wm/TaskSnapshotSurface.java`
- `frameworks/base/services/devicepolicy/java/com/android/server/devicepolicy/DevicePolicyCacheImpl.java`
- `frameworks/base/core/java/android/view/SurfaceControl.java`
- `frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`
- `frameworks/native/services/surfaceflinger/BufferLayer.cpp`

## 1. 先用一个真实场景划清四个完成点

用户从应用 A 回到桌面，随后打开 Overview。看起来只是“显示一张 A 的缩略图”，系统内部却至少经历四个阶段：

1. WindowManager 发现 A 所在 Task 即将整体不可见，决定是否捕获。
2. SurfaceFlinger 把 Task 的可见子层渲染进离屏缓冲区，或由 system_server 绘制主题占位图。
3. `TaskSnapshotCache` 立即保存对象，Overview 已可能从内存读到它。
4. `TaskSnapshotPersister` 只把 Store 项排进后台队列；稍后才尝试写 proto 与 JPEG。

因此，下面四句话不能互换：

| 说法 | 在 r48 中真正成立的条件 |
| --- | --- |
| “画面抓到了” | 原生捕获返回非空缓冲区，尺寸也通过 Controller 校验 |
| “当前进程能取到” | 新对象已放进 running cache；也可能读到先前的旧对象 |
| “写盘动作执行过” | Store 项没有被队列深度淘汰，用户已解锁，后台线程确实调用了 `write()` |
| “重启后可恢复” | proto 与所选 JPEG 都可读、可解码，而且二者恰好能组成可用结果 |

`snapshotTasks()` 的提交顺序尤其重要：先 `putSnapshot()`，再 `persistSnapshot()`，最后 `task.onSnapshotChanged()`。通知发出时，持久化只是入队，甚至该 Store 项以后可能在执行前被丢弃。通知不是磁盘提交回执。

反过来，磁盘恢复成功也不会回填 running cache。下一次请求仍可能再次读取 proto、解码 JPEG、复制成硬件位图并创建 `GraphicBuffer`。

这给排障一个最有用的起点：先问“失败发生在哪个完成点”，不要把所有空白、旧图和重启后丢失都归为“截图失败”。

## 2. Controller不是仓库，而是生命周期编排器

`TaskSnapshotController` 在 WMS 内持有三个协作者：

- `TaskSnapshotCache`：保存当前 system_server 生命周期中的快照对象，并负责按需转交 Loader。
- `TaskSnapshotPersister`：管理后台写队列、文件写入、删除和过期清理。
- `TaskSnapshotLoader`：从 credential-encrypted 目录读取 proto 与 JPEG，重建 `TaskSnapshot`。

Controller 自己负责的是策略与时序：

```text
入口事件
  ↓
候选 Task + 模式判定
  ↓
REAL: prepare → SurfaceFlinger capture
THEME: system_server 绘制主题图
  ↓
running cache → Store 入队 → onSnapshotChanged
```

`systemReady()` 只负责启动 Persister 的后台线程。它不预热磁盘快照，也不把已有文件装回内存。

线程边界同样不能忽略：

- 普通捕获决策在 WM/ATMS 的锁语境中发生。
- Controller 的无参 `new Handler()` 绑定构造 WMS 时所在的 DisplayThread Looper；`post()` 是切回该 Looper，不是创建一条 Controller 专属线程。
- 原生 layer capture 的正常路径会对 RenderEngine draw fence 调用 `sync_wait(fd, -1)`；但 r48 没检查该 wait 的返回值，所以代码表达了“等待离屏目标”的意图，不能把异常 wait 分支也提升成硬成功证明。这仍不等于某一帧已在物理屏幕 present。
- running cache 更新是同步动作。
- Store 只是同步入队，实际 I/O 在 `TaskSnapshotPersister` 线程完成。
- `onSnapshotChanged()` 对本地监听者可直接调用，对远端监听者则通过 Handler 消息分发；两者都不是写盘完成信号。

快照 ID 取自 `System.currentTimeMillis()`。它适合标识一次捕获，却不是严格单调、全局唯一的事务序号，不能据此推导文件组的一致代次。

## 3. 五类入口并不共享同一个总开关

第一类是应用切换的常规路径。`onTransitionStarting(DisplayContent)` 与“没有 transition 但可见性变为 false”的路径都会进入 `handleClosingApps()`。前者是在 `handleAppTransitionReady()` 中，closing Activity 已提交不可见、动画已 apply、`goodToGo()` 已执行之后，并且发生在清空 `mClosingApps` 之前。`handleClosingApps()` 先检查产品形态开关，再收集真正整体不可见的 Task。

第二类是 resumed Activity finish 分支的提前捕获。`ActivityRecord` 在隐藏前直接调用 `snapshotTasks(task)`，随后把 Task 加入 skip 集合，意图是在后续 closing pass 中避免重复抓取；这不是任意 Activity finish 都会走的通用动作。

第三类是 Recents animation 的主动请求。`screenshotTask()` 与 `screenshotRecentTask()` 同样先直接调用 `snapshotTasks()`，再加入 skip 集合，然后从 running cache 取结果。

第四类是息屏。`screenTurningOff()` 把工作投递给 Handler；在全局锁内遍历所有可见 Task，必要时允许临时捕获 Home，最后才调用 `ScreenOffListener.onScreenOff()`。

第五类不是捕获入口，而是 Binder 读取：`ActivityTaskManagerService.getTaskSnapshot()` 查找 Task 后调用 Controller 的读取链。它可能命中内存，也可能离锁读取磁盘。

容易被忽略的事实是，`shouldDisableSnapshots()` 在 r48 只有两处调用：

- `handleClosingApps()`；
- `screenTurningOff()`。

TV、Wear、Embedded 对这两条路径会关闭快照，但这个方法不是 `snapshotTasks()` 内的总门。Activity finish 的提前捕获与两条 Recents 主动捕获会绕过它。因而不能把“该产品形态关闭常规快照”扩大成“任何入口都绝不捕获”。

正确的审计方法，是从每个调用点沿控制流分别追踪，而不是只读一个名字像总策略的方法。

## 4. closing候选、skip集合与失败后的第二次机会

`getClosingTasks()` 不会把 closing Apps 简单转换成 Task 列表。候选至少要满足：

- Activity 仍属于某个 Task；
- 整个 Task 已经不可见，而不只是某个 Activity 正在关闭；
- Task 不在 `mSkipClosingAppSnapshotTasks` 中。

集合使用 `ArraySet` 去重，所以同一 Task 内多个 Activity 同时关闭通常只触发一次捕获。

skip 的设计假设是：“调用者刚刚已经得到了一张更早、更合适的快照。”但 r48 的 `snapshotTasks()` 返回 `void`，调用方不知道捕获是否成功。Activity finish 和 Recents 两条路径都会在调用后无条件加入 skip。

由此形成一个具体失效窗口：

```text
提前捕获进入 snapshotTasks
  ↓
prepare 或原生 capture 失败，新快照没有写入 cache
  ↓
调用方仍把 Task 加入 skip
  ↓
稍后的 closing pass 跳过该 Task
```

若 running cache 中已有旧图，Recents 随后的读取还可能返回旧图；若没有旧图，则得到 `null`。无论哪种结果，skip 都已经占用了下一次常规捕获机会。

`handleClosingApps()` 走到末尾后会清空 skip 集合，所以它不是永久黑名单。问题在于这一次关键的不可见边沿已经过去，清空集合不会自动补抓。并且清理没有放在 `finally`：`shouldDisableSnapshots()` 的提前返回不会清集合，未捕获异常也可能留下集合状态。

诊断“偶发旧缩略图”时，至少同时记录：提前入口、prepare 失败原因、capture 结果、put 是否发生、skip 加入时刻与后续 closing pass。只看最终 `getSnapshot()` 返回值无法区分“新抓成功”和“失败后读到旧缓存”。

## 5. NONE、REAL与APP_THEME如何决策

除临时 Home 分支外，Controller 先通过 `getSnapshotMode(Task)` 选择模式：

- `ACTIVITY_TYPE_STANDARD`、`ACTIVITY_TYPE_UNDEFINED`、`ACTIVITY_TYPE_ASSISTANT` 可以继续判断。
- 其他 Activity type 返回 `SNAPSHOT_MODE_NONE`。
- 允许的 Task 再查看 topmost Activity 是否要求使用应用主题图。
- 若要求，则走 `SNAPSHOT_MODE_APP_THEME`；否则走真实 layer capture。

若 topmost Activity 本身为 `null`，r48 的模式函数仍返回 REAL；真正的失败通常稍后发生在 prepare 阶段。

这里有两个对象不能混淆：

1. **模式决策对象**是 `task.getTopMostActivity()`。
2. **真实捕获准备阶段选择的 Activity**来自 `findAppTokenForSnapshot()`，它会跳过没有可用 Surface、没有主窗口或没有已显示窗口的候选。

例如一个位于栈顶但尚未真正显示的 trampoline Activity，可能参与了主题/隐私模式判断，却不是 REAL 路径用来收集窗口元数据的 Activity。审计时必须同时写下“topmost 是谁”和“findAppToken 选中了谁”。

临时 Home 是特殊分支：当 `allowSnapshotHome=true` 且 Task 是 Home 时，代码直接尝试 REAL 捕获，不经过 `getSnapshotMode()`。它也不进入常规持久化与通知流程，只更新内存缓存。第 6 节会说明这条旁路为何值得单独做安全审计。

NONE 并不删除旧缓存或旧文件；它只表示本次不产生新快照。因此看到旧图时，要区分“当前策略允许读取旧数据”和“当前时刻允许重新捕获真实内容”。

## 6. 安全策略发生在模式选择，原生合成仍有第二道保护

topmost Activity 的 `shouldUseAppThemeSnapshot()` 在两类条件下返回 true：

- 应用通过隐藏接口 `Activity.setDisablePreviewScreenshots(true)` 禁止 Overview 预览；
- 该 Activity 的窗口集合中存在 `WindowState.isSecureLocked()` 为 true 的窗口。

`WindowState.isSecureLocked()` 又包含两条来源：窗口带 `FLAG_SECURE`，或者设备策略不允许该用户截屏。设备策略检查把 `mShowUserId` 与 `mOwnerCanAddInternalSystemWindow` 一起交给 `DevicePolicyCache`；拥有添加内部系统窗口能力的 owner 在该 DPM 判断中有例外，但显式 `FLAG_SECURE` 不因此失效。这里不是简单的“用户策略关闭即所有窗口一律 secure”。Surface 创建时，这个判断会转成 `SurfaceControl.SECURE`；设备策略动态变化时，WMS 也会刷新现有 Window Surface 的 secure 状态。

`setDisablePreviewScreenshots()` 与 `FLAG_SECURE` 也不是同一承诺。前者针对最近任务表示，后者还影响更广泛的屏幕捕获与投屏语义。

在生产链中，安全门首先通过 APP_THEME 避免把真实 Task 内容送入快照。但即使审视 native 兜底，也不能得出“system_server 有权限，所以安全层会被真实抓入”的结论。

r48 的调用链是：

```text
SurfaceControl.captureLayersExcluding
  → android_view_SurfaceControl.cpp
  → SurfaceComposerClient::captureChildLayers
  → SurfaceFlinger::captureLayers
```

layer capture 的 render target 不是 secure target；`LayerRenderArea::isSecure()` 返回 false。`BufferLayer` 在向非安全目标绘制 secure layer 时会输出黑色。AID_SYSTEM 通过的是“允许发起捕获”的权限门，不会把目标自动升级为可承载 secure 内容的缓冲区。

JNI 返回的 `capturedSecureLayers` 标记在这条 r48 路径被写成 false，不能用它证明“Task 中没有 secure layer”；真正的内容保护依靠模式选择和 SurfaceFlinger 的非安全目标合成行为。

仍需保留一个策略边界：模式判断只遍历 topmost `ActivityRecord` 的窗口，而 Task 可能包含别的 Activity；REAL 路径也可能选中另一 Activity 作为元数据来源。代码结构不能证明“Task 内任意 Activity 的任意 secure 窗口都必然触发 APP_THEME”。下层 Activity 的 secure BufferLayer 仍受 native 黑化保护，但可能留下黑块而不是品牌化主题图；下层 Activity 仅设置 `mDisablePreviewScreenshots` 时则没有这道 Surface secure 兜底。

这些策略只约束本次新捕获。动态启用 `FLAG_SECURE`、DPM 禁止截屏或 disable-preview，本身都不会追溯删除旧 running/disk snapshot，读取路径也不会重新检查当前隐私状态。只有后来成功生成 APP_THEME，才会替换 running entry；磁盘替换仍是异步的，Store 若被淘汰，旧磁盘图还会继续存在。设备策略更新会刷新相应用户现有 Window Surface 的 secure 状态，却不会主动清除已经保存的旧 TaskSnapshot。

锁屏不是统一的“禁止持久化”信号。Store 检查的是用户是否已解锁 credential-encrypted 存储；设备重新上锁后，用户通常仍处于 unlocked 状态，普通快照仍可写。Keyguard 配置为 secure 只在息屏路径决定是否允许临时 Home 捕获，含义也不是“Keyguard 此刻一定正在显示”。该临时路径还会绕过 Home 的 disable-preview 模式判断，代码本身也没有再次比较 `task.mUserId` 与当前用户；它依赖可见 Task/用户状态提供通常成立的上下文约束。

## 7. REAL捕获前的准备门与元数据来源

`prepareTaskSnapshot()` 会在真正进入 SurfaceFlinger 前逐项收集条件和元数据。主要失败门包括：

- `PhoneWindowManager.isScreenOn()` 为 false；它来自默认显示的 early-on 状态，不是待抓 Task 所在 display 的 HWC present 证明；
- 找不到满足条件的 Activity；
- Activity 的 Surface 正挂在已提交的 animation leash 上；
- 找不到主窗口；
- Activity 正处于 fixed-rotation transform。

`findAppTokenForSnapshot()` 要求候选 Activity 的 Surface 正在显示、存在主窗口，并且至少一个窗口动画器处于 shown 且 alpha 大于 0。它会跳过只负责跳转、尚无可见 Surface 的栈顶 Activity。

通过准备门后，Builder 先收集的核心数据包括：

- `id`、被选中 Activity 的组件名；
- 已把 letterboxInsets 合入其中的 contentInsets；
- orientation、display rotation、windowing mode；
- system UI visibility；
- 是否真实快照、是否 translucent；
- 预期 pixel format。

Task 的未缩放尺寸不是 `prepareTaskSnapshot()` 写入的。native capture 返回后，`createTaskSnapshot(task, builder)` 才把本地 crop 的宽高填进 `taskSize`；最终对象也不另存一份独立的 letterboxInsets。

contentInsets 不是简单复制 Task 边界。代码先对主窗口的 contentInsets 与 stableInsets 逐边取较小值，再加上 letterboxInsets；它的坐标语义以窗口 frame 为参照。

system UI visibility 取自 Task 顶部 fullscreen Activity 的顶部 fullscreen opaque window；没有这样的窗口时为 0。它不是把 Task 中所有窗口的 UI flag 做并集。至此已有三套选择器：模式看 topmost Activity，REAL 准备看首个 surface-ready Activity，system UI 元数据又看 top fullscreen opaque Window，它们不保证指向同一对象。

像素格式选择也只是策略意图：当调用参数 `pixelFormat == UNKNOWN`、设备允许 16-bit、被选中的 Activity fillsParent，且不存在“主窗口 translucent 同时显示 wallpaper”的组合时，Builder 选择 `RGB_565`；其余情况选择 `RGBA_8888`。`isTranslucent` 则由这个预期格式、Activity 填充状态和窗口格式推导。下一节会看到，r48 Java wrapper 并没有把该格式真正传到 native capture。

## 8. Task Surface只是坐标根，真正抓的是可见后代层

`createTaskSnapshot()` 以 Task 的 `SurfaceControl` 为根，把完整 Task bounds 平移到 `(0, 0)` 形成本地 crop，设置缩放，并把当前 IME Surface 放入 exclude 列表，然后调用 `captureLayersExcluding()`。Insets 只是随快照保存的元数据，不参与这次 crop。

这里“以 Task Surface 为根”不等于把根层自身当作图像内容。r48 经 JNI 调到 `captureChildLayers`，native 使用 `childrenOnly=true`：Task Surface 提供层级边界与坐标锚点，实际遍历、合成的是它的可见后代层。

排除 IME 的目的，是避免把共享的输入法 Surface 固化进某个应用 Task 的缩略图。被排除的 layer 连同其后代都会跳过。系统条、应用窗口、壁纸等是否进入结果，取决于它们在该 Task 子树中的归属、可见性以及 capture 的层级过滤，而不能只凭屏幕上“肉眼重叠”判断。

r48 还有一个精确的版本实现差异：

- Controller 把准备阶段选择的 `pixelFormat` 传给 `SurfaceControl.captureLayersExcluding()`。
- Java wrapper 内部调用 native 时却硬编码 `PixelFormat.RGBA_8888`。
- 所以真实捕获缓冲区不是由 Controller 的 `RGB_565` 意图直接决定。
- 但预期格式仍参与 `isTranslucent` 推导，Loader 也会依据持久化的 translucency 与当前 16-bit 配置选择解码偏好。

这意味着“内存占用按 RGB_565 计算”和“isTranslucent 一定反映实际 buffer alpha 能力”都不安全。版本分析必须同时核对调用者参数、Java wrapper 和 native 实参。

缩放因子默认为资源中的高分辨率比例，overlay 可在 `(0, 1]` 内覆盖。`taskSize` 保留未缩放尺寸；buffer 尺寸则按 crop 与 scale 截断为整数，native 会把极小结果钳到 1。REAL 的 `createTaskSnapshot()` 在宽或高不大于 1 时直接返回 `null`，这条分支没有显式 destroy；公共提交层另行检查已经构成的非空 `TaskSnapshot`，若宽或高等于 0 才显式销毁 buffer。因此理论上 APP_THEME 与 REAL 的 1 像素边界并不完全相同。

rotation 只是保存进对象的元数据，Controller 没有向 layer-capture API 传入旋转参数。REAL 的 ID 在进入 native capture 前取得，APP_THEME 的 ID 在硬件位图创建后取得；两者都不能当作精确的像素采样时间。

正常的原生调度在离屏渲染并成功等待 draw fence 后返回，此时可把 buffer 视为可用；由于 `sync_wait` 返回值未检查，单凭 Java 返回不能覆盖 wait 出错的边角分支。无论如何，它都不能解释为“该画面已经在物理显示器完成 present”，更不能延伸成“文件已经持久化”。

## 9. APP_THEME是重新绘制，不是给真实截图打码

APP_THEME 路径不调用 SurfaceFlinger 去抓应用像素，而是在 system_server 中创建硬件位图并绘制：

- 不透明的 `TaskDescription` 背景色；
- 根据主窗口 InsetsState、requested visibility 与 system UI 状态计算出的系统栏背景；
- 与缩放后 Task 尺寸对应的 RenderNode 内容。

因此，它的安全性质来自“没有读取应用真实 layer”，不是先截图再模糊或遮盖。

它不会解析或绘制应用主题里的 `windowBackground` drawable，也不绘制时间、通知图标、导航按钮或手势提示。状态栏或导航栏颜色透明、对应栏隐藏、或者 inset 为空时，相关矩形可能不画。`TaskDescription.backgroundColor == 0` 被强制补成不透明 alpha 后是黑色，而不是默认白色。

该路径仍要求 topmost Activity 与主窗口存在。尺寸按高分辨率比例缩放并取整；极小边界可能得到无效尺寸，因此上层仍要检查最终 buffer。系统栏 Painter 使用这个 topmost Activity 主窗口的 flags、privateFlags 与 systemUiVisibility；快照对象中保存的 systemUiVisibility 却来自 top fullscreen opaque Window，两者仍可能不是同一窗口。

主题图的关键字段与 REAL 有意不同：

| 字段 | APP_THEME | REAL |
| --- | --- | --- |
| `isRealSnapshot` | false | true |
| `isLowResolution` | false | false（新捕获） |
| `isTranslucent` | false | 由准备阶段策略推导 |
| 内容来源 | system_server 绘制背景和系统栏 | Task 子层离屏合成 |
| rotation/orientation | 主窗口相关状态 | Task 配置与 Display 状态 |

主题背景色会被处理为不透明色。它保护内容，但不承诺精确复现应用退出前的最后一帧。产品若把“安全”与“视觉连续”同时设为验收条件，应分别验证，不能把主题图当作降质的真实截图。

相应地，`isRealSnapshot=true` 只证明本次走了 REAL 路径，不证明图中没有被 SurfaceFlinger 黑化的 secure 区域。

`TaskSnapshotSurface` 后续把快照作为 starting window 展示时，会移除 starting window 自身的 `FLAG_SECURE`。因此，生产时是否选择主题图、以及 native 是否黑化 secure layer，是阻止敏感内容进入可复用快照的关键；展示阶段不会替你把一张已经含敏感像素的位图重新变安全。

## 10. 从capture结果到通知：提交顺序仍不等于事务

对普通 Task，`snapshotTasks()` 在拿到有效对象后依次执行：

```text
mCache.putSnapshot(task, snapshot)
mPersister.persistSnapshot(task.mTaskId, task.mUserId, snapshot)
task.onSnapshotChanged(snapshot)
```

对临时 Home，则只有第一步。没有 Store，也没有 `onSnapshotChanged()`。

这个顺序带来三条可观察事实：

1. 监听者收到新 ID 时，running cache 已经可读。
2. 监听者收到通知时，磁盘文件可能仍是旧代、部分新代，或根本尚未开始写。
3. 若 Store 后来被队列淘汰，通知也不会撤回；没有生产回调告诉调用方持久化失败。

`TaskSnapshot` 适合被当作一次捕获的值载体，但不是深不可变对象。`getTaskSize()` 与 `getContentInsets()` 暴露的是可变的 `Point`、`Rect` 引用，`GraphicBuffer` 也可被销毁。调用方不能仅凭字段多为 final 就假设跨线程任意共享修改都安全。

Parcelable 写出时，如果 GraphicBuffer 已 destroyed，buffer 会以 `null` 写入，而其他元数据仍可继续序列化。接收方因此要把“对象非空”和“图像 buffer 非空”作为不同检查。

失败路径也不会统一清掉旧状态：本次模式为 NONE、prepare 失败、capture 返回空，通常都意味着“不提交新对象”，而不是“删除先前快照”。这正是旧图可以在新抓失败后继续出现的原因。

## 11. running cache的两张表、命中语义与残留映射

`TaskSnapshotCache` 使用两张 `ArrayMap`：

- `mRunningCache`：`taskId → CacheEntry(snapshot, topApp)`；
- `mAppTaskMap`：`ActivityRecord → taskId`，用于 Activity 移除或死亡时反查。

它不是 LRU，没有基于容量的自动驱逐。`putSnapshot()` 替换同一 taskId 的旧 entry，并维护 topApp 的反向映射。这里的 topApp 又是 put 当下重新取得的 `task.getTopMostActivity()`，不一定等于 REAL 准备阶段选中的 Activity；它是淘汰 owner，不是像素或 Insets 来源。

running hit 有两个容易误判的语义：

- 它按 taskId 查找，不校验请求传入的 userId；正确性依赖 taskId 的系统分配约束。
- 它忽略 `isLowResolution`，返回当前 running entry；新捕获对象固定不是 low，但这个 entry 相对本次请求仍可能已经陈旧。低清选择主要发生在磁盘 Loader 路径。

若内存 miss 且允许 restore，Cache 直接返回 `mLoader.loadTask()` 的结果，却不调用 `putSnapshot()`。所以恢复不是 cache warm-up：同一 Task 的下一次磁盘请求可能重复整套解码成本，而紧接着一次 `restoreFromDisk=false` 仍会 miss。

清理语义也有一个 r48 的尖锐边界：`clearRunningCache()` 只清空 `mRunningCache`，没有清空 `mAppTaskMap`。之后 `onAppRemoved()` 或 `onAppDied()` 虽会找到反向 taskId，但 `removeRunningEntry()` 只有在 running entry 仍存在时才顺带删除反向键；于是残留映射不会在这条路径中被回收。更糟的是，若同 taskId 后来放入 topApp=B 的新 entry，旧键 A→taskId 仍可在 A 的 removed/died 回调中按 taskId 删除当前 B 的 entry。这是从两张表的更新顺序可直接推导出的错误代际清理窗口。

Task 从 Recents 移除时，Controller 同步删内存 entry，再异步排一个磁盘 Delete。Activity removed/died 只影响内存。`invalidateHomeTaskSnapshot()` 也只删内存。普通移除并没有显式销毁 entry 中的 GraphicBuffer，生命周期最终依赖对象引用与底层资源管理。

因此，“清 cache”“杀 Activity”“从 Recents 删除”是三种不同操作，不能互相代替。

## 12. 写队列的深度、轮转与暂停边界

Persister 有一个总队列 `mWriteQueue`，另有一个只追踪待执行 Store 的 `mStoreQueueItems`。Store 上限为 2：新 Store 入队后，若待执行 Store 数超过 2，就淘汰最老的待执行 Store。

这个“2”不是整个队列容量，也不是系统内最多两个写操作：

- Delete 与 obsolete-cleanup 不计入、不因该上限被淘汰。
- Store 在真正 `write()` 前已从 `mStoreQueueItems` 移除，所以可有 1 个正在执行、2 个仍待执行。
- 队列不按 taskId 合并；同一 Task 的多个代次可以并存。
- 被淘汰的 Store 没有失败回调，也不会自动删除该 Task 的旧磁盘文件。

这会产生一种“内存新、磁盘旧”的稳定结果：新快照已缓存并通知，但对应 Store 在执行前被淘汰；内存一旦被清，Loader 又恢复先前磁盘代次。

`setPaused(true)` 只阻止后台线程取下一个 item，不中断正在执行的写。WindowAnimator 会在昂贵的 app transition、屏幕旋转和 Recents animation 期间暂停快照写入，以降低争用；解除暂停后再继续消费。

Store 的 ready 条件是所属用户已解锁。未 ready 的 Store 会被移到队尾，线程按约 100 ms 节流后重试；这不是注册一次 unlock 回调并休眠。队尾轮转让后面的 ready item 有机会执行，但当队列只有 locked-user Store 时，会持续周期性检查。

Delete 和 obsolete-cleanup 使用默认 ready=true，不共享 Store 的 unlock 门。它们可能在 CE 目录不可用时尝试删除；删除返回值没有被升级成重试协议。

## 13. 删除不是终止栅栏，三文件也不是原子快照

每个 Task 的常规磁盘表示位于用户的 credential-encrypted snapshots 目录，最多包含：

```text
<taskId>.proto
<taskId>.jpg
<taskId>_reduced.jpg
```

proto 保存 task width/height、insets、orientation、rotation、windowing mode、system UI visibility、real/translucent、top Activity 与快照 ID 等元数据。r48 的 `writeProto()` 对 left、top、right、bottom 四个 inset 各赋值一次，没有重复写 top 的问题。

图像路径把硬件 buffer 包装为 Bitmap，再复制成 ARGB 软件位图，以 JPEG quality 95 写高清图；构造 Persister 时已把配置值计算为 `mLowResScaleFactor = configLow / configHigh`，开启低清后直接乘这个比率再写 `_reduced.jpg`。JPEG 不保留 alpha，也没有在 proto 中保存原始 ColorSpace 或文件校验值。

事务边界比文件名看起来更弱：

- proto 单文件通过 `AtomicFile` 写，并在 finish 前同步其流。
- 两张 JPEG 直接用 `FileOutputStream` 覆写，没有同等级的临时文件替换与显式 fsync。
- 三个文件没有共同 generation、校验和或跨文件 commit marker。
- Loader 与写线程之间没有覆盖这三个文件的一把共享锁。

所以“proto 原子”不等于“快照原子”。并发读取可能遇到新 proto 加旧图、完整 proto 加半写 JPEG、高清已更新而低清仍旧等组合。返回 `null` 只是部分结果；如果旧 JPEG 仍能解码，也可能重建出元数据与像素不同代的对象。

Store 仅在 `writeProto()` 或 `writeBuffer()` 返回 false 时执行三文件清理。不能扩大成“任何实际故障都会删除整个组”：`Bitmap.compress()` 的 boolean 返回值没有检查，若运行时异常逃出也没有每个 item 的顶层恢复协议，删除自身的返回值同样被忽略。失败清理也不是回滚到上一组完整文件，而是尝试删除当前路径。

从 Recents 删除 Task 也不是队列的终止栅栏。删除入口不会取消已排队的同 taskId Store。根据现有轮转规则，可以推导出：locked-user Store 被转到 Delete 后面，Delete 先运行；用户随后解锁，旧 Store 又把文件写回来。以后一次 obsolete cleanup 可能清掉它，但 Delete 本身不保证“此后再无写入”。

过期清理会结合 persistent taskIds 与“上次清理请求后排过持久化的 taskIds”保护近期对象，但这是尽力而为的共享集合，不是代次栅栏。它只针对 Recents 数据已加载的 running users 异步排队执行；无法解析为合法 taskId 的陌生文件通常也会作为 `-1` 候选被删。

## 14. Loader恢复的是重建对象，不是原对象回放

Loader 的顺序是先读 proto，再根据请求与兼容规则选择 JPEG。现代格式下：

- high 请求选 `<taskId>.jpg`；
- low 请求选 `<taskId>_reduced.jpg`；
- 所选文件缺失就返回 `null`，没有从 low 自动回退 high 或反向回退。

当系统关闭低清快照时，是 Controller 在进入 Cache/Loader 前把 low 请求改成 false，所以最终读高清文件。若测试或内部代码直接要求 Loader 读取 low，而 reduced 文件不存在，Loader 仍返回 `null`。

对 Android O/P/Q 遗留文件，Loader 根据 proto 中 taskWidth 是否为 0、设备 low-RAM 状态、legacyScale 与高清文件存在性推导旧比例，有时会为名义上的 high 请求强制选择 reduced 文件。这条兼容分支还暴露一个元数据差异：重建对象的 `isLowResolution` 取传入的 `loadLowResolutionBitmap`，不是实际被兼容逻辑选中的文件，因此可能读了旧 reduced 图却标为 false。

解码时，`RGB_565` 只是 `BitmapFactory.Options.inPreferredConfig` 偏好，不是保证。软件 Bitmap 随后复制成 HARDWARE Bitmap，再提取 `GraphicBuffer`。任一步失败都可能返回 `null`。

重建对象还存在这些信息损失：

- proto 没有保存 `isLowResolution`，它由本次请求重建。
- ColorSpace 来自解码后的 Bitmap，不是原捕获对象的独立持久化字段。
- JPEG 丢弃 alpha；translucent 只是 proto 元数据，不能恢复原始透明像素。
- 没有 generation 或校验和验证 proto 与 JPEG 是否属于同一次捕获。

Loader 没有显式 `isUserUnlocked()` 检查，只尝试访问 CE 路径并在文件不可用或 I/O 异常时失败。它也不删除损坏文件。因此一次恢复失败不代表后续必然失败，但反复失败也不会由 Loader 自行修复磁盘组。

最后再强调一次：成功 load 的对象直接返回给调用者，不进入 running cache。

## 15. Binder读取、Recents直抓与息屏临时Home

公开 Binder 入口 `ActivityTaskManagerService.getTaskSnapshot()` 要求调用者与配置的 Recents 组件具有相同 appId，或持有 hidden、signature 级 `READ_FRAME_BUFFER`；普通三方应用不能获得后者。Recents 判断使用 `UserHandle.isSameApp()`，忽略 userId。服务清除调用身份，在全局锁内以 `MATCH_TASK_IN_STACKS_OR_RECENT_TASKS` 查找 Task；Task 找到后退出大锁，再以该 Task 自己的 `mUserId` 允许磁盘读取，避免把 JPEG I/O 放在 WM 全局锁内。

通过入口权限后，代码没有再做“调用者 user/profile 必须与 Task 相同”的匹配。CE 按 userId 分目录是文件命名与加密隔离，不是这个 Binder 方法额外实施的逐 Task 授权门。公开 Binder 路径固定 `restoreFromDisk=true`；`false` 是 starting window、Recents animation 等内部调用采用的策略。

请求 low resolution 时，Controller 只有在 Persister 配置支持低清文件的情况下才把 true 继续传给 Cache。running cache 命中仍返回内存对象，不因为 low 请求临时缩放；只有 miss 后的磁盘路径才按高清/低清文件选择。

Recents animation 的 `screenshotTask()` 更特殊：它在 WM 锁内主动 `snapshotTasks()`，无条件加入 skip，再以 `restoreFromDisk=false` 读取 running cache。代码传入 user 0，但 running hit 本身不检查 userId，且这里禁止磁盘恢复；关键索引仍是 taskId。若新抓失败而旧 entry 尚在，它可返回旧图。

`screenshotRecentTask()` 使用 Task 自己的 userId，但“主动抓取无返回值、无条件 skip、随后可能读旧 cache”这一时序相同。Recents 取消动画若拿到非空 snapshot，也不代表取消流程已结束；runner 以后调用 `cleanupScreenshot()`、截图动画清理后，才进入 animation finished。若 snapshot 为 null，则走立即结束分支。

息屏链路带有 listener，但在 r48 中它不是端到端的熄屏同步栅栏：

1. 产品形态禁用时，直接调用 `onScreenOff()`。
2. 否则向 Controller Handler 投递工作。
3. 工作取得 WM 全局锁，收集所有 display area 中的可见 Task。
4. 当前用户配置了 secure keyguard 时，允许 Home 进入临时 REAL 分支。
5. 捕获尝试结束后，在 `finally` 中调用 listener。

Controller 自己保证在 posted 工作的 `finally` 中回调 listener，所以回调发生在它的候选遍历之后；但 DisplayPowerController 调用 policy 的 `screenTurningOff(listener)` 后紧接着就解除 screen-off block，并不等待该回调。显示随后可能进入 OFF，`PhoneWindowManager.isScreenOn()` 也可能先变为 false，导致异步 REAL prepare 失败。因此 listener 不证明每一个候选成功，不证明物理熄屏等过截图，更不代表 Store 队列清空。

临时 Home 只缓存、不持久化、不通知；它还绕过常规 `getSnapshotMode()`。Home 被选作 snapshot starting-window 候选时，`addStartingWindow()` 会先从 running cache 移除它，再检查 direct-unlock flag；只有 flag 满足才实际展示，不满足也已经消费了缓存。因而 Home 仅设置 disable-preview 时，这条特例不会检查它，也没有 `FLAG_SECURE` 对应的 native 黑化兜底；只有窗口实际被标成 secure 时，SurfaceFlinger 的黑化才仍然有效。`allowSnapshotHome` 取 WMS 当前用户的 secure-keyguard 配置，不查询 profile 的独立 challenge。即使如此，REAL 的其他 prepare 门仍然存在。

这个 power handshake 也不覆盖所有“屏幕变黑”的物理原因。例如 DisplayPowerController 的 proximity 路径可以跳过常规 `screenTurningOff()` 握手，所以不能把 listener 当作每一次显示熄灭前都必经的全局栅栏。

把三条路径放在一起，就能避免常见误读：Binder get 是读取；Recents screenshot 是“主动抓后只读内存”；screen-off 是“批量抓，并可能临时允许 Home”。它们的权限、锁、持久化与安全门并不相同。

## 16. 用九个只读练习建立可复验的证据链

以下命令都固定在 `android-11.0.0_r48`，不修改工作区。每个练习先写出自己的预测，再对照源码；重点是区分调用入口、内存可见与磁盘可恢复，而不是只找同名方法。

### 练习 1：画出五类入口与产品开关覆盖范围

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '138,180p;410,430p;558,590p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '2590,2640p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/RecentsAnimationController.java | nl -ba | sed -n '175,215p;670,690p'
```

标出 `handleClosingApps`、Activity finish、两条 Recents 主动截图和 `screenTurningOff`。逐条回答谁调用 `shouldDisableSnapshots()`，谁在调用后无条件加入 skip。

### 练习 2：验证候选Task与失败后的skip窗口

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '130,225p;435,455p'
git -C frameworks/base show android-11.0.0_r48:services/tests/wmtests/src/com/android/server/wm/TaskSnapshotControllerTest.java | nl -ba | sed -n '60,130p'
```

列出 `getClosingTasks()` 的三个筛选条件，再找出 `snapshotTasks()` 为什么无法向调用者报告失败。构造“旧 cache 存在”和“不存在”两种结果。

### 练习 3：对照模式对象、真实捕获对象与隐私门

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '245,520p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityRecord.java | nl -ba | sed -n '4360,4390p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/WindowState.java | nl -ba | sed -n '1768,1786p'
```

分别记录 `getTopMostActivity()` 与 `findAppTokenForSnapshot()` 的用途。说明 disable-preview、`FLAG_SECURE` 和设备策略在哪一层把 REAL 改成 APP_THEME。

### 练习 4：追到native确认捕获子层、格式与secure黑化

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:core/java/android/view/SurfaceControl.java | nl -ba | sed -n '2105,2160p'
git -C frameworks/base show android-11.0.0_r48:core/jni/android_view_SurfaceControl.cpp | nl -ba | sed -n '317,373p'
git -C frameworks/native show android-11.0.0_r48:services/surfaceflinger/SurfaceFlinger.cpp | nl -ba | sed -n '5545,5600p;5695,5735p;5760,5800p;5828,5860p'
git -C frameworks/native show android-11.0.0_r48:services/surfaceflinger/BufferLayer.cpp | nl -ba | sed -n '175,190p'
```

确认三个结论：Task Surface 是层级根而非被绘制的根内容；Java 到 native 使用的实际格式；secure layer 面向非安全 render target 时的处理。

### 练习 5：比较REAL与APP_THEME的数据来源

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '270,520p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/app/ActivityManager.java | nl -ba | sed -n '2080,2240p;2250,2410p'
```

做一张字段表：buffer、taskSize、insets、rotation、orientation、pixelFormat 意图、real、low、translucent、ColorSpace。再找出哪些 getter 暴露可变对象。

### 练习 6：验证cache命中、磁盘miss与不回填

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotCache.java | nl -ba | sed -n '30,125p'
git -C frameworks/base show android-11.0.0_r48:services/tests/wmtests/src/com/android/server/wm/TaskSnapshotCacheTest.java | nl -ba | sed -n '90,135p'
```

指出哪张 map 被 `clearRunningCache()` 清空、磁盘 restore 在何处返回，以及为什么 restore 成功后再用 `restoreFromDisk=false` 仍可能 miss。

### 练习 7：模拟Store深度、暂停和locked-user轮转

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotPersister.java | nl -ba | sed -n '90,220p;261,371p;450,518p'
git -C frameworks/base show android-11.0.0_r48:services/tests/wmtests/src/com/android/server/wm/TaskSnapshotPersisterLoaderTest.java | nl -ba | sed -n '97,152p'
```

先模拟暂停时连续排入四个 Store，标出谁被淘汰；再模拟 locked-user Store 后跟 Delete，解释为何 Delete 不是阻止后续写回的栅栏。

### 练习 8：审计三文件事务与Loader重建损失

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotPersister.java | nl -ba | sed -n '50,115p;220,255p;350,518p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '120,130p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/os/Environment.java | nl -ba | sed -n '385,412p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotLoader.java | nl -ba | sed -n '85,205p'
git -C frameworks/base show android-11.0.0_r48:proto/src/task_snapshot.proto | nl -ba | sed -n '20,50p'
```

为 user 10、task 42 写出三个路径，指出哪一个使用 `AtomicFile`。列出 proto 没有保存的信息，并给出 Loader 可能看到混合代次的两个时刻。

### 练习 9：把Binder、Recents与screen-off完成点并排

```bash
cd /Users/ninebot/androidSource
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/ActivityTaskManagerService.java | nl -ba | sed -n '3608,3622p;4435,4463p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/RecentTasks.java | nl -ba | sed -n '382,402p'
git -C frameworks/base show android-11.0.0_r48:core/java/android/os/UserHandle.java | nl -ba | sed -n '160,180p'
git -C frameworks/base show android-11.0.0_r48:core/res/AndroidManifest.xml | nl -ba | sed -n '3825,3840p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/RecentsAnimationController.java | nl -ba | sed -n '175,215p;295,315p;600,700p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/wm/TaskSnapshotController.java | nl -ba | sed -n '175,230p;558,590p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/display/DisplayPowerController.java | nl -ba | sed -n '1248,1320p'
git -C frameworks/base show android-11.0.0_r48:services/core/java/com/android/server/policy/PhoneWindowManager.java | nl -ba | sed -n '4514,4522p;4571,4579p;4651,4654p'
```

为每条路径回答四个问题：需要什么权限、是否主动捕获、是否允许磁盘 restore、返回或回调时能证明哪个完成点。若答案中出现“已经落盘”，必须给出对应的同步证据。

最终可用一条不变量收束整章：**Controller 优先让当前进程尽快看到新快照，Persister 尽力让未来进程恢复它；二者之间没有跨缓存、队列、proto 与 JPEG 的原子提交。** 对本次新捕获，安全性由 APP_THEME 决策与 native 非安全目标对 secure layer 的黑化共同守住；旧快照何时失效是独立边界，不能靠通知时序、锁屏状态或“调用者是 system_server”来替代。
