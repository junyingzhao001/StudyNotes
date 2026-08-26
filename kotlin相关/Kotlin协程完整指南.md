# Kotlin 协程完整指南

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

## 一、协程基础

### 1.1 什么是协程？

协程（Coroutine）是一种轻量级的线程，可以在不阻塞线程的情况下挂起和恢复执行。协程不是线程，而是运行在线程上的任务。

核心特点：
- 挂起（Suspend）：可以在不阻塞线程的情况下暂停执行
- 恢复（Resume）：可以在需要时恢复执行
- 轻量级：可以创建数百万个协程，而不会导致性能问题
- 结构化并发：支持父子关系和取消传播

### 1.2 协程 vs 线程

| 特性 | 协程 | 线程 |
| --- | --- | --- |
| **数量限制** | 数百万 | 数千 |
| **调度** | 由协程库调度 | 由操作系统调度 |
| **创建成本** | 极低（几KB） | 较高（1MB+） |
| **切换成本** | 极低 | 较高（上下文切换） |
| **阻塞** | 挂起，不阻塞线程 | 阻塞线程 |

### 1.3 基本使用

添加依赖

```kotlin
// build.gradle.kts
dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.7.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}
```

启动协程

```kotlin
import kotlinx.coroutines.*

fun main() {
    // 方式1：使用 runBlocking（阻塞当前线程）
    runBlocking {
        println("协程开始")
        delay(1000)  // 挂起1秒
        println("协程结束")
    }
    
    // 方式2：使用 GlobalScope（不推荐，生命周期太长）
    GlobalScope.launch {
        println("GlobalScope 协程")
    }
    
    // 方式3：使用 CoroutineScope（推荐）
    val scope = CoroutineScope(Dispatchers.Default)
    scope.launch {
        println("Scope 协程")
    }
}
```

### 1.4 协程构建器

launch - 启动一个协程，不返回结果

```kotlin
fun main() = runBlocking {
    val job = launch {
        delay(1000)
        println("任务完成")
    }
    println("主线程继续执行")
    job.join()  // 等待协程完成
}
```

async - 启动一个协程，返回 Deferred 结果

```kotlin
fun main() = runBlocking {
    val deferred = async {
        delay(1000)
        "结果"
    }
    println("等待结果...")
    val result = deferred.await()  // 获取结果
    println("结果: $result")
}
```

withContext - 切换协程上下文

```kotlin
fun main() = runBlocking {
    val result = withContext(Dispatchers.IO) {
        // 在 IO 线程执行
        delay(1000)
        "IO 操作完成"
    }
    println(result)
}
```

### 1.5 协程上下文（CoroutineContext）

协程上下文包含：
- Job：协程的任务
- Dispatcher：调度器，决定协程在哪个线程执行
- CoroutineName：协程名称
- ExceptionHandler：异常处理器

```kotlin
fun main() = runBlocking {
    // 指定调度器
    launch(Dispatchers.Main) {
        // 主线程
    }
    
    launch(Dispatchers.IO) {
        // IO 线程池
    }
    
    launch(Dispatchers.Default) {
        // CPU 密集型任务线程池
    }
    
    launch(Dispatchers.Unconfined) {
        // 不指定线程，在调用线程执行
    }
    
    // 组合多个上下文元素
    launch(Dispatchers.IO + CoroutineName("MyCoroutine")) {
        println(coroutineContext[CoroutineName])  // CoroutineName(MyCoroutine)
    }
}
```

## 二、协程原理

### 2.1 挂起函数（Suspend Function）

挂起函数是协程的核心，使用 suspend 关键字标记：

```kotlin
suspend fun fetchData(): String {
    delay(1000)  // 挂起点
    return "数据"
}
```

挂起函数的特性：
- 只能在协程或其他挂起函数中调用
- 可以挂起当前协程，不阻塞线程
- 挂起时，线程可以执行其他任务

### 2.2 状态机实现

Kotlin 协程通过\*\*状态机（State Machine）\*\*实现挂起和恢复：

```kotlin
// 原始代码
suspend fun example() {
    println("1")
    delay(100)
    println("2")
    delay(100)
    println("3")
}

// 编译后的伪代码（简化版）
fun example(continuation: Continuation<Unit>): Any {
    when (continuation.label) {
        0 -> {
            println("1")
            continuation.label = 1
            return delay(100, continuation)  // 挂起
        }
        1 -> {
            println("2")
            continuation.label = 2
            return delay(100, continuation)  // 挂起
        }
        2 -> {
            println("3")
            return Unit  // 完成
        }
    }
}
```

