# 221 Android AppTransitionController opening/closing apps 与转场启动条件

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 本章只讨论旧版 `AppTransition` 管线：可以由源码证明某个 Display 何时从 pending 进入 READY、哪些门会阻止统一提交、动画容器怎样从 Activity 向上提升，以及 RUNNING 怎样回到 IDLE；不能据此证明 SurfaceFlinger 已 latch、HWC 已 present，也不能把 Android 12 以后 Shell Transitions 的协议倒灌进来。第 220 章留下的 `allDrawn` 在这里仅是 opening/changing readiness 的一个输入；第 222 章再进入 `AnimationAdapter`、`SurfaceAnimator` 与动画 leash。

Activity 已 resumed、starting window 已显示，甚至真实窗口已 `allDrawn`，页面为什么仍可能不切换？核心不是再找一个“最终 ready”布尔值，而是分清四本账：**待执行 transit、AppTransition 状态、三组参与集合、动画是否仍存在**。它们在一次 surface placement 中汇合，却不是同一个状态机。

## 1. 固定一条普通开页链，先命名九个观察点

先固定场景 `L_open`：默认 Display 上，旧 Activity A 关闭、新 Activity B 打开；二者位于可动画环境；没有 Keyguard、壁纸改写、relaunch、异步 spec、unknown visibility、旋转冲突或异常；B 的真实窗口最终绘制完成；经典本地动画能够创建并正常结束。后文再逐项放宽。

| 点 | 源码侧定义 | 仍不能推出 |
|---|---|---|
| `T_set` | `mNextAppTransition` 不再是 `TRANSIT_UNSET`，5 秒 runnable 已重新计时 | 状态已经 READY |
| `S_join` | B 在 `mOpeningApps`、A 在 `mClosingApps` | 集合成员已经通过 ready 门 |
| `E_ready` | `executeAppTransition()` 令状态成为 READY，并请求 traversal | 动画已绑定 |
| `G_pass` | opening 与 changing 两次 `transitionGoodToGo()` 都返回 true | closing app 已 drawn |
| `A_bind` | opening/closing target 已选出，`applyAnimation()` 已被调用 | Surface 动画已经获准真正起跑 |
| `V_commit` | closing 先 `commitVisibility(false)`，opening 后 `commitVisibility(true)` 并 show windows | transaction 已 present |
| `R_run` | `AppTransition.goodToGo()` 清 pending transit/flags，并写 RUNNING | 远端 runner 的 Binder `onAnimationStart` 已调用 |
| `M_start` | Metrics 消费 `mTempTransitionReasons` | 每次转场都一定有非空 reason |
| `R_idle` | Display 子树已找不到 `isAnimating(PARENTS \| TRANSITION)` 为 true 的 Activity，收尾把状态写回 IDLE | 用户已经看到目标像素 |

固定场景的常见局部顺序是：

```text
T_set < S_join
T_set < E_ready
E_ready < G_pass
G_pass < A_bind < V_commit < R_run < M_start
R_run < R_idle
```

`S_join` 与 `E_ready` 没有值得依赖的通用先后：不同调用链可以先收集参与者，也可以先执行已准备的 transition 后再因 screen-freeze 特例加入 opening。更不能补写 `allDrawn < G_pass` 为全局定律，因为 starting window 与 5 秒 timeout 都能让门继续前进。

## 2. 四个状态、一个 transit 与三组集合是正交账本

`AppTransition` 的整数状态只有四个：

| 状态 | 写点 | 精确含义 |
|---|---|---|
| IDLE | 初值、`prepare()`、动画收尾 | 尚未获准执行，或上一轮已收尾 |
| READY | `setReady()` | surface placement 可以开始检查 |
| RUNNING | `goodToGo()` | 已完成本轮统一提交，等待 Display 的 `isAppTransitioning()` 谓词转 false |
| TIMEOUT | 5 秒 runnable 的有条件分支 | `isReady()` 仍为 true，但普通门会被绕过 |

状态之外，`mNextAppTransition` 单独回答“当前记录了哪种待执行 transit”。因此 `isTransitionSet()` 与 `isReady()` 不能互换：prepare 后通常是“transit 已 set、状态 IDLE”；`goodToGo()` 后则是“transit 已清、状态 RUNNING”。

每个 `DisplayContent` 还维护：

- `mOpeningApps`：元素是 `ActivityRecord`，表示参加 opening 侧；普通延迟路径稍后提交 visible，screen-freeze 特例却可能已经 commit；
- `mClosingApps`：元素也是 `ActivityRecord`，表示参加 closing 侧；普通延迟路径稍后提交 invisible，移除路径却可以先 commit 再入组；
- `mChangingContainers`：元素是 `WindowContainer`，r48 的实际入口可把 `Task` 放进来并冻结起始 surface。

`ActivityRecord.setVisibility(false)` 在 `mVisibleRequested` 已为 false 时有提前返回；它至多补发此前 deferred 的 client-hide，不执行后面的移组与请求状态写入。其余完整路径才先把自己从 opening/closing 两组移除，再写请求状态。只有 `okToAnimate(true)` 且 transit 已 set 时，它才重新加入对应集合并延迟 `commitVisibility()`；否则当场 commit 与更新 reported visibility。可见请求还会先 `setClientVisible(true)`，避免一边等窗口绘制、一边禁止客户端生产窗口。

