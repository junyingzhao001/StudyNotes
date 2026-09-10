# 191 Android Input Monitor、Gesture Monitor 与 pilferPointers：旁听者何时能让 App 收到 CANCEL，却没有独占整条手势

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`
> 核心问题：global monitor 与 gesture monitor 分别在什么时候进入目标集合；`pilferPointers()` 究竟取消谁、保留谁，以及它的返回为什么不是“抢流已经完成”的证明？

---

## 1. 从“App 收到 CANCEL，系统手势却仍不是独占”开始

### 一个最容易画错的现场

设 display 0 上同时存在：

- 一个正常 App 窗口；
- 一个 global monitor；
- gesture monitor A；
- gesture monitor B。

手指按下时，四个接收者都可能看到 `DOWN`。A 根据自己已经收到的 `DOWN/MOVE` 判断这是系统手势，于是调用 `pilferPointers()`。接下来最直观的现象是：

```text
App window      DOWN ─ MOVE ─ CANCEL
global monitor  DOWN ─ MOVE ─────── MOVE ─ UP
gesture A       DOWN ─ MOVE ─ pilfer ─ MOVE ─ UP
gesture B       DOWN ─ MOVE ─────── MOVE ─ UP
```

但这张图只描述最普通的单指路径，不能推出“A 获得了独占所有权”。native 实现做的是：

1. 给当前 `TouchState.windows` 中的普通窗口连接尝试合成 pointer cancel；
2. 清空 `windows` 与 `portalWindows`；
3. 保留整个 `gestureMonitors`，也保留 `down`、`split`、`deviceId`、`source` 和 `displayId`。

所以 B 不会被赶走，A 不会重收一个新 `DOWN`，物理设备也不会停止上报。更反直觉的是：如果这条流已经是 split touch，后续 `ACTION_POINTER_DOWN` 仍可能重新命中并加入一个普通窗口。

### 一句话结论

> r48 的 global monitor 是“目标解析成功后逐事件追加的旁听目标”；gesture monitor 是“只在 `ACTION_DOWN` 快照选中并粘进 `TouchState` 的手势参与者”；`pilferPointers()` 只是取消并移除当时的普通窗口目标，既不转移硬件流，也不排他保留调用者。

### 本章要守住的四条边界

- “已经注册”不等于“被选入当前手势”；
- “已经成为 `InputTarget`”不等于“事件已经 publish 到 client”；
- “`pilferPointers()` 返回”不等于“App 已收到并处理 `CANCEL`”；
- “App 被取消”不等于“此后直到 `UP` 都绝不再出现普通窗口目标”。

本章延续第 190 章的 `InputChannel`、`Connection`、fd 与回执账，但不再展开 socket wire ABI。这里集中追踪 monitor 的创建、目标选择、`TouchState`、响应性、pilfer 与销毁。

源码主锚点：`InputManagerService.java:505-550,1861-1876,2337-2353`、`InputMonitor.java:26-78,91-159`、`InputDispatcher.cpp:849-861,1111-1305,1536-2053,2209-2613,4327-4488`、`TouchState.cpp:27-147`。

## 2. 先拆开三种“monitor”和四条生命线

### 三个同名概念不是同一个对象

| 名称 | 所在层 | 真正含义 |
|---|---|---|
| global monitor | Dispatcher 路由类型 | 成功解析 key/motion 目标后，按 display 追加的副本接收者 |
| gesture monitor | Dispatcher 路由类型 | `ACTION_DOWN` 时被选入 `TouchState.gestureMonitors` 的 pointer 手势接收者 |
| `android.view.InputMonitor` | Java Parcelable | gesture monitor 返回给调用者的 client `InputChannel` 与控制 Host 的组合 |
| `Connection.monitor` | Dispatcher 连接标志 | 表示该 Connection 属于 monitor，用于诊断、断链处理与注销时移出 monitor 表 |

global 路径返回的只是 `InputChannel`，并不返回 `InputMonitor`。反过来，Java 类名叫 `InputMonitor` 也不表示它能订阅所有 Dispatcher 事件；r48 的隐藏入口 `monitorGestureInput()` 创建的是 gesture monitor。

### 两类 monitor 的精确对照

| 维度 | Global monitor | Gesture monitor |
|---|---|---|
| 注册表 | `mGlobalMonitorsByDisplay` | `mGestureMonitorsByDisplay` |
| 进入目标的时机 | 每次成功解析 key/motion 后 | 仅 pointer `ACTION_DOWN` 时选中 |
| key | 成功找到 focused target 后可收到 | 不收到 |
| pointer motion | 成功目标解析后逐事件尝试追加 | DOWN 时入选后随 `TouchState` 延续 |
| non-pointer motion | 成功找到 focused target 后可收到 | 不收到 |
| hover / scroll | global 路径可逐事件追加 | 不进入 gesture monitor 快照 |
| 写入 `TouchState` | 否 | 是，元素类型为 `TouchedMonitor` |
| 能否让无窗口 DOWN 成功 | 单独不能 | 至少一个候选通过筛选时能 |
| 能否调用 pilfer | 不能 | 能，但必须属于 Dispatcher 查到的当前流 |
| responsive 筛选 | 追加目标时没有 | 新 DOWN 选择时有一次 |

“global 能收到 key/motion”仍要带上两个限定：目标解析必须成功，而且 Connection 自己的 `InputState` 必须接受该序列。一个中途注册的 global monitor 即使被追加到某个 `MOVE` 的目标中，也可能因没有此前 `DOWN` memento 而在入队前丢掉这次事件。

### 四条生命线必须分别取证

| 生命线 | 关键容器或动作 | 它回答的问题 |
|---|---|---|
| 注册线 | monitor map、token map、fd map、Looper watch | 这个 monitor 仍登记在 Dispatcher 吗 |
| 路由线 | 本次 `InputTarget`、`TouchState.gestureMonitors` | 它是否有资格参与这一个事件或当前手势 |
| 传输线 | `Connection.status`、`responsive`、outbound/wait queue、FINISHED | 事件有没有真正发出、对端是否跟得上 |
| 控制线 | `IInputMonitorHost` Binder、native pilfer/unregister | pilfer/dispose 请求走到了哪一个完成点 |

四条线不会原子变化。比如 unregister 已经删除注册表，但旧 `TouchState` 仍可暂存一个 `TouchedMonitor`；又比如 pilfer 已清窗口路由，合成 `CANCEL` 却可能尚在 outbound queue。

### monitor 不是 EventHub 原始监听器

monitor 复用 Dispatcher 到应用的 `InputTransport`。它通常看到的是已经经过 EventHub、InputReader 映射、policy 与 Dispatcher 目标决策后的 key 或 motion；注入事件也可进入 Dispatcher 并被 monitor 选中，所以不能把每个 monitor 事件都断言为 InputReader 产物。无论哪种来源，它都不是 Linux `input_event`、`EV_SYN` 或 `SYN_REPORT` 原始流。

如果问题是“驱动是否上报 `SYN_REPORT`”，应去 EventHub/InputReader 取证；如果问题是“Dispatcher 选中了谁、某个系统组件为何没 finish”，monitor 才处在合适的观察层。

## 3. Java 创建路径决定了权限、能力与两端所有权

### global monitor 是 system_server 内部入口

`InputManagerService.monitorInput()` 的核心顺序是：

```java
InputChannel[] inputChannels = InputChannel.openInputChannelPair(inputChannelName);
nativeRegisterInputMonitor(mPtr, inputChannels[0], displayId, false);
inputChannels[0].dispose();
return inputChannels[1];
```

它检查名字非空、`displayId >= 0`，但方法本身没有 `MONITOR_INPUT` 检查。这里不能推导“普通 App 可绕过权限”：r48 的 `IInputManager.aidl` 没有暴露这个方法，它是 system_server 直接调用的 Java 方法。

实际生产调用之一在 `DisplayContent`。每个 display 创建：

```text
InputManagerService.monitorInput(...)
  -> client InputChannel
  -> PointerEventDispatcher
  -> 多个 WindowManagerPolicyConstants.PointerEventListener