### 2.3 Continuation Passing Style (CPS)

协程使用 CPS（Continuation Passing Style） 转换：

```kotlin
// 挂起函数
suspend fun getUser(id: Int): User {
    val profile = getProfile(id)  // 挂起点1
    val avatar = getAvatar(id)    // 挂起点2
    return User(profile, avatar)
}

// 转换为 CPS 风格（伪代码）
fun getUser(id: Int, continuation: Continuation<User>): Any {
    // 状态0：开始
    if (continuation.label == 0) {
        continuation.label = 1
        return getProfile(id, continuation)
    }
    // 状态1：profile 获取完成
    if (continuation.label == 1) {
        val profile = continuation.result as Profile
        continuation.label = 2
        return getAvatar(id, continuation)
    }
    // 状态2：avatar 获取完成
    if (continuation.label == 2) {
        val avatar = continuation.result as Avatar
        val profile = continuation.data as Profile
        return User(profile, avatar)
    }
}

```

### 2.4 协程调度器原理

```kotlin
// Dispatchers 的实现原理（简化版）
object Dispatchers {
    val Main: CoroutineDispatcher = MainCoroutineDispatcher()
    val IO: CoroutineDispatcher = DefaultScheduler.IO
    val Default: CoroutineDispatcher = DefaultScheduler.Default
    
    // IO 调度器使用线程池
    // Default 调度器使用工作窃取算法
}
```

调度流程：

1.协程挂起时，保存状态到 Continuation

2.调度器决定在哪个线程恢复

3.恢复时，从 Continuation 恢复状态

4.继续执行

### 2.5 协程的挂起和恢复

```kotlin
suspend fun suspendExample() {
    println("Before suspend")
    delay(1000)  // 挂起点
    println("After suspend")
}
```

执行流程：

1.挂起前：执行 "Before suspend"  
2.挂起：调用 delay()，协程状态保存到 Continuation

3.线程释放：当前线程可以执行其他任务

4.恢复：1秒后，调度器在合适的线程恢复协程

5.继续执行：执行 "After suspend"

## 三、编译后代码分析

### 3.1 挂起函数的编译

原始代码

```kotlin
suspend fun fetchUser(id: Int): User {
    val profile = getProfile(id)
    val avatar = getAvatar(id)
    return User(profile, avatar)
}

suspend fun getProfile(id: Int): Profile { ... }
suspend fun getAvatar(id: Int): Avatar { ... }
```

编译后的代码（反编译 Java）

```java
// 挂起函数被编译为普通函数，接受 Continuation 参数
public static final Object fetchUser(int id, Continuation<? super User> continuation) {
    // 创建状态机
    Object $result = continuation.getResult();
    Object var4 = IntrinsicsKt.getCOROUTINE_SUSPENDED();
    
    switch(continuation.label) {
        case 0:
            // 状态0：开始执行
            ResultKt.throwOnFailure($result);
            continuation.label = 1;
            // 调用第一个挂起点
            Object profile = getProfile(id, continuation);
            if (profile == var4) {
                return var4;  // 返回 SUSPENDED，表示挂起
            }
            // 继续执行（内联优化）
            break;
            
        case 1:
            // 状态1：从 getProfile 恢复
            ResultKt.throwOnFailure($result);
            Profile profile = (Profile)$result;
            continuation.label = 2;
            // 调用第二个挂起点
            Object avatar = getAvatar(id, continuation);
            if (avatar == var4) {
                return var4;  // 挂起
            }
            break;
                    
        case 2:
            // 状态2：从 getAvatar 恢复
            ResultKt.throwOnFailure($result);
            Avatar avatar = (Avatar)$result;
            Profile profile = (Profile)continuation.getContext().get(Key);
            // 创建结果并返回
            return new User(profile, avatar);
    }
    
    // 内联优化后的代码路径
    Profile profile = getProfile(id, continuation);
    if (profile == COROUTINE_SUSPENDED) return COROUTINE_SUSPENDED;
    
    Avatar avatar = getAvatar(id, continuation);
    if (avatar == COROUTINE_SUSPENDED) return COROUTINE_SUSPENDED;
    
    return new User(profile, avatar);
}
```

### 3.2 Continuation 接口

