# 209 Android LaunchActivityItem：实例化 Activity 与 Context、Window 组装

> 源码基线：Android 11 / API 30 / `android-11.0.0_r48`。
>
> 当前环境只有源码快照，可以证明对象建立顺序、同步调用点、生命周期状态转换与窗口提交边界；不能据此测出某台设备的真实启动耗时，也不能把 `onCreate()` 返回、`WindowManager.addView()` 返回或客户端 `activityResumed()` 回报直接解释成首帧已经呈现。

第 208 章停在 App 主线程开始分派 `H.EXECUTE_TRANSACTION`：`handleBindApplication()` 已完成，`LaunchActivityItem` 所在事务也已经从 Binder 接收账进入主队列。本章继续追同一个目标 `B_target`，直到业务 `Activity`、Activity Context 与 `PhoneWindow` 在客户端完成组装，并划清它们与 `DecorView`、`ViewRootImpl`、WMS `WindowState`、Surface 和首帧之间的边界。

本章只追一个问题：**固定普通 App 的 `LaunchActivityItem.execute()` 已经开始；当开发者看到 `onCreate()` 末尾日志时，客户端创建链究竟交付到了哪个完成点？** 答案必须同时解释三件事：哪些对象一定存在、哪些客户端登记还隔着一步，以及为什么 system_server 可以早已把 `ActivityRecord` 标成 `RESUMED`，WMS 却仍可能没有这个 Activity 的客户端主窗口。

## 1. 固定 B_target，把“Activity 已创建”拆成十三个完成点

沿用第 203—208 章的 `P_B` 与目标 `B_target`，先冻结主路径：

| 维度 | 固定值或前提 |
|---|---|
| 进程 | 普通远端 App 进程 `P_B`，已 attach 且 `handleBindApplication()` 正常完成 |
| 组件 | 初始包中的真实 Activity，不走 alias，显式 Intent |
| 生命周期目标 | `andResume=true`，事务最终请求是 `ResumeActivityItem` |
| Context | 默认 display、无 isolated split、无 fixed-rotation adjustment |
| Application | 第 205 章的初始 `Application` 已存在 |
| 重建 | 首次 launch，无 preserved Window 与 non-config instance |
| 回调 | 默认 Instrumentation；`onCreate()` 调用 `super`，不 finish、不启动另一 Activity |
| UI | 可见 Activity；是否在 `onCreate()` 调用 `setContentView()`暂不固定 |

alias、多 display、isolated split、relaunch、system-process 本地调用与异常都是真实路径，但它们会改变局部顺序或证据强度；后文逐一从固定主线分叉。

先定义十三个完成点：

| 点 | 精确定义 | 仍不能推出 |
|---|---|---|
| `S_submit` | 远端 App 路径中，system_server 的 launch oneway 已被 Binder 驱动接纳 | App Stub 已进入或服务端 Java 代理已返回 |
| `P_pre` | App 侧 `ClientTransaction.preExecute()` 已完成 | 主线程已取到事务 |
| `Q_tx` | `H.EXECUTE_TRANSACTION` 已入主队列 | `LaunchActivityItem.execute()` 已开始 |
| `L_exec` | 主线程开始执行 launch callback | `ActivityClientRecord` 已构造完 |
| `R_record` | 新 `ActivityClientRecord` 已返回，含 token 与 `LoadedApk` | Activity Context 或业务实例存在 |
| `C_context` | `createBaseContextForActivity()` 已返回 | Activity 构造函数已执行 |
| `N_instance` | `AppComponentFactory.instantiateActivity()` 已返回 | base Context、Application、Intent、token 或 Window 已写入实例 |
| `A_attach` | `Activity.attach()` 已返回 | `onCreate()` 已开始 |
| `O_create` | Instrumentation 的 create 调用正常返回，且 super 检查通过 | `mActivities[token]` 已登记 |
| `M_commit` | `r.activity`、客户端 `ON_CREATE` 与 `mActivities[token]` 已提交 | `onStart()` 或 `onResume()` 已执行 |
| `T_start` | executor 已完成 Start、状态恢复与 `onPostCreate()`阶段 | `onResume()` 或主窗口加入已完成 |
| `U_resume` | `performResumeActivity()` 已正常把客户端账设为 `ON_RESUME` | `WindowManager.addView()` 已执行或首帧已绘制 |
| `W_add` | 普通新窗口的 `wm.addView()` 正常返回，WMS 接受 add 请求 | 首次 traversal、buffer 提交或 SurfaceFlinger present 已完成 |

主路径的核心全序是：

```text
S_submit → P_pre → Q_tx → L_exec → R_record → C_context → N_instance
         → A_attach → O_create → M_commit → T_start → U_resume → W_add
```

对本章开头那条开发者日志，还要保留一个更细的局部关系：

```text
A_attach < 子类onCreate末尾日志 < O_create < M_commit
```

所以看到这条日志时，十三个已定义点中最新完成的是 `A_attach`；日志本身位于尚未命名的中间区间，不能提前算作 `O_create`。这条直线只属于表中固定的成功主路径。服务端 `ActivityRecord` 状态不嵌在这条客户端全序里；`DecorView` 可能在 O_create 之前由 `setContentView()` 懒创建，也可能到 W_add 前的 `getDecorView()` 才创建；preserved Window 更会绕过本次 `addView()`。因此“Activity 已创建”必须附带具体完成点。

## 2. 三个 Activity、多种 token 与七本不能合并的账

名字相近的对象分布在不同进程和层级：

| 对象 | 所在位置 | 何时出现 | 主要职责 |
|---|---|---|---|
| `ActivityRecord` | system_server | 客户端进程甚至尚不存在时即可建立 | Task/Display 层级、服务端生命周期、可见性与窗口容器 |
| `ActivityRecord.Token` / `appToken` | system_server Binder 对象，App 持其 Binder 身份 | `ActivityRecord` 建立时 | 跨进程定位同一逻辑 Activity，并作为应用窗口 token |
| `ClientTransaction` / `LaunchActivityItem` | 服务端构造；远端路径经 Parcel 到 App | `realStartActivityLocked()` | 携带一次 launch 快照与最终生命周期请求 |
| `ActivityClientRecord` | App | `LaunchActivityItem.execute()` | 客户端 Intent/state/config/生命周期账与业务实例引用 |
| `ContextImpl` | App | Activity 实例化之前 | Activity 专属资源、display、split ClassLoader、服务代理与 token |
| 业务 `Activity` 实例 | App | Factory 返回时 | 开发者生命周期与 `ContextWrapper` 外层对象 |
| `PhoneWindow` | App | `Activity.attach()` | 窗口策略、属性、回调、Decor 懒创建入口与本地 WindowManager |

