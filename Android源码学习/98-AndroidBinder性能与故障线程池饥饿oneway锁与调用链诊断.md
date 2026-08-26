# 98 Android Binder 性能与故障：线程池饥饿、同步/oneway 队列、锁与调用链诊断

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：将 Binder 驱动对象模型、Parcel/AIDL、system_server 线程模型组合成一套故障分析方法；能从“调用慢、卡住、死亡、大事务、回调积压”追到真正等待链，而不是只归因于 Binder。

---

## 1. “Binder 调用慢”至少包含五段

一次同步调用：

```text
客户端调用前工作
  → Proxy 写 Parcel
  → 驱动投递/排队
  → 服务端 Binder 线程执行 Stub 和业务
  → reply 序列化、驱动返回、客户端读 reply
```

若服务端再 post 到 Handler 并等待：

```text
client
  → server Binder thread
      → post server Handler
      → Binder thread 等 Handler
          → Handler 排队/执行业务
      ← signal
  ← reply
```

用户看到的总时长包含所有段。`BinderProxy.transactNative` 只是客户端当前停留点，不等于根因在内核。

---

## 2. 本章源码地图

```text
frameworks/native/libs/binder/IPCThreadState.cpp
frameworks/native/libs/binder/ProcessState.cpp
frameworks/native/libs/binder/BpBinder.cpp
frameworks/base/core/jni/android_util_Binder.cpp
frameworks/base/core/java/android/os/Binder.java
frameworks/base/core/java/android/os/BinderProxy.java
frameworks/base/core/java/com/android/internal/os/BinderCallsStats.java
frameworks/base/services/core/java/com/android/server/BinderCallsStatsService.java
frameworks/base/services/core/java/com/android/server/Watchdog.java
frameworks/base/core/java/android/os/TransactionTooLargeException.java
frameworks/base/core/java/android/os/DeadObjectException.java
```

当前 checkout 未必含匹配设备的 kernel Binder driver，内核细节应结合设备内核版本；本章以 AOSP Android 11 用户空间和 Binder UAPI 语义为准。

---

## 3. 同步事务的等待模型

AIDL 普通方法默认同步：

```text
Client thread --BC_TRANSACTION--> driver
Client thread 进入等待
Driver --BR_TRANSACTION--> Server Binder thread
Server 执行并 --BC_REPLY--> driver
Driver --BR_REPLY--> Client thread
Client 解 reply 后返回
```

同步调用的优点：

- 返回值与异常清楚；
- 顺序直观；
- 调用完成就是服务端此次方法已返回。

风险：

- 客户端线程被占用；
- 可形成跨进程等待环；
- 主线程调用慢服务直接卡 UI；
- 服务端线程池/Handler/锁问题向客户端传播。

---

## 4. oneway 的真实语义

oneway AIDL 设置 `FLAG_ONEWAY`：客户端把事务提交给驱动后不等待业务 reply。

```text
Client --oneway--> driver queue → Server Binder thread → method
Client <立即返回提交结果>
```

它不意味着：

- 服务端立即执行；
- 创建专用后台线程；
- 无限并发；
- 业务异常返回给客户端；
- 不占 Binder buffer/线程/CPU；
- 不会排队。

oneway 解决“客户端是否同步等待”，没有消除服务端工作。

---

## 5. oneway 为什么会积压

同一目标 Binder node 的异步事务需要维持序列化/顺序语义。若每个回调耗时 100ms，生产速度又高于消费速度：

```text
oneway queue: [1][2][3][4][5]...[1000]
server:          每 100ms 消费一个
```

客户端调用看似快速成功，但：

- 状态更新越来越迟；
- Binder buffer 压力增大；
- 旧事件可能在新状态后很久才到；
- 其他事务可能被资源竞争影响；
- 新内核可能报告 one-way spam，但 Android 11 设备能力依内核版本而异。

回调接口要考虑合并、去重、限流、只传最新状态，而非把每个高频采样都当可靠消息队列。

