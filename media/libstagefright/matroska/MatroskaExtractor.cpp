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
#define LOG_TAG "MatroskaExtractor"
#include <utils/Log.h>

#include "MatroskaExtractor.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>

#include <inttypes.h>

#ifdef MTK_AOSP_ENHANCEMENT
#include "../include/avc_utils.h"
#include <media/stagefright/foundation/ABuffer.h>

// big endian fourcc
#define BFOURCC(c1, c2, c3, c4) \
    (c4 << 24 | c3 << 16 | c2 << 8 | c1)

#include <cutils/log.h>
#endif
namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
#define MKV_RIFF_WAVE_FORMAT_PCM            (0x0001)
#define MKV_RIFF_WAVE_FORMAT_ALAW           (0x0006)
#define MKV_RIFF_WAVE_FORMAT_ADPCM_ms       (0x0002)
#define MKV_RIFF_WAVE_FORMAT_ADPCM_ima_wav  (0x0011)
#define MKV_RIFF_WAVE_FORMAT_MULAW          (0x0007)
#define MKV_RIFF_WAVE_FORMAT_MPEGL12        (0x0050)
#define MKV_RIFF_WAVE_FORMAT_MPEGL3         (0x0055)
#define MKV_RIFF_WAVE_FORMAT_AMR_NB         (0x0057)
#define MKV_RIFF_WAVE_FORMAT_AMR_WB         (0x0058)
#define MKV_RIFF_WAVE_FORMAT_AAC            (0x00ff)
#define MKV_RIFF_IBM_FORMAT_MULAW           (0x0101)
#define MKV_RIFF_IBM_FORMAT_ALAW            (0x0102)
#define MKV_RIFF_WAVE_FORMAT_WMAV1          (0x0160)
#define MKV_RIFF_WAVE_FORMAT_WMAV2          (0x0161)
#define MKV_RIFF_WAVE_FORMAT_WMAV3          (0x0162)
#define MKV_RIFF_WAVE_FORMAT_WMAV3_L        (0x0163)
#define MKV_RIFF_WAVE_FORMAT_AAC_AC         (0x4143)
#define MKV_RIFF_WAVE_FORMAT_VORBIS         (0x566f)
#define MKV_RIFF_WAVE_FORMAT_VORBIS1        (0x674f)
#define MKV_RIFF_WAVE_FORMAT_VORBIS2        (0x6750)
#define MKV_RIFF_WAVE_FORMAT_VORBIS3        (0x6751)
#define MKV_RIFF_WAVE_FORMAT_VORBIS1PLUS    (0x676f)
#define MKV_RIFF_WAVE_FORMAT_VORBIS2PLUS    (0x6770)
#define MKV_RIFF_WAVE_FORMAT_VORBIS3PLUS    (0x6771)
#define MKV_RIFF_WAVE_FORMAT_AAC_pm         (0x706d)
#define MKV_RIFF_WAVE_FORMAT_GSM_AMR_CBR    (0x7A21)
#define MKV_RIFF_WAVE_FORMAT_GSM_AMR_VBR    (0x7A22)

static const uint32_t kMP3HeaderMask = 0xfffe0c00;//0xfffe0c00 add by zhihui zhang no consider channel mode
static const char *MKVwave2MIME(uint16_t id) {
    switch (id) {
        case  MKV_RIFF_WAVE_FORMAT_AMR_NB:
        case  MKV_RIFF_WAVE_FORMAT_GSM_AMR_CBR:
        case  MKV_RIFF_WAVE_FORMAT_GSM_AMR_VBR:
            return MEDIA_MIMETYPE_AUDIO_AMR_NB;

        case  MKV_RIFF_WAVE_FORMAT_AMR_WB:
            return MEDIA_MIMETYPE_AUDIO_AMR_WB;

        case  MKV_RIFF_WAVE_FORMAT_AAC:
        case  MKV_RIFF_WAVE_FORMAT_AAC_AC:
        case  MKV_RIFF_WAVE_FORMAT_AAC_pm:
            return MEDIA_MIMETYPE_AUDIO_AAC;

        case  MKV_RIFF_WAVE_FORMAT_VORBIS:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS1:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS2:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS3:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS1PLUS:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS2PLUS:
        case  MKV_RIFF_WAVE_FORMAT_VORBIS3PLUS:
            return MEDIA_MIMETYPE_AUDIO_VORBIS;

        case  MKV_RIFF_WAVE_FORMAT_MPEGL12:
        return MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II;
        case  MKV_RIFF_WAVE_FORMAT_MPEGL3:
            return MEDIA_MIMETYPE_AUDIO_MPEG;

        case MKV_RIFF_WAVE_FORMAT_MULAW:
        case MKV_RIFF_IBM_FORMAT_MULAW:
            return MEDIA_MIMETYPE_AUDIO_G711_MLAW;

        case MKV_RIFF_WAVE_FORMAT_ALAW:
        case MKV_RIFF_IBM_FORMAT_ALAW:
            return MEDIA_MIMETYPE_AUDIO_G711_ALAW;

        case MKV_RIFF_WAVE_FORMAT_PCM:
            return MEDIA_MIMETYPE_AUDIO_RAW;
#if defined(MTK_AUDIO_ADPCM_SUPPORT) || defined(HAVE_ADPCMENCODE_FEATURE)
        case MKV_RIFF_WAVE_FORMAT_ADPCM_ms:
            return MEDIA_MIMETYPE_AUDIO_MS_ADPCM;
        case MKV_RIFF_WAVE_FORMAT_ADPCM_ima_wav:
            return MEDIA_MIMETYPE_AUDIO_DVI_IMA_ADPCM;
#endif
        case MKV_RIFF_WAVE_FORMAT_WMAV1:
            return MEDIA_MIMETYPE_AUDIO_WMA;
        case MKV_RIFF_WAVE_FORMAT_WMAV2:
            return MEDIA_MIMETYPE_AUDIO_WMA;
        default:
            ALOGW("unknown wave %x", id);
            return "";
    };
}

static const uint32_t AACSampleFreqTable[16] =
{
    96000, /* 96000 Hz */
    88200, /* 88200 Hz */
    64000, /* 64000 Hz */
    48000, /* 48000 Hz */
    44100, /* 44100 Hz */
    32000, /* 32000 Hz */
    24000, /* 24000 Hz */
    22050, /* 22050 Hz */
    16000, /* 16000 Hz */
    12000, /* 12000 Hz */
    11025, /* 11025 Hz */
    8000, /*  8000 Hz */
    0, /* future use */
    0, /* future use */
    0, /* future use */
    0  /* escape value */
};

static bool findAACSampleFreqIndex(uint32_t freq, uint8_t &index){
    uint8_t i;
    uint8_t num = sizeof(AACSampleFreqTable)/sizeof(AACSampleFreqTable[0]);
    for (i=0; i < num; i++) {
        if (freq == AACSampleFreqTable[i])
            break;
    }
    if (i > 11)
        return false;

    index = i;
    return true;
}

static uint8_t charLower(uint8_t ch){
    uint8_t ch_out = ch;
    if(ch >= 'A' && ch<= 'Z')
        ch_out = ch + 32;
    return ch_out;
}

/* trans all FOURCC  to lower char */
static uint32_t FourCCtoLower(uint32_t fourcc){
    uint8_t ch_1 = (uint8_t)charLower(fourcc>>24);
    uint8_t ch_2 = (uint8_t)charLower(fourcc>>16);
    uint8_t ch_3 = (uint8_t)charLower(fourcc>>8);
    uint8_t ch_4 = (uint8_t)charLower(fourcc);
    uint32_t fourcc_out = ch_1<<24 | ch_2<<16 | ch_3<<8 | ch_4;

    return fourcc_out;
}

static const char *BMKVFourCC2MIME(uint32_t fourcc) {
    ALOGD("BMKVFourCC2MIME fourcc 0x%8.8x", fourcc);
    uint32_t lowerFourcc = FourCCtoLower(fourcc);
    ALOGD("BMKVFourCC2MIME fourcc to lower 0x%8.8x", lowerFourcc);
    switch (lowerFourcc) {
        case BFOURCC('m', 'p', '4', 'a'):
            return MEDIA_MIMETYPE_AUDIO_AAC;

        case BFOURCC('s', 'a', 'm', 'r'):
            return MEDIA_MIMETYPE_AUDIO_AMR_NB;

        case BFOURCC('s', 'a', 'w', 'b'):
            return MEDIA_MIMETYPE_AUDIO_AMR_WB;

        case BFOURCC('x', 'v', 'i', 'd'):
            return MEDIA_MIMETYPE_VIDEO_XVID;
        case BFOURCC('d', 'i', 'v', 'x'):
            return MEDIA_MIMETYPE_VIDEO_DIVX;
        case BFOURCC('d', 'x', '5', '0'):
        case BFOURCC('m', 'p', '4', 'v'):
            return MEDIA_MIMETYPE_VIDEO_MPEG4;

        case BFOURCC('d', 'i', 'v', '3'):
        case BFOURCC('d', 'i', 'v', '4'):
            return MEDIA_MIMETYPE_VIDEO_DIVX3;

        case BFOURCC('s', '2', '6', '3'):
        case BFOURCC('h', '2', '6', '3'):
            return MEDIA_MIMETYPE_VIDEO_H263;

        case BFOURCC('a', 'v', 'c', '1'):
        case BFOURCC('h', '2', '6', '4'):
            return MEDIA_MIMETYPE_VIDEO_AVC;

        case BFOURCC('m', 'p', 'g', '2'):
            return MEDIA_MIMETYPE_VIDEO_MPEG2;
        case BFOURCC('m', 'j', 'p', 'g'):
        case BFOURCC('m', 'p', 'p', 'g'):
            return MEDIA_MIMETYPE_VIDEO_MJPEG;

                case FOURCC('h', 'v', 'c', '1'):
        case BFOURCC('h', 'e', 'v', 'c'):
        case FOURCC('h', 'e', 'v', '1'):
            return MEDIA_MIMETYPE_VIDEO_HEVC;

        default:
            ALOGW("unknown fourcc 0x%8.8x", fourcc);
            return "";
    }
}

#ifdef MTK_SUBTITLE_SUPPORT
static const char *BMapCodecId2SubTT(const char *const codecID){
    if ((!strcmp("S_TEXT/SSA", codecID))
        ||(!strcmp("S_SSA", codecID))) {
        return MEDIA_MIMETYPE_TEXT_SSA;
    } else if (!strcmp("S_TEXT/ASS", codecID)) {
        return MEDIA_MIMETYPE_TEXT_ASS;
    } else if (!strcmp("S_TEXT/UTF8", codecID)) {
        return MEDIA_MIMETYPE_TEXT_TXT;//need confirm, from BD code, it is SRT
    } else if (!strcmp("S_VOBSUB", codecID)) {
        return MEDIA_MIMETYPE_TEXT_VOBSUB;
    } else {
        ALOGE("unknown subtitle codecId,%s",codecID);
        return "";
    }
}
#endif

static bool get_mp3_info(
        uint32_t header, size_t *frame_size,
        int *out_sampling_rate = NULL, int *out_channels = NULL,
        int *out_bitrate = NULL) {
    *frame_size = 0;

    if (out_sampling_rate) {
        *out_sampling_rate = 0;
    }

    if (out_channels) {
        *out_channels = 0;
    }

    if (out_bitrate) {
        *out_bitrate = 0;
    }

    if ((header & 0xffe00000) != 0xffe00000) {
        ALOGD("line=%d", __LINE__);
        return false;
    }

    unsigned version = (header >> 19) & 3;
    if (version == 0x01) {
        ALOGD("line=%d", __LINE__);
        return false;
    }

    unsigned layer = (header >> 17) & 3;
    if (layer == 0x00) {
        ALOGD("line=%d", __LINE__);
        return false;
    }

    unsigned bitrate_index = (header >> 12) & 0x0f;
    if (bitrate_index == 0 || bitrate_index == 0x0f) {
        // Disallow "free" bitrate.
    ALOGD("line=%d", __LINE__);
        return false;
    }

    unsigned sampling_rate_index = (header >> 10) & 3;
    if (sampling_rate_index == 3) {
    ALOGD("line=%d", __LINE__);
        return false;
    }

    static const int kSamplingRateV1[] = { 44100, 48000, 32000 };
    int sampling_rate = kSamplingRateV1[sampling_rate_index];
    if (version == 2 /* V2 */) {
        sampling_rate /= 2;
    } else if (version == 0 /* V2.5 */) {
        sampling_rate /= 4;
    }

    unsigned padding = (header >> 9) & 1;
    if (layer == 3) {        // layer I

        static const int kBitrateV1[] = {
            32, 64, 96, 128, 160, 192, 224, 256,
            288, 320, 352, 384, 416, 448
        };

        static const int kBitrateV2[] = {
            32, 48, 56, 64, 80, 96, 112, 128,
            144, 160, 176, 192, 224, 256
        };

        int bitrate =
            (version == 3 /* V1 */)
                ? kBitrateV1[bitrate_index - 1]
                : kBitrateV2[bitrate_index - 1];

        if (out_bitrate) {
            *out_bitrate = bitrate;
        }

        *frame_size = (12000 * bitrate / sampling_rate + padding) * 4;
    } else {
        // layer II or III
        static const int kBitrateV1L2[] = {
            32, 48, 56, 64, 80, 96, 112, 128,
            160, 192, 224, 256, 320, 384
        };

        static const int kBitrateV1L3[] = {
            32, 40, 48, 56, 64, 80, 96, 112,
            128, 160, 192, 224, 256, 320
        };

        static const int kBitrateV2[] = {
            8, 16, 24, 32, 40, 48, 56, 64,
            80, 96, 112, 128, 144, 160
        };

        int bitrate;
        if (version == 3 /* V1 */) {
            bitrate = (layer == 2 /* L2 */)
                ? kBitrateV1L2[bitrate_index - 1]
                : kBitrateV1L3[bitrate_index - 1];
        } else {            // V2 (or 2.5)
            bitrate = kBitrateV2[bitrate_index - 1];
        }

        if (out_bitrate) {
            *out_bitrate = bitrate;
        }

        if (version == 3 /* V1 */) {
            *frame_size = 144000 * bitrate / sampling_rate + padding;
        } else {            // V2 or V2.5
#ifdef MTK_AOSP_ENHANCEMENT
            if(layer == 2 /* L2 */){
                    *frame_size = 144000 * bitrate / sampling_rate + padding;
            }else{
#endif
                *frame_size = 72000 * bitrate / sampling_rate + padding;
#ifdef MTK_AOSP_ENHANCEMENT
            }
#endif
        }
    }

    if (out_sampling_rate) {
        *out_sampling_rate = sampling_rate;
    }

    if (out_channels) {
        int channel_mode = (header >> 6) & 3;
        *out_channels = (channel_mode == 3) ? 1 : 2;
    }
    return true;
}

