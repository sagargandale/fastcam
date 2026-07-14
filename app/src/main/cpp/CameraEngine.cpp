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
            std::lock_guard<std::mutex> eisLock(mEisMutex);
            mEis.fovX = 2.0f * std::atan(physEntry.data.f[0] / (2.0f * focalEntry.data.f[0]));
            mEis.fovY = 2.0f * std::atan(physEntry.data.f[1] / (2.0f * focalEntry.data.f[0]));
            LOGI("initCamera EIS FOV loaded: %.1f\u00b0 x %.1f\u00b0",
                 mEis.fovX * 180.0f / M_PI, mEis.fovY * 180.0f / M_PI);
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
                std::lock_guard<std::mutex> eisLock(mEisMutex);
                mEis.fovX = 2.0f * std::atan(sensorW_mm / (2.0f * focalLen_mm));
                mEis.fovY = 2.0f * std::atan(sensorH_mm / (2.0f * focalLen_mm));
                LOGI("EIS FOV calibrated: %.1f\u00b0 x %.1f\u00b0 (sensor %.2fx%.2fmm, f=%.2fmm)",
                     mEis.fovX * 180.0f / M_PI, mEis.fovY * 180.0f / M_PI,
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
            std::lock_guard<std::mutex> eisLock(mEisMutex);
            mEis.fovX = 2.0f * std::atan(physEntry.data.f[0] / (2.0f * focalEntry.data.f[0]));
            mEis.fovY = 2.0f * std::atan(physEntry.data.f[1] / (2.0f * focalEntry.data.f[0]));
            LOGI("EIS FOV refreshed for lens %s: %.1f\u00b0 x %.1f\u00b0",
                 lensId.c_str(), mEis.fovX * 180.0f / M_PI, mEis.fovY * 180.0f / M_PI);
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
        // Reset EIS state on lens switch so we start fresh with the new lens geometry
        {
            std::lock_guard<std::mutex> eisLock(mEisMutex);
            mEis = EisState{};
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
    // Both auto-mode and manual mode use CONTINUOUS_VIDEO so the lens always hunts
    // and converges. Tap-to-focus narrows the metering region rather than switching
    // to one-shot AF_MODE_AUTO (which would stall after a single scan).
    if (mAutoFocus) {
        uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        // Ensure AF trigger is always IDLE in the repeating request
        uint8_t afIdle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_TRIGGER, 1, &afIdle);
    } else {
        uint8_t afMode = ACAMERA_CONTROL_AF_MODE_OFF;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        ACaptureRequest_setEntry_float(mCaptureRequest, ACAMERA_LENS_FOCUS_DISTANCE, 1, &mFocusDistance);
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

    // Reset EIS state so re-enabling starts with a clean slate
    if (enabled) {
        std::lock_guard<std::mutex> eisLock(mEisMutex);
        mEis = EisState{};
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
    LOGI("AE lock: %s", locked ? "LOCKED" : "UNLOCKED");

    if (mCaptureRequest) {
        // Only apply AE lock/unlock — do NOT touch AF trigger here.
        // AF is managed independently by setFocusPoint() via burst captures.
        uint8_t aeLock = locked ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
        ACaptureRequest_setEntry_u8(mCaptureRequest, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);
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
        LOGW("No accelerometer detected. EIS running gyro-only (no drift correction).");
    }

    // Reset EIS state
    {
        std::lock_guard<std::mutex> lock(mEisMutex);
        mEis = EisState{};
    }

    // Start dedicated sensor thread — creates its own ALooper internally
    mSensorRunning = true;
    mSensorThread = std::thread(&CameraEngine::sensorLoop, this);
    LOGI("EIS sensor thread started (200Hz gyro + 50Hz accel).");
}

void CameraEngine::releaseGyroscope() {
    mSensorRunning = false;
    if (mSensorThread.joinable()) {
        mSensorThread.join();
    }
    // mSensorEventQueue and mSensorLooper are owned by sensorLoop() — cleaned up there
    mSensorEventQueue = nullptr;
    mSensorLooper     = nullptr;
    mGyroSensor       = nullptr;
    mAccelSensor      = nullptr;
    mSensorManager    = nullptr;
    LOGI("EIS sensor thread stopped.");
}

// ─────────────────────────────────────────────────────────────────────────────
// sensorLoop() — dedicated 200Hz IMU integration thread
//
// Research-backed fixes vs previous version:
//  1. IIR low-pass filter on raw gyro (cutoff ~40Hz) — kills MEMS quantization noise
//     before it enters the quaternion integrator. Gyroflow uses 30-70Hz LPF.
//  2. Smoothing alpha = 0.04 (was 0.25). At 200Hz: 200*ln(1/0.96) ≈ 8 rad/s time
//     constant → ~25 frame lag. This gives the smoothed path enough separation
//     from qCurrent so the deviation is meaningful, not zero.
//  3. Adaptive alpha: temporarily raises to 0.15 during fast pans (omega > 0.3 rad/s)
//     so the crop window tracks intentional motion instead of fighting it.
//  4. Fixed SLERP non-degenerate branch — removed dead variable 'sa', uses correct
//     Shoemake formulation throughout.
// ─────────────────────────────────────────────────────────────────────────────
void CameraEngine::sensorLoop() {
    ALooper* looper = ALooper_prepare(0);
    mSensorLooper = looper;

    ASensorEventQueue* queue = ASensorManager_createEventQueue(
            mSensorManager, looper, 0, nullptr, nullptr);
    mSensorEventQueue = queue;

    ASensorEventQueue_enableSensor(queue, mGyroSensor);
    ASensorEventQueue_setEventRate(queue, mGyroSensor, 5000); // 200Hz

    if (mAccelSensor) {
        ASensorEventQueue_enableSensor(queue, mAccelSensor);
        ASensorEventQueue_setEventRate(queue, mAccelSensor, 20000); // 50Hz
    }

    glm::vec3 gyroLp(0.0f);

    while (mSensorRunning) {
        // 5ms poll — tight enough to not miss 200Hz events
        ALooper_pollOnce(5, nullptr, nullptr, nullptr);

        ASensorEvent event;
        while (ASensorEventQueue_getEvents(queue, &event, 1) > 0) {

            if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                std::lock_guard<std::mutex> lock(mEisMutex);

                if (mEis.lastGyroTs == 0) {
                    mEis.lastGyroTs = event.timestamp;
                    gyroLp = glm::vec3(event.data[0], event.data[1], event.data[2]);
                    continue;
                }
                float dt = (event.timestamp - mEis.lastGyroTs) * 1e-9f;
                mEis.lastGyroTs = event.timestamp;
                if (dt <= 0.f || dt > 0.05f) continue; // reject invalid dt

                // 1. Time-normalized IIR low-pass filter on raw gyro (15Hz cutoff)
                glm::vec3 rawGyro(event.data[0], event.data[1], event.data[2]);
                constexpr float cutoffHz = 15.0f;
                constexpr float rc = 1.0f / (2.0f * M_PI * cutoffHz);
                float alpha_lpf = dt / (rc + dt);
                gyroLp = gyroLp * (1.f - alpha_lpf) + rawGyro * alpha_lpf;
                mEis.gyroFiltered = gyroLp;

                // 2. Quaternion integration (division-by-zero safe)
                float omega = glm::length(gyroLp);
                float angle = omega * dt;
                glm::quat dq;
                if (angle < 1e-7f) {
                    dq = glm::quat(1.0f, gyroLp.x * dt * 0.5f, gyroLp.y * dt * 0.5f, gyroLp.z * dt * 0.5f);
                } else {
                    glm::vec3 axis = gyroLp / omega;
                    float halfAngle = angle * 0.5f;
                    dq = glm::quat(std::cos(halfAngle), axis.x * std::sin(halfAngle), axis.y * std::sin(halfAngle), axis.z * std::sin(halfAngle));
                }
                mEis.currentQuat = glm::normalize(mEis.currentQuat * dq);

                // 3. Time-normalized trajectory smoothing with continuous smoothstep pan blending
                float panBlend = glm::smoothstep(0.15f, 1.00f, omega);
                float targetAlpha = glm::mix(mEis.smoothingAlpha, 0.55f, panBlend);
                float alpha = 1.0f - std::exp(-dt * 200.0f * targetAlpha);
                mEis.smoothedQuat = glm::normalize(glm::slerp(mEis.smoothedQuat, mEis.currentQuat, alpha));

                // 4. Save raw orientation into history ring-buffer for timestamp interpolation
                mEis.history[mEis.historyIdx].timestampNs = event.timestamp;
                mEis.history[mEis.historyIdx].orientation = mEis.currentQuat;
                mEis.historyIdx = (mEis.historyIdx + 1) % EisState::HISTORY_SIZE;
                if (mEis.historyIdx == 0) mEis.historyFull = true;

                // 5. Push to latency buffer and run 6-state Kalman predictor-corrector
                pushLatencyBuffer(mEis.currentQuat, event.timestamp);
                kalmanPredictUpdate(gyroLp, dt);

            } else if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
                std::lock_guard<std::mutex> lock(mEisMutex);

                glm::vec3 accel(event.data[0], event.data[1], event.data[2]);
                float accelMag = glm::length(accel);

                // 1G Magnitude gate: only fuse gravity when net acceleration magnitude is close to 9.81 m/s²
                // Rejects transient movements (footsteps, knocks, running) that distort gravity estimation
                if (std::abs(accelMag - 9.81f) < 1.5f) {
                    mEis.gravity = mEis.gravity * 0.98f + accel * 0.02f;

                    glm::vec3 g = glm::normalize(mEis.gravity);
                    glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
                    glm::vec3 crossVec = glm::cross(g, worldUp);
                    float crossLen = glm::length(crossVec);

                    if (crossLen > 1e-6f) {
                        float cosAngle = glm::clamp(g.z, -1.0f, 1.0f);
                        float halfAngle = std::acos(cosAngle) * 0.5f;
                        glm::quat qAccel = glm::quat(std::cos(halfAngle),
                                                     crossVec.x * std::sin(halfAngle) / crossLen,
                                                     crossVec.y * std::sin(halfAngle) / crossLen,
                                                     0.0f); // Gravity-leveling tilt correction (pitch/roll only)

                        // Complementary filter fusion (stabilizes tilt drift)
                        mEis.currentQuat = glm::normalize(glm::slerp(qAccel, mEis.currentQuat, 0.98f));
                    }
                }
            }
        }
    }

    if (mGyroSensor)  ASensorEventQueue_disableSensor(queue, mGyroSensor);
    if (mAccelSensor) ASensorEventQueue_disableSensor(queue, mAccelSensor);
    ASensorManager_destroyEventQueue(mSensorManager, queue);
    LOGI("EIS sensor thread exiting cleanly.");
}