`assistToken` 是 Assist/语音能力使用的另一 Binder 身份，不能与 activity token 混称。固定无 parent 路径里，activity token 同时连接 `ActivityRecord`、`ActivityClientRecord`、Activity Context 资源分组和主窗口 `LayoutParams.token`；它不是任一 Java 对象的跨进程地址。

排障时至少要分开七本账：

| 账本 | 典型字段或调用 | 它能回答什么 |
|---|---|---|
| 服务端 Activity 账 | `ActivityRecord.state`、`r.appToken`、Task/Display | system_server 希望该逻辑 Activity 到什么状态 |
| 事务账 | callback 列表、final lifecycle item、Parcel 数据 | 本次请求携带了什么 |
| 客户端 record 账 | `mActivities[token]`、`mLifecycleState` | App 已登记到哪个客户端状态 |
| Context/资源账 | `ContextImpl.mToken`、ActivityResources、Resources/ResourcesKey | 代码、资源、display 与 override config 怎样关联 |
| 实例账 | `r.activity`、`Activity.mToken/mApplication/mIntent` | 业务对象是否已组装并提交 |
| 本地窗口账 | `PhoneWindow`、`DecorView`、`mWindowAdded`、`ViewRootImpl` | App 侧窗口对象推进到哪里 |
| 服务端窗口/呈现账 | `WindowState`、Surface、buffer、present | WMS/SF 是否已有可显示内容 |

同一个 token 能关联多本账，不代表这些账同步提交。服务端可先写 `RESUMED`，客户端仍在构造函数；Activity 可已有 `PhoneWindow`，WMS 仍无该客户端主窗口对应的 `WindowState`；WMS 可已接受主窗口，屏幕仍没有该 Activity 的首帧。

## 3. 交接复盘：system_server 发送的是启动快照，不是已创建回执

`ActivityStackSupervisor.realStartActivityLocked()` 先检查 pause 门槛、把 `ActivityRecord` 关联到已有 `WindowProcessController`，处理配置与可见性，再确认 `proc.hasThread()`。固定主路径随后构造：

```java
ClientTransaction transaction = ClientTransaction.obtain(
        proc.getThread(), r.appToken);
transaction.addCallback(LaunchActivityItem.obtain(
        new Intent(r.intent), System.identityHashCode(r), r.info, ...));
transaction.setLifecycleStateRequest(
        ResumeActivityItem.obtain(isForward));
```

这里有四个容易被“跨 Binder 传对象”掩盖的细节。

第一，`new Intent(r.intent)` 在服务端主动复制 Intent。普通远端 App 还会经历 Parcel；即使 Activity 恰在 system_server 内、调用走本地接口而不 Parcel，这个显式副本仍避免客户端修改 `ActivityRecord.intent`。不能由此反推每个字段都做了同等深度的主动复制。

第二，r48 特意新建 `MergedConfiguration`，其构造器把 process 与 override configuration 复制到自身的 `Configuration` 字段。源码注释给出的原因正是 system-process Activity 可能不跨 Binder，不能依赖 Parcel 顺手制造新对象。

第三，saved state、persistent state、referrer、profiler、assist token、fixed-rotation adjustment 都可进入 item；`results` 和 `newIntents` 只有 `andResume=true` 时才随 launch 交付。`System.identityHashCode(r)` 只是本次服务端对象的 ident 提示，不是稳定业务 ID，也不能替代 Binder token。

第四，callback 是 launch，final request 才决定事务末态。`andResume=true` 选择 `ResumeActivityItem`；否则选择 `PauseActivityItem`。后者不表示客户端只执行 Create 后静置，executor 会补齐通往 Pause 所需的中间生命周期。

对固定远端 App，`IApplicationThread` 是 `oneway interface`。所以服务端 `scheduleTransaction()` 返回只证明异步事务已经提交；它不等待 `onCreate()`。随后 `realStartActivityLocked()` 甚至可立即调用 `minimalResumeActivityLocked()`，把服务端 `ActivityRecord` 写成 `RESUMED` 并完成服务端 resume bookkeeping。

```text
服务端 RESUMED
  ≠ App 已收到 transaction
  ≠ ActivityClientRecord 已存在
  ≠ Activity.onCreate 已执行
  ≠ 客户端 ON_RESUME
  ≠ WindowState 或首帧已存在
```

system-process 本地接口是重要反例：本地 Stub 调用会直接执行 `scheduleTransaction()`，所以 `preExecute()` 与主队列入队可在服务端调用返回前完成；AIDL 上写着 oneway 也不会强迫本地 Java 调用异步化。本章固定远端路径使用 `S_submit`，不能把这项证据原封不动移到本地路径。

## 4. 交接复盘：preExecute、主队列与“已销毁前取消”门槛

普通远端路径中，App Binder worker 进入 `ApplicationThread.scheduleTransaction()`，转调 `ActivityThread.this.scheduleTransaction()`。`ClientTransactionHandler` 的顺序只有两步：

```java
transaction.preExecute(this);
sendMessage(ActivityThread.H.EXECUTE_TRANSACTION, transaction);
```

`LaunchActivityItem.preExecute()` 此时做三件事：launching count 加一、更新 process state、登记 pending configuration。final `ResumeActivityItem` 也有自己的可选 process-state 预处理。它们发生在 `Q_tx` 之前，却不创建 `ActivityClientRecord`、Context 或 Activity。

“发生在 Binder 接收线程”只适用于固定远端路径的常态。测试可调用 `executeTransaction()` 立即执行，本地 IApplicationThread 也可在调用者线程预处理；可靠表述是：**preExecute 发生在真正调度或立即执行 transaction 之前，而不是无条件绑定某一种线程。**

主线程取到 `H.EXECUTE_TRANSACTION` 后调用 `TransactionExecutor.execute()`。在 callback 前还有一扇取消门：若同一 token 已在 `mActivitiesToBeDestroyed` 中，且客户端尚无对应 record，executor 会跳过整笔预销毁 transaction。于是：

```text
P_pre 与 Q_tx 已发生
  ≠ L_exec 一定会发生
  ≠ Activity 一定会被实例化
```

未被跳过时，executor 先执行 callbacks，再执行 final lifecycle state。`LaunchActivityItem` 自己没有声明 post-execution lifecycle state；它在 handler 内把新 record 提交到 `ON_CREATE`。callback 正常返回后，`postExecute()` 才把 launching count 减一；然后 final request 从客户端当前状态继续补 Start/Resume 或 Start/Resume/Pause。

这两节只负责承接第 208 章；第 209 章新增的对象组装主线从 `L_exec` 开始。`H.EXECUTE_TRANSACTION` 已 dispatch 只交付这个入口点，不是整笔 transaction 完成。

## 5. ActivityClientRecord 先聚合参数，组件名再决定实例化类

