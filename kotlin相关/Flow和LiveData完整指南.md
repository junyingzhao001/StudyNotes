# Flow 和 LiveData 完整指南

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

## 一、Flow 基础

### 1.1 什么是 Flow？

Flow 是 Kotlin 协程中的异步数据流，用于处理一系列异步产生的值。类似于 RxJava 的 Observable。

核心特性：
- 冷流（Cold Flow）：只有被收集时才执行
- 挂起函数：基于协程的挂起机制
- 背压处理：支持背压（Backpressure）
- 类型安全：编译时类型检查

### 1.2 Flow 的基本使用

创建 Flow

```kotlin
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.*

// 方式1：使用 flow 构建器
fun simpleFlow(): Flow<Int> = flow {
    for (i in 1..3) {
        delay(100)
        emit(i)  // 发送值
    }
}

// 方式2：使用 flowOf
fun flowOfExample(): Flow<Int> = flowOf(1, 2, 3)

// 方式3：使用 asFlow
fun asFlowExample(): Flow<Int> = listOf(1, 2, 3).asFlow()

// 方式4：使用 channelFlow
fun channelFlowExample(): Flow<Int> = channelFlow {
    send(1)
    send(2)
    send(3)
    close()
}

// 方式5：使用 callbackFlow
fun callbackFlowExample(): Flow<String> = callbackFlow {
    val callback = object : SomeCallback {
        override fun onData(data: String) {
            trySend(data)
        }
        override fun onComplete() {
            close()
        }
    }
    registerCallback(callback)
    awaitClose { unregisterCallback(callback) }
}
```

收集 Flow

```kotlin
fun main() = runBlocking {
    // 基本收集
    simpleFlow().collect { value ->
        println(value)
    }
    
    // 只收集第一个值
    val first = simpleFlow().first()
    
    // 收集到列表
    val list = simpleFlow().toList()
    
    // 收集到 Set
    val set = simpleFlow().toSet()
}
```

### 1.3 Flow 的类型

Cold Flow（冷流）

```kotlin
fun coldFlow(): Flow<Int> = flow {
    println("Flow 开始执行")
    for (i in 1..3) {
        emit(i)
    }
}

fun main() = runBlocking {
    coldFlow().collect { println("收集1: $it") }
    coldFlow().collect { println("收集2: $it") }
    // 输出两次 "Flow 开始执行"
}
```

Hot Flow（热流）

```kotlin
fun main() = runBlocking {
    val hotFlow = flow {
        println("上游开始执行")
        for (i in 1..3) {
            delay(100)
            emit(i)
        }
    }.shareIn(
        scope = this,
        started = SharingStarted.Eagerly,
        replay = 1
    )

    // collect 是挂起且通常不会主动结束的操作，多个收集者要并发启动。
    val collector1 = launch {
        hotFlow.take(3).collect { println("收集1: $it") }
    }
    val collector2 = launch {
        hotFlow.take(3).collect { println("收集2: $it") }
    }
    joinAll(collector1, collector2)
}
```

这里的“热”表示上游由共享协程驱动，而不是每个收集者各执行一遍。实际能收到几个值还受
`started` 策略、订阅时机和 `replay` 影响，不能简单理解成“热流永远只执行一次”。

### 1.4 Flow 操作符

转换操作符

```kotlin
fun transformOperators() = runBlocking {
    // map - 转换每个值
    flowOf(1, 2, 3)
        .map { it * 2 }
        .collect { println(it) }  // 2, 4, 6
    
    // transform - 通用转换
    flowOf(1, 2, 3)
        .transform { value ->
            emit(value * 2)
            emit(value * 3)
        }
        .collect { println(it) }  // 2, 3, 4, 6, 6, 9
    
    // flatMapConcat - 顺序展开
    flowOf(1, 2, 3)
        .flatMapConcat { value ->
            flowOf(value * 2, value * 3)
        }
        .collect { println(it) }  // 2, 3, 4, 6, 6, 9
    
    // flatMapMerge - 并发展开
    flowOf(1, 2, 3)
        .flatMapMerge { value ->
            flowOf(value * 2, value * 3)
        }
        .collect { println(it) }  // 顺序可能不同
        
   // flatMapLatest - 只处理最新的
    flowOf(1, 2, 3)
        .flatMapLatest { value ->
            flow {
                delay(100)
                emit(value * 2)
            }
        }
        .collect { println(it) }  // 可能只输出最后一个
}      
        
```

过滤操作符

