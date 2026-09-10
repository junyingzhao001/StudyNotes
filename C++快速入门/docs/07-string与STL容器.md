# 07 · string 与 STL 容器

[返回学习地图](../README.md) · [下一篇：算法Lambda与迭代器](08-算法Lambda与迭代器.md)

## 本篇目标

会用 string、vector、map，能够从实际需求选择常见容器；理解 C++ 容器的值语义、访问方式和借用失效。

## 学习作用

日常 C++ 开发的大部分数据不需要自己申请数组或管理字符串内存。先把 vector 当默认序列、把 map/unordered_map 当键值表，已经能解决大量业务问题。

## 先用 Java 建立对应关系

| Java | C++17 常用对应 | 最需要注意的差别 |
| --- | --- | --- |
| `String` | `std::string` | C++ string 可修改，普通赋值通常复制字符内容 |
| `int[]` | `std::array<int, N>` | N 是 C++ array 类型的一部分；长度固定 |
| `ArrayList<T>` | `std::vector<T>` | 连续存储；扩容可能使元素的指针/引用失效 |
| `ArrayDeque<T>` | `std::deque<T>` | 适合两端增删；内存不保证整体连续 |
| `LinkedList<T>` | `std::list<T>` | C++ 中通常不作为默认序列，只在明确需要稳定迭代器/中间插入时考虑 |
| `HashMap<K,V>` | `std::unordered_map<K,V>` | 平均查找快，不保证遍历顺序 |
| `TreeMap<K,V>` | `std::map<K,V>` | 按键排序，常见查找复杂度 O(log n) |
| `HashSet<T>` | `std::unordered_set<T>` | 只保存键，不保证顺序 |
| `TreeSet<T>` | `std::set<T>` | 按值排序 |
| `Optional<T>` | `std::optional<T>` | optional 直接包含一个 T 值或为空，不依赖 GC |

如果没有特殊需求，序列先选 vector；需要键值且不关心顺序先考虑 unordered_map；需要按键有序输出时选 map。不要因为容器名字多就每种都用一遍。

## 最大区别：元素通常按值存储

Java 的 `ArrayList<Task>` 对对象类型通常存 Task 引用。C++ 的 `std::vector<Task>` 直接拥有一组 Task 对象：

```cpp
std::vector<Task> tasks;
tasks.push_back(Task{1, "learn", false});
```

容器销毁时其中的 Task 也自动销毁。复制整个 vector 通常也会复制所有 Task：

```cpp
auto copied = tasks;  // 独立容器和独立元素
auto& alias = tasks;  // 同一个容器的别名
```

若要保存多态对象或独占动态对象，才会看到 `std::vector<std::unique_ptr<Base>>`。先理解 `vector<T>` 的直接值语义，再学习这种组合。

## vector：最常用的序列

```cpp
std::vector<std::string> names{"Ada", "Bjarne"};
names.push_back("Grace");
std::cout << names.at(0);  // 检查越界
```

| 操作 | C++ | Java ArrayList |
| --- | --- | --- |
| 追加 | `values.push_back(x)` | `values.add(x)` |
| 元素数 | `values.size()` | `values.size()` |
| 安全按下标访问 | `values.at(i)` | `values.get(i)` |
| 快速但不检查的访问 | `values[i]` | 没有直接对应；get 会检查 |
| 是否为空 | `values.empty()` | `values.isEmpty()` |
| 清空 | `values.clear()` | `values.clear()` |
| 预留容量 | `values.reserve(n)` | `values.ensureCapacity(n)` |

`size()` 是当前元素数，`capacity()` 是当前已分配空间能容纳多少元素。reserve 增加容量但不创建元素；resize 改变 size，并会创建或销毁元素。`vector<int>{3, 7}` 是两个元素 3、7，而 `vector<int>(3, 7)` 是三个值为 7 的元素。

## map：按键保存值

```cpp
std::map<std::string, int> counts;
++counts["cpp"];
```

`counts["cpp"]` 在键不存在时插入一个值初始化的 int，也就是 0，然后再加一。这个写法适合统计；如果只想查询，operator[] 会意外改变 map，应改用 find：

```cpp
const auto it = counts.find("python");
if (it != counts.end()) {
    std::cout << it->second;
}
```

map 的每个元素近似一对 key/value。`it->first` 是键，`it->second` 是值。C++17 结构化绑定可以把这对值拆成有意义的名字：

