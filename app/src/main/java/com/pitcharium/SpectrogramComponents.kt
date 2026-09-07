package com.pitcharium

import androidx.compose.ui.platform.testTag

import android.graphics.BitmapShader
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Shader
import androidx.compose.animation.core.*
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.isActive
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.toArgb
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.sin

// Custom Palette for Elegant Dark design theme
val LabBackground = Color(0xFF0A0B0C) // Pure Elegant Dark background
val LabSurface = Color(0xFF151619) // Main canvas container background
val LabCardBg = Color(0xFF1E2024) // Metric card background / status components (slightly lighter for contrast)
val LabBottomPanel = Color(0xFF121316) // Controls container background
val LabAccentAmber = Color(0xFFD1E4FF) // Ice-blue elegant accent color
val LabButtonText = Color(0xFF003258) // Text/icon color on active accent background
val LabAccentGreen = Color(0xFF34D399) // Emerald-400 equivalent for pulsing
val LabAccentBlue = Color(0xFFD1E4FF) // Matching active selector
val LabAccentPink = Color(0xFFF43F5E) // Matching active delete/refresh/stop state
val LabGridDashed = Color(0x0DFFFFFF) // White/5 border equivalent
val LabTextPrimary = Color(0xFFE2E2E6) // Bright text for dark theme
val LabTextSecondary = Color(0xFF8E9299) // Soft muted labels

private val fullPaletteColors: List<Color> by lazy {
    val baseColors = arrayOf(
        floatArrayOf(178f, 255f, 102f), // 0
        floatArrayOf(102f, 255f, 102f), // 1
        floatArrayOf(102f, 255f, 178f), // 2
        floatArrayOf(102f, 255f, 255f), // 3
        floatArrayOf(102f, 179f, 255f), // 4
        floatArrayOf(102f, 102f, 255f), // 5
        floatArrayOf(178f, 102f, 255f), // 6
        floatArrayOf(255f, 102f, 255f), // 7
        floatArrayOf(255f, 102f, 178f), // 8
        floatArrayOf(255f, 102f, 102f), // 9
        floatArrayOf(255f, 178f, 102f), // 10
        floatArrayOf(255f, 255f, 102f)  // 11
    )

    List(192) { row ->
        var phase = row.toFloat() / 16.0f - 0.5f
        while (phase < 0.0f) phase += 12.0f
        while (phase >= 12.0f) phase -= 12.0f
        val base = kotlin.math.floor(phase.toDouble()).toInt()
        val next = (base + 1) % 12
        val fraction = phase - base

        val r = baseColors[base][0] + 1.0f + fraction * (baseColors[next][0] - baseColors[base][0])
        val g = baseColors[base][1] + 1.0f + fraction * (baseColors[next][1] - baseColors[base][1])
        val b = baseColors[base][2] + 1.0f + fraction * (baseColors[next][2] - baseColors[base][2])

        Color(
            red = (r / 255.0f).coerceIn(0f, 1f),
            green = (g / 255.0f).coerceIn(0f, 1f),
            blue = (b / 255.0f).coerceIn(0f, 1f),
            alpha = 1.0f
        )
    }
}

