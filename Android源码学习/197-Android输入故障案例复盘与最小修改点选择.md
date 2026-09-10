# 197 Android 输入故障案例复盘与最小修改点选择

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`。  
> 本章只做源码级定位、概念修改与验证设计；当前 macOS 环境不编译、不刷写 AOSP，也不把方案写成已经在真机成立的修复。

一块触屏横纵轴颠倒、第二根手指“变成第一根”、系统手势已经识别但 App 仍在滑、`MotionEvent` 坐标正确而按钮不点击——这些现象都能在输入链上找到许多“相关代码”，却通常只有一处适合下手。

**核心结论：最小修改点不是离最终现象最近的文件，而是“最早可证明偏离预期的输出”所在层中，作用域最窄、又能恢复状态机不变量的位置。**

读完本章，你应能把十五类输入故障压缩成三道问题：

1. 错的是值、所有权，还是完成状态？
2. 哪个可观察边界第一次出现分歧？
3. 配置、Mapper、Dispatcher 与 App 四类候选中，哪一个既能修根因，又不会扩大设备、窗口、用户和安全影响面？

本章不会给某个具体硬件凭空写补丁。没有设备报告、原始事件、窗口快照与 App 侧回执，任何“直接改这里”的答案都只是一种猜测。

## 1. 先把“症状位置”与“修改位置”分开

输入故障最容易犯的错误，是从用户看到的最后一幕倒推文件名：

```text
按钮没点击        → 改 View
App 收错窗口      → 改 Dispatcher
坐标不对          → 改 TouchInputMapper
长按不重复        → 改 KeyboardInputMapper
输入 ANR          → 改 timeout
```

这五条都可能碰巧有效，也都可能只是把更早的错误遮住。Android 输入链同时变换三种东西：

| 维度 | 典型问题 | 第一批应核对的对象 |
|---|---|---|
| 值 | 坐标、keyCode、displayId、pointer 数量不对 | `RawEvent`、Raw/Cooked state、`KeyEntry` / `MotionEntry` |
| 所有权 | 事件到了错误窗口、错误 child、错误 monitor | `TouchState`、`InputTarget`、`InputState`、`TouchTarget` |
| 完成 | DOWN 有而 UP/CANCEL 没有、队列不退、注入等待超时 | outbound/wait、`FINISHED`、channel 状态、App stage |

“最早偏离”应按这三个维度分别寻找。值正确，不代表窗口所有权正确；目标正确，不代表 socket 已 publish；App 已收到，不代表 View 消费；`FINISHED` 已回，不代表点击回调已经运行。

下面十五个现场最终会落到同一套方法：

| 现场 | 首先证伪什么 | 不应立即做什么 |
|---|---|---|
| 全局 X/Y 对调 | evdev axis 与 Reader cooked 坐标 | 在每个 App 交换坐标 |
| 右下角误差越来越大 | raw range、viewport 与缩放斜率 | 加一个固定 offset |
| 边缘虚拟键误触 | virtual-key 区域、quiet 与 key 状态 | 全局丢同 keyCode |
| 第二指变第一指 | pointer index 与 pointerId | 给数组位置打补丁 |
| 触控板双指飞跳 | raw contact 与 gesture reference | 在单 App 做低通 |
| 实体键长按不 repeat | raw value 2、设备 repeat 能力与 Dispatcher timer | 同时保留两套 repeat |
| Key DOWN 有而 UP 丢 | 丢在 EventHub、Mapper、Dispatcher 还是 App | App 定时伪造普通 UP |
| MOVE 进 B 仍发给 A | 它是否仍是同一手势 | 每次 MOVE 重新命中 |
| monitor 看见但 App 仍滑 | 是否真正 pilfer、是否送达 CANCEL | 把“看见”等同“接管” |
| 窗口销毁后仍认为按下 | 快照、channel、cancel 三个完成点 | 向 broken channel 无限重试 |
| App 输入 ANR | publish 后等待谁 finish | 全局放大超时 |
| outbound 持续增长 | 前序 wait、fd 读取与 channel 状态 | 把队列改得更大 |
| Motion 正确但 View 不点 | InputStage、intercept 与 DOWN 消费 | 回头改 Reader |
| 只有注入失败 | UID、权限、display 与同步模式 | 删除跨 UID 权限门 |
| 旋转后鼠标留旧屏 | event display 与 sprite display 是否同错 | 一律改 raw REL |

所以，本章不是十五份独立“药方”，而是一套逐层缩小因果范围的训练。

## 2. “第一处错误输出”必须带身份、时间和完成点

一句“Reader 日志正常”不是证据。输入系统里同名事件会复制、拆分、改写、重注入，一张合格的故障卡至少要能关联下面四本账。

| 账本 | 建议记录 | 为什么不能省 |
|---|---|---|
| 设备 | descriptor、name、location、vendor/product、当次 deviceId | 动态 deviceId 会变化；name 也未必唯一 |
| 事件 | event id、source、action、scan/key、pointerId、displayId、down/event time | pointer index、slot、trackingId 不是同一种身份 |
| 目标 | window id/token、channel token、ownerUid、frame、touchable region | 可见窗口不等于当前输入目标 |
| 完成 | enqueue、publish、outbound/wait、Java seq、`FINISHED`、channel status | “已选择目标”离“App 已处理”还很远 |

还应固定六项环境：

- Android tag 与厂商配置版本；
- 屏幕方向、logical/physical viewport 与 displayId；
- 输入设备是否重连、是否经历 `SYN_DROPPED`；
- 窗口栈、焦点、动画、portal、monitor 和触摸区域；
- InputFilter、IME、accessibility、pointer capture 是否启用；
- 一台正常设备或一个正常方向上的同场景对照。

**先采基线，再做变更。** 否则“修好”也可能只是重启清了旧状态、窗口恰好换了代际，或日志改变了时序。

推荐把证据按边界排成一列，而不是把所有日志按进程混在一起：

```text
evdev report
  ↓  值/能力是否正确
RawPointerData 或 scan/usage
  ↓  身份是否连续
Cooked state / Notify args
  ↓  display、action、policy 是否正确
Dispatcher EventEntry
  ↓  window target 与 per-connection 投影是否正确
DispatchEntry → publish
  ↓  outbound / wait / FINISHED
App MotionEvent / KeyEvent
  ↓  InputStage / ViewGroup / View
