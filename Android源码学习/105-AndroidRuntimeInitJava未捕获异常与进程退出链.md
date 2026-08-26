# 105 Android RuntimeInit——Java 未捕获异常、AMS 报告与进程退出链

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 核心目标：从 ART 发现线程未捕获异常开始，追踪 `Thread.dispatchUncaughtException()`、Android pre-handler、`RuntimeInit.KillApplicationHandler`、`ParcelableCrashInfo`、AMS Binder、AppErrors 和最终进程退出。  
> 环境：macOS 只读源码，不要求编译、运行或制造崩溃。

---

## 1. Java crash 的一句话定义

某个 Java 线程抛出 `Throwable`，沿调用栈一直没有被业务代码捕获，运行时最终把它交给该线程的 `UncaughtExceptionHandler`。

在标准 Android App 默认配置下，平台会：

```text
先写 crash log
 → 把结构化 CrashInfo 同步报告给 AMS
 → AMS/AppErrors 记录并决定组件/UI 策略
 → 崩溃进程 finally 强制结束自身
```

并不是 ART 看到异常后立即直接 `kill -9`。

---

## 2. 本章源码地图

```text
art/runtime/thread.cc
libcore/ojluni/src/main/java/java/lang/Thread.java
libcore/dalvik/src/main/java/dalvik/system/RuntimeHooks.java
frameworks/base/core/java/com/android/internal/os/RuntimeInit.java
frameworks/base/core/java/com/android/internal/os/ZygoteInit.java
frameworks/base/core/java/android/app/ActivityThread.java
frameworks/base/core/java/android/app/ApplicationErrorReport.java
frameworks/base/core/java/android/app/IActivityManager.aidl
frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
frameworks/base/services/core/java/com/android/server/am/AppErrors.java
frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
```

上一章的 AppErrors 后半段在本章复用，本章重点放在 Java 进程内前半段。

---

## 3. 完整调用链

```text
App 某线程抛 Throwable
  ↓ 栈展开，无 catch
ART Thread::HandleUncaughtExceptions
  ↓ JNI 调用 Java Thread.dispatchUncaughtException(e)
Android uncaughtExceptionPreHandler
  ↓ RuntimeInit.LoggingHandler：写 LOG_ID_CRASH
线程专属 handler / ThreadGroup / default handler
  ↓ 默认 RuntimeInit.KillApplicationHandler
ensureLogging + mCrashing 防重入 + stopProfiling
  ↓
new ParcelableCrashInfo(e)：异常转字符串/字段，限制大小
  ↓ 同步 Binder
IActivityManager.handleApplicationCrash(appToken, crashInfo)
  ↓
AMS.findAppProcess → handleApplicationCrashInner → AppErrors
  ↓ 可能等待 crash UI 结果
Binder 返回或报告失败
  ↓ finally
Process.killProcess(myPid)
System.exit(10)
```

---

## 4. ART 是最早的调度者

`art/runtime/thread.cc` 中，ART 在未捕获异常离开 Java 执行边界时调用已缓存的：

```text
java.lang.Thread.dispatchUncaughtException(Throwable)
```

若 dispatch handler 自己又抛异常，ART 会清除该异常，避免继续无限递归。

ART 负责把控制权交到 Java 策略层；Android Framework 决定如何记录、报告和终止。

---

## 5. “未捕获”是相对于当前线程

每个线程有独立 Java 调用栈。后台线程抛异常不会自动沿另一线程传播到主线程。

```text
Thread-A 抛异常且无 catch → Thread-A 的 uncaught handler
Thread-B 仍可暂时运行
默认 Android handler 最终杀整个进程
```

因此最终进程死亡，不是因为异常跨线程传播，而是默认 handler 选择了进程级终止。

---

## 6. Thread 的 handler 层级

标准 Java 语义：

```text
线程显式 UncaughtExceptionHandler
  否则 ThreadGroup
    ThreadGroup 可能再委托 defaultUncaughtExceptionHandler
```

Android 又在最前增加：

```text
uncaughtExceptionPreHandler
```

所以实际 dispatch 先运行平台 pre-handler，再运行 `getUncaughtExceptionHandler()` 返回的正常链。

---

## 7. Android pre-handler 的实现

`Thread.dispatchUncaughtException()`：

```java
UncaughtExceptionHandler initialUeh =
        Thread.getUncaughtExceptionPreHandler();
if (initialUeh != null) {
    try {
        initialUeh.uncaughtException(this, e);
    } catch (RuntimeException | Error ignored) {
    }
}
getUncaughtExceptionHandler().uncaughtException(this, e);
```

pre-handler 抛出的异常被忽略，正常 handler 仍继续。

---

## 8. 为什么平台需要 pre-handler

App 可以调用：

```java
Thread.setDefaultUncaughtExceptionHandler(customHandler);
```

甚至给单个线程设 handler。如果只有 default handler，App 替换后平台可能失去标准 `FATAL EXCEPTION` 日志。

