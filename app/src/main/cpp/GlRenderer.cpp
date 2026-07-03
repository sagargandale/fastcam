#include "GlRenderer.h"
#include <android/log.h>
#include <cstring>
#include <cmath>

#define LOG_TAG "[GlRenderer]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ────────────────────────────────────────────────────────────────
// Shaders
// ────────────────────────────────────────────────────────────────

// Camera preview shader — renders external OES texture with EIS crop/shift
static const char* VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
uniform float uShiftX;
uniform float uShiftY;
uniform float uRotate;
uniform float uIsFront;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    // Apply EIS offset via texture coordinate shift (crops edges for stabilization margin)
    float cropScale = 0.9; // 10% crop for EIS headroom
    vec2 centered = (aTexCoord - 0.5) * cropScale;
    vec2 shifted = centered + 0.5 + vec2(uShiftX, uShiftY);
    if (uRotate > 0.5) {
        if (uIsFront > 0.5) {
            // Front Camera Portrait Preview: mirror horizontally (flip y-axis in rotated projection)
            vTexCoord = vec2(shifted.y, 1.0 - shifted.x);
        } else {
            // Back Camera Portrait Preview
            vTexCoord = vec2(1.0 - shifted.y, 1.0 - shifted.x);
        }
    } else {
        if (uIsFront > 0.5) {
            // Front Camera Landscape Recording: mirror horizontally, keep vertical correct
            vTexCoord = vec2(1.0 - shifted.x, 1.0 - shifted.y);
        } else {
            // Back Camera Landscape Recording
            vTexCoord = vec2(shifted.x, 1.0 - shifted.y);
        }
    }
})";

static const char* FRAGMENT_SHADER = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vTexCoord;
uniform samplerExternalOES uTexture;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vTexCoord);
})";

// Luminance downscale shader — reads camera texture and outputs grayscale for metering
static const char* LUMA_VERTEX = R"(#version 300 es
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
})";

static const char* LUMA_FRAGMENT = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vTexCoord;
uniform samplerExternalOES uTexture;
out vec4 fragColor;
void main() {
    vec4 color = texture(uTexture, vTexCoord);
    // Rec. 709 luminance coefficients
    float luma = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(luma, luma, luma, 1.0);
})";

