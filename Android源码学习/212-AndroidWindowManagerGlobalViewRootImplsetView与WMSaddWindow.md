# 212 Android WindowManagerGlobal：ViewRootImpl.setView 与 WMS addWindow

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只读源码，可以证明本地顶层窗口账、同步 Binder、服务端 WindowState、输入通道与首次 traversal 的源码顺序；不能据此测量某台设备的 add 延迟，也不能把窗口登记、容器层 SurfaceControl、可绘制 Surface、首个 Buffer 与屏幕 present 合并成一个完成点。

第 211 章已经把业务 XML 同步挂进 `PhoneWindow` 的 content，但 Decor 还没有接入窗口系统。普通首次 resume 会走到：

```java
a.mWindowAdded = true;
wm.addView(decor, l);
```

本章只追一个问题：**这次 `WindowManager.addView()` 正常返回时，App 与 system_server 各自已经提交了什么；为什么 Decor 有了 ViewRoot parent 和 WMS WindowState，仍不能说 `onAttachedToWindow()`、可绘制 Surface 或首帧已经完成？**

## 1. 固定普通首次主窗口，用十八个完成点回答“add 已完成”

先固定 `B_target`：

| 维度 | 固定值或前提 |
|---|---|
| 上游 | 普通 Activity 首次 `handleResumeActivity()`；`performResumeActivity()` 成功，未进入待销毁集合；无待清理旧 Window，`r.newConfig==null` |
| 可见门 | Activity 未 finish、没有因启动另一 Activity 被隐藏，`willBeVisible=true`、`mVisibleFromClient=true` |
| 本地窗口 | `r.window==null`、非 preserved；PhoneWindow、Decor 与业务子树已存在，Decor 尚无 parent/AttachInfo |
| 参数 | 主窗口 `TYPE_BASE_APPLICATION`；Activity token 有效；无兼容缩放、手动 Surface 接管与特殊输入队列 |
| 身份 | 同用户、可访问 Display；不是子窗口、Toast、IME、Overlay 或其他系统窗口 |
| 服务端 | Display ready；对应 ActivityRecord 仍在容器树；权限、Policy 与死亡监听均通过；没有重复 IWindow |
| 输入 | 未设置 `INPUT_FEATURE_NO_INPUT_CHANNEL`，InputChannel 创建与客户端 receiver 均成功 |
| 可重入 | 从 `D_hide` 到 `D_show` 的 RootViewSurfaceTaker、PendingInsets、无障碍等同步可控回调均正常返回，且不改 Decor visibility/parent、Activity `mWindowAdded` 或窗口账 |
| 并发 | 从 `D_hide` 到 `D_show` 无其他线程 add/remove/update 目标 Decor/Window，也不并发修改其 visibility/parent、`mWindowAdded` 或本地窗口账 |
| 返回 | `addToDisplayAsUser()` 返回非负成功位，包含 App 可见；后续 makeVisible、首次 relayout 与首帧也正常 |

十八个完成点如下：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `H_gate` | resume 后续 UI 门已通过 | Decor 已交给 WindowManager |
| `D_hide` | Decor 已设为 INVISIBLE，type 已设为 BASE，Activity 已先写 `mWindowAdded=true` | 本地三联账已建立 |
| `G_book` | WindowManagerGlobal 已创建 ViewRootImpl，并追加 View/Root/Params 三联账 | 首次 traversal 已排队 |
| `T_queue` | ViewRootImpl 已写 `mAdded=true` 并排好 traversal barrier/callback | measure 已开始 |
| `B_enter` | App 主线程进入同步 `IWindowSession.addToDisplayAsUser()` | WMS 已接受窗口 |
| `W_gate` | permission、Display、user、重复 IWindow 与 Activity token 门均通过 | WindowState 已进入正式账本 |
| `W_state` | WindowState 已构造、复制 attrs、链接死亡监听，Policy 校验也通过 | 输入通道或提交点已到 |
| `I_server` | WMS 已创建 InputChannel pair，注册服务端端点并转移客户端端点 | 新窗口已经可收输入 |
| `C_commit` | 执行越过“此后不允许错误”的意图提交线 | 全部服务端挂账已做完 |
| `W_link` | Session、mWindowMap 与 Activity token 层级已挂账，Policy 后置调用、初始 Insets 和输入更新请求等已完成 | App 已收到回复 |
| `B_return` | 同步 Binder 回复把非负结果、frame hint、Insets、controls 与客户端 channel 带回 App | Decor 已成为 ViewRoot 的 child |
| `I_client` | App 已用客户端 channel 创建主 Looper 的 InputEventReceiver | 输入 stage 已全部串好 |
| `P_parent` | `view.assignParent(ViewRootImpl)` 已写入 Decor 的 `mParent` | View 已 attached |
| `V_ready` | touch/app flags、无障碍入口、输入 stage 链与 Decor 的 PendingInsets 重放已完成，`setView()` 正常返回 | 外层 add 已返回 |
| `G_return` | `WindowManagerGlobal.addView()` 与 void `WindowManager.addView()` 正常返回 | Decor 已改为 VISIBLE |
| `D_show` | 同一主线程稍后执行 `Activity.makeVisible()`，Decor 改为 VISIBLE | WMS 已收到 VISIBLE relayout |
| `T_relayout` | 首次 traversal 已 dispatch attach、measure，并以 VISIBLE 完成 relayout、取得可绘制 Surface | draw 或 Buffer present 已完成 |
| `F_present` | 首个目标可见 Buffer 达到所选 present 证据 | 较早 add 没有性能问题 |

固定路径的局部全序是：

```text
H_gate < D_hide < G_book < T_queue < B_enter
       < W_gate < W_state < I_server < C_commit < W_link
       < B_return < I_client < P_parent < V_ready < G_return
       < D_show < T_relayout < F_present
```

`G_return` 没有 View 返回值；外层 API 是 `void`。它证明 App 的顶层根账、WMS 的窗口账和输入连接已经正常建立，第一次 traversal 也已经排队。此刻 Decor 仍是 INVISIBLE，View 自身仍无 AttachInfo；没有 measure、relayout、可承载 Buffer 的窗口 Surface、draw 或 present。

## 2. 四本对象账与四类身份同时出现，但不能互相替代

一笔 add 跨过四本账：

