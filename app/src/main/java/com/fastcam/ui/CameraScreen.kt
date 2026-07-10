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

import kotlinx.coroutines.withContext
import kotlinx.coroutines.Dispatchers

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

    var histogramBins by remember { mutableStateOf(IntArray(64)) }

    // Periodically fetch histogram data (every 50ms = 20fps)
    LaunchedEffect(surfaceObj) {
        if (surfaceObj != null) {
            while (true) {
                val bins = withContext(Dispatchers.IO) { NativeBridge.nativeGetHistogram() }
                histogramBins = bins
                delay(50)
            }
        } else {
            histogramBins = IntArray(64)
        }
    }
    
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

    // Retrieve lenses — called AFTER nativeInit() so gEngine is populated
    // (calling before init would hit the gEngine==null path and return empty)
    suspend fun fetchLenses() {
        val lenses = withContext(Dispatchers.IO) { NativeBridge.nativeGetAvailableLenses() }
        availableLenses = lenses
        val rearIndex = lenses.indexOfFirst { it.facing == 1 }
        if (rearIndex >= 0) activeLensIndex = rearIndex
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Re-init ONLY when the surface changes or resolution changes.
    // Changing other runtime params (fps, ae, flicker…) does NOT need a full
    // camera teardown — they are applied via their own individual setters below.
    // ──────────────────────────────────────────────────────────────────────────
    LaunchedEffect(surfaceObj, resolutionWidth, resolutionHeight) {
        val s = surfaceObj ?: return@LaunchedEffect
        withContext(Dispatchers.IO) {
            NativeBridge.nativeInit(s, resolutionWidth, resolutionHeight, stabilizationEnabled)
            // Apply current session state to freshly opened engine
            NativeBridge.nativeSetOis(oisEnabled)
            NativeBridge.nativeSetAeMode(aeMode)
            NativeBridge.nativeSetAntiFlicker(antiFlickerHz)
            NativeBridge.nativeSetFrameRate(targetFps)
            NativeBridge.nativeSetNoiseReduction(noiseReductionMode.toByte())
            NativeBridge.nativeSetHdrEnabled(hdrEnabled)
            NativeBridge.nativeSetMode(!isManualMode)
            NativeBridge.nativeSetZoom(zoomRatio)
            if (aeLocked) NativeBridge.nativeLockAe(true)
        }
        // Fetch lenses after engine is ready (gEngine != null)
        fetchLenses()
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Runtime parameter changes — applied directly without re-initialising camera
    // ──────────────────────────────────────────────────────────────────────────
    LaunchedEffect(stabilizationEnabled) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetStabilization(stabilizationEnabled) }
    }
    LaunchedEffect(oisEnabled) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetOis(oisEnabled) }
    }
    LaunchedEffect(aeMode) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetAeMode(aeMode) }
    }
    LaunchedEffect(antiFlickerHz) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetAntiFlicker(antiFlickerHz) }
    }
    LaunchedEffect(targetFps) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetFrameRate(targetFps) }
    }
    LaunchedEffect(noiseReductionMode) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetNoiseReduction(noiseReductionMode.toByte()) }
    }
    LaunchedEffect(hdrEnabled) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetHdrEnabled(hdrEnabled) }
    }
    LaunchedEffect(zoomRatio) {
        if (surfaceObj != null) withContext(Dispatchers.IO) { NativeBridge.nativeSetZoom(zoomRatio) }
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

    // Recording error display state
    var recordingErrorMessage by remember { mutableStateOf<String?>(null) }

    fun handleStartStopRecording() {
        if (isRecording) {
            // Dispatch stop to IO: audioCapture.stop() joins the audio thread (~20-50ms)
            // and nativeStopRecording() finalizes the muxer — neither should block the UI thread.
            coroutineScope.launch(Dispatchers.IO) {
                audioCapture.stop()
                NativeBridge.nativeStopRecording()
            }
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
            if (videoUri == null) {
                recordingErrorMessage = "Failed to create video file in gallery"
                Log.e("CameraScreen", "Failed to insert video entry into MediaStore")
                return
            }
            try {
                val pfd = resolver.openFileDescriptor(videoUri, "rw")
                    ?: throw Exception("Could not open file descriptor for MediaStore URI")

                val rawFd = pfd.detachFd()
                pfd.close() // Release the PFD Java wrapper — ownership of rawFd is now with native

                // Compute final video rotation
                val activeLens = availableLenses.getOrNull(activeLensIndex)
                val facing = activeLens?.facing ?: 1
                val sensorOrientation = if (facing == 0) 270 else 90
                val rotationDegrees = if (facing == 0) {
                    (sensorOrientation + deviceOrientationDegrees) % 360
                } else {
                    (sensorOrientation - deviceOrientationDegrees + 360) % 360
                }

                val started = NativeBridge.nativeStartRecording(rawFd, rotationDegrees)
                if (!started) {
                    // Native encoder failed — clean up the orphaned MediaStore entry
                    resolver.delete(videoUri, null, null)
                    recordingErrorMessage = "Failed to start encoder. Try a lower resolution."
                    Log.e("CameraScreen", "nativeStartRecording returned false")
                    return
                }

                audioCapture.audioSource = audioSource
                audioCapture.start()
                isRecording = true
                lastRecordedUri = videoUri
            } catch (e: Exception) {
                // Clean up orphaned MediaStore entry on any exception
                resolver.delete(videoUri, null, null)
                recordingErrorMessage = "Recording failed: ${e.message}"
                Log.e("CameraScreen", "Failed to start recording", e)
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Error snackbar — shown when recording fails
        recordingErrorMessage?.let { msg ->
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .align(Alignment.BottomCenter)
                    .padding(16.dp)
            ) {
                Snackbar(
                    action = {
                        TextButton(onClick = { recordingErrorMessage = null }) {
                            Text("Dismiss", color = MaterialTheme.colorScheme.primary)
                        }
                    }
                ) { Text(msg) }
            }
        }
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
                        .background(if (isRecording) Color(0x552C2C2C) else Color(0x991A1A1A))
                        .clickable(enabled = !isRecording) {
                            if (availableLenses.isNotEmpty()) {
                                activeLensIndex = (activeLensIndex + 1) % availableLenses.size
                                val chosenLens = availableLenses[activeLensIndex]
                                // Dispatch to IO — setLens() calls stopLoopThread() which joins the
                                // camera background thread. Blocking the main thread here causes ANR.
                                coroutineScope.launch(Dispatchers.IO) {
                                    NativeBridge.nativeSetLens(chosenLens.id)
                                }
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

        // Histogram Overlay (Blackmagic Style)
        HistogramOverlay(
            bins = histogramBins,
            modifier = Modifier
                .padding(start = 16.dp, bottom = 170.dp)
                .align(Alignment.BottomStart)
        )
    }
}

@Composable
fun HistogramOverlay(
    bins: IntArray,
    modifier: Modifier = Modifier
) {
    Canvas(
        modifier = modifier
            .width(160.dp)
            .height(60.dp)
            .background(Color(0x44000000), shape = RoundedCornerShape(6.dp))
            .padding(6.dp)
    ) {
        val width = size.width
        val height = size.height
        val binCount = bins.size
        if (binCount == 0) return@Canvas

        val maxVal = (bins.maxOrNull() ?: 1).coerceAtLeast(1).toFloat()
        val path = androidx.compose.ui.graphics.Path()

        val step = width / (binCount - 1).toFloat()

        for (i in 0 until binCount) {
            val normalizedHeight = bins[i].toFloat() / maxVal
            val x = i * step
            val y = height - (normalizedHeight * (height - 2f))

            if (i == 0) {
                path.moveTo(x, height)
                path.lineTo(x, y)
            } else {
                path.lineTo(x, y)
            }
        }
        path.lineTo(width, height)
        path.close()

        // Draw semi-transparent fill
        drawPath(
            path = path,
            color = Color(0x66FFFFFF)
        )

        // Draw top outline path
        val strokePath = androidx.compose.ui.graphics.Path()
        for (i in 0 until binCount) {
            val normalizedHeight = bins[i].toFloat() / maxVal
            val x = i * step
            val y = height - (normalizedHeight * (height - 2f))
            if (i == 0) {
                strokePath.moveTo(x, y)
            } else {
                strokePath.lineTo(x, y)
            }
        }
        drawPath(
            path = strokePath,
            color = Color.White,
            style = Stroke(width = 1.dp.toPx())
        )
    }
}

