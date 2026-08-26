# 82 BootControl HAL、A/B Slot、update_engine 与 OTA 无缝更新链路

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不执行真实 OTA、切槽或刷写  
> 前置章节：26 AVB、80 APEX staged、81 userdata checkpoint

---

## 1. 什么叫“无缝更新”

传统 non-A/B OTA 常要进入 recovery，在设备不可正常使用时修改当前系统分区。A/B 更新准备两套可启动系统槽位：

```text
当前运行：slot A
后台更新：slot B
完成后重启：尝试 slot B
失败：bootloader 退回 slot A
```

下载和大部分写入发生在用户继续使用 A 的时候，因此停机时间主要是一次重启。

但“无缝”不等于没有风险，也不等于所有数据都有两份。A/B 主要保护系统启动分区，`/data` 通常仍共享，需要第 81 章 filesystem checkpoint 等机制协调数据兼容性。

---

## 2. slot 到底是什么

slot 是一组可共同启动的分区版本。常见后缀：

```text
_a
_b
```

经典 A/B 设备可能有：

```text
boot_a      boot_b
system_a    system_b
vendor_a    vendor_b
vbmeta_a    vbmeta_b
...
```

当前从 A 启动时，update_engine 可以写 B，不破坏正在运行的 A。

并非所有分区都必须 slot 化。`userdata`、`metadata` 等通常是共享分区；具体以设备 fstab、bootloader 和分区表为准。

---

## 3. 本章源码地图

| 文件 | 作用 |
|---|---|
| `frameworks/base/core/java/android/os/UpdateEngine.java` | 系统更新客户端 Java API |
| `system/update_engine/binder_service_android.cc` | Binder 服务入口 |
| `system/update_engine/update_attempter_android.cc` | ApplyPayload、InstallPlan、Action 流水线和状态 |
| `system/update_engine/payload_consumer/download_action.cc` | 下载/读取 payload 并交给 DeltaPerformer |
| `system/update_engine/payload_consumer/delta_performer.cc` | 解析 manifest、校验并执行分区 operation |
| `system/update_engine/payload_consumer/filesystem_verifier_action.cc` | 更新后分区 hash 验证 |
| `system/update_engine/payload_consumer/postinstall_runner_action.cc` | 挂载目标分区、运行 postinstall、切 active slot |
| `system/update_engine/boot_control_android.cc` | update_engine 对 BootControl HAL 的封装 |
| `hardware/interfaces/boot/1.0/IBootControl.hal` | slot 查询与 bootable/successful API |
| `hardware/interfaces/boot/1.1/types.hal` | Virtual A/B snapshot merge 状态 |
| `hardware/interfaces/boot/1.1/default/boot_control/libboot_control.cpp` | misc 中 bootloader_control 参考实现 |
| `system/update_engine/dynamic_partition_control_android.cc` | dynamic partition/Virtual A/B 协调 |
| `system/core/fs_mgr/libsnapshot` | Virtual A/B snapshot 创建、映射和 merge |

这里列出的 `IBootControl.hal` 与 `types.hal` 才是仓库中的 HIDL 接口源文件。代码里常见的
`android/hardware/boot/1.x/IBootControl.h`、`types.h` 等 C++ 头文件由 HIDL 构建规则生成，
不能把它们当成可在当前源码树中直接打开的手写源文件路径。

---

## 4. 三个参与者各负责什么

| 组件 | 职责 |
|---|---|
| update client/System Update UI | 获取 OTA 包，调用 UpdateEngine，展示进度，决定何时重启 |
| update_engine | 验证 payload、计算 source/target slot、写目标分区、校验、postinstall、请求切槽 |
| BootControl HAL + bootloader | 保存 slot priority/tries/successful/bootable，启动时选择槽位并失败退槽 |

update_engine 不能自己决定 bootloader 最终从哪里启动；它通过 BootControl HAL 写入 bootloader 能理解的 metadata。

---

## 5. Java UpdateEngine API

特权更新客户端通常：

```java
UpdateEngine engine = new UpdateEngine();
engine.bind(callback);
engine.applyPayload(url, offset, size, headers);
```

回调包括：

- `onStatusUpdate(status, percent)`；
- `onPayloadApplicationComplete(errorCode)`。

