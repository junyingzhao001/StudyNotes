# 245 Android Connection队列、Socket背压与FINISHED释放链

本文以 `android-11.0.0_r48` 为唯一源码基线，从第244章已经定型的 `DispatchEntry` 出发，追到它被正常回执、fallback 重发或异常 drain 后释放。现实中的典型疑问是：`dumpsys input` 明明显示 entry 已在 wait，为什么 App 可能还没回调；Java 已调用 `finishInputEvent()`，为什么服务端欠账仍可能存在；一次 `WOULD_BLOCK` 又为什么有时只是背压、有时直接断链？

主线只问一件事：**一笔已进入 `Connection` 的 `DispatchEntry`，怎样跨 outbound、Unix socket、App 处理、反向 FINISHED 与 wait 账，在背压、乱序、fallback 或断链下最终释放，并且只结一次 foreground 债？**

这里不再解释目标、action、坐标与 eventId 如何派生，那是第244章的边界；也不回溯 Reader 通知、inbound queue 与 `dispatchOnce` 如何选中这笔事件，那些留给第246章。

## 1. 主问题：一笔“已发送”的事件其实有五个完成点

排障时最危险的词是“发完了”。r48 至少有五个不同完成点：

| 完成点 | 能证明什么 | 还不能证明什么 |
|---|---|---|
| entry 已入 outbound | Dispatcher 已为该 connection 建立投递凭据 | socket 接受了消息 |
| `sendMessage()` 返回 OK | 一个完整 `InputMessage` 已写入内核 socket | App 已从 socket 读取 |
| entry 已入 wait | Dispatcher 已建立等待 FINISHED 的欠账 | 消息当前在内核、JNI、IME 还是 View |
| live receiver 对命中 `mSeqMap` 的事件调用 `finishInputEvent()` | Java 已给出 handled 结论并消费自己的 sequence 映射 | FINISHED 已到服务端；反向 socket 可能满 |
| `releaseDispatchEntry()` | 该投递凭据已结账，引用和 foreground 债按规则释放 | `handled` 必为 true，或 App 必然处理过事件 |

`waitQueue` 因而不是“socket 之后的第三个物理缓冲”。正向 send 成功的同一时刻，消息进入内核，而对应 entry 进入 wait；此后消息可以仍在内核、已被 native consumer 读取、正在 InputStage 中，甚至已处理完但 FINISHED 卡在反向队列，wait 记录都仍然存在。它是一张服务端欠账表，与实际数据位置重叠。

本章将三本账分开：`Connection` 的 outbound/wait、双向 socket 的包、客户端的 Java map/seq chain/finish queue。只有这样，背压、ANR、乱序确认和异常释放才不会被压成一条虚假的线性流水线。

## 2. Connection：status、responsive 与两条队列是四个独立维度

每个注册的 server 端 `InputChannel` 对应一个 `Connection`。它持有 channel、`InputPublisher`、该接收者的 `InputState`、`monitor` 标记、状态、响应性，以及两条装 `DispatchEntry*` 的 deque。

状态只有三种：

| `status` | 含义 | 新的 prepare 是否入队 |
|---|---|---|
| `NORMAL` | 通道仍可参加发送周期 | 可以 |
| `BROKEN` | 已遇到不可恢复通信错误 | 跳过 |
| `ZOMBIE` | 已走 unregister；既可能是显式调用，也可能由 receive error/HANGUP 触发 | 跳过 |

`responsive` 是另一根轴。ANR 到点会把它改为 false，但不会把 `status` 自动改成 BROKEN，也不会自动清空两条队列。r48 会用它过滤新 touch 手势的普通窗口与 gesture monitor，并停止为该 connection 的新成功 publish 项登记 ANR 索引；global monitor 的常规添加并不做同一项 responsive 过滤。`startDispatchCycleLocked()` 本身也只检查 `status == NORMAL`，不检查 `responsive`。既有流、焦点事件和合成 CANCEL 仍可能继续入队或 publish，只是新 wait 项可能没有 tracker 记录。

`findWaitQueueEntry(seq)` 线性扫描整个 wait deque，不要求命中队首。这个设计允许非队首 FINISHED，但也意味着“deque 保持发送顺序”与“确认必须按顺序”是两回事。

### 练习 1：给一份 Connection 快照标注三本账

固定 `Connection` 为 NORMAL、responsive=true，outbound 为 `[seq31, seq32]`，wait 为 `[seq21, seq22]`；seq31/32 都是首轮 entry，不是 fallback restart，其中 seq31 已尝试 send 并因 wait 非空而返回 WOULD_BLOCK，seq32 尚未轮到。独立 trace 已知：seq21/22 成功 publish 时 connection 都 responsive，此后没有 ANR、timeout extension、FINISHED、unregister 或 `resetAndDropEverythingLocked()`；App 已从正向 socket 读走 seq21，seq22 的消息仍在 server→client 内核缓冲。

唯一答案是：seq31 的首次 send 尚未成功，seq32 尚未尝试，客户端都不承担这两笔的确认责任；seq21/22 已成功 publish，也都在服务端 wait 欠账中。seq21 正在 App 侧某阶段，seq22 仍在内核，但仅看 wait 无法区分它们的位置。`mAnrTracker` 中属于该 connection token 的记录恰有两项，分别由 seq21/22 成功时插入；outbound 两项尚未产生 tracker 记录，其他 token 是否有记录不影响本题。outbound 长度本身不是 ANR 计数。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'enum Status {' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'STATUS_BROKEN,' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'STATUS_ZOMBIE' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'bool responsive = true;' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'std::deque<DispatchEntry*> outboundQueue;' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'std::deque<DispatchEntry*> waitQueue;' frameworks/native/services/inputflinger/dispatcher/Connection.h
grep -n -F 'Connection::findWaitQueueEntry(uint32_t seq)' frameworks/native/services/inputflinger/dispatcher/Connection.cpp
grep -n -F 'if ((*it)->seq == seq) {' frameworks/native/services/inputflinger/dispatcher/Connection.cpp
grep -n -F 'while (connection->status == Connection::STATUS_NORMAL && !connection->outboundQueue.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.clear();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 3. InputChannel 是成对、全双工、保包边界的非阻塞协议

