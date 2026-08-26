# 96 Android SystemServer 启动性能：TimingsTrace、InitThreadPool、依赖与卡顿排查

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：不靠“感觉启动慢”，而是用关键路径、trace slice、耗时日志、Future 汇合点和线程等待关系定位 system_server 启动瓶颈。  
> 环境说明：本章以 macOS 只读源码分析为主，不要求编译 AOSP；有设备时可选用 adb/Perfetto 验证。

---

## 1. 启动性能的核心不是“总工作量”，而是关键路径

假设开机要做三个任务：

```text
A = 500 ms
B = 800 ms
C = 300 ms
```

若全部串行：总时间约 1600ms。若 A、B 并行，但 C 必须等两者：

```text
主线程：A ─────────┐
后台池：B ──────────────┐
                     join ├─ C
```

总时间接近 `max(A,B)+C=1100ms`，而不是三个时长相加。

因此启动分析要找：

- system_server 主线程上的串行工作；
- 并行任务的最长分支；
- 主线程真正等待 Future/latch 的位置；
- 后续 phase 必须等待的依赖；
- Binder、锁、I/O 造成的不可见等待。

这条决定最终时间的最长依赖链就是**关键路径**。

---

## 2. 先区分四类“启动时间”

日常说“Android 启动用了 20 秒”可能指：

| 时间区间 | 起止点示例 | 主要负责人 |
|---|---|---|
| bootloader/kernel | 上电 → init | Bootloader、kernel、驱动 |
| userspace boot | init → framework ready | init、native daemon、Zygote、SystemServer |
| system_server init | SystemServer.run → services ready | 本章重点 |
| 用户可交互 | 启动动画/Home/解锁完成 | AMS、WMS、SystemUI、Launcher、用户状态 |

优化前必须声明测量边界。`SystemServerTiming` 只能解释 system_server 中被打点的区间，不能代表完整上电时间。

---

## 3. 本章源码地图

```text
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/services/core/java/com/android/server/SystemServiceManager.java
frameworks/base/services/core/java/com/android/server/SystemServerInitThreadPool.java
frameworks/base/services/core/java/com/android/server/utils/TimingsTraceAndSlog.java
frameworks/base/core/java/android/util/TimingsTraceLog.java
frameworks/base/core/java/com/android/internal/util/ConcurrentUtils.java
frameworks/base/core/java/android/os/Trace.java
frameworks/base/services/core/java/com/android/server/Watchdog.java
```

可选观察命令涉及：

```text
adb logcat
atrace / Perfetto
dumpsys activity processes
dumpsys binder_calls_stats
```

---

## 4. SystemServer 主线程是一条大串行链

`SystemServer.run()` 准备主 Looper、创建 system Context 和 SSM 后调用：

```java
startBootstrapServices(t);
startCoreServices(t);
startOtherServices(t);
```

这些方法中的绝大多数普通语句都在 system_server 主线程顺序执行：

```text
StartInstaller
 → StartActivityManager
 → StartPowerManager
 → StartDisplayManager
 → WaitForDisplay
 → StartPackageManager
 → ...
```

某个步骤慢 500ms，若它位于主线程关键路径，通常直接增加至少约 500ms。只有显式提交到其他线程的工作才可能与主线程重叠。

---

## 5. TimingsTraceAndSlog 是什么

SystemServer 创建计时器：

```java
TimingsTraceAndSlog t = new TimingsTraceAndSlog();
```

使用方式：

```java
t.traceBegin("StartPowerManager");
mSystemServiceManager.startService(PowerManagerService.class);
t.traceEnd();
```

它同时做两件事：

1. 调用 `Trace.traceBegin/traceEnd`，产生 `TRACE_TAG_SYSTEM_SERVER` slice。
2. 在允许的构建类型中记录嵌套开始时间并输出 duration 日志。

名字 `AndSlog` 表示它还会在 begin 时向 logcat 写阶段名。

---

## 6. trace slice 与耗时日志不是一回事

`TimingsTraceLog.traceBegin()` 先执行：

```java
Trace.traceBegin(mTraceTag, name);
```

然后 Android 11 中只有非 user build 的 `DEBUG_BOOT_TIME` 才保存嵌套开始时间，用于 `traceEnd()` 时计算并 `Slog.d`：

