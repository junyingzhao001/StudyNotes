# 99 Android 启动可观测性：bootstat、EventLog、statsd、启动属性与 Perfetto

> 源码版本：Android 11（`android-11.0.0_r48`）  
> 本章目标：给 Android 各种“启动完成”指标建立准确起止点，理解属性、EventLog、bootstat 记录、statsd Atom、广播、logcat 与 Perfetto 的数据来源和边界。  
> 环境：macOS 只读源码为主；adb 命令均为可选验证，不要求编译。

---

## 1. Android 没有唯一的“开机完成时刻”

不同角色关心不同完成点：

```text
bootloader 完成
 → kernel 启动 init
 → /data 可用
 → Zygote/SystemServer 启动
 → 核心系统服务 ready
 → 可以启动第三方应用
 → boot animation 完成
 → PHASE_BOOT_COMPLETED
 → sys.boot_completed=1
 → 某用户解锁
 → 该用户 BOOT_COMPLETED 广播发出
 → 所有接收器处理完
 → Launcher 第一帧/用户可交互
```

这些点可能接近，但不相等。讨论“优化了 500ms”前必须先写清楚测量起点、终点、时钟和用户。

---

## 2. 本章源码地图

```text
frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
frameworks/base/services/core/java/com/android/server/am/UserController.java
frameworks/base/services/java/com/android/server/SystemServer.java
frameworks/base/core/java/com/android/internal/os/ZygoteInit.java
frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
frameworks/base/core/java/com/android/server/BootReceiver.java
system/core/bootstat/bootstat.cpp
system/core/bootstat/boot_event_record_store.cpp
system/core/bootstat/bootstat.rc
system/core/init/init.cpp
system/core/init/service.cpp
system/core/rootdir/init.rc
frameworks/base/cmds/statsd/src/atoms.proto
```

上一章的 `TimingsTraceAndSlog`、Perfetto 和 BinderCallsStats 是本章的细粒度工具。

---

## 3. 先建立“信号—写入者—消费者”表

| 信号 | 写入者 | 表达什么 | 典型消费者 |
|---|---|---|---|
| `ro.boottime.*` | init/service 管理 | init/服务启动时间点或阶段耗时 | bootstat、诊断工具 |
| boot progress EventLog | Zygote/PMS/AMS/WMS | Framework 内部里程碑 | 测试、日志分析 |
| BootTime stats Atom | Framework/bootstat | 结构化启动事件 | statsd/遥测 |
| `PHASE_BOOT_COMPLETED` | AMS→SSM | system_server 内服务最终 boot phase | SystemService |
| `sys.boot_completed=1` | AMS | Framework 主启动完成的全局属性门槛 | init rc、daemon、脚本 |
| `dev.bootcomplete=1` | AMS（受加密状态条件影响） | device boot complete 兼容信号 | vendor/device 组件 |
| `LOCKED_BOOT_COMPLETED` | UserController | 某用户进入 running-locked | direct-boot aware 组件 |
| `BOOT_COMPLETED` | UserController | 某用户已解锁并完成前置流程 | 应用 Receiver |
| bootstat record files | bootstat | 跨启动保存事件数值 | bootstat print/log |
| Perfetto trace | 多进程 tracepoints | 单次完整时间线 | 性能工程师 |

同名 `boot_complete` 出现在不同系统时，必须看具体写入路径。

---

## 4. Android 常用的几种时钟

### Wall clock

真实日期时间，可被网络/用户校准，不适合直接计算启动阶段差值。

### uptime

自 kernel 启动的单调时间，不包含 suspend。Java：`SystemClock.uptimeMillis()`。

### elapsed realtime

自 kernel 启动的单调时间，包含 suspend。Java：`SystemClock.elapsedRealtime()`。

### boot clock

native `CLOCK_BOOTTIME` 语义，包含 suspend。bootstat Android 11 使用 `android::base::boot_clock` 记录事件。

### CPU time

线程/进程真正占用 CPU 的时间，不包含锁/I/O/Binder 等待。