```kotlin
// Continuation 接口定义
interface Continuation<in T> {
    val context: CoroutineContext
    fun resumeWith(result: Result<T>)
}

// 编译器生成的 Continuation 实现
class FetchUserContinuation(
    completion: Continuation<Unit>
) : ContinuationImpl(completion) {
    var result: Any? = null
    var label: Int = 0
    
    override fun invokeSuspend(result: Any?): Any? {
        this.result = result
        label = label or Int.Companion.MIN_VALUE
        return fetchUser(id, this)
    }
}
```

### 3.3 协程构建器的编译

launch 编译后

```kotlin
// 原始代码
launch {
    println("Hello")
}

// 编译后的伪代码
launch(
    context = EmptyCoroutineContext,
    start = CoroutineStart.DEFAULT,
    block = { continuation ->
        println("Hello")
        continuation.resume(Unit)
    }
)
```

### 3.4 内联优化

编译器会进行内联优化，减少状态机的开销：

```kotlin
// 如果挂起函数可以内联，编译器会优化
suspend fun simple(): Int {
    return 42  // 没有挂起点，可以内联
}

// 编译后可能直接返回，不创建状态机
```

一个比较完善的案例kotlin 里面

```kotlin
launch(Dispatchers.Main) {
    // 1. 在主线程执行
    val user = fetchUser() // 这是一个 suspend 函数
    // 2. 此时协程挂起了！主线程被释放，可以去响应用户的点击、滑动
    
    // 3. 当网络数据回来，协程恢复
    showUser(user)
    // 4. 恢复到主线程继续显示 UI
}

suspend fun fetchUser(): User = withContext(Dispatchers.IO) {
    // 这里的代码会在 IO 线程池中执行
    val result = api.getUser()
    result // 返回结果
} 
```

反编译后代码：

```typescript
// 这是 launch 内部生成的“状态机”类
class MyCoroutine implements Continuation<Object> {
    int label = 0; // 状态码
    User user;     // 存储挂起点的中间结果

    @Override
    public void resumeWith(Object result) {
        // 核心逻辑：每次恢复都会调用此方法
        switch (this.label) {
            case 0:
                // 对应步骤 1
                this.label = 1;
                // 调用 fetchUser，并把“自己(this)”传进去，以便它回来找我
                Object maybeUser = fetchUser(this); 
                if (maybeUser == COROUTINE_SUSPENDED) return; // 挂起，直接跳出执行
                // 如果没挂起，直接进入 case 1
            case 1:
                // 对应步骤 3
                this.user = (User) result;
                showUser(this.user);
                return;
        }
    }
}
```

#### 3.4.1、执行顺序解析

##### 第一阶段：启动

调用 `launch`，初始化状态机 `label = 0`。

执行到 `fetchUser`。由于它内部有 `withContext(Dispatchers.IO)`，它会：

将任务丢给 IO 线程池。

返回一个特殊的标志位：`COROUTINE_SUSPENDED`。

**主线程看到标志位，立刻退出了 `resumeWith` 方法**。此时主线程是空闲的，可以继续刷新 UI 或处理点击。

##### 第二阶段：挂起中

主线程在做别的事（比如处理动画）。

IO 线程在执行 `api.getUser()`。

##### 第三阶段：恢复

IO 线程拿到了 `result`。

`withContext` 发现任务完成了，它会通过 `Dispatchers.Main` 向主线程发送一个消息：“帮我恢复这个状态机”。

主线程收到消息，再次调用状态机的 `resumeWith(result)`。

##### 第四阶段：继续执行

此时 `label` 是 `1`，代码直接跳到 `case 1`。

将传入的 `result` 赋值给 `user`。

执行 `showUser(user)`。

#### 3.4.2、核心机制总结

**为什么不会阻塞？** 因为 `fetchUser` 并没有让主线程等待，而是让主线程执行了一个 `return`。

**为什么能接着跑？** 因为协程把还没执行完的代码封装成了 `case 1, case 2...`，并把当前的变量（如 `user`）存成了状态机的成员变量。

**`Continuation` 是什么？** 它是“后续计算”的接口，保存协程上下文，并通过
`resumeWith` 接收成功或失败结果。编译器生成的挂起 lambda / 挂起函数状态机通常会实现或继承
Continuation 相关类型，但不能简单断言“任意 Continuation 就是状态机”；它也可能是调度器包装后的续体。

#### 3.4.3、 `withContext` 做了什么？

从概念上看，`withContext(Dispatchers.IO)` 做了两件事：

**切走**：它会把当前状态机封装成一个 Task，提交给 IO 线程。

**切回**：代码块完成后，恢复调用方原来的协程上下文。若调用方在
`Dispatchers.Main`，后续通常回到主线程。

