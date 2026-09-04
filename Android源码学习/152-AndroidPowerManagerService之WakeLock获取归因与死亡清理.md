# 152 Android PowerManagerService：WakeLock 获取、归因与死亡清理

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读源码，不编译、不连接设备  
> 前置章节：第 25、68、69、72、150 章

---

## 1. 本章只回答一个问题

第 150 章里，AlarmManagerService 用一把 `*alarm*` PARTIAL WakeLock 保护全部 InFlight。现在向下一层追：

> 一次 `WakeLock.acquire()` 怎样从客户端引用，变成 system_server 中的一条 token 记录，再汇总成阻止 kernel suspend 的共享 blocker；正常 release、超时与进程死亡又分别在哪一层销账？

完整主线是：

```text
客户端WakeLock对象
  → 本地计数决定是否跨Binder
  → PMS按Binder token建一条记录
  → owner状态决定PARTIAL是否有效
  → 全部有效记录OR成mWakeLockSummary
  → 共享SuspendBlocker约束kernel suspend
  → release或Binder death删记录并重算
```

一句话结论：

> “持有 WakeLock”至少有客户端声明、服务端登记、政策有效、summary 贡献和 kernel blocker 五层含义；这五层既不一一对应，也不能用一个 `isHeld()` 互相替代。

## 2. WakeLock 不是 Java 锁，也不是进程保活

核心源码：

```text
frameworks/base/core/java/android/os/PowerManager.java
frameworks/base/core/java/android/os/IPowerManager.aidl
frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
frameworks/base/services/core/java/com/android/server/power/Notifier.java
frameworks/base/services/core/java/com/android/server/power/WakeLockLog.java
frameworks/base/services/core/jni/com_android_server_power_PowerManagerService.cpp
```

`synchronized` 和 `ReentrantLock` 保护共享数据；WakeLock 向电源状态机提交约束。PARTIAL 只要求 CPU 在系统尊重该记录时保持可运行，不保证：

- 某个 Java 线程立即得到调度；
- 网络持续可用；
- App 进程不被杀；
- 屏幕点亮或保持 interactive；
- Doze 与后台政策被绕过。

常见 level 在 PMS 中先映射为 summary bit：

| level | 初始 summary 贡献 | 边界 |
|---|---|---|
| `PARTIAL_WAKE_LOCK` | `WAKE_LOCK_CPU` | App UID 可被动态 disabled |
| `SCREEN_DIM_WAKE_LOCK` | screen dim | deprecated |
| `SCREEN_BRIGHT_WAKE_LOCK` | screen bright | deprecated |
| `FULL_WAKE_LOCK` | screen + button bright | deprecated |
| `PROXIMITY_SCREEN_OFF_WAKE_LOCK` | proximity | 还受设备配置影响 |
| `DOZE_WAKE_LOCK` | doze | 内部能力，另需 `DEVICE_POWER` |
| `DRAW_WAKE_LOCK` | draw | 内部能力，之后会隐含 CPU |

level 位于 `flags & WAKE_LOCK_LEVEL_MASK`，高位还可带 acquire/release 行为 flag。

## 3. `newWakeLock()` 只创建客户端 token

```java
public WakeLock newWakeLock(int levelAndFlags, String tag) {
    validateWakeLockParameters(levelAndFlags, tag);
    return new WakeLock(levelAndFlags, tag,
            mContext.getOpPackageName());
}
```

构造函数只保存 flags、tag、package，并创建：

```java
mToken = new Binder();
```

此时没有 Binder 调用，也没有服务端记录。tag 只是诊断标签；真正的主键是这个 Binder token。两个对象即使 tag 相同，也会有不同 token 和两条 PMS 记录。

客户端还保存：

```text
mInternalCount / mExternalCount
mRefCounted（默认true）
mHeld
mWorkSource / mHistoryTag
```

这些字段都在 `synchronized (mToken)` 下维护；PMS 看不到客户端计数。

## 4. 默认引用计数怎样折叠 Binder 调用

`acquireLocked()` 先增加两张本地账：

```java
mInternalCount++;
mExternalCount++;
if (!mRefCounted || mInternalCount == 1) {
    mService.acquireWakeLock(mToken, ...);
    mHeld = true;
}
```

默认引用计数下：

```text
acquire #1 → internal 0→1，Binder acquire
acquire #2 → internal 1→2，仅本地加数
release #1 → internal 2→1，仅本地减数
release #2 → internal 1→0，Binder release
```