`LaunchActivityItem.execute()` 首先调用 `new ActivityClientRecord(...)`。构造器保存 token、assist token、Intent、ActivityInfo、config、state、pending result/intent 等字段，并同步调用：

```java
packageInfo = client.getPackageInfoNoCheck(
        activityInfo.applicationInfo, compatInfo);
```

这里得到的是 `LoadedApk`：它封装包的代码、资源、ClassLoader、Application 缓存与组件 Factory 环境，不是业务 Activity，也不保证这个 `LoadedApk` 已有 Application。固定初始包路径通常命中 bind 阶段建立的对象。

新 record 的 `mLifecycleState` 初值是 `PRE_ON_CREATE`；`init()` 把 `paused=false`、`stopped=false`。稍后 `r.setState(ON_CREATE)` 才会把两个兼容布尔值都改成 true，表达“尚未 Start/Resume”的位置，而不是证明真实执行过 `onPause()` 与 `onStop()`。

`performLaunchActivity()` 再决定实例化类：

1. 优先取 `r.intent.getComponent()`；为空才调用 PackageManager resolve 并写回 Intent。
2. 若 `ActivityInfo.targetActivity != null`，局部 `component` 改成 alias 的目标类。
3. Instrumentation 把这个局部 component 的 class name 交给选中的 `AppComponentFactory`。

默认 Factory 路径中，运行时类是 alias 的目标 Java 类，并由该 Factory 完成类加载与 `newInstance()`；自定义 Factory 则只承诺返回它选择的 Activity 实例。无论走哪条路径，稍后 `Activity.attach()` 都把 `mComponent = intent.getComponent()` 写回，因而仍可保留 manifest alias；`getComponentName()` 与运行时类名不必相同。

activity token 才是客户端主键：它进入 record、ContextImpl、Activity 与窗口参数，并作为 `mActivities` key。ident 来自服务端对象的 identity hash，只适合日志提示；进程重启或对象重建后不稳定，也没有 Binder 身份能力。

`R_record` 能证明 LoadedApk 查找已经返回，却不能证明 component resolve、Context 创建或任何开发者代码已经执行。

## 6. Activity Context 先于实例：display、split、资源与旋转各有独立账

`performLaunchActivity()` 在 `newActivity()` 之前调用 `createBaseContextForActivity(r)`。固定远端 App 的主线程先同步调用 ATMS `getDisplayId(r.token)`；r48 服务端在 token 找不到、stack 为空或 display 无效时都返回默认 display。`ContextImpl.createActivityContext()` 仍额外把 `INVALID_DISPLAY` clamp 到默认值，这是防御性边界，不是当前调用者通常能观察到的第四种 display 状态。

若包启用 isolated split loading，ContextImpl 按 `ActivityInfo.splitName` 取专用 split ClassLoader 与 split 路径；否则使用 `LoadedApk` 的普通 ClassLoader 与 split 资源目录。随后创建带下列性质的 `ContextImpl`：

- `mToken` 是 activity token；
- `mSplitName` 与 ClassLoader 已选定；
- 标记为 UI Context 且关联 display；
- Resources 以 res/split/overlay/library 路径、display、override config、compat、loaders 等条件建立。

这里必须拆开三个常被混成一个 key 的概念：

| 概念 | r48 中实际角色 |
|---|---|
| activity token | 在 `ResourcesManager.mActivityResourceReferences` 中索引独立 `ActivityResources` 结构 |
| `ResourcesKey` | 包含资源路径、display、override config、compat 与 loaders；**不包含 token** |
| ClassLoader | 单独参与查找/创建 `Resources` 对象；**也不是 ResourcesKey 字段** |

`createBaseTokenResources()` 先确保 token 对应的 ActivityResources 存在，再更新该 token 的 base override、rebase key，并按 key 加 ClassLoader 寻找或创建 Resources。于是可以说 token 隔离 Activity 资源更新账，却不能说 token“被塞进 ResourcesKey”。

fixed rotation 支路也不是一句“先改配置”：`handleLaunchActivity()` 先把 adjustment 设到 Application display adjustments，再处理最新 Configuration；进入 `createBaseContextForActivity()` 建好 token 资源后，又把最后一个 active adjustment 覆盖到该 token 的 display adjustments，并消费 pending 字段。所有这些都早于 Factory，所以构造期读取 display 的代码可看到过渡目标；它仍不表示窗口已创建。

到 `C_context` 时，ContextImpl 的底层服务与资源能力已经存在，但 `mOuterContext` 暂时仍是它自身。Activity 对象尚不存在，开发者也拿不到这个 Context。

## 7. AppComponentFactory 只交付 Java 实例，不交付 Android 环境

ActivityThread 从 `appContext.getClassLoader()` 取得实际 ClassLoader，再走：

```java
activity = mInstrumentation.newActivity(
        cl, component.getClassName(), r.intent);
```

Instrumentation 根据 Intent component 的包名选择 `AppComponentFactory`：包名为空或 Instrumentation 未绑定 ActivityThread 时直接用默认 Factory；`peekPackageInfo()` 未命中时则取 system Context 的 `LoadedApk` factory。默认 `instantiateActivity()` 用 ClassLoader 加载类并 `newInstance()`。

`N_instance` 只证明：

- 选中的 Factory 已返回一个 Activity；默认 Factory 路径的类加载、静态初始化、对象分配、字段初始化与构造函数已经完成；
- 自定义 Factory 有机会选择替代实例或实现构造期依赖注入，不能把默认 Factory 的反射细节外推为通用保证；
- `StrictMode.incrementExpectedActivityCount()` 随后记录预期实例数量。

它不证明：

- `ContextWrapper.mBase` 已设置；
- `mApplication`、`mIntent`、`mToken`、`mActivityInfo` 已写入；
- `PhoneWindow` 或本地 WindowManager 已建立；
- 任何生命周期回调已开始。

AppComponentFactory 的注释直接警告，返回对象尚未作为 Context 初始化，不应在这里调用依赖 Android Context 的 API。因此 Activity 构造函数适合普通 Java 字段初值；调用 `getResources()`、`getSystemService()`、`getWindow()` 或做重 I/O，都越过了这一步能保证的边界。

Factory 返回后，ActivityThread 才把 Intent extras 与 saved-state Bundle 的 ClassLoader 改成 Activity ClassLoader，并调用 `intent.prepareToEnterProcess()`。跨进程外壳到达不等于嵌套自定义 Parcelable 已用正确 loader 展开。persistent state 是受限类型的 `PersistableBundle`，源码并未在这里对它调用同样的 `setClassLoader()`。

自定义 Factory 能改变“new 出谁”，不能把 attach 提前。若 Factory 为了注入而调用尚无 base Context 的方法，问题发生在 `N_instance` 之前或之后的构造区间，不应归咎于 `onCreate()`。

## 8. Application、合并配置、资源 loader 与 preserved Window 在 attach 前汇合

