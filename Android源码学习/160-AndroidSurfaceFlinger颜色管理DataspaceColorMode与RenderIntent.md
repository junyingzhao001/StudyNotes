# 160 Android SurfaceFlinger 颜色管理：Dataspace、ColorMode 与 RenderIntent

> 源码版本：Android 11 / API 30 / `android-11.0.0_r48`  
> 学习方式：macOS 静态阅读，不编译、不连接设备  
> 前置章节：第 12、67、155、158、159 章

---

## 1. 同一帧为何同时需要三套颜色概念

第 159 章停在 Java 侧的几个 SurfaceFlinger 私有事务：

```text
1014：native Daltonizer
1015：Framework 合成后的 4×4 client color matrix
1022：SurfaceFlinger saturation factor
1023：display color setting + 可选 color mode
```

SurfaceFlinger 收到它们后，还不能直接“把颜色写到屏幕”。同一帧里可能同时有 sRGB UI、Display P3 图片、PQ 视频、GPU client target 和 HWC overlay。系统必须回答：

- 每个 Layer 的数字原本按什么色域、transfer 和 range 解释；
- 本帧输出应选哪种 dataspace；
- 显示设备应切哪个 `ColorMode`；
- 用哪个 `RenderIntent` 映射颜色；
- 全局矩阵究竟由 RenderEngine 还是 HWC 应用；
- 何时才能说新颜色真的被显示。

一句话结论是：

> `Dataspace` 是输入/输出像素的解释，`ColorMode` 是显示设备的工作档位，`RenderIntent` 是映射意图；SurfaceFlinger 先从可见 Layer 选理想输出，再由 `DisplayColorProfile` 降级到 HWC 支持组合，最后按 client/device composition 分配 GPU 与 HWC 的颜色职责。

本章仍是静态源码结论。具体面板色准、vendor HAL 对矩阵的容忍度和实际呈现时刻需要设备证据。

---

## 2. Dataspace、ColorMode、RenderIntent 各回答什么

| 概念 | 回答的问题 | 典型值 |
|---|---|---|
| `Dataspace` | 这批像素怎样解释 | sRGB、P3、BT.2020、PQ、HLG |
| `ColorMode` | 显示设备切到哪种校准/输出模式 | NATIVE、SRGB、DISPLAY_P3、BT2100_PQ |
| `RenderIntent` | 源颜色到目标显示怎样映射 | COLORIMETRIC、ENHANCE、TONE_MAP_* |

### Dataspace 是 bitfield

现代 Dataspace 主要由三组字段组成：

```text
STANDARD：primaries、white point 等
TRANSFER：linear、sRGB、ST2084(PQ)、HLG 等
RANGE：full、limited、extended
```

例如：

```text
DISPLAY_P3
= STANDARD_DCI_P3
| TRANSFER_SRGB
| RANGE_FULL
```

所以“P3”还不够：

| Dataspace | primaries | transfer |
|---|---|---|
| `DISPLAY_P3_LINEAR` | P3 | linear |
| `DISPLAY_P3` | P3 | sRGB |
| `DCI_P3` | P3 | gamma 2.6 |

矩阵通常要在线性域工作，这也是 Night Display 为 linear/native color mode 使用不同系数的背景。

### UNKNOWN 仍有默认约定

HAL 对 `UNKNOWN` 的描述不是“随机解释”：RGB 最安全的默认是假定 sRGB/BT.709 类 primaries 与 range，但不承诺自动做精确 gamma 转换。它表示生产者没给完整颜色元数据，消费者只能按约定默认值处理。

类比可以帮助记忆：

```text
Dataspace    = 原稿的坐标与编码
ColorMode    = 显示器的工作档
RenderIntent = 原稿放不进显示器时的映射风格
```

---

## 3. 源码地图与两条控制输入

主源码：

