# 143 Android JSS：常量热更新与运行期生效边界

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`
>
> 学习方式：macOS 静态阅读；不在 Mac 上编译或连接设备
>
> 前置阅读：第 131、134、136、141 章

## 先看问题：把并发数从 10 改成 2，为什么正在运行的 6 个 Job 不会立刻停

Android 11 把 JSS 的批处理、负载降权、并发矩阵、退避下限、网络比例与 schedule API quota 放在 `Settings.Global.job_scheduler_constants` 这一条逗号分隔字符串中。ContentObserver 能在 system_server 运行时重新解析它，但“字段已经变更”不等于“已有 Job 立即重算”。

有的消费者在下一轮扫描现场读取新值；有的值只在创建下一代 JobStatus 时使用；并发限制等到下一次分槽才收敛；只有 API quota 更新会主动改独立 CountQuotaTracker。坏配置还可能让 Constants 已更新、quota 账本已清空，而 Tracker 新窗口未提交。

本章回答：**一条配置变化从解析到消费者实际生效要经过哪些完成点，缺失、拼错、越界与异常分别会保留默认、钳位还是留下部分提交？**

## 1. 先分清三套配置入口

| 配置 | Settings.Global 键 | 主要消费者 |
|---|---|---|
| JSS 总控 | `job_scheduler_constants` | JSS、JCM、Connectivity 与 API schedule quota |
| quota controller | `job_scheduler_quota_controller_constants` | QuotaController 执行配额 |
| time controller | `job_scheduler_time_controller_constants` | TimeController 延迟/截止时间报警策略 |

三者有不同 URI、parser 和 observer。本章只讲第一条；修改总控串不会顺带修改 QuotaController 或 TimeController 的专用常量。

JSS 到 `PHASE_SYSTEM_SERVICES_READY` 才启动 `ConstantsObserver`。`start()` 注册观察者后立即调用一次 `updateConstants()`，所以初值来自当时 Settings，而不必等待第一次变化通知。

## 2. 更新在哪个线程、哪个锁域发生

ConstantsObserver 绑定 JSS `mHandler`，也就是 system_server 主 Looper。变化回调在主线程进入，并在 `mLock` 中依次执行：

```java
                try {
                    mConstants.updateConstantsLocked(Settings.Global.getString(mResolver,
                            Settings.Global.JOB_SCHEDULER_CONSTANTS));
                    for (int controller = 0; controller < mControllers.size(); controller++) {
                        final StateController sc = mControllers.get(controller);
                        sc.onConstantsUpdatedLocked();
                    }
                    updateQuotaTracker();
                } catch (IllegalArgumentException e) {
                    // Failed to parse the settings string, log this and move on
                    // with defaults.
                    Slog.e(TAG, "Bad jobscheduler settings", e);
                }
```

源码路径：`frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java`

在 r48 当前 controllers 树中，没有类 override `StateController.onConstantsUpdatedLocked()`；这轮循环只是空扩展点，不会重扫 Job 或主动重算 Connectivity 条件。更新方法也不发送 `MSG_CHECK_JOB`。

因此锁只能保证 JSS 共享字段不会被同锁读者看到半段更新，不代表跨 Constants、Controller、CountQuotaTracker 的事务，更不保证产生一次调度事件。

## 3. 新字符串是完整快照，不是增量 patch

`KeyValueListParser.setString()` 每次先执行 `mValues.clear()`，再逐项 `put()`：

```java
    public void setString(String str) throws IllegalArgumentException {
        mValues.clear();
        if (str != null) {
            mSplitter.setString(str);
            for (String pair : mSplitter) {
                int sep = pair.indexOf('=');
                if (sep < 0) {
                    mValues.clear();
                    throw new IllegalArgumentException(
                            "'" + pair + "' in '" + str + "' is not a valid key-value pair");
                }
                mValues.put(pair.substring(0, sep).trim(), pair.substring(sep + 1).trim());
            }
        }
    }