pre-handler 保证平台日志先执行，同时允许 App 自定义后续行为。

---

## 9. App 能否替换 pre-handler

公开 API 只暴露 normal/default handler。pre-handler setter 是 `@hide`，由 `dalvik.system.RuntimeHooks` 给平台配置。

普通 SDK App 可以替换 default handler，但通常不能通过公开 API替换平台 pre-handler。

因此“自定义 handler 后 logcat 完全没有 FATAL EXCEPTION”不是默认预期。

---

## 10. RuntimeInit 在哪里安装两层 handler

`commonInit()`：

```java
LoggingHandler loggingHandler = new LoggingHandler();
RuntimeHooks.setUncaughtExceptionPreHandler(loggingHandler);
Thread.setDefaultUncaughtExceptionHandler(
        new KillApplicationHandler(loggingHandler));
```

同一个 `LoggingHandler` 实例由 default killer 持有，用 `mTriggered` 判断 pre-handler 是否已经执行。

---

## 11. `commonInit()` 什么时候调用

App 由 Zygote fork 后进入：

```text
ZygoteInit.zygoteInit(...)
 → RuntimeInit.commonInit()
 → nativeZygoteInit（Binder thread pool 等）
 → RuntimeInit.applicationInit(...)
 → ActivityThread.main
```

所以在应用业务入口执行前，默认异常处理器已经安装。

非 Zygote Java 工具走 `RuntimeInit.main()` 时也会调用 `commonInit()`。

---

## 12. 为什么不是在 Zygote fork 前安装

`preForkInit()` 只做适合 fork 前共享的 runtime hook/MIME 等初始化；异常 handler 的 `commonInit()` 在子进程路径调用。

这样每个子进程有自己的 handler 状态，例如：

```text
mCrashing
LoggingHandler.mTriggered
mApplicationObject
```

避免把运行期可变状态在 Zygote 中预先污染后复制。

---

## 13. LoggingHandler 做什么

主要逻辑：

```text
mTriggered = true
若 mCrashing 已为 true，则返回，避免重复日志
system process 特殊格式，或普通 App FATAL EXCEPTION 格式
写入 LOG_ID_CRASH
```

它只负责可靠记录，不负责通知 AMS 或杀进程。

---

## 14. 标准 crash 日志

普通 App 类似：

```text
FATAL EXCEPTION: main
Process: com.example, PID: 1234
java.lang.IllegalStateException: ...
    at ...
```

写入 `Log.LOG_ID_CRASH`，logcat 可通过 crash buffer 读取。

“FATAL EXCEPTION” 字符串仍会出现，即使 App 自定义 normal handler 后选择不终止进程；源码注释明确提醒这一点。

---

## 15. system_server 日志格式

当：

```text
mApplicationObject == null
且 uid == SYSTEM_UID
```

记录：

```text
*** FATAL EXCEPTION IN SYSTEM PROCESS: <thread>
```

`mApplicationObject == null` 也可能是 `am` 之类非 Zygote Java 工具，因此还要同时看 system UID，避免误标为 system_server。

---

## 16. 为什么先设置 `mTriggered`

```java
mTriggered = true;
```

即使后续日志调用抛异常，KillApplicationHandler 的 `ensureLogging()` 也不会无条件重复调用同一 handler。

不过 `ensureLogging()` 自身会捕获 logging 异常，最终终止进程仍可继续。

记录失败不能阻断退出保险。

---

## 17. App 自定义 default handler 后会怎样

```text
平台 LoggingHandler 先记录
 → App custom default handler 执行
```

若 custom handler 不调用旧 handler、不杀进程，抛异常的线程终止，但其他线程和进程可能继续存活。

此时默认 `KillApplicationHandler` 没运行，AMS 不一定收到标准 crash report。

---

## 18. 线程专属 handler 更优先

`getUncaughtExceptionHandler()` 返回线程显式 handler，否则 ThreadGroup。显式 handler 可完全接管 normal 链，不自动调用 default killer。

但 Android pre-handler仍先运行。

所以需要区分：

```text
日志是否出现
AMS 是否收到
线程是否终止
整个进程是否终止
```

---

## 19. 自定义 handler “吞异常”真的恢复了吗

抛异常线程的栈已经展开并即将结束。handler 不能跳回异常点继续执行。

主线程若结束，Looper 不再调度，App 可能呈现无响应或空壳；后台线程结束也可能破坏不变量。

保持进程不死不等于恢复到一致状态，通常不推荐长期吞掉未捕获异常。

---

## 20. KillApplicationHandler 的第一步

```java
ensureLogging(t, e);
```

正常 runtime dispatch 中 pre-handler 已触发；但有些代码可能直接调用 default handler 的 `uncaughtException()`，绕开 dispatch。

`ensureLogging()` 补回历史上“直接调用 handler 也会记录”的诊断行为。