@Composable
fun SpectrogramVisualizer(
    viewModel: SpectrogramViewModel,
    isRecording: Boolean,
    detectedNote: String,
    scrollSpeed: Float,
    modifier: Modifier = Modifier
) {
    val presenter = viewModel.presenter
    val shader = remember { BitmapShader(presenter.bitmap, Shader.TileMode.REPEAT, Shader.TileMode.CLAMP) }
    val shaderMatrix = remember { Matrix() }
    val shaderPaint = remember {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            isFilterBitmap = true
            this.shader = shader
        }
    }
    var sourceStart by remember { mutableFloatStateOf(presenter.sourceStart) }
    var redraw by remember { mutableIntStateOf(0) }

    LaunchedEffect(scrollSpeed) {
        sourceStart = presenter.sourceStart
        redraw++
    }

    LaunchedEffect(isRecording) {
        while (isActive && isRecording) {
            withFrameNanos { frameNanos ->
                if (presenter.advance(frameNanos)) {
                    sourceStart = presenter.sourceStart
                    redraw++
                }
            }
        }
        sourceStart = presenter.sourceStart
        redraw++
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(LabSurface)
    ) {
        // Draw Spectrogram Canvas - NO Rounded Corner Shape, NO border
        Canvas(
            modifier = Modifier.fillMaxSize()
        ) {
            redraw
            val scaleX = size.width / presenter.visibleColumns
            val scaleY = size.height / 193f
            shaderMatrix.reset()
            shaderMatrix.setScale(scaleX, -scaleY)
            shaderMatrix.postTranslate(-sourceStart * scaleX, size.height)
            shader.setLocalMatrix(shaderMatrix)
            drawIntoCanvas { canvas ->
                canvas.nativeCanvas.drawRect(0f, 0f, size.width, size.height, shaderPaint)
            }

            // Draw horizontal chromatic grid lines centered with the physical space on the y axis where the notes are
            val rowHeight = size.height / 12f

            for (i in 0..11) {
                val y = rowHeight * (i + 0.5f)
                drawLine(
                        color = Color.Black.copy(alpha = 0.8f),
                        start = Offset(0f, y),
                        end = Offset(size.width, y),
                        strokeWidth = 3.dp.toPx()
                    )
                    drawLine(
                        color = Color.White.copy(alpha = 0.75f),
                    start = Offset(0f, y),
                    end = Offset(size.width, y),
                    strokeWidth = 1.dp.toPx()
                )
            }
        }

        // Note Name overlay sidebar on the Left side matching the HTML style
        Column(
            modifier = Modifier
                .fillMaxHeight()
                .width(36.dp)
                .align(Alignment.CenterStart)
                    .background(Color.Black.copy(alpha = 0.85f)),
            verticalArrangement = Arrangement.SpaceAround,
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Render from top down (B down to C)
            val reversedNotes = viewModel.noteNames.reversed()
            reversedNotes.forEach { note ->
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.Center
                ) {
                    val isActive = note == detectedNote
                    Text(
                        text = note,
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = if (isActive) FontWeight.ExtraBold else FontWeight.Bold,
                            color = if (isActive) LabAccentAmber else Color.White,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp
                    )
                }
            }
        }
    }
}

internal fun smoothRadialBrightness(current: Float, target: Float, deltaSeconds: Float): Float {
    val difference = target - current
    val responseSeconds = when {
        abs(difference) > 0.35f -> 0.035f
        difference >= 0f -> 0.05f
        else -> 0.11f
    }
    val amount = 1f - exp(-deltaSeconds / responseSeconds)
    return current + difference * amount
}

private fun colorBrightness(color: Int): Float =
    maxOf((color shr 16) and 0xFF, (color shr 8) and 0xFF, color and 0xFF) / 255f

