# 243 Android触摸遮挡、Trusted Overlay与Tapjacking防护链

本文以 `android-11.0.0_r48` 为唯一源码基线，沿着 `InputDispatcher`、`MotionEvent`、`View/ViewGroup` 与 `WindowManagerService` 追一条完整的安全链。问题不是“屏幕上有没有浮窗”这么宽泛，而是：正常命中已经选出窗口之后，系统如何判断目标上方是否存在不可信窗口，如何把风险逐目标带到应用，以及应用拒绝事件与系统隐藏覆盖层分别在哪里真正生效。

阅读时始终分清四个动作：**命中目标、标记风险、拒绝触摸、隐藏窗口**。它们发生在不同对象、不同进程与不同时间点；把其中任意两个合并，都会得到错误的调试结论。

## 1. 主问题：能命中敏感 View，为什么仍不等于可信

Tapjacking 的典型前提是：用户看到的视觉提示与真正接收触摸的窗口不一致。一个位于上层的窗口可以因为 `FLAG_NOT_TOUCHABLE`、触摸区域留洞等原因不成为正常输入目标，却仍遮住或装饰下层按钮。于是“事件到达了正确 Activity”只能证明命中结果，不能证明用户看到的就是该 Activity。

Android 11 r48 给出的不是一个万能的“恶意浮窗判定器”，而是一组逐层收窄的事实：

| 层次 | 记录或动作 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| `InputDispatcher` 命中 | 选出接收窗口 | 这个窗口符合本次正常命中规则 | 目标上方没有别的窗口 |
| 遮挡扫描 | 写入 `OBSCURED` 或 `PARTIALLY_OBSCURED` | 某个符合条件的上层窗口在点或 frame 上构成遮挡 | 上层窗口一定恶意、属于哪个包 |
| `View` 安全过滤 | 对该事件返回拒绝 | 监听器和 `onTouchEvent()` 不处理这次事件 | 覆盖层已消失、Dispatcher 会重新命中 |
| WMS 隐藏策略 | 对特定覆盖窗口调用 hide/show 路径 | 特权安全页面可主动压低一类覆盖风险 | 所有窗口都被隐藏、Surface 已同步完成 |

因此，本章的核心判断式是：

> `正确目标` 与 `可信触摸` 是两项独立条件；风险标志只是证据，只有消费端策略或窗口管理策略执行后，防护才产生行为结果。

## 2. 两本账：正常命中与安全遮挡扫描

第一本账是“谁接收事件”。`findTouchedWindowAtLocked()` 按前到后的 Z 序扫描同一 display 的窗口，只在 `visible` 为真时继续；带 `FLAG_NOT_TOUCHABLE` 的窗口不能成为目标。这里的 touch-modal 指 `FLAG_NOT_FOCUSABLE` 与 `FLAG_NOT_TOUCH_MODAL` 两位都未设置；窗口要么满足这项 modal 条件，要么让 `touchableRegionContainsPoint(x, y)` 命中。找到第一个合格窗口便返回。

第二本账是“目标上方谁构成安全遮挡”。目标确定后，`isWindowObscuredAtPointLocked()` 与 `isWindowObscuredLocked()` 又从前到后扫描，但在遇到目标 handle 时停止。这里不重新执行正常命中的全部条件，而是先走 `canBeObscuredBy()` 的安全主体门，再分别检查上层窗口 frame 是否含触点、或两个 frame 是否严格相交。

这本安全账的候选不只来自拥有 InputChannel 的 WMS 窗口。r48 的 `BufferLayer::needsInputInfo()` 对所有非 cursor buffered layer 返回 true；没有显式 input info 的 Layer 会由 `fillInputInfo()` 合成 owner PID/UID、frame 和 `INPUT_FEATURE_NO_INPUT_CHANNEL`。InputDispatcher 更新窗口 handle 时，正是这个 feature 让“没有注册 input channel”的条目继续留在 Z 序中。它的 touchable region 为空，不能成为正常目标，却能参与 PID-based occlusion detection。这堵住了“只创建可见 Layer、不创建输入通道，就逃过遮挡标记”的缺口。

两本账的关键差异如下：

| 问题 | 正常命中 | 安全遮挡 |
|---|---|---|
| 扫描目的 | 选择输入目标 | 给已选目标增加风险位 |
| 触摸区域 | 会读取 `touchableRegion` | 不读取上层 `touchableRegion` |
| `NOT_TOUCHABLE` | 会跳过该窗口 | 不作为排除条件 |
| touch-modal | 参与命中 | 不参与遮挡几何 |
| 几何对象 | 点与 touchable region，或 modal | 点与上层 frame；上层与目标 frame |
| 无 InputChannel 的 buffered layer | 不能形成可派发目标 | 可作为 occlusion-only 候选保留 |
| 终止位置 | 第一个合格目标 | 精确目标 handle；其下方不再检查 |

这解释了最常见的反直觉现象：上层窗口可以让触摸“穿过去”，同时让下层窗口收到 `FLAG_WINDOW_IS_OBSCURED`。

## 3. 威胁模型与完成点：标记、拒绝、隐藏不是一件事

先固定三个角色：敏感目标窗口 `T`、位于其上的候选输入条目 `O`、最终接收回调的敏感 `View V`。`O` 既可能对应常规窗口，也可能是 SurfaceFlinger 为无 InputChannel 的 BufferLayer 合成的 occlusion-only 条目。本章只讨论已经安装进 InputDispatcher 的 `InputWindowInfo` 快照，不把 `visible` 等同于“可触摸”，也不从视觉截图倒推输入事实。

一次防护链有多级可观测完成点：

1. `T` 被正常命中，O 通过主体门与几何判断，风险位先写在临时 `TouchState`。
2. 注入权限检查通过后，临时状态才可提交；`INPUT_EVENT_INJECTION_SUCCEEDED` 表示找到输出候选，不等于已经投递。
3. `addWindowTargetLocked()` 按已注册 channel 产出 `InputTarget`；channel 已注销时会直接跳过。
4. connection 必须为 `NORMAL`，且 `trackMotion()` 接受 action 序列，目标化的 `DispatchEntry` 才进入 outbound queue。
5. `publishMotionEvent()` 成功后，该 entry 才从 outbound 移到 wait queue；pipe 满或其他错误都不能算发布完成。
6. 客户端拿到 `MotionEvent` 后，`View` 安全门或业务监听器才作出拒绝/消费；应用回报 FINISHED 又是更晚的完成点。
7. 独立的 WMS 路径还要满足授权、请求窗口 Surface shown、全局成员表翻转与目标类型门，才进入 hide/show 策略。

