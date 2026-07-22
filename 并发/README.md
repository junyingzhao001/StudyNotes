# 并发相关知识

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、账号、组织域名等公司相关信息。

## 目录

- [一、Java 语义层：`synchronized` 是什么](#一java-语义层synchronized-是什么)
- [二、字节码层：`monitorenter` / `monitorexit`](#二字节码层monitorenter--monitorexit)
- [三、对象头与锁状态](#三对象头与锁状态)
- [`synchronized` 的隐藏特性](#synchronized-的隐藏特性)
- [`synchronized` 和 `ReentrantLock` 的本质区别](#synchronized-和-reentrantlock-的本质区别)
- [ART 和 HotSpot 的核心差异](#art-和-hotspot-的核心差异)
- [`synchronized` vs AQS](#synchronized-vs-aqs)
- [AQS 是什么](#aqs-是什么)
- [`ReentrantLock.lock()` 全流程](#reentrantlocklock-全流程)
- [`unlock()` 全流程](#unlock-全流程)
- [公平锁 vs 非公平锁](#公平锁-vs-非公平锁)
- [`park` 不是 `sleep`](#park-不是-sleep)
- [相关面试题](#相关面试题)

## 一、Java 语义层：`synchronized` 是什么

### 三种用法

```java
synchronized (obj) {}                 // 锁对象是 obj
synchronized void foo() {}            // 锁对象是 this
static synchronized void foo() {}     // 锁对象是当前类的 Class 对象
```

### 语义保证

`synchronized` 主要提供两个核心保证：

1. **互斥（Mutual Exclusion）**
2. **可见性（Happens-Before）**

因此可以简单理解为：

```text
synchronized = 互斥 + 内存语义
```

## 二、字节码层：`monitorenter` / `monitorexit`

编译后可以看到：

```text
monitorenter
    ...
monitorexit
```

`monitorenter` / `monitorexit` 是 JVM 字节码指令。

如果同步块内发生异常，锁也会被释放，这由编译器生成的 `try-finally` 结构保证：

```java
try {
    monitorenter
    ...
} finally {
    monitorexit
}
```

## 三、对象头与锁状态

对象在内存中的结构可以简化为：

```text
| Mark Word | Klass Pointer | Instance Data | Padding |
```

锁相关的数据主要存放在 **Mark Word** 中。

Mark Word 是一个复用字段，内容会随锁状态变化。

### 普通对象（无锁）

```text
| unused | hashcode | age | 01 |
```

### 锁状态编码

锁状态通常由 Mark Word 的低位标识：

| 状态 | 标识位 |
| --- | --- |
| 无锁 | `01` |
| 偏向锁 | `101` |
| 轻量级锁 | `00` |
| 重量级锁 | `10` |

### 锁升级过程

锁升级通常是：

```text
无锁 → 偏向锁 → 轻量级锁 → 重量级锁
```

特点：

- 通常只能升级，不能降级。
- 调用 `wait()` 会直接升级为重量级锁。
- `wait()` 需要 `WaitSet`，而 `WaitSet` 只有 `ObjectMonitor` 才有。

### 偏向锁

偏向锁适用于只有一个线程使用锁的场景，目标是减少无竞争同步的开销。

Mark Word 中保存的信息可以简化为：

```text
| threadId | epoch | age | 101 |
```

流程：

1. 第一次进入同步代码块。
2. JVM 通过 CAS 把线程 ID 写入 Mark Word。
3. 下一次同一线程进入时，不需要再次 CAS，可以直接进入。

没有竞争时，偏向锁几乎是零开销。

> 注：偏向锁是 HotSpot 早期的重要优化。JDK 15 后默认关闭，JDK 18 中已移除。

### 轻量级锁（Thin Lock）

适用场景：多个线程交替执行同步块，但没有真正并发竞争。

核心结构是 **Lock Record**，位于线程栈中：

```text
Thread Stack:
| Lock Record |
   └── displaced mark word
```

其中：

```text
displaced mark word = 被对象让出来的原始对象头 Mark Word
```

#### Lock Record 是什么

Lock Record 是 JVM 在轻量级锁阶段，为每一次 `synchronized` 进入而在线程栈中创建的一小块数据结构。

核心作用：

1. 保存对象原始的 Mark Word，也就是 displaced mark word。
2. 作为 CAS 操作的目标，与对象头 Mark Word 发生关联。

Lock Record 不在堆里，也不在对象中，而在**线程私有的栈帧**里。

#### 轻量级锁加锁过程

1. JVM 在当前线程栈中创建 Lock Record。
2. 通过 CAS 将对象 Mark Word 更新为指向 Lock Record。
3. CAS 成功后，完成加锁。

对象头 Mark Word 可以简化为：

```text
| 指向 Lock Record | 00（轻量级锁） |
```

解锁时，再把对象头恢复为原始 Mark Word。

### 重量级锁（Monitor / Mutex）

重量级锁依赖 `ObjectMonitor`。

数据结构可以简化为：

```cpp
ObjectMonitor {
    void* _owner;
    ObjectWaiter* _EntryList;
    ObjectWaiter* _WaitSet;
    int _recursions;
}
```

特点：

- 使用 OS Mutex。
- 涉及线程阻塞 / 唤醒。
- 性能最差，但功能最完整。

### Monitor 的 JVM C++ 实现位置

HotSpot 中相关源码位置：

```text
hotspot/src/share/vm/runtime/objectMonitor.hpp
hotspot/src/share/vm/runtime/objectMonitor.cpp
```

`monitorenter` 的实现入口：

```text
InterpreterRuntime::monitorenter
```

JIT 后可能进入：

```text
ObjectMonitor::enter()
```

关键 C++ 方法：

```cpp
void ObjectMonitor::enter(Thread* current);
void ObjectMonitor::exit(bool not_suspended, Thread* current);
```

`enter()` 核心逻辑可以简化为：

```cpp
if (_owner == NULL) {
    _owner = current;   // 抢锁
} else if (_owner == current) {
    _recursions++;      // 可重入
} else {
    // 阻塞
    park();
}
```

## `synchronized` 的隐藏特性

1. **可重入**：Monitor 通过 `_recursions` 记录重入次数，同一线程多次进入不会死锁。
2. **内存语义**：锁释放前的写操作，对后续获取同一把锁的线程可见。
3. **不可被中断**：`monitorenter` 本身不是可中断阻塞。

## `synchronized` 和 `ReentrantLock` 的本质区别

| 维度 | `synchronized` | `ReentrantLock` |
| --- | --- | --- |
| 实现层级 | JVM | Java |
| 锁升级 | 有 | 无 |
| 公平锁 | 不支持显式公平策略 | 支持 |
| 条件队列 | `wait` / `notify` | `Condition` |
| 可中断 | 不支持 | 支持 |
| 性能 | JDK 6+ 优化后表现较强 | 更灵活，特性更多 |

## ART 和 HotSpot 的核心差异

| 维度 | HotSpot（JVM） | ART（Android） |
| --- | --- | --- |
| 实现语言 | C++ | C++ |
| 对象头 | Mark Word | LockWord |
| 偏向锁 | 早期版本有 | 无 |
| 轻量级锁 | 有 | 有 |
| 重量级锁 | Monitor | Monitor |
| `wait` / `notify` | ObjectMonitor | Monitor |

Android AOSP 中相关源码位置：

```text
art/runtime/lock_word.h
art/runtime/monitor.cc
```

## `synchronized` vs AQS

| 维度 | `synchronized` | AQS |
| --- | --- | --- |
| 层级 | JVM | Java |
| 锁状态 | 对象头 | `state` |
| 队列 | EntryList | CLH |
| `wait` | Monitor | Condition |
| 可中断 | 不可以 | 可以 |

## AQS 是什么

AQS，即 `AbstractQueuedSynchronizer`，是一个基于以下机制实现的同步器框架：

- `volatile state`
- CAS
- CLH 队列

基于 AQS 的相关类包括：

- `ReentrantLock`
- `Semaphore`
- `CountDownLatch`
- `FutureTask`
- `ThreadPoolExecutor`（部分）

### AQS 的核心字段

```java
abstract class AbstractQueuedSynchronizer {
    volatile int state;              // 锁状态（0 / 重入次数）
    transient volatile Node head;    // 队列头（哨兵节点）
    transient volatile Node tail;    // 队列尾
}
```

### CLH 队列 Node 结构

```java
static final class Node {
    volatile Node prev;
    volatile Node next;
    volatile Thread thread;
    volatile int waitStatus;
}
```

### `waitStatus` 值的含义

| 值 | 含义 |
| --- | --- |
| `0` | 默认 |
| `SIGNAL` | 后继需要唤醒 |
| `CANCELLED` | 取消 |
| `CONDITION` | 在 Condition 队列 |
| `PROPAGATE` | 共享模式 |

### 类关系总览

```text
ReentrantLock
 └── Sync (abstract)
      ├── NonfairSync
      └── FairSync
           ↑
AbstractQueuedSynchronizer (AQS)
```

## `ReentrantLock.lock()` 全流程

调用：

```java
lock.lock();
```

实际调用：

```java
sync.lock();
```

整体流程：

```text
lock()
 └── sync.acquire(1)
       ├── tryAcquire()
       ├── addWaiter()
       ├── acquireQueued()
       └── selfInterrupt()
```

### 第一步：进入 `lock()`

```java
public void lock() {
    sync.lock();
}
```

非公平锁默认实现：

```java
final void lock() {
    if (compareAndSetState(0, 1))
        setExclusiveOwnerThread(Thread.currentThread()); // 抢锁成功
    else
        acquire(1); // 抢锁失败
}
```

### 第二步：CAS 抢锁

`state` 的含义：

- `state == 0`：无人持锁。
- `state > 0`：已被占用 / 表示重入次数。

```java
compareAndSetState(0, 1)
```

成功：

```text
state = 1
owner = currentThread
→ lock() 结束
```

失败：

```java
acquire(1);
```

### 第三步：`acquire()`

```java
public final void acquire(int arg) {
    if (!tryAcquire(arg) &&
        acquireQueued(addWaiter(Node.EXCLUSIVE), arg))
        selfInterrupt();
}
```

主要做三件事：

1. 再试一次抢锁。
2. 抢不到则入队。
3. 阻塞线程。

### 第四步：`tryAcquire()`，可重入的关键

```java
protected final boolean tryAcquire(int acquires) {
    final Thread current = Thread.currentThread();
    int c = getState();

    if (c == 0) {
        if (CAS && 设置 owner)
            return true;
    } else if (current == owner) {
        state += acquires; // 重入
        return true;
    }
    return false;
}
```

### 第五步：`addWaiter()`，进入 AQS 队列

```java
Node node = new Node(currentThread, EXCLUSIVE);
enq(node);
```

### 第六步：`acquireQueued()`，自旋 + 阻塞

```java
final boolean acquireQueued(Node node, int arg) {
    for (;;) {
        Node p = node.predecessor();

        if (p == head && tryAcquire(arg)) {
            setHead(node);
            p.next = null;
            return false;
        }

        if (shouldParkAfterFailedAcquire(p, node))
            LockSupport.park(this);
    }
}
```

自旋判断能不能抢锁：

```text
前驱是 head
→ 说明轮到我了
→ 再抢一次
```

抢不到就 `park`：

```java
LockSupport.park(this);
```

特点：

- 不忙等。
- 不占用 CPU。
- 等待 `unpark` 唤醒。

队列示例：

```text
state = 1
owner = Thread A

AQS Queue:
Head → Thread B (parked) → Thread C (parked)
```

## `unlock()` 全流程

实际执行：

```java
sync.release(1);
```

核心逻辑：

```java
public final boolean release(int arg) {
    if (tryRelease(arg)) {
        Node h = head;
        if (h != null && h.waitStatus != 0)
            unparkSuccessor(h);
        return true;
    }
    return false;
}
```

### `tryRelease()`：重入次数递减

```java
protected final boolean tryRelease(int releases) {
    state -= releases;

    if (state == 0) {
        owner = null;
        return true;
    }
    return false;
}
```

只有 `state == 0` 才真正释放锁。

### `unparkSuccessor()`：唤醒队列中的下一个线程

```java
LockSupport.unpark(next.thread);
```

被唤醒线程接下来会：

```text
从 park() 返回
继续 for (;;) 循环
发现自己是 head.next
CAS 抢锁
成功 → 成为新 head
```

## 公平锁 vs 非公平锁

### 非公平锁（默认）

```java
if (CAS 成功)
    直接插队
```

### 公平锁

```java
if (队列里有人)
    不允许 CAS 插队
```

## `park` 不是 `sleep`

> `LockSupport.park` 是精确唤醒机制，不依赖 `synchronized`。

## 相关面试题

### 1. `synchronized` 底层是怎么实现的？

`monitorenter` / `monitorexit` + 对象头 Mark Word + `ObjectMonitor`。

### 2. 锁升级过程是什么？

```text
无锁 → 偏向锁 → 轻量级锁 → 重量级锁
```

通常不可逆。

### 3. 为什么要有偏向锁？

减少无竞争同步场景下的 CAS 开销。

### 4. 偏向锁什么时候失效？

常见情况：

- 多线程竞争。
- 计算过 identity hashCode。
- JVM 参数关闭。
- 使用不支持偏向锁的 JDK 版本。

### 5. `synchronized` 和 `volatile` 的区别？

- `volatile` 解决的是**可见性 + 有序性**，不解决**原子性**。
- `synchronized` 解决的是**原子性 + 可见性 + 有序性**。

### 6. `wait` / `notify` 为什么必须在 `synchronized` 中？

`wait` / `notify` 操作的是对象的 Monitor 状态。只有持有该 Monitor 的线程，才有资格释放或唤醒它。

### 7. `hashCode` 对 `synchronized` 有什么影响？

`hashCode` 和 `synchronized` 都依赖对象头的 Mark Word。一旦对象计算过 identity hash，就无法再使用偏向锁，甚至可能触发锁膨胀，从而影响 `synchronized` 的性能。

### 8. `synchronized` 和 `ReentrantLock` 谁更快？

- 低竞争：`synchronized` 通常足够好。
- 高竞争 + 高级特性：`ReentrantLock` 更灵活。

### 9. Android 上推荐使用 `synchronized` 吗？

推荐，但要注意：

- 不要锁大对象。
- 不要随意锁 `Class` 对象。
- UI 线程慎用。

### 10. 为什么 AQS 使用 CLH 队列？

主要原因：

- FIFO。
- 自旋少。
- 适合 CAS。