业务结果
```

这里的“第一处”是**当前证据能证明的最窄因果边界**，不一定已经是最底层根因。例如 Cooked 坐标首次错误，只能把问题缩到 capability、持久化校准、viewport 或 Mapper；它还不能单凭一份 dump 判定是哪一行矩阵计算错了。

诊断输出也有边界。r48 的 `dumpsys input` 由 Java 与 native 多段内容拼成，各段持锁和取快照的时刻不同；Recent 队列只保留有限历史，user build 还会隐藏输入细节。标准 `inputflinger` 独立服务的同名 dump 也可能没有 system_server 那份完整现场。因此：

- dump 可证明“采样时账本里有什么”；
- dump 不能自动证明所有段属于同一原子时刻；
- 某字段没打印，不等于对象里没有；
- 一次正常 dump 不能否定短暂的中间态。

## 3. 先排候选，再选作用域最小的根因修复

把第一处分歧定位后，至少列两个候选修改点。只写一个候选，往往说明还没有比较影响面。

| 修改层 | 适合修什么 | 典型影响面 | 主要回归风险 |
|---|---|---|---|
| 驱动 / 固件 | 能力、axis range、tracking 生命周期本身错误 | 该硬件及所有系统 | Linux 输入协议、恢复与功耗 |
| KL / KCM / IDC / virtualkeys | 稳定、可描述、可按设备限定的映射或参数 | 命中的设备配置 | descriptor 命中、方向与热配置 |
| `InputMapper` | raw→Android 语义或设备特有状态机错误 | 同 mapper/source，可能跨设备 | action、identity、reset、repeat |
| DMS / IMS / WMS 生产端 | viewport、display 关联、窗口快照错误 | display 或窗口类型 | 旋转、多屏、动画时序 |
| `InputDispatcher` | 平台级命中、路由、队列或安全不变量错误 | 所有设备、窗口、monitor、注入 | CANCEL、split、ANR、安全 |
| App / View | Framework 交付已正确，业务消费契约错误 | 一个界面或组件 | 手势冲突、生命周期 |

缩写在本章只表示职责边界：KL/KCM/IDC 分别是按键布局、按键字符映射与输入设备配置；DMS/IMS/WMS/SF 分别负责显示、输入、窗口与合成相关的系统控制面。它们彼此传递快照，不是一个可随意互换的“Framework 配置层”。

配置优先只是一条**有条件的偏好**：

```text
问题能被设备专属配置完整表达
&& 配置命中稳定
&& 不要求改变跨帧状态机
&& 正反向用例证明没有扩散
→ 优先配置
```

如果驱动声明的 axis capability 本身不真实，配置只是长期替它擦屁股；如果错误是 pointer 身份中途断裂，静态参数也表达不了；如果问题来自窗口生命周期，改设备配置更无意义。

评审候选时，给每项填五列：

| 候选 | 修复的第一处分歧 | 设备范围 | 事件/状态范围 | 回滚与验证 |
|---|---|---|---|---|
| A | 是否直接恢复根因 | 单 descriptor / 全 source | 单帧值 / 跨手势状态 | 能否独立开关或撤回 |
| B | 是否只遮住下游症状 | 单 App / 全窗口 | DOWN / MOVE / 完整序列 | 是否可重复复现 |

“最小”不是代码行数最少。Dispatcher 里加三行全局特判，可能比一个二十行的 descriptor 受限 Mapper quirk 风险大得多。真正的最小修改同时满足：

1. 修复当前最早分歧；
2. 不改变无关设备、display、窗口和 UID；
3. 保持 DOWN→UP/CANCEL、pointerId、队列结算与权限不变量；
4. 能用负向用例证明边界；
5. 有清晰回滚条件。

## 4. 坐标错位：用误差形状区分轴、范围、viewport 与窗口变换

坐标问题先不要看最终按钮位置，先在同一批固定物理点上比较四层数值：

```text
evdev ABS_X / ABS_Y
→ RawPointerData
→ Cooked display coordinates
→ Dispatcher 投给目标的 global/raw 与 window-local
→ View child-local
```

至少采中心、四角、四条边中点，并在 0°、90°、180°、270° 重复。误差形状比一句“偏了”更有诊断力：

| 误差形状 | 优先怀疑 | 仍需排除 |
|---|---|---|
| 全局 X/Y 对调 | axis 声明、安装方向、viewport orientation、校准 | App 或窗口又做了一次变换 |
| 中心对、越靠边越偏 | raw min/max、physical/logical 尺寸、scale | 非线性硬件误差 |
| 全域固定偏移 | physicalLeft/Top、frame offset、局部 layout | 同时存在的 scale |
| 单轴镜像 | min/max、方向与矩阵符号 | 屏幕旋转重复补偿 |
| 仅一个窗口错误 | window scale/offset、View matrix | 全局 Reader 坐标 |

**案例一：所有 App 与 show touches 都 X/Y 对调。**  
若 evdev 值符合硬件坐标文档，而 `TouchInputMapper` 输出的 Cooked 坐标已经交换，故障界线在 Reader 配置/Mapper 附近。此时逐 App 交换会让 `getRawX/Y`、系统栏、monitor 和未来窗口各自形成不同语义；Dispatcher 交换又会同时改变命中、monitor 和局部投影，层次太晚。

但“全局错”也不能直接推出某个配置一定正确。应继续核对：

- 驱动暴露的 ABS axis 与 min/max；
- 设备如何由 port、unique id 或 viewport type 选中 display；
- viewport 的 logical、physical、device frame 与 orientation；
- 按 descriptor 与 surface rotation 保存的 `TouchCalibration`；
- surface 旋转前后是否又做了同一补偿。

这里必须纠正一个常见混称：r48 的 XY affine 不是 `.idc` 里的通用仿射属性。它由 IMS 的 `PersistentDataStore` 按 descriptor 与 surface rotation 取得 `TouchCalibration`，送入 Mapper；`cookPointerData()` 先应用 `mAffineTransform`，后执行 `rotateAndScale()`。IDC 在这条链上负责 `touch.deviceType`、`touch.orientationAware`、display association 等参数，不能把两种配置源写成一件事。

**案例二：中心正确，右下角逐渐偏离。**  
这是典型斜率线索。raw 宽高按 `max - min + 1` 建立；surface scale 与 translate 又同时受 logical frame、physical crop 和 device size 影响，`rotateAndScale()` 本身不替错误结果做最终 clamp。若 raw 最大值声明比设备真实范围大，或 viewport 两套尺寸比例不一致，中心附近可能恰好接近，边缘误差却线性放大。给 X/Y 加常量只能修一个采样点。

候选选择应按证据落层：

| 第一处分歧 | 首选候选 |
|---|---|
| evdev capability/range 就不真实 | 驱动或固件 |
| raw 正确，持久化 affine 可表达且按方向稳定 | descriptor+rotation 的 `TouchCalibration` |
| viewport 快照尺寸/方向已错 | DMS/IMS 的 viewport 生产与更新 |
| Cooked 正确，所有窗口 local 都错 | Dispatcher 投影或窗口快照 |
| 只有一个 child-local 错 | View layout/matrix/业务换算 |

概念修改前要写清矩阵所处坐标系与执行顺序。把硬件安装 90° 和 display rotation 各补一次，会在一个方向“修好”、另一个方向再次旋转。正向测试也不能只点中心：

- 四方向九宫格误差；
- 两指同时位于不同象限；
- 分别核对 Mapper surface/virtual-key 的含边界判定，以及窗口 Region/View 几何的右、下开边界，不能共用一套边界假设；
- 内外屏切换、显示裁剪和窗口缩放；
- show touches、普通 App 与系统栏三方一致性；
- 旋转中正在进行的手势是取消、重建还是保持。

## 5. Virtual Key 误触：先找改道门，不要把它当 View 按钮

边缘虚拟键不是屏幕里的普通 `View`。r48 大致沿这条链建立它：

```text
/sys/board_properties/virtualkeys.<canonical-device-name>
→ VirtualKeyMap 解析中心、宽高与 scanCode
→ EventHub 经 KCM/KL 映射 Android keyCode
→ TouchInputMapper 建 raw hit box
→ consumeRawTouches() 在 cookPointerData() 前决定改道
→ NotifyKey；否则继续成为 Motion
```

这个顺序很关键：被 virtual key 消费的 raw pointer 会在 cooking 前从触摸集合移走。App 层既看不到原始触点，也无法可靠重建 Mapper 当时的 key latch、quiet window 与 scanCode。

**案例三：边缘滑动容易误触虚拟键。** 合格证据要同时含：

- 实际读取的 sysfs 文件名与解析结果；
- raw DOWN 是否在 surface 外、落入哪个 hit box；
- 本帧 touching pointer 数量与最低 Android pointer id；
- active key 的 keyCode、scanCode、DOWN/UP/CANCELED 序列；
- Reader 的全局 virtual-key quiet deadline 是否覆盖本帧；
- 同 keyCode 的实体键是否同时存在。

r48 的初次命中门比“摸到矩形”严格。上一份 raw touching 必须为空，当前是初始接触，点在 surface 外，而且恰好一指，才会尝试挑选 virtual key。按住同一 key 时当前帧继续被消费。滑出或加入第二指会先解除 active key；若该 key 未因 quiet 被标为 ignored，再发带 CANCELED 的 Key UP。随后继续执行本帧的初始接触/越界判断：若首个点已进入 surface，本帧可建立 Motion DOWN；若仍在 surface 外，本帧仍可能被整体消费，恰好一指时还可能命中另一个 virtual key。

还有两个容易反直觉的方向：

- 从 surface 内起手再滑出去，不会在手势中途武装 virtual key；
- 从 surface 外起手且初次没有命中，外部帧仍会被消费、last raw 继续保持空。随后滑入某个 key 区域，可能再次满足“初始”条件并命中。

Virtual Key 的显示矩形会换算为 raw hit box，命中检查又早于 affine cooking。于是 affine 修正了屏内坐标，不代表 virtual-key 区域也被同一矩阵自动修正；这是一条必须单独验证的负向用例。

quiet 也要与触控板的 `QUIET` gesture mode 分开：

- virtual-key quiet time 是 `InputReader` 的全局时间戳；
- TouchInputMapper 会查询它；
- 带 `POLICY_FLAG_VIRTUAL` 的 KeyboardInputMapper 也会查询它；
- quiet 命中时 active key 会记为 ignored，不发 Key DOWN/UP；它在仍命中或正常抬起的帧继续被消费，滑出后的当前帧则仍走上面的重新判断。

最小修改选择：

| 证据 | 候选 |
|---|---|
| 单一机型 hit box 过大 | 修改该设备 virtualkeys 定义 |
| sysfs 文件命中错或 scan 映射错 | 设备命名、KL/KCM 或加载链 |
| 合法滑动序列在 Mapper 状态转换错误 | 受限的 TouchInputMapper 修复 |
| 只有业务页面不接受该键 | App 正常消费/忽略该 KeyEvent |

不要在 Dispatcher 全局按 keyCode 删除事件。同一 Android keyCode 可能来自实体键盘、车机按键、遥控器或另一块触摸板；那会把一个几何问题扩成所有设备的键盘问题。

回归至少覆盖：屏幕外命中并抬起、命中后滑出、外部初次 miss 后滑入、surface 内起手再滑出、virtual key 转 surface、第二指加入、quiet 边界前后、设备 reset、实体同 keyCode 长按。最终验收对象不是“按钮没误触”，而是完整 Key 与 Motion 序列都闭合。

## 6. 多点触摸：先证明变的是 index，还是 pointerId

“第二根手指偶尔变成第一根”至少有四种编号，不能只截图 App 的数组位置：

| 名称 | 谁维护 | 生命周期 |
|---|---|---|
| slot | Protocol B 驱动状态表 | 一个槽位可承载多代 contact |
| trackingId | 驱动 | 一次物理接触 |
| pointerId | Android Reader | Framework 认为连续的一段 pointer |
| pointer index | 单笔 `MotionEvent` 数组 | 只在当前事件有效 |

pointer index 改变通常完全合法。一个 pointer 抬起后，剩余元素会压紧；r48 的一条 Mapper 打包路径又会按 pointerId bitset 从低位迭代，但 App API 并不承诺所有输入来源永远按 ID 排序。跨事件追踪必须保存 pointerId，再对每一笔调用 `findPointerIndex()`。

**案例四的第一步**不是改 Mapper，而是画下面这张表：

| eventTime | action/actionIndex | 数组 index | pointerId | 可见 tracking/slot 证据 |
|---|---:|---:|---:|---|
| t0 | DOWN / 0 | 0 | ? | ? |
| t1 | POINTER_DOWN / ? | 0..n | ? | ? |
| t2 | MOVE | 0..n | ? | ? |
| t3 | POINTER_UP / ? | 0..n | ? | ? |

`ACTION_POINTER_UP` 的事件数组仍包含即将抬起的 pointer，action index 指向本笔变化项；到下一笔它才消失。把 action index、pointer index 或“第二根”的自然语言当稳定身份，都会制造假故障。

只有在同一物理 contact 的 trackingId 稳定，而 Android pointerId 中途变化时，才把第一处分歧缩到 Mapper：

- Protocol B 是否正确选择 slot；
- 活动 trackingId 是否唯一且接触期稳定；
- release 后是否先报新 trackingId，再更新其他轴；
- 是否出现重复 trackingId、负 tracking 或 `SYN_DROPPED`；
- tracking 直映射是否失败并令整帧退到距离匹配；
- 无 tracking 时，两指交叉是否让贪心距离匹配交换身份。

r48 的 tracking 快路会把 Linux trackingId 映射到 Android pointerId。若某个 slot 无法取得合法 id，这一帧整体退到父类的距离匹配；退路只在相同 toolType 间建立候选，再按距离从小到大贪心取边，并不求全局最优。因此两指交叉或一次噪声跳变确实可能交换身份。

重复 trackingId 是另一种故障：它未必触发 fallback。两个活动 slot 可能取得同一 Android id，`idToIndex` 被后写项覆盖，最终更像“两枚 contact 塌成一枚”，而不只是 pointerId 改变。

r48 内部使用 32 位 pointer-id bitset，而单笔最大并发 pointer 是 16；slot 缓存容量又是另一项约束。这里可以说“该 Mapper 路径的 pointerId 数字空间是 0..31”，不能说“事件能带 32 指”，也不能把内部升序组装扩大成 App API 契约。

候选优先级：

1. App 缓存 index：修 App；
2. 驱动 tracking 生命周期违反协议：修驱动；
3. 硬件无法升级且错误模式可精确识别：再评估设备受限 quirk；
4. 只有明确的 Mapper 公共算法不变量被破坏，才改通用 ID 分配。

回归不能只做“两指点下再抬起”。还要交叉、同帧一抬一落、三指、Palm、hover/touch 转换、设备 overrun/reset、split 到两个窗口，并逐笔检查 action index 指向正确的 pointerId。

## 7. 触控板飞跳：raw contact 与合成 gesture 是两套坐标

触控板两指最终可能不以“两枚触摸指针”交给 App。`DEVICE_MODE_POINTER` 且 gestures enabled 时，source 以 MOUSE 为基础；`TouchInputMapper` 会先把 finger 集合解释为 HOVER、TAP、BUTTON、PRESS、SWIPE、FREEFORM 或 QUIET，再展开成鼠标式 Motion 流。

排查时要拆开五本账：

| 账本 | 观察内容 |
|---|---|
| raw contact | tracking/pointer id、位置、工具类型、数量 |
| gesture mode | PRESS、SWIPE、FREEFORM、QUIET 等状态 |
| reference | raw centroid、每指累计 delta、gesture 锚点 |
| pointer controller | 主光标位置、display、边界裁剪、button |
| 输出事件 | action、gesture pointer 数、downTime、坐标 |

**案例五：双指偶尔飞跳。** 先问跳变第一次出现在哪里：

- raw contact 已突然换 tracking 或位置：驱动/协议层；
- raw 稳定，pointerId 映射交换：MultiTouch 映射；
- raw 与 ID 稳定，PRESS/SWIPE/FREEFORM reference 突变：gesture 状态机；
- gesture Motion 坐标连续，只有屏幕 sprite 跳：PointerController/显示事务；
- Framework 与系统光标都正确，只有某 App 内容跳：App 手势解释。

r48 的多指手势尤其不能凭状态名猜行为：

- settle 控制早期加指时是否重建 reference，不是“强制等待一段时间再分类”；
- PRESS 会累计 reference delta；
- SWIPE 是合成的单 gesture pointer，不等于两指 `ACTION_SCROLL`；
- PRESS→FREEFORM 要用 CANCEL 切断旧单 pointer 语义，再建多 pointer 流；
- FREEFORM 才建立 touch-id→gesture-id 映射；
- QUIET 没有独立 timeout 请求，要等下一包输入才重新判断；
- SWIPE/FREEFORM 的 gesture 坐标与主光标可以走不同轨道，收尾 hover 因而可能回到另一位置。

这也解释了一个重要反例：raw contacts 全程稳定，PRESS→FREEFORM 的合法 CANCEL 与 reference 重建仍可能被 App 描述成“突然跳”。先把设计转换与数值异常分开，才知道有没有 bug。

在单 App 对 Mouse MOVE 做低通通常不是根因修复：系统光标、其他 App 和 monitor 仍会看到跳变，还会额外增加尾延迟。r48 `.idc` 在这里直接提供 `touch.gestureMode` 等设备属性，但 multitouch settle、minimum distance、swipe angle 和 gesture quiet interval 来自全局 Reader 配置/资源，并不是现成的每设备 IDC 阈值。为了某设备新增参数通路，本身就是比“改一个 IDC 数字”更大的工程变更。

若证据显示是稳定硬件噪声，应先判断驱动过滤或已有设备参数能否表达；若是 reference 在特定状态转换时失守，才改 Mapper 状态机；只有输出事件正确而业务缩放错误，才留在 App。

阈值修改必须同时证明慢速精细移动没有变黏、轻触与拖拽没有延后、第二/第三指加入边界没有多余 CANCEL、按钮优先级和 QUIET 仍正确。平均轨迹更平滑，不足以证明交互更好；应看跳变峰值、尾延迟与错误 mode 转换次数。

## 8. Key 的两类故障：repeat 不发生，与 UP 没到达

“长按不重复”和“DOWN 有、UP 无”都涉及 Key 状态，却不是同一条修复路径。

**案例六：实体键长按不 repeat。** r48 至少有两种 repeat 来源：

```text
驱动 EV_KEY value 2
    → Mapper 当作 down 处理