```

`PointerEventDispatcher` 只把 pointer-class `MotionEvent` 转给监听者，所以 WMS 的这个消费者会忽略它收到的 key 或其他输入；但 `finally` 中仍对每个事件调用 `finishInputEvent()`。

这也区分了“global monitor 在 native 层能收到哪些类型”与“某个具体 Java consumer 会使用哪些类型”。

### gesture monitor 是隐藏 Binder API

`IInputManager.aidl` 暴露：

```java
InputMonitor monitorGestureInput(String name, int displayId);
```

`InputManager.monitorGestureInput()` 是 `@hide` 包装。IMS 的 Binder 方法先调用 `checkCallingPermission(MONITOR_INPUT, ...)`，再检查名字和 display，随后清除 calling identity，创建 pair、Host 并注册：

```java
InputChannel[] inputChannels = InputChannel.openInputChannelPair(inputChannelName);
InputMonitorHost host = new InputMonitorHost(inputChannels[0]);
nativeRegisterInputMonitor(mPtr, inputChannels[0], displayId, true);
return new InputMonitor(inputChannels[1], host);
```

`MONITOR_INPUT` 在框架 manifest 中是 signature 权限。`checkCallingPermission()` 还明确允许与 system_server 同 PID 的直接调用；因此准确模型是“外进程创建需要 signature 权限，同进程调用直接通过”，而不是无条件每次鉴权。

### 跨进程返回会转移发送侧的 client wrapper

gesture 方法的返回值是 Parcelable。AIDL 生成的 Stub 对返回值使用 `PARCELABLE_WRITE_RETURN_VALUE`；`InputMonitor.writeToParcel()` 把这个 flags 原样传给内部 client `InputChannel`。后者先把 name、shared token 与 dup 后的 fd 写进 Parcel，再因 return-value flag 调用自己的 `dispose()`。

因此常见跨进程返回的所有权变化是：

```text
system_server 临时 client wrapper
  -> fd dup 进 reply Parcel
  -> 发送侧 client wrapper dispose
  -> 调用进程从 Parcel 建立新的 client InputChannel

InputMonitorHost
  -> 仍在 system_server 持有 server wrapper
```

这里转移的是 client endpoint 的可用 fd 持有，不会生成新 pair token。同进程 local-interface 直接返回 Java 对象，不经过这次 Parcel/return-value dispose；排查 fd 数量时要先确认 Binder 是否真的跨进程。

### 权限只在创建时铸造 capability

`InputMonitorHost` 的两个方法没有再次检查调用 UID：

```java
public void pilferPointers() {
    nativePilferPointers(mPtr, mInputChannel.getToken());
}

public void dispose() {
    nativeUnregisterInputChannel(mPtr, mInputChannel);
    mInputChannel.dispose();
}
```

`InputMonitor` 又是 Parcelable：它把 client channel 与 `IInputMonitorHost` 的强 Binder 一起写入 Parcel。于是权限模型应理解为：

```text
MONITOR_INPUT / same-process gate
             │
             ▼
创建一次 InputMonitor capability
             │
             ├── client InputChannel：接收事件
             └── Host Binder：pilfer / dispose
```

谁被合法转授这个 `InputMonitor` 或 Host，谁就能调用这两个控制方法；Host 不按最初创建者 UID 复查。这里的安全边界是 capability 的持有与转授。

### pair token 不是“server 私钥”

server/client 是 socketpair 两个 endpoint；两端共享同一个 connection token。Host 的作用不是保存一个 client 永远拿不到的私密 token，而是：

- 不让调用者向 `pilferPointers()` 传任意 token；
- 总是从 Host 自己保存的 server `InputChannel` 读取这对 channel 的固定 token；
- 把一次控制能力绑定到创建时那一个 monitor。

client `InputChannel.getToken()` 看到的是同一 pair identity。安全性来自接口不接受可伪造的 token 参数和 Binder capability，不来自“两端 token 不同”。

共享 token 也不表示两个 endpoint 能互换用于注销。Dispatcher 开始 unregister 时确实按 token 找 Connection，但随后按传入 `InputChannel` 对象从 monitor vector 删除，并按该对象的 fd 删除 Looper watch。把返回给 consumer 的 client channel 当成已注册 server channel 传回，会让这些结构清错对象与 fd；Host 必须保存创建时真正注册的 server wrapper。

## 4. Native 注册复用 Connection，却有几处 monitor 特例

### JNI 的 monitor 路径不安装 dispose callback

`nativeRegisterInputMonitor()`：

1. 从 Java wrapper 取得 native `sp<InputChannel>`；
2. wrapper 未初始化时抛 `IllegalStateException`；
3. 单独拒绝 `ADISPLAY_ID_NONE`；
4. 调 `NativeInputManager::registerInputMonitor()`；
5. 非零 status 转成 `RuntimeException`。

普通 `nativeRegisterInputChannel()` 成功后还会安装 `handleInputChannelDisposed` callback；monitor 注册没有这一步。因此 global 创建路径紧接着 `inputChannels[0].dispose()` 时，不会因 callback 自动 unregister。

它也不会关掉 Dispatcher 需要的 endpoint：`Connection`、`mInputChannelsByToken` 和 monitor vector 都持有 server `sp<InputChannel>`。gesture 路径还多一个 Java `InputMonitorHost.mInputChannel` 持有。

### Dispatcher 建立三张索引和一个 Looper 请求

`registerInputMonitor()` 在锁内做：

```text
new Connection(inputChannel, monitor=true)
server fd    -> mConnectionsByFd
pair token   -> mInputChannelsByToken
displayId    -> global 或 gesture monitor vector
server fd    -> Looper.addFd(handleReceiveCallback)
```

锁外 `wake()`，然后返回 `OK`。monitor 与普通窗口因此共享：

- `InputPublisher`；
- 每 Connection 一份 `InputState`；
- outbound queue 与 wait queue；
- client 的 FINISHED 回执；
- Connection 级超时与响应性。

`Monitor` 结构本身只有一个非空 `sp<InputChannel>`。display 是外层 map 的 key，类型由它在哪张 map 决定。只有被粘进手势时才包装成 `TouchedMonitor`，再加 `xOffset/yOffset`。

### 正常 API 依赖 token 唯一，但 monitor 注册没守这个门

普通 `registerInputChannel()` 先按 token 查已有 Connection，重复就返回 `BAD_VALUE`。r48 的 `registerInputMonitor()` 没有这项检查，而是直接写 fd 表与 token 表、再向 vector 追加。

正常 Java API 每次创建新 pair，因此通常得到新 token。若定制代码用 dup 或同 token 重复注册，则可能出现：

- 同一个 channel/fd 再注册：fd map 与 Looper request 被替换，monitor vector 却继续追加；
- dup 后的不同 fd 共享 token 再注册：fd map 和 Looper 可留下多项，token map 被新 channel 覆盖，vector 也出现重复；
- `getConnectionLocked(token)` 扫描 fd map，重复 token 下命中哪一个 Connection 不再有可靠语义；
- unregister 按 token 找 Connection，却又按传入 channel 对象与 fd 清 monitor vector 和 Looper。

这不是“重复注册会更新旧 monitor”的受支持语义，而是破坏多张索引一致性的非法状态。

### `addFd()` 的返回值也被忽略

代码调用 `mLooper->addFd(...)`，却不检查返回值，仍 wake 并返回 `OK`。因此 native 注册返回成功严格证明的是内部表已经改写且 addFd 被调用，不足以单独证明 Looper watch 已成功安装。

在正常唯一 fd/token 不变量下，这通常没有问题；在重复 fd、Looper 异常或定制路径中，不能把 `OK` 扩写成“反向 FINISHED/HUP 监听必然可用”。

若 addFd 真正失败，gesture monitor 尚有 Host 可显式 unregister；global monitor 却没有正确的 server-side 控制句柄，而 peer close 也不会经一个不存在的 Looper watch 触发 HUP callback，内部注册就可能长期残留。这是忽略返回值带来的直接清理风险。

### 注册完成仍只是第一道门

要让一个事件最终出现在 client，还需依次满足：

```text
存在于对应 monitor registry
  -> 本次路由把它加入 InputTarget
  -> token 仍能找到 Connection
  -> Connection.status == NORMAL
  -> Connection.InputState 接受 action 序列
  -> publishInputEvent 成功
  -> client endpoint 实际读取