另有三个容易漏掉的入口：

- transit 未 set、状态却 READY 时，可见 Activity 会加入 opening，用于 screen freeze 解冻等待；
- `TRANSIT_TASK_OPEN_BEHIND` 除 launching Activity 外，还会把当前 focused Activity 强制放入 opening，以便装载动画。
- `onRemovedFromDisplay()` 会先 `commitVisibility(false)`；若随后发现 transit 仍 set，再把 Activity 放入 closing以延迟实体移除。

这些集合是当前批次，不是历史。成功路径会全部清空；Activity 移除、跨 Display 等路径也会主动搬移或删除成员。

## 3. prepare 与 execute 分工，5 秒计时也不是封闭状态图

`prepareAppTransitionLocked()` 先选择或合并 transit，再调用 `prepare()`：

- force override、传入 Keyguard transit、当前未 set、当前为 NONE，或允许 crash-close 覆盖时，直接写新 transit；
- 否则在未要求 always-keep、当前也不是 Keyguard/crash-close 时，TASK_OPEN 可盖 TASK_CLOSE，ACTIVITY_OPEN 可盖 ACTIVITY_CLOSE，Task 级 transit 可盖当前 Activity 级 transit；
- `setAppTransition()` 对 flags 使用按位或，不是每次覆盖清零。

`prepare()` 只有在当前不为 RUNNING 时才把状态写 IDLE、通知 pending listeners 并重置 clip-reveal 统计；RUNNING 时返回 false。这里有一个比“RUNNING 时 prepare 无效”更精确的边界：**transit 选择发生在 `prepare()` 之前**，而且只要选择后仍 `isTransitionSet()`，方法无论 `prepared` 是 true 还是 false，都会移除旧 callback 并重新 post 5 秒 timeout。

`DisplayContent.prepareAppTransition()` 只在 `prepared && okToAnimate()` 时清 `mSkipAppTransitionAnimation`。所以返回值既不代表“transit 一定没被碰”，也不代表“timeout 没被重置”。

`executeAppTransition()` 则只在 transit 已 set 时做两件事：`setReady()` 与 `requestTraversal()`。前者还会启动已登记的异步 animation-spec Future；它不是在 ATMS 当前调用栈里直接选动画或提交可见性。

5 秒 runnable 的行为也不是简单的 READY→TIMEOUT 箭头：

1. 进入 global lock 后先无条件通知 AppTransition timeout listeners；
2. 只有 transit 仍 set，或 opening/closing/changing 任一集合非空，才写 TIMEOUT 并同步 `performSurfacePlacement()`；
3. 若已没有待处理内容，只发生 listener 通知，不改状态；
4. 由于 RUNNING 期间的新 prepare 尝试仍可能重设 transit并重新计时，runnable 到点时并不以“当前必为 READY”为前提。

TIMEOUT 是整批转场的逃生门，不是每个 Activity 各有五秒，也不会撤销仍在执行的 spec Future。

spec Future 还没有批次 generation。worker 只捕获 Future Binder；完成时先把共享 pending bit写 false，再读取当时共享的 future-callback 与 scale-up 字段调用 `overridePendingAppTransitionMultiThumb()`。若此时没有 transit，或当前已是 remote override，结果会被拒绝；若一个更新的非 remote transit 已 set，旧结果却可能写进新批次。更新批次也在取 Future 时复用同一个 pending bit，因此旧 worker完成还可能过早打开新批次的 spec 门。最终无论 override是否接受，代码都会清共享 future-callback并请求 traversal。

## 4. ready 检查位于 transaction 之后，并且短路调用两次

`RootWindowContainer.performSurfacePlacement()` 进入 `performSurfacePlacementNoTrace()`；后者先在一笔 WMS Surface transaction 中执行 `applySurfaceChangesTransaction()`，关闭 transaction并运行 after-prepare-surfaces callbacks，随后才调用 `checkAppTransitionReady()`。它按 Display 的逆 Z 序遍历；当前 Display 的 `isReady()` 为 true时，才进入 controller。

入口形状必须逐字读：

```java
mTempTransitionReasons.clear();
if (!transitionGoodToGo(mDisplayContent.mOpeningApps, mTempTransitionReasons)
        || !transitionGoodToGo(mDisplayContent.mChangingContainers,
                mTempTransitionReasons)) {
    return;
}
```

由短路语义可得到五个结论：

- opening 先检查；它失败时 changing 本轮根本不检查；
- opening 通过、changing 失败时，reason map 可能已经含 opening 的条目；
- 下一轮入口会先 clear，所以失败轮的部分 reason 不会累积；
- closing 不在这两个调用里，因而 closing 是否 `allDrawn` 不阻止启动；
- rotation、spec、unknown visibility 与 wallpaper 这些全局门写在 `transitionGoodToGo()` 内，所以 opening 通过后会在 changing 调用中再检查一次；即使集合为空，这些全局门也照样执行。

