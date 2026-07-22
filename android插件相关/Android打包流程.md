# Android 打包流程

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件直链、账号标识、组织域名等公司相关信息。
> 图片已下载到 `image/`，附件已下载到 `file/` 并使用相对路径引用；可导出的文本绘图、代码块与表格已尽量保留。

## Android 从源码到 APK 的完整编译流程

## 1、先看全局：谁在干活？

现代 Android 工程通常不是 Android Studio 自己在编译，而是由 Gradle + Android Gradle Plugin，简称 AGP 驱动。Android Studio 只是调用 Gradle 任务，例如：

```java
./gradlew :app:assembleDebug
./gradlew :app:assembleRelease
./gradlew :commonlibs:compileDebugJavaWithJavac
```

Gradle 负责“任务调度”和“依赖解析”，AGP 负责注册 Android 专属任务，例如资源编译、Manifest 合并、Dex 生成、APK 打包、签名等。官方文档也说明，Android build system 会编译 app 的资源和源码，并打包成 APK 或 AAB；Gradle 使用 task-based 方式组织这些命令，插件负责定义任务和配置。(developer.android.com)


> [!NOTE]
> 原文此处为只读绘图/流程图块；当前文档导出接口未返回可还原内容，已保留此位置。


## 2、启动阶段：./gradlew assembleDebug

### 干什么？

当你执行：

```java
./gradlew :app:assembleDebug
```

Gradle 会启动构建，读取工程结构、插件、依赖、任务图，然后执行目标任务依赖的一串任务。

常见入口：

```java
./gradlew :app:assembleDebug
./gradlew :app:assembleRelease
./gradlew :app:bundleRelease
./gradlew :lib:assembleDebug
```

其中：


| 命令 | 结果 |
| --- | --- |
| assembleDebug | 生成 debug APK |
| assembleRelease | 生成 release APK |
| bundleRelease | 生成 AAB |
| compileDebugJavaWithJavac | 只跑 Java 编译相关任务，常用于验证某模块 Java 编译 |


### 怎么干涉？

主要改这些文件：

```java
settings.gradle / settings.gradle.kts
build.gradle / build.gradle.kts
gradle.properties
gradle/libs.versions.toml
```

比如：

```java
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.xxx.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.xxx.app"
        minSdk = 23
        targetSdk = 35
    }
}
```

你想改“用哪个 AGP、哪个 Kotlin、哪个依赖版本”，就是这个阶段干涉。

## 3、Gradle 三阶段：初始化、配置、执行

Gradle 构建可以理解成三个阶段：

### 3.1 Initialization 初始化阶段

读取：

```java
settings.gradle
```

决定这个工程有哪些 module：

```java
pluginManagement { }
dependencyResolutionManagement { }

rootProject.name = "Android-Mower"

include(":app")
include(":commonlibs")
include(":deviceModule")
```

### 3.2 Configuration 配置阶段

读取每个 module 的：

```java
build.gradle
```

AGP 会根据你的配置创建 variant，例如：

```java
debug
release
googleDebug
chinaRelease
```

如果你有：

```java
buildTypes {
    debug { }
    release { }
}

productFlavors {
    create("domestic") { }
    create("oversea") { }
}
```

那 AGP 会组合出：

```java
domesticDebug
domesticRelease
overseaDebug
overseaRelease
```

每个 variant 都有自己的一套任务、依赖、资源、Manifest、BuildConfig 等。

### 3.3 Execution 执行阶段

真正执行任务，例如：

```java
:app:mergeDebugResources
:app:processDebugMainManifest
:app:compileDebugKotlin
:app:compileDebugJavaWithJavac
:app:dexBuilderDebug
:app:mergeDexDebug
:app:packageDebug
:app:assembleDebug
```

#### 方式一：Android DSL

最常用：

```javascript
android {
    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/java")
            res.srcDirs("src/main/res")
            assets.srcDirs("src/main/assets")
        }
    }
}
```

配置 release 包是否混淆/压缩，配置 源码、资源、assets 的目录位置

