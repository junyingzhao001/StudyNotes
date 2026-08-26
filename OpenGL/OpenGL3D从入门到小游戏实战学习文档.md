# OpenGL 3D 从入门到小游戏实战学习文档

> 本文由内部知识库文档整理为 GitHub 可直接阅读的 Markdown。已移除原始内部链接、附件链接、账号 token、组织域名等公司相关信息。

引言：

1. OpenGL / OpenGL ES 到底是什么

2. 3D 渲染为什么能把“点”变成“画面”

3. 这个项目里各个核心类分别在做什么

4. 要掌握哪些基础知识，才能自己写 3D 效果

5. 如何从“会画一个 3D 物体”走到“做一个小游戏”

## 1、什么是 OpenGL

OpenGL 可以理解成一套“告诉 GPU 如何画图”的标准接口。

它本身不是一个游戏引擎，也不是一个 UI 框架，而是一套偏底层的图形 API。

你可以把它理解成：

```kotlin
CPU 负责准备数据、组织场景、更新逻辑
GPU 负责把这些数据高速转换成屏幕上的像素
```

在 Android 上，我们更常见的是 OpenGL ES：

```kotlin
OpenGL 偏桌面端
OpenGL ES 是移动端/嵌入式版本
```

当前项目使用的是 OpenGL ES 3.x

## 2、OpenGL 为什么能画出 3D

本质上，屏幕永远是 2D 的。

所以“画 3D”并不是真的把一个立方体塞进屏幕，而是做了下面这些事：

1. 在程序里定义一个 3D 世界

2. 用数学方法决定“相机从哪里看这个世界”

3. 把 3D 点投影到 2D 屏幕上

4. 算出哪个面在前，哪个面在后

5. 再把结果画到屏幕上

所以 3D 图形的核心不是“特殊控件”，而是：
- 几何数据
- 矩阵变换
- 光栅化
- 深度测试
- 纹理
- Shader

## 3、一个最重要的心智模型

学习 3D，最重要的一句话是：

```kotlin
你不是在“画图片”，你是在“描述一个世界，再让相机去看它”。
```

这句话很关键。

2D UI 开发时，我们常常想的是：
- 这里放个 View
- 那里放张图
- 再加点动画

但 3D 的思维方式更像这样：
- 世界里有一个地面
- 地面上有墙
- 墙之间有一个角色
- 角色面前有一些豆子
- 相机在角色后上方
- 相机朝角色前方看过去

也就是说，3D 更像“搭场景”。

## 4、OpenGL 的运行原理

### 4.1、从 CPU 到 GPU

一次渲染大致会经过这几个阶段：

1. Java / Kotlin / C++ 代码准备顶点、纹理、矩阵等数据
2. 这些数据通过 OpenGL API 传给 GPU
3. GPU 运行 Vertex Shader 处理顶点
4. GPU 把图元组装成三角形
5. GPU 做裁剪、投影、光栅化
6. GPU 运行 Fragment Shader 计算像素颜色
7. GPU 把结果写入颜色缓冲区
8. 最后交换缓冲区，把画面显示到屏幕

这里最重要的原则是：

- CPU 负责“准备”
- GPU 负责“并行计算和出图”

### 4.2 、为什么图形学里总是“三角形”

GPU 最擅长处理的是三角形。

原因很简单：

- 三角形一定是平面的
- 三角形最稳定
- 任意复杂模型都能拆成三角形

所以：

- 一个矩形会拆成 2 个三角形
- 一个圆会拆成很多小三角形
- 一个球体会拆成大量三角形网格

当前项目里很多填充区域最终都会转成 GL_TRIANGLES 来画。

### 4.3 、什么是 Shader

Shader 是运行在 GPU 上的小程序。

最常见的两种：

- Vertex Shader：处理每个顶点，输出裁剪空间位置及需要插值的数据
- Fragment Shader：处理光栅化产生的片段，计算颜色；片段还可能被深度、模板等测试丢弃，因此不等同于“最终屏幕像素”

可以把它们理解成：

- Vertex Shader：几何变换工厂
- Fragment Shader：上色工厂

## 5、3D 渲染管线的基础概念

下面这些概念是必须掌握的。

### 5.1 、顶点 Vertex

顶点就是几何体的关键点。

一个最简单的 3D 顶点可以只包含：

```java
x, y, z
```

如果要贴图，还会带：

```java
u, v
```

如果要光照，还可能带：

```java
nx, ny, nz
```

