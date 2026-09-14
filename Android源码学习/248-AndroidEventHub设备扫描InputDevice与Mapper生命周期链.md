# 248 Android EventHub设备扫描、InputDevice与Mapper生命周期链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源，不编译、不运行 AOSP

## 1. 主问题：一个event节点何时才成为上层可见且可恢复的输入设备

`/dev/input/event*` 已出现却不在 `dumpsys input`，一块复合硬件被列成两个设备，或者拔掉触摸屏后 App 仍像有手指按住，都不能只用“驱动有没有上报”解释。内核节点要经过四层变换：路径先成为 EventHub 的节点级 `Device`，synthetic 增加事件再让 InputReader 建立逻辑 `InputDevice`，每个子节点获得自己的 `InputDeviceContext` 与 Mapper，最后完整 `InputDeviceInfo` 快照才进入 Java。

本章只回答一条主问题：**节点如何被识别、聚合、配置、公开，并在丢包、禁用、拔出或全量重开时恢复上下游协议？** 先把七个常被混用的完成点分开：

| 完成点 | 已经能证明 | 仍然不能证明 |
|---|---|---|
| `open()` 成功 | 当前路径得到一个非阻塞读写 fd | Android 已识别能力、fd 已进 epoll |
| `openDeviceLocked()` 返回 `OK` | 节点已分类、输入 fd 已注册、`Device` 已进入 `mDevices` 与 opening 链 | Reader 已处理 `DEVICE_ADDED` |
| Reader 处理 `DEVICE_ADDED` | 逻辑设备已创建或合并，Mapper 已配置并 reset，节点映射已提交 | Java listener 已收到列表、Dispatcher 已处理 reset |
| Reader 发现 generation 改变 | 本轮锁内已生成一次完整设备快照 | DisplayThread 已交付 listener |
| IMS `notifyInputDevicesChanged()` 返回 | 最新数组已保存，且至多有一条交付消息 pending | 中间每份快照都会逐份回调 |
| `notifyDeviceReset()` 入 Dispatcher | `DeviceResetEntry` 已进入受锁保护的入站账 | App 已收到 CANCEL，所有队列已清空 |
| Reader 处理 `DEVICE_REMOVED` | eventHubId 映射已删除，幸存逻辑设备已重配并整体 reset | 设备列表回调或 App 收尾已经完成 |

下一章才深入 ABS/MT slot、校准和坐标投影；这里把 Mapper 当作节点能力到 Android 事件语义的边界，不展开每种触摸算法。

### 练习 1：给同一个节点标出四层对象和三个不可越级的完成点

设节点 E 能被识别为多点触摸，`openDeviceLocked()` 已返回 `OK`，但 EventHub 尚未输出 synthetic 事件。问此刻 EventHub `Device`、Reader `InputDevice`、Mapper、Java `InputDevice` 哪些已经存在；什么时候才能分别证明 Reader 映射建立、Java 已保存新快照、App listener 已收到变化？

