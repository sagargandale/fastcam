package com.fastcam.ui

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.OrientationEventListener
import androidx.compose.animation.core.*
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.gestures.detectTapGestures
import kotlinx.coroutines.launch
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cached
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.fastcam.audio.AudioCapture
import com.fastcam.engine.CameraLens
import com.fastcam.engine.NativeBridge
import java.io.File
import java.text.SimpleDateFormat
import java.util.*
import kotlinx.coroutines.delay

@Composable
fun CameraScreen(
    onOpenSettings: () -> Unit,
    resolutionWidth: Int,
    resolutionHeight: Int,
    stabilizationEnabled: Boolean,
    audioSource: Int,
    oisEnabled: Boolean,
    aeMode: Int,
    antiFlickerHz: Int,
    targetFps: Int,
    noiseReductionMode: Int,
    hdrEnabled: Boolean
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    var isRecording by remember { mutableStateOf(false) }
    var isManualMode by remember { mutableStateOf(false) }
    var aeLocked by remember { mutableStateOf(false) }
    var surfaceObj by remember { mutableStateOf<Surface?>(null) }

    LaunchedEffect(isManualMode) {
        if (isManualMode && aeLocked) {
            aeLocked = false
            NativeBridge.nativeLockAe(false)
        }
    }
    
    // Zoom control
    var zoomRatio by remember { mutableFloatStateOf(1.0f) }
    
    // Active Lens Selection
    var availableLenses by remember { mutableStateOf<Array<CameraLens>>(emptyArray()) }
    var activeLensIndex by remember { mutableIntStateOf(0) }
    
    // Audio Capture Engine
    val audioCapture = remember { AudioCapture() }

    // Recording file state
    var lastRecordedUri by remember { mutableStateOf<Uri?>(null) }
    var recordingSeconds by remember { mutableIntStateOf(0) }

    // Touch to focus ring animation helper
    var focusTapPoint by remember { mutableStateOf<Offset?>(null) }
    val focusRingScale = remember { Animatable(1.5f) }
    val focusRingAlpha = remember { Animatable(0f) }

    // Retrieve lenses
    LaunchedEffect(Unit) {
        try {
            availableLenses = NativeBridge.nativeGetAvailableLenses()
            // Find default rear active lens index
            val rearIndex = availableLenses.indexOfFirst { it.facing == 1 }
            if (rearIndex >= 0) {
                activeLensIndex = rearIndex
            }
        } catch (e: Exception) {
            Log.e("CameraScreen", "Error getting lenses: ${e.message}")
        }
    }

    // Trigger JNI Re-init and sync all settings states dynamically to the C++ engine
    LaunchedEffect(
        surfaceObj, resolutionWidth, resolutionHeight, stabilizationEnabled,
        oisEnabled, aeMode, antiFlickerHz, targetFps, noiseReductionMode, hdrEnabled
    ) {
        val s = surfaceObj
        if (s != null) {
            NativeBridge.nativeInit(s, resolutionWidth, resolutionHeight, stabilizationEnabled)
            // Apply all current parameters to newly re-created engine
            NativeBridge.nativeSetOis(oisEnabled)
            NativeBridge.nativeSetAeMode(aeMode)
            NativeBridge.nativeSetAntiFlicker(antiFlickerHz)
            NativeBridge.nativeSetFrameRate(targetFps)
            NativeBridge.nativeSetNoiseReduction(noiseReductionMode.toByte())
            NativeBridge.nativeSetHdrEnabled(hdrEnabled)
            NativeBridge.nativeSetMode(!isManualMode)
            NativeBridge.nativeSetZoom(zoomRatio)
            if (aeLocked) {
                NativeBridge.nativeLockAe(true)
            }
        }
    }

    // Timer coroutine for active video recording duration
    LaunchedEffect(isRecording) {
        if (isRecording) {
            recordingSeconds = 0
            while (isRecording) {
                delay(1000)
                recordingSeconds++
            }
        }
    }

    // Physical Device Orientation Tracker
    var deviceOrientationDegrees by remember { mutableIntStateOf(0) }
    val orientationEventListener = remember {
        object : OrientationEventListener(context) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN) return
                // Map raw degrees (0-359) to nearest 90-degree quadrant
                val rotation = when (orientation) {
                    in 45 until 135 -> 270 // Landscape Right
                    in 135 until 225 -> 180 // Portrait Upside Down
                    in 225 until 315 -> 90 // Landscape Left
                    else -> 0 // Portrait
                }
                if (rotation != deviceOrientationDegrees) {
                    deviceOrientationDegrees = rotation
                }
            }
        }
    }

    DisposableEffect(orientationEventListener) {
        if (orientationEventListener.canDetectOrientation()) {
            orientationEventListener.enable()
        }
        onDispose {
            orientationEventListener.disable()
        }
    }

    fun handleStartStopRecording() {
        if (isRecording) {
            // Stop audio capture FIRST (joins the capture thread) so no pushAudioFrame()
            // calls race with MediaEncoder teardown inside nativeStopRecording().
            audioCapture.stop()
            NativeBridge.nativeStopRecording()
            isRecording = false
        } else {
            // Start recording
            val timeStamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(Date())
            val contentValues = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, "FASTCAM_${timeStamp}.mp4")
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, "DCIM/Camera")
            }
            val resolver = context.contentResolver
            val videoUri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, contentValues)
            if (videoUri != null) {
                try {
                    val pfd = resolver.openFileDescriptor(videoUri, "rw")
                    if (pfd != null) {
                        // Compute final video rotation
                        val activeLens = availableLenses.getOrNull(activeLensIndex)
                        val facing = activeLens?.facing ?: 1 // Default to back camera
                        val sensorOrientation = if (facing == 0) 270 else 90
                        val rotationDegrees = if (facing == 0) {
                            (sensorOrientation + deviceOrientationDegrees) % 360
                        } else {
                            (sensorOrientation - deviceOrientationDegrees + 360) % 360
                        }

                        NativeBridge.nativeStartRecording(pfd.detachFd(), rotationDegrees)
                        audioCapture.audioSource = audioSource
                        audioCapture.start()
                        isRecording = true
                        lastRecordedUri = videoUri
                    } else {
                        Log.e("CameraScreen", "Failed to open FileDescriptor for MediaStore URI")
                    }
                } catch (e: Exception) {
                    Log.e("CameraScreen", "Failed to start recording: ${e.message}")
                }
            } else {
                Log.e("CameraScreen", "Failed to insert video entry into MediaStore")
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Viewfinder surface with gesture handling
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            surfaceObj = holder.surface
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            surfaceObj = null
                            NativeBridge.nativeRelease()
                        }
                    })
                }
            },
            modifier = Modifier
                .aspectRatio(resolutionHeight.toFloat() / resolutionWidth.toFloat())
                .align(Alignment.Center)
                .pointerInput(Unit) {
                    detectTransformGestures { _, _, zoom, _ ->
                        val newZoom = (zoomRatio * zoom).coerceIn(1.0f, 8.0f)
                        if (newZoom != zoomRatio) {
                            zoomRatio = newZoom
                            NativeBridge.nativeSetZoom(newZoom)
                        }
                    }
                }
                .pointerInput(Unit) {
                    // Tap to Focus handling
                    // Map local coordinates to match SurfaceView layout
                    // Passing parameters down to CameraEngine
                    detectTapGestures(
                        onTap = { offset ->
                            focusTapPoint = offset
                            NativeBridge.nativeSetFocusPoint(
                                offset.x,
                                offset.y,
                                size.width,
                                size.height
                            )
                            // Animate focus ring
                            coroutineScope.launch {
                                focusRingAlpha.snapTo(1f)
                                focusRingScale.snapTo(1.5f)
                                focusRingScale.animateTo(
                                    targetValue = 1.0f,
                                    animationSpec = tween(durationMillis = 300, easing = FastOutSlowInEasing)
                                )
                                delay(800)
                                focusRingAlpha.animateTo(
                                    targetValue = 0f,
                                    animationSpec = tween(durationMillis = 200)
                                )
                                focusTapPoint = null
                            }
                        }
                    )
                }
        )

        // Focus ring animation canvas
        focusTapPoint?.let { point ->
            Canvas(
                modifier = Modifier
                    .aspectRatio(resolutionHeight.toFloat() / resolutionWidth.toFloat())
                    .align(Alignment.Center)
            ) {
                drawCircle(
                    color = Color(0xFFFFB300),
                    radius = 35.dp.toPx() * focusRingScale.value,
                    center = point,
                    style = Stroke(width = 2.dp.toPx()),
                    alpha = focusRingAlpha.value
                )
            }
        }

        // Top Column containing recording timer and settings button
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 48.dp, start = 24.dp, end = 24.dp)
                .align(Alignment.TopCenter),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Keep left empty for balance
            Spacer(modifier = Modifier.width(48.dp))

            // Timer running in top center
            if (isRecording) {
                val hh = String.format("%02d", recordingSeconds / 3600)
                val mm = String.format("%02d", (recordingSeconds % 3600) / 60)
                val ss = String.format("%02d", recordingSeconds % 60)
                Box(
                    modifier = Modifier
                        .background(Color(0x99D32F2F), RoundedCornerShape(12.dp))
                        .padding(horizontal = 12.dp, vertical = 6.dp)
                ) {
                    Text(
                        text = "$hh:$mm:$ss",
                        color = Color.White,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
            } else {
                Box(
                    modifier = Modifier
                        .background(Color(0x991A1A1A), RoundedCornerShape(12.dp))
                        .padding(horizontal = 12.dp, vertical = 6.dp)
                ) {
                    Text(
                        text = "00:00:00",
                        color = Color.White,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (!isManualMode) {
                    IconButton(
                        onClick = {
                            aeLocked = !aeLocked
                            NativeBridge.nativeLockAe(aeLocked)
                        },
                        modifier = Modifier
                            .size(44.dp)
                            .background(if (aeLocked) Color(0xFFFFB300) else Color(0x991A1A1A), CircleShape)
                    ) {
                        Icon(
                            imageVector = if (aeLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                            contentDescription = "Lock Exposure & Focus",
                            tint = if (aeLocked) Color.Black else Color.White
                        )
                    }
                }

                IconButton(
                    onClick = onOpenSettings,
                    modifier = Modifier
                        .size(44.dp)
                        .background(Color(0x991A1A1A), CircleShape)
                ) {
                    Icon(
                        imageVector = Icons.Default.Settings,
                        contentDescription = "Settings",
                        tint = Color.White
                    )
                }
            }
        }

        // Bottom section layout
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .align(Alignment.BottomCenter)
                .background(Color(0x33000000))
                .padding(bottom = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Manual tuning panel (Focus/EV/ISO sliders when manual selected)
            var showSliderPanel by remember { mutableStateOf(false) }
            ControlsOverlay(
                isManualMode = isManualMode,
                onManualControlOpen = { open -> showSliderPanel = open }
            )

            // Zoom indicator
            if (zoomRatio > 1.0f) {
                Box(
                    modifier = Modifier
                        .background(Color(0xAA1A1A1A), RoundedCornerShape(8.dp))
                        .padding(horizontal = 8.dp, vertical = 4.dp)
                ) {
                    Text(
                        text = String.format(Locale.US, "%.1fx Zoom", zoomRatio),
                        color = MaterialTheme.colorScheme.primary,
                        fontSize = 11.sp
                    )
                }
                Spacer(modifier = Modifier.height(8.dp))
            }

            // Mode Selector (AUTO / MANUAL toggle button above shutter)
            Row(
                modifier = Modifier
                    .clip(RoundedCornerShape(20.dp))
                    .background(Color(0x991A1A1A))
                    .padding(4.dp),
                horizontalArrangement = Arrangement.Center
            ) {
                Text(
                    text = "AUTO",
                    color = if (!isManualMode) Color.Black else Color.White,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .background(if (!isManualMode) MaterialTheme.colorScheme.primary else Color.Transparent)
                        .clickable {
                            isManualMode = false
                            NativeBridge.nativeSetMode(true)
                        }
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                )
                Text(
                    text = "MANUAL",
                    color = if (isManualMode) Color.Black else Color.White,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .background(if (isManualMode) MaterialTheme.colorScheme.primary else Color.Transparent)
                        .clickable {
                            isManualMode = true
                            NativeBridge.nativeSetMode(false)
                        }
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                )
            }

            Spacer(modifier = Modifier.height(16.dp))

            // Bottom Shutter Controls row
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 32.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Bottom left: Recent video preview thumbnail
                Box(
                    modifier = Modifier
                        .size(54.dp)
                        .clip(CircleShape)
                        .border(1.dp, Color.White, CircleShape)
                        .background(Color.DarkGray)
                        .clickable {
                            // Launch photo gallery for recorded videos
                        },
                    contentAlignment = Alignment.Center
                ) {
                    if (lastRecordedUri != null) {
                        Icon(
                            imageVector = Icons.Default.Cached, // Fallback icon representational of files
                            contentDescription = "Recent File",
                            tint = Color.LightGray,
                            modifier = Modifier.size(24.dp)
                        )
                    }
                }

                // Bottom center: Large recording/shutter button
                Box(
                    modifier = Modifier
                        .size(80.dp)
                        .clip(CircleShape)
                        .border(4.dp, Color.White, CircleShape)
                        .background(Color.Transparent)
                        .clickable { handleStartStopRecording() }
                        .padding(8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    val scale by animateFloatAsState(if (isRecording) 0.65f else 1.0f, label = "Record scale")
                    Box(
                        modifier = Modifier
                            .fillMaxSize(scale)
                            .clip(if (isRecording) RoundedCornerShape(8.dp) else CircleShape)
                            .background(Color(0xFFD32F2F))
                    )
                }

                // Bottom right: Lens Selector button
                Box(
                    modifier = Modifier
                        .size(54.dp)
                        .clip(CircleShape)
                        .background(Color(0x991A1A1A))
                        .clickable {
                            if (availableLenses.isNotEmpty()) {
                                activeLensIndex = (activeLensIndex + 1) % availableLenses.size
                                val chosenLens = availableLenses[activeLensIndex]
                                NativeBridge.nativeSetLens(chosenLens.id)
                                zoomRatio = 1.0f
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    val label = if (availableLenses.isNotEmpty()) {
                        val focal = availableLenses[activeLensIndex].focalLength
                        // Map focal length to lens multipliers (1x wide, 0.5x ultrawide, etc)
                        if (focal < 3.0f) "0.5x" else if (focal > 6.0f) "2x" else "1x"
                    } else {
                        "1x"
                    }
                    Text(
                        text = label,
                        color = Color.White,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }
    }
}

