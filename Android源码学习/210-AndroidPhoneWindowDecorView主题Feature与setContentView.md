# 210 Android PhoneWindow、DecorView、主题 Feature 与 setContentView

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只读源码，可以证明同步调用顺序、对象父子关系、Feature/Flag 账与窗口加入边界；不能据此测量某台设备的 inflate 耗时，也不能把本地 View 树完成、WMS 接受窗口、首个 Buffer 提交或硬件 present 合并成一个“页面已显示”。

第 209 章已经把普通启动追到 `Activity.attach()`、`onCreate()`、客户端提交、Start/Resume 与窗口 add。本章回到其中的本地窗口支路：接过已经存在的 `PhoneWindow` 和可能尚不存在的 Decor，沿一次 `Activity.setContentView(R.layout.activity_main)` 看 Theme、Feature、系统骨架与业务 View 树怎样汇合。

本章只追一个问题：**固定普通冷启动，开发者在 `onCreate()` 中调用的 `Activity.setContentView(int)` 正常返回时，客户端究竟交付了哪一层；为什么 Decor 与业务树都已存在，仍不能推出 ViewRoot、WMS `WindowState`、Surface 或首帧已经存在？**

## 1. 固定一次 setContentView，把“页面已设置”拆成十四个完成点

先固定 `B_target` 的主路径，避免把 Dialog、重建与动画分支混进同一句话：

| 维度 | 固定值或前提 |
|---|---|
| Activity | 普通远端 App 的顶层 Activity，`Activity.attach()` 已正常返回 |
| 调用位置 | App 主线程的 `onCreate()` 内 |
| Window | 新建 `PhoneWindow`，此前没有 `getDecorView()`、`findViewById()` 或 preserved Window；非 freeform，`hasWindowDecorCaption()` 为 false |
| Feature | 先成功请求 `FEATURE_NO_TITLE` |
| Theme | `ActivityInfo.getThemeResource()!=0`；非 floating、非 NoDisplay，不请求 content/activity transition 或 ActionMode overlay，也无其他影响骨架的 Feature |
| 内容 | 首次 `setContentView(int)`；合法 XML，根标签不是 `<merge>` |
| Callback | `Activity` 仍是 Window callback；`onContentChanged()` 使用默认空实现且不抛异常 |
| 生命周期 | 不 finish；后续正常 Start、Resume 和新窗口 add |

再定义十四个完成点：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `A_attach` | 第 209 章的 `Activity.attach()` 已返回，PhoneWindow 与 local WindowManager 已写入 | DecorView 已存在 |
| `F_accept` | `requestWindowFeature(FEATURE_NO_TITLE)` 返回 true，Feature 位已接受 | 系统装饰骨架已选择 |
| `P_set` | `PhoneWindow.setContentView(int)` 开始执行 | Decor 已创建 |
| `D_shell` | `generateDecor(-1)` 返回新的 DecorView 外壳 | `screen_*.xml` 或 content 容器存在 |
| `G_layout` | `generateLayout()` 返回 `android.R.id.content` | 业务 XML 已加入 |
| `I_decor` | `installDecor()` 的尾部协议接线正常完成 | 业务 XML 已加入 |
| `V_content` | 固定路径的 LayoutInflater 已把业务 XML 树挂到 content 并返回 | Insets 已交付或 ViewRoot 已存在 |
| `R_insets` | `requestApplyInsets()` 本次调用返回 | 真实 WindowInsets 已分发 |
| `C_notice` | callback 的 `onContentChanged()` 正常返回 | `setContentView()` 已返回 |
| `E_lock` | `mContentParentExplicitlySet=true` 已写入，PhoneWindow setter 正常返回 | Activity 的 ActionBar 尾部初始化已完成 |
| `B_bar` | `initWindowDecorActionBar()` 返回；固定无标题路径中它检查后直接跳过创建 | ViewRoot 或窗口已加入 |
| `S_return` | 开发者调用的 `Activity.setContentView(int)` 正常返回 | `onCreate()`、Create commit、Resume 或 add 已完成 |
| `W_add` | 后续普通新窗口的 `wm.addView()` 正常返回，WMS 已接受客户端主窗口 | 首个 Buffer 或 present 已完成 |
| `F_present` | 目标窗口首个可见帧完成所选显示完成证据 | 更早阶段没有性能问题 |

固定主路径的局部全序是：

```text
A_attach → F_accept → P_set → D_shell → G_layout → I_decor
         → V_content → R_insets → C_notice → E_lock → B_bar → S_return
         → W_add → F_present
```

答案先钉死：`S_return` 交付的是 **App 进程内的窗口配置与分层 View 树**，外加 callback 和 ActionBar 检查已经同步返回。它不交付 `ViewRootImpl`、WMS `WindowState`、Surface 或首帧；这些对象与事件分别位于后续窗口加入、relayout、遍历、Buffer 和合成链。

## 2. 六层对象与六本账，名字相近也不能合并

固定路径在 `S_return` 时形成的对象层级是：

```text
Activity
└─ PhoneWindow                         窗口策略与配置对象，不是 View
   └─ DecorView                        FrameLayout，PhoneWindow 持有的本地根容器
      └─ system 装饰 root                   screen_simple 等系统布局生成
         ├─ 标题、ActionBar、ActionMode 等可选区域
         └─ FrameLayout @android:id/content
            └─ 应用 XML 生成的业务根与子树

尚未出现：
DecorView ─X→ ViewRootImpl ─X→ IWindowSession/WMS WindowState
          ─X→ Surface/BufferQueue ─X→ 首帧 present
```

各层职责不同：