```kotlin
fun filterOperators() = runBlocking {
    // filter - 过滤
    flowOf(1, 2, 3, 4, 5)
        .filter { it % 2 == 0 }
        .collect { println(it) }  // 2, 4
    
    // take - 取前 n 个
    flowOf(1, 2, 3, 4, 5)
        .take(3)
        .collect { println(it) }  // 1, 2, 3
    
    // takeWhile - 条件取
    flowOf(1, 2, 3, 4, 5)
        .takeWhile { it < 4 }
        .collect { println(it) }  // 1, 2, 3
    
    // drop - 跳过前 n 个
    flowOf(1, 2, 3, 4, 5)
        .drop(2)
        .collect { println(it) }  // 3, 4, 5
    
    // distinctUntilChanged - 去重连续相同
    flowOf(1, 1, 2, 2, 3, 3)
        .distinctUntilChanged()
        .collect { println(it) }  // 1, 2, 3
}
```

组合操作符

```kotlin
fun combineOperators() = runBlocking {
    // zip - 组合两个 Flow
    val flow1 = flowOf(1, 2, 3)
    val flow2 = flowOf("a", "b", "c")
    flow1.zip(flow2) { a, b -> "$a$b" }
        .collect { println(it) }  // 1a, 2b, 3c
    
    // combine - 组合多个 Flow（每次任一 Flow 发射时组合）
    val flowA = flowOf(1, 2, 3).onEach { delay(100) }
    val flowB = flowOf("a", "b", "c").onEach { delay(150) }
    flowA.combine(flowB) { a, b -> "$a$b" }
        .collect { println(it) }  // 组合所有可能的值
    
    // merge - 合并多个 Flow
    merge(
        flowOf(1, 2, 3),
        flowOf(4, 5, 6)
    ).collect { println(it) }  // 1, 2, 3, 4, 5, 6 或混合顺序
}
```

终端操作符

```kotlin
fun terminalOperators() = runBlocking {
    val numbers = flowOf(1, 2, 3, 4, 5)
    
    // collect - 收集所有值
    numbers.collect { println(it) }
    
    // first - 第一个值
    val first = numbers.first()
    
    // last - 最后一个值
    val last = numbers.last()
    
    // single - 唯一值（如果有多个或为空会抛出异常）
    val single = flowOf(42).single()
    
    // count - 计数
    val count = numbers.count()
    
    // fold - 累积（从初始值开始）
    val sum = numbers.fold(0) { acc, value -> acc + value }
    
    // reduce - 累积（使用第一个值作为初始值）
    val product = numbers.reduce { acc, value -> acc * value }
    
    // toList - 转换为列表
    val list = numbers.toList()
    
    // toSet - 转换为 Set
    val set = numbers.toSet()
}
```

异常处理操作符

```kotlin
fun exceptionOperators() = runBlocking {
    val flow = flow {
        emit(1)
        throw Exception("错误")
        emit(2)
    }
    
    // catch - 捕获异常
    flow.catch { e ->
        println("捕获异常: ${e.message}")
    }.collect { println(it) }
    
    // retry - 重试
    flow.retry(3) { e ->
        e is Exception && attempt < 2
    }.collect { println(it) }
    
    // onCompletion - 完成回调
    flow.onCompletion { cause ->
        if (cause != null) {
            println("异常完成: ${cause.message}")
        } else {
            println("正常完成")
        }
    }.catch { e ->
        println("捕获: ${e.message}")
    }.collect { println(it) }
}
```

背压处理操作符

```kotlin
fun backpressureOperators() = runBlocking {
    val fastFlow = flow {
        for (i in 1..1000) {
            emit(i)
        }
    }
    
    // buffer - 缓冲
    fastFlow.buffer(10)
        .collect { 
            delay(100)
            println(it)
        }
    
    // conflate - 只保留最新值
    fastFlow.conflate()
        .collect {
            delay(100)
            println(it)  // 可能跳过中间值
        }
    
    // collectLatest - 只处理最新值
    fastFlow.collectLatest {
        delay(100)
        println(it)  // 只处理最后的值
    }
}
```

共享操作符

```kotlin
fun sharingOperators() {
    val sourceFlow = flow {
        for (i in 1..5) {
            delay(100)
            emit(i)
        }
    }
    
    // shareIn - 转换为热流
    val sharedFlow = sourceFlow.shareIn(
        scope = CoroutineScope(Dispatchers.Default),
        started = SharingStarted.WhileSubscribed(),
        replay = 1  // 新订阅者收到的最近 n 个值
    )
    
    // stateIn - 转换为 StateFlow
    val stateFlow = sourceFlow.stateIn(
        scope = CoroutineScope(Dispatchers.Default),
        started = SharingStarted.WhileSubscribed(),
        initialValue = 0
    )
}
```

### 1.5 StateFlow 和 SharedFlow

StateFlow

```kotlin
// StateFlow 特点：
// 1. 必须有初始值
// 2. 只保存最新值
// 3. 新的订阅者立即收到当前值
// 4. 值必须不同才发射（使用 == 比较）

fun stateFlowExample() = runBlocking {
    val stateFlow = MutableStateFlow(0)

    val collector = launch {
        stateFlow.take(3).collect { value ->
            println("StateFlow 值: $value")
        }
    }

    yield() // 让收集者先订阅并收到初始值 0
    stateFlow.value = 1
    yield() // StateFlow 会合并过快的更新，让收集者有机会处理 1
    stateFlow.value = 1 // 与当前值相等，不会再次发射
    stateFlow.value = 2

    collector.join()
}
```

