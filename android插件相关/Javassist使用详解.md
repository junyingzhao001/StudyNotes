# Javassist使用详解

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件直链、账号标识、组织域名等公司相关信息。
> 图片已下载到 `image/`，附件已下载到 `file/` 并使用相对路径引用；可导出的文本绘图、代码块与表格已尽量保留。

## **1、Javassist 是什么？**

Javassist 是一个 Java 字节码操作库。你可以把它理解成：**在 Java 类被 JVM 加载前，或者运行时，修改/生成 .**class **文件的工具**。

它常用于：

- AOP：方法前后插入日志、埋点、耗时统计、异常处理。
- 动态代理：运行时生成接口实现类或包装类。
- ORM/DTO：运行时生成 Bean、Getter、Setter。
- Java Agent：应用启动时批量改造某些类。
- 插件化/热修复：修改某个 ClassLoader 中的类副本。
- 框架底层增强：类似 Hibernate、MyBatis、Spring 某些扩展场景中的运行时增强。

和 ASM 相比，Javassist 更适合新手，因为 Javassist 可以用接近 Java 源码字符串的方式写增强逻辑；ASM 更底层，需要理解 JVM 指令。

## **2. Demo项目结构**

```kotlin
JavassistDemo
├── build.gradle.kts
├── docs
│   └── Javassist全场景Demo分享文档.md
└── src/main/java/org/example/javassistdemo
    ├── JavassistAllScenesDemo.java       # 全场景主入口
    ├── generated/GeneratedLookupAnchor.java
    ├── loader/ChildFirstClassLoader.java # 自定义类加载器 Demo
    └── samples                           # 被增强的普通业务类
        ├── ApiMarker.java
        ├── Greeter.java
        ├── LocalCache.java
        ├── MarkedApi.java
        ├── OrderService.java
        ├── PaymentService.java
        ├── PluginTask.java
        ├── User.java
        └── UserService.java
```

依赖配置：

```kotlin
plugins {
    kotlin("jvm") version "2.2.10"
    application
}

repositories {
    mavenCentral()
}

dependencies {
    implementation("org.javassist:javassist:3.30.2-GA")
    testImplementation(kotlin("test"))
}

application {
    mainClass.set("org.example.javassistdemo.JavassistAllScenesDemo")
}
```

## **3、学 Javassist 先理解 4 个核心概念**

### **3.1 ClassPool**

ClassPool 是 Javassist 的“类仓库”。你要找类、创建类、修改类，基本都从它开始。

```kotlin
ClassPool pool = ClassPool.getDefault();
CtClass ctClass = pool.get("java.lang.String");
```

### **3.2 CtClass**

CtClass 是 Javassist 对一个类的抽象。它代表一个还没有被 JVM 正式定义，或者正在被修改的类。

你可以通过它：

- 新增字段：addField
- 新增方法：addMethod
- 新增构造器：addConstructor
- 找已有方法：getDeclaredMethod
- 修改类名：setName 或 getAndRename
- 生成真正的 Class：toClass
- 写出 .class：writeFile

### **3.3 CtField / CtMethod / CtConstructor**

这三个分别代表字段、方法、构造器。

```kotlin
CtField field = CtField.make("private String name;", ctClass);
CtMethod method = CtNewMethod.make("public String hello() { return \"hi\"; }", ctClass);
CtConstructor constructor = CtNewConstructor.make("public Demo() {}", ctClass);
```

### **3.4 toClass 与类加载**

toClass() 会把 CtClass 变成 JVM 里的 Class<?>。注意：**一个类名在同一个 ClassLoader 中只能被定义一次**。

所以 Demo 里经常会：

- 修改样例类前先 setName("xxx.GeneratedClass")，避免覆盖原类。
- 使用 detach() 从 ClassPool 缓存中释放 CtClass。
- 对自定义 ClassLoader 场景，使用 toBytecode() 后自己 defineClass。

Javassist 的基本流程：

