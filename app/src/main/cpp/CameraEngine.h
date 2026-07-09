#ifndef CAMERA_ENGINE_H
#define CAMERA_ENGINE_H

#include <jni.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/sensor.h>
#include <android/looper.h>
#include <android/log.h>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <string>
#include <vector>

#include "GlRenderer.h"
#include "MediaEncoder.h"
#include "PidController.h"

struct CameraLensInfo {
    std::string id;
    float focalLength;
    int facing;
};

class CameraEngine {
public:
    CameraEngine(JavaVM* vm);
    ~CameraEngine();

    bool initCamera(JNIEnv* env, ANativeWindow* previewWindow, int width, int height, bool useStabilization);
    void startRecording(int fd, int rotationDegrees = 0);
    void stopRecording();
    void setStabilization(bool enabled);
    void setOis(bool enabled);
    bool isStabilizationActive();
    void setAeMode(int mode);
    void setAntiFlicker(int hz);
    void lockAe(bool locked);
    void releaseCamera();
    
    // UI controls
    void setMode(bool isAuto); // Auto vs Manual mode
    void setFocus(bool autoFocus, float distance);
    void setExposure(bool autoExposure, int32_t iso, int64_t shutterSpeedNs);
    void setFrameRate(int32_t fps);
    void setZoom(float ratio);
    void setNoiseReduction(uint8_t mode);
    void setFocusPoint(float x, float y, int viewWidth, int viewHeight);
    void setExposureCompensation(int32_t value);
    void pushAudioFrame(const uint8_t* data, int size);
    void setLens(const std::string& lensId);
    std::vector<CameraLensInfo> getAvailableLenses();
    void setHdrEnabled(bool enabled);
    

    // Callback notification
    void notifyFrameAvailable();

    // Internal callback handlers
    void onDeviceStateChange(ACameraDevice* device, int state);
    void onSessionStateChange(ACameraCaptureSession* session, int state);

private:
    bool findRearCamera();
    bool selectLensById(const std::string& id);
    bool createCaptureSession();
    bool configureCaptureRequest();
    void applyExposureSettings();
    void updateRepeatingRequest();
    void closeCamera();
    
    // Loop control
    void startLoopThread();
    void stopLoopThread();
    void cameraLoop();
    void initGyroscope();
    void releaseGyroscope();

    // Java VM and JNI
    JavaVM* mJavaVM = nullptr;
    jobject mSurfaceTextureRef = nullptr;
    jmethodID mUpdateTexImageMethod = nullptr;
    jmethodID mGetTimestampMethod = nullptr;

    // Camera NDK objects
    ACameraManager* mCameraManager = nullptr;
    ACameraIdList* mCameraIdList = nullptr;
    std::string mCameraId;
    ACameraDevice* mCameraDevice = nullptr;
    ACameraCaptureSession* mCaptureSession = nullptr;
    ANativeWindow* mCameraWindow = nullptr; // Camera target window

    // Session outputs
    ACaptureSessionOutputContainer* mOutputContainer = nullptr;
    ACaptureSessionOutput* mCameraOutput = nullptr;
    ACameraOutputTarget* mCameraTarget = nullptr;
    ACaptureRequest* mCaptureRequest = nullptr;

    // Renderers & Encoders
    std::unique_ptr<GlRenderer> mGlRenderer;
    std::unique_ptr<MediaEncoder> mMediaEncoder;
    std::unique_ptr<PidController> mPidController;
    float mFilteredLuma = 0.47f;

    // Settings
    bool mIsAutoMode = true;
    bool mUseStabilization = false;
    bool mUseOis = false;
    int mAeMode = 0; // 0 = Custom Cinema, 1 = Hardware Default, 2 = Cinematic Portrait (Face Priority)
    int mAntiFlickerHz = 0; // 0 = off, 50 = 50Hz, 60 = 60Hz
    int mActiveLensFacing = 1; // 0 = front, 1 = back, 2 = external
    int mWidth = 1920;
    int mHeight = 1080;
    int32_t mTargetFps = 60;
    
    bool mAutoFocus = true;
    float mFocusDistance = 0.0f; // 0.0f = infinity
    
    bool mAutoExposure = true;
    int32_t mIso = 400;
    int64_t mShutterSpeedNs = 16666666; // 1/60s
    int32_t mExposureCompensation = 0;
    uint8_t mNoiseReductionMode = 1; // Fast
    float mZoomRatio = 1.0f;
    bool mAeLocked = false; // AE/AF lock: freezes auto exposure and autofocus in auto mode
    int  mVideoRotation = 0; // Rotation hint in degrees for the MP4 container (0, 90, 180, 270)
    bool mHdrEnabled = false;

    // Loop & Synchronization
    std::thread mLoopThread;
    std::atomic<bool> mIsLoopRunning{false};
    std::atomic<bool> mIsRecording{false};

    std::mutex mFrameMutex;
    std::condition_variable mFrameCondVar;
    bool mFrameAvailable = false;

    // Gyroscope EIS fields
    ASensorManager* mSensorManager = nullptr;
    const ASensor* mGyroSensor = nullptr;
    ASensorEventQueue* mSensorEventQueue = nullptr;
    ALooper* mLooper = nullptr;
    int64_t mLastGyroTimestamp = 0;
    float mGyroX = 0.0f;
    float mGyroY = 0.0f;

    // Exposure calculations
    std::mutex mCameraMutex;
    float mLastLuma = 0.5f; // Initial luminance guess (neutral)

    // Cached lenses list
    std::vector<CameraLensInfo> mLenses;
};

#endif // CAMERA_ENGINE_H
