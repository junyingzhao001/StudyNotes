# 15 · CMake 工程化构建与测试

[返回学习地图](../README.md) · [下一篇：调试排错与实际开发路线](16-调试排错与实际开发路线.md)

## 先说结论

这一篇解决“源文件、库、程序、测试和构建开关越来越多后，怎样稳定地构建同一个项目”的问题。读完后，你能看懂 CMake target，判断头文件路径和链接依赖如何传播，构建 Debug/Release，并用 CTest 运行测试。

它有用，是因为真实项目不能靠每个人手写一条越来越长的编译命令。CMake 把构建关系保存成代码，让命令行、IDE 和持续集成使用同一份目标图。声明、定义和多文件编译若还不熟，先看 [01A · 作用域、头文件、cpp 与宏定义](01A-作用域头文件与宏定义.md)。

## 问题：编译命令只描述一次操作，没有表达项目关系

单文件可以直接运行 `c++ demo.cpp -o demo`。拆成库、应用和测试后，你还要持续回答：

- 哪些 cpp 属于哪个库？
- 谁可以看到哪个头文件目录？
- 哪个程序链接哪个库？
- 哪个目标开启跟踪宏？
- 测试怎样被统一发现和执行？

CMake 的核心答案是 target。target 可以是库或可执行程序，依赖通过 target 连接。

Java/Gradle 类比可以帮助理解：

| Java/Gradle | CMake/C++ 中较接近的概念 | 差别 |
| --- | --- | --- |
| module | target | target 可以是库或可执行程序 |
| source set | `add_library` / `add_executable` 的源文件 | 每个 cpp 仍独立编译 |
| `implementation project(...)` | `target_link_libraries(...)` | C++ 还要处理头文件可见性 |
| test task | 测试可执行程序 + CTest | 测试以进程返回码表示成功/失败 |
| classpath | 无单一对应 | include 搜索路径和二进制链接分开管理 |

## 方案：把真实关系建成 target 图

[完整 CMakeLists.txt](../CMakeLists.txt) 中，`structure_demo` 对应三类真实目标：

```cmake
add_library(score_rules structure_demo/score.cpp)
target_include_directories(score_rules PUBLIC structure_demo)

add_executable(header_source_demo structure_demo/main.cpp)
target_link_libraries(header_source_demo PRIVATE score_rules)

add_executable(header_source_trace_demo structure_demo/main.cpp)
target_link_libraries(header_source_trace_demo PRIVATE score_rules)
target_compile_definitions(header_source_trace_demo PRIVATE QUICKSTART_TRACE)
```

关系图是：

```text
score.cpp ─► score_rules（库）
                  │
                  ├──► header_source_demo
                  │       输出普通结果
                  └──► header_source_trace_demo
                          额外定义 QUICKSTART_TRACE
```

两个程序使用同一个 main.cpp 和同一个库。trace 目标通过 `target_compile_definitions` 定义宏，因此只有它会编译 `#ifdef QUICKSTART_TRACE` 中的输出。这比在源码里手改 `#define` 更可重复，也避免不同构建互相污染。宏与条件编译的含义见 [01A](01A-作用域头文件与宏定义.md)。

## PUBLIC 与 PRIVATE 在传播什么

`score_rules` 的 PUBLIC include 路径表示两层含义：构建 score_rules 自己需要它，链接 score_rules 的下游目标也需要它来找到 `score.h`。

`header_source_demo` PRIVATE 链接 score_rules，表示该程序使用这个库，但不会把依赖继续作为自己的公开使用要求传播。可执行程序通常没有下游，这里主要用来建立语义。

课程中的 `task_core` 同样是库，`task_cli`、`task_store_test` 和 `task_file_test` 链接它。第 17–22 篇的 `task_executor` 还 PUBLIC 链接 `Threads::Threads`；CMake 会按当前平台选择所需线程参数，避免到处硬编码 `-pthread`。

## 编译特性和警告也属于 target

本项目的 `configure_course_target` 为每个目标要求 C++17、关闭编译器扩展并启用常用警告。`target_compile_features(... cxx_std_17)` 表示至少需要 C++17。

