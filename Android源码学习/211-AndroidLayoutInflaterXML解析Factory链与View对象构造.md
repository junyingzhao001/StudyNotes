# 211 Android LayoutInflater：XML 解析、Factory 链与 View 对象构造

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只读源码，可以证明资源选择、Parser 所有权、Factory 优先级、构造与父子提交的同步关系；不能据此测量某台设备的 inflate 耗时，也不能把“业务树已挂到 content”扩大成 ViewRoot、measure/layout/draw、Surface 或首帧已经完成。

第 210 章停在 `PhoneWindow` 的这行：

```java
mLayoutInflater.inflate(layoutResID, mContentParent);
```

本章只追一个问题：**固定普通首次 `setContentView(int)`，这次 `inflate(resource, root)` 正常返回时，`R.layout` 整数怎样变成带 Context、LayoutParams 和父子关系的 View 树；返回的究竟是业务根还是传入的 content；为什么这仍不是“页面已经显示”？**

## 1. 固定一次普通 inflate，用十六个完成点拆开“布局已加载”

先固定 `B_target`，让主线只有一个答案：

| 维度 | 固定值或前提 |
|---|---|
| 上游 | 第 210 章固定首次 `setContentView(int)`；`installDecor()` 已返回，`mContentParent` 是非空 `FrameLayout`，但 Decor 尚无 ViewRoot |
| Inflater | Activity Context 对应的 `PhoneLayoutInflater`；Activity private Factory 已安装，且 Activity 对非 fragment 使用基类回调并返回 null；无公开 Factory/Factory2 与 Filter |
| 资源 | 有效 `R.layout`；当前配置能选出一份编译 XML；使用 r48 常规路径，预编译布局未启用 |
| XML | 普通单根，非 `<merge>`、`<include>`、`<view>`、`<fragment>`、`<blink>` 等特殊标签；没有标签级 `android:theme` |
| View | 短名平台 View 或完整限定名业务 View 都存在，并提供 public `(Context, AttributeSet)` 构造器 |
| 参数 | 调用两参数重载，`root=mContentParent`，所以 `attachToRoot=true` |
| 执行 | App 主线程；Factory、构造器、`generateLayoutParams()`、`onFinishInflate()` 与 `addView()` 均正常返回 |
| 下游 | Activity 不 finish，后续正常 Resume、窗口 add 与首帧显示 |

定义十六个完成点：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `I_ready` | PhoneLayoutInflater、Activity private Factory 与 content root 已就绪 | 业务 XML 已解析 |
| `L_enter` | `inflate(layoutResID, mContentParent)` 资源重载开始 | 资源变体已选中 |
| `R_select` | `getValue(id, value, true)` 已把 id 解析为当前配置的 file 与 assetCookie | Parser 已存在 |
| `P_open` | `ResourcesImpl` 从缓存或新 XmlBlock 创建的 Parser 已返回 | 已到根标签 |
| `X_root` | Parser 从当前位置推进到首个根 `START_TAG` | 根 View 已构造 |
| `V_root` | 固定根标签经 private Factory 放行、默认类创建并从构造器返回 | 根 LayoutParams 或孩子已完成 |
| `Q_params` | 外部 content 已根据根标签 attrs 生成根 LayoutParams | 业务根已挂到 content |
| `D_tree` | 所有后代都完成创建、自己的递归与向 XML 父节点的加入 | XML 根 `onFinishInflate()` 已返回 |
| `O_finish` | XML 根的 `onFinishInflate()` 正常返回 | 已加入外部 content |
| `A_content` | `mContentParent.addView(temp, params)` 正常返回 | Parser 已关闭 |
| `I_inner` | `inflate(parser, root, true)` 已计算返回值并离开核心构树段 | 资源重载已经返回 |
| `P_close` | 资源重载的 finally 已关闭本次 Parser 计数持有 | XmlBlock 必已销毁 |
| `L_return` | 两参数资源 inflate 正常返回传入的 content root | PhoneWindow 的 Insets/callback/explicit 尾部已完成 |
| `S_return` | 第 210 章的 `Activity.setContentView(int)` 正常返回 | ViewRoot 或 WMS 窗口已存在 |
| `W_add` | 后续普通主窗口 add 正常返回，WMS 已接受窗口 | draw 或 present 已完成 |
| `F_present` | 首个目标可见帧达到所选显示完成证据 | 早先 inflate 没有性能问题 |

固定局部全序是：

```text
I_ready < L_enter < R_select < P_open < X_root
        < V_root < Q_params < D_tree < O_finish
        < A_content < I_inner < P_close < L_return
        < S_return < W_add < F_present
```

答案先钉死：标准 XML 路径的 `L_return` 返回 **传入的 content**，不是业务 XML 根；业务根此时已经带着 content 生成的 LayoutParams 挂入 content，内部子树和普通节点的 `onFinishInflate()` 也已同步完成。Parser 已关闭，但缓存中的 XmlBlock 可以继续存活。Decor 仍没有 ViewRoot，因此 measure、layout、draw、Surface 和 present 都不由这次返回证明。

## 2. 资源、游标、对象与父容器是六类不同证据

“XML 变成 View”至少跨六层：

```text
R.layout 整数
→ TypedValue(file, assetCookie)
→ XmlBlock（编译 XML 与字符串池的 native 包装）
→ XmlBlock.Parser（独立 parse state，同时充当 AttributeSet）
→ Java View / ViewGroup 对象
→ 父容器解释出的 LayoutParams 与 addView 父子关系
```

