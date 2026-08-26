# AQS (AbstractQueuedSynchronizer) 使用文档

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

## 一、AQS 简介

### **1.1 什么是 AQS？**

AbstractQueuedSynchronizer（AQS）是 Java 并发包中提供的一个**同步框架**，用于构建锁和其他同步工具。它是 ReentrantLock、CountDownLatch、Semaphore 等同步工具的基础。

### **1.2 AQS 的核心组件**

1. **state（状态）**：`volatile int`，表示同步状态
2. **等待队列**：双向链表（CLH 队列），存储等待的线程
3. **CAS 操作**：原子性地修改 state
4. **模板方法模式**：定义需要子类实现的方法

### **1.3 AQS 的优势**

- **代码复用**：队列管理、阻塞/唤醒等通用逻辑由 AQS 处理
- **标准化**：统一的同步框架，易于理解和维护
- **性能优化**：JVM 层面的优化，性能优秀
- **灵活性**：支持独占模式和共享模式

## 二、AQS 的核心方法

### **2.1 需要子类实现的方法**

#### 独占模式（Exclusive Mode）

```kotlin
// 尝试获取锁
protected fun tryAcquire(arg: Int): Boolean
// 尝试释放锁
protected fun tryRelease(arg: Int): Boolean
// 判断当前线程是否独占锁
protected fun isHeldExclusively(): Boolean
```

#### 共享模式（Shared Mode）

```kotlin
// 尝试获取锁（共享）
protected fun tryAcquireShared(arg: Int): Int
// 返回值：>= 0 表示成功，< 0 表示失败
// 尝试释放锁（共享）
protected fun tryReleaseShared(arg: Int): Boolean
```

### **2.2 AQS 提供的公共方法**

```kotlin
// 获取锁（独占模式，会阻塞）
fun acquire(arg: Int)

// 释放锁（独占模式）
fun release(arg: Int)

// 获取锁（共享模式，会阻塞）
fun acquireShared(arg: Int)

// 释放锁（共享模式）
fun releaseShared(arg: Int)

// 获取当前状态
fun getState(): Int

// 设置状态
fun setState(newState: Int)

// CAS 操作：原子性地修改状态
fun compareAndSetState(expect: Int, update: Int): Boolean

// 设置独占线程
fun setExclusiveOwnerThread(thread: Thread?)

// 获取独占线程
fun getExclusiveOwnerThread(): Thread?
```

## 三、基于当前代码的实现详解

### **3.1 CustomLock - 非公平可重入锁**

#### 类结构

```kotlin
class CustomLock {
    private val sync = Sync()
    
    private class Sync : AbstractQueuedSynchronizer() {
        // 实现 tryAcquire, tryRelease, isHeldExclusively
    }
}
```

#### tryAcquire() 实现

```kotlin
override fun tryAcquire(acquires: Int): Boolean {
    val current = Thread.currentThread()
    val state = state
    
    // 情况1：锁未被占用（state == 0）
    if (state == 0) {
        // 使用 CAS 尝试获取锁（非公平：直接尝试，不检查队列）
        if (compareAndSetState(0, acquires)) {
            setExclusiveOwnerThread(current)
            return true
        }
    }
    // 情况2：可重入（当前线程已持有锁）
    else if (current == exclusiveOwnerThread) {
        val nextState = state + acquires
        if (nextState < 0) {
            throw Error("Maximum lock count exceeded")
        }
        setState(nextState)  // 增加重入次数
        return true
    }
    
    return false  // 获取锁失败
}
```

**关键点：**

- `state == 0`：锁未被占用
- `state > 0`：锁被占用，值表示重入次数
- `compareAndSetState()``：原子性地修改状态
- `setExclusiveOwnerThread()``：设置独占线程

#### tryRelease() 实现

```kotlin
override fun tryRelease(releases: Int): Boolean {
    val current = Thread.currentThread()
    
    // 检查是否是锁的持有者
    if (current != exclusiveOwnerThread) {
        throw IllegalMonitorStateException("当前线程不是锁的持有者")
    }
    
    val state = this.state - releases  // 减少重入次数
    val free = state == 0  
    // 是否完全释放
    if (free) {
        setExclusiveOwnerThread(null)  // 清除独占线程
    }
    
    setState(state)  // 更新状态
    return free
}
```

