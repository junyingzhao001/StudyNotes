# 190 Android InputChannel 与窗口生命周期：屏幕上的窗口、路由快照和 socket 为何不会同时消失

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`
> 核心问题：一个窗口退出、失去输入资格或客户端断开时，`InputWindowHandle`、connection token、`InputChannel`、`Connection` 与底层 fd 分别在什么时候失效？

---

## 1. 从“退出动画还在，窗口却已经点不动”开始

### 屏幕可见不是输入存活证明

应用移除窗口时，相对于 App 侧销毁 `InputEventReceiver` / client channel，`ViewRootImpl` 会先经 `IWindowSession.remove()` 请求 WMS 删除窗口；这不是说它早于 `ViewRootImpl` 内部的 surface 清理等所有 detach 步骤。`WindowState.removeIfPossible()` 很早就调用 `disposeInputChannel()`，之后才判断 WMS 是否还要为退出动画保留 surface。

因此完全可能出现：

```text
surface 仍在屏幕上播放退出动画
  ≠ WindowState 仍有注册的 InputChannel
  ≠ InputDispatcher 的窗口快照仍允许命中
  ≠ App 的 client endpoint 仍然打开
```

反方向也成立：一次窗口快照可以先移除某个 `InputWindowHandle`，而它对应的 `Connection` 仍在 Dispatcher 的注册表中。快照更新会先做 focus、touch 与 hover 收尾；但就旧 handle 本体及注册/传输资源而言，末尾只调用 `releaseChannel()`。这个名字很像“释放 socket”，实际却只清掉旧 handle 中的 Binder token。

### 一句话结论

> Android 11 r48 把窗口视觉存在、输入路由资格、Dispatcher 注册状态、传输健康状态和 fd 引用寿命分开管理；排障时必须逐层找各自的完成点，不能把“窗口没了”“channel 注销了”“socket 关了”和“App 已收尾”当成同一件事。

### 读完应能回答什么

- pair 的两端、fd 副本和 connection token 分别是什么关系；
- 为什么 `token` 能找连接，却不能唯一标识一个 input window；
- WMS 的窗口信息怎样经 SurfaceFlinger 成为 Dispatcher 的有序快照；
- 窗口刷新为何复用旧 handle，以及怎样清焦点、触摸和 hover 引用；
- 正常注销、发送端硬错误、接收端 HUP、ANR 与最终 fd close 有何不同；
- 为什么 `WAIT_FOR_FINISHED` 返回成功，也未必证明 App 发过 `FINISHED`。

本章不重复第 174、175、188 章的 wire ABI、App 输入阶段和 FINISHED 编号账，也不展开 monitor 与 `pilferPointers()` 的目标选择；后者留到第 191 章。这里只追踪普通窗口的生命周期，并在必要处标出 monitor 例外。

源码主锚点：`WindowState.java:2160-2355,2453-2510`、`InputTransport.h:177-249`、`InputTransport.cpp:249-429`、`InputManager.cpp:94-119`、`InputDispatcher.cpp:2456-2762,3565-3793,4299-4401,4491-4531,4630-4640`。

## 2. 先把五条生命线和五种身份拆开

### 五条生命线不会原子提交

| 维度 | 主要对象或容器 | “还活着”表示什么 |
|---|---|---|
| 视觉层 | `WindowState`、Surface、SurfaceFlinger Layer | 画面或退出动画仍可能被合成 |
| 路由层 | `mWindowHandlesByDisplay`、focus、`TouchState` | Dispatcher 当前快照仍可能选择该 input window |
| 注册层 | `mConnectionsByFd`、`mInputChannelsByToken`、Looper fd | Dispatcher 仍能按 token/fd 找到注册端 |
| 传输层 | `Connection::status`、outbound/wait、peer endpoint | channel 是 `NORMAL`、`BROKEN` 还是已注销的 `ZOMBIE` |
| 资源层 | Java wrapper、native `sp<InputChannel>`、`unique_fd` | 哪些持有者仍让某个本地 fd 或其副本保持打开 |

一次 API 调用通常只改变其中一两层。例如：

- `setInputWindows` 移除 handle：先改变路由层，不自动注销 Connection；
- `unregisterInputChannel`：改变注册层与传输状态，不自动删除窗口快照；
- `InputChannel.dispose()`：释放一个 Java wrapper 的 native 持有，未必立刻 close fd；
- peer close：先成为传输事件，Dispatcher 的 receive callback 再决定是否注销；
- surface 退出动画：可以在输入通道已经注销后继续存在。

### `id`、`token`、fd、对象指针和名字各回答什么

| 标识 | 粒度 | 是否可跨进程直接比较 | 关键限制 |
|---|---|---:|---|
| `InputWindowInfo.id` | 一个 input window / Layer | 作为快照字段可以 | r48 主路径由 SurfaceFlinger 的 Layer `sequence` 填入 |
| `InputWindowInfo.token` / connection `token` | 输入路由接收身份；普通 channel-backed 窗口把它接到一对 channel | 可借 Binder identity | 不唯一标识窗口；portal 可使用不对应已注册 channel 的独立 Binder |
| fd 数值 | 当前进程中的一个打开描述符 | 不可 | 跨进程数值无意义；dup 后数值也不同；关闭后会复用 |
| `sp<InputWindowHandle>` 指针身份 | 当前 native 对象 | 不可 | focus、touch、hover 等内部引用依赖它稳定 |
| channel/window name | 诊断标签 | 字符串可见 | 明确允许不唯一，不能当关联键 |

`InputWindow.h` 对两个字段写得很直接：`token` 不得用来唯一标识窗口，不同 input windows 可以有同一个 token；`id` 才唯一标识 input window。

`InputTransport.h` 另行说明 `InputChannel` 自身的 token 标识同时创建的 channel pair，不能用它判断两个具体 `InputChannel` 对象是否相等。普通 `WindowState` 会把这个 token 写入 window handle，所以两种语义在常见路径上重合；但 portal 会放入一个独立 `Binder`，没有对应的已注册 channel。两端本来就共享 pair token，同一端的 `dup()` 也保留它。

### 普通 channel-backed 窗口中，token 是连接身份，不是窗口身份

r48 甚至有专门的 `InputDispatcherMultiWindowSameTokenTests`：两个不同 frame、不同 handle 的 input windows 共用一个 client token。Dispatcher 构造目标时按 token 查找已有 `InputTarget`，同 token 的窗口会汇入同一个 channel target。

这带来三个直接结论：

1. `id + token` 才适合判断“这一份旧窗口快照是否仍存在”；
2. 只按 token 的反查得到的是“某个使用该连接的窗口”，不保证唯一；
3. 移除一个共享 token 的 handle 时，对 connection 合成取消可能波及仍存在的同 token handle，因为 `InputState` 是每 Connection 一份。

`addWindowTargetLocked()` 对同 token 目标还断言 `targetFlags` 与 `globalScaleFactor` 一致，再累计 pointer 映射。因此“允许共享 token”不等于任意两个窗口属性都可以毫无约束地共享同一接收连接。

### 本章会遇到的几张表

| 所在层 | 表 | 键 → 值 | 用途 |
|---|---|---|---|
| WMS | `mWindowMap` | `IWindow.asBinder()` → `WindowState` | 客户端窗口 Binder 身份反查 |
| WMS | `mInputToWindowMap` | connection token → `WindowState` | broken、ANR、focus 等输入回调反查普通窗口 |
| Dispatcher | `mWindowHandlesByDisplay` | displayId → 有序 handles | 命中与窗口快照 |
| Dispatcher | `mInputChannelsByToken` | connection token → 已注册端 | 从路由目标接上传输端 |
| Dispatcher | `mConnectionsByFd` | 本进程 fd → `Connection` | Looper 回执/错误回调与发送账 |

`mWindowMap` 和 `mInputToWindowMap` 也不是同一生命周期。窗口可因退出动画继续留在前者，而 `disposeInputChannel()` 已经把它从后者删除。

源码锚点：`InputWindow.h:121-128`、`InputTransport.h:177-249`、`InputDispatcher.cpp:1999-2029,3565-3612`、`InputDispatcher_test.cpp:2133-2154`、`DisplayContent.java:5176-5191`、`WindowManagerService.java:565-569,1916-1919`。

## 3. 一对 InputChannel 到底创建了什么

### 内核对象是 `SOCK_SEQPACKET` socketpair

Java 的 `InputChannel.openInputChannelPair(name)` 校验名字非空，JNI 再调用 native `InputChannel::openInputChannelPair()`。核心创建逻辑可缩成：

```cpp
socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets);
sp<IBinder> token = new BBinder();
outServerChannel = InputChannel::create(name + " (server)",
        unique_fd(sockets[0]), token);