| 对象或账 | r48 中的角色 | `S_return` 的固定路径状态 |
|---|---|---|
| `PhoneWindow` | Feature、`LayoutParams`、Inflater、Decor、content、菜单和 transition 策略 | 已存在 |
| `PhoneWindow.mDecor` | Window 持有的 DecorView | 已存在 |
| `Activity.mDecor` | ActivityThread 在 resume 窗口支路另写的引用 | 尚未写入 |
| 系统装饰 root | 标题/ActionBar/ActionMode 与 content 的骨架 | 已加入 Decor |
| `mContentParent` | 指向 `android.R.id.content` 的 ViewGroup | 已解析 |
| 业务树 | XML 对象与父子关系 | 已加入 content |
| `mFeatures/mLocalFeatures` | 有效 Feature 与当前 Window 本地负责的 Feature 位图 | 已结算固定请求与主题请求 |
| `WindowManager.LayoutParams` | flags、尺寸、动画、cutout、soft input 等窗口属性 | 已有本地值，尚不等于 WMS 已接收 |
| `PhoneWindow` 的栏色状态 | status/navigation/divider color 与相应 contrast 策略 | 已有本地值，尚不等于 Surface 上已有对应像素 |
| `mContentParentExplicitlySet` | 后续 `requestFeature()` 的实现门 | 已为 true |
| `ViewRootImpl/WindowState` | View 树与窗口 session/WMS 的连接 | 尚不存在 |

`DecorView` 继承 `FrameLayout`，会参加 measure/layout/draw；`PhoneWindow` 继承 `Window`，自身不是 View。`android.R.id.content` 也不是 DecorView 的别名，而是系统骨架中的统一业务容器。只有把这些身份拆开，才不会把“Window 已有”“Decor 已有”“内容已设”“窗口已加”和“首帧已出”误当成同一点。

## 3. attach 先建立 PhoneWindow，但构造函数并非“整棵窗口树”

`Activity.attach()` 在业务 `onCreate()` 前完成：

```java
mWindow = new PhoneWindow(this, window, activityConfigCallback);
mWindow.setCallback(this);
mWindow.getLayoutInflater().setPrivateFactory(this);
mWindow.setWindowManager(...);
mWindowManager = mWindow.getWindowManager();
```

`Window(Context)` 先建立默认 Feature 账；`PhoneWindow(Context)` 用 Activity Context 取得 LayoutInflater，并读取 compositor shadow 的全局设置。主 Activity 构造重载还标记 `mUseDecorContext`，读取强制 resizable 设置与设备 PiP 能力；这些调用并非纯粹的空字段赋值。

但固定新窗口构造不会调用 `generateDecor()` 或 `generateLayout()`。到 `A_attach` 可以断言：

- Activity 已持有 PhoneWindow；
- PhoneWindow 已持有 LayoutInflater、Window callback 与 local WindowManager；
- soft input、UI options、token 等 attach 字段已接入；
- 不能断言 Decor、系统骨架、content、业务 View 或 ViewRoot 存在。

local `WindowManagerImpl` 只是客户端门面。它要到后续 `addView()` 才让 `WindowManagerGlobal` 创建 `ViewRootImpl` 并走 WMS；PhoneWindow 名字里有 Window，不会让构造函数自动跨进程注册窗口。

## 4. Decor 是懒安装的；get、find 与 resume 都可能改写对象现场

`PhoneWindow.getDecorView()` 的实现带副作用：

```java
if (mDecor == null || mForceDecorInstall) {
    installDecor();
}
return mDecor;
```

以下入口都可能间接触发它：

- 首次 `setContentView()` 或 `addContentView()`；
- `Window.findViewById()`，因为它先调用 `getDecorView()`；
- `Activity.initWindowDecorActionBar()`；
- 普通 resume 窗口支路为 add 取得 Decor；
- 任何显式 `getWindow().getDecorView()` 调用。

与之相对，`peekDecorView()` 只返回现值，不负责安装。排查“谁先造了 Decor”时，应区分 get 与 peek。

仅触发 `getDecorView()`，`installDecor()` 就可完成 Decor、系统骨架和空的 content 容器，却不会写 `mContentParentExplicitlySet`，也不会加入业务树。此后 `setContentView()` 看到 `mContentParent != null`，会复用这套骨架。

这带来一个实现与公共用法的差别：`requestFeature()` 的异常门检查 explicit 标志，而非 `mDecor != null`。先触发 Decor、再请求 Feature，代码未必立即抛异常；但 `generateLayout()` 不会自动重跑，既有骨架也不会随新位图重建。可靠顺序仍是先配置 Feature，再触发任何 Decor 安装。

## 5. requestFeature 写位图；冲突规则与“内容之前”是两层约束

`Activity.requestWindowFeature()` 只把调用委托给 PhoneWindow。基类 `Window.requestFeature()` 把 `featureId` 转为位：

```java
final int flag = 1 << featureId;
mFeatures |= flag;
mLocalFeatures |= mContainer != null
        ? (flag & ~mContainer.mFeatures) : flag;
```

`mFeatures` 是 Window 的有效 Feature 集；`mLocalFeatures` 是当前 Window 自己需要提供的部分。固定顶层 Window 没有 container，两者接近；嵌套 Window 不能把它们永远画等号。

PhoneWindow 在写位前后增加约束：

- `mContentParentExplicitlySet` 已为 true 时直接抛出“必须在 adding content 前请求”；
- `FEATURE_NO_TITLE` 已有时，请求 ActionBar 返回 false；
- 已有 ActionBar 时请求 No Title，会先移除 ActionBar；
- Custom Title 与不兼容标题 Feature 组合会抛异常；
- watch 设备的特定 progress Feature 还有平台限制。

因此 `F_accept` 不是“方法被调用”而是请求正常返回且目标位真正存在。固定路径先接受 No Title，再进入 `P_set`。

实现门仍不等于完整正确性门。`mContentParentExplicitlySet` 在两个 PhoneWindow `setContentView` 主实现的最后才写，`addContentView()` 在 r48 根本不写；callback 重入或 add 后再请求 Feature 可能绕过异常，却不会让已安装系统骨架重新选择。公开代码应把所有窗口 Feature 放在首次 Decor/内容操作之前。

## 6. Theme 的主要投影点是 generateLayout，不是唯一读取点