```text
StartPowerManager took to complete: 12ms
```

因此：

- Perfetto/atrace slice 是否可见，取决于 trace tag 是否启用和采集配置；
- logcat duration 是否输出，还受 build type、日志级别影响；
- 没有 duration 日志，不代表代码没有 trace 打点；
- begin 时的 `Slog.i` 与 end 时的 duration log 也不是同一条记录。

排查时不要只 `grep "took to complete"` 就断言某段没有打点。

---

## 7. TimingsTraceLog 为什么要求同线程

构造时记录：

```java
mThreadId = Thread.currentThread().getId();
```

每次 begin/end 都调用 `assertSameThread()`。如果从另一线程使用同一实例，会抛 IllegalStateException。

原因：

- begin/end 使用栈式嵌套；
- `Trace.traceEnd()` 关闭当前线程最近 slice；
- 开始时间数组也按单线程栈维护；
- 跨线程 begin/end 会破坏配对关系。

异步任务必须创建自己的：

```java
TimingsTraceAndSlog.newAsyncLog()
```

不能把 SystemServer 主线程的 `t` 捕获到线程池里继续使用。

---

## 8. 嵌套深度与 user build 边界

非 user build 默认最多保存 10 层嵌套计时：

```java
MAX_NESTED_CALLS = 10;
```

超过后会警告“不再记录该层 duration”。这不一定阻止底层 Trace API 的 slice 形成，因为 `Trace.traceBegin()` 在深度检查之前已经调用。

user build 中用于 Java duration 计算的数组可以不创建，降低启动时调试开销。性能观测功能本身也有成本，所以量产版本往往减少详细日志。

---

## 9. `traceEnd()` 必须放 finally

推荐：

```java
t.traceBegin("StartDemo");
try {
    startDemo();
} finally {
    t.traceEnd();
}
```

如果异常路径漏掉 end：

- 后续 slice 嵌套结构错误；
- duration 名称与结束位置错配；
- `getUnfinishedTracesForDebug()` 会看到未完成项；
- Perfetto 时间线难以阅读。

打点的正确性也是源码质量的一部分。不能为了“有 trace”只加 begin 不保证成对退出。

---

## 10. SystemServiceManager 的第二层计时

SystemServer 外层常有：

```text
StartPowerManager
```

SSM 内部又有：

```text
StartService com.android.server.power.PowerManagerService
```

并测量 `onStart()`：超过 50ms 打印：

```text
Service ... took N ms in onStart
```

三者的范围不同：

| 指标 | 包含范围 |
|---|---|
| SystemServer 外层 slice | 调用点包住的所有代码 |
| SSM `StartService` trace | 反射构造、加入列表、onStart |
| 50ms warning | 只测 onStart 或单个生命周期回调 |

外层慢、onStart 不慢时，可能慢在类加载、调用前后代码或额外初始化；反之应进入服务 `onStart()` 深挖。

---

## 11. 50ms warning 不是硬超时

`SystemServiceManager`：

```java
if (duration > 50) {
    Slog.w(TAG, "Service ... took ...");
}
```

它不会：

- 中断回调；
- 回滚服务；
- 自动迁移后台；
- 立即触发 Watchdog；
- 证明有 bug。

它是“值得调查”的阈值。第一次初始化确实可能超过 50ms，但应解释时间花在哪里、是否位于关键路径、能否缓存/并行/延后。

---

## 12. elapsedRealtime、uptime 与 CPU time

本章源码主要使用：

```java
SystemClock.elapsedRealtime()
SystemClock.uptimeMillis()
```

区别：

- elapsedRealtime 包含设备深度睡眠时间；
- uptime 不包含深度睡眠；
- 两者都是单调时间，不受用户改墙钟影响；
- 它们都不是线程 CPU time。

system_server 启动时通常不会进入长时间深睡眠，两者数值常接近，但语义仍不同。某步骤 wall duration 500ms 可能是 CPU 执行、锁等待、Binder 等待或 I/O 阻塞，不能仅凭数字判定 CPU 热点。

---

## 13. InitThreadPool 如何创建

