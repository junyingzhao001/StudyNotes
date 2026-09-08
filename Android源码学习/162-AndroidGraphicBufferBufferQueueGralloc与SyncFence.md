# 162 Android GraphicBuffer、BufferQueue、Gralloc 与 Sync Fence

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 11、12、21、29、30、66、161 章

---

## 1. 先把五个名词放到一条链上

第 161 章追到了 client target 和 HWC present，但“一张 buffer”仍可能被误解成普通 C++ 数组。本章先给出最短模型：

```text
GraphicBuffer
  = buffer 元数据 + imported native handle 的 C++ 包装

Gralloc allocator
  = 创建新的底层图形分配

Gralloc mapper
  = 导入、校验、CPU 映射和释放当前进程的 handle

BufferQueue
  = 用 slot 循环 producer / consumer 对 buffer 的逻辑所有权

Fence
  = 所有权已经交出，但访问前仍须满足的异步完成条件
```

完整循环是：

```text
consumer release fence
  → producer dequeue 后等待
  → producer 写 buffer
  → producer queue acquire fence
  → consumer acquire 后等待
  → consumer 读 buffer
  → consumer release fence
```

所以 BufferQueue 不复制像素，也通常不先替双方阻塞等完所有工作。它主要传递三样东西：

- slot 对应关系；
- buffer 及帧元数据；
- 下一任访问前必须等待的 fence。

本章最重要的判断方法是：

> 先问“slot 的逻辑所有权在谁手里”，再问“上一任的异步工作是否已由 fence 宣告完成”。状态已经迁移，不等于硬件工作已经结束。

---

## 2. 源码地图与对象边界

主线文件：

```text
frameworks/native/libs/ui/
├── GraphicBuffer.cpp
├── GraphicBufferAllocator.cpp
├── GraphicBufferMapper.cpp
├── Gralloc2.cpp / Gralloc3.cpp / Gralloc4.cpp
├── Fence.cpp
└── include/ui/
    ├── GraphicBuffer.h
    ├── BufferQueueDefs.h
    └── Fence.h

frameworks/native/libs/gui/
├── BufferQueue.cpp
├── BufferQueueCore.cpp
├── BufferQueueProducer.cpp
├── BufferQueueConsumer.cpp
├── Surface.cpp
└── include/gui/
    ├── BufferQueueCore.h
    ├── BufferSlot.h
    └── BufferItem.h

hardware/interfaces/graphics/
├── allocator/4.0/IAllocator.hal
└── mapper/4.0/IMapper.hal

system/core/libcutils/
├── native_handle.cpp
└── include/cutils/native_handle.h
```

对象之间不是一一对应：

| 对象 | 主要身份 | 容易误解成什么 |
|---|---|---|
| `GraphicBuffer` | 当前进程的描述对象 | 像素数组本身 |
| imported handle | 当前进程可用的 gralloc handle | 跨进程不变的指针 |
| `BufferSlot` | 最多 64 个映射位置之一 | 一张永久固定的 buffer |
| `BufferItem` | 一次排队提交的帧记录 | buffer 本体 |
| `Fence` | 一个 sync_file fd 的 RAII 包装 | 全局固定名称的“完成信号” |

`BufferQueueProducer` 与 `BufferQueueConsumer` 共享同一个 `BufferQueueCore`，因此同进程调用也要遵守同样的 slot/fence 协议；跨进程时，Binder 再负责对象与 fd 的传输。

---

## 3. GraphicBuffer 描述分配，不内嵌像素

`GraphicBuffer` 继承 `ANativeWindowBuffer`、引用计数基类和 `Flattenable`。核心字段可以分两组：

```text
公开 buffer 属性
  width / height / stride
  format / layerCount / usage
  handle

管理属性
  mId
  mGenerationNumber
  mOwner
  mTransportNumFds / mTransportNumInts
```

`handle` 指向 `native_handle_t`。它描述如何引用底层共享分配，却不是 CPU 可直接解引用的像素地址。CPU 地址要到 mapper `lock()` 后才得到。

### stride 不是 width 的别名

`width` 是有效像素列数；`stride` 在其有意义时，是相邻行同一列之间的像素步长。allocator 可以为了对齐、硬件布局或压缩块让它大于 width。

因此下面的通用估算并不可靠：

