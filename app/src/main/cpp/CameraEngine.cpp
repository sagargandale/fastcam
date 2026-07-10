#include "CameraEngine.h"
#include <camera/NdkCameraMetadataTags.h>
#include <cmath>
#include <algorithm>

#define LOG_TAG "[NativeCameraEngine]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Static state callbacks for ACameraDevice
static void onDeviceDisconnected(void* context, ACameraDevice* device) {
    (void)context;
    LOGI("Camera device %s disconnected.", ACameraDevice_getId(device));
}

static void onDeviceError(void* context, ACameraDevice* device, int error) {
    (void)context;
    LOGE("Camera device %s error: %d", ACameraDevice_getId(device), error);
}

// Static state callbacks for ACameraCaptureSession
static void onSessionActive(void* context, ACameraCaptureSession* session) {
    (void)context; (void)session;
    LOGI("Camera capture session active.");
}

static void onSessionReady(void* context, ACameraCaptureSession* session) {
    (void)context; (void)session;
    LOGI("Camera capture session ready.");
}

static void onSessionClosed(void* context, ACameraCaptureSession* session) {
    (void)context; (void)session;
    LOGI("Camera capture session closed.");
}

CameraEngine::CameraEngine(JavaVM* vm) : mJavaVM(vm) {
    mCameraManager = ACameraManager_create();
    // Initialize PID Controller: Kp, Ki, Kd, Target Luminance
    mPidController = std::make_unique<PidController>(1.8f, 0.4f, 0.1f, 0.47f); 
}

CameraEngine::~CameraEngine() {
    releaseCamera();
    if (mCameraManager) {
        ACameraManager_delete(mCameraManager);
        mCameraManager = nullptr;
    }
}

bool CameraEngine::initCamera(JNIEnv* env, ANativeWindow* previewWindow, int width, int height, bool useStabilization) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mWidth = width;
    mHeight = height;
    mUseStabilization = useStabilization;

    // 1. Initialize EGL & OpenGL
    mGlRenderer = std::make_unique<GlRenderer>();
    if (!mGlRenderer->initEgl(previewWindow)) {
        LOGE("Failed to initialize EGL!");
        return false;
    }

    if (!mGlRenderer->setupGl(width, height)) {
        LOGE("Failed to setup OpenGL ES rendering pipeline!");
        return false;
    }

    GLuint textureId = mGlRenderer->getCameraTextureId();

    // 2. Instantiate Java SurfaceTexture: new SurfaceTexture(textureId)
    jclass stClass = env->FindClass("android/graphics/SurfaceTexture");
    jmethodID stInit = env->GetMethodID(stClass, "<init>", "(I)V");
    jobject stObj = env->NewObject(stClass, stInit, textureId);
    if (!stObj) {
        LOGE("Failed to instantiate Java SurfaceTexture object!");
        env->DeleteLocalRef(stClass);
        return false;
    }

    mSurfaceTextureRef = env->NewGlobalRef(stObj);
    mUpdateTexImageMethod = env->GetMethodID(stClass, "updateTexImage", "()V");
    mGetTimestampMethod = env->GetMethodID(stClass, "getTimestamp", "()J");

    jmethodID setDefaultBufferSizeMethod = env->GetMethodID(stClass, "setDefaultBufferSize", "(II)V");
    if (setDefaultBufferSizeMethod) {
        env->CallVoidMethod(mSurfaceTextureRef, setDefaultBufferSizeMethod, width, height);
        LOGI("SurfaceTexture default buffer size set to: %dx%d", width, height);
    } else {
        LOGE("Failed to find setDefaultBufferSize method!");
    }

    // 3. Instantiate Java Surface: new Surface(surfaceTexture)
    jclass surfaceClass = env->FindClass("android/view/Surface");
    jmethodID surfaceInit = env->GetMethodID(surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    jobject cameraSurfaceObj = env->NewObject(surfaceClass, surfaceInit, mSurfaceTextureRef);

    mCameraWindow = ANativeWindow_fromSurface(env, cameraSurfaceObj);

    env->DeleteLocalRef(stClass);
    env->DeleteLocalRef(stObj);
    env->DeleteLocalRef(surfaceClass);
    env->DeleteLocalRef(cameraSurfaceObj);

    if (!mCameraWindow) {
        LOGE("Failed to create ANativeWindow from camera Surface!");
        return false;
    }

    // Configure camera device
    if (mCameraId.empty() && !findRearCamera()) {
        LOGE("Could not locate rear camera sensor!");
        return false;
    }

    ACameraDevice_StateCallbacks deviceCallbacks{
        .context = this,
        .onDisconnected = onDeviceDisconnected,
        .onError = onDeviceError
    };

    camera_status_t status = ACameraManager_openCamera(
        mCameraManager, 
        mCameraId.c_str(), 
        &deviceCallbacks, 
        &mCameraDevice
    );

    if (status != ACAMERA_OK || !mCameraDevice) {
        LOGE("Failed to open camera device! Status: %d", status);
        return false;
    }

    if (!createCaptureSession()) {
        LOGE("Failed to instantiate capture session!");
        return false;
    }

    mGlRenderer->unbindEgl();
    startLoopThread();

    LOGI("CameraEngine initialization complete.");
    return true;
}

