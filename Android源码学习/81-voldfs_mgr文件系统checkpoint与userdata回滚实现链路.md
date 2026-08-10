# 81 vold、fs_mgr、文件系统 checkpoint 与 userdata 回滚实现链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求真实挂载或触发 checkpoint  
> 前置章节：22 存储/FBE、77 SystemProperties、80 APEX staged install

---

## 1. 本章解决什么问题

上一章中，StagingManager 在重启前调用：

```java
storageManager.startCheckpoint(2);
```

启动成功时 AMS 调用：

```java
storageManager.commitChanges();
```

失败时又可能调用：

```java
storageManager.abortChanges(reason, retry);
```

这些 Java API 背后不是一个简单“复制 /data”的程序，而是一组跨越 Framework、vold、fs_mgr、fstab、文件系统、device-mapper、BootControl 和密钥管理的协议。

目标是：

```text
让新系统/新 APEX 有机会修改 userdata 并试启动
  → 成功：接受这些修改
  → 失败：回到修改前的一致状态
```

关键词是“一致状态”，而不是逐个文件撤销。

---

## 2. checkpoint 与普通备份不同

普通备份通常把文件复制到另一个位置，稍后按文件恢复。checkpoint 更像一个事务：

```text
BEGIN
  → 允许大量文件系统写入
  → COMMIT：承认新写入
  或
  → ABORT/ROLLBACK：让旧视图重新成为权威
```

它保护的是被 fstab 标记支持 checkpoint 的文件系统/块设备，通常重点是 userdata。它不会自动替代：

- RollbackManager 的指定 App DE/CE snapshot；
- apexd 在无 checkpoint 设备上的 active APEX backup；
- OTA A/B slot 中 system/vendor 等只读分区切换；
- 云端或用户数据备份。

---

## 3. 源码地图

| 文件 | 作用 |
|---|---|
| `frameworks/base/services/core/java/com/android/server/StorageManagerService.java` | Java Binder API、权限和 vold 转发 |
| `system/vold/VoldNativeService.cpp` | IVold Binder 入口 |
| `system/vold/Checkpoint.cpp` | checkpoint 状态、重试、提交、放弃、health daemon |
| `system/vold/Checkpoint.h` | native 接口 |
| `system/vold/vdc.cpp` | init/shell 到 vold 的命令行桥 |
| `system/core/fs_mgr/fs_mgr_fstab.cpp` | 解析 `checkpoint=fs/block` |
| `system/core/fs_mgr/fs_mgr.cpp` | 挂载时建立 F2FS checkpoint 或 dm-bow |
| `system/core/fs_mgr/libdm/.../dm_target.h` | dm-bow target 描述 |
| `system/core/rootdir/init.rc` | markBootAttempt、prepareCheckpoint 的启动时序 |
| `system/vold/MetadataCrypt.cpp` | metadata encryption 与 checkpoint 设备栈 |
| `system/vold/KeyStorage.cpp` | checkpoint 期间延迟销毁旧 Keymaster key |
| `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java` | finishBooting 时 commitChanges |

---

## 4. 从 Java 到 native 的调用链

以 `startCheckpoint(2)` 为例：

```text
StagingManager
  → IStorageManager.startCheckpoint(2)
  → StorageManagerService 权限检查
  → IVold.startCheckpoint(2)
  → VoldNativeService::startCheckpoint
  → cp_startCheckpoint(2)
  → 写 /metadata/vold/checkpoint
```

其他接口同理：

| Java/IVold | Checkpoint.cpp |
|---|---|
| `supportsCheckpoint()` | `cp_supportsCheckpoint()` |
| `startCheckpoint(n)` | `cp_startCheckpoint(n)` |
| `needsCheckpoint()` | `cp_needsCheckpoint()` |
| `needsRollback()` | `cp_needsRollback()` |
| `isCheckpointing()` | `cp_isCheckpointing()` |
| `commitChanges()` | `cp_commitChanges()` |
| `abortChanges()` | `cp_abortChanges()` |
| `markBootAttempt()` | `cp_markBootAttempt()` |
| `prepareCheckpoint()` | `cp_prepareCheckpoint()` |

StorageManagerService 不是 checkpoint 算法实现者，主要负责 Java 系统服务接口和调用权限。

---

## 5. 设备是否支持 checkpoint 由 fstab 决定

fs_mgr 解析两种 flag：

```text
checkpoint=fs
checkpoint=block
```

