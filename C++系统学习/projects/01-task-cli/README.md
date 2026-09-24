# 项目一：本地任务管理器

## 这个项目练什么

把类、容器、错误处理、文件、filesystem、CMake 和测试连起来。暂时不使用线程和网络。

## 必须实现的行为

- `add TITLE`：标题去掉首尾空白后不能为空。
- `done ID`：任务存在时标为完成，不存在时给出稳定错误。
- `remove ID`：删除指定任务。
- `list`：按 id 输出所有任务。
- 退出后重新启动，任务仍然存在。

## 建议目录

```text
task-cli/
├── include/task_store.h
├── src/task_store.cpp
├── src/task_file.cpp
├── src/main.cpp
├── tests/task_store_test.cpp
├── tests/task_file_test.cpp
└── CMakeLists.txt
```

## 实现顺序

1. 先写内存中的 `TaskStore` 和单元测试。
2. 再写命令解析，不接文件。
3. 加入加载和保存，先写临时文件，成功后替换正式文件。
4. 测试坏行、目录不存在和保存失败。
5. 最后整理 CMake target 和一键验证脚本。

## 完成标准

正常路径和失败路径都有自动化测试；业务类不直接读终端；保存失败时内存和磁盘不会对外宣称成功。
