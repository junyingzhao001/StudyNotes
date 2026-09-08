# 178 Android InputFilter、AccessibilityInputFilter 与事件变换

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 只读源码，不要求编译、不要求连接设备  
> 前置章节：第 20、51、174—177 章

---

## 1. 本章只追一个问题：原始输入被截走后，什么事件重新回到 Dispatcher

把 InputFilter 想成下面这个同步函数，会误判几乎所有完成点：

```text
event = filter(event);
dispatch(event);
```

Android 11 的真实结构是：

```text
InputReader 产生硬件 Key/Motion
        ↓
InputDispatcher 先执行 early policy
        ↓
复制为 Java InputEvent，交给顶层 IInputFilter
        ↓
原 notify 路径停止，不再自行进入 inboundQueue
        ↓
system_server 主 Looper 上的 transformation chain
        ↓
丢弃 / 延迟 / 拆分 / 合并 / 生成新事件
        ↓
IInputFilterHost 以 ASYNC + FILTERED 重新注入
        ↓
InputDispatcher 重新选目标、publish、等待 App FINISHED
```

因此，本章只需要抓住一句话：

> InputFilter 不是“修改原对象后返回”，而是接管原始流，再生产一条新的注入流。

这个模型立刻解释了四个容易混淆的事实：

- filter 回调没有“修改后的返回事件”；
- 默认透传也会走一次重新注入；
- filter 处理完不等于 App 已收到；
- 开关或重建 transformation 时，必须自己维护 DOWN/UP/CANCEL 一致性。

---

## 2. 源码地图与四个完成点

核心文件：

```text
frameworks/native/services/inputflinger/dispatcher/
└── InputDispatcher.cpp

frameworks/base/services/core/
├── java/com/android/server/input/InputManagerService.java
└── jni/com_android_server_input_InputManagerService.cpp

frameworks/base/core/java/android/view/
├── InputFilter.java
├── IInputFilter.aidl
└── IInputFilterHost.aidl

frameworks/base/services/accessibility/java/com/android/server/accessibility/
├── AccessibilityInputFilter.java
├── EventStreamTransformation.java
├── BaseEventStreamTransformation.java
├── AutoclickController.java
├── KeyboardInterceptor.java
├── KeyEventDispatcher.java
└── MotionEventInjector.java
```

先把“完成”拆成四层：

| 时刻 | 能证明什么 | 不能证明什么 |
|---|---|---|
| native filter callback 返回 false | Java 侧已接管原事件 | transformation 已运行 |
| `InputFilter.onInputEvent()` 返回 | 本轮同步 transformation 调用结束，输入 Java 对象可回收 | 输出已送达 App |
| host 的 ASYNC inject 返回 | native 已完成同步校验并通常已入 inbound queue | 找到目标、publish 或 FINISHED |
| App 回送 FINISHED | 对应 connection 的 dispatch entry 已结账 | View 业务、click 或画面已完成 |

`IInputFilter` 与 `IInputFilterHost` 都声明为 `oneway`。不过标准无障碍 filter 与 IMS 同在 system_server，常见路径可能是本地 Binder 对象直接调用；即使如此，`InputFilter` 方法也只投 Handler，host 又使用 ASYNC 注入，仍没有端到端确认。

---

## 3. native 入口：early policy 先于 filter，而且只截硬件 Key/Motion

`InputDispatcher::notifyKey()` 的顺序是：

```text
validate
→ 补 TRUSTED、处理虚拟键/Meta shortcut
→ interceptKeyBeforeQueueing
→ 检查 mInputFilterEnabled
→ policyFlags |= FILTERED
→ mPolicy->filterInputEvent(...)
→ 若返回 true，才构造 KeyEntry 并 enqueue
```

`notifyMotion()` 同样先：

```text
validate
→ 补 TRUSTED
→ interceptMotionBeforeQueueing
→ 检查 filter
→ 构造临时 MotionEvent
→ policyFlags |= FILTERED
→ filterInputEvent
```

early policy 必须先做，是因为唤醒、电源键、`PASS_TO_USER` 等系统语义不能依赖无障碍主线程是否及时运行。