固定返回点的账如下：

| 对象或账 | 角色 | `L_return` 状态 |
|---|---|---|
| layout resource id | package/type/entry 身份 | 已解析 |
| TypedValue | 当前配置选中的 file、cookie 与类型 | 临时对象已归还 Resources 池 |
| XmlBlock cache | 最近编译 XML block 的四槽循环缓存 | 可能命中或新写入 |
| Parser/native parse state | 当前调用的事件游标与 AttributeSet | 已关闭 |
| Inflater base Context | 资源、ClassLoader 与默认 Theme 来源 | 仍由 Inflater 持有 |
| 每标签 viewContext | 标签级 Theme 传给 Factory/构造器的候选 Context | 根固定为 Activity；后代从 XML parent 的实际 Context 起步 |
| View 实例 | 构造器与自身属性状态 | 全部已创建 |
| LayoutParams | 由将接纳该孩子的 ViewGroup 解释 `layout_*` | 已生成并写入/用于 add |
| XML 内部父子关系 | 业务根以下的层级 | 已建立 |
| 外部 content 关系 | 业务根到 `mContentParent` 的边 | 已建立 |
| ViewRoot/WindowState | View 树到窗口 session/WMS 的连接 | 尚不存在 |

`AttributeSet` 不是事后复制出的 Map；固定 Parser 本身实现该接口，属性读取跟随同一 native 游标当前位置。`View` 构造器应在当前标签仍有效时读取所需属性，不能把这个对象当成稳定快照长期保存。

## 3. PhoneLayoutInflater 来自 Activity 的可视 Context，private Factory 随 attach 接入

普通 Activity 的 ContextImpl 在 `activity.attach()` 之前先执行：

```java
appContext.setOuterContext(activity);
activity.attach(appContext, ...);
```

base ContextImpl 的 LayoutInflater 服务注册使用 outer Context：

```java
return new PhoneLayoutInflater(ctx.getOuterContext());
```

但 Activity 自身继承 `ContextThemeWrapper`。它查询 LayoutInflater 时会先从 base Context 取得上述 PhoneLayoutInflater，再克隆并缓存在 wrapper：

```java
mInflater = LayoutInflater.from(getBaseContext()).cloneInContext(this);
```

`PhoneWindow` 构造从 Activity Context 取得的是这份 wrapper clone，`Activity.attach()` 再把自己接成 private Factory：

```java
mLayoutInflater = LayoutInflater.from(context);
mWindow.getLayoutInflater().setPrivateFactory(this);
```

所以固定 `I_ready` 可以断言：

- 最终 Inflater 是 Activity `ContextThemeWrapper.mInflater` 缓存的 `PhoneLayoutInflater` clone，base Context 是 Activity；
- base ContextImpl 也会缓存提供 clone 原型的 PhoneLayoutInflater；两者是本地对象且不是同一实例，更不是向 system_server 取远程 Binder 对象；
- Activity 作为 private `Factory2` 已在业务 `onCreate()` 前接入；
- content 是创建对象的外部 root，但尚未连接 ViewRoot。

这条 ContextThemeWrapper clone 正是固定 Activity 路径，不是旁支。clone 会复制原型已有的公开/私有 Factory 与 Filter，并换掉 base Context；它不是只改一个字段。后续某个标签带 `android:theme` 时，Inflater 只是把包装 Context 交给该标签创建者，并不会为每个标签自动再克隆一把 Inflater；标签递归仍由当前这一个实例驱动。

## 4. R.layout 先选择当前配置，再按 cookie 与 file 打开编译 XML

`Resources.getLayout(id)` 只是入口：

```java
return loadXmlResourceParser(id, "layout");
```

内部取得临时 `TypedValue`，调用 `ResourcesImpl.getValue(id, value, true)` 解析引用。资源 id 编码 package/type/entry；AssetManager 结合已应用的 Configuration 在候选中选最佳项，最终要求 value 类型为 `TYPE_STRING`，再把：

```text
file + resource id + assetCookie
```

交给 `ResourcesImpl.loadXmlResourceParser()`。因此：

- id 不是文件偏移量；
- `layout/foo` 不保证永远对应同一变体，方向、night、density 等配置会影响选择；
- assetCookie 标识所选资源包位置，file 是该包中的资源路径；
- 选择发生在 `getValue` 链，后面的 `openXmlBlockAsset(cookie, file)` 按已选结果打开，并不重新做一次变体竞赛。

运行时读取的是 aapt 预处理后的编译 XML，不是工程纯文本逐字符直译。Parser 提供高层事件与已编码属性；注释、空白与类型信息的行为也不能照普通文本 PullParser 推断。

## 5. 四槽缓存保存 XmlBlock，不保存 Parser 或 View

`ResourcesImpl` 为每个实例维护四槽循环缓存：

```java
private static final int XML_BLOCK_CACHE_SIZE = 4;
```

缓存键是 `assetCookie + file`。命中时仍调用 `cachedXmlBlocks[i].newParser(id)`，所以每次 `getLayout()` 都得到新的 Parser/native parse state；未命中时打开新 XmlBlock，覆盖下一槽并关闭旧 block。它不是 LRU，也不缓存已经生成的 View 树。

生命周期必须按 open count 描述：

