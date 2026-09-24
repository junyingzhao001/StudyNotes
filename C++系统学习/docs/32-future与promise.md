# 32 · future、promise 与一次性异步结果

[返回学习地图](../README.md) · [上一篇](31-条件变量与任务队列.md) · [下一篇](33-atomic与内存模型.md)

## 先说这篇讲什么

这篇讲在线程之间传递返回值或异常，并区分启动任务、等待结果和超时观察。

## 它有什么用

普通 thread 没有直接返回值通道。future 让调用者稍后取得一个结果，任务异常也能在 get 时重新抛出。

## 要解决的问题

wait_for 超时只表示结果此刻未就绪，不会自动取消任务；get 取走结果后不能再次调用。

## 先把几个词说清楚

- **future**：稍后取得一次结果的接收端
- **promise**：由生产者设置值或异常的发送端
- **共享状态**：连接发送端和接收端的中间对象

## 一个案例

异步计算报表，main 同时处理界面；等待 100ms 未完成就显示“仍在计算”，最终仍调用 get。

```cpp
auto result = std::async(std::launch::async, compute);
if (result.wait_for(100ms) == std::future_status::timeout)
    std::cout << "still working\n";
std::cout << result.get() << '\n';
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `std::async` | 启动或延迟执行一次性任务 |
| `future::get` | 等待并取得结果或异常 |
| `wait_for` | 等待一段时间，只观察是否就绪 |
| `packaged_task` | 把可调用对象包装成带 future 的任务 |

## 怎么验证自己真的理解了

测试正常返回和任务抛异常。get 后检查 valid 为 false，不要再次 get。

## 常见坑

- 省略 async 启动策略时任务可能延迟到 get 执行。
- 丢弃临时 future 可能让代码意外同步。
- 多个观察者使用 shared_future，不要多次 get 普通 future。

## 马上动手

用 promise 在线程中返回一个值和一个异常，main 分别处理。

## 学完标准

能用 future 接收值和异常，并解释超时等待为何不等于取消。