#### 方式二：Gradle task hook

比如在某任务前后加逻辑：

```java
tasks.named("preBuild") {
    doLast {
        println("preBuild finished")
    }
}
```

#### 方式三：AGP Variant API

更推荐用于现代 AGP：

```java
androidComponents {
    onVariants { variant ->
        println("variant = ${variant.name}")
    }
}
```

#### 方式四：自定义 Gradle Plugin

如果你想系统性改编译流程，比如插桩、检查资源、生成代码、接入埋点，应该做 Gradle Plugin

## 4、Variant 创建：Debug / Release / Flavor 是怎么来的？

Android 构建不是简单编译一个 app，而是编译某个 Variant。

一个 Variant 通常由这些维度组成：

```java
buildType + productFlavor + sourceSet + signingConfig + dependencies
```

例如：

```java
overseaRelease
```

可能使用：

```java
src/main/
src/oversea/
src/release/
src/overseaRelease/
```

资源、Manifest、Java/Kotlin 源码都可能来自不同 sourceSet。

### 怎么干涉？

#### 改 buildType

```java
android {
    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            isDebuggable = true
        }

        release {
            isMinifyEnabled = true
            isShrinkResources = true
        }
    }
}
```

#### 改 flavor

```javascript
android {
    flavorDimensions += "region"

    productFlavors {
        create("cn") {
            dimension = "region"
        }

        create("global") {
            dimension = "region"
        }
    }
}
```

#### 改 sourceSet

```java
android {
    sourceSets {
        getByName("debug") {
            res.srcDirs("src/debug/res")
        }
    }
}
```

## 5、依赖解析：AAR / JAR / module 先被拿进来

### 5.1 Gradle 先解析依赖图

比如：

```java
dependencies {
    implementation(project(":commonlibs"))
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation(files("libs/foo.jar"))
    implementation(files("libs/bar.aar"))
}

```

Gradle 会解析：

```java
当前 module 依赖
传递依赖
版本冲突
compileClasspath
runtimeClasspath
annotationProcessorClasspath
kspClasspath
```

官方文档说明，Gradle Android 工程可以引入本地或远程依赖，并自动包含它们声明的传递依赖。(developer.android.com)

### 4.2 implementation、api、compileOnly 的区别


| 配置 | 编译可见 | 运行打包 | 会不会暴露给下游 |
| --- | --- | --- | --- |
| implementation | 当前模块可见 | 会打包 | 不暴露 |
| api | 当前模块可见 | 会打包 | 暴露给依赖本模块的人 |
| compileOnly | 当前模块可见 | 不打包 | 不暴露 |
| runtimeOnly | 编译不可见 | 会打包 | 不暴露 |
| annotationProcessor | 注解处理器用 | 不直接打包 | 不暴露 |
| ksp | KSP 处理器用 | 不直接打包 | 不暴露 |


举例：

```java
dependencies {
    api("androidx.annotation:annotation:1.8.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    compileOnly("javax.annotation:jsr250-api:1.0")
}
```

如果 commonlibs 用 api 暴露了某个库，那么 app 依赖 commonlibs 后，也能直接看到这个库的类型。

如果用 implementation，app 编译时看不到它，除非自己也声明依赖。

## 6、AAR 在编译阶段怎么处理？

AAR 本质是 Android Library 的压缩包，它里面不只是 .class，还可能包含资源、Manifest、assets、jni、ProGuard 规则等。

一个典型 AAR 内部类似：

```java
foo.aar
├── AndroidManifest.xml
├── classes.jar
├── R.txt
├── res/
├── assets/
├── libs/
│   └── xxx.jar
├── jni/
│   ├── arm64-v8a/libxxx.so
│   └── armeabi-v7a/libxxx.so
├── proguard.txt / consumer-rules.pro
└── prefab/
```

AAR 会被 AGP 拆开，然后不同内容进入不同阶段：