```text
allocation bytes = width × height × 4
```

YUV、多平面、压缩格式和 modifier 都可能打破它。r48 的 `GraphicBufferAllocator` dump 也只把 `stride × height × bytesPerPixel` 当作估算；当 stride 无意义、格式的 `bytesPerPixel()` 为 0，或存在 vendor 私有布局时，它不是实际显存占用的权威数据。

### mId 的范围

r48 生成逻辑 ID 的方式是：

```cpp
uint64_t id = static_cast<uint64_t>(getpid()) << 32;
id |= static_cast<uint32_t>(android_atomic_inc(&nextId));
```

flatten 会传输这个 ID，接收端重建的 `GraphicBuffer` 因而保留同一逻辑身份。但应区分：

```text
mId             逻辑 buffer ID，会随 flatten 传输
handle 指针      当前进程地址
fd 数字          当前进程 fd 表下标
底层内核对象      fd 所引用的共享分配
```

PID 复用和 32 位计数回绕决定了 `mId` 不是跨无限时间的密码学唯一 ID；排障时也不能比较两个进程里的 handle 指针或 fd 数字来判断是否同一 allocation。

---

## 4. native_handle 与 GraphicBuffer 所有权

`native_handle_t` 的公共布局很简单：

```cpp
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
```

```text
data[0 ... numFds-1]                 fd
data[numFds ... numFds+numInts-1]    普通整数
```

fd 引用共享内存或驱动对象；ints 可保存 vendor 私有元数据。具体字段布局属于 gralloc 实现，不是 Framework 的统一协议。

`native_handle_clone()` 会逐个 `dup()` fd，再 `memcpy()` 普通整数。只复制整数值会让两个对象错误地共享同一 fd 所有权。

资源释放也分两步：

```text
native_handle_close()   关闭 handle 中的 fd
native_handle_delete()  释放 native_handle 结构体内存
```

不要用其中一个替代另一个。

### 四种 HandleWrapMethod

| 模式 | 是否 import | 输入 handle 的所有权 | GraphicBuffer 析构 |
|---|---:|---|---|
| `WRAP_HANDLE` | 否 | 不接管 | 不释放 |
| `TAKE_HANDLE` | 否 | 接管已 imported handle | mapper `freeBuffer()` |
| `TAKE_UNREGISTERED_HANDLE` | 是 | 成功后接管 raw handle | 先关/删 raw，最终释放 imported handle |
| `CLONE_HANDLE` | 是 | 不接管输入 | 释放自己得到的 imported handle |

`initWithSize()` 创建的对象记为 `ownData`；析构时走 allocator `free()`。不过 r48 的 allocator `free()` 最终同样调用 mapper `freeBuffer()`，因为 allocator 返回的 raw handle 已在 Framework 内导入。

这解释了一个常见误区：所有权针对的是“当前进程这份 imported handle 及其引用”，不是宣称全系统再没有其他进程引用底层 allocation。

---

## 5. allocator 创建，mapper 让当前进程可用

Gralloc 把职责拆成两部分。

`IAllocator::allocate()` 接收 descriptor 和数量，返回 stride 与 raw handles。descriptor 包含：

```text
name
width / height
layerCount
format
usage
reservedSize
```

`IMapper` 负责：

```text
createDescriptor
importBuffer / freeBuffer
validateBufferSize
getTransportSize
CPU lock / unlock
读取或设置 metadata
```

主分配链：

```text
GraphicBuffer(width, height, ...)
  → GraphicBuffer::initWithSize()
  → GraphicBufferAllocator::allocate()
  → GrallocXAllocator::allocate()
  → IMapper::createDescriptor()
  → IAllocator::allocate() 得到 raw handle
  → IMapper::importBuffer()
  → GraphicBuffer 保存当前进程的 imported handle
```

r48 的 mapper 和 allocator 都按 `Gralloc4 → Gralloc3 → Gralloc2` 尝试；没有可用实现会 fatal。Gralloc 4 mapper 还明确要求 passthrough，远程 mapper 会触发 fatal，因为 imported handle 和 CPU 映射结果必须在调用进程直接使用。allocator 则可以是 HAL 服务。

### usage 会影响真实分配

usage 不是文档注解，而是 allocator 的输入约束：

```text
CPU read/write
GPU texture / render target
composer
video encoder
protected content
```