这是便于理解的模型，不是每次都必然发生“两次线程切换”。当新旧上下文使用同一调度器，
或者调度器判断当前线程无需再次派发时，库可以走更快的执行路径。

#### 3.4.4、Continuation 的本质

在 Kotlin 中，`Continuation` 接口的定义非常简单，它本质上就是一个**带有上下文的回调（Callback）**：

```kotlin
interface Continuation<in T> {
    val context: CoroutineContext // 携带了 Dispatcher、Job 等信息
    fun resumeWith(result: Result<T>) // 相当于把数据传回状态机，并唤醒它
}
```

#### 3.4.5、Context 如何携带 Dispatcher 信息？

当你组合上下文（如 `Dispatchers.Main + Job()`）时，它们会被存储在一个类似 **类型安全 Map** 的结构中。

**Dispatcher 的身份**：`CoroutineDispatcher` 实际上实现了 `ContinuationInterceptor`（续体拦截器）接口。

**拦截机制**：当协程准备恢复（Resume）时，它会检查上下文。如果发现有拦截器（Dispatcher），它不会直接执行代码，而是说：“嘿，Dispatcher，我这里有个任务要执行，你来安排一下。”

#### 3.4.6、拦截器的“套娃”过程

这就是为什么 `withContext(Dispatchers.IO)` 通常能切走又切回来：

**挂起时**：`withContext` 获取当前的 `Continuation`（状态机），并把它交给 `Dispatchers.IO`。

**执行时**：`Dispatchers.IO` 把这个状态机封装成一个 `Runnable`，丢进自己的线程池队列。

**恢复时（关键点）**：

在 IO 任务结束时，结果会沿续体链恢复。

但是！状态机外层包裹着 **父协程的拦截器**（也就是 `Dispatchers.Main`）。

如果父协程上下文是 Android 主线程调度器，并且当前不在可直接执行的主线程位置，
拦截器会通过主线程消息机制安排恢复。若父上下文不是 Main，就会回到父上下文对应的执行器。

## 四、异常传递机制

### 4.1 异常传播规则

launch - 异常会向上传播

```kotlin
fun main() = runBlocking {
    val job = launch {
        throw Exception("launch 异常")
    }
    job.join()
    // 异常会传播，可能导致程序崩溃
}
```

async - 异常在 await 时抛出

```kotlin
fun main() = runBlocking {
    val deferred = async {
        throw Exception("async 异常")
    }
    try {
        deferred.await()  // 异常在这里抛出
    } catch (e: Exception) {
        println("捕获异常: ${e.message}")
    }
}
```

### 4.2 SupervisorJob - 隔离异常

```kotlin
fun main() = runBlocking {
    // 使用 SupervisorJob，子协程的异常不会影响其他子协程
    val supervisor = SupervisorJob()
    val scope = CoroutineScope(Dispatchers.Default + supervisor)
    
    scope.launch {
        throw Exception("异常1")
    }
    
    scope.launch {
        delay(100)
        println("这个协程仍然会执行")  // 不会因为上面的异常而取消
    }
    
    delay(200)
}
```

### 4.3 supervisorScope - 作用域隔离

```kotlin
fun main() = runBlocking {
    supervisorScope {
        launch {
            throw Exception("异常")
        }
        launch {
            delay(100)
            println("继续执行")  // 不受影响
        }
    }
}
```

### 4.4 CoroutineExceptionHandler - 全局异常处理

```kotlin
fun main() = runBlocking {
    val handler = CoroutineExceptionHandler { context, exception ->
        println("捕获异常: ${exception.message}")
    }
    
    val scope = CoroutineScope(Dispatchers.Default + handler)
    
    scope.launch {
        throw Exception("未捕获的异常")
    }
    
    delay(100)
}
```

### 4.5 异常传递示例

```kotlin
fun main() = runBlocking {
    try {
        coroutineScope {
            launch {
                throw Exception("子协程异常")
            }
            delay(1000)
        }
    } catch (e: Exception) {
        println("捕获到异常: ${e.message}")  // 会捕获
    }
    
    // 使用 SupervisorJob
    supervisorScope {
        launch {
            throw Exception("异常1")
        }
        launch {
            delay(100)
            println("继续执行")  // 不受影响
        }
    }
}
```

### 4.6 异常处理最佳实践