- XmlBlock 创建时 owner count 为 1；
- 每个 `newParser()` 创建独立 native parse state，并把 count 加一；
- `Parser.close()` 销毁自己的 parse state，再释放这一份计数持有；
- 缓存淘汰或 flush 调 `XmlBlock.close()`，释放 block owner 的一份计数；
- 只有 count 归零才销毁底层 native XML tree，并通知 AssetManager。

这不是“没有 Java 引用就立即释放”。Parser 即使 close 后仍有 final `mBlock` 字段；决定 native tree 销毁的是 open count。反过来，所有 Parser 都关闭而 block 仍在缓存时，owner count 仍让它存活；block 已被淘汰但仍有活动 Parser 时，Parser 的计数又会保护 native tree。

## 6. 资源重载拥有 Parser；Parser 重载不替调用者关闭

资源 id 重载的真实骨架是：

```java
View view = tryInflatePrecompiled(resource, res, root, attachToRoot);
if (view != null) {
    return view;
}
XmlResourceParser parser = res.getLayout(resource);
try {
    return inflate(parser, root, attachToRoot);
} finally {
    parser.close();
}
```

所以正常 `I_inner` 先产生返回值，finally 再到 `P_close`，最后才有 `L_return`。标准构树抛出 Exception 时，已经取得的 Parser 同样关闭。

所有权还有三个边界：

- `res.getLayout(resource)` 位于 try 之前；若资源查找/打开本身抛 `Resources.NotFoundException`，还没有 Parser 可供这段 finally 关闭；
- 直接调用 `inflate(XmlPullParser, root, attachToRoot)` 时，Inflater 不关闭传入 Parser，所有权仍在调用者；
- `parseInclude()` 自己打开的 child Parser 由它自己的 finally 关闭。

`XmlBlock.Parser.next()` 到 `END_DOCUMENT` 会自动 close，但普通 `rInflate()` 通常在当前元素的匹配 `END_TAG` 就返回，不能依赖“读到文档末尾”代替显式 finally。

核心 Parser 重载把整次标准递归放在 `synchronized (mConstructorArgs)` 内。这会让同一个 Inflater 的这段共享参数/`mTempValue` 使用不交错，却不能推导 LayoutInflater 已线程安全：类契约仍要求单实例只由一个线程使用，直接 `createView()`、Factory 变更及不同 Inflater 共享的静态 constructor map 也不由这把实例锁统一保护。

## 7. advanceToRootNode 只向前找；AttributeSet 始终跟着游标

进入 Parser 重载后，Inflater 先做：

```java
final AttributeSet attrs = Xml.asAttributeSet(parser);
advanceToRootNode(parser);
final String name = parser.getName();
```

`advanceToRootNode()` 从 Parser **当前位置**反复 `next()`，直到遇见 `START_TAG` 或 `END_DOCUMENT`。它不 rewind，也不读取 depth；如果调用者已把 Parser 停在某个 `START_TAG`，它会先越过当前事件再向后找。递归边界才使用 `getDepth()`。

固定根不是 `merge`，因此先调用 `createViewFromTag(root, name, inflaterContext, attrs)`。对一个普通标签的局部顺序是：

```text
标签名规范化
→ 可选标签 Theme Context
→ 特殊 blink / Factory / private Factory / 默认创建择一
→ View 构造完成
→ 外层父容器生成 LayoutParams
→ 深度优先构造孩子
→ 当前 View.onFinishInflate()
→ 当前 View 加入 XML 外层父节点
```

这不是全树的一条平铺序列。一个孩子会先完成自己的整棵子树和 `onFinishInflate()`，随后才加入它的 XML 父 ViewGroup；较早兄弟已经加入时，较晚兄弟才开始。

## 8. 标签 Theme 是创建输入，不保证成为 Factory 产物的 Context

`createViewFromTag()` 先处理两件事：

1. `<view class="...">` 把实际 class 属性改写为标签名；
2. 未要求忽略时读取 `android:theme`，必要时创建 `ContextThemeWrapper`。

随后这个 `context` 被传给公开 Factory、private Factory 与默认构造路径。固定路径没有标签 Theme，Activity private Factory 又返回 null，所以默认反射确实把 Activity Context 传进根标签的 public 二参数构造器；后代每次改从 XML parent 的实际 `getContext()` 起步。只有沿途父 View 都保留收到的构造 Context，后代构造入参才继续等于 Activity Context；业务 View 若把另一个 Context 传给 `super`，会从该层改变后代的起点。

通用路径不能写成“有 `android:theme` 就保证 `view.getContext()` 是该 wrapper”。Factory 可以忽略收到的 context，自行返回一个使用其他 Context 的 View。Inflater 接下来又通过：

```java
rInflate(parser, parent, parent.getContext(), attrs, finishInflate);
```

让 **Factory 实际返回对象的 `getContext()`** 成为其 XML 后代的递归 Context。标签 Theme 是提供给创建者的输入；只有创建者采用它，继承链才按预期继续。

因此 `view.getContext()` 可能是 Activity、ContextThemeWrapper 或 Factory 选择的别的 Context。业务代码不应无条件强转 Activity；Inflater 的 class lookup Context、构造参数 Context 和 View 最终持有的 Context 也必须分别标注。

## 9. 普通标签的创建链是短路选择，不是所有 Factory 依次必经

`tryCreateView()` 的优先级是：

```text
name == "blink" ? 直接 new BlinkLayout
: mFactory2 != null ? 调 Factory2
: mFactory != null ? 调 Factory
: 无公开 Factory

公开结果为 null
→ mPrivateFactory（若存在）

仍为 null
→ createViewFromTag 的默认创建
```