---

## 21. `mCrashing` 防重入

```java
if (mCrashing) return;
mCrashing = true;
```

可能发生：

- 两个线程几乎同时未捕获异常。
- crash reporting 本身又抛异常。
- custom/系统回调递归进入。

只让第一个进入完整 AMS 报告，后续避免重复递归。

---

## 22. `volatile` 的意义

`mCrashing` 是 `volatile`，多个 Java 线程能看到已开始 crash 的状态。

但 check-then-set 不是原子 CAS：两个线程理论上仍可能同时看到 false。代码主要降低重入，不是严格的一次性并发协议。

最终 kill 和 AMS 的其他去重策略进一步限制影响。

---

## 23. 为什么 stopProfiling

若 ActivityThread 存在且 method profiling 正在运行，先 `stopProfiling()` 尝试 flush 内存缓冲。

否则下一步杀进程会丢失用于分析 crash 的 trace 数据。

这是一项 best effort；若 stopProfiling 也抛异常，外层 catch 后仍进入 finally 终止。

---

## 24. `mApplicationObject` 是什么

App `ActivityThread.attach(false, startSeq)` 中：

```java
RuntimeInit.setApplicationObject(mAppThread.asBinder());
```

`mAppThread` 是 `ApplicationThread` Binder Stub。AMS 可用该 Binder 在其进程表中找到对应 ProcessRecord。

它不是 `Application` Java 对象，也不是 package name 字符串。

---

## 25. 为什么在 attachApplication 前设置 token

顺序：

```text
setApplicationObject(mAppThread Binder)
 → AMS.attachApplication(mAppThread, startSeq)
```

如果 attach 期间或之后早期初始化 crash，RuntimeInit 已有可报告的 Binder token。

不过 AMS 是否已经完成 ProcessRecord 关联仍受启动时序影响，早期 crash 可能找不到完整上下文。

---

## 26. system_server 为什么 token 为 null

system_server 并不是普通 App 通过 `ActivityThread.attach(false)` 注册的进程，`mApplicationObject` 保持 null。

AMS `handleApplicationCrash()` 接收到 null 时将 processName 视为 `system_server`。

但 system_server 自己向内部 AMS 同步报告的可用性受崩溃位置和锁状态影响，最终仍由 RuntimeInit finally 结束进程。

---

## 27. Throwable 如何变成 ParcelableCrashInfo

```java
new ApplicationErrorReport.ParcelableCrashInfo(e)
```

构造函数调用父类 `CrashInfo(Throwable)`，在崩溃进程本地把 Throwable 转成：

```text
exceptionClassName
exceptionMessage
throwFileName/className/methodName/lineNumber
stackTrace String
crashTag
```

Binder 不会把任意 Throwable 对象图直接传到 system_server。

---

## 28. stackTrace 怎么生成

```java
StringWriter + FastPrintWriter
Throwable.printStackTrace(pw)
```

它包括顶层异常、cause、suppressed exception 等标准打印结构，然后经过 `sanitizeString()`。

AMS 拿到的是快照字符串；后续原 Throwable 对象变化不影响报告。

---

## 29. root cause 字段选择

构造器沿 `getCause()` 向下，选择最后一个拥有非空 stack 的 cause 作为 `rootTr`，并用最深层非空 message 更新 exceptionMessage。

所以：

```text
stackTrace 字符串：通常含完整 cause 链
exceptionClassName/throwMethod：偏向根 cause
exceptionMessage：偏向最深非空 message
```

字段不一定描述最外层抛出的包装异常。

---

## 30. 为什么选择根 cause

例如：

```text
RuntimeException("load failed")
  caused by IOException("disk full")
```

顶层说明业务阶段，根 cause 更接近直接故障机制。CrashInfo 用根 cause 填摘要字段，完整 stack 仍保留包装上下文。

分析时两者都要看。

---

## 31. 字符串大小限制

`sanitizeString()` 对超过 20 KiB 字符的字符串：

```text
保留开头 10 KiB
插入 [TRUNCATED N CHARS]
保留结尾 10 KiB
```

用于 stackTrace 和 exceptionMessage，避免 Binder Parcel、DropBox、日志与内存被异常大文本占满。

注意是 Java char 数，不等同于 UTF-8 字节数。

---

## 32. 为什么保留首尾而非只截尾

stack trace 开头通常包含直接 crash 线程和顶层 exception；尾部可能含最深 cause、suppressed 或 `... N more` 上下文。

保留首尾比只留前 20 KiB 更有机会同时看到入口与根 cause。

中间帧仍可能丢失，报告会明确标注截断字符数。

---

## 33. Parcel 大小调试检查

`writeToParcel()` 计算字段写入字节数；若 `Binder.CHECK_PARCEL_SIZE` 开启且超过 20 KiB，会打印详细 debug 信息。

这不是运行时硬拒绝阈值，真正 Binder 事务限制还受进程共享 buffer 和其他在途事务影响。