**关键点：**

- 检查是否是锁的持有者
- 减少重入次数
- 如果 `state == 0`，完全释放锁

#### 使用示例

```kotlin
val lock = CustomLock()

// 获取锁
lock.lock()
try {
    // 临界区代码
    doSomething()
} finally {
    lock.unlock()  // 必须在 finally 中释放
}

// 尝试获取锁（非阻塞）
if (lock.tryLock()) {
    try {
        // 获取成功
    } finally {
        lock.unlock()
    }
} else {
    // 获取失败
}
```

### **3.2 FairLock - 公平可重入锁**

#### 与 CustomLock 的区别

**非公平锁（CustomLock）：**

```kotlin
if (state == 0) {
    if (compareAndSetState(0, acquires)) {  
    // 直接尝试获取// ...
    }
}
```

**公平锁（FairLock）：**

```kotlin
if (state == 0) {
    // 关键：检查队列中是否有等待的线程
    if (!hasQueuedPredecessors() && compareAndSetState(0, acquires)) {
        // ...
    }
}
```

#### hasQueuedPredecessors() 方法

```kotlin
// AQS 提供的方法
public final boolean hasQueuedPredecessors() {
    Node t = tail;
    Node h = head;
    Node s;
    return h != t &&
        ((s = h.next) == null || s.thread != Thread.currentThread());
}
```

**作用：**

- 检查等待队列中是否有其他线程在等待
- 如果有，当前线程不能"插队"，必须排队
- 保证按照 FIFO 顺序获取锁

#### 使用场景

- **公平锁**：适合需要保证公平性的场景（如资源分配）
- **非公平锁**：适合大多数场景，性能更好

### **3.3 CustomReadWriteLock - 读写锁**

#### state 的设计

读写锁使用 state 的**高 16 位**和**低 16 位**分别表示：

- **高 16 位**：读锁数量（共享锁）
- **低 16 位**：写锁状态（独占锁）

```kotlin
companion object {
    private const val SHARED_SHIFT = 16
    private const val SHARED_UNIT = (1 shl SHARED_SHIFT)  // 65536
    private const val MAX_COUNT = (1 shl SHARED_SHIFT) - 1  // 65535
    private const val EXCLUSIVE_MASK = (1 shl SHARED_SHIFT) - 1  // 65535
    
    // 获取读锁数量
    fun sharedCount(c: Int): Int = c ushr SHARED_SHIFT
    
    // 获取写锁状态
    fun exclusiveCount(c: Int): Int = c and EXCLUSIVE_MASK
}
```

#### 读锁实现（共享模式）

```kotlin
override fun tryAcquireShared(acquires: Int): Int {
    val current = Thread.currentThread()
    var c = state
    
    // 如果有写锁且不是当前线程持有，获取失败
    if (exclusiveCount(c) != 0 && exclusiveOwnerThread != current) {
        return -1  // 失败
    }
    
    val r = sharedCount(c)
    if (r < MAX_COUNT && compareAndSetState(c, c + SHARED_UNIT)) {
        return 1  // 成功
    }
    
    return -1  // 失败
}
```

**关键点：**

- 读锁是共享的，多个线程可以同时持有
- 如果有写锁，读锁不能获取（除非是同一个线程）
- 返回值：>= 0 表示成功，< 0 表示失败

#### 写锁实现（独占模式）

```kotlin
override fun tryAcquire(acquires: Int): Boolean {
    val current = Thread.currentThread()
    val c = state
    
    // 如果有读锁或写锁，获取失败
    if (c != 0) {
        if (exclusiveOwnerThread != current) {
            return false
        }
        // 可重入
        if (exclusiveCount(c) + acquires > MAX_COUNT) {
            throw Error("Maximum lock count exceeded")
        }
        setState(c + acquires)
        return true
    }
    
    // 尝试获取写锁
    if (compareAndSetState(0, acquires)) {
        setExclusiveOwnerThread(current)
        return true
    }
    
    return false
}
```

**关键点：**

- 写锁是独占的，获取时不能有任何读锁或写锁
- 支持可重入

#### 使用示例

