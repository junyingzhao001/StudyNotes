# 77 SystemProperties、property_service 与 SELinux 属性链路

> 源码基线：Android 11（`android-11.0.0_r48`）  
> 本章目标：理解 Android 属性的共享内存读取、init 写入服务、SELinux 权限、持久化和 property trigger。  
> 阅读方式：Mac 上只读源码，不要求编译或修改设备属性。

---

## 1. 一句话抓住属性系统的非对称设计

Android system property 是全设备范围的小型字符串键值机制，但读和写故意走不同路径：

```text
读取：进程直接查只读映射的共享属性区
      快，通常不需要 Binder，也不连接 init

写入：进程把请求发给 init 的 property service
      init 校验身份、SELinux、名称和值，再修改属性区
```

为什么这样设计？

- 读非常频繁，需要低成本；
- 写很少且高风险，必须集中裁决；
- 只有权威写者才能维护 serial、唤醒等待者、持久化 `persist.*`；
- init 还要根据变化执行 `on property:...` action 和 `ctl.*` 服务控制。

```text
Java SystemProperties.get()
  → JNI → bionic __system_property_find/read
  → mmap 属性区（直接读取）

Java SystemProperties.set()
  → JNI → libc property set
  → Unix socket /dev/socket/property_service
  → init HandlePropertySet
  → property_contexts + SELinux set 权限
  → 更新共享属性区 / persist 文件 / init action
```

本章最重要的主线是：

> 共享内存解决高频读取，init property service 解决受控写入。

---

## 2. 源码地图

| 文件 | 作用 | 重点 |
|---|---|---|
| `frameworks/base/core/java/android/os/SystemProperties.java` | Java 隐藏/System API | typed get、set、callback、Handle |
| `frameworks/base/core/jni/android_os_SystemProperties.cpp` | JNI 桥 | Java String/Handle 到 libc API |
| `system/core/libcutils/properties.cpp` | native 便捷 API | `property_get/set` 包装 |
| `bionic/libc/system_properties/` | 属性区客户端 | contexts、prop_area、serial、find/read |
| `system/core/init/property_service.cpp` | init 写服务 | socket、权限、`PropertySet()`、加载 |
| `system/core/init/persistent_properties.cpp` | `persist.*` 文件 | protobuf、临时文件、fsync/rename |
| `system/core/init/init.cpp` | 属性变化与控制消息 | `PropertyChanged()`、ctl handler |
| `system/core/init/action_manager.cpp` | action 调度 | property trigger 入队 |
| `system/core/init/action_parser.cpp` | rc trigger 解析 | `on property:name=value` |
| `system/sepolicy/*/property_contexts` | 名称到 SELinux type | exact/prefix 匹配 |
| `system/core/property_service/` | property info 编译解析 | trie、context、值类型 |

---

## 3. 属性不是 Settings，也不是环境变量

| 机制 | 范围/生命周期 | 典型用途 |
|---|---|---|
| System property | 设备全局；多数仅本次 boot，`persist.*` 跨重启 | boot/native/硬件/服务状态 |
| SettingsProvider | global 或 per-user；XML 持久化 | 用户设置、Framework 动态配置 |
| 环境变量 | 单进程及子进程继承 | 进程启动环境 |
| resource overlay | 构建、安装或 overlay 层 | 设备静态默认与 UI 资源 |
| sysfs/procfs | 内核/驱动接口 | 实时内核状态与控制 |

system property 适合短小、低频变化、跨语言读取的值。它不是通用数据库：

- 值是字符串；
- 没有复杂查询和多 key 事务；
- 不是 per-user；
- 大量高频数据会污染全局命名空间；
- 访问受稳定 API 和 SELinux 约束。

---

## 4. Java `SystemProperties`

常见 API：

```java
SystemProperties.get(key)
SystemProperties.get(key, defaultValue)
SystemProperties.getInt(key, defaultValue)
SystemProperties.getLong(key, defaultValue)
SystemProperties.getBoolean(key, defaultValue)
SystemProperties.set(key, value)
```

它是隐藏/System API。普通应用不应依赖反射访问；跨 partition/module 使用的稳定属性，Android 建议通过 `*.sysprop` 声明并生成类型安全 API。

### 4.1 typed getter 底层仍是字符串

```text
"42"                         → getInt 返回 42
"bad"                        → getInt 返回 default
"y"/"yes"/"1"/"true"/"on"    → getBoolean true
"n"/"no"/"0"/"false"/"off"   → getBoolean false
其他值                       → default
```