也就是法线。

### 5.2 、图元 Primitive

OpenGL 会把顶点解释成不同类型的图元，比如：

- GL_POINTS
- GL_LINES
- GL_LINE_STRIP
- GL_TRIANGLES
- GL_TRIANGLE_STRIP
- GL_TRIANGLE_FAN

3D 里最常见的是 GL_TRIANGLES。

### 5.3 、坐标系

3D 里有多个坐标系：

1. 模型坐标系
2. 世界坐标系
3. 观察坐标系
4. 裁剪坐标系
5. 屏幕坐标系

初学者最容易混淆，但其实可以简单理解成：

- 模型坐标系：物体自己内部的局部坐标
- 世界坐标系：物体放到整个场景后的坐标
- 观察坐标系：以相机为参考的坐标
- 屏幕坐标系：最后显示在屏幕上的 2D 坐标

### 5.4 、矩阵变换

图形学里几乎所有移动都离不开矩阵。

最常见的三个变换：

- 平移 Translate
- 旋转 Rotate
- 缩放 Scale

通常我们会得到三类矩阵：

- Model Matrix
- View Matrix
- Projection Matrix

最后一般会合成：

```java
MVP = Projection * View * Model
```

这是最经典的 3D 公式。

### 5.5 、正交投影和透视投影

#### 正交投影 Ortho

特点：

- 没有近大远小
- 适合地图、CAD、2D 编辑器

#### 透视投影 Perspective

特点：

- 有近大远小
- 更像人眼
- 更有 3D 感

当前项目里：

- 2D 模式主要是正交投影
- 3D 模式主要是透视投影

### 5.6、 深度测试

深度测试解决的是：

> 当前像素到底该显示前面的物体，还是后面的物体？

OpenGL 会为每个像素保存一个深度值。

当新的片元要写进去时，会比较：

- 更近：允许覆盖
- 更远：丢弃

这就是为什么近处墙体能挡住远处豆子。

### 5.7 、纹理 Texture

纹理就是贴在几何体表面的图片。

比如：

- 小车表面贴一张图片
- 幽灵表面贴一张图片
- 地板贴一张图片

没有纹理时，物体通常只用纯色。

## 6、Android 上 OpenGL ES 是怎么跑起来的

### 6.1 、需要 EGL

在 Android 上，OpenGL ES 不是直接对屏幕画，而是要先通过 EGL 建立渲染上下文。

你可以把 EGL 理解成：

- GPU 渲染环境的初始化器
- OpenGL 和系统窗口之间的桥梁

当前项目里的 MapGLRenderer.init(...) 就在做这件事。

它主要完成：

1. 获取 EGLDisplay
2. 选择 EGLConfig
3. 创建 EGLContext
4. 创建 EGLSurface
5. 把 Context 绑定到当前线程

这一步做完后，当前线程才能安全地调用 OpenGL API。

### 6.2 、为什么渲染通常要求同一线程

图形上下文往往和创建它的线程绑定。  

所以你会看到类似这样的注释：

> 必须在持有 EGL 的同一线程调用 drawFrame

原因是：

- EGL Context 不是线程随便共享的普通对象
- OpenGL 的状态机也依赖当前绑定的上下文

所以项目里专门用了 `HandlerThread` 跑 GL 渲染。

## 7、Android 上 OpenGL ES 的最小运行结构

一个最小 Android OpenGL ES 页面，通常有 3 层：

1. Activity
2. GLSurfaceView
3. Renderer

关系如下：

-  Activity

管页面生命周期

- GLSurfaceView

提供 GL 渲染表面

管理 GL 线程

- Renderer

真正写 OpenGL 绘制逻辑

## 8、Android 最小 3D 工程结构示意

可以先按这个结构理解：

```kotlin
app/
  src/main/java/com/example/opencl_learn/
    MainActivity.kt
    MyGLSurfaceView.kt
    SimpleRenderer.kt
    shader/
      ShaderUtils.kt
    mesh/
      Triangle.kt
      Cube.kt
```

说明：

- MainActivity负责把 GLSurfaceView 放到页面上
- MyGLSurfaceView设置 OpenGL ES 版本绑定 Renderer处理触摸手势
- SimpleRenderer写 onSurfaceCreated/onSurfaceChanged/onDrawFrame
- Triangle/Cube保存顶点数据和 draw 逻辑
- ShaderUtils编译和链接 Shader

## 9、Android 最小可运行例子

**MainActivity**