```kotlin
val rwLock = CustomReadWriteLock()

// 读操作
rwLock.readLock().lock()
try {
    val value = readData()  // 多个读线程可以同时执行
} finally {
    rwLock.readLock().unlock()
}

// 写操作
rwLock.writeLock().lock()
try {
    writeData(value)  // 独占访问
} finally {
    rwLock.writeLock().unlock()
}
```

## 四、AQS 工作流程

### **4.1 获取锁流程（独占模式）**

```text
lock()
  └─> acquire(1)
       ├─> tryAcquire(1)  // 尝试获取锁
       │    └─> 成功？返回 true，获取锁成功
       │
       └─> 失败？
            ├─> addWaiter(Node.EXCLUSIVE)  // 加入等待队列
            └─> acquireQueued(node, 1)
                 ├─> 自旋检查
                 ├─> 前驱是头节点 && tryAcquire(1)？
                 │    └─> 成功，设置为头节点
                 └─> 失败？
                      └─> LockSupport.park()  // 阻塞线程
```

### **4.2 释放锁流程（独占模式）**

```text
unlock()
  └─> release(1)
       ├─> tryRelease(1)  // 尝试释放锁
       │    └─> 成功？
       │         └─> unparkSuccessor(head)  // 唤醒下一个线程
       │              └─> LockSupport.unpark(nextThread)
       └─> 返回
```

### **4.3 等待队列（CLH 队列）**

AQS 使用双向链表实现的等待队列：

```kotlin
static final class Node {
    volatile int waitStatus;      // 等待状态
    volatile Node prev;            // 前驱节点
    volatile Node next;            // 后继节点
    volatile Thread thread;        // 等待的线程
    Node nextWaiter;              // 下一个等待者
}
```

**等待状态值：**

- CANCELLED (1): 节点已取消
- SIGNAL (**-1**): 后继节点需要被唤醒
- CONDITION (**-2**): 节点在条件队列中
- PROPAGATE (**-3**): 共享模式下使用

## 五、实际使用示例

### **5.1 多线程竞争测试**

```kotlin
val lock = CustomLock()
var counter = 0

// 创建多个线程竞争锁
val threads = mutableListOf<Thread>()

for (i in 1..5) {
    val thread = Thread {
        lock.lock()
        try {
            val oldValue = counter
            Thread.sleep(100)  // 模拟业务操作
            counter = oldValue + 1
        } finally {
            lock.unlock()
        }
    }
    threads.add(thread)
}

threads.forEach { it.start() }
threads.forEach { it.join() }

// 最终 counter = 5
```

### **5.2 可重入测试**

```kotlin
val lock = CustomLock()

lock.lock()
try {
    // 外层
    lock.lock()  // 重入
    try {
        // 内层
        println("重入次数: ${lock.getHoldCount()}")  // 输出: 2
    } finally {
        lock.unlock()
    }
} finally {
    lock.unlock()
}
```

### **5.3 公平锁测试**

```kotlin
val fairLock = FairLock()

// 按顺序启动线程，公平锁应该按顺序获取
for (i in 1..5) {
    Thread {
        fairLock.lock()
        try {
            println("线程 $i 获取锁")
        } finally {
            fairLock.unlock()
        }
    }.start()
    Thread.sleep(10)  // 稍微延迟，确保按顺序请求
}
```

### **5.4 读写锁测试**

```kotlin
val rwLock = CustomReadWriteLock()
var readValue = 0

// 多个读线程可以同时读取
for (i in 1..3) {
    Thread {
        rwLock.readLock().lock()
        try {
            println("读线程 $i 读取: $readValue")
        } finally {
            rwLock.readLock().unlock()
        }
    }.start()
}

// 写线程独占访问
Thread {
    rwLock.writeLock().lock()
    try {
        readValue = 100
        println("写线程写入: $readValue")
    } finally {
        rwLock.writeLock().unlock()
    }
}.start()
```

## 六、实现自定义同步工具

### **6.1 实现步骤**

1. **定义同步器类**：继承 AbstractQueuedSynchronizer
2. **实现模板方法**：`tryAcquire`、`tryRelease` 等
3. **设计 state 的含义**：根据需求设计状态表示
4. **提供公共接口**：封装 acquire、`release` 等方法

### **6.2 示例：实现一个简单的信号量**

