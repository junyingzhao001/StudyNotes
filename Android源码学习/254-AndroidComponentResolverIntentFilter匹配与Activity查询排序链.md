# 254 Android ComponentResolver、IntentFilter匹配与Activity查询排序链

## 1. 主问题：一次 Intent 解析不是“一次匹配”，而是五段裁决

当应用拿着一个 `Intent` 询问“哪些 Activity 能处理它”时，PMS 并不会把 manifest 中的所有 `intent-filter` 从头扫到尾，也不会只看 `priority` 选最大的组件。Android 11 / `android-11.0.0_r48` 的真实链路可以压缩为五段：

`组件与 Filter 注册 → 查询路径分流 → 候选 Filter 召回 → 完整匹配并生成 ResolveInfo → 合并、过滤、排序与单项选择`

这五段回答的是不同问题：

1. `ComponentResolver` 当前知道哪些组件、Filter 与 Provider authority；
2. 显式 component、指定 package、完全隐式 Intent 应从哪组对象开始；
3. MIME type、scheme 或 action 索引能否便宜地召回“可能匹配”的 Filter；
4. action、data/type、categories、用户态与 instant 规则是否让候选真正进入列表；
5. 跨 profile、域名偏好、AppsFilter、动态 split 和 preferred activity 最终留下什么，以及 `resolveIntent()` 返回谁。

最重要的边界是：`queryIntentActivities()` 返回列表，`resolveIntent()` 在列表上做单项选择，`startActivity()` 还要进入 ATMS 的 exported、permission、后台启动、任务栈与进程调度检查。查询成功既不保证选择结果是目标应用，也不保证它最终可以启动。

本章固定在以下源码坐标：

- `frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java`
- `frameworks/base/services/core/java/com/android/server/IntentResolver.java`
- `frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java`
- `frameworks/base/core/java/android/content/IntentFilter.java`
- `frameworks/base/services/core/java/com/android/server/pm/PreferredActivity.java`
- `frameworks/base/services/core/java/com/android/server/pm/PreferredIntentResolver.java`

第253章的 AppsFilter 是本章的一道可见性门，但不是 Filter 匹配器；本章也止于 PMS 返回 `ResolveInfo`，不把 ATMS 的执行安全检查混进来。

## 2. 注册面：四套 Resolver、组件表与 Provider authority 表各司其职

`ComponentResolver` 与 PMS 共用 `mLock`。原因不只是保护几张 Map：生成结果时 Resolver 会回读 `PackageSetting`、用户状态与当前 `AndroidPackage`。若它独立加锁后再进入 PMS 状态，锁顺序很容易反转；当前实现选择共享主锁，让注册与查询看到一致的组件/Settings 组合。

包进入设置提交阶段后，`addAllComponents()` 在锁内依次注册 Activity、Receiver、Provider、Service。四类组件各有自己的 Resolver，Activity 与 Receiver 虽都以 `ParsedActivity` 表示，索引实例仍相互隔离。以 Activity 为例，`addActivity()` 做两件不同的事：

- 把 `ComponentName → ParsedActivity` 写入 `mActivities`，供显式组件直查；
- 把每个 `(ParsedActivity, ParsedIntentInfo)` Pair 交给通用 `IntentResolver.addFilter()`，供隐式查询召回。

Pair 同时携带组件与具体 Filter，是因为索引单位是 Filter，而生成 `ActivityInfo` 又需要组件本体。一个 Activity 可以有多个 Pair；结果去重则在查询阶段按组件身份完成。

Provider 还有独立的 `mProvidersByAuthority`。manifest 的 authority 可以用分号声明多个名称；注册时逐个占位，重复名称保持先注册者并跳过后来者。只有第一个 authority 对应的 Provider 可能保留 `syncable`，从第二个开始会复制对象并清除该位。因而 `content://authority/...` 的 Provider 查找通常是 authority 直达，不是 Activity 式的 action 匹配。

删除或替换包时，`removeAllComponents()` 对称地移除组件与索引项。这里的“不扫描全部组件”依赖注册面持续维护正确；查询面本身不会替注册面修复陈旧索引。

