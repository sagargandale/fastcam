package com.fastcam

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import android.media.MediaRecorder
import com.fastcam.ui.CameraScreen
import com.fastcam.ui.SettingsSheet
import com.fastcam.ui.theme.FastCamTheme

class MainActivity : ComponentActivity() {

    private val requiredPermissions = arrayOf(
        Manifest.permission.CAMERA,
        Manifest.permission.RECORD_AUDIO
    )

    private var hasPermissions by mutableStateOf(false)

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        hasPermissions = permissions.values.all { it }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        hasPermissions = checkPermissions()
        if (!hasPermissions) {
            requestPermissionLauncher.launch(requiredPermissions)
        }

        setContent {
            FastCamTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color.Black
                ) {
                    if (hasPermissions) {
                        CameraAppContent()
                    } else {
                        PermissionPlaceholder {
                            requestPermissionLauncher.launch(requiredPermissions)
                        }
                    }
                }
            }
        }
    }

    private fun checkPermissions(): Boolean {
        return requiredPermissions.all {
            ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
        }
    }
}

@Composable
fun CameraAppContent() {
    var showSettings by remember { mutableStateOf(false) }
    
    // Live parameters managed by Activity context
    var resolutionWidth by remember { mutableIntStateOf(1920) }
    var resolutionHeight by remember { mutableIntStateOf(1080) }
    var stabilizationEnabled by remember { mutableStateOf(true) }
    var oisEnabled by remember { mutableStateOf(false) }
    var aeMode by remember { mutableIntStateOf(0) }
    var antiFlickerHz by remember { mutableIntStateOf(0) } // 0 = off, 50 = 50Hz, 60 = 60Hz
    var targetFps by remember { mutableIntStateOf(60) }
    var noiseReductionMode by remember { mutableIntStateOf(1) } // 1 = Fast, 0 = Off, 2 = High Quality
    var audioSource by remember { mutableIntStateOf(MediaRecorder.AudioSource.CAMCORDER) }

    Box(modifier = Modifier.fillMaxSize()) {
        CameraScreen(
            onOpenSettings = { showSettings = true },
            resolutionWidth = resolutionWidth,
            resolutionHeight = resolutionHeight,
            stabilizationEnabled = stabilizationEnabled,
            audioSource = audioSource
        )

        if (showSettings) {
            SettingsSheet(
                onDismiss = { showSettings = false },
                resolutionWidth = resolutionWidth,
                resolutionHeight = resolutionHeight,
                onResolutionChange = { w, h ->
                    resolutionWidth = w
                    resolutionHeight = h
                },
                stabilizationEnabled = stabilizationEnabled,
                onStabilizationChange = { stabilizationEnabled = it },
                oisEnabled = oisEnabled,
                onOisChange = { oisEnabled = it },
                aeMode = aeMode,
                onAeModeChange = { aeMode = it },
                antiFlickerHz = antiFlickerHz,
                onAntiFlickerChange = { antiFlickerHz = it },
                fps = targetFps,
                onFpsChange = { targetFps = it },
                noiseReductionMode = noiseReductionMode,
                onNoiseReductionChange = { noiseReductionMode = it },
                audioSource = audioSource,
                onAudioSourceChange = { audioSource = it }
            )
        }
    }
}

@Composable
fun PermissionPlaceholder(onRequest: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = "FastCam requires Camera and Audio Recording permissions to capture high-quality cinematic footage.",
            color = Color.White,
            fontSize = 16.sp,
            textAlign = TextAlign.Center,
            fontWeight = FontWeight.Medium
        )
        Spacer(modifier = Modifier.height(32.dp))
        Button(
            onClick = onRequest,
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = Color.Black
            ),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.height(48.dp)
        ) {
            Text(
                text = "Grant Permissions",
                fontWeight = FontWeight.Bold,
                fontSize = 14.sp
            )
        }
    }
}
