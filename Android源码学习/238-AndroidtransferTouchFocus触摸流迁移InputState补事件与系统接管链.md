# 238 Android transferTouchFocus触摸流迁移、InputState补事件与系统接管链

> 源码版本：Android 11 / `android-11.0.0_r48`  
> 学习方式：macOS 只读核源；不编译、不修改 AOSP

## 1. 本章问题：把“焦点”翻译成触摸流所有权

`transferTouchFocus(fromToken, toToken)` 中的 focus，不是键盘焦点、View 焦点或 IME 编辑焦点。它的典型契约是：一条已经 `DOWN` 的 pointer 流，今后应由哪个窗口连接接收。

普通触摸先在 `DOWN` 时命中窗口，后续 `MOVE/UP` 通常沿既有 `TouchState` 发送。系统拖放、自由窗移动/缩放或内联建议切入 IME 时，接管者是在手势中途出现的；仅把新窗口放到顶层，不能重写已经锁定的接收者。在非同 token、两窗口与两 Connection 有效且源端有 pointer memento 的典型完整路径中，该 API 会完成五件事：

```text
显式选择 from / to
→ 改写后续真实事件的路由
→ 把新端尚未知晓的 pointer 状态合并过去
→ 用 CANCEL 结束旧端视角
→ 用 DOWN / POINTER_DOWN 建立新端视角
```

它不直接修改 `mFocusedWindowHandlesByDisplay`、`InputWindowInfo.hasFocus` 或 App 内部焦点，也不直接把后续 `KeyEvent` 路由给 `to`。但补出的首个 `DOWN` 仍会经过 `dispatchPointerDownOutsideFocus()`；若策略层能把 token 映射到可聚焦窗口，异步回调可能推动 WMS 调整 task focus。“不是键盘焦点转移”不等于“焦点绝无间接变化”。

本章要抓住三个结论：

1. 迁移的是“窗口当前拥有的触摸份额”，不是某个可单独指定的 pointer ID。
2. `TouchState` 与每个 `Connection.inputState` 是两本不同的账，缺一不可。
3. 返回 `true` 不等于两个客户端都已处理完补偿事件，r48 甚至存在“路由已改、补偿未发仍返回成功”的边界。

## 2. 全链路：一个路由账本加两个连接账本

典型完整主链可以先压缩成：

```text
WMS / IMMS 等系统路径
→ InputManagerService / InputManagerInternal
→ nativeTransferTouchFocus()
→ InputDispatcher::transferTouchFocus()
   ├─ TouchState：from 条目删除，to 条目加入或合并
   ├─ from.inputState → to.inputState：复制或追加 pointer memento
   ├─ from Connection：合成 CANCEL / HOVER_EXIT
   └─ to Connection：合成 DOWN / POINTER_DOWN
→ 后续真实 MOVE / UP 按新 TouchState 分发
```

三处状态各自回答不同问题：

| 状态 | 粒度 | 回答的问题 | 迁移时的动作 |
|---|---|---|---|
| `TouchState.windows` | 每个 Display 的当前 pointer 流 | 下一条真实事件发给哪些窗口 | 删除 `from`，加入或合并 `to` |
| `fromConnection.inputState` | 单个 InputChannel | 旧端已被 InputDispatcher 承诺过哪些输入状态 | 保留到生成取消事件，再由入队跟踪消掉 |
| `toConnection.inputState` | 单个 InputChannel | 新端已经知道哪些 pointer | 合并旧端快照，标出需要补起点的后缀 |

这里的“已知”不是“App 主线程已经处理完成”。`enqueueDispatchEntryLocked()` 在把 `DispatchEntry` 放入 `outboundQueue` 前就调用 `trackMotion()`；所以 `InputState` 更准确地表示 dispatcher 已接受并承诺给该连接的协议状态。

## 3. Java 与 JNI：token 如何抵达 InputDispatcher

`InputManagerService` 有两个重载：

```text
transferTouchFocus(InputChannel fromChannel, InputChannel toChannel)
transferTouchFocus(IBinder fromChannelToken, IBinder toChannelToken)
```

`InputChannel` 重载取出两端 connection token；`IBinder` 重载供 `InputManagerService.LocalService` 等内部调用，这个内部类实现了 `InputManagerInternal`。JNI 只做三步：

1. 任一 Java token 为 `null` 时返回 `false`；
2. 将 Java `IBinder` 转成 native `sp<IBinder>`；
3. 调用 dispatcher，并把 `bool` 原样转成 JNI 布尔值。

这里有一个 r48 细节：`Objects.nonNull(fromChannelToken)` 只返回布尔值，忽略其返回值不会抛异常。因此 token 重载的 `@NonNull` 并未在 Java 层形成运行时拒绝，真正的 `null` 防线位于 JNI；而 `InputChannel` 重载对空对象调用 `getToken()`，会先在 Java 层触发空指针异常。非空但已 dispose、没有 native peer 的 `InputChannel` 则可由 `getToken()` 得到空 token，最终由 JNI 返回 `false`。

这不是面向普通 App 的公开迁移接口。主要入口位于 system_server 的服务对象和 LocalServices。不过，不能因此推导出 native 自带调用者授权：迁移函数没有 owner UID、权限或 Binder 身份检查。尤其内联建议路径会把 renderer 侧上报的 source token 经 oneway Binder 回调送回 system_server；回调 Stub 随即 `mHandler.post()`，真正的 LocalServices 调用发生在后续 Handler turn，renderer 的 Binder calling identity 不能充当 transfer 的授权依据。调用链仍需审视 token 来源与上层校验。