```sql
 1、创建或获取 ClassPool
 2、获取 CtClass 或 makeClass
 3、新增/修改字段 方法 构造器
 4、输出方式
 5、toClass 加载到 JVM
 6、writeFile 写出 class 文件
 7、反射调用或强转接口调用
 8、javap / IDE 反编译查看
```

## **4、Demo 0：ClassPool / CtClass 基础认知**

源码片段：

```java
private static void classPoolBasics() throws Exception {
    ClassPool pool = ClassPool.getDefault();
    pool.insertClassPath(new ClassClassPath(User.class));

    CtClass ctClass = pool.get("org.example.javassistdemo.samples.User");
    System.out.println("CtClass 名称 = " + ctClass.getName());
    System.out.println("是否接口 = " + ctClass.isInterface());
    System.out.println("声明方法 = " + Arrays.toString(Arrays.stream(ctClass.getDeclaredMethods()).map(CtMethod::getName).toArray()));
}
```

解释：

- ClassPool.getDefault()：拿默认类池。
- insertClassPath(new ClassClassPath(User.class))：告诉 Javassist 从当前类所在路径查找 class。
- pool.get(...)：拿到 User 的字节码抽象。

输出类似：

```text
CtClass 名称 = org.example.javassistdemo.samples.User
是否接口 = false
声明方法 = [getId, setId, getName, setName, getAge, setAge, displayName, toString]
```

## **5、Demo 1：从零创建一个新类**

场景：运行时创建一个 GeneratedHello 类，让它实现 Greeter 接口。

接口：

```kotlin
package org.example.javassistdemo.samples;

public interface Greeter {
    String hello(String name);
}
```

Javassist 代码：

```java
private static void createNewClassDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.makeClass("org.example.javassistdemo.generated.GeneratedHello");
    ctClass.addInterface(pool.get(Greeter.class.getName()));

    CtField prefix = new CtField(pool.get(String.class.getName()), "prefix", ctClass);
    prefix.setModifiers(Modifier.PRIVATE);
    ctClass.addField(prefix, CtField.Initializer.constant("Hello"));

    ctClass.addConstructor(CtNewConstructor.make("public GeneratedHello() {}", ctClass));
    ctClass.addMethod(CtNewMethod.make(
            "public String hello(String name) { return this.prefix + \", \" + name + \"!\"; }",
            ctClass));

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Greeter greeter = (Greeter) clazz.getDeclaredConstructor().newInstance();
    System.out.println(greeter.hello("Javassist"));
    ctClass.writeFile(GENERATED_DIR.toString());
    ctClass.detach();
}
```

关键点：

- makeClass：创建新类。
- addInterface：让生成类实现接口。
- addField：新增字段。
- CtField.Initializer.constant("Hello")：字段默认值。
- CtNewConstructor.make：用源码字符串创建构造器。
- CtNewMethod.make：用源码字符串创建方法。
- toClass(GeneratedLookupAnchor.class)：Java 9+ 下用同包 lookup anchor 定义类。

输出：

```kotlin
Hello, Javassist!
```

## **6、Demo 2：修改已存在类结构**

场景：基于 User 生成 EnhancedUser，新增 email 字段、Getter/Setter、contactCard() 方法。

原始类片段：

```java
public class User {
    private long id;
    private String name;
    private int age;

    public User(long id, String name, int age) {
        this.id = id;
        this.name = name;
        this.age = age;
    }

    public String displayName() {
        return name + "(" + age + ")";
    }
}
```

增强代码：

```kotlin
private static void addMembersToExistingClassDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.get(User.class.getName());
    ctClass.setName("org.example.javassistdemo.generated.EnhancedUser");

    CtField email = CtField.make("private String email;", ctClass);
    ctClass.addField(email);
    ctClass.addMethod(CtNewMethod.getter("getEmail", email));
    ctClass.addMethod(CtNewMethod.setter("setEmail", email));
    ctClass.addMethod(CtNewMethod.make("public String contactCard() { return getName() + \" <\" + this.email + \">\"; }", ctClass));

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Object user = clazz.getConstructor(long.class, String.class, int.class).newInstance(1L, "Alice", 18);
    clazz.getMethod("setEmail", String.class).invoke(user, "alice@example.com");
    System.out.println(clazz.getMethod("contactCard").invoke(user));
    ctClass.writeFile(GENERATED_DIR.toString());
    ctClass.detach();
}
```

