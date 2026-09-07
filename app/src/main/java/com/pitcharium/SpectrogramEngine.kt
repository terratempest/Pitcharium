package com.pitcharium

class SpectrogramEngine {
    companion object {
        init {
            System.loadLibrary("spectrogram-jni")
        }
    }

    /**
     * Initializes or resets the spectrogram engine with the given sample rate and step overlap divisor.
     * Precomputes all static tables and configures parameters.
     */
    external fun init(sampleRate: Int, step: Int, useMultithreading: Boolean)

    /**
     * Updates the reference frequency for A4 (typically 440 Hz). Recomputes frequency-to-pitch maps.
     */
    external fun setA4Reference(a4: Int)

    /**
     * Returns the current A4 reference frequency.
     */
    external fun getA4Reference(): Int

    /** Copies valid, raw Hz, stabilized Hz, pitch position, and confidence atomically. */
    external fun getProInspiredResult(outResult: FloatArray): Boolean
    external fun resetFrequencyDetectionState()

    /** Recovered Pro frontend used by the single detector path. */
    external fun runProEstimatorStageA(samples: ShortArray, hashes: LongArray, scalars: FloatArray): Boolean

    /** Returns 0x1f when Stage A dimensions, operand tables, and silence invariants match. */
    external fun validateProEstimatorStageA(): Long
    external fun validateProEstimatorBackend(): Long
    /** Returns 0xf when the recovered work68 chain checks pass. */
    external fun validateProWork68Chain(): Long
    /** Returns 0x1b when the Pro tracker candidate state-machine checks pass. */
    external fun validateProTrackerAssociation(): Long
    /** Returns 0x7 for fresh Pro timing, strict gate, and signed phase checks. */
    external fun validateProEstimatorContract(): Long
    /** Returns 0x3 when fresh Pro raw/effective frame geometry and 4:1 ingress are valid. */
    external fun validateProIngressContract(): Long
    external fun validateProEstimatorEndToEnd(): Long

    /**
     * Appends mono 16-bit PCM samples to the engine circular buffer.
     * Automatically processes all complete N-sized frames using the specified overlap hop size.
     * Updates the internal 256-column history texture.
     * Returns the number of columns generated during this processing call.
     */
    external fun processAudio(samples: ShortArray, length: Int): Int

    /** Drains generated columns in chronological order; pixels are column-major, 256 per column. */
    external fun drainColumns(
        outPixels: IntArray,
        outSequences: LongArray,
        outSamplePositions: LongArray,
        outStatus: LongArray,
        maxColumns: Int
    ): Int

    external fun resetPresentationQueue()

    /**
     * Unwraps the circular texture history into the output array of size 256x256.
     * Output pixel format is packed ARGB (0xAARRGGBB) for direct bitmap manipulation.
     */
    external fun getHistoryTexture(outPixels: IntArray)

    /**
     * Copies the latest 192 folded octave spectrum rows for visualization or diagnostics.
     */
    external fun getFoldedSpectrum(outFolded: IntArray)

    /**
     * Cleans up dynamic memory allocations inside the JNI layer.
     */
    external fun cleanup()
}
