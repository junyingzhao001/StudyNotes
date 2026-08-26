# 97 Android system_server 线程模型：主 Looper、Binder 线程池、ServiceThread 与 Watchdog

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：看到任意 system_server 代码时，能判断它实际运行在主线程、Binder 线程、共享 ServiceThread、专用 HandlerThread 还是调用者线程，并能解释消息积压、Binder 线程耗尽、锁等待和 Watchdog 的关系。

---

## 1. system_server 不是“一个主线程运行所有服务”

许多 Framework 服务都在同一个 system_server 进程，但代码可能运行在完全不同的线程：

```text
system_server process
 ├─ main                 SystemServer 主 Looper、部分生命周期
 ├─ Binder:xxxxx_*       Binder 入站事务线程池
 ├─ android.fg           共享前台 ServiceThread
 ├─ android.ui           系统服务 UI 线程
 ├─ android.io           较短 I/O/daemon 通信
 ├─ android.display      显示关键操作
 ├─ android.anim         窗口动画
 ├─ android.anim.lf      Surface animation（真实路径在 WM 包）
 ├─ android.bg           共享后台线程
 ├─ 各服务 HandlerThread 专用线程
 └─ watchdog             监督线程
```

“这段代码属于 PowerManagerService”不能回答线程问题。线程由**入口和是否 post**决定。

---

## 2. 本章最重要的追踪规则

```text
普通 Java 调用：继续在调用者线程
Handler.post/sendMessage：切到该 Handler 绑定的 Looper 线程
跨进程同步 Binder：服务端在 Binder 线程执行，客户端等待
oneway Binder：客户端不等 reply，服务端仍在 Binder 线程池排队执行
LocalServices：普通同进程 Java 直调，不自动切线程
Executor.execute：切到该 Executor 实际后端
```

不要根据方法名中的 `async`、`Internal`、`Service` 猜线程，要追对象来源。

---

## 3. 源码地图

```text
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/services/core/java/com/android/server/ServiceThread.java
frameworks/base/services/core/java/com/android/server/FgThread.java
frameworks/base/services/core/java/com/android/server/UiThread.java
frameworks/base/services/core/java/com/android/server/IoThread.java
frameworks/base/services/core/java/com/android/server/DisplayThread.java
frameworks/base/services/core/java/com/android/server/AnimationThread.java
frameworks/base/services/core/java/com/android/server/Watchdog.java
frameworks/base/core/java/com/android/internal/os/BackgroundThread.java
frameworks/base/core/java/android/os/HandlerThread.java
frameworks/base/core/java/android/os/Looper.java
frameworks/base/core/java/android/os/MessageQueue.java
frameworks/base/core/jni/android_util_Binder.cpp
frameworks/native/libs/binder/ProcessState.cpp
```

SurfaceAnimationThread 位于 WindowManager 源码目录，不要只在 `com/android/server/` 根目录寻找。

---

## 4. system_server 主线程如何成为 Looper 线程

SystemServer 早期：

```java
Process.setThreadPriority(Process.THREAD_PRIORITY_FOREGROUND);
Process.setCanSelfBackground(false);
Looper.prepareMainLooper();
```

完成同步服务启动后：

```java
Looper.loop();
throw new RuntimeException("Main thread loop unexpectedly exited");
```

因此主线程有两个时期：

```text
早期：直接执行 SystemServer.run/startXxxServices 串行启动代码
后期：进入 Looper.loop，处理发给 main Handler 的消息
```

早期慢代码不一定表现为“某条 Looper 消息 dispatch 慢”，因为当时还未进入 loop。

---

## 5. 主线程负责什么

常见工作：

- SystemService 构造、`onStart()`、BootPhase 分发的调用线程；
- 一些服务把 Handler 绑定到 `Looper.getMainLooper()`；
- system_server Application/Context 相关回调；
- 部分 AMS/系统生命周期消息；
- Watchdog main-thread checker 的探针。

但 Binder 客户端调用系统服务通常不会先自动转到 main。AIDL Stub 的 `onTransact()` 默认在 Binder 线程。