它可能影响 heap、缓存策略、布局与压缩方式。producer 申请时，`dequeueBuffer()` 会执行：

```cpp
usage |= mCore->mConsumerUsageBits;
```

也就是把 producer 和 consumer 的需求合并后再判断旧 buffer 能否复用。

### r48 的三个包装边界

1. 宽或高任一为 0 时，`allocateHelper()` 把两者一起改成 `1 × 1`，所以 `0 × 1080` 也不会变成 `1 × 1080`。
2. `layerCount < 1` 被改成 1。
3. HAL allocate 的非零错误向上统一变为 `NO_MEMORY`，细分的 unsupported/no-resources 等原因只能结合前置日志判断。

另外，`GraphicBuffer::needsReallocation()` 对 usage 使用“已有 usage 是否覆盖新需求”的包含判断，但 protected bit 必须精确一致：

```cpp
if ((usage & inUsage) != inUsage) return true;
if ((usage & USAGE_PROTECTED) !=
        (inUsage & USAGE_PROTECTED)) return true;
```

旧 buffer 多出的普通 usage 通常可接受；protected 属性变化则强制重分配。

---

## 6. flatten 传描述，unflatten 后重新 import

`GraphicBuffer::flatten()` 的固定头在新格式下是 13 个 `int32_t`：

```text
[0]  magic = GB01
[1]  width
[2]  height
[3]  stride
[4]  format
[5]  layerCount
[6]  usage low 32
[7]  mId high 32
[8]  mId low 32
[9]  generation number
[10] transport fd count
[11] transport int count
[12] usage high 32
后续 transport ints
另行传 transport fds
```

这里用 `getTransportSize()`，不是盲目发送 imported handle 的所有 fd/int。Gralloc 4 允许 imported handle 在尾部附加当前进程的运行时数据，传输时可省略这些本地字段。

Binder 传 fd 时，为接收进程建立新的 fd 引用；接收端看到的整数通常不同，但仍可引用同一底层内核对象。

`unflatten()` 的关键步骤是：

```text
校验 magic、字节数、fd/int 数
  → native_handle_create()
  → 填入 Binder 交付的 fd 与 ints
  → 恢复尺寸、usage、ID、generation
  → GraphicBufferMapper::importBuffer(raw handle)
  → validateBufferSize()
  → 关闭并删除临时 raw handle
  → 保存 imported handle
```

`IMapper` 规范明确规定：从另一进程、HAL 或 clone 得到的 raw handle 不能直接访问底层 buffer，必须先 import。import 是建立当前进程的有效 handle 与 bookkeeping，不是逐像素复制。

### 格式兼容与输入防护

```text
GB01   13 words，64-bit usage
GBFR   12 words，旧 32-bit usage
```

未知 magic 返回 `BAD_TYPE`。r48 还限制：

```cpp
numFds < 4096
numInts < 4096 - flattenWordCount
```

并逐步检查 buffer 字节数和 fd 数，避免恶意或损坏输入造成溢出、越界分配。

---

## 7. CPU lock/unlock 也遵守 fence

`GraphicBuffer::lockAsync()` 把 imported handle、usage、访问矩形和输入 fence fd 交给 mapper：

```text
输入 fence   之前的设备访问何时结束
返回地址     CPU 可访问映射
```

矩形超出 width/height 会先返回 `BAD_VALUE`。即使持有 handle，也不保证任意 usage 都能 lock；protected buffer、特定压缩布局或 vendor 限制都可能拒绝 CPU 映射。

`unlockAsync()` 返回一个 fence fd，表示 CPU 写入相关的异步 cache flush 等工作何时完成。下一任必须继承这个条件。

同步包装 `GraphicBufferMapper::unlock()` 的行为更重：

```cpp
unlockAsync(handle, &fenceFd);
if (fenceFd >= 0) {
    sync_wait(fenceFd, -1);
    close(fenceFd);
}
```

也就是同步版本会在本线程等待完成；异步版本把 fence 交给调用者继续传递。不能因为 API 名叫 lock/unlock，就忽略 buffer 在 CPU、GPU、HWC 之间的同步。

---

## 8. BufferQueue 是槽位映射，不是 64 张常驻图片

`BufferQueue::createBufferQueue()` 创建共享同一 Core 的 producer/consumer 两端：

