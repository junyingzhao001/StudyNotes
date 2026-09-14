# 236 Android InputDispatcher触摸目标、TouchState、多指拆分与pilfer链

上一章从 focused window 讲到 Connection 回执与输入 ANR。本章回到 publish 之前，回答 pointer Motion 最核心的问题：一个 `ACTION_DOWN` 怎样选出窗口，为什么后续 `MOVE` 通常不再跟着坐标换窗口，多指怎样拆成各自合法的局部事件流，gesture monitor 又怎样在手势中途 pilfer（截获）窗口的后续接收权。

先给结论：**普通触摸路由不是“每一帧重新命中”，而是“首个 DOWN 建立所有权，后续事件沿 TouchState 延续”。** outside、split、slippery、wallpaper、monitor 与 pilfer 看似例外很多，核心仍是让通道正常的主路径保持完整事件流；通道注销、目标失联等生命周期断点则必须单独识别，不能伪装成正常所有权转移。

本文以 `android-11.0.0_r48` 为准，核心源码位于：

- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp`
- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.h`
- `frameworks/native/services/inputflinger/dispatcher/TouchState.h` 与 `TouchState.cpp`
- `frameworks/native/services/inputflinger/dispatcher/TouchedWindow.h`
- `frameworks/native/services/inputflinger/dispatcher/InputTarget.h`
- `frameworks/native/include/input/InputWindow.h`
- `frameworks/base/core/java/android/view/InputMonitor.java`
- `frameworks/base/core/java/android/view/IInputMonitorHost.aidl`
- `frameworks/base/services/core/java/com/android/server/input/InputManagerService.java`

## 1. 一条主线：DOWN建账，UP或CANCEL结账

先把普通单指手势压缩成一条时间线：

```text
MotionEntry(DOWN)
  → 按当前 display 的窗口列表做 hit-test
  → 生成本次 InputTarget
  → 把窗口、角色与流身份写入 TouchState
  → publish 到目标 Connection

MotionEntry(MOVE / POINTER_UP ... UP/CANCEL)
  → 读取同一 display 的 TouchState
  → 沿既有窗口继续生成 InputTarget
  → UP/CANCEL 发给当前目标后 reset TouchState
```

手指从 A 的区域移到 B 上方，默认仍是 `A: DOWN → MOVE → UP`，B 什么也收不到。若每个 MOVE 都重新命中，A 会缺少结束事件，B 会从无来源的 MOVE 开始，点击、拖拽、长按和速度计算都无法维持状态。

需要重新选目标的情形都由源码显式列出：首个 DOWN、SCROLL、Hover，以及已进入 split 模式后的 `POINTER_DOWN`；单指 slippery MOVE 是另一条受限的转移分支。把“普通延续”和这些显式入口分开，是读懂本章的第一把钥匙。

## 2. 六个对象分别记录事实、所有权和一次投递

触摸链里最容易混淆的对象可以这样分层：

| 对象 | 生命周期 | 回答的问题 |
|---|---|---|
| `MotionEntry` | 一条原始输入事件 | 设备这次报告了什么 action、坐标和 pointer 数组 |
| `InputWindowHandle` | 一版 WMS 输入窗口快照 | 哪些窗口可见、可触摸，Region、flags、token 是什么 |
| `TouchState` | 一个 display 当前的手势状态 | 这条流由哪个设备产生，哪些窗口/monitor 持有它 |
| `TouchedWindow` | TouchState 中的窗口角色 | 此窗口有哪些 target flags，拥有哪些 pointer IDs |
| `InputTarget` | 当前 MotionEntry 的投递计划 | 本次要向哪个 Channel 发送何种模式、坐标变换和 ID 子集 |
| `DispatchEntry` | 面向一条 Connection 的协议账 | 最终 action、event ID、seq、delivery/deadline 是什么 |

`mTouchStatesByDisplay` 是 `displayId → TouchState` 的 map，所以多 display 可以各有一份状态；但 r48 的每份 TouchState 只有一组 `deviceId/source/displayId` 身份，不是“同一 display 任意多设备并行流”的通用容器。

`InputWindowHandle` 也不是 Java View。InputDispatcher 只能选 Window 与 Channel；事件进入应用以后，才由 ViewRoot/ViewGroup 在窗口内部寻找具体 View。

## 3. `dispatchMotionLocked()`先区分pointer，再决定monitor加入时机

InputReader 交给 listener 的边界对象是 `NotifyMotionArgs`；`InputDispatcher::notifyMotion()` 接收它并创建内部 `MotionEntry`，之后才进入 `dispatchMotionLocked()`。后者用 source 的 `AINPUT_SOURCE_CLASS_POINTER` 位区分两条路线：

- pointer Motion 进入 `findTouchedWindowTargetsLocked()`，使用本章的 TouchState。
- trackball 等非 pointer Motion 进入上一章的 `findFocusedWindowTargetsLocked()`。

目标选择成功后，代码才追加当前或 focused display 的 global monitor，再统一进入 `dispatchEventLocked()`。如果目标选择返回 `PENDING`，本轮先等待，不取消 monitor；permission denied 则直接丢弃。只有已经得到最终结果、且既非 success 也非 permission denied 时，代码才按 pointer/non-pointer 模式为注册表中的 monitor 合成取消语义。global monitor 不能把失败“救活”。