```java
// 1. 在 async 中使用 try-catch
val deferred = async {
    try {
        riskyOperation()
    } catch (e: Exception) {
        // 处理异常
        null
    }
}

// 2. 使用 CoroutineExceptionHandler
val handler = CoroutineExceptionHandler { _, exception ->
    // 记录异常
    Log.e("Coroutine", "异常", exception)
}

// 3. 使用 supervisorScope 隔离关键任务
supervisorScope {
    launch {
        // 关键任务1
    }
    launch {
        // 关键任务2，不受任务1异常影响
    }
}
```

## 五、并发处理

### 5.1 并发执行多个任务

使用 async 并发

```kotlin
suspend fun fetchData(): List<String> = coroutineScope {
    val data1 = async { fetchFromSource1() }
    val data2 = async { fetchFromSource2() }
    val data3 = async { fetchFromSource3() }
    
    listOf(
        data1.await(),
        data2.await(),
        data3.await()
    )
}
```

使用 launch 并发

```kotlin
suspend fun processData() = coroutineScope {
    val jobs = List(10) { index ->
        launch {
            processItem(index)
        }
    }
    jobs.joinAll()  // 等待所有任务完成
}
```

### 5.2 并发限制

使用 Semaphore 限制并发数

```kotlin
suspend fun limitedConcurrency() = coroutineScope {
    val semaphore = Semaphore(3)  // 最多3个并发
    
    repeat(10) { index ->
        launch {
            semaphore.withPermit {
                // 最多3个协程同时执行
                processItem(index)
            }
        }
    }
}
```

使用 Channel 限制并发

```kotlin
suspend fun channelConcurrency() = coroutineScope {
    val channel = Channel<Unit>(3)  // 容量为3
    
    repeat(10) { index ->
        launch {
            channel.send(Unit)  // 获取许可
            try {
                processItem(index)
            } finally {
                channel.receive()  // 释放许可
            }
        }
    }
}
```

### 5.3 共享状态处理

问题：并发修改共享变量

```kotlin
var counter = 0

fun main() = runBlocking {
    repeat(10000) {
        launch {
            counter++  // 并发修改，可能丢失更新
        }
    }
    delay(1000)
    println(counter)  // 可能小于 10000
}
```

解决方案1：使用 Mutex

```kotlin
var counter = 0
val mutex = Mutex()

fun main() = runBlocking {
    repeat(10000) {
        launch {
            mutex.withLock {
                counter++  // 线程安全
            }
        }
    }
    delay(1000)
    println(counter)  // 10000
}
```

解决方案2：使用 Atomic

```kotlin
val counter = AtomicInteger(0)

fun main() = runBlocking {
    repeat(10000) {
        launch {
            counter.incrementAndGet()  // 原子操作
        }
    }
    delay(1000)
    println(counter.get())  // 10000
}
```

解决方案3：使用 Actor

```kotlin
sealed class CounterMsg
object IncCounter : CounterMsg()
class GetCounter(val response: CompletableDeferred<Int>) : CounterMsg()

fun counterActor() = actor<CounterMsg> {
    var counter = 0
    for (msg in channel) {
        when (msg) {
            is IncCounter -> counter++
            is GetCounter -> msg.response.complete(counter)
        }
    }
}

fun main() = runBlocking {
    val counter = counterActor()
    repeat(10000) {
        counter.send(IncCounter)
    }
    val response = CompletableDeferred<Int>()
    counter.send(GetCounter(response))
    println(response.await())  // 10000
    counter.close()
}
```

### 5.4 并发集合

使用 ConcurrentHashMap

```kotlin
val map = ConcurrentHashMap<String, Int>()

fun main() = runBlocking {
    repeat(1000) {
        launch {
            map["key"] = map.getOrDefault("key", 0) + 1
        }
    }
    delay(1000)
    println(map["key"])
}
```

### 5.5 并发测试

```kotlin
suspend fun testConcurrency() = coroutineScope {
    val results = Collections.synchronizedList(mutableListOf<Int>())
    
    repeat(100) {
        launch {
            val result = compute(it)
            results.add(result)
        }
    }
    
    // 等待所有任务完成
    // results 包含100个结果
}
```

### 5.6 并发模式

生产者-消费者模式

```kotlin
fun main() = runBlocking {
    val channel = Channel<Int>()
    
    // 生产者
    launch {
        for (x in 1..5) {
            channel.send(x)
        }
        channel.close()
    }
    
    // 消费者
    launch {
        for (value in channel) {
            println(value)
        }
    }
}
```

扇出（Fan-out）

```kotlin
fun main() = runBlocking {
    val channel = produceNumbers()
    repeat(5) {
        launchProcessor(it, channel)  // 多个消费者
    }
    delay(1000)
    channel.cancel()
}
```