| AAR 内容 | 去哪里 |
| --- | --- |
| AndroidManifest.xml | 参与 Manifest merge |
| res/ | 参与资源 merge / AAPT2 编译 |
| classes.jar | 进入 Java/Kotlin compile classpath，之后进入 D8/R8 |
| libs/*.jar | 同样进入 classpath / dex |
| assets/ | 合并到 APK assets |
| jni/*.so | 合并到 APK lib 目录 |
| consumer-rules.pro | 传递给 app 的 R8 / ProGuard |
| R.txt | 用于资源符号引用和非传递 R 类等场景 |
| prefab/ | 给 C/C++ native dependency 使用 |


官方文档也提到，AAR 可以携带 native 依赖，prefab 目录可以包含 native dependency 的 headers 和 libraries。(developer.android.com)

### AAR 不是被“重新编译”

这是很多人容易误解的点。

依赖进来的 AAR 通常已经是编译好的 library artifact：

```java
AAR 的 Java/Kotlin 源码不会再编译
AAR 的 classes.jar 会直接进入 classpath
AAR 的资源会重新和 app 资源一起 link
AAR 的 manifest 会重新 merge
AAR 的 consumer rules 会影响最终 R8
AAR 的 so/assets 会参与最终打包
```

所以你改 implementation("xxx.aar") 时，本质上是把一个已经产出的 library 作为输入。

## 7、JAR 在编译阶段怎么处理？

JAR 相比 AAR 简单很多。

一个普通 JAR 里面通常是：

```java
foo.jar
├── com/xxx/A.class
├── com/xxx/B.class
└── META-INF/
```

JAR 只能提供 Java bytecode 和普通资源，不能提供 Android 资源。


| 能力 | JAR | AAR |
| --- | --- | --- |
| Java/Kotlin class | 是 | 是 |
| AndroidManifest.xml | 否 | 是 |
| res/layout/drawable/string | 否 | 是 |
| assets | 一般可以有，但 Android 资源意义弱 | 是 |
| jniLibs .so | 一般不规范 | 是 |
| consumer ProGuard rules | 否 | 是 |
| Android Library 标准产物 | 否 | 是 |


JAR 在编译阶段的处理：

```java
1. 放进 compileClasspath，供 Java/Kotlin 编译引用
2. 放进 runtimeClasspath，供打包阶段使用
3. 被 D8/R8 转成 dex
4. JAR 里的 META-INF 等资源可能参与 packaging，冲突时需要 packagingOptions 处理
```

### 怎么干涉 JAR / AAR？

```javascript
dependencies {
    implementation(files("libs/foo.jar"))
    implementation(files("libs/bar.aar"))

    implementation("com.xxx:lib:1.0.0") {
        exclude(group = "com.bad", module = "bad-lib")
    }
}
```

处理冲突：

```java
android {
    packaging {
        resources {
            excludes += "META-INF/LICENSE*"
            excludes += "META-INF/DEPENDENCIES"
        }

        jniLibs {
            pickFirsts += "lib/**/libc++_shared.so"
        }
    }
}
```

还有ASM等

## 8、Manifest 合并阶段

### 干什么？

Android 最终只能有一个 AndroidManifest.xml，但实际来源很多：

```java
app/src/main/AndroidManifest.xml
src/debug/AndroidManifest.xml
flavor Manifest
各个 AAR 的 AndroidManifest.xml
测试 Manifest
```

Manifest Merger 会把它们合成一个最终 Manifest。

官方文档说明，Manifest merger 会按优先级和 merge 规则组合多个 manifest，并且可以用特殊 XML 属性定义合并偏好。(developer.android.com)

### 输入

```java
主 Manifest
buildType Manifest
flavor Manifest
AAR Manifest
manifestPlaceholders
navigation / provider 等生成内容
```

### 输出

```java
build/intermediates/merged_manifest/...
```

最终 APK 里的 Manifest 会变成 binary XML。

### 常见问题

#### 权限被 AAR 带进来了

比如某个 AAR 里声明：

```java
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
```

最终 app 也会合进去。

#### Activity exported 冲突

Android 12 以后有 intent-filter 的组件需要明确 android:exported。

#### provider authority 冲突

多个库可能用了相同 authority。

### 怎么干涉？

```java
android {
    defaultConfig {
        manifestPlaceholders["appAuthRedirectScheme"] = "myapp"
    }
}
```

Manifest 中：

```java
<data android:scheme="${appAuthRedirectScheme}" />
```

#### tools 节点控制合并

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:tools="http://schemas.android.com/tools">

    <uses-permission
        android:name="android.permission.ACCESS_FINE_LOCATION"
        tools:node="remove" />

    <application
        tools:replace="android:theme">
    </application>
</manifest>
```

