# 161 Android RenderEngine 客户端合成：LayerSettings 与 HWC 混合合成

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 12、21、67、155、160 章

---

## 1. 混合合成不是 GPU 与 HWC 同写一块屏幕

同一帧可以让部分 Layer 走 GPU、部分 Layer 走 HWC。真正的数据关系是：

```text
CLIENT Layers
  → RenderEngine 先画成一张 client target
                                │
DEVICE Layers ─────────────────┼─→ HWC 按 Z 序合成 → present
                                │
client target ──────────────────┘
```

GPU 与 HWC 不是并发写同一 client-target buffer。GPU 是这张 buffer 的生产者，HWC 把它当作一个合成输入，再与 device-composed Layers 组合。

本章的核心结论是：

> SurfaceFlinger 先把候选 Layer 状态交给 HWC validate，接受改判后才生成 `LayerSettings` 并绘制 client target；GPU ready fence 经 BufferQueue 成为 HWC acquire fence，HWC present/release fence 再闭合显示与 buffer 复用。任何一层的“完成”都不能替代下一层。

要特别注意，r48 的错误路径并不普遍提供回滚：validate、RenderEngine draw、client-target advance 或 HWC present 失败，常见行为是记录错误、保留局部状态或继续推进。

---

## 2. 四组对象分别负责什么

主源码：

```text
frameworks/native/services/surfaceflinger/
├── CompositionEngine/src/
│   ├── Output.cpp
│   ├── Display.cpp
│   ├── OutputLayer.cpp
│   ├── RenderSurface.cpp
│   └── ClientCompositionRequestCache.cpp
├── DisplayHardware/
│   ├── HWComposer.cpp
│   ├── HWC2.cpp
│   └── FramebufferSurface.cpp
├── Layer.cpp
├── BufferLayer.cpp
├── BufferQueueLayer.cpp
└── BufferStateLayer.cpp

frameworks/native/libs/renderengine/
├── include/renderengine/
│   ├── DisplaySettings.h
│   └── LayerSettings.h
└── gl/GLESRenderEngine.cpp
```

| 对象 | 职责 |
|---|---|
| `Output / Display` | 编排一帧、维护 output 状态、选择合成策略 |
| `OutputLayer / HWComposer` | 写 HWC 候选、接收 changed types 与 requests |
| `LayerFE::LayerSettings` | 描述一笔 GPU 绘制请求 |
| `RenderSurface / FramebufferSurface` | 管理 client-target BufferQueue 与 HWC 交接 |
| `GLESRenderEngine` | 等 fence、绑定 FBO、逐层绘制、产出 ready fence |

`CLIENT` 是 HWC2 `Composition::CLIENT`，不表示 App 进程负责最终合成；`DEVICE` 也不表示 Layer 没有 App buffer，只表示主要像素合成交给 HWC。

---

## 3. Output::present 的顺序就是阅读骨架

主流程是：

```cpp
updateColorProfile(refreshArgs);
updateAndWriteCompositionState(refreshArgs);
setColorTransform(refreshArgs);
beginFrame();
prepareFrame();
devOptRepaintFlash(refreshArgs);
finishFrame(refreshArgs);
postFramebuffer();
```

因果关系：

```text
更新 profile / Layer 候选
  → 把 frame、crop、z、alpha、dataspace、buffer/fence 等写给 HWC
  → beginFrame 决定是否必须重组
  → validate / presentOrValidate 确定最终类型
  → RenderEngine 只画最终 CLIENT 请求
  → queue client target 并 advanceFrame
  → HWC present
  → 回传 per-layer release 与 present fence
```

不能在 validate 前固定 GPU 列表，因为 HWC 可能接受候选 DEVICE，也可能根据 plane、缩放、色彩和带宽把它改成 CLIENT。

### 没有 Layer 仍可能要送一帧

`beginFrame()` 计算：

```cpp
mustRecompose =
        dirty && !(empty && wasEmpty);
```

上一帧有内容、本帧刚变空时，仍要发出一次空/黑结果以移除旧画面；连续为空才可跳过重复重组。

物理 Display 的 `finishFrame()` 即使 dirty region 为空也可能继续，让 HWC 的状态机保持同步；无 HWC display 的输出才会在 dirty 为空时直接跳过。这也是“无脏区”不等于所有 output 都不走后续路径。

---

## 4. 候选、validate、accept、最终类型是四个阶段