因此，“完成”必须注明对象与队列。日志里计算出 `OBSCURED`，甚至目标选择返回 succeeded，都不能证明应用已收到或 View 已拒绝；设置隐藏标志也不能证明目标 Surface 和新的 InputWindow 快照已经同步完成。

## 4. 遮挡标志在哪次目标建立时计算

`findTouchedWindowTargetsLocked()` 先把旧 `TouchState` 复制到临时状态。遇到新手势，或 split 手势中的新 `POINTER_DOWN`，它才为相应指针寻找目标。对普通新目标，`targetFlags` 先含 `FOREGROUND | DISPATCH_AS_IS`，split 时再含 `SPLIT`，然后执行这轮二选一：

- 点被合格上层 frame 覆盖：加入 `FLAG_WINDOW_IS_OBSCURED`；
- 否则，只要合格上层 frame 与目标 frame 相交：加入 `FLAG_WINDOW_IS_PARTIALLY_OBSCURED`；
- 两者都不成立：本轮不增加遮挡位。

这里的 `if / else-if` 只保证**这一次普通目标建账**不会同时新增两位，不保证一个窗口在整条流中永远只有一位。后续 split 指针再次命中同一窗口时，`TouchState::addOrUpdateWindow()` 会对 `targetFlags` 做按位 OR；wallpaper 路径更会直接强制两位。

### 练习 1：区分命中、标记、拒绝与隐藏

固定 `T` 为 O 之后的第一个正常命中目标；T 未 paused、注册 channel、connection 为 `NORMAL` 且 responsive，注入许可通过、action 序列一致、publish 成功。`O` 位于其上，`visible=true`、`FLAG_NOT_TOUCHABLE`、与 T 不同 token 和 PID、同 display、非 trusted，且 `O.frame` 包含触点。V 已 enabled 且不在拖动滚动条，分别令 V 未开启过滤与开启 `filterTouchesWhenObscured`。填写唯一结果：

| 状态 | 正常目标 | `T` 的风险位 | 是否送到 `T` 的窗口连接 | 是否越过 V 的安全门 | `O` 是否因此消失 |
|---|---|---|---|---|---|
| 未开启过滤 | `T` | `OBSCURED` | 是 | 是 | 否 |
| 开启默认过滤 | `T` | `OBSCURED` | 是 | 否 | 否 |

注意，View 返回拒绝不是 Dispatcher 的二次命中信号，也不会自动隐藏 `O` 或给另一个进程补发事件。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'Traverse windows from front to back to find touched window.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!(flags & InputWindowInfo::FLAG_NOT_TOUCHABLE))' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowInfo->touchableRegionContainsPoint(x, y)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (isWindowObscuredAtPointLocked(newTouchedWindowHandle, x, y))' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'targetFlags |= InputTarget::FLAG_WINDOW_IS_OBSCURED;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'public boolean onFilterTouchEventForSecurity(MotionEvent event)' frameworks/base/core/java/android/view/View.java
grep -n -F 'Window is obscured, drop this touch.' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (onFilterTouchEventForSecurity(event))' frameworks/base/core/java/android/view/View.java
```

## 5. canBeObscuredBy：五道安全主体门

`canBeObscuredBy(T, O)` 不判断几何，只回答 `O` 是否有资格被当作 `T` 的安全遮挡者。五道门按源码顺序执行：

| 次序 | 返回 false 的条件 | 含义 |
|---|---|---|
| 1 | `haveSameToken(T, O)` | 同 token 的 cloned layer 不重复构成安全边界 |
| 2 | `O.visible == false` | 未在该输入快照中可见 |
| 3 | `T.ownerPid == O.ownerPid` | 同进程内没有这里要标记的跨进程边界 |
| 4 | `O.isTrustedOverlay()` | 该窗口类型属于 r48 的可信覆盖白名单 |
| 5 | `O.displayId != T.displayId` | 不跨 display 计算遮挡 |

两个容易写错的细节是：第一道门比较 token，不是 Java 对象名；第三道门比较 PID，不是 UID。因此，同一 UID 的两个不同 PID 仍可能形成遮挡。只有五道门全部通过后，调用方才检查 frame。

扫描顺序同样属于结论的一部分。窗口向量是前到后；一旦迭代到 `T` 的 exact handle 就 `break`，所以 `T` 下方即使 frame 覆盖触点，也不是 `T` 的遮挡者。

### 练习 2：逐项执行 canBeObscuredBy

给 `T(pid=20, uid=10020, token=t, display=0)` 上方依次放六个窗口。所有候选默认 `pid=30, uid=10020, token` 与 t 不同、`visible=true`、non-trusted、`display=0`，frame 都含触点；每个候选只覆盖下列一个字段：A 改为同 token，B 改为 `visible=false`，C 改为 `pid=20`，D 改为 trusted 类型，E 改为 `display=1`，F 不作覆盖。

唯一结果是 A—E 分别在对应门第一次返回 false，只有 F 返回 true；若扫描随后遇到 `T`，其下方窗口一律不再看。F 明确与 T 同 UID、不同 PID，不能把“同 UID”误当成同进程豁免。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static bool canBeObscuredBy' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (haveSameToken(windowHandle, otherHandle))' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!otherInfo->visible)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'info->ownerPid == otherInfo->ownerPid' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'otherInfo->isTrustedOverlay()' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'otherInfo->displayId != info->displayId' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (windowHandle == otherHandle)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'break; // All future windows are below us. Exit early.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 6. Trusted Overlay：r48 类型列表与权限边界

`InputWindowInfo::isTrustedOverlay()` 在 r48 不是一个运行时授予应用的通用能力，而是按 `layoutParamsType` 硬编码判断。完整的十二类是：

| 分组 | 类型 |
|---|---|
| 输入与辅助 | `TYPE_INPUT_METHOD`、`TYPE_INPUT_METHOD_DIALOG`、`TYPE_ACCESSIBILITY_OVERLAY` |
| 系统栏与面板 | `TYPE_STATUS_BAR`、`TYPE_NOTIFICATION_SHADE`、`TYPE_NAVIGATION_BAR`、`TYPE_NAVIGATION_BAR_PANEL` |
| 系统覆盖 | `TYPE_MAGNIFICATION_OVERLAY`、`TYPE_SECURE_SYSTEM_OVERLAY`、`TYPE_INPUT_CONSUMER`、`TYPE_TRUSTED_APPLICATION_OVERLAY` |
| 布局设施 | `TYPE_DOCK_DIVIDER` |

普通 `TYPE_APPLICATION_OVERLAY` 不在列表中；它即使能够显示，也不因此获得遮挡豁免。源码注释还提示，这套按类型判断计划在后续演进中由 `trustedOverlay` 字段取代，但 r48 的实际判断仍必须按上述函数阅读。

类型分类也不等于创建权限。`TYPE_TRUSTED_APPLICATION_OVERLAY` 是标注 `@hide` 的非公开系统窗口类型，`DisplayPolicy` 对它执行 `android.permission.INTERNAL_SYSTEM_WINDOW` 检查；清单中该权限为 `signature`。r48 没有一个名为 `TRUSTED_APPLICATION_OVERLAY` 的权限。换言之，先要有资格创建这种窗口，它才会在 InputDispatcher 的类型白名单中免于成为遮挡者。

### 练习 3：判断类型与创建权限

固定四个候选：普通应用创建 `TYPE_APPLICATION_OVERLAY`；系统 IME 创建 `TYPE_INPUT_METHOD`；持有 `INTERNAL_SYSTEM_WINDOW` 的系统组件创建 `TYPE_TRUSTED_APPLICATION_OVERLAY`；不持有该权限的普通进程请求同一 trusted 类型。

| 候选 | `isTrustedOverlay()` 分类 | 是否由本节的 `INTERNAL_SYSTEM_WINDOW` 分支决定 |
|---|---|---|
| 普通 application overlay | false | 否；其自身的权限、AppOp 与窗口策略另行决定 |
| IME | true | 否；由 IME 对应的系统策略另行决定 |
| 有内部权限的 trusted application overlay | true | 是；进入该分支并通过检查 |
| 无内部权限却请求 trusted application overlay | 若只看类型值会是 true | 是；进入该分支即被拒绝，不能进入有效窗口快照 |

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'bool InputWindowInfo::isTrustedOverlay() const' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'layoutParamsType == TYPE_INPUT_METHOD' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'layoutParamsType == TYPE_NOTIFICATION_SHADE' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'layoutParamsType == TYPE_ACCESSIBILITY_OVERLAY' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'layoutParamsType == TYPE_TRUSTED_APPLICATION_OVERLAY;' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'TYPE_APPLICATION_OVERLAY = FIRST_SYSTEM_WINDOW + 38' frameworks/base/core/java/android/view/WindowManager.java
grep -n -F 'TYPE_TRUSTED_APPLICATION_OVERLAY = FIRST_SYSTEM_WINDOW + 42' frameworks/base/core/java/android/view/WindowManager.java
grep -n -F 'case TYPE_TRUSTED_APPLICATION_OVERLAY:' frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java
grep -n -F 'android.Manifest.permission.INTERNAL_SYSTEM_WINDOW' frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java
grep -n -F '<permission android:name="android.permission.INTERNAL_SYSTEM_WINDOW"' frameworks/base/core/res/AndroidManifest.xml
```

