LOCAL_PATH := $(call my-dir)

VR_SDK_LIB := $(firstword \
        $(realpath $(VR_SDK_ROOT)/jni/$(TARGET_ARCH_ABI)) \
        $(realpath $(VR_SDK_ROOT)/lib/$(TARGET_ARCH_ABI)) )

ifeq ($(OS),Windows_NT)
VR_SDK_LIB := $(VR_SDK_ROOT)/jni/$(TARGET_ARCH_ABI)
endif

include $(CLEAR_VARS)
LOCAL_MODULE := wvr_api
LOCAL_SRC_FILES := $(VR_SDK_LIB)/libwvr_api.so
include $(PREBUILT_SHARED_LIBRARY)

COMMON_INCLUDES := \
    $(VR_SDK_ROOT)/include \
    $(LOCAL_PATH)

include $(CLEAR_VARS)
LOCAL_MODULE := hellovr_common
LOCAL_C_INCLUDES := $(COMMON_INCLUDES)
LOCAL_SRC_FILES := \
    hellovr.cpp \
    Context.cpp
LOCAL_CFLAGS := -g
LOCAL_LDLIBS := -llog -ljnigraphics -landroid -lEGL -lGLESv3
LOCAL_SHARED_LIBRARIES := wvr_api
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := hellovr_jni
LOCAL_C_INCLUDES := $(COMMON_INCLUDES)
LOCAL_SRC_FILES := jni.cpp
LOCAL_CFLAGS := -g
LOCAL_LDLIBS := -llog -landroid
LOCAL_SHARED_LIBRARIES := hellovr_common wvr_api
include $(BUILD_SHARED_LIBRARY)