唯一答案是：此刻只有 EventHub `Device`；Reader 处理 `DEVICE_ADDED` 后才有逻辑对象、Context 与 Mapper；Reader generation 改变并锁外调用 Policy，只证明 JNI/IMS 入口会同步保存数组；直到 DisplayThread 执行 `MSG_DELIVER_INPUT_DEVICES_CHANGED` 并调用 listener，才有最后一个完成点。任一前置返回都不能越级替代后者。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mNeedToScanDevices(true),' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mInputWd = inotify_add_watch(mINotifyFd, DEVICE_PATH, IN_DELETE | IN_CREATE);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'result = pipe(wakeFds);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mLock.unlock(); // release lock before poll' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'readNotifyLocked();' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'event->type = DEVICE_ADDED;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'void InputReader::addDeviceLocked(nsecs_t when, int32_t eventHubId) {' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'device->configure(when, &mConfig, 0);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'device->reset(when);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mPolicy->notifyInputDevicesChanged(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mQueuedListener->flush();' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mHandler.obtainMessage(MSG_DELIVER_INPUT_DEVICES_CHANGED,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'deliverInputDevicesChanged((InputDevice[])msg.obj);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
```

## 2. EventHub先建立发现与等待基础设施，第一次getEvents才主动扫描

EventHub 构造时先验证进程具备 `CAP_BLOCK_SUSPEND`，再创建 epoll、给 `/dev/input` 安装 `IN_CREATE | IN_DELETE` 的 inotify watch，并把 inotify fd 与非阻塞 wake pipe 读端注册为 `EPOLLIN | EPOLLWAKEUP`。视频输入默认还会监视整个 `/dev`，但只接受名字以 `v4l-touch` 开头的节点；`ro.input.video_enabled=false` 才关闭这条扫描。

watch 负责未来变化，不负责枚举已有文件，所以 `mNeedToScanDevices` 初值是 true。Reader 第一次调用 `getEvents()` 时，EventHub 在自己的 `mLock` 下执行 `scanDevicesLocked()`；主目录的每个非 `.`/`..` 项都会尝试 `openDeviceLocked()`，并非先靠文件名筛成 `event*`。真正的 `epoll_wait()` 前才释放 EventHub 锁，返回后重新取得。

热插入由 inotify `IN_CREATE → openDeviceLocked()`，目录删除由 `IN_DELETE → closeDeviceByPathLocked()`；此外设备 fd 读到 0、得到 `ENODEV` 或收到 `EPOLLHUP` 也能进入 close。若 inotify 与设备 fd 同属一批 epoll 结果，代码先处理其余 pending item，最后才读 inotify，目的是尽量保留同批已经就绪的数据；这不是“目录删除前一定把内核所有残留读到 EAGAIN”的更强屏障。

## 3. openDevice是一串可失败的筛选，不是一条全有或全无事务

节点先以 `O_RDWR | O_CLOEXEC | O_NONBLOCK` 打开。`EVIOCGNAME` 失败只留下空 name 并继续；按 name 命中 excluded list 会立即关闭。驱动版本 `EVIOCGVERSION` 或基本身份 `EVIOCGID` 失败则终止本节点，physical location 与 unique id 获取失败却只是缺字段。EventHub 随后装载 `.idc`，读取 key/abs/rel/switch/LED/force-feedback/property bitmask，再结合 key map 推导可累加的 classes。

`classes == 0` 的 fd 会被删除；识别成功也还要先把输入 fd 加入 epoll。只有这些门都通过，`configureFd()` 才尝试关闭内核 key repeat、切换 `CLOCK_MONOTONIC`，最后 `addDeviceLocked()` 提交节点。两项配置 ioctl 失败只记日志，不撤销成功打开。

r48 的失败边界并不完全回滚：`mNextDeviceId++` 在分类前已经消费 id；built-in keyboard 标记与 controller number 也在 epoll 注册前确定，而注册失败分支只是 `delete device`。因此一次失败可以留下整数空洞，源码也没有在该分支显式归还这两项先前选择。日志中的某个 id 缺号不是设备一定曾向 Reader 可见。

### 练习 2：四种open结果分别停在哪个完成点

设四个节点都能 `open()`：A 的 name ioctl 失败，但 `EVIOCGID` 成功且能力可识别；B 的 `EVIOCGID` 失败；C 身份读取成功但最终 classes 为 0；D 已被判为 gamepad 并分到 controller number，随后输入 fd 的 epoll 注册失败。问谁能进入 opening 链，哪些失败仍会消耗动态 id？

唯一答案是：只有 A 可继续并在后续门成功时进入 opening；B、C、D 都不会。四者都在 `mNextDeviceId++` 之前还是之后要逐段判断：B 在分配 `Device` 前失败，不耗 id；A、C、D 已执行分配，所以都会耗 id。D 的 controller number 已在注册前取得，而该失败分支没有显式释放；不能把 `open()` 成功或 id 出现过解释成提交完成。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'int fd = open(devicePath, O_RDWR | O_CLOEXEC | O_NONBLOCK);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (ioctl(fd, EVIOCGNAME(sizeof(buffer) - 1), &buffer) < 1) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (ioctl(fd, EVIOCGVERSION, &driverVersion)) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (ioctl(fd, EVIOCGID, &inputId)) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'int32_t deviceId = mNextDeviceId++;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mBuiltInKeyboardId = device->id;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->controllerNumber = getNextControllerNumberLocked(device);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (device->classes == 0) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (registerDeviceForEpollLocked(device) != OK) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'configureFd(device);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'addDeviceLocked(device);' frameworks/native/services/inputflinger/reader/EventHub.cpp
```

## 4. descriptor期望跨连接稳定，nonce却只解决当前无uniqueId冲突

`InputDeviceIdentifier` 的 name、bus/vendor/product/version、location 与 uniqueId 都来自 evdev，但 descriptor 没有把全部字段串进去。原始串总含 vendor/product；uniqueId 非空时追加它；没有 uniqueId 时，nonce 非零才追加 nonce。只有 vendor 与 product 同时为 0，才再选 name，若 name 为空才选 location。bus 与 version 不参与这版 hash。

`assignDescriptorLocked()` 先用 nonce 0 计算 SHA-1。仅当 uniqueId 为空时，它才用当前 EventHub 连接集合查冲突并不断增加 nonce；有 uniqueId 的两个节点即使撞出相同 descriptor，也不会被这层拆开。InputReader 后面正是按“两个 descriptor 都非空且完全相等”复用逻辑 `InputDevice`，所以这个键既允许一块复合硬件的多个节点合并，也会合并固件错误地复用同一 uniqueId 的不同物理设备。它是实现使用的聚合键，不是物理同机身证明或安全身份。

源码注释希望 descriptor 跨重启、重连稳定，但 nonce 会受当前连接集合与发现次序影响。反过来，同一 uniqueId 若 vendor/product 变化也会改变 hash。诊断时应先还原 raw descriptor 输入，而不是看到 SHA-1 形式就假定它来自不可变序列号。

### 练习 3：同型号无序列号与重复序列号会得到相反的聚合结果

假定四个节点 vendor/product 相同且其他参与字段相同。A、B 都没有 uniqueId，按 A 后 B 打开；C、D 都报告同一个非空 uniqueId，且另起一个空连接集合按 C 后 D 打开。问两组最终 descriptor 与 Reader 逻辑设备数量的关系。

