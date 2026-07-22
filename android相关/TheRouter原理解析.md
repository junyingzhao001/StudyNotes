# TheRouter 原理解析

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。图片已下载到 `image/` 目录并改为本地引用；已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

## 1、TheRouter到底解决什么问题

TheRouter 不是一个“只负责页面跳转”的路由库，它实际上把 Android 组件化里最常见的四类能力放进了一套统一机制里：

1. 页面导航：@Route + TheRouter.build().navigation()

2. 参数注入：@Autowired + TheRouter.inject(this)

3. 跨模块服务发现：@ServiceProvider + TheRouter.get(XX.class)

4. 初始化任务编排：@FlowTask + Digraph

它的核心设计思想可以概括成一句话：

编译期收集元数据，构建期聚合并织入入口，运行期只做查表、分发和少量拦截。

所以它的性能和扩展性，主要来自三层协作：

1. apt/ksp 负责扫描注解，生成索引类

2. plugin 负责在构建期收集所有索引类，并把调用入口织入 a.TheRouterServiceProvideInjecter

3. router 负责运行时调度、匹配、跳转、注入和任务执行

## 2、总体架构图

![图片展示了TheRouter总体架构图。业务代码中包含@Route、@Autowired等注解，经ASP / KSP生成RouterMap、Autowired / ServiceProvider索引类，再由Gradle Plugin / ASM扫描class.jar，获取元数据、枚举、接口等入口。a.TheRouterServiceProviderInjecter通过trojan / autowiredinject / addFlowTask / initDefaultRouteMap等方法处理。router运行时，可进行Navigator跳转、RouterInject服务注入、Digraph任务编排、ActionManager动作分发等操作。](image/therouter-architecture.svg)

## 3、初始化总流程

运行时真正的总入口是 TheRouter.init()。

源码里已经把流程写得很清楚：

```kotlin
fun init(context: Context?, asyncInitRouterInject: Boolean) {
    if (!inited) {
        digraph.beforeInit(context)
        if (asyncInitRouterInject) {
            routerInject.asyncInitRouterInject(context)
        } else {
            routerInject.syncInitRouterInject(context)
        }
        asyncInitRouteMap(context)
        execute {
            parserList.addFirst(DefaultObjectParser())
            parserList.addFirst(DefaultServiceParser())
            parserList.addFirst(DefaultUrlParser())
            parserList.addFirst(DefaultIdParser())
        }
        inited = true
    }
}
```

初始化可以拆成 4 件事：

1. 先执行 FlowTask 中“路由初始化前”的任务

2. 初始化 ServiceProvider 服务表

3. 异步初始化 RouteMap 路由表

4. 注册 @Autowired 的默认解析器

这里最值得注意的点是：
- FlowTask 比路由表初始化还早，这让业务可以在框架初始化前做路由修正、拦截器安装、预置配置
- 路由表和依赖注入可以异步初始化
- 未初始化完成时支持 PendingNavigator 挂起跳转

![图片展示了TheRouter初始化总流程的时序图。从Application开始，先执行TheRouter的init(context)；接着TheRouter调用Digraph的beforeInit(context)；然后RouterInject的addFlowTask(context, digraph)；之后RouterInject的asyncInitRouterInject和asyncInitRouteMap；最后RouteMap的initDefaultRouteMap，读取assets/therouter/routeMap.json或自定义任务。该图与文档中TheRouter初始化流程的源码解析上下文对应，直观呈现了各组件间调用关系及流程步骤。](image/therouter-init-flow.svg)

## 4、编译原理：注解不是直接生效，先生成索引类

### 4.1 @Route 会生成路由索引类

APT/KSP 会扫描所有 @Route / @Routes，把它们转成 RouteItem 列表，然后生成类似下面的类：
- 前缀：RouterMap\_\_TheRouter\_\_
- 包名：固定是 a

核心逻辑在：

```kotlin
TheRouterAnnotationProcessor.genRouterMapFile()
TheRouterSymbolProcessor.genRouterMapFile()
```

生成代码的思路是：

1. 把路由元数据序列化成 JSON 常量，挂到生成类字段上

2. 同时生成一个 addRoute() 方法，把每条路由调用 RouteMapKt.addRouteItem(item)

这意味着 TheRouter 同时保留了两种能力：

1. 构建期聚合 JSON，用于生成/校验 assets/therouter/routeMap.json

2. 运行期直接执行 addRoute()，无需再解析注解

典型业务代码：

```kotlin
@Route(path = HomePathIndex.KOTLIN)
@Route(path = HomePathIndex.KOTLIN2)
class KotlinTestActivity : AppCompatActivity()
```

