# 05 · 类对象与RAII

[返回学习地图](../README.md) · [下一篇：智能指针与所有权](06-智能指针与所有权.md)

## 本篇目标

理解构造函数、析构函数、成员初始化列表、访问控制和 const 成员函数。

## 学习作用

读懂业务类，掌握 C++ 自动管理文件、锁和内存的核心思路。

## 核心知识

`class` 默认成员私有，`struct` 默认成员公开，二者都能有构造函数和方法。`public` 暴露接口，`private` 隐藏内部状态。

构造函数创建有效对象；冒号后的成员初始化列表直接初始化成员。成员的实际初始化顺序由类中声明顺序决定，而不是初始化列表书写顺序。`explicit` 防止构造函数被用于不期望的隐式转换。

析构函数 `~Type()` 在对象生命周期结束时执行。RAII（资源获取即初始化）就是把资源所有权放进对象：构造时取得资源，析构时释放资源。局部对象正常离开作用域或发生异常栈展开时会析构；强制终止进程不能依赖这套清理机制。

本 demo 用日志显示生命周期；真正的资源管理例子有 `std::ofstream` 自动关闭文件、`std::unique_ptr` 自动释放对象。方法尾部的 `const` 表示不能通过 this 修改普通成员，因而可以在 const 对象上调用该方法。

成员名 `name_` 尾部下划线只是常见命名习惯，用来区分成员和参数，不是 C++ 语法。`explicit` 适合只有一个主要参数的构造函数，可阻止编译器把字符串等值在你没明确要求时自动转成 Session。

例如去掉 explicit 后，`Session session = std::string{"C++"};` 可以隐式转换；保留 explicit 时必须明确写 `Session session{std::string{"C++"}};`。实际项目中，除非这种自动转换本身就是清楚且有意的接口，否则单参数构造函数通常考虑 explicit。

## Java 对照：RAII 和 try-with-resources

Java 普通对象何时被 GC 回收并不确定，finalize 也不应作为资源管理方式。Java 文件或锁常用 try-with-resources/finally；C++ 把同样的清理职责放进对象析构函数，局部资源对象离开作用域时确定清理。

```java
try (BufferedReader reader = Files.newBufferedReader(path)) {
    // 使用 reader
}
```

```cpp
{
    std::ifstream input(path);
    // 使用 input
} // input 在这里析构并关闭文件
```

RAII 不只用于内存，还用于文件、互斥锁、套接字和图形资源。关键是“资源的有效期绑定到一个对象”。

## 可运行 demo

源码：[demos/05.cpp](../demos/05.cpp)。从 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/05.cpp -o build/demo_05
./build/demo_05
```

```cpp
#include <iostream>
#include <string>

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

int main() {
    std::cout << "before scope" << '\n';
    {
        const Session session{"C++"};
        session.greet();
    }
    std::cout << "after scope" << '\n';
}
```

预期输出：

```text
before scope
open C++
hello C++
close C++
after scope
```

## 生命周期时间线

```text
输出 before scope
进入内层作用域 → 构造 session → 输出 open
调用 greet → 输出 hello
离开内层作用域 → 析构 session → 输出 close
输出 after scope
```

这里不需要手动调用 close。哪怕内层后续代码通过异常离开，只要 session 已完整构造且发生正常栈展开，它仍会析构。

## 常见坑

不要随意手动调用析构函数。入门优先让 string、vector、智能指针管理资源，这样自己的类通常不用手写析构、拷贝和移动操作（Rule of Zero）。

## 动手练习

1. 预测：在同一作用域依次创建 A、B，离开时谁先析构？
2. 修改：给 Session 增加 id 成员和只读 getter。
3. 独立：写一个函数在局部创建 std::ofstream 并写一行文字，不显式 close；让函数返回、确认流已离开作用域后，再重新打开并读取，说明 RAII 在哪一步发生。

## 完成标准

能解释为什么离开大括号就触发 close，以及文件流为何能自动关闭。
