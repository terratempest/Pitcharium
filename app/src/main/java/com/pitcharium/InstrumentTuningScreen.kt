package com.pitcharium

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.isActive
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.sin

internal data class InstrumentStringTarget(
    val noteName: String,
    val octave: Int
) {
    val label: String get() = "$noteName$octave"
}

internal data class InstrumentTuning(
    val name: String,
    val strings: List<InstrumentStringTarget>
)

internal data class InstrumentDefinition(
    val name: String,
    val tunings: List<InstrumentTuning>
)

private object InstrumentCatalog {
    val instruments: List<InstrumentDefinition> = listOf(
        InstrumentDefinition(
            "Guitar (6-string)",
            listOf(
                tuning("Standard", "E2", "A2", "D3", "G3", "B3", "E4"),
                tuning("Half Step Down", "Eb2", "Ab2", "Db3", "Gb3", "Bb3", "Eb4"),
                tuning("Whole Step Down", "D2", "G2", "C3", "F3", "A3", "D4"),
                tuning("Drop D", "D2", "A2", "D3", "G3", "B3", "E4"),
                tuning("Double Drop D", "D2", "A2", "D3", "G3", "B3", "D4"),
                tuning("DADGAD", "D2", "A2", "D3", "G3", "A3", "D4"),
                tuning("Open D", "D2", "A2", "D3", "F#3", "A3", "D4"),
                tuning("Open E", "E2", "B2", "E3", "G#3", "B3", "E4"),
                tuning("Open G", "D2", "G2", "D3", "G3", "B3", "D4"),
                tuning("Open A", "E2", "A2", "E3", "A3", "C#4", "E4"),
                tuning("C Standard", "C2", "F2", "Bb2", "Eb3", "G3", "C4")
            )
        ),
        InstrumentDefinition(
            "Guitar (7-string)",
            listOf(
                tuning("B Standard", "B1", "E2", "A2", "D3", "G3", "B3", "E4"),
                tuning("Drop A", "A1", "E2", "A2", "D3", "G3", "B3", "E4"),
                tuning("A Standard", "A1", "D2", "G2", "C3", "F3", "A3", "D4")
            )
        ),
        InstrumentDefinition(
            "Guitar (8-string)",
            listOf(
                tuning("F# Standard", "F#1", "B1", "E2", "A2", "D3", "G3", "B3", "E4"),
                tuning("Drop E", "E1", "B1", "E2", "A2", "D3", "G3", "B3", "E4")
            )
        ),
        InstrumentDefinition(
            "Guitar (12-string)",
            listOf(
                tuning(
                    "Standard",
                    "E2", "E3", "A2", "A3", "D3", "D4",
                    "G3", "G4", "B3", "B3", "E4", "E4"
                ),
                tuning(
                    "Half Step Down",
                    "Eb2", "Eb3", "Ab2", "Ab3", "Db3", "Db4",
                    "Gb3", "Gb4", "Bb3", "Bb3", "Eb4", "Eb4"
                )
            )
        ),
        InstrumentDefinition(
            "Bass (4-string)",
            listOf(
                tuning("Standard", "E1", "A1", "D2", "G2"),
                tuning("Drop D", "D1", "A1", "D2", "G2"),
                tuning("Half Step Down", "Eb1", "Ab1", "Db2", "Gb2"),
                tuning("D Standard", "D1", "G1", "C2", "F2"),
                tuning("BEAD", "B0", "E1", "A1", "D2")
            )
        ),
        InstrumentDefinition(
            "Bass (5-string)",
            listOf(
                tuning("B Standard", "B0", "E1", "A1", "D2", "G2"),
                tuning("Drop A", "A0", "E1", "A1", "D2", "G2"),
                tuning("High C", "E1", "A1", "D2", "G2", "C3")
            )
        ),
        InstrumentDefinition(
            "Bass (6-string)",
            listOf(
                tuning("B Standard", "B0", "E1", "A1", "D2", "G2", "C3"),
                tuning("F# Standard", "F#0", "B0", "E1", "A1", "D2", "G2")
            )
        ),
        InstrumentDefinition(
            "Ukulele",
            listOf(
                tuning("Standard High G", "G4", "C4", "E4", "A4"),
                tuning("Standard Low G", "G3", "C4", "E4", "A4"),
                tuning("D Tuning", "A4", "D4", "F#4", "B4")
            )
        ),
        InstrumentDefinition(
            "Baritone Ukulele",
            listOf(
                tuning("Standard", "D3", "G3", "B3", "E4"),
                tuning("High D", "D4", "G3", "B3", "E4")
            )
        ),
        InstrumentDefinition(
            "Mandolin",
            listOf(
                tuning("Standard", "G3", "G3", "D4", "D4", "A4", "A4", "E5", "E5"),
                tuning("Cajun", "F3", "F3", "C4", "C4", "G4", "G4", "D5", "D5")
            )
        ),
        InstrumentDefinition(
            "Banjo (5-string)",
            listOf(
                tuning("Open G", "G4", "D3", "G3", "B3", "D4"),
                tuning("Double C", "G4", "C3", "G3", "C4", "D4"),
                tuning("Sawmill", "G4", "D3", "G3", "C4", "D4")
            )
        ),
        InstrumentDefinition(
            "Violin",
            listOf(
                tuning("Standard", "G3", "D4", "A4", "E5"),
                tuning("Cajun", "F3", "C4", "G4", "D5")
            )
        ),
        InstrumentDefinition(
            "Viola",
            listOf(tuning("Standard", "C3", "G3", "D4", "A4"))
        ),
        InstrumentDefinition(
            "Cello",
            listOf(
                tuning("Standard", "C2", "G2", "D3", "A3"),
                tuning("Low B", "B1", "G2", "D3", "A3")
            )
        ),
        InstrumentDefinition(
            "Double Bass",
            listOf(
                tuning("Orchestral", "E1", "A1", "D2", "G2"),
                tuning("Solo", "F#1", "B1", "E2", "A2"),
                tuning("Low C Extension", "C1", "A1", "D2", "G2")
            )
        )
    )