outClientChannel = InputChannel::create(name + " (client)",
        unique_fd(sockets[1]), token);
```

这里创建的是两个互为 peer 的 socket endpoint，不是“一个 fd 复制成两个”。`SOCK_SEQPACKET` 保留消息边界，适合承载定长头加不同 payload 的 `InputMessage`。

两端都通过 `InputChannel::create()` 设置为 non-blocking。四次 `setsockopt()` 尝试调整两端 send/receive buffer，但 r48 忽略返回值：pair 创建成功不证明这四次 buffer 调整全部成功。

### server/client 只是角色标签

native 头文件明确称两端 equivalent，server/client 是方便理解的命名。真正角色由后续组合决定：

```text
注册给 InputDispatcher 的端
  -> Connection.inputPublisher
  -> 正向发 KEY / MOTION / FOCUS
  -> 反向收 FINISHED

交给 App InputEventReceiver 的端
  -> InputConsumer
  -> 正向收输入
  -> 反向发 FINISHED
```

socket 本身是双向的；“server 只能写、client 只能读”不是内核约束，而是输入协议的使用约定。

### token 不是 socket 自动生成的身份

token 是输入框架在创建 pair 时额外构造的 `BBinder`，随后作为强 Binder 引用保存在两端 `InputChannel::mToken` 中。它不是 fd 号，不是 inode，也不是 Unix socket 凭据。

Parcel 序列化会分别写：

```text
name
strong Binder token
unique file descriptor
```

所以 token 与 fd 虽一起旅行，却承担不同职责：token 让框架跨层关联 connection，fd 才承载字节。

### fd 关闭看“最后持有”，不看某个 Java 变量是否置空

native `InputChannel` 用 `unique_fd` 持有一个本地描述符；该 native 对象最后一个强引用消失时，成员析构才 close 这个 fd。Dispatcher 中至少可能存在这些 native 持有：

- Java `InputChannel.mPtr` 指向的 `NativeInputChannel`；
- `Connection::inputChannel`；
- `Connection::inputPublisher` 内部的 channel 强引用；
- `mInputChannelsByToken` 中的 channel 强引用；
- 正在执行的 command 对 `Connection` 的强引用。

此外，Parcel 与 `dup()` 还能制造指向同一 endpoint 的其他 fd。关闭其中一个描述符，并不关闭仍被其他 fd 引用的 endpoint。

源码锚点：`InputChannel.java:55-102`、`android_view_InputChannel.cpp:127-160`、`InputTransport.h:177-249`、`InputTransport.cpp:249-303,383-429`。

## 4. `transferTo`、Parcel、`dup`、`dispose` 不是同一种“复制或释放”

### `transferTo()` 移动 Java wrapper 内的 native 指针

`WindowState` 把 client 端交给 Binder out 参数时会调用：

```java
mClientChannel.transferTo(outInputChannel);
mClientChannel.dispose();
mClientChannel = null;
```

JNI 实现没有调用 `dup()`：

```text
确认目标 wrapper 尚未初始化
  -> 把源对象的 NativeInputChannel* 写进目标 mPtr
  -> 把源对象 mPtr 清成 null
```

因此 `transferTo()` 是所有权移动。紧随其后的源 `mClientChannel.dispose()` 看见空 `mPtr`，实际上无事可做。此刻仍在 system_server 内；跨进程复制尚未由 `transferTo()` 完成。

### Parcel 才建立可跨进程持有的 fd

`InputChannel::write()` 调 `Parcel::writeUniqueFileDescriptor()`；r48 的 Parcel 先用 `F_DUPFD_CLOEXEC` 复制描述符并接管副本。读取端 `readUniqueFileDescriptor()` 又为接收方建立自己拥有的 fd，再用同一个 name/token 构造新的 native `InputChannel`。

所以 App 拿到的是：

- 指向 client socket endpoint 的本进程 fd；
- 与 server/client 原始对象相同的 connection token；
- 一个独立 Java/native wrapper 生命周期。

不同进程里的 fd 数字即便碰巧相同，也没有身份意义。

Java `writeToParcel()` 若收到 `PARCELABLE_WRITE_RETURN_VALUE`，会在写完后 dispose 发送方 wrapper。这是 out 参数交付后释放发送方包装的机制，不代表接收方 fd 也被关闭。

### `dup()` 复制同一个 endpoint，不创建 peer

`InputChannel::dup()` 对当前 fd 调 `::dup()`，新对象保留相同 name 和 token：

```text
pair 两端：不同 endpoint，token 相同
同端 dup：同一 endpoint 的不同 fd，token 也相同
```

fd 耗尽的 `EMFILE/ENFILE` 在这里会触发 fatal；其他 dup 失败返回 null，再由 Java JNI 转成异常。

普通 Dispatcher 注册按 token 查重。因此把已注册端的 dup 再作为另一条普通 connection 注册，会因相同 token 被拒绝；dup 不是创建新连接的办法。

### `dispose()` 与 `release()` 的关键区别

Java API 的表面都像“释放”：

| API | 清当前 wrapper 的 native 指针 | 调 dispose callback | 保证底层 fd 立即关闭 |
|---|---:|---:|---:|
| `dispose()` / finalizer | 是 | 若已安装则是 | 否，其他强引用或 fd 副本可继续持有 |
| `release()` | 是 | 否 | 否 |
| `transferTo(other)` | 源清、目标接管 | callback 随整个 native wrapper 移动 | 否 |
| `dup()` | 原对象不变 | 新 wrapper 不继承原 callback | 否 |

`release()` 的注释本来就强调：它只放弃 Java 对 native channel 的持有，native-land 若还有引用，channel 可以继续存在。把它用于一个已注册的普通 server wrapper，会绕过自动注销安全网。

源码锚点：`InputChannel.java:76-176`、`android_view_InputChannel.cpp:163-200,202-283`、`InputTransport.cpp:383-425`、`Parcel.cpp:1127-1160,2078-2092`。

## 5. WindowState 怎样把 pair 接入普通窗口

### 创建顺序先注册，后把 token 写入 handle

`WindowState.openInputChannel()` 的主顺序是：

```text
检查 mInputChannel 尚不存在
  -> openInputChannelPair(name)
  -> 保存 server 到 mInputChannel
  -> 保存 client 到 mClientChannel
  -> IMS.registerInputChannel(server)
  -> mInputWindowHandle.token = server.getToken()
  -> 交付或本地消费 client
  -> mInputToWindowMap[token] = this
```

这里至少有三个不同完成点：

1. pair 创建成功：两个 endpoint 与 token 已存在；
2. register 返回：Dispatcher 普通注册路径返回 `OK`；
3. token 进入窗口 handle 并出现在后续快照：窗口才可能成为路由目标。

所以注册成功不等于窗口已经可命中；窗口快照与 channel 注册是两次独立提交。

### 普通应用分支把 client 交给 ViewRootImpl

App 先准备一个未初始化 `InputChannel` 作为添加窗口的 out 参数。WMS 通过 `transferTo()` 把 client native wrapper 移到这个 out 对象，Binder 序列化后，App 侧再围绕它创建 `WindowInputEventReceiver`。

WMS 随即把本地 `mClientChannel` 置 null。因此普通窗口销毁时，WMS 不再拥有 App 的 client endpoint；客户端 receiver 自己负责其生命周期。

### “应用死了但窗口暂时可见”会换一对 channel

`removeIfPossible(keepVisibleDeadWindow=true)` 的分支更特别：

```text
先注销原 server Connection，并释放 WMS 对旧端点的持有
  -> WindowState / surface 暂时保留
  -> openInputChannel(null) 创建新 pair 和新 token
  -> client 不交给 App
  -> WMS Handler Looper 上创建 DeadWindowEventReceiver