| 账本 | 进程 | 关键对象 | 表达什么 |
|---|---|---|---|
| View 树 | App | Decor → content → 业务 View | 应用 UI 父子关系 |
| 顶层窗口账 | App | WindowManagerGlobal 的 `mViews/mRoots/mParams` | 每棵顶层树对应哪个 ViewRootImpl 与请求参数 |
| 根控制账 | App | ViewRootImpl、AttachInfo、IWindow、IWindowSession | traversal、Insets、输入与窗口协议 |
| 窗口容器账 | system_server | DisplayContent → ActivityRecord/WindowToken → WindowState | 跨应用层级、Policy、输入与 Surface 管理 |

ViewRootImpl 不是 View，也不是 system_server 的 WindowState。它实现 ViewParent，在 App 内控制一棵顶层 View 树；WMS 不能持有 Decor 的 Java 引用，只能持有 IWindow Binder、Parcelable 参数和服务端状态。

同一路径还有四类不能串号的身份：

| 身份 | 固定路径来源 | 服务对象 |
|---|---|---|
| Activity token / `attrs.token` | system_server 创建后随 Activity 代际交给 App | 找到既有 ActivityRecord/WindowToken |
| IWindow / `ViewRootImpl.W` Binder | 每个 ViewRootImpl 新建 | 标识具体客户端窗口，也是 WMS→App 回调端点与 `mWindowMap` key |
| IWindowSession | `openSession()` 返回，普通进程由 WindowManagerGlobal 缓存 | 承载 add/relayout/remove，并保存客户端 uid/pid 与窗口计数 |
| InputChannel token | WMS 创建的 channel pair | InputDispatcher 路由和 `mInputToWindowMap` 身份 |

顶层 Activity token 指“这扇窗口属于哪一代 Activity”；IWindow 指“具体是哪一个客户端窗口”。子窗口常把父 IWindow token 放进 `attrs.token`，但固定主窗口放的是 Activity token。

## 3. resume 先隐藏 Decor、先写 mWindowAdded，再调用 void addView

`handleResumeActivity()` 先完成 `performResumeActivity()`，再排除目标已待销毁、已 finish 或当前不应可见等情况。普通首次路径的关键顺序是：

```java
View decor = r.window.getDecorView();
decor.setVisibility(View.INVISIBLE);
WindowManager.LayoutParams l = r.window.getAttributes();
l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
a.mWindowAdded = true;
wm.addView(decor, l);
```

`willBeVisible` 初值是 `!a.mStartedActivity`；必要时还会同步询问 ATMS。`mVisibleFromClient=false` 时，即使其他门通过，也不会在这里 add。preserved Window 则复用既有根并走 callback 重绑，不属于固定路径。

这里有三个容易混淆的事实：

- `mWindowAdded=true` 是 **调用前的客户端意图位**，不是 add 成功回执；异常不会由 ActivityThread 自动改回 false。
- 传给 WMS 的初始 host visibility 来自 Decor 当前状态；固定路径此时是 INVISIBLE。
- `TYPE_BASE_APPLICATION` 决定应用 token、Policy 与层级语义，不只是日志标签。

正常 add 返回后，ActivityThread 还会处理配置和 soft-input 标志，再写 `mVisibleFromServer=true` 并调用 `makeVisible()`。因此 `G_return < D_show`；固定主线程尚未回到 Looper，先前排队的 traversal 不会插进这段同步调用栈。

## 4. Activity 的本地 WindowManager 负责把 Activity token 填进请求

`Activity.attach()` 已把 Activity token 交给 PhoneWindow：

```java
mWindow.setWindowManager(
        context.getSystemService(Context.WINDOW_SERVICE),
        mToken, mComponent.flattenToString(), hardwareAccelerated);
```

`Window.setWindowManager()` 保存 `mAppToken`，并基于进程 WindowManagerImpl 创建带 `mParentWindow=this` 的本地门面。于是主窗口 add 的路径是：

```text
Activity WindowManagerImpl
→ applyDefaultToken()
→ WindowManagerGlobal.addView(..., parentWindow=PhoneWindow, userId)
→ PhoneWindow.adjustLayoutParamsForSubWindow()
```

方法名 `adjustLayoutParamsForSubWindow()` 不能按字面理解。固定 `TYPE_BASE_APPLICATION` 走它的普通应用分支：token 为空时补 `mAppToken`；标题为空且 `mAppName` 非空时补 `mAppName`，Activity 路径传入的是 component flatten 字符串。packageName 是随后独立补齐的字段，不是 title 的回退值；所有类型最后还会按 Window 的硬件加速状态补 flag。真正的 sub-window 分支才会尝试 Decor 的 window token。

`applyDefaultToken()` 在 `mDefaultToken!=null && mParentWindow==null` 时才补 token；Activity 本地门面有 parentWindow，所以它不是主窗口 token 的来源。

参数也会分叉成多份：

| 阶段 | 参数关系 |
|---|---|
| PhoneWindow → WMG | 调用者 `l` 被本地补 token/title/package/flag |
| Decor 与 WMG | `view.setLayoutParams(wparams)` 与 `mParams.add(wparams)` 保存这份本地对象 |
| ViewRootImpl | `mWindowAttributes.copyFrom(attrs)` 建独立快照 |
| Binder / WMS | `in LayoutParams` 经 Parcel 成为 system_server 对象 |
| WindowState | `mAttrs.copyFrom(a)` 再建服务端窗口快照 |

因此调用者事后直接改原对象并不会自动修改 ViewRoot/WMS；正常更新要走 `updateViewLayout()` 协议。

## 5. WindowManagerGlobal 先记三联账，再把有外部效果的 setView 放到最后

`WindowManagerGlobal.addView()` 先检查 view、Display 和参数类型，然后在 `mLock` 下：

1. 按 View 身份查重；若旧异步 remove 尚未结束，先 `doDie()`；
2. 只为 sub-window 按父 IWindow token 找本地 `panelParentView`；
3. `new ViewRootImpl(view.getContext(), display)`；
4. 给 Decor 写顶层 LayoutParams；
5. 依次追加 `mViews/mRoots/mParams`；
6. 最后调用 `root.setView()`。

三张并行列表的同一索引是一组本地顶层窗口记录；一个进程可有多个 Activity、Dialog 或其他顶层窗口，所以可有多个 ViewRootImpl。