---

## 6. oneway 的顺序不要过度外推

可依赖的是 Binder 为特定异步事务流提供的顺序约束；不要推导成：

- 不同 Binder 对象之间全局有序；
- oneway 与同步事务跨对象严格按客户端源码顺序完成；
- post 到两个不同 Handler 后仍保持 Binder 接收顺序；
- 多客户端发送顺序可合成唯一全局顺序。

若业务要求版本一致性，消息应携带 sequence/generation，并在消费端丢弃过期事件。

---

## 7. Binder 线程池如何增长

native Binder 线程加入池时向驱动发送：

```text
BC_ENTER_LOOPER / BC_REGISTER_LOOPER
```

驱动发现线程不足时可返回：

```text
BR_SPAWN_LOOPER
```

ProcessState 再按配置生成 Binder 线程，直到上限。system_server Android 11 将最大线程配置提高为 31；普通 native 进程常见默认 15。

上限不是并发能力保证：线程可能全在等待同一把锁，增加数量只会增加等待者。

---

## 8. 线程池饥饿的三类根因

### 业务阻塞

Binder 方法直接做磁盘、HAL、网络或长计算。

### 锁竞争

所有线程进入服务后等待同一个全局锁。

### 嵌套同步 IPC

Binder 线程向外同步调用，对端又慢、池耗尽或反向调用。

典型 dump：

```text
Binder:system_1  waiting on mLock
Binder:system_2  waiting on mLock
...
Binder:system_31 BinderProxy.transactNative → vendor process
```

真正要找的是持锁者/最末端远端，不是把上限从 31 改成更大。

---

## 9. Watchdog 的 BinderThreadMonitor

system_server Watchdog Monitor 调用：

```java
Binder.blockUntilThreadAvailable();
```

其 native 目标是等待 Binder 线程可用。若线程池长期被占满，这个 Monitor 不能返回，foreground HandlerChecker 会超时。

它证明“system_server 已无法及时接收入站 IPC”，但不会自动指出哪个事务占满线程。仍需分析所有 Binder 线程栈和依赖图。

---

## 10. 同步 Binder 环形死锁

```text
Process A thread 1 --sync--> Process B thread 1
Process B thread 1 --sync--> Process A
```

Binder 支持嵌套事务，并可能让原等待线程处理回入事务，从而缓解某些简单重入。但不能依赖它解决所有环：

- 回调等待 A 中另一 Handler；
- A 持锁发起 B 调用；
- B 回调 A 需要同一锁；
- 多进程、多线程、多锁形成复杂环；
- 线程池其他线程也已耗尽。

最安全原则仍是：不持关键锁做外部同步 IPC。

---

## 11. 锁内 IPC 的标准等待图

```text
A Binder thread
  lock(A.mLock)
  → sync call B

B Binder thread
  → callback A

A callback Binder thread
  → wait A.mLock

原 A thread 等 B reply；B 等 callback；callback 等 A.mLock
```

修复通常不是加 timeout，而是：

- 锁内复制状态/回调列表；
- 锁外 IPC；
- 回来后用 generation 验证状态未过期；
- 必要时拆分事务为异步协议。

---

## 12. Handler 二次排队

许多服务为保护状态机：Binder 入站后 post 到 Handler。异步 API 可立即返回；同步 API 有时用 latch 等结果。

耗时拆分：

```text
Ttotal = Binder driver queue
       + Stub/permission
       + Handler delivery latency
       + Handler dispatch duration
       + reply serialization
```

如果 BinderCallsStats 显示方法 wall time 长，CPU time 很短，可能大部分时间在等 Handler/锁/远端，而不是 Binder 方法 CPU 执行。

---

## 13. 客户端主线程同步 Binder

客户端主线程调用：

```text
UI main → BinderProxy.transact → wait service
```