源码映射为：

- `fs_mgr_flags.checkpoint_fs`；
- `fs_mgr_flags.checkpoint_blk`。

`cp_supportsCheckpoint()` 遍历默认 fstab，只要存在任一标记就返回 true。

所以“Android 11 支持 checkpoint”不表示每台 Android 11 设备都启用。必须看该设备的 fstab、文件系统类型、内核/device-mapper 能力和厂商配置。

---

## 6. 两种实现路径总览

| 模式 | fstab | 主要实现 | 保护层级 |
|---|---|---|---|
| filesystem checkpoint | `checkpoint=fs` | Android 11 主要是 F2FS 原生 checkpoint disable/enable | 文件系统内部 |
| block checkpoint | `checkpoint=block` | dm-bow（backup-on-write） | 文件系统下方块层 |

两者都向上提供同一套 vold API，但底层原理不同：

```text
F2FS：
文件系统自己保留可回退的 checkpoint 语义

dm-bow：
原块第一次被覆盖前，把旧内容搬到保留区域并记映射
```

“checkpoint”是统一能力名，不代表底下一定使用 snapshot 分区。

---

## 7. startCheckpoint(2) 到底写了什么

`cp_startCheckpoint(retry)` 首先确认支持，然后写：

```text
/metadata/vold/checkpoint
```

普通参数的内容是：

```cpp
content = std::to_string(retry + 1);
```

因此传入 2 时初始写入 3，而不是 2。

为什么加一？因为启动流程中的 `markBootAttempt` 会在本次/后续启动扣减。计数文件需要覆盖“进入 checkpoint 的启动”以及允许的重试语义。

不要只根据文件中的数字直译“还允许几次”。正确分析要结合：

- startCheckpoint 写入时的 +1；
- init 每次何时 markBootAttempt；
- 当前是否已扣减；
- 0 代表 needs rollback。

参数 `-1` 是特殊模式，还会记录当前 A/B slot suffix，用于与 BootControl slot 状态联合判断，不是 staged APEX 常用的 `2` 路径。

---

## 8. 为什么状态文件放在 /metadata

checkpoint 保护目标往往是 `/data`。如果“是否应回滚”的唯一状态也放进正在被回滚的 `/data`，启动早期会出现循环依赖：

```text
要决定怎样挂载 /data
  → 先读取 checkpoint 状态
状态却在尚未确定视图的 /data
```

`/metadata` 在 userdata 之前可用，适合保存少量跨重启协调状态。

这份文件不是用户数据快照本体，只是计数/slot 协议标记。

---

## 9. init 如何参与每次启动计数

`init.rc` 在 `on post-fs` 执行：

```rc
exec - system system -- /system/bin/vdc checkpoint markBootAttempt
```

`vdc` 调用 vold `cp_markBootAttempt()`：

1. checkpoint 文件不存在则直接成功；
2. 读取最前面的整数；
3. 大于 0 就减一；
4. 写回新值。

例如简化为：

```text
startCheckpoint(2) 写 3
第一次启动 markBootAttempt：3 → 2
再次失败重启：2 → 1
再次失败重启：1 → 0
下次早期查询 needsRollback：true
```

具体“第几次被判失败”要以实际触发时序为准，但 0 的含义明确：不再继续 checkpoint 尝试，应回到安全状态。

---

## 10. needsCheckpoint 与 needsRollback 不一样

### `cp_needsCheckpoint()`

回答：“本次启动是否应以 checkpoint 模式挂载/运行？”

判断来源包括：

- 当前 BootControl slot 尚未标 successful；
- `/metadata/vold/checkpoint` 存在且内容不是 0。

它第一次调用后用 `needsCheckpointWasCalled` 和 `isCheckpointing` 缓存结果。源码注释强调只应在 boot 期间确定一次，避免启动中途状态文件变化导致不同组件对挂载模式得出矛盾结论。

### `cp_needsRollback()`

回答：“重试是否耗尽，或者特殊 slot 条件是否要求回滚？”

普通计数文件内容为 `0` 时返回 true。

所以：

```text
needsCheckpoint=true
  → 继续试运行新状态

needsRollback=true
  → 不应继续试，开始回到旧状态
```

两者不是互为简单取反。

---

## 11. fs_mgr 在挂载前做什么

fs_mgr 的 `CheckpointManager` 在处理带 checkpoint flag 的 fstab entry 时：

