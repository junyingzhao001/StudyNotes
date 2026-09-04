# 159 Android ColorDisplayService：Night Display 与颜色矩阵策略

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 158 章

---

## 1. 屏幕偏黄时，不能只盯着 Night Display 开关

用户打开夜间模式后，界面开关已经变成“开启”，屏幕颜色却可能仍在过渡；切换色彩模式后，暖色矩阵还会重新计算；Display White Balance、灰度、反色和全局饱和度又可能同时参与最终输出。

排查这类问题，至少要分清五层状态：

| 层次 | 典型问题 |
|---|---|
| 设置层 | Secure/System setting 写成了什么 |
| 策略层 | 手动、固定时段、Twilight 谁决定 active |
| 控制器层 | `Boolean` 激活态、色温缓存、目标矩阵是什么 |
| 合成层 | 多个 level 的矩阵按什么顺序相乘 |
| native 层 | 请求是否已送到 SurfaceFlinger |

本章的核心结论是：

> Android 11 r48 的 `ColorDisplayService` 不是一个单独的 Night Display 开关，而是“多用户设置观察 + 时间策略 + 色彩模式选择 + 多种 tint 仲裁 + 矩阵合成 + SurfaceFlinger Binder 请求”的中枢；API 返回成功通常只证明写设置或发起请求，不证明屏幕已经完成变色。

本章重点读 Java system_server 侧。SurfaceFlinger 如何解释 color mode、dataspace 与 render intent，留到第 160 章。

---

## 2. 一张职责图：谁定策略，谁合矩阵，谁显示

主源码位于：

```text
services/core/java/com/android/server/display/color/
├── ColorDisplayService.java
├── TintController.java
├── GlobalSaturationTintController.java
├── AppSaturationController.java
├── DisplayWhiteBalanceTintController.java
└── DisplayTransformManager.java

core/java/android/hardware/display/
└── ColorDisplayManager.java

services/core/java/com/android/server/twilight/
├── TwilightService.java
└── TwilightManager.java
```

职责可压缩成：

```text
ColorDisplayManager / IColorDisplayManager
        │
        ▼
ColorDisplayService
  ├─ 观察当前用户设置
  ├─ 运行 Night Display 自动策略
  ├─ 决定 Night / DWB / accessibility 状态
  ├─ 驱动 tint 动画
  └─ 管理全局、单应用饱和度
        │
        ├─ 全局 4×4 矩阵 ──► DisplayTransformManager
        │                         └─► SurfaceFlinger
        │
        └─ 单应用 3×3 矩阵 ──► Window 的 ColorTransformController
```

`ColorDisplayService` 构造时把 `TintHandler` 绑定到共享 `DisplayThread`，并把 Handler 标成 async：

```java
mHandler = new TintHandler(DisplayThread.get().getLooper());

private TintHandler(Looper looper) {
    super(looper, null, true /* async */);
}
```

设置 observer、用户切换消息和 tint 动画通常在这条线程上串行。不过 Binder 方法并非全部转发到 Handler；后文会看到，服务不能简单理解为“所有状态只归 DisplayThread”。

---

## 3. 服务发布很早，真正 setup 要过两道门

`onStart()` 只发布三个入口：

```java
publishBinderService(Context.COLOR_DISPLAY_SERVICE, new BinderService());
publishLocalService(ColorDisplayServiceInternal.class,
        new ColorDisplayServiceInternal());
publishLocalService(DisplayTransformManager.class,
        new DisplayTransformManager());
```

它们分别服务于：

- framework/系统组件的 Binder API；
- DPC 等 system_server 组件的本地调用；
- 统一保存、合成并下发全局颜色矩阵。

真正注册 observer、恢复颜色状态，要同时满足：

```text
PHASE_BOOT_COMPLETED
        &&
当前用户 USER_SETUP_COMPLETE == 1
```

用户尚未完成初始化时，服务只观察 `Secure.USER_SETUP_COMPLETE`。完成后才调用 `setUp()`。所以“Binder service 已存在”不等于颜色策略已经恢复。