SurfaceFlinger 先给 HWC Layer 写：

```text
display frame / source crop / z
buffer transform / blend / plane alpha
dataspace / HDR metadata
layer color transform
surface damage
buffer + acquire fence
candidate composition type
```

普通 buffer 常以 DEVICE 为候选，cursor、sideband、solid color 有各自类型；Framework 已知不能硬件处理的效果会预先 force client。

真正的协商在 `Display::chooseCompositionStrategy()`：

```text
先调用 Output::chooseCompositionStrategy()
  → output flags 默认 all-client
若有 HWC display
  → getDeviceCompositionChanges()
      → validate 或 presentOrValidate
      → 读取 changed types / requests / client-target property
      → HWComposer 内部 acceptChanges()
  → Display 再把 changed types/requests 应用到本地 OutputLayer
  → 由最终 per-layer type 重算 usesClient / usesDevice
```

这里 `acceptChanges()` 实际在 `HWComposer::getDeviceCompositionChanges()` 返回前完成；Display 随后才同步本地 Layer 类型。不要把“HAL 已 accept”与“本地状态已应用”合为同一步。

最终两项统计：

```cpp
usesClient = anyLayersRequireClientComposition();
usesDevice = !allLayersRequireClientComposition();
```

空集合下 `any_of=false`、`all_of=true`，所以两者都是 false。

| usesClient | usesDevice | 帧形态 |
|---:|---:|---|
| false | false | 空栈/两类都不需要 |
| true | false | 全 client |
| false | true | 全 device |
| true | true | 混合合成 |

### validate 失败不是可靠的“全 GPU 回退”

Display 进入函数时确实先把 output flags 设为 all-client；但 `getDeviceCompositionChanges()` 失败后直接 return，没有把每个 OutputLayer 的 composition type 一并改成 CLIENT。后续 request 生成仍查询 per-layer type。

因此该错误路径最多是“保留默认 output 标志并记录错误”，不能写成已经完成一致的全 GPU recovery。

---

## 5. presentOrValidate 省的是握手，不是任何帧都可快进

HWComposer 仅在 validate 前的候选状态没有 client composition 时尝试 `presentOrValidate`：

```cpp
if (!frameUsesClientComposition) {
    presentOrValidate(..., &state);
} else {
    validate(...);
}
```

原因很直接：一旦预计有 CLIENT Layer，新 client target 还没画、没 queue、也没 `setClientTarget()`，不能提前 present。

返回分两种：

- `state == 1`：present 已成功，HWComposer 保存 present fence 与 release-fence map，并标记 `validateWasSkipped`；
- 其他有效状态：只完成 validate，继续读取变化、accept，稍后正常 present。

后续 `presentAndGetReleaseFences()` 看到 fast path 状态时不会重复 present，而是取已缓存的结果。

所以准确结论是：

> `presentOrValidate` 只在“预计不依赖新 client target，且 HWC 可以直接接受本帧”时合并一轮握手；函数名不保证调用后已经 present。

---

## 6. HWC requests 还会改 client target 的使用方式

validate 返回的不只有 changed types：

| 返回 | 影响 |
|---|---|
| display request | 例如 `FLIP_CLIENT_TARGET` |
| layer request | 例如 `CLEAR_CLIENT_TARGET` |
| client-target property | 希望的 pixel format 与 dataspace |

### FLIP_CLIENT_TARGET

即使最终没有 CLIENT Layer，HWC 也可要求翻转 client target。`composeSurfaces()` 因此在以下任一条件成立时 dequeue：

```cpp
usesClientComposition || flipClientTarget
```

如果只是 flip，它不调用 RenderEngine，返回空 ready fence；`finishFrame()` 仍 queue 这块 scratch/current target，再由 `advanceFrame()` 交给显示链。

### r48 的 client-target dataspace 写回缺口

`Display::applyClientTargetRequests()` 写的是：

```cpp
auto outputState = editState(); // 注意：没有 &
outputState.dataspace = clientTargetProperty.dataspace;

getRenderSurface()->setBufferDataspace(...);
getRenderSurface()->setBufferPixelFormat(...);
```

第一行复制 `OutputCompositionState`，所以对 `outputState.dataspace` 的赋值只改局部副本；RenderSurface 的 dataspace/pixel format 调用才真实保留。

