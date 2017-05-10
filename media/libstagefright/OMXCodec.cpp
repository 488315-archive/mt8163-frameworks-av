/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "OMXCodec"

#ifdef __LP64__
#define OMX_ANDROID_COMPILE_AS_32BIT_ON_64BIT_PLATFORMS
#endif

#include <utils/Log.h>

#include "include/AACEncoder.h"

#include "include/ESDS.h"

#include <binder/IServiceManager.h>
#include <binder/MemoryDealer.h>
#include <binder/ProcessState.h>
#include <HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/SurfaceUtils.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/SkipCutBuffer.h>
#include <utils/Vector.h>

#ifdef MTK_AOSP_ENHANCEMENT
#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>
#ifdef HAVE_AEE_FEATURE
#include "aee.h"
#endif
#include <cutils/properties.h>
#include <cutils/log.h>
#include "graphics_mtk_defs.h"
#include "gralloc_mtk_defs.h"
#include <media/stagefright/OMXClient.h>
#endif //MTK_AOSP_ENHANCEMENT

#include <OMX_Audio.h>
#include <OMX_AudioExt.h>
#include <OMX_Component.h>
#include <OMX_IndexExt.h>
#include <OMX_VideoExt.h>
#include <OMX_AsString.h>

#include "include/avc_utils.h"

#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
#include "ds_config.h"
#endif // DOLBY_END

namespace android {

// Treat time out as an error if we have not received any output
// buffers after 3 seconds.
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_ENABLE_VIDEO_EDITOR
#define OMX_VE_AUDIO
#endif
const static int64_t VDeckBufferFilledEventTimeOutNs = 6000000000LL; //50000000000LL;// video decode need too much time
const static int64_t kPreRollTimeOutUs = 3000000LL;//pre roll time out

#define MP3_MULTI_FRAME_COUNT_IN_ONE_INPUTBUFFER_FOR_PURE_AUDIO 10
#define MP3_MULTI_FRAME_COUNT_IN_ONE_INPUTBUFFER_FOR_VIDEO 1
#define MP3_MULTI_FRAME_COUNT_IN_ONE_OUTPUTBUFFER_FOR_PURE_AUDIO 10
#define MP3_MULTI_FRAME_COUNT_IN_ONE_OUTPUTBUFFER_FOR_VIDEO 1

static int16_t mp3FrameCountInBuffer = 1;
#endif //MTK_AOSP_ENHANCEMENT
const static int64_t kBufferFilledEventTimeOutNs = 3000000000LL;

// OMX Spec defines less than 50 color formats. If the query for
// color format is executed for more than kMaxColorFormatSupported,
// the query will fail to avoid looping forever.
// 1000 is more than enough for us to tell whether the omx
// component in question is buggy or not.
const static uint32_t kMaxColorFormatSupported = 1000;

#define FACTORY_CREATE_ENCODER(name) \
static sp<MediaSource> Make##name(const sp<MediaSource> &source, const sp<MetaData> &meta) { \
    return new name(source, meta); \
}

#define FACTORY_REF(name) { #name, Make##name },

FACTORY_CREATE_ENCODER(AACEncoder)

static sp<MediaSource> InstantiateSoftwareEncoder(
        const char *name, const sp<MediaSource> &source,
        const sp<MetaData> &meta) {
    struct FactoryInfo {
        const char *name;
        sp<MediaSource> (*CreateFunc)(const sp<MediaSource> &, const sp<MetaData> &);
    };

    static const FactoryInfo kFactoryInfo[] = {
        FACTORY_REF(AACEncoder)
    };
    for (size_t i = 0;
         i < sizeof(kFactoryInfo) / sizeof(kFactoryInfo[0]); ++i) {
        if (!strcmp(name, kFactoryInfo[i].name)) {
            return (*kFactoryInfo[i].CreateFunc)(source, meta);
        }
    }

    return NULL;
}

#undef FACTORY_CREATE_ENCODER
#undef FACTORY_REF

#define CODEC_LOGI(x, ...) ALOGI("[%s] " x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGV(x, ...) ALOGV("[%s] " x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGD(x, ...) ALOGD("[%s] " x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGW(x, ...) ALOGW("[%s] " x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGE(x, ...) ALOGE("[%s] " x, mComponentName, ##__VA_ARGS__)

struct OMXCodecObserver : public BnOMXObserver {
    OMXCodecObserver() {
    }

    void setCodec(const sp<OMXCodec> &target) {
        mTarget = target;
    }

    // from IOMXObserver
    virtual void onMessages(const std::list<omx_message> &messages) {
        sp<OMXCodec> codec = mTarget.promote();

        if (codec.get() != NULL) {
            Mutex::Autolock autoLock(codec->mLock);
            for (std::list<omx_message>::const_iterator it = messages.cbegin();
                  it != messages.cend(); ++it) {
                codec->on_message(*it);
            }
            codec.clear();
        }
    }

protected:
    virtual ~OMXCodecObserver() {}

private:
    wp<OMXCodec> mTarget;

    OMXCodecObserver(const OMXCodecObserver &);
    OMXCodecObserver &operator=(const OMXCodecObserver &);
};

template<class T>
static void InitOMXParams(T *params) {
    COMPILE_TIME_ASSERT_FUNCTION_SCOPE(sizeof(OMX_PTR) == 4); // check OMX_PTR is 4 bytes.
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

static bool IsSoftwareCodec(const char *componentName) {
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
    if (!strncmp("OMX.dolby.", componentName, 10))
    {
        return true;
    }
#endif // DOLBY_UDC
    if (!strncmp("OMX.google.", componentName, 11)) {
        return true;
    }

    if (!strncmp("OMX.", componentName, 4)) {
        return false;
    }

    return true;
}

// A sort order in which OMX software codecs are first, followed
// by other (non-OMX) software codecs, followed by everything else.
static int CompareSoftwareCodecsFirst(
        const OMXCodec::CodecNameAndQuirks *elem1,
        const OMXCodec::CodecNameAndQuirks *elem2) {
    bool isOMX1 = !strncmp(elem1->mName.string(), "OMX.", 4);
    bool isOMX2 = !strncmp(elem2->mName.string(), "OMX.", 4);

    bool isSoftwareCodec1 = IsSoftwareCodec(elem1->mName.string());
    bool isSoftwareCodec2 = IsSoftwareCodec(elem2->mName.string());

    if (isSoftwareCodec1) {
        if (!isSoftwareCodec2) { return -1; }

        if (isOMX1) {
            if (isOMX2) { return 0; }

            return -1;
        } else {
            if (isOMX2) { return 0; }

            return 1;
        }

        return -1;
    }

    if (isSoftwareCodec2) {
        return 1;
    }

    return 0;
}

// static
void OMXCodec::findMatchingCodecs(
        const char *mime,
        bool createEncoder, const char *matchComponentName,
        uint32_t flags,
        Vector<CodecNameAndQuirks> *matchingCodecs) {
    matchingCodecs->clear();

    const sp<IMediaCodecList> list = MediaCodecList::getInstance();
    if (list == NULL) {
        return;
    }

    size_t index = 0;
    for (;;) {
        ssize_t matchIndex =
            list->findCodecByType(mime, createEncoder, index);

        if (matchIndex < 0) {
            break;
        }

        index = matchIndex + 1;

        const sp<MediaCodecInfo> info = list->getCodecInfo(matchIndex);
        CHECK(info != NULL);
        const char *componentName = info->getCodecName();

        // If a specific codec is requested, skip the non-matching ones.
        if (matchComponentName && strcmp(componentName, matchComponentName)) {
            continue;
        }

        // When requesting software-only codecs, only push software codecs
        // When requesting hardware-only codecs, only push hardware codecs
        // When there is request neither for software-only nor for
        // hardware-only codecs, push all codecs
        if (((flags & kSoftwareCodecsOnly) &&   IsSoftwareCodec(componentName)) ||
            ((flags & kHardwareCodecsOnly) &&  !IsSoftwareCodec(componentName)) ||
            (!(flags & (kSoftwareCodecsOnly | kHardwareCodecsOnly)))) {

            ssize_t index = matchingCodecs->add();
            CodecNameAndQuirks *entry = &matchingCodecs->editItemAt(index);
#ifdef MTK_AOSP_ENHANCEMENT
            if (entry == NULL){
                ALOGE("matchingCodecs returns NULL for mine(%s), createEncoder(%d), matchIndex(%zu), componentName(%s)",
                    mime, createEncoder, matchIndex, componentName);
                continue;
            }
#endif
            entry->mName = String8(componentName);
            entry->mQuirks = getComponentQuirks(info);

            ALOGV("matching '%s' quirks 0x%08x",
                  entry->mName.string(), entry->mQuirks);
        }
    }

    if (flags & kPreferSoftwareCodecs) {
        matchingCodecs->sort(CompareSoftwareCodecsFirst);
    }
}

// static
uint32_t OMXCodec::getComponentQuirks(
        const sp<MediaCodecInfo> &info) {
    uint32_t quirks = 0;
    if (info->hasQuirk("requires-allocate-on-input-ports")) {
        quirks |= kRequiresAllocateBufferOnInputPorts;
    }
    if (info->hasQuirk("requires-allocate-on-output-ports")) {
        quirks |= kRequiresAllocateBufferOnOutputPorts;
    }
    if (info->hasQuirk("output-buffers-are-unreadable")) {
        quirks |= kOutputBuffersAreUnreadable;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (info->hasQuirk("decoder-lies-about-number-of-channels")) {
        quirks |= kDecoderLiesAboutNumberOfChannels;
    }
    if (info->hasQuirk("supports-multiple-frames-per-input-buffer")) {
        quirks |= kSupportsMultipleFramesPerInputBuffer;
    }
    if (info->hasQuirk("wants-NAL-fragments")) {
        quirks |= kWantsNALFragments;
    }
    if (info->hasQuirk("avoid-memcpy-input-recording-frames")) {
        quirks |= kAvoidMemcopyInputRecordingFrames;
    }
    if (info->hasQuirk("decoder-need-prebuffer")) {
        quirks |= kDecoderNeedPrebuffer;
    }
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
    if (info->hasQuirk("needs-flush-before-disable"))
    {
        quirks |= kNeedsFlushBeforeDisable;
    }
    if (info->hasQuirk("requires-flush-complete-emulation"))
    {
        quirks |= kRequiresFlushCompleteEmulation;
    }
#endif //MTK_AUDIO_DDPLUS_SUPPORT
#endif //MTK_AOSP_ENHANCEMENT
    return quirks;
}

// static
bool OMXCodec::findCodecQuirks(const char *componentName, uint32_t *quirks) {
    const sp<IMediaCodecList> list = MediaCodecList::getInstance();
    if (list == NULL) {
        return false;
    }

    ssize_t index = list->findCodecByName(componentName);

    if (index < 0) {
        return false;
    }

    const sp<MediaCodecInfo> info = list->getCodecInfo(index);
    CHECK(info != NULL);
    *quirks = getComponentQuirks(info);

    return true;
}

// static
sp<MediaSource> OMXCodec::Create(
        const sp<IOMX> &omx,
        const sp<MetaData> &meta, bool createEncoder,
        const sp<MediaSource> &source,
        const char *matchComponentName,
        uint32_t flags,
        const sp<ANativeWindow> &nativeWindow) {
#ifdef MTK_AOSP_ENHANCEMENT
    ATRACE_CALL();
#endif
    int32_t requiresSecureBuffers;
    if (source->getFormat()->findInt32(
                kKeyRequiresSecureBuffers,
                &requiresSecureBuffers)
            && requiresSecureBuffers) {
        flags |= kIgnoreCodecSpecificData;
        flags |= kUseSecureInputBuffers;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    int32_t requiresMaxFBuffers;
    if (source->getFormat()->findInt32(
            kKeyRequiresMaxFBuffers,
            &requiresMaxFBuffers)
        && requiresMaxFBuffers)
    {
        flags |= kUseMaxOutputBuffers;
    }
#endif //MTK_AOSP_ENHANCEMENT

    const char *mime;
    bool success = meta->findCString(kKeyMIMEType, &mime);
    CHECK(success);

    Vector<CodecNameAndQuirks> matchingCodecs;
    findMatchingCodecs(
            mime, createEncoder, matchComponentName, flags, &matchingCodecs);

    if (matchingCodecs.isEmpty()) {
        ALOGV("No matching codecs! (mime: %s, createEncoder: %s, "
                "matchComponentName: %s, flags: 0x%x)",
                mime, createEncoder ? "true" : "false", matchComponentName, flags);
        return NULL;
    }

    sp<OMXCodecObserver> observer = new OMXCodecObserver;
    IOMX::node_id node = 0;

    for (size_t i = 0; i < matchingCodecs.size(); ++i) {
        const char *componentNameBase = matchingCodecs[i].mName.string();
        uint32_t quirks = matchingCodecs[i].mQuirks;
        const char *componentName = componentNameBase;

        AString tmp;
        if (flags & kUseSecureInputBuffers) {
            tmp = componentNameBase;
            tmp.append(".secure");

            componentName = tmp.c_str();
        }

        if (createEncoder) {
            sp<MediaSource> softwareCodec =
                InstantiateSoftwareEncoder(componentName, source, meta);

            if (softwareCodec != NULL) {
                ALOGV("Successfully allocated software codec '%s'", componentName);

                return softwareCodec;
            }
        }

        ALOGV("Attempting to allocate OMX node '%s'", componentName);

        if (!createEncoder
                && (quirks & kOutputBuffersAreUnreadable)
                && (flags & kClientNeedsFramebuffer)) {
            if (strncmp(componentName, "OMX.SEC.", 8)) {
                // For OMX.SEC.* decoders we can enable a special mode that
                // gives the client access to the framebuffer contents.

                ALOGW("Component '%s' does not give the client access to "
                     "the framebuffer contents. Skipping.",
                     componentName);

                continue;
            }
        }

        status_t err = omx->allocateNode(componentName, observer, &node);
        if (err == OK) {
            ALOGV("Successfully allocated OMX node '%s'", componentName);

            sp<OMXCodec> codec = new OMXCodec(
                    omx, node, quirks, flags,
                    createEncoder, mime, componentName,
                    source, nativeWindow);

#ifdef MTK_AOSP_ENHANCEMENT  // Morris Yang for Camera recording
            int32_t prCamMode;
            void *prCamMemInfo;   //CamMemInfo_t pointer
            int32_t prIsCamWhiteboardEffect = 0;
            CamMCIMemInfo_t *camMCIMemInfo = NULL;

            if (meta->findInt32(kKeyCamMemMode, &prCamMode)) {
                ALOGD("Camera Recording Mode (%d)", prCamMode);
            }

            if (meta->findInt32(kKeyCamWhiteboardEffect, &prIsCamWhiteboardEffect)) {
                ALOGD("Camera Recording IsWhiteBoardEffect (%d)", prIsCamWhiteboardEffect);
                codec->getCameraMeta()->setInt32(kKeyCamWhiteboardEffect, prIsCamWhiteboardEffect);
            }

            if (meta->findPointer(kKeyCamMCIMemInfo, (void **)&camMCIMemInfo)) {
                ALOGD("Camera Recording SetMCIMode (security = %d, coherent = %d)", camMCIMemInfo->u4Security, camMCIMemInfo->u4Coherent);
                codec->getCameraMeta()->setPointer(kKeyCamMCIMemInfo, camMCIMemInfo);
            }

            if (prCamMode == CAMERA_DISCONTINUOUS_MEM_ION_MODE) {
                if (meta->findPointer(kKeyCamMemInfo, (void **)&prCamMemInfo)) {
                    ALOGD("Hello camera recording MEM ION MODE!!!");
                    for (uint32_t u4I = 0; u4I < ((CamMemIonInfo_t *)prCamMemInfo)->u4VdoBufCount; u4I++) {
                        ALOGD("VdoBufVA = 0x%08x, IonFd = %d, VdoBufCount = %d, VdoBufSize = %d,\n",
                              (unsigned int)((CamMemIonInfo_t *)prCamMemInfo)->u4VdoBufVA[u4I],
                              ((CamMemIonInfo_t *)prCamMemInfo)->IonFd[u4I],
                              ((CamMemIonInfo_t *)prCamMemInfo)->u4VdoBufCount,
                              ((CamMemIonInfo_t *)prCamMemInfo)->u4VdoBufSize
                             );
                    }
                    codec->getCameraMeta()->setInt32(kKeyCamMemMode, prCamMode);
                    codec->getCameraMeta()->setPointer(kKeyCamMemInfo, prCamMemInfo);
                }
            }
            else {    // CAMERA_DISCONTINUOUS_MEM_VA_MODE
                if (meta->findPointer(kKeyCamMemInfo, (void **)&prCamMemInfo)) {
                    ALOGD("Hello camera recording MEM VA MODE!!!");
                    for (uint32_t u4I = 0; u4I < ((CamMemInfo_t *)prCamMemInfo)->u4VdoBufCount; u4I++) {
                        ALOGD("VdoBufVA = 0x%08x, VdoBufCount = %d, VdoBufSize = %d,\n",
                              (unsigned int)((CamMemInfo_t *)prCamMemInfo)->u4VdoBufVA[u4I],
                              ((CamMemInfo_t *)prCamMemInfo)->u4VdoBufCount,
                              ((CamMemInfo_t *)prCamMemInfo)->u4VdoBufSize
                             );
                    }
                    codec->getCameraMeta()->setInt32(kKeyCamMemMode, prCamMode);
                    codec->getCameraMeta()->setPointer(kKeyCamMemInfo, prCamMemInfo);
                }
            }
#endif //MTK_AOSP_ENHANCEMENT

            observer->setCodec(codec);

            err = codec->configureCodec(meta);
            if (err == OK) {
                return codec;
            }

            ALOGV("Failed to configure codec '%s'", componentName);
        }
    }

    return NULL;
}

status_t OMXCodec::parseHEVCCodecSpecificData(
        const void *data, size_t size,
        unsigned *profile, unsigned *level) {
    const uint8_t *ptr = (const uint8_t *)data;

    // verify minimum size and configurationVersion == 1.
    if (size < 7 || ptr[0] != 1) {
        return ERROR_MALFORMED;
    }

    *profile = (ptr[1] & 31);
    *level = ptr[12];

    ptr += 22;
    size -= 22;

    size_t numofArrays = (char)ptr[0];
    ptr += 1;
    size -= 1;
    size_t j = 0, i = 0;
    for (i = 0; i < numofArrays; i++) {
        ptr += 1;
        size -= 1;

        // Num of nals
        size_t numofNals = U16_AT(ptr);
        ptr += 2;
        size -= 2;

        for (j = 0;j < numofNals;j++) {
            if (size < 2) {
                return ERROR_MALFORMED;
            }

            size_t length = U16_AT(ptr);

            ptr += 2;
            size -= 2;

            if (size < length) {
                return ERROR_MALFORMED;
            }
            addCodecSpecificData(ptr, length);

            ptr += length;
            size -= length;
        }
    }
    return OK;
}

status_t OMXCodec::parseAVCCodecSpecificData(
        const void *data, size_t size,
        unsigned *profile, unsigned *level) {
    const uint8_t *ptr = (const uint8_t *)data;

    // verify minimum size and configurationVersion == 1.
    if (size < 7 || ptr[0] != 1) {
        return ERROR_MALFORMED;
    }

    *profile = ptr[1];
    *level = ptr[3];

    // There is decodable content out there that fails the following
    // assertion, let's be lenient for now...
    // CHECK((ptr[4] >> 2) == 0x3f);  // reserved

    size_t lengthSize __unused = 1 + (ptr[4] & 3);

    // commented out check below as H264_QVGA_500_NO_AUDIO.3gp
    // violates it...
    // CHECK((ptr[5] >> 5) == 7);  // reserved

    size_t numSeqParameterSets = ptr[5] & 31;

    ptr += 6;
    size -= 6;

    for (size_t i = 0; i < numSeqParameterSets; ++i) {
        if (size < 2) {
            return ERROR_MALFORMED;
        }

        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        if (size < length) {
            return ERROR_MALFORMED;
        }

        addCodecSpecificData(ptr, length);

        ptr += length;
        size -= length;
    }

    if (size < 1) {
        return ERROR_MALFORMED;
    }

    size_t numPictureParameterSets = *ptr;
    ++ptr;
    --size;

    for (size_t i = 0; i < numPictureParameterSets; ++i) {
        if (size < 2) {
            return ERROR_MALFORMED;
        }

        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        if (size < length) {
            return ERROR_MALFORMED;
        }

        addCodecSpecificData(ptr, length);

        ptr += length;
        size -= length;
    }

    return OK;
}

status_t OMXCodec::configureCodec(const sp<MetaData> &meta) {
#ifdef MTK_AOSP_ENHANCEMENT
    void *pTs = NULL;
    CHECK(meta != NULL);
    if (meta->findPointer(kkeyOmxTimeSource, &pTs) == true)
    {
        CHECK(pTs != NULL);
        mOMX->setParameter(
            mNode, OMX_IndexVendorMtkOmxVdecTimeSource, pTs, sizeof(void *));
    }

#ifdef MTK_SLOW_MOTION_VIDEO_SUPPORT
    int32_t mtk_slowmotion_speed = 0;
    if (meta->findInt32(kKeySlowMotionSpeedValue, &mtk_slowmotion_speed) == true)
    {
        mIsSlowMotion = true;
    }
    else
    {
        mIsSlowMotion = false;
    }
#endif

#endif //MTK_AOSP_ENHANCEMENT

    ALOGV("configureCodec protected=%d",
         (mFlags & kEnableGrallocUsageProtected) ? 1 : 0);

    if (!(mFlags & kIgnoreCodecSpecificData)) {
        uint32_t type;
        const void *data;
        size_t size;
#ifdef MTK_AOSP_ENHANCEMENT//check VOS first, than esds
        if (meta->findData(kKeyMPEG4VOS, &type, &data, &size)) {  //MPEG4 raw codec info
            addCodecSpecificData(data, size);
        }
        else
#endif
        if (meta->findData(kKeyESDS, &type, &data, &size)) {
            ESDS esds((const char *)data, size);
#ifdef MTK_AOSP_ENHANCEMENT
            {
                status_t err = esds.InitCheck();
                if (err != OK) {
                    ALOGE("esds.InitCheck() ERROR %d!!!", err);
                    return err;
                }
            }
#else
            CHECK_EQ(esds.InitCheck(), (status_t)OK);
#endif //MTK_AOSP_ENHANCEMENT

            const void *codec_specific_data;
            size_t codec_specific_data_size;
            esds.getCodecSpecificInfo(
                    &codec_specific_data, &codec_specific_data_size);

            addCodecSpecificData(
                    codec_specific_data, codec_specific_data_size);
        } else if (meta->findData(kKeyAVCC, &type, &data, &size)) {
            // Parse the AVCDecoderConfigurationRecord

            unsigned profile, level;
            status_t err;
            if ((err = parseAVCCodecSpecificData(
                            data, size, &profile, &level)) != OK) {
                ALOGE("Malformed AVC codec specific data.");
                return err;
            }

            CODEC_LOGI(
                    "AVC profile = %u (%s), level = %u",
                    profile, AVCProfileToString(profile), level);
#ifdef MTK_AOSP_ENHANCEMENT
        }
        else if (meta->findData(kKeyCodecConfigInfo, &type, &data, &size))
        {
            ALOGI("OMXCodec::configureCodec--config Codec Info for AAC");
            addCodecSpecificData(data, size);
#endif //MTK_AOSP_ENHANCEMENT
        }
        else if (meta->findData(kKeyHVCC, &type, &data, &size)) {
            // Parse the HEVCDecoderConfigurationRecord

            unsigned profile, level;
            status_t err;
            if ((err = parseHEVCCodecSpecificData(
                            data, size, &profile, &level)) != OK) {
                ALOGE("Malformed HEVC codec specific data.");
                return err;
            }

            CODEC_LOGI(
                    "HEVC profile = %u , level = %u",
                    profile, level);
        } else if (meta->findData(kKeyVorbisInfo, &type, &data, &size)) {
            addCodecSpecificData(data, size);

            CHECK(meta->findData(kKeyVorbisBooks, &type, &data, &size));
#ifdef MTK_AOSP_ENHANCEMENT
        status_t sizeValid = vorbisSizeValid(size);
        if(sizeValid != OK){
        return sizeValid;
            }
#endif //MTK_AOSP_ENHANCEMENT
            addCodecSpecificData(data, size);
        }
        else if (meta->findData(kKeyOpusHeader, &type, &data, &size)) {
            addCodecSpecificData(data, size);

            CHECK(meta->findData(kKeyOpusCodecDelay, &type, &data, &size));
            addCodecSpecificData(data, size);
            CHECK(meta->findData(kKeyOpusSeekPreRoll, &type, &data, &size));
            addCodecSpecificData(data, size);
        }
    }

    int32_t bitRate = 0;
    if (mIsEncoder) {
        CHECK(meta->findInt32(kKeyBitRate, &bitRate));
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CAM_STEREO_CAMERA_SUPPORT
        OMX_INDEXTYPE index;
        status_t err2;
        int32_t Stereo3DMode;
        if (meta->findInt32(kKeyVideoStereoMode, &Stereo3DMode))
        {
            switch (Stereo3DMode)
            {
                case VIDEO_STEREO_2D: // 2D
                    break; // do nothing
                case VIDEO_STEREO_SIDE_BY_SIDE: // side by side
                case VIDEO_STEREO_TOP_BOTTOM: // top and bottom
                    err2 = mOMX->getExtensionIndex(mNode, "OMX.MTK.index.param.video.3DVideoEncode", &index);
                    CHECK_EQ((int)err2, (int)OK);
                    err2 = mOMX->setParameter(mNode, index, &Stereo3DMode, sizeof(Stereo3DMode));
                    CHECK_EQ((int)err2, (int)OK);
                    break;
                case VIDEO_STEREO_FRAME_SEQUENCE: // frame sequence
                default:
                    CHECK_EQ(Stereo3DMode, VIDEO_STEREO_DEFAULT); // should be 0 for default
                    break;
            }
        }
#endif
#endif //MTK_AOSP_ENHANCEMENT
    }
    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mMIME)) {
#ifdef MTK_AOSP_ENHANCEMENT
        status_t errSetFormat  =  setAMRFormat(false /* isWAMR */, bitRate);
        if (errSetFormat != OK)
        {
            return errSetFormat;
        }
#else
        setAMRFormat(false /* isWAMR */, bitRate);
#endif //MTK_AOSP_ENHANCEMENT
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mMIME)) {
#ifdef MTK_AOSP_ENHANCEMENT
        status_t errSetFormat  =  setAMRFormat(true /* isWAMR */, bitRate);
        if (errSetFormat != OK)
        {
            return errSetFormat;
        }
#else
        setAMRFormat(true /* isWAMR */, bitRate);
#endif //MTK_AOSP_ENHANCEMENT
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mMIME)) {
        int32_t numChannels, sampleRate, aacProfile;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

        if (!meta->findInt32(kKeyAACProfile, &aacProfile)) {
            aacProfile = OMX_AUDIO_AACObjectNull;
        }

        int32_t isADTS;
        if (!meta->findInt32(kKeyIsADTS, &isADTS)) {
            isADTS = false;
        }

#ifdef MTK_AOSP_ENHANCEMENT
    status_t setupAAC_err = setupAACFormat(numChannels,sampleRate,bitRate,aacProfile,isADTS,meta);
    if(OK != setupAAC_err) {
       return setupAAC_err;
    }
#else
        status_t err = setAACFormat(numChannels, sampleRate, bitRate, aacProfile, isADTS);
        if (err != OK) {
            CODEC_LOGE("setAACFormat() failed (err = %d)", err);
            return err;
        }
#endif //#ifdef MTK_AOSP_ENHANCEMENT
    }
#ifndef MTK_AOSP_ENHANCEMENT
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG, mMIME)) {
        int32_t numChannels, sampleRate;
        if (meta->findInt32(kKeyChannelCount, &numChannels)
                && meta->findInt32(kKeySampleRate, &sampleRate)) {
            // Since we did not always check for these, leave them optional
            // and have the decoder figure it all out.
            setRawAudioFormat(
                    mIsEncoder ? kPortIndexInput : kPortIndexOutput,
                    sampleRate,
                    numChannels);
        }
    }
#endif
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mMIME)) {
        int32_t numChannels;
        int32_t sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

        status_t err = setAC3Format(numChannels, sampleRate);
        if (err != OK) {
            CODEC_LOGE("setAC3Format() failed (err = %d)", err);
            return err;
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_ALAW, mMIME)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_MLAW, mMIME)) {
        // These are PCM-like formats with a fixed sample rate but
        // a variable number of channels.

        int32_t sampleRate;
        int32_t numChannels;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        if (!meta->findInt32(kKeySampleRate, &sampleRate)) {
            sampleRate = 8000;
        }

        setG711Format(sampleRate, numChannels);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RAW, mMIME)) {

        CHECK(!mIsEncoder);

        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

#ifdef MTK_AOSP_ENHANCEMENT
        status_t setupG711_err = setupG711Format(numChannels,meta);
        if(OK != setupG711_err) {
           return setupG711_err;
        }
#else
        setG711Format(sampleRate, numChannels);
#endif//#ifdef MTK_AOSP_ENHANCEMENT
    }
#ifdef MTK_AOSP_ENHANCEMENT
#if defined(MTK_AUDIO_ADPCM_SUPPORT) || defined(HAVE_ADPCMENCODE_FEATURE)
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MS_ADPCM, mMIME) || !strcasecmp(MEDIA_MIMETYPE_AUDIO_DVI_IMA_ADPCM, mMIME))
    {
        status_t setupADPCM_err = setupADPCMFormat(meta);
        if(OK != setupADPCM_err) {
           return setupADPCM_err;
        }
    }
#endif
#endif
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RAW, mMIME))
    {
        CHECK(!mIsEncoder);

        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_RAW_SUPPORT
        status_t setupRaw_err = setupRawFormat(numChannels,sampleRate,meta);
        if(OK != setupRaw_err){
            return setupRaw_err;
        }
#else
        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
#endif
#else
        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
#endif
    }
#ifdef MTK_AOSP_ENHANCEMENT
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG, mMIME))
    {
        status_t setupMp3_err = setupMp3Format(meta);
        if(OK != setupMp3_err) {
            return setupMp3_err;
        }
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FLAC, mMIME))
    {

        status_t setupFLAC_err = setupFLACFormat(meta);
        if(OK != setupFLAC_err) {
            return setupFLAC_err;
        }
    }
#ifdef MTK_AUDIO_APE_SUPPORT
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_APE, mMIME))
    {
        status_t setupAPE_err = setupAPEFormat(meta);
        if(OK != setupAPE_err) {
            return setupAPE_err;
        }
    }
#endif
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mMIME) && mIsEncoder)
    {
        setVORBISFormat(meta);
    }
#ifdef MTK_AUDIO_ALAC_SUPPORT
    else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_ALAC, mMIME))
    {
        setupALACFormat(meta);
    }
#endif

