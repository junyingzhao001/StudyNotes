# 247 Android InputReader与InputDispatcher线程、Looper与锁顺序并发链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：两条输入线程怎样睡、醒、交接与安全退出

第 246 章已经把一笔事件从 `NotifyArgs` 追到 `mInboundQueue` 与 `mPendingEvent`。如果只看函数箭头，很容易把后续并发关系误读成“Reader 把事件发给 Dispatcher，Dispatcher 再交给 App”这一条串行流水线。真实系统里至少同时存在 InputReader、InputDispatcher、调用 Java Policy 的当前线程、更新窗口状态的外部线程、App 回写 FINISHED 的 socket，以及执行 Watchdog Monitor 的 `FgThread`。

本章不背线程名列表，而是围绕一个主问题展开：**共享状态由哪把锁提交，睡眠由哪个 fd 打断，回调在哪条线程同步执行，解锁期间失效的事实又怎样重新确认？** 先把七个常被混用的完成点分开：

| 完成点 | 已经能够证明 | 仍然不能证明 |
|---|---|---|
| `nativeInit()` 返回 | native 对象图与 Java 全局引用已建立，`addService(inputflinger)` 已被调用 | Binder 注册成功得到确认、Reader/Dispatcher 工作线程已启动 |
| `InputManager::start()` 返回 `OK` | 两个组件的 `start()` 都返回了 `OK` | r48 包装层没有吞掉底层 `Thread::run()` 失败 |
| `EventHub::getEvents()` 返回 | EventHub 因 RawEvent、显式 wake、timeout 或 reopen 请求回到 Reader | Mapper 已处理、事件已进入 Dispatcher |
| `enqueueInboundEventLocked()` 完成 | `EventEntry` 已在 Dispatcher 锁保护的入站账里 | Dispatcher 线程已经消费它 |
| `mLooper->wake()` 完成 | 写入成功，或写入当时已有饱和的未消费计数；该事实可能已合并或被并发取走 | 返回时 fd 仍可读、一次 wake 对应一次循环、Policy 不会阻塞 |
| `mDispatcherEnteredIdle` 被通知 | 本轮没有安排下一次主动推进时间 | 所有容器为空、所有 App 都已 FINISHED |
| `stop()` 正常返回 | 目标 `InputThread` 的退出等待已经结束 | App 侧处理、显示或其他外部工作也已结束 |

最稳定的阅读模型只有三句话：业务事实写在受锁保护的状态中；wake 只是门铃；有重入或长阻塞风险的 Reader/Dispatcher 外部同步调用，通常在加锁前发生，或保存参数后解锁、回来再验证。Classifier 包装层持自身锁下传，以及接口明示 non-reentrant 的 Policy 检查，是必须单独核对的窄例外。

## 2. 一个进程、两个 native 工作线程、两个不相同的 Looper

`SystemServer` 在 `system_server` 进程里构造 `InputManagerService`。Java 构造函数把 `DisplayThread` 的 Looper 交给 `InputManagerHandler`，又把这条 Looper 的 `MessageQueue` 传入 `nativeInit()`。JNI 取出对应 native Looper，构造 `NativeInputManager`；后者在同一进程创建 native `InputManager`，并以 `inputflinger` 为名字注册 Binder service。

服务名不能用来推断进程边界。这里没有因为出现 `inputflinger` 名字就再启动一个 Linux daemon；对象的创建栈仍在 `system_server`。`NativeInputManager` 调用 `addService()` 尝试注册这个名字，却没有检查返回值，所以 `nativeInit()` 也不是注册成功的确认点。native `InputManager` 再依次装配 `InputDispatcher → InputClassifier → InputReader`，Classifier 只是 listener 链中的包装层，不是第三条事件派发线程。

还要区分两套 Looper：

| 所有者 | 来源或等待器 | 主要用途 | 执行线程 |
|---|---|---|---|
| `InputManagerService.mHandler`、`NativeInputManager.mLooper` | Java `DisplayThread` 的 MessageQueue/Looper | 设备变化消息、pointer/sprite 等显示侧工作 | `DisplayThread` |
| `InputDispatcher.mLooper` | Dispatcher 构造函数独立 `new Looper(false)` | wake eventfd、InputChannel fd、调度 timeout | `InputDispatcher` |
| `InputReader` | 不使用上述任一 Looper；直接调用 `EventHub::getEvents()` | input/inotify/video fd 与 wake pipe | `InputReader` |

所以“都在 system_server”只说明地址空间相同，“都是 Looper”也不说明队列相同。InputDispatcher 的 fd callback 不会跑到 DisplayThread，DisplayThread 上的 Handler 消息也不会被 Dispatcher 的 `pollOnce()` 消费。

### 练习 1：给五个对象标出唯一的进程、线程与等待器

设系统已经完成 `nativeInit()`，尚未调用 `start()`；随后创建 pointer controller、启动两条输入线程，并收到一个 App FINISHED。问：`inputflinger` service、pointer controller 使用的 Looper、内核 RawEvent 等待、FINISHED callback、IMS 的普通 Handler 消息分别属于哪里？