### 练习 1：钉住注册入口、组件表与 authority 分支

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'void addAllComponents(AndroidPackage pkg, boolean chatty) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'addActivitiesLocked(pkg, newIntents, chatty);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'addReceiversLocked(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'addProvidersLocked(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'addServicesLocked(pkg, chatty);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'mActivities.put(a.getComponentName(), a);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'addFilter(Pair.create(a, intent));' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'String[] names = p.getAuthority().split(";");' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (!mProvidersByAuthority.containsKey(names[j])) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'p.setSyncable(false);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'void removeAllComponents(AndroidPackage pkg, boolean chatty) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
```

## 3. 注册后的 priority 不是 manifest 原值：它先受防劫持策略约束

Activity Filter 写入索引后，`addAllComponents()` 会在自己那段内层 `synchronized (mLock)` 之后调用 `adjustPriority()`；但它的正常提交调用点外层仍持有同一把可重入 `mLock`，不能把这段源码形状解释成已脱离主注册临界区。因此排序阶段读取的 `ResolveInfo.priority` 是提交期间完成政策调整后的值，不能仅凭 APK manifest 推断。

规则按当前实现依次收紧：

- priority 小于等于 0 时原样保留；
- 非 privileged 应用的正 priority 一律封顶为 0；
- 系统分区上的 privileged 应用通常可保留正值，但 `SEND`、`SENDTO`、`SEND_MULTIPLE`、`VIEW` 是 protected actions；
- 扫描早期还不知道 setup wizard 是谁，protected Filter 会暂存；系统包扫描完成后，只有 setup wizard 例外可保留高值，其余降为 0；
- 预装 privileged 应用的 `/data` 更新版不能借更新扩张高优先级范围：新增 Activity 降为 0，既有 Activity 的新 Filter 也只能在系统基础版可覆盖的 action、category、scheme、authority 子集内保留，并封顶到匹配系统 Filter 的最大 priority。

最后一条尤其容易被写成“两个 Filter 必须完全相等”。当前代码在这一段逐类缩小候选系统 Filter，却没有把 MIME type、path 等每一个维度都做同形等价比较。正确结论只能落在源码实际检查的集合上。

这套政策只限制能进入后续比较的 priority；它不替代 `IntentFilter.match()`，也不等价于 preferred activity。高 priority Filter 若完整匹配失败，根本不会生成 `ResolveInfo`。

### 练习 2：核对正 priority 的三层封顶

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private static final Set<String> PROTECTED_ACTIONS = new ArraySet<>();' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'PROTECTED_ACTIONS.add(Intent.ACTION_SEND);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'PROTECTED_ACTIONS.add(Intent.ACTION_SENDTO);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'PROTECTED_ACTIONS.add(Intent.ACTION_SEND_MULTIPLE);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'PROTECTED_ACTIONS.add(Intent.ACTION_VIEW);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'private void adjustPriority(List<ParsedActivity> systemActivities, ParsedActivity activity,' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (intent.getPriority() <= 0) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (!privilegedApp) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'intent.setPriority(0);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'mProtectedFilters.add(Pair.create(activity, intent));' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (packageName.equals(setupWizardPackage)) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'ParsedActivity foundActivity = findMatchingActivity(systemActivities, activity);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'getIntentListSubset(intentListCopy, IntentFilter::actionsIterator, actionsIterator);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'getIntentListSubset(intentListCopy, IntentFilter::authoritiesIterator,' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
```

## 4. 六张倒排表：索引决定从哪里找，不决定谁匹配

通用 `IntentResolver` 维护一个完整 Filter 集合和六张与召回有关的 Map：

| 表 | key | 收录条件与用途 |
| --- | --- | --- |
| `mTypeToFilter` | 完整/规范化 MIME | 精确 type 或 Filter 自身 wildcard 的入口 |
| `mBaseTypeToFilter` | `image` 一类 base | 查询 `image/*` 时召回该大类的具体 Filter |
| `mWildTypeToFilter` | `image` 或 `*` | 召回 `image/*`、`*/*` Filter |
| `mSchemeToFilter` | `https` 等 scheme | URI 候选入口，host/path 仍未验证 |
| `mActionToFilter` | action | 只收既无 scheme 又无 MIME 的 Filter |
| `mTypedActionToFilter` | action | 只收声明 MIME 的 Filter，为 `*/*` 查询缩小集合 |

`addFilter()` 先注册 scheme，再注册 MIME。只有两者数量都为 0，actions 才进入 `mActionToFilter`；只要声明 MIME，actions 还会进入 `mTypedActionToFilter`。因此 `mActionToFilter` 不能理解为“所有声明了该 action 的 Filter”。

MIME 的内部表示也值得逐行核对。`image/jpeg` 同时写入完整 type 表和 base type 表；解析器内部保存的 `image/*` 可表现为无斜杠的 `image`，注册时补成 `image/*` 写入 type 表，并以 `image` 写入 wild 表；全局 wildcard 对应 wild 表的 `*` key。

动态 MIME group 还有一层包装：`MimeGroupsAwareIntentResolver.addFilter()` 先把 group 当时包含的类型展开为 Filter 的 dynamic data types，再调用普通 `addFilter()` 写入上述 MIME 索引；`mMimeGroupToFilter` 用于 MIME group 日后变化时定位并重建相关 Filter，不是查询 Intent 时的第七张 cut 表。

Map 的 value 是以 `null` 结尾、按需扩容的数组。一个 Filter 可能进入多张表，所以索引召回天然可能重复。移除路径按同样的 scheme/type/action 条件清表，最后再从总集合删除。

### 练习 3：从 addFilter 还原六张表的写入条件

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public void addFilter(F f) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'mFilters.add(f);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'mSchemeToFilter, "      Scheme: ");' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'int numT = register_mime_types(f, "      Type: ");' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (numS == 0 && numT == 0) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'mActionToFilter, "      Action: ");' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (numT != 0) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'mTypedActionToFilter, "      TypedAction: ");' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'addFilter(mTypeToFilter, name, filter);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'addFilter(mBaseTypeToFilter, baseName, filter);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'addFilter(mWildTypeToFilter, baseName, filter);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'private final ArrayMap<String, F[]> mTypeToFilter = new ArrayMap<String, F[]>();' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'private final ArrayMap<String, F[]> mTypedActionToFilter = new ArrayMap<String, F[]>();' frameworks/base/services/core/java/com/android/server/IntentResolver.java
```

## 5. 候选 cut：type、scheme、action 最多形成四路召回

`queryIntent()` 不是在六张表中任选一张。它根据 `resolvedType`、scheme 和 action 组合出最多四个数组：三个 type cut 加一个 scheme cut，然后逐个交给同一个 `buildResolveList()`。

MIME 召回可以用三个例子记忆：

| 查询 type | 第一 cut | 第二 cut | 第三 cut |
| --- | --- | --- | --- |
| `image/jpeg` | 精确 `image/jpeg` | Filter 的 `image/*` | Filter 的 `*/*` |
| `image/*` | base 表中的 `image` 具体类型 | Filter 的 `image/*` | Filter 的 `*/*` |
| `*/*` 且有 action | typed-action 表中的该 action | 无 | 无 |

scheme 非空时，无论 MIME 是否存在，都可额外从 `mSchemeToFilter[scheme]` 召回。只有 `resolvedType == null && scheme == null && action != null` 时，才从无 data 的 action 表召回。

这是一套“不能漏掉潜在真匹配”的粗筛，不是匹配证明。scheme cut 只知道 `https`，还不知道 host、port、path；type cut 也没有检查 action 与 categories。相反，一个 Filter 同时带 type 与 scheme 时可能被两条 cut 召回，后面必须去重。

若 `resolvedType` 是 `*/*` 却没有 action，当前全局查询不会构造 type cut；type、scheme、action 三者全空时也没有任何 cut。另一方面，直接调用 `IntentFilter.match()` 时 action 为 `null` 会跳过 action 检查。可见“匹配函数理论上成立”不等于“全局索引路径一定召回”，端到端分析必须把索引可达性放在 match 之前。

若 `resolvedType` 格式没有斜杠，当前 cut 选择代码同样不会为它构造 MIME cut；正常公共路径应传入解析后的合法 MIME。排障时应把 `Intent.getType()`、`resolvedType` 与 URI scheme 分开记录，不能把 data URI 猜成 MIME。

### 练习 4：把三种 MIME 查询和 scheme/action 分支对应到数组

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'F[] firstTypeCut = null;' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'F[] secondTypeCut = null;' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'F[] thirdTypeCut = null;' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'firstTypeCut = mTypeToFilter.get(resolvedType);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'secondTypeCut = mWildTypeToFilter.get(baseType);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'firstTypeCut = mBaseTypeToFilter.get(baseType);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'thirdTypeCut = mWildTypeToFilter.get("*");' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'firstTypeCut = mTypedActionToFilter.get(intent.getAction());' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'schemeCut = mSchemeToFilter.get(scheme);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (resolvedType == null && scheme == null && intent.getAction() != null) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'firstTypeCut = mActionToFilter.get(intent.getAction());' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'filterResults(finalList);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'sortResults(finalList);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
```

