#include "MediaEncoder.h"
#include <android/log.h>
#include <unistd.h>
#include <chrono>

#define LOG_TAG "[MediaEncoder]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

MediaEncoder::MediaEncoder() = default;

MediaEncoder::~MediaEncoder() {
    stop();
}

bool MediaEncoder::configure(int fd, int width, int height, int bitrate, int fps, int sampleRate, int channelCount) {
    mFd = fd;
    mSampleRate = sampleRate;
    mChannelCount = channelCount;

    mMuxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!mMuxer) {
        LOGE("Failed to create MediaMuxer");
        return false;
    }

    // 1. Configure Video Encoder (H.264)
    mVideoFormat = AMediaFormat_new();
    AMediaFormat_setString(mVideoFormat, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789); // COLOR_FormatSurface
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(mVideoFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1); // 1 second keyframes

    mVideoCodec = AMediaCodec_createEncoderByType("video/avc");
    if (!mVideoCodec) {
        LOGE("Failed to create video encoder");
        return false;
    }

    media_status_t status = AMediaCodec_configure(mVideoCodec, mVideoFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK) {
        LOGE("Failed to configure video encoder: %d", status);
        return false;
    }

    status = AMediaCodec_createInputSurface(mVideoCodec, &mEncoderWindow);
    if (status != AMEDIA_OK || !mEncoderWindow) {
        LOGE("Failed to create input surface for video encoder: %d", status);
        return false;
    }

    // 2. Configure Audio Encoder (AAC)
    mAudioFormat = AMediaFormat_new();
    AMediaFormat_setString(mAudioFormat, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(mAudioFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, sampleRate);
    AMediaFormat_setInt32(mAudioFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channelCount);
    AMediaFormat_setInt32(mAudioFormat, AMEDIAFORMAT_KEY_BIT_RATE, 128000); // 128 kbps
    AMediaFormat_setInt32(mAudioFormat, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AAC-LC

    mAudioCodec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!mAudioCodec) {
        LOGE("Failed to create audio encoder");
        // Audio optional, but log failure
    } else {
        status = AMediaCodec_configure(mAudioCodec, mAudioFormat, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        if (status != AMEDIA_OK) {
            LOGE("Failed to configure audio encoder: %d", status);
            AMediaCodec_delete(mAudioCodec);
            mAudioCodec = nullptr;
        }
    }

    LOGI("MediaEncoder configured successfully.");
    return true;
}

bool MediaEncoder::start() {
    if (mVideoCodec) {
        media_status_t status = AMediaCodec_start(mVideoCodec);
        if (status != AMEDIA_OK) {
            LOGE("Failed to start video codec");
            return false;
        }
        mVideoRunning = true;
        mVideoThread = std::thread(&MediaEncoder::videoEncoderLoop, this);
    }

    if (mAudioCodec) {
        media_status_t status = AMediaCodec_start(mAudioCodec);
        if (status != AMEDIA_OK) {
            LOGE("Failed to start audio codec");
        } else {
            mAudioRunning = true;
            mAudioThread = std::thread(&MediaEncoder::audioEncoderLoop, this);
        }
    }

    LOGI("Encoding threads started.");
    return true;
}

void MediaEncoder::stop() {
    // Signal loops to stop
    mVideoRunning = false;
    mAudioRunning = false;

    // Wake up audio thread
    mAudioQueueCondVar.notify_all();

    if (mVideoThread.joinable()) {
        mVideoThread.join();
    }
    if (mAudioThread.joinable()) {
        mAudioThread.join();
    }

    // Stop and release codecs
    if (mVideoCodec) {
        AMediaCodec_stop(mVideoCodec);
        AMediaCodec_delete(mVideoCodec);
        mVideoCodec = nullptr;
    }
    if (mAudioCodec) {
        AMediaCodec_stop(mAudioCodec);
        AMediaCodec_delete(mAudioCodec);
        mAudioCodec = nullptr;
    }

    // Release formats
    if (mVideoFormat) {
        AMediaFormat_delete(mVideoFormat);
        mVideoFormat = nullptr;
    }
    if (mAudioFormat) {
        AMediaFormat_delete(mAudioFormat);
        mAudioFormat = nullptr;
    }

    // Stop muxer
    if (mMuxer) {
        std::lock_guard<std::mutex> lock(mMuxerMutex);
        if (mMuxerStarted) {
            AMediaMuxer_stop(mMuxer);
            mMuxerStarted = false;
        }
        AMediaMuxer_delete(mMuxer);
        mMuxer = nullptr;
    }

    if (mEncoderWindow) {
        ANativeWindow_release(mEncoderWindow);
        mEncoderWindow = nullptr;
    }

    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }

    // Clear audio queue
    std::lock_guard<std::mutex> lock(mAudioQueueMutex);
    while(!mAudioQueue.empty()) mAudioQueue.pop();
    mAudioFrameCounter = 0;

    {
        std::lock_guard<std::mutex> muxLock(mMuxerMutex);
        mBufferedPackets.clear();
    }

    LOGI("MediaEncoder stopped and resources released.");
}

