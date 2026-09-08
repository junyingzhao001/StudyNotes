# 180 Android KeyboardInputMapper、Key Layout 与按键状态

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`（`frameworks/base` 提交 `1d9b9ab5`）  
> 学习方式：macOS 只读源码，不要求编译，不要求连接设备  
> 前置章节：第 20、174、176、179 章

---

## 1. 本章只追一个问题：一笔 EV_KEY 怎样成为稳定的 KeyEvent

当 `getevent` 已看到按键，App 却收到错误 keyCode、组合键状态粘住、长按不重复，或切换物理布局后 DOWN/UP 对不上时，需要把 Reader 的键盘状态机与 Dispatcher 的后处理拆开。读完本章，应能从一组 `MSC_SCAN/EV_KEY` 手算 Mapper 输出，并判断问题属于 KL/KCM、按下记录、meta/LED、repeat 还是下游 fallback；窗口选取、IME 消费和 InputChannel 回执沿用第 174—176 章，不在这里重讲。

按下键盘上的 A，看似只有一个动作，源码却要依次回答：

1. 驱动报告的是哪个 Linux `scanCode`，前面有没有一次性 HID `usageCode`；
2. 合并后的 Key Character Map 与 Key Layout 谁先把它映射成 Android `keyCode`；
3. 这是不是首次 DOWN，是否需要旋转、抑制虚拟键或取消触摸；
4. DOWN 与 UP 如何配对，modifier、锁定灯和 `downTime` 如何更新；
5. Dispatcher 是否再改键、生成 repeat/long-press，或在未处理后生成 fallback；
6. App 最终如何从 `keyCode + metaState` 查询字符。

主链可以先压成一行：

```text
MSC_SCAN(optional) + EV_KEY
  → KeyboardInputMapper
  → combined KCM usage/scan
  → KL usage/scan
  → KCM replacement
  → scanCode-keyed DOWN/UP bookkeeping
  → rotation + meta + LED + policy flags
  → NotifyKeyArgs
  → InputDispatcher policy/repeat/meta shortcut
  → KeyEvent
  → App按KCM解释字符，或在未处理后走fallback
```

最重要的边界是：Mapper 的 `mKeyDowns` 只锁住 `scanCode → keyCode`。它不会锁住 `displayId`、`policyFlags`、`metaState` 或 `downTime`；这些字段在后续 raw event 到来时仍可能使用新状态。

---

## 2. 先建立四套编号与三份状态

### 四套编号不是同一枚举

| 名称 | 示例 | 定义者 | 在链路中的意义 |
|---|---:|---|---|
| event type | `EV_KEY` | Linux input API | 说明 raw event 是键状态变化 |
| scanCode | `KEY_A = 30` | Linux evdev | 驱动上报的 code，也是 Mapper 配对键 |
| HID usage | `0x00070004` | USB HID | 可比 scanCode 更明确地描述用途 |
| Android keyCode | `AKEYCODE_A = 29` | Android | Framework 与 App 使用的逻辑键 |
| Unicode 字符 | `'a'` / `'A'` | KCM + meta | App 层文本含义 |

`KEY_A=30` 与 `AKEYCODE_A=29` 只是碰巧接近，不能相加减。真正映射必须查表。

### 三份状态分别回答不同问题

| 状态 | 所有者 | 回答什么 |
|---|---|---|
| `mCurrentHidUsage` | 单个 KeyboardInputMapper | 下一笔 EV_KEY 可否用某个 usage 辅助映射 |
| `mKeyDowns` | 单个 KeyboardInputMapper | 哪些 scanCode 已 DOWN、它们首次选出的 keyCode 是什么 |
| `mMetaState` | 单个 KeyboardInputMapper | 这台键盘当前的 Shift/Alt/Caps 等状态 |

InputReader 还把所有逻辑设备的 meta state 按位 OR 成 `mGlobalMetaState`，供鼠标、触摸等其他 Mapper 使用。这份全局状态不保存“是哪台键盘贡献了某一位”。

### 源码地图

```text
frameworks/native/services/inputflinger/reader/
├── EventHub.cpp
├── InputDevice.cpp
├── InputReader.cpp
└── mapper/KeyboardInputMapper.cpp

frameworks/native/libs/input/
├── Keyboard.cpp
├── KeyLayoutMap.cpp
└── KeyCharacterMap.cpp

frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
frameworks/base/core/java/android/view/{KeyEvent,KeyCharacterMap,ViewRootImpl}.java
frameworks/base/data/keyboards/{Generic,Virtual}.kl
frameworks/base/data/keyboards/{Generic,Virtual}.kcm
```

这些 native 组件与 Java `InputManagerService` 都在 `system_server`。`KeyboardInputMapper::process()` 跑在 InputReader 线程；它只向 `QueuedInputListener` 排队，Reader 解锁后才 flush 给 Dispatcher，不会直接调用 App。

---

## 3. 哪些设备会创建 Mapper，raw event 又怎样进入它

### 一个 Mapper 不只代表全尺寸键盘

`InputDevice::addEventHubDevice()` 根据 EventHub class 组合 source：

```cpp
if (classes & INPUT_DEVICE_CLASS_KEYBOARD) {
    keyboardSource |= AINPUT_SOURCE_KEYBOARD;
}
if (classes & INPUT_DEVICE_CLASS_DPAD) {
    keyboardSource |= AINPUT_SOURCE_DPAD;
}
if (classes & INPUT_DEVICE_CLASS_GAMEPAD) {
    keyboardSource |= AINPUT_SOURCE_GAMEPAD;
}
if (keyboardSource != 0) {
    mappers.push_back(std::make_unique<KeyboardInputMapper>(...));
}
```

所以遥控器方向键、手柄按钮也可能进入 KeyboardInputMapper。同一 EventHub 子设备若兼具鼠标、触摸能力，还会同时创建其他 Mapper；一笔 raw event 会按顺序交给该子设备的各 Mapper。

`INPUT_DEVICE_CLASS_ALPHAKEY` 只决定 Mapper 对外的 `keyboardType` 是 `ALPHABETIC` 还是 `NON_ALPHABETIC`；EventHub 以设备能否通过 KL/capability 找到 `AKEYCODE_Q` 作为这项廉价判据。KCM 的 `type FULL` 是另一份配置属性，并不会直接把 Mapper 的 `keyboardType` 设为 alphabetic，更不保证某一笔键一定能产字符。

### EV_KEY 的 value 没有直接变成 repeatCount

典型 raw event 是：

```text
type  = EV_KEY
code  = Linux scanCode
value = 0 / 1 / 2
```

Linux 常用语义是释放、首次按下、自动重复，但 r48 Mapper 只做：

```cpp
processKey(rawEvent->when, rawEvent->value != 0, scanCode, usageCode);
```

因此 `1` 和 `2` 在这里都只是 `down=true`；`NotifyKeyArgs` 根本没有 `repeatCount` 字段。

### MSC_SCAN 是只活到下一笔键的一次性前缀

常见 HID 序列：

```text
EV_MSC / MSC_SCAN / usage
EV_KEY / scanCode / value
EV_SYN / SYN_REPORT
```

Mapper 收到 `MSC_SCAN` 时覆盖 `mCurrentHidUsage`。下一笔 `EV_KEY` 先取它，再立即清零；若没有 EV_KEY，`SYN_REPORT` 也会清零。连续多个 `MSC_SCAN` 只有最后一个留下，且任何 EV_KEY——即使随后因按钮范围被 KeyboardInputMapper 过滤——都会消费它。

`usageCode` 只参与查表，不会覆盖发给 App 的 `KeyEvent.scanCode`。

---

## 4. 映射顺序不是“KL 先定键，KCM 只定字符”

`EventHub::mapKey()` 的真实顺序是：

```text
combined KCM:
  usageCode → scanCode
        ↓ 未命中
Key Layout:
  usageCode → scanCode
        ↓
KCM tryRemapKey(keyCode, oldMetaState)
```

也就是：

```text
KCM usage → KCM scan → KL usage → KL scan → UNKNOWN
```

两张表内部都只在 code 非零时查对应 map；usage 命中便不再看 scan。

### KCM 命中会绕过 KL flag

代码不是把两张表的结果合并：

```cpp
if (kcm != nullptr && !kcm->mapKey(...)) {
    *outFlags = 0;
    status = NO_ERROR;
}
if (status != NO_ERROR && haveKeyLayout()) {
    keyLayoutMap->mapKey(..., outKeycode, outFlags);
}
```

因此 combined KCM 若直接完成 scan/usage 映射，`outFlags` 从零开始；同一个 scan 在 KL 上写的 `WAKE`、`VIRTUAL`、`FUNCTION` 或 `GESTURE` 不会再补入。

### replacement 改 keyCode，不重算 flag

无论 keyCode 来自 KCM 还是 KL，最后都会用同一份 KCM 调 `tryRemapKey()`。replacement 可改 `keyCode/metaState`，但不会拿 replacement 后的 keyCode 回头再查 KL，所以 policy flag 仍属于 replacement 之前的映射结果。

### 完全映射失败仍可产生 UNKNOWN

失败时 Mapper设：

```cpp
keyCode = AKEYCODE_UNKNOWN; // 0
keyMetaState = mMetaState;
policyFlags = 0;
```

随后仍可将该 scanCode 加入 `mKeyDowns` 并发出 UNKNOWN DOWN/UP。只有 `isKeyboardOrGamepadKey()` 排除的鼠标/数字化器按钮区间会在进入 `processKey()` 前被忽略，以免和 Cursor/Touch Mapper 重复解释。

---

## 5. KL、KCM 与 layout overlay 各自负责什么

### Key Layout：硬件编号与输入策略

`Generic.kl` 的核心格式是：

```text
key 30                  A
key 42                  SHIFT_LEFT
key 143                 WAKEUP
key usage 0x0c006f      BRIGHTNESS_UP
key 465                 ESCAPE FUNCTION
led 0x01                CAPS_LOCK
```

按键条目给出 Android keyCode 和四种可选 raw policy flag：

| KL flag | native flag | Reader/Dispatcher 中的作用 |
|---|---|---|
| `WAKE` | `POLICY_FLAG_WAKE` | 交给 policy 判断唤醒 |
| `VIRTUAL` | `POLICY_FLAG_VIRTUAL` | 首次 DOWN 可防误触，Dispatcher 转成虚拟硬键 flag |
| `FUNCTION` | `POLICY_FLAG_FUNCTION` | Dispatcher 只给这一笔事件补 `AMETA_FUNCTION_ON` |
| `GESTURE` | `POLICY_FLAG_GESTURE` | 首次 DOWN 取消同一逻辑设备的触摸 |

KL 还可描述 axis 与 LED；它不是字符表。

### Key Character Map：字符、行为与可选硬件映射

`Generic.kcm` 常见内容：

```text
type FULL