```

后文每次说“收到”，若只由较早一层源码能证明，会明确写成“被选中”或“尝试投递”。

## 5. Global monitor 只在成功解析的 key/motion 后追加

### Key 的加入点只有一个

`dispatchKeyLocked()` 先运行 policy 与 focused-window 目标查找。结果为 pending 就继续等；结果失败就设置 injection result 并返回。只有成功时才执行：

```cpp
addGlobalMonitoringTargetsLocked(inputTargets, getTargetDisplayId(*entry));
dispatchEventLocked(currentTime, entry, inputTargets);
```

因此：

- 没有 focused target 而仍 pending 时，global monitor 不会提前看到 key；
- 最终目标失败时，global monitor 也不会把 key 变成成功；
- 它不是所有 `EventEntry` 的广播通道。

`FocusEntry` 由另一条定向连接路径发送；configuration change 与 device reset 也没有调用 global-target builder。r48 中只有成功的 key 与 motion 主分发调用 `addGlobalMonitoringTargetsLocked()`。

### Motion 也先完成目标和权限判断

pointer motion 先走 `findTouchedWindowTargetsLocked()`；non-pointer motion 走 focused-window 目标查找。然后：

1. pending：等待；
2. permission denied：直接丢弃；
3. 其他失败：给已登记 monitors 合成相应类别的取消；
4. 成功：先追加事件或焦点 display 的 global monitors；
5. pointer 再按已提交 `TouchState.portalWindows` 追加 portal 目标 display 的 global monitors；
6. 最后 dispatch。

这解释了一个常见反例：

```text
无窗口 + 只有 global monitor
  -> pointer DOWN 的目标查找失败
  -> global 还没到追加时机
  -> 不能靠 global 维持这条流

无窗口 + 有合格 gesture monitor + 有 global monitor
  -> gesture 让目标查找成功
  -> 随后 global 也被追加
  -> 两者都可能看到 DOWN
```

global 在这里是成功结果的消费者，不是成功条件。

### 失败时的 cancel 不是一条统一规则

| 情况 | r48 行为 |
|---|---|
| key 目标失败 | 返回，不走 monitor-only cancel |
| motion 目标为 `PERMISSION_DENIED` | 直接返回，不 cancel monitors |
| motion 目标为其他失败 | 遍历两类 monitor 的所有 display 注册表，按 pointer/non-pointer 模式合成取消 |
| 顶层 `dropInboundEventLocked()` | 按 drop reason 对所有 Connections（窗口与 monitor）合成相应取消 |
| 成功 motion 但 pointer action 冲突 | 在当前事件 dispatch 前，对所有 Connections 合成 pointer cancel |

`synthesizeCancelationEventsForMonitorsLocked()` 不按当前 device/display 先过滤 registry；真正能生成什么取决于每个 Connection 的 `InputState` memento。不要把它写成“只取消当前 display 上的 monitor”。

### injection result 早于 monitor 的实际 publish

key 和 motion 都会在追加或投递 monitor 之前设置 injection result。`WAIT_FOR_FINISHED` 又只用 foreground target 计数。于是：

- 某个 monitor 后续找不到 Connection，不会反写 injection result；
- monitor `InputState` 拒绝不一致序列，也不会把已成功的结果改成失败；
- monitor-only 的 gesture 路径可以在 monitor 发 FINISHED 前就让注入调用返回。

### 中途注册 global 的第一个 MOVE 可能根本发不出去

global 每个成功事件都重新追加，所以表面看它可以从手势中间开始。但 `enqueueDispatchEntryLocked()` 会先用该 Connection 的 `InputState.trackMotion()` 校验序列。

若新 Connection 没见过 `DOWN`，首个 `MOVE/POINTER_UP/UP` 通常是不一致事件，会在压入 outbound queue 前被跳过。首个 `HOVER_MOVE` 则有特殊修补，可被改写为 `HOVER_ENTER`。

所以“global 逐事件入选”与“client 能从任意 action 开始观察”仍是两件事。

## 6. Gesture monitor 只在 ACTION_DOWN 快照一次，再粘进 TouchState

### `newGesture` 不等于“新 gesture monitor 集合”

目标函数把以下事件都称为 `newGesture`：

- `ACTION_DOWN`；
- `ACTION_SCROLL`；
- hover enter/move/exit。

它们都会重置临时 `TouchState` 并做一次 hit-test。但收集 gesture monitors 的条件更窄：

```cpp
bool isDown = maskedAction == AMOTION_EVENT_ACTION_DOWN;
std::vector<TouchedMonitor> newGestureMonitors = isDown
        ? findTouchedGestureMonitorsLocked(displayId, tempTouchState.portalWindows)
        : std::vector<TouchedMonitor>{};
```

因此 gesture monitor 不接 key、non-pointer motion、hover 或 scroll；它只在 pointer `ACTION_DOWN` 被选入。

### DOWN 的选择顺序

普通 DOWN 可以压缩成：

```text
复制旧 TouchState 到 temp
  -> newGesture：reset，记录 down/device/source/display
  -> 按坐标命中普通窗口，并收集经过的 portal
  -> 收集源 display + portal 目标 display 的 gesture monitors
  -> 过滤缺 Connection 或 responsive=false 的新 monitors
  -> 要求“有窗口”或“newGestureMonitors 非空”
  -> 把窗口和 monitors 写入 temp
  -> 完成注入权限判断
  -> 生成本次 InputTargets
  -> 提交 temp 到 mTouchStatesByDisplay[source display]
```

`selectResponsiveMonitorsLocked()` 只检查：

- token 能找到 Connection；
- `connection->responsive == true`。

它没有检查 `Connection.status == NORMAL`。因此一个仍标 responsive、但 status 已 BROKEN 的异常 Connection 可能通过选择门并让无窗口 DOWN 的早期条件成立，真正到 `prepareDispatchCycleLocked()` 又因非 NORMAL 被跳过。

### “粘住”表示后续不重新枚举 registry

`tempTouchState.addGestureMonitors(newGestureMonitors)` 把快照保存下来。后续 `MOVE`、`POINTER_UP`、`UP` 复制旧 state，再直接遍历其中的 `gestureMonitors` 建目标：

```cpp
for (const TouchedMonitor& touchedMonitor : tempTouchState.gestureMonitors) {
    addMonitoringTargetLocked(touchedMonitor.monitor, touchedMonitor.xOffset,
                              touchedMonitor.yOffset, inputTargets);
}
```

后续不会：

- 把手势中途新注册的 monitor 补进来；
- 因已有 monitor 后来变得 `responsive=false` 就从 state 删除；
- 每次重新计算其 portal offset。

最终 `UP/CANCEL` 这一事件仍先由旧 temp state 生成 gesture targets，随后才 reset 已保存的 state；下一条手势重新快照。

### POINTER_DOWN 不是第二次 gesture monitor 选择

split touch 的 `ACTION_POINTER_DOWN` 会进入新 pointer 的 hit-test 分支，但局部 `isDown` 只对 `ACTION_DOWN` 为真，因此 `newGestureMonitors` 是空 vector。它可以给普通 split window 分配新 pointer，却不会吸收一个刚注册的 gesture monitor。

### 无普通窗口时，gesture monitor 可以成为成功条件

新 DOWN 在坐标处没有合格窗口时，只要 `newGestureMonitors` 非空，就不触发“no touchable window or gesture monitor”失败。之后 `haveForegroundWindow=false` 但 `hasGestureMonitor=true`，目标查找仍可成功。

这使 gesture consumer 在没有 touchable App window 的位置仍能建立并观察一条流；具体是不是屏幕边缘手势，由 consumer 收到事件后自行识别，Dispatcher 并不按 edge/region 筛选 gesture monitors。它也意味着 gesture monitor 不只是旁听副本：其存在会改变“这条 pointer 流是否成立”的路由结果。

## 7. 无窗口 gesture-only 路径暴露了 r48 的注入权限缺口

### 注入并不是在 IMS 入口统一拒绝

`InputManagerService.injectInputEventInternal()` 记录调用者 pid/uid、清身份并进入 native。Dispatcher 创建 `InjectionState`；调用者拥有 `INJECT_EVENTS` 时可带 trusted policy flag，但没有权限并不会在入口处一律立即返回。

目标选择再根据要投向的窗口决定：

```text
目标窗口 ownerUid == injectorUid
  -> 允许投向自己的窗口

