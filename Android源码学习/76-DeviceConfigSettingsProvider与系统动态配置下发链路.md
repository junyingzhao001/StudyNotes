# 76 DeviceConfig、SettingsProvider 与系统动态配置下发链路

> 源码基线：Android 11（`android-11.0.0_r48`）  
> 本章目标：理解 Android 的系统设置和动态 feature flag 如何读取、缓存、持久化、通知，并影响正在运行的系统服务。  
> 阅读方式：Mac 上只读源码，不需要连接设备或修改真实配置。

---

## 1. 为什么这一章重要

前面很多系统源码都会出现类似代码：

```java
boolean enabled = DeviceConfig.getBoolean(
        DeviceConfig.NAMESPACE_ACTIVITY_MANAGER,
        "some_feature_enabled",
        true);
```

或者：

```java
int value = Settings.Global.getInt(
        resolver, "some_setting", defaultValue);
```

它们表面上只是“读一个字符串/整数”，背后却包含：

- 逻辑表与 namespace；
- 多用户归属；
- Binder/ContentProvider 调用；
- 进程内缓存与 generation 失效；
- XML/AtomicFile 持久化；
- 读写权限和 AppOps；
- 默认值、当前值、恢复与 reset；
- ContentObserver 与指定 Executor 回调；
- Java 配置向 native system property 的桥接。

```text
远端配置服务 / shell / 系统组件
                 │ 写
                 ▼
            SettingsProvider
        ┌────────┴─────────┐
        │                  │
 Settings三类逻辑表      Config表
 System/Secure/Global    namespace/key
        │                  │
        └──── notify + generation ────┐
                                      ▼
                  Settings / DeviceConfig 进程内缓存与监听
                                      │
                                      ▼
                       ActivityManager、Display、Job 等服务
```

本章先建立一个原则：

> 动态配置只是“输入值发生变化”；业务服务是否立即应用、在哪个线程应用、是否要重启，取决于消费端代码。

---

## 2. 先分清五个容易撞名的对象

| 名称 | 它是什么 |
|---|---|
| `android.provider.Settings` | Framework API 门面，含 System/Secure/Global/Config |
| `SettingsProvider` | `com.android.providers.settings` ContentProvider，真正管理数据 |
| `SettingsState` | Provider 内一张逻辑设置表的内存状态与 XML 持久化实现 |
| `android.provider.DeviceConfig` | 按 namespace 管理动态配置的 System API 门面 |
| Settings App | 用户看到的“设置”应用 UI，不是设置数据库本身 |

`DeviceConfig` 没有一个独立的 `DeviceConfigProvider`。Android 11 中它最终仍通过 `Settings.Config` 访问同一个 SettingsProvider 的 config 类型。

---

## 3. 源码地图

| 文件 | 作用 | 重点 |
|---|---|---|
| `frameworks/base/core/java/android/provider/Settings.java` | Settings API | `NameValueCache`、System/Secure/Global/Config |
| `frameworks/base/core/java/android/provider/DeviceConfig.java` | DeviceConfig API | typed getter、Properties、listener |
| `frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java` | Provider 服务端 | `call()`、权限、用户路由、notify、generation |
| `frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/SettingsState.java` | 单表状态与文件 | current/default、异步写、AtomicFile |
| `frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/GenerationRegistry.java` | 缓存版本协调 | generation memory、increment |
| `frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/DeviceConfigService.java` | shell service | list/get/put/delete/reset 命令入口 |
| `frameworks/base/services/core/java/com/android/server/am/SettingsToPropertiesMapper.java` | native bridge | DeviceConfig → `persist.device_config.*` |
| `frameworks/base/services/core/java/com/android/server/am/ActivityManagerConstants.java` | 典型消费端 | listener、默认值、更新成员变量 |

---

## 4. Settings 的三张经典逻辑表

### 4.1 `Settings.System`

偏向用户可见、每用户的传统设置，例如部分显示、声音等偏好。

```text
user 0 System ≠ user 10 System
```

普通应用写入通常需要 `WRITE_SETTINGS`，且这是带特殊授权流程的权限；不是在 manifest 写一行就必然可写。

### 4.2 `Settings.Secure`

每用户的安全/系统控制设置。普通应用一般不可写，系统组件通过 `WRITE_SECURE_SETTINGS` 等受限权限操作。