Android 11 源码注释说明布尔解析大小写敏感，不能假定任意 `TRUE` 都会得到 true。

### 4.2 名称和值长度

Java 暴露 `PROP_VALUE_MAX = 91`。Android O 移除了旧的 property name 长度上限，但为了反射兼容仍保留 `PROP_NAME_MAX = Integer.MAX_VALUE`。

底层还会校验名称、UTF-8、property type 与合法值。Java 前置检查不是唯一安全边界。`ro.*` 的底层只读值还有较长值兼容路径，不能机械认为所有属性都只有完全相同的长度行为。

### 4.3 `Handle`

`SystemProperties.find(name)` 可获得内部 Handle：

```text
第一次：按名称通过 property-info trie 查 prop_info
后续：直接用 native handle 读取
```

它适合热点只读属性，减少重复查名。Handle 指向 property 项，不是“找到时值的快照”：值更新后同一 Handle 可读到新值。

若 find 时属性不存在，返回 null；后来新增该属性并不会让旧 null 自动变成 Handle，需要重新 find。

---

## 5. JNI：为什么 get 是 FastNative，set 不是

源码直接写道：

```java
@FastNative
private static native String native_get(...);

// _NOT_ FastNative: native_set performs IPC and can block
private static native void native_set(...);
```

get 只是进入 libc 查询映射内存，适合 FastNative/CriticalNative；set 要连接 property service、等待校验与响应，可能阻塞。

`android_os_SystemProperties.cpp` 主要完成：

- Java String 与 UTF-8 转换；
- 调用 `android::base::GetProperty` 或 `__system_property_*`；
- typed parse；
- 调用 `SetProperty()`；
- native change callback 到 Java `callChangeCallbacks()` 的桥接。

`SystemProperties.set()` 绝不是更新某个 Java static Map。

---

## 6. 属性区：读为什么不需要 Binder

### 6.1 init 创建，其他进程只读映射

启动早期 init 初始化属性工作区。bionic 客户端依据 property info 找到目标 context 区域，并 mmap 为只读。

```text
property_info_area
  属性名 → SELinux context/type + 对应 prop area

prop_area(context A)
  ├─ prop_info(name1, value, serial)
  └─ prop_info(name2, value, serial)

prop_area(context B)
  └─ prop_info(name3, value, serial)
```

现代 Android 并非把全部属性简单塞在一个无隔离的文本文件里。按 SELinux context 拆分 property area，能配合不同 domain 的读取权限。

### 6.2 `prop_info` 与 serial

每个属性项含名称、值、serial 等元数据。写者更新时遵循协议，读者根据 serial 取得一致快照：

```text
读取 serial
  → 若正在写则等待/重试
  → 读取 value
  → 再确认版本状态
```

这样大量进程读取时不用争用一个 Binder 服务全局锁。

### 6.3 find 与 callback read

```cpp
const prop_info* pi = __system_property_find(name);
__system_property_read_callback(pi, callback, cookie);
```

callback 读取能正确处理同步和较长只读值。旧固定 buffer API 为兼容仍存在。

---

## 7. 读权限：能 mmap 不等于能读所有属性

属性名通过 property info 映射到 SELinux target context。init 在校验 rc 子上下文能否使用属性 trigger 时，可以直接表达为：

```cpp
property_info_area->GetPropertyInfo(name, &target_context, ...);
selinux_check_access(source_context, target_context,
        "file", "read", ...);
```

普通 bionic `get` 不会每次读属性都 RPC 到 init 再调一次 `selinux_check_access()`。它会按 context 找到 `/dev/__properties__` 下对应的带 SELinux 标签文件，并尝试只读打开/映射；文件打开权限在这一步落地。所以“所有进程都可以读取同一大块属性内存”是过时简化：不同 context 对应不同 prop area 和访问策略。普通应用能读部分公开 `ro.*`，并不表示能读所有 vendor 或敏感属性。

---

## 8. 写链路：从 `setprop`/SystemProperties 到 init

写调用最终向 property service socket 发送：

```text
property name + value
客户端 ucred（pid/uid/gid）
客户端 SELinux peer context
协议版本/命令
```

init 的 property service：

1. `accept4()` 接收连接；
2. 读取消息；
3. 获取内核提供的 peer credential/security context；
4. 从 property info 查 target context 和值类型；
5. `CheckMacPerms()` 检查 `property_service set`；
6. 校验名称和值；
7. 处理 `ctl.*`，或调用 `PropertySet()`；
8. 返回 `PROP_SUCCESS` 或具体错误码。