ownerUid 不同
  -> 需要 INJECT_EVENTS

没有窗口
  -> 正常失败收尾应调用 checkInjectionPermission(nullptr, ...)
```

### gesture-only 成功分支把 UNKNOWN 直接改成 GRANTED

r48 的 `findTouchedWindowTargetsLocked()` 先遍历 `tempTouchState.windows`：

```cpp
bool haveForegroundWindow = false;
for (const TouchedWindow& touchedWindow : tempTouchState.windows) {
    if (touchedWindow.targetFlags & FLAG_FOREGROUND) {
        haveForegroundWindow = true;
        if (!checkInjectionPermission(touchedWindow.windowHandle, entry.injectionState)) {
            // permission denied
        }
    }
}
bool hasGestureMonitor = !tempTouchState.gestureMonitors.empty();
if (!haveForegroundWindow && !hasGestureMonitor) {
    // failed
}
injectionPermission = INJECTION_PERMISSION_GRANTED;
```

当“没有 foreground window，但有 gesture monitor”时，循环没有检查任何 window，失败门又被 monitor 绕过，随后却无条件把 permission 标为 granted。

函数 `Failed:` 标签下确实有：

```cpp
if (injectionPermission == INJECTION_PERMISSION_UNKNOWN) {
    checkInjectionPermission(nullptr, entry.injectionState);
}
```

但 gesture-only 成功路径已经把状态从 UNKNOWN 改成 GRANTED，因而不会执行这个 null-window 全局权限检查。

### 精确结论与限制

> 在 Android 11 r48 的这段实现中，一个注入的 pointer DOWN 若落在无 foreground window 的区域，同时存在被选中的 responsive gesture monitor，就可能通过本应检查 `INJECT_EVENTS` 的目标权限门。

这个结论要同时带上边界：

- 它描述 r48 的具体实现，不是 API 契约；
- 只讨论 pointer 目标选择的这一条分支，不代表其他注入前置条件都消失；
- 创建 gesture monitor 本身通常受 signature 级 `MONITOR_INPUT` 保护，但 monitor capability 与注入调用者能力仍应分别审视；
- global monitor 不参与这个条件，也不能单独造成该结果；
- 有 foreground window 时仍逐窗口执行注入权限检查。

还要先区分注入同步模式。`INPUT_EVENT_INJECTION_SYNC_NONE` 在事件入 inbound queue 后就把调用结果设为 `SUCCEEDED`，不等待后续 target/permission result；因此 ASYNC 返回 true 对这个权限分支没有证明力。`WAIT_FOR_RESULT` 或 `WAIT_FOR_FINISHED` 的 success 虽排除了这种 ASYNC 盲成功，也仍非充分证据：policy 若先消费事件，`dropReason=POLICY` 会在完全不做 target lookup 时把 injection result 映射成 success。必须再确认事件不是 policy-consumed、目标路径确实执行；最强证据是 monitor 实际收到该事件。其中 `WAIT_FOR_FINISHED` 仍不会等待非 foreground monitor 的 FINISHED。

排障时若看到“没有 App 窗口的同步注入居然成功”，不能引用 `Failed:` 下那次 null 检查就断言安全门必然执行；必须沿实际控制流确认 `injectionPermission` 何时离开 UNKNOWN。

## 8. Monitor 目标的 flags、pointer 集合与 portal 坐标

### Monitor 目标只有 `DISPATCH_AS_IS`

`addMonitoringTargetLocked()` 每次创建新 `InputTarget`：

```cpp
target.inputChannel = monitor.inputChannel;
target.flags = InputTarget::FLAG_DISPATCH_AS_IS;
target.setDefaultPointerInfo(xOffset, yOffset, 1, 1);
inputTargets.push_back(target);
```

它不设置：

- `FLAG_FOREGROUND`；
- `FLAG_SPLIT`；
- `FLAG_WINDOW_IS_OBSCURED`；
- `FLAG_WINDOW_IS_PARTIALLY_OBSCURED`；
- `FLAG_DISPATCH_AS_OUTSIDE`。

默认 pointer info 的空 pointer-id 集合表示全部 pointers。monitor 因而拿的是完整事件目标，而不是某个 App window 在 split touch 下的 pointer 子集。

“`AS_IS`”也不是绝对保证 action 字段原样到 client：Connection 自己没有 hover memento 时，`HOVER_MOVE` 会被修成 `HOVER_ENTER`；不一致的 touch action 则可能直接被 `trackMotion()` 拒绝。

### 非 FOREGROUND 不表示免除传输账

monitor target 不带 foreground，至少产生两项差异：

- 不增加 EventEntry 的 `pendingForegroundDispatches`，`WAIT_FOR_FINISHED` 不等待这个副本；
- key 回执为 unhandled 时，不走 foreground window 的正常 fallback 生成分支；若 Connection 已有关联 fallback，代码反而进入取消分支。

这仍不表示每个“创建过的” `DispatchEntry` 都必然进入 wait queue：它要先通过 `InputState.trackKey/trackMotion` 才会压入 outbound，再要 publish 成功才移进 wait queue。从这一步开始，它才需要 FINISHED，并可进入 ANR tracker。

所以：

```text
没有 FLAG_FOREGROUND
  = WAIT_FOR_FINISHED 不把该副本计入 foreground 完成
  = 不为这个副本启动正常 unhandled-key fallback
  ≠ 不需要 FINISHED
  ≠ 不进入 waitQueue
  ≠ 不会触发 Connection ANR