```text
frameworks/native/services/surfaceflinger/
├── SurfaceFlinger.cpp
├── Layer.cpp
├── BufferStateLayer.cpp
├── CompositionEngine/src/
│   ├── Output.cpp
│   ├── OutputLayer.cpp
│   ├── Display.cpp
│   └── DisplayColorProfile.cpp
└── DisplayHardware/
    ├── HWComposer.cpp
    └── HWC2.cpp

frameworks/native/libs/renderengine/gl/
├── GLESRenderEngine.cpp
├── Program.cpp
└── ProgramCache.cpp

hardware/interfaces/graphics/
├── common/1.0/types.hal
└── composer/2.1/IComposerClient.hal
```

颜色决策有两条输入链：

```text
内容链
App/Media → Layer buffer + dataspace + layer transform
                         │
                         ▼
                每帧 Output profile 选择

控制链
ColorDisplayService → 1014/1015/1022/1023
                         │
                         ▼
       全局矩阵 + managed/enhanced/unmanaged 策略
```

它们在 composition 阶段汇合，而不是谁替代谁。

还要区分 composition preference。SurfaceFlinger 暴露的 default/wide composition dataspace 与 pixel format，主要帮助图形栈选择 client-target buffer；它们不等于本帧最后选出的 HWC mode/intent。

---

## 4. 启动时建立的是能力基线，不是固定输出答案

SurfaceFlinger 启动时读取 `ro.surface_flinger.*` 属性：

```cpp
hasWideColorDisplay = has_wide_color_display(false);
useColorManagement = use_color_management(false);

mDefaultCompositionDataspace =
        default_composition_dataspace(Dataspace::V0_SRGB);
mWideColorGamutCompositionDataspace =
        wcg_composition_dataspace(
                hasWideColorDisplay
                    ? Dataspace::DISPLAY_P3
                    : Dataspace::V0_SRGB);
mColorSpaceAgnosticDataspace =
        color_space_agnostic_dataspace(Dataspace::UNKNOWN);
```

随后为物理 display 收集：

- HWC 声明的所有 `ColorMode`；
- 每种 mode 支持的 `RenderIntent`；
- HDR capabilities；
- per-frame metadata 支持位。

仅当 `useColorManagement && displayId` 时才枚举 mode/intent 并构造 WCG 映射。`hasWideColorGamut` 由是否发现 wide color mode 得出。

### 一个容易误读的 HDR 能力补齐

`DisplayColorProfile` 先用 HWC 原始 HDR types 设置：

```text
mHasHdr10 / mHasHLG / mHasHdr10Plus / mHasDolbyVision
```

如果 display 被认定为 WCG，但 HWC 原始列表没有 HDR10 或 HLG，构造函数仍把缺失类型加入对外 `mHdrCapabilities`，注释理由是系统可强制相关 HDR Layer 走 client composition。

但是内部 `mHasHdr10/mHasHLG` 不因此变 true；`isDataspaceSupported(PQ/HLG)` 仍返回原生支持事实。于是两句话可同时成立：

```text
对外能力列表包含 HDR10/HLG
原生 HWC 不支持该 dataspace，Layer 被迫 GPU 合成
```

能力“可由系统处理”与“硬件原生支持”不是同一口径。

当 `useColorManagement` 为 false 时，refresh 会把 output setting 强制成 unmanaged；但全局 color matrix 仍是独立字段，不应推断 Night Display 也被关闭。

---

## 5. Layer Dataspace 怎样进入每帧合成状态

App/系统组件可在 `SurfaceComposerClient::Transaction` 上设置 dataspace。`BufferStateLayer` 收到变化后更新 current state、标记 modified 并请求 transaction。

进入每帧 CompositionEngine 状态时，Layer 至少贡献三类颜色信息：

```text
dataspace            内容像素的颜色解释
colorTransform       单 Layer 颜色矩阵
isColorspaceAgnostic 是否允许改用 output target dataspace
```

它们与 display-wide transform 是不同层级。

`OutputLayer` 对 colorspace-agnostic Layer 做：

```cpp
state.dataspace =
        layerFEState->isColorspaceAgnostic
        && outputState.targetDataspace != Dataspace::UNKNOWN
    ? outputState.targetDataspace
    : layerFEState->dataspace;
```