首次 DOWN
    → Dispatcher 保存 repeat seed
    → deadline 到期时合成重复
```

EventHub 打开每个 keyboard fd 时会先尝试用 `EVIOCSREP {0, 0}` 关闭内核 repeat；失败并不让设备直接不可用，也可能令 value 2 继续出现。Keyboard Mapper 把 value 1 和 2 都布尔化为 DOWN。`keyboard.handlesKeyRepeat` 只从 IDC 的 PropertyMap 读取，KL 不声明它；该属性会让 Mapper 给 DOWN 与 UP 都加 `DISABLE_KEY_REPEAT`，从而禁止 Dispatcher 合成。

这带来一个有用的反例：`keyboard.handlesKeyRepeat=true`，EventHub 又成功关掉内核 repeat，而固件没有独立重复来源，最终就会完全没有 repeat。“声明设备处理”不是“设备确实会处理”的完成证明。

Dispatcher 的合成 repeat 也不是“一到时间必发”。r48 只有 pending event 为空且 inbound queue 为空时才有机会从 repeat state 造事件；raw repeat 与合成 repeat 又共享一套全局状态。观察到连续同 keyCode 的 raw DOWN 后，Dispatcher 会增加 repeatCount 并把自己的 timer 置为最大值，所以“驱动 repeat 与 timer 必然双发”并不准确。

仍需测试混合节拍：首枚固件 repeat 很晚、交错按键、错误 IDC，以及两个设备的同 keyCode。当前 raw repeat 判定主要比较 keyCode，并未同时比较 deviceId、scanCode 与 source；不同设备的同 keyCode DOWN 可能被误看成同一重复序列。

排查必须记录：

- raw 是否真的有 value 2；
- 首次 DOWN 是否带 `DISABLE_KEY_REPEAT`；
- repeat seed 指向哪一笔 `KeyEntry`；
- policy 延迟和 inbound 积压是否一直挡住合成时机；
- 两个设备或两枚同 keyCode 键是否互相影响；
- App 是否收到 repeatCount，而不是只看业务回调。

最小候选随证据变化：

| 第一处分歧 | 候选 |
|---|---|
| 设备声明自行 repeat，实际完全不发 | 设备 IDC 或驱动 |
| 驱动发送畸形 repeat 节奏 | 驱动 |
| Dispatcher 公共 repeat 状态违反平台不变量 | Dispatcher，需跨设备回归 |
| App 收到 repeatCount 却忽略 | App |

还应保留 r48 的 Mapper 边界：一个 scanCode 已在 `mKeyDowns` 时才按 repeat 处理，每次 down 都会覆盖共享 `mDownTime`，并非每个同时按下键各存一份独立 downTime。测试两键交错不能套用理想化模型。

**案例七：DOWN 到了，UP 丢了。** 用同一 scan/key 身份沿链找断点：

1. EventHub 没见 value 0：硬件、驱动或节点读取；
2. EventHub 有 UP，Mapper 报 `key was not down` 或没有 Notify：scanCode 配对、`mKeyDowns`、reset/复合设备；
3. Dispatcher 有 UP，目标改变或连接被取消：焦点、policy、channel、InputState；
4. App native 已收，ViewRoot/View 没走到业务：App stage 与消费逻辑。

Mapper 的 UP 按 scanCode 查 `mKeyDowns`。reset 已清表、DOWN 被 virtual-key quiet 提前丢弃，或 DOWN/UP 来自不同节点/逻辑设备时，都可能让 UP 在这里被拒绝。App 定时合成一枚普通 UP 很危险：它无法还原 scanCode、meta、repeat、downTime、display、焦点和安全来源，也可能把合法长按提前终止。

几种“收尾”必须分开写：

| 触发 | r48 主要行为 | 不能推出什么 |
|---|---|---|
| DeviceReset | 按 deviceId 对各 connection 做 `CANCEL_ALL_EVENTS` | 该函数单独必然清 Dispatcher 全局 repeat |
| focused window token 改变 | 对旧连接合成 `CANCEL_NON_POINTER_EVENTS` | 任意焦点相关回调都补 Key UP |
| 仍注册窗口从快照移除 | 对对应连接合成 pointer cancel | 已断链客户端也会收到 |
| connection 已 BROKEN / unregister | 跳过合成或 drain，最终 ZOMBIE | 还能靠协议 CANCEL 修客户端 |

InputState 对“目标没有记录过 DOWN 的 Key UP”还会故意放行，以兼容 popup 等焦点场景。因此旧窗口可能先收 canceled UP，新窗口随后又收到没有本地 DOWN 的物理 UP；“每个窗口都严格 DOWN→UP”不是 Dispatcher 的无条件承诺。

若硬件永久漏 UP 且无法升级，任何 timeout 兼容都必须限定设备、按键、允许的最长按压与误取消风险。把它写成 Dispatcher 全局规则，会伤害所有正常长按设备。

## 9. 事件“还在 A”：先分普通手势粘性与新 DOWN 命中错误

**案例八：手指已经移动到窗口 B，事件却仍给窗口 A。** 先看 action。如果这是从 A 开始的同一条普通 touch 手势，MOVE 继续发给 A 是设计行为，不是一次新的 hit-test 失败。

Dispatcher 的 `TouchState` 保存 display 级路由记忆。首枚 DOWN 决定 foreground 窗口；后续帧从旧 state 建临时副本，再为本帧生成 `InputTarget`。这样做保证控件可以收到完整拖拽，即使手指越过窗口边界，也不会被邻窗中途偷走。

这条“粘性”有明确限定，不能扩大成所有 pointer action：

- split touch 的 `POINTER_DOWN` 可以按新 pointer 重新命中并增加目标；
- 满足完整条件的单指 `SLIPPERY` MOVE 可以把旧窗 CANCEL、给新窗 DOWN；
- hover 与 scroll 本来就有不同的临时命中规则；
- gesture monitor 的 pilfer 和显式 touch-focus transfer 会改变后续所有权；
- 窗口移除、device reset 等路径可能终止旧流。

所以，同一普通 MOVE 到 A 通常不需要补丁。产品若希望跨区域拖动切换，应在 App/父 View 设计手势，或使用平台已经定义的窗口语义，而不是把 Dispatcher 改成“每个 MOVE 重新选顶层窗口”。后者会破坏拖拽、长按、手写、split、wallpaper、monitor 与 CANCEL 不变量。

若这是**一枚新的 DOWN** 却命中错误，才沿窗口链核对：

```text
WMS / SurfaceFlinger 生成 InputWindowInfo 快照
→ display 窗口顺序、visible、paused、flags、touchableRegion
→ modal / region / portal hit-test
→ foreground、OUTSIDE、wallpaper、gesture monitor
→ InputTarget 的 offset、scale、pointerIds
→ per-connection DispatchEntry
```

优先区分三类第一处分歧：

| 证据 | 修改候选 |
|---|---|
| WMS/SF 输入快照的 frame、region、flags、Z 序已错 | 修快照生产或窗口生命周期 |
| 快照正确，但 Dispatcher 违反既有 modal/region/portal 规则 | 才考虑 Dispatcher |
| 顶层窗口正确，App 内 child 错 | ViewGroup/View/业务手势 |

“屏幕上 B 在最前”也不是充分证据。绘制可见性与输入可见性可能错峰；touch-modal 窗的空 region、portal 目标、trusted overlay、窗口动画和 token 代际都可能影响命中。应记录用于输入的那份快照，而不是只录屏。

坐标也要在 target 投影后再判断。Dispatcher 会按窗口 frame、window scale 和 global scale 形成 per-connection 参数；同一 `MotionEntry` 发给不同目标，可以得到不同局部坐标。若命中窗口正确而局部值错，应修窗口几何或投影，不能回头改变所有设备的 cooked 坐标。

## 10. monitor 看见不等于接管：pilfer 改写后续路由，但不证明 CANCEL 已送达

**案例九：系统手势已经识别，App 仍继续滑动。** 先区分“观测”和“所有权”：

- global monitor 在成功的 key/motion 路由后追加；
- gesture monitor 只在当前 gesture 的 DOWN 时进入 `TouchState` 快照；
- monitor 收到事件，不代表调用过 pilfer；
- pilfer 返回 native OK，也不代表 CANCEL 已经 publish 或 App 已经处理。

r48 的 Java `InputMonitor.pilferPointers()` 是 `void`。调用一路经 Host/JNI 到 native；JNI 丢弃 `status_t`，调用方没有同步成功值。native `pilferPointers(token)` 也只接收 token：Dispatcher 用 gesture-monitor 注册表反查 display，再查该 monitor 是否存在于当前 down 的 `TouchState`；deviceId 来自这份 state，而不是调用者再传一个待校验参数。

因此，正确的排查顺序是：

1. 该 token 是否仍注册为 gesture monitor；
2. 它是否在当前 display 的 DOWN 时已经进入 state；
3. state 是否仍为 down，且 token/display/device 关系能由内部账本对齐；
4. native 日志、trace 或 dump 是否显示 pilfer 真正执行；
5. non-monitor window 是否从未来路由记录移除；
6. 合成 CANCEL 是否进入其 connection 的 outbound、publish 到 wait；
7. App 是否收到并处理 CANCEL。

pilfer 内部会对非 monitor connection 请求 pointer cancel，再调用 `filterNonMonitors()`。但这两步不是一个“客户端已收 CANCEL”的事务：

- connection 没有匹配 memento，可能没有可合成事件；
- channel 已失效，取消可能无法发布；
- 原先已经排进 outbound 的 MOVE 可能位于 CANCEL 前；
- native 路由 state 已经清窗，App 端却仍暂时保留自己的触摸状态。

r48 还有一个反直觉边界：`filterNonMonitors()` 只清 `windows` 与 `portalWindows`，保留 `gestureMonitors` 以及 down/split/device/source/display；global monitor 不在 `TouchState` 中，而是在每次成功路由后独立追加。后续仅当 state.split 为 true 的 `POINTER_DOWN` 才会重新 hit-test 并可能加入窗口。因此 pilfer 不是“从此只有一个 monitor 收到整条流”的绝对独占锁。

常见最小修改点：

| 第一处分歧 | 候选 |
|---|---|
| 识别成功但组件未调用 pilfer | 系统手势组件 |
| monitor 未在 DOWN 时注册，或 display 注册关系错误 | monitor 生命周期/创建参数 |
| pilfer 已执行，TouchState 仍保留普通窗口 | Dispatcher 状态转换 |
| state 已清，但 CANCEL 未入队且 connection 正常、有 memento | Dispatcher/InputState 取消路径 |
| CANCEL 已到 App，App 仍继续业务动画 | App 手势收尾 |

不要让 App 根据边缘坐标猜测“系统手势发生了”并自行伪造 CANCEL。那会复制平台策略，也无法处理多 display、monitor 生命周期与权限边界。

## 11. 窗口销毁后的卡触摸：三种 teardown 信号并不对称

**案例十：窗口已经销毁，App 或系统仍认为手指按着。** 至少要拆成三个完成点：

1. 新输入窗口快照不再包含它；
2. server input channel 从 Dispatcher 注销并清队列；
3. client/server endpoint 最后关闭，Looper 收到 HUP 或 receiver dispose。

这三步的顺序并非简单对称，也不能只凭“先注销”就断言 WMS 生命周期有 bug。r48 的 `WindowState.disposeInputChannel()` 本来就说明先 unregister server channel；正常销毁、窗口移除和进程死亡会走不同分支。

就“窗口从输入快照移除”这条触发而言，只有此时仍能找到可工作的 connection，Dispatcher 才有机会按 `InputState` 合成并排队 CANCEL。`setInputWindowsLocked()` 会检查旧 handle，尝试取消旧 pointer 流，再从 `TouchState` 移除窗口。

若 connection 已经 unregister：

```text
移出 connection/channel map
→ drain outbound 与 wait
→ NORMAL connection 进入 BROKEN
→ 最终标记 ZOMBIE，等待引用消失
```

这条路径不是“再补一次 CANCEL”。broken connection 在 `synthesizeCancelationEventsForConnectionLocked()` 入口就被跳过；已经关闭的客户端也不可能靠服务端无限重试恢复状态。

还要记住 `InputState` 是提前账：创建 per-connection dispatch entry 时，它在 outbound push/publish 之前就跟踪输入。于是 memento 只能证明 Dispatcher **计划让该 connection 看见** 某条流，不能证明 publish 成功，更不能证明 App 收到。排障必须分别查：

| 位置 | 能证明什么 |
|---|---|
| InputState 有 memento | 服务端提前记录了期望语义 |
| CANCEL 进入 outbound | 已请求对该 connection 收尾 |
| CANCEL 进入 wait | socket publish 成功，等待回执 |
| 收到对应 FINISHED | App native 回了该 seq 的完成信号 |
| App 手势状态清空 | 客户端实际执行了本地收尾 |

最小修改也随断点变化：

- 快照长期不移除：修 WMS/SF 输入窗口生产；
- channel 正常且有 memento，快照移除却不产生取消：查 Dispatcher/InputState；
- client endpoint 尚活却没有读取：查 App Looper/receiver；
- channel 已注销或进程已死：清服务端残留并保证新窗口从干净状态开始，不能要求死亡客户端收事件；
- 重建后的 App 保留业务级“按下”状态：修 App 生命周期恢复。

验收不能只看窗口从屏幕消失。要在动画退出、旋转重建、进程死亡、display 移除、channel HUP、pilfer 与 ANR 恢复中分别核对取消、队列 drain 和新手势起点。

## 12. ANR 与 outbound 增长：先判断债停在 publish 前还是 FINISHED 前

**案例十一：App 偶发输入 ANR。** `waitQueue` 非空只证明该 connection 有事件已经 publish 到 socket、却尚未收到对应 `FINISHED`。它不自动证明业务代码卡住，也不告诉你事件停在 View、IME、native `InputQueue`、GC、Binder 还是 Looper 上。

一条正常债务线是：

```text
DispatchEntry 入 outbound
→ publish 成功
→ 从 outbound 移到 wait
→ App native 收消息并交给 Java/View
→ finishInputEvent
→ FINISHED 回到 Dispatcher
→ wait 出队
```

超时也不是所有窗口固定五秒。Dispatcher 为每笔发送取得目标的 dispatching timeout；只有缺少可用窗口配置时才回落到默认值。publish 成功后，若 connection 仍 responsive，相应 deadline 才加入 `mAnrTracker`。

到期后还要注意三点：

1. Dispatcher 先把 connection 标为 unresponsive、从 `mAnrTracker` 移除，再异步询问 policy；
2. ANR reason 会展示 oldest wait entry，但源码明确它不保证就是触发本次 timeout 的那一项；
3. policy 返回正 extension 时，Dispatcher 才把 connection 设回 responsive，并把 deadline 不晚于新截止点的 wait 项延后、重新登记；否则合成 ANR cancel。

这个 Dispatcher 层既不直接 kill App，也不会撤回客户端已经收到的事件。

所以，调大 timeout 只是把报警线向后移。只有产品交互明确允许更长同步阻塞、且核心导航不会被拖住时，才讨论特定窗口 timeout；常见根因仍是 UI 主线程锁、长 Binder、IME、native queue、GC、死循环或错误的异步完成。

**案例十二：outboundQueue 持续增长。** outbound 表示已经为该 connection 建立、尚未成功 publish 的 `DispatchEntry`，不等于“socket 一定已满”。最常见的是前序 wait 占满 channel 后遇到 `WOULD_BLOCK`，也可能只是正处于本轮 publish 顺序中；paused window 通常在目标选择阶段返回 PENDING，不能直接拿来解释已经存在的 outbound。

当 publish 真正返回 `WOULD_BLOCK`：

| 当时状态 | r48 行为 | 诊断方向 |
|---|---|---|
| wait 非空 | 保留 outbound，等待客户端追上 | App 不读/不 finish、前序背压 |
| wait 为空 | 立即 abort broken dispatch cycle，drain 队列，NORMAL→BROKEN 并通知 | fd/endpoint/协议异常，不是普通积压 |

因此“增大 outbound 容量”通常既不恢复完成协议，也只会增加延迟和内存。应把 connection token、dispatch seq、event id 与 App native/Java seq 对齐，判断最老未清债务：

- App Looper 是否持续读取 fd；
- native receiver 是否把事件交给 Java；
- Java async stage 是否 defer 后没有回调；
- `finishInputEvent()` 是否拿到正确 seq；
- monitor、IME 或自定义 `InputQueue` 是否是实际慢接收者；
- connection 是否已经 unresponsive、BROKEN 或 ZOMBIE。

性能修复要比较分布：Reader→Dispatcher、enqueue→publish、publish→App、App→FINISHED 的 p50/p95/p99、最大值和队列峰值。只报平均耗时，会把偶发但致命的尾延迟藏掉。

## 13. MotionEvent 已正确到 App：再拆 InputStage、View 树与 click

**案例十三：App 记录到正确 `MotionEvent`，View 却不点击。** “正确”必须注明观察时点和坐标空间：

- native receiver 收到的 window/global 语义；
- ViewRoot compatibility 与 scroll 处理后的坐标；
- DecorView/内容根坐标；
- ViewGroup 应用 parent scroll、child left/top、inverse matrix 后的 child-local 坐标。

一个层次上的 X/Y 正确，不保证下一个层次仍正确。

ViewRoot 先走 InputStage。事件可能返回：继续 `FORWARD`、已处理 `FINISH_HANDLED`、未处理完成，或异步 `DEFER`。它还会在 root removed、stopped、ambient、transition 等状态下丢弃或把终止事件改成 canceled 后继续。pointer-class Motion 通常跳过 IME，但 NativePostIme 的 `InputQueue` 仍可能 defer 或消费；不能用“是触摸”推出它必然直达 View。

进入顶层 View 后，常见回调环是：

```text
ViewRootImpl: mView.dispatchPointerEvent
→ DecorView.dispatchTouchEvent
→ Window.Callback（通常是 Activity）
→ Activity.dispatchTouchEvent
→ Window.superDispatchTouchEvent
→ DecorView / ViewGroup 的正常派发
```

安全过滤也不是 DecorView 独占逻辑；实际门在 View/ViewGroup 继承的 `onFilterTouchEventForSecurity()`。

到 `ViewGroup.dispatchTouchEvent()` 后，继续区分五个完成点：

1. child 几何命中；
2. child 整体 dispatch 初始 DOWN 返回 true；
3. 因而建立 `TouchTarget`；
4. 后续 UP 仍保持 pressed/click 资格；
5. `PerformClick` 真正运行并产生业务副作用。

`OnTouchListener` 返回 false 不等于 child 整体返回 false；`View.dispatchTouchEvent()` 还会继续调用 `onTouchEvent()`。只有 child 对初始 DOWN 的完整 dispatch 最终为 false，ViewGroup 才不会为它建立 TouchTarget。

DOWN 后归属也有合法变化：父容器可在中途 intercept，旧 child 应收到 CANCEL，后续由父处理；split motion 可用 `pointerIdBits` 给多个 child；新 DOWN 又会清掉上个手势的 disallow-intercept 标志。把“归属永不变”写成绝对规则，同样会误诊。

点击还有一个时间边界：`View.onTouchEvent()` 在 UP 上可能 post `mPerformClick`。因此当前输入已经 handled、甚至外层 FINISHED 已开始回传，也不能推出 `OnClickListener` 的副作用已经执行。

针对不同证据，最小修改点分别是：

| 第一处分歧 | 候选 |
|---|---|
| InputStage 被错误 consume/defer/drop | 对应 ViewRoot/InputQueue/IME 接入层 |
| parent 状态机误 intercept | 父 ViewGroup |
| child 初始 DOWN 整体不消费 | child 的 listener/onTouchEvent 契约 |
| child-local matrix/layout 错 | layout、animation 或 matrix |
| UP 丢失 pressed/click 资格 | View 状态与 CANCEL/intercept 处理 |
| click 已执行，业务无效果 | 业务回调之后的逻辑 |

修这一层时仍要保留负向用例：滑出、CANCEL、父中途拦截、split、多指、非 identity matrix、遮挡过滤、禁用 View 与 accessibility target。只让一次中心点击成功，不能证明 View 手势契约恢复。

## 14. 注入失败与多显示错位：控制面、可信度和视觉要分账

**案例十四：只有注入事件失败，硬件事件正常。** 这首先证伪 Reader/Mapper 是共同根因，应查看 injection state、目标 UID、display 与同步模式。

Dispatcher 会自行给注入事件加 `POLICY_FLAG_INJECTED`；只有调用者通过注入权限检查时，才加 `POLICY_FLAG_TRUSTED`。有 injection state 时，只要目标为 null，或目标窗口 ownerUid 与 injectorUid 不同，就要求注入权限。同 UID 可在没有 `INJECT_EVENTS` 时路由到自己的窗口，却不能由此推导事件是 trusted，也不能推导它可以跨 UID 或无目标注入。

三种同步模式只改变“等待到哪里”：

| 模式 | 成功最远能证明什么 | 仍不能证明什么 |
|---|---|---|
| ASYNC | 事件被接受进入 inbound 处理 | 已命中、已 publish、已处理 |
| WAIT_FOR_RESULT | 路由/policy 得到结果 | App 已 finish、handled=true |
| WAIT_FOR_FINISHED | foreground dispatch 计数归零 | monitor 完成、业务成功、窗口仍在 |

WAIT_FOR_FINISHED 不等待 monitor，也不要求 `handled=true`；异常 queue drain 也会减少 foreground 计数。带 history 的 Motion 在 r48 又是多枚 `MotionEntry` 展开，只有末项持有用于同步结账的 injection state。测试工具若只看一个 boolean，极易把“已接收请求”误写成“目标执行成功”。

普通 Binder 注入者也不能靠传 flag 获得内部 `FILTERED` 或 TRUSTED 身份；InputFilter 的默认透传会走受控的 filtered reinjection 路径，这是另一条系统内部协议。修测试工具不应删除 `checkInjectionPermission()`，也不应让调用方伪造 policy 位。

安全审查还要覆盖遮挡。r48 默认 `View.onFilterTouchEventForSecurity()` 只有在 View 开启过滤，且事件带 `FLAG_WINDOW_IS_OBSCURED` 时才拒绝；`FLAG_WINDOW_IS_PARTIALLY_OBSCURED` 本身不会自动触发同一默认拒绝。正确修复应核查 overlay 所有者、trusted-overlay、Z 序和 App 的显式安全策略，不能简单清掉 Dispatcher 写入的安全 flag。

**案例十五：旋转后鼠标仍在旧屏。** 要把事件坐标与视觉 sprite 分开：

```text
DMS 产生 DisplayViewport 列表
→ IMS/Reader configuration mailbox
→ CursorInputMapper 选择 display、变换 REL
→ PointerController 保存位置与 display
→ Sprite/Surface 事务显示光标
```

如果 App 看到的 event displayId 和 logical coordinates 都错，查 viewport 生产、default pointer display、Reader change bit 与 Mapper 重配置。若事件已属于新 display，只有光标图标仍留在旧屏，则故障更靠 PointerController/Sprite/显示事务；改 raw REL 会把正确的数据面也破坏。

viewport 更新是异步控制面。Java 请求返回或 change bit 已投递，只证明控制请求进入邮箱；若观察到对应 InputDevice/Mapper generation 增长，则能证明 Reader 已执行相应 configure，但仍不能证明旧在途 Motion 已排空，或 sprite 的 Surface 事务已经显示。旋转中测试必须用同一时间线记录：

- viewport 快照的 uniqueId、port、orientation、logical/physical frame；
- Reader 何时应用配置；
- CursorInputMapper 与 PointerController 当前 display；
- 事件 displayId 与坐标；
- sprite 真正出现在哪块屏；
- 正在按下的鼠标按钮或 capture 流怎样收尾。

这两个案例看似无关，实际都在提醒：控制请求返回、内部信任标记、事件数据与视觉结果是四种不同完成点。

## 15. 一份可评审的概念补丁，要同时带不变量、测试与回滚

在当前 macOS 环境里，合格交付不是一段假装能编译的 diff，而是一份可交给 Linux/userdebug 环境验证的概念补丁单：

```text
症状与稳定复现：
正常对照：
第一处错误输出：
证据身份与时间线：
候选 A / B：
选择理由与作用域：
保持的不变量：
概念修改：
正向 / 负向 / 状态切换 / 安全 / 性能测试：
观察盲区：
回滚条件与升级风险：
```

不变量要写成带前提的可验证命题，而不是愿望。例如：

- 同一正常 touch 手势内，活跃 pointerId 唯一且跨事件稳定；
- 当上游产生 UP，或发生 device reset、窗口移除、显式取消等收尾条件，且 connection 仍可工作时，已收到 DOWN 的客户端应得到对应 UP/CANCEL；
- 未授权注入不能到达跨 UID 目标，null-target 注入也不能绕过权限；
- pilfer 后普通窗口不再获得未来常规路由，但不能据此承诺已排队事件消失；
- connection 的一笔 wait 债只能由匹配 FINISHED 或明确 teardown 结算；
- descriptor 受限修复不改变正常键盘、鼠标、外屏与其他方向。

测试按五层证据分级：

| 层级 | 能证明什么 | 不能冒充什么 |
|---|---|---|
| 源码检查与手算 | 候选路径、条件与预期状态 | 运行结果 |
| 单元测试 | 隔离 seam 的输入/输出 | 完整跨进程链 |
| native integration / uinput | Reader/Dispatcher 组合 | Java View 与真实硬件 |
| Framework instrumentation / CTS | App/View 契约 | 厂商驱动与物理时序 |
| 真机 trace、日志与重复统计 | 实际设备、窗口、IME、显示时序 | 所有机型普遍成立 |

r48 已有不少 InputReader/InputDispatcher 测试，但 Java `ViewRootImplTest` 主要覆盖 layout/insets compatibility，`ViewGroupTest` 与 CTS 的现有输入用例也不足以自动证明中途 CANCEL、matrix、split、安全过滤和异步 click 的组合。不能因为“仓库里有 Test 文件”就声称端到端覆盖。

每个修改至少准备五类用例：

1. **正向**：原故障在同一基线上消失；
2. **负向**：同 keyCode 实体键、其他 descriptor、其他 display/方向不变；
3. **状态切换**：旋转、窗口销毁、设备重连、filter/IME/capture/pilfer 切换；
4. **安全**：跨 UID 注入、monitor token、遮挡 flag、日志脱敏不退化；
5. **性能**：p95/p99、最大延迟、outbound/wait 峰值与事件丢序不恶化。

日志只加在状态边界，并打印可关联而不过度敏感的身份：device/display/event、action、pointer/key、token/channel 的稳定摘要、旧→新状态、queue size。不要无条件记录每个 MOVE 的精确坐标与按键内容，也不要在热锁区用大量格式化输出改变问题时序。

最后看升级面：Android 新版本是否已经迁移相关逻辑，设备选择器是否仍稳定，新增配置是否有默认值和反序列化兼容，测试是否能在 rebase 时自动报警。代码最短、当场现象消失，都不等于风险最小。

## 16. 九组只读练习：把“应该改哪里”变成可复算结论

下面九组命令都只读取 Android 11 r48 源码树。每题先写“预期不变量”，再从代码填条件与失败分支；产物是候选修改说明，不是编译或真机验收结果。

### 练习 1：建立触摸坐标变换账本

```bash
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h \
  | sed -n '32,52p'
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp \
  | sed -n '612,805p;1343,1361p;2178,2245p'
