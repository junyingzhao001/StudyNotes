# 08 · 算法Lambda与迭代器

[返回学习地图](../README.md) · [下一篇：拷贝移动与返回值](09-拷贝移动与返回值.md)

## 先说结论

这一篇解决一个很常见的问题：从成绩列表中删除低分、排序，再找到第一个高分。读完后，你能用 `remove_if`、`sort`、`find_if` 和 Lambda 表达这些动作，也能判断迭代器何时有效。

这些能力有用，是因为真实 C++ 项目大量使用标准算法。看到算法名就能知道代码意图，通常比逐行拆一个手写循环更快。

## 问题：一个循环承担太多职责

假设输入是 `90, 40, 70, 60`，要求：

1. 删除低于 60 的成绩；
2. 按从高到低排序；
3. 找到第一个至少 80 的成绩。

可以手写多个循环，但删除、排序、查找的边界容易混在一起。标准算法把“怎么遍历”封装好，调用者只描述条件。

Java 中可能使用 `removeIf`、`sort` 或 Stream；C++17 使用算法加迭代器区间：

| 目标 | Java 中常见写法 | C++17 |
| --- | --- | --- |
| 按条件删除 | `list.removeIf(predicate)` | `remove_if` 后再 `vector::erase` |
| 排序 | `list.sort(comparator)` | `sort(begin, end, comparator)` |
| 查找首项 | Stream/filter 或循环 | `find_if(begin, end, predicate)` |

## 方案：算法负责遍历，Lambda 负责规则

算法通常接收半开区间 `[begin, end)`：包含 `begin` 指向的元素，不包含 `end`。`end()` 是尾后位置，不能解引用。

[完整 demo](../demos/08.cpp) 的关键处理是：

```cpp
const auto newEnd = std::remove_if(scores.begin(), scores.end(),
    [threshold](int score) { return score < threshold; });
scores.erase(newEnd, scores.end());

std::sort(scores.begin(), scores.end(),
    [](int left, int right) { return left > right; });

const auto it = std::find_if(scores.begin(), scores.end(),
    [](int score) { return score >= 80; });
```

Lambda 的形式是 `[捕获](参数) { 函数体 }`：

- `[threshold]` 把当前阈值复制进 Lambda；
- `[&threshold]` 借用外部阈值，之后读取的是同一个对象；
- `[]` 不使用外部局部变量。

Java Lambda 捕获的局部变量必须是 effectively final；C++ 需要在 `[]` 中明确选择复制还是引用。引用捕获不会延长对象生命，回调晚于局部变量销毁时会悬空。

## 为什么 remove_if 后还要 erase

`remove_if` 只把要保留的元素移动到容器前部，并返回“逻辑新终点”。它不能改变传入容器的大小，因为算法只拿到了迭代器，没有拿到 vector 本身。

| 阶段 | 可依赖的内容 | `scores.size()` |
| --- | --- | --- |
| 初始 | `90, 40, 70, 60` | 4 |
| `remove_if` 后的 `[begin,newEnd)` | `90, 70, 60` | 4 |
| `erase(newEnd,end)` 后 | `90, 70, 60` | 3 |
| 降序 `sort` 后 | `90, 70, 60` | 3 |

Java 的 `List.removeIf` 会直接缩短集合，这是两者最容易混淆的地方。

## 比较器为什么不能写小于等于

排序比较器 `compare(a, b)` 表示“a 是否应该排在 b 前面”。当两个值相等时必须返回 `false`，所以降序写 `a > b`，不要写 `a >= b`。标准库要求比较规则形成严格弱序；不满足要求时，排序结果没有可靠含义。

## 验证：运行并核对结果

在 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/08.cpp -o build/demo_08
./build/demo_08
```

预期输出：

```text
90 70 60 
first >= 80: 90
```

输出证明三件事：40 被删除，剩余成绩已经降序，`find_if` 返回了第一个满足条件的元素。代码必须先检查 `it != scores.end()`，再使用 `*it`。

## 局限与常见坑

- `remove_if` 后、`erase` 前，不要依赖 `newEnd` 后面的值。
- `erase` 会使删除位置及其后的迭代器、引用和指针失效；之后重新获取。
- 不要解引用 `find_if` 返回的 `end()`。
- `[&]` 很短，但隐藏了借用对象；异步保存 Lambda 时尤其容易悬空。
- 原地算法会修改输入。需要保留原数据时，先复制或生成新容器。

## 三级练习

1. 预测：把阈值改为 75，最终保留哪些成绩？先写答案再运行。
2. 修改：改成升序；查找至少 100 的成绩，找不到时输出 `not found`。
3. 独立：先用普通循环生成一个过滤后的新 vector，再用 erase-remove 原地过滤；分别写出输入是否改变和预期输出。

## 完成标准

能解释 `[begin,end)`、复制捕获和引用捕获；能正确写出 erase-remove；能在解引用前判断查找结果，并知道哪些操作会让迭代器失效。

## 马上动手

打开 [demos/08.cpp](../demos/08.cpp)，先把 `threshold` 改为 75 并写下预期输出，再编译运行。预测与实际不同，就按“过滤 → erase → 排序 → 查找”四个阶段打印容器定位差异。
