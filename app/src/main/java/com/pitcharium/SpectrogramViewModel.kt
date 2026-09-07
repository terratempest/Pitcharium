package com.pitcharium

import android.annotation.SuppressLint
import android.app.Application
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlin.math.log10

private val LEGACY_DETECTOR_PREFERENCE_KEYS = arrayOf(
    "frequency_strategy_id",
    "use_floating_point",
    "use_estimated_float_frequency",
    "float_smoothing_window",
    "use_fundamental_detection",
)

internal fun cleanLegacyDetectorPreferences(prefs: SharedPreferences) {
    val editor = prefs.edit()
    LEGACY_DETECTOR_PREFERENCE_KEYS.forEach(editor::remove)
    editor.apply()
}

class SpectrogramViewModel(application: Application) : AndroidViewModel(application) {

    private val engine = SpectrogramEngine()
    internal val presenter = SpectrogramPresenter(engine)
    private val noiseSuppressor = CalibratedNoiseSuppressor()

    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()

    val noiseCancellationState: StateFlow<NoiseCancellationState> = noiseSuppressor.state
    val noiseCalibrationProgress: StateFlow<Float> = noiseSuppressor.calibrationProgress

    private val _sampleRate = MutableStateFlow(44100)
    val sampleRate: StateFlow<Int> = _sampleRate.asStateFlow()

    private val _step = MutableStateFlow(8)
    val step: StateFlow<Int> = _step.asStateFlow()

    private val _a4Reference = MutableStateFlow(440)
    val a4Reference: StateFlow<Int> = _a4Reference.asStateFlow()

    private val _scrollSpeed = MutableStateFlow(1f)
    val scrollSpeed: StateFlow<Float> = _scrollSpeed.asStateFlow()

    private val _permissionGranted = MutableStateFlow(false)
    val permissionGranted: StateFlow<Boolean> = _permissionGranted.asStateFlow()

    private val _useMultithreading = MutableStateFlow(true)
    val useMultithreading: StateFlow<Boolean> = _useMultithreading.asStateFlow()

    // Real-time metadata for display
    private val _detectedNote = MutableStateFlow("None")
    val detectedNote: StateFlow<String> = _detectedNote.asStateFlow()

    private val _centsDeviation = MutableStateFlow(0f)
    val centsDeviation: StateFlow<Float> = _centsDeviation.asStateFlow()

    private val _signalDb = MutableStateFlow(-120f)
    val signalDb: StateFlow<Float> = _signalDb.asStateFlow()

    private val _detectedFrequency = MutableStateFlow(0f)
    val detectedFrequency: StateFlow<Float> = _detectedFrequency.asStateFlow()

    private val _detectedOctave = MutableStateFlow(-1)
    val detectedOctave: StateFlow<Int> = _detectedOctave.asStateFlow()

    private var audioRecord: AudioRecord? = null
    private var recordingJob: Job? = null
    private var analysisJob: Job? = null
    private var reconfigureJob: Job? = null
    private val reconfigureMutex = Mutex()
    @Volatile private var engineInitialized = false
    private var pendingAutoStart = false
    private val proInspiredResult = FloatArray(5)
    private var analysisTick = 0
    private var lastLoggedFrequency = -1f
    private var lastLoggedNote = "None"

    val noteNames = listOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

    private val prefs = application.getSharedPreferences("spectrogram_settings", Context.MODE_PRIVATE)

    init {
        // Load persisted settings before running the engine
        _sampleRate.value = prefs.getInt("sample_rate", 44100)
        _step.value = prefs.getInt("step", 8)
        _a4Reference.value = prefs.getInt("a4_reference", 440)
        _scrollSpeed.value = prefs.getFloat("scroll_speed", 1f)
        _useMultithreading.value = prefs.getBoolean("use_multithreading", true)
        cleanLegacyDetectorPreferences(prefs)
        Log.d(TAG, "startup detector=PRO_INSPIRED legacy detector preferences cleaned")
        presenter.setScrollSpeed(_scrollSpeed.value)

        checkPermission()
        reinitializeEngine(startWhenReady = _permissionGranted.value)
    }

    fun checkPermission() {
        val granted = ContextCompat.checkSelfPermission(
            getApplication(),
            android.Manifest.permission.RECORD_AUDIO
        ) == PackageManager.PERMISSION_GRANTED
        _permissionGranted.value = granted
        if (granted) requestAutoStart()
    }