字符串预先截断是主要 guardrail。

---

## 34. AIDL 方法是同步的

```aidl
void handleApplicationCrash(
    in IBinder app,
    in ParcelableCrashInfo crashInfo);
```

没有 `oneway`，所以崩溃线程等待 AMS Binder 方法返回。

AppErrors 可能等待 crash dialog 结果，因此进程可以在 uncaught handler 中停留一段时间，而不是日志一出就立即消失。

---

## 35. 为什么同步等待有价值

让 AMS 有机会在进程仍存在、token/状态仍可用时：

- 完成 crash 记录。
- 更新 ProcessRecord。
- 决定 Activity/Task/Service 策略。
- 等用户做 Force Quit/Restart/Report 等选择。

最终 App 仍在 finally 自杀，AMS 不必仅依靠异步消息与瞬时死亡竞态。

---

## 36. 同步等待的风险

如果 system_server 卡住或 Binder 线程池耗尽，崩溃线程也会等待。应用其他线程仍可能运行，资源处于不一致状态。

最终并没有客户端侧显式 timeout；依赖 system_server 健康、Watchdog 和进程管理。

这也是 crash reporting 必须尽量稳健、避免持锁慢路径的原因。

---

## 37. DeadObjectException 分支

若 AMS/system_server 已死：

```java
if (t2 instanceof DeadObjectException) {
    // ignore
}
```

没有可报告对象时不再递归输出大量错误，直接进入 finally 杀自身。

App 进程通常也会因 system_server 重启和 zygote/系统恢复被清理。

---

## 38. 报告自身抛异常怎么办

catch `Throwable t2`，尽量通过 crash log 写：

```text
Error reporting crash
```

若连日志也抛，再吞掉 `t3`。无论如何 finally 执行。

这是失败路径中的失败路径：最重要的不变量是不要让已崩溃进程继续运行。

---

## 39. finally 为什么做两次退出

```java
Process.killProcess(Process.myPid());
System.exit(10);
```

`killProcess` 通常发送 SIGKILL，理论上不会返回；`System.exit(10)` 是后备手段，确保前一步异常/行为变化时仍退出。

它们不是两个顺序可观察的正常结束阶段，而是“尽一切办法结束”。

---

## 40. Java crash 的 Linux 退出原因

默认情况下 `killProcess(myPid)` 使进程因 SIGKILL 结束，`System.exit(10)` 往往来不及成为最终 status。

ApplicationExitInfo 的 Framework reason 由 AMS 预先记为 `REASON_CRASH`，不只依赖 kernel wait status。

Java exception 类型与 Linux 最终 signal 是两个不同层面的退出原因。

---

## 41. 为什么 Android App 不跑 shutdown hooks

`RuntimeInit.applicationInit()` 调用 native：

```java
nativeSetExitWithoutCleanup(true);
```

注释说明 Android App 无法做传统 JVM 式优雅 shutdown；shutdown hooks 关闭 Binder driver 等操作可能使残余线程在真正退出前异常。

App 应在生命周期/持久化边界主动保存，不应依赖 crash 时 shutdown hook。

---

## 42. System.exit 与 crash 的区别

业务主动 `System.exit(0)` 不会产生一个未捕获 Throwable，因此通常不走 LoggingHandler/KillApplicationHandler/CrashInfo。

RuntimeInit 配置让它直接退出而不跑完整清理。

```text
未捕获异常 → crash 报告 + finally 退出
主动 exit → 直接进程退出
```

ApplicationExitInfo 原因也可能不同。

---

## 43. AMS 如何找 App

`ActivityManagerService.handleApplicationCrash()`：

```java
ProcessRecord r = findAppProcess(app, "Crash");
String processName = app == null ? "system_server"
        : (r == null ? "unknown" : r.processName);
handleApplicationCrashInner("crash", r, processName, crashInfo);
```

Binder token 比自报 PID/package 更适合关联 AMS 已注册的进程对象。

---

## 44. 找不到 ProcessRecord 仍记录

`r == null` 时 processName 为 `unknown`，inner 方法仍可写 EventLog/stats/DropBox，再进入 AppErrors；AppErrors 因无 ProcessRecord 无法做 Activity/Service/UI 策略。

早期启动、竞态或非标准进程仍可能留下 crash 证据。

“无业务上下文”和“没有 crash”不是一回事。

---

## 45. Java 与 native 在 AMS 的 eventType

```text
Java uncaught："crash"
NativeCrashReporter："native_crash"
```

它们在 `handleApplicationCrashInner()` 汇合，后续共同写：

```text
AM_CRASH EventLog
APP_CRASH_OCCURRED Atom
DropBox
AppErrors
```

ApplicationExitInfo 再通过 CrashInfo class name 区分 native reason。

---

## 46. Java crash 没有 tombstone