#endif //MTK_AOSP_ENHANCEMENT
#ifdef MTK_AOSP_ENHANCEMENT
    if ((mFlags & kClientNeedsFramebuffer) && (mFlags & kEnableThumbnailOptimzation))
    {
        mPropFlags |= OMXCODEC_THUMBNAIL_MODE;
        mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVdecThumbnailMode, &mPropFlags, sizeof(void *));
    }

#ifdef MTK_CLEARMOTION_SUPPORT
    if (mFlags & kUseClearMotion)
    {
        mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVdecUseClearMotion, &mPropFlags, sizeof(void *));
    }
#endif
#ifdef MTK_CMMB_ENABLE
    int32_t IsCMMBtempflg;
    if (meta->findInt32(kKeyIsCmmb, &IsCMMBtempflg))
    {
        if (0 == IsCMMBtempflg)
        {
            IsCMMBFlag = false;
        }
        else
        {
            IsCMMBFlag = true;
        }
    }
    else
    {
        IsCMMBFlag = false;
    }
    ALOGE("Omxcodec configureCodec IsCMMBFlag = %d", IsCMMBFlag);
#endif
#endif

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_WMV_PLAYBACK_SUPPORT
    // Morris Yang add for ASF
    if ((!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mMIME)))
    {
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyWMAC, &type, &data, &size))
        {
            ALOGD("addCodecSpecificData for WMA");
            const uint8_t *ptr = (const uint8_t *)data;
            addCodecSpecificData(ptr, size);
        }
    }
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mMIME))
    {
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyWMVC, &type, &data, &size))
        {
            ALOGD("addCodecSpecificData for WMV");
            const uint8_t *ptr = (const uint8_t *)data;
            addCodecSpecificData(ptr, size);
        }
    }
#endif
#ifdef MTK_SWIP_WMAPRO
    if ((!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMAPRO, mMIME)))
    {
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyWMAPROC, &type, &data, &size))
        {
            ALOGD("addCodecSpecificData for WMAPRO");
            const uint8_t *ptr = (const uint8_t *)data;
            addCodecSpecificData(ptr, size);
        }
    }
#endif
#endif

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_ALAC_SUPPORT
    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_ALAC, mMIME))
    {
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyALACC, &type, &data, &size))
        {
            ALOGD("addCodecSpecificData for ALAC");
            const uint8_t *ptr = (const uint8_t *)data;
            addCodecSpecificData(ptr, size);
        }
    }
#endif
#endif

    if (!strncasecmp(mMIME, "video/", 6)) {

        if (mIsEncoder) {
            setVideoInputFormat(mMIME, meta);
        } else {
#ifdef MTK_AOSP_ENHANCEMENT
#if 0//def  MTK_S3D_SUPPORT
            OMX_INDEXTYPE index;
            status_t err2;
            int32_t Stereo3DMode;
            if (meta->findInt32(kKeyVideoStereoMode, &Stereo3DMode))
            {
                switch (Stereo3DMode)
                {
                    case VIDEO_STEREO_2D: // 2D
                        break; // do nothing
                    case VIDEO_STEREO_SIDE_BY_SIDE: // side by side
                    case VIDEO_STEREO_TOP_BOTTOM: // top and bottom
                        err2 = mOMX->getExtensionIndex(mNode, "OMX.MTK.index.param.video.3DVideoPlayback", &index);
                        CHECK_EQ((int)err2, (int)OK);
                        err2 = mOMX->setParameter(mNode, index, &Stereo3DMode, sizeof(Stereo3DMode));
                        CHECK_EQ((int)err2, (int)OK);
                        break;
                    case VIDEO_STEREO_FRAME_SEQUENCE: // frame sequence
                    default:
                        CHECK_EQ(Stereo3DMode, VIDEO_STEREO_DEFAULT); // should be 0 for default
                        break;
                }
            }
#endif
#endif //MTK_AOSP_ENHANCEMENT
            status_t err = setVideoOutputFormat(
                    mMIME, meta);

            if (err != OK) {
                return err;
            }
        }
    }

    int32_t maxInputSize;
    if (meta->findInt32(kKeyMaxInputSize, &maxInputSize)) {
        setMinBufferSize(kPortIndexInput, (OMX_U32)maxInputSize);
    }
#ifdef MTK_AOSP_ENHANCEMENT
    {
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;
        status_t err = mOMX->getParameter(
                           mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            ALOGE("%s , line %d,  return %x", __FUNCTION__, __LINE__, err );
            return err;
        }

        if (def.eDomain == OMX_PortDomainVideo) {
            OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;
            if (video_def->nFrameHeight == 0 || video_def->nFrameWidth == 0) {
                ALOGE("%s nFrameHeight = %d  nFrameWidth = %d return bad value", __FUNCTION__, video_def->nFrameHeight, video_def->nFrameWidth);
                return BAD_VALUE;
            }
        }
    }
#endif

#ifdef MTK_AOSP_ENHANCEMENT
    if (OK != initOutputFormat(meta)){
        setState(ERROR);
    }
#else
    initOutputFormat(meta);
#endif

#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_SLOW_MOTION_VIDEO_SUPPORT)
    if (!strncasecmp(mMIME, "video/", 6) && mIsEncoder) {
        OMX_VIDEO_NONREFP   nonRefP;
        InitOMXParams(&nonRefP);
        //query non-ref P frequency
        //int frequency = ((3<<16)|(4));
        status_t err = mOMX->getParameter(
                           mNode, OMX_IndexVendorMtkOmxVencNonRefPOp, &nonRefP, sizeof(nonRefP));
        mOutputFormat->setInt32(kKeyNonRefPFreq, nonRefP.nFreq);
        ALOGD("set frequency %d", nonRefP.nFreq);

        if (err != OK) {
            ALOGE("%s , line %d,  return %x", __FUNCTION__, __LINE__, err );
            return err;
        }

    }
#endif//not MTK_AOSP_ENHANCEMENT && MTK_SLOW_MOTION_VIDEO_SUPPORT

    if ((mFlags & kClientNeedsFramebuffer)
            && !strncmp(mComponentName, "OMX.SEC.", 8)) {
        // This appears to no longer be needed???

        OMX_INDEXTYPE index;

        status_t err =
            mOMX->getExtensionIndex(
                    mNode,
                    "OMX.SEC.index.ThumbnailMode",
                    &index);

        if (err != OK) {
            return err;
        }

        OMX_BOOL enable = OMX_TRUE;
        err = mOMX->setConfig(mNode, index, &enable, sizeof(enable));

        if (err != OK) {
            CODEC_LOGE("setConfig('OMX.SEC.index.ThumbnailMode') "
                       "returned error 0x%08x", err);

            return err;
        }

        mQuirks &= ~kOutputBuffersAreUnreadable;
    }

    if (mNativeWindow != NULL
        && !mIsEncoder
        && !strncasecmp(mMIME, "video/", 6)
        && !strncmp(mComponentName, "OMX.", 4)) {
        status_t err = initNativeWindow();
        if (err != OK) {
            return err;
        }
    }
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CMMB_ENABLE
    int32_t CMMBEnableFlag, ConcealLevel;
    if (meta->findInt32(kKeyIsCmmb, &CMMBEnableFlag))
    {
        if (1 == CMMBEnableFlag)
        {
            ConcealLevel = 1;
            mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVdecConcealmentLevel, &ConcealLevel, sizeof(void *));
        }
    }
#endif
#endif

    return OK;
}

void OMXCodec::setMinBufferSize(OMX_U32 portIndex, OMX_U32 size) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    if ((portIndex == kPortIndexInput && (mQuirks & kInputBufferSizesAreBogus))
        || (def.nBufferSize < size)) {
        def.nBufferSize = size;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

#ifdef MTK_AOSP_ENHANCEMENT
    if ((!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.RAW")) || (!strncmp(mComponentName, "OMX.MTK.AUDIO.DECODER.ADPCM", 27))) {
        SLOGI("Raw and ADPCM component Do not change input buffer size !");
    }
    else {
#endif
    // Make sure the setting actually stuck.
    if (portIndex == kPortIndexInput
            && (mQuirks & kInputBufferSizesAreBogus)) {
        CHECK_EQ(def.nBufferSize, size);
    } else {
        CHECK(def.nBufferSize >= size);
    }
#ifdef MTK_AOSP_ENHANCEMENT
    }
#endif
}

status_t OMXCodec::setVideoPortFormatType(
        OMX_U32 portIndex,
        OMX_VIDEO_CODINGTYPE compressionFormat,
        OMX_COLOR_FORMATTYPE colorFormat) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = portIndex;
    format.nIndex = 0;
    bool found = false;

    OMX_U32 index = 0;
    for (;;) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        // The following assertion is violated by TI's video decoder.
        // CHECK_EQ(format.nIndex, index);

#if 1
        CODEC_LOGV("portIndex: %u, index: %u, eCompressionFormat=%d eColorFormat=%d",
             portIndex,
             index, format.eCompressionFormat, format.eColorFormat);
#endif

        if (format.eCompressionFormat == compressionFormat
                && format.eColorFormat == colorFormat) {
            found = true;
            break;
        }

        ++index;
        if (index >= kMaxColorFormatSupported) {
            CODEC_LOGE("color format %d or compression format %d is not supported",
                colorFormat, compressionFormat);
            return UNKNOWN_ERROR;
        }
    }

    if (!found) {
        return UNKNOWN_ERROR;
    }

    CODEC_LOGV("found a match.");
    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));

    return err;
}

static size_t getFrameSize(
        OMX_COLOR_FORMATTYPE colorFormat, int32_t width, int32_t height) {
    switch (colorFormat) {
        case OMX_COLOR_FormatYCbYCr:
        case OMX_COLOR_FormatCbYCrY:
            return width * height * 2;

        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
        case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
        /*
        * FIXME: For the Opaque color format, the frame size does not
        * need to be (w*h*3)/2. It just needs to
        * be larger than certain minimum buffer size. However,
        * currently, this opaque foramt has been tested only on
        * YUV420 formats. If that is changed, then we need to revisit
        * this part in the future
        */
        case OMX_COLOR_FormatAndroidOpaque:
#ifdef MTK_AOSP_ENHANCEMENT
            /*
             * FIXME: We use this FrameSize for temp solution
             * in order to check functionality,
             * and we need to get FrameSize accurately in the future
             */
        case OMX_MTK_COLOR_FormatYV12:
        case OMX_COLOR_FormatVendorMTKYUV:
        case OMX_COLOR_FormatVendorMTKYUV_FCM:
#endif
            return (width * height * 3) / 2;

        default:
            CHECK(!"Should not be here. Unsupported color format.");
            break;
    }
    return 0;
}

status_t OMXCodec::findTargetColorFormat(
        const sp<MetaData>& meta, OMX_COLOR_FORMATTYPE *colorFormat) {
    ALOGV("findTargetColorFormat");
    CHECK(mIsEncoder);

    *colorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    int32_t targetColorFormat;
    if (meta->findInt32(kKeyColorFormat, &targetColorFormat)) {
        *colorFormat = (OMX_COLOR_FORMATTYPE) targetColorFormat;
    }

    // Check whether the target color format is supported.
    return isColorFormatSupported(*colorFormat, kPortIndexInput);
}

status_t OMXCodec::isColorFormatSupported(
        OMX_COLOR_FORMATTYPE colorFormat, int portIndex) {
    ALOGV("isColorFormatSupported: %d", static_cast<int>(colorFormat));

    // Enumerate all the color formats supported by
    // the omx component to see whether the given
    // color format is supported.
    OMX_VIDEO_PARAM_PORTFORMATTYPE portFormat;
    InitOMXParams(&portFormat);
    portFormat.nPortIndex = portIndex;
    OMX_U32 index = 0;
    portFormat.nIndex = index;
    while (true) {
        if (OMX_ErrorNone != mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &portFormat, sizeof(portFormat))) {
            break;
        }
        // Make sure that omx component does not overwrite
        // the incremented index (bug 2897413).
        CHECK_EQ(index, portFormat.nIndex);
        if (portFormat.eColorFormat == colorFormat) {
            CODEC_LOGV("Found supported color format: %d", portFormat.eColorFormat);
            return OK;  // colorFormat is supported!
        }
        ++index;
        portFormat.nIndex = index;

        if (index >= kMaxColorFormatSupported) {
            CODEC_LOGE("More than %u color formats are supported???", index);
            break;
        }
    }

    CODEC_LOGE("color format %d is not supported", colorFormat);
    return UNKNOWN_ERROR;
}

void OMXCodec::setVideoInputFormat(
        const char *mime, const sp<MetaData>& meta) {

    int32_t width, height, frameRate, bitRate, stride, sliceHeight;
    bool success = meta->findInt32(kKeyWidth, &width);
    success = success && meta->findInt32(kKeyHeight, &height);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyStride, &stride);
    success = success && meta->findInt32(kKeySliceHeight, &sliceHeight);
    CHECK(success);
    CHECK(stride != 0);

    OMX_VIDEO_CODINGTYPE compressionFormat = OMX_VIDEO_CodingUnused;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingAVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingHEVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG4;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
        compressionFormat = OMX_VIDEO_CodingH263;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VPX, mime)) {
        compressionFormat = OMX_VIDEO_CodingVP8;
    }
#endif //MTK_AOSP_ENHANCEMENT
    else {
        ALOGE("Not a supported video mime type: %s", mime);
        CHECK(!"Should not be here. Not a supported video mime type.");
    }

    OMX_COLOR_FORMATTYPE colorFormat;
    CHECK_EQ((status_t)OK, findTargetColorFormat(meta, &colorFormat));

    status_t err;
    OMX_PARAM_PORTDEFINITIONTYPE def;
    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    //////////////////////// Input port /////////////////////////
    CHECK_EQ(setVideoPortFormatType(
            kPortIndexInput, OMX_VIDEO_CodingUnused,
            colorFormat), (status_t)OK);

    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    def.nBufferSize = getFrameSize(colorFormat,
            stride > 0? stride: -stride, sliceHeight);

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->nStride = stride;
    video_def->nSliceHeight = sliceHeight;
    video_def->xFramerate = (frameRate << 16);  // Q16 format
    video_def->eCompressionFormat = OMX_VIDEO_CodingUnused;
    video_def->eColorFormat = colorFormat;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    //////////////////////// Output port /////////////////////////
    CHECK_EQ(setVideoPortFormatType(
            kPortIndexOutput, compressionFormat, OMX_COLOR_FormatUnused),
            (status_t)OK);
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);
    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->xFramerate = 0;      // No need for output port
    video_def->nBitrate = bitRate;  // Q16 format
    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;
    if (mQuirks & kRequiresLargerEncoderOutputBuffer) {
        // Increases the output buffer size
        def.nBufferSize = ((def.nBufferSize * 3) >> 1);
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    /////////////////// Codec-specific ////////////////////////
    switch (compressionFormat) {
        case OMX_VIDEO_CodingMPEG4:
        {
            CHECK_EQ(setupMPEG4EncoderParameters(meta), (status_t)OK);
            break;
        }

        case OMX_VIDEO_CodingH263:
            CHECK_EQ(setupH263EncoderParameters(meta), (status_t)OK);
            break;

        case OMX_VIDEO_CodingAVC:
        {
            CHECK_EQ(setupAVCEncoderParameters(meta), (status_t)OK);
            break;
        }
#ifdef MTK_AOSP_ENHANCEMENT
        case OMX_VIDEO_CodingVP8:
        {
            CHECK_EQ(setupVP8EncoderParameters(meta), (status_t)OK);
            break;
        }
#endif //MTK_AOSP_ENHANCEMENT
        default:
            CHECK(!"Support for this compressionFormat to be implemented.");
            break;
    }
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_SLOW_MOTION_VIDEO_SUPPORT)
    OMX_VIDEO_NONREFP   nonRefP;
    InitOMXParams(&nonRefP);
    //check if enable non-ref P
    if (compressionFormat == OMX_VIDEO_CodingAVC) {
        int enable = false;
        bool success = meta->findInt32(kKeyEnableNonRefP, &enable);
        if (success) {
            if (enable) {
                //enable
                nonRefP.bEnable = OMX_TRUE;
                err = mOMX->setParameter(
                          mNode, OMX_IndexVendorMtkOmxVencNonRefPOp, &nonRefP, sizeof(nonRefP));
            }
        }
    }
    //we will set mOutputFormat for non-ref p frequency in configureCodec()
    //after mOutputFormat initialized.
#endif//not MTK_AOSP_ENHANCEMENT && MTK_SLOW_MOTION_VIDEO_SUPPORT
}

static OMX_U32 setPFramesSpacing(int32_t iFramesInterval, int32_t frameRate) {
    if (iFramesInterval < 0) {
        return 0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        return 0;
    }
    OMX_U32 ret = frameRate * iFramesInterval - 1;
    return ret;
}

status_t OMXCodec::setupErrorCorrectionParameters() {
    OMX_VIDEO_PARAM_ERRORCORRECTIONTYPE errorCorrectionType;
    InitOMXParams(&errorCorrectionType);
    errorCorrectionType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
    if (err != OK) {
        ALOGW("Error correction param query is not supported");
        return OK;  // Optional feature. Ignore this failure
    }

    errorCorrectionType.bEnableHEC = OMX_FALSE;
    errorCorrectionType.bEnableResync = OMX_TRUE;
    errorCorrectionType.nResynchMarkerSpacing = 256;
    errorCorrectionType.bEnableDataPartitioning = OMX_FALSE;
    errorCorrectionType.bEnableRVLC = OMX_FALSE;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
    if (err != OK) {
        ALOGW("Error correction param configuration is not supported");
    }

    // Optional feature. Ignore the failure.
    return OK;
}

status_t OMXCodec::setupBitRate(int32_t bitRate) {
    OMX_VIDEO_PARAM_BITRATETYPE bitrateType;
    InitOMXParams(&bitrateType);
    bitrateType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
    CHECK_EQ(err, (status_t)OK);

    bitrateType.eControlRate = OMX_Video_ControlRateVariable;
    bitrateType.nTargetBitrate = bitRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
    CHECK_EQ(err, (status_t)OK);
    return OK;
}

status_t OMXCodec::getVideoProfileLevel(
        const sp<MetaData>& meta,
        const CodecProfileLevel& defaultProfileLevel,
        CodecProfileLevel &profileLevel) {
    CODEC_LOGV("Default profile: %u, level #x%x",
            defaultProfileLevel.mProfile, defaultProfileLevel.mLevel);

    // Are the default profile and level overwriten?
    int32_t profile, level;
    if (!meta->findInt32(kKeyVideoProfile, &profile)) {
        profile = defaultProfileLevel.mProfile;
    }
    if (!meta->findInt32(kKeyVideoLevel, &level)) {
        level = defaultProfileLevel.mLevel;
    }
    CODEC_LOGV("Target profile: %d, level: %d", profile, level);

    // Are the target profile and level supported by the encoder?
    OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
    InitOMXParams(&param);
    param.nPortIndex = kPortIndexOutput;
    for (param.nProfileIndex = 0;; ++param.nProfileIndex) {
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoProfileLevelQuerySupported,
                &param, sizeof(param));

        if (err != OK) break;

        int32_t supportedProfile = static_cast<int32_t>(param.eProfile);
        int32_t supportedLevel = static_cast<int32_t>(param.eLevel);
        CODEC_LOGV("Supported profile: %d, level %d",
            supportedProfile, supportedLevel);

        if (profile == supportedProfile &&
            level <= supportedLevel) {
            // We can further check whether the level is a valid
            // value; but we will leave that to the omx encoder component
            // via OMX_SetParameter call.
            profileLevel.mProfile = profile;
            profileLevel.mLevel = level;
            return OK;
        }
    }

    CODEC_LOGE("Target profile (%d) and level (%d) is not supported",
            profile, level);
    return BAD_VALUE;
}

status_t OMXCodec::setupH263EncoderParameters(const sp<MetaData>& meta) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);
    OMX_VIDEO_PARAM_H263TYPE h263type;
    InitOMXParams(&h263type);
    h263type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));
    CHECK_EQ(err, (status_t)OK);

    h263type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    h263type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
    if (h263type.nPFrames == 0) {
        h263type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    h263type.nBFrames = 0;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = h263type.eProfile;
    defaultProfileLevel.mLevel = h263type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    h263type.eProfile = static_cast<OMX_VIDEO_H263PROFILETYPE>(profileLevel.mProfile);
    h263type.eLevel = static_cast<OMX_VIDEO_H263LEVELTYPE>(profileLevel.mLevel);

    h263type.bPLUSPTYPEAllowed = OMX_FALSE;
    h263type.bForceRoundingTypeToZero = OMX_FALSE;
    h263type.nPictureHeaderRepetition = 0;
    h263type.nGOBHeaderInterval = 0;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);
    CHECK_EQ(setupErrorCorrectionParameters(), (status_t)OK);

    return OK;
}

status_t OMXCodec::setupMPEG4EncoderParameters(const sp<MetaData>& meta) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);
    OMX_VIDEO_PARAM_MPEG4TYPE mpeg4type;
    InitOMXParams(&mpeg4type);
    mpeg4type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));
    CHECK_EQ(err, (status_t)OK);

    mpeg4type.nSliceHeaderSpacing = 0;
    mpeg4type.bSVH = OMX_FALSE;
    mpeg4type.bGov = OMX_FALSE;

    mpeg4type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    mpeg4type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
    if (mpeg4type.nPFrames == 0) {
        mpeg4type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    mpeg4type.nBFrames = 0;
    mpeg4type.nIDCVLCThreshold = 0;
    mpeg4type.bACPred = OMX_TRUE;
    mpeg4type.nMaxPacketSize = 256;
    mpeg4type.nTimeIncRes = 1000;
    mpeg4type.nHeaderExtension = 0;
    mpeg4type.bReversibleVLC = OMX_FALSE;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = mpeg4type.eProfile;
    defaultProfileLevel.mLevel = mpeg4type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    mpeg4type.eProfile = static_cast<OMX_VIDEO_MPEG4PROFILETYPE>(profileLevel.mProfile);
    mpeg4type.eLevel = static_cast<OMX_VIDEO_MPEG4LEVELTYPE>(profileLevel.mLevel);

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);
    CHECK_EQ(setupErrorCorrectionParameters(), (status_t)OK);

    return OK;
}
#ifdef MTK_AOSP_ENHANCEMENT
status_t OMXCodec::setupVP8EncoderParameters(const sp<MetaData> &meta)
{
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);
    OMX_VIDEO_PARAM_VP8TYPE vp8type;
    InitOMXParams(&vp8type);
    vp8type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
                       mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoVp8, &vp8type, sizeof(vp8type));
    CHECK_EQ(err, (status_t)OK);

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = vp8type.eProfile;
    defaultProfileLevel.mLevel = vp8type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) { return err; }
    vp8type.eProfile = static_cast<OMX_VIDEO_VP8PROFILETYPE>(profileLevel.mProfile);
    vp8type.eLevel = static_cast<OMX_VIDEO_VP8LEVELTYPE>(profileLevel.mLevel);

    vp8type.nDCTPartitions = 0;
    vp8type.bErrorResilientMode = OMX_FALSE;

    err = mOMX->setParameter(
              mNode, (OMX_INDEXTYPE)OMX_IndexParamVideoVp8, &vp8type, sizeof(vp8type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);
    CHECK_EQ(setupErrorCorrectionParameters(), (status_t)OK);

    return OK;
}
#endif //MTK_AOSP_ENHANCEMENT
status_t OMXCodec::setupAVCEncoderParameters(const sp<MetaData> &meta)
{
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);

    OMX_VIDEO_PARAM_AVCTYPE h264type;
    InitOMXParams(&h264type);
    h264type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));
    CHECK_EQ(err, (status_t)OK);

    h264type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = h264type.eProfile;
    defaultProfileLevel.mLevel = h264type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    h264type.eProfile = static_cast<OMX_VIDEO_AVCPROFILETYPE>(profileLevel.mProfile);
    h264type.eLevel = static_cast<OMX_VIDEO_AVCLEVELTYPE>(profileLevel.mLevel);

#ifndef MTK_AOSP_ENHANCEMENT
    //Bruce Hsu 2013/01/08 we hope use the platform default profile & level to keep the video quality
    if (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline) {
        ALOGW("Use baseline profile instead of %d for AVC recording",
            h264type.eProfile);
        h264type.eProfile = OMX_VIDEO_AVCProfileBaseline;
    }
#endif//MTK_AOSP_ENHANCEMENT

    if (h264type.eProfile == OMX_VIDEO_AVCProfileBaseline) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 1;
        h264type.nBFrames = 0;
        h264type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
        if (h264type.nPFrames == 0) {
            h264type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
        }
        h264type.nRefIdx10ActiveMinus1 = 0;
        h264type.nRefIdx11ActiveMinus1 = 0;
        h264type.bEntropyCodingCABAC = OMX_FALSE;
        h264type.bWeightedPPrediction = OMX_FALSE;
        h264type.bconstIpred = OMX_FALSE;
        h264type.bDirect8x8Inference = OMX_FALSE;
        h264type.bDirectSpatialTemporal = OMX_FALSE;
        h264type.nCabacInitIdc = 0;
    }

    if (h264type.nBFrames != 0) {
        h264type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
    }

    h264type.bEnableUEP = OMX_FALSE;
    h264type.bEnableFMO = OMX_FALSE;
    h264type.bEnableASO = OMX_FALSE;
    h264type.bEnableRS = OMX_FALSE;
    h264type.bFrameMBsOnly = OMX_TRUE;
    h264type.bMBAFF = OMX_FALSE;
    h264type.eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);

    return OK;
}

status_t OMXCodec::setVideoOutputFormat(
        const char *mime, const sp<MetaData>& meta) {

    int32_t width, height;
    bool success = meta->findInt32(kKeyWidth, &width);
    success = success && meta->findInt32(kKeyHeight, &height);
    CHECK(success);

    CODEC_LOGV("setVideoOutputFormat width=%d, height=%d", width, height);

    OMX_VIDEO_CODINGTYPE compressionFormat = OMX_VIDEO_CodingUnused;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingAVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG4;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingHEVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
        compressionFormat = OMX_VIDEO_CodingH263;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP8, mime)) {
        compressionFormat = OMX_VIDEO_CodingVP8;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP9, mime)) {
        compressionFormat = OMX_VIDEO_CodingVP9;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG2, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG2;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime))           // Morris Yang add for ASF
    {
        compressionFormat = OMX_VIDEO_CodingWMV;
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime))
    {
        compressionFormat = OMX_VIDEO_CodingDIVX;
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX3, mime))
    {
        compressionFormat = OMX_VIDEO_CodingDIVX3;
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_XVID, mime))
    {
        compressionFormat = OMX_VIDEO_CodingXVID;
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MJPEG, mime))
    {
        compressionFormat = OMX_VIDEO_CodingMJPEG;
    }
    else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_SORENSON_SPARK, mime))
    {
        compressionFormat = OMX_VIDEO_CodingS263;
    }
#endif //MTK_AOSP_ENHANCEMENT
    else
    {
        ALOGE("Not a supported video mime type: %s", mime);
        CHECK(!"Should not be here. Not a supported video mime type.");
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (mFlags & kUseMaxOutputBuffers)
    {
        OMX_INDEXTYPE index;
        status_t err = mOMX->getExtensionIndex(mNode, "OMX.MTK.index.param.video.FixedMaxBuffer", &index);
        if (err == OK)
        {
            OMX_BOOL m = OMX_TRUE;
            status_t err2 = mOMX->setParameter(mNode, index, &m, sizeof(m));
            ALOGI("set FixedMaxBuffer, index = %x, err = %x, err2 = %x", index, err, err2);
        }
    }
#endif //MTK_AOSP_ENHANCEMENT

    status_t err = setVideoPortFormatType(
            kPortIndexInput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        return err;
    }

#if 1
    {
        OMX_VIDEO_PARAM_PORTFORMATTYPE format;
        InitOMXParams(&format);
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;

        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));
        CHECK_EQ(err, (status_t)OK);
        CHECK_EQ((int)format.eCompressionFormat, (int)OMX_VIDEO_CodingUnused);

#if 0 //KitKat cancel this check start
        CHECK(format.eColorFormat == OMX_COLOR_FormatYUV420Planar
              || format.eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar
              || format.eColorFormat == OMX_COLOR_FormatCbYCrY
              || format.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar
              || format.eColorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar
              || format.eColorFormat == OMX_QCOM_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka);
#endif //KitKat cancel this check end

        int32_t colorFormat;
        if (meta->findInt32(kKeyColorFormat, &colorFormat)
                && colorFormat != OMX_COLOR_FormatUnused
                && colorFormat != format.eColorFormat) {

            while (OMX_ErrorNoMore != err) {
                format.nIndex++;
                err = mOMX->getParameter(
                        mNode, OMX_IndexParamVideoPortFormat,
                            &format, sizeof(format));
                if (format.eColorFormat == colorFormat) {
                    break;
                }
            }
            if (format.eColorFormat != colorFormat) {
                CODEC_LOGE("Color format %d is not supported", colorFormat);
                return ERROR_UNSUPPORTED;
            }
        }

        err = mOMX->setParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }
    }
#endif

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

#if 1
    // XXX Need a (much) better heuristic to compute input buffer sizes.
    const size_t X = 64 * 1024;
    if (def.nBufferSize < X) {
        def.nBufferSize = X;
    }
#endif

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    ////////////////////////////////////////////////////////////////////////////

    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

#if 0
    def.nBufferSize =
        (((width + 15) & -16) * ((height + 15) & -16) * 3) / 2;  // YUV420
#endif

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    return err;
}

OMXCodec::OMXCodec(
        const sp<IOMX> &omx, IOMX::node_id node,
        uint32_t quirks, uint32_t flags,
        bool isEncoder,
        const char *mime,
        const char *componentName,
        const sp<MediaSource> &source,
        const sp<ANativeWindow> &nativeWindow)
    : mOMX(omx),
      mOMXLivesLocally(omx->livesLocally(node, getpid())),
      mNode(node),
      mQuirks(quirks),
      mFlags(flags),
      mIsEncoder(isEncoder),
      mIsVideo(!strncasecmp("video/", mime, 6)),
      mMIME(strdup(mime)),
      mComponentName(strdup(componentName)),
      mSource(source),
      mCodecSpecificDataIndex(0),
      mState(LOADED),
      mInitialBufferSubmit(true),
      mSignalledEOS(false),
      mNoMoreOutputData(false),
      mOutputPortSettingsHaveChanged(false),
      mSeekTimeUs(-1),
      mSeekMode(ReadOptions::SEEK_CLOSEST_SYNC),
      mTargetTimeUs(-1),
      mOutputPortSettingsChangedPending(false),
      mSkipCutBuffer(NULL),
      mLeftOverBuffer(NULL),
      mPaused(false),
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CMMB_ENABLE
      IsCMMBFlag(false),
