# 36 · Asio 执行模型

[返回学习地图](../README.md) · [上一篇](35-网络基础.md) · [下一篇](37-TCP客户端与服务端.md)

## 先说这篇讲什么

这篇讲 Asio 的 io_context、socket、resolver 和异步回调怎样合作。Asio 是第三方跨平台网络库。

## 它有什么用

标准 C++20 没有通用 socket API。Asio 把 Windows、Linux 和 macOS 的网络事件封装成较一致的接口。

## 要解决的问题

发起 async 操作不代表回调立刻执行；必须有线程运行 io_context，相关对象也要活到回调结束。

## 先把几个词说清楚

- **io_context**：事件循环，分派已经完成的异步操作
- **handler**：操作完成后由 Asio 调用的函数
- **buffer**：描述一段已有内存，不拥有数据

## 一个案例

先用同步代码看清对象关系：resolver 产生地址列表，socket 连接其中一个地址。改成 `async_resolve`、`async_connect` 后，步骤不变，但完成结果进入回调，而且 main 必须调用 `io.run()` 才会分派回调。

```cpp
asio::io_context io;
tcp::resolver resolver{io};
auto endpoints = resolver.resolve("example.com", "80");
tcp::socket socket{io};
asio::connect(socket, endpoints);
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `asio::io_context` | 保存并运行异步事件 |
| `tcp::resolver` | 把主机名和服务解析为 endpoint |
| `tcp::socket` | TCP 连接端点 |
| `async_*` | 发起操作并立即返回，完成后调用 handler |

## 怎么验证自己真的理解了

先运行上面的同步 resolve/connect 确认环境。再改成 async 版本：回调里检查 error_code，main 调用 `io.run()`；故意省略 run 时，确认回调不会执行。

## 常见坑

- asio::buffer 不复制数据，异步操作期间底层内存必须有效。
- 回调里要检查 error_code，不要只看 bytes。
- 一个 io_context 可以由多个线程运行，但共享对象仍需同步规则。

## 马上动手

画出 resolve、connect、io.run 和回调的先后关系，并标出每个对象的生命周期。

## 学完标准

能解释 io_context 为什么必须运行，以及异步操作期间 socket、handler 和 buffer 谁要存活。