key A {
    label:           'A'
    base:            'a'
    shift, capslock: 'A'
}
```

KCM 能表达：

- 键盘类型和 keyCode 的字符行为；
- dead key / combining accent；
- `fallback` 与 `replace`；
- overlay 格式中的 `map key`，即 scan/usage → keyCode。

InputReader 不会因 KCM 的字符规则额外发送一个字符事件。App 收到 KeyEvent 后，才通过设备 KCM 和 `keyCode + metaState` 查询 Unicode。

### overlay 是热替换，不是 reopen

`CHANGE_KEYBOARD_LAYOUTS` 时，`InputDevice::configure()` 向 policy 取 KCM overlay，然后对每个 EventHub 子设备执行：

```cpp
device->overlayKeyMap = map;
device->combinedKeyMap =
        KeyCharacterMap::combine(device->keyMap.keyCharacterMap, map);
```

只要 overlay 的 `sp<>` 指针与旧指针不同，EventHub 就返回 changed，逻辑 InputDevice 随后 bump generation；它不比较内容是否等价，也不销毁 KeyboardInputMapper。

`combine()` 的语义也不是任意深合并：

- 先复制 base；
- overlay 中同 keyCode 的整份 `Key/Behavior` 替换 base 条目；
- scan 与 usage map 按 code 覆盖或追加；
- base 与 overlay 都存在时，结果保留 base 对象的 keyboard type。

已经按下的键不会因 overlay 更新立即重放。后续 raw event 会用新 combined KCM 先映射，但 UP/重复 DOWN 的 keyCode 最后仍会被 `mKeyDowns` 中的旧值覆盖。

---

## 6. replacement 与 fallback 不在同一阶段

| 机制 | 何时发生 | 原键是否先送到 App | 谁执行 |
|---|---|---|---|
| KCM `replace` | Reader 映射 raw key 时 | 否 | `KeyCharacterMap::tryRemapKey()` |
| policy fallback | 前台目标报告原键未处理后 | 是 | Dispatcher + PhoneWindowManager |
| App synthetic fallback | 调用方显式重投 unhandled event 后 | 已经离开原派发轮次 | ViewRoot SyntheticInputStage |

### replacement 会临时消费部分 meta

`tryRemapKey()` 先按原 keyCode 和旧 `mMetaState` 选择 behavior；命中 replacement 后：

1. 改成 replacement keyCode；
2. 清 behavior 声明的 modifier；
3. 对 ALT、CTRL、SHIFT 额外清理通用位与左右位的依赖；
4. `normalizeMetaState()` 再补仍有左右位支撑的通用位。

它并未对 META 做与 ALT/CTRL/SHIFT 对称的依赖清理；若自定义 KCM 用 META 触发 replacement，左右 META 位可能经 normalize 把通用 `META_ON` 补回来。这是 r48 实现边界，不应外推成“所有 modifier 都一定被完整消费”。

### fallback 不是 Reader 重映射

普通窗口返回未处理后，Dispatcher 才调用 policy 的 `dispatchUnhandledKey()`。初次 DOWN 有两层锁存：

- `PhoneWindowManager.mFallbackActions` 按原 keyCode 保存 `FallbackAction`；
- connection 的 `InputState` 保存 original keyCode → fallback keyCode。

后续 repeat/UP 继续询问 policy，但 Dispatcher 要求 fallback keyCode 与首次一致；变化或取消会触发 fallback cancel。原 UP 后映射被移除。

这两张表都不是按完整 `deviceId + keyCode + target` 统一建模：policy 的 `SparseArray` 只按 keyCode，connection 表也只按 original keyCode。多设备同键交叠时，不应声称 fallback 状态天然完全隔离。

### App synthetic fallback 是另一条入口

`ViewRootImpl.dispatchUnhandledInputEvent()` 会发 `MSG_SYNTHESIZE_INPUT_EVENT`，打上内部 `FLAG_UNHANDLED`，随后 `SyntheticKeyboardHandler` 对当前这一笔重新查 KCM。它不是普通 View 树返回 false 时必经的自动路径，本类也没有 DOWN→UP fallback 表。

而且 r48 使用不带 displayId 的 `KeyEvent.obtain(...)` 重载，synthetic fallback 的 displayId 会变成 `INVALID_DISPLAY`。这条路径的行为不能套用 Dispatcher/PhoneWindowManager 的两层锁存保证。

---

## 7. DOWN/UP 状态机只用 scanCode 配对

`mKeyDowns` 的元素只有：

```cpp
struct KeyDown {
    int32_t keyCode;
    int32_t scanCode;
};
```

### 首次 DOWN

顺序是：

1. 用当前 KCM/KL 和旧 meta 映射；
2. 若 `orientationAware`，旋转 keyCode；
3. 按 scanCode 查 `mKeyDowns`；
4. 首次虚拟键可被 `shouldDropVirtualKey()` 丢弃；
5. GESTURE 键可调用 `cancelTouch()`；
6. 保存最终 `scanCode → keyCode`；
7. 更新共享 `mDownTime`、meta/LED，再发 NotifyKey。

虚拟键若在第 4 步被抑制，不会进入 `mKeyDowns`；随后的 UP 因找不到 DOWN 也会被丢弃。

### 重复 DOWN

若 scanCode 已存在：

- 不再新增记录；
- 使用保存的 keyCode，抵抗旋转或 layout 变化；
- 对已经成功入表的键，不再执行 virtual suppression 或 gesture cancel；若首次虚拟 DOWN 被抑制而未入表，后来的 value=2/重复报告仍会再次按“首次 DOWN”评估；
- 但映射已在查找记录之前重新跑过，新的 `policyFlags/keyMetaState` 没有从旧记录恢复；
- `mDownTime` 仍会被这笔 DOWN 覆盖。

### UP

UP 也先按当前 KCM/KL 映射，再按 scanCode 找旧记录：

- 找到：只把 `keyCode` 换回 DOWN 时保存的值，然后删记录；
- 找不到：记录日志并直接丢弃，不更新 meta，也不发 UNKNOWN UP。

因此“按住期间换 layout/旋转，UP keyCode 仍配对”成立；“UP 是 DOWN 全字段回放”不成立。UP 的 policy flag、keyMetaState、displayId 与 downTime 都可能不同。

### usage-only 的配对弱点

`mKeyDowns` 不保存 usageCode。若设备把多枚 usage 键的 EV_KEY code 都报告为 0，同时按下时它们会竞争同一个 scanCode=0 记录；第二枚会被视作重复并沿用第一枚 keyCode。源码测试覆盖单枚 usage-only 键，不证明多键并发安全。

---

## 8. 旋转与 displayId 是两条相关但不同的链

### viewport 选择

`findViewport()` 的顺序：

1. 设备有 associated display port：取该 port 对应 viewport；
2. 否则若 `keyboard.orientationAware=true`：取内部显示 viewport；
3. 否则没有 viewport。

有 viewport 时，NotifyKey 携带其 displayId；没有则是 `ADISPLAY_ID_NONE`。关联 port 即使不做方向旋转，也仍可给键事件指定 displayId。

### DPAD/SYSTEM_NAVIGATION 只在 DOWN 分支旋转

例如物理 `DPAD_UP`：

| viewport orientation | 首次 DOWN 的最终 keyCode |
|---|---|
| 0° | `DPAD_UP` |
| 90° | `DPAD_LEFT` |
| 180° | `DPAD_DOWN` |
| 270° | `DPAD_RIGHT` |

重复 DOWN 先计算新旋转值，却最终复用记录中的旧 keyCode；UP 直接复用旧 keyCode。于是按键期间屏幕转向不会拆坏 keyCode 配对。

不过 `mKeyDowns` 没保存 displayId。`CHANGE_DISPLAY_INFO` 会热更新 `mViewport`，所以 DOWN 在 display A、UP 在 display B 是实现上可能出现的组合。

### stem 键只有 180° 特例

`STEM_PRIMARY/STEM_1/2/3` 可由 IDC 配置：

```text
keyboard.rotated.stem_primary = ...
keyboard.rotated.stem_1 = ...
```

它只在 180° 时替换。更危险的边界是 `stemKeyRotationMap` 为文件级可变 static 数组：

- 不是每个 Mapper 一份；
- `configureParameters()` 只在首次配置调用；
- 缺失某属性时不会把旧 static 槽重置为 identity。

所以一个 orientation-aware 设备写入的 stem 映射可能污染同进程后来创建、却没写对应属性的 Mapper。

---

## 9. metaState：瞬时键、锁定键与事件临时值

### 瞬时 modifier

`ALT/SHIFT/CTRL/META` 左右键以及 `SYM/FUNCTION` 的 keyCode 走 `setEphemeralMetaState()`：

- DOWN：设置对应位；
- UP：清侧位以及 ALT/SHIFT/CTRL/META 四个通用位；
- `normalizeMetaState()` 根据仍按下的另一侧重新补通用位。

所以左右 Shift 同时按下、只抬起一侧时，`SHIFT_ON` 仍保持。

### 锁定 modifier 在 UP 翻转

`CAPS_LOCK/NUM_LOCK/SCROLL_LOCK` 走：

```cpp
if (down) {
    return oldMetaState;
}
return oldMetaState ^ mask;
```

Caps DOWN 自身不打开锁定位；配对 UP 才翻转。孤立 UP 在 `processKey()` 更早处被丢弃，因此也不会翻转。

### 当前事件携带哪份 meta

映射和更新的时序：

```text
old mMetaState
  → mapKey / tryRemapKey 得到 keyMetaState
  → 用最终且已锁存的 keyCode 更新 mMetaState
  → 若设备meta真的变化：事件使用新 mMetaState
  → 否则：事件保留 keyMetaState
