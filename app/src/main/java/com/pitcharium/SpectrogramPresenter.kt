package com.pitcharium

import android.graphics.Bitmap
import android.graphics.Color
import kotlin.math.max

internal class SpectrogramPresenter(private val engine: SpectrogramEngine) {
    data class Telemetry(
        val generated: Long = 0,
        val presented: Long = 0,
        val pending: Long = 0,
        val maximumDepth: Long = 0,
        val late: Long = 0,
        val coalesced: Long = 0
    )

    private data class Column(val pixels: IntArray = IntArray(256), var samplePosition: Long = 0)

    val bitmap: Bitmap = Bitmap.createBitmap(1024, 256, Bitmap.Config.ARGB_8888).apply {
        eraseColor(Color.BLACK)
    }
    private val drainPixels = IntArray(256 * 256)
    private val drainSequences = LongArray(256)
    private val drainPositions = LongArray(256)
    private val status = LongArray(9)
    private var queue = Array(512) { Column() }
    private var head = 0
    private var count = 0
    private var epoch = -1L
    private var sampleRate = 44100
    private var hop = 1024
    private var anchorNanos = Long.MIN_VALUE
    private var anchorSample = 0L
    private var writeIndex = 256L
    private var presented = 0L
    private var late = 0L
    private var coalesced = 0L
    private var speed = 1f

    var fraction: Float = 0f
        private set
    var telemetry: Telemetry = Telemetry()
        private set
    val latestColumn = IntArray(256)

    val visibleColumns: Float get() = 256f / speed
    val sourceStart: Float
        get() = (writeIndex and 1023L).toFloat() - visibleColumns + fraction

    fun setScrollSpeed(value: Float) {
        require(value.isFinite() && value > 0f)
        speed = value
    }

    fun reset(rate: Int, analysisStep: Int) {
        sampleRate = rate
        hop = when (rate) {
            8000, 11025 -> 2048
            22050 -> 4096
            else -> 8192
        } / analysisStep
        clearPresentation()
    }

    fun advance(frameNanos: Long): Boolean {
        drainNative()
        if (count == 0) {
            fraction = 0f
            updateTelemetry()
            return false
        }
        if (anchorNanos == Long.MIN_VALUE) {
            // Keep one AudioRecord block queued so 2,048-sample reads cannot burst columns visually.
            anchorNanos = frameNanos + 2048L * 1_000_000_000L / sampleRate
            anchorSample = queue[head].samplePosition
        }

        var committed = 0
        while (count > 0 && dueNanos(queue[head].samplePosition) <= frameNanos) {
            val due = dueNanos(queue[head].samplePosition)
            if (frameNanos - due > hopNanos()) late++
            val column = queue[head]
            val x = (writeIndex and 1023L).toInt()
            bitmap.setPixels(column.pixels, 0, 1, x, 0, 1, 256)
            column.pixels.copyInto(latestColumn)
            writeIndex++
            head = (head + 1) % queue.size
            count--
            presented++
            committed++
        }
        if (committed > 0) updatePreviewGuards()
        if (committed > 1) coalesced += committed - 1L

        fraction = if (count == 0) 0f else {
            val nextDue = dueNanos(queue[head].samplePosition)
            ((frameNanos - (nextDue - hopNanos())).toDouble() / hopNanos())
                .coerceIn(0.0, 1.0).toFloat()
        }
        updateTelemetry()
        return committed > 0 || fraction > 0f
    }

    private fun drainNative() {
        val drained = engine.drainColumns(drainPixels, drainSequences, drainPositions, status, 256)
        if (epoch != status[3]) {
            epoch = status[3]
            clearPresentation()
        }
        ensureCapacity(count + drained)
        for (i in 0 until drained) {
            val tail = (head + count) % queue.size
            drainPixels.copyInto(queue[tail].pixels, 0, i * 256, (i + 1) * 256)
            queue[tail].samplePosition = drainPositions[i]
            count++
        }
        if (drained > 0) updatePreviewGuards()
    }

    private fun ensureCapacity(required: Int) {
        if (required <= queue.size) return
        var size = queue.size
        while (size < required) size *= 2
        val grown = Array(size) { Column() }
        for (i in 0 until count) grown[i] = queue[(head + i) % queue.size]
        queue = grown
        head = 0
    }

    private fun clearPresentation() {
        head = 0
        count = 0
        anchorNanos = Long.MIN_VALUE
        writeIndex = 256L
        presented = 0
        late = 0
        coalesced = 0
        fraction = 0f
        bitmap.eraseColor(Color.BLACK)
        telemetry = Telemetry()
        latestColumn.fill(0)
    }

    private fun updatePreviewGuards() {
        for (offset in 0..1) {
            val x = ((writeIndex + offset) and 1023L).toInt()
            val pixels = if (offset < count) {
                queue[(head + offset) % queue.size].pixels
            } else {
                BLACK_COLUMN
            }
            bitmap.setPixels(pixels, 0, 1, x, 0, 1, 256)
        }
    }

    private fun hopNanos(): Long = max(1L, hop * 1_000_000_000L / sampleRate)
    private fun dueNanos(samplePosition: Long): Long =
        anchorNanos + (samplePosition - anchorSample) * 1_000_000_000L / sampleRate

    private fun updateTelemetry() {
        telemetry = Telemetry(status[2], presented, status[0] + count, status[1], late, coalesced)
    }

    private companion object {
        val BLACK_COLUMN = IntArray(256) { Color.BLACK }
    }
}