```text
user 0 Secure ≠ work profile 10 Secure
```

### 4.3 `Settings.Global`

设备全局共享，不随当前前台用户切换而各自保存。Provider 统一把它归到 system user：

```text
Global → UserHandle.USER_SYSTEM
```

这不表示“任何用户都能写”，只是数据归属全局。

### 4.4 不是所有常量都严格留在名字对应的表

Android 历史升级中，一些 key 从 System 移到 Secure 或 Global。兼容 API 可能把旧入口转发到新表。阅读一个常量时应检查当前版本的 moved-to 集合和具体 getter，而不是只凭类名断言存储位置。

---

## 5. DeviceConfig 的 Config 表

`SettingsState` 类型定义还包括：

```java
SETTINGS_TYPE_CONFIG = 4;
```

DeviceConfig 数据逻辑上是全设备共享，Provider 使用 system user 保存。key 在 Config 表中采用复合名称：

```text
namespace + "/" + propertyName
```

例如：

```text
activity_manager/max_cached_processes
display_manager/refresh_rate_in_low_zone
```

`DeviceConfig.getProperties(namespace, names...)` 在 API 层加前缀读取，返回时再去掉前缀，让调用者只看到 namespace 内的短 key。

### 5.1 为什么需要 namespace

- 避免不同模块的 key 冲突；
- 批量原子更新同一模块配置；
- 按模块注册监听；
- 按 namespace reset；
- 做访问控制、回滚与崩溃恢复；
- 将 native namespace 映射到系统属性。

namespace 是协议的一部分，不能随便改名。改名相当于所有旧值和监听者都失去联系。

---

## 6. 一次 Settings 读取的完整链路

调用：

```java
String value = Settings.Global.getString(resolver, name);
```

概念链路：

```text
调用进程
  → Settings.Global / NameValueCache
  → 检查本进程缓存 generation
  → 缓存未命中时通过 IContentProvider.call(GET_global)
  → SettingsProvider.call()
  → SettingsRegistry / SettingsState 内存 map
  → Bundle 返回 value + generation tracking 数据
  → 调用进程更新本地缓存
```

### 6.1 为什么优先使用 `call()` 而不是 query Cursor

单 key 读写非常频繁。`ContentProvider.call()` 可用紧凑 Bundle 返回值，避免构造 Cursor、列和窗口。旧式 query/insert 接口仍可能存在，但 Framework 自己的 NameValueCache 优先走专用 call method。

### 6.2 Provider holder

每个表有 `ContentProviderHolder`，惰性获取 `settings` authority 的 provider Binder，并在调用间复用。Provider 死亡或远端异常时会重新处理，而不是每次都从头解析 authority。

---

## 7. `NameValueCache` 与 GenerationRegistry

### 7.1 为什么需要缓存

ActivityManager、WindowManager 等热点代码可能频繁读配置。如果每次都跨进程 Binder，会增加时延和 system_server/provider 压力。

每个调用进程中的 `NameValueCache` 大致保存：

```text
name → cached string value
generation tracker → 服务端共享 generation 当前值
```

### 7.2 generation 怎样失效缓存

Provider 返回值时可附带一个共享内存 generation 索引。写设置后，服务端：

```java
mGenerationRegistry.incrementGeneration(key);
```

客户端下一次读时比较 tracker 中的 generation：

```text
本地记录 generation = 8
共享区当前 generation = 9
        → 本地 map 过期，清理/重新读取
```

这避免每次读都先发一个 Binder 请求问“变了吗”。generation 值像表版本号，不是设置值本身。

### 7.3 缓存一致性不是业务原子事务

generation 只保证客户端知道缓存已过期。它不把多个分别写入的 key 自动变成业务事务；需要多 key 原子更新时应使用 DeviceConfig `setProperties()`/Config 批量接口。

### 7.4 为什么还需要 ContentObserver

- generation：让下一次主动读取拿到新值；
- ContentObserver：主动通知消费者“现在有变化”，让它立即重算行为。

一个解决 pull cache freshness，一个解决 push notification，职责不同。

---

## 8. 一次写入发生什么

以 DeviceConfig 单 key 写为例：

```java
DeviceConfig.setProperty(namespace, name, value, makeDefault);
```

链路：