nl -ba frameworks/base/services/core/java/com/android/server/input/InputManagerService.java \
  | sed -n '950,995p'
nl -ba frameworks/base/services/core/java/com/android/server/input/PersistentDataStore.java \
  | sed -n '85,111p;449,480p'
nl -ba frameworks/native/services/inputflinger/tests/InputReader_test.cpp \
  | sed -n '4599,4765p;7278,7345p'
```

画出 raw min/max、natural surface、viewport、affine、rotation/scale 的顺序。分别为“全局 X/Y 对调”和“中心正确、边缘渐偏”列两个候选，说明还缺哪份设备证据才能二选一。

验收点：不能把 `TouchCalibration` 写成 IDC 属性；还要解释 `max-min+1` 与 logical/physical frame 如何产生斜率误差。

### 练习 2：推演 Virtual Key 的完整收尾

```bash
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp \
  | sed -n '1045,1094p;1506,1539p;1725,1838p;3660,3682p'
nl -ba frameworks/native/services/inputflinger/tests/InputReader_test.cpp \
  | sed -n '4167,4255p;4338,4388p'
```

逐帧推演四条序列：外部命中后正常抬起、命中后滑出、外部初次 miss 后滑入、quiet suppression。每帧填写 last/current raw touching、active/ignored key、Key 输出、Motion 输出。

再加两条负向判断：surface 内起手滑出为什么不会武装 key；affine 改正屏内坐标后为何不能自动证明 raw virtual-key hit box 也正确。

### 练习 3：区分 trackingId、pointerId、index 与 action index

```bash
nl -ba frameworks/native/services/inputflinger/reader/mapper/MultiTouchInputMapper.cpp \
  | sed -n '239,331p'
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp \
  | sed -n '1425,1455p;1838,2012p;3532,3565p;3683,3865p'
