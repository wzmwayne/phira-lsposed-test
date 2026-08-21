LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE     := phira_agent
LOCAL_SRC_FILES  := phira_agent.c
LOCAL_LDLIBS     := -llog -landroid

include $(BUILD_SHARED_LIBRARY)