还要注意 filter 的覆盖面：

- 硬件通知入口的 Key/Motion 会被截；
- 普通 `injectInputEvent()` 不走 `notifyKey()/notifyMotion()`，所以不会再次进入 filter；
- FOCUS、DEVICE_RESET、CONFIGURATION_CHANGED、Dispatcher 自己生成的 repeat/cancel 也不是这条 Java filter 回调。

也就是说，“安装 InputFilter 后收到所有 input”并不准确；它收到的是这两个硬件通知入口能转换的 Key/Motion。

Dispatcher 在调用 Java policy 前主动释放 `mLock`。这避免拿 Dispatcher 锁跨 JNI/Java，却也意味着 filter 开关可在这段窗口中变化，后文会看到一个 disable 竞态。

---

## 4. 那个 boolean 属于 JNI policy 回调，不属于 IInputFilter

JNI 先把 native 事件转为 Java 对象：

- Key：`android_view_KeyEvent_fromNative()`；
- Motion：`android_view_MotionEvent_obtainAsCopy()`；
- 其他类型：直接返回 true，继续正常派发。

`IInputFilter.filterInputEvent()` 本身是 void。真正返回 boolean 的是 native policy 调到 IMS 的内部方法：

```java
final boolean filterInputEvent(InputEvent event, int policyFlags) {
    synchronized (mInputFilterLock) {
        if (mInputFilter != null) {
            try {
                mInputFilter.filterInputEvent(event, policyFlags);
            } catch (RemoteException e) {
                /* ignore */
            }
            return false;
        }
    }
    event.recycle();
    return true;
}
```

其语义是：

| IMS 返回值 | native notify 路径 |
|---|---|
| false | Java filter 已接管，原事件不入普通 inbound queue |
| true | 当前没有可用 filter，原事件继续 enqueue |

false 只证明“调用已经投给 IInputFilter”，不证明 Handler 已运行。

失败分支并不统一：

1. JNI 无法创建 Java 对象，或 Java callback 抛出未清异常：JNI 令 `pass=true`，原事件 fail-open。
2. IMS 成员非空，但远端 `mInputFilter.filterInputEvent()` 抛 `RemoteException`：异常被忽略，IMS 仍返回 false，原事件被吃掉。
3. `setInputFilter()` 调 install 失败也不回滚成员或 native enable；后续远端失败可继续落入上一条。
4. IMS 把对象所有权交给 callee，却不在“成员非空”分支显式 recycle。标准同进程实现会让同一对象最终进入 Handler finally；若换成远端 proxy，远端回收的是反序列化副本，调用方这份对象无论成功还是 RemoteException 都只会等待 GC。

所以不能把“filter 故障”笼统写成 fail-open。

---

## 5. InputFilter 的 Handler 同时规定线程与对象所有权

`InputFilter` 的三个 AIDL 入口只投消息：

```text
install(host)         → MSG_INSTALL
uninstall()           → MSG_UNINSTALL
filterInputEvent(...) → MSG_INPUT_EVENT
```

Handler 依次执行：

```text
MSG_INSTALL:
    mHost = host
    reset consistency verifiers
    onInstalled()

MSG_UNINSTALL:
    onUninstalled()
    finally mHost = null

MSG_INPUT_EVENT:
    onInputEvent(event, flags)
    finally event.recycle()
```

这给出三个结论：

- non-reentrant 依赖同一 Looper 的消息串行，不是全局锁；
- 回调收到的 InputEvent 在 `onInputEvent()` 返回后就会 recycle；
- transformation 想延迟保存事件，必须 `KeyEvent.obtain()` 或 `MotionEvent.obtain()`。

`AccessibilityInputFilter` 构造时传入 `context.getMainLooper()`。其 filter、feature handler、KeyboardInterceptor、Key timeout 和 MotionEventInjector 调度，大量工作都汇到 system_server 主 Looper。

若 `onInputEvent()` 抛 RuntimeException，finally 仍会 recycle，但异常本身不会在本类中被吞掉；对 system_server 主线程而言，这不是温和的单事件失败。

---

## 6. 默认“原样透传”也会改变事件身份