这与旧稿常见的“两处状态都更新”理解不同。若 HWC 返回的 dataspace 与当前 profile 不同，r48 可出现：

```text
RenderEngine 使用 Output state 中的旧 outputDataspace 编码
BufferQueue / RenderSurface 却按 HWC 请求声明新 dataspace
```

是否产生可见错误取决于请求值是否真的不同以及 vendor HWC 行为，但代码级状态分离是确定的。

---

## 7. 一个 Layer 可以生成零笔、一笔或多笔 LayerSettings

`DisplaySettings` 描述整个输出：

```text
physical display / clip / orientation
output dataspace / max luminance
display color transform
clear region
```

`LayerSettings` 描述一笔 GPU 绘制请求：

```text
geometry boundaries / position transform
buffer 或 solid color
alpha / opaque / premultiplied
source dataspace / local color transform
texture transform / filtering
shadow / rounded corners / background blur
disableBlending
```

`LayerFE::LayerSettings` 还带 `bufferId` 与 `frameNumber`，供整帧 request cache 比较内容代际。

一个 SurfaceFlinger Layer 不等于一笔 GPU draw：

- clip 为空或没有可画内容，可以生成 0 笔；
- 普通 buffer/solid content 通常 1 笔；
- 阴影与真实内容同时可见时，生成 shadow + content 两笔；
- 内容被遮住但阴影可见时，只生成 shadow；
- HWC clear request 会生成一笔特殊透明覆盖。

请求顺序仍按 output 的 back-to-front Z 序组织。

### 两组矩阵不能互换

| 字段 | 回答的问题 |
|---|---|
| `positionTransform` | Layer 几何最终画到输出哪里 |
| `textureTransform` | 几何内部从源 buffer 哪些坐标采样 |

RenderEngine 使用：

```cpp
projectionMatrix * positionTransform
```

构造屏幕顶点；纹理矩阵只改变 texture coordinates。位置正确但内容倒置、或内容方向正确但窗口跑位，常来自把这两组变换混淆。

`needsFiltering` 会让纹理从 `GL_NEAREST` 切到 `GL_LINEAR`，解决缩放/非整数采样质量，但增加采样成本。

---

## 8. clearClientTarget 与 clearRegion 清的不是同一件事

`generateClientCompositionRequests()` 先算：

```cpp
clip = viewport ∩ visibleRegion;
```

clip 为空直接跳过，但仍把 `firstLayer=false`。因此代码里的 first layer 是遍历首层语义，不一定是首个实际可见/绘制请求。

### CLEAR_CLIENT_TARGET 是透明覆盖

HWC 对某个非-client Layer 请求 clear 时，只有同时满足：

```text
clearClientTarget
&& Layer opaque
&& !firstLayer
```

才生成特殊请求。透明 Layer 必须与下层混合，不能挖空；底层之前 client target 本就会被全透明清理。

特殊请求为：

```text
solidColor = black
alpha = 0
disableBlending = true
```

关闭 blending 后，它直接写 `(0,0,0,0)`，所以效果是透明清除，不是画黑。若用普通 alpha=0 blending，输出会保留旧 destination，根本清不掉。

### clearRegion 是不透明黑色补洞

`BufferLayer` 尚无 buffer 时，会观察下层覆盖，只把未被下层覆盖的洞加入 `DisplaySettings.clearRegion`。RenderEngine 绘制开始后以 `(0,0,0,1)` 填这一区域。

对照：

| 路径 | 写入值 | 目的 |
|---|---|---|
| 整个 target 初始 clear | 透明黑 | 防复用 ghost、给 overlay 留透明底 |
| HWC CLEAR_CLIENT_TARGET | 透明黑且禁 blending | 擦除指定 device Layer 对应区域 |
| missing-buffer clearRegion | 不透明黑 | 填补没有内容且下层未覆盖的洞 |

---

## 9. secure output 与 protected GPU context 是两道门

`secure` 描述输出是否允许承载安全内容；`protected` 描述 buffer、GPU context 与目标 surface 是否走受保护路径。

当输出 secure、RenderEngine 支持 protected content、且至少一个 Layer 含 protected buffer 时，`composeSurfaces()` 尝试：

```text
RenderEngine.useProtectedContext(true)
RenderSurface.setProtected(true)
```

只有 RenderEngine 的切换结果与目标状态一致时，才继续同步 RenderSurface 状态。

`BufferLayer::prepareClientComposition()` 还会判断：