### 8.1 为什么客户端不能直接写 mmap

否则任意进程都能修改安全属性、伪造服务状态、触发 init action、污染持久数据，甚至破坏 serial/共享内存结构。因此普通进程只读映射，权威更新集中在 init。

---

## 9. `property_contexts`：属性名的安全类型系统

示意：

```text
persist.sys.some_flag  u:object_r:some_prop:s0 exact bool
vendor.some.           u:object_r:vendor_some_prop:s0 prefix string
```

记录可描述：

- 属性名称或前缀；
- SELinux property type/context；
- exact/prefix 匹配；
- string、bool、int、enum 等值类型。

### 9.1 编译成 trie

platform、system_ext、product、vendor、odm 等 property contexts 会被合并/序列化为 trie。运行时按属性名高效查 context 与类型，不必每次线性扫描文本。

### 9.2 读写是两种权限

```text
读：source domain → target property context 的 file read
写：source domain → target property context 的 property_service set
```

能读不等于能写。

### 9.3 Treble 所有权

vendor 组件应使用 vendor-owned namespace/context；跨 partition 属性应定义稳定 sysprop API。Framework 私自依赖 vendor 私有属性，会造成升级兼容和 SELinux 边界问题。

---

## 10. `PropertySet()` 的核心顺序

简化源码：

```cpp
if (!IsLegalPropertyName(name)) return INVALID_NAME;
if (!IsLegalPropertyValue(name, value)) return INVALID_VALUE;

prop_info* pi = __system_property_find(name);
if (pi != nullptr) {
    if (StartsWith(name, "ro.")) return READ_ONLY;
    __system_property_update(pi, value);
} else {
    __system_property_add(name, value);
}

if (persistent_properties_loaded && StartsWith(name, "persist.")) {
    WritePersistentProperty(name, value);
}
if (accept_messages) {
    PropertyChanged(name, value);
}
```

即：

1. 校验名称和值；
2. add 或 update；
3. 必要时持久化；
4. init 主循环可接收时通知变化。

---

## 11. `ro.*`：write-once，而不是“编译期常量”

源码注释明确：

```cpp
// ro.* properties are actually "write-once".
```

含义是：

- 属性尚不存在时，init 启动加载阶段可第一次创建；
- 一旦存在，后续 update 返回 read-only 错误；
- 不是“任何时刻任何代码都不能第一次 set”，而是“设定一次后不可覆盖”。

许多 `ro.*` 描述构建身份、硬件、verified boot 状态和兼容能力。运行时变化会让不同进程处于不一致世界，也会破坏安全假设。

### 11.1 `ro.boot.*`

init 从 kernel cmdline、bootconfig、device tree 导入：

```text
androidboot.foo=bar → ro.boot.foo=bar
```

随后又把部分标准 boot 字段派生到兼容属性，如 hardware、bootloader 等。

---

## 12. `persist.*`：共享内存值加跨重启文件

当 persistent properties 已加载后，修改 `persist.*` 还会更新：

```text
/data/property/persistent_properties
```

Android 11 使用 protobuf 汇总文件，写入过程包括：

```text
读取现有记录
  → 修改或新增 property
  → 序列化到 .tmp
  → fsync 临时文件
  → rename 正式文件
  → fsync 所在目录
```

临时文件避免正式文件出现半截内容；文件 fsync 推送数据，rename 原子替换目录项，目录 fsync 提高 rename 跨断电持久性。

### 12.1 为什么不能过早持久化

启动早期默认属性还在加载。若此时把默认 `persist.*` 写盘，可能覆盖上次用户保存值。流程是：

```text
加载默认/build 属性
  → /data 可用后 LoadPersistentProperties
  → 设置 ro.persistent_properties.ready=true
  → persistent_properties_loaded=true
  → 此后 persist.* 更新才同步文件
```

### 12.2 持久化不等于业务立即应用

共享区当前值会变化，文件保证下次 boot 恢复；但消费组件可能只在启动读取。是否动态应用仍看消费端。

---

## 13. 普通属性：重启后通常消失

不以 `persist.` 开头、也不由 build/default/boot 重新加载的运行时属性，只存在当前 boot 的属性区。

常见族：

- `sys.*`：系统运行请求/状态；
- `init.svc.*`：init 服务状态；
- `debug.*`：调试开关；
- `vendor.*`：vendor 运行状态；
- `ctl.*`：特殊控制消息。