---

## 6. Looper、MessageQueue、Handler 的关系

```text
Thread
  └─ Looper（一个线程最多一个）
       └─ MessageQueue（时间排序的消息队列）
            ↑
         Handler（投递与分发入口，可有多个）
```

Handler 构造时绑定某个 Looper：

```java
Handler h = new Handler(looper);
h.post(runnable);
```

Runnable 并不是“开启一个新线程”，而是在 looper 所在线程排队。

多个服务共享一个 HandlerThread 时，它们的消息共享同一条串行队列；一个慢消息会推迟后面的其他服务消息。

---

## 7. dispatch 慢与 delivery 慢

Android 11 system_server 主 Looper、FgThread、UiThread 等设置慢日志阈值。

```text
delivery latency：消息应该执行到真正开始执行的等待时间
dispatch duration：真正开始执行到处理结束的时间
```

例子：

```text
消息 A 执行 500ms
消息 B 在 A 后面，自己只执行 2ms
```

- A 是 slow dispatch。
- B 可能是 slow delivery。

因此看到 delivery 慢，不应立即优化 B 的处理代码；真正阻塞者可能是它前面的消息、同步屏障或线程调度。

---

## 8. Binder 线程池是什么

客户端跨进程调用 system_server 服务时：

```text
client thread
  → Binder driver
  → system_server Binder thread pool 中某线程
  → Stub.onTransact
  → 服务 BinderService 方法
```

Binder 线程池不是 Java `ExecutorService`，由 Binder 驱动与 libbinder/Java Binder 运行时协作按需管理。

SystemServer 设置：

```java
private static final int sMaxBinderThreads = 31;
BinderInternal.setMaxThreads(sMaxBinderThreads);
```

Android 11 libbinder 一般默认最大值为 15，而 system_server 主动提高为 31，因为它承载大量系统服务。这里是允许驱动请求生成的额外线程上限语义，不能简单当作进程启动时立即创建 31 个常驻 Java 线程。

---

## 9. 为什么禁用 Binder 后台调度继承

SystemServer：

```java
BinderInternal.disableBackgroundScheduling(true);
```

注释意图是让进入 system_server 的 Binder 调用以重要前台调度处理，避免调用者本身是后台优先级时把服务端关键 Binder 处理线程也压低。

它不代表所有 system_server 工作都以最高实时优先级运行，也不绕过明确的线程 priority/group 设置。

服务若把后续重活 post 到 BackgroundThread，该工作仍采用后台线程自己的调度属性。

---

## 10. Binder 入站方法为何不应做重活

如果每个 Binder 方法都阻塞：

```text
Binder thread 1：等磁盘
Binder thread 2：等 HAL
Binder thread 3：等大锁
...
Binder thread 31：等另一个进程
```

新的 IPC 无线程可处理，其他无关系统服务也会受影响，因为它们共享 system_server Binder 线程资源。

常见设计：

1. Binder 线程做参数、权限、calling UID 校验。
2. 在锁内快速更新状态或复制请求。
3. post 到专用 Handler 执行串行状态机。
4. 若 API 必须同步返回，再用受控等待或同步路径，但要审查死锁。

“一律 post”也不正确；同步 API 的返回语义和 ordering 必须保留。

---

## 11. Binder 线程池耗尽与高 CPU 不同

池耗尽通常是线程都被占用，可能 CPU 很低：

- 等锁；
- 等同步 Binder reply；
- 等条件变量/latch；
- 阻塞 I/O；
- sleep；
- 调用可能反向回 system_server 的外部进程。

症状：多个客户端卡在 Binder 调用，system_server 中许多 Binder 线程栈停在相似等待点。

不能仅看 CPU 低就排除严重 Binder 饥饿。

---

## 12. Watchdog 如何检查 Binder 可用性

Watchdog 添加：

```java
addMonitor(new BinderThreadMonitor());
```

其 monitor：

```java
Binder.blockUntilThreadAvailable();
```

它会等待 Binder 线程可用于处理新入站 IPC，从而检测 system_server 是否仍具备对外响应能力。