用户生命周期也不是每次 `onStartUser()` 都抢占当前用户：

- `onStartUser()` 仅在 `mCurrentUser == USER_NULL` 时选择用户；
- `onSwitchUser()` 总是发送 `MSG_USER_CHANGED`；
- 当前用户被停止时，`onStopUser()` 把目标切到 `USER_NULL`；
- 三者都先投递到 DisplayThread，再由 `onUserChanged()` 执行。

这避免直接在 SystemService 生命周期回调线程里做整套颜色重建。

---

## 4. setUp 有顺序，tearDown 却不是“恢复出厂色彩”

`setUp()` 的关键顺序如下：

```text
注册当前用户的 10 个 setting observer
  → 先应用反色与 Daltonizer
  → 建立 color mode → composition color space 映射
  → 选择并应用 color mode
  → 初始化 Night Display 系数、矩阵、auto mode 与 active
  → 初始化 Display White Balance
```

“先无障碍、再 color mode”很重要。`getColorModeInternal()` 会在无障碍启用时优先返回资源 `config_accessibilityColorMode`；后续 color mode 选择因此已经看到无障碍状态。

设置 observer 以 URI 最后一段字段名分派。服务自己写 Secure/System setting 后也可能收到回调，但 Night active、温度等分支先比较当前状态，通常会压掉重复应用。

### tearDown 只停止管理动作

用户离开时，`tearDown()` 做的是：

- 注销共享 settings observer；
- 停止 Night auto mode；
- `endAnimator()` 让 Night/DWB 当前动画跳到终点；
- 把全局饱和度 controller 的激活态设回 `null`。

它没有：

- 删除 `DisplayTransformManager` 中 Night、DWB、灰度、反色矩阵；
- 把 SurfaceFlinger 恢复为单位矩阵；
- 清空 Night 的 `mColorTemp` 缓存；
- 清理 `AppSaturationController` 的包/用户状态。

因此 tearDown 是“停止旧用户的监听与动画”，不是颜色硬件状态的事务式清零。新用户若还卡在 `USER_SETUP_COMPLETE` 门外，旧矩阵存在继续生效的窗口；这是从 r48 代码直接推出的多用户切换风险。

---

## 5. Night Display 有三种“状态”，不要合成一个布尔值

Night Display 至少有三层：

| 状态 | 位置 | 含义 |
|---|---|---|
| persisted active | `Secure.NIGHT_DISPLAY_ACTIVATED` | 用户设置值 |
| controller active | `TintController.mIsActivated` | nullable 的运行期状态 |
| actual matrix | DTM 的 level 100 | 当前合成链中的实际矩阵 |

`mIsActivated` 是 `Boolean`，不是 `boolean`：

```java
public boolean isActivated() {
    return mIsActivated != null && mIsActivated;
}

public boolean isActivatedStateNotSet() {
    return mIsActivated == null;
}
```

`null` 表示尚未初始化，读取时却表现为 false。初始化代码必须额外检查 `isActivatedStateNotSet()`，否则“未知”会被误当成“明确关闭”。

Night controller 的 `getMatrix()` 根据 active 返回：

```java
return isActivated() ? mMatrix : MATRIX_IDENTITY;
```

也就是说关闭 Night 并不一定从 DTM 删除 level 100；它可以在 level 100 放一张单位矩阵。功能关闭与槽位不存在不是同一件事。

### 状态改变才记录 LAST_ACTIVATED_TIME

`setActivated(activated, time)` 只在控制器已经初始化且真的翻转时写：

```text
Secure.NIGHT_DISPLAY_LAST_ACTIVATED_TIME
```

首次从 `null` 恢复持久化状态不算“用户刚切换”，不会覆盖历史时间。随后它同步设置 controller 状态、必要时回写 activated setting，并通知自动策略与 DWB，最后投递 3 秒动画。

Binder 的 `setNightDisplayActivated()` 无论状态是否变化都返回 true。这个 true 不是“发生了变色”，更不是 SurfaceFlinger 完成确认。