```text
IGraphicBufferProducer
          │
          ▼
BufferQueueCore
  slots[64]
  queue FIFO
  free/active/unused 集合
  mutex + conditions
          ▲
          │
IGraphicBufferConsumer
```

`BufferQueueDefs::NUM_BUFFER_SLOTS = 64` 只是 Framework 能追踪的最大 slot 数。Core 初始化时只把当前计算出的可用数量放进 `mFreeSlots`，其余在 `mUnusedSlots`；buffer 更是按 dequeue 需要才分配。

数量公式是：

```cpp
maxCount =
    mMaxAcquiredBufferCount +
    mMaxDequeuedBufferCount +
    ((mAsyncMode || mDequeueBufferCannotBlock) ? 1 : 0);

maxCount = min(mMaxBufferCount, maxCount);
```

默认 `maxAcquired = 1`、`maxDequeued = 1`、同步且可阻塞时，计算结果是 2；异步或不可阻塞时多留 1 个，常见为 3。运行时配置、consumer 限额和显式 max count 都会改变结果。

因此：

```text
64 slots     映射表硬上限
maxCount     当前允许纳入管理的 slot 数
allocated    当前真的持有 GraphicBuffer 的 slot 数
```

三者不能互换。

Core 还区分：

| 集合 | 含义 |
|---|---|
| `mUnusedSlots` | 当前 maxCount 之外 |
| `mFreeSlots` | 可用但没有可复用 GraphicBuffer |
| `mFreeBuffers` | FREE 且保留可复用 GraphicBuffer |
| `mActiveBuffers` | dequeued/queued/acquired，或 shared 特例 |
| `mQueue` | 等 consumer acquire 的 `BufferItem` FIFO |

“在 FIFO 中”与“slot 状态”高度相关，却不是同一个维度；shared mode、stale item 与 drop 路径尤其不能只看队列长度推断所有权。

---

## 9. slot 状态和 fence 必须一起读

普通模式下最容易理解的状态环是：

```text
FREE
  --dequeueBuffer--> DEQUEUED
  --queueBuffer----> QUEUED
  --acquireBuffer--> ACQUIRED
  --releaseBuffer--> FREE
```

`cancelBuffer()` 则让 `DEQUEUED → FREE`。

r48 的 `BufferState` 实际不是单 enum，而是三个计数加一个 shared 标志：

```text
mDequeueCount
mQueueCount
mAcquireCount
mShared
```

普通模式下计数组合表现为 FREE、DEQUEUED、QUEUED、ACQUIRED；shared mode 可让同一 slot 同时拥有多种计数。

| 状态 | 逻辑持有者 | slot.mFence 表示什么 |
|---|---|---|
| FREE | BufferQueue | 上一 consumer 读完，或 cancel 前 producer 写完 |
| DEQUEUED | producer | fence 已交 producer，slot 内重置为 `NO_FENCE` |
| QUEUED | BufferQueue | producer 写完条件 |
| ACQUIRED | consumer | fence 已交 consumer，slot 内重置为 `NO_FENCE` |

所以 `mFence` 不能永久命名成 acquire fence 或 release fence；名字来自接收者视角：

```text
producer queue 的 fence
  = consumer 的 acquire fence

consumer release 的 fence
  = producer 下次 dequeue 的等待 fence
```

最关键的反例是：

> slot 进入 FREE，只说明逻辑所有权回到 BufferQueue；其 fence 可能尚未 signal。BufferQueue 可以把 slot 和 fence 一起交给 producer，由 producer 在覆盖写之前等待。

`Fence::NO_FENCE` 是一个非空 `Fence` 对象，内部 fd 为 -1，表示无需等待。它与 `nullptr` 不同；queue/cancel/release 等接口会把 null fence 当作参数错误。

---

## 10. dequeue 与 requestBuffer：先定 slot，再补对象

`BufferQueueProducer::dequeueBuffer()` 的主路径：

```text
检查连接、尺寸与 abandoned
  → 合并 consumer usage，补默认尺寸/格式
  → 等待 allocation 或可用 slot
  → 优先 mFreeBuffers，其次允许分配时取 mFreeSlots
  → slot 状态记为 DEQUEUED
  → 判断现有 GraphicBuffer 是否满足属性
  → 必要时清旧映射并在锁外分配
  → 返回 slot、flags、buffer age、fence
```