    fun setPermissionGranted(granted: Boolean) {
        _permissionGranted.value = granted
        if (granted) requestAutoStart()
    }

    private fun reinitializeEngine(startWhenReady: Boolean) {
        reconfigureJob?.cancel()
        reconfigureJob = viewModelScope.launch {
            reconfigureMutex.withLock {
                noiseSuppressor.disable()
                val recording = recordingJob
                val analysis = analysisJob
                _isRecording.value = false
                recording?.cancelAndJoin()
                analysis?.cancelAndJoin()
                recordingJob = null
                analysisJob = null
                engineInitialized = false

                val rate = _sampleRate.value
                val analysisStep = _step.value
                val reference = _a4Reference.value
                val useMulti = _useMultithreading.value
                withContext(Dispatchers.Default) {
                    engine.init(rate, analysisStep, useMulti)
                    engine.setA4Reference(reference)
                    Log.d(TAG, "engine init detector=PRO_INSPIRED multithreading=$useMulti")
                }
                presenter.reset(rate, analysisStep)
                engineInitialized = true
                if (startWhenReady) requestAutoStart()
            }
        }
    }

    private fun requestAutoStart() {
        Log.d(TAG, "requestAutoStart permission=${_permissionGranted.value} engineInitialized=$engineInitialized isRecording=${_isRecording.value}")
        pendingAutoStart = true
        maybeStartRecording()
    }

    private fun maybeStartRecording() {
        Log.d(TAG, "maybeStartRecording pending=$pendingAutoStart permission=${_permissionGranted.value} engineInitialized=$engineInitialized isRecording=${_isRecording.value}")
        if (!pendingAutoStart) return
        if (!_permissionGranted.value || !engineInitialized || _isRecording.value) return
        pendingAutoStart = false
        startRecording()
    }

    fun updateSampleRate(newRate: Int) {
        if (newRate != _sampleRate.value) {
            _sampleRate.value = newRate
            prefs.edit().putInt("sample_rate", newRate).apply()
            val wasRecording = _isRecording.value
            reinitializeEngine(startWhenReady = wasRecording)
        }
    }

    fun updateStep(newStep: Int) {
        if (newStep != _step.value) {
            _step.value = newStep
            prefs.edit().putInt("step", newStep).apply()
            val wasRecording = _isRecording.value
            reinitializeEngine(startWhenReady = wasRecording)
        }
    }

    fun updateMultithreading(enabled: Boolean) {
        if (enabled != _useMultithreading.value) {
            _useMultithreading.value = enabled
            prefs.edit().putBoolean("use_multithreading", enabled).apply()
            val wasRecording = _isRecording.value
            reinitializeEngine(startWhenReady = wasRecording)
        }
    }

    fun updateA4Reference(newA4: Int) {
        if (newA4 != _a4Reference.value) {
            _a4Reference.value = newA4
            prefs.edit().putInt("a4_reference", newA4).apply()
            engine.setA4Reference(newA4)
        }
    }

    fun updateScrollSpeed(newSpeed: Float) {
        if (newSpeed != _scrollSpeed.value) {
            _scrollSpeed.value = newSpeed
            prefs.edit().putFloat("scroll_speed", newSpeed).apply()
            presenter.setScrollSpeed(newSpeed)
        }
    }

    fun toggleRecording() {
        if (_isRecording.value) {
            stopRecording()
        } else {
            startRecording()
        }
    }

    fun toggleNoiseCancellation() {
        if (noiseSuppressor.state.value == NoiseCancellationState.OFF && !_isRecording.value) {
            startRecording()
            if (!_isRecording.value) return
        }
        noiseSuppressor.toggle()
    }