```text
DeviceConfig
  → Settings.Config.putString()
  → NameValueCache.putStringForUser()
  → IContentProvider.call(PUT_config)
  → SettingsProvider 校验 WRITE_DEVICE_CONFIG
  → 复合 key namespace/name
  → SettingsRegistry.insertSettingLocked()
  → SettingsState 更新内存 Setting
  → 安排异步 XML 写盘
  → increment generation
  → ContentResolver.notifyChange(config/namespace/name)
```

### 8.1 value=null

通常表达删除当前 property，而不是字符串 `"null"`。typed getter 读不到值时返回调用者提供的 default。

### 8.2 `setProperties()` 不是 patch

源码文档明确：它把 `Properties` 视为某 namespace 的完整集合：

- 提供的 key 被新增/更新；
- namespace 中未提供的旧 key 会被删除；
- 整组更新以原子方式应用。

把它误当成“只更新这几个 key”，可能意外清掉同 namespace 其他配置。只改一个 key 应使用 `setProperty()`。

---

## 9. 当前值、默认值与 reset

`SettingsState.Setting` 不只保存一个 value，还包括：

```text
name
value                 当前生效值
defaultValue          可 reset 回去的默认值
packageName           谁写入
tag                   配置批次/来源标签
defaultFromSystem     默认是否来自系统
isValuePreservedInRestore 等恢复状态
```

### 9.1 `makeDefault`

`DeviceConfig.setProperty(..., makeDefault=true)` 尝试把本次值设为默认。但源码约束：若系统已经建立默认值，后来调用者不能随便覆盖系统默认。

### 9.2 reset 不是必然删除

根据 reset mode 和来源，可能：

- 恢复到可信默认值；
- 清除非默认 override；
- 只处理某 namespace/tag/package；
- 保留系统设置或 restore 要保护的值。

因此 `resetToDefaults()` 和 `setProperty(..., null)` 语义不同：前者按默认/来源规则回滚，后者删除具体当前属性。

### 9.3 BadConfigException

`DeviceConfig.setProperties()` 如果配置被拒绝，会抛 `BadConfigException`。源码说明 RescueParty 识别坏配置并 reset namespace 后，可能拒绝重新应用同一问题配置，防止设备反复 crash loop。

---

## 10. SettingsState 如何持久化

Android 11 的现代 SettingsProvider 不是简单依赖一张 SQLite 表；`SettingsState` 在内存保存 map，并把状态写入 XML 文件。

### 10.1 写入不是每次同步落盘

`insertSettingLocked()` 更新内存后调用：

```java
scheduleWriteIfNeededLocked();
```

短时间多个修改可以合并，减少频繁 I/O。必要路径可调用 `persistSyncLocked()` 强制同步。

### 10.2 AtomicFile

写盘使用 `AtomicFile`：

```text
旧有效文件
  → 写临时/备份方案
  → 成功后替换
  → 失败时恢复旧版本
```

它保护的是“文件不会因半次写入完全损坏”，不表示跨多个 SettingsState 文件具有数据库级事务。

### 10.3 每用户和全局文件

System/Secure 按用户拥有各自 SettingsState；Global/Config 统一归 system user。用户创建、解锁、停止、删除时，SettingsRegistry 会创建、迁移、销毁相应状态并通知 generation。

---

## 11. DeviceConfig typed getter：类型是调用端解释出来的

底层存储仍是 String：

```java
DeviceConfig.getBoolean(namespace, key, defaultValue)
DeviceConfig.getInt(namespace, key, defaultValue)
DeviceConfig.getLong(namespace, key, defaultValue)
DeviceConfig.getFloat(namespace, key, defaultValue)
DeviceConfig.getString(namespace, key, defaultValue)
```

### 11.1 缺失与格式错误

- key 缺失：返回 default；
- int/long/float 解析失败：记录错误并返回 default；
- boolean 使用字符串解析规则；
- 空字符串和 null 是不同输入，消费端要明确处理。

### 11.2 默认值属于谁

要区分两种 default：

- getter 的 `defaultValue`：调用代码编译时提供的 fallback，不一定存盘；
- SettingsState 的 `defaultValue`：随设置记录保存，可被 reset 使用。

```java
DeviceConfig.getInt(ns, key, 42)
```

这里的 42 不会自动写进 SettingsProvider。若 key 不存在，每次只是本地返回 42。

---

## 12. DeviceConfig listener 的完整链路

注册：

