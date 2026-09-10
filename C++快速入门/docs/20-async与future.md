# 20 · async 与 future

[返回学习地图](../README.md) · [下一篇：原子变量](21-原子变量.md)

## 本篇目标

使用 `std::async` 启动有返回值的异步任务，通过 `std::future` 等待结果并接收异常。

## 学习作用

普通 thread 没有返回值通道，异常逃出线程入口还会终止进程。future 把“稍后得到一个值或异常”建模成对象，适合一次性后台计算，也为任务执行器提供结果接口。

## Java 对照

`std::future<T>` 接近 Java `Future<T>`：get 会等待并返回结果。Java 通常抛出包住原因的 `ExecutionException`，C++ 的 get 会直接重抛任务保存的原异常。`std::async(std::launch::async, ...)` 接近把 Callable 提交给异步执行环境，但标准库可能为每次调用创建线程，不是完整线程池。

C++ 调用 async 时若省略启动策略，标准允许任务异步执行，也允许推迟到 get/wait 时才执行。需要本例确定异步启动，就显式写 `std::launch::async`。

## future 的规则

- get 会阻塞直到结果就绪，返回值或重新抛出任务异常。
- 普通 future 的取值动作只能进行一次；无论 get 返回值还是重抛任务异常，之后都不能再次 get。
- wait 只等待，不会取值，也不会把任务异常抛给调用者。
- 来自 `launch::async` 的 future 在某些析构场景会等待任务结束，不适合被当作 fire-and-forget。
- `std::promise` 可由生产者手工设置值/异常；`std::packaged_task` 把可调用对象与 future 状态连接起来，第 22 篇会实际使用。
- C++17 future 没有 Java Future 那样的通用 cancel；停止必须由任务与所有者另行设计协作协议。

## 可运行 demo

源码：[demos/20.cpp](../demos/20.cpp)：

```bash
c++ -std=c++17 -Wall -Wextra -Wpedantic -g -pthread \
  demos/20.cpp -o build/demo_20
./build/demo_20
```

```cpp
#include <future>
#include <iostream>
#include <stdexcept>

int divide(int left, int right) {
    if (right == 0) throw std::invalid_argument("division by zero");
    return left / right;
}

int main() {
    auto answer = std::async(std::launch::async, divide, 84, 2);
    auto failure = std::async(std::launch::async, divide, 10, 0);

    std::cout << "answer: " << answer.get() << '\n';
    try {
        std::cout << failure.get() << '\n';
    } catch (const std::exception& error) {
        std::cout << "error: " << error.what() << '\n';
    }
}
```

预期输出：

```text
answer: 42
error: division by zero
```

两个任务何时结束并不确定，但 main 固定先 get answer、再 get failure，所以输出稳定。第二个任务中的 invalid_argument 被 future 保存，到主线程调用 get 时才在主线程重新抛出。

## 常见坑

- 不保存 async 返回的 future，临时 future 的析构行为可能让代码意外同步。
- 不要在持有某把 mutex 时 get 一个需要该 mutex 的任务结果。
- 多次 get 同一个 future 会触发 future_error；多个观察者可研究 shared_future。
- async 适合简单独立任务；大量细小任务通常应交给受控执行器或项目已有线程池。

## 三级练习与完成标准

1. 预测：先 get failure 再 get answer 时输出顺序怎样变化？任务实际完成顺序能否由此确定？
2. 修改：再提交一个返回 string 的任务，main 按固定顺序获取三个结果。
3. 独立：写一个 `asyncAverage(vector<int>)`，空输入抛 invalid_argument；分别验证正常结果和异常路径。

完成标准：知道何时必须指定 launch::async，能只调用一次 get，并能让异步异常在明确的调用边界被处理。