```

### Portal 同时改变候选 display 与局部坐标

`findTouchedGestureMonitorsLocked(sourceDisplay, portalWindows)`：

1. 先加入 source display 的 gesture monitors，offset 为 0；
2. 对每个 portal window，加入其 `portalToDisplayId` 上的 gesture monitors；
3. offset 使用该 portal 的 `-frameLeft/-frameTop`。

global pointer 路径也会用同样负 frame offset 追加 portal 目标 display 的 monitors。

但 `addMonitoringTargetLocked()` 没有改 `MotionEntry.displayId`。publish 时仍发送原 entry 的 displayId。因此 portal 目标屏 monitor 可能同时看到：

- `MotionEvent.displayId` 仍是源 display；
- `getX()/getY()` 已按 portal frame 平移；
- raw coordinates 不叠加这次 portal target offset，保持当前 `MotionEntry` 的坐标基准。

不要用 event 的 displayId 猜“这个 monitor 注册在哪张屏”，也不要把 monitor 的 `getX()/getY()` 直接当作某个 View local 坐标。

### offset 何时冻结

gesture monitor 的 `TouchedMonitor` 在 DOWN 时保存 offset；后续复制 `TouchState`，所以整条已选流沿用该值。global portal monitor 则在每个实际通过已提交 `portalWindows` 追加 portal target 的事件上，从当时保存的 portal handle 读取 frame。

r48 对每个 portal 使用它自己的负 frame offset，并不在 `findTouchedGestureMonitorsLocked()` 中累计多级 portal 的全部位移。复杂嵌套 display 不能先验假设已经得到完整祖先坐标变换。

### portal 目标还可能重复

`TouchState.addPortalWindow()` 只按同一个 window-handle 指针去重。不同 portal 指向同一 destination display 时，会各自再次枚举那张 display 的 monitors；`addMonitoringTargetLocked()` 本身也不按 token 去重。

正常窗口拓扑通常避免病态重复，但定制嵌套场景应检查同 token 是否被加入多个 `InputTarget`，以及不同 portal offset 是否让同一 Connection 获得重复目标。

### Portal global 并不覆盖每个终止 action

`dispatchMotionLocked()` 是在 `findTouchedWindowTargetsLocked()` 返回后，读取已经提交的 `mTouchStatesByDisplay[entry.displayId].portalWindows` 来追加 portal globals。这里有几个 r48 时序边界：

- 普通 DOWN、MOVE 与中间 POINTER_UP 通常仍保存 portal 列表；
- 最终 UP/CANCEL 在目标函数返回前已经 reset/erase 已保存 state，因此 portal globals 通常不会因旧 portal 列表收到这一个终止事件；
- hover 会在保存前 reset 临时 state，因而不保留 portal；
- scroll 不保存本次临时 state，追加点可能看不到本次 portal，甚至读到旧 touch state；
- pilfer 会清 `portalWindows`，此后同一流也不再靠该列表追加 portal globals。

source 或 focused display 的 global monitor 仍在成功后先被无条件追加；这里收窄的是“经 portal 额外追加的另一张 display”，不是所有 global monitor。

### 合成 CANCEL 的坐标也有边界

Connection 的 `InputState` 保存 raw pointer memento。给 monitor 合成取消时没有 window handle，构造 monitor target 的 offset 回到 0。对 portal monitor 来说，合成 `CANCEL` 的 `getX()/getY()` 基准因此可能与此前带 portal offset 的原始事件不同。

识别器应把 CANCEL 首先当作状态终止信号，不应假设它在复杂 portal 路径中与最后一个 MOVE 保持完全相同的局部坐标变换。

## 9. 被选中不等于送达：FINISHED、背压与 ANR 仍然存在

### 目标到 client 之间还有四道门

`dispatchEventLocked()` 会再次按 target token 查 Connection。找不到就只丢这个 target。找到后：

1. `prepareDispatchCycleLocked()` 拒绝非 NORMAL Connection；
2. `enqueueDispatchEntryLocked()` 让 Connection 的 `InputState` 检查 key/motion 序列；
3. outbound 首项由 `InputPublisher` 写 socket；
4. publish 成功才移入 wait queue，等待 client FINISHED。

因此以下状态都可能让“registry 里有、路由也选中”的 monitor 实际没有事件：

| 状态 | 停在哪一步 |
|---|---|
| unregister 后旧 `TouchState` 仍留 monitor；旧 Connection 已 ZOMBIE | token 在 fd map 中找不到 Connection |
| Connection 已 BROKEN 但尚未注销 | prepare 阶段跳过 |
| 中途开始的 MOVE/UP 不符合 memento | `trackMotion()` 返回 false |
| socket 满且 wait queue 非空 | 暂停 publish，等待 client 追上 |
| fatal publish error | abort dispatch cycle |

### monitor 必须及时 FINISHED

monitor Connection 与窗口 Connection 走同一协议。publish 成功后 `DispatchEntry` 进入 wait queue，并在 `connection->responsive` 为 true 时加入 ANR tracker。

WMS 的 `PointerEventDispatcher` 展示了正确形态：

```java
try {
    // 只把 pointer MotionEvent 交给 listeners
} finally {
    finishInputEvent(event, false);
}
```

即便 consumer 忽略事件类型，也必须 finish。否则 socket 反向账不推进，wait queue 与超时判断都不会因为“它只是监控副本”而豁免。

### 没有 window handle 时使用默认 5 秒

`getDispatchingTimeoutLocked(token)` 找不到对应 `InputWindowHandle` 时返回 `DEFAULT_INPUT_DISPATCHING_TIMEOUT`，r48 常量为 5 秒。monitor token 通常没有普通窗口 handle，所以走这个默认值。

超时后：

```text
ANR tracker 命中 token
  -> connection.responsive = false
  -> 调 policy notifyANR
  -> WMS 通常找不到对应 WindowState / ActivityRecord
  -> 返回 0，不给扩展时间
  -> 对该 Connection 合成 CANCEL_ALL
```

`cancelEventsForAnrLocked()` 明确不在这里断开 Connection；status 为 NORMAL 时只合成取消。monitor 没有 foreground flag，也不妨碍 Connection 自己 ANR。

### responsive 的三种不对称

| 路由类型 | `responsive=false` 后怎样 |
|---|---|
| 新 gesture DOWN 候选 | 在 `selectResponsiveMonitorsLocked()` 被过滤 |
| 已经在旧 `TouchState` 的 gesture monitor | 后续事件不重筛，仍创建 target |
| global monitor | 每次追加时根本不筛 responsive |

而 `prepareDispatchCycleLocked()` 只看 status，不看 responsive。于是 status 仍 NORMAL 的不响应 monitor 可继续积压 outbound/wait；publish 成功时因为 responsive 已 false，不再给新条目加入 ANR tracker。

反方向也有异常组合：BROKEN 但 responsive 仍 true 的 gesture Connection 可通过新手势筛选，随后在 prepare 阶段被跳过。

### 一个 monitor 还可能拖慢后续 focused key

`shouldWaitToSendKeyLocked()` 会观察全局 ANR tracker，给早先事件最多留一段 key 等待时间。由于 monitor 的成功 publish 也能进入 ANR tracker，一个卡住的 monitor 不只是“自己丢副本”，还可能让后续 focused key 的发送短暂等待。

这不等于 monitor 成为 foreground 注入目标；它说明 Connection 超时账与 injection foreground 完成账是两套机制。

### 迟到 FINISHED 或 timeout extension 可以恢复响应

收到 FINISHED 后，Dispatcher 从 wait queue 完成相应序号，并重新计算 Connection 是否还有过期条目；健康时才能把 responsive 恢复为 true。policy 若返回正 timeout extension，也可重建 deadline。

正 timeout extension 会直接把 responsive 设回 true，无需先收 FINISHED；但仅仅把 Java 监听逻辑恢复正常，不会自动修改 native 标志或清掉旧队列，仍要靠 FINISHED 或 policy extension 推进状态。

## 10. pilfer 是 oneway 控制请求，不是同步抢流结果

### 控制调用跨过四层

```text
gesture consumer
  -> InputMonitor.pilferPointers()
  -> IInputMonitorHost.pilferPointers() Binder
  -> InputManagerService.InputMonitorHost
  -> nativePilferPointers(pair token)
  -> InputDispatcher::pilferPointers(token)