static int mkv_mp3HeaderStartAt(const uint8_t *start, int length, uint32_t header) {
    uint32_t code = 0;
    int i = 0;

    for(i=0; i<length; i++){        //ALOGD("start[%d]=%x", i, start[i]);
        code = (code<<8) + start[i];        //ALOGD("code=0x%8.8x, mask=0x%8.8x", code, kMP3HeaderMask);
        if ((code & kMP3HeaderMask) == (header & kMP3HeaderMask)) {            // some files has no seq start code
            return i - 3;
        }
    }
    return -1;
}
#endif
struct DataSourceReader : public mkvparser::IMkvReader {
    DataSourceReader(const sp<DataSource> &source)
        : mSource(source) {
    }

    virtual int Read(long long position, long length, unsigned char* buffer) {
        CHECK(position >= 0);
        CHECK(length >= 0);

        if (length == 0) {
            return 0;
        }

        ssize_t n = mSource->readAt(position, buffer, length);

        if (n <= 0) {
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGE("readAt %zd bytes, Read return -1\nposition= %lld, length= %ld", n, position, length);
#endif
            return -1;
        }

        return 0;
    }

    virtual int Length(long long* total, long long* available) {
        off64_t size;
        if (mSource->getSize(&size) != OK) {
            *total = -1;
            *available = (long long)((1ull << 63) - 1);

            return 0;
        }

        if (total) {
            *total = size;
        }

        if (available) {
            *available = size;
        }

        return 0;
    }

private:
    sp<DataSource> mSource;

    DataSourceReader(const DataSourceReader &);
    DataSourceReader &operator=(const DataSourceReader &);
};

////////////////////////////////////////////////////////////////////////////////

struct BlockIterator {
    BlockIterator(MatroskaExtractor *extractor, unsigned long trackNum, unsigned long index);
#ifdef MTK_AOSP_ENHANCEMENT
    enum EosType {
        NOTEOS,
        ENDOFSTRM,
        ERRJUMP
    };
    EosType mEosType;
#endif
    bool eos() const;
#ifdef MTK_AOSP_ENHANCEMENT
    EosType getEndOfStream();
#endif

    void advance();
    void reset();

    void seek(
            int64_t seekTimeUs, bool isAudio,
            int64_t *actualFrameTimeUs);

    const mkvparser::Block *block() const;
    int64_t blockTimeUs() const;
#ifdef MTK_SUBTITLE_SUPPORT
    int64_t blockEndTimeUs() const;
#endif

private:
    MatroskaExtractor *mExtractor;
    long long mTrackNum;
    unsigned long mIndex;

    const mkvparser::Cluster *mCluster;
    const mkvparser::BlockEntry *mBlockEntry;
    long mBlockEntryIndex;
#ifdef MTK_AOSP_ENHANCEMENT
    unsigned long mTrackType;
    void backward();
    bool backward_eos(const mkvparser::Cluster*, const mkvparser::BlockEntry*);
    void seekwithoutcue(int64_t seekTimeUs);
#endif

    void advance_l();

    BlockIterator(const BlockIterator &);
    BlockIterator &operator=(const BlockIterator &);
};

struct MatroskaSource : public MediaSource {
    MatroskaSource(
            const sp<MatroskaExtractor> &extractor, size_t index);

    virtual status_t start(MetaData *params);
    virtual status_t stop();

    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

protected:
    virtual ~MatroskaSource();

private:
    enum Type {
        AVC,
        AAC,
#ifdef MTK_AOSP_ENHANCEMENT
        VP8,
        VP9,
        VORBIS,
        MPEG4,
        MPEG2,
        RV,
        MP2_3,
        COOK,
        MJPEG,
        HEVC,
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
        AC3,
        EAC3,
#endif
#endif
        OTHER
    };

    sp<MatroskaExtractor> mExtractor;
    size_t mTrackIndex;
    Type mType;
    bool mIsAudio;
#ifdef MTK_AOSP_ENHANCEMENT
    bool mWantsNALFragments;
#endif
    BlockIterator mBlockIter;
    size_t mNALSizeLen;  // for type AVC

    List<MediaBuffer *> mPendingFrames;

    status_t advance();

    status_t readBlock();
    void clearPendingFrames();

    MatroskaSource(const MatroskaSource &);
    MatroskaSource &operator=(const MatroskaSource &);
#ifdef MTK_AOSP_ENHANCEMENT
    status_t findMP3Header(uint32_t *header);
    unsigned char* mTrackContentAddData;
    size_t mTrackContentAddDataSize;
    bool mNewFrame;
    int64_t mCurrentTS;
    bool mFirstFrame;
    uint32_t mMP3Header;
    bool mIsFromFFmpeg;
    uint64_t mDefaultDurationNs;
public:
    void setCodecInfoFromFirstFrame();
#endif
};

const mkvparser::Track* MatroskaExtractor::TrackInfo::getTrack() const {
    return mExtractor->mSegment->GetTracks()->GetTrackByNumber(mTrackNum);
}

// This function does exactly the same as mkvparser::Cues::Find, except that it
// searches in our own track based vectors. We should not need this once mkvparser
// adds the same functionality.
const mkvparser::CuePoint::TrackPosition *MatroskaExtractor::TrackInfo::find(
        long long timeNs) const {
    ALOGV("mCuePoints.size %zu", mCuePoints.size());
    if (mCuePoints.empty()) {
        return NULL;
    }

    const mkvparser::CuePoint* cp = mCuePoints.itemAt(0);
    const mkvparser::Track* track = getTrack();
    if (timeNs <= cp->GetTime(mExtractor->mSegment)) {
        return cp->Find(track);
    }

    // Binary searches through relevant cues; assumes cues are ordered by timecode.
    // If we do detect out-of-order cues, return NULL.
    size_t lo = 0;
    size_t hi = mCuePoints.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const mkvparser::CuePoint* const midCp = mCuePoints.itemAt(mid);
        const long long cueTimeNs = midCp->GetTime(mExtractor->mSegment);
        if (cueTimeNs <= timeNs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return NULL;
    }

    cp = mCuePoints.itemAt(lo - 1);
    if (cp->GetTime(mExtractor->mSegment) > timeNs) {
        return NULL;
    }

    return cp->Find(track);
}

MatroskaSource::MatroskaSource(
        const sp<MatroskaExtractor> &extractor, size_t index)
    : mExtractor(extractor),
      mTrackIndex(index),
      mType(OTHER),
      mIsAudio(false),
#ifdef MTK_AOSP_ENHANCEMENT
      mWantsNALFragments(false),
#endif
      mBlockIter(mExtractor.get(),
                 mExtractor->mTracks.itemAt(index).mTrackNum,
                 index),
      mNALSizeLen(0) {
#ifdef MTK_AOSP_ENHANCEMENT
        mCurrentTS = 0;
        mFirstFrame = true;
        (mExtractor->mTracks.itemAt(index)).mTrack->GetContentAddInfo(&mTrackContentAddData, &mTrackContentAddDataSize);
        //add debug info
        ALOGD("MatroskaSource constructor,mTrackContentAddDataSize=%zu", mTrackContentAddDataSize);
        for(size_t i = 0; i < mTrackContentAddDataSize; i++){
            ALOGD("mTrackContentAddData[%zu] = 0x%x", i, *(mTrackContentAddData + i));
        }
        //check whether is ffmeg video with codecID of  V_MS/VFW/FOURCC
        mIsFromFFmpeg = false;
        const char * CodecId = (mExtractor->mTracks.itemAt(index)).mTrack->GetCodecId();
        ALOGI("MatroskaSource contructor mCodecId = %s",CodecId);
        if (!strcmp("V_MS/VFW/FOURCC", CodecId))
                mIsFromFFmpeg = true;

        //get default duration--set timestamp for each frame in a block
        mDefaultDurationNs = 0;
        mDefaultDurationNs = (mExtractor->mTracks.itemAt(index)).mTrack->GetDefaultDuration();
        ALOGD("MatroskaSource contructor mDefaultDurationNs = %lld", (long long)mDefaultDurationNs);
#endif
    sp<MetaData> meta = mExtractor->mTracks.itemAt(index).mMeta;

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    mIsAudio = !strncasecmp("audio/", mime, 6);

    if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
        mType = AVC;

        uint32_t dummy;
        const uint8_t *avcc;
        size_t avccSize;
#ifdef MTK_AOSP_ENHANCEMENT
        if (!meta->findData(kKeyAVCC, &dummy, (const void **)&avcc, &avccSize)){
            sp<MetaData> metadata = NULL;
            while (metadata == NULL){
                clearPendingFrames();
                            while (mPendingFrames.empty()){
                                status_t err = readBlock();

                                if (err != OK) {
                                    clearPendingFrames();
                                    break;
                                }
                            }
            if(!mPendingFrames.empty()){
                MediaBuffer *buffer = *mPendingFrames.begin();
                sp < ABuffer >  accessUnit = new ABuffer(buffer->range_length());
                ALOGD("bigbuf->range_length() = %zu", buffer->range_length());
                memcpy(accessUnit->data(),buffer->data(),buffer->range_length());
                metadata = MakeAVCCodecSpecificData(accessUnit);
            }
        }
        CHECK(metadata->findData(kKeyAVCC, &dummy, (const void **)&avcc, &avccSize));
        ALOGD("avccSize = %zu ", avccSize);
        CHECK_GE(avccSize, 5u);
        meta->setData(kKeyAVCC, 0, avcc, avccSize);
        mBlockIter.reset();
        clearPendingFrames();
    }
#endif
        CHECK(meta->findData(
                    kKeyAVCC, &dummy, (const void **)&avcc, &avccSize));

        CHECK_GE(avccSize, 5u);

        mNALSizeLen = 1 + (avcc[4] & 3);
        ALOGV("mNALSizeLen = %zu", mNALSizeLen);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        mType = AAC;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_VPX)){
        mType = VP8;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_VP9)){
        mType = VP9;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)){
        mType = VORBIS;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4))    {
        mType = MPEG4;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_XVID)){
        mType = MPEG4;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_DIVX)){
        mType = MPEG4;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_DIVX3))    {
        mType = MPEG4;
    }else if(!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG2)){
        mType = MPEG2;
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)){
        mType = MP2_3;
        if (findMP3Header(&mMP3Header) != OK){
            ALOGW("No mp3 header found");
        }
        ALOGD("mMP3Header=0x%8.8x", mMP3Header);
    }else if(!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MJPEG)) {
        mType=MJPEG;
    }else if(!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)){
        uint32_t dummy;
        const uint8_t *hvcc;
        size_t hvccSize;
        mType=HEVC;
        ALOGI("MatroskaSource, is HEVC");

        if (!meta->findData(kKeyHVCC, &dummy, (const void **)&hvcc, &hvccSize)) {
            sp<MetaData> metadata = NULL;
            while (metadata == NULL){
                clearPendingFrames();
                while (mPendingFrames.empty()) {
                    status_t err = readBlock();

                    if (err != OK) {
                        clearPendingFrames();
                        break;
                    }
                }
                if(!mPendingFrames.empty()){
                    MediaBuffer *buffer = *mPendingFrames.begin();
                    sp < ABuffer >  accessUnit = new ABuffer(buffer->range_length());
                    ALOGD("firstBuffer->range_length() = %zu", buffer->range_length());
                    memcpy(accessUnit->data(),buffer->data(),buffer->range_length());
                    metadata = MakeHEVCCodecSpecificData(accessUnit);
                }
            }
            CHECK(metadata->findData(kKeyHVCC, &dummy, (const void **)&hvcc, &hvccSize));
            ALOGD("avccSize = %zu ", hvccSize);
            CHECK_GE(hvccSize, 5u);
            meta->setData(kKeyHVCC, 0, hvcc, hvccSize);
            mBlockIter.reset();
            clearPendingFrames();
        }
        CHECK(meta->findData(kKeyHVCC, &dummy, (const void **)&hvcc, &hvccSize));
        CHECK_GE(hvccSize, 5u);
        mNALSizeLen = 1 + (hvcc[21] & 3);
        ALOGI("hevc mNALSizeLen = %zu", mNALSizeLen);
    }
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
    else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AC3)) {
        mType = AC3;
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_EAC3)) {
        mType = EAC3;
    }