前缀语义来自代码、policy 和命名约定的组合，不能只看英文名字猜。

---

## 14. `ctl.*`：外观像属性，本质是控制命令

> 下列命令只用于解释协议，**不是本章的 macOS 练习**。它们会改变 Android 设备服务状态；当前只读学习不要执行。

```bash
setprop ctl.start logd
setprop ctl.stop some_service
setprop ctl.restart some_service
```

property service 识别 `ctl.` 后不会把它作为普通键值长期存入 property area，而是调用 init control message handler：

```text
ctl.start=logd
  → HandleControlMessage("start", "logd", ...)
  → 查找 rc Service
  → 针对命令和目标校验 SELinux
  → Service::Start()
```

还有 `ctl.interface_start` 等按 interface 启动 lazy service 的形式。

### 14.1 为什么权限要带目标

如果只检查统一 `ctl.start`，有权启动低风险服务的进程可能顺便控制关键服务。init 会结合 control command 与目标服务解析更具体 property context。

### 14.2 rc 内不应绕一圈

init 自己执行 rc action 时可直接调用 Service 方法。源码会阻止/提醒在 init 内通过 `setprop ctl.*` 再绕 property service。

---

## 15. 属性变化如何触发 init action

rc 示例：

```rc
on property:sys.boot_completed=1
    start some_service
```

链路：

```text
setprop sys.boot_completed 1
  → PropertySet 更新属性
  → PropertyChanged(name, value)
  → ActionManager.QueuePropertyChange
  → 匹配 property:sys.boot_completed=1
  → action commands 在 init 主循环中执行
```

### 15.1 不是在客户端线程执行

设置者等待 property service 响应；rc action 由 init 事件循环排队执行。set 成功不等于 action 已同步执行完。

### 15.2 已存在属性和组合条件

init 在适当启动阶段检查已有属性 trigger；trigger 也可匹配通配值并与其他 property 条件组合。因此它不只是“注册以后才接收边沿事件”。

跨 partition action 还受 actionable property 规则限制，vendor rc 不能随意依赖 platform 私有属性。

---

## 16. `init.svc.*`：状态出口

init 管理的服务变化时设置：

```text
init.svc.<service>=running|stopped|restarting|...
```

`ctl.*` 是控制入口，`init.svc.*` 是由 init 维护的状态出口：

```text
ctl.start.foo
      ↓
init 启动 foo
      ↓
init.svc.foo=running
```

外部进程不应直接写 `init.svc.*` 伪造状态。

---

## 17. 属性加载顺序

简化顺序：

```text
first-stage init
  → 建立早期环境、取得 boot 信息

second-stage init
  → 初始化 property info/context 区
  → 加载 system/vendor/product/odm 等默认属性
  → 导入 kernel/bootloader 的 androidboot.*
  → 派生 ro.boot.* 与构建属性
  → 启动 property service socket/thread
  → /data 可用后加载 persist.*
  → ro.persistent_properties.ready=true
  → init 主循环处理后续 property change
```

Android 11 已有分区化的属性加载规则。不要照搬早期 Android 只讲单个 `default.prop` 的教程。

---

## 18. ChangeCallback：比 DeviceConfig listener 粒度粗

```java
SystemProperties.addChangeCallback(runnable);
```

第一次注册时 JNI 安装一个**进程内** native callback。但 Android 11 r48 中要特别区分两件事：

```text
__system_property_set()
  → 更新属性区和 global serial
  ≠ 自动在所有 Java 进程调用 Runnable

SystemProperties.reportSyspropChanged()
或 Binder SYSPROPS_TRANSACTION
  → libutils report_sysprop_change()
  → 当前进程 Java callChangeCallbacks()
```

AMS 在收到 `SYSPROPS_TRANSACTION` 时还会把它 one-way 转发给应用进程。因此这套 callback 是一条需要显式“报告 sysprop 变化”的粗粒度通知机制，不是 bionic 在后台为每个 key 建立全局 observer。若只需等待某个属性的 serial/value 变化，native 代码还有 `__system_property_wait()` 这类更直接的机制。

### 18.1 不提供具体 key

Runnable 没有参数。消费者需要重新读取自己关心的属性并与旧值比较。

### 18.2 不保证主线程

回调在哪条线程执行，取决于当前进程从哪条路径收到/调用了 `reportSyspropChanged`（常见是 Binder 事务线程）。业务代码不应假定它一定是主线程；需要线程亲和性时应 post 到自己的 Handler/Executor。

