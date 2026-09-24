# 25 · 项目目录与 CMake target

[返回学习地图](../README.md) · [上一篇](24-C接口ABI与回调.md) · [下一篇](26-测试与可测试设计.md)

## 先说这篇讲什么

这篇讲如何把源码组织成库、程序和测试目标，让编译关系写进项目而不是记在脑中。

## 它有什么用

手写编译命令会随文件增多变得不可维护。CMake target 能携带源文件、头文件路径、编译标准和依赖关系。

## 要解决的问题

全局 include_directories 和 flags 会影响无关目标，依赖来源难追踪。现代 CMake 尽量把配置挂到具体 target。

## 先把几个词说清楚

- **target**：一次构建产生的库或程序
- **PUBLIC/PRIVATE**：依赖是否要继续传给使用者
- **out-of-source build**：构建产物放在源码目录之外

## 一个案例

task_core 是业务库，task_cli 链接它，task_test 也链接它。三者共用接口，但 main 和测试代码互不混合。

```cpp
add_library(task_core task_store.cpp)
target_include_directories(task_core PUBLIC include)
target_compile_features(task_core PUBLIC cxx_std_20)
add_executable(task_cli main.cpp)
target_link_libraries(task_cli PRIVATE task_core)
```

## 常用 API 和作用

| API / 规则 | 用来做什么 |
| --- | --- |
| `add_library/add_executable` | 创建库或程序目标 |
| `target_link_libraries` | 声明目标依赖 |
| `target_include_directories` | 声明头文件搜索路径 |
| `target_compile_features` | 声明需要的 C++ 标准能力 |

## 怎么验证自己真的理解了

在空 build 目录执行 configure 和 build，再删除 build 重建。确认没有依赖旧产物。

## 常见坑

- 不要用 glob 自动吞入所有源码；新文件是否进入目标应清楚可见。
- PUBLIC 会传播依赖，不是“更强的 PRIVATE”。
- CMake 版本、编译器和生成器是三个不同概念。

## 马上动手

把本地任务程序拆成 core 库、cli 和 test 三个 target。

## 学完标准

能从 CMakeLists 找到一个程序由哪些源码和库组成，并能干净重建。