不能把来自不同基准的两个数字直接相减。bootstat README 中“uptime”的通俗描述要结合实现：该版本 `boot_clock` 更接近 boottime/elapsed realtime 语义。

---

## 5. `ro.boottime.*` 从哪里来

init 为自身阶段设置：

```text
ro.boottime.init
ro.boottime.init.first_stage
ro.boottime.init.selinux
ro.boottime.init.cold_boot_wait
```

init 管理的服务第一次启动时，还可产生：

```text
ro.boottime.<service-name>
```

例如 servicemanager、surfaceflinger、zygote 等。

这些属性适合回答“某 daemon 何时开始”或 init 某阶段耗时，不表示该服务内部业务完全 ready。

---

## 6. `ro.boottime.<service>` 不是服务 ready 时间

init `Service::Start()` 记录的是进程启动相关时间点。此后还可能发生：

- linker 和库加载；
-进程 main 初始化；
- Binder 服务注册；
- HAL 硬件初始化；
- 第一次回调；
- 业务数据恢复。

```text
ro.boottime.cameraserver
    ≠ CameraService 已可成功打开摄像头
```

若要测 ready，需要选该服务自己的注册/回调/首个成功请求作为终点。

---

## 7. EventLog 的 boot progress 里程碑

Android 11 在不同组件写 EventLog：

```text
Zygote preload start/end
SystemServer system run
PMS start
PMS system scan start
PMS data scan start
PMS scan end
PMS ready
AMS ready
WMS enable screen
```

它们常以 `boot_progress_*` 标签出现。EventLog 是二进制事件缓冲区，tag 由 event-log-tags 定义，工具显示为结构化字段。

相比普通文本 log，它更适合机器解析里程碑；相比 Perfetto，它缺少完整调度和嵌套执行上下文。

---

## 8. 如何读取 EventLog

有设备时：

```bash
adb logcat -b events -d | grep -E 'boot_progress|boot_completed'
```

或：

```bash
adb shell logcat -b events -v threadtime
```

注意：

- ring buffer 会覆盖旧事件；
- 设备 boot 后太久再导出可能丢失；
- tag 名在不同版本会变化；
- EventLog 时间戳显示格式与事件 payload 中的 uptime 可能同时存在；
- 不要用文本行打印顺序推断跨线程严格 happens-before。

---

## 9. `finishBooting()` 的真实触发门槛

AMS 的 `finishBooting()` 若 boot animation 尚未完成：

```java
if (!mBootAnimationComplete) {
    mCallFinishBooting = true;
    return;
}
```

因此 Framework services ready 并不立刻进入最终 boot completed。它还与 boot animation/Home/屏幕启用等时序协调。

随后 `finishBooting()` 执行多个关键动作，不能把它看成只写一个属性的函数。

---

## 10. finishBooting 的关键顺序

Android 11 源码顺序简化为：

```text
确认 boot animation complete
  → 通知 Zygote/VMRuntime bootCompleted
  → 提交文件系统 checkpoint，失败则重启
  → SSM.startBootPhase(PHASE_BOOT_COMPLETED)
  → 释放 held processes / 安排电源检查
  → userspace reboot 成功记账
  → set sys.boot_completed=1
  → 条件满足时 set dev.bootcomplete=1
  → UserController.sendBootCompleted(...)
  → 启动 profiles / 后续维护
```

所以 phase 1000 早于 `sys.boot_completed=1`；属性又早于每个用户广播接收完成。

---

## 11. `PHASE_BOOT_COMPLETED` 的语义

这是 system_server 内部 SystemService 生命周期里程碑。SSM 顺序调用所有此前已启动服务的：

```java
onBootPhase(PHASE_BOOT_COMPLETED)
```

它不是 Intent，也不按用户发送。服务回调都返回后，SSM 还关闭 SystemServerInitThreadPool。

某个应用 Receiver 无法直接“接收 phase 1000”；它接收的是用户广播。

---

