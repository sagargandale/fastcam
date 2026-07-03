package com.fastcam.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import com.fastcam.engine.NativeBridge
import kotlin.concurrent.thread

/**
 * Capture PCM audio frames on a background thread and push them to the C++ encoder.
 */
class AudioCapture {
    var audioSource: Int = MediaRecorder.AudioSource.CAMCORDER

    private var audioRecord: AudioRecord? = null
    private var isRecording = false
    private var captureThread: Thread? = null

    private val sampleRate = 44100
    private val channelConfig = AudioFormat.CHANNEL_IN_STEREO
    private val audioFormat = AudioFormat.ENCODING_PCM_16BIT

    @SuppressLint("MissingPermission")
    fun start() {
        val bufferSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        if (bufferSize == AudioRecord.ERROR || bufferSize == AudioRecord.ERROR_BAD_VALUE) {
            Log.e(TAG, "Invalid buffer size computed")
            return
        }

        try {
            audioRecord = AudioRecord(
                audioSource,
                sampleRate,
                channelConfig,
                audioFormat,
                bufferSize * 2
            )
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied for audio recording: ${e.message}")
            return
        }

        if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord initialization failed")
            return
        }

        audioRecord?.startRecording()
        isRecording = true

        captureThread = thread(start = true, name = "fastcam-audio-capture") {
            val chunkSize = 2048
            val buffer = ByteArray(chunkSize)
            while (isRecording) {
                val bytesRead = audioRecord?.read(buffer, 0, buffer.size) ?: 0
                if (bytesRead > 0) {
                    NativeBridge.nativePushAudioFrame(buffer, bytesRead)
                } else if (bytesRead < 0) {
                    Log.e(TAG, "Error reading audio data: $bytesRead")
                    break
                }
            }
        }
    }

    fun stop() {
        isRecording = false
        captureThread?.join(500)
        captureThread = null

        try {
            audioRecord?.stop()
            audioRecord?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioRecord: ${e.message}")
        }
        audioRecord = null
    }

    companion object {
        private const val TAG = "AudioCapture"
    }
}
