#pragma once

#include <stdint.h>
#include <sys/time.h>
#include <vector>

#include <jni.h>
#include <GLES3/gl3.h>
#include <wvr/wvr.h>
#include <wvr/wvr_device.h>
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
    bool renderEye(WVR_Eye eye, void *queue, std::vector<EyeTarget> *targets, uint32_t index);
    void logProbeIfNeeded();
    void recordPoseAge();
    void updateSharpenAmount();
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
};

void FlowProbe_SetActivity(JNIEnv *env, jobject activity);
void FlowProbe_SetDecoderSurfaceTexture(JNIEnv *env, jobject surfaceTexture);
void FlowProbe_ClearDecoderSurfaceTexture(JNIEnv *env);
void FlowProbe_SetDecoderStreamInfo(int width, int height, int layout);
void FlowProbe_SetFramePoseSequence(int64_t ptsUs, uint32_t poseSequence);
void FlowProbe_SetPoseTargetHost(const char *host);