---

## 6. 手动开关为什么不会立刻被自动策略打回去

`LAST_ACTIVATED_TIME` 的目的不是展示历史，而是记录本时段内的手动覆盖。

有两类写入时间：

- 手动切换走 `setActivated(Boolean)`，记录 `LocalDateTime.now()`；
- 固定时段自动切换走二参数版本，记录理论边界 `start` 或 `end`。

后者使用理论边界而非回调实际到达时刻，能让“本次自动切换属于哪个时间段”保持稳定。

假设计划为 22:00—06:00：

```text
22:00 自动开启
23:00 用户手动关闭
23:30 因时间变化重新计算
```

自动策略发现最后手动变化发生在当前夜间区间内，就保留 setting 中的关闭状态，不会立即再次开启。越过下一条计划边界后，覆盖才失效。

历史时间保存为 `LocalDateTime.toString()`。读取兼容两种格式：

1. 先尝试解析 `LocalDateTime` 字符串；
2. 失败后尝试把旧值当 epoch millis，再用当前系统时区转换；
3. 都失败则返回 `LocalDateTime.MIN`。

`LocalDateTime` 本身没有时区。跨时区或改时钟后的语义依赖当前 wall clock 与代码的重新计算，不应把它当单调时间戳。

---

## 7. 固定时段模式：算法精确，但 Alarm 不负责唤醒设备

`CustomNightDisplayAutoMode.updateActivated()` 先建立当前所属时间窗：

```java
final LocalDateTime now = LocalDateTime.now();
final LocalDateTime start = getDateTimeBefore(mStartTime, now);
final LocalDateTime end = getDateTimeAfter(mEndTime, start);
boolean activate = now.isBefore(end);
```

`getDateTimeBefore()` 取“不晚于 now 的最近 start”，`getDateTimeAfter()` 再取“晚于该 start 的最近 end”。这种写法自然覆盖跨午夜时段。

接着，算法用 `mLastActivatedTime` 判断当前时段里是否发生过手动覆盖；若有，就沿用持久化 active。最后按当前状态安排下一次 start 或 end：

```java
mAlarmManager.setExact(
        AlarmManager.RTC, millis, TAG, this, null);
```

这里有三个容易遗漏的边界：

1. 类型是 `RTC`，不是 `RTC_WAKEUP`。设备睡眠时它不会以“唤醒设备”为承诺；实际变色可能等设备醒来后补发。
2. listener 的 Handler 参数为 null；该 Alarm 回调不会被显式绑定到 `mHandler`。
3. 时间/时区 BroadcastReceiver 注册时也没传 Handler，通常在注册线程的主 Looper 回调。

auto mode 的创建通常发生在 DisplayThread，而 receiver/Alarm 回调可能来自主线程。`mStartTime`、`mEndTime`、`mLastActivatedTime` 没有锁或统一线程封送，因此 r48 不能被描述成严格的单线程状态机。

### 设置存秒，边界计算只取时和分

custom start/end 通过 `LocalTime.toSecondOfDay() * 1000` 持久化；但 `getDateTimeBefore/After()` 重新组装日期时只使用 hour、minute，没有带 second/nano。

因此非整分钟值虽然可以保存和读回，真正的边界判断会落在该分钟的 `:00`。普通 UI 多半只提供分钟粒度，但这是服务端的实际语义。

---

## 8. Twilight 模式：没有有效天文状态时保持当前值

`TwilightNightDisplayAutoMode` 向 `TwilightManager` 注册 listener，且显式把 `mHandler` 传进去，所以 twilight 更新回到 DisplayThread。

核心逻辑先取：

```java
boolean activate = state.isNight();
```

再用最后一次激活时间判断当前日出/日落区间里是否有手动覆盖。若有，仍以 `NIGHT_DISPLAY_ACTIVATED` setting 为准。

`TwilightState` 为 null 时，代码直接返回：

```java
if (state == null) {
    return;
}
```

