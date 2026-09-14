# 20 · async 与 future

[返回学习地图](../README.md) · [下一篇：原子变量](21-原子变量.md)

## 先说结论

**真实问题：** 普通 `std::thread` 没有直接的返回值通道，线程函数中的异常也不能穿过线程边界回到调用者。

**读完你能做什么：** 用 `std::async` 启动一次性异步计算，用 `std::future` 等待值或接收异常，并判断什么时候不该使用 async。

**为什么有用：** future 把“稍后得到一个值，或者得到一个失败”变成明确接口。第 22 篇的后台任务执行器也会用同一种结果模型。

## 问题：join 只等结束，不带回结果

使用普通 `std::thread` 时，结果通常要写入外部共享变量，错误也要另建通道。这会把返回值、生命周期和同步揉在一起。

可以把 future 想成取件凭证：异步任务负责把“值或异常”放入对应格口，调用者拿着凭证，在需要时通过 `get()` 领取。任务是否已经完成和调用者何时领取可以分开。

## 方案：用 async 创建带结果的任务

```cpp
auto answer = std::async(
    std::launch::async,
    divide,
    84,
    2
);

std::cout << answer.get();
```

显式指定 `std::launch::async`，表示请求异步执行。若省略策略，标准允许实现选择异步执行，也允许把任务推迟到第一次 `get()/wait()` 时才在等待线程中执行。

`std::async` 适合少量、相互独立的一次性任务。它不是线程池接口，标准也没有承诺多个 async 调用会复用固定数量的工作线程。

## future 同时传值和传异常

`get()` 会等待共享状态就绪。任务正常返回时，它给出结果；任务抛异常时，它在调用 `get()` 的线程中重新抛出原异常：

```cpp
auto failure = std::async(
    std::launch::async,
    divide,
    10,
    0
);

try {
    std::cout << failure.get();
} catch (const std::invalid_argument& error) {
    std::cout << error.what();
}
```

这让异常有明确的处理边界。任务线程不会因为这个业务异常直接触发 `std::terminate`。

## Java 对照：Future 很像，但异常包装不同

| Java | C++17 | 需要记住的差别 |
| --- | --- | --- |
| `Future<T>` | `std::future<T>` | 都可等待并取得异步结果 |
| `Future.get()` 抛 `ExecutionException` | `future.get()` 重抛原任务异常 | C++ 通常直接捕获任务原本抛出的异常类型 |
| `ExecutorService.submit(Callable)` | 本篇 `async`；第 22 篇 `TaskExecutor::submit` | async 不是完整执行器或线程池 |
| `Future.cancel` | 没有通用对应接口 | C++17 任务停止要另行设计协作协议 |
| 多处等待常借助 `CompletableFuture` 等 | `std::shared_future` | 普通 future 的结果只能 get 一次 |

## future 的五条使用规则

1. `get()` 可能阻塞，直到值或异常准备好。
2. 普通 `future` 只能成功调用一次 `get()`；之后它不再关联共享状态，可用 `valid()` 检查。
3. `wait()` 只等待，不取值，也不会把任务异常抛给调用者。
4. `future` 可移动、不可复制；多个观察者需要 `shared_future`。
5. C++17 future 本身没有通用取消能力。

不要在持有 mutex 时调用一个可能等待同一把锁的 future：

```cpp
std::lock_guard<std::mutex> lock(mutex);
result.get();  // 如果任务也需要 mutex，就会互相等待
```

## 一个容易忽略的行为：async future 析构可能等待

来自 `std::launch::async` 的 future 在某些析构场景会等待任务结束。因此下面写法可能在每个分号处就等待，使两次调用表现成串行：

```cpp
std::async(std::launch::async, firstTask);
std::async(std::launch::async, secondTask);
```

应保存 future，并在清楚的边界统一等待或取值。不要把丢弃 async 返回值当作 fire-and-forget。

## promise 和 packaged_task 放在哪里

三者共享“值或异常稍后就绪”的思想，但生产方式不同：

- `std::async`：把启动任务和创建 future 合在一起。
- `std::promise<T>`：生产者手工调用 `set_value` 或 `set_exception`。
- `std::packaged_task<Result()>`：把一个可调用对象包装成能写入 future 的任务。

第 22 篇会把 packaged_task 放进队列，由长期存在的工作线程执行；测试则会用 promise 作为可控的线程间开关。

## 验证：值和异常都走同一接口

完整源码：[demos/20.cpp](../demos/20.cpp)

```bash
c++ -std=c++17 -Wall -Wextra -Wpedantic -g -pthread \
  demos/20.cpp -o build/demo_20
./build/demo_20
```

预期输出：

```text
answer: 42
error: division by zero
```

两个任务的实际完成顺序不确定，但 main 固定先 `get()` answer，再 `get()` failure，因此输出顺序确定。输出顺序不能反推任务完成顺序。

## 局限与常见坑

- 省略启动策略时，任务可能延迟执行；需要确定并发就显式指定 `launch::async`。
- 不保存 async 返回的 future，临时 future 析构可能让代码意外同步。
- `get()` 会取走结果并让这个普通 future 失去共享状态，之后不要再次调用；调用前可用 `valid()` 检查。多个观察者需要 `shared_future`。
- 只调用 `wait()` 不会观察任务异常；最终仍应 `get()`。
- 大量细小任务不应盲目使用 async；线程数量和排队策略需要受控执行器。
- future 不会自动让引用捕获安全；任务可能在被引用对象销毁后才访问它。
- future 没有通用取消、优先级或超时停止任务的能力；`wait_for` 超时只表示此刻结果未就绪。

## 三级练习与完成标准

1. **预测：** 先 get failure 再 get answer 时输出如何变化？能否据此判断两个任务实际谁先结束？
2. **修改：** 再提交一个返回 `std::string` 的任务，由 main 按固定顺序取得三个结果。
3. **独立：** 编写 `asyncAverage(vector<int>)`。空输入抛 `invalid_argument`，分别验证正常值和异常路径。

完成标准：知道何时应指定 `launch::async`，能只调用一次 `get()`，能在清楚的调用边界处理异步异常，并能解释为什么 async 不是线程池。

## 马上动手

先预测 [demos/20.cpp](../demos/20.cpp) 的输出，再运行验证。随后交换两个 `get()` 的顺序，观察输出并解释“领取顺序”和“完成顺序”的区别；完成后进入第 21 篇。