含义是：该 Layer 声明自己的数值可按输出语境直接解释时，就使用已知 target dataspace，避免多余转换。真实照片/视频若错误标成 agnostic，会丢失源空间语义，不能把它当通用性能开关。

每帧写给 HWC Layer 的仍是 output-dependent `state.dataspace`。若该 dataspace 不被 profile 支持，或 Layer 自身已被强制 client composition，`forceClientComposition` 会置 true。

---

## 6. 1014、1015、1022、1023 改的不是同一种状态

### 1014：native Daltonizer

传入整数的个位决定色弱类型，`n >= 10` 选择 correction，否则 simulation。分支在 `mStateLock` 内更新 `mDaltonizer`，再重算全局矩阵。

模拟全色盲不走这里；第 159 章已看到它退化成 Framework level 200 灰度矩阵，包含在 1015 输入中。

### 1015：Framework client matrix

有矩阵时读取 16 个 column-major float；无矩阵时恢复 identity。然后检查最后一行必须接近：

```text
{0, 0, 0, 1}
```

检查失败只写 error log，不拒绝矩阵，也不回滚。

### 1022：SF saturation factor

```cpp
mGlobalSaturationFactor =
        max(0.0f, min(input, 2.0f));
```

其范围是 `[0,2]`。它主要承接 Natural/Boosted 等 color mode 的 1.0/1.1，不是 Framework level 150 的同一个饱和度控制：

```text
Framework 全局去饱和
  → level 150 → 与其他 level 合成 → 1015

Natural / Boosted
  → 1022 → SF 再生成 saturation matrix
```

### 1023：输出策略与可选 mode

第一个 int 写 `mDisplayColorSetting`；若 Parcel 还有第二个 int，才更新 `mForceColorMode`。随后 invalidate HWC geometry 并 repaint。

已知 setting 为：

```text
0 Managed
1 Unmanaged
2 Enhanced
其他值按 vendor RenderIntent 解释
```

与 1014/1015/1022 不同，r48 的 1023 分支没有显式取得 `mStateLock`。它也不是当场调用 HWC，而是修改下一轮 profile 选择的输入。

这些 1000—1036 backdoor transaction 会在 `onTransact()` 再检查：调用者必须是 system UID，或持有 `HARDWARE_TEST`。普通应用不能直接使用。

---

## 7. “force mode”其实只是一项有限覆盖，而且可能残留

Framework 的 `DisplayTransformManager.setDisplayColor()` 总会写第一个 output setting；只有 composition mapping 有效时才：

- 更新 `persist.sys.sf.color_mode`；
- 在 1023 Parcel 追加第二个 int。

SurfaceFlinger 读取第二个 int 失败时不会清空 `mForceColorMode`。所以从“有 composition mapping 的 mode”切到“无 mapping 的 mode”，上次值可继续残留。

更要注意名字：`forceOutputColorMode` 在 `pickColorProfile()` 中只特殊识别：

```cpp
case ColorMode::SRGB:
    bestDataSpace = Dataspace::V0_SRGB;
    break;
case ColorMode::DISPLAY_P3:
    bestDataSpace = Dataspace::DISPLAY_P3;
    break;
default:
    break;
```

它不是“强令 HWC 最终切到任意这个 ColorMode”。而且 HDR 判断发生在这段覆盖之后：

```text
扫描 Layer
  → 用 force SRGB/P3 改写 SDR best
  → 再判断是否采用 PQ/HLG
```

若满足原生 HDR 条件，HDR dataspace 仍会覆盖刚才的 sRGB/P3 best。最终 HWC mode 还必须经过 `DisplayColorProfile` 映射。

因此第 159 章资源名 `config_displayCompositionColorSpaces`、Framework 参数名 composition color mode、SF 字段名 force color mode，都不能单靠名字推断为绝对硬件模式锁定。

---

## 8. SF 会在 1015 之外再合成两张矩阵

`updateColorMatrixLocked()` 的源码表达式是：

```cpp
colorMatrix =
        mClientColorMatrix
        * saturationMatrix
        * mDaltonizer();
```

当 saturation factor 为 1 时省略中间项。层次是：