重要状态：

```text
IDLE
  → UPDATE_AVAILABLE
  → DOWNLOADING
  → VERIFYING
  → FINALIZING
  → UPDATED_NEED_REBOOT
```

Java API 返回并不表示 OTA 完成。真正完成要以异步 callback、update_engine 持久状态、新 slot 启动和 successful 标记为证据。

---

## 6. payload 不是整包解压脚本

A/B OTA 核心通常是 `payload.bin` 加属性：

- payload metadata；
- DeltaArchiveManifest；
- 各分区的新旧大小和 hash；
- InstallOperation 列表；
- operation 数据 blob；
- payload 签名。

`applyPayload` 还接收：

- URL 或 fd；
- offset；
- size；
- `FILE_HASH`；
- `FILE_SIZE`；
- `METADATA_HASH`；
- `METADATA_SIZE`；
- 是否切槽、是否执行 postinstall 等 header。

官方 build 中 `hash_checks_mandatory=true`，不能把下载完成当成内容可信。

---

## 7. full payload 与 delta payload

### Full OTA

大部分目标内容直接来自 payload：

```text
REPLACE / REPLACE_BZ / REPLACE_XZ
  → 解压数据
  → 写目标 extent
```

### Delta OTA

利用当前 source slot 的旧块：

```text
SOURCE_COPY
  → 从 source extent 复制

SOURCE_BSDIFF / BROTLI_BSDIFF / PUFFDIFF
  → 读取旧块
  → 应用 patch
  → 生成新块
```

还有 ZERO、DISCARD 等 operation。

Delta 更省下载量，但依赖源分区精确匹配。源块 hash 不对时不能“差不多继续”，否则目标镜像不可验证。

---

## 8. InstallPlan：本次更新的施工图

`UpdateAttempterAndroid::ApplyPayload()` 构造 `InstallPlan`：

```cpp
install_plan_.source_slot = GetCurrentSlot();
install_plan_.target_slot = GetTargetSlot();
```

典型：

```text
current/source = A(0)
target         = B(1)
```

InstallPlan 还记录：

- payload hash/size/metadata；
- resume；
- powerwash requirement；
- switch_slot_on_reboot；
- run_post_install；
- write_verity；
- 每个目标 partition 的 source/target path、size、hash。

它贯穿各 Action，避免下载、验证和 postinstall 对“写哪个槽”理解不一致。

---

## 9. update_engine Action 流水线

Android 11 Android 路径依次入队：

```text
UpdateBootFlagsAction
  → CleanupPreviousUpdateAction
  → InstallPlanAction
  → DownloadAction
  → FilesystemVerifierAction
  → PostinstallRunnerAction
```

| Action | 作用 |
|---|---|
| UpdateBootFlags | 处理当前启动/更新标记 |
| CleanupPreviousUpdate | 清理前次更新或 Virtual A/B merge |
| InstallPlan | 把施工图送入流水线 |
| Download | 拉取 payload、执行分区 operations |
| FilesystemVerifier | 对写后目标分区做 hash/verity 验证 |
| PostinstallRunner | 运行目标版本 postinstall，完成 dynamic partition 更新并设置 active slot |

Action 通过输入输出 pipe 串联；任一步失败都会终止后续动作并报告 ErrorCode。

---

## 10. 下载与写分区为何能断点续传

update_engine 把 payload ID、已处理 operation、偏移、签名上下文等进度写入 prefs。

再次 `applyPayload` 时：

```text
payload_id 相同
  + DeltaPerformer::CanResumeUpdate()
  → install_plan.is_resume=true
  → 从可信 checkpoint 继续
```

断点不能只保存“下载百分比”。对于分区写入，必须保证：

- 前面 operation 已完整完成；
- 已写块可验证；
- payload 未变；
- source slot/版本未变；
- hash context 能继续。

超过恢复失败限制或状态不可信时，需要重置进度重新开始。

---

## 11. 写目标槽前为什么可标 unbootable

BootControl 的 `setSlotAsUnbootable(target)` 用于避免正在施工的 B 被 bootloader 选中：

```text
B 可能只有 boot 写了一半
  → 意外重启
  → 若 B 仍被认为可启动
  → 可能进入残缺系统
```