输出：

```kotlin
Alice <alice@example.com>
```

适用场景：

- 运行时增强实体类。
- 给 DTO 自动加字段。
- 生成框架内部辅助类。

## **7、Demo 3：修改方法体**

这一节展示最常用的 4 种方法增强：


| API | 作用 |
| --- | --- |
| insertBefore | 方法开头插入代码 |
| insertAfter | 方法正常返回前插入代码 |
| addCatch | 给方法包一层 catch |
| setBody | 完全替换方法体 |


被增强类：

```java
public class UserService {
    public User findUser(long id) {
        sleep(30);
        return new User(id, "user-" + id, 20);
    }

    public String createUser(String name, int age) {
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("name must not be blank");
        }
        sleep(20);
        return "created:" + name + ":" + age;
    }

    public int add(int a, int b) {
        return a + b;
    }

    public String alwaysFail() {
        throw new IllegalStateException("business exception");
    }
}
```

增强代码：

```java
CtMethod createUser = ctClass.getDeclaredMethod("createUser");
createUser.insertBefore("{ System.out.println(\"[before] createUser args: name=\" + $1 + \", age=\" + $2); }");
createUser.insertAfter("{ System.out.println(\"[after] createUser result=\" + $_); }");

CtMethod add = ctClass.getDeclaredMethod("add");
add.setBody("{ System.out.println(\"[setBody] force add result to 100\"); return 100; }");

CtMethod alwaysFail = ctClass.getDeclaredMethod("alwaysFail");
alwaysFail.addCatch("{ System.out.println(\"[catch] caught: \" + $e.getMessage()); return \"fallback\"; }", pool.get(Exception.class.getName()));
```

Javassist 特殊变量：


| 变量 | 含义 |
| --- | --- |
| $0 | 当前对象，相当于 this，静态方法没有 $0 |
| $1, $2 | 第 1、第 2 个参数 |
| $args | 参数数组 |
| $$ | 所有参数，常用于 $proceed($$) |
| $_ | 方法返回值 |
| $e | catch 到的异常 |
| $r | 返回类型转换 |
| $w | 包装基本类型为包装类型 |


输出：

```text
[before] createUser args: name=Bob, age=22
[after] createUser result=created:Bob:22
created:Bob:22
[setBody] force add result to 100
add(1,2) = 100
[catch] caught: business exception
alwaysFail() = fallback
```

## **8、Demo 4：表达式级增强**

方法级增强是“在方法入口/出口动刀”，表达式级增强是“在方法内部某个调用点/字段访问点动刀”。

### **8.1 拦截字段读取**

```java
CtMethod displayName = ctClass.getDeclaredMethod("displayName");
displayName.instrument(new ExprEditor() {
    @Override
    public void edit(FieldAccess f) throws CannotCompileException {
        if (f.isReader() && "name".equals(f.getFieldName())) {
            f.replace("{ $_ = ($proceed($$) == null ? \"<no-name>\" : $proceed($$)); }");
        }
    }
});
```

含义：当读取 name 字段时，如果原值是 null，就替换成 <no-name>。

输出：

### **8.2 拦截方法调用**

```sql
createOrder.instrument(new ExprEditor() {
    @Override
    public void edit(MethodCall m) throws CannotCompileException {
        if ("java.lang.StringBuilder".equals(m.getClassName()) && "append".equals(m.getMethodName())) {
            m.replace("{ System.out.println(\"[MethodCall] StringBuilder.append called\"); $_ = $proceed($$); }");
        }
    }
});
```

实际项目里可以用它实现：

- 替换某个第三方 SDK 调用。
- 给内部某个方法调用点加日志。
- 改造字段读写行为。

## **9、Demo 5：动态代理**

场景：运行时生成 Greeter 接口实现类。