基类默认实现只有一行：

```java
public void onInputEvent(InputEvent event, int policyFlags) {
    sendInputEvent(event, policyFlags);
}
```

host 最终调用：

```java
nativeInjectInputEvent(
        mPtr, event,
        0, 0,
        InputManager.INJECT_INPUT_EVENT_MODE_ASYNC,
        0,
        policyFlags | WindowManagerPolicy.FLAG_FILTERED);
```

固定参数意味着：

- injector pid/uid 是 0；
- root 身份使注入获得全局权限和 TRUSTED；
- sync mode 是 ASYNC；
- timeout 参数为 0；
- 输出带 FILTERED，并在 native 注入入口再加 INJECTED。

“原样”只表示 Java 层通常复用相同 KeyEvent/MotionEvent 内容，不表示重新进入 native 后身份完全不变。r48 `injectInputEvent()` 对 Key 和 Motion 都把 deviceId 重写为 `VIRTUAL_KEYBOARD_ID`；source、合法 displayId、event id 和大部分载荷继续取自输入，publish 时 HMAC 由 Dispatcher 重新生成。

因此，即便 filter 什么也没改，派发链内部仍会发生：

```text
硬件 deviceId → VIRTUAL_KEYBOARD_ID
硬件路径 flags → 再带 INJECTED / FILTERED / TRUSTED
原 HMAC → Dispatcher 对输出重新签名
```

其中 policy flags 是系统内部路由信息，并不作为 KeyEvent/MotionEvent 字段直接暴露给 App；App 可直接观察到的关键差异是 deviceId。它不能再凭这个字段还原 filter 之前的物理设备身份。

---

## 7. FILTERED 做两件事，但不是“递归开关”

硬件事件在送进 filter 前已经带 `FILTERED`；host 输出又 OR 一次。

注入入口看到 FILTERED 后跳过：

```text
interceptKeyBeforeQueueing
interceptMotionBeforeQueueing
```

这样默认透传不会重复触发 wake、系统键策略或 `PASS_TO_USER` 决策。

输出不再进入顶层 filter 的根本原因则是入口不同：

```text
硬件：notifyKey/notifyMotion → filter hook
输出：injectInputEvent → 直接生成 injected EventEntry
```

所以两条职责要分开：

- “不再进 filter”来自 inject 路径没有 filter hook；
- “不重复 early policy”来自 FILTERED。

FILTERED 也不等于“合法”或“必达”。注入仍会做事件结构校验、目标选择和派发；ASYNC 的调用方不会等待后续无目标、stale、disabled 或 connection 故障。

---

## 8. 安装、卸载、replacement 与 reset 的真实边界

`InputManagerService.setInputFilter()` 在 `mInputFilterLock` 下：

```text
若有 old:
    mInputFilter = null
    oldHost.disconnectLocked()
    oldFilter.uninstall()

若有 new:
    mInputFilter = new
    newHost = InputFilterHost
    newFilter.install(newHost)

nativeSetInputFilterEnabled(filter != null)
```

old host 的 `mDisconnected` 是能力撤销点。旧 Handler 或延迟任务即使还持有 host Binder，调用也只会静默丢弃，不能污染新配置。

native enabled 布尔真正变化时，Dispatcher 执行：

```text
synthesize CANCEL_ALL for every connection
reset key repeat
release pending event
drain inbound queue
clear no-focus timeout / AnrTracker
clear TouchState / hover / replaced keys
```

但它不遍历 connection 去 drain outboundQueue/waitQueue。已 publish 的旧 entry 仍可能等 FINISHED，合成的 CANCEL 也要沿 connection 队列发送；“drop everything”不能解释成所有节点瞬间消失。

还有两个 r48 角落：

1. 非 null filter 直接替换为另一个非 null filter时，native 看到 enabled 仍为 true，会提前返回，不做全局 reset；安全主要依靠旧 host disconnect 和新旧 Handler 的序列门。
2. `notifyKey/notifyMotion` 在 filter callback 前释放 Dispatcher 锁。若它已看到 enabled=true，随后另一线程完成 disable/reset，IMS callback 再看到 `mInputFilter=null` 并返回 true，这笔较早事件可在 reset 之后重新加锁入队。因而 enable/disable 是恢复协议，不是严格的跨线程原子切面。