SharedFlow

```kotlin
// SharedFlow 特点：
// 1. 可以有或没有初始值
// 2. 可以保存多个值（通过 replay）
// 3. 新订阅者可以收到历史值（replay > 0）
// 4. 默认不会像 StateFlow 那样按 equals 自动去重

fun sharedFlowExample() = runBlocking {
    val sharedFlow = MutableSharedFlow<Int>(
        replay = 2,  // 保存最近2个值
        extraBufferCapacity = 1,  // 额外缓冲区
        onBufferOverflow = BufferOverflow.DROP_OLDEST  // 缓冲区溢出策略
    )
    
    // 发射值
    sharedFlow.emit(1)
    sharedFlow.emit(2)
    sharedFlow.emit(3)
    
    // 收集（会收到最后2个值：2, 3）
    sharedFlow.collect { value ->
        println("SharedFlow 值: $value")
    }
}
```

### 1.6 Flow 在 Android 中的使用

```kotlin
class UserViewModel : ViewModel() {
    private val _users = MutableStateFlow<List<User>>(emptyList())
    val users: StateFlow<List<User>> = _users.asStateFlow()
    
    private val _events = MutableSharedFlow<Event>()
    val events: SharedFlow<Event> = _events.asSharedFlow()
    
    fun loadUsers() {
        viewModelScope.launch {
            userRepository.getUsers()
                .catch { e ->
                    _events.emit(Event.Error(e.message))
                }
                .collect { users ->
                    _users.value = users
                }
        }
    }
}

// Activity/Fragment 中
class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        lifecycleScope.launch {
            // STARTED 以下会取消内部收集，重新 STARTED 时再次启动。
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.users.collect(::updateUI)
                }
                launch {
                    viewModel.events.collect { event ->
                        when (event) {
                            is Event.Error -> showError(event.message)
                        }
                    }
                }
            }
        }
    }
}
```

## 二、LiveData 基础

### 2.1 什么是 LiveData？

LiveData 是 Android Architecture Components 提供的生命周期感知的观察者模式实现。

核心特性：
- 生命周期感知：自动管理观察者的生命周期
- 数据持有者：可以持有数据
- 主线程更新：自动切换到主线程
- 内存安全：避免内存泄漏

### 2.2 LiveData 的基本使用

```kotlin
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel

class UserViewModel : ViewModel() {
    // 私有可变 LiveData
    private val _users = MutableLiveData<List<User>>()
    
    // 公开不可变 LiveData
    val users: LiveData<List<User>> = _users
    
    fun loadUsers() {
        _users.value = listOf(User("Alice"), User("Bob"))
    }
}

// Activity/Fragment 中观察
class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        viewModel.users.observe(this) { users ->
            updateUI(users)
        }
    }
}
```

### 2.3 LiveData 的类型

MutableLiveData

```kotlin
val liveData = MutableLiveData<String>()
liveData.value = "Hello"  // 主线程设置
liveData.postValue("World")  // 任意线程设置
```

Transformations

```kotlin
// map - 转换
val sourceLiveData = MutableLiveData<Int>()
val mappedLiveData = Transformations.map(sourceLiveData) { it * 2 }

// switchMap - 切换 LiveData
val userIdLiveData = MutableLiveData<Int>()
val userLiveData = Transformations.switchMap(userIdLiveData) { userId ->
    userRepository.getUser(userId)
}

// distinctUntilChanged - 去重连续相同值
val distinctLiveData = Transformations.distinctUntilChanged(sourceLiveData)
```

MediatorLiveData

```kotlin
val mediatorLiveData = MediatorLiveData<String>()

val source1 = MutableLiveData<String>()
val source2 = MutableLiveData<String>()

mediatorLiveData.addSource(source1) { value ->
    mediatorLiveData.value = "Source1: $value"
}

mediatorLiveData.addSource(source2) { value ->
    mediatorLiveData.value = "Source2: $value"
}

// 移除源
mediatorLiveData.removeSource(source1)
```

### 2.4 LiveData 操作符

基本操作

```java
// 设置值
liveData.value = "value"  // 主线程
liveData.postValue("value")  // 任意线程

// 获取值
val value = liveData.value
val lastValue = liveData.value  // 可能为 null

// 观察
liveData.observe(owner) { value ->
    // 生命周期感知的观察
}

// 永久观察（不推荐，需要手动移除）
liveData.observeForever { value ->
    // 需要调用 removeObserver
}
```

扩展操作

```kotlin
// 只在至少有一次变化时观察
liveData.observeOnce(owner) { value ->
    // 只接收一次更新
}

// 过滤空值
liveData.observeNonNull(owner) { value ->
    // value 不会是 null
}

// 防抖
liveData.debounce(300L)
    .observe(owner) { value ->
        // 300ms 内只接收最后一次更新
    }
```