因此定位、天文计算或初始化暂时没有状态时，服务不会擅自关闭 Night Display，而是保留当前 controller/矩阵状态。这里没有“无 Twilight 数据就回退到固定时段”的逻辑。

固定时段和 Twilight 共用同一个 `LAST_ACTIVATED_TIME` 机制，但更新来源不同：

| 模式 | 驱动 |
|---|---|
| custom time | exact RTC alarm + 时间/时区广播 |
| twilight | `TwilightManager` listener |
| disabled | 只响应手动设置 |

切换有效 auto mode 时，旧 mode 会 `onStop()`，随后新建并 `onStart()`。

setter 本身没有验证枚举范围。`setNightDisplayAutoModeInternal()` 会把任意 int 写入 setting；普通 getter 发现值不属于 disabled/custom/twilight 时记录错误并按 disabled 返回，而 raw getter 仍返回原值。也就是说“持久化值”“有效策略值”仍可能不同。若新入参与当前有效 mode 不同，setter 还会先清空 `LAST_ACTIVATED_TIME`。

---

## 9. 色温如何变成 4×4 矩阵

Night matrix 初始为单位矩阵，只改变 RGB 对角项：

```text
| red    0      0    0 |
| 0      green  0    0 |
| 0      0      blue 0 |
| 0      0      0    1 |
```

每个通道由 CCT 的二次多项式计算：

```text
channel(cct) = a × cct² + b × cct + c
```

总计 9 个系数。来源取决于当前 color mode 是否需要在线性空间应用矩阵：

```java
needsLinear
    ? config_nightDisplayColorTemperatureCoefficients
    : config_nightDisplayColorTemperatureCoefficientsNative
```

所以切 color mode 不只是让 SurfaceFlinger 换一个枚举；服务还会重选 Night 系数，并用当前 setting 温度重算矩阵。

激活/关闭走 3000 ms、`fast_out_slow_in` 的矩阵逐元素插值；调节色温走 `MSG_APPLY_NIGHT_DISPLAY_IMMEDIATE`，直接替换 level 100，没有 3 秒动画。

### 一个真实的 clamp 缺口

setting 读取与 getter 都会把温度钳到资源定义的 min/max；但 Binder setter 的执行顺序是：

```java
mColorTemp = temperature;
Secure.putIntForUser(..., temperature, mCurrentUser);
onColorTemperatureChanged(temperature); // 原值直接 setMatrix
```

`ColorDisplayManager` 这一 API 甚至没有 `@IntRange`，服务端也没有先 clamp。因此有权限的直接调用者传入越界值时：

- 原始值被持久化；
- 多项式立即按原始值计算并应用；
- getter 却对缓存值 clamp 后返回；
- 后续 observer/setup 从 setting 读取时又会 clamp 并重算。

于是短期“屏幕所用值”与“getter 报告值”可能不一致。常规 Settings UI 不一定触发它，但服务边界确实没有完成输入归一化。

---

## 10. 色彩模式会重建 Night，并决定 DWB 能不能工作

`getColorModeInternal()` 的选择次序是：

```text
无障碍已启用且 config_accessibilityColorMode >= 0
  → 强制使用该模式
否则读取当前用户 DISPLAY_COLOR_MODE
  → 未设置时由 persist.sys.sf.* 推导
  → 不可用时做有限 fallback
```

fallback 仅包括：

```text
BOOSTED → NATURAL
SATURATED ↔ AUTOMATIC
其他不可用值 → -1
```

`onDisplayColorModeChanged()` 做四件事：

1. 取消 Night 与 DWB 的当前 animator；
2. 按新模式重载 Night 系数并重算温度矩阵；
3. 调 `DisplayTransformManager.setColorMode()`；
4. 再刷新 DWB active 状态。

第 3 步必须早于第 4 步，因为 DWB 要询问 `DisplayTransformManager.needsLinearColorMatrix()`。

DTM 对标准模式的 native 请求大致是：

| color mode | saturation property | display color |
|---|---:|---|
| NATURAL | 1.0 | managed |
| BOOSTED | 1.1 | managed |
| SATURATED | 1.0 | unmanaged |
| AUTOMATIC | 1.0 | enhanced |
| vendor range | 1.0 | vendor mode 值 |

