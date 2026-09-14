# 05 · 类、对象与 RAII

[返回学习地图](../README.md) · [上一篇：指针、生命周期与内存](04-指针生命周期与内存.md) · [下一篇：智能指针与所有权](06-智能指针与所有权.md)

## 先说结论

C++ 对象可以在构造时取得资源，在析构时自动释放资源。这个规则叫 RAII，它解决的是“无论函数正常返回还是抛出异常，文件、锁和内存都必须可靠清理”的问题。

读完本篇，你能读懂类的公开接口、构造函数、成员初始化列表、析构函数和 `const` 成员函数；也能解释为什么局部对象离开大括号时会立即清理。真实项目中的文件流、互斥锁和智能指针都建立在这套机制上。

## 问题：资源不能等垃圾回收来清理

Java 普通对象的回收时间由 GC 决定，所以文件等资源通常用 try-with-resources，锁通常用 `finally` 释放。C++ 没有把局部对象的清理交给 GC：对象的生命周期结束时，析构函数会按语言规则执行。

本篇先用 `Session` 做生命周期跟踪模型：它打印“打开—使用—关闭”的顺序，方便观察析构时机，但打印 `close` 本身并没有释放真实的操作系统资源。

```text
构造 Session → 资源可用 → 调用 greet → 离开作用域 → 析构 Session
```

真实 RAII 类型会在析构函数中执行真正的释放动作。把释放动作绑定到析构后，调用者不必记住每个 return 和异常路径上的清理代码。

## 方案一：让类维护自己的有效状态

关键代码来自完整的 [demos/05.cpp](../demos/05.cpp)：

```cpp
class Session {
public:
    explicit Session(const std::string& name) : name_(name) {
        std::cout << "open " << name_ << '\n';
    }
    ~Session() { std::cout << "close " << name_ << '\n'; }
    void greet() const { std::cout << "hello " << name_ << '\n'; }
private:
    std::string name_;
};
```

从使用者角度先读 `public`：它提供构造、析构和 `greet`。`private` 中的 `name_` 只能由类自己的代码直接访问，避免外部随意破坏状态。

`class` 和 `struct` 都能有字段、构造函数和方法。主要语法差别是：`class` 的成员默认 private，`struct` 的成员默认 public。业务对象常用 class 隐藏状态，简单数据记录常用 struct；这只是常见选择，不是强制规则。

## 方案二：构造时初始化，析构时清理

`Session(const std::string& name) : name_(name)` 中冒号后的部分是成员初始化列表。它直接用参数 `name` 初始化成员 `name_`。

成员真正的初始化顺序由它们在类中的声明顺序决定，与初始化列表的书写顺序无关。读多成员类时，应先看成员声明。

`~Session()` 是析构函数。局部 `Session` 离开作用域时会调用它。正常返回和异常导致的栈展开都会析构已经完整构造的局部对象；进程被强制终止时不能依赖这套清理。

这就是 RAII：

- 构造成功，代表资源已经处于可用状态；
- 对象活着，资源就由它负责；
- 对象析构，资源随之释放。

标准库已经为常见资源实现 RAII。例如 `std::ofstream` 析构时关闭文件，`std::lock_guard` 析构时解锁，`std::unique_ptr` 析构时释放其拥有的对象。

下面是一个真实资源的最小例子：

```cpp
void writeLog() {
    std::ofstream out{"session.log"};
    if (!out) throw std::runtime_error{"cannot open session.log"};
    out << "started\n";
} // out 析构，文件被关闭
```

`Session` demo 验证语言规定的析构顺序；`ofstream` 展示同一机制如何管理文件。析构能保证对象生命周期结束时调用清理逻辑，但磁盘写入仍可能失败，重要数据还要检查写入和关闭结果。

## 方案三：把接口意图写进类型

构造函数前的 `explicit` 阻止意外的隐式转换。保留它时，调用者需要明确写 `Session session{"C++"};`，而不会在需要 Session 的地方悄悄把一个字符串转换成 Session。

`greet() const` 末尾的 const 表示该方法不能通过 `this` 修改普通成员，因此可以在 const Session 对象上调用。

成员名尾部的下划线只是常见命名习惯，用来区分成员 `name_` 和参数 `name`，不是 C++ 语法。

## Java 对照

| Java 中的做法 | C++ 中的做法 | 关键差别 |
| --- | --- | --- |
| 构造器建立有效对象 | 构造函数建立有效对象 | C++ 常直接在成员初始化列表中初始化字段 |
| private 字段 + public 方法 | private 成员 + public 方法 | 封装目的相近 |
| try-with-resources | RAII 资源对象 | C++ 局部对象离开作用域时确定析构 |
| `finally` 中 unlock | `lock_guard` 析构自动 unlock | 清理与对象生命周期绑定 |
| finalizer/GC | 析构函数 | 析构用于确定清理，不能类比成不确定时间的 GC 回调 |

## 验证：观察构造和析构顺序

在 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/05.cpp -o build/demo_05
./build/demo_05
```

预期输出：

```text
before scope
open C++
hello C++
close C++
after scope
```

`close C++` 出现在 `after scope` 之前，证明 session 在离开内层大括号时已经析构，不需要等待 main 结束，也不需要手动调用 close。

## 常见坑

- 不要随意手动调用析构函数；这通常会破坏对象的正常生命周期。
- 不要在析构函数中让异常逃出。清理代码应尽量可靠。
- 初始化列表的书写顺序不会改变成员的实际初始化顺序。
- `const` 成员函数表示不能通过当前对象修改普通成员，不表示程序中不存在其他对象或线程修改相关数据。
- 自己的类优先组合 string、vector、文件流和智能指针，让标准类型完成资源管理。这样通常不需要手写析构、拷贝和移动操作，这叫 Rule of Zero。

## 三级练习

1. **预测**：在同一作用域先创建 Session A，再创建 Session B，离开作用域时谁先析构？
2. **修改**：给 Session 增加 `id_` 成员和只读 getter，并确认初始化顺序与成员声明一致。
3. **独立完成**：在函数中创建局部 `std::ofstream` 写入一行文字，不显式 close；函数返回后重新打开文件读取，指出 RAII 清理发生的位置。

## 完成标准

能从类声明中找到公开接口和内部状态；能解释构造、析构和 `greet() const` 的作用；能说明文件流为什么可以不手写 close。

## 马上动手

1. 运行上面的 demo，按输出标出“构造”“使用”“析构”三个时刻。
2. 在 [demos/05.cpp](../demos/05.cpp) 的内层作用域中依次创建 A、B 两个 Session。
3. 运行并确认析构顺序与构造顺序相反。
4. 恢复 demo 后执行 `bash scripts/run_all.sh`，确认全部检查通过。