1. 通过 `vdc checkpoint needsCheckpoint` 查询；
2. 若不需要 checkpoint，按普通块设备挂载；
3. 若需要，调用 `UpdateCheckpointPartition()`；
4. 根据 fs/block 模式改写挂载选项或块设备路径。

这一步必须发生在真正挂载和大量写入之前，否则新启动已经覆盖旧数据，再建立保护就太晚了。

---

## 12. F2FS filesystem checkpoint

对于：

```text
checkpoint=fs
fs_type=f2fs
```

fs_mgr 加入：

```text
checkpoint=disable
```

作为 F2FS 挂载选项。

直观理解：

```text
旧的稳定 checkpoint
  → 新启动继续产生写入
  → 暂不把这些写入确认为新的不可回退基线
```

成功提交时 `cp_commitChanges()` 遍历 `/proc/mounts`，对该 F2FS remount：

```text
checkpoint=enable
```

让文件系统恢复正常 checkpoint，并接受新状态。

Android 11 这条 `checkpoint=fs` 路径明确检查 F2FS；其他文件系统若标记却未实现，会记录错误。

---

## 13. dm-bow block checkpoint

`checkpoint=block` 时，fs_mgr 在实际块设备上创建名为 `bow` 的 device-mapper target：

```text
真实 userdata block device
  ↓
dm-bow（backup on write）
  ↓
可能的 dm-default-key/metadata encryption 层
  ↓
文件系统
```

具体堆叠顺序会随 metadata encryption 配置变化。源码特别处理 dm-bow block size 与 dm-default-key data unit 对齐，避免 I/O 被拆成不合法大小。

### 13.1 backup-on-write 的核心

当上层第一次要覆盖某个旧块：

1. 先把旧块搬到尚未使用的空间；
2. 写一条 source → backup destination 映射日志；
3. 再允许新数据写入原逻辑位置。

于是：

```text
提交：旧备份不再需要，接受新块
回滚：根据映射把旧块恢复到原位置
```

它不是 copy-on-write 新块旁路模型的同义词；这里强调“保存将被覆盖的旧内容”。

---

## 14. dm-bow 状态 1 和 2

`cp_prepareCheckpoint()` 对 block checkpoint：

1. 对挂载点执行 FITRIM，尽量准备空闲块；
2. 写 dm-bow sysfs `state=1`，进入 checkpoint 保护；
3. 启动空间 health daemon。

成功提交时：

```text
state=2
```

让 bow 接受/完成新状态。

这些数字是内核 dm-bow 状态协议，不应脱离源码猜成通用 Android session 状态；它们与 PackageInstaller READY/APPLIED 完全无关。

---

## 15. prepareCheckpoint 为什么在 post-fs-data 开头

`init.rc`：

```rc
on post-fs-data
    exec - system system -- /system/bin/vdc checkpoint prepareCheckpoint
```

并且注释是：

```text
Start checkpoint before we touch data
```

随后才进行 `/data` 权限、restorecon、installkey 等操作。

挂载层保护由 fs_mgr 更早建立，而 `prepareCheckpoint` 把 dm-bow 切入正式记录状态、启动健康监控。二者共同保证“业务和系统服务开始修改 data 前”保护已经就绪。

---

## 16. 空间不足为什么危险

dm-bow 需要空闲块保存被覆盖的旧数据；F2FS checkpoint 模式也可能积累额外空间压力。若保护区耗尽：

- 无处保存旧块；
- 新写入不能安全继续；
- 回滚能力可能被破坏；
- 整机可能因 userdata 写失败而崩溃。

`cp_healthDaemon` 周期检查 free space，默认阈值约 100 MiB。由属性控制：

- `ro.sys.cp_msleeptime`；
- `ro.sys.cp_min_free_bytes`；
- `ro.sys.cp_commit_on_full`。

空间过低时：

```text
commit_on_full=true
  → 提前 commit，牺牲回滚能力以保证继续写

commit_on_full=false
  → abortChanges 并重启，优先保住回滚能力
```

这是“可恢复性”和“设备继续运行”之间的工程权衡。

---

## 17. commitChanges 的完整动作

AMS `finishBooting()` 中：

```text
About to commit checkpoint
  → IStorageManager.commitChanges
  → vold cp_commitChanges
```

vold：

