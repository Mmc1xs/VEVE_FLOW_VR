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
#include <sys/system_properties.h>
#include <mutex>

#define FLOW_LOG_TAG "FLOW_MIN_PROBE"
#define FLOW_POSE_HOST "192.168.0.102" // until the stream socket tells us the PC address
#define FLOW_POSE_PORT 8002

// Virtual desktop screen placement, in meters.
#define FLOW_SCREEN_DISTANCE 2.0f
#define FLOW_SCREEN_WIDTH 4.8f
#define FLOW_NEAR_CLIP 0.1f
#define FLOW_FAR_CLIP 100.0f
// Per-eye render size: the Flow panel resolution (and the per-eye size of the SteamVR stream).
#define FLOW_EYE_BUFFER_SIZE 1600u
// Unsharp-mask strength for the streamed picture (override: debug.flow.sharpen).
#define FLOW_DEFAULT_SHARPEN 0.0f // off: no visible gain on the Flow (tested 0..2)

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

    WVR_RenderInitParams_t params = {WVR_GraphicsApiType_OpenGL, WVR_RenderConfig_Default};
    WVR_RenderError renderError = WVR_RenderInit(&params);
    if (renderError != WVR_RenderError_None) {
        LOGE("%s WVR_RenderInit failed error=%d", FLOW_LOG_TAG, renderError);
        return false;
    }

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
    mRenderWidth = mRenderWidth < FLOW_EYE_BUFFER_SIZE ? FLOW_EYE_BUFFER_SIZE : mRenderWidth;
    mRenderHeight = mRenderHeight < FLOW_EYE_BUFFER_SIZE ? FLOW_EYE_BUFFER_SIZE : mRenderHeight;
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
    if (mDecoderReady && gStreamLayout.load() == FLOW_LAYOUT_STEREO_SBS) {
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

    if (!renderEye(WVR_Eye_Left, mLeftQueue, &mLeftTargets, mLeftIndex) ||
        !renderEye(WVR_Eye_Right, mRightQueue, &mRightTargets, mRightIndex)) {
        return true;
    }

    ++mFrameCount;
    ++mTotalFrames;
    if (mTotalFrames % 75 == 1) {
        updateSharpenAmount();
    }
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

    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(FLOW_POSE_PORT);
    uint32_t targetAddr = gPoseTargetAddr.load();
    if (targetAddr != 0) {
        target.sin_addr.s_addr = targetAddr;
    } else {
        inet_pton(AF_INET, FLOW_POSE_HOST, &target.sin_addr);
    }
    sendto(mPoseSocket, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
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
