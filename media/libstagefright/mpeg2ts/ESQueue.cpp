/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2010 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "ESQueue"
#include <media/stagefright/foundation/ADebug.h>

#include "ESQueue.h"

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include "include/avc_utils.h"

#include <inttypes.h>
#include <netinet/in.h>
#if 0

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_STAGEFRIGHT_USE_XLOG
#include <cutils/log.h>
#endif
#endif

#endif

#ifdef MTK_AOSP_ENHANCEMENT
#include "include/hevc_utils.h"
#endif


namespace android {

ElementaryStreamQueue::ElementaryStreamQueue(Mode mode, uint32_t flags)
    : mMode(mode),
      mFlags(flags),
      mEOSReached(false) {
#ifdef MTK_AOSP_ENHANCEMENT
      mSeeking = false;
      mMP3Header = 0;
      mAudioFrameDuration = 20000;
      mVorbisStatus = 0;
      mH264UsePPs = false;
#ifdef MTK_OGM_PLAYBACK_SUPPORT
      mfgSearchStartCodeOptimize = 0;
#endif
      mfgFirstFrmAfterSeek = false;
      mLastTimeUs = -1;
#endif
}

sp<MetaData> ElementaryStreamQueue::getFormat() {
    return mFormat;
}

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_SUBTITLE_SUPPORT
void ElementaryStreamQueue::setFormat(uint32_t key, const char *value) {
    if (mFormat == NULL) {
        mFormat = new MetaData;
    }

    mFormat->setCString(key, value);
}
#endif
#endif

void ElementaryStreamQueue::clear(bool clearFormat) {
    if (mBuffer != NULL) {
        mBuffer->setRange(0, 0);
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
        if (mfgSearchStartCodeOptimize) {
            mBuffer->meta()->setInt32("LPos", 0);
        }
#endif
        #ifdef MTK_AOSP_ENHANCEMENT
        mBuffer.clear();
        mBuffer = NULL;
        #endif
    }
#ifdef MTK_AOSP_ENHANCEMENT
    mLastTimeUs = -1;
#endif
    mRangeInfos.clear();
#ifdef MTK_AOSP_ENHANCEMENT
    if (mMode == H264) {
        accessUnits.clear();
    }

    mfgFirstFrmAfterSeek = false;
#endif
    if (clearFormat) {
        mFormat.clear();
    }

    mEOSReached = false;
}

// Parse AC3 header assuming the current ptr is start position of syncframe,
// update metadata only applicable, and return the payload size
static unsigned parseAC3SyncFrame(
        const uint8_t *ptr, size_t size, sp<MetaData> *metaData) {
    static const unsigned channelCountTable[] = {2, 1, 2, 3, 3, 4, 4, 5};
    static const unsigned samplingRateTable[] = {48000, 44100, 32000};

    static const unsigned frameSizeTable[19][3] = {
        { 64, 69, 96 },
        { 80, 87, 120 },
        { 96, 104, 144 },
        { 112, 121, 168 },
        { 128, 139, 192 },
        { 160, 174, 240 },
        { 192, 208, 288 },
        { 224, 243, 336 },
        { 256, 278, 384 },
        { 320, 348, 480 },
        { 384, 417, 576 },
        { 448, 487, 672 },
        { 512, 557, 768 },
        { 640, 696, 960 },
        { 768, 835, 1152 },
        { 896, 975, 1344 },
        { 1024, 1114, 1536 },
        { 1152, 1253, 1728 },
        { 1280, 1393, 1920 },
    };

    ABitReader bits(ptr, size);
    if (bits.numBitsLeft() < 16) {
        return 0;
    }
    if (bits.getBits(16) != 0x0B77) {
        return 0;
    }

    if (bits.numBitsLeft() < 16 + 2 + 6 + 5 + 3 + 3) {
        ALOGV("Not enough bits left for further parsing");
        return 0;
    }
    bits.skipBits(16);  // crc1

    unsigned fscod = bits.getBits(2);
    if (fscod == 3) {
        ALOGW("Incorrect fscod in AC3 header");
        return 0;
    }

    unsigned frmsizecod = bits.getBits(6);
    if (frmsizecod > 37) {
        ALOGW("Incorrect frmsizecod in AC3 header");
        return 0;
    }

    unsigned bsid = bits.getBits(5);
    if (bsid > 8) {
        ALOGW("Incorrect bsid in AC3 header. Possibly E-AC-3?");
        return 0;
    }

    unsigned bsmod __unused = bits.getBits(3);
    unsigned acmod = bits.getBits(3);
    unsigned cmixlev __unused = 0;
    unsigned surmixlev __unused = 0;
    unsigned dsurmod __unused = 0;

    if ((acmod & 1) > 0 && acmod != 1) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        cmixlev = bits.getBits(2);
    }
    if ((acmod & 4) > 0) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        surmixlev = bits.getBits(2);
    }
    if (acmod == 2) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        dsurmod = bits.getBits(2);
    }

    if (bits.numBitsLeft() < 1) {
        return 0;
    }
    unsigned lfeon = bits.getBits(1);

    unsigned samplingRate = samplingRateTable[fscod];
    unsigned payloadSize = frameSizeTable[frmsizecod >> 1][fscod];
    if (fscod == 1) {
        payloadSize += frmsizecod & 1;
    }
    payloadSize <<= 1;  // convert from 16-bit words to bytes

    unsigned channelCount = channelCountTable[acmod] + lfeon;

    if (metaData != NULL) {
        (*metaData)->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
        (*metaData)->setInt32(kKeyChannelCount, channelCount);
        (*metaData)->setInt32(kKeySampleRate, samplingRate);
    }

    return payloadSize;
}

static bool IsSeeminglyValidAC3Header(const uint8_t *ptr, size_t size) {
    return parseAC3SyncFrame(ptr, size, NULL) > 0;
}