```text
DTM levels 100/125/150/200/300 的乘积
  → 1015 mClientColorMatrix
  → 1022 生成的 SF saturation matrix
  → 1014 native Daltonizer
  → mCurrentState.colorMatrix
```

这里直接记录乘法表达式最安全；不要脱离矩阵/向量约定只凭书写左右宣称肉眼效果的先后次序。

SF saturation 使用：

```text
0.213, 0.715, 0.072
```

Framework `GlobalSaturationTintController` 使用的是：

```text
0.231, 0.715, 0.072
```

两层不是相同权重的同一个公式。

### r48 的齐次矩阵契约冲突

Framework controller 第一次直接设置 `level < 100` 时，只填 3×3 项，未先把新数组设成 4×4 identity，所以 `matrix[15] == 0`；它的单测也明确期望尾值 0。若先设置过 100，identity 留下的尾值 1 又会被后续低 level 保留，因此问题还依赖调用历史。

SF 1015 consumer 与 HWC HAL 契约要求 affine 矩阵最后一行/齐次项正确。SF 会记录错误，却继续保存并传递。

RenderEngine shader 使用：

```glsl
vec3(outputTransformMatrix * vec4(color, 1.0))
```

只取 RGB，也不除以 w，所以 GPU 路径可能掩盖尾值问题。HWC 可接受、拒绝、强制 client composition，或表现出 vendor 差异；静态通用源码无法把契约冲突直接升级成“所有设备必然显示异常”。

---

## 9. Binder 返回到屏幕显示之间还有几层状态

矩阵事务在 Binder 线程、`mStateLock` 内更新：

```text
mClientColorMatrix
  → updateColorMatrixLocked()
  → mCurrentState.colorMatrix
  → colorMatrixChanged = true
  → eTransactionNeeded
```

主线程提交 transaction 后，`mDrawingState` 才得到新颜色状态。下一轮 refresh 只有看到 `mDrawingState.colorMatrixChanged`，才把矩阵放进：

```text
CompositionRefreshArgs.colorTransformMatrix
```

各 Output 更新自己的持久状态并 dirty whole output；物理 Display 还向 HWC 发 `setColorTransform`。之后才进入 validate、client composition、present。

所以完成点应拆开：

```text
Java/SF Binder transact 返回
  ≠ SF current→drawing 已提交
  ≠ CompositionEngine 已接收
  ≠ HWC/RenderEngine 已采用
  ≠ present fence 已 signal
  ≠ 面板已扫描显示
```

1015 没有 reply payload 或 present fence。1023 也只触发 geometry/repaint。Framework 无法从这些私有 transaction 的返回值知道后续 `setActiveColorMode` 是否成功。

---

## 10. getBestDataspace 不是“选数值最大的色域”

`Output::getBestDataspace()` 从 sRGB 开始，按 Z 序扫描 output layers：

| Layer dataspace | 写入 SDR best | 写入 HDR |
|---|---|---|
| scRGB / BT2020 / DISPLAY_BT2020 | DISPLAY_BT2020 | 不变 |
| DISPLAY_P3 | DISPLAY_P3 | 不变 |
| BT2020_PQ | DISPLAY_P3 | PQ |
| BT2020_HLG | DISPLAY_P3 | HLG，但已有 HDR 时不覆盖 |

有两个路径依赖。

### SDR/WCG best 是覆盖，不是“最大值归并”

遇到识别的 Layer 就直接改写 `bestDataSpace`。Output 在 finalize 时明确把列表整理为 back-to-front，因此后遇到的顶层 P3 可以覆盖下层 BT2020，反之亦然。r48 测试就是按 top/bottom 场景验证，不是做色域面积比较。

### PQ 永远压过 HLG，但 client 标志只跟 PQ

遇到 HLG 时，仅在 `outHdrDataSpace == UNKNOWN` 才写 HLG；任何位置遇到 PQ 都会写 PQ，所以混合内容最终偏向 PQ，并让 HLG 转 PQ。