```

`IInputMonitorHost.aidl` 声明的是整个 `oneway interface`，`pilferPointers()` 与 `dispose()` 都没有返回值。

### 跨进程返回只证明事务已提交

常见跨进程调用中，Java `InputMonitor.pilferPointers()` 返回时，能确认的是 Binder oneway 请求已成功提交；不能确认：

- system_server 已开始执行 Host 方法；
- Dispatcher 已取得锁并完成三道校验；
- `TouchState.windows` 已清；
- 合成 `CANCEL` 已 publish；
- App 已读取 CANCEL；
- App 已回 FINISHED。

服务端抛出的运行时异常也不会作为同步结果回给 oneway 调用者。同进程 local-interface 调用可能直接执行，不能把跨进程完成语义无条件套到这一分支。

### JNI 又主动丢掉 native status

`InputDispatcher::pilferPointers()` 返回 `OK` 或 `BAD_VALUE`，但 JNI 只是：

```cpp
sp<IBinder> token = ibinderForJavaObject(env, tokenObj);
im->pilferPointers(token);
```

status 没有转成 Java 返回值，也没有抛异常。调用者无法从 `void pilferPointers()` 区分：

- token 不是 gesture monitor；
- 对应 display 没有当前 `TouchState`；
- monitor 没参与这条流；
- state 已经不再 down；
- native 已成功清窗口目标。

失败主要留在 Dispatcher 日志。成功也只证明锁内完成了状态修改和取消入队尝试，不证明接收端完成。

### 更准确的完成阶梯

```text
T0  调用者把 oneway Binder 请求提交
T1  system_server Host 开始处理
T2  Dispatcher 在锁内校验、尝试合成取消、filterNonMonitors
T3  CANCEL 从 outbound queue publish 到窗口 socket
T4  App InputEventReceiver 取出 CANCEL
T5  App 对 CANCEL 调 finishInputEvent
```

常见跨进程调用只向调用者暴露 T0；同进程 local-interface 可能同步执行到 T2，但同样拿不到 native status，也没有内建 ack 把 T3—T5 返回。

## 11. pilfer 的三道校验，以及 portal 跨 display 的结构性失配

### 第一道：token 必须仍在 gesture registry

`findGestureMonitorDisplayByTokenLocked(token)` 只遍历 `mGestureMonitorsByDisplay`。因此这些 token 都失败：

- global monitor；
- 普通窗口；
- null；
- 已经 unregister 的 gesture monitor。

找到 token 时同时返回“注册 display”。若非法状态把同一 token 注册在多处，unordered map 的首次匹配还会让 display 选择不稳定；正常唯一注册不应触发此情况。

### 第二道：注册 display 必须有当前 TouchState

Dispatcher 用上一步得到的 registration display 查询：

```cpp
mTouchStatesByDisplay.find(displayId)
```

不存在就返回 `BAD_VALUE`。注册着 gesture monitor 不表示任意时刻都可 pilfer；对应 display 必须有一份当前 pointer state。

### 第三道：token 在 state 中，而且 `down == true`

代码扫描 `state.gestureMonitors`，找到相同 token 才取 `state.deviceId`，随后要求 `state.down`。这挡住：

- 手势中途注册、从未收到当前 DOWN 的 monitor；
- 只注册但这次 DOWN 被 responsive 筛掉的 monitor；
- 最终 UP/CANCEL 后已 reset 的 state。

pilfer 只针对这份 display state 当前记录的 device/source 手势，不是一次抢走设备上所有 pointer streams。r48 每个 display 只有一份 `TouchState`，其中只记录一个当前 device/source；切换设备的冲突路径还可能触发面向所有 Connections 的 pointer cancel。

### 校验没有检查调用者 Connection 健康

三道校验没有重新查询 pilferer 的 Connection，也不检查：

- `status == NORMAL`；
- `responsive == true`；
- client endpoint 仍在读。

只要 token 仍登记在 gesture map、`TouchedMonitor` 仍在 state 且 down，调用就可能成功清窗口，即使发起者自己已经无法继续接收后续事件。

### Portal monitor 能收流，却通常不能 pilfer 这条流

这是 r48 最容易漏掉的 display 不对称：

```text
源 display S 上 DOWN
  -> 命中 portal，目标 display 为 D
  -> 收集注册在 D 的 gesture monitor M
  -> 把 M 保存进 mTouchStatesByDisplay[S]

M 调 pilfer
  -> registry 按 token 找到 registration display D
  -> 查询 mTouchStatesByDisplay[D]
  -> 看不到保存在 S 的那一份 membership
  -> 通常 BAD_VALUE
```

也就是说，“M 已收到一条经 portal 路由的手势”不足以证明“M 能 pilfer 这条手势”。更危险的异常是 D 恰好另有当前流且 membership 也出现同 token，此时校验可能命中与调用者所识别流不同的 D state。

根因不是坐标 offset，而是选择时以源 display 保存 state，pilfer 时却以 monitor 注册 display 查 state。

## 12. pilfer 真正取消什么，filterNonMonitors 又保留什么

### 先尝试给每个 TouchedWindow 合成 pointer CANCEL

三道校验通过后，Dispatcher 创建：

```cpp
CancelationOptions(CANCEL_POINTER_EVENTS,
        "gesture monitor stole pointer stream");
options.deviceId = state.deviceId;
options.displayId = displayId;
```

随后遍历 `state.windows`，按每个 `TouchedWindow.windowHandle->token` 找 `InputChannel`，找到才调用 `synthesizeCancelationEventsForInputChannelLocked()`。

这可能覆盖当前 state 中的：

- foreground App window；
- wallpaper；
- 仍以 AS_IS 形式保留的其他 touched windows。

具体能合成哪些 pointer 由各 Connection 的 `InputState` memento 决定。`CANCEL_POINTER_EVENTS` 按 deviceId、displayId 与 pointer-class source 过滤，并不按某个精确 source 或 pointer id 列表过滤；hovering memento 还可能生成 `HOVER_EXIT`。

### “尝试取消”不等于每个窗口都收到

某个 window token 已经没有 channel 时，循环直接跳过；Connection 非 NORMAL、socket 错误或 client 不读，也都可能让 CANCEL 到不了应用。无论这些传输结果如何，代码随后都会执行 `state.filterNonMonitors()`。

因此即使 App 没实际收到 CANCEL，Dispatcher 的普通窗口路由仍可能已被清掉。pilfer 的 native `OK` 也没有逐窗口 delivery ack。

### `filterNonMonitors()` 只有两行

```cpp
void TouchState::filterNonMonitors() {
    windows.clear();
    portalWindows.clear();
}
```

字段变化表比“只保留 monitor”更准确：

| `TouchState` 字段 | pilfer 后 |
|---|---|
| `windows` | 清空 |
| `portalWindows` | 清空 |
| `gestureMonitors` | 全部保留，不只调用者 |
| `down` | 保留 true |
| `split` | 保留原值 |
| `deviceId` | 保留 |
| `source` | 保留 |
| `displayId` | 保留 |

### 它不是 touch focus transfer

`transferTouchFocus(A, B)` 需要结束 A、把 pointer memento 合并给 B，并可能为 B 补 `DOWN/POINTER_DOWN`。pilfer 完全不同：

- gesture monitors 从原始 DOWN 就已经参与；
- 不创建“新 owner”字段；
- 不把其他 monitors 删除；
- 不给发起者补 DOWN；
- 只清当时的普通 window/portal route。

从数据结构上看，pilfer 更接近“剪掉 `TouchState` 的窗口分支”，而不是“把手势所有权转移给一个 monitor”。

## 13. pilfer 之后仍有四个反直觉后续状态

### 其他 gesture monitors 继续存在

`gestureMonitors` 整个 vector 保留。以后成功的 MOVE/UP 会为所有 retained monitors 建 target，系统没有记录“谁 pilfer 过”。

第二个 monitor 也可在同一 state 上调用 pilfer。只要三道校验仍成立，重复调用通常返回 native `OK`；当 `windows` 已空时，它只是再次过滤一个空窗口集合。

### split POINTER_DOWN 可把普通窗口重新加回来

pilfer 保留 `split`。如果 pilfer 前某个 window 让这条流进入 split 模式，那么后续 `ACTION_POINTER_DOWN` 满足：

```cpp
isSplit && maskedAction == ACTION_POINTER_DOWN
```

目标函数会对新 pointer 再 hit-test。找到支持 split 的普通窗口时，它会用新的 pointer id 加回 `tempTouchState.windows`。该 Connection 的 split memento 可能把动作转换成自己的 `DOWN` 或 `POINTER_DOWN`。

所以“第一次 pilfer 后，到物理 UP 为止永远只有 monitors”在 r48 并不成立。若要再次清掉后来加入的窗口，只能再 pilfer；API 没有为调用者自动保持排他屏障。

还有一个尖锐边界：若 split POINTER_DOWN 找不到窗口，局部 `newGestureMonitors` 是空的；早期“window 或 newGestureMonitors”检查不会拿 retained monitors 来兜底，事件可能因此失败，即使旧 state 里仍有 gesture monitors。

### 清 portal 会改变后续 global monitor 集合

pilfer 清掉 `portalWindows`。因此后续 pointer 事件即使仍由 retained gesture monitors 延续，也不会再通过旧 portal 列表追加 destination display 的 global monitors。

源 display global monitor 仍可在每次目标查找成功后追加。若 split 新 pointer 又建立新的 portal 路径，临时 state 也可能重新积累新的 portal windows。

### stale TouchedMonitor 可能让“成功”没有真实接收者

unregister 会从 gesture registry 与 Connection 表删除 monitor，却不遍历所有 `TouchState` 删除已有 `TouchedMonitor`。直到 UP/CANCEL/reset，旧 state 仍可能保留其 `sp<InputChannel>` 与 token。

后续目标判断使用 `tempTouchState.gestureMonitors.empty()`，所以 stale 元素仍可让 `hasGestureMonitor=true`；生成 target 后，`dispatchEventLocked()` 又因找不到 Connection 而丢弃这个 delivery。

结果可能是：

```text
目标解析：成功
实际 gesture receiver：没有
source display global：因整体成功而仍可能收到副本
```

这不是正常生命周期的理想状态，而是“注册线已经结束、路由线尚未 reset”的窗口期。pilfer 则相反：token 已不在 registry，第一道校验立即失败，即使 stale membership 仍在。

## 14. dispose、HUP、unregister 与最后一个 fd close 是不同完成点

### Global monitor 主要依靠 client 断链回收

global `monitorInput()` 没有返回 Host，因此调用者没有显式 native unregister 控制面。以 WMS 为例，display 移除时 `PointerEventDispatcher.dispose()` 释放其 `InputEventReceiver` 与 client channel。

只有 client endpoint 的最后一个 fd/native 引用真正释放后，server endpoint 才观察到 HUP/DEAD_OBJECT。client 侧可能同时存在：

- Java `InputChannel` wrapper；
- native `InputEventReceiver/InputConsumer` 的 `sp<InputChannel>`；
- Parcel 或 dup 制造的其他 fd。

关闭一个 wrapper 不能直接证明 peer HUP 已产生。HUP 到达 Dispatcher receive callback 后，才会自动 unregister server channel。

### Gesture dispose 先动 client，再发 oneway Host.dispose

`InputMonitor.dispose()` 的顺序是：

```java
mInputChannel.dispose();
mHost.dispose();
```

Host 在 system_server：

```text
nativeUnregisterInputChannel(server channel)
  -> server Java wrapper dispose