// Full-screen quad vertices: position (xy) + texcoord (uv)
static const float QUAD_VERTICES[] = {
    // pos        // uv
    -1.0f,  1.0f, 0.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

// ────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────

GlRenderer::GlRenderer() = default;

GlRenderer::~GlRenderer() {
    releaseGl();
    releaseEgl();
}

bool GlRenderer::initEgl(ANativeWindow* window) {
    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEglDisplay == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(mEglDisplay, &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }
    LOGI("EGL initialized: %d.%d", major, minor);

    // Config: RGBA8888, recordable (for MediaCodec encoder surface)
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_NONE
    };

    EGLint numConfigs;
    eglChooseConfig(mEglDisplay, configAttribs, &mEglConfig, 1, &numConfigs);
    if (numConfigs == 0) {
        LOGE("eglChooseConfig returned 0 configs");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (mEglContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    mPreviewSurface = eglCreateWindowSurface(mEglDisplay, mEglConfig, window, nullptr);
    if (mPreviewSurface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed for preview");
        return false;
    }

    if (!eglMakeCurrent(mEglDisplay, mPreviewSurface, mPreviewSurface, mEglContext)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    EGLint previewWidth = 0;
    EGLint previewHeight = 0;
    eglQuerySurface(mEglDisplay, mPreviewSurface, EGL_WIDTH, &previewWidth);
    eglQuerySurface(mEglDisplay, mPreviewSurface, EGL_HEIGHT, &previewHeight);
    mPreviewWidth = previewWidth;
    mPreviewHeight = previewHeight;

    LOGI("EGL context and preview surface created successfully. Size: %dx%d", mPreviewWidth, mPreviewHeight);
    return true;
}

void GlRenderer::bindEgl() {
    if (mEglDisplay != EGL_NO_DISPLAY && mEglContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(mEglDisplay, mPreviewSurface, mPreviewSurface, mEglContext);
    }
}

void GlRenderer::unbindEgl() {
    if (mEglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

void GlRenderer::releaseEgl() {
    if (mEglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (mPreviewSurface != EGL_NO_SURFACE) {
            eglDestroySurface(mEglDisplay, mPreviewSurface);
            mPreviewSurface = EGL_NO_SURFACE;
        }
        if (mEncoderSurface != EGL_NO_SURFACE) {
            eglDestroySurface(mEglDisplay, mEncoderSurface);
            mEncoderSurface = EGL_NO_SURFACE;
        }
        if (mEglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(mEglDisplay, mEglContext);
            mEglContext = EGL_NO_CONTEXT;
        }

        eglTerminate(mEglDisplay);
        mEglDisplay = EGL_NO_DISPLAY;
    }
    LOGI("EGL released.");
}

// ────────────────────────────────────────────────────────────────
// GL Setup
// ────────────────────────────────────────────────────────────────

bool GlRenderer::setupGl(int width, int height) {
    mWidth = width;
    mHeight = height;

    // Create external OES texture for camera frames
    glGenTextures(1, &mCameraTextureId);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mCameraTextureId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    if (!createShaderProgram()) {
        LOGE("Failed to create camera shader program!");
        return false;
    }

    if (!createLumaFbo()) {
        LOGE("Failed to create luminance readback FBO!");
        return false;
    }

    // Create full-screen quad VAO/VBO
    glGenVertexArrays(1, &mQuadVao);
    glGenBuffers(1, &mQuadVbo);

    glBindVertexArray(mQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTICES), QUAD_VERTICES, GL_STATIC_DRAW);

    // Position attribute (location 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // TexCoord attribute (location 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);

    LOGI("OpenGL ES pipeline initialized (%dx%d).", width, height);
    return true;
}

void GlRenderer::releaseGl() {
    if (mShaderProgram) { glDeleteProgram(mShaderProgram); mShaderProgram = 0; }
    if (mLumaProgram) { glDeleteProgram(mLumaProgram); mLumaProgram = 0; }
    if (mCameraTextureId) { glDeleteTextures(1, &mCameraTextureId); mCameraTextureId = 0; }
    if (mLumaTexture) { glDeleteTextures(1, &mLumaTexture); mLumaTexture = 0; }
    if (mLumaFbo) { glDeleteFramebuffers(1, &mLumaFbo); mLumaFbo = 0; }
    if (mQuadVbo) { glDeleteBuffers(1, &mQuadVbo); mQuadVbo = 0; }
    if (mQuadVao) { glDeleteVertexArrays(1, &mQuadVao); mQuadVao = 0; }
    LOGI("GL resources released.");
}

GLuint GlRenderer::getCameraTextureId() const {
    return mCameraTextureId;
}

// ────────────────────────────────────────────────────────────────
// Encoder Surface
// ────────────────────────────────────────────────────────────────

void GlRenderer::createEncoderSurface(ANativeWindow* encoderWindow) {
    if (mEncoderSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEncoderSurface);
    }
    mEncoderSurface = eglCreateWindowSurface(mEglDisplay, mEglConfig, encoderWindow, nullptr);
    mRecordingStartTimestampNs = -1;
    if (mEncoderSurface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL encoder surface!");
    } else {
        LOGI("Encoder EGL surface created.");
    }
}

void GlRenderer::destroyEncoderSurface() {
    if (mEncoderSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEncoderSurface);
        mEncoderSurface = EGL_NO_SURFACE;
        LOGI("Encoder EGL surface destroyed.");
    }
}

// ────────────────────────────────────────────────────────────────
// Rendering
// ────────────────────────────────────────────────────────────────

void GlRenderer::renderFrame(float eisShiftX, float eisShiftY, bool recording, int64_t timestampNs, bool isFront) {
    // 1. Render to preview surface
    eglMakeCurrent(mEglDisplay, mPreviewSurface, mPreviewSurface, mEglContext);
    glViewport(0, 0, mPreviewWidth, mPreviewHeight);
    renderQuad(eisShiftX, eisShiftY, true, isFront); // Rotate preview for portrait view
    eglSwapBuffers(mEglDisplay, mPreviewSurface);

    // 2. Render to encoder surface if recording
    if (recording && mEncoderSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(mEglDisplay, mEncoderSurface, mEncoderSurface, mEglContext);

        if (mRecordingStartTimestampNs < 0) {
            mRecordingStartTimestampNs = timestampNs;
        }
        int64_t relativeTimestampNs = timestampNs - mRecordingStartTimestampNs;

        // Set presentation timestamp for encoder
        eglPresentationTimeANDROID(mEglDisplay, mEncoderSurface, relativeTimestampNs);

        glViewport(0, 0, mWidth, mHeight);
        renderQuad(eisShiftX, eisShiftY, false, isFront); // No rotation for landscape encoder
        eglSwapBuffers(mEglDisplay, mEncoderSurface);

        // Restore preview context
        eglMakeCurrent(mEglDisplay, mPreviewSurface, mPreviewSurface, mEglContext);
    }

    // 3. Render to luminance FBO for exposure metering
    glBindFramebuffer(GL_FRAMEBUFFER, mLumaFbo);
    glViewport(0, 0, LUMA_SIZE, LUMA_SIZE);

    glUseProgram(mLumaProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mCameraTextureId);
    glUniform1i(mLumaUniformTexture, 0);

    glBindVertexArray(mQuadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer::renderQuad(float shiftX, float shiftY, bool rotate, bool isFront) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(mShaderProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mCameraTextureId);
    glUniform1i(mUniformTexture, 0);
    glUniform1f(mUniformShiftX, shiftX);
    glUniform1f(mUniformShiftY, shiftY);
    glUniform1f(mUniformRotate, rotate ? 1.0f : 0.0f);
    glUniform1f(mUniformIsFront, isFront ? 1.0f : 0.0f);

    glBindVertexArray(mQuadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

float GlRenderer::readAverageLuma() {
    glBindFramebuffer(GL_FRAMEBUFFER, mLumaFbo);

    // Read the downscaled luminance pixels
    uint8_t pixels[LUMA_SIZE * LUMA_SIZE * 4];
    glReadPixels(0, 0, LUMA_SIZE, LUMA_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    float weightedSum = 0.0f;
    float totalWeight = 0.0f;

    for (int row = 0; row < LUMA_SIZE; ++row) {
        for (int col = 0; col < LUMA_SIZE; ++col) {
            int i = row * LUMA_SIZE + col;

            // Calculate distance from center (7.5, 7.5)
            float dx = (float)col - 7.5f;
            float dy = (float)row - 7.5f;
            float distSq = dx * dx + dy * dy;
            float maxDistSq = 112.5f; // 7.5^2 + 7.5^2

            // Center-weighted factor (ranges from 1.0 at center to 0.3 at corners)
            float weight = 1.0f - 0.7f * (distSq / maxDistSq);

            // Read Rec.709 luminance from red channel (already converted in shader)
            float luma = pixels[i * 4] / 255.0f;

            weightedSum += luma * weight;
            totalWeight += weight;
        }
    }

    return (totalWeight > 0.0f) ? (weightedSum / totalWeight) : 0.0f;
}

// ────────────────────────────────────────────────────────────────
// Shader Compilation
// ────────────────────────────────────────────────────────────────

bool GlRenderer::createShaderProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SHADER);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (!vs || !fs) return false;

    mShaderProgram = glCreateProgram();
    glAttachShader(mShaderProgram, vs);
    glAttachShader(mShaderProgram, fs);
    glLinkProgram(mShaderProgram);

    GLint linked;
    glGetProgramiv(mShaderProgram, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked) {
        char log[512];
        glGetProgramInfoLog(mShaderProgram, 512, nullptr, log);
        LOGE("Shader link failed: %s", log);
        glDeleteProgram(mShaderProgram);
        mShaderProgram = 0;
        return false;
    }

    mUniformTexture = glGetUniformLocation(mShaderProgram, "uTexture");
    mUniformShiftX = glGetUniformLocation(mShaderProgram, "uShiftX");
    mUniformShiftY = glGetUniformLocation(mShaderProgram, "uShiftY");
    mUniformRotate = glGetUniformLocation(mShaderProgram, "uRotate");
    mUniformIsFront = glGetUniformLocation(mShaderProgram, "uIsFront");

    LOGI("Camera shader program compiled and linked.");
    return true;
}

bool GlRenderer::createLumaFbo() {
    // Compile luma shader
    GLuint vs = compileShader(GL_VERTEX_SHADER, LUMA_VERTEX);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, LUMA_FRAGMENT);
    if (!vs || !fs) return false;

    mLumaProgram = glCreateProgram();
    glAttachShader(mLumaProgram, vs);
    glAttachShader(mLumaProgram, fs);
    glLinkProgram(mLumaProgram);

    GLint linked;
    glGetProgramiv(mLumaProgram, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked) {
        LOGE("Luma shader link failed.");
        return false;
    }

    mLumaUniformTexture = glGetUniformLocation(mLumaProgram, "uTexture");

    // Create FBO with RGBA8 texture
    glGenFramebuffers(1, &mLumaFbo);
    glGenTextures(1, &mLumaTexture);

    glBindTexture(GL_TEXTURE_2D, mLumaTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, LUMA_SIZE, LUMA_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, mLumaFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mLumaTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Luma FBO incomplete: 0x%x", status);
        return false;
    }

    LOGI("Luminance readback FBO created (%dx%d).", LUMA_SIZE, LUMA_SIZE);
    return true;
}

GLuint GlRenderer::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        LOGE("Shader compile error (%s): %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