#endif
    ALOGI("MatroskaSource constructor mType=%d",mType);
#endif
}

MatroskaSource::~MatroskaSource() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("~MatroskaSource destructor");
#endif
    clearPendingFrames();
}

#ifdef MTK_AOSP_ENHANCEMENT
status_t MatroskaSource::start(MetaData * params) {
#else
status_t MatroskaSource::start(MetaData * /* params */) {
#endif
    mBlockIter.reset();
#ifdef MTK_AOSP_ENHANCEMENT
    mNewFrame = true;

    int32_t val;
    if (params && params->findInt32(kKeyWantsNALFragments, &val)
        && val != 0) {
        mWantsNALFragments = true;
    } else {
        mWantsNALFragments = false;
    }
    ALOGD("MatroskaSource start,mWantsNALFragments=%s",mWantsNALFragments?"true":"false");
#endif

    return OK;
}

status_t MatroskaSource::stop() {
    clearPendingFrames();

    return OK;
}

sp<MetaData> MatroskaSource::getFormat() {
    return mExtractor->mTracks.itemAt(mTrackIndex).mMeta;
}

////////////////////////////////////////////////////////////////////////////////

BlockIterator::BlockIterator(
        MatroskaExtractor *extractor, unsigned long trackNum, unsigned long index)
    : mExtractor(extractor),
      mTrackNum(trackNum),
      mIndex(index),
      mCluster(NULL),
      mBlockEntry(NULL),
      mBlockEntryIndex(0) {
#ifdef MTK_AOSP_ENHANCEMENT
        mTrackType = mExtractor->mSegment->GetTracks()->GetTrackByNumber(trackNum)->GetType();
      mEosType = NOTEOS;
#endif
    reset();
}

bool BlockIterator::eos() const {
#ifdef MTK_AOSP_ENHANCEMENT
    return (mCluster == NULL || mCluster->EOS()) || ((mEosType == ERRJUMP) && (mTrackType == 17));
#else
    return mCluster == NULL || mCluster->EOS();
#endif
}
#ifdef MTK_AOSP_ENHANCEMENT
BlockIterator::EosType BlockIterator::getEndOfStream(){
    return mEosType;
}
#endif

void BlockIterator::advance() {
    Mutex::Autolock autoLock(mExtractor->mLock);
    advance_l();
}

void BlockIterator::advance_l() {
    for (;;) {
        long res = mCluster->GetEntry(mBlockEntryIndex, mBlockEntry);
        ALOGV("GetEntry returned %ld", res);

        long long pos;
        long len;
        if (res < 0) {
            // Need to parse this cluster some more

            CHECK_EQ(res, mkvparser::E_BUFFER_NOT_FULL);

            res = mCluster->Parse(pos, len);
            ALOGV("Parse returned %ld", res);

            if (res < 0) {
                // I/O error
#ifdef MTK_AOSP_ENHANCEMENT
                //start ALPS01558424
                ALOGV("Cluster::Parse returned result %ld", res);

                const mkvparser::Cluster *nextCluster;
                res = mExtractor->mSegment->ParseNext(
                mCluster, nextCluster, pos, len);
                ALOGE("try ParseNext returned %ld", res);

                if (res != 0) {
                    // EOF or error
                    ALOGI("try to parser next cluster, res %ld,pos:0x%llx",res,pos);
                    mEosType = ENDOFSTRM;
                    mCluster = NULL;
                    break;
                }

                CHECK_EQ(res, 0);
                CHECK(nextCluster != NULL);
                CHECK(!nextCluster->EOS());

                mCluster = nextCluster;

                res = mCluster->Parse(pos, len);
                if (res < 0) {
                    // I/O error
                    ALOGE("Cluster::Parse(3) returned result %ld", res);
                    mEosType = ENDOFSTRM;
                    mCluster = NULL;
                    break;
                }
                ALOGE("Parse (3) returned %ld", res);
                CHECK_GE(res, 0);

                mBlockEntryIndex = 0;
                mEosType = NOTEOS;
                continue;
                //end ALPS01558424
#else
                mCluster = NULL;
                break;
#endif
            }
#ifdef MTK_AOSP_ENHANCEMENT
            mEosType = NOTEOS;
#endif
            continue;
        } else if (res == 0) {
            // We're done with this cluster

            const mkvparser::Cluster *nextCluster;
            res = mExtractor->mSegment->ParseNext(
                    mCluster, nextCluster, pos, len);
            ALOGV("ParseNext returned %ld", res);

            if (res != 0) {
                // EOF or error
#ifdef MTK_AOSP_ENHANCEMENT
                ALOGI("BlockIterator::advance_l,no more cluter found in file, res %ld",res);
                mEosType = ENDOFSTRM;
#endif

                mCluster = NULL;
                break;
            }

            CHECK_EQ(res, 0);
            CHECK(nextCluster != NULL);
            CHECK(!nextCluster->EOS());

            mCluster = nextCluster;

            res = mCluster->Parse(pos, len);
#ifdef MTK_AOSP_ENHANCEMENT
            if (res < 0) {
                // I/O error
                ALOGE("Cluster::Parse(2) returned result %ld", res);

                const mkvparser::Cluster *nextCluster;
                res = mExtractor->mSegment->ParseNext(
                mCluster, nextCluster, pos, len);
                ALOGE("try Next & Next returned %ld", res);

                if (res != 0) {
                    // EOF or error
                    ALOGI("try to parser next cluster, res %ld,pos:0x%llx",res,pos);
                    mEosType = ENDOFSTRM;
                    mCluster = NULL;
                    break;
                }

                CHECK_EQ(res, 0);
                CHECK(nextCluster != NULL);
                CHECK(!nextCluster->EOS());

                mCluster = nextCluster;

                res = mCluster->Parse(pos, len);
                if (res < 0) {
                    // I/O error
                    ALOGE("Cluster::Parse(4) returned result %ld", res);
                    mEosType = ENDOFSTRM;
                    mCluster = NULL;
                    break;
                }
                ALOGE("Parse (5) returned %ld", res);
                CHECK_GE(res, 0);

                mBlockEntryIndex = 0;
                mEosType = NOTEOS;
                if (17 == mTrackType) {//add this control follow for ALPS01482593, also control by this flag "mEosType"
                    //mCluster = NULL;
                    ALOGE("subtitle, no need to parser more");
                    mEosType = ERRJUMP;
                    break;
                }
                continue;
                //end ALPS0558424
                //mCluster = NULL;
                //break;
            }
#endif
            ALOGV("Parse (2) returned %ld", res);
            CHECK_GE(res, 0);

            mBlockEntryIndex = 0;
#ifdef MTK_AOSP_ENHANCEMENT
            mEosType = NOTEOS;
            if (17 == mTrackType) {//add this control follow for ALPS01482593, also control by this flag "mEosType"
                //mCluster = NULL;
                ALOGE("subtitle, no need to parser more");
                mEosType = ERRJUMP;
                break;
            }
#endif
            continue;
        }

        CHECK(mBlockEntry != NULL);
        CHECK(mBlockEntry->GetBlock() != NULL);
        ++mBlockEntryIndex;
#ifdef MTK_AOSP_ENHANCEMENT
            mEosType = NOTEOS;
#endif

        if (mBlockEntry->GetBlock()->GetTrackNumber() == mTrackNum) {
            break;
        }
    }
}

void BlockIterator::reset() {
    Mutex::Autolock autoLock(mExtractor->mLock);

    mCluster = mExtractor->mSegment->GetFirst();
    mBlockEntry = NULL;
    mBlockEntryIndex = 0;

    do {
        advance_l();
    } while (!eos() && block()->GetTrackNumber() != mTrackNum);
}

void BlockIterator::seek(
        int64_t seekTimeUs, bool isAudio,
        int64_t *actualFrameTimeUs) {
    Mutex::Autolock autoLock(mExtractor->mLock);

    *actualFrameTimeUs = -1ll;

    const int64_t seekTimeNs = seekTimeUs * 1000ll - mExtractor->mSeekPreRollNs;

    mkvparser::Segment* const pSegment = mExtractor->mSegment;

    // Special case the 0 seek to avoid loading Cues when the application
    // extraneously seeks to 0 before playing.
    if (seekTimeNs <= 0) {
        ALOGV("Seek to beginning: %" PRId64, seekTimeUs);
        mCluster = pSegment->GetFirst();
        mBlockEntryIndex = 0;
        do {
            advance_l();
        } while (!eos() && block()->GetTrackNumber() != mTrackNum);
        return;
    }

    ALOGV("Seeking to: %" PRId64, seekTimeUs);

    // If the Cues have not been located then find them.
    const mkvparser::Cues* pCues = pSegment->GetCues();
    const mkvparser::SeekHead* pSH = pSegment->GetSeekHead();
    if (!pCues && pSH) {
        const size_t count = pSH->GetCount();
        const mkvparser::SeekHead::Entry* pEntry;
        ALOGV("No Cues yet");

        for (size_t index = 0; index < count; index++) {
            pEntry = pSH->GetEntry(index);

            if (pEntry->id == 0x0C53BB6B) { // Cues ID
                long len; long long pos;
                pSegment->ParseCues(pEntry->pos, pos, len);
                pCues = pSegment->GetCues();
                ALOGV("Cues found");
                break;
            }
        }

        if (!pCues) {
            ALOGE("No Cues in file");
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGI("no cue data,seek without cue data");
            seekwithoutcue(seekTimeUs);
#endif
            return;
        }
    }
    else if (!pSH) {
        ALOGE("No SeekHead");
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("no seekhead, seek without cue data");
        seekwithoutcue(seekTimeUs);
#endif
        return;
    }

    const mkvparser::CuePoint* pCP;
    mkvparser::Tracks const *pTracks = pSegment->GetTracks();
    while (!pCues->DoneParsing()) {
        pCues->LoadCuePoint();
        pCP = pCues->GetLast();
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGV("pCP= %s",pCP==NULL?"NULL":"not NULL");
        if(pCP==NULL)
            continue;
#else
        CHECK(pCP);
#endif

        size_t trackCount = mExtractor->mTracks.size();
        for (size_t index = 0; index < trackCount; ++index) {
            MatroskaExtractor::TrackInfo& track = mExtractor->mTracks.editItemAt(index);
            const mkvparser::Track *pTrack = pTracks->GetTrackByNumber(track.mTrackNum);
            if (pTrack && pTrack->GetType() == 1 && pCP->Find(pTrack)) { // VIDEO_TRACK
                track.mCuePoints.push_back(pCP);
            }
        }

        if (pCP->GetTime(pSegment) >= seekTimeNs) {
            ALOGV("Parsed past relevant Cue");
            break;
        }
    }

    const mkvparser::CuePoint::TrackPosition *pTP = NULL;
    const mkvparser::Track *thisTrack = pTracks->GetTrackByNumber(mTrackNum);
    if (thisTrack->GetType() == 1) { // video
        MatroskaExtractor::TrackInfo& track = mExtractor->mTracks.editItemAt(mIndex);
        pTP = track.find(seekTimeNs);
    } else {
        // The Cue index is built around video keyframes
        unsigned long int trackCount = pTracks->GetTracksCount();
        for (size_t index = 0; index < trackCount; ++index) {
            const mkvparser::Track *pTrack = pTracks->GetTrackByIndex(index);
            if (pTrack && pTrack->GetType() == 1 && pCues->Find(seekTimeNs, pTrack, pCP, pTP)) {
                ALOGV("Video track located at %zu", index);
                break;
            }
        }
    }


    // Always *search* based on the video track, but finalize based on mTrackNum
    if (!pTP) {
        ALOGE("Did not locate the video track for seeking");
#ifdef MTK_AOSP_ENHANCEMENT
        seekwithoutcue(seekTimeUs);
#endif
        return;
    }

    mCluster = pSegment->FindOrPreloadCluster(pTP->m_pos);
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("Cluster num=%ld",pSegment->GetCount());
#endif

    CHECK(mCluster);
    CHECK(!mCluster->EOS());

    // mBlockEntryIndex starts at 0 but m_block starts at 1
    CHECK_GT(pTP->m_block, 0);
    mBlockEntryIndex = pTP->m_block - 1;

    for (;;) {
        advance_l();

        if (eos()) break;

        if (isAudio || block()->IsKey()) {
            // Accept the first key frame
            int64_t frameTimeUs = (block()->GetTime(mCluster) + 500LL) / 1000LL;
            if (thisTrack->GetType() == 1 || frameTimeUs >= seekTimeUs) {
                *actualFrameTimeUs = frameTimeUs;
                ALOGV("Requested seek point: %" PRId64 " actual: %" PRId64,
                      seekTimeUs, *actualFrameTimeUs);
                break;
            }
        }
    }
}

const mkvparser::Block *BlockIterator::block() const {
    CHECK(!eos());

    return mBlockEntry->GetBlock();
}

int64_t BlockIterator::blockTimeUs() const {
    return (mBlockEntry->GetBlock()->GetTime(mCluster) + 500ll) / 1000ll;
}

#ifdef MTK_SUBTITLE_SUPPORT
int64_t BlockIterator::blockEndTimeUs() const {
    if (mBlockEntry->GetKind() == mkvparser::BlockEntry::kBlockGroup){
        mkvparser::BlockGroup* p = (mkvparser::BlockGroup*)(mBlockEntry);
        //ALOGD("p->GetDuration() %lld ",p->GetDuration());
        if (p->GetDuration()){
            mkvparser::Segment* const pSegment = mExtractor->mSegment;
            long long timeScale = pSegment->GetInfo()->GetTimeCodeScale();
            //ALOGD("p->timeScale() %lld ",timeScale);
            return (p->GetDuration() * timeScale + 500ll) / 1000ll + blockTimeUs() ;
        }else{
            return -1;// unvalue
        }
    }else{
        return -1;// unvalue
    }
}
#endif
////////////////////////////////////////////////////////////////////////////////

static unsigned U24_AT(const uint8_t *ptr) {
    return ptr[0] << 16 | ptr[1] << 8 | ptr[2];
}

void MatroskaSource::clearPendingFrames() {
    while (!mPendingFrames.empty()) {
        MediaBuffer *frame = *mPendingFrames.begin();
        mPendingFrames.erase(mPendingFrames.begin());

        frame->release();
        frame = NULL;
    }
}

status_t MatroskaSource::readBlock() {
    CHECK(mPendingFrames.empty());

    if (mBlockIter.eos()) {
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGI("MatroskaSource::readBlock,%s end of stream",mIsAudio? "audio":"video");
        if (BlockIterator::ERRJUMP == mBlockIter.getEndOfStream()){//for ALPS01558424, these files' data is error, need do error jump, but can't return end of stream.
            mBlockIter.advance();
            return ERROR_MALFORMED;
        }
#endif
        return ERROR_END_OF_STREAM;
    }

    const mkvparser::Block *block = mBlockIter.block();

    int64_t timeUs = mBlockIter.blockTimeUs();

#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_SUBTITLE_SUPPORT
    int64_t timeUsEnd = mBlockIter.blockEndTimeUs();
#endif
    uint32_t frameCount = block->GetFrameCount();
    ALOGV("readBlock,%s Block frameCount =%d,block timeUs =%lld",
        mIsAudio? "audio":"video", frameCount, (long long)timeUs);
    if (mType == MP2_3){
        uint64_t size = block->GetDataSize();
        MediaBuffer *bigbuf= new MediaBuffer(size+frameCount*mTrackContentAddDataSize);
        uint64_t buf_offset=0;
        for (uint32_t i = 0; i < frameCount; ++i) {
            const mkvparser::Block::Frame &frame = block->GetFrame(i);
            MediaBuffer *mbuf = new MediaBuffer(frame.len);
            long n = frame.Read(mExtractor->mReader, (unsigned char *)mbuf->data());
            if (n != 0) {
                mPendingFrames.clear();
                mBlockIter.advance();
                return ERROR_IO;
            }
            if(mTrackContentAddDataSize != 0){
                memcpy((unsigned char *)bigbuf->data()+buf_offset, mTrackContentAddData, mTrackContentAddDataSize);
                buf_offset+=mTrackContentAddDataSize;
            }
            memcpy((unsigned char *)bigbuf->data()+buf_offset,mbuf->data(),frame.len);
            buf_offset += frame.len;
            mbuf->release();
        }
        if(buf_offset!=(size+frameCount*mTrackContentAddDataSize)){
            uint64_t tmp = buf_offset - (size + frameCount * mTrackContentAddDataSize);
            ALOGD("mp3 data count failed,we lost %llu number data ", (unsigned long long)tmp);
            return ERROR_IO;
        }
#ifdef MTK_SUBTITLE_SUPPORT
        if (timeUsEnd && mType == OTHER){
            bigbuf->meta_data()->setInt64(kKeyDriftTime, timeUsEnd);
            ALOGD("subtitle mType = %d timeUsBegin: %lld timeUsEnd %lld ",mType,(long long)timeUs,(long long)timeUsEnd);
        }
#endif
        bigbuf->meta_data()->setInt64(kKeyTime, timeUs);
        bigbuf->meta_data()->setInt32(kKeyIsSyncFrame, block->IsKey());
        mPendingFrames.push_back(bigbuf);
    }else{
         for (uint32_t i = 0; i < frameCount; ++i) {
            const mkvparser::Block::Frame &frame = block->GetFrame(i);
            ALOGV("readBlock,%s frame.len=%ld,mTrackContentAddDataSize=%zu",
                                                                                          mIsAudio? "audio":"video",
                                                                                          frame.len,
                                                                                          mTrackContentAddDataSize);
            MediaBuffer *mbuf = new MediaBuffer(frame.len+mTrackContentAddDataSize);
            if (mTrackContentAddDataSize != 0){
                memcpy(mbuf->data(), mTrackContentAddData, mTrackContentAddDataSize);
            }
            long n = frame.Read(mExtractor->mReader, (unsigned char *)mbuf->data()+mTrackContentAddDataSize);

            if((mDefaultDurationNs > 0) && (mType == AAC)) {
                mbuf->meta_data()->setInt64(kKeyTime, timeUs + i * (mDefaultDurationNs /1000));
                ALOGV("readBlock,%s Block frame %u timestamp %llu",
                               mIsAudio? "audio":"video", i,
                               (unsigned long long)(timeUs + i * (mDefaultDurationNs /1000)));
            }else{
                mbuf->meta_data()->setInt64(kKeyTime, timeUs);
            }
#ifdef MTK_SUBTITLE_SUPPORT
            if (timeUsEnd && mType == OTHER){
                mbuf->meta_data()->setInt64(kKeyDriftTime, timeUsEnd);
                ALOGD("subtitle mType = %d timeUsBegin: %lld timeUsEnd %lld ",mType,(long long)timeUs,(long long)timeUsEnd);
            }
#endif
            mbuf->meta_data()->setInt32(kKeyIsSyncFrame, block->IsKey());
            if (n != 0) {
                mPendingFrames.clear();
                 mBlockIter.advance();
                return ERROR_IO;
            }
            mPendingFrames.push_back(mbuf);
        }
    }
#else
    for (int i = 0; i < block->GetFrameCount(); ++i) {
        const mkvparser::Block::Frame &frame = block->GetFrame(i);

        MediaBuffer *mbuf = new MediaBuffer(frame.len);
        mbuf->meta_data()->setInt64(kKeyTime, timeUs);
        mbuf->meta_data()->setInt32(kKeyIsSyncFrame, block->IsKey());

        long n = frame.Read(mExtractor->mReader, (unsigned char *)mbuf->data());
        if (n != 0) {
            mPendingFrames.clear();

            mBlockIter.advance();
            return ERROR_IO;
        }

        mPendingFrames.push_back(mbuf);
    }
#endif
    mBlockIter.advance();

    return OK;
}

status_t MatroskaSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    *out = NULL;

#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("%s mType=%d,MatroskaSource::read--> ",mIsAudio? "audio":"video",mType);
#endif
    int64_t targetSampleTimeUs = -1ll;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
#ifdef MTK_AOSP_ENHANCEMENT
    bool seeking = false;
#endif
    if (options && options->getSeekTo(&seekTimeUs, &mode)
            && !mExtractor->isLiveStreaming()) {
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("seek, mEosType =%d",mBlockIter.mEosType);
        if (OTHER == mType){
            mBlockIter.mEosType = BlockIterator::NOTEOS;
        }
#endif
        clearPendingFrames();

        // The audio we want is located by using the Cues to seek the video
        // stream to find the target Cluster then iterating to finalize for
        // audio.
        int64_t actualFrameTimeUs;
        mBlockIter.seek(seekTimeUs, mIsAudio, &actualFrameTimeUs);

#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("read, seeking mode=%d,seekTimeUs=%lld,%s mType=%d,actualFrameTimeUs=%lld",
                                                                                mode, (long long)seekTimeUs,
                                                                                mIsAudio? "audio":"video",
                                                                                mType, (long long)actualFrameTimeUs);

        if (mIsAudio||mode == ReadOptions::SEEK_CLOSEST) {
            ALOGD("mIsAudio=%d or mode=%d,need set targetSampleTimeUs=seekTimeUs",mIsAudio,mode);
            targetSampleTimeUs = seekTimeUs;
            seeking = true;
        }
#else
        if (mode == ReadOptions::SEEK_CLOSEST) {
            targetSampleTimeUs = actualFrameTimeUs;
        }
#endif
    }

    while (mPendingFrames.empty()) {
        status_t err = readBlock();

        if (err != OK) {
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGW("%s,mType=%d,MatroskaSource::readBlock fail err=%d", mIsAudio? "audio":"video", mType, err);
#endif
            clearPendingFrames();

            return err;
        }
    }

    MediaBuffer *frame = *mPendingFrames.begin();