@Composable
fun RadialVisualizer(
    viewModel: SpectrogramViewModel,
    isRecording: Boolean,
    detectedNote: String,
    centsDeviation: Float,
    detectedOctave: Int,
    modifier: Modifier = Modifier
) {
    val presenter = viewModel.presenter
    var redraw by remember { mutableIntStateOf(0) }
    val displayedBrightness = remember { FloatArray(192) }

    LaunchedEffect(isRecording) {
        var lastFrameNanos = 0L
        var initialized = false
        while (isActive && isRecording) {
            withFrameNanos { frameNanos ->
                presenter.advance(frameNanos)
                val deltaSeconds = if (lastFrameNanos == 0L) 0f else {
                    ((frameNanos - lastFrameNanos) / 1_000_000_000f).coerceAtMost(0.05f)
                }
                lastFrameNanos = frameNanos

                for (i in displayedBrightness.indices) {
                    val target = colorBrightness(presenter.latestColumn[(i - 1 + displayedBrightness.size) % displayedBrightness.size]) * 0.25f +
                        colorBrightness(presenter.latestColumn[i]) * 0.5f +
                        colorBrightness(presenter.latestColumn[(i + 1) % displayedBrightness.size]) * 0.25f
                    displayedBrightness[i] = if (initialized) {
                        smoothRadialBrightness(displayedBrightness[i], target, deltaSeconds)
                    } else {
                        target
                    }
                }
                initialized = true
                redraw++
            }
        }
        redraw++
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(LabSurface),
        contentAlignment = Alignment.Center
    ) {
        val reusablePath = remember { Path() }

        Canvas(
            modifier = Modifier.fillMaxSize()
        ) {
            redraw // Trigger recomposition redraw on frame update

            val cx = size.width / 2f
            val cy = size.height / 2f
            val minDim = minOf(size.width, size.height)
            val innerRadius = minDim * 0.22f
            val maxLineLength = minDim * 0.12f

            val N = displayedBrightness.size

            // Generate loop points on outer boundary with a minimum 10% extension
            val points = Array(N) { i ->
                val brightness = displayedBrightness[i]
                val radius = innerRadius + maxLineLength * (0.10f + brightness * 0.90f)
                val angleRad = (i * 2 * Math.PI / N - Math.PI / 2).toFloat()
                Offset(cx + radius * cos(angleRad), cy + radius * sin(angleRad))
            }

            // Interpolate a smooth curve through the points
            reusablePath.reset()
            reusablePath.moveTo(points[0].x, points[0].y)
            for (i in 0 until N) {
                val p1 = points[i]
                val p2 = points[(i + 1) % N]
                val p0 = points[(i - 1 + N) % N]
                val p3 = points[(i + 2) % N]

                // Catmull-Rom spline to cubic Bezier curve conversion
                val cp1X = p1.x + (p2.x - p0.x) * 0.15f
                val cp1Y = p1.y + (p2.y - p0.y) * 0.15f
                val cp2X = p2.x - (p3.x - p1.x) * 0.15f
                val cp2Y = p2.y - (p3.y - p1.y) * 0.15f

                reusablePath.cubicTo(cp1X, cp1Y, cp2X, cp2Y, p2.x, p2.y)
            }
            reusablePath.close()

            // Shift colors by 48 bins (90 degrees) to align with starting top offset (-PI/2)
            val colors = List(N) { i ->
                fullPaletteColors[(i + 48) % N]
            }
            val sweepBrush = Brush.sweepGradient(colors = colors, center = Offset(cx, cy))

            // Draw area under the curve (towards the circle)
            drawPath(
                path = reusablePath,
                brush = sweepBrush
            )

            // Draw subtle spoke reference lines for each of the 12 semitones
            for (s in 0 until 12) {
                val bin = 8 + s * 16
                val angleRad = (bin * 2 * Math.PI / N - Math.PI / 2).toFloat()
                val startX = cx + innerRadius * cos(angleRad)
                val startY = cy + innerRadius * sin(angleRad)
                val endX = cx + (innerRadius + maxLineLength) * cos(angleRad)
                val endY = cy + (innerRadius + maxLineLength) * sin(angleRad)

                drawLine(
                    color = Color.White.copy(alpha = 0.08f),
                    start = Offset(startX, startY),
                    end = Offset(endX, endY),
                    strokeWidth = 1.dp.toPx()
                )
            }

            // Outline with a fine stroke for crisp boundaries
            drawPath(
                path = reusablePath,
                brush = sweepBrush,
                style = Stroke(width = 1.5.dp.toPx())
            )

            // Draw clean background circular mask for the inner circle
            drawCircle(
                color = LabBackground,
                radius = innerRadius
            )

            // Draw dynamic active tuner border ring
            val borderColor = if (detectedNote != "None" && Math.abs(centsDeviation) < 5f) {
                LabAccentGreen
            } else {
                LabAccentAmber.copy(alpha = 0.25f)
            }
            drawCircle(
                color = borderColor,
                radius = innerRadius,
                style = Stroke(width = 2.dp.toPx())
            )

            // Draw clean outer circular gauge ring as background for notes
            drawCircle(
                color = Color.White.copy(alpha = 0.05f),
                radius = innerRadius + maxLineLength,
                style = Stroke(width = 1.dp.toPx())
            )

            // Draw elegant chromatic note labels around the outer ring
            val radiusText = innerRadius + maxLineLength + 16.dp.toPx()
            val textPaint = android.graphics.Paint().apply {
                isAntiAlias = true
                textAlign = android.graphics.Paint.Align.CENTER
                typeface = android.graphics.Typeface.create("sans-serif-condensed", android.graphics.Typeface.BOLD)
            }

            for (s in 0 until 12) {
                val noteName = viewModel.noteNames[s]
                val isActive = noteName == detectedNote

                textPaint.textSize = if (isActive) 15.sp.toPx() else 11.sp.toPx()
                textPaint.color = if (isActive) LabAccentGreen.toArgb() else LabTextSecondary.copy(alpha = 0.4f).toArgb()

                val angleRad = (s * Math.PI / 6 - 5 * Math.PI / 12).toFloat()
                val tx = cx + radiusText * cos(angleRad)
                val ty = cy + radiusText * sin(angleRad) - ((textPaint.descent() + textPaint.ascent()) / 2)

                drawIntoCanvas { canvas ->
                    canvas.nativeCanvas.drawText(noteName, tx, ty, textPaint)
                }
            }
        }

        // Center overlay content containing elegant tuner typography
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Row(
                verticalAlignment = Alignment.Bottom,
                horizontalArrangement = Arrangement.Center
            ) {
                Text(
                    text = if (detectedNote == "None") "--" else detectedNote,
                    style = MaterialTheme.typography.headlineLarge,
                    fontWeight = FontWeight.ExtraBold,
                    color = if (detectedNote == "None") LabTextSecondary else LabAccentGreen,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 44.sp
                )
                if (detectedNote != "None" && detectedOctave >= 0) {
                    Text(
                        text = detectedOctave.toString(),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                        color = LabAccentGreen,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.padding(bottom = 6.dp, start = 2.dp)
                    )
                }
            }

            Spacer(modifier = Modifier.height(4.dp))

            if (detectedNote != "None") {
                Text(
                    text = "${if (centsDeviation >= 0) "+" else ""}${String.format("%.1f", centsDeviation)} c",
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Bold,
                    color = if (Math.abs(centsDeviation) < 5f) LabAccentGreen else LabTextPrimary,
                    fontFamily = FontFamily.Monospace
                )
            } else {
                Text(
                    text = "STANDBY",
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                    color = LabTextSecondary,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}

@Composable
fun ControlsBoard(
    sampleRate: Int,
    step: Int,
    a4Reference: Int,
    scrollSpeed: Float,
    useMultithreading: Boolean,
    onSampleRateChange: (Int) -> Unit,
    onStepChange: (Int) -> Unit,
    onA4Change: (Int) -> Unit,
    onScrollSpeedChange: (Float) -> Unit,
    onUseMultithreadingChange: (Boolean) -> Unit,
) {
    var sampleRateExpanded by remember { mutableStateOf(false) }
    var stepExpanded by remember { mutableStateOf(false) }
    var speedExpanded by remember { mutableStateOf(false) }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, Color.White.copy(alpha = 0.05f), RoundedCornerShape(24.dp)),
        colors = CardDefaults.cardColors(containerColor = LabBottomPanel),
        shape = RoundedCornerShape(24.dp)
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Text(
                text = "DSP ENGINE OPTIMIZATION",
                style = MaterialTheme.typography.labelSmall,
                color = LabTextSecondary,
                fontFamily = FontFamily.Monospace,
                letterSpacing = 1.sp
            )

            // Pipelined Multithreading Option
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    text = "PIPELINED MULTITHREADING",
                    fontSize = 10.sp,
                    color = LabTextSecondary,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(LabCardBg)
                        .border(1.dp, Color.White.copy(alpha = 0.15f), RoundedCornerShape(8.dp))
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column(modifier = Modifier.weight(1f).padding(end = 8.dp)) {
                        Text(
                            text = "Multi-core Pipelining",
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = LabTextPrimary,
                            fontFamily = FontFamily.Monospace
                        )
                        Text(
                            text = "Parallelizes Gaussian FFT, Derivative FFT & rendering across CPU cores for zero UI latency",
                            fontSize = 10.sp,
                            color = LabTextSecondary,
                            lineHeight = 13.sp
                        )
                    }
                    Switch(
                        checked = useMultithreading,
                        onCheckedChange = onUseMultithreadingChange,
                        modifier = Modifier.testTag("multithreading_switch"),
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = LabButtonText,
                            checkedTrackColor = LabAccentAmber,
                            uncheckedThumbColor = LabTextSecondary,
                            uncheckedTrackColor = LabSurface
                        )
                    )
                }
            }

            // Configuration Options (Sample Rate and Overlap Step Side by Side)
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                // Sample Rate Selector
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        text = "SAMPLE RATE",
                        fontSize = 10.sp,
                        color = LabTextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    )

                    Box(modifier = Modifier.fillMaxWidth()) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(44.dp)
                                .clip(RoundedCornerShape(8.dp))
                                .background(LabCardBg)
                                .border(
                                    width = 1.dp,
                                    color = Color.White.copy(alpha = 0.15f),
                                    shape = RoundedCornerShape(8.dp)
                                )
                                .clickable { sampleRateExpanded = true },
                            contentAlignment = Alignment.Center
                        ) {
                            Row(
                                modifier = Modifier.padding(horizontal = 12.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text(
                                    text = when (sampleRate) {
                                        8000 -> "8.0 kHz"
                                        11025 -> "11.02 kHz"
                                        22050 -> "22.05 kHz"
                                        else -> "44.10 kHz"
                                    },
                                    style = MaterialTheme.typography.labelMedium,
                                    fontWeight = FontWeight.SemiBold,
                                    color = LabTextPrimary,
                                    fontFamily = FontFamily.Monospace,
                                    modifier = Modifier.weight(1f)
                                )
                                Text(
                                    text = "v",
                                    color = LabAccentAmber,
                                    fontSize = 12.sp
                                )
                            }
                        }

                        DropdownMenu(
                            expanded = sampleRateExpanded,
                            onDismissRequest = { sampleRateExpanded = false },
                            modifier = Modifier
                                .background(LabCardBg)
                                .border(1.dp, Color.White.copy(alpha = 0.1f), RoundedCornerShape(8.dp))
                        ) {
                            val rates = listOf(8000, 11025, 22050, 44100)
                            rates.forEach { rate ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            text = when (rate) {
                                                8000 -> "8.0 kHz"
                                                11025 -> "11.02 kHz"
                                                22050 -> "22.05 kHz"
                                                else -> "44.10 kHz"
                                            },
                                            fontFamily = FontFamily.Monospace,
                                            color = if (rate == sampleRate) LabAccentAmber else LabTextPrimary
                                        )
                                    },
                                    onClick = {
                                        onSampleRateChange(rate)
                                        sampleRateExpanded = false
                                    }
                                )
                            }
                        }
                    }
                }

                // Step Overlap Selector
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        text = "OVERLAP STEP",
                        fontSize = 10.sp,
                        color = LabTextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    )

                    Box(modifier = Modifier.fillMaxWidth()) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(44.dp)
                                .clip(RoundedCornerShape(8.dp))
                                .background(LabCardBg)
                                .border(
                                    width = 1.dp,
                                    color = Color.White.copy(alpha = 0.15f),
                                    shape = RoundedCornerShape(8.dp)
                                )
                                .clickable { stepExpanded = true },
                            contentAlignment = Alignment.Center
                        ) {
                            Row(
                                modifier = Modifier.padding(horizontal = 12.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text(
                                    text = when (step) {
                                        4 -> "4x (75%)"
                                        8 -> "8x (87.5%)"
                                        else -> "16x (93.7%)"
                                    },
                                    style = MaterialTheme.typography.labelMedium,
                                    fontWeight = FontWeight.SemiBold,
                                    color = LabTextPrimary,
                                    fontFamily = FontFamily.Monospace,
                                    modifier = Modifier.weight(1f)
                                )
                                Text(
                                    text = "v",
                                    color = LabAccentAmber,
                                    fontSize = 12.sp
                                )
                            }
                        }

                        DropdownMenu(
                            expanded = stepExpanded,
                            onDismissRequest = { stepExpanded = false },
                            modifier = Modifier
                                .background(LabCardBg)
                                .border(1.dp, Color.White.copy(alpha = 0.1f), RoundedCornerShape(8.dp))
                        ) {
                            val steps = listOf(4, 8, 16)
                            steps.forEach { item ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            text = when (item) {
                                                4 -> "4x (75%)"
                                                8 -> "8x (87.5%)"
                                                else -> "16x (93.7%)"
                                            },
                                            fontFamily = FontFamily.Monospace,
                                            color = if (item == step) LabAccentAmber else LabTextPrimary
                                        )
                                    },
                                    onClick = {
                                        onStepChange(item)
                                        stepExpanded = false
                                    }
                                )
                            }
                        }
                    }
                }
            }

            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    text = "SCROLL SPEED",
                    fontSize = 10.sp,
                    color = LabTextSecondary,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )
                Box(modifier = Modifier.fillMaxWidth()) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(44.dp)
                            .clip(RoundedCornerShape(8.dp))
                            .background(LabCardBg)
                            .border(1.dp, Color.White.copy(alpha = 0.15f), RoundedCornerShape(8.dp))
                            .clickable { speedExpanded = true },
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = "${scrollSpeed}x",
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.SemiBold,
                            color = LabTextPrimary
                        )
                    }
                    DropdownMenu(
                        expanded = speedExpanded,
                        onDismissRequest = { speedExpanded = false },
                        modifier = Modifier.background(LabCardBg)
                    ) {
                        listOf(0.5f, 1f, 2f).forEach { item ->
                            DropdownMenuItem(
                                text = {
                                    Text(
                                        text = "${item}x",
                                        fontFamily = FontFamily.Monospace,
                                        color = if (item == scrollSpeed) LabAccentAmber else LabTextPrimary
                                    )
                                },
                                onClick = {
                                    onScrollSpeedChange(item)
                                    speedExpanded = false
                                }
                            )
                        }
                    }
                }
            }

            // Input field for A4 Calibration with custom design
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = "A4 TUNING FREQUENCY (Hz)",
                    style = MaterialTheme.typography.labelSmall,
                    color = LabTextSecondary,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )

                var textValue by remember(a4Reference) { mutableStateOf(a4Reference.toString()) }

                OutlinedTextField(
                    value = textValue,
                    onValueChange = { newValue ->
                        val filtered = newValue.filter { it.isDigit() }
                        if (filtered.length <= 4) {
                            textValue = filtered
                            val parsed = filtered.toIntOrNull()
                            if (parsed != null && parsed in 400..500) {
                                onA4Change(parsed)
                            }
                        }
                    },
                    textStyle = MaterialTheme.typography.bodyLarge.copy(
                        fontFamily = FontFamily.Monospace,
                        color = LabTextPrimary
                    ),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = LabAccentAmber,
                        unfocusedBorderColor = Color.White.copy(alpha = 0.15f),
                        cursorColor = LabAccentAmber,
                        focusedContainerColor = LabCardBg,
                        unfocusedContainerColor = LabCardBg
                    ),
                    shape = RoundedCornerShape(12.dp),
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    placeholder = {
                        Text(
                            text = "440",
                            color = LabTextSecondary.copy(alpha = 0.5f),
                            fontFamily = FontFamily.Monospace
                        )
                    }
                )

                Text(
                    text = "Enter calibration frequency (typical range: 415 - 466 Hz).",
                    fontSize = 11.sp,
                    color = LabTextSecondary,
                    lineHeight = 15.sp
                )
            }
        }
    }
}