glm::quat CameraEngine::getInterpolatedOrientation(int64_t frameTimestampNs) {
    // Note: Caller must hold mEisMutex!
    if (mEis.historyIdx == 0 && !mEis.historyFull) {
        return mEis.currentQuat;
    }

    int count = mEis.historyFull ? EisState::HISTORY_SIZE : mEis.historyIdx;
    
    // Find closest orientation samples before and after the frame presentation timestamp
    int lowerIdx = -1;
    int upperIdx = -1;
    int64_t minDiffUpper = INT64_MAX;
    int64_t minDiffLower = INT64_MAX;

    for (int i = 0; i < count; ++i) {
        int64_t ts = mEis.history[i].timestampNs;
        if (ts >= frameTimestampNs) {
            int64_t diff = ts - frameTimestampNs;
            if (diff < minDiffUpper) {
                minDiffUpper = diff;
                upperIdx = i;
            }
        } else {
            int64_t diff = frameTimestampNs - ts;
            if (diff < minDiffLower) {
                minDiffLower = diff;
                lowerIdx = i;
            }
        }
    }

    // Perform spherical linear interpolation between the two closest bounds
    if (lowerIdx != -1 && upperIdx != -1) {
        int64_t t1 = mEis.history[lowerIdx].timestampNs;
        int64_t t2 = mEis.history[upperIdx].timestampNs;
        float t = (float)(frameTimestampNs - t1) / (float)(t2 - t1);
        return glm::normalize(glm::slerp(mEis.history[lowerIdx].orientation, mEis.history[upperIdx].orientation, t));
    }

    return mEis.currentQuat;
}
void CameraEngine::applyAdaptiveInflation(float omegaMag, float innovationMag) {
    // Smooth estimate of motion intensity
    mKalman.motionAvg = mKalman.motionAvg * 0.95f + omegaMag * 0.05f;
    mKalman.motionInflation = 1.0f + 5.0f * mKalman.motionAvg;

    // Smooth estimate of measurement innovation (error between prediction and accel orientation)
    mKalman.innovationAvg = mKalman.innovationAvg * 0.95f + innovationMag * 0.05f;
    mKalman.innovationInflation = 1.0f + 10.0f * glm::clamp(mKalman.innovationAvg, 0.0f, 1.0f);
}