## 6. 查询分流：显式 component、指定 package、全局隐式是三条不同路径

`queryIntentActivitiesInternal()` 的入口顺序本身就是诊断信息：用户不存在先返回空列表；以 `filterCallingUid` 判断 instant caller；用当前 Binder UID 校验跨用户权限；保存外层 package/component 并处理 selector；最后调用 `updateFlagsForResolve()`。后者会结合 safe mode、隐式拍照约束、instant 可见性与 Direct Boot 用户解锁状态补齐或收紧 flags。只有记录规范化后的 flags，才能解释 `newResult()` 为何拒绝组件。

随后形成三条主路：

1. component 非空：直接 `getActivityInfo()`；
2. component 为空、外层 package 非空：拿该 `PackageSetting.pkg.getActivities()` 走包内列表匹配；
3. component 与外层 package 都为空：走全局 Activity Resolver 的倒排索引，并可能合入跨 profile、域名与 instant 候选。

显式路径不会调用 `IntentFilter.match()`。component 已准确指定时，即使 action、data、category 与目标 Activity 的任何 Filter 都不相容，也仍可能产生单条 `ResolveInfo`。但“显式”只跳过 Filter 匹配，不跳过 `getActivityInfo()` 内的用户/组件状态，也不跳过 instant 暴露规则、普通查询的 AppsFilter 与最终 post-resolution 过滤。还要区分身份：公开 `getActivityInfo(comp, flags, userId)` 会按当下 Binder UID 做自己的权限/可见性判断，查询入口随后才以保存的 `filterCallingUid` 做额外显式过滤；可信内部路径常先清除成 system 身份，但不能据此声称 `resolveForStart` 无条件绕过前一层。

指定 package 的实现也不是“全局索引召回后再扔掉其他包”。PMS 先拿目标包 Activity 列表，`queryIntentForPackage()` 为每个带 Filter 的 Activity 构造一个 Pair 数组，再调用 `queryIntentFromList()`。这会扫描目标包内 Filter，但仍执行完整 match、默认类目检查、结果生成、去重与排序。