这与 gesture monitor 不同。gesture monitor 参与 `findTouchedWindowTargetsLocked()`，首个 DOWN 即使没有普通触摸窗口，只要有合格的 gesture monitor，也仍可能构成成功目标。两类 monitor 的加入时机与状态归属必须分账。

目标选择还可能报告 `conflictingPointerActions`。权限允许且本次仍成功时，dispatcher 会先对所有 Connection 合成 pointer cancel，再发布新事件，使旧流不会和新设备/新 DOWN 的流悄悄重叠。

## 4. hit-test按前到后遍历，modal与Region共同决定是否穿透

`findTouchedWindowAtLocked(displayId, x, y, ...)` 取得该 display 的窗口列表并按 front-to-back 遍历。对每个 handle，依次关注：

1. `windowInfo->displayId` 是否匹配。
2. `visible` 是否为真。
3. 是否没有 `FLAG_NOT_TOUCHABLE`。
4. 窗口是否 touch modal，或 `touchableRegionContainsPoint(x, y)` 是否为真。

touch modal 的 r48 定义是同时没有 `FLAG_NOT_FOCUSABLE` 与 `FLAG_NOT_TOUCH_MODAL`。因此：

- `NOT_TOUCHABLE` 让窗口不能成为正常命中/foreground 目标，不是“收到后不处理”；若它同时 visible 且声明 `WATCH_OUTSIDE_TOUCH`，仍可能作为 outside 观察者加入。
- `NOT_TOUCH_MODAL` 使区域外坐标继续向低 z 序窗口查找。
- `NOT_FOCUSABLE` 不等于不能接收触摸；它只是也让窗口不能靠 modal 语义吞掉 Region 外的点。
- touchable Region 可以是非矩形，不能只拿 frame 判断命中。

触屏命中把 action pointer 的浮点 X/Y 转成 `int32_t`；投递给客户端的 PointerCoords 仍保留浮点值。mouse 则使用独立的 `xCursorPosition/yCursorPosition`。Case 1 的正常 split 判定会排除 mouse；slippery 分支缺少同一排除，后文单独说明。

若命中的是 portal window，函数可递归到 `portalToDisplayId` 继续查找，同时把经过的 portal 记录进临时 TouchState。这里的 portal 不是最终前台接收者；它还会影响目标 display 上 monitor 的收集与坐标偏移。

## 5. 临时TouchState先演算，权限通过后才允许影响全局状态

`findTouchedWindowTargetsLocked()` 先从 `mTouchStatesByDisplay` 取旧状态，再 `copyFrom()` 到 `tempTouchState`。窗口加入、outside、split、hover 与 monitor 选择都先作用于临时副本。

首个 DOWN、SCROLL 或 Hover 被视为 `newGesture`。代码重置临时状态并写入本次 `deviceId/source/displayId`；只有 DOWN 把 `down` 设为真。普通 MOVE、POINTER_UP、UP、CANCEL 和非 split 的 `POINTER_DOWN` 则沿旧状态进入延续分支。

这套“先演算”主要守住注入安全：软件注入必须对所有 foreground window 通过身份/`INJECT_EVENTS` 检查，才允许后续状态提交；outside、wallpaper 与 monitor 不逐项做这项检查。若本次只有 gesture monitor 而没有 foreground window，代码直接把 permission 状态记为 granted。真实 InputReader 事件没有 `InjectionState`，但仍走相同的窗口与 TouchState 算法。

不过它不是“只有目标选择成功才提交”的数据库事务。到 `Failed:` 标签后，代码仍先收敛 injection permission；只要权限为 granted、且不是 `wrongDevice` 分支，某些失败路径仍会按 action 更新或保存临时状态。真实 InputReader 事件以 null injection state 通过这道权限检查，所以一个 hit-test 没记录任何窗口、也没有 gesture monitor 的 DOWN 虽然返回 failed，仍可能留下 `down=true`、且 `windows` 为空的状态；若途中已收集 outside 或 portal，失败尾还可能把这些一次性记录一并保存。准确边界是“未授权事件不能改真实路由账”，而不是“任何 injection failed 都绝不触碰 TouchState”。

设备冲突还要按 action 分开。map 已按本次 display 查找，所以另一个 display 通常使用另一份状态；同一 display 内，deviceId 或 source 任一改变都算切流。不同流的 MOVE 直接以 permission denied 返回且不写回；已有 down 时的 SCROLL 走 failed/`wrongDevice`，也不写回。新的 DOWN 或 Hover 会先重置临时状态；权限通过且不是 `wrongDevice` 时，失败尾也可能标记 conflict。其他 POINTER_DOWN/POINTER_UP/UP/CANCEL 则可能沿旧状态完成本次选择，再在尾部标记 conflict，其中 UP/CANCEL 还会 reset。只有成功结果回到 `dispatchMotionLocked()` 后，conflict 才会触发面向所有 Connection 的 pointer cancel。

## 6. Case 1的新窗口还要过paused、Connection与responsive三道门

