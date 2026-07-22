# Gradle buildSrc 插件使用详解

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件直链、账号标识、组织域名等公司相关信息。
> 图片已下载到 `image/`，附件已下载到 `file/` 并使用相对路径引用；可导出的文本绘图、代码块与表格已尽量保留。

## 一、buildSrc 是什么

buildSrc 是 Gradle 内置的一个**特殊目录**。它的核心特点：

- 放在项目根目录下，目录名必须是 buildSrc（不可改）
- Gradle 在构建**任何模块之前**，会先编译 buildSrc
- 编译产物自动加入所有模块的 **build script classpath**
- 不需要发布到 Maven、不需要在 settings.gradle 中 include

简单理解：**buildSrc 就是一个"本地的、免发布的 Gradle 插件仓库"**。

## 二、目录结构

```kotlin
buildSrc/
├── build.gradle              ← buildSrc 自身的构建配置
├── src/main/
│   ├── kotlin/                   ← Kotlin 源码（插件/Task）
│   │   └── com/example/coverage/
│   │       ├── IncrementalCoveragePlugin.kt
│   │       ├── AnalyzeIncrementalChangesTask.kt
│   │       ├── InstrumentCoverageTask.kt
│   │       └── GenerateCoverageReportTask.kt
│   ├── java/                     ← Java 源码（可混用）
│   │   └── org/jacoco/core/internal/flow/
│   │       ├── CoverageFilter.java
│   │       └── ClassProbesAdapter.java
│   └── resources/
│       └── META-INF/gradle-plugins/
│           └── com.example.coverage.properties  ← 插件ID注册

```

#### 关键文件说明


| 文件 | 作用 |
| --- | --- |
| build.gradle | 定义 buildSrc 自身的依赖（如 AGP、ASM、JaCoCo） |
| src/main/kotlin/ | 插件和 Task 的源码 |
| META-INF/gradle-plugins/xxx.properties | 注册插件 ID，文件名就是插件 ID |


## 三、如何定义一个插件

### 3.1 buildSrc 自身的 build.gradle.kts

```javascript
// buildSrc/build.gradle.kts
plugins {
    `kotlin-dsl`  // 必须启用，提供 Kotlin DSL 支持
}

repositories {
    google()        // Android 相关依赖
    mavenCentral()  // 通用依赖
}

dependencies {
    // 如果要操作 Android 构建流程，必须依赖 AGP
    implementation("com.android.tools.build:gradle:8.6.0")
    // 其他需要的库...
    implementation("org.jacoco:org.jacoco.core:0.8.11")
}

```

### 3.2 实现 Plugin 接口

```kotlin
// src/main/kotlin/com/example/coverage/IncrementalCoveragePlugin.kt
class IncrementalCoveragePlugin : Plugin<Project> {
    override fun apply(project: Project) {
        // 这里的代码在 apply plugin 时执行
        // project 就是应用了这个插件的模块
        println("插件被应用到: ${project.name}")
        
        // 注册自定义 Task
        project.tasks.register("myTask", MyCustomTask::class.java)
    }
}
```

### 3.3 注册插件 ID

创建文件 src/main/resources/META-INF/gradle-plugins/<插件ID>.properties：

```kotlin
## com.example.coverage.properties
## 文件名 = 插件ID，在 build.gradle 中用 apply plugin: 'com.example.coverage' 引用
implementation-class=com.example.coverage.IncrementalCoveragePlugin

```

### 3.4 在模块中使用

```kotlin
// app/build.gradle
apply plugin: 'com.example.coverage'   // 插件ID = properties文件名
```

**就这三步，插件就生效了。不需要发布、不需要版本号、不需要额外配置 classpath。**

## 四、插件的能力

### 4.1 注册自定义 Task