```

于是：

- Shift DOWN 携带“Shift 已按下”；
- Shift UP 携带“Shift 已释放”；
- Shift+A 中 A 的 DOWN/UP 都可携带 Shift；
- replacement 若消费 Shift，普通键事件可携带临时去掉 Shift 的 `keyMetaState`，但设备自己的 `mMetaState` 仍保留 Shift。

若 replacement 把普通键变成 modifier，Mapper 会按 replacement 后 keyCode 更新真实 `mMetaState`；这说明 replacement 不只是改展示标签。

### KL FUNCTION flag 不等于 FUNCTION key 状态

`POLICY_FLAG_FUNCTION` 到 Dispatcher 才给“这一笔”事件 OR `AMETA_FUNCTION_ON`。它不会更新 KeyboardInputMapper 的 `mMetaState`，也不会让随后另一笔键自动携带 FUNCTION。

相反，真正映射成 `AKEYCODE_FUNCTION` 的 DOWN/UP 会走 Mapper 的瞬时 meta 状态机，并影响后续事件与全局 meta。两种机制必须分开。

---

## 10. global meta 与 LED 是状态的两个投影

### InputReader 的全局 meta 是 OR

每次某个 Mapper 的本地 meta 发生变化，它调用：

```cpp
mGlobalMetaState = 0;
for (each InputDevice) {
    mGlobalMetaState |= device->getMetaState();
}
```

逻辑 InputDevice 又 OR 自己所有 Mapper 的 meta。于是键盘 A 按住 Ctrl 时，鼠标 Mapper 可把全局 Ctrl 带入 MotionEvent；键盘 B 抬起自己的 Ctrl 不会清掉 A 的贡献。

这不是全局按键集合，也没有引用计数。正确性依赖每个 Mapper 先维护好自己的位，再重新 OR。

### LED 不是 meta 的输入源

reset 时 KeyboardInputMapper：

1. 只通过 KL 的 scan-code `led` 条目和 EventHub LED capability 判断 Caps/Num/Scroll 是否可用；虽然 KeyLayoutMap 还能解析 usage LED，EventHub 的 `mapLed()` 在此不查它；
2. 把本地 `LedState.on` 初始化为 false；
3. 强制调用 `setLedState(..., false)`。

因此 r48 不会读取硬件灯的初始开关来恢复锁定状态，反而会把支持的三盏灯先关闭。之后锁定 meta 在 UP 翻转时，只有期望值变化才写 `EV_LED`。

EventHub 写 LED 时只对 `EINTR` 重试；最终写入长度或其他错误没有反馈给 Mapper。`LedState.on` 仍会更新成期望值，所以它是“我们认为灯是什么状态”，不是硬件确认。

### 程序化 toggleCapsLock

`InputReader::toggleCapsLockState(deviceId)` 直接调用逻辑 InputDevice 的 `updateMetaState(AKEYCODE_CAPS_LOCK)`；KeyboardInputMapper 把它当 `down=false`，因此会翻转并更新 LED/global meta，却不生成 Caps KeyEvent。

若一个逻辑复合设备拥有多个 KeyboardInputMapper，`InputDevice::updateMetaState()` 会对全部 Mapper 调用，可能一次翻转多份本地 Caps 状态；全局 OR 只能显示最终是否至少一份为 on。

---

## 11. policyFlags 决定的是前后处理，不是 App flags 原样复制

### VIRTUAL 与 GESTURE 只在首次 DOWN触发 Reader 副作用

`POLICY_FLAG_VIRTUAL` 让首次 DOWN 经过 `shouldDropVirtualKey()`；通过后，Dispatcher 还把它转换为 `AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY`。

`POLICY_FLAG_GESTURE` 让首次 DOWN 调 `InputDevice::cancelTouch()`，它遍历同一逻辑 InputDevice 的所有 Mapper，而不只是产生该按键的 EventHub 子设备。重复 DOWN 不再取消。

### 外接设备的默认 WAKE 是附加规则

Mapper 仅在以下条件同时成立时 OR `POLICY_FLAG_WAKE`：

```text
DOWN
AND logical InputDevice isExternal
AND keyboard.doNotWakeByDefault == false
AND final keyCode 不在 isMediaKey() 列表
```

因此普通外接键的自动 WAKE 只出现在 DOWN。`isMediaKey()` 硬编码名单内的媒体/音量键不自动添加；它不是“所有名字含 MEDIA 的 keyCode”，例如 r48 的 `MEDIA_CLOSE`、`MEDIA_EJECT`、`MEDIA_TOP_MENU` 不在名单中，仍可能获得默认 WAKE。若 KL 明确写 `WAKE`，它可在 DOWN 与 UP 都保留，`doNotWakeByDefault` 也不会删除它。

`isExternal()` 是逻辑复合设备聚合后的 class。只要合并进来的任一 EventHub 子设备带 EXTERNAL，所有 KeyboardInputMapper 都会观察到逻辑设备为 external。

### KCM precedence 可让 KL flag 消失

若 overlay KCM 自己 `map key` 命中，EventHub 直接令 flags=0，不再查 KL。于是 overlay 不只是改变字符布局，还可能无意间让 WAKE/VIRTUAL/FUNCTION/GESTURE 消失。

反过来，KCM replacement 不重查 KL，可能让新 keyCode 继续携带原 keyCode 的 flag。这两种行为都来自第 4 节那条非合并映射链。

---

## 12. downTime 与 repeat：Reader、驱动、Dispatcher 是三本账

### Mapper 只有一份共享 mDownTime

单键时看似正常：

```text
t=100 A DOWN → A DOWN(downTime=100)
t=180 A UP   → A UP  (downTime=100)
```

多键并按时暴露共享字段：

```text
t=100 A DOWN → mDownTime=100
t=120 B DOWN → mDownTime=120
t=150 A UP   → A UP(downTime=120)
t=170 B UP   → B UP(downTime=120)
```

任何 DOWN——包括同 scanCode 的重复 DOWN——都会执行 `mDownTime=when`。`mKeyDowns` 没有逐键时间，所以 UP 无法恢复各自首次时间。

### EventHub 总会尝试关闭内核 repeat

只要设备带 `INPUT_DEVICE_CLASS_KEYBOARD`，打开以及重新 enable fd 时调用的 `configureFd()` 都会执行：

```cpp
unsigned int repeatRate[] = {0, 0};
ioctl(fd, EVIOCSREP, repeatRate);
```

失败只写 warning。这个动作不读取 `keyboard.handlesKeyRepeat`；IDC 参数要到 Mapper 配置时才读。因此 `handlesKeyRepeat=1` 并不会阻止 EventHub 先尝试关闭内核 repeat。

### Dispatcher 合成 repeat

Reader 进入 `InputDispatcher::notifyKey()` 时 repeatCount 固定为 0。对可信、未带 `DISABLE_KEY_REPEAT` 的初次 DOWN：

1. Dispatcher 保存全局 `lastKeyEntry`；
2. `eventTime + keyRepeatTimeout` 后合成 repeatCount=1；
3. 后续每隔 `keyRepeatDelay` 递增；
4. synthetic repeat 保留保存 entry 的 `downTime`；
5. 仅 repeatCount==1 时加 `AKEY_EVENT_FLAG_LONG_PRESS`，更高 repeat 会清掉该 flag。

### 驱动重复与 handlesKeyRepeat 的反直觉

若后续 raw DOWN 仍以 repeatCount=0 进入，且全局 `lastKeyEntry.keyCode` 相同，Dispatcher 把它识别为 driver repeat、递增 count，并关闭自己的 timer。r48 条件没有比较 deviceId 或 scanCode，所以两台设备连续按同一 keyCode 也可能被误判为同一重复序列。

若 `keyboard.handlesKeyRepeat=1`，Mapper 给每笔事件加 `POLICY_FLAG_DISABLE_KEY_REPEAT`。这会绕过上述识别和合成；raw `value=2` 又已被 Reader 压成普通 DOWN，所以 App 会收到多笔 DOWN，但每笔仍是 `repeatCount=0`，也不会由这条逻辑加 LONG_PRESS。

若 EventHub 的 `EVIOCSREP` 又成功关闭了设备内核 repeat，则该设置甚至可能同时没有硬件 repeat 与 Framework repeat；它只适合仍能自行产生重复报告的设备实现。

---

## 13. Mapper 出口之后仍可能再次改键

### NotifyKeyArgs 的完成边界

Mapper 输出字段：

| 字段 | 来源 |
|---|---|
| id | InputReader id generator |
| eventTime | `RawEvent.when` |
| deviceId/source | 逻辑 InputDevice / Mapper source |
| displayId | 当前 viewport，或 NONE |
| policyFlags | 当前 KCM/KL 映射 + 自动 WAKE + DISABLE_REPEAT |
| action | DOWN / UP |
| flags | 固定含 `FROM_SYSTEM` |
| keyCode | replacement、旋转、按下记录后的结果 |
| scanCode | 原 EV_KEY code |
| metaState | 当前设备 meta 或 KCM 临时 meta |
| downTime | Mapper 共享 `mDownTime` |

这只证明 Reader 的解释已完成，不证明最终 App KeyEvent 字段不再变化。

### Dispatcher 的 Meta 快捷键是第二层 replacement

`notifyKey()` 先处理：

```text
Meta + DEL   → BACK
Meta + ENTER → HOME
```

`mReplacedKeys` 用 `original keyCode + deviceId` 锁存，UP 即使 Meta 已释放也能得到同一 replacement，并清 Meta 位。它与 KCM replacement 是两套独立机制：前者发生在 Dispatcher，后者已在 Reader。

Dispatcher 还会：

- 把 FUNCTION policy flag OR 到事件 meta；
- 把 VIRTUAL policy flag转成 KeyEvent flag；
- 调 `interceptKeyBeforeQueueing()`；
- 经过 filter、目标选择、窗口通道与 FINISHED 结账；
- 维护 repeatCount/long-press；
- 在真正未处理后尝试 policy fallback。

### App 看到字符的完成点

KeyEvent 只携带 keyCode/meta 等字段。`getUnicodeChar()`、`getDisplayLabel()` 或输入法再通过 deviceId 取得 KeyCharacterMap。字符解释发生得比 Reader 映射晚，也可能受 App 当前拿到的 KCM 对象与 event meta 影响。

所以：

```text
EV_KEY → keyCode
```

与：

```text
keyCode + metaState → Unicode
```

是两个不同问题。

---

## 14. 查询、reset 与失败边界：事件账不等于硬件账

### key state 查询直接读内核

`getScanCodeState()`：

- 检查 scanCode 范围、设备 fd 和 key capability；
- 用 `EVIOCGKEY` 读取当前 bitmap；
- 不读取 `mKeyDowns`。

`getKeyCodeState()`：

- 只用 KL 反查该 keyCode 的所有 scanCode；
- 读取 `EVIOCGKEY`；
- 任一对应 bit 按下即返回 DOWN；它没有先把每个反查 scanCode 与设备 capability `keyBitmask` 相交。

因此它反映“驱动现在报告什么”，不等于“Mapper 曾向 Dispatcher 发过什么”。设备 disabled、fd 失效、ioctl 失败或 KL 无 scan 映射时返回 UNKNOWN。KL 有映射但驱动未声明该 capability 时，成功读取 bitmap 后仍可能报告 UP；同一个键却会被下述 supported-key 查询判为不支持。

### supported-key 查询同样偏向 KL scan

`markSupportedKeyCodes()` 只把 KL 的 `findScanCodesForKey()` 与 EventHub `keyBitmask` 相交。它不会遍历：

- combined KCM 的 `map key`；
- KL 的 usage-only 映射；
- 当前 `mKeyDowns`。

所以“运行时 mapKey 可成功”与“hasKeys 报支持”不是双向等价。

r48 KL parser 对 scan 数字本身没有 KEY_MAX 范围校验，而 `markSupportedKeyCodes()` 在 `test_bit(scanCode, keyBitmask)` 前也没有局部范围检查；恶意或错误 KL 的越界 code 是配置可信边界，不是普通设备能力语义。

### reset 的两层收尾

KeyboardInputMapper reset：

```text
mMetaState = NONE
mDownTime = 0
mKeyDowns.clear()
mCurrentHidUsage = 0
重新探测并强制关闭三类lock LED
```

它不逐键发送正常 UP。`InputDevice::reset()` 在所有 Mapper reset 后更新 global meta，并排队 `NotifyDeviceResetArgs`；Dispatcher 再根据每条 connection 的 `InputState` 合成 canceled key/pointer events。

### DeviceReset 没清所有 Dispatcher 全局表

r48 `dispatchDeviceResetLocked()` 只按 deviceId 给所有 connection 合成 cancel。它没有：

- `resetKeyRepeatLocked()`；
- 按 deviceId 清 `mReplacedKeys`。

`ConfigurationChangedEntry` 会重置 key repeat，全局 `resetAndDropEverythingLocked()` 才清 replacement 表；但一次孤立的 `SYN_DROPPED → InputDevice::reset() → DeviceReset` 不自带这两个保证。于是“窗口侧已收到 canceled UP”不能单独证明 Dispatcher 的全局 repeat/meta-shortcut 缓存也已清空。

---

## 15. 九个 macOS 只读练习

以下命令都在 `/Users/ninebot/androidSource` 执行，不编译、不改源码。

### 练习一：确认 Mapper 创建条件

```bash
sed -n '125,190p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