bool CameraEngine::findRearCamera() {
    camera_status_t status = ACameraManager_getCameraIdList(mCameraManager, &mCameraIdList);
    if (status != ACAMERA_OK || !mCameraIdList) {
        LOGE("Failed to get camera list: %d", status);
        return false;
    }

    mLenses.clear();
    for (int i = 0; i < mCameraIdList->numCameras; ++i) {
        const char* id = mCameraIdList->cameraIds[i];
        ACameraMetadata* chars = nullptr;
        ACameraManager_getCameraCharacteristics(mCameraManager, id, &chars);

        ACameraMetadata_const_entry facingEntry;
        ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &facingEntry);
        auto facing = facingEntry.data.u8[0];

        ACameraMetadata_const_entry focalLengthsEntry;
        float focalLength = 4.0f; // Default fallback
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalLengthsEntry) == ACAMERA_OK) {
            focalLength = focalLengthsEntry.data.f[0];
        }

        CameraLensInfo info{
            .id = id,
            .focalLength = focalLength,
            .facing = facing
        };
        mLenses.push_back(info);

        // Pick default rear camera (first rear one found)
        if (facing == ACAMERA_LENS_FACING_BACK && mCameraId.empty()) {
            mCameraId = id;
            mActiveLensFacing = facing;

            // Cache sensor active array size once to avoid repeated IPC calls
            ACameraMetadata_const_entry arrayEntry;
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &arrayEntry) == ACAMERA_OK
                    && arrayEntry.count == 4) {
                mSensorArrayLeft   = arrayEntry.data.i32[0];
                mSensorArrayTop    = arrayEntry.data.i32[1];
                mSensorArrayWidth  = arrayEntry.data.i32[2] - arrayEntry.data.i32[0];
                mSensorArrayHeight = arrayEntry.data.i32[3] - arrayEntry.data.i32[1];
                LOGI("Sensor active array: left=%d top=%d w=%d h=%d",
                     mSensorArrayLeft, mSensorArrayTop, mSensorArrayWidth, mSensorArrayHeight);
            }

            // Compute EIS field-of-view from physical sensor size + focal length.
            // fov = 2 * atan(sensor_dimension / (2 * focal_length_mm))
            ACameraMetadata_const_entry physEntry, focalEntry;
            if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &physEntry) == ACAMERA_OK
                    && physEntry.count == 2
                    && ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalEntry) == ACAMERA_OK
                    && focalEntry.count >= 1) {
                float sensorW_mm  = physEntry.data.f[0];
                float sensorH_mm  = physEntry.data.f[1];
                float focalLen_mm = focalEntry.data.f[0];
                mEisFovX = 2.0f * std::atan(sensorW_mm / (2.0f * focalLen_mm));
                mEisFovY = 2.0f * std::atan(sensorH_mm / (2.0f * focalLen_mm));
                LOGI("EIS FOV calibrated: %.1f\u00b0 x %.1f\u00b0 (sensor %.2fx%.2fmm, f=%.2fmm)",
                     mEisFovX * 180.0f / M_PI, mEisFovY * 180.0f / M_PI,
                     sensorW_mm, sensorH_mm, focalLen_mm);
            }
        }

        ACameraMetadata_free(chars);
    }
    return !mCameraId.empty();
}

std::vector<CameraLensInfo> CameraEngine::getAvailableLenses() {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    if (mLenses.empty()) {
        findRearCamera();
    }
    return mLenses;
}

void CameraEngine::setLens(const std::string& lensId) {
    if (mCameraId == lensId) return;

    LOGI("Switching lens to: %s", lensId.c_str());

    // Stop the loop thread BEFORE acquiring mCameraMutex to prevent deadlock.
    // cameraLoop() acquires mCameraMutex internally for AE updates; joining
    // while holding it would deadlock the camera and UI threads.
    stopLoopThread();

    std::lock_guard<std::mutex> lock(mCameraMutex);

    mCameraId = lensId;
    // Update active lens facing direction and refresh sensor array cache
    for (const auto& lens : mLenses) {
        if (lens.id == lensId) {
            mActiveLensFacing = lens.facing;
            break;
        }
    }
    // Refresh cached sensor array and EIS FOV for the newly selected lens
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(mCameraManager, mCameraId.c_str(), &chars) == ACAMERA_OK && chars) {
        ACameraMetadata_const_entry arrayEntry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &arrayEntry) == ACAMERA_OK
                && arrayEntry.count == 4) {
            mSensorArrayLeft   = arrayEntry.data.i32[0];
            mSensorArrayTop    = arrayEntry.data.i32[1];
            mSensorArrayWidth  = arrayEntry.data.i32[2] - arrayEntry.data.i32[0];
            mSensorArrayHeight = arrayEntry.data.i32[3] - arrayEntry.data.i32[1];
        }
        ACameraMetadata_const_entry physEntry, focalEntry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &physEntry) == ACAMERA_OK
                && physEntry.count == 2
                && ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalEntry) == ACAMERA_OK
                && focalEntry.count >= 1) {
            mEisFovX = 2.0f * std::atan(physEntry.data.f[0] / (2.0f * focalEntry.data.f[0]));
            mEisFovY = 2.0f * std::atan(physEntry.data.f[1] / (2.0f * focalEntry.data.f[0]));
            LOGI("EIS FOV refreshed for lens %s: %.1f\u00b0 x %.1f\u00b0",
                 lensId.c_str(), mEisFovX * 180.0f / M_PI, mEisFovY * 180.0f / M_PI);
        }
        // Reset EIS angle state on lens switch
        mEisAngleX = mEisAngleY = 0.0f;
        mEisSmoothedX = mEisSmoothedY = 0.0f;
        mLastGyroTimestamp = 0;
        ACameraMetadata_free(chars);
    }

    // If camera is already open, recreate session dynamically
    if (mCameraDevice) {
        if (mCaptureSession) {
            ACameraCaptureSession_stopRepeating(mCaptureSession);
            ACameraCaptureSession_close(mCaptureSession);
            mCaptureSession = nullptr;
        }
        ACameraDevice_close(mCameraDevice);
        mCameraDevice = nullptr;

        // Open new camera lens device
        ACameraDevice_StateCallbacks deviceCallbacks{
            .context = this,
            .onDisconnected = onDeviceDisconnected,
            .onError = onDeviceError
        };
        ACameraManager_openCamera(mCameraManager, mCameraId.c_str(), &deviceCallbacks, &mCameraDevice);
        createCaptureSession();
        startLoopThread();
    }
}