在 new gesture 或 split `POINTER_DOWN` 的 Case 1 中，hit-test 得到 handle 后，代码先决定 split 能力，再检查目标是否可实际开始这次新投递：

- paused window 被置空。
- token 找不到 Connection 时被置空。
- Connection 已是 `responsive=false` 时，不向它开始新 gesture。
- 新选出的 gesture monitors 也会经 `selectResponsiveMonitorsLocked()` 过滤。

若普通窗口为空且这次新选的 gesture monitor 也为空，目标选择失败。这里有两个容易误推的边界。

第一，这些门只约束 Case 1 的 `newTouchedWindowHandle`。已经记录在 TouchState 的窗口不会在每个 MOVE 上重新执行同一组 paused/responsive hit-test；outside、稍后批量加入的 wallpaper，以及 Case 2 的 slippery 新窗口也不经过这整组三道门。旧流要靠取消、窗口移除或完成协议收口。

第二，paused 对 pointer 新手势不是上一章按焦点寻址事件的 pending 等待。没有 gesture monitor 接住时，它会走 injection failed/drop，也不会启动一只 paused 专用 ANR 计时器。

正常窗口目标得到 `FLAG_FOREGROUND | FLAG_DISPATCH_AS_IS`，split 时再加 `FLAG_SPLIT`。这里的 FOREGROUND 表示“本次触摸的主要窗口”，不等于键盘 focused window；未持有键盘焦点的窗口完全可能因触点命中而成为 foreground touch target。

## 7. obscured flags描述遮挡事实，不等于dispatcher拒绝事件

InputDispatcher 在选定 foreground window 时计算两种标志：

- 上层合格窗口的 frame 覆盖触点：`FLAG_WINDOW_IS_OBSCURED`。
- 未覆盖触点，但上层窗口矩形与目标窗口相交：`FLAG_WINDOW_IS_PARTIALLY_OBSCURED`。

`canBeObscuredBy()` 会排除同 token 克隆层、不可见窗口、同 owner PID、trusted overlay 与不同 display 的窗口。这里比较的是 owner PID，不是 owner UID；点遮挡看 frame，部分遮挡看窗口 overlap，均不能和命中所用的 touchable Region 混为一谈。

进入 `DispatchEntry` 后，它们分别变成 `AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED` 与 `AMOTION_EVENT_FLAG_WINDOW_IS_PARTIALLY_OBSCURED`。r48 的这段 dispatcher 逻辑是在事件上携带事实，不是看到遮挡就一律丢触摸；应用或上层组件是否拒绝敏感操作，要看其安全策略。普通 continuation 不逐帧重算遮挡，而是沿 TouchState 中保留的 target flags；slippery enter 只重算 point-obscured，wallpaper 则固定同时带两种标志。

坐标变换同样属于 InputTarget/DispatchEntry，而非 TouchState 所有权：窗口目标记录 frame 左上角负偏移、window scale 与 global scale。理解“谁接收”和“客户端看到什么坐标”时要分别核对。

## 8. WATCH_OUTSIDE只参与首个DOWN，并在当前投递后退出状态

只有首个 `ACTION_DOWN` 调 hit-test 时传入 `addOutsideTargets=true`。遍历尚未命中的可见上层窗口时，若它带 `FLAG_WATCH_OUTSIDE_TOUCH`，临时 TouchState 会加入 `FLAG_DISPATCH_AS_OUTSIDE` 角色。

这类目标本次收到的是派生的 `ACTION_OUTSIDE`，不是原始 DOWN。生成当前 `inputTargets` 后，`filterNonAsIsTouchWindows()` 会删除纯 outside 角色，因此后续 MOVE/POINTER_UP/UP/CANCEL 不再投给它。

outside 不能独自让路由成功：仍需至少一个 foreground window 或 gesture monitor。若 outside window 与实际 foreground window 的 owner UID 不同，代码给 outside 目标追加 `FLAG_ZERO_COORDS`；发布前所有 PointerCoords 被 `clear()`，避免跨应用泄露精确位置。同 UID 的多个顶层窗口不会因这条规则自动清零。

这形成一个典型的“当前目标与下一状态不同”：outside 出现在本次 InputTarget，却不会留在下一次 TouchState。把二者当成同一个集合，就会误以为它也应持续收到整条手势。

## 9. MOVE沿旧账，POINTER_UP先投递再移除，UP或CANCEL才reset

TouchState 的持久化规则按动作分别处理：

- 普通 MOVE 沿 `windows` 与 `gestureMonitors` 继续，不重新命中。
- split `POINTER_UP` 先用旧 pointer ownership 生成当前 targets；随后才从带 `FLAG_SPLIT` 的窗口清除该 pointer ID，集合为空的窗口从状态移除。
- `ACTION_UP` 与 `ACTION_CANCEL` 也先形成当前投递，然后 reset 整份临时状态并从 map 删除。
- SCROLL 的临时 TouchState 只服务当前动作，不写回 map。
- Hover 通过全局单例、而非按 display 分桶的 `mLastHoverWindowHandle` 补齐旧窗口 HOVER_EXIT、新窗口 HOVER_ENTER；保存的主要是 hover 设备身份，不是 `down` 手势窗口集合。