```java
int size = Runtime.getRuntime().availableProcessors();
mService = ConcurrentUtils.newFixedThreadPool(
        size,
        "system-server-init-thread",
        Process.THREAD_PRIORITY_FOREGROUND);
```

特点：

- 固定线程数等于可用处理器数量；
- 线程名便于 trace/stack 识别；
- 前台调度优先级；
- 只服务 system_server 启动期；
- PHASE_BOOT_COMPLETED 后关闭。

线程数等于 CPU 数不代表可以无代价提交无限 CPU 重活。多个前台优先级任务可能与主线程、SurfaceFlinger 等争抢 CPU 和内存带宽。

---

## 14. submit 做了哪些额外工作

每个任务需要 description：

```java
SystemServerInitThreadPool.submit(runnable, "StartSensorService");
```

内部：

1. 检查 pool 已启动且未 shutdown。
2. 将 description 加入 `mPendingTasks`。
3. 在线程池执行时创建 async timing log。
4. 产生 `InitThreadPoolExec:<description>` trace。
5. debug build 记录开始/完成日志。
6. 正常完成后从 pending 列表移除。
7. RuntimeException 记录并放进 Future。

description 不只是好看的文字；pool 关闭超时时会用 pending list 报告未完成任务。

---

## 15. 异步任务失败不一定立即让主线程知道

线程池任务抛 RuntimeException 后，异常被 Future 保存。提交线程不会在 submit 时同步收到。

```text
main submit(task) → 立即拿 Future 继续
worker task throws → Future 标记 failed
main 若从不 get/wait → 可能只看到 worker 日志
```

只有在正确依赖点调用类似：

```java
ConcurrentUtils.waitForFutureNoInterrupt(future, description);
```

主线程才会观察完成与异常。

所以性能和正确性问题是同一件事：并行任务必须明确“谁拥有 Future、何时汇合、失败如何传播”。

---

## 16. Android 11 的几个真实并行任务

SystemServer 包括：

- `SystemConfig::getInstance`；
- SensorService 启动；
- secondary Zygote preload；
- BlobStore service 启动；
- WebView preparation；
- 某些 HIDL/native 服务初始化。

并非所有 submit 都保留 Future。有些任务只要求最终在 boot complete 前结束；有些有明确早期消费者，必须保留并等待。

读每个 submit 时固定问：

```text
任务产物是什么？
第一个消费者是谁？
消费者前有没有 wait？
若任务失败，系统能否继续？
若一直不结束，shutdown 会怎样？
```

---

## 17. SensorService 的提交与汇合

早期提交：

```java
mSensorServiceStart = SystemServerInitThreadPool.submit(() -> {
    startSensorService();
}, START_SENSOR_SERVICE);
```

主线程继续启动无依赖工作。到 WindowManager/Input 等需要底层传感器服务的关键位置前：

```java
ConcurrentUtils.waitForFutureNoInterrupt(
        mSensorServiceStart, START_SENSOR_SERVICE);
```

图示：

```text
main:   submit Sensor ── other work ── wait ── WMS/Input next
worker:        └──────── startSensor ── done
```

若 worker 在 main 到达 wait 前完成，等待几乎为零；若没完成，剩余时长进入主线程关键路径。

---

## 18. WebView prepare 为什么要等到三方应用前

WebView preparation 可以与部分 systemReady 工作并行，但在：

```java
PHASE_THIRD_PARTY_APPS_CAN_START
```

之前必须等待完成，因为三方应用一旦启动，就可能立刻使用 WebView。

正确优化不是简单删掉 wait，而是：

- 尽可能早提交；
- 与无依赖工作重叠；
- 保留“开放三方应用”之前的必要汇合。

删除安全屏障可能让 trace 看起来更快，却把启动竞态转成运行期崩溃。

---

## 19. Zygote preload 的依赖链

secondary Zygote preload 可异步执行；WebView prepare 内又可能等待它。

```text
secondary zygote preload
          ↓
WebView preparation
          ↓
third-party apps can start
```

即使三个任务运行在不同位置，它们仍形成依赖链。Perfetto 中不能只看主线程空不空，还要沿 Flow/Future/日志把 worker 任务与主线程 join 连接起来。

---

## 20. Pool shutdown 是最后一道完整性检查

