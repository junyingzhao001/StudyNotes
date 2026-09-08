# 179 Android EventHub、InputDevice 与 Mapper 配置重建

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译、不要求连接 Android 设备  
> 前置章节：第 02、20、173、178 章

---

## 1. 本章只追一个问题：一条 evdev 记录凭什么变成某台 Android 设备的事件

第 178 章从 `InputDispatcher::notifyKey/notifyMotion()` 开始。再向下追，会遇到 Linux：

```c
struct input_event {
    struct timeval time;
    __u16 type;
    __u16 code;
    __s32 value;
};
```

例如：

```text
EV_ABS / ABS_MT_POSITION_X / 1234
```

它还不是 MotionEvent。Android 必须先回答：

- 这条记录来自哪个 fd、哪个 EventHub device；
- 该节点有哪些 key/abs/rel/switch/ff capability；
- 应加载哪份 `.idc/.kl/.kcm`；
- 它应建 Keyboard、Cursor、Touch 还是多个 Mapper；
- 若多个 evdev 节点属于一台逻辑设备，怎样合并；
- 热插拔、disable、配置刷新和 SYN_DROPPED 怎样划断旧状态；
- 哪个 generation 改变，何时通知 Java 与 Dispatcher。

本章主线是：

```text
/dev/input 节点
→ EventHub fd/identifier/classes
→ RawEvent(eventHubId)
→ 逻辑 InputDevice(framework id)
→ 子设备 Context + Mappers
→ QueuedInputListener
→ Dispatcher
```

Linux 节点、EventHub Device、逻辑 InputDevice 与 Mapper 是四层对象，不能只用“输入设备”一个词带过。

---

## 2. 源码地图与三种身份

核心文件：

```text
frameworks/native/services/inputflinger/reader/
├── EventHub.cpp
├── InputReader.cpp
├── InputDevice.cpp
├── include/EventHub.h
├── include/InputReader.h
├── include/InputDevice.h
└── mapper/
    ├── KeyboardInputMapper.cpp
    ├── CursorInputMapper.cpp
    ├── SingleTouchInputMapper.cpp
    ├── MultiTouchInputMapper.cpp
    ├── JoystickInputMapper.cpp
    ├── SwitchInputMapper.cpp
    ├── RotaryEncoderInputMapper.cpp
    ├── ExternalStylusInputMapper.cpp
    └── VibratorInputMapper.cpp

frameworks/native/libs/input/
├── InputDevice.cpp
└── Keyboard.cpp
```

三种身份分别解决不同问题：

| 身份 | 产生位置 | 作用 | 稳定性 |
|---|---|---|---|
| path，如 `event7` | kernel/devtmpfs 枚举 | 打开当前节点 | 重启、重插可变 |
| EventHub id | `openDeviceLocked()` | RawEvent 指向当前 fd | 普通节点每次 open 递增 |
| Framework deviceId | InputReader 建逻辑 InputDevice | App/Mapper/Dispatcher 共用 | 逻辑对象存活期稳定 |
| descriptor | EventHub 根据硬件身份求 SHA-1 | 尝试跨节点/重连识别性质 | 取决于 uniqueId 与连接顺序 |

表中实际有四行，因为 descriptor 不是数字 id，而是另一条“关联身份轴”。

保留值：

```text
VIRTUAL_KEYBOARD_ID = -1
BUILT_IN_KEYBOARD_ID = 0
END_RESERVED_ID      = 1
```

普通 Framework id 从 2 开始递增；不要把日志里的 EventHub id 与 Java `InputDevice.getId()` 机械等同。

---

## 3. EventHub 的等待集合：epoll、inotify、wake pipe 与 capability

构造 EventHub 时创建：

1. `epoll_create1(EPOLL_CLOEXEC)`；
2. `inotify_init()`，监视 `/dev/input` 的 CREATE/DELETE；
3. 普通 `pipe()`，再分别用 `fcntl(F_SETFL, O_NONBLOCK)` 设成非阻塞。

inotify read fd、wake read fd、真实 input fd 和已配对的 V4L fd 都进入同一个 epoll，事件标志为：

```cpp
EPOLLIN | EPOLLWAKEUP
```

因此 poll 可因四类原因返回：