### 2.5 LiveData 在 Android 中的使用

```kotlin
class UserViewModel : ViewModel() {
    private val repository = UserRepository()
    
    private val _users = MutableLiveData<List<User>>()
    val users: LiveData<List<User>> = _users
    
    private val _loading = MutableLiveData<Boolean>()
    val loading: LiveData<Boolean> = _loading
    
    private val _error = MutableLiveData<String?>()
    val error: LiveData<String?> = _error
    
    fun loadUsers() {
        _loading.value = true
        viewModelScope.launch {
            try {
                val result = repository.getUsers()
                _users.value = result
                _error.value = null
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }
}
class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // 观察多个 LiveData
        viewModel.users.observe(this) { users ->
            updateUsers(users)
        }
        
        viewModel.loading.observe(this) { isLoading ->
            showLoading(isLoading)
        }
        
        viewModel.error.observe(this) { error ->
            error?.let { showError(it) }
        }
    }
}
```

## 三、API 完整对比

### **3.1 创建数据流**

| **功能** | **Flow** | **LiveData** |
| --- | --- | --- |
| **可变数据源** | MutableStateFlow<**T**>  <br/>MutableSharedFlow<**T**> | MutableLiveData<**T**> |
| **不可变数据源** | StateFlow<**T**>  <br/>SharedFlow<**T**> | LiveData<**T**> |
| **初始值** | StateFlow 必须有  <br/>SharedFlow 可选 | 可选 |
| **空安全** | 支持可空类型 | 支持可空类型 |

### **3.2 设置/发射值**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **设置值（主线程）** | stateFlow.**value** = **value** | liveData.**value** = **value** |
| **设置值（任意线程）** | stateFlow.tryEmit(**value**) | liveData.postValue(**value**) |
| **发射值** | sharedFlow.emit(**value**)  <br/>flow { emit(**value**) } | liveData.**value** = **value** |
| **批量更新** | stateFlow.update { **it** + 1 } | 不支持 |

### **3.3 观察/收集数据**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **观察（生命周期感知）** | **flow**.collectAsState()  <br/>**flow**.collect { } + Lifecycle | **liveData**.observe(**owner**) { } |
| **永久观察** | **flow**.collect { } | **liveData**.observeForever { } |
| **单次观察** | flow.**first**() | 需要扩展函数 |
| **带过滤观察** | **flow**.filter { }.collect { } | Transformations.map() |

### **3.4 转换操作**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **映射** | .map { } | Transformations.map() |
| **切换数据源** | .flatMapLatest { } | **Transformations**.switchMap() |
| **去重** | .distinctUntilChanged() | **Transformations**.distinctUntilChanged() |
| **合并** | .combine()  <br/>.zip()  <br/>merge() | MediatorLiveData |
| **过滤** | .filter { } | 需要扩展函数 |

### **3.5 线程切换**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **切换调度器** | .flowOn(**Dispatchers**.IO) | 在 Repository 层切换 |
| **切换到主线程** | .flowOn(**Dispatchers**.Main) | 自动在主线程更新 |
| **收集时切换** | collect { } 在指定调度器 | **observe**() 自动主线程 |

### **3.6 异常处理**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **捕获异常** | .catch { } | **try**-**catch** |
| **重试** | .retry(**n**) | 手动实现 |
| **完成回调** | .onCompletion { } | 不支持 |

### **3.7 生命周期管理**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **生命周期感知** | 需要配合 Lifecycle | 内置 |
| **自动取消** | 使用 lifecycleScope | 内置 |
| **配置变化保持** | 需要 ViewModel | 需要 ViewModel |

### **3.8 操作符对比表**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **创建** | flow { }  <br/>**flowOf**()  <br/>**asFlow**() | MutableLiveData() |
| **转换** | Map  <br/>**Transform**  <br/>flatMapConcat/Merge/Latest | Transformations.map  <br/>**Transformations**.switchMap |
| **过滤** | Filter  <br/>Take  <br/>drop | 需要扩展函数 |
| **组合** | Zip  <br/>Combine  <br/>**merge** | MediatorLiveData |
| **异常** | **Catch**  <br/>**Retry**  <br/>onCompletion | **try**-**catch** |
| **终端** | **Collect**  <br/>**first**  <br/>**toList** | observe |
| **共享** | shareIn  <br/>stateIn | LiveData（天然共享） |

---

## 四、使用场景对比

### **4.1 Flow 适用场景**

#### ✅ 适合使用 Flow 的场景

**复杂的数据流处理**

```kotlin
// 需要多个操作符链式调用
fun searchUsers(query: String): Flow<List<User>> {
    return flow {
        emit(query)
    }
    .debounce(300)
    .distinctUntilChanged()
    .flatMapLatest { q ->
        api.searchUsers(q)
    }
    .catch { e ->
        emit(emptyList())
    }
}
```