第 6 节解决“实例化前，Activity Context 与资源身份是什么”；本节解决“attach 前，Context、实例、Application 与重建输入怎样最终汇合”。两处都出现 config 或资源，不代表它们是同一个完成点。

Activity 实例化尝试结束后，ActivityThread 调用：

```java
Application app = r.packageInfo.makeApplication(
        false, mInstrumentation);
```

固定初始包路径在第 205 章已经建立 `LoadedApk.mApplication`，这里直接返回缓存，既不重新 new Application，也不再次调用其 `onCreate()`。共享进程后续加载另一个尚无 Application 的 `LoadedApk` 是反例：因为传入非空 Instrumentation，`makeApplication()` 会创建对象、登记到 `mAllApplications`，并在返回前调用这个 Application 的 `onCreate()`。

接着，成功主线依次准备：

1. 从 `ActivityInfo` 加载 title；
2. 复制 `mCompatConfiguration`，再合并 Activity override config；
3. 若是保留窗口的 relaunch，取出 `mPendingRemoveWindow` 作为 preserved Window；
4. 把 Application Resources 当前的 `ResourcesLoader` 加到 Activity Resources；
5. `appContext.setOuterContext(activity)`；
6. 调用 `activity.attach(...)`。

ContextImpl 与 Activity 是合作关系，不是同一对象。底层 ContextImpl 保存资源、服务代理、包/display/token；外层 Activity 继承 ContextWrapper，开发者的 Context API 经 base delegate 下沉。`setOuterContext(activity)` 又让底层需要“外层 Context”语义时返回 Activity。

preserved Window 只用于 relaunch：新 `PhoneWindow` 可复用旧 Decor、elevation 和 app token，并在 resume 时跳过本次 `addView()`。它不是普通首次 launch 的默认优化，也不等于复用旧 Activity 实例；新 Activity 与新 PhoneWindow 仍会建立。

到调用 `attach()` 前，Activity Context、Activity 空壳和 Application 可以同时存在，但三者尚未在 Activity 字段中闭合。`setOuterContext()` 已发生也不等于 `ContextWrapper.mBase` 已接上，真正的桥从下一节第一行开始。

## 9. Activity.attach：先接 base Context，再组装 PhoneWindow 与核心字段

`Activity.attach()` 的第一条实质调用是 `attachBaseContext(context)`。Activity override 先交给 `ContextWrapper.attachBaseContext()` 设置唯一 base，再把 Autofill client 与 Content Capture options 接到新 base。此后 Activity 的 `getResources()`、`getSystemService()` 等 ContextWrapper API 才有可靠 delegate。

`attachBaseContext()` 是可覆写方法；固定主路径假设子类 override 会继续调用 super。这里没有像 `onCreate()` 那样的 `mCalled` 强制检查，不调用 super 往往会让 base 仍为空并在后续组装中失败，不能把“方法被调用”误当成 Context 已接好。

随后 attach 按程序顺序完成四组工作。

第一组是 Fragment 与窗口策略：

```java
mFragments.attachHost(null);
mWindow = new PhoneWindow(this, preservedWindow, activityConfigCallback);
mWindow.setWindowControllerCallback(...);
mWindow.setCallback(this);
mWindow.getLayoutInflater().setPrivateFactory(this);
```

还会在 `onCreate()` 前写入 manifest 的 `softInputMode` 与 `uiOptions`。`PhoneWindow` 构造本身可读取全局设置与 PackageManager feature，不应想象成完全无外部工作的空字段赋值。

第二组是 Activity 身份与宿主字段：当前 UI 线程、ActivityThread、Instrumentation、activity/assist token、ident、Application、Intent、referrer、component、ActivityInfo、title、parent、non-config 与 voice interactor。构造函数阶段缺失的 Android 环境在这里集中接入。

第三组是窗口门面：`PhoneWindow.setWindowManager()` 接收 Context 的 WindowManager、activity token、component 字符串与硬件加速位，创建绑定当前 PhoneWindow 的 local `WindowManagerImpl`；Activity 再保存 `mWindowManager`。若有 legacy parent，还会设置 container，`getActivityToken()` 也会返回 parent token；固定无 parent 主路径直接使用本 Activity token。

第四组是当前 Configuration、color mode、minimal post-processing、Autofill 与 Content Capture 选项。

alias 支路的双身份在这里落地：`mComponent = intent.getComponent()` 可保留 alias；默认 Factory 路径中实例的 Java class 是 `targetActivity`，自定义 Factory 则以其实际返回类为准。传给 WindowManager 的 app name 仍来自这个 Intent component。

`A_attach` 正常返回后，可以安全断言 base Context、Application、Intent、token、PhoneWindow 与 local WindowManager 都已写入；仍不能断言主题已应用、`onCreate()` 已执行、Decor 已生成或 WMS 收到 add 请求。

## 10. 对象层级：PhoneWindow 已存在，不等于 Decor、ViewRoot 或客户端主 WindowState 已存在

固定首次 launch 中，`new PhoneWindow(this, null, callback)` 建立的是客户端窗口策略对象。`PhoneWindow` 继承 `Window`，不是 View；普通构造器建立 LayoutInflater 等状态，却不调用 `installDecor()`。

本地对象层级要逐层辨认：

```text
Activity
  └─ PhoneWindow                 A_attach 前建立
       ├─ WindowManagerImpl      A_attach 内建立的本地门面
       └─ DecorView              setContentView/getDecorView 时懒创建
            └─ ViewRootImpl      WindowManagerGlobal.addView 时创建
                 └─ IWindow      交给 WMS 建 WindowState
                      └─ Surface/buffer/合成/present
```

`Window.setWindowManager()` 的源码注释特意说明：这个 manager 供 Window 添加 panel/subwindow，本身不负责显示 Activity 主 Window；主窗口必须由客户端稍后显式 `addView()`。因此持有 `mWindowManager` 不是 WMS 完成点。

`handleLaunchActivity()` 在实例化前还有两个容易误判的预热：

- 满足硬件加速且 renderer 未禁用时，`HardwareRenderer.preload()` 的 native 实现通过 `RenderThread::getInstance()` 创建并启动 RenderThread，再把 EGL/Vulkan driver preload 投到其队列；方法返回不证明异步 preload 已跑完，更不证明存在某个窗口的 `CanvasContext` 或一帧。
- `WindowManagerGlobal.initialize()` 首次调用会取得 WMS 代理，并同步查询 animator scale 与 BLAST 使用状态；它不打开本 Activity 的窗口。IWindowSession 通常到构造 `ViewRootImpl`、调用 `getWindowSession()` 时才按需建立，当然也可能已被进程中其他窗口缓存。