```text
evdev raw event
inotify 节点变化
Reader 主动 wake
可选 touch video frame
```

`ensureProcessCanBlockSuspend()` 强制检查 effective `CAP_BLOCK_SUSPEND`，缺失即 fatal。没有该 capability，EPOLLWAKEUP 不能承担这里需要的唤醒保护。

其完成边界是：设备驱动读完最后事件后，epoll 继续持有 wakeup source，直到下一次对该 epoll 实例调用 `epoll_wait()`。正常情况下，EventHub 先把 RawEvent 返回给 InputReader，Reader 完成 Mapper 处理、设备列表通知和 listener flush，下一轮才重新 wait。

它不保护到：

- App 回 FINISHED；
- View callback；
- Surface present。

r48 的 epoll fd 有 CLOEXEC，但 `inotify_init()` 和 `pipe()` 没使用 CLOEXEC 版本；这是 fd 创建细节，不应从“epoll 自身 CLOEXEC”推导所有伴随 fd 都有同样标志。

---

## 4. 扫描与 open：名字只是起点，capability 才决定是否留下

首次 `mNeedToScanDevices=true`。`scanDevicesLocked()`：

- 遍历 `/dev/input`；
- 可选遍历 `/dev/v4l-touch*`；
- 确保 virtual keyboard 存在。

`scanDirLocked()` 只跳过 `.` 和 `..`，并不先筛 `event*`；目录中其他名字也会被尝试 open，失败或 class=0 后自然淘汰。

真实节点用：

```cpp
O_RDWR | O_CLOEXEC | O_NONBLOCK
```

打开，随后读取：

| ioctl | 字段 |
|---|---|
| `EVIOCGNAME` | name |
| `EVIOCGVERSION` | driver version |
| `EVIOCGID` | bus/vendor/product/version |
| `EVIOCGPHYS` | location |
| `EVIOCGUNIQ` | uniqueId |
| `EVIOCGBIT/EVIOCGPROP` | capability/property bitmask |

excluded name 在取得 name 后就检查；它是 Reader policy 给的排除表，不是 SELinux 拒绝状态。

分类与 keymap 完成后，若 `classes==0`，Device 被删除且 fd 随析构关闭，不注册 epoll，也不发送 DEVICE_ADDED。

留下的设备按顺序：

```text
匹配可选 video
→ 注册 input/video fd 到 epoll
→ configureFd
→ 加入 mDevices 与 mOpeningDevices
```

`configureFd()` 尝试关闭 kernel key repeat，并用 `EVIOCSCLOCKID` 请求 CLOCK_MONOTONIC；失败只记日志，设备仍可继续。

---

## 5. descriptor、nonce 与 composite：只有某些“相同”会被合并

descriptor 的 raw 输入不是 path，也不包含 bus/version。它从：

```text
:vendor:product:
+ 若 uniqueId 非空：uniqueId
+ 否则仅在冲突时：nonce
+ 若 vendor==0 且 product==0：name；name 空才用 location
```

拼接后取 SHA-1。SHA-1 在这里用于短而 opaque 的标识，不是安全认证。

关键分支：

```cpp
if (identifier.uniqueId.empty()) {
    while (已有相同 descriptor) {
        nonce++;
        重新生成 descriptor;
    }
}
```

所以 nonce 只对“没有 uniqueId”的当前连接节点强制唯一：

- 两个同型号、无序列号节点会得到不同 descriptor；
- 哪个实体拿 nonce=0/1 取决于当次 open 顺序，跨重启可互换；
- 有非空 uniqueId 时不做冲突消解，相同 uniqueId 的多个节点保留相同 descriptor。

InputReader 恰好按 descriptor 查找已有逻辑 InputDevice。因此 composite 合并通常发生在一块物理设备的多个 interface/node 暴露相同非空 uniqueId 时；无 uniqueId 的相似节点反而会被 nonce 拆开。

同样的机制也有误合并风险：两台设备若错误上报相同非空 uniqueId，就会进入同一逻辑 InputDevice。不能把“descriptor 相同”当成经过加密验证的物理同一性。

---

## 6. 配置文件、class 与 Mapper 是三次不同决策

配置文件先按名字查：

```text
Vendor_vvvv_Product_pppp_Version_xxxx
→ Vendor_vvvv_Product_pppp
→ canonical device name
```