`outIsHdrClientComposition` 只在 PQ 分支赋值。多层 PQ 时最后扫描到的 PQ 状态覆盖之前值；HLG 分支不更新它。因此：

- 顶层 PQ 被迫 GPU 合成时，可阻止原生 PQ output；
- 下层 PQ 被迫 GPU 合成、顶层 PQ 走 HWC 时，仍可选 PQ；
- HLG 被迫 GPU 合成本身不通过这个 flag 阻止 HLG output。

这些不是一般 HDR 理论，而是 r48 的具体分支与测试行为。

---

## 11. pickColorProfile 先选理想值，再决定 intent

完整决策可写成：

```text
outputColorSetting == Unmanaged
  → NATIVE / UNKNOWN / COLORIMETRIC
否则
  → 扫描 Layer 得到 SDR best、HDR dataspace、PQ client flag
  → force SRGB/P3 有限覆盖 SDR best
  → 原生 HDR 可用且关键 PQ 非 client 时，best 改为 PQ/HLG
  → Managed 选择 colorimetric/tone-map colorimetric
  → Enhanced 选择 enhance/tone-map enhance
  → vendor setting 直接转成 RenderIntent
  → DisplayColorProfile 映射到 HWC 支持组合
```

Managed 与 Enhanced 的 intent：

| 策略 | SDR | HDR |
|---|---|---|
| Managed | COLORIMETRIC | TONE_MAP_COLORIMETRIC |
| Enhanced | ENHANCE | TONE_MAP_ENHANCE |

Unmanaged 直接返回 NATIVE/UNKNOWN，不扫描 Layer，也不做 profile 映射；但如前所述，这不自动移除全局颜色矩阵。

是否采用 HDR 的门是：

```cpp
hdrDataSpace != UNKNOWN
    && !hasLegacyHdrSupport(hdrDataSpace)
    && !isHdrClientComposition
```

这里的 `isHdrClientComposition` 实际是上一节所述 PQ 特化标志，名字比实现范围更宽。

---

## 12. DisplayColorProfile 怎样回退，以及 legacy HDR 是什么

profile 初始化时预计算：

```text
(request dataspace, request intent)
  → (actual dataspace, HWC ColorMode, HWC RenderIntent)
```

### ColorMode 候选

请求 mode 自己最先。若它是 HDR mode，再尝试另一个 HDR mode；然后依次尝试已知 SDR mode：

```text
DISPLAY_BT2020 → DISPLAY_P3 → SRGB
```

都没命中 HWC map，硬回退为 `NATIVE`。

### RenderIntent 候选

候选搜索先保持类别：

```text
HDR request：自己 → 另一个 HDR intent
SDR request：自己 → 另一个 SDR intent
```

若仍没有命中，函数最后硬回退 `COLORIMETRIC`。所以准确说法是“候选搜索不跨类，最终默认值会回到 SDR colorimetric”，不能笼统说永远不跨 HDR/SDR 类别。

自定义 intent 的收集也有锚点：额外 SDR intents 从 SRGB mode 收集，额外 HDR intents 从 BT2100_PQ mode 收集。只在其他 mode 独有的 vendor intent 不一定进入请求映射全集。

非 WCG display 不执行 `populateColorModes()`。查询不到映射时统一返回：

```text
Dataspace::UNKNOWN
ColorMode::NATIVE
RenderIntent::COLORIMETRIC
```

### legacy HDR 的精确定义

若 HWC 原始能力声称支持对应 HDR type，但：

- 没有 `(PQ/HLG, TONE_MAP_COLORIMETRIC)` 映射，或
- 该映射实际回退到了别的 dataspace，

`hasLegacyHdrSupport()` 返回 true。

它不是“不支持 HDR”，而是“有 HDR format capability，但没有匹配的原生 HDR color-mode 映射”。这时 `pickColorProfile()` 不把整屏 output 切成 PQ/HLG，通常保留 Display P3 路线，由既有 client/HWC 链处理。

---

## 13. Profile 应用是 per-output，失败不会回滚 Framework 设置

选出的 `ColorProfile` 包含：

```text
mode
dataspace
renderIntent
colorSpaceAgnosticDataspace
```