```java
private static void dynamicProxyDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.makeClass("org.example.javassistdemo.generated.GreeterProxy");
    ctClass.addInterface(pool.get(Greeter.class.getName()));
    ctClass.addField(CtField.make("private String targetName;", ctClass));
    ctClass.addConstructor(CtNewConstructor.make("public GreeterProxy(String targetName) { this.targetName = targetName; }", ctClass));
    ctClass.addMethod(CtNewMethod.make("public String hello(String name) { System.out.println(\"[proxy] before hello\"); return \"hi \" + name + \" from \" + this.targetName; }", ctClass));

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Greeter greeter = (Greeter) clazz.getConstructor(String.class).newInstance("runtime-proxy");
    System.out.println(greeter.hello("Carol"));
    ctClass.detach();
}
```

输出：

```text
[proxy] before hello
hi Carol from runtime-proxy
```

对比 JDK 动态代理：

- JDK Proxy 只能代理接口。
- Javassist 可以直接生成普通类、继承类、实现接口，灵活度更高。
- 但 Javassist 需要更小心类加载和字节码合法性。

## **10、Demo 6：运行时生成 Bean/DTO**

场景：根据字段列表动态生成 DTO，这类需求常见于报表、ORM、低代码平台。

```c++
private static void runtimeBeanDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.makeClass("org.example.javassistdemo.generated.RuntimeProductDTO");
    addProperty(pool, ctClass, "sku", String.class);
    addProperty(pool, ctClass, "price", int.class);
    ctClass.addMethod(CtNewMethod.make("public String toString() { return \"RuntimeProductDTO{sku='\" + this.sku + \"', price=\" + this.price + \"}\"; }", ctClass));

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Object dto = clazz.getDeclaredConstructor().newInstance();
    clazz.getMethod("setSku", String.class).invoke(dto, "p-100");
    clazz.getMethod("setPrice", int.class).invoke(dto, 1999);
    System.out.println(dto);
    ctClass.writeFile(GENERATED_DIR.toString());
    ctClass.detach();
}
```

辅助方法：

```java
private static void addProperty(ClassPool pool, CtClass ctClass, String name, Class<?> type)
        throws CannotCompileException, NotFoundException {
    CtField field = new CtField(pool.get(type.getName()), name, ctClass);
    field.setModifiers(Modifier.PRIVATE);
    ctClass.addField(field);
    String methodSuffix = Character.toUpperCase(name.charAt(0)) + name.substring(1);
    ctClass.addMethod(CtNewMethod.getter("get" + methodSuffix, field));
    ctClass.addMethod(CtNewMethod.setter("set" + methodSuffix, field));
}
```

输出：

```java
RuntimeProductDTO{sku='p-100', price=1999}
```

## **11、Demo 7：AOP 耗时统计**

场景：给所有 public 方法增加耗时日志。

```java
private static void aopTimingDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.get(UserService.class.getName());
    ctClass.setName("org.example.javassistdemo.generated.TimingUserService");
    for (CtMethod method : ctClass.getDeclaredMethods()) {
        if (Modifier.isPublic(method.getModifiers())) {
            method.addLocalVariable("__start", CtClass.longType);
            method.addLocalVariable("__cost", CtClass.doubleType);
            method.insertBefore("{ __start = System.nanoTime(); }");
            method.insertAfter("{ __cost = (System.nanoTime() - __start) / 1000000.0; System.out.println(\"[timing] " + method.getName() + " cost=\" + __cost + \" ms\"); }", false);
        }
    }

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Object service = clazz.getDeclaredConstructor().newInstance();
    clazz.getMethod("findUser", long.class).invoke(service, 10L);
    ctClass.detach();
}
```

输出：

```text
[timing] findUser cost=36.29625 ms
```

注意：

- addLocalVariable 可以给方法增加局部变量。
- insertAfter(..., false) 表示正常返回时执行。
- 如果希望异常时也统计耗时，可使用 insertAfter(..., true) 或 addCatch，但复杂方法上要注意 StackMapTable/校验问题；生产实践中更推荐完整替换为 try/finally 结构或在 Java Agent 阶段统一处理。

## **12、Demo 8：缓存场景**

场景：把原方法替换成带本地缓存的逻辑。