固定路径取 `ActivityInfo.getThemeResource()!=0`，所以 ActivityThread 在 `onCreate()` 前调用 `Activity.setTheme()`；后者既更新 Activity Context 的 Theme，也把资源 id 交给 PhoneWindow。若 activity/application theme id 都为 0，这次调用会被跳过，`ContextThemeWrapper.getTheme()` 首次读取时才选择默认 Theme，且不会顺带调用 `PhoneWindow.setTheme()`。开发者显式调用 `Activity.setTheme()` 仍会同步两处；要在 `onCreate()` 更换 Theme，也必须在首次 Window style 读取和 Decor 安装之前完成，才有可靠的结构效果。

`Window.getWindowStyle()` 第一次通过 Activity Context 取得 `R.styleable.Window` 的 TypedArray，此后缓存同一对象。固定路径的第一次主要消费发生在 `generateLayout()`：

```text
Manifest / Activity.setTheme
→ Activity Context Theme
→ Window.getWindowStyle() 缓存
→ generateLayout 投影 Feature、Flag、尺寸与装饰资源
```

`generateLayout()` 会处理：

- `windowNoTitle`、`windowActionBar`、overlay 与 content/activity transition Feature；
- floating、fullscreen、translucent、wallpaper、split touch 等尺寸或 flag；
- fixed/min width 与 height、dim、animation、soft input；
- status/navigation bar 颜色、light 标志、对比度与 cutout 模式；
- background、frame、elevation、outline 等 Decor 属性。

Feature 描述客户端窗口内部需要的能力或骨架，例如 No Title、ActionBar、Custom Title、Content Transitions。Flag 位于 `WindowManager.LayoutParams.flags`，供后续窗口策略与显示链消费，例如 Fullscreen、Dim Behind、Draws System Bar Backgrounds。两者可同由 Theme 导出，但绝不是同一本账。

targetSdk 的门要落到具体比较：split touch 的主题默认值取 target≥11；非 floating 窗口按 Theme 打开 system-bar backgrounds 要 target≥21；透明栏 contrast 属性只对 target≥29 读取；close-on-touch 的主题属性要求 target≥11，除非 Window 被设为 always-read。

显式优先级也不只一本账。开发者在安装前用 `setFlags()` 写入的 mask 会进入 `mForcedWindowFlags`，相应 Theme flag 投影会避开这些位；status/navigation bar color 使用各自的 forced boolean，soft input 则先看 `hasSoftInputMode()`。不能把所有“显式值压过 Theme”都归因于 `mForcedWindowFlags`。

“主要投影点”不能写成“所有 Theme 只在这里读取”。`windowNoDisplay` 在开发者 `onCreate()` 返回后由 `Activity.performCreate()` 另读；activity-transition 的具体对象还在 `installDecor()` 尾部初始化。Style 一旦缓存、骨架一旦安装，后续换 Theme 也不会自动重跑整条结构链。

## 7. generateDecor 只造外壳；DecorContext 不能被扩大解释

`installDecor()` 的第一层在 `mDecor == null` 时调用：

```java
mDecor = generateDecor(-1);
mDecor.setDescendantFocusability(FOCUS_AFTER_DESCENDANTS);
mDecor.setIsRootNamespace(true);
```

主 Activity Window 的 `generateDecor()` 通常基于 Application/display Context 创建 `DecorContext`，再把它作为新 `DecorView` 的 Context。到 `D_shell` 只交付一个 DecorView 外壳；系统 `screen_*.xml` 和 content 仍未加入。

`DecorContext` 的边界要按对象图描述：

- 它的 base 是从 Application Context 建出的 display Context；
- 它以弱引用读取当前 Activity Context 的资源和部分服务；
- WINDOW_SERVICE 返回当前 PhoneWindow 的 local manager；
- `setPhoneWindow()` 可在 preserved 重绑定时切到新 PhoneWindow。

这能避免把 Activity **直接**作为 DecorView 的 base context，并规避特定 service cache 长持 Activity 的路径；它不等于“从 Decor 再也强达不到 Activity”。`DecorView` 自身强持 PhoneWindow，PhoneWindow/Window 又强持构造时的 Activity Context。

还要区分“DecorView 自己的 Context”和“子树 inflate 的 Context”。PhoneWindow 的 `mLayoutInflater` 是在构造时从 Activity Context 取得的；`DecorView.onResourcesLoaded()` 接收这把 inflater 去展开系统骨架，业务 XML 也走它或 Scene 的 Context。不能把整棵窗口树都标成 DecorContext 或 Application Context。

## 8. generateLayout 选择一份系统骨架，并解析统一 content

第二层 `generateLayout(mDecor)` 先做 Theme 投影，再按 `getLocalFeatures()` 的优先级选 **一份**装饰资源。常见分支是：

| 条件 | r48 常见资源 | 主要额外区域 |
|---|---|---|
| 左/右标题图标 | `screen_title_icons` 或浮动主题资源 | 图标与标题 |
| progress 且无 ActionBar | `screen_progress` | progress 与标题 |
| Custom Title | `screen_custom_title` 或浮动主题资源 | 自定义标题容器 |
| floating 且有标题 | Theme 的 `dialogTitleDecorLayout` | 浮动标题骨架 |
| 非 floating、有标题且有 ActionBar | `windowActionBarFullscreenDecorLayout` 或默认 `screen_action_bar` | `DecorContentParent` 与 ActionBar |
| 非 floating 的普通标题 | `screen_title` | title |
| No Title + ActionMode overlay | `screen_simple_overlay_action_mode` | overlay stub |
| 普通 No Title | `screen_simple` | content 与 ActionMode stub |

这些分支有顺序，不是每个 Feature 各 inflate 一棵互不相干的树。

选定资源后，PhoneWindow 调用：

```java
mDecor.startChanging();
mDecor.onResourcesLoaded(mLayoutInflater, layoutResource);
ViewGroup contentParent =
        (ViewGroup) findViewById(ID_ANDROID_CONTENT);
...
mDecor.finishChanging();
return contentParent;
```