selector 有一个值得保留原始变量的细节：`pkgName` 在替换成 selector 之前就已读取，component 则在替换后重新读取。于是外层 package 约束仍决定三路分流，而 action/data/categories 可来自 selector；不能简单说“有 selector 就完全丢弃外层 Intent”。反过来，若只有 selector 设置 package、外层 package 为空且 selector 也无 component，PMS 仍进入全局 Resolver 路径，但 `buildResolveList()` 读取的是 selector 的 package，仍会在候选数组中跳过其他包 Filter。`updateFlagsForResolve()` 的 `onlyExposedExplicitly` 又使用“selector 处理后的 component 或处理前的外层 package”：所以仅 selector 带 package 不会像外层 `setPackage()` 那样为 instant caller 补 `MATCH_EXPLICITLY_VISIBLE_ONLY`。

### 练习 5：跟踪三路分流与包内 listCut

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'final String pkgName = intent.getPackage();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ComponentName comp = intent.getComponent();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'intent = intent.getSelector();' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (comp != null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'final ActivityInfo ai = getActivityInfo(comp, flags, userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (pkgName == null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'intent, resolvedType, flags, setting.pkg.getActivities(), userId),' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'List<ResolveInfo> queryIntentForPackage(Intent intent, String resolvedType,' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'ArrayList<Pair<ParsedActivity, ParsedIntentInfo>[]> listCut =' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'array[arrayIndex] = Pair.create(activity, intentFilters.get(arrayIndex));' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'return super.queryIntentFromList(intent, resolvedType, defaultOnly, listCut, userId);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
```

## 7. buildResolveList 的顺序：先廉价门与去重，再完整 match，再生成结果

每条 cut 最终都进入 `buildResolveList()`。对数组中的每个 Filter，控制流严格按以下顺序执行：

`excludingStopped → package 限定 → autoVerify 仅调试观察 → allowFilterResult 去重 → IntentFilter.match → MATCH_DEFAULT_ONLY → newResult → add`

几个顺序差异会直接改变结论。

第一，stopped 门只存在于这些隐式 Filter 路径，而且只在 Intent 明确处于 excluding-stopped 语义时生效；显式 component 不走此门。Activity Resolver 的 `isFilterStopped()` 又明确让 system package 不被视为 stopped。`autoVerify` 在此处也不负责决定是否加入列表；代码只在调试日志中打印已验证状态和 authorities。Web domain 的候选过滤在 PMS 更外层处理。

第二，Activity 的 `allowFilterResult()` 检查目标列表中是否已有同 package + activity name，而且发生在本 Filter 的完整 `match()` 之前。若前一个 Filter 匹配失败，它没有结果可供去重，后一个 Filter 仍可尝试；一旦某个 Filter 成功生成并加入 Activity 结果，后续同 Activity Filter 会在比较自己的 match 之前被跳过。

第三，去重保留的是“按 cut 与数组遍历顺序首个成功产出结果的 Filter”，不保证它是该 Activity 所有命中 Filter 中 `match` 数值最大或最具体的那个。若请求了 `GET_RESOLVED_FILTER`，`ResolveInfo.filter` 也只携带这一个 Filter；priority、isDefault、label/icon 与 `handleAllWebDataURI` 同样来自获胜 Filter，而不一定是该组件所有 Filter 中最有利的一组值。它既能防止同一 Filter 被 type/scheme 双路召回后重复，也会把同一 Activity 的多个不同 Filter 合成一行。

第四，`newResult()` 返回 `null` 时不会写入目标列表，所以后续同组件 Filter 仍有机会继续。于是准确措辞应是“首个成功创建并加入的结果赢得该组件去重”，而不是“首个候选 Filter 永远赢”。

### 练习 6：验证廉价门、匹配、默认类目与去重的真实次序

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'if (excludingStopped && isFilterStopped(filter, userId)) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (packageName != null && !isPackageForFilter(packageName, filter)) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (intentFilter.getAutoVerify()) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (!allowFilterResult(filter, dest)) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'match = intentFilter.match(action, resolvedType, scheme, data, categories, TAG);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'if (!defaultOnly || intentFilter.hasCategory(Intent.CATEGORY_DEFAULT)) {' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'final R oneResult = newResult(filter, match, userId);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'dest.add(oneResult);' frameworks/base/services/core/java/com/android/server/IntentResolver.java
grep -n -F 'protected boolean allowFilterResult(Pair<ParsedActivity, ParsedIntentInfo> filter,' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F '&& Objects.equals(destAi.packageName, filter.first.getPackageName())) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'return false;' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
```

## 8. 完整匹配第一层：action、categories 与 CATEGORY_DEFAULT 是三种语义

`IntentFilter.match()` 的顺序是 action、data/type、categories。只要 Intent action 非空且 Filter 不含该 action，立即返回 `NO_MATCH_ACTION`；Intent action 为 `null` 时，这一层不会要求 Filter 也为空。这是单向检查，不是两个 action 集合做相等比较。

categories 的方向与 action 相反：Intent 携带的每一个 category 都必须存在于 Filter；Filter 多声明 category 不妨碍匹配。若 Intent 没有 categories，`matchCategories()` 直接成功，哪怕 Filter 自己有多个 category。

`CATEGORY_DEFAULT` 又不等于普通 category 检查。应用通过 `MATCH_DEFAULT_ONLY` 查询时，`buildResolveList()` 在完整 `match()` 成功后额外要求 Filter 声明 `CATEGORY_DEFAULT`。它没有把 DEFAULT 临时添加到 Intent categories，也没有改变 `matchCategories()` 的方向。于是一个 Filter 可能普通 categories 匹配成功，却因 default-only 门被丢弃。

失败码把失败层分开：type 为 `-1`，data 为 `-2`，action 为 `-3`，category 为 `-4`。这些负值用于诊断，成功值则是“匹配类别 + 调整量”的位组合；不要把负值绝对值理解成优先级。

## 9. 完整匹配第二层：data/type 是一棵有短路与覆盖的决策树

`matchData()` 不能概括成“scheme、host、path、MIME 全部相等”。按非 wildcard 的普通解析路径，先看 Filter 是否同时没有 types 与 schemes：只有 Intent 的 type 与 data 都为空才成功；否则返回 data 不匹配。

Filter 声明 schemes 时：

1. Intent scheme 必须出现在 Filter；Intent scheme 为 `null` 会按空字符串比较；
2. 若 Filter 声明 scheme-specific-part 且 data 非空，SSP 命中就得到 SSP 类别并跳过 authority/path；SSP 未命中时，只有存在 authority 才可能由下一分支挽回，没有 authority 就维持 data 失败；
3. 声明 authority 时必须先匹配 host，以及可选 port；host 可用前导 `*` 做后缀匹配，当前实现的 host 比较忽略大小写，声明 port 后端口也必须相等；
4. 只有 authority 已声明且命中时，声明的 path 才会以 `PatternMatcher` 的 literal、prefix 或 glob 语义继续检查；它不是简单字符串相等；
5. 没有 SSP 与 authority 约束时，scheme 自身可构成 data 匹配。

Filter 没声明 scheme 时存在专门便利规则：Intent 只允许无 scheme、空 scheme、`content` 或 `file`；其他 scheme 即使 MIME 相同也会先以 data 失败。它支持“只声明 MIME 的 Filter”处理常见 ContentProvider/file URI，却绝不是任意 scheme wildcard。

URI 检查通过后才处理 MIME。Filter 有 types 时必须由 `findMimeType()` 命中；Filter 无 types 时 Intent type 必须为空。方向要从“这个 Filter 能否接受查询 type”理解：Filter `image/jpeg` 可接受 `image/jpeg`、查询方 `image/*` 与 `*/*`，但不接受 `image/png`；Filter `image/*` 可接受任意非空 `image/subtype`、`image/*` 与 `*/*`；Filter `*/*` 可接受任意非空 type，type 为 `null` 仍失败。所有这些 MIME 成功路径都得到 TYPE 类别，不按 exact/base/global wildcard 再分高低，而且 MIME 比较按大小写敏感语义工作。

一个很容易影响排序的细节是：只要 MIME 成功，局部 `match` 会被写成 `MATCH_CATEGORY_TYPE`。也就是说，同时匹配了 scheme/host/path 与 MIME 的 Filter，最终 `ResolveInfo.match` 的 category 体现 TYPE，而不会保留 PATH 比较值。SSP 的“跳过 authority/path”和 MIME 的“覆盖 URI 类别”是两种不同现象。

### 练习 7：逐行走完 data、URI 与 MIME 决策树

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'public final int matchData(String type, String scheme, Uri data) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (!wildcardWithMimegroups && types == null && schemes == null) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (schemes.contains(scheme != null ? scheme : "")' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F '? MATCH_CATEGORY_SCHEME_SPECIFIC_PART : NO_MATCH_DATA;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (match != MATCH_CATEGORY_SCHEME_SPECIFIC_PART) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'int authMatch = matchDataAuthority(data, wildcardSupported);' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'match = MATCH_CATEGORY_PATH;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F '&& !"content".equals(scheme)' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F '&& !"file".equals(scheme)' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (findMimeType(type)) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'match = MATCH_CATEGORY_TYPE;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'return match + MATCH_ADJUSTMENT_NORMAL;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'private final boolean findMimeType(String type) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (typeLength == 3 && type.equals("*/*")) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'if (hasPartialTypes() && t.contains("*")) {' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public static final int MATCH_CATEGORY_EMPTY = 0x0100000;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public static final int MATCH_CATEGORY_TYPE = 0x0600000;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public static final int MATCH_CATEGORY_MASK = 0xfff0000;' frameworks/base/core/java/android/content/IntentFilter.java
grep -n -F 'public static final int MATCH_ADJUSTMENT_NORMAL = 0x8000;' frameworks/base/core/java/android/content/IntentFilter.java
```

## 10. match 正值说明 data 匹配类别，不等于 Filter priority

完整 `match()` 先在 action 失败时返回 `NO_MATCH_ACTION`，再把 `matchData()` 的负值原样返回，最后以 `matchCategories()` 决定是否返回 `NO_MATCH_CATEGORY`；三层都通过才返回 `dataMatch`。因此成功值主要来自 data/type 匹配，不编码 action 或 categories 的“数量”。

成功值不是多个 category flag 的集合，而是一个 category 基值加 adjustment。当前常量依次是 EMPTY `0x0100000`、SCHEME `0x0200000`、HOST `0x0300000`、PORT `0x0400000`、PATH `0x0500000`、SCHEME_SPECIFIC_PART `0x0580000`、TYPE `0x0600000`；普通路径再加 `MATCH_ADJUSTMENT_NORMAL` 的 `0x8000`。category mask 是 `0xfff0000`，adjustment mask 是 `0x000ffff`。数值较高表示这次 data 匹配类别更高，但它与以下概念互不等价：

- `IntentFilter.priority` 是 Filter 的排序字段，优先比较；
- `ResolveInfo.isDefault` 表示 Filter 是否声明 DEFAULT；
- `preferredOrder` 是另一个 ResolveInfo 字段，当前 Activity `newResult()` 并未从包写入有效值；Web domain 过滤可把 `linkGeneration` 写进去，出现 ALWAYS_ASK 时又会把相关项清零，instant installer 模板也把它设为 1；
- preferred activity 是用户/系统保存的选择规则，在列表排序之后参与 `chooseBestActivity()`；
- domain verification 会在 Web Intent 的候选合并过滤阶段介入。

所以“path 比 host 更具体”只说明没有被 MIME 覆盖时的 `match` 类别关系；它不能越过更高的 Filter priority，也不能自动成为 preferred activity。还有一个诊断边角：Filter 同时没有 types 与 schemes 时，只要 Intent 的 type 非空或 data 非空，早期特殊分支统一返回 `NO_MATCH_DATA`，不能仅凭“看起来是类型不符”断言一定得到 `NO_MATCH_TYPE`。

## 11. newResult 是第二次资格门：从 Filter 命中到 ResolveInfo 还隔着用户态

完整 Filter 匹配成功后，Activity Resolver 调用 `newResult()`。这个方法不是纯粹的对象构造器，它依次确认：

- 目标用户仍存在；
- Activity 所属 `AndroidPackage` 仍存在；
- `isEnabledAndMatches()` 允许该 Activity 在当前 flags / userId 下出现；
- `PackageSetting` 与 `PackageUserState` 可用；
- `generateActivityInfo()` 成功；
- instant 调用/目标符合 visible-to-instant、explicitly-visible 与 `MATCH_INSTANT` 规则；
- instant 目标没有处于需要转入 instant resolution 的更新状态。

因此 index 命中、`IntentFilter.match() >= 0` 与列表实际出现是三个不同完成点。第253章的 per-user enabled、installed、Direct Boot 规则在这里通过 `isEnabledAndMatches()` 接回本章；`excludingStopped` 则更早在 `buildResolveList()` 使用 Intent 自身的排除标志。

成功后，`ResolveInfo` 才获得 `activityInfo`、可选的 resolved Filter、`handleAllWebDataURI`、调整后的 priority、match、DEFAULT 标志、label/icon、system 与 instant 状态。普通 Activity `newResult()` 没有写 `preferredOrder`，所以本地结果通常为 0；PMS 后续的 Web domain 与 installer 路径仍可改它，不能把“普通构造时为 0”推广到最终所有候选。

这里还没有做普通完整应用调用者的最终 AppsFilter 遍历过滤。全局隐式分支会先生成候选列表，随后由 PMS 的 post-resolution 门按真正调用者再清理；指定 package 分支则在进入包内匹配前就先判断目标包可见性，最后仍会经过统一后过滤。

## 12. post-resolution：AppsFilter、instant 与动态 split 在列表末端再清一次

`applyPostResolutionFilter()` 从列表尾部向前处理，避免删除导致索引错位。它首先移除 Web instant 被全局禁用时的本地 instant 结果。若命中的 Activity 位于尚未安装的动态 split，且允许动态 split，它不会把原 Activity 当作可启动结果：没有 installer 时删除，有 installer 时构造 installer `ResolveInfo` 替换，并复制原候选的展示标签与图标信息。无论删除还是替换，该轮都会立即 `continue`；替换出的 installer 不会在同一次循环继续经过下方 AppsFilter/instant 调用者分支。

接着按调用者形态过滤：

- 完整应用调用者：`resolveForStart` 为真时放行；否则调用 `mAppsFilter.shouldFilterApplication()`，被过滤的目标移除；
- instant 调用者访问同包：放行；
- start 场景中的 Web / match-external、且没有显式 package/component：允许间接进入其他 instant 目标；
- 普通目标 Activity 带 `FLAG_VISIBLE_TO_INSTANT_APP`、即向 instant 可见：放行；这里不再排除 implicitly-visible，是否只接受显式暴露由更早的 flags/newResult 门决定；
- 其余对 instant 调用者不可见的结果移除。

显式 component 路径在构造单条列表前已有一次 `blockNormalResolution` 检查，但最后仍调用这个统一方法。全局与包内隐式路径也都在返回前调用它。这样 `resolveForStart` 能为真正启动路径绕开用于防枚举的 AppsFilter，却没有授权绕过 ATMS 后续 exported/permission 等执行门。

AppsFilter 的方向是 `filterCallingUid → resolvedSetting`。若排障只看目标 Filter 是否匹配，而不记录真正用于过滤的 UID，很容易把“匹配成功但对调用者隐藏”误报为匹配器缺陷。

### 练习 8：观察动态 split 替换与最终可见性清理

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'private List<ResolveInfo> applyPostResolutionFilter(@NonNull List<ResolveInfo> resolveInfos,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'for (int i = resolveInfos.size() - 1; i >= 0; i--) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (info.isInstantAppAvailable && blockInstant) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& info.activityInfo.splitName != null' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'resolveInfos.set(i, installerInfo);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (ephemeralPkgName == null) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (resolveForStart' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '|| !mAppsFilter.shouldFilterApplication(' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '} else if (ephemeralPkgName.equals(info.activityInfo.packageName)) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& (intent.isWebIntent()' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& intent.getPackage() == null' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'resolveInfos.remove(i);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 13. 全局隐式查询还会合流：跨 profile、域名偏好与 instant installer

完全隐式路径不是简单地把 `mComponentResolver.queryActivities()` 原样返回。PMS 先查询可能要求跳过当前 profile 的 cross-profile Filter；命中时可直接返回一个 forwarding `ResolveInfo`。否则查询当前 profile，再根据结果与 flags 判断是否允许 instant resolution，并尝试加入普通 cross-profile forwarding 候选。

Web Intent 还会查询 parent profile 的 domain preferred 状态。跨 profile 结果代表 `IntentForwarderActivity`，不是把另一个用户的目标 Activity 直接泄露给调用者。若 parent domain 候选存在，代码会避免把普通 cross-profile forwarding 结果重复加入；候选较多时再通过 domain preferred 逻辑过滤。

默认浏览器也在这一步介入，而不是作为 `chooseBestActivity()` 内的一条用户 PreferredActivity。domain 过滤先把 verified ALWAYS、ALWAYS_ASK、未定义、NEVER 与通用浏览器分组：存在 ALWAYS 时优先保留 verified 候选并用 `linkGeneration` 填 `preferredOrder`；存在 ALWAYS_ASK 时让相关候选重新打平并允许浏览器出现。需要纳入浏览器且未带 `MATCH_ALL` 时，只有默认浏览器确实命中、并且它的 priority 不低于 `max(0, 所有通用浏览器的最高 priority)`，才只保留该默认浏览器；否则保留通用浏览器集合。零下限来自 `maxMatchPrio` 以 0 初始化：当全部通用浏览器 priority 都为负时，最高的默认浏览器也不会被单独保留。

外部 instant resolution 也不直接把未安装应用的 Activity 放进列表，而是可能加入 instant app installer 的 `ResolveInfo`。installer 被设置成 default，并带有用于后续阶段的 auxiliary 信息。合流改变列表后，只有 `sortResult` 为真才触发一次公共 `RESOLVE_PRIORITY_SORTER` 重排；纯当前 profile 的 ComponentResolver 结果已经在 Resolver 内排序。注意 `addInstant` 与 `sortResult` 是独立布尔值：追加 installer 并不必然开启重排，后面的 dynamic-split 替换也是保留原槽位。因此只有 ComponentResolver 本地产出的列表可无条件按六键理解，PMS 最终公开列表不能一概宣称刚刚完整重排过。

这解释了三个看似异常却可能正确的输出：结果组件是 IntentForwarder、结果组件是 installer、Web 候选经 domain 状态后比原始匹配列表更少。它们都发生在完整 Filter 匹配之外，不能通过改 action/category 来解释。

## 14. 列表排序：六个键的顺序固定，末尾稳定性却有限定条件

Activity Resolver 用 `RESOLVE_PRIORITY_SORTER` 排序它本地产出的 `ResolveInfo`，比较键严格按以下顺序：

1. `priority` 降序；
2. `preferredOrder` 降序；
3. `isDefault == true` 在前；
4. `match` 数值降序；
5. `system == true` 在前；
6. packageName 字典序。

这里的 packageName 是 Activity/Service/Provider 对应 Info 中的包名。排序器不比较 Activity class name：同一 package 的两个 Activity 若前五项也完全相同，比较器返回 0。Java 的对象列表排序是稳定排序，因此在这一次排序中会保留输入相对顺序；但输入顺序来自索引数组、cut 顺序、注册顺序及可能的合流，不能把它提升成公开 API 保证，更不能认为 class name 会自动破同分。

manifest `android:order` 也不是这里的 `preferredOrder`。解析阶段会把组件所有 Filter 的最大 order 记到组件，并用它预排包内 Activity、Receiver、Service；它没有被普通 Activity `newResult()` 复制到六键比较器字段。最多在同包且六个比较键全部相等时，通过稳定排序保留下来的输入相对顺序间接可见，不能把两种 order 混写。

去重还会改变可供排序的信息。同一 Activity 的后续 Filter 可能有更高 match，却在完整匹配前因已有结果而跳过；排序器只看留下的那条 `ResolveInfo`。所以“排序代码把更具体 Filter 排错了”之前，必须先确认更具体 Filter 是否真的生成过独立结果。

`priority` 在 `match` 之前，system 在 `match` 之后。系统应用不会天然压过更高 match 的普通应用，更不会压过更高 priority；只有前四项相等时，system 才是破同分键。

## 15. resolve 单项选择：列表第一名、preferred 与 ResolverActivity 不是同一规则

`resolveIntentInternal()` 先规范化 flags、查询列表，再调用 `chooseBestActivity()`。选择逻辑并非重新完整排序：

- 0 项返回 `null`，1 项直接返回该项；
- 多项时只比较传入列表前两项的 `priority`、`preferredOrder`、`isDefault`；三者任一不同，直接返回第一项；列表通常继承 Resolver 排序或合流后的重排，但第13节的追加/原位替换路径可能没有完整重排；
- 三者相同才查 persistent preferred 与用户 preferred activity；
- preferred 没命中时，若候选中有已安装 instant 应用且域名状态不是 ALWAYS_ASK，可直接返回它；
- 调用方要求 `RESOLVE_NON_RESOLVER_ONLY` 时，不允许回退 ResolverActivity，返回 `null`；
- 否则复制系统的 `mResolveInfo`，返回承载用户选择界面的 ResolverActivity。

`chooseBestActivity()` 自己不会补排序。通常传入列表此前已按 `match` 与 `system` 等键排过，所以二者会间接影响前两项；但追加 instant installer 或动态 split 原位替换可以破坏这一前提，代码仍机械读取当前前两项。即使列表确实有序，只要当前三个关键字段相等，第一项因更高 match 排在前面也不会据此直接自动选中，方法仍会继续尝试 preferred，最终可能展示 ResolverActivity。

preferred 也不是只存一个 ComponentName。查找时先看 persistent preferred：它自己的 IntentFilter 必须匹配，目标组件也必须仍在当前 query，但这一分支不拿保存的 match category 与本次最高值比较。未命中才用 Intent 条件查询每用户的 `PreferredIntentResolver`；普通 PreferredActivity 会计算当前 query 的最高 `match & MATCH_CATEGORY_MASK`，要求它等于记录值，并继续校验目标仍在候选、候选集合是否变化、always/last-chosen 语义及包可见性。失效记录可能被清理，不能把 Settings 中存在一行 preferred 当成必然命中。

普通 `queryIntentActivities()` 到列表返回就完成，不执行 `chooseBestActivity()`。这也是“query 第一项”和“resolve 返回值”可能不同的根本原因。

### 练习 9：把排序键与最终选择分成两次阅读

```bash
set -eu
ROOT=${1:-.}
cd "$ROOT"
grep -n -F 'static final Comparator<ResolveInfo> RESOLVE_PRIORITY_SORTER = (r1, r2) -> {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'v1 = r1.preferredOrder;' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (r1.isDefault != r2.isDefault) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'v1 = r1.match;' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'if (r1.system != r2.system) {' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'return r1.activityInfo.packageName.compareTo(r2.activityInfo.packageName);' frameworks/base/services/core/java/com/android/server/pm/ComponentResolver.java
grep -n -F 'private ResolveInfo chooseBestActivity(Intent intent, String resolvedType,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (N == 1) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (r0.priority != r1.priority' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ResolveInfo ri = findPreferredActivityNotLocked(intent, resolvedType,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (ri.activityInfo.applicationInfo.isInstantApp()) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '& PackageManagerInternal.RESOLVE_NON_RESOLVER_ONLY) != 0) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ri = new ResolveInfo(mResolveInfo);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'ResolveInfo pri = findPersistentPreferredActivityLP(intent, resolvedType, flags, query,' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'PreferredIntentResolver pir = mSettings.mPreferredActivities.get(userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'if (pa.mPref.mMatch != match) {' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'info.preferredOrder = linkGeneration;' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F 'mPermissionManager.getDefaultBrowser(userId);' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
grep -n -F '&& defaultBrowserMatch.priority >= maxMatchPrio' frameworks/base/services/core/java/com/android/server/pm/PackageManagerService.java
```

## 16. 收束：用“召回、匹配、可见、排序、选择、启动”六个完成点排障

一次 Activity 解析最省时间的排查顺序如下：

1. 固定 `Intent` 的外层 package/component、selector、action、`resolvedType`、data URI、categories、flags、`filterCallingUid` 与 userId；
2. 显式 component 先查组件表与用户态，不去寻找并不存在的 Filter match；指定 package 查目标包 listCut；完全隐式才查全局 type/scheme/action cut；
3. 对隐式候选区分“被索引召回”与“完整 match 成功”，按 action → data/type → categories → default-only 找第一个拒绝点；
4. 对同一 Activity 多 Filter，记录 cut/数组顺序、哪个 Filter 首先成功加入，以及后续 Filter 是否在 match 前被去重；
5. 非空匹配再检查 `newResult()` 的用户、enabled、Direct Boot、instant 与 ActivityInfo 生成门；随后检查 cross-profile/domain/installer 合流和 post-resolution AppsFilter；
6. ComponentResolver 本地列表按六个 comparator 键解释；若 PMS 后来合流、追加或原位替换结果，还要确认 `sortResult` 是否真的触发；`resolveIntent()` 的最终对象再单独按前三键、preferred、instant 与 ResolverActivity 回退解释；
7. 最后进入 ATMS 验证 exported、permission 与启动政策，不用 PMS 的查询成功替代执行授权。

由此可以给六个完成点下精确定义：索引召回只说明“值得检查”；完整 match 说明“Filter 条件成立”；`newResult` 与 post-filter 说明“本次公开查询或内部启动解析允许保留这条 ResolveInfo”，只有普通公开 query 才能把它解释成调用者可枚举；排序说明“Resolver 本地列表如何排列”；`chooseBestActivity` 说明“resolve 返回谁”；ATMS 检查完成才说明“这次启动被允许继续”。

下一章进入 255：Web Intent 的 autoVerify 如何形成域名验证事实，并与用户策略、默认浏览器及跨 profile 候选共同决定 URL 去向。