```

这个 dummy receiver 对收到的每个事件都调用 `finishInputEvent(event, true)`。源码注释给出的目的不是让已死 App 继续收事件，而是让 input monitor 仍可观察 tap，并有机会重启应用。

所以“同一个 WindowState”也不保证 connection token 永远不变。旧 server connection 已注销，死窗占位分支改用新 pair、新 token 与 WMS 本地 consumer；这一步本身不能证明旧 pair 的任意 fd 副本或临时 native 引用都已销毁。

### WMS 的 token 反向表只覆盖这类普通窗口所有权

创建末尾写入：

```java
mWmService.mInputToWindowMap.put(mInputWindowHandle.token, this);
```

后续 native broken/ANR/focus 回调可用 connection token 找 `WindowState`。但这张表不是“系统所有 channel 的总表”：monitor、某些 input consumer 或其他自定义 channel 不一定对应一项 `WindowState`。

源码锚点：`WindowState.java:2218-2286,2453-2510`、`ViewRootImpl.java:1019-1035,1124-1131`、`WindowManagerService.java:565-569`。

## 6. 注册把 channel 变成 Connection，但仍没把窗口放进路由表

### Java/JNI 不生成新 token

`InputManagerService.registerInputChannel()` 只检查 Java 参数不为 null。JNI 再取出 native `sp<InputChannel>`：若 wrapper 未初始化，抛 `IllegalStateException`；否则直接交给 `NativeInputManager`，再进入 Dispatcher。

`InputManagerService.java` 的旧 JavaDoc 写着 registration 使用 generated token，但实际注册代码没有生成或替换 token；token 已在 `openInputChannelPair()` 的 native 实现中创建。遇到注释与调用链冲突，应以实现为准。

Dispatcher 返回任何非零注册状态，包括重复 token 的 `BAD_VALUE`，JNI 都转成 `RuntimeException`。只有 native 注册成功后，JNI 才给这个普通 Java server wrapper 安装 dispose callback。

### 普通注册先按 token 查重

`registerInputChannel()` 在锁内执行：

```cpp
sp<Connection> old = getConnectionLocked(channel->getConnectionToken());
if (old != nullptr) return BAD_VALUE;

sp<Connection> c = new Connection(channel, false, mIdGenerator);
mConnectionsByFd[channel->getFd()] = c;
mInputChannelsByToken[channel->getConnectionToken()] = channel;
mLooper->addFd(channel->getFd(), 0, ALOOPER_EVENT_INPUT,
               handleReceiveCallback, this);
```

`getConnectionLocked(token)` 没有 token→Connection 专表；它线性遍历 fd map，比较每个 channel 的 connection token。相反，路由按 token 找 `InputChannel` 时使用哈希表。不要把两种查找复杂度混成一个结论。

普通路径也没有像 monitor 注册那样显式拒绝 null token，它依赖调用者传来合法 pair。WindowState 的 pair 满足这个不变量。

### 新 Connection 的初态

构造完成后：

- `status = STATUS_NORMAL`；
- `responsive = true`；
- `monitor = false`；
- outbound/wait 两队列为空；
- `InputPublisher` 持有同一注册端 channel；
- `InputState` 是这条 connection 的提前投递语义账。

最后一点沿用第 189 章的精确定义：`InputState` 在 outbound 入队、publish 与 App 回调之前就会更新，不能称为“接收者已经看到的事实”。

### fd 表和 token 表服务不同方向

```text
窗口命中
  -> handle.token
  -> mInputChannelsByToken[token]
  -> InputTarget
  -> getConnectionLocked(token)
  -> Connection / InputPublisher

server fd 可读或异常
  -> Looper callback(fd)
  -> mConnectionsByFd[fd]
  -> receive FINISHED 或注销
```

fd 是本进程 poll 键；token 是跨窗口快照与连接的逻辑 join key。两张表不是冗余索引。

### Looper 监听反向协议与错误

注册只显式订阅 `ALOOPER_EVENT_INPUT`。带 callback 的 `addFd()` 会忽略传入 ident `0`，内部使用 `POLL_CALLBACK`。Looper 即使未显式订阅，也会报告 error/hangup。

server fd 可读时，r48 publisher 协议期望反向消息是 `FINISHED`；其他 message type 会成为 `UNKNOWN_ERROR`。这是第 14 节自动注销路径的入口。

一个低概率但真实的 r48 边界是：Dispatcher 不检查 `mLooper->addFd()` 的返回值。若 epoll 安装失败，fd/token 两张 map 已写入，注册仍 wake 后返回 `OK`，也没有回滚。正常路径依赖 `addFd()` 成功，但 Java 返回不能证明 Looper 监听一定安装成功。

### wake 只让调度线程醒来观察新状态

解锁后 `mLooper->wake()` 写的是 Looper 自己的 eventfd，不是 input-channel fd。它打断可能阻塞的 `pollOnce()`，让 Dispatcher 尽快进入下一轮。

源码对 register 只说 connection 集合已变化；“某个 pending 事件因此可能重新获得机会”是合理推论，不是 wake 的交付保证。它不会替调用者提交窗口快照，也不等待 App。

源码锚点：`InputManagerService.java:553-576`、`com_android_server_input_InputManagerService.cpp:427-443,1338-1420`、`Connection.h:29-69`、`Connection.cpp:23-31`、`InputDispatcher.cpp:4299-4324,4491-4504`、`Looper.cpp:395-407,426-510`。

## 7. 窗口路由快照实际绕过了哪几层

### r48 主路径是 WMS → Surface transaction → SF → InputFlinger

容易从旧版本印象中误以为 WMS 直接把 Java `InputWindowHandle[]` 交给 Dispatcher。Android 11 r48 的普通窗口主路径多了一次 SurfaceFlinger 汇总：

```text
WMS InputMonitor（窗口快照生产者）
  -> 遍历 WindowState，填 Java InputWindowHandle
  -> SurfaceControl.Transaction.setInputWindowInfo(surface, handle)
  -> JNI 把 Java 字段复制成 InputWindowInfo
  -> SurfaceFlinger 把 InputWindowInfo 保存在 Layer drawing state
  -> SF reverse-Z 遍历需要 input info 的 Layer
  -> IInputFlinger.setInputWindows(flat infos)，oneway
  -> InputManager 按 displayId 分组并新建 BinderWindowHandle
  -> InputDispatcher.setInputWindows(handlesPerDisplay)
```

这里的 WMS `InputMonitor` 是窗口信息生产类，不要与第 191 章讨论的 public `InputMonitor` / gesture monitor 接收通道混为一谈。

### 为什么要由 SurfaceFlinger 汇总

窗口属性来自 WMS，但最终 Layer 的 Z 序、frame、scale、crop 与 display 归属要结合 SurfaceFlinger drawing state。`Layer::fillInputInfo()` 会把 Layer `sequence` 写成 `InputWindowInfo.id`，并根据 Layer transform 调整 frame、touchable region 与 window scale。

因此 Dispatcher 接到的是“用于本轮命中的合成快照”，不是一个活的 Java `WindowState` 指针。路由层与 WMS 对象层天然允许短暂不同代。

### 顺序由上游给，Dispatcher 不重排

SurfaceFlinger 用 `traverseInReverseZOrder()` 收集 flat list；`InputManager` 分组时保留各 display 中的相对到达顺序。Dispatcher 的触摸查找明确按 vector 的 front-to-back 遍历，并不会自己按 layer、type 或名称再次排序。

焦点选择也信任这个顺序：一个 display 出现多个 `hasFocus && visible` 候选时，只选第一个，也就是上游顺序中的 topmost。

### setInputWindows 是按“本次出现的 display”更新

`InputManager::setInputWindows()` 只为 flat list 中实际出现的 `info.displayId` 创建 map key。Dispatcher 再只遍历本次 map 中出现的 display：

```cpp
for (const auto& [displayId, handles] : handlesPerDisplay) {
    setInputWindowsLocked(handles, displayId);
}
```

所以 API 语义是逐 display 替换，不是“map 里缺失的 display 自动清空”。显示设备移除时，`NativeInputManager::displayRemoved()` 会显式提交 `{displayId, emptyList}`。WMS 的 `onDisplayRemoved()` 还先 `syncInputWindows()`，避免较旧 transaction 在清理之后迟到。

不要从这段代码进一步推断“任何仍存在但暂时没有 input layer 的 display 一定残留旧窗口”；上游通常还会提供无 channel 的 overlay/occlusion 信息。源码可确定的边界只是：Dispatcher 自身只处理显式 map key。

### oneway 与 sync listener 的完成点

SurfaceFlinger 对 `IInputFlinger.setInputWindows()` 使用 `IBinder::FLAG_ONEWAY`。`InputManager::setInputWindows()` 在 Dispatcher 方法返回后，才调用可选的 `ISetInputWindowsListener.onSetInputWindowsFinished()`。

因此即使上层使用 `syncInputWindows()` 等到了 listener，也最多说明这份窗口快照已走完 Dispatcher 的同步安装调用。它不说明：

- 某个 channel 已注册成功；
- 某事件已选中该窗口；
- socket publish 成功；
- App 已消费或回 `FINISHED`；
- surface 已 present 到屏幕。

源码锚点：`InputMonitor.java:130-169,329-361,443-560`、`android_view_SurfaceControl.cpp:510-525`、`Layer.cpp:2368-2405`、`SurfaceFlinger.cpp:2897-2928`、`IInputFlinger.cpp:28-46,65-81`、`InputManager.cpp:94-119`。

## 8. Dispatcher 怎样过滤、复用并安装 handle

### 空输入与“过滤后为空”不是完全相同的内部状态

`updateWindowHandlesForDisplayLocked()` 若收到的 vector 本身为空，直接：

```cpp
mWindowHandlesByDisplay.erase(displayId);
return;
```

若 vector 非空，但每个候选都被后面的规则过滤，函数最后仍执行 `mWindowHandlesByDisplay[displayId] = newHandles`，留下一个值为空 vector 的 map 项。

对大部分查询，两者都表现为“没有窗口”；但 dump 可分别出现整个 display key 不存在，或 `Display: X / Windows: <none>`。读状态时不必把容器形状差异误判成路由差异。

### 每个候选的检查顺序

非空列表中，每个 handle 按以下顺序处理：

```text
updateInfo()
  -> 缺少已注册 channel 时的合法性过滤
  -> info.displayId 与本轮 displayId 一致性
  -> 按 id 找旧项并比较 token
  -> 复用旧对象或接纳新对象