1. 若当前不在 checkpoint，幂等返回成功；
2. 测试属性 `persist.vold.dont_commit_checkpoint=1` 时故意不提交；
3. 调用 BootControl `markBootSuccessful`；
4. 清除 `ota.warm_reset`；
5. 遍历已挂载文件系统；
6. F2FS：remount checkpoint=enable；
7. dm-bow：设置 state=2；
8. 设置 `vold.checkpoint_committed=1`；
9. `isCheckpointing=false`；
10. 删除 `/metadata/vold/checkpoint`。

因此“删除 checkpoint 文件”只是最后的协议清理，不是 commit 的全部工作。

---

## 18. 为什么还要标 A/B slot successful

checkpoint 常与 A/B OTA 或 staged 更新共存。新 slot 未确认 successful 时，BootControl 本身也代表“本次启动仍处于试运行”。

`cp_needsCheckpoint()` 会把当前 slot 未 successful 视为 checkpointing；`cp_commitChanges()` 同时调用 `markBootSuccessful()`。

这样：

```text
userdata 新状态确认
  ↔ 当前系统 slot 确认成功
```

尽量在同一个成功点收口，避免 data 已承认新格式，而 bootloader 却切回旧 slot。

但 filesystem checkpoint 与 A/B slot 仍是两套机制：一个保护可写数据，一个选择启动系统镜像。

---

## 19. abortChanges 为什么主要是重启请求

`cp_abortChanges(message, retry)`：

```cpp
if (!cp_needsCheckpoint()) return;
if (!retry) abort_metadata_file();
android_reboot(..., message);
```

它没有在当前正在运行的系统里逐文件倒带。原因是当前进程、文件描述符、mmap 和缓存都建立在新数据视图上，在线恢复会产生严重不一致。

正确策略是：

```text
修改 metadata 计数/回滚意图
  → 带原因重启
  → 下一次启动在挂载前恢复旧视图
```

若 `retry=false`，`abort_metadata_file()` 把正数计数改成 0，迫使下次走 rollback；`retry=true` 则保留继续尝试的可能。

还有一个容易困惑的细节：当前进程中的 `isCheckpointing` 不会因为文件刚被改成 0 就立刻变成 false。这个 boot 已经按 checkpoint 视图挂载，不能在线改口；0 是给“下一次启动早期”的 `needsRollback()` 读取的。这正是 abort 必须紧接重启的原因。

---

## 20. block checkpoint 的恢复发生在哪里

fs_mgr 在建立新 checkpoint 前，对 block 模式可能调用：

```text
vdc checkpoint restoreCheckpoint <block_device>
```

vold 的恢复逻辑读取 dm-bow 写在块设备中的日志：

- 校验 magic、版本、block size、sequence、checksum；
- 找出 source → destination relocation；
- 先验证可恢复性；
- 再按映射把旧块写回；
- 对中断恢复还有 partial restore 标记。

这也是为什么恢复必须在文件系统正常读写之前完成。若恢复一半突然断电，日志协议需要在下一次启动识别并续作，而不能把半恢复卷当成健康文件系统。

---

## 21. F2FS 回退与 block 回退不要混讲

| 问题 | F2FS fs checkpoint | dm-bow block checkpoint |
|---|---|---|
| 谁理解旧/新状态 | F2FS | dm-bow target + vold 日志 |
| fstab flag | checkpoint=fs | checkpoint=block |
| 启动时设置 | mount checkpoint=disable | 建 dm-bow device |
| 准备 | health daemon | FITRIM + state=1 + health daemon |
| 提交 | remount checkpoint=enable | bow state=2 |
| 回退 | 文件系统 checkpoint 语义 | 根据 relocation log 恢复块 |

上层 API 相同不代表底层日志格式相同。

---

## 22. metadata encryption 与设备栈

metadata encryption 常用 `dm-default-key` 把 userdata 块设备加密。checkpoint block target 必须放在正确层级：

```text
文件系统 I/O
  → dm-default-key（加密/解密）
  → dm-bow（保护底层被覆盖块）
  → 真实块设备
```

源码也会依据设备首发版本和 `ro.crypto.dm_default_key.options_format.version` 调整 dm-bow block size。

理解关键不是死背每台设备的 dm 层顺序，而是追踪“bow 保存的是哪一层看到的块”以及 block size/data unit 是否一致。

---

## 23. checkpoint 为什么影响 Keymaster key 删除

系统更新过程中，Keymaster key blob 可能需要升级。若立刻删除旧 key：

