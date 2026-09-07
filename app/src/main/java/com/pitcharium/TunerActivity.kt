package com.pitcharium

import android.Manifest
import android.os.Bundle
import android.view.Window
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.pitcharium.ui.theme.PitchariumTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.math.abs

enum class TunerScreenMode(val title: String) {
    SPECTROGRAM("SPECTROGRAM"),
    RADIAL("RADIAL"),
    INSTRUMENT("INSTRUMENT TUNER")
}

class TunerActivity : ComponentActivity() {
    private val viewModel: SpectrogramViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enableEdgeToEdge()
        setContent {
            PitchariumTheme {
                Scaffold(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(LabBackground),
                    contentWindowInsets = WindowInsets(0, 0, 0, 0)
                ) { innerPadding ->
                    PitchariumRoot(
                        viewModel = viewModel,
                        window = window,
                        modifier = Modifier.padding(innerPadding)
                    )
                }
            }
        }
    }
}

@Composable
private fun PitchariumRoot(
    viewModel: SpectrogramViewModel,
    window: Window,
    modifier: Modifier = Modifier
) {
    val permissionGranted by viewModel.permissionGranted.collectAsStateWithLifecycle()
    val isRecording by viewModel.isRecording.collectAsStateWithLifecycle()
    val sampleRate by viewModel.sampleRate.collectAsStateWithLifecycle()
    val step by viewModel.step.collectAsStateWithLifecycle()
    val a4Reference by viewModel.a4Reference.collectAsStateWithLifecycle()
    val scrollSpeed by viewModel.scrollSpeed.collectAsStateWithLifecycle()
    val useMultithreading by viewModel.useMultithreading.collectAsStateWithLifecycle()
    val detectedNote by viewModel.detectedNote.collectAsStateWithLifecycle()
    val centsDeviation by viewModel.centsDeviation.collectAsStateWithLifecycle()
    val signalDb by viewModel.signalDb.collectAsStateWithLifecycle()
    val detectedFrequency by viewModel.detectedFrequency.collectAsStateWithLifecycle()
    val detectedOctave by viewModel.detectedOctave.collectAsStateWithLifecycle()
    val noiseCancellationState by viewModel.noiseCancellationState.collectAsStateWithLifecycle()
    val noiseCalibrationProgress by viewModel.noiseCalibrationProgress.collectAsStateWithLifecycle()

    var screenIndex by rememberSaveable { mutableStateOf(0) }
    var totalDrag by remember { mutableFloatStateOf(0f) }
    var showPerformanceOverlay by rememberSaveable { mutableStateOf(false) }
    val screens = TunerScreenMode.entries
    val currentScreen = screens[screenIndex]

    val requestPermissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted -> viewModel.setPermissionGranted(granted) }

    LaunchedEffect(Unit) { viewModel.checkPermission() }

    if (!permissionGranted) {
        MicrophonePermissionScreen(
            modifier = modifier,
            onRequestPermission = {
                requestPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
            }
        )
        return
    }

    val drawerState = rememberDrawerState(initialValue = DrawerValue.Closed)
    val scope = rememberCoroutineScope()

    LaunchedEffect(permissionGranted) {
        repeat(20) {
            viewModel.startRecording()
            if (viewModel.isRecording.value) return@LaunchedEffect
            delay(100)
        }
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        gesturesEnabled = true,
        drawerContent = {
            EngineSettingsDrawer(
                isRecording = isRecording,
                sampleRate = sampleRate,
                step = step,
                a4Reference = a4Reference,
                scrollSpeed = scrollSpeed,
                useMultithreading = useMultithreading,
                showPerformanceOverlay = showPerformanceOverlay,
                onToggleRecording = viewModel::toggleRecording,
                onSampleRateChange = viewModel::updateSampleRate,
                onStepChange = viewModel::updateStep,
                onA4Change = viewModel::updateA4Reference,
                onScrollSpeedChange = viewModel::updateScrollSpeed,
                onUseMultithreadingChange = viewModel::updateMultithreading,
                onPerformanceOverlayChange = { showPerformanceOverlay = it }
            )
        }
    ) {
        Column(
            modifier = modifier
                .fillMaxSize()
                .statusBarsPadding()
                .background(LabBackground)
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .pointerInput(screenIndex) {
                        detectHorizontalDragGestures(
                            onDragStart = { totalDrag = 0f },
                            onDragEnd = {
                                screenIndex = when {
                                    totalDrag < -100f -> (screenIndex + 1) % screens.size
                                    totalDrag > 100f -> (screenIndex - 1 + screens.size) % screens.size
                                    else -> screenIndex
                                }
                            },
                            onHorizontalDrag = { change, dragAmount ->
                                change.consume()
                                totalDrag += dragAmount
                            }
                        )
                    }
            ) {
                when (currentScreen) {
                    TunerScreenMode.SPECTROGRAM -> SpectrogramVisualizer(
                        viewModel = viewModel,
                        isRecording = isRecording,
                        detectedNote = detectedNote,
                        scrollSpeed = scrollSpeed
                    )

                    TunerScreenMode.RADIAL -> RadialVisualizer(
                        viewModel = viewModel,
                        isRecording = isRecording,
                        detectedNote = detectedNote,
                        centsDeviation = centsDeviation,
                        detectedOctave = detectedOctave
                    )

                    TunerScreenMode.INSTRUMENT -> InstrumentTuningScreen(
                        viewModel = viewModel,
                        detectedFrequency = detectedFrequency,
                        signalDb = signalDb,
                        a4Reference = a4Reference,
                        isRecording = isRecording
                    )
                }

                if (showPerformanceOverlay) {
                    PerformanceSummaryOverlay(
                        window = window,
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(16.dp)
                    )
                }

                if (noiseCancellationState == NoiseCancellationState.CALIBRATING) {
                    NoiseCalibrationOverlay(
                        progress = noiseCalibrationProgress,
                        modifier = Modifier.align(Alignment.Center)
                    )
                }
            }

            ScreenAwareTuningPanel(
                screenTitle = currentScreen.title,
                detectedNote = detectedNote,
                centsDeviation = centsDeviation,
                isRecording = isRecording,
                detectedFrequency = detectedFrequency,
                detectedOctave = detectedOctave,
                noiseCancellationState = noiseCancellationState,
                onToggleNoiseCancellation = viewModel::toggleNoiseCancellation,
                onOpenSettings = { scope.launch { drawerState.open() } }
            )
        }
    }
}