`mExternalCount` 只由显式 acquire/release 平衡；timeout release 不减它：

```java
if (mInternalCount > 0) mInternalCount--;
if ((flags & RELEASE_FLAG_TIMEOUT) == 0) mExternalCount--;
```

这样 `acquire(10s)` 超时使 internal 归零并撤销服务端记录后，业务代码仍可显式 `release()` 把 external 从 1 还到 0，而不会误报 under-lock。再多 release 一次，客户端才抛 `WakeLock under-locked`；这只是该 Java 对象的外部引用账错误，不是 kernel 负引用。

## 5. timed 与非引用模式有哪些非直觉行为

`acquire(timeout)` 在 acquire 后向 `PowerManager` 持有的 Handler 投递 `mReleaser`。标准 `SystemServiceRegistry` 传入 `ActivityThread` 主 Handler，所以 timeout 不是 PMS 或 kernel 的硬截止：主 Looper 堵塞会让它迟到，Handler 的 uptime 时间基准也会在 CPU suspend 时停止推进。

默认引用模式的第二次 acquire 不进入 Binder 分支，也不会移除旧 callback，因此多个 timed 引用可各有一个 timeout：

```text
t0 acquire(10s) → internal=1，服务端一条记录
t1 acquire(20s) → internal=2，仍是一条记录
t10 timeout     → internal=1，继续持有
t21 timeout     → internal=0，Binder release
```

改成 `setReferenceCounted(false)` 后，每次 acquire 都跨 Binder，并先移除旧的 `mReleaser`；对 repeated timed acquire，更接近“后一次 timeout 替换前一次”。任意一次 release 都足以撤销 `mHeld`，虽然本地两个 count 仍会变化且不再做 external under-lock 检查。

非引用模式之所以每次都重发，是为了允许客户端在不知道旧记录曾被内部强制释放时重新建立状态。但 PMS 仍只按同一 token 更新一条记录，不会因此出现多条。

## 6. `mHeld` 与可变属性都只是客户端视图

`isHeld()` 只返回本地 `mHeld`，不会查询 system_server。`true` 只能说明这个对象曾成功走过 acquire 且本地尚未走到 release 边沿，不能证明服务端记录当前未 disabled、summary 含 CPU bit 或 kernel blocker 已持有。

`finalize()` 发现 `mHeld` 时会 wtf 并尝试 Binder release，但 GC 时机不确定。业务代码仍应把 acquire/release 放在可证明的生命周期或 `try/finally` 中，timeout 只是兜底。

r48 的隐藏属性 setter 还有传播边界：

- `setWorkSource()` 在值变化且 held 时会立即调用服务端更新；
- `setTag()`、`setHistoryTag()`、`setUnimportantForLogging()` 只改本地字段；
- 默认引用模式下，held 时再 acquire 可能只加本地计数，PMS 仍看到旧属性；
- 客户端 trace name 在构造时由原 tag 固定，后续 `setTag()` 不重建它。

服务端 `hasSameProperties()` 又不比较 packageName 和 historyTag。同 token 只改 historyTag 再 acquire 会被视为“属性相同”而忽略；若同时改了其他被比较字段，才会进入更新路径。诊断 tag、trace、BatteryStats history 时要区分这几个版本化视图。

## 7. Binder 入口怎样确定 owner 与归因权限

AIDL 传递：

```aidl
void acquireWakeLock(IBinder lock, int flags, String tag,
    String packageName, in WorkSource ws, String historyTag);
```

PMS 依次验证 token/package 非 null、level/tag 合法、`WAKE_LOCK` 权限；DOZE level 额外要求 `DEVICE_POWER`，非空 WorkSource 额外要求 `UPDATE_DEVICE_STATS`。空 WorkSource 会规范成 null。

真实 owner 从 Binder 上下文取得：

```java
int uid = Binder.getCallingUid();
int pid = Binder.getCallingPid();
long ident = Binder.clearCallingIdentity();
try {
    acquireWakeLockInternal(..., uid, pid);
} finally {
    Binder.restoreCallingIdentity(ident);
}
```

权限和 owner 取值都在 clear identity 前完成；之后内部跨服务调用虽以 system_server 身份运行，记录中的 owner 仍是原 calling UID/PID。

tag、historyTag、packageName 与 WorkSource 是参数，不是驱动认证身份。标准 `PowerManager` 使用 context 的 op package；PMS 这一入口本身只检查 packageName 非 null，没有在此处把它重新解析成 calling UID。安全与政策判断不能拿字符串 package 替代 owner UID。

