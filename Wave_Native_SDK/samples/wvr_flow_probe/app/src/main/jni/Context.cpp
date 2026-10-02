// "WaveVR SDK 
// © 2017 HTC Corporation. All Rights Reserved.
//
// Unless otherwise required by copyright law and practice,
// upon the execution of HTC SDK license agreement,
// HTC grants you access to and use of the WaveVR SDK(s).
// You shall fully comply with all of HTC’s SDK license agreement terms and
// conditions signed by you and all SDK and API requirements,
// specifications, and documentation provided by HTC to You."

#define LOG_TAG "Context"
#include <stdlib.h>
#include <string.h>
#include <Context.h>
#include "log.h"

EnvWrapper::EnvWrapper(JavaVM * vm, JNIEnv * env, bool needAttach) :
        mVM(vm), mEnv(env), mNeedAttach(needAttach) {
    if (mNeedAttach && mEnv != NULL) {
        int ret = mVM->AttachCurrentThread(&mEnv, NULL);
        if (ret != 0) {
            mEnv = NULL;
            mNeedAttach = false;
        }
    }
}

EnvWrapper::~EnvWrapper() {
    if (mNeedAttach && mEnv != NULL) {
        mVM->DetachCurrentThread();
    }
}

BitmapFactory::BitmapFactory(JNIEnv * env) {
    const char * BitmapFactoryClassName = "android/graphics/BitmapFactory";
    jclass localClazz = env->FindClass(BitmapFactoryClassName);
    if (localClazz == NULL) {
        LOGE("Unable to find class %s", BitmapFactoryClassName);
        return;
    }
    
    mBitmapFactoryClass = reinterpret_cast<jclass>(env->NewGlobalRef(localClazz));
    mIdDecordByteArray = env->GetStaticMethodID(mBitmapFactoryClass, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
}

void BitmapFactory::clean(JNIEnv * env) {
    if (mBitmapFactoryClass)
        env->DeleteGlobalRef(mBitmapFactoryClass);
}

uint8_t * BitmapFactory::decodeByteArray(JNIEnv * env, const void * array, size_t size, AndroidBitmapInfo & outputInfo)
{
    jbyteArray jarray = env->NewByteArray(size);
    env->SetByteArrayRegion(jarray, 0, size, (jbyte *) array);

    jobject jBitmap = env->CallStaticObjectMethod(mBitmapFactoryClass, mIdDecordByteArray, jarray, 0, size);
    uint8_t * pixels = NULL;
    if (jBitmap == NULL) {
        LOGE("Unable to decode");
    } else {
        pixels = decodeAndroidBitmap(env, jBitmap, outputInfo);
        recycleBitmap(env, jBitmap);
        env->DeleteLocalRef(jBitmap);
    }
    env->DeleteLocalRef(jarray);
    return pixels;
}

uint8_t * BitmapFactory::decodeAndroidBitmap(JNIEnv * env, jobject jBitmap, AndroidBitmapInfo & outputInfo)
{
    int ret = -1;
    AndroidBitmapInfo info;
    if ((ret = AndroidBitmap_getInfo(env, jBitmap, &info)) < 0) {
        LOGE("AndroidBitmap_getInfo() failed ! error=%d", ret);
        return NULL;
    }
    // Read Bitmap pixels
    uint8_t * bitmapPixels = NULL;
    if ((ret = AndroidBitmap_lockPixels(env, jBitmap, (void**)&bitmapPixels)) < 0) {
        LOGE("AndroidBitmap_lockPixels() failed ! error=%d", ret);
        return NULL;
    }
    outputInfo = info;
    const size_t bmpSize = info.stride * info.height;
    uint8_t * bitmap = new uint8_t [bmpSize];
    memcpy(bitmap, bitmapPixels, bmpSize);
    AndroidBitmap_unlockPixels(env, jBitmap);
    return bitmap;
}

void BitmapFactory::recycleBitmap(JNIEnv *env, jobject bitmap)
{
    jclass clsBmp = env->GetObjectClass(bitmap);
    jmethodID recycleMethod = env->GetMethodID(clsBmp, "recycle", "()V");
    env->CallVoidMethod(bitmap, recycleMethod);
    env->DeleteLocalRef(clsBmp);
}

Context * Context::sInstance = NULL;

Context::Context(JavaVM* vm) : mVM(vm), mActivityNative(NULL) {
    sInstance = this;
}

Context::Context() : mVM(NULL), mActivityNative(NULL) {
    sInstance = this;
}

Context::~Context() {
    if (mActivityNative) {
        if (mBitmapFactory) delete mBitmapFactory;
        return;
    }
        
    EnvWrapper envwrapper = getEnv();
    JNIEnv * env = envwrapper.get();

    if (mAssetManagerInstance)
        env->DeleteGlobalRef(mAssetManagerInstance);
    mBitmapFactory->clean(env);
    if (mBitmapFactory) delete mBitmapFactory;
    mBitmapFactory = NULL;
}

void Context::init(JNIEnv * env, jobject assetManagerInstance) {
    mAssetManagerInstance = env->NewGlobalRef(assetManagerInstance);

    mAssetManager = AAssetManager_fromJava(env, mAssetManagerInstance);
    if (mAssetManager == NULL) {
        LOGE("Fail to get AssetManager from Java");
        abort();
    }

    mBitmapFactory = new BitmapFactory(env);
}

EnvWrapper Context::getEnv() {
    if (mActivityNative != NULL)
        return EnvWrapper(mVM, mActivityNative->env, true);

    JNIEnv * env = NULL;
    int stat;
    stat = mVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (stat == JNI_EDETACHED) {
        return EnvWrapper(mVM, env, true);
    } else if (stat == JNI_OK) {
        return EnvWrapper(mVM, env, false);
    } else if (stat == JNI_EVERSION) {
        LOGE("GetEnv: version not supported");
    } else {
        LOGE("Could not get java env!");
    }
    Context * context = Context::getInstance();
    context->getEnv();
    return EnvWrapper(mVM, NULL, false);
}

AssetFile::AssetFile(AAssetManager * assetManager, const char * assetPath) :
    mAssetManager(assetManager),
    mPath(assetPath),
    mAsset(NULL) {
}

AssetFile::~AssetFile() {
    close();
}

bool AssetFile::open() {
    if (mAsset != NULL)
        return true;

    if (mPath == NULL) {
        LOGE("File Path is NULL");
        return false;
    }

    if (mAssetManager == NULL) {
        LOGE("AssetManager is NULL");
        return false;
    }

    mAsset = AAssetManager_open(mAssetManager, mPath, AASSET_MODE_UNKNOWN);
    if (mAsset == NULL) {
        LOGE("Open file failed: %s", mPath);
        return false;
    }

    return true;
}

void AssetFile::close() {
    if (mAsset == NULL)
        return;
    AAsset_close(mAsset);
    mAsset = NULL;
}

const void * AssetFile::getBuffer() {
    if (mAsset == NULL)
        return NULL;
    return AAsset_getBuffer(mAsset);
}

size_t AssetFile::getLength() {
    if (mAsset == NULL)
        return 0;
    return AAsset_getLength(mAsset);
}

char * AssetFile::toString() {
    if (mAsset == NULL)
        return NULL;
    const void * buffer = AAsset_getBuffer(mAsset);
    const size_t N = AAsset_getLength(mAsset);
    if (N == 0)
        return NULL;
    char * dup = new char [N + 1];
    memcpy(dup, buffer, N);
    dup[N] = 0;
    return dup;
}