#### 看最终结果

Android Studio 有 Merged Manifest 视图；命令行也可以看：

```java
app/build/intermediates/merged_manifest/debug/AndroidManifest.xml
```

## 9、资源编译阶段：AAPT2

### 干什么？

Android 资源不是直接塞进 APK 就完了。res/layout、res/drawable、res/values 等会经过 AAPT2 编译和链接。

AAPT2 是 Android Studio 和 AGP 使用的资源编译/打包工具，会解析、索引并把资源编译成 Android 平台优化过的二进制格式。(developer.android.com)

资源阶段大概分两步：

```java
AAPT2 compile
AAPT2 link
```

### 8.1 AAPT2 compile

把每个资源文件单独编译成中间格式。

输入：

```java
src/main/res/layout/activity_main.xml
src/main/res/drawable/icon.xml
src/main/res/values/strings.xml
AAR 里的 res/
```

输出：

```java
compiled resources
```

这一步适合增量编译：你只改一个 layout，理论上只需要重新编译这个 layout。

### 8.2 AAPT2 link

把所有资源链接到一起，生成最终资源表。

输出包括：

```java
resources.arsc
R.java / R.jar / R.txt
binary AndroidManifest.xml
res/*.xml binary format
```

### 资源冲突怎么发生？

比如 app 和 AAR 都定义了：

```java
<string name="app_name">xxx</string>
```

资源合并时会按优先级覆盖或报错。

通常优先级大致是：

```java
app 自己 > flavor > buildType > main > library dependency
```

### 怎么干涉？

#### 改资源目录

```java
android {
    sourceSets {
        getByName("main") {
            res.srcDirs("src/main/res", "src/common/res")
        }
    }
}
```

#### 配置 resValue

```java
android {
    defaultConfig {
        resValue("string", "api_host", "https://api.example.com")
    }
}
```

#### 资源 shrink

```java
android {
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
        }
    }
}
```

#### 资源冲突排除

```java
android {
    packaging {
        resources {
            excludes += "META-INF/*"
        }
    }
}
```

## 10、BuildConfig / R / DataBinding / ViewBinding / 生成代码

在 Java/Kotlin 真正编译前，AGP 和插件会先生成一批代码。

常见生成内容：

```java
BuildConfig.java
R.java / R.jar
DataBinding 生成类
ViewBinding 生成类
Room 生成代码
Dagger / Hilt 生成代码
Glide GeneratedAppGlideModule
Router 表
ARouter / TheRouter 映射表
KSP 生成代码
KAPT 生成代码
```

### BuildConfig

如果启用：

```java
android {
    buildFeatures {
        buildConfig = true
    }
}
```

可以生成：

```java
public final class BuildConfig {
    public static final boolean DEBUG = true;
    public static final String VERSION_NAME = "1.0";
}
```

干涉：

```sql
android {
    defaultConfig {
        buildConfigField("String", "API_HOST", "\"https://api.example.com\"")
        buildConfigField("boolean", "LOG_ENABLE", "true")
    }
}
```

### ViewBinding

```java
android {
    buildFeatures {
        viewBinding = true
    }
}
```

生成：

```java
ActivityMainBinding
FragmentHomeBinding
```

### DataBinding

```java
android {
    buildFeatures {
        dataBinding = true
    }
}
```