void CameraEngine::kalmanPredictUpdate(const glm::vec3& gyro, float dt) {
    // 1. Predict state (angle in radians, bias constant)
    mKalman.angle += (gyro - mKalman.bias) * dt;

    // 2. Predict covariance analytically: P = F * P * F_T + Q
    float pPrev[6][6];
    std::memcpy(pPrev, mKalman.P, sizeof(pPrev));

    // Analytical equation for F * P * F_T where F = [[I, -I*dt], [0, I]]
    float pNew[6][6];
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            if (i < 3 && j < 3) {
                pNew[i][j] = pPrev[i][j] - dt * (pPrev[i + 3][j] + pPrev[i][j + 3]) + dt * dt * pPrev[i + 3][j + 3];
            } else if (i < 3 && j >= 3) {
                pNew[i][j] = pPrev[i][j] - dt * pPrev[i + 3][j];
            } else if (i >= 3 && j < 3) {
                pNew[i][j] = pPrev[i][j] - dt * pPrev[i][j + 3];
            } else {
                pNew[i][j] = pPrev[i][j];
            }
        }
    }

    // Add process noise covariance Q
    float qAngle = mKalman.Q_angle * mKalman.motionInflation;
    float qBias = mKalman.Q_bias;
    for (int i = 0; i < 3; ++i) {
        pNew[i][i] += qAngle;
        pNew[i + 3][i + 3] += qBias;
    }
    std::memcpy(mKalman.P, pNew, sizeof(mKalman.P));

    // 3. Prepare measurement z from filtered accelerometer + raw yaw
    float ax = mEis.gravity.x;
    float ay = mEis.gravity.y;
    float az = mEis.gravity.z;
    float roll = std::atan2(ay, az);
    float pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az));

    // Yaw comes from raw integrated gyro (pitch = x, yaw = y, roll = z in glm::eulerAngles)
    glm::vec3 rawEuler = glm::eulerAngles(mEis.currentQuat);
    float yaw = rawEuler.y;

    glm::vec3 z(roll, pitch, yaw);

    // 4. Update step
    // Innovation y = z - H * x
    glm::vec3 y = z - mKalman.angle;

    // Update adaptive inflation factors
    applyAdaptiveInflation(glm::length(gyro), glm::length(y));

    // Innovation covariance S = H * P * H_T + R * I
    glm::mat3 S;
    float rVal = mKalman.R * mKalman.innovationInflation;
    S[0][0] = mKalman.P[0][0] + rVal; S[1][0] = mKalman.P[1][0];        S[2][0] = mKalman.P[2][0];
    S[0][1] = mKalman.P[0][1];        S[1][1] = mKalman.P[1][1] + rVal; S[2][1] = mKalman.P[2][1];
    S[0][2] = mKalman.P[0][2];        S[1][2] = mKalman.P[1][2];        S[2][2] = mKalman.P[2][2] + rVal;

    glm::mat3 S_inv = glm::inverse(S);

    // Kalman gain K = P * H_T * S_inv (dimensions 6x3)
    float K[6][3];
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 3; ++j) {
            // Row i of P_left multiplied by Column j of S_inv (column-major)
            K[i][j] = mKalman.P[i][0] * S_inv[j].x + mKalman.P[i][1] * S_inv[j].y + mKalman.P[i][2] * S_inv[j].z;
        }
    }

    // Update state estimate: x = x + K * y
    float dx[6] = {0.0f};
    for (int row = 0; row < 6; ++row) {
        dx[row] = K[row][0] * y.x + K[row][1] * y.y + K[row][2] * y.z;
    }

    mKalman.angle.x += dx[0];
    mKalman.angle.y += dx[1];
    mKalman.angle.z += dx[2];
    mKalman.bias.x  += dx[3];
    mKalman.bias.y  += dx[4];
    mKalman.bias.z  += dx[5];
    // Update covariance estimate: P = (I - K * H) * P = P - K * H * P
    float KH_P[6][6];
    for (int col = 0; col < 6; ++col) {
        for (int row = 0; row < 6; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += K[row][k] * mKalman.P[k][col];
            }
            KH_P[row][col] = sum;
        }
    }
    for (int col = 0; col < 6; ++col) {
        for (int row = 0; row < 6; ++row) {
            mKalman.P[row][col] -= KH_P[row][col];
        }
    }
}