`DecorView.onResourcesLoaded()` 用 `inflater.inflate(layoutResource, null)` 独立生成系统 root，再以明确 LayoutParams 加到 Decor；自由窗口 caption 分支会先把 root 放进 `DecorCaptionView`。所有可用的 Activity 装饰资源都必须提供 `android.R.id.content`，否则 `generateLayout()` 立即抛错。

固定 `G_layout` 的三层对象是：

```text
DecorView
└─ screen_simple 生成的系统 root
   ├─ ActionMode 等系统区域
   └─ FrameLayout @android:id/content  ← 返回给 mContentParent
```

`generateLayout()` 同时把 background、frame、elevation、outline 等应用到 Decor。它只在 `PhoneWindow.mTitle != null` 时条件调用 `setTitle()`；固定新 PhoneWindow 的 title 通常仍为 null，因为 `Activity.attach()` 只写了 `Activity.mTitle`。具体 `DecorContentParent` 或 title view 要在 `installDecor()` 尾部绑定，普通 Activity 的初始 title 通常到后续 `onPostCreate()` 才通过 `onTitleChanged()` 推给 Window。

`startChanging()/finishChanging()` 只协调 Decor 内部批量结构变化，不是 WMS transaction，也不是 Surface 提交点。

## 9. installDecor 尾部接协议；系统骨架完成仍没有业务内容

`generateLayout()` 返回后，`installDecor()` 还会：

- 调整 framework optional fits-system-windows；
- 查找 `R.id.decor_content_parent`；
- 若存在，设置 Window callback、窗口标题、逐个初始化 local Feature、UI options、icon 与 logo；
- 若不存在，绑定普通 title view，并按 No Title 隐藏相应标题容器；
- 必要时延后菜单失效，避免在 `onCreate()` 中间同步创建 options menu；
- 应用 background fallback；
- 在 `FEATURE_ACTIVITY_TRANSITIONS` 下创建或读取 TransitionManager，并解析 enter/return/shared-element 等 transition。

标题控件或 DecorContentParent 的实际同步发生在这一尾部，而不是仅凭 `generateLayout()` 里的一次 `setTitle()` 就完成。普通 Activity 的 `onPostCreate()` 仍可能稍后再推一次 Activity title。

到 `I_decor`，PhoneWindow 已有 Decor、系统 root 与 `mContentParent`，装饰协议也已接好；固定路径尚未调用业务布局 inflater。若安装过程中资源、布局或 transition 解析抛异常，方法不会回滚已经创建的外壳或部分子树，也不能声称到达 `I_decor`。

## 10. setContentView(int) 把业务 XML 挂进 content，不是替换 Decor

固定首次调用进入 `PhoneWindow.setContentView(int)` 后，先因 `mContentParent == null` 完成 `installDecor()`。没有 Content Transitions 时再执行：

```java
mLayoutInflater.inflate(layoutResID, mContentParent);
```

LayoutInflater 的两参数重载等价于：

```java
inflate(resource, root, root != null);
```

`mContentParent` 非空，所以 `attachToRoot=true`。对于普通单根 XML，Inflater 创建业务根、递归创建子树、用 content 生成根 LayoutParams，并在返回前执行 `root.addView(temp, params)`。返回值是传入的 root，也就是 content，而不是业务根；PhoneWindow 本来也不接这个返回值。

`<merge>` 是另一种合法形态：它要求非空 root 且 `attachToRoot=true`，没有单一 XML 根对象，而是把其子节点直接 inflate 到 content。固定路径排除它只是为了让对象图更直观，不能把“每份布局都有一个业务根 View”写成通用规律。

Activity 在 attach 时把自己设置为 LayoutInflater private Factory。对普通 View 标签，公开 Factory2/Factory 未创建对象时，private Factory 才接手；平台 Activity 特别处理 `<fragment>`，其余普通 View 再回到一般创建路径。`<merge>`、`<include>`、`<requestFocus>`、`<tag>` 等结构标签以及 Blink 特判并不逐项走这条普通 Factory 顺序，不能把它外推成“每个 XML 标签都回调 Activity”。这里属于 XML 到 Java View 的构造链，第 211 章会继续拆资源解析、Factory 顺序、反射与 LayoutParams。

到 `V_content`，业务 View 对象和父子关系已经在 App 内存中存在。View/ViewGroup 本来就能在没有 ViewRoot 的情况下离线构树；`ViewRootImpl` 负责的，是把树连接到窗口 session、输入、VSync 与 traversal。

## 11. View 重载、重复设置、追加与 Scene 不是同一语义

三种 PhoneWindow setter 的公共骨架相同，但内容交付方式不同：

| 入口 | 固定无 transition 时的内容动作 | 关键边界 |
|---|---|---|
| `setContentView(int)` | inflate XML 到 content | 两参重载自动 attach；支持 `<merge>` |
| `setContentView(View)` | 用新建的 MATCH_PARENT/MATCH_PARENT 参数 add | 不保留 View 原有 LayoutParams |
| `setContentView(View, params)` | 直接 `addView(view, params)` | 不做 XML 解析 |
| 重复 `setContentView` | 先 `removeAllViews()` 再加入 | 复用 Decor 与系统骨架 |
| `addContentView` | 保留旧子节点并追加 | r48 不写 explicit 标志 |

启用 `FEATURE_CONTENT_TRANSITIONS` 后，setter 不走普通 remove/add 分支，而是建立 Scene。首次 `mContentScene == null` 时，PhoneWindow 直接调用 `Scene.enter()`：同步清旧子节点、inflate 或 add 新内容，且源码明确这一首次 enter 不运行 transition。已有 Scene 时才调用 `mTransitionManager.transitionTo()`；只有 scene root 尚未列入 `sPendingTransitions` 时，`changeScene()` 才在当前调用栈执行旧 Scene exit、新 Scene enter 并安装后续监听，真正 transition 仍要等 pre-draw 驱动。若同一 root 已有 pending transition，本次 `changeScene()` 会被忽略、树保持原状，尽管 PhoneWindow 随后仍把 `mContentScene` 更新为本次请求的 Scene。setter 返回因此既不能证明动画结束，也不总能证明请求的新 Scene 已进树。