回答：KEYBOARD、DPAD、GAMEPAD 如何组合成一个 `mSource`？ALPHAKEY 改的是 source 还是 keyboardType？

### 练习二：手工走一遍 raw 状态机

```bash
sed -n '198,365p' \
  frameworks/native/services/inputflinger/reader/mapper/KeyboardInputMapper.cpp
```

标出 usage 的两个清零点、首次/重复 DOWN、孤立 UP、共享 downTime、WAKE 与 NotifyKeyArgs。

### 练习三：证明 KCM 先于 KL

```bash
sed -n '540,580p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '80,120p' frameworks/native/libs/input/KeyLayoutMap.cpp
sed -n '325,400p' frameworks/native/libs/input/KeyCharacterMap.cpp
```

写出 `KCM usage → KCM scan → KL usage → KL scan`，并解释 KCM 命中为何令 flags=0。

### 练习四：检查 overlay 的合并粒度

```bash
sed -n '155,200p' frameworks/native/libs/input/KeyCharacterMap.cpp
sed -n '235,270p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '665,690p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
```

回答：同 keyCode 的 behaviors 是逐条合并还是整项替换？“变化”按内容还是 `sp<>` 指针判断？

### 练习五：验证 meta 与 LED

```bash
sed -n '175,255p' frameworks/native/libs/input/Keyboard.cpp
sed -n '380,445p' \
  frameworks/native/services/inputflinger/reader/mapper/KeyboardInputMapper.cpp
```

