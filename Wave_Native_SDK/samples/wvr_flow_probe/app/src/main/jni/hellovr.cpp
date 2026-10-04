#include "hellovr.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <Context.h>
#include <log.h>
#include <wvr/wvr_events.h>
#include <wvr/wvr_projection.h>
#include <wvr/wvr_system.h>

#include <atomic>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <mutex>

#define FLOW_LOG_TAG "FLOW_MIN_PROBE"
#define FLOW_POSE_HOST "192.168.0.102" // until the stream socket tells us the PC address
#define FLOW_POSE_PORT 8002
#define FLOW_HAND_JOINTS 26u // Wave natural hand tracker joints (WVR_HandJoint)

// Virtual desktop screen placement, in meters.
#define FLOW_SCREEN_DISTANCE 2.0f
#define FLOW_SCREEN_WIDTH 4.8f
#define FLOW_NEAR_CLIP 0.1f
#define FLOW_FAR_CLIP 100.0f
// Per-eye render size: the Flow panel resolution (and the per-eye size of the SteamVR stream).
#define FLOW_EYE_BUFFER_SIZE 1600u // override: debug.flow.eyebuffer (read when the renderer starts)
// Unsharp-mask strength for the streamed picture (override: debug.flow.sharpen).
#define FLOW_DEFAULT_SHARPEN 0.0f // off: no visible gain on the Flow (tested 0..2)
#define FLOW_DEFAULT_HANDS 1 // hand tracking feeds the PC's Index controllers; debug.flow.hands=0 turns it off

static jobject gActivity = nullptr;
static jmethodID gStartDecoderMethod = nullptr;
// Guards gSurfaceTexture: the UI thread swaps it on pause/resume while the render
// thread calls updateTexImage on it.
static std::mutex gDecoderMutex;
static jobject gSurfaceTexture = nullptr;
static jmethodID gUpdateTexImageMethod = nullptr;
static jmethodID gGetTransformMatrixMethod = nullptr;
static jmethodID gGetTimestampMethod = nullptr;
static jfloatArray gTexMatrixArray = nullptr;
static std::atomic<int> gVideoWidth(1920);
static std::atomic<int> gVideoHeight(1080);

// Desktop layer stream (see MainApplication::initDesktopBridge).
static jmethodID gStartDesktopMethod = nullptr;
static std::mutex gDesktopMutex;
static jobject gDesktopSurfaceTexture = nullptr;
static jfloatArray gDesktopTexMatrixArray = nullptr;
static std::atomic<int> gDesktopVideoWidth(0);
static std::atomic<int> gDesktopVideoHeight(0);
// Where Desktop+ shows its panel (from the SteamVR stream); the layer is shown only there.
struct DesktopPanel {
    bool visible = false;
    int flags = 0; // bit 1 = keypad pointer active: draw its reticle
    float transform[12] = {}; // row-major 3x4, includes Desktop+'s scale
    float width = 0.0f;
};
static std::mutex gDesktopPanelMutex;
static DesktopPanel gDesktopPanel;

// Stream layouts from the FLOWH264 v4 header.
#define FLOW_LAYOUT_MONO 0       // desktop image, shown on a world-locked virtual screen
#define FLOW_LAYOUT_STEREO_SBS 1 // SteamVR left|right eye images, shown full-view per eye
static std::atomic<int> gStreamLayout(FLOW_LAYOUT_MONO);

// Pose UDP destination (IPv4, network byte order); 0 = use FLOW_POSE_HOST.
static std::atomic<uint32_t> gPoseTargetAddr(0);

// pts -> Flow pose sequence for recently received frames, filled by the Java receive thread
// (FLOWH264 v5) and read on the render thread for the frame SurfaceTexture latched.
struct FramePoseEntry {
    int64_t ptsUs;
    uint32_t poseSequence;
};
static std::mutex gFramePoseMutex;
static FramePoseEntry gFramePoses[64] = {};
static uint32_t gFramePoseNext = 0;

static uint32_t lookupFramePoseSequence(int64_t ptsUs) {
    std::lock_guard<std::mutex> lock(gFramePoseMutex);
    for (const FramePoseEntry &entry : gFramePoses) {
        if (entry.poseSequence != 0 && entry.ptsUs == ptsUs) {
            return entry.poseSequence;
        }
    }
    return 0;
}

static uint64_t elapsedUsec(const timeval &now, const timeval &last) {
    return static_cast<uint64_t>(now.tv_sec - last.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(now.tv_usec - last.tv_usec);
}

typedef MainApplication::Mat4 Mat4;

static Mat4 mat4Identity() {
    Mat4 r = {};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

static Mat4 mat4FromWvr(const WVR_Matrix4f_t &w) {
    Mat4 r;
    memcpy(r.m, w.m, sizeof(r.m));
    return r;
}

static Mat4 mat4Mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = sum;
        }
    }
    return r;
}

// Inverse of a rotation + translation matrix: [R t]^-1 = [R^T -R^T t].
static Mat4 mat4RigidInverse(const Mat4 &a) {
    Mat4 r = mat4Identity();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[j][i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    }
    return r;
}