扇入（Fan-in）

```kotlin
fun main() = runBlocking {
    val channel = Channel<String>()
    launch { sendString(channel, "foo", 200L) }
    launch { sendString(channel, "BAR", 500L) }
    repeat(6) {
        println(channel.receive())
    }
    coroutineContext.cancelChildren()
}
```

## 六、最佳实践

### 6.1 协程作用域管理

```kotlin
class MyActivity : AppCompatActivity() {
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        scope.launch {
            // 协程代码
        }
    }
    
    override fun onDestroy() {
        super.onDestroy()
        scope.cancel()  // 取消所有协程
    }
}
```

### 6.2 避免 GlobalScope

```kotlin
// ❌ 不推荐
GlobalScope.launch {
    // 生命周期太长，难以管理
}

// ✅ 推荐
class MyViewModel : ViewModel() {
    private val viewModelScope = viewModelScope
    
    fun loadData() {
        viewModelScope.launch {
            // 自动管理生命周期
        }
    }
}
```

### 6.3 结构化并发

```kotlin
suspend fun fetchUserData(userId: Int): UserData = coroutineScope {
    val profile = async { getProfile(userId) }
    val avatar = async { getAvatar(userId) }
    
    UserData(
        profile = profile.await(),
        avatar = avatar.await()
    )
    // 如果任何一个失败，所有子协程都会被取消
}
```

### 6.4 取消和超时

```kotlin
// 取消协程
val job = launch {
    repeat(1000) { i ->
        if (isActive) {  // 检查是否已取消
            println("Job: $i")
            delay(100L)
        }
    }
}
delay(500L)
job.cancel()  // 取消
job.join()

// 超时
try {
    withTimeout(1000L) {
        // 如果超过1秒，会抛出 TimeoutCancellationException
        longRunningOperation()
    }
} catch (e: TimeoutCancellationException) {
    println("操作超时")
}

// 超时返回 null
val result = withTimeoutOrNull(1000L) {
    longRunningOperation()
}
```

### 6.5 性能优化

```kotlin
// 1. 使用适当的调度器
launch(Dispatchers.IO) {
    // IO 操作
}

launch(Dispatchers.Default) {
    // CPU 密集型操作
}

// 2. 避免不必要的挂起
suspend fun optimized() {
    val data = getData()  // 如果数据已缓存，不需要挂起
    if (data != null) {
        return data
    }
    return fetchData()  // 只有需要时才挂起
}

// 3. 使用 Flow 处理流式数据
fun numbers(): Flow<Int> = flow {
    for (i in 1..5) {
        delay(100)
        emit(i)
    }
}
```

### 6.6 调试技巧

```kotlin
// 1. 使用 CoroutineName
launch(CoroutineName("MyCoroutine")) {
    println(coroutineContext[CoroutineName])
}

// 2. 使用 -Dkotlinx.coroutines.debug
// JVM 参数：-Dkotlinx.coroutines.debug
// 输出会包含协程名称

// 3. 使用协程 ID
println("Coroutine: ${Thread.currentThread().name}")
```

## 七、常见问题

### 7.1 协程泄漏

```kotlin
// ❌ 协程泄漏
class MyActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        GlobalScope.launch {
            // 如果 Activity 被销毁，这个协程仍然在运行
            delay(10000)
            updateUI()  // 可能导致崩溃
        }
    }
}

// ✅ 正确做法
class MyActivity : AppCompatActivity() {
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        scope.launch {
            delay(10000)
            if (isActive) {
                updateUI()
            }
        }
    }
    
    override fun onDestroy() {
        super.onDestroy()
        scope.cancel()
    }
}
```

### 7.2 阻塞 vs 挂起

```kotlin
// ❌ 阻塞线程
Thread.sleep(1000)

// ✅ 挂起协程
delay(1000)

// ❌ 在协程中阻塞
launch {
    Thread.sleep(1000)  // 阻塞线程，浪费资源
}

// ✅ 使用挂起函数
launch {
    delay(1000)  // 挂起协程，释放线程
}
```

### 7.3 异常处理

```kotlin
// ❌ 异常丢失
launch {
    throw Exception("异常")
}
// 异常可能被忽略

// ✅ 正确处理异常
launch {
    try {
        riskyOperation()
    } catch (e: Exception) {
        // 处理异常
    }
}

// 或使用 CoroutineExceptionHandler
val handler = CoroutineExceptionHandler { _, exception ->
    // 处理异常
}
launch(handler) {
    throw Exception("异常")
}
```