---

## 9. AccessibilityInputFilter 怎样把 feature 拼成链

AMS 在主 Handler 上计算当前用户 flags。0→非 0 时创建或复用 `AccessibilityInputFilter` 并经 WMS 交给 IMS；非 0→0 时卸载顶层 filter。

filter 已存在而 feature 集合变化时，只调用：

```text
setUserAndEnabledFeatures
→ disableFeatures()
→ 更新 userId / flags
→ enableFeatures()
```

这不会调用 native `setInputFilterEnabled()`，所以没有 Dispatcher 全局 reset。

`enableFeatures()` 先添加一个共享 Autoclick，再按 display 依次添加：

```text
TouchExplorer
MagnificationGestureHandler
MotionEventInjector
```

每次 `addFirstEventHandler()` 都把新节点插到链首。因此功能全开时，Motion 在副屏的大致顺序是：

```text
MotionEventInjector
→ MagnificationGestureHandler
→ TouchExplorer
→ AutoclickController
→ AccessibilityInputFilter sink
```

默认显示最后还在链首增加一个 KeyboardInterceptor：

```text
KeyboardInterceptor
→ MotionEventInjector
→ Magnification
→ TouchExplorer
→ Autoclick
→ sink
```

KeyboardInterceptor 对 Motion 使用接口默认实现继续向后传；Key 则只进入默认显示的这条链。顺序是功能语义的一部分：后加入、排在前面的节点先决定是否消费或变换。

`onInstalled()` 先设 `mInstalled=true`，再 disable/enable；`onUninstalled()` 先设 false，再 disable。复用对象时，这能清理旧 handler、注入计划、observer 和 stream state。

---

## 10. per-display chain 之外，stream gate 并不都是 per-device

`mEventHandler`、TouchExplorer、Magnification 和 MotionEventInjector 都按 display 保存；Motion 按 event displayId 选 chain，不存在对应 key 时改用默认 display 的 chain。

但这只是“选哪条链”。event 自身的 displayId 没被修正：

- displayId=-1 的输出会在 Dispatcher 侧改投当前 focused display；
- 不存在的正 displayId 即使借默认 chain 处理，重新注入时仍保留该正值，未必能找到目标。

更重要的是顶层 stream state：

| 类型 | 状态粒度 | reset 后的起点 | 何时自动关闭 |
|---|---|---|---|
| Touchscreen | 全 filter 共一份 | DOWN 或 HOVER_ENTER | 常规 UP/CANCEL 不关闭 |
| Mouse | 全 filter 共一份 | DOWN 或 HOVER_MOVE；SCROLL 可单独处理 | 常规 UP 不关闭 |
| Keyboard | SparseBooleanArray 按 deviceId | 该 device 首个 DOWN | UP 不删除 map 项 |

这些 gate 在 source 变化、`PASS_TO_USER` 清理、feature 重建或安装卸载时 reset；它们不是每个手势结束就复位。

这与 `InputFilter` 文档要求“按 deviceId + source 分开流”存在实现落差：

- Touch/Mouse gate 没按 deviceId 或 displayId 分账；
- 一个触屏设备见过 DOWN 后，另一同 source 设备的半条流可通过顶层 gate；
- Keyboard 虽按 deviceId 建项，但首个 DOWN 后一直保留 true 到外部 reset。

下游 TouchExplorer、Magnifier 等仍可能有自己的严格状态机，不能用这个顶层 gate 证明整条输出一定一致。

---

## 11. raw、transformed、PASS_TO_USER 与回收

Motion 进入 transformation 前，filter 会：

```text
mPm.userActivity(eventTime, false)
transformedEvent = MotionEvent.obtain(original)
chain.onMotionEvent(transformedEvent, original, policyFlags)
transformedEvent.recycle()
```

随后外层 `InputFilter.H` 的 finally 再 recycle original。

两份对象的职责是：

- `transformedEvent`：前序节点可修改并继续下传；
- `rawEvent`：保留进入 AccessibilityInputFilter 时的轨迹，供识别器参考。