void CameraEngine::pushLatencyBuffer(const glm::quat& q, int64_t ts) {
    mLatencyBuffer.buffer[mLatencyBuffer.head] = {q, ts};
    mLatencyBuffer.head = (mLatencyBuffer.head + 1) % EisLatencyBuffer::SIZE;
    if (mLatencyBuffer.count < EisLatencyBuffer::SIZE) {
        mLatencyBuffer.count++;
    }
}

glm::quat CameraEngine::getCompensatedFromBuffer(int64_t frameTs, float latencyMs) {
    // Note: Caller must hold mEisMutex!
    int64_t targetTs = frameTs - static_cast<int64_t>(latencyMs * 1000000.0f);

    if (mLatencyBuffer.count == 0) {
        return mEis.currentQuat;
    }

    // Find closest orientation samples before and after targetTs
    int lowerIdx = -1;
    int upperIdx = -1;
    int64_t minDiffUpper = INT64_MAX;
    int64_t minDiffLower = INT64_MAX;

    for (int i = 0; i < mLatencyBuffer.count; ++i) {
        int64_t ts = mLatencyBuffer.buffer[i].ts;
        if (ts >= targetTs) {
            int64_t diff = ts - targetTs;
            if (diff < minDiffUpper) {
                minDiffUpper = diff;
                upperIdx = i;
            }
        } else {
            int64_t diff = targetTs - ts;
            if (diff < minDiffLower) {
                minDiffLower = diff;
                lowerIdx = i;
            }
        }
    }

    if (lowerIdx != -1 && upperIdx != -1) {
        int64_t t1 = mLatencyBuffer.buffer[lowerIdx].ts;
        int64_t t2 = mLatencyBuffer.buffer[upperIdx].ts;
        float t = (float)(targetTs - t1) / (float)(t2 - t1);
        return glm::normalize(glm::slerp(mLatencyBuffer.buffer[lowerIdx].quat, mLatencyBuffer.buffer[upperIdx].quat, t));
    }

    // Fallback to latest sample in buffer
    int latestIdx = (mLatencyBuffer.head - 1 + EisLatencyBuffer::SIZE) % EisLatencyBuffer::SIZE;
    return mLatencyBuffer.buffer[latestIdx].quat;
}