PHASE_BOOT_COMPLETED 分发完成后 SSM 调用：

```java
SystemServerInitThreadPool.shutdown();
```

shutdown：

1. 禁止新任务。
2. `ExecutorService.shutdown()`。
3. 最多等待 20 秒。
4. 被中断或超时则抓 system_server 和重要 native 进程堆栈。
5. `shutdownNow()` 取回未开始任务。
6. 若仍未正常结束，抛 IllegalStateException 并列出 unfinished descriptions。

它不是“boot complete 后悄悄丢弃后台任务”。相反，它要求启动任务必须收敛，否则把问题升级为严重启动错误。

---

## 21. 为什么不能在 boot completed 后继续 submit

pool 的生命周期就是启动期。关闭后 `sInstance=null`，再次 submit 会因 precondition 失败。

运行期工作应使用服务自己的 Handler/Executor 或系统共享线程，而不是依赖 init pool。否则：

- 生命周期不清楚；
- boot complete 关闭时可能竞态；
- pending task 会阻塞 shutdown；
- 调试人员误把常驻任务当启动未完成。

---

## 22. 并行化的四个必要条件

一段启动工作适合并行，至少要满足：

1. 不依赖主线程后续马上产生的数据。
2. 不会与主线程并发修改无保护的共享状态。
3. 有明确的第一个消费者与完成屏障。
4. 异常能在合适位置被观察和处理。

还要考虑：

- 是否触发 class initialization 锁竞争；
- 是否同步 Binder 回调主线程；
- 是否同时进行大量磁盘随机 I/O；
- 是否争抢 PackageManager 全局锁；
- 是否真的减少关键路径，而不是把工作换到另一线程后仍立刻 wait。

---

## 23. “submit 后马上 wait”通常没有并行收益

```java
Future<?> f = pool.submit(task);
waitForFutureNoInterrupt(f);
```

如果中间没有主线程可重叠工作：

- 增加线程调度和 Future 开销；
- 总时间几乎不变；
- trace 更分散；
- 调试更复杂。

并行优化需要扩大：

```text
submit ─────────────── first consumer / join
```

之间的独立工作窗口，同时不能越过真实依赖。

---

## 24. 常见瓶颈一：磁盘 I/O

SystemServer 启动可能读取：

- package/settings XML；
- system config；
- user/service 状态文件；
-数据库和 journal；
- dex/oat/profile 元数据；
- sysfs/procfs 节点。

症状：

- slice wall time 长，但 CPU running 时间低；
- 线程处于 uninterruptible sleep 或文件系统等待；
- 首次启动/升级明显比稳定重启慢；
- 多个并行任务同时读盘后反而变慢。

优化方向：减少同步读取、合并小文件、缓存稳定配置、延迟非关键数据、避免在关键路径 fsync；但涉及持久化正确性时不能为了速度删除安全写入。

---

## 25. 常见瓶颈二：Binder 同步等待

system_server 主线程在启动中调用 native daemon/HAL：

```text
main → Binder transact → daemon/HAL
main blocked waiting reply
```

耗时可能实际发生在：

- 对端 Binder 队列排队；
- 对端线程池不足；
- 对端等待驱动/硬件；
- 对端反向同步调用 system_server；
- SELinux/audit 或服务 lazy start。

仅看 system_server Java 栈会看到 `BinderProxy.transactNative`，需要继续检查目标进程 trace 和 Binder flow。

---

## 26. 常见瓶颈三：锁竞争

启动并行化后，两个任务可能同时需要：

- PackageManager lock；
- AMS global lock；
- WMS global lock；
- class loader lock；
- LocalServices 内某服务锁；
- 文件/数据库内部锁。

如果 worker 持锁，main 在 join 前更早的位置就被同一锁挡住，并行窗口会消失。

```text
worker: [hold global lock ─────────]
main:          wait lock ──────────
```

优化前画锁持有与外部调用图，避免简单增加线程把串行依赖变成不可预测竞争。

---

## 27. 常见瓶颈四：类加载与静态初始化

首次 `startService(Class)` 可能触发：

- dex page fault；
- ClassLinker 工作；
- verification；
- 静态字段初始化；
- native library load；
- SystemServiceRegistry 等大静态块。