```cpp
(isProtected() && !supportsProtectedContent)
    || (isSecure() && !targetOutputIsSecure)
```

命中时不采样真实 buffer，而把 Layer 改成 alpha=1 的不透明黑色 solid source。这是安全遮蔽，不是普通渲染故障的黑屏兜底。

Layer 若能留在 DEVICE 路径，是否安全显示还要受 HWC/输出安全能力约束；本节说的是被 RenderEngine 绘制时的保护。

---

## 10. RenderEngine 的 drawLayers 按什么顺序执行

只要 LayerSettings 列表非空，`GLESRenderEngine::drawLayers()`：

1. 等待 client-target dequeue fence；
2. 验证输出 buffer 非空；
3. 绑定 native FBO，或为 background blur 准备离屏目标；
4. 把整个 target 清成透明；
5. 先填不透明 `clearRegion`；
6. 按 Z 序设置几何、crop、颜色、纹理与 blend；
7. 提交 GL 并返回 draw/ready fence。

target fence 优先通过 EGL/native fence 等待；失败时退到 `sync_wait(..., -1)`。这表示上一轮消费者必须先释放目标，GPU 才能覆盖写。

每笔 Layer 的 buffer acquire fence 则在 `bindExternalTextureBuffer()` 使用，保护 App/Codec 生产尚未完成的源 buffer。两者对象不同：

| fence | 被保护的 buffer | 当前等待者 |
|---|---|---|
| Layer acquire | App/Codec 新生产的 Layer buffer | RE 或 HWC |
| target dequeue | 下游上一轮使用的 client target | RE |

### 几何、颜色与纹理

RenderEngine 每层设置：

```cpp
projectionMatrix * layer.positionTransform
display.colorTransform * layer.colorTransform
sourceDataspace
textureTransform + filtering
```

全局矩阵只有在本帧职责确实交给 GPU 时才出现在 `display.colorTransform`；Layer 自己的矩阵仍可存在。

### Alpha 合成

预乘纹理使用：

```cpp
glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
```

```text
out.rgb = src.rgb + dst.rgb × (1 - src.a)
```

非预乘纹理使用：

```cpp
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
```

```text
out.rgb = src.rgb × src.a + dst.rgb × (1 - src.a)
```

仅当整体 alpha=1、buffer opaque、且没有 rounded-corner 边缘覆盖时可关闭 blending。透明 clear 虽 alpha=0，却通过 `disableBlending` 强制覆盖。

draw 结束调用 `flush()` 产出 ready fence；若拿不到 native fence，就 `finish()` 同步等待 GL 完成，此时可返回无有效 fd，因为 CPU 返回前已经完成。

---

## 11. request cache 复用的是“同一输出 buffer 上的完整结果”

`ClientCompositionRequestCache` 以 RenderSurface output `bufferId` 为第一键，再比较：

- 完整 `DisplaySettings`；
- LayerSettings 序列与长度；
- 每笔 `bufferId + frameNumber`；
- 几何、alpha、dataspace、颜色矩阵；
- texture name/transform/filter、opaque/premultiplied；
- shadow、blur、clear 等。

快照把实际 `GraphicBuffer` 与 fence 指针清为 null，避免缓存强引用延长对象生命周期。内容代际仍由 bufferId/frameNumber 表达。

cache hit 的含义是：

```text
这块输出 buffer 已经包含完全相同的像素结果
  → 不再调用 drawLayers
  → ready fence 为空
  → 仍重新 queue 这块 target 给下游
```

它不等于纹理/FBO 资源缓存，也不表示其他 buffer 的同步约束消失。

### 先 add，失败再 remove

r48 在真正 draw 前就把 miss 的 request 加入 cache；若 `drawLayers()` 返回错误，再删除该 output buffer 的 entry。

但是 `composeSurfaces()` 仍返回当时的 ready fence，`finishFrame()` 仍可能 queue target。清缓存只防未来误复用，不会自动：

- 改判为 DEVICE；
- 恢复上一张正确 target；
- 回滚已发生的 clear/partial draw。

具体画面取决于失败发生在 bind、clear、blur、逐层 draw 还是 flush。

---

## 12. ready fence 怎样随 client target 进入 HWC

`finishFrame()` 拿到 optional ready fence 后调用：

```cpp
mRenderSurface->queueBuffer(std::move(readyFence));
```