bool CameraEngine::createCaptureSession() {
    camera_status_t status = ACaptureSessionOutputContainer_create(&mOutputContainer);
    if (status != ACAMERA_OK) return false;

    status = ACaptureSessionOutput_create(mCameraWindow, &mCameraOutput);
    if (status != ACAMERA_OK) return false;
    ACaptureSessionOutputContainer_add(mOutputContainer, mCameraOutput);

    status = ACameraOutputTarget_create(mCameraWindow, &mCameraTarget);
    if (status != ACAMERA_OK) return false;

    status = ACameraDevice_createCaptureRequest(mCameraDevice, TEMPLATE_RECORD, &mCaptureRequest);
    if (status != ACAMERA_OK) return false;
    ACaptureRequest_addTarget(mCaptureRequest, mCameraTarget);

    if (!configureCaptureRequest()) {
        LOGW("Failed to apply initial manual capture request parameters.");
    }

    ACameraCaptureSession_stateCallbacks sessionCallbacks{};
    sessionCallbacks.context = this;
    sessionCallbacks.onClosed = onSessionClosed;
    sessionCallbacks.onReady = onSessionReady;
    sessionCallbacks.onActive = onSessionActive;

    // Log OIS support for this camera
    ACameraMetadata* chars = nullptr;
    ACameraManager_getCameraCharacteristics(mCameraManager, mCameraId.c_str(), &chars);
    ACameraMetadata_const_entry oisEntry;
    bool hardwareOisSupported = false;
    if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, &oisEntry) == ACAMERA_OK) {
        for (uint32_t i = 0; i < oisEntry.count; i++) {
            if (oisEntry.data.u8[i] == ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON) {
                hardwareOisSupported = true;
                break;
            }
        }
    }
    LOGI("Camera %s OIS support check: Available = %d, Requested = %d", 
         mCameraId.c_str(), hardwareOisSupported, mUseOis);
    ACameraMetadata_free(chars);

    status = ACameraDevice_createCaptureSession(
        mCameraDevice, 
        mOutputContainer, 
        &sessionCallbacks, 
        &mCaptureSession
    );

    if (status == ACAMERA_OK) {
        updateRepeatingRequest();
        return true;
    }
    return false;
}

bool CameraEngine::configureCaptureRequest() {
    if (!mCaptureRequest) return false;

    // Set stable target fps (e.g. 60)
    int32_t fpsRange[2] = {mTargetFps, mTargetFps};
    ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);

    int64_t frameDurationNs = 1000000000LL / mTargetFps;
    ACaptureRequest_setEntry_i64(mCaptureRequest, ACAMERA_SENSOR_FRAME_DURATION, 1, &frameDurationNs);

    // Disable stock video stabilization and configure OIS based on settings
    uint8_t videoStabMode = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    uint8_t oisMode = mUseOis ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &videoStabMode);
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &oisMode);

    // Focus controls
    if (mIsAutoMode) {
        uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
    } else {
        if (mAutoFocus) {
            uint8_t afMode = ACAMERA_CONTROL_AF_MODE_AUTO;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        } else {
            uint8_t afMode = ACAMERA_CONTROL_AF_MODE_OFF;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
            ACaptureRequest_setEntry_float(mCaptureRequest, ACAMERA_LENS_FOCUS_DISTANCE, 1, &mFocusDistance);
        }
    }

    // Exposure controls
    applyExposureSettings();

    // Noise Reduction mode
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_NOISE_REDUCTION_MODE, 1, &mNoiseReductionMode);

    // Anti-banding (flicker) configuration for hardware AE modes
    // For Custom Cinema (mode 0), we snap shutter speeds manually in cameraLoop
    // For Hardware AE and Portrait modes, let hardware ISP handle anti-banding
    if (mAeMode >= 1) {
        uint8_t antibanding;
        if (mAntiFlickerHz == 50) {
            antibanding = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ;
        } else if (mAntiFlickerHz == 60) {
            antibanding = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ;
        } else {
            antibanding = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
        }
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &antibanding);
    } else {
        // Custom Cinema: disable hardware anti-banding, we snap manually
        uint8_t antibanding = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &antibanding);
    }

    return true;
}