static GLuint compileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("%s shader compile failed type=%u log=%s", FLOW_LOG_TAG, type, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(const char *vertexSource, const char *fragmentSource) {
    GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("%s program link failed log=%s", FLOW_LOG_TAG, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

MainApplication::MainApplication() {
    memset(mDevicePairs, 0, sizeof(mDevicePairs));
    gettimeofday(&mLastTime, nullptr);
    LOGI("%s create", FLOW_LOG_TAG);
}

MainApplication::~MainApplication() {
    LOGI("%s destroy", FLOW_LOG_TAG);
}

bool MainApplication::initVR() {
    WVR_InitError initError = WVR_Init(WVR_AppType_VRContent);
    if (initError != WVR_InitError_None) {
        LOGE("%s WVR_Init failed error=%d message=%s", FLOW_LOG_TAG, initError, WVR_GetInitErrorString(initError));
        return false;
    }

    // debug.flow.fse <0..1> (read once, here): Wave's frame sharpness enhancement for the
    // content layer. Unset = off, as the runtime then also skips its two-stage pipeline.
    char fseValue[PROP_VALUE_MAX] = {};
    uint64_t renderConfig = WVR_RenderConfig_Default;
    if (__system_property_get("debug.flow.fse", fseValue) > 0) {
        mFrameSharpness = strtof(fseValue, nullptr);
        mFrameSharpness = mFrameSharpness < 0.0f ? 0.0f : (mFrameSharpness > 1.0f ? 1.0f : mFrameSharpness);
        renderConfig |= WVR_RenderConfig_Initialize_FrameSharpnessEnhancement;
    }
    WVR_RenderInitParams_t params = {WVR_GraphicsApiType_OpenGL, renderConfig};
    WVR_RenderError renderError = WVR_RenderInit(&params);
    if (renderError != WVR_RenderError_None) {
        LOGE("%s WVR_RenderInit failed error=%d", FLOW_LOG_TAG, renderError);
        return false;
    }
    if (mFrameSharpness >= 0.0f) {
        const WVR_Result result = WVR_SetFrameSharpnessEnhancementLevel(mFrameSharpness);
        LOGI("%s frame sharpness enhancement level=%.2f result=%d", FLOW_LOG_TAG, mFrameSharpness, result);
    }
    mMaxFrameLayers = WVR_GetMaxFrameLayerCount();
    LOGI("%s max frame layers=%u (1 = no compositor layers)", FLOW_LOG_TAG, mMaxFrameLayers);

    LOGI("%s initVR ok interactionMode=%d gazeTrigger=%d",
         FLOW_LOG_TAG,
         WVR_GetInteractionMode(),
         WVR_GetGazeTriggerType());
    return true;
}

bool MainApplication::initGL() {
    LOGI("%s GL version=%s vendor=%s renderer=%s",
         FLOW_LOG_TAG,
         glGetString(GL_VERSION),
         glGetString(GL_VENDOR),
         glGetString(GL_RENDERER));

    WVR_GetRenderTargetSize(&mRenderWidth, &mRenderHeight);
    if (mRenderWidth == 0 || mRenderHeight == 0) {
        LOGE("%s invalid render target %ux%u", FLOW_LOG_TAG, mRenderWidth, mRenderHeight);
        return false;
    }
    // Wave recommends 1440x1440, but the panel is 1600x1600 per eye and the PC streams 1600 per
    // eye: render at panel size so the stream maps 1:1 instead of being upscaled again.
    const uint32_t recommendedWidth = mRenderWidth;
    const uint32_t recommendedHeight = mRenderHeight;
    uint32_t eyeBuffer = FLOW_EYE_BUFFER_SIZE;
    char eyeBufferValue[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.flow.eyebuffer", eyeBufferValue) > 0) {
        const int requested = atoi(eyeBufferValue);
        if (requested >= 1024 && requested <= 2560) {
            eyeBuffer = static_cast<uint32_t>(requested);
        }
    }
    mRenderWidth = mRenderWidth < eyeBuffer ? eyeBuffer : mRenderWidth;
    mRenderHeight = mRenderHeight < eyeBuffer ? eyeBuffer : mRenderHeight;
    LOGI("%s eye buffer %ux%u (Wave recommended %ux%u)", FLOW_LOG_TAG,
         mRenderWidth, mRenderHeight, recommendedWidth, recommendedHeight);

    mLeftQueue = WVR_ObtainTextureQueue(WVR_TextureTarget_2D,
                                        WVR_TextureFormat_RGBA,
                                        WVR_TextureType_UnsignedByte,
                                        mRenderWidth,
                                        mRenderHeight,
                                        0);
    mRightQueue = WVR_ObtainTextureQueue(WVR_TextureTarget_2D,
                                         WVR_TextureFormat_RGBA,
                                         WVR_TextureType_UnsignedByte,
                                         mRenderWidth,
                                         mRenderHeight,
                                         0);

    if (!mLeftQueue || !mRightQueue) {
        LOGE("%s texture queue allocation failed left=%p right=%p", FLOW_LOG_TAG, mLeftQueue, mRightQueue);
        return false;
    }

    if (!createEyeTargets(mLeftQueue, &mLeftTargets) ||
        !createEyeTargets(mRightQueue, &mRightTargets)) {
        return false;
    }

    initEyeMatrices();
    if (!initTestPattern()) {
        return false;
    }
    if (!initDesktopBridge()) {
        LOGW("%s desktop layer unavailable", FLOW_LOG_TAG);
    }
    if (!initDecoderBridge()) {
        return false;
    }
    initPoseSocket();

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    LOGI("%s initGL ok renderTarget=%ux%u leftQueue=%d rightQueue=%d",
         FLOW_LOG_TAG,
         mRenderWidth,
         mRenderHeight,
         WVR_GetTextureQueueLength(mLeftQueue),
         WVR_GetTextureQueueLength(mRightQueue));
    return true;
}

// Unit quad (-0.5..0.5) drawn as a 4-vertex triangle strip; uMvp places it in the world.
static const char *kScreenQuadVertexSource =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform mat4 uMvp;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 0.5 : -0.5,\n"
    "                  (gl_VertexID >= 2) ? 0.5 : -0.5);\n"
    "    vUv = p + 0.5;\n"
    "    gl_Position = uMvp * vec4(p, 0.0, 1.0);\n"
    "}\n";

bool MainApplication::initTestPattern() {
    static const char *fragmentSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vUv;\n"
        "uniform float uFrame;\n"
        "out vec4 oColor;\n"
        "float stripe(float x, float count) { return step(0.5, fract(x * count)); }\n"
        "void main() {\n"
        "    float t = uFrame * 0.0125;\n"
        "    float bars = floor(fract(vUv.x + t) * 6.0);\n"
        "    vec3 c0 = vec3(0.95, 0.10, 0.08);\n"
        "    vec3 c1 = vec3(0.10, 0.85, 0.18);\n"
        "    vec3 c2 = vec3(0.08, 0.28, 0.95);\n"
        "    vec3 c3 = vec3(0.95, 0.85, 0.10);\n"
        "    vec3 c4 = vec3(0.85, 0.12, 0.90);\n"
        "    vec3 c5 = vec3(0.10, 0.85, 0.90);\n"
        "    vec3 color = bars < 1.0 ? c0 : bars < 2.0 ? c1 : bars < 3.0 ? c2 : bars < 4.0 ? c3 : bars < 5.0 ? c4 : c5;\n"
        "    float grid = max(stripe(vUv.x + t * 0.33, 32.0), stripe(vUv.y - t * 0.25, 18.0));\n"
        "    float vignette = smoothstep(0.85, 0.25, distance(vUv, vec2(0.5)));\n"
        "    color = mix(color, vec3(1.0), grid * 0.12);\n"
        "    oColor = vec4(color * vignette, 1.0);\n"
        "}\n";

    mPatternProgram = linkProgram(kScreenQuadVertexSource, fragmentSource);
    if (!mPatternProgram) {
        return false;
    }
    mPatternFrameUniform = glGetUniformLocation(mPatternProgram, "uFrame");
    mPatternMvpUniform = glGetUniformLocation(mPatternProgram, "uMvp");
    LOGI("%s test pattern shader ready program=%u", FLOW_LOG_TAG, mPatternProgram);
    return true;
}

void MainApplication::shutdownTestPattern() {
    if (mPatternProgram != 0) {
        glDeleteProgram(mPatternProgram);
        mPatternProgram = 0;
    }
}

bool MainApplication::initDecoderBridge() {
    glGenTextures(1, &mDecoderTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mDecoderTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    // uTexMatrix is SurfaceTexture.getTransformMatrix(): maps quad UVs (origin bottom-left)
    // to the decoder buffer, including any vertical flip the codec output needs.
    static const char *fragmentSource =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision mediump float;\n"
        "in vec2 vUv;\n"
        "uniform samplerExternalOES uVideo;\n"
        "uniform mat4 uTexMatrix;\n"
        "uniform vec4 uUvRect;\n" // xy = offset, zw = scale: picks one eye of a side-by-side frame
        "uniform vec2 uTexel;\n"   // one video pixel in texture coordinates
        "uniform float uSharpen;\n" // unsharp-mask strength, 0 = off
        "out vec4 oColor;\n"
        "const vec3 kLuma = vec3(0.299, 0.587, 0.114);\n"
        "void main() {\n"
        "    vec2 uv = (uTexMatrix * vec4(uUvRect.xy + vUv * uUvRect.zw, 0.0, 1.0)).xy;\n"
        "    vec3 c = texture(uVideo, uv).rgb;\n"
        "    if (uSharpen > 0.0) {\n"
        // Sharpen luma only (4:2:0 chroma is too coarse and noisy to sharpen) and clamp to the
        // neighbourhood's range so edges get crisper without bright/dark halos.
        "        float l = dot(c, kLuma);\n"
        "        float n = dot(texture(uVideo, uv + vec2(0.0, uTexel.y)).rgb, kLuma);\n"
        "        float s = dot(texture(uVideo, uv - vec2(0.0, uTexel.y)).rgb, kLuma);\n"
        "        float e = dot(texture(uVideo, uv + vec2(uTexel.x, 0.0)).rgb, kLuma);\n"
        "        float w = dot(texture(uVideo, uv - vec2(uTexel.x, 0.0)).rgb, kLuma);\n"
        "        float lo = min(l, min(min(n, s), min(e, w)));\n"
        "        float hi = max(l, max(max(n, s), max(e, w)));\n"
        "        float sharp = clamp(l + uSharpen * (l - 0.25 * (n + s + e + w)), lo, hi);\n"
        "        c = clamp(c + (sharp - l), 0.0, 1.0);\n"
        "    }\n"
        "    oColor = vec4(c, 1.0);\n"
        "}\n";

    mDecoderProgram = linkProgram(kScreenQuadVertexSource, fragmentSource);
    if (!mDecoderProgram) {
        return false;
    }
    mDecoderMvpUniform = glGetUniformLocation(mDecoderProgram, "uMvp");
    mDecoderTexMatrixUniform = glGetUniformLocation(mDecoderProgram, "uTexMatrix");
    mDecoderUvRectUniform = glGetUniformLocation(mDecoderProgram, "uUvRect");
    mDecoderTexelUniform = glGetUniformLocation(mDecoderProgram, "uTexel");
    mDecoderSharpenUniform = glGetUniformLocation(mDecoderProgram, "uSharpen");
    GLint videoUniform = glGetUniformLocation(mDecoderProgram, "uVideo");
    glUseProgram(mDecoderProgram);
    glUniform1i(videoUniform, 0);
    glUseProgram(0);

    if (gActivity && gStartDecoderMethod) {
        EnvWrapper envWrapper = Context::getInstance()->getEnv();
        JNIEnv *env = envWrapper.get();
        env->CallVoidMethod(gActivity, gStartDecoderMethod, (jint)mDecoderTexture);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("%s Java decoder start failed", FLOW_LOG_TAG);
            return false;
        }
        LOGI("%s decoder bridge requested texture=%u", FLOW_LOG_TAG, mDecoderTexture);
    } else {
        LOGW("%s decoder bridge has no activity callback", FLOW_LOG_TAG);
    }
    return true;
}

void MainApplication::shutdownDecoderBridge() {
    if (mDecoderProgram != 0) {
        glDeleteProgram(mDecoderProgram);
        mDecoderProgram = 0;
    }
    if (mDecoderTexture != 0) {
        glDeleteTextures(1, &mDecoderTexture);
        mDecoderTexture = 0;
    }
}

void MainApplication::updateDecoderFrame() {
    std::lock_guard<std::mutex> lock(gDecoderMutex);
    if (!gSurfaceTexture || !gUpdateTexImageMethod) {
        mDecoderReady = false;
        return;
    }

    timeval before;
    gettimeofday(&before, nullptr);
    EnvWrapper envWrapper = Context::getInstance()->getEnv();
    JNIEnv *env = envWrapper.get();
    env->CallVoidMethod(gSurfaceTexture, gUpdateTexImageMethod);
    timeval after;
    gettimeofday(&after, nullptr);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mDecoderReady = false;
        return;
    }
    if (gGetTimestampMethod) {
        // MediaCodec renders to the SurfaceTexture with timestamp = presentationTimeUs * 1000.
        jlong timestampNs = env->CallLongMethod(gSurfaceTexture, gGetTimestampMethod);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else {
            mDisplayedPoseSequence = lookupFramePoseSequence(static_cast<int64_t>(timestampNs / 1000));
            // updateTexImage keeps the old frame when nothing new arrived; count only new ones.
            if (timestampNs != mLastFrameTimestampNs) {
                mLastFrameTimestampNs = timestampNs;
                ++mNewVideoFrames;
            }
        }
    }
    if (gGetTransformMatrixMethod && gTexMatrixArray) {
        env->CallVoidMethod(gSurfaceTexture, gGetTransformMatrixMethod, gTexMatrixArray);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else {
            env->GetFloatArrayRegion(gTexMatrixArray, 0, 16, mDecoderTexMatrix);
        }
    }
    uint64_t updateUsec = elapsedUsec(after, before);
    mDecoderUpdateTotalUsec += updateUsec;
    if (updateUsec > mDecoderUpdateMaxUsec) {
        mDecoderUpdateMaxUsec = updateUsec;
    }
    ++mDecoderUpdateCount;
    mDecoderReady = true;
    ++mDecoderFrames;
}

void MainApplication::drawDecoderFrame(const Mat4 &mvp, float uOffset, float uScale) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mDecoderTexture);
    glUseProgram(mDecoderProgram);
    // Mat4 is row-major, so let GL transpose it.
    glUniformMatrix4fv(mDecoderMvpUniform, 1, GL_TRUE, &mvp.m[0][0]);
    glUniformMatrix4fv(mDecoderTexMatrixUniform, 1, GL_FALSE, mDecoderTexMatrix);
    glUniform4f(mDecoderUvRectUniform, uOffset, 0.0f, uScale, 1.0f);
    glUniform2f(mDecoderTexelUniform, 1.0f / static_cast<float>(gVideoWidth.load()),
                1.0f / static_cast<float>(gVideoHeight.load()));
    glUniform1f(mDecoderSharpenUniform, mSharpenAmount);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
}