@Composable
private fun MicrophonePermissionScreen(
    modifier: Modifier,
    onRequestPermission: () -> Unit
) {
    Box(
        modifier = modifier
            .fillMaxSize()
            .background(LabBackground)
            .systemBarsPadding()
            .padding(24.dp),
        contentAlignment = Alignment.Center
    ) {
        Card(
            modifier = Modifier
                .fillMaxWidth()
                .wrapContentHeight()
                .border(1.dp, LabAccentAmber.copy(alpha = 0.2f), RoundedCornerShape(24.dp)),
            colors = CardDefaults.cardColors(containerColor = LabSurface),
            shape = RoundedCornerShape(24.dp)
        ) {
            Column(
                modifier = Modifier.padding(32.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(24.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.Info,
                    contentDescription = "Microphone needed",
                    tint = LabAccentAmber,
                    modifier = Modifier.size(64.dp)
                )
                Text(
                    text = stringResource(R.string.app_name),
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.Medium,
                    color = LabAccentAmber,
                    letterSpacing = 1.5.sp
                )
                Text(
                    text = "Microphone access is required for the spectrogram and instrument tuner. Audio remains on-device and is processed by the native engine.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = LabTextSecondary,
                    textAlign = TextAlign.Center,
                    lineHeight = 22.sp
                )
                Button(
                    onClick = onRequestPermission,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = LabAccentAmber,
                        contentColor = LabButtonText
                    ),
                    shape = RoundedCornerShape(12.dp),
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                ) {
                    Text("Grant Microphone Access", fontWeight = FontWeight.SemiBold)
                }
            }
        }
    }
}