void CameraEngine::applyExposureSettings() {
    if (!mCaptureRequest) return;

    if (mIsAutoMode) {
        if (mAeMode == 1) {
            // Hardware Default: Standard auto-exposure
            uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
            uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_MODE, 1, &controlMode);
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
        } else if (mAeMode == 2) {
            // Cinematic Portrait: Face priority auto-exposure
            uint8_t controlMode = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
            uint8_t sceneMode = ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY;
            uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_MODE, 1, &controlMode);
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_SCENE_MODE, 1, &sceneMode);
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
        } else {
            // Custom Cinema: Manual exposure control driven by our logarithmic PID target
            uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
            uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_MODE, 1, &controlMode);
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
            
            // Dynamically update PID target based on exposure compensation value
            float baseTarget = mHdrEnabled ? 0.33f : 0.47f;
            float newTarget = baseTarget * std::pow(1.5f, (float)mExposureCompensation);
            mPidController->setTarget(std::max(0.05f, std::min(newTarget, 0.95f)));
            
            ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &mIso);
            ACaptureRequest_setEntry_i64(mCaptureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &mShutterSpeedNs);
        }
    } else {
        // In Manual Screen: Reset controlMode to AUTO to disable scenes, and apply manual focus/exposure
        uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_MODE, 1, &controlMode);

        if (mAutoExposure) {
            if (mAeMode == 1) {
                uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
                ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
            } else if (mAeMode == 2) {
                // Keep face priority enabled in manual screen if Auto-Exposure toggle is checked
                uint8_t sceneControl = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
                uint8_t sceneMode = ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY;
                uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
                ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_MODE, 1, &sceneControl);
                ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_SCENE_MODE, 1, &sceneMode);
                ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
            } else {
                uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
                ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
                
                // Dynamically update PID target based on exposure compensation value
                float baseTarget = mHdrEnabled ? 0.33f : 0.47f;
                float newTarget = baseTarget * std::pow(1.5f, (float)mExposureCompensation);
                mPidController->setTarget(std::max(0.05f, std::min(newTarget, 0.95f)));
                
                ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &mIso);
                ACaptureRequest_setEntry_i64(mCaptureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &mShutterSpeedNs);
            }
        } else {
            // Full manual exposure settings
            uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
            
            ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &mIso);
            ACaptureRequest_setEntry_i64(mCaptureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &mShutterSpeedNs);
        }
    }
}

void CameraEngine::updateRepeatingRequest() {
    if (mCaptureSession && mCaptureRequest) {
        ACameraCaptureSession_captureCallbacks captureCallbacks{
            .context = this,
            .onCaptureStarted = nullptr,
            .onCaptureProgressed = nullptr,
            .onCaptureCompleted = [](void* context, ACameraCaptureSession* session, ACaptureRequest* request, const ACameraMetadata* result) {
                (void)session; (void)request; (void)result;
                auto* engine = static_cast<CameraEngine*>(context);
                engine->notifyFrameAvailable();
            },
            .onCaptureFailed = nullptr,
            .onCaptureSequenceCompleted = nullptr,
            .onCaptureSequenceAborted = nullptr,
            .onCaptureBufferLost = nullptr
        };
        ACameraCaptureSession_setRepeatingRequest(mCaptureSession, &captureCallbacks, 1, &mCaptureRequest, nullptr);
    }
}

void CameraEngine::setMode(bool isAuto) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mIsAutoMode = isAuto;
    if (mIsAutoMode) {
        mAutoFocus = true;
        mAutoExposure = true;
    }
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::notifyFrameAvailable() {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    mFrameAvailable = true;
    mFrameCondVar.notify_one();
}

void CameraEngine::startRecording(int fd, int rotationDegrees) {
    std::lock_guard<std::mutex> cameraLock(mCameraMutex);
    if (mIsRecording) return;

    LOGI("Starting hardware recording to fd: %d (Res: %dx%d, rotation: %d)", fd, mWidth, mHeight, rotationDegrees);

    mVideoRotation = rotationDegrees;
    auto encoder = std::make_unique<MediaEncoder>();
    // Set 60 Mbps for 4K / 35 Mbps for 1080p for exceptional detail retention
    int bitrate = (mWidth >= 3840) ? 60000000 : 35000000;

    if (encoder->configure(fd, mWidth, mHeight, bitrate, mTargetFps, rotationDegrees, 44100, 2)) {
        encoder->start();
        mGlRenderer->createEncoderSurface(encoder->getEncoderWindow());
        {
            std::lock_guard<std::mutex> encLock(mEncoderMutex);
            mMediaEncoder = std::move(encoder);
        }
        mIsRecording = true;
    } else {
        LOGE("Failed to configure hardware MediaEncoder!");
        close(fd);
    }
}

void CameraEngine::stopRecording() {
    std::lock_guard<std::mutex> cameraLock(mCameraMutex);
    if (!mIsRecording) return;

    LOGI("Stopping hardware recording.");
    mIsRecording = false;

    std::unique_ptr<MediaEncoder> enc;
    {
        std::lock_guard<std::mutex> encLock(mEncoderMutex);
        enc = std::move(mMediaEncoder); // Transfer ownership; mMediaEncoder is now null
    }
    if (enc) {
        enc->stop();
        mGlRenderer->destroyEncoderSurface();
    }
}

