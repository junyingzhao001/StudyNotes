# 98 Android Binder 性能与故障：线程池饥饿、oneway 积压、锁与调用链诊断

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`
> 阅读方式：不需要编译 AOSP；跟着一个 system_server 卡顿场景，学会从现象追到真正的等待者。

这篇文章解决一个很实际的问题：

> 应用栈停在 `BinderProxy.transactNative()`，system_server 又出现 Binder 线程池告警，
> 到底是驱动慢、服务慢、oneway 堵了，还是一把锁制造了跨进程死锁？

读完后，你应该能做到四件事：

- 把一次 Binder “很慢”拆成排队、服务端执行、同步调用方等待三本账；
- 解释 system_server 的 `31` 是怎样配置的，以及线程为什么不是一次性建齐；
- 判断 oneway 是“调用方不等业务回复”，而不是“服务端无限并发”；
- 用线程栈、Perfetto、BinderCallsStats 和 binderfs 快照拼出等待链。

一句话结论：

> Binder 通常只是把阻塞传播给调用方；真正的根因常在服务端锁、下游同步调用或无背压的
> oneway 生产者。先找到“谁在等谁”，再谈扩线程池。

文中的 `ISettingsPolicy`、`IVendorPolicy` 是为诊断教学组合出的接口名，不是声称 AOSP 中存在同名故障。

## 1. 问题：点一下设置，为什么像整个系统都卡住了

先看贯穿全篇的诊断场景。

用户在设置应用里点击“读取策略”。调用链如下：

~~~text
Settings 主线程
  → 同步调用 ISettingsPolicy.getEffectivePolicy()
      → system_server Binder 线程进入 PolicyService
          → 持有 mLock
          → 同步调用 vendor 进程 IVendorPolicy.readPolicy()
~~~

vendor 服务把读取工作交给自己的工作线程，并等待结果。

这个 vendor 工作线程又同步回调 system_server：

~~~text
vendor worker
  → IPolicyCallback.onBeforeRead()
      → system_server 另一条 Binder 线程
          → 等待同一个 mLock
~~~

于是等待关系闭环：

~~~text
system_server Binder #12
  持有 mLock
  └─ 等 vendor readPolicy() 返回
       vendor Binder 线程
       └─ 等 vendor worker 完成
            vendor worker
            └─ 等 system_server callback 返回
                 system_server Binder #19
                 └─ 等 mLock
~~~

与此同时，其他调用进入 `PolicyService` 后也等待 `mLock`。当足够多的 Binder 线程都困在这条等待链上，新事务只能排队。

另一个客户端还在高频发送：

~~~aidl
oneway void notifyPolicyInputChanged(int generation);
~~~

这里的 `generation` 是递增的状态版本号：数字越大，代表更新的状态。

调用方看起来“发送很快”，但状态迟迟没有生效。

这里其实混着三个不同问题：

1. 设置应用为什么一直等？
2. system_server 的 Binder 线程为什么没有归还线程池？
3. oneway 为什么返回了，服务端却还没处理？

若把三个问题都叫作“Binder 慢”，修复方向很容易跑偏。

## 2. 先拆三本时间账：排队、执行、同步等待

### 2.1 同步调用方看到的是端到端时间

普通 AIDL 方法默认是同步事务。

~~~mermaid
sequenceDiagram
    participant C as 应用调用线程
    participant D as Binder 驱动
    participant S as system_server Binder 线程
    participant V as vendor 服务
    C->>D: 提交同步事务
    Note over C: 等待最终 reply
    D->>S: 投递事务
    S->>V: 下游同步调用
    Note over S: 等待下游 reply
    V-->>S: reply
    S-->>D: reply
    D-->>C: 唤醒并返回
~~~

调用方从调用到返回的总时间，大致包含：

~~~text
T客户端 =
  参数序列化
  + 目标进程排队
  + 服务端 onTransact 与业务执行
  + 服务端内部等待
  + reply 序列化与返回调度
~~~

所以客户端栈停在：

~~~text
android.os.BinderProxy.transactNative
~~~

只证明“这条线程还在等同步事务结束”。它不能证明 CPU 时间花在 Binder 驱动里。

这像打客服电话：你听到的是等待音乐，但真正耽搁可能是坐席在等另一个部门盖章。

### 2.2 驱动排队时间回答“多久才有人接单”

排队从事务可被目标进程接收开始，到目标线程真正接到事务为止。
排队变长可能是：

- 目标 Binder 线程都在执行或阻塞；
- 目标线程虽可运行，却长时间没有得到 CPU；
- 同一 Binder node 的前序异步事务尚未完成；
- 目标进程被冻结、卡死或正承受严重内存压力。

排队时间不能只靠一份 Java 栈精确得出。更适合用 Binder ftrace/Perfetto 的 transaction 与 received 事件连接两端。
### 2.3 服务端执行时间不等于“有效 CPU 计算”

服务端从进入 Java Binder Stub 到返回，期间可能：

- 真正在 CPU 上运行；
- 等 Java 锁或 native 锁；
- 等 Handler；
- 等磁盘或设备 I/O；
- 再发同步 Binder，并等待下游 reply。

Android 11 的 `BinderCallsStats` 同时取得线程 CPU 时间和 elapsed real time：

~~~java
final long duration =
        getThreadTimeMicro() - s.cpuTimeStarted;
final long latencyDuration =
        getElapsedRealtimeMicro() - s.timeStarted;
callStat.cpuTimeMicros += duration;
callStat.latencyMicros += latencyDuration;
~~~

路径：`frameworks/base/core/java/com/android/internal/os/BinderCallsStats.java`

如果 elapsed time 高而线程 CPU time 低，说明“等待”是重要候选；但统计本身不会告诉你在等哪把锁、哪个 Handler 或哪个远端进程。
### 2.4 三本账如何配合

| 要回答的问题 | 首选证据 | 不能单独证明什么 |
|---|---|---|
| 调用方总共等了多久 | 客户端 trace、方法埋点、Perfetto | 根因就在客户端或驱动 |
| 事务多久才到服务线程 | Binder flow / ftrace | 服务方法内部做了什么 |
| Stub 进入后耗时在哪 | 服务端栈、服务 trace、CPU/elapsed 对比 | 排队前发生了什么 |

诊断时先明确自己在量哪一本账。把三者混成一个“Binder latency”，后面的优化很难验证。

## 3. 机制：system_server 的 Binder 线程池怎样长到配置上限

### 3.1 31 是配置值，不是预先创建的线程数

Android 11 r48 的 `SystemServer` 明确把最大线程配置为 31：

~~~java
// maximum number of binder threads used for system_server
// will be higher than the system default
private static final int sMaxBinderThreads = 31;
// Increase the number of binder threads in system_server
BinderInternal.setMaxThreads(sMaxBinderThreads);
~~~

路径：`frameworks/base/services/java/com/android/server/SystemServer.java`

JNI 最终调用：

~~~cpp
static void android_os_BinderInternal_setMaxThreads(
        JNIEnv*, jobject, jint maxThreads) {
    ProcessState::self()
            ->setThreadPoolMaxThreadCount(maxThreads);
}
~~~

`ProcessState` 再通过 `BINDER_SET_MAX_THREADS` 告诉驱动。

普通 libbinder 进程打开驱动时的默认配置是 15：

~~~cpp
#define DEFAULT_MAX_BINDER_THREADS 15

size_t maxThreads = DEFAULT_MAX_BINDER_THREADS;
result = ioctl(fd, BINDER_SET_MAX_THREADS, &maxThreads);
~~~

system_server 随后把自己的配置改为 31。这不是说所有进程都是 31，也不是说启动时已经存在 31 条工作线程。

更准确的理解是：

> 31 是 system_server 交给 Binder 驱动、同时保存在 libbinder 中的池上限配置；
> 线程按需求增长，上限也不等于同时完成 31 份独立业务的保证。

驱动请求创建的注册线程、主动加入池的线程，以及同步等待期间承接嵌套事务的线程，会让“进程里实际可见多少条相关线程”比一句“池大小等于 31”更复杂。
### 3.2 第一条池线程由用户空间主动启动

`ProcessState::startThreadPool()` 只在第一次调用时创建主池线程：

~~~cpp
void ProcessState::startThreadPool() {
    AutoMutex _l(mLock);
    if (!mThreadPoolStarted) {
        mThreadPoolStarted = true;
        spawnPooledThread(true);
    }
}
~~~

这条线程进入 `joinThreadPool(true)`，向驱动发送 `BC_ENTER_LOOPER`。

system_server 走过 `ZygoteInit.nativeZygoteInit()` 后，`frameworks/base/cmds/app_process/app_main.cpp` 会调用 `startThreadPool()`。
### 3.3 后续线程是驱动按需提出、用户空间创建

当驱动判断需要补充线程且未超过配置边界时，会返回 `BR_SPAWN_LOOPER`。libbinder 的处理很直接：

~~~cpp
case BR_SPAWN_LOOPER:
    mProcess->spawnPooledThread(false);
    break;
~~~

新线程用 `joinThreadPool(false)` 加入，并发送 `BC_REGISTER_LOOPER`。

可以把它想成收费站：

- `startThreadPool()` 先开一个窗口；
- 排队出现且符合驱动条件时，驱动亮出“再开窗口”的牌子；
- 用户空间收到牌子后创建线程；
- 达到配置边界后，不会因为队伍继续变长就无限开窗口。

`BR_SPAWN_LOOPER` 的边界也要说清：

- 它是驱动给用户空间的请求，不是“一笔事务固定创建一条线程”；
- `spawnPooledThread()` 只有在池已经启动时才真正创建；
- 创建线程仍受调度和资源影响，不等于事务立刻开始；
- 线程已创建也可能全部卡在同一把锁上。
### 3.4 “线程池满”可能几乎不消耗 CPU

`IPCThreadState` 对正在处理命令的线程计数。当计数长时间顶到本地 `mMaxThreads`，离开饥饿状态时会记录：

~~~cpp
if (starvationTimeMs > 100) {
    ALOGE("binder thread pool (%zu threads) starved for %"
          PRId64 " ms", mProcess->mMaxThreads,
          starvationTimeMs);
}
~~~

这里的 100 ms 是 r48 代码里的日志阈值，不是“超过 100 ms 就一定死锁”。

当执行中计数达到 31，而相关线程都睡在锁、条件变量或下游 Binder 上时，线程池已经没有及时处理新入站工作的余量，但 CPU 使用率完全可能不高。

## 4. oneway：不等业务 reply，为什么仍会排队

### 4.1 oneway 省掉的是哪一段等待

AIDL 的 oneway 最终带上 `TF_ONE_WAY` / `FLAG_ONEWAY`。

libbinder 的关键分支是：

~~~cpp
if ((flags & TF_ONE_WAY) == 0) {
    err = waitForResponse(reply);
} else {
    err = waitForResponse(nullptr, nullptr);
}
~~~

看到 oneway 分支仍调用 `waitForResponse`，不要误以为它在等服务端结果。`waitForResponse(nullptr, nullptr)` 收到 `BR_TRANSACTION_COMPLETE` 后即可结束：

~~~cpp
case BR_TRANSACTION_COMPLETE:
    if (!reply && !acquireResult) goto finish;
    break;
~~~

这个 complete 表示发送侧事务命令已被驱动处理，不是“服务端方法执行完成”。远端方法可能在调用方返回前已经开始，也可能稍后才获得线程；调用方不能拿自己的返回时刻推断服务端进度。

因此最准确的说法是：

> oneway 不同步等待服务端业务 reply；它仍要序列化、进入驱动并完成发送侧提交，
> 所以也不是一条保证瞬时返回、永不阻塞的普通函数。

服务端执行完 oneway 后不会发送 reply。服务端抛出的业务异常也无法沿同一次调用返回客户端。

如果调用方需要确认完成，应把协议设计成：

~~~text
submit(requestId)  --oneway-->
                  <-- callback(requestId, result)
~~~

或者提交后用 `requestId` 查询状态。“oneway 调用返回”不能充当完成回执。

### 4.2 同一 Binder node 的异步队列为何会积压

这里的 node 可以先理解成“某个服务端 Binder 对象在驱动里的身份”。

Binder 对同一目标 node 的异步事务维持串行处理边界：一笔异步事务尚未完成时，后续异步事务进入该 node 的 async 队列。

r48 用户空间仓库没有设备内核的 `drivers/android/binder.c`，但 libbinder 测试明确构造了两笔 oneway，并检查预期顺序：

~~~cpp
ret = pollServer->transact(
        BINDER_LIB_TEST_DELAYED_CALL_BACK,
        data, nullptr, TF_ONE_WAY);
// second transaction will end up on the async_todo list
ret = pollServer->transact(
        BINDER_LIB_TEST_DELAYED_CALL_BACK,
        data2, nullptr, TF_ONE_WAY);
~~~

路径：`frameworks/native/libs/binder/tests/binderLibTest.cpp` 的 `OnewayQueueing`。

这个串行边界不是“整个进程所有 oneway 全部串行”：

- 不同 Binder node 可以有不同的异步处理进度；
- 同步事务不因此变成同一条全局 FIFO；
- 多个发送者之间不要自行推导业务级全局顺序；
- Stub 收到后若再投递到别的 Executor/Handler，后续业务顺序由那个队列决定。

回到诊断场景：

1. 第一笔 `notifyPolicyInputChanged()` 在服务端等待 `mLock`；
2. 同一 node 的后续 oneway 继续排队；
3. 客户端每次都不等业务完成，所以生产速度没有被服务端自然限制；
4. 用户最终看到的是“调用都返回了，状态却越来越旧”。

这就像把快件不断放进驿站：寄件人不用等收件人拆包，但驿站只有一条对应货架的处理通道。入库成功不等于已经送到家。

### 4.3 oneway 需要业务层背压

状态通知通常不该把每个瞬间都当成必须送达的历史事件。可选策略包括：

- 发送前合并，只发送最新 generation；
- 服务端用单槽“最新状态”覆盖旧状态；
- 给队列设上限，并定义溢出行为；
- 消费端丢弃 generation 小于当前值的旧消息；
- 必须逐条可靠处理时，使用有确认、有窗口的协议。

Binder 的 oneway 标志只改变调用/回复语义，不会替业务决定丢弃、重试、合并或限速。

## 5. 根因：持锁跨 Binder 怎样形成等待环

### 5.1 危险点不是“用了锁”，而是锁的释放受远端控制

下面这类代码最值得警惕：

~~~java
synchronized (mLock) {
    updateLocalStateLocked();
    return mVendor.readPolicy(); // 远端同步 Binder
}
~~~

一旦进入 `readPolicy()`，`mLock` 的持有时长就由远端决定。

远端可能：

- 等自己的工作线程；
- 等设备 I/O；
- 再调用第三个进程；
- 回调当前进程；
- 已经发生线程池饥饿。

于是一个本地临界区被扩展成跨进程临界区。其他本来很快的方法也会堵在 `mLock` 后面。

### 5.2 本案例为何不会被简单 Binder 重入自动解开

libbinder 的同步等待循环并非只能接收 `BR_REPLY`。遇到其他命令时，它会执行：

~~~cpp
default:
    err = executeCommand(cmd);
    if (err != NO_ERROR) goto finish;
    break;
~~~

这意味着等待同步 reply 的 Binder 线程具备处理嵌套入站事务的能力。某些直接的 A→B→A 调用可能因此回到原等待线程。

但这不是死锁免疫：

- 回调可能由 B 的另一工作线程发起，不在原嵌套调用栈上；
- 回调可能先 post 到 A 的 Handler，再同步等待 Handler；
- 环中可能还有第三个进程或第二把锁；
- native 非递归锁与 Java 可重入 monitor 的行为不同；
- 即使同线程重入成功，也可能破坏“外部调用期间状态不变”的假设。

本案例中，vendor Binder 线程等待 vendor worker，而 callback 是 vendor worker 新发起的同步事务。system_server 中承接 callback 的另一 Binder 线程只能等待 `mLock`，所以环仍然成立。

### 5.3 先快照，锁外调用，再校验 generation

一种常见改法是：

~~~java
final Snapshot snapshot;
final long generation;
synchronized (mLock) {
    snapshot = makeSnapshotLocked();
    generation = mGeneration;
}
Result result = mVendor.readPolicy(snapshot); // 锁外 IPC
synchronized (mLock) {
    if (generation == mGeneration) {
        applyResultLocked(result);
    }
}
~~~

这里解决了两件事：

- 远端再慢，也不再占着 `mLock`；
- 远端返回时用 generation 防止旧结果覆盖新状态。

它也有局限：

- 快照必须足够表达本次请求；
- 状态变化后，是丢弃、重试还是合并，需要业务定义；
- 锁外调用期间对象生命周期必须安全；
- 不能为了“移到锁外”而悄悄破坏原来的原子语义。

所以修改前要先写清不变量，再决定快照内容和冲突策略。

## 6. 方案：为什么盲目增加线程通常只是延后爆发

假设 31 个执行槽位都在等待同一个 `mLock`。把配置提高后，新增线程也会走到同一行等待。

结果通常只是：

~~~text
原来：较少等待者 + 较早出现池耗尽
后来：更多等待者 + 更多线程/栈内存/调度与锁竞争
最终：共同阻塞点仍未释放
~~~

扩线程还可能把压力继续传给 vendor 服务，让下游更快达到自己的队列或线程上限。

优先修复顺序应当是：

1. 找出大多数 Binder 线程共同等待的位置；
2. 找到锁 owner 或最末端未返回的下游调用；
3. 移除锁内同步 IPC；
4. 缩短 Binder Stub 内的同步工作；
5. 为 oneway 增加合并、限速或确认；
6. 再评估独立短事务是否真的缺少并行度。
不同根因对应不同方案：

| 证据 | 更可能的机制 | 优先方案 |
|---|---|---|
| 多数线程等待同一锁 | 临界区过大或锁内外调 | 找 owner，拆临界区，锁外 IPC |
| 多数线程停在同一远端 transact | 下游慢或环形等待 | 继续跨进程追踪，改异步协议 |
| Binder elapsed 高、CPU 低 | 锁/I/O/Handler/下游等待 | 找具体等待对象 |
| CPU 与 elapsed 都高 | 服务端计算或高频调用 | 优化算法、缓存、批处理 |
| oneway 状态越来越旧 | 生产快于单 node 消费 | 合并最新状态、限流、确认 |
| 线程互不依赖且都是短任务 | 突发并发确实过高 | 测量后再评估池配置 |

线程上限不是永远不能改。只有证据显示请求彼此独立、没有共同锁与下游瓶颈，并且增加并发不会破坏内存和尾延迟时，它才可能是合理调参。

## 7. 验证：四类证据怎样拼成一条等待链

### 7.1 线程栈先回答“此刻停在哪里”

需要同时看：

- 调用应用的阻塞线程；
- system_server 的全部 Binder 线程；
- 涉及的 vendor/native 服务线程；
- 可能承接回调或 Handler 消息的线程。

如果只截到应用主线程：

~~~text
main → BinderProxy.transactNative
~~~

只能标出等待链起点。

本案例的关键证据应是同一时刻出现：

~~~text
App main                 → 等 ISettingsPolicy reply
system_server Binder #12 → 持 mLock，等 IVendorPolicy reply
vendor Binder            → 等 worker
vendor worker            → 等 callback reply
system_server Binder #19 → 等 mLock
~~~

Java traces 常能给出“waiting to lock”及 owner 线索；native futex 还要结合符号、锁日志和相邻线程状态。

不要因为很多线程栈相同，就立即把那一行叫根因。共同等待点的 owner 才是下一站。

### 7.2 Perfetto 负责把跨进程片段连起来

r48 的 atrace 类别 `binder_driver` 启用：

~~~text
binder_transaction
binder_transaction_received
binder_transaction_alloc_buf
binder_set_priority（可选）
~~~

对应注册位于：`frameworks/native/cmds/atrace/atrace.cpp`。

在 Perfetto 中沿 flow 检查：

1. 客户端何时提交 transaction；
2. system_server 哪条线程何时 received；
3. 该线程何时再发往 vendor；
4. vendor callback 是否从另一线程返回 system_server；
5. 各线程是 Running、Runnable，还是 blocked/sleeping；
6. reply 最后停在哪一段。

Perfetto 给出时序关系，线程栈给出代码位置。两者结合，才能区分“没调度到”和“拿不到锁”。

### 7.3 BinderCallsStats 用来找入口热点，不负责找锁 owner

Android 11 的服务名是 `binder_calls_stats`。在有相应权限的调试设备上，可以先查看帮助，再限定复现窗口：

~~~bash
adb shell dumpsys binder_calls_stats -h
adb shell dumpsys binder_calls_stats --reset
# 在这里复现一次问题，再读取这一窗口的统计
adb shell dumpsys binder_calls_stats
~~~

需要 Parcel 大小、异常等详细维度时，可临时启用 detailed tracking；完成后应恢复：

~~~bash
adb shell dumpsys binder_calls_stats --enable-detailed-tracking
adb shell dumpsys binder_calls_stats --disable-detailed-tracking
~~~

读取统计时记住四个边界：

- 它围绕 Java `Binder.execTransactInternal()` 记账，不含事务到达 Stub 前的驱动排队；
- CPU time 只统计当前线程实际消耗，elapsed time 会包含内部等待；
- 默认存在采样与条目上限，未出现不代表从未调用；
- r48 在设备状态尚未就绪或正在充电时会跳过 `callStarted` 记录。

因此它适合回答“哪个 Java Binder 入口调用多、累计 CPU/elapsed 可疑”，不适合单独回答“锁由谁持有”。

### 7.4 binderfs 快照告诉你驱动此刻看到什么

Android 11 r48 的 `init.rc` 挂载 `/dev/binderfs`，`dumpstate` 会优先读取：

~~~text
/dev/binderfs/binder_logs/state
/dev/binderfs/binder_logs/stats
/dev/binderfs/binder_logs/transactions
/dev/binderfs/binder_logs/proc/<pid>
~~~

不可访问时，r48 的 dumpstate 还会回退到 `/sys/kernel/debug/binder`。

这些文件受 build 类型、SELinux、内核和权限影响。普通 user 设备不一定允许直接读取，优先从已授权的 bugreport 获取。

驱动快照能帮助确认线程、node、transaction 与 async 队列状态，但格式随设备内核变化，且它仍不能替代业务源码中的锁关系。

### 7.5 Watchdog 是报警器，不是根因分析器

`Watchdog.BinderThreadMonitor` 调用：

~~~java
public void monitor() {
    Binder.blockUntilThreadAvailable();
}
~~~

native 侧等待 `mExecutingThreadsCount < mMaxThreads`。

它长时间不返回，说明 system_server 缺少可及时接收入站工作的 Binder 执行容量。它不会自动指出是哪把锁、哪个 transaction 或哪个 vendor 服务造成的。

还要注意：`dumpsys` 自己也常通过 Binder 请求服务。池已经完全堵住时，诊断命令可能卡住或改变现场；Watchdog 自动 traces、预先启用的 Perfetto 环形缓冲和 bugreport 中已有快照因此很重要。

## 8. 用“问题 → 机制 → 验证”闭环本案例

### 问题

可观察到的事实是：

- 设置应用同步调用未返回；
- system_server Binder 可用性出现告警；
- oneway 发送者没有同步报错，但状态更新迟到。

这些是症状，不是结论。

### 机制假设

根据调用链提出可证伪假设：

~~~text
锁内下游同步 IPC
  → vendor worker 发起非原事务栈上的同步回调
  → callback 等 system_server 的 mLock
  → 原线程持锁等 vendor
  → 其他 Binder 线程继续堆在 mLock
  → 池可用容量耗尽
  → 同 node oneway 继续积压
~~~

### 验证

只有下面的证据能够互相对应，假设才站得住：

- 栈能连出持锁者、远端等待者和 callback 等锁者；
- Perfetto 能连出 app→system_server→vendor→system_server callback；
- Binder 快照与线程栈中的 pid/tid、transaction 方向一致；
- BinderCallsStats 若有样本，入口 elapsed 明显包含等待，而非只看 CPU；
- 移除锁内 IPC 后，同一等待环不再出现，相关线程能够归还。

验证修复时不要写“性能明显提升”就结束。至少比较同样复现场景中的：

- 同步调用端到端分布；
- Binder received 前的排队；
- 服务端 Stub elapsed 与 CPU；
- 池饥饿日志是否再现；
- oneway 最大滞后 generation 或业务队列深度；
- 功能语义是否因快照冲突策略发生变化。

这里不填写虚构数值。真实项目应记录设备、build、负载、样本数和统计窗口，再报告结果。

## 9. Mac 上只读源码：不编译也能验证哪些结论

以下命令都以源码根目录 `/Users/ninebot/androidSource` 为当前目录。

### 9.1 先确认两个核心仓库的版本

~~~bash
git -C frameworks/base describe --tags --always
git -C frameworks/native describe --tags --always
~~~

本章对应输出应是 `android-11.0.0_r48`。若 tag 不同，应重新核对常量、统计开关和驱动接口。

### 9.2 追 31 从 Java 到 ioctl

~~~bash
rg -n "sMaxBinderThreads|setMaxThreads" \
  frameworks/base/services/java/com/android/server/SystemServer.java \
  frameworks/base/core/jni/android_util_Binder.cpp
rg -n "DEFAULT_MAX_BINDER_THREADS|BINDER_SET_MAX_THREADS" \
  frameworks/native/libs/binder/ProcessState.cpp
~~~

### 9.3 追线程池从首线程到动态扩展

~~~bash
rg -n "startThreadPool|spawnPooledThread|BC_ENTER_LOOPER|BC_REGISTER_LOOPER" \
  frameworks/native/libs/binder/ProcessState.cpp \
  frameworks/native/libs/binder/IPCThreadState.cpp
rg -n "BR_SPAWN_LOOPER" \
  frameworks/native/libs/binder/IPCThreadState.cpp
~~~

应能画出：

~~~text
startThreadPool
  → 首线程 BC_ENTER_LOOPER
  → 驱动 BR_SPAWN_LOOPER
  → 新线程 BC_REGISTER_LOOPER
~~~

### 9.4 验证同步与 oneway 等待边界

~~~bash
sed -n '650,720p' \
  frameworks/native/libs/binder/IPCThreadState.cpp
sed -n '832,875p' \
  frameworks/native/libs/binder/IPCThreadState.cpp
~~~

再看 r48 的顺序测试：

~~~bash
sed -n '928,955p' \
  frameworks/native/libs/binder/tests/binderLibTest.cpp
~~~

### 9.5 验证统计、Watchdog 与 binderfs 的边界

~~~bash
rg -n "callStarted|cpuTimeStarted|timeStarted|isCharging" \
  frameworks/base/core/java/com/android/internal/os/BinderCallsStats.java
rg -n "BinderThreadMonitor|blockUntilThreadAvailable" \
  frameworks/base/services/core/java/com/android/server/Watchdog.java \
  frameworks/native/libs/binder/IPCThreadState.cpp
sed -n '175,190p' system/core/rootdir/init.rc
sed -n '1538,1548p' \
  frameworks/native/cmds/dumpstate/dumpstate.cpp
~~~

完成后，用一张纸画出本案例的五个线程和四条等待边。能标出每条边是“等锁、等 worker、等同步 reply”中的哪一种，比记住 `BR_SPAWN_LOOPER` 的名字更重要。

## 10. 边界、常见翻车点、自测答案与行动清单

### 10.1 这套方法的边界

- 当前 AOSP 工作区没有设备内核的 `drivers/android/binder.c`；
  node 异步队列的具体实现要和目标设备内核源码、trace 格式一起核对。
- 厂商可能修改线程配置、服务实现、内核与 SELinux 权限。
- Perfetto 丢事件或缺少类别时，时间线可能不完整。
- Java BinderCallsStats 看不到 native Binder 服务的完整内部耗时。
- 采样统计适合找候选，不等于单次故障的因果证据。
- 线程栈是一个瞬间；竞争短暂时需要多次样本或连续 trace。
- 本案例是教学用复合场景，不能拿它替代目标设备的真实等待链。

### 10.2 六个最常见的翻车点

**翻车 1：看到 `transactNative` 就改 Binder 驱动。**

先沿 transaction 找服务端。`transactNative` 多数时候只是同步调用方的等待位置。

**翻车 2：把 31 当作启动时固定存在的 31 条线程。**

线程池按需求扩展；配置上限、实际线程数、正在执行数不是同一个量。

**翻车 3：认为 oneway 完全不会阻塞调用线程。**

它不等业务 reply，但仍有序列化、驱动提交、buffer 与调度成本。

**翻车 4：认为 oneway 返回等于服务端成功。**

服务端还可能排队，异常也不会沿同一事务返回。需要结果就设计 callback 或查询协议。

**翻车 5：线程池满就立刻加线程。**

如果所有线程等同一锁，新增线程只是新增等待者。

**翻车 6：只看平均耗时。**

池耗尽常由尾部等待、突发并发和跨进程环触发。还要看调用量、最大/分位延迟、队列滞后和线程状态。

### 10.3 自测题与答案

1. **调用方停在 `BinderProxy.transactNative`，能否断定驱动慢？**

   不能。它只说明同步 reply 尚未回来，根因可能在目标线程排队、服务端锁、Handler、I/O 或更下游 Binder。

2. **system_server 的 31 表示什么？**

   它是 r48 在 `SystemServer` 中设置的 Binder 池上限配置，通过 JNI 和 `BINDER_SET_MAX_THREADS` 交给 libbinder/驱动；不是启动时预建 31 条线程，也不是 31 份业务必然并行。

3. **`BR_SPAWN_LOOPER` 做什么？**

   驱动在需要补充池线程且符合边界时通知用户空间；libbinder 收到后调用 `spawnPooledThread(false)`。

4. **oneway 为什么还会调用 `waitForResponse(nullptr, nullptr)`？**

   发送侧仍需等待 `BR_TRANSACTION_COMPLETE` 完成提交；它不读取服务端业务 reply。

5. **同一个 node 的 oneway 为什么会越积越多？**

   异步事务在该 node 上有串行处理边界。若生产速度持续大于完成速度，后续事务只能排队。

6. **为什么直接 A→B→A 有时没有死锁，仍不能依赖 Binder 自动解环？**

   同步等待循环可以处理嵌套命令，但回调可能来自另一线程、经 Handler、经过第三进程或等待另一把锁；重入还可能破坏状态不变量。

7. **CPU 很低，为何 Binder 池仍可能耗尽？**

   线程可以全部睡眠在锁、futex、I/O 或远端 reply 上，“占着执行槽位”不等于“正在运行 CPU”。

8. **BinderCallsStats 能否直接给出锁 owner？**

   不能。它帮助定位 Java Binder 入口的调用量、CPU 与 elapsed 候选，owner 仍要靠栈、trace 和源码等待关系确认。

### 10.4 读完即可执行的诊断清单

遇到下一次 Binder 卡顿，按顺序做：

- [ ] 写下具体客户端线程、接口 descriptor 和 transaction/method；
- [ ] 标明它是同步还是 oneway；
- [ ] 分开记录客户端等待、驱动排队、服务端执行；
- [ ] 收集同一时间窗内 app、system_server 和下游服务栈；
- [ ] 给每个等待点找 owner，而不是停在第一处 `transactNative`；
- [ ] 检查服务端是否持锁做同步外部 Binder；
- [ ] 检查回调是否来自另一工作线程或经 Handler；
- [ ] 检查 oneway 的 node 边界、生产速率和业务背压；
- [ ] 用 Perfetto flow 验证跨进程方向与先后；
- [ ] 用 BinderCallsStats 找入口热点，但注明采样与充电状态；
- [ ] 用 bugreport/binderfs 快照补充驱动现场；
- [ ] 修复后用同一场景、同一统计口径验证，不编造提升数字；
- [ ] 最后才决定线程池配置是否需要调整。

真正值得带走的不是“Binder 有 31 条线程”这句话，而是这条判断链：

~~~text
谁在等待
  → 等的是队列、锁、Handler、I/O 还是同步 reply
  → 谁拥有解除等待的条件
  → 哪个协议或临界区让等待传播
  → 修复后用相同证据验证等待链已断开
~~~

只要能画出这张等待图，“Binder 慢”就不再是一个模糊结论，而会变成可以定位、修改和验证的工程问题。