@Composable
private fun EngineSettingsDrawer(
    isRecording: Boolean,
    sampleRate: Int,
    step: Int,
    a4Reference: Int,
    scrollSpeed: Float,
    useMultithreading: Boolean,
    showPerformanceOverlay: Boolean,
    onToggleRecording: () -> Unit,
    onSampleRateChange: (Int) -> Unit,
    onStepChange: (Int) -> Unit,
    onA4Change: (Int) -> Unit,
    onScrollSpeedChange: (Float) -> Unit,
    onUseMultithreadingChange: (Boolean) -> Unit,
    onPerformanceOverlayChange: (Boolean) -> Unit
) {
    ModalDrawerSheet(
        drawerContainerColor = LabSurface,
        drawerShape = RoundedCornerShape(topEnd = 16.dp, bottomEnd = 16.dp),
        modifier = Modifier
            .width(320.dp)
            .fillMaxHeight()
            .border(
                1.dp,
                Color.White.copy(alpha = 0.05f),
                RoundedCornerShape(topEnd = 16.dp, bottomEnd = 16.dp)
            )
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(LabSurface)
                .verticalScroll(rememberScrollState())
                .statusBarsPadding()
                .navigationBarsPadding()
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.Settings,
                    contentDescription = null,
                    tint = LabAccentAmber,
                    modifier = Modifier.size(24.dp)
                )
                Text(
                    text = "ENGINE CONFIG",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = LabAccentAmber,
                    letterSpacing = 1.sp
                )
            }

            HorizontalDivider(color = Color.White.copy(alpha = 0.08f))

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(LabCardBg, RoundedCornerShape(12.dp))
                    .padding(16.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = if (isRecording) "ACTIVE" else "STANDBY",
                        color = if (isRecording) LabAccentGreen else LabTextSecondary,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        text = "${String.format("%.1f", sampleRate / 1000f)} kHz rate",
                        color = LabTextSecondary,
                        fontSize = 12.sp
                    )
                }
                Button(
                    onClick = onToggleRecording,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (isRecording) LabAccentPink else LabAccentAmber,
                        contentColor = if (isRecording) Color.White else LabButtonText
                    ),
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Text(if (isRecording) "STOP" else "START", fontWeight = FontWeight.Bold)
                }
            }

            ControlsBoard(
                sampleRate = sampleRate,
                step = step,
                a4Reference = a4Reference,
                scrollSpeed = scrollSpeed,
                useMultithreading = useMultithreading,
                onSampleRateChange = onSampleRateChange,
                onStepChange = onStepChange,
                onA4Change = onA4Change,
                onScrollSpeedChange = onScrollSpeedChange,
                onUseMultithreadingChange = onUseMultithreadingChange
            )

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(LabCardBg, RoundedCornerShape(12.dp))
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "Performance Overlay",
                        color = LabTextPrimary,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        text = "Show live engine state over the active screen.",
                        color = LabTextSecondary,
                        fontSize = 11.sp
                    )
                }
                Switch(
                    checked = showPerformanceOverlay,
                    onCheckedChange = onPerformanceOverlayChange,
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = LabButtonText,
                        checkedTrackColor = LabAccentAmber,
                        uncheckedThumbColor = LabTextSecondary,
                        uncheckedTrackColor = LabSurface
                    )
                )
            }
        }
    }
}

@Composable
private fun PerformanceSummaryOverlay(
    window: Window,
    modifier: Modifier = Modifier
) {
    val snapshot by rememberPerformanceSnapshot(window)
    Card(
        modifier = modifier
            .width(220.dp)
            .border(1.dp, Color.White.copy(alpha = 0.12f), RoundedCornerShape(12.dp)),
        colors = CardDefaults.cardColors(containerColor = Color.Black.copy(alpha = 0.82f)),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(5.dp)
        ) {
            Text(
                text = "PERFORMANCE HUD",
                color = LabAccentAmber,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp
            )
            fun metric(value: Double?, suffix: String = "", approximate: Boolean = false) =
                value?.let { (if (approximate) "≈" else "") + "%.1f".format(it) + suffix } ?: "—"
            val rows = listOf(
                "UI FPS" to metric(snapshot.fps, approximate = snapshot.approximateFrames),
                "Slow frames" to metric(snapshot.slowPercent, "%",
                    snapshot.approximateFrames || snapshot.estimatedSlowFrames),
                "App CPU" to metric(snapshot.cpuPercent, "%"),
                "Memory (PSS)" to metric(snapshot.memoryMiB, " MiB")
            )
            rows.forEach { (label, measurement) ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(label, modifier = Modifier.weight(1f), color = LabTextSecondary, fontSize = 11.sp)
                    Text(
                        text = measurement,
                        color = LabTextPrimary, fontFamily = FontFamily.Monospace, fontSize = 11.sp
                    )
                }
            }
            Text("CPU: 100% = all cores", color = LabTextSecondary, fontSize = 10.sp)
            if (snapshot.approximateFrames || snapshot.estimatedSlowFrames) {
                Text("≈ estimated frame statistics", color = LabTextSecondary, fontSize = 10.sp)
            }
        }
    }
}