若某 trace 外层很慢但 `onStart` warning 不明显，构造器之前的类初始化可能是原因。

不要把大 I/O、Binder 查询或线程启动塞入静态初始化块：它在类首次使用线程执行，依赖点隐蔽，异常表现为 `ExceptionInInitializerError`，还可能持有 class initialization lock。

---

## 28. 常见瓶颈五：GC 与内存压力

启动阶段大量解析 XML、扫描包、构建 map/list，会快速分配对象。症状：

- Perfetto 中出现 GC pause/concurrent GC；
- ART 日志显示频繁 GC；
- 主线程被 suspend；
- 多个并行任务提高瞬时内存峰值。

优化应先查对象生命周期和重复解析，而不是盲目调大 heap。并行化可缩短墙钟时间，也可能提高峰值内存、触发更早 GC，最终抵消收益。

---

## 29. 常见瓶颈六：日志与调试功能

大量启动日志、stack trace、StrictMode、Binder calls stats、debug instrumentation 会有成本。userdebug/eng 与 user 版本的启动时间不能直接横比。

但不要把所有差异归咎于日志：

- 同一 build、同一采集配置做前后对照；
- 分清冷 page cache 与热 cache；
- 多次测量看分布；
- 避免 trace buffer 太小导致事件丢失；
- 记录是否首次启动、OTA 后首启、runtime restart。

---

## 30. Looper slow dispatch 与启动阶段

SystemServer 主 Looper 设置：

```java
setSlowLogThresholdMs(
        SLOW_DISPATCH_THRESHOLD_MS,   // 100ms
        SLOW_DELIVERY_THRESHOLD_MS);  // 200ms
```

含义：

- dispatch 慢：消息真正执行太久；
- delivery 慢：消息从计划到开始执行等待太久。

但 SystemServer 早期许多启动代码发生在 `Looper.loop()` 正式进入消息循环之前，未必表现为普通 slow dispatch。因此启动 trace 与 Looper slow log 是互补工具，不能相互替代。

---

## 31. Watchdog 与 50ms warning 的量级不同

| 机制 | 关注点 | 典型结果 |
|---|---|---|
| SSM 50ms warning | 单个生命周期回调偏慢 | 日志提醒 |
| Looper slow log | 消息投递/执行偏慢 | 日志与诊断 |
| Watchdog | system_server 关键线程长时间无响应/锁死 | 抓取现场，可能终止 system_server |

50ms warning 多次累积会拖慢启动，但远未必构成 Watchdog 超时。Watchdog 超时也不一定能由某一条 `took 60ms` 日志解释。

---

## 32. BOOT_TIME_EVENT 与 trace 的区别

SystemServer 还通过 `FrameworkStatsLog.write()` 记录宏观事件，例如：

- system_server init start；
- package manager init start/ready；
- system_server ready。

Atom/事件适合跨设备统计关键里程碑；trace 适合单次启动的详细时间线。

```text
统计 Atom：这批设备 PMS 初始化 P95 多久？
Perfetto：这一次 PMS 的 820ms 卡在哪个线程/锁/Binder？
```

二者粒度和用途不同。

---

## 33. 日志法的最低成本分析流程

有 userdebug 设备时可先：

```bash
adb logcat -b all -v threadtime \
  -s SystemServerTiming SystemServerTimingAsync SystemServiceManager
```

关注：

```text
StartX
X took to complete: Nms
Service X took N ms in onStart/onBootPhase
InitThreadPoolExec:X
WaitInitThreadPoolShutdown
```

日志法优点是快；缺点是：

- user build 可能没有 duration；
- 无法完整显示 CPU 调度和锁；
- begin/end 日志可能交错；
- 多线程事件不能只按文本顺序推断依赖。

发现大块后应进入 trace。

---

## 34. Perfetto 中应看什么

采集需包含适当 atrace category（常见 `ss`/system_server）与调度、Binder、频率、I/O 等数据。打开后：