几个边界决定了真实调用数：

- Factory2 与 Factory 是 `if/else if`，不会对同一标签先后各调一次；
- 任一 Factory 返回非 null，后续创建者被短路，但 Inflater 仍负责该 View 的孩子递归、LayoutParams 与 add；
- `blink` 位于全部 Factory 之前；
- 文档根只由核心入口特判 `merge`；`include/requestFocus/tag` 作为 `rInflate` 子级时才走结构分支。若这些名字出现在文档根，它们仍会进入普通创建链，Factory 可以拦截；没有 Factory 产物时才通常因找不到同名 View 类而失败；
- Factory2 收到的 parent 只是创建语义输入；`root!=null, attachToRoot=false` 时，XML 根仍收到这个 parent，却不会由 Inflater 挂上去。

公开 `setFactory()/setFactory2()` 共用 `mFactorySet`，同一 Inflater 公开设置第二次会抛异常。clone 会复制 `mFactory/mFactory2`，但新对象的 `mFactorySet` 初始仍为 false，所以有一次再设置机会：调用 `setFactory2()` 会同时重建两字段并把新 Factory2 放在旧链之前；若 clone 已带非空 `mFactory2`，却只调用 `setFactory()`，新对象只改 `mFactory`，实际分派仍优先走旧 `mFactory2`，新 Factory 甚至不会被本条路径访问。`setPrivateFactory()` 是隐藏入口，可多次把新 private Factory 合并到旧链之前。

普通 Activity 已是 private Factory2。它对 `fragment` 交给平台 FragmentController；非 fragment 则转旧版 `onCreateView(name, context, attrs)`，Activity 基类默认返回 null，子类仍可覆盖。`AppComponentFactory` 负责 Activity 等组件实例化，不是普通 View 的默认工厂。

Factory 返回的 View 绕过默认反射、constructor cache 与 Filter。Filter 不是全链安全沙箱，也约束不到 `blink`。

## 10. 默认反射分短名与全名；缓存键、ClassLoader 和构造 Context 各自独立

所有创建钩子都返回 null 后：

```java
if (-1 == name.indexOf('.')) {
    view = onCreateView(context, parent, name, attrs);
} else {
    view = createView(context, name, null, attrs);
}
```

`PhoneLayoutInflater` 对短名依次尝试：

```text
android.widget. → android.webkit. → android.app. → android.view.
```

只有 `ClassNotFoundException` 才继续下一个前缀。若某前缀下类存在但不是 View、缺构造器或构造失败，路径会以 InflateException 中止，不会继续“碰运气”。含点完整类名直接按原名创建，和短名前缀链是互斥分支。

默认 `createView()` 的账要拆开：

| 项 | r48 行为 |
|---|---|
| class lookup | `Class.forName(fullName, false, mContext.getClassLoader())`，使用 Inflater base Context 的 ClassLoader |
| 类型门 | `asSubclass(View.class)` |
| 构造器门 | `Class.getConstructor(Context.class, AttributeSet.class)`，要求 **public** 二参数构造器 |
| 构造参数 | 使用当前标签的 themed `viewContext` 与活动 AttributeSet |
| cache key | 静态 Map 以传入的原始 `name` 为键，不含 prefix 与 ClassLoader |
| cache hit | `verifyClassLoader()` 接受声明 loader 是 boot、等于 base Context ClassLoader，或位于其向上 parent 链 |
| Filter | 只在默认反射路径检查；命中 constructor 时用当前 Inflater 的 `mFilterMap` 记允许结果 |
| 实例化 | `constructor.newInstance(args)`，每次仍创建新 View |

`setAccessible(true)` 发生在找到 public constructor **之后**，不会让 private/protected 二参数构造器被 `getConstructor()` 找到。很多 View 的二参构造器内部继续调用三参/四参以解析默认样式；Inflater 自己仍只反射 public 二参入口。

ClassLoader 校验只处理缓存兼容性，不核对 prefix，也不能保证静态 Map 绝不持有旧 Constructor。命中不兼容 lookup 时会移除重载；若再无 lookup，Map 仍可能持有既有元数据。缓存的是 Constructor，不是 View 实例。

`mConstructorArgs` 会临时放入 viewContext 与 attrs；内外层 finally 恢复旧 Context 并清空 attrs 槽，防止递归主题串位和无意长期持有 Parser。构造器本身是应用代码边界，可以同步读资源、创建对象、执行 Binder 或 I/O，也可以抛异常；“inflate 在 App 进程同步执行”不等于“只有纯本地轻量工作”。

## 11. 深度递归决定 onFinishInflate 与 addView 的精确顺序

`rInflate()` 进入时记录当前 `depth`，循环到：

- 当前事件是 `END_TAG` 且 Parser depth 不大于入口 depth；或
- `END_DOCUMENT`。

正常返回时游标停在当前元素匹配的 `END_TAG`。对普通孩子，源码顺序是：

```java
View view = createViewFromTag(parent, name, context, attrs);
ViewGroup viewGroup = (ViewGroup) parent;
ViewGroup.LayoutParams params = viewGroup.generateLayoutParams(attrs);
rInflateChildren(parser, view, attrs, true);
viewGroup.addView(view, params);
```

因此：

- `layout_width/layout_height/layout_gravity` 等由 **即将接纳孩子的父 ViewGroup** 解释，不是孩子基类统一解释；
- 当前孩子的全部后代先加入当前孩子；
- 当前孩子的 `onFinishInflate()` 随 `rInflateChildren(..., true)` 返回前发生；
- 当前孩子随后才加入外层 parent；
- 根 View 的 `onFinishInflate()` 也发生在根加入调用者提供的外部 root 之前。