唯一答案是：A 先取得 nonce 0 的 descriptor，B 因冲突递增 nonce，最终二者不同，Reader 建两个逻辑设备；C、D 都直接采用包含相同 uniqueId 的原始串，不运行冲突循环，descriptor 相同，Reader 会把两个 eventHubId 聚合到一个逻辑设备。name 相同本身对 vendor/product 非零的这两组都不增加证明力。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'rawDescriptor += StringPrintf(":%04x:%04x:", identifier.vendor, identifier.product);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (!identifier.uniqueId.empty()) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F '} else if (identifier.nonce != 0) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (identifier.vendor == 0 && identifier.product == 0) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'identifier.descriptor = sha1(rawDescriptor);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (identifier.uniqueId.empty()) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'while (getDeviceByDescriptorLocked(identifier.descriptor) != nullptr) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'devicePair.second->getDescriptor() == identifier.descriptor;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mIdentifier(identifier),' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'void InputDevice::removeEventHubDevice(int32_t eventHubId) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

## 5. capability先形成可叠加class，class再决定Mapper而不是一对一改名

EventHub 的 bitmask 起初清零，能力 ioctl 返回值在这段分类代码里没有逐项变成 open 失败；读不到的位自然不会贡献 class。`.idc` 又在分类前装载，所以底层 bit 与配置共同决定结果：

| 条件摘要 | 追加或调整的 class |
|---|---|
| 普通键范围或 gamepad/joystick button 范围 | `KEYBOARD` |
| `BTN_MOUSE + REL_X + REL_Y` | `CURSOR` |
| `.idc` 的 `device.type=rotaryEncoder` | `ROTARY_ENCODER` |
| `ABS_MT_POSITION_X/Y` 且有 `BTN_TOUCH` 或没有 gamepad buttons | `TOUCH + TOUCH_MT` |
| 无 MT，但有 `BTN_TOUCH + ABS_X + ABS_Y` | `TOUCH` |
| pressure/touch 存在而 X/Y 都不存在 | `EXTERNAL_STYLUS`，并撤销 `KEYBOARD` |
| gamepad buttons 加至少一个可归 joystick 的绝对轴 | `JOYSTICK` |
| 任意 switch bit / `FF_RUMBLE` | `SWITCH` / `VIBRATOR` |

触摸节点加载 virtual-key map 成功还会补 `KEYBOARD`；key map 随后可再补 `ALPHAKEY`、完整五向 `DPAD` 与 `GAMEPAD`。`.idc` 提供设备配置与校准，`.kl` 映射 scan code/axis，`.kcm` 描述字符与 meta 组合，三者不能互换。最后才追加 MIC、EXTERNAL 等属性位。class 是节点能力集合，并非单选枚举；Android source 要等 Mapper configure 后才汇总，一个 Mapper 也可产生多个 source。

### 练习 4：从一组能力推导class与Mapper

设节点同时报告 `BTN_MOUSE、REL_X、REL_Y、BTN_TOUCH、ABS_MT_POSITION_X/Y、FF_RUMBLE`，没有 gamepad buttons，virtual-key map 又成功加载。问 EventHub 至少得到哪些主 class，Reader 建哪些 Mapper，是否还会建 SingleTouchInputMapper？