1. 找 `system_server` 主线程。
2. 找 `StartServices/startBootstrapServices/startOtherServices`。
3. 展开嵌套 `StartX`。
4. 查同时间段 `system-server-init-thread-*`。
5. 看主线程是 Running、Runnable 还是 Sleeping/Blocked。
6. 沿 Binder transaction 查看目标进程。
7. 查 GC、I/O、锁竞争和 CPU 频率。
8. 找 Future wait 与 worker 完成的交点。

大 slice 只是入口，线程状态和跨线程依赖才告诉你原因。

---

## 35. 线程状态如何解读

| 状态 | 常见含义 | 下一步 |
|---|---|---|
| Running | 正在 CPU 执行 | 看调用栈/方法热点 |
| Runnable | 想运行但未获得 CPU | 看 CPU 竞争、优先级、核心频率 |
| Sleeping | 等待事件/Future/Binder/I/O | 查 wakeup 与依赖线程 |
| blocked on monitor | Java 锁竞争 | 找锁 owner 及其等待链 |
| binder wait | 同步 IPC | 沿 transaction 到目标进程 |

不要看到主线程不 Running 就说“SystemServer 没做事”。它可能被另一条关键路径控制。

---

## 36. 冷启动、热启动与首次启动

至少分三种样本：

### 冷设备启动

文件页不在内存、硬件初始化完整，最接近用户开机体验。

### runtime restart / framework restart

kernel/native daemon/文件缓存可能仍在，只重启 runtime/system_server，通常更快。

### 首次启动或 OTA 后首启

包扫描、dexopt、数据迁移、APEX/rollback 检查更多，可能显著更慢。

比较数据前记录 `mRuntimeRestart`、first boot、upgrade、build type 和设备温度。否则结论可能只是样本不同。

---

## 37. 平均值不够

启动受 I/O、温度、调度和后台状态影响。应多次测量：

```text
median / P50：典型体验
P90/P95：偶发慢启动
max：极端异常线索
```

一次从 10.2s 到 9.7s 不能证明优化有效。应控制变量、重复测试、保留原始 trace，并确认功能/启动依赖未被破坏。

---

## 38. 源码级关键路径分析法（无需设备）

Mac 上可以建立表格：

| 序号 | 主线程步骤 | 是否 submit | Future | 第一个 wait/消费者 | 依赖 |
|---:|---|---|---|---|---|
| 1 | SystemConfig | 是 | 未保存 | 隐式首次 get | 配置解析 |
| 2 | SensorService | 是 | `mSensorServiceStart` | WMS 前 wait | PMS/AppOps |
| 3 | BlobStore | 是 | `mBlobStoreServiceStart` | phase 520 前后 wait | 包/存储 |
| 4 | WebView prep | 是 | local Future | phase 600 前 wait | Zygote preload |

然后画 DAG：

```text
提交点 → worker work → wait点 → 下一 phase
主线程其他 startService ──────┘
```

即使没有实测时长，也能找出潜在串行点、错误早等、遗漏等待和隐式依赖。

---

## 39. 优化策略一：延后非关键工作

如果一项工作不影响：

- 默认显示；
- PMS/AMS/WMS 基础 ready；
- 启动 Home；
- 用户解锁；
- 第一帧交互；

可以考虑推到更晚 phase 或 boot completed 后。

但“延后”不是“消失”：

- 可能把卡顿转移到用户首次使用；
- 可能违反广播/三方应用可启动前的安全依赖；
- 需要明确触发、重试和异常处理；
- 不能让 Binder 已发布却在首个调用中做不可控重初始化。

---

## 40. 优化策略二：安全并行

适合：相互独立的只读配置解析、预加载、无共享锁 native 初始化。

不适合直接并行：

- 修改相同全局 map；
- 依赖确定注册顺序；
- 同时持有 PMS/WMS/AMS 大锁；
- 必须在主 Looper 线程初始化的对象；
- 可能互相 Binder 回调的服务。

并行优化必须同时提交“依赖证明”：为什么并发安全、在哪里 join、错误如何传播。

---

## 41. 优化策略三：缩小同步初始化

把服务初始化拆成：

```text
最小可发布核心
  + phase 前必须完成部分
  + 用户解锁后部分
  + 首次使用可延迟部分
  + 后台维护部分
```

但发布前必须建立线程安全不变量。一个好设计不是“onStart 越短越好”，而是“onStart 只同步完成对外安全可见所必需的状态”。