```kotlin
package com.example.opencl_learn

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private lateinit var glView: MyGLSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        glView = MyGLSurfaceView(this)
        setContentView(glView)
    }

    override fun onResume() {
        super.onResume()
        glView.onResume()
    }

    override fun onPause() {
        glView.onPause()
        super.onPause()
    }
}
```

**MyGLSurfaceView**

```kotlin
package com.example.opencl_learn

import android.content.Context
import android.opengl.GLSurfaceView
import android.view.MotionEvent

class MyGLSurfaceView(context: Context) : GLSurfaceView(context) {
    private val renderer: SimpleRenderer
    private var lastX = 0f
    private var lastY = 0f

    init {
        setEGLContextClientVersion(3)
        renderer = SimpleRenderer()
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = event.x
                lastY = event.y
            }

            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount == 1) {
                    val dx = event.x - lastX
                    val dy = event.y - lastY

                    // GLSurfaceView 默认在独立 GL 线程回调 Renderer。
                    // 通过 queueEvent 修改渲染状态，避免 UI 线程与 GL 线程直接竞争。
                    queueEvent {
                        renderer.rotateBy(
                            horizontalDelta = dx * 0.3f,
                            verticalDelta = dy * 0.3f
                        )
                    }

                    lastX = event.x
                    lastY = event.y
                }
            }
        }
        return true
    }
}
```

**SimpleRenderer 基础骨架**

```kotlin
package com.example.opencl_learn

import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import com.example.opencl_learn.mesh.Cube
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
class SimpleRenderer : GLSurfaceView.Renderer {
    private lateinit var cube: Cube

    private val viewMatrix = FloatArray(16)
    private val projMatrix = FloatArray(16)
    private val modelMatrix = FloatArray(16)
    private val tempMatrix = FloatArray(16)
    private val mvpMatrix = FloatArray(16)

    private var angleX = 20f
    private var angleY = 30f

    fun rotateBy(horizontalDelta: Float, verticalDelta: Float) {
        angleY += horizontalDelta
        angleX = (angleX + verticalDelta).coerceIn(-90f, 90f)
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES30.glClearColor(0.08f, 0.08f, 0.12f, 1f)
        GLES30.glEnable(GLES30.GL_DEPTH_TEST)
        cube = Cube()
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        GLES30.glViewport(0, 0, width, height)
        val ratio = width.toFloat() / height.toFloat()
        Matrix.perspectiveM(projMatrix, 0, 60f, ratio, 0.1f, 100f)
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT or GLES30.GL_DEPTH_BUFFER_BIT)

        Matrix.setLookAtM(
            viewMatrix, 0,
            0f, 0f, 6f,
            0f, 0f, 0f,
            0f, 1f, 0f
        )

        Matrix.setIdentityM(modelMatrix, 0)
        Matrix.rotateM(modelMatrix, 0, angleX, 1f, 0f, 0f)
        Matrix.rotateM(modelMatrix, 0, angleY, 0f, 1f, 0f)

        Matrix.multiplyMM(tempMatrix, 0, viewMatrix, 0, modelMatrix, 0)
        Matrix.multiplyMM(mvpMatrix, 0, projMatrix, 0, tempMatrix, 0)

        cube.draw(mvpMatrix)
    }
}
```

**Cube**