#endif //MTK_CMMB_ENABLE
      mIsVideoDecoder(false),
      mIsVideoEncoder(false),
      mInputBufferPoolMemBase(NULL),
      mOutputBufferPoolMemBase(NULL),
      mPropFlags(0),
      mMaxQueueBufferNum(0),
      mQueueWaiting(false),
      mSupportsPartialFrames(false),
      mVideoAspectRatioWidth(1),
      mVideoAspectRatioHeight(1),
      mIsVENCTimelapseMode(false),
      mRTSPOutputTimeoutUS(-1),
      mHTTPOutputTimeoutUS(-1),
      mIsHttpStreaming(false),
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
      mDolbyProcessedAudio(false),
      mDolbyProcessedAudioStateChanged(false),
#endif // DOLBY_END
#endif //MTK_AOSP_ENHANCEMENT
      mNativeWindow(
              (!strncmp(componentName, "OMX.google.", 11))
                        ? NULL : nativeWindow) {
    mPortStatus[kPortIndexInput] = ENABLED;
    mPortStatus[kPortIndexOutput] = ENABLED;

    setComponentRole();

#ifdef MTK_AOSP_ENHANCEMENT
    mPreRollStartTime = -1;

    if (false == mIsEncoder)
    {
        if ((!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)) ||        // Morris Yang add for ASF
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG2, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VPX, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP8, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP9, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MJPEG, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX3, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_XVID, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_SORENSON_SPARK, mime)))
        {
            mIsVideoDecoder = true;

            char value[PROPERTY_VALUE_MAX];
            property_get("omxcodec.video.input.error.rate", value, "0.0");
            mVideoInputErrorRate = atof(value);
            if (mVideoInputErrorRate > 0)
            {
                mPropFlags |= OMXCODEC_ENABLE_VIDEO_INPUT_ERROR_PATTERNS;
            }
            ALOGD("mVideoInputErrorRate(%f)", mVideoInputErrorRate);
        }
    }
    else
    {
        if ((!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VPX, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP8, mime)) ||
            (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)))
        {
            mIsVideoEncoder = true;

            mCameraMeta = new MetaData;

            if (!mOMXLivesLocally)
            {
                mQuirks &= ~kAvoidMemcopyInputRecordingFrames;
            }
        }
    }

    ALOGD("!@@!>> create tid (%d) OMXCodec mOMXLivesLocally=%d, mIsVideoDecoder(%d), mIsVideoEncoder(%d), mime(%s)", gettid(), mOMXLivesLocally, mIsVideoDecoder, mIsVideoEncoder, mime);
#endif
}

// static
void OMXCodec::setComponentRole(
        const sp<IOMX> &omx, IOMX::node_id node, bool isEncoder,
        const char *mime) {
    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_MPEG,
            "audio_decoder.mp3", "audio_encoder.mp3" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I,
            "audio_decoder.mp1", "audio_encoder.mp1" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
            "audio_decoder.mp2", "audio_encoder.mp2" },
        { MEDIA_MIMETYPE_AUDIO_AMR_NB,
            "audio_decoder.amrnb", "audio_encoder.amrnb" },
        { MEDIA_MIMETYPE_AUDIO_AMR_WB,
            "audio_decoder.amrwb", "audio_encoder.amrwb" },
        { MEDIA_MIMETYPE_AUDIO_AAC,
            "audio_decoder.aac", "audio_encoder.aac" },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
            "audio_decoder.vorbis", "audio_encoder.vorbis" },
#ifdef MTK_AOSP_ENHANCEMENT
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", ""},
#ifdef MTK_AUDIO_ALAC_SUPPORT
        {
            MEDIA_MIMETYPE_AUDIO_ALAC,
            "audio_decoder.alac", "audio_encoder.alac"
        },
#endif
#endif //MTK_AOSP_ENHANCEMENT
        { MEDIA_MIMETYPE_AUDIO_OPUS,
            "audio_decoder.opus", "audio_encoder.opus" },
        { MEDIA_MIMETYPE_AUDIO_G711_MLAW,
            "audio_decoder.g711mlaw", "audio_encoder.g711mlaw" },
        { MEDIA_MIMETYPE_AUDIO_G711_ALAW,
            "audio_decoder.g711alaw", "audio_encoder.g711alaw" },
        { MEDIA_MIMETYPE_VIDEO_AVC,
            "video_decoder.avc", "video_encoder.avc" },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
            "video_decoder.hevc", "video_encoder.hevc" },
        { MEDIA_MIMETYPE_VIDEO_MPEG4,
            "video_decoder.mpeg4", "video_encoder.mpeg4" },
        { MEDIA_MIMETYPE_VIDEO_H263,
            "video_decoder.h263", "video_encoder.h263" },
        { MEDIA_MIMETYPE_VIDEO_VP8,
            "video_decoder.vp8", "video_encoder.vp8" },
        { MEDIA_MIMETYPE_VIDEO_VP9,
            "video_decoder.vp9", "video_encoder.vp9" },
#ifdef MTK_AOSP_ENHANCEMENT
        {  MEDIA_MIMETYPE_VIDEO_VPX,
            "video_decoder.vpx", "video_encoder.vpx" },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
            "video_decoder.mpeg2", "video_encoder.mpeg2" },
        { MEDIA_MIMETYPE_VIDEO_DIVX,
            "video_decoder.divx", "video_encoder.divx" },
        { MEDIA_MIMETYPE_VIDEO_DIVX3,
            "video_decoder.divx3", "video_encoder.divx3" },
        { MEDIA_MIMETYPE_VIDEO_XVID,
            "video_decoder.xvid", "video_encoder.xvid" },
        { MEDIA_MIMETYPE_VIDEO_SORENSON_SPARK,
            "video_decoder.s263", "video_encoder.s263" },
#endif //MTK_AOSP_ENHANCEMENT
        { MEDIA_MIMETYPE_AUDIO_RAW,
            "audio_decoder.raw", "audio_encoder.raw" },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", "audio_encoder.flac" },
        { MEDIA_MIMETYPE_AUDIO_MSGSM,
            "audio_decoder.gsm", "audio_encoder.gsm" },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
            "video_decoder.mpeg2", "video_encoder.mpeg2" },
        { MEDIA_MIMETYPE_AUDIO_AC3,
            "audio_decoder.ac3", "audio_encoder.ac3" },
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
        { MEDIA_MIMETYPE_AUDIO_EAC3,
            "audio_decoder.ec3", NULL },
        { MEDIA_MIMETYPE_AUDIO_EAC3_JOC,
            "audio_decoder.ec3_joc", NULL },
#endif // DOLBY_END
    };

    static const size_t kNumMimeToRole =
        sizeof(kMimeToRole) / sizeof(kMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return;
    }

    const char *role =
        isEncoder ? kMimeToRole[i].encoderRole
                  : kMimeToRole[i].decoderRole;

    if (role != NULL) {
        OMX_PARAM_COMPONENTROLETYPE roleParams;
        InitOMXParams(&roleParams);

        strncpy((char *)roleParams.cRole,
                role, OMX_MAX_STRINGNAME_SIZE - 1);

        roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

        status_t err = omx->setParameter(
                node, OMX_IndexParamStandardComponentRole,
                &roleParams, sizeof(roleParams));

        if (err != OK) {
            ALOGW("Failed to set standard component role '%s'.", role);
        }
    }
}

void OMXCodec::setComponentRole() {
    setComponentRole(mOMX, mNode, mIsEncoder, mMIME);
}

OMXCodec::~OMXCodec() {
#ifdef MTK_AOSP_ENHANCEMENT
    CHECK_EQ(mQueueWaiting, false);
    char *mTmpMIME = strdup(mMIME);
#endif //MTK_AOSP_ENHANCEMENT
    mSource.clear();

#ifdef MTK_AOSP_ENHANCEMENT
    //freeNode takes care of the rest
    if (!((mState == LOADED || mState == ERROR || mState == LOADED_TO_IDLE))){
        ALOGW("mState exit at %d", (uint32_t)mState);
    }
#else
    CHECK(mState == LOADED || mState == ERROR || mState == LOADED_TO_IDLE);
#endif

    status_t err = mOMX->freeNode(mNode);
    CHECK_EQ(err, (status_t)OK);

    mNode = 0;
    setState(DEAD);

    clearCodecSpecificData();

    free(mComponentName);
    mComponentName = NULL;

    free(mMIME);
    mMIME = NULL;
#ifdef MTK_AOSP_ENHANCEMENT

    //android::CallStack stack("OMXCodec");

    ALOGD("!@@!>> destroy tid (%d) OMXCodec mOMXLivesLocally=%d, mIsVideoDecoder(%d), mIsVideoEncoder(%d), mime(%s)", gettid(), mOMXLivesLocally, mIsVideoDecoder, mIsVideoEncoder, mTmpMIME);
    free(mTmpMIME);
    mTmpMIME = NULL;

#endif //MTK_AOSP_ENHANCEMENT
}

status_t OMXCodec::init() {
    // mLock is held.

    CHECK_EQ((int)mState, (int)LOADED);

#ifdef MTK_AOSP_ENHANCEMENT
    const char *mime = NULL;
    sp<MetaData> meta = mSource->getFormat();
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (mIsVideoEncoder && (mCameraMeta.get() != NULL) && (mFlags & kOnlySubmitOneInputBufferAtOneTime) &&
        (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mMIME) || !strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mMIME) || !strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mMIME)))
    {
        // Timelapse mode
        ALOGD("Set timelapse mode");
        OMX_BOOL bTimeLapseEnabled = OMX_TRUE;
        status_t err2 = mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVencSetTimelapseMode, &bTimeLapseEnabled, sizeof(bTimeLapseEnabled));
        CHECK_EQ((int)err2, (int)OK);
        mIsVENCTimelapseMode = true;
    }
    if (mOMXLivesLocally)
    {
        OMX_BOOL bIsLocally = OMX_TRUE;
        status_t err2 = mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVideoSetClientLocally, &bIsLocally, sizeof(bIsLocally));
        //CHECK_EQ((int)err2, (int)OK);
        ALOGD("setParameter bIsLocally %d, return %x", bIsLocally, err2);
    }
    else
    {
        OMX_BOOL bIsLocally = OMX_FALSE;
        status_t err2 = mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVideoSetClientLocally, &bIsLocally, sizeof(bIsLocally));
        ALOGD("setParameter bIsLocally %d, return %x", bIsLocally, err2);
    }
#endif //MTK_AOSP_ENHANCEMENT

    status_t err;
    if (!(mQuirks & kRequiresLoadedToIdleAfterAllocation)) {
        err = mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
        CHECK_EQ(err, (status_t)OK);
        setState(LOADED_TO_IDLE);
    }

    err = allocateBuffers();
    if (err != (status_t)OK) {
        return err;
    }

    if (mQuirks & kRequiresLoadedToIdleAfterAllocation) {
        err = mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
        CHECK_EQ(err, (status_t)OK);

        setState(LOADED_TO_IDLE);
    }

    while (mState != EXECUTING && mState != ERROR) {
        mAsyncCompletion.wait(mLock);
    }

    return mState == ERROR ? UNKNOWN_ERROR : OK;
}

// static
bool OMXCodec::isIntermediateState(State state) {
    return state == LOADED_TO_IDLE
        || state == IDLE_TO_EXECUTING
        || state == EXECUTING_TO_IDLE
        || state == IDLE_TO_LOADED
        || state == RECONFIGURING;
}

status_t OMXCodec::allocateBuffers() {
    status_t err = allocateBuffersOnPort(kPortIndexInput);

    if (err != OK) {
        return err;
    }

    return allocateBuffersOnPort(kPortIndexOutput);
}

status_t OMXCodec::allocateBuffersOnPort(OMX_U32 portIndex) {
    if (mNativeWindow != NULL && portIndex == kPortIndexOutput) {
        return allocateOutputBuffersFromNativeWindow();
    }

    if ((mFlags & kEnableGrallocUsageProtected) && portIndex == kPortIndexOutput) {
        ALOGE("protected output buffers must be stent to an ANativeWindow");
        return PERMISSION_DENIED;
    }

    status_t err = OK;
    if ((mFlags & kStoreMetaDataInVideoBuffers)
            && portIndex == kPortIndexInput) {
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexInput, OMX_TRUE);
        if (err != OK) {
            ALOGE("Storing meta data in video buffers is not supported");
            return err;
        }
    }

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    CODEC_LOGD("allocating %u buffers of size %u on %s port",
            def.nBufferCountActual, def.nBufferSize,
            portIndex == kPortIndexInput ? "input" : "output");

    if (def.nBufferSize != 0 && def.nBufferCountActual > SIZE_MAX / def.nBufferSize) {
        return BAD_VALUE;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    //MemoryDealer would eat memory on each allocation
    OMX_U32 memoryAlign = 32;
    size_t totalSize = def.nBufferCountActual *
        ((def.nBufferSize + (memoryAlign - 1))&(~(memoryAlign - 1)));
#else
    size_t totalSize = def.nBufferCountActual * def.nBufferSize;
#endif
    mDealer[portIndex] = new MemoryDealer(totalSize, "OMXCodec");

    for (OMX_U32 i = 0; i < def.nBufferCountActual; ++i)
    {
        sp<IMemory> mem = mDealer[portIndex]->allocate(def.nBufferSize);
#ifdef MTK_AOSP_ENHANCEMENT
        //SW codec supports all resolution and possibly fails to allocate
        if (mem.get() == NULL){
            CODEC_LOGE("Failed to allocate memory from mDealer for %d from %zu",
                    def.nBufferSize, totalSize);
            return NO_MEMORY;
        }
#else
        if (mem == NULL || mem->pointer() == NULL) {
            return NO_MEMORY;
        }
#endif

        BufferInfo info;
        info.mData = NULL;
        info.mSize = def.nBufferSize;

        IOMX::buffer_id buffer;
        if (portIndex == kPortIndexInput
                && ((mQuirks & kRequiresAllocateBufferOnInputPorts)
                    || (mFlags & kUseSecureInputBuffers))) {
            if (mOMXLivesLocally) {
                mem.clear();

                err = mOMX->allocateBuffer(
                        mNode, portIndex, def.nBufferSize, &buffer,
                        &info.mData);
            } else {
                err = mOMX->allocateBufferWithBackup(
                        mNode, portIndex, mem, &buffer, mem->size());
            }
        } else if (portIndex == kPortIndexOutput
                && (mQuirks & kRequiresAllocateBufferOnOutputPorts)) {
            if (mOMXLivesLocally) {
                mem.clear();

                err = mOMX->allocateBuffer(
                        mNode, portIndex, def.nBufferSize, &buffer,
                        &info.mData);
            } else {
                err = mOMX->allocateBufferWithBackup(
                        mNode, portIndex, mem, &buffer, mem->size());
            }
        } else {
            err = mOMX->useBuffer(mNode, portIndex, mem, &buffer, mem->size());
        }

        if (err != OK) {
            ALOGE("allocate_buffer_with_backup failed");
            return err;
        }

        if (mem != NULL) {
            info.mData = mem->pointer();
        }

        info.mBuffer = buffer;
        info.mStatus = OWNED_BY_US;
        info.mMem = mem;
        info.mMediaBuffer = NULL;

        if (portIndex == kPortIndexOutput) {
            // Fail deferred MediaBuffer creation until FILL_BUFFER_DONE;
            // this legacy mode is no longer supported.
            LOG_ALWAYS_FATAL_IF((mOMXLivesLocally
                    && (mQuirks & kRequiresAllocateBufferOnOutputPorts)
                    && (mQuirks & kDefersOutputBufferAllocation)),
                    "allocateBuffersOnPort cannot defer buffer allocation");

            info.mMediaBuffer = new MediaBuffer(info.mData, info.mSize);
            info.mMediaBuffer->setObserver(this);
        }

        mPortBuffers[portIndex].push(info);

        CODEC_LOGD("allocated buffer %u on %s port", buffer,
             portIndex == kPortIndexInput ? "input" : "output");
    }

    if (portIndex == kPortIndexOutput) {

        sp<MetaData> meta = mSource->getFormat();
        int32_t delay = 0;
        if (!meta->findInt32(kKeyEncoderDelay, &delay)) {
            delay = 0;
        }
        int32_t padding = 0;
        if (!meta->findInt32(kKeyEncoderPadding, &padding)) {
            padding = 0;
        }
        int32_t numchannels = 0;
        if (delay + padding) {
            if (mOutputFormat->findInt32(kKeyChannelCount, &numchannels)) {
                size_t frameSize = numchannels * sizeof(int16_t);
                if (mSkipCutBuffer != NULL) {
                    size_t prevbuffersize = mSkipCutBuffer->size();
                    if (prevbuffersize != 0) {
                        ALOGW("Replacing SkipCutBuffer holding %zu bytes", prevbuffersize);
                    }
                }
                mSkipCutBuffer = new SkipCutBuffer(delay * frameSize, padding * frameSize);
            }
        }
    }

    // dumpPortStatus(portIndex);

    if (portIndex == kPortIndexInput && (mFlags & kUseSecureInputBuffers)) {
        Vector<MediaBuffer *> buffers;
        for (size_t i = 0; i < def.nBufferCountActual; ++i) {
            const BufferInfo &info = mPortBuffers[kPortIndexInput].itemAt(i);

            MediaBuffer *mbuf = new MediaBuffer(info.mData, info.mSize);
            buffers.push(mbuf);
        }

        status_t err = mSource->setBuffers(buffers);

        if (err != OK) {
            for (size_t i = 0; i < def.nBufferCountActual; ++i) {
                buffers.editItemAt(i)->release();
            }
            buffers.clear();

            CODEC_LOGE(
                    "Codec requested to use secure input buffers but "
                    "upstream source didn't support that.");

            return err;
        }
    }

    return OK;
}

status_t OMXCodec::allocateOutputBuffersFromNativeWindow() {
    // Get the number of buffers needed.
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        CODEC_LOGE("getParameter failed: %d", err);
        return err;
    }

    sp<MetaData> meta = mSource->getFormat();

#ifdef MTK_AOSP_ENHANCEMENT
    uint32_t eHalColorFormat = HAL_PIXEL_FORMAT_YV12;//tmp for build pass
    uint32_t eHalColorFormatFromOMX = def.format.video.eColorFormat;
    switch (eHalColorFormatFromOMX)
    {
        case OMX_COLOR_FormatYUV420Planar:
#if  ((defined MTK_CLEARMOTION_SUPPORT) || (defined MTK_POST_PROCESS_FRAMEWORK_SUPPORT) || (defined MTK_DEINTERLACE_SUPPORT))
            eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;
            ALOGD("[MJC][OMX_COLOR_FormatYUV420Planar] eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;");
#else
            //eHalColorFormat = HAL_PIXEL_FORMAT_YV12;
            eHalColorFormat = HAL_PIXEL_FORMAT_I420;
#endif
            break;
        case OMX_COLOR_FormatVendorMTKYUV:
#if  ((defined MTK_CLEARMOTION_SUPPORT) || (defined MTK_POST_PROCESS_FRAMEWORK_SUPPORT) || (defined MTK_DEINTERLACE_SUPPORT))
            eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;
            ALOGD("[MJC][OMX_COLOR_FormatVendorMTKYUV] eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;");
#else
            eHalColorFormat = HAL_PIXEL_FORMAT_NV12_BLK;
#endif
            break;
        case OMX_COLOR_FormatVendorMTKYUV_FCM:
#if  ((defined MTK_CLEARMOTION_SUPPORT) || (defined MTK_DEINTERLACE_SUPPORT))
            eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;
            ALOGD("[MJC][OMX_COLOR_FormatVendorMTKYUV_FCM] eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;");
#else
            eHalColorFormat = HAL_PIXEL_FORMAT_NV12_BLK_FCM;
#endif
            break;
        case OMX_MTK_COLOR_FormatYV12:
#if  ((defined MTK_CLEARMOTION_SUPPORT) || (defined MTK_POST_PROCESS_FRAMEWORK_SUPPORT) || (defined MTK_DEINTERLACE_SUPPORT))
            eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;
            ALOGD("[MJC][OMX_MTK_COLOR_FormatYV12] eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;");
#else
            eHalColorFormat = HAL_PIXEL_FORMAT_YV12;
#endif
            break;
        case OMX_COLOR_Format32bitARGB8888:
            eHalColorFormat = HAL_PIXEL_FORMAT_RGBA_8888;
            break;
        case OMX_COLOR_FormatVendorMTKYUV_UFO:
            eHalColorFormat = HAL_PIXEL_FORMAT_UFO;
            break;
        case HAL_PIXEL_FORMAT_UFO:
            eHalColorFormat = HAL_PIXEL_FORMAT_UFO;
            break;
         case HAL_PIXEL_FORMAT_YV12:
            eHalColorFormat = HAL_PIXEL_FORMAT_YV12;
            break;
        default:
#if  ((defined MTK_CLEARMOTION_SUPPORT) || (defined MTK_POST_PROCESS_FRAMEWORK_SUPPORT) || (defined MTK_DEINTERLACE_SUPPORT))
            eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;
            ALOGD("[MJC][default] eHalColorFormat = HAL_PIXEL_FORMAT_YUV_PRIVATE;");
#else
            //eHalColorFormat = HAL_PIXEL_FORMAT_YV12;
            eHalColorFormat = HAL_PIXEL_FORMAT_I420;
            ALOGE("allocateOutputBuffersFromNativeWindow undefined switch case");
            ALOGE("native_window_set_buffers_geometry to colorformat 0x%x",eHalColorFormat);
#endif
            break;
    }
#endif //MTK_AOSP_ENHANCEMENT

    int32_t rotationDegrees;
    if (!meta->findInt32(kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    // Set up the native window.
    OMX_U32 usage = 0;
    err = mOMX->getGraphicBufferUsage(mNode, kPortIndexOutput, &usage);
    if (err != 0) {
        ALOGW("querying usage flags from OMX IL component failed: %d", err);
        // XXX: Currently this error is logged, but not fatal.
        usage = 0;
    }

#ifdef MTK_SEC_VIDEO_PATH_SUPPORT
    if (mFlags & kUseSecureInputBuffers)
    {
        usage |= GRALLOC_USAGE_SECURE;
        ALOGD("@@ set GRALLOC_USAGE_SECURE");
    }
#endif //MTK_SEC_VIDEO_PATH_SUPPORT

    if (mFlags & kEnableGrallocUsageProtected) {
        usage |= GRALLOC_USAGE_PROTECTED;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    usage |= (GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN);
#endif //MTK_AOSP_ENHANCEMENT

    err = setNativeWindowSizeFormatAndUsage(
            mNativeWindow.get(),
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
            def.format.video.eColorFormat,
            rotationDegrees,
            usage | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);
    if (err != 0) {
        return err;
    }

    int minUndequeuedBufs = 0;
    err = mNativeWindow->query(mNativeWindow.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
    if (err != 0) {
        ALOGE("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }
    // FIXME: assume that surface is controlled by app (native window
    // returns the number for the case when surface is not controlled by app)
    // FIXME2: This means that minUndeqeueudBufs can be 1 larger than reported
    // For now, try to allocate 1 more buffer, but don't fail if unsuccessful

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_SLOW_MOTION_VIDEO_SUPPORT
    if (mIsSlowMotion) {
        minUndequeuedBufs += 1; // Slowmotion will use Async mode
        ALOGD("SM async. minUndeq +1 (%d)", minUndequeuedBufs);
        mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVdecGetMinUndequeuedBufs, &minUndequeuedBufs, sizeof(void *));
        err = mOMX->getParameter(
                  mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
        if (err != OK)
        {
            CODEC_LOGE("getParameter failed: %d", err);
            return err;
        }
    }
#endif

#ifdef MTK_CLEARMOTION_SUPPORT
    if (def.format.video.nFrameWidth * 0.9 >= 1280 || def.format.video.nFrameHeight * 0.9 >= 736) //MJC scaler off for display performance cancel buffer +1
    {
        minUndequeuedBufs += 1;
        CODEC_LOGV("MJC scaler off for display performance cancel buffer +1 !! %d x %d minUndequeuedBufs %d", def.format.video.nFrameWidth, def.format.video.nFrameHeight, minUndequeuedBufs);
    }
    if (mFlags & kUseClearMotion)
    {
        mOMX->setParameter(mNode, OMX_IndexVendorMtkOmxVdecGetMinUndequeuedBufs, &minUndequeuedBufs, sizeof(void *));
    }
    err = mOMX->getParameter(
              mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK)
    {
        CODEC_LOGE("getParameter failed: %d", err);
        return err;
    }
#endif
#endif

    // Use conservative allocation while also trying to reduce starvation
    //
    // 1. allocate at least nBufferCountMin + minUndequeuedBuffers - that is the
    //    minimum needed for the consumer to be able to work
    // 2. try to allocate two (2) additional buffers to reduce starvation from
    //    the consumer
    //    plus an extra buffer to account for incorrect minUndequeuedBufs
    CODEC_LOGI("OMX-buffers: min=%u actual=%u undeq=%d+1",
            def.nBufferCountMin, def.nBufferCountActual, minUndequeuedBufs);

    for (OMX_U32 extraBuffers = 2 + 1; /* condition inside loop */; extraBuffers--) {
        OMX_U32 newBufferCount =
            def.nBufferCountMin + minUndequeuedBufs + extraBuffers;
        def.nBufferCountActual = newBufferCount;
        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err == OK) {
            minUndequeuedBufs += extraBuffers;
            break;
        }

        CODEC_LOGW("setting nBufferCountActual to %u failed: %d",
                newBufferCount, err);
        /* exit condition */
        if (extraBuffers == 0) {
            return err;
        }
    }
    CODEC_LOGI("OMX-buffers: min=%u actual=%u undeq=%d+1",
            def.nBufferCountMin, def.nBufferCountActual, minUndequeuedBufs);

    err = native_window_set_buffer_count(
            mNativeWindow.get(), def.nBufferCountActual);
    if (err != 0) {
        ALOGE("native_window_set_buffer_count failed: %s (%d)", strerror(-err),
                -err);
        return err;
    }

    CODEC_LOGV("allocating %u buffers from a native window of size %u on "
            "output port", def.nBufferCountActual, def.nBufferSize);

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < def.nBufferCountActual; i++) {
        ANativeWindowBuffer* buf;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
        if (err != 0) {
            ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
            break;
        }

        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
        BufferInfo info;
        info.mData = NULL;
        info.mSize = def.nBufferSize;
        info.mStatus = OWNED_BY_US;
        info.mMem = NULL;
        info.mMediaBuffer = new MediaBuffer(graphicBuffer);
        info.mMediaBuffer->setObserver(this);
        mPortBuffers[kPortIndexOutput].push(info);

        IOMX::buffer_id bufferId;
        err = mOMX->useGraphicBuffer(mNode, kPortIndexOutput, graphicBuffer,
                &bufferId);
        if (err != 0) {
            CODEC_LOGE("registering GraphicBuffer with OMX IL component "
                    "failed: %d", err);
            break;
        }

        mPortBuffers[kPortIndexOutput].editItemAt(i).mBuffer = bufferId;

        CODEC_LOGV("registered graphic buffer with ID %u (pointer = %p)",
                bufferId, graphicBuffer.get());
    }

    OMX_U32 cancelStart;
    OMX_U32 cancelEnd;
    if (err != 0) {
        // If an error occurred while dequeuing we need to cancel any buffers
        // that were dequeued.
        cancelStart = 0;
        cancelEnd = mPortBuffers[kPortIndexOutput].size();
    } else {
        // Return the last two buffers to the native window.
        cancelStart = def.nBufferCountActual - minUndequeuedBufs;
        cancelEnd = def.nBufferCountActual;
    }

    for (OMX_U32 i = cancelStart; i < cancelEnd; i++) {
        BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(i);
        cancelBufferToNativeWindow(info);
    }

    return err;
}

status_t OMXCodec::cancelBufferToNativeWindow(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    CODEC_LOGV("Calling cancelBuffer on buffer %u", info->mBuffer);
    int err = mNativeWindow->cancelBuffer(
        mNativeWindow.get(), info->mMediaBuffer->graphicBuffer().get(), -1);
    if (err != 0) {
      CODEC_LOGE("cancelBuffer failed w/ error 0x%08x", err);

      setState(ERROR);
      return err;
    }
    info->mStatus = OWNED_BY_NATIVE_WINDOW;
    return OK;
}

OMXCodec::BufferInfo* OMXCodec::dequeueBufferFromNativeWindow() {
    // Dequeue the next buffer from the native window.
    ANativeWindowBuffer* buf;
    int err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
    if (err != 0) {
      CODEC_LOGE("dequeueBuffer failed w/ error 0x%08x", err);
#ifdef MTK_AOSP_ENHANCEMENT
        mFinalStatus = ERROR_BUFFER_DEQUEUE_FAIL;
#endif
      setState(ERROR);
      return 0;
    }

    // Determine which buffer we just dequeued.
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    BufferInfo *bufInfo = 0;
    for (size_t i = 0; i < buffers->size(); i++) {
      sp<GraphicBuffer> graphicBuffer = buffers->itemAt(i).
          mMediaBuffer->graphicBuffer();
      if (graphicBuffer->handle == buf->handle) {
        bufInfo = &buffers->editItemAt(i);
        break;
      }
    }

    if (bufInfo == 0) {
        CODEC_LOGE("dequeued unrecognized buffer: %p", buf);

        setState(ERROR);
        return 0;
    }

    // The native window no longer owns the buffer.
    CHECK_EQ((int)bufInfo->mStatus, (int)OWNED_BY_NATIVE_WINDOW);
    bufInfo->mStatus = OWNED_BY_US;

    return bufInfo;
}

int64_t OMXCodec::getDecodingTimeUs() {
    CHECK(mIsEncoder && mIsVideo);

    if (mDecodingTimeList.empty()) {
#ifdef MTK_AOSP_ENHANCEMENT
        if (mState != ERROR){
             CHECK(mSignalledEOS || mNoMoreOutputData);
        }
#else
        CHECK(mSignalledEOS || mNoMoreOutputData);
#endif
        // No corresponding input frame available.
        // This could happen when EOS is reached.
        return 0;
    }

    List<int64_t>::iterator it = mDecodingTimeList.begin();
    int64_t timeUs = *it;
    mDecodingTimeList.erase(it);
    return timeUs;
}

void OMXCodec::on_message(const omx_message &msg) {
#ifndef MTK_AOSP_ENHANCEMENT // remove Android 4.0 default code , Legis.
    if (mState == ERROR)
    {
        /*
         * only drop EVENT messages, EBD and FBD are still
         * processed for bookkeeping purposes
         */
        if (msg.type == omx_message::EVENT) {
            ALOGW("Dropping OMX EVENT message - we're in ERROR state.");
            return;
        }
    }
#endif //MTK_AOSP_ENHANCEMENT

    switch (msg.type) {
        case omx_message::EVENT:
        {
            onEvent(
                 msg.u.event_data.event, msg.u.event_data.data1,
                 msg.u.event_data.data2);

            break;
        }

        case omx_message::EMPTY_BUFFER_DONE:
        {
            IOMX::buffer_id buffer = msg.u.extended_buffer_data.buffer;

            CODEC_LOGV("EMPTY_BUFFER_DONE(buffer: %u)", buffer);

            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            size_t i = 0;
            while (i < buffers->size() && (*buffers)[i].mBuffer != buffer) {
                ++i;
            }

            CHECK(i < buffers->size());
            if ((*buffers)[i].mStatus != OWNED_BY_COMPONENT) {
                ALOGW("We already own input buffer %u, yet received "
                     "an EMPTY_BUFFER_DONE.", buffer);
            }

            BufferInfo* info = &buffers->editItemAt(i);
            info->mStatus = OWNED_BY_US;

            // Buffer could not be released until empty buffer done is called.
            if (info->mMediaBuffer != NULL) {
                info->mMediaBuffer->release();
                info->mMediaBuffer = NULL;
            }

            if (mPortStatus[kPortIndexInput] == DISABLING) {
                CODEC_LOGV("Port is disabled, freeing buffer %u", buffer);

                status_t err = freeBuffer(kPortIndexInput, i);
                CHECK_EQ(err, (status_t)OK);
            } else if (mState != ERROR
                    && mPortStatus[kPortIndexInput] != SHUTTING_DOWN) {
                CHECK_EQ((int)mPortStatus[kPortIndexInput], (int)ENABLED);

                if (mFlags & kUseSecureInputBuffers) {
                    drainAnyInputBuffer();
                } else {
#ifdef OMX_VE_AUDIO
                    if (!(mFlags & kAudUseOMXForVE))
                    {
                        drainInputBuffer(&buffers->editItemAt(i));
                    }
#else  //OMX_VE_AUDIO
                    drainInputBuffer(&buffers->editItemAt(i));
#endif  //OMX_VE_AUDIO
                }
            }
            break;
        }

        case omx_message::FILL_BUFFER_DONE:
        {
            IOMX::buffer_id buffer = msg.u.extended_buffer_data.buffer;
            OMX_U32 flags = msg.u.extended_buffer_data.flags;

            CODEC_LOGV("FILL_BUFFER_DONE(buffer: %u, size: %u, flags: 0x%08x, timestamp: %lld us (%.2f secs))",
                 buffer,
                 msg.u.extended_buffer_data.range_length,
                 flags,
                 msg.u.extended_buffer_data.timestamp,
                 msg.u.extended_buffer_data.timestamp / 1E6);

            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
            size_t i = 0;
            while (i < buffers->size() && (*buffers)[i].mBuffer != buffer) {
                ++i;
            }

            CHECK(i < buffers->size());
            BufferInfo *info = &buffers->editItemAt(i);

            if (info->mStatus != OWNED_BY_COMPONENT) {
                ALOGW("We already own output buffer %u, yet received "
                     "a FILL_BUFFER_DONE.", buffer);
            }

            info->mStatus = OWNED_BY_US;

            if (mPortStatus[kPortIndexOutput] == DISABLING) {
                CODEC_LOGV("Port is disabled, freeing buffer %u", buffer);

                status_t err = freeBuffer(kPortIndexOutput, i);
                CHECK_EQ(err, (status_t)OK);

#if 0
            } else if (mPortStatus[kPortIndexOutput] == ENABLED
                       && (flags & OMX_BUFFERFLAG_EOS)) {
                CODEC_LOGV("No more output data.");
                mNoMoreOutputData = true;
                mBufferFilled.signal();
#endif
            } else if (mPortStatus[kPortIndexOutput] != SHUTTING_DOWN) {
                CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)ENABLED);

                MediaBuffer *buffer = info->mMediaBuffer;
                bool isGraphicBuffer = buffer->graphicBuffer() != NULL;

                if (!isGraphicBuffer
                    && msg.u.extended_buffer_data.range_offset
                        + msg.u.extended_buffer_data.range_length
                            > buffer->size()) {
                    CODEC_LOGE(
                            "Codec lied about its buffer size requirements, "
                            "sending a buffer larger than the originally "
                            "advertised size in FILL_BUFFER_DONE!");
                }
                buffer->set_range(
                        msg.u.extended_buffer_data.range_offset,
                        msg.u.extended_buffer_data.range_length);

                buffer->meta_data()->clear();

                buffer->meta_data()->setInt64(
                        kKeyTime, msg.u.extended_buffer_data.timestamp);
#ifdef MTK_AOSP_ENHANCEMENT
                if (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.AMR"))
                {
                    int64_t lduration = (msg.u.extended_buffer_data.range_length >> 5) * 20000;
                    //ALOGD("encode offset=%d,length=%d,lduration=%lli",msg.u.extended_buffer_data.range_offset,msg.u.extended_buffer_data.range_length,lduration);
                    buffer->meta_data()->setInt64(kKeyDuration, lduration);
                }
#endif //MTK_AOSP_ENHANCEMENT
                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_SYNCFRAME) {
                    buffer->meta_data()->setInt32(kKeyIsSyncFrame, true);
                }
                bool isCodecSpecific = false;
                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_CODECCONFIG) {
                    buffer->meta_data()->setInt32(kKeyIsCodecConfig, true);
                    isCodecSpecific = true;
                }
#ifdef MTK_AOSP_ENHANCEMENT
#if 0//def MTK_S3D_SUPPORT
                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_3D_SIDEBYSIDE)
                {
                    buffer->meta_data()->setInt32(kKeyVideoStereoMode, 2);
                }
                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_3D_TOPANDBOTTOM)
                {
                    buffer->meta_data()->setInt32(kKeyVideoStereoMode, 3);
                }
#endif
#endif //MTK_AOSP_ENHANCEMENT
                if (isGraphicBuffer || mQuirks & kOutputBuffersAreUnreadable)
                {
                    buffer->meta_data()->setInt32(kKeyIsUnreadable, true);
                }

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CLEARMOTION_SUPPORT
                if (mIsVideoDecoder)
                {
                    if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_INTERPOLATE_FRAME)
                    {
                        buffer->meta_data()->setInt32(kKeyInterpolateFrame, true);
                    }
                    else
                    {
                        buffer->meta_data()->setInt32(kKeyInterpolateFrame, false);
                    }
                }
#endif
#endif //MTK_AOSP_ENHANCEMENT

                buffer->meta_data()->setInt32(
                        kKeyBufferID,
                        msg.u.extended_buffer_data.buffer);

                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_EOS) {
                    CODEC_LOGV("No more output data.");
                    mNoMoreOutputData = true;
#ifdef MTK_AOSP_ENHANCEMENT   //<---for omx component, the buffer flag is EOS, and buffer is empty.
                    ALOGD("OMXCodec::on_message EOS received!!!!");
                    mTargetTimeUs = -1;
                    if (0 == msg.u.extended_buffer_data.range_length)
                    {
                        mBufferFilled.signal();
                        break;
                    }
#endif //--->
                }

                if (mIsEncoder && mIsVideo) {
                    int64_t decodingTimeUs = isCodecSpecific? 0: getDecodingTimeUs();
                    buffer->meta_data()->setInt64(kKeyDecodingTime, decodingTimeUs);
                }

                if (mTargetTimeUs >= 0) {
#ifdef MTK_AOSP_ENHANCEMENT
                    int64_t preRollDuration = systemTime() / 1000 - mPreRollStartTime;
                    ALOGD("Key time=%lld, Target time=%lld, Preroll time=%lld, Preroll duration=%lld",
                          msg.u.extended_buffer_data.timestamp, (long long)mTargetTimeUs, (long long)mPreRollStartTime, (long long)preRollDuration);
                    if ((msg.u.extended_buffer_data.timestamp < mTargetTimeUs) && (preRollDuration < kPreRollTimeOutUs)) {
#else
                    CHECK(msg.u.extended_buffer_data.timestamp <= mTargetTimeUs);

                    if (msg.u.extended_buffer_data.timestamp < mTargetTimeUs) {
#endif //MTK_AOSP_ENHANCEMENT
                        CODEC_LOGV(
                                "skipping output buffer at timestamp %lld us",
                                msg.u.extended_buffer_data.timestamp);

                        fillOutputBuffer(info);
                        break;
                    }

                    CODEC_LOGV(
                            "returning output buffer at target timestamp "
                            "%lld us",
                            msg.u.extended_buffer_data.timestamp);
#ifdef MTK_AOSP_ENHANCEMENT
                    mPreRollStartTime = -1;
#endif //MTK_AOSP_ENHANCEMENT
                    mTargetTimeUs = -1;
                }

                mFilledBuffers.push_back(i);
                mBufferFilled.signal();
                if (mIsEncoder) {
                    sched_yield();
                }
            }

            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }
}