#ifdef MTK_AOSP_ENHANCEMENT
    if (seeking || mFirstFrame){
        ALOGD("MatroskaSource::read,%s mType=%d,seeking =%d or mFirstFrame=%d",mIsAudio? "audio":"video",mType,seeking,mFirstFrame);
        mFirstFrame = false;
        frame->meta_data()->findInt64(kKeyTime, &mCurrentTS);
        if(mCurrentTS >= 0)
            ALOGD("frame mCurrentTS=%lld", (long long)mCurrentTS);
        else{
            ALOGE("frame mCurrentTS=%lld, set ts = 0", (long long)mCurrentTS);
            mCurrentTS =0;
            frame->meta_data()->setInt64(kKeyTime, mCurrentTS);
        }
    }
    size_t size = frame->range_length();
    ALOGV("%s mType=%d,frame size =%zu", mIsAudio? "audio":"video", mType, size);
    if (seeking && (mType == VP8||mType == VP9||mType == MPEG4||mType==MJPEG||mType==MPEG2||mType==HEVC))   {
        frame->meta_data()->setInt64(kKeyTargetTime, (targetSampleTimeUs>= 0ll?targetSampleTimeUs:seekTimeUs));
    }

    if ((mType != AVC) && (mType != HEVC)) {

        if (MP2_3 == mType) {
            ALOGV("MatroskaSource::read MP2_3-->");
            int32_t start = -1;
            while (start < 0) {
                start = mkv_mp3HeaderStartAt((const uint8_t*)frame->data()+frame->range_offset(), frame->range_length(), mMP3Header);
                ALOGV("start=%d, frame->range_length() = %zu, frame->range_offset() =%zu",
                    start, frame->range_length(), frame->range_offset());
                if (start >= 0)
                    break;
                frame->release();
                mPendingFrames.erase(mPendingFrames.begin());
                while (mPendingFrames.empty()) {
                    status_t err = readBlock();
                    if (err != OK) {
                        clearPendingFrames();               //      ALOGE("tianread MatroskaSource::read-----<");
                        return err;
                    }
                }
                frame = *mPendingFrames.begin();
                frame->meta_data()->findInt64(kKeyTime, &mCurrentTS);   //ALOGD("mCurrentTS1=%lld", mCurrentTS);
            }

            frame->set_range(frame->range_offset()+start, frame->range_length()-start);

            uint32_t header = *(uint32_t*)((uint8_t*)frame->data()+frame->range_offset());
            header = ((header >> 24) & 0xff) | ((header >> 8) & 0xff00) | ((header << 8) & 0xff0000) | ((header << 24) & 0xff000000);   //ALOGD("HEADER=%8.8x", header);
            size_t frame_size;
            int out_sampling_rate;
            int out_channels;
            int out_bitrate;
            if (!get_mp3_info(header, &frame_size, &out_sampling_rate, &out_channels, &out_bitrate)) {
                ALOGE("MP3 Header read fail!!");
                return ERROR_UNSUPPORTED;
            }
            MediaBuffer *buffer = new MediaBuffer(frame_size);
            ALOGV("MP3 frame %zu frame->range_length() %zu", frame_size, frame->range_length());

            if (frame_size > frame->range_length()) {
                memcpy(buffer->data(), (uint8_t*)(frame->data())+frame->range_offset(), frame->range_length());
                size_t sumSize =0;
                sumSize += frame->range_length();
                size_t needSize = frame_size - frame->range_length();
                frame->release();
                mPendingFrames.erase(mPendingFrames.begin());
                while (mPendingFrames.empty()) {
                    status_t err = readBlock();

                    if (err != OK) {
                        clearPendingFrames();
                        return err;
                    }
                }
                frame = *mPendingFrames.begin();
                size_t offset = frame->range_offset();
                size_t size = frame->range_length();

                while(size < needSize){//the next buffer frame is not enough to fullfill mp3 frame, we have read until mp3 frame is completed.
                    memcpy((uint8_t*)(buffer->data())+sumSize, (uint8_t*)(frame->data())+offset, size);
                    needSize -= size;
                    sumSize+=size;
                    frame->release();
                    mPendingFrames.erase(mPendingFrames.begin());
                    while (mPendingFrames.empty()) {
                        status_t err = readBlock();

                        if (err != OK) {
                            clearPendingFrames();
                            return err;
                        }
                    }
                    frame = *mPendingFrames.begin();
                    offset = frame->range_offset();
                    size = frame->range_length();
                }
                memcpy((uint8_t*)(buffer->data())+sumSize, (uint8_t*)(frame->data())+offset, needSize);
                frame->set_range(offset+needSize, size-needSize);
             } else {
                size_t offset = frame->range_offset();
                size_t size = frame->range_length();
                memcpy(buffer->data(), (uint8_t*)(frame->data())+offset, frame_size);
                frame->set_range(offset+frame_size, size-frame_size);
            }
            if (frame->range_length() < 4) {
                frame->release();
                frame = NULL;
                mPendingFrames.erase(mPendingFrames.begin());
            }
            ALOGV("MatroskaSource::read MP2_3 frame kKeyTime=%lld,kKeyTargetTime=%lld",
                                                                (long long)mCurrentTS, (long long)targetSampleTimeUs);
            buffer->meta_data()->setInt64(kKeyTime, mCurrentTS);
            mCurrentTS += (int64_t)frame_size*8000ll/out_bitrate;

            if (targetSampleTimeUs >= 0ll)
                buffer->meta_data()->setInt64(kKeyTargetTime, targetSampleTimeUs);
            *out = buffer;
            ALOGV("MatroskaSource::read MP2_3--<, keyTime=%lld for next frame", (long long)mCurrentTS);
            return OK;

        }else {
        ALOGV("MatroskaSource::read,not AVC,HEVC,mp3,return frame directly,kKeyTargetTime=%lld",
                                                                        (long long)targetSampleTimeUs);
#else
    mPendingFrames.erase(mPendingFrames.begin());

    if (mType != AVC) {
#endif
        if (targetSampleTimeUs >= 0ll) {
            frame->meta_data()->setInt64(
                    kKeyTargetTime, targetSampleTimeUs);
        }
        *out = frame;
#ifdef MTK_AOSP_ENHANCEMENT
            mPendingFrames.erase(mPendingFrames.begin());
            return OK;
        }
#else

        return OK;
#endif
    }

#ifdef MTK_AOSP_ENHANCEMENT
    //is AVC or HEVC
    if (size < mNALSizeLen) {
        *out = frame;       //frame->release();
        frame = NULL;
        mPendingFrames.erase(mPendingFrames.begin());
        ALOGE("[Warning]size:%zu < mNALSizeLen:%zu", size, mNALSizeLen);
        return OK;
        }
    ALOGV("MatroskaSource::read,mType is %d(AVC/HEVC),mWantsNALFragments=%d",mType,mWantsNALFragments);
    if(!mWantsNALFragments) {
#endif
    // Each input frame contains one or more NAL fragments, each fragment
    // is prefixed by mNALSizeLen bytes giving the fragment length,
    // followed by a corresponding number of bytes containing the fragment.
    // We output all these fragments into a single large buffer separated
    // by startcodes (0x00 0x00 0x00 0x01).

    const uint8_t *srcPtr =
        (const uint8_t *)frame->data() + frame->range_offset();

    size_t srcSize = frame->range_length();

#ifdef MTK_AOSP_ENHANCEMENT
    if (( srcSize >=4 && *(srcPtr+0) == 0x00 && *(srcPtr+1) == 0x00 && *(srcPtr+2) == 0x00 && (*(srcPtr+3) == 0x01 || *(srcPtr+3) == 0x00)) ||
        (mIsFromFFmpeg && frame->range_length() >= 3 && *(srcPtr+0) == 0x00 && *(srcPtr+1) == 0x00 && *(srcPtr+2) == 0x01)){
        //already nal start code+nal data
        ALOGV("MatroskaSource::read,frame is already nal start code + nal data,return frame directly, isFromFFMpeg=%d",mIsFromFFmpeg);
         if (targetSampleTimeUs >= 0ll) {
                frame->meta_data()->setInt64(
                        kKeyTargetTime, targetSampleTimeUs);
         }
            *out = frame;
        mPendingFrames.erase(mPendingFrames.begin());
        return OK;
    }
#endif
    size_t dstSize = 0;
    MediaBuffer *buffer = NULL;
    uint8_t *dstPtr = NULL;

    for (int32_t pass = 0; pass < 2; ++pass) {
        size_t srcOffset = 0;
        size_t dstOffset = 0;
        while (srcOffset + mNALSizeLen <= srcSize) {
            size_t NALsize;
            switch (mNALSizeLen) {
                case 1: NALsize = srcPtr[srcOffset]; break;
                case 2: NALsize = U16_AT(srcPtr + srcOffset); break;
                case 3: NALsize = U24_AT(srcPtr + srcOffset); break;
                case 4: NALsize = U32_AT(srcPtr + srcOffset); break;
                default:
                    TRESPASS();
            }
            if (srcOffset + mNALSizeLen + NALsize > srcSize) {
                break;
            }

            if (pass == 1) {
                memcpy(&dstPtr[dstOffset], "\x00\x00\x00\x01", 4);

#ifdef MTK_AOSP_ENHANCEMENT
                     size_t NALtype = (srcPtr[srcOffset + mNALSizeLen] & 0x7E) >> 1;
                    ALOGV("read,pass =%d,memcpy one NAL (type=%zu,NALsize=%zu) to buffer",
                                   pass, NALtype, NALsize);
#endif
                memcpy(&dstPtr[dstOffset + 4],
                       &srcPtr[srcOffset + mNALSizeLen],
                       NALsize);
            }

            dstOffset += 4;  // 0x00 00 00 01
            dstOffset += NALsize;

            srcOffset += mNALSizeLen + NALsize;
        }

        if (srcOffset < srcSize) {
            // There were trailing bytes or not enough data to complete
            // a fragment.

            frame->release();
            frame = NULL;
            mPendingFrames.erase(mPendingFrames.begin());

            return ERROR_MALFORMED;
        }

        if (pass == 0) {
            dstSize = dstOffset;

            buffer = new MediaBuffer(dstSize);

            int64_t timeUs;
            CHECK(frame->meta_data()->findInt64(kKeyTime, &timeUs));
            int32_t isSync;
            CHECK(frame->meta_data()->findInt32(kKeyIsSyncFrame, &isSync));

            buffer->meta_data()->setInt64(kKeyTime, timeUs);
            buffer->meta_data()->setInt32(kKeyIsSyncFrame, isSync);

            dstPtr = (uint8_t *)buffer->data();
        }
    }

    frame->release();
    frame = NULL;

    if (targetSampleTimeUs >= 0ll) {
        buffer->meta_data()->setInt64(
                kKeyTargetTime, targetSampleTimeUs);
    }

    *out = buffer;
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGV("read return,buffer range_length=%zu,buffer offset=%zu",
                        buffer->range_length(), buffer->range_offset());
        mPendingFrames.erase(mPendingFrames.begin());
#endif

    return OK;
#ifdef MTK_AOSP_ENHANCEMENT
    }else{
        //1. Maybe more than one NALs in one sample, so we should parse and send
        //these NALs to decoder one by one, other than skip data.
        //2. Just send the pure NAL data to decoder. (No NAL size field or start code)
        uint8_t *data = (uint8_t *)frame->data() + frame->range_offset();

        size_t NALsize = 0;
        if (frame->range_length() >=4 && *(data+0) == 0x00 && *(data+1) == 0x00 && *(data+2) == 0x00 && *(data+3) == 0x01)
        {
            mNALSizeLen = 4;
            MediaBuffer *tmpbuffer = *mPendingFrames.begin();

            uint8_t * data = (uint8_t*)tmpbuffer->data() + tmpbuffer->range_offset();
            int size = tmpbuffer->range_length();           //ALOGD("accessunit size = %d",size);
            int tail = 4;
            while(tail <= size - 4) {
                if((*(data+tail+0) == 0x00 && *(data+tail+1) == 0x00 && *(data+tail+2) == 0x00 && *(data+tail+3) == 0x01) || tail == size -4)
                {
                    int nalsize = 0;
                    if(tail == size -4){
                        nalsize = size;
                    }else{
                        nalsize = tail;
                    }
                    NALsize = nalsize - 4;
                    break;
                }
                tail++;
            }
        }//add by mtk80691 to support NAL start code of V_MS/VFW/FOURCC track with 00 00 01
        else if(mIsFromFFmpeg && frame->range_length() >= 3 && *(data+0) == 0x00 && *(data+1) == 0x00 && *(data+2) == 0x01){
            ALOGV("MatroskaSource read,NAL start with 00 00 01");
            mNALSizeLen = 3;
            MediaBuffer *tmpbuffer = *mPendingFrames.begin();

            uint8_t * data = (uint8_t*)tmpbuffer->data() + tmpbuffer->range_offset();
            int size = tmpbuffer->range_length();
            int tail = 3;
            while(tail <= size - 3){
                if((*(data+tail+0) == 0x00 && *(data+tail+1) == 0x00 && *(data+tail+2) == 0x01) || tail == size -3){
                    int nalsize = 0;
                    if(tail == size -3){
                        nalsize = size;
                    }else{
                        nalsize = tail;
                    }
                    NALsize = nalsize - 3;
                    break;
                }
                tail++;
            }
        }else{
                switch (mNALSizeLen) {
                    case 1: NALsize = data[0]; break;
                    case 2: NALsize = U16_AT(&data[0]); break;
                    case 3: NALsize = U24_AT(&data[0]); break;
                    case 4: NALsize = U32_AT(&data[0]); break;
                    default:
                        TRESPASS();
                }
           //  ALOGE("MatroskaSource, size =%d, NALsize =%d, mNALSizeLen=%d", size, NALsize, mNALSizeLen);
          }
        if (size < NALsize + mNALSizeLen) {         //frame->release();
            frame->set_range(frame->range_offset() + mNALSizeLen, frame->range_length() - mNALSizeLen);
            *out = frame;
            frame = NULL;
            mPendingFrames.erase(mPendingFrames.begin());
            ALOGV("MatroskaSource read, size:%zu, NALsize:%zu, mNALSizeLen:%zu", size, NALsize, mNALSizeLen);
            return OK;
        }
        MediaBuffer *buffer = new MediaBuffer(NALsize);
        int64_t timeUs;
        CHECK(frame->meta_data()->findInt64(kKeyTime, &timeUs));
        int32_t isSync;
        CHECK(frame->meta_data()->findInt32(kKeyIsSyncFrame, &isSync));

        buffer->meta_data()->setInt64(kKeyTime, timeUs);
        buffer->meta_data()->setInt32(kKeyIsSyncFrame, isSync);
        memcpy((uint8_t *)buffer->data(),
               (const uint8_t *)frame->data() + frame->range_offset() + mNALSizeLen,
               NALsize);
        frame->set_range(frame->range_offset() + mNALSizeLen + NALsize
                        ,frame->range_length() - mNALSizeLen - NALsize);

        ALOGV("read,one NAL return,buffer range_length=%zu,buffer offset=%zu,timeUs=%lld,sync=%d",
                       buffer->range_length(), buffer->range_offset(), (long long)timeUs, isSync);

        if (frame->range_length() == 0) {
            frame->release();
            frame = NULL;
            mPendingFrames.erase(mPendingFrames.begin());
            ALOGV("all NAL parsed of the frame ");
            //mNewFrame = true;
        }

        if (seeking){
            buffer->meta_data()->setInt64(kKeyTargetTime, targetSampleTimeUs >= 0ll?targetSampleTimeUs:seekTimeUs);
            int64_t tmp = targetSampleTimeUs >= 0ll? targetSampleTimeUs : seekTimeUs;
            ALOGI("read,seeking,kKeyTargetTime=%lld", (long long)tmp);
        }
        *out = buffer;
        return OK;
    }
#endif
}