这体现出固定顺序：先从临时状态输出“当前事件发给谁”，再过滤一次性角色、更新 pointer ownership，最后决定写回或删除 map。

窗口快照在手势中途更新时，`setInputWindowsLocked()` 会逐项检查 `state.windows`。被移除的窗口若仍有 Channel，就按 `CANCEL_POINTER_EVENTS` 由该 Connection 的 `InputState` 生成取消语义，然后只移除这一项；其他 split 窗口、wallpaper 或 gesture monitor 可以继续。取消只是加入分发队列，不代表客户端已消费或回执。

这里的清理范围并不对称：`setInputWindowsLocked()` 只查看“本次更新 display 所键控的 TouchState”里的 `windows`，不会顺手清 `portalWindows` 或 `gestureMonitors`；跨 portal 存在入口 display 状态时，更新嵌入 display 的窗口也不会反向扫描它。`unregisterInputChannelLocked()` 会移除 Connection 和 monitor 注册，却同样不遍历 TouchState。因此旧 handle/monitor 可能暂留到账本自然结束；窗口目标会在找不到 Channel 时跳过，monitor 目标会在找不到 Connection 时跳过。设备 reset 会为匹配设备的 Connection 合成取消，却不清 map；`resetAndDropEverythingLocked()` 才直接清空全部 TouchState。这些是生命周期清理边界，不是并发数据竞争。

## 10. split资格由首窗建立，新指针只重新分配自己的pointer ID

首个 DOWN 命中的窗口支持 split 且 source 不是 mouse 时，本地 `isSplit` 变为真，foreground target 带 `FLAG_SPLIT`，action pointer ID 被记入 `TouchedWindow.pointerIds`。

之后 `ACTION_POINTER_DOWN` 才会再次按 action index 对应的新指针坐标 hit-test。旧 pointer 不重新分配。若新命中窗口支持 split，就给它加入这一枚 pointer ID；同一个窗口再次命中时，`addOrUpdateWindow()` 对 BitSet 做 OR。

若手势已经 split，而新命中的窗口不支持 split，代码忽略这个新窗口，再尝试把新 pointer 分给当前第一个 foreground window。若 hit-test 根本没找到窗口，也走相同 fallback。

但 paused、Connection 不存在或 unresponsive 的检查发生在 fallback 之后。一个先被命中、随后在这些门上被置空的新窗口，并不会再次 fallback；若本次也没有新 gesture monitor，`POINTER_DOWN` 会失败。这是顺序决定的 r48 边界。

pointer ID 与 pointer index 也必须分开：index 是本次 MotionEvent 数组位置，抬指后可能重排；ID 在一条手势内稳定，才适合作为跨事件的窗口所有权。

## 11. `splitMotionEvent()`同时裁剪数组并重写局部action

只有目标带 `FLAG_SPLIT`，且其 pointer ID 数不等于原事件 `pointerCount` 时，`prepareDispatchCycleLocked()` 才构造新的 MotionEntry。函数复制目标 BitSet 中的 PointerProperties/PointerCoords，并按局部视角重写 action：

| 原始动作 | 变化 pointer 是否属于目标 | 目标拥有 ID 数 | 局部动作 |
|---|---:|---:|---|
| `POINTER_DOWN` | 是 | 1 | `DOWN` |
| `POINTER_UP` | 是 | 1 | `UP` |
| `POINTER_DOWN/UP` | 是 | 多个 | 保留 masked action，action index 改为裁剪后位置 |
| `POINTER_DOWN/UP` | 否 | 任意 | `MOVE` |

例：id0 已在 A，id1 新落到 B。原始 `POINTER_DOWN(id0,id1; action=id1)` 会变成 `A: MOVE(id0)` 与 `B: DOWN(id1)`。id1 抬起时对应 `A: MOVE(id0)` 与 `B: UP(id1)`，最后 id0 的全局 UP 再成为 A 的 UP。

一旦进入 `splitMotionEvent()`，若目标 BitSet 期望的某个 ID 在原 MotionEntry 中不存在，函数返回空并放弃该目标的 split 投递。成功拆分会取得新的 event ID，保留时间、设备、source、display、精度、downTime 等字段，并增加 InjectionState 引用。

但调用者先以“目标 ID 数是否等于原 pointerCount”决定要不要拆：若数量相等，即使具体 ID 集合异常地不同，也会绕过 `splitMotionEvent()` 而直接投递原事件。正常输入序列不应出现这种组合，但这说明 r48 的缺 ID 防御不是覆盖所有集合不一致的完备校验。

split 是逐 InputTarget 属性，不是把原始事件自动裁成全局唯一形态。没有 `FLAG_SPLIT` 的 wallpaper 与 monitor 仍可收到完整原始 pointer 数组。

## 12. slippery用同一MOVE派生旧CANCEL与新DOWN

普通 MOVE 唯一的窗口重命中特例是 slippery。触发条件同时包括：

- action 为 MOVE；
- `pointerCount == 1`；
- TouchState 恰好有一个 foreground window；
- 该窗口带 `FLAG_SLIPPERY`。