    private fun tuning(name: String, vararg notes: String): InstrumentTuning =
        InstrumentTuning(name, notes.map(::parseTarget))

    private fun parseTarget(value: String): InstrumentStringTarget {
        val octaveStart = value.indexOfFirst { it.isDigit() || it == '-' }
        require(octaveStart > 0) { "Invalid note target: $value" }
        return InstrumentStringTarget(
            noteName = value.substring(0, octaveStart),
            octave = value.substring(octaveStart).toInt()
        )
    }
}

@Composable
internal fun InstrumentTuningScreen(
    viewModel: SpectrogramViewModel,
    detectedFrequency: Float,
    signalDb: Float,
    a4Reference: Int,
    isRecording: Boolean,
    modifier: Modifier = Modifier
) {
    var instrumentIndex by rememberSaveable { mutableStateOf(0) }
    var tuningIndex by rememberSaveable { mutableStateOf(0) }
    var focusedStringIndex by rememberSaveable { mutableStateOf<Int?>(null) }

    val instrument = InstrumentCatalog.instruments[instrumentIndex]
    val tuning = instrument.tunings[tuningIndex.coerceIn(instrument.tunings.indices)]
    val displayedStrings = remember(tuning) {
        tuning.strings.withIndex().toList().asReversed()
    }
    val differences = remember(detectedFrequency, a4Reference, tuning) {
        tuning.strings.map { target ->
            centsDifference(detectedFrequency, target, a4Reference)
        }
    }
    val automaticStringIndex = differences
        .withIndex()
        .filter { it.value != null }
        .minByOrNull { abs(it.value ?: Float.MAX_VALUE) }
        ?.takeIf { abs(it.value ?: Float.MAX_VALUE) <= 150f }
        ?.index
    val activeStringIndex = focusedStringIndex ?: automaticStringIndex

    val transition = rememberInfiniteTransition(label = "StringMotion")
    val phase by transition.animateFloat(
        initialValue = 0f,
        targetValue = (2f * PI).toFloat(),
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 520, easing = LinearEasing),
            repeatMode = RepeatMode.Restart
        ),
        label = "StringMotionPhase"
    )

    LaunchedEffect(isRecording) {
        while (isActive && isRecording) {
            withFrameNanos { frameNanos ->
                viewModel.presenter.advance(frameNanos)
            }
        }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(LabSurface)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            TuningSelector(
                label = "INSTRUMENT",
                selectedValue = instrument.name,
                values = InstrumentCatalog.instruments.map { it.name },
                modifier = Modifier.weight(1f),
                onSelected = { selected ->
                    instrumentIndex = selected
                    tuningIndex = 0
                    focusedStringIndex = null
                }
            )
            TuningSelector(
                label = "TUNING",
                selectedValue = tuning.name,
                values = instrument.tunings.map { it.name },
                modifier = Modifier.weight(1f),
                onSelected = { selected ->
                    tuningIndex = selected
                    focusedStringIndex = null
                }
            )
        }

        Text(
            text = if (focusedStringIndex == null) {
                "Strings follow physical first-to-last order from top to bottom. Low pitch bends down; high pitch bends up."
            } else {
                val physicalStringNumber = tuning.strings.size - focusedStringIndex!!
                "Focused on physical string $physicalStringNumber. Tap it again to return to automatic selection."
            },
            color = LabTextSecondary,
            style = MaterialTheme.typography.labelSmall,
            lineHeight = 16.sp
        )

        LazyColumn(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            itemsIndexed(displayedStrings) { _, indexedTarget ->
                val originalIndex = indexedTarget.index
                val target = indexedTarget.value
                val difference = differences[originalIndex]
                PhysicalStringRow(
                    physicalStringNumber = tuning.strings.size - originalIndex,
                    target = target,
                    targetFrequency = targetFrequency(target, a4Reference),
                    centsDifference = difference,
                    isActive = originalIndex == activeStringIndex && difference != null,
                    isFocused = originalIndex == focusedStringIndex,
                    signalDb = signalDb,
                    phase = phase,
                    onClick = {
                        focusedStringIndex = if (focusedStringIndex == originalIndex) null else originalIndex
                    }
                )
            }
        }
    }
}