////////////////////////////////////////////////////////////////////////////////

MatroskaExtractor::MatroskaExtractor(const sp<DataSource> &source)
    : mDataSource(source),
      mReader(new DataSourceReader(mDataSource)),
      mSegment(NULL),
      mExtractedThumbnails(false),
      mIsWebm(false),
      mSeekPreRollNs(0) {
    off64_t size;
    mIsLiveStreaming =
        (mDataSource->flags()
            & (DataSource::kWantsPrefetching
                | DataSource::kIsCachingDataSource))
        && mDataSource->getSize(&size) != OK;

    mkvparser::EBMLHeader ebmlHeader;
    long long pos;
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGD("=====================================\n");
    ALOGD("MatroskaExtractor constructor +++ \n");
    ALOGD("[MKV Playback capability info]\n");
    ALOGD("Support Codec = \"Video:AVC,H263,MPEG4,HEVC,VP8,VP9\" \n");
    ALOGD("Support Codec = \"Audio: VORBIS,AAC,AMR,MP3\" \n");
    ALOGD("=====================================\n");
#endif
    if (ebmlHeader.Parse(mReader, pos) < 0) {
        return;
    }

    if (ebmlHeader.m_docType && !strcmp("webm", ebmlHeader.m_docType)) {
        mIsWebm = true;
    }

    long long ret =
        mkvparser::Segment::CreateInstance(mReader, pos, mSegment);

    if (ret) {
        CHECK(mSegment == NULL);
        return;
    }

#ifdef MTK_AOSP_ENHANCEMENT
if (isLiveStreaming()){
ALOGI("MatroskaExtractor is live streaming");
#endif
    // from mkvparser::Segment::Load(), but stop at first cluster
    ret = mSegment->ParseHeaders();
    if (ret == 0) {
        long len;
        ret = mSegment->LoadCluster(pos, len);
        if (ret >= 1) {
            // no more clusters
            ret = 0;
        }
    } else if (ret > 0) {
        ret = mkvparser::E_BUFFER_NOT_FULL;
    }
#ifdef MTK_AOSP_ENHANCEMENT
}else{
ret = mSegment->ParseHeaders();
        if(ret<0){
            ALOGE("MatroskaExtractor,Segment parse header return fail %lld", ret);
            delete mSegment;
            mSegment = NULL;
            return;
        }else if (ret == 0) {
        const mkvparser::Cues* mCues= mSegment->GetCues();
        const mkvparser::SeekHead* mSH = mSegment->GetSeekHead();
        if((mCues==NULL)&&(mSH!=NULL)){
            size_t count = mSH->GetCount();
            const mkvparser::SeekHead::Entry* mEntry;
            for (size_t index = 0; index < count; index++) {
                mEntry = mSH->GetEntry(index);
                if (mEntry->id == 0x0C53BB6B) { // Cues ID
                    long len;
                    long long pos;
                    mSegment->ParseCues(mEntry->pos, pos, len);
                    mCues = mSegment->GetCues();
                    ALOGD("find cue data by seekhead");
                    break;
                }
            }
        }

        if(mCues){
            ALOGI("has Cue data");
            long len;
            ret = mSegment->LoadCluster(pos, len);
            ALOGD("Cluster num=%ld",mSegment->GetCount());
            ALOGD("LoadCluster done");
        } else  {
            ALOGW("no Cue data");
            ret = mSegment->Load();
        }
}else if (ret > 0) {
ret = mkvparser::E_BUFFER_NOT_FULL;
}
}
#endif

    if (ret < 0) {
        ALOGW("Corrupt %s source: %s", mIsWebm ? "webm" : "matroska",
                uriDebugString(mDataSource->getUri()).c_str());
        delete mSegment;
        mSegment = NULL;
        return;
    }

#if 0
    const mkvparser::SegmentInfo *info = mSegment->GetInfo();
    ALOGI("muxing app: %s, writing app: %s",
         info->GetMuxingAppAsUTF8(),
         info->GetWritingAppAsUTF8());
#endif

#ifdef MTK_AOSP_ENHANCEMENT
    mFileMetaData = new MetaData;
    mFileMetaData->setInt32(kKeyVideoPreCheck, 1);
#endif
    addTracks();
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGD("MatroskaExtractor constructor ---");
#endif
}