只有新旧窗口都非空且不同才建立转移计划。旧窗口被标成 `DISPATCH_AS_SLIPPERY_EXIT`，当前 MOVE 在它的 DispatchEntry 中解析成 CANCEL；新窗口被标成 `DISPATCH_AS_SLIPPERY_ENTER`，同一个 MOVE 对它解析成 DOWN。若新窗口支持 split，`isSplit` 会被置真；但状态原本已经 split 时，即使新窗口不支持，分支也不会把它清回 false。只要 `isSplit` 最终为真，新目标就带 `FLAG_SPLIT` 并记录该 pointer ID。

r48 的 slippery 分支没有复用 Case 1 的 paused/Connection/responsive 检查。它先生成 enter target；到 `addWindowTargetLocked()` 时若 Channel 已不可查，本次投递目标会被跳过，但 enter 经过滤归一化后仍可能留在 TouchState。因此“新窗口收到 DOWN”应读作尝试形成并投递局部 DOWN，而不是同步送达保证。

这条分支还没有 `isFromMouse` 排除：若 slippery 新窗口支持 split，本地状态也会设 `isSplit=true` 并带 `FLAG_SPLIT`。典型 mouse 仍只有一个 pointer，通常不会触发实际数组裁剪，但不能把“mouse 永不出现 split 状态”当作 r48 全局不变量。

slippery 重命中还使用 `addOutsideTargets=false`、`addPortalWindows=false` 的默认参数。它可以递归穿过 portal 找到新窗口，却不会把这条新 portal 路径记入状态，也不会在 MOVE 时新选 gesture monitor；本次 MOVE 的 portal global monitor 追加仍只依据进入该事件前 TouchState 已保存的 `portalWindows`。

本次 targets 输出后，`filterNonAsIsTouchWindows()` 删除旧 exit，把新 enter 归一为 AS_IS，于是下一次 MOVE 沿新窗口继续。新目标为空时不会把旧窗口静默丢掉；多指针也不进入这条路径。

slippery 不是 `transferTouchFocus(from, to)`。后者是另一个显式 API，会改写 TouchState、合并两条 Connection 的 InputState，并在两端 Connection 都存在时为旧端生成 CANCEL、为新端合成 DOWN；本章不把它混入基于 MOVE 坐标的 slippery 判定。

## 13. wallpaper、portal、global monitor与gesture monitor有四种加入规则

这些附加路由角色可以用一张表分开：

| 角色 | 何时加入 | 是否留在 TouchState | 能否让无窗口 DOWN 成功 |
|---|---|---:|---:|
| wallpaper | 首个 DOWN 的 foreground window `hasWallpaper` | 是，直到流结束/被移除 | 否 |
| portal | hit-test 穿过跨 display 门户 | 作为 portalWindows 留存 | 否 |
| global monitor | pointer 目标选择成功后统一追加 | 否 | 否 |
| gesture monitor | 首个 DOWN 选择响应式 monitor | 是 | 是 |

wallpaper 收集发生在 foreground 确认后：代码枚举的是原始 MotionEntry/入口 `displayId` 上的全部 `TYPE_WALLPAPER`，即使 foreground 是经 portal 命中的嵌入 display 窗口，也不会改用那个嵌入 display。它们不另行筛 visible、paused、Connection、responsive 或注入权限，被加入 AS_IS 并固定带 full/partial obscured flags；缺少 Channel 时只在生成 InputTarget 时跳过。wallpaper 是锁定的副本目标，不是 foreground，也不因 A/B split 自动裁剪。Hover 与 SCROLL 不走 wallpaper 收集。

portalWindows 让 dispatcher 同时加入 portal 目标 display 上的 monitor。portal frame 左上角的负偏移用于对应 monitor 坐标；后续事件仍沿记录的 portal 路径追加相关 global monitors。

gesture monitor 只在首个 DOWN 新选并过滤 unresponsive Connection，随后作为 TouchState 一部分持续收流。它面向 pointer gesture，不接 Key；global monitor 则由 Key/Motion 的公共追加路径加入。两者都有 InputChannel、Connection、waitQueue 与 finished 协议，“监控者”不等于无需回执。

gesture monitor 的安全门在能力创建处：`InputManagerService.monitorGestureInput()` 先要求调用者持有 `MONITOR_INPUT`，再创建 Channel pair、注册 native monitor，并把 consumer Channel 与 `IInputMonitorHost` 一起封装进可跨进程传递的 `InputMonitor`。后续 `pilferPointers()` 不重新读取 Binder caller 权限，而是凭这枚已交付 host 最终携带的 Channel token 做 native 资格检查；“获准创建 capability”与“该 token 正参与当前 down 流”仍是两道独立条件。

## 14. pilfer先验证当前参与资格，再取消窗口并保留整组monitor

调用链是：

```text
InputMonitor.pilferPointers()
  → oneway IInputMonitorHost.pilferPointers()
  → InputManagerService.InputMonitorHost
  → nativePilferPointers
  → InputDispatcher::pilferPointers(token)
```

native 依次验证：token 仍属于已注册 gesture monitor；对应 display 有 TouchState；该 token 确实在 `state.gestureMonitors` 中；`state.down` 为真。只注册却没参与当前 DOWN，或流已经结束，都得到 native `BAD_VALUE`。