glm::mat4 CameraEngine::getEisTransform(int64_t frameTimestampNs, float zoomRatio) {
    std::lock_guard<std::mutex> lock(mEisMutex);

    if (!mUseStabilization) {
        return glm::mat4(1.0f); // Identity matrix
    }

    // 1. Get raw orientation from latency buffer (compensated for 45ms camera pipeline lag)
    glm::quat compensated = getCompensatedFromBuffer(frameTimestampNs, 45.0f);

    // 2. Get filtered/smoothed orientation from Kalman filter
    glm::quat filtered = glm::normalize(glm::quat(mKalman.angle));

    // 3. Deviation calculation using 3D rotation matrix (preserves coupling, avoids gimbal lock)
    glm::quat deviation = glm::inverse(filtered) * compensated;
    glm::mat3 rotMat = glm::mat3_cast(deviation);

    // Extract roll (Z-axis rotation), yaw (Y-axis), and pitch (X-axis) rotation angles
    float roll = std::atan2(rotMat[0][1], rotMat[0][0]);
    float yaw = std::asin(rotMat[2][0]);
    float pitch = std::asin(-rotMat[2][1]);

    // 4. Physical Camera Projection Model (focal length normalized mapping)
    // maps physical angles to exact pixel shifts in normalized coordinate space
    float tx = std::tan(-yaw) / std::tan(mEis.fovX * 0.5f);
    float ty = std::tan(-pitch) / std::tan(mEis.fovY * 0.5f);

    // 5. Pan-adaptive strength scaling (continuous blend using smoothstep)
    float omega = glm::length(mEis.gyroFiltered);
    float panBlend = glm::smoothstep(0.15f, 1.00f, omega);
    float adaptive = glm::mix(1.0f, 0.5f, panBlend);
    float strength = adaptive / glm::max(zoomRatio, 1.0f);

    float offsetX = glm::clamp(tx * strength, -mEis.maxCorrection, mEis.maxCorrection);
    float offsetY = glm::clamp(ty * strength, -mEis.maxCorrection, mEis.maxCorrection);

    // 6. Zoom-adaptive crop scale: dynamically crops more at high zoom to provide extra stabilization headroom
    float baseCrop = mEis.cropScale;
    float zoomPenalty = 0.05f * std::max(0.0f, zoomRatio - 1.0f);
    float finalScale = std::max(baseCrop - zoomPenalty, 0.60f);

    // Create complete 2D affine transform matrix: translate * rotate * scale
    glm::mat4 trans = glm::translate(glm::mat4(1.0f), glm::vec3(offsetX, offsetY, 0.0f));
    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), -roll, glm::vec3(0.0f, 0.0f, 1.0f)); // Roll compensation
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(finalScale, finalScale, 1.0f));

    return trans * rot * scale;
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

        // (EIS transform query is moved to step 5 below to sync with exact frame timestamp)


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

        // 5. Snapshot render state (lock-free — no mutex held during GL work)
        //    This allows setZoom/setOis/etc. to acquire mCameraMutex immediately
        //    without waiting for a full frame render to complete.
        bool   snapRecording = mIsRecording.load();
        bool   snapIsFront   = (mActiveLensFacing == 0);

        // Update SurfaceTexture frame and render GPU passes
        env->CallVoidMethod(mSurfaceTextureRef, mUpdateTexImageMethod);
        jlong timestampNs = env->CallLongMethod(mSurfaceTextureRef, mGetTimestampMethod);

        // 5. Query timestamp-aligned, latency-compensated EIS transform
        glm::mat4 eisMat = getEisTransform(timestampNs, mZoomRatio);

        mGlRenderer->renderFrame(eisMat, snapRecording, timestampNs, snapIsFront);

        // 6. Read back hardware downsampled luminance for next PID iteration
        mLastLuma = mGlRenderer->readAverageLuma();

        // 7. Update histogram cached data on the loop thread (holds current EGL context)
        int32_t tempHist[64];
        if (mGlRenderer->getHistogram(tempHist, 64)) {
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

    // Update repeating request with new focus/metering region (trigger stays IDLE)
    updateRepeatingRequest();

    // Send AF_TRIGGER_START as a SINGLE BURST CAPTURE — not via the repeating request.
    // This guarantees the HAL sees exactly one START frame before returning to IDLE.
    // Using the repeating request for START causes a race: the IDLE overwrite may arrive
    // before the HAL processes the START, losing the trigger intermittently.
    ACaptureRequest* triggerReq = nullptr;
    if (ACameraDevice_createCaptureRequest(mCameraDevice, TEMPLATE_RECORD, &triggerReq) == ACAMERA_OK && triggerReq) {
        // Copy current AF mode and region settings into the one-shot trigger request
        uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        ACaptureRequest_setEntry_u8(triggerReq, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
        ACaptureRequest_setEntry_i32(triggerReq, ACAMERA_CONTROL_AF_REGIONS, 5, box);
        ACaptureRequest_setEntry_i32(triggerReq, ACAMERA_CONTROL_AE_REGIONS, 5, box);

        uint8_t afStart = ACAMERA_CONTROL_AF_TRIGGER_START;
        ACaptureRequest_setEntry_u8(triggerReq, ACAMERA_CONTROL_AF_TRIGGER, 1, &afStart);

        ACameraCaptureSession_capture(mCaptureSession, nullptr, 1, &triggerReq, nullptr);
        ACaptureRequest_free(triggerReq);
    }
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