这不是统计“31 个线程中用了几个”的监控面板，也不是每个 Binder 调用的超时器。它被 Watchdog 监控链执行；若长时间无法返回，最终表现为 Watchdog 阻塞证据。

---

## 13. ServiceThread 在 HandlerThread 上增加了什么

```java
public class ServiceThread extends HandlerThread {
    private final boolean mAllowIo;

    public void run() {
        Process.setCanSelfBackground(false);
        if (!mAllowIo) {
            StrictMode.initThreadDefaults(null);
        }
        super.run();
    }
}
```

它仍是一个带 Looper 的 HandlerThread，但增加两项 system_server 约束：

- 不允许线程因通用机制自行变成后台调度；
- `allowIo=false` 时安装 StrictMode 线程默认策略，帮助发现不应发生的磁盘/网络 I/O。

`allowIo=false` 不是内核级绝对禁止 I/O，而是通过 StrictMode 发现违规；具体惩罚取决于策略/build。

---

## 14. FgThread

```java
super("android.fg", THREAD_PRIORITY_DEFAULT, true);
```

用途：常规前台系统服务操作，不能被共享 BackgroundThread 的长时间保存状态工作拖延。

特征：

- 惰性单例；
- 默认线程优先级；
- 允许 I/O；
- Looper 使用 system_server trace tag；
- slow dispatch 100ms、slow delivery 200ms；
- 暴露 Handler 与 HandlerExecutor。

“Fg”表示相对重要和及时，不意味着可以放无限重活。

---

## 15. UiThread

```java
super("android.ui", THREAD_PRIORITY_FOREGROUND, false);
```

并把线程放入 top-app 调度组，用于 system_server 自己显示的 UI，如系统对话框、部分 Autofill UI 等。

它要求操作只需几毫秒，避免系统 UI 卡顿；不允许 I/O，并有 100/200ms 慢日志。

它不是应用进程的 Android main/UI thread。只是 system_server 中专门服务 UI 操作的线程。

---

## 16. IoThread

```java
super("android.io", THREAD_PRIORITY_DEFAULT, true);
```

用于可能短暂阻塞的非后台服务 I/O，尤其与网络 daemon 通信。它允许 I/O，但名称不表示可以把任意无限期磁盘扫描、网络下载都堆进去。

共享队列意味着一个服务的长 I/O 会影响 BluetoothManager、EntropyMixer 等其他使用者。长且独立的工作更适合专用线程/Executor。

---

## 17. DisplayThread 与 AnimationThread

DisplayThread：

- `android.display`；
- `THREAD_PRIORITY_DISPLAY + 1`；
- 不允许 I/O；
- WMS、DMS、InputManager 的低延迟显示操作。

AnimationThread：

- `android.anim`；
- `THREAD_PRIORITY_DISPLAY`；
- 不允许 I/O；
- 传统窗口动画、starting window、traversal 相关工作。

AnimationThread 比 DisplayThread 高一个优先级等级（数值更小/更重要），因为动画时序更敏感。

这些线程不等于 RenderThread、SurfaceFlinger 主线程或应用 Choreographer 线程。

---

## 18. SurfaceAnimationThread

用于 SurfaceControl 层面的动画工作，与传统 AnimationThread 分开，减少一类动画阻塞另一类。

它被 Watchdog 单独检查，说明它对系统视觉响应很关键。

读源码时注意真实包路径通常在：

```text
frameworks/base/services/core/java/com/android/server/wm/SurfaceAnimationThread.java
```

类 package 仍可被 `com.android.server.Watchdog` 引用，但文件不一定和 FgThread 同目录。

---

## 19. BackgroundThread 为什么不被 Watchdog 默认检查

Watchdog 注释明确：BackgroundThread 可能执行较长任务，对及时性没有同等保证，所以没有加入默认 HandlerChecker。

这不代表：

- 后台线程永远不会影响系统；
- 可以无限阻塞；
- 持有的锁不会挡住关键线程；
- 任务丢失无需处理。

