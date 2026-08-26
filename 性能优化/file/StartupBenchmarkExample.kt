package com.example.benchmark

import androidx.benchmark.macro.CompilationMode
import androidx.benchmark.macro.StartupMode
import androidx.benchmark.macro.StartupTimingMetric
import androidx.benchmark.macro.junit4.MacrobenchmarkRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.filters.LargeTest
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * 示例代码仅用于说明 APA 与 Macrobenchmark 的组合方式。
 *
 * 请替换：
 * 1. targetPackage；
 * 2. 启动前的业务准备动作；
 * 3. iterations 与 compilationMode。
 */
@LargeTest
@RunWith(AndroidJUnit4::class)
class StartupBenchmarkExample {

    @get:Rule
    val benchmarkRule = MacrobenchmarkRule()

    @Test
    fun coldStartup() {
        benchmarkRule.measureRepeated(
            packageName = "com.example.app",
            metrics = listOf(StartupTimingMetric()),
            iterations = 10,
            startupMode = StartupMode.COLD,
            compilationMode = CompilationMode.Partial()
        ) {
            pressHome()
            startActivityAndWait()
        }
    }
}