这会多出 DataBinding 相关的分析和代码生成任务。

## 11、Kotlin / Java 编译阶段

### 干什么？

这一阶段把源码编译成 .class 字节码。

输入：

```java
src/main/java
src/main/kotlin
生成代码目录
依赖 AAR 的 classes.jar
依赖 JAR
Android bootClasspath
```

输出：

```java
.class 文件
```

典型任务：

```java
compileDebugKotlin
compileDebugJavaWithJavac
kaptDebugKotlin
kspDebugKotlin
```

### Kotlin 和 Java 怎么配合？

常见顺序不是绝对固定，但可以理解成：

```java
KSP/KAPT 生成代码
Kotlin 编译
Java 编译
```

Kotlin 编译器需要能看到 Java stub / classpath。Java 编译器也需要看到 Kotlin 编译输出。

### AAR/JAR 在这里做什么？

AAR 的：

```java
classes.jar
libs/*.jar
```

JAR 的：

```java
*.class
```

会进入 compileClasspath。

它们不会重新编译，只是给当前源码提供类型引用。

例如你的代码：

```java
OkHttpClient client = new OkHttpClient();
```

那 okhttp.jar 或 okhttp AAR 中的 class 需要在 compileClasspath 上，否则 Java/Kotlin 编译就报：

```java
Cannot resolve symbol OkHttpClient
```

### 怎么干涉？

#### Java 编译参数

```java
android {
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
```

#### Kotlin 编译参数

```java
kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}
```

#### 注解处理器

```java
dependencies {
    kapt("com.google.dagger:hilt-compiler:xxx")
    ksp("androidx.room:room-compiler:xxx")
}
```

#### 编译失败排查

```java
./gradlew :app:compileDebugKotlin
./gradlew :app:compileDebugJavaWithJavac
```

## 12、字节码处理 / 插桩阶段

源码编译成 .class 后，某些插件可能会修改字节码。

比如：

```java
埋点插桩
日志插桩
性能 Trace
路由扫描
热修复
代码覆盖率
ASM transform
```

旧时代常用 Transform API；现代 AGP 推荐使用新的 instrumentation / artifacts API。官方博客说明，AGP 8.0 移除了 Transform API，并建议用更细分、更高效的新 API，例如 Instrumentation API 可用 ASM callback 分析或转换已编译 class。(android-developers.googleblog.com)

### 怎么干涉？

#### 自定义 Gradle Plugin + ASM

思路：

```java
1. 写 Gradle Plugin
2. 使用 androidComponents.onVariants
3. 注册 instrumentation transform
4. 用 ASM ClassVisitor 修改 class
```

适合做：

```java
方法耗时统计
自动埋点
权限调用扫描
日志注入
```

但不建议业务代码里到处乱 hook task，因为 AGP 版本升级容易炸。

## 13、D8 / R8：从 .class 到 .dex

Android 运行时不直接运行 JVM .class，而是运行 DEX。

### Debug 常见路径：D8

D8 把 Java bytecode 编译成 DEX bytecode。官方文档说明，D8 是 Android Studio 和 AGP 使用的命令行工具，用于把 Java bytecode 编译成 Android 设备可运行的 DEX bytecode。(developer.android.com)

输入：

```java
当前模块 .class
依赖 AAR classes.jar
依赖 JAR
生成代码 .class
```

输出：

```java
classes.dex
classes2.dex
...
```

如果方法数超过限制，会产生 multidex。

### Release 常见路径：R8

Release 开启混淆压缩时，R8 会参与：

```java
android {
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
        }
    }
}
```

R8 负责：

```java
代码压缩 shrink
代码优化 optimize
混淆 obfuscate
生成 dex
应用 keep rules
```

官方文档说明，R8 会通过移除无用代码和资源、重写代码等方式优化 app；其优化流程包括 code shrinking、logical optimization 等。(developer.android.com)

### R8 输入规则从哪里来？