// Has the format changed in any way that the client would have to be aware of?
static bool formatHasNotablyChanged(
        const sp<MetaData> &from, const sp<MetaData> &to) {
    if (from.get() == NULL && to.get() == NULL) {
        return false;
    }

    if ((from.get() == NULL && to.get() != NULL)
        || (from.get() != NULL && to.get() == NULL)) {
        return true;
    }

    const char *mime_from, *mime_to;
    CHECK(from->findCString(kKeyMIMEType, &mime_from));
    CHECK(to->findCString(kKeyMIMEType, &mime_to));

    if (strcasecmp(mime_from, mime_to)) {
        return true;
    }

    if (!strcasecmp(mime_from, MEDIA_MIMETYPE_VIDEO_RAW)) {
        int32_t colorFormat_from, colorFormat_to;
        CHECK(from->findInt32(kKeyColorFormat, &colorFormat_from));
        CHECK(to->findInt32(kKeyColorFormat, &colorFormat_to));

        if (colorFormat_from != colorFormat_to) {
            return true;
        }

        int32_t width_from, width_to;
        CHECK(from->findInt32(kKeyWidth, &width_from));
        CHECK(to->findInt32(kKeyWidth, &width_to));

        if (width_from != width_to) {
            return true;
        }

        int32_t height_from, height_to;
        CHECK(from->findInt32(kKeyHeight, &height_from));
        CHECK(to->findInt32(kKeyHeight, &height_to));

        if (height_from != height_to) {
            return true;
        }

        int32_t left_from, top_from, right_from, bottom_from;
        CHECK(from->findRect(
                    kKeyCropRect,
                    &left_from, &top_from, &right_from, &bottom_from));

        int32_t left_to, top_to, right_to, bottom_to;
        CHECK(to->findRect(
                    kKeyCropRect,
                    &left_to, &top_to, &right_to, &bottom_to));

        if (left_to != left_from || top_to != top_from
                || right_to != right_from || bottom_to != bottom_from) {
            return true;
        }
    } else if (!strcasecmp(mime_from, MEDIA_MIMETYPE_AUDIO_RAW)) {
        int32_t numChannels_from, numChannels_to;
        CHECK(from->findInt32(kKeyChannelCount, &numChannels_from));
        CHECK(to->findInt32(kKeyChannelCount, &numChannels_to));

        if (numChannels_from != numChannels_to) {
            return true;
        }

        int32_t sampleRate_from, sampleRate_to;
        CHECK(from->findInt32(kKeySampleRate, &sampleRate_from));
        CHECK(to->findInt32(kKeySampleRate, &sampleRate_to));

        if (sampleRate_from != sampleRate_to) {
            return true;
        }
    }

    return false;
}

void OMXCodec::onEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch ((int)event) {
        case OMX_EventCmdComplete:
        {
            onCmdComplete((OMX_COMMANDTYPE)data1, data2);
            break;
        }

        case OMX_EventError:
        {
            CODEC_LOGE("OMX_EventError(0x%08x, %u)", data1, data2);
#ifdef MTK_AOSP_ENHANCEMENT
            if ((OMX_S32)data1 == OMX_ErrorStreamCorrupt)
            {
                ALOGW("OMXCodec::onEvent--OMX Error Stream Corrupt!!");
#ifdef MTK_AUDIO_APE_SUPPORT
                ///for ape error state to exit playback start.
                if (data2 == OMX_AUDIO_CodingAPE)
                {
                    setState(ERROR);
                }
                ///for ape error state to exit playback end.
#endif //MTK_AUDIO_APE_SUPPORT
                if (mIsVideoEncoder)
                {
                    ALOGW("OMXCodec::onEvent--Video encoder error");
                    mFinalStatus = ERROR_UNSUPPORTED_VIDEO;
                    setState(ERROR);
                }
            }
            else
            {
                if (mIsVideoDecoder && (OMX_S32)data1 == OMX_ErrorBadParameter)
                {
                    ALOGW("OMXCodec::onEvent--OMX Bad Parameter!!");
                    mFinalStatus = ERROR_UNSUPPORTED_VIDEO;
                }
                if (!mIsEncoder && !mIsVideoDecoder && (OMX_S32)data1 == OMX_ErrorBadParameter)
                {
                    ALOGW("OMXCodec::onEvent--Audio OMX Bad Parameter!!");
                    mFinalStatus = ERROR_UNSUPPORTED_AUDIO;
                }
                setState(ERROR);
            }
#else
            setState(ERROR);
#endif //MTK_AOSP_ENHANCEMENT
            break;
        }

        case OMX_EventPortSettingsChanged:
        {
            CODEC_LOGD("OMX_EventPortSettingsChanged(port=%u, data2=0x%08x)",
                       data1, data2);

#ifdef MTK_AOSP_ENHANCEMENT
            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition || data2 == OMX_IndexVendorMtkOmxVdecGetAspectRatio
                || data2 == OMX_IndexVendorMtkOmxVdecGetCropInfo)
            {
#else
            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition)
            {
#endif //MTK_AOSP_ENHANCEMENT

                // There is no need to check whether mFilledBuffers is empty or not
                // when the OMX_EventPortSettingsChanged is not meant for reallocating
                // the output buffers.

#ifdef MTK_AOSP_ENHANCEMENT
                if (data1 == kPortIndexOutput
                && data2 != OMX_IndexVendorMtkOmxVdecGetAspectRatio
                && data2 != OMX_IndexVendorMtkOmxVdecGetCropInfo) {
                    mFilledBuffers.clear();
                }

                if (mState == EXECUTING_TO_IDLE) { //Bruce, do nothing after stop
                    ALOGE("Get port_setting_changed_event after stop!");
                    break;
                }

                if (data2 != OMX_IndexVendorMtkOmxVdecGetCropInfo) {
                    onPortSettingsChanged(data1);
                }
#else
                onPortSettingsChanged(data1);
#endif //MTK_AOSP_ENHANCEMENT

#ifdef MTK_AOSP_ENHANCEMENT
                if (data2 == OMX_IndexVendorMtkOmxVdecGetAspectRatio) {
                    ALOGE("@@ GOT OMX_IndexVendorMtkOmxVdecGetAspectRatio");
                    OMX_S32 aspectRatio = 0;
                    if (OK == mOMX->getConfig(mNode, OMX_IndexVendorMtkOmxVdecGetAspectRatio, &aspectRatio, sizeof(aspectRatio))) {
                        ALOGE("@@ AspectRatioWidth (%d), AspectRatioHeight(%d)", (aspectRatio & 0xFFFF0000) >> 16, (aspectRatio & 0x0000FFFF));
                        mVideoAspectRatioWidth = ((aspectRatio & 0xFFFF0000) >> 16);
                        mVideoAspectRatioHeight = (aspectRatio & 0x0000FFFF);
                    }
                }
                if (data2 == OMX_IndexVendorMtkOmxVdecGetCropInfo) {
                    OMX_CONFIG_RECTTYPE rect;
                    InitOMXParams(&rect);
                    rect.nPortIndex = kPortIndexOutput;
                    status_t err =
                        mOMX->getConfig(
                            mNode, OMX_IndexVendorMtkOmxVdecGetCropInfo,
                            &rect, sizeof(rect));

                    if (err == OK) {
                        CHECK_GE(rect.nLeft, 0);
                        CHECK_GE(rect.nTop, 0);
                        CHECK_GE(rect.nWidth, 0u);
                        CHECK_GE(rect.nHeight, 0u);

                        CODEC_LOGI(
                            "Set CropInfo: Crop rect is %d x %d @ (%d, %d)",
                            rect.nWidth, rect.nHeight, rect.nLeft, rect.nTop);
                    }

                    if (err == OK && mNativeWindow != NULL) {
                        android_native_rect_t crop;

                        crop.left = rect.nLeft;
                        crop.top = rect.nTop;
                        crop.right = rect.nLeft + rect.nWidth;
                        crop.bottom = rect.nTop + rect.nHeight;

                        CODEC_LOGI("Set native window crop.left %d, crop.top %d, crop.right %d, crop.bottom %d", crop.left, crop.top, crop.right, crop.bottom);
                        native_window_set_crop(mNativeWindow.get(), &crop);
                    }
                }
#endif //MTK_AOSP_ENHANCEMENT
            }
            else if (data1 == kPortIndexOutput &&
                        (data2 == OMX_IndexConfigCommonOutputCrop ||
                         data2 == OMX_IndexConfigCommonScale)) {

                sp<MetaData> oldOutputFormat = mOutputFormat;
#ifdef MTK_AOSP_ENHANCEMENT
                if (OK != initOutputFormat(mSource->getFormat())){
                    setState(ERROR);
                }
#else
                initOutputFormat(mSource->getFormat());
#endif

                if (data2 == OMX_IndexConfigCommonOutputCrop &&
                    formatHasNotablyChanged(oldOutputFormat, mOutputFormat)) {
                    mOutputPortSettingsHaveChanged = true;

                } else if (data2 == OMX_IndexConfigCommonScale) {
                    OMX_CONFIG_SCALEFACTORTYPE scale;
                    InitOMXParams(&scale);
                    scale.nPortIndex = kPortIndexOutput;

                    // Change display dimension only when necessary.
                    if (OK == mOMX->getConfig(
                                        mNode,
                                        OMX_IndexConfigCommonScale,
                                        &scale, sizeof(scale))) {
                        int32_t left, top, right, bottom;
                        CHECK(mOutputFormat->findRect(kKeyCropRect,
                                                      &left, &top,
                                                      &right, &bottom));

                        // The scale is in 16.16 format.
                        // scale 1.0 = 0x010000. When there is no
                        // need to change the display, skip it.
                        ALOGV("Get OMX_IndexConfigScale: 0x%x/0x%x",
                                scale.xWidth, scale.xHeight);

                        if (scale.xWidth != 0x010000) {
                            mOutputFormat->setInt32(kKeyDisplayWidth,
                                    ((right - left +  1) * scale.xWidth)  >> 16);
                            mOutputPortSettingsHaveChanged = true;
                        }

                        if (scale.xHeight != 0x010000) {
                            mOutputFormat->setInt32(kKeyDisplayHeight,
                                    ((bottom  - top + 1) * scale.xHeight) >> 16);
                            mOutputPortSettingsHaveChanged = true;
                        }
                    }
                }
            }
#ifdef MTK_DEINTERLACE_SUPPORT
            else if (data2 == OMX_IndexVendMtkOmxUpdateColorFormat)
            {
#ifdef MTK_AOSP_ENHANCEMENT
                if (data1 == kPortIndexOutput)
                {
                    OMX_COLOR_FORMATTYPE colorFormat;
                    status_t err = mOMX->getParameter(
                                       mNode, OMX_IndexVendorMtkOmxVdecGetColorFormat,
                                       &colorFormat, sizeof(colorFormat));
                    mOutputFormat->setInt32(kKeyColorFormat, colorFormat);
                    if (err != OK){
                        CODEC_LOGW("Failed to get OMX_IndexVendorMtkOmxVdecGetColorFormat %d", err);
                    }
                }
#endif
            }
#endif
            break;
        }
#ifdef MTK_AOSP_ENHANCEMENT
        case OMX_EventComponentInfoNotified:
        {
            if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.AAC"))
            {
                mOutputFormat->setInt32(kKeyAacProfile, getAACProfile());
                CODEC_LOGI("OMX Notify AAC Profile %d", getAACProfile());
            }
        }
#endif //MTK_AOSP_ENHANCEMENT

#if 0
        case OMX_EventBufferFlag:
        {
            CODEC_LOGV("EVENT_BUFFER_FLAG(%ld)", data1);

            if (data1 == kPortIndexOutput) {
                mNoMoreOutputData = true;
            }
            break;
        }
#endif

#ifdef MTK_AUDIO_DDPLUS_SUPPORT
        case OMX_EventDolbyProcessedAudio:
        {
            mDolbyProcessedAudio = data1;
            mDolbyProcessedAudioStateChanged = true;
            break;
        }
#endif // DOLBY_END
        default:
        {
            CODEC_LOGV("EVENT(%d, %u, %u)", event, data1, data2);
            break;
        }
    }
}

void OMXCodec::onCmdComplete(OMX_COMMANDTYPE cmd, OMX_U32 data) {
    switch (cmd) {
        case OMX_CommandStateSet:
        {
            onStateChange((OMX_STATETYPE)data);
            break;
        }

        case OMX_CommandPortDisable:
        {
            OMX_U32 portIndex = data;
            CODEC_LOGV("PORT_DISABLED(%u)", portIndex);

            CHECK(mState == EXECUTING || mState == RECONFIGURING);
            CHECK_EQ((int)mPortStatus[portIndex], (int)DISABLING);
            CHECK_EQ(mPortBuffers[portIndex].size(), 0u);

            mPortStatus[portIndex] = DISABLED;

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                sp<MetaData> oldOutputFormat = mOutputFormat;
#ifdef MTK_AOSP_ENHANCEMENT
                if (OK != initOutputFormat(mSource->getFormat())){
                    setState(ERROR);
                }
#else
                initOutputFormat(mSource->getFormat());
#endif

                // Don't notify clients if the output port settings change
                // wasn't of importance to them, i.e. it may be that just the
                // number of buffers has changed and nothing else.
                bool formatChanged = formatHasNotablyChanged(oldOutputFormat, mOutputFormat);
                if (!mOutputPortSettingsHaveChanged) {
#ifdef OMX_VE_AUDIO
                    if (mFlags & kMtkAudDecForVE)
                    {
                        mOutputPortSettingsHaveChanged = true;
                    }
                    else
#endif //OMX_VE_AUDIO
                    mOutputPortSettingsHaveChanged = formatChanged;
                }

                status_t err = enablePortAsync(portIndex);
                if (err != OK) {
                    CODEC_LOGE("enablePortAsync(%u) failed (err = %d)", portIndex, err);
                    setState(ERROR);
                } else {
                    err = allocateBuffersOnPort(portIndex);
                    if (err != OK) {
                        CODEC_LOGE("allocateBuffersOnPort (%s) failed "
                                   "(err = %d)",
                                   portIndex == kPortIndexInput
                                        ? "input" : "output",
                                   err);

                        setState(ERROR);
                    }
#ifdef MTK_AOSP_ENHANCEMENT
                    OMX_CONFIG_RECTTYPE rect;
                    memset(&rect, 0, sizeof(OMX_CONFIG_RECTTYPE));
                    InitOMXParams(&rect);
                    rect.nPortIndex = kPortIndexOutput;
                    err = mOMX->getConfig(
                              mNode, OMX_IndexVendorMtkOmxVdecGetCropInfo,
                              &rect, sizeof(rect));

                    if (err == OK)
                    {
                        CHECK_GE(rect.nLeft, 0);
                        CHECK_GE(rect.nTop, 0);
                        CHECK_GE(rect.nWidth, 0u);
                        CHECK_GE(rect.nHeight, 0u);

                        CODEC_LOGI(
                            "Set CropInfo: Crop rect is %d x %d @ (%d, %d)",
                            rect.nWidth, rect.nHeight, rect.nLeft, rect.nTop);
                    }

                    if (err == OK && mNativeWindow != NULL)
                    {
                        android_native_rect_t crop;

                        crop.left = rect.nLeft;
                        crop.top = rect.nTop;
                        crop.right = rect.nLeft + rect.nWidth;
                        crop.bottom = rect.nTop + rect.nHeight;

                        CODEC_LOGI("Set native window crop.left %d, crop.top %d, crop.right %d, crop.bottom %d", crop.left, crop.top, crop.right, crop.bottom);
                        native_window_set_crop(mNativeWindow.get(), &crop);
                    }
#endif //MTK_AOSP_ENHANCEMENT
                }
            }
            break;
        }

        case OMX_CommandPortEnable:
        {
            OMX_U32 portIndex = data;
            CODEC_LOGV("PORT_ENABLED(%u)", portIndex);

            CHECK(mState == EXECUTING || mState == RECONFIGURING);
            CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLING);

            mPortStatus[portIndex] = ENABLED;

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                setState(EXECUTING);

                fillOutputBuffers();
            }
            break;
        }

        case OMX_CommandFlush:
        {
            OMX_U32 portIndex = data;

            CODEC_LOGV("FLUSH_DONE(%u)", portIndex);

            CHECK_EQ((int)mPortStatus[portIndex], (int)SHUTTING_DOWN);
            mPortStatus[portIndex] = ENABLED;

            CHECK_EQ(countBuffersWeOwn(mPortBuffers[portIndex]),
                     mPortBuffers[portIndex].size());

            if (mSkipCutBuffer != NULL && mPortStatus[kPortIndexOutput] == ENABLED) {
                mSkipCutBuffer->clear();
            }

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                disablePortAsync(portIndex);
            } else if (mState == EXECUTING_TO_IDLE) {
                if (mPortStatus[kPortIndexInput] == ENABLED
                    && mPortStatus[kPortIndexOutput] == ENABLED) {
                    CODEC_LOGV("Finished flushing both ports, now completing "
                         "transition from EXECUTING to IDLE.");

                    mPortStatus[kPortIndexInput] = SHUTTING_DOWN;
                    mPortStatus[kPortIndexOutput] = SHUTTING_DOWN;

                    status_t err =
                        mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
                    CHECK_EQ(err, (status_t)OK);
                }
            } else {
                // We're flushing both ports in preparation for seeking.

                if (mPortStatus[kPortIndexInput] == ENABLED
                    && mPortStatus[kPortIndexOutput] == ENABLED) {
                    CODEC_LOGV("Finished flushing both ports, now continuing from"
                         " seek-time.");

#ifdef MTK_AOSP_ENHANCEMENT
                    if ((mState == ERROR) && (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.APE")))
                    {
                        ALOGE("Dropping complete - we're in APE ERROR state.");
                        return;
                    }
                    if ((mState == ERROR) && (!strcmp(mComponentName, "OMX.MTK.VIDEO.DECODER.AVC")))
                    {
                        ALOGE("Dropping complete - we're in AVC ERROR state.");
                        return;
                    }
#endif //MTK_AOSP_ENHANCEMENT
                    // We implicitly resume pulling on our upstream source.
                    mPaused = false;

                    drainInputBuffers();
                    fillOutputBuffers();
                }

                if (mOutputPortSettingsChangedPending) {
                    CODEC_LOGV(
                            "Honoring deferred output port settings change.");

                    mOutputPortSettingsChangedPending = false;
                    onPortSettingsChanged(kPortIndexOutput);
                }
            }

            break;
        }

        default:
        {
            CODEC_LOGV("CMD_COMPLETE(%d, %u)", cmd, data);
            break;
        }
    }
}

void OMXCodec::onStateChange(OMX_STATETYPE newState) {
    CODEC_LOGV("onStateChange %d", newState);

    switch (newState) {
        case OMX_StateIdle:
        {
            CODEC_LOGV("Now Idle.");
            if (mState == LOADED_TO_IDLE) {
                status_t err = mOMX->sendCommand(
                        mNode, OMX_CommandStateSet, OMX_StateExecuting);

                CHECK_EQ(err, (status_t)OK);

                setState(IDLE_TO_EXECUTING);
            } else {
#ifdef MTK_AOSP_ENHANCEMENT // modify Android 4.0 default code , Legis.
                if (mState != ERROR)
                {
#endif //MTK_AOSP_ENHANCEMENT
                    CHECK_EQ((int)mState, (int)EXECUTING_TO_IDLE);
#ifdef MTK_AOSP_ENHANCEMENT // modify Android 4.0 default code , Legis.
                }
#endif //MTK_AOSP_ENHANCEMENT

                if (countBuffersWeOwn(mPortBuffers[kPortIndexInput]) !=
                    mPortBuffers[kPortIndexInput].size()) {
                    ALOGE("Codec did not return all input buffers "
                          "(received %zu / %zu)",
                            countBuffersWeOwn(mPortBuffers[kPortIndexInput]),
                            mPortBuffers[kPortIndexInput].size());
#ifdef MTK_AOSP_ENHANCEMENT
                    dumpBufferOwner(mPortBuffers[kPortIndexInput]);
#endif
                    TRESPASS();
                }

#ifdef MTK_AOSP_ENHANCEMENT
                waitClientBuffers(mPortBuffers[kPortIndexInput]) ;
#endif
                if (countBuffersWeOwn(mPortBuffers[kPortIndexOutput]) !=
                    mPortBuffers[kPortIndexOutput].size()) {
                    ALOGE("Codec did not return all output buffers "
                          "(received %zu / %zu)",
                            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]),
                            mPortBuffers[kPortIndexOutput].size());
#ifdef MTK_AOSP_ENHANCEMENT
                    dumpBufferOwner(mPortBuffers[kPortIndexOutput]);
#endif
                    TRESPASS();
                }
#ifdef MTK_AOSP_ENHANCEMENT
                waitClientBuffers(mPortBuffers[kPortIndexOutput]) ;
#endif

                status_t err = mOMX->sendCommand(
                        mNode, OMX_CommandStateSet, OMX_StateLoaded);

                CHECK_EQ(err, (status_t)OK);

                err = freeBuffersOnPort(kPortIndexInput);
                CHECK_EQ(err, (status_t)OK);

                err = freeBuffersOnPort(kPortIndexOutput);
                CHECK_EQ(err, (status_t)OK);

                mPortStatus[kPortIndexInput] = ENABLED;
                mPortStatus[kPortIndexOutput] = ENABLED;

                if ((mFlags & kEnableGrallocUsageProtected) &&
                        mNativeWindow != NULL) {
                    // We push enough 1x1 blank buffers to ensure that one of
                    // them has made it to the display.  This allows the OMX
                    // component teardown to zero out any protected buffers
                    // without the risk of scanning out one of those buffers.
                    pushBlankBuffersToNativeWindow(mNativeWindow.get());
                }

                setState(IDLE_TO_LOADED);
            }
            break;
        }

        case OMX_StateExecuting:
        {
            CHECK_EQ((int)mState, (int)IDLE_TO_EXECUTING);

            CODEC_LOGV("Now Executing.");

            mOutputPortSettingsChangedPending = false;

            setState(EXECUTING);

            // Buffers will be submitted to the component in the first
            // call to OMXCodec::read as mInitialBufferSubmit is true at
            // this point. This ensures that this on_message call returns,
            // releases the lock and ::init can notice the state change and
            // itself return.
            break;
        }

        case OMX_StateLoaded:
        {
            CHECK_EQ((int)mState, (int)IDLE_TO_LOADED);

            CODEC_LOGV("Now Loaded.");

            setState(LOADED);
            break;
        }

        case OMX_StateInvalid:
        {
            setState(ERROR);
            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }
}