若 `onCreate()` 调用 `setContentView()` 或主动 `getDecorView()`，Decor 可在 O_create 之前出现；若不触发，resume 路径会通过 `getDecorView()` 确保它存在。Decor 是本地 View 根容器，也仍不等于 ViewRoot、WindowState 或 Surface。

## 11. network gate、主题与 Instrumentation 怎样围住 onCreate

`Activity.attach()` 返回后，`performLaunchActivity()` 的顺序不是直接调用开发者代码：

```text
可选 customIntent 替换
→ 清 lastNonConfigurationInstances
→ checkAndBlockForNetworkAccess
→ mStartedActivity = false
→ setTheme(ActivityInfo theme)
→ mCalled = false
→ Instrumentation.callActivityOnCreate
→ 检查 mCalled
```

若 AMS 此前通过独立的 `IApplicationThread.setNetworkBlockSeq(procStateSeq)` 写入了 `mNetworkBlockSeq`，`checkAndBlockForNetworkAccess()` 会同步调用 AMS `waitForNetworkStateUpdate()`；否则立即返回。它位于主题和 `onCreate()` 之前，是合法的主线程等待点。

主题在 attach 之后、create 之前应用。因此 `onCreate()` 中读取 styled attribute 或 `setContentView()` 通常已使用 manifest 选择的主题；但 PhoneWindow 早已建立，Decor 则仍可不存在。

默认 Instrumentation 的 create 调用依次是 `prePerformCreate()`、`activity.performCreate()`、`postPerformCreate()`。`performCreate()` 又按顺序：

- 分发 pre-created callback；
- 初始化多窗口/PiP 标志并恢复权限请求状态；
- ActivityThread 会按 `r.isPersistable()` 选择 Instrumentation overload；进入 `Activity.performCreate(icicle, persistentState)` 后，只有 `persistentState != null` 才调用业务两参数 `onCreate()`，否则仍调用一参数版本；
- 记录事件、恢复 transition state；
- 从 Window style 计算 `mVisibleFromClient`；
- 分发 Fragment created 与 post-created callback。

Activity 基类 `onCreate()` 自己会恢复 Fragment 状态、分发兼容 created callback，并把 `mCalled=true`。子类可以在自身方法中选择何时调用 `super`，所以一条写在子类末尾的日志只证明开发者方法快要返回；它还早于 Instrumentation 的 `postPerformCreate()` 与 ActivityThread 的 super 检查。

若返回后 `mCalled` 仍为 false，ActivityThread 抛 `SuperNotCalledException`。只有 Instrumentation 调用正常返回且这项检查通过，才到 `O_create`。`onCreate()` 中可选择建立 Decor/内容树，但 PhoneWindow、Decor、ViewRoot 与客户端主窗口对应的 WMS `WindowState` 仍是四个不同完成层。

## 12. onCreate 返回后还有客户端提交，再由 executor 补 Start/Resume

成功主路径在 O_create 之后才执行：

```java
r.activity = activity;
mLastReportedWindowingMode.put(activity.getActivityToken(), ...);
r.setState(ON_CREATE);
synchronized (mResourcesManager) {
    mActivities.put(r.token, r);
}
```

这形成一个虽短但真实的 `O_create → M_commit` 窗口。`onCreate()` 执行期间，Activity 自己已经持有 token，却还没有通过 `ActivityThread.mActivities[token]` 对其他客户端路径公开。map 修改借 `mResourcesManager` 锁保护，是因为其他线程的 pending activity configuration 更新会读取这张表；它不把所有 ActivityClientRecord 字段自动变成跨线程安全状态。

`r.setState(ON_CREATE)` 会把 `paused=true`、`stopped=true`，只是 executor 的兼容状态编码。不能由这两个布尔值伪造出 `onPause()`、`onStop()` 已发生的历史。

`performLaunchActivity()` 返回后、`handleLaunchActivity()` 返回前，固定成功且未 finish 的路径把 `r.state` 以及 `shouldRestoreInstanceState`、`shouldCallOnPostCreate` 两个标志写入 `PendingTransactionActions`；后续 Start 据此恢复状态并调用 `onPostCreate()`。对于固定 final Resume，TransactionExecutor 的路径是：

```text
M_commit
→ handleStartActivity / performStart / 客户端 ON_START
→ 可选 onRestoreInstanceState
→ onPostCreate / T_start
→ ResumeActivityItem.handleResumeActivity
```

因此恢复回调与 `onPostCreate()` 不在 `performLaunchActivity()` 内，也不是另一笔服务端 Binder 请求；`onPostCreate()` 阶段完成后形成 `T_start`。

final Pause 支路会从 ON_CREATE 补 `ON_START → ON_RESUME`，再由 `PauseActivityItem` 执行 Pause；这与服务端“以 paused 状态启动”的注释一致。中间 ON_RESUME 调用 `handleResumeActivity(..., finalStateRequest=false)`，仍可能执行正常窗口 add，不能把 final Pause 理解成跳过 Resume 的窗口阶段。

服务端用无参 `PauseActivityItem.obtain()` 创建该请求，使 `mDontReport=true`；它的 `postExecute()` 因而不再回报 `activityPaused`，而服务端已自行把 ActivityRecord 记为 PAUSED。

`performResumeActivity()` 在交付 pending intents/results、调用 `Activity.performResume()` 后把客户端账设为 `ON_RESUME`，形成 U_resume。接下来的窗口工作仍在同一个 `handleResumeActivity()` 中，不能把 `onResume()` 返回当成 W_add。

## 13. 动态提交：resume 才交主窗口，mWindowAdded 也不是 WMS 回执

普通新窗口要进入下面这段代码，至少还需 `performResumeActivity()` 返回非空、token 不在待销毁表、`r.window == null`、Activity 未 finish、`willBeVisible=true`，并在内层满足 `mVisibleFromClient=true`；启动了另一 Activity 时还会同步询问 ATMS 是否仍应可见。固定主路径满足这些条件，`handleResumeActivity()` 在 U_resume 后执行：

```java
r.window = r.activity.getWindow();
View decor = r.window.getDecorView();
decor.setVisibility(View.INVISIBLE);
ViewManager wm = activity.getWindowManager();
activity.mDecor = decor;
layoutParams.type = TYPE_BASE_APPLICATION;
activity.mWindowAdded = true;
wm.addView(decor, layoutParams);
```

这里有三个必须分开的点。

第一，`getDecorView()` 会在需要时调用 `installDecor()`，所以未在 `onCreate()` 建 Decor 的 Activity 仍可在 resume 建立本地 View 根。它是否包含业务内容取决于应用是否设置内容；第 210 章专门拆这一步。

第二，`mWindowAdded=true` 写在 `wm.addView()` **之前**；preserved Window 路径也会直接把它置 true 并跳过本次 add。因此单独读到 true 既不是 `addView()` 正常返回证明，也不是 WMS `WindowState` 的实时回读。它更接近防重入/复用控制位。