@Composable
private fun NoiseCalibrationOverlay(
    progress: Float,
    modifier: Modifier = Modifier
) {
    val safeProgress = progress.coerceIn(0f, 1f)
    Card(
        modifier = modifier
            .width(270.dp)
            .border(1.dp, LabAccentAmber.copy(alpha = 0.5f), RoundedCornerShape(16.dp)),
        colors = CardDefaults.cardColors(containerColor = Color.Black.copy(alpha = 0.9f)),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(
            modifier = Modifier.padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = "CALIBRATING ROOM NOISE",
                color = LabAccentAmber,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.sp,
                textAlign = TextAlign.Center
            )
            Text(
                text = "Keep the room quiet for a few seconds.",
                color = LabTextPrimary,
                style = MaterialTheme.typography.bodySmall,
                textAlign = TextAlign.Center
            )
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(4.dp)
                    .background(Color.White.copy(alpha = 0.12f), RoundedCornerShape(50))
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth(safeProgress)
                        .fillMaxHeight()
                        .background(LabAccentAmber, RoundedCornerShape(50))
                )
            }
            Text(
                text = "${(safeProgress * 100f).toInt()}%",
                color = LabTextSecondary,
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp
            )
        }
    }
}

@Composable
private fun NoiseCancellationButton(
    state: NoiseCancellationState,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val tint = when (state) {
        NoiseCancellationState.OFF -> LabTextSecondary
        NoiseCancellationState.CALIBRATING -> LabAccentAmber
        NoiseCancellationState.ACTIVE -> LabAccentGreen
    }
    val description = when (state) {
        NoiseCancellationState.OFF -> "Calibrate room noise cancellation"
        NoiseCancellationState.CALIBRATING -> "Cancel room noise calibration"
        NoiseCancellationState.ACTIVE -> "Disable noise cancellation"
    }

    IconButton(
        onClick = onClick,
        modifier = modifier
            .size(24.dp)
            .semantics { contentDescription = description }
            .testTag("noise_cancellation_button")
    ) {
        Canvas(modifier = Modifier.size(18.dp)) {
            val heights = floatArrayOf(0.35f, 0.7f, 1f, 0.7f, 0.35f)
            heights.forEachIndexed { index, heightFraction ->
                val x = size.width * (index + 1f) / (heights.size + 1f)
                val halfHeight = size.height * heightFraction * 0.38f
                drawLine(
                    color = tint,
                    start = Offset(x, size.height / 2f - halfHeight),
                    end = Offset(x, size.height / 2f + halfHeight),
                    strokeWidth = 2.dp.toPx(),
                    cap = StrokeCap.Round
                )
            }
        }
    }
}