只要本帧使用 client composition 或有 flip request，`RenderSurface` 就把当前 GraphicBuffer 与 ready fence queue 到 BufferQueue；物理显示 queue 失败会 fatal，虚拟显示尝试 cancel。

不论是否 queue 新 buffer，随后都调用 `DisplaySurface::advanceFrame()`。

物理显示落到 `FramebufferSurface::nextBuffer()`：

```text
acquireBufferLocked()
  → slot / GraphicBuffer / BufferItem fence / dataspace
  → HWC buffer cache 转换 slot/buffer
  → HWComposer.setClientTarget(
        slot, buffer, fence, dataspace)
```

从 GPU 生产者看，它叫 ready fence；进入 FramebufferSurface/HWC 后，同一个同步条件叫 client-target acquire fence：

```text
GPU：我何时写完
HWC：我何时可以读
```

若 `acquireBufferLocked()` 返回 `NO_BUFFER_AVAILABLE`，代码取当前 HWC buffer cache 后直接返回，不重新调用 `setClientTarget()`；HWC 沿用既有 client-target 状态。这支持纯 DEVICE 等帧继续 present，但不是“伪造一张新 target”。

`advanceFrame()` 或 `setClientTarget()` 失败只在这一层记录错误并返回；`RenderSurface::queueBuffer()` 记录后仍让外层继续到 post/present，没有整帧回滚。

---

## 13. 四类 fence 保护四段所有权

| 名称 | 谁产生/携带 | 证明什么 |
|---|---|---|
| Layer acquire fence | App/Codec 随 Layer buffer | 消费者何时可读新 buffer |
| target dequeue fence | ANativeWindow dequeue client target | GPU 何时可覆盖这块旧 target |
| GPU ready / target acquire | RenderEngine → BufferQueue → HWC | HWC 何时可读新 client target |
| Layer release fence | HWC → SF → Layer producer | 旧 Layer buffer 何时可复用 |
| present fence | HWC → SF | 整帧 present 的完成时间线 |

“ready”和“acquire”可以是同一个 fence 的角色命名变化；Layer acquire 与 client-target acquire 却保护两块不同 buffer。

present fence 也不能普遍替代 per-layer release fence。前者面向 display frame；后者面向某 Layer 旧 buffer 的消费者完成。只有代码无法精确匹配时，才会看到 present fence 作为保守兜底。

无 native ready fence 可能有两种完全不同含义：

- cache hit，本帧没有重新写 target；
- RenderEngine 已用同步 `finish()` 等待完成。

所以 fd 无效不等于 GPU 工作丢失，必须结合路径判断。

---

## 14. postFramebuffer 怎样把 fence 还给 Layer 与 target

物理 `Display::presentAndGetFrameFences()`：

```text
presentAndGetReleaseFences()
  → 取 present fence
  → 按当前 HWC Layer 取 per-layer release fence
  → 清 HWComposer 内部 release map
```

若 `presentOrValidate` 已直接 present，HWComposer 使用此前缓存的 present/release 结果，不重复提交。

### 当前 Layer 的保守合并

`Output::postFramebuffer()` 对每个当前 OutputLayer：

1. 先按 HWC Layer 找 release fence；
2. 若当前帧 `usesClientComposition`，再合并当前 client-target acquire fence；
3. 调 `LayerFE.onLayerDisplayed()`。

源码注释说理想上应合并“该 Layer 上一帧被 client-composed 时的对应 fence”，但 r48 没跟踪这份代际信息，于是当前帧只要有任何 client composition，就对所有当前 Layer 合并当前 target fence。

这保证更保守的安全性，却可能让无关 buffer 更晚释放；也不能把“当前 fence”改写成一般协议定义。

### 已离开 output 的 Layer

`mReleasedLayers` 与当前 output list 不相交，无法再按 HWC Layer 精确取 fence，只能传本帧 present fence。这是 released-layer 兜底，不是所有 Layer 的常规 release 规则。

### 两种 Layer 的接收方式

`BufferQueueLayer` 把 fence 交给 `BufferLayerConsumer.setReleaseFence()`，之后释放 pending buffer 并更新 frame history。

`BufferStateLayer` 遍历本帧 callback handles，只把 `previousReleaseFence` 给第一笔 `releasePreviousBuffer` 的事务：同帧更早的新 buffer 若又被后续事务覆盖，根本没显示，应立即释放；只有第一笔真正替换上一帧 buffer 的事务需要等待显示 release。