后续 Scene 路径需要非空 TransitionManager。r48 只在 activity-transition 安装分支自动创建它，调用者也可显式设置；若只打开 content Feature、已经有 current Scene、又没有 manager，第二次切换会在调用处失败。Feature 位本身不保证动画基础设施完整。

`addContentView()` 即使发现 content transition，也只记录“不支持”并照常 `addView()`；不能类推为自动 Scene 动画。旧 View 移除也不等于立刻 GC，它还可能被应用字段、listener 或消息引用。

## 12. Insets、内容回调、explicit 锁和 ActionBar 决定真正返回点

业务树加入后，PhoneWindow 的固定顺序是：

```text
mContentParent.requestApplyInsets()
→ callback.onContentChanged()
→ mContentParentExplicitlySet = true
→ PhoneWindow.setContentView 返回
→ Activity.initWindowDecorActionBar()
→ Activity.setContentView 返回
```

`R_insets` 只表示这次请求调用返回。r48 的 `View.requestApplyInsets()` 只是转调 `requestFitSystemWindows()` 并沿 `mParent` 上抛；固定冷启动中 Decor 尚未 attach 到 ViewRoot，冒泡走到 parent 为 null 就停止，不持久化一个可供未来消费的 pending 位。只有请求真正到达 `ViewRootImpl.requestFitSystemWindows()`，才会写 `mApplyInsetsRequested=true` 并调度 traversal；首次 traversal 还会走自己的 Insets 分发。这里不能说真实 WindowInsets 已计算或 callback 已收到。

callback 只有在非空且 Window 未 destroyed 时才调用。固定 Activity callback 的默认 `onContentChanged()` 为空，但子类可以覆盖，Toolbar 也可能包装 Window callback。它是同步结构通知，不是生命周期回调、布局完成回调或绘制回调。

explicit 标志位于 callback **之后**。若 inflate、Scene、Insets 路径或 callback 抛异常，方法不会正常到达 `E_lock`；已有系统骨架或业务子树也不会自动回滚。callback 重入请求 Feature 还可能看到 false，这正是不能把实现门当公开安全顺序的原因。

Activity 的三种 `setContentView` 和 `addContentView` 都在 PhoneWindow 调用返回后执行 `initWindowDecorActionBar()`。它先用 `getDecorView()` 确保 Feature 已结算；顶层且有 `FEATURE_ACTION_BAR` 时才创建 `WindowDecorActionBar`，否则返回。固定 No Title 路径在 `B_bar` 跳过创建，随后才形成 `S_return`。若 ActionBar 构造抛异常，PhoneWindow 内容可已完整提交，但开发者的 Activity API 仍未正常返回。

## 13. S_return 只完成本地树；它仍位于 onCreate 与窗口 add 之前

固定路径在几个完成点的对象可用性如下：

| 完成点 | PhoneWindow Decor | 系统 root/content | 业务树 | `Activity.mDecor` | ViewRoot | WMS 客户端主 WindowState | 首帧 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `A_attach` | 否 | 否 | 否 | 否 | 否 | 否 | 否 |
| `D_shell` | 是 | 否 | 否 | 否 | 否 | 否 | 否 |
| `G_layout` | 是 | 是 | 否 | 否 | 否 | 否 | 否 |
| `V_content` | 是 | 是 | 是 | 否 | 否 | 否 | 否 |
| `S_return` | 是 | 是 | 是 | 否 | 否 | 否 | 否 |
| `W_add` | 是 | 是 | 是 | 是 | 是 | 是 | 否 |
| `F_present` | 是 | 是 | 是 | 是 | 是 | 是 | 是 |

`S_return` 还能证明：

- 本次业务构树、Insets 请求调用和内容 callback 均正常返回；
- 两个 set 主实现的 explicit gate 已锁定；
- Activity 的 ActionBar 初始化检查已完成；
- Window 的本地 `LayoutParams`、background 与 Feature 账已经可供后续 add 使用。

它不能证明 measure/layout/draw 已发生。固定新 Decor 没有 ViewRoot，`Activity.mDecor` 也要到 `handleResumeActivity()` 窗口支路才从 `r.window.getDecorView()` 赋值。`WindowManagerGlobal.addView()` 随后创建并登记 ViewRoot，再调用 `ViewRootImpl.setView()`；`setView()` 先用 `requestLayout()` 排一次 traversal，接着同步经 session 请求 WMS `addWindow()`。WMS 的 add 阶段创建 `WindowState`，却明确把布局留给后续 relayout。

主 Looper 以后执行首次 traversal，才推进 attach/初始 Insets、measure、relayout 取 Surface、必要的再测量、layout 与 pre-draw；只有 pre-draw 未取消且窗口可见时，才在同趟进入 draw。若 pre-draw 取消但窗口仍可见，本分支跳过 draw 并重新调度 traversal；若窗口不可见，则同样跳过 draw，但该分支本身不保证重新调度。即使走到 `finishDrawing()`，它也仍不是 SurfaceFlinger 或硬件 present。于是 `W_add` 与 `F_present` 之间仍有多组独立失败点。

“本地树阶段没有必需的 WMS IPC”也不等于调用栈绝无跨进程工作。自定义 View 构造、公开 Factory、`onContentChanged()` 与其他应用回调都可主动调用 Binder；本章完成点只描述框架必经结构链，不给任意开发者代码添加“无 IPC”保证。

把第 209 章的生命周期点接回来，调用位于 `onCreate()` 内时有：

```text
A_attach < S_return < 子类 onCreate 末尾
         < O_create < M_commit < T_start < U_resume < W_add < F_present
```