@Composable
private fun ScreenAwareTuningPanel(
    screenTitle: String,
    detectedNote: String,
    centsDeviation: Float,
    isRecording: Boolean,
    detectedFrequency: Float,
    detectedOctave: Int,
    noiseCancellationState: NoiseCancellationState,
    onToggleNoiseCancellation: () -> Unit,
    onOpenSettings: () -> Unit,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(LabSurface)
            .border(width = 1.dp, color = Color.White.copy(alpha = 0.05f))
            .navigationBarsPadding()
    ) {
        Column(
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(6.dp)
                            .background(
                                if (isRecording) LabAccentGreen else LabTextSecondary,
                                RoundedCornerShape(50)
                            )
                    )
                    Text(
                        text = screenTitle,
                        style = MaterialTheme.typography.labelSmall,
                        color = LabTextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 1.sp
                    )
                }

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    NoiseCancellationButton(
                        state = noiseCancellationState,
                        onClick = onToggleNoiseCancellation
                    )
                    IconButton(
                        onClick = onOpenSettings,
                        modifier = Modifier
                            .size(24.dp)
                            .testTag("settings_button_tuner")
                    ) {
                        Icon(
                            imageVector = Icons.Default.Settings,
                            contentDescription = "Open Settings",
                            tint = LabAccentAmber,
                            modifier = Modifier.size(18.dp)
                        )
                    }
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(24.dp),
                modifier = Modifier.padding(vertical = 4.dp)
            ) {
                Box(
                    modifier = Modifier.size(68.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Canvas(modifier = Modifier.fillMaxSize()) {
                        drawCircle(
                            color = if (detectedNote != "None") {
                                LabAccentGreen.copy(alpha = 0.15f)
                            } else {
                                LabAccentAmber.copy(alpha = 0.05f)
                            },
                            radius = size.minDimension / 2f,
                            style = Stroke(width = 1.5.dp.toPx())
                        )
                    }
                    Row(verticalAlignment = Alignment.Bottom) {
                        Text(
                            text = if (detectedNote == "None") "--" else detectedNote,
                            style = MaterialTheme.typography.headlineMedium,
                            fontWeight = FontWeight.Bold,
                            color = if (detectedNote == "None") LabTextSecondary else LabAccentGreen,
                            fontFamily = FontFamily.Monospace
                        )
                        if (detectedNote != "None" && detectedOctave >= 0) {
                            Text(
                                text = detectedOctave.toString(),
                                color = LabAccentGreen,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold,
                                fontSize = 12.sp,
                                modifier = Modifier.padding(bottom = 5.dp)
                            )
                        }
                    }
                }

                if (detectedNote != "None") {
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(
                            text = "${if (centsDeviation >= 0) "+" else ""}${String.format("%.1f", centsDeviation)} cents",
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            color = if (abs(centsDeviation) < 5f) LabAccentGreen else LabTextPrimary,
                            fontFamily = FontFamily.Monospace
                        )
                        Text(
                            text = "${String.format("%.1f", detectedFrequency)} Hz",
                            style = MaterialTheme.typography.bodySmall,
                            fontWeight = FontWeight.Bold,
                            color = LabAccentAmber,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                } else {
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(
                            text = "NO SIGNAL",
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            color = LabTextSecondary,
                            fontFamily = FontFamily.Monospace
                        )
                        Text(
                            text = "Play a sound to tune",
                            style = MaterialTheme.typography.labelSmall,
                            color = LabTextSecondary
                        )
                    }
                }
            }

            val animatedNeedle by animateFloatAsState(
                targetValue = if (detectedNote == "None") 0f else centsDeviation,
                animationSpec = spring(dampingRatio = 0.7f, stiffness = 300f),
                label = "TuningNeedle"
            )

            Canvas(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(20.dp)
            ) {
                val centerY = size.height / 2f
                drawLine(
                    color = Color.White.copy(alpha = 0.05f),
                    start = Offset(0f, centerY),
                    end = Offset(size.width, centerY),
                    strokeWidth = 2.dp.toPx()
                )
                for (i in 0..10) {
                    val x = size.width * i / 10f
                    val isCenter = i == 5
                    val tickHeight = if (isCenter) 12.dp.toPx() else 6.dp.toPx()
                    drawLine(
                        color = if (isCenter) LabAccentGreen else Color.White.copy(alpha = 0.15f),
                        start = Offset(x, centerY - tickHeight / 2f),
                        end = Offset(x, centerY + tickHeight / 2f),
                        strokeWidth = (if (isCenter) 2.dp else 1.dp).toPx()
                    )
                }
                val needleX = size.width * ((animatedNeedle + 50f) / 100f).coerceIn(0f, 1f)
                val needleColor = if (abs(animatedNeedle) < 5f) LabAccentGreen else LabAccentPink
                drawCircle(needleColor, 4.dp.toPx(), Offset(needleX, centerY))
                drawLine(
                    color = needleColor,
                    start = Offset(needleX, 2f),
                    end = Offset(needleX, size.height - 2f),
                    strokeWidth = 1.5.dp.toPx()
                )
            }
        }
    }
}