```text
新启动升级 key 并删除旧 key
  → 随后 userdata checkpoint 回滚
  → 文件系统回到仍引用旧 key 的状态
  → 旧 key 已不存在
  → 数据永久无法解密
```

`KeyStorage.cpp` 在 `cp_needsCheckpoint()==true` 时延迟删除旧 key，后台等待：

```text
vold.checkpoint_committed=1
```

只有 checkpoint 成功提交才真正删除旧 key。

这说明文件块可回滚还不够，访问这些块所需的密钥生命周期也必须与事务一致。

---

## 24. checkpoint 与 FBE DE/CE 的关系

FBE 把用户数据分为 DE/CE，但 filesystem/block checkpoint 保护的是底层被标记卷的写入一致性。两者是不同维度：

| FBE | checkpoint |
|---|---|
| 决定谁在何时能解密哪些文件 | 决定新写入是否提交或回退 |
| key 受用户解锁状态影响 | checkpoint 状态在启动早期决定 |
| DE 早期可用，CE 解锁后可用 | 可保护承载 DE/CE 的 userdata 块/文件系统 |

RollbackManager 仍需要 per-user DE/CE app snapshot，因为它可能在 14 天观察期后只回滚某个包；filesystem checkpoint 只覆盖当前 staged 启动事务窗口。

---

## 25. 与 APEX staged install 的完整接力

```text
重启前：
StagingManager pre-reboot verification
  → startCheckpoint(2)
  → /metadata/vold/checkpoint = 3
  → Framework READY + apexd STAGED
  → reboot

挂载期：
init early-fs 启动 vold
  → fs_mgr/vold 判断 needsCheckpoint
  → F2FS checkpoint=disable 或建立 dm-bow
  → init post-fs markBootAttempt
  → init post-fs-data prepareCheckpoint
  → apexd 激活新 APEX
  → system_server 安装同 train APK
  → staged session APPLIED

成功：
AMS finishBooting
  → commitChanges
  → BootControl slot successful
  → F2FS enable 或 bow state=2
  → checkpoint_committed=1
  → 删除 metadata checkpoint 文件
  → PHASE_BOOT_COMPLETED
  → apexd session SUCCESS

失败：
关键服务/安装失败
  → abortChanges(..., retry=false) 或重试耗尽
  → metadata 计数变 0
  → reboot
  → 启动挂载前恢复旧 filesystem/block 视图
  → apexd revert staged APEX
  → Framework session FAILED
```

---

## 26. 三种“成功”也要分开

| 成功点 | 证明什么 |
|---|---|
| filesystem checkpoint commit | 本次 userdata 写入被接受，旧视图不再保留 |
| BootControl markBootSuccessful | 当前 A/B slot 被确认可启动 |
| apexd markStagedSessionSuccessful | APEX staged session 已通过启动观察，可清理 APEX 保护材料 |

Android 11 在相近的 boot 完成阶段完成它们，但它们属于不同状态机。任一步失败都不能用另一步的日志替代证明。

---

## 27. 权限与安全边界

StorageManagerService 限制：

- startCheckpoint：root、system_server、shell；
- commitChanges：仅 system process；
- needsCheckpoint 等要求存储相关权限。

vold 运行在高权限 native daemon 中，能操作块设备、mount、reboot 和 metadata 状态。普通应用不能通过公开 API 开启事务或强迫设备回退。

测试属性 `persist.vold.dont_commit_checkpoint` 也不是业务开关，错误启用会让设备长期保留 checkpoint 状态和额外空间压力。

---

## 28. 常见误解复读

### 误解一：startCheckpoint(2) 会把数字 2 写进 metadata

错误。Android 11 写 `retry + 1`，即 3。

### 误解二：needsRollback 等于 !needsCheckpoint

错误。前者表示重试耗尽/特殊 slot 回退条件，后者表示本次是否运行在 checkpoint 模式。

### 误解三：abortChanges 当场恢复 /data

错误。它更新协议状态并重启，恢复在下一次安全挂载阶段完成。

### 误解四：所有 checkpoint 都由 dm-bow 实现

错误。fstab 可选择 F2FS filesystem checkpoint 或 dm-bow block checkpoint。

### 误解五：commit 只是删掉 /metadata/vold/checkpoint

错误。它还确认 slot、切换 F2FS/bow 状态、发布 committed property。

### 误解六：空间不足最多导致 checkpoint 慢