普通 dequeue 优先复用 `mFreeBuffers.front()`；它不是扫描所有 64 个槽位，也不保证总选某个固定 slot。attach 恰好反过来优先空 slot，减少覆盖已有 buffer。

### 超额检查的启动边界

producer 的 `maxDequeued` 检查只在 `mBufferHasBeenQueued == true` 后执行：

```cpp
if (mBufferHasBeenQueued &&
        dequeuedCount >= mMaxDequeuedBufferCount) {
    return INVALID_OPERATION;
}
```

这是源码中的兼容边界，不应简化成“从连接第一刻起任何 dequeue 都严格被同一判断拒绝”。

若没有 slot：

- 可阻塞模式在 condition 上等待；
- async 或 cannot-block 模式通常返回 `WOULD_BLOCK`；
- consumer 为原子 acquire+release 临时多持有 1 个时，producer 仍可等待，不立刻 `WOULD_BLOCK`；
- 配置了非负 timeout 时可能返回 `TIMED_OUT`。

### 分配为什么放在 Core 锁外

需要新 buffer 时，代码先：

```text
slot = DEQUEUED
mGraphicBuffer = null
mIsAllocating = true
BUFFER_NEEDS_REALLOCATION
```

然后离开 Core 锁构造 `GraphicBuffer`，最后重新加锁提交结果。这样慢 HAL 调用不长期占住 `mMutex`；`mIsAllocating` 与 condition 防止其他相关路径并行破坏映射。

若分配失败或期间队列被 abandoned，slot 会回到 free 并被清理，不保留半完成分配。

### 为什么 Surface 还要 requestBuffer

`dequeueBuffer()` 的跨进程热路径首先返回 slot 和 flags。只有发生以下任一条件时，`Surface` 才调用：

```cpp
if ((result & BUFFER_NEEDS_REALLOCATION) ||
        localSlotBuffer == nullptr) {
    producer->requestBuffer(slot, &localSlotBuffer);
}
```

`requestBuffer()` 不负责分配；分配已经在 dequeue 内完成。它只校验 slot 当前属于 producer，设置 `mRequestBufferCalled = true`，再返回 Core 中的 `GraphicBuffer`。

queue 前要求这个标志为 true，可发现 producer 收到新映射却仍使用旧 slot cache 的错误。首次、重分配、consumer attach 以及 `RELEASE_ALL_BUFFERS` 后都可能需要重取对象。

### dequeue fence 的交接

```cpp
*outFence = mSlots[found].mFence;
mSlots[found].mFence = Fence::NO_FENCE;
```

producer 已获得 slot，但必须等返回 fence 后才能覆盖写。shared buffer 除首帧外会返回 `NO_FENCE`，这是其并发状态模型的特殊协议，不能推广到普通模式。

旧 `EGLSyncKHR` 路径最多等 1 秒；失败或超时只记录日志仍返回 buffer，因为所有权已经转移、此处太晚回滚。现代 sync-file fence 主路径则把 fence 交给 producer。

---

## 11. queue 与 cancel：提交帧不等于显示帧

`queueBuffer(slot, input, output)` 先校验：

```text
连接仍有效
slot 范围合法且当前 DEQUEUED
requestBuffer 已调用
fence 非 null
scaling mode 与 crop 合法
```

`QueueBufferInput` 携带的不只是 fence：

```text
requested present timestamp / auto timestamp
dataspace / HDR metadata
crop / scaling / transform / sticky transform
surface damage
producer 完成 fence
是否请求 frame timestamps
```

UNKNOWN dataspace 会换成 consumer 默认值。随后：

```cpp
mSlots[slot].mFence = acquireFence;
mSlots[slot].mBufferState.queue();
++mCore->mFrameCounter;
mSlots[slot].mFrameNumber = currentFrameNumber;
```

变量叫 `acquireFence`，是因为它将成为 consumer 的获取条件；从 producer 角度，它表达本次写入何时完成。

queue 只把 `BufferItem` 放入 FIFO 或替换一个可丢尾项，然后通知 consumer。它不表示：

- SurfaceFlinger 已经 acquire；
- GPU 写 fence 已 signal；
- HWC 已 present；
- 屏幕已经扫描到这帧。

