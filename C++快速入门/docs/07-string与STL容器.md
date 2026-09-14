# 07 · string 与 STL 容器

[返回学习地图](../README.md) · [上一篇：智能指针与所有权](06-智能指针与所有权.md) · [下一篇：算法、Lambda 与迭代器](08-算法Lambda与迭代器.md)

## 先说结论

日常 C++ 业务数据优先放进标准库类型：文本用 `std::string`，顺序数据先考虑 `std::vector`，键值查询按是否需要顺序选择 `std::unordered_map` 或 `std::map`。这些容器会管理元素的创建、移动与销毁，通常不需要自己申请数组。

读完本篇，你能把 Java 的 ArrayList、HashMap、TreeMap 使用经验迁移到 C++，完成一个词频统计；还能判断容器保存的是值还是借用，并知道哪些修改可能让旧引用和迭代器失效。

## 问题一：选错容器会让代码绕远

本篇只围绕一个需求：保存单词列表，并统计每个单词出现次数。

先按需求选择：

| 需求 | C++17 首选 | Java 中较接近的类型 |
| --- | --- | --- |
| 保存可变长度的连续序列 | `std::vector<T>` | `ArrayList<T>` |
| 按键查值，不关心遍历顺序 | `std::unordered_map<K, V>` | `HashMap<K, V>` |
| 按键有序保存和输出 | `std::map<K, V>` | `TreeMap<K, V>` |
| 两端频繁增删 | `std::deque<T>` | `ArrayDeque<T>` |
| 只保存不重复的值 | `std::unordered_set<T>` / `std::set<T>` | `HashSet<T>` / `TreeSet<T>` |
| 可能有一个值，也可能没有 | `std::optional<T>` | `Optional<T>` |

本 demo 需要按字母顺序输出结果，所以选择 `vector<string>` 保存输入，选择 `map<string, int>` 统计次数。若只关心查找速度而不关心顺序，可以评估 unordered_map。

`std::list` 接近 Java LinkedList，但在 C++ 中通常不作为默认序列。没有明确的稳定迭代器或中间插入需求时，先选 vector。

## 方案一：用值语义保存数据

Java 的 `ArrayList<Task>` 通常保存 Task 引用。C++ 的 `vector<Task>` 默认直接拥有一组 Task 对象。容器销毁时，元素也随之销毁；复制整个容器时，若元素类型可复制，通常也会复制元素。

词频例子中的 string 同样是值：

- `words` 拥有其中的每个 string；
- `words.push_back("rust")` 在容器中加入一个字符串值；
- `auto copied = words` 会得到独立 vector 和独立 string 副本；
- `auto& alias = words` 才是同一个 vector 的别名。

C++ 的 string 可修改，普通赋值通常复制字符内容。这与 Java String 不可变、对象变量保存引用的习惯不同。

## 方案二：用同一段逻辑完成统计和查询

关键代码来自完整的 [demos/07.cpp](../demos/07.cpp)：

```cpp
std::vector<std::string> words{"cpp", "java", "cpp"};
words.push_back("rust");

std::map<std::string, int> counts;
for (const auto& word : words) {
    ++counts[word];
}

const auto it = counts.find("python");
```

`const auto& word` 表示每轮只读借用容器中的 string，避免逐个复制。`counts[word]` 在键不存在时会插入一个值初始化的 int，也就是 0，然后 `++` 把它增加为 1。这适合统计。

只查询时不要使用 `counts["python"]`，因为它会插入新键。`find` 不修改 map：

- 找到时，迭代器不等于 `counts.end()`；
- 找不到时，迭代器等于 `counts.end()`；
- `end()` 是“末尾之后”的哨兵，不能解引用。

map 的元素包含键和值。通过迭代器访问时，`it->first` 是键，`it->second` 是值。C++17 也可使用 `const auto& [word, count]` 结构化绑定来读取它们。

## vector 的日常操作