```java
DeviceConfig.addOnPropertiesChangedListener(
        namespace,
        executor,
        listener);
```

### 12.1 每 namespace 只注册一份底层 observer

DeviceConfig 静态结构保存：

```text
sListeners: listener → (namespace, executor)
sNamespaces: namespace → (ContentObserver, 引用计数)
```

同一进程有 5 个 listener 监听同一 namespace，只向 ContentResolver 注册一个 ContentObserver，引用计数为 5。最后一个 listener 移除时才注销底层 observer。

### 12.2 URI 携带变化范围

Provider 通知 URI 类似：

```text
content://settings/config/<namespace>/<changedKey1>/<changedKey2>
```

`handleChange(uri)` 取 namespace 和变化 key，再读取该 namespace 当前值，构造 `DeviceConfig.Properties`。

回调拿到的 Properties 只包含此次新增、更新或删除的 key，不是整个 namespace 完整快照。删除的 key 也可能出现在 keyset 中，其 value 为 null。

### 12.3 回调在哪个线程

ContentObserver 收到变化后，不直接替你决定业务线程。DeviceConfig 使用注册时传入的 Executor：

```java
executor.execute(() -> listener.onPropertiesChanged(properties));
```

所以：

- 传 main executor，回调在主线程；
- 传 BackgroundThread/线程池，回调在对应线程；
- 传直接执行 executor，可能在通知调用路径执行；
- listener 内修改共享服务字段时要遵守该服务自己的锁/handler 规则。

“DeviceConfig listener 一定在 Binder 线程”或“一定在主线程”都不正确。

### 12.4 同一个 listener 再注册

- 同 namespace：替换 executor；
- 换 namespace：减少旧 namespace 引用，增加新 namespace 引用；
- 不会让一个 listener 实例同时监听两个 namespace。

---

## 13. 一个 system_server 消费案例：ActivityManagerConstants

`ActivityManagerConstants` 监听 activity_manager namespace：

```java
DeviceConfig.addOnPropertiesChangedListener(
        DeviceConfig.NAMESPACE_ACTIVITY_MANAGER,
        executor,
        properties -> { ... });
```

变化后更新：

- background settle time；
- service restart duration/factor；
- max service inactivity；
- background start timeout；
- async process start flag；
- PSS/GC/CPU 检查间隔；
- foreground service 相关开关等。

### 13.1 动态应用不是自动完成的

消费端需要自己：

1. 注册 listener；
2. 首次启动主动读取现有值；
3. 在 properties keyset 中只处理相关 key；
4. typed parse 并设置合理 fallback；
5. 在正确锁/线程更新字段；
6. 必要时触发重新调度现有对象。

只改变成员变量不会自动回溯修改已经排队的消息。例如“新 timeout”是否影响旧任务，要看消费端有没有主动 reschedule。

### 13.2 dynamic、native 与 native_boot

namespace 命名常暗示应用时机：

- 普通 namespace：Java 服务可监听后立即应用；
- `_native`：需要映射为 system property 供 native 读取；
- `_native_boot`：通常写成持久属性，相关 native 组件在下次启动读取。

这只是约定与配套实现，不是字符串后缀自身具有魔法。

---

## 14. `SettingsToPropertiesMapper`：给 native 世界搭桥

native 进程不方便直接使用 Java DeviceConfig/ContentResolver。system_server 中的 mapper 监听一组 namespace，并构造：

```text
persist.device_config.<namespace>.<key>
```

变化后调用 `SystemProperties.set()`。

### 14.1 属性名限制

mapper 检查合法字符和禁止子串。无法构造安全 system property 名时会拒绝映射，而不是把任意 namespace/key 写入属性服务。

### 14.2 删除为何写空字符串

Android system property 通常不能真正删除。DeviceConfig value 变 null 时：

- 若旧 property 本来为空，不做事；
- 否则写 `""` 表达清空。

native 消费者必须把空值按“未配置/恢复默认”解释。

### 14.3 crash-loop reset

mapper 还关注 native flags reset 状态。若 RescueParty 在当前 boot 为恢复崩溃循环重置过 native flag，应避免启动时马上把坏值再次推回 property。

这形成保护链：

```text
坏动态配置 → native/system_server 反复崩溃
  → RescueParty 标记并 reset
  → 本次 boot 不重新灌回坏 native flag
  → setProperties 可能被 BadConfig 拒绝
```