// static
size_t OMXCodec::countBuffersWeOwn(const Vector<BufferInfo> &buffers) {
    size_t n = 0;
    for (size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i].mStatus != OWNED_BY_COMPONENT) {
            ++n;
        }
    }

    return n;
}

#ifdef MTK_AOSP_ENHANCEMENT
void OMXCodec::waitClientBuffers(const Vector<BufferInfo> &buffers)
{
    #define WAIT_LIMIT_US  (200000)
    #define PER_WAIT_US (5000)
    #define WAIT_TIMES (WAIT_LIMIT_US/PER_WAIT_US)

    size_t n = 0;
    size_t i = 0;
    while(i < buffers.size()){
        if (buffers[i].mStatus == OWNED_BY_CLIENT)
        {
            if (n > WAIT_TIMES){
                ALOGE("Client did not return buffer %zu for %d ms", i, WAIT_LIMIT_US/1000);
                dumpBufferOwner(buffers);
                //Intentionally trigger NE
                TRESPASS();
            }

            ALOGD("Waiting for Clirent returning buffer %zu", i);

            // 20150401 Marcus Huang:
            //  Here we release the lock acquired by on_message() to make OMXCodec::signalBufferReturned() able to acquire the lock.
            //  Then it can return buffer from client to us. Besides, we should not keep the lock during sleeping.
            mLock.unlock();
            usleep(5000);
            mLock.lock();

            ++n;
        }else{
            n = 0;
            ++i;
        }
    };
}

void OMXCodec::dumpBufferOwner(const Vector<BufferInfo> &buffers)
{
    //size_t n = 0;
    ALOGD("%s ++", __func__);
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        ALOGD("buffers[i].mStatus: %d, mBuffer %x, mMediaBuffer %p",
            buffers[i].mStatus, buffers[i].mBuffer, buffers[i].mMediaBuffer);
    }
    ALOGD("%s --", __func__);
}
#endif

status_t OMXCodec::freeBuffersOnPort(
        OMX_U32 portIndex, bool onlyThoseWeOwn) {
    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    status_t stickyErr = OK;

    for (size_t i = buffers->size(); i-- > 0;) {
        BufferInfo *info = &buffers->editItemAt(i);

#ifdef MTK_AOSP_ENHANCEMENT
        if (onlyThoseWeOwn && (info->mStatus == OWNED_BY_COMPONENT || info->mStatus == OWNED_BY_CLIENT))
        {
#else
        if (onlyThoseWeOwn && info->mStatus == OWNED_BY_COMPONENT)
        {
#endif //MTK_AOSP_ENHANCEMENT
            continue;
        }

        CHECK(info->mStatus == OWNED_BY_US
                || info->mStatus == OWNED_BY_NATIVE_WINDOW);

        CODEC_LOGD("freeing buffer %u on port %u", info->mBuffer, portIndex);

        status_t err = freeBuffer(portIndex, i);

        if (err != OK) {
            stickyErr = err;
        }

    }
    CODEC_LOGD("freeBuffersOnPort onlyThoseWeOwn %d,  buffers->isEmpty() %d", onlyThoseWeOwn,  buffers->isEmpty());

    CHECK(onlyThoseWeOwn || buffers->isEmpty());

    return stickyErr;
}

status_t OMXCodec::freeBuffer(OMX_U32 portIndex, size_t bufIndex) {
    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    BufferInfo *info = &buffers->editItemAt(bufIndex);

    status_t err = mOMX->freeBuffer(mNode, portIndex, info->mBuffer);

    if (err == OK && info->mMediaBuffer != NULL) {
        CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);
        info->mMediaBuffer->setObserver(NULL);

        // Make sure nobody but us owns this buffer at this point.
        CHECK_EQ(info->mMediaBuffer->refcount(), 0);

        // Cancel the buffer if it belongs to an ANativeWindow.
        sp<GraphicBuffer> graphicBuffer = info->mMediaBuffer->graphicBuffer();
        if (info->mStatus == OWNED_BY_US && graphicBuffer != 0) {
            err = cancelBufferToNativeWindow(info);
        }

        info->mMediaBuffer->release();
        info->mMediaBuffer = NULL;
    }

    if (err == OK) {
        buffers->removeAt(bufIndex);
    }

    return err;
}

void OMXCodec::onPortSettingsChanged(OMX_U32 portIndex) {
    CODEC_LOGV("PORT_SETTINGS_CHANGED(%u)", portIndex);

    CHECK(mState == EXECUTING || mState == EXECUTING_TO_IDLE);
    CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);
    CHECK(!mOutputPortSettingsChangedPending);

    if (mPortStatus[kPortIndexOutput] != ENABLED) {
        CODEC_LOGV("Deferring output port settings change.");
        mOutputPortSettingsChangedPending = true;
        return;
    }

    setState(RECONFIGURING);

    if (mQuirks & kNeedsFlushBeforeDisable) {
        if (!flushPortAsync(portIndex)) {
            onCmdComplete(OMX_CommandFlush, portIndex);
        }
    } else {
        disablePortAsync(portIndex);
    }
}

bool OMXCodec::flushPortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING
            || mState == EXECUTING_TO_IDLE);

    CODEC_LOGV("flushPortAsync(%u): we own %zu out of %zu buffers already.",
         portIndex, countBuffersWeOwn(mPortBuffers[portIndex]),
         mPortBuffers[portIndex].size());

    CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLED);
    mPortStatus[portIndex] = SHUTTING_DOWN;

#ifdef MTK_AOSP_ENHANCEMENT
    if (mQueueWaiting && kPortIndexOutput == portIndex)
    {
        mBufferSent.signal();
    }
#endif //MTK_AOSP_ENHANCEMENT

    if ((mQuirks & kRequiresFlushCompleteEmulation)
        && countBuffersWeOwn(mPortBuffers[portIndex])
                == mPortBuffers[portIndex].size()) {
        // No flush is necessary and this component fails to send a
        // flush-complete event in this case.

        return false;
    }

    status_t err =
        mOMX->sendCommand(mNode, OMX_CommandFlush, portIndex);
    CHECK_EQ(err, (status_t)OK);

    return true;
}

void OMXCodec::disablePortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING);

    CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLED);
    mPortStatus[portIndex] = DISABLING;

    CODEC_LOGV("sending OMX_CommandPortDisable(%u)", portIndex);
    status_t err =
        mOMX->sendCommand(mNode, OMX_CommandPortDisable, portIndex);
    CHECK_EQ(err, (status_t)OK);

    freeBuffersOnPort(portIndex, true);
}

status_t OMXCodec::enablePortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING);

    CHECK_EQ((int)mPortStatus[portIndex], (int)DISABLED);
    mPortStatus[portIndex] = ENABLING;

    CODEC_LOGV("sending OMX_CommandPortEnable(%u)", portIndex);
    return mOMX->sendCommand(mNode, OMX_CommandPortEnable, portIndex);
}

void OMXCodec::fillOutputBuffers() {
    CHECK_EQ((int)mState, (int)EXECUTING);

    // This is a workaround for some decoders not properly reporting
    // end-of-output-stream. If we own all input buffers and also own
    // all output buffers and we already signalled end-of-input-stream,
    // the end-of-output-stream is implied.
    if (mSignalledEOS
            && countBuffersWeOwn(mPortBuffers[kPortIndexInput])
                == mPortBuffers[kPortIndexInput].size()
            && countBuffersWeOwn(mPortBuffers[kPortIndexOutput])
                == mPortBuffers[kPortIndexOutput].size()) {
        mNoMoreOutputData = true;
        mBufferFilled.signal();

        return;
    }

    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);
        if (info->mStatus == OWNED_BY_US) {
            fillOutputBuffer(&buffers->editItemAt(i));
        }
    }
}

void OMXCodec::drainInputBuffers() {
    CHECK(mState == EXECUTING || mState == RECONFIGURING);

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CMMB_ENABLE
    if (true == IsCMMBFlag)
    {
        uint32_t buffersize; //CMMB
        Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
        ALOGE("OMXCodec::drainInputBuffers buffer pool size = %d", buffers->size());
        if ((buffers->size() > 3))
        {
            buffersize = 3;
        }
        else
        {
            buffersize = buffers->size();
        }
        for (size_t i = 0; i < buffersize; ++i)
        {
            drainInputBuffer(&buffers->editItemAt(i));
        }
    }
    else{
#endif
#endif //MTK_AOSP_ENHANCEMENT
    {
        if (mFlags & kUseSecureInputBuffers) {
            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            for (size_t i = 0; i < buffers->size(); ++i) {
#ifdef MTK_AOSP_ENHANCEMENT
                if (!drainAnyInputBuffer(true)
#else
            if (!drainAnyInputBuffer()
#endif //MTK_AOSP_ENHANCEMENT
                    || (mFlags & kOnlySubmitOneInputBufferAtOneTime)) {
                    break;
                }
            }
        } else {
            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            for (size_t i = 0; i < buffers->size(); ++i) {
                BufferInfo *info = &buffers->editItemAt(i);

                if (info->mStatus != OWNED_BY_US) {
                    continue;
                }

#ifdef MTK_AOSP_ENHANCEMENT
                if (!drainInputBuffer(info, true)) {
#else
                if (!drainInputBuffer(info)) {
#endif //MTK_AOSP_ENHANCEMENT
                    break;
                }

                if (mFlags & kOnlySubmitOneInputBufferAtOneTime) {
                    break;
                }
            }
        }
    }
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_CMMB_ENABLE
    }
#endif
#endif //MTK_AOSP_ENHANCEMENT
}

#ifdef MTK_AOSP_ENHANCEMENT
bool OMXCodec::drainAnyInputBuffer(bool init)
{
    return drainInputBuffer((BufferInfo *)NULL, init);
}
#else //MTK_AOSP_ENHANCEMENT
bool OMXCodec::drainAnyInputBuffer()
{
    return drainInputBuffer((BufferInfo *)NULL);
}
#endif //MTK_AOSP_ENHANCEMENT

#ifdef MTK_AOSP_ENHANCEMENT
void OMXCodec::PutErrorPatterns(uint8_t *pBuffer, uint32_t length)
{
    int error_count = 0;
    int _RAND_LIMIT = 32768;
    srand(time(0));
    for (uint32_t i = 0 ; i < length ; i++)
    {
        int error_mask = 0;
        float rand_num;
        for (int j = 0; j < 8; j++)
        {
            rand_num = (float)((rand() % _RAND_LIMIT) * _RAND_LIMIT + (rand() % _RAND_LIMIT)) / ((float)_RAND_LIMIT) / ((float)_RAND_LIMIT);

            if (rand_num > 1)
            {
                CHECK(false);    // assert
            }

            if (rand_num < mVideoInputErrorRate)
            {
                error_count++;
            }

            error_mask += (rand_num < mVideoInputErrorRate);
            error_mask <<= 1;
        }

        pBuffer[i] ^= (uint8_t) error_mask;
    }

    //LOGD ("target_error_rate = %f, real_error_rate = %f", mVideoInputErrorRate, (float)error_count/8/length);
}
#endif //MTK_AOSP_ENHANCEMENT

OMXCodec::BufferInfo *OMXCodec::findInputBufferByDataPointer(void *ptr) {
    Vector<BufferInfo> *infos = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < infos->size(); ++i) {
        BufferInfo *info = &infos->editItemAt(i);

        if (info->mData == ptr) {
            CODEC_LOGV(
                    "input buffer data ptr = %p, buffer_id = %u",
                    ptr,
                    info->mBuffer);

            return info;
        }
    }

    TRESPASS();
}

OMXCodec::BufferInfo *OMXCodec::findEmptyInputBuffer() {
    Vector<BufferInfo> *infos = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < infos->size(); ++i) {
        BufferInfo *info = &infos->editItemAt(i);

        if (info->mStatus == OWNED_BY_US) {
            return info;
        }
    }

    TRESPASS();
}

#ifdef MTK_AOSP_ENHANCEMENT
bool OMXCodec::drainInputBuffer(BufferInfo *info, bool init) {
#else //MTK_AOSP_ENHANCEMENT
bool OMXCodec::drainInputBuffer(BufferInfo *info) {
#endif //MTK_AOSP_ENHANCEMENT
    if (info != NULL) {
        CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    }

    //MTK80721 2011-08-18 vorbis encoder EOS not return
#ifdef MTK_AOSP_ENHANCEMENT
    if (mSignalledEOS && strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.VORBIS") && (mSeekTimeUs < 0))   // ALPS01640967
    {
#else
    if (mSignalledEOS)
    {
#endif //MTK_AOSP_ENHANCEMENT
        return false;
    }

    if (mCodecSpecificDataIndex < mCodecSpecificData.size()) {
        CHECK(!(mFlags & kUseSecureInputBuffers));

        const CodecSpecificData *specific =
            mCodecSpecificData[mCodecSpecificDataIndex];

        size_t size = specific->mSize;

        if ((!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mMIME) ||
             !strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mMIME))
                && !(mQuirks & kWantsNALFragments)) {
            static const uint8_t kNALStartCode[4] =
                    { 0x00, 0x00, 0x00, 0x01 };

            if (info->mSize < specific->mSize + 4) {
                ALOGE("info size %zu < specific size %zu", info->mSize, specific->mSize + 4);
                setState(ERROR);
                return false;
            }

            size += 4;

            memcpy(info->mData, kNALStartCode, 4);
            memcpy((uint8_t *)info->mData + 4,
                   specific->mData, specific->mSize);
        } else {
            if (info->mSize < specific->mSize) {
                ALOGE("info size %zu < specific size %zu", info->mSize, specific->mSize);
                setState(ERROR);
                return false;
            }
            memcpy(info->mData, specific->mData, specific->mSize);
        }

        mNoMoreOutputData = false;

        CODEC_LOGD("calling emptyBuffer with codec specific data");

        status_t err = mOMX->emptyBuffer(
                mNode, info->mBuffer, 0, size,
                OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_CODECCONFIG,
                0);
        CHECK_EQ(err, (status_t)OK);

        info->mStatus = OWNED_BY_COMPONENT;

        ++mCodecSpecificDataIndex;
        return true;
    }

    if (mPaused) {
        return false;
    }

    status_t err;

#ifdef MTK_AOSP_ENHANCEMENT
    bool isPartialFrame = false;
    bool reComputePTS = false;
#endif //MTK_AOSP_ENHANCEMENT
    bool signalEOS = false;
    int64_t timestampUs = 0;

    size_t offset = 0;
    int32_t n = 0;


    for (;;) {
        MediaBuffer *srcBuffer;
        if (mSeekTimeUs >= 0) {
            if (mLeftOverBuffer) {
                mLeftOverBuffer->release();
                mLeftOverBuffer = NULL;
            }

            MediaSource::ReadOptions options;
            options.setSeekTo(mSeekTimeUs, mSeekMode);

            mSeekTimeUs = -1;
            mSeekMode = ReadOptions::SEEK_CLOSEST_SYNC;
            mBufferFilled.signal();

            err = mSource->read(&srcBuffer, &options);

            if (err == OK) {
                int64_t targetTimeUs;
                if (srcBuffer->meta_data()->findInt64(
                            kKeyTargetTime, &targetTimeUs)
                        && targetTimeUs >= 0) {
                    CODEC_LOGV("targetTimeUs = %lld us", (long long)targetTimeUs);
                    mTargetTimeUs = targetTimeUs;
                } else {
                    mTargetTimeUs = -1;
                }
            }
        } else if (mLeftOverBuffer) {
            srcBuffer = mLeftOverBuffer;
            mLeftOverBuffer = NULL;

            err = OK;
        } else {
#ifdef MTK_AOSP_ENHANCEMENT
            if (mNoMoreOutputData)
            {
                CODEC_LOGE("Read source after no more output data");//for EOS may hang
            }
            // if we have enough output buffers, pause pulling source
            if (mMaxQueueBufferNum > 0 && !init)
            {
                // mLock must be held to get here
                while ((mFilledBuffers.size() > mMaxQueueBufferNum) &&
                       (mState == EXECUTING || mState == RECONFIGURING))
                {
                    mQueueWaiting = true;
                    status_t err = mBufferSent.waitRelative(mLock, 10000000000LL);
                    if (err == -ETIMEDOUT)
                    {
                        ALOGI("drainInputBuffer wait timeout with state %d, seek %lld",
                              mState, (long long)mSeekTimeUs);
                    }
                }
                mQueueWaiting = false;
                if (mSeekTimeUs >= 0 ||
                    (mState != EXECUTING && mState != RECONFIGURING))
                {
                    // seek/stop during waiting
                    ALOGD("drainInputBuffer break with state %d, seek %lld",
                          mState, (long long)mSeekTimeUs);
                    return false;
                }
                mLock.unlock();
                // mSource is safe here because AwesomePlayer calls stop before dtor
                err = mSource->read(&srcBuffer);
                if (err != OK && !strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.VORBIS"))
                {
                    err = ERROR_END_OF_STREAM;
                }
                mLock.lock();
            }
            else
            {
                if (mIsVideoDecoder && !mIsHttpStreaming)
                {
                    mLock.unlock();
                }
#endif
                err = mSource->read(&srcBuffer);
#ifdef MTK_AOSP_ENHANCEMENT
                if (mIsVideoDecoder && !mIsHttpStreaming)
                {
                    mLock.lock();
                }
            }
            if (err != OK && !strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.VORBIS"))
            {
                err = ERROR_END_OF_STREAM;
            }
            if (mNoMoreOutputData)
            {
                CODEC_LOGE("Read source after no more output data done, err=0x%08x", err);//for EOS may hang
            }
#endif //MTK_AOSP_ENHANCEMENT
        }

        if (err != OK) {
            signalEOS = true;
            mFinalStatus = err;
            mSignalledEOS = true;
            mBufferFilled.signal();
            break;
        }
        else if (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.VORBIS")) { }
        else
        {
            signalEOS = false;
            mSignalledEOS = false;
        }

        if (mFlags & kUseSecureInputBuffers) {
            info = findInputBufferByDataPointer(srcBuffer->data());
            CHECK(info != NULL);
        }

        size_t remainingBytes = info->mSize - offset;

        if (srcBuffer->range_length() > remainingBytes) {
#ifdef MTK_AOSP_ENHANCEMENT
            // don't fail if codec supports partial frames
            if (offset == 0 && !mSupportsPartialFrames)
            {
#else //MTK_AOSP_ENHANCEMENT
            if (offset == 0)
            {
#endif //MTK_AOSP_ENHANCEMENT
                CODEC_LOGE(
                     "Codec's input buffers are too small to accomodate "
                     "buffer read from source (info->mSize = %zu, srcLength = %zu)",
                     info->mSize, srcBuffer->range_length());

                srcBuffer->release();
                srcBuffer = NULL;

                setState(ERROR);
                return false;
            }
#ifdef MTK_AOSP_ENHANCEMENT
#if defined(MTK_AUDIO_ADPCM_SUPPORT) || defined(HAVE_ADPCMENCODE_FEATURE)

            if (!strncmp(mComponentName, "OMX.MTK.AUDIO.DECODER.ADPCM", 27))
            {
                int32_t blockAlign = 0;
                sp<MetaData> mMetaDataForADPCM;
                mMetaDataForADPCM = mSource->getFormat();
                CHECK(mMetaDataForADPCM->findInt32(kKeyBlockAlign, &blockAlign));
                ALOGD("Prepare for ADPCM Playback, blockAlign is %d", blockAlign);

                remainingBytes = blockAlign * (remainingBytes / blockAlign);
                ALOGD("remainingBytes After Align is %zu", remainingBytes);
            }
#endif
#endif //MTK_AOSP_ENHANCEMENT

#ifdef MTK_AOSP_ENHANCEMENT
            if (offset != 0)
            {
                mLeftOverBuffer = srcBuffer;
                break;
            }
            ALOGD("OMXCodec: split big input buffer %zu to %zu",
                  srcBuffer->range_length(), remainingBytes);
            // split input buffer
            bool needOwner = srcBuffer->refcount() == 0;
            mLeftOverBuffer = srcBuffer;
            srcBuffer = mLeftOverBuffer->clone();
            srcBuffer->set_range(mLeftOverBuffer->range_offset(), remainingBytes);
            mLeftOverBuffer->set_range(mLeftOverBuffer->range_offset() + remainingBytes,
                                       mLeftOverBuffer->range_length() - remainingBytes);
            if (needOwner)
            {
                // make a owner for MediaBuffer to help release
                mLeftOverBuffer->setObserver(&this->mOMXPartialBufferOwner);
                mLeftOverBuffer->add_ref();
            }
            isPartialFrame = true;
#else
            mLeftOverBuffer = srcBuffer;
            break;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        }

        bool releaseBuffer = true;
        if (mFlags & kStoreMetaDataInVideoBuffers) {
                releaseBuffer = false;
                info->mMediaBuffer = srcBuffer;
        }

        if (mFlags & kUseSecureInputBuffers) {
                // Data in "info" is already provided at this time.

                releaseBuffer = false;

                CHECK(info->mMediaBuffer == NULL);
                info->mMediaBuffer = srcBuffer;
        } else {
            CHECK(srcBuffer->data() != NULL) ;
            memcpy((uint8_t *)info->mData + offset,
                    (const uint8_t *)srcBuffer->data()
                        + srcBuffer->range_offset(),
                    srcBuffer->range_length());
        }

        int64_t lastBufferTimeUs;
#ifdef MTK_AOSP_ENHANCEMENT
        int32_t InvalidKeyTime;
        if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.AAC"))
        {
            int32_t nIsAACADIF;
            sp<MetaData> meta = mSource->getFormat();
            const char *mime;
            CHECK(meta->findCString(kKeyMIMEType, &mime));
            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime) && meta->findInt32(kKeyIsAACADIF, &nIsAACADIF))
            {
                if (0 != nIsAACADIF && !(srcBuffer->meta_data()->findInt64(kKeyTime, &lastBufferTimeUs)))
                {
                    //SXLOGD("OMXCodec::drainInputBuffer--not find BufferTimeUs for ADIF");
                    lastBufferTimeUs = 0;
                    srcBuffer->meta_data()->setInt64(kKeyTime, lastBufferTimeUs);
                }
            }
        }
        if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.FLAC"))
        {
            if (!(srcBuffer->meta_data()->findInt64(kKeyTime, &lastBufferTimeUs)))
            {
                ///LOGD("OMXCodec::drainInputBuffer--not find BufferTimeUs for FLAC");
                lastBufferTimeUs = 0;
                srcBuffer->meta_data()->setInt64(kKeyTime, lastBufferTimeUs);
            }
        }

#ifdef MTK_AUDIO_APE_SUPPORT
        if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.APE"))
        {
            status_t apeSeek_err = apeSeekFunc(srcBuffer);
            if(OK != apeSeek_err){
                return apeSeek_err;
            }
        }
#endif //MTK_AUDIO_APE_SUPPORT
        if (!strcmp(mComponentName, "OMX.MTK.VIDEO.DECODER.MPEG2"))
        {
            if ((srcBuffer->meta_data()->findInt32(kInvalidKeyTime, &InvalidKeyTime)) && InvalidKeyTime)
            {
                reComputePTS = true;
            }
        }

#endif //MTK_AOSP_ENHANCEMENT
        CHECK(srcBuffer->meta_data()->findInt64(kKeyTime, &lastBufferTimeUs));
        CHECK(lastBufferTimeUs >= 0);
        if (mIsEncoder && mIsVideo) {
            mDecodingTimeList.push_back(lastBufferTimeUs);
        }

        if (offset == 0) {
            timestampUs = lastBufferTimeUs;
        }

        offset += srcBuffer->range_length();

#ifdef MTK_AOSP_ENHANCEMENT
        if (!mIsEncoder && !strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mMIME)) {
#else //MTK_AOSP_ENHANCEMENT
        if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mMIME)) {
#endif //MTK_AOSP_ENHANCEMENT
            CHECK(!(mQuirks & kSupportsMultipleFramesPerInputBuffer));
            CHECK_GE(info->mSize, offset + sizeof(int32_t));

            int32_t numPageSamples;
            if (!srcBuffer->meta_data()->findInt32(
                        kKeyValidSamples, &numPageSamples)) {
                numPageSamples = -1;
            }

            memcpy((uint8_t *)info->mData + offset,
                   &numPageSamples,
                   sizeof(numPageSamples));

            offset += sizeof(numPageSamples);
        }

        if (releaseBuffer) {
            srcBuffer->release();
            srcBuffer = NULL;
        }

        ++n;

        if (!(mQuirks & kSupportsMultipleFramesPerInputBuffer)) {
            break;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG, mMIME))
        {
            if (n >= mp3FrameCountInBuffer)
            {
                ALOGV("mp3FrameCountInBuffer is %d", mp3FrameCountInBuffer);
                break;
            }
        }
#endif //MTK_AOSP_ENHANCEMENT

        int64_t coalescedDurationUs = lastBufferTimeUs - timestampUs;

        if (coalescedDurationUs > 250000ll) {
            // Don't coalesce more than 250ms worth of encoded data at once.
            break;
        }
    }

    if (n > 1) {
        ALOGV("coalesced %d frames into one input buffer", n);
    }

    OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;

#ifdef MTK_AOSP_ENHANCEMENT
    if (isPartialFrame)
    {
        flags = 0;
    }
#endif //MTK_AOSP_ENHANCEMENT

#ifdef MTK_AOSP_ENHANCEMENT
    if (signalEOS || (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.VORBIS") && mSignalledEOS))
    {
#else //MTK_AOSP_ENHANCEMENT
    if (signalEOS) {
#endif //MTK_AOSP_ENHANCEMENT
        flags |= OMX_BUFFERFLAG_EOS;
    } else {
        mNoMoreOutputData = false;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (reComputePTS)
    {
        flags |= OMX_BUFFERFLAG_INVALID_TIMESTAMP;
    }
#endif //MTK_AOSP_ENHANCEMENT

    if (info == NULL)
    {
        CHECK(mFlags & kUseSecureInputBuffers);
        CHECK(signalEOS);

        // This is fishy, there's still a MediaBuffer corresponding to this
        // info available to the source at this point even though we're going
        // to use it to signal EOS to the codec.
        info = findEmptyInputBuffer();
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (mPortStatus[kPortIndexInput] == SHUTTING_DOWN)
    {
        CODEC_LOGI("input port in shutdown mode");
        return false;
    }
#endif //MTK_AOSP_ENHANCEMENT

    CODEC_LOGD("Calling emptyBuffer on buffer %u (length %zu), "
               "timestamp %lld us (%.2f secs)",
               info->mBuffer, offset,
               (long long)timestampUs, timestampUs / 1E6);

    err = mOMX->emptyBuffer(
            mNode, info->mBuffer, 0, offset,
            flags, timestampUs);

    if (err != OK) {
        setState(ERROR);
        return false;
    }

    info->mStatus = OWNED_BY_COMPONENT;

    return true;
}

void OMXCodec::fillOutputBuffer(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);

    if (mNoMoreOutputData) {
#ifdef MTK_AOSP_ENHANCEMENT
        CODEC_LOGE("No more output data in fillOutputBuffer, mFilledBuffers size=%zu", mFilledBuffers.size());//for EOS may hang
#endif //MTK_AOSP_ENHANCEMENT
        CODEC_LOGV("There is no more output data available, not "
             "calling fillOutputBuffer");
        return;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (mPortStatus[kPortIndexOutput] == SHUTTING_DOWN){
        CODEC_LOGW("mPortStatus[kPortIndexOutput] is SHUTTING_DOWN, skip fillOutputBuffer for %u",
            info->mBuffer);
        return;
    }
#endif

    CODEC_LOGD("Calling fillBuffer on buffer %u", info->mBuffer);
    status_t err = mOMX->fillBuffer(mNode, info->mBuffer);

    if (err != OK) {
        CODEC_LOGE("fillBuffer failed w/ error 0x%08x", err);

        setState(ERROR);
        return;
    }

    info->mStatus = OWNED_BY_COMPONENT;
}

bool OMXCodec::drainInputBuffer(IOMX::buffer_id buffer) {
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        if ((*buffers)[i].mBuffer == buffer) {
            return drainInputBuffer(&buffers->editItemAt(i));
        }
    }

    CHECK(!"should not be here.");

    return false;
}

void OMXCodec::fillOutputBuffer(IOMX::buffer_id buffer) {
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        if ((*buffers)[i].mBuffer == buffer) {
            fillOutputBuffer(&buffers->editItemAt(i));
            return;
        }
    }

    CHECK(!"should not be here.");
}

void OMXCodec::setState(State newState) {
    mState = newState;
    mAsyncCompletion.signal();

    // This may cause some spurious wakeups but is necessary to
    // unblock the reader if we enter ERROR state.
    mBufferFilled.signal();
}

status_t OMXCodec::waitForBufferFilled_l() {

    if (mIsEncoder) {
        // For timelapse video recording, the timelapse video recording may
        // not send an input frame for a _long_ time. Do not use timeout
        // for video encoding.
        return mBufferFilled.wait(mLock);
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if ((mState == ERROR) && (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.APE"))) {
        CODEC_LOGE("ape is  in error state, just return!!!!!!!!!!!!!!!!!!!!");
        return UNKNOWN_ERROR;
    }
    if ((mState == ERROR) && (!strcmp(mComponentName, "OMX.MTK.VIDEO.DECODER.AVC"))) {
        CODEC_LOGE("avc is  in error state, just return!!!!!!!!!!!!!!!!!!!!");
        return UNKNOWN_ERROR;
    }

    //if (!strncmp("OMX.MTK.VIDEO.DECODER", mComponentName, 21))
    {
        CODEC_LOGE("+waitForBufferFilled_l: %d/%d",
                   (int)countBuffersWeOwn(mPortBuffers[kPortIndexInput]),
                   (int)countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));//for EOS may hang
    }

    status_t err;
    if (mRTSPOutputTimeoutUS != -1) {
        CODEC_LOGI("output buf time out %lld us for rtsp.", (long long)mRTSPOutputTimeoutUS);
        err = mBufferFilled.waitRelative(mLock, mRTSPOutputTimeoutUS);
    }
    else if (mHTTPOutputTimeoutUS != -1) {
        CODEC_LOGI("output buf time out %lld us for http.", (long long)mHTTPOutputTimeoutUS);
        err = mBufferFilled.waitRelative(mLock, mHTTPOutputTimeoutUS);
    }
    else if (!strncmp(mComponentName, "OMX.MTK.VIDEO.DECODER", 21)) {
        err = mBufferFilled.waitRelative(mLock, VDeckBufferFilledEventTimeOutNs);
    }
    else {
        err = mBufferFilled.waitRelative(mLock, kBufferFilledEventTimeOutNs);
    }
#else //MTK_AOSP_ENHANCEMENT
    status_t err = mBufferFilled.waitRelative(mLock, kBufferFilledEventTimeOutNs);
#endif //MTK_AOSP_ENHANCEMENT
    if (err != OK) {
        CODEC_LOGE("Timed out waiting for output buffers: %zu/%zu",
            countBuffersWeOwn(mPortBuffers[kPortIndexInput]),
            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));
    }
#ifdef HAVE_AEE_FEATURE
    if (mIsVideo)
    {
        if (!strncmp(mComponentName, "OMX.MTK.VIDEO.DECODER", 21)){
            //aee_system_exception("OMXCodec", NULL, DB_OPT_FTRACE, "[%s] Timed out waiting for output buffers\nCRDISPATCH_KEY:%s", mComponentName, mComponentName);
        }
    }
#endif

#ifdef MTK_AOSP_ENHANCEMENT
    CODEC_LOGE("-waitForBufferFilled_l");//for EOS may hang
#endif //MTK_AOSP_ENHANCEMENT
    return err;
}

void OMXCodec::setRawAudioFormat(
        OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels) {

    // port definition
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;
    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;
    CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamPortDefinition,
            &def, sizeof(def)), (status_t)OK);

    // pcm param
    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    CHECK_EQ(err, (status_t)OK);

    pcmParams.nChannels = numChannels;
    pcmParams.eNumData = OMX_NumericalDataSigned;
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nBitPerSample = 16;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    CHECK_EQ(getOMXChannelMapping(
                numChannels, pcmParams.eChannelMapping), (status_t)OK);

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    CHECK_EQ(err, (status_t)OK);
}

static OMX_AUDIO_AMRBANDMODETYPE pickModeFromBitRate(bool isAMRWB, int32_t bps) {
    if (isAMRWB) {
        if (bps <= 6600) {
            return OMX_AUDIO_AMRBandModeWB0;
        } else if (bps <= 8850) {
            return OMX_AUDIO_AMRBandModeWB1;
        } else if (bps <= 12650) {
            return OMX_AUDIO_AMRBandModeWB2;
        } else if (bps <= 14250) {
            return OMX_AUDIO_AMRBandModeWB3;
        } else if (bps <= 15850) {
            return OMX_AUDIO_AMRBandModeWB4;
        } else if (bps <= 18250) {
            return OMX_AUDIO_AMRBandModeWB5;
        } else if (bps <= 19850) {
            return OMX_AUDIO_AMRBandModeWB6;
        } else if (bps <= 23050) {
            return OMX_AUDIO_AMRBandModeWB7;
        }

        // 23850 bps
        return OMX_AUDIO_AMRBandModeWB8;
    } else {  // AMRNB
        if (bps <= 4750) {
            return OMX_AUDIO_AMRBandModeNB0;
        } else if (bps <= 5150) {
            return OMX_AUDIO_AMRBandModeNB1;
        } else if (bps <= 5900) {
            return OMX_AUDIO_AMRBandModeNB2;
        } else if (bps <= 6700) {
            return OMX_AUDIO_AMRBandModeNB3;
        } else if (bps <= 7400) {
            return OMX_AUDIO_AMRBandModeNB4;
        } else if (bps <= 7950) {
            return OMX_AUDIO_AMRBandModeNB5;
        } else if (bps <= 10200) {
            return OMX_AUDIO_AMRBandModeNB6;
        }

        // 12200 bps
        return OMX_AUDIO_AMRBandModeNB7;
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
status_t OMXCodec::setAMRFormat(bool isWAMR, int32_t bitRate)
{
#else //MTK_AOSP_ENHANCEMENT
void OMXCodec::setAMRFormat(bool isWAMR, int32_t bitRate) {
#endif //MTK_AOSP_ENHANCEMENT
    OMX_U32 portIndex = mIsEncoder ? kPortIndexOutput : kPortIndexInput;

    OMX_AUDIO_PARAM_AMRTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err =
        mOMX->getParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

    def.eAMRFrameFormat = OMX_AUDIO_AMRFrameFormatFSF;

    def.eAMRBandMode = pickModeFromBitRate(isWAMR, bitRate);
    def.nBitRate = bitRate;
    ALOGD("setAMRFormat:bitrate:%d", def.nBitRate);
    err = mOMX->setParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
#ifdef MTK_AOSP_ENHANCEMENT
    if (mIsEncoder && err != OK)
    {
        return err;
    }
#endif //MTK_AOSP_ENHANCEMENT

    ////////////////////////

    if (mIsEncoder) {
        sp<MetaData> format = mSource->getFormat();
        int32_t sampleRate;
        int32_t numChannels;
        CHECK(format->findInt32(kKeySampleRate, &sampleRate));
        CHECK(format->findInt32(kKeyChannelCount, &numChannels));

        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
    }

#ifdef MTK_AOSP_ENHANCEMENT
    return OK;
#endif //MTK_AOSP_ENHANCEMENT

}

status_t OMXCodec::setAACFormat(
        int32_t numChannels, int32_t sampleRate, int32_t bitRate, int32_t aacProfile, bool isADTS) {
    if (numChannels > 2) {
        ALOGW("Number of channels: (%d) \n", numChannels);
    }

    if (mIsEncoder) {
        if (isADTS) {
            return -EINVAL;
        }

        //////////////// input port ////////////////////
        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);

        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        InitOMXParams(&format);
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;
        status_t err = OMX_ErrorNone;
        while (OMX_ErrorNone == err) {
            CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioPortFormat,
                    &format, sizeof(format)), (status_t)OK);
            if (format.eEncoding == OMX_AUDIO_CodingAAC) {
                break;
            }
            format.nIndex++;
        }
        CHECK_EQ((status_t)OK, err);
        CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format)), (status_t)OK);

        // port definition
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;
        CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);
        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;
        CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);

        // profile
        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;
        CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioAac,
                &profile, sizeof(profile)), (status_t)OK);
        profile.nChannels = numChannels;
        profile.eChannelMode = (numChannels == 1?
                OMX_AUDIO_ChannelModeMono: OMX_AUDIO_ChannelModeStereo);
        profile.nSampleRate = sampleRate;
        profile.nBitRate = bitRate;
        profile.nAudioBandWidth = 0;
        profile.nFrameLength = 0;
        profile.nAACtools = OMX_AUDIO_AACToolAll;
        profile.nAACERtools = OMX_AUDIO_AACERNone;
        profile.eAACProfile = (OMX_AUDIO_AACPROFILETYPE) aacProfile;
        profile.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
        err = mOMX->setParameter(mNode, OMX_IndexParamAudioAac,
                &profile, sizeof(profile));

        if (err != OK) {
            CODEC_LOGE("setParameter('OMX_IndexParamAudioAac') failed "
                       "(err = %d)",
                       err);
            return err;
        }
    } else {
        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexInput;

        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));
        CHECK_EQ(err, (status_t)OK);

        profile.nChannels = numChannels;
        profile.nSampleRate = sampleRate;

        profile.eAACStreamFormat =
            isADTS
                ? OMX_AUDIO_AACStreamFormatMP4ADTS
                : OMX_AUDIO_AACStreamFormatMP4FF;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            CODEC_LOGE("setParameter('OMX_IndexParamAudioAac') failed "
                       "(err = %d)",
                       err);
            return err;
        }
    }

    return OK;
}