参考实现中 unbootable 会设置：

```text
successful_boot = 0
tries_remaining = 0
```

等所有写入、验证和 postinstall 成功后，`setActiveBootSlot(B)` 才重新给予高 priority 和启动次数。

这里的调用点并不含糊：Android 11 的
`system/update_engine/payload_consumer/download_action.cc` 在
`DownloadAction::PerformAction()` 准备开始下载/应用 payload 时执行：

```cpp
LOG(INFO) << "Marking new slot as unbootable";
if (!boot_control_->MarkSlotUnbootable(install_plan_.target_slot)) {
  LOG(WARNING) << "Unable to mark new slot ...";
}
```

也就是说，顺序是“先让目标槽不可启动，再开始施工”。源码把标记失败当作
warning 并继续更新，这是 update_engine 的错误处理策略；设备仍应正确实现
BootControl，不能据此认为 unbootable 标记不重要。

---

## 12. DeltaPerformer 如何执行 operation

`DownloadAction` 获取数据后交给 `DeltaPerformer`。它：

1. 解析 payload manifest；
2. 验证 metadata signature；
3. 建立 source/target partition fd；
4. 检查 operation data hash；
5. 按顺序执行 operation；
6. 更新 resume checkpoint；
7. 最后提取并验证 payload signature。

典型分发：

```cpp
switch (operation.type()) {
  REPLACE...       → PerformReplaceOperation
  ZERO/DISCARD     → PerformZeroOrDiscardOperation
  SOURCE_COPY      → PerformSourceCopyOperation
  SOURCE_BSDIFF... → PerformSourceBsdiffOperation
  PUFFDIFF         → PerformPuffDiffOperation
}
```

“签名验证”不是最后一次总验才开始；metadata、每个 operation 数据、source hash、最终 payload/partition 都有不同校验点。

---

## 13. 目标分区写完为何还要 FilesystemVerifier

operation 都成功只说明每步没有报告错误，不足以证明整个目标分区等于发布者预期。

`FilesystemVerifierAction` 根据 InstallPlan 中的目标 size/hash 重新读取目标分区并计算 hash：

```text
operations 写完
  → 读取 target partition
  → 计算整体 hash
  → 与 manifest target hash 比较
```

同时可能写入/处理 verity/FEC 数据。

这相当于施工队自检后，再按最终蓝图整体验收。若整体 hash 不匹配，不能切槽。

---

## 14. postinstall 是什么

某些分区在新版本首次启动前需要准备工作，例如生成优化产物或执行模块声明的脚本。

`PostinstallRunnerAction`：

1. 找出带 postinstall 配置的目标分区；
2. 只读或按要求挂载目标分区；
3. 以目标 slot 参数运行 postinstall program；
4. 读取进度输出；
5. 卸载；
6. 全部成功才允许切槽。

postinstall 运行在旧系统仍在工作的环境里，但操作目标版本的挂载内容。这意味着 postinstall 接口必须兼容当前执行环境。

---

## 15. 何时设置新 slot active

`CompletePostinstall()` 中，只有成功时：

```text
DynamicPartitionControl.FinishUpdate()
  → BootControl.SetActiveBootSlot(target)
  → 设置 warm reset 提示
```

如果 `SWITCH_SLOT_ON_REBOOT=0`，更新可以写完但暂不切槽，返回 `kUpdatedButNotActive`。之后可用相应 API 再切换。

正常成功后 update_engine 进入：

```text
UPDATED_NEED_REBOOT
```

这只表示“目标槽已经准备好，等待重启尝试”，不表示新版本 boot successful。

---

## 16. BootControl 的四个核心维度

参考 `bootloader_control.slot_info`：

| 字段/概念 | 含义 |
|---|---|
| priority | bootloader 优先尝试哪个槽 |
| tries_remaining | 尚未 successful 时还允许尝试多少次 |
| successful_boot | OS 已确认这个槽成功启动 |
| verity_corrupted | AVB/verity 发现损坏的标志 |

参考实现 `setActiveBootSlot` 对目标槽：

```text
priority = 15
tries_remaining = 6
```

并降低其他同优先级槽；但源码注释明确：