---

## 42. 优化策略四：减少重复工作

常见重复：

- 同一 XML 被多个服务各自解析；
- 相同包列表反复复制/排序；
- 同一 Binder 服务每次重新查询；
- 相同文件存在性/属性反复同步读取；
- 启动阶段生成很快就丢弃的大对象。

可通过共享只读快照、LocalService、明确缓存生命周期减少。但缓存要考虑用户、配置变化、OTA 和 runtime restart，不能只为一次 benchmark 永久缓存错误状态。

---

## 43. 优化策略五：消除主线程同步 Binder

先判断调用是否必须在当前点拿到结果：

- 若结果影响下一步安全决策，保留同步但优化对端。
- 若只是通知，可考虑 oneway/异步 callback，但要保证顺序和失败语义。
- 若可预取，提前在独立线程启动并在使用点 join。
- 若对端尚未 ready，调整明确启动顺序而不是轮询 sleep。

不能为减少等待随意改成 oneway；oneway 没有同步返回和异常，且会形成异步队列积压。

---

## 44. 错误的“优化”示例

### 删除必要 wait

时间线变短，但三方 App 偶发在 WebView/包数据未准备时启动。

### 全部扔线程池

造成锁竞争、CPU 抢占、内存峰值和不可预测时序。

### 只移动 trace 标签

测量数字变小，真实工作被挪到标签外，用户体验未变化。

### 捕获异常继续

表面 boot 成功，服务半初始化，后续更难诊断。

### 把工作移到第一次 API 调用

开机指标好看，但用户第一次点功能出现长卡顿。

性能优化必须以端到端用户里程碑和正确性验证为准。

---

## 45. 一次慢启动的排查模板

```text
现象：从哪一里程碑到哪一里程碑慢？
环境：build、设备、冷/热、首次/升级、温度
证据：logcat duration + Perfetto trace
最大 slice：名称、线程、开始/结束
线程状态：Running/Runnable/Sleeping/Blocked
依赖：Binder 目标、锁 owner、Future worker、I/O 文件
根因：真正控制关键路径的工作
修改：延后/并行/减少/优化对端
正确性：依赖屏障、异常、用户/phase 边界
结果：多次 P50/P95，回归检查
```

“某服务 slice 最大”只是定位入口，不等于根因结论。

---

## 46. 常见误区纠正

### 误区 1：所有 `traceBegin` 都会在 logcat 打 duration

错误。user build 与日志配置会影响 duration 日志，trace slice 是另一条机制。

### 误区 2：异步任务不算启动时间

错误。只要关键路径最终 wait 它，或 boot complete shutdown 等它，就会影响启动。

### 误区 3：线程池越大启动越快

错误。CPU/I/O/锁竞争可能让主线程更慢。

### 误区 4：50ms warning 就是 ANR

错误。它只是 SSM 性能提醒。

### 误区 5：主线程 slice 长就是 CPU 热点

错误。可能在等 Binder、锁、Future 或 I/O。

### 误区 6：去掉 wait 就是优化

错误。可能破坏依赖和启动安全。

### 误区 7：SystemServer ready 等于用户已经看到 Launcher

错误。不同指标的起止里程碑不同。

### 误区 8：一次测量足够

错误。应控制环境并看分布。

### 误区 9：userdebug 和 user 可直接横比

错误。调试日志、检查和 instrumentation 不同。

### 误区 10：把工作挪出 trace 就变快了

错误。测量边界变化不等于用户关键路径变化。

---

## 47. 复读：最容易不理解的五个关系

### 47.1 slice 时长与 CPU 时长

slice 是 begin 到 end 的墙钟区间，包含执行和等待。CPU 忙不忙必须结合调度轨道。

### 47.2 submit 与完成

submit 只表示排队成功；worker 开始、任务完成、Future 被消费是三个不同时间点。

### 47.3 并行分支与关键路径

一条 worker 任务耗时很长，但若在用户里程碑后才需要，它未必影响当前指标；短任务若主线程立即等待，反而可能直接在关键路径。

### 47.4 trace 与日志

trace 为时间线结构，日志为离散文本。二者共享名字但启用条件、线程关系和分析能力不同。