所以 `setContentView()` 返回甚至早于开发者 `onCreate()` 返回，更不可能自动证明客户端 Create commit、Resume 或上屏。system_server 若已把 ActivityRecord 早置为 `RESUMED`，也只是另一进程的服务端账，不能填补本地窗口证据。

## 14. preserved、NoDisplay、starting Window 与异常会改写哪些关系

固定结论不能无条件外推到所有路径：

| 分支 | r48 的差异 | 不能沿用的固定断言 |
|---|---|---|
| preserved Window relaunch | 旧 Activity 销毁时先 `clearContentView()`，保留框架控制的 Decor 部分；新 PhoneWindow 取旧 Decor/elevation/token 并强制重装骨架 | `D_shell` 必然 new Decor，或 `S_return` 时必无 ViewRoot |
| preserved resume | `mDecor.setWindow(newPhoneWindow)` 重绑 DecorContext；resume 跳过本次 `wm.addView()`，改为 `notifyChildRebuilt()` | 每次 launch 都有新的 `W_add` 调用 |
| 提前 `getDecorView()` | 系统骨架先建，首次 setter 复用 | `P_set` 后才首次出现 Decor |
| Content Transitions | Scene 取代普通 remove/add；首次无动画，后续动画异步 | setter 返回等于 transition 结束 |
| NoDisplay | `Activity.performCreate()` 在开发者 `onCreate()` 返回后才由 `windowNoDisplay` 写 `Activity.mVisibleFromClient=false` | setContentView 当下已知道最终可见性 |
| NoDisplay 新目标版本 | targetSdk > 22 且 `onResume()` 完成前未 finish 会抛 `IllegalStateException` | 无 UI Activity 可正常保持 resumed |
| NoDisplay legacy 且未 finish | fresh-window 条件仍可先取得 Decor，再因 `mVisibleFromClient=false` 跳过普通 add | 取得 Decor 必有 WMS 主窗口 |
| NoDisplay 已 finish | `!a.mFinished` 条件失败，resume 的整个 fresh-window 分支跳过 | resume 必再取得 Decor 或尝试 add |
| starting/snapshot/splash Window | 可由 system_server 在客户端主窗口前建立，是另一 WindowState/Surface | 启动画面就是此 PhoneWindow 的 Decor |
| inflate/callback/ActionBar 异常 | 已创建或已挂接对象不自动回滚；完成点停在第一处未返回调用之前 | 看见部分树就等于 `S_return` |

preserved 路径尤其要按代际读：旧销毁阶段 `DecorView.clearContentView()` 删除业务与多数旧系统子树，只保留框架 color/status guard 等受控 View；新 `installDecor()` 调 `mDecor.setWindow(this)`，再建新系统 root/content。`DecorView.setWindow()` 会让 `DecorContext` 指向新 PhoneWindow/Activity。它复用的是已挂接的窗口根基础设施，不是把旧业务树原封不动交给新 Activity。

NoDisplay 的时序同样重要：开发者可以在 `onCreate()` 中先构出完整本地树，`performCreate()` 才在回调返回后读取 no-display 属性并写客户端 `Activity.mVisibleFromClient`。system_server 还会独立解析并保存 `ActivityRecord.noDisplay`；服务端可见性账与客户端字段不能互相替代。对象存在与最终可见性从来不是一回事。

## 15. 用最小证据定位“内容已设但没显示”，并交给第 211 章

先按最小证据判断卡点：

| 证据 | 至少说明 | 仍不能说明 |
|---|---|---|
| `requestWindowFeature()` 返回 true | `F_accept` | 骨架已选择 |
| `PhoneWindow.mDecor != null` | Decor 外壳至少已创建或复用 | system root/content 已完整 |
| `findViewById(android.R.id.content) != null` | 系统骨架与统一 content 可查 | 业务 root 已加入 |
| 业务 View 的 `getParent()` 是 content | 对应树已挂接 | ViewRoot/WMS 已存在 |
| `onContentChanged()` 入口日志 | 树变更已走到 callback | explicit flag或 Activity API 已返回 |
| `Activity.setContentView()` 下一行日志 | `S_return` | `onCreate`、Resume、窗口 add 或首帧 |
| Decor 的 `getViewRootImpl() == null` | 尚未 attach 到 ViewRoot | 业务构树失败 |
| `Activity.mDecor` 已写且 ViewRoot 存在 | resume 窗口支路已推进 | WMS add 必然成功或首帧已出 |
| WMS 中有客户端主 `WindowState` | add 已被接受 | relayout、Buffer 或 present 已完成 |
| frame/present 证据 | 对应绘制或显示点已到 | 更早 Theme/inflate 没有耗时 |

调查顺序可以缩成四问：

1. Feature/Theme 是否在第一次 Window style 与 Decor 安装前确定？
2. PhoneWindow 是停在 Decor 外壳、系统 skeleton，还是业务 content？
3. callback、explicit gate 与 Activity wrapper 是否正常返回？
4. 问题属于本地构树，还是已经越过 ViewRoot/WMS 进入绘制链？

本章到 `S_return` 为止详细解释 PhoneWindow、Decor 与 content，只把 `W_add/F_present` 当下游边界。第 211 章从 `mLayoutInflater.inflate()` 内部继续：XML 怎样经 Resources/XmlBlock、Factory 链、Context 包装、反射构造和 LayoutParams 变成真实 View 对象。

## 16. 九组只读练习：亲手重建 Feature、Decor、content 与返回边界

以下命令只做文件存在检查和文本检索。可以在仓库根目录运行，也可先设置 `ANDROID_BUILD_TOP`；每组都应在 Android 11 r48 快照中独立以 0 退出。

