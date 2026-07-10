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

    // Always query capabilities and cache sensor array / FOV / zoom range for the selected mCameraId
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(mCameraManager, mCameraId.c_str(), &chars) == ACAMERA_OK && chars) {
        ACameraMetadata_const_entry arrayEntry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &arrayEntry) == ACAMERA_OK
                && arrayEntry.count == 4) {
            mSensorArrayLeft   = arrayEntry.data.i32[0];
            mSensorArrayTop    = arrayEntry.data.i32[1];
            mSensorArrayWidth  = arrayEntry.data.i32[2] - arrayEntry.data.i32[0];
            mSensorArrayHeight = arrayEntry.data.i32[3] - arrayEntry.data.i32[1];
            LOGI("initCamera active array size loaded: left=%d top=%d w=%d h=%d",
                 mSensorArrayLeft, mSensorArrayTop, mSensorArrayWidth, mSensorArrayHeight);
        }
        ACameraMetadata_const_entry physEntry, focalEntry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &physEntry) == ACAMERA_OK
                && physEntry.count == 2
                && ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focalEntry) == ACAMERA_OK
                && focalEntry.count >= 1) {
            mEisFovX = 2.0f * std::atan(physEntry.data.f[0] / (2.0f * focalEntry.data.f[0]));
            mEisFovY = 2.0f * std::atan(physEntry.data.f[1] / (2.0f * focalEntry.data.f[0]));
            LOGI("initCamera EIS FOV loaded: %.1f\u00b0 x %.1f\u00b0",
                 mEisFovX * 180.0f / M_PI, mEisFovY * 180.0f / M_PI);
        }
        // Query Zoom Ratio range (API 30+)
        ACameraMetadata_const_entry zoomRatioEntry;
        if (ACameraMetadata_getConstEntry(chars, 0x01002e, &zoomRatioEntry) == ACAMERA_OK && zoomRatioEntry.count == 2) {
            mMinZoomRatio = zoomRatioEntry.data.f[0];
            mMaxZoomRatio = zoomRatioEntry.data.f[1];
            mHasZoomRatio = (mMaxZoomRatio > mMinZoomRatio);
            LOGI("initCamera ZoomRatio support: range=[%.2f, %.2f]", mMinZoomRatio, mMaxZoomRatio);
        } else {
            mHasZoomRatio = false;
            mMinZoomRatio = 1.0f;
            mMaxZoomRatio = 8.0f;
            LOGI("initCamera: ZoomRatio support not found, falling back to crop region zoom.");
        }
        ACameraMetadata_free(chars);
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
            // Query Zoom Ratio range (API 30+)
            ACameraMetadata_const_entry zoomRatioEntry;
            if (ACameraMetadata_getConstEntry(chars, 0x01002e, &zoomRatioEntry) == ACAMERA_OK && zoomRatioEntry.count == 2) {
                mMinZoomRatio = zoomRatioEntry.data.f[0];
                mMaxZoomRatio = zoomRatioEntry.data.f[1];
                mHasZoomRatio = (mMaxZoomRatio > mMinZoomRatio);
                LOGI("findRearCamera ZoomRatio support: range=[%.2f, %.2f]", mMinZoomRatio, mMaxZoomRatio);
            } else {
                mHasZoomRatio = false;
                mMinZoomRatio = 1.0f;
                mMaxZoomRatio = 8.0f;
                LOGI("findRearCamera: ZoomRatio support not found, falling back to crop region zoom.");
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
        // Query Zoom Ratio range (API 30+)
        ACameraMetadata_const_entry zoomRatioEntry;
        if (ACameraMetadata_getConstEntry(chars, 0x01002e, &zoomRatioEntry) == ACAMERA_OK && zoomRatioEntry.count == 2) {
            mMinZoomRatio = zoomRatioEntry.data.f[0];
            mMaxZoomRatio = zoomRatioEntry.data.f[1];
            mHasZoomRatio = (mMaxZoomRatio > mMinZoomRatio);
            LOGI("setLens ZoomRatio support: range=[%.2f, %.2f]", mMinZoomRatio, mMaxZoomRatio);
        } else {
            mHasZoomRatio = false;
            mMinZoomRatio = 1.0f;
            mMaxZoomRatio = 8.0f;
            LOGI("setLens: ZoomRatio support not found, falling back to crop region zoom.");
        }
        // Reset EIS quaternion state on lens switch
        mOrientation[0] = 1.0f; mOrientation[1] = 0.0f;
        mOrientation[2] = 0.0f; mOrientation[3] = 0.0f;
        mLastGyroTimestamp = 0;
        mLastAccelTimestamp = 0;
        mOrientRingHead = 0;
        mOrientRingFull = false;
        for (int i = 0; i < kEisRingSize; ++i) {
            mOrientRing[i][0] = 1.0f;
            mOrientRing[i][1] = mOrientRing[i][2] = mOrientRing[i][3] = 0.0f;
        }
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

    // Apply zoom ratio (API 30+) or fallback to crop region (legacy)
    if (mHasZoomRatio) {
        // Set ACAMERA_CONTROL_ZOOM_RATIO (0x01002f)
        float zoomValue = std::max(mMinZoomRatio, std::min(mZoomRatio, mMaxZoomRatio));
        ACaptureRequest_setEntry_float(mCaptureRequest, 0x01002f, 1, &zoomValue);

        // Also set crop region to the full active array to prevent double crop
        int32_t cropRegion[4] = {
            mSensorArrayLeft,
            mSensorArrayTop,
            mSensorArrayLeft + mSensorArrayWidth,
            mSensorArrayTop + mSensorArrayHeight
        };
        ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SCALER_CROP_REGION, 4, cropRegion);

        LOGI("Zoom applied (via ZoomRatio tag): ratio=%.2f (clamped: %.2f), range=[%.2f, %.2f], array=[%d,%d,%d,%d]",
             mZoomRatio, zoomValue, mMinZoomRatio, mMaxZoomRatio,
             mSensorArrayLeft, mSensorArrayTop, mSensorArrayWidth, mSensorArrayHeight);
    } else {
        // Fallback to legacy crop region zoom (API < 30)
        int32_t cropW = (int32_t)(mSensorArrayWidth  / mZoomRatio);
        int32_t cropH = (int32_t)(mSensorArrayHeight / mZoomRatio);
        int32_t cropL = mSensorArrayLeft + (mSensorArrayWidth  - cropW) / 2;
        int32_t cropT = mSensorArrayTop  + (mSensorArrayHeight - cropH) / 2;
        int32_t cropRegion[4] = {cropL, cropT, cropL + cropW, cropT + cropH};
        ACaptureRequest_setEntry_i32(mCaptureRequest, ACAMERA_SCALER_CROP_REGION, 4, cropRegion);

        LOGI("Zoom applied (via CropRegion fallback): ratio=%.2f, sensorArray=[%d, %d, %d, %d], cropRegion=[%d, %d, %d, %d]",
             mZoomRatio, mSensorArrayLeft, mSensorArrayTop, mSensorArrayWidth, mSensorArrayHeight,
             cropL, cropT, cropL + cropW, cropT + cropH);
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
    // NOTE: EIS and OIS are NOT mutually exclusive. Hardware OIS corrects
    // lens mechanical sway; software EIS corrects residual texture-space tremor.
    // Both can be active simultaneously for maximum stability.

    // Reset quaternion IMU state so re-enabling starts with a clean slate
    if (enabled) {
        mOrientation[0] = 1.0f; mOrientation[1] = 0.0f;
        mOrientation[2] = 0.0f; mOrientation[3] = 0.0f;
        mLastGyroTimestamp = 0;
        mLastAccelTimestamp = 0;
        mOrientRingHead = 0;
        mOrientRingFull = false;
        for (int i = 0; i < kEisRingSize; ++i) {
            mOrientRing[i][0] = 1.0f;
            mOrientRing[i][1] = mOrientRing[i][2] = mOrientRing[i][3] = 0.0f;
        }
    }
    // Crop: 15% headroom (scale=0.85) when EIS on, full sensor (1.0) when off
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
    // NOTE: OIS and EIS are NOT mutually exclusive. Hardware OIS corrects
    // optical-path mechanical movement; software EIS corrects residual
    // high-frequency shake in texture UV space. Both can be active at once.
    // EIS crop scale is controlled solely by mUseStabilization, not OIS.
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
        LOGW("No hardware gyroscope detected. EIS disabled.");
        return;
    }

    mAccelSensor = ASensorManager_getDefaultSensor(mSensorManager, ASENSOR_TYPE_ACCELEROMETER);
    if (!mAccelSensor) {
        LOGW("No accelerometer detected. EIS will run gyro-only (no drift correction).");
    }

    mLooper = ALooper_prepare(0);
    mSensorEventQueue = ASensorManager_createEventQueue(
            mSensorManager, mLooper, 0, nullptr, nullptr);

    // Gyroscope at 200Hz (5ms) — high rate to capture fast hand shake (1-20Hz)
    ASensorEventQueue_enableSensor(mSensorEventQueue, mGyroSensor);
    ASensorEventQueue_setEventRate(mSensorEventQueue, mGyroSensor, 5000);

    // Accelerometer at 50Hz (20ms) — used only for long-term gravity correction
    if (mAccelSensor) {
        ASensorEventQueue_enableSensor(mSensorEventQueue, mAccelSensor);
        ASensorEventQueue_setEventRate(mSensorEventQueue, mAccelSensor, 20000);
    }

    // Init quaternion state: identity orientation [w=1, x=0, y=0, z=0]
    mOrientation[0] = 1.0f; mOrientation[1] = 0.0f;
    mOrientation[2] = 0.0f; mOrientation[3] = 0.0f;
    mGravity[0] = 0.0f; mGravity[1] = 0.0f; mGravity[2] = -9.81f;
    mLastGyroTimestamp  = 0;
    mLastAccelTimestamp = 0;
    mOrientRingHead = 0;
    mOrientRingFull = false;
    // Zero-fill ring buffer with identity quaternions
    for (int i = 0; i < kEisRingSize; ++i) {
        mOrientRing[i][0] = 1.0f;
        mOrientRing[i][1] = mOrientRing[i][2] = mOrientRing[i][3] = 0.0f;
    }

    LOGI("EIS IMU initialised: gyro@200Hz + accel@50Hz. Quaternion complementary filter.");
}