```kotlin
override fun apply(project: Project) {
    // 注册一个 Task，可以通过 ./gradlew myTask 执行
    project.tasks.register("myTask", MyCustomTask::class.java) {
        // 配置 Task 属性
        outputDir.set(project.file("build/outputs/my-output"))
    }
}

```

Task 定义：

```kotlin
abstract class MyCustomTask : DefaultTask() {
    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun execute() {
        // Task 执行逻辑
        println("执行自定义任务")
    }
}

```

### 4.2 拦截 Android 编译流程（AGP API）

 **拦截并修改编译产物**：

```kotlin
override fun apply(project: Project) {
    val androidComponents = project.extensions
        .findByType(AndroidComponentsExtension::class.java)

    androidComponents?.onVariants { variant ->
        // variant.name = "debug" / "release" / "domesticDebug" 等

        val task = project.tasks.register("${variant.name}Transform", MyTransformTask::class.java)
        
        // 关键：拦截 CLASS 产物，在 class → dex 之间做字节码修改
        variant.artifacts
            .forScope(ScopedArtifacts.Scope.ALL)     // ALL = 所有模块+依赖
            .use(task)
            .toTransform(
                ScopedArtifact.CLASSES,               // 拦截 .class 文件
                MyTransformTask::inputJars,           // 输入 JAR 列表
                MyTransformTask::inputDirs,           // 输入目录列表
                MyTransformTask::outputJar            // 输出单一 JAR
            )
    }
}

```

**Scope 对比**：


| Scope | 范围 | 用途 |
| --- | --- | --- |
| PROJECT | 仅当前模块自身的代码 | 单模块处理 |
| ALL | 所有模块 + 所有第三方依赖 | 全工程处理（本项目使用） |


### 4.3 读取/修改 Gradle 配置

```kotlin
override fun apply(project: Project) {
    // 读取 android { } 配置块
    val android = project.extensions.findByType(
        com.android.build.gradle.AppExtension::class.java
    )
    val compileSdk = android?.compileSdkVersion

    // 读取项目属性
    val myProp = project.findProperty("myKey") as? String

    // 添加依赖
    project.dependencies.add("implementation", "com.example:lib:1.0")

    // 遍历所有子模块
    project.rootProject.subprojects.forEach { sub ->
        println("子模块: ${sub.name}")
    }
}

```

### 4.4 注册构建监听器

```kotlin
override fun apply(project: Project) {
    // 在所有模块配置完成后执行
    project.afterEvaluate {
        println("模块 ${project.name} 配置完成")
    }

    // 在构建结束时执行
    project.gradle.buildFinished {
        println("构建结束，耗时: ${it.gradle?.startParameter}")
    }
}

```

### 4.5 生成代码/资源

```kotlin
abstract class GenerateCodeTask : DefaultTask() {
    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun generate() {
        val file = outputDir.get().asFile.resolve("Generated.java")
        file.writeText("""
            package com.example;
            public class Generated {
                public static final String BUILD_TIME = "${System.currentTimeMillis()}";
            }
        """.trimIndent())
    }
}

```

### 4.6 执行外部命令

```kotlin
@TaskAction
fun execute() {
    // 执行 Git 命令
    val process = ProcessBuilder("git", "log", "--oneline", "-5")
        .directory(project.rootDir)
        .start()
    val output = process.inputStream.bufferedReader().readText()
    process.waitFor()
    println(output)
}

```

## 五、buildSrc vs 其他插件方案


| 特性 | buildSrc | 独立插件项目 | build.gradle 脚本 |
| --- | --- | --- | --- |
| 发布到 Maven | ❌ 不需要 | ✅ 需要 | ❌ 不需要 |
| 跨项目复用 | ❌ 仅当前项目 | ✅ 可复用 | ❌ 仅当前项目 |
| IDE 支持 | ✅ 完整（补全/重构） | ✅ 完整 | ⚠️ 有限 |
| 可测试性 | ✅ 可写单元测试 | ✅ 可写单元测试 | ❌ 难以测试 |
| 编译开销 | ⚠️ 每次都编译 | ✅ 只编译一次 | ✅ 无额外编译 |
| 适用场景 | 项目级别的构建逻辑 | 团队/公司级通用工具 | 简单的一次性脚本 |