void CameraEngine::setStabilization(bool enabled) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mUseStabilization = enabled;
    if (enabled && mUseOis) {
        // EIS and OIS are mutually exclusive — disable hardware OIS when EIS is requested.
        // Software EIS does its own stabilisation via the gyro; hardware OIS would fight it.
        mUseOis = false;
        LOGI("EIS enabled — hardware OIS disabled (mutually exclusive).");
    }
    // Reset accumulated EIS angle so there is no jump when re-enabling
    mEisAngleX = mEisAngleY = 0.0f;
    mEisSmoothedX = mEisSmoothedY = 0.0f;
    mLastGyroTimestamp = 0;
    // Crop: 15% headroom (scale=0.85) for EIS, full sensor (scale=1.0) when off
    if (mGlRenderer) {
        mGlRenderer->setEisCropScale(mUseStabilization ? 0.85f : 1.0f);
    }
    LOGI("Custom EIS stabilization: %s", enabled ? "ENABLED" : "DISABLED");
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::setOis(bool enabled) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mUseOis = enabled;
    if (enabled && mUseStabilization) {
        // OIS and EIS are mutually exclusive — disable EIS when hardware OIS is requested.
        mUseStabilization = false;
        mEisAngleX = mEisAngleY = 0.0f;
        mEisSmoothedX = mEisSmoothedY = 0.0f;
        mLastGyroTimestamp = 0;
        LOGI("OIS enabled — custom EIS disabled (mutually exclusive).");
    }
    // OIS uses the full sensor readout — no artificial crop needed.
    if (mGlRenderer) {
        mGlRenderer->setEisCropScale(mUseStabilization ? 0.85f : 1.0f);
    }
    LOGI("Hardware OIS: %s", enabled ? "ENABLED" : "DISABLED");
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

bool CameraEngine::isStabilizationActive() {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    return mUseStabilization && (mGyroSensor != nullptr);
}

void CameraEngine::setAeMode(int mode) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mAeMode = mode;
    LOGI("AE mode updated: mAeMode = %d", mode);
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::setAntiFlicker(int hz) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mAntiFlickerHz = hz;
    LOGI("Anti-flicker frequency: %d Hz", hz);
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::lockAe(bool locked) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mAeLocked = locked;
    LOGI("AE/AF lock: %s", locked ? "LOCKED" : "UNLOCKED");

    if (mCaptureRequest) {
        if (locked) {
            // Hardware AE lock: freeze hardware AE state at current value
            uint8_t aeLock = ACAMERA_CONTROL_AE_LOCK_ON;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);
            // Trigger AF to lock focus at current position
            uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_START;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afTrigger);
        } else {
            // Hardware AE unlock: resume normal AE
            uint8_t aeLock = ACAMERA_CONTROL_AE_LOCK_OFF;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);
            // Cancel AF trigger to resume continuous autofocus
            uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
            ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afTrigger);
        }
        if (mCaptureSession) {
            updateRepeatingRequest();
        }
    }
}

void CameraEngine::setHdrEnabled(bool enabled) {
    LOGI("HDR mode: %s", enabled ? "ENABLED" : "DISABLED");
    if (mGlRenderer) {
        mGlRenderer->setHdrEnabled(enabled);
    }
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mHdrEnabled = enabled;
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::startLoopThread() {
    mIsLoopRunning = true;
    mLoopThread = std::thread(&CameraEngine::cameraLoop, this);
    LOGI("Synchronous camera capture and render loop started.");
}

void CameraEngine::stopLoopThread() {
    mIsLoopRunning = false;
    
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mFrameCondVar.notify_all();
    }

    if (mLoopThread.joinable()) {
        mLoopThread.join();
    }
    LOGI("Synchronous camera capture loop stopped.");
}

void CameraEngine::initGyroscope() {
    mSensorManager = ASensorManager_getInstanceForPackage("com.fastcam");
    if (!mSensorManager) return;

    mGyroSensor = ASensorManager_getDefaultSensor(mSensorManager, ASENSOR_TYPE_GYROSCOPE);
    if (!mGyroSensor) {
        LOGW("No hardware gyroscope detected. EIS stabilization disabled.");
        return;
    }

    mLooper = ALooper_prepare(0);
    mSensorEventQueue = ASensorManager_createEventQueue(mSensorManager, mLooper, 0, nullptr, nullptr);
    ASensorEventQueue_enableSensor(mSensorEventQueue, mGyroSensor);

    // 200Hz (5ms) — fast enough to capture hand-shake frequencies (1–10Hz) with good temporal
    // resolution for accurate Euler integration. The render loop drains ALL queued events.
    ASensorEventQueue_setEventRate(mSensorEventQueue, mGyroSensor, 5000);

    mLastGyroTimestamp = 0; // 0 = no sample yet; first event will be skipped to seed the timestamp
    mEisAngleX = mEisAngleY = 0.0f;
    mEisSmoothedX = mEisSmoothedY = 0.0f;
    LOGI("EIS Gyroscope initialised at 200 Hz.");
}

void CameraEngine::releaseGyroscope() {
    if (mSensorEventQueue) {
        ASensorEventQueue_disableSensor(mSensorEventQueue, mGyroSensor);
        ASensorManager_destroyEventQueue(mSensorManager, mSensorEventQueue);
        mSensorEventQueue = nullptr;
    }
    mGyroSensor = nullptr;
    mSensorManager = nullptr;
    LOGI("EIS Gyroscope released.");
}