如果普通 View 标签含普通子标签，`rInflate()` 会把 parent 强转 ViewGroup；非 ViewGroup 因此失败。源码在强转之前已经创建了这个子 View，所以其 Factory/构造器副作用仍可能发生。`requestFocus` 与 `tag` 不要求创建普通孩子，不能把“出现任何子标签”一概等同于 parent 必须是 ViewGroup。`requestFocus` 会先记 pending，当前层元素读完后 `restoreDefaultFocus()`，再调用该 parent 的 `onFinishInflate()`。

`onFinishInflate()` 只证明 XML 构造阶段到达相应局部回调。它不证明 `onAttachedToWindow()`、measure、layout 或 draw。顶层 merge 的最外层调用就是 `finishInflate=false`，所以调用者 root 不会因这次 inflate 收到回调；include 到 merge 的那一层嵌套调用也不额外回调当前 parent，但包围 include 的正常外层 `rInflate(..., true)` 最终仍可对同一 parent 调用一次 `onFinishInflate()`。两种 merge 中实际创建的普通孩子仍各自收到回调。

## 12. root 与 attachToRoot 的矩阵同时决定参数、父子边和返回值

标准 Parser、普通非 merge 根的四种组合是：

| root | attachToRoot | 根 LayoutParams | Inflater 建立外部父子边 | 返回值 |
|---|---:|---|---|---|
| null | false | 不生成 | 否 | XML 根 `temp` |
| null | true | 不生成；true 实际不起 attach 作用 | 否 | XML 根 `temp` |
| 非 null | false | root 生成并 `temp.setLayoutParams(params)` | 否 | XML 根 `temp` |
| 非 null | true | root 生成并传给 `root.addView` | 是 | 传入的 `root` |

两参数 `inflate(resource, root)` 自动令 `attachToRoot = (root != null)`。固定 PhoneWindow 调用因此走最后一行：content 生成 `FrameLayout.LayoutParams`，业务根在全部子树完成后加入 content，返回值却是 content。PhoneWindow 不使用这个返回值。

参数的“生成”和“装到根 View 上”也不是同一点：`root!=null, attach=false` 会在递归前执行 `temp.setLayoutParams(params)`；`attach=true` 只先把 params 留在局部变量，直到最终 `root.addView(temp, params)` 才安装。因此根构造器和根 `onFinishInflate()` 在后一条路径中不能依赖 Inflater 已写入外层 LayoutParams。

这也解释两个常见场景：

- 列表 item 应用 `inflate(layout, recyclerView, false)`：不立即 attach，却取得真正父容器类型的 LayoutParams；
- `inflate(layout, null)` 虽能返回 View，但缺少父容器对 `layout_*` 的解释，后来加入真实父容器时可能丢语义或触发参数不兼容。

`Factory2.parent` 与最终 parent 不能画等号：第三行仍把非空 root 传给根标签 Factory2，却明确不 attach。反过来，`root=null, attach=true` 也不会凭空创造父容器。

`attachToRoot` 只表示加入传入 ViewGroup，不等于第一次连接 ViewRoot。若调用者给的是已经 attached 的 root，最终 `addView()` 可以在 inflate 返回前同步分发 attach；本章固定 Activity content 尚未 attached，所以 `A_content/L_return` 仍没有 ViewRoot。

以上矩阵只认证标准 Parser 路径。r48 常规实例不会进入预编译布局；测试入口若强行启用并成功，`tryInflatePrecompiled()` 即使 `root!=null && attach=true` 也直接返回生成的 `view`，不是标准路径的 root，必须单列。

## 13. merge 与 include 改写“根对象”和提交时机

顶层 `<merge>` 没有自己的 View 对象，只允许：

```text
root != null && attachToRoot == true
```

Inflater 直接 `rInflate(parser, root, ..., false)`，把每个实际孩子依次加入外部 root，最后返回初始 `result=root`。`root==null` 或 `attachToRoot=false` 都抛；递归内部再遇 `merge` 也抛“必须是根元素”。由于 `finishInflate=false`，外部 root 不收到本次 Inflater 的 `onFinishInflate()`。

`<include>` 的 include 语义只在子递归的结构分支解析。它要求当前 parent 是 ViewGroup，解析另一份 layout：

- include 自己的 `android:theme` 若存在，先包 Context，并让被 include 的非 merge 根忽略自己的 theme；
- layout 可直接是资源，也可经主题属性解析；
- 被 include 根是 merge 时，孩子直接加入当前 parent，没有单一根可应用 include 的 id/visibility；
- 普通根时，先尝试用 include 标签 attrs 让外部 parent 生成一整个 LayoutParams 对象；抛 RuntimeException 或返回 null 时，整体改用被 include 根 attrs，不做逐属性合并；
- include 的 id 与 visibility 若提供，会在子树完成后覆盖被 include 根，再 `group.addView(view)`；
- child Parser 在 finally 关闭，外层 include 标签剩余元素再被消费。

这不是文本粘贴。include Theme、LayoutParams、id/visibility 和 Parser 所有权都有单独规则。文档根名为 `include` 时不会自动调用 `parseInclude()`；它会走普通根的 Factory/默认创建。Factory 若主动返回 View 可以成功，固定无 Factory 产物时则因没有同名 View 类而失败。