`openInputChannelPair()` 用 `socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets)` 创建两个等价端点；server/client 只是便于描述的名字。两端共享同一个 connection token，但各自有 fd，不能用 token 判断“这是同一个端点对象”。

创建 `InputChannel` 时还用 `fcntl(..., O_NONBLOCK)` 把 fd 设为非阻塞；每次 send 又显式传 `MSG_DONTWAIT | MSG_NOSIGNAL`。正向 KEY/MOTION/FOCUS 与反向 FINISHED 走同一对全双工 socket 的相反方向，各有自己的发送压力。一个方向满，不等于另一个方向同时满。

r48 请求把两端的 `SO_SNDBUF` 与 `SO_RCVBUF` 都设为 32 KiB，但四次 `setsockopt()` 的返回值没有被检查。内核还可能调整、加倍或限制实际容量；不同 type、不同 pointerCount 的 `InputMessage::size()` 也不同。因此“32 KiB 等于固定 N 笔 Motion”不是源码契约。

`SOCK_SEQPACKET` 保留包边界。Motion 的 message 长度只覆盖实际 pointer 数量；接收端要求收到的长度与该 type 自报长度精确一致。发送前 `getSanitizedCopy()` 先清零整块结构，再逐字段复制有效内容，目的是避免 union 未用区和 padding 泄漏，不是把任意非法消息自动修好。

## 4. send 与 recv 的状态表不同：同一个 WOULD_BLOCK 也不是同一句话

`InputChannel::sendMessage()` 与 `receiveMessage()` 都在 EINTR 时重试，但错误映射不能混成一张无方向的表：

| 调用 | 条件 | r48 返回 | 语义 |
|---|---|---|---|
| send | EAGAIN/EWOULDBLOCK | `WOULD_BLOCK` | 当前发送缓冲无空间；头文件保证整包未发送 |
| send | EPIPE/ENOTCONN/ECONNREFUSED/ECONNRESET | `DEAD_OBJECT` | peer 或连接不可用 |
| send | 正长度但小于 message length | `DEAD_OBJECT` | 不接受半包成功 |
| recv | EAGAIN/EWOULDBLOCK | `WOULD_BLOCK` | 当前没有包可读，不表示“接收缓冲满” |
| recv | EPIPE/ENOTCONN/ECONNREFUSED | `DEAD_OBJECT` | 已知的 peer/连接失效 |
| recv | 返回 0 | `DEAD_OBJECT` | EOF，peer 已关闭 |
| recv | ECONNRESET | `-ECONNRESET` | r48 没把它列进 recv 的 DEAD_OBJECT 分支 |
| recv | 包长度/type 内容不满足 `isValid()` | `BAD_VALUE` | 收到协议非法包 |

send 的其他 errno 返回负 errno；recv 也一样。上层可能把所有非 OK/非预期 WOULD_BLOCK 都当通道错误收口，但底层枚举仍不同。日志若只写“socket 错误”，会丢失“正向写不进”“当前无回执可读”“peer 已关”“协议包非法”四种完全不同的分支。

### 练习 2：把七种系统调用结果映射到协议状态

固定在 r48，不考虑厂商修改：A=send 得 EAGAIN；B=send 得 ECONNRESET；C=recv 得 EAGAIN；D=recv 得 ECONNRESET；E=recv 返回 0；F=send 返回小于完整包长的正数；G=recv 得到一个使 `isValid(actualSize)` 为 false 的包。