可能造成卡顿甚至应用 ANR。Framework Manager 经常隐藏 Binder 细节，API 看似普通 Java getter，也可能跨进程。

排查 API 前应确认：

- Manager 是否缓存本地值；
- 方法是否 AIDL；
- 是否允许主线程调用；
- 服务是否可能 I/O/等待；
- callback/async API 是否更合适。

不能因方法名是 `getX()` 就假设 O(1) 本地读取。

---

## 14. `warnOnBlocking` 与 `allowBlocking`

Framework 可启用 Binder Proxy 阻塞调用警告，帮助发现不应同步调用的 Binder。某些已知服务引用通过：

```java
Binder.allowBlocking(binder)
```

允许阻塞调用。

这只是诊断策略标记，不会把同步调用改成异步，也不会保证服务快速。看到 `allowBlocking` 应理解为“调用者明确接受这里可能阻塞”，不是性能豁免证书。

---

## 15. BinderCallsStats 观测什么

`BinderCallsStatsService` 发布：

```text
binder_calls_stats
```

并可通过 `Binder.setObserver()` 安装 `BinderCallsStats`，在 Java Binder 入站事务周围采样/记账。

典型维度包括：

- calling/work-source UID；
- Binder class 与 transaction code/method；
- call count、recorded count；
- CPU time；
- latency/wall time；
- request/reply size（配置相关）；
- exception count；
- screen interactive 状态等。

具体输出与设置、采样率、detailed tracking 有关，不要把未采样条目当作从未调用。

---

## 16. WorkSource UID 为什么需要授权

调用者可携带 work-source attribution，但不能相信任意 UID 声称“耗时算给别人”。`AuthorizedWorkSourceProvider`：

- 默认归因真实 calling UID；
- 只有白名单 appId（system_server 自身、持特定系统权限包）可设置可信 work source；
- userId 与 appId 要区分。

因此 Binder 统计既是性能数据，也涉及资源归因安全。

---

## 17. BinderCallsStats 的成本

详细追踪可能记录 CPU、latency、Parcel size 和调用 UID，带来：

- 每事务时间读取；
- map 查找与锁；
- 内存条目；
- dump/导出开销。

Android 11 支持 sampling interval、最大条目、detailed tracking 开关。诊断时提高精度，测量本身也可能扰动系统；应记录配置并在完成后恢复。

---

## 18. 统计数据不能直接给出等待 owner

BinderCallsStats 能告诉你：

```text
某 UID 调某 transaction 很多/很慢
```

但仅靠聚合无法区分：

- 等服务内部锁；
- 等 Handler；
- 等下游 Binder/HAL；
- 服务自己 CPU 热点；
- GC/调度延迟。

下一步必须用 Perfetto Binder flow、线程栈、锁 owner 和服务源码还原单次调用链。

---

## 19. transaction code 如何映射方法名

AIDL Stub 常定义：

```java
static final int TRANSACTION_doWork = FIRST_CALL_TRANSACTION + N;
```

并可实现 `getDefaultTransactionName(code)`。诊断输出只有 code 时：

1. 找接口 Stub。
2. 搜 `TRANSACTION_` 常量。
3. 看 onTransact case。
4. 对照参数反序列化和目标方法。

native AIDL/HIDL 需到对应生成后端代码/接口定义映射，不能把不同 descriptor 下相同 code 当同一方法。

---

## 20. TransactionTooLargeException 的边界

Android 11 r48 的 Java Binder JNI 对 `FAILED_TRANSACTION` 使用启发式映射：当该调用允许抛
`RemoteException` 且本次 data Parcel 大于 `200 * 1024` 字节时，抛
`TransactionTooLargeException`；较小 Parcel 则通常按“远端可能已死”映射为
`DeadObjectException`（不允许抛受检异常的入口会变成 RuntimeException）。源码注释同时明确：
事务过大是 `FAILED_TRANSACTION` 最常见的原因，但驱动还可能因 malformed transaction、已关闭
FD 等原因给出同类失败，所以这个异常仍是启发式诊断，不是驱动返回的精确失败分类。

