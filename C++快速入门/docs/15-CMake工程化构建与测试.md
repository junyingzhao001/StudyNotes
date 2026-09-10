# 15 · CMake 工程化构建与测试

[返回学习地图](../README.md) · [下一篇：调试排错与实际开发路线](16-调试排错与实际开发路线.md)

## 本篇目标

会用 CMake 构建“库 + 应用 + 测试”，理解 target、include 路径、链接关系、Debug/Release 和 CTest。

## 学习作用

单文件可以直接调用 c++ 编译器，真实项目却需要稳定描述几十个甚至上千个源文件之间的关系。CMake 保存的是构建规则，让开发机、IDE 和持续集成使用同一套目标。

## Java 对照

如果用过 Gradle 或 Maven，可以把 CMake 理解为 C++ 常见的构建配置层，但概念不能逐字对应：

| Java/Gradle 常见概念 | CMake/C++ 中较接近的概念 |
| --- | --- |
| module | target（库或可执行程序） |
| `implementation project(...)` | `target_link_libraries(...)` |
| source set | `add_library` / `add_executable` 中的源文件 |
| test task | 测试可执行文件 + CTest |
| classpath | 没有单一对应物；需分别处理头文件搜索和二进制链接 |
| Debug/Release variant | 构建配置，但具体行为取决于生成器 |

`#include` 只让编译器看见声明，链接库才让链接器找到实现。Java 的 import 也不负责下载依赖；同理，C++ 的 include 更不是包管理器。

## 本课程项目的目标关系

```text
task_core（库）
├── task_store.cpp
└── task_file.cpp
        ▲
        │ link
   ┌────┴──────────┐
task_cli      task_store_test / task_file_test
（应用）             （测试程序）
```

在 [CMakeLists.txt](../CMakeLists.txt) 中：

```cmake
add_library(task_core project/task_store.cpp project/task_file.cpp)
target_include_directories(task_core PUBLIC project)
target_compile_features(task_core PRIVATE cxx_std_17)

add_executable(task_cli project/main.cpp)
target_link_libraries(task_cli PRIVATE task_core)
```

`task_core` 是可复用业务逻辑，task_cli 只负责用户输入输出。PUBLIC include 路径会传给链接 task_core 的调用者；PRIVATE 链接表示 task_cli 使用该库，但不会把这个依赖继续传给自己的下游。

`target_compile_features` 要求这个目标至少支持 C++17。课程通过一个小函数为每个目标设置该特性、关闭编译器扩展并启用常用警告。警告不是“可以永远忽略的小提示”；它经常能提前暴露类型转换、未使用变量和可疑控制流。

第 17–22 篇使用线程。直接调用 GCC/Clang 时通常要加 `-pthread`；在 CMake 中应链接可移植的线程 target，而不是把某个平台参数散落到每个目标：

```cmake
find_package(Threads REQUIRED)

add_library(task_executor concurrency/task_executor.cpp)
target_link_libraries(task_executor PUBLIC Threads::Threads)
```

链接 task_executor 的 demo 和测试会继承这项 PUBLIC 使用要求。Windows、Linux 和 macOS 的底层参数可能不同，`Threads::Threads` 由 CMake 按当前工具链处理。

## 构建与运行

先检查是否已安装：

```bash
cmake --version
```

安装后在课程目录执行：

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake
(cd build/cmake && ctest --output-on-failure)
./build/cmake/task_cli
```

`-S .` 指源码目录，`-B build/cmake` 指独立构建目录。不要把生成的大量中间文件散落到源码目录。单配置生成器使用 `CMAKE_BUILD_TYPE`；Visual Studio、Xcode 等多配置生成器通常在构建时写 `cmake --build build/cmake --config Debug`，测试时写 `(cd build/cmake && ctest -C Debug --output-on-failure)`，可执行文件也常位于 `build/cmake/Debug/task_cli` 等配置子目录。

Release 构建常启用优化并减少调试信息：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

不要用 Release 是否更快来判断逻辑是否正确；先让测试在 Debug 下通过，再比较真实性能。

## 单元测试和端到端测试

[task_store_test.cpp](../tests/task_store_test.cpp) 直接调用业务类，验证新增、完成、删除和错误输入，这是单元测试。[task_file_test.cpp](../tests/task_file_test.cpp) 用 stringstream 验证保存后再加载，避免依赖真实磁盘。

测试代码没有引入第三方框架，核心结构只有三步：准备数据、执行行为、检查结果。`expect` 失败时累计失败数，main 最终返回非 0，CTest 和自动脚本因此能判断测试失败。大型项目通常采用 GoogleTest、Catch2 或项目已有框架；进入项目后先遵循仓库现有选择。

`scripts/run_all.sh` 还把命令写给 task_cli，再比较整段输出，并让两个独立进程共用数据文件。这属于更接近用户视角的端到端检查。单元测试定位快，端到端测试覆盖组装结果，两者作用不同。

本机不具备 CMake 时，仍可运行等价的直接编译验证：

```bash
bash scripts/run_all.sh
```

## 常见工程目录

较大的库经常组织为：

```text
project/
├── CMakeLists.txt
├── include/project_name/   对外头文件
├── src/                    实现
├── app/                    可执行程序入口
└── tests/                  测试
```

本课程为减少第一轮阅读跳转，把小项目的头文件和源文件放在同一个 project/ 下，测试单独放 tests/。理解 target 关系后，再按团队规范拆目录。目录本身不会自动建立模块边界，target 与公开接口才是关键。

## 第三方依赖先懂原则

进入已有项目时先阅读 README、CMakePresets.json、vcpkg.json、conanfile.py/ conanfile.txt 等文件，使用仓库已经选定的依赖方式。新项目可以评估 vcpkg 或 Conan；CMake 的 FetchContent 也能获取部分源码依赖。版本要可重复，依赖要通过 target 链接，不要从网上复制一对头文件就当作完整依赖管理。

## 动手练习与完成标准

新增一个 `task_count_test` 测试目标，先写一个必定失败的检查，确认 CTest 能显示失败；再修正它。随后在 task_core 中新增源文件，并把它加入 `add_library`，观察漏加源文件时的链接错误。

完成标准：能从空 build 目录构建项目；能解释 task_core、task_cli 和测试的链接关系；测试失败时命令返回非 0；知道 include 成功不代表链接一定成功。