## 12. `sys.boot_completed=1`

由 AMS 在 `finishBooting()` 中写入：

```java
SystemProperties.set("sys.boot_completed", "1");
```

作用：

- 给 init property trigger 提供全局门槛；
- native daemon/脚本可查询；
- bootstat 由 rc 触发记录；
- init 对服务 crash 频率的处理会区分 boot completed 前后；
- userspace reboot 状态机使用它。

它是一个可观察信号，不是“所有 App 的 BOOT_COMPLETED Receiver 已执行完”的 ack。

---

## 13. init property trigger 是异步后续链

`bootstat.rc`：

```rc
on property:sys.boot_completed=1 && property:sys.bootstat.first_boot_completed=0
    exec_background ... bootstat --record_boot_complete ... -l
    setprop sys.bootstat.first_boot_completed 1
```

AMS 设置属性后，property service 通知 init，init 再匹配 action 并执行命令。因此：

```text
属性写成功
  ≠ 所有 on property action 已完成
```

如果测 bootstat 命令完成时间，它会晚于属性变更时间。

---

## 14. `dev.bootcomplete` 与 sys 属性的区别

AMS 在不是特定 FDE restart-min-framework 进度状态时设置：

```java
SystemProperties.set("dev.bootcomplete", "1");
```

它紧随 `sys.boot_completed`，但有加密条件，因此不能假设所有启动场景两者始终同时存在。

设备/vendor 代码可能使用 `dev.bootcomplete`，但 Framework 与 bootstat 的主触发常围绕 `sys.boot_completed`。排查自定义产品时必须搜索本产品 rc/property consumers。

---

## 15. userspace reboot 为什么特殊

userspace reboot 不重启 kernel，许多“自 kernel boot 的时间”不会归零。init 会清理/重置部分 boot-complete 属性，并维护：

```text
sys.init.userspace_reboot.in_progress
```

bootstat rc 用 `sys.bootstat.first_boot_completed` 确保一次完整硬重启只记录一次主要 boot complete，避免 userspace reboot 再次污染完整开机统计。

因此比较 full reboot 和 userspace reboot 的 bootstat/elapsed 数字必须明确基准。

---

## 16. bootstat 是什么

`/system/bin/bootstat` 用于：

- 记录命名 boot event 及相对时间/数值；
- 持久化 boot reason；
- 计算 time since factory reset/last boot 等；
- 打印本地记录；
- 把事件写 EventLog/stats atom 供聚合分析。

它不是持续运行的 daemon，而是由 init rc 或命令按需执行的工具。

---

## 17. bootstat 的存储技巧

目录：

```text
/data/misc/bootstat/
```

每个 event 用一个空/小文件表示，数值存入文件 `mtime`：

```cpp
creat(record_path, ...);
utime(record_path, {.modtime = value});
```

读取时 `stat()` 并取 `st_mtime`。

这是减少文件内容和小文件写入复杂度的实现技巧。看到目录文件大小为 0，不代表没有值；值在 metadata 的修改时间字段中。

---

## 18. bootstat 的默认事件时间

`AddBootEvent(event)`：

```cpp
auto uptime = duration_cast<seconds>(
    android::base::boot_clock::now().time_since_epoch());
```

然后存秒级值。它不是毫秒级 trace 工具；适合宏观启动事件和跨启动持久化。

对于几十毫秒优化，应使用 Perfetto/TimingsTrace；bootstat 秒级记录可能看不出差异。

---

## 19. bootstat `--record_boot_complete`

由 `sys.boot_completed=1` 的 init action 异步执行，记录：

- boot complete；
- 无加密影响版本；
- OTA/factory-reset 相关变体；
- boot reason；
- time since factory reset；
- 并用 `-l` 日志化事件。

因此 bootstat 的 `boot_complete` 接近“bootstat 进程处理属性触发的时刻”，会略晚于 AMS 写属性。它不是 AMS 内部赋值的纳秒级时间戳。

---

## 20. boot reason 的来源链

概念链：