```java
public class LocalCache {
    public String get(String key) {
        System.out.println("[LocalCache] slow query for key=" + key);
        return "value:" + key;
    }
}
```

增强代码：

```java
private static void cacheDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.get(LocalCache.class.getName());
    ctClass.setName("org.example.javassistdemo.generated.CachedLocalCache");
    ctClass.addField(CtField.make("private java.util.Map cache = new java.util.HashMap();", ctClass));
    CtMethod get = ctClass.getDeclaredMethod("get");
    get.setBody("{ if (cache.containsKey($1)) { System.out.println(\"[cache] hit key=\" + $1); return (String) cache.get($1); } " +
            "String value = \"value:\" + $1; System.out.println(\"[cache] miss key=\" + $1); cache.put($1, value); return value; }");

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    Object cache = clazz.getDeclaredConstructor().newInstance();
    Method method = clazz.getMethod("get", String.class);
    System.out.println(method.invoke(cache, "a"));
    System.out.println(method.invoke(cache, "a"));
    ctClass.detach();
}
```

输出：

```text
[cache] miss key=a
value:a
[cache] hit key=a
value:a
```

## **13、Demo 9：注解和字节码元数据**

场景：运行时给一个类添加注解。

注解定义：

```java
@Retention(RetentionPolicy.RUNTIME)
public @interface ApiMarker {
    String value();
}
```

增强代码：

```java
private static void annotationDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.get(MarkedApi.class.getName());
    ctClass.setName("org.example.javassistdemo.generated.RuntimeMarkedApi");

    ClassFile classFile = ctClass.getClassFile();
    ConstPool constPool = classFile.getConstPool();
    AnnotationsAttribute attr = new AnnotationsAttribute(constPool, AnnotationsAttribute.visibleTag);
    Annotation annotation = new Annotation(ApiMarker.class.getName(), constPool);
    annotation.addMemberValue("value", new StringMemberValue("runtime-added", constPool));
    attr.addAnnotation(annotation);
    classFile.addAttribute(attr);

    Class<?> clazz = ctClass.toClass(GeneratedLookupAnchor.class);
    ApiMarker marker = clazz.getAnnotation(ApiMarker.class);
    System.out.println("annotation value = " + (marker == null ? null : marker.value()));
    ctClass.detach();
}
```

输出：

```text
annotation value = runtime-added
```

关键点：

- Javassist 高层 API 主要改类、字段、方法。
- 注解、泛型签名、属性表等属于更底层的 bytecode 结构，需要使用 javassist.bytecode.\*。

## **14、Demo 10：类重命名 / 复制**

场景：基于一个模板类复制出新类，再单独增强。

```java
private static void renameAndCopyDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass copy = pool.getAndRename(PaymentService.class.getName(), "org.example.javassistdemo.generated.PaymentServiceCopy");
    CtMethod pay = copy.getDeclaredMethod("pay");
    pay.insertBefore("{ System.out.println(\"[copy] pay cents=\" + $1); }");

    Class<?> clazz = copy.toClass(GeneratedLookupAnchor.class);
    Object service = clazz.getDeclaredConstructor().newInstance();
    System.out.println("pay result = " + clazz.getMethod("pay", int.class).invoke(service, 100));
    copy.detach();
}
```

输出：

```text
[copy] pay cents=100
pay result = 100
```

适用场景：

- 基于模板生成多个变体类。
- 避免直接修改原始类。
- 做灰度增强、插件类复制。

## **15、Demo 11：类加载隔离**

### **15.1 为什么需要类加载隔离？**

JVM 判断两个类是否相同，看两个条件：

1. 类全限定名是否相同。
2. 定义它的 ClassLoader 是否相同。

同一个 `org.example.PluginTask`，如果由两个不同 ClassLoader 加载，它们就是两个不同的 Class。

自定义 ClassLoader：