## 7. 两种几何：触点落入 frame 与两个 frame 相交

通过主体门后，直接遮挡调用 `frameContainsPoint(x, y)`。它使用半开矩形：`left <= x < right` 且 `top <= y < bottom`。右边界和下边界不属于窗口，左边界和上边界属于窗口。

部分遮挡调用 `overlaps(other)`。四个不等式都是严格的：左右、上下都必须存在正面积交集。两个 frame 仅在边上或角上相接，不算 overlap。

这个 frame 不是简单照抄 WMS 的逻辑 frame。SurfaceFlinger 的 `Layer::fillInputInfo()` 对普通非 portal Layer 先取 buffer size，无效时回退 cropped buffer size；portal 则取 touchable region bounds。随后应用 Layer transform，并把随缩放调整且限制到半个边长以内的 `surfaceInset` 从四边扣除，最终重写 `frameLeft/Top/Right/Bottom`。后面的 touchable-region crop、replace 与 clone-root 裁剪只改变 Region，不回头改变遮挡 frame。调试几何时应抓最终 `InputWindowInfo.frame`，不能拿 WMS frame 或裁过的 touchable region 代替。

固定目标 `T=[0,0,100,100)`、上层 `O=[20,20,60,60)`：

| 触点 | `O.frameContainsPoint` | `O.overlaps(T)` | 普通新目标本轮结果 |
|---|---:|---:|---|
| `(20,20)` | true | true | `OBSCURED` |
| `(59,59)` | true | true | `OBSCURED` |
| `(60,40)` | false | true | `PARTIALLY_OBSCURED` |
| `(10,10)` | false | true | `PARTIALLY_OBSCURED` |

若把 `O` 改成 `[100,0,130,30)`，它只贴住 `T` 的右边界：含点检查对 `T` 内触点为 false，严格相交也为 false，因此不增加任何遮挡位。

### 练习 4：手算半开边界与严格相交