“new ViewRootImpl 在首次跨 Binder 前”并不成立。两参构造器先求值 `WindowManagerGlobal.getWindowSession()`；冷路径会经 IWindowManager 同步 `openSession()`。准确边界是：

```text
可能的 openSession Binder
< ViewRootImpl 构造完成
< WMG 三联账追加
< 本次 addToDisplayAsUser Binder
```

普通路径随后复用静态缓存的 IWindowSession。源码只说一般每进程一个 Session；这不是协议层禁止多个 Session。

还有一个并发边界：WMG 的 `mLock` 覆盖 `root.setView()`，所以也覆盖本次同步 add Binder 等待。它保护本地列表一致性，不会把 App 与 WMS 变成共享 Java 事务。

## 6. ViewRootImpl 已有线程、IWindow 与 AttachInfo，但 View 还没有 AttachInfo

ViewRootImpl 构造阶段已经：

```java
mWindowSession = session;
mThread = Thread.currentThread();
mWindow = new W(this);
mAttachInfo = new View.AttachInfo(
        mWindowSession, mWindow, display, this, mHandler, this, context);
```

固定主窗口由 App 主线程构造，后续 `checkThread()` 以 `mThread` 约束 View 树操作，并使用主线程 Choreographer。`W extends IWindow.Stub` 是每窗 Binder 回调端，内部弱引用 ViewRootImpl；WMS 回调进入 App Binder 线程并不自动等于 View 回调已在主线程执行，仍要看各 dispatch 方法怎样投递。

`setView()` 只在 `mView==null` 时接受根 View。它会复制 attrs、补 packageName、加入 `PRIVATE_FLAG_USE_BLAST`、准备 surfaceInsets/兼容缩放/硬件渲染状态，然后写：

```java
mAttachInfo.mRootView = view;
mAdded = true;
```

这里的 AttachInfo 仍是 **ViewRootImpl 持有的准备对象**。Decor 自己的 `View.mAttachInfo` 要等首次 `dispatchAttachedToWindow()` 才写入。因此 `mAdded=true` 只说明客户端 add 流程开始，既不证明 WMS 接受，也不证明 `View.isAttachedToWindow()`。

ViewRootImpl 构造时还已有一个客户端 `SurfaceSession` 字段；它同样不是当前窗口已经取得可绘制 Surface 的证据。Surface 名字相似的对象要到第 14 节再逐层结账。

## 7. requestLayout 先排一笔 traversal 债，当前栈并不执行 measure

`setView()` 在调用 WMS 前明确执行：

```java
mAdded = true;
requestLayout();
mWindowSession.addToDisplayAsUser(...);
```

`requestLayout()` 标记 `mLayoutRequested=true`，`scheduleTraversals()` 再：

```java
mTraversalBarrier = queue.postSyncBarrier();
mChoreographer.postCallback(
        Choreographer.CALLBACK_TRAVERSAL, mTraversalRunnable, null);
```

这证明 `T_queue < B_enter`，但不能推出 traversal 已运行。同步屏障和 Choreographer callback 建立的是未来调度；固定 App 主线程仍在当前 `handleResumeActivity()` 调用栈，马上进入同步 Binder 并等待。

成功路径更强的顺序是：

```text
schedule callback
< 同步 add Binder 返回
< InputEventReceiver / assignParent / input stages
< WindowManager.addView 返回
< Activity.makeVisible
< 主线程回到 Looper 后的首次 traversal
```

WMS 期间可以并发调用 App 的 IWindow Binder；那是 Binder worker 的入口，通常还要转 ViewRoot Handler，不能据此把主线程 traversal 插到同步 add 中间。

## 8. addToDisplayAsUser 是同步协议，Session 既转发也保存客户端身份

`IWindowSession.aidl` 的 `addToDisplayAsUser()` 有 int 返回值和多个 out 参数，不是 oneway。App 主线程发送后要等 system_server 的 Session/WMS 执行并把结果写回。

固定调用的主要输入与输出是：

| 方向 | 内容 |
|---|---|
| 输入 | IWindow、seq、ViewRoot attrs 快照、INVISIBLE host visibility、displayId、userId |
| 输出 | frame hint、content/stable Insets、DisplayCutout、InsetsState、controls、客户端 InputChannel |
| 返回 int | 负值表示拒绝；非负值是成功 bitset |

Session 的方法主体转调 `mService.addWindow()`，但 Session 不是无状态代理。`openSession()` 时它已保存原始 `mUid/mPid` 和若干权限能力；成功 `win.attach()` 还会增加窗口计数，并在首窗创建服务端 SurfaceSession。

WMS 的身份顺序需要精确写：

```text
Policy.checkAddPermission()
→ 保存本次 Binder callingUid/callingPid
→ clearCallingIdentity()
→ 在 system_server 身份下处理内部对象
```

后续仍显式使用保存的 calling uid/pid 与 Session uid。成功路径离开全局锁后调用 `restoreCallingIdentity(origId)`；多个早退分支没有在这个方法内经过该语句，失败边界见第 15 节。

## 9. WMS 的门按顺序缩小候选，普通 Activity 不会凭空创建 token

`WMS.addWindow()` 先在全局锁外做 type permission 检查，再在 `mGlobalLock` 内依次核对：

| 门 | 固定路径要求 | 典型拒绝 |
|---|---|---|
| Display | 服务已 ready，Display 存在且 `hasAccess(session.mUid)` | 未 ready 抛 `IllegalStateException`；其余为 `ADD_INVALID_DISPLAY` |
| 重复窗 | `mWindowMap` 不含同一 IWindow Binder | `ADD_DUPLICATE_ADD` |
| 窗口种类 | 不是非法 sub-window/presentation 组合 | BAD_SUBWINDOW、PERMISSION 或 INVALID_DISPLAY |
| user | 请求 user 与 Session user 一致，或通过 incoming-user 校验 | `ADD_INVALID_USER` |
| token | 找到既有 Activity WindowToken | `ADD_BAD_APP_TOKEN` |
| Activity | token 可转 ActivityRecord，且仍有 parent | NOT_APP_TOKEN 或 APP_EXITING |
| WindowState | IWindow 能 link death，Display 尚未移除 | APP_EXITING 或 INVALID_DISPLAY |
| Policy | adjust 与 validate 均接受 | `SecurityException`，或 permission/type/singleton 类 `ADD_*` |