第三，普通 `WindowManagerImpl.addView()` 进入 `WindowManagerGlobal`，创建 `ViewRootImpl`，把 View/root/params 放入进程全局表，然后 `ViewRootImpl.setView()` 先 request layout，再同步调用 `IWindowSession.addToDisplayAsUser()`。负返回码会转成 BadToken、InvalidDisplay 等异常；正常 `wm.addView()` 返回才可作为 W_add，说明客户端 ViewRoot 已建立且 WMS 接受了 add。

W_add 之后仍有：首次 traversal、measure/layout/draw、relayout、Surface/BufferQueue、GPU/CPU 渲染、buffer queue、SurfaceFlinger latch/composition/present。`ResumeActivityItem.postExecute()` 随后同步调用 ATMS `activityResumed(token)`，这份客户端回报也不携带“首帧已呈现”证明。

这里讨论的是 App 客户端提交的 Activity 主窗口。为了遮住冷启动空白，system_server 可能更早为同一 ActivityRecord 安排 starting/snapshot/splash window；那类 WMS `WindowState` 的存在既不证明 N_instance，也不证明客户端主窗口已到 W_add。

```text
PhoneWindow存在
  ≠ Decor存在
  ≠ ViewRootImpl存在
  ≠ WMS接受Window
  ≠ Surface可用
  ≠ 首帧draw/submit
  ≠ SurfaceFlinger present
```

## 14. 异常、本地调用、relaunch 与 finish 会在哪一层截断主线

固定成功主线之外，要按 catch 范围和提交点判断：

| 分支或失败 | r48 边界 | 不能笼统声称 |
|---|---|---|
| 服务端无 thread / schedule `RemoteException` | 首次失败标 `launchFailed`、解绑 Activity 后抛出重试；第二次会结束 Activity 并处理进程死亡 | transaction 已到 App |
| pre-destroyed transaction | `P_pre/Q_tx` 后可在 executor callback 前跳过 | ActivityClientRecord 一定创建 |
| record 构造、package 修复、component resolve、Context 创建 | 位于实例化 catch 之外，异常直接越过该 `onException()` 分支 | 所有 launch 异常都交给 Instrumentation |
| newActivity/StrictMode/Intent-state loader | 第一段 `catch (Exception)` 可询问 `Instrumentation.onException()` | 返回 true 就等于 launch 完整成功 |
| makeApplication/attach/theme/onCreate/客户端提交 | 外层 `catch (Exception)` 可询问 Instrumentation；`Error` 不在此 catch 内 | 任意异常都可恢复到 M_commit |
| 未调用 `super.onCreate()` | `SuperNotCalledException` 被专门重新抛出 | custom Instrumentation 可把它当普通异常吞掉 |
| 实例化异常被处理且 Activity 仍为 null | handler 最终返回 null，`handleLaunchActivity()` 请求 ATMS finish token | system_server 会无限等实例 |
| Activity 在 create 中 finish | record 仍可提交，但 pending Start/Resume 与窗口路径受 finished 条件抑制 | O_create 必然导向 W_add |
| system-process Activity | IApplicationThread 可为本地调用，参数不一定经过 Parcel，preExecute/入队可发生在服务端调用栈 | remote oneway 的弱完成语义原样适用 |
| preserved Window relaunch | 新 Activity/PhoneWindow 复用旧 Decor/ViewRoot 关联，resume 跳过本次 add | 每次 N_instance 都对应全新窗口树 |

`Instrumentation.onException()` 返回 true 只是表示该 catch 不立即包装抛出，并不替后续字段建立不变量。尤其在 Activity 非空却尚未赋给 `r.activity` 时，继续执行的代码可能仍因缺失提交而失败；不能把它当成通用事务回滚或恢复协议。

窗口 add 失败发生在 Create/Start/Resume 之后。此时 Activity 对象与 `mActivities` 记录可能早已存在，但主线程仍会因 BadToken 等异常终止当前路径。反过来，构造函数失败时连 A_attach 都没有，WMS 不应成为第一调查点。

## 15. 用最小证据定位卡点，并把 Decor 细节交给第 210 章

看到日志时，先问它属于哪个完成点：

| 最小证据 | 至少说明 | 仍不能说明 |
|---|---|---|
| `LaunchActivityItem.execute()` 入口 trace | L_exec | record 构造成功 |
| Activity 构造函数末尾日志 | 默认 Factory 构造路径尚未到 N_instance；自定义路径不可据此定位 | base Context 已 attach |
| `attachBaseContext()` 返回后的探针 | attach 正在推进 | PhoneWindow 与全部字段已完成 |
| 子类 `onCreate()` 入口 | A_attach、network gate、theme 已越过 | super 已调用或 O_create |
| 子类 `onCreate()` 末尾日志 | 开发者方法将返回 | Instrumentation post、super check 与 M_commit |
| `mActivities[token]` 可见且 state 为 ON_CREATE | M_commit | Start/Resume 或窗口已加入 |
| `onResume()` 返回后的 framework trace | U_resume 附近 | `wm.addView()` 已正常返回 |
| WMS 已有客户端主 `IWindow` 对应的 `WindowState` | addToDisplay 已被接受 | 首帧 buffer 已提交/present |
| frame timeline / present fence 证据 | 对应绘制或 present 点 | 不能反向替代更早对象为何变慢的调用栈 |

用对象可用性再做一次反向检查：

| 阶段 | base Context | Application/token/Intent 字段 | PhoneWindow | Decor | 客户端 ViewRootImpl | 客户端主窗口的 WMS WindowState |
|---|---:|---:|---:|---:|---:|---:|
| Activity 构造函数 | 否 | 否 | 否 | 否 | 否 | 否 |
| A_attach 之后 | 是 | 是 | 是 | 普通路径未必 | 否 | 否 |
| O_create 之后 | 是 | 是 | 是 | 取决于是否触发安装 | 否 | 否 |
| U_resume 之后、窗口分支之前 | 是 | 是 | 是 | 可能已有 | 否 | 否 |
| W_add 之后 | 是 | 是 | 是 | 是 | 是 | 是 |

回到唯一问题：严格关系是 `A_attach < 子类 onCreate 末尾日志 < O_create < M_commit`。所以十三个定义点中最新完成的是 `A_attach`，日志只把调查推进到 A_attach 与 O_create 之间；它还不能替代 Instrumentation 返回、super 检查、`r.activity` 赋值、客户端 map 登记、Start/Resume 与窗口 add。system_server 的早期 `RESUMED` 又只是服务端预期状态，不能补强这些客户端证据。

本章交付 `A_attach/O_create/M_commit` 三个核心边界，并只向后追到 W_add 以防止“有 Window 就已上屏”的误读。第 210 章从 `PhoneWindow` 已存在、Decor 仍可为空的位置继续，专门分析 theme feature、`installDecor()`、系统骨架与 `setContentView()`。