这不是“所有三组成员逐个 ready 后一起开跑”，而是“opening 与 changing 的活动门，加上可能重复执行的 Display 级门”。日志若只写“Checking opening apps”，也不能替代实际传入的集合类型。

## 5. 单个 opening/changing 对象有三条通路，reason 只是分类

非 TIMEOUT 时，`transitionGoodToGo(apps, outReasons)` 对每个成员先调用 `getAppFromContainer()`：

- Activity 直接得到自身；
- Task 映射为 `getTopNonFinishingActivity()`；
- 得不到 Activity 的容器被跳过，不形成 reason，也不阻塞。

得到 Activity 后，真实窗口通路不是裸 `allDrawn`，而是：

```java
allDrawn = activity.allDrawn && !activity.isRelaunching();
ready = allDrawn || activity.startingDisplayed || activity.startingMoved;
```

三者都为 false 才立即返回。reason 的写法则是：

| ready 通路 | 写入值 | 不能据此反推 |
|---|---|---|
| `allDrawn && !isRelaunching()` | `APP_TRANSITION_WINDOWS_DRAWN` | buffer 已 present |
| 非 allDrawn，且 starting data 是 `SplashScreenStartingData` | `APP_TRANSITION_SPLASH_SCREEN` | 真实主窗口已完成 |
| 其余 starting 通路 | `APP_TRANSITION_SNAPSHOT` | 当前一定存在可见 snapshot surface |

最后一行尤其重要：代码在“不是 SplashScreenStartingData”时统一归为 SNAPSHOT；`startingMoved=true`、starting data 已变化等边界都可能得到这个标签。它是 Metrics 原因枚举，不是对当前 surface 类型的强证明。

closing 没有经过这张表。旧页面未完整绘制也可以参加退出动画；稍后的 closing 提交甚至会主动把其 `allDrawn` 写 true，这只是退出流程控制，不是补出绘制证据。

## 6. Activity 门与四类全局门按固定顺序检查

每次非 TIMEOUT 调用的顺序是：

1. 取**默认 Display** 的 `ScreenRotationAnimation`；若它正在动画，且**当前 Display** 的 `DisplayRotation.needsUpdate()` 为 true，返回 false；
2. 检查本次 apps 中的 Activity 三通路；
3. 若 animation specs Future 仍 pending，返回 false；
4. 若 unknown-app visibility map 非空，返回 false；
5. 若 `isWallpaperVisible()` 为 false则通过；它为 true时要求 `wallpaperTransitionReady()`。

这里混用了默认 Display 的 rotation animation 与当前 Display 的 needs-update，是 r48 的实现事实，多屏排障不能擅自改写成“同一 Display 的两个条件”。

这里的方法名也比事实强：r48 的 `isWallpaperVisible()` 只判断当前 `mWallpaperTarget` 或 `mPrevWallpaperTarget` 是否非 null，并不读取 wallpaper window 的实际 shown/present 状态。

unknown visibility 没有独立超时。锁屏后启动的 Activity 正常依次经过 WAITING_RESUME → WAITING_RELAYOUT → WAITING_VISIBILITY_UPDATE；只有 relayout 发生在正确前态才请求更新 Keyguard flags，最终 callback 删除处于第三态的项并主动做一次 surface placement。移除或隐藏 Activity 可直接删项。若全局 5 秒先到，ready 成功路径会把整个 controller 清空，未走完三态的项也不再挡本轮。

壁纸是另一套三态与 500ms 定时器。首次发现“应可见但未 drawn”的 wallpaper 时从 NORMAL 进 PENDING并发消息；到时只在仍为 PENDING 时改 TIMEOUT，并触发 surface placement。TIMEOUT 状态下，即使 wallpaper 仍未 drawn，`wallpaperTransitionReady()` 也允许转场继续；该状态没有 transition generation，可以跨到后续批次，直到某次检查发现 wallpaper 全部 ready，才恢复 NORMAL并移除消息。

AppTransition 自身的 5 秒 TIMEOUT 更强：`transitionGoodToGo()` 直接返回 true，以上 rotation、Activity、spec、unknown、wallpaper 检查以及 reason 写入全部跳过。于是两次调用都会通过，入口刚清过的 `mTempTransitionReasons` 保持为空；后面的 Metrics 通知不会凭空制造 transition-start 条目。

## 7. 通过门后先消费一次性状态，这段不是可回滚预演

两次 good-to-go 都成功后，controller 才开始真正处理本批。顺序如下：

1. 读取 pending transit 到局部变量；
2. 若 `mSkipAppTransitionAnimation` 为 true且不是 Keyguard-going-away transit，先把局部 transit 置 UNSET；
3. 无条件清 skip flag与 `mNoAnimationNotifyOnTransitionFinished`；
4. 移除 AppTransition 的 5 秒 callback；
5. 清 `mWallpaperMayChange`；
6. 对 opening Activity 清 animating flags；对 changing 容器映射出的 Activity 也清；
7. 调整本 Display 的 wallpaper windows，再计算 opening/closing 是否可作 wallpaper target；
8. 依次做 translucent rewrite 与 wallpaper rewrite；
9. 选择 activity types、anim-LP owner、三组 top app与可能的 remote override；
10. defer SurfaceAnimationRunner 的动画启动，进入 apply/commit/goodToGo 主体。