唯一答案是：service 对象仍在 `system_server`；pointer controller 使用传入的 DisplayThread Looper；RawEvent 由 InputReader 在线程自己的 EventHub epoll 上等待；FINISHED 由 InputDispatcher 线程自己的 Looper fd callback 处理；IMS Handler 消息仍由 DisplayThread 执行。这里不存在名为 `inputflinger` 的独立进程，也不存在 Reader 与 Dispatcher 共用的一条 Looper。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'inputManager = new InputManagerService(context);' frameworks/base/services/java/com/android/server/SystemServer.java
grep -n -F 'this.mHandler = new InputManagerHandler(DisplayThread.get().getLooper());' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'mPtr = nativeInit(this, mContext, mHandler.getLooper().getQueue());' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'messageQueue->getLooper());' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'mInputManager = new InputManager(this, this);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'defaultServiceManager()->addService(String16("inputflinger"),' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'mDispatcher = createInputDispatcher(dispatcherPolicy);' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mLooper = new Looper(false);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 3. 启动顺序有回滚意图，但 r48 没有完整传递线程创建失败

Java 构造阶段只建立对象；`InputManagerService.start()` 调用 `nativeStart()` 后，native `InputManager::start()` 才先发起 Dispatcher 线程创建、再发起 Reader 线程创建。若 `mDispatcher->start()` 显式报错，函数立即返回；若 Dispatcher 成功而 `mReader->start()` 报错，代码调用 `mDispatcher->stop()` 回滚。这个顺序先表达消费者优先，却不是 ready barrier：即使底层 `Thread::run()` 返回成功，也不等待 Dispatcher 第一次 `threadLoop()` 已经执行，Reader 仍可能先真正跑到 flush；正确性还依赖 inbound 状态可积累、eventfd 门铃可保持。

但还必须再追一层。Reader 与 Dispatcher 的 `start()` 都只是创建一个 `InputThread` 后返回 `OK`；`InputThread` 构造函数立即调用 `mThread->run(...)`，却没有检查、保存或上报返回值。`utils::Thread::run()` 在底层创建失败时会返回 `UNKNOWN_ERROR`，在创建成功时返回的 `OK` 也只保证线程被成功启动，不保证 `readyToRun()` 或 loop 后续执行成功。因此上层的回滚分支表达了正确意图，却接不到这条包装层吞掉的创建错误。

停止采用生产者优先的反向顺序：`InputManager::stop()` 先 Reader、后 Dispatcher，而且 Reader 停止失败后仍会继续尝试 Dispatcher。组件自己的 `stop()` 先拒绝由本线程调用，再用 `mThread.reset()` 触发 `InputThread` 析构。这里没有单独的 lifecycle mutex；`start()`、`stop()` 与 owner 析构必须由外部串行化。若从任一输入工作线程误调 `InputManager::stop()`，self-join 防线只保住当前线程，另一条线程仍可能已经停掉，形成半停止状态。

对象析构也依赖这项前提。`InputManager::~InputManager()` 先调用 `stop()`；这是标准 owner 路径。`InputReader` 自身析构为空，`InputDispatcher` 自身析构会清路由状态但不会先停止线程；不能把 C++ 成员的逆序析构当作安全的替代协议，更不能并发调用 start/stop。

### 练习 2：推演一次被包装层吞掉的 Reader 创建失败

设首次启动时 Dispatcher 的底层线程创建成功，Reader 的 `utils::Thread::run()` 返回 `UNKNOWN_ERROR`，没有其他错误。问 `InputReader::start()`、`InputManager::start()`、JNI `nativeStart()` 各看到什么，Dispatcher 回滚会不会执行？

唯一答案是：`InputThread` 构造函数丢弃 `run()` 的错误，`InputReader::start()` 仍返回 `OK`；`InputManager::start()` 因而也返回 `OK`，不会进入 Reader 失败分支，JNI 不抛“could not be started”异常。结果可能是 Dispatcher 活着而 Reader 没有运行。这个结论只描述 r48 的错误传播缺口，不表示正常启动顺序失效。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'result = mDispatcher->start();' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'result = mReader->start();' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mDispatcher->stop();' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'status_t result = mReader->stop();' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'result = mDispatcher->stop();' frameworks/native/services/inputflinger/InputManager.cpp
grep -n -F 'mThread->run(mName.c_str(), ANDROID_PRIORITY_URGENT_DISPLAY);' frameworks/native/services/inputflinger/InputThread.cpp
grep -n -F 'if (res == false) {' system/core/libutils/Threads.cpp
grep -n -F 'return UNKNOWN_ERROR;' system/core/libutils/Threads.cpp
grep -n -F 'return OK;' frameworks/native/services/inputflinger/reader/InputReader.cpp
```

## 4. InputThread既是重复循环壳，也是退出协议与JNI能力的承载者

Reader 与 Dispatcher 都把一个无参 lambda 交给 `InputThread`：前者每次调用 `loopOnce()`，后者每次调用 `dispatchOnce()`。内部 `InputThreadImpl::threadLoop()` 执行一次 lambda 后固定返回 `true`；`utils::Thread::_threadLoop()` 因此继续下一轮，并在每轮结束处观察 `mExitPending`。线程名分别是 `InputReader`、`InputDispatcher`，创建优先级都是 `ANDROID_PRIORITY_URGENT_DISPLAY`。

`InputThreadImpl` 以 `Thread(/* canCallJava */ true)` 构造。`Thread::run()` 因而调用 `createThreadEtc`，而不是 raw 创建函数；AndroidRuntime 注册 native 方法时把进程级创建钩子设为 `javaCreateThreadEtc`，其 shell 在进入真正 loop 前 `javaAttachThread()`，退出后 detach。这使两条 native 输入线程能够取得当前线程的 `JNIEnv` 并同步进入 Java Policy，但不会把它们迁移到 Java main、DisplayThread 或任意 Handler Looper。

析构协议是三步：先 `requestExit()` 写退出标志，再调用专属 wake 函数打断可能的无限等待，最后 `requestExitAndWait()` 等到 `_threadLoop()` 结束。Reader 的 wake 指向 `EventHub::wake()`，Dispatcher 的 wake 指向 `Looper::wake()`。只写退出标志不能叫醒 `epoll_wait()`；只写 wake fd 又不会令循环决定退出，所以两步缺一不可。

这套协议不是抢占式取消。wake 只能打断将要发生或正在发生的 fd 等待，不能中止一段正在运行的 JNI/Policy 回调、不能强夺别的 mutex，也没有超时。若 Dispatcher 正同步调用一个长期不返回的 Java Policy，`requestExitAndWait()` 就会等到该调用返回、`dispatchOnce()` 收尾并回到线程壳观察退出标志。`stop()` 的 self-thread 检查正是为了避免确定性的自等待死锁。

### 练习 3：同时停止一个在 epoll 中的 Reader 和一个卡在 Policy 的 Dispatcher

设 t0 时 owner 在线程 C 调用正常的 `InputManager::stop()`：Reader 正睡在 EventHub epoll，且规定它在 wake 后没有额外事件或回调、于 t0+1 ms 前结束；Dispatcher 恰在 t0 刚进入一个固定持续 80 ms 的锁外 Java Policy 回调。因此 owner 会在该 Policy 返回前进入 `InputDispatcher::stop()`。问两个 wake 各能立即改变什么，线程 C 能否在 Policy 返回前结束 stop？

唯一答案是：Reader 的 pipe write 可立即使 EventHub epoll 返回，Reader 完成本轮后看到退出请求；Dispatcher 的 eventfd write 只能保证它以后进入 `pollOnce()` 时不会睡住，不能抢占当前 Java 回调。线程 C 先等 Reader 结束，再等 Dispatcher 的回调与当前 `dispatchOnce()` 收尾、线程壳观察退出标志，所以不可能在 t0+80 ms 的 Policy 返回点之前结束 stop。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F ': Thread(/* canCallJava */ true), mThreadLoop(loop) {}' frameworks/native/services/inputflinger/InputThread.cpp
grep -n -F 'mThreadLoop();' frameworks/native/services/inputflinger/InputThread.cpp
grep -n -F 'if (mCanCallJava) {' system/core/libutils/Threads.cpp
grep -n -F 'res = createThreadEtc(_threadLoop,' system/core/libutils/Threads.cpp
grep -n -F 'javaAttachThread(name, &env)' frameworks/base/core/jni/AndroidRuntime.cpp
grep -n -F 'androidSetCreateThreadFunc((android_create_thread_fn) javaCreateThreadEtc);' frameworks/base/core/jni/AndroidRuntime.cpp
grep -n -F 'mThread->requestExit();' frameworks/native/services/inputflinger/InputThread.cpp
grep -n -F 'mThreadWake();' frameworks/native/services/inputflinger/InputThread.cpp
grep -n -F 'mThread->requestExitAndWait();' frameworks/native/services/inputflinger/InputThread.cpp
```

## 5. Reader一轮有四个锁区边界，设备通知还排在事件flush之前

`InputReader::loopOnce()` 的准确结构不是“持 Reader 锁读取并处理”，而是四段：

| 阶段 | Reader `mLock` | 发生的工作 |
|---|---|---|
| A：准备 | 持有 | 记录 generation，消费配置刷新请求，计算 `timeoutMillis` |
| B：等待 | 不持有 | 调 `EventHub::getEvents()`，由 EventHub 自己管理另一把锁与 epoll |
| C：归并 | 持有 | 广播 Reader alive，处理 RawEvent 与 mapper timeout，必要时复制设备列表快照 |
| D：外发 | 不持有 | 先通知 policy 设备列表变化，再 `mQueuedListener->flush()` |

Mapper 在 C 段修改设备、按键、指针、全局 meta、generation 与 timeout 等 Reader 内部状态，所以必须共享 Reader 锁。外部查询接口也用这把锁读一致状态。B 段可能无限睡眠，D 段可能同步进入 Classifier、Dispatcher、JNI 与 WindowManager；把两者放在锁外，才能让其他线程查询 Reader，并切断 `Reader → Dispatcher/Policy/WMS → Reader` 的回环。这里只说主循环的 `getEvents()` 边界；Reader 的配置、设备查询与 dump 等其他锁内路径仍可能再调用 EventHub 方法，形成短暂的 `Reader mLock → EventHub mLock`。

设备列表通知与事件队列不是一个总 FIFO。若 C 段同时发现 generation 改变并排入多笔 NotifyArgs，D 段先同步进入 `InputManagerService.notifyInputDevicesChanged()` 更新快照并 post Handler 消息，然后才按 Queued listener 顺序 flush 各笔事件。两种入口都由同一 InputReader 线程发起，但真正的设备 listener delivery 在 DisplayThread；它甚至可以与后续 flush 并发，不能由 post 顺序推出“所有设备监听者先收到，再有 K/M 进入 Dispatcher”。

Reader Policy 还有独立契约：接口声明这些方法不会重入 InputReader，所以配置读取、pointer controller 获取等少数调用可以出现在 Reader 锁内。不能把“Policy 一律锁外”从 Dispatcher 机械套过来；应先读每个接口自己的重入承诺。

### 练习 4：给配置刷新、两笔事件与设备快照排唯一时间线

设 A 段看到配置刷新请求并把 timeout 设为 0；B 段取得足量 RawEvent；C 段依次让 Mapper 排入 Key K、Motion M，并使 generation 改变。本题把“设备列表回调”限定为同步进入 JNI/IMS 的 `notifyInputDevicesChanged()`，不把随后 Handler 的 listener delivery 混进来。问 alive、设备快照、该同步回调、K、M 的先后与 Reader 锁状态。

唯一答案是：A 解锁 → B 取事件 → C 加锁并先广播 alive、处理出 K/M、复制设备快照 → C 解锁 → 同步 JNI/IMS 设备回调 → flush K → flush M。后三次入口都仍由 InputReader 线程执行且不持 Reader 锁；B 段也不持 Reader 锁，但 EventHub 会短暂使用自己的 `mLock`。DisplayThread 何时真正 delivery 取决于调度，不能塞进这条固定顺序。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'oldGeneration = mGeneration;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'timeoutMillis = -1;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'size_t count = mEventHub->getEvents(timeoutMillis, mEventBuffer, EVENT_BUFFER_SIZE);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mReaderIsAliveCondition.broadcast();' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'processEventsLocked(mEventBuffer, count);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'getInputDevicesLocked(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mPolicy->notifyInputDevicesChanged(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mQueuedListener->flush();' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'all calls to the listener must happen from the same thread.' frameworks/native/services/inputflinger/include/InputReaderBase.h
```

## 6. EventHub用自己的epoll与非阻塞pipe，把等待信号和业务事件明确分开

EventHub 构造时创建 epoll、inotify fd 和一对非阻塞 pipe。输入设备 fd、视频设备 fd、inotify 与 pipe 读端进入同一个 epoll；Reader 无需再套一层 Looper。`getEvents()` 先持 EventHub 的 `mLock` 处理设备表、上次 epoll 返回的 pending items 与读取结果，在真正 `epoll_wait()` 前显式解锁，返回后再加锁。因此一条空闲 Reader 同时不占 Reader 锁和 EventHub 锁。

`getEvents()` 也不保证每次返回都经过 epoll。若 `mNeedToReopenDevices` 已置位，它会先关设备、安排下一轮重新扫描，然后直接 break，返回量可以为 0；设备移除、新增与扫描完成事件留给后续轮次报告。因此“函数返回”只能证明 Reader 获得一次重新检查机会，不能反推本轮一定有 fd、wake 或 timeout。

`EventHub::wake()` 向 pipe 写端写一个字节 `W`。读端就绪后，`getEvents()` 把积累字节读空并设置局部 `awoken=true`，但不会据此构造 Key、Motion 或任何 RawEvent；若没有业务事件，`awoken` 本身也足以让本轮返回，Reader 随后重新检查配置、timeout 或退出标志。写端因非阻塞而返回 `EAGAIN` 时也无需补救：pipe 已满就已经保持可读，门铃事实没有丢失。一个故障例外值得单列：`epoll_wait()` 若以非 `EINTR` 错误返回，源码已经重新取得 EventHub 锁，再持锁 `usleep(100 ms)` 退避；“睡眠不持 EventHub 锁”只适用于正常 epoll 等待，不是所有错误分支。

这套机制还承担电源语义。输入 fd 以 `EPOLLWAKEUP` 注册，使从 epoll 报告事件到下一次针对该 fd 进入 `epoll_wait()` 的处理窗口维持内核唤醒保护。它不是 Java `PowerManager.WakeLock` 对象，也不意味着 timeout 会叫醒已经睡眠的设备。源码明确把 EventHub timeout 称为 advisory：设备休眠时不会只为这个软件 deadline 唤醒系统。

### 练习 5：三次wake与一笔内核事件会返回几笔RawEvent

设 Reader 已睡在 epoll，pipe 有足够容量；三个线程先后各写一次 wake，随后一个 input fd 产生恰好一笔事件。epoll 本轮同时交回 wake fd 与 input fd，输出 buffer 足够大。问 `getEvents()` 会不会返回四笔 RawEvent，是否必须执行三轮 Reader loop？

唯一答案是：业务返回量只有那一笔 input RawEvent；三个 `W` 只让同一个 fd 保持可读，读端会把积累字节排空并记录本轮被显式唤醒。一次就绪足以打断等待，不承诺 wake 次数与 Reader 轮数一一对应。事件与 wake item 在 epoll 数组中的先后未定义，但在容量充足的设定下都会在本轮 pending items 中被处理。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'result = pipe(wakeFds);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (mNeedToReopenDevices) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'eventItem.data.fd = mWakeReadPipeFd;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (eventItem.data.fd == mWakeReadPipeFd) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'nRead = read(mWakeReadPipeFd, buffer, sizeof(buffer));' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (event != buffer || awoken) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mLock.unlock(); // release lock before poll' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'int pollResult = epoll_wait(mEpollFd, mPendingEventItems, EPOLL_MAX_EVENTS, timeoutMillis);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'nWrite = write(mWakeWritePipeFd, "W", 1);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (nWrite != 1 && errno != EAGAIN) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'eventItem.events = EPOLLIN | EPOLLWAKEUP;' frameworks/native/services/inputflinger/reader/EventHub.cpp
```

## 7. flush仍在Reader线程；真正的线程交接是入站状态提交加Looper门铃

`QueuedInputListener::flush()` 没有工作线程，也没有 Looper。它在 InputReader 线程逐项调用 `args->notify(mInnerListener)`；标准装配下链路是 `QueuedInputListener → InputClassifier → InputDispatcher`。MotionClassifier 与 HAL 通信另有异步线程，但那条线程只更新分类结果，不承担原事件向 Dispatcher 的投递。

所以硬件 Key/Motion 进入 `InputDispatcher::notify*()` 时，当前线程通常仍是 InputReader。`intercept*BeforeQueueing` 在取得 Dispatcher 锁前同步执行；可选 Filter 的 native/IMS 入口也在 notify 调用线程同步进入，但 `IInputFilter.filterInputEvent()` 是 oneway，常见 `InputFilter` 实现会再向指定 Handler 投递，真正的 `onInputEvent()` 属于后续线程阶段。Filter 接走时原事件不会建立 EventEntry，以后带 FILTERED 标志回送才形成另一条注入链。直到未被接走的事件构造 EventEntry，在 `mLock` 下压入 `mInboundQueue`，再解锁并按 `needWake` 写 Looper eventfd，生产者才完成可见的交接。

Motion 还带着一把容易漏画的锁：`InputClassifier::notifyMotion()` 无论当前有没有可用的 MotionClassifier，都会先持自己的 `mLock`，并跨过下游 `mListener->notifyMotion()`；DeviceReset 也采用同样的外层锁。Reader 锁已经释放不等于整条调用链无锁；只有分类功能可用时才另外把 HAL 计算交给专用线程，那条线程也不等于 Filter Handler 或 Dispatcher 线程。

`enqueueInboundEventLocked()` 的默认规则是：入队前队列为空才需要 wake。App-switch Key UP 与能够解阻旧积压的 pointer DOWN 还会强制 wake。这个布尔量不是“事件重要程度”，也不能解释成“非空队列时 Dispatcher 必然正在 CPU 上运行”；它只是当前状态机维护的唤醒条件。

消费侧交接真正发生在 InputDispatcher 线程下一次执行 `dispatchOnce()` 并取得 `mLock` 时。稳态空闲路径通常是 `pollOnce()` 因 wake 返回、线程壳再调用 `dispatchOnce()`；但线程首次启动可以直接进入第一次 `dispatchOnce()`，不要求此前已经 poll。共享对象的内容由同一 `mLock` 的 unlock/lock 建立发布与读取关系，eventfd 不携带 EventEntry 指针，只负责让消费者重新查看受锁保护的账本。

### 练习 6：一次flush只有一个wake，为什么两笔事件都不会丢

设 Dispatcher 入站队列起初为空，未安装 InputFilter；从 Reader 开始 flush 到题目观察点，InputDispatcher 线程完全未获调度，既不执行 `awoken()` 也不进入 `dispatchOnce()`。Reader 依次 flush 普通 Key K 与普通 Motion M，二者都不是强制 wake 的特殊情形。问两次 `enqueueInboundEventLocked()` 的 `needWake`、eventfd 状态和最终队列是什么？

唯一答案是：K 入队前为空，所以返回 true，解锁后写一次 eventfd；M 入队前已有 K，所以返回 false，不再写。最终受锁保护的队列是 `[K, M]`，eventfd 至少保持一次未消费的可读事实。Dispatcher 醒来后按队列事实继续处理；一次门铃足以提示多笔工作，门铃个数从来不是工作项计数。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'void QueuedInputListener::flush() {' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'args->notify(mInnerListener);' frameworks/native/services/inputflinger/InputListener.cpp
grep -n -F 'mListener->notifyKey(args);' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'void InputDispatcher::notifyKey(const NotifyKeyArgs* args)' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->interceptKeyBeforeQueueing(&event, /*byref*/ policyFlags);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'needWake = enqueueInboundEventLocked(newEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'bool needWake = mInboundQueue.empty();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInboundQueue.push_back(entry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLooper->wake();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mListener->notifyMotion(&newArgs);' frameworks/native/services/inputflinger/InputClassifier.cpp
grep -n -F 'oneway interface IInputFilter {' frameworks/base/core/java/android/view/IInputFilter.aidl
grep -n -F 'mH.obtainMessage(MSG_INPUT_EVENT, policyFlags, 0, event).sendToTarget();' frameworks/base/core/java/android/view/InputFilter.java
grep -n -F 'mWakeEventFd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));' system/core/libutils/Looper.cpp
grep -n -F 'mLooper->addFd(fd, 0, ALOOPER_EVENT_INPUT, handleReceiveCallback, this);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'int callbackResult = response.request.callback->handleEvent(fd, events, data);' system/core/libutils/Looper.cpp
grep -n -F 'receiveFinishedSignal(&seq, &handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 8. Dispatcher的Looper把eventfd、InputChannel回执与deadline汇到同一线程

InputDispatcher 独立构造 `Looper(false)`。参数 `false` 表示不接受没有 callback 的 fd 注册；Connection 注册时传入 `handleReceiveCallback`，Looper 因而把 ident 规范为 `POLL_CALLBACK`。这条 Looper 在 r48 的 Dispatcher 用法里只直接接三类推进理由：自身 wake eventfd、每条服务端 InputChannel fd 的输入/错误/挂断，以及 `dispatchOnce()` 传给 `pollOnce()` 的 timeout。Dispatcher 没有用它投递另一套 Message 工作队列。

Looper 的 wake fd 是非阻塞 `eventfd`。每次 `wake()` 写一个 64 位增量，`awoken()` 一次 read 取走累计计数；多个 wake 可以合并为一次可读回报。EventHub 的 pipe 保存字节流，Looper 的 eventfd 保存计数，数据结构不同：未消费时它们保持可读；若已经被正在收尾的 poll 取走，该次 poll 本身已经提供推进机会。上层只把它们当作可合并门铃，不按次数计算工作；两者也都不传业务对象。

`pollInner()` 在 epoll 返回后取得 Looper 内部锁，把就绪 fd 转成 response；处理完消息后先释放 Looper 锁，再逐一执行 fd callback。于是 `handleReceiveCallback()` 虽然会在同一 InputDispatcher 线程取得 Dispatcher `mLock`、批量读取 FINISHED、更新 waitQueue 并运行后处理 Command，却不是在“Looper 锁 → Dispatcher 锁”的嵌套里执行。wake eventfd 本身没有 Dispatcher callback：Looper 只用 `awoken()` 读掉计数，`pollOnce()` 返回后，要等线程壳下一次调用 `dispatchOnce()` 才推进 inbound。若 wake fd 与 InputChannel fd 同批就绪，FINISHED callback 可在当前 `pollInner()` 内先跑，新的 inbound 则留到下一轮；二者没有唯一的“wake 后先取 inbound”顺序。

反方向确实存在短边：注册或移除 InputChannel 时，Dispatcher 代码可在持 `mLock` 时调用 `Looper::addFd/removeFd`，后者短暂取得 Looper 锁。但 callback 前释放 Looper 锁避免了反向长边；callback 返回 1 表示继续监听，返回 0 才让 Looper 按 fd 与 sequence 移除该注册，它不是“本事件是否 handled”的回执值。r48 的注册路径还没有检查 `addFd()` 返回值：若 epoll 注册失败，Connection map 已写入而外层仍可能返回 `OK`，这是故障诊断时必须认识的非事务边界；sequence 检查则防止 callback 返回后误删 fd 复用产生的新注册。

## 9. dispatchOnce先延续Command，再合并ANR期限，最后才无锁poll

每次 `dispatchOnce()` 先令 `nextWakeupTime=LONG_LONG_MAX`，然后在 Dispatcher 锁内按固定顺序推进：

1. 通知 `mDispatcherIsAlive`，表示线程已经到达新一轮锁区。
2. 只有 commandQueue 为空时，才运行新的 `dispatchOnceInnerLocked()`；已有 Command 会优先延续旧状态机。
3. 把 commandQueue 一直执行到空；只要执行过 Command，就把下一次唤醒时间强制为 `LONG_LONG_MIN`。
4. 调 `processAnrsLocked()`，把最近的无焦点或 Connection ANR deadline 与当前时间取最小值。
5. 若最终仍为 `LONG_LONG_MAX`，通知 `mDispatcherEnteredIdle`。
6. 解锁，把绝对时间换算成向上取整的毫秒，再调用 `mLooper->pollOnce()`。

三个时间值要按语义读：`LONG_LONG_MIN` 已早于当前时刻，换算成 0，下一轮不阻塞；普通绝对纳秒 deadline 换成向上取整的毫秒，避免提前醒来反复空转；`LONG_LONG_MAX` 的差值超过可表达范围，换成 -1，无限等待 fd。窗口变化、FINISHED、显式 wake，以及 `enqueueInboundEventLocked()` 返回 `needWake=true` 的新 entry，都可能早于既定 timeout 打断 epoll；普通 entry 在已有 inbound 且没有强制条件时不会额外 wake。

idle 通知只看最终时间。正常未冻结且没有 pending/inbound/Command/deadline 时，它通常与“无事可做”一致；但 `mDispatchFrozen` 会让 inner 直接返回，不处理已有 inbound，也不登记其推进时间。如果同时没有 Command 与 ANR deadline，最终仍可得到 MAX、通知 idle 并无限 poll。这就是为什么 idle 不能定义成“所有队列严格为空”。

### 练习 7：计算三轮poll timeout与idle结果

设换算参考时刻均为 T。A 轮没有 Command，inner 给出 `T+2.1 ms`，最近 ANR 为 `T+5 s`；B 轮 inner 新增一个 Command，该 Command 已执行完成，ANR 仍在未来；C 轮分发 frozen，inbound 非空，但没有 Command、ANR、repeat 或其他 deadline。分别问 timeout 与 idle 通知。

唯一答案是：A 轮取较早的 2.1 ms，并向上取整为 3 ms，不通知 idle；B 轮因为执行过 Command 设为 MIN，timeout 为 0，不通知 idle；C 轮 inner 立即返回且其他来源仍为 MAX，通知 idle，timeout 为 -1，队列却仍非空，直到后续状态变化或 wake 才重查。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'nsecs_t nextWakeupTime = LONG_LONG_MAX;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!haveCommandsLocked()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (runCommandsLockedInterruptible()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'nextWakeupTime = LONG_LONG_MIN;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const nsecs_t nextAnrCheck = processAnrsLocked();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (nextWakeupTime == LONG_LONG_MAX) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (timeoutTime > referenceTime) {' system/core/libutils/Timers.cpp
grep -n -F '(timeoutDelay + 999999LL) / 1000000LL;' system/core/libutils/Timers.cpp
```

## 10. wake是可合并的门铃，互斥锁才是共享状态的发布协议

一个可靠的 producer 操作通常按“锁内改状态 → unlock → wake”排列。若没有一轮正在收尾并准备读取 wake fd，epoll 前的写会让 eventfd 保持可读，等待期间的写会让 epoll 返回；若上一轮 poll 已拿到 wake fd、尚未执行 `awoken()`，新写也可能并入这次累计 read，而当前 poll 本身已经获得返回理由；read 完成后的新写则留给下一轮。多名 producer 的计数可以累积并被一次 read 消费。消费者随后重新取得 Dispatcher 锁，从队列、窗口、Connection 与 deadline 读取唯一事实。因此不会因为“两个事件只有一个 callback”就丢工作，也不能因看见三次 wake 就推断执行三轮。

内存可见性同样不能寄托在门铃上。生产者在 `mLock` 下写入 EventEntry 或窗口状态，unlock 发布；Dispatcher 醒来再 lock 获取。pipe/eventfd 的职责是避免消费者无限睡眠，不是替代 mutex，也不为未受锁保护的 C++ 对象建立一套新的所有权协议。

数据路径常把 wake 放到解锁之后，让刚醒的线程更可能直接取得锁，减少“醒来又堵锁”的往返。但“持锁 wake”不是自动死锁。`monitor()` 与 `waitForIdle()` 都在持 Dispatcher 锁时写 eventfd，随后 condition wait 原子释放这把锁；`Looper::wake()` 本身不取 Dispatcher 锁。判断安全性必须同时看 wake 的实现与下一步是否释放锁，不能只按表面顺序套规则。

反过来，wake 也不是抢占器。线程若卡在 Policy、JNI、Binder、mapper 计算或等待另一把锁，fd 变得可读只会让它未来的 poll 不睡；无法让当前调用栈跳回循环。这一点同时解释了 stop 为什么可能长等，以及 Watchdog 为什么只能从旁检测而不能替它完成取消。

## 11. 四把核心锁保护四份账，源码靠断开跨组件锁边而不是虚构全局总序

本章主线至少有四把互不等价的锁：

| 锁 | 保护的事实 | 长等待边界 | 外部调用边界 |
|---|---|---|---|
| Reader `mLock` | device/mapper、meta、generation、配置与 timeout | 调 EventHub 前释放 | listener 与设备变化通知前释放；Reader Policy 有不重入例外 |
| EventHub `mLock` | fd 到 Device 映射、扫描队列、pending epoll items | `epoll_wait()` 前释放 | 主要在 EventHub 内部闭合 |
| Dispatcher `mLock` | inbound/pending/command、窗口/焦点/触摸、Connection 队列、ANR 与注入账 | `pollOnce()` 前释放；condition wait 会原子释放 | 普通 Policy 回调前释放并在回来后重验 |
| Looper `mLock` | fd requests、message envelopes 与 epoll rebuild 状态；poll responses/index 是单一 poller 私有状态 | epoll 睡眠不持有 | message/fd callback 前释放 |

`GUARDED_BY(mLock)` 与 `REQUIRES(mLock)` 是线程安全注解，告诉静态分析器和读者哪份状态属于哪把锁，不会在运行时自动加锁。普通 `*Locked` 假定从进入到返回保持 Dispatcher 锁；`*LockedInterruptible` 则允许中途解锁调用 Policy、再重新加锁。接口头还明确限制调用方向：Interruptible 可以调用 Locked，反向不允许，否则上层会误以为不变量从未暴露给并发修改。

系统里还有 `NativeInputManager.mLock`、Java `mInputFilterLock`、WindowManager 锁、Binder 内部锁和 App 端锁。r48 没有给它们规定一张简单的永久 A→B→C 总序；主要策略是主动删掉危险的跨组件持锁边：Reader 锁外 flush、EventHub 无锁 epoll、Dispatcher 锁外 Policy、Looper 锁外 callback。遇到例外时，再依赖接口明确的 non-reentrant 承诺。

这也说明“看到两把锁嵌套就记顺序”不够。真正要问的是：外部代码能否重入、睡眠是否有界、对象在解锁期靠什么延寿、回来后哪些状态会失效，以及另一方向是否存在同时可达的锁边。

## 12. Command不是线程池，而是同一Dispatcher线程上的解锁续体

Dispatcher 头文件给出核心不变量：Policy 可能阻塞或重入，所以一般不能在持内部锁时调用。若状态机在锁内已经找到了候选窗口，却还需要 `interceptKeyBeforeDispatching()`、`notifyAnr()`、焦点通知或 FINISHED 后的 fallback 决策，它会构造 `CommandEntry`，保存函数指针、token、强引用与标量参数，再压入 commandQueue。

`runCommandsLockedInterruptible()` 仍由 InputDispatcher 线程调用。它弹出队首，直接执行 `command(*this, commandEntry.get())`，并循环到队列为空；Command 在回调后的续体还可以追加新 Command，本轮继续处理。它没有 worker、没有另一条 Looper，也不是异步 RPC。`LockedInterruptible` 表示允许释放锁，不表示每个 Command 必然解锁：例如部分 Motion 后处理或已 handled 的 Key 可以全程在锁内返回。需要外部 Policy 时，它的价值才体现为把状态机显式切成“保存稳定输入 → 解锁同步回调 → 重新加锁 → 重验并续跑”。

`beforeQueueing` 通常不需要 Command：硬件 notify 还在 Reader 线程且尚未取得 Dispatcher 锁，注入路径也在调用者线程先做 capability 预检与 interception。初次 `hasInjectionPermission()` 成功只会置 `TRUSTED`；失败不会当场拒绝，因为选出目标后，同 UID 注入仍可成立，最终还要按目标 `ownerUid` 走 `checkInjectionPermission()`。`beforeDispatching` 则发生在 Dispatcher 已持锁处理 pending Key、并快照当时 focused InputChannel 之后，必须用 Command 断开锁；真正的 Connection 与 targets 会在 Policy 结果回来后的状态机中重新查找，不能称为回调前已经锁定。`checkInjectEventsPermissionNonReentrant()` 是有文档的窄例外；它承诺实现不会重入，所以 capability 检查也可能在 Dispatcher 锁环境中同步进入 Policy，不能把这个例外推广给其他 Policy 方法。

### 练习 8：ANR回调期间Connection消失，强引用能保住什么

设 `doNotifyAnrLockedInterruptible()` 已从 Command 保存 InputChannel/token，解锁进入 Policy；期间另一线程注销该 Connection，Policy 返回 0。问回来后能否沿用回调前的 Connection 并执行取消？

唯一答案是：保存的强引用只保证相关对象内存不会过早释放，不保证 token 仍在 `mConnectionsByFd`、Connection 仍注册或状态仍 NORMAL。代码重新加锁后用 token 调 `getConnectionLocked()`；得到 null 就直接返回，不执行 `cancelEventsForAnrLocked()`。生命周期安全与业务状态有效是两项独立证明。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F "Methods marked 'LockedInterruptible' must be called with the lock acquired but" frameworks/native/services/inputflinger/dispatcher/InputDispatcher.h
grep -n -F "A 'LockedInterruptible' method may called a 'Locked' method, but NOT vice-versa." frameworks/native/services/inputflinger/dispatcher/InputDispatcher.h
grep -n -F 'command(*this, commandEntry.get());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::doNotifyAnrLockedInterruptible(CommandEntry* commandEntry) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->notifyAnr(commandEntry->inputApplicationHandle, token, commandEntry->reason);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<Connection> connection = getConnectionLocked(token);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'cancelEventsForAnrLocked(connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'checkInjectEventsPermissionNonReentrant' frameworks/native/services/inputflinger/dispatcher/include/InputDispatcherPolicyInterface.h
```

## 13. 解锁回来必须重验；外部状态更新也可能当场产生派发副作用

强引用解决 use-after-free，不解决“事实还是不是原来的事实”。r48 在几个关键续体里分别采用不同重验方式：

| 回调场景 | 解锁前保存 | 回来后重新确认 |
|---|---|---|
| Connection ANR Policy | application、InputChannel/token、reason | 正 extension 重新按 token 延长；非正值重新按 token 查 Connection，仍存在才取消 |
| no-focused-window ANR Policy | application、空 token、reason | 正 extension 仅在 no-focus timer 仍存在且 application 非空时重置；非正值查不到 Connection 后直接返回 |
| Key beforeDispatching | KeyEntry 引用、候选 channel/token | 把结果写回仍存活的 Entry，下一次目标选择继续检查状态 |
| FINISHED 后 fallback | Connection 强引用、finishTime、seq、handled；首次查找后临时取得 DispatchEntry/EventEntry | Command 起始按 seq 首查；若后处理解锁，再按 seq 二查；仅 restart 时检查 `STATUS_NORMAL` |
| channel broken/focus | Connection 或 old/new token | 回调本身锁外；后续状态以当前 map/queue 为准 |

FINISHED 路径尤其能说明两层安全。`handleReceiveCallback()` 先读到 seq，`finishDispatchCycleLocked()` 据此 post Command；Command 执行时才首次 `findWaitQueueEntry(seq)`。后续 fallback Policy 可能中途解锁，回来后源码第二次按同一 seq 查 waitQueue，因为注销可能已经排空队列。即使 `sp<Connection>` 仍让对象活着，也不能重复 erase、释放或重发一个已经消失的 DispatchEntry。

外部更新也不是统一的“写字段、wake、等 Dispatcher 处理”。例如 `setInputWindows()` 的调用线程在 Dispatcher 锁内替换窗口集合时，会按实际旧/新窗口条件识别焦点 token 改变、为仍有 channel 的旧焦点合成 non-pointer CANCEL、把 FocusEntry 插到普通 pending 前、为被移除且仍有 channel 的 touched window 合成 pointer CANCEL，并及时释放已经从新快照移除的旧 window channel；只有更新的 display 同时是 `mFocusedDisplayId`，才 post 焦点变化 Command。若 Connection 并非 `BROKEN`，且 InputState 实际生成了非空 CANCEL，合成事件才会直接入该 Connection 的 dispatch cycle。解锁后的 wake 让普通 pending 事件按新快照继续选择目标，但不能把前面的同步副作用抹掉。

因此诊断一条来自 Binder 或 system_server 其他线程的 Dispatcher API 时，必须读完整锁区。函数末尾出现 `mLooper->wake()`，不等于此前只做了惰性赋值；同样，看到一次锁外回调，也不能假设回来仍能继续使用旧 map 迭代器、queue 位置或 Connection 状态。

## 14. JNI同步调用沿用当前native线程，Java对象不会自动把工作送到主Looper

InputReader 与 InputDispatcher 因 `canCallJava=true` 已挂接 JavaVM。`NativeInputManager` 的 Policy 实现取得当前线程 `JNIEnv` 后直接 `CallVoidMethod`、`CallIntMethod` 或 `CallLongMethod`；这些都是同步调用。目标是 Java 对象，只改变语言边界，不改变线程身份。初始 Java 方法若没有显式 post，就在进入 JNI 的那条 native 线程上执行并返回。

常见入口应逐项标注：

| 调用 | native 调用线程，以及发生时的首个 Java callback |
|---|---|
| 硬件 Key 的 `interceptKeyBeforeQueueing` | InputReader 线程，并同步进入 Java |
| 硬件 Motion 的 `interceptMotionBeforeQueueing` | C++ 调用在 InputReader；interactive 快路通常只改 flag，non-interactive 分支才同步进入 Java |
| 软件注入的 beforeQueueing | 注入调用者线程，常见为 Binder 入口线程；只有满足 Policy 分支时才继续同步进入 Java |
| beforeDispatching、ANR、focus Command | InputDispatcher 线程 |
| Switch 直接通知 | 硬件主线上的 InputReader 线程 |
| App FINISHED 后处理 | InputDispatcher 线程的 Looper fd callback |
| Watchdog 的 `InputManagerService.monitor()` | Watchdog 的 foreground HandlerChecker 所绑定 `FgThread` |

Java 方法内部仍可再次切线程。例如 `notifyInputDevicesChanged()` 先在 Reader 线程进入 Java、更新快照并向 `mHandler` 发消息，真正向 listeners 交付稍后在 DisplayThread；Handler 甚至可能在 Reader 完成随后 flush 前就获得调度。Filter 的 oneway/Handler 链、tablet-mode 分支也有第二次交接。相反，`notifyANR()`、`notifyFocusChanged()` 等可以继续同步进入 WindowManager callback。必须把“第一次进入 Java”与“Java 实现主动 post 后的第二次交接”画成两个完成点。

传给 `NativeInputManager` 的 DisplayThread Looper主要服务 pointer/sprite 与 IMS Handler 相关设施；Dispatcher 另建的 native Looper只服务其 fd 循环。Policy 回调即使最终操作显示，也不会因为 `NativeInputManager` 保存了 DisplayThread Looper而自动在那里执行。

性能判断也要收窄。源码中的 50 ms `SLOW_INTERCEPTION_THRESHOLD` 计时覆盖几个 interception 路径，不是任意 Policy callback 的统一超时，更不会强制终止慢调用。Dispatcher 已解锁可让 producer 继续入队，但当前输入工作线程仍被同步占住，eventfd 只会积累门铃。

## 15. condition释放锁但必须重查谓词；monitor与idle只是不同强度的探针

同步注入最标准地展示 condition 协议。注入线程在 `unique_lock` 下循环读取 `injectionResult`；仍为 PENDING 时计算剩余时间并 `wait_for()`。wait 会原子释放 Dispatcher 锁，Dispatcher 才能取得同一锁、写结果并 `notify_all()`；返回后等待者重新持锁，再次检查结果。WAIT_FOR_FINISHED 还用第二个循环检查 `pendingForegroundDispatches`。循环同时处理虚假唤醒与其他状态变化，notify 本身不等于谓词成立；等待超时也只改变调用者得到的结果，不会撤销已经入队、可能稍后仍被派发的事件。

Reader monitor 与 Dispatcher monitor 是较弱的握手。Reader monitor 先取得 Reader 锁、wake EventHub，再无谓词 wait；Reader 从 `getEvents()` 返回并取得锁后广播 alive，monitor 醒来后还调用 `EventHub::monitor()` 取得并释放 EventHub 锁。Dispatcher monitor 同样持锁 wake、无谓词等待下一轮开头的 alive 通知，但没有额外 Looper 锁探测。先持目标锁再 wake、随后 wait 原子解锁，避免了正常路径的 lost wake；两种 wait 都没有 predicate loop，所以从 condition 规范看仍存在虚假返回边界，只能视为 best-effort 活性探针。即使收到了真实 Dispatcher alive，线程也可能紧接着在本轮 Command 的慢 Policy 调用中阻塞；通知只证明到达轮首，不证明整轮完成。

Watchdog 线程本身负责安排检查并评估超时；真正逐个调用 Monitor 的 `HandlerChecker.run()` 绑定 `FgThread.getHandler()`。IMS monitor 先试 Java `mInputFilterLock`、`mAssociationsLock`，再经 JNI 严格按 Reader monitor → Dispatcher monitor 执行。前序 Monitor 或 Reader 卡住，本次检查就到不了 Dispatcher。native monitor 自己没有 timeout；本树 `DB=false`，foreground HandlerChecker 的窗口因此是 60 秒。Watchdog 不能单独抢占或取消那次 Policy 调用，但 overdue 后会抓取诊断证据；若没有 debugger、允许 restart 且 controller 未要求继续等待，最终还会终止 `system_server` 促使系统重启。

`waitForIdle()` 又是另一种语义。它持 Dispatcher 锁 wake 后，用一次无谓词的 100 ms `wait_for()` 等 `mDispatcherEnteredIdle`；超时返回 false，非超时返回 true。100 ms 是等待参数，不是严格墙钟上限：线程调度与返回前重新取得 mutex 都可能令实际返回更晚。健康线程若正在执行较长工作也可 false；理论上的虚假唤醒可带来 false positive；真正通知只代表 `nextWakeupTime==MAX`，frozen inbound 仍可能存在；通知后新工作也可能在调用者返回前入队。因此它适合作为内部同步便利门，不是事务屏障、App FINISHED 证明或 ANR 定义。

### 练习 9：区分注入谓词、Watchdog超时与frozen idle

设三个互不共享状态的子场景都忽略虚假唤醒。A：注入线程在等 foreground 计数归零。B：t0 时 Dispatcher 刚进入持续 70 秒的锁外 Java Policy，Reader monitor 已完成，FgThread 已进入 Dispatcher monitor；本树 Watchdog 窗口为 60 秒，但测试先把 `mAllowRestart` 设为 false，进程不会被终止。C：分发 frozen，inbound 有一笔 Key，且没有 Command、repeat、无焦点 deadline、ANR tracker 或其他 deadline，此时调用 `waitForIdle()`。问三类等待各能证明什么？

唯一答案是：A 中注入线程只有在重新持锁并看到 `pendingForegroundDispatches==0` 时才能认定完成，单次 notify 不够；B 中 FgThread 在 Dispatcher alive 等待处卡住，Watchdog 线程约 60 秒后把 foreground checker 判为 overdue、收集证据，却不能抢占 Policy，且本题因禁止 restart 不杀进程；C 中 Dispatcher 可在队列非空时得到 MAX 并通知 idle，所以 `waitForIdle()` 的 true 仍不能证明入站或 App 回执账已清空。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'while (injectionState->pendingForegroundDispatches != 0) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mInjectionSyncFinished.wait_for(_l, std::chrono::nanoseconds(remainingTimeout));' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mMonitorChecker = new HandlerChecker(FgThread.getHandler(),' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'private static final boolean DB = false;' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'mHandler.postAtFrontOfQueue(this);' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'mCurrentMonitor.monitor();' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'Process.killProcess(Process.myPid());' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'System.exit(10);' frameworks/base/services/core/java/com/android/server/Watchdog.java
grep -n -F 'Watchdog.getInstance().addMonitor(this);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'nativeMonitor(mPtr);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'im->getInputManager()->getReader()->monitor();' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'im->getInputManager()->getDispatcher()->monitor();' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'mReaderIsAliveCondition.wait(mLock);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mDispatcherIsAlive.wait(_l);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mDispatcherEnteredIdle.wait_for(lock, TIMEOUT);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 16. 诊断顺序：先定位等待者与完成点，再判断锁、wake和回调

看到“输入卡住”时，按下面顺序比背一张全局锁表更可靠：

1. **先认线程。** 当前栈是 InputReader、InputDispatcher、FgThread Monitor、Binder producer、DisplayThread，还是 App 线程？Java 栈帧不等于 Java 主线程。
2. **再认等待器。** Reader 的正常空闲点是 EventHub `epoll_wait()`，Dispatcher 的正常空闲点是 Looper `epoll_wait()`；前者由 pipe wake，后者由 eventfd 或 InputChannel fd 唤醒。
3. **找受保护的事实。** RawEvent/Device 属于 EventHub/Reader，inbound/pending/window/Connection/ANR 属于 Dispatcher；Looper 锁保护 fd request、message envelope 与 epoll rebuild，poll response/index 则由单一 poller 私有。wake 次数不能替代这些账。
4. **检查睡眠是否放锁。** Reader 调 EventHub 前放 Reader 锁，EventHub epoll 前放自己的锁；Dispatcher poll 前放 `mLock`，Looper callback 前放 Looper 锁；condition wait 则在睡眠时原子放锁。
5. **标记外部同步调用。** Policy/JNI/Binder 若正在当前输入线程执行，wake 不能抢占。检查调用前保存了什么、回来后是否按 token、seq、status 与 queue 重验。
6. **最后选择完成点。** enqueue 不是 Dispatcher 已消费，wake 不是循环完成，idle 不是队列全空，monitor 返回不是业务正确，stop 返回也不是 App 或显示完成。

几个典型现场可立即缩小范围：InputReader 长期睡在 EventHub epoll 且无设备/配置工作，通常是正常空闲；InputDispatcher 睡在 Looper epoll 且队列与 deadline 均空，也通常正常；Dispatcher 卡在锁外 Policy 时 producer 仍可入队并累积 eventfd，但派发线程不前进；FgThread 栈停在 Reader monitor 时，本次 JNI 链尚未检查 Dispatcher；`waitForIdle()==false` 只说明 100 ms 内没收到该通知，不能单独判 ANR；stop 卡住而 wake fd 已可读，则应向 poll 之外的回调或锁等待追。

本章最终留下的并发图不是一条固定锁总序，而是四类可核验边界：**状态在所属锁内提交，长睡眠不占组件状态锁，有重入或长阻塞风险的跨组件调用先断开危险锁边，回来用稳定身份重新定位；明示 non-reentrant 或包装层持锁下传的窄例外则逐处核对。** 第 248 章沿着 Reader 一侧继续：EventHub 怎样扫描 `/dev/input/event*`，怎样建立 Device 与 Mapper，设备增删、配置 generation、reset 与通知又怎样闭合为一条生命周期链。