```java
public class ChildFirstClassLoader extends ClassLoader {
    private final String childFirstPackage;

    public ChildFirstClassLoader(ClassLoader parent, String childFirstPackage) {
        super(parent);
        this.childFirstPackage = childFirstPackage;
    }

    public Class<?> defineEnhancedClass(String name, byte[] bytes) {
        return defineClass(name, bytes, 0, bytes.length);
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        synchronized (getClassLoadingLock(name)) {
            Class<?> loaded = findLoadedClass(name);
            if (loaded == null && name.startsWith(childFirstPackage)) {
                try {
                    loaded = findClass(name);
                } catch (ClassNotFoundException ignored) {
                    // 如果自己找不到，再交给父加载器。
                }
            }
            if (loaded == null) {
                loaded = super.loadClass(name, false);
            }
            if (resolve) {
                resolveClass(loaded);
            }
            return loaded;
        }
    }

    @Override
    protected Class<?> findClass(String name) throws ClassNotFoundException {
        String resource = name.replace('.', '/') + ".class";
        try (InputStream input = getParent().getResourceAsStream(resource)) {
            if (input == null) {
                throw new ClassNotFoundException(name);
            }
            byte[] bytes = input.readAllBytes();
            return defineClass(name, bytes, 0, bytes.length);
        } catch (IOException e) {
            throw new ClassNotFoundException(name, e);
        }
    }
}
```

增强代码：

```java
private static void classLoaderIsolationDemo() throws Exception {
    ChildFirstClassLoader childLoader = new ChildFirstClassLoader(
            JavassistAllScenesDemo.class.getClassLoader(),
            "org.example.javassistdemo.samples.PluginTask");

    ClassPool pool = new ClassPool(false);
    pool.appendSystemPath();
    pool.insertClassPath(new LoaderClassPath(childLoader));
    CtClass ctClass = pool.get(PluginTask.class.getName());
    CtMethod run = ctClass.getDeclaredMethod("run");
    run.insertBefore("{ System.out.println(\"[child-loader enhanced] input=\" + $1); }");
    run.insertAfter("{ $_ = $_ + \"|enhanced\"; }");

    Class<?> pluginClass = childLoader.defineEnhancedClass(ctClass.getName(), ctClass.toBytecode());
    Object plugin = pluginClass.getDeclaredConstructor().newInstance();
    System.out.println("parent class result = " + new PluginTask().run("x"));
    System.out.println("child class result  = " + pluginClass.getMethod("run", String.class).invoke(plugin, "x"));
    System.out.println("parent == child ? " + (PluginTask.class == pluginClass));
    ctClass.detach();
}

```

输出：

```text
parent class result = original:x
[child-loader enhanced] input=x
child class result  = original:x|enhanced
parent == child ? false
```

## **16、Demo 12：输出 .class 文件**

场景：把生成的类写到磁盘，便于 `javap` 或 IDE 反编译查看。

```java
private static void writeClassFileDemo() throws Exception {
    ClassPool pool = newPool();
    CtClass ctClass = pool.makeClass("org.example.javassistdemo.generated.ClassFileOnlyExample");
    ctClass.addMethod(CtNewMethod.make("public int answer() { return 42; }", ctClass));
    ctClass.writeFile(GENERATED_DIR.toString());
    System.out.println("已写出：" + GENERATED_DIR.resolve("org/example/javassistdemo/generated/ClassFileOnlyExample.class"));
    ctClass.detach();
}
```

输出目录：

```java
build/generated-javassist-classes
可以用下面命令查看字节码：
javap -classpath build/generated-javassist-classes -c org.example.javassistdemo.generated.ClassFileOnlyExample

```

## **17、完整 Demo 主流程**

JavassistAllScenesDemo.main 会依次执行