**异步数据流**

```kotlin
// 从多个异步源获取数据
fun loadUserData(userId: Int): Flow<UserData> = flow {
    val profile = async { getProfile(userId) }
    val posts = async { getPosts(userId) }
    emit(UserData(profile.await(), posts.await()))
}
```

**背压处理**

```kotlin
// 生产者快，消费者慢
fun fastProducer(): Flow<Int> = flow {
    for (i in 1..10000) {
        emit(i)
    }
}.buffer(10)  // 缓冲处理背压
```

**需要重试机制**

```kotlin
fun fetchWithRetry(): Flow<String> = flow {
    emit(fetchData())
}.retry(3) { e ->
    e is IOException
}
```

**冷流需求（按需执行）**

```kotlin
// 每次收集都重新执行
fun fetchData(): Flow<String> = flow {
    println("开始获取数据")
    emit(api.getData())
}
```

1. **与协程深度集成**

```kotlin
suspend fun processItems(): Flow<Result> {
    return channelFlow {
        for (item in items) {
            val result = processItem(item)
            send(result)
        }
    }
}
```

### **4.2 LiveData 适用场景**

#### ✅ 适合使用 LiveData 的场景

**简单的 UI 状态管理**

```kotlin
class UserViewModel : ViewModel() {
    private val _users = MutableLiveData<List<User>>()
    val users: LiveData<List<User>> = _users
    
    fun loadUsers() {
        _users.value = repository.getUsers()
    }
}
```

**生命周期感知是必需的**

```kotlin
// Activity/Fragment 中，自动管理生命周期
viewModel.users.observe(this) { users ->
    updateUI(users)  // 自动在主线程
}
```

**简单的数据更新**

```kotlin
// 不需要复杂的转换
viewModel.isLoading.observe(this) { loading ->
    progressBar.visibility = if (loading) VISIBLE else GONE
}
```

**单一数据源**

```kotlin
// 只需要一个值，不需要流
viewModel.currentUser.observe(this) { user ->
    displayUser(user)
}
```

**团队熟悉 LiveData**

```kotlin
// 现有项目使用 LiveData，保持一致性// 避免引入新的学习成本
```

### **4.3 混合使用场景**

```kotlin
class UserViewModel : ViewModel() {
    // Repository 层使用 Flow（处理复杂逻辑）
    private val userFlow: Flow<List<User>> = repository
        .getUsers()
        .debounce(300)
        .distinctUntilChanged()
    
    // ViewModel 层转换为 LiveData（简化 UI 层）
    val users: LiveData<List<User>> = userFlow.asLiveData()
    
    // 或者使用 StateFlow
    private val _usersState = MutableStateFlow<List<User>>(emptyList())
    val usersState: StateFlow<List<User>> = _usersState.asStateFlow()
    
    init {
        viewModelScope.launch {
            userFlow.collect { users ->
                _usersState.value = users
            }
        }
    }
}
```

---

## 五、原理深入解析

### **5.1 Flow 原理**

**Flow 的冷流机制**

```kotlin
// Flow 是冷流，每次 collect 都会重新执行
fun coldFlow(): Flow<Int> = flow {
    println("Flow 执行")
    emit(1)
    emit(2)
}

fun main() = runBlocking {
    coldFlow().collect { println("收集1: $it") }
    // 输出: Flow 执行, 收集1: 1, 收集1: 2
    
    coldFlow().collect { println("收集2: $it") }
    // 输出: Flow 执行, 收集2: 1, 收集2: 2
}
```

**实现原理：**

- `flow { ... }` 描述的是一段可挂起的生产逻辑，本身不保存“历史发射值”。
- 每次 `collect` 都会重新执行这段上游逻辑；执行发生在收集者所在协程中，并不必然“额外创建一个新协程”。
- `flowOn`、`buffer`、`channelFlow`、`shareIn` 等操作符可能引入额外协程或 Channel，因此不能把所有 Flow 都理解成同一条同步调用链。

**Flow 的状态机实现**

```kotlin
// 原始代码
fun flowExample(): Flow<Int> = flow {
    emit(1)
    delay(100)
    emit(2)
    delay(100)
    emit(3)
}

// 编译后的概念模型（不是可直接编译的反编译结果）
// 每个可能挂起的位置对应一个 label；局部变量会按需保存到状态机字段。
when (label) {
    0 -> {
        collector.emit(1)
        label = 1
        // delay 若真正挂起，则返回 COROUTINE_SUSPENDED
    }
    1 -> {
        collector.emit(2)
        label = 2
    }
    2 -> collector.emit(3)
}
```

要点不是记住伪代码长相，而是理解：`emit` 和 `delay` 都可能挂起；恢复后从对应
`label` 继续，而不是从函数第一行重新运行。

**StateFlow 的实现原理**