portal monitor 还有 display 键不对称：它虽然能通过 portal 被收进“入口 display”的 TouchState，但 pilfer 首先从 token 找到 monitor 自己的注册 display，再用该 display 查 TouchState。因此只经 portal 参与的 monitor 通常找不到那条跨 display 流，不能凭参与收包就推断它一定有 pilfer 资格。

验证通过后，dispatcher 遍历 `state.windows`，对仍能找到 Channel 的每个窗口使用限定当前 device/display 的 `CANCEL_POINTER_EVENTS`，由各 Connection 的 InputState 生成具体取消事件；它不按 source 或某个 pointer ID 子集再缩小，所以同一 Connection 上满足 device/display 的 pointer-class memento 都可能被取消。多个 TouchState 窗口若共享 Channel，循环可能重复请求，但第一次通常已经清掉可匹配 memento。随后 `state.filterNonMonitors()` 清空 windows 与 portalWindows。

它保留的是整组 gesture monitors，而非只有 pilfer 发起者；同时保留 `down/split/deviceId/source/displayId`。所以 TouchState 对已存在 pointer 的后续 MOVE/POINTER_UP/UP/CANCEL 不再产出窗口目标，只产出 gesture monitor；`dispatchMotionLocked()` 仍会另加 global monitors，UP 或 CANCEL 才 reset 状态。已发布或已经排在窗口 outbound 中的旧事件不会被“倒吸回来”，取消也要排队、送达和回执。

Java 侧还有一个完成边界：`IInputMonitorHost` 整个接口是 oneway，`nativePilferPointers()` 又忽略 native `status_t`。因此 `InputMonitor.pilferPointers()` 返回 void 只表示请求已提交；调用者既不能据此证明 native 已执行，也观察不到资格校验的 `BAD_VALUE`，更不能证明窗口已收到 CANCEL。即使 native 返回 `OK`，合成 CANCEL 也只是排入 Connection 队列：原有 outbound 项可能在它前面，pipe 满时它还会停在 outbound，真正消费与 finished 更晚。

r48 还存在一个反直觉尖角：`filterNonMonitors()` 不清 `split`。若 pilfer 前已经是 split 流，后续又出现 `POINTER_DOWN`，代码仍会进入 split 新指针 hit-test；只要它命中可用且支持 split 的窗口，就可能重新把这个新窗口加入 TouchState，并让它收到局部 DOWN。因而“pilfer 后直到 UP/CANCEL 永远只有 monitor”只适用于既有 pointers 的 MOVE/POINTER_UP/UP/CANCEL，不能推广到这种新增指针。反过来，若这枚新 pointer 找不到合格窗口，Case 1 的立即失败门查看的是仅 DOWN 才填充的 `newGestureMonitors`，不是状态里保留的旧 monitors；该 POINTER_DOWN 仍会失败，并对注册表中的 monitor Connections 合成 pointer cancel，而权限通过的失败尾还可能把原 TouchState 保存回去。

pilfer 清空 portalWindows 后，既有 pointer 的普通 MOVE/POINTER_UP/UP/CANCEL 不再追加 portal display 的 global monitors；已经选入 `gestureMonitors` 的 portal monitor 仍保留。但 split `POINTER_DOWN` 会以 `addPortalWindows=true` 重新 hit-test，可能重建 portal 路径：成功时该事件就能再次追加 portal globals，失败尾也可能把新路径写回，供后续成功事件使用。这种 portal monitor 的 token 仍受前述注册 display 查表不对称限制，是否继续收包与是否能主动 pilfer 是两个问题。

## 15. 九组只读练习：逐段重建路由账

下面命令只读，默认源码根目录为 `/Users/ninebot/androidSource`，也可把其他 AOSP 根目录作为第一个参数传入。每条 `grep` 都应独立命中；任何一条失败都表示源码版本或形态不同，应先核对基线再继续推理。