固定主窗口的 `rootType` 落在 application 范围。若 `attrs.token` 未知，`unprivilegedAppCanCreateTokenWith()` 会拒绝，而不是替应用造一个 Activity token；已有 token 不是 ActivityRecord 会报 NOT_APP_TOKEN；ActivityRecord 已脱离父容器会报 APP_EXITING。

Display 选择也不是无条件信任传入的 displayId。`getDisplayContentOrCreate(displayId, attrs.token)` 会先查既有 token；命中时返回 token 所属 DisplayContent，未命中才按 displayId 取得或创建。随后仍以 `session.mUid` 做 Display access 检查。

sub-window 则先用 `attrs.token` 找父 WindowState，且父本身不能再是 sub-window；随后沿用父窗口的 token 和 rootType 规则。IME、Wallpaper、Toast、Accessibility Overlay 等各有专门 token/权限分支，不能把 Activity 规则外推给所有 type。

## 10. WindowState 是服务端副本，构造成功仍早于正式挂账

前置身份通过后，WMS 构造：

```java
WindowState win = new WindowState(
        this, session, client, token, parentWindow,
        appOp, seq, attrs, viewVisibility,
        session.mUid, userId, ...);
```

WindowState 会复制 attrs，保存 owner/show user、ActivityRecord 与 IWindow，链接 IWindow death recipient，创建 WindowStateAnimator 和 InputWindowHandle。此时 system_server 没有 Decor 引用；客户端 View 属性只有经协议同步的副本。

固定顶层 Activity 的 WindowState 在构造器里不会立即加入 Activity token；`win.mToken.addWindow(win)` 位于后面的提交段。sub-window 构造器却会先 `parentWindow.addChild()`，这是失败分析时不能忽略的旁支。

构造出 `WindowStateAnimator` 也不等于已有窗口 Buffer surface：它的 `mSurfaceController` 仍为空。DisplayPolicy 随后先调整 `win.mAttrs`，再执行 `validateAddingWindowLw()`；只有通过才进入输入通道步骤。

死亡监听同样只是清理能力：它让客户端 Binder 死亡时 WMS 有机会回收状态，不证明 add 已正常返回。

## 11. InputChannel 在意图提交线之前成对创建，注册不等于能收触摸

App 在 Binder 前创建的 `new InputChannel()` 只是 out 参数容器。固定 Policy 通过后，WindowState 才真正：

```java
InputChannel[] channels = InputChannel.openInputChannelPair(name);
mInputChannel = channels[0];
mClientChannel = channels[1];
mWmService.mInputManager.registerInputChannel(mInputChannel);
mInputWindowHandle.token = mInputChannel.getToken();
mClientChannel.transferTo(outInputChannel);
mClientChannel.dispose();
mClientChannel = null;
mWmService.mInputToWindowMap.put(mInputWindowHandle.token, this);
```

服务端端点先被 InputManager 注册，channel token 再写进 InputWindowHandle；随后 `transferTo(outInputChannel)` 只在 system_server 内把客户端端点所有权移入服务端的 out 容器，WMS 释放原包装引用，最后才以 token 为 key 写入 `mInputToWindowMap`。真正的跨进程 Parcel 要等整个 addWindow 返回，由 Binder reply 把 out 容器带回 App；这几步相邻却不是同一点。

这个动作发生在源码注释的意图提交线之前：

```text
WindowState + death link
< Policy validate
< InputChannel pair/register/transfer
< “From now on, no exceptions or errors allowed!”
< Session/map/token 正式挂账
```

注释表达后续代码必须按成功路径维持一致性，不是语言或运行时保证“绝不会抛异常”。

固定窗口的服务端 `mViewVisibility` 是 INVISIBLE，而 `canReceiveKeys()` 明确要求 VISIBLE，所以它在 add 阶段不是新的按键焦点候选。WMS 稍后只会让 InputMonitor 经 Handler 排异步更新；add 返回不证明新快照已推给 InputDispatcher。App 也要等 Binder 返回后才创建 `WindowInputEventReceiver`。因此 channel pair、服务端注册、快照推送、窗口可命中、App receiver 和某个事件被消费是六个完成点。

## 12. 服务端提交把 WindowState 挂入三套关系，却刻意不做最终 layout

越过 `C_commit` 后，固定主线的关键顺序是：

```java
win.attach();
mWindowMap.put(client.asBinder(), win);
win.initAppOpsState();
win.mToken.addWindow(win);
displayPolicy.addWindowLw(win, attrs);
```

这几行分别完成：

- Session 首窗时创建服务端 SurfaceSession、加入 WMS Session 集合并增加窗口计数；
- 以 IWindow Binder 为 key 登记 WindowState；
- 建立 AppOps/挂起或 overlay 隐藏状态；
- 把 WindowState 加到 ActivityRecord/WindowToken 容器层级；
- 调用 DisplayPolicy 的后置入口；只有特定系统窗口或 Insets provider 会在这里登记角色，固定普通主窗通常没有新增 Policy 角色账。

随后 WMS 标记进入动画意图，调用 `getLayoutHint()` 填初始 frame/content/stable Insets/cutout，复制 InsetsState，组合 touch/app-visible/BLAST 等返回位，处理焦点/IME 候选与子层级，调用 InputMonitor 经 Handler 排异步快照更新，最后填可控 Insets controls。`getLayoutHint()` 自己说明数据来自最近一次 layout，不保证与新窗口的最终 layout 相同；add 返回也不保证 InputDispatcher 已收到新快照。

固定 INVISIBLE 窗口不会因 add 就成为新焦点；`ADD_FLAG_APP_VISIBLE` 又读取 ActivityRecord 的 client-visible 状态，不等同于 Decor 当前 Java visibility。

源码在这里明确不做最终 layout：

```text
Don't do layout here; the window must call relayout to be displayed.
```

所以 outFrame 是 layout hint，不是首次 relayout 的最终 frame；WindowState 已存在也不是可绘制 Surface 已交给 App。

## 13. 非负结果先被拆成状态，再按 receiver → parent → stages → PendingInsets 完成接线

Binder 返回后，ViewRootImpl 先接收 frame/Insets/cutout/controls，再判断 `res < ADD_OKAY`。固定非负路径随后按源码顺序：