不运行 UI，只按上表的固定坐标重算四个点，再重算贴边矩形。答案必须同时给出两个布尔值，不能只凭示意图说“看起来覆盖”。最后说明：`(60,40)` 得 partial，不是因为它在目标 frame 内，而是因为触点未落入 `O`、同时两个 frame 仍有正面积交集。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'bool InputWindowInfo::frameContainsPoint(int32_t x, int32_t y) const' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'return x >= frameLeft && x < frameRight' frameworks/native/libs/input/InputWindow.cpp
grep -n -F '&& y >= frameTop && y < frameBottom;' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'bool InputWindowInfo::overlaps(const InputWindowInfo* other) const' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'return frameLeft < other->frameRight && frameRight > other->frameLeft' frameworks/native/libs/input/InputWindow.cpp
grep -n -F '&& frameTop < other->frameBottom && frameBottom > other->frameTop;' frameworks/native/libs/input/InputWindow.cpp
grep -n -F 'otherInfo->frameContainsPoint(x, y)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'otherInfo->overlaps(windowInfo)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'Rect layerBounds = info.portalToDisplayId == ADISPLAY_ID_NONE' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'layerBounds = t.transform(layerBounds);' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.frameLeft = layerBounds.left;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.touchableRegion = info.touchableRegion.intersect' frameworks/native/services/surfaceflinger/Layer.cpp
```

## 8. OBSCURED 与 PARTIAL：普通新目标为何采用 if / else-if

先统一本章短语：direct 只表示 `OBSCURED` 位存在；partial-only 表示没有 `OBSCURED`、但有 `PARTIALLY_OBSCURED`，两种短语本身都不证明位的来源。对这轮由目标扫描产生的两位而言，`FLAG_WINDOW_IS_OBSCURED` 值为 `0x1`，说明触点本身落在合格上层 frame；`FLAG_WINDOW_IS_PARTIALLY_OBSCURED` 值为 `0x2`，说明触点未被那类 frame 盖住，但目标窗口的 frame 与某个合格上层 frame 相交。

普通建账先查点遮挡，再以 `else if` 查 frame 相交，原因不是两个事实在数学上互斥——点在上层 frame 时，两个 frame 往往也相交——而是将这一轮结果归入更强的直接遮挡。于是读取标志时应这样解释：

| 位组合 | 普通首次建账的常见来源 | 仍需考虑的特殊来源 |
|---|---|---|
| `00` | 没有合格上层遮挡 | 也可能 trusted、同 PID 等被豁免 |
| `01` | 触点被盖住 | slippery enter 只算这一位 |
| `10` | 仅 frame 部分重叠 | split 后可能继续 OR |
| `11` | 不来自单轮普通 `if / else-if` | wallpaper 强制；或多次 split 更新同一窗口 |

target-derived 标志没有携带遮挡窗口的 token、PID、UID、包名或 frame。应用可以据此收紧一次敏感操作，却不能仅凭两位准确归因某个 overlay；§12 还会说明原始事件自带 flags 的来源边界。

## 9. 遮挡扫描刻意不看的字段：touchable、focus 与 alpha

正常命中和安全遮挡的字段集合故意不同。给定一个已安装到 InputDispatcher 的上层条目，且 `visible=true`，`canBeObscuredBy()` 与两个几何函数没有读取以下字段：

- 上层窗口的 `touchableRegion` 是否为空、是否在触点处留洞；
- `FLAG_NOT_TOUCHABLE` 与 `FLAG_NOT_FOCUSABLE`；
- 窗口整体 alpha、像素级透明度或可见图形的不规则洞；
- 多个半透明窗口合成后的总不透明度。

因此不能用“点没被上层接收”推出“点没被安全遮挡”，也不能用截图中某像素透明推出 flag 必为零。反过来，若快照的 `visible=false`，它会在主体门被排除。这里还要分清 `visible` 的两种生产方式：已有 input info 的 Layer 使用 `canReceiveInput()`，r48 中它等于 `!isHiddenByPolicy()`，刻意不要求已有 buffer 或 alpha 大于零；没有 input info 的 occlusion-only BufferLayer 才使用真实 `isVisible()`，要求未被 policy 隐藏、alpha 大于零，且已有 buffer 或 sideband stream。两者都没有扣除“被更高层覆盖后”的实际可见像素区域。

### 练习 5：透明、不可触摸与安全遮挡

场景 A 固定一个已有 input info 的 `O`：快照 `visible=true`、空 `touchableRegion`、`NOT_TOUCHABLE | NOT_FOCUSABLE`、alpha 为 `0.2`，并令其 frame 包含触点；O 与 T 跨 PID、同 display、不同 token、非 trusted。结果是正常命中跳过 O 而选到 T，安全扫描给 T 标 `OBSCURED`。只把输入快照改成 `visible=false` 后，O 才在第二道主体门被排除。

场景 B 继承场景 A 的 T 命中条件、同 display、不同 token/PID、non-trusted 与 frame 含点条件，只把 O 的来源改成没有 input info 的非 cursor BufferLayer，并固定它未被 policy 隐藏。SurfaceFlinger 为其合成 `NO_INPUT_CHANNEL` 条目；alpha 大于零且已有 buffer 时，真实 `isVisible()` 为 true，空触摸区域使它不成为目标，而安全扫描确定给 T 标 `OBSCURED`。alpha 变为零或 buffer/sideband 都不存在时，合成快照的 visible 为 false，不再构成遮挡。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int32_t flags = windowInfo->layoutParamsFlags;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (windowInfo->visible)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputWindowInfo::FLAG_NOT_FOCUSABLE' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputWindowInfo::FLAG_NOT_TOUCH_MODAL' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowInfo->touchableRegionContainsPoint(x, y)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!otherInfo->visible)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'otherInfo->frameContainsPoint(x, y)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'otherInfo->overlaps(windowInfo)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool visible = false;' frameworks/native/include/input/InputWindow.h
grep -n -F 'bool needsInputInfo() const override { return !mPotentialCursor; }' frameworks/native/services/surfaceflinger/BufferLayer.h
grep -n -F 'InputWindowInfo Layer::fillInputInfo()' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'InputWindowInfo::INPUT_FEATURE_NO_INPUT_CHANNEL;' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'info.visible = hasInputInfo() ? canReceiveInput() : isVisible();' frameworks/native/services/surfaceflinger/Layer.cpp
grep -n -F 'return !isHiddenByPolicy() && getAlpha() > 0.0f &&' frameworks/native/services/surfaceflinger/BufferLayer.cpp
grep -n -F 'info->inputFeatures & InputWindowInfo::INPUT_FEATURE_NO_INPUT_CHANNEL' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 10. 流内生命周期：普通延续、split、slippery 与 transfer

遮挡位首先属于 `TouchState` 中的 `TouchedWindow.targetFlags`，不是每个 MOVE 都重新查询窗口栈。普通 MOVE、UP 等进入 Case 2，通常沿用从旧状态复制来的窗口和位；在手势进行中移动、显示或隐藏 overlay，并不会自动回写既有目标的遮挡位。下一次普通 DOWN 会重新建账。

四条路径必须分开：

| 路径 | 是否重新找目标 | 遮挡处理 |
|---|---|---|
| 普通流内 MOVE/UP | 否 | 沿用既有 `TouchedWindow.targetFlags` |
| split `POINTER_DOWN` | 为新指针重新 hit-test | 无合格新窗或新窗不支持 split 时回退第一前景窗；按新指针坐标重算 direct/partial，并用 OR 合并 |
| slippery 单指 MOVE 换窗 | 是 | 新窗口只调用点遮挡检查，没有 partial 分支 |
| `transferTouchFocus(T,D)` | 按 token 迁移，不做坐标命中 | 从 T 取出的 flags 只保留 `FOREGROUND | SPLIT | DISPATCH_AS_IS`；D 已有的自身位仍可保留，不做几何重算 |

`filterNonAsIsTouchWindows()` 在本轮输出目标之后删除不再按原样接收的窗口，例如 slippery exit；对仍为 AS_IS 或 slippery enter 的保留目标，它只归一化 dispatch mode，不清掉遮挡位。这正是普通流内事件继续携带旧风险结论的机制之一。

slippery 也有独立准入边界：只有单指 MOVE 且当前状态恰有一个 slippery 前景窗口时才尝试，且新旧窗口都存在并不相同才切换。旧窗口带既有安全位输出 CANCEL，新窗口输出 DOWN，但新窗口只计算点遮挡。这段没有重复 Case 1 对 paused、connection 存在与 responsive 的前置检查；新窗口仍要通过稍后的注入权限检查，无注册 channel 或非 `NORMAL` connection 则分别在更晚的目标生成、派发准备阶段被跳过。

transfer 要同时看两本流内账。`TouchState` 侧不会把 T 的遮挡位迁给 D；若 D 原本已在同一状态中，`addOrUpdateWindow()` 会保留 D 自己已有的位，若 D 是新加入项则其 targetFlags 没有两种遮挡位。`InputState` 侧却另存 connection 的 motion memento：初始 DOWN 会保存当时的 `resolvedFlags`。两个 connection 都存在时，旧连接的合成 CANCEL 可复用 T memento 的 flags；新连接若没有同 device/source/display 的 memento，会复制 T memento，合成 DOWN 也可带旧位；若 D 已有匹配 memento，则保留 D memento 的 flags，只追加迁入指针并合成 POINTER_DOWN。以上都没有重新执行几何扫描。后续物理事件才重新用 D 当前 targetFlags 与该 `MotionEntry.flags` 产生 resolvedFlags。

### 练习 6：比较四条生命周期路径

下面是四个互不相承的场景；相关 handle 与 channel 都存在，connection 均为 `NORMAL` 且 responsive，注入许可通过、`trackMotion()` 接受、publish 成功。普通延续场景固定初始 T 为 partial，MOVE 前 overlay 移走。split 场景固定旧状态 `split=true`、T 未 paused 且已有 obscured，新指针没有命中其他可 split 窗口，回退 T 后按新坐标算出 partial。slippery 场景固定单指 MOVE、恰有一个 slippery foreground，旧窗与 S 均存在且不同，S 触点被盖住。transfer 场景固定调用成功且两个 connection 都存在，把不同 token、同 display 的 T 迁移到此前不在该流中的 D；T 的 memento 保存 partial，D 没有匹配 memento，后续物理 MOVE 的原始 flags 固定为 0。

| 路径 | 唯一结果 |
|---|---|
| T 的普通 MOVE | 仍沿用 partial，不因 overlay 已移走而重算 |
| split 新指针回退 T | 使用新指针坐标重算 partial，和 T 既有 obscured OR 后为两位 |
| slippery 进入 S | 旧窗携原位收到 CANCEL；S 携 obscured 收到 DOWN，该路径不产生 partial-only |
| T → D transfer | D 的新 TouchState targetFlags 无两位且不重算；旧 CANCEL 与新合成 DOWN 均复用 partial，随后原始 flags 为 0 的物理 MOVE 不带两位 |

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'tempTouchState.copyFrom(*oldState);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newTouchedWindowHandle = tempTouchState.getFirstForegroundWindowHandle();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'touchedWindow.targetFlags |= targetFlags;' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'if (maskedAction == AMOTION_EVENT_ACTION_MOVE && entry.pointerCount == 1' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_SLIPPERY_ENTER' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool InputDispatcher::transferTouchFocus' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'int32_t newTargetFlags = oldTargetFlags &' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_FOREGROUND | InputTarget::FLAG_SPLIT |' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizeCancelationEventsForConnectionLocked(fromConnection, options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizePointerDownEventsForConnectionLocked(toConnection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'memento.flags = flags;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.mergePointerStateTo(otherMemento);' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'other.mMotionMementos.push_back(memento);' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

## 11. Wallpaper 例外与 per-target targetFlags

首次 `ACTION_DOWN` 命中的前景窗口若声明 `hasWallpaper`，Dispatcher 会在同 display 找到 `TYPE_WALLPAPER`，并把它作为额外 target 加入临时触摸状态。这里没有运行前述两次遮挡扫描，而是直接写入：

`WINDOW_IS_OBSCURED | WINDOW_IS_PARTIALLY_OBSCURED | DISPATCH_AS_IS`。

循环会选择该 display 的所有 wallpaper 类型条目，且这段本身没有再检查 wallpaper 的 `visible`、paused、touchable 或 responsive；wallpaper 又是在前景窗口注入权限检查之后加入，所以没有逐个 wallpaper 重做这项检查。它没有 `FOREGROUND | SPLIT`，将接收未按 pointerIds 拆分的流。不过它随后仍要经过 `addWindowTargetLocked()` 的注册 channel 门；没有 channel 的 wallpaper 不能形成可派发的 `InputTarget`。wallpaper 只在首个 DOWN 锁定，slippery 换窗不会换一批，中途出现的新 wallpaper 也不会加入。

另两个目标族不要混入这套计算：OUTSIDE 窗口只在首个 DOWN 扫描中加入，目标生成位只有 `DISPATCH_AS_OUTSIDE`，跨 UID 时可再加 `ZERO_COORDS`，本轮输出后便从 TouchState 过滤，不接后续 MOVE/UP；monitor 由 `addMonitoringTargetLocked()` 只设置 `DISPATCH_AS_IS`。它们都不会继承前景 target 计算出的两种位，但若原始 `MotionEntry.flags` 自带这些位，后续 `resolvedFlags` 的复制规则仍然适用。已锁定的 wallpaper 和 monitor 在 slippery 转移时继续接收那份原始 MOVE。

这证明 `11` 是合法状态，也再次说明 flag 是 **per-target** 的调度账，而不是整个原始 `MotionEntry` 的唯一全局属性。同一物理输入可以面对前景窗口 F、wallpaper P、OUTSIDE 观察窗口等多个连接目标；各自的 `InputTarget.flags` 不同，之后生成的 `DispatchEntry.resolvedFlags` 也可不同。

这里的 target 身份最终按 connection token 收敛，不是“每个 handle 必有一份”。`addWindowTargetLocked()` 会用 `inputChannel->getConnectionToken()` 查找已有 `InputTarget`；同 token 的多个 handle 命中已有项时不会再创建一项，并断言 flags 与 scale 相同。诊断时既要保留遮挡扫描遇到 exact handle 才停止的语义，也要在派发输出端按 connection token 看合并结果。

对 split 流也是一样。`TouchedWindow` 以 handle 聚合指针集合与 flags；新 pointer 命中同一 handle 时，flags 和 pointerIds 都按位 OR。因而一个先 direct、后 partial 的窗口最终可以持有两位，虽然每次普通计算只有一位。

## 12. 从 resolvedFlags 到 MotionEvent 与 Verified 边界

`enqueueDispatchEntryLocked()` 面向某个 connection 和 `InputTarget` 创建 `DispatchEntry`。这里的 resolved action 是按具体目标与 dispatch mode 改写后的 action；resolved flags 则先取 `motionEntry.flags`，再根据该目标的内部两位分别 OR 入公开的 `AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED` 与 `...PARTIALLY_OBSCURED`。`publishMotionEvent()` 发布的是 `dispatchEntry->resolvedFlags`，不是未经目标化的原始值。

公开 Java 值为 `0x1` 与 `0x2`。`VERIFIED_MOTION_EVENT_FLAGS` 也只包含这两项安全位；HMAC 是基于哈希的消息认证码，但这里还有 action 边界：`getSignature()` 只有在 resolved action 为 DOWN 或 UP 时签名，其他 action 返回 `INVALID_HMAC`。slippery enter 的 resolved action 是 DOWN，所以应按目标化后的 action 判断，不应只看原始 action 名称。

这个规则会让同一原始事件在不同目标上得到不同签名结论：split 新指针进入第二个窗口时，旧窗口可见的是 MOVE，新窗口可见的是 DOWN，只有后者签名；若同一 split 窗口拥有原事件全部 pointers，它不发生实体拆分，仍看到 POINTER_DOWN，因此不签名；slippery MOVE 对旧窗解析成 CANCEL、对新窗解析成 DOWN，只有新窗签名，而 wallpaper 与 monitor 仍看到原 MOVE。

`InputManager.verifyInputEvent()` 成功时返回的是输入事件的可验证子集。`VerifiedMotionEvent.getFlag()` 对这两位返回 true/false，对其他 flag 返回 null。成功验证说明这份子集与 Dispatcher 签名一致，不等于证明设备一定来自某块物理触摸屏，也不提供遮挡者身份；普通 MOVE 没有有效 HMAC，验证返回 null 是正常边界之一。

还要区分“位被签名”与“位由本轮几何扫描产生”。`resolvedFlags` 先复制 `motionEntry.flags`；软件注入路径构造 `MotionEntry` 时又原样读取调用方 `MotionEvent.getFlags()`，随后才 OR 当前 target 的内部位。因此，即使 DOWN/UP 的两位通过 HMAC 校验，也只能证明 Dispatcher 签署了最终 resolved 子集，不能倒推出这两位必然来自本次 `isWindowObscured...()` 扫描。跨 UID 注入仍受 Dispatcher 的 injection permission 检查，这个来源边界不等于普通应用可以任意向别的应用伪造触摸。split 与 slippery 还可能把同一原始 action 为 POINTER_DOWN 或 MOVE 的事件，对不同连接改写成不同 resolved action，于是一个目标被签名、另一个目标不被签名。

### 练习 7：手算 per-target flags 与签名边界

令原始 `MotionEntry.flags=0`，前景 F 的 target 为 obscured，wallpaper P 被强制为两位。分别构造 resolved action 为 DOWN、MOVE、UP 的派发项：

| 目标 | `resolvedFlags` | DOWN HMAC | MOVE HMAC | UP HMAC |
|---|---|---|---|---|
| F | `0x1` | 有效 | 无效 | 有效 |
| P | `0x3` | 有效 | 无效 | 有效 |

对成功验证的 DOWN/UP，`getFlag(0x1)`、`getFlag(0x2)` 返回布尔；请求不在允许集合中的其他位返回 null。MOVE 不能用该接口证明这两位。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'dispatchEntry->resolvedFlags = motionEntry.flags;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->targetFlags & InputTarget::FLAG_WINDOW_IS_OBSCURED' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedFlags,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'constexpr int32_t VERIFIED_MOTION_EVENT_FLAGS' frameworks/native/include/input/Input.h
grep -n -F 'AMOTION_EVENT_ACTION_UP) || (actionMasked == AMOTION_EVENT_ACTION_DOWN)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return INVALID_HMAC;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'public @Nullable Boolean getFlag(int flag)' frameworks/base/core/java/android/view/VerifiedMotionEvent.java
grep -n -F 'case FLAG_WINDOW_IS_PARTIALLY_OBSCURED:' frameworks/base/core/java/android/view/VerifiedMotionEvent.java
grep -n -F 'public @Nullable VerifiedInputEvent verifyInputEvent' frameworks/base/core/java/android/hardware/input/InputManager.java
grep -n -F 'policyFlags, action, actionButton, motionEvent->getFlags(),' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'target.flags = InputTarget::FLAG_DISPATCH_AS_IS;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'already unregistered input channel' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->status != Connection::STATUS_NORMAL)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!connection->inputState.trackMotion' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_back' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (status == WOULD_BLOCK)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.push_back(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 13. View 安全过滤顺序：默认为何只拒 OBSCURED

`View.dispatchTouchEvent()` 在滚动条处理、`OnTouchListener` 与 `onTouchEvent()` 之前调用 `onFilterTouchEventForSecurity()`。`ViewGroup.dispatchTouchEvent()` 同样先过这道门，然后才处理 DOWN 的旧状态清理、intercept 与 child 路由。根 ViewGroup 在安全门返回 false 时，当前事件不会进入这些分支，也不会发给子 View。

默认实现只有同时满足两项才拒绝：View 的 `FILTER_TOUCHES_WHEN_OBSCURED` 位已开，并且事件含 `FLAG_WINDOW_IS_OBSCURED`。启用方式是 Java 的 `setFilterTouchesWhenObscured(true)` 或对应 XML 属性。默认实现没有检查 `FLAG_WINDOW_IS_PARTIALLY_OBSCURED`，因此 partial-only 不会被这道默认安全门拒绝；随后是否调用 listener 或 `onTouchEvent()`，仍由 enabled、滚动条处理与 listener 返回值等普通 dispatch 条件决定。

返回 false 的作用域必须准确：它让当前 View/ViewGroup 的这次 `dispatchTouchEvent()` 返回未处理，并跳过内部回调；它不会请求 Dispatcher 再选另一个窗口，不会隐藏 overlay，也不会凭空向其他进程合成 CANCEL。若只给某个子按钮开启过滤，上层父容器与其他敏感控件并不会自动继承一套页面级策略。

### 练习 8：追踪 View 回调顺序

固定输入均为 `ACTION_DOWN`；Button 已 enabled、开启默认过滤、不在拖动滚动条，listener 已安装，listener 与 `onTouchEvent()` 各有计数器。题中的严格 override 固定为 `flags & (0x1 | 0x2)` 非零时返回 false：

| 输入与实现 | listener | `onTouchEvent()` | 结果 |
|---|---:|---:|---|
| obscured + 默认过滤 | 0 | 0 | 安全门拒绝 |
| partial-only + 默认过滤，listener 返回 true | 1 | 0 | listener 消费 |
| partial-only + 严格 override | 0 | 0 | override 拒绝 |

再把安全门放到根 ViewGroup：若它拒绝，DOWN reset、intercept 与 child dispatch 都不执行。此结果仍不触发窗口重命中、覆盖层隐藏或跨进程补发。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public boolean dispatchTouchEvent(MotionEvent event)' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (onFilterTouchEventForSecurity(event))' frameworks/base/core/java/android/view/View.java
grep -n -F 'li.mOnTouchListener.onTouch(this, event)' frameworks/base/core/java/android/view/View.java
grep -n -F 'if (!result && onTouchEvent(event))' frameworks/base/core/java/android/view/View.java
grep -n -F 'public boolean dispatchTouchEvent(MotionEvent ev)' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'if (onFilterTouchEventForSecurity(ev))' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'cancelAndClearTouchTargets(ev);' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'final boolean intercepted;' frameworks/base/core/java/android/view/ViewGroup.java
grep -n -F 'FILTER_TOUCHES_WHEN_OBSCURED) != 0' frameworks/base/core/java/android/view/View.java
grep -n -F 'setFilterTouchesWhenObscured(boolean enabled)' frameworks/base/core/java/android/view/View.java
```

