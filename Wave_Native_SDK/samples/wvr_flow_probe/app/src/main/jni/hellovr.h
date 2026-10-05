#pragma once

#include <stdint.h>
#include <sys/time.h>
#include <string>
#include <vector>

#include <jni.h>
#include <GLES3/gl3.h>
#include <wvr/wvr.h>
#include <wvr/wvr_device.h>
#include <wvr/wvr_hand.h>
#include <wvr/wvr_render.h>

class MainApplication {
public:
    // Row-major 4x4, same layout as WVR_Matrix4f_t (m[row][col]).
    struct Mat4 {
        float m[4][4];
    };

    MainApplication();
    ~MainApplication();

    bool initVR();
    bool initGL();
    bool handleInput();
    bool renderFrame();
    void updateHMDPose();
    void shutdownGL();
    void shutdownVR();

private:
    struct EyeTarget {
        GLuint framebuffer = 0;
        GLuint depth = 0;
    };

    bool createEyeTargets(void *queue, std::vector<EyeTarget> *targets);
    void destroyEyeTargets(std::vector<EyeTarget> *targets);
    bool initTestPattern();
    void shutdownTestPattern();
    void drawTestPattern(const Mat4 &mvp);
    bool initDecoderBridge();
    void shutdownDecoderBridge();
    void updateDecoderFrame();
    void drawDecoderFrame(const Mat4 &mvp, float uOffset, float uScale);
    void initEyeMatrices();
    void placeScreenIfNeeded();
    Mat4 screenMvp(WVR_Eye eye) const;
    void initPoseSocket();
    void shutdownPoseSocket();
    void sendPosePacket(const WVR_PoseState_t &pose);
    void sendHandPacket();
    bool renderEye(WVR_Eye eye, void *queue, std::vector<EyeTarget> *targets, uint32_t index);
    void logProbeIfNeeded();
    void recordPoseAge();
    void updateSharpenAmount();
    // Diagnostics: `adb shell setprop debug.flow.dumpeye <new value>` saves the next left eye
    // buffer (what goes to timewarp) to files/flow_eye_left.ppm in the app's data directory.
    void checkEyeDumpRequest();
    // A/B sharpness test (`adb shell setprop debug.flow.layertest 1`): the same still image in
    // both eyes at the same place, one eye drawn into the eye buffer (the stream's path, sampled
    // again by timewarp), the other submitted as a Wave compositor layer (sampled once, like the
    // Flow's own UI). Image files: files/test_1080.png and files/test_4k.png in the app data dir.
    void updateLayerTestSettings();
    bool loadLayerTestImage(int index);
    Mat4 layerTestMvp(WVR_Eye eye) const;
    void drawLayerTestImage(WVR_Eye eye);
    bool layerTestOnEye(WVR_Eye eye) const;
    // Wave takes all layers of a frame (both eyes) in one WVR_SubmitFrameLayers call.
    bool submitLayerTestFrame();
    WVR_TextureParams_t mLayerTestEyeTexture[2] = {};
    WVR_PoseState_t mLayerTestPose = {};

    // Desktop layer: a second H.264 stream of the PC desktop (TCP 8005), decoded into its own
    // SurfaceTexture, copied 1:1 into a Wave texture queue and shown as a compositor layer, so
    // the compositor samples the desktop only once (like the Flow's own UI) instead of going
    // through the SteamVR picture, the eye buffer and timewarp.
    bool initDesktopBridge();
    void shutdownDesktopBridge();
    void updateDesktopFrame();
    void updateDesktopSettings();
    bool layersActive() const;
    bool submitDesktopFrame();
    GLuint mDesktopOesTexture = 0;
    GLuint mDesktopCopyProgram = 0;
    GLint mDesktopCopyMvpUniform = -1;
    GLint mDesktopCopyTexMatrixUniform = -1;
    GLuint mDesktopFramebuffer = 0;
    float mDesktopTexMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    void *mDesktopQueue = nullptr;
    int mDesktopQueueSize[2] = {0, 0};
    int32_t mDesktopIndex = -1;    // queue texture holding the newest desktop frame
    int64_t mDesktopLastTimestampNs = -1;
    uint32_t mDesktopNewFrames = 0;
    bool mDesktopEnabled = true;   // debug.flow.desktop
    float mDesktopWidth = 3.2f;    // debug.flow.desktop.width, metres at FLOW_SCREEN_DISTANCE
    // Keypad reticle: where the head-aimed keypad laser meets the panel, drawn into the layer
    // texture (the layer covers SteamVR's own reticle). Needs a fresh copy whenever it moves.
    bool desktopReticleUv(float *u, float *v) const;
    GLuint mReticleProgram = 0;
    GLint mReticleMvpUniform = -1;
    bool mDesktopReticleDrawn = false;
    void dumpEyeBuffer();
    void updateHandTrackingEnabled();
    void updateHands();
    void logHandsIfNeeded(float seconds);
    const WVR_PoseState_t *findSentPose(uint32_t sequence) const;