普通 Java exception 已由 VM 保留对象和 Java stack，可直接构造 CrashInfo，无需 fatal signal、ptrace、vm snapshot、libunwindstack 和 `/data/tombstones`。

它通常有：

```text
crash log buffer
AMS crash DropBox
stats/EventLog/ExitInfo
```

但不会生成 native tombstone，除非 crash reporting/退出过程中另发生 native fault。

---

## 47. 主线程 crash

ActivityThread main thread 抛未捕获异常：

```text
main Looper 停止正常消息处理
当前 Activity 生命周期无法继续
KillApplicationHandler 同步报告 AMS
AMS 清理 task/activity，可能显示 UI
进程 finally 退出
```

UI 对话框由 system_server/SystemUI 窗口链显示，不依赖崩溃 App 主线程继续绘制。

---

## 48. 后台线程 crash

默认 handler 同样是进程级 killer，因此一个普通 worker 线程未捕获异常也会杀整个 App。

理由是共享 heap 状态可能已不一致，Framework 无法证明只结束该线程安全。

这与标准 Java “线程终止即可”的可配置能力不同，是 Android 默认策略选择。

---

## 49. Binder 线程抛 Java exception

r48 的服务端 `Binder.execTransactInternal()` 会捕获 `RemoteException | RuntimeException`。同步调用通常把异常写进 reply，oneway 调用只能记录日志；这两类不会因为“从 Stub 抛出”就自动进入 RuntimeInit 的进程级 crash 链。

`java.lang.Error`（例如 `AssertionError`/某些严重 VM 错误）不在上述 Java catch 中。native Binder 桥的 `report_java_lang_error()` 会尝试调用当前线程的 `dispatchUncaughtException()`；若该流程异常返回，native 端再走 fatal abort。

所以判断 Binder 服务是否会死，至少要区分 `RuntimeException/RemoteException`、`Error`，以及同步/oneway。不能笼统说“Binder 方法抛任何 Exception 都杀服务进程”，也不能笼统说“都会传回客户端”。

---

## 50. Looper callback 异常

Handler/Looper 默认不会为每个 Message 包一层业务 catch。`handleMessage()` 或 Runnable 抛 RuntimeException 可穿出 `Looper.loop()`，成为主线程未捕获异常。

因此：

```text
消息处理错误
 → main thread uncaught
 → RuntimeInit crash 链
```

异步 post 并不自动隔离异常。

---

## 51. Executor/Future 的差异

`Executor.execute(Runnable)` 中异常是否成为线程 uncaught，取决于 worker 实现；`FutureTask`/`submit(Callable)` 常把异常保存进 Future，由 `get()` 抛 `ExecutionException`。

若从不 `get()`，错误可能没有进入 RuntimeInit crash 链。

源码阅读要找线程池包装层，不能只看任务 body 抛了什么。

---

## 52. coroutine/Rx 等框架异常

高层并发库可能先捕获异常、路由到自己的 handler，再决定是否调用线程 uncaught handler。

Android 平台链只接收最终确实交给 `Thread.dispatchUncaughtException()` 的 Throwable。

业务框架“全局异常处理”与 RuntimeInit default handler 是不同层。

---

## 53. `ThreadDeath` 等 Throwable

handler 参数是 `Throwable`，不仅限 `Exception`。`Error`、`OutOfMemoryError`、某些 runtime fatal 状态也可进入。

所以 crash reporting 本身避免依赖大额分配；但 Java 链仍需构造 StringWriter/Parcel，OOM 时可能失败，最终 catch/finally 仍保证退出。

---

## 54. OOM crash 的局限

若 heap 极度不足：

- printStackTrace 分配失败。
- ParcelableCrashInfo 构造失败。
- Binder/日志分配失败。
- stopProfiling 失败。

平台只能 best effort，不能保证每次 OOM 都有完整 CrashInfo。

更早的 low-memory 监控、heap dump 或 stats 可能更有价值。

---

## 55. StackOverflowError 的局限

进入 uncaught handler 时栈已展开一部分，通常有空间运行 handler；但异常 cause/printStackTrace 仍可能复杂。

Java stack overflow 与 native stack guard SIGSEGV 的链不同：前者通常是 `StackOverflowError`，后者可能进入 debuggerd。

实际表现取决于 ART 能否安全构造/抛出 SOE。

---

## 56. 自定义 handler 的正确委托

若 App 只想额外记录，应保存旧 handler：

```java
UncaughtExceptionHandler old =
        Thread.getDefaultUncaughtExceptionHandler();
Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
    try {
        recordSmallCrashMarker(thread, error);
    } finally {
        if (old != null) old.uncaughtException(thread, error);
    }
});
```

自定义工作必须快速、有界、避免锁/I/O 死等，并最终委托平台 handler。

---

## 57. 为什么不能在 handler 中做网络上传

此时：

- 进程状态可能损坏。
- 主线程/锁可能不可用。
- 网络服务和 Binder 可能阻塞。
- AMS 正等待报告或系统正资源紧张。
- OS 随时结束进程。