```text
setting active does not change successful bit
```

因此 active、bootable、successful 是三个不同概念。

---

## 17. active、current、bootable、successful

| 词 | 正确理解 |
|---|---|
| active | bootloader 下次优先尝试的槽 |
| current | 本次实际从哪个槽启动，由 bootloader suffix 决定 |
| bootable | 还有启动资格，参考实现主要看 tries_remaining != 0 |
| successful | OS 已走到成功确认点，不再依赖有限重试 |

例如更新完成但还未重启：

```text
current = A
active  = B
B bootable = true
B successful = false
```

首次从 B 启动：

```text
current = B
B successful 仍可能 false
tries_remaining 随 bootloader 尝试减少
```

直到 Android 明确 markBootSuccessful。

---

## 18. bootloader 怎样自动退回旧槽

简化选择策略：

```text
选择 priority 最高且 bootable 的 slot
  → 若 successful=false，消耗一次 tries_remaining
  → 启动失败/反复重启且一直未标成功
  → tries_remaining 逐渐到 0
  → 新槽不再 bootable
  → 选择仍可启动的旧槽
```

旧 A 在后台更新期间没有被覆盖，这就是 A/B 的基础回退能力。

bootloader 具体算法由设备实现决定，HAL 定义的是 OS 与 bootloader 共享的状态语义；不能把参考实现的 15、6 当作所有厂商绝对固定值。

---

## 19. 谁调用 markBootSuccessful

这里最容易误解，因为源码里不止一个调用入口，而且它们出现的时机不同。

第 81 章看到 AMS `finishBooting()` 调用 `IStorageManager.commitChanges()`。设备支持
filesystem checkpoint 时，vold 在 checkpoint commit 路径调用 BootControl：

```text
markBootSuccessful(current slot)
```

设备不支持 checkpoint 时，`bootable/recovery/update_verifier/update_verifier.cpp`
会先验证 care map 指定的分区块，验证通过后直接调用
`markBootSuccessful()`；如果支持 checkpoint，它会明确延迟这一步，让 checkpoint
成功提交路径完成标记。

update_engine 自身也有 `MarkBootSuccessfulAsync`。尤其要注意：
`UpdateBootFlagsAction` 位于一次 OTA 的动作链开头，它是在应用下一份 payload 前，
把**此刻已经运行的 current slot** 标成 good；它不是“写完 target slot 后立即把
target 标成功”。target 此时还没有成为 current，更没有经历重启验证。

因此不要背成“某个唯一组件在某一行统一宣告成功”。更准确的判断方式是：

```text
未重启前：target 只能被设为 active/可尝试，不能冒充 successful
重启以后：被启动的 target 变成 current
验证与 checkpoint 策略满足后：current 才被标为 successful
后续 Virtual A/B cleanup/merge：还会等待这个 successful 条件
```

这些入口按设备是否支持 checkpoint、当前所处流程协作；BootControl 操作应当允许
重复查询或幂等更新，不能把它们误画成“同一次启动必然重复写两次”的固定脚本。

参考实现成功时：

```text
successful_boot = 1
tries_remaining = 1
```

这里 tries 设 1 是确保最后一次尝试成功的槽仍被视为 bootable；successful bit 才表示以后不按普通试启动逻辑淘汰它。

---

## 20. slot suffix 从哪里来

BootControl `getCurrentSlot()` 必须与 bootloader 传入的 suffix 一致。suffix 来源可通过：

```text
androidboot.slot_suffix
```

进入内核命令行或 device tree，再形成只读启动属性。

常见：

```text
slot 0 → _a
slot 1 → _b
```

但业务代码应通过 BootControl/boot properties 查询，不应到处硬编码 “A 就一定是某个固定块设备路径”。dynamic partitions 和厂商布局会让实际映射更复杂。

---

## 21. AVB 与 A/B 怎样配合

每个槽的 boot、vbmeta、system/vendor 等必须形成可验证链。

启动新 B 时：

```text
bootloader 验证 vbmeta_b
  → 验证 boot_b / logical partitions 的 descriptors
  → rollback index 防止降到不允许的旧版本
  → dm-verity 在运行时验证只读分区块
```

