#ifndef GL_RENDERER_H
#define GL_RENDERER_H

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>

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

    // Render a frame with EIS offset and optional encoder output
    void renderFrame(float eisShiftX, float eisShiftY, bool recording, int64_t timestampNs, bool isFront);

    // Read average luminance from last rendered frame (for PID exposure)
    float readAverageLuma();

    // Enable/disable real-time computational HDR tone-mapping
    void setHdrEnabled(bool enabled);

private:
    bool createShaderProgram();
    bool createLumaFbo();
    GLuint compileShader(GLenum type, const char* source);
    void renderQuad(float shiftX, float shiftY, bool rotate, bool isFront);

    // EGL objects
    EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
    EGLConfig mEglConfig = nullptr;
    EGLContext mEglContext = EGL_NO_CONTEXT;
    EGLSurface mPreviewSurface = EGL_NO_SURFACE;
    EGLSurface mEncoderSurface = EGL_NO_SURFACE;

    // GL objects
    GLuint mCameraTextureId = 0;
    GLuint mShaderProgram = 0;
    GLint mUniformTexture = -1;
    GLint mUniformShiftX = -1;
    GLint mUniformShiftY = -1;
    GLint mUniformRotate     = -1;
    GLint mUniformIsFront    = -1;
    GLint mUniformHdrEnabled = -1;
    GLuint mQuadVao = 0;
    GLuint mQuadVbo = 0;

    // Point metering FBO
    GLuint mLumaFbo = 0;
    GLuint mLumaTexture = 0;
    GLuint mLumaProgram = 0;
    GLint mLumaUniformTexture = -1;
    static const int LUMA_SIZE = 16; // 16x16 downscale for average

    int mWidth = 1920;
    int mHeight = 1080;
    int mPreviewWidth = 0;
    int mPreviewHeight = 0;
    int64_t mRecordingStartTimestampNs = -1;
    bool mHdrEnabled = false;
};

#endif // GL_RENDERER_H