手算左右 Shift 与 Caps Lock；再找出 reset 为什么先把支持的 lock LED 关掉。

### 练习六：追三类 repeat

```bash
sed -n '1498,1518p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '1000,1050p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '1105,1148p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

区分内核 repeat、raw driver repeat、Dispatcher synthetic repeat，并说明 `handlesKeyRepeat=1` 时 App 的 repeatCount。

### 练习七：对比两类 replacement 与两条 fallback

```bash
sed -n '350,400p' frameworks/native/libs/input/KeyCharacterMap.cpp
sed -n '3035,3080p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '3070,3150p' \
  frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java
sed -n '7100,7140p' \
  frameworks/base/core/java/android/view/ViewRootImpl.java
```

回答：谁锁存 UP 配对？哪条 App synthetic 路径丢失 displayId？

### 练习八：确认查询不读 mKeyDowns

```bash
sed -n '430,535p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
```

分别写出 scan state、keyCode state 与 supported-key 的数据源和盲区。

### 练习九：核准 reset 的完成点

```bash
sed -n '315,338p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '1055,1080p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4018,4050p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

比较 DeviceReset 与全局 reset：谁取消 connection 状态，谁清 repeat 和 `mReplacedKeys`？

---

## 16. 用三条时间线验收本章

### 时间线一：Shift + A