所以该异常表示“本次 data Parcel 已越过 r48 JNI 的 200 KiB 启发式门槛”，也是大事务/
transaction buffer 压力的强线索，但仍不是“刚好越过固定 Binder 上限”的证明；排查还应考虑：

- request 大；
- reply 大；
- 进程共享 Binder buffer 中并发未完成事务多；
- 大量 oneway 排队占用 buffer；
- FD/Binder 对象数组与偏移开销；
- 驱动其他失败状态被粗粒度映射。

不要杜撰一个适用于所有设备/版本/并发状态的“刚好固定最大单 Parcel 字节数”。

---

## 21. request 大还是 reply 大

异常可能在调用发出或读取回复阶段暴露。应查看接口：

- 入参是否 Bundle、Intent extras、大 List、Bitmap/byte[]；
- out/return 是否返回大集合；
- 服务是否把数据库全量结果一次返回；
- 是否在 Activity lifecycle state 保存大对象。

解决：分页、流式接口、共享内存/FD、ContentProvider、文件、只传标识符后按需读取。

“捕获异常重试同样大数据”通常只会再次失败并增加压力。

---

## 22. Binder buffer 是进程共享压力

应用文档常用约 1MB 解释 Binder transaction buffer，但重要语义是：buffer 空间由进程中在途事务共享，而不是每个调用独享一个永远固定的完整额度。

因此：

```text
单次 700KB 可能失败
多个并发 200KB 也可能失败
同样 payload 在不同瞬间结果可能不同
```

设计 IPC 时应远低于极限，不能以实验“这台设备偶尔能过”作为协议保证。

---

## 23. DeadObjectException

表示目标 Binder 所在进程/对象已死亡或连接不可用。它回答的是存活性，不解释死亡原因。

客户端恢复链：

```text
DeadObject/death recipient
  → 使本地代理/session/callback 状态失效
  → 避免在 Binder 回调线程做重恢复
  → 重新查询/绑定服务
  → 重新注册 callback
  → 使用 generation 防旧回调污染
```

不要对每个 DeadObject 立即无限 tight-loop 重试；服务可能在 crash loop，重连需要退避和系统生命周期信号。

---

## 24. `linkToDeath` 的竞态

可能发生：

- 调用前服务已死；
- `linkToDeath` 时已死；
- 注册成功后立即死亡；
- 死亡回调和新服务注册交错；
- 旧 death recipient 晚到，误清理新连接。

使用连接 generation：

```text
connect generation=7
death callback captures 7
later reconnect generation=8
old death(7) arrives → 不清理 generation 8
```

死亡通知是边沿信号，不是完整状态同步；重连后仍需重新拉取快照。

---

## 25. RemoteException 的错误分层

客户端看到 RemoteException 可能来自：

- Binder transport/目标死亡；
- 服务端写入远程异常协议；
- AIDL wrapper 转换；
- transaction failure。

Framework Java Manager 常调用：

```java
throw e.rethrowFromSystemServer();
```

转换为 RuntimeException。排查不能只看客户端最终异常类，要找原始 cause、服务端日志和 transaction。

业务错误应尽量用明确返回/typed exception，而不是让所有失败都表现为 RemoteException。

---

## 26. 服务端异常与 oneway

同步调用可把受支持异常写入 reply，客户端读出。oneway 没有 reply，服务端异常不能按同步方式返回客户端。

因此 oneway 权限检查失败可能只在服务端日志可见，客户端“调用返回”并不证明业务成功。

需要确认完成时应设计：

- 单独 callback 带 requestId；
- 状态查询；
- 结果事件；
- 有界超时/取消；
- 幂等重试。

---

## 27. 高调用频率与单次慢是不同问题

```text
接口 A：每次 20ms，每秒 1 次
接口 B：每次 0.2ms，每秒 5000 次
```