```

源码路径：`frameworks/base/core/java/android/util/KeyValueListParser.java`

由此得到四个基础规则：

- null 或空串使 map 为空，所有 getter 返回代码默认值；
- 新串未写的旧键不会保留，而是回默认；
- 未知键可进入 map，但没有消费者就静默无效；
- 重复键以后一次 `put()` 覆盖前一次。

所以运维命令若只写一个键，实际会把同一总控串的其他键全部恢复默认。拼错键通常也不报错，只让真正字段缺失并回默认。

## 4. 整串语法错与单字段类型错的失败范围不同

裸字段没有 `=` 时，`setString()` 清空 map 并抛异常。`Constants.updateConstantsLocked()` 在内部捕获后仍继续逐字段 getter，于是整套 JSS 总控值按空 map 重写为默认。

数字或 Duration 类型错误则由单个 getter 捕获，只让该字段回默认，其他合法字段继续解析。例如：

```text
heavy_use_factor=abc,min_ready_non_active_jobs_count=2
```

结果是 heavy 阈值回默认 0.9，而 batch 数得到 2。

Boolean 更反直觉：`Boolean.parseBoolean()` 只有忽略大小写等于 `true` 才返回 true，其他拼写都返回 false，不抛异常。因此 `enable_api_quotas=tru` 会真的关闭 API quota，而不是回默认 true。

`getDurationMillis()` 接受毫秒整数或 Java Duration 字符串，例如 `30000` 与 `PT30S`；非法值只回退该字段。float 则没有 0—1 业务钳位，负数、2 或 `NaN` 都可能进入网络比例和负载阈值，后续比较会出现非常规结果。

## 5. 不同常量何时真正生效

| 参数组 | 字段更新后是否主动重建 | 真正读取新值的时点 |
|---|---|---|
| batch 数量/最长等待 | 否，也不设新 Alarm | 下一次普通 maybe 扫描 |
| heavy/moderate load 阈值 | 否 | 下一次 `evaluateJobPriorityLocked()` |
| 并发矩阵 | 不主动停止 active | 下一次 JCM 分槽 |
| screen-off ramp delay | 不重投在途 Runnable | 下一次屏幕转场或旧 Runnable 执行时复查 |
| linear/exp backoff 下限 | 不改已有运行窗口 | 下一次失败创建新 JobStatus |
| connectivity 比例 | Controller 空回调 | 下一次网络约束评估 |
| API schedule quota | 主动复制到 Tracker | 后续 quota 判断；Tracker 同时失效统计并安排检查 |

第 141 章的 31 分钟 batch 阈值没有专用 Alarm。把它改成 1 分钟，只会让下一次扫描把已等待够 1 分钟的 Job 归为 unbatched；若没有新消息，时间流逝本身不会唤醒 JSS。

已经写入 `JobStatus.lastEvaluatedPriority` 的值也不会由 observer 刷新；已有失败 Job 的 earliest runtime 不会因 backoff 下限改变而重算。热更新改变未来决策，不回写历史派生状态。

## 6. 并发矩阵会钳位，但不会立即抢占

r48 为 screen on/off × normal/moderate/low/critical 各维护一组 `total/maxBg/minBg`。解析后依次约束：

```text
total  ∈ [1, 16]
maxBg  ∈ [1, total]
minBg  ∈ [0, maxBg]
minBg  < total
```

16 来自固定的 `MAX_JOB_CONTEXTS_COUNT`，配置不能创建第 17 个 JobServiceContext。若输入 `total=1,maxBg=99,minBg=99`，最终是 `1,1,0`。

把 total 从 10 调成 2 时，observer 不遍历并停止现有 6 个 active Job。下一次 `assignJobsToContextsLocked()` 才用新矩阵限制继续分配，已有执行随完成/停止逐步收敛。

JCM 的内存 trim 级别还有 1 秒刷新缓存：下一次分槽会读取新矩阵，但可能仍用缓存的 trim 档位选择其中一格。配置字段更新与外部系统状态刷新是两个时钟。

## 7. screen-off delay 的在途 Runnable 不能无缝改期

屏幕熄灭时，JCM 按当时值 `postDelayed(mRampUpForScreenOff, delay)`。常量更新不会 remove 并重新 post。

旧 Runnable 执行时会用当前新 delay 再检查 `lastScreenOff + delay > now`。若新 delay 变长，旧回调可能提前到达后直接 return，且该分支不补发剩余时间的 Runnable；effective interactive 状态可能一直等到后续屏幕事件才改变。若新 delay 变短，旧回调仍按旧投递时间晚到。

这是典型的“消费时二次读新值，却没有重定时机制”：既不能简单归类成立即生效，也不能说完全沿用旧值。

## 8. API quota 是少数主动更新下游对象的参数

JSS 先把 count 做 `Math.max(250, parsedCount)`，所以可调高但不能低于 250。随后：

```java
    void updateQuotaTracker() {
        mQuotaTracker.setEnabled(mConstants.ENABLE_API_QUOTAS);
        mQuotaTracker.setCountLimit(QUOTA_TRACKER_CATEGORY_SCHEDULE_PERSISTED,
                mConstants.API_QUOTA_SCHEDULE_COUNT,
                mConstants.API_QUOTA_SCHEDULE_WINDOW_MS);
    }