`getTargetDataspace()` 再推导 output target：

```text
HDR ColorMode                → UNKNOWN
否则 agnostic dataspace有效 → agnostic dataspace
否则                        → profile dataspace
```

profile 变化会同时：

- 更新 output mode/dataspace/renderIntent/targetDataspace；
- 给 client-target RenderSurface 设置 buffer dataspace；
- dirty entire output；
- 对物理 display 调 HWC `setActiveColorMode(mode, intent)`。

HDR mode 的 target 为 UNKNOWN，因此 colorspace-agnostic Layer 不会被简单改标成 PQ/HLG。

`Display::setColorProfile()` 遇到 virtual display 会记录 invalid operation 并直接返回；虚拟输出没有等价的物理 HWC color-mode 切换。

HWC `setActiveColorMode()` 失败会在底层记录错误并返回 status，但 `Display::setColorProfile()` 不检查它，也不撤销 output 状态。Java 侧 Settings、SF 已选 profile、HWC 实际档位因此可能在故障时分离；没有跨层事务回滚。

颜色 profile 属于每个 output，而 1015 形成的 `mDrawingState.colorMatrix` 会作为 refresh 输入传播给所有 outputs。多显示器可以选择不同 mode/intent，却面对同一份全局 tint；最终能力差异由各自 HWC/client path 承担。

---

## 14. 全局矩阵由 GPU 还是 HWC 应用，取决于 composition

SurfaceFlinger 每次颜色矩阵变化都会向物理 Display 调 HWC `setColorTransform`。HAL 规定它是“composition 之后”的 affine transform：

- 能应用 hint 或 arbitrary matrix，就由显示端覆盖最终 composition；
- 不能应用时，必须在 `VALIDATE_DISPLAY` 把所有 Layer 强制成 client composition；
- 若具备 `SKIP_CLIENT_COLOR_TRANSFORM`，即使所有 Layer 都由 client 合成，client 也绝不能自己应用，显示端仍负责。

CompositionEngine 给 RenderEngine 设置全局矩阵的门是：

```cpp
if (!outputState.usesDeviceComposition
        && !getSkipColorTransform()) {
    clientCompositionDisplay.colorTransform =
            outputState.colorTransformMatrix;
}
```

可归纳为：

| 组合状态 | 全局 transform 的承担者 |
|---|---|
| 有任何 device-composed Layer | HWC/display |
| 全部 client，且无 SKIP 能力 | RenderEngine |
| 全部 client，但有 SKIP 能力 | HWC/display |

这样可避免只 tint client target、遗漏 overlay，也避免 GPU 与 HWC重复应用。

单 Layer 的 `setColorTransform` 是另一接口。HWC Layer 不支持它时，只需把该 Layer 强制到 client composition；display global transform 不支持时，HAL 契约要求所有 Layer 转 client。两者作用域不同。

HWC 2.3 可查询 per-display `SKIP_CLIENT_COLOR_TRANSFORM`；查询不支持时，r48 才用旧的 global capability 兼容推导。

---

## 15. RenderEngine 在线性颜色链中做什么

对每个 client-composed Layer，SurfaceFlinger 准备：

```cpp
setOutputDataSpace(display.outputDataspace);
setColorTransform(display.colorTransform
        * layer.colorTransform);
setSourceDataSpace(layer.sourceDataspace);
```

shader 主表达式是：

```glsl
OETF(
    OutputTransform(
        OOTF(
            InputTransform(
                EOTF(rgb)))))
```

可以按以下方式理解：

1. EOTF 把纹理的非线性编码还原到线性量；
2. InputTransform 把源 primaries 转到 XYZ/中间域；
3. OOTF 做 HDR 相关的场景/显示映射；
4. OutputTransform 转到目标 primaries，并包含颜色矩阵；
5. OETF 编回目标 transfer。

`Program.cpp` 把全局/Layer color matrix 与 gamut output matrix 合成：

```cpp
mat4 outputTransformMatrix =
        desc.colorMatrix * desc.outputTransformMatrix;
```

