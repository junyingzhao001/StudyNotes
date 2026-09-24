# 可运行示例

这些示例对应课程中最容易只靠文字误解的几个主题：

- `00_hello.cpp`：编译、运行和退出码。
- `class_lifecycle.cpp`：类的构造、使用和析构顺序。
- `file_tasks.cpp`：文件打开、写入、关闭检查与读取。
- `thread_queue.cpp`：条件变量、排空队列和 join。
- `frame_parser.cpp`：TCP 字节流中的长度前缀、拆包和粘包。

运行全部示例：

```bash
bash scripts/run_examples.sh
```

网络库、JSON、测试框架和数据库示例需要第三方依赖，因此正文会先解释接口责任，再在对应综合项目中统一接入，避免每篇重复安装不同依赖。