### 18.3 为什么复制后锁外执行

若持有 `sChangeCallbacks` 锁调用用户 Runnable，callback 内增删 callback 或执行慢操作会造成死锁/长时间阻塞。源码先复制列表，再在锁外执行。

---

## 19. `*.sysprop`：正式的跨模块属性 API

sysprop 描述可声明：

- owner；
- scope（public/internal）；
- access（read-only/read-write）；
- type；
- property name；
- API method name。

构建系统生成 Java/C++/Rust 等类型安全方法。优势：

- 编译期类型检查；
- 集中管理名称与默认；
- 明确 partition owner；
- 可审核稳定 API；
- 避免各处硬编码字符串。

直接使用 `SystemProperties` 适合模块内部/兼容代码，不应成为绕过 Treble API 治理的捷径。

---

## 20. 完整案例：DeviceConfig native_boot flag

承接上一章：

```text
DeviceConfig runtime_native_boot/foo = "true"
  → SettingsProvider Config XML
  → SettingsToPropertiesMapper listener
  → SystemProperties.set(
        "persist.device_config.runtime_native_boot.foo", "true")
  → JNI/libc property service socket
  → init 查 property_contexts
  → SELinux property_service set 允许
  → 更新共享属性区
  → 写 /data/property/persistent_properties
  → 下次 boot 恢复
  → runtime/native 组件初始化时读取并应用
```

这里有两个持久化层：

- Config XML 是 DeviceConfig 权威配置；
- `persist.device_config.*` 是供 native boot 消费的镜像。

RescueParty/mapper 还需避免导致 crash loop 的坏配置在启动时立刻被灌回。

---

## 21. 为什么设置会失败

可能失败于：

1. 属性名非法；
2. value 非法、过长或不符合声明类型；
3. property_contexts 未匹配到正确 target context；
4. 调用 domain 无 `property_service set`；
5. `ro.*` 已存在；
6. `ctl.*` 目标 service 不存在或无控制权限；
7. socket/service 尚未就绪；
8. 持久文件写入失败。

Java `SystemProperties.set()` 会抛 RuntimeException；更具体原因通常在 libc/init/SELinux 日志中。反复重试不能解决 policy 或类型错误。

---

## 22. 只读源码练习路线

### 第一轮：Java/JNI

```bash
sed -n '80,320p' frameworks/base/core/java/android/os/SystemProperties.java
sed -n '70,280p' frameworks/base/core/jni/android_os_SystemProperties.cpp
```

回答为什么 set 不是 FastNative。

### 第二轮：bionic 读路径

```bash
rg -n "__system_property_find|__system_property_read_callback|AreaInit" \
  bionic/libc/system_properties
```

画出 property_info_area 到 per-context prop_area。

### 第三轮：init 写路径

```bash
rg -n "HandlePropertySet|CheckMacPerms|PropertySet|handle_property_set_fd" \
  system/core/init/property_service.cpp
```

标出 credential、SELinux、值类型、ro、persist、PropertyChanged 的顺序。

### 第四轮：持久属性

```bash
sed -n '120,255p' system/core/init/persistent_properties.cpp
```

说明 tmp、rename、文件 fsync、目录 fsync 各自保护什么。

### 第五轮：trigger 与 control

```bash
rg -n "PropertyChanged|QueuePropertyChange|HandleControlMessage|ctl\\." \
  system/core/init
```

区分普通 property trigger 和 ctl command。

---

## 23. 常见误区纠正

### 误区 1：get 和 set 都通过 Binder

不对。get 直接读 mmap 属性区；set 通过 Unix property service socket，也不是 Binder。

### 误区 2：属性是进程内 static Map

不对。它是 init 管理、不同进程映射读取的系统级属性区。

### 误区 3：`ro.*` 从任何时候都绝对不能写

不准确。它是 write-once：启动阶段可首次创建，存在后不能更新。

### 误区 4：所有属性都会跨重启

不对。只有 `persist.*` 以及每次启动从 build/boot/default 重建的属性会回来。

### 误区 5：`persist.*` 写入后所有组件立即应用

不对。持久化与业务消费时机分离，组件可能只在启动读取。

### 误区 6：`ctl.start` 会像普通属性一样保存

不对。它是命令通道，`getprop ctl.start` 不能读回最后命令。

### 误区 7：能读就能写

不对。`file read` 与 `property_service set` 是不同 SELinux 权限。

### 误区 8：ChangeCallback 告诉具体 key