B 每秒总 CPU 可能更高。诊断同时看：

- call count；
- recorded/sample count；
- total CPU/latency；
- max latency；
- per-call average；
- payload size；
- 调用方 UID 分布。

优化热点不只按“平均最慢”排序。

---

## 28. 批处理的收益与风险

把 100 次小 IPC 合并为 1 次可减少：

- syscall/transaction 固定开销；
- Parcel header；
- 上下文切换；
- 服务端锁进入次数。

但批过大又会：

- 增加 TTLE 风险；
- 单次 Binder 线程占用更久；
- 提高尾延迟；
- 失败时重试成本更大。

需要有界 batch、分页和背压，而不是“越大越好”。

---

## 29. dump 调用也会占 Binder 资源

`dumpsys` 通常通过 Binder 请求服务 dump。若 dump：

- 持服务主锁遍历大状态；
- 在锁内调用远端；
- 写大量文本而 pipe 消费慢；
- 执行同步 I/O；

会占用 Binder 线程并影响业务。

优秀 dump 应复制快照后锁外输出、支持参数缩小范围、避免触发副作用，并处理 fd 写阻塞。

---

## 30. Binder 优先级继承不是万能药

Binder 驱动/运行时可参与调度优先级传播，system_server 又禁用后台降级，但它不能解决：

- 锁 owner 是低优先级非 Binder 线程；
- 等 I/O；
- Binder 环死锁；
- oneway 队列积压；
- 服务端算法慢；
- CPU 热降频。

优先级只影响 Runnable 线程调度，不创造缺失的 reply 或释放锁。

---

## 31. Perfetto 的 Binder 调用链

合适配置下可看到客户端 transaction、目标线程、reply/flow。步骤：

1. 定位客户端长 `binder transaction`。
2. 沿 flow 到服务端 Binder thread。
3. 看服务端何时 runnable/running。
4. 若服务端又发 Binder，继续沿 flow。
5. 若 post Handler，找对应 trace/message。
6. 若 blocked，结合 stack 找 lock owner。
7. 沿依赖直到最后一个真正执行/等待外部资源的节点。

这比只截客户端 ANR 栈更接近根因。

---

## 32. 没有 Perfetto 时如何画等待图

收集同一时刻：

- 客户端 traces；
- system_server traces；
- 目标 native/HAL 进程 traces；
- BinderCallsStats/dumpsys；
- logcat transaction/slow-call；
- kernel binder state（设备允许时）。

手动画：

```text
App main
 └─ waits IPackageManager.foo
      SystemServer Binder #8
       └─ waits mPackages lock
            owner: PackageManager main
             └─ waits installd Binder
                  installd worker
                   └─ blocked disk I/O
```

最后一段 disk I/O 才是根因候选。

---

## 33. BinderCallsStats 常用思路

有权限的调试设备可观察：

```bash
adb shell dumpsys binder_calls_stats
adb shell dumpsys binder_calls_stats --help
```

具体参数以该 Android 11 checkout/设备 `--help` 为准，因为不同版本开关会变化。

建议流程：

1. reset。
2. 控制时间窗口复现。
3. 导出调用统计。
4. 找总 CPU、总 latency、次数、异常/大 Parcel 线索。
5. 映射 transaction code。
6. 对候选接口采 Perfetto/stack。
7. 恢复 detailed/sampling 配置。

---

## 34. 常见故障症状对照

| 症状 | 首要怀疑 | 关键证据 |
|---|---|---|
| 单个同步调用偶发很慢 | 锁、下游 IPC、I/O、调度 | 单次 Perfetto + stacks |
| 大量服务一起无响应 | system_server Binder 池/全局锁 | 所有 Binder 线程栈 |
| oneway 客户端快但状态迟到 | 异步队列/Handler 积压 | transaction/queue 时间线 |
| `TransactionTooLargeException` | request/reply/共享 buffer 压力 | payload、并发在途事务 |
| `DeadObjectException` | 目标进程死亡/重启 | death log、tombstone、service re-register |
| CPU 高但单次不慢 | 高频小 IPC | call count + total CPU |
| Watchdog Binder monitor | 无可用 system_server Binder thread | Binder thread dump/等待图 |