```java
app/proguard-rules.pro
getDefaultProguardFile(...)
AAR consumer-rules.pro
AGP 默认规则
Kotlin / AndroidX / 反射相关规则
```

AR 的 consumerProguardFiles 很关键。库作者可以写：

```java
android {
    defaultConfig {
        consumerProguardFiles("consumer-rules.pro")
    }
}
```

这样 app 使用这个 AAR 时，这些规则会自动进入 app 的 R8。

### 怎么干涉？

#### keep 某个类

```java
-keep class com.xxx.Foo { *; }
```

#### keep 注解类

```java
-keep @androidx.annotation.Keep class * { *; }
-keepclassmembers class * {
    @androidx.annotation.Keep *;
}
```

#### 打印 R8 结果

```java
-printmapping mapping.txt
-printusage usage.txt
```

#### 常见排查

```java
./gradlew :app:minifyReleaseWithR8
```

看：

```java
app/build/outputs/mapping/release/mapping.txt
app/build/outputs/mapping/release/usage.txt
```

## 14、Dex 合并阶段

多来源 dex 会被合并。

输入：

```java
当前模块 dex
依赖模块 dex
依赖 JAR/AAR 转出来的 dex
```

输出：

```java
classes.dex
classes2.dex
...
```

Debug 构建为了快，可能会有更细粒度的 dex builder / merge dex 任务。Release 则更依赖 R8 整体处理。

### 怎么干涉？

#### multidex

```java
android {
    defaultConfig {
        multiDexEnabled = true
    }
}
```

依赖：

```java
dependencies {
    implementation("androidx.multidex:multidex:2.0.1")
}
```

#### main dex rules

某些低版本机型需要主 dex 保留启动类：

```java
android {
    defaultConfig {
        multiDexKeepProguard = file("multidex-rules.pro")
    }
}
```

## 15、Native / JNI / .so 合并阶段

如果依赖中有 .so：

```java
src/main/jniLibs/arm64-v8a/libxxx.soAAR/jni/arm64-v8a/libxxx.so
```

最终会被打到 APK：

```java
lib/arm64-v8a/libxxx.solib/armeabi-v7a/libxxx.so
```

### 怎么干涉？

#### 指定 ABI

```java
android {
    defaultConfig {
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }
}
```

#### 解决 so 冲突

```java
android {
    packaging {
        jniLibs {
            pickFirsts += "lib/**/libc++_shared.so"
        }
    }
}
```

#### 是否压缩 so

```java
android {
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

```

## 16、Assets / Java resources / META-INF 合并

### assets

来源：

```java
src/main/assets
AAR/assets
```

最终进入 APK：

```java
assets/xxx
```

### Java resources

JAR 里可能有：

```java
META-INF/LICENSE
META-INF/services/xxx
```

这些可能产生冲突。

### 怎么干涉？

```java
android {
    packaging {
        resources {
            excludes += "META-INF/LICENSE*"
            excludes += "META-INF/NOTICE*"
            excludes += "META-INF/DEPENDENCIES"
            pickFirsts += "META-INF/services/**"
        }
    }
}
```

## 17、APK 打包阶段

这个阶段把所有东西装进 APK。

APK 本质是 zip 包，内部类似：

```java
app-debug.apk
├── AndroidManifest.xml
├── classes.dex
├── classes2.dex
├── resources.arsc
├── res/
├── assets/
├── lib/
│   └── arm64-v8a/libxxx.so
├── META-INF/
└── kotlin/
```

输入：

```java
binary AndroidManifest.xml
resources.arsc
compiled res
dex
assets
jniLibs
Java resources
```

输出：

```java
app/build/outputs/apk/debug/app-debug.apk
```

### 怎么干涉？

#### 改 APK 名称

```java
androidComponents {
    onVariants { variant ->
        variant.outputs.forEach { output ->
            // AGP 新版本需要用对应 API 设置 outputFileName，写法随版本略有差异
        }
    }
}
```

#### 排除资源

```java
android {
    packaging {
        resources {
            excludes += "**/*.proto"
        }
    }
}
```