BootControl 说“B bootable”不等于 AVB 一定验证成功。验证失败时 bootloader/AVB 可以把槽判坏或转向另一个槽。

反过来，AVB 全部通过也不代表业务能启动完成；system_server/native crash 仍可能耗尽 B 的 tries，然后回 A。

---

## 22. Dynamic Partitions 与 A/B

Android 10+ 常把 system、vendor、product 等逻辑分区放进 `super`。逻辑 A/B 不一定表现为两个固定物理分区，而是两套 logical partition metadata/extent。

`DynamicPartitionControlAndroid` 在更新前：

- 读取 source/target slot metadata；
- 调整目标 logical partition 大小；
- 分配/释放 super extents；
- 提供目标 block device path；
- 更新 metadata；
- 完成或清理上次更新。

所以“写 system_b”是逻辑概念。底层可能是 super 中的一组 extent，不必存在传统 GPT `/dev/block/by-name/system_b`。

---

## 23. Classic A/B 与 Virtual A/B

### Classic A/B

```text
A 分区集合完整一份
B 分区集合完整一份
更新直接写 inactive B
```

优点是模型简单、回退直接；缺点是需要较多空间。

### Virtual A/B

Android 11 源码包含 `libsnapshot`，由属性：

```text
ro.virtual_ab.enabled
```

控制相关路径。它使用 dynamic partition snapshot 和 COW：

```text
当前 base blocks
  + 新版本 COW blocks
  → 映射出 target snapshot 视图
```

首次启动成功后再后台 merge，把新块合并回 base。

Virtual A/B 仍保留 slot/BootControl 语义，但“target slot”不再等于一整套独立物理分区。

---

## 24. Virtual A/B 更新状态

`libsnapshot` 的 **UpdateState** 高层状态不只四种。正常成功主线可先记为：

```text
Initiated
  → Unverified
  → Merging
  → MergeNeedsReboot（仅在清理需要重启时出现）
  → MergeCompleted
```

完整枚举还包含 `None`、`MergeFailed` 与 `Cancelled`。不要把上面的成功主线误当成完整状态集合；另外，
每个分区的 `SnapshotState` 又是 `NONE/CREATED/MERGING/MERGE_COMPLETED`，与全局 UpdateState 不是同一个 enum。

BootControl 1.1 还有 merge status：

- NONE；
- SNAPSHOTTED；
- MERGING；
- CANCELLED。

关键安全边界：

```text
尚未开始 merge
  → 可以丢弃 snapshot/COW，回到旧 base

已经进入 merge
  → base 正在吸收新块
  → 不能再简单丢 COW 回滚
  → 必须把 merge 正确完成/恢复
```

所以 cleanup previous update 不是随手删除临时文件，它可能需要等待或恢复 snapshot merge。

---

## 25. Virtual A/B 与 userdata checkpoint 不同

| Virtual A/B snapshot | filesystem checkpoint |
|---|---|
| 保护 system/vendor 等动态只读分区更新 | 保护 userdata 可写文件系统 |
| COW 表示目标系统分区的新块 | 保存/延迟确认 /data 的新写入 |
| 成功后 merge 到 base | 成功后 F2FS/bow commit |
| 失败时切槽/丢弃未 merge snapshot | 失败时下次启动恢复 userdata 旧视图 |

它们可能在一次 OTA 中协同，但不是同一个 snapshot。

---

## 26. OTA 与 APEX staged update 也不同

整机 OTA payload 可以更新 slot 中的 system/product/vendor，也可能携带预装 APEX 的新基线。第 80 章的独立 APEX staged install 则把更新 APEX 放在 data 侧并由 apexd 激活。

| OTA A/B | 独立 APEX staged |
|---|---|
| update_engine 写 target slot/Virtual A/B snapshot | PackageInstaller + StagingManager + apexd |
| 重启切系统槽 | 重启激活 data APEX |
| BootControl 失败退槽 | apexd session/checkpoint revert |
| 可改变 build fingerprint | 模块级更新通常不换整机 build |

两条链可同时存在，但状态数据库不同。

---

## 27. 为什么 OTA 后可能需要 powerwash