```

`CountQuotaTracker.setCountLimit()` 再把非负 window 钳到自己的 `MIN_WINDOW_SIZE_MS..MAX_WINDOW_SIZE_MS`，失效 execution stats 并安排 quota check。因此 JSS dump 中的 window 可能是 parser 字段，而 Tracker 真正使用的是二次钳位值。

`QuotaTracker.setEnabled(false)` 不是暂停开关：状态从 enabled 变成 disabled 时调用 `clear()`，丢弃事件与跟踪数据；以后重新开启不会恢复旧 schedule 历史。

第 139 章的 throw/return 两个超额开关只影响后续 API 调用，已经成功注册的 Job 不会被追溯取消。

## 9. 负 quota window 怎样造成部分提交

`getDurationMillis()` 可解析负整数，Constants 没有先钳位。假设写入：

```text
enable_api_quotas=false,aq_schedule_window_ms=-1
```

执行顺序是：

```text
Constants 全部字段已写入
→ Controller 空回调完成
→ Tracker.setEnabled(false)，清空历史
→ Tracker.setCountLimit(..., -1) 抛 IllegalArgumentException
→ ConstantsObserver 外层 catch 只记录日志
```

外层 catch 不恢复旧 Constants、不重新加载默认，也不复原已清账本。最终可以是 Constants 显示新值，而 Tracker 仍保留旧 limit/window，enable 副作用却已发生。

这与 parser 的裸字段语法错不同：语法错在 Constants 内部清空 map 后整体读默认；负 window 是下游业务校验晚失败，前面步骤已经部分提交。日志同样写“Bad jobscheduler settings”，却不能据此推断最终状态一致。

## 10. dump 能证明什么，不能证明什么

`dumpsys jobscheduler` 的 Settings 段显示 Constants 解析、默认回退和 JSS 层钳位后的当前字段，不原样回显整条 Settings 字符串，也不标注每项来自显式值还是默认。

它不能单独证明：

- Tracker 的 window 是否又被二次钳位；
- quota 是否曾关闭并清过历史；
- 已有 Job 的 backoff/priority 派生快照是否重算；
- 在途 screen-off Runnable 按哪个旧投递时点到达；
- 新 batch 值是否已经经历一次扫描。

文本与 Proto 都是当前快照，不是配置提交事务日志。诊断必须同时看消费者状态、相关时间戳、Handler/Alarm 是否触发，以及必要的错误日志。

## 11. 公开面与版本边界

- `JOB_SCHEDULER_CONSTANTS`、内部键、JCM 矩阵与 API quota Tracker 都不是普通应用的稳定调参 API。
- r48 保留若干 deprecated key 名称，却不在 `updateConstantsLocked()` 读取；键名存在不等于仍有消费者。
- 默认 5/31 分钟、矩阵数值、最低 250 与 Tracker window 上下限都可能随版本变化。
- 设置修改属于有副作用的系统操作；本章练习只读源码与已有 dump，不要求在设备上写 Global Settings。
- 静态阅读能证明生效边界与可能的部分提交，不能给出具体设备的调度延迟或竞态概率。

## 12. 从源码验证一次热更新

在 Android 11 r48 源码根目录只读执行：

1. 读 observer、更新顺序与 quota 复制：

   ```bash
   sed -n '325,380p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