唯一答案是：至少有 `CURSOR、TOUCH、TOUCH_MT、VIBRATOR、KEYBOARD`；Reader 依创建顺序加入 Vibrator、Keyboard、Cursor、MultiTouch Mapper。`TOUCH_MT` 分支优先，所以不会再建 SingleTouch。能否追加 ALPHA/DPAD/GAMEPAD 还取决于 key map 映射，题目条件不足以断言。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_KEYBOARD;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_CURSOR;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_ROTARY_ENCODER;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_TOUCH | INPUT_DEVICE_CLASS_TOUCH_MT;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_EXTERNAL_STYLUS;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes = assumedClasses;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_SWITCH;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_VIBRATOR;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_ALPHAKEY;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_DPAD;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->classes |= INPUT_DEVICE_CLASS_GAMEPAD;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (classes & INPUT_DEVICE_CLASS_TOUCH_MT) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F '} else if (classes & INPUT_DEVICE_CLASS_TOUCH) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
```

## 6. EventHub Device拥有节点fd；禁用只关门，不重新识别身份

一个 EventHub `Device` 持 path、当前 fd、identifier、capability bitmask、classes、`.idc`、key map、controller number 与可选 video device。它通常对应一个 evdev 节点，不等于 Reader 或 Java 的同名类。V4L touch 节点又是另一种资源：源码按 name 给第一个匹配的 evdev `Device` 配对，而不是按 descriptor；video fd 注册失败只记日志，输入 fd 注册结果仍可为成功。已经存在的 evdev 后来单独挂上或失去 video 节点，也不会因此进入 opening/closing、生成 Reader 设备列表变化；video 是这个节点对象的附属资源。

普通 EventHub 节点 id 从 1 递增。Virtual Keyboard 固定为 -1、fd 为 -1；被选中的 built-in keyboard 在 EventHub 内仍有正 id，但向 Reader 查询和普通 RawEvent/`DEVICE_ADDED` 时映射为 0。Reader 自己的 `mNextInputDeviceId` 从保留边界 1 开始、先自增再返回，所以普通新逻辑设备首个动态 id 是 2。四者中的 path、eventHubId、logical deviceId、descriptor 没有任何两项可以普遍互换。

`disableDevice()` 从 epoll 注销并关闭 fd，却保留 EventHub `Device`、identifier 与已读能力；`enableDevice()` 只是重新打开同一路径、再做 fd 配置并注册 epoll，不重跑身份/capability/key-map 分类。若路径已被复用，这条 API 本身也不会证明还是旧硬件。Reader 的 composite enable/disable 还假定所有子节点状态一致，逐节点调用的 status 却没有在 `InputDevice::setEnabled()` 中汇总成事务结果。

## 7. opening与closing链把对象提交转换成synthetic扫描批次

`addDeviceLocked()` 先把节点放入 EventHub `mDevices`，再头插 `mOpeningDevices`。`getEvents()` 每轮顶部先输出 closing，按需扫描，再输出 opening，最后用一个 `FINISHED_DEVICE_SCAN` 收束本批 synthetic 变化；之后才消费普通 pending fd。synthetic type 大于等于 `FIRST_SYNTHETIC_EVENT`，不是内核 `input_event`，也不会直接进入 Mapper。

`FINISHED_DEVICE_SCAN` 表示 EventHub 已报告当前这批增删，并且至少会在初始扫描后出现一次；即使目录扫描本身报错，外层仍会设置发送标志，所以它也不是“所有节点都扫描/open 成功”的证明。Reader 用它更新全局 meta 并排队 ConfigurationChanged。它不证明 Java 设备 listener、Dispatcher、App 或显示完成。opening/closing 都是头插头取，目录遍历本身也无稳定顺序，所以完整快照和 id 才是上层依据，数组位置不是设备永久排名。

容量足够时，若扫描顺序明确为 A 再 B，扫描结束后才创建 Virtual Keyboard，opening 输出会是 Virtual、B、A、FINISHED。这个逆序只是当前链表实现结果，不是可依赖的外部 API。还要注意 r48 在 synthetic while 内是写入后才把 capacity 减到 0、只跳出当前链表循环，后续 FINISHED 写入前没有新的容量门；所以“buffer 满了会安全分页”也不是这段代码提供的保证，本节所有顺序推演都显式要求容量足够。

### 练习 5：手算初始扫描的synthetic顺序

设 EventHub 初始无设备，buffer 足够，目录枚举被题目固定为先 A 后 B，两者都成功识别，且系统随后创建 Virtual Keyboard；没有 closing 或旧 pending fd。问本次 `getEvents()` 的 synthetic 次序，以及 Reader 在第几个标记处调用 `handleConfigurationChangedLocked()`。

唯一答案是：每次 add 都头插，所以顺序为 `DEVICE_ADDED(-1 Virtual) → DEVICE_ADDED(B) → DEVICE_ADDED(A) → FINISHED_DEVICE_SCAN`；Reader 对前三项分别 add，遇到最后一项才执行 configuration-changed 处理。这个答案依赖题设的固定扫描顺序和足够容量，不能推广成真实目录排序契约。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mNextDeviceId(1),' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'new Device(-1, ReservedInputDeviceId::VIRTUAL_KEYBOARD_ID, "<virtual>", identifier);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'status_t EventHub::enableDevice(int32_t deviceId) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'status_t EventHub::disableDevice(int32_t deviceId) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->videoDevice = std::move(videoDevice);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->videoDevice = nullptr;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mNextInputDeviceId(END_RESERVED_ID),' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'return ++mNextInputDeviceId;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'device->next = mOpeningDevices;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'while (mClosingDevices) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'while (mOpeningDevices != nullptr) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (--capacity == 0) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'if (mNeedToSendFinishedDeviceScan) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'event->type = FINISHED_DEVICE_SCAN;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'case EventHubInterface::FINISHED_DEVICE_SCAN:' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'handleConfigurationChangedLocked(rawEvent->when);' frameworks/native/services/inputflinger/reader/InputReader.cpp
```

## 8. Reader用两张映射把多个节点聚合成一个逻辑InputDevice

Reader 的正向表是 `eventHubId → shared_ptr<InputDevice>`，反向表是 `shared_ptr<InputDevice> → [eventHubId...]`。正向表让一笔 RawEvent 找到逻辑拥有者，反向表让完整快照每个逻辑设备只枚举一次；`shared_ptr` 保证移除一个子节点不会销毁仍被其他 eventHubId 引用的 composite 对象。

处理 `DEVICE_ADDED` 时，`createDeviceLocked()` 先在已有正向表中找相同非空 descriptor。有则复用；无则按保留 id 或 `nextInputDeviceIdLocked()` 创建。随后 `addEventHubDevice()` 建 Context/Mapper 并 bump per-device generation，`configure(0)`、`reset()`，最后才把新 eventHubId 写进两张 Reader 映射并 bump 全局 generation。由此可见，reset 通知可以先进入 `QueuedInputListener`，但在本轮 flush 前映射已经提交。

逻辑 `InputDevice` 的 `mIdentifier` 来自第一次构造，不会在添加子节点或移除原始子节点时替换。于是 descriptor 相同但 name/location 等字段不同的 composite，公开身份取首个创建者；首子节点拔掉而其他子节点幸存时，identifier 仍可保留已拔节点的非 descriptor 字段。聚合只证明键相同，不会自动融合全部 identifier 字段。