几个写操作在动画真正绑定前就已发生，所以不能把这段叫作“纯检查”。源码的 `try/finally` 只包围 defer 之后的主体，并保证 `continueStartingAnimations()`；它没有建立一份可恢复快照，也没有在任意异常后自动还原已经清掉的 flag、timeout 或 animating flags。

`mNoAnimationNotifyOnTransitionFinished` 在这里先清空，是为了开启新的 transition-finish 补偿区间。它稍后可由 opening 的“没有当前动画 source”分支填入，也可在 Activity 移除且自身/祖先有任意类型动画或 waiting-to-start 时填入；正常收尾统一消费。

## 8. transit 会连续改写，skip 也不保证最终没有动画

第一轮是 translucent rewrite。change transit 原样返回；其余只有 Task/Activity transit 才考虑：

- closing 非空、每个 closing 都不 `fillsParent()`，并且所有 opening 已经 visible时，改成 `TRANSIT_TRANSLUCENT_ACTIVITY_CLOSE`；
- opening 非空，所有“尚不可见的 opening”都不 `fillsParent()`，且 closing 为空时，改成 `TRANSIT_TRANSLUCENT_ACTIVITY_OPEN`。

第二轮才是 wallpaper rewrite。NONE、crash-close、dock-from-recents 与 change transit会直接保留；其余根据 wallpaper target、opening/closing target 属性、top app与原 transit，可能变成 GOING_AWAY_ON_WALLPAPER、WALLPAPER_INTRA_OPEN/CLOSE、WALLPAPER_OPEN 或 WALLPAPER_CLOSE。

两处不符合直觉的边界值得单列：

- skip 分支写的是局部 `TRANSIT_UNSET`，但 wallpaper rewrite 的提前返回列表**没有 UNSET**；若 old/new wallpaper 条件成立，UNSET 仍可被改成 WALLPAPER_OPEN/CLOSE，因此“skip=true必然让 `applyAnimations()` 早退”不成立；
- wallpaper 方法的总保护只针对 `isKeyguardGoingAwayTransit()` 两种 going-away 值；不能把它扩大成所有 Keyguard transit。OCCLUDE/UNOCCLUDE 没有同样的 blanket guard。

最后写入 `setLastAppTransition()`、传入动画与 listeners 的都是这个局部最终 transit；最初 prepare 的类型只是候选起点。

## 9. animLp owner、remote definition 与 voice flag 在 target 提升前决定

controller 先把 opening、closing、changing 三组的 activity type 放入 `ArraySet<Integer>`，再以 prefix-order index 选择 `animLpActivity`。三层筛选依次是：

1. 具有与最终 transit/activity-types 匹配的 remote animation definition；
2. `fillsParent()` 且能找到 main window；
3. 能找到 main window。

每层都会在 closing、opening、changing 的合并候选中取 prefix-order 最高者。changing 项若是 Task，排名使用原 Task 的 prefix-order index，filter与最终返回身份才映射为 top non-finishing Activity。第一层不要求 main window，所以 remote 命中者可以令 `getAnimLp()` 返回 null；普通本地动画才应把 main-window LayoutParams 理解成 theme/style 输入。

接下来 `overrideWithRemoteAnimationIfSet()` 仍发生在 animation targets 计算之前。它查的是 **`animLpActivity` 自己的 definition**，未命中再查 controller 的 Display 级 definition；crash-close 明确不允许这次覆盖。稍后可能提升出的 Task 或 TaskDisplayArea 不是这一步的一级查找对象。

r48 还有一处必须忠实保留的重复判断：

```java
containsVoiceInteraction(mOpeningApps)
        || containsVoiceInteraction(mOpeningApps)
```

两侧都是 opening，并没有检查 closing。于是“只有 closing app 属于 voice interaction”的批次会得到 false。文章应把它标为该版本实现，而不是按变量名脑补成 opening-or-closing。

远端 adapter 在这里仅被登记到 pending transition。`AppTransition.goodToGo()` 随后只是进入 `RemoteAnimationController.goodToGo()`；真正的 runner Binder `onAnimationStart()` 被安排到 after-prepare-surfaces runnable，且无 target、已取消等分支可以直接 finish，根本不调用 runner。两者不是同一个完成点。

## 10. 动画 target 从 Activity 向上提升，要同时通过三类阻断

opening 与 closing 分开计算 target。候选 Activity 先通过 `shouldApplyAnimation(visible)`：

```text
当前isVisible与目标visible不同
或 当前不可见且mIsExiting
或 这是opening且任一窗口waitingForReplacement
```

未开启 hierarchical animations 时，候选 Activity 直接成为 targets。开启后，每一侧还会构造“另一侧所有 Activity 及其全部祖先”集合，然后循环尝试把 current 提升为 parent。