MatroskaExtractor::~MatroskaExtractor() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("~MatroskaExtractor destructor");
#endif
    delete mSegment;
    mSegment = NULL;

    delete mReader;
    mReader = NULL;
}

size_t MatroskaExtractor::countTracks() {
    return mTracks.size();
}

sp<MediaSource> MatroskaExtractor::getTrack(size_t index) {
    if (index >= mTracks.size()) {
        return NULL;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    sp<MediaSource> matroskasource = new MatroskaSource(this, index);
    int32_t isinfristframe = false;
    ALOGI("getTrack,index=%zu", index);
    if (mTracks.itemAt(index).mMeta->findInt32(kKeyCodecInfoIsInFirstFrame, &isinfristframe)
        && isinfristframe) {
        ALOGD("Codec info is in first frame");;
        (static_cast<MatroskaSource*>(matroskasource.get()))->setCodecInfoFromFirstFrame();
    }
    return matroskasource;
#else
    return new MatroskaSource(this, index);
#endif
}

sp<MetaData> MatroskaExtractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    if (index >= mTracks.size()) {
        return NULL;
    }

    if ((flags & kIncludeExtensiveMetaData) && !mExtractedThumbnails
            && !isLiveStreaming()) {
        findThumbnails();
        mExtractedThumbnails = true;
    }

    return mTracks.itemAt(index).mMeta;
}

bool MatroskaExtractor::isLiveStreaming() const {
    return mIsLiveStreaming;
}

static int bytesForSize(size_t size) {
    // use at most 28 bits (4 times 7)
    CHECK(size <= 0xfffffff);

    if (size > 0x1fffff) {
        return 4;
    } else if (size > 0x3fff) {
        return 3;
    } else if (size > 0x7f) {
        return 2;
    }
    return 1;
}

static void storeSize(uint8_t *data, size_t &idx, size_t size) {
    int numBytes = bytesForSize(size);
    idx += numBytes;

    data += idx;
    size_t next = 0;
    while (numBytes--) {
        *--data = (size & 0x7f) | next;
        size >>= 7;
        next = 0x80;
    }
}

static void addESDSFromCodecPrivate(
        const sp<MetaData> &meta,
        bool isAudio, const void *priv, size_t privSize) {

    int privSizeBytesRequired = bytesForSize(privSize);
    int esdsSize2 = 14 + privSizeBytesRequired + privSize;
    int esdsSize2BytesRequired = bytesForSize(esdsSize2);
    int esdsSize1 = 4 + esdsSize2BytesRequired + esdsSize2;
    int esdsSize1BytesRequired = bytesForSize(esdsSize1);
    size_t esdsSize = 1 + esdsSize1BytesRequired + esdsSize1;
    uint8_t *esds = new uint8_t[esdsSize];

    size_t idx = 0;
    esds[idx++] = 0x03;
    storeSize(esds, idx, esdsSize1);
    esds[idx++] = 0x00; // ES_ID
    esds[idx++] = 0x00; // ES_ID
    esds[idx++] = 0x00; // streamDependenceFlag, URL_Flag, OCRstreamFlag
    esds[idx++] = 0x04;
    storeSize(esds, idx, esdsSize2);
    esds[idx++] = isAudio ? 0x40   // Audio ISO/IEC 14496-3
                          : 0x20;  // Visual ISO/IEC 14496-2
    for (int i = 0; i < 12; i++) {
        esds[idx++] = 0x00;
    }
    esds[idx++] = 0x05;
    storeSize(esds, idx, privSize);
    memcpy(esds + idx, priv, privSize);

    meta->setData(kKeyESDS, 0, esds, esdsSize);

    delete[] esds;
    esds = NULL;
}

status_t addVorbisCodecInfo(
        const sp<MetaData> &meta,
        const void *_codecPrivate, size_t codecPrivateSize) {
    // hexdump(_codecPrivate, codecPrivateSize);

    if (codecPrivateSize < 1) {
        return ERROR_MALFORMED;
    }

    const uint8_t *codecPrivate = (const uint8_t *)_codecPrivate;

    if (codecPrivate[0] != 0x02) {
        return ERROR_MALFORMED;
    }

    // codecInfo starts with two lengths, len1 and len2, that are
    // "Xiph-style-lacing encoded"...

    size_t offset = 1;
    size_t len1 = 0;
    while (offset < codecPrivateSize && codecPrivate[offset] == 0xff) {
        if (len1 > (SIZE_MAX - 0xff)) {
            return ERROR_MALFORMED; // would overflow
        }
        len1 += 0xff;
        ++offset;
    }
    if (offset >= codecPrivateSize) {
        return ERROR_MALFORMED;
    }
    if (len1 > (SIZE_MAX - codecPrivate[offset])) {
        return ERROR_MALFORMED; // would overflow
    }
    len1 += codecPrivate[offset++];

    size_t len2 = 0;
    while (offset < codecPrivateSize && codecPrivate[offset] == 0xff) {
        if (len2 > (SIZE_MAX - 0xff)) {
            return ERROR_MALFORMED; // would overflow
        }
        len2 += 0xff;
        ++offset;
    }
    if (offset >= codecPrivateSize) {
        return ERROR_MALFORMED;
    }
    if (len2 > (SIZE_MAX - codecPrivate[offset])) {
        return ERROR_MALFORMED; // would overflow
    }
    len2 += codecPrivate[offset++];

    if (len1 > SIZE_MAX - len2 || offset > SIZE_MAX - (len1 + len2) ||
            codecPrivateSize < offset + len1 + len2) {
        return ERROR_MALFORMED;
    }

    if (codecPrivate[offset] != 0x01) {
        return ERROR_MALFORMED;
    }
    meta->setData(kKeyVorbisInfo, 0, &codecPrivate[offset], len1);

    offset += len1;
    if (codecPrivate[offset] != 0x03) {
        return ERROR_MALFORMED;
    }

    offset += len2;
    if (codecPrivate[offset] != 0x05) {
        return ERROR_MALFORMED;
    }

    meta->setData(
            kKeyVorbisBooks, 0, &codecPrivate[offset],
            codecPrivateSize - offset);

    return OK;
}

void MatroskaExtractor::addTracks() {
    const mkvparser::Tracks *tracks = mSegment->GetTracks();
#ifdef MTK_AOSP_ENHANCEMENT
    bool hasVideo = false;
    bool hasAudio = false;
#endif

    for (size_t index = 0; index < tracks->GetTracksCount(); ++index) {
        const mkvparser::Track *track = tracks->GetTrackByIndex(index);

        if (track == NULL) {
            // Apparently this is currently valid (if unexpected) behaviour
            // of the mkv parser lib.
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGW("MatroskaExtractor::addTracks,Unsupport track type");
#endif
            continue;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        String8 codecIDString = String8(track->GetCodecId());
        ALOGI("addTracks,fileCodecId = %s",codecIDString.string());
        codecIDString.toUpper();
        const char* codecID = codecIDString.string();
        ALOGI("addTracks,codec id to upper = %s",codecID);
        ALOGI("addTracks,codec name = %s", track->GetCodecNameAsUTF8());
#if defined(MTK_AUDIO_CHANGE_SUPPORT) || defined(MTK_SUBTITLE_SUPPORT)
        if(track->GetLanguageAsUTF8()!=NULL)
            ALOGE("language = %s", track->GetLanguageAsUTF8());
#endif
#else
        const char *const codecID = track->GetCodecId();
        ALOGV("codec id = %s", codecID);
        ALOGV("codec name = %s", track->GetCodecNameAsUTF8());
#endif

        if (codecID == NULL) {
            ALOGW("unknown codecID is not supported.");
            continue;
        }

        size_t codecPrivateSize;
        const unsigned char *codecPrivate =
            track->GetCodecPrivate(codecPrivateSize);

#ifdef MTK_SUBTITLE_SUPPORT
            enum { VIDEO_TRACK = 1, AUDIO_TRACK = 2, SUBTT_TRACK = 17 };
#else
        enum { VIDEO_TRACK = 1, AUDIO_TRACK = 2 };
#endif

        sp<MetaData> meta = new MetaData;

#ifndef ANDROID_DEFAULT_CODE
    #if defined(MTK_AUDIO_CHANGE_SUPPORT) || defined(MTK_SUBTITLE_SUPPORT)
        if(track->GetLanguageAsUTF8()!=NULL){
            ALOGE("language = %s", track->GetLanguageAsUTF8());
            meta->setCString(kKeyMediaLanguage, track->GetLanguageAsUTF8());
        }
    #endif
#endif
        status_t err = OK;

        switch (track->GetType()) {
            case VIDEO_TRACK:
            {
                const mkvparser::VideoTrack *vtrack =
                    static_cast<const mkvparser::VideoTrack *>(track);

#ifdef MTK_AOSP_ENHANCEMENT
                long long width = vtrack->GetWidth();
                long long height = vtrack->GetHeight();
                meta->setInt32(kKeyWidth, width);
                meta->setInt32(kKeyHeight, height);
                ALOGD("video track width=%lld, height=%lld",width,height);
#endif
                if (!strcmp("V_MPEG4/ISO/AVC", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
#ifdef MTK_AOSP_ENHANCEMENT
                    ALOGI("Video Codec: AVC");
                    if (NULL == codecPrivate){
                        ALOGE("Unsupport AVC Video: No codec info");
                        mFileMetaData->setInt32(kKeyHasUnsupportVideo,true);
                        continue;
                    }

#endif
                    meta->setData(kKeyAVCC, 0, codecPrivate, codecPrivateSize);
#ifdef MTK_AOSP_ENHANCEMENT
                } else if ((!strcmp("V_MPEG4/ISO/ASP", codecID)) ||
                         (!strcmp("V_MPEG4/ISO/SP", codecID))) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
                    ALOGD("Video Codec: MPEG4");
                    if (codecPrivate != NULL)
                        meta->setData(kKeyMPEG4VOS, 0, codecPrivate, codecPrivateSize);
                    else {
                        ALOGW("No specific codec private data, find it from the first frame");
                        meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                    }
#else
                } else if (!strcmp("V_MPEG4/ISO/ASP", codecID)) {
                    if (codecPrivateSize > 0) {
                        meta->setCString(
                                kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
                        addESDSFromCodecPrivate(
                                meta, false, codecPrivate, codecPrivateSize);
                    } else {
                        ALOGW("%s is detected, but does not have configuration.",
                                codecID);
                        continue;
                    }
#endif
                } else if (!strcmp("V_VP8", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VP8);
#ifdef MTK_AOSP_ENHANCEMENT
                    ALOGI("Video Codec: VP8");
#endif
                } else if (!strcmp("V_VP9", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VP9);
#ifdef MTK_AOSP_ENHANCEMENT
                    ALOGI("Video Codec: VP9");
                } else if(!strcmp("V_MPEGH/ISO/HEVC", codecID)){
                        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_HEVC);
                        if (NULL == codecPrivate){
                            ALOGE("HEVC Video: No codec info, need make Codec info using the stream");
                            //mFileMetaData->setInt32(kKeyHasUnsupportVideo,true);          //continue;
                        }else {
                            meta->setData(kKeyHVCC, 0, codecPrivate, codecPrivateSize);
                            ALOGD("Video Codec: HEVC,codecPrivateSize =%zu", codecPrivateSize);
                        }
                }else if(!strcmp("V_MPEG4/MS/V3", codecID)){
                        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_DIVX3);
                        meta->setData(kKeyMPEG4VOS, 0, NULL, 0);

                }else if((!strcmp("V_MPEG2", codecID))||(!strcmp("V_MPEG1", codecID))){
                        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
                        if (codecPrivate != NULL)
                            addESDSFromCodecPrivate(meta, false, codecPrivate, codecPrivateSize);
                        else{
                            ALOGW("No specific codec private data, find it from the first frame");
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }
                }else if(!strcmp("V_MJPEG",codecID)){
                        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MJPEG);
                }else if (!strcmp("V_MS/VFW/FOURCC", codecID)) {
                    ALOGD("Video CodecID: V_MS/VFW/FOURCC");
                    if ((NULL == codecPrivate) || (codecPrivateSize < 20)) {
                        ALOGE("Unsupport video: V_MS/VFW/FOURCC has no invalid private data: \n");
                        ALOGE("codecPrivate=%p, codecPrivateSize=%zu", codecPrivate, codecPrivateSize);
                        mFileMetaData->setInt32(kKeyHasUnsupportVideo,true);
                        continue;
                    } else {
                        uint32_t fourcc = *(uint32_t *)(codecPrivate + 16);
                        //uint32_t j=0;
                        //for(j; j<codecPrivateSize; j++){
                        //  ALOGD("dump codecPrivate[%d]  %02x",j,*(uint8_t*)(codecPrivate+j));
                        //}
                        const char* mime = BMKVFourCC2MIME(fourcc);
                        ALOGD("V_MS/VFW/FOURCC type is %s", mime);
                        if (!strncasecmp("video/", mime, 6)) {
                            meta->setCString(kKeyMIMEType, mime);
                        } else {
                            ALOGE("V_MS/VFW/FOURCC continue,unsupport video type");
                            mFileMetaData->setInt32(kKeyHasUnsupportVideo,true);
                            continue;
                        }
                        if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_DIVX)){
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }else if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_DIVX3)){
                            meta->setData(kKeyMPEG4VOS, 0, codecPrivate, 0);
                            uint16_t divx3_width,divx3_height;
                            divx3_width= *(uint16_t *)(codecPrivate + 4);
                            divx3_height=*(uint16_t *)(codecPrivate + 8);
                            meta->setInt32(kKeyWidth, width);
                            meta->setInt32(kKeyHeight, height);
                            ALOGD("divx3_width=%d,divx3_height=%d",divx3_width,divx3_height);
                        }else if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_XVID)){
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }else if (!strcmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4)) {
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        } else if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG2)){
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }else if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)){
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }
                        else if(!strcmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)){
                            meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                        }

                    }
