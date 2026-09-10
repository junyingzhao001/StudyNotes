# 03 · 函数引用与const

[返回学习地图](../README.md) · [下一篇：指针生命周期与内存](04-指针生命周期与内存.md)

## 本篇目标

区分值传递、引用传递和 const 引用，能为函数选择参数形式。

## 学习作用

判断函数是否修改调用者的数据，理解 C++ API 中频繁出现的 &。

## 核心知识

值传递 `int x` 创建参数副本；引用 `int& x` 是已有对象的别名，可以修改原对象；`const std::string& text` 借用字符串读取，通常避免一次字符串复制。

这里的 `&` 是类型声明中的引用符号。引用必须初始化，绑定后不能改为另一个对象。引用的有效性依赖被引用对象仍然存活：它并不会延长任意对象的生命。

严格来说，`const T& local = T{};` 这类直接用临时对象初始化局部 const 引用的特定写法，会把该临时对象延长到 local 的作用域；但函数参数中的 const 引用只保证临时对象活到这次完整调用表达式结束，函数不能保存这条引用或指针借用供以后使用（复制出自己的值则是另一回事）。入门阶段优先使用命名对象，仍按“借用不能比目标活得久”检查接口。

参数选择的起点：int、bool 等小类型常按值传递；只读大对象常用 `const T&`；明确要修改调用者对象时用 `T&`。需要可选的借用对象时，后面会用指针表达空值。

`const T&` 只表示不能通过这个引用修改对象，不表示对象绝对不会被其他代码修改。不要返回局部变量的引用：函数返回时局部变量已经销毁。返回普通值通常是清晰且高效的默认选择。

## Java 对照：这里最容易误会

Java 所有参数都是按值传递。传入 int 时复制整数；传入对象时复制“指向对象的引用值”，所以方法能修改对象内容，却不能让调用者的变量改指向另一个对象。

C++ 把选择直接写进函数签名：

| C++ 参数 | 调用含义 | Java 中较接近的感觉 |
| --- | --- | --- |
| `Task task` | 复制一个 Task，函数改副本 | 显式复制对象后传入 |
| `const Task& task` | 借用原 Task，只读 | 传对象引用并约定不修改，但 C++ 由类型检查 |
| `Task& task` | 借用原 Task，可以修改 | 方法修改传入对象，但它还是更直接的别名语义 |
| `Task* task` | 可空借用，通过 `->` 访问 | 可为 null 的对象引用，但没有 GC 保活 |

不能笼统说“Java 是引用传递”。准确说法是：Java 复制参数值，对象变量的值恰好是一个引用；C++ 另有真正的引用参数 `T&`。

## 可运行 demo

源码：[demos/03.cpp](../demos/03.cpp)。从 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/03.cpp -o build/demo_03
./build/demo_03
```

```cpp
#include <iostream>
#include <string>

int addCopy(int value) { return ++value; } // 返回修改后的副本
void addReference(int& value) { ++value; }
std::size_t length(const std::string& text) { return text.size(); }

int main() {
    int count{10};
    const int copiedResult = addCopy(count);
    std::cout << "after copy: " << count << '\n';
    std::cout << "returned copy: " << copiedResult << '\n';
    addReference(count);
    std::cout << "after reference: " << count << '\n';
    const std::string greeting{"hello"};
    std::cout << "length: " << length(greeting) << '\n';
}
```

预期输出：

```text
after copy: 10
returned copy: 11
after reference: 11
length: 5
```

## 跟踪变量变化

| 时刻 | count | copiedResult | 原因 |
| --- | ---: | ---: | --- |
| 初始化后 | 10 | 尚未创建 | count 自己拥有整数值 |
| `addCopy(count)` 后 | 10 | 11 | 函数增加的是参数副本，并返回它 |
| `addReference(count)` 后 | 11 | 11 | value 是 count 的别名，修改落到原变量 |

length 使用 `const std::string&` 只读借用 greeting。函数返回前 greeting 始终存活，所以借用有效。

## 常见坑

`const auto&` 保留只读引用；普通 `auto` 经常得到副本。例如 `auto x = count;` 改 x 不会改 count。

## 动手练习

写一个 swapValues(int& a, int& b)，交换两个变量；再改成值参数观察差别。

## 完成标准

只看函数声明，就能初步判断它复制、读取还是修改参数。