```kotlin
class SimpleSemaphore(private val permits: Int) {
    private val sync = Sync(permits)
    
    private class Sync(permits: Int) : AbstractQueuedSynchronizer() {
        init {
            setState(permits)  // 初始化许可数
        }
        
        override fun tryAcquireShared(acquires: Int): Int {
            while (true) {
                val current = state
                val remaining = current - acquires
                if (remaining < 0 || compareAndSetState(current, remaining)) {
                    return remaining
                }
            }
        }
        
        override fun tryReleaseShared(releases: Int): Boolean {
            while (true) {
                val current = state
                val next = current + releases
                if (next < current) {  // 溢出
                    throw Error("Maximum permit count exceeded")
                }
                if (compareAndSetState(current, next)) {
                    return true
                }
            }
        }
    }
    
    fun acquire() {
        sync.acquireShared(1)
    }
    
    fun release() {
        sync.releaseShared(1)
    }
}
```

## 七、最佳实践

### **7.1 状态设计**

- **简单锁**：state = 0（未占用）或 > 0（重入次数）
- **读写锁**：高 16 位（读锁数量）+ 低 16 位（写锁状态）
- **信号量**：`state` 表示可用许可数
- **CountDownLatch**：`state` 表示剩余计数

### **7.2 错误处理**

```kotlin
override fun tryRelease(releases: Int): Boolean {
    val current = Thread.currentThread()
    
    // 检查是否是锁的持有者
    if (current != exclusiveOwnerThread) {
        throw IllegalMonitorStateException("当前线程不是锁的持有者")
    }
    
    // 检查溢出
    val state = this.state - releases
    if (state < 0) {
        throw Error("Lock count underflow")
    }
    
    // ...
}
```

### **7.3 性能优化**

1. **快速路径**：先尝试获取锁，失败才加入队列
2. **自旋**：在阻塞前自旋检查，减少上下文切换
3. **CAS 操作**：使用 compareAndSetState 原子性修改状态

### **7.4 注意事项**

1. **必须在 finally 中释放锁**：避免死锁
2. **检查重入次数**：防止溢出
3. **验证线程身份**：确保只有持有者能释放锁
4. **使用 volatile 或 CAS**：保证状态可见性

## 八、常见问题

### **8.1 为什么需要可重入？**

避免死锁，允许同一线程多次获取锁：

```kotlin
fun method1() {
    lock.lock()
    try {
        method2()  // 需要再次获取锁
    } finally {
        lock.unlock()
    }
}

fun method2() {
    lock.lock()  // 如果是不可重入锁，会死锁
    try {
        // ...
    } finally {
        lock.unlock()
    }
}
```

### **8.2 公平锁 vs 非公平锁**

| 维度 | 非公平锁 | 公平锁 |
| --- | --- | --- |
| **性能** | 更好 | 略差 |
| **公平性** | 不保证 | 保证 FIFO |
| **实现** | 直接尝试获取 | 检查队列 |
| **适用场景** | 大多数场景 | 需要公平性的场景 |

### **8.3 读写锁的优势**

- **读多写少**：多个读线程可以并发，提高性能
- **写操作**：独占访问，保证数据一致性
- **降级**：支持从写锁降级到读锁

### **8.4 如何选择独占模式还是共享模式？**

- **独占模式**：互斥锁、写锁等，同一时刻只有一个线程能获取
- **共享模式**：读锁、信号量等，多个线程可以同时获取

## 九、调试技巧

### **9.1 查看等待队列**

```kotlin
// 检查是否有等待的线程
fun hasQueuedThreads(): Boolean {
    return sync.hasQueuedThreads()
}

// 获取等待队列长度
fun getQueueLength(): Int {
    return sync.queueLength
}
```

### **9.2 日志记录**

```kotlin
override fun tryAcquire(acquires: Int): Boolean {
    val current = Thread.currentThread()
    Log.d(TAG, "线程 ${current.name} 尝试获取锁")
    
    // ...
    if (success) {
        Log.d(TAG, "线程 ${current.name} 获取锁成功")
    } else {
        Log.d(TAG, "线程 ${current.name} 获取锁失败")
    }
}
```

### **9.3 使用 jstack 查看线程状态**

```bash
jstack <pid>
```

可以查看：

- 线程的等待状态
- 等待的锁对象
- 死锁信息

Demo：