#### 控制语言资源

```java
android {
    defaultConfig {
        resourceConfigurations += listOf("en", "zh")
    }
}
```

## 18、zipalign

### 干什么？

zipalign 会优化 APK 内部 zip 条目的对齐方式，让运行时读取资源更高效。

通常 release 包会经历：

```java
package
zipalign
sign
```

或者根据签名版本顺序有所调整，但你可以理解为 APK 打包后需要对齐和签名。

### 怎么干涉？

一般不直接手动干涉，AGP 自动处理。

如果你手动处理 APK，可能会用：

```java
zipalign -v -p 4 input.apk aligned.apk
```

## 19、签名阶段

Android APK 必须签名才能安装。

Debug 构建默认用 debug keystore：

```java
~/.android/debug.keystore
```

Release 构建需要配置签名：

```sql
android {
    signingConfigs {
        create("release") {
            storeFile = file("release.keystore")
            storePassword = "xxx"
            keyAlias = "xxx"
            keyPassword = "xxx"
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("release")
        }
    }
}
```

### 签名版本

常见：

```java
v1 JAR signing
v2 APK Signature Scheme
v3
v4
```

### 怎么干涉？

```java
android {
    signingConfigs {
        getByName("release") {
            enableV1Signing = true
            enableV2Signing = true
            enableV3Signing = true
        }
    }
}

```

## 20、最终产物：APK / AAB

### APK

直接安装到设备

```java
adb install app-debug.apk
```

### AAB

AAB 是发布格式，不是直接安装格式。官方文档说明，Android App Bundle 包含编译后的代码和资源，APK 生成和签名可以交给 Google Play，Google Play 会根据设备配置生成优化后的 APK。(developer.android.com)

生成：

```java
./gradlew :app:bundleRelease
```

输出：

```java
app/build/outputs/bundle/release/app-release.aab
```

### APK 和 AAB 区别


| 产物 | 用途 |
| --- | --- |
| APK | 可直接安装 |
| AAB | 上传 Google Play，由 Play 生成设备专属 APK |
| Universal APK | 从 AAB 转出来的通用 APK |
| Split APK | 针对 ABI、语言、屏幕密度拆分的 APK |


## 21、一条 Debug 构建链路示例

```java
./gradlew :app:assembleDebug
```

为例，概念上会经过：

```markdown
1. 初始化 Gradle
2. 配置项目
3. 创建 debug variant
4. 解析 debugCompileClasspath / debugRuntimeClasspath
5. 合并 Manifest
6. 合并资源
7. AAPT2 compile / link
8. 生成 R / BuildConfig / ViewBinding 等
9. KSP / KAPT
10. Kotlin 编译
11. Java 编译
12. 字节码处理
13. D8 转 dex
14. 合并 dex
15. 合并 assets / jniLibs / java resources
16. packageDebug
17. zipalign
18. debug signing
19. assembleDebug
```

Debug 构建一般：

```java
不混淆
不 shrink
尽量增量
编译快
可调试
```

## 22、一条 Release 构建链路示例

```java
./gradlew :app:assembleRelease
```

概念上：

```markdown
1. 初始化 / 配置 / 创建 release variant
2. 解析 release 依赖
3. Manifest merge
4. AAPT2 资源处理
5. 生成代码
6. Kotlin / Java 编译
7. R8 shrink / optimize / obfuscate
8. resource shrink
9. dex 输出
10. packageRelease
11. zipalign
12. release signing
13. 输出 APK
```

Release 构建一般：

```java
开启 R8
开启资源 shrink
签正式证书
不可调试
代码可能被混淆
构建更慢
```

## 23、“我想改这里”应该从哪里下手？

下面是一个速查表。