```text
t1 Shift DOWN → Mapper meta = SHIFT_LEFT_ON | SHIFT_ON
t2 A DOWN     → KeyEvent携带Shift；App按KCM可解释为'A'
t3 A UP       → Shift仍按住，meta仍含Shift
t4 Shift UP   → Mapper meta = NONE
```

字符 `'A'` 不是 Mapper 另发的事件。

### 时间线二：多键 downTime

```text
t=100 A DOWN
t=120 B DOWN
t=150 A UP
```

r48 的 A UP 使用共享 `mDownTime=120`，不是 100。若 t=130 还有 A 的 raw repeat DOWN，`mDownTime` 还会先变成 130。

### 时间线三：layout 在按住期间变化

```text
旧layout: scan 30 → A，DOWN时保存 (A, 30)
切换overlay与display viewport
新layout: scan 30 → B
UP到来：先映射成B及新flags/meta，随后只把keyCode换回A
```

因此 UP 的 keyCode 与 DOWN 配对，但 displayId、policyFlags、metaState 和 downTime 不受同一记录保护。

### 最终模型

```text
KeyboardInputMapper把一次性usage与EV_KEY交给“combined KCM优先、KL兜底”的映射链，
用scanCode只锁存最终keyCode，再处理旋转、meta、LED和policy flag，生成NotifyKey；
InputDispatcher仍会补FUNCTION/VIRTUAL、做Meta快捷键替换、repeat/long-press和policy fallback，
App最后才把keyCode+metaState解释成字符。
```

检查自己能否回答：

1. KCM 命中为什么可能让 KL 的 WAKE 消失？
2. replacement 后为何不能按新 keyCode 重新推断 policy flag？
3. usage-only 多键为何会在 scanCode=0 上冲突？
4. `mKeyDowns` 究竟锁住哪些字段、没锁住哪些字段？
5. KL `FUNCTION` 与 `AKEYCODE_FUNCTION` 如何不同？
6. `handlesKeyRepeat=1` 为什么可能使所有 raw repeat 的 count 都是 0？
7. DeviceReset 为什么不等于清空 Dispatcher 的全部键状态？
8. supported-key 为什么看不到 KCM-only/usage-only 映射？

下一章进入第 181 章 CursorInputMapper：继续追相对位移、鼠标按钮、滚轮、pointer capture、加速度、显示关联与 MotionEvent 坐标生成。