```

顺序本身有诊断意义。例如一个同时“缺 channel”和“display 错”的候选，可能先被 channel 规则过滤，根本走不到 display mismatch 日志。

### `updateInfo() == false` 不是主快照常见校验失败

抽象 `InputWindowHandle` 接口允许 `updateInfo()` 返回 false。旧 JNI `NativeInputWindowHandle` 在 Java weak reference 已经失效时会先 `releaseChannel()` 再返回 false；它不是一个通用的字段合法性验证器。

而 r48 的 SurfaceFlinger 主路径已把 Java handle 变成一份平坦 `InputWindowInfo`。`InputManager` 为它构造的 `BinderWindowHandle::updateInfo()` 固定返回 true。因此正文中的 false 分支应理解为泛型接口防线、测试或其他调用路径边界，而不是主路径中每帧都可能发生的 Java 对象校验。

### 缺少注册 channel 时的精确布尔条件

非 portal 候选按 token 找不到已注册 channel 时，代码计算：

```text
noInputChannel = INPUT_FEATURE_NO_INPUT_CHANNEL 已设置
canReceiveInput = 非 NOT_TOUCHABLE  或  非 NOT_FOCUSABLE
```

只有 `canReceiveInput && !noInputChannel` 才记录日志并跳过。因此无注册 channel 的候选仍可保留，只要：

- 明确声明 `INPUT_FEATURE_NO_INPUT_CHANNEL`；或
- 同时设置 `FLAG_NOT_TOUCHABLE` 与 `FLAG_NOT_FOCUSABLE`。

这里不检查 `visible`，也不直接检查 `canReceiveKeys`。无 channel 的 overlay 仍可能用于遮挡判断；“保留在窗口快照中”不等于最终会成为 App 输入 target。

### portal 绕过本地 channel 过滤

`portalToDisplayId != ADISPLAY_ID_NONE` 的 handle 即使没有本地注册 channel，也不会在这一步被跳过。正常跨 display portal 被触摸命中，且 `portalToDisplayId != 当前 displayId` 时，`findTouchedWindowAtLocked()` 才递归进入目标 display，而不是把 portal 自己作为最终 App endpoint。若异常数据把 portal 指回当前 display，它会作为“找到的窗口”返回，后续按其独立 token 接 channel 时可能失败。

Portal 仍要通过 display 一致性检查。r48 的递归没有显式 cycle guard，依赖上游 portal 图无环；这是实现前提，不应把 portal 例外改写成任意跨 display 跳转都安全。

### display mismatch 会跳过候选

通过前面过滤后，若 `info.displayId != displayId`，Dispatcher 记录错误并跳过。它不会用 map key 覆盖 handle 自报 display。

主路径本来就是先按 `info.displayId` 分组，这一检查主要保护直接调用者、测试或异常数据。

源码锚点：`android_hardware_input_InputWindowHandle.cpp:95-205`、`InputManager.cpp:94-114`、`InputDispatcher.cpp:802-846,3614-3670`、`InputMonitor.java:564-594`。

## 9. 为什么刷新快照必须保留旧 handle 对象

### SF 每次提交快照时都会带来新 BinderWindowHandle

`InputManager::setInputWindows()` 为 flat list 中每个 `InputWindowInfo` 新建 `BinderWindowHandle`。SurfaceFlinger 只在可见区域或 input info 变化等条件成立时提交，不是每个渲染帧都必调；但每次提交到 Dispatcher 的 handle 对象仍是新的。

若 Dispatcher 把新对象原样替换，focus map 与 `TouchState` 中的旧引用不会因此自动被判定为“窗口消失”：前者按 token 比较，后者按 `id + token` 查存续，反而会继续持有字段过期的旧 handle。hover 则直接比较对象指针，会被清空。复用旧对象既把最新 `mInfo` 刷进这些既有引用，也保住 hover 所需的指针身份。

因此 Dispatcher 先对当前 display 的旧列表建立：

```text
old id -> old handle object
```

新候选同时满足 `id` 相同、token 相同时，不保存新对象，而是：

```cpp
oldHandle->updateFrom(newHandle); // 只复制 mInfo
newHandles.push_back(oldHandle);  // 保留 old sp identity
```

这样 geometry、flags、focus 等字段能更新，内部对象身份又保持稳定。

### 两个字段各防一种误合并

| 新旧关系 | 是否复用 | 原因 |
|---|---:|---|
| 同 id、同 token | 是 | 同一 input window、同一接收连接的新快照 |
| 同 id、新 token | 否 | Layer 身份相同但 connection 已换代 |
| 新 id、同 token | 否 | 不同 input window 共享同一接收连接 |
| 新 id、新 token | 否 | 两层身份都不同 |

`id` 在主路径来自 Layer `sequence`，回答 input window 身份；普通 channel-backed 窗口的 token 来自 channel pair，portal 等特殊 handle 则可使用独立 Binder。`id + token` 不是在否定 id 的唯一性，而是 Dispatcher 判断“旧 handle 是否仍属于同一接收代次、可以复用”的复合键。

### 同 token 多窗口对反查的影响

`getWindowHandleLocked(token)` 遍历所有 display，遇到第一个 token 相同的 handle 就返回。它适合回答“是否仍有某个 handle 使用这条 connection”，却不能唯一定位某个 Layer。

这会影响按 token 查询的 connection 级功能，例如：

- 取某个 dispatch timeout；
- 为 ANR 生成窗口标签；
- HUP 时判断 `stillHaveWindowHandle`。

如果多个同 token handle 的属性不同，单 token 反查天然只能选其中一个。这些调用虽然以 connection token 为入口，部分调用者仍会读取首个匹配 window 的 timeout、名称、frame 或 scale；源码没有协调多个共享 token handle 的这些属性，因此代表窗口的选择本身有歧义。

### 重复 id 依赖上游不变量

旧列表建表时，相同 id 后写覆盖；新列表也不显式去重。生产路径依赖 SurfaceFlinger Layer sequence 唯一。源码没有在 Dispatcher 再做一层 duplicate-id 拒绝，因此异常输入的结果不能描述成“会安全报错并保留旧列表”。

源码锚点：`Layer.cpp:2368-2383`、`InputWindow.cpp:150-168`、`InputDispatcher.cpp:525-530,3570-3603,3614-3670`、`DisplayContent.java:5176-5191`。

## 10. 安装新列表后，焦点、hover 和 TouchState 怎样收尾

### 精确顺序先安装新主表，最后才清旧 token

`setInputWindowsLocked(list, displayId)` 在同一把 Dispatcher 锁内按以下顺序运行：

```text
复制该 display 的旧 handles
  -> 过滤/复用并安装新主列表
  -> 选择新焦点、检查 hover
  -> 处理焦点 token 变化
  -> 清该 display 的 TouchState.windows
  -> 对真正消失的旧 handle 调 releaseChannel()
