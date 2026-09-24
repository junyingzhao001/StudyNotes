# 37 · TCP 客户端与服务端

[返回学习地图](../README.md) · [上一篇](36-Asio执行模型.md) · [下一篇](38-消息边界超时与背压.md)

## 先说这篇讲什么

这篇讲服务端 accept、客户端 connect，以及怎样正确完成读写而不是假设一次操作传完全部数据。

## 它有什么用

TCP 服务的核心是连接生命周期。每个连接需要自己的 socket、输入缓冲、输出队列和错误处理。

## 要解决的问题

socket.read_some 可能只读到一部分；多个 async_write 同时使用同一 socket 还可能让输出交错。

## 先把几个词说清楚

- **acceptor**：服务端监听端口并接受连接的对象
- **连接会话**：管理一条连接全部状态的对象
- **部分读写**：一次系统调用只完成部分字节

## 一个案例

EchoSession 拥有 socket 和缓冲区。读到数据后排入单一写队列，写完再继续下一次写。

```cpp
acceptor.async_accept([&](auto error, tcp::socket socket) {
    if (!error)
        std::make_shared<Session>(std::move(socket))->start();
    acceptNext();
});
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `tcp::acceptor` | bind、listen 并 accept 连接 |
| `async_accept` | 异步取得新 socket |
| `async_read/async_write` | 按给定长度完成传输 |
| `read_some/write_some` | 允许只处理部分字节 |

## 怎么验证自己真的理解了

启动服务端和多个客户端，发送空消息、小消息和大消息。确认每个响应完整且连接断开后对象释放。

## 常见坑

- 异步 Session 常需要 shared_from_this 保活，但不能形成永久循环。
- 不要在事件循环线程做长时间 CPU 或磁盘工作。
- SIGPIPE、连接重置等平台差异要由库和错误码处理。

## 马上动手

为 Session 画状态：reading、writing、closing，并标出每个回调允许的转换。

## 学完标准

能实现持续 accept 的服务端，并正确处理部分读写和连接对象生命期。