### 练习 1：从 Activity.attach 追到 PhoneWindow 门面

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
test -f "$A" && test -f "$P"
grep -nE 'new PhoneWindow|setCallback\(this\)|setPrivateFactory|setWindowManager|public void setContentView|initWindowDecorActionBar|requestWindowFeature' "$A"
grep -nE 'public PhoneWindow\(Context|mLayoutInflater = LayoutInflater.from|mUseDecorContext|DEVELOPMENT_RENDER_SHADOWS_IN_COMPOSITOR|DEVELOPMENT_FORCE_RESIZABLE_ACTIVITIES' "$P"
```

把 `A_attach` 后已有的 PhoneWindow、callback、Inflater 和 local WindowManager，与尚未出现的 Decor 分成两栏；再标出 Activity wrapper 的尾部调用。

完成标准：答案必须说明 PhoneWindow 构造可读 Settings/PackageManager，但固定新窗口不在构造时安装 Decor。

### 练习 2：证明 Decor 懒安装与查询副作用

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
W="$SRC/frameworks/base/core/java/android/view/Window.java"
test -f "$P" && test -f "$W"
grep -nE 'public final @NonNull View getDecorView|mDecor == null|mForceDecorInstall|installDecor\(\)|generateDecor\(-1\)|mContentParent = generateLayout|peekDecorView' "$P"
grep -nE 'findViewById\(@IdRes int id\)|getDecorView\(\)\.findViewById|public abstract @NonNull View getDecorView|public abstract View peekDecorView' "$W"
```

画出 `getDecorView/findViewById/setContentView/resume` 到 `installDecor` 的汇合图，并把 `peekDecorView` 标成只读分支。

完成标准：能解释“Decor 已安装但 content 仍为空”为什么成立，以及它为何不等于 explicit gate 已锁。

### 练习 3：拆开 DecorContext 与 preserved Window 代际

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
D="$SRC/frameworks/base/core/java/com/android/internal/policy/DecorContext.java"
V="$SRC/frameworks/base/core/java/com/android/internal/policy/DecorView.java"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
test -f "$P" && test -f "$D" && test -f "$V" && test -f "$T"
grep -nE 'PhoneWindow\(Context context, Window preservedWindow|preservedWindow != null|preservedWindow.getDecorView|mForceDecorInstall = true|protected DecorView generateDecor|new DecorContext|return new DecorView' "$P"
grep -nE 'class DecorContext extends ContextThemeWrapper|WeakReference|setPhoneWindow|createDisplayContext|attachBaseContext|getResources' "$D"
grep -nE 'void setWindow\(PhoneWindow|void clearContentView|removeViewAt' "$V"
grep -nE 'mPreserveWindow|mPendingRemoveWindow = r.window|clearContentView|notifyChildRebuilt' "$T"
```

画旧 Activity、旧 Decor、新 PhoneWindow 与新业务树的代际关系；注明哪些引用重绑、哪些子树清除、哪一步不再 add。

完成标准：不能把 DecorContext 说成切断了所有 Activity 强引用，也不能把 preserved 说成完整保留旧业务树。

### 练习 4：还原 Theme、Feature、Flag 与调用时机

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
W="$SRC/frameworks/base/core/java/android/view/Window.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
C="$SRC/frameworks/base/core/java/android/view/ContextThemeWrapper.java"
test -f "$P" && test -f "$W" && test -f "$A" && test -f "$T" && test -f "$C"
grep -nE 'TypedArray a = getWindowStyle|Window_windowIsFloating|Window_windowNoTitle|Window_windowActionBar|mContentParentExplicitlySet|requestFeature\(\) must be called before adding content|FEATURE_CUSTOM_TITLE|removeFeature\(FEATURE_ACTION_BAR\)|VERSION_CODES.HONEYCOMB|targetPreL|targetPreQ|mForcedStatusBarColor|mForcedNavigationBarColor|mAlwaysReadCloseOnTouchAttr|getForcedWindowFlags|hasSoftInputMode' "$P"
grep -nE 'public final TypedArray getWindowStyle|mWindowStyle == null|public boolean requestFeature|final int flag = 1<<featureId|mFeatures|mLocalFeatures|mForcedWindowFlags' "$W"
grep -nE 'public void setTheme\(int resid\)|mWindow.setTheme|requestWindowFeature' "$A"
grep -nE 'getThemeResource|theme != 0|activity.setTheme\(theme\)' "$T"
grep -nE 'public Resources.Theme getTheme|Resources.selectDefaultTheme|initializeTheme' "$C"
```

先拆出 manifest 主题 id 非零时的显式设置线，以及 id 为零时 `ContextThemeWrapper` 的延迟默认主题线；再画 Theme→Feature、Theme→Flag 和显式 API→forced 优先账，并解释 No Title/ActionBar 冲突在哪一层解决。

完成标准：指出 `generateLayout` 是主要投影点而非唯一 Theme 读取点，写出 split touch、系统栏背景、透明栏对比度与 close-on-touch 的准确 targetSdk 门，并区分 flag、栏色和 soft input 的独立显式优先账；还要说明先 Decor 后 Feature 为什么可能不抛却仍不安全。