#endif
                } else {
                    ALOGW("%s is not supported.", codecID);
#ifdef MTK_AOSP_ENHANCEMENT
                    mFileMetaData->setInt32(kKeyHasUnsupportVideo,true);
#endif
                    continue;
                }

                meta->setInt32(kKeyWidth, vtrack->GetWidth());
                meta->setInt32(kKeyHeight, vtrack->GetHeight());
                break;
            }

            case AUDIO_TRACK:
            {
                const mkvparser::AudioTrack *atrack =
                    static_cast<const mkvparser::AudioTrack *>(track);

#ifdef MTK_AOSP_ENHANCEMENT
                if (!strncasecmp("A_AAC", codecID, 5)) {
                    unsigned char aacCodecInfo[2]={0, 0};
                    if (codecPrivateSize >= 2) {
                    } else if (NULL == codecPrivate) {

                        if (!strcasecmp("A_AAC", codecID)) {
                            ALOGW("Unspport AAC: No profile");
                            continue;
                        } else {
                            uint8_t freq_index=-1;
                            uint8_t profile;
                            if (!findAACSampleFreqIndex((uint32_t)atrack->GetSamplingRate(), freq_index)) {
                                ALOGE("Unsupport AAC freq");
                                continue;
                            }

                            if (!strcasecmp("A_AAC/MPEG4/LC", codecID) ||
                                !strcasecmp("A_AAC/MPEG2/LC", codecID))
                                profile = 2;
                            else if (!strcasecmp("A_AAC/MPEG4/LC/SBR", codecID) ||
                                !strcasecmp("A_AAC/MPEG2/LC/SBR", codecID))
                                profile = 5;
                            else if (!strcasecmp("A_AAC/MPEG4/LTP", codecID))
                                profile = 4;
                            else {
                                ALOGE("Unsupport AAC Codec profile %s", codecID);
                                continue;
                            }

                            codecPrivate = aacCodecInfo;
                            codecPrivateSize = 2;
                            aacCodecInfo[0] |= (profile << 3) & 0xf8;   // put it into the highest 5 bits
                            aacCodecInfo[0] |= ((freq_index >> 1) & 0x07);     // put 3 bits
                            aacCodecInfo[1] |= ((freq_index << 7) & 0x80); // put 1 bit
                            aacCodecInfo[1] |= ((unsigned char)atrack->GetChannels()<< 3);
                            ALOGD("Make codec info 0x%x, 0x%x", aacCodecInfo[0], aacCodecInfo[1]);
                        }
                    }else {
                        ALOGE("Incomplete AAC Codec Info %zu byte", codecPrivateSize);
                        continue;
                    }
                    addESDSFromCodecPrivate(meta, true, codecPrivate, codecPrivateSize);
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
                    ALOGD("Audio Codec: %s", codecID);
#else
                if (!strcmp("A_AAC", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
                    CHECK(codecPrivateSize >= 2);

                    addESDSFromCodecPrivate(
                            meta, true, codecPrivate, codecPrivateSize);
#endif
                } else if (!strcmp("A_VORBIS", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_VORBIS);

                    err = addVorbisCodecInfo(
                            meta, codecPrivate, codecPrivateSize);
#ifdef MTK_AOSP_ENHANCEMENT
                    ALOGD("Audio Codec: VORBIS,addVorbisCodecInfo return err=%d",err);
#endif
                } else if (!strcmp("A_OPUS", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_OPUS);
                    meta->setData(kKeyOpusHeader, 0, codecPrivate, codecPrivateSize);
                    meta->setInt64(kKeyOpusCodecDelay, track->GetCodecDelay());
                    meta->setInt64(kKeyOpusSeekPreRoll, track->GetSeekPreRoll());
                    mSeekPreRollNs = track->GetSeekPreRoll();
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_DDPLUS_SUPPORT
                } else if (!strcmp("A_AC3", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
                } else if (!strcmp("A_EAC3", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_EAC3);
#endif
#ifdef MTK_AUDIO_RAW_SUPPORT
                }else if(!strcmp("A_PCM/INT/LIT", codecID)){
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
                    meta->setInt32(kKeyEndian, 2);
                    meta->setInt32(kKeyBitWidth, atrack->GetBitDepth());
                    meta->setInt32(kKeyPCMType, 1);
                    if(atrack->GetBitDepth()==8)
                        meta->setInt32(kKeyNumericalType, 2);
                }
                else if(!strcmp("A_PCM/INT/BIG", codecID)){
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
                    meta->setInt32(kKeyEndian, 1);
                    meta->setInt32(kKeyBitWidth, atrack->GetBitDepth());
                    meta->setInt32(kKeyPCMType, 1);
                    if(atrack->GetBitDepth()==8)
                        meta->setInt32(kKeyNumericalType, 2);
#endif
                }else if (!strcmp("A_MPEG/L1", codecID)){
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
                    ALOGD("Audio Codec: MPEG/L1");
                    if (atrack->GetChannels() > 2) {
                        ALOGE("Unsupport MP1 Channel count=%lld", (long long)atrack->GetChannels());
                        continue;
                    }
                    if ((atrack->GetSamplingRate() < 0.1) || (atrack->GetChannels() == 0)){
                        meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                    }
                }else if (!strcmp("A_MPEG/L3", codecID)){
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
                    ALOGD("Audio Codec: MPEG");
                    if (atrack->GetChannels() > 2) {
                        ALOGE("Unsupport MP3 Channel count=%lld", (long long)atrack->GetChannels());
                        continue;
                    }
                    if ((atrack->GetSamplingRate() < 0.1) || (atrack->GetChannels() == 0)){
                        meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                    }
                }else if (!strcmp("A_MPEG/L2", codecID)){
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
                    ALOGD("Audio Codec: MPEG2");
                    if (atrack->GetChannels() > 2) {
                        ALOGE("Unsupport MP2 Channel count=%lld", (long long)atrack->GetChannels());
                        continue;
                    }
                    if ((atrack->GetSamplingRate() < 0.1) || (atrack->GetChannels() == 0)){
                        meta->setInt32(kKeyCodecInfoIsInFirstFrame,true);
                    }
                }else if ((!strcmp("A_MS/ACM", codecID))) {
                    if ((NULL == codecPrivate) || (codecPrivateSize < 8)) {
                        ALOGE("Unsupport audio: A_MS/ACM has no invalid private data: \n");
                        ALOGE("codecPrivate=%p, codecPrivateSize=%zu", codecPrivate, codecPrivateSize);
                        continue;
                    }else {
                        uint16_t ID = *(uint16_t *)codecPrivate;
                        const char* mime = MKVwave2MIME(ID);
                        ALOGD("A_MS/ACM type is %s", mime);
                        if (!strncasecmp("audio/", mime, 6)) {
                            meta->setCString(kKeyMIMEType, mime);
                        } else {
                            ALOGE("A_MS/ACM continue");
                            continue;
                        }
#if defined(MTK_AUDIO_ADPCM_SUPPORT) || defined(HAVE_ADPCMENCODE_FEATURE)
                        if((!strcmp(mime, MEDIA_MIMETYPE_AUDIO_DVI_IMA_ADPCM))||(!strcmp(mime, MEDIA_MIMETYPE_AUDIO_MS_ADPCM))){
                            uint32_t channel_count = *(uint16_t*)(codecPrivate+2);
                            uint32_t sample_rate = *(uint32_t*)(codecPrivate+4);
                            uint32_t BlockAlign= *(uint16_t*)(codecPrivate+12);
                            uint32_t BitesPerSample=*(uint16_t*)(codecPrivate+14);
                            uint32_t cbSize=*(uint16_t*)(codecPrivate+16);
                            ALOGD("channel_count=%u,sample_rate=%u,BlockAlign=%u,BitesPerSampe=%u,cbSize=%u",
                                channel_count,sample_rate,BlockAlign,BitesPerSample,cbSize);
                            meta->setInt32(kKeySampleRate, sample_rate);
                            meta->setInt32(kKeyChannelCount, channel_count);
                            meta->setInt32(kKeyBlockAlign, BlockAlign);
                            meta->setInt32(kKeyBitsPerSample, BitesPerSample);
                            meta->setData(kKeyExtraDataPointer, 0, codecPrivate+18, cbSize);
                        }
#endif
                        if(!strcmp(mime, MEDIA_MIMETYPE_AUDIO_WMA)){
                        meta->setData(kKeyWMAC, 0, codecPrivate, codecPrivateSize);
                        }
#ifdef MTK_AUDIO_RAW_SUPPORT
                        if(!strcmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)){
                        uint32_t BitesPerSample=*(uint16_t*)(codecPrivate+14);
                        meta->setInt32(kKeyBitWidth, BitesPerSample);
                        meta->setInt32(kKeyEndian, 2);
                        meta->setInt32(kKeyPCMType, 1);
                        if(BitesPerSample==8)
                            meta->setInt32(kKeyNumericalType, 2);
                        }
#endif
                    }
                }else {
                    ALOGW("%s is not supported.", codecID);
                    continue;
                }
#else
                } else if (!strcmp("A_MPEG/L3", codecID)) {
                    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
                } else {
                    ALOGW("%s is not supported.", codecID);
                    continue;
                }
#endif

                meta->setInt32(kKeySampleRate, atrack->GetSamplingRate());
                meta->setInt32(kKeyChannelCount, atrack->GetChannels());
#ifdef MTK_AOSP_ENHANCEMENT
                ALOGD("Samplerate=%f, channelcount=%lld", atrack->GetSamplingRate(), atrack->GetChannels());
                meta->setInt32(kKeyMaxInputSize, 32768);
                hasAudio = true;
#if defined(MTK_AUDIO_CHANGE_SUPPORT) || defined(MTK_SUBTITLE_SUPPORT)
                if(track->GetLanguageAsUTF8()!=NULL)
                    meta->setCString(kKeyMediaLanguage, track->GetLanguageAsUTF8());
#endif
#endif
                break;
            }

#ifdef MTK_SUBTITLE_SUPPORT
                case SUBTT_TRACK:
                {
                    //uint32_t j=0;
                    //for(j = 0; j<codecPrivateSize; j++)
                    //{
                        //ALOGE("dump SBS codecPrivate[%d]  %02x",j,*(uint8_t*)(codecPrivate+j));
                    //}
                    const char* mimeSubTT = BMapCodecId2SubTT(codecID);
                    meta->setCString(kKeyMIMEType, mimeSubTT);
                    meta->setData(kKeyTextFormatData, 0, codecPrivate, codecPrivateSize);
                    break;
                }
#endif
            default:
                continue;
        }

        if (err != OK) {
            ALOGE("skipping track, codec specific data was malformed.");
            continue;
        }

        long long durationNs = mSegment->GetDuration();
        meta->setInt64(kKeyDuration, (durationNs + 500) / 1000);

        mTracks.push();
        TrackInfo *trackInfo = &mTracks.editItemAt(mTracks.size() - 1);
        trackInfo->mTrackNum = track->GetNumber();
        trackInfo->mMeta = meta;
        trackInfo->mExtractor = this;
