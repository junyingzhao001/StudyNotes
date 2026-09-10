# 08 · 算法Lambda与迭代器

[返回学习地图](../README.md) · [下一篇：拷贝移动与返回值](09-拷贝移动与返回值.md)

## 本篇目标

掌握 sort、find_if、remove_if 和 Lambda 捕获。

## 学习作用

读懂现代 C++ 集合处理代码，减少重复循环。

## 核心知识

算法通常接受半开区间 `[begin, end)`：包含起点，不包含终点。`sort` 原地排序；`find_if` 返回第一个符合条件的迭代器，找不到返回 end。

Lambda 形式是 `[捕获](参数) { 函数体 }`。`[threshold]` 复制外部变量；`[&threshold]` 引用外部变量；`[]` 不捕获。引用捕获不会延长对象生命，回调若稍后执行，被引用变量必须仍有效。

`remove_if` 将要保留的元素挪到前部并返回逻辑新终点，它本身不缩短 vector。配合 `erase(newEnd, end)` 才真正删除尾部，这是 C++17 常见 erase-remove 写法。

排序比较器必须满足严格弱序；比较数值大小通常使用 `<` 或 `>`，不要用 `<=`。复杂类型排序时可只比较业务字段。

## Java Stream / 集合操作对照

Java 的 `list.removeIf(predicate)` 会真正缩短集合；C++17 的 `std::remove_if` 只把保留元素移到前部并返回逻辑终点，还要调用 vector.erase 才改变 size。

Java Stream 的 filter/sorted 常构成新流水线并最终 collect；这里的 C++ 算法直接操作 `[begin, end)` 迭代器区间，sort 也会原地改变 vector。两边都使用 Lambda，但捕获规则不同：Java 捕获局部变量要求它 effectively final；C++ 用 `[threshold]` 明确复制捕获，用 `[&threshold]` 明确引用捕获。

比较器先记一条可执行规则：`compare(a, b)` 表示“a 应排在 b 前面”。相等元素必须返回 false，所以不要写 `a <= b`。这套规则在标准术语中叫严格弱序。

## 可运行 demo

源码：[demos/08.cpp](../demos/08.cpp)。从 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/08.cpp -o build/demo_08
./build/demo_08
```

```cpp
#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> scores{90, 40, 70, 60};
    int threshold{60}; // 可配置阈值，由 Lambda 复制捕获
    const auto newEnd = std::remove_if(scores.begin(), scores.end(),
        [threshold](int score) { return score < threshold; });
    scores.erase(newEnd, scores.end());
    std::sort(scores.begin(), scores.end(),
        [](int left, int right) { return left > right; });
    for (int score : scores) std::cout << score << ' ';
    std::cout << '\n';
    const auto it = std::find_if(scores.begin(), scores.end(),
        [](int score) { return score >= 80; });
    if (it != scores.end()) std::cout << "first >= 80: " << *it << '\n';
}
```

预期输出：

```text
90 70 60 
first >= 80: 90
```

## 把 erase-remove 拆开看

| 时刻 | 有效内容 | 说明 |
| --- | --- | --- |
| 初始 | 90, 40, 70, 60 | size 为 4 |
| remove_if 后 `[begin,newEnd)` | 90, 70, 60 | 保留元素已移到前部；newEnd 后面的值不应依赖，vector size 仍为 4 |
| erase 后 | 90, 70, 60 | 尾部真正删除，size 为 3 |
| sort 后 | 90, 70, 60 | 比较器使用 `>`，所以降序 |

find_if 找到第一个至少 80 的值并返回它的位置。必须先比较 `it != scores.end()`，确认找到后才能 `*it`。

## 常见坑

remove_if 只移动/赋值元素，不改变 vector 的容量和 size；但一些位置现在保存了不同值。随后 erase 会使删除点及其后的迭代器、引用和指针失效，包括 newEnd 与旧 end，之后应重新获取。不要解引用 find_if 返回的 end。

## 动手练习

1. 预测：阈值改为 75 后剩哪些元素？
2. 修改：改成升序，并在找不到至少 100 的元素时输出 `not found`。
3. 独立：先用普通 for 循环生成一个过滤后的新 vector，再用 remove_if + erase 原地过滤；比较两种写法的输入是否改变，并分别写预期输出。

## 完成标准

能读懂捕获列表和算法区间，写出过滤再排序的代码。