### 练习 5：比较系统骨架并找到统一 content

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
S="$SRC/frameworks/base/core/res/res/layout/screen_simple.xml"
T="$SRC/frameworks/base/core/res/res/layout/screen_title.xml"
A="$SRC/frameworks/base/core/res/res/layout/screen_action_bar.xml"
C="$SRC/frameworks/base/core/res/res/layout/screen_custom_title.xml"
R="$SRC/frameworks/base/core/res/res/layout/screen_progress.xml"
I="$SRC/frameworks/base/core/res/res/layout/screen_title_icons.xml"
O="$SRC/frameworks/base/core/res/res/layout/screen_simple_overlay_action_mode.xml"
test -f "$P" && test -f "$S" && test -f "$T" && test -f "$A" && test -f "$C" && test -f "$R" && test -f "$I" && test -f "$O"
grep -nE 'R.layout.screen_title_icons|R.layout.screen_progress|R.layout.screen_custom_title|R.layout.screen_action_bar|R.layout.screen_title|R.layout.screen_simple_overlay_action_mode|R.layout.screen_simple|ID_ANDROID_CONTENT' "$P"
grep -nE '@android:id/content|action_mode_bar' "$S" "$T" "$C" "$R" "$I" "$O"
grep -nE '@android:id/content|decor_content_parent|ActionBarOverlayLayout|ActionBarContainer|ActionBarView|ActionBarContextView' "$A"
```

按 `getLocalFeatures()` 的 if/else 顺序给正文八个 selector 表项排序，再把脚本读取的各份骨架中的统一 content 与可选装饰区域标出来；floating 分支则记录其布局资源来自 Theme 属性。

完成标准：答案必须是“选择一份骨架，再在其中找 content”，不能画成多个 Feature 布局同时平铺。

### 练习 6：证明 DecorView 怎样装入系统骨架并完成接线

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
D="$SRC/frameworks/base/core/java/com/android/internal/policy/DecorView.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
test -f "$D" && test -f "$P"
grep -nE 'class DecorView extends FrameLayout|void onResourcesLoaded|inflater.inflate\(layoutResource, null\)|mDecorCaptionView.addView|addView\(root, 0|mContentRoot = \(ViewGroup\) root' "$D"
grep -nE 'mDecor.startChanging|mDecor.onResourcesLoaded|findViewById\(ID_ANDROID_CONTENT\)|content container view|mDecor.finishChanging|decor_content_parent|setWindowCallback|initFeature|FEATURE_ACTIVITY_TRANSITIONS' "$P"
```

把 `D_shell`、`G_layout`、`I_decor` 分成三个检查点，注明 caption、content 查找、title/Feature 接线分别发生在哪一段。

完成标准：能说明 `startChanging/finishChanging` 是 Decor 内部协调，并非 WMS 或 Surface 事务。

### 练习 7：重建 XML attach 与 Activity wrapper 返回点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
test -f "$A" && test -f "$P" && test -f "$L"
grep -nE 'public void setContentView|public void addContentView|getWindow\(\)\.setContentView|getWindow\(\)\.addContentView|initWindowDecorActionBar' "$A"
grep -nE 'public void setContentView\(int|mLayoutInflater.inflate\(layoutResID, mContentParent\)|mContentParent.addView\(view, params\)|mContentParent.removeAllViews' "$P"
grep -nE 'public View inflate\(@LayoutRes int resource, @Nullable ViewGroup root\)|root != null|boolean attachToRoot|TAG_MERGE|root.addView\(temp, params\)|View result = root|mPrivateFactory|createViewFromTag|tryCreateView|TAG_INCLUDE|TAG_REQUEST_FOCUS|TAG_TAG|TAG_1995' "$L"
```

分别走普通单根 XML 与 `<merge>`，标出 LayoutParams 由谁生成、子树何时 attach、inflate 返回值是什么；最后接上 Activity 的 ActionBar 尾部。

完成标准：不能把 inflate 返回值当业务根，也不能说所有合法 XML 都有一个单独根 View。

### 练习 8：区分替换、追加、Scene、Insets 请求与 explicit 锁

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
S="$SRC/frameworks/base/core/java/android/transition/Scene.java"
T="$SRC/frameworks/base/core/java/android/transition/TransitionManager.java"
V="$SRC/frameworks/base/core/java/android/view/View.java"
R="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
test -f "$P" && test -f "$S" && test -f "$T" && test -f "$V" && test -f "$R"
grep -nE 'FEATURE_CONTENT_TRANSITIONS|Scene.getSceneForLayout|new Scene\(mContentParent, view\)|mContentScene == null|scene.enter\(\)|mTransitionManager.transitionTo|removeAllViews|addContentView\(View view|does not support content transitions|mContentParentExplicitlySet = true' "$P"
grep -nE 'public void enter\(\)|removeAllViews|inflate\(mLayoutId|mSceneRoot.addView|No transition will be run' "$S"
grep -nE 'private static void changeScene|sPendingTransitions.contains|Scene oldScene|scene.enter\(\)|sPendingTransitions.add|transitionTo\(Scene scene\)' "$T"
grep -nE 'public void requestApplyInsets|requestFitSystemWindows|mParent != null|mParent.requestFitSystemWindows' "$V"
grep -nE 'public void requestFitSystemWindows|mApplyInsetsRequested = true|scheduleTraversals' "$R"
```

对首次 Scene、后续 Scene、同 root 已 pending、普通替换和 add 画五条支路；再把 Insets 请求在“有/无 ViewRoot”时的终点分开。

完成标准：明确首次 Scene 同步进树但不运行动画，pending guard 会忽略同帧后续换 Scene，而未 attach 的 Insets 请求也不会凭空保存一个 ViewRoot pending 位。

### 练习 9：从 S_return 追到 WMS，并单列 NoDisplay

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
test -f "$A" && test -f "$T" && test -f "$G" && test -f "$V" && test -f "$S" && test -f "$M"
grep -nE 'mVisibleFromClient = !mWindow.getWindowStyle|without a UI must call finish|targetSdkVersion|did not call finish\(\) prior to onResume' "$A"
grep -nE 'public void handleResumeActivity|r.window.getDecorView|a.mDecor = decor|a.mWindowAdded = true|mVisibleFromClient|wm.addView\(decor, l\)|notifyChildRebuilt' "$T"
grep -nE 'new ViewRootImpl|mViews.add|mRoots.add|root.setView' "$G"
grep -nE 'public void setView|requestLayout\(\)|addToDisplayAsUser|ADD_BAD_APP_TOKEN|ADD_INVALID_DISPLAY' "$V"
grep -nE 'addToDisplayAsUser|mService.addWindow' "$S"
grep -nE 'public int addWindow|new WindowState|return res' "$M"
```

写出固定冷启动的 `S_return → W_add` 链，再给 preserved 与 NoDisplay 各画一条绕行；到 WMS `addWindow` 返回即停，不追首帧。

完成标准：答案必须区分 PhoneWindow.mDecor、Activity.mDecor、ViewRoot、WindowState 与 present，并说明 NoDisplay 在开发者 `onCreate()` 返回后才结算。