#ifdef MTK_AOSP_ENHANCEMENT
        trackInfo->mTrack = track;
        if (!hasVideo && hasAudio){ //mFileMetaData->setCString(kKeyMIMEType, "audio/x-matroska");
            mFileMetaData->setCString(kKeyMIMEType, mIsWebm ? "audio/webm" : "audio/x-matroska");
            ALOGI("MatroskaExtractor::addTracks,only audio,is %s",mIsWebm ? "audio/webm" : "audio/x-matroska");
        }else{          //mFileMetaData->setCString(kKeyMIMEType, "video/x-matroska");
            mFileMetaData->setCString(
                                kKeyMIMEType,
                                mIsWebm ? "video/webm" : "video/x-matroska");
            ALOGI("MatroskaExtractor::addTracks,has video and audio,is %s",mIsWebm ? "video/webm" : "video/x-matroska");

    }
#endif
    }
}

void MatroskaExtractor::findThumbnails() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        TrackInfo *info = &mTracks.editItemAt(i);

        const char *mime;
        CHECK(info->mMeta->findCString(kKeyMIMEType, &mime));

        if (strncasecmp(mime, "video/", 6)) {
            continue;
        }

        BlockIterator iter(this, info->mTrackNum, i);
        int32_t j = 0;
        int64_t thumbnailTimeUs = 0;
        size_t maxBlockSize = 0;
        while (!iter.eos() && j < 20) {
            if (iter.block()->IsKey()) {
                ++j;

                size_t blockSize = 0;
                for (int k = 0; k < iter.block()->GetFrameCount(); ++k) {
                    blockSize += iter.block()->GetFrame(k).len;
                }

                if (blockSize > maxBlockSize) {
                    maxBlockSize = blockSize;
                    thumbnailTimeUs = iter.blockTimeUs();
                }
            }
            iter.advance();
        }
        info->mMeta->setInt64(kKeyThumbnailTime, thumbnailTimeUs);
    }
}

sp<MetaData> MatroskaExtractor::getMetaData() {
#ifdef MTK_AOSP_ENHANCEMENT
    if(mFileMetaData != NULL){
        mFileMetaData->setCString(
            kKeyMIMEType,
            mIsWebm ? "video/webm" : "video/x-matroska");
        ALOGE("getMetaData , %s",mIsWebm ? "video/webm" : "video/x-matroska");
    }
    return mFileMetaData;
#else
    sp<MetaData> meta = new MetaData;

    meta->setCString(
            kKeyMIMEType,
            mIsWebm ? "video/webm" : MEDIA_MIMETYPE_CONTAINER_MATROSKA);

    return meta;
#endif
}

uint32_t MatroskaExtractor::flags() const {
    uint32_t x = CAN_PAUSE;
    if (!isLiveStreaming()) {
        x |= CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK;
    }

    return x;
}

bool SniffMatroska(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {
    DataSourceReader reader(source);
    mkvparser::EBMLHeader ebmlHeader;
    long long pos;
    if (ebmlHeader.Parse(&reader, pos) < 0) {
        return false;
    }

    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MATROSKA);
    *confidence = 0.6;

    return true;
}

#ifdef MTK_AOSP_ENHANCEMENT
//Need Refine: move mtk function to the end of file
status_t MatroskaSource::findMP3Header(uint32_t * header)
{
    if (header != NULL)
        *header = 0;

    uint32_t code = 0;
    while (0 == *header) {
        while (mPendingFrames.empty())
        {
            status_t err = readBlock();

            if (err != OK)
            {
                clearPendingFrames();
                return err;
            }
        }
        MediaBuffer *frame = *mPendingFrames.begin();
        size_t size = frame->range_length();
        size_t offset = frame->range_offset();
        size_t i;
        size_t frame_size;
        for (i=0; i<size; i++) {
            ALOGD("data[%zu]=%x", i, *((uint8_t*)frame->data()+offset+i));
            code = (code<<8) + *((uint8_t*)frame->data()+offset+i);
            if (get_mp3_info(code, &frame_size, NULL, NULL, NULL)) {
                *header = code;
                mBlockIter.reset();
                clearPendingFrames();
                return OK;
            }
        }
    }

    return ERROR_END_OF_STREAM;
}

//added by vend_am00033 start for seeking backward
void BlockIterator::backward()
{
    while ((mCluster != NULL) && (mCluster != &mExtractor->mSegment->m_eos))
    {
        if (mBlockEntry != NULL) {
            mBlockEntry = mCluster->GetPrev(mBlockEntry);
        } else if (mCluster != NULL) {
            mCluster = mExtractor->mSegment->GetPrev(mCluster);

            if (mCluster == &mExtractor->mSegment->m_eos) {
                break;
            }

        //mBlockEntry = mCluster->GetLast(mBlockEntry);
        const long status = mCluster->GetLast(mBlockEntry);
            if (status < 0){  //error
            ALOGE("get last blockenry failed!");
            mCluster=NULL;
                return ;
            }

        }

        if (mBlockEntry != NULL
                && mBlockEntry->GetBlock()->GetTrackNumber() == mTrackNum) {
            break;
        }

    }
}

bool BlockIterator::backward_eos(const mkvparser::Cluster* oldCluster, const mkvparser::BlockEntry* oldBlock)
{
    if (mCluster == &mExtractor->mSegment->m_eos)
    {
        //cannot seek I frame backward, so we seek I frame forward again
        mCluster = oldCluster;
        mBlockEntry = oldBlock;
        mBlockEntryIndex = oldBlock->GetIndex()+1;
        while (!eos() && (mTrackType != 2) && !block()->IsKey())
        {
            advance();
        }

        return true;
    }
    return false;
}
//added by vend_am00033 end

void BlockIterator::seekwithoutcue(int64_t seekTimeUs) {
    //    Mutex::Autolock autoLock(mExtractor->mLock);
    mCluster = mExtractor->mSegment->FindCluster(seekTimeUs * 1000ll);
    const long status = mCluster->GetFirst(mBlockEntry);
    if (status < 0){  //error
    ALOGE("get last blockenry failed!");
        mCluster=NULL;
        return ;
        }
    //    mBlockEntry = mCluster != NULL ? mCluster->GetFirst(mBlockEntry): NULL;
    //    mBlockEntry = NULL;
    mBlockEntryIndex = 0;

    //added by vend_am00033 start for seeking backward
    const mkvparser::Cluster* startCluster = mCluster;//cannot be null
    const mkvparser::Cluster* iframe_cluster = NULL;
    const mkvparser::BlockEntry* iframe_block = NULL;
    bool find_iframe = false;
    assert(startCluster != NULL);
    if (mBlockEntry)
    {
        if ((mTrackType != 2) && (block()->GetTrackNumber() == mTrackNum) && (block()->IsKey()))
        {
            find_iframe = true;
            iframe_cluster = mCluster;
            iframe_block = mBlockEntry;
        }
    }
    //added by vend_am00033 end
    while (!eos() && ((block()->GetTrackNumber() != mTrackNum) || (blockTimeUs() < seekTimeUs)))

    {
            advance_l();
    //added by vend_am00033 start for seeking backward
        if (mBlockEntry)
        {
            if ((mTrackType != 2) && (block()->GetTrackNumber() == mTrackNum) && (block()->IsKey()))
            {
                find_iframe = true;
                iframe_cluster = mCluster;
                iframe_block = mBlockEntry;
            }

        }
    //added by vend_am00033 end
    }

    //added by vend_am00033 start for seeking backward
    if (!eos() && (mTrackType != 2) && (!block()->IsKey()))
    {
        if (!find_iframe)
        {
            const mkvparser::Cluster* oldCluster = mCluster;
            const mkvparser::BlockEntry* oldBlock = mBlockEntry;
            mCluster = mExtractor->mSegment->GetPrev(startCluster);

            if (backward_eos(oldCluster, oldBlock))
                return;

            //mBlockEntry = mCluster != NULL ? mCluster->GetLast(mBlockEntry): NULL;
            const long status = mCluster->GetLast(mBlockEntry);
            if (status < 0){  //error
                    ALOGE("get last blockenry failed!");
                    mCluster=NULL;
                     return ;
                }

            while ((mCluster != &mExtractor->mSegment->m_eos) &&
                ((block()->GetTrackNumber() != mTrackNum) || (!block()->IsKey())))
            {
                backward();
            }
        if (mCluster == &mExtractor->mSegment->m_eos) {
            return;
        }
            mBlockEntryIndex=mBlockEntry->GetIndex()+1;
            if (backward_eos(oldCluster, oldBlock))
                return;

        }else{
            mCluster = iframe_cluster;
            mBlockEntry = iframe_block;
            mBlockEntryIndex = iframe_block->GetIndex()+1;
        }
    }

    //added by vend_am00033 end
    while (!eos() && !mBlockEntry->GetBlock()->IsKey() && (mTrackType != 2)/*Audio*/)//hai.li
    {
        advance_l();
       }
}

void MatroskaSource::setCodecInfoFromFirstFrame() {
    ALOGD("setCodecInfoFromFirstFrame");
    clearPendingFrames();
    int64_t actualFrameTimeUs;
        mBlockIter.seek(0, mIsAudio, &actualFrameTimeUs);
    //mBlockIter.seek(0);

    status_t err = readBlock();
    if (err != OK) {
        ALOGE("read codec info from first block fail!");
        mBlockIter.reset();
        clearPendingFrames();
        return;
    }
    if (mPendingFrames.empty()) {
        return;
    }
    if (MPEG4 == mType) {
        size_t vosend;
        for (vosend=0; (vosend<200) && (vosend<(*mPendingFrames.begin())->range_length()-4); vosend++)
        {
            if (0xB6010000 == *(uint32_t*)((uint8_t*)((*mPendingFrames.begin())->data()) + vosend))
            {
                break;//Send VOS until VOP
            }
        }
        getFormat()->setData(kKeyMPEG4VOS, 0, (*mPendingFrames.begin())->data(), vosend);
        //for (int32_t i=0; i<vosend; i++)
        //  ALOGD("VOS[%d] = 0x%x", i, *((uint8_t *)((*mPendingFrames.begin())->data())+i));
    }
    else if(MPEG2== mType){
        size_t header_start = 0;
        size_t header_lenth = 0;
        for (header_start=0; (header_start<200) && (header_start<(*mPendingFrames.begin())->range_length()-4); header_start++)
        {
            if (0xB3010000 == *(uint32_t*)((uint8_t*)((*mPendingFrames.begin())->data()) + header_start))
            {
                break;
            }
        }
        for (header_lenth=0; (header_lenth<200) && (header_lenth<(*mPendingFrames.begin())->range_length()-4-header_start); header_lenth++)
        {
            if (0xB8010000 == *(uint32_t*)((uint8_t*)((*mPendingFrames.begin())->data()) +header_start + header_lenth))
            {
                break;
            }
        }
        for (size_t i=0; i< header_lenth; i++)
            ALOGD("MPEG2info[%zu] = 0x%x", i, *((uint8_t *)((*mPendingFrames.begin())->data())+i+header_start));
            addESDSFromCodecPrivate(getFormat(), false,(uint8_t*)((*mPendingFrames.begin())->data())+header_start, header_lenth);
    }
    else if (MP2_3 == mType) {
        uint32_t header = *(uint32_t*)((uint8_t*)(*mPendingFrames.begin())->data()+(*mPendingFrames.begin())->range_offset());
        header = ((header >> 24) & 0xff) | ((header >> 8) & 0xff00) | ((header << 8) & 0xff0000) | ((header << 24) & 0xff000000);
        ALOGD("HEADER=0x%x", header);
        size_t frame_size;
        int32_t out_sampling_rate;
        int32_t out_channels;
        int32_t out_bitrate;
        if(!get_mp3_info(header, &frame_size, &out_sampling_rate, &out_channels, &out_bitrate))
        {
            ALOGE("Get mp3 info fail");
            return;
        }
        ALOGD("mp3: frame_size=%zu, sample_rate=%d, channel_count=%d, out_bitrate=%d",
                       frame_size, out_sampling_rate, out_channels, out_bitrate);
        if (out_channels > 2)
        {
            ALOGE("Unsupport mp3 channel count %d", out_channels);
            return;
        }
        getFormat()->setInt32(kKeySampleRate, out_sampling_rate);
        getFormat()->setInt32(kKeyChannelCount, out_channels);


    }

    mBlockIter.reset();
    clearPendingFrames();
}
#endif
}  // namespace android