不对。它是无参数 Runnable，需要重新读取关心 key。

### 误区 9：任意 `setprop` 成功都会自动唤起所有 Java ChangeCallback

不对。属性区 serial 会变，但 Java Runnable 通知还需要 `reportSyspropChanged()`/`SYSPROPS_TRANSACTION` 等显式传播路径。

### 误区 10：set 成功意味着 rc action 已执行完

不对。action 在 init 主循环排队执行。

### 误区 11：硬编码名字就是稳定跨模块 API

不对。正式跨边界依赖应使用 `*.sysprop`。

---

## 24. 排查属性问题的九层法

```text
① 名字：前缀、拼写、partition owner 正确吗？
② 类型：property_contexts 声明类型和值匹配吗？
③ 来源：build、boot、rc、DeviceConfig 还是运行时 set？
④ 读取：source domain 有 target context 的 file read 吗？
⑤ 写入：source domain 有 property_service set 吗？
⑥ 特殊语义：ro 已存在？persist 已加载？ctl 目标有效？
⑦ 属性区：set 后 get 是否看到新值？
⑧ 消费端：动态监听还是只在 boot 读取？
⑨ trigger：rc 条件匹配吗，action 是否已排队执行？
```

还应检查 SELinux denial、property service 错误、值类型错误和持久文件写入日志。

---

## 25. 练习题

### 题 1

为什么 get 适合 FastNative，而 set 不适合？

### 题 2

`ro.demo` 不存在时 init 首次写成功，随后再次写为什么失败？

### 题 3

某进程能读 `vendor.foo`，能否推出它能修改？

### 题 4

设置 `persist.demo=1` 后立刻断电，应分别考虑哪两处状态？

### 题 5

`setprop ctl.start myservice` 与 `getprop ctl.start` 为什么不对称？

### 题 6

属性 callback 后怎样判断是不是自己关心的 key 变化？

### 参考答案

1. get 直接读映射内存；set 要 socket IPC、权限检查和响应，可能阻塞。
2. `ro.*` 是 write-once，第一次 add 后 update 返回只读错误。
3. 不能。读写使用不同 SELinux class/permission。
4. 当前共享内存值，以及 persistent properties 文件是否已安全落盘。
5. `ctl.*` 是控制命令，不是存储属性。
6. callback 没有 key 参数，应重新读取关心值并与本地旧值比较。

---

## 26. 复读后的易懂性补强

复读后最难理解的是“共享内存谁都能映射，为什么仍然安全”。可以把它想成玻璃公告栏：

```text
属性区 = 玻璃封住的公告栏
获准进程可以快速阅读，但不能伸手修改

property service = 公告栏管理员 init
写申请携带真实身份和目标栏目
管理员按 property_contexts + SELinux 审批后张贴
```

三类特殊前缀：

```text
ro.*      = 贴上后不可覆盖的公告
persist.* = 公告内容同时抄进跨重启档案
ctl.*     = 交给管理员的操作申请，不贴到公告栏
```

变化后的三件事也必须分开：

```text
属性区更新成功
  ≠ persist 文件一定已经安全落盘
  ≠ init property action 已执行完成
  ≠ 业务组件已经重新读取并应用
```

排障时需要分别找证据。

---

## 27. 本章总结

```text
启动：
init 合并 property_contexts、创建按 context 的属性区
  → 导入 kernel/bootloader/build/default
  → 启动 property service
  → /data 可用后加载 persist.*

读取：
SystemProperties/生成 sysprop API
  → JNI/bionic find/read
  → 直接访问只读 mmap prop_area

写入：
SystemProperties.set/setprop
  → JNI/libc property service socket
  → init 获取 ucred + SELinux peer context
  → property_contexts 解析 target context/type
  → property_service set 权限检查
  → add/update（ro.* write-once）
  → persist.* 文件
  → PropertyChanged/callback/init action

控制：
ctl.* → 不存储，直接进入 init Service control handler
```

本章应掌握六条边界：

1. 共享内存读与 property service 写；
2. `file read` 与 `property_service set`；
3. 普通属性、`ro.*`、`persist.*`、`ctl.*`；
4. 当前属性区与持久属性文件；
5. 属性更新、callback、init action 和业务应用；
6. 私有硬编码 property 与正式 `*.sysprop` API。

下一章将学习 `Watchdog、SystemServer 卡死检测与 RescueParty 故障自愈链路`，把本章 property/config reset 与 system_server 线程卡死、重启和配置回滚联系起来。