## 14. requestFocus、tag、blink、ViewStub 与预编译各有不同语义

`rInflate()` 在子级位置还识别：

- `<requestFocus>`：消费其子元素，记录 pending；本层完成时调用 parent.`restoreDefaultFocus()`；
- `<tag>`：读取 key/value，直接对当前 parent `setTag(key, value)`；
- `<blink>`：在 `tryCreateView()` 最前直接创建内部 `BlinkLayout`，绕过公开/private Factory 与 Filter；
- 普通 `ViewStub`：当前只创建零尺寸占位 View，不创建 `android:layout` 指向的目标树。

默认反射创建 ViewStub 后，Inflater 会给它设置 `cloneInContext(actualConstructorContext)`，让日后展开沿用相应 Context、Factory 与 Filter。若 ViewStub 是 Factory 直接返回，这段默认反射后处理不会执行，不能保证自动得到同一 clone。

`ViewStub.inflate()` 日后用 `factory.inflate(layout, parent, false)` 先创建但不 attach，再移除 Stub，并以 Stub 自己原有的 LayoutParams 把新 View 加回同一索引。占位对象创建、目标布局构造和替换完成是三个时间点。

预编译布局则要按版本限定：r48 `initPrecompiledViews()` 明确把 enabled 硬编码为 false，常规生产 Inflater 不会进入该旁路。只有 `@TestApi setPrecompiledLayoutsEnabledForTesting()` 可尝试开启；即使成功且 root 非空，它仍打开 XML 只为生成父参数。不能把测试路径写成 Android 11 应用的默认优化，更不能把它的返回值套入标准矩阵。

测试旁路把任意 `Throwable` 捕获在内部，失败后回落到标准 XML；这保证不了旁路调用过的应用代码副作用被撤销。它与 Parser 核心只包装 `Exception` 的边界也不能混为一谈。

## 15. 异常没有事务回滚；用最小证据定位停点

标准 Parser 核心分别捕获 `XmlPullParserException` 与其他 `Exception`，包装为 `InflateException`；后者通常附带当前资源/行位置。`Error`、`LinkageError`、OOM 不属于 `Exception`，不会由这层包装，但 Java finally 仍恢复 constructor args、结束 trace，并在资源重载中关闭已取得的 Parser。`Resources.getLayout()` 自身抛出的 NotFoundException 也不经过 Parser 核心包装。

Inflater 没有树事务：

| 失败位置 | 框架已经可能留下什么 | 不应声称 |
|---|---|---|
| 资源选择/打开 | 没有本次 Parser 或 View | 构造器失败 |
| 根 Factory/构造器 | 应用回调副作用；尚无框架外部 add | content 已加入根 |
| 普通根内部较晚孩子 | temp 内已有较早兄弟；temp 尚未加入调用者 root | 全部自动回滚 |
| 顶层 merge 的较晚孩子 | 较早孩子已直接留在调用者 root | merge 失败保持 root 原样 |
| `onFinishInflate()` | 当前局部子树已建；当前节点尚未必加入外层 parent | 回调出现等于外部 attach |
| 最终 `root.addView` | 完整 temp 与参数已存在；add 可能失败，回调异常时甚至可能已写入部分父子状态 | `L_return` 已到达 |
| PhoneWindow 重复 set | 它在 inflate 前已 `removeAllViews()` | 新 inflate 失败会恢复旧内容 |

固定首次普通根路径中，直到整棵 temp 与根 `onFinishInflate()` 成功，框架才向 content 做最终 add；这比 merge 的逐孩子提交更接近“外部原子”，却仍不是通用回滚承诺。Factory/构造器可以保存对象引用或自行修改别处，`addView()` 本身也可能抛。默认路径还会在 `constructor.newInstance()` 之前把新 Constructor 放进静态 cache；实例构造随后失败不会自动删除这项元数据。

最小诊断表：

| 证据 | 至少证明 | 仍不能证明 |
|---|---|---|
| `Resources.getLayout()` 返回 Parser | 当前 id 已解析并创建 parse state | 已读到根标签 |
| Factory 日志 | 对应普通标签已到该 Factory | 它返回了 View 或默认路径结束 |
| View 构造日志 | 该实例构造器已进入/返回到日志点 | 其孩子或 LayoutParams 已完成 |
| 根 `onFinishInflate()` 日志 | 普通根内部孩子已加入 | 根已加入外部 content |
| 业务根 `getParent()==content` | 外部父子边至少已经写入 | `addView()` 或 resource inflate 已正常返回 |
| `inflate` 下一行日志 | `L_return`；资源重载 Parser 已关闭 | PhoneWindow 尾部或 Activity API 已返回 |
| `Activity.setContentView` 下一行 | `S_return` | ViewRoot/WMS/首帧 |
| `view.getViewRootImpl()==null` | 尚未 attach 到 ViewRoot | 本地构树失败 |
| WMS 有对应 WindowState | `W_add` 至少已被接受 | draw、Buffer 或 present |

本章的边界是 `L_return`：资源已选、对象已构、参数已解释、固定父子边已提交、Parser 已关闭。第 210 章再接 Insets、callback、explicit 与 ActionBar；ViewRoot、WMS、traversal 和显示链仍在后面。

## 16. 九组只读练习：亲手重建资源、Factory、递归与返回矩阵

以下命令只做文件存在检查和文本检索。可在源码根运行，也可先设置 `ANDROID_BUILD_TOP`；每组应在 Android 11 r48 快照中独立以 0 退出。