### 4.2 @Autowired 会为每个类生成一个专属注入类

例如下面这段业务代码：

```kotlin
@JvmField
@Autowired
var test: String? = null
```

位置：Test2Activity.kt

APT/KSP 会为目标类生成：
- 当前类名 + \_\_TheRouter\_\_Autowired

生成类的职责很简单：

1. 判断 obj is 当前目标类
2. 轮询 TheRouter.parserList
3. 依次尝试解析字段值
4. 解析成功后直接给字段赋值

KSP 生成逻辑在：
- TheRouterSymbolProcessor.genAutowiredFile()

运行时调用入口在：
- TheRouter.inject()

也就是说，TheRouter.inject(this) 本质并不是反射扫字段，而是调用编译生成好的注入函数；反射只作为非常有限的辅助，不是主链路。

### 4.3 @ServiceProvider 会生成服务拦截器索引类

示例代码：

```typescript
@ServiceProvider
public static IUserService test() {
    return new IUserService() {
        @Override
        public String getUserInfo() {
            return "这是用户信息";
        }
    };
}
```

位置：business-b/src/main/java/com/therouter/demo/b/Test.java

APT 会生成前缀为 ServiceProvider\_\_TheRouter\_\_ 的类，这个类实现了 Interceptor。

生成类内部主要包含两部分能力：

1. interception(Class<T>, Object... params)：根据接口类型和参数类型，返回对应服务实例

2. initFlowTask(context, digraph)：把本模块声明的 FlowTask 注册到任务图

这一层非常关键，因为 TheRouter 把“跨模块服务发现”和“模块初始化任务收集”都塞进了同一批生成类里，减少了运行期入口数量。

### 4.4 @FlowTask 也是编译期收集

APT 对 @FlowTask 的要求很严格：

1. 必须标在 static 方法上
2. 返回值必须是 void
3. 参数必须只有一个 Context

解析逻辑在：
- TheRouterAnnotationProcessor.parseFlowTask()

这么设计的好处是：
- 任务注册完全静态化
- 不需要运行时再反射找初始化方法
- 编译就能做依赖检查、环检测辅助

## 5、构建期原理：Plugin 做“聚合、校验、织入”

如果只靠 APT/KSP，其实只能做到“每个模块各自生成一份索引”。

真正把整个 App 拼起来的是 plugin 模块。

它做了三件大事：

1. 扫描所有 class/jar，找出生成索引类
2. 聚合路由、服务、Autowired、FlowTask 元数据并做校验
3. 用 ASM 修改 a.TheRouterServiceProvideInjecter

### 5.1 Plugin 怎么识别哪些类是 TheRouter 生成物

它靠命名约定识别：
- RouterMap\_\_TheRouter\_\_\*
- ServiceProvider\_\_TheRouter\_\_\*
- \*\_\_TheRouter\_\_Autowired

见：
- TheRouterInjects.groovy
- TheRouterGetAllTask.java

这也是为什么 TheRouter 的编译体系比较“稳定”：它不依赖复杂协议，更多依赖“固定生成格式 + 统一聚合入口”。

### 5.2 Plugin 会把所有路由聚合成 assets/therouter/routeMap.json

AGP8 任务核心在：
- TheRouterGetAllTask.check()

它会：

1. 从所有 RouterMap\_\_TheRouter\_\_\* 类里读出 ROUTERMAPxx 常量
2. 反序列化成 RouteItem
3. 和 assets/therouter/routeMap.json 里的手工路由做合并
4. 检查“一条 URL 不能对应多个 Activity”
5. 检查 routeMap.json 中类名是否真的存在
6. 最后重新输出新的 routeMap.json

这一步非常适合分享时强调：
- 编译时就提前发现冲突
- 允许第三方页面通过 assets 手工接入
- 最终产物可视化，方便排查

仓库里就有一个真实产物：
- app/src/main/assets/therouter/routeMap.json

### 5.3 Plugin 会做 ASM 织入，把空壳入口变成真实入口

TheRouter 运行时有一个非常关键的“壳类”：

```kotlin
fun trojan() {}
fun autowiredInject(obj: Any?) {}
fun addFlowTask(context: Context?, digraph: Digraph) {}
fun initDefaultRouteMap() {}
```

位置：TheRouterServiceProvideInjecter.kt

这个文件源码里几乎是空的，但 plugin 会在构建时把它改写掉。

改写逻辑在：
- AddCodeVisitor.java

ASM 会往 4 个方法里塞代码：