```kotlin
      
package com.example.myapplication

import android.util.Log
import java.util.concurrent.locks.AbstractQueuedSynchronizer

/**
 * 自定义锁实现 - 基于 AQS
 * 
 * 这是一个简单的互斥锁实现，支持可重入特性
 */
class CustomLock {
    private val sync = Sync()
    
    companion object {
        private const val TAG = "CustomLock"
    }
    
    /**
     * 内部同步器，继承自 AQS
     * 使用 state 表示锁的状态：
     * - state = 0: 锁未被占用
     * - state > 0: 锁被占用，值表示重入次数
     */
    private class Sync : AbstractQueuedSynchronizer() {
        
        /**
         * 尝试获取锁（非公平方式）
         * @param acquires 请求的锁数量（通常为 1）
         * @return true 如果成功获取锁，false 否则
         */
        override fun tryAcquire(acquires: Int): Boolean {
            val current = Thread.currentThread()
            val state = state
            
            // 情况1：锁未被占用（state == 0）
            if (state == 0) {
                // 使用 CAS 尝试获取锁
                if (compareAndSetState(0, acquires)) {
                    setExclusiveOwnerThread(current)  // 设置独占线程
                    Log.d(TAG, "线程 ${current.name} 获取锁成功")
                    return true
                }
            }
            // 情况2：锁已被当前线程占用（可重入）
            else if (current == exclusiveOwnerThread) {
                val nextState = state + acquires
                if (nextState < 0) {  // 溢出检查
                    throw Error("Maximum lock count exceeded")
                }
                setState(nextState)  // 增加重入次数
                Log.d(TAG, "线程 ${current.name} 重入锁，重入次数: $nextState")
                return true
            }
            
            return false  // 获取锁失败
        }
        
        /**
         * 尝试释放锁
         * @param releases 释放的锁数量（通常为 1）
         * @return true 如果锁完全释放（state == 0），false 否则
         */
        override fun tryRelease(releases: Int): Boolean {
            val current = Thread.currentThread()
            
            // 检查是否是锁的持有者
            if (current != exclusiveOwnerThread) {
                throw IllegalMonitorStateException("当前线程不是锁的持有者")
            }
            
            val state = this.state - releases  // 减少重入次数
            val free = state == 0
            
            if (free) {
                // 完全释放锁
                setExclusiveOwnerThread(null)
                Log.d(TAG, "线程 ${current.name} 完全释放锁")
            } else {
                Log.d(TAG, "线程 ${current.name} 释放锁，剩余重入次数: $state")
            }
            
            setState(state)  // 更新状态
            return free
        }
        
        /**
         * 判断当前线程是否独占锁
         */
        override fun isHeldExclusively(): Boolean {
            return exclusiveOwnerThread == Thread.currentThread()
        }
        
        /**
         * 获取当前持有锁的线程
         */
        fun getOwner(): Thread? {
            return exclusiveOwnerThread
        }
        
        /**
         * 获取重入次数
         */
        fun getHoldCount(): Int {
            return if (isHeldExclusively()) state else 0
        }
        
        /**
         * 公共方法：尝试获取锁（供外部类调用）
         */
        fun tryLock(): Boolean {
            return tryAcquire(1)
        }
        
        /**
         * 公共方法：获取状态（供外部类调用）
         */
        fun getStateValue(): Int {
            return state
        }
        
        /**
         * 公共方法：判断当前线程是否持有锁（供外部类调用）
         */
        fun isHeldByCurrentThread(): Boolean {
            return isHeldExclusively()
        }
    }
    
    /**
     * 获取锁（阻塞直到获取成功）
     */
    fun lock() {
        sync.acquire(1)
    }
    
    /**
     * 尝试获取锁（非阻塞）
     * @return true 如果成功获取锁，false 否则
     */
    fun tryLock(): Boolean {
        return sync.tryLock()
    }
    
    /**
     * 释放锁
     */
    fun unlock() {
        sync.release(1)
    }
    
    /**
     * 判断锁是否被占用
     */
    fun isLocked(): Boolean {
        return sync.getStateValue() != 0
    }
    
    /**
     * 判断当前线程是否持有锁
     */
    fun isHeldByCurrentThread(): Boolean {
        return sync.isHeldByCurrentThread()
    }
    
    /**
     * 获取当前持有锁的线程
     */
    fun getOwner(): Thread? {
        return sync.getOwner()
    }
    
    /**
     * 获取当前线程的重入次数
     */
    fun getHoldCount(): Int {
        return sync.getHoldCount()
    }
}

/**
 * 公平锁实现 - 基于 AQS
 * 
 * 公平锁保证按照线程请求锁的顺序获取锁（FIFO）
 */
class FairLock {
    private val sync = FairSync()
    
    companion object {
        private const val TAG = "FairLock"
    }
    
    /**
     * 公平锁同步器
     */
    private class FairSync : AbstractQueuedSynchronizer() {
        
        override fun tryAcquire(acquires: Int): Boolean {
            val current = Thread.currentThread()
            val state = state
            
            if (state == 0) {
                // 关键区别：检查队列中是否有等待的线程
                // 如果有，不能"插队"，必须排队
                if (!hasQueuedPredecessors() && compareAndSetState(0, acquires)) {
                    setExclusiveOwnerThread(current)
                    Log.d(TAG, "线程 ${current.name} 公平获取锁成功")
                    return true
                }
            } else if (current == exclusiveOwnerThread) {
                // 可重入
                val nextState = state + acquires
                if (nextState < 0) {
                    throw Error("Maximum lock count exceeded")
                }
                setState(nextState)
                Log.d(TAG, "线程 ${current.name} 公平重入锁，重入次数: $nextState")
                return true
            }
            
            return false
        }
        
        override fun tryRelease(releases: Int): Boolean {
            val current = Thread.currentThread()
            if (current != exclusiveOwnerThread) {
                throw IllegalMonitorStateException()
            }
            
            val state = this.state - releases
            val free = state == 0
            
            if (free) {
                setExclusiveOwnerThread(null)
                Log.d(TAG, "线程 ${current.name} 公平释放锁")
            }
            
            setState(state)
            return free
        }
        
        override fun isHeldExclusively(): Boolean {
            return exclusiveOwnerThread == Thread.currentThread()
        }
        
        /**
         * 公共方法：尝试获取锁（供外部类调用）
         */
        fun tryLock(): Boolean {
            return tryAcquire(1)
        }
        
        /**
         * 公共方法：获取状态（供外部类调用）
         */
        fun getStateValue(): Int {
            return state
        }
    }
    
    fun lock() {
        sync.acquire(1)
    }
    
    fun unlock() {
        sync.release(1)
    }
    
    fun tryLock(): Boolean {
        return sync.tryLock()
    }
    
    fun isLocked(): Boolean {
        return sync.getStateValue() != 0
    }
}

/**
 * 读写锁实现 - 基于 AQS
 * 
 * 支持多个读线程同时访问，但写线程独占访问
 */
class CustomReadWriteLock {
    private val readLock = ReadLock()
    private val writeLock = WriteLock()
    
    // 读写锁常量（移到外部，因为 inner class 不能有 companion object）
    companion object {
        private const val SHARED_SHIFT = 16
        private const val SHARED_UNIT = (1 shl SHARED_SHIFT)
        private const val MAX_COUNT = (1 shl SHARED_SHIFT) - 1
        private const val EXCLUSIVE_MASK = (1 shl SHARED_SHIFT) - 1
        
        // 获取读锁数量
        fun sharedCount(c: Int): Int = c ushr SHARED_SHIFT
        
        // 获取写锁状态
        fun exclusiveCount(c: Int): Int = c and EXCLUSIVE_MASK
    }
    
    /**
     * 读写锁同步器
     * state 的高 16 位表示读锁数量，低 16 位表示写锁状态
     */
    private inner class Sync : AbstractQueuedSynchronizer() {
        
        // 尝试获取读锁
        override fun tryAcquireShared(acquires: Int): Int {
            val current = Thread.currentThread()
            var c = state
            
            // 如果有写锁且不是当前线程持有，获取失败
            if (CustomReadWriteLock.exclusiveCount(c) != 0 && exclusiveOwnerThread != current) {
                return -1
            }
            
            val r = CustomReadWriteLock.sharedCount(c)
            if (r < MAX_COUNT && compareAndSetState(c, c + SHARED_UNIT)) {
                return 1  // 成功
            }
            
            return -1  // 失败
        }
        
        // 释放读锁
        override fun tryReleaseShared(releases: Int): Boolean {
            var c = state
            while (!compareAndSetState(c, c - SHARED_UNIT)) {
                c = state
            }
            return true
        }
        
        // 尝试获取写锁
        override fun tryAcquire(acquires: Int): Boolean {
            val current = Thread.currentThread()
            val c = state
            
            // 如果有读锁或写锁，获取失败
            if (c != 0) {
                if (exclusiveOwnerThread != current) {
                    return false
                }
                // 可重入
                if (CustomReadWriteLock.exclusiveCount(c) + acquires > MAX_COUNT) {
                    throw Error("Maximum lock count exceeded")
                }
                setState(c + acquires)
                return true
            }
            
            // 尝试获取写锁
            if (compareAndSetState(0, acquires)) {
                setExclusiveOwnerThread(current)
                return true
            }
            
            return false
        }
        
        // 释放写锁
        override fun tryRelease(releases: Int): Boolean {
            val current = Thread.currentThread()
            if (current != exclusiveOwnerThread) {
                throw IllegalMonitorStateException()
            }
            
            val c = state - releases
            val free = CustomReadWriteLock.exclusiveCount(c) == 0
            
            if (free) {
                setExclusiveOwnerThread(null)
            }
            
            setState(c)
            return free
        }
        
        override fun isHeldExclusively(): Boolean {
            return exclusiveOwnerThread == Thread.currentThread()
        }
    }
    
    /**
     * 读写锁同步器实例（共享）
     */
    private val sync = Sync()
    
    /**
     * 读锁
     */
    inner class ReadLock {
        fun lock() {
            sync.acquireShared(1)
        }
        
        fun unlock() {
            sync.releaseShared(1)
        }
    }
    
    /**
     * 写锁
     */
    inner class WriteLock {
        fun lock() {
            sync.acquire(1)
        }
        
        fun unlock() {
            sync.release(1)
        }
    }
    
    fun readLock() = readLock
    fun writeLock() = writeLock
}
```