只有 vendor 和 product 都非 0 才走前两级。每个名字再按根目录：

```text
/odm/usr/{idc,keylayout,keychars}/
→ /vendor/usr/...
→ $ANDROID_ROOT/usr/...
→ $ANDROID_DATA/system/devices/...
```

`.idc` 由 `PropertyMap::load()` 解析。路径找到但解析失败时保留路径日志，configuration 内容则按默认处理。

键盘映射分两层：

- `.kl`：Linux scan code/HID usage → Android keyCode/flags，也可映射 joystick axis；
- `.kcm`：keyCode + meta state → 字符、fallback/behavior 与 keyboard type。

KeyMap 可先使用 `.idc` 指定的名字，再按 identifier、Generic、Virtual 补缺；`.kl` 与 `.kcm` 是分别探测的，因此最终两份文件不一定来自同一个 basename。

EventHub class 主要来自 capability 组合：

| 代表条件 | class |
|---|---|
| keyboard/gamepad button 范围有 bit | KEYBOARD |
| BTN_MOUSE + REL_X + REL_Y | CURSOR |
| ABS_MT_POSITION_X/Y，且有 BTN_TOUCH 或非 gamepad | TOUCH + TOUCH_MT |
| BTN_TOUCH + ABS_X/Y | TOUCH |
| pressure/touch 但无 X/Y | EXTERNAL_STYLUS，并移除 KEYBOARD |
| gamepad button + 可解释 ABS axis | JOYSTICK |
| 任一 SW bit | SWITCH |
| FF_RUMBLE | VIBRATOR |
| `device.type=rotaryEncoder` | ROTARY_ENCODER |

keymap 又派生 ALPHAKEY、DPAD、GAMEPAD；`.idc` 还能影响 internal/external、mic 等属性。

InputDevice 最后按 class 建 Mapper。KEYBOARD/ALPHAKEY/DPAD/GAMEPAD 合并为一个 KeyboardInputMapper；TOUCH_MT 优先 MultiTouch，否则 SingleTouch。一个子设备可以同时拥有 Keyboard、Cursor、Vibrator 等多个 Mapper。

---

## 7. getEvents：内核时间被保留，但跨 fd 没有全局时间排序

`epoll_wait()` 一次最多返回 16 个 ready item。EventHub 按 epoll 给出的 item 顺序逐 fd read，并把每个 `input_event` 转成：

```text
RawEvent {
    when     = input_event.time
    deviceId = EventHub id（built-in 特例为 0）
    type/code/value
}
```

`when` 不是 read 时的 `now()`。它保留 evdev client buffer 入队时刻，正常为 monotonic：

```cpp
seconds_to_nanoseconds(tv_sec)
+ microseconds_to_nanoseconds(tv_usec)
```

但 EventHub 不把不同 fd 的记录按 `when` 归并排序。若 epoll 先返回 B，再返回 A，B 缓冲中的较新事件可能先于 A 的较旧事件进入 RawEvent 数组。

稳定的顺序只包括：

- 同一 fd 单次 read 中的 kernel buffer 顺序；
- EventHub 实际遍历并写入数组的顺序；
- InputReader 按数组顺序处理。

不能由 timestamp 推出跨设备全局 dispatch 顺序。

一批 epoll items 同时含设备数据与 inotify 时，EventHub 先把 inotify 标 pending，处理完其他 ready fd 后才 `readNotifyLocked()`。它尽量在 DELETE 前读走旧 fd 的残余事件，但遇到 ENODEV、HUP 或驱动行为时不保证一定得到尾部 UP。

---

## 8. 合成事件、opening/closing list 与缓冲区边界

EventHub 自定义三种高位 type：

```text
DEVICE_ADDED          0x10000000
DEVICE_REMOVED        0x20000000
FINISHED_DEVICE_SCAN  0x30000000
```

它们不是 Linux EV_*。时间取本轮 monotonic `now`，InputReader 只读取所需字段。

新 Device 已先进入 `mDevices`，再压到单链表 `mOpeningDevices`；关闭对象则离开 mDevices，进入 `mClosingDevices`，直到 DEVICE_REMOVED 被取走后才 delete。

链表都从 head 输出，因此一次扫描/open 多个节点时通知顺序不承诺等于 `readdir` 或 inotify 原始顺序。

