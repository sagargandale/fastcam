#include "GlRenderer.h"
#include <android/log.h>
#include <algorithm>
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
uniform float uCropScale;  // 0.85 for EIS (15% headroom), 1.0 for OIS/disabled
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    // Scale UV around centre to create stabilisation headroom without black borders.
    vec2 centered = (aTexCoord - 0.5) * uCropScale;
    
    // In portrait mode, the screen coordinates are rotated 90 degrees relative
    // to the sensor coordinates. To shift horizontally/vertically on the screen,
    // we must swap the shift axes mapping to the sensor.
    vec2 shift = (uRotate > 0.5) ? vec2(uShiftY, uShiftX) : vec2(uShiftX, uShiftY);
    vec2 shifted  = centered + 0.5 + shift;

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
uniform float uHdrEnabled;
out vec4 fragColor;

// Shadow recovery: lifts the dark toe region ONLY (pow-4 falloff protects midtones & highlights)
float shadowLift(float x, float lift) {
    return x + lift * pow(1.0 - x, 4.0);
}

// Highlight roll-off: compresses values above 0.5 gracefully — branchless via step/mix
// to eliminate GPU warp divergence on mobile Adreno/Mali architectures.
float highlightCompress(float x) {
    float overshoot = max(0.0, x - 0.5);
    float compressed = 0.5 + overshoot / (1.0 + overshoot * 1.5);
    return mix(x, compressed, step(0.5, x));
}