@Composable
private fun TuningSelector(
    label: String,
    selectedValue: String,
    values: List<String>,
    modifier: Modifier = Modifier,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }

    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(
            text = label,
            color = LabTextSecondary,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            fontSize = 10.sp
        )
        Box(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(46.dp)
                    .background(LabCardBg, RoundedCornerShape(10.dp))
                    .border(
                        1.dp,
                        Color.White.copy(alpha = 0.14f),
                        RoundedCornerShape(10.dp)
                    )
                    .clickable { expanded = true }
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = selectedValue,
                    color = LabTextPrimary,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f)
                )
                Text("v", color = LabAccentAmber, fontSize = 11.sp)
            }

            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
                modifier = Modifier.background(LabCardBg)
            ) {
                values.forEachIndexed { index, value ->
                    DropdownMenuItem(
                        text = {
                            Text(
                                text = value,
                                color = if (value == selectedValue) LabAccentAmber else LabTextPrimary
                            )
                        },
                        onClick = {
                            expanded = false
                            onSelected(index)
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun PhysicalStringRow(
    physicalStringNumber: Int,
    target: InstrumentStringTarget,
    targetFrequency: Float,
    centsDifference: Float?,
    isActive: Boolean,
    isFocused: Boolean,
    signalDb: Float,
    phase: Float,
    onClick: () -> Unit
) {
    val absoluteDifference = abs(centsDifference ?: Float.MAX_VALUE)
    val inTune = isActive && absoluteDifference <= 5f
    val close = isActive && absoluteDifference <= 25f
    val approaching = isActive && absoluteDifference <= 100f
    val rowColor = when {
        inTune -> LabAccentGreen
        close -> LabAccentAmber
        approaching -> LabTextPrimary
        else -> LabTextSecondary
    }

    val bendTarget = when {
        !isActive || centsDifference == null || !approaching || inTune -> 0f
        else -> {
            val approachStrength = ((100f - absoluteDifference) / 75f).coerceIn(0f, 1f)
            val settleStrength = ((absoluteDifference - 5f) / 20f).coerceIn(0f, 1f)
            val strength = minOf(approachStrength, settleStrength)
            if (centsDifference < 0f) strength else -strength
        }
    }
    val animatedBend by animateFloatAsState(
        targetValue = bendTarget,
        animationSpec = spring(dampingRatio = 0.72f, stiffness = 240f),
        label = "StringBend$physicalStringNumber"
    )
    val signalScale = ((signalDb + 60f) / 40f).coerceIn(0.25f, 1f)

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(66.dp)
            .background(
                color = if (isActive) rowColor.copy(alpha = 0.09f) else Color.Transparent,
                shape = RoundedCornerShape(12.dp)
            )
            .border(
                width = 1.dp,
                color = when {
                    isFocused -> LabAccentAmber.copy(alpha = 0.8f)
                    isActive -> rowColor.copy(alpha = 0.35f)
                    else -> Color.White.copy(alpha = 0.06f)
                },
                shape = RoundedCornerShape(12.dp)
            )
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Column(
            modifier = Modifier.width(56.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(
                text = target.label,
                color = rowColor,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                fontSize = 16.sp
            )
            Text(
                text = "STRING $physicalStringNumber",
                color = LabTextSecondary,
                fontFamily = FontFamily.Monospace,
                fontSize = 8.sp
            )
        }

        Canvas(
            modifier = Modifier
                .weight(1f)
                .height(40.dp)
        ) {
            val centerY = size.height / 2f
            val strokeWidth = if (inTune) 3.dp.toPx() else 2.dp.toPx()
            val maximumBend = 12.dp.toPx()
            val rippleAmplitude = if (isActive && !inTune) 0.65.dp.toPx() * signalScale else 0f
            val segments = 42

            drawLine(
                color = Color.White.copy(alpha = 0.05f),
                start = Offset(0f, centerY),
                end = Offset(size.width, centerY),
                strokeWidth = 1.dp.toPx()
            )

            var previous = Offset(0f, centerY)
            for (segment in 1..segments) {
                val fraction = segment / segments.toFloat()
                val x = size.width * fraction
                val envelope = sin(PI * fraction).toFloat()
                val bend = animatedBend * maximumBend * envelope
                val ripple = sin(phase + fraction * 6f * PI.toFloat()) * rippleAmplitude * envelope
                val current = Offset(x, centerY + bend + ripple)
                drawLine(
                    color = rowColor.copy(alpha = if (isActive) 1f else 0.38f),
                    start = previous,
                    end = current,
                    strokeWidth = strokeWidth
                )
                previous = current
            }

            if (isActive && centsDifference != null) {
                val apexY = centerY + animatedBend * maximumBend
                drawCircle(
                    color = rowColor,
                    radius = if (inTune) 5.dp.toPx() else 3.dp.toPx(),
                    center = Offset(size.width / 2f, apexY)
                )
            }
        }

        Column(
            modifier = Modifier.width(76.dp),
            horizontalAlignment = Alignment.End,
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(
                text = "${String.format("%.1f", targetFrequency)} Hz",
                color = LabAccentAmber,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                fontSize = 10.sp,
                maxLines = 1
            )
            Text(
                text = when {
                    centsDifference == null -> "WAITING"
                    !isActive -> "TARGET"
                    inTune -> "IN TUNE"
                    centsDifference < 0f -> "LOW ${String.format("%.1f", absoluteDifference)}c"
                    else -> "HIGH ${String.format("%.1f", absoluteDifference)}c"
                },
                color = rowColor,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                fontSize = 9.sp,
                maxLines = 1
            )
        }
    }
}

internal fun targetFrequency(
    target: InstrumentStringTarget,
    a4Reference: Int
): Float {
    val midi = targetMidi(target)
    return (a4Reference * Math.pow(2.0, (midi - 69) / 12.0)).toFloat()
}

internal fun centsDifference(
    detectedFrequency: Float,
    target: InstrumentStringTarget,
    a4Reference: Int
): Float? {
    if (!detectedFrequency.isFinite() || detectedFrequency <= 0f || a4Reference <= 0) return null
    val detectedMidi = 69.0 + 12.0 * (ln(detectedFrequency / a4Reference.toDouble()) / ln(2.0))
    return ((detectedMidi - targetMidi(target)) * 100.0).toFloat()
}

private fun targetMidi(target: InstrumentStringTarget): Int {
    val noteIndex = when (target.noteName) {
        "C" -> 0
        "C#", "Db" -> 1
        "D" -> 2
        "D#", "Eb" -> 3
        "E" -> 4
        "F" -> 5
        "F#", "Gb" -> 6
        "G" -> 7
        "G#", "Ab" -> 8
        "A" -> 9
        "A#", "Bb" -> 10
        "B" -> 11
        else -> error("Unsupported note name: ${target.noteName}")
    }
    return (target.octave + 1) * 12 + noteIndex
}