`cancelBuffer()` 不生成帧。它把 DEQUEUED 计数减掉，非 shared slot 放回 free buffers，并保存调用者 fence，保证下一次复用不会踩到 cancel 前仍在进行的写。

### 回调顺序与 EGL 节流

queue 在释放 Core 主锁后才调用 consumer listener，避免外部回调重入主状态机；另一个 callback mutex 加 ticket 保证多个 producer 调用的回调顺序与 queue 顺序一致。

若 producer API 是 EGL，回调后还会等待“上一笔 queue 的 acquire fence”：

```cpp
lastQueuedFence->waitForever(
        "Throttling EGL Production");
```

代码注释给出的目标是允许两张完整 buffer 排队，但阻止第三张继续拉大队列；这是偏向延迟的 producer 节流，不等于等待当前帧显示完成。

---

## 12. acquire 与 release：consumer 也靠 slot cache

`BufferQueueConsumer::acquireBuffer()` 首先统计已 acquired 数量。判断条件是：

```cpp
numAcquiredBuffers >=
        mMaxAcquiredBufferCount + 1
```

也就是 consumer 可短暂比配置上限多持有 1 张，以便先建立新内容，再释放旧内容；若已经达到“上限 + 1”，继续 acquire 才返回 `INVALID_OPERATION`。

普通获取会拿 FIFO front：

```text
QUEUED → ACQUIRED
BufferItem 交给 consumer
slot.mFence → NO_FENCE
FIFO 删除该 item
```

结果需要区分：

```text
NO_ERROR             成功获得帧
NO_BUFFER_AVAILABLE  队列确实没有可取帧
PRESENT_LATER        有帧，但时间或 frame 上限尚不允许取
INVALID_OPERATION    consumer 已超出临时获取上限
```

### mAcquireCalled 为什么能省 GraphicBuffer

第一次让 consumer 见到某 slot 时，`BufferItem` 带真实 `GraphicBuffer`，consumer 建立本地：

```text
slot → GraphicBuffer
```

以后同一映射再次 acquire，producer 预先记录到 item 的 `mAcquireCalled` 为 true，于是 Core 可把：

```cpp
outBuffer->mGraphicBuffer = nullptr;
```

这表示“沿用 consumer 的 slot cache”，不是没有 buffer。consumer attach 的映射可能频繁变化，所以相关路径把 `mAcquireCalled` 设为 false，强制下一次发送对象。

### release 的两个校验

`releaseBuffer(slot, frameNumber, releaseFence, ...)` 要求 slot 正在 ACQUIRED，且 fence 非 null。普通模式还要求 frame number 与 slot 当前值一致：

```cpp
if (frameNumber != mSlots[slot].mFrameNumber &&
        !mSlots[slot].mBufferState.isShared()) {
    return STALE_BUFFER_SLOT;
}
```

这是防止“旧帧的迟到 release”释放掉同 slot 后来装入的新一代 buffer。shared mode 中 queue/acquire 可并发改变 frame number，因此豁免此检查。

成功后：

```text
slot.mFence = consumer release fence
ACQUIRED → FREE
非 shared slot 进入 mFreeBuffers
通知等待中的 producer
```

通知时 release fence 完全可能尚未 signal；producer 下一次 dequeue 取得它后再等待。

---

## 13. 丢帧、时间戳与 damage 的真实规则

queue 创建 `BufferItem` 时，根据下列条件决定它是否可丢：

```text
async mode
或 SurfaceFlinger consumer + queueBufferCanDrop
或 legacy drop + queueBufferCanDrop
或 shared buffer
```

若 FIFO 非空且最后一项 `last.mIsDroppable`，新 item 会覆盖队尾，而不是继续增长。非 stale 的旧 slot 执行 `freeQueued()`，在非 shared 情况下回到 free buffers。

注意判断的是“已有尾项是否可丢”，不是新来的 item 自称可丢就能覆盖任意不可丢帧。

### 为什么必须合并 surface damage

假设：

```text
帧 A 只更新左上角
帧 B 只更新右下角
A 被 B 替换
```

若只保留 B 的 damage，consumer 可能永远不知道 A 对左上角的修改。r48 因而执行：

```cpp
item.mSurfaceDamage |= last.mSurfaceDamage;
```

任一方为 `INVALID_REGION` 时，结果也是 invalid/full。这里 damage 合并的是更新语义，不是复制被丢 buffer 的像素。