更稳妥是写一个小型本地原子标记，下次冷启动由正常状态上传。

---

## 58. 为什么不能 sleep 后再退出

延迟退出让损坏状态继续服务用户、持有 Binder/锁和文件，可能造成 ANR、数据污染或二次 crash。

平台默认先同步完成必要系统协调，再立即 kill。

“让日志 SDK 多上传几秒”不能优先于进程一致性与系统恢复。

---

## 59. 为什么不能重启当前 Activity 来“恢复”

handler 所在线程可能是主线程，Looper 已经退出；共享单例、锁、数据库事务可能残缺。

应让进程死亡，由 AMS/用户从 Task 重新启动新进程。上一章的 Restart 正是进程级重建，不是在旧进程内 `startActivity()` 自救。

---

## 60. CrashInfo 的隐私边界

stack/message 可能含：

- 文件路径。
- URL/账号或业务 ID。
- SQL/网络响应片段。
- 用户输入。
- 内部类名与实现。

AMS/DropBox/错误报告 receiver 都必须受权限和隐私策略控制。截断解决大小，不自动脱敏。

---

## 61. CrashInfo 的可靠性边界

字段来自 Throwable API 和当前构建符号：

```text
混淆后类/方法名可能不可读
native method 行号为特殊值
动态代理/反射增加包装帧
cause 链可能人为循环或异常实现
自定义 Throwable.getMessage/printStackTrace 可有副作用
```

平台尽量捕获异常，但不能把任意恶意 Throwable 当完全可信数据源。

---

## 62. Java crash 的完成点

```text
Throwable 已穿出线程入口
pre-handler 已写 crash log
default handler 已开始
CrashInfo 已构造
Binder 请求已到 AMS
EventLog/stats/DropBox 已写
AppErrors 状态已更新
UI 已作决定
Binder 已返回
killProcess 已发送
AMS appDied cleanup 已完成
新进程/Task 已重新启动
```

“异常已捕获”通常只处于很早阶段。

---

## 63. Java crash 与 native crash 对照

| 维度 | Java uncaught | Native fatal |
|---|---|---|
| 最早入口 | ART/Thread dispatch | bionic signal handler |
| 原始证据 | Throwable + Java stack | siginfo/ucontext/process memory |
| 辅助进程 | 无 | crash_dump32/64 |
| ptrace/unwindstack | 无 | 有 |
| 正式原始文件 | 通常无 tombstone | `/data/tombstones/tombstone_XX` |
| AMS 通道 | Binder IActivityManager | `/data/system/ndebugsocket` |
| AMS payload | ParcelableCrashInfo | pid/signal + debuggerd 摘要 |
| 进程终止 | finally killProcess | 恢复默认并重发原 signal |
| AMS 汇合 | handleApplicationCrashInner("crash") | inner("native_crash") |

---

## 64. 两条链的退出状态差异

Native crash 努力保留原 `SIGSEGV/SIGABRT` 的 waitpid 语义；Java crash 默认通过 `killProcess()` 快速结束，Framework 另行记录 `REASON_CRASH`。

因此 Linux signal/status 不能单独区分所有上层 crash 原因。

ApplicationExitInfo、logcat、stats 和 tombstone 要联合查看。

---

## 65. system_server Java crash

流程仍从 RuntimeInit pre/default handler 开始，但：

```text
mApplicationObject 为 null
LoggingHandler 用 SYSTEM PROCESS 格式
向 AMS 的调用是进程内 Binder/local path 语义，且 AMS 本身可能处故障上下文
finally 杀 system_server
init/zygote/system_server 重启链恢复系统
```

不能依赖普通应用 crash dialog 完整执行。

---

## 66. mCrashing 对 system_server 尤其重要

system_server 有大量线程。一个线程 fatal 后，其他线程可能因共享状态/服务失败继续抛异常。

`mCrashing` 降低并发重复报告；最终进程级重启恢复整体状态。

Watchdog、RescueParty、PackageWatchdog 等处理的是不同重复故障尺度。

---

## 67. Java 工具进程

`am`、`pm` 等通过 RuntimeInit 启动的 Java 工具可能：

```text
mApplicationObject == null
uid 可能为 shell/root，而非 SYSTEM_UID
```

LoggingHandler 走普通 `FATAL EXCEPTION` 格式；AMS token 为空时可能被视作 system_server 的特殊名，这取决于是否进入 KillApplicationHandler/调用环境。

这类非 App 路径需按具体入口验证，不能完全套普通 ActivityThread。

---

## 68. 日志出现但 stats 没有

排障路径：

```text
pre-handler 是否运行？——有 FATAL EXCEPTION
normal/default handler 是否被 App 替换？
KillApplicationHandler 是否进入？
CrashInfo 构造是否 OOM/失败？
AMS Binder 是否 DeadObject/阻塞？
ProcessRecord 是否找到？
statsd/DropBox 写入是否启用？
```