```java
public static void main(String[] args) throws Exception {
    Files.createDirectories(GENERATED_DIR);

    title("0. ClassPool / CtClass 基础认知");
    classPoolBasics();

    title("1. 从零创建一个新类：字段、构造器、方法、接口");
    createNewClassDemo();

    title("2. 修改已存在的类结构：新增字段、方法、构造器");
    addMembersToExistingClassDemo();

    title("3. 修改方法体：insertBefore / insertAfter / addCatch / setBody");
    modifyMethodBodyDemo();

    title("4. 表达式级增强：拦截方法调用和字段访问");
    expressionEditorDemo();

    title("5. 动态代理场景：运行时生成接口实现类");
    dynamicProxyDemo();

    title("6. ORM/DTO 场景：运行时生成 Bean 类");
    runtimeBeanDemo();

    title("7. AOP 计时场景：给业务方法自动加耗时日志");
    aopTimingDemo();

    title("8. 缓存场景：把方法体替换成带缓存逻辑");
    cacheDemo();

    title("9. 注解和字节码元数据：运行时给类添加注解");
    annotationDemo();

    title("10. 类重命名 / 复制：基于模板类生成新类");
    renameAndCopyDemo();

    title("11. 类加载隔离：用自定义 ClassLoader 修改同名类副本");
    classLoaderIsolationDemo();

    title("12. 输出 .class 文件：用于排查和反编译学习");
    writeClassFileDemo();
}
```

## **18、常见坑位总结**

### **18.1 同一个 ClassLoader 不能重复定义同名类**

错误表现：

```text
LinkageError: duplicate class definition
```

解决：

- 生成类时使用唯一类名。
- 测试时每次 `setName` 到 `generated.xxx`。
- 需要同名类多版本时，使用不同 ClassLoader。

### **18.2 类一旦被加载，通常就不能再用 Javassist 改同一个 Class 对象**

如果类已经被 JVM 加载了，普通 Javassist 不能直接“原地修改它”。

可选方案：

- 加载前修改。
- 使用 Java Agent + Instrumentation 重转换类。
- 生成一个新类名或使用新 ClassLoader。

### **18.3 Java 9+ 模块限制**

原因是：生成类在 org.example.javassistdemo.generated 包下，所以 lookup anchor 也放在同包。

### **18.4 Javassist 源码字符串不是完整 Java 文件**

CtNewMethod.make 里写的是方法源码，不是完整 class 文件。

可以写：

```java
"public int add(int a, int b) { return a + b; }"
```

不能写：

```java
"public class X { public int add(...) {...} }"
```

### **18.5 需要写全限定类名**

在 Javassist 字符串里，很多时候不能依赖 Java 文件里的 `import`。推荐写全限定名：

```java
"private java.util.Map cache = new java.util.HashMap();"
```

### **18.6 及时 detach**

ClassPool 会缓存 CtClass，大量生成类时要：

```java
ctClass.detach();
```

避免内存越来越大。

### **18.7 方法增强要注意返回类型**

insertAfter 访问返回值用 \$\_。如果方法返回 void，就不能把 \$\_ 当作普通值用

#### **18.8 异常路径增强更复杂**

insertAfter(code, true) 表示 finally 语义，但复杂方法、局部变量、StackMapTable 可能导致校验问题。生产代码建议：

- 控制增强范围。
- 对复杂方法做充分测试。
- 必要时使用 setBody 手动构造 try/finally。
- 或使用更底层 ASM/Byte Buddy。

### **19、Javassist API 速查表**


| API | 说明 |
| --- | --- |
| ClassPool.getDefault() | 获取默认类池 |
| new ClassPool(true) | 创建新类池并带系统路径 |
| pool.get("类名") | 获取已有类 |
| pool.makeClass("类名") | 创建新类 |
| pool.getAndRename(old, new) | 获取并重命名 |
| ctClass.setName(...) | 修改类名 |
| ctClass.addField(...) | 新增字段 |
| ctClass.addMethod(...) | 新增方法 |
| ctClass.addConstructor(...) | 新增构造器 |
| ctClass.getDeclaredMethod(...) | 获取声明方法 |
| method.insertBefore(...) | 方法前插入代码 |
| method.insertAfter(...) | 方法后插入代码 |
| method.addCatch(...) | 增加 catch 逻辑 |
| method.setBody(...) | 替换方法体 |
| method.instrument(new ExprEditor()) | 表达式级增强 |
| ctClass.toClass(...) | 加载为 JVM Class |
| ctClass.toBytecode() | 转成 class 字节数组 |
| ctClass.writeFile(...) | 写出 class 文件 |
| ctClass.detach() | 从 ClassPool 缓存释放 |