ANativeWindow* MediaEncoder::getEncoderWindow() const {
    return mEncoderWindow;
}

void MediaEncoder::encodeAudioFrame(const uint8_t* data, int size) {
    if (!mAudioRunning) return;

    std::lock_guard<std::mutex> lock(mAudioQueueMutex);
    AudioBuffer buffer;
    buffer.data.assign(data, data + size);
    
    // Calculate timestamp based on total samples processed
    // PCM 16-bit Mono = 2 bytes per sample
    int numSamples = size / (2 * mChannelCount);
    int64_t frameUs = (mAudioFrameCounter * 1000000LL) / mSampleRate;
    buffer.timestampUs = frameUs;
    mAudioFrameCounter += numSamples;

    mAudioQueue.push(std::move(buffer));
    mAudioQueueCondVar.notify_one();
}

void MediaEncoder::videoEncoderLoop() {
    while (mVideoRunning) {
        drainVideo();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Flush remaining frames
    drainVideo();
}

void MediaEncoder::drainVideo() {
    if (!mVideoCodec) return;

    AMediaCodecBufferInfo info;
    ssize_t outBufId = AMediaCodec_dequeueOutputBuffer(mVideoCodec, &info, 1000);
    while (outBufId >= 0) {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            // Buffer contains config data instead of media, ignored as track format handles this
            AMediaCodec_releaseOutputBuffer(mVideoCodec, outBufId, false);
            outBufId = AMediaCodec_dequeueOutputBuffer(mVideoCodec, &info, 1000);
            continue;
        }

        size_t outSize;
        uint8_t* outBuf = AMediaCodec_getOutputBuffer(mVideoCodec, outBufId, &outSize);
        if (outBuf && info.size > 0) {
            std::lock_guard<std::mutex> lock(mMuxerMutex);
            if (mMuxerStarted) {
                AMediaMuxer_writeSampleData(mMuxer, mVideoTrackIndex, outBuf, &info);
            } else {
                BufferedPacket packet;
                packet.trackIndex = mVideoTrackIndex;
                packet.data.assign(outBuf + info.offset, outBuf + info.offset + info.size);
                packet.info = info;
                packet.info.offset = 0;
                mBufferedPackets.push_back(std::move(packet));
            }
        }

        AMediaCodec_releaseOutputBuffer(mVideoCodec, outBufId, false);
        outBufId = AMediaCodec_dequeueOutputBuffer(mVideoCodec, &info, 1000);
    }

    if (outBufId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        std::lock_guard<std::mutex> lock(mMuxerMutex);
        AMediaFormat* newFormat = AMediaCodec_getOutputFormat(mVideoCodec);
        mVideoTrackIndex = AMediaMuxer_addTrack(mMuxer, newFormat);
        AMediaFormat_delete(newFormat);
        LOGI("Video track added: %d", mVideoTrackIndex);

        // Start muxer if all enabled tracks are ready
        if (mVideoTrackIndex >= 0 && (!mAudioCodec || mAudioTrackIndex >= 0)) {
            AMediaMuxer_start(mMuxer);
            mMuxerStarted = true;
            LOGI("Muxer started from Video Track Change. Flushing %d buffered packets.", (int)mBufferedPackets.size());
            for (auto& packet : mBufferedPackets) {
                AMediaMuxer_writeSampleData(mMuxer, packet.trackIndex, packet.data.data(), &packet.info);
            }
            mBufferedPackets.clear();
        }
    }
}