```cpp
for (const auto& [word, count] : counts) {
    std::cout << word << count;
}
```

这里 const auto& 表示只读借用每个键值对，避免循环时复制。map 的键本身不能通过迭代器修改，否则会破坏排序结构。

## 和 Java 写同一个词频统计

Java：

```java
List<String> words = new ArrayList<>(List.of("cpp", "java", "cpp"));
words.add("rust");
Map<String, Integer> counts = new TreeMap<>();
for (String word : words) {
    counts.merge(word, 1, Integer::sum);
}
```

C++：

```cpp
std::vector<std::string> words{"cpp", "java", "cpp"};
words.push_back("rust");
std::map<std::string, int> counts;
for (const auto& word : words) {
    ++counts[word];
}
```

两段代码做同一件事。Java 需要 Integer 装箱并用 merge 处理缺省值；C++ map 的 int 值初始化为 0。C++ 循环明确写 const 引用，是为了只读借用 string 而不逐个复制。

## 借用和迭代器何时失效

迭代器可以先理解为“指向容器中某个位置的可移动访问器”。`begin()` 是第一个元素，`end()` 是末尾之后的哨兵，不能解引用。

vector 扩容会搬到一块更大的连续内存，之前指向元素的指针、引用和迭代器会失效。erase 也会使被删除位置及其后的相关借用失效。Java 集合修改时常通过 ConcurrentModificationException 暴露问题；C++ 的失效借用可能没有异常，继续使用会产生未定义行为。

常见规则可先查这张表：

| 操作 | 既有迭代器/引用/指针 |
| --- | --- |
| vector 插入并发生扩容 | 指向元素的全部借用和迭代器失效 |
| vector 插入但未扩容 | 插入点之前保持有效；插入点及之后（包括旧 end）失效 |
| vector erase | 删除点及之后失效 |
| map 插入 | 既有迭代器和元素借用保持有效 |
| map erase | 只有被删除元素的迭代器和借用失效 |
| unordered_map rehash | 迭代器失效；未删除元素的引用/指针通常仍有效 |
| unordered_map erase | 被删除元素的迭代器和借用失效 |

这是常用操作的入门规则，其他容器与操作应查对应文档。Java 的 fail-fast 异常只是尽力发现部分并发修改，也不能作为逻辑保证；C++ 更不会自动用异常保护失效迭代器。

不要一次死背全部规则。实际代码在“保存了迭代器/引用之后又修改容器”时，应查询该容器具体操作的失效规则，并尽量缩短借用范围。

## 可运行 demo

源码：[demos/07.cpp](../demos/07.cpp)。从 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/07.cpp -o build/demo_07
./build/demo_07
```

```cpp
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> words{"cpp", "java", "cpp"};
    words.push_back("rust");
    std::map<std::string, int> counts;
    for (const auto& word : words) ++counts[word];
    for (const auto& [word, count] : counts) {
        std::cout << word << ": " << count << '\n';
    }
    const auto it = counts.find("python");
    std::cout << "python found: " << (it != counts.end()) << '\n';
    std::cout << "first: " << words.at(0) << '\n';
}
```

预期输出：

```text
cpp: 2
java: 1
rust: 1
python found: 0
first: cpp
```

## 跟踪容器状态

| 时刻 | words | counts |
| --- | --- | --- |
| 初始化后 | cpp, java, cpp | 空 |
| push_back 后 | cpp, java, cpp, rust | 空 |
| 统计完成 | 不变 | cpp→2, java→1, rust→1 |
| find python 后 | 不变 | 不变，find 不会插入 |

map 按键排序，所以输出 cpp、java、rust。换成 unordered_map 后次数相同，但遍历顺序不能作为程序约定。

## 三级练习

1. 预测：把 `find("python")` 换成 `counts["python"]`，map 的 size 会怎样变化？
2. 修改：换成 unordered_map，并让输出顺序不参与正确性判断。
3. 独立：把上面的 Java 词频例子关掉，从需求重新写 C++ 版本；增加“只输出次数大于 1 的词”，并为空输入写一条测试。

## 完成标准

能根据 ArrayList/HashMap/TreeMap 经验选出 vector/unordered_map/map；能解释 `vector<Task>` 拥有元素、复制 vector 会复制元素；能安全查询不存在的键，并知道修改 vector 后要重新确认旧借用是否有效。