## 9. 每个子节点有独立Context与Mapper向量，事件不会广播给整个composite

`InputDeviceContext` 同时记 logical deviceId 与自己的 eventHubId，Mapper 通过它把轴、key map、enable/vibrate 等查询限制到对应节点。一个子节点的 Mapper 向量按源码顺序加入 Switch、Rotary、Vibrator、Keyboard、Cursor、MultiTouch 或 SingleTouch、Joystick、ExternalStylus；Vibrator 即使不产生普通输入 source，也会令 Mapper 数非零。Reader 所谓 ignored 是 Mapper 总数为 0，与 EventHub 更早的 `classes == 0` 不是同一门。

InputReader 先把连续且 eventHubId 相同的普通 RawEvent 分为一批，逻辑设备再按“每个 RawEvent → 该子节点 Mapper 向量”处理。它不会把 event3 的坐标发给同一 composite 的 event4 Mapper；向量内顺序稳定，跨节点容器却是 `unordered_map`，所以 configure/reset 等遍历全部子节点时没有同样的跨节点顺序契约。`mConfiguration.addAll()` 遇到子节点同名 key 时，覆盖者也会受这次无序遍历影响。

### 练习 6：三笔跨子节点RawEvent到底由谁看到

设 composite 有子节点 A，Mapper 向量依次为 Keyboard、Joystick；子节点 B 的向量依次为 Vibrator、MultiTouch。RawEvent 顺序是 A 的 key、B 的 ABS、A 的 SYN_REPORT。问处理顺序与观察者，能否先把三笔统一交给 Keyboard 再交给其他 Mapper？

唯一答案是：Reader 按 A、B、A 三个连续批次依次调用；第一笔只按 Keyboard→Joystick，第二笔只按 Vibrator→MultiTouch，第三笔再按 Keyboard→Joystick。不能按 Mapper 类型重排，否则跨类型副作用会越过原始事件顺序；B 的 Mapper 也绝不会因为共享 logical deviceId 而看到 A 的事件。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mDevices.emplace(eventHubId, device);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mDeviceToEventHubIdsMap.emplace(device, ids);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'device->addEventHubDevice(eventHubId);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'std::make_unique<KeyboardInputMapper>(*contextPtr, keyboardSource, keyboardType)' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mappers.push_back(std::make_unique<MultiTouchInputMapper>(*contextPtr));' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mappers.push_back(std::make_unique<JoystickInputMapper>(*contextPtr));' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'for (const RawEvent* rawEvent = rawEvents; count != 0; rawEvent++) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'for_each_mapper_in_subdevice(rawEvent->deviceId, [rawEvent](InputMapper& mapper) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mapper.process(rawEvent);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'std::unordered_map<int32_t, DevicePair> mDevices;' frameworks/native/services/inputflinger/reader/include/InputDevice.h
grep -n -F 'mConfiguration.addAll(&configuration);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'index = VectorImpl::replaceAt(item, index);' system/core/libutils/VectorImpl.cpp
```

## 10. configure重建并集与source，但初次禁用顺序存在两条不同路径

每次 `InputDevice::configure()` 都先清 `mSources/mClasses/mControllerNumber`，从全部 Context 重新求 class 并集，再得到 external/mic。多个子节点各有非零 controller number 时，源码只告警并在无序遍历中继续覆盖，没有定义稳定赢家。`changes == 0` 还清空逻辑 PropertyMap，把子节点配置逐个 `addAll()`；非 Virtual 设备按逻辑首 identifier 查询 keyboard overlay 与 alias，变化可 bump generation。Mapper configure 最后把各自 source OR 成公开 `mSources`。

源码多处把 `!changes` 注释成 first time，但 Reader 在 partial composite removal 后也对幸存对象调用 `configure(..., 0)`。因此配置合并、参数初始化与各 Mapper 的零 changes 分支都可能在同一 logical deviceId 上重跑；这里把 0 理解为“全量/重建式配置”比理解成只执行一次更准确。

启停按 logical deviceId 应用于所有子节点。`setEnabled(true)` 的顺序是逐节点 reopen → 整体 reset → bump；`setEnabled(false)` 是整体 reset → 逐节点 close → bump，因为某些 reset 要在 fd 尚可查询时读取驱动状态。display port 有声明却找不到 viewport 时，`setEnabled(true)` 自己会改成 false。

这里不能照抄源码末尾注释为“所有首次禁用都等 Mapper 配完”。r48 前面的 enabled-state 分支写的是 `!changes || CHANGE_ENABLED_STATE`：若新设备已经列在 `disabledDevices`，第一次 `configure(0)` 会在 Mapper configure 前调用 `setEnabled(false)`。只有初次由缺失 viewport 导致的关闭，才借 display 分支的 `if (changes)` 延后，并在 Mapper configure 后的最后一次 `setEnabled()` 生效。再加上逐子节点 enable/disable status 被忽略，composite 的“一致启停”是实现假设而非失败时的强事务。增加或移除 external stylus 还会触发 `CHANGE_EXTERNAL_STYLUS_PRESENCE`，使当前其他设备的触摸 Mapper 一并重配；它不是只改新旧笔自己的列表项。

### 练习 7：比较初次显式禁用与缺viewport禁用的真实顺序

设 P 是新设备且 logical id 已在 `disabledDevices`；Q 不在该集合，但 location 映射到不存在的 display viewport。两者初始 fd 都开启，均执行 `configure(0)`。问谁在 Mapper configure 前关闭，谁在后关闭？

唯一答案是：P 在早期 enabled-state 分支调用 `setEnabled(false)`，先整体 reset 再关闭 fd，之后才配置 Mapper；末尾同值调用不再改变状态。Q 的早期调用保持 enabled，display 分支只记录缺 viewport 而因 `changes==0` 不立即关闭；Mapper 配完后，末尾 `setEnabled(true)` 被内部端口检查改成 false，才 reset 并关闭。两条路径证明原文不能给出统一的“首次最后才 disable”保证。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'mSources = 0;' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mClasses = 0;' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'composite device contains multiple unique' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mControllerNumber = controllerNumber;' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mConfiguration.clear();' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mConfiguration.addAll(&configuration);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'if (!changes || (changes & InputReaderConfiguration::CHANGE_ENABLED_STATE)) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'bool enabled = it == config->disabledDevices.end();' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'if (enabled && mAssociatedDisplayPort && !mAssociatedViewport) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'for_each_subdevice([](auto& context) { context.enableDevice(); });' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'for_each_subdevice([](auto& context) { context.disableDevice(); });' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mapper.configure(when, config, changes);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mSources |= mapper.getSources();' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'setEnabled(config->disabledDevices.find(mId) == config->disabledDevices.end(), when);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'notifyExternalStylusPresenceChanged();' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'refreshConfigurationLocked(InputReaderConfiguration::CHANGE_EXTERNAL_STYLUS_PRESENCE);' frameworks/native/services/inputflinger/reader/InputReader.cpp
```