若 BackgroundThread 持有主线程所需锁，主线程仍会被 Watchdog 检测为卡死。Watchdog 关注的是关键线程能否前进，不是给每个线程设统一 SLA。

---

## 20. 共享线程还是专用线程

选择共享线程的优点：

- 少创建线程，节省内存；
- 生命周期统一；
- 方便 Watchdog/trace；
- 同类任务天然串行。

风险：

- 一个服务阻塞同队列所有消费者；
- 隐式耦合难发现；
- 消息积压归因困难。

专用线程适合：

- 独立长状态机；
- 可阻塞 I/O；
- 特定优先级；
- 与共享队列隔离故障。

但线程过多会增加内存、调度和维护成本。应按延迟、阻塞性、隔离和串行状态需求选择。

---

## 21. Handler 不自动提供线程安全

把所有状态只在同一个 Handler 线程访问，可以形成 thread confinement；但 Binder 方法如果在入站线程直接读写相同字段，就打破了这个假设。

常见模式：

```text
Binder thread：权限检查 → post message
Handler thread：唯一修改核心状态
Binder thread：同步 getter 可能加锁读快照
```

必须明确：

- 哪些字段只属于 Handler；
- 哪些字段由 lock 保护；
- callback 是否在锁外发出；
- dump 是否可能并发读状态。

“我们有 Handler”不等于类天然线程安全。

---

## 22. 同步 Handler 跳转的风险

`runWithScissors()`、latch、Future 可让当前线程等待目标 Handler 完成：

```text
Binder thread → post to main → wait
main → 处理 runnable → signal
```

若 main 同时等待这个 Binder transaction 返回，就形成死锁。

```text
main --sync Binder--> Binder thread
Binder thread --post+wait--> main
```

所以同步跨线程执行必须审查调用来源；普通异步 `post` 更安全，但不能满足所有同步返回 API。

---

## 23. 锁内 Binder 调用是系统级高风险模式

```java
synchronized (mLock) {
    remote.callback();
}
```

远端可能：

- 很慢；
- 死亡；
- 反向调用本服务并再次请求 `mLock`；
- 调用另一个等待本服务的系统组件。

优先做法：锁内复制必要数据/目标 callback，锁外执行 Binder 调用，再在需要时锁内提交结果并校验 generation。

不是所有锁都能机械移除；关键是缩短持锁区并固定全局锁顺序。

---

## 24. calling identity 与线程切换

Binder 入站线程可取得真实：

```java
Binder.getCallingUid()
Binder.getCallingPid()
```

如果 Binder 方法 post Runnable 到 Handler 后再在 Handler 线程调用 `getCallingUid()`，通常看到的是 system_server 自己，因为原事务调用上下文没有随普通 Runnable 自动传播。

正确模式：

```java
int uid = Binder.getCallingUid();
String packageName = ...校验...;
mHandler.post(() -> handle(uid, packageName));
```

或者在入站线程完成权限校验后，显式传递可信结果。

`clearCallingIdentity()` 也只作用于当前 Binder 调用线程的身份上下文，必须 `finally restoreCallingIdentity(token)`。

---

## 25. oneway 不是后台线程注解

oneway AIDL 的含义：客户端事务提交后不等待 reply，且同一 Binder node 的异步事务有顺序/排队语义。

服务端仍由 Binder 线程池取出并执行。如果 oneway 方法耗时：

- 消耗 Binder 线程；
- 异步队列可能积压；
- 客户端感知不到同步异常；
- 新状态到达延迟。

服务端可再 post 到 Handler，但要注意 Binder oneway 顺序与 Handler 中其他来源消息的相对顺序。

---

## 26. Watchdog HandlerChecker 怎样判断活性

每个 checker 绑定一个 Handler。周期检查时：

1. 若 Looper 正在 polling 且没有 Monitor，认为线程健康，无需切换。
2. 否则 `postAtFrontOfQueue(this)`。
3. 目标线程执行 checker，并依次调用 Monitor。
4. 执行结束标记 completed。

状态：