2. 核对字段默认、parser、钳位与 dump：

   ```bash
   sed -n '380,780p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
   ```

3. 验证完整快照、boolean 与 Duration 解析：

   ```bash
   sed -n '30,220p' frameworks/base/core/java/android/util/KeyValueListParser.java
   ```

4. 验证 Tracker disable 清账与 window 二次校验：

   ```bash
   sed -n '240,270p' \
     frameworks/base/services/core/java/com/android/server/utils/quota/QuotaTracker.java
   sed -n '220,260p' \
     frameworks/base/services/core/java/com/android/server/utils/quota/CountQuotaTracker.java
   ```

5. 追 screen-off Runnable 与 1 秒内存状态缓存：

   ```bash
   sed -n '140,250p' \
     frameworks/base/apex/jobscheduler/service/java/com/android/server/job/JobConcurrencyManager.java
   ```

## 13. 练习与参考答案

### 练习一：完整快照

旧串显式设置 batch count=2 与 heavy factor=0.7，新串只写 batch count=1。heavy factor 最终是多少？

参考答案：回到默认 0.9。parser 先清空，更新不是增量 patch。

### 练习二：错误范围

比较 `heavy_use_factor=abc,min_ready_non_active_jobs_count=2` 与 `heavy_use_factor=.8,oops`。

参考答案：前者仅 heavy 回默认、batch 得到 2；后者因裸字段清空整张 map，所有字段按默认重写。

### 练习三：并发收敛

当前运行 6 个 Job，把 total 从 10 改 2。observer 返回时应剩几个？

参考答案：仍可能是 6 个；更新不主动 stop，下一次分配只是不再按旧上限扩张，随后自然收敛。

### 练习四：在途 screen-off

熄屏时 delay=30 秒，10 秒后改成 60 秒。旧 Runnable 在 30 秒到达会怎样？

参考答案：它用新值复查，发现尚未满 60 秒并 return；该分支不自动补发剩余 30 秒。

### 练习五：错误 boolean 与负 window

`enable_api_quotas=tru` 和 `enable_api_quotas=false,aq_schedule_window_ms=-1` 的风险分别是什么？

参考答案：前者被解析成 false 并清 Tracker；后者同样可能先清 Tracker，再因负 window 抛错，留下 Constants/Tracker 部分提交。

## 本章带走什么

JSS 常量热更新是一条“完整字符串覆盖 parser → 共享字段更新 → 空 Controller 钩子 → 部分参数复制到独立 Tracker”的链。语法错会清 map 后整体回默认，单值类型错通常只回该字段，错误 boolean 却常变成 false；数值还可能在 JSS 或下游再次钳位。

判断生效不能停在 observer 已回调：batch、load、网络和 backoff 要等各自下一消费点；并发要等分槽且不主动停止已有任务；screen-off 在途 Runnable 不会无缝改期；quota disable 会立即清历史，而负 window 又可能在之后失败且无回滚。热更新改变政策字段，不等于一次原子、全量、即时的重新调度。