## 14. 严格页面策略：整条流拒绝、业务复核与用户反馈

高价值确认页通常不能把 partial-only 当作无风险。页面可以在共同祖先 ViewGroup 覆写安全过滤，对 `flags & (OBSCURED | PARTIALLY_OBSCURED)` 非零时统一返回 false；或者给每个敏感入口安装监听器，但监听器必须对可疑流持续返回 true，确保事件不会继续落入该 View 的 `onTouchEvent()`。

SystemUI 的 `WifiDebuggingActivity` 展示了后一种模式：肯定按钮的 `OnTouchListener` 同时检查两位，命中时消费事件，并在 UP 给出 Toast。这里“返回 true”非常关键；如果监听器只记录日志却返回 false，`View.dispatchTouchEvent()` 仍可能继续调用 `onTouchEvent()`。`MediaProjectionPermissionActivity` 则组合了两层措施：肯定按钮开启默认 direct 过滤，窗口再请求隐藏非系统 overlay。

推荐把页面策略写成清晰的决策表：

| 风险输入 | 敏感操作 | 用户反馈 | 后续动作 |
|---|---|---|---|
| direct | 拒绝 | 告知存在覆盖风险 | 等待新的、干净的 DOWN 流 |
| partial-only | 高价值确认页也拒绝 | 指示关闭悬浮内容 | 不在同一可疑流内重试 |
| 无标志 | 继续业务校验 | 正常 UI | 仍检查权限、会话与业务状态 |