```

先清 focus/touch，后清旧 handle token 是有意顺序：取消阶段仍需拿旧 token 查 `InputChannel`。

一次 `setInputWindows(handlesPerDisplay)` 从遍历所有 display 到完成始终持有同一把 Dispatcher 锁，因此其他 Dispatcher 操作不会观察到“只更新了一半 display”的中间状态：对外可见性发生在解锁之后。锁内仍是按 `unordered_map` 的不确定顺序逐 display 执行；涉及全 display 查询或全局引用的清理会互相影响，异常的跨 display 同一 `id + token` 迁移尤其可能出现顺序差异。它是原子可见的串行更新，不是各 display 同时计算后再一次提交。

### 焦点按 token 判断“接收端是否变化”

新列表中第一个 `hasFocus && visible` 的 handle 成为候选。若 `haveSameToken(old, new)` 为 false：

1. 旧焦点存在时，按旧 token 查 channel；
2. **只有 channel 仍存在**，才合成 `CANCEL_NON_POINTER_EVENTS` 并入队 `Focus(false)`；
3. 从 focused map 删除旧项；
4. 新焦点存在时，先写 focused map，再无条件创建 `Focus(true)`；
5. 仅当本 display 是全局 `mFocusedDisplayId`，才排队 policy focus-change command。

`Focus(true)` 的创建无条件，不代表必能投递。之后 `dispatchFocusLocked()` 若按 token 找不到 channel，会把它当作窗口已离开并直接返回。

old/new handle 不同但 token 相同时，不发 focus 事件、不通知 policy，也不替换 focused map。正常同 id+token 刷新由于旧对象已复用而安全；若焦点迁到“不同 id、相同 token”的 handle，focused map 会继续指向旧对象。若旧 `id + token` 已同时从所有 display 消失，末尾还会对该旧对象执行 `releaseChannel()`，focused map 因而可能留下 token 已清空的陈旧 handle；若旧 handle 仍以非焦点窗口留在某个快照，它不会被 release，但 focused map 仍是旧对象。源码允许共享 token，这里的边界只是焦点迁移按接收 token 而非 window id 判断，Dispatcher 不会替调用者协调两种窗口身份。

这里只取消 non-pointer，正是因为触摸归属由 `TouchState` 从 DOWN 到手势结束保持；键焦点变化不应擅自把正在进行的 pointer 手势改送新窗口。

### r48 的 hover 引用是全局单值

`mLastHoverWindowHandle` 不是按 display 分表。每次处理一个 display，代码只在这个 display 的新列表中寻找全局旧 hover 对象；若没找到就清空。

于是只更新另一个 display，也可能清掉真正 hover display 的对象。一次 multi-display 提交只要还包含任一不拥有该对象的 display，在正常“对象只属于一个 display”的前提下，最终都会把它清掉，并不取决于 `unordered_map` 顺序；一旦先清为 null，后面的 display 也没有把它重新赋回的逻辑。这个清空动作本身不合成 `HOVER_EXIT`。

第 189 章已经说明：`InputState` 有能力把命中的 hover memento 合成为 `HOVER_EXIT`，但“存在生成能力”与“此处清指针必然调用它”不是一回事。

### touched-window 移除只清 `TouchState.windows`

对当前 display 的每个 `TouchedWindow`，若 `hasWindowHandleLocked(oldHandle)` 为 false：

```text
按旧 token 找 channel
  -> 找到则合成 CANCEL_POINTER_EVENTS
  -> 无论 channel 是否找到，都从 state.windows 删除该项
```

这个 cancellation 没有设置 deviceId 或 displayId filter；它会匹配该 Connection 的所有 pointer-class memento。若多个 handles 共用 token，移除其中一个 handle 可能取消整个 connection 的 pointer `InputState`，包括仍存在 handle 的流。

函数也没有在这里重置 `TouchState.down`、`portalWindows` 或 `gestureMonitors`。Portal 引用可继续被正在进行的 TouchState 持有，直到后续 `reset()` 等路径清理。

### `hasWindowHandleLocked()` 的 display 检查只记录日志

该函数遍历所有 display，以 `id + token` 匹配。若找到的 map key 与传入旧 handle 自报 display 不同，只打印错误，仍返回 true。

所以它回答的不是“同一 display 中是否存在”，而是“所有当前 display 列表里是否还有同 id+token”。异常迁移到另一 display 的旧引用可能因此被视为仍存在，不触发本轮取消或 `releaseChannel()`。

源码锚点：`InputDispatcher.h:370-383`、`InputDispatcher.cpp:245-255,1077-1108,3672-3793`、`TouchState.h:29-55`、`TouchState.cpp:32-51`。

## 11. `releaseChannel()` 为什么既不 release channel，也不注销 Connection

### 实现只有一行

```cpp
void InputWindowHandle::releaseChannel() {
    mInfo.token.clear();
}
```

它只清这一份 native handle 快照持有的 Binder token 强引用。它不会：

- close socket fd；
- dispose Java `InputChannel`；
- 删除 `mConnectionsByFd`；
- 删除 `mInputChannelsByToken`；
- 调 `unregisterInputChannel()`；
- 清 WMS Java `InputWindowHandle.token`；
- 删除 WMS 的 `mInputToWindowMap`；
- 清 handle 的名称、frame、flags 等其他 `InputWindowInfo` 字段。

调用名带 `Channel` 是历史语义，r48 的 handle 实际只保存连接 token，不保存 `sp<InputChannel>`。

### 为什么仍要及时清 token

旧 handle 可能被局部 `sp<>`、focus map 或 TouchState 暂时持有，不能假设主窗口列表替换后立即析构。主动清 token 可及时释放这份旧快照的 Binder 强引用，并让继续误用旧 handle 时无法再按原 token 路由。

但同一对象若仍被多个内部结构引用，它们都会观察到 token 已清。这里没有复制一份“只对主列表不可见”的私有字段。

### 窗口快照消失与注册端存活的合法组合

```text
新 setInputWindows 不再包含 handle
  -> focus/touch 收尾
  -> oldHandle.releaseChannel()
  -> Dispatcher token/fd 两张注册表可仍保留 Connection
  -> Java WindowState 的 server wrapper 也可仍存在
```

此时新事件不会再从该 handle 命中，但 Looper 仍可读取旧在途事件的 FINISHED，或者等待 WMS 稍后显式注销。这不是单凭一张表就能判为泄漏的状态。

反向组合也可能短暂出现：WMS 已注销 Connection，但旧 SurfaceFlinger 快照尚未被新 transaction 替换。`addWindowTargetLocked()` 即使看到 handle，按 token 找不到 channel 时也会记录“already unregistered”并跳过 target。

### WMS 的 Java token 不由 native release 修改

SurfaceFlinger → InputFlinger 路径传的是 `InputWindowInfo` 值拷贝。Dispatcher 在旧 `BinderWindowHandle` 上清 token，不会反向修改 WMS 的 Java `mInputWindowHandle.token`。

Java token 真正置 null 在 `WindowState.disposeInputChannel()` 末尾；在这之前还要用当前 token 删除 key interception 与 `mInputToWindowMap` 条目。

源码锚点：`InputWindow.cpp:150-168`、`InputDispatcher.cpp:1999-2029,3781-3791`、`WindowState.java:2490-2510`。

## 12. 正常销毁为何要按分支看，而不是背一句“server 总是先关”

### 普通 App 主路径刻意先请求 WMS remove

`ViewRootImpl` 销毁时的顺序是：

```text
mWindowSession.remove(mWindow)
  -> WMS WindowState.removeIfPossible()
  -> disposeInputChannel()
  -> Dispatcher unregister server

返回 App
  -> mInputEventReceiver.dispose()
  -> dispose client InputChannel
```

源码注释明确解释动机：若先 dispose client，server 可能把 peer close 观察成 broken channel。因此普通正常路径先让 WMS 注销 server，再关闭 App consumer。

若 Binder `remove()` 本身发生 `RemoteException`，App 仍会继续 dispose receiver；这时上述“先由 WMS 正常注销”的顺序不再有保证。若 Dispatcher 仍然存活，peer close 可由 HUP/DEAD_OBJECT 路径触发自动清理，但源码不能据此断言这是唯一的后续清理来源。

### WMS 可能先撤输入，再保留画面做动画

`WindowState.removeIfPossible()` 在决定是否延迟 surface removal 之前就调用 `disposeInputChannel()`。若退出动画成立，会设置 `mRemoveOnExit` 并保留 WindowState/surface；相应注释也说 input channel 已经 gone，需要更新焦点与输入窗口。

因此“画面还在但已经点不动”不是反常竞态，而是正常设计：退出动画是视觉生命周期，输入资格已先撤销。

### 普通窗口的 `disposeInputChannel()` 顺序

普通应用打开时，WMS 已把 client transfer 给 App 并把本地 `mClientChannel` 置 null。销毁时有效动作是：

```text
unregister mInputChannel(server)
  -> dispose server Java wrapper
  -> 清 key interception / mInputToWindowMap
  -> Java handle token = null
```

这里的 unregister 与 dispose 仍是两步：前者逻辑移除 Dispatcher 注册，后者释放 WMS Java wrapper 的 native 持有。fd 是否在 dispose 当刻关闭，还取决于是否存在额外 native 引用或 dup。

### dead-window dummy 分支是明确例外

WMS 真正还保留 client 的分支是 `DeadWindowEventReceiver`。其销毁顺序实际为：

```text
mDeadWindowEventReceiver.dispose()
  -> InputEventReceiver.dispose()
  -> dispose dummy client wrapper
  -> unregister server
  -> dispose server wrapper
  -> 再次 mClientChannel.dispose()（该 wrapper 已失效，实际 no-op）