若新系统与当前 userdata 不兼容，payload/header 可要求 `POWERWASH`。update_engine 在 InstallPlan 记录 `powerwash_required`，postinstall 完成时安排擦除。

这与 checkpoint rollback 方向相反：

```text
checkpoint：失败时尽量恢复旧 userdata
powerwash：明确放弃用户数据，以满足目标系统启动条件
```

属于高影响策略，不能从“版本降级/升级”自动推断，必须看 payload metadata 和更新策略。

---

## 28. 更新完成但用户不重启时

`UPDATED_NEED_REBOOT` 可跨 update_engine 重启恢复。当前仍运行旧 A：

```text
current=A
target B 已写好
active=B（默认）
status=UPDATED_NEED_REBOOT
```

此时：

- 新功能尚未生效；
- 不能再次覆盖同一更新状态；
- 用户可继续短时使用旧系统；
- 目标 slot 材料必须保持完整；
- 重启后才进入 B 的试启动窗口。

若取消 pending update，可把 current A 重新 `setActiveBootSlot(A)`，但是否允许、如何清理 snapshot 要由 update_engine 正式流程处理，不能只手改 active bit。

---

## 29. 失败发生在不同阶段意味着什么

| 阶段 | 典型失败 | 结果 |
|---|---|---|
| metadata/payload 验证 | 签名/hash 不符 | 不写或停止，保持当前槽 |
| operation 写入 | I/O/source hash/空间错误 | 目标槽保持不可用 |
| filesystem verify | target hash 不符 | 不切槽 |
| postinstall | 脚本失败 | 不设 target active |
| 首次启动 AVB | vbmeta/verity 失败 | bootloader 尝试旧槽 |
| 首次启动业务 | system_server/native crash | 未 mark successful，tries 耗尽退槽 |
| Virtual A/B merge | 重启/merge 中断 | libsnapshot 恢复并继续 merge |
| userdata checkpoint | 新系统未达成功点 | /data 回滚并配合退槽 |

因此“OTA 失败”必须先定位发生在哪一层。

---

## 30. 排障证据

### update_engine

查：

```text
current_slot / target_slot
InstallPlan
payload_id
operation index
ErrorCode
target partition hash
postinstall exit
UPDATED_NEED_REBOOT
```

源码入口：

```bash
rg -n "ApplyPayload|BuildUpdateActions|SetActiveBootSlot|UPDATED_NEED_REBOOT"   system/update_engine/update_attempter_android.cc   system/update_engine/payload_consumer/postinstall_runner_action.cc
```

### BootControl

查：

- current slot suffix；
- priority；
- tries_remaining；
- successful_boot；
- bootable；
- verity_corrupted；
- Virtual A/B merge status。

### 启动链

查：

- bootloader 选择的 slot；
- AVB 错误；
- boot reason；
- system_server 是否到 finishBooting；
- checkpoint 是否 commit；
- 是否又回到旧 slot。

---

## 31. 常见误解复读

### 误解一：active slot 就是当前 slot

错误。active 是下次优先选择；current 是本次实际启动来源。

### 误解二：setActiveBootSlot 会把新槽标 successful

错误。参考实现明确不改变 successful bit，只设置 priority 和 tries。

### 误解三：UPDATED_NEED_REBOOT 表示 OTA 已最终成功

错误。它只表示目标准备完成；还没经过新槽首次启动。

### 误解四：bootable 等于 successful

错误。未成功的新槽在 tries_remaining 大于 0 时仍可 bootable。

### 误解五：bootloader 退槽会自动恢复所有 /data

错误。系统分区退槽和 userdata checkpoint 是两套机制，必须协调。

### 误解六：Virtual A/B 就是不需要 slot

错误。它仍保留 slot/boot success 语义，只是用 snapshot/COW 减少完整分区副本。

### 误解七：payload 下载 hash 通过就不必验证目标分区

错误。还需要 operation/source/target partition/verity 等多层校验。

### 误解八：snapshot merge 开始后还能直接删除 COW 回滚

错误。merge 已修改 base，必须完成或恢复 merge 协议。

---

## 32. Mac 上的六轮只读练习

### 第一轮：追 Java API

```bash
sed -n '240,380p' frameworks/base/core/java/android/os/UpdateEngine.java
sed -n '150,325p' system/update_engine/update_attempter_android.cc
```