### 练习 1：钉住 Java、LocalService 与 JNI 边界

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'public boolean transferTouchFocus(@NonNull InputChannel fromChannel,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'return nativeTransferTouchFocus(mPtr, fromChannel.getToken(), toChannel.getToken());' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'public boolean transferTouchFocus(@NonNull IBinder fromChannelToken,' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'Objects.nonNull(fromChannelToken);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'public IBinder getToken() {' frameworks/base/core/java/android/view/InputChannel.java
grep -n -F 'return nativeGetToken();' frameworks/base/core/java/android/view/InputChannel.java
grep -n -F 'static jobject android_view_InputChannel_nativeGetToken(JNIEnv* env, jobject obj) {' frameworks/base/core/jni/android_view_InputChannel.cpp
grep -n -F 'return 0;' frameworks/base/core/jni/android_view_InputChannel.cpp
grep -n -F 'return InputManagerService.this.transferTouchFocus(fromChannelToken, toChannelToken);' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'LocalServices.addService(InputManagerInternal.class, new LocalService());' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'private final class LocalService extends InputManagerInternal {' frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
grep -n -F 'public abstract boolean transferTouchFocus(@NonNull IBinder fromChannelToken,' frameworks/base/core/java/android/hardware/input/InputManagerInternal.java
grep -n -F 'static jboolean nativeTransferTouchFocus(JNIEnv* env,' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'if (fromChannelTokenObj == nullptr || toChannelTokenObj == nullptr) {' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'sp<IBinder> fromChannelToken = ibinderForJavaObject(env, fromChannelTokenObj);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'sp<IBinder> toChannelToken = ibinderForJavaObject(env, toChannelTokenObj);' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
grep -n -F 'getDispatcher()->transferTouchFocus(' frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp
```

读完后分别回答：两个重载的空值表现为何不同？哪一层开始出现 native token？哪一层都没有做 owner UID 判断？

## 4. Native 前置条件：五类出口先定成功边界

`InputDispatcher::transferTouchFocus()` 的控制流很短，出口却决定了整个 API 的语义。

| 顺序 | 条件 | 结果 |
|---|---|---|
| 1 | `fromToken == toToken` | 立即 `true`，发生在加锁和有效性检查之前 |
| 2 | 任一 token 找不到 `InputWindowHandle` | `false` |
| 3 | 两个 handle 的 `displayId` 不同 | `false` |
| 4 | 所有 `mTouchStatesByDisplay[*].windows` 中都找不到 `from` | `false` |
| 5 | 找到 `from` | 改写状态，函数尾部 `true` |

所以，相同的两个非空无效 token 也能走快速成功；JNI 入口则会在 token 为空时先返回 `false`。调用者不能用同 token 的 `true` 证明窗口存在。

代码没有额外要求：

```text
from 带 FLAG_FOREGROUND
命中的 TouchState 满足 state.down == true
当前指针位于 to 的 frame / touchableRegion
to 为 visible 且没有 FLAG_NOT_TOUCHABLE
to 未 paused 且 responsive
from / to ownerUid 相同
两个 Connection 均存在且处于正常状态
```

它先比较两个 handle 自身的 `displayId`，随后在 `mTouchStatesByDisplay` 全表中查找 `from`。全表搜索有正常用途：portal 路由以入口 Display 为 map key，却可把 portal 指向 Display 的目标 handle 存进该 `TouchState.windows`。transfer 会在命中的入口账本中原位替换，不会按 from/to handle 的 Display 重新建 key；“同 Display”约束只比较两个 handle。

循环容器是 `unordered_map`，找到第一份含 `from` 的 `TouchState` 后便 `goto Found`，而 API 没有 display 参数。若同一 handle 因 portal 或多入口路由同时出现在多份状态中，静态实现只替换迭代时先命中的那一份，也没有承诺是哪一份；这不能概括成“按 handle 所属 Display 精确选择状态”。

### 练习 2：列出 native 的真实成功与失败出口

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'bool InputDispatcher::transferTouchFocus(const sp<IBinder>& fromToken, const sp<IBinder>& toToken) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (fromToken == toToken) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'std::scoped_lock _l(mLock);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<InputWindowHandle> fromWindowHandle = getWindowHandleLocked(fromToken);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (fromWindowHandle == nullptr || toWindowHandle == nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (fromWindowHandle->getInfo()->displayId != toWindowHandle->getInfo()->displayId) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'for (std::pair<const int32_t, TouchState>& pair : mTouchStatesByDisplay) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'goto Found;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!found) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'touchState->addPortalWindow(windowHandle);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return findTouchedWindowAtLocked(portalToDisplayId, x, y, touchState,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mTouchStatesByDisplay[displayId] = tempTouchState;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (windowInfo->visible) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!(flags & InputWindowInfo::FLAG_NOT_TOUCHABLE)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<Connection> fromConnection = getConnectionLocked(fromToken);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'sp<Connection> toConnection = getConnectionLocked(toToken);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (fromConnection != nullptr && toConnection != nullptr) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F -A 2 '// Wake up poll loop since it may need to make new input dispatching choices.' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

把五类返回路径画成决策树，并特别标出“同 token 快返”与“找到 `from` 后连接缺失”不属于同一种成功。

## 5. TouchState：迁走的是 from 的整个窗口份额

找到 `TouchedWindow` 后，dispatcher 先保存：

```text
oldTargetFlags
pointerIds
```

随后从 `state.windows` 删除 `from`，并用下面的掩码构造要交给 `to` 的源侧 flags：

```text
FLAG_FOREGROUND | FLAG_SPLIT | FLAG_DISPATCH_AS_IS
```

obscured、outside、zero-coordinates、slippery enter/exit 等一次性或旧窗口相关标志不会从 `from` 继承。注意“只继承三类”只描述源侧贡献：若 `to` 已在 `TouchState` 中，`addOrUpdateWindow()` 会把新 flags 与 `to` 原有 flags 做 OR，并把两端 `pointerIds` 做 OR；目标既有标志不会被清空。

`pointerIds` 的含义还要分两种情况：

- 带 `FLAG_SPLIT` 时，它是该窗口拥有的 pointer ID 位图；迁移的是 `from` 的整个位图，没有参数只挑其中一个 ID。
- 不带 `FLAG_SPLIT` 时，`TouchedWindow.h` 明确说该位图为零；窗口按完整 Motion 流接收，不能把零误解为“没有指针”。

其他 `state.windows` 条目（包括别的 split 目标与随流锁定的 wallpaper）、gesture monitor 和 portal window 都留在原状态中；代码不会依据 `to.hasWallpaper` 重建目标集合。它也不验证 `from` 必须带 `FLAG_FOREGROUND`：若命中的 `from` 本身是 wallpaper 条目，实现照样迁移它。因此“只迁当前前台窗口”是比源码更强的说法。

transfer 也不会调用 `toWindowHandle->getInfo()->supportsSplitTouch()` 重新判断能力。源条目带 `FLAG_SPLIT` 时，这一位直接贡献给 `to`，即使目标窗口没有声明 split；源条目不带时，目标仅因自身支持 split 也不会从本次迁移新增该位。当然，`to` 若早已在 `TouchState` 中带 split，OR 合并会保留它。系统调用者必须保证显式目标与要承接的流相容。

### 练习 3：验证删除、掩码与目标合并

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'int32_t oldTargetFlags = touchedWindow.targetFlags;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'BitSet32 pointerIds = touchedWindow.pointerIds;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.windows.erase(state.windows.begin() + i);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_FOREGROUND | InputTarget::FLAG_SPLIT |' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.addOrUpdateWindow(toWindowHandle, newTargetFlags, pointerIds);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (targetFlags & InputTarget::FLAG_SPLIT) {' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'touchedWindow.targetFlags |= targetFlags;' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'touchedWindow.pointerIds.value |= pointerIds.value;' frameworks/native/services/inputflinger/dispatcher/TouchState.cpp
grep -n -F 'BitSet32 pointerIds; // zero unless target flag FLAG_SPLIT is set' frameworks/native/services/inputflinger/dispatcher/TouchedWindow.h
grep -n -F 'newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'targetFlags |= InputTarget::FLAG_SPLIT;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

手算两个场景：A 独占两指后迁给空的 B；A 持 id0、B 持 id1 后 A→B。分别写出迁移后的窗口条目和位图。

## 6. InputState：为何改完路由还不能直接发 MOVE

`TouchState` 只回答“下一条真实事件发给谁”。若 B 从未见过 `DOWN`，仅把下一条 `MOVE` 路由到 B，`InputState::trackMotion()` 找不到非 hover memento，会把该 `MOVE` 判为不一致并跳过；客户端自己的手势状态机也无法理解无起点的移动。

每个 `Connection` 因此维护独立 `InputState`：

- `KeyMemento`：按键、fallback 关系；
- `MotionMemento`：pointer 或其他 Motion 流的当前快照；
- dispatcher 已经承诺给当前连接的输入序列状态。

`MotionMemento` 保存 device、source、display、resolved flags、精度、cursor position、原始 `downTime`、pointer properties、最后一组 coords、hovering 和 policy flags。它不保存整段历史样本，也不保存原始事件的每个附属字段。

迁移只调用 `mergePointerStateTo()`，因此 Key memento 与 fallback key 不会转移。更需要注意的是，它遍历 `fromConnection` 中所有 `source & AINPUT_SOURCE_CLASS_POINTER` 的 Motion memento，没有按本次 `TouchState.pointerIds`、device 或 display 再过滤。正常窗口与手势生命周期让这些状态通常对应当前流；从静态实现看，连接上并存的其他 pointer-class 状态也在合并与随后取消的范围内。

由此形成一个范围不对称：路由层只替换全表搜索先命中的一份 `TouchedWindow`，连接层却合并并取消该 Connection 跨 device/source/display 的全部 pointer-class memento。常规系统调用依赖“目标 token 对应正在接管的那条流”等前提；函数本身没有用 display 参数把两层范围收窄到同一份状态。

## 7. mergePointerStateTo：合并规则与 firstNewPointerIdx

对每个源 pointer memento，目标端按且仅按：

```text
deviceId + source + displayId
```

寻找匹配项。匹配键不含 `hovering`。

若目标没有匹配项，代码先直接把源 memento 的字段写成：

```text
firstNewPointerIdx = 0
```

再把它复制进目标列表。这表示目标对其中每个 pointer 都未知，稍后要从首个 `DOWN` 补起；同时也意味着源对象暂时被写入了同样的分界。正常旧端 `CANCEL` 入队会删掉源 memento，但旧 Connection 已 broken 而取消 helper 跳过时，源侧可残留 `firstNewPointerIdx = 0`。

若目标已有匹配项且其 `firstNewPointerIdx < 0`，代码在第一次追加前记录：

```text
firstNewPointerIdx = target.pointerCount
```

已有有效分界时不会重写它；随后把源端所有 `PointerProperties/PointerCoords` 追加到目标数组。此时目标原有 memento 的 flags、精度、cursor、`downTime`、hovering 与 policy flags 保持不变；源端只贡献 pointer 数组。这一点在 split 汇流时很重要：补出的 `POINTER_DOWN` 使用目标既有流的外围元数据。

`firstNewPointerIdx` 不是 pointer ID，也不是编码后的 action index，而是数组分界：

```text
[0, firstNewPointerIdx)       目标已知
[firstNewPointerIdx, count)   刚追加、需要补起点
```

merge 不删除源 memento：有匹配项时只追加到目标，无匹配项时先改源分界再复制。它也不去重 pointer ID、不单独做 `MAX_POINTERS` 容量检查。合法 split 状态依赖各窗口的 pointer 集合互斥，合并后总量仍不超过真实事件的 pointer 数；不能把这些调用前不变量误写成函数内部校验。

### 练习 4：核对匹配键、追加内容与数组分界

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'int32_t firstNewPointerIdx = INVALID_POINTER_INDEX;' frameworks/native/services/inputflinger/dispatcher/InputState.h
grep -n -F 'void InputState::MotionMemento::mergePointerStateTo(MotionMemento& other) const {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'if (other.firstNewPointerIdx < 0) {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'other.firstNewPointerIdx = other.pointerCount;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'other.pointerProperties[other.pointerCount].copyFrom(pointerProperties[i]);' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'other.pointerCoords[other.pointerCount].copyFrom(pointerCoords[i]);' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'other.pointerCount++;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.flags = flags;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.hovering = hovering;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.policyFlags = entry.policyFlags;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'if (memento.source & AINPUT_SOURCE_CLASS_POINTER) {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.deviceId == otherMemento.deviceId &&' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.source == otherMemento.source &&' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.displayId == otherMemento.displayId) {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.mergePointerStateTo(otherMemento);' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.firstNewPointerIdx = 0;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
```

用“目标已有 id1、源端有 id0”走一遍数组。答案应是 `[id1, id0]`、分界为 1；数值较小的 id0 不会被排序到前面。

## 8. 旧端收尾：CANCEL 从当前快照合成

合并完成后，dispatcher 对旧连接构造：

```text
CancelationOptions(
    CANCEL_POINTER_EVENTS,
    "transferring touch focus from this window to another window")
```

这里没有设置 `deviceId` 或 `displayId` 过滤器。`shouldCancelMotion()` 会选中旧连接上的全部 pointer-class memento：非 hover 流合成 `ACTION_CANCEL`，hover 流合成 `HOVER_EXIT`。因此“只取消 `TouchedWindow.pointerIds` 中的指针”也比 r48 实现更窄。

合成事件来自 memento 的最后快照：

- `eventTime = now()`，事件 ID 由 dispatcher 新生成；
- 沿用 device/source/display、flags、精度、cursor、`downTime`、pointer 数组与 policy flags；
- actionButton、metaState、buttonState、classification、edgeFlags、x/y offset 使用构造器给出的中性值。

`mergePointerStateTo()` 没有删除旧状态。`synthesizeCancelationEvents()` 先生成事件列表；随后 `enqueueDispatchEntryLocked()` 对 `CANCEL/HOVER_EXIT` 调用 `trackMotion()`，匹配的旧 memento 才被移除。这样生成取消事件时仍能读到完整快照。

若旧 Connection 状态恰为 `STATUS_BROKEN`，helper 在生成事件前直接返回，旧端不会收到取消。该结果不会回传成 transfer 的 `false`。

## 9. 新端起步：DOWN/POINTER_DOWN 怎样补齐

`synthesizePointerDownEvents()` 遍历目标端全部 pointer-class memento，但只处理 `firstNewPointerIdx >= 0` 的项。

它不检查 memento 的 `hovering`。纯 hover 目标在普通分发收尾时不会作为 window 条目留在 `TouchState`，通常不能单独满足 from-in-TouchState；真正的静态边界是：迁移一条正在按下的 touch 时，`fromConnection` 若还并存其他 pointer-class hover memento，连接级全量 merge 也会把它带走。旧端取消 helper 为它生成 `HOVER_EXIT`，目标端这里却会按下面的规则补 `DOWN`。

它先复制目标已知的前缀，然后逐个加入未知后缀：

```text
加入后 pointerCount == 1
→ ACTION_DOWN

加入后 pointerCount > 1
→ ACTION_POINTER_DOWN | (数组位置 << INDEX_SHIFT)
```

因此目标为空且源端有两指时，会依次得到：

```text
DOWN(pointerCount=1, actionIndex隐含为0)
POINTER_DOWN(pointerCount=2, actionIndex=1)
```

目标已经知道一指、源端再追加一指时，不会重复 `DOWN`，只补一个 `POINTER_DOWN`。action index 来自合并后的数组位置，不来自 pointer ID 数值。

为某个 MotionMemento 生成完全部补事件后，代码把它的 `firstNewPointerIdx` 复位为 `INVALID_POINTER_INDEX`；整个函数先生成并复位各 memento，再把事件 vector 返回给 dispatcher 逐条入队。入队时 `trackMotion()` 用 `DOWN/POINTER_DOWN` 重建目标端正常状态，随后真实 `MOVE/UP` 才能通过一致性检查。

若目标 Connection 已是 `STATUS_BROKEN`，helper 会在调用 `synthesizePointerDownEvents()` 前返回，分界也不会被消费；但 transfer 仍然返回 `true`。

### 练习 5：从 memento 推导补事件

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'synthesizeCancelationEventsForConnectionLocked(fromConnection, options);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'synthesizePointerDownEventsForConnectionLocked(toConnection);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'const int32_t action = memento.hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F ': AMOTION_EVENT_ACTION_CANCEL;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'std::vector<EventEntry*> InputState::synthesizePointerDownEvents(nsecs_t currentTime) {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'if (memento.firstNewPointerIdx < 0) {' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'const int32_t action = (pointerCount <= 1)' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F '? AMOTION_EVENT_ACTION_DOWN' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F ': AMOTION_EVENT_ACTION_POINTER_DOWN' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'memento.firstNewPointerIdx = INVALID_POINTER_INDEX;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'case CancelationOptions::CANCEL_POINTER_EVENTS:' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'return memento.source & AINPUT_SOURCE_CLASS_POINTER;' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'action, 0 /*actionButton*/, memento.flags, AMETA_NONE,' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F '0 /*buttonState*/, MotionClassification::NONE,' frameworks/native/services/inputflinger/dispatcher/InputState.cpp
grep -n -F 'if (connection->status == Connection::STATUS_BROKEN) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

分别为“目标空、源端一指”“目标空、源端三指”“目标已有两指、源端一指”写出每条事件的 pointerCount 和 action index。

## 10. 三条时间线：单指、多指与 split 汇流

第一条：单指、目标原本为空。

```text
真实：A ← DOWN(id0)
迁移：A ← CANCEL(id0)
      B ← DOWN(id0)
真实：B ← UP(id0)
```

第二条：两指非 split、目标原本为空。

```text
真实：A ← DOWN(id0), POINTER_DOWN(id0,id1)
迁移：A ← CANCEL(id0,id1)
      B ← DOWN(id0), POINTER_DOWN(id0,id1; index=1)
真实：B ← POINTER_UP, UP
```

第三条：两窗口 split 后汇流。测试让 A 的区域先得到 id0，再让 id1 落到 B：

```text
A 已知 [id0]；第二指按下时 A 看到局部 MOVE
B 已知 [id1]；它把自己的第一指看成 DOWN
A → B
A ← CANCEL(id0)
B ← POINTER_DOWN([id1,id0]; index=1)
后续两个 pointer 都归 B
```

B 的数组原有 id1 在前，追加 id0 在后；源码与测试都没有按 pointer ID 排序。业务代码应以 ID 识别 pointer 身份，不应把 index 长期当作稳定身份。

三条时间线只表达每个 Connection 内部必须成立的局部协议；把 A 的 `CANCEL` 写在 B 的补 `DOWN` 前，不声明两个客户端实际观察到它们的先后。跨连接顺序留到第 12 节结账。

### 练习 6：让测试约束外部可观察协议

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'TEST_F(InputDispatcherTest, TransferTouchFocus_OnePointer) {' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'TEST_F(InputDispatcherTest, TransferTouchFocus_TwoPointerNoSplitTouch) {' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'TEST_F(InputDispatcherTest, TransferTouchFocus_TwoPointersSplitTouch) {' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'mDispatcher->transferTouchFocus(firstWindow->getToken(), secondWindow->getToken());' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'firstWindow->consumeMotionCancel();' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'secondWindow->consumeMotionDown();' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'secondWindow->consumeMotionPointerDown(1);' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'firstWindow->consumeMotionMove();' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'secondWindow->consumeMotionPointerUp(1);' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
grep -n -F 'secondWindow->consumeMotionUp();' frameworks/native/services/inputflinger/tests/InputDispatcher_test.cpp
```

将三组测试转成“迁移前状态—补偿事件—迁移后真实事件”表格。注意 native 测试关注事件协议，并未把客户端处理完成当作 transfer 返回条件。

## 11. 坐标与字段：补事件不是历史回放

补出的事件不是原始 `DOWN` 的副本。它用“当前最后快照”重建协议起点：

| 维度 | 目标无匹配 memento | 目标已有匹配 memento |
|---|---|---|
| pointer properties / coords | 从源 memento 复制 | 追加源 pointer 到目标数组 |
| device/source/display | 复制源值 | 保留目标值；三者本来就是匹配键 |
| flags、精度、cursor、`downTime`、policy flags | 复制源值 | 保留目标既有值 |
| `eventTime` | 合成时的 `now()` | 合成时的 `now()` |
| actionButton、meta、button、classification、edge | 中性值 | 中性值 |

所以 `eventTime` 与 `downTime` 可以相差很久；迁移表达的是“目标从现在开始观察一条早已开始的流”。

pointer coords 仍以 MotionEntry 快照保存。合成 helper 根据连接 token 重新取得目标窗口，并把 `-frameLeft/-frameTop`、`windowXScale/windowYScale` 与 `globalScaleFactor` 写入 `InputTarget`；发布时再把这些目标变换带给目标连接。transfer 本身不按坐标重新 hit-test，也不会把指针钳制进 `to.touchableRegion`。

还要把两类 flags 分开：第 5 节的三位掩码只约束未来路由的 `TouchedWindow.targetFlags`；补事件的 Motion flags 来自 memento。目标无匹配项时，源端已解析的 flags 会随整个 memento 复制，其中可能包含旧窗口视角下的 obscured 位；目标已有匹配项时则沿用目标自己的 memento flags。合成路径不会按新窗口重新计算这些位。

`hovering` 留在 memento 中供状态匹配与取消动作选择，却不是补 `MotionEntry` 的构造字段。目标已有匹配项时它仍保留在 memento，`synthesizePointerDownEvents()` 则完全不看它；不能把“memento 保留 hovering”写成“补事件携带 hovering”。

与第 237 章的验证链相接：合成的首个 `ACTION_DOWN` 走正常 Motion 发布路径，resolved action 为 `DOWN` 时可以得到 dispatcher 的 HMAC；`POINTER_DOWN` 与 `CANCEL` 不在 r48 Motion 签名动作集合中。签名说明事件由输入系统封装，不会把这个合成 `DOWN` 变成硬件原始历史的重放。

## 12. 入队、发布与回执：原子性止于哪里

旧端与新端 helper 都构造只带 `FLAG_DISPATCH_AS_IS` 的 `InputTarget`，直接调用 `enqueueDispatchEntryLocked()`：

```text
create DispatchEntry
→ trackMotion 更新 Connection.inputState
→ push outboundQueue
→ startDispatchCycleLocked 尝试 publish
→ 成功后移入 waitQueue
→ 客户端 finish signal 再释放
```

这带来四个边界。

第一，`trackMotion()` 发生在 publish 之前；`InputState` 是 dispatcher 的连接协议账，不是客户端执行进度。

第二，整个 TouchState 改写、merge、旧端合成以及新端合成都在 `mLock` 内。正常 dispatcher 线程看不到只完成一半的内部状态；而 helper 会在锁内立即尝试发布，不是等函数末尾的 `mLooper->wake()` 才开始。

这把切换点定义在 dispatcher 的加锁处理顺序上，而不是硬件 `eventTime` 上。已经放进旧 Connection 的 outbound/wait 项不会被搜索、撤回或改写，合成取消排在该连接已有 outbound 项之后；反过来，一个时间戳更早、但仍在 inbound 阶段且尚未选目标的 MotionEntry，可以在 transfer 之后按新 `TouchState` 路由。

第三，旧端 helper 先运行，新端 helper 后运行，只能保证 dispatcher 的调用顺序。两条事件进入不同 InputChannel，两个客户端何时调度、谁先处理，没有跨连接全序保证；每个连接内部仍保持自己的 FIFO。即使旧端 pipe 暂时阻塞，新端 helper 仍会继续执行，因此 B 可能先观察到补 `DOWN`，A 才稍后观察到 `CANCEL`。

第四，函数不创建 `InjectionState`，也不提供 `WAIT_FOR_FINISH` 模式。合成目标 flags 没有 `FLAG_FOREGROUND`，不会挂接某次注入请求的 foreground pending 计数；但成功发布的普通 DispatchEntry 仍进入 `waitQueue`、需要 finish signal，也可能参与连接超时诊断。

合成首个 `DOWN` 还有一条控制面副作用：`enqueueDispatchEntryLocked()` 会调用 `dispatchPointerDownOutsideFocus()`，后者在目标不是当前 focused token 时投递策略命令。策略回调解锁后进入 `onPointerDownOutsideFocus()`；WMS 若找到可接收按键的窗口，可把 Display 移到顶层并处理 task focus。这个异步分支不改变 transfer 的返回条件，却解释了为何“函数不直接改焦点”仍不能推导“焦点状态必定不变”。

只有非同 token 且完成 TouchState 改写的分支会走到解锁后的 `mLooper->wake()`；同 token 快返和三个 `false` 出口都提前离开。两个 helper 在锁内已经调用 `startDispatchCycleLocked()`，所以 wake 既不是补事件存在的证明，也不是发布完成点；它只是让 loop 及时处理新的分发选择和已投递命令。

### 练习 7：追到 outbound、publish 与 waitQueue

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'enqueueDispatchEntryLocked(connection, cancelationEventEntry,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'enqueueDispatchEntryLocked(connection, downEventEntry,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->inputState.trackMotion(motionEntry, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->outboundQueue.push_back(dispatchEntry.release());' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'void InputDispatcher::startDispatchCycleLocked(nsecs_t currentTime,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '.publishMotionEvent(dispatchEntry->seq,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->waitQueue.push_back(dispatchEntry);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'connection->inputPublisher.receiveFinishedSignal(&seq, &handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'd->finishDispatchCycleLocked(currentTime, connection, seq, handled);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (dispatchEntry->hasForegroundTarget()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if ((actionMasked == AMOTION_EVENT_ACTION_UP) || (actionMasked == AMOTION_EVENT_ACTION_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'dispatchPointerDownOutsideFocus(motionEntry.source, dispatchEntry->resolvedAction,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F '&InputDispatcher::doOnPointerDownOutsideFocusLockedInterruptible);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'mPolicy->onPointerDownOutsideFocus(commandEntry->newToken);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'handleTaskFocusChange(touchedWindow.getTask());' frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java
```

沿一条合成 `DOWN` 标出“协议状态更新、outbound、实际 publish、waitQueue、finish”五个时点。分别对立即 publish、`WOULD_BLOCK`、无合成事件或 broken 三支定位 transfer 的返回，并说明为何不存在统一的客户端完成点。

## 13. 失败与竞态：true 不等于强交付

最稳妥的返回值定义是：

```text
true =
  from == to 的无操作快返
  或
  两个窗口存在且同 Display、from 条目被找到并完成 TouchState 改写
```

它不承诺补偿事件已送达。找到 `from` 后，代码才查询两个 Connection；只有两者都非空，才执行 merge、CANCEL 与 DOWN。任一为空时，三步全部跳过，路由不回滚，函数仍返回 `true`。

即便两个 Connection 都存在，源端没有可迁移的 pointer memento 时，merge 为空，两个合成列表也可能为空；`true` 仍只描述前面的路由结果。入队阶段若 `trackMotion()` 判定事件不一致，也可能不产生对应 `DispatchEntry`。

两者都存在也不是事务性强交付：

- 源端存在 pointer-class memento 时，merge 会修改目标 `InputState`；无匹配分支还会先把该源 memento 的 `firstNewPointerIdx` 写成 0；
- 目标补事件 vector 生成时已消费各 memento 的 `firstNewPointerIdx`，后续入队或发布失败不会自动恢复该分界；
- 任一 Connection 为 `STATUS_BROKEN` 时，对应合成 helper 直接跳过；
- pipe 写入失败可能在 `startDispatchCycleLocked()` 中把连接标为 broken；
- paused、`responsive == false`、touchable region 与 owner UID 都不在这里预检；
- 函数忽略 helper 的发布结果，没有回滚 TouchState 或另一连接已经排入的事件。

窗口快照与连接表在本函数持锁期间不会被并发改一半，但调用前后仍有生命周期竞态：`to` 尚未同步到 dispatcher 会返回 `false`；window handle 仍在而 channel 已移除，则可能命中“路由改写成功、Connection 缺失”的 `true`。

安全边界也要按证据表述。在非同 token 的实质迁移分支，native 要求 source token 能映射到当前窗口且确实存在于 `TouchState.windows`，并要求目标同 Display；它没有做调用者 UID 授权。IMMS 的 r48 实现验证目标是当前 Display 的非空 IME host token，却留有编号 `b/150843766` 的 source token 有效性检查注释。这个事实应写成“上层来源校验尚不完整”，不能脱离 token 可达性与其余检查直接推导攻击结论。

## 14. 三类系统接管者：拖放、窗口定位与内联建议

拖放路径先创建 `drag` InputChannel pair：server 端注册给 dispatcher，client 端交给 `DragInputEventReceiver`。`DragState` 创建对应 `InputWindowHandle`，把它挂到顶层 input surface，并调用 `syncInputWindows()` 后再 transfer。若迁移失败，`performDrag()` 返回失败，`finally` 路径关闭尚未进入进行态的 `DragState`。

`DragState` 把 `touchableRegion` 设为空，并注释其不能接收新触摸；但 r48 的 hit-test 还要结合 flags。该窗口 `layoutParamsFlags = 0`，`findTouchedWindowAtLocked()` 会把它判断为 touch-modal，而 touch-modal 分支可绕过 region 命中。可靠结论是“当前流由 transfer 显式接管”；空 region 本身不足以排除普通新 `DOWN`，短生命周期和清理时序同样重要。

`TaskPositioner` 也创建 server/client pair、注册专用 handle、显示并同步 input surface。`TaskPositioningController` 优先从主窗口迁移；若当前焦点是同一 Activity 的另一个窗口，则改从该窗口迁移。代码只比较 current focus、窗口身份与 `mActivityRecord`，没有在这里另做 Z-order 判断。失败时先清理 positioner，成功后才 `startDrag()`。

内联建议路径更长：

```text
InlineSuggestionRoot 检测到 FLAG_WINDOW_IS_PARTIALLY_OBSCURED，或移动超过 touch slop
→ 用 ViewRoot input token + displayId 回调
→ Autofill system_server connector
→ InputMethodManagerInternal
→ 当前 IME host input token
→ transferTouchFocus(source, host)
```

IMMS 拒绝 display 不等于当前 IME token display、或 host token 为空的请求；最终失败还会触发 Autofill UI 的 error callback。

这里还有一个上层快照竞态：IMMS 在 `mMethodMap` 锁内检查并复制 `mCurHostInputToken`，释放锁后才调用 `mInputManagerInternal.transferTouchFocus()`。当前 IME host 可在两步之间变化；旧 token 若已从 dispatcher 移除，native 会失败，若仍有效则可能成为已经过期的接管目标。代码没有在调用后重新确认它仍是当前 host。

### 练习 8：核对三个调用者的准备与失败路径

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'InputChannel[] channels = InputChannel.openInputChannelPair("drag");' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mService.mInputManager.registerInputChannel(mServerChannel);' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragWindowHandle.layoutParamsFlags = 0;' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mDragWindowHandle.touchableRegion.setEmpty();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'mTransaction.syncInputWindows();' frameworks/base/services/core/java/com/android/server/wm/DragState.java
grep -n -F 'bool isTouchModal = (flags &' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (isTouchModal || windowInfo->touchableRegionContainsPoint(x, y)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'return service.transferTouchFocus(source, state.getInputChannel());' frameworks/base/services/core/java/com/android/server/wm/WindowManagerInternal.java
grep -n -F 'Slog.e(TAG_WM, "Unable to transfer touch focus");' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'if (mDragState != null && !mDragState.isInProgress()) {' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'mDragState.closeLocked();' frameworks/base/services/core/java/com/android/server/wm/DragDropController.java
grep -n -F 'final InputChannel[] channels = InputChannel.openInputChannelPair(TAG);' frameworks/base/services/core/java/com/android/server/wm/TaskPositioner.java
grep -n -F 'mService.mInputManager.registerInputChannel(mServerChannel);' frameworks/base/services/core/java/com/android/server/wm/TaskPositioner.java
grep -n -F 'mDragWindowHandle = new InputWindowHandle(mDragApplicationHandle,' frameworks/base/services/core/java/com/android/server/wm/TaskPositioner.java
grep -n -F 'mService.mTaskPositioningController.showInputSurface(win.getDisplayId());' frameworks/base/services/core/java/com/android/server/wm/TaskPositioner.java
grep -n -F 'mTransaction.syncInputWindows().apply();' frameworks/base/services/core/java/com/android/server/wm/TaskPositioningController.java
grep -n -F 'if (!mInputManager.transferTouchFocus(' frameworks/base/services/core/java/com/android/server/wm/TaskPositioningController.java
grep -n -F 'cleanUpTaskPositioner();' frameworks/base/services/core/java/com/android/server/wm/TaskPositioningController.java
grep -n -F 'mCallback.onTransferTouchFocusToImeWindow(getViewRootImpl().getInputToken(),' frameworks/base/core/java/android/service/autofill/InlineSuggestionRoot.java
grep -n -F 'mHandler.post(() -> handleOnTransferTouchFocusToImeWindow(sourceInputToken, displayId));' frameworks/base/services/autofill/java/com/android/server/autofill/ui/RemoteInlineSuggestionUi.java
grep -n -F 'if (!inputMethodManagerInternal.transferTouchFocusToImeWindow(sourceInputToken,' frameworks/base/services/autofill/java/com/android/server/autofill/ui/RemoteInlineSuggestionViewConnector.java
grep -n -F 'mOnErrorCallback.run();' frameworks/base/services/autofill/java/com/android/server/autofill/ui/RemoteInlineSuggestionViewConnector.java
grep -n -F 'if (displayId != mCurTokenDisplayId || mCurHostInputToken == null) {' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'curHostInputToken = mCurHostInputToken;' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
grep -n -F 'return mInputManagerInternal.transferTouchFocus(sourceInputToken, curHostInputToken);' frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java
```

为 Drag 与 TaskPositioner 各列出“创建通道—建立 handle—同步窗口—执行迁移—失败清理”五格。内联建议则列“source token 来源—host token 快照—display/空值检查—执行迁移—错误回调”，并写明它复用当前 IME host token。

## 15. 与 pilfer、slippery、split 的分界

四种机制都会改变 pointer 的可见方式，但触发者和状态修复不同：

| 机制 | 触发 | 新接收者怎样确定 | 旧端怎样结束 | 新端为何有合法起点 |
|---|---|---|---|---|
| `transferTouchFocus` | 系统显式传 from/to token | 指定 `to` 窗口 | touch memento 合成 `CANCEL`，hover memento 合成 `HOVER_EXIT` | merge 后补 `DOWN/POINTER_DOWN` |
| `pilferPointers` | 已注册且已参与当前流的 gesture monitor 调用 | 该 monitor 已固定在 `gestureMonitors` | 逐个取消 `state.windows`，再 `filterNonMonitors()` | monitor 从最初 `DOWN` 就收到副本，无需补 |
| slippery | 单指 `MOVE` 越出唯一 slippery 前台窗口 | 用当前坐标重新 hit-test | 同一真实 `MOVE` 派生 slippery exit / `CANCEL` | 同一 `MOVE` 派生 slippery enter / `DOWN` |
| 普通 split touch | 新 pointer 按下 | 按新 pointer 坐标命中支持 split 的窗口 | 原窗口仍持有自己的 pointer | 新目标把自己的第一指看作局部 `DOWN` |

transfer 不清空 gesture monitors，也不移除同一份 TouchState 中其他 split 窗口条目；其 Connection 级取消范围仍服从第 6 节的全 pointer-memento 边界。pilfer 则清空普通窗口和 portal 路由、保留 monitor 集合。transfer 不要求 `FLAG_SLIPPERY`、不依赖当前坐标，还能处理多指；slippery 的 r48 分支明确要求 `MOVE`、`pointerCount == 1` 和唯一 slippery foreground。

### 练习 9：用源码条件区分四种改路由机制

```bash
set -eu
ROOT=${1:-/Users/ninebot/androidSource}
cd "$ROOT"
grep -n -F 'status_t InputDispatcher::pilferPointers(const sp<IBinder>& token) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'findGestureMonitorDisplayByTokenLocked(token);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (!foundDeviceId || !state.down) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'state.filterNonMonitors();' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (maskedAction == AMOTION_EVENT_ACTION_MOVE && entry.pointerCount == 1 &&' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'tempTouchState.isSlippery()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'findTouchedWindowAtLocked(displayId, x, y, &tempTouchState);' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_DISPATCH_AS_SLIPPERY_EXIT,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'InputTarget::FLAG_FOREGROUND | InputTarget::FLAG_DISPATCH_AS_SLIPPERY_ENTER;' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'if (newGesture || (isSplit && maskedAction == AMOTION_EVENT_ACTION_POINTER_DOWN)) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'newTouchedWindowHandle->getInfo()->supportsSplitTouch()) {' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
grep -n -F 'MotionEntry* InputDispatcher::splitMotionEvent(const MotionEntry& originalMotionEntry,' frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp
```

给定“monitor 抢占”“单指越界”“新 pointer 按坐标进入另一 split 窗口”“A/B 已 split 后显式 A→B”四个日志场景，只依据触发条件、是否 hit-test、是否补 `DOWN` 判断使用了哪种机制。

## 16. 调试闭环与本章结论

遇到“transfer 返回成功但新端没有连续事件”时，按状态层级排查：

```text
入口
[ ] 两 token 是否 null、相同，是否拿的是 InputChannel connection token

窗口与路由
[ ] 两个 InputWindowHandle 是否已同步到 dispatcher
[ ] handle 是否同 Display
[ ] from 是否仍在某个 TouchState.windows
[ ] from 的 flags / pointerIds 是什么，to 是否已有条目

连接协议
[ ] 两个 Connection 是否存在、是否 broken
[ ] fromConnection 上实际有多少 pointer-class memento
[ ] to 是否已有相同 device/source/display 的 memento
[ ] firstNewPointerIdx 是 0、旧 count，还是无效值

队列与客户端
[ ] from 的 CANCEL 与 to 的 DOWN 是否进入各自 outbound/waitQueue
[ ] 是否发生 pipe 错误、连接超时或客户端未 finish
[ ] 后续真实 MOVE/UP 是否按新 TouchState 到达 to
```

最后把整章压成一句话：

```text
transferTouchFocus =
  在 dispatcher 锁内迁移未来路由
  + 用 Connection InputState 修复两端各自的 pointer 协议视角
```

“锁内”只保证 dispatcher 的状态变更与排队/发布尝试不被自身并发观察成半成品；它不保证两个进程同时看到事件，也不保证客户端已处理，更不为缺失或 broken 的 Connection 提供回滚。掌握这条边界，才能正确解释拖放与窗口定位为何先建通道、同步 InputWindowHandle，再把现有触摸流交给系统接管者。

下一章进入第 239 章：从 `View.startDragAndDrop()` 与 WMS `performDrag()` 出发，继续追 `DragState`、`DragEvent` 跨窗口分发、DROP 结果、URI 权限与结束清理。