```

所以不能把 `WindowState` 中“unregister server first”的行内注释提升为所有分支的绝对顺序。它准确描述普通 server 分支的意图，但 dead-window receiver 在源码顺序上先被 dispose。

### 自动 dispose callback 只是普通注册 wrapper 的安全网

成功的 `nativeRegisterInputChannel()` 会把 callback 安装在那个具体 Java/native server wrapper 上。若调用者忘记显式 unregister 就 `dispose()` 或被 finalizer 清理：

```text
nativeDispose()
  -> callback 记录“disposed without unregister”警告
  -> 用该 wrapper 内的 InputChannel 补调 unregister
  -> 清 mPtr 并删除 NativeInputChannel wrapper
```

正常 `nativeUnregisterInputChannel()` 先移除 callback，再注销，避免随后 `dispose()` 重复补调。已注销的 `BAD_VALUE` 被 JNI 忽略，其他状态才转异常。

这个安全网有严格边界：

- monitor 注册不安装同一 callback；
- `release()` 不调用 callback；
- `dup()` 的新 wrapper 不继承 callback；
- callback 只属于那个 wrapper，不属于“所有同 token 对象”；
- dispose 一个 wrapper 不保证所有 fd 副本关闭。

### 注销必须传回实际注册的 endpoint

`unregisterInputChannelLocked(inputChannel)` 先按参数的 token 找 Connection，但移除 Looper 时使用的是**参数自己的 fd**：

```cpp
Connection c = getConnectionLocked(inputChannel->getConnectionToken());
removeConnectionLocked(c);
mInputChannelsByToken.erase(inputChannel->getConnectionToken());
mLooper->removeFd(inputChannel->getFd());
```

因此“token 相同”不足以让 peer 或任意 dup 成为完全可替换的 unregister 句柄。若误传 client 端或同端 dup，Connection map 会按 token 被移除，却可能对错误 fd 调 `removeFd()`，从而留下原 Looper request；若原 fd 后来触发 callback，spurious 分支才会返回 0 自清。

正常 WindowState 保存并传回原 server 对象，dispose callback 也拿同一 wrapper 的 channel，满足这项隐含不变量。

源码锚点：`ViewRootImpl.java:4660-4690`、`Session.java:191-194`、`WindowManagerService.java:1896-1906`、`WindowState.java:2160-2355,2490-2510`、`InputEventReceiver.java:88-114`、`com_android_server_input_InputManagerService.cpp:1343-1420`。

## 13. 注销究竟清了什么，为什么最后才叫 ZOMBIE

### native 注销的精确顺序

`unregisterInputChannelLocked()` 在 Dispatcher 锁内：

```text
按 token 找 Connection；找不到则 BAD_VALUE
  -> erase 该 token 的 ANR tracker 项
  -> 从 fd->Connection 表移除
  -> 从 token->InputChannel 表移除
  -> monitor 才额外清 monitor 列表
  -> Looper.removeFd(参数 fd)
  -> abortBrokenDispatchCycleLocked()
  -> status = ZOMBIE
```

从两张注册表移除后，正常路由查找已不能取得这条 Connection。局部变量、command 或其他 `sp<Connection>` 仍可让对象存活，所以 `ZOMBIE` 表示“已经注销”，不等于 C++ 对象已经析构，更不等于 fd 已关闭。

### 为什么正常注销内部会短暂写 BROKEN

`abortBrokenDispatchCycleLocked()` 先无条件 drain outbound 与 wait；若当前状态是 `NORMAL`，再写 `BROKEN`。显式 unregister 传 `notify=false`，随后同一持锁调用立刻覆写为 `ZOMBIE`：

```text
正常显式注销：NORMAL -> BROKEN -> ZOMBIE
已有发送故障再注销：BROKEN -> ZOMBIE
```

第一次转移只是复用“drain 并停止发送”的实现步骤，不能把正常注销诊断为一次对外可见的真实断链。稳定的生命周期语义仍是 NORMAL → 已注销。

### drain 释放两条队列，但不做这些事

`abortBrokenDispatchCycleLocked()` 会：

- 依次释放 outbound/wait 中每个 `DispatchEntry`；
- 对 foreground target 递减 injection pending 计数；
- 在计数归零时唤醒 `mInjectionSyncFinished`；
- 必要时把 `NORMAL` 改成 `BROKEN` 并排一个 policy command。

它不会：

- 从 fd/token map 注销 Connection；
- 移除 Looper callback；
- 清窗口、focus 或 `TouchState`；
- 调 `InputState::clear()`；
- 合成取消事件；
- 擦除 ANR tracker；
- 把已成功的 `injectionResult` 改成失败。

ANR tracker 的删除属于 `removeConnectionLocked()`，因此显式 unregister 会在 drain 前清 tracker；单独的发送端 broken abort 则不会。

### `WAIT_FOR_FINISHED` 成功不等于 App 回过 FINISHED

注入在选中前台 target 时先把 `pendingForegroundDispatches` 加一。正常 FINISHED 会最终释放 entry 并减一；但 broken/unregister drain 释放同一个 entry 时也减一。

如果路由阶段已经把 `injectionResult` 设为 `SUCCEEDED`，随后连接断开并 drain 到 pending=0，等待线程会被唤醒并保留原 success：

```text
WAIT_FOR_FINISHED
  -> 路由成功
  -> foreground pending = 1
  -> App 尚未 FINISHED
  -> unregister / broken drain
  -> pending = 0
  -> 返回 SUCCEEDED
```

这个 success 表示同步账不再等待任何前台 entry，不证明 App 消费、handled、View 回调或屏幕 present。第 177、188 章的完成点边界在 channel 生命周期里再次出现。

### 析构不是队列的兜底 drain

`Connection::~Connection()` 是空实现，队列元素又是 raw `DispatchEntry*`。正常不变量要求 unregister/broken 路径在对象最后释放前完成 drain；不能把“最后一个 `sp<Connection>` 消失”当作额外的队列清理阶段。

普通 `prepareDispatchCycleLocked()` 与 `startDispatchCycleLocked()` 都会用 status 阻止 BROKEN/ZOMBIE 正常派发。但若内部帮助函数拿着脱离注册表的 retained ZOMBIE 并直接追加 raw entry，析构本身没有补救 drain；这是第 189 章已经审计过的边缘风险。

源码锚点：`InputDispatcher.cpp:2209-2235,2456-2613,2655-2694,3418-3467,3547-3562,4359-4401,4506-4509`、`Connection.cpp:23-31`。

## 14. 发送失败与接收失败必须画成两条状态线

### `WOULD_BLOCK` 不总是 BROKEN

publish 返回 `WOULD_BLOCK` 时，Dispatcher 先看 wait queue：

| 条件 | 判断 | 结果 |
|---|---|---|
| wait 非空 | 已有消息在途，pipe 满是正常背压 | 保持 `NORMAL`，outbound 留队，等 FINISHED 后重试 |
| wait 为空 | pipe 理应为空却写不进，是不变量异常 | drain，两队列清空，`NORMAL → BROKEN`，排 broken command |
| 其他非零错误 | 不可恢复的 publish 失败 | 同样进入 `BROKEN` |

send 失败路径只调用 `abortBrokenDispatchCycleLocked()`，没有注销：

```text
publish fatal
  -> drain outbound + wait
  -> status = BROKEN
  -> fd/token maps 仍在
  -> Looper callback 仍在
  -> policy broken command 排队