void CameraEngine::cameraLoop() {
    JNIEnv* env = nullptr;
    mJavaVM->AttachCurrentThread(&env, nullptr);

    mGlRenderer->bindEgl();
    initGyroscope();

    auto nextFrameTime = std::chrono::steady_clock::now();
    int aeFrameCounter = 0;
    
    while (mIsLoopRunning) {
        // Recompute interval each iteration so FPS changes take effect immediately
        int frameIntervalUs = 1000000 / std::max(1, (int)mTargetFps);
        nextFrameTime += std::chrono::microseconds(frameIntervalUs);

        // 1. EIS — integrate all queued gyroscope events and compute UV stabilisation shift
        float shiftX = 0.0f;
        float shiftY = 0.0f;

        if (mUseStabilization && mSensorEventQueue) {
            ASensorEvent event;
            // Drain ALL events queued since the last frame (gyro runs at 200Hz, camera at 60fps)
            while (ASensorEventQueue_getEvents(mSensorEventQueue, &event, 1) > 0) {
                if (event.type != ASENSOR_TYPE_GYROSCOPE) continue;

                if (mLastGyroTimestamp == 0) {
                    // First sample — seed the timestamp but do not integrate (dt unknown)
                    mLastGyroTimestamp = event.timestamp;
                    continue;
                }

                float dt = (event.timestamp - mLastGyroTimestamp) * 1e-9f;
                mLastGyroTimestamp = event.timestamp;

                // Reject stale, duplicate, or out-of-order samples
                if (dt <= 0.0f || dt > 0.05f) continue;

                // Euler integration: angular velocity (rad/s) × dt → angular displacement (rad)
                // Sensor axes (Android body frame):
                //   data[0] = rotation around X (pitch: tilt up/down) → shifts image vertically
                //   data[1] = rotation around Y (roll: tilt left/right) → shifts image horizontally
                //   data[2] = rotation around Z (yaw: spin) → not useful for 2-axis EIS
                mEisAngleX += event.data[1] * dt;  // Y-axis gyro → horizontal UV shift
                mEisAngleY += event.data[0] * dt;  // X-axis gyro → vertical UV shift
            }

            // High-pass filter: separate intentional panning (low-freq) from camera shake (high-freq)
            // Smoothed tracks the slow trend; the difference is the shake we must counteract.
            // Time constant tau = 0.5 s: movements slower than 0.5 s are treated as intentional pan.
            const float tau   = 0.5f;
            const float dt_fr = 1.0f / std::max(24, (int)mTargetFps);
            const float alpha = std::exp(-dt_fr / tau); // ~0.967 at 60fps

            mEisSmoothedX = alpha * mEisSmoothedX + (1.0f - alpha) * mEisAngleX;
            mEisSmoothedY = alpha * mEisSmoothedY + (1.0f - alpha) * mEisAngleY;

            // Shake component = total angle minus the slow pan trend
            float shakeX = mEisAngleX - mEisSmoothedX;
            float shakeY = mEisAngleY - mEisSmoothedY;

            // Convert angular shake (rad) → normalised UV shift using calibrated FOV.
            // shiftUV = shakeAngle / FOV.  FOV > 0 guaranteed by initialiser defaults.
            float rawX = shakeX / mEisFovX;
            float rawY = shakeY / mEisFovY;

            // Clamp to \u00b17.5% UV headroom (crop scale 0.85 → 15% total headroom → \u00b17.5%)
            const float kMaxShift = 0.075f;
            shiftX = std::max(-kMaxShift, std::min(rawX, kMaxShift));
            shiftY = std::max(-kMaxShift, std::min(rawY, kMaxShift));
        }
        // When EIS is OFF (or OIS is used), shiftX/Y remain 0 and the shader uses full sensor readout


        // Update filtered average luminance to reject high-frequency noise
        if (mLastLuma > 0.0f) {
            mFilteredLuma = mFilteredLuma * 0.8f + mLastLuma * 0.2f;
        }

        // 2. Auto exposure logic (runs in Auto mode or when auto exposure is selected in manual mode)
        if ((mIsAutoMode || mAutoExposure) && mAeMode == 0 && !mAeLocked) {
            aeFrameCounter++;
            if (aeFrameCounter >= 4) { // Update every 4 frames to compensate for hardware pipeline delay
                aeFrameCounter = 0;
                
                float targetLuma = mPidController->getTarget();
                float currentLuma = std::max(0.01f, mFilteredLuma);

                // Calculate required exposure change in EV stops
                float stopsError = std::log2(targetLuma / currentLuma);

                // Dead-band clamp to prevent microscopic exposure jitter on static scenes
                float stepStops = 0.0f;
                if (std::abs(stopsError) >= 0.03f) {
                    // Damped step size (15% per step) with max change clamp (0.25 EV stops) for smooth cinematic transition
                    stepStops = std::max(-0.25f, std::min(stopsError * 0.15f, 0.25f));
                }

                if (stepStops != 0.0f) {
                    // Adjust current exposure parameters relative to the control output
                    double currentExpVal = (double)mIso * (double)mShutterSpeedNs;
                    double targetExpVal = currentExpVal * std::pow(2.0, (double)stepStops);

                    // Locks shutter to standard cinematic 180 degree angle: 1 / (2 * FPS)
                    int64_t targetShutterNs = 1000000000LL / (mTargetFps * 2);

                    // Anti-flicker: snap shutter speed to nearest multiple of the AC light cycle
                    // Artificial lights flicker at 2x the AC frequency (100Hz for 50Hz mains, 120Hz for 60Hz mains)
                    // Shutter period must be a whole multiple of the light flicker period to avoid banding
                    if (mAntiFlickerHz == 50 || mAntiFlickerHz == 60) {
                        // Light flicker period in nanoseconds: 1 / (2 * Hz)
                        int64_t flickerPeriodNs = 1000000000LL / (2 * mAntiFlickerHz);
                        // Snap to nearest whole multiple of flicker period
                        int64_t multiples = (targetShutterNs + flickerPeriodNs / 2) / flickerPeriodNs;
                        if (multiples < 1) multiples = 1;
                        targetShutterNs = multiples * flickerPeriodNs;
                        LOGI("Anti-flicker snap: %d Hz → shutter %lld ns (%d multiples of %lld ns)",
                             (int)mAntiFlickerHz, (long long)targetShutterNs,
                             (int)multiples, (long long)flickerPeriodNs);
                    }

                    int32_t targetIso = (int32_t)(targetExpVal / targetShutterNs);

                    if (targetIso < 100) {
                        // Highlight fallback: lock ISO to 100 and shorten shutter speed to prevent overexposure
                        targetIso = 100;
                        int64_t highlightShutterNs = (int64_t)(targetExpVal / 100.0);
                        // Snap highlight shutter to flicker period too
                        if (mAntiFlickerHz == 50 || mAntiFlickerHz == 60) {
                            int64_t flickerPeriodNs = 1000000000LL / (2 * mAntiFlickerHz);
                            int64_t multiples = (highlightShutterNs + flickerPeriodNs / 2) / flickerPeriodNs;
                            if (multiples < 1) multiples = 1;
                            highlightShutterNs = multiples * flickerPeriodNs;
                        }
                        targetShutterNs = highlightShutterNs;
                    } else if (targetIso > 3200) {
                        // Low-light fallback: lock ISO to 3200 and lengthen shutter speed to gather more light
                        targetIso = 3200;
                        int64_t lowlightShutterNs = (int64_t)(targetExpVal / 3200.0);
                        // Snap low-light shutter to flicker period too
                        if (mAntiFlickerHz == 50 || mAntiFlickerHz == 60) {
                            int64_t flickerPeriodNs = 1000000000LL / (2 * mAntiFlickerHz);
                            int64_t multiples = (lowlightShutterNs + flickerPeriodNs / 2) / flickerPeriodNs;
                            if (multiples < 1) multiples = 1;
                            lowlightShutterNs = multiples * flickerPeriodNs;
                        }
                        targetShutterNs = lowlightShutterNs;
                    }

                    // Safety clamps: Shutter cannot be longer than 360-degree (1 / FPS) and cannot be shorter than hardware limits (1/8000s)
                    int64_t maxShutterNs = 1000000000LL / mTargetFps;
                    int64_t minShutterNs = 1000000000LL / 8000;
                    targetShutterNs = std::max(minShutterNs, std::min(targetShutterNs, maxShutterNs));

                    {
                        std::lock_guard<std::mutex> lock(mCameraMutex);
                        mIso = targetIso;
                        mShutterSpeedNs = targetShutterNs;
                        applyExposureSettings();
                        updateRepeatingRequest();
                    }
                }
            }
        }

        // 4. Synchronously wait for camera frame callback trigger (max 25ms to keep loop moving)
        std::unique_lock<std::mutex> frameLock(mFrameMutex);
        mFrameCondVar.wait_for(frameLock, std::chrono::milliseconds(25), [this] {
            return mFrameAvailable || !mIsLoopRunning;
        });

        if (!mIsLoopRunning) break;
        
        mFrameAvailable = false;
        frameLock.unlock();

        // 5. Update SurfaceTexture frame and render GPU passes
        env->CallVoidMethod(mSurfaceTextureRef, mUpdateTexImageMethod);
        jlong timestampNs = env->CallLongMethod(mSurfaceTextureRef, mGetTimestampMethod);

        mGlRenderer->renderFrame(shiftX, shiftY, mIsRecording.load(), timestampNs, mActiveLensFacing == 0);

        // 6. Read back hardware downsampled luminance for next PID iteration
        mLastLuma = mGlRenderer->readAverageLuma();

        // High precision sleep synchronizer
        std::this_thread::sleep_until(nextFrameTime);
    }

    releaseGyroscope();
    mJavaVM->DetachCurrentThread();
}

