LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ISchedulingPolicyService.cpp \
    SchedulingPolicyService.cpp

# FIXME Move this library to frameworks/native
LOCAL_MODULE := libscheduling_policy

include $(BUILD_STATIC_LIBRARY)

# build libmtktimestretch.so
include $(CLEAR_VARS)
LOCAL_C_INCLUDES := \
$(call include-path-for, audio-utils)\
$(TOP)/frameworks/av/services/AudioFlinger\
$(MTK_PATH_SOURCE)/external/AudioTimeStretch/inc\

LOCAL_SHARED_LIBRARIES := \
    libtimestretch\
    libcutils\
    libutils \
    liblog \
    libdl \

LOCAL_SRC_FILES += \
     AudioMTKTimeStretch.cpp \
     AudioUtilmtk.cpp

LOCAL_MULTILIB := both
LOCAL_MODULE:= libMTKAudioTimeStretch
include $(BUILD_SHARED_LIBRARY)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    ServiceUtilities.cpp

# FIXME Move this library to frameworks/native
LOCAL_MODULE := libserviceutility

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libbinder

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    AudioFlinger.cpp            \
    Threads.cpp                 \
    Tracks.cpp                  \
    AudioHwDevice.cpp           \
    AudioStreamOut.cpp          \
    SpdifStreamOut.cpp          \
    Effects.cpp                 \
    AudioMixer.cpp.arm          \
    BufferProviders.cpp         \
    PatchPanel.cpp              \
    StateQueue.cpp

LOCAL_C_INCLUDES := \
    $(TOPDIR)frameworks/av/services/audiopolicy \
    $(TOPDIR)external/sonic \
    $(call include-path-for, audio-effects) \
    $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudiospdif \
    libaudioutils \
    libcommon_time_client \
    libcutils \
    libutils \
    liblog \
    libbinder \
    libmedia \
    libnbaio \
    libhardware \
    libhardware_legacy \
    libeffects \
    libpowermanager \
    libserviceutility \
    libsonic

LOCAL_STATIC_LIBRARIES := \
    libscheduling_policy \
    libcpustats \
    libmedia_helper

#mtk added
LOCAL_C_INCLUDES += \
        $(TOP)/frameworks/av/include/media \
        $(TOP)/system/media/audio_effects/include\
        $(MTK_PATH_SOURCE)/external/audiodcremoveflt \
        $(MTK_PATH_SOURCE)/external/AudioCompensationFilter \
        $(MTK_PATH_SOURCE)/external/AudioComponentEngine \
        $(MTK_PATH_SOURCE)/external/AudioDCRemoval \
        $(MTK_PATH_SOURCE)/external/blisrc/blisrc32 \
        $(MTK_PATH_SOURCE)/external/blisrc \
   $(MTK_PATH_SOURCE)/external/blisrc/blisrc16 \
        $(MTK_PATH_SOURCE)/external/limiter \
        $(MTK_PATH_SOURCE)/external/shifter \
        $(MTK_PATH_SOURCE)/external/bessurround_mtk/inc \
        $(MTK_PATH_SOURCE)/external/bessound_HD \
        $(MTK_PATH_SOURCE)/external/bessound \
        $(MTK_PATH_SOURCE)/external/AudioTimeStretch/inc\
        $(MTK_PATH_SOURCE)/hardware/audio/common/include \
        $(MTK_PATH_CUSTOM)/hal/audioflinger/audio

ifeq ($(strip $(MTK_LOSSLESS_BT_SUPPORT)),yes)
    LOCAL_CFLAGS += -DMTK_LOSSLESS_BT_SUPPORT
    LOCAL_SRC_FILES += AudioLosslessBTBroadcast.cpp
endif

ifeq ($(MTK_AUDIO),yes)
    AudioDriverIncludePath := aud_drv


    LOCAL_CFLAGS += -DMTK_AUDIO
    LOCAL_CFLAGS += -DMTK_AUDIO_DCREMOVAL
    LOCAL_CFLAGS += -DMTK_HDMI_MULTI_CHANNEL_SUPPORT

    ifeq ($(strip $(MTK_USE_RT_PRIORITY)),yes)
        LOCAL_CFLAGS += -DMTK_USE_RT_PRIORITY=1
    else
        LOCAL_CFLAGS += -DMTK_USE_RT_PRIORITY=0
    endif

#ifeq ($(strip $(MTK_BESLOUDNESS_SUPPORT)),yes)
    LOCAL_CFLAGS += -DMTK_AUDIOMIXER_ENABLE_DRC
#endif

ifeq ($(strip $(TARGET_BUILD_VARIANT)),eng)
  LOCAL_CFLAGS += -DCONFIG_MT_ENG_BUILD
endif

ifeq ($(strip $(MTK_CROSSMOUNT_SUPPORT)),yes)
    LOCAL_CFLAGS += -DMTK_CROSSMOUNT_SUPPORT
endif

    ifeq ($(strip $(MTK_DOLBY_DAP_SUPPORT)), yes)
    endif
    ifeq ($(strip $(MTK_BESSURROUND_SUPPORT)),yes)
    LOCAL_CFLAGS += -DMTK_BESSURROUND_ENABLE
    LOCAL_SHARED_LIBRARIES+= libbessurround_mtk
    endif

    LOCAL_CFLAGS += -DMTK_HIFI_AUDIO