事务接收、latch、present、release fence 返回、fence signal 是不同完成点。

### client target 自己的释放

`FramebufferSurface` 取到新 slot 时，把旧 slot 记为 pending release。`onFrameCommitted()` 用本帧 HWC present fence给旧 client target 添加 release fence，再把它还给 BufferQueue。

这条 target 生命周期与 App Layer 的 per-layer release 是两套循环。

---

## 15. 颜色职责、性能提示与排障边界

### 全局颜色矩阵只应应用一次

CompositionEngine 只在：

```cpp
!usesDeviceComposition
    && !getSkipColorTransform()
```

时把 display global matrix 放进 RenderEngine。

| 帧形态 | 全局矩阵承担者 |
|---|---|
| 有 DEVICE Layer | HWC/display，覆盖最终混合结果 |
| 全 CLIENT、无 SKIP | RenderEngine |
| 全 CLIENT、有 SKIP | HWC/display |

Layer 自己的 matrix 仍随 `LayerSettings` 进入 GPU；混合合成不等于 CLIENT Layer 不做局部颜色转换。

### expensive rendering 提示可能延续

P3 client target 或昂贵 background blur 会调用：

```cpp
setExpensiveRenderingExpected(true);
```

无 client composition 与 cache hit 会显式设 false。但“仍有 client draw、已回普通 sRGB 且无昂贵 blur”的分支没有显式 false。能从本文件确认的是提示可能延续到后续路径再复位，不能直接断言 GPU 永久高频。

### 线程/进程并不由 C++ 连续调用抹平

```text
Output / LayerSettings 编排：SurfaceFlinger composition 上下文
RenderEngine GL：SF 进程 → GPU driver/kernel
Composer commands：SF → vendor composer，transport 依实现
BufferQueue：对象可同进程，buffer/fence 所有权仍经内核设施
release/callback：最终可跨 Binder 回到生产者
```

排障时按以下问题收集证据：

```text
1. Layer 候选 type 与 validate 后最终 type 分别是什么？
2. getDeviceCompositionChanges 是否成功，是否真正应用 changed types？
3. HWC 是否要求 FLIP/CLEAR_CLIENT_TARGET？
4. client-target property 是否只改到 RenderSurface？
5. 生成了哪些 LayerSettings，clip/shadow/clear 是否改变数量？
6. secure/protected 条件是否把真实 buffer 换成黑色？
7. cache hit，还是 RenderEngine 真正 draw？
8. ready fence 是有效 fd、cache 空 fence，还是同步 finish？
9. setClientTarget / advanceFrame / present 哪一步失败？
10. per-layer release、合并 target fence、released-layer present fence 属于哪条路径？
```

---

## 16. 结论：先定分工，再画 target，最后闭合所有权

整章主链：

```text
Layer 候选状态
  → HWC validate / presentOrValidate
  → changed types + requests + accept
  → 最终 CLIENT / DEVICE 集合
  → LayerSettings
  → RenderEngine client target
  → ready fence 随 BufferQueue 成为 HWC acquire fence
  → HWC 与 DEVICE Layers 合成并 present
  → release/present fences 归还 Layer 与旧 client target
```

最需要记住的十个边界：

1. CLIENT 是 GPU 合成协议类型，不是 App 自己完成合成。
2. validate 是改判/协商，不是显示完成。
3. validate 出错只保留 output 默认 flags，不保证 per-layer 状态一致回退。
4. `presentOrValidate` 仅在预计无新 client target 依赖时可能直接 present。
5. r48 的 client-target dataspace 对 Output state 赋值落在副本上，只有 RenderSurface 设置真实生效。
6. 一个 SF Layer 可产生零、一或多笔 LayerSettings。
7. 透明 target、透明 clear 与不透明 clearRegion 有三种不同用途。
8. cache hit 只证明同一输出 buffer 已有相同结果；draw 失败清 cache 不会回滚本帧。
9. ready/acquire/release/present fence 分别闭合不同阶段和 buffer。
10. r48 对 Layer release 合并当前 target fence是缺少上一帧精确跟踪后的保守近似。

下一章进入 GraphicBuffer、BufferQueue、Gralloc 与 sync fence，继续追踪 slot、generation、dequeue/queue/acquire/release 以及 native handle 怎样组成可复用 buffer 生命周期。