    WVR_DevicePosePair_t mDevicePairs[WVR_DEVICE_COUNT_LEVEL_1];
    uint32_t mRenderWidth = 0;
    uint32_t mRenderHeight = 0;
    void *mLeftQueue = nullptr;
    void *mRightQueue = nullptr;
    std::vector<EyeTarget> mLeftTargets;
    std::vector<EyeTarget> mRightTargets;
    uint32_t mLeftIndex = 0;
    uint32_t mRightIndex = 0;
    GLuint mPatternProgram = 0;
    GLint mPatternFrameUniform = -1;
    GLint mPatternMvpUniform = -1;
    GLuint mDecoderProgram = 0;
    GLuint mDecoderTexture = 0;
    GLint mDecoderMvpUniform = -1;
    GLint mDecoderTexMatrixUniform = -1;
    GLint mDecoderUvRectUniform = -1;
    GLint mDecoderTexelUniform = -1;
    GLint mDecoderSharpenUniform = -1;
    float mSharpenAmount = -1.0f; // set by updateSharpenAmount()
    std::string mEyeDumpSeen; // last debug.flow.dumpeye value acted on
    bool mEyeDumpChecked = false;
    bool mEyeDumpPending = false;
    bool mLayerTest = false;
    int mLayerTestEye = 1;   // eye shown as a compositor layer: 0 = left, 1 = right
    int mLayerTestShape = 0; // 0 = quad, 1 = cylinder
    int mLayerTestImage = 0; // 0 = test_1080.png, 1 = test_4k.png
    float mLayerTestWidth = 3.2f; // metres, at FLOW_SCREEN_DISTANCE
    int mLayerTestSource = 0;      // debug.flow.layertest.src: 0 = Wave texture queue, 1 = plain GL texture
    bool mLayerTestHeadLocked = false; // debug.flow.layertest.headlocked: 2 m straight ahead of the head
    GLuint mImageProgram = 0;
    GLint mImageMvpUniform = -1;
    GLuint mLayerTestTextures[2] = {0, 0};
    // The compositor runs outside this process, so a layer must use a Wave texture queue; each
    // queue texture holds a 1:1 copy of the image (bottom row first, as the layer expects).
    void *mLayerTestQueues[2] = {nullptr, nullptr};
    int mLayerTestSize[2][2] = {};
    bool mLayerTestLoadFailed[2] = {false, false};
    uint32_t mMaxFrameLayers = 0;
    bool mLayerSubmitFailed = false;
    float mFrameSharpness = -1.0f; // debug.flow.fse at startup; < 0 = feature not initialized
    float mDecoderTexMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool mDecoderReady = false;

    // Virtual screen, world-locked in the WVR_PoseOriginModel_OriginOnHead space.
    Mat4 mProjection[2] = {};
    Mat4 mEyeFromHead[2] = {};
    float mScreenCenter[3] = {0.0f, 0.0f, -2.0f};
    float mScreenSinYaw = 0.0f;
    float mScreenCosYaw = 1.0f;
    bool mScreenPlaced = false;
    WVR_PoseState_t mHmdPose = {};
    uint32_t mDecoderFrames = 0;
    uint32_t mDecoderUpdateCount = 0;
    uint64_t mDecoderUpdateTotalUsec = 0;
    uint64_t mDecoderUpdateMaxUsec = 0;

    timeval mLastTime = {};
    uint32_t mFrameCount = 0;
    uint32_t mTotalFrames = 0;
    float mLastFps = 0.0f;
    bool mHmdValid = false;
    bool mHmd6Dof = false;
    float mHmdX = 0.0f;
    float mHmdY = 0.0f;
    float mHmdZ = 0.0f;
    int mPoseSocket = -1;
    uint32_t mPoseSequence = 0;

    // Poses sent to the PC, by sequence, so a SteamVR frame can be submitted with the pose
    // it was rendered from. 256 entries at 75 Hz covers ~3.4 s of round trip.
    struct PoseHistoryEntry {
        uint32_t sequence = 0;
        WVR_PoseState_t pose = {};
    };
    static const uint32_t kPoseHistorySize = 256;
    PoseHistoryEntry mPoseHistory[kPoseHistorySize];
    uint32_t mDisplayedPoseSequence = 0;
    uint64_t mPoseAgeTotal = 0;
    uint32_t mPoseAgeCount = 0;
    uint32_t mPoseAgeMax = 0;
    uint32_t mPoseAgeMisses = 0;
    int64_t mLastFrameTimestampNs = 0;
    uint32_t mNewVideoFrames = 0; // distinct decoded frames latched since last log

    // Hand tracking (on by default; `adb shell setprop debug.flow.hands 0|1` toggles it).
    bool mHandsActive = false;
    uint32_t mHandJointCount = 0;
    std::vector<WVR_Pose_t> mHandJoints[2]; // [0] left, [1] right
    WVR_HandTrackingData_t mHandData = {};
    WVR_HandPoseData_t mHandPose = {};
    uint32_t mHandQueries = 0;
    uint32_t mHandQueryFailures = 0;
    uint32_t mHandValid[2] = {};
    uint32_t mHandPinching[2] = {}; // index pinch strength >= 0.8
    uint64_t mHandQueryTotalUsec = 0;
    uint64_t mHandQueryMaxUsec = 0;
    int64_t mHandLastTimestamp = 0;
    uint32_t mHandNewSamples = 0;   // queries that returned a new tracker timestamp
};

void FlowProbe_SetActivity(JNIEnv *env, jobject activity);
void FlowProbe_SetDecoderSurfaceTexture(JNIEnv *env, jobject surfaceTexture);
void FlowProbe_ClearDecoderSurfaceTexture(JNIEnv *env);
void FlowProbe_SetDecoderStreamInfo(int width, int height, int layout);
void FlowProbe_SetFramePoseSequence(int64_t ptsUs, uint32_t poseSequence);
void FlowProbe_SetPoseTargetHost(const char *host);
void FlowProbe_SetDesktopSurfaceTexture(JNIEnv *env, jobject surfaceTexture);
void FlowProbe_ClearDesktopSurfaceTexture(JNIEnv *env);
void FlowProbe_SetDesktopStreamInfo(int width, int height);
void FlowProbe_SetDesktopPanel(int flags, const float transform[12], float width, float curvature);