```text
bootloader/kernel cmdline
  → ro.boot.bootreason
  → init/bootstat mirror + normalize
  → sys.boot.reason / persisted record
  → BootReceiver/DropBox/stats telemetry
```

boot reason 字符串由厂商/bootloader 产生，可能不规范。bootstat 包含规范化逻辑，把多种 reason 映射为较稳定类别。

“reboot”不等于原因已知；还要区分用户请求、watchdog、kernel panic、thermal、battery、OTA、userspace reboot 等。

---

## 21. BootReceiver 做什么

`com.android.server.BootReceiver` 位于：

```text
frameworks/base/core/java/com/android/server/BootReceiver.java
```

它在启动后收集/记录上次启动和崩溃线索，例如：

- kernel last_kmsg/pstore；
- recovery logs；
- tombstone/系统崩溃相关信息；
- boot reason；
- filesystem shutdown/修复信息；
- DropBox 条目。

BootReceiver 是启动后取证与日志归档者，不是决定 system_server “可以启动 App”的核心状态机。

---

## 22. DropBox 与 EventLog 的区别

| DropBox | EventLog |
|---|---|
| 可保存较大文本/文件/压缩内容 | 小型结构化事件 |
| 用 tag 分类 ANR、crash、boot 等 | 用数值 tag/schema |
| 可持久化较长时间并限额清理 | logd ring buffer 易覆盖 |
| 适合事后取证 | 适合里程碑/计数流 |

BootReceiver 把 pstore 等材料送 DropBox；boot progress 则常写 EventLog。不要在 EventLog 中期待完整 kernel panic 文本。

---

## 23. statsd BootTime Atom

Framework 通过生成的 `FrameworkStatsLog.write()` 写 `BOOT_TIME_EVENT_ELAPSED_TIME_REPORTED` 等 Atom，事件类型标明：

- SystemServer init start/ready；
- PMS init start/ready；
- Framework locked boot completed；
- Framework boot completed；
- 其他 bootstat 映射事件。

Atom 包含事件枚举和 elapsed time，适合跨设备/版本聚合 P50/P95。

第 73 章已讲过：Atom 是事实输入，Metric 才是 statsd 按配置聚合的结果。

---

## 24. 为什么过滤 first boot、upgrade、runtime restart

源码在上报某些稳定 boot-time Atom 前检查：

```text
不是 secondary user
不是 runtime restart
不是 first boot / upgrade
```

原因是这些场景工作量不同：

- 首次启动建立数据和默认状态；
- OTA 后升级扫描、迁移、dexopt；
- runtime restart 不包含 kernel/native 全流程；
- secondary user 启动不是设备开机。

若混入同一分布，指标波动会掩盖真实回归。

---

## 25. `LOCKED_BOOT_COMPLETED` 是每用户事件

UserController 把用户从 BOOTING 推到 RUNNING_LOCKED 后发送：

```java
Intent.ACTION_LOCKED_BOOT_COMPLETED
```

仅 direct-boot aware 组件能在用户 CE 尚未解锁时依赖 DE 数据运行。

它不是系统全局只发一次：每个真实用户在相应生命周期中都可收到。系统用户、当前用户、managed profile 时序也可能不同。

---

## 26. `BOOT_COMPLETED` 广播为什么更晚

用户需要：

```text
key unlocked
 → RUNNING_UNLOCKING
 → SystemService onUserUnlocking
 → RUNNING_UNLOCKED
 → USER_UNLOCKED
 → 必要时 PRE_BOOT_COMPLETED receivers 完成
 → 用户初始化/widgets 等
 → BOOT_COMPLETED
```

OTA/Build fingerprint 变化时，PRE_BOOT 接收器故意阻塞 BOOT_COMPLETED，避免应用升级初始化与正常 boot receiver 竞态。

这解释了为何属性已经是 1，某应用仍晚一段时间才收到广播。

---

## 27. BOOT_COMPLETED 的发送线程和完成点