| 你想改什么 | 改哪里 |
| --- | --- |
| 改包名 | defaultConfig.applicationId |
| 改 namespace | android.namespace |
| 改编译 SDK | compileSdk |
| 改 minSdk / targetSdk | defaultConfig |
| 改 Debug / Release 行为 | buildTypes |
| 改渠道包 | productFlavors |
| 改资源目录 | sourceSets.res.srcDirs |
| 改 Java/Kotlin 源码目录 | sourceSets.java.srcDirs |
| 改 assets | sourceSets.assets.srcDirs |
| 改 Manifest 变量 | manifestPlaceholders |
| 移除某个权限 | Manifest tools:node="remove" |
| 改依赖版本 | dependencies / version catalog |
| 排除传递依赖 | exclude(group, module) |
| 改混淆规则 | proguard-rules.pro |
| 给库提供 keep 规则 | consumerProguardFiles |
| 改 so ABI | ndk.abiFilters |
| 解决 META-INF 冲突 | packaging.resources.excludes |
| 解决 so 冲突 | packaging.jniLibs.pickFirsts |
| 改签名 | signingConfigs |
| 插桩 class | AGP Instrumentation API / ASM |
| 生成代码 | KSP / KAPT / Gradle task |
| 改 APK 输出名 | AGP Variant API |
| 构建前后做检查 | 自定义 Gradle task / plugin |


## 24、AAR / JAR 在不同阶段的处理总结

### AAR 处理流程


> [!NOTE]
> 原文此处为只读绘图/流程图块；当前文档导出接口未返回可还原内容，已保留此位置。


### JAR 处理流程  


> [!NOTE]
> 原文此处为只读绘图/流程图块；当前文档导出接口未返回可还原内容，已保留此位置。


### 核心区别

```java
AAR = Android library，带资源、Manifest、assets、so、consumer rules
JAR = Java bytecode library，主要只有 class 和 Java resources
```

所以：

```java
AAR 会参与 Manifest merge 和资源编译
JAR 不会参与 Android res / Manifest merge
AAR 和 JAR 的 class 最终都会进 D8/R8
```

## 25、常用排查命令

### 看任务图

```java
./gradlew :app:assembleDebug --dry-run
```

### 看详细日志

```java
./gradlew :app:assembleDebug --info
```

### 看依赖树

```java
./gradlew :app:dependencies
```

或指定配置：

```java
./gradlew :app:dependencies --configuration debugRuntimeClasspath
./gradlew :app:dependencies --configuration debugCompileClasspath
```

### 查某个依赖为什么被引入

```java
./gradlew :app:dependencyInsight \
  --dependency okhttp \
  --configuration debugRuntimeClasspath
```

### 只验证 Java 编译

```java
./gradlew :commonlibs:compileDebugJavaWithJavac
```

### 只验证 Kotlin 编译

```java
./gradlew :app:compileDebugKotlin
```

### 看最终 Manifest

```java
app/build/intermediates/merged_manifest/debug/AndroidManifest.xml
```

### 看 APK 内容

```java
unzip -l app-debug.apk
```

或者：

```java
aapt dump badging app-debug.apk
```

## 26、最后用一句话总结

Android 编译不是“一步把代码变 APK”，而是一条由 Gradle 调度、AGP 编排的流水线：

```java
配置工程
→ 创建 Variant
→ 解析 AAR/JAR/module 依赖
→ 合并 Manifest
→ AAPT2 编译资源
→ 生成 R / BuildConfig / Binding / 注解代码
→ Kotlin/Java 编译成 class
→ 字节码插桩
→ D8/R8 转 dex
→ 合并资源/assets/so/dex
→ 打 APK
→ zipalign
→ 签名
→ 输出 APK/AAB
```

如果你要“干涉编译流程”，优先顺序建议是：

```markdown
1. Android DSL
2. dependencies / sourceSets / packaging / signingConfigs
3. manifest tools 规则
4. proguard / consumer rules
5. AGP Variant API
6. 自定义 Gradle Plugin
7. ASM instrumentation
```

不要一上来就 hook 具体 task 名称，因为 AGP 版本升级后 task 名、输入输出、执行时机都可能变化；能用 DSL 和 Variant API 解决的，就不要硬改 task。
