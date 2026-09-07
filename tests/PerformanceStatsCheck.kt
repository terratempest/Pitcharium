package com.pitcharium

fun main() {
    val frames = PerformanceFrames()
    check(frames.sample(0, 0, 4).fps == null)
    check(frames.sample(1000, 0, 4).let { it.fps == 0.0 && it.slowPercent == null })
    frames.record(12_000_000, 16_666_667, false, 0, true) // 60 Hz: within budget
    frames.record(12_000_000, 8_333_333, false, 1, true) // 120 Hz: slow
    frames.record(90_000_000, 8_333_333, true, 0, false) // first draw excluded
    val sample = frames.sample(1000, 1000, 4)
    check(sample.fps == 3.0 && sample.slowPercent == 50.0 && sample.cpuPercent == 25.0)
    check(sample.approximateFrames && sample.estimatedSlowFrames)
    check(frames.sample(1000, 0, 4).let {
        it.fps == 0.0 && it.slowPercent == null && !it.approximateFrames && !it.estimatedSlowFrames
    })
    frames.record(10, 10, false, 0, false)
    check(frames.sample(500, 0, 4).let { it.fps == 2.0 && it.slowPercent == 0.0 })
    println("Performance statistics checks passed")
}
