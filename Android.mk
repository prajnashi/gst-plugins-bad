LOCAL_PATH := $(call my-dir)

GST_PLUGINS_BAD_TOP := $(LOCAL_PATH)

include $(CLEAR_VARS)

include $(GST_PLUGINS_BAD_TOP)/sys/fbdev/Android.mk
include $(GST_PLUGINS_BAD_TOP)/gst/selector/Android.mk