同步链调用返回后两者都会被回收。任何 delayed decision 都必须自己 obtain 副本。

若 early policy 清掉 `PASS_TO_USER`，AccessibilityInputFilter 不直接保留旧状态：

```text
state.reset()
→ 对所有 display chain 调 clearEvents(eventSource)
→ super.onInputEvent(original, originalFlags)
```

`clearEvents()` 让下游有机会生成已开放流的 CANCEL；原事件本身仍经 host 注入，但因没有 PASS_TO_USER，Dispatcher 会按 policy 丢弃。

source 改变也会 reset 对应顶层 state，并对所有 display chain 广播 `clearEvents(newSource)`。注意参数是新事件的 source；每个 transformation 必须按 source 自己决定清哪份状态。

---

## 12. Autoclick 暴露的共享实例与副屏 displayId 缺口

Autoclick 并非每个 display 一个实例：

```java
mAutoclickController = new AutoclickController(...);
addFirstEventHandlerForAllDisplays(displaysList, mAutoclickController);
```

同一个对象只有一份：

- `ClickScheduler`；
- 最近一笔 MotionEvent；
- anchor、metaState、policyFlags；
- settings observer。

类注释还明确假定“每个实例只接收一只 mouse”，但 AccessibilityInputFilter 会把所有 display chain 指向这一实例。多鼠标或跨 display 事件因此共享调度状态；`SparseArray` 的外观不代表内部隔离。

鼠标移动超过 20 px 后，scheduler 延迟生成 synthetic DOWN/UP。它调用的是没有 displayId 参数的公开 `MotionEvent.obtain(...)` 重载，该重载固定传 `DEFAULT_DISPLAY`：

```text
最近 hover 的 device/source/coords/flags → 被复制
最近 hover 的 displayId               → 没有复制，变成 0
```

所以 r48 在副屏触发 autoclick 时，生成点击会以默认显示 0 重新注入。再叠加上一节的 deviceId 重写，App 最终看到的也不是原鼠标身份。

Autoclick 位于 Motion 链末端、sink 之前；它用 `AutoclickController.super.onMotionEvent()` 发送 synthetic DOWN/UP，因此这些点击直接走下游 sink，不会回头重跑前面的 Injector/Magnifier/TouchExplorer。

---

## 13. Key 过滤：两个串行队列、一个名义 500ms 和一个放行缺口

开启 key filtering 后，默认 display 链首的 KeyboardInterceptor 先处理音量键 policy delay：

| 返回值 | 行为 |
|---|---|
| < 0 | 丢弃 |
| = 0 | 交给 AMS |
| > 0 | 按 uptime 延迟后重新询问 |

只要队列已有旧键，后到键即使 delay=0 也先排队。链表 tail 是最旧项，保证后到键不越过被 policy 延迟的音量键。

AMS 再让 `KeyEventDispatcher` 把一个 Key clone 发给所有当前可过滤服务。服务方法的即时 boolean 只表示“回调已成功发出、以后会回结果”，不是 handled。对每个接受者，`PendingKeyEvent.referenceCount++`。

聚合规则：

- 任一服务最终回 `handled=true`，聚合结果变 true；
- 每个结果、service flush 或 timeout 都减引用；
- 引用归零且 handled=false，才把 Key 投回 AMS 主 Handler；
- handled=true 时回收，不再给 App。

`ON_KEY_EVENT_TIMEOUT_MILLIS=500` 只是同一个主 Looper 上的 delayed Message，不是硬截止时间。主线程阻塞时 timeout 会晚执行；在它真正运行前到达的服务结果仍可能被接受。

还有一个 r48 放行缺口：

```java
mAms.notifyKeyEvent(event, policyFlags); // boolean 被忽略
```

若没有任何 bound service 接受回调，`notifyKeyEventLocked()` 返回 false 并回收 clone；KeyboardInterceptor 不调用 `super.onKeyEvent()`，原 Key 也不会自动透传。正常 feature 计算要求存在合资格服务，降低了概率，但安装/权限/服务状态竞态仍不能被描述成 fail-open。