release/update 同样以 token 找资源，而不重新要求 calling UID 等于 owner。Binder token 在这里是一项能力；正常 App 只把自己的私有 token 交给 `PowerManager`。

## 8. PMS 为什么是一 token 一记录

查找使用 system_server 中的 Binder 对象身份：

```java
if (mWakeLocks.get(i).mLock == lock) return i;
```

新 token 会：

```text
取得或创建owner UidState
  → UidState.mNumWakeLocks++
  → 构造服务端WakeLock并复制WorkSource
  → linkToDeath
  → 成功后加入mWakeLocks
  → 计算初始disabled状态
```

先 link 再加入，保证跨进程 token 发布进列表前已有死亡监控；若 token 已死，抛 `IllegalArgumentException`，不留下无法回收的记录。

同 token 再 acquire 不增加服务端 count。属性不同则先通知统计层 changing，再更新同一对象；owner UID/PID 与已保存 package 在真正进入 `updateProperties()` 时禁止改变。

但“同 token 幂等”不能理解得太强：无论记录是否已存在，PMS 都会再次执行 acquire flag、副作用、置 `DIRTY_WAKE_LOCKS` 和状态重算。带 `ACQUIRE_CAUSES_WAKEUP` 的非引用模式 repeated acquire 可能再次触发 wakeup 路径；只是不会多一条记录或重复发送普通 acquired 统计。

## 9. acquire flag 与电源约束的完成点不同

API 文档说 `ACQUIRE_CAUSES_WAKEUP` 和 `ON_AFTER_RELEASE` 不能与 PARTIAL 组合，但 `validateWakeLockParameters()` 只校验 level 与 tag，并不拒绝该组合。PMS 真正应用时再要求 `isScreenLock()`：

```text
ACQUIRE_CAUSES_WAKEUP + FULL/BRIGHT/DIM
  → wakeUpNoUpdateLocked()

ON_AFTER_RELEASE + FULL/BRIGHT/DIM
  → userActivityNoUpdateLocked(NO_CHANGE_LIGHTS)
```

所以给 PARTIAL 添加这两个 flag，在 r48 通常是被实现条件忽略，不是在构造时抛异常。PARTIAL 本身不会点亮屏幕。

新记录完成后，PMS 的顺序是：

```java
applyWakeLockFlagsOnAcquireLocked(...);
mDirty |= DIRTY_WAKE_LOCKS;
updatePowerStateLocked();
notifyWakeLockAcquiredLocked(...);
```

普通 acquired 统计放在电源状态更新之后，源码明确说这是为了先建立 kernel stay-awake 条件，再开始 BatteryStats 记账。`ACQUIRE_CAUSES_WAKEUP` 属于更早的 wakefulness 输入，两者不能混成同一个完成点。

释放 screen lock 的 `ON_AFTER_RELEASE` 只是刷新用户活动计时，不会把已经 ASLEEP 的设备主动唤醒。

## 10. owner 与 WorkSource 是两本不同的账

服务端记录同时保留：

| 字段 | 由谁决定 | 主要用途 |
|---|---|---|
| owner UID/PID | Binder calling 身份 | 所有权、UID政策、进程状态 |
| packageName | 客户端参数 | 诊断、无 WorkSource 时 AppOps |
| WorkSource/WorkChain | 有权限的客户端参数 | BatteryStats 与 profile 归因 |
| token | Binder 对象 | 查找、release、death cleanup |

AlarmManagerService 的 `*alarm*` 就是典型代理：owner 是 system_server，WorkSource 或已知 creator UID 指向业务 App。WorkSource 改变“能耗记给谁”，不会改 owner、token death 归属或 disabled 政策。

held 中更新 WorkSource 时，客户端和值都发生变化才调用 `updateWakeLockWorkSource()`；PMS 找不到 token 会抛异常。Notifier 在旧、新 WorkSource 都非 null且 monitor type 有效时用 `noteChangeWakelockFromSource()`，否则退化为旧 release 加新 acquire。属性 change 还会结束旧 long-partial 段并重新开始计时。

非空 WorkSource 不走 owner 的 `OP_WAKE_LOCK` AppOp start/finish 分支，而走 source-aware BatteryStats API；这不代表 owner 身份被替换。

## 11. PARTIAL 为什么可能“记录还在但不生效”