`FINISHED_DEVICE_SCAN` 表示最近一轮 opening/closing 报告已走到扫描边界。InputReader 收到后：

```text
updateGlobalMetaStateLocked()
→ queue NotifyConfigurationChangedArgs
```

它不是 Java 屏幕 Configuration，也不表示所有 App 已观察到新设备。

r48 还有一个极端容量假设：InputReader 的 RawEvent buffer 是 256。closing/opening 循环在 `--capacity==0` 时只 break 当前 while，却没有在继续 scan 或写 FINISHED 前统一退出。设备数逼近缓冲上限时，控制流并不提供可靠的“自然跨下一批”保护；标准设备数量远低于此值，但审计时不能把 bufferSize 当成已严格防护的协议上限。

---

## 9. InputReader 一轮：两段持锁，中间 poll，最后先通知设备列表再 flush

`InputReader::loopOnce()` 的骨架：

```text
Reader lock:
    保存 old global generation
    取走并清 configuration changes
    refresh config / 计算 timeout

lock outside:
    EventHub.getEvents(timeout)

Reader lock:
    process RawEvent
    处理 mapper timeout
    若 global generation 改变，快照完整 InputDeviceInfo 列表

lock outside:
    policy.notifyInputDevicesChanged(list)
    QueuedInputListener.flush()
```

Mapper 在 Reader 锁内把 NotifyArgs 复制进 QueuedInputListener。最后锁外 flush，是因为 Dispatcher/WindowManager 可能反向查询 InputReader；持锁回调会形成真实死锁环。

不过不要扩大成“Reader 从不持锁调用 policy”。`refreshConfigurationLocked()` 会在 Reader 锁内调用 `mPolicy->getReaderConfiguration()`；锁外的是设备列表变更通知和 queued input flush。

顺序也值得记住：

> 同一 loop 中，policy 先收到新的完整 InputDeviceInfo 列表，随后 Dispatcher 才收到 Mapper 已排队的 DeviceReset/ConfigurationChanged/Key/Motion。

这是两个完成点，不是一个原子广播。

---

## 10. 配置请求与软件 timeout：wake 只负责叫醒，不保证准点

其他线程请求配置刷新：

```cpp
bool needWake = !mConfigurationChangesToRefresh;
mConfigurationChangesToRefresh |= changes;
if (needWake) {
    mEventHub->wake();
}
```

多次请求用 OR 合并；只有 pending 从 0 变非 0 时向非阻塞 pipe 写一字节。

若 pipe 已满，`write()` 返回 EAGAIN 会被忽略，因为旧 wake byte 已足以让 epoll 返回。其他写错误只记日志；changes bit 仍在，但若没有别的事件，Reader 可能不能立即醒来。

Mapper 也可设置 `mNextTimeout`，Reader 将它换算为 `epoll_wait` timeout。EventHub 明确写着：

> timeout is advisory only

系统已 suspend 时，不会仅为这个软件 timeout 唤醒。EPOLLWAKEUP 保护上一次 ready event 的处理窗口，却在下一次 epoll_wait 时释放；它不是精确 Alarm。

所以手势/按键 Mapper 的 timeout 到点只能解释成“线程下次被调度且 poll 返回后处理”，不是硬实时 deadline。

---

## 11. 逻辑 InputDevice：添加、合并、移除与 DeviceReset

收到 DEVICE_ADDED 后，InputReader：

```text
取 EventHub identifier
→ 在当前 mDevices 中找相同非空 descriptor
→ 命中：复用已有 shared InputDevice
→ 未命中：分配 Framework deviceId、新建 InputDevice
→ addEventHubDevice(eventHubId)
→ configure(changes=0)
→ reset
→ 把 eventHubId 映射到该 shared InputDevice
```

内部可能是：

```text
logical InputDevice id=7
├── eventHubId=14 → Context + KeyboardMapper
└── eventHubId=15 → Context + CursorMapper
```

RawEvent 仍按 eventHubId 找入口，只有该子设备的 mappers 收到它；公开设备列表则通过 `mDeviceToEventHubIdsMap` 对 shared InputDevice 去重。

移除一个子节点时：