## 11. reset修复状态协议；它既不等于remove，也不保证只产生一笔通知

`InputDevice::reset()` 先对当前所有 Mapper 调 `reset(when)`，再调用 Reader 的全局 meta 重算，最后构造 `NotifyDeviceResetArgs(logicalDeviceId)` 放入 `QueuedInputListener`。这一步保留对象、id 与节点集合；启停、配置变化、SYN_DROPPED、增加/移除子节点都可能触发它，扫描并不是 reset 的隐含动作。新增逻辑设备还有一个顺序细节：reset 发生在两张 Reader 映射提交前，所以这次 meta 重算尚枚举不到全新的对象；随后 `FINISHED_DEVICE_SCAN` 的 configuration-changed 处理会再次重算。

“一项变化只对应一个 DeviceReset”同样不成立。新设备的 `addDeviceLocked()` 总会在 configure 后 reset；configure 内的 `setEnabled()` 可先 reset，Touch/Cursor Mapper 的某些动态配置也能直接排队 reset。`QueuedInputListener` 只保证稍后按队列 flush，并不去重这些 marker。

Dispatcher 的 `notifyDeviceReset()` 只是入队；轮到 `dispatchDeviceResetLocked()` 时，它按 logical deviceId 请求所有 Connection 从各自 `InputState` 合成 `CANCEL_ALL_EVENTS`。只有实际存在匹配状态的 Connection 才产生事件，且这不会自动删除更早或更晚的所有 inbound 工作。reset 的完成点是协议恢复请求被推进，不是 App 已处理、更不是设备已从列表消失。

## 12. global generation触发完整快照，per-device generation决定客户端是否重载

Reader `mGeneration` 是 Reader-wide 的设备/config change sequence；每个 `InputDevice::mGeneration` 是写进 `InputDeviceInfo` 的该设备版本。二者都取自 `bumpGenerationLocked()` 的单调序列，却不要求相等，也不能用差值计算“发生了几件事”。新建逻辑对象、添加子节点、启停、overlay/alias/Mapper 配置和最终加入/移除 Reader 表都可能各自 bump；counter 改变会触发重建快照，但不反向保证快照每个字段都变了。

每轮 `loopOnce()` 先保存 oldGeneration，处理整批 RawEvent、timeout 与配置；只要最终值不同，就在 Reader 锁内通过反向表生成一次完整快照，跳过没有 Mapper 的设备。反向表也是无序容器，输出顺序不是稳定 API。随后 Reader 解锁，同步进入 native Policy/JNI/IMS 保存数组，再在这次调用返回后 flush reset/configuration 等 listener 事件。

IMS 的第二级合并更强：第一份变化把旧数组装进一条 DisplayThread message，pending 期间后续调用只替换 `mInputDevices`。Handler 最终用第一次的 old 与届时最新数组做键盘布局处理，并向 listener 发送最新 `deviceId,generation` 对；中间数组可以完全不可见。

partial composite removal 还有一个 r48 缺口：Reader 一定 bump global generation，但 `removeEventHubDevice()` 只 erase，没有直接 bump 幸存逻辑设备 generation。后续 `configure(0)` 可能因别的变化 bump，也可能不 bump；因此完整快照虽变了，客户端只比较 id/generation 时仍可能不重载该设备已经改变的 sources/ranges。全局集合变更与单设备版本推进不能互相替代。

### 练习 8：两次Reader快照为何只交付最后一份

