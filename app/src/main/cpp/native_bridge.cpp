#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "CameraEngine.h"

#define LOG_TAG "[NativeBridge]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static CameraEngine* gEngine = nullptr;
static JavaVM* gJavaVM = nullptr;

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    gJavaVM = vm;
    return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_fastcam_engine_NativeBridge_nativeInit(JNIEnv* env, jobject thiz, jobject surface, jint width, jint height, jboolean useStabilization) {
    (void)thiz;
    if (gEngine) {
        LOGW("CameraEngine is already initialized, releasing first.");
        gEngine->releaseCamera();
        delete gEngine;
        gEngine = nullptr;
    }

    gEngine = new CameraEngine(gJavaVM);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("Failed to get ANativeWindow from Surface");
        delete gEngine;
        gEngine = nullptr;
        return JNI_FALSE;
    }

    bool success = gEngine->initCamera(env, window, width, height, useStabilization);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeStartRecording(JNIEnv* env, jobject thiz, jint fd) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->startRecording(fd);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeStopRecording(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->stopRecording();
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeRelease(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->releaseCamera();
        delete gEngine;
        gEngine = nullptr;
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetMode(JNIEnv* env, jobject thiz, jboolean isAuto) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setMode(isAuto);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetFocus(JNIEnv* env, jobject thiz, jboolean autoFocus, jfloat distance) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setFocus(autoFocus, distance);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetExposure(JNIEnv* env, jobject thiz, jboolean autoExposure, jint iso, jlong shutterNs) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setExposure(autoExposure, iso, shutterNs);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetFrameRate(JNIEnv* env, jobject thiz, jint fps) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setFrameRate(fps);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetZoom(JNIEnv* env, jobject thiz, jfloat ratio) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setZoom(ratio);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetNoiseReduction(JNIEnv* env, jobject thiz, jbyte mode) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setNoiseReduction(mode);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetFocusPoint(JNIEnv* env, jobject thiz, jfloat x, jfloat y, jint viewW, jint viewH) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setFocusPoint(x, y, viewW, viewH);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetExposureCompensation(JNIEnv* env, jobject thiz, jint value) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setExposureCompensation(value);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativePushAudioFrame(JNIEnv* env, jobject thiz, jbyteArray data, jint size) {
    (void)thiz;
    if (gEngine && data) {
        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (bytes) {
            gEngine->pushAudioFrame(reinterpret_cast<const uint8_t*>(bytes), size);
            env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetLens(JNIEnv* env, jobject thiz, jstring lensId) {
    (void)thiz;
    if (gEngine && lensId) {
        const char* idStr = env->GetStringUTFChars(lensId, nullptr);
        if (idStr) {
            gEngine->setLens(idStr);
            env->ReleaseStringUTFChars(lensId, idStr);
        }
    }
}

JNIEXPORT jobjectArray JNICALL
Java_com_fastcam_engine_NativeBridge_nativeGetAvailableLenses(JNIEnv* env, jobject thiz) {
    (void)thiz;
    if (!gEngine) {
        // Create a temporary CameraEngine to list lenses if engine hasn't been initialized yet
        CameraEngine tempEngine(gJavaVM);
        auto lenses = tempEngine.getAvailableLenses();
        
        jclass clazz = env->FindClass("com/fastcam/engine/CameraLens");
        jmethodID constructor = env->GetMethodID(clazz, "<init>", "(Ljava/lang/String;FI)V");
        
        jobjectArray array = env->NewObjectArray(lenses.size(), clazz, nullptr);
        for (size_t i = 0; i < lenses.size(); ++i) {
            jstring id = env->NewStringUTF(lenses[i].id.c_str());
            jobject obj = env->NewObject(clazz, constructor, id, lenses[i].focalLength, lenses[i].facing);
            env->SetObjectArrayElement(array, i, obj);
            env->DeleteLocalRef(id);
            env->DeleteLocalRef(obj);
        }
        env->DeleteLocalRef(clazz);
        return array;
    }

    auto lenses = gEngine->getAvailableLenses();
    jclass clazz = env->FindClass("com/fastcam/engine/CameraLens");
    jmethodID constructor = env->GetMethodID(clazz, "<init>", "(Ljava/lang/String;FI)V");
    
    jobjectArray array = env->NewObjectArray(lenses.size(), clazz, nullptr);
    for (size_t i = 0; i < lenses.size(); ++i) {
        jstring id = env->NewStringUTF(lenses[i].id.c_str());
        jobject obj = env->NewObject(clazz, constructor, id, lenses[i].focalLength, lenses[i].facing);
        env->SetObjectArrayElement(array, i, obj);
        env->DeleteLocalRef(id);
        env->DeleteLocalRef(obj);
    }
    env->DeleteLocalRef(clazz);
    return array;
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetStabilization(JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setStabilization(enabled);
    }
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetOis(JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setOis(enabled);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_fastcam_engine_NativeBridge_nativeIsStabilizationActive(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (gEngine) {
        return gEngine->isStabilizationActive() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_fastcam_engine_NativeBridge_nativeSetAeMode(JNIEnv* env, jobject thiz, jint mode) {
    (void)env; (void)thiz;
    if (gEngine) {
        gEngine->setAeMode(mode);
    }
}

} // extern "C"