```

所以 dump 中看见 `BROKEN` Connection 仍挂在 fd 表是预期中间态，不是状态机自相矛盾。

### BROKEN 与 ANR 回答不同问题

| 状态 | 事实 | 不代表什么 |
|---|---|---|
| `responsive=false` | 某个 ANR tracker deadline 到期后，Dispatcher 已把连接标为不响应 | 当前 wait 必然仍存在、policy ANR 必然已发出，或 socket 必然损坏 |
| connection policy ANR | `onAnrLocked(connection)` 确认 wait 仍非空并排出通知 | transport 必然已经 BROKEN |
| `BROKEN` | publish/transport 出现不可恢复错误 | App 一定先发生 ANR |
| `ZOMBIE` | Dispatcher 已逻辑注销 | fd 一定已经 close |

`cancelEventsForAnrLocked()` 的注释明确说这里不 break connection；若 policy 决定关闭 App，稍后通过窗口/channel removal 注销。

### 单独 broken abort 留下哪些延迟清理

发送失败后 window handles、focus、TouchState、token/fd maps 与 `InputState` 都还可能存在。新目标仍可能按 token 找到 channel 与 Connection，但 `prepareDispatchCycleLocked()` 因 status 非 NORMAL 而跳过投递。

如果 fatal 前 wait queue 已有项，drain 不直接从 `mAnrTracker` 擦除对应 deadline。通常后续 WMS unregister 会用 `eraseToken()` 清掉；若没有及时注销，`processAnrsLocked()` 到期后会先把 retained BROKEN Connection 的 `responsive` 设为 false，再擦 token。随后 `onAnrLocked()` 因 wait 已空而直接返回，不排 policy ANR 通知。结果可以是 `BROKEN + responsive=false + 空 wait`，却没有真正上报 ANR。

因此“waitQueue 已空”也不严格推出“ANR tracker 此刻已空”。这是 r48 的短暂不同步窗口。

### broken policy command 如何安全回到 WMS

command queue 不是另一条专用 worker；Dispatcher 在自己的循环中稍后执行 command。`doNotifyInputChannelBrokenLockedInterruptible()`：

```text
持 Dispatcher 锁检查 status != ZOMBIE
  -> 解 Dispatcher 锁
  -> 同步调用 native policy / JNI / IMS / WMS
  -> 重新加 Dispatcher 锁
```

WMS 的 `InputManagerCallback.notifyInputChannelBroken(token)` 在 `mInputToWindowMap` 查普通 `WindowState`，找到就 `removeIfPossible()`。该移除会同步回调 Dispatcher unregister；因为原 command 已解锁，不会形成同一把 Dispatcher 锁的自锁。

典型重入时间线是：

```text
publish fatal，Connection=BROKEN
  -> command 检查尚非 ZOMBIE
  -> 解锁并进入 WMS
  -> WMS disposeInputChannel
  -> Dispatcher unregister：BROKEN -> ZOMBIE
  -> 回到原 command，再加锁
```

检查只发生在解锁前；Java 回调进行中若状态变成 ZOMBIE，不会撤销已经开始的回调。

若 token 没有对应 `WindowState`、Java 回调抛异常被 JNI 清掉，或所有者不是普通窗口，这条通知本身不会执行一个通用的兜底 unregister。Connection 可保持 BROKEN，直到所有者或后续 HUP 清理。

源码锚点：`InputDispatcher.cpp:489-530,1364-1377,2456-2678,4523-4552,4630-4640`、`com_android_server_input_InputManagerService.cpp:724-738`、`InputManagerService.java:1942-1945`、`InputManagerCallback.java:76-94`。

## 15. FINISHED、HUP 与 receive 错误为什么走另一条线

### 正常 readable 回调循环读 FINISHED

`handleReceiveCallback(fd, events, data)` 先确认 fd 仍在 `mConnectionsByFd`；已被注销却迟到的 callback 会打印 spurious 日志并返回 0，让 Looper 删除监听。

没有 ERROR/HANGUP 且包含 INPUT 时，它循环：

```text
receiveFinishedSignal(seq, handled)
  -> OK：finishDispatchCycleLocked() 只排 completion command
  -> 继续读下一条
  -> WOULD_BLOCK：说明已读空
```

只要读到过至少一条，就运行 command queue；终态为 `WOULD_BLOCK` 时返回 1，表示这次 callback 不主动要求 Looper 删除监听。若 command 执行期间解锁重入了 unregister，`removeFd()` 已经发生，后来的返回 1 不会把 request 加回来；只有未被重入注销时，已有监听才继续保留。这里的 `gotOne` 也是必要条件：若一次 INPUT callback 的第一读就是 `WOULD_BLOCK`，代码不会把它当作“正常读空”返回 1，而会落入错误路径并注销 connection。

同一轮收到多个 FINISHED 时，Dispatcher 用进入循环前取得的同一个 `currentTime` 作为 finish time。这是第 188 章已经分析过的计时口径。

### wait queue 真正删除发生在 deferred command

`finishDispatchCycleLocked()` 不直接按 seq 删 wait entry，只调用 `onDispatchCycleFinishedLocked()` 排命令。`doDispatchCycleFinishedLockedInterruptible()` 才：

1. 第一次查找 seq；
2. 执行 Key fallback 等 post-event policy；
3. policy 可能解锁并重入 WMS/Dispatcher；
4. 再次按 seq 查找；
5. 若 entry 仍存在，才删 wait、删 tracker、release 或 restart；
6. 尝试启动下一轮 outbound。

第二次查找不是多余防御。若 policy 解锁期间窗口被注销，unregister 已 drain wait；command 必须发现 entry 不在，避免二次释放。若 Connection 已变 BROKEN/ZOMBIE，restart 也受 status 门限制。

但二次查找只保护 wait entry 的后续删除，并没有覆盖所有 post-policy 副作用。`afterKeyEventLockedInterruptible()` 的“已有 fallback 需要取消”分支会解锁调用 policy，回锁后不复检 status，直接调用 `synthesizeCancelationEventsForConnectionLocked()`；该 helper 只拒绝 BROKEN，不拒绝 ZOMBIE。若这段解锁期间恰好 unregister，retained ZOMBIE Connection 仍可能被追加 cancellation raw entry，而 `startDispatchCycleLocked()` 又因 status 非 NORMAL 拒绝发送。结合空析构，这正是“脱表对象仍可能滞留队列项”的具体边缘，而不能笼统说二次查找让整个 FINISHED 重入都安全。

### ERROR/HANGUP 优先于 INPUT

若 events 同时含 ERROR 或 HANGUP，代码不先读可能存在的 FINISHED，而是走异常分支。它只为日志/notify 计算：

```cpp
stillHaveWindowHandle = getWindowHandleLocked(token) != nullptr;
notify = !connection->monitor && stillHaveWindowHandle;
```

普通 connection 即使已经没有窗口 handle，仍会被立即注销，只是少一条“consumer closed/error”告警和 broken notify 请求。任意同 token handle 存在，就足以让 `stillHaveWindowHandle` 为 true。

### 普通 read failure 与 HUP 的 notify 规则不同

若没有 ERROR/HUP，而 `receiveFinishedSignal()` 最终返回非 `WOULD_BLOCK`：

```cpp
notify = status != DEAD_OBJECT || !connection->monitor;
```

因此：

- 普通 channel 对任何 read failure 都请求 notify；
- monitor 只对精确 `DEAD_OBJECT` 抑制，其他协议/读取错误仍记录；
- HUP 分支则对所有 monitor 抑制，并要求普通 channel 仍有某个 window handle。

收到非 `FINISHED` message 会在 `InputPublisher` 返回 `UNKNOWN_ERROR`，也进入这条注销线。

### receive/HUP 会立即注销，不稳定停在 BROKEN

无论上述 `notify` 是真是假，回调最后都调用内部 unregister：

```text
remove fd/token maps
  -> remove Looper fd
  -> drain queues
  -> NORMAL 时短暂变 BROKEN
  -> 立即 status = ZOMBIE
  -> callback 返回 0
```

这与发送 fatal 截然不同：receive/HUP 不会把一个仍注册的 BROKEN 留给 WMS 后续清理。

若 `notify=true`，unregister 中的 abort 确实会排 broken command；但同一持锁函数紧接着把状态写成 ZOMBIE。command 后来执行时，`status != ZOMBIE` 门必然失败。因此由这一次 receive-side unregister 自己排出的 policy 通知不会到达 WMS。

这里的 `notify` 仍影响日志与“是否排过命令”的内部行为，却不能改写成“Java/WMS 必然收到 broken callback”。能稳定走 `BROKEN → policy → WMS unregister` 的典型入口是发送端 fatal，而不是 receive/HUP。

### 三条状态线不要再合并

```text
正常背压：
NORMAL -- WOULD_BLOCK + wait非空 --> NORMAL -- FINISHED --> 继续 publish

发送端硬错误：
NORMAL -- publish fatal --> BROKEN（仍注册）
       -- policy/WMS 或后续HUP --> ZOMBIE（已注销）

