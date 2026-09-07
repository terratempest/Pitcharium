package com.pitcharium

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

enum class NoiseCancellationState {
    OFF,
    CALIBRATING,
    ACTIVE
}

/**
 * Learns a stationary room-noise spectrum during a short quiet calibration window and removes
 * that spectrum from later microphone blocks before they reach the native tuner engine.
 *
 * The active path combines frequency-selective spectral subtraction with a calibrated silence
 * gate. The native visualizers normalize every non-zero frame, so ambient-only buffers are made
 * exactly silent instead of merely very quiet. Clear instrument partials remain open while
 * persistent hum, fan noise, and broadband room noise near the learned profile are attenuated.
 */
internal class CalibratedNoiseSuppressor(
    private val calibrationDurationNanos: Long = DEFAULT_CALIBRATION_DURATION_NANOS
) {
    private val _state = MutableStateFlow(NoiseCancellationState.OFF)
    val state: StateFlow<NoiseCancellationState> = _state.asStateFlow()

    private val _calibrationProgress = MutableStateFlow(0f)
    val calibrationProgress: StateFlow<Float> = _calibrationProgress.asStateFlow()

    private val real = DoubleArray(FFT_SIZE)
    private val imaginary = DoubleArray(FFT_SIZE)
    private val accumulatedNoisePower = DoubleArray(SPECTRUM_SIZE)
    private val noisePowerSpectrum = DoubleArray(SPECTRUM_SIZE)
    private val smoothedBinGain = DoubleArray(SPECTRUM_SIZE) { 1.0 }

    private var calibrationStartedNanos = 0L
    private var calibrationFrameCount = 0L
    private var calibrationSumSquares = 0.0
    private var calibrationSampleCount = 0L
    private var calibratedNoiseRms = 0.0
    private var gateHoldBlocks = 0

    @Synchronized
    fun toggle() {
        when (_state.value) {
            NoiseCancellationState.OFF -> beginCalibration()
            NoiseCancellationState.CALIBRATING,
            NoiseCancellationState.ACTIVE -> disable()
        }
    }

    @Synchronized
    fun disable() {
        _state.value = NoiseCancellationState.OFF
        _calibrationProgress.value = 0f
        calibrationStartedNanos = 0L
        calibrationFrameCount = 0L
        calibrationSumSquares = 0.0
        calibrationSampleCount = 0L
        calibratedNoiseRms = 0.0
        gateHoldBlocks = 0
        accumulatedNoisePower.fill(0.0)
        noisePowerSpectrum.fill(0.0)
        smoothedBinGain.fill(1.0)
    }

    /** Processes a PCM block in place and returns its RMS level after suppression. */
    @Synchronized
    fun processInPlace(
        samples: ShortArray,
        length: Int,
        nowNanos: Long = System.nanoTime()
    ): Double {
        val safeLength = length.coerceIn(0, samples.size)
        if (safeLength == 0) return 0.0

        val rawRms = calculateRms(samples, safeLength)
        when (_state.value) {
            NoiseCancellationState.OFF -> return rawRms

            NoiseCancellationState.CALIBRATING -> {
                collectCalibrationFrames(samples, safeLength)
                calibrationSumSquares += rawRms * rawRms * safeLength
                calibrationSampleCount += safeLength

                if (calibrationStartedNanos == 0L) calibrationStartedNanos = nowNanos
                val elapsed = (nowNanos - calibrationStartedNanos).coerceAtLeast(0L)
                _calibrationProgress.value =
                    (elapsed.toDouble() / calibrationDurationNanos).toFloat().coerceIn(0f, 1f)

                if (elapsed >= calibrationDurationNanos && calibrationFrameCount > 0L) {
                    finishCalibration()
                    silence(samples, safeLength)
                    return 0.0
                }
                return rawRms
            }

            NoiseCancellationState.ACTIVE -> {
                val gateThreshold = (calibratedNoiseRms * IDLE_GATE_MULTIPLIER)
                    .coerceAtLeast(MIN_GATE_RMS)

                if (rawRms > gateThreshold) {
                    gateHoldBlocks = GATE_HOLD_BLOCKS
                } else if (gateHoldBlocks > 0) {
                    gateHoldBlocks--
                } else {
                    silence(samples, safeLength)
                    return 0.0
                }

                var offset = 0
                while (offset + FFT_SIZE <= safeLength) {
                    suppressFrame(samples, offset)
                    offset += FFT_SIZE
                }

                if (offset < safeLength) {
                    val transitionGain = ((rawRms - gateThreshold) / gateThreshold)
                        .coerceIn(0.0, 1.0)
                    applyScalarGain(samples, safeLength - offset, transitionGain, offset)
                }

                val processedRms = calculateRms(samples, safeLength)
                val visualizationSilenceFloor =
                    (calibratedNoiseRms * POST_SUPPRESSION_SILENCE_MULTIPLIER)
                        .coerceAtLeast(MIN_GATE_RMS)
                if (processedRms <= visualizationSilenceFloor && gateHoldBlocks == 0) {
                    silence(samples, safeLength)
                    return 0.0
                }
                return processedRms
            }
        }
    }

    private fun collectCalibrationFrames(samples: ShortArray, length: Int) {
        var offset = 0
        while (offset + FFT_SIZE <= length) {
            loadFrame(samples, offset)
            fft(real, imaginary, inverse = false)
            for (bin in 0 until SPECTRUM_SIZE) {
                accumulatedNoisePower[bin] +=
                    real[bin] * real[bin] + imaginary[bin] * imaginary[bin]
            }
            calibrationFrameCount++
            offset += FFT_SIZE
        }
    }

    private fun finishCalibration() {
        val divisor = calibrationFrameCount.toDouble().coerceAtLeast(1.0)
        for (bin in 0 until SPECTRUM_SIZE) {
            noisePowerSpectrum[bin] = accumulatedNoisePower[bin] / divisor
        }

        // Spread narrow calibrated peaks slightly so hum and fan tones remain covered when their
        // frequency drifts between adjacent FFT bins.
        val copy = noisePowerSpectrum.copyOf()
        for (bin in 0 until SPECTRUM_SIZE) {
            var weighted = copy[bin] * 0.50
            var weight = 0.50
            if (bin > 0) {
                weighted += copy[bin - 1] * 0.20
                weight += 0.20
            }
            if (bin + 1 < SPECTRUM_SIZE) {
                weighted += copy[bin + 1] * 0.20
                weight += 0.20
            }
            if (bin > 1) {
                weighted += copy[bin - 2] * 0.05
                weight += 0.05
            }
            if (bin + 2 < SPECTRUM_SIZE) {
                weighted += copy[bin + 2] * 0.05
                weight += 0.05
            }
            noisePowerSpectrum[bin] = (weighted / weight).coerceAtLeast(MIN_POWER)
        }

        calibratedNoiseRms = if (calibrationSampleCount > 0L) {
            sqrt(calibrationSumSquares / calibrationSampleCount)
        } else {
            MIN_GATE_RMS
        }
        gateHoldBlocks = 0
        smoothedBinGain.fill(MIN_SPECTRAL_GAIN)
        _calibrationProgress.value = 1f
        _state.value = NoiseCancellationState.ACTIVE
    }

    private fun suppressFrame(samples: ShortArray, offset: Int) {
        loadFrame(samples, offset)
        fft(real, imaginary, inverse = false)

        for (bin in 0 until SPECTRUM_SIZE) {
            val power = real[bin] * real[bin] + imaginary[bin] * imaginary[bin]
            val learnedNoise = noisePowerSpectrum[bin] * SPECTRAL_OVERSUBTRACTION
            val residualPower = (power - learnedNoise)
                .coerceAtLeast(power * MIN_SPECTRAL_GAIN * MIN_SPECTRAL_GAIN)
            var targetGain = if (power > MIN_POWER) sqrt(residualPower / power) else MIN_SPECTRAL_GAIN

            // Clearly dominant partials are likely the played instrument and should remain intact.
            if (power > learnedNoise * DOMINANT_PARTIAL_RATIO) {
                targetGain = targetGain.coerceAtLeast(DOMINANT_PARTIAL_MIN_GAIN)
            }
            targetGain = targetGain.coerceIn(MIN_SPECTRAL_GAIN, 1.0)

            val smoothing = if (targetGain > smoothedBinGain[bin]) {
                BIN_ATTACK_SMOOTHING
            } else {
                BIN_RELEASE_SMOOTHING
            }
            smoothedBinGain[bin] += (targetGain - smoothedBinGain[bin]) * smoothing
            val gain = smoothedBinGain[bin].coerceIn(MIN_SPECTRAL_GAIN, 1.0)

            real[bin] *= gain
            imaginary[bin] *= gain
            if (bin in 1 until FFT_SIZE / 2) {
                val mirror = FFT_SIZE - bin
                real[mirror] *= gain
                imaginary[mirror] *= gain
            }
        }

        fft(real, imaginary, inverse = true)
        for (index in 0 until FFT_SIZE) {
            samples[offset + index] = (real[index] * PCM_SCALE)
                .roundToInt()
                .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
                .toShort()
        }
    }

    private fun loadFrame(samples: ShortArray, offset: Int) {
        for (index in 0 until FFT_SIZE) {
            real[index] = samples[offset + index].toDouble() / PCM_SCALE
            imaginary[index] = 0.0
        }
    }

    private fun fft(real: DoubleArray, imaginary: DoubleArray, inverse: Boolean) {
        var j = 0
        for (i in 1 until FFT_SIZE) {
            var bit = FFT_SIZE shr 1
            while (j and bit != 0) {
                j = j xor bit
                bit = bit shr 1
            }
            j = j xor bit
            if (i < j) {
                val realTemp = real[i]
                real[i] = real[j]
                real[j] = realTemp
                val imaginaryTemp = imaginary[i]
                imaginary[i] = imaginary[j]
                imaginary[j] = imaginaryTemp
            }
        }

        var length = 2
        while (length <= FFT_SIZE) {
            val angle = 2.0 * PI / length * if (inverse) 1.0 else -1.0
            val wLengthReal = cos(angle)
            val wLengthImaginary = sin(angle)
            var start = 0
            while (start < FFT_SIZE) {
                var wReal = 1.0
                var wImaginary = 0.0
                for (index in 0 until length / 2) {
                    val even = start + index
                    val odd = even + length / 2
                    val oddReal = real[odd] * wReal - imaginary[odd] * wImaginary
                    val oddImaginary = real[odd] * wImaginary + imaginary[odd] * wReal

                    real[odd] = real[even] - oddReal
                    imaginary[odd] = imaginary[even] - oddImaginary
                    real[even] += oddReal
                    imaginary[even] += oddImaginary

                    val nextWReal = wReal * wLengthReal - wImaginary * wLengthImaginary
                    wImaginary = wReal * wLengthImaginary + wImaginary * wLengthReal
                    wReal = nextWReal
                }
                start += length
            }
            length = length shl 1
        }

        if (inverse) {
            for (index in 0 until FFT_SIZE) {
                real[index] = real[index] / FFT_SIZE.toDouble()
                imaginary[index] = imaginary[index] / FFT_SIZE.toDouble()
            }
        }
    }

    private fun calculateRms(samples: ShortArray, length: Int): Double {
        var sumSquares = 0.0
        for (index in 0 until length) {
            val normalized = samples[index].toDouble() / PCM_SCALE
            sumSquares += normalized * normalized
        }
        return sqrt(sumSquares / length)
    }

    private fun applyScalarGain(
        samples: ShortArray,
        length: Int,
        gain: Double,
        offset: Int = 0
    ) {
        for (index in offset until offset + length) {
            samples[index] = (samples[index] * gain)
                .roundToInt()
                .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
                .toShort()
        }
    }

    private fun silence(samples: ShortArray, length: Int) {
        samples.fill(0, fromIndex = 0, toIndex = length)
    }

    private fun beginCalibration() {
        calibrationStartedNanos = 0L
        calibrationFrameCount = 0L
        calibrationSumSquares = 0.0
        calibrationSampleCount = 0L
        calibratedNoiseRms = 0.0
        gateHoldBlocks = 0
        accumulatedNoisePower.fill(0.0)
        noisePowerSpectrum.fill(0.0)
        smoothedBinGain.fill(1.0)
        _calibrationProgress.value = 0f
        _state.value = NoiseCancellationState.CALIBRATING
    }

    private companion object {
        const val DEFAULT_CALIBRATION_DURATION_NANOS = 3_000_000_000L
        const val FFT_SIZE = 1024
        const val SPECTRUM_SIZE = FFT_SIZE / 2 + 1
        const val PCM_SCALE = 32768.0
        const val MIN_POWER = 1.0e-15
        const val MIN_GATE_RMS = 1.0e-5
        const val IDLE_GATE_MULTIPLIER = 1.8
        const val POST_SUPPRESSION_SILENCE_MULTIPLIER = 0.35
        const val GATE_HOLD_BLOCKS = 4
        const val SPECTRAL_OVERSUBTRACTION = 2.4
        const val MIN_SPECTRAL_GAIN = 0.035
        const val DOMINANT_PARTIAL_RATIO = 8.0
        const val DOMINANT_PARTIAL_MIN_GAIN = 0.88
        const val BIN_ATTACK_SMOOTHING = 0.82
        const val BIN_RELEASE_SMOOTHING = 0.55
    }
}