### 练习 1：确认 Activity Inflater 的 Context 与 private Factory

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
R="$SRC/frameworks/base/core/java/android/app/SystemServiceRegistry.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
C="$SRC/frameworks/base/core/java/android/view/ContextThemeWrapper.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
F="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneLayoutInflater.java"
test -f "$T" && test -f "$R" && test -f "$A" && test -f "$C" && test -f "$P" && test -f "$L" && test -f "$F"
grep -nE 'setOuterContext\(activity\)|activity.attach\(' "$T"
grep -nE 'Context.LAYOUT_INFLATER_SERVICE|new PhoneLayoutInflater|ctx.getOuterContext' "$R"
grep -nE 'class Activity extends ContextThemeWrapper|getSystemService\(|getLayoutInflater\(\).setPrivateFactory\(this\)|public LayoutInflater getLayoutInflater|onCreateView\(' "$A"
grep -nE 'LAYOUT_INFLATER_SERVICE.equals|LayoutInflater.from\(getBaseContext\(\)\).cloneInContext|return mInflater' "$C"
grep -nE 'mLayoutInflater = LayoutInflater.from|getLayoutInflater\(\)' "$P"
grep -nE 'public static LayoutInflater from|cloneInContext' "$L"
grep -nE 'class PhoneLayoutInflater|sClassPrefixList|cloneInContext' "$F"
```

画 `setOuterContext → base ContextImpl 的 PhoneLayoutInflater → Activity wrapper clone → PhoneWindow → Activity private Factory`；标出两把 Inflater 不是同一实例，且都在 App 进程内。

完成标准：不能把 LayoutInflater 系统服务画成 Binder 服务，也不能把 Activity private Factory 说成创建所有普通 View。

### 练习 2：把资源选择、四槽 XmlBlock 与 Parser 计数拆开

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
R="$SRC/frameworks/base/core/java/android/content/res/Resources.java"
I="$SRC/frameworks/base/core/java/android/content/res/ResourcesImpl.java"
A="$SRC/frameworks/base/core/java/android/content/res/AssetManager.java"
X="$SRC/frameworks/base/core/java/android/content/res/XmlBlock.java"
test -f "$R" && test -f "$I" && test -f "$A" && test -f "$X"
grep -nE 'XmlResourceParser getLayout|loadXmlResourceParser\(id, "layout"\)|impl.getValue\(id, value, true\)|TypedValue.TYPE_STRING|value.assetCookie' "$R"
grep -nE 'XML_BLOCK_CACHE_SIZE = 4|mCachedXmlBlockCookies|mCachedXmlBlockFiles|mCachedXmlBlocks|openXmlBlockAsset|newParser\(id\)|oldBlock.close|flushLayoutCache' "$I"
grep -nE 'openXmlBlockAsset\(int cookie|nativeOpenXmlAsset' "$A"
grep -nE 'mOpenCount = 1|mOpenCount\+\+|nativeCreateParseState|nativeDestroyParseState|decOpenCountLocked|mOpenCount == 0|ev == END_DOCUMENT' "$X"
```

给 cache owner 与两个同时存活的 Parser 各画一份计数持有，再模拟缓存淘汰和两个 Parser 依次 close。

完成标准：指出缓存键、四槽循环策略与每次新 parse state；不能用“Java 引用为零”替代 open-count 条件。

### 练习 3：区分资源重载、Parser 重载与游标起点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
X="$SRC/frameworks/base/core/java/android/util/Xml.java"
test -f "$L" && test -f "$X"
grep -nE 'inflate\(@LayoutRes int resource|tryInflatePrecompiled|XmlResourceParser parser = res.getLayout|parser.close\(\)|inflate\(XmlPullParser parser|synchronized \(mConstructorArgs\)|advanceToRootNode|No start tag found' "$L"
grep -nE 'asAttributeSet\(XmlPullParser parser\)|parser instanceof AttributeSet|new XmlPullAttributes' "$X"
```

分别写出 getLayout 抛错、核心递归抛 Exception、直接传 Parser 三条所有权线；再说明已停在 START_TAG 的 Parser 为什么不会从该标签重新开始。

完成标准：资源重载取得的 Parser 由 finally 关闭，Parser 重载不越权关闭调用者对象；advance 只向前看事件，不用 depth。

### 练习 4：还原普通标签的 Theme 与 Factory 短路链

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
C="$SRC/frameworks/base/core/java/android/view/ContextThemeWrapper.java"
test -f "$L" && test -f "$A" && test -f "$C"
grep -nE 'name.equals\("view"\)|ATTRS_THEME|new ContextThemeWrapper|tryCreateView|name.equals\(TAG_1995\)|mFactory2 != null|else if \(mFactory != null\)|mPrivateFactory|new FactoryMerger|mFactorySet|setPrivateFactory' "$L"
grep -nE 'onCreateView\(@Nullable View parent|!"fragment".equals\(name\)|mFragments.onCreateView|return null' "$A"
grep -nE 'LAYOUT_INFLATER_SERVICE.equals|LayoutInflater.from\(getBaseContext\(\)\).cloneInContext|mInflater' "$C"
```

画 blink、Factory2/Factory 二选一、private Factory 与默认创建的分支；另画 Factory 返回自选 Context 后，后代怎样从 `parent.getContext()` 继续。

完成标准：不能把 Factory2 与 Factory 画成两个必经回调，也不能把标签 Theme 强制等同于最终 `view.getContext()`。