然后作为同一个 shader uniform 使用。因此 GPU managed path 中的 Night/DWB/灰度等并不是对最终 8-bit gamma RGB 的事后乘法，而处在 OETF 之前的 output transform 阶段。

sRGB、Display P3 与 BT.2020 可经各自 to-XYZ / from-XYZ 矩阵互转；常见组合也有预计算直达矩阵。无法识别为 DCI-P3 或 BT2020 的 input standard 通常按 BT709/sRGB 基础处理，这是 r48 支持范围的默认分支，不是任意自定义 primaries 解析器。

### 两个场景串起全链

```text
场景 A：sRGB UI + P3 照片 + Night，Managed、无 HDR
  → 扫描通常选 P3/colorimetric
  → 两层都 client：RE 分别解码源空间、转 P3、加入 Night、编码 P3
  → 照片走 HWC overlay：HWC 解释其 P3 dataspace，并在最终 composition 后加全局 transform

场景 B：PQ 视频 + SDR 字幕
  → 原生 PQ mapping 可用、关键 PQ Layer 非 client
  → 选 PQ + tone-map intent
  → legacy HDR 或关键 PQ Layer 被迫 client
  → r48 可能保留 P3 output，让 RE 承担 tone mapping
```

“存在 HDR Layer”不是充分条件；PQ/HLG 混合、legacy mapping 和具体 Layer 的 composition 位置都会改变结果。

---

## 16. 排障清单与结论

只读核对入口：

```bash
cd /Users/ninebot/androidSource

rg -n "case 1014|case 1015|case 1022|case 1023" \
  frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp

sed -n '626,725p' \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/Output.cpp

sed -n '80,195p' \
  frameworks/native/services/surfaceflinger/CompositionEngine/src/DisplayColorProfile.cpp

sed -n '705,765p' \
  hardware/interfaces/graphics/composer/2.1/IComposerClient.hal
```

遇到“广色域没生效、HDR 档位不对、Night 漏到某层、颜色切换延迟”时，按顺序问：

```text
1. Layer 实际 dataspace 与 colorspace-agnostic 标志是什么？
2. useColorManagement、outputColorSetting、残留 force mode 是什么？
3. getBestDataspace 最后见到的 WCG/PQ Layer 是谁？
4. PQ 的 forceClientComposition 标志是什么？
5. 是否被 hasLegacyHdrSupport 挡回 SDR/P3？
6. DisplayColorProfile 把理想组合映射成了什么？
7. profile 是否成功下发 HWC，还是只更新了 SF 状态？
8. 本帧有 device composition 吗，SKIP capability 如何？
9. global transform 由 RE 还是 HWC 负责？
10. 当前证据只到 transact/validate，还是已有 present fence？
```

本章最重要的结论是：

1. Dataspace、ColorMode、RenderIntent 分别描述像素解释、硬件档位和映射策略，不能互换。
2. 输出 profile 是每帧从 Layer 与策略推导的；composition preference 只是 client-target 基线。
3. `forceOutputColorMode` 只有限覆盖 sRGB/P3，原生 HDR 判断还能在之后改写结果。
4. DisplayColorProfile 先按同类候选回退，最终仍可能硬落到 NATIVE/COLORIMETRIC。
5. 对外 HDR capability 可包含 client-composition 能力补齐，不等于 HWC 原生支持。
6. 1015 只是 Framework client matrix；SF 还合入 1022 saturation 与 1014 Daltonizer。
7. r48 首次全局去饱和可能产生齐次尾值 0，与 SF/HWC affine 契约冲突；GPU 可掩盖，不代表所有 HAL 安全。
8. 有 device Layer 时全局 transform 必须覆盖整个 display；全 client 时才可能由 RenderEngine 应用。
9. HWC mode/transform 失败缺少 Framework 级回滚，Binder 返回也不等于 present 完成。
10. 多 output 可选不同 profile，却共享 SF 的全局矩阵输入。

下一章进入 RenderEngine client composition，追踪纹理、crop、transform、alpha、blend、client target、HWC overlay 与 fence 如何共同完成一帧。