nl -ba frameworks/native/services/inputflinger/tests/InputReader_test.cpp \
  | sed -n '5405,5485p;5681,5768p;5856,5945p'
```

用两指交叉做两遍：第一遍有稳定、唯一 trackingId，第二遍没有 trackingId。记录每帧 id bitset、数组 index 与 action index，说明“index 改变但 pointerId 稳定”为何合法，以及贪心 fallback 在哪种轨迹上会换身份。

最后构造两个 slot 使用重复 trackingId，预测 `idToIndex` 后写覆盖后为何可能表现为 contact 塌缩。

### 练习 4：把触控板的 raw、reference、gesture 与光标分开

```bash
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.cpp \
  | sed -n '2317,2520p;2546,2675p;2930,3265p'
nl -ba frameworks/native/services/inputflinger/reader/mapper/TouchInputMapper.h \
  | sed -n '535,665p'
nl -ba frameworks/native/services/inputflinger/reader/InputReader.cpp \
  | sed -n '680,706p'
nl -ba frameworks/native/services/inputflinger/reader/mapper/CursorInputMapper.cpp \
  | sed -n '112,193p;300,345p'
```

画 PRESS→SWIPE 与 PRESS→FREEFORM 两条状态线，标出哪条保持单 pointer，哪条要 CANCEL 后重建。再分别记录 referenceTouch、referenceGesture 与 PointerController position，解释 raw 稳定时为什么仍可能看到合法的坐标轨道切换。

检查 QUIET 的退出条件，并指出源码为什么需要下一份输入，而不是到 deadline 自动发一笔事件。

再把 display-info change、CursorInputMapper configure、PointerController displayId 和输出 Motion 的 displayId 接成一线，说明 event 已到新屏但 sprite 仍在旧屏时，为什么不能继续改 raw REL。

### 练习 5：核对 Key repeat、reset 与 canceled UP

```bash
nl -ba frameworks/native/services/inputflinger/reader/EventHub.cpp \
  | sed -n '1488,1518p'