接收端错误：
NORMAL -- read error / HUP --> [短暂 BROKEN] --> ZOMBIE（立即注销）
```

源码锚点：`InputTransport.cpp:575-595`、`InputDispatcher.cpp:2638-2864,4379-4401,4751-4859`、`Looper.cpp:341-358,513-542`。

## 16. 用状态矩阵排障，并完成九个只读练习

### 先按事实定位，不先猜“窗口死了”

| 观察组合 | 更接近的解释 | 下一步证据 |
|---|---|---|
| 画面仍在，Connection 不在 | 输入已先撤销，surface 可能在退出动画 | 看 WMS `mRemoveOnExit/mAnimatingExit` 与 remove 时序 |
| Window handle 不在，Connection `NORMAL` | 路由快照已移除，显式 unregister 可能尚未来 | 看 WindowState server wrapper 与后续 dispose |
| Window handle 在，token 查不到 channel | 注册先撤销、窗口快照尚未刷新 | 看 `addWindowTargetLocked` 的 unregistered 警告 |
| Connection `BROKEN` 且两队列空 | 发送 fatal 已 abort，但尚未正式注销 | 看 broken 日志、WMS callback 与随后 ZOMBIE 清理 |
| Connections 中完全没有该项 | 只能证明已从 fd map 移除 | 不能推出所有 wrapper/dup fd 都已 close |
| `WAIT_FOR_FINISHED` success，但客户端无回执证据 | 可能由 drain 把 foreground pending 归零 | 对照 broken/unregister 时间和 wait queue |
| ANR 但 status 仍 `NORMAL` | 处理超时，不是 transport 已坏 | 看 `responsive`、wait age 与 ANR tracker |

`dumpsys input` 的 r48 文本会列 display windows、focus、TouchState，以及每个 connection 的本地 fd、channelName、status、responsive、outbound/wait。它不会在这些打印行中直接给出完整的 id/token join，因此名称相似只能用于缩小范围，不能替代 token 证据。需要精确关联时，应结合日志、trace、调试器或有目的的临时埋点。

以下练习都只读取本地 Android 11 r48 源码，不要求编译。

### 练习 1：证明 pair 两端共享 token，但不是同一个 endpoint

```bash
nl -ba frameworks/native/include/input/InputTransport.h | sed -n '177,249p'
nl -ba frameworks/native/libs/input/InputTransport.cpp | sed -n '249,303p'
nl -ba frameworks/native/services/inputflinger/dispatcher/Connection.cpp | sed -n '23,29p'
nl -ba frameworks/base/core/jni/android_view_InputEventReceiver.cpp | sed -n '56,100p'
```

记录 `socketpair` 类型、`new BBinder()` 次数、两个 `unique_fd` 和两端命名。观察标准：能解释“等价端点”为什么仍在输入协议中承担 publisher/consumer 不同角色。

### 练习 2：区分 transfer、Parcel 与 dup

```bash
nl -ba frameworks/base/core/jni/android_view_InputChannel.cpp | sed -n '163,283p'
nl -ba frameworks/native/libs/input/InputTransport.cpp | sed -n '383,425p'
nl -ba frameworks/native/libs/binder/Parcel.cpp | sed -n '1127,1161p;2078,2092p'
```

观察标准：指出哪条路径移动 `NativeInputChannel*`、哪条调用 `dup()`、哪条复制 Parcel fd，以及它们为何都保留 connection token。

### 练习 3：画出 WindowState 的两个 client 分支

```bash
nl -ba frameworks/base/services/core/java/com/android/server/wm/WindowState.java | sed -n '2453,2510p'
nl -ba frameworks/base/core/java/android/view/InputEventReceiver.java | sed -n '88,114p'
```

分别画普通 App 与 `DeadWindowEventReceiver`。观察标准：普通分支销毁时 WMS 没有 client；dummy 分支却先由 receiver dispose client，再 unregister server。

### 练习 4：恢复窗口快照的真实跨进程路径

```bash
nl -ba frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp | sed -n '2897,2929p'
nl -ba frameworks/native/services/inputflinger/InputManager.cpp | sed -n '94,119p'
nl -ba frameworks/native/libs/input/IInputFlinger.cpp | sed -n '28,46p'
```

观察标准：找出 reverse-Z、按 display 分组、oneway 与 listener 的位置，并写下 listener 能证明和不能证明的完成点。

### 练习 5：验证注册的两张表、Looper 与未检查返回值

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '2696,2710p;3606,3612p;4299,4325p;4491,4509p'
nl -ba system/core/libutils/Looper.cpp | sed -n '426,510p'
```

观察标准：解释 token 查重为何是线性、fd/token 两张表各服务什么，并确认 `addFd()` 可失败而调用方没有读取其返回值。

### 练习 6：验证过滤与 handle 复用顺序

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '3565,3670p'
nl -ba frameworks/native/services/surfaceflinger/Layer.cpp | sed -n '2368,2384p'
```

观察标准：列出候选检查顺序，并回答同 id 新 token、同 token 新 id、输入 vector 为空和过滤后为空分别怎样处理。

### 练习 7：逐行核对 focus、hover、touch 与 release

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '3672,3793p'
nl -ba frameworks/native/libs/input/InputWindow.cpp | sed -n '150,168p'
nl -ba frameworks/native/services/inputflinger/dispatcher/CancelationOptions.h | sed -n '25,48p'
```

观察标准：找到旧焦点 `Focus(false)` 的 channel 条件、全局 hover 被清的位置、pointer cancel 缺少的过滤字段，以及 `releaseChannel()` 唯一修改的成员。

### 练习 8：证明 unregister、drain 与 fd close 是三个完成点

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '2655,2694p;4359,4401p;4506,4509p'
nl -ba frameworks/native/services/inputflinger/dispatcher/Connection.h | sed -n '27,60p'
nl -ba frameworks/native/services/inputflinger/dispatcher/Connection.cpp | sed -n '23,31p'
nl -ba frameworks/native/include/input/InputTransport.h | sed -n '177,184p'
```

观察标准：写出 map、ANR tracker、Looper、队列、status 的修改顺序，并说明为何空析构不能作为 raw queue 的兜底清理。

### 练习 9：分开推演 send fatal 与 receive/HUP

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '2456,2678p;2696,2762p;4379,4401p'
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp | sed -n '4523,4531p;4630,4640p'
```

观察标准：画出两条状态线；指出哪条保留注册中的 BROKEN，哪条立即 ZOMBIE，以及为什么 receive 侧自己排的 broken command 最终过不了 ZOMBIE 门。

### 检查题与答案

1. **为什么 token 不能唯一标识窗口？**
   在普通 channel-backed 路径中，它承接 channel pair/connection 身份，server、client、dup 与多个 input windows 都可共享；portal 还可使用不对应已注册 channel 的独立 Binder。input window 唯一身份是 `id`，刷新存续判断使用 `id + token`。

2. **为什么窗口快照要复用旧 handle 对象？**
   因为 focus、TouchState 与 hover 保存 `sp<InputWindowHandle>` 指针。每次 SF 提交快照都会新建 BinderWindowHandle；若不按同 id+token 把信息拷回旧对象，focus/touch 会保留字段过期的旧引用，hover 则因指针不等而被清空。

3. **为什么 `releaseChannel()` 后 fd 还可能存在？**
   因为它只执行 `mInfo.token.clear()`；注册表、Connection、Java wrapper、native `InputChannel` 与 `unique_fd` 都由其他路径管理。

4. **正常 App 销毁为何先经 WMS remove，再 dispose receiver？**
   为了让 Dispatcher 先注销 server；否则 client close 会让 server 收到 HUP/DEAD_OBJECT，正常销毁会被观察成异常断链。

5. **为什么 send fatal 和 receive HUP 不能画成一条 BROKEN 路径？**
   send fatal 只 abort，保留 maps/Looper 与稳定 BROKEN，等待 policy/WMS；receive/HUP 直接调用 unregister，短暂 BROKEN 后立刻 ZOMBIE。

6. **`WAIT_FOR_FINISHED` success 能证明 App 回了 FINISHED 吗？**
   不能。broken/unregister drain 也会释放 foreground entry、把 pending 递减到零，同时不改写已经成功的 injection result。

### 最后只保留这张心智图

```text
Layer/InputWindow id ──标识 input window
          │
          ├─ handle.token ──路由接收身份
          │                    ├─ 普通窗口 -> registered InputChannel / Connection
          │                    │                         ├─ outbound / wait / InputState
          │                    │                         └─ fd map / Looper -> FINISHED、HUP
          │                    └─ portal 可没有已注册 channel
          │
          └─ display ordered list

窗口路由移除 ──不等于── unregister
unregister ─────不等于── fd close
fd close ───────不等于── App 已收到取消或 FINISHED
画面消失/保留 ─不等于── 上述任一传输完成点
```

下一章继续精读 **Input Monitor、Gesture Monitor 与 `pilferPointers()`**：普通 monitor 与 gesture monitor 怎样按 display 注册，为什么旁听与抢流是两种语义，以及 monitor 生命周期为何不能直接套用普通 `WindowState` 的 dispose callback 规则。