---

## 15. 权限边界

### 15.1 DeviceConfig 读取

默认需要 `READ_DEVICE_CONFIG`。Android 11 有一组 public namespace，可不持该权限读取；`DeviceConfig.enforceReadPermission()` 会检查 namespace 是否在白名单。

“某些 namespace 公开”不表示所有 key 都适合普通应用依赖，也不表示能写。

### 15.2 DeviceConfig 写入

需要 `WRITE_DEVICE_CONFIG`，通常只授予特定系统配置服务、shell/root 或受信任组件。普通应用不能借此修改 ActivityManager 或网络系统参数。

### 15.3 Settings System/Secure/Global

- System 写入受 `WRITE_SETTINGS`、AppOps 和可写 key 限制；
- Secure/Global 写入通常需要 `WRITE_SECURE_SETTINGS` 等 signature 权限；
- 某些 key 有单独读限制或只对 target SDK/系统包公开；
- 跨用户读取/写入需要相应 cross-user 权限；
- Provider 用 Binder calling UID 和 package 做真实校验。

不要把 `adb shell settings put ...` 能执行，误解为普通 App API 也有同等能力。shell 是特殊 UID。

---

## 16. 多用户边界

```text
System  → per user
Secure  → per user
Global  → system user / device global
Config  → system user / device global
```

但实际读取还可能有：

- clone/profile owning user 映射；
- moved key 转发表；
- managed profile 限制；
- secure SSAID 之类特殊按调用者生成值；
- instant app 可访问 key 白名单。

因此“Secure 是 per-user”是第一层模型，不是所有 key 的完整访问语义。

### 16.1 ContentResolver userId

Settings API 会使用 resolver 的 userId；Provider 再通过 `resolveCallingUserIdEnforcingPermissionsLocked()` 校验。客户端传一个 userId 并不能绕过跨用户权限。

---

## 17. `Properties`：原子快照与变化集合

`DeviceConfig.Properties` 包含：

```text
namespace
Map<String, String>
```

并提供 typed getter。它有两种上下文：

### 17.1 主动 `getProperties()`

返回请求 key 或 namespace 全部 key 的同一读取快照。批量读取适合参数间有一致性要求：

```text
min <= max
```

若分两次单 key 读取，恰逢配置更新，可能读到旧 min + 新 max 的混合组合。

### 17.2 listener callback

Properties 是此次变化 key 的集合，不保证包含 namespace 未变化的所有 key。消费端若要完整重建配置：

- 可以只 patch 变化字段；或
- 回调后主动 `getProperties(namespace)` 读取全量快照。

把 callback Properties 当全量 map，会把未出现的 key 错误恢复默认。

---

## 18. 初始读取与监听之间的竞态

常见写法：

```text
① 先 get 全量
② 再 add listener
```

若配置在 ① 和 ② 之间变化，可能漏掉一次通知。反过来：

```text
① 先 add listener
② 再 get 全量
```

可能出现 listener 更新新值后，② 的旧/交错结果又覆盖它（具体取决于调用时序）。

Framework 消费端通常通过同一 handler/executor 串行更新、注册后立即触发一次统一 refresh，或把 Properties 视为幂等快照来降低竞态。

不存在由 DeviceConfig 自动替所有业务对象完成的“订阅并原子获取初值”操作，消费端设计仍然重要。

---

## 19. DeviceConfig 与资源 overlay、Settings、system property 的区别

| 机制 | 修改时机 | 典型用途 | 多用户 |
|---|---|---|---|
| resource overlay | 构建/安装/重启或 overlay 切换 | 设备静态默认、UI 资源 | 常非用户 key-value |
| Settings.System/Secure | 用户/系统运行时 | 用户偏好、安全状态 | 多为 per-user |
| Settings.Global | 系统运行时 | 全设备传统设置 | global |
| DeviceConfig | 配置服务/系统运行时 | feature flag、可调策略参数 | global + namespace |
| SystemProperties | boot/native/底层开关 | native 可读、早期启动、只读构建属性 | global |

### 19.1 同一功能可能组合多层

消费端常用优先级：

```text
强制 debug override
  > DeviceConfig 远程/动态值
  > Settings 用户选择
  > resource overlay 默认
  > 代码默认
```

具体优先级由业务代码定义，没有全 Android 统一规则。阅读时要画出每个来源，而不是只找到一个 getter 就停止。