第一层成功不证明后续都成功。

---

## 69. stats 有 crash 但用户没弹窗

上一章的 UI 条件仍适用：后台用户、设备不允许 dialog、首次 crash 开关、mute、throttle、instrumentation、free resize 或快速重复 crash 都可不显示。

AMS 在 UI 决策前已写 EventLog/stats/DropBox。

UI 是弱观察面，不应作为 crash 计数来源。

---

## 70. App 卡住很久才退出

可能停在同步 `handleApplicationCrash()`：

- AMS AppErrors 等 UI 结果。
- Binder 线程/锁拥塞。
- system_server 本身慢或卡住。
- ActivityController/报告链阻塞。

线程 dump 可看到 `KillApplicationHandler`/BinderProxy transact 栈。不要误判为原业务异常仍在执行。

---

## 71. Crash handler 自身再次 crash

`mCrashing=true` 后 LoggingHandler/kill handler 都尽量避免重入。需要注意一个细节：若第二次调用进入 `KillApplicationHandler`，它虽然在 try 中因 `mCrashing` 直接 return，Java 的 `finally` 仍会执行并强杀进程；这里的 return 只跳过重复报告，不会取消退出。

第一次 crash 的外层 catch 会捕获报告异常，finally 同样强杀。

若 App custom handler 自己递归调用旧 handler多次，也可能产生复杂行为。委托必须只做一次，并防自身异常。

---

## 72. 崩溃前日志与 crash stack 对齐

建议使用：

```text
wall timestamp
pid/tid/thread name
process start/sequence
exception root cause
Activity/Service 生命周期日志
Binder/trace 等待链
```

PID 可能复用，单看 PID 跨长时间关联不可靠。进程名、UID、start sequence 和时间窗口一起使用。

---

## 73. macOS 只读练习一：Thread dispatch

```bash
cd /Users/ninebot/androidSource

sed -n '2070,2210p' \
  libcore/ojluni/src/main/java/java/lang/Thread.java

sed -n '2500,2540p' art/runtime/thread.cc
```

画出 ART、pre-handler、线程 handler、ThreadGroup、default handler 的准确顺序。

---

## 74. macOS 只读练习二：RuntimeInit 两层 handler

```bash
cd /Users/ninebot/androidSource

sed -n '55,210p' \
  frameworks/base/core/java/com/android/internal/os/RuntimeInit.java

sed -n '245,275p' \
  frameworks/base/core/java/com/android/internal/os/RuntimeInit.java
```

分别标注“只记录”“报告 AMS”“防重入”“无条件退出”的代码行。

---

## 75. macOS 只读练习三：App token

```bash
cd /Users/ninebot/androidSource

sed -n '7325,7355p' \
  frameworks/base/core/java/android/app/ActivityThread.java

rg -n "setApplicationObject|findAppProcess\(" \
  frameworks/base/core/java/com/android/internal/os/RuntimeInit.java \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

解释 Binder token 如何从 App 进程注册到 AMS，以及 early crash 时可能缺少哪些上下文。

---

## 76. macOS 只读练习四：CrashInfo 截断

```bash
cd /Users/ninebot/androidSource

sed -n '330,510p' \
  frameworks/base/core/java/android/app/ApplicationErrorReport.java
```

构造一个包装异常 + 根 cause + 30 KiB message，手算摘要字段来自哪一层、最终保留哪些字符段。

---

## 77. macOS 只读练习五：同步 Binder

```bash
cd /Users/ninebot/androidSource

sed -n '100,110p' \
  frameworks/base/core/java/android/app/IActivityManager.aidl

rg -n "handleApplicationCrash\(|result.get\(|SHOW_ERROR_UI_MSG" \
  frameworks/base/core/java/com/android/internal/os/RuntimeInit.java \
  frameworks/base/services/core/java/com/android/server/am
```

画出崩溃线程等待 AMS、AMS UI Handler 等用户、结果沿 Binder 返回的时序。

---

## 78. macOS 只读练习六：Java/native 对照

```bash
cd /Users/ninebot/androidSource

rg -n "handleApplicationCrashInner\(" \
  frameworks/base/services/core/java/com/android/server/am

rg -n "activity_manager_notify|ndebugsocket" \
  system/core/debuggerd frameworks/base/services/core/java/com/android/server/am