    @SuppressLint("MissingPermission")
    fun startRecording() {
        Log.d(TAG, "startRecording entry permission=${_permissionGranted.value} engineInitialized=$engineInitialized isRecording=${_isRecording.value}")
        if (!_permissionGranted.value) {
            checkPermission()
            if (!_permissionGranted.value) return
        }

        if (_isRecording.value || !engineInitialized) {
            Log.d(TAG, "startRecording skip isRecording=${_isRecording.value} engineInitialized=$engineInitialized")
            return
        }
        engine.resetFrequencyDetectionState()

        _isRecording.value = true

        recordingJob = viewModelScope.launch(Dispatchers.IO) {
            val rate = _sampleRate.value
            val minBufferSize = AudioRecord.getMinBufferSize(
                rate,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT
            )
            val bufferSize = maxOf(minBufferSize, 4096)

            try {
                val record = AudioRecord(
                    MediaRecorder.AudioSource.MIC,
                    rate,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    bufferSize
                )
                audioRecord = record

                val audioBuffer = ShortArray(2048)
                record.startRecording()

                while (isActive && _isRecording.value) {
                    val readSamples = record.read(audioBuffer, 0, audioBuffer.size)
                    if (readSamples > 0) {
                        if (!isActive || !_isRecording.value) break

                        // Calibrate or suppress room noise before native pitch detection and rendering.
                        val rms = noiseSuppressor.processInPlace(audioBuffer, readSamples)
                        val db = if (rms > 0.0) 20.0 * log10(rms) else -120.0
                        _signalDb.value = db.toFloat()

                        // Process the calibrated/suppressed PCM natively.
                        engine.processAudio(audioBuffer, readSamples)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "startRecording failed", e)
                e.printStackTrace()
            } finally {
                audioRecord?.let {
                    try {
                        it.stop()
                        it.release()
                    } catch (e: Exception) {}
                }
                audioRecord = null
            }
        }

        // Start real-time analytical metadata pooling (like note detection and cents)
        analysisJob = viewModelScope.launch(Dispatchers.Default) {
            while (isActive && _isRecording.value) {
                delay(100) // Poll at 10 Hz
                analysisTick++
                val result = proInspiredResult
                if (engine.getProInspiredResult(result)) {
                    val rawHz = result[1]
                    val stableHz = result[2]
                    val pitch = result[3]
                    val confidence = result[4]
                    val frequency = when {
                        stableHz > 0f && stableHz.isFinite() -> stableHz
                        rawHz > 0f && rawHz.isFinite() -> rawHz
                        else -> -1f
                    }
                    if (frequency > 0f) {
                        setDetectionFromFrequency(frequency)
                        if (analysisTick % 5 == 0 || _detectedFrequency.value != lastLoggedFrequency || _detectedNote.value != lastLoggedNote) {
                            Log.d(TAG, "pro result tick=$analysisTick raw=$rawHz stable=$stableHz pitch=$pitch conf=$confidence hz=${_detectedFrequency.value} cents=${_centsDeviation.value}")
                            lastLoggedFrequency = _detectedFrequency.value
                            lastLoggedNote = _detectedNote.value
                        }
                    } else {
                        _detectedNote.value = "None"
                        _centsDeviation.value = 0f
                        _detectedFrequency.value = 0f
                        _detectedOctave.value = -1
                    }
                } else {
                    _detectedNote.value = "None"
                    _centsDeviation.value = 0f
                    _detectedFrequency.value = 0f
                    _detectedOctave.value = -1
                }
            }
        }
    }

    fun stopRecording() {
        _isRecording.value = false
        noiseSuppressor.disable()
        recordingJob?.cancel()
        analysisJob?.cancel()
        recordingJob = null
        analysisJob = null
        if (engineInitialized) engine.resetFrequencyDetectionState()
        if (engineInitialized) engine.resetPresentationQueue()
        presenter.reset(_sampleRate.value, _step.value)
        _detectedNote.value = "None"
        _centsDeviation.value = 0f
        _signalDb.value = -120f
        _detectedFrequency.value = 0f
        _detectedOctave.value = -1
    }

    private fun setDetectionFromFrequency(frequency: Float) {
        val notePosition = 57.0 + 12.0 * (kotlin.math.ln(frequency / _a4Reference.value) / kotlin.math.ln(2.0))
        val nearestNote = kotlin.math.round(notePosition).toInt()
        _detectedNote.value = noteNames[Math.floorMod(nearestNote, 12)]
        _centsDeviation.value = ((notePosition - nearestNote) * 100.0).toFloat()
        _detectedFrequency.value = frequency
        _detectedOctave.value = Math.floorDiv(nearestNote, 12)
    }

    fun getHistoryTexture(outPixels: IntArray) {
        engine.getHistoryTexture(outPixels)
    }

    override fun onCleared() {
        super.onCleared()
        reconfigureJob?.cancel()
        stopRecording()
        engine.cleanup()
    }

    companion object {
        private const val TAG = "SpectrogramVM"
    }
}