UserController 为保证与 widget 广播顺序，把发送动作 post 到 FgThread，再发送有序广播。最终 result receiver 执行时记录：

```text
Finished processing BOOT_COMPLETED for uX
```

并设置内部 `mBootCompleted=true`。

至少有三个点：

```text
Posting BOOT_COMPLETED
 → BroadcastQueue 开始/逐 Receiver 投递
 → Finished processing BOOT_COMPLETED
```

日志中的“Posting”不是全部接收器完成。

---

## 28. Broadcast offload 不等于无成本

BOOT_COMPLETED 使用 offload 标志/队列以避免长广播直接阻塞常规广播路径，但接收器仍可能：

- 冷启动应用进程；
- 做 I/O；
- 安排 Job；
- 超时/ANR；
- 与其他启动后任务争抢 CPU/存储。

因此用户“桌面出来后仍卡”可能是 boot receivers storm，而不是 system_server services init 慢。

---

## 29. Launcher 第一帧与 boot completed

Launcher 可在最终广播处理完之前显示；广播也可能在 Launcher 已有画面后继续执行。

用户体验指标可选：

- boot animation 退出；
- keyguard 可交互；
- Launcher window drawn；
- Launcher first frame presented；
- input 首次成功响应；
- 所有 boot receivers 完成。

它们回答不同问题。做产品 KPI 时应使用最接近用户可用性的终点，同时保留 Framework 内部里程碑帮助归因。

---

## 30. Perfetto 提供什么

Perfetto 可统一观察：

- kernel scheduling/CPU frequency；
- process/thread lifecycle；
- atrace slices（SystemServerTiming 等）；
- Binder transaction flows；
- ftrace/I/O；
- ART GC；
- SurfaceFlinger/frame timeline（配置和版本允许时）；
-自定义 trace counters。

它最适合回答“这一次启动为何在某两里程碑间慢”，而 statsd 更适合回答“很多设备上是否发生回归”。

---

## 31. logcat、Perfetto、statsd 的组合

```text
statsd/bootstat：发现跨设备 P95 回归
  ↓
EventLog/logcat：确认哪个宏观阶段推迟
  ↓
Perfetto：定位单次线程/IPC/锁/I/O 根因
  ↓
源码：证明调用链和依赖
  ↓
修改后重复测量 + stats distribution 验证
```

不要只用单台设备一条 logcat 推断整个产品分布，也不要只凭聚合 Atom 猜具体锁。

---

## 32. 一张完整启动观测时间线

```text
Bootloader                ro.boot.bootreason / vendor timing
    │
Kernel start              boot/elapsed time origin
    │
init first stage          ro.boottime.init.first_stage
    │
SELinux / second stage    ro.boottime.init.selinux
    │
zygote preload            boot_progress_preload_*
    │
SystemServer.run          boot_progress_system_run + Atom
    │
PMS scan                  boot_progress_pms_* + trace
    │
AMS ready / enable screen boot_progress_* + trace
    │
boot animation complete
    │
PHASE_BOOT_COMPLETED      SystemService callback
    │
sys.boot_completed=1      property + init triggers
    ├─ bootstat record/log
    └─ per-user flow
         ├─ LOCKED_BOOT_COMPLETED
         ├─ user unlock / PRE_BOOT
         └─ BOOT_COMPLETED posting → receivers finished
```

---

## 33. 一次启动数据的采集清单

有 userdebug 设备时可选：

```bash
adb shell getprop ro.boottime.init
adb shell getprop sys.boot_completed
adb shell getprop dev.bootcomplete
adb shell bootstat -p
adb logcat -b events -d
adb logcat -b system -b main -d
adb shell dumpsys dropbox --print SYSTEM_BOOT
```

命令权限和 tag 名依设备而异。Perfetto 最好在启动前配置并由 init/host 触发，开机后手动开始无法捕获早期阶段。

---

## 34. `adb wait-for-device` 不是 boot complete

它通常只等待 adb transport/device 可连接。此时：