唯一答案依次是：A `WOULD_BLOCK` 且整包未发送；B `DEAD_OBJECT`；C `WOULD_BLOCK` 且只是当前无包；D `-ECONNRESET`；E `DEAD_OBJECT`；F `DEAD_OBJECT`；G `BAD_VALUE`。只有 A 是 Dispatcher 正向发送周期里的背压候选，C 则是正常“已经读空”结束条件。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static const size_t SOCKET_BUFFER_SIZE = 32 * 1024;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'const int result = fcntl(fd, F_SETFL, O_NONBLOCK);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets)) {' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'msg->getSanitizedCopy(&cleanMsg);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'nWrite = ::send(mFd.get(), &cleanMsg, msgLength, MSG_DONTWAIT | MSG_NOSIGNAL);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F '} while (nWrite == -1 && errno == EINTR);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'if (error == EPIPE || error == ENOTCONN || error == ECONNREFUSED || error == ECONNRESET) {' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'nRead = ::recv(mFd.get(), msg, sizeof(InputMessage), MSG_DONTWAIT);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'if (nRead == 0) { // check for EOF' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'if (!msg->isValid(nRead)) {' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'if (size_t(nWrite) != msgLength) {' frameworks/native/libs/input/InputTransport.cpp
```

## 5. outbound 到 wait：时间先写，send 成功后才迁账

`enqueueDispatchEntriesLocked()` 先记住 outbound 原先是否为空，再为目标的各 mode 建 entry。只有“调用前为空、调用后非空”才立即启动发送周期；已有队首因背压停住时，新 entry 只排在后面，不会因再次 enqueue 自动重试队首。

`startDispatchCycleLocked(currentTime, connection)` 在 status 为 NORMAL 且 outbound 非空时循环。每轮取队首，并在调用 publisher **之前**写：

- `deliveryTime = currentTime`；
- `timeoutTime = currentTime + getDispatchingTimeoutLocked(token)`。

同一次函数调用里的多笔 entry 共用传入的 `currentTime`，但 timeout 每笔仍重新按 token 查询。send 失败不会撤销这两个字段，只是它们还没有稳定的 wait/ANR 含义；后续重试会覆盖。

send 返回 OK 后，代码才从 outbound 删除该指针并 `push_back` 到 wait。若 connection 当时 responsive，再把 `(timeoutTime, token)` 插入 `mAnrTracker`。send OK 是完整 packet 的内核接纳点，也是服务端 wait 起点；它既不是 App read，也不是业务处理开始的硬件观测点。

### 练习 3：计算一次部分成功、一次重试后的时间与队列

固定默认 dispatch timeout 为 50 ns，connection NORMAL 且 responsive。t=100 时 outbound 原为空，同一次 enqueue 展开 seq1、seq2 并启动周期；seq1 send=OK，seq2 send=`WOULD_BLOCK`。t=120 收到 seq1 的合法 Motion FINISHED，处理期间 `now()` 也固定返回 120；随后重试 seq2，send=OK。

第一次周期结束时：wait=`[seq1]`，其 delivery/timeout=`100/150`，tracker 有 `(150,T)`；outbound=`[seq2]`，其字段也暂为 `100/150`，但无 tracker。t=120 处理 seq1 后，它从 wait 移除并释放；末尾重启周期覆盖 seq2 的字段为 `120/170`，最终 outbound 为空、wait=`[seq2]`、tracker 只有 `(170,T)`。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'bool wasEmpty = connection->outboundQueue.empty();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (wasEmpty && !connection->outboundQueue.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DispatchEntry* dispatchEntry = connection->outboundQueue.front();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->deliveryTime = currentTime;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'getDispatchingTimeoutLocked(connection->inputChannel->getConnectionToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntry->timeoutTime = currentTime + timeout;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.erase(std::remove(connection->outboundQueue.begin(),' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.push_back(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->responsive) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.insert(dispatchEntry->timeoutTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 6. 正向 WOULD_BLOCK 有两条路，且 Dispatcher 不监听可写事件

同样是 publisher 返回 `WOULD_BLOCK`，`startDispatchCycleLocked()` 还要看 wait：

| wait 状态 | 判断 | outbound 队首 | connection |
|---|---|---|---|
| 非空 | 已有成功 publish 但未确认的包，正向 pipe 满可以由慢 consumer 解释 | 保留，函数 return | 仍 NORMAL |
| 为空 | 没有任何服务端已发布欠账，源码认为 pipe 不该满 | 被 broken drain 释放 | NORMAL 会转 BROKEN |

server fd 注册到 Looper 时只请求 `ALOOPER_EVENT_INPUT`，没有像客户端回执队列那样订阅 OUTPUT。也就是说，正向 fd 后来变得可写本身不会直接唤醒 Dispatcher 重试。正常推进点是收到一笔**能命中 wait 的** FINISHED：命令移除/重启该 entry 后，末尾调用 `startDispatchCycleLocked(now(), connection)`。未知 seq 在第一次查找失败就 return，连这个重试调用也到不了；合成 cancel/down 等显式调用发送周期的路径也可能带来重试，但不能把它描述成通用 writable callback。

### 练习 4：比较空 wait、非空 wait 与未知 FINISHED

场景 A：NORMAL connection 的 wait 为空、outbound=`[seq51]`，send seq51 得 `WOULD_BLOCK`。场景 B：另一个 NORMAL、responsive connection 的 wait=`[seq41]`、outbound=`[seq51]`，tracker 含 seq41 对应的 pair；先 send seq51 得 `WOULD_BLOCK`，随后依次收到 FINISHED seq99 与 FINISHED seq41，二者都是格式合法的 Motion 回执，seq51 在最后一次重试时 send=OK。

唯一答案是：A 立即 drain seq51 并转 BROKEN，不等待 OUTPUT。B 的第一次背压保留 seq51；seq99 找不到 wait entry，队列不变且不会触发末尾重试；seq41 命中后被移除、撤对应 tracker、释放，再触发发送周期，最终 seq51 从 outbound 移到 wait，并因 connection 仍 responsive 而插入一项新 tracker。格式合法不等于 seq 已知。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (status == WOULD_BLOCK) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->waitQueue.empty()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'abortBrokenDispatchCycleLocked(currentTime, connection, true /*notify*/);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '// Pipe is full and we are waiting for the app to finish process some events' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mLooper->addFd(fd, 0, ALOOPER_EVENT_INPUT, handleReceiveCallback, this);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::deque<DispatchEntry*>::iterator dispatchEntryIt = connection->findWaitQueueEntry(seq);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (dispatchEntryIt == connection->waitQueue.end()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.erase(dispatchEntryIt);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'startDispatchCycleLocked(now(), connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 7. ANR tracker 是 timeout/token 多重集，不是 waitQueue 的镜像

`AnrTracker` 保存的是 `(timeoutTime, connectionToken)` multiset，不保存 seq。相同 connection、相同 deadline 可以有重复项；正常 FINISHED 用同一 pair 只擦一个，注销用 `eraseToken()` 擦该 token 的全部项。它是为了快速得到全局最早 deadline，而不是替代 waitQueue 的所有权账。

`processAnrsLocked()` 取 tracker 最早项。当前时间尚未到就把该 deadline 作为下次 wakeup；到点后找到 connection，将 `responsive=false`，先 `eraseToken()` 停止为它反复唤醒，再进入 policy ANR。这里没有 drain outbound/wait，也没有递减 injected foreground 债。

ANR reason 使用 waitQueue 的 oldest entry，因为它通常最能描述 App 堵在哪里；真正触发 tracker 最早 deadline 的却可能是较新的 entry，例如窗口 timeout 改变后，新 entry 的期限更早。日志“等待最老事件多久”与“哪一个 deadline 触发”不是同一个字段。

FINISHED 还以严格 `eventDuration > 2s` 记录 slow processing。这个阈值与每窗 ANR timeout 独立：一笔可产生 slow log 而未 ANR，也可能在不同 timeout 配置下先到 ANR。

## 8. unresponsive 的恢复有两个索引缺口，不能只看 bool

policy 若返回正 extension，r48 先把 connection 改回 responsive，计算 `newTimeout=now()+extension`。但它只对满足 `newTimeout >= old timeoutTime` 的 wait entry 改写 deadline 并插回 tracker；原 deadline 晚于 newTimeout 的 entry 既不改写，也不会在这一轮重新插入，因为 ANR 时该 token 的旧索引已被整批擦除。

迟到 FINISHED 的恢复更窄：移除命中 entry 后，若 connection 原为 unresponsive，就扫描剩余 wait；只要没有 `entry.timeoutTime < now()`，便把 responsive 设回 true。它不会为幸存 entry 补建 tracker。严格小于还意味着 deadline 恰好等于扫描时刻时被视为 responsive，尽管正常 `processAnrsLocked()` 在 `currentTime >= deadline` 时会到期。

所以 r48 存在“responsive 已 true、wait 仍非空、旧 entry 没有 tracker 项”的静态可达状态。后续新 publish 会为新 entry 建索引，但不会顺带补齐旧项；若新项将来触发 ANR，reason 又可能指向那个未索引的 oldest entry。诊断工具不能假设 `responsive == true` 等价于 tracker 与整条 waitQueue 完全同步。

### 练习 5：推演一次迟到 FINISHED 后的恢复缺口

从 ANR 标记完成后的状态开始：connection NORMAL、responsive=false，wait=`[seq61(deadline=100), seq62(deadline=170)]`，tracker 已对 token T 清空；两笔都是同一 injected Motion 的 foreground entry，`pendingForegroundDispatches=2`。令处理 seq61 FINISHED 时 `now()=130`，afterMotion 不重启。

唯一答案是：seq61 被移除并释放，pending 从 2 降到 1；seq62 留在 wait。扫描看到 `170 < 130` 为 false，于是 responsive 恢复 true；但 tracker 仍为空，因为该恢复分支没有 insert。即使时间随后到 180，单靠 tracker 也不会为 seq62 安排这一次 ANR wakeup。ANR 本身没有释放两笔债，真正改变 pending 的是这次 `releaseDispatchEntry()`。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'nextAnrCheck = std::min(nextAnrCheck, mAnrTracker.firstTimeout());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = false;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mAnrTracker.eraseToken(connection->inputChannel->getConnectionToken());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'DispatchEntry* oldestEntry = *connection->waitQueue.begin();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (eventDuration > SLOW_EVENT_PROCESSING_WARNING_TIMEOUT) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = true;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const nsecs_t newTimeout = now() + timeoutExtension;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newTimeout >= entry->timeoutTime) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (entry->timeoutTime < currentTime) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->responsive = isConnectionResponsive(*connection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 9. 客户端消费时同时出现 eventId、Java sequence 与 transport seq

App 端 native receiver 监听 client fd 的 INPUT，`InputConsumer.consume()` 从正向 socket 取包，构造 KeyEvent/MotionEvent，并把 transport seq 作为独立 `outSeq` 返回。message 的 `eventId` 进入 `InputEvent` 身份；transport seq 不进入 `MotionEvent.getId()`。

native 随后调用 Java 私有方法 `dispatchInputEvent(transportSeq, event)`。真正持有 `mSeqMap` 的是 Java `InputEventReceiver`：它以 `event.getSequenceNumber()` 为 key、transport seq 为 value，然后调用 `onInputEvent()`。因此三类编号必须分开：

| 编号 | 所在对象/账 | 用途 |
|---|---|---|
| eventId | KeyEvent/MotionEvent | 事件身份与追踪 |
| Java sequence | `InputEvent` 对象 | Java map key，区分对象代际 |
| transport seq | InputMessage/DispatchEntry | FINISHED 定位 wait entry |

Focus 是例外：native 调用 Java `onFocusEvent()` 后，直接以 transport seq、handled=true 走自己的 `finishInputEvent()`，不创建 Java `InputEvent`，也不进入 `mSeqMap`。

Native `consumeEvents()` 可以在一次 fd 回调中循环取多包并多次回调 Java；`InputEventReceiver` 文档的“finish 前不再收到新输入”应当作为接收者必须及时完成的使用契约理解，不能据此断言 r48 native 实现有一把严格 single-flight 门。ViewRoot 自己还有 `QueuedInputEvent` 链和可能异步返回的 stage。

## 10. ViewRoot 的 handled 结论先结束 Java 账，再尝试写反向消息

ViewRoot 把事件依次交给 InputStage。stage 可以 forward、finish handled 或 finish unhandled；异步 IME 等 stage 会延迟最终完成。链尾 `finishInputEvent(q)` 读取 `FLAG_FINISHED_HANDLED`，经 compatibility 反向处理后调用保存的 receiver。`handled=false` 仍必须完成协议，它只影响后续 policy/fallback，不代表“无需回执”。

Java `InputEventReceiver.finishInputEvent()` 的顺序是：

1. receiver 已 dispose 时只警告，不查 map、不发 native FINISHED；
2. 否则按 Java sequence 查 `mSeqMap`；未知或重复对象只警告；
3. 命中时先取 transport seq 并移除 map，再调用 native；
4. 方法走到末尾就调用 `recycleIfNeededAfterDispatch()`；MotionEvent 会回收，KeyEvent 的 override 是 no-op。

由于 map 在 native 调用前就删除，native 把 FINISHED 暂存到反向 finish queue 时，Java 侧也已经认为该对象不再 in progress。若 native 抛出非 DEAD_OBJECT 异常，map 不会自动恢复，方法末尾的条件回收也会被异常跳过；不能把 map 是否存在当成服务端 wait 的镜像。

finish 查的是**传入对象自身**的 Java sequence。r48 默认 `InputEventCompatProcessor` 就地调整并返回同一 MotionEvent，finish hook 默认也返回原对象；但接口允许 compatibility 层返回另一对象或 null。新 copy 会取得新的 Java sequence，框架不会自动把旧 map 映射迁给它；而 `processInputEventBeforeFinish()` 返回 null 时，ViewRoot 的该分支根本不调用 receiver finish。于是“兼容层产生过对象”并不天然保证原 transport seq 会被确认。

`dispose()` 也不是服务端立即结账：Java 先用 native dispose 取消 client Looper 的 fd 监听，再 dispose 本端 InputChannel、关闭 fd；Dispatcher 随后从 HANGUP/error 的注销路径移除 connection 并 drain。已 dispose 后调用 Java finish 只警告，不承担这次服务端清理。

### 练习 6：计算 Java map、native回执与 Focus 旁路

固定 live receiver 的 `mSeqMap={java1001→transport71, java1002→transport72}`，其中 java1002 明确是不会在 dispatch 后回收的 KeyEvent，且无 batch chain；再固定本机内存足够、这次 `mFinishQueue.add()` 成功。先 finish 一个 java sequence=999 的外来 KeyEvent；再 finish java1002、handled=false，此时直接反向 send 得 `WOULD_BLOCK`；最后再次 finish 同一个 java1002。另有 Focus transport seq73 到达，`onFocusEvent()` 正常返回且它的反向 send=OK。

唯一答案是：999 只产生 not-in-progress 警告，不写 FINISHED；第一次 java1002 从 map 删除 1002→72，native finishQueue 加 `(72,false)`，helper 返回 OK，因而 `void` JNI 调用正常返回且不抛异常；第二次 java1002 只再警告，不新增队列项。最终 Java map 只剩 1001→71。Focus 不进 map，native 直接写 FINISHED `(73,true)`。这些 Java 调用都不能证明排队的 seq72 已到 Dispatcher。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private final SparseIntArray mSeqMap = new SparseIntArray();' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'mSeqMap.put(event.getSequenceNumber(), seq);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'int index = mSeqMap.indexOfKey(event.getSequenceNumber());' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'int seq = mSeqMap.valueAt(index);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'mSeqMap.removeAt(index);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'nativeFinishInputEvent(mReceiverPtr, seq, handled);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'private static native void nativeFinishInputEvent(long receiverPtr, int seq, boolean handled);' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'status_t status = receiver->finishInputEvent(seq, handled);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'if (status && status != DEAD_OBJECT) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'event.recycleIfNeededAfterDispatch();' frameworks/base/core/java/android/view/InputEventReceiver.java
grep -n -F 'q.mReceiver.finishInputEvent(q.mEvent, handled);' frameworks/base/core/java/android/view/ViewRootImpl.java
grep -n -F 'finishInputEvent(seq, true /* handled */);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
```

## 11. 正常反向背压走 mFinishQueue 路径，但它允许跨队列乱序

NativeInputEventReceiver 的常规 `finishInputEvent(seq, handled)` 先调用 `InputConsumer.sendFinishedSignal()`。若返回 WOULD_BLOCK，它尝试用 `mFinishQueue.add()` 保存终端 seq/handled，把 fd 监听改为 INPUT|OUTPUT，再让 native helper 返回 OK；由于 Java 声明是 `void nativeFinishInputEvent(...)`，可观察效果是 JNI 调用正常返回、不抛异常，而不是 Java 收到一个 OK 值。`mFinishQueue.add()` 的返回值本身没有检查，所以这个本地 OK 也不能强化成耐久入队或 Dispatcher 接纳证明。

OUTPUT 回调从 vector 前向后发送。第 i 项失败时，`removeItemsAt(0,i)` 只删已成功前缀；若仍是 WOULD_BLOCK，保留失败项及后缀并继续监听。全部成功才 clear 队列、恢复只监听 INPUT。

有两个不显眼的顺序边界：

- `finishInputEvent()` 不检查旧 finishQueue 是否非空，新完成项仍先尝试直接 send；若此刻 socket 恢复空间，它可能越过旧队列中的 FINISHED。服务端按 seq 查找正好容忍这种顺序。
- `handleEvent()` 在同一次 flags 同时含 INPUT 与 OUTPUT 时先走 INPUT 分支并 return，OUTPUT 队列要等后续回调；“fd 可写”不保证这一轮立即 drain 回执。

非 WOULD_BLOCK 错误不会进入常规重试：直接 Java 调用中，DEAD_OBJECT 不抛 RuntimeException，其他错误会抛；OUTPUT 回调中则移除 fd callback，非 DEAD_OBJECT 还会创建并清理 Java 异常。无论哪种情况，都不是 FINISHED 已被 Dispatcher 接纳的证明。

### 练习 7：证明新回执可以越过旧 finishQueue

固定无 batch chain，native finishQueue 初始为 `[(10,true),(11,false)]`，监听 INPUT|OUTPUT。一次纯 OUTPUT 回调中，send10=OK、send11=`WOULD_BLOCK`；回调返回后、下一次 OUTPUT 前，Java 完成 seq12，直接 send12=OK；再下一次 OUTPUT 中 send11=OK。

唯一答案是：第一次 OUTPUT 删除成功前缀10，只留下 `(11,false)` 并保持 OUTPUT；新 seq12 不先排队，而是直接到 socket；最后 seq11 才从队列发出。服务端观察顺序可以是 10、12、11。最后 finishQueue 清空、监听恢复 INPUT-only。乱序不是 deque 自己重排，而是“新直发路径”和“旧重试路径”并存的结果。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'status_t status = mInputConsumer.sendFinishedSignal(seq, handled);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'if (status == WOULD_BLOCK) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'mFinishQueue.add(finish);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'setFdEvents(ALOOPER_EVENT_INPUT | ALOOPER_EVENT_OUTPUT);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'if (events & ALOOPER_EVENT_INPUT) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'if (events & ALOOPER_EVENT_OUTPUT) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'for (size_t i = 0; i < mFinishQueue.size(); i++) {' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'mFinishQueue.removeItemsAt(0, i);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'mFinishQueue.clear();' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
grep -n -F 'setFdEvents(ALOOPER_EVENT_INPUT);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
```

## 12. Motion batch 用 seq chain 拆账，但两条自动回执路径绕过 finishQueue

InputConsumer 会暂存 MOVE/HOVER_MOVE。它先按 deviceId/source 找 batch，再要求 action、pointerCount 与每个 PointerProperties 一致才追加。这里没有逐项比较 displayId、flags、scale 等所有字段；输出 MotionEvent 由第一包初始化，后续 sample 主要追加时间/coords并 OR metaState。这依赖同一手势流的上游不变量，不能把 `canAddSample()` 当完整消息等价校验。

消费 N 个 sample 时，第一包初始化 Java MotionEvent；每个后续包建立 `SeqChain{seq=当前, chain=前一包}`，最终 `outSeq` 是最后一包。Java map 只需记终端 seq。完成时 InputConsumer 反向遍历 chain，按最老到最新发送每个前驱 FINISHED，最后发送终端 seq；一份 Java handled 值用于整条链。

若中途发送失败，代码重建尚未确认的 chain 并返回错误。仅当这个返回值是 WOULD_BLOCK 时，常规 NativeInputEventReceiver 才尝试用 `mFinishQueue.add()` 保存终端 seq；若添加成功，下一次用终端 seq 重试时，InputConsumer 会再次展开剩余链，不会重发已经成功的前缀。其他错误不走这条排队恢复。

resampling 还可能通过 interpolation 或 extrapolation 向 Java MotionEvent 增加本地 sample，但这种 history 没有独立 transport seq。不能用 `MotionEvent.getHistorySize()` 反推应该回多少个 FINISHED；回执数由实际消费的 InputMessage 与 seq chain 决定。

但 mFinishQueue 不是所有自动回执的兜底：

- 已有 pointer batch 遇 CANCEL 时，InputConsumer 对被抛弃 sample 直接调用 `sendFinishedSignal(seq,false)`，忽略返回值；
- Java 对象创建失败会进入 `skipCallbacks`；当前及同一次 consume loop 的剩余事件都直接调用 `mInputConsumer.sendFinishedSignal(seq,false)`；
- `dispatchInputEvent()` 中的 `onInputEvent()` 抛异常也会进入同一分支。异常发生前 Java 已执行 `mSeqMap.put()`，所以当前 transport seq 虽被 native 直发 false，Java map 还可能留下这笔映射。

上面是两个忽略返回值的源码发送点：CANCEL 丢 batch 的一处，以及 `skipCallbacks` 统一收口的一处；后者有对象创建失败和 callback 异常两种入口。它们在反向 socket 满时可能没有留下 finishQueue 重试项。只能说“正常 Java/native finish helper 的 WOULD_BLOCK 会尝试排队”，不能说所有 FINISHED 都经过同一重试层。

### 练习 8：重建一次部分失败的 batch seq chain

三包 MOVE 的 transport seq 为 41、42、43，被合成一份 Java MotionEvent，handled=true。初始 chain 为 `42→41、43→42`。第一次 finish 时 send41=OK、send42=`WOULD_BLOCK`；常规 NativeInputEventReceiver 接住返回值，且固定这次 `mFinishQueue.add()` 成功。下一次 OUTPUT 重试中所有 send 都成功。

唯一答案是：第一次调用先删除已解析的旧 chain，再成功发41；42失败时只重建 `43→42`，InputConsumer 返回 WOULD_BLOCK，native finishQueue 存终端 `(43,true)`，43 尚未发送。重试终端43时，chain 先展开并发送42，再发送43；服务端最终收到 41、42、43 各一次，三笔都使用 handled=true。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mMsg.body.motion.action == AMOTION_EVENT_ACTION_MOVE ||' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'SeqChain seqChain;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'seqChain.seq = msg.body.motion.seq;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'seqChain.chain = chain;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F '*outSeq = chain;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'for (size_t i = seqChainCount; i > 0; ) {' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'chainSeqs[chainIndex++] = currentSeq;' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'status = sendUnchainedFinishedSignal(chainSeqs[chainIndex], handled);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F '// An error occurred so at least one signal was not sent, reconstruct the chain.' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'return sendUnchainedFinishedSignal(seq, handled);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'sendFinishedSignal(msg.body.motion.seq, false);' frameworks/native/libs/input/InputTransport.cpp
grep -n -F 'mInputConsumer.sendFinishedSignal(seq, false);' frameworks/base/core/jni/android_view_InputEventReceiver.cpp
```

## 13. 服务端先收包再 post 命令：允许非队首，也必须二次查找

server fd 读回执时先固定一次 `currentTime=now()`，循环 `receiveFinishedSignal()` 直到非 OK。每个合法 FINISHED 只含 seq 与 handled，接收函数校验消息 type 后，`finishDispatchCycleLocked()` 为 NORMAL connection post 一条 command；BROKEN/ZOMBIE 直接忽略。若本轮至少收到一包，Dispatcher 先运行这些 command；最后状态是 WOULD_BLOCK 时保留 fd callback。

“读到 WOULD_BLOCK 后正常返回”也有前提：本轮至少成功读过一包。对 publisher 来说，收到非 FINISHED type 会返回 UNKNOWN_ERROR，底层包长非法会是 BAD_VALUE，其他非预期状态也会走注销；只有格式与 type 合法、但 seq 在 command 阶段未知的 FINISHED 才会被静默忽略而不拆 connection。

因此同一 receive callback 排空的多包共享 finishTime。2 秒 slow duration 用这个接收时刻减各自 `deliveryTime`，不是 Java 调用时刻，也不是 command 真正运行时刻。

command 第一次按 seq 扫 wait：找不到就静默 return，既不误删别项，也不启动 blocked outbound。找到后先统计，再按事件类型调 `after*`：Motion 版本在 r48 恒返回 false 且不解锁，Key 版本的特定 policy/fallback 分支才可能解锁。统一流程重新加锁后仍做第二次 seq 查找，以防 Key 回调期间 connection 已注销或队列已被 drain。只有仍命中时才执行前四步：

1. 从 wait 擦 entry；
2. 按 `(timeoutTime, token)` 擦一个 ANR 索引；
3. 若此前 unresponsive，扫描剩余 wait 重算 bool；
4. restart 则把同一指针压到 outbound 前端，否则 release。

一旦第一次查找曾命中，函数末尾都会启动下一轮发送；这一步在第二次查找的条件块之外。若 Key policy 回调期间原 entry 已消失，前四步跳过，发送周期仍会依据此刻的 status 与 outbound 决定是否推进。

“未知/重复 FINISHED 安全”必须加条件：当前 wait 中没有同 seq 的 live entry。通常 seq 全局唯一且 Java map 防重复；但 Key fallback 会主动复用同一 `DispatchEntry::seq`，于是旧重复包可能别名命中新一轮 fallback wait。

## 14. handled 只影响后处理；Key fallback 让同一 seq 再活一轮

Motion 的 `afterMotionEventLockedInterruptible()` 在 r48 恒返回 false，所以 handled=true/false 都走普通释放。Key 更复杂：只有 foreground、非 fallback 的未处理 Key 才可能问 policy 是否产生 fallback；初始 DOWN 决定并锁存 fallback keyCode，后续代际沿用。

policy 回调期间 Dispatcher 解锁。若 policy 要 fallback，源码原地改写共享 `KeyEntry` 的 eventTime、device/source/display、flags、keyCode、scanCode、meta/repeat/downTime，并返回 restart。第二次查找仍命中且 connection 仍 NORMAL 时，同一 `DispatchEntry` 从 wait 移到 outbound 前端；它没有重新构造，因此：

- transport seq 不变；
- foreground pending 不先减、也不再加；
- 下一次发送会覆盖 delivery/timeout；只有 send=OK 才建立新 wait 周期；
- `resolvedEventId/resolvedAction/resolvedFlags` 没有重新跑 create/track 分支。publish 的 keyCode 等字段读取已改写 KeyEntry，但 action/flags/eventId 仍读取旧 DispatchEntry 的 resolved 字段。

restart 并不保证重发成功。旧 entry 已先离开 wait；若它原本是唯一 wait 项，紧接着的 fallback send 返回 WOULD_BLOCK，就会落入“wait 为空”的异常分支，drain 并置 BROKEN；若还有其他 wait 项，则 fallback 留在 outbound 等已知 FINISHED 推进。只有重发 OK 时，它才回到 wait，并在 responsive 时插入新 tracker 项。

fallback 自身若 handled=false，因 KeyEntry 已带 FALLBACK 标志，后处理只报告 unhandled并返回 false，不会无限 fallback。

按 seq 定位非队首 entry 只保证容器操作可行，不保证 fallback 状态机对任意确认顺序语义等价。若同一原始按键的 UP 先以 handled=false 完成，而初始 DOWN 后完成并首次建立 fallback 映射，UP 当时既不是 initial DOWN、又查不到已锁存映射，会直接错过重发；结果可能只剩一笔 fallback DOWN。

### 练习 9：推演两个重复旧回执怎样提前结束 fallback

构造一个协议异常对端：NORMAL、responsive connection 的 wait 中只有 injected foreground Key，`action=DOWN`、`repeatCount=0`、不带 FALLBACK flag、`seq=90`、pending=1，且 tracker 含它的 pair；对端在 Dispatcher 运行 command 前连续写入两包 `(90,false)`。两包在同一次 receive loop 被转成两条 command。第一条调用 policy，policy 返回一枚 fallback key，fallback send=OK；处理两条 command 期间 client 不读取正向 socket，其他调用也不注销 connection。

唯一答案是：第一条 command 移除旧 wait/ANR 项，把同一 DispatchEntry push_front 并立即以同一 seq90 成功重发；pending 仍为1，新 wait 又出现 seq90。第二条“重复旧回执”随后按 seq 查找时会命中这笔 fallback；KeyEntry 已带 FALLBACK，它不再 restart，而是被移除并 release，pending 降到0。于是 App 尚未处理 fallback，服务端账却被旧重复包提前结清。无异常的常规 Java finish 路径会借 `mSeqMap` 阻止普通重复 finish，但服务端的 seq 复用使恶意或错误 peer 场景不能被概括为绝对无害。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'status = connection->inputPublisher.receiveFinishedSignal(&seq, &handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'd->finishDispatchCycleLocked(currentTime, connection, seq, handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'd->runCommandsLockedInterruptible();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (connection->status == Connection::STATUS_BROKEN ||' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'afterKeyEventLockedInterruptible(connection, dispatchEntry, keyEntry, handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchEntryIt = connection->findWaitQueueEntry(seq);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (keyEntry->flags & AKEY_EVENT_FLAG_FALLBACK) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return true; // restart the event' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_front(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'releaseDispatchEntry(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'decrementPendingForegroundDispatches(dispatchEntry->eventEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'nsecs_t endTime = now() + std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectedEntries.back()->injectionState = injectionState;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'while (injectionState->pendingForegroundDispatches != 0) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'injectionState->injectionResult = injectionResult;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'combinedMotionEntry->injectionState->refCount += 1;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'splitMotionEntry->injectionState->refCount += 1;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'addRecentEventLocked(entry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

## 15. broken、zombie、ANR 与正常 FINISHED 释放的是不同范围

异常结束必须按入口区分：

| 入口 | 队列 | ANR tracker | status | foreground 债 |
|---|---|---|---|---|
| 正常 FINISHED（Motion 或不 restart 的 Key） | 只移除命中 wait entry | 尝试擦该 pair 一项，无项则 no-op | 不变 | 该 foreground entry 减 1 |
| Key fallback restart 到重发前 | wait→outbound front | 尝试擦旧 pair 一项 | 仍 NORMAL | 不减、不重加 |
| ANR 到点 | outbound/wait 不 drain | `eraseToken()` 全擦 | status 仍 NORMAL，responsive=false | 不减 |
| send 错误或空-wait WOULD_BLOCK | outbound/wait 全 drain | `abortBrokenDispatchCycleLocked()` 本身不擦 tracker | NORMAL→BROKEN | 每个被释放 foreground entry 各减 1 |
| unregister | 先 remove connection 并 `eraseToken()`，再 drain | token 全擦 | 最终 ZOMBIE | 每个被释放 foreground entry 各减 1 |

`drainDispatchQueue()` 对每个指针调用 `releaseDispatchEntry()`；后者只按 target 的 FOREGROUND 位决定是否调用 decrement，真正没有 `InjectionState` 时 decrement helper 什么也不做。`WAIT_FOR_FINISHED` 等的是共享 `pendingForegroundDispatches` 归零，不要求 handled=true，也不要求 entry 一定曾成功 send。BROKEN drain 可以让一个从未到 App 的 foreground outbound entry 完成这本同步注入账。

fallback 重发随后重新进入普通发送矩阵：send=OK 才从 outbound 回到 wait；WOULD_BLOCK 且仍有其他 wait 时保留 outbound；WOULD_BLOCK 且 wait 已空，或其他 send error，则 broken drain 并在释放该 entry 时把 foreground 债减掉。因而“restart 当下 pending 不变”和“这笔债最终只减一次”可以同时成立。

foreground pending 在 entry 通过 InputState、准备压入 outbound 时就增加，不是 publish 或 tracker 成功后才增加；而 routing 成功还会更早把 `injectionResult` 写成 SUCCEEDED，不能把这个字段误读成 publish 或 App 处理成功。注入调用的同一个 deadline 同时约束“等待 injectionResult”与“等待 pending 归零”；超时只把本次调用的局部返回值改成 TIMED_OUT，不会撤销队列、不递减 pending，也不会把已经写入 `InjectionState.injectionResult` 的 SUCCEEDED 改回去。ANR policy 选择不延长时只合成 CANCEL，并明确不主动 break connection，原 wait 债仍要等 FINISHED 或后续断链清理。

引用账又与 pending 分开。带 history 的 injected Motion 只把 InjectionState 挂到 `injectedEntries.back()`，而 split/combined clone 会按自己的构造路径增加 state 引用；这些引用计数都不能由 history size 推导。`DispatchEntry` 析构释放一份 EventEntry 引用，EventEntry 最后一份引用消失时才释放其 InjectionState；recent event 队列还可能继续持有 EventEntry。因此 pending=0 足以唤醒 WAIT_FOR_FINISHED，却不保证 EventEntry 或 InjectionState 已立刻析构。

direct publish error 的 broken drain 没有同步 `eraseToken()`。若此前 wait 中有已登记项，队列虽已释放，tracker 可暂时留下 stale pair；后续 removeConnection 会整 token 清除，或者到期检查先把 responsive 置 false、擦掉 token，再因 wait 已空而不 raise ANR。不能把“队列与 pending 已闭合”写成“所有索引当场完全同步”。

注销先把 connection 从 fd/token 活动映射移除并清 tracker，再 remove fd、调用 abort drain，最后把 status 覆盖为 ZOMBIE。receive error/HANGUP 路径虽然计算普通窗口与 monitor 不同的 notify 值，但 abort post 的 broken command 会在稍后看到 ZOMBIE 而跳过 policy callback；r48 这里的日志/notify 布尔也不能直接当成“WMS 已收到 broken 通知”的完成证明。

## 16. 诊断顺序：从欠账位置走到唯一释放点

`dumpsys input` 会为每条活动 Connection 打印 fd、channel/window 名、status、monitor、responsive，以及 outbound/wait 长度。outbound 项只有基于 `eventTime` 的 age；wait 项额外有基于 `deliveryTime` 的 wait。它不打印 mFinishQueue、Java mSeqMap、InputConsumer seq chains，也不直接打印 AnrTracker 的完整多重集；因此一份 server dump 无法定位 wait 消息究竟卡在内核、JNI、IME、View 还是反向 FINISHED。

最短排障顺序是：

1. 先分 status 与 responsive：BROKEN/ZOMBIE 是生命周期，false 是超时评价。
2. 看 outbound/wait：outbound 尚未 send OK；wait 只是已发布未确认账，不是物理位置。
3. 对 outbound 队首区分 WOULD_BLOCK 的 wait 空/非空分支；确认是否真的存在可触发重试的已知 FINISHED。
4. 对 wait 写出 delivery、timeout、当时 responsive，以及 tracker 是否实际插入；不要由 bool 反推全索引。
5. 在客户端分别找 transport seq、Java sequence、eventId；再查 map、batch chain 与 finishQueue。
6. 区分常规 helper 和两个忽略 send status 的自动 false-FINISHED 旁路。
7. 服务端按 connection+seq 做第一次查找，考虑 policy 解锁后第二次查找；非队首合法，未知 seq 不推进 outbound。
8. 若是 Key，确认是否原地 fallback、是否复用 seq；若是 Motion，handled 不改变 restart=false。
9. 最后只在 `releaseDispatchEntry()` 或 drain 点结 foreground 债，并单独核对 tracker 是否同步清理。

r48 的闭环可以压成一句话：**outbound 证明“准备写”，send OK 同时建立内核包与 wait 欠账，Java finish 先结本地对象账，FINISHED 到达并命中 live seq 后才正常释放；fallback、ANR 与 broken 则分别改变重发、响应性和强制清账范围。**

到这里，第245章完成的是 `DispatchEntry → Connection → socket → App → FINISHED → release`。下一章将回到它的上游，追 `NotifyArgs` 怎样跨 Reader/Dispatcher 边界进入 inbound queue，怎样被 `dispatchOnce` 选为 pending event，并在 policy、dropReason 与 dispatchInProgress 之间到达本章入口。