#ifdef MTK_AOSP_ENHANCEMENT
void OMXCodec::setVORBISFormat(const sp<MetaData> &meta)
{
    int32_t iChannelNum = 0, iSampleRate = 0, iBitRate = 0;

    CHECK(meta->findInt32(kKeyBitRate, &iBitRate));
    CHECK(meta->findInt32(kKeyChannelCount, &iChannelNum));
    CHECK(meta->findInt32(kKeySampleRate, &iSampleRate));

    CHECK(iChannelNum == 1 || iChannelNum == 2);

    //////////////// input port ////////////////////
    setRawAudioFormat(kPortIndexInput, iSampleRate, iChannelNum);

    //////////////// output port ////////////////////
    // format
    OMX_AUDIO_PARAM_PORTFORMATTYPE format;
    format.nPortIndex = kPortIndexOutput;
    format.nIndex = 0;
    status_t err = OMX_ErrorNone;
    while (OMX_ErrorNone == err)
    {
        CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioPortFormat, &format, sizeof(format)), (status_t)OK);
        if (format.eEncoding == OMX_AUDIO_CodingVORBIS)
        {
            break;
        }
        format.nIndex++;
    }
    CHECK_EQ((status_t)OK, err);
    CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamAudioPortFormat, &format, sizeof(format)), (status_t)OK);

    // port definition
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;
    CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def)), (status_t)OK);
    def.format.audio.bFlagErrorConcealment = OMX_TRUE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingVORBIS;
    CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def)), (status_t)OK);

    // profile
    OMX_AUDIO_PARAM_VORBISTYPE profile;
    InitOMXParams(&profile);
    profile.nPortIndex = kPortIndexOutput;
    CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioVorbis, &profile, sizeof(profile)), (status_t)OK);
    profile.nChannels = iChannelNum;
    profile.nSampleRate = iSampleRate;
    profile.nBitRate = iBitRate;
    profile.nAudioBandWidth = 0;

    CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamAudioVorbis, &profile, sizeof(profile)), (status_t)OK);

}
int32_t OMXCodec::getAACProfile()
{
    OMX_AUDIO_PARAM_AACPROFILETYPE profileAAC;
    InitOMXParams(&profileAAC);
    profileAAC.nPortIndex = kPortIndexInput;
    status_t errAAC = mOMX->getParameter(mNode, OMX_IndexParamAudioAac, &profileAAC, sizeof(profileAAC));
    CHECK_EQ((status_t)OK, errAAC);
    CODEC_LOGV("profileAAC.eAACProfile %d", profileAAC.eAACProfile);
    return profileAAC.eAACProfile;
}
#endif //MTK_AOSP_ENHANCEMENT

status_t OMXCodec::setAC3Format(int32_t numChannels, int32_t sampleRate)
{
    OMX_AUDIO_PARAM_ANDROID_AC3TYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
            &def,
            sizeof(def));

    if (err != OK) {
        return err;
    }

    def.nChannels = numChannels;
    def.nSampleRate = sampleRate;

    return mOMX->setParameter(
            mNode,
            (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3,
            &def,
            sizeof(def));
}

void OMXCodec::setG711Format(int32_t sampleRate, int32_t numChannels) {
    CHECK(!mIsEncoder);
    setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
}

void OMXCodec::setImageOutputFormat(
        OMX_COLOR_FORMATTYPE format, OMX_U32 width, OMX_U32 height) {
    CODEC_LOGV("setImageOutputFormat(%u, %u)", width, height);

#if 0
    OMX_INDEXTYPE index;
    status_t err = mOMX->get_extension_index(
            mNode, "OMX.TI.JPEG.decode.Config.OutputColorFormat", &index);
    CHECK_EQ(err, (status_t)OK);

    err = mOMX->set_config(mNode, index, &format, sizeof(format));
    CHECK_EQ(err, (status_t)OK);
#endif

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainImage);

    OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

    CHECK_EQ((int)imageDef->eCompressionFormat, (int)OMX_IMAGE_CodingUnused);
    imageDef->eColorFormat = format;
    imageDef->nFrameWidth = width;
    imageDef->nFrameHeight = height;

    switch (format) {
        case OMX_COLOR_FormatYUV420PackedPlanar:
        case OMX_COLOR_FormatYUV411Planar:
        {
            def.nBufferSize = (width * height * 3) / 2;
            break;
        }

        case OMX_COLOR_FormatCbYCrY:
        {
            def.nBufferSize = width * height * 2;
            break;
        }

        case OMX_COLOR_Format32bitARGB8888:
        {
            def.nBufferSize = width * height * 4;
            break;
        }

        case OMX_COLOR_Format16bitARGB4444:
        case OMX_COLOR_Format16bitARGB1555:
        case OMX_COLOR_Format16bitRGB565:
        case OMX_COLOR_Format16bitBGR565:
        {
            def.nBufferSize = width * height * 2;
            break;
        }

        default:
            CHECK(!"Should not be here. Unknown color format.");
            break;
    }

    def.nBufferCountActual = def.nBufferCountMin;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
}

void OMXCodec::setJPEGInputFormat(
        OMX_U32 width, OMX_U32 height, OMX_U32 compressedSize) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainImage);
    OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

    CHECK_EQ((int)imageDef->eCompressionFormat, (int)OMX_IMAGE_CodingJPEG);
    imageDef->nFrameWidth = width;
    imageDef->nFrameHeight = height;

    def.nBufferSize = compressedSize;
    def.nBufferCountActual = def.nBufferCountMin;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
}

void OMXCodec::addCodecSpecificData(const void *data, size_t size) {
    CodecSpecificData *specific =
        (CodecSpecificData *)malloc(sizeof(CodecSpecificData) + size - 1);

    specific->mSize = size;
    memcpy(specific->mData, data, size);

    mCodecSpecificData.push(specific);
}

void OMXCodec::clearCodecSpecificData() {
    for (size_t i = 0; i < mCodecSpecificData.size(); ++i) {
        free(mCodecSpecificData.editItemAt(i));
    }
    mCodecSpecificData.clear();
    mCodecSpecificDataIndex = 0;
}

status_t OMXCodec::start(MetaData *meta) {
#ifdef MTK_AOSP_ENHANCEMENT
    ATRACE_CALL();
#endif
    Mutex::Autolock autoLock(mLock);

    if (mState != LOADED) {
        CODEC_LOGE("called start in the unexpected state: %d", mState);
        return UNKNOWN_ERROR;
    }

    sp<MetaData> params = new MetaData;
    if (mQuirks & kWantsNALFragments) {
        params->setInt32(kKeyWantsNALFragments, true);
    }
    if (meta) {
        int64_t startTimeUs = 0;
        int64_t timeUs;
        if (meta->findInt64(kKeyTime, &timeUs)) {
            startTimeUs = timeUs;
        }
        params->setInt64(kKeyTime, startTimeUs);
#ifdef MTK_AOSP_ENHANCEMENT
        int32_t isHttpStreaming = 0;
        if (meta->findInt32(kKeyIsHTTPStreaming, &isHttpStreaming) && isHttpStreaming)
        {
            mIsHttpStreaming = true;
            ALOGD("@@ mIsHttpStreaming (%d)", mIsHttpStreaming);
        }

        int32_t mode;
        if (meta->findInt32(kKeyRTSPSeekMode, &mode) && mode != 0)
        {
            status_t err2 = OK;
            OMX_INDEXTYPE index = OMX_IndexMax;
            status_t err = mOMX->getExtensionIndex(mNode, "OMX.MTK.index.param.video.StreamingMode", &index);
            if (err == OK)
            {
                OMX_BOOL m = OMX_TRUE;
                err2 = mOMX->setParameter(mNode, index, &m, sizeof(m));
            }
            ALOGI("set StreamingMode, index = %x, err = %x, err2 = %x", index, err, err2);
        }
        // mtk80902: ALPS00390150
        int64_t to;
        if (meta->findInt64(kKeyRTSPOutputTimeoutUS, &to) && to != 0)
        {
            ALOGI("set output buffer timeout %lld for rtsp.", (long long)to);
            mRTSPOutputTimeoutUS = to;
        }
        if (meta->findInt64(kKeyHTTPOutputTimeoutUS, &to) && to != 0)
        {
            ALOGI("set output buffer timeout %lld for http.", (long long)to);
            mHTTPOutputTimeoutUS = to;
        }
        int32_t number = -1;
        if (meta->findInt32(kKeyMaxQueueBuffer, &number) && number > 0)
        {
            mMaxQueueBufferNum = number;
        }
        if (meta->findInt32(kKeyInputBufferNum, &number) && number > 0)
        {
            OMX_PARAM_PORTDEFINITIONTYPE def;
            InitOMXParams(&def);
            def.nPortIndex = kPortIndexInput;

            status_t err = mOMX->getParameter(
                               mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((int)err, (int)OK);

            def.nBufferCountActual = number > (int32_t)def.nBufferCountMin
                                     ? number : def.nBufferCountMin;

            err = mOMX->setParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((int)err, (int)OK);

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((int)err, (int)OK);

        }
#ifdef OMX_VE_AUDIO
        int32_t buffersize = 0;
        int32_t maxframenum = -1;
        if ((mFlags & kMtkAudDecForVE))
        {
            OMX_PARAM_PORTDEFINITIONTYPE def;
            InitOMXParams(&def);
            def.nPortIndex = kPortIndexOutput;
            status_t err = mOMX->getParameter(
                               mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((int)err, (int)OK);

            if (meta->findInt32(kKeyOutputBufferNum, &number) && number > 0)
            {
                def.nBufferCountActual = number > (int32_t)def.nBufferCountMin
                                         ? number : def.nBufferCountMin;
            }
            if (meta->findInt32(kKeyOutBufSize, &buffersize) && buffersize > 0)
            {
                def.nBufferSize = buffersize;
            }
            err = mOMX->setParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((int)err, (int)OK);
            if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.MP3"))
            {
                OMX_MP3_CONFIG_TYPE defmp3;
                err = mOMX->getParameter(
                          mNode, OMX_IndexVendorMtkMP3Decode, &defmp3, sizeof(defmp3));
                CHECK_EQ((int)err, (int)OK);

                if (meta->findInt32(kKeyFrameNum, &maxframenum) && maxframenum > 0)
                {
                    defmp3.nMaxBufFrameNum = maxframenum;
                }
                err = mOMX->setParameter(
                          mNode, OMX_IndexVendorMtkMP3Decode, &defmp3, sizeof(defmp3));
                CHECK_EQ((int)err, (int)OK);
                CODEC_LOGI("set port num %d", maxframenum);
            }
            CODEC_LOGI("set buffer num %d and size %d", def.nBufferCountActual, def.nBufferSize);
        }
#endif //OMX_VE_AUDIO
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    }

    mCodecSpecificDataIndex = 0;
    mInitialBufferSubmit = true;
    mSignalledEOS = false;
    mNoMoreOutputData = false;
    mOutputPortSettingsHaveChanged = false;
    mSeekTimeUs = -1;
    mSeekMode = ReadOptions::SEEK_CLOSEST_SYNC;
    mTargetTimeUs = -1;
    mFilledBuffers.clear();
    mPaused = false;

#ifdef MTK_AOSP_ENHANCEMENT
    if (mIsEncoder)
    {
        status_t err;
        if ((err = init()) != OK)
        {
            CODEC_LOGE("init failed: %d", err);
            return err;
        }
        params->setInt32(kKeyNumBuffers, mPortBuffers[kPortIndexInput].size());
        err = mSource->start(params.get());
        if (err != OK)
        {
            CODEC_LOGE("source failed to start: %d", err);
            stopOmxComponent_l();
        }
        return err;
    }
    status_t err = mSource->start(params.get());

    if (err != OK)
    {
        return err;
    }
    if (!strncmp("OMX.MTK.", mComponentName, 8))
    {
        OMX_BOOL value;
        // check if codec supports partial frames input
        status_t err = mOMX->getParameter(mNode,
                                          (OMX_INDEXTYPE)OMX_IndexVendorMtkOmxPartialFrameQuerySupported,
                                          &value, sizeof(value));
        mSupportsPartialFrames = value;
        if (err != OK)
        {
            mSupportsPartialFrames = false;
        }
        ALOGI("mSupportsPartialFrames %d err %d ", mSupportsPartialFrames, err);
    }

    err = init();
    if (err != OK)
    {
        ALOGE("line=%d,err:%d,init fail,stop mSource", __LINE__, err);
        mSource->stop();
    }
    return err;
#else //MTK_AOSP_ENHANCEMENT
    status_t err;
    if (mIsEncoder) {
        // Calling init() before starting its source so that we can configure,
        // if supported, the source to use exactly the same number of input
        // buffers as requested by the encoder.
        if ((err = init()) != OK) {
            CODEC_LOGE("init failed: %d", err);
            return err;
        }

        params->setInt32(kKeyNumBuffers, mPortBuffers[kPortIndexInput].size());
        err = mSource->start(params.get());
        if (err != OK) {
            CODEC_LOGE("source failed to start: %d", err);
            stopOmxComponent_l();
        }
        return err;
    }

    // Decoder case
    if ((err = mSource->start(params.get())) != OK) {
        CODEC_LOGE("source failed to start: %d", err);
        return err;
    }
    return init();
#endif //MTK_AOSP_ENHANCEMENT
}

status_t OMXCodec::stop() {
#ifdef MTK_AOSP_ENHANCEMENT
    ATRACE_CALL();
#endif
    CODEC_LOGD("stop mState=%d", mState);
    Mutex::Autolock autoLock(mLock);
    status_t err = stopOmxComponent_l();

#if 0 ////ALPS01877791 video transcoder can not destroy instances
    //if( mDeathNotifier ) //tmp fix
    {
        CODEC_LOGD("mDeathNotifier.clear");
        mDeathNotifier.clear();
    }
#endif

    mSource->stop();
    //Prevent hang in OMXCodec::read() on cancelling transcoder
    mBufferFilled.signal();
    CODEC_LOGD("stopped in state %d", mState);
    return err;
}

status_t OMXCodec::stopOmxComponent_l() {
    CODEC_LOGV("stopOmxComponent_l mState=%d", mState);

    while (isIntermediateState(mState)) {
        mAsyncCompletion.wait(mLock);
    }

    bool isError = false;
    switch (mState) {
        case LOADED:
            break;

        case ERROR:
        {
            if (mPortStatus[kPortIndexOutput] == ENABLING) {
                // Codec is in a wedged state (technical term)
                // We've seen an output port settings change from the codec,
                // We've disabled the output port, then freed the output
                // buffers, initiated re-enabling the output port but
                // failed to reallocate the output buffers.
                // There doesn't seem to be a way to orderly transition
                // from executing->idle and idle->loaded now that the
                // output port hasn't been reenabled yet...
                // Simply free as many resources as we can and pretend
                // that we're in LOADED state so that the destructor
                // will free the component instance without asserting.
                freeBuffersOnPort(kPortIndexInput, true /* onlyThoseWeOwn */);
                freeBuffersOnPort(kPortIndexOutput, true /* onlyThoseWeOwn */);
                setState(LOADED);
                break;
            } else {
                OMX_STATETYPE state = OMX_StateInvalid;
                status_t err = mOMX->getState(mNode, &state);
                CHECK_EQ(err, (status_t)OK);

                if (state != OMX_StateExecuting) {
                    break;
                }
                // else fall through to the idling code
            }

            isError = true;
        }

        case EXECUTING:
        {
            setState(EXECUTING_TO_IDLE);

            if (mQuirks & kRequiresFlushBeforeShutdown) {
                CODEC_LOGV("This component requires a flush before transitioning "
                     "from EXECUTING to IDLE...");

                bool emulateInputFlushCompletion =
                    !flushPortAsync(kPortIndexInput);

                bool emulateOutputFlushCompletion =
                    !flushPortAsync(kPortIndexOutput);

                if (emulateInputFlushCompletion) {
                    onCmdComplete(OMX_CommandFlush, kPortIndexInput);
                }

                if (emulateOutputFlushCompletion) {
                    onCmdComplete(OMX_CommandFlush, kPortIndexOutput);
                }
            } else {
                mPortStatus[kPortIndexInput] = SHUTTING_DOWN;
                mPortStatus[kPortIndexOutput] = SHUTTING_DOWN;

#ifdef MTK_AOSP_ENHANCEMENT
                if (mQueueWaiting)
                {
                    mBufferSent.signal();
                }
#endif // #ifdef MTK_AOSP_ENHANCEMENT

                status_t err =
                    mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
#ifdef MTK_AOSP_ENHANCEMENT
                if (err != OK) {
                    CODEC_LOGE("Failed to set OMX state to idle, returned error %d", err);
                    return err;
                }
#else
                CHECK_EQ(err, (status_t)OK);
#endif
            }

            while (mState != LOADED && mState != ERROR) {
                mAsyncCompletion.wait(mLock);
            }

            if (isError) {
                // We were in the ERROR state coming in, so restore that now
                // that we've idled the OMX component.
                setState(ERROR);
            }

            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }

    if (mLeftOverBuffer) {
        mLeftOverBuffer->release();
        mLeftOverBuffer = NULL;
    }

    return OK;
}

sp<MetaData> OMXCodec::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mOutputFormat;
}

#ifdef MTK_AOSP_ENHANCEMENT  // Morris Yang for Camera recording
sp<MetaData> OMXCodec::getCameraMeta()
{
    Mutex::Autolock autoLock(mLock);

    return mCameraMeta;
}

status_t OMXCodec::vEncSetForceIframe(bool enable)
{
    CODEC_LOGV("vEncSetForceIframe %d", enable);

    if (mIsVideoEncoder)
    {
        if (!strncmp(mComponentName, "OMX.MTK.", 8))
        {
            OMX_INDEXTYPE index;
            status_t err =
                mOMX->getExtensionIndex(
                    mNode,
                    "OMX.MTK.index.param.video.EncSetForceIframe",
                    &index);

            if (err != OK)
            {
                return err;
            }

            OMX_BOOL enable = OMX_TRUE;
            err = mOMX->setConfig(mNode, index, &enable, sizeof(enable));

            if (err != OK)
            {
                CODEC_LOGE("setConfig('OMX.MTK.index.param.video.EncSetForceIframe') returned error 0x%08x", err);
                return err;
            }
        }
    }

    return OK;
}

status_t OMXCodec::vEncSetFrameRate(unsigned int u4FrameRate)
{
    OMX_CONFIG_FRAMERATETYPE    ConfigFrameRate;
    status_t    err = OK;
    if (mIsVideoEncoder)
    {
        CODEC_LOGE("@@@>> set frame rate >> %d", u4FrameRate);
        InitOMXParams(&ConfigFrameRate);
        ConfigFrameRate.xEncodeFramerate = u4FrameRate << 16;
        ConfigFrameRate.nPortIndex = kPortIndexOutput;
        status_t err = mOMX->setConfig(mNode, OMX_IndexConfigVideoFramerate, &ConfigFrameRate, sizeof(OMX_CONFIG_FRAMERATETYPE));
        if (err != OK)
        {
            CODEC_LOGE("Fail to adjust framerate-rate, returned error 0x%08x", err);
            return err;
        }
    }
    return err;
}

// for dynamic bit-rate adjustment [
status_t OMXCodec::vEncSetBitRate(unsigned int u4BitRate)
{

    OMX_VIDEO_CONFIG_BITRATETYPE ConfigBitrate;
    status_t err = OK;

    if (mIsVideoEncoder)
    {
        //CODEC_LOGE("@@@>> set bit rate >> %d",u4BitRate);
        ConfigBitrate.nEncodeBitrate = u4BitRate;
        ConfigBitrate.nPortIndex = kPortIndexOutput;
        err = mOMX->setConfig(mNode, OMX_IndexConfigVideoBitrate, &ConfigBitrate, sizeof(OMX_VIDEO_CONFIG_BITRATETYPE));
        if (err != OK)
        {
            ALOGE("Fail to adjust bit-rate, returned error 0x%08x", err);
            return err;
        }
    }
    return err;
}
// ]

status_t OMXCodec::vDecSwitchBwTVout(bool enable)     // true: w/ tvout,  false: w/o tvout
{
    if (mIsVideoDecoder)
    {
        if (!strncmp(mComponentName, "OMX.MTK.", 8))
        {
            OMX_INDEXTYPE index;
            status_t err =
                mOMX->getExtensionIndex(
                    mNode,
                    "OMX.MTK.index.param.video.SwitchBwTVout",
                    &index);

            if (err != OK)
            {
                return err;
            }

            OMX_BOOL mEnable = (OMX_BOOL)enable;
            err = mOMX->setConfig(mNode, index, &mEnable, sizeof(mEnable));

            if (err != OK)
            {
                CODEC_LOGE("setConfig('OMX.MTK.index.param.video.SwitchBwTVout') returned error 0x%08x", err);
                return err;
            }
        }
    }
    return OK;
}
size_t OMXCodec::buffersOwn()
{
    const Vector<BufferInfo> &buffers = mPortBuffers[kPortIndexOutput];
    size_t n = 0;
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        if (buffers[i].mStatus == OWNED_BY_US)
        {
            ++n;
        }
    }

    return n;
}

//for videoeditor MVA mode
void *OMXCodec::findInputBufferByDataNumber(OMX_U32 portIndex, uint32_t number)
{
    Vector<BufferInfo> *infos = &mPortBuffers[portIndex];
    for (size_t i = 0; i < infos->size(); ++i)
    {
        BufferInfo *info = &infos->editItemAt(i);

        if (i == number)
        {
            CODEC_LOGI(
                "portIndex %d buffer data number = %d, buffer_id = %u",
                portIndex, number,
                info->mBuffer);

            return info;
        }
    }

    TRESPASS();
}

#endif //MTK_AOSP_ENHANCEMENT

status_t OMXCodec::read(
        MediaBuffer **buffer, const ReadOptions *options) {
#ifdef MTK_AOSP_ENHANCEMENT
    ATRACE_CALL();
#endif
    status_t err = OK;

#ifdef MTK_AOSP_ENHANCEMENT
    //MTK80721 Vorbis Enc
    if (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.VORBIS") && *buffer != NULL)
    {
        mSignalledEOS = true;
        ALOGD("OMXCodec::read:*buffer=%p", *buffer);
        (*buffer)->release();
    }
#endif //MTK_AOSP_ENHANCEMENT

    *buffer = NULL;

    Mutex::Autolock autoLock(mLock);

    if (mState != EXECUTING && mState != RECONFIGURING) {
#ifdef MTK_AOSP_ENHANCEMENT
        if (mState == ERROR && ((mFinalStatus == ERROR_UNSUPPORTED_VIDEO) || (mFinalStatus == ERROR_UNSUPPORTED_AUDIO) || (mFinalStatus == ERROR_BUFFER_DEQUEUE_FAIL)))
        {
            return mFinalStatus;
        }
#endif //MTK_AOSP_ENHANCEMENT
        return UNKNOWN_ERROR;
    }

    bool seeking = false;
    int64_t seekTimeUs;
    ReadOptions::SeekMode seekMode;
    if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        seeking = true;
#ifdef MTK_AOSP_ENHANCEMENT
        mPreRollStartTime = systemTime() / 1000;
        CODEC_LOGI("mPreRollStartTime = %lld", (long long)mPreRollStartTime);
#endif //MTK_AOSP_ENHANCEMENT
    }

    if (mInitialBufferSubmit) {
        mInitialBufferSubmit = false;

        if (seeking) {
            CHECK(seekTimeUs >= 0);
            mSeekTimeUs = seekTimeUs;
            mSeekMode = seekMode;

            // There's no reason to trigger the code below, there's
            // nothing to flush yet.
            seeking = false;
            mPaused = false;
        }

#ifdef MTK_AOSP_ENHANCEMENT  // Morris Yang for Camera recording
        if (mCameraMeta.get() != NULL)
        {
#if 0
            // drain one input from camera preview pool
            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            drainInputBuffer(&buffers->editItemAt(0));
#else
            int32_t _initial_submit_count = 0;
            int32_t _initial_submit_limit = 2;
            if (mIsVENCTimelapseMode)
            {
                _initial_submit_limit = 1;
                ALOGD("Submit one frame for timelapse mode!!");
            }
            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            for (size_t i = 0; i < buffers->size(); ++i)
            {
                BufferInfo *info = &buffers->editItemAt(i);

                if (info->mStatus != OWNED_BY_US)
                {
                    continue;
                }

                if (!drainInputBuffer(info, true))
                {
                    break;
                }

                _initial_submit_count++;
                if (_initial_submit_count == _initial_submit_limit)
                {
                    break;
                }
            }
#endif
        }
        else
        {
            drainInputBuffers();
        }
#else //MTK_AOSP_ENHANCEMENT
        drainInputBuffers();
#endif //MTK_AOSP_ENHANCEMENT
        if (mState == EXECUTING)
        {
            // Otherwise mState == RECONFIGURING and this code will trigger
            // after the output port is reenabled.
            fillOutputBuffers();
        }
        if (seeking)
        {
#ifdef MTK_AOSP_ENHANCEMENT
            OMX_TICKS seekTime = seekTimeUs;
            mOMX->setConfig(mNode, OMX_IndexVendorMtkOmxVdecSeekMode, (void *)&seekTime, sizeof(void *));
#endif //MTK_AOSP_ENHANCEMENT
        }
    }
#ifdef  OMX_VE_AUDIO
    else if ((mFlags & kAudUseOMXForVE))
    {
        Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
        //LOGV("VE Drain buffers->size()=%d",buffers->size());
        for (size_t i = 0; i < buffers->size(); ++i)
        {
            BufferInfo *info = &buffers->editItemAt(i);
            if (info->mStatus != OWNED_BY_US)
            {
                continue;
            }
            if (!drainInputBuffer(info, true))
            {
                break;
            }
        }
    }
#endif //OMX_VE_AUDIO
    if (seeking)
    {
        while (mState == RECONFIGURING)
        {
            if ((err = waitForBufferFilled_l()) != OK)
            {
#ifdef MTK_AOSP_ENHANCEMENT
                ALOGE("timeout in RECONFIGURING state");
#endif //MTK_AOSP_ENHANCEMENT
                return err;
            }
        }

        if (mState != EXECUTING) {
#ifdef MTK_AOSP_ENHANCEMENT
            if (mState == ERROR && ((mFinalStatus == ERROR_UNSUPPORTED_VIDEO) || (mFinalStatus == ERROR_UNSUPPORTED_AUDIO))) {
                return mFinalStatus;
            }
#endif //MTK_AOSP_ENHANCEMENT
            return UNKNOWN_ERROR;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        CODEC_LOGI("seeking to %" PRId64 " us (%.2f secs)", seekTimeUs, seekTimeUs / 1E6);
#else
        CODEC_LOGV("seeking to %" PRId64 " us (%.2f secs)", seekTimeUs, seekTimeUs / 1E6);
#endif //MTK_AOSP_ENHANCEMENT

        mSignalledEOS = false;

        CHECK(seekTimeUs >= 0);
        mSeekTimeUs = seekTimeUs;
        mSeekMode = seekMode;
#ifdef MTK_AOSP_ENHANCEMENT
        //Bruce Hsu resend SPS and PPS for AVC after seek
        if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mMIME))
        {
            mCodecSpecificDataIndex = 0;
            if (mCodecSpecificData.size() != 2)
            {
                ALOGE("Resend unexpected data, %zu!!", mCodecSpecificData.size());
            }
        }
#endif //MTK_AOSP_ENHANCEMENT
        mFilledBuffers.clear();

        CHECK_EQ((int)mState, (int)EXECUTING);

        bool emulateInputFlushCompletion = !flushPortAsync(kPortIndexInput);
        bool emulateOutputFlushCompletion = !flushPortAsync(kPortIndexOutput);

        if (emulateInputFlushCompletion) {
            onCmdComplete(OMX_CommandFlush, kPortIndexInput);
        }

        if (emulateOutputFlushCompletion) {
            onCmdComplete(OMX_CommandFlush, kPortIndexOutput);
        }
#ifdef MTK_AOSP_ENHANCEMENT
        OMX_TICKS seekTime = seekTimeUs;
        mOMX->setConfig(mNode, OMX_IndexVendorMtkOmxVdecSeekMode, (void *)&seekTime, sizeof(void *));
#endif //MTK_AOSP_ENHANCEMENT
        while (mSeekTimeUs >= 0)
        {
            if ((err = waitForBufferFilled_l()) != OK)
            {
#ifdef MTK_AOSP_ENHANCEMENT
                ALOGE("timeout in seeking state");
#endif //MTK_AOSP_ENHANCEMENT
                return err;
            }
#ifdef MTK_AOSP_ENHANCEMENT
            size_t index = *mFilledBuffers.begin();
            if (!mFilledBuffers.empty() && mPortBuffers[kPortIndexOutput].size() > 0 && index < mPortBuffers[kPortIndexOutput].size())
            {
                BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(index);
                MediaBuffer *buffer = info->mMediaBuffer;
                if (buffer && buffer->range_length() == 0 && info->mStatus == OWNED_BY_US)   // invalid buffer and return
                {
                    mFilledBuffers.erase(mFilledBuffers.begin());
                    CODEC_LOGV("output buffer length 0 return to omx directly");
                    fillOutputBuffer(info);
                }
            }
#endif //MTK_AOSP_ENHANCEMENT
        }
    }

    while (mState != ERROR && !mNoMoreOutputData && mFilledBuffers.empty()) {
        if ((err = waitForBufferFilled_l()) != OK) {
#ifdef MTK_AOSP_ENHANCEMENT
            CODEC_LOGE("timeout in %d  state", mState);
#endif //MTK_AOSP_ENHANCEMENT
            return err;
        }
    }

    if (mState == ERROR) {
#ifdef MTK_AOSP_ENHANCEMENT
        if ((mFinalStatus == ERROR_UNSUPPORTED_VIDEO) || (mFinalStatus == ERROR_UNSUPPORTED_AUDIO)) {
            return mFinalStatus;
        }
#endif //MTK_AOSP_ENHANCEMENT
        return UNKNOWN_ERROR;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (mOutputPortSettingsHaveChanged) {
        mOutputPortSettingsHaveChanged = false;
        ALOGD("INFO_FORMAT_CHANGED");
        return INFO_FORMAT_CHANGED;
    }
#endif //MTK_AOSP_ENHANCEMENT

    if (mFilledBuffers.empty()) {
#ifdef MTK_AOSP_ENHANCEMENT
        CODEC_LOGE("read() final return");//for EOS may hang
        if (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.VORBIS"))
        {
            return ERROR_END_OF_STREAM;
        }
        else
#endif //MTK_AOSP_ENHANCEMENT
        return mSignalledEOS ? mFinalStatus : ERROR_END_OF_STREAM;
    }
#ifndef MTK_AOSP_ENHANCEMENT
    if (mOutputPortSettingsHaveChanged) {
        mOutputPortSettingsHaveChanged = false;

        return INFO_FORMAT_CHANGED;
    }
#endif //MTK_AOSP_ENHANCEMENT
    size_t index = *mFilledBuffers.begin();
#ifdef MTK_AOSP_ENHANCEMENT
    Vector<BufferInfo> *buftest = &mPortBuffers[kPortIndexOutput];
//    CHECK(index < buftest->size());
    if (index >= buftest->size()) {
        CODEC_LOGE("index %zu > size %zu", index, buftest->size());
     }
#endif //MTK_AOSP_ENHANCEMENT
    mFilledBuffers.erase(mFilledBuffers.begin());

#ifdef MTK_AOSP_ENHANCEMENT
    if (mQueueWaiting)
    {
        mBufferSent.signal();
    }
    if (index >= buftest->size()) {
         return UNKNOWN_ERROR;
    }
#endif //MTK_AOSP_ENHANCEMENT

    BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(index);
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    info->mStatus = OWNED_BY_CLIENT;

    info->mMediaBuffer->add_ref();
    if (mSkipCutBuffer != NULL) {
        mSkipCutBuffer->submit(info->mMediaBuffer);
    }
    *buffer = info->mMediaBuffer;

#ifdef MTK_AUDIO_DDPLUS_SUPPORT
    if (mDolbyProcessedAudioStateChanged) {
        mDolbyProcessedAudioStateChanged = false;
        return mDolbyProcessedAudio
            ? INFO_DOLBY_PROCESSED_AUDIO_START
            : INFO_DOLBY_PROCESSED_AUDIO_STOP;
    }
#endif  // DOLBY_END
    return OK;
}

void OMXCodec::signalBufferReturned(MediaBuffer *buffer) {
    Mutex::Autolock autoLock(mLock);

    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);

        if (info->mMediaBuffer == buffer) {
#ifdef MTK_AOSP_ENHANCEMENT
            if (mState != RECONFIGURING)
            {
                if (mPortStatus[kPortIndexOutput] == SHUTTING_DOWN){
                    //mpeg4Writer stops encoder before stopping itself makes this possible
                    CODEC_LOGW("Buffer %p is release when mPortStatus[kPortIndexOutput] is SHUTTING_DOWN",
                        buffer);
                }
                else{
                    CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)ENABLED);
                }
            }
#else //MTK_AOSP_ENHANCEMENT
            CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)ENABLED);