void main() {
    vec4 raw = texture(uTexture, vTexCoord);
    if (uHdrEnabled < 0.5) {
        fragColor = raw;
        return;
    }

    // --- HDR Computational Mode ---
    // Step 1: Moderate global gain to pull down highlights and avoid overexposed look
    vec3 color = raw.rgb * 0.90;

    // Step 2: Recover shadow detail in the toe region (protecting midtones)
    color.r = shadowLift(color.r, 0.15);
    color.g = shadowLift(color.g, 0.12);
    color.b = shadowLift(color.b, 0.14);

    // Step 3: Compress highlights above 0.5 smoothly
    color.r = highlightCompress(color.r);
    color.g = highlightCompress(color.g);
    color.b = highlightCompress(color.b);

    // Step 4: Boost saturation for rich, cinematic look
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, 1.15);
    color = clamp(color, 0.0, 1.0);

    fragColor = vec4(color, raw.a);
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

    if (!createHistFbo()) {
        LOGE("Failed to create histogram readback FBO!");
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
    if (mLumaPbo[0]) { glDeleteBuffers(2, mLumaPbo); mLumaPbo[0] = 0; mLumaPbo[1] = 0; }
    mPboReady = false;
    mPboIndex = 0;

    if (mHistFbo) { glDeleteFramebuffers(1, &mHistFbo); mHistFbo = 0; }
    if (mHistTexture) { glDeleteTextures(1, &mHistTexture); mHistTexture = 0; }
    if (mHistPbo[0]) { glDeleteBuffers(2, mHistPbo); mHistPbo[0] = 0; mHistPbo[1] = 0; }
    mHistPboReady = false;
    mHistPboIndex = 0;

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

    // 4. Render to histogram FBO for real-time histogram (throttled to every 4th frame)
    // The histogram is used for UI display only — 15fps refresh is more than enough.
    // At 60fps this skips 3 of every 4 histogram draws, saving ~20% GPU work.
    static int sHistThrottle = 0;
    if (++sHistThrottle >= 4) {
        sHistThrottle = 0;

        glBindFramebuffer(GL_FRAMEBUFFER, mHistFbo);
        glViewport(0, 0, HIST_SIZE, HIST_SIZE);

        glUseProgram(mLumaProgram); // Reuse same program to generate grayscale output
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, mCameraTextureId);
        glUniform1i(mLumaUniformTexture, 0);

        glBindVertexArray(mQuadVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer::renderQuad(float shiftX, float shiftY, bool rotate, bool isFront) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(mShaderProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mCameraTextureId);
    glUniform1i(mUniformTexture,    0);
    glUniform1f(mUniformShiftX,     shiftX);
    glUniform1f(mUniformShiftY,     shiftY);
    glUniform1f(mUniformRotate,     rotate   ? 1.0f : 0.0f);
    glUniform1f(mUniformIsFront,    isFront  ? 1.0f : 0.0f);
    glUniform1f(mUniformHdrEnabled, mHdrEnabled ? 1.0f : 0.0f);
    glUniform1f(mUniformCropScale,  mEisCropScale);

    glBindVertexArray(mQuadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

float GlRenderer::readAverageLuma() {
    // PBO double-buffering for async luminance readback:
    // - Frame N: issue DMA transfer into mLumaPbo[writeIdx] (non-blocking)
    // - Frame N+1: map mLumaPbo[readIdx] to get frame N's data (GPU DMA complete by now)
    int writeIdx = mPboIndex;
    int readIdx  = 1 - mPboIndex;
    mPboIndex    = readIdx; // Alternate for next frame

    // Issue async GPU→CPU DMA transfer of the luma FBO into the write PBO
    glBindFramebuffer(GL_FRAMEBUFFER, mLumaFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mLumaPbo[writeIdx]);
    glReadPixels(0, 0, LUMA_SIZE, LUMA_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!mPboReady) {
        // First frame: PBO[readIdx] has no data yet, return neutral luma
        mPboReady = true;
        return 0.5f;
    }

    // Map the PBO written in the PREVIOUS frame (DMA should be complete by now)
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mLumaPbo[readIdx]);
    const uint8_t* pixels = (const uint8_t*)glMapBufferRange(
        GL_PIXEL_PACK_BUFFER, 0, LUMA_SIZE * LUMA_SIZE * 4, GL_MAP_READ_BIT);

    float weightedSum = 0.0f;
    float totalWeight = 0.0f;

    if (pixels) {
        for (int row = 0; row < LUMA_SIZE; ++row) {
            for (int col = 0; col < LUMA_SIZE; ++col) {
                int i = row * LUMA_SIZE + col;

                // Center-weighted metering (1.0 at center, 0.3 at corners)
                float dx = (float)col - 7.5f;
                float dy = (float)row - 7.5f;
                float distSq = dx * dx + dy * dy;
                float weight = 1.0f - 0.7f * (distSq / 112.5f);

                // Rec.709 luminance from red channel (already greyscale from luma shader)
                float luma = pixels[i * 4] / 255.0f;
                weightedSum += luma * weight;
                totalWeight += weight;
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return (totalWeight > 0.0f) ? (weightedSum / totalWeight) : 0.0f;
}

void GlRenderer::setHdrEnabled(bool enabled) {
    mHdrEnabled = enabled;
}

void GlRenderer::setEisCropScale(float scale) {
    // Clamp: 1.0 = no crop (OIS/disabled), 0.85 = 15% headroom (EIS active)
    mEisCropScale = std::max(0.80f, std::min(scale, 1.0f));
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

    mUniformTexture    = glGetUniformLocation(mShaderProgram, "uTexture");
    mUniformShiftX     = glGetUniformLocation(mShaderProgram, "uShiftX");
    mUniformShiftY     = glGetUniformLocation(mShaderProgram, "uShiftY");
    mUniformRotate     = glGetUniformLocation(mShaderProgram, "uRotate");
    mUniformIsFront    = glGetUniformLocation(mShaderProgram, "uIsFront");
    mUniformHdrEnabled = glGetUniformLocation(mShaderProgram, "uHdrEnabled");
    mUniformCropScale  = glGetUniformLocation(mShaderProgram, "uCropScale");

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

    // Create two PBOs for async GPU→CPU DMA readback (eliminates glReadPixels pipeline stall)
    glGenBuffers(2, mLumaPbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, mLumaPbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, LUMA_SIZE * LUMA_SIZE * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mPboIndex = 0;
    mPboReady = false;

    LOGI("Luminance readback FBO + PBO created (%dx%d).", LUMA_SIZE, LUMA_SIZE);
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

bool GlRenderer::createHistFbo() {
    // FBO with RGBA8 texture for histogram downsampling (64x64)
    glGenFramebuffers(1, &mHistFbo);
    glGenTextures(1, &mHistTexture);

    glBindTexture(GL_TEXTURE_2D, mHistTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, HIST_SIZE, HIST_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, mHistFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mHistTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Hist FBO incomplete: 0x%x", status);
        return false;
    }

    // Create two PBOs for asynchronous readback
    glGenBuffers(2, mHistPbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, mHistPbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, HIST_SIZE * HIST_SIZE * 4, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mHistPboIndex = 0;
    mHistPboReady = false;

    LOGI("Histogram readback FBO + PBO created (%dx%d).", HIST_SIZE, HIST_SIZE);
    return true;
}

void GlRenderer::getHistogram(int32_t* outBins, int binCount) {
    std::memset(outBins, 0, binCount * sizeof(int32_t));

    int writeIdx = mHistPboIndex;
    int readIdx  = 1 - mHistPboIndex;
    mHistPboIndex = readIdx;

    glBindFramebuffer(GL_FRAMEBUFFER, mHistFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mHistPbo[writeIdx]);
    glReadPixels(0, 0, HIST_SIZE, HIST_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!mHistPboReady) {
        mHistPboReady = true;
        return;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, mHistPbo[readIdx]);
    const uint8_t* pixels = (const uint8_t*)glMapBufferRange(
        GL_PIXEL_PACK_BUFFER, 0, HIST_SIZE * HIST_SIZE * 4, GL_MAP_READ_BIT);

    if (pixels) {
        for (int i = 0; i < HIST_SIZE * HIST_SIZE; ++i) {
            float luma = pixels[i * 4] / 255.0f;
            int bin = (int)(luma * (binCount - 1));
            if (bin >= 0 && bin < binCount) {
                outBins[bin]++;
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
