# 15 · string、string_view 与 span

[返回学习地图](../README.md) · [上一篇](14-模板与concepts.md) · [下一篇](16-顺序容器.md)

## 先说这篇讲什么

这篇区分拥有数据的容器和不拥有数据的视图，并说明文本长度为什么不等于用户看到的字符数。

## 它有什么用

解析器、网络库和 C API 常希望只读一段现有数据而不复制。视图很轻，但不会替你延长底层数据生命。

## 要解决的问题

返回指向临时 string 的 string_view 会立即悬空；按字节截断 UTF-8 还可能切开一个字符。

## 先把几个词说清楚

- **拥有者**：真正存放字符或元素的对象
- **视图**：只记录地址和长度的轻量借用
- **UTF-8**：一种变长编码，一个可见字符可能占多个字节

## 一个案例

parseCommand 只在调用期间读取输入，因此接收 string_view；它不能把 view 保存到比原 string 更久的对象中。

```cpp
bool startsWith(std::string_view text,
                std::string_view prefix) {
    return text.starts_with(prefix);
}
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `std::string` | 拥有并管理可修改字符 |
| `std::string_view` | 只读字符视图，不保证零结尾 |
| `std::span<T>` | 连续 T 元素的视图 |
| `data/size/c_str` | 取得地址、长度或零结尾 C 字符串 |

## 怎么验证自己真的理解了

传入 string、字符串字面量和子串视图。再写一个返回局部 string_view 的错误版本，用注释解释为什么不能调用。

## 常见坑

- string_view::data() 不保证末尾有 `\0`。
- 修改或销毁原 string 可能让 view 失效。
- size 返回字节/代码单元数量，不是用户看到的字符数量。

## 马上动手

实现一个接收 span<const int> 的 sum，让 array 和 vector 都能调用。

## 学完标准

能区分拥有者和视图，并为每个 view 标出有效期。