一次提升必须同时满足：

- parent 存在且 `canCreateRemoteAnimationTarget()`；
- parent 不在 other-side ancestors 中，否则 opening/closing 会被揉进同一 target；
- parent 的每个**直接 child**要么就是 current，要么也在 candidates 中，要么不可见；任何可见但不参动的 sibling 都会阻止提升。

扫描直接 children 时，代码会把候选 sibling 从链表移出并放入临时 siblings。若能提升，只把 parent 重新入队；若不能，则把 current 与已收拢的 candidate siblings一起放入最终 targets。parent 还可继续向 Task、TaskDisplayArea 等更高层尝试，直到某一层失败。

因此“同 Task 内所有 opening Activity”仍不足以推出动画一定提升到 Task：另一侧后代、可见非候选 sibling、父容器能力任一项都能截断。反过来，不可见且非候选的 sibling 不会单独阻止提升。

## 11. apply 先建立 source 归属，再统一放行动画启动

若最终 transit 仍是 UNSET，或 opening 与 closing 同时为空，`applyAnimations(opening, closing, ...)` 直接返回。changing-only 批次仍会在后面的独立路径应用 change animation，但不会经过这段 opening/closing accessibility 通知。

正常路径先算 opening targets，再算 closing targets。对每个 target，controller 遍历本侧 apps，收集所有 `isDescendantOf(target)` 的 Activity 作为 `transitioningDescendants`，再调用：

```java
target.applyAnimation(
        animLp, transit, visible, voiceInteraction, transitioningDescendants);
```

这份 sources 很关键：动画若提升到父容器，`SurfaceAnimator` 只会在父 target 上触发容器完成；sources 让各参与 Activity 仍能收到自己的 transition-finished 处理。它也被 opening 提交流程用来判断某个 token 是否确实由当前动画覆盖。

外层在 apply 前调用 `SurfaceAnimationRunner.deferStartingAnimations()`，在 `finally` 中 `continueStartingAnimations()`。所以 `applyAnimation()` 已装好 adapter 不等于动画 runner 已在调用点同步起跑；controller 刻意先把两侧动画、可见性和回调整批装配完再放行。

## 12. 主体顺序固定为 apply、closing、opening、changing

defer 区间内的顺序不是 opening 优先：

```text
applyAnimations(opening, closing)
handleClosingApps()
handleOpeningApps()
handleChangingApps()
setLastAppTransition()
goodToGo()
handleNonAppWindowsInTransition()
postAnimationCallback()
clear()
finally continueStartingAnimations()
```

每个 closing Activity 依次执行：

1. `commitVisibility(false, false)`；
2. `updateReportedVisibilityLocked()`；
3. 强制 `allDrawn=true`；
4. 若 starting window 存在且没有 animating-exit，移除它；
5. 若配置 thumbnail-down，附加 thumbnail animation。

每个 opening Activity 的顺序则是：

1. `commitVisibility(true, false)`；
2. 用 `getAnimatingContainer(PARENTS, ANIMATION_TYPE_APP_TRANSITION)` 找动画容器；
3. 容器为空，或其 animation sources 不含当前 Activity 时，把 token 放入 no-animation-finish 列表；
4. `updateReportedVisibilityLocked()`；
5. 清 `waitingToShow`；
6. 为这个 Activity 单独 open Surface transaction，调用 `showAllWindowsLocked()`，再 close；
7. 按 pending override 附加 thumbnail-up 或 cross-profile thumbnail。

这再次证明第 220 章的 `nowVisible` 不是 Surface show 回执：opening 先更新 reported visibility，后调用 `showAllWindowsLocked()`。changing 容器不走 visible commit，只执行 `applyAnimation(null, transit, true, false, null)`。

## 13. goodToGo 是统一提交点，但后处理仍有严格先后

三组主体处理完后，`setLastAppTransition()` 保存最终 transit 与各组 top app 的字符串。接着先抓取 flags，再调用 `AppTransition.goodToGo()`：

- 将 pending transit 写 UNSET、flags 清零；
- 把状态写 RUNNING；
- 从 top opening app 当前 animating container 取 animation adapter，用其 duration hint与 status-bar start time通知 AppTransition listeners；
- 若有 `RemoteAnimationController`，调用它的 `goodToGo()`；
- 返回 listeners 请求的 layout-redo 位。

flags 必须在 `goodToGo()` 前保存，因为方法内部会清零；controller 随后才据旧 flags处理 Keyguard wallpaper与 non-app windows。之后 `postAnimationCallback()` 把 started callback投到 Handler并置 null，`clear()` 清 override type、package、spec、remote controller、尚未被 worker取走的 Future引用与 finished callback；它不清正在执行的 worker、共享 spec-pending bit、future-callback或scale-up字段，也**不把状态写回 IDLE**。

finally 放行动画后，成功路径继续：