---

## 20. 完整案例：动态改变一个 ActivityManager 超时

```text
配置服务拥有 WRITE_DEVICE_CONFIG
  → setProperty("activity_manager", "bg_start_timeout", "12000", false)
  → Settings.Config 组合 key activity_manager/bg_start_timeout
  → SettingsProvider PUT_config 校验并写 SettingsState 内存
  → generation++，安排 AtomicFile XML 写盘
  → notify content://settings/config/activity_manager/bg_start_timeout
  → system_server 进程内 DeviceConfig ContentObserver 收到
  → handleChange 读取新值，构造只含 bg_start_timeout 的 Properties
  → 注册的 executor 执行 ActivityManagerConstants listener
  → parse long，更新成员变量
  → 后续相关 ActivityManager 逻辑读取新 timeout
```

### 20.1 哪些事情没有自动发生

- 不会自动校验 12000 对业务是否合理，除非消费端做范围检查；
- 不会自动让已安排的所有旧 timeout 重新排队；
- 不会自动同步到 native，除非 namespace 在 mapper 范围；
- 不会自动要求重启，除非消费端只在 boot 读取；
- 不会自动回滚，除非 Watchdog/RescueParty 识别到健康问题。

---

## 21. 只读源码学习路线

### 第一轮：Settings 读链

```bash
rg -n "class NameValueCache|getStringForUser|GenerationTracker" \
  frameworks/base/core/java/android/provider/Settings.java
```

回答：缓存在哪个进程？generation 在哪里递增？

### 第二轮：Provider 写链

```bash
sed -n '350,460p' \
  frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java
rg -n "insertSettingLocked|scheduleWriteIfNeededLocked|AtomicFile" \
  frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/SettingsState.java
```

区分内存生效、通知与持久化三个时点。

### 第三轮：DeviceConfig listener

```bash
sed -n '650,825p' \
  frameworks/base/core/java/android/provider/DeviceConfig.java
```

画出 listener、namespace observer 和引用计数关系。

### 第四轮：消费端

```bash
rg -n "DeviceConfig.addOnPropertiesChangedListener|onPropertiesChanged" \
  frameworks/base/services/core/java/com/android/server/am/ActivityManagerConstants.java
```

检查首次加载、变化更新、线程和锁。

### 第五轮：native bridge

```bash
sed -n '100,260p' \
  frameworks/base/services/core/java/com/android/server/am/SettingsToPropertiesMapper.java
```

解释 value=null 为什么映射为空 property，以及 `_native_boot` 为什么通常下次启动才应用。

---

## 22. 常见误区纠正

### 误区 1：DeviceConfig 有自己独立 Provider

不对。Android 11 中它通过 `Settings.Config` 进入 SettingsProvider 的 config 类型。

### 误区 2：System/Secure/Global 都是全设备一份

不对。System/Secure 主要按用户，Global 才归 system user 全局共享。

### 误区 3：typed getter 在数据库保存 int/bool

不对。底层是字符串，类型转换在调用端完成。

### 误区 4：getter default 会写入配置

不对。它只是读取缺失/解析失败时的本地 fallback。

### 误区 5：listener 回调包含 namespace 全量值

不对。通常只含本次变化 key；需要全量应主动 get。

### 误区 6：listener 固定运行在主线程

不对。运行在注册者提供的 Executor。

### 误区 7：`setProperties()` 只 patch 给出的 key

不对。它用给定集合替换 namespace 完整内容，遗漏 key 会删除。

### 误区 8：设置成功返回后 XML 一定已经同步落盘

不对。内存可先更新，写盘通常异步合并；特殊路径才同步 persist。

### 误区 9：配置变化后所有旧对象立即采用新值

不对。消费端必须监听、更新字段，并决定是否重调度已有状态。

### 误区 10：shell 能写，所以第三方 App 也能写

不对。shell/root/系统配置服务拥有特殊身份和权限。

---

## 23. 排查“改了配置但没生效”的八层法

```text
① 名称：namespace/key 是否拼写、大小写完全一致？
② 存储：写的是 Config、Global、Secure 还是 System？
③ 用户：per-user 表是否写到了正确 userId？
④ 权限：写调用是否真实成功，还是被 Provider 拒绝？
⑤ 读取：消费端读取的是否同一来源，是否命中其他优先级 override？
⑥ 通知：URI 是否发出，listener 是否注册，是否只处理了其他 key？
⑦ 线程：Executor 是否阻塞，锁内更新是否成功？
⑧ 应用：该 flag 是动态、native 还是 native_boot，是否需要重调度/重启？
```

