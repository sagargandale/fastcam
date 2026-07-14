#ifndef GL_RENDERER_H
#define GL_RENDERER_H

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <glm/glm.hpp>

/**
 * OpenGL ES 3.1 GPU rendering pipeline.
 * Handles EGL context, external camera texture, preview rendering with EIS,
 * encoder surface rendering, and luminance readback.
 */
class GlRenderer {
public:
    GlRenderer();
    ~GlRenderer();

    // EGL lifecycle
    bool initEgl(ANativeWindow* window);
    void bindEgl();
    void unbindEgl();
    void releaseEgl();

    // GL setup and teardown
    bool setupGl(int width, int height);
    void releaseGl();

    // Camera texture
    GLuint getCameraTextureId() const;

    // Encoder surface for recording
    void createEncoderSurface(ANativeWindow* encoderWindow);
    void destroyEncoderSurface();

    // Render a frame with EIS transform matrix and optional encoder output
    void renderFrame(const glm::mat4& eisMat, bool recording, int64_t timestampNs, bool isFront);

    // Read average luminance from last rendered frame (for PID exposure)
    float readAverageLuma();

    // Compute histogram from last rendered frame (returns true if a new frame was read)
    bool getHistogram(int32_t* outBins, int binCount);

    // Enable/disable real-time computational HDR tone-mapping
    void setHdrEnabled(bool enabled);

    // Set EIS crop scale: 0.85 when EIS is active (15% headroom), 1.0 when OIS/disabled.
    // Call before each frame or when stabilization mode changes.
    void setEisCropScale(float scale);

private:
    bool createShaderProgram();
    bool createLumaFbo();
    bool createHistFbo();
    GLuint compileShader(GLenum type, const char* source);
    void renderQuad(const glm::mat4& eisMat, bool rotate, bool isFront);

    // EGL objects
    EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
    EGLConfig mEglConfig = nullptr;
    EGLContext mEglContext = EGL_NO_CONTEXT;
    EGLSurface mPreviewSurface = EGL_NO_SURFACE;
    EGLSurface mEncoderSurface = EGL_NO_SURFACE;

    // GL objects
    GLuint mCameraTextureId = 0;
    GLuint mShaderProgram = 0;
    GLint mUniformTexture    = -1;
    GLint mUniformEisMat     = -1;
    GLint mUniformRotate     = -1;
    GLint mUniformIsFront    = -1;
    GLint mUniformHdrEnabled = -1;
    GLint mUniformCropScale  = -1; // EIS crop scale uniform
    GLuint mQuadVao = 0;
    GLuint mQuadVbo = 0;

    // Point metering FBO
    GLuint mLumaFbo = 0;
    GLuint mLumaTexture = 0;
    GLuint mLumaProgram = 0;
    GLint mLumaUniformTexture = -1;
    static const int LUMA_SIZE = 16; // 16x16 downscale for average

    // Histogram FBO (64x64 downscale)
    GLuint mHistFbo = 0;
    GLuint mHistTexture = 0;
    GLuint mHistPbo[2] = {0, 0};
    int mHistPboIndex = 0;
    bool mHistPboReady = false;
    static const int HIST_SIZE = 64;

    int mWidth = 1920;
    int mHeight = 1080;
    int mPreviewWidth = 0;
    int mPreviewHeight = 0;
    int64_t mRecordingStartTimestampNs = -1;
    bool mHdrEnabled = false;
    float mEisCropScale = 0.85f; // 0.85 for EIS (15% headroom), 1.0 for OIS or disabled

    // PBO double-buffer for asynchronous luminance readback.
    // Eliminates GPU pipeline stall: issue DMA on frame N, read result on frame N+1.
    GLuint mLumaPbo[2] = {0, 0};
    int  mPboIndex  = 0;    // Write index (0 or 1, alternates each frame)
    bool mPboReady  = false; // False until first PBO write is issued
};

#endif // GL_RENDERER_H