`setWakeLockDisabledStateLocked()` 只处理 App UID 的 PARTIAL。默认 `NO_CACHED_WAKE_LOCKS=true` 时，以下条件可让它 disabled：

- force suspend 期间的 App UID；
- owner UID inactive，且 procState 比 RECEIVER 更后台；
- Device Idle 中 owner App 不在永久/临时白名单，且 procState 比 BOUND_FOREGROUND_SERVICE 更后台。

判断使用 `mOwnerUid` 与该 owner 的 `UidState`，不读取 WorkSource UID。系统服务不能仅靠把账归给前台 App，就绕过自己的 owner 政策；反过来，system_server owner 的代理 WakeLock 也不会因 WorkSource App 进入 cached 自动 disabled，代理服务必须自己闭合业务生命周期。

disabled 的状态转换是：

```text
记录仍留在mWakeLocks
  → mDisabled=true
  → Notifier按release结算有效段
  → 不再贡献WAKE_LOCK_CPU

owner重新允许
  → 同一记录mDisabled=false
  → Notifier重新按acquire开始新段
```

`uidGoneInternal()` 不是资源删除：它把对应 UidState 标为 NONEXISTENT/inactive，从全局 `mUidState` 表移除；只有 Device Idle 且旧 state 仍有记录时才触发一次重评。disabled 条件又明确排除 NONEXISTENT，因而 UID gone 本身不等于禁用或清除 WakeLock，具体记录仍依赖 Binder death。这是 r48 很重要的两条清理链分工。

## 12. 多条记录怎样汇总成共享 SuspendBlocker

PMS 遍历 `mWakeLocks`，把每个 level 的结果按位 OR 到 `mWakeLockSummary`。只有未 disabled 的 PARTIAL 才返回 CPU bit；screen、proximity、doze、draw 产生各自 bit。

之后 `adjustWakeLockSummaryLocked()` 按 wakefulness 修正：

- 非 DOZING 去掉 DOZE 与 DRAW；
- ASLEEP 或存在 DOZE 时去掉 screen bright/dim/button；
- ASLEEP 再去掉 proximity；
- AWAKE 的 screen bright/dim 隐含 CPU 与 STAY_AWAKE；
- DREAMING 的 screen bright/dim 隐含 CPU；
- DRAW 隐含 CPU。

所以一条服务端记录存在，不代表当前 wakefulness 下必然贡献它名字暗示的效果。

`updateSuspendBlockerLocked()` 最终只看聚合条件：

```text
summary含WAKE_LOCK_CPU
  → 持有共享mWakeLockSuspendBlocker

display未ready、亮屏/转换、特定doze显示等
  → 持有独立mDisplaySuspendBlocker
```

多个 App PARTIAL 与 `*alarm*` 可以共同令同一个 WakeLock SuspendBlocker 保持 acquired，而不会在 native 层逐项建立同名锁。JNI 最终对 blocker 名调用 `acquire_wake_lock(PARTIAL_WAKE_LOCK, name)`；当全局 CPU bit 消失才释放。

释放最后一个 PARTIAL 后设备也不保证立即 suspend：Display blocker、autosuspend 配置和其他状态仍可能阻止它。

## 13. release 与 Binder death 怎样只清一次

服务端 release 找到 token 后直接：

```text
unlinkToDeath
  → 从mWakeLocks删除记录
  → owner UidState.mNumWakeLocks--
  → Notifier结算有效/long统计
  → 应用ON_AFTER_RELEASE
  → DIRTY_WAKE_LOCKS并重算
```

服务端没有引用计数。自定义 AIDL 调用者若用同 token acquire 两次、release 一次，记录就会删除；普通 Java 客户端必须先在本地折叠引用。

未知 token 的 release 只在 DEBUG_SPEW 下记日志并返回，不向调用者抛“未持有”。没有服务端异常不能证明客户端 external count 已配平。

服务端 WakeLock 实现 `IBinder.DeathRecipient`。进程死亡后，`binderDied()` 按具体 WakeLock 对象在列表中查找并调用相同的 `removeWakeLockLocked()`。正常 release 与 death 都持 PMS `mLock`：谁先删，另一条路径随后都找不到，因此最多清一次。

death 能兜底崩溃、被杀和未执行 finally 的进程终止，却不能处理“进程活着但忘记 release”。若 system_server 代表 App 持锁，token 属于 system_server；业务 App 死亡不会自动杀死该 token，系统服务仍需用完成回调、timeout 或生命周期监听自行释放。

## 14. Notifier、长锁和 dumpsys 分别提供什么证据