ifeq ($(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5)
  LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5
else
  ifeq ($(strip $(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV)),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4)
    LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4
  endif
endif

    ifeq ($(strip $(MTK_HIGH_RESOLUTION_AUDIO_SUPPORT)),no)
    LOCAL_CFLAGS += -DMTK_AUDIO_USE_16BIT
    endif
    LOCAL_SRC_FILES += \
        AudioUtilmtk.cpp\
        AudioMTKSurroundMix.cpp


    LOCAL_SHARED_LIBRARIES += \
        libblisrc \
        libaudiocompensationfilter \
        libaudiocomponentengine \
        libblisrc32 \
        libMTKAudioTimeStretch\
        libmtklimiter \
        libaudiomtkdcremoval \
        libaudiodcrflt

    LOCAL_STATIC_LIBRARIES +=
  LOCAL_CFLAGS += -DDEBUG_AUDIO_PCM
  LOCAL_CFLAGS += -DDEBUG_MIXER_PCM

# SRS Processing
        ifeq ($(strip $(HAVE_SRSAUDIOEFFECT_FEATURE)),yes)
            LOCAL_CFLAGS += -DHAVE_SRSAUDIOEFFECT
            include $(MTK_ROOT)/external/SRS_AudioEffect/srs_processing/AF_PATCH.mk
        endif
# SRS Processing

# MATV ANALOG SUPPORT
    ifeq ($(HAVE_MATV_FEATURE),yes)
  ifeq ($(MTK_MATV_ANALOG_SUPPORT),yes)
    LOCAL_CFLAGS += -DMATV_AUDIO_LINEIN_PATH
  endif
    endif
# MATV ANALOG SUPPORT

# MTK_DOWNMIX_ENABLE
    LOCAL_CFLAGS += -DMTK_DOWNMIX_ENABLE
# MTK_DOWNMIX_ENABLE

 #MTK_TIMESTRETCH_ENABLE
ifeq ($(MTK_SLOW_MOTION_VIDEO_SUPPORT),yes)
   LOCAL_CFLAGS += -DTIME_STRETCH_ENABLE
endif
# MTK_TIMESTRETCH_ENABLE
endif

LOCAL_MODULE:= libaudioflinger
LOCAL_32_BIT_ONLY := true

LOCAL_SRC_FILES += \
    AudioWatchdog.cpp        \
    FastCapture.cpp          \
    FastCaptureDumpState.cpp \
    FastCaptureState.cpp     \
    FastMixer.cpp            \
    FastMixerDumpState.cpp   \
    FastMixerState.cpp       \
    FastThread.cpp           \
    FastThreadDumpState.cpp  \
    FastThreadState.cpp

LOCAL_CFLAGS += -DSTATE_QUEUE_INSTANTIATIONS='"StateQueueInstantiations.cpp"'

LOCAL_CFLAGS += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)

#
# build audio resampler test tool
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=               \
    test-resample.cpp           \

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)

LOCAL_STATIC_LIBRARIES := \
    libsndfile

LOCAL_SHARED_LIBRARIES := \
    libaudioresampler \
    libaudioutils \
    libdl \
    libcutils \
    libutils \
    liblog

ifeq ($(MTK_AUDIO),yes)
    LOCAL_CFLAGS += -DMTK_AUDIO
    LOCAL_C_INCLUDES += \
        $(MTK_PATH_SOURCE)/external/blisrc/blisrc32 \
   $(MTK_PATH_SOURCE)/external/blisrc/blisrc16 \
        $(MTK_PATH_SOURCE)/external/blisrc
    LOCAL_SHARED_LIBRARIES += \
    libblisrc \
    libblisrc32
 endif

 ifeq ($(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5)
  LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5
else
  ifeq ($(strip $(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV)),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4)
    LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4
  endif
endif

LOCAL_MODULE:= test-resample
LOCAL_32_BIT_ONLY := true

LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioResampler.cpp.arm \
    AudioResamplerCubic.cpp.arm \
    AudioResamplerSinc.cpp.arm \
    AudioResamplerDyn.cpp.arm

LOCAL_C_INCLUDES := \
    $(call include-path-for, audio-utils)\

ifeq ($(MTK_AUDIO),yes)
    LOCAL_CFLAGS += -DMTK_AUDIO

LOCAL_SHARED_LIBRARIES := \
    libblisrc \
    libblisrc32

LOCAL_C_INCLUDES += \
    $(MTK_PATH_SOURCE)/external/blisrc/blisrc32 \
    $(MTK_PATH_SOURCE)/external/blisrc/blisrc16 \
    $(MTK_PATH_SOURCE)/external/blisrc

LOCAL_SRC_FILES += \
    AudioResamplermtk.cpp\
    AudioUtilmtk.cpp

#ifeq ($(strip $(MTK_HIGH_RESOLUTION_AUDIO_SUPPORT)),yes)
   # LOCAL_CFLAGS += -DMTK_HD_AUDIO_ARCHITECTURE
#    LOCAL_SRC_FILES += \
#        AudioResamplerMTK32.cpp
#endif
endif

ifeq ($(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5)
  LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V5
else
  ifeq ($(strip $(MTK_AUDIO_BLOUD_CUSTOMPARAMETER_REV)),MTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4)
    LOCAL_CFLAGS += -DMTK_AUDIO_BLOUD_CUSTOMPARAMETER_V4
  endif
endif

LOCAL_SHARED_LIBRARIES += \
    libcutils \
    libutils \
    libdl \
    liblog

LOCAL_MODULE := libaudioresampler
LOCAL_32_BIT_ONLY := true

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