错误。旧块保护区耗尽会破坏继续写或回退能力，因此 health daemon 可能提前 commit 或立即 abort/reboot。

### 误解七：块恢复成功就一定能解密数据

错误。旧 Keymaster key 也必须保留到事务提交，所以 KeyStorage 会延迟删除。

---

## 29. Mac 上的六轮只读练习

### 第一轮：追 Java 到 vold

```bash
sed -n '3120,3195p'   frameworks/base/services/core/java/com/android/server/StorageManagerService.java
sed -n '780,875p' system/vold/VoldNativeService.cpp
```

### 第二轮：追 metadata 计数

```bash
sed -n '85,150p' system/vold/Checkpoint.cpp
sed -n '235,305p' system/vold/Checkpoint.cpp
sed -n '710,770p' system/vold/Checkpoint.cpp
```

手算 `startCheckpoint(2)` 的计数变化。

### 第三轮：追 fs_mgr 挂载

```bash
sed -n '1030,1170p' system/core/fs_mgr/fs_mgr.cpp
rg -n "checkpoint=block|checkpoint=fs"   system/core/fs_mgr/fs_mgr_fstab.cpp
```

### 第四轮：比较提交路径

```bash
sed -n '150,225p' system/vold/Checkpoint.cpp
```

分别标记 F2FS 和 dm-bow 动作。

### 第五轮：追启动时序

```bash
rg -n "checkpoint markBootAttempt|checkpoint prepareCheckpoint"   system/core/rootdir/init.rc
rg -n "commitChanges"   frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
```

### 第六轮：追密钥事务

```bash
rg -n "needsCheckpoint|checkpoint_committed|Defer.*Key|commit_key" \
  system/vold/KeyStorage.cpp \
  system/vold/MetadataCrypt.cpp
```

---

## 30. 自测题

1. checkpoint 为什么是事务而不是普通文件备份？
2. `checkpoint=fs` 与 `checkpoint=block` 谁解析？
3. startCheckpoint(2) 为什么初始写 3？
4. metadata 文件为什么不能放在 /data？
5. markBootAttempt 在哪个 init 阶段执行？
6. needsCheckpoint 与 needsRollback 分别回答什么？
7. F2FS checkpoint 提交时做什么？
8. dm-bow 为什么要在覆盖旧块前搬走它？
9. prepareCheckpoint 为什么先 FITRIM？
10. health daemon 空间不足时有哪些策略？
11. abortChanges 为什么必须重启？
12. block restore 为什么要有 checksum 和 partial restore？
13. commit 为什么同时 mark A/B slot successful？
14. Keymaster 旧 key 为什么要延迟删除？
15. filesystem checkpoint 与 RollbackManager DE/CE snapshot 有何区别？

---

## 31. 初学者最终心智模型

```text
控制面：
StagingManager / AMS
  → StorageManagerService
  → IVold
  → Checkpoint.cpp
  → /metadata/vold/checkpoint

挂载数据面（二选一）：
fstab checkpoint=fs
  → F2FS checkpoint=disable
  → 成功时 remount checkpoint=enable

fstab checkpoint=block
  → fs_mgr 创建 dm-bow
  → 第一次覆盖旧块前 backup
  → 成功 state=2
  → 失败按 relocation log restore

启动协议：
startCheckpoint
  → reboot
  → markBootAttempt
  → needsCheckpoint
  → prepareCheckpoint
  → 新系统试运行
       ├─ finishBooting → commitChanges
       └─ failure/retry exhausted → abort/reboot → restore

配套一致性：
BootControl slot
APEX staged session
Keymaster key 生命周期
userdata checkpoint
必须在同一启动成功点协同收口
```

---

## 32. 本章总结

本章必须掌握八条边界：

1. checkpoint 事务与文件复制备份；
2. metadata 控制状态与真正旧数据；
3. needsCheckpoint 与 needsRollback；
4. F2FS filesystem checkpoint 与 dm-bow block checkpoint；
5. start、mark attempt、prepare、commit、abort/restore；
6. userdata checkpoint 与 A/B slot；
7. 块内容回滚与密钥生命周期；
8. filesystem checkpoint 与 RollbackManager/apexd 各自的备份机制。

下一章将学习 `BootControl HAL、A/B Slot、update_engine 与 OTA 无缝更新链路`，继续解释当前 slot 为什么有 successful/bootable/retry 状态，以及系统分区更新如何与本章 userdata checkpoint 配合。