两位都为零也不是“绝对可信”：同 token、同 PID、trusted 类型会被设计性豁免，frame 模型也不提供覆盖者身份或像素级语义。触摸过滤应与业务二次确认、限时、不可重放 token 等机制并用。

## 15. 纵深防御：隐藏非系统 Overlay 及相邻机制边界

另一条防线发生在 WMS。`Window.addSystemFlags(SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS)` 实际把该位加入 `privateFlags`；API 是 `@SystemApi`，要求 `HIDE_NON_SYSTEM_OVERLAY_WINDOWS`。清单把权限声明为 `signature|preinstalled`，`Session` 在构造时把检查结果快照到 `mCanHideNonSystemOverlayWindows`。所以未授权进程即使设法把位写进属性，`WindowState.hideNonSystemOverlayWindowsWhenVisible()` 仍返回 false。

授权请求窗口还必须进入 Surface shown 状态。`WindowSurfaceController.setShown()` 把状态交给 `updateNonSystemOverlayWindowsVisibilityIfNeeded()`；已 shown 窗口改变该 private flag 时，relayout 路径也用当前 shown 值更新。请求窗口只有在 `surfaceShown && predicate` 时进入 `mHidingNonSystemOverlayWindows`。列表从空变非空或从非空变空时，WMS 才通过 `mRoot.forAllWindows()` 遍历全部窗口。该调用没有 display 筛选，因此 r48 的开关是全局、可跨 display 的；多个请求者由成员表共同维持，少一个请求者消失并不一定恢复覆盖层。全局状态已开启后新添加的窗口，也会在 addWindow 路径按当前列表立即调用同一 force-hide 判断。