| 操作 | C++ vector | Java ArrayList |
| --- | --- | --- |
| 追加 | `values.push_back(x)` | `values.add(x)` |
| 元素数 | `values.size()` | `values.size()` |
| 带边界检查访问 | `values.at(i)` | `values.get(i)` |
| 不做边界检查访问 | `values[i]` | 没有直接对应；get 会检查 |
| 是否为空 | `values.empty()` | `values.isEmpty()` |
| 清空 | `values.clear()` | `values.clear()` |
| 预留容量 | `values.reserve(n)` | `values.ensureCapacity(n)` |

`size()` 是当前元素数；`capacity()` 是已分配空间当前可容纳的元素数。`reserve` 只增加容量，不创建元素；`resize` 会改变 size，并创建或销毁元素。

初始化形式也不能机械替换：

- `vector<int>{3, 7}`：两个元素，值分别为 3、7；
- `vector<int>(3, 7)`：三个元素，每个值都是 7。

## 问题二：容器修改后，旧借用可能失效

vector 为了连续存储，扩容时可能把全部元素搬到新内存。之前指向元素的引用、指针和迭代器便不能继续使用。Java 集合有时用 `ConcurrentModificationException` 暴露修改问题；C++ 失效借用通常不会自动抛异常，继续使用可能产生未定义行为。

先掌握最常见的规则：

| 操作 | 对既有迭代器、引用和指针的影响 |
| --- | --- |
| vector 插入并发生扩容 | 指向元素的全部借用和迭代器失效 |
| vector 插入但未扩容 | 插入点之前保持有效；插入点及之后失效 |
| vector erase | 删除点及之后失效 |
| map 插入 | 既有迭代器和元素借用保持有效 |
| map erase | 只有指向被删除元素的借用失效 |
| unordered_map rehash | 迭代器失效；未删除元素的引用和指针保持有效 |
| unordered_map erase | 指向被删除元素的借用失效 |

不用一次背完所有规则。只要代码“先保存了元素位置，随后又修改容器”，就应查询该操作的失效规则，并尽量缩短借用时间。

## 验证：检查词频、顺序和只读查询

在 `C++快速入门` 目录执行：

```bash
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Wpedantic -g demos/07.cpp -o build/demo_07
./build/demo_07
```

预期输出：

```text
cpp: 2
java: 1
rust: 1
python found: 0
first: cpp
```

map 按键排序，因此前三行按 cpp、java、rust 输出。`find("python")` 没有插入新键，所以显示 0；`words.at(0)` 带边界检查并返回第一个单词。

## 常见坑

- 用 map 的 `operator[]` 做只读查询，会在键不存在时修改容器。
- 把 unordered_map 的遍历顺序写进业务约定或测试，结果可能不稳定。
- 用 `values[i]` 访问越界不会像 Java `get` 那样稳定抛出边界异常；不确定输入时优先 `at` 或先检查范围。
- 保存 vector 元素的引用或指针后继续 push_back，旧借用可能因扩容失效。
- 复制 vector 默认复制元素；若想操作同一个容器，需要明确使用引用。
- `vector<bool>` 是特殊压缩表示，元素行为与普通 `vector<T>` 不完全相同；入门时不要把它作为理解引用规则的例子。

## 三级练习

1. **预测**：把 `find("python")` 换成 `counts["python"]` 后，map 的 size 会怎样变化，随后遍历会多出什么？
2. **修改**：把 map 换成 unordered_map，并让验证只检查词频，不依赖遍历顺序。
3. **独立完成**：从空 vector 开始录入单词，统计词频，只输出次数大于 1 的词；为空输入补一条测试。

## 完成标准

能根据 ArrayList、HashMap、TreeMap 的经验选择 vector、unordered_map、map；能解释 `vector<Task>` 拥有元素、复制 vector 通常会复制元素；能安全查询不存在的键，并在修改容器后重新确认旧借用是否有效。

## 马上动手

1. 运行 demo，核对五行固定输出。
2. 在 [demos/07.cpp](../demos/07.cpp) 中先输出 `counts.size()`，再执行 `counts["python"]`，再次输出 size。
3. 把查询恢复为 find，确认 size 不再变化。
4. 完成后恢复 demo，执行 `bash scripts/run_all.sh`。