还应检查：

- 值是否解析失败而回落默认；
- 范围校验是否拒绝异常值；
- RescueParty 是否重置/拒绝 bad config；
- system property 名是否合法；
- 回调 Properties 是否被误当全量，导致其他字段被重置。

---

## 24. 练习题

### 题 1

为什么 Settings 缓存既有 generation，又仍需 ContentObserver？

### 题 2

`DeviceConfig.getInt(ns, "x", 10)` 返回 10，能否说明 Provider 中存有字符串 `"10"`？

### 题 3

namespace 原来有 `{a=1,b=2}`，调用 `setProperties({a=3})` 后是什么？

### 题 4

listener callback 的 Properties 只含 key `a`，能否认为 `b` 已删除？

### 题 5

为什么 `_native_boot` flag 改完后 native 组件可能要重启才使用？

### 题 6

Global 与 Config 都全局共享，为什么还要分开？

### 参考答案

1. generation 让下一次读取发现缓存过期；ContentObserver 主动驱动业务立即响应变化。
2. 不能。key 可能不存在或解析失败，10 只是调用端 fallback。
3. `{a=3}`；批量 set 是全量替换，未提供的 b 被删除。
4. 不能。回调是变化集合，未出现代表本次没变化，不代表不存在。
5. mapper 把值写为持久 system property，但该组件的实现可能只在启动初始化时读取。
6. Global 是传统系统设置模型；Config 提供 namespace、批量原子更新、动态监听、默认/reset、配置服务和崩溃恢复等 feature flag 语义。

---

## 25. 复读后的易混淆点补强

复读后最容易卡住的是三个问题。

### 25.1 值究竟存在哪里

```text
Settings/DeviceConfig Java 类：API 门面 + 进程内缓存
SettingsProvider：权威内存状态和访问控制
SettingsState XML：跨重启持久化
System property：部分 native flag 的镜像，不是 Config 表本体
```

不要把任何一层单独称为“全部配置数据库”。

### 25.2 谁能改

```text
用户设置 UI
  只能通过其系统权限修改允许的 Settings key

远端/系统配置服务
  持 WRITE_DEVICE_CONFIG 修改 Config namespace

普通 App
  通常只能读公开/允许的部分，不能改系统策略 flag

shell/root
  调试能力更强，但不代表 App 权限模型
```

### 25.3 变化在哪里执行

```text
Provider 写入/notify：SettingsProvider 所在线程与锁
ContentObserver：ContentResolver 通知路径
业务 listener：注册时指定的 Executor
真实策略变更：消费服务自己的锁、Handler、状态机
```

这四处可能是不同线程。`onPropertiesChanged()` 中直接触碰线程受限对象，是实际代码审查中必须警惕的问题。

---

## 26. 本章总结

完整链路：

```text
Settings.System/Secure/Global 或 DeviceConfig API
  → NameValueCache / Settings.Config 复合 namespace/key
  → IContentProvider.call 进入 SettingsProvider
  → 按类型和 user 进行权限/归属判断
  → SettingsState 更新 current/default/source
  → 异步 AtomicFile XML 持久化
  → GenerationRegistry++ 让各进程缓存失效
  → ContentResolver notify namespace/key URI
  → DeviceConfig ContentObserver 汇总变化
  → 指定 Executor 执行 Properties listener
  → system_server 消费者解析、校验并更新策略
  → 部分 native namespace 经 mapper 变为 persist.device_config.*
```

本章应掌握六条边界：

1. Settings API、SettingsProvider 与 Settings App；
2. System/Secure 的 per-user 与 Global/Config 的 device-global；
3. getter fallback default 与持久化 default；
4. generation 缓存失效与 ContentObserver 主动通知；
5. callback 变化集合与 namespace 全量快照；
6. 配置存储更新与业务真正应用。

下一章将学习 `SystemProperties、property_service、SELinux property_contexts 与启动属性链路`，继续追踪 Java/native 配置如何进入底层共享属性区、触发 init property action，并理解 `ro.*`、`persist.*` 等属性的生命周期。