设 IMS 当前数组为 S0、没有 pending message。Reader 的两轮变化先后同步调用 IMS，传入 S1、S2；DisplayThread 在两次调用之后才运行。问 message 携带哪个 old，Handler 读取哪个 current，listener 能否观察 S1？

唯一答案是：第一次调用把 S0 放进 message 并置 pending，再保存 S1；第二次只把当前数组覆盖为 S2。Handler 得到 old=S0、锁内读取 current=S2，发送 S2 的 id/generation 对，S1 不形成独立 listener 回调。Reader 每轮一次与 IMS pending 期一次是两级不同的合并。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'oldGeneration = mGeneration;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'if (oldGeneration != mGeneration) {' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'getInputDevicesLocked(inputDevices);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'return ++mGeneration;' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'mGeneration = mContext->bumpGeneration();' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mDevices.erase(eventHubId);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'for (const auto& [device, eventHubIds] : mDeviceToEventHubIdsMap) {' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'if (!device->isIgnored()) {' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'if (!mInputDevicesChangedPending) {' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'mInputDevices = inputDevices;' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'deviceIdAndGeneration[i * 2 + 1] = inputDevice.getGeneration();' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'if (device.getGeneration() != generation) {' frameworks/base/core/java/android/hardware/input/InputManager.java
grep -n -F 'mInputDevices.setValueAt(index, null);' frameworks/base/core/java/android/hardware/input/InputManager.java
```

## 13. SYN_REPORT收束硬件帧；SYN_DROPPED使增量状态失去可信前缀

正常 RawEvent 只进入对应子节点 Mapper。多数 accumulator 到 `EV_SYN/SYN_REPORT` 才把此前的按键或轴变化收束成 NotifyArgs，所以它是 evdev 报告边界，不是 Dispatcher 或 App 完成点。

内核缓冲溢出以 `SYN_DROPPED` 告诉用户空间中间增量已经丢失。r48 的 `InputDevice::process()` 立即把 `mDropUntilNextSync` 置 true 并整体 reset；之后忽略事件，遇到下一笔 `SYN_REPORT` 时只清 flag，该边界本身也不再交给 Mapper。reset 可让 Mapper查询当前状态并让 Dispatcher 取消旧协议，下一份完整报告再从新基线产生事件。

关键边界是 flag 位于逻辑 `InputDevice`，不是 `eventHubId → flag`。composite 的一个子节点 A 报 dropped 后，B 的事件也被丢弃，而任一子节点的下一个 SYN_REPORT 都会解除状态；代码没有确认解除者仍是 A。这是 r48 实现语义，不能擅自改写成“只丢 A，直到 A 的同步帧”。

### 练习 9：composite中谁能结束另一个子节点的drop状态

设 A、B 聚合为一个逻辑设备。顺序为 A:`SYN_DROPPED`，B:`ABS_X`，B:`SYN_REPORT`，A:`ABS_Y`，最后才是 A:`SYN_REPORT`。问哪几笔会进 Mapper，何时清 drop？

唯一答案是：A 的 dropped 触发整体 reset 但不进普通 Mapper；B 的 ABS 被丢；B 的 SYN_REPORT 清逻辑设备共享 flag，本身也不进 Mapper；随后 A 的 ABS_Y 已恢复，会进入 A 的 Mapper，最后 A 的 SYN_REPORT 也正常进入。实现没有等待 A 自己的同步边界。

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (mDropUntilNextSync) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'if (rawEvent->type == EV_SYN && rawEvent->code == SYN_REPORT) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mDropUntilNextSync = false;' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F '} else if (rawEvent->type == EV_SYN && rawEvent->code == SYN_DROPPED) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'mDropUntilNextSync = true;' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'reset(rawEvent->when);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'void InputDevice::reset(nsecs_t when) {' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'NotifyDeviceResetArgs args(mContext->getNextId(), when, mId);' frameworks/native/services/inputflinger/reader/InputDevice.cpp
grep -n -F 'options.deviceId = entry->deviceId;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizeCancelationEventsForAllConnectionsLocked(options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'event->deviceId = device->id == mBuiltInKeyboardId ? 0 : device->id;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mBuiltInKeyboardId = NO_BUILT_IN_KEYBOARD;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'event->deviceId = (device->id == mBuiltInKeyboardId)' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'Ignoring spurious device removed event for eventHubId' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'releaseControllerNumberLocked(device);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mDevices.removeItem(device->id);' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'mOpeningDevices = device->next;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->next = mClosingDevices;' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'device->removeEventHubDevice(eventHubId);' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'if (device->hasEventHubDevices()) {' frameworks/native/services/inputflinger/reader/InputReader.cpp
grep -n -F 'if (mNeedToReopenDevices) {' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'closeAllDevicesLocked();' frameworks/native/services/inputflinger/reader/EventHub.cpp
grep -n -F 'break; // return to the caller before we actually rescan' frameworks/native/services/inputflinger/reader/EventHub.cpp
```

## 14. close先终止节点资源，再用REMOVED让Reader收缩或销毁逻辑设备

EventHub close 先处理 built-in 标记、尝试从 epoll 移除输入/video fd、把仍存在的 video 节点放回 unattached 队列、释放 controller number、从 `mDevices` 移除并关闭 fd。若节点还在 opening 链，Reader 从未见过它，代码直接摘链并 delete，不发幽灵 `DEVICE_REMOVED`；已报告节点则头插 closing，稍后先从对象取出映射后的 id 写成 synthetic removal，再 delete。RawEvent 并不携带整份 identifier。

Reader 收到 remove 后先删正向 eventHubId，再从反向列表删该 id，并 bump global generation；然后才从逻辑对象 erase Context/Mapper。若还有子节点，重新 `configure(0)` 求 classes/sources；无论是否还有子节点，最后都整体 reset。partial removal 因而保留 logical id，却会取消该逻辑 id 的既有下游状态；last removal 则让反向表失去对象，下一份公开快照不再包含它。

关闭发现来自 IN_DELETE、read 0/ENODEV 或 HUP。pending inotify 被安排在同批其他 epoll item 后处理；若读/HUP 自己先 close，后来目录删除只会查不到 path 并忽略。多条发现理由不等于多次上层 remove。

built-in keyboard 是值得单列的版本边界：`closeDeviceLocked()` 在把对象挂到 closing 前先把 `mBuiltInKeyboardId` 设为 `NO_BUILT_IN_KEYBOARD`；稍后生成 REMOVED 又用“旧正 id 是否等于当前 marker”决定是否映射为 0。由代码顺序可见，此时旧 built-in removal 携带正 id，而 Reader 原先的映射键是 0，会被当成 spurious removal。源码日志已经警告关闭 built-in 会让应用不满意；不能宣称这条稀有路径与普通设备完全对称。

## 15. remove、disable、reset与reopen必须按对象集合和id变化分别诊断

| 操作 | EventHub Device/fd | Reader logical device | id与公开列表 | 下游恢复 |
|---|---|---|---|---|
| reset | 都保留 | 保留 Mapper 并重置 | id/集合通常不变 | 排队 DeviceReset，按 InputState 生成取消 |
| Reader `setEnabled(false)` | 对象保留，fd 注销并关闭 | 保留 | 仍可作为 disabled 设备出现在快照 | 关闭前 reset；再次 enable 重开同一路径 |
| remove | 节点对象延迟删除 | 删一个 child；最后一个才消失 | global generation 改变，logical id 可保留或消失 | 幸存/末项都整体 reset |
| `CHANGE_MUST_REOPEN` | 先 close all，下一轮再 scan/open | 先 remove 后重新 create/merge | 动态 eventHubId 与 logical id 通常重新分配 | 旧对象 reset，新增对象也 reset |

`requestReopenDevices()` 只设置 EventHub 标志。Reader 当前配置轮接着调用 `getEvents()` 时，分支 close all、设置 needScan 并先返回 0，刻意不在同一次调用里重扫；后续调用先报告 closing，再扫描、报告 opening 与 FINISHED。descriptor 若原始输入稳定可保持，但两个整数分配器都继续递增，所以不能拿旧 id 串接新连接期。

排查时先问对象是否仍在，而不是先看有没有 reset：disabled 会留在列表；partial composite removal 会留 logical id；同一 IMS pending 窗口里的短暂 add/remove 甚至可能只交付最终快照。相反，看到 DeviceReset 也只证明恢复 marker 进入事件账，必须再核对 Dispatcher 是否轮到它、各 Connection 是否有可合成的匹配 InputState，以及 App 是否回执。

## 16. 诊断顺序：沿身份、集合、版本和恢复marker逐层收证据

面对“节点在、设备不在”或“拔出后状态卡住”，按下面顺序最省时间：

1. **先认路径与 open 门。** 核对 excluded name、基本 identity ioctl、classes 是否为 0、输入 fd epoll 注册是否成功；不要从 `open()` 成功直接跳到 Reader。
2. **再还原 descriptor。** 明确 vendor/product、uniqueId、nonce 与零 vendor/product 的 name/location fallback；同 name 不是 merge 条件，相同 descriptor 也不是物理同机身证明。
3. **画两张 Reader 表。** 记录每个 eventHubId 指向哪个 shared logical device，以及反向表有哪些 child；再区分保留 id、动态 logical id 与首 identifier。
4. **列每个 child 的 Mapper。** class、Mapper、source 三列分开；普通 RawEvent 只送对应 child 的向量，SYN drop flag 却属于整个 logical device。
5. **看 configure 的实际分支。** 区分 changes=0、显式 disabled、缺 viewport、overlay/alias 与 enable 失败；不要只读函数末尾注释推断 fd 关闭时机。
6. **对比两个 generation。** global 改变决定 Reader 生成快照，per-device 改变才让只看 id/generation 的客户端识别版本；partial removal 是重点缺口。
7. **最后追恢复完成点。** EventHub closing、Reader remove/reset、Queued listener flush、Dispatcher DeviceReset、Connection CANCEL 与 App FINISHED 是逐层推进，不是一笔同步事务。

本章留下的稳定模型是：**EventHub Device 的 path 与当前 fd 管节点资源，eventHubId 寻址节点对象，descriptor 决定 Reader 聚合，logical deviceId 寻址公开设备，per-device generation 标识其版本，DeviceReset 修复已经建立的事件协议。** 第 249 章继续沿 Mapper 内部追踪：evdev ABS/MT slot 与 accumulator 怎样以 SYN_REPORT 收束，再经校准和 DisplayViewport 投影形成 `NotifyMotionArgs`。