## 16. 九组只读练习：亲手重建实例、Context 与窗口边界

以下命令默认在 Android 11 源码根目录运行；也可预先设置 `ANDROID_BUILD_TOP`。每段只读取源码，不编译、不连接设备，也不修改工作区。

### 练习 1：证明服务端提交与客户端创建不是同一点

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java"
I="$SRC/frameworks/base/core/java/android/app/IApplicationThread.aidl"
C="$SRC/frameworks/base/services/core/java/com/android/server/wm/ClientLifecycleManager.java"
test -f "$A" && test -f "$I" && test -f "$C"
grep -nE 'allPausedActivitiesComplete|ClientTransaction\.obtain|LaunchActivityItem\.obtain|new Intent\(r\.intent\)|ResumeActivityItem\.obtain|PauseActivityItem\.obtain|scheduleTransaction\(clientTransaction\)|minimalResumeActivityLocked|setState\(PAUSED' "$A"
grep -nE '^oneway interface IApplicationThread|scheduleTransaction' "$I"
grep -nE 'scheduleTransaction|transaction\.schedule|instanceof Binder|remote call|local calls' "$C"
```

画出 system_server、App Binder worker 与 App main 三条泳道，把 `S_submit`、服务端 `minimalResumeActivityLocked()` 与 L_exec 分开。再说明为何 system-process 本地 IApplicationThread 会改变线程归属和“调用返回”能证明的内容。

完成标准：不得由服务端 `RESUMED` 推出 Activity 构造函数已运行。

### 练习 2：从 preExecute 追到预销毁取消门

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
L="$SRC/frameworks/base/core/java/android/app/servertransaction/LaunchActivityItem.java"
H="$SRC/frameworks/base/core/java/android/app/ClientTransactionHandler.java"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
X="$SRC/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java"
test -f "$L" && test -f "$H" && test -f "$T" && test -f "$X"
grep -nE 'void preExecute|countLaunchingActivities|updateProcessState|updatePendingConfiguration|void execute|new ActivityClientRecord|postExecute' "$L"
grep -nE 'void scheduleTransaction|transaction\.preExecute|sendMessage.*EXECUTE_TRANSACTION|executeTransaction' "$H"
grep -nE 'public void scheduleTransaction|ActivityThread\.this\.scheduleTransaction|case EXECUTE_TRANSACTION|mTransactionExecutor\.execute' "$T"
grep -nE 'activitiesToBeDestroyed|getActivitiesToBeDestroyed|getActivityClient\(token\) == null|Skip pre-destroyed|executeCallbacks|executeLifecycleState' "$X"
```

把 P_pre、Q_tx、L_exec 标到调用链，另画一条在 callback 前返回的 pre-destroyed 分支。解释为什么 launching count 变化不证明 ActivityClientRecord 存在。

完成标准：能写出 `P_pre → Q_tx`，也能指出二者之后仍有取消门。

### 练习 3：核准 record 初值、LoadedApk 与 alias 双身份

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
A="$SRC/frameworks/base/services/core/java/com/android/server/wm/ActivityStackSupervisor.java"
test -f "$T" && test -f "$A"
grep -nE 'mLifecycleState = PRE_ON_CREATE|ActivityClientRecord\(IBinder|paused = false|stopped = false|getPackageInfoNoCheck|case ON_CREATE|paused = true|stopped = true' "$T"
grep -nE 'intent\.getComponent|resolveActivity|setComponent|targetActivity|new ComponentName|newActivity\(|mComponent = intent\.getComponent' "$T" "$SRC/frameworks/base/core/java/android/app/Activity.java"
grep -nE 'System\.identityHashCode\(r\)|r\.appToken' "$A"
```

分别记录 record 构造时、M_commit 时的 lifecycle/paused/stopped，并画出 alias Intent component 与实际 class name。说明 ident 为何不能当跨进程主键。

完成标准：不能把 `PRE_ON_CREATE` 记录误写成业务 Activity 已存在。

### 练习 4：拆开 token、ResourcesKey 与 ClassLoader

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
C="$SRC/frameworks/base/core/java/android/app/ContextImpl.java"
R="$SRC/frameworks/base/core/java/android/app/ResourcesManager.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/ActivityTaskManagerService.java"
test -f "$T" && test -f "$C" && test -f "$R" && test -f "$M"
grep -nE 'createBaseContextForActivity|getDisplayId\(r\.token\)|createActivityContext|overrideTokenDisplayAdjustments' "$T"
grep -nE 'requestsIsolatedSplitLoading|getSplitClassLoader|getSplitPaths|mIsUiContext|mIsAssociatedWithDisplay|INVALID_DISPLAY|createBaseTokenResources|getAdjustedDisplay' "$C"
grep -nE 'mActivityResourceReferences|getOrCreateActivityResourcesStructLocked|new ResourcesKey|findResourcesForActivityLocked|createResources\(token' "$R"
grep -nE 'int getDisplayId|ActivityRecord\.getStackLocked|return DEFAULT_DISPLAY' "$M"
```

画三栏：token 索引、ResourcesKey 字段、ClassLoader 参数。再把 fixed rotation 的 application adjustment 与 token adjustment 标到 Activity 实例化之前。

完成标准：答案必须明确 ResourcesKey 本身没有 token 和 ClassLoader 字段。

### 练习 5：证明 Factory 返回的是未 attach 的实例

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
I="$SRC/frameworks/base/core/java/android/app/Instrumentation.java"
F="$SRC/frameworks/base/core/java/android/app/AppComponentFactory.java"
W="$SRC/frameworks/base/core/java/android/content/ContextWrapper.java"
test -f "$T" && test -f "$I" && test -f "$F" && test -f "$W"
grep -nE 'appContext\.getClassLoader|mInstrumentation\.newActivity|incrementExpectedActivityCount|setExtrasClassLoader|prepareToEnterProcess|r\.state\.setClassLoader' "$T"
grep -nE 'Activity newActivity\(ClassLoader|getFactory\(pkg\)|instantiateActivity' "$I"
grep -nE 'instantiateActivity|will not be initialized|newInstance' "$F"
grep -nE 'Context mBase|attachBaseContext|Base context already set|mBase\.getResources' "$W"
```

按 C_context、默认 Factory 的类加载/构造、Factory 返回形成 N_instance、extras loader、A_attach 排序；再单列自定义 Factory 可返回替代实例的分支。列出 Factory 内可做的普通 Java 注入与必须延后的 Context/Window 操作。

完成标准：只有 Factory 返回才交付 N_instance；默认构造函数末尾日志仍在它之前，且两者都不能交付 A_attach。

### 练习 6：重建 makeApplication 到 Activity.attach 的闭合顺序

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
L="$SRC/frameworks/base/core/java/android/app/LoadedApk.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
test -f "$T" && test -f "$L" && test -f "$A"
grep -nE 'makeApplication\(false|loadLabel|new Configuration\(mCompatConfiguration\)|mPendingRemoveWindow|addLoaders|setOuterContext|activity\.attach|checkAndBlockForNetworkAccess|activity\.setTheme' "$T"
grep -nE 'makeApplication\(|mApplication != null|newApplication|mAllApplications\.add|callApplicationOnCreate' "$L"
grep -nE 'final void attach\(|attachBaseContext|new PhoneWindow|setWindowManager|mApplication =|mIntent =|mToken =|mWindowManager =' "$A"
```