### 什么时候用 buildSrc？

- ✅ 构建逻辑**只在当前项目**使用
- ✅ 逻辑较复杂，需要拆分成多个类
- ✅ 需要依赖第三方库（如 JavaParser、JaCoCo）
- ✅ 需要 IDE 的代码补全和重构支持
- ❌ 不适合需要跨项目复用的通用工具（应改用独立插件项目）

## 六、项目完整实例

### 编译顺序

```kotlin
① Gradle 发现 buildSrc/ 目录
② 编译 buildSrc/build.gradle.kts 中的依赖
③ 编译 buildSrc/src/main/ 下所有源码
④ 产物自动加入所有模块的 classpath
⑤ Gradle 开始配置各模块
⑥ app/build.gradle 执行 apply plugin: 'com.example.coverage'
⑦ IncrementalCoveragePlugin.apply() 被调用
⑧ onVariants 回调注册（延迟到 variant 确定后执行）
⑨ 编译阶段：analyzeIncrementalChanges → instrumentCoverage 自动执行

```

### 各组件协作关系

```kotlin
buildSrc/build.gradle.kts
    └── 声明依赖: AGP, ASM, JaCoCo, JavaParser
META-INF/gradle-plugins/com.example.coverage.properties
    └── 注册插件ID → IncrementalCoveragePlugin
IncrementalCoveragePlugin.apply()
    ├── 注册 AnalyzeIncrementalChangesTask   ← git diff + AST 分析
    ├── 注册 InstrumentCoverageTask          ← 字节码插桩
    ├── 注册 GenerateCoverageReportTask      ← HTML 报告
    └── variant.artifacts.toTransform()      ← 拦截 .class 产物
CoverageFilter + ClassProbesAdapter
    └── 同包覆盖 JaCoCo 原始类，实现方法级过滤
        （Java 的类加载机制：buildSrc 的类优先于 JAR 中的同名类）
```

### 为什么 CoverageFilter 放在 org.jacoco.core.internal.flow 包下？

这是一个**同包覆盖**技巧。ClassProbesAdapter 是 JaCoCo 的核心类，它在 org.jacoco.core JAR 包中。 我们在 buildSrc 中创建了同包名、同类名的 ClassProbesAdapter.java，利用 **classpath 优先级**（buildSrc > 外部 JAR）， Gradle 会加载我们修改后的版本而不是 JAR 中的原始版本。CoverageFilter 也放在同包下，方便 ClassProbesAdapter 直接调用。

## 七、常用 API 速查

```typescript
// ===== Project API =====
project.name                          // 模块名
project.rootDir                       // 项目根目录
project.buildDir                      // 当前模块的 build 目录
project.file("path")                  // 相对路径转 File
project.rootProject                   // 根 Project
project.rootProject.subprojects       // 所有子模块
project.findProperty("key")          // 读取 gradle.properties

// ===== Task API =====
project.tasks.register("name", MyTask::class.java)    // 注册（延迟创建）
project.tasks.named("name")                            // 获取已有 Task
taskA.configure { dependsOn(taskB) }                   // 依赖关系

// ===== Android API（需依赖 AGP） =====
project.extensions.findByType(AndroidComponentsExtension::class.java)
androidComponents.onVariants { variant -> ... }         // variant 回调
variant.artifacts.forScope(Scope.ALL).use(task).toTransform(...)  // 拦截产物

// ===== Task 注解 =====
@TaskAction      // 标记 Task 的执行方法
@InputFile        // 输入文件（变化时触发重新执行）
@InputFiles       // 输入文件集合
@OutputFile       // 输出文件
@OutputDirectory  // 输出目录

```