feature 重建时 `disableFeatures()` 调用 `mKeyboardInterceptor.onDestroy()`，可它没有 override，实际落到接口空实现；已排队的 policy-delay Key 不会在这里清除。KeyEventDispatcher 中已经发给服务的 pending 也只会因结果、500ms 消息或 service `flush()` 结账。

---

## 14. MotionEventInjector：计划发完不等于注入成功

服务调用 `dispatchGesture()` 时，AMS：

1. 检查目标 display 可触摸；
2. 在 `mMotionEventInjectors==null` 时最多按 uptime 等 1000ms；
3. 取对应 display 的 injector；
4. 把 gesture steps 投到其主 Handler。

MotionEventInjector 将 steps 展开成定时 MotionEvent；每一笔在指定 eventTime 到达时同步送入下游 transformation。最后一笔发送后立即：

```java
service.onPerformGestureResult(sequence, true);
```

这个 true 只证明 injector 的计划已走到末项。后续节点可以消费事件；host 是 ASYNC；Dispatcher 也可能找不到目标。它不证明 App 收到，更不证明 View handled。

真实 Motion 到来时通常会：

```text
remove pending synthetic messages
→ 若已向下游发过 DOWN，则补 CANCEL
→ 对未完成 sequence 回调 false
→ 再下传真实 Motion
```

但 r48 有两个精确角落：

- injected touch 已开放时，mouse HOVER_MOVE 直接 return：它既不取消注入手势，也不向下游传这笔真实 hover；
- `cancelAnyGestureInProgress()` 用无 displayId 的 `MotionEvent.obtain(...)` 生成 CANCEL，默认 display=0。副屏注入流被物理事件打断时，这个 CANCEL 可能被发往默认屏，无法关闭副屏 Dispatcher TouchState。

普通计划事件在入队前会显式 `event.setDisplayId(displayId)`，所以“正常 gesture 在副屏”与“取消事件仍回默认屏”是两条不同路径。

---

## 15. 诊断顺序与九组只读练习

遇到“开启无障碍后输入丢失/变慢/跑错屏”，按阶段排查：

| 现象 | 优先检查 |
|---|---|
| 所有 Key/Motion 普遍变慢 | system_server main Looper、InputFilter 消息积压 |
| 原事件无 App 输出 | IMS RemoteException、handler 主动消费、Key 无接受者分支 |
| 恰在 feature 切换时缺 UP/CANCEL | handler `onDestroy/clearEvents`、旧 host disconnect、无 native reset |
| Key 约 500ms 或更久才到 | KeyboardInterceptor policy queue、主 Looper、KeyEventDispatcher pending |
| performGesture 回 true 但 App 无事件 | 下游 transformation、host ASYNC、Dispatcher target |
| 副屏 autoclick 跑到主屏 | Autoclick synthetic obtain 默认 display=0 |
| 副屏注入被打断后流未关闭 | MotionEventInjector synthetic CANCEL 默认 display=0 |
| App 看到 deviceId=-1 | filter output 经 inject 重写为 VIRTUAL_KEYBOARD_ID |
| tracker 已空但 connection 仍有旧项 | reset 清 AnrTracker，不 drain waitQueue |

### 练习一：证明 early policy 先于 filter

```bash
cd /Users/ninebot/androidSource
sed -n '3075,3230p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

分别标出 Key/Motion 的 validate、early policy、FILTERED 和 boolean return。

### 练习二：证明 boolean 与对象所有权

```bash
sed -n '954,985p' \
  frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
sed -n '1958,1980p' \
  frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '120,260p' \
  frameworks/base/core/java/android/view/InputFilter.java
```

解释 JNI local ref、Java recycle 和 Handler finally 分别管理什么。

### 练习三：证明默认透传会改 deviceId

```bash
sed -n '2310,2335p' \
  frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '3274,3425p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

找出 pid/uid、ASYNC、FILTERED、INJECTED、TRUSTED 与 `VIRTUAL_KEYBOARD_ID`。

### 练习四：核对切换与 reset

