#ifndef MEDIA_ENCODER_H
#define MEDIA_ENCODER_H

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>

/**
 * NDK Hardware Video and Audio Encoder.
 * Uses MediaCodec to encode video frames rendered by GlRenderer
 * and audio frames pushed from Java. Outputs an MP4 file.
 */
class MediaEncoder {
public:
    MediaEncoder();
    ~MediaEncoder();

    bool configure(int fd, int width, int height, int bitrate, int fps, int sampleRate = 44100, int channelCount = 1);
    bool start();
    void stop();

    ANativeWindow* getEncoderWindow() const;
    void encodeAudioFrame(const uint8_t* data, int size);

private:
    void videoEncoderLoop();
    void audioEncoderLoop();
    void drainVideo();
    void drainAudio();

    // File writing & muxing
    int mFd = -1;
    AMediaMuxer* mMuxer = nullptr;
    std::mutex mMuxerMutex;
    std::atomic<bool> mMuxerStarted{false};
    int mVideoTrackIndex = -1;
    int mAudioTrackIndex = -1;

    // Video codec
    AMediaCodec* mVideoCodec = nullptr;
    AMediaFormat* mVideoFormat = nullptr;
    ANativeWindow* mEncoderWindow = nullptr;
    std::thread mVideoThread;
    std::atomic<bool> mVideoRunning{false};

    // Audio codec
    AMediaCodec* mAudioCodec = nullptr;
    AMediaFormat* mAudioFormat = nullptr;
    std::thread mAudioThread;
    std::atomic<bool> mAudioRunning{false};

    // Audio input queue
    struct AudioBuffer {
        std::vector<uint8_t> data;
        int64_t timestampUs;
    };
    std::queue<AudioBuffer> mAudioQueue;
    std::mutex mAudioQueueMutex;
    std::condition_variable mAudioQueueCondVar;
    std::atomic<int64_t> mAudioFrameCounter{0};
    int mSampleRate = 44100;
    int mChannelCount = 1;

    struct BufferedPacket {
        int trackIndex;
        std::vector<uint8_t> data;
        AMediaCodecBufferInfo info;
    };
    std::vector<BufferedPacket> mBufferedPackets;
};

#endif // MEDIA_ENCODER_H