只有 `mSystemReady && !mDisabled` 的记录才进入 `notifyWakeLockAcquiredLocked()`。Notifier 将 level 映射到 BatteryStats monitor type；无 WorkSource 时还启停 owner 的 `OP_WAKE_LOCK` AppOp，有 WorkSource 时用 source-aware BatteryStats 入口。

有效 PARTIAL 会用 uptime 记录 `mAcquireTime`，并安排约 60 秒 long check。超过阈值仍 notified-acquired 且未 marked-long，才写 BatteryStats/statsd long-partial start；release、change 或 disabled 写 finish。

这只是统计，不会自动释放 WakeLock。它以 uptime 计时也很关键：描述的是 CPU 可运行的有效段，而不是 token 自登记以来的 wall duration；disabled 后重新 enabled 会重新起算。

`dumpsys power` 中应联合看：

```text
mWakeLockSummary
Wake Locks: size=N
  PARTIAL_WAKE_LOCK 'tag' ... DISABLED ACQ=-1m23s LONG
Suspend Blockers: size=N
  PowerManagerService.WakeLocks: ref count=...
```

`DISABLED` 表示记录存在但不被尊重；`ACQ=` 只在已通知 acquired 时出现，并以当前 uptime 为参照显示过去多久；`LONG` 表示长锁事件已开始。owner uid/pid 与 `ws=` 必须分别读。

WakeLockLog 是有限容量的 acquire/release 历史，当前 `mWakeLocks` 才回答“此刻登记什么”。客户端 internal/external count 不会出现在 PMS dump，因为从未跨 Binder 传入。

## 15. 用四个场景检验五层模型

场景一：`isHeld()==true`，但 CPU 仍可 suspend。

先看 PMS 是否有该 token 对应记录，再看是否 `DISABLED`、summary 是否含 CPU bit、WakeLock SuspendBlocker 是否持有。若 owner 是 cached inactive 或 Doze 非白名单后台，客户端声明和服务端有效性可以同时为真/假不同组合。

场景二：同一 tag 在 dump 中出现两次。

tag 不是键。比较 owner uid/pid、WorkSource、flags、ACQ 时间和 token 来源；两个 Java WakeLock 对象或两个进程使用同一 tag 都会形成两条记录，不能直接诊断成重复引用。

场景三：system_server 的 tag 指向业务 App，WorkSource 也指向它。

owner 仍是 system_server，death 也绑定 system_server token。业务 App 死亡不会自动撤销；应回到代理服务的 InFlight/任务账查释放协议。

场景四：release 后屏幕仍亮。

先确认目标记录已删除，再检查其他 screen WakeLock、user activity timeout、`ON_AFTER_RELEASE`、Display SuspendBlocker、brightness boost 和 display transition。撤销一条约束不等于系统立即收敛到 ASLEEP。

## 16. 静态验证与结论

先手算客户端引用：

```bash
sed -n '2320,2495p' \
  frameworks/base/core/java/android/os/PowerManager.java
```

分别推演 `acquire/acquire/release/release`、两个 timed acquire、非引用模式 repeated acquire。每一步记录 internal、external、held、Handler callback 数和 Binder 调用；预期证明引用计数只存在客户端。

再追服务端资源与政策：

```bash
rg -n "acquireWakeLockInternal|findWakeLockIndexLocked|linkToDeath|handleWakeLockDeath" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java

rg -n "setWakeLockDisabledStateLocked|uidGoneInternal|updateWakeLockSummaryLocked" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java
```

预期证明：token 用 `==` 查找；一 token 一记录；UID gone 不删记录；disabled 按 owner 而非 WorkSource；只有有效 summary 最终驱动 blocker。

最后追到 native：

```bash
rg -n "updateSuspendBlockerLocked|nativeAcquireSuspendBlocker" \
  frameworks/base/services/core/java/com/android/server/power/PowerManagerService.java \
  frameworks/base/services/core/jni/com_android_server_power_PowerManagerService.cpp
```

本章最终应记住：客户端 count 决定 Binder 边沿，Binder token 决定服务端记录，owner 状态决定 PARTIAL 是否有效，summary 决定共享 blocker，release/death 才真正撤销服务端资源。

下一章进入 PowerManagerService 的 wakefulness 状态机：WakeLock 作为一个输入，怎样与 user activity、timeout、Dream、Doze 和 Display ready 共同完成 AWAKE、DREAMING、DOZING、ASLEEP 之间的转换。