```bash
sed -n '590,625p' \
  frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
sed -n '3920,3942p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
sed -n '4026,4048p' \
  frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

确认非 null→非 null replacement 为什么不触发第二次 reset。

### 练习五：手工重建 transformation 顺序

```bash
sed -n '404,540p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/AccessibilityInputFilter.java
```

写出默认 display 与副屏各自的 head→sink 顺序。

### 练习六：核对三类 stream gate

```bash
sed -n '560,735p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/AccessibilityInputFilter.java
```

检查哪些状态按 deviceId，哪些在 UP/CANCEL 后仍为 true。

### 练习七：复现 Autoclick 的 displayId 丢失

```bash
sed -n '398,435p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/AutoclickController.java
sed -n '1680,1765p' \
  frameworks/base/core/java/android/view/MotionEvent.java
```

沿无 displayId 的 overload 找到 `DEFAULT_DISPLAY`。

### 练习八：核对 Key 的两个缺口

```bash
sed -n '1,190p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/KeyboardInterceptor.java
sed -n '120,275p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/KeyEventDispatcher.java
sed -n '275,300p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/AbstractAccessibilityServiceConnection.java
```

证明 500ms 由 Handler 实现、AMS boolean 被忽略、KeyboardInterceptor 没有自己的 onDestroy。

### 练习九：核对 gesture 完成与副屏 CANCEL

```bash
sed -n '1048,1078p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/AccessibilityManagerService.java
sed -n '100,220p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/MotionEventInjector.java
sed -n '291,330p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/MotionEventInjector.java
sed -n '447,480p' \
  frameworks/base/services/accessibility/java/com/android/server/accessibility/MotionEventInjector.java
```

区分正常计划事件的 `setDisplayId()` 与取消事件的默认 display。

---

## 16. 本章结论、自检与下一章

最终模型可以压缩成：

```text
接管
= hardware notify 已完成 early policy
+ native 给 Java 一份 Key/Motion copy
+ IMS 返回 false 终止原 notify 路径

变换
= system_server 主 Looper
+ per-display handler chain
+ 不完全 per-device 的顶层 stream gate
+ 每个 handler 自己维护取消与延迟对象

输出
= root pid/uid
+ ASYNC + FILTERED + INJECTED
+ deviceId 重写与 HMAC 重签
+ 重新走 Dispatcher target/publish/FINISHED
```

最重要的边界是：

- false 是“原事件被接管”，不是 filter 业务成功；
- FILTERED 跳过重复 early policy，inject 入口本身才避免 filter 回环；
- 默认透传也把硬件 deviceId 改为 VIRTUAL_KEYBOARD_ID；
- enable 布尔变化会 reset 全局路由状态，但不 drain 每个 connection 队列；
- 非 null replacement 和 feature 内部重建都没有同样的 native reset；
- Touch/Mouse 顶层 gate 不按 device/display 分账，Keyboard 的 device map 也不在 UP 时关闭；
- Key 的 500ms 是主 Looper 定时，且“没有服务接受”分支不会自动下传；
- Autoclick 的点击与 MotionEventInjector 的取消在 r48 都存在副屏 displayId 丢失；
- gesture success 只证明计划发到末项，不证明 App 收到或处理。

自检时应能回答：

1. 为什么 early policy 必须在 filter 前？
2. `IInputFilter.filterInputEvent()` 是 void，native 的 boolean 从哪里来？
3. JNI fail-open 与 IMS RemoteException 为什么方向相反？
4. 默认透传后哪些字段不再代表原硬件事件？
5. FILTERED 和 inject 入口分别避免什么重复？
6. 顶层 enable reset 为什么不等于 waitQueue 已空？
7. 为什么 per-display chain 不能推出 per-device stream state？
8. Key 的 500ms 为什么可能超过 500ms？
9. MotionEventInjector 的 true 回调为什么不是注入成功证明？
10. 两条副屏 synthetic Motion 路径分别在哪里丢失 displayId？

下一章进入 **InputReader 的 EventHub、InputDevice 与 mapper 配置重建**，从 `/dev/input/event*` 反向追踪原始 evdev 记录怎样成为本章入口处的 NotifyKey/NotifyMotion。
