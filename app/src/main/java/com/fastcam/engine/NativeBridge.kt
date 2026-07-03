package com.fastcam.engine

import android.view.Surface

object NativeBridge {
    init {
        System.loadLibrary("fastcam-engine")
    }

    external fun nativeInit(surface: Surface, width: Int, height: Int, useStabilization: Boolean): Boolean
    external fun nativeStartRecording(fd: Int)
    external fun nativeStopRecording()
    external fun nativeRelease()
    external fun nativeSetMode(isAuto: Boolean)
    external fun nativeSetStabilization(enabled: Boolean)
    external fun nativeSetOis(enabled: Boolean)
    external fun nativeIsStabilizationActive(): Boolean
    external fun nativeSetAeMode(mode: Int)
    external fun nativeSetAntiFlicker(hz: Int)
    external fun nativeLockAe(lock: Boolean)
    external fun nativeSetFocus(autoFocus: Boolean, distance: Float)
    external fun nativeSetExposure(autoExposure: Boolean, iso: Int, shutterNs: Long)
    external fun nativeSetFrameRate(fps: Int)
    external fun nativeSetZoom(ratio: Float)
    external fun nativeSetNoiseReduction(mode: Byte)
    external fun nativeSetFocusPoint(x: Float, y: Float, viewW: Int, viewH: Int)
    external fun nativeSetExposureCompensation(value: Int)
    external fun nativePushAudioFrame(data: ByteArray, size: Int)
    external fun nativeSetLens(lensId: String)
    external fun nativeGetAvailableLenses(): Array<CameraLens>
}