然后 DTM 再设置 level 100 Night matrix，并调用 `ActivityTaskManager.updateConfiguration(null)`。这个 configuration 更新是通知显示配置重算，不是颜色硬件完成 fence。

composition color space 来自两组资源数组。两数组长度不一致时整个映射保持 null；某模式无映射时向下传 `Display.COLOR_MODE_INVALID`。

---

## 11. 无障碍、Night、DWB：哪些互斥，哪些会叠加

r48 的全局矩阵 level 为：

| level | 变换 |
|---:|---|
| 100 | Night Display |
| 125 | Display White Balance |
| 150 | global saturation |
| 200 | grayscale |
| 300 | invert color |

`DisplayTransformManager` 以 `SparseArray` 升序取矩阵，并依次调用 `Matrix.multiplyMM`。因此 level 不只是标识符，也是合成顺序。

DWB active 的条件是：

```text
DWB setting enabled
  && Night 未激活
  && 反色、Daltonizer 均未启用
  && 当前颜色模式需要 linear matrix
```

所以 Night 与 DWB 明确互斥；无障碍也会压住 DWB。

但 Night 本身不会因灰度、反色或全局饱和度自动关闭。它们可以分别占据 100、150、200、300，并被 DTM 合成。最终视觉不是任何单张矩阵的独立结果，而是乘积。

Daltonizer 还有一个分叉：

- 普通色盲矫正/模拟模式走 SurfaceFlinger 的 native Daltonizer transaction 1014；
- `DALTONIZER_SIMULATE_MONOCHROMACY` 不由 native Daltonizer 实现，服务关闭 native 模式，并在 level 200 放灰度矩阵。

反色始终在 level 300 放入/删除固定 4×4 矩阵。

---

## 12. 全局饱和度：先异步排队，再做 3 秒动画

Binder `setSaturationLevel(level)` 接受新权限或旧版 `CONTROL_DISPLAY_SATURATION`。清除 calling identity 后，它只做：

```java
Message message = mHandler.obtainMessage(MSG_APPLY_GLOBAL_SATURATION);
message.arg1 = level;
mHandler.sendMessage(message);
return true;
```

因此返回 true 时，controller 甚至可能还没看到新值。DisplayThread 随后：

```text
level clamp 到 0..100
  → 计算 4×4 saturation matrix
  → 100 时 active=false 且单位矩阵
  → 其他值 active=true
  → 从 DTM 当前 level 150 矩阵动画到目标
```

矩阵使用的亮度系数是 r48 源码中的：

```text
0.231, 0.715, 0.072
```

三者相加为 1.018。这里应记录代码原值，不要凭常见 Rec.709 系数把 `0.231` “修正”为 `0.213`。

controller 自己会 clamp，所以直接 Binder 越界最终仍归一到 0 或 100。与上一节 Night 温度的未 clamp 路径不同。

tearDown 只把全局 saturation controller 的 active 设为 null，没有向 DTM 投递单位矩阵；旧 level 150 矩阵不会仅因此消失。

---

## 13. 单应用饱和度不是全局矩阵的一层

`AppSaturationController` 的键结构是：

```text
affected package
  └─ userId
      ├─ calling package A → level
      ├─ calling package B → level
      └─ WeakReference<ColorTransformController>...
```

同一目标应用可被多个调用者设置。有效饱和度取所有 level 的最小值，也就是最强去饱和：

```java
int saturationLevel = 100;
for (each caller level) {
    saturationLevel = Math.min(saturationLevel, level);
}
```

level 100 的语义是移除该调用者记录。结果转换成 3×3 matrix，加一条全零 translation，通过窗口侧 `ColorTransformController.applyAppSaturation()` 应用；它不进入 DTM 的全局 4×4 level 链。

controller 使用弱引用避免窗口对象被这张表强行保活，失效引用在 attach/update 时惰性清理。但 package/user/caller 映射没有在本类中随用户切换或包卸载主动删除，system_server 生命周期中可继续保留。