1. trojan()：把所有 ServiceProvider\_\_TheRouter\_\_\* 实例注册进 RouterInject
2. addFlowTask()：把各模块 FlowTask 注册进 Digraph
3. autowiredInject()：调用所有 \_\_TheRouter\_\_Autowired.autowiredInject(obj)
4. initDefaultRouteMap()：调用所有 RouterMap\_\_TheRouter\_\_\*.addRoute()

个设计是 TheRouter 最核心的“绝活”之一：
- 业务模块完全不知道彼此存在
- Router 核心库也不知道有哪些业务模块
- 但构建产物里，入口函数已经被自动拼好

类关系可以画成这样：

![图片展示了TheRouter构建期的类关系图。上方是TheRouter，有四个方法。下方有Navigator、RouterInject、Digraph、RouteMap等类。中间是TheRouterServiceProviderInjecter，有trojan()、autowiredInject(obj)等方法。其下还有ServiceProvider__TheRouter__X、RouterMap__TheRouter__X、Target__TheRouter__Autowired等类。该图与文档中介绍的TheRouter构建期原理相关，直观呈现了各类之间的关系及关键方法，辅助理解其构建期的逻辑。](image/therouter-build-time-classes.svg)

## 6、运行时原理一：路由跳转链路

页面跳转主入口是：

```kotlin
TheRouter.build(url).navigation(context)
```

对应核心类：
- Navigator.kt

它不是简单包一层 Intent，而是一个带状态的跳转描述对象：
- url：原始路由
- extras：运行时参数
- kvPair：从 URL 查询参数解析出来的参数
- pending：是否挂起
- intentData / clipData / identifier：原生 Intent 能力补充

### 6.1 、build(url) 发生了什么

Navigator 构造函数里会先做两件事：

1. 执行 NavigatorPathFixHandle
2. 把 query/hash 参数解析进 kvPair

也就是说，最早介入的不是路由匹配，而是 path 修正。

源码位置：
- Navigator.init { ... }

6. 2 navigation() 的执行链路

主流程在：
- Navigator.navigation()

执行步骤如下：

![这张图展示了TheRouter运行时路由跳转的链路流程，对应文档中提到的路由跳转链路相关内容。流程起始于TheRouter构建URL和导航参数，先判断路由表是否完成初始化，若未完成则加入PendingNavigator队列，若已完成则执行PathReplaceInterceptor，接着匹配simpleURI。之后判断是否为Activity且未匹配页面，若为前者则通过ActionManager处理动作，若为后者则合并extra、kvPairs、RouteItem的参数。后续依次执行RouterReplaceInterceptor、全局RouterInterceptor AOP，再构建Intent或Fragment，启动Activity或创建Fragment，最后触发回调、历史记录或动作，该流程与文档中提及的路由跳转链路的4个重要扩展点的执行逻辑相呼应。](image/therouter-navigation-flow.svg)

这条链里有 4 个重要扩展点

1. NavigatorPathFixHandle：修 URL
2. PathReplaceInterceptor：按 path 替换
3. RouterReplaceInterceptor：按 RouteItem 替换
4. RouterInterceptor：整个跳转过程 AOP

### 6.3、 路由表匹配其实就是查 RegexpKeyedMap

TheRouter 的路由表容器是：
- ROUTER_MAP = `RegexpKeyedMap<RouteItem>()`

匹配逻辑在：
- matchRouteMap()

关键点：

1. 先通过 TheRouter.build(url).simpleUrl 去掉 query
2. 去掉末尾 
3. 从 ROUTER_MAP 中取值
4. 取到后做一次 copy()，避免外部修改污染全局路由表

这说明 TheRouter 运行时查路由的主成本非常低，本质就是一次 map 查找加少量参数合并。

### 6.4、 参数优先级怎么合并

参数来源有三份：

1. 路由表静态参数：@Route(params = ...)
2. URL 参数：build("xxx?a=1")
3. 运行时参数：withString("a", "2")

合并逻辑主要分布在：
- RouteItem.getExtras()
- Navigator.navigation()

优先级结论：

1. 运行时 extras 最高
2. URL kvPair 次之
3. @Route(params) 最低

这套规则非常符合直觉：越“晚”提供的数据，优先级越高。

### 6.5、 PendingNavigator：为什么初始化没结束也敢跳

如果路由表没初始化完，Navigator.navigation() 不会直接失败，而是：

1. 标记 pending = true
2. 放进 disposableQueue
3. 等路由表初始化完成后调用 sendPendingNavigator()