被 drop 的 slot 中仍保存它原来的 producer 完成 fence。slot 虽已逻辑 FREE，后续 dequeue 会把该 fence 交回 producer，防止它在上一笔写尚未完成时重写同一分配。

### acquire 也会按 desired-present 丢旧 front

consumer 提供 `expectedPresent` 且队列至少两项时，代码检查第二项：

- 它不能超过 `maxFrameNumber`；
- timestamp 必须落在 `expectedPresent - 1s` 到 `expectedPresent`；
- front 不能因下一项明显太早或异常 timestamp 而被丢。

满足时可循环 drop 更老的 front。r48 源码还留有 TODO：这里没有先确认第二项 fence 已 signal。因此准确表述是“选择时间上更合适的帧”，不能说“只会丢掉且切换到已经完全 ready 的帧”；consumer 取得后仍须等待它的 acquire fence。

最后再判断 front：

```text
desired present 尚未来到
或 frameNumber 超过 maxFrameNumber
  → PRESENT_LATER
```

`PRESENT_LATER` 不改变 slot 所有权，也不等于空队列。

---

## 14. 四种标识、shared mode 与连接生命周期

排障时至少分开四种标识：

| 标识 | 范围 | 用途 |
|---|---|---|
| slot index | 当前 BufferQueue，0–63 | 双方缓存映射键 |
| `GraphicBuffer::mId` | buffer 逻辑对象 | 跟踪同一分配描述 |
| generation number | attach 兼容代 | 阻止旧队列世代的 buffer 重新挂入 |
| frame number | 单队列成功 queue 序列 | 时间线、stale release、buffer age |

`attachBuffer()` 会比较 buffer 与 Core 的 generation，不一致返回 `BAD_VALUE`。slot 自身会复用，不能当永久 buffer ID。

`buffer age` 在复用旧 buffer 时按：

```cpp
mFrameCounter + 1 - slot.mFrameNumber
```

计算；新分配时为 0。它帮助 producer 决定需要累计多少历史 damage，不是 buffer 的年龄秒数，也不是 fence 时间。

### shared buffer mode 是有意的状态机例外

shared mode 可让同一 slot 同时 dequeued、queued、acquired，普通四态互斥模型不再成立：

```text
第一次 dequeue/queue 确立 shared slot
后续 dequeue 总返回该 slot
队列空且 autoRefresh 时 consumer 可重建 BufferItem
cancel、producer/consumer detach、attach 都被禁止
除首帧外 dequeue 可返回 NO_FENCE
```

因此 shared mode 是“反复共享同一分配并靠专门协议协调”，不是“普通 BufferQueue 恰好只有一张图”。

### disconnect、abandon 与 Binder death

consumer `disconnect()` 会：

```text
mIsAbandoned = true
清 listener 与 FIFO
freeAllBuffersLocked()
清 shared slot
唤醒 producer
```

之后 producer 的 dequeue/request/queue/cancel 等返回 `NO_INIT`。producer 正常 disconnect 则清理它的连接 API、buffer 和死亡通知，但不等同于 consumer 永久 abandon。

producer listener 若是远端 Binder，会注册 death recipient；远端死亡触发 producer 断开，避免 Core 永远保留一条已失效的生产连接。

---

## 15. Fence 实现和 r48 的失败边界

`Fence` 本质是：

```cpp
class Fence {
    base::unique_fd mFenceFd;
};
```

`unique_fd` 负责析构关闭 fd。`NO_FENCE` 内部 fd 为 -1，多数操作把它当作“已经满足”：

```text
NO_FENCE.wait(...)       → NO_ERROR
NO_FENCE.isValid()       → false
NO_FENCE.getSignalTime() → SIGNAL_TIME_INVALID
```

`wait(timeoutMs)` 调用 `sync_wait`；`waitForever(name)` 先等 3000 ms，超时就打印 sync_file 和内部 sync points，再无限等待。三秒是告警阈值，不是最终超时。

`Fence::merge(name, A, B)` 返回同时等待 A、B 的新 sync_file：

- 两者有效：正常 merge；
- 只有一个有效：把该 fence 与自身 merge，以得到指定名称的新 fence；
- 两者都无效：返回 `NO_FENCE`；
- `sync_merge` 失败：记录错误并返回 `NO_FENCE`。