void CameraEngine::releaseGyroscope() {
    if (mSensorEventQueue) {
        if (mGyroSensor)  ASensorEventQueue_disableSensor(mSensorEventQueue, mGyroSensor);
        if (mAccelSensor) ASensorEventQueue_disableSensor(mSensorEventQueue, mAccelSensor);
        ASensorManager_destroyEventQueue(mSensorManager, mSensorEventQueue);
        mSensorEventQueue = nullptr;
    }
    mGyroSensor  = nullptr;
    mAccelSensor = nullptr;
    mSensorManager = nullptr;
    LOGI("EIS IMU sensors released.");
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

        // 1. EIS — iOS-inspired Quaternion IMU Complementary Filter
        float shiftX = 0.0f;
        float shiftY = 0.0f;

        if (mUseStabilization && mSensorEventQueue) {
            // ----------------------------------------------------------------
            // Helper lambdas (capture nothing — operate on local temps only)
            // ----------------------------------------------------------------

            // Quaternion multiply: out = a ⊗ b (Hamilton product)
            auto quatMul = [](float* out, const float* a, const float* b) {
                out[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
                out[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
                out[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
                out[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
            };

            // Normalize quaternion in-place
            auto quatNorm = [](float* q) {
                float n = std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
                if (n < 1e-6f) { q[0]=1.f; q[1]=q[2]=q[3]=0.f; return; }
                float inv = 1.0f / n;
                q[0]*=inv; q[1]*=inv; q[2]*=inv; q[3]*=inv;
            };

            // Spherical linear interpolation between two unit quaternions
            auto quatSlerp = [](float* out, const float* a, const float* b, float t) {
                float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
                // Choose shortest path
                float b0=b[0], b1=b[1], b2=b[2], b3=b[3];
                if (dot < 0.0f) { dot=-dot; b0=-b0; b1=-b1; b2=-b2; b3=-b3; }
                if (dot > 0.9995f) {
                    // Nearly identical — linear interpolation is fine
                    out[0]=a[0]+(b0-a[0])*t; out[1]=a[1]+(b1-a[1])*t;
                    out[2]=a[2]+(b2-a[2])*t; out[3]=a[3]+(b3-a[3])*t;
                    float n=std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
                    if(n>1e-6f){out[0]/=n;out[1]/=n;out[2]/=n;out[3]/=n;}
                    return;
                }
                float theta0 = std::acos(dot);
                float theta  = theta0 * t;
                float sinT0  = std::sin(theta0);
                float sinT   = std::sin(theta);
                float s0 = std::cos(theta) - dot * sinT / sinT0;
                float s1 = sinT / sinT0;
                out[0]=s0*a[0]+s1*b0; out[1]=s0*a[1]+s1*b1;
                out[2]=s0*a[2]+s1*b2; out[3]=s0*a[3]+s1*b3;
            };

            // ----------------------------------------------------------------
            // Stage 1: Drain IMU events, integrate gyro via quaternion, fuse accel
            // ----------------------------------------------------------------
            // Poll the looper with 0ms timeout (non-blocking) to ensure that the
            // looper queries sensor file descriptors and populates the event queue.
            ALooper_pollOnce(0, nullptr, nullptr, nullptr);

            ASensorEvent event;
            while (ASensorEventQueue_getEvents(mSensorEventQueue, &event, 1) > 0) {

                if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                    if (mLastGyroTimestamp == 0) {
                        mLastGyroTimestamp = event.timestamp;
                        continue; // seed timestamp; dt unknown
                    }
                    float dt = (event.timestamp - mLastGyroTimestamp) * 1e-9f;
                    mLastGyroTimestamp = event.timestamp;
                    if (dt <= 0.0f || dt > 0.05f) continue; // reject stale/invalid

                    // Angular velocity in device frame [rad/s]
                    float wx = event.data[0];
                    float wy = event.data[1];
                    float wz = event.data[2];

                    // Magnitude of rotation in this timestep (rad)
                    float omega = std::sqrt(wx*wx + wy*wy + wz*wz);
                    float halfAngle = omega * dt * 0.5f;

                    // Delta quaternion from gyro: dq = [cos(θ/2), n̂ sin(θ/2)]
                    float dq[4];
                    if (omega > 1e-6f) {
                        float s = std::sin(halfAngle) / omega;
                        dq[0] = std::cos(halfAngle);
                        dq[1] = wx * s;
                        dq[2] = wy * s;
                        dq[3] = wz * s;
                    } else {
                        // Near-zero rotation: identity + first-order approximation
                        dq[0] = 1.0f;
                        dq[1] = wx * dt * 0.5f;
                        dq[2] = wy * dt * 0.5f;
                        dq[3] = wz * dt * 0.5f;
                    }

                    // Integrate: q_new = q ⊗ dq
                    float qNew[4];
                    quatMul(qNew, mOrientation, dq);
                    quatNorm(qNew);
                    mOrientation[0]=qNew[0]; mOrientation[1]=qNew[1];
                    mOrientation[2]=qNew[2]; mOrientation[3]=qNew[3];

                } else if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
                    // --------------------------------------------------------
                    // Stage 2: Accelerometer complementary correction (drift fix)
                    // Low-pass filter gravity; compute tilt quaternion; SLERP-blend.
                    // Alpha=0.9 for gravity LP (reject vibration) → stable gravity estimate.
                    // --------------------------------------------------------
                    const float lpAlpha = 0.9f;
                    mGravity[0] = lpAlpha*mGravity[0] + (1.0f-lpAlpha)*event.data[0];
                    mGravity[1] = lpAlpha*mGravity[1] + (1.0f-lpAlpha)*event.data[1];
                    mGravity[2] = lpAlpha*mGravity[2] + (1.0f-lpAlpha)*event.data[2];

                    // Normalize gravity vector
                    float gLen = std::sqrt(mGravity[0]*mGravity[0]+
                                           mGravity[1]*mGravity[1]+
                                           mGravity[2]*mGravity[2]);
                    if (gLen < 1e-6f) continue;
                    float gx = mGravity[0]/gLen;
                    float gy = mGravity[1]/gLen;
                    float gz = mGravity[2]/gLen;

                    // Rotate the world "up" vector (0,0,1) by current orientation
                    // to get predicted gravity in device frame. Compare vs measured.
                    // For tilt correction, compute the rotation that maps measured_g → world_up.
                    // Cross product gives rotation axis; dot product gives angle.
                    // World up = (0, 0, 1). Measured acceleration from sensor is positive Z when flat.
                    float crossX =  gy; // cross(g_device, world_up) = (gy*1-gz*0, gz*0-gx*1, gx*0-gy*0) = (gy, -gx, 0)
                    float crossY = -gx;
                    float crossZ =  0.0f;
                    float crossLen = std::sqrt(crossX*crossX + crossY*crossY);

                    if (crossLen > 1e-6f) {
                        // Angle between measured gravity and (0,0,1)
                        float cosA = gz; // dot(g_device, (0,0,1))
                        cosA = std::max(-1.0f, std::min(1.0f, cosA));
                        float halfA = std::acos(cosA) * 0.5f;
                        float s = std::sin(halfA) / crossLen;
                        float q_accel[4] = { std::cos(halfA), crossX*s, crossY*s, crossZ*s };

                        // Complementary blend: gyro dominates (α=0.98), accel corrects drift (1-α=0.02)
                        const float fusionAlpha = 0.98f;
                        float q_fused[4];
                        quatSlerp(q_fused, q_accel, mOrientation, fusionAlpha);
                        quatNorm(q_fused);
                        mOrientation[0]=q_fused[0]; mOrientation[1]=q_fused[1];
                        mOrientation[2]=q_fused[2]; mOrientation[3]=q_fused[3];
                    }
                }
            }

            // ----------------------------------------------------------------
            // Stage 3: Ring-buffer trajectory smoothing → isolate shake
            // ----------------------------------------------------------------
            // Push current orientation into ring buffer
            int slot = mOrientRingHead;
            mOrientRing[slot][0] = mOrientation[0];
            mOrientRing[slot][1] = mOrientation[1];
            mOrientRing[slot][2] = mOrientation[2];
            mOrientRing[slot][3] = mOrientation[3];
            mOrientRingHead = (mOrientRingHead + 1) % kEisRingSize;
            if (!mOrientRingFull && mOrientRingHead == 0) mOrientRingFull = true;

            int ringN = mOrientRingFull ? kEisRingSize : std::max(1, mOrientRingHead);

            // Compute spherical average over the ring buffer = "intended camera path"
            // Start from oldest entry, SLERP toward each subsequent entry by 1/remaining.
            int oldest = mOrientRingFull ? mOrientRingHead : 0;
            float q_smooth[4] = {
                mOrientRing[oldest][0], mOrientRing[oldest][1],
                mOrientRing[oldest][2], mOrientRing[oldest][3]
            };
            for (int i = 1; i < ringN; ++i) {
                int idx = (oldest + i) % kEisRingSize;
                float t = 1.0f / (i + 1.0f); // blending weight toward current sample
                float q_tmp[4];
                quatSlerp(q_tmp, q_smooth, mOrientRing[idx], t);
                q_smooth[0]=q_tmp[0]; q_smooth[1]=q_tmp[1];
                q_smooth[2]=q_tmp[2]; q_smooth[3]=q_tmp[3];
            }

            // q_shake = conj(q_smooth) ⊗ q_current  (deviation from smooth path)
            float q_smoothConj[4] = { q_smooth[0], -q_smooth[1], -q_smooth[2], -q_smooth[3] };
            float q_shake[4];
            quatMul(q_shake, q_smoothConj, mOrientation);
            quatNorm(q_shake);

            // --- Dynamic Panning Attenuation ---
            // If the user is intentionally panning/tilting the camera, the deviation angle
            // will grow large. To prevent the crop window from hitting the ±7.5% boundary and
            // causing visual jerks, we smoothly pull the target reference trajectory (q_smooth)
            // towards the current orientation if the deviation exceeds kMaxDeviationRad.
            float cosHalfTheta = std::abs(q_shake[0]);
            float theta = 2.0f * std::acos(std::min(1.0f, cosHalfTheta));
            const float kMaxDeviationRad = 0.05f; // ~2.8 degrees
            if (theta > kMaxDeviationRad) {
                float blendFactor = (theta - kMaxDeviationRad) / 0.02f; // fully blend if over 4 degrees
                blendFactor = std::max(0.0f, std::min(1.0f, blendFactor));
                
                float q_adjusted[4];
                quatSlerp(q_adjusted, q_smooth, mOrientation, blendFactor);
                q_smooth[0]=q_adjusted[0]; q_smooth[1]=q_adjusted[1];
                q_smooth[2]=q_adjusted[2]; q_smooth[3]=q_adjusted[3];

                // Re-evaluate q_shake with the pan-adjusted reference path
                float q_smoothConjAdjusted[4] = { q_smooth[0], -q_smooth[1], -q_smooth[2], -q_smooth[3] };
                quatMul(q_shake, q_smoothConjAdjusted, mOrientation);
                quatNorm(q_shake);
            }

            // Extract pitch (X rotation) and yaw correction (Y rotation) from q_shake.
            // For small angles: q ≈ [1, qx/2, qy/2, qz/2] so 2*qx ≈ angle_around_x.
            // Roll (around Z) is excluded — yaw/pan is intentional camera movement.
            float shakeAngleX = 2.0f * q_shake[1]; // pitch (vertical shake)
            float shakeAngleY = 2.0f * q_shake[2]; // roll  (horizontal shake)

            // Convert angular shake (rad) → normalized UV shift via calibrated FOV
            float rawX = shakeAngleY / mEisFovX; // horizontal image shift
            float rawY = shakeAngleX / mEisFovY; // vertical image shift

            // Clamp to ±7.5% UV headroom (crop scale 0.85 → 15% total → ±7.5%)
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
                        int64_t flickerPeriodNs = 1000000000LL / (2 * mAntiFlickerHz);
                        int64_t multiples = (targetShutterNs + flickerPeriodNs / 2) / flickerPeriodNs;
                        if (multiples < 1) multiples = 1;
                        targetShutterNs = multiples * flickerPeriodNs;
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

        // 7. Update histogram cached data on the loop thread (holds current EGL context)
        int32_t tempHist[64];
        mGlRenderer->getHistogram(tempHist, 64);
        {
            std::lock_guard<std::mutex> histLock(mHistMutex);
            std::memcpy(mHistogramCached, tempHist, sizeof(tempHist));
        }

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
    mZoomRatio = std::max(1.0f, std::min(ratio, 8.0f));
    if (mCaptureRequest) {
        configureCaptureRequest();
        updateRepeatingRequest();
    }
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

    // Fire AF trigger for one repeating cycle, then immediately reset to IDLE.
    // Leaving AF_TRIGGER_START in the repeating request causes the camera to restart
    // autofocus on every frame — it never locks. IDLE lets AF run and converge.
    uint8_t afStart = ACAMERA_CONTROL_AF_TRIGGER_START;
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afStart);
    updateRepeatingRequest();

    // Reset to IDLE immediately — the START frame was already queued above
    uint8_t afIdle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afIdle);
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

void CameraEngine::getCachedHistogram(int32_t* outBins, int binCount) {
    std::lock_guard<std::mutex> lock(mHistMutex);
    int count = std::min(binCount, 64);
    std::memcpy(outBins, mHistogramCached, count * sizeof(int32_t));
    if (binCount > 64) {
        std::memset(outBins + 64, 0, (binCount - 64) * sizeof(int32_t));
    }
}