```text
COMPLETED
WAITING       < timeout/2
WAITED_HALF   >= timeout/2 且 < timeout
OVERDUE       >= timeout
```

检查的本质是：“高优先级探针能否在期限内到达队首并完成 monitor”。

---

## 27. 为什么 `postAtFrontOfQueue` 仍可能迟迟不执行

队首插入不能打断当前正在执行的消息。若目标线程：

- 正在执行一个超长 Runnable；
- 卡在 monitor/锁；
- 同步 Binder 调用不返回；
- 阻塞 I/O；
- native 代码死循环；

checker 只能等待当前 dispatch 结束。

因此它能检测“线程不能回到消息循环”，但不是抢占式 watchdog。

---

## 28. Watchdog 默认监控哪些线程

Android 11 构造器加入：

```text
foreground thread
main thread
ui thread
i/o thread
display thread
animation thread
surface animation thread
```

并在 foreground checker 上运行各种 Monitor，包括 BinderThreadMonitor。

服务也可 `addThread(handler, timeout)` 添加关键线程，或 `addMonitor()` 检查重要锁。

并非所有 system_server 线程自动被监控。

---

## 29. Looper polling 为何可视为健康

如果 MessageQueue 正在 native poll，说明线程已经处理完此前消息，正等待新事件。没有额外 Monitor 时，这本身证明线程未卡在 Java 工作或锁中，所以可以跳过实际投递 checker，减少上下文切换。

但若有 Monitor，仍需让 checker 线程执行 monitor，因为 Monitor 可能尝试获取服务锁；Looper 空闲并不能证明其他关键锁可用。

---

## 30. pauseWatchingCurrentThread 的边界

SystemServer 在某些已知长操作（PMS main、dexopt 等）暂时 pause 主线程 checker，完成后 resume。

这不是通用性能优化，也不能掩盖未知卡死：

- 必须限定明确操作和原因；
- 必须 try/finally 成对恢复；
- pause 有计数，嵌套 resume 次数要匹配；
- 暂停期间 Watchdog 对该 checker 的保障减弱。

只因代码可能慢就随意 pause，会把真正死锁变成无限挂起。

---

## 31. 三种常见卡顿图

### 主 Looper 消息积压

```text
main: [A 800ms][B][C][D]
                 ↑ B/C/D delivery late
```

### Binder 池耗尽

```text
31 Binder threads → 全部等待 mLock/remote reply
new clients → driver queue，无法及时处理
```

### 锁跨线程传播

```text
BackgroundThread 持 Lock X 做 I/O
Binder thread 等 X
main thread 同步调用该 Binder path
Watchdog 最终看到 main 不前进
```

根因线程不一定就是最终被 Watchdog 报告的线程。

---

## 32. 如何从线程 dump 判断入口

常见栈顶部：

| 栈/线程 | 含义 |
|---|---|
| `Looper.loop` / `MessageQueue.nativePollOnce` | Looper 空闲或等待消息 |
| `Handler.dispatchMessage` | 正在处理 Handler 消息 |
| `Binder.execTransactInternal` | Binder 入站事务 |
| `BinderProxy.transactNative` | 正在同步调用远端 Binder |
| `Object.wait` / `ConditionVariable.block` | 条件等待 |
| `Blocked` + monitor owner | Java synchronized 锁竞争 |

完整分析要记录：线程名、入口、持锁、等待对象、目标 Binder、谁能唤醒它。

---

## 33. Perfetto 线程分析步骤

1. 找 system_server process。
2. 展开 main、Binder、android.fg/ui/io/display/anim。
3. 找长 Running slice 或长 Sleeping 区间。
4. 检查 Looper message/trace 名称。
5. 沿 Binder flow 找目标进程。
6. 看线程从 Runnable 到 Running 的调度延迟。
7. 对照锁竞争/堆栈采样。
8. 判断是处理慢、排队慢还是跨线程依赖慢。

线程名字只是线索，最终以 tid、调用栈和调度轨迹为准。

---

## 34. 线程优先级不能解决逻辑死锁