可以把 StateFlow **概念化**为“一个当前值 + 一组订阅槽位”，但不要用
`CopyOnWriteArrayList` 之类的示意代码冒充真实实现。真实实现会随
`kotlinx.coroutines` 版本演进，并使用专门的 slot、原子更新和恢复机制。

**关键点：**

**值比较**：使用 == 比较，相同值不发射

**立即订阅**：新订阅者立即收到当前值

**线程安全**：公开更新 API 是线程安全的；复合更新优先使用 `update { ... }`，避免“先读后写”竞争

**永不正常完成**：`StateFlow.collect` 通常一直挂起，直到收集协程被取消

**SharedFlow 的实现原理**

SharedFlow 可以概念化为“replay cache + 额外缓冲 + 订阅槽位”。需要特别注意：

- `replay` 决定新订阅者能补收多少个历史值。
- `extraBufferCapacity` 主要用于已有慢订阅者时吸收突发数据，不会扩大新订阅者的历史回放数量。
- 没有订阅者时只保留 `replay` 个值，发射者不会因为 `BufferOverflow.SUSPEND` 而等待。
- `emit` 可能挂起，`tryEmit` 只会立即返回成功或失败；二者不能无条件互换。

**关键点：**

**缓冲区**：保存历史值（replay）

**溢出策略**：DROP_OLDEST、DROP_LATEST、SUSPEND

**新订阅者**：收到最近 replay 个值

**无初始值限制**：不需要初始值

### **5.2 LiveData 原理**

**LiveData 的生命周期感知**

```kotlin
// LiveData 观察者注册（简化版）
class LiveData<T> {
    private val observers = SafeIterableMap<Observer<T>, ObserverWrapper>()
    
    @MainThread
    fun observe(owner: LifecycleOwner, observer: Observer<T>) {
        // 包装观察者
        val wrapper = LifecycleBoundObserver(owner, observer)
        observers.put(observer, wrapper)
        
        // 注册生命周期观察
        owner.lifecycle.addObserver(wrapper)
    }
    
    private inner class LifecycleBoundObserver(
        val owner: LifecycleOwner,
        val observer: Observer<T>
    ) : LifecycleObserver {
        
        fun onStateChanged() {
            if (owner.lifecycle.currentState == Lifecycle.State.DESTROYED) {
                removeObserver(observer)
            } else {
                activeStateChanged(
                    owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
                )
            }
        }
    }
}
```

**关键机制：**

**LifecycleOwner**：关联生命周期组件（Activity/Fragment）

**ObserverWrapper**：包装原始观察者，添加生命周期感知

**自动管理**：生命周期变化时自动添加/移除观察

**内存安全**：组件销毁时自动移除观察者

**LiveData 的线程切换**

```kotlin
// LiveData 的值更新（简化版）
class LiveData<T> {
    private var version = -1
    private var data: T? = null

    @MainThread
    fun setValue(value: T) {
        version++
        data = value
        // 通知所有活跃的观察者
        dispatchingValue(null)
    }
    
    // 从任意线程设置值
    fun postValue(value: T) {
        // 概念示意：真实实现会合并尚未派发的 pending 值
        val postTask = Runnable {
            setValue(value)
        }
        mainHandler.post(postTask)
    }
    
    private fun dispatchingValue(initiator: ObserverWrapper<T>?) {
        observers.forEach { (observer, wrapper) ->
            if (wrapper.isActive) {
                wrapper.considerNotify(observer)
            }
        }
    }
}
```

**关键点：**

**版本号**：使用 version 防止重复通知

**主线程切换**：postValue 使用 Handler 切换到主线程

**活跃状态**：只通知处于活跃状态的观察者

**连续 postValue 的合并**：主线程处理任务前多次调用 `postValue`，中间值可能被后一个值覆盖

**Transformations 的实现原理**

```kotlin
// Transformations.map 实现（简化版）
class Transformations {
    companion object {
        fun <X, Y> map(
            source: LiveData<X>,
            transform: (X) -> Y
        ): LiveData<Y> {
            val result = MediatorLiveData<Y>()
            result.addSource(source) { x ->
                result.value = transform(x)
            }
            return result
        }
        
        fun <X, Y> switchMap(
            source: LiveData<X>,
            transform: (X) -> LiveData<Y>
        ): LiveData<Y> {
            val result = MediatorLiveData<Y>()
            var currentSource: LiveData<Y>? = null
            
            result.addSource(source) { x ->
                // 移除旧的源
                currentSource?.let { result.removeSource(it) }
                // 添加新的源
                val newSource = transform(x)
                currentSource = newSource
                result.addSource(newSource) { y ->
                    result.value = y
                }
            }
            return result
        }
    }
}
```

**关键点：**

**MediatorLiveData**：用于组合多个 LiveData

**惰性激活**：结果 LiveData 有活跃观察者时才监听上游；上游每次派发都会执行转换，并不自动按值去重

**自动清理**：switchMap 自动移除旧的源

**生命周期清理**：生命周期观察者会在 `DESTROYED` 后移除；`observeForever` 仍需手动移除