- system_server 可能仍启动；
- PackageManager 未 ready；
- 用户未解锁；
- Launcher 未显示。

自动化脚本常用：

```bash
adb wait-for-device
adb shell 'until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 1; done'
```

但这仍只等属性。若测试依赖用户解锁、广播处理、Launcher idle，应增加对应明确门槛，而不是任意 sleep 10 秒。

---

## 35. 为什么固定 sleep 是坏指标

```text
sleep 30
```

既可能过长浪费时间，也可能慢设备不够；无法告诉你卡在哪一阶段。

应等待有语义的条件：

- property；
- ActivityManager user state；
- Window drawn；
- package service available；
-特定 broadcast/result；
- trace event。

等待还应有超时，并在超时时自动采集 logcat、properties、process stacks 和 trace。

---

## 36. bootstat 文件持久化的注意点

- 目录在 `/data`，依赖 post-fs-data。
- 文件 mtime 被当数值，普通工具修改时间可能破坏记录。
- OTA 会有 DAC 兼容 chown。
- event 名成为文件名，需要受控输入。
- 秒级粒度不适合微优化。
- full reboot/userspace reboot 状态机避免重复主记录。

不要把目录直接当普通文本日志编辑。

---

## 37. 启动指标回归分析

发现 P95 增长后：

1. 按设备、版本、boot reason、first boot/OTA/runtime restart 分桶。
2. 找最早开始偏移的里程碑。
3. 判断偏移发生在 bootloader/kernel/init/zygote/system_server/user/app。
4. 对同场景采 Perfetto。
5. 找关键路径和根因变更。
6. 验证修改没有只移动终点/日志。
7. 回看 P50/P95 和用户可交互指标。

后续里程碑都统一晚 500ms，根因通常在更早第一个偏移点，而不是每个后续组件都慢。

---

## 38. 常见误区纠正

### 误区 1：`sys.boot_completed=1` 表示所有应用启动完成

错误。它在 BOOT_COMPLETED 广播分发前设置。

### 误区 2：phase 1000 就是 BOOT_COMPLETED Intent

错误。前者 system_server 内部、非按用户；后者每用户广播。

### 误区 3：`ro.boottime.service` 是服务 ready

错误。主要表示 init 启动进程的时点。

### 误区 4：bootstat 是常驻 daemon

错误。它是被 rc/命令按需执行的工具。

### 误区 5：bootstat 空文件没有记录

错误。数值存于 mtime。

### 误区 6：EventLog 与普通 logcat 完全相同

错误。EventLog 有 tag/schema 和独立 events buffer。

### 误区 7：Launcher 显示一定晚于所有 BOOT_COMPLETED receiver

错误。两条链可重叠。

### 误区 8：`adb wait-for-device` 等于开机完成

错误。只表示 adb transport 可用。

### 误区 9：不同启动场景可混合求平均

错误。首次/OTA/runtime/userspace reboot 工作量和时间基准不同。

### 误区 10：单一工具可以解释全部启动问题

错误。聚合指标、事件、日志、trace、源码各有粒度。

---

## 39. 第二遍复读：六个同名完成点

### 39.1 Framework phase complete

SSM 已向 SystemService 分发 phase 1000 并等待同步回调返回。

### 39.2 Property complete

AMS 已写 `sys.boot_completed=1`，但 init property actions 可能刚被触发。

### 39.3 bootstat record complete

bootstat 进程已在属性之后运行并把 event 写入 mtime/日志。

### 39.4 user locked boot complete

特定用户到 RUNNING_LOCKED，direct-boot aware receiver 可运行，CE 仍可能锁定。

### 39.5 user boot broadcast posted

UserController 已把 BOOT_COMPLETED 放入广播系统，不等于所有 receiver 结束。

### 39.6 user boot broadcast finished

有序/offload 广播的最终 result receiver 已执行；仍不保证所有应用后台 Job、网络同步和 UI 工作完成。

---

## 40. Mac 只读源码练习

### 练习 1：标 finishBooting 顺序