```text
读取 BLAST / triple-buffering 成功位
→ 可选 RootViewSurfaceTaker input queue
→ new WindowInputEventReceiver(clientChannel, Looper.myLooper())
→ view.assignParent(this)
→ 写 mAddedTouchMode 与 mAppVisible
→ 检查无障碍状态；enabled 时建立连接
→ NativePreIme ... SyntheticInputStage
→ Decor 提供 PendingInsetsController，并 replayAndAttach 到真实 InsetsController
→ setView 返回
```

不能把 receiver、assignParent、stage 链与 PendingInsets 重放画成同一点：receiver 在 parent 之前，stage 链在 parent 之后；固定 Decor 始终返回自己的 PendingInsetsController，`replayAndAttach()` 又在 stage 链之后、`setView()` 返回之前执行。它可能同步重放此前积累的 Insets 请求与监听器，因此也是独立的异常边界。

r48 的成功位还有一个必须保留的源码事实：

| 常量 | r48 值 | ViewRoot 用途 |
|---|---:|---|
| `ADD_FLAG_IN_TOUCH_MODE` | `0x1` | 初始化 touch mode |
| `ADD_FLAG_APP_VISIBLE` | `0x2` | 初始化 `mAppVisible` |
| `ADD_FLAG_USE_TRIPLE_BUFFERING` | `0x4` | 打开 triple buffering |
| `ADD_FLAG_ALWAYS_CONSUME_SYSTEM_BARS` | `0x4` | 初始化 always-consume-bars |
| `ADD_FLAG_USE_BLAST` | `0x8` | 选择 BLAST adapter |

triple-buffering 与 always-consume-bars 在 r48 共享 `0x4`，WMS 可因任一来源置位，客户端会把两者都读成 true。它是此版本常量别名，不代表两个概念相同，也不能从该 bit 反推唯一来源。

`P_parent` 之后 Decor 的 `mParent` 已是 ViewRootImpl，但它自己的 `mAttachInfo` 仍为空。因此在 `G_return`：

```text
decor.getParent() == ViewRootImpl
decor.isAttachedToWindow() == false
decor.getViewRootImpl() == null
decor.getWindowToken() == null
```

`getViewRootImpl()` 与 `getWindowToken()` 都依赖 View 的 AttachInfo，不只看 parent。外层 add 返回 void；调用者拿不到服务端 result，只能观察这些分层副作用。

## 14. makeVisible 与首次 traversal 才跨入 attach、relayout 和 Buffer surface

正常 `G_return` 后，ActivityThread 才写服务端可见镜像并调用：

```java
r.activity.mVisibleFromServer = true;
r.activity.makeVisible();
```

`makeVisible()` 固定路径不再 add，只把 Decor 改为 VISIBLE。因为 `assignParent()` 已完成，这次 View 状态变化可以继续请求 ViewRoot 调度；WMS 仍要等首次 traversal 的 relayout 才收到 VISIBLE。

第一次 `performTraversals()` 的关键相对顺序是：

```text
dispatchAttachedToWindow + 初始 Insets
→ measure
→ IWindowSession.relayout
→ WMS layout / 可见 Surface 创建
→ App 接收 Surface
→ layout
→ pre-draw 决策
→ draw
```

所以 `onAttachedToWindow()` 甚至早于本次可见 relayout；attached 也不能证明已有可绘制 Surface。pre-draw 还可以取消当次 draw，首 Buffer 与 present 更晚。

“Surface 已有”至少要区分五层：

| 对象 | 最早出现 | `G_return` 能否证明 |
|---|---|---|
| ViewRootImpl 客户端 SurfaceSession | ViewRoot 构造 | 能证明对象已建；不能证明窗口 Buffer surface |
| system_server Session SurfaceSession | `win.attach()` 的首窗 | 能证明会话已建；不能证明当前窗口可画 |
| WindowState 继承的 container-layer SurfaceControl | `win.mToken.addWindow(win)` 触发 parent/onParentChanged | 成功 add 可已有；它是容器层，不是窗口 Buffer |
| WindowStateAnimator 的 WindowSurfaceController | 可见 `relayoutWindow()` 的 `createSurfaceLocked()` | `G_return` 不能证明 |
| App 可绘制 Surface、首 Buffer、present | relayout 返回后及图形流水线 | `G_return` 全都不能证明 |

因此“add 阶段没有任何 SurfaceControl”也是错误说法。准确结论是：服务端层级用 container SurfaceControl 可以已经存在，但承载窗口 Buffer 的 WindowSurfaceController 与交给 App 的有效 Surface 仍等待可见 relayout。

## 15. add 不是跨进程原子事务：失败残留与诊断证据必须逐点看

固定主线正常返回；通用失败路径却不能用“抛异常就全部回滚”概括：

| 失败位置 | r48 已可能留下什么 | 不能声称 |
|---|---|---|
| 本地参数/重复 View 检查 | token/title/package/硬件 flag 可能已写进调用者 attrs | 所有输入原样 |
| 冷 `openSession()` | 本次 addToDisplay 尚未发生；参数已被 parentWindow 调整 | ViewRoot/三联账必已建立 |
| WMS 早期 display/token/user 拒绝 | 无正式 WindowState 挂账；方法内多个 return 不经过底部显式 identity restore | 每个出口都执行了该 restore |
| WindowState/Policy 阶段拒绝 | death link 已可能建立；sub-window 还可能先入本地父 WindowState | 服务端零副作用 |
| InputChannel 后的 Toast 拒绝 | channel 注册/映射已可能发生，源码无统一 rollback 块 | 注释提交线以前天然原子 |
| 负 ADD_* 或 RemoteException 回到 App | traversal 会取消，部分 ViewRoot 字段复位；Activity `mWindowAdded` 已提前为 true | WMG 三联数组必删除 |
| WMS/Policy 直接抛 RuntimeException | Policy 前可已有 death link、sub-window parent 或新非应用 token；提交线后还可已有 Session/map/token/container surface | 一定转成负 ADD_*，或执行 ViewRoot 的负码/RemoteException 清理 |
| WMS 已提交、客户端 receiver/无障碍/stage/PendingInsets 重放再异常 | WindowState 与输入服务端账可能已存在 | 客户端异常会自动 remove 远端窗口 |
| 首次 traversal/relayout/draw 失败 | add 账仍可能完整；没有目标首帧 | add 成功等于显示成功 |