nl -ba frameworks/native/services/inputflinger/reader/mapper/KeyboardInputMapper.cpp \
  | sed -n '173,206p;209,233p;270,355p'
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp \
  | sed -n '1007,1075p;1111,1147p;2799,2828p;3691,3749p'
nl -ba frameworks/native/services/inputflinger/dispatcher/InputState.cpp \
  | sed -n '41,89p;218,297p;402,450p'
nl -ba frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp \
  | sed -n '1214,1258p;1725,1824p'
```

分别画 timer repeat、raw value 2、focus change 与 device reset 四条线。填写 `DISABLE_KEY_REPEAT`、repeatCount、共享 downTime、InputState memento 与最终 action flags。

必答反例：为何 `handlesKeyRepeat=true` 反而可能得到零 repeat；为何两个设备的同 keyCode 需要专门测试；为何 DeviceReset 的 canceled UP 不能推出全局 repeat seed 已由该函数清除。

### 练习 6：判断新 DOWN、旧 MOVE 与窗口局部坐标

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp \
  | sed -n '802,842p;1562,1805p;1890,2028p;2209,2425p;2456,2553p'
nl -ba frameworks/native/services/inputflinger/dispatcher/TouchState.cpp \
  | sed -n '27,147p'
nl -ba frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp \
  | sed -n '1034,1113p;2197,2357p'
```