1. 删除 eventHubId→shared device 映射；
2. 从逻辑对象移除该子设备；
3. 若仍有子设备，重新 `configure(changes=0)`；
4. 无论是否还有子设备，都 `reset(when)`。

`reset()` 的顺序是：

```text
所有剩余 Mapper.reset
→ 重算 global meta
→ queue NotifyDeviceResetArgs(logical deviceId)
```

Dispatcher 收 DeviceReset 后按逻辑 deviceId 合成取消。因此拔掉复合设备的一个 interface 也会重置整台逻辑设备，而不只是被移除节点对应的 Mapper。

---

## 12. 首次 configure 与 enable/disable：注释意图并未完全实现

`configure(changes=0)` 会重新汇总：

- 所有子设备 classes/configuration/controller number；
- isExternal、hasMic；
- keyboard overlay、alias、enabled；
- display port/viewport；
- 每个 Mapper 的完整参数、sources/ranges。

普通增量 configure 则复用现有 fd、Context 和 Mapper，只让关心 changes bit 的路径更新。

`setEnabled()` 的正常顺序：

```text
enable:  EventHub open/configure/register → Mapper reset
disable: Mapper reset → EventHub unregister/close
```

reset 在关闭前执行，是因为 MultiTouch 等 Mapper 可能需要查询仍有效的 fd。

但 r48 首次配置有一处源码/注释矛盾：

```cpp
if (!changes || (changes & CHANGE_ENABLED_STATE)) {
    setEnabled(enabled, when);
}
...
mapper.configure(...)
...
if (!changes) {
    setEnabled(enabled, when);
}
```

注释声称新设备应先完成 Mapper configure 再按 policy disable；可 `!changes` 使前一个分支已经生效。若 Framework id 预先位于 `disabledDevices`，fd 会在 Mapper.configure 前关闭，末尾调用只是 no-op。

显示端口缺 viewport 的路径稍有不同：首次 display 分支先记录 association，末尾 `setEnabled(true)` 又会因“有 port 无 viewport”在函数入口改成 false，所以它确实是在 Mapper configure 后禁用。

还有失败边界：

- composite enable 对每个子设备调用 `enableDevice()`，返回值被 lambda 忽略；
- 某个 open 失败仍会继续 reset 并 bump generation；
- EventHub open 成功但 epoll register 失败时没有关闭/回滚，device 可显示 enabled 却收不到事件；
- `isEnabled()` 只抽查第一个子设备，并假设 composite 全部同态。

因此 enabled 是软件期望/局部状态，不是“全部 fd 已可靠进入 epoll”的完成证明。

---

## 13. 增量重配与 MUST_REOPEN：对象保留和全量重建的分界

常见 changes：

```text
POINTER_SPEED
POINTER_GESTURE_ENABLEMENT
DISPLAY_INFO
SHOW_TOUCHES
KEYBOARD_LAYOUTS
DEVICE_ALIAS
TOUCH_AFFINE_TRANSFORMATION
EXTERNAL_STYLUS_PRESENCE
POINTER_CAPTURE
ENABLED_STATE
MUST_REOPEN
```

非 MUST_REOPEN：

```text
同一 EventHub fd
+ 同一 InputDevice/Context/Mapper
+ mapper.configure(changes)
```

它不会重新读取 capability，也不会重新决定 Mapper 类型。Keyboard layout overlay 有自己的增量路径，不要求重开 evdev。

MUST_REOPEN：

```text
requestReopenDevices + wake
→ 下一次 getEvents 看到 flag
→ closeAllDevicesLocked
→ mNeedToScanDevices=true
→ break，本次通常返回 0
→ 后续 getEvents 先报 DEVICE_REMOVED
→ scan/open
→ 报 DEVICE_ADDED
→ FINISHED_DEVICE_SCAN
```

不要记成严格“第二次只 removed、第三次才 added”。后续一次 `getEvents()` 只要缓冲仍有空间，就会在 closing list 后立即 scan 并继续输出 opening/finished；设备多时才可能跨批，极端 256 容量还有上一节的实现缺口。

普通动态 EventHub id 在 reopen 后继续递增，旧逻辑对象又已先 removed，因此新的 Framework deviceId 也会重新分配；只有 -1/0 保留设备走固定 id。descriptor 帮助识别设备性质，并不会让 InputReader 复活已删除对象或复用普通数字 id。

