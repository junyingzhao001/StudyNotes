# ReentrantLock 源码解析

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

## 1、概述

ReentrantLock 是 Java 并发包 (`java.util.concurrent.locks`) 中提供的一个可重入的互斥锁实现。它是 **Lock** 接口的实现类，提供了比 **synchronized** 关键字更灵活的锁机制。

### **1.1 什么是可重入锁？**

可重入锁（Reentrant Lock）是指同一个线程可以多次获取同一把锁，而不会造成死锁。例如：

```java
public void method1() {
    lock.lock();
    try {
        method2(); // 可以再次获取同一把锁
    } finally {
        lock.unlock();
    }
}

public void method2() {
    lock.lock(); // 同一个线程可以再次获取锁
    try {
        // 业务逻辑
    } finally {
        lock.unlock();
    }
}
```

## 2、核心数据结构

### **2.1 类结构**

```java
public class ReentrantLock implements Lock, java.io.Serializable {
    private static final long serialVersionUID = 7373984872572414699L;
    
    /** 同步器，负责实现锁的核心逻辑 */
    private final Sync sync;
    
    /**
     * 抽象同步器基类
     * 继承自 AbstractQueuedSynchronizer (AQS)
     */
    abstract static class Sync extends AbstractQueuedSynchronizer {
        // ...
    }
    
    /**
     * 非公平锁实现
     */
    static final class NonfairSync extends Sync {
        // ...
    }
    
    /**
     * 公平锁实现
     */
    static final class FairSync extends Sync {
        // ...
    }
}
```

### **2.2 关键字段说明**

- **Sync sync**: 核心同步器，所有锁操作都委托给它
- **AbstractQueuedSynchronizer (AQS)**: 底层同步框架，提供了锁的排队、阻塞、唤醒机制

## 3、核心方法源码分析

### **3.1 构造函数**

```java
// 默认创建非公平锁
public ReentrantLock() {
    sync = new NonfairSync();
}

// 根据参数决定创建公平锁还是非公平锁
public ReentrantLock(boolean fair) {
    sync = fair ? new FairSync() : new NonfairSync();
}
```

**公平锁 vs 非公平锁：**

- **公平锁**：按照线程请求锁的顺序获取锁，先到先得
- **非公平锁**：允许"插队"，新来的线程可能直接获取锁，性能更好但可能导致饥饿

### **3.2 lock() 方法 - 非公平锁实现**

```java
// ReentrantLock.lock()
public void lock() {
    sync.lock();
}

// NonfairSync.lock()
final void lock() {
    // 1. 首先尝试直接获取锁（CAS 操作）
    if (compareAndSetState(0, 1))
        setExclusiveOwnerThread(Thread.currentThread());
    else // 2. 如果失败，调用 AQS 的 acquire 方法
        acquire(1);
}
```

**执行流程：**

1. **快速路径**：使用 CAS（Compare-And-Swap）尝试将 state 从 0 改为 1

   - 如果成功，设置当前线程为独占线程
   - 如果失败，进入慢速路径
2. **慢速路径**：调用 **`acquire`**`(1)`

```java
// AbstractQueuedSynchronizer.acquire()
public final void acquire(int arg) {
    if (!tryAcquire(arg) &&           // 尝试获取锁
        acquireQueued(addWaiter(Node.EXCLUSIVE), arg))  // 加入队列并等待
        selfInterrupt();
}
```

### **3.3 tryAcquire() 方法 - 非公平锁实现**

```java
// NonfairSync.tryAcquire()
protected final boolean tryAcquire(int acquires) {
    return nonfairTryAcquire(acquires);
}

// Sync.nonfairTryAcquire()
final boolean nonfairTryAcquire(int acquires) {
    final Thread current = Thread.currentThread();
    int c = getState();  // 获取当前锁状态
    // 情况1：锁未被占用（state == 0）
    if (c == 0) {
        // 尝试获取锁
        if (compareAndSetState(0, acquires)) {
            setExclusiveOwnerThread(current);
            return true;
        }
    }
    // 情况2：锁已被当前线程占用（可重入）
    else if (current == getExclusiveOwnerThread()) {
        int nextc = c + acquires;  // 重入次数 +1
        if (nextc < 0)  // 溢出检查
            throw new Error("Maximum lock count exceeded");
        setState(nextc);  // 更新重入次数
        return true;
    }
    return false;  // 获取锁失败
}
```

**关键点：**

- **state = 0**：锁未被占用
- **state > 0**：锁被占用，值表示重入次数
- **可重入机制**：如果当前线程已持有锁，直接增加重入次数

### **3.4 tryAcquire() 方法 - 公平锁实现**

```java
// FairSync.tryAcquire()
protected final boolean tryAcquire(int acquires) {
    final Thread current = Thread.currentThread();
    int c = getState();
    
    if (c == 0) {
        // 关键区别：检查队列中是否有等待的线程
        if (!hasQueuedPredecessors() &&  // 没有前驱节点
            compareAndSetState(0, acquires)) {
            setExclusiveOwnerThread(current);
            return true;
        }
    }
    else if (current == getExclusiveOwnerThread()) {
        int nextc = c + acquires;
        if (nextc < 0)
            throw new Error("Maximum lock count exceeded");
        setState(nextc);
        return true;
    }
    return false;
}

// 检查是否有前驱节点在等待
public final boolean hasQueuedPredecessors() {
    Node t = tail;
    Node h = head;
    Node s;
    return h != t &&
        ((s = h.next) == null || s.thread != Thread.currentThread());
}
```

