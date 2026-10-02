// "WaveVR SDK 
// © 2017 HTC Corporation. All Rights Reserved.
//
// Unless otherwise required by copyright law and practice,
// upon the execution of HTC SDK license agreement,
// HTC grants you access to and use of the WaveVR SDK(s).
// You shall fully comply with all of HTC’s SDK license agreement terms and
// conditions signed by you and all SDK and API requirements,
// specifications, and documentation provided by HTC to You."

/** @file
    @brief Header

    @date 2016

    @author
    hTC, Inc.
*/

#ifndef _LOG
#define _LOG

#ifndef LOG_TAG
#define LOG_TAG "vrsample"
#endif

#include <android/log.h>

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__)

#define LogD(tag, ...) __android_log_print(ANDROID_LOG_DEBUG, tag, __VA_ARGS__)
#define LogE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)

#define LOGENTRY(...) vrsample::log::LogEntry local_log_entry(LOG_TAG, __func__)
#define LOGLEAVE()

namespace vrsample {
namespace log {
class LogEntry {
private:
    const char * mLogTag;
    const char * mFuncName;
    LogEntry() {
    }
public:
    LogEntry(const char * logtag, const char * func) : mLogTag(logtag), mFuncName(func) {
        __android_log_print(ANDROID_LOG_DEBUG, mLogTag, " +++%s", mFuncName);
    }
    LogEntry(const char * logtag, const char * func, const char * format, ...) : mLogTag(logtag), mFuncName(func) {
        __android_log_print(ANDROID_LOG_DEBUG, mLogTag, " +++%s", mFuncName);
        if (format != nullptr) {
            va_list args;
            va_start (args, format);
            __android_log_vprint(ANDROID_LOG_DEBUG, mLogTag, format, args);
            va_end(args);
        }
    }
    ~LogEntry() {
        __android_log_print(ANDROID_LOG_DEBUG, mLogTag, " ---%s", mFuncName);
    }
};
}  // namespace log
}  // namespace vrsample

#endif  // __LOG