void MainApplication::drawTestPattern(const Mat4 &mvp) {
    glUseProgram(mPatternProgram);
    glUniform1f(mPatternFrameUniform, static_cast<float>(mTotalFrames));
    glUniformMatrix4fv(mPatternMvpUniform, 1, GL_TRUE, &mvp.m[0][0]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
}

// Sharpening strength for the streamed picture, from `adb shell setprop debug.flow.sharpen <0..2>`
// (unset = default). Read periodically so it can be tuned while wearing the headset.
void MainApplication::updateSharpenAmount() {
    char value[PROP_VALUE_MAX] = {};
    float amount = FLOW_DEFAULT_SHARPEN;
    if (__system_property_get("debug.flow.sharpen", value) > 0) {
        amount = strtof(value, nullptr);
    }
    amount = amount < 0.0f ? 0.0f : (amount > 2.0f ? 2.0f : amount);
    if (amount != mSharpenAmount) {
        mSharpenAmount = amount;
        LOGI("%s sharpen=%.2f", FLOW_LOG_TAG, amount);
    }
}

static int propertyInt(const char *name, int fallback) {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(name, value) > 0 ? atoi(value) : fallback;
}

void MainApplication::updateLayerTestSettings() {
    const bool enabled = propertyInt("debug.flow.layertest", 0) != 0;
    const int eye = propertyInt("debug.flow.layertest.eye", 1) != 0 ? 1 : 0;
    const int shape = propertyInt("debug.flow.layertest.shape", 0) != 0 ? 1 : 0;
    const int image = propertyInt("debug.flow.layertest.image", 0) != 0 ? 1 : 0;
    char widthValue[PROP_VALUE_MAX] = {};
    float width = __system_property_get("debug.flow.layertest.width", widthValue) > 0 ? strtof(widthValue, nullptr) : 3.2f;
    width = width < 0.5f ? 0.5f : (width > 8.0f ? 8.0f : width);
    const int source = propertyInt("debug.flow.layertest.src", 0) != 0 ? 1 : 0;
    const bool headLocked = propertyInt("debug.flow.layertest.headlocked", 0) != 0;
    if (enabled == mLayerTest && eye == mLayerTestEye && shape == mLayerTestShape &&
        image == mLayerTestImage && width == mLayerTestWidth && source == mLayerTestSource &&
        headLocked == mLayerTestHeadLocked) {
        return;
    }
    mLayerTestSource = source;
    mLayerTestHeadLocked = headLocked;
    mLayerTest = enabled;
    mLayerTestEye = eye;
    mLayerTestShape = shape;
    mLayerTestImage = image;
    mLayerTestWidth = width;
    if (mLayerTest && mLayerTestTextures[image] == 0 && !mLayerTestLoadFailed[image]) {
        mLayerTestLoadFailed[image] = !loadLayerTestImage(image);
    }
    if (mLayerTest && mLayerTestTextures[image] == 0) {
        mLayerTest = false; // nothing to show
    }
    if (mLayerTest && !mScreenPlaced) {
        placeScreenIfNeeded();
    }
    LOGI("%s layer test %s: layer eye=%s shape=%s image=%s width=%.2fm source=%s headLocked=%d maxLayers=%u",
         FLOW_LOG_TAG, mLayerTest ? "on" : "off", eye ? "right" : "left", shape ? "cylinder" : "quad",
         image ? "test_4k" : "test_1080", width, source ? "gl" : "queue", headLocked ? 1 : 0, mMaxFrameLayers);
}

bool MainApplication::loadLayerTestImage(int index) {
    const char *path = index ? "/data/data/com.htc.vr.samples.wvr_flow_probe/files/test_4k.png"
                             : "/data/data/com.htc.vr.samples.wvr_flow_probe/files/test_1080.png";
    FILE *file = fopen(path, "rb");
    if (!file) {
        LOGE("%s layer test: cannot open %s", FLOW_LOG_TAG, path);
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[64 * 1024];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    fclose(file);

    EnvWrapper envWrapper = Context::getInstance()->getEnv();
    AndroidBitmapInfo info = {};
    uint8_t *pixels = Context::getInstance()->getBitmapFactory()->decodeByteArray(
        envWrapper.get(), bytes.data(), bytes.size(), info);
    if (!pixels || info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        LOGE("%s layer test: cannot decode %s (format %d)", FLOW_LOG_TAG, path, pixels ? info.format : -1);
        delete[] pixels;
        return false;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(info.stride / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, info.width, info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenerateMipmap(GL_TEXTURE_2D);
    // Trilinear for the eye-buffer path: its best case when the image is minified.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    delete[] pixels;

    if (mImageProgram == 0) {
        static const char *fragmentSource =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 vUv;\n"
            "uniform sampler2D uImage;\n"
            "out vec4 oColor;\n"
            "void main() { oColor = vec4(texture(uImage, vec2(vUv.x, 1.0 - vUv.y)).rgb, 1.0); }\n";
        mImageProgram = linkProgram(kScreenQuadVertexSource, fragmentSource);
        mImageMvpUniform = glGetUniformLocation(mImageProgram, "uMvp");
    }
    mLayerTestTextures[index] = texture;
    mLayerTestSize[index][0] = static_cast<int>(info.width);
    mLayerTestSize[index][1] = static_cast<int>(info.height);

    void *queue = WVR_ObtainTextureQueue(WVR_TextureTarget_2D, WVR_TextureFormat_RGBA, WVR_TextureType_UnsignedByte,
                                         info.width, info.height, 0);
    const int queueLength = queue ? WVR_GetTextureQueueLength(queue) : 0;
    GLuint framebuffers[2] = {0, 0};
    glGenFramebuffers(2, framebuffers);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    for (int i = 0; i < queueLength; ++i) {
        const GLuint target = static_cast<GLuint>(reinterpret_cast<uintptr_t>(WVR_GetTexture(queue, i).id));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        LOGI("%s layer test: queue[%d] texture=%u read fbo=0x%x draw fbo=0x%x", FLOW_LOG_TAG, i, target,
             glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
        // Same size, nearest: an exact copy, flipped so the image's top row ends up on top.
        glBlitFramebuffer(0, 0, info.width, info.height, 0, info.height, info.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, framebuffers);
    glFinish();
    mLayerTestQueues[index] = queue;
    LOGI("%s layer test: texture queue %p length=%d", FLOW_LOG_TAG, queue, queueLength);
    LOGI("%s layer test: loaded %s %ux%u texture=%u", FLOW_LOG_TAG, path, info.width, info.height, texture);
    return true;
}

bool MainApplication::layerTestOnEye(WVR_Eye eye) const {
    return mMaxFrameLayers > 1 && !mLayerSubmitFailed && (eye == WVR_Eye_Right) == (mLayerTestEye == 1);
}

// Same placement as the stream's mono screen (placeScreenIfNeeded), with the test image's size.
MainApplication::Mat4 MainApplication::layerTestMvp(WVR_Eye eye) const {
    const int *size = mLayerTestSize[mLayerTestImage];
    const float width = mLayerTestWidth;
    const float height = width * static_cast<float>(size[1]) / static_cast<float>(size[0]);
    Mat4 model = mat4Identity();
    model.m[0][0] = mScreenCosYaw * width;
    model.m[0][2] = mScreenSinYaw;
    model.m[1][1] = height;
    model.m[2][0] = -mScreenSinYaw * width;
    model.m[2][2] = mScreenCosYaw;
    model.m[0][3] = mScreenCenter[0];
    model.m[1][3] = mScreenCenter[1];
    model.m[2][3] = mScreenCenter[2];
    const int index = eye == WVR_Eye_Left ? 0 : 1;
    const Mat4 headFromWorld = mHmdPose.isValidPose ? mat4RigidInverse(mat4FromWvr(mHmdPose.poseMatrix)) : mat4Identity();
    return mat4Mul(mProjection[index], mat4Mul(mat4Mul(mEyeFromHead[index], headFromWorld), model));
}

void MainApplication::drawLayerTestImage(WVR_Eye eye) {
    const Mat4 mvp = layerTestMvp(eye);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mLayerTestTextures[mLayerTestImage]);
    glUseProgram(mImageProgram);
    glUniformMatrix4fv(mImageMvpUniform, 1, GL_TRUE, &mvp.m[0][0]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Submits both eyes as content layers plus the test image as an overlay layer on the layer eye,
// in one call as Wave expects. Returns false (the caller then uses WVR_SubmitFrame per eye) if
// the runtime rejects layers.
bool MainApplication::submitLayerTestFrame() {
    if (mMaxFrameLayers <= 1 || mLayerSubmitFailed) {
        return false;
    }
    WVR_LayerParams_t layers[2 + 2 * 8] = {};
    for (int i = 0; i < 2; ++i) {
        WVR_LayerParams_t &layer = layers[i];
        layer.eye = i == 0 ? WVR_Eye_Left : WVR_Eye_Right;
        layer.id = mLayerTestEyeTexture[i].id;
        layer.target = mLayerTestEyeTexture[i].target;
        layer.layout.rightUpUVs.v[0] = 1.0f;
        layer.layout.rightUpUVs.v[1] = 1.0f;
        layer.opts = WVR_TextureOption_Opaque;
        layer.shape = WVR_TextureShape_Quad;
        layer.type = WVR_TextureLayerType_Content;
        layer.compositionDepth = 0;
        layer.pose = &mLayerTestPose;
        layer.width = mRenderWidth;
        layer.height = mRenderHeight;
    }

    const int *size = mLayerTestSize[mLayerTestImage];
    const float width = mLayerTestWidth;
    const float height = width * static_cast<float>(size[1]) / static_cast<float>(size[0]);
    const float halfYaw = 0.5f * atan2f(mScreenSinYaw, mScreenCosYaw);
    WVR_Pose_t layerPose = {};
    layerPose.position.v[0] = mScreenCenter[0];
    layerPose.position.v[1] = mScreenCenter[1];
    layerPose.position.v[2] = mScreenCenter[2];
    if (mLayerTestShape == 1) {
        // Cylinder pose = the cylinder's axis (the viewer), like OpenXR; the surface is
        // cylinderRadius in front of it.
        layerPose.position.v[0] -= -mScreenSinYaw * FLOW_SCREEN_DISTANCE;
        layerPose.position.v[2] -= -mScreenCosYaw * FLOW_SCREEN_DISTANCE;
    }
    layerPose.rotation.w = cosf(halfYaw);
    layerPose.rotation.y = sinf(halfYaw);
    WVR_Vector3f_t layerSize = {};
    layerSize.v[0] = width;
    layerSize.v[1] = height;
    layerSize.v[2] = 1.0f;
    if (mLayerTestHeadLocked) {
        layerPose = {};
        layerPose.position.v[2] = -FLOW_SCREEN_DISTANCE;
        layerPose.rotation.w = 1.0f;
    }
    void *queue = mLayerTestQueues[mLayerTestImage];
    uint32_t count = 2;
    if (queue || mLayerTestSource == 1) {
        WVR_LayerParams_t &layer = layers[2];
        layer.eye = mLayerTestEye == 1 ? WVR_Eye_Right : WVR_Eye_Left;
        if (mLayerTestSource == 1) {
            // Uploaded top row first: flip with the UVs (the queue copies are flipped already).
            layer.id = reinterpret_cast<WVR_Texture_t>(static_cast<uintptr_t>(mLayerTestTextures[mLayerTestImage]));
        } else {
            layer.id = WVR_GetTexture(queue, WVR_GetAvailableTextureIndex(queue)).id;
        }
        layer.target = WVR_TextureTarget_2D;
        layer.layout.leftLowUVs.v[1] = mLayerTestSource == 1 ? 1.0f : 0.0f;
        layer.layout.rightUpUVs.v[0] = 1.0f;
        layer.layout.rightUpUVs.v[1] = mLayerTestSource == 1 ? 0.0f : 1.0f;
        layer.opts = mLayerTestHeadLocked ? WVR_TextureOption_HeadLocked : WVR_TextureOption_None;
        layer.shape = mLayerTestShape == 1 ? WVR_TextureShape_Cylinder : WVR_TextureShape_Quad;
        layer.type = WVR_TextureLayerType_Overlay;
        layer.compositionDepth = 7;
        layer.pose = &mLayerTestPose;
        layer.poseTransform = &layerPose;
        layer.size = &layerSize;
        layer.width = static_cast<uint32_t>(size[0]);
        layer.height = static_cast<uint32_t>(size[1]);
        layer.cylinderRadius = FLOW_SCREEN_DISTANCE;
        // Wave only shows overlays submitted for both eyes, so the other eye gets the same layer
        // shrunk to nothing; debug.flow.layertest.both=1 shows it on both eyes instead.
        static WVR_Vector3f_t hiddenSize = {{0.001f, 0.001f, 1.0f}};
        layers[3] = layer;
        layers[3].eye = layer.eye == WVR_Eye_Left ? WVR_Eye_Right : WVR_Eye_Left;
        // debug.flow.layertest.count=N: N overlay pairs stacked vertically, on both eyes, to find
        // how many layers the Flow composes.
        const int pairs = propertyInt("debug.flow.layertest.count", 1);
        if (propertyInt("debug.flow.layertest.both", 0) == 0 && pairs <= 1) {
            layers[3].size = &hiddenSize;
        }
        count = 4;
        static WVR_Pose_t stacked[8];
        for (int n = 1; n < pairs && n < 8; ++n) {
            stacked[n] = layerPose;
            stacked[n].position.v[1] += (n % 2 ? 1.0f : -1.0f) * ((n + 1) / 2) * height * 1.05f;
            layers[count] = layers[2];
            layers[count].poseTransform = &stacked[n];
            layers[count + 1] = layers[count];
            layers[count + 1].eye = layers[count].eye == WVR_Eye_Left ? WVR_Eye_Right : WVR_Eye_Left;
            layers[count].compositionDepth = layers[count + 1].compositionDepth = propertyInt("debug.flow.layertest.samedepth", 0) ? 7 : 7 + n;
            count += 2;
        }
    }
    const WVR_SubmitError error = WVR_SubmitFrameLayers(layers, count, WVR_SubmitExtend_Default);
    static uint32_t loggedCount = 0;
    static WVR_SubmitError loggedError = WVR_SubmitError_None;
    if (count != loggedCount || error != loggedError) {
        loggedCount = count;
        loggedError = error;
        LOGI("%s layer test: submitted %u layers -> error=%d", FLOW_LOG_TAG, count, error);
    }
    if (error != WVR_SubmitError_None) {
        if (count <= 4) {
            mLayerSubmitFailed = true; // the basic pair failed: layers unusable
        }
        return false;
    }
    return true;
}

bool MainApplication::initDesktopBridge() {
    glGenTextures(1, &mDesktopOesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mDesktopOesTexture);
    // Nearest: the copy below is pixel-aligned, so it moves decoded pixels unchanged.
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    static const char *fragmentSource =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision mediump float;\n"
        "in vec2 vUv;\n"
        "uniform samplerExternalOES uVideo;\n"
        "uniform mat4 uTexMatrix;\n"
        "out vec4 oColor;\n"
        "void main() { oColor = vec4(texture(uVideo, (uTexMatrix * vec4(vUv, 0.0, 1.0)).xy).rgb, 1.0); }\n";
    mDesktopCopyProgram = linkProgram(kScreenQuadVertexSource, fragmentSource);
    if (!mDesktopCopyProgram) {
        return false;
    }
    mDesktopCopyMvpUniform = glGetUniformLocation(mDesktopCopyProgram, "uMvp");
    // White ring and centre dot with a dark outline (like the helper's SteamVR reticle).
    static const char *reticleSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vUv;\n"
        "out vec4 oColor;\n"
        "void main() {\n"
        "    float r = length(vUv - 0.5) * 64.0;\n"
        "    float white = (r >= 20.0 && r <= 25.0) || r <= 3.0 ? 1.0 : 0.0;\n"
        "    float dark = (r >= 17.0 && r <= 28.0) || r <= 5.5 ? 1.0 : 0.0;\n"
        "    if (dark == 0.0) discard;\n"
        "    oColor = vec4(vec3(white), white > 0.0 ? 1.0 : 0.8);\n"
        "}\n";
    mReticleProgram = linkProgram(kScreenQuadVertexSource, reticleSource);
    mReticleMvpUniform = glGetUniformLocation(mReticleProgram, "uMvp");
    mDesktopCopyTexMatrixUniform = glGetUniformLocation(mDesktopCopyProgram, "uTexMatrix");
    glGenFramebuffers(1, &mDesktopFramebuffer);
    if (!gActivity || !gStartDesktopMethod) {
        return false;
    }
    EnvWrapper envWrapper = Context::getInstance()->getEnv();
    JNIEnv *env = envWrapper.get();
    env->CallVoidMethod(gActivity, gStartDesktopMethod, (jint)mDesktopOesTexture);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    LOGI("%s desktop layer bridge requested texture=%u", FLOW_LOG_TAG, mDesktopOesTexture);
    return true;
}

void MainApplication::shutdownDesktopBridge() {
    if (mDesktopQueue) {
        WVR_ReleaseTextureQueue(mDesktopQueue);
        mDesktopQueue = nullptr;
    }
    if (mDesktopFramebuffer) {
        glDeleteFramebuffers(1, &mDesktopFramebuffer);
        mDesktopFramebuffer = 0;
    }
    if (mDesktopCopyProgram) {
        glDeleteProgram(mDesktopCopyProgram);
        mDesktopCopyProgram = 0;
    }
    if (mReticleProgram) {
        glDeleteProgram(mReticleProgram);
        mReticleProgram = 0;
    }
    if (mDesktopOesTexture) {
        glDeleteTextures(1, &mDesktopOesTexture);
        mDesktopOesTexture = 0;
    }
}

void MainApplication::updateDesktopSettings() {
    const bool enabled = propertyInt("debug.flow.desktop", 1) != 0;
    char value[PROP_VALUE_MAX] = {};
    float width = __system_property_get("debug.flow.desktop.width", value) > 0 ? strtof(value, nullptr) : 3.2f;
    width = width < 0.5f ? 0.5f : (width > 8.0f ? 8.0f : width);
    if (enabled != mDesktopEnabled || width != mDesktopWidth) {
        mDesktopEnabled = enabled;
        mDesktopWidth = width;
        LOGI("%s desktop layer %s width=%.2fm", FLOW_LOG_TAG, enabled ? "enabled" : "disabled", width);
    }
}

// Latches the newest decoded desktop frame and copies it into the next free queue texture.
void MainApplication::updateDesktopFrame() {
    std::lock_guard<std::mutex> lock(gDesktopMutex);
    const int width = gDesktopVideoWidth.load();
    const int height = gDesktopVideoHeight.load();
    if (!gDesktopSurfaceTexture || !gUpdateTexImageMethod || width <= 0 || height <= 0) {
        mDesktopIndex = -1;
        return;
    }
    EnvWrapper envWrapper = Context::getInstance()->getEnv();
    JNIEnv *env = envWrapper.get();
    env->CallVoidMethod(gDesktopSurfaceTexture, gUpdateTexImageMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    const jlong timestampNs = env->CallLongMethod(gDesktopSurfaceTexture, gGetTimestampMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    float reticleU = 0.0f, reticleV = 0.0f;
    const bool reticle = desktopReticleUv(&reticleU, &reticleV);
    if (timestampNs == mDesktopLastTimestampNs && mDesktopIndex >= 0 && !reticle && !mDesktopReticleDrawn) {
        return; // nothing new: keep showing the last copy
    }
    mDesktopLastTimestampNs = timestampNs;
    env->CallVoidMethod(gDesktopSurfaceTexture, gGetTransformMatrixMethod, gDesktopTexMatrixArray);
    if (!env->ExceptionCheck()) {
        env->GetFloatArrayRegion(gDesktopTexMatrixArray, 0, 16, mDesktopTexMatrix);
    } else {
        env->ExceptionClear();
    }

    if (!mDesktopQueue || mDesktopQueueSize[0] != width || mDesktopQueueSize[1] != height) {
        if (mDesktopQueue) {
            WVR_ReleaseTextureQueue(mDesktopQueue);
        }
        mDesktopQueue = WVR_ObtainTextureQueue(WVR_TextureTarget_2D, WVR_TextureFormat_RGBA,
                                               WVR_TextureType_UnsignedByte, width, height, 0);
        mDesktopQueueSize[0] = width;
        mDesktopQueueSize[1] = height;
        mDesktopIndex = -1;
        LOGI("%s desktop layer queue %dx%d length=%d", FLOW_LOG_TAG, width, height,
             mDesktopQueue ? WVR_GetTextureQueueLength(mDesktopQueue) : 0);
        if (!mDesktopQueue) {
            return;
        }
    }
    const int32_t index = WVR_GetAvailableTextureIndex(mDesktopQueue);
    if (index < 0) {
        return;
    }
    const GLuint target = static_cast<GLuint>(reinterpret_cast<uintptr_t>(WVR_GetTexture(mDesktopQueue, index).id));
    glBindFramebuffer(GL_FRAMEBUFFER, mDesktopFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glViewport(0, 0, width, height);
    Mat4 fullView = mat4Identity();
    fullView.m[0][0] = 2.0f; // unit quad -0.5..0.5 -> clip space -1..1
    fullView.m[1][1] = 2.0f;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, mDesktopOesTexture);
    glUseProgram(mDesktopCopyProgram);
    glUniformMatrix4fv(mDesktopCopyMvpUniform, 1, GL_TRUE, &fullView.m[0][0]);
    glUniformMatrix4fv(mDesktopCopyTexMatrixUniform, 1, GL_FALSE, mDesktopTexMatrix);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (reticle && mReticleProgram) {
        const float sizePx = 40.0f; // the ring is 25/32 of this across
        Mat4 quad = mat4Identity();
        quad.m[0][0] = 2.0f * sizePx / static_cast<float>(width);
        quad.m[1][1] = 2.0f * sizePx / static_cast<float>(height);
        quad.m[0][3] = 2.0f * reticleU - 1.0f;
        quad.m[1][3] = 2.0f * reticleV - 1.0f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(mReticleProgram);
        glUniformMatrix4fv(mReticleMvpUniform, 1, GL_TRUE, &quad.m[0][0]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glUseProgram(0);
        glDisable(GL_BLEND);
    }
    mDesktopReticleDrawn = reticle;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mDesktopIndex = index;
    ++mDesktopNewFrames;
}

// Where the keypad controller's laser meets the Desktop+ panel, as panel UVs (0..1, v up).
// Mirrors driver_flowvr's keypad controller: it sits 10 cm below and 15 cm in front of the
// eyes, pitched up so its laser crosses the gaze line 2 m ahead.
bool MainApplication::desktopReticleUv(float *u, float *v) const {
    DesktopPanel panel;
    {
        std::lock_guard<std::mutex> lock(gDesktopPanelMutex);
        panel = gDesktopPanel;
    }
    if (!panel.visible || (panel.flags & 2) == 0 || !mHmdPose.isValidPose || mDesktopQueueSize[0] <= 0) {
        return false;
    }
    const WVR_Matrix4f_t &head = mHmdPose.poseMatrix;
    const float down = 0.10f, forward = 0.15f, converge = 2.0f;
    const float pitch = atan2f(down, converge - forward);
    // Laser in head space: origin (0, -down, -forward), direction (0, sin(pitch), -cos(pitch)).
    const float lo[3] = {0.0f, -down, -forward};
    const float ld[3] = {0.0f, sinf(pitch), -cosf(pitch)};
    float o[3], d[3];
    for (int r = 0; r < 3; ++r) {
        o[r] = head.m[r][0] * lo[0] + head.m[r][1] * lo[1] + head.m[r][2] * lo[2] + head.m[r][3];
        d[r] = head.m[r][0] * ld[0] + head.m[r][1] * ld[1] + head.m[r][2] * ld[2];
    }
    const float *m = panel.transform;
    const float scale = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    if (scale < 1e-6f) {
        return false;
    }
    // Panel axes (unit) and centre.
    const float ax[3] = {m[0] / scale, m[4] / scale, m[8] / scale};
    const float ay[3] = {m[1] / scale, m[5] / scale, m[9] / scale};
    const float az[3] = {m[2] / scale, m[6] / scale, m[10] / scale};
    const float c[3] = {m[3], m[7], m[11]};
    const float denom = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
    if (fabsf(denom) < 1e-4f) {
        return false;
    }
    const float t = ((c[0] - o[0]) * az[0] + (c[1] - o[1]) * az[1] + (c[2] - o[2]) * az[2]) / denom;
    if (t <= 0.0f) {
        return false;
    }
    const float p[3] = {o[0] + t * d[0] - c[0], o[1] + t * d[1] - c[1], o[2] + t * d[2] - c[2]};
    const float width = panel.width * scale;
    const float height = width * static_cast<float>(mDesktopQueueSize[1]) / static_cast<float>(mDesktopQueueSize[0]);
    *u = (p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2]) / width + 0.5f;
    *v = (p[0] * ay[0] + p[1] * ay[1] + p[2] * ay[2]) / height + 0.5f;
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

bool MainApplication::layersActive() const {
    if (mMaxFrameLayers <= 1 || mLayerSubmitFailed) {
        return false;
    }
    if (mLayerTest) {
        return true;
    }
    std::lock_guard<std::mutex> lock(gDesktopPanelMutex);
    return mDesktopEnabled && mDesktopIndex >= 0 && gDesktopPanel.visible && gDesktopPanel.width > 0.0f;
}

// Both eyes' content layers (the SteamVR picture) plus the desktop as an overlay layer pair.
bool MainApplication::submitDesktopFrame() {
    WVR_LayerParams_t layers[4] = {};
    for (int i = 0; i < 2; ++i) {
        WVR_LayerParams_t &layer = layers[i];
        layer.eye = i == 0 ? WVR_Eye_Left : WVR_Eye_Right;
        layer.id = mLayerTestEyeTexture[i].id;
        layer.target = mLayerTestEyeTexture[i].target;
        layer.layout.rightUpUVs.v[0] = 1.0f;
        layer.layout.rightUpUVs.v[1] = 1.0f;
        layer.opts = WVR_TextureOption_Opaque;
        layer.shape = WVR_TextureShape_Quad;
        layer.type = WVR_TextureLayerType_Content;
        layer.pose = &mLayerTestPose;
        layer.width = mRenderWidth;
        layer.height = mRenderHeight;
    }
    DesktopPanel panel;
    {
        std::lock_guard<std::mutex> lock(gDesktopPanelMutex);
        panel = gDesktopPanel;
    }
    const float *m = panel.transform;
    // Split Desktop+'s uniform scale off the rotation; it scales the panel size instead.
    const float scale = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    const float inv = scale > 1e-6f ? 1.0f / scale : 1.0f;
    const float r00 = m[0] * inv, r01 = m[1] * inv, r02 = m[2] * inv;
    const float r10 = m[4] * inv, r11 = m[5] * inv, r12 = m[6] * inv;
    const float r20 = m[8] * inv, r21 = m[9] * inv, r22 = m[10] * inv;
    WVR_Pose_t pose = {};
    pose.position.v[0] = m[3];
    pose.position.v[1] = m[7];
    pose.position.v[2] = m[11];
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const float t = sqrtf(trace + 1.0f) * 2.0f;
        pose.rotation.w = 0.25f * t;
        pose.rotation.x = (r21 - r12) / t;
        pose.rotation.y = (r02 - r20) / t;
        pose.rotation.z = (r10 - r01) / t;
    } else if (r00 > r11 && r00 > r22) {
        const float t = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        pose.rotation.w = (r21 - r12) / t;
        pose.rotation.x = 0.25f * t;
        pose.rotation.y = (r01 + r10) / t;
        pose.rotation.z = (r02 + r20) / t;
    } else if (r11 > r22) {
        const float t = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        pose.rotation.w = (r02 - r20) / t;
        pose.rotation.x = (r01 + r10) / t;
        pose.rotation.y = 0.25f * t;
        pose.rotation.z = (r12 + r21) / t;
    } else {
        const float t = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        pose.rotation.w = (r10 - r01) / t;
        pose.rotation.x = (r02 + r20) / t;
        pose.rotation.y = (r12 + r21) / t;
        pose.rotation.z = 0.25f * t;
    }
    const float width = panel.width * scale;
    const float height = width * static_cast<float>(mDesktopQueueSize[1]) / static_cast<float>(mDesktopQueueSize[0]);
    WVR_Vector3f_t size = {};
    size.v[0] = width;
    size.v[1] = height;
    size.v[2] = 1.0f;
    const WVR_TextureParams_t desktop = WVR_GetTexture(mDesktopQueue, mDesktopIndex);
    for (int i = 2; i < 4; ++i) {
        WVR_LayerParams_t &layer = layers[i];
        layer.eye = i == 2 ? WVR_Eye_Left : WVR_Eye_Right;
        layer.id = desktop.id;
        layer.target = WVR_TextureTarget_2D;
        layer.layout.rightUpUVs.v[0] = 1.0f;
        layer.layout.rightUpUVs.v[1] = 1.0f;
        layer.opts = WVR_TextureOption_None;
        layer.shape = WVR_TextureShape_Quad;
        layer.type = WVR_TextureLayerType_Overlay;
        layer.compositionDepth = 7;
        layer.pose = &mLayerTestPose;
        layer.poseTransform = &pose;
        layer.size = &size;
        layer.width = static_cast<uint32_t>(mDesktopQueueSize[0]);
        layer.height = static_cast<uint32_t>(mDesktopQueueSize[1]);
    }
    const WVR_SubmitError error = WVR_SubmitFrameLayers(layers, 4, WVR_SubmitExtend_Default);
    if (error != WVR_SubmitError_None) {
        LOGE("%s desktop layer: WVR_SubmitFrameLayers failed error=%d; back to eye-buffer only", FLOW_LOG_TAG, error);
        mLayerSubmitFailed = true;
        return false;
    }
    return true;
}

void MainApplication::checkEyeDumpRequest() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.flow.dumpeye", value);
    if (mEyeDumpChecked && mEyeDumpSeen != value) {
        mEyeDumpPending = true; // value changed since the last check: dump the next frame
    }
    mEyeDumpSeen = value;
    mEyeDumpChecked = true;
}

// Reads back the bound eye framebuffer and writes it as a binary PPM (top row first).
void MainApplication::dumpEyeBuffer() {
    const char *dir = "/data/data/com.htc.vr.samples.wvr_flow_probe/files";
    const std::string path = std::string(dir) + "/flow_eye_left.ppm";
    std::vector<uint8_t> rgba(static_cast<size_t>(mRenderWidth) * mRenderHeight * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, mRenderWidth, mRenderHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    mkdir(dir, 0700);
    FILE *file = fopen(path.c_str(), "wb");
    if (!file) {
        LOGE("%s eye dump: cannot write %s", FLOW_LOG_TAG, path.c_str());
        return;
    }
    fprintf(file, "P6\n%u %u\n255\n", mRenderWidth, mRenderHeight);
    std::vector<uint8_t> row(static_cast<size_t>(mRenderWidth) * 3);
    for (uint32_t y = 0; y < mRenderHeight; ++y) {
        const uint8_t *src = &rgba[static_cast<size_t>(mRenderHeight - 1 - y) * mRenderWidth * 4];
        for (uint32_t x = 0; x < mRenderWidth; ++x) {
            row[x * 3] = src[x * 4];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), file);
    }
    fclose(file);
    LOGI("%s eye dump: wrote %ux%u to %s", FLOW_LOG_TAG, mRenderWidth, mRenderHeight, path.c_str());
}

// The natural (camera) hand tracker runs by default; `adb shell setprop debug.flow.hands 0`
// stops it (and 1 restarts it) on the live stream.
void MainApplication::updateHandTrackingEnabled() {
    char value[PROP_VALUE_MAX] = {};
    const bool wanted = __system_property_get("debug.flow.hands", value) > 0 ? atoi(value) != 0 : FLOW_DEFAULT_HANDS != 0;
    if (wanted == mHandsActive) {
        return;
    }
    if (!wanted) {
        WVR_StopHandTracking(WVR_HandTrackerType_Natural);
        mHandsActive = false;
        LOGI("%s hands stopped", FLOW_LOG_TAG);
        return;
    }
    const WVR_Result started = WVR_StartHandTracking(WVR_HandTrackerType_Natural);
    uint32_t jointCount = 0;
    const WVR_Result counted = WVR_GetHandJointCount(WVR_HandTrackerType_Natural, &jointCount);
    LOGI("%s hands start result=%d jointCountResult=%d joints=%u", FLOW_LOG_TAG, started, counted, jointCount);
    if (started != WVR_Success || counted != WVR_Success || jointCount == 0) {
        if (started == WVR_Success) {
            WVR_StopHandTracking(WVR_HandTrackerType_Natural);
        }
        return; // retried on the next property poll
    }
    mHandJointCount = jointCount;
    for (int hand = 0; hand < 2; ++hand) {
        mHandJoints[hand].assign(jointCount, WVR_Pose_t{});
    }
    mHandsActive = true;
}

void MainApplication::updateHands() {
    if (!mHandsActive) {
        return;
    }
    mHandData.left.jointCount = mHandJointCount;
    mHandData.left.joints = mHandJoints[0].data();
    mHandData.right.jointCount = mHandJointCount;
    mHandData.right.joints = mHandJoints[1].data();

    timeval before;
    gettimeofday(&before, nullptr);
    const WVR_Result result = WVR_GetHandTrackingData(WVR_HandTrackerType_Natural,
                                                      WVR_HandModelType_WithoutController,
                                                      WVR_PoseOriginModel_OriginOnHead,
                                                      &mHandData, &mHandPose);
    timeval after;
    gettimeofday(&after, nullptr);
    const uint64_t usec = elapsedUsec(after, before);
    ++mHandQueries;
    mHandQueryTotalUsec += usec;
    if (usec > mHandQueryMaxUsec) {
        mHandQueryMaxUsec = usec;
    }
    if (result != WVR_Success) {
        ++mHandQueryFailures;
        return;
    }
    if (mHandData.timestamp != mHandLastTimestamp) {
        mHandLastTimestamp = mHandData.timestamp;
        ++mHandNewSamples;
    }
    sendHandPacket();
    const WVR_HandJointData_t *hands[2] = {&mHandData.left, &mHandData.right};
    const WVR_HandPoseState_t *poses[2] = {&mHandPose.left, &mHandPose.right};
    for (int hand = 0; hand < 2; ++hand) {
        if (!hands[hand]->isValidPose) {
            continue;
        }
        ++mHandValid[hand];
        if (poses[hand]->base.type == WVR_HandPoseType_Pinch && poses[hand]->pinch.strength >= 0.8f) {
            ++mHandPinching[hand];
        }
    }
}

void MainApplication::logHandsIfNeeded(float seconds) {
    if (!mHandsActive && mHandQueries == 0) {
        return;
    }
    const float queries = mHandQueries > 0 ? static_cast<float>(mHandQueries) : 1.0f;
    const WVR_Vector3f_t &wristL = mHandJoints[0].empty() ? WVR_Vector3f_t{} : mHandJoints[0][WVR_HandJoint_Wrist].position;
    const WVR_Vector3f_t &wristR = mHandJoints[1].empty() ? WVR_Vector3f_t{} : mHandJoints[1][WVR_HandJoint_Wrist].position;
    LOGI("%s hands queries=%u failures=%u trackerHz=%.1f validL=%.0f%% validR=%.0f%% pinchL=%.0f%% pinchR=%.0f%% "
         "queryAvgUs=%llu queryMaxUs=%llu confL=%.2f confR=%.2f wristL=%.2f,%.2f,%.2f wristR=%.2f,%.2f,%.2f",
         FLOW_LOG_TAG, mHandQueries, mHandQueryFailures, mHandNewSamples / seconds,
         100.0f * mHandValid[0] / queries, 100.0f * mHandValid[1] / queries,
         100.0f * mHandPinching[0] / queries, 100.0f * mHandPinching[1] / queries,
         static_cast<unsigned long long>(mHandQueries > 0 ? mHandQueryTotalUsec / mHandQueries : 0),
         static_cast<unsigned long long>(mHandQueryMaxUsec),
         mHandData.left.confidence, mHandData.right.confidence,
         wristL.v[0], wristL.v[1], wristL.v[2], wristR.v[0], wristR.v[1], wristR.v[2]);
    mHandQueries = 0;
    mHandQueryFailures = 0;
    mHandValid[0] = mHandValid[1] = 0;
    mHandPinching[0] = mHandPinching[1] = 0;
    mHandQueryTotalUsec = 0;
    mHandQueryMaxUsec = 0;
    mHandNewSamples = 0;
}

void MainApplication::initEyeMatrices() {
    const WVR_Eye eyes[2] = {WVR_Eye_Left, WVR_Eye_Right};
    for (int i = 0; i < 2; ++i) {
        mProjection[i] = mat4FromWvr(WVR_GetProjection(eyes[i], FLOW_NEAR_CLIP, FLOW_FAR_CLIP));
        mEyeFromHead[i] = mat4RigidInverse(mat4FromWvr(WVR_GetTransformFromEyeToHead(eyes[i])));
    }
    LOGI("%s eye offsets left=%.4f right=%.4f",
         FLOW_LOG_TAG, -mEyeFromHead[0].m[0][3], -mEyeFromHead[1].m[0][3]);
    // Lens frustum as tangents; the SteamVR driver's GetProjectionRaw must match these.
    for (int i = 0; i < 2; ++i) {
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
        WVR_GetClippingPlaneBoundary(eyes[i], &left, &right, &top, &bottom);
        const WVR_Matrix4f_t eyeToHead = WVR_GetTransformFromEyeToHead(eyes[i]);
        LOGI("%s lens eye=%s clip l=%.5f r=%.5f t=%.5f b=%.5f eyeToHead=%.5f,%.5f,%.5f renderTarget=%ux%u",
             FLOW_LOG_TAG, i == 0 ? "left" : "right", left, right, top, bottom,
             eyeToHead.m[0][3], eyeToHead.m[1][3], eyeToHead.m[2][3], mRenderWidth, mRenderHeight);
    }
}

// Places the screen FLOW_SCREEN_DISTANCE in front of the current gaze, using yaw only,
// so it stays upright at head height. Runs on the first valid pose and after a recenter.
void MainApplication::placeScreenIfNeeded() {
    if (mScreenPlaced || !mHmdPose.isValidPose) {
        return;
    }

    const WVR_Matrix4f_t &head = mHmdPose.poseMatrix;
    // Head forward is -Z of the head rotation.
    float forwardX = -head.m[0][2];
    float forwardZ = -head.m[2][2];
    float length = sqrtf(forwardX * forwardX + forwardZ * forwardZ);
    if (length < 0.001f) {
        forwardX = 0.0f;
        forwardZ = -1.0f;
    } else {
        forwardX /= length;
        forwardZ /= length;
    }
    // RotY(yaw) maps local -Z onto (forwardX, 0, forwardZ).
    mScreenSinYaw = -forwardX;
    mScreenCosYaw = -forwardZ;
    mScreenCenter[0] = head.m[0][3] + forwardX * FLOW_SCREEN_DISTANCE;
    mScreenCenter[1] = head.m[1][3];
    mScreenCenter[2] = head.m[2][3] + forwardZ * FLOW_SCREEN_DISTANCE;
    mScreenPlaced = true;
    LOGI("%s screen placed center=%.3f,%.3f,%.3f yawDeg=%.1f",
         FLOW_LOG_TAG, mScreenCenter[0], mScreenCenter[1], mScreenCenter[2],
         atan2f(mScreenSinYaw, mScreenCosYaw) * 57.29578f);
}

MainApplication::Mat4 MainApplication::screenMvp(WVR_Eye eye) const {
    // Height follows the live stream aspect, which may arrive after the screen is placed.
    int videoWidth = gVideoWidth.load();
    int videoHeight = gVideoHeight.load();
    float aspect = (videoWidth > 0 && videoHeight > 0)
                       ? static_cast<float>(videoHeight) / static_cast<float>(videoWidth)
                       : 9.0f / 16.0f;
    float width = FLOW_SCREEN_WIDTH;
    float height = FLOW_SCREEN_WIDTH * aspect;

    // model = Translate(center) * RotY(yaw) * Scale(width, height, 1)
    Mat4 model = mat4Identity();
    model.m[0][0] = mScreenCosYaw * width;
    model.m[0][2] = mScreenSinYaw;
    model.m[1][1] = height;
    model.m[2][0] = -mScreenSinYaw * width;
    model.m[2][2] = mScreenCosYaw;
    model.m[0][3] = mScreenCenter[0];
    model.m[1][3] = mScreenCenter[1];
    model.m[2][3] = mScreenCenter[2];

    int index = eye == WVR_Eye_Left ? 0 : 1;
    Mat4 headFromWorld = mHmdPose.isValidPose
                             ? mat4RigidInverse(mat4FromWvr(mHmdPose.poseMatrix))
                             : mat4Identity();
    Mat4 view = mat4Mul(mEyeFromHead[index], headFromWorld);
    return mat4Mul(mProjection[index], mat4Mul(view, model));
}

bool MainApplication::createEyeTargets(void *queue, std::vector<EyeTarget> *targets) {
    int length = WVR_GetTextureQueueLength(queue);
    targets->resize(length);

    for (int i = 0; i < length; ++i) {
        WVR_TextureParams_t texture = WVR_GetTexture(queue, i);
        EyeTarget &target = targets->at(i);

        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);

        glGenRenderbuffers(1, &target.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mRenderWidth, mRenderHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)(uintptr_t)texture.id, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("%s framebuffer incomplete index=%d status=0x%x", FLOW_LOG_TAG, i, status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    return true;
}

void MainApplication::destroyEyeTargets(std::vector<EyeTarget> *targets) {
    for (EyeTarget &target : *targets) {
        if (target.depth != 0) {
            glDeleteRenderbuffers(1, &target.depth);
            target.depth = 0;
        }
        if (target.framebuffer != 0) {
            glDeleteFramebuffers(1, &target.framebuffer);
            target.framebuffer = 0;
        }
    }
    targets->clear();
}

bool MainApplication::handleInput() {
    WVR_Event_t event;
    while (WVR_PollEventQueue(&event)) {
        if (event.common.type == WVR_EventType_Quit) {
            LOGI("%s received quit event", FLOW_LOG_TAG);
            return true;
        }
        if (event.common.type == WVR_EventType_RecenterSuccess ||
            event.common.type == WVR_EventType_RecenterSuccess3DoF) {
            LOGI("%s recenter event type=%d, re-placing screen", FLOW_LOG_TAG, event.common.type);
            mScreenPlaced = false;
        }
    }
    return false;
}

bool MainApplication::renderEye(WVR_Eye eye, void *queue, std::vector<EyeTarget> *targets, uint32_t index) {
    EyeTarget &target = targets->at(index);
    WVR_TextureParams_t texture = WVR_GetTexture(queue, index);

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, mRenderWidth, mRenderHeight);
    WVR_PreRenderEye(eye, &texture);

    glClearColor(0.03f, 0.03f, 0.04f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (mLayerTest) {
        if (!layerTestOnEye(eye)) {
            drawLayerTestImage(eye);
        }
    } else if (mDecoderReady && gStreamLayout.load() == FLOW_LAYOUT_STEREO_SBS) {
        // SteamVR already rendered this eye with the Flow's frustum (driver GetProjectionRaw),
        // so its half of the frame maps 1:1 onto the whole eye buffer.
        Mat4 fullView = mat4Identity();
        fullView.m[0][0] = 2.0f; // unit quad -0.5..0.5 -> clip space -1..1
        fullView.m[1][1] = 2.0f;
        drawDecoderFrame(fullView, eye == WVR_Eye_Left ? 0.0f : 0.5f, 0.5f);
    } else if (mDecoderReady) {
        drawDecoderFrame(screenMvp(eye), 0.0f, 1.0f);
    } else {
        drawTestPattern(screenMvp(eye));
    }
    if (eye == WVR_Eye_Left && mEyeDumpPending) {
        mEyeDumpPending = false;
        dumpEyeBuffer();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    texture.layout.leftLowUVs.v[0] = 0.0f;
    texture.layout.leftLowUVs.v[1] = 0.0f;
    texture.layout.rightUpUVs.v[0] = 1.0f;
    texture.layout.rightUpUVs.v[1] = 1.0f;

    // Tell the runtime which pose this frame was rendered with so timewarp corrects against it.
    // For SteamVR frames that is the older pose the PC rendered with, not the current one:
    // timewarp then rotates the image by however far the head moved during the round trip.
    const WVR_PoseState_t *renderPose = mHmdPose.isValidPose ? &mHmdPose : nullptr;
    if (mDecoderReady && gStreamLayout.load() == FLOW_LAYOUT_STEREO_SBS) {
        const WVR_PoseState_t *streamPose = findSentPose(mDisplayedPoseSequence);
        if (streamPose) {
            renderPose = streamPose;
        }
    }
    if (layersActive()) {
        mLayerTestEyeTexture[eye == WVR_Eye_Left ? 0 : 1] = texture;
        mLayerTestPose = renderPose ? *renderPose : mHmdPose;
        return true; // submitted for both eyes at once in renderFrame
    }
    WVR_SubmitError submitError = WVR_SubmitFrame(eye, &texture, renderPose, WVR_SubmitExtend_Default);
    if (submitError != WVR_SubmitError_None) {
        LOGE("%s submit failed eye=%d error=%d frame=%u index=%u", FLOW_LOG_TAG, eye, submitError, mTotalFrames, index);
        return false;
    }
    return true;
}

bool MainApplication::renderFrame() {
    mLeftIndex = WVR_GetAvailableTextureIndex(mLeftQueue);
    mRightIndex = WVR_GetAvailableTextureIndex(mRightQueue);
    updateDecoderFrame();
    updateDesktopFrame();

    if (!renderEye(WVR_Eye_Left, mLeftQueue, &mLeftTargets, mLeftIndex) ||
        !renderEye(WVR_Eye_Right, mRightQueue, &mRightTargets, mRightIndex)) {
        return true;
    }
    if (layersActive() && !(mLayerTest ? submitLayerTestFrame() : submitDesktopFrame())) {
        // Layers rejected: show this frame the normal way (later frames skip layers).
        WVR_SubmitFrame(WVR_Eye_Left, &mLayerTestEyeTexture[0], &mLayerTestPose, WVR_SubmitExtend_Default);
        WVR_SubmitFrame(WVR_Eye_Right, &mLayerTestEyeTexture[1], &mLayerTestPose, WVR_SubmitExtend_Default);
    }

    ++mFrameCount;
    ++mTotalFrames;
    if (mTotalFrames % 75 == 1) {
        updateSharpenAmount();
        updateHandTrackingEnabled();
        checkEyeDumpRequest();
        updateLayerTestSettings();
        updateDesktopSettings();
    }
    updateHands();
    recordPoseAge();
    logProbeIfNeeded();
    usleep(1);
    return false;
}

void MainApplication::updateHMDPose() {
    WVR_GetSyncPose(WVR_PoseOriginModel_OriginOnHead, mDevicePairs, WVR_DEVICE_COUNT_LEVEL_1);
    const WVR_PoseState_t &pose = mDevicePairs[WVR_DEVICE_HMD].pose;
    mHmdValid = pose.isValidPose;
    mHmd6Dof = pose.is6DoFPose;
    if (mHmdValid) {
        mHmdPose = pose;
        mHmdX = pose.poseMatrix.m[0][3];
        mHmdY = pose.poseMatrix.m[1][3];
        mHmdZ = pose.poseMatrix.m[2][3];
        placeScreenIfNeeded();
        sendPosePacket(pose);
        PoseHistoryEntry &entry = mPoseHistory[mPoseSequence % kPoseHistorySize];
        entry.sequence = mPoseSequence;
        entry.pose = pose;
    }
}

const WVR_PoseState_t *MainApplication::findSentPose(uint32_t sequence) const {
    if (sequence == 0) {
        return nullptr;
    }
    const PoseHistoryEntry &entry = mPoseHistory[sequence % kPoseHistorySize];
    return entry.sequence == sequence ? &entry.pose : nullptr;
}

void MainApplication::initPoseSocket() {
    if (mPoseSocket >= 0) {
        return;
    }
    mPoseSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (mPoseSocket < 0) {
        LOGE("%s pose UDP socket create failed", FLOW_LOG_TAG);
        return;
    }
    LOGI("%s pose UDP target=%s:%d", FLOW_LOG_TAG, FLOW_POSE_HOST, FLOW_POSE_PORT);
}

void MainApplication::shutdownPoseSocket() {
    if (mPoseSocket >= 0) {
        close(mPoseSocket);
        mPoseSocket = -1;
    }
}

// Sends a datagram to the PC's pose port (address learned from the stream socket).
static void sendToPc(int socket, const void *data, size_t size) {
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(FLOW_POSE_PORT);
    uint32_t targetAddr = gPoseTargetAddr.load();
    if (targetAddr != 0) {
        target.sin_addr.s_addr = targetAddr;
    } else {
        inet_pton(AF_INET, FLOW_POSE_HOST, &target.sin_addr);
    }
    sendto(socket, data, size, 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
}

static void matrixToQuat(const WVR_Matrix4f_t &matrix, float *x, float *y, float *z, float *w) {
    float trace = matrix.m[0][0] + matrix.m[1][1] + matrix.m[2][2];
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        *w = 0.25f * s;
        *x = (matrix.m[2][1] - matrix.m[1][2]) / s;
        *y = (matrix.m[0][2] - matrix.m[2][0]) / s;
        *z = (matrix.m[1][0] - matrix.m[0][1]) / s;
    } else if (matrix.m[0][0] > matrix.m[1][1] && matrix.m[0][0] > matrix.m[2][2]) {
        float s = sqrtf(1.0f + matrix.m[0][0] - matrix.m[1][1] - matrix.m[2][2]) * 2.0f;
        *w = (matrix.m[2][1] - matrix.m[1][2]) / s;
        *x = 0.25f * s;
        *y = (matrix.m[0][1] + matrix.m[1][0]) / s;
        *z = (matrix.m[0][2] + matrix.m[2][0]) / s;
    } else if (matrix.m[1][1] > matrix.m[2][2]) {
        float s = sqrtf(1.0f + matrix.m[1][1] - matrix.m[0][0] - matrix.m[2][2]) * 2.0f;
        *w = (matrix.m[0][2] - matrix.m[2][0]) / s;
        *x = (matrix.m[0][1] + matrix.m[1][0]) / s;
        *y = 0.25f * s;
        *z = (matrix.m[1][2] + matrix.m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + matrix.m[2][2] - matrix.m[0][0] - matrix.m[1][1]) * 2.0f;
        *w = (matrix.m[1][0] - matrix.m[0][1]) / s;
        *x = (matrix.m[0][2] + matrix.m[2][0]) / s;
        *y = (matrix.m[1][2] + matrix.m[2][1]) / s;
        *z = 0.25f * s;
    }
    float norm = sqrtf((*x * *x) + (*y * *y) + (*z * *z) + (*w * *w));
    if (norm > 0.001f) {
        *x /= norm;
        *y /= norm;
        *z /= norm;
        *w /= norm;
    } else {
        *x = 0.0f;
        *y = 0.0f;
        *z = 0.0f;
        *w = 1.0f;
    }
}

void MainApplication::sendPosePacket(const WVR_PoseState_t &pose) {
    if (mPoseSocket < 0) {
        return;
    }

#pragma pack(push, 1)
    struct PosePacket {
        uint32_t magic;
        uint32_t sequence;
        float x;
        float y;
        float z;
        float qx;
        float qy;
        float qz;
        float qw;
    };
#pragma pack(pop)

    PosePacket packet = {};
    packet.magic = 0x31504C46;
    packet.sequence = ++mPoseSequence;
    packet.x = pose.poseMatrix.m[0][3];
    packet.y = pose.poseMatrix.m[1][3];
    packet.z = pose.poseMatrix.m[2][3];
    matrixToQuat(pose.poseMatrix, &packet.qx, &packet.qy, &packet.qz, &packet.qw);

    sendToPc(mPoseSocket, &packet, sizeof(packet));
}

// Both hands' joints (head-origin space, like the head pose) and index pinch strength, every
// frame while hand tracking runs. driver_flowvr turns them into Index controllers.
void MainApplication::sendHandPacket() {
    if (mPoseSocket < 0 || mHandJointCount != FLOW_HAND_JOINTS) {
        return;
    }
#pragma pack(push, 1)
    struct HandPacket {
        uint32_t magic; // "FLH1"
        uint32_t sequence;
        uint8_t valid[2]; // [0] left, [1] right
        uint8_t reserved[2];
        float pinch[2];
        float joints[2][FLOW_HAND_JOINTS][3];
    };
#pragma pack(pop)
    HandPacket packet = {};
    packet.magic = 0x31484C46;
    packet.sequence = mPoseSequence;
    const WVR_HandJointData_t *hands[2] = {&mHandData.left, &mHandData.right};
    const WVR_HandPoseState_t *poses[2] = {&mHandPose.left, &mHandPose.right};
    for (int hand = 0; hand < 2; ++hand) {
        packet.valid[hand] = hands[hand]->isValidPose ? 1 : 0;
        const WVR_HandPoseState_t &pose = *poses[hand];
        packet.pinch[hand] = pose.base.type == WVR_HandPoseType_Pinch && pose.pinch.finger == WVR_FingerType_Index
                                 ? pose.pinch.strength : 0.0f;
        for (uint32_t joint = 0; joint < FLOW_HAND_JOINTS; ++joint) {
            memcpy(packet.joints[hand][joint], mHandJoints[hand][joint].position.v, sizeof(packet.joints[hand][joint]));
        }
    }
    sendToPc(mPoseSocket, &packet, sizeof(packet));
}

// Called once per rendered frame in stereo mode: how many pose updates (~13 ms each at 75 Hz)
// separate the pose the shown frame was rendered with from the newest one.
void MainApplication::recordPoseAge() {
    if (!mDecoderReady || gStreamLayout.load() != FLOW_LAYOUT_STEREO_SBS) {
        return;
    }
    if (findSentPose(mDisplayedPoseSequence) == nullptr) {
        ++mPoseAgeMisses;
        return;
    }
    uint32_t age = mPoseSequence - mDisplayedPoseSequence;
    mPoseAgeTotal += age;
    ++mPoseAgeCount;
    if (age > mPoseAgeMax) {
        mPoseAgeMax = age;
    }
}

void MainApplication::logProbeIfNeeded() {
    timeval now;
    gettimeofday(&now, nullptr);
    uint64_t usec = elapsedUsec(now, mLastTime);
    if (usec < 2000000ULL) {
        return;
    }

    mLastFps = static_cast<float>(mFrameCount) / (static_cast<float>(usec) / 1000000.0f);
    uint64_t avgDecoderUpdateUsec = mDecoderUpdateCount > 0
                                        ? mDecoderUpdateTotalUsec / mDecoderUpdateCount
                                        : 0;
    LOGI("%s frame=%u fps=%.1f hmdValid=%d hmdDoF=%s hmdXYZ=%.3f,%.3f,%.3f target=%ux%u leftIndex=%u rightIndex=%u decoderReady=%d decoderFrames=%u updateTexImageAvgUs=%llu updateTexImageMaxUs=%llu",
         FLOW_LOG_TAG,
         mTotalFrames,
         mLastFps,
         mHmdValid ? 1 : 0,
         mHmd6Dof ? "6" : "3",
         mHmdX,
         mHmdY,
         mHmdZ,
         mRenderWidth,
         mRenderHeight,
         mLeftIndex,
         mRightIndex,
         mDecoderReady ? 1 : 0,
         mDecoderFrames,
         static_cast<unsigned long long>(avgDecoderUpdateUsec),
         static_cast<unsigned long long>(mDecoderUpdateMaxUsec));
    if (mDecoderReady) {
        LOGI("%s timewarp poseAgeAvg=%.1f poseAgeMax=%u (pose steps) poseMisses=%u displayedSeq=%u newestSeq=%u videoFps=%.1f video=%dx%d",
             FLOW_LOG_TAG,
             mPoseAgeCount > 0 ? static_cast<float>(mPoseAgeTotal) / mPoseAgeCount : 0.0f,
             mPoseAgeMax, mPoseAgeMisses, mDisplayedPoseSequence, mPoseSequence,
             static_cast<float>(mNewVideoFrames) / (static_cast<float>(usec) / 1000000.0f),
             gVideoWidth.load(), gVideoHeight.load());
    }
    logHandsIfNeeded(static_cast<float>(usec) / 1000000.0f);
    mPoseAgeTotal = 0;
    mPoseAgeCount = 0;
    mPoseAgeMax = 0;
    mPoseAgeMisses = 0;
    mNewVideoFrames = 0;

    mFrameCount = 0;
    mDecoderUpdateCount = 0;
    mDecoderUpdateTotalUsec = 0;
    mDecoderUpdateMaxUsec = 0;
    mLastTime = now;
}

void MainApplication::shutdownGL() {
    shutdownPoseSocket();
    shutdownDecoderBridge();
    shutdownDesktopBridge();
    shutdownTestPattern();
    destroyEyeTargets(&mLeftTargets);
    destroyEyeTargets(&mRightTargets);
    if (mLeftQueue) {
        WVR_ReleaseTextureQueue(mLeftQueue);
        mLeftQueue = nullptr;
    }
    if (mRightQueue) {
        WVR_ReleaseTextureQueue(mRightQueue);
        mRightQueue = nullptr;
    }
}

void MainApplication::shutdownVR() {
    WVR_Quit();
}

void FlowProbe_SetActivity(JNIEnv *env, jobject activity) {
    if (gActivity) {
        env->DeleteGlobalRef(gActivity);
        gActivity = nullptr;
    }
    gActivity = env->NewGlobalRef(activity);
    jclass activityClass = env->GetObjectClass(activity);
    gStartDecoderMethod = env->GetMethodID(activityClass, "startDecoderSurface", "(I)V");
    gStartDesktopMethod = env->GetMethodID(activityClass, "startDesktopSurface", "(I)V");
    env->DeleteLocalRef(activityClass);
}

void FlowProbe_SetDecoderSurfaceTexture(JNIEnv *env, jobject surfaceTexture) {
    std::lock_guard<std::mutex> lock(gDecoderMutex);
    if (gSurfaceTexture) {
        env->DeleteGlobalRef(gSurfaceTexture);
        gSurfaceTexture = nullptr;
    }
    gSurfaceTexture = env->NewGlobalRef(surfaceTexture);
    jclass surfaceTextureClass = env->GetObjectClass(surfaceTexture);
    gUpdateTexImageMethod = env->GetMethodID(surfaceTextureClass, "updateTexImage", "()V");
    gGetTransformMatrixMethod = env->GetMethodID(surfaceTextureClass, "getTransformMatrix", "([F)V");
    gGetTimestampMethod = env->GetMethodID(surfaceTextureClass, "getTimestamp", "()J");
    env->DeleteLocalRef(surfaceTextureClass);
    if (!gTexMatrixArray) {
        jfloatArray localArray = env->NewFloatArray(16);
        gTexMatrixArray = static_cast<jfloatArray>(env->NewGlobalRef(localArray));
        env->DeleteLocalRef(localArray);
    }
    LOGI("%s decoder SurfaceTexture attached", FLOW_LOG_TAG);
}

void FlowProbe_ClearDecoderSurfaceTexture(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(gDecoderMutex);
    if (gSurfaceTexture) {
        env->DeleteGlobalRef(gSurfaceTexture);
        gSurfaceTexture = nullptr;
    }
    gUpdateTexImageMethod = nullptr;
    gGetTransformMatrixMethod = nullptr;
    gGetTimestampMethod = nullptr;
    LOGI("%s decoder SurfaceTexture cleared", FLOW_LOG_TAG);
}

void FlowProbe_SetFramePoseSequence(int64_t ptsUs, uint32_t poseSequence) {
    std::lock_guard<std::mutex> lock(gFramePoseMutex);
    gFramePoses[gFramePoseNext % 64] = {ptsUs, poseSequence};
    ++gFramePoseNext;
}

void FlowProbe_SetPoseTargetHost(const char *host) {
    in_addr addr = {};
    if (host && inet_pton(AF_INET, host, &addr) == 1) {
        gPoseTargetAddr.store(addr.s_addr);
        LOGI("%s pose UDP target=%s:%d", FLOW_LOG_TAG, host, FLOW_POSE_PORT);
    }
}

void FlowProbe_SetDecoderStreamInfo(int width, int height, int layout) {
    if (width <= 0 || height <= 0) {
        return;
    }
    gVideoWidth.store(width);
    gVideoHeight.store(height);
    gStreamLayout.store(layout == FLOW_LAYOUT_STEREO_SBS ? FLOW_LAYOUT_STEREO_SBS : FLOW_LAYOUT_MONO);
    LOGI("%s decoder video size=%dx%d layout=%s", FLOW_LOG_TAG, width, height,
         layout == FLOW_LAYOUT_STEREO_SBS ? "stereo-sbs" : "mono");
}

void FlowProbe_SetDesktopSurfaceTexture(JNIEnv *env, jobject surfaceTexture) {
    std::lock_guard<std::mutex> lock(gDesktopMutex);
    if (gDesktopSurfaceTexture) {
        env->DeleteGlobalRef(gDesktopSurfaceTexture);
    }
    gDesktopSurfaceTexture = env->NewGlobalRef(surfaceTexture);
    if (!gDesktopTexMatrixArray) {
        jfloatArray localArray = env->NewFloatArray(16);
        gDesktopTexMatrixArray = static_cast<jfloatArray>(env->NewGlobalRef(localArray));
        env->DeleteLocalRef(localArray);
    }
    // updateTexImage etc. are SurfaceTexture methods, shared with the main decoder.
    if (!gUpdateTexImageMethod) {
        jclass surfaceTextureClass = env->GetObjectClass(surfaceTexture);
        gUpdateTexImageMethod = env->GetMethodID(surfaceTextureClass, "updateTexImage", "()V");
        gGetTransformMatrixMethod = env->GetMethodID(surfaceTextureClass, "getTransformMatrix", "([F)V");
        gGetTimestampMethod = env->GetMethodID(surfaceTextureClass, "getTimestamp", "()J");
        env->DeleteLocalRef(surfaceTextureClass);
    }
    LOGI("%s desktop SurfaceTexture attached", FLOW_LOG_TAG);
}

void FlowProbe_ClearDesktopSurfaceTexture(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(gDesktopMutex);
    if (gDesktopSurfaceTexture) {
        env->DeleteGlobalRef(gDesktopSurfaceTexture);
        gDesktopSurfaceTexture = nullptr;
    }
    gDesktopVideoWidth.store(0);
    gDesktopVideoHeight.store(0);
    LOGI("%s desktop SurfaceTexture cleared", FLOW_LOG_TAG);
}

void FlowProbe_SetDesktopStreamInfo(int width, int height) {
    gDesktopVideoWidth.store(width > 0 ? width : 0);
    gDesktopVideoHeight.store(height > 0 ? height : 0);
    LOGI("%s desktop stream size=%dx%d", FLOW_LOG_TAG, width, height);
}

void FlowProbe_SetDesktopPanel(int flags, const float transform[12], float width) {
    std::lock_guard<std::mutex> lock(gDesktopPanelMutex);
    const bool visible = (flags & 1) != 0;
    if (visible != gDesktopPanel.visible) {
        LOGI("%s Desktop+ panel %s at %.2f,%.2f,%.2f width=%.2fm", FLOW_LOG_TAG, visible ? "shown" : "hidden",
             transform[3], transform[7], transform[11], width);
    }
    gDesktopPanel.visible = visible;
    gDesktopPanel.flags = flags;
    memcpy(gDesktopPanel.transform, transform, sizeof(gDesktopPanel.transform));
    gDesktopPanel.width = width;
}