WindowManagerGlobal 的 r48 细节尤其关键：fresh View 的

```java
int index = findViewLocked(view, false);
```

通常得到 `-1`。代码随后追加三联数组；`root.setView()` 抛 RuntimeException 时，catch 却只在旧 `index >= 0` 时调用 `removeViewLocked(index, true)`。所以普通首次 BadToken、InvalidDisplay 或包装后的 RemoteException 可以留下已追加的本地条目。这个 guard 复用了 add 前的查重索引，不能当成新条目的可靠回滚。

ViewRootImpl 对负结果也先更新部分 frame/Insets 状态，随后才检查 `res < 0`；RemoteException 与负业务拒绝的字段复位还不完全相同。常见翻译包括 BadTokenException、InvalidDisplayException 与未知码 RuntimeException；`ADD_STARTING_NOT_NEEDED` 则只在 starting-window 旁支静默返回。

还有第三类不能并入这两支：`DisplayPolicy.validateAddingWindowLw()` 的权限强制可直接抛 `SecurityException`。它经 Binder 回到 App 后不会命中 ViewRootImpl 只捕获 `RemoteException` 的 catch，也不会经过负码 switch，因此 `mAdded/mView` 与已排 traversal 都可能保持；外层 WMG 的 fresh `index=-1` 又不删除刚追加的三联账。非应用 token 可在构造时先入 Display，sub-window 可在 WindowState 构造时先入 parent；越过意图提交线后若容器 Surface 创建等运行时代码再抛错，还可能已留下 Session、map 或 token 的更深提交。

WMS 在成功落底才显式 `restoreCallingIdentity(origId)`；多个锁内拒绝 return 和异常没有在 `addWindow()` 内使用统一 finally。Binder transaction 退栈还有自己的身份边界，但不能把底部语句画成所有源码出口的必经点。

正常 remove 也有自己的完成点，不能拿 parent 为空当成清账完成：

```text
removeView(false)
→ ViewRootImpl.die(false) 排 MSG_DIE
→ 立即 view.assignParent(null)，加入 mDyingViews
→ 稍后 doDie / dispatchDetachedFromWindow
→ 同步 IWindowSession.remove → WMS removeWindow → WindowState.removeIfPossible
   ↳ 可立即 removeImmediately → postWindowRemoveCleanupLocked
   ↳ 也可因 replacement / 已显示的退出动画先标记并返回
→ App 释放 InputEventReceiver，WindowManagerGlobal.doRemoveView 删除本地三联数组与 mDyingViews 条目
→ 若服务端延迟，稍后才 removeImmediately 并删除 mWindowMap 条目
```

排队后、`doDie()` 前，三联数组仍在，View 的 AttachInfo 也可能仍在；`removeViewImmediate()` 若恰处于 traversal 同样会退化为排 `MSG_DIE`。即使它在客户端同步走完，Session.remove 正常返回也只证明服务端 `removeIfPossible()` 已返回：replacement 或已显示窗口的退出动画仍可推迟 `removeImmediately()`，真正的 `mWindowMap.remove()` 要等 `postWindowRemoveCleanupLocked()`。客户端的 immediate 不会强迫服务端 immediate。

若 detach 内的 Session.remove 遇 RemoteException，异常会被吞掉，本地仍继续释放 receiver 和删三联账，所以本地清账甚至不证明 WMS 收到 remove。回调异常又可能截断这串没有统一 finally 的清理，诊断时仍要逐点取证。

诊断时用最窄证据：

| 证据 | 至少证明 | 仍不能证明 |
|---|---|---|
| `Activity.mWindowAdded=true` | 客户端曾决定 add 或复用 | WMG/WMS 成功 |
| WMG 三联数组有 Decor | 本地 append 已发生 | `setView` 正常返回 |
| 同一 WMG/Decor 先见三联账、后确认已无 | 客户端 `doRemoveView()` 已删该本地条目 | WMS `mWindowMap` 已删 WindowState |
| WMS `mWindowMap` 有 IWindow | 服务端已越过 map 提交点 | 客户端 parent/stages 已完成 |
| Decor parent 是 ViewRootImpl | `assignParent` 已执行 | View AttachInfo 已分发 |
| `decor.isAttachedToWindow()` | 首次 traversal 已 dispatch attach | relayout Surface 或 draw |
| InputEventReceiver 非空 | App 已接客户端 channel | 窗口可命中或已消费事件 |
| `mWinAnimator.mSurfaceController!=null` | 窗口 Buffer surface 已在 relayout 创建 | Buffer 已提交或 present |
| 目标帧 present 证据 | 所选画面已到显示完成点 | 较早阶段耗时无异常 |

本章结算在 `G_return`：本地顶层账、远端窗口账、输入连接和未来 traversal 债都已建立；可见性提交、attach、relayout、Surface、draw 与 present 仍必须分别取证。

## 16. 九组只读练习：重建 resume、add、输入、Surface 与失败账

以下命令只检查文件并检索文本。可在 Android 11 r48 源码根运行，也可先设置 `ANDROID_BUILD_TOP`；每组在 Bash 3.2 与 Zsh 5.9 中都应独立以 0 退出。

### 练习 1：证明 INVISIBLE、mWindowAdded 与 makeVisible 的顺序

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
test -f "$T" && test -f "$A"
grep -nE 'performResumeActivity\(|mActivitiesToBeDestroyed|willActivityBeVisible|r.window == null|decor.setVisibility\(View.INVISIBLE\)|l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION|a.mWindowAdded = true|wm.addView\(decor, l\)|mVisibleFromServer = true|r.activity.makeVisible\(' "$T"
grep -nE 'void makeVisible\(\)|wm.addView\(mDecor|mWindowAdded = true|mDecor.setVisibility\(View.VISIBLE\)' "$A"
```

画出 `onResume 返回 → D_hide → G_return → D_show → 首次 traversal`，并标注 `mWindowAdded=true` 在 add 调用前。

完成标准：不能用 Activity 字段 true 证明 WMS 成功，也不能把 INVISIBLE 当成 GONE 或已显示。

### 练习 2：找到主窗口 token 的本地补齐点与参数副本

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
W="$SRC/frameworks/base/core/java/android/view/Window.java"
I="$SRC/frameworks/base/core/java/android/view/WindowManagerImpl.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
test -f "$A" && test -f "$W" && test -f "$I" && test -f "$G" && test -f "$V" && test -f "$S"
grep -nE 'mWindow.setWindowManager\(|mToken, mComponent.flattenToString' "$A"
grep -nE 'mAppToken = appToken|createLocalWindowManager\(this\)|adjustLayoutParamsForSubWindow|wp.token = mContainer == null|wp.packageName = mContext.getPackageName|FLAG_HARDWARE_ACCELERATED' "$W"
grep -nE 'applyDefaultToken\(|mParentWindow == null|mGlobal.addView\(' "$I"
grep -nE 'view.setLayoutParams\(wparams\)|mParams.add\(wparams\)' "$G"
grep -nE 'mWindowAttributes.copyFrom\(attrs\)' "$V"
grep -nE 'mAttrs.copyFrom\(a\)' "$S"
```