#endif //MTK_AOSP_ENHANCEMENT
            CHECK_EQ((int)info->mStatus, (int)OWNED_BY_CLIENT);

            info->mStatus = OWNED_BY_US;

#ifdef MTK_AOSP_ENHANCEMENT
            if (mState == RECONFIGURING)
            {
                CODEC_LOGE("freeBuffer from signalBufferReturned");
                freeBuffer(kPortIndexOutput, i);
            }
            else
            {
#endif //MTK_AOSP_ENHANCEMENT
            if (buffer->graphicBuffer() == 0) {
                fillOutputBuffer(info);
            } else {
                sp<MetaData> metaData = info->mMediaBuffer->meta_data();
                int32_t rendered = 0;
                if (!metaData->findInt32(kKeyRendered, &rendered)) {
                    rendered = 0;
                }
                if (!rendered) {
                    status_t err = cancelBufferToNativeWindow(info);
                    if (err < 0) {
                        return;
                    }
                }

                info->mStatus = OWNED_BY_NATIVE_WINDOW;

                // Dequeue the next buffer from the native window.
                BufferInfo *nextBufInfo = dequeueBufferFromNativeWindow();
                if (nextBufInfo == 0) {
                    return;
                }

                // Give the buffer to the OMX node to fill.
                fillOutputBuffer(nextBufInfo);
            }
#ifdef MTK_AOSP_ENHANCEMENT
            }
#endif //MTK_AOSP_ENHANCEMENT
            return;
        }
    }

    CHECK(!"should not be here.");
}

void OMXCodec::dumpPortStatus(OMX_U32 portIndex) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    printf("%s Port = {\n", portIndex == kPortIndexInput ? "Input" : "Output");

    CHECK((portIndex == kPortIndexInput && def.eDir == OMX_DirInput)
          || (portIndex == kPortIndexOutput && def.eDir == OMX_DirOutput));

    printf("  nBufferCountActual = %" PRIu32 "\n", def.nBufferCountActual);
    printf("  nBufferCountMin = %" PRIu32 "\n", def.nBufferCountMin);
    printf("  nBufferSize = %" PRIu32 "\n", def.nBufferSize);

    switch (def.eDomain) {
        case OMX_PortDomainImage:
        {
            const OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

            printf("\n");
            printf("  // Image\n");
            printf("  nFrameWidth = %" PRIu32 "\n", imageDef->nFrameWidth);
            printf("  nFrameHeight = %" PRIu32 "\n", imageDef->nFrameHeight);
            printf("  nStride = %" PRIu32 "\n", imageDef->nStride);

            printf("  eCompressionFormat = %s\n",
                   asString(imageDef->eCompressionFormat));

            printf("  eColorFormat = %s\n",
                   asString(imageDef->eColorFormat));

            break;
        }

        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *videoDef = &def.format.video;

            printf("\n");
            printf("  // Video\n");
            printf("  nFrameWidth = %" PRIu32 "\n", videoDef->nFrameWidth);
            printf("  nFrameHeight = %" PRIu32 "\n", videoDef->nFrameHeight);
            printf("  nStride = %" PRIu32 "\n", videoDef->nStride);

            printf("  eCompressionFormat = %s\n",
                   asString(videoDef->eCompressionFormat));

            printf("  eColorFormat = %s\n",
                   asString(videoDef->eColorFormat));

            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audioDef = &def.format.audio;

            printf("\n");
            printf("  // Audio\n");
            printf("  eEncoding = %s\n",
                   asString(audioDef->eEncoding));

            if (audioDef->eEncoding == OMX_AUDIO_CodingPCM) {
                OMX_AUDIO_PARAM_PCMMODETYPE params;
                InitOMXParams(&params);
                params.nPortIndex = portIndex;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                CHECK_EQ(err, (status_t)OK);

                printf("  nSamplingRate = %" PRIu32 "\n", params.nSamplingRate);
                printf("  nChannels = %" PRIu32 "\n", params.nChannels);
                printf("  bInterleaved = %d\n", params.bInterleaved);
                printf("  nBitPerSample = %" PRIu32 "\n", params.nBitPerSample);

                printf("  eNumData = %s\n",
                       params.eNumData == OMX_NumericalDataSigned
                        ? "signed" : "unsigned");

                printf("  ePCMMode = %s\n", asString(params.ePCMMode));
            } else if (audioDef->eEncoding == OMX_AUDIO_CodingAMR) {
                OMX_AUDIO_PARAM_AMRTYPE amr;
                InitOMXParams(&amr);
                amr.nPortIndex = portIndex;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioAmr, &amr, sizeof(amr));
                CHECK_EQ(err, (status_t)OK);

                printf("  nChannels = %" PRIu32 "\n", amr.nChannels);
                printf("  eAMRBandMode = %s\n",
                        asString(amr.eAMRBandMode));
                printf("  eAMRFrameFormat = %s\n",
                        asString(amr.eAMRFrameFormat));
            }

            break;
        }

        default:
        {
            printf("  // Unknown\n");
            break;
        }
    }

    printf("}\n");
}

status_t OMXCodec::initNativeWindow() {
    // Enable use of a GraphicBuffer as the output for this node.  This must
    // happen before getting the IndexParamPortDefinition parameter because it
    // will affect the pixel format that the node reports.
    status_t err = mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_TRUE);
    if (err != 0) {
        return err;
    }

    return OK;
}

void OMXCodec::initNativeWindowCrop() {
    int32_t left, top, right, bottom;

    CHECK(mOutputFormat->findRect(
                        kKeyCropRect,
                        &left, &top, &right, &bottom));

    android_native_rect_t crop;
    crop.left = left;
    crop.top = top;
    crop.right = right + 1;
    crop.bottom = bottom + 1;

    // We'll ignore any errors here, if the surface is
    // already invalid, we'll know soon enough.
    native_window_set_crop(mNativeWindow.get(), &crop);
}

#ifdef MTK_AOSP_ENHANCEMENT
status_t OMXCodec::initOutputFormat(const sp<MetaData> &inputFormat) {
#else
void OMXCodec::initOutputFormat(const sp<MetaData> &inputFormat) {
#endif
    mOutputFormat = new MetaData;
    mOutputFormat->setCString(kKeyDecoderComponent, mComponentName);
    if (mIsEncoder) {
        int32_t timeScale;
        if (inputFormat->findInt32(kKeyTimeScale, &timeScale)) {
            mOutputFormat->setInt32(kKeyTimeScale, timeScale);
        }
    }

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    switch (def.eDomain) {
        case OMX_PortDomainImage:
        {
            OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;
            CHECK_EQ((int)imageDef->eCompressionFormat,
                     (int)OMX_IMAGE_CodingUnused);

            mOutputFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
            mOutputFormat->setInt32(kKeyColorFormat, imageDef->eColorFormat);
            mOutputFormat->setInt32(kKeyWidth, imageDef->nFrameWidth);
            mOutputFormat->setInt32(kKeyHeight, imageDef->nFrameHeight);
            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audio_def = &def.format.audio;

            if (audio_def->eEncoding == OMX_AUDIO_CodingPCM) {
                OMX_AUDIO_PARAM_PCMMODETYPE params;
                InitOMXParams(&params);
                params.nPortIndex = kPortIndexOutput;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                CHECK_EQ(err, (status_t)OK);

                CHECK_EQ((int)params.eNumData, (int)OMX_NumericalDataSigned);
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_HIGH_RESOLUTION_AUDIO_SUPPORT
                int32_t bitWidth;
                // M: modify for the dirty memory cause the CHECK_EQ NE
                if ((inputFormat->findInt32(kKeyBitWidth, &bitWidth)) && bitWidth == 24)
                {
                    SLOGD("bitWidth is 24bit");
                    mOutputFormat->setInt32(kKeyBitWidth, bitWidth);
                    params.nBitPerSample = 32;
                    //ALOGD("set outpcmparameter bitperSample:%d",params.nBitPerSample);
                    err = mOMX->setParameter(mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                    CHECK_EQ((status_t)OK, err);
                }
#endif
#else
                CHECK_EQ(params.nBitPerSample, 16u);
#endif
                CHECK_EQ((int)params.ePCMMode, (int)OMX_AUDIO_PCMModeLinear);

                int32_t numChannels, sampleRate;
                inputFormat->findInt32(kKeyChannelCount, &numChannels);
                inputFormat->findInt32(kKeySampleRate, &sampleRate);

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_RAW_SUPPORT
                int32_t channelMask;
                if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.RAW"))
                {
                    if (inputFormat->findInt32(kKeyChannelMask, &channelMask))
                    {
                        SLOGD("Raw channelMask is 0x%x", channelMask);
                        mOutputFormat->setInt32(kKeyChannelMask, channelMask);
                    }
                }
#endif //MTK_AUDIO_RAW_SUPPORT
#ifdef MTK_SWIP_WMAPRO
                if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.WMAPRO"))
                {
                    OMX_U32 channelMask = 0;
                    err = mOMX->getParameter(
                              mNode, OMX_IndexParamAudioWmaProfile, &channelMask, sizeof(channelMask));
                    CHECK_EQ(err, (status_t)OK);
                    SLOGD("WMAPro channelMask is 0x%x", channelMask);
                    mOutputFormat->setInt32(kKeyChannelMask, channelMask);
                }
#endif //MTK_SWIP_WMAPRO
#endif //MTK_AOSP_ENHANCEMENT
                if ((OMX_U32)numChannels != params.nChannels) {
                    ALOGV("Codec outputs a different number of channels than "
                         "the input stream contains (contains %d channels, "
                         "codec outputs %u channels).",
                         numChannels, params.nChannels);
                }

                if (sampleRate != (int32_t)params.nSamplingRate) {
                    ALOGV("Codec outputs at different sampling rate than "
                         "what the input stream contains (contains data at "
                         "%d Hz, codec outputs %u Hz)",
                         sampleRate, params.nSamplingRate);
                }

                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_APE_SUPPORT
                if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_APE, mMIME))
                {
                    mOutputFormat->setCString(kKeyApeFlag, MEDIA_MIMETYPE_AUDIO_APE);
                    ///LOGE("set ape tag................");
                }
#endif
#endif //MTK_AOSP_ENHANCEMENT
                // Use the codec-advertised number of channels, as some
                // codecs appear to output stereo even if the input data is
                // mono. If we know the codec lies about this information,
                // use the actual number of channels instead.
                mOutputFormat->setInt32(
                        kKeyChannelCount,
                        (mQuirks & kDecoderLiesAboutNumberOfChannels)
                            ? numChannels : params.nChannels);
#ifdef MTK_AOSP_ENHANCEMENT
                if (!strncmp(mComponentName, "OMX.MTK.AUDIO.DECODER.", 22))
                {
                    if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.AAC"))
                    {
                        mOutputFormat->setInt32(kKeySampleRate, params.nSamplingRate);
#ifdef AAC_MULTI_CH_SUPPORT
                        if (params.nChannels > 2)
                        {
                            int32_t channelMask = 0;
                            getAACChannelMask(&channelMask, params.nChannels);
                            mOutputFormat->setInt32(kKeyChannelMask, channelMask);
                        }
#endif //AAC_MULTI_CH_SUPPORT
                    }
                    else
                    {
                        mOutputFormat->setInt32(kKeySampleRate, sampleRate);
                        if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.VORBIS"))
                        {
                            mOutputFormat->setCString(kKeyVorbisFlag, MEDIA_MIMETYPE_AUDIO_VORBIS);
                            if (numChannels > 2)
                            {
#ifndef MTK_SWIP_VORBIS
                                SLOGE("Tremolo does not support multi channel");
#endif
                            }
                        }
                    }
                }
                else
                {
#endif //MTK_AOSP_ENHANCEMENT
                    mOutputFormat->setInt32(kKeySampleRate, params.nSamplingRate);
#ifdef MTK_AOSP_ENHANCEMENT
                }
#endif //MTK_AOSP_ENHANCEMENT
            }
            else if (audio_def->eEncoding == OMX_AUDIO_CodingAMR)
            {
                OMX_AUDIO_PARAM_AMRTYPE amr;
                InitOMXParams(&amr);
                amr.nPortIndex = kPortIndexOutput;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioAmr, &amr, sizeof(amr));
                CHECK_EQ(err, (status_t)OK);

                CHECK_EQ(amr.nChannels, 1u);
                mOutputFormat->setInt32(kKeyChannelCount, 1);

                if (amr.eAMRBandMode >= OMX_AUDIO_AMRBandModeNB0
                    && amr.eAMRBandMode <= OMX_AUDIO_AMRBandModeNB7) {
                    mOutputFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AMR_NB);
                    mOutputFormat->setInt32(kKeySampleRate, 8000);
                } else if (amr.eAMRBandMode >= OMX_AUDIO_AMRBandModeWB0
                            && amr.eAMRBandMode <= OMX_AUDIO_AMRBandModeWB8) {
                    mOutputFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AMR_WB);
                    mOutputFormat->setInt32(kKeySampleRate, 16000);
                } else {
                    CHECK(!"Unknown AMR band mode.");
                }
            } else if (audio_def->eEncoding == OMX_AUDIO_CodingAAC) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
                int32_t numChannels, sampleRate, bitRate;
                inputFormat->findInt32(kKeyChannelCount, &numChannels);
                inputFormat->findInt32(kKeySampleRate, &sampleRate);
                inputFormat->findInt32(kKeyBitRate, &bitRate);
#ifdef MTK_AOSP_ENHANCEMENT
                int32_t aacProfile = OMX_AUDIO_AACObjectLC;
                inputFormat->findInt32(kKeyAACProfile, &aacProfile);
                mOutputFormat->setInt32(kKeyAACProfile, aacProfile);
#endif //MTK_AOSP_ENHANCEMENT
                mOutputFormat->setInt32(kKeyChannelCount, numChannels);
                mOutputFormat->setInt32(kKeySampleRate, sampleRate);
                mOutputFormat->setInt32(kKeyBitRate, bitRate);
            } else if (audio_def->eEncoding ==
                    (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingAndroidAC3) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
                int32_t numChannels, sampleRate, bitRate;
                inputFormat->findInt32(kKeyChannelCount, &numChannels);
                inputFormat->findInt32(kKeySampleRate, &sampleRate);
                inputFormat->findInt32(kKeyBitRate, &bitRate);
                mOutputFormat->setInt32(kKeyChannelCount, numChannels);
                mOutputFormat->setInt32(kKeySampleRate, sampleRate);
                mOutputFormat->setInt32(kKeyBitRate, bitRate);
            }
            //MTK80721 2011-08-17 Vorbis Encoder
#ifdef MTK_AOSP_ENHANCEMENT
            else if (audio_def->eEncoding == OMX_AUDIO_CodingVORBIS)
            {
                mOutputFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_VORBIS);
            }
#ifdef HAVE_ADPCMENCODE_FEATURE
            else if (audio_def->eEncoding == OMX_AUDIO_CodingADPCM)
            {
                OMX_AUDIO_PARAM_ADPCMTYPE adpcm;
                InitOMXParams(&adpcm);
                adpcm.nPortIndex = kPortIndexOutput;

                err = mOMX->getParameter(
                          mNode, OMX_IndexParamAudioAdpcm, &adpcm, sizeof(adpcm));
                CHECK_EQ(err, (status_t)OK);

                mOutputFormat->setCString(kKeyMIMEType, adpcm.nFormatTag == WAVE_FORMAT_MS_ADPCM ? MEDIA_MIMETYPE_AUDIO_MS_ADPCM : MEDIA_MIMETYPE_AUDIO_DVI_IMA_ADPCM);
                mOutputFormat->setInt32(kKeyChannelCount, adpcm.nChannelCount);
                mOutputFormat->setInt32(kKeySampleRate, adpcm.nSamplesPerSec);
                mOutputFormat->setInt32(kKeyBlockAlign, adpcm.nBlockAlign);
                mOutputFormat->setInt32(kKeyBitsPerSample, adpcm.nBitsPerSample);
                mOutputFormat->setData(kKeyExtraDataPointer, 0, adpcm.pExtendData, adpcm.nExtendDataSize);

                SLOGD("ADPCM OMXCodec mime type is %d", adpcm.nFormatTag);
                SLOGD("ADPCM OMXCodec num_channels is %d", adpcm.nChannelCount);
                SLOGD("ADPCM OMXCodec sample_rate is %d", adpcm.nSamplesPerSec);
                SLOGD("ADPCM OMXCodec block_align is %d", adpcm.nBlockAlign);
                SLOGD("ADPCM OMXCodec bits_per_sample is %d", adpcm.nBitsPerSample);
                SLOGD("ADPCM OMXCodec extra_data_size is %d", adpcm.nExtendDataSize);
            }
#endif //HAVE_ADPCMENCODE_FEATURE
#endif //MTK_AOSP_ENHANCEMENT
            else
            {
                CHECK(!"Should not be here. Unknown audio encoding.");
            }
            break;
        }

        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

            if (video_def->eCompressionFormat == OMX_VIDEO_CodingUnused) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingMPEG4) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingH263) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
#ifdef MTK_AOSP_ENHANCEMENT
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingAVC) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
#ifdef MTK_VIDEO_HEVC_SUPPORT
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingHEVC) {
                mOutputFormat->setCString(
                    kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_HEVC);
#endif
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingVP8) {
                mOutputFormat->setCString(
                    kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VP8);
#endif //MTK_AOSP_ENHANCEMENT
            } else {
                CHECK(!"Unknown compression format.");
            }

            mOutputFormat->setInt32(kKeyWidth, video_def->nFrameWidth);
            mOutputFormat->setInt32(kKeyHeight, video_def->nFrameHeight);
#ifdef MTK_AOSP_ENHANCEMENT
            mOutputFormat->setInt32(kKeyStride, video_def->nStride);
            mOutputFormat->setInt32(kKeySliceHeight, video_def->nSliceHeight);
            CODEC_LOGI("InitOutputFormat width(%d), height(%d), stride(%d), sliceheight(%d), colorformat(%x)", video_def->nFrameWidth, video_def->nFrameHeight, video_def->nStride, video_def->nSliceHeight, video_def->eColorFormat);
#endif//MTK_AOSP_ENHANCEMENT
            mOutputFormat->setInt32(kKeyColorFormat, video_def->eColorFormat);

            if (!mIsEncoder) {
                OMX_CONFIG_RECTTYPE rect;
                InitOMXParams(&rect);
                rect.nPortIndex = kPortIndexOutput;
                status_t err =
                        mOMX->getConfig(
                            mNode, OMX_IndexConfigCommonOutputCrop,
                            &rect, sizeof(rect));

                CODEC_LOGI("video dimensions are %u x %u",
                        video_def->nFrameWidth, video_def->nFrameHeight);

                if (err == OK) {
                    CHECK_GE(rect.nLeft, 0);
                    CHECK_GE(rect.nTop, 0);
                    CHECK_GE(rect.nWidth, 0u);
                    CHECK_GE(rect.nHeight, 0u);
                    CHECK_LE(rect.nLeft + rect.nWidth - 1, video_def->nFrameWidth);
                    CHECK_LE(rect.nTop + rect.nHeight - 1, video_def->nFrameHeight);

                    mOutputFormat->setRect(
                            kKeyCropRect,
                            rect.nLeft,
                            rect.nTop,
                            rect.nLeft + rect.nWidth - 1,
                            rect.nTop + rect.nHeight - 1);

                    CODEC_LOGI("Crop rect is %u x %u @ (%d, %d)",
                            rect.nWidth, rect.nHeight, rect.nLeft, rect.nTop);
                } else {
                    mOutputFormat->setRect(
                            kKeyCropRect,
                            0, 0,
                            video_def->nFrameWidth - 1,
                            video_def->nFrameHeight - 1);
                }

                if (mNativeWindow != NULL) {
                     initNativeWindowCrop();
                }
            }
            break;
        }

        default:
        {
#ifdef MTK_AOSP_ENHANCEMENT
            //SD card removal->binder transaction fail->bad value
            CODEC_LOGE("should not be here, neither audio nor video");
            return BAD_VALUE;
#else
            CHECK(!"should not be here, neither audio nor video.");
            break;
#endif
        }
    }

    // If the input format contains rotation information, flag the output
    // format accordingly.

    int32_t rotationDegrees;
    if (mSource->getFormat()->findInt32(kKeyRotation, &rotationDegrees)) {
        mOutputFormat->setInt32(kKeyRotation, rotationDegrees);
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (mIsVideoDecoder) {
        mOutputFormat->setInt32(kKeyAspectRatioWidth, mVideoAspectRatioWidth);
        mOutputFormat->setInt32(kKeyAspectRatioHeight, mVideoAspectRatioHeight);
    }

    return OK;
#endif //MTK_AOSP_ENHANCEMENT
}

status_t OMXCodec::pause() {
    Mutex::Autolock autoLock(mLock);
#ifdef MTK_AOSP_ENHANCEMENT
    CODEC_LOGI("PAUSE++++++++++++++");
#endif //MTK_AOSP_ENHANCEMENT
    mPaused = true;

    return OK;
}

#ifdef MTK_AOSP_ENHANCEMENT
void OMXCodec::resume()
{
    Mutex::Autolock autoLock(mLock);

    CODEC_LOGE("RESUME--------------------");
    mPaused = false;

}
#endif //MTK_AOSP_ENHANCEMENT

////////////////////////////////////////////////////////////////////////////////

status_t QueryCodecs(
        const sp<IOMX> &omx,
        const char *mime, bool queryDecoders, bool hwCodecOnly,
        Vector<CodecCapabilities> *results) {
    Vector<OMXCodec::CodecNameAndQuirks> matchingCodecs;
    results->clear();

    OMXCodec::findMatchingCodecs(mime,
            !queryDecoders /*createEncoder*/,
            NULL /*matchComponentName*/,
            hwCodecOnly ? OMXCodec::kHardwareCodecsOnly : 0 /*flags*/,
            &matchingCodecs);

    for (size_t c = 0; c < matchingCodecs.size(); c++) {
        const char *componentName = matchingCodecs.itemAt(c).mName.string();

        results->push();
        CodecCapabilities *caps = &results->editItemAt(results->size() - 1);

        status_t err =
            QueryCodec(omx, componentName, mime, !queryDecoders, caps);

        if (err != OK) {
            results->removeAt(results->size() - 1);
        }
    }

    return OK;
}