`getSignalTime()` 返回：

```text
有效且已 signal   所有内部 sync point 的最晚 timestamp
有效但未 signal   INT64_MAX / SIGNAL_TIME_PENDING
无效或查询失败    -1 / SIGNAL_TIME_INVALID
```

Fence flatten 固定写一个 fd 数；有效 fence 另传 1 个 fd，接收端 unflatten 接管交付的 fd。

### 两条“记录错误但继续”的路径

1. 前述旧 `EGLSyncKHR` dequeue 等待失败或 1 秒超时后仍返回 buffer。
2. `Surface::dequeueBuffer()` 把 `Fence` 转成 native-window fd 时，`fence->dup()` 若失败只记录日志并继续，输出 fd 会是 -1。源码注释承认最坏可能出现短暂可见损坏。

两者都是既有所有权迁移后的错误恢复策略，不能推广成“fence 可以随便忽略”。

### 线程与锁的边界

```text
BufferQueue 状态迁移
  受 Core mMutex 保护

gralloc allocation
  通过 mIsAllocating 标记，在 Core 锁外执行

consumer callbacks
  在 Core 锁外调用，另用 ticket 串行化顺序

fence 等待
  尽量留给真正访问资源的一方或设备依赖
```

这套设计追求的是缩短临界区并保留异步流水，不是保证所有 API 都永不阻塞。无 free slot、EGL 节流、CPU 同步 unlock 和 `waitForever()` 都可能等待。

---

## 16. 用一轮生命周期收束本章

把普通模式的一张 buffer 从 consumer 归还开始串起来：

```text
1. consumer:
   releaseBuffer(slot, consumerDoneFence)
   ACQUIRED → FREE

2. producer:
   dequeueBuffer()
   FREE → DEQUEUED
   得到 consumerDoneFence，访问前等待

3. Surface:
   若 flags 要求或本地 cache 缺失
   requestBuffer(slot) 取得 GraphicBuffer

4. producer:
   GPU/CPU 写入
   queueBuffer(slot, producerDoneFence)
   DEQUEUED → QUEUED

5. consumer:
   acquireBuffer()
   QUEUED → ACQUIRED
   得到 producerDoneFence，读取前等待

6. consumer:
   合成、编码或显示读取完成后
   再提交新的 release fence
```

读源码时可按下面的核对顺序定位问题：

```text
1. 这是 slot、GraphicBuffer、allocation 还是 BufferItem？
2. 当前普通状态/三个计数分别是什么？
3. mGraphicBuffer 为空是待分配，还是接收端 slot cache 命中？
4. fence 是上一 producer 写完，还是上一 consumer 读完？
5. fence 已随所有权交出，还是仍保存在 slot？
6. maxCount、队列长度、dequeued/acquired 数分别是多少？
7. 是否处于 async、cannot-block 或 shared 特例？
8. frame/generation 是否匹配，是否遇到 stale release？
9. drop 时旧 slot、旧 fence 与 damage 是否都正确继承？
10. 错误路径是回滚、返回，还是仅记录后继续？
```

本章最终应记住：

1. `GraphicBuffer` 是底层共享分配的描述与 handle 包装，不内嵌整块像素。
2. raw handle 跨边界后必须 import；handle 指针和 fd 数字都不是跨进程身份。
3. allocator 创建分配，mapper 导入、验证、映射并释放进程内 handle。
4. 64 是 slot 硬上限，实际可用数由 acquired、dequeued、async 和 max count 共同决定。
5. `requestBuffer()` 获取 slot 对应对象，不是分配入口。
6. slot 状态表达逻辑所有权，fence 表达异步完成；必须同时判断。
7. queue 只表示提交，acquire 只表示取得，二者都不等于 present。
8. FREE 不保证硬件已完成，下一 producer 仍要等待 dequeue fence。
9. async replacement 和 expected-present 都可丢帧，但规则不同；replacement 还必须合并 damage。
10. `NO_FENCE` 是合法的无需等待对象，`nullptr` 才是多数接口拒绝的参数。

下一章转向应用侧的一帧生产链：`ViewRootImpl`、`ThreadedRenderer`、`RenderNode`、RenderThread 与 `Surface` 怎样把 Java UI 绘制变成一次真实的 dequeue、渲染和 queue。