```bash
sed -n '5460,5570p' \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

给 phase、属性、用户广播标序号。

### 练习 2：追属性消费者

```bash
rg -n "property:sys.boot_completed=1|sys\.boot_completed" \
  system/core frameworks/base | head -100
```

区分写入者、init trigger 和只读查询者。

### 练习 3：理解 bootstat mtime

```bash
sed -n '20,120p' system/core/bootstat/boot_event_record_store.cpp
sed -n '50,90p' system/core/bootstat/bootstat.rc
```

画出 property → init → bootstat → record file/EventLog。

### 练习 4：追每用户广播

```bash
rg -n "LOCKED_BOOT_COMPLETED|Posting BOOT_COMPLETED|Finished processing BOOT_COMPLETED|PRE_BOOT" \
  frameworks/base/services/core/java/com/android/server/am/UserController.java
```

说明锁定用户与已解锁用户的差别。

### 练习 5：列 EventLog/Atom 里程碑

```bash
rg -n "BOOT_PROGRESS|BOOT_TIME_EVENT_ELAPSED_TIME_REPORTED" \
  frameworks/base/services frameworks/base/core/java/com/android/internal/os
```

为每个事件记录时钟、过滤条件和代码位置。

---

## 41. 自测题

1. 为什么不存在唯一 boot complete？
2. phase 1000 与 sys.boot_completed 谁先？
3. 属性设为 1 后 bootstat 是否已经记录完成？
4. dev.bootcomplete 为什么可能不同时设置？
5. bootstat 记录值存在哪里？
6. ro.boottime.servicemanager 表示什么、不表示什么？
7. LOCKED_BOOT_COMPLETED 与 BOOT_COMPLETED 差异是什么？
8. Posting BOOT_COMPLETED 是否等于接收完成？
9. stats Atom 与 Perfetto 各适合什么？
10. 为什么过滤 first boot/OTA/runtime restart？
11. adb wait-for-device 能否作为 Launcher ready 指标？
12. bootstat README 的 uptime 为什么要结合实现理解？

---

## 42. 参考答案

1. 各层、各用户和用户体验有不同里程碑。
2. Android 11 finishBooting 中 phase 1000 先完成，再写属性。
3. 不一定；init 后续异步执行 bootstat。
4. FDE/restart-min-framework 加密进度条件会限制它。
5. `/data/misc/bootstat/<event>` 文件的 mtime。
6. 表示 init 启动该服务进程的时间线索，不保证 Binder/业务 ready。
7. 前者在用户 running-locked、DE 可用；后者要求用户解锁并完成 PRE_BOOT 等前置。
8. 不等于，最终 result receiver 才表示广播链处理结束。
9. Atom 做跨设备聚合；Perfetto 解释单次详细线程/IPC/I/O 根因。
10. 工作量和时间基准不同，混合会污染统计。
11. 不能，只说明 adb 连接可用。
12. Android 11 实现用 native boot_clock，语义包含 suspend，更接近 elapsed/boottime。

---

## 43. 本章总结

```text
早期启动：ro.boottime + boot progress EventLog
Framework：TimingsTrace + BootTime Atom + BootPhase
全局门槛：sys.boot_completed 属性 → init actions
持久记录：bootstat mtime files + EventLog/stats logging
每用户：LOCKED_BOOT_COMPLETED → unlock/PRE_BOOT → BOOT_COMPLETED
用户体验：boot animation、window drawn、first frame、input ready
根因定位：Perfetto + logs + stacks + source
```

任何启动数字都必须带四个标签：

```text
起点、终点、时钟、场景（full/first/OTA/runtime/user）
```

---

## 44. 下一章预告

第 100 章将做阶段综合实战：

**新增一个可观测、可恢复的 Framework SystemService：从启动、Binder、Manager、线程与 SELinux 到诊断闭环**

不实际编译，而是基于 Android 11 源码完成一套可用于真实项目评审的设计与只读追踪文档，把第 85～99 章串成端到端方法。