### **5.3 性能对比**

**内存占用**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **冷流存储** | 不存储数据，只存储逻辑 | 存储当前值 |
| **热流存储** | StateFlow: 1个值  <br/>SharedFlow: replay个值 | 1个值 |
| **观察者/收集者管理** | 由协程与具体 Flow 实现管理 | 内部映射保存 ObserverWrapper |

**执行效率**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **创建** | 很快（只是函数引用） | 很快（对象创建） |
| **发射值** | 快（直接传递） | 快（版本号检查） |
| **观察者通知** | 可能同协程直接调用，也可能经过调度/缓冲 | `setValue` 主线程直接派发；`postValue` 切到主线程 |
| **线程切换** | 协程调度器 | Handler |

---

## 六、最佳实践

### **6.1 Flow 最佳实践**

**1. 使用适当的 Flow 类型**

```kotlin
// ✅ StateFlow - UI 状态
private val _uiState = MutableStateFlow<UiState>(UiState.Loading)
val uiState: StateFlow<UiState> = _uiState.asStateFlow()

// ✅ SharedFlow - 一次性事件
private val _events = MutableSharedFlow<Event>()
val events: SharedFlow<Event> = _events.asSharedFlow()

// ✅ Flow - 数据流
fun searchResults(query: String): Flow<List<Result>> = flow {
    emit(search(query))
}.debounce(300)
```

**2. 正确处理异常**

```kotlin
fun fetchData(): Flow<String> = flow {
    emit(api.getData())
}
.catch { e ->
    // 处理异常，可以继续发射值
    emit("默认值")
    // 或者记录日志
    Log.e(TAG, "错误", e)
}
.onCompletion { cause ->
    if (cause == null) {
        Log.d(TAG, "完成")
    } else {
        Log.e(TAG, "异常完成", cause)
    }
}
```

**3. 使用背压处理**

```kotlin
// 生产者快，消费者慢
fun fastFlow(): Flow<Int> = flow {
    for (i in 1..10000) {
        emit(i)
    }
}
.buffer(10)  // 缓冲10个值
.collect { value ->
    delay(100)  // 慢速消费
    process(value)
}
```

**4. 生命周期管理**

```kotlin
// ✅ UI 收集推荐使用 repeatOnLifecycle
lifecycleScope.launch {
    repeatOnLifecycle(Lifecycle.State.STARTED) {
        flow.collect { value ->
            updateUI(value)
        }
    }
}

// ❌ 错误：GlobalScope（生命周期过长）
GlobalScope.launch {
    flow.collect { value ->
        updateUI(value)  // 可能导致崩溃
    }
}
```

**5. 避免重复收集**

```kotlin
// ✅ 使用 stateIn 或 shareIn 避免重复执行
private val sharedFlow = repository.getData()
    .shareIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5000),
        replay = 1
    )

// ❌ 每次 collect 都会重新执行
flow.collect { }  // 执行一次
flow.collect { }  // 又执行一次
```

### **6.2 LiveData 最佳实践**

**1. 使用不可变 LiveData**

```kotlin
class ViewModel {
    // ✅ 私有可变
    private val _users = MutableLiveData<List<User>>()
    
    // ✅ 公开不可变
    val users: LiveData<List<User>> = _users
    
    fun loadUsers() {
        _users.value = repository.getUsers()
    }
}
```

**2. 避免在 LiveData 中执行耗时操作**

```kotlin
// ❌ 错误：仅仅进入 viewModelScope 不等于切到后台线程；
// 如果 repository.getData() 是阻塞调用，仍会卡住主线程。
fun loadData() {
    viewModelScope.launch {
        val data = repository.getData()
        _data.value = data
    }
}

// ✅ 正确：在 Repository 层处理异步
class Repository {
    suspend fun getData(): Data {
        return withContext(Dispatchers.IO) {
            // IO 操作
        }
    }
}
```

**3. 使用 Transformations 简化逻辑**

```kotlin
// ✅ 使用 map 转换
val userName: LiveData<String> = Transformations.map(userId) { id ->
    repository.getUserName(id)
}

// ✅ 使用 switchMap 切换数据源
val user: LiveData<User> = Transformations.switchMap(userId) { id ->
    repository.getUser(id)
}
```

**4. 避免内存泄漏**

```kotlin
// ✅ 正确：使用 observe（自动管理）
liveData.observe(this) { value ->
    updateUI(value)
}

// ❌ 错误：observeForever 需要手动移除
liveData.observeForever { value ->
    updateUI(value)
}
// 忘记移除会导致内存泄漏
```

### **6.3 混合使用最佳实践**

**Repository 层使用 Flow**

```kotlin
class UserRepository {
    // 返回 Flow，处理复杂的数据流
    fun searchUsers(query: String): Flow<List<User>> = flow {
        emit(emptyList())
        delay(300)  // 防抖
        val results = api.search(query)
        emit(results)
    }
    .catch { e ->
        emit(emptyList())
    }
}
```

