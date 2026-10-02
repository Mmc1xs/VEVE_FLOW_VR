// "WaveVR SDK 
// © 2017 HTC Corporation. All Rights Reserved.
//
// Unless otherwise required by copyright law and practice,
// upon the execution of HTC SDK license agreement,
// HTC grants you access to and use of the WaveVR SDK(s).
// You shall fully comply with all of HTC’s SDK license agreement terms and
// conditions signed by you and all SDK and API requirements,
// specifications, and documentation provided by HTC to You."

#pragma once
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_activity.h>
#include <android/bitmap.h>

class Context;
class EnvWrapper {
private:
    JavaVM * mVM;
    JNIEnv * mEnv;
    bool mNeedAttach;

public:
    EnvWrapper(JavaVM * vm, JNIEnv * env, bool needAttach);

    ~EnvWrapper();

    inline JNIEnv * get() {
        return mEnv;
    }
};

class BitmapFactory {
private:
    jclass mBitmapFactoryClass;
    jmethodID mIdDecordByteArray;
    void recycleBitmap(JNIEnv *env, jobject bitmap);
    uint8_t * decodeAndroidBitmap(JNIEnv * env, jobject jBitmap, AndroidBitmapInfo & outputInfo);

public:
    BitmapFactory(JNIEnv * env);
    void clean(JNIEnv * env);
    
    /**
     * Put the source file byte array, and its size in byte.  And return the 
     * decoded void* bitmap array.  You can retrive bitmap info from outputInfo.
     * Remember to delete returned array.
    **/
    uint8_t * decodeByteArray(JNIEnv * env, const void * array, size_t size, AndroidBitmapInfo & outputInfo);
};

class Context
{
private:
    ANativeActivity * mActivityNative;
    JavaVM * mVM;

    jobject mAssetManagerInstance;
    AAssetManager * mAssetManager;

    BitmapFactory * mBitmapFactory;

    static Context * sInstance;

public:
    Context(JavaVM* vm);
    Context();

    ~Context();

    void init(JNIEnv * env, jobject assetManagerInstance);

    inline static Context * getInstance() {
        return sInstance;
    }

    EnvWrapper getEnv();

    inline AAssetManager * getAssetManager() {
        return mAssetManager;
    }
    
    inline BitmapFactory * getBitmapFactory() {
        return mBitmapFactory;
    }
};


class AssetFile
{
private:
    AAssetManager * mAssetManager;
    const char * mPath;
    AAsset * mAsset;

public:
    AssetFile(AAssetManager * assetManager, const char * assetPath);

    ~AssetFile();

    bool open();
    
    void close();
    
    const void * getBuffer();

    size_t getLength();

    char * toString();
};