提高优先级可减少 Runnable 状态的调度等待，但不能解决：

- 等一把永不释放的锁；
- Binder 环等待；
- 队列中当前消息无限阻塞；
- 条件变量永不 signal；
- I/O 设备无响应。

错误提升大量线程优先级还会抢占真正关键线程。先区分 CPU 调度不足与逻辑等待。

---

## 35. 共享 Handler 的消息取消与 token

服务在共享线程投递延迟任务时，应管理：

- Runnable/Message token；
- userId/session generation；
- 服务状态变化后的 removeCallbacks；
- 迟到消息二次校验；
- callback 对象死亡后的清理。

否则用户切换或服务重连后，旧消息可能在新状态上执行。线程串行只能保证执行顺序，不能保证消息仍然有效。

---

## 36. 创建专用 HandlerThread 的检查表

- 名称是否能在 trace/stack 中识别？
- 线程 priority 是否与延迟需求匹配？
- 是否允许 I/O，StrictMode 策略是什么？
- 谁 start、谁 quit？system_server 常驻服务是否确实不退出？
- 核心状态是否仅此线程访问？
- 是否需要 Watchdog.addThread？
- Binder 方法如何切换以及如何返回同步结果？
- 用户 stop/runtime restart 如何清理队列？

优先考虑 `ServiceThread` 而不是裸 HandlerThread，可继承 system_server 的线程约束。

---

## 37. 常见误区纠正

### 误区 1：所有 system_server 服务调用都在主线程

错误。Binder 入站通常在 Binder 线程，Handler 工作在绑定 Looper。

### 误区 2：Handler.post 会创建新线程

错误。它只是向已有 Looper 队列投递。

### 误区 3：oneway 方法不占服务端线程

错误。服务端仍需 Binder 线程执行。

### 误区 4：LocalService 会自动切到服务线程

错误。普通 Java 直调，继续在调用者线程。

### 误区 5：Binder 最大线程数 31 表示启动时已有 31 个线程

错误。Binder 线程按需生成，31 是上限配置语义。

### 误区 6：`allowIo=false` 从内核禁止文件访问

错误。ServiceThread 通过 StrictMode 帮助检测。

### 误区 7：Watchdog 监控 system_server 每一个线程

错误。它监控显式 HandlerChecker 和 Monitor。

### 误区 8：BackgroundThread 没被 Watchdog 检查，所以不影响系统

错误。它持锁或提供依赖时仍可阻塞关键线程。

### 误区 9：提高线程优先级能修死锁

错误。逻辑等待不会因优先级消失。

### 误区 10：线程安全等于所有方法 synchronized

错误。粗锁会扩大竞争与 Binder 环风险，应设计 confinement、快照和锁外调用。

---

## 38. 复读：五组最易混线程边界

### 38.1 服务所属进程与代码执行线程

服务对象位于 system_server，但一次方法可以由任意 Binder 线程、主线程或 LocalService 调用者线程执行。

### 38.2 Binder thread 与 Handler thread

Stub 方法先在 Binder thread；只有显式 post 后，后续代码才在 Handler thread。post 前取得 calling UID，post 后不能重新依赖 Binder calling identity。

### 38.3 shared thread 与 service-owned thread

Fg/Io/Background 是多服务共享，阻塞会跨服务传播；专用线程隔离更强但成本更高。

### 38.4 queue idle 与 lock healthy

Looper polling 说明队列线程空闲，不说明服务的所有锁都可取得；有 Monitor 时 Watchdog 仍必须执行锁检查。

### 38.5 reported blocked thread 与 root cause thread

Watchdog 报 main blocked，根因可能是 Binder thread 等后台线程持有的锁。要沿等待链找到最末端 owner。

---

## 39. Mac 只读源码练习

### 练习 1：列共享线程属性

```bash
for f in FgThread UiThread IoThread DisplayThread AnimationThread; do
  rg -n "super\(|setSlowLogThresholdMs|setTraceTag" \
    frameworks/base/services/core/java/com/android/server/$f.java
done
```

记录线程名、priority、allowIo、trace tag 和 slow threshold。