void CameraEngine::setFocus(bool autoFocus, float distance) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mAutoFocus = autoFocus;
    mFocusDistance = distance;
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::setExposure(bool autoExposure, int32_t iso, int64_t shutterSpeedNs) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mAutoExposure = autoExposure;
    if (iso > 0) mIso = iso;
    if (shutterSpeedNs > 0) mShutterSpeedNs = shutterSpeedNs;
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::setFrameRate(int32_t fps) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mTargetFps = fps;
    if (mCaptureSession && mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
}

void CameraEngine::setZoom(float ratio) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    if (!mCaptureRequest) return;

    mZoomRatio = std::max(1.0f, std::min(ratio, 8.0f));

    // Use cached sensor array to avoid live IPC call to camera service
    int32_t cropW = (int32_t)(mSensorArrayWidth  / mZoomRatio);
    int32_t cropH = (int32_t)(mSensorArrayHeight / mZoomRatio);
    int32_t cropL = mSensorArrayLeft + (mSensorArrayWidth  - cropW) / 2;
    int32_t cropT = mSensorArrayTop  + (mSensorArrayHeight - cropH) / 2;

    int32_t cropRegion[4] = {cropL, cropT, cropW, cropH};
    ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SCALER_CROP_REGION, 4, cropRegion);

    updateRepeatingRequest();
}