### 这里也只靠 API 注解约束范围

`ColorDisplayManager.setAppSaturationLevel()` 参数带 `@IntRange(0..100)`，服务端却没有 clamp 或 reject。`@IntRange` 是静态/API 合约，不是 Binder 运行时校验。

有权限的直接 Binder 调用者若传：

- 大于 100：该值不会成为最小值，可能等价于“没有降低”，但记录仍存在；
- 小于 0：`level / 100f` 为负，3×3 公式会外推到约定范围之外。

调用者键来自 `PackageManagerInternal.getNameForUid(Binder.getCallingUid())`。共享 UID 的名字选择、查不到时的 null 键，都不是“每个真实包天然唯一”的强身份模型；权限仍是首要安全门。

另一个实现边界是：`setSaturationLevel()` 在持有 `mLock` 时直接遍历 weak refs 并调用窗口 controller。若本地回调昂贵或重入，锁持有时间会被放大。

---

## 14. DTM 的“已应用”只到 Binder transact，不到显示完成

`DisplayTransformManager.setColorMatrix()` 会：

1. 验证非 null 数组必须恰好 16 项；
2. 在 `mColorMatrix` 锁内比较旧值；
3. 更新指定 level；
4. 按升序重算总矩阵；
5. 调 SurfaceFlinger transaction 1015。

无矩阵时它向 SurfaceFlinger 写标记 0；有矩阵时写标记 1 和 16 个 float。

调用形式是同步 Binder transact：

```java
sFlinger.transact(
        SURFACE_FLINGER_TRANSACTION_COLOR_MATRIX,
        data, null, 0);
```

但 reply 为 null，协议也没有返回 present fence。它能说明 Binder 调用返回或抛 `RemoteException`，不能证明：

- SurfaceFlinger 已在下一帧采用矩阵；
- HWC/display pipeline 已完成显示；
- 用户肉眼所见已经到达目标颜色。

动画期间每一帧都会走一次 `setColorMatrix()` 和 transaction。新动画开始前先 cancel 旧动画，并以 DTM 当前矩阵为起点；旧动画的 `onAnimationEnd` 若看到 cancelled，不会把旧目标强行写回。

### 日志中的 min/max 有一个诊断瑕疵

`TintValueAnimator` 初始化每项最大值时使用 `Float.MIN_VALUE`。Java 的这个常量是“最小正数”，不是最负数。若某个矩阵分量在整段动画里始终为负，日志里的 max 可能不准确。

它只影响动画诊断日志，不改变插值矩阵本身。

---

## 15. API 返回值、线程与多用户：一棵排障树

### 并非所有 Binder 写操作都异步

最明显的异步例外是全局饱和度：只投递 Handler 后就返回。

但 Night active、温度、auto mode、custom time、color mode、DWB setting 和单应用饱和度，大多在 Binder 线程清除 identity 后直接改 controller 或写 Settings。它们可能再触发 ContentObserver/Handler，但第一步并未统一封送到 DisplayThread。

所以并发模型应画成：

```text
DisplayThread：用户切换、setting observer、auto-mode 启停、tint 动画
Binder threads：多种 getter/setter 的直接状态或 Settings 操作
main thread：custom-time receiver / 未指定 Handler 的 Alarm 回调
other system threads：LocalService 的 DWB / window attach 调用
```

代码中没有一把总锁覆盖 Night controller、auto mode 和用户字段。分析偶发问题时，应考虑交错，不要假设“有 Handler 就全线程封闭”。

### 返回值也各不相同

| API 类型 | true/返回值通常证明什么 |
|---|---|
| Night active | 方法走完；即使状态未变化也 true |
| Night temperature | `Settings.putIntForUser` 的结果 |
| auto mode/custom time/color mode setting | Settings 写入结果或 void |
| global saturation | 消息已投递 |
| app saturation | 至少一个现存 window controller 被 update |
| DWB LocalService set CCT | DWB 当时 active，且应用消息已投递 |

