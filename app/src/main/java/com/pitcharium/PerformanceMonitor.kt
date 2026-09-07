package com.pitcharium

import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.view.FrameMetrics
import android.view.Window
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.produceState
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

@Composable
internal fun rememberPerformanceSnapshot(window: Window): State<PerformanceSnapshot> {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    return produceState(PerformanceSnapshot(), window, lifecycle) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.RESUMED) {
            val frames = PerformanceFrames()
            val listener = Window.OnFrameMetricsAvailableListener { _, metrics, dropped ->
                val deadline = if (Build.VERSION.SDK_INT >= 31)
                    metrics.getMetric(FrameMetrics.DEADLINE) else -1L
                val refreshHz = window.decorView.display?.refreshRate
                val fallback = refreshHz?.takeIf { it.isFinite() && it > 0f }
                    ?.let { (1_000_000_000.0 / it).toLong() } ?: -1L
                frames.record(
                    metrics.getMetric(FrameMetrics.TOTAL_DURATION),
                    if (deadline > 0) deadline else fallback,
                    metrics.getMetric(FrameMetrics.FIRST_DRAW_FRAME) == 1L,
                    dropped, deadline <= 0
                )
            }
            var lastWall = SystemClock.elapsedRealtime()
            var lastCpu = Process.getElapsedCpuTime()
            val cores = Runtime.getRuntime().availableProcessors()
            window.addOnFrameMetricsAvailableListener(listener, Handler(Looper.getMainLooper()))
            try {
                while (isActive) {
                    delay(1000)
                    val wall = SystemClock.elapsedRealtime()
                    val cpu = Process.getElapsedCpuTime()
                    val snapshot = frames.sample(wall - lastWall, cpu - lastCpu, cores)
                    lastWall = wall
                    lastCpu = cpu
                    val memory = withContext(Dispatchers.IO) {
                        val info = Debug.MemoryInfo()
                        Debug.getMemoryInfo(info)
                        info.totalPss.takeIf { it > 0 }?.div(1024.0)
                    }
                    value = snapshot.copy(memoryMiB = memory)
                }
            } finally {
                window.removeOnFrameMetricsAvailableListener(listener)
                value = PerformanceSnapshot()
            }
        }
    }
}