---

## 35. 修复线程池饥饿的优先顺序

1. 找到占用线程的共同栈/等待点。
2. 找真正 lock owner 或下游服务。
3. 缩短 Binder 入站同步工作。
4. 锁外做外部 IPC。
5. 对可异步工作 post，但保留协议语义。
6. 拆分大锁/使用不可变快照。
7. 限制高频调用和 oneway 生产速率。
8. 最后才评估线程上限。

单纯扩池常把“31 个线程卡住”变成“63 个线程卡住”，还扩大内存和竞争。

---

## 36. 接口设计阶段的性能检查表

- 方法必须同步返回吗？
- 是否能 callback/requestId？
- 高频状态能共享内存/监听变化而非轮询吗？
- List 是否分页、有最大数量吗？
- Parcel 是否可能含 Bitmap/大 byte[]/嵌套 Bundle？
- oneway 是否有背压/合并？
- 服务端是否持锁外调？
- 回调死亡如何清理？
- 调用方 UID/WorkSource 如何归因？
- 是否能取消过期请求？
- dump 是否复制快照并限量？

IPC 性能首先是协议设计问题，其次才是微优化序列化代码。

---

## 37. 常见误区纠正

### 误区 1：栈停在 transactNative，Binder 驱动就是根因

错误。客户端只是等待，根因常在服务端或更下游。

### 误区 2：oneway 不会阻塞任何线程

错误。客户端不等 reply，服务端仍执行并可能积压。

### 误区 3：线程池耗尽就把上限调大

错误。先找线程为何不归还。

### 误区 4：TTLE 证明单个 Parcel 恰好超过固定 1MB

错误。`FAILED_TRANSACTION` 映射较粗，buffer 还是进程共享压力。

### 误区 5：DeadObjectException 是网络超时

错误。它表示 Binder 目标死亡/不可用，不解释原因。

### 误区 6：BinderCallsStats 能直接显示锁 owner

错误。它是聚合入口数据，需结合 trace/stack。

### 误区 7：平均延迟最低的接口无需优化

错误。高频接口总 CPU 可能最大。

### 误区 8：allowBlocking 会让调用更快

错误。只改变阻塞诊断许可。

### 误区 9：catch RemoteException 就完成恢复

错误。还需清状态、重查服务、重注册 callback 和防旧事件。

### 误区 10：dump 与业务无关

错误。dump 也可能占 Binder 线程与服务锁。

---

## 38. 第二遍复读：最易混的五种“慢”

### 38.1 客户端等待慢

只说明端到端同步方法未返回，不证明服务端一直执行 CPU。

### 38.2 驱动排队慢

可能是目标线程池无空闲、目标调度迟、oneway 前序积压，而非驱动算法本身慢。

### 38.3 服务端执行慢

应继续拆 CPU、锁、I/O、Handler wait、下游 IPC。

### 38.4 高频累计慢

单次很快但调用量巨大，主要消耗来自固定 IPC/Parcel/锁开销。

### 38.5 恢复慢

服务死亡后重连、重新注册和状态重放不完整，会表现为功能长期不可用，和单次 transaction latency 是不同问题。

---

## 39. Mac 只读源码练习

### 练习 1：追同步与 oneway 生成差异

```bash
rg -n "FLAG_ONEWAY|transact\(" \
  out/soong/.intermediates 2>/dev/null | head
rg -n "oneway" frameworks/base -g '*.aidl' | head -30
```

若没有生成目录，就用第 92 章方法阅读 AIDL generator/已有 Java Stub。

### 练习 2：追线程池协议