给 caller attrs、WMG mParams、ViewRoot 快照、Parcel 对象和 WindowState 快照画引用/复制关系。

完成标准：固定 Activity token 来自 parent PhoneWindow 的 mAppToken；DefaultToken 不是这条主线。

### 练习 3：还原三联账、冷 openSession、失败与 remove guard

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
W="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
test -f "$G" && test -f "$V" && test -f "$S" && test -f "$M" && test -f "$W"
grep -nE 'findViewLocked\(view, false\)|mDyingViews.contains\(view\)|new ViewRootImpl|mViews.add\(view\)|mRoots.add\(root\)|mParams.add\(wparams\)|root.setView\(|catch \(RuntimeException e\)|if \(index >= 0\)|removeViewLocked\(index, true\)' "$G"
grep -nE 'IWindowSession getWindowSession\(|getWindowManagerService\(\)|openSession\(|sWindowSession =' "$G"
grep -nE 'WindowManagerGlobal.getWindowSession\(\)|mThread = Thread.currentThread\(\)|mWindow = new W\(this\)|new View.AttachInfo' "$V"
grep -nE 'catch \(RemoteException e\)|res < WindowManagerGlobal.ADD_OKAY|mAdded = false|unscheduleTraversals\(\)' "$V"
grep -nE 'removeView\(View view, boolean immediate\)|removeViewLocked\(|root.die\(immediate\)|view.assignParent\(null\)|mDyingViews.add\(view\)|void doRemoveView\(' "$G"
grep -nE 'boolean die\(boolean immediate\)|if \(immediate && !mIsInTraversal\)|sendEmptyMessage\(MSG_DIE\)|void doDie\(\)|dispatchDetachedFromWindow\(\)|mWindowSession.remove\(mWindow\)|doRemoveView\(this\)' "$V"
grep -nE 'void remove\(IWindow window\)|mService.removeWindow\(this, window\)' "$S"
grep -nE 'void removeWindow\(Session session, IWindow client\)|win.removeIfPossible\(\)|void postWindowRemoveCleanupLocked\(|mWindowMap.remove\(win.mClient.asBinder\(\)\)' "$M"
grep -nE 'void removeIfPossible\(\)|mWillReplaceWindow|setupWindowForRemoveOnExit\(\)|void removeImmediately\(\)' "$W"
```

分别画冷 Session 与热 Session 的顺序，再令 fresh View 的 `index=-1` 推演 `setView` 抛错后的三联账；最后比较 `removeView(false)`、traversal 内的 immediate remove 与真正 `doRemoveView()`。

完成标准：只能说三联账早于 addToDisplay Binder，不能说 ViewRoot 构造早于可能的 openSession Binder；parent 清空或客户端 immediate 完成，也不等于三联账、AttachInfo 与 WMS 窗口均已清除。

### 练习 4：证明 traversal 只是先排队，add Binder 是同步调用

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
Q="$SRC/frameworks/base/core/java/android/view/IWindowSession.aidl"
test -f "$V" && test -f "$Q"
grep -nE 'if \(mView == null\)|mAttachInfo.mRootView = view|mAdded = true|requestLayout\(\)|new InputChannel\(\)|mWindowSession.addToDisplayAsUser|setFrame\(mTmpFrame\)|postSyncBarrier\(\)|CALLBACK_TRAVERSAL|void doTraversal\(\)|performTraversals\(\)' "$V"
grep -nE '^interface IWindowSession|int addToDisplayAsUser|out Rect outFrame|out InputChannel outInputChannel' "$Q"
```

按当前主线程写出 `T_queue < B_enter < B_return < G_return < D_show < doTraversal`。

完成标准：AIDL 方法不是 oneway；`requestLayout()` 没在当前调用栈同步 measure。

### 练习 5：按顺序重建 WMS 的身份、Display、user 与 token 门

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
P="$SRC/frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java"
test -f "$M" && test -f "$P"
grep -nE 'mPolicy.checkAddPermission\(|Binder.getCallingUid\(\)|Binder.getCallingPid\(\)|Binder.clearCallingIdentity\(\)|getDisplayContentOrCreate\(|displayContent.hasAccess\(session.mUid\)|mWindowMap.containsKey\(client.asBinder\(\)\)|handleIncomingUser\(|getWindowToken\(|unprivilegedAppCanCreateTokenWith\(|token.asActivityRecord\(\)|activity.getParent\(\) == null|new WindowState\(' "$M"
grep -nE 'if \(token != null\)|mRoot.getWindowToken\(token\)|wToken.getDisplayContent\(\)|mRoot.getDisplayContentOrCreate\(displayId\)' "$M"
grep -nE 'ADD_INVALID_DISPLAY|ADD_DUPLICATE_ADD|ADD_BAD_SUBWINDOW_TOKEN|ADD_INVALID_USER|ADD_BAD_APP_TOKEN|ADD_NOT_APP_TOKEN|ADD_APP_EXITING' "$M"
grep -nE 'int validateAddingWindowLw\(|mContext.enforcePermission\(' "$P"
```

把固定 Activity 路径和 sub-window 路径分开；标注 calling uid/pid、Session uid 与 request user 分别被谁使用。

完成标准：未知 application token 被拒绝，不会现场创建 ActivityRecord；底部 identity restore 不是所有早退的必经点，Policy 权限强制也可能直接抛出 SecurityException。

### 练习 6：追踪 WindowState death link 与 InputChannel 两端

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
Q="$SRC/frameworks/base/core/java/android/view/IWindowSession.aidl"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
W="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowState.java"
N="$SRC/frameworks/base/services/core/java/com/android/server/input/InputManagerService.java"
test -f "$V" && test -f "$Q" && test -f "$M" && test -f "$W" && test -f "$N"
grep -nE 'INPUT_FEATURE_NO_INPUT_CHANNEL|new InputChannel\(\)|WindowInputEventReceiver' "$V"
grep -nE 'out InputChannel outInputChannel' "$Q"
grep -nE 'mDeathRecipient == null|openInputChannels|win.openInputChannel\(outInputChannel\)' "$M"
grep -nE 'linkToDeath\(|openInputChannelPair|registerInputChannel|mInputWindowHandle.token|transferTo|mInputToWindowMap.put' "$W"
grep -nE 'void registerInputChannel\(|nativeRegisterInputChannel' "$N"
```