```

跨进程时第二步是 oneway，所以调用者返回不证明 server 已注销。client endpoint 最后一个引用若先释放，HUP 可能先触发自动 unregister；Host 随后显式 unregister 会得到 `BAD_VALUE`，JNI 特意忽略“already unregistered”。

反过来，Host 显式注销也可能先赢；Looper 中已经排队的 fd callback 仍可能最后跑一次，Dispatcher 的 unknown-fd guard 将它当 spurious event 处理。

### 逻辑注销不等于物理 fd 当场关闭

`unregisterInputChannelLocked()` 做的是：

1. 按 token 找 Connection；
2. 从 fd map 与 ANR 相关状态移除 Connection；
3. 删除 token map；
4. monitor 类型再从 global/gesture vectors 移除；
5. 从 Looper 移除 fd；
6. abort/drain outbound 与 wait；
7. 把 Connection 置为 ZOMBIE。

这里没有承诺所有 `sp<InputChannel>` 同时销毁。旧 `TouchState.gestureMonitors` 仍可持有 server channel；正在执行的局部强引用也可能延长对象寿命。因此：

```text
unregister 完成
  = Dispatcher 不再把它当已注册 Connection
  ≠ 所有进程里的 fd 已经 close
```

### BAD_VALUE 容忍不等于 dispose 幂等

第一次 Host dispose 后，server Java wrapper 会变成 uninitialized。再次执行 Host dispose 时，JNI 可能在取得 native channel 阶段直接抛 `IllegalStateException`，还没走到“忽略 unregister BAD_VALUE”。

而 Host 接口又是 oneway，跨进程 caller 通常看不到这个服务端异常。因此调用方应把 `dispose()` 视为一次性终止操作，不依赖重复调用得到确定结果。

### receive callback 里的旧注释不能当完整生命周期

r48 receive path 有“Monitor channels are never explicitly unregistered”一类历史注释。对 legacy global monitor，它描述了主要依赖 peer HUP 的现实；对带 `InputMonitorHost.dispose()` 的 gesture monitor 已经过宽。

可靠判断应以真实调用链为准：

- global：没有 Host，通常随 consumer/client 末端关闭而 HUP 回收；
- gesture：既可能显式 Host unregister，也可能由 client 先断开触发 HUP；
- 两条路径最终都汇入 Dispatcher 的 unregister 清理，但完成时机不同。

通知语义也相同：monitor 的 DEAD_OBJECT 以及 HUP/ERROR 都令 `notify=false`，显式 `unregisterInputChannel()` 也传 `notify=false`。它们会 drain 并进入 ZOMBIE，但不会像仍有窗口 handle 的普通 Connection 那样走 WMS broken-channel 通知。

## 15. 用四维矩阵排障，避免被 dumpsys 的盲区误导

### `dumpsys input` 能看到什么

Dispatcher dump 会列出：

- `Global monitors by display`；
- `Gesture monitors by display`；
- Connections 的 fd、name、status、monitor、responsive 与队列；
- 每个 display 的 `TouchState`：down、split、deviceId、source、windows、portal windows。

但 r48 的 `TouchState::dump` 不打印 `gestureMonitors` membership。于是：

```text
Gesture monitor 表里有 M
  ≠ M 已加入当前 TouchState

当前 TouchState down=true
  ≠ dumpsys 能直接证明 M 在其中
```

这正是 pilfer 资格所需的关键关联，却不能从单次 dumpsys 直接读出。

### 最小诊断矩阵

| 维度 | 要看什么 | 常见误判 |
|---|---|---|
| Registered | 对应 display 的 monitor 表是否有名字/token | 有注册就以为能 pilfer 当前流 |
| Selected | DOWN 时是否通过筛选并写入源 display TouchState | 只看 registry，不看手势开始时机 |
| Deliverable | Connection 是否存在、NORMAL，InputState 是否连贯 | 看见 InputTarget 就说 client 已收到 |
| Healthy | responsive、outbound/wait、ANR tracker | 把非 foreground 误当成无需 FINISHED |
| Controlled | Host oneway 是否已在服务端执行 | 把 Java void 返回当作 native OK |
| Alive | client/server 还有哪些 fd 与 `sp` | 把 wrapper dispose 当作最后 fd close |

### 症状到取证点

| 症状 | 第一批取证 |
|---|---|
| 新 gesture monitor 收不到当前 MOVE | 它是否在 DOWN 前注册；当前流是否已开始 |
| global 表里有名字却没事件 | 目标解析是否成功；首个 action 是否被 `InputState` 拒绝；Connection status |
| pilfer 后 App 没见 CANCEL | native 三道校验、窗口 channel 是否仍在、outbound/wait 与 App receiver |
| pilfer 后还有别的 monitor 收事件 | 这是设计结果；`filterNonMonitors` 保留整个 vector |
| pilfer 后又有 App 收到新 pointer | 检查 `split` 与后续 `ACTION_POINTER_DOWN` |
| portal monitor 能收却 pilfer 失败 | 对比源 display TouchState 与 monitor registration display |
| monitor ANR 后仍继续积压 | global/retained gesture 不做 responsive 重筛；prepare 只看 status |
| dispose 返回后 dumpsys 仍短暂可见 | oneway、最后 client fd、HUP 与显式 unregister 的竞态 |

### 现有 native tests 证明到哪里

`InputDispatcher_test.cpp` 的 gesture-monitor 测试能直接证明：

- gesture monitor 收 pointer motion，但不收 key；
- 不 finish monitor 的 DOWN 会触发以 monitor token 上报的 ANR；
- App 因 key 或 motion 不响应时，新的 tap 仍可交给 responsive gesture monitor；
- 普通窗口中途 `releaseChannel()` 后，已在当前 state 的 monitor 仍可在 pilfer 调用后收到 UP。

最后一项不能独自证明“pilfer 给窗口成功送达 CANCEL”：测试在调用前已经释放 window channel，不检查 `pilferPointers()` 的 status，也不断言窗口 CANCEL 或 `TouchState.windows`。现有这组测试还没有覆盖 invalid token、portal registration-display 失配、多个 monitors、重复 pilfer、unregister 后 stale membership，以及 split POINTER_DOWN 重新加入窗口。

因此测试是证据的一部分，不是对实现每个边界的完整规格。对未覆盖分支仍要逐行走控制流，或另写带状态与事件断言的定向测试。

### 四个结论必须用不同证据

若要宣称下列结论，证据也应分别对应：

1. “创建成功”：Java/JNI 返回且 Dispatcher 表已写入；
2. “选入当前手势”：DOWN 路径与源 display `TouchState` membership；
3. “事件送达”：publish 成功、client receiver 日志或队列回执；
4. “pilfer 生效”：服务端状态/日志与窗口 CANCEL，而不是仅看 caller 方法返回。

只凭名字出现在 monitor 表里，最多证明这次 dump 快照中“registry membership 当前存在”；表项写入早于 addFd、native 返回与 Binder 返回值封送，不能倒推出 Java/JNI 已成功返回。

## 16. 九个只读练习、检查题与下一章

### 练习 1：对照两种 Java 创建路径与权限门

```bash
nl -ba frameworks/base/services/core/java/com/android/server/input/InputManagerService.java |
  sed -n '499,551p;1858,1877p;2334,2355p'