```bash
rg -n "BR_SPAWN_LOOPER|BC_ENTER_LOOPER|BC_REGISTER_LOOPER|joinThreadPool" \
  frameworks/native/libs/binder/IPCThreadState.cpp \
  frameworks/native/libs/binder/ProcessState.cpp
```

说明驱动请求与用户空间建线程如何衔接。

### 练习 3：检查 FAILED_TRANSACTION 映射

```bash
sed -n '830,890p' frameworks/base/core/jni/android_util_Binder.cpp
```

回答为什么不能把每次 TransactionTooLargeException 都当精确尺寸证明。

### 练习 4：阅读 BinderCallsStats 安装

```bash
rg -n "setObserver|setProxyTransactListener|setSamplingInterval|setDetailedTracking" \
  frameworks/base/services/core/java/com/android/server/BinderCallsStatsService.java \
  frameworks/base/core/java/com/android/internal/os/BinderCallsStats.java
```

区分统计启用、采样、WorkSource 与详细追踪。

### 练习 5：画一次真实等待链

任选 Framework Manager API：

```text
Manager → AIDL Proxy → target Stub → permission → lock/Handler
        → downstream Binder → reply → client
```

为每段标线程、是否持锁、是否同步、可能错误。

---

## 40. 自测题

1. 同步 Binder 总时长包含哪些阶段？
2. oneway 客户端返回是否代表服务端已执行？
3. BR_SPAWN_LOOPER 起什么作用？
4. Binder 池耗尽为何 CPU 可能很低？
5. Watchdog 如何检测 system_server Binder 可用性？
6. 为什么不应持服务锁做远端回调？
7. Handler 二次排队怎样进入同步调用时长？
8. BinderCallsStats 的 CPU 与 latency 有何区别？
9. TTLE 为什么不是精确的单 Parcel 尺寸证明？
10. DeadObject 后为什么要 generation？
11. 高频快调用为何仍可能是热点？
12. 为什么扩 Binder 池不是首选修复？

---

## 41. 参考答案

1. 客户端准备/序列化、驱动排队、服务端线程/业务、reply 序列化和返回。
2. 不代表，只说明异步事务已提交，服务端可能仍排队。
3. 驱动通知用户空间 Binder 线程不足，可按上限生成新 looper 线程。
4. 线程可能全在等锁、IPC、I/O 或条件而不占 CPU。
5. BinderThreadMonitor 调用 `Binder.blockUntilThreadAvailable()`。
6. 远端可能慢或反向调用，形成长持锁和死锁环。
7. Binder 线程 post 后等待，Handler delivery/dispatch 都包含在端到端时间。
8. CPU 是实际处理消耗；latency/wall 包含等待与调度。
9. JNI 从较粗的 FAILED_TRANSACTION 映射，且进程 buffer 由在途事务共享。
10. 防旧连接 death/callback 晚到后清理或污染新连接。
11. 固定 IPC 开销乘以巨大调用次数会产生高总 CPU/调度成本。
12. 它不消除共同阻塞点，可能扩大竞争和资源占用。

---

## 42. 本章总结

```text
“Binder 慢”
  → 先定同步/oneway、request/reply 和目标 descriptor/code
  → 找客户端等待线程
  → 沿驱动 flow 到服务端 Binder thread
  → 拆 permission、lock、Handler、I/O、下游 IPC
  → 找等待链末端 owner
  → 用 BinderCallsStats 看频率/累计热点
  → 用 Parcel/并发证据判断大事务
  → 用 death + generation 设计恢复
```

Binder 是运输与调度机制。真正的性能和可靠性往往取决于接口粒度、线程模型、锁边界、背压和恢复协议。

---

## 43. 下一章预告

第 99 章将学习：

**Android 启动可观测性：bootstat、EventLog、statsd、sys.boot_completed、logcat 与 Perfetto**

重点是给各种“开机完成”指标建立准确起止点，并将属性、事件、统计 Atom、trace 与用户可交互时刻对应起来。