static bool IsSeeminglyValidADTSHeader(
        const uint8_t *ptr, size_t size, size_t *frameLength) {
    if (size < 7) {
        // Not enough data to verify header.
        return false;
    }

    if (ptr[0] != 0xff || (ptr[1] >> 4) != 0x0f) {
        return false;
    }

    unsigned layer = (ptr[1] >> 1) & 3;

    if (layer != 0) {
        return false;
    }

    unsigned ID = (ptr[1] >> 3) & 1;
    unsigned profile_ObjectType = ptr[2] >> 6;

    if (ID == 1 && profile_ObjectType == 3) {
        // MPEG-2 profile 3 is reserved.
        return false;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    //in case of find the wrong error
    uint8_t number_of_raw_data_blocks_in_frame;
    number_of_raw_data_blocks_in_frame = (ptr[6]) & 0x03;
    if (number_of_raw_data_blocks_in_frame != 0) {
        ALOGE
            ("Error: fake header here number_of_raw_data_blocks_in_frame=%d",
             number_of_raw_data_blocks_in_frame);
             return false;
    }
#endif
    size_t frameLengthInHeader =
            ((ptr[3] & 3) << 11) + (ptr[4] << 3) + ((ptr[5] >> 5) & 7);
    if (frameLengthInHeader > size) {
        return false;
    }
    *frameLength = frameLengthInHeader;
    return true;
}

static bool IsSeeminglyValidMPEGAudioHeader(const uint8_t *ptr, size_t size) {
    if (size < 3) {
        // Not enough data to verify header.
        return false;
    }

    if (ptr[0] != 0xff || (ptr[1] >> 5) != 0x07) {
        return false;
    }

    unsigned ID = (ptr[1] >> 3) & 3;

    if (ID == 1) {
        return false;  // reserved
    }

    unsigned layer = (ptr[1] >> 1) & 3;

    if (layer == 0) {
        return false;  // reserved
    }

    unsigned bitrateIndex = (ptr[2] >> 4);

    if (bitrateIndex == 0x0f) {
        return false;  // reserved
    }

    unsigned samplingRateIndex = (ptr[2] >> 2) & 3;

    if (samplingRateIndex == 3) {
        return false;  // reserved
    }

    return true;
}

#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
static bool IsSeeminglyValidDDPAudioHeader(const uint8_t *ptr, size_t size) {
    if (size < 2) return false;
    if (ptr[0] == 0x0b && ptr[1] == 0x77) return true;
    if (ptr[0] == 0x77 && ptr[1] == 0x0b) return true;
    return false;
}
#endif // DOLBY_END
status_t ElementaryStreamQueue::appendData(
        const void *data, size_t size, int64_t timeUs) {

    if (mEOSReached) {
        ALOGE("appending data after EOS");
        return ERROR_MALFORMED;
    }

    if (mBuffer == NULL || mBuffer->size() == 0) {
        switch (mMode) {
            case H264:
#ifdef MTK_AOSP_ENHANCEMENT
            case HEVC:
#endif

#ifndef MTK_AOSP_ENHANCEMENT
            case MPEG_VIDEO:
#endif
            {
#if 0
                if (size < 4 || memcmp("\x00\x00\x00\x01", data, 4)) {
                    return ERROR_MALFORMED;
                }
#else
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i + 2 < size; ++i) {
                    if (!memcmp("\x00\x00\x01", &ptr[i], 3)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
#ifdef MTK_AOSP_ENHANCEMENT
                    ALOGE("appendData::H264 this is not a valid ES Frame");
                    return ERROR_INVALID_ES_FRAME;//add by zyao
#else
                    return ERROR_MALFORMED;
#endif
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an H.264/MPEG syncword "
                          "at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }
#ifdef MTK_AOSP_ENHANCEMENT
            case MPEG_VIDEO:
#endif

            case MPEG4_VIDEO:
            {
#if 0
                if (size < 3 || memcmp("\x00\x00\x01", data, 3)) {
                    return ERROR_MALFORMED;
                }
#else
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i + 2 < size; ++i) {
                    if (!memcmp("\x00\x00\x01", &ptr[i], 3)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an H.264/MPEG syncword "
                          "at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }

#ifdef MTK_AOSP_ENHANCEMENT
        case VC1_VIDEO:
        {
            uint8_t *ptr = (uint8_t *) data;

            ssize_t startOffset = -1;
            for (size_t i = 0; i + 2 < size; ++i) {
                if (!memcmp("\x00\x00\x01", &ptr[i], 3)) {
                    startOffset = i;
                    break;
                }
            }

            if (startOffset < 0) {
                return ERROR_MALFORMED;
            }

                if (startOffset > 0) {
                    ALOGD("found something resembling an AVS/VC1 VIDEO syncword at "
                         "offset %zd",
                         startOffset);
                }

            data = &ptr[startOffset];
            size -= startOffset;
            break;
        }
#endif
            case AAC:
            {
                uint8_t *ptr = (uint8_t *)data;

#if 0
                if (size < 2 || ptr[0] != 0xff || (ptr[1] >> 4) != 0x0f) {
                    return ERROR_MALFORMED;
                }
#else
                ssize_t startOffset = -1;
                size_t frameLength;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidADTSHeader(
                            &ptr[i], size - i, &frameLength)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an AAC syncword at "
                          "offset %zd",
                          startOffset);
                }

                if (frameLength != size - startOffset) {
                    ALOGV("First ADTS AAC frame length is %zd bytes, "
                          "while the buffer size is %zd bytes.",
                          frameLength, size - startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }

            case AC3:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidAC3Header(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an AC3 syncword at "
                          "offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }

            case MPEG_AUDIO:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidMPEGAudioHeader(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
#ifdef MTK_AOSP_ENHANCEMENT
                    //ALPS001585646, MpegAudioHeader is lost, but we can ignore this error
                    ALOGW("cannot find MPEGAudio Header, ignore it");
                    startOffset = 0;
#else
                    return ERROR_MALFORMED;
#endif
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an MPEG audio "
                          "syncword at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }
#ifdef MTK_AOSP_ENHANCEMENT
        case PSLPCM:
        case LPCM:
        case BDLPCM:

            break;

        case VORBIS_AUDIO:
        {
            uint8_t *ptr = (uint8_t *) data;
            if (!memcmp("\x76\x6f\x72\x62\x69\x73", &ptr[1], 6)) {
                if (mVorbisStatus > 6) {
                    ALOGI("SKIP VORBIS header, type %d", ptr[0]);
                    data = &ptr[size];
                    size -= size;
                    break;
                }
                ALOGI("found VORBIS header, type %d", ptr[0]);
            }
            break;
        }
#ifdef MTK_SUBTITLE_SUPPORT
        case SUBTITLE:
        {
            uint8_t *ptr = (uint8_t *) data;
            ALOGI("subtitle header, %02x %02x %02x %02x %02x %02x",
                  ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5]);
            /* check data_identifier and subtitle stream id */
            if ((0x20 == ptr[0]) && 0 == ptr[1]) {
                /*skip dvb subtitle header */
                //data += 2;
                data = &ptr[2];
                size -= 2;
            } else {
                return ERROR_MALFORMED;
            }
            break;
        }
#endif
#endif
            case PCM_AUDIO:
            case METADATA:
            {
                break;
            }

#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
            case EC3:
            {
                uint8_t *ptr = (uint8_t *)data;
                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidDDPAudioHeader(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }
                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }
                if (startOffset > 0) {
                    ALOGI("found something resembling a DDP audio "
                         "syncword at offset %zd",
                         startOffset);
                }
                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }
#endif // DOLBY_END
            default:
                ALOGE("Unknown mode: %d", mMode);
                return ERROR_MALFORMED;
        }
    }

    size_t neededSize = (mBuffer == NULL ? 0 : mBuffer->size()) + size;
    if (mBuffer == NULL || neededSize > mBuffer->capacity()) {
        neededSize = (neededSize + 65535) & ~65535;

        ALOGV("resizing buffer to size %zu", neededSize);

        sp<ABuffer> buffer = new ABuffer(neededSize);
        if (mBuffer != NULL) {
            memcpy(buffer->data(), mBuffer->data(), mBuffer->size());
            buffer->setRange(0, mBuffer->size());
        } else {
            buffer->setRange(0, 0);
        }

        mBuffer = buffer;
    }
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
    if (mfgSearchStartCodeOptimize && ((mBuffer->size() == 0))) {
        mBuffer->meta()->setInt32("LPos", 0);
    }
#endif

    memcpy(mBuffer->data() + mBuffer->size(), data, size);
    mBuffer->setRange(0, mBuffer->size() + size);

    RangeInfo info;
    info.mLength = size;
    info.mTimestampUs = timeUs;
#ifdef MTK_AOSP_ENHANCEMENT
    //info.mInvalidTimestamp = fgInvalidPTS;
    info.mInvalidTimestamp = false;
#endif
    mRangeInfos.push_back(info);

#if 0
    if (mMode == AAC) {
        ALOGI("size = %zu, timeUs = %.2f secs", size, timeUs / 1E6);
        hexdump(data, size);
    }
#endif

    return OK;
}

#ifdef MTK_AOSP_ENHANCEMENT
sp <ABuffer> ElementaryStreamQueue::dequeueAccessUnit() {
     return dequeueAccessUnit1();//cherry
}
#else
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnit() {
    if ((mFlags & kFlag_AlignedData) && mMode == H264) {
        if (mRangeInfos.empty()) {
            return NULL;
        }

        RangeInfo info = *mRangeInfos.begin();
        mRangeInfos.erase(mRangeInfos.begin());

        sp<ABuffer> accessUnit = new ABuffer(info.mLength);
        memcpy(accessUnit->data(), mBuffer->data(), info.mLength);
        accessUnit->meta()->setInt64("timeUs", info.mTimestampUs);

        memmove(mBuffer->data(),
                mBuffer->data() + info.mLength,
                mBuffer->size() - info.mLength);

        mBuffer->setRange(0, mBuffer->size() - info.mLength);

        if (mFormat == NULL) {
            mFormat = MakeAVCCodecSpecificData(accessUnit);
        }

        return accessUnit;
    }

    switch (mMode) {
        case H264:
            return dequeueAccessUnitH264();
        case AAC:
            return dequeueAccessUnitAAC();
        case AC3:
            return dequeueAccessUnitAC3();
        case MPEG_VIDEO:
            return dequeueAccessUnitMPEGVideo();
        case MPEG4_VIDEO:
            return dequeueAccessUnitMPEG4Video();
        case PCM_AUDIO:
            return dequeueAccessUnitPCMAudio();
        case METADATA:
            return dequeueAccessUnitMetadata();
        default:
            if (mMode != MPEG_AUDIO) {
                ALOGE("Unknown mode");
                return NULL;
            }
            return dequeueAccessUnitMPEGAudio();
    }
}
#endif

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitAC3() {
    unsigned syncStartPos = 0;  // in bytes
    unsigned payloadSize = 0;
    sp<MetaData> format = new MetaData;
    while (true) {
        if (syncStartPos + 2 >= mBuffer->size()) {
            return NULL;
        }

        payloadSize = parseAC3SyncFrame(
                mBuffer->data() + syncStartPos,
                mBuffer->size() - syncStartPos,
                &format);
        if (payloadSize > 0) {
            break;
        }
        ++syncStartPos;
    }

    if (mBuffer->size() < syncStartPos + payloadSize) {
        ALOGV("Not enough buffer size for AC3");
        return NULL;
    }

    if (mFormat == NULL) {
        mFormat = format;
    }

    sp<ABuffer> accessUnit = new ABuffer(syncStartPos + payloadSize);
    memcpy(accessUnit->data(), mBuffer->data(), syncStartPos + payloadSize);

    int64_t timeUs = fetchTimestamp(syncStartPos + payloadSize);
    if (timeUs < 0ll) {
        ALOGE("negative timeUs");
        return NULL;
    }
    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    memmove(
            mBuffer->data(),
            mBuffer->data() + syncStartPos + payloadSize,
            mBuffer->size() - syncStartPos - payloadSize);

    mBuffer->setRange(0, mBuffer->size() - syncStartPos - payloadSize);

    return accessUnit;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitPCMAudio() {
    if (mBuffer->size() < 4) {
        return NULL;
    }

    ABitReader bits(mBuffer->data(), 4);
    if (bits.getBits(8) != 0xa0) {
        ALOGE("Unexpected bit values");
        return NULL;
    }
    unsigned numAUs = bits.getBits(8);
    bits.skipBits(8);
    unsigned quantization_word_length __unused = bits.getBits(2);
    unsigned audio_sampling_frequency = bits.getBits(3);
    unsigned num_channels = bits.getBits(3);

    if (audio_sampling_frequency != 2) {
        ALOGE("Wrong sampling freq");
        return NULL;
    }
    if (num_channels != 1u) {
        ALOGE("Wrong channel #");
        return NULL;
    }

    if (mFormat == NULL) {
        mFormat = new MetaData;
        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
        mFormat->setInt32(kKeyChannelCount, 2);
        mFormat->setInt32(kKeySampleRate, 48000);
    }
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_RAW_SUPPORT
    mFormat->setInt32(kKeyEndian, 2);   //1: big endian, 2: little endia
    mFormat->setInt32(kKeyBitWidth, 16);
    mFormat->setInt32(kKeyPCMType, 1);  //1: WAV file, 2: BD file, 3: DVD_VOB file, 4: DVD_AOB file
    //mFormat->setInt32(kKeyChannelAssignment, 1);  // 2 channels
#endif //MTK_AUDIO_RAW_SUPPORT
#endif //ANDROID_DEFAULT_CODE

    static const size_t kFramesPerAU = 80;
    size_t frameSize = 2 /* numChannels */ * sizeof(int16_t);

    size_t payloadSize = numAUs * frameSize * kFramesPerAU;

    if (mBuffer->size() < 4 + payloadSize) {
        return NULL;
    }

    sp<ABuffer> accessUnit = new ABuffer(payloadSize);
    memcpy(accessUnit->data(), mBuffer->data() + 4, payloadSize);

    int64_t timeUs = fetchTimestamp(payloadSize + 4);
    if (timeUs < 0ll) {
        ALOGE("Negative timeUs");
        return NULL;
    }
    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    int16_t *ptr = (int16_t *)accessUnit->data();
    for (size_t i = 0; i < payloadSize / sizeof(int16_t); ++i) {
        ptr[i] = ntohs(ptr[i]);
    }

    memmove(
            mBuffer->data(),
            mBuffer->data() + 4 + payloadSize,
            mBuffer->size() - 4 - payloadSize);

    mBuffer->setRange(0, mBuffer->size() - 4 - payloadSize);

    return accessUnit;
}

#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
//Search start code optimiztion
//fgEnable == true: memorize last scanned buffer potition, start to search at last stop position
//fgEnable == false: always start to search at the beginning of mbuffer queue
void ElementaryStreamQueue::setSearchSCOptimize(bool fgEnable) {
    mfgSearchStartCodeOptimize = fgEnable;
}
#endif

#ifdef MTK_AOSP_ENHANCEMENT
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitAAC() {
    Vector<size_t> ranges;
    Vector<size_t> frameOffsets;
    Vector<size_t> frameSizes;
    size_t auSize = 0;

    size_t offset = 0;
#ifdef MTK_AOSP_ENHANCEMENT
    bool fristGetFormat = false;
    bool fristGetFormatError = false;
#endif

    while (offset + 7 <= mBuffer->size()) {
        ABitReader bits(mBuffer->data() + offset, mBuffer->size() - offset);

        // adts_fixed_header = 7bytes

#ifdef MTK_AOSP_ENHANCEMENT
        uint8_t *data = mBuffer->data() + offset;
        size_t size = mBuffer->size() - offset;
        size_t invalidLength = 0;
        bool hasInvalidADTSHeader = false;
        uint32_t startCode = bits.getBits(12);
        //adts sync code: 0xFFF

        //[qian] find the sync code to make sure it the start of a aac frame
        while (!((Compare_EQ(startCode, 0xfffu)).empty())) {
            ABitReader tempBits(data, size);
            hasInvalidADTSHeader = true;
            uint8_t *ptr = data;
            ssize_t startOffset = -1;
            size_t frameLength;
            for (size_t i = 1; i < size; ++i) {
                if (IsSeeminglyValidADTSHeader(&ptr[i], size - i,&frameLength)) {
                    startOffset = i;
                    break;
                }
            }
            if (startOffset < 0) {
                ALOGE("error here , no header???, lefte byte=%ld",
                      (long)(mBuffer->size() - offset));
                return NULL;
            }

            data = &ptr[startOffset];
            size -= startOffset;
            offset += startOffset;
            invalidLength += startOffset;

            bits.skipBits(startOffset * 8);
            tempBits.skipBits(startOffset * 8);
            startCode = tempBits.getBits(12);
        }
#else
        CHECK_EQ(bits.getBits(12), 0xfffu);
#endif

/*

		adts_fixed_header()//7bytes
		{
			syncword; 12 bslbf
			ID; 1 bslbf
			layer; 2 uimsbf
			protection_absent; 1 bslbf
			profile; 2 uimsbf
			sampling_frequency_index; 4 uimsbf
			private_bit; 1 bslbf
			channel_configuration; 3 uimsbf
			original_copy; 1 bslbf
			home; 1 bslbf
		}

*/
        bits.skipBits(3);       // ID, layer
        bool protection_absent = bits.getBits(1) != 0;

        if (mFormat == NULL) {

            unsigned profile = bits.getBits(2);
#ifdef MTK_AOSP_ENHANCEMENT
            if (profile == 3) {
                ALOGE("error in check aac profile");
                fristGetFormatError = true;
            }
#else
            CHECK_NE(profile, 3u);
#endif
            unsigned sampling_freq_index = bits.getBits(4);
            bits.getBits(1);    // private_bit
            unsigned channel_configuration = bits.getBits(3);

#ifdef MTK_AOSP_ENHANCEMENT
            if (channel_configuration == 0) {

                fristGetFormatError = true;
                ALOGE("error in check aac channel_configuration ");
            }
#else
            CHECK_NE(channel_configuration, 0u);
#endif
            bits.skipBits(2);   // original_copy, home

            mFormat =
                MakeAACCodecSpecificData(profile, sampling_freq_index,
                                         channel_configuration);

            int32_t sampleRate;
            int32_t numChannels;
            if (!mFormat->findInt32(kKeySampleRate, &sampleRate)) {
                ALOGE("SampleRate not found");
                return NULL;
            }
            if (!mFormat->findInt32(kKeyChannelCount, &numChannels)) {
                ALOGE("ChannelCount not found");
                return NULL;
            }

#ifdef MTK_AOSP_ENHANCEMENT
            fristGetFormat = true;
            if (sampleRate > 0)
                mAudioFrameDuration = 1024 * 1000000ll / sampleRate;    //us
            ALOGE("AACmAudioFrameDuration %lld sampleRate=%d",
                  (long long)mAudioFrameDuration, sampleRate);

#endif

            ALOGE("found AAC codec config (%d Hz, %d channels)",
                  sampleRate, numChannels);
        } else {
            // profile_ObjectType, sampling_frequency_index, private_bits,
            // channel_configuration, original_copy, home
            bits.skipBits(12);
        }

        // adts_variable_header

/*
		adts_variable_header()//7bytes
		{
			copyright_identification_bit; 1 bslbf
			copyright_identification_start; 1 bslbf
			aac_frame_length; 13 bslbf//Length of the frame including headers and error_check in bytes
			adts_buffer_fullness; 11 bslbf
			number_of_raw_data_blocks_in_frame; 2 uimsfb
		}

*/

        // copyright_identification_bit, copyright_identification_start
        bits.skipBits(2);

        unsigned aac_frame_length = bits.getBits(13);

        bits.skipBits(11);      // adts_buffer_fullness

        unsigned number_of_raw_data_blocks_in_frame = bits.getBits(2);
        //[qian]Number of raw_data_block()¡¯s that are multiplexed in the
        //adts_frame() is equal to number_of_raw_data_blocks_in_frame
        //+ 1. The minimum value is 0 indicating 1 raw_data_block()
        if (number_of_raw_data_blocks_in_frame != 0) {
            // To be implemented.
           ALOGE("[TS_ERROR]only support number_of_raw_data_blocks_in_frame=0, realy=%d\n",number_of_raw_data_blocks_in_frame);
            TRESPASS();
        }

        if (offset + aac_frame_length > mBuffer->size()) {
            ALOGD("break aac_frame_length=%u,mBuffer->size()=%zu",
                  aac_frame_length, mBuffer->size());
            //[qian] notice this
            break;
        }

        size_t headerSize __unused = protection_absent ? 7 : 9;

/*
		adts_error_check()
		{
			if (protection_absent == ¡®0¡¯)
				crc_check; 16 rpchof
		}
*/

#ifdef MTK_AOSP_ENHANCEMENT

        if (fristGetFormatError) {
            ALOGE("Error skip this AAC frame");
            fetchTimestamp(aac_frame_length + invalidLength);
            offset += aac_frame_length;
            if (mFormat != NULL)
                mFormat = NULL;
            fristGetFormat = false;
            fristGetFormatError = false;
        } else                  //error skip this frame
        {
            if (hasInvalidADTSHeader) {
                ranges.push(aac_frame_length + invalidLength);
                hasInvalidADTSHeader = false;
            } else {
                ranges.push(aac_frame_length);
            }
            frameOffsets.push(offset + headerSize);
            frameSizes.push(aac_frame_length - headerSize);
            auSize += aac_frame_length - headerSize;
            offset += aac_frame_length;
        }
#else
        ranges.push(aac_frame_length);

        frameOffsets.push(offset + headerSize);
        frameSizes.push(aac_frame_length - headerSize);
        auSize += aac_frame_length - headerSize;

        offset += aac_frame_length;
#endif
    }

    if (offset == 0) {
        return NULL;
    }
    //[qian] audio a ts packet should be a frame?
    int64_t timeUs = -1;

    for (size_t i = 0; i < ranges.size(); ++i) {
        int64_t tmpUs = fetchTimestamp(ranges.itemAt(i));

        if (i == 0) {
            timeUs = tmpUs;
        }
    }

    sp<ABuffer> accessUnit = new ABuffer(auSize);
    size_t dstOffset = 0;
    for (size_t i = 0; i < frameOffsets.size(); ++i) {
        size_t frameOffset = frameOffsets.itemAt(i);

        memcpy(accessUnit->data() + dstOffset,
               mBuffer->data() + frameOffset, frameSizes.itemAt(i));

        dstOffset += frameSizes.itemAt(i);
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (fristGetFormat) {       //[qian]?
        mFormat->setInt32(kKeyMaxInputSize, offset * 1.5);
        ALOGE("AAC kKeyMaxInputSize=%zu ", offset);
    }
#endif
    memmove(mBuffer->data(), mBuffer->data() + offset,
            mBuffer->size() - offset);
    mBuffer->setRange(0, mBuffer->size() - offset);

    if (timeUs >= 0) {
        accessUnit->meta()->setInt64("timeUs", timeUs);
    } else {
        ALOGW("no time for AAC access unit");
    }
#ifdef MTK_AOSP_ENHANCEMENT

    if (mBuffer->size() > 0 && frameOffsets.size() > 0
        && mRangeInfos.size() == 1) {
        RangeInfo *info = &*mRangeInfos.begin();
        ALOGD
            ("qian AAC correct the timestamp from %lld to %lld,mAudioFrameDuration=%lld",
             (long long)info->mTimestampUs,
             (long long)(info->mTimestampUs +
             mAudioFrameDuration * frameOffsets.size()), (long long)mAudioFrameDuration);
        info->mTimestampUs =
            info->mTimestampUs + (mAudioFrameDuration * frameOffsets.size());

        info = &*mRangeInfos.begin();
        ALOGE("qian AAC correct the timestamp is =%lld", (long long)info->mTimestampUs);
    }
#endif

    return accessUnit;
}
#else
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitAAC() {
    if (mBuffer->size() == 0) {
        return NULL;
    }

    if (mRangeInfos.empty()) {
        return NULL;
    }

    const RangeInfo &info = *mRangeInfos.begin();
    if (mBuffer->size() < info.mLength) {
        return NULL;
    }

    if (info.mTimestampUs < 0ll) {
        ALOGE("Negative info.mTimestampUs");
        return NULL;
    }

    // The idea here is consume all AAC frames starting at offsets before
    // info.mLength so we can assign a meaningful timestamp without
    // having to interpolate.
    // The final AAC frame may well extend into the next RangeInfo but
    // that's ok.
    size_t offset = 0;
    while (offset < info.mLength) {
        if (offset + 7 > mBuffer->size()) {
            return NULL;
        }

        ABitReader bits(mBuffer->data() + offset, mBuffer->size() - offset);

        // adts_fixed_header

        if (bits.getBits(12) != 0xfffu) {
            ALOGE("Wrong atds_fixed_header");
            return NULL;
        }
        bits.skipBits(3);  // ID, layer
        bool protection_absent __unused = bits.getBits(1) != 0;

        if (mFormat == NULL) {
            unsigned profile = bits.getBits(2);
            if (profile == 3u) {
                ALOGE("profile should not be 3");
                return NULL;
            }
            unsigned sampling_freq_index = bits.getBits(4);
            bits.getBits(1);  // private_bit
            unsigned channel_configuration = bits.getBits(3);
            if (channel_configuration == 0u) {
                ALOGE("channel_config should not be 0");
                return NULL;
            }
            bits.skipBits(2);  // original_copy, home

            mFormat = MakeAACCodecSpecificData(
                    profile, sampling_freq_index, channel_configuration);

            mFormat->setInt32(kKeyIsADTS, true);

            int32_t sampleRate;
            int32_t numChannels;
            if (!mFormat->findInt32(kKeySampleRate, &sampleRate)) {
                ALOGE("SampleRate not found");
                return NULL;
            }
            if (!mFormat->findInt32(kKeyChannelCount, &numChannels)) {
                ALOGE("ChannelCount not found");
                return NULL;
            }

            ALOGI("found AAC codec config (%d Hz, %d channels)",
                 sampleRate, numChannels);
        } else {
            // profile_ObjectType, sampling_frequency_index, private_bits,
            // channel_configuration, original_copy, home
            bits.skipBits(12);
        }

        // adts_variable_header

        // copyright_identification_bit, copyright_identification_start
        bits.skipBits(2);

        unsigned aac_frame_length = bits.getBits(13);

        bits.skipBits(11);  // adts_buffer_fullness

        unsigned number_of_raw_data_blocks_in_frame = bits.getBits(2);

        if (number_of_raw_data_blocks_in_frame != 0) {
            // To be implemented.
            ALOGE("Should not reach here.");
            return NULL;
        }

        if (offset + aac_frame_length > mBuffer->size()) {
            return NULL;
        }

        size_t headerSize __unused = protection_absent ? 7 : 9;

        offset += aac_frame_length;
    }

    int64_t timeUs = fetchTimestamp(offset);

    sp<ABuffer> accessUnit = new ABuffer(offset);
    memcpy(accessUnit->data(), mBuffer->data(), offset);

    memmove(mBuffer->data(), mBuffer->data() + offset,
            mBuffer->size() - offset);
    mBuffer->setRange(0, mBuffer->size() - offset);

    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    return accessUnit;
}
#endif
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
static int
calc_dd_frame_size(int code)
{
    static const int FrameSize32K[] = { 96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336, 336, 384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960, 1152, 1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920 };
    static const int FrameSize44K[] = { 69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243, 244, 278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697, 835, 836, 975, 976, 114, 1115, 1253, 1254, 1393, 1394 };
    static const int FrameSize48K[] = { 64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224, 256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768, 768, 896, 896, 1024, 1024, 1152, 1152, 1280, 1280 };
    int fscod = (code >> 6) & 0x3;
    int frmsizcod = code & 0x3f;
    if (fscod == 0) return 2 * FrameSize48K[frmsizcod];
    if (fscod == 1) return 2 * FrameSize44K[frmsizcod];
    if (fscod == 2) return 2 * FrameSize32K[frmsizcod];
    return 0;
}
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitDDP() {
    unsigned int size;
    unsigned char* ptr;
    int bsid;
    size_t frame_size = 0;
    size_t auSize = 0;
    size = mBuffer->size();
    ptr = mBuffer->data();
    if(size <= 6)
    {
        return NULL;
    }
    if(mFormat == NULL)
    {
        sp<MetaData> meta = new MetaData;
        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_EAC3);
        int32_t sampleRate = 0;
        int32_t numChannels = 0;
        meta->setInt32(kKeySampleRate, sampleRate);
        meta->setInt32(kKeyChannelCount, numChannels);
        mFormat = meta;
    }
    bsid = (ptr[5] >> 3) & 0x1f;
    if (bsid > 10 && bsid <= 16)
    {
        frame_size = 2 * ((((ptr[2] << 8) | ptr[3]) & 0x7ff) + 1);
    }
    else
    {
        frame_size = calc_dd_frame_size(ptr[4]);
    }
    if (size < frame_size) {
        ALOGW("Buffer size insufficient for frame size");
        return NULL;
    }
    auSize += frame_size;
    int64_t timeUs = -1;
    if(!mRangeInfos.empty())
        timeUs = fetchTimestamp(frame_size);
    else
        ALOGW("Timestamp not created because mRangeInfos was empty");
    sp<ABuffer> accessUnit = new ABuffer(auSize);
    memcpy(accessUnit->data(), mBuffer->data(), frame_size);
    memmove(mBuffer->data(), mBuffer->data() + frame_size, mBuffer->size() - frame_size);
    mBuffer->setRange(0, mBuffer->size() - frame_size);
    accessUnit->meta()->setInt64("timeUs", timeUs);
    if (timeUs >= 0) {
        accessUnit->meta()->setInt64("timeUs", timeUs);
    } else {
        ALOGW("no time for DDP access unit");
    }
    return accessUnit;
}
#endif // DOLBY_END
int64_t ElementaryStreamQueue::fetchTimestamp(size_t size
#ifdef MTK_AOSP_ENHANCEMENT
, bool* pfgInvalidPTS
#endif
) {
    int64_t timeUs = -1;
    bool first = true;

    while (size > 0) {
        if (mRangeInfos.empty()) {
            return timeUs;
        }

        RangeInfo *info = &*mRangeInfos.begin();
#ifdef MTK_AOSP_ENHANCEMENT
        //Add for Special MPEG File
        if ((first == false) && (timeUs == 0xFFFFFFFF)
            && (info->mTimestampUs != 0x0)) {
                ALOGV("fetchTimestamp - Change: %lld  %lld \n", (long long)timeUs,
                    (long long)info->mTimestampUs);
            timeUs = info->mTimestampUs;
        }
#endif //#ifndef ANDROID_DEFAULT_CODE

        if (first) {
            timeUs = info->mTimestampUs;
#ifdef MTK_AOSP_ENHANCEMENT
        if(pfgInvalidPTS)
        {
            *pfgInvalidPTS = info->mInvalidTimestamp;
            if(info->mInvalidTimestamp || info->mLength > size)
            {
                ALOGV("set mInvalidTimestamp=true\n");
            }

            info->mInvalidTimestamp = true;
        }
#endif

            first = false;
        }

        if (info->mLength > size) {
            info->mLength -= size;
            size = 0;
        } else {
            size -= info->mLength;

            mRangeInfos.erase(mRangeInfos.begin());
            info = NULL;
        }

    }

    if (timeUs == 0ll) {
        ALOGV("Returning 0 timestamp");
    }

    return timeUs;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitH264() {
    const uint8_t *data = mBuffer->data();

    size_t size = mBuffer->size();
    Vector<NALPosition> nals;

    size_t totalSize = 0;
    size_t seiCount = 0;

    status_t err;
    const uint8_t *nalStart;
    size_t nalSize;
    bool foundSlice = false;
    bool foundIDR = false;
    while ((err = getNextNALUnit(&data, &size, &nalStart, &nalSize)) == OK) {
        if (nalSize == 0) continue;

        unsigned nalType = nalStart[0] & 0x1f;
        bool flush = false;

        if (nalType == 1 || nalType == 5) {
            if (nalType == 5) {
                foundIDR = true;
            }
            if (foundSlice) {
                ABitReader br(nalStart + 1, nalSize);
                unsigned first_mb_in_slice = parseUE(&br);

                if (first_mb_in_slice == 0) {
                    // This slice starts a new frame.

                    flush = true;
                }
            }

            foundSlice = true;
        } else if ((nalType == 9 || nalType == 7) && foundSlice) {
            // Access unit delimiter and SPS will be associated with the
            // next frame.

            flush = true;
        } else if (nalType == 6 && nalSize > 0) {
            // found non-zero sized SEI
            ++seiCount;
        }

        if (flush) {
            // The access unit will contain all nal units up to, but excluding
            // the current one, separated by 0x00 0x00 0x00 0x01 startcodes.

            size_t auSize = 4 * nals.size() + totalSize;
            sp<ABuffer> accessUnit = new ABuffer(auSize);
            sp<ABuffer> sei;

            if (seiCount > 0) {
                sei = new ABuffer(seiCount * sizeof(NALPosition));
                accessUnit->meta()->setBuffer("sei", sei);
            }

#if !LOG_NDEBUG
            AString out;
#endif

            size_t dstOffset = 0;
            size_t seiIndex = 0;
            for (size_t i = 0; i < nals.size(); ++i) {
                const NALPosition &pos = nals.itemAt(i);

                unsigned nalType = mBuffer->data()[pos.nalOffset] & 0x1f;

                if (nalType == 6 && pos.nalSize > 0) {
                    if (seiIndex >= sei->size() / sizeof(NALPosition)) {
                        ALOGE("Wrong seiIndex");
                        return NULL;
                    }
                    NALPosition &seiPos = ((NALPosition *)sei->data())[seiIndex++];
                    seiPos.nalOffset = dstOffset + 4;
                    seiPos.nalSize = pos.nalSize;
                }

#if !LOG_NDEBUG
                char tmp[128];
                sprintf(tmp, "0x%02x", nalType);
                if (i > 0) {
                    out.append(", ");
                }
                out.append(tmp);
#endif

                memcpy(accessUnit->data() + dstOffset, "\x00\x00\x00\x01", 4);

                memcpy(accessUnit->data() + dstOffset + 4,
                       mBuffer->data() + pos.nalOffset,
                       pos.nalSize);

                dstOffset += pos.nalSize + 4;
            }

#if !LOG_NDEBUG
            ALOGV("accessUnit contains nal types %s", out.c_str());
#endif

            const NALPosition &pos = nals.itemAt(nals.size() - 1);
            size_t nextScan = pos.nalOffset + pos.nalSize;

            memmove(mBuffer->data(),
                    mBuffer->data() + nextScan,
                    mBuffer->size() - nextScan);

            mBuffer->setRange(0, mBuffer->size() - nextScan);

            int64_t timeUs = fetchTimestamp(nextScan);
            if (timeUs < 0ll) {
                ALOGE("Negative timeUs");
                return NULL;
            }

            accessUnit->meta()->setInt64("timeUs", timeUs);
            if (foundIDR) {
                accessUnit->meta()->setInt32("isSync", 1);
            }

            if (mFormat == NULL) {
                mFormat = MakeAVCCodecSpecificData(accessUnit);
            }

            return accessUnit;
        }

        NALPosition pos;
        pos.nalOffset = nalStart - mBuffer->data();
        pos.nalSize = nalSize;

        nals.push(pos);

        totalSize += nalSize;
    }
    if (err != (status_t)-EAGAIN) {
        ALOGE("Unexpeted err");
        return NULL;
    }

    return NULL;
}

#ifdef MTK_AOSP_ENHANCEMENT
static const uint32_t kMP3HeaderMask = 0xfffe0c00;  //0xfffe0cc0 add by zhihui zhang no consider channel mode
const size_t kMaxBytesChecked = 128 * 1024;
static int mp3HeaderStartAt(const uint8_t * start, int length, int header) {
    uint32_t code = 0;
    int i = 0;

    for (i = 0; i < length; i++) {
        code = (code << 8) + start[i];
        if ((code & kMP3HeaderMask) == (header & kMP3HeaderMask)) {
            // some files has no seq start code
            return i - 3;
        }
    }

    return -1;
}
status_t findMP3Header(const uint8_t * buf, ssize_t size,
                       ssize_t * offset, int *pHeader) {
    uint32_t header1 = 0, header2 = 0;
    size_t frameSize = 0, frameSize2 = 0;
    bool retb = false;
    //header1 = U32_AT(buf+*offset);
    while (*offset + 4 < size) {
        //bool retb = GetMPEGAudioFrameSize(header1, &frameSize,NULL,NULL,NULL,NULL);
        //if(!retb)
        {
            //find 1st header and verify
            for (ssize_t i = *offset; i < size - 4; i++) {
                if (IsSeeminglyValidMPEGAudioHeader(&buf[i], size - i)) {
                    *offset = i;
                    header1 = U32_AT(buf + *offset);
                    retb =
                        GetMPEGAudioFrameSize(header1, &frameSize, NULL,
                                              NULL, NULL, NULL);
                    if (!retb || (frameSize == 0)) {
                        //ALOGI("1.%s err 0x%x, ofst/retb/fSz=%d/%d/%d\n", __FUNCTION__, header1, *offset, retb, frameSize);
                        continue;
                    } else {
                        //ALOGI("2.%s 0x%x, ofst/retb/fSz=%d/%d/%d\n", __FUNCTION__, header1, *offset, retb, frameSize);
                        break;
                    }
                }
            }
            if (!retb || (frameSize == 0)) {
                break;
            }
        }
        //find 2nd header and verify
        if ((long long)(*offset + frameSize) < (long long)size) {
            *offset += frameSize;
            header2 = U32_AT(buf + *offset);
            if ((header2 & kMP3HeaderMask) == (header1 & kMP3HeaderMask)) {
                *pHeader = header1;
                return OK;
            } else
                if (GetMPEGAudioFrameSize
                    (header2, &frameSize2, NULL, NULL, NULL, NULL)
                    && (frameSize2 > 0)) {
                header1 = header2;
                //ALOGI("3.%s 2nd 0x%x, ofst/fSz/Sz %d/%d/%d\n", __FUNCTION__, header2, *offset, frameSize2, size);
            } else              //header1's frameSize has problem, re-find header1
            {
                *offset -= (frameSize - 1);
                //ALOGI("4.%s 2nd err 0x%x, new ofst/fSz/sz %d/%d/%d\n", __FUNCTION__, header2, *offset, frameSize2, size);
            }
        } else {
            ALOGI("frame overflow buffer");
            break;
        }
    }
    ALOGI
        ("%s():size:%lld,Not found MP3Headr,buf:%2x %2x %2x %2x %2x %2x %2x %2x",
         __FUNCTION__, (long long)size, buf[0], buf[1], buf[2], buf[3], buf[4],
         buf[5], buf[6], buf[7]);
    return UNKNOWN_ERROR;
}
#endif
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEGAudio() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    if (size < 4) {
        return NULL;
    }

    uint32_t header = U32_AT(data);

    size_t frameSize;
    int samplingRate, numChannels, bitrate, numSamples;
#ifdef MTK_AOSP_ENHANCEMENT
    ssize_t offset = 0;
    // when mMP3Header is not set, set it
    if (mMP3Header == 0) {
        if (findMP3Header(data, size, &offset, &mMP3Header) != OK) {
            if (size > kMaxBytesChecked) {
                ALOGE("findMP3Header fail size>%zu, skip this buffer",
                      kMaxBytesChecked);
                mBuffer->setRange(0, 0);
                return NULL;
            } else {
                ALOGW("Not get mMP3Header, size:%zu", size);
                return NULL;
            }
        } else {
            ALOGI("mMP3Header:%x", mMP3Header);
        }
    }
    // mMP3Header is set, check header
    int start = mp3HeaderStartAt(data + offset, size - offset, mMP3Header);
    if (start >= 0 && start + offset + 3 < (int)size) {
        offset += start;
        header = U32_AT(data + offset);
        ALOGV("header:%x", header);
        bool retb = GetMPEGAudioFrameSize(header, &frameSize, &samplingRate,
                                          &numChannels, &bitrate,
                                          &numSamples);
        if (!retb) {
            ALOGE("GetMPEGAudioFrameSize fail, skip this buffer");
            mBuffer->setRange(0, 0);
            return NULL;
        }
        size -= offset;
    } else {
        ALOGE("not found mMP3Header,skip");
        mBuffer->setRange(0, 0);
        return NULL;
    }
#else
    if (!GetMPEGAudioFrameSize(
                header, &frameSize, &samplingRate, &numChannels,
                &bitrate, &numSamples)) {
        ALOGE("Failed to get audio frame size");
        return NULL;
    }
#endif

    if (size < frameSize) {
        return NULL;
    }

    unsigned layer = 4 - ((header >> 17) & 3);

    sp<ABuffer> accessUnit = new ABuffer(frameSize);
#ifdef MTK_AOSP_ENHANCEMENT
    memcpy(accessUnit->data(), data + offset, frameSize);
    memmove(mBuffer->data(), mBuffer->data() + offset + frameSize,
            mBuffer->size() - offset - frameSize);

    mBuffer->setRange(0, mBuffer->size() - offset - frameSize);

    int64_t timeUs = fetchTimestamp(offset + frameSize);
#else
    memcpy(accessUnit->data(), data, frameSize);

    memmove(mBuffer->data(),
            mBuffer->data() + frameSize,
            mBuffer->size() - frameSize);

    mBuffer->setRange(0, mBuffer->size() - frameSize);

    int64_t timeUs = fetchTimestamp(frameSize);
#endif
    if (timeUs < 0ll) {
        ALOGE("Negative timeUs");
        return NULL;
    }

    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    if (mFormat == NULL) {
        mFormat = new MetaData;

        switch (layer) {
            case 1:
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
                break;
            case 2:
#ifdef MTK_AUDIO_CHANGE_SUPPORT
                mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
#else
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
#endif
                break;
            case 3:
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
                break;
            default:
                return NULL;
        }

        mFormat->setInt32(kKeySampleRate, samplingRate);
        mFormat->setInt32(kKeyChannelCount, numChannels);

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_OGM_PLAYBACK_SUPPORT
        mFormat->setInt32(kKeyMPEGAudLayer, (int32_t) layer);
#endif
#endif
    }

    return accessUnit;
}

static void EncodeSize14(uint8_t **_ptr, size_t size) {
    if (size > 0x3fff) {
        ALOGE("Wrong size");
        return;
    }

    uint8_t *ptr = *_ptr;

    *ptr++ = 0x80 | (size >> 7);
    *ptr++ = size & 0x7f;

    *_ptr = ptr;
}

static sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd) {
    sp<ABuffer> esds = new ABuffer(csd->size() + 25);

    uint8_t *ptr = esds->data();
    *ptr++ = 0x03;
    EncodeSize14(&ptr, 22 + csd->size());

    *ptr++ = 0x00;  // ES_ID
    *ptr++ = 0x00;

    *ptr++ = 0x00;  // streamDependenceFlag, URL_Flag, OCRstreamFlag

    *ptr++ = 0x04;
    EncodeSize14(&ptr, 16 + csd->size());

    *ptr++ = 0x40;  // Audio ISO/IEC 14496-3

    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = 0x00;
    }

    *ptr++ = 0x05;
    EncodeSize14(&ptr, csd->size());

    memcpy(ptr, csd->data(), csd->size());

    return esds;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEGVideo() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    bool sawPictureStart = false;
    int pprevStartCode = -1;
    int prevStartCode = -1;
    int currentStartCode = -1;
    bool gopFound = false;
    bool isClosedGop = false;
    bool brokenLink = false;

    size_t offset = 0;
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_OGM_PLAYBACK_SUPPORT //for tablet only

    int prev_sawPictureStart = 0;

    //Search start code optimiztion
    //mfgSearchStartCodeOptimize == true: memorize last scanned buffer potition, start to search at last stop position
    //mfgSearchStartCodeOptimize == false: always start to search at the beginning of mbuffer queue
    int32_t prev_offset = 0;
    if (!mSeeking) {
        if (mBuffer->meta()->findInt32("LPos", &prev_offset)) {
            offset = (mfgSearchStartCodeOptimize && (mFormat != NULL)
                      && (prev_offset > 4)) ? (prev_offset - 4) : 0;
            if ((offset > 0)
                && mBuffer->meta()->findInt32("PicS", &prev_sawPictureStart)) {
                sawPictureStart = (mfgSearchStartCodeOptimize
                                   && (mFormat !=
                                       NULL)) ? (prev_sawPictureStart >
                                                 0) : false;
            }
        }
        ALOGV("offset %d/sawPictureStart %d\n", (int)offset, (int)sawPictureStart);
    }
#endif

    int lastGOPOff = -1;
#endif
    while (offset + 3 < size) {
        if (memcmp(&data[offset], "\x00\x00\x01", 3)) {
            ++offset;
            continue;
        }

        pprevStartCode = prevStartCode;
        prevStartCode = currentStartCode;
        currentStartCode = data[offset + 3];

        if (currentStartCode == 0xb3 && mFormat == NULL) {
            memmove(mBuffer->data(), mBuffer->data() + offset, size - offset);
            size -= offset;
            (void)fetchTimestamp(offset);
            offset = 0;
            mBuffer->setRange(0, size);
        }

        if ((prevStartCode == 0xb3 && currentStartCode != 0xb5)
                || (pprevStartCode == 0xb3 && prevStartCode == 0xb5)) {
            // seqHeader without/with extension

            if (mFormat == NULL) {
                if (size < 7u) {
                    ALOGE("Size too small");
                    return NULL;
                }

                unsigned width =
                    (data[4] << 4) | data[5] >> 4;

                unsigned height =
                    ((data[5] & 0x0f) << 8) | data[6];

                mFormat = new MetaData;
                mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
                mFormat->setInt32(kKeyWidth, width);
                mFormat->setInt32(kKeyHeight, height);

                ALOGI("found MPEG2 video codec config (%d x %d)", width, height);

                sp<ABuffer> csd = new ABuffer(offset);
                memcpy(csd->data(), data, offset);

                memmove(mBuffer->data(),
                        mBuffer->data() + offset,
                        mBuffer->size() - offset);

                mBuffer->setRange(0, mBuffer->size() - offset);
                size -= offset;
                (void)fetchTimestamp(offset);
                offset = 0;
                #if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
                if(mfgSearchStartCodeOptimize)
                {
                      mBuffer->meta()->setInt32("LPos", 0);
                      mBuffer->meta()->setInt32("PicS", 0);
                }
                #endif

                // hexdump(csd->data(), csd->size());

                sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                mFormat->setData(
                        kKeyESDS, kTypeESDS, esds->data(), esds->size());

                return NULL;
            }
        }
#ifdef MTK_AOSP_ENHANCEMENT
        // save the GOP, send to decode when seek. if Format is NULL, lastGOPOff make no sense
        if (mSeeking && currentStartCode == 0xB8 && mFormat != NULL) {
            lastGOPOff = offset;
        }
        if (mFormat != NULL && (currentStartCode == 0x00 || (sawPictureStart && currentStartCode == 0xB7))) { //ALPS00473447
            if (mSeeking) {
                if (((data[offset + 5] >> 3) & 0x7) == 1) { // I frame
                    mSeeking = false;

                    size_t tmpOff = offset;
                    if (lastGOPOff != -1) {
                        tmpOff = lastGOPOff;
                        ALOGI
                            ("Send GOP when seeking, offset:%zu lastGOPOff:%x",
                             offset, lastGOPOff);
                    }
                    memmove(mBuffer->data(), mBuffer->data() + tmpOff,
                            size - tmpOff);
                    size -= tmpOff;
                    (void) fetchTimestamp(tmpOff);
                    offset = offset - tmpOff;
                    mBuffer->setRange(0, size);
                    ALOGI("Found I Frame when seeking");
                    mfgFirstFrmAfterSeek = true;
                } else {
                    offset++;
                    continue;
                }
            }
#else

        if (mFormat != NULL && currentStartCode == 0xb8) {
            // GOP layer
            gopFound = true;
            isClosedGop = (data[offset + 7] & 0x40) != 0;
            brokenLink = (data[offset + 7] & 0x20) != 0;
        }

        if (mFormat != NULL && currentStartCode == 0x00) {
#endif
            // Picture start

            if (!sawPictureStart) {
                sawPictureStart = true;
            } else {
                sp<ABuffer> accessUnit = new ABuffer(offset);
                memcpy(accessUnit->data(), data, offset);

                memmove(mBuffer->data(),
                        mBuffer->data() + offset,
                        mBuffer->size() - offset);

                mBuffer->setRange(0, mBuffer->size() - offset);

#ifdef MTK_AOSP_ENHANCEMENT
                bool fgInvalidTimeUs = false;
#endif
                int64_t timeUs = fetchTimestamp(offset
#ifdef MTK_AOSP_ENHANCEMENT
                , &fgInvalidTimeUs
#endif
                );
                if (timeUs < 0ll) {
                    ALOGE("Negative timeUs");
                    return NULL;
                }

                offset = 0;
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
                if (mfgSearchStartCodeOptimize) {
                    mBuffer->meta()->setInt32("LPos", 0);
                    mBuffer->meta()->setInt32("PicS", 0);
                }
#endif
#ifdef MTK_AOSP_ENHANCEMENT
                if(timeUs == mLastTimeUs )
                {
#ifdef MTK_AUDIO_CHANGE_SUPPORT
                    fgInvalidTimeUs = true;
                    ALOGV("set fgInvalidTimeUs:%d",fgInvalidTimeUs);
#endif
                }else {
                    mLastTimeUs = timeUs;
                }
#endif
                accessUnit->meta()->setInt64("timeUs", timeUs);
                if (gopFound && (!brokenLink || isClosedGop)) {
                    accessUnit->meta()->setInt32("isSync", 1);
                }

#ifdef MTK_AOSP_ENHANCEMENT
                accessUnit->meta()->setInt32("invt", (int32_t)fgInvalidTimeUs);
                ALOGV("AU time %lld/%d,FrmType %d,%x %x %x %x %x %x,sz %d\n", (long long)timeUs, (int)fgInvalidTimeUs, (*(accessUnit->data()+5)>>3) & 0x7,
                      *(accessUnit->data()+0), *(accessUnit->data()+1), *(accessUnit->data()+2), *(accessUnit->data()+3),
                      *(accessUnit->data()+4), *(accessUnit->data()+5),
                      (int)accessUnit->size());
                if(mfgFirstFrmAfterSeek)
                {
                      if(fgInvalidTimeUs)
                      {
                            ALOGI("Keep search I with valid PTS\n");
                            mSeeking = true;
                            lastGOPOff = -1;
                            size = mBuffer->size();
                            sawPictureStart = false;
                            continue;
                      }
                      else
                      {
                            mfgFirstFrmAfterSeek = false;
                      }
                }
#endif
                ALOGV("returning MPEG video access unit at time %" PRId64 " us",
                      timeUs);

                // hexdump(accessUnit->data(), accessUnit->size());

                return accessUnit;
            }
        }

        ++offset;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (mSeeking) {             // discard buffer
        (void) fetchTimestamp(offset);
        memmove(mBuffer->data(),
                mBuffer->data() + offset, mBuffer->size() - offset);

        mBuffer->setRange(0, mBuffer->size() - offset);
        offset = 0;
    }
#endif
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
    if (mfgSearchStartCodeOptimize) {
        mBuffer->meta()->setInt32("LPos", ((lastGOPOff != -1)
                                           && (!mfgFirstFrmAfterSeek)
                                           && (offset >
                                               0)) ? (int) lastGOPOff : (int)
                                  offset);
        mBuffer->meta()->setInt32("PicS", (((lastGOPOff != -1)
                                            && (!mfgFirstFrmAfterSeek))
                                           || (offset ==
                                               0)) ? 0 : (int)
                                  sawPictureStart);
        ALOGD("Saved Pos/PicS=0x%x/%d\n", ((lastGOPOff != -1)
                                           && (!mfgFirstFrmAfterSeek)) ? (int)
              lastGOPOff : (int)
              offset, (((lastGOPOff != -1) && (!mfgFirstFrmAfterSeek))
                       || (offset == 0)) ? 0 : (int) sawPictureStart);
    }
#endif
    return NULL;
}

static ssize_t getNextChunkSize(
        const uint8_t *data, size_t size) {
    static const char kStartCode[] = "\x00\x00\x01";

    if (size < 3) {
        return -EAGAIN;
    }

    if (memcmp(kStartCode, data, 3)) {
        return -EAGAIN;
    }

    size_t offset = 3;
    while (offset + 2 < size) {
        if (!memcmp(&data[offset], kStartCode, 3)) {
            return offset;
        }

        ++offset;
    }

    return -EAGAIN;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEG4Video() {
    uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    enum {
        SKIP_TO_VISUAL_OBJECT_SEQ_START,
        EXPECT_VISUAL_OBJECT_START,
        EXPECT_VO_START,
        EXPECT_VOL_START,
        WAIT_FOR_VOP_START,
        SKIP_TO_VOP_START,
#ifdef MTK_AOSP_ENHANCEMENT
        WAIT_FOR_VOL_START,
#endif
    } state;

    if (mFormat == NULL) {
#ifdef MTK_AOSP_ENHANCEMENT
        state = WAIT_FOR_VOL_START;
#else
        state = SKIP_TO_VISUAL_OBJECT_SEQ_START;
#endif
    } else {
        state = SKIP_TO_VOP_START;
    }

    int32_t width = -1, height = -1;

    size_t offset = 0;
    ssize_t chunkSize;
    while ((chunkSize = getNextChunkSize(
                    &data[offset], size - offset)) > 0) {
        bool discard = false;

        unsigned chunkType = data[offset + 3];

        switch (state) {
            case SKIP_TO_VISUAL_OBJECT_SEQ_START:
            {
                if (chunkType == 0xb0) {
                    // Discard anything before this marker.

                    state = EXPECT_VISUAL_OBJECT_START;
                } else {
#ifdef MTK_AOSP_ENHANCEMENT
                offset += chunkSize;
#endif
                    discard = true;
                }
                break;
            }

            case EXPECT_VISUAL_OBJECT_START:
            {
                if (chunkType != 0xb5) {
                    ALOGE("Unexpected chunkType");
                    return NULL;
                }
                state = EXPECT_VO_START;
                break;
            }

            case EXPECT_VO_START:
            {
                if (chunkType > 0x1f) {
                    ALOGE("Unexpected chunkType");
                    return NULL;
                }
                state = EXPECT_VOL_START;
                break;
            }

            case EXPECT_VOL_START:
            {
                if ((chunkType & 0xf0) != 0x20) {
                    ALOGE("Wrong chunkType");
                    return NULL;
                }

                if (!ExtractDimensionsFromVOLHeader(
                            &data[offset], chunkSize,
                            &width, &height)) {
                    ALOGE("Failed to get dimension");
                    return NULL;
                }

                state = WAIT_FOR_VOP_START;
                break;
            }
#ifdef MTK_AOSP_ENHANCEMENT
        case WAIT_FOR_VOL_START:
        {
            if ((chunkType & 0xf0) == 0x20) {
                CHECK(ExtractDimensionsFromVOLHeader
                      (&data[offset], chunkSize, &width, &height));

                state = WAIT_FOR_VOP_START;
                break;
            } else {
                offset += chunkSize;
                discard = true;
            }
            break;
        }
#endif
            case WAIT_FOR_VOP_START:
            {
                if (chunkType == 0xb3 || chunkType == 0xb6) {
                    // group of VOP or VOP start.

                    mFormat = new MetaData;
                    mFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);

                    mFormat->setInt32(kKeyWidth, width);
                    mFormat->setInt32(kKeyHeight, height);

                    ALOGI("found MPEG4 video codec config (%d x %d)",
                         width, height);

                    sp<ABuffer> csd = new ABuffer(offset);
                    memcpy(csd->data(), data, offset);

                    // hexdump(csd->data(), csd->size());

                    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                    mFormat->setData(
                            kKeyESDS, kTypeESDS,
                            esds->data(), esds->size());

                    discard = true;
                    state = SKIP_TO_VOP_START;
                }

                break;
            }

            case SKIP_TO_VOP_START:
            {
                if (chunkType == 0xb6) {
                    int vopCodingType = (data[offset + 4] & 0xc0) >> 6;

                    offset += chunkSize;
#ifdef MTK_AOSP_ENHANCEMENT
                if (mSeeking) {
                    switch (data[4] & 0xC0) {
                    case 0x00:
                        mSeeking = false;
                        ALOGI("I frame");
                        break;
                    case 0x40:
                        ALOGI("P frame");
                        break;
                        //continue;
                    case 0x80:
                        ALOGI("B frame");
                        break;
                        //continue;
                    default:
                        ALOGI("default");
                        break;
                        //continue;
                    }
                }

                if (mSeeking) {
                    discard = true;
                    break;
                }
#endif
                    sp<ABuffer> accessUnit = new ABuffer(offset);
                    memcpy(accessUnit->data(), data, offset);

                    memmove(data, &data[offset], size - offset);
                    size -= offset;
                    mBuffer->setRange(0, size);

                    int64_t timeUs = fetchTimestamp(offset);
                    if (timeUs < 0ll) {
                        ALOGE("Negative timeus");
                        return NULL;
                    }

                    offset = 0;

                    accessUnit->meta()->setInt64("timeUs", timeUs);
                    if (vopCodingType == 0) {  // intra-coded VOP
                        accessUnit->meta()->setInt32("isSync", 1);
                    }

                    ALOGV("returning MPEG4 video access unit at time %" PRId64 " us",
                         timeUs);

                    // hexdump(accessUnit->data(), accessUnit->size());

                    return accessUnit;
                } else if (chunkType != 0xb3) {
                    offset += chunkSize;
                    discard = true;
                }

                break;
            }

            default:
                ALOGE("Unknown state: %d", state);
                return NULL;
        }

        if (discard) {
            (void)fetchTimestamp(offset);
            memmove(data, &data[offset], size - offset);
            size -= offset;
            offset = 0;
            mBuffer->setRange(0, size);
        } else {
            offset += chunkSize;
        }
    }

    return NULL;
}

void ElementaryStreamQueue::signalEOS() {
    if (!mEOSReached) {
        if (mMode == MPEG_VIDEO) {
            const char *theEnd = "\x00\x00\x01\x00";
            appendData(theEnd, 4, 0);
        }
        mEOSReached = true;
    } else {
        ALOGW("EOS already signaled");
    }
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMetadata() {
    size_t size = mBuffer->size();
    if (!size) {
        return NULL;
    }

#ifdef MTK_AOSP_ENHANCEMENT
     return dequeueAccessUnitMetadata_mtk();//cherry
#endif
    sp<ABuffer> accessUnit = new ABuffer(size);
    int64_t timeUs = fetchTimestamp(size);
    accessUnit->meta()->setInt64("timeUs", timeUs);

    memcpy(accessUnit->data(), mBuffer->data(), size);
    mBuffer->setRange(0, 0);

    if (mFormat == NULL) {
        mFormat = new MetaData;
        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_DATA_TIMED_ID3);
    }

    return accessUnit;
}

#ifdef MTK_AOSP_ENHANCEMENT
//=========================================================================================================//
//Vorbis Header Parsing Funcitons
//=========================================================================================================//
status_t vorbis_parse_infoheader(const uint8_t * data,
                                 uint8_t * ChannelCount,
                                 uint32_t * SampleRate, uint32_t * Bitrate) {
    uint8_t vorbisAudioChannels;
    uint32_t vorbisSampleRate;
    uint32_t vorbisBitrateMax;
    uint32_t vorbisBitrateMin;
    uint32_t vorbisBitrateNominal;
    data += 7;                  // Skip Type and vorbis syncword
    data += 4;                  // Skip Vorbis Version
    vorbisAudioChannels = data[0];
    data += 1;
    vorbisSampleRate = U32LE_AT(data);
    data += 4;
    vorbisBitrateMax = U32LE_AT(data);
    data += 4;
    vorbisBitrateNominal = U32LE_AT(data);
    data += 4;
    vorbisBitrateMin = U32LE_AT(data);
    data += 4;
    data += 2;                  // useless size field

    *ChannelCount = vorbisAudioChannels;
    *SampleRate = vorbisSampleRate;
    *Bitrate =
        (vorbisBitrateNominal ==
         0) ? (vorbisBitrateMax +
               vorbisBitrateMin) / 2 : vorbisBitrateNominal;
    return OK;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitVORBISAudio() {
    uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();
    sp<ABuffer> accessUnit = NULL;

    if (size == 0)
        return NULL;

    //In this case, all data in should be a header buffer
    if (mVorbisStatus < 5) {
        if (data[0] == 1 && mVorbisStatus == 0) {
            if (mFormat == NULL) {
                uint8_t ChannelCount;
                uint32_t SampleRate;
                uint32_t Bitrate;
                vorbis_parse_infoheader(data, &ChannelCount, &SampleRate,
                                        &Bitrate);
                ALOGI
                    ("Vorbis Create Formate with %d channels, %d samplerate",
                     ChannelCount, SampleRate);
                mFormat = new MetaData;
                mFormat->setCString(kKeyMIMEType,
                                    MEDIA_MIMETYPE_AUDIO_VORBIS);
                mFormat->setData(kKeyVorbisInfo, 0, data, size);
                mFormat->setInt32(kKeySampleRate, SampleRate);
                mFormat->setInt32(kKeyChannelCount, ChannelCount);
                mFormat->setInt32(kKeyBitRate, Bitrate);
            }
            mVorbisStatus = 2;
            mBuffer->setRange(0, 0);
            return NULL;
        } else if (data[0] == 3 && mVorbisStatus == 2) {
            ALOGI("Parsing Comment header?");
            mBuffer->setRange(0, 0);
            mVorbisStatus = 4;
            return NULL;
        } else if (data[0] == 5 && mVorbisStatus == 4) {
            ALOGI("Parsing Books header, size = %zu", size);
            mFormat->setData(kKeyVorbisBooks, 0, data, size);
            mBuffer->setRange(0, 0);
            mVorbisStatus = 6;
            return NULL;
        } else {
            ALOGI("Header info not completed, drop data, size = %zu", size);
            mBuffer->setRange(0, 0);
            return NULL;
        }
    } else {
        size_t checksize = size;
        while (checksize >= 0xFF)
            checksize -= 0xFF;
        if (checksize) {
            sp<ABuffer> accessUnit = new ABuffer(size);
            memcpy(accessUnit->data(), data, size);
            mBuffer->setRange(0, 0);
            int64_t timeUs = fetchTimestamp(size);
            if (timeUs < 0) {
                ALOGI("timeUs error");
                timeUs = 0;
            }
            CHECK_GE(timeUs, 0ll);
            accessUnit->meta()->setInt64("timeUs", timeUs);
            //hexdump(accessUnit->data(), accessUnit->size());
            return accessUnit;
        } else
            return NULL;
    }
}
#endif
#ifdef MTK_AOSP_ENHANCEMENT
struct asf_bitmapinfoheader_s {
    uint32_t biSize;
    uint32_t biWidth;
    uint32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    uint32_t biXPelsPerMeter;
    uint32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
    uint8_t *data;
};
#if 0                           // check Frame Type
static uint32_t glace = (uint32_t) "lace";
#endif
#define ASF_BITMAPINFOHEADER_SIZE 40
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitVC1Video() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    int prevStartCode = -1;
    int currentStartCode = -1;
    int pprevStartCode = -1;

    size_t offset = 0;
    bool sawPicture = false;
    while (offset + 3 < size) {
        if (memcmp(&data[offset], "\x00\x00\x01", 3)) {
            ++offset;
            continue;
        }
        ALOGV("%02x %02x %02x %02x", data[offset + 0], data[offset + 1],
              data[offset + 2], data[offset + 3]);
        pprevStartCode = prevStartCode;
        prevStartCode = currentStartCode;
        currentStartCode = data[offset + 3];

        /*video sequence start, video_edit_code */
        if (currentStartCode == 0x0F && mFormat == NULL) {
            if (offset) {
                memmove(mBuffer->data(), mBuffer->data() + offset,
                        size - offset);
                size -= offset;
                (void) fetchTimestamp(offset);
                mBuffer->setRange(0, size);
                offset = 0;
            }
        }

        /* parse sequence_header */
        if ((prevStartCode == 0x0F && currentStartCode != 0x0E)
            || (pprevStartCode == 0x0F && prevStartCode == 0x0E)) {
            if (mFormat == NULL) {
                ABitReader bits(data, offset);

                bits.skipBits(32);  /*start code 00 00 01 0F */
                uint8_t profile = bits.getBits(2);
                ALOGD("dequeueAccessUnitVC1Video:   profile is:%u", profile);
                if (profile != 3) {
                    return NULL;
                }

                uint8_t level = bits.getBits(3);
                ALOGD("dequeueAccessUnitVC1Video:   level is:%u", level);

                bits.skipBits(2);   /*COLORDIFF_FORMAT */
                bits.skipBits(3);   /*FRMRTQ_POSTPROC */
                bits.skipBits(5);   /*BITRTQ_POSTPROC */
                bits.skipBits(1);
                 /*POSTPROCFLAG*/
                    uint32_t pictureWidth = (bits.getBits(12) * 2) + 2;
                ALOGD("dequeueAccessUnitVC1Video:   pictureWidth:%u",
                      pictureWidth);

                uint32_t pictureHeight = (bits.getBits(12) * 2) + 2;
                ALOGD("dequeueAccessUnitVC1Video:   pictureHeight:%u",
                      pictureHeight);
#if 0                           // check Frame Type
                bits.skipBits(1);   /* */
                uint32_t interlace = bits.getBits(1);
#endif

                sp<ABuffer> WMVCData =
                    new ABuffer(offset + ASF_BITMAPINFOHEADER_SIZE);
                //uint8_t codecProfile = 8;   /*codec specific data */
                struct asf_bitmapinfoheader_s header;
                memset(&header, 0, ASF_BITMAPINFOHEADER_SIZE);
                header.biSize = offset + ASF_BITMAPINFOHEADER_SIZE;
                header.biWidth = pictureWidth;
                header.biHeight = pictureHeight;
                memcpy(&(header.biCompression), "WVC1", 4);
#if 0
                memcpy(WMVCData->data(), (void *) &codecProfile, 1);
                memcpy(WMVCData->data() + 1, (void *) &pictureWidth, 2);
                memcpy(WMVCData->data() + 3, (void *) &pictureHeight, 2);
#else
                memcpy(WMVCData->data(), (void *) &header,
                       ASF_BITMAPINFOHEADER_SIZE);
#endif

                memcpy(WMVCData->data() + ASF_BITMAPINFOHEADER_SIZE, data,
                       offset);

                mFormat = new MetaData;
                mFormat->setData(kKeyWMVC, 0, WMVCData->data(),
                                 WMVCData->size());
                mFormat->setCString(kKeyMIMEType, "video/x-ms-wmv");
                mFormat->setInt32(kKeyWidth, pictureWidth);
                mFormat->setInt32(kKeyHeight, pictureHeight);
#if 0                           // check Frame Type
                mFormat->setInt32(glace, (int32_t) interlace);
#endif

                memmove(mBuffer->data(), mBuffer->data() + offset,
                        size - offset);
                size -= offset;
                (void) fetchTimestamp(offset);
                mBuffer->setRange(0, mBuffer->size() - offset);
                offset = 0;
                return NULL;
            }

        }
        if (currentStartCode == 0x0E && mFormat != NULL && mSeeking) {
            mSeeking = false;
            memmove(mBuffer->data(), mBuffer->data() + offset, size - offset);
            size -= offset;
            (void) fetchTimestamp(offset);
            offset = 0;
            mBuffer->setRange(0, size);
            ALOGI("Found entry header when seeking");
        }
        if (currentStartCode == 0x0D && mFormat != NULL && !mSeeking) {
            /*frame start code */
#if 0                           // check Frame Type
            int32_t lace = 0;
            ALOGV("%2x %2x %2x %2x %2x", data[0], data[1], data[2],
                  data[3], data[4]);
            uint8_t data0 = data[0];
            if (mFormat->findInt32(glace, &lace) && lace == 1) {
                ALOGI("It is interlace");
                if (data0 & 0x80 == 0x80) {
                    data0 = data0 << 2;
                } else {
                    data0 = data0 << 1;
                }
            }
            if ((data[4] & 0xE0) == 0xC0) {
                ALOGI("I frame");
            } else if ((data[4] & 0x80) == 0x0) {
                ALOGI("P frame");
            } else if ((data[4] & 0xC0) == 0x80) {
                ALOGI("B frame");
            } else if ((data[4] & 0xF0) == 0xE0) {
                ALOGI("BI frame");
            } else if ((data[4] & 0xF0) == 0xF0) {
                ALOGI("Skipped frame");
            }
#endif

            if (!sawPicture) {
                sawPicture = true;
            } else {
                sp<ABuffer> accessUnit = new ABuffer(offset);
                memcpy(accessUnit->data(), data, offset);

                memmove(mBuffer->data(),
                        mBuffer->data() + offset, mBuffer->size() - offset);

                mBuffer->setRange(0, mBuffer->size() - offset);

                int64_t timeUs = fetchTimestamp(offset);
                CHECK_GE(timeUs, 0ll);
                offset = 0;
                accessUnit->meta()->setInt64("timeUs", timeUs);

                return accessUnit;
            }
        }
#if 0
        else if (prevStartCode == 0x0E /* && currentStartCode == 0x0D */ ) {
            /*entry point header */
            ALOGI("entry point header");
            memmove(mBuffer->data(), mBuffer->data() + offset, size - offset);
            (void) fetchTimestamp(offset);
            size -= offset;
            offset = 0;
            mBuffer->setRange(0, size);
        }
#endif

        ++offset;
    }                           /*while */
    if (mSeeking) {             // discard buffer
        (void) fetchTimestamp(offset);
        memmove(mBuffer->data(),
                mBuffer->data() + offset, mBuffer->size() - offset);
        size -= offset;
        mBuffer->setRange(0, mBuffer->size() - offset);
        offset = 0;
    }

    return NULL;
}
#endif

#ifdef MTK_AOSP_ENHANCEMENT     ///////////////////////////////////////////refine
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnit1(){

    if ((mFlags & kFlag_AlignedData) && mMode == H264) {
            if (mRangeInfos.empty()) {
                return NULL;
            }
            ALOGD("[WFD]: ElementaryStreamQueue::dequeueAccessUnit");
            RangeInfo info = *mRangeInfos.begin();
            mRangeInfos.erase(mRangeInfos.begin());

            sp <ABuffer> accessUnit = new ABuffer(info.mLength);
            memcpy(accessUnit->data(), mBuffer->data(), info.mLength);
            accessUnit->meta()->setInt64("timeUs", info.mTimestampUs);

            memmove(mBuffer->data(),
                    mBuffer->data() + info.mLength,
                    mBuffer->size() - info.mLength);

            mBuffer->setRange(0, mBuffer->size() - info.mLength);

            if (mFormat == NULL) {
                mFormat = MakeAVCCodecSpecificData(accessUnit);
            }

            return accessUnit;
        }

        switch (mMode) {
        case H264:
            return dequeueAccessUnitH264_mtk();
        case AAC:
            return dequeueAccessUnitAAC();
        case PSLPCM:
            return dequeueAccessUnitPSLPCM();
        case VORBIS_AUDIO:
            return dequeueAccessUnitVORBISAudio();
        case LPCM:
            return dequeueAccessUnitLPCM();
        case BDLPCM:
            return dequeueAccessUnitBDLPCM();
        case METADATA:
            return dequeueAccessUnitMetadata();
        case VC1_VIDEO:
            return dequeueAccessUnitVC1Video();
        case HEVC:
            return dequeueAccessUnitHEVC();
#ifdef MTK_SUBTITLE_SUPPORT
        case SUBTITLE:
            return dequeueAccessUnitDvbSubtitle();
#endif
        case MPEG_VIDEO:
            return dequeueAccessUnitMPEGVideo();
        case MPEG4_VIDEO:
            return dequeueAccessUnitMPEG4Video();
        case PCM_AUDIO:
            return dequeueAccessUnitPCMAudio();
        case AC3:
            return dequeueAccessUnitAC3();
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
        case EC3:
            return dequeueAccessUnitDDP();
#endif // DOLBY_END
        default:
            CHECK_EQ((unsigned) mMode, (unsigned) MPEG_AUDIO);
            return dequeueAccessUnitMPEGAudio();
        }

}

void ElementaryStreamQueue::setSeeking(bool h264UsePPs) {
    mSeeking = true;
    mH264UsePPs = h264UsePPs;
}

bool ElementaryStreamQueue::IsIFrame(uint8_t * nalStart, size_t nalSize) {
    unsigned nalType = nalStart[0] & 0x1f;
    unsigned slice_type = 0;

    if ((nalType > 0 && nalType < 6) || nalType == 19) {
        ABitReader br(nalStart + 1, nalSize);
        //unsigned first_mb_in_slice = parseUE(&br);
        slice_type = parseUE(&br);
    }

    if ((mH264UsePPs && nalType == 7) ||    //PPS
        (!mH264UsePPs && (nalType == 5 || slice_type == 2 || slice_type == 7))) {   // I frame
        ALOGI("%s() nalType=%d slice_type=%d ,nalSize:%zu", __FUNCTION__,
              nalType, slice_type, nalSize);
        return true;
    }
    return false;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitHEVC() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();
    Vector<NALPosition> nals;
    size_t totalSize = 0;

    status_t err;
    const uint8_t *nalStart;
    size_t nalSize;
    bool foundSlice = false;
    int preVCLIndex = -1;
    bool foundIDR = false;
    while ((err = getNextNALUnit(&data, &size, &nalStart, &nalSize)) == OK) {
        CHECK_GT(nalSize, 0u);

        unsigned sliceType = (nalStart[0] & 0x7E) >> 1;

        bool flush = false;
        if(sliceType == 19){
            foundIDR = true;
        }
        if (sliceType == 35) {
            /*delimiter starting an AU */
            if (foundSlice && (preVCLIndex != -1)) {
                flush = true;
            }
            foundSlice = true;
        } else if (((int)sliceType >= 0 && sliceType <= 9)
                   || (sliceType >= 16 && sliceType <= 21)) {
            //slice_segment_layer_rbsp()
            /*first_slice_segment_in_pic_flag */
            unsigned firstSlice = (nalStart[2] & 0x80) >> 7;

            if (firstSlice) {   //firstSlice indicates an new AU
                if (foundSlice && (preVCLIndex != -1)) {
                    flush = true;
                }
                foundSlice = true;
            }
        }

        if (flush) {
            // The access unit will contain all nal units up to, but excluding
            // the current one, separated by 0x00 0x00 0x00 0x01 startcodes.

            ALOGV
                ("[%s]flush sliceType = %u preVCLIndex:%d nals.size():%zu",
                 __FUNCTION__, sliceType, preVCLIndex, nals.size());

            size_t auSize = 4 * preVCLIndex + totalSize;
            sp<ABuffer> accessUnit = new ABuffer(auSize);

            size_t dstOffset = 0;
            for (int i = 0; i < preVCLIndex; ++i) {
                const NALPosition & pos = nals.itemAt(i);
                ALOGV("[hevc]pos:%zu size:%zu", pos.nalOffset, pos.nalSize);
                uint8_t *tmpnalStart = mBuffer->data() + pos.nalOffset;
                unsigned sliceType = (tmpnalStart[0] & 0x7E) >> 1;
                if(!mSeeking||
                    (mSeeking && (((sliceType >= 16) && (sliceType <= 21))||((sliceType >= 32) && (sliceType <= 34))))){

                      mSeeking = false;
                      memcpy(accessUnit->data() + dstOffset, "\x00\x00\x00\x01", 4);

                      memcpy(accessUnit->data() + dstOffset + 4,
                             mBuffer->data() + pos.nalOffset, pos.nalSize);

                      dstOffset += pos.nalSize + 4;
                }
            }

            const NALPosition & pos = nals.itemAt(preVCLIndex - 1);
            size_t nextScan = pos.nalOffset + pos.nalSize;

            memmove(mBuffer->data(),
                    mBuffer->data() + nextScan, mBuffer->size() - nextScan);

            mBuffer->setRange(0, mBuffer->size() - nextScan);

            int64_t timeUs = fetchTimestamp(nextScan);
            CHECK_GE(timeUs, 0ll);
            if(foundIDR){
                accessUnit->meta()->setInt32("isSync", 1);
            }
            accessUnit->meta()->setInt64("timeUs", timeUs);

            if (mFormat == NULL) {
                mFormat = MakeHEVCMetaData(accessUnit);
                //mFormat = MakeHEVCCodecSpecificData(accessUnit);
            }
            return accessUnit;
        }

        NALPosition pos;
        pos.nalOffset = nalStart - mBuffer->data();
        pos.nalSize = nalSize;

        nals.push_back(pos);
        totalSize += nalSize;
        if (0 <= (int)sliceType && sliceType <= 31) {    /*VCL unit */
            preVCLIndex = nals.size();  //position of preVCLunit
        }
        ALOGV("nals add sliceType:%u, nals.size:%zu", sliceType, nals.size());
    }
    CHECK_EQ(err, (status_t) - EAGAIN);

    return NULL;

}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitH264_mtk() {
    if (accessUnits.empty()) {
        const uint8_t *data = mBuffer->data();
        size_t size = mBuffer->size();
        Vector<NALPosition> nals;
        size_t totalSize = 0;
        size_t seiCount = 0;//SEI
        status_t err;
        const uint8_t *nalStart;
        size_t nalSize;
        bool foundSlice = false;
        bool over = false;
        bool foundIDR = false;
        //while (!over && (err = getNextNALUnit(&data, &size, &nalStart, &nalSize)) == OK)
        while (!over &&(((err =getNextNALUnit(&data, &size, &nalStart, &nalSize)) == OK)
                || ((err != OK) && (nalStart != NULL)
                    && ((nalStart[0] & 0x1f) != 1)
                    && ((nalStart[0] & 0x1f) != 5)))) {

            //CHECK_GT(nalSize, 0u);

            unsigned nalType = nalStart[0] & 0x1f;
            bool flush = false;
            if (err != OK) {
                over = true;
            }
            if (nalType == 1 || nalType == 5) {
                CHECK_GT(nalSize, 0u);
                if (nalType == 5) {
                    foundIDR = true;
                }
                if (foundSlice) {
                    ABitReader br(nalStart + 1, nalSize);
                    unsigned first_mb_in_slice = parseUE(&br);
                    if (first_mb_in_slice == 0) {
                        // This slice starts a new frame.
                        flush = true;
                    }
                }

                foundSlice = true;
            } else if ((nalType == 9 || nalType == 7 || nalType == 8)
                       && foundSlice) {
                // Access unit delimiter and SPS will be associated with the
                // next frame.
                flush = true;
            //SEI
            } else if (nalType == 6 && nalSize > 0) {
                // found non-zero sized SEI
            ++seiCount;
            }

            if (flush) {
                // The access unit will contain all nal units up to, but excluding
                // the current one, separated by 0x00 0x00 0x00 0x01 startcodes.
                size_t auSize = 4 * nals.size() + totalSize;
                sp<ABuffer> MultiAccessUnit = new ABuffer(auSize);
                sp<ABuffer> MultiNal = new ABuffer(auSize);
                size_t dstOffset = 0;
                size_t dstOffset1 = 0;

                /*skip redunt data for fetch right timestamp */
                size_t scanoffset = 0;
                sp<ABuffer> sei;
                //for sei
                if (seiCount > 0) {
                    sei = new ABuffer(seiCount * sizeof(NALPosition));
                    MultiNal->meta()->setBuffer("sei", sei);
                }

                for (size_t i = 0; i < nals.size(); ++i) {
                    const NALPosition & tmppos = nals.itemAt(i);
                    uint8_t *tmpnalStart = mBuffer->data() + tmppos.nalOffset;
                    unsigned nalType = tmpnalStart[0] & 0x1f;
                    if ((nalType != 9) && (nalType != 7)
                        && (nalType != 8)) {
                        //eaqul flush nal type
                        if (i != 0) {
                            const NALPosition & tmppos = nals.itemAt(i - 1);
                            scanoffset = tmppos.nalOffset + tmppos.nalSize;
                            fetchTimestamp(tmppos.nalOffset + tmppos.nalSize);
                        }
                        break;
                    }
                }
                const NALPosition & pos0 = nals.itemAt(nals.size() - 1);
                size_t TotalnextScan = pos0.nalOffset + pos0.nalSize;
                size_t TimestampNextScan =
                    pos0.nalOffset + pos0.nalSize - scanoffset;

                int64_t timeUs; //= fetchTimestamp(nextScan);
                //CHECK_GE(timeUs, 0ll);
                //CHECK(mUseFrameBase==false);
                //for sei
                size_t seiIndex = 0;

                for (size_t i = 0; i < nals.size(); ++i) {
                    const NALPosition & pos = nals.itemAt(i);
                    unsigned nalType = mBuffer->data()[pos.nalOffset] & 0x1f;
                    // for crossmount sei data +
                    if (nalType == 6) {
                        sp<ABuffer> sei_cm = new ABuffer(pos.nalSize);
                        memcpy(sei_cm->data(), mBuffer->data() + pos.nalOffset, pos.nalSize);
                        MultiNal->meta()->setBuffer("sei_cm", sei_cm);
                        ALOGD("get sei_cm buffer");
                    }
                    // for crossmount sei data -
                    //for sei
                    if (nalType == 6 && pos.nalSize > 0) {
                        if (seiIndex >= sei->size() / sizeof(NALPosition)) {
                            ALOGE("Wrong seiIndex");
                            return NULL;
                        }
                        NALPosition &seiPos = ((NALPosition *)sei->data())[seiIndex++];
                        seiPos.nalOffset = dstOffset1 + 4;
                        seiPos.nalSize = pos.nalSize;
                    }
                    if (!mSeeking
                        || (mSeeking
                            && IsIFrame(mBuffer->data() + pos.nalOffset,
                                        pos.nalSize))) {
                        mSeeking = false;   // found I frame, seek complete
                        memcpy(MultiNal->data() + dstOffset1,
                               "\x00\x00\x00\x01", 4);
                        memcpy(MultiNal->data() + dstOffset1 + 4,
                               mBuffer->data() + pos.nalOffset, pos.nalSize);
                        dstOffset1 += pos.nalSize + 4;

                    }
                    memcpy(MultiAccessUnit->data() + dstOffset,
                           "\x00\x00\x00\x01", 4);
                    memcpy(MultiAccessUnit->data() + dstOffset + 4,
                           mBuffer->data() + pos.nalOffset, pos.nalSize);
                    dstOffset += pos.nalSize + 4;
                }

                memmove(mBuffer->data(),
                        mBuffer->data() + TotalnextScan,
                        mBuffer->size() - TotalnextScan);

                mBuffer->setRange(0, mBuffer->size() - TotalnextScan);

                if (mFormat == NULL) {
                    mFormat = MakeAVCCodecSpecificData(MultiAccessUnit);
                }
                timeUs = fetchTimestamp(TimestampNextScan);
                if (dstOffset1 != 0) {  //mUseFrameBase
                    ALOGD("flush timeUs = %lld", (long long)timeUs);
                    MultiNal->setRange(0, dstOffset1);
                    MultiNal->meta()->setInt64("timeUs", timeUs);
                    if (foundIDR) {
                        MultiNal->meta()->setInt32("isSync", 1);
                    }
                    accessUnits.push_back(MultiNal);
                }
                over = true;
            }
            if (nalSize != 0) {
                NALPosition pos;
                pos.nalOffset = nalStart - mBuffer->data();
                pos.nalSize = nalSize;
                nals.push(pos);
                totalSize += nalSize;
            }
        }
    }
    if (!accessUnits.empty()) {
        sp<ABuffer> accessUnit = *accessUnits.begin();
        accessUnits.erase(accessUnits.begin());
        return accessUnit;
    } else {
        return NULL;
    }

}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitPSLPCM() {
    /*  http://www.mpucoder.com/DVD/ass-hdr.html
       10100DDD|NNNNNNNN|LLLLLLLL|LLLLLLLL|E M R NNNNN|BB SS R CCC|XXX YYYYY
       10000DDD(8bits) : sub stream ID 0xA0-A7 (DDD: id)
       NNNNNNNN(8bits) : number of frame headers
       LLL...(16bits) : first access unit pointer
       E : audio emphasis flag (0:off, 1:on)
       M : audio mute flag (0:off, 1:on)
       R : reserved
       NNNNN(5bits) : audio frame number
       BB(2bits) : Bits per sample[bits] (0:16, 1:20, 2:24, 3:reserved)
       SS(2bits) : Sampling frequency[kHz] (0:48, 1:96, etc:reserved)
       CCC(3bits) : number of channels[ch] (CCC + 1)
       XXX(3bits) : dynamic range X (linear gain = 2^(4-(X+(Y/30))))
       YYYYY(5bits) : dynamic range Y (dB gain = 24.082 - 6.0206 X - 0.2007 Y)
     */
    uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();
    size_t offset = 0;
    unsigned ignore = 0;
    int samplingRate, numChannels, numSamples;

    if (mBuffer->size() < 7) {
        return NULL;
    }
    ABitReader bits(mBuffer->data() + offset, mBuffer->size() - offset);
    unsigned streamID = bits.getBits(8);
    while (streamID < 0xa0 || streamID > 0xa7) {
        streamID = bits.getBits(8);
        offset++;
    }
    /*unsigned frame_header_count= */ bits.getBits(8);
    unsigned first_access_unit_pointer = bits.getBits(16);
    ignore = first_access_unit_pointer;
    //offset = offset  + 3 + first_access_unit_pointer;
    /*bool audio_emphasis_flag= */ bits.getBits(1);
    /*bool audio_mute_flag= */ bits.getBits(1);
    bits.getBits(1);
    unsigned audio_frame_num = bits.getBits(5);
    ignore = audio_frame_num;
    unsigned bits_per_sample = bits.getBits(2);
    if (bits_per_sample == 0) {
        numSamples = 16;
    } else if (bits_per_sample == 1) {
        numSamples = 20;
    } else {
        numSamples = 24;
    }
    unsigned sampling_frequency = bits.getBits(2);
    if (sampling_frequency == 0) {
        samplingRate = 48000;
    } else {
        samplingRate = 96000;
    }
    bits.getBits(1);
    numChannels = bits.getBits(3) + 1;
    /*unsigned dynamic_range_X= */ bits.getBits(3);
    /*unsigned dynamic_range_Y= */ bits.getBits(5);
    uint16_t framesize;
    //framesize = samplingRate * numSamples * numChannels * audio_frame_num / 8;
    //if (framesize > size) {
    if ((size - 7) % 2 != 0) {
        framesize = (size - 7) - 1;
    } else {
        framesize = (size - 7);
    }
    //}
    ALOGD("Warning: framesize:%d size:%zu", framesize, size);

    sp<ABuffer> accessUnit = new ABuffer(framesize);
    uint8_t *ptr = accessUnit->data();

#ifdef MTK_AUDIO_RAW_SUPPORT
    memcpy(ptr, data + 7, framesize);
#else
    // 16 bit big endian to little endian
    int j = 0;
    for (int i = 0; i < (framesize / 2); i++) {
        j = 2 * i;
        ptr[j] = data[j + 8];
        ptr[j + 1] = data[j + 7];
    }
#endif

    mBuffer->setRange(0, 0);

    int64_t timeUs = fetchTimestamp(size);
    ALOGD
        ("PCM DEQUEUE timeUs=%lld framesize is %d buffer size is %zu size is %zu offset is %zu",
         (long long)timeUs, framesize, mBuffer->size(), size, offset);
    accessUnit->meta()->setInt64("timeUs", timeUs);

    if (numSamples != 16
#ifdef MTK_AUDIO_RAW_SUPPORT
        && numSamples != 24
#endif
        ) {
        ALOGD("PCM BitWidth %d not support\n", numSamples);
        return accessUnit;
    } else if (mFormat == NULL) {
        mFormat = new MetaData;

        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

        mFormat->setInt32(kKeySampleRate, samplingRate);
        mFormat->setInt32(kKeyChannelCount, numChannels);
#ifdef MTK_AUDIO_RAW_SUPPORT
        mFormat->setInt32(kKeyEndian, 1);   //1: big endian, 2: little endia
        mFormat->setInt32(kKeyBitWidth, numSamples);
        mFormat->setInt32(kKeyPCMType, 3);  //1: WAV file, 2: BD file, 3: DVD_VOB file, 4: DVD_AOB file
        mFormat->setInt32(kKeyChannelAssignment, 0x1b); // 2 channels
        ALOGD
            ("PCM SampleRate %d, ChannelCount %d, Big endian, BitWidth %d, PCMType:DVD_VOB\n",
             samplingRate, numChannels, numSamples);

#endif
    }

    return accessUnit;
}
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitLPCM() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();
    const int64_t framesize = 1920;

    if (size < 4) {
        return NULL;
    }

    PCM_Header *pcmHeader = (PCM_Header *) data;
    if (pcmHeader->sub_stream_id != 0xA0
        || pcmHeader->number_of_frame_header != 0x6) {
        ALOGE("pcmHeader incorrent, subid:%d, numHeader:%d",
              pcmHeader->sub_stream_id, pcmHeader->number_of_frame_header);
        mBuffer->setRange(0, 0);
        return NULL;
    }

    sp<ABuffer> accessUnit = new ABuffer(framesize);
    uint8_t *ptr = accessUnit->data();
#if 1
    int j = 0;
    for (int i = 0; i < (framesize / 2); i++) {
        j = 2 * i;
        ptr[j] = data[4 + j + 1];
        ptr[j + 1] = data[4 + j];
    }
#else
    memcpy(ptr, data + 4, framesize);
#endif
    mBuffer->setRange(0, 0);

    int64_t timeUs = fetchTimestamp(size);
    CHECK_GE(timeUs, 0ll);
    accessUnit->meta()->setInt64("timeUs", timeUs);
    ALOGV("PCM: size:%zu, timeUs:%lld", size, (long long)timeUs);

    if (mFormat == NULL) {
        mFormat = new MetaData;

        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
        if (pcmHeader->audio_sampling_frequency == 1) {
            ALOGW("SampleRate 44100");
            mFormat->setInt32(kKeySampleRate, 44100);
        } else if (pcmHeader->audio_sampling_frequency == 2) {
            ALOGW("SampleRate 48000");
            mFormat->setInt32(kKeySampleRate, 48000);
        } else {
            mFormat.clear();
            ALOGW("SampleRate is uncorrect");
        }

        if (((pcmHeader->number_of_audio_channel != 1) &&
             (pcmHeader->number_of_audio_channel == 0)) ||
            pcmHeader->quantization_word_length != 0) {
            mFormat.clear();
            ALOGW("channel is uncorrect");
        } else {
            mFormat->setInt32(kKeyChannelCount, 2);
        }
    }

    return accessUnit;
}

#ifdef MTK_SUBTITLE_SUPPORT
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitDvbSubtitle() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();
    const int64_t framesize = size + 32;

    if (size < 4) {
        return NULL;
    }

    sp<ABuffer> accessUnit = new ABuffer(framesize);
    uint8_t *ptr = accessUnit->data();
    memcpy(ptr, data, size);
    mBuffer->setRange(0, 0);

    int64_t timeUs = fetchTimestamp(size);
    CHECK_GE(timeUs, 0ll);
    accessUnit->meta()->setInt64("timeUs", timeUs);
    ALOGI("DVB: size:%d, timeUs:%lld", (int)size, (long long)timeUs);

    if (mFormat == NULL) {
        mFormat = new MetaData;

        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_TEXT_DVB);

    }

    return accessUnit;
}
#endif

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitBDLPCM() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    if (size < 4) {
        return NULL;
    }

    uint16_t framesize = U16_AT(data);
    data += 4;
    size -= 4;
    ALOGI("framesize:%d size:%zu", framesize, size);
    if (framesize > size) {
        ALOGI("Warning: framesize:%d size:%zu", framesize, size);
        if (size % 2 != 0) {
            framesize = size - 1;
        } else {
            framesize = size;
        }
    }

    sp<ABuffer> accessUnit = new ABuffer(framesize);
    uint8_t *ptr = accessUnit->data();

    int j = 0;
    for (int i = 0; i < (framesize / 2); i++) {
        j = 2 * i;
        ptr[j] = data[j + 1];
        ptr[j + 1] = data[j];
    }

    mBuffer->setRange(0, 0);

    int64_t timeUs = fetchTimestamp(size + 4);
    CHECK_GE(timeUs, 0ll);
    accessUnit->meta()->setInt64("timeUs", timeUs);

    if (mFormat == NULL) {
        mFormat = new MetaData;

        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

        mFormat->setInt32(kKeySampleRate, 48000);
        mFormat->setInt32(kKeyChannelCount, 2);
    }

    return accessUnit;
}
//for metadata
static int findID3(const uint8_t * start, int offset, int length) {
    int i = 0;

#if 0
    LOGI("dump data to check ID3 location");
    for (j = 0; j < 256; ++j) {
        ALOGD("%02d: %02x", j, start[j]);
    }
#endif

    for (i = offset; i < length; ++i) {
        if ((start[i] == 0x49)
            && (start[i + 1] == 0x44)
            && (start[i + 2] == 0x33)) {
            ALOGD("found right ID3 at %d", i);
            return i;
        }
    }
    ALOGE("can't find ID3");
    return -1;
}

static int findAPIC(const uint8_t * start, int offset, int length) {
    int i = 0;

#if 0
    LOGI("dump data to check APIC location");
    for (j = 0; j < 256; ++j) {
        ALOGD("%02d: %02x", j, start[j]);
    }
#endif

    for (i = offset; i < length; ++i) {
        if ((start[i] == 0x41)
            && (start[i + 1] == 0x50)
            && (start[i + 2] == 0x49)
            && (start[i + 3] == 0x43)) {
            ALOGD("found right APIC at %d", i);
            return i;
        }
    }
    ALOGE("can't find APIC");
    return -1;
}

static int findSOI(const uint8_t * start, int offset, int length) {
    int i = 0;

#if 0
    LOGI("dump data to check SOI location");
    for (j = 0; j < 256; ++j) {
        ALOGD("%02d: %02x", j, start[j]);
    }
#endif

    for (i = offset; i < length; ++i) {
        if ((start[i] == 0xff)
            && (start[i + 1] == 0xd8)) {
            ALOGD("found right SOI at %d", i);
            return i;
        }
    }
    ALOGE("can't find SOI");
    return -1;
}

static int findEOI(const uint8_t * start, int offset, int length) {
    int i = 0;

#if 0
    LOGI("dump data to check EOI location");
    for (int j = 0; j < 256; ++j) {
        ALOGD("%02d: %02x", j, start[j]);
    }
#endif

    for (i = offset; i < length; ++i) {
        if ((start[i] == 0xff)
            && (start[i + 1] == 0xd9)) {
            ALOGD("found right EOI at %d", i);
            return i;
        }
    }
    ALOGE("can't find EOI");
    return -1;
}

sp <ABuffer> ElementaryStreamQueue::dequeueAccessUnitMetadata_mtk() {
    int i, j;
    int length, start, end, apic_pos, id3_pos, tag_length;
    ALOGD("new member: dequeue pes meta");
    size_t size = mBuffer->size();
    if (!size) {
        return NULL;
    }
    int64_t timeUs = fetchTimestamp(size);

    const uint8_t *src =
        (const uint8_t *) mBuffer->data() + mBuffer->offset();

    start = findSOI(src, 0, mBuffer->size());
    end = findEOI(src, 0, mBuffer->size());
    id3_pos = findID3(src, 0, mBuffer->size());
    apic_pos = findAPIC(src, 0, mBuffer->size());
    if ((-1 == id3_pos)
        || (-1 == apic_pos)
        || (apic_pos < id3_pos)) {
        ALOGD("no legal id3 album picture");
    }

    tag_length = mBuffer->data()[apic_pos + 4] * 0x1000000
        + mBuffer->data()[apic_pos + 5] * 0x10000
        + mBuffer->data()[apic_pos + 6] * 0x100
        + mBuffer->data()[apic_pos + 7];
    ALOGD("mBuffer->data()[apic_pos + 4] is 0x%x",
          mBuffer->data()[apic_pos + 4]);
    ALOGD("mBuffer->data()[apic_pos + 5] is 0x%x",
          mBuffer->data()[apic_pos + 5]);
    ALOGD("mBuffer->data()[apic_pos + 6] is 0x%x",
          mBuffer->data()[apic_pos + 6]);
    ALOGD("mBuffer->data()[apic_pos + 7] is 0x%x",
          mBuffer->data()[apic_pos + 7]);
    ALOGD("tag_length is %d", tag_length);

    length = end - start + 2;
    ALOGD("start at %d, end at %d, length is %d", start, end, length);
    if ((start == -1) || (end == -1)) {
        ALOGD("can't find a legal jpeg bitstream.");

        sp<ABuffer> accessUnit = new ABuffer(size);
        accessUnit->meta()->setInt64("timeUs", timeUs);
        accessUnit->meta()->setString("mime", "timedID3/hls", 12);

        memcpy(accessUnit->data(), mBuffer->data(), size);
        mBuffer->setRange(0, 0);

        if (mFormat == NULL) {
            mFormat = new MetaData;
            mFormat->setCString(kKeyMIMEType, "image/jpeg");
        }

        return accessUnit;
    }

    CHECK(mBuffer != NULL);
    mBuffer->setRange(mBuffer->offset() + start, length);

    if (mBuffer->size() == 0) {
        return NULL;
    }

    ALOGD("dump data to check SOI location");
    for (i = 0; i < 16; ++i) {
        ALOGD("%02d: %02x", i, mBuffer->data()[i]);
    }

    ALOGD("dump data to check EOI location");
    for (j = 16; j > 0; --j) {
        ALOGD("%02d: %02x", j, mBuffer->data()[length - j]);
    }

    sp <ABuffer> accessUnit = new ABuffer(length);
    memcpy(accessUnit->data(), mBuffer->data(), length);

    accessUnit->meta()->setString("mime", "image/jpeg", 10);
    accessUnit->meta()->setInt64("timeUs", timeUs);
    ALOGD("set album art picture here");

    if (mFormat == NULL) {
        mFormat = new MetaData;
        mFormat->setCString(kKeyMIMEType, "image/jpeg");
    }

    AString testMimeType;
    sp <ABuffer> testBuffer;
    if (((accessUnit)->meta()->findString("mime", &testMimeType))
        && ((accessUnit)->meta()->findBuffer("pictureBuffer", &testBuffer))) {
        ALOGD("test get access unit meta() ok");
    }

    mBuffer->setRange(0, 0);    // To avoid repeatedly read the same buffer

    return accessUnit;
}

#endif

}  // namespace android