1. 通知 `TaskSnapshotController.onTransitionStarting()`；
2. 清 opening、closing、changing 与 unknown-visibility map；
3. 标记 layout needed并重算 IME target；
4. 把 reason map交给 `ActivityMetricsLogger.notifyTransitionStarting()`；
5. single-task-display 特例登记 after-prepare-surfaces callback；
6. 合并 listener redo、LAYOUT与CONFIG位。

TIMEOUT 路径的 reason map可以为空。Metrics 方法只遍历 map，空 map意味着这次调用不会把任何 `TransitionInfo.mLoggedTransitionStarting` 写 true；这与 AppTransition listeners 已收到 starting 是两套账，也解释了第 220 章的启动计时门为何不能用 RUNNING 状态替代。

## 14. RUNNING 的终点看精确谓词；无命中时可同轮进入 IDLE

`checkAppTransitionReady()` 在调用 ready handler 后，紧接着判断：

```java
curDisplay.mAppTransition.isRunning()
        && !curDisplay.isAppTransitioning()
```

`WindowContainer.isAppTransitioning()` 在子树中寻找 `app.isAnimating(PARENTS | TRANSITION)` 为 true 的 Activity。这个单参数重载检查 `ANIMATION_TYPE_ALL`，所以 Activity自身或祖先上的任意 SurfaceAnimator animation type 都可能命中，不限 APP_TRANSITION 类型；`TRANSITION` flag还把 `isWaitingForTransitionStart()` 计为 true。成功 handler已经清掉 pending transit与三组集合时，这个 waiting贡献通常消失，但 RUNNING 期间又准备的新批次仍可能重新引入它。

如果 skip 后 transit 保持 UNSET、候选全被 `shouldApplyAnimation()` 排除、动画装载失败，或本批只有没有令上述精确谓词为 true 的工作，并且没有其他类型动画/新 pending批次使它命中，`goodToGo()` 刚写 RUNNING后，第二个 if 就可能在**同一次 `checkAppTransitionReady()` 调用**里立即收尾。

`DisplayContent.handleAnimatingStoppedAndTransition()` 的顺序是：

1. `mAppTransition.setIdle()`；
2. 为补偿列表中的每个 token通知 transition finished，再清列表；
3. 隐藏 deferred wallpapers；
4. 调用 `onAppTransitionDone()`；
5. 加 LAYOUT redo，重算 IME target；
6. 令 `mWallpaperMayChange=true`、`mFocusMayChange=true`。

由当前 target覆盖的 Activity 依靠 animation sources与容器完成回调收尾。补偿列表有两个明确 producer：opening Activity不在当前动画 sources 中，以及 Activity被移除时 `isAnimating(TRANSITION | PARENTS)` 为 true；所以它既不是 opening-only，也不能仅按字段名理解为“绝无动画”。若精确谓词仍为 true，未来相关动画或 waiting状态消失后触发的 traversal/surface placement，才会再次判断并执行上述步骤。

## 15. 九个只读练习把结论钉回 r48 源码

以下命令只读取本地源码；每个 `rg -e` 都应独立命中。建议先预测结果，再看上下文，而不是把命中行号当作完整时序。

### 练习 1：分开状态、prepare、execute 与五秒计时

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'private static final long APP_TRANSITION_TIMEOUT_MS = 5000;' \
  -e 'private final static int APP_STATE_IDLE = 0;' \
  -e 'private final static int APP_STATE_TIMEOUT = 3;' \
  -e 'boolean prepared = prepare();' \
  -e 'mHandler.postDelayed(mHandleAppTransitionTimeoutRunnable, APP_TRANSITION_TIMEOUT_MS);' \
  -e 'fetchAppTransitionSpecsFromFuture();' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
rg -n -F \
  -e 'mAppTransition.setReady();' \
  -e 'mWmService.mWindowPlacerLocked.requestTraversal();' \
  frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
```

解释 RUNNING 时 `prepare()` 返回 false，为什么仍不能推出 transit 与 timeout 保持原样。

### 练习 2：追 opening、closing 与 changing 的入组条件

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'if (!visible && !mVisibleRequested) {' \
  -e 'displayContent.mOpeningApps.remove(this);' \
  -e 'displayContent.mClosingApps.remove(this);' \
  -e 'if (okToAnimate(true /* ignoreFrozen */) && appTransition.isTransitionSet()) {' \
  -e 'displayContent.mOpeningApps.add(this);' \
  -e 'displayContent.mClosingApps.add(this);' \
  -e 'commitVisibility(false /* visible */, true /* performLayout */);' \
  -e 'getDisplayContent().mClosingApps.add(this);' \
  -e 'commitVisibility(visible, true /* performLayout */);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F \
  -e 'mDisplayContent.mChangingContainers.add(this);' \
  -e 'mSurfaceFreezer.freeze(getPendingTransaction(), startBounds);' \
  frameworks/base/services/core/java/com/android/server/wm/Task.java
```

分别写出“延迟 commit”“立即 commit”和 change-transition freeze 的必要条件。