### 47.5 boot completed 与 pool shutdown

Android 11 在 phase 1000 回调之后关闭 init pool；仍未完成的启动任务最多再被等待 20 秒，超时会抓栈并失败，而不是静默留到运行期。

---

## 48. Mac 上的只读练习

### 练习 1：导出主线程 trace 名字

```bash
rg -n 't\.traceBegin\("' \
  frameworks/base/services/java/com/android/server/SystemServer.java
```

按行号整理启动顺序，不要按字母排序。

### 练习 2：配对 submit 与 wait

```bash
rg -n "SystemServerInitThreadPool\.submit|waitForFutureNoInterrupt" \
  frameworks/base/services/java/com/android/server/SystemServer.java
```

为每个 Future 标出提交点、等待点和中间可重叠工作。

### 练习 3：比较两层计时

```bash
rg -n "StartService |warnIfTooLong|SERVICE_CALL_WARN_TIME_MS" \
  frameworks/base/services/core/java/com/android/server/SystemServiceManager.java
```

说明外层 StartX 与 SSM onStart warning 的范围差异。

### 练习 4：检查 pool 失败路径

```bash
sed -n '1,220p' \
  frameworks/base/services/core/java/com/android/server/SystemServerInitThreadPool.java
```

回答：任务异常何时被主线程看见？shutdown 超时抓哪些栈？pending description 何时删除？

### 练习 5：画静态关键路径图

任选 Sensor、WebView 或 BlobStore，画：

```text
submit → worker prerequisites → execution → Future completion
     ↘ main overlapping work → wait → next phase/consumer
```

---

## 49. 自测题

1. 为什么三项工作总时长不能简单相加？
2. TimingsTraceAndSlog 能否跨线程共用？
3. user build 没有 duration 日志是否说明没有 trace？
4. SSM 50ms warning 测的是哪一段？
5. InitThreadPool 的线程数和优先级是什么？
6. submit 返回说明任务完成了吗？
7. Future 异常怎样传播到主线程？
8. 为什么 WebView prepare 必须在 phase 600 前汇合？
9. shutdown 最多等待多久？
10. 主线程处于 Sleeping 是否说明它不影响启动？
11. 并行化为何可能变慢？
12. 如何区分 wall duration 与 CPU time？

---

## 50. 参考答案

1. 并行任务可重叠，最终由最长依赖链决定。
2. 不能；实例记录创建线程并维护线程内嵌套栈。
3. 不说明；Trace slice 和 Java duration 日志启用条件不同。
4. 单个 SystemService 的 onStart/onBootPhase/用户回调墙钟时长。
5. 可用 CPU 数量的固定线程池，前台线程优先级。
6. 不说明，只表示任务已提交并返回 Future。
7. 在 Future get/`waitForFutureNoInterrupt` 等汇合点观察并包装传播。
8. 三方 App 启动后可能立即使用 WebView，依赖必须先完成。
9. Android 11 源码为 20 秒。
10. 不说明；它可能在关键路径上等待决定性依赖。
11. 会引入 CPU/I/O/锁/内存竞争及调度开销。
12. 用调度轨道/线程运行区间结合 slice 分析，不能只看 begin-end。

---

## 51. 本章总结

SystemServer 启动性能的分析主线：

```text
SystemServer 主线程串行骨架
  + TimingsTraceAndSlog 标记区间
  + SSM 记录服务生命周期慢调用
  + InitThreadPool 提供受控并行
  + Future/latch 在真实依赖点汇合
  + Perfetto 还原线程、Binder、锁、I/O、GC
  = 找到决定用户里程碑的关键路径
```

真正的优化不是让某条日志数字变小，而是：

- 缩短端到端关键路径；
- 保留所有正确依赖和失败传播；
- 不把卡顿转移到首个用户操作；
- 用同环境、多样本和 trace 证明结果。

---

## 52. 下一章预告

第 97 章将继续学习：

**Android system_server 线程模型：主 Looper、Binder 线程池、ServiceThread、HandlerThread 与 Watchdog**

重点回答生命周期回调、Binder 请求和 Handler 消息分别在哪个线程执行，以及线程池耗尽、锁等待和主线程消息堆积如何形成系统级卡顿。