---

## 14. 两种 generation 与 SYN_DROPPED 的 composite 共享状态

r48 有两层 generation：

| 字段 | 含义 | 更新方式 |
|---|---|---|
| `InputReader::mGeneration` | 整份设备列表/任一设备信息变化的全局脉冲 | `bumpGenerationLocked()` |
| `InputDevice::mGeneration` | 某个逻辑设备公开描述的版本 | `device.bumpGeneration()` 取得新的全局值 |

每轮只比较 old/new 全局 generation；只要不同，就快照并通知完整设备列表。多次 bump 会合并成一次 callback。

两者不能混写：

- 添加子设备时 `addEventHubDevice()` 会 bump 单设备 generation，add 流程还会再 bump 全局；
- enabled、alias、keyboard overlay 等可 bump 单设备；
- 移除子设备时 `InputReader::removeDeviceLocked()` 只无条件 bump 全局，`InputDevice::removeEventHubDevice()` 本身不 bump 单设备；
- 若 composite 仍存活，其 sources/ranges 可因重新 configure 改变，但公开的 per-device generation 不一定随这次移除更新。

这是 r48 的版本边界，不能把 generation 统一解释成一个严格递增的“设备描述事务号”。

另一份容易忽略的共享状态是 `InputDevice::mDropUntilNextSync`。它属于整个逻辑 InputDevice，不属于 eventHubId。

收到：

```text
EV_SYN / SYN_DROPPED
```

后会 reset 整个逻辑设备并置 true。随后来自任一子节点的 raw event 都被丢弃；遇到任一子节点的下一笔 SYN_REPORT 时只清 flag，该 SYN_REPORT 本身也不交 Mapper。

于是 composite 设备中：

- A 节点 overrun 会短暂丢掉 B 节点事件；
- B 的 SYN_REPORT 也可能先替 A 清掉恢复门。

它是按逻辑设备恢复的保守近似，不是每个 evdev buffer 各自精确同步。

---

## 15. virtual keyboard、V4L 旁路、诊断与九组练习

virtual keyboard 在扫描末尾确保存在：

```text
fd = -1
id = VIRTUAL_KEYBOARD_ID(-1)
classes = KEYBOARD | ALPHAKEY | DPAD | VIRTUAL
```

它加载 keymap、发送 DEVICE_ADDED，但不进入 epoll。第 177/178 章的 inject 会把 Key/Motion deviceId 都改成 -1；这只是保留输入身份语义，不代表存在一个能产出 Motion raw event 的虚拟 evdev fd。

V4L touch video 则是旁路：

- 默认扫描 `/dev/v4l-touch*`，可用 `ro.input.video_enabled=false` 关闭；
- 以 device name 与 input node 配对；
- 已配对 video fd 才进入 epoll，未配对对象留在 holding queue；
- frame 被读取并排队，Touch Mapper 可消费为 `TouchVideoFrame`；
- evdev axes/SYN 仍是主触摸时序。

按 name 配对也是启发式：多台同名设备可能关联到先命中的 input Device。V4L 又不支持多客户端，EventHub 打开后会阻止调试工具同时直接读取。

诊断顺序：

```text
EventHub 是否 open 成功
→ identifier / descriptor / classes 是否合理
→ configuration / kl / kcm 实际路径
→ eventHubId 是否合入预期 logical deviceId
→ Mapper 集合、enabled、associated viewport
→ RawEvent / SYN_DROPPED
→ QueuedListener / Dispatcher
```

### 练习一：核对四类 epoll fd 与 capability

```bash
cd /Users/ninebot/androidSource
sed -n '250,335p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '1128,1215p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
```

找出 CAP_BLOCK_SUSPEND、inotify、wake pipe、input/video 注册点。

### 练习二：重建 getEvents 顺序

```bash
sed -n '847,1060p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
```

标出 reopen、closing、scan、opening、finished、epoll item 与 inotify 的先后。

### 练习三：手工分类一个节点

```bash
sed -n '1218,1510p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
```

假设有 BTN_MOUSE、REL_X/Y、字母键和 FF_RUMBLE，推导 Cursor、Keyboard、Vibrator Mappers；ALPHAKEY 还要看 keymap 是否映出 Q。