先画固定初始包的缓存 Application 路径，再画共享进程中另一个 LoadedApk 首次创建 Application 的分支。给 `setOuterContext()` 与 `attachBaseContext()` 分配不同完成点。

完成标准：A_attach 必须晚于 Context、Activity 空壳和 Application 三者出现。

### 练习 7：证明 PhoneWindow、Decor 与 RenderThread 是三条独立线

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
P="$SRC/frameworks/base/core/java/com/android/internal/policy/PhoneWindow.java"
W="$SRC/frameworks/base/core/java/android/view/Window.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
R="$SRC/frameworks/base/libs/hwui/renderthread/RenderProxy.cpp"
Q="$SRC/frameworks/base/libs/hwui/renderthread/RenderThread.cpp"
test -f "$A" && test -f "$P" && test -f "$W" && test -f "$G" && test -f "$R" && test -f "$Q"
grep -nE 'new PhoneWindow|setWindowManager|mWindowManager =' "$A"
grep -nE 'PhoneWindow\(Context context, Window|preservedWindow|getDecorView|installDecor|generateDecor' "$P"
grep -nE 'not.*used for displaying|createLocalWindowManager' "$W"
grep -nE 'static void initialize|getWindowManagerService|getWindowSession|openSession' "$G"
grep -nE 'void RenderProxy::preload|RenderThread::getInstance|queue\(\)\.post' "$R"
grep -nE 'RenderThread::getInstance|RenderThread::RenderThread|start\("RenderThread"\)' "$Q"
```

分别回答：RenderThread 已启动、图形驱动 preload 已完成、PhoneWindow 已有、Decor 已有、IWindowSession 已有，这五件事之间哪些能由当前代码建立必然边。

完成标准：既不能说 `HardwareRenderer.preload()` 没启动 RenderThread，也不能说它返回时某 Activity 已有 renderer 或帧。

### 练习 8：标出 onCreate 与 M_commit 之间的真实窗口

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
I="$SRC/frameworks/base/core/java/android/app/Instrumentation.java"
A="$SRC/frameworks/base/core/java/android/app/Activity.java"
X="$SRC/frameworks/base/core/java/android/app/servertransaction/TransactionExecutor.java"
H="$SRC/frameworks/base/core/java/android/app/servertransaction/TransactionExecutorHelper.java"
test -f "$T" && test -f "$I" && test -f "$A" && test -f "$X" && test -f "$H"
grep -nE 'mCalled = false|callActivityOnCreate|SuperNotCalledException|r\.activity = activity|r\.setState\(ON_CREATE\)|mActivities\.put|handleStartActivity|setRestoreInstanceState|setCallOnPostCreate' "$T"
grep -nE 'prePerformCreate|activity\.performCreate|postPerformCreate' "$I"
grep -nE 'final void performCreate|dispatchActivityPreCreated|onCreate\(icicle|mVisibleFromClient|dispatchActivityPostCreated|mCalled = true' "$A"
grep -nE 'executeCallbacks|executeLifecycleState|handleStartActivity|handleResumeActivity|handlePauseActivity' "$X"
grep -nE 'getLifecyclePath|excludeLastState|ON_START|ON_RESUME' "$H"
```

画 O_create、`r.activity` 赋值、state、map 四个点；再从 ON_CREATE 分别推演 final Resume 与 final Pause。说明 `paused=true/stopped=true` 为什么不是回调历史。

完成标准：子类 `onCreate()` 末尾日志不得被标成 M_commit。

### 练习 9：从 U_resume 追到 WMS 接受窗口，但停在首帧之前

```bash
set -eu
SRC="${ANDROID_BUILD_TOP:-$PWD}"
T="$SRC/frameworks/base/core/java/android/app/ActivityThread.java"
W="$SRC/frameworks/base/core/java/android/view/WindowManagerImpl.java"
G="$SRC/frameworks/base/core/java/android/view/WindowManagerGlobal.java"
V="$SRC/frameworks/base/core/java/android/view/ViewRootImpl.java"
R="$SRC/frameworks/base/core/java/android/app/servertransaction/ResumeActivityItem.java"
S="$SRC/frameworks/base/services/core/java/com/android/server/wm/Session.java"
M="$SRC/frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java"
test -f "$T" && test -f "$W" && test -f "$G" && test -f "$V" && test -f "$R" && test -f "$S" && test -f "$M"
grep -nE 'performResumeActivity|r\.setState\(ON_RESUME\)|getDecorView|mWindowAdded = true|wm\.addView|makeVisible|addIdleHandler' "$T"
grep -nE 'void addView|mGlobal\.addView|createLocalWindowManager' "$W"
grep -nE 'new ViewRootImpl|mViews\.add|mRoots\.add|root\.setView' "$G"
grep -nE 'ViewRootImpl\(Context|requestLayout\(\)|addToDisplayAsUser|ADD_BAD_APP_TOKEN|ADD_INVALID_DISPLAY' "$V"
grep -nE 'addToDisplayAsUser|mService\.addWindow' "$S"
grep -nE 'int addWindow\(|new WindowState\(|return res' "$M"
grep -nE 'handleResumeActivity|activityResumed|getTargetState|ON_RESUME' "$R"
```

把 U_resume、`mWindowAdded=true`、ViewRoot 构造、WMS add 返回、`activityResumed()` 与首帧画成六个点。搜索不到本段中的 draw/present 完成边，正是 W_add 不能证明首帧的依据之一。

完成标准：`mWindowAdded=true` 必须画在 `wm.addView()` 调用之前，并给 preserved Window 另画一条绕过本次 add 的支路。

把九组练习合起来，应能重建这条诊断句：**固定远端普通 App 中，LaunchActivityItem 先建立客户端 record 和带 token/display/config 的 Activity Context，再由 Factory 交付尚未 attach 的 Java 实例；`Activity.attach()` 才接上 Application、Intent、token、PhoneWindow 与 local WindowManager，Instrumentation 正常完成 create 后又要经过 M_commit、Start/Resume 和窗口 add，最终才把调查交给绘制与呈现链。**