### 第二轮：画 Action 流水线

```bash
sed -n '705,755p' system/update_engine/update_attempter_android.cc
```

### 第三轮：追 operation

```bash
sed -n '670,770p'   system/update_engine/payload_consumer/delta_performer.cc
```

区分 full replace 与 source delta。

### 第四轮：追验收与切槽

```bash
rg -n "target_hash|PerformAction|SetActiveBootSlot"   system/update_engine/payload_consumer/filesystem_verifier_action.cc   system/update_engine/payload_consumer/postinstall_runner_action.cc
```

### 第五轮：追 BootControl

```bash
sed -n '240,335p'   hardware/interfaces/boot/1.1/default/boot_control/libboot_control.cpp
```

画出 priority/tries/successful。

### 第六轮：追 Virtual A/B

```bash
sed -n '20,125p'   system/core/fs_mgr/libsnapshot/android/snapshot/snapshot.proto
rg -n "CreateUpdateSnapshots|InitiateMerge|ProcessUpdateState"   system/core/fs_mgr/libsnapshot
```

---

## 33. 自测题

1. 为什么 inactive slot 可以在后台写？
2. 哪些常见分区不会因 A/B 自动获得双份？
3. full payload 与 delta payload 的 operation 有何不同？
4. InstallPlan 为什么必须贯穿所有 Action？
5. 断点续传为什么不能只记下载百分比？
6. 为什么施工中的 target 要保持 unbootable？
7. operation 全成功后为何还要整体 hash？
8. postinstall 运行在哪个系统环境、操作哪个槽？
9. active、current、bootable、successful 分别是什么？
10. setActiveBootSlot 为什么不能顺便 successful？
11. bootloader 怎样用 tries 实现自动退槽？
12. AVB 成功为何仍可能退槽？
13. dynamic partition 的 system_b 为什么可能不是固定物理分区？
14. Virtual A/B merge 开始前后，回滚能力有何变化？
15. Virtual A/B snapshot 与 userdata checkpoint 分别保护什么？
16. UPDATED_NEED_REBOOT 之后还有哪些成功点？

---

## 34. 初学者最终总图

```text
当前启动：
Bootloader 选择 A
  → current=A, A successful

后台 OTA：
UpdateEngine.applyPayload
  → InstallPlan source=A target=B
  → target B 保持不可启动
  → DownloadAction + DeltaPerformer
  → 写 boot/system/vendor/vbmeta... B
  → FilesystemVerifier 整体验证
  → PostinstallRunner
  → FinishUpdate
  → setActiveBootSlot(B)
  → B priority 高、tries=有限、successful=false
  → UPDATED_NEED_REBOOT

首次启动：
Bootloader 尝试 B，tries--
  → AVB 验证 B
  → Android 启动
  → userdata 在 checkpoint 保护下试运行
       ├─ 成功到 finishBooting
       │    → commit userdata checkpoint
       │    → markBootSuccessful(B)
       │    → Virtual A/B 开始/继续 merge
       │
       └─ 反复失败
            → B tries 耗尽/unbootable
            → Bootloader 选择旧 A
            → userdata checkpoint 回滚

Classic A/B：
直接写独立 B 分区

Virtual A/B：
base + COW 映射目标视图
  → 成功后 merge
  → merge 开始前可丢 snapshot 回旧状态
```

---

## 35. 本章总结

本章必须掌握九条边界：

1. source/current slot 与 target/inactive slot；
2. payload 下载、operation 写入、目标整体验证；
3. active、current、bootable、successful；
4. setActiveBootSlot 与 markBootSuccessful；
5. bootloader tries 退槽与 Android 业务成功确认；
6. A/B 系统分区与共享 userdata；
7. Classic A/B 与 Dynamic/Virtual A/B；
8. Virtual A/B snapshot 与 filesystem checkpoint；
9. 整机 OTA、独立 APEX staged update 与 RollbackManager。

下一章将学习 `AVB 2.0、vbmeta、dm-verity 与 rollback index 启动验证链路`，进一步回答 bootloader 如何验证某个 slot 的每一层内容，以及“签名正确但版本太旧”为何仍会拒绝启动。