**ViewModel 层转换为 StateFlow/LiveData**

```kotlin
class UserViewModel : ViewModel() {
    // 方式1：使用 StateFlow
    val users: StateFlow<List<User>> = repository
        .searchUsers("")
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5000),
            initialValue = emptyList()
        )
    
    // 方式2：转换为 LiveData
    val usersLiveData: LiveData<List<User>> = repository
        .searchUsers("")
        .asLiveData()
}
```

**UI 层观察**

```kotlin
// 使用 StateFlow
lifecycleScope.launch {
    viewModel.users.collect { users ->
        updateUI(users)
    }
}

// 或使用 LiveData
viewModel.usersLiveData.observe(this) { users ->
    updateUI(users)
}
```

---

## 七、迁移指南

### **7.1 从 LiveData 迁移到 Flow**

**步骤1：替换数据源**

```kotlin
// 之前：LiveData
private val _users = MutableLiveData<List<User>>()
val users: LiveData<List<User>> = _users

// 之后：StateFlow
private val _users = MutableStateFlow<List<User>>(emptyList())
val users: StateFlow<List<User>> = _users.asStateFlow()
```

**步骤2：替换设置值**

```kotlin
// 之前：LiveData
_users.value = newUsers
_users.postValue(newUsers)

// 之后：StateFlow
_users.value = newUsers
_users.tryEmit(newUsers)  
// 或者 value = ...
```

**步骤3：替换观察**

```kotlin
// 之前：LiveData
viewModel.users.observe(this) { users ->
    updateUI(users)
}

// 之后：StateFlow。UI 层推荐按生命周期重复收集。
lifecycleScope.launch {
    repeatOnLifecycle(Lifecycle.State.STARTED) {
        viewModel.users.collect { users ->
            updateUI(users)
        }
    }
}
```

**步骤4：替换 Transformations**

```kotlin
// 之前：LiveData
val userName = Transformations.map(userId) { id ->
    repository.getUserName(id)
}

// 之后：Flow
val userName: Flow<String> = userId
    .map { id ->
        repository.getUserName(id)
    }
```

### **7.2 常见迁移场景**

**场景1：简单状态**

```kotlin
// LiveData
val isLoading = MutableLiveData<Boolean>()

// StateFlow
val isLoading = MutableStateFlow<Boolean>(false)
```

**场景2：一次性事件**

```kotlin
// LiveData（使用 SingleLiveEvent 或 EventWrapper）
private val _event = MutableLiveData<Event>()
val event: LiveData<Event> = _event

// SharedFlow：适合“在线订阅者消费”的事件
private val _event = MutableSharedFlow<Event>()
val event: SharedFlow<Event> = _event.asSharedFlow()
```

`MutableSharedFlow(replay = 0)` 在没有订阅者时不会保存事件。若“页面暂时停止收集也绝不能丢”
是业务要求，应先明确事件语义，再考虑状态建模、持久队列或 Channel；不要仅因它叫
SharedFlow 就默认一次性事件一定可靠送达。

**场景3：列表数据**

```kotlin
// LiveData
val users = MutableLiveData<List<User>>()

// StateFlow
val users = MutableStateFlow<List<User>>(emptyList())
```

---

## 八、总结

### **8.1 核心对比**

| **操作** | **Flow** | **LiveData** |
| --- | --- | --- |
| **类型** | `flow {}` 默认是冷流；StateFlow/SharedFlow 是热流 | 热数据持有者 |
| **生命周期感知** | 需要配合 Lifecycle | 内置 |
| **操作符** | 丰富的操作符链 | Transformations |
| **异常处理** | catch、retry | try-catch |
| **背压处理** | buffer、conflate | 不支持 |
| **协程集成** | 深度集成 | 需要转换 |
| **学习曲线** | 较陡 | 平缓 |

### **8.2 选择建议**

**选择 Flow 如果：**

- ✅ 需要复杂的数据流处理

- ✅ 需要背压处理

- ✅ 需要丰富的操作符

- ✅ 项目已广泛使用协程

- ✅ 需要冷流特性

**选择 LiveData 如果：**

- ✅ 简单的 UI 状态管理

- ✅ 团队熟悉 LiveData

- ✅ 需要生命周期感知

- ✅ 现有项目使用 LiveData

- ✅ 不需要复杂的数据流

**混合使用：**

Repository 层：Flow（处理复杂逻辑）

ViewModel 层：StateFlow 或转换为 LiveData

UI 层：根据情况选择

### **8.3 关键要点**

**Flow 是数据流，LiveData 是数据持有者**

**StateFlow ≈ MutableLiveData，但有初始值要求**

**SharedFlow 可用于一次性事件，但要先设计清楚无订阅者、缓冲和丢失策略**

**LiveData 生命周期感知更方便**

**Flow 操作符更强大**

**可以混合使用，发挥各自优势**