status_t QueryCodec(
        const sp<IOMX> &omx,
        const char *componentName, const char *mime,
        bool isEncoder,
        CodecCapabilities *caps) {
    bool isVideo = !strncasecmp(mime, "video/", 6);
    if (strncmp(componentName, "OMX.", 4))
    {
        // Not an OpenMax component but a software codec.
        caps->mFlags = 0;
        caps->mComponentName = componentName;
        return OK;
    }

    sp<OMXCodecObserver> observer = new OMXCodecObserver;
    IOMX::node_id node;
    status_t err = omx->allocateNode(componentName, observer, &node);

    if (err != OK) {
        return err;
    }

    OMXCodec::setComponentRole(omx, node, isEncoder, mime);

    caps->mFlags = 0;
    caps->mComponentName = componentName;

    // NOTE: OMX does not provide a way to query AAC profile support
    if (isVideo) {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
        InitOMXParams(&param);

        param.nPortIndex = !isEncoder ? 0 : 1;

        for (param.nProfileIndex = 0;; ++param.nProfileIndex) {
            err = omx->getParameter(
                    node, OMX_IndexParamVideoProfileLevelQuerySupported,
                    &param, sizeof(param));

            if (err != OK) {
                break;
            }

            CodecProfileLevel profileLevel;
            profileLevel.mProfile = param.eProfile;
            profileLevel.mLevel = param.eLevel;

            caps->mProfileLevels.push(profileLevel);
        }

        // Color format query
        // return colors in the order reported by the OMX component
        // prefix "flexible" standard ones with the flexible equivalent
        OMX_VIDEO_PARAM_PORTFORMATTYPE portFormat;
        InitOMXParams(&portFormat);
        portFormat.nPortIndex = !isEncoder ? 1 : 0;
        for (portFormat.nIndex = 0;; ++portFormat.nIndex)  {
            err = omx->getParameter(
                    node, OMX_IndexParamVideoPortFormat,
                    &portFormat, sizeof(portFormat));
            if (err != OK) {
                break;
            }

            OMX_U32 flexibleEquivalent;
            if (ACodec::isFlexibleColorFormat(
                        omx, node, portFormat.eColorFormat, false /* usingNativeWindow */,
                        &flexibleEquivalent)) {
                bool marked = false;
                for (size_t i = 0; i < caps->mColorFormats.size(); i++) {
                    if (caps->mColorFormats.itemAt(i) == flexibleEquivalent) {
                        marked = true;
                        break;
                    }
                }
                if (!marked) {
                    ALOGI("mColorFormats.push flexibleEquivalent = %x", flexibleEquivalent);
                    //ignore push flexible format in turkey
                    caps->mColorFormats.push(flexibleEquivalent);
                }
            }
            caps->mColorFormats.push(portFormat.eColorFormat);
        }
    }

    if (isVideo && !isEncoder) {
        if (omx->storeMetaDataInBuffers(
                    node, 1 /* port index */, OMX_TRUE) == OK ||
            omx->prepareForAdaptivePlayback(
                    node, 1 /* port index */, OMX_TRUE,
                    1280 /* width */, 720 /* height */) == OK) {
            caps->mFlags |= CodecCapabilities::kFlagSupportsAdaptivePlayback;
        }
    }

    CHECK_EQ(omx->freeNode(node), (status_t)OK);

    return OK;
}

status_t QueryCodecs(
        const sp<IOMX> &omx,
        const char *mimeType, bool queryDecoders,
        Vector<CodecCapabilities> *results) {
    return QueryCodecs(omx, mimeType, queryDecoders, false /*hwCodecOnly*/, results);
}

// These are supposed be equivalent to the logic in
// "audio_channel_out_mask_from_count".
status_t getOMXChannelMapping(size_t numChannels, OMX_AUDIO_CHANNELTYPE map[]) {
    switch (numChannels) {
        case 1:
            map[0] = OMX_AUDIO_ChannelCF;
            break;
        case 2:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            break;
        case 3:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            break;
        case 4:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelLR;
            map[3] = OMX_AUDIO_ChannelRR;
            break;
        case 5:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLR;
            map[4] = OMX_AUDIO_ChannelRR;
            break;
        case 6:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            break;
        case 7:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelCS;
            break;
        case 8:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelLS;
            map[7] = OMX_AUDIO_ChannelRS;
            break;
        default:
            return -EINVAL;
    }

    return OK;
}

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef AAC_MULTI_CH_SUPPORT
void getAACChannelMask(int32_t *channelMask, int32_t numChannels)
{
    enum
    {
        FRONT_LEFT        = 0x1,
        FRONT_RIGHT       = 0x2,
        FRONT_CENTER      = 0x4,
        LOW_FREQUENCY     = 0x8,
        BACK_LEFT         = 0x10,
        BACK_RIGHT        = 0x20,
        FRONT_LEFT_OF_CENTER  = 0x40,
        FRONT_RIGHT_OF_CENTER = 0x80,
        BACK_CENTER           = 0x100,
        SIDE_LEFT             = 0x200,
        SIDE_RIGHT            = 0x400,
    };
    switch (numChannels)
    {
        case 3:
            *channelMask = FRONT_LEFT |
                           FRONT_RIGHT |
                           FRONT_CENTER;
            break;
        case 4:
            *channelMask = FRONT_LEFT |
                           FRONT_RIGHT |
                           FRONT_CENTER |
                           BACK_CENTER;
            break;
        case 5:
            *channelMask = FRONT_LEFT |
                           FRONT_RIGHT |
                           FRONT_CENTER |
                           BACK_LEFT |
                           BACK_RIGHT;
            break;
        case 6: //5.1 ch
            *channelMask = FRONT_LEFT |
                           FRONT_RIGHT |
                           FRONT_CENTER |
                           LOW_FREQUENCY |
                           BACK_LEFT |
                           BACK_RIGHT;
            break;
        case 8: //7.1 ch
            *channelMask = FRONT_LEFT |
                           FRONT_RIGHT |
                           FRONT_CENTER |
                           LOW_FREQUENCY |
                           BACK_LEFT |
                           BACK_RIGHT |
                           SIDE_LEFT |
                           SIDE_RIGHT;
            break;
    }
    ALOGD("channelMask =%d", *channelMask);
}
#endif //AAC_MULTI_CH_SUPPORT

status_t OMXCodec::vorbisSizeValid(int size)
{
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = OMX_DirInput;
    status_t err = mOMX->getParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ((status_t)OK, err);

    if (size > (int)def.nBufferSize)
    {
        SLOGE("vorbisbooks size exceeds input buffer size");
        return ERROR_UNSUPPORTED;
    }
    return OK;
}
status_t OMXCodec::setupAACFormat(int numChannels,int sampleRate,int bitRate,int aacProfile,int isADTS,const sp<MetaData> &meta) {
    ALOGD("setAACFormat_refine");
    if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.AAC"))
        {
            bool bIsAACADIF = false;
            int32_t nIsAACADIF;
            if (meta->findInt32(kKeyIsAACADIF, &nIsAACADIF))
            {
                if (0 == nIsAACADIF)
                {
                    bIsAACADIF = false;
                }
                else
                {
                    bIsAACADIF = true;
                }
            }
            status_t errAAC;
            if (bIsAACADIF)
            {
                OMX_AUDIO_PARAM_AACPROFILETYPE profileAAC;
                InitOMXParams(&profileAAC);
                profileAAC.nPortIndex = kPortIndexInput;
                errAAC = mOMX->getParameter(
                             mNode, OMX_IndexParamAudioAac, &profileAAC, sizeof(profileAAC));
                CHECK_EQ((status_t)OK, errAAC);
                profileAAC.nChannels = numChannels;
                profileAAC.nSampleRate = sampleRate;
                profileAAC.eAACStreamFormat = OMX_AUDIO_AACStreamFormatADIF;
                errAAC = mOMX->setParameter(
                             mNode, OMX_IndexParamAudioAac, &profileAAC, sizeof(profileAAC));
            }
            else
            {
                errAAC = setAACFormat(numChannels, sampleRate, bitRate, aacProfile, isADTS);
            }
            if (errAAC != OK)
            {
                CODEC_LOGE("setAACFormat() failed (errAAC = %d)", errAAC);
                return errAAC;
            }
        }
        else
        {
        status_t err = setAACFormat(numChannels, sampleRate, bitRate, aacProfile, isADTS);
            if (err != OK)
            {
                CODEC_LOGE("setAACFormat() failed (err = %d)", err);
                return err;
            }
    }

    return OK;
}

status_t OMXCodec::setupG711Format(int numChannels,const sp<MetaData> &meta){
    ALOGD("setupG711Format");
    status_t errG711;
    if (!strncmp(mComponentName, "OMX.MTK.AUDIO.DECODER.G711", 26))
        {
            bool IsG711 = false;
            OMX_AUDIO_PCMMODETYPE PCMMode;
            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_ALAW, mMIME))
            {
                PCMMode = OMX_AUDIO_PCMModeALaw ;
                IsG711 = true;
            }
            else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_MLAW, mMIME))
            {
                PCMMode = OMX_AUDIO_PCMModeMULaw ;
                IsG711 = true;
            }
            if (IsG711)
            {
                OMX_AUDIO_PARAM_PCMMODETYPE profileG711;
                InitOMXParams(&profileG711);
                profileG711.nPortIndex = kPortIndexInput;
                errG711 = mOMX->getParameter(
                                       mNode, OMX_IndexParamAudioPcm, &profileG711, sizeof(profileG711));
                CHECK_EQ((status_t)OK, errG711);
                int32_t nChs = 0, sR = 0;
                meta->findInt32(kKeyChannelCount, &nChs);
                meta->findInt32(kKeySampleRate, &sR);
                profileG711.nChannels = nChs;
                profileG711.nSamplingRate = sR;
                profileG711.ePCMMode = PCMMode;
                errG711 = mOMX->setParameter(
                              mNode, OMX_IndexParamAudioPcm, &profileG711, sizeof(profileG711));
                if (errG711 != OK)
                {
                    CODEC_LOGE("setG711Format() failed (errG711 = %d)", errG711);
                    return errG711;
                }
            }
        }
        else {
            int32_t sampleRate;
            if (!meta->findInt32(kKeySampleRate, &sampleRate)) {
                sampleRate = 8000;
            }
            setG711Format(sampleRate, numChannels);
        }
        return OK;

}

status_t OMXCodec::setupADPCMFormat(const sp<MetaData> &meta){
    ALOGD("setupADPCMFormat");
    status_t errADPCM = OK;
    if (!strncmp(mComponentName, "OMX.MTK.AUDIO.DECODER.ADPCM", 27))
            {
                SLOGD("start configure adpcm decodec!!!");
                OMX_AUDIO_ADPCMPARAM profileADPCM;
                InitOMXParams(&profileADPCM);
                profileADPCM.nPortIndex = kPortIndexInput;
                uint32_t type;

                errADPCM = mOMX->getParameter(mNode, OMX_IndexParamAudioAdpcm, &profileADPCM, sizeof(profileADPCM));
                CHECK_EQ((status_t)OK, errADPCM);

                profileADPCM.nFormatTag = (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MS_ADPCM, mMIME)) ? WAVE_FORMAT_MS_ADPCM : WAVE_FORMAT_DVI_IMA_ADPCM;
                CHECK(meta->findInt32(kKeyChannelCount, (int32_t *)&profileADPCM.nChannelCount));
                CHECK(meta->findInt32(kKeySampleRate, (int32_t *)&profileADPCM.nSamplesPerSec));
                CHECK(meta->findInt32(kKeyBlockAlign, (int32_t *)&profileADPCM.nBlockAlign));
                CHECK(meta->findInt32(kKeyBitsPerSample, (int32_t *)&profileADPCM.nBitsPerSample));
                CHECK(meta->findData(kKeyExtraDataPointer, &type, (const void **)&profileADPCM.pExtendData, (size_t *)&profileADPCM.nExtendDataSize));

                SLOGD("ADPCM Decode profileADPCM.nBlockAlign is %d", profileADPCM.nBlockAlign);
                SLOGD("ADPCM Decode profileADPCM.nBitsPerSample is %d", profileADPCM.nBitsPerSample);
                SLOGD("ADPCM Decode profileADPCM.nExtendDataSize is %d", profileADPCM.nExtendDataSize);
                SLOGD("ADPCM Decode profileADPCM.pExtendData is 0x%lx", (unsigned long)profileADPCM.pExtendData);

                errADPCM = mOMX->setParameter(mNode, OMX_IndexParamAudioAdpcm, &profileADPCM, sizeof(profileADPCM));
                CHECK_EQ((status_t)OK, errADPCM);
            }
            else if (!strcmp(mComponentName, "OMX.MTK.AUDIO.ENCODER.ADPCM"))
            {
                SLOGD("start configure adpcm encodec!!!");
                OMX_AUDIO_PARAM_ADPCMTYPE profileADPCM;
                InitOMXParams(&profileADPCM);
                profileADPCM.nPortIndex = kPortIndexOutput;
                //uint32_t type;

                errADPCM = mOMX->getParameter(mNode, OMX_IndexParamAudioAdpcm, &profileADPCM, sizeof(profileADPCM));
                CHECK_EQ((status_t)OK, errADPCM);
                SLOGD("ADPCM Encode profileADPCM.nBitsPerSample is %d", profileADPCM.nBitsPerSample);

                profileADPCM.nFormatTag = (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MS_ADPCM, mMIME)) ? WAVE_FORMAT_MS_ADPCM : WAVE_FORMAT_DVI_IMA_ADPCM;
                CHECK(meta->findInt32(kKeyChannelCount, (int32_t *)&profileADPCM.nChannelCount));
                CHECK(meta->findInt32(kKeySampleRate, (int32_t *)&profileADPCM.nSamplesPerSec));

                SLOGD("ADPCM Encode profileADPCM.nChannelCount is %d", profileADPCM.nChannelCount);
                SLOGD("ADPCM Encode profileADPCM.nSamplesPerSec is %d", profileADPCM.nSamplesPerSec);

                errADPCM = mOMX->setParameter(mNode, OMX_IndexParamAudioAdpcm, &profileADPCM, sizeof(profileADPCM));
                CHECK_EQ((status_t)OK, errADPCM);
            }
            return errADPCM;
}

status_t OMXCodec::setupRawFormat(int numChannels,int sampleRate,const sp<MetaData> &meta){
            ALOGD("setupRawFormat");
            if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.RAW"))
            {
                int32_t endian = 1, bitWidth = 16, pcmType = 1, channelAssignment = 0, numericalType = 0;

                SLOGD("start configure mtk raw codec!!!");
                OMX_AUDIO_PARAM_RAWTYPE profileRAW;
                InitOMXParams(&profileRAW);
                profileRAW.nPortIndex = kPortIndexInput;
                //uint32_t type;

                status_t errRAW = mOMX->getParameter(mNode, OMX_IndexParamAudioRaw, &profileRAW, sizeof(profileRAW));
                CHECK_EQ((status_t)OK, errRAW);

                profileRAW.nChannels = numChannels;
                profileRAW.nSamplingRate = sampleRate;
                if (meta->findInt32(kKeyBitWidth, &bitWidth)) {
                    profileRAW.nBitPerSample = bitWidth;
                }
                if (meta->findInt32(kKeyChannelAssignment, &channelAssignment)) {
                    profileRAW.nChannelAssignment = channelAssignment;
                }

                meta->findInt32(kKeyEndian, &endian);
                meta->findInt32(kKeyPCMType, &pcmType);
                meta->findInt32(kKeyNumericalType, &numericalType);
                SLOGD("endian is %d, bitWidth is %d, pcmType is %d, channelAssignment is %d, numericalType is %d", endian, bitWidth, pcmType, channelAssignment, numericalType);

                switch(endian)
                {
                    case 1:
                        profileRAW.eEndian = OMX_EndianBig;
                        break;
                    case 2:
                        profileRAW.eEndian = OMX_EndianLittle;
                        break;
                    default:
                        SLOGD("Unknow eEndian Type, use default value");
                        profileRAW.eEndian = OMX_EndianLittle;
                }

                SLOGD("Config raw codec, pcmType is %d", pcmType);
                switch(pcmType)
                {
                    case 1:
                        profileRAW.eRawType = PCM_WAVE;
                        break;
                    case 2:
                        profileRAW.eRawType = PCM_BD;
                        break;
                    case 3:
                        profileRAW.eRawType = PCM_DVD_VOB;
                        break;
                    case 4:
                        profileRAW.eRawType = PCM_DVD_AOB;
                        break;
                    default:
                        SLOGE("unknow raw type!");
                }

                switch(numericalType)
                {
                    case 1:
                        profileRAW.eNumData = OMX_NumericalDataSigned;
                        break;
                    case 2:
                        profileRAW.eNumData = OMX_NumericalDataUnsigned;
                        break;
                    default:
                        SLOGE("default numerical type is OMX_NumericalDataSigned !");
                        profileRAW.eNumData = OMX_NumericalDataSigned;
                }
                errRAW = mOMX->setParameter(mNode, OMX_IndexParamAudioRaw, &profileRAW, sizeof(profileRAW));
                CHECK_EQ((status_t)OK, errRAW);
            }
            else
            {
                setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
            }
            return OK;

}

status_t OMXCodec::setupMp3Format(const sp<MetaData> &meta){
            ALOGD("setupMp3Format");
            status_t errMp3 = OK;
            if (!strcmp(mComponentName, "OMX.MTK.AUDIO.DECODER.MP3"))
            {
                OMX_AUDIO_PARAM_MP3TYPE profileMp3;
                InitOMXParams(&profileMp3);
                profileMp3.nPortIndex = kPortIndexInput;

                status_t errMp3 = mOMX->getParameter(
                                      mNode, OMX_IndexParamAudioMp3, &profileMp3, sizeof(profileMp3));
                CHECK_EQ((status_t)OK, errMp3);
                int32_t ch = 0, saR = 0, isFromMP3Extractor = 0;
                meta->findInt32(kKeyChannelCount, &ch);
                meta->findInt32(kKeySampleRate, &saR);

                OMX_MP3_CONFIG_TYPE defmp3;
                status_t err;
                err = mOMX->getParameter(
                          mNode, OMX_IndexVendorMtkMP3Decode, &defmp3, sizeof(defmp3));
                CHECK_EQ((int)err, (int)OK);
                meta->findInt32(kKeyIsFromMP3Extractor, &isFromMP3Extractor);
                if (isFromMP3Extractor == 1)
                {
                    mp3FrameCountInBuffer = MP3_MULTI_FRAME_COUNT_IN_ONE_INPUTBUFFER_FOR_PURE_AUDIO;
                    defmp3.nMaxBufFrameNum = MP3_MULTI_FRAME_COUNT_IN_ONE_OUTPUTBUFFER_FOR_PURE_AUDIO;
                    ALOGD("The frame is from MP3Extractor and set mp3FrameCountInBuffer is %d", mp3FrameCountInBuffer);
                }
                else
                {
                    mp3FrameCountInBuffer = MP3_MULTI_FRAME_COUNT_IN_ONE_INPUTBUFFER_FOR_VIDEO;
                    defmp3.nMaxBufFrameNum = MP3_MULTI_FRAME_COUNT_IN_ONE_OUTPUTBUFFER_FOR_VIDEO;
                    ALOGD("The frame is not from MP3Extractor and mp3FrameCountInBuffer use default value is %d", mp3FrameCountInBuffer);
                }
                err = mOMX->setParameter(
                          mNode, OMX_IndexVendorMtkMP3Decode, &defmp3, sizeof(defmp3));
                CHECK_EQ((int)err, (int)OK);
                CODEC_LOGI("Set MP3 Frame Count in one output buffer %d", defmp3.nMaxBufFrameNum);


                profileMp3.nChannels = ch;
                profileMp3.nSampleRate = saR;
                errMp3 = mOMX->setParameter(
                             mNode, OMX_IndexParamAudioMp3, &profileMp3, sizeof(profileMp3));
            }
            return errMp3;
}

status_t OMXCodec::setupFLACFormat(const sp<MetaData> &meta){
            ALOGD("setupFLACFormat");
            uint32_t type;
            status_t err = OK;
            typedef struct
            {
                unsigned min_blocksize, max_blocksize;
                unsigned min_framesize, max_framesize;
                unsigned sample_rate;
                unsigned channels;
                unsigned bits_per_sample;
                uint64_t total_samples;
                unsigned char md5sum[16];
                unsigned int mMaxBufferSize;
                bool      has_stream_info;
            } FLAC__StreamMetadata_Info_;
            const void *data;
            size_t size;
            CHECK(meta->findData(kKeyFlacMetaInfo, &type, &data, &size));


            OMX_AUDIO_PARAM_FLACTYPE profile;
            InitOMXParams(&profile);
            profile.nPortIndex = OMX_DirInput;

            err = mOMX->getParameter(
                               mNode, OMX_IndexParamAudioFlac, &profile, sizeof(profile));
            CHECK_EQ((status_t)OK, err);

            profile.channel_assignment =  OMX_AUDIO_FLAC__CHANNEL_ASSIGNMENT_LEFT_SIDE;
            profile.total_samples = ((FLAC__StreamMetadata_Info_ *)data)->total_samples;
            profile.min_framesize = ((FLAC__StreamMetadata_Info_ *)data)->min_framesize;
            profile.max_framesize = ((FLAC__StreamMetadata_Info_ *)data)->max_framesize;
            profile.nSampleRate = ((FLAC__StreamMetadata_Info_ *)data)->sample_rate;
            profile.min_blocksize = ((FLAC__StreamMetadata_Info_ *)data)->min_blocksize;
            profile.max_blocksize = ((FLAC__StreamMetadata_Info_ *)data)->max_blocksize;
            profile.nChannels = ((FLAC__StreamMetadata_Info_ *)data)->channels;
            profile.bits_per_sample = ((FLAC__StreamMetadata_Info_ *)data)->bits_per_sample;
            memcpy(profile.md5sum, ((FLAC__StreamMetadata_Info_ *)data)->md5sum, 16 * sizeof(OMX_U8));
            if (((FLAC__StreamMetadata_Info_ *)data)->has_stream_info == true)
            {
                profile.has_stream_info = OMX_TRUE;
            }
            else
            {
                profile.has_stream_info = OMX_FALSE;
            }

            SLOGV("kKeyFlacMetaInfo = %lld, %d, %d, %d, %d, %d, %d, %d", profile.total_samples, profile.min_framesize, profile.max_framesize,
                   profile.nSampleRate, profile.min_blocksize, profile.max_blocksize, profile.nChannels, profile.bits_per_sample);
            err = mOMX->setParameter(
                      mNode, OMX_IndexParamAudioFlac, &profile, sizeof(profile));
            OMX_PARAM_PORTDEFINITIONTYPE def;
            InitOMXParams(&def);
            def.nPortIndex = OMX_DirInput;

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((status_t)OK, err);

            if (def.nBufferSize < ((FLAC__StreamMetadata_Info_ *)data)->mMaxBufferSize) {
                def.nBufferSize = ((FLAC__StreamMetadata_Info_ *)data)->mMaxBufferSize;
            }
            err = mOMX->setParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

            OMX_PARAM_PORTDEFINITIONTYPE outputdef;
            InitOMXParams(&outputdef);
            outputdef.nPortIndex = OMX_DirOutput;

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &outputdef, sizeof(outputdef));
            CHECK_EQ((status_t)OK, err);

            if (outputdef.nBufferSize < profile.max_blocksize * 8 * 2)
                outputdef.nBufferSize = profile.max_blocksize * 8 * 2;
#ifdef MTK_HIGH_RESOLUTION_AUDIO_SUPPORT
            if (profile.bits_per_sample > 16) {
                outputdef.nBufferSize *= 2;
            }
#endif

            err = mOMX->setParameter(
                      mNode, OMX_IndexParamPortDefinition, &outputdef, sizeof(outputdef));

            ALOGD("err= %d", err);

            return err;
}

status_t OMXCodec::setupAPEFormat(const sp<MetaData> &meta){
            ALOGD("setupAPEFormat");
            OMX_AUDIO_PARAM_APETYPE profile;
            InitOMXParams(&profile);
            profile.nPortIndex = OMX_DirInput;
            status_t err = OK;
            err = mOMX->getParameter(
                               mNode, OMX_IndexParamAudioApe, &profile, sizeof(profile));
            CHECK_EQ((status_t)OK, err);

            CHECK(meta->findInt32(kkeyApechl, (int32_t *)&profile.channels));
            CHECK(meta->findInt32(kkeyApebit, (int32_t *)&profile.Bitrate));
            CHECK(meta->findInt32(kKeyBufferSize, (int32_t *)&profile.SourceBufferSize));
            CHECK(meta->findInt32(kKeySampleRate, (int32_t *)&profile.SampleRate));

            if (profile.SampleRate > 0)
            {
                profile.bps = (unsigned short)(profile.Bitrate / (profile.channels * profile.SampleRate));
            }
            else
            {
                profile.bps = 0;
            }
            CHECK(meta->findInt32(kKeyFileType, (int32_t *)&profile.fileversion));
            CHECK(meta->findInt32(kkeyComptype, (int32_t *)&profile.compressiontype));
            CHECK(meta->findInt32(kKeySamplesperframe, (int32_t *)&profile.blocksperframe));
            CHECK(meta->findInt32(kKeyTotalFrame, (int32_t *)&profile.totalframes));
            CHECK(meta->findInt32(kKeyFinalSample, (int32_t *)&profile.finalframeblocks));

            err = mOMX->setParameter(
                      mNode, OMX_IndexParamAudioApe, &profile, sizeof(profile));
            ///LOGD("err= %d",err);

            OMX_PARAM_PORTDEFINITIONTYPE def;
            InitOMXParams(&def);
            def.nPortIndex = OMX_DirInput;

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((status_t)OK, err);

            def.nBufferSize = profile.SourceBufferSize;
            err = mOMX->setParameter(
                      mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
            CHECK_EQ((status_t)OK, err);
#ifdef MTK_HIGH_RESOLUTION_AUDIO_SUPPORT
            if (profile.bps == 24)
            {
                InitOMXParams(&def);
                def.nPortIndex = OMX_DirOutput;
                err = mOMX->getParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
                CHECK_EQ((status_t)OK, err);
                def.nBufferSize <<= 1;
                err = mOMX->setParameter(mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
                CHECK_EQ((status_t)OK, err);
            }
#endif
            return err;

}

void OMXCodec::setupALACFormat(const sp<MetaData> &meta){
            ALOGD("setupALACFormat");
            int32_t numChannels = 0, sampleRate = 0, bitWidth = 0, numSamples = 0;

            CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
            CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

            setRawAudioFormat(kPortIndexOutput, sampleRate, numChannels);

            OMX_AUDIO_PARAM_ALACTYPE profileAlac;
            InitOMXParams(&profileAlac);
            profileAlac.nPortIndex = kPortIndexInput;

            status_t err = mOMX->getParameter(
                               mNode, OMX_IndexParamAudioAlac, &profileAlac, sizeof(profileAlac));
            CHECK_EQ((status_t)OK, err);

            profileAlac.nChannels   = numChannels;
            profileAlac.nSampleRate = sampleRate;
            if (meta->findInt32(kKeyNumSamples, &numSamples) && numSamples > 0)
            {
                profileAlac.nSamplesPerPakt = numSamples;
            }
            if (meta->findInt32(kKeyBitWidth, &bitWidth) && bitWidth > 0)
            {
                profileAlac.nBitsWidth  = bitWidth;
            }
            err = mOMX->setParameter(
                      mNode, OMX_IndexParamAudioAlac, &profileAlac, sizeof(profileAlac));
            CHECK_EQ((status_t)OK, err);

            OMX_PARAM_PORTDEFINITIONTYPE inputdef, outputdef;

            InitOMXParams(&inputdef);
            inputdef.nPortIndex = OMX_DirInput;

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &inputdef, sizeof(inputdef));
            CHECK_EQ((status_t)OK, err);

            inputdef.nBufferSize = profileAlac.nChannels * (profileAlac.nBitsWidth >> 3) * profileAlac.nSamplesPerPakt;
            err = mOMX->setParameter(mNode, OMX_IndexParamPortDefinition, &inputdef, sizeof(inputdef));
            CHECK_EQ((status_t)OK, err);

            InitOMXParams(&outputdef);
            outputdef.nPortIndex = OMX_DirOutput;

            err = mOMX->getParameter(
                      mNode, OMX_IndexParamPortDefinition, &outputdef, sizeof(outputdef));
            CHECK_EQ((status_t)OK, err);
            outputdef.nBufferSize = profileAlac.nChannels * 2 * profileAlac.nSamplesPerPakt;

            if (profileAlac.nBitsWidth > 16)
            {
                outputdef.nBufferSize <<= 1;
            }

            err = mOMX->setParameter(mNode, OMX_IndexParamPortDefinition, &outputdef, sizeof(outputdef));
            CHECK_EQ((status_t)OK, err);


}

uint32_t OMXCodec::getEmptyInputBufferCount()
{
    uint32_t bufferCount = 0;
    Vector<BufferInfo> *infos = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < infos->size(); ++i)
    {
        BufferInfo *info = &infos->editItemAt(i);

        if (info->mStatus == OWNED_BY_US)
        {
            bufferCount++;
        }
    }

    return bufferCount;
}

status_t OMXCodec::apeSeekFunc(MediaBuffer *srcBuffer)
{
        status_t err  = OK;
        int64_t lastBufferTimeUs;
        int32_t newframe, firstbyte;
        if (!(srcBuffer->meta_data()->findInt64(kKeyTime, &lastBufferTimeUs)))
        {
            ///LOGD("OMXCodec::drainInputBuffer--not find BufferTimeUs for APE");
            lastBufferTimeUs = 0;
            srcBuffer->meta_data()->setInt64(kKeyTime, lastBufferTimeUs);
        }
        else if ((srcBuffer->meta_data()->findInt32(kKeyNemFrame, &newframe))
                         && (srcBuffer->meta_data()->findInt32(kKeySeekByte, &firstbyte)))
            {
                ///LOGD("OMXCodec::drainInputBuffer--not find seek for APE");
                OMX_AUDIO_PARAM_APETYPE profile;
                InitOMXParams(&profile);
                profile.nPortIndex = kPortIndexInput;

                status_t err = mOMX->getParameter(
                                       mNode, OMX_IndexParamAudioApe, &profile, sizeof(profile));
                CHECK_EQ(err, (status_t)OK);

                profile.seekbyte = firstbyte;
                profile.seekfrm = newframe;

                err = mOMX->setParameter(
                              mNode, OMX_IndexParamAudioApe, &profile, sizeof(profile));
                ///LOGD("err= %d",err);
            }
        return err;

}

#endif //MTK_AOSP_ENHANCEMENT


}  // namespace android
