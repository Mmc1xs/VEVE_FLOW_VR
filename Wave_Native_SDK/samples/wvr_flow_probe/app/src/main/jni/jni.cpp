#define LOG_TAG "FlowProbeJNI"

#include <jni.h>
#include <unistd.h>

#include <Context.h>
#include <hellovr.h>
#include <log.h>
#include <wvr/wvr.h>

int main(int argc, char *argv[]) {
    LOGI("FLOW_MIN_PROBE native main start");
    MainApplication *app = new MainApplication();
    if (!app) {
        return 1;
    }

    if (!app->initVR() || !app->initGL()) {
        LOGE("FLOW_MIN_PROBE initialization failed");
        app->shutdownGL();
        app->shutdownVR();
        delete app;
        return 1;
    }

    while (true) {
        if (app->handleInput()) {
            break;
        }
        app->updateHMDPose();
        if (app->renderFrame()) {
            break;
        }
    }

    app->shutdownGL();
    app->shutdownVR();
    delete app;
    LOGI("FLOW_MIN_PROBE native main end");
    return 0;
}

extern "C" {
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_init(JNIEnv *env, jobject activity, jobject assetManager);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDecoderSurfaceTexture(JNIEnv *env, jobject activity, jobject surfaceTexture);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_clearDecoderSurfaceTexture(JNIEnv *env, jobject activity);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDecoderStreamInfo(JNIEnv *env, jclass clazz, jint width, jint height, jint layout);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setFramePoseSequence(JNIEnv *env, jclass clazz, jlong ptsUs, jint poseSequence);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setPoseTargetHost(JNIEnv *env, jclass clazz, jstring host);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopSurfaceTexture(JNIEnv *env, jobject activity, jobject surfaceTexture);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_clearDesktopSurfaceTexture(JNIEnv *env, jobject activity);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopStreamInfo(JNIEnv *env, jclass clazz, jint width, jint height);
JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopPanel(JNIEnv *env, jclass clazz, jint flags, jfloatArray transform, jfloat width);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopPanel(JNIEnv *env, jclass clazz, jint flags, jfloatArray transform, jfloat width) {
    float values[12] = {};
    env->GetFloatArrayRegion(transform, 0, 12, values);
    FlowProbe_SetDesktopPanel(flags, values, width);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopSurfaceTexture(JNIEnv *env, jobject activity, jobject surfaceTexture) {
    FlowProbe_SetDesktopSurfaceTexture(env, surfaceTexture);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_clearDesktopSurfaceTexture(JNIEnv *env, jobject activity) {
    FlowProbe_ClearDesktopSurfaceTexture(env);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDesktopStreamInfo(JNIEnv *env, jclass clazz, jint width, jint height) {
    FlowProbe_SetDesktopStreamInfo(width, height);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_init(JNIEnv *env, jobject activity, jobject assetManager) {
    Context::getInstance()->init(env, assetManager);
    FlowProbe_SetActivity(env, activity);
    WVR_RegisterMain(main);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDecoderSurfaceTexture(JNIEnv *env, jobject activity, jobject surfaceTexture) {
    FlowProbe_SetDecoderSurfaceTexture(env, surfaceTexture);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_clearDecoderSurfaceTexture(JNIEnv *env, jobject activity) {
    FlowProbe_ClearDecoderSurfaceTexture(env);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setDecoderStreamInfo(JNIEnv *env, jclass clazz, jint width, jint height, jint layout) {
    FlowProbe_SetDecoderStreamInfo(width, height, layout);
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setFramePoseSequence(JNIEnv *env, jclass clazz, jlong ptsUs, jint poseSequence) {
    FlowProbe_SetFramePoseSequence(ptsUs, static_cast<uint32_t>(poseSequence));
}

JNIEXPORT void JNICALL Java_com_htc_vr_samples_wvr_1flow_1probe_MainActivity_setPoseTargetHost(JNIEnv *env, jclass clazz, jstring host) {
    const char *chars = env->GetStringUTFChars(host, nullptr);
    FlowProbe_SetPoseTargetHost(chars);
    env->ReleaseStringUTFChars(host, chars);
}

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    new Context(vm);
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM *vm, void *reserved) {
    delete Context::getInstance();
}