给空 out 容器、服务端 endpoint、客户端 endpoint、InputWindowHandle token 与 App receiver 分别编号。

完成标准：服务端注册不等于 App receiver 已创建；固定 INVISIBLE WindowState 也不是新输入焦点。

### 练习 7：核准提交顺序、初始 hint 与 r48 成功位别名

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
I="$SRC/frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
test -f "$M" && test -f "$S" && test -f "$G" && test -f "$I" && test -f "$V"
grep -nE 'From now on, no exceptions or errors allowed|sEnableTripleBuffering|ADD_FLAG_USE_TRIPLE_BUFFERING|win.attach\(\)|mWindowMap.put\(client.asBinder\(\), win\)|win.initAppOpsState\(\)|win.mToken.addWindow\(win\)|displayPolicy.addWindowLw|getLayoutHint\(|ADD_FLAG_ALWAYS_CONSUME_SYSTEM_BARS|outInsetsState.set|setUpdateInputWindowsNeededLw|updateInputWindowsLw|Don.t do layout here|restoreCallingIdentity\(origId\)' "$M"
grep -nE 'void windowAddedLocked|mSurfaceSession = new SurfaceSession\(\)|mNumWindow\+\+' "$S"
grep -nE 'ADD_FLAG_IN_TOUCH_MODE = 0x1|ADD_FLAG_APP_VISIBLE = 0x2|ADD_FLAG_USE_TRIPLE_BUFFERING = 0x4|ADD_FLAG_ALWAYS_CONSUME_SYSTEM_BARS = 0x4|ADD_FLAG_USE_BLAST = 0x8' "$G"
grep -nE 'mAlwaysConsumeSystemBars =|ADD_FLAG_ALWAYS_CONSUME_SYSTEM_BARS|ADD_FLAG_USE_TRIPLE_BUFFERING|mEnableTripleBuffering = true' "$V"
grep -nE 'void updateInputWindowsLw\(boolean force\)|scheduleUpdateInputWindows\(\)|mHandler.post\(mUpdateInputWindows\)' "$I"
```

按源码列出 WMS 成功挂账顺序，并分别找出谁能置 `0x4`、ViewRoot 又怎样读取它。

完成标准：frame 是 hint；`0x4` 在 r48 无法区分 triple-buffering 与 always-consume-bars 的来源。

### 练习 8：证明 receiver、parent、input stages 与 PendingInsets 不是同一点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
W="$SRC/frameworks/base/core/java/android/view/View.java"
D="$SRC/frameworks/base/core/java/com/android/internal/policy/DecorView.java"
test -f "$V" && test -f "$W" && test -f "$D"
grep -nE 'mInsetsController.onStateChanged|mInsetsController.onControlsChanged|res < WindowManagerGlobal.ADD_OKAY|mUseBLASTAdapter = true|new WindowInputEventReceiver|view.assignParent\(this\)|mAddedTouchMode =|mAppVisible =|mFirstInputStage =|providePendingInsetsController\(\)|replayAndAttach\(mInsetsController\)' "$V"
grep -nE 'boolean isAttachedToWindow\(\)|return mAttachInfo != null|ViewRootImpl getViewRootImpl\(\)|IBinder getWindowToken\(\)' "$W"
grep -nE 'mPendingInsetsController = new PendingInsetsController\(\)|PendingInsetsController providePendingInsetsController\(\)|return mPendingInsetsController' "$D"
```

写出 `B_return < I_client < P_parent < input stages < PendingInsets replay < V_ready`，并解释 parent 已写但三个 View 查询为何仍受 AttachInfo 限制。

完成标准：固定 Decor 的 PendingInsetsController 非空且在 stages 后重放；命令能在 Bash 3.2 与 Zsh 5.9 中都以 0 退出；assignParent 不等于 dispatchAttached。

### 练习 9：区分 container SurfaceControl、窗口 Buffer surface 与首帧

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
K="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowToken.java"
C="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowStateAnimator.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
D="$SRC/frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java"
test -f "$K" && test -f "$C" && test -f "$M" && test -f "$A" && test -f "$V" && test -f "$D"
grep -nE 'void addWindow\(final WindowState|addChild\(win' "$K"
grep -nE 'void onParentChanged|createSurfaceControl\(false|setInitialSurfaceControlProperties|SurfaceControl.Builder makeSurface\(\)|return p.makeChildSurface\(this\)' "$C"
grep -nE 'SurfaceControl.Builder makeChildSurface\(WindowContainer child\)|makeSurfaceBuilder\(s\).setContainerLayer\(\)|setParent\(mSurfaceControl\)' "$D"
grep -nE 'shouldRelayout|createSurfaceControl\(outSurfaceControl|winAnimator.createSurfaceLocked' "$M"
grep -nE 'WindowSurfaceController createSurfaceLocked|new WindowSurfaceController|mSurfaceController' "$A"
grep -nE 'performTraversals\(\)|dispatchAttachedToWindow|measureHierarchy\(|relayoutWindow\(params|performLayout\(|dispatchOnPreDraw\(\)|cancelDraw|performDraw\(\)' "$V"
```

画 `container layer → dispatch attach → visible relayout/buffer surface → pre-draw → draw → Buffer → present`。

完成标准：add 成功可已有 container SurfaceControl；只有 relayout 才创建本章所说的窗口 Buffer surface，draw 仍可被 pre-draw 取消。