### 练习 3：验证 placement 入口与两次短路检查

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mWmService.mAnimator.executeAfterPrepareSurfacesRunnables();' \
  -e 'checkAppTransitionReady(surfacePlacer);' \
  -e 'if (curDisplay.mAppTransition.isReady()) {' \
  -e 'curDisplay.mAppTransitionController.handleAppTransitionReady();' \
  frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
rg -n -F \
  -e 'mTempTransitionReasons.clear();' \
  -e 'transitionGoodToGo(mDisplayContent.mOpeningApps, mTempTransitionReasons)' \
  -e 'mDisplayContent.mChangingContainers,' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

指出 opening 失败、changing 失败、两组都为空时，哪些全局门实际被执行几次。

### 练习 4：手算 Activity ready 与 reason

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'wc.asTask().getTopNonFinishingActivity()' \
  -e 'final boolean allDrawn = activity.allDrawn && !activity.isRelaunching();' \
  -e '!activity.startingDisplayed && !activity.startingMoved' \
  -e 'outReasons.put(activity, APP_TRANSITION_WINDOWS_DRAWN);' \
  -e 'activity.mStartingData instanceof SplashScreenStartingData' \
  -e '? APP_TRANSITION_SPLASH_SCREEN' \
  -e ': APP_TRANSITION_SNAPSHOT' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

至少构造四例：relaunching+allDrawn、startingDisplayed、startingMoved、Task无top Activity，并分别预测 pass与reason。

### 练习 5：按源码顺序定位四类全局阻塞与两个重试器

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'screenRotationAnimation.isAnimating()' \
  -e 'mDisplayContent.getDisplayRotation().needsUpdate()' \
  -e 'isFetchingAppTransitionsSpecs()' \
  -e 'mUnknownAppVisibilityController.allResolved()' \
  -e 'mWallpaperControllerLocked.wallpaperTransitionReady()' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
rg -n -F \
  -e 'UNKNOWN_STATE_WAITING_RESUME' \
  -e 'UNKNOWN_STATE_WAITING_RELAYOUT' \
  -e 'UNKNOWN_STATE_WAITING_VISIBILITY_UPDATE' \
  -e 'mService.mWindowPlacerLocked.performSurfacePlacement();' \
  frameworks/base/services/core/java/com/android/server/wm/UnknownAppVisibilityController.java
rg -n -F \
  -e 'WALLPAPER_DRAW_PENDING_TIMEOUT_DURATION = 500;' \
  -e 'return wallpaperTarget != null || mPrevWallpaperTarget != null;' \
  -e 'mWallpaperDrawState = WALLPAPER_DRAW_PENDING;' \
  -e 'mWallpaperDrawState = WALLPAPER_DRAW_TIMEOUT;' \
  frameworks/base/services/core/java/com/android/server/wm/WallpaperController.java
```

区分“状态变化会主动请求下一轮”与“只能依赖别的 traversal”的情况。

### 练习 6：核对 skip、两次 rewrite、remote owner 与 voice 实现

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'transit = WindowManager.TRANSIT_UNSET;' \
  -e 'transit = maybeUpdateTransitToTranslucentAnim(transit);' \
  -e 'transit = maybeUpdateTransitToWallpaper(transit, openingAppHasWallpaper,' \
  -e 'isKeyguardGoingAwayTransit(transit)' \
  -e 'findAnimLayoutParamsToken(transit, activityTypes);' \
  -e 'getRemoteAnimationOverride(animLpActivity, transit, activityTypes);' \
  -e 'containsVoiceInteraction(mDisplayContent.mOpeningApps)' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

说明为何 UNSET 仍可能变成 wallpaper transit，并判断 closing-only voice interaction 得到什么值。

### 练习 7：在一棵小树上手推 target 提升

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'return isVisible() != visible || (!isVisible() && mIsExiting)' \
  -e 'visible && forAllWindows(WindowState::waitingForReplacement, true)' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
rg -n -F \
  -e 'final LinkedList<WindowContainer> candidates = new LinkedList<>();' \
  -e '!parent.canCreateRemoteAnimationTarget()' \
  -e 'otherAncestors.contains(parent)' \
  -e 'if (candidates.remove(sibling)) {' \
  -e 'else if (sibling != current && sibling.isVisible()) {' \
  -e 'candidates.add(parent);' \
  -e 'targets.addAll(siblings);' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
```

给同一 parent 放入“另一侧 child”“可见非候选 child”“不可见非候选 child”，分别推导 target停在哪一层。

### 练习 8：逐行确认 apply 后的三组处理顺序

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'handleClosingApps();' \
  -e 'handleOpeningApps();' \
  -e 'handleChangingApps(transit);' \
  -e 'app.commitVisibility(false /* visible */, false /* performLayout */);' \
  -e 'app.allDrawn = true;' \
  -e 'app.commitVisibility(true /* visible */, false /* performLayout */);' \
  -e 'app.getAnimatingContainer(PARENTS,' \
  -e 'mDisplayContent.mNoAnimationNotifyOnTransitionFinished.add(app.token);' \
  -e 'app.showAllWindowsLocked();' \
  -e 'wc.applyAnimation(null, transit, true, false, null /* sources */);' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
rg -n -F \
  -e 'if (isAnimating(TRANSITION | PARENTS)) {' \
  -e 'getDisplayContent().mNoAnimationNotifyOnTransitionFinished.add(token);' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
```

给每行标注它改变的是 visible账、draw控制位、Surface show、动画归属还是 finish补偿，并区分补偿列表的两个 producer。

### 练习 9：验证 RUNNING、Metrics 与同轮无动画收尾

```bash
set -eu
cd /Users/ninebot/androidSource
rg -n -F \
  -e 'mNextAppTransition = TRANSIT_UNSET;' \
  -e 'setAppTransitionState(APP_STATE_RUNNING);' \
  -e 'mRemoteAnimationController.goodToGo();' \
  frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
rg -n -F \
  -e 'curDisplay.mAppTransition.isRunning() && !curDisplay.isAppTransitioning()' \
  frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
rg -n -F \
  -e 'return isAnimating(flags, ANIMATION_TYPE_ALL);' \
  -e 'return getActivity(app -> app.isAnimating(PARENTS | TRANSITION)) != null;' \
  -e '&& isWaitingForTransitionStart()) {' \
  frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
rg -n -F \
  -e 'mAppTransition.setIdle();' \
  -e 'mAppTransition.notifyAppTransitionFinishedLocked(token);' \
  -e 'mWallpaperController.hideDeferredWallpapersIfNeeded();' \
  frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
rg -n -F \
  -e 'for (int index = activityToReason.size() - 1; index >= 0; index--) {' \
  -e 'info.mLoggedTransitionStarting = true;' \
  frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
```

分别模拟“有本地动画”“transit保持UNSET”“TIMEOUT且reason map为空”，不要把三种完成路径合成一个时间点。

## 16. 排障矩阵、源码地图与下一章边界

| 现象 | 第一证据点 | 常见误判 | 下一步 |
|---|---|---|---|
| 状态长期 READY | `transitionGoodToGo()` 首个 return false | closing没画完 | 按 rotation→本组Activity→spec→unknown→wallpaper 顺序定位 |
| opening原因有记录却仍未启动 | changing 的第二次检查 | reason map代表整批已通过 | 查看 changing Activity或第二遍全局门 |
| 五秒后转场启动、Displayed计时仍悬空 | TIMEOUT分支与空 reason map | RUNNING等于Metrics已starting | 回到 `TransitionInfo.mLoggedTransitionStarting` 与 pending-draw双门 |
| 旧页面未allDrawn仍退出 | ready入口没有closing | 所有参与者都要drawn | 检查opening/changing即可 |
| 设置skip仍看到壁纸动画 | UNSET后的wallpaper rewrite | skip是最终transit锁 | 重建old/new wallpaper条件 |
| remote动画来源看似选错 | `animLpActivity` 与Display definition | promoted target先决定remote | 先还原三级owner筛选，再到第223章核对target注册 |
| closing-only voice场景走普通参数 | opening OR opening | 变量名意味着两组都查 | 按r48实现记录，并评估版本修复 |
| RUNNING几乎立刻变IDLE | 同一方法中的第二个if | 至少会维持一帧RUNNING | 按 `isAnimating(PARENTS \| TRANSITION)` 与全 animation-type 范围重建谓词 |
| opening token收不到常规动画完成 | animation sources与no-animation列表 | target有动画就覆盖所有后代 | 核对该Activity是否在sources中 |
| unknown visibility一直阻塞 | controller debug map中的三态 | relayout可从任意前态跳转 | 核对resume→relayout→visibility-update顺序 |
| 壁纸约半秒后放行 | wallpaper PENDING/TIMEOUT | AppTransition五秒先到 | 区分500ms壁纸门与5000ms整批门 |

源码导航：

```text
frameworks/base/services/core/java/com/android/server/wm/AppTransition.java
frameworks/base/services/core/java/com/android/server/wm/AppTransitionController.java
frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java
frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java
frameworks/base/services/core/java/com/android/server/wm/ActivityRecord.java
frameworks/base/services/core/java/com/android/server/wm/Task.java
frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java
frameworks/base/services/core/java/com/android/server/wm/UnknownAppVisibilityController.java
frameworks/base/services/core/java/com/android/server/wm/WallpaperController.java
frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
frameworks/base/services/core/java/com/android/server/wm/ActivityMetricsLogger.java
frameworks/base/services/core/java/com/android/server/wm/RemoteAnimationController.java
```

本章最小心智模型是：`mNextAppTransition` 选择候选类型；READY/TIMEOUT 决定是否进入门检查；opening与changing决定能否启动，closing参加动画、提交与移除延期；target提升决定动画挂在哪个容器，sources补回Activity完成归属；RUNNING最终由 Display 子树的精确 `isAppTransitioning()` 谓词结束。任何一个点都不是物理present证明。

下一章进入第 222 章“Android AppTransition动画加载、AnimationAdapter、SurfaceAnimator 与动画 Leash”，继续追 `WindowContainer.applyAnimation()` 怎样选择 adapter、`SurfaceAnimator` 怎样创建 leash并把开始与完成回调接回容器；远端 runner 协议留到第 223 章。