### 练习 2：追一个 Binder 到 Handler

任选 `PowerManagerService.BinderService` 或 `ClipboardService.ClipboardImpl`：

```bash
rg -n "class BinderService|class ClipboardImpl|mHandler\.(post|sendMessage)" \
  frameworks/base/services/core/java/com/android/server
```

标出入站线程、权限检查点、post 点和状态修改线程。

### 练习 3：检查 calling identity

```bash
rg -n "getCallingUid|clearCallingIdentity|restoreCallingIdentity" \
  frameworks/base/services/core/java/com/android/server | head -100
```

确认 clear/restore 是否 finally 成对，身份是否在 post 前捕获。

### 练习 4：阅读 Watchdog checker

```bash
sed -n '130,370p' \
  frameworks/base/services/core/java/com/android/server/Watchdog.java
```

解释 polling 快路径、front-of-queue、Monitor 和 BinderThreadMonitor。

### 练习 5：找共享线程风险

```bash
rg -n "(FgThread|IoThread|BackgroundThread)\.getHandler" \
  frameworks/base/services | head -100
```

选两个不同服务，判断是否可能因共享队列互相延迟。

---

## 40. 自测题

1. SystemServer 主线程何时进入 Looper.loop？
2. Binder 入站默认在哪类线程执行？
3. Android 11 system_server Binder 最大线程配置是多少？
4. Handler.post 是否创建线程？
5. delivery 慢与 dispatch 慢有什么区别？
6. ServiceThread 的 allowIo=false 如何生效？
7. FgThread 与 BackgroundThread 的用途有何区别？
8. 为什么 Binder 池耗尽时 CPU 可能很低？
9. post 后为什么不能重新取得原 calling UID？
10. Watchdog 为什么在 queue polling 时可跳过无 Monitor 的 checker？
11. 为什么 BackgroundThread 默认未加入 Watchdog？
12. LocalService 的线程由谁决定？

---

## 41. 参考答案

1. 三组服务启动和早期初始化完成后，在 SystemServer.run 末尾进入。
2. system_server Binder 线程池中的线程。
3. 31；它是上限配置，不是预创建数量。
4. 不会，只投递到 Handler 绑定的 Looper。
5. delivery 是排队/调度到开始的延迟；dispatch 是实际处理耗时。
6. 在线程 run 中初始化 StrictMode 默认线程策略，帮助发现违规 I/O。
7. Fg 处理需及时的前台系统操作；Background 可处理更长后台任务，及时性保证较低。
8. 线程可能全部在等待锁、IPC、条件或 I/O，而不是运行 CPU。
9. Binder calling identity 是当前入站事务线程上下文，不随普通 Runnable 传播。
10. native poll 证明线程已经回到队列等待，未卡在处理逻辑。
11. 它设计上允许较长任务，不能用同样及时性 SLA；但仍不能破坏关键锁依赖。
12. 普通直调由调用者线程决定，除非实现内部再 post。

---

## 42. 本章总结

```text
SystemServer 主线程：启动骨架 + 主 Looper 消息
Binder 线程池：跨进程入站，最多配置 31，必须避免阻塞耗尽
ServiceThread：带 Looper、priority、StrictMode 约束的系统服务线程
Fg/Ui/Io/Display/Anim：按延迟与任务类型分工的共享队列
专用 HandlerThread：隔离服务状态机与阻塞工作
Watchdog：向关键 Handler 投探针、运行 Monitor、检查 Binder 可用性
```

定位线程问题时始终画：

```text
入口线程 → 是否 post → 目标队列 → 是否持锁 → 是否同步 IPC
        → 谁在等待谁 → 最末端 owner/完成条件
```

---

## 43. 下一章预告

第 98 章将学习：

**Android Binder 性能与故障：线程池饥饿、同步/oneway 队列、锁与调用链诊断**

会把本章线程模型和第 90～92 章 Binder/Parcel/AIDL 串起来，形成可直接用于 system_server 卡顿、跨进程超时和 Binder 调用慢的排查方法。
