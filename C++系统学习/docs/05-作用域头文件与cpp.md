# 05 · 作用域、namespace、头文件与 cpp

[返回学习地图](../README.md) · [上一篇](04-函数重载引用与Lambda.md) · [下一篇](06-宏与条件编译.md)

## 先说这篇讲什么

这篇讲名字在哪里可见，以及公开接口和实现为什么要分开放。读完能正确拆分多文件。

## 它有什么用

真实项目不会把所有代码放进一个 cpp。清楚头文件和实现文件的职责，才能定位重复定义、名字冲突和链接不到实现。

## 要解决的问题

把所有函数实现写进公共头文件，多个 cpp 包含后可能产生重复定义；把声明藏在 cpp，其他文件又无法调用。

## 先把几个词说清楚

- **作用域**：一个名字能被使用的代码范围
- **声明**：告诉编译器名字和类型
- **定义**：真正提供对象或函数实现

## 一个案例

score.h 只公开 normalizeScore 的声明；score.cpp 放算法和私有辅助函数；main.cpp 只依赖头文件。头文件像菜单，cpp 像厨房。

```cpp
// score.h
#pragma once
namespace score { int normalize(int value); }

// score.cpp
#include "score.h"
int score::normalize(int value) { return value < 0 ? 0 : value; }
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `namespace` | 组织公开名字，减少冲突 |
| `匿名 namespace` | 让辅助名字只在当前 cpp 可见 |
| `include guard` | 防止头文件在同一编译单元重复展开 |
| `#pragma once` | 主流编译器支持的头文件保护写法 |

## 怎么验证自己真的理解了

分别编译 main.cpp 和 score.cpp。然后漏掉 score.cpp，确认错误发生在链接阶段。

## 常见坑

- 头文件中不要写 `using namespace std;`，它会影响所有包含者。
- 不要 `#include "score.cpp"` 来绕过构建配置。
- 作用域是名字是否可见；生命周期是对象是否还活着，两者不是一回事。

## 马上动手

把一个已有单文件程序拆成 `.h + .cpp + main.cpp`，辅助函数放匿名 namespace。

## 学完标准

能解释头文件、cpp、声明、定义、include 和链接分别做什么。
