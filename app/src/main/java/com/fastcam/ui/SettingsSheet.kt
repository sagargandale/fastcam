package com.fastcam.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.media.MediaRecorder
import com.fastcam.engine.NativeBridge

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsSheet(
    onDismiss: () -> Unit,
    resolutionWidth: Int,
    resolutionHeight: Int,
    onResolutionChange: (width: Int, height: Int) -> Unit,
    stabilizationEnabled: Boolean,
    onStabilizationChange: (Boolean) -> Unit,
    oisEnabled: Boolean,
    onOisChange: (Boolean) -> Unit,
    aeMode: Int,
    onAeModeChange: (Int) -> Unit,
    fps: Int,
    onFpsChange: (Int) -> Unit,
    noiseReductionMode: Int,
    onNoiseReductionChange: (Int) -> Unit,
    audioSource: Int,
    onAudioSourceChange: (Int) -> Unit
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF1A1A1A),
        contentColor = Color.White
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(24.dp)
        ) {
            Text(
                text = "FastCam Settings",
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                color = Color.White
            )
            Spacer(modifier = Modifier.height(24.dp))

            // Resolution setting (4K vs 1080p)
            Text(
                text = "Video Resolution",
                fontSize = 14.sp,
                color = Color.Gray,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                ResolutionBtn(
                    label = "1080p (FHD)",
                    selected = resolutionWidth == 1920 && resolutionHeight == 1080,
                    onClick = { onResolutionChange(1920, 1080) }
                )
                ResolutionBtn(
                    label = "4K (UHD)",
                    selected = resolutionWidth == 3840 && resolutionHeight == 2160,
                    onClick = { onResolutionChange(3840, 2160) }
                )
            }

            Spacer(modifier = Modifier.height(24.dp))

            // Frame Rate (24, 30, 60 fps)
            Text(
                text = "Target Frame Rate",
                fontSize = 14.sp,
                color = Color.Gray,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                ResolutionBtn(
                    label = "24 FPS",
                    selected = fps == 24,
                    onClick = { 
                        onFpsChange(24)
                        NativeBridge.nativeSetFrameRate(24)
                    }
                )
                ResolutionBtn(
                    label = "30 FPS",
                    selected = fps == 30,
                    onClick = { 
                        onFpsChange(30)
                        NativeBridge.nativeSetFrameRate(30)
                    }
                )
                ResolutionBtn(
                    label = "60 FPS",
                    selected = fps == 60,
                    onClick = { 
                        onFpsChange(60)
                        NativeBridge.nativeSetFrameRate(60)
                    }
                )
            }

            // Auto Exposure Mode (Custom PID vs Hardware AE)
            Text(
                text = "Auto Exposure Mode",
                fontSize = 14.sp,
                color = Color.Gray,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                ResolutionBtn(
                    label = "Custom (Cinema)",
                    selected = aeMode == 0,
                    onClick = { 
                        onAeModeChange(0)
                        NativeBridge.nativeSetAeMode(0)
                    }
                )
                ResolutionBtn(
                    label = "Hardware (Auto)",
                    selected = aeMode == 1,
                    onClick = { 
                        onAeModeChange(1)
                        NativeBridge.nativeSetAeMode(1)
                    }
                )
                ResolutionBtn(
                    label = "Portrait (Face)",
                    selected = aeMode == 2,
                    onClick = { 
                        onAeModeChange(2)
                        NativeBridge.nativeSetAeMode(2)
                    }
                )
            }

            Spacer(modifier = Modifier.height(24.dp))

            var isEisActive by remember { mutableStateOf(false) }
            LaunchedEffect(stabilizationEnabled) {
                isEisActive = NativeBridge.nativeIsStabilizationActive()
            }

            // Custom EIS Setting
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = "Custom EIS",
                            fontSize = 16.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.White
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        Box(
                            modifier = Modifier
                                .background(
                                    color = if (isEisActive) Color(0xFF1E3F20) else Color(0xFF3F1E1E),
                                    shape = RoundedCornerShape(4.dp)
                                )
                                .padding(horizontal = 6.dp, vertical = 2.dp)
                        ) {
                            Text(
                                text = if (isEisActive) "Active" else "Inactive",
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                                color = if (isEisActive) Color(0xFF4CAF50) else Color(0xFFF44336)
                            )
                        }
                    }
                    Text(
                        text = "Gyroscope stabilized rendering",
                        fontSize = 12.sp,
                        color = Color.Gray
                    )
                }
                Switch(
                    checked = stabilizationEnabled,
                    onCheckedChange = { checked ->
                        onStabilizationChange(checked)
                        NativeBridge.nativeSetStabilization(checked)
                    },
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = MaterialTheme.colorScheme.primary,
                        checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                    )
                )
            }
            
            Spacer(modifier = Modifier.height(24.dp))

            // Hardware OIS Setting
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = "Hardware OIS",
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                    Text(
                        text = "Lens optical image stabilization",
                        fontSize = 12.sp,
                        color = Color.Gray
                    )
                }
                Switch(
                    checked = oisEnabled,
                    onCheckedChange = { checked ->
                        onOisChange(checked)
                        NativeBridge.nativeSetOis(checked)
                    },
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = MaterialTheme.colorScheme.primary,
                        checkedTrackColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                    )
                )
            }
            
            Spacer(modifier = Modifier.height(24.dp))
            
            // Noise reduction setting
            Text(
                text = "Noise Reduction Mode",
                fontSize = 14.sp,
                color = Color.Gray,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                ResolutionBtn(
                    label = "Off (Preserve)",
                    selected = noiseReductionMode == 0,
                    onClick = { 
                        onNoiseReductionChange(0)
                        NativeBridge.nativeSetNoiseReduction(0)
                    }
                )
                ResolutionBtn(
                    label = "Fast (Optimal)",
                    selected = noiseReductionMode == 1,
                    onClick = { 
                        onNoiseReductionChange(1)
                        NativeBridge.nativeSetNoiseReduction(1)
                    }
                )
                ResolutionBtn(
                    label = "HQ (Cinematic)",
                    selected = noiseReductionMode == 2,
                    onClick = { 
                        onNoiseReductionChange(2)
                        NativeBridge.nativeSetNoiseReduction(2)
                    }
                )
            }

            Spacer(modifier = Modifier.height(24.dp))

            // Audio microphone source setting
            Text(
                text = "Audio Microphone Source",
                fontSize = 14.sp,
                color = Color.Gray,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                ResolutionBtn(
                    label = "Camcorder Mic",
                    selected = audioSource == MediaRecorder.AudioSource.CAMCORDER,
                    onClick = { onAudioSourceChange(MediaRecorder.AudioSource.CAMCORDER) }
                )
                ResolutionBtn(
                    label = "Standard Mic",
                    selected = audioSource == MediaRecorder.AudioSource.MIC,
                    onClick = { onAudioSourceChange(MediaRecorder.AudioSource.MIC) }
                )
                ResolutionBtn(
                    label = "Unprocessed",
                    selected = audioSource == MediaRecorder.AudioSource.UNPROCESSED,
                    onClick = { onAudioSourceChange(MediaRecorder.AudioSource.UNPROCESSED) }
                )
            }
            Spacer(modifier = Modifier.height(48.dp))
        }
    }
}

@Composable
fun RowScope.ResolutionBtn(
    label: String,
    selected: Boolean,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .weight(1f)
            .height(48.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(if (selected) MaterialTheme.colorScheme.primary else Color(0xFF2C2C2C))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = if (selected) Color.Black else Color.White,
            fontWeight = FontWeight.Bold,
            fontSize = 12.sp
        )
    }
}