代码位置：
- Pending 逻辑
- sendPendingNavigator()

示例里甚至在 attachBaseContext() 就提前发起了跳转：

```kotlin
TheRouter.build(HomePathIndex.PENDING).navigation();
```

位置：App.java

这是 TheRouter 相比很多路由框架很实用的一个点：它把“初始化时机不稳定”问题，转成了“挂起后自动恢复”问题。

## 7、运行时原理二：@Autowired 参数注入链路

调用方式很简单：

```kotlin
TheRouter.inject(this)
```

但底层实际上是一个“多解析器责任链”：

1. 默认对象解析器 DefaultObjectParser
2. 默认服务解析器 DefaultServiceParser
3. 默认 URL 参数解析器 DefaultUrlParser
4. 默认 ViewId 解析器 DefaultIdParser

这些解析器在初始化时注册：
- TheRouter.init()

注入流程如下：

![图片展示了TheRouter运行时参数注入链路。从TheRouter.inject(target)开始，经autowiredInject(target)、ASM织入后的autowiredInject()，再遍历所有Target__TheRouter__Autowired，判断obj是否当前目标类，若否则继续遍历TheRouter.parserList，调用parser.parse(type, target, AutowiredItem)，若返回值非空则给字段赋值。该图与文档中对@Autowired参数注入链路的解析内容相呼应，直观呈现了注入过程。](image/therouter-autowired-flow.svg)

这种设计的优点：

1. 字段枚举发生在编译期，不走运行时反射扫描
2. 数据来源可以继续扩展
3. 每个字段是“按解析器顺序尝试解析”，扩展很自然

Kotlin 的一个注意点，示例里也体现了：

```kotlin
@JvmField
@Autowired
var test: String? = null
```

因为生成代码通常是直接给字段赋值，所以 Kotlin 属性需要 @JvmField 或 lateinit var 等方式配合。

## 8、运行时原理三：跨模块依赖注入

业务调用：

```kotlin
TheRouter.get(IUserService.class)
```

示例落地页：

```kotlin
textview1.setText("测试获取用户信息服务：" + TheRouter.get(IUserService.class).getUserInfo());
```

位置：TestInjectActivity.java

对应运行时入口：
- TheRouter.get()
- RouterInject.get()

核心流程：

1. 先查 RecyclerBin 缓存
2. 查不到再遍历自定义拦截器 mCustomInterceptors
3. 再遍历编译生成的服务拦截器 mInterceptors
4. 找到实例后再放回缓存

源码很直白：

```kotlin
operator fun <T> get(clazz: Class<T>, vararg params: Any?): T? {
    var temp = mRecyclerBin.get(clazz, *params)
    if (temp == null) {
        temp = createDI(clazz, *params)
        if (temp != null) {
            mRecyclerBin.put(clazz, temp, *params)
        }
    }
    return temp
}
```

位置：RouterInject.kt

值得强调的点：

1. 它不是传统 IOC 容器的“全量对象图装配”
2. 更像“基于接口和参数签名的服务定位器”
3. 编译已经把服务创建逻辑静态化，所以运行时只是遍历少量工厂

## 9、运行时原理四：FlowTask 任务编排

TheRouter 不是简单地“按顺序调 init”，而是把初始化任务建成一张有向图。

核心类：
- Digraph.kt

它维护了三类数据：

1. tasks：真实任务
2. virtualTasks：业务节点/内置节点
3. todoList：拓扑展开后的待执行列表

内置虚拟节点尤其关键：

1. BEFORE_THEROUTER_INITIALIZATION
2. THEROUTER_INITIALIZATION
3. APP_ONSPLASH

构造规则见：
- makeVirtualFlowTask()

执行流程可以画成：

![这张图是TheRouter运行时原理中FlowTask任务编排的流程示意图，清晰展示了任务执行的完整链路。流程从TheRouter.init()开始，依次执行Digraph.beforeInit()、通过ASM addFlowTask()注入各模块任务、beforeSchedule()步骤；接着先执行BEFORE_THETHER_INITIALIZATION阶段，同步完成仅依赖该阶段的同步任务，再通过异步initSchedule()构建todoList，最后由schedule()按依赖状态调度任务，任务完成后触发后续任务。该图对应文档中运行时原理里FlowTask任务编排的内容，直观呈现了任务编排的核心流程。](image/therouter-flowtask-flow.svg)

这里设计得很巧妙的地方有两个：

1. 先同步跑“路由初始化前任务”，保证这类任务能真正影响路由初始化
2. 不存在的依赖会先被建成 VirtualFlowTask，这样业务还能手动 TheRouter.runTask(name) 触发