**公平锁的关键：**

- 在尝试获取锁之前，先检查队列中是否有其他线程在等待
- 如果有，则不能"插队"，必须排队

### **3.5 unlock() 方法**

```java
// ReentrantLock.unlock()
public void unlock() {
    sync.release(1);
}

// AbstractQueuedSynchronizer.release()
public final boolean release(int arg) {
    if (tryRelease(arg)) {  // 尝试释放锁
        Node h = head;
        if (h != null && h.waitStatus != 0)
            unparkSuccessor(h);  // 唤醒等待队列中的下一个线程
            return true;
    }
    return false;
}

// Sync.tryRelease()
protected final boolean tryRelease(int releases) {
    int c = getState() - releases;  // 减少重入次数
    // 检查是否是锁的持有者
    if (Thread.currentThread() != getExclusiveOwnerThread())
        throw new IllegalMonitorStateException();
    
    boolean free = false;
    if (c == 0) {  // 完全释放锁
        free = true;
        setExclusiveOwnerThread(null);
    }
    setState(c);  // 更新状态
    return free;
}
```

**释放流程：**

1. 减少重入次数（state - 1）
2. 如果 state 变为 0，完全释放锁
3. 唤醒等待队列中的下一个线程

## 4、AQS (AbstractQueuedSynchronizer) 核心机制

### **4.1 等待队列（CLH 队列）**

AQS 使用双向链表实现的等待队列：

```java
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

### **4.2 acquireQueued() - 排队等待**

```java
// AbstractQueuedSynchronizer.acquireQueued()
final boolean acquireQueued(final Node node, int arg) {
    boolean failed = true;
    try {
        boolean interrupted = false;
        for (;;) {  // 自旋
        final Node p = node.predecessor();
            // 如果前驱是头节点，尝试获取锁
            if (p == head && tryAcquire(arg)) {
                setHead(node);  // 成为新的头节点
                p.next = null;  // 帮助 GC
                failed = false;
                return interrupted;
            }
            // 检查是否需要阻塞，如果需要则阻塞
            if (shouldParkAfterFailedAcquire(p, node) &&
                parkAndCheckInterrupt())
                interrupted = true;
        }
    } finally {
        if (failed)
            cancelAcquire(node);
    }
}
```

**执行流程：**

1. 自旋检查：前驱节点是否为头节点
2. 如果是头节点，尝试获取锁
3. 如果获取失败，检查是否需要阻塞
4. 使用 **LockSupport**.park() 阻塞当前线程

### **4.3 阻塞与唤醒**

```java
// 阻塞当前线程
private final boolean parkAndCheckInterrupt() {
    LockSupport.park(this);  // 阻塞线程
    return Thread.interrupted();  // 检查是否被中断
}

// 唤醒后继节点
private void unparkSuccessor(Node node) {
    int ws = node.waitStatus;
    if (ws < 0)
        compareAndSetWaitStatus(node, ws, 0);
    
    Node s = node.next;
    if (s == null || s.waitStatus > 0) {
        s = null;
        // 从尾部向前查找可唤醒的节点
        for (Node t = tail; t != null && t != node; t = t.prev)
            if (t.waitStatus <= 0)
                s = t;
    }
    if (s != null)
        LockSupport.unpark(s.thread);  // 唤醒线程
}
```

## 5、LockSupport详解

### 5.1、核心功能

park(): 阻塞当前线程

unpark(Thread): 唤醒指定线程

parkNanos(): 阻塞指定时间（纳秒）

parkUntil(): 阻塞直到指定时间点

LockSupport 提供了更底层、更灵活的线程阻塞/唤醒机制，不需要获取锁，使用更简单

### 5.2、Permit（许可）

LockSupport 使用一个\*\*虚拟的许可（permit）\*\*机制：

每个线程都有一个 permit，值为 0 或 1

unpark() 给线程一个 permit（如果已有则保持为 1）

park() 消耗一个 permit，如果没有 permit 则阻塞

关键特性：先 unpark 后 park 不会阻塞

### 5.3、Unsafe 的作用

直接内存操作（绕过 Java 安全检查）

CAS 操作

线程阻塞/唤醒的底层实现

字段偏移量获取

### 5.4、park(Object blocker) - 阻塞线程（带 blocker）

```java
public static void park(Object blocker) {
    Thread t = Thread.currentThread();
    setBlocker(t, blocker);  // 设置阻塞原因
    try {
        if (t.isVirtual()) {
            VirtualThreads.park();  // 虚拟线程
        } else {
            U.park(false, 0L);  // 平台线程：相对时间，无限期阻塞
        }
    } finally {
        setBlocker(t, null);  // 清除 blocker
    }
}
```

底层代码是c++

```cpp
// hotspot/src/os/linux/vm/os_linux.cpp