void MediaEncoder::audioEncoderLoop() {
    while (mAudioRunning) {
        std::unique_lock<std::mutex> lock(mAudioQueueMutex);
        mAudioQueueCondVar.wait(lock, [this] { return !mAudioQueue.empty() || !mAudioRunning; });

        if (!mAudioRunning && mAudioQueue.empty()) break;

        AudioBuffer buf = std::move(mAudioQueue.front());
        mAudioQueue.pop();
        lock.unlock();

        // Feed input buffer
        ssize_t inBufId = AMediaCodec_dequeueInputBuffer(mAudioCodec, 2000);
        if (inBufId >= 0) {
            size_t inSize;
            uint8_t* inBuf = AMediaCodec_getInputBuffer(mAudioCodec, inBufId, &inSize);
            if (inBuf && inSize >= buf.data.size()) {
                std::memcpy(inBuf, buf.data.data(), buf.data.size());
                AMediaCodec_queueInputBuffer(mAudioCodec, inBufId, 0, buf.data.size(), buf.timestampUs, 0);
            } else {
                AMediaCodec_queueInputBuffer(mAudioCodec, inBufId, 0, 0, 0, 0);
            }
        }

        drainAudio();
    }
    // Flush remaining frames
    drainAudio();
}

void MediaEncoder::drainAudio() {
    if (!mAudioCodec) return;

    AMediaCodecBufferInfo info;
    ssize_t outBufId = AMediaCodec_dequeueOutputBuffer(mAudioCodec, &info, 1000);
    while (outBufId >= 0) {
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            AMediaCodec_releaseOutputBuffer(mAudioCodec, outBufId, false);
            outBufId = AMediaCodec_dequeueOutputBuffer(mAudioCodec, &info, 1000);
            continue;
        }

        size_t outSize;
        uint8_t* outBuf = AMediaCodec_getOutputBuffer(mAudioCodec, outBufId, &outSize);
        if (outBuf && info.size > 0) {
            std::lock_guard<std::mutex> lock(mMuxerMutex);
            if (mMuxerStarted) {
                AMediaMuxer_writeSampleData(mMuxer, mAudioTrackIndex, outBuf, &info);
            } else {
                BufferedPacket packet;
                packet.trackIndex = mAudioTrackIndex;
                packet.data.assign(outBuf + info.offset, outBuf + info.offset + info.size);
                packet.info = info;
                packet.info.offset = 0;
                mBufferedPackets.push_back(std::move(packet));
            }
        }

        AMediaCodec_releaseOutputBuffer(mAudioCodec, outBufId, false);
        outBufId = AMediaCodec_dequeueOutputBuffer(mAudioCodec, &info, 1000);
    }

    if (outBufId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        std::lock_guard<std::mutex> lock(mMuxerMutex);
        AMediaFormat* newFormat = AMediaCodec_getOutputFormat(mAudioCodec);
        mAudioTrackIndex = AMediaMuxer_addTrack(mMuxer, newFormat);
        AMediaFormat_delete(newFormat);
        LOGI("Audio track added: %d", mAudioTrackIndex);

        // Start muxer if all enabled tracks are ready
        if (mVideoTrackIndex >= 0 && mAudioTrackIndex >= 0) {
            AMediaMuxer_start(mMuxer);
            mMuxerStarted = true;
            LOGI("Muxer started from Audio Track Change. Flushing %d buffered packets.", (int)mBufferedPackets.size());
            for (auto& packet : mBufferedPackets) {
                AMediaMuxer_writeSampleData(mMuxer, packet.trackIndex, packet.data.data(), &packet.info);
            }
            mBufferedPackets.clear();
        }
    }
}