```

从两条入口各追到 `AppErrors.crashApplication()`，标注汇合前后哪些步骤只属于某一类 crash。

---

## 79. 只读思想实验：自定义 handler

假设 App 保存 old default handler，然后测试三种实现：

```text
A：只写日志，不调用 old
B：finally 调用 old
C：先调用 old，再尝试继续上传
```

回答每种情况下：平台 pre-log、AMS report、App 自定义日志、进程退出、old 返回后的代码是否有机会执行。

---

## 80. 只读思想实验：两个线程同时 crash

时间线：

```text
T1 pre-handler → mTriggered=true
T2 pre-handler
T1 读 mCrashing=false
T2 也可能读 false
T1/T2 分别设 true
```

讨论 volatile 不能提供 CAS 的窗口，以及 AMS crash dialog去重、进程 kill 如何限制最终影响。

---

## 81. 第一遍复盘问题

1. ART 与 RuntimeInit 各负责什么？
2. Android 为什么有 pre-handler？
3. App 替换 default handler 后哪些平台动作仍会发生？
4. `mApplicationObject` 是什么，何时设置？
5. ParcelableCrashInfo 是否传整个 Throwable？
6. root cause 摘要与完整 stack 有何差异？
7. 为什么首尾各保留 10 KiB？
8. handleApplicationCrash 为什么同步？
9. finally 为什么同时 killProcess 与 System.exit？
10. Java crash 为什么没有 native tombstone？

---

## 82. 第二遍复读：易混点一——catch 与 uncaught handler

业务 `try/catch` 在栈展开途中恢复控制；uncaught handler 在异常已经逃出线程入口、该线程即将终止时执行。

handler 不是最后一个能跳回业务代码的 catch。

---

## 83. 易混点二——pre-handler 与 default handler

pre-handler 是 Android 平台的先行日志钩子；default handler 是 Java 标准可替换处理器。

App 替换后者不会自动替换前者，但会绕开默认 AMS report/kill，除非委托旧 handler。

---

## 84. 易混点三——线程 crash 与进程 crash

异常最初属于单线程；Android 默认 KillApplicationHandler 将其升级为整个进程终止。

自定义 handler 可改变进程终止，但无法让已展开的线程回到异常点。

---

## 85. 易混点四——Throwable 与 CrashInfo

Throwable 是崩溃进程内复杂对象；CrashInfo 是提取后的有限字符串/标量快照，可安全 Parcel 化。

对象 identity、任意自定义字段和完整 heap 不会跨 Binder。

---

## 86. 易混点五——20 KiB 字符与 Binder 字节

CrashInfo 首尾限制按 Java String char 计数；Parcel 使用 UTF-16/内部编码并带字段开销，实际字节不是恰好 20 KiB。

Binder 共享 buffer 也不是单个 CrashInfo 专属固定槽。

---

## 87. 易混点六——Binder 返回与进程死亡

同步 AMS 方法返回后才进入 finally kill；但 AMS/Binder death、异常会通过 catch 提前走 finally。

Binder 返回只表示报告调用结束，不表示 AMS 的全部异步死亡清理或重启已完成。

---

## 88. 易混点七——killProcess 与 crash 原因

Linux 最终看到 SIGKILL，不意味着根因是外部低内存杀进程。Framework 已记录这是 Java uncaught crash。

需要结合 ApplicationExitInfo、AM_CRASH 和 FATAL EXCEPTION，而不是只看 wait status。

---

## 89. 易混点八——WTF 与 uncaught crash

`RuntimeInit.wtf()` 报告严重错误，但是否退出由 AMS/system policy 返回值决定；未捕获异常默认 finally 无条件退出。

WTF 不是 Java exception 自然逃出线程边界的同义词。

---

## 90. “飞机黑匣子与紧急降落”类比

```text
Throwable              = 飞行中失控事件
ART                     = 发现控制流程已无法回到航线
pre-handler             = 黑匣子先记录告警
custom/default handler  = 运营方的紧急处置程序
ParcelableCrashInfo     = 压缩后的标准事故电报
Binder → AMS            = 向地面控制中心同步报告
AppErrors               = 地面决定清场、重启航班、通知用户
killProcess             = 放弃当前机体运行状态
新进程 Restart          = 换一架飞机重新执行任务
```

黑匣子记录完成不等于地面处置完成；不降落也不代表飞机恢复安全。

---

## 91. 本章结论

Android 11 的 Java crash 链把“记录”和“终止”故意拆开：

```text
ART：识别线程未捕获异常并调度 Java handler
Thread pre-handler：即使 App 替换 default，也尽力保留标准 crash log
KillApplicationHandler：防重入、保存 profiling、同步报告 AMS
ParcelableCrashInfo：将 Throwable 压缩成有界、可 Parcel 的事实快照
AMS/AppErrors：把线程异常翻译成进程、组件、用户和系统健康策略
finally：无论报告成功与否，结束不可信进程
```

最容易误解的是：看到 `FATAL EXCEPTION` 不等于默认 killer 已运行；App 自定义 handler 可以让进程暂存，但不能恢复已崩溃线程和共享状态；Binder 报告同步返回也不等于 appDied/重启已完成。

下一章将精读 `ApplicationExitInfo` 与 ProcessExitInfoTracker：系统如何跨进程死亡记录 exit reason、subreason、status、importance、PSS/RSS 和 trace FD，并让应用查询自己的历史退出原因。