被强制隐藏的目标也有严格范围：它必须是 `isSystemAlertWindowType(type)` 所列六种之一——`PHONE`、`PRIORITY_PHONE`、`SYSTEM_ALERT`、`SYSTEM_ERROR`、`SYSTEM_OVERLAY`、`APPLICATION_OVERLAY`——或者 `TYPE_TOAST`，并且 owner 没有添加内部系统窗口的能力。普通 Activity、IME，以及具备内部系统窗口能力的覆盖窗口都不会因这条策略被强制隐藏。

`setForceHideNonSystemOverlayWindowIfNeeded()` 改内部 gate 后调用 `hideLw()` 或 `showLw()` 并请求动画。方法返回和成员表更新是策略完成点，不等于 Surface 合成与 InputWindow 快照已在同一瞬间完成；解除 force-hide 也只移除这一道门，其他可见性原因仍可能使窗口保持隐藏。

### 练习 9：手算 WMS 纵深防御矩阵

固定安全窗口 `S`：其 Session 有权限、已设置系统隐藏位、`surfaceShown=true`。候选覆盖窗口依次是普通进程的 `TYPE_APPLICATION_OVERLAY`、普通进程的 `TYPE_TOAST`、IME、具备内部系统窗口能力的 trusted application overlay。

| 候选 | 此策略是否 force-hide | 理由 |
|---|---:|---|
| 普通 application overlay | 是 | alert type，owner 无内部能力 |
| 普通 toast | 是 | 显式纳入范围，owner 无内部能力 |
| IME | 否 | 不在 alert 六类或 toast 范围 |
| 内部系统 trusted overlay | 否 | owner 能添加内部系统窗口 |