它们都不是 display present ACK。

### 两个多用户状态风险

第一处很直接：`getNightDisplayLastActivatedTimeSetting()` 用的是：

```java
Secure.getStringForUser(..., getContext().getUserId())
```

而同一 setting 的写入用 `mCurrentUser`。在通常的 system_server context 下，读取用户与当前前台用户可能不一致；至少从代码契约看，这是明确的 user-id 不对称，不能假定二者永远相等。

第二处是 `NightDisplayTintController.mColorTemp`：

- Binder setter 写入这个 service-wide `Integer` 缓存；
- tearDown/用户切换不清它；
- getter 有缓存就优先返回缓存；
- setUp 构造矩阵却读取新用户的 setting。

外部 setting 变更的 observer 即使调用 `onColorTemperatureChanged(newValue)`，也不会同步更新 `mColorTemp`。因此单用户下也可能出现“矩阵已按新 setting 重算，getter 仍返回旧缓存”；跨用户后又可能出现“实际矩阵按新用户设置重建，但 getter/observer 比较仍受旧用户缓存影响”。这里描述的是 r48 可见状态模型的风险，不等同于已经用设备复现。

权限检查也不是所有接口一致。Night active/temperature 等 setter 会在服务端 enforce `CONTROL_DISPLAY_COLOR_TRANSFORMS`，但若干 getter（例如 active、temperature、raw auto mode、custom time、DWB setting）没有同样的服务端检查。`@hide` 只限制 SDK 暴露方式，不等于运行时权限；不过是否能形成实际越权还取决于 Binder service 可达性、平台签名与系统整体策略，本章不把单段代码扩大成安全结论。

### 排障顺序

```text
1. 当前 mCurrentUser 是谁，是否 boot complete + setup complete？
2. 目标 setting 写到了哪个 userId？
3. Night controller 是 null / false / true 哪一种？
4. 当前 auto mode 是 disabled、custom 还是 twilight？
5. LAST_ACTIVATED_TIME 是否让本时段保留了手动覆盖？
6. color mode 是否重载了另一组 Night 系数？
7. DWB 是否因 Night / accessibility / nonlinear mode 被压制？
8. DTM 各 level 当前分别是什么？
9. 动画是否刚被 cancel、正在过渡或已经写目标？
10. 只有 transact 证据，还是有 SurfaceFlinger/帧侧证据？
```

静态阅读可配合：

```bash
adb shell dumpsys color_display
adb shell settings get secure night_display_activated
adb shell settings get secure night_display_color_temperature
adb shell settings get secure night_display_auto_mode
adb shell settings get secure night_display_last_activated_time
adb shell settings get system display_color_mode
```

本系列不连接设备；以上命令是后续真机验证入口，不是本章已执行证据。

---

## 16. 结论：先区分控制面，再讨论最终像素

把本章压成一条因果链：

```text
当前用户完成 setup
  → ColorDisplayService 观察设置
  → 手动 / custom / twilight 决定 Night active
  → 色温 + 当前 color mode 系数生成 Night matrix
  → Night 与 DWB 做互斥判断
  → saturation / grayscale / invert 等占据各自 level
  → DisplayTransformManager 按 level 升序合成
  → Binder 请求 SurfaceFlinger
  → 后续帧才可能呈现新颜色
```

最值得保留的边界有六个：

1. setup 受 boot complete 与当前用户 setup complete 双重门控；
2. tearDown 不清空既有全局矩阵，且部分缓存跨用户存活；
3. custom alarm 是 `RTC` 非唤醒，receiver/Alarm 也未统一到 DisplayThread；
4. Night 色温与单应用饱和度存在服务端输入范围校验缺口；
5. DWB 与 Night/无障碍/非 linear mode 互斥，但其他全局矩阵可与 Night 合成；
6. Settings 成功、Handler 入队、Binder transact 返回，都不是屏幕完成显示的同义词。

下一章进入 SurfaceFlinger，继续追踪 `ColorMode`、`Dataspace` 与 `RenderIntent` 怎样进入 native composition。