nl -ba frameworks/base/core/java/android/hardware/input/IInputManager.aidl |
  sed -n '88,100p'
```

先回答三个问题：哪个方法在 AIDL；permission check 在参数校验前还是后；同 PID 为什么能通过。

### 练习 2：证明 Host 是 oneway capability，pair 两端共享 token

```bash
nl -ba frameworks/base/core/java/android/view/IInputMonitorHost.aidl |
  sed -n '17,30p'

nl -ba frameworks/base/core/java/android/view/InputMonitor.java |
  sed -n '26,78p;127,159p'

nl -ba frameworks/base/core/java/android/view/InputChannel.java |
  sed -n '153,187p'

nl -ba frameworks/native/libs/input/InputTransport.cpp |
  sed -n '249,305p;383,429p'

nl -ba frameworks/native/libs/binder/Parcel.cpp |
  sed -n '1127,1161p'

nl -ba system/tools/aidl/aidl_to_java.cpp |
  sed -n '185,191p;382,399p'
```

画出 client channel、Host Binder、server channel 和 shared token；再标明返回值 flags 怎样 dispose 发送侧 wrapper，以及跨进程方法返回能证明的最远完成点。

### 练习 3：比较普通 channel 与 monitor 注册的不变量

```bash
nl -ba frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp |
  sed -n '1343,1428p'

nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '4299,4357p'

nl -ba system/core/libutils/Looper.cpp |
  sed -n '465,510p'
```

圈出普通注册的 token 查重、dispose callback，以及 monitor 路径忽略 `addFd()` 返回的位置。

### 练习 4：证明 global 只追加在成功 key/motion 路径

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '1111,1212p;1225,1306p;2032,2054p'
```

分别记录 pending、permission denied、其他 motion failure 与 success 会不会追加 global、会不会取消 monitors。

### 练习 5：恢复 gesture DOWN 快照与 sticky TouchState

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '849,862p;1536,1561p;1580,1750p;1894,1997p'

nl -ba frameworks/native/services/inputflinger/dispatcher/TouchState.cpp |
  sed -n '27,123p'
```

重点比较 `newGesture` 与 `isDown`，并验证 POINTER_DOWN 为什么不会吸收新 monitor。

### 练习 6：逐行走通无窗口注入权限缺口

```bash
nl -ba frameworks/base/services/core/java/com/android/server/input/InputManagerService.java |
  sed -n '641,681p;2000,2005p'

nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '598,605p;1235,1240p;1819,1845p;1894,1924p;2056,2076p;3274,3290p;3398,3464p'
```

令 `windows` 为空、`gestureMonitors` 非空、`injectionPermission` 初始 UNKNOWN，手算哪一行把它改成 GRANTED、null-window check 为何被跳过；再比较 ASYNC 与两种同步模式，并解释 policy-consumed success 为何仍不能证明目标路径执行。

### 练习 7：验证 monitor target、坐标与实际 publish 边界

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '1999,2054p;2209,2263p;2298,2426p;2456,2614p'

nl -ba frameworks/native/services/inputflinger/dispatcher/InputTarget.h |
  sed -n '26,146p'
```

区分 default pointer set、offset、scale、displayId、foreground 完成计数和 wait queue。

### 练习 8：证明 pilfer 的三道校验与保留字段

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '4429,4489p'

nl -ba frameworks/native/services/inputflinger/dispatcher/TouchState.h |
  sed -n '29,57p'

nl -ba frameworks/native/services/inputflinger/dispatcher/TouchState.cpp |
  sed -n '32,52p;89,123p'
```

把每个 `BAD_VALUE` 条件列出来，再解释为什么 registration display 与 source display 不同会破坏 portal pilfer。

### 练习 9：闭合 ANR、注销、HUP 与诊断

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp |
  sed -n '489,531p;1338,1378p;2696,2800p;4088,4182p;4379,4428p;4741,4800p'

nl -ba frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java |
  sed -n '175,275p'

nl -ba frameworks/base/core/java/android/view/InputChannel.java |
  sed -n '113,120p'

nl -ba frameworks/base/core/java/android/view/InputEventReceiver.java |
  sed -n '63,74p;88,114p'

nl -ba frameworks/base/core/jni/android_view_InputEventReceiver.cpp |
  sed -n '95,122p'

nl -ba frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp |
  sed -n '1468,1580p;2559,2610p'
```

分别标出 responsive 变 false、monitor ANR 找不到 WindowState、HUP 自动注销、逻辑 ZOMBIE 与迟到 FINISHED 恢复；再列出现有 tests 已证明与尚未证明的 pilfer 条件。

### 检查题与答案

1. **为什么 global monitor 不能单独让无窗口 DOWN 成功？**  
   因为它在 pointer 目标查找成功后才追加，不参与“窗口或 gesture monitor 至少存在一个”的判定。

2. **为什么手势中途注册的 gesture monitor 不能 pilfer 当前流？**  
   它没有在 ACTION_DOWN 时写入当前 `TouchState.gestureMonitors`，过不了 pilfer 第三道校验。

3. **App 收到 CANCEL 后，发起者为何不重收 DOWN？**  
   发起者从原始 DOWN 起就在流中；pilfer 只剪掉 window/portal 分支，不创建新流。

4. **为什么一个 gesture monitor pilfer 后，另一个仍能收到 MOVE？**  
   `filterNonMonitors()` 保留整个 `gestureMonitors` vector，没有记录独占 owner。

5. **什么情况下 pilfer 后普通窗口可能重新出现？**  
   旧 state 的 `split=true`，后续 ACTION_POINTER_DOWN 对新 pointer 再 hit-test 并加入 split window。

6. **为什么 portal monitor 能收到 DOWN 却可能无法 pilfer？**  
   membership 保存进源 display 的 TouchState，pilfer 却按 monitor 注册 display 查 TouchState。

7. **没有 FOREGROUND flag，为何 monitor 仍会 ANR？**  
   它影响 injection 的 pending-foreground 计数与 unhandled-key fallback，但不豁免传输账；成功 publish 后仍进入 Connection wait queue 与 ANR tracker。

8. **`InputMonitor.pilferPointers()` 正常返回能证明 native 返回 OK 吗？**  
   不能。Host 是 oneway，JNI 还丢弃 native `status_t`；跨进程 caller 只知道请求已提交。

### 最后只保留这张心智图

```text
注册
  global registry ──成功 key/motion──> 每事件 InputTarget
  gesture registry ──ACTION_DOWN─────> TouchState.gestureMonitors
                                              │
                                              ├── 后续 pointer sticky
                                              └── pilfer
                                                    │
                       尝试给 TouchState.windows 合成 CANCEL
                                                    │
                       clear windows + portalWindows
                       keep all monitors + down + split + device/source/display

任意 InputTarget
  -> Connection 仍存在且 NORMAL
  -> InputState 接受序列
  -> publish
  -> wait FINISHED / ANR
```

下一章将精读 **InputReaderPolicy、InputReaderConfiguration 与 DisplayViewport**：system_server 的输入配置怎样跨 JNI 进入 Reader，configuration generation 如何触发 Mapper 重新配置，以及设备坐标如何借 viewport 对齐逻辑与物理 display。