先推演 A 内 DOWN→移入 B 的普通 MOVE，再推演 split POINTER_DOWN 与合法 slippery transfer。分别写 TouchState、InputTarget、per-connection pointer 子集与 resolved action。

然后让 A 的 frameLeft 与 window scale 改变而 identity 不变，证明“不重新 hit-test”与“局部坐标仍可变化”可以同时成立。

### 练习 7：把 pilfer、窗口移除、CANCEL 与注入权限放进一张矩阵

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp \
  | sed -n '2056,2072p;2655,2678p;2799,2864p;3274,3308p;3398,3467p;3510,3562p;3691,3793p;4359,4402p;4429,4488p'
nl -ba frameworks/native/services/inputflinger/dispatcher/InputState.cpp \
  | sed -n '27,193p;218,297p;402,450p'
nl -ba frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp \
  | sed -n '1214,1258p;1506,1580p'
```

矩阵行写 connection NORMAL/BROKEN、memento 有/无、window still registered/removed；列写“请求 CANCEL、进入 outbound、publish、App 收到”。再叠加 pilfer，指出 native OK 最远能证明什么。

最后比较 same-owner、cross-UID 与 null-target 注入，说明权限、TRUSTED 与三种 sync mode 为什么是三套独立判断。

### 练习 8：从 outbound 到 FINISHED 定位 ANR

```bash
nl -ba frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp \
  | sed -n '79,101p;489,530p;2456,2678p;4545,4698p;4737,4806p'