### 练习 5：核对前缀、public 构造器、缓存键与 Filter 边界

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneLayoutInflater.java"
test -f "$L" && test -f "$P"
grep -nE 'name.indexOf|Class.forName|asSubclass\(View.class\)|mContext.getClassLoader|getConstructor\(mConstructorSignature\)|sConstructorMap.get\(name\)|sConstructorMap.put\(name|verifyClassLoader|mFilter.onLoadClass|mFilterMap|mConstructorSignature|constructor.newInstance|view instanceof ViewStub|cloneInContext' "$L"
grep -nE 'sClassPrefixList|android.widget|android.webkit|android.app|catch \(ClassNotFoundException|super.onCreateView' "$P"
```

对一个短名与一个完整类名分别标注 lookup ClassLoader、constructor 参数 Context、cache key 和 Filter 生效点；再写出类存在但缺二参构造器时为何不会试下一前缀。

完成标准：构造器必须是 public `(Context, AttributeSet)`；Filter 只覆盖默认反射，Factory/blink 产物不受它保证。

### 练习 6：证明孩子先完成自己，再加入外层父节点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
G="$SRC/frameworks/base/core/java/android/view/ViewGroup.java"
V="$SRC/frameworks/base/core/java/android/view/View.java"
test -f "$L" && test -f "$G" && test -f "$V"
grep -nE 'final int depth = parser.getDepth|parser.getDepth\(\) > depth|createViewFromTag\(parent|viewGroup.generateLayoutParams|rInflateChildren\(parser, view|viewGroup.addView|pendingRequestFocus|restoreDefaultFocus|parent.onFinishInflate' "$L"
grep -nE 'generateLayoutParams\(AttributeSet attrs\)|addView\(View child, int index, LayoutParams params\)|addViewInner' "$G"
grep -nE 'protected void onFinishInflate|onAttachedToWindow|onMeasure\(' "$V"
```

选一个三层 XML，按事件游标列出每个构造器、LayoutParams、`onFinishInflate` 和 add 的精确顺序。

完成标准：子 ViewGroup 的 `onFinishInflate` 在其后代加入之后、它自己加入外层 parent 之前；不能把它当 attach 或 measure。

### 练习 7：亲算 root × attachToRoot 返回矩阵

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
test -f "$L" && test -f "$P"
grep -nE 'return inflate\(resource, root, root != null\)|View result = root|root != null|root.generateLayoutParams|!attachToRoot|temp.setLayoutParams|root.addView\(temp, params\)|result = temp|TAG_MERGE.equals|ViewGroup root and attachToRoot=true' "$L"
grep -nE 'mLayoutInflater.inflate\(layoutResID, mContentParent\)|mContentParent.addView|mContentParent.removeAllViews' "$P"
```

写满普通根四行矩阵，再单列 merge；把 PhoneWindow 的两参数调用映射到矩阵最后一行。

完成标准：固定 `L_return` 返回 content 而非业务根；`root=null, attach=true` 不会 attach，`root!=null, attach=false` 仍会生成父类型参数。

### 练习 8：拆开 merge、include、结构标签、ViewStub 与测试旁路

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
V="$SRC/frameworks/base/core/java/android/view/ViewStub.java"
test -f "$L" && test -f "$V"
grep -nE 'TAG_MERGE|TAG_INCLUDE|TAG_REQUEST_FOCUS|TAG_TAG|parseInclude|hasThemeOverride|Include_id|Include_visibility|group.generateLayoutParams|consumeChildElements|new BlinkLayout' "$L"
grep -nE 'Precompiled layouts are not supported in this release|setPrecompiledLayoutsEnabledForTesting|tryInflatePrecompiled|mUseCompiledView|return view' "$L"
grep -nE 'inflateViewNoAdd|factory.inflate\(mLayoutResource, parent, false\)|replaceSelfWithView|removeViewInLayout|mInflatedViewRef|setLayoutInflater' "$V"
```

为五类标签写“是否创建当前标签 View、是否进 Factory、何时加到外部 parent”；再对 ViewStub 标三次完成点。

完成标准：merge 的失败可留下已加孩子，include 非文本粘贴，并区分结构名字位于文档根与子级时是否进入 Factory；Factory 返回的 ViewStub 也不能借用默认反射专属的 clone 保证，r48 常规预编译路径关闭。

### 练习 9：从异常停点追到 setContentView 与 ViewRoot 边界

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/view/LayoutInflater.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
V="$SRC/frameworks/base/core/java/android/view/View.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
test -f "$L" && test -f "$P" && test -f "$V" && test -f "$G"
grep -nE 'catch \(XmlPullParserException|catch \(Exception e\)|getParserStateDescription|mConstructorArgs\[0\] = lastContext|mConstructorArgs\[1\] = null|Trace.traceEnd' "$L"
grep -nE 'mLayoutInflater.inflate|requestApplyInsets|onContentChanged|mContentParentExplicitlySet = true' "$P"
grep -nE 'getViewRootImpl\(\)|isAttachedToWindow\(\)|mAttachInfo' "$V"
grep -nE 'new ViewRootImpl|mViews.add|mRoots.add|root.setView' "$G"
```

分别画普通根内部失败、merge 中途失败和 `L_return` 成功三条线，再接第 210 章的 Insets/callback/explicit 与后续 add。

完成标准：Parser 清理和 constructor args 恢复不是树回滚；`L_return` 有本地父子树，却仍没有 ViewRoot、WMS 窗口或首帧。
