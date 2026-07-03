package com.fastcam.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Brightness5
import androidx.compose.material.icons.filled.CameraRoll
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.WbSunny
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.fastcam.engine.NativeBridge

enum class ActiveManualControl {
    NONE, FOCUS, EXPOSURE, ISO
}

@Composable
fun ControlsOverlay(
    modifier: Modifier = Modifier,
    isManualMode: Boolean,
    onManualControlOpen: (Boolean) -> Unit
) {
    var activeControl by remember { mutableStateOf(ActiveManualControl.NONE) }
    
    // Manual adjustment states
    var manualFocusVal by remember { mutableFloatStateOf(0.0f) } // 0 = infinity
    var isManualFocusAuto by remember { mutableStateOf(true) }
    
    var manualIsoVal by remember { mutableIntStateOf(400) }
    var isManualIsoAuto by remember { mutableStateOf(true) }
    
    var manualShutterNs by remember { mutableLongStateOf(16666666L) } // 1/60s
    
    var evVal by remember { mutableIntStateOf(0) }

    LaunchedEffect(isManualMode) {
        if (!isManualMode) {
            activeControl = ActiveManualControl.NONE
            onManualControlOpen(false)
        }
    }

    Column(
        modifier = modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Active slider section
        if (isManualMode && activeControl != ActiveManualControl.NONE) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 24.dp)
                    .background(Color(0xAA1A1A1A), RoundedCornerShape(16.dp))
                    .padding(16.dp),
                contentAlignment = Alignment.Center
            ) {
                when (activeControl) {
                    ActiveManualControl.NONE -> {}
                    ActiveManualControl.FOCUS -> {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("Focus Distance", color = Color.White, fontSize = 14.sp)
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text("Auto Focus", color = Color.White, fontSize = 12.sp)
                                    Checkbox(
                                        checked = isManualFocusAuto,
                                        onCheckedChange = { auto ->
                                            isManualFocusAuto = auto
                                            NativeBridge.nativeSetFocus(auto, manualFocusVal)
                                        },
                                        colors = CheckboxDefaults.colors(
                                            checkedColor = MaterialTheme.colorScheme.primary,
                                            checkmarkColor = Color.Black
                                        )
                                    )
                                }
                            }
                            if (!isManualFocusAuto) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Text("∞", color = Color.LightGray, fontSize = 16.sp)
                                    Slider(
                                        value = manualFocusVal,
                                        onValueChange = { valDist ->
                                            manualFocusVal = valDist
                                            NativeBridge.nativeSetFocus(false, valDist)
                                        },
                                        valueRange = 0.0f..10.0f,
                                        modifier = Modifier.weight(1f),
                                        colors = SliderDefaults.colors(
                                            thumbColor = MaterialTheme.colorScheme.primary,
                                            activeTrackColor = MaterialTheme.colorScheme.primary
                                        )
                                    )
                                    Text("Close", color = Color.LightGray, fontSize = 12.sp)
                                }
                            }
                        }
                    }
                    ActiveManualControl.EXPOSURE -> {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Exposure Compensation (EV): ${if (evVal >= 0) "+$evVal" else evVal}", color = Color.White, fontSize = 14.sp)
                            Slider(
                                value = evVal.toFloat(),
                                onValueChange = { valEv ->
                                    val evInt = valEv.toInt()
                                    evVal = evInt
                                    NativeBridge.nativeSetExposureCompensation(evInt)
                                },
                                valueRange = -3.0f..3.0f,
                                steps = 5,
                                colors = SliderDefaults.colors(
                                    thumbColor = MaterialTheme.colorScheme.primary,
                                    activeTrackColor = MaterialTheme.colorScheme.primary
                                )
                            )
                        }
                    }
                    ActiveManualControl.ISO -> {
                        val isoValues = listOf(100, 200, 400, 800, 1600, 3200)
                        val shutterSpeeds = listOf(
                            1000000000L / 1000, // 1/1000s
                            1000000000L / 500,  // 1/500s
                            1000000000L / 250,  // 1/250s
                            1000000000L / 125,  // 1/125s
                            1000000000L / 60,   // 1/60s
                            1000000000L / 30,   // 1/30s
                            1000000000L / 24    // 1/24s
                        )
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("ISO / Shutter Speed", color = Color.White, fontSize = 14.sp)
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text("Auto AE", color = Color.White, fontSize = 12.sp)
                                    Checkbox(
                                        checked = isManualIsoAuto,
                                        onCheckedChange = { auto ->
                                            isManualIsoAuto = auto
                                            NativeBridge.nativeSetExposure(auto, manualIsoVal, manualShutterNs)
                                        },
                                        colors = CheckboxDefaults.colors(
                                            checkedColor = MaterialTheme.colorScheme.primary,
                                            checkmarkColor = Color.Black
                                        )
                                    )
                                }
                            }
                            if (!isManualIsoAuto) {
                                Spacer(modifier = Modifier.height(8.dp))
                                Text("ISO: $manualIsoVal", color = Color.White, fontSize = 12.sp)
                                val currentIsoIndex = isoValues.indexOf(manualIsoVal).takeIf { it != -1 } ?: 2
                                Slider(
                                    value = currentIsoIndex.toFloat(),
                                    onValueChange = { valIso ->
                                        val index = Math.round(valIso).toInt().coerceIn(0, isoValues.size - 1)
                                        manualIsoVal = isoValues[index]
                                        NativeBridge.nativeSetExposure(false, manualIsoVal, manualShutterNs)
                                    },
                                    valueRange = 0f..(isoValues.size - 1).toFloat(),
                                    steps = isoValues.size - 2,
                                    colors = SliderDefaults.colors(
                                        thumbColor = MaterialTheme.colorScheme.primary,
                                        activeTrackColor = MaterialTheme.colorScheme.primary
                                    )
                                )
                                Spacer(modifier = Modifier.height(8.dp))
                                val shutterFraction = if (manualShutterNs > 0) 1.0f / (manualShutterNs / 1e9f) else 0f
                                Text("Shutter Speed: 1/${shutterFraction.toInt()}s", color = Color.White, fontSize = 12.sp)
                                val currentShutterIndex = shutterSpeeds.indexOf(manualShutterNs).takeIf { it != -1 } ?: 4
                                Slider(
                                    value = currentShutterIndex.toFloat(),
                                    onValueChange = { valShutter ->
                                        val index = Math.round(valShutter).toInt().coerceIn(0, shutterSpeeds.size - 1)
                                        manualShutterNs = shutterSpeeds[index]
                                        NativeBridge.nativeSetExposure(false, manualIsoVal, manualShutterNs)
                                    },
                                    valueRange = 0f..(shutterSpeeds.size - 1).toFloat(),
                                    steps = shutterSpeeds.size - 2,
                                    colors = SliderDefaults.colors(
                                        thumbColor = MaterialTheme.colorScheme.primary,
                                        activeTrackColor = MaterialTheme.colorScheme.primary
                                    )
                                )
                            }
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.height(16.dp))
        }

        // Circular Icon Buttons Row
        if (isManualMode) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 24.dp),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically
            ) {
                ManualIconBtn(
                    icon = Icons.Default.CenterFocusStrong,
                    label = "Focus",
                    selected = activeControl == ActiveManualControl.FOCUS,
                    onClick = {
                        activeControl = if (activeControl == ActiveManualControl.FOCUS) ActiveManualControl.NONE else ActiveManualControl.FOCUS
                        onManualControlOpen(activeControl != ActiveManualControl.NONE)
                    }
                )
                ManualIconBtn(
                    icon = Icons.Default.WbSunny,
                    label = "EV",
                    selected = activeControl == ActiveManualControl.EXPOSURE,
                    onClick = {
                        activeControl = if (activeControl == ActiveManualControl.EXPOSURE) ActiveManualControl.NONE else ActiveManualControl.EXPOSURE
                        onManualControlOpen(activeControl != ActiveManualControl.NONE)
                    }
                )
                ManualIconBtn(
                    icon = Icons.Default.Brightness5,
                    label = "ISO",
                    selected = activeControl == ActiveManualControl.ISO,
                    onClick = {
                        activeControl = if (activeControl == ActiveManualControl.ISO) ActiveManualControl.NONE else ActiveManualControl.ISO
                        onManualControlOpen(activeControl != ActiveManualControl.NONE)
                    }
                )
            }
            Spacer(modifier = Modifier.height(16.dp))
        }
    }
}

@Composable
fun ManualIconBtn(
    icon: ImageVector,
    label: String,
    selected: Boolean,
    onClick: () -> Unit
) {
    val accent = MaterialTheme.colorScheme.primary
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier.clickable(onClick = onClick)
    ) {
        Box(
            modifier = Modifier
                .size(48.dp)
                .clip(CircleShape)
                .background(if (selected) accent else Color(0x771A1A1A))
                .padding(8.dp),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = icon,
                contentDescription = label,
                tint = if (selected) Color.Black else Color.White,
                modifier = Modifier.size(24.dp)
            )
        }
        Spacer(modifier = Modifier.height(4.dp))
        Text(label, color = Color.White, fontSize = 11.sp)
    }
}
