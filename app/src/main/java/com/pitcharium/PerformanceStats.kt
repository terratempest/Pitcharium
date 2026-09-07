package com.pitcharium

internal data class PerformanceSnapshot(
    val fps: Double? = null,
    val slowPercent: Double? = null,
    val cpuPercent: Double? = null,
    val memoryMiB: Double? = null,
    val approximateFrames: Boolean = false,
    val estimatedSlowFrames: Boolean = false
)

internal class PerformanceFrames {
    private var frames = 0L
    private var eligible = 0L
    private var slow = 0L
    private var dropped = false
    private var estimated = false

    fun record(durationNanos: Long, deadlineNanos: Long, firstDraw: Boolean,
               droppedReports: Int, estimatedDeadline: Boolean) {
        frames++
        dropped = dropped || droppedReports > 0
        if (!firstDraw && durationNanos >= 0 && deadlineNanos > 0) {
            eligible++
            if (durationNanos > deadlineNanos) slow++
            estimated = estimated || estimatedDeadline
        }
    }

    fun sample(elapsedMillis: Long, cpuMillis: Long, cores: Int): PerformanceSnapshot {
        val result = PerformanceSnapshot(
            fps = if (elapsedMillis > 0) frames * 1000.0 / elapsedMillis else null,
            slowPercent = if (eligible > 0) slow * 100.0 / eligible else null,
            cpuPercent = if (elapsedMillis > 0 && cpuMillis >= 0 && cores > 0)
                (cpuMillis * 100.0 / elapsedMillis / cores).coerceIn(0.0, 100.0) else null,
            approximateFrames = dropped,
            estimatedSlowFrames = estimated
        )
        frames = 0
        eligible = 0
        slow = 0
        dropped = false
        estimated = false
        return result
    }
}