runTask() 入口在：
- TheRouter.runTask()

## 10、Action 机制：TheRouter 不只会跳页面

如果一个 URL 在路由表中匹配不到页面，但它是一个已注册 Action，那么就不会走 Activity 跳转，而会进入：
- ActionManager.handleAction()

关键判断在：

```kotlin
if (ActionManager.isAction(this) && match == null) {
    ActionManager.handleAction(this, context)
    return
}
```

位置：Navigator.kt

示例 ActionInterceptor：

```java
@com.therouter.router.action.ActionInterceptor(actionName = HomePathIndex.ACTION2)
public class TestActionInterceptor extends ActionInterceptor {
    @Override
    public boolean handle(@NonNull Context context, @NonNull Navigator navigator) {
        Toast.makeText(context, HomePathIndex.ACTION2, Toast.LENGTH_SHORT).show();
        return super.handle(context, navigator);
    }
}
```

位置：TestActionInterceptor.java

可以把它理解成：
- Path 更像“页面地址”
- Action 更像“应用内消息/命令分发”

## 11、拦截器体系：TheRouter 为什么扩展性很强

TheRouter 的拦截器并不是一个点，而是一整条链路上的多个点位：

### 11.1、NavigatorPathFixHandle

最早执行，用于修正输入 path。

示例：

```typescript

NavigatorKt.addNavigatorPathFixHandle(new NavigatorPathFixHandle() {
    @Nullable
    @Override
    public String fix(@Nullable String path) {
        if (path != null) {
            path = path.replace("http://", "https://");
        }
        return path;
    }
});
```

位置：InterceptorActivity.java

### 11.2 、PathReplaceInterceptor

按 path 做替换，适合占位路由、壳工程跳首页。

### 11.3、 RouterReplaceInterceptor

路由已经匹配到 RouteItem 后再替换，适合登录态拦截、权限兜底。

### 11.4 、RouterInterceptor

全局 AOP，只能设置一个，控制粒度最大。

所以 TheRouter 的扩展能力不是来自“单一大拦截器”，而是来自：

输入修正 -> Path 替换 -> Route 替换 -> 全局 AOP

这比很多只提供单一 navigation interceptor 的设计更细。

## 12、routeMap.json 和运行时 RouteMap 的关系

很多人第一次看 TheRouter 会误以为：

路由匹配完全依赖 assets/therouter/routeMap.json

其实不是。

真实情况是：

1. 编译生成 RouterMap\_\_TheRouter\_\_\*
2. 运行时优先执行 initDefaultRouteMap()，把索引类里的路由直接塞进内存路由表
3. 然后再看有没有 assets/therouter/routeMap.json 或自定义 RouterMapInitTask

对应代码：

```kotlin
initDefaultRouteMap()
if (!asm) {
    getAllDI(context)
    getRouterMapIndex().forEach { it.init() }
}
initedRouteMap = true
if (initTask == null) {
    initRouteMap()
} else {
    initTask?.asyncInitRouteMap()
}
```

位置：RouteMap.asyncInitRouteMap()

所以可以这么理解：
- RouterMap\_\_TheRouter\_\_\* 是主数据源
- routeMap.json 更像补充输入 / 调试产物 / 第三方接入桥

## 13、一个完整案例：从注解到跳转成功，中间发生了什么

以这段代码为例：

```kotlin
@Route(path = HomePathIndex.TEST_AUTOWIRED)
class Test2Activity : AppCompatActivity() {

    @JvmField
    @Autowired
    var test: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        TheRouter.inject(this)
    }
}
```

它背后的完整链路是：

1. KSP 扫描到 @Route，生成 RouterMap\_\_TheRouter\_\_xxx
2. KSP 扫描到 @Autowired，生成 Test2Activity\_\_TheRouter\_\_Autowired
3. Plugin 扫描到这两个生成类
4. Plugin 把路由聚合到 routeMap.json
5. Plugin 用 ASM 改写 TheRouterServiceProvideInjecter
6. App 启动时 TheRouter.init() 调 initDefaultRouteMap()，把路由加入内存表
7. 代码执行 TheRouter.build(path).navigation()
8. Navigator 匹配到 Test2Activity
9. Test2Activity 启动后调用 TheRouter.inject(this)
10. 注入入口转发到 Test2Activity\_\_TheRouter\_\_Autowired.autowiredInject(this)
11. 解析器链从 Intent/Bundle/对象池等来源拿到参数并赋值给 test

这就是 TheRouter 的核心风格：