当 S 不再 shown 且没有其他请求者时，成员表变空，WMS 遍历并解除这道 force-hide。若普通 overlay 之后因其他条件恢复可见并以不可触摸方式覆盖 T 的触点，则输入链仍可标记 `OBSCURED`，开启默认过滤的敏感 View 会拒绝；这正是两条防线应叠加的原因。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS = 0x00080000' frameworks/base/core/java/android/view/WindowManager.java
grep -n -F '@RequiresPermission(permission.HIDE_NON_SYSTEM_OVERLAY_WINDOWS)' frameworks/base/core/java/android/view/WindowManager.java
grep -n -F 'public void addSystemFlags' frameworks/base/core/java/android/view/Window.java
grep -n -F 'mCanHideNonSystemOverlayWindows = service.mContext.checkCallingOrSelfPermission' frameworks/base/services/core/java/com/android/server/wm/Session.java
grep -n -F '<permission android:name="android.permission.HIDE_NON_SYSTEM_OVERLAY_WINDOWS"' frameworks/base/core/res/AndroidManifest.xml
grep -n -F 'boolean hideNonSystemOverlayWindowsWhenVisible()' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'void updateNonSystemOverlayWindowsVisibilityIfNeeded' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'mHidingNonSystemOverlayWindows.add(win);' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'mRoot.forAllWindows((w) -> {' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'w.setForceHideNonSystemOverlayWindowIfNeeded(hideSystemAlertWindows);' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
grep -n -F 'void setForceHideNonSystemOverlayWindowIfNeeded' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'isSystemAlertWindowType(mAttrs.type)' frameworks/base/services/core/java/com/android/server/wm/WindowState.java
grep -n -F 'case TYPE_APPLICATION_OVERLAY:' frameworks/base/core/java/android/view/WindowManager.java
grep -n -F 'mService.updateNonSystemOverlayWindowsVisibilityIfNeeded' frameworks/base/services/core/java/com/android/server/wm/WindowSurfaceController.java
grep -n -F 'w.addSystemFlags(SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS);' frameworks/base/packages/SystemUI/src/com/android/systemui/media/MediaProjectionPermissionActivity.java
```

相邻机制要保持边界：`FLAG_SECURE` 管截图、非安全显示等内容保护，不是触摸遮挡过滤器；第 241 章解释正常目标的 touchable geometry，第 242 章解释边缘返回手势的 exclusion 与仲裁。本章只在目标已经选出后追遮挡风险。完整的 action 改写、pointer 子集、坐标、OUTSIDE/monitor、eventId 与 `DispatchEntry` 细节留到第 244 章。

## 16. r48 诊断顺序、结论与第 244 章接口

遇到“明明点到正确按钮，却被判遮挡”或“有悬浮窗却没被拒绝”，按下面顺序诊断最省时间：

1. **先锁版本与 display 窗口序**：确认分析的是 r48 的 `InputWindowInfo` 快照，找到目标 exact handle 及其上方窗口；不要从截图直接代替输入快照。
2. **先算正常命中**：逐项检查 `visible`、`NOT_TOUCHABLE`、touch-modal 与 `touchableRegion`，得到唯一目标。
3. **再过五道主体门**：同 token、不可见、同 PID、trusted 类型、异 display 中任一成立，候选都不构成此处遮挡。
4. **最后算两种 frame 几何**：先半开矩形含点，再严格正面积相交；记录这是否为普通新建账、split 更新、slippery、wallpaper 或 transfer。
5. **沿 per-target 传输**：从 `TouchedWindow.targetFlags` 到 `InputTarget`、`DispatchEntry.resolvedFlags`、`publishMotionEvent()`，不要拿另一个 target 的 flags 代替当前连接。
6. **核对消费策略**：默认 View 只拒 direct；严格页面是否拒 partial、listener 是否持续消费、根 ViewGroup 是否统一拦截，都要分别验证。
7. **若使用隐藏策略，核对三条件**：请求者有 Session 权限、标志有效、Surface shown；再检查全局成员表和被隐藏窗口是否属于限定类型且 owner 无内部能力。
8. **把完成点写入证据**：临时风险位、权限后 TouchState 提交、InputTarget、outbound、成功 publish 后的 wait、View 拒绝、FINISHED、WMS gate 与 Surface/InputWindow 新快照都不是同一个时刻。

最终可以把整章压缩为一句话：Android 11 r48 先按正常规则选目标，再用目标上方窗口的主体关系与 frame 几何生成逐目标风险位；默认 View 只拒直接遮挡，严格页面需自行覆盖 partial，而特权 WMS 隐藏策略是独立、范围受限且依赖 shown 状态的纵深防线。

第 244 章将从这里的 `InputTarget` 接口继续向下，完整展开 `DispatchEntry` 如何改写 action、筛选 pointer、变换坐标、分发到 OUTSIDE 与 monitor 目标，并解释 eventId 与 HMAC 在不同目标上的最终完成点。