### 练习 1：从Motion入口走到窗口命中

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'bool InputDispatcher::dispatchMotionLocked(nsecs_t currentTime, MotionEntry* entry,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool isPointerEvent = entry->source & AINPUT_SOURCE_CLASS_POINTER;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'findTouchedWindowTargetsLocked(currentTime, *entry, inputTargets, nextWakeupTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<InputWindowHandle> InputDispatcher::findTouchedWindowAtLocked(' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '// Traverse windows from front to back to find touched window.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!(flags & InputWindowInfo::FLAG_NOT_TOUCHABLE)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (isTouchModal || windowInfo->touchableRegionContainsPoint(x, y)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (addOutsideTargets && (flags & InputWindowInfo::FLAG_WATCH_OUTSIDE_TOUCH)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'addGlobalMonitoringTargetsLocked(inputTargets, getTargetDisplayId(*entry));' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：按源码顺序说明 pointer/non-pointer 分流，以及 visible、NOT_TOUCHABLE、touch modal、Region、outside 与 global monitor 各在哪个阶段生效。

### 练习 2：区分临时演算、当前targets与下一状态

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'std::unordered_map<int32_t, TouchState> mTouchStatesByDisplay GUARDED_BY(mLock);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.h
grep -n -F 'tempTouchState.copyFrom(*oldState);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool newGesture = (maskedAction == AMOTION_EVENT_ACTION_DOWN ||' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '// Success!  Output targets.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'tempTouchState.filterNonAsIsTouchWindows();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (injectionPermission != INJECTION_PERMISSION_GRANTED) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!wrongDevice) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (maskedAction != AMOTION_EVENT_ACTION_SCROLL) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mTouchStatesByDisplay[displayId] = tempTouchState;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：解释为何“权限确认前不改全局”不等于“目标选择失败必回滚”，并推演 outside、SCROLL、UP 三种当前目标与下一状态的差异。

### 练习 3：核对新目标门、注入权限与坐标保护

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (newTouchedWindowHandle != nullptr && newTouchedWindowHandle->getInfo()->paused) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<Connection> connection = getConnectionLocked(newTouchedWindowHandle->getToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '} else if (!connection->responsive) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newGestureMonitors = selectResponsiveMonitorsLocked(newGestureMonitors);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!haveForegroundWindow && !hasGestureMonitor) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!checkInjectionPermission(touchedWindow.windowHandle, entry.injectionState)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (inputWindowHandle->getInfo()->ownerUid != foregroundWindowUid) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'tempTouchState.addOrUpdateWindow(inputWindowHandle,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (dispatchEntry->targetFlags & InputTarget::FLAG_ZERO_COORDS) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'scaledCoords[i].clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：说明仅有 outside 为什么不足以成功、gesture monitor 怎样补位、权限检查为何只遍历 foreground，以及跨 UID outside 坐标在哪两步被标记和清零。

### 练习 4：推演split资格与pointer ownership

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const bool isFromMouse = entry.source == AINPUT_SOURCE_MOUSE;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'isSplit = !isFromMouse;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '} else if (isSplit) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newTouchedWindowHandle = tempTouchState.getFirstForegroundWindowHandle();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'pointerIds.markBit(pointerId);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (targetFlags & InputTarget::FLAG_SPLIT) {' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'touchedWindow.pointerIds.value |= pointerIds.value;' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
```

要求：分别代入首指、split 后的新指针、不支持 split 的新窗口和 mouse，写出 hit-test 与 pointer ID 归属结果。

### 练习 5：手算splitMotionEvent的局部action

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (inputTarget.flags & InputTarget::FLAG_SPLIT) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (inputTarget.pointerIds.count() != originalMotionEntry.pointerCount) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'MotionEntry* InputDispatcher::splitMotionEvent(' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (splitPointerCount != pointerIds.count()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerIds.hasBit(pointerId)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (pointerIds.count() == 1) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '? AMOTION_EVENT_ACTION_DOWN' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'action = AMOTION_EVENT_ACTION_MOVE;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'int32_t newId = mIdGenerator.nextId();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'touchedWindow.pointerIds.clearBit(pointerId);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：原 ID 集合为 `{0,3}`、变化 pointer 为 3 时，分别计算目标集合 `{0}`、`{3}`、`{0,3}` 在 POINTER_DOWN 与 POINTER_UP 中看到的 action，并解释清 ownership 的时机。

### 练习 6：比较普通MOVE、slippery、Hover与SCROLL

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'if (maskedAction == AMOTION_EVENT_ACTION_MOVE && entry.pointerCount == 1 &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'tempTouchState.isSlippery()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_SLIPPERY_EXIT,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_FOREGROUND | InputTarget::FLAG_DISPATCH_AS_SLIPPERY_ENTER;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'isSplit = true;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newHoverWindowHandle != mLastHoverWindowHandle) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '(InputTarget::FLAG_DISPATCH_AS_IS | InputTarget::FLAG_DISPATCH_AS_SLIPPERY_ENTER)) {' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_CANCEL;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->resolvedAction = AMOTION_EVENT_ACTION_DOWN;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (maskedAction != AMOTION_EVENT_ACTION_SCROLL) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：比较四条路径是否重新命中、当前事件怎样改写、下一 TouchState 保留什么；再指出 slippery 新窗口没有复用 Case 1 的哪些门。

### 练习 7：分开wallpaper、portal与两类monitor

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'touchState->addPortalWindow(windowHandle);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return findTouchedWindowAtLocked(portalToDisplayId, x, y, touchState,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::vector<TouchedMonitor> InputDispatcher::findTouchedGestureMonitorsLocked(' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newGestureMonitors = selectResponsiveMonitorsLocked(newGestureMonitors);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (foregroundWindowHandle && foregroundWindowHandle->getInfo()->hasWallpaper) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'windowHandle->getInfo()->layoutParamsType == InputWindowInfo::TYPE_WALLPAPER) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'addMonitoringTargetLocked(touchedMonitor.monitor, touchedMonitor.xOffset,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::addGlobalMonitoringTargetsLocked(std::vector<InputTarget>& inputTargets,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'addGlobalMonitoringTargetsLocked(inputTargets, windowInfo->portalToDisplayId,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：画出四类附加路由角色的加入时机，解释哪些进入 TouchState、哪一类可让无窗口 DOWN 成功，以及 portal 为何保存路径与偏移。

### 练习 8：追pilfer的oneway调用与资格校验

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public void pilferPointers() {' frameworks/base/core/java/android/view/InputMonitor.java
grep -n -F 'mHost.pilferPointers();' frameworks/base/core/java/android/view/InputMonitor.java
grep -n -F 'public InputMonitor monitorGestureInput(String inputChannelName, int displayId) {' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'if (!checkCallingPermission(android.Manifest.permission.MONITOR_INPUT,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'return new InputMonitor(inputChannels[1], host);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'oneway interface IInputMonitorHost {' frameworks/base/core/java/android/view/IInputMonitorHost.aidl
grep -n -F 'void pilferPointers();' frameworks/base/core/java/android/view/IInputMonitorHost.aidl
grep -n -F 'nativePilferPointers(mPtr, mInputChannel.getToken());' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'static void nativePilferPointers(' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'im->pilferPointers(token);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'status_t NativeInputManager::pilferPointers(const sp<IBinder>& token) {' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'return mInputManager->getDispatcher()->pilferPointers(token);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'status_t InputDispatcher::pilferPointers(const sp<IBinder>& token) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'findGestureMonitorDisplayByTokenLocked(token);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mTouchStatesByDisplay.find(displayId);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!foundDeviceId || !state.down) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：追 token 跨层传递，区分“已注册”“参与当前流”“仍为 down”三道门，并证明 Java API 为什么观察不到 native 的 `OK/BAD_VALUE`。

### 练习 9：验证pilfer的取消、保留状态与split尖角

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'CancelationOptions options(CancelationOptions::CANCEL_POINTER_EVENTS,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '"gesture monitor stole pointer stream");' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'for (const TouchedWindow& window : state.windows) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizeCancelationEventsForInputChannelLocked(channel, options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.filterNonMonitors();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void TouchState::filterNonMonitors() {' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'windows.clear();' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'portalWindows.clear();' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'std::vector<TouchedMonitor> gestureMonitors;' frameworks/native/services/inputflinger/dispatcher/TouchState.h
grep -n -F 'if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!hasWindowHandleLocked(touchedWindow.windowHandle)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'status_t InputDispatcher::unregisterInputChannelLocked(const sp<InputChannel>& inputChannel,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'removeMonitorChannelLocked(inputChannel);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mTouchStatesByDisplay.clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

要求：说明哪些窗口被加入 cancel、哪些 TouchState 字段被保留、为什么不是 pilfer 发起者独占；再推演已 split 流在 pilfer 后新增 pointer 时，窗口怎样可能重新进入。

## 16. 从完整双指场景收束，并形成诊断顺序

在同一个入口 display 上，设 top-to-bottom 依次有观察 outside 的 O、支持 split 且 `hasWallpaper` 的 A、也支持 split 的 B、wallpaper W，另有 gesture monitor M。O、A、B 都是可见且 `NOT_TOUCH_MODAL`：O 的 touchable Region 不含两处触点，A、B 的 Region 互不重叠，id0 命中 A、id1 命中 B。这样上层窗口才会按题设穿透，不能省略这个 hit-test 前提。

第一根手指 id0 落在 A：

```text
O ← ACTION_OUTSIDE（跨 UID 时坐标清零，本次后从状态过滤）
A ← ACTION_DOWN(id0)，成为 foreground + split owner
W ← 带遮挡 flags 的完整 DOWN 副本
M ← 完整 DOWN 副本，并写入 gestureMonitors
```

第二根手指 id1 落在 B：

```text
A ← MOVE(id0)
B ← DOWN(id1)
W ← 未 split 的完整 POINTER_DOWN(id0,id1)
M ← 未 split 的完整 POINTER_DOWN(id0,id1)
```

若 M 随后 pilfer，A、B、W 的 Connection 被加入 pointer cancel，TouchState 清除窗口与 portal、保留 monitors 与流身份；既有 id0/id1 的后续 MOVE/POINTER_UP/UP/CANCEL 继续给 monitor。若这时再落下 id2，则必须记住上一节的 r48 split 尖角，不能假定窗口绝无重新加入的可能。

排查实际路由问题时，按下列顺序收证据：

1. 确认原始 action、action index、pointer IDs、device/source/display。
2. 还原首个 DOWN 当时的窗口 z 序、visible、flags、frame 与 touchable Region。
3. 区分 foreground、outside、wallpaper、global monitor、gesture monitor 五种角色。
4. 查看 TouchState 的 down/split/windows/pointerIds/portal/monitors，而不是用当前坐标猜目标。
5. 对每个 InputTarget 核对 dispatch mode、是否 split、坐标偏移与 obscured flags。
6. 若目标改变，找 slippery、窗口移除、显式 transfer 或 pilfer 的 CANCEL/DOWN 证据。
7. 最后沿上一章的 outbound/waitQueue/finished 链确认“选中了目标”是否真的变成“客户端已完成”。

一份事后 dump 只反映观察时刻；窗口层级和 TouchState 都可能已变化。可靠结论需要把 DOWN 时的窗口快照、每次状态改写、派生 DispatchEntry 与客户端回执放在同一时间轴上。

下一章将继续追软件注入安全：Java/native 入口怎样进入 InputDispatcher，目标 UID 与 `INJECT_EVENTS` 权限怎样裁决，事件签名和 `VerifiedInputEvent` 能证明什么，以及异步/等待模式各自在哪个完成点返回。