```kotlin
package com.example.opencl_learn.mesh

import android.opengl.GLES30
import com.example.opencl_learn.shader.ShaderUtils
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

class Cube {
    private val vertexBuffer: FloatBuffer
    private val program: Int
    private val uMvp: Int
    private val uColor: Int

    private val vertices = floatArrayOf(
        // front
        -1f, -1f,  1f,   1f, -1f,  1f,   1f,  1f,  1f,
        -1f, -1f,  1f,   1f,  1f,  1f,  -1f,  1f,  1f,

        // back
        -1f, -1f, -1f,  -1f,  1f, -1f,   1f,  1f, -1f,
        -1f, -1f, -1f,   1f,  1f, -1f,   1f, -1f, -1f,

        // left
        -1f, -1f, -1f,  -1f, -1f,  1f,  -1f,  1f,  1f,
        -1f, -1f, -1f,  -1f,  1f,  1f,  -1f,  1f, -1f,

        // right
        1f, -1f, -1f,   1f, -1f,  1f,   1f,  1f,  1f,
        1f, -1f, -1f,   1f,  1f,  1f,   1f,  1f, -1f,

        // top
        -1f,  1f, -1f,  -1f,  1f,  1f,   1f,  1f,  1f,
        -1f,  1f, -1f,   1f,  1f,  1f,   1f,  1f, -1f,

        // bottom
        -1f, -1f, -1f,  -1f, -1f,  1f,   1f, -1f,  1f,
        -1f, -1f, -1f,   1f, -1f,  1f,   1f, -1f, -1f
    )

    init {
        vertexBuffer = ByteBuffer.allocateDirect(vertices.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
        vertexBuffer.put(vertices).position(0)

        val vertexShader = """
            #version 300 es
            layout(location = 0) in vec3 aPos;
            uniform mat4 uMVP;

            void main() {
                gl_Position = uMVP * vec4(aPos, 1.0);
            }
        """.trimIndent()

        val fragmentShader = """
            #version 300 es
            precision mediump float;
            uniform vec4 uColor;
            out vec4 fragColor;

            void main() {
                fragColor = uColor;
            }
        """.trimIndent()

        program = ShaderUtils.createProgram(vertexShader, fragmentShader)
        uMvp = GLES30.glGetUniformLocation(program, "uMVP")
        uColor = GLES30.glGetUniformLocation(program, "uColor")
    }

    fun draw(mvpMatrix: FloatArray) {
        GLES30.glUseProgram(program)
        GLES30.glUniformMatrix4fv(uMvp, 1, false, mvpMatrix, 0)
        GLES30.glUniform4f(uColor, 0.3f, 0.7f, 1.0f, 1.0f)

        vertexBuffer.position(0)
        GLES30.glEnableVertexAttribArray(0)
        GLES30.glVertexAttribPointer(0, 3, GLES30.GL_FLOAT, false, 3 * 4, vertexBuffer)
        GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, vertices.size / 3)
        GLES30.glDisableVertexAttribArray(0)
    }
}
```

**ShaderUtils**

```kotlin
package com.example.opencl_learn.shader

import android.opengl.GLES30

object ShaderUtils {
    fun createProgram(vertexCode: String, fragmentCode: String): Int {
        val vertexShader = loadShader(GLES30.GL_VERTEX_SHADER, vertexCode)
        val fragmentShader = loadShader(GLES30.GL_FRAGMENT_SHADER, fragmentCode)

        val program = GLES30.glCreateProgram()
        GLES30.glAttachShader(program, vertexShader)
        GLES30.glAttachShader(program, fragmentShader)
        GLES30.glLinkProgram(program)

        val status = IntArray(1)
        GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            val error = GLES30.glGetProgramInfoLog(program)
            GLES30.glDeleteProgram(program)
            throw RuntimeException("Link program failed: $error")
        }

        GLES30.glDeleteShader(vertexShader)
        GLES30.glDeleteShader(fragmentShader)
        return program
    }

    private fun loadShader(type: Int, code: String): Int {
        val shader = GLES30.glCreateShader(type)
        GLES30.glShaderSource(shader, code)
        GLES30.glCompileShader(shader)

        val status = IntArray(1)
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            val error = GLES30.glGetShaderInfoLog(shader)
            GLES30.glDeleteShader(shader)
            throw RuntimeException("Compile shader failed: $error")
        }

        return shader
    }
}
```

这个例子已经具备一个最小 3D 程序的关键元素：

- GLSurfaceView
- Renderer
- 深度测试
- 相机 lookAt
- 透视投影
- 一个 3D 模型

还要特别注意线程模型：Activity 的触摸、生命周期回调通常在主线程，而
`Renderer` 的三个回调运行在 GLSurfaceView 的 GL 线程。UI 状态传给渲染器时，应使用
`queueEvent`、线程安全队列或清晰的同步策略；不要默认两边读写普通字段一定安全。

为了保证这是最小学习版本，忽略了这些东西：

- 纹理
- 灯光
- VBO / VAO 优化
- 复杂输入系统
- 模型加载器

## 10、OpenGL ES 的 3 个核心回调

Android 的 GLSurfaceView.Renderer 最重要的 3 个方法是：

**onSurfaceCreated**

```kotlin
适合做：
    设置背景色
    打开深度测试
    编译 Shader
    创建纹理
    初始化模型
```

**onSurfaceChanged**

```kotlin
适合做：
    设置 glViewport
    根据屏幕宽高比更新投影矩阵
```

**onDrawFrame**

```kotlin
适合做：
    清屏
    更新相机
    更新动画
    计算矩阵
    调用各个模型的 draw
```