void CameraEngine::setFocusPoint(float x, float y, int viewWidth, int viewHeight) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    if (!mCaptureRequest) return;

    // Use cached sensor array to avoid live IPC call to camera service
    int32_t cropW = (int32_t)(mSensorArrayWidth  / mZoomRatio);
    int32_t cropH = (int32_t)(mSensorArrayHeight / mZoomRatio);
    int32_t cropL = mSensorArrayLeft + (mSensorArrayWidth  - cropW) / 2;
    int32_t cropT = mSensorArrayTop  + (mSensorArrayHeight - cropH) / 2;

    float nx = std::max(0.0f, std::min(1.0f, x / (float)viewWidth));
    float ny = std::max(0.0f, std::min(1.0f, y / (float)viewHeight));

    // Sensor coordinate mapping depends on camera facing direction.
    // Back camera (sensor orientation 90°): portrait x→sensorY, portrait y→sensorX
    // Front camera (sensor orientation 270°): axes are mirrored.
    int32_t sensorX, sensorY;
    if (mActiveLensFacing == ACAMERA_LENS_FACING_FRONT) {
        sensorX = cropL + (int32_t)((1.0f - ny) * cropW);
        sensorY = cropT + (int32_t)(nx * cropH);
    } else {
        sensorX = cropL + (int32_t)(ny * cropW);
        sensorY = cropT + (int32_t)((1.0f - nx) * cropH);
    }

    int32_t halfW = cropW / 20;
    int32_t halfH = cropH / 20;

    int32_t box[5] = {
        std::max(cropL, sensorX - halfW),
        std::max(cropT, sensorY - halfH),
        std::min(cropL + cropW, sensorX + halfW),
        std::min(cropT + cropH, sensorY + halfH),
        1000 // Focus region weight
    };

    ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_CONTROL_AF_REGIONS, 5, box);
    ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_CONTROL_AE_REGIONS, 5, box);

    uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_START;
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afTrigger);

    updateRepeatingRequest();
}

void CameraEngine::setExposureCompensation(int32_t value) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mExposureCompensation = value;
    if (mCaptureRequest) {
        ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &mExposureCompensation);
        applyExposureSettings();
        updateRepeatingRequest();
    }
}

void CameraEngine::setNoiseReduction(uint8_t mode) {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    mNoiseReductionMode = mode;
    if (mCaptureRequest) {
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_NOISE_REDUCTION_MODE, 1, &mNoiseReductionMode);
        updateRepeatingRequest();
    }
}

void CameraEngine::pushAudioFrame(const uint8_t* data, int size) {
    // Guard with mEncoderMutex only (NOT mCameraMutex) so audio capture
    // does not contend with camera configuration updates on every audio frame.
    if (!mIsRecording) return;
    std::lock_guard<std::mutex> lock(mEncoderMutex);
    if (mMediaEncoder) {
        mMediaEncoder->encodeAudioFrame(data, size);
    }
}

void CameraEngine::closeCamera() {
    stopLoopThread();
    std::lock_guard<std::mutex> lock(mCameraMutex);

    if (mCaptureSession) {
        ACameraCaptureSession_stopRepeating(mCaptureSession);
        ACameraCaptureSession_close(mCaptureSession);
        mCaptureSession = nullptr;
    }

    if (mCameraDevice) {
        ACameraDevice_close(mCameraDevice);
        mCameraDevice = nullptr;
    }

    if (mCameraWindow) {
        ANativeWindow_release(mCameraWindow);
        mCameraWindow = nullptr;
    }

    if (mCameraOutput) {
        ACaptureSessionOutput_free(mCameraOutput);
        mCameraOutput = nullptr;
    }

    if (mCameraTarget) {
        ACameraOutputTarget_free(mCameraTarget);
        mCameraTarget = nullptr;
    }

    if (mOutputContainer) {
        ACaptureSessionOutputContainer_free(mOutputContainer);
        mOutputContainer = nullptr;
    }

    if (mCaptureRequest) {
        ACaptureRequest_free(mCaptureRequest);
        mCaptureRequest = nullptr;
    }

    if (mGlRenderer) {
        // Bind EGL context to the calling thread before issuing GL delete calls.
        // After stopLoopThread(), the loop thread (which had EGL current) has exited.
        // GL resource deletion requires a valid current context on the calling thread.
        mGlRenderer->bindEgl();
        mGlRenderer->releaseGl();
        mGlRenderer->releaseEgl();
        mGlRenderer.reset();
    }
}

void CameraEngine::releaseCamera() {
    closeCamera();

    JNIEnv* env = nullptr;
    if (mSurfaceTextureRef && mJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        env->DeleteGlobalRef(mSurfaceTextureRef);
        mSurfaceTextureRef = nullptr;
    }

    if (mCameraIdList) {
        ACameraManager_deleteCameraIdList(mCameraIdList);
        mCameraIdList = nullptr;
    }
}