警告经常提前暴露未使用变量、可疑转换和控制流问题，但警告不是完整正确性证明。项目应统一警告策略，避免某个目标漏掉关键检查。

## 验证：配置、构建、运行、测试

先确认 CMake 可用，再从课程目录执行：

```bash
cmake --version
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake
./build/cmake/header_source_demo
./build/cmake/header_source_trace_demo
(cd build/cmake && ctest --output-on-failure)
```

两个结构示例的预期输出分别是：

```text
normalized: 100
```

```text
normalized: 100
trace: raw=135, max=100
```

CTest 成功时，`header_source_demo`、`header_source_trace_demo`、任务项目测试和执行器测试都应通过；失败时加 `--output-on-failure` 可看到目标输出。

当前课程也提供不依赖 CMake 的等价验证入口：

```bash
bash scripts/run_all.sh
```

## Debug、Release 与生成器差异

单配置生成器常在配置时设置 `CMAKE_BUILD_TYPE=Debug` 或 Release。Release 示例：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Visual Studio、Xcode 等多配置生成器通常在构建时使用 `--config Debug`，CTest 使用 `-C Debug`，可执行文件也可能在 `build/cmake/Debug/` 子目录。

Release 常启用优化并减少调试信息，但不能用“Release 跑得更快”判断逻辑正确。先让测试在 Debug 与必要的 Sanitizer 配置通过，再做性能测量。

## 测试 target 为什么独立

[task_store_test.cpp](../tests/task_store_test.cpp) 直接测试业务状态，[task_file_test.cpp](../tests/task_file_test.cpp) 用 stringstream 测试格式，无需启动交互程序。它们各自是可执行 target，并通过 `add_test` 注册给 CTest。

测试遵循“准备 → 执行 → 检查”，失败时 main 返回非 0。`scripts/run_all.sh` 还把固定命令输入 task_cli、比较完整输出并跨进程检查持久化，这属于端到端验证。单元测试定位快，端到端测试证明模块组装后的行为。

## 第三方依赖和目录边界

进入项目先找 README、CMakePresets.json、vcpkg.json、conanfile 等现有约定。依赖版本要可重复，并通过 target 传播使用要求；不要复制几份头文件就当作依赖管理。

常见目录会把公开头文件放 `include/`、实现放 `src/`、入口放 `app/`、测试放 `tests/`。目录本身不会自动形成模块，真正的边界仍由公开接口和 target 关系决定。

## 局限与常见坑

- include 成功只说明声明可见，不说明实现已链接。
- 不要使用全局 `include_directories` 和 `add_definitions` 随意污染所有目标；优先 `target_*` 命令。
- PUBLIC/PRIVATE 写错会导致依赖泄漏或下游缺少使用要求。
- 新增 cpp 后忘记加入 target，可能出现链接错误。
- 构建目录应与源码分开；不要把生成文件散落到项目目录各处。
- CMake 负责描述构建，不自动成为包管理器，也不替代测试。

## 三级练习

1. 预测：删除 `target_link_libraries(header_source_demo PRIVATE score_rules)` 后，失败发生在编译还是链接？为什么？
2. 修改：新增 `header_source_quiet_demo`，仍链接 score_rules，但不定义 QUICKSTART_TRACE；运行并核对输出。
3. 独立：新增 `task_count_test` target，先写一个会失败的检查确认 CTest 能发现，再修正测试；随后新增一个 cpp 到 task_core 并观察漏加源文件时的链接错误。

## 完成标准

能从空 build 目录配置并构建；能画出 score_rules 与两个程序的依赖；能解释 PUBLIC/PRIVATE、include 与 link 的区别；能用 target 定义编译宏并用 CTest 识别失败。

## 马上动手

打开 [CMakeLists.txt](../CMakeLists.txt)，先沿 `score_rules → header_source_trace_demo` 读一遍。然后分别运行普通版和 trace 版；若输出相同，就检查 `target_compile_definitions` 是否只挂在 trace target 上。