nl -ba frameworks/native/services/inputflinger/dispatcher/AnrTracker.cpp \
  | sed -n '20,80p'
nl -ba frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp \
  | sed -n '2362,2728p'
```

为三种现场写债务位置：outbound 尚未 publish、wait 已 publish 未 finish、WAIT_FOR_FINISHED 因 teardown 归零。再比较 `WOULD_BLOCK` 时 wait 空与非空的分支。

产物必须注明窗口 timeout 的来源、默认值适用条件、policy extension，以及 oldest wait entry 为什么不一定等于触发超时的那项。

### 练习 9：还原 Java View 回调链并设计缺口测试

```bash
nl -ba frameworks/base/core/java/android/view/ViewRootImpl.java \
  | sed -n '1145,1160p;5290,5425p;5590,5845p;5940,6035p;8037,8122p'
nl -ba frameworks/base/core/java/com/android/internal/policy/DecorView.java \
  | sed -n '442,488p'
nl -ba frameworks/base/core/java/android/app/Activity.java \
  | sed -n '4121,4129p'
nl -ba frameworks/base/core/java/android/view/View.java \
  | sed -n '14275,14361p;14566,14571p;15656,15745p'
nl -ba frameworks/base/core/java/android/view/ViewGroup.java \
  | sed -n '2633,2835p;2880,2962p;3045,3145p;3226,3290p'
```

画出 ViewRoot stage、Callback/super 回环与 ViewGroup TouchTarget。分别为 DOWN 整体 false、中途 intercept→CANCEL、inverse matrix、obscured filtering、异步 click 指定一个断言点。

最后回答：`OnTouchListener=false` 为何不等于 child=false；handled/FINISHED 为什么都不能单独证明 `OnClickListener` 已执行。

完成九组练习后，用下面六句话验收自己的修改选择：

1. 我能指出第一处错误输出，并给出同一身份、同一时间线上的前后对照；
2. 我列过至少两个候选，并解释为什么被淘汰者只是下游遮掩或影响面更大；
3. 我没有把配置、状态请求、队列入账、socket publish、客户端回执和业务结果混成一个“完成”；
4. 我写明了 DOWN/UP/CANCEL、pointerId、路由、FINISHED 与权限不变量的成立前提；
5. 我设计了正向、负向、切换、安全和尾延迟测试；
6. 我把当前成果称为源码推演与候选方案，等待 Linux 构建、自动测试和真机证据闭环。

下一篇进入 **198 Android 输入定制的补丁组织、测试与升级策略**：把本章选出的最小修改点放进可维护的提交结构、分层测试、设备配置、回滚与版本升级流程。