void Parker::park(bool isAbsolute, jlong time) {
    // 1. 检查 permit
    if (_counter > 0) {
        _counter = 0;
        _cur_index = -1;
        return;
    }
    
    // 2. 计算超时时间
    timespec absTime;
    if (time < 0 || (isAbsolute && time == 0)) {
        // 无限期阻塞
        absTime.tv_sec = 0;
        absTime.tv_nsec = 0;
    } else {
        // 计算绝对时间
        unpackTime(&absTime, isAbsolute, time);
    }
    
    // 3. 使用 pthread_cond_timedwait 阻塞
    pthread_mutex_lock(_mutex);
    _cur_index = REL_INDEX;
    pthread_cond_timedwait(_cond[_cur_index], _mutex, &absTime);
    pthread_mutex_unlock(_mutex);
    
    _cur_index = -1;
}

```



### 5.5、unpark(Thread thread) - 唤醒线程

```cpp
public static void unpark(Thread thread) {
    if (thread != null) {
        if (thread.isVirtual()) {
            VirtualThreads.unpark(thread);  // 虚拟线程（Java 19+）
        } else {
            U.unpark(thread);  // 平台线程
        }
    }
}
```

底层代码是c++

```cpp
// hotspot/src/os/linux/vm/os_linux.cpp
void Parker::unpark() {
    int s, status;
    status = pthread_mutex_lock(_mutex);
    assert_status(status == 0, status, "mutex_lock");
    s = _counter;
    _counter = 1;  // 设置 permit
    if (s < 1) {
        // 唤醒等待的线程
        if (_cur_index != -1) {
            pthread_cond_signal(_cond[_cur_index]);
        }
    }
    status = pthread_mutex_unlock(_mutex);
    assert_status(status == 0, status, "mutex_unlock");
}
```



## 6、与 synchronized 的对比

| 维度 | `ReentrantLock` | `synchronized` |
| --- | --- | --- |
| **锁类型** | 显式锁，需要手动释放 | 隐式锁，自动释放 |
| **灵活性** | 支持公平/非公平锁 | 只支持非公平锁 |
| **功能** | 支持尝试获取、超时、中断 | 不支持 |
| **性能** | Java 6+ 性能相当 | Java 6+ 性能相当 |
| **可读性** | 代码更复杂 | 代码更简洁 |
| **异常处理** | 必须在 finally 中释放 | 自动释放 |

## 7、使用示例

### **7.1 基本使用**

```java
ReentrantLock lock = new ReentrantLock();

public void method() {
    lock.lock();  // 获取锁
    try {
        // 临界区代码
        doSomething();
    } finally {
        lock.unlock();  // 必须在 finally 中释放锁
    }
}
```

### **7.2 尝试获取锁（非阻塞）**

```java
if (lock.tryLock()) {
    try {
        // 获取锁成功，执行代码
    } finally {
        lock.unlock();
    }
} else {
    // 获取锁失败，执行其他逻辑
}
```

### **7.3 超时获取锁**

```java
try {
    if (lock.tryLock(5, TimeUnit.SECONDS)) {
        try {
            // 获取锁成功
        } finally {
            lock.unlock();
        }
    } else {
        // 超时未获取到锁
    }
} catch (InterruptedException e) {
    // 被中断
    Thread.currentThread().interrupt();
}
```

### **7.4 可中断获取锁**

```java
try {
    lock.lockInterruptibly();  // 可中断地获取锁
    try {
        // 临界区代码
    } finally {
        lock.unlock();
    }
} catch (InterruptedException e) {
    // 被中断时的处理
    Thread.currentThread().interrupt();
}
```

### **7.5 公平锁使用**

```java
// 创建公平锁
ReentrantLock fairLock = new ReentrantLock(true);

fairLock.lock();
try {
    // 按照请求顺序获取锁
} finally {
    fairLock.unlock();
}
```

## 8、常见问题

### **8.1 为什么需要可重入？**

```java
public class Example {
    private ReentrantLock lock = new ReentrantLock();
    
    public void method1() {
        lock.lock();
        try {
            method2();  // 调用 method2，需要再次获取锁
        } finally {
            lock.unlock();
        }
    }
    
    public void method2() {
        lock.lock();  // 如果是不可重入锁，这里会死锁
        try {
            // 业务逻辑
        } finally {
            lock.unlock();
        }
    }
}
```

### **8.2 死锁预防**

```java
// 错误示例：可能导致死锁
ReentrantLock lock1 = new ReentrantLock();
ReentrantLock lock2 = new ReentrantLock();

// 线程1
lock1.lock();
lock2.lock();  // 如果线程2已持有 lock2，会死锁

// 线程2
lock2.lock();
lock1.lock();  // 如果线程1已持有 lock1，会死锁

// 正确做法：按固定顺序获取锁// 所有线程都按 lock1 -> lock2 的顺序获取
```

### **8.3 性能考虑**

- **非公平锁**：性能更好，适合大多数场景
- **公平锁**：保证公平性，但性能略差
- **synchronized**：在 Java 6+ 中性能已优化，与 ReentrantLock 相当