### 练习四：证明 nonce 不总消除 descriptor 重复

```bash
sed -n '680,740p' \
  frameworks/native/services/inputflinger/reader/EventHub.cpp
sed -n '269,288p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
```

比较 uniqueId 为空和非空时的冲突处理，再说明 composite 的形成条件。

### 练习五：追配置文件与混合 fallback

```bash
sed -n '35,130p' \
  frameworks/native/libs/input/InputDevice.cpp
sed -n '35,155p' \
  frameworks/native/libs/input/Keyboard.cpp
```

分别写出 filename、partition roots、identifier/Generic/Virtual 的优先级。

### 练习六：验证 Reader 的两个锁外动作

```bash
sed -n '80,180p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
sed -n '329,375p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
```

确认 getReaderConfiguration 在锁内，而 device-list notify 和 listener flush 在锁外。

### 练习七：复现首次 disabled 顺序矛盾

```bash
sed -n '40,90p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '245,330p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

令 `changes=0` 且 deviceId 已在 disabledDevices，按行推演 fd 何时关闭。

### 练习八：区分两种 generation

```bash
sed -n '185,290p' \
  frameworks/native/services/inputflinger/reader/InputReader.cpp
sed -n '125,215p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '450,465p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

比较 add subdevice 与 remove subdevice 对全局/单设备 generation 的影响。

### 练习九：证明 SYN_DROPPED 状态不是 per-subdevice

```bash
sed -n '327,370p' \
  frameworks/native/services/inputflinger/reader/InputDevice.cpp
sed -n '110,145p' \
  frameworks/native/services/inputflinger/reader/include/InputDevice.h
```

找出 `mDropUntilNextSync` 的对象归属，并推演两节点 composite 的交错事件。

---

## 16. 本章结论、自检与下一章

最终模型：

```text
发现
= inotify 告知名字变化
+ open/ioctl/config 建 identifier 与 classes
+ epoll 读取真实 fd

建模
= EventHubId 标记当前节点
+ descriptor 决定是否合入逻辑 InputDevice
+ class 决定每个子设备的 Mapper 集合

演进
= changes bit 增量 configure
+ MUST_REOPEN 的 remove→scan→add
+ global/device 两层 generation
+ reset/SYN_DROPPED 划断旧状态

交付
= Reader 锁内产生 NotifyArgs
+ 锁外先通知设备列表
+ 再 flush 到 Dispatcher
```

最重要的边界是：

- EPOLLWAKEUP 只覆盖到底层管线进入下一次 wait，不到 App FINISHED；
- RawEvent 保留 kernel timestamp，但多 fd 不按时间全局排序；
- nonce 只唯一化无 uniqueId 节点，复合合并通常依赖重复非空 uniqueId；
- `.kl` 与 `.kcm` 可从不同 fallback basename 补齐；
- class=0 的节点在 EventHub 就被淘汰；
- 首次显式 disabled 会在 Mapper.configure 前关 fd，和后段注释意图冲突；
- enabled 不证明 composite 每个 fd 都 open 且 epoll 注册成功；
- reopen 后普通数字 id 重新分配，descriptor 不复活旧对象；
- global generation 与 per-device generation 是两份账，子设备移除只保证前者 bump；
- SYN_DROPPED 的 drop flag 由整个逻辑 composite 共享。

自检时应能回答：

1. inotify、wake pipe、EPOLLWAKEUP 分别解决什么问题？
2. 为什么一个 `eventN` 不等于一个 Java InputDevice？
3. 没有 uniqueId 的两个相同节点为什么通常不会合并？
4. 同一子设备为何可同时有 Keyboard、Cursor、Vibrator Mapper？
5. 不同 fd 的事件为什么不能按 when 推导全局先后？
6. policy 的设备列表通知为什么早于 queued input flush？
7. 首次 disabled 的实际顺序与源码注释哪里矛盾？
8. MUST_REOPEN 后普通 deviceId 为什么不会保留？
9. composite 子节点移除时哪层 generation 一定变化？
10. A 的 SYN_DROPPED 为什么会影响 B 的 raw event？

下一章深入 **KeyboardInputMapper、`.kl/.kcm`、meta state、key repeat 与 fallback**，逐笔推演 Linux scan code 怎样成为 Android KeyEvent。