```kotlin
      
package com.example.myapplication

import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private val TAG = "MyApplication"
    
    private lateinit var btnTestCustomLock: Button
    private lateinit var btnTestFairLock: Button
    private lateinit var btnTestReadWriteLock: Button
    private lateinit var tvLog: TextView
    
    // 测试用的锁实例
    private val customLock = CustomLock()
    private val fairLock = FairLock()
    private val readWriteLock = CustomReadWriteLock()
    
    // 共享资源
    private var counter = 0
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        Log.i(TAG,"MainActivity onCreate.....................")
        
        initViews()
    }
    
    private fun initViews() {
        btnTestCustomLock = findViewById(R.id.btnTestCustomLock)
        btnTestFairLock = findViewById(R.id.btnTestFairLock)
        btnTestReadWriteLock = findViewById(R.id.btnTestReadWriteLock)
        tvLog = findViewById(R.id.tvLog)
        
        btnTestCustomLock.setOnClickListener {
            testCustomLock()
        }
        
        btnTestFairLock.setOnClickListener {
            testFairLock()
        }
        
        btnTestReadWriteLock.setOnClickListener {
            testReadWriteLock()
        }
    }
    
    /**
     * 测试自定义锁
     */
    private fun testCustomLock() {
        appendLog("========== 测试自定义锁 ==========")
        counter = 0
        
        // 创建多个线程竞争锁
        val threads = mutableListOf<Thread>()
        
        for (i in 1..5) {
            val thread = Thread {
                customLock.lock()
                try {
                    appendLog("线程 $i 获取锁，开始操作")
                    val oldValue = counter
                    Thread.sleep(100)  // 模拟业务操作
                    counter = oldValue + 1
                    appendLog("线程 $i 完成操作，counter = $counter")
                } finally {
                    customLock.unlock()
                    appendLog("线程 $i 释放锁")
                }
            }
            thread.name = "Thread-$i"
            threads.add(thread)
        }
        
        threads.forEach { it.start() }
        
        // 等待所有线程完成
        Thread {
            threads.forEach { it.join() }
            runOnUiThread {
                appendLog("所有线程完成，最终 counter = $counter")
                appendLog("=====================================\n")
            }
        }.start()
    }
    
    /**
     * 测试可重入锁
     */
    private fun testReentrantLock() {
        appendLog("========== 测试可重入锁 ==========")
        
        Thread {
            customLock.lock()
            try {
                appendLog("外层获取锁")
                customLock.lock()  // 重入
                try {
                    appendLog("内层获取锁（重入）")
                    appendLog("重入次数: ${customLock.getHoldCount()}")
                } finally {
                    customLock.unlock()
                    appendLog("内层释放锁")
                }
            } finally {
                customLock.unlock()
                appendLog("外层释放锁")
            }
            appendLog("=====================================\n")
        }.start()
    }
    
    /**
     * 测试公平锁
     */
    private fun testFairLock() {
        appendLog("========== 测试公平锁 ==========")
        
        val threads = mutableListOf<Thread>()
        
        for (i in 1..5) {
            val thread = Thread {
                fairLock.lock()
                try {
                    appendLog("线程 $i 获取公平锁")
                    Thread.sleep(50)
                } finally {
                    fairLock.unlock()
                }
            }
            thread.name = "FairThread-$i"
            threads.add(thread)
        }
        
        // 按顺序启动，公平锁应该按顺序获取
        threads.forEach {
            it.start()
            Thread.sleep(10)  // 稍微延迟，确保按顺序请求
        }
        
        Thread {
            threads.forEach { it.join() }
            runOnUiThread {
                appendLog("公平锁测试完成")
                appendLog("=====================================\n")
            }
        }.start()
    }
    
    /**
     * 测试读写锁
     */
    private fun testReadWriteLock() {
        appendLog("========== 测试读写锁 ==========")
        
        var readValue = 0
        
        // 创建多个读线程
        val readThreads = mutableListOf<Thread>()
        for (i in 1..3) {
            val thread = Thread {
                readWriteLock.readLock().lock()
                try {
                    appendLog("读线程 $i 开始读取，值: $readValue")
                    Thread.sleep(200)  // 模拟读取操作
                    appendLog("读线程 $i 读取完成")
                } finally {
                    readWriteLock.readLock().unlock()
                }
            }
            thread.name = "ReadThread-$i"
            readThreads.add(thread)
        }
        
        // 创建写线程
        val writeThread = Thread {
            readWriteLock.writeLock().lock()
            try {
                appendLog("写线程开始写入")
                Thread.sleep(100)
                readValue = 100
                appendLog("写线程写入完成，新值: $readValue")
            } finally {
                readWriteLock.writeLock().unlock()
            }
        }
        writeThread.name = "WriteThread"
        
        // 启动所有线程
        readThreads.forEach { it.start() }
        Thread.sleep(50)  // 让读线程先启动
        writeThread.start()
        
        Thread {
            readThreads.forEach { it.join() }
            writeThread.join()
            runOnUiThread {
                appendLog("读写锁测试完成，最终值: $readValue")
                appendLog("=====================================\n")
            }
        }.start()
    }
    
    private fun appendLog(message: String) {
        runOnUiThread {
            val logMessage = "${System.currentTimeMillis() % 100000}: $message\n"
            tvLog.append(logMessage)
            Log.i(TAG, message)
        }
    }
//    onCreate -> onStart -> onResume-> onPostResume

    override fun onStart() {
        super.onStart()
        Log.i(TAG,"MainActivity onStart.....................")
    }

    override fun onResume() {
        super.onResume()
        Log.i(TAG,"MainActivity onResume.....................")
    }

    override fun onRestart() {
        super.onRestart()
        Log.i(TAG,"MainActivity onRestart.....................")
    }

    override fun onPause() {
        super.onPause()
        Log.i(TAG,"MainActivity onPause.....................")
    }

    override fun onStop() {
        super.onStop()
        Log.i(TAG,"MainActivity onStop.....................")
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG,"MainActivity onDestroy.....................")
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        Log.i(TAG,"MainActivity onSaveInstanceState.....................")
    }

    override fun onRestoreInstanceState(savedInstanceState: Bundle) {
        super.onRestoreInstanceState(savedInstanceState)
        Log.i(TAG,"MainActivity onRestoreInstanceState.....................")
    }


    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        Log.i(TAG,"MainActivity onConfigurationChanged.....................")
    }


}
```