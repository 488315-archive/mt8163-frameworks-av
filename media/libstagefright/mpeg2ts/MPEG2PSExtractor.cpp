/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "MPEG2PSExtractor"
#include <utils/Log.h>

#include "include/MPEG2PSExtractor.h"

#include "AnotherPacketSource.h"
#include "ESQueue.h"

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>

#include <inttypes.h>

#ifdef MTK_AOSP_ENHANCEMENT
#include "include/avc_utils.h"
#include <sys/time.h>
#include "AnotherPacketSource.h"
#include <media/stagefright/MediaBuffer.h>
#define BUFFERING_THRESHOLD_MS 20
#define PARSE_MAX_PTS_LIMIT    100
#define SNIFF_LENGTH_900K 0xE1000
#define SNIFF_LENGTH_1K 0x400
#define PS_SNIFF_LENGTH SNIFF_LENGTH_1K
#endif

namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
static const size_t kChunkSize = 16384;
const static int64_t kMaxPTSTimeOutUs  = 3000000LL;
const static int64_t kMaxInitTimeOutUs =  300000LL;
#endif

struct MPEG2PSExtractor::Track : public MediaSource {
    Track(MPEG2PSExtractor *extractor,
          unsigned stream_id, unsigned stream_type);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

protected:
    virtual ~Track();

private:
    friend struct MPEG2PSExtractor;

    MPEG2PSExtractor *mExtractor;

    unsigned mStreamID;
    unsigned mStreamType;
    sp<AnotherPacketSource> mSource;
#ifdef MTK_AOSP_ENHANCEMENT
    bool seeking;
    int64_t mMaxTimeUs;
    bool mFirstPTSValid;
    uint64_t mFirstPTS;
    bool mSeekable;
    int64_t mTimeUsPrev;
    bool fgFirstPes;

    int64_t getPTS();
    bool isVideo();
    bool isAudio();
    int64_t convertPTSToTimestamp(uint64_t PTS);
    void signalDiscontinuity(const bool bKeepFormat = false);
#endif
    ElementaryStreamQueue *mQueue;
    status_t appendPESData(
            unsigned PTS_DTS_flags,
            uint64_t PTS, uint64_t DTS,
            const uint8_t *data, size_t size);

    DISALLOW_EVIL_CONSTRUCTORS(Track);

};

struct MPEG2PSExtractor::WrappedTrack : public MediaSource {
    WrappedTrack(const sp<MPEG2PSExtractor> &extractor, const sp<Track> &track);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

protected:
    virtual ~WrappedTrack();

private:
    sp<MPEG2PSExtractor> mExtractor;
    sp<MPEG2PSExtractor::Track> mTrack;

    DISALLOW_EVIL_CONSTRUCTORS(WrappedTrack);
};

////////////////////////////////////////////////////////////////////////////////

MPEG2PSExtractor::MPEG2PSExtractor(const sp<DataSource> &source)
    :
    #ifdef MTK_AOSP_ENHANCEMENT
      bisPlayable(true),
    #endif
      mDataSource(source),
      mOffset(0),
      mFinalResult(OK),
      mBuffer(new ABuffer(0)),
      mScanning(true),
    #ifdef MTK_AOSP_ENHANCEMENT
      mDurationUs(0),
      mSeekTimeUs(0),
      mSeeking(false),
      mSeekingOffset(0),
      mFileSize(0),
      mMinOffset(0),
      mMaxOffset(0),
      mMaxcount(0),
      mhasVTrack(false),
      mhasATrack(false),
      mValidESFrame(true),
      mSearchPTS(0),
      mAverageByteRate(0),
      mSystemHeaderValid(false),
      mParseMaxTime(false),
      mNeedDequeuePES(true),
    #endif
      mProgramStreamMapValid(false) {
    #ifdef MTK_AOSP_ENHANCEMENT
      init();
      if(countTracks()>0) {
        parseMaxPTS();
      }
      else {
        mDurationUs=0;
      }
      signalDiscontinuity(true);
      mOffset = 0;//Init Offset
      mSystemHeaderValid = false;
      mhasVTrack = false;
      mhasATrack = false;
      for (size_t index=0; index < mTracks.size(); index++) {
        if (mTracks.valueAt(index)->isVideo() && (mTracks.valueAt(index)->getFormat() != NULL)) {
            mhasVTrack = true;
        }
        else if (mTracks.valueAt(index)->isAudio() && (mTracks.valueAt(index)->getFormat() != NULL)) {
            mhasATrack = true;
        }
      }
    #else
      for (size_t i = 0; i < 500; ++i) {
        if (feedMore() != OK) {
            break;
        }
      }
    #endif

    // Remove all tracks that were unable to determine their format.
    for (size_t i = mTracks.size(); i-- > 0;) {
        if (mTracks.valueAt(i)->getFormat() == NULL) {
            mTracks.removeItemsAt(i);
        }
    }

    mScanning = false;
}

MPEG2PSExtractor::~MPEG2PSExtractor() {
}

size_t MPEG2PSExtractor::countTracks() {
    return mTracks.size();
}

sp<MediaSource> MPEG2PSExtractor::getTrack(size_t index) {
    if (index >= mTracks.size()) {
        return NULL;
    }
#ifdef MTK_AOSP_ENHANCEMENT
//    bool seekable = true;
    if (mTracks.size() > 1) {
        sp<MetaData> meta = mTracks.editValueAt(index)->getFormat();
        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!strncasecmp("audio/", mime, 6)) {
            mTracks.editValueAt(index)->mSeekable = false;
        }
    }
#endif
    return new WrappedTrack(this, mTracks.valueAt(index));
}

sp<MetaData> MPEG2PSExtractor::getTrackMetaData(
        size_t index, uint32_t /* flags */) {
    if (index >= mTracks.size()) {
        return NULL;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    sp<MetaData> meta = mTracks.editValueAt(index)->getFormat();
    meta->setInt64(kKeyDuration,  getDurationUs());
#endif
    return mTracks.valueAt(index)->getFormat();
}

sp<MetaData> MPEG2PSExtractor::getMetaData() {
    sp<MetaData> meta = new MetaData;
#ifdef MTK_AOSP_ENHANCEMENT
    mDataSource->getSize(&mFileSize);
    setDequeueState(false);
    mSystemHeaderValid = true;

    meta->setInt64(kKeyDuration,  getDurationUs());

    setDequeueState(true);
    mFinalResult = OK;
    mBuffer->setRange(0, 0);

    //Init Max PTS
    for (size_t index=0; index<mTracks.size(); index++) {
      mTracks.valueAt(index)->mMaxTimeUs = 0x0;
    }

    signalDiscontinuity(true);
    mOffset = 0;
    mSystemHeaderValid = false;

    //ALOGD("[get meta] mDurationUs:%lld, Track Number: %d ", mDurationUs, mTracks.size());
#endif
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MPEG2PS);

    return meta;
}

uint32_t MPEG2PSExtractor::flags() const {
#ifdef MTK_AOSP_ENHANCEMENT
    uint32_t flags = 0x0;
    flags = CAN_PAUSE | CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK;
    return flags;
#else
    return CAN_PAUSE;
#endif
}

status_t MPEG2PSExtractor::feedMore() {
    Mutex::Autolock autoLock(mLock);
#ifndef MTK_AOSP_ENHANCEMENT
    // How much data we're reading at a time
    static const size_t kChunkSize = 8192;
#else
    int64_t pts_prev;
    int64_t pts;

    int64_t ret = -1;
    if (mSeeking) {
        pts_prev = mSearchPTS;

        if((pts = mhasVTrack ? getMaxVideoPTS():getMaxPTS()) > 0) {
            mSearchPTS = pts;
        }

        //ALOGE("feedMore - Check Time Diff: %ld %ld ",pts/1000, mSeekTimeUs/1000);

        if (pts > 0) {
            mMaxcount++;
            if (((pts - mSeekTimeUs < 500000) && (pts - mSeekTimeUs > -500000)) ||     //Sync with TS Extractor
            mMinOffset == mMaxOffset ||
            mMaxcount > 13) {
                signalDiscontinuity();
                if( mhasVTrack == true ) {
                    //ALOGE("feedMore - Seek to Target: %lld %lld, mMaxcount %d  ", pts/1000, mSeekTimeUs/1000, mMaxcount);
                    //mBuffer->setRange(0, 0);    //Need to Reset mBuffer Data

                    //ALOGE("[MP2PS] AAAA feedMore - Seek to Target: %lld", mOffset);
                    ret = getLastPESWithIFrame(mOffset);  //MP2PS'
                    if(ret != -1 ) {
                        mOffset = ret;
                        //ALOGE("B feedMore - Seek to Target: %lld", mOffset);
                    }
                    else {
                        signalDiscontinuity();
                        ret = getNextPESWithIFrame(mOffset);
                        //ALOGE("mOffset wont be changed( %lld ) because findI returned -1", mOffset);

                        if(ret != -1 ) {
                            mOffset = ret;
                            //ALOGE("feedMore - Seek to Target: %lld", mOffset);
                        }
                        else {
                            //mOffset wont be changed
                            mOffset -= kChunkSize;/////////////////////////////////////////////////////////
                            signalDiscontinuity();
                            mOffset = SearchValidOffset(mOffset);
                            //ALOGE("[MP2PS]E mOffset wont be changed( %lld ) because findI returned -1", mOffset);
                        }
                    }
                }
                else {
                    mOffset = SearchValidOffset(mOffset);
                }

                //signalDiscontinuity();
                mBuffer->setRange(0, 0);    //Need to Reset mBuffer Data
                mSeeking = false;
                setDequeueState(true);
                mSearchPTS = 0;
            }
            else {
                signalDiscontinuity();
                updateSeekOffset(pts);
                mOffset = mSeekingOffset;
                mBuffer->setRange(0, 0);    //Need to Reset mBuffer Data
                mOffset = SearchValidOffset(mOffset);
            }
            ALOGD("pts=%lld, mSeekTimeUs=%lld, mMaxcount=%lld, mMinOffset=%lld, mMaxOffset=%lld, mSeekingOffset=%lld, mOffset = %lld ",
                (long long)pts, (long long)mSeekTimeUs, (long long)mMaxcount, (long long)mMinOffset, (long long)mMaxOffset, (long long)mSeekingOffset, (long long)mOffset);
        }
    }
#endif
    for (;;) {
        status_t err = dequeueChunk();

        if (err == -EAGAIN && mFinalResult == OK) {
            memmove(mBuffer->base(), mBuffer->data(), mBuffer->size());
            mBuffer->setRange(0, mBuffer->size());

            if (mBuffer->size() + kChunkSize > mBuffer->capacity()) {
            #ifdef MTK_AOSP_ENHANCEMENT
                size_t newCapacity = (mBuffer->capacity()==0)?kChunkSize:(mBuffer->capacity()<<1);// + kChunkSize;
                //ALOGD("Capacity %d->%d\n", mBuffer->capacity(), newCapacity);
            #else
                size_t newCapacity = mBuffer->capacity() + kChunkSize;
            #endif
                sp<ABuffer> newBuffer = new ABuffer(newCapacity);
                memcpy(newBuffer->data(), mBuffer->data(), mBuffer->size());
                newBuffer->setRange(0, mBuffer->size());
                mBuffer = newBuffer;
            }

            ssize_t n = mDataSource->readAt(
                    mOffset, mBuffer->data() + mBuffer->size(), kChunkSize);

            if (n < (ssize_t)kChunkSize) {
                mFinalResult = (n < 0) ? (status_t)n : ERROR_END_OF_STREAM;
                return mFinalResult;
            }

            mBuffer->setRange(mBuffer->offset(), mBuffer->size() + n);
            mOffset += n;

#ifdef MTK_AOSP_ENHANCEMENT
        }else if (false == mValidESFrame) {
                //ALOGE("feedMore::get a invalid ES ,now dequeue again!");
                mValidESFrame = true;
                continue;
#endif
        }else if (err != OK) {
            mFinalResult = err;
            return err;
        } else {
            return OK;
        }
    }
}

status_t MPEG2PSExtractor::dequeueChunk() {
    if (mBuffer->size() < 4) {
        return -EAGAIN;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    bool found=false;
    while (mBuffer->size() > 3) {
        if(memcmp("\x00\x00\x01", mBuffer->data(), 3)) {
            mBuffer->setRange(mBuffer->offset() + 1, mBuffer->size() - 1);
        }
        else {
            found = true;
            break;
        }
    }
    if(found == false) {    //go forward for following in file
        ALOGD("dequeueChunk found no 000001");
        return -EAGAIN;
    }
#else
    if (memcmp("\x00\x00\x01", mBuffer->data(), 3)) {
       return ERROR_MALFORMED;
    }
#endif

    unsigned chunkType = mBuffer->data()[3];

    ssize_t res;

    switch (chunkType) {
        case 0xba:
        {
            res = dequeuePack();
            break;
        }

        case 0xbb:
        {
            res = dequeueSystemHeader();
            break;
        }

        default:
        {
            res = dequeuePES();
            break;
        }
    }

    if (res > 0) {
        if (mBuffer->size() < (size_t)res) {
            return -EAGAIN;
        }

        mBuffer->setRange(mBuffer->offset() + res, mBuffer->size() - res);
        res = OK;
    }

    return res;
}

#ifdef MTK_AOSP_ENHANCEMENT

ssize_t MPEG2PSExtractor::dequeuePack() {
    // 32 + 2 + 3 + 1 + 15 + 1 + 15+ 1 + 9 + 1 + 22 + 1 + 1 | +5
    unsigned ConstantHdrLength = 14;

    if (mBuffer->size() < 12) {  //14->12
        return -EAGAIN;
    }

    if ((*(mBuffer->data()+4) >> 6) != 1) {
        ConstantHdrLength = 12;
        return ConstantHdrLength;
    }
    else {
        unsigned pack_stuffing_length = mBuffer->data()[13] & 7;
        return pack_stuffing_length + ConstantHdrLength;
    }
}
#else
ssize_t MPEG2PSExtractor::dequeuePack() {

    if (mBuffer->size() < 14) {
        return -EAGAIN;
    }

    unsigned pack_stuffing_length = mBuffer->data()[13] & 7;

    return pack_stuffing_length + 14;
}
#endif

ssize_t MPEG2PSExtractor::dequeueSystemHeader() {
    if (mBuffer->size() < 6) {
        return -EAGAIN;
    }

    unsigned header_length = U16_AT(mBuffer->data() + 4);
    #ifdef MTK_AOSP_ENHANCEMENT
    mSystemHeaderValid = true;
    #endif
    return header_length + 6;
}

#ifdef MTK_AOSP_ENHANCEMENT
int MPEG2PSExtractor::findNextPES(const void* data,int length)
{
    //Search Start Code & StreamID
    uint8_t* p = (uint8_t*)data;
    size_t index = 0;
    int off=0;
    unsigned stream_id_Video = 0xFF;
    unsigned stream_id_Audio = 0xFF;
    unsigned temp;
    if (length <= 6) {
        return -1;
    }
    for (index=0; index<mTracks.size(); index++) {
        if (mTracks.valueAt(index)->isVideo()) {
            stream_id_Video = mTracks.valueAt(index)->mStreamID;
        }
        if (mTracks.valueAt(index)->isAudio()) {
            stream_id_Audio = mTracks.valueAt(index)->mStreamID;
        }
    }

    while (off < length - 3) {
        temp = p[off+3];
        if( p[off] == 0x00 && p[off+1] == 0x00 && p[off+2] == 0x01 &&
            ((mhasVTrack == true && temp == stream_id_Video)||
            (mhasATrack == true && temp == stream_id_Audio)||
            temp == 0xb9 || temp == 0xbe || temp == 0xbc || temp == 0xbf ||
            temp == 0xf0 || temp == 0xf1 || temp == 0xff || temp == 0xf2 || temp == 0xf8)) {
            return off;
        }
        off++;
    }
  return -1;
}
///////////////////////////////
ssize_t MPEG2PSExtractor::dequeuePES() {
    if (mBuffer->size() < 6) {
        return -EAGAIN;
    }
    unsigned PTS_DTS_flags;
    uint64_t PTS = 0;
    uint64_t DTS = 0;
    unsigned dataLength;
    unsigned PES_packet_length = U16_AT(mBuffer->data() + 4);
    unsigned substid = 0;

    unsigned stream_id_tmp = U16_AT(mBuffer->data() + 2);
    if(stream_id_tmp == 0x01b9) {
        return 4;
    }
    while(PES_packet_length < 3 || stream_id_tmp < 0x01bc) {    //ALPS00440695 seek to 3:06,  file offset 0x7EAD2E9 has PES_packet_length = 1;.dat files have invalid stream_id
        ALOGD("Hdr Err PES_packet_length=%d, stream_id=0x%x\n", PES_packet_length, stream_id_tmp);
        int find = -1;
        //ALPS00433594
        mBuffer->setRange(mBuffer->offset()+3, mBuffer->size()-3); //ignore current PES because it's length==0

        find = findNextPES(mBuffer->data(),mBuffer->size());
        if(find > (int)(mBuffer->size()) || find < 0) {
            return -EAGAIN;
        }
        else {
            mBuffer->setRange(mBuffer->offset()+find, mBuffer->size()-find);
            PES_packet_length = U16_AT(mBuffer->data() + 4);
            stream_id_tmp = U16_AT(mBuffer->data() + 2);
        }

    }
    size_t n = PES_packet_length + 6;

    if (mBuffer->size() < n) {
        return -EAGAIN;
    }

    ABitReader br(mBuffer->data(), n);

    unsigned packet_startcode_prefix = br.getBits(24);

    ALOGV("packet_startcode_prefix = 0x%08x", packet_startcode_prefix);

    if (packet_startcode_prefix != 1) {
        ALOGD("Supposedly payload_unit_start=1 unit does not start "
             "with startcode.");

        return ERROR_MALFORMED;
    }

    unsigned stream_id = br.getBits(8);
    ALOGV("stream_id = 0x%02x, len %d\n", stream_id, PES_packet_length);

    /* unsigned PES_packet_length = */br.getBits(16);

    if (stream_id == 0xbc) {
        // program_stream_map

        if (!mScanning) {
            return n;
        }

        mStreamTypeByESID.clear();

        /* unsigned current_next_indicator = */br.getBits(1);
        /* unsigned reserved = */br.getBits(2);
        /* unsigned program_stream_map_version = */br.getBits(5);
        /* unsigned reserved = */br.getBits(7);
        /* unsigned marker_bit = */br.getBits(1);
        unsigned program_stream_info_length = br.getBits(16);

        size_t offset = 0;
        while (offset < program_stream_info_length) {
            if (offset + 2 > program_stream_info_length) {
                return ERROR_MALFORMED;
            }

            unsigned descriptor_tag = br.getBits(8);
            unsigned descriptor_length = br.getBits(8);

            ALOGI("found descriptor tag 0x%02x of length %u",
                 descriptor_tag, descriptor_length);

            if (offset + 2 + descriptor_length > program_stream_info_length) {
                return ERROR_MALFORMED;
            }

            br.skipBits(8 * descriptor_length);

            offset += 2 + descriptor_length;
        }

        unsigned elementary_stream_map_length = br.getBits(16);

        offset = 0;
        while (offset < elementary_stream_map_length) {
            if (offset + 4 > elementary_stream_map_length) {
                return ERROR_MALFORMED;
            }

            unsigned stream_type = br.getBits(8);
            unsigned elementary_stream_id = br.getBits(8);

            mStreamTypeByESID.add(elementary_stream_id, stream_type);

            unsigned elementary_stream_info_length = br.getBits(16);

            if (offset + 4 + elementary_stream_info_length
                    > elementary_stream_map_length) {
                //ALPS001495390. sometimes the element stream info length is invalid but it's not a serious error.
                //so we ignore it.
                ALOGE("elementary_stream_info_length may be wrong, but ignore it.");
            }

            offset += 4 + elementary_stream_info_length;
        }

        /* unsigned CRC32 = */br.getBits(32);

        mProgramStreamMapValid = true;
    } else if (stream_id != 0xbe  // padding_stream
            && stream_id != 0xbf  // private_stream_2
            && stream_id != 0xf0  // ECM
            && stream_id != 0xf1  // EMM
            && stream_id != 0xff  // program_stream_directory
            && stream_id != 0xf2  // DSMCC
            && stream_id != 0xf8
            && ((stream_id == 0xbd)||(stream_id >= 0xc0 && stream_id <= 0xdf) || (stream_id >= 0xe0 && stream_id <= 0xef))) {

        //check if PCM
        if(stream_id == 0xbd ) {
            //skip "000001BD" and packet length, 6 bytes, find the sub_stream_id
            substid = findSubStreamId(mBuffer->data()+6, mBuffer->size());
            // not between 0xA0 to 0xA7
            // 0x80 is AC3, need to appendData
            if( (substid < 0xA0 || substid > 0xA7 )
                &&  (substid != 0x80)
                ) {
                br.skipBits(PES_packet_length * 8);
                return n;
            }
        }

        unsigned next2bits = *(br.data())>>6;
        if(next2bits == 2u) {
            //MPEG2 Spec
            br.getBits(2);
            mMPEG1Flag = false;
            /* unsigned PES_scrambling_control = */br.getBits(2);
            /* unsigned PES_priority = */br.getBits(1);
            /* unsigned data_alignment_indicator = */br.getBits(1);
            /* unsigned copyright = */br.getBits(1);
            /* unsigned original_or_copy = */br.getBits(1);

            PTS_DTS_flags = br.getBits(2);
            ALOGV("PTS_DTS_flags = %u", PTS_DTS_flags);

            unsigned ESCR_flag = br.getBits(1);
            ALOGV("ESCR_flag = %u", ESCR_flag);

            unsigned ES_rate_flag = br.getBits(1);
            ALOGV("ES_rate_flag = %u", ES_rate_flag);

            unsigned DSM_trick_mode_flag = br.getBits(1);
            ALOGV("DSM_trick_mode_flag = %u", DSM_trick_mode_flag);

            unsigned additional_copy_info_flag = br.getBits(1);
            ALOGV("additional_copy_info_flag = %u", additional_copy_info_flag);

            /* unsigned PES_CRC_flag = */br.getBits(1);
            /* PES_extension_flag = */br.getBits(1);

            unsigned PES_header_data_length = br.getBits(8);
            ALOGV("PES_header_data_length = %u", PES_header_data_length);

            unsigned optional_bytes_remaining = PES_header_data_length;

            if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3) {
                br.getBits(4);
                PTS = ((uint64_t)br.getBits(3)) << 30;
                br.getBits(1);
                PTS |= ((uint64_t)br.getBits(15)) << 15;
                br.getBits(1);
                PTS |= br.getBits(15);
                br.getBits(1);


                optional_bytes_remaining -= 5;

                if (PTS_DTS_flags == 3) {
                    br.getBits(4);

                    DTS = ((uint64_t)br.getBits(3)) << 30;
                    br.getBits(1);
                    DTS |= ((uint64_t)br.getBits(15)) << 15;
                    br.getBits(1);
                    DTS |= br.getBits(15);
                    br.getBits(1);

                    //ALOGV("DTS = %lu", DTS);

                    optional_bytes_remaining -= 5;
                }
            }

            if (ESCR_flag) {

                br.getBits(2);

                uint64_t ESCR = ((uint64_t)br.getBits(3)) << 30;
                br.getBits(1);
                ESCR |= ((uint64_t)br.getBits(15)) << 15;
                br.getBits(1);
                ESCR |= br.getBits(15);
                br.getBits(1);

                ALOGV("ESCR = %llu", (unsigned long long)ESCR);
                /* unsigned ESCR_extension = */br.getBits(9);
                br.getBits(1);

                optional_bytes_remaining -= 6;
            }

            if (ES_rate_flag) {
                br.getBits(1);
                /* unsigned ES_rate = */br.getBits(22);
                br.getBits(1);
                optional_bytes_remaining -= 3;
            }

            br.skipBits(optional_bytes_remaining * 8);

            // ES data follows.

            dataLength = PES_packet_length - 3 - PES_header_data_length;

            if (br.numBitsLeft() < dataLength * 8) {
                ALOGE("PES packet does not carry enough data to contain "
                     "payload. (numBitsLeft = %lu, required = %lu)",
                     (unsigned long)br.numBitsLeft(), (unsigned long)dataLength * 8);

                return ERROR_MALFORMED;
            }

        }
        else {
            //MPEG1 Spec
            unsigned offset = 0;
            mMPEG1Flag = true;
            while(offset < 17 && *(br.data())== 0xff) {
             br.skipBits(8);
             offset++;
            }

            if(offset == 17) {
             ALOGD("*********************parsePES ERROR:too much MPEG-1 stuffing*********************");
             return offset;
            }

            next2bits = *(br.data())>>6;
            if(next2bits== 0x01) {
                br.getBits(2);
                /*unsigned STD_buffer_scale = */br.getBits(1);
                /*uint32_t STD_buffer_size = */br.getBits(13);
                offset += 2;
            }

            PTS_DTS_flags = *(br.data())>>4;

            if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3) {
                offset += 5;
                br.skipBits(4);
                PTS = ((uint64_t)br.getBits(3)) << 30;

                br.getBits(1);

                PTS |= ((uint64_t)br.getBits(15)) << 15;

                br.getBits(1);

                PTS |= br.getBits(15);

                br.getBits(1);

            if (PTS_DTS_flags == 3) {
                offset += 5;

                br.getBits(4);

                DTS = ((uint64_t)br.getBits(3)) << 30;

                br.getBits(1);

                DTS |= ((uint64_t)br.getBits(15)) << 15;

                br.getBits(1);

                DTS |= br.getBits(15);

                br.getBits(1);
            }
        }
        else {
            offset += 1;
            unsigned NO_PTSDTS_FFData = br.getBits(8) & 0xF;
            if (NO_PTSDTS_FFData != 0xF) {
                ALOGD("parsePES: Skip No PTS/DTS Error = %x \n", NO_PTSDTS_FFData);
            }
        }

        dataLength = PES_packet_length - offset;
    }

        if (PTS_DTS_flags != 2 && PTS_DTS_flags != 3) {
            //ALOGV("dequeuePES PTS_DTS_flags %d, PTS %llu, fileOffs 0x%x", PTS_DTS_flags, PTS/90, mOffset);
        }

        ssize_t index = mTracks.indexOfKey(stream_id);
        if (index < 0 && mScanning) {
            unsigned streamType;

            ssize_t streamTypeIndex;
            if (mProgramStreamMapValid && (streamTypeIndex = mStreamTypeByESID.indexOfKey(stream_id)) >= 0) {
                streamType = mStreamTypeByESID.valueAt(streamTypeIndex);
            } else if ((stream_id & ~0x1f) == 0xc0) {
                // ISO/IEC 13818-3 or ISO/IEC 11172-3 or ISO/IEC 13818-7
                // or ISO/IEC 14496-3 audio
                streamType = ATSParser::STREAMTYPE_MPEG2_AUDIO;
            }else if (stream_id == 0xbd) {
                // uncompressed auio, LPCM
                if(substid == 0x80 ) {
                    streamType = ATSParser::STREAMTYPE_AC3;//STREAMTYPE_AC3_AUDIO
                } else {
                    streamType = ATSParser::STREAMTYPE_AUDIO_PSLPCM;
                }
            }else if ((stream_id & ~0x0f) == 0xe0) {
                // ISO/IEC 13818-2 or ISO/IEC 11172-2 or ISO/IEC 14496-2 video
                streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;
            } else {
                streamType = ATSParser::STREAMTYPE_RESERVED;
            }

            //Add Check For Audio/Video Code When No Program Stream Map
            if (mProgramStreamMapValid == false) {
                if (streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO) {
                    //Video Part
                    unsigned StartCode[5];
                    StartCode[0] = *(br.data());
                    StartCode[1] = *(br.data()+1);
                    StartCode[2] = *(br.data()+2);
                    StartCode[3] = *(br.data()+3);
                    StartCode[4] = *(br.data()+4);

                    //MPEG2 or MPEG1
                    if ((StartCode[0] == 0x0) && (StartCode[1] == 0x0) && (StartCode[2] == 0x01) && (StartCode[3] == 0xB3)) {
                        //[MP2PS] add to distinct MPEG1 and MPEG2
                        if(mMPEG1Flag == true) {
                            streamType = ATSParser::STREAMTYPE_MPEG1_VIDEO;
                        } else {
                            streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;
                        }
                    } else if ((StartCode[0] == 0x0) && (StartCode[1] == 0x0) && (StartCode[2] == 0x00) && (StartCode[3] == 0x01) && (StartCode[4] == 0x67)) {
                        streamType = ATSParser::STREAMTYPE_H264;
                    } else if ((StartCode[0] == 0x0) && (StartCode[1] == 0x0) && (StartCode[2] == 0x01) && ((StartCode[3] == 0xB0))) {
                        streamType = ATSParser::STREAMTYPE_MPEG4_VIDEO;
                    } else {
                        //TODO
                        streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;

                        //try to sequentially search 128 bytes
                        uint8_t* paucTmpBuf = (uint8_t*)br.data();
                        int32_t idx, buf_size = ((br.numBitsLeft()>>3) > 128)?128:(br.numBitsLeft()>>3);
                        for(idx = 0;idx < buf_size-5; idx++) {
                            if (!memcmp("\x00\x00\x00\x01", &paucTmpBuf[idx], 4)) {
                                if(paucTmpBuf[idx + 4] == 0x67 || paucTmpBuf[idx + 4] == 0x6) {
                                    streamType = ATSParser::STREAMTYPE_H264;
                                    break;
                                }
                            } else if(!memcmp("\x00\x00\x01", &paucTmpBuf[idx], 3)) {
                                if(paucTmpBuf[idx + 3] == 0xB3) {
                                    //[MP2PS] add to distinct MPEG1 and MPEG2
                                    if(mMPEG1Flag == true) {
                                        streamType = ATSParser::STREAMTYPE_MPEG1_VIDEO;
                                    } else {
                                        streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;
                                    }
                                    break;
                                } else if(paucTmpBuf[idx + 3] == 0xB0) {
                                    streamType = ATSParser::STREAMTYPE_MPEG4_VIDEO;
                                    break;
                                } else if(paucTmpBuf[idx + 3] == 0x00 && paucTmpBuf[idx + 4] == 0x02) {
                                    streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;
                                    break;
                                } else if(paucTmpBuf[idx + 3] == 0x00 && paucTmpBuf[idx + 4] == 0x00) {
                                    streamType = ATSParser::STREAMTYPE_MPEG4_VIDEO;
                                    break;
                                }
                            }
                        }
                        ALOGD("V:No PMT and search type %d\n",streamType);

                    }
                } else if (streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO) {
                    //Audio Part
                    if (IsSeeminglyValidADTSHeader(br.data(), dataLength)) {
                        streamType = ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS;
                    } else if (IsSeeminglyValidMPEGAudioHeader(br.data(), dataLength)) {
                        streamType = ATSParser::STREAMTYPE_MPEG2_AUDIO;
                    } else {
                        //TODO
                        streamType = ATSParser::STREAMTYPE_RESERVED;
                        ALOGD("A:No PMT and startcode unknown\n");
                    }
                }
            }

            //mtk02420 2012/08/07
            //filter audiostream with more than 6 channels

            if( streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS || (streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO && IsSeeminglyValidADTSHeader(br.data(), dataLength) )){
                //AAC Audio
                /*peer the channel number for filttering
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
                unsigned ADTSChannelConfig[2];
                ADTSChannelConfig[0] = *(br.data()+2);
                ADTSChannelConfig[1] = *(br.data()+3);
                unsigned channel_configuration = ((ADTSChannelConfig[0]&0x01)<<2)+((ADTSChannelConfig[1]&0xC0)>>6);
                ALOGD("found AAC codec config %d channels)", channel_configuration);

                if( channel_configuration <= 2 ){
                    streamType = ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS;
                } else {
                    streamType = ATSParser::STREAMTYPE_RESERVED;
                }
            }

#ifdef MTK_AUDIO_CHANGE_SUPPORT
            if (streamType != ATSParser::STREAMTYPE_RESERVED) {
                ALOGD("found streamId Index=%d streamType:%02X", (int)index, streamType);
                if(streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO ||
                    streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO ||
                    streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO ||
                    streamType == ATSParser::STREAMTYPE_H264) {

                    mhasVTrack = true;
                    index = mTracks.add(stream_id, new Track(this, stream_id, streamType));
                    ALOGD("PES - add video track %02X,index=%d",stream_id,(int)index);
                }
                else if (streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS||
                    streamType == ATSParser::STREAMTYPE_MPEG1_AUDIO||
                    streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO||
                    streamType == ATSParser::STREAMTYPE_AUDIO_PSLPCM
                    || streamType == ATSParser::STREAMTYPE_AC3//STREAMTYPE_AC3_AUDIO
                     ) {
                    mhasATrack= true;
                    index = mTracks.add(stream_id, new Track(this, stream_id, streamType));
                    ALOGD("PES - add audio track %02X,index=%d",stream_id,(int)index);
                }
            }
#else
             if (streamType != ATSParser::STREAMTYPE_RESERVED) {
                 ALOGD("mhasATrack=%d mhasVTrack=%d",mhasATrack,mhasVTrack);
                 if(mhasVTrack == false &&
                     (streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO ||
                      streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO ||
                      streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO ||
                      streamType == ATSParser::STREAMTYPE_H264)){
                     index = mTracks.add(stream_id, new Track(this, stream_id, streamType));
                     mhasVTrack = true;
                     //ALOGD("PES - add video track %02X,index=%ld",stream_id,index);
                 }
                 else if( mhasATrack == false &&
                     (streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS||
                      streamType == ATSParser::STREAMTYPE_MPEG1_AUDIO||
                      streamType == ATSParser::STREAMTYPE_MPEG2_AUDIO ||
                      streamType == ATSParser::STREAMTYPE_AUDIO_PSLPCM
                      || streamType == ATSParser::STREAMTYPE_AC3//STREAMTYPE_AC3_AUDIO
                    )) {
                     index = mTracks.add(stream_id, new Track(this, stream_id, streamType));
                     mhasATrack= true;
                     //ALOGD("PES - add audio track %02X,index=%ld",stream_id,index);
                 }
             }
#endif
        }

        status_t err = OK;
        if (index >= 0) {
            //ALOGD("append %c, len %d, pts %lld, moffset %lld\n",
                //mTracks.editValueAt(index)->isVideo()?'V':'A', dataLength, PTS*100/9, mOffset);
            //AC3 need skip other 4 * 8 bits
            if((stream_id == 0xbd) && (substid == 0x80 )) {
                ALOGD("AC3, need move 32 bits");
                err =
                    mTracks.editValueAt(index)->appendPESData(
                        PTS_DTS_flags, PTS, DTS, br.data()+4, dataLength -4);

            }
            else {
                err =
                    mTracks.editValueAt(index)->appendPESData(
                        PTS_DTS_flags, PTS, DTS, br.data(), dataLength);
            }
        }
        br.skipBits(dataLength * 8);

#ifdef MTK_AOSP_ENHANCEMENT
        if (ERROR_INVALID_ES_FRAME == err) {//add by zyao
            mValidESFrame = false;
            ALOGE("dequeuePES::get a invalid ES ,set mValidESFrame false");
            return n;
        }
#endif
        if (err != OK) {
            return err;
        }
    } else if (stream_id == 0xbe) {  // padding_stream
        br.skipBits(PES_packet_length * 8);
    } else {
        br.skipBits(PES_packet_length * 8);
    }

    return n;
}

#else //#ifdef MTK_AOSP_ENHANCEMENT
ssize_t MPEG2PSExtractor::dequeuePES() {
    if (mBuffer->size() < 6) {
        return -EAGAIN;
    }

    unsigned PES_packet_length = U16_AT(mBuffer->data() + 4);
    CHECK_NE(PES_packet_length, 0u);

    size_t n = PES_packet_length + 6;

    if (mBuffer->size() < n) {
        return -EAGAIN;
    }

    ABitReader br(mBuffer->data(), n);

    unsigned packet_startcode_prefix = br.getBits(24);

    ALOGV("packet_startcode_prefix = 0x%08x", packet_startcode_prefix);

    if (packet_startcode_prefix != 1) {
        ALOGV("Supposedly payload_unit_start=1 unit does not start "
             "with startcode.");

        return ERROR_MALFORMED;
    }

    CHECK_EQ(packet_startcode_prefix, 0x000001u);

    unsigned stream_id = br.getBits(8);
    ALOGV("stream_id = 0x%02x", stream_id);

    /* unsigned PES_packet_length = */br.getBits(16);

    if (stream_id == 0xbc) {
        // program_stream_map

        if (!mScanning) {
            return n;
        }

        mStreamTypeByESID.clear();

        /* unsigned current_next_indicator = */br.getBits(1);
        /* unsigned reserved = */br.getBits(2);
        /* unsigned program_stream_map_version = */br.getBits(5);
        /* unsigned reserved = */br.getBits(7);
        /* unsigned marker_bit = */br.getBits(1);
        unsigned program_stream_info_length = br.getBits(16);

        size_t offset = 0;
        while (offset < program_stream_info_length) {
            if (offset + 2 > program_stream_info_length) {
                return ERROR_MALFORMED;
            }

            unsigned descriptor_tag = br.getBits(8);
            unsigned descriptor_length = br.getBits(8);

            ALOGI("found descriptor tag 0x%02x of length %u",
                 descriptor_tag, descriptor_length);

            if (offset + 2 + descriptor_length > program_stream_info_length) {
                return ERROR_MALFORMED;
            }

            br.skipBits(8 * descriptor_length);

            offset += 2 + descriptor_length;
        }

        unsigned elementary_stream_map_length = br.getBits(16);

        offset = 0;
        while (offset < elementary_stream_map_length) {
            if (offset + 4 > elementary_stream_map_length) {
                return ERROR_MALFORMED;
            }

            unsigned stream_type = br.getBits(8);
            unsigned elementary_stream_id = br.getBits(8);

            ALOGI("elementary stream id 0x%02x has stream type 0x%02x",
                 elementary_stream_id, stream_type);

            mStreamTypeByESID.add(elementary_stream_id, stream_type);

            unsigned elementary_stream_info_length = br.getBits(16);

            if (offset + 4 + elementary_stream_info_length
                    > elementary_stream_map_length) {
                return ERROR_MALFORMED;
            }

            offset += 4 + elementary_stream_info_length;
        }

        /* unsigned CRC32 = */br.getBits(32);

        mProgramStreamMapValid = true;
    } else if (stream_id != 0xbe  // padding_stream
            && stream_id != 0xbf  // private_stream_2
            && stream_id != 0xf0  // ECM
            && stream_id != 0xf1  // EMM
            && stream_id != 0xff  // program_stream_directory
            && stream_id != 0xf2  // DSMCC
            && stream_id != 0xf8) {  // H.222.1 type E
        CHECK_EQ(br.getBits(2), 2u);

        /* unsigned PES_scrambling_control = */br.getBits(2);
        /* unsigned PES_priority = */br.getBits(1);
        /* unsigned data_alignment_indicator = */br.getBits(1);
        /* unsigned copyright = */br.getBits(1);
        /* unsigned original_or_copy = */br.getBits(1);

        unsigned PTS_DTS_flags = br.getBits(2);
        ALOGV("PTS_DTS_flags = %u", PTS_DTS_flags);

        unsigned ESCR_flag = br.getBits(1);
        ALOGV("ESCR_flag = %u", ESCR_flag);

        unsigned ES_rate_flag = br.getBits(1);
        ALOGV("ES_rate_flag = %u", ES_rate_flag);

        unsigned DSM_trick_mode_flag = br.getBits(1);
        ALOGV("DSM_trick_mode_flag = %u", DSM_trick_mode_flag);

        unsigned additional_copy_info_flag = br.getBits(1);
        ALOGV("additional_copy_info_flag = %u", additional_copy_info_flag);

        /* unsigned PES_CRC_flag = */br.getBits(1);
        /* PES_extension_flag = */br.getBits(1);

        unsigned PES_header_data_length = br.getBits(8);
        ALOGV("PES_header_data_length = %u", PES_header_data_length);

        unsigned optional_bytes_remaining = PES_header_data_length;

        uint64_t PTS = 0, DTS = 0;

        if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3) {
            CHECK_GE(optional_bytes_remaining, 5u);

            CHECK_EQ(br.getBits(4), PTS_DTS_flags);

            PTS = ((uint64_t)br.getBits(3)) << 30;
            CHECK_EQ(br.getBits(1), 1u);
            PTS |= ((uint64_t)br.getBits(15)) << 15;
            CHECK_EQ(br.getBits(1), 1u);
            PTS |= br.getBits(15);
            CHECK_EQ(br.getBits(1), 1u);

            ALOGV("PTS = %llu", PTS);
            // ALOGI("PTS = %.2f secs", PTS / 90000.0f);

            optional_bytes_remaining -= 5;

            if (PTS_DTS_flags == 3) {
                CHECK_GE(optional_bytes_remaining, 5u);

                CHECK_EQ(br.getBits(4), 1u);

                DTS = ((uint64_t)br.getBits(3)) << 30;
                CHECK_EQ(br.getBits(1), 1u);
                DTS |= ((uint64_t)br.getBits(15)) << 15;
                CHECK_EQ(br.getBits(1), 1u);
                DTS |= br.getBits(15);
                CHECK_EQ(br.getBits(1), 1u);

                ALOGV("DTS = %llu", DTS);

                optional_bytes_remaining -= 5;
            }
        }

        if (ESCR_flag) {
            CHECK_GE(optional_bytes_remaining, 6u);

            br.getBits(2);

            uint64_t ESCR = ((uint64_t)br.getBits(3)) << 30;
            CHECK_EQ(br.getBits(1), 1u);
            ESCR |= ((uint64_t)br.getBits(15)) << 15;
            CHECK_EQ(br.getBits(1), 1u);
            ESCR |= br.getBits(15);
            CHECK_EQ(br.getBits(1), 1u);

            ALOGV("ESCR = %llu", ESCR);
            /* unsigned ESCR_extension = */br.getBits(9);

            CHECK_EQ(br.getBits(1), 1u);

            optional_bytes_remaining -= 6;
        }

        if (ES_rate_flag) {
            CHECK_GE(optional_bytes_remaining, 3u);

            CHECK_EQ(br.getBits(1), 1u);
            /* unsigned ES_rate = */br.getBits(22);
            CHECK_EQ(br.getBits(1), 1u);

            optional_bytes_remaining -= 3;
        }

        br.skipBits(optional_bytes_remaining * 8);

        // ES data follows.

        CHECK_GE(PES_packet_length, PES_header_data_length + 3);

        unsigned dataLength =
            PES_packet_length - 3 - PES_header_data_length;

        if (br.numBitsLeft() < dataLength * 8) {
            ALOGE("PES packet does not carry enough data to contain "
                 "payload. (numBitsLeft = %zu, required = %u)",
                 br.numBitsLeft(), dataLength * 8);

            return ERROR_MALFORMED;
        }

        CHECK_GE(br.numBitsLeft(), dataLength * 8);

        ssize_t index = mTracks.indexOfKey(stream_id);
        if (index < 0 && mScanning) {
            unsigned streamType;

            ssize_t streamTypeIndex;
            if (mProgramStreamMapValid
                    && (streamTypeIndex =
                            mStreamTypeByESID.indexOfKey(stream_id)) >= 0) {
                streamType = mStreamTypeByESID.valueAt(streamTypeIndex);
            } else if ((stream_id & ~0x1f) == 0xc0) {
                // ISO/IEC 13818-3 or ISO/IEC 11172-3 or ISO/IEC 13818-7
                // or ISO/IEC 14496-3 audio
                streamType = ATSParser::STREAMTYPE_MPEG2_AUDIO;
            } else if ((stream_id & ~0x0f) == 0xe0) {
                // ISO/IEC 13818-2 or ISO/IEC 11172-2 or ISO/IEC 14496-2 video
                streamType = ATSParser::STREAMTYPE_MPEG2_VIDEO;
            } else {
                streamType = ATSParser::STREAMTYPE_RESERVED;
            }

            index = mTracks.add(
                    stream_id, new Track(this, stream_id, streamType));
        }

        status_t err = OK;

        if (index >= 0) {
            err =
                mTracks.editValueAt(index)->appendPESData(
                    PTS_DTS_flags, PTS, DTS, br.data(), dataLength);
        }

        br.skipBits(dataLength * 8);

        if (err != OK) {
            return err;
        }
    } else if (stream_id == 0xbe) {  // padding_stream
        CHECK_NE(PES_packet_length, 0u);
        br.skipBits(PES_packet_length * 8);
    } else {
        CHECK_NE(PES_packet_length, 0u);
        br.skipBits(PES_packet_length * 8);
    }

    return n;
}
#endif //#ifdef MTK_AOSP_ENHANCEMENT

#ifdef MTK_AOSP_ENHANCEMENT
bool MPEG2PSExtractor::needRemoveData(sp<AnotherPacketSource> pSource, int64_t timeUs) {
    status_t finalResult;
    if (pSource->hasBufferAvailable(&finalResult)) {
       sp<AMessage> firstMeta = pSource->getLatestDequeuedMeta();
       if (firstMeta == NULL) {
           int64_t timeDuration = pSource->getEstimatedDurationUs();
           if (timeDuration > 2000000LL) {  //2s
               ALOGV("duration :%lld >2s", (long long)timeDuration);
               return true;
           }
           else {
               ALOGV("duration :%lld", (long long)timeDuration);
               return false;
           }
       }
       int64_t timeUs2;
       if ((firstMeta != NULL) && firstMeta->findInt64("timeUs", &timeUs2)) {
          if ((timeUs2 != -1) && (timeUs2 + 500000LL < timeUs)) {
              ALOGV("timeUS:%lld, timeUs2:%lld", (long long)timeUs, (long long)timeUs2);
              return true;
          }
       }
    }
    return false;
}

bool MPEG2PSExtractor::consumeData(sp<AnotherPacketSource> pSource, int64_t timeUs) {
    //ALOGV("consumeData+ Track Num:%d",mTracks.size());
    sp<MetaData> meta = pSource->getFormat();
    for (size_t index = 0; index < mTracks.size(); index++) {
        sp<Track> ptrack = mTracks.editValueAt(index);
        sp<AnotherPacketSource> source = ptrack->mSource;
        if (source != pSource) {
            sp<MetaData> metal = source->getFormat();
            if (metal == NULL) {
                ALOGD("Meta Data is Null");
                continue;
            }
            const char *mime;
            CHECK(metal->findCString(kKeyMIMEType, &mime));
            if (!strncasecmp("audio/", mime, 6)) {
                status_t finalResult;
                while (source->hasBufferAvailable(&finalResult)) {
                    if (needRemoveData(source, timeUs) == true) {
                        MediaBuffer *out;
                        MediaSource::ReadOptions options;
                        status_t errl = source->read(&out,&options);
                        if (errl == OK) {
                           int64_t removetimeUs;
                           MediaBuffer *buffer = out;
                           CHECK(buffer->meta_data()->findInt64(kKeyTime, &removetimeUs));
                           buffer->release();
                           //ALOGV("remove time:%lld,play time :%lld, index:%d, mime:%s", removetimeUs, timeUs, index, mime);
                        }
                        else {
                            //ALOGV("read err:%d",errl);
                        }
                     }
                     else {
                          ALOGV("no need remove");
                          break;
                     }
                }
            }
        }
    }
    ALOGV("consumeData--");
    return true;
}

void MPEG2PSExtractor::setDequeueState(bool needDequeuePES) {  //For Seek  //true: When Enable Seek, false: When Get Target Seek Time
    mNeedDequeuePES = needDequeuePES;
}

bool MPEG2PSExtractor::getDequeueState() {    //For Seek
    return mNeedDequeuePES;
}

//get duration
int64_t MPEG2PSExtractor::getMaxPTS() {     //For Seek
    int64_t maxPTS=0;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        int64_t pts = mTracks.editValueAt(i)->getPTS();
        if (maxPTS < pts) {
            maxPTS = pts;
        }
    }
    return maxPTS;
}


int64_t MPEG2PSExtractor::getMaxVideoPTS() {     //For Seek
    int64_t maxPTS=0;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        int64_t pts = mTracks.editValueAt(i)->getPTS();
        if (maxPTS < pts && mTracks.editValueAt(i)->isVideo()) {
        maxPTS = pts;
        }
    }
    return maxPTS;
}
void MPEG2PSExtractor::seekTo(int64_t seekTimeUs, unsigned StreamID) {
    Mutex::Autolock autoLock(mLock);

    ALOGE("seekTo:mDurationMs =%lld,seekTimeMs= %lld",(long long)mDurationUs/1000,(long long)seekTimeUs/1000);

    if (seekTimeUs == 0) {
        mOffset = 0;
        mSeeking = false;
        signalDiscontinuity(true);
    }
    else if((mDurationUs-seekTimeUs) < 200000)
    {
        mOffset = mFileSize;
        mSeeking = false;
        signalDiscontinuity(true);
    }
    else
    {
      /// Added by MTK40142@20140228, for ALPS01445625
        signalDiscontinuity(true);
        mSeekingOffset = mOffset;
        mSeekTimeUs=seekTimeUs;
        mMinOffset = 0;
        mMaxOffset = mFileSize;
        mMaxcount=0;
        setDequeueState(false);
        mSeeking=true;
        mSeekStreamID = StreamID;
    }
    mBuffer->setRange(0, 0);    //Need to Reset mBuffer Data
    mFinalResult = OK;    //Reset mFinalResult Status for Repeat Flow

    ALOGE("seekTo: moffset: %lld %lld ", (long long)mOffset, (long long)mMaxOffset);

    return;
}

void MPEG2PSExtractor::parseMaxPTS() {
    size_t index = 0;

    ALOGD("parseMaxPTS in \n");
    mDataSource->getSize(&mFileSize);
    ALOGD("File length :%lld ", (long long)mFileSize);
    setDequeueState(false);
    signalDiscontinuity();

    int64_t maxPTSStart = systemTime() / 1000;

    off64_t mOffsetSearch = mFileSize;
    for (off64_t i = 1; mOffsetSearch >= (off64_t)(kChunkSize/100); i++) {
        int64_t maxPTSDuration = systemTime()/1000 - maxPTSStart;
        if (maxPTSDuration > kMaxPTSTimeOutUs) {
            ALOGD("TimeOut find PTS, start time=%lld, duration=%lld", (long long)maxPTSStart, (long long)maxPTSDuration);
            break;
        }
        if (mOffsetSearch > (off64_t)(100 * i * kChunkSize)) {
            mOffsetSearch= (off64_t)(mOffsetSearch - 100 * i * kChunkSize);
        }
        else {
            mOffsetSearch = 0;
        }
        mOffset = SearchValidOffset(mOffsetSearch);
        ALOGD("Parse %lld times, Search offset %lld, Valid Offset: %lld, %2.2f%% of Total File", (long long)i, (long long)mOffsetSearch, (long long)mOffset, (float)(100 * mOffsetSearch / mFileSize));
        mFinalResult = OK;
        mBuffer->setRange(0, 0);
        while (feedMore() == OK) {

            int64_t maxPTSDuration = systemTime()/1000 - maxPTSStart;
            if (maxPTSDuration > kMaxPTSTimeOutUs) {
                ALOGD("TimeOut find PTS, start time=%lld, duration=%lld", (long long)maxPTSStart, (long long)maxPTSDuration);
                break;
            }

            if(((mOffset - mOffsetSearch) > (off64_t)(100 * kChunkSize)) && (0 == getMaxPTS()) ) {
                ALOGD("stop feedmore (no PES) mOffset=%lld  mOffsetSearch=%lld",(long long)mOffset,(long long)mOffsetSearch);
                break;
             }

             if((mOffset - mOffsetSearch) > (off64_t)(100 * i * kChunkSize)) {
                ALOGD("stop feedmore (enough data) mOffset=%lld  mOffsetSearch=%lld",(long long)mOffset,(long long)mOffsetSearch);
                break;
             }
        }
        mDurationUs = getMaxPTS();
        if (mDurationUs) {
            ALOGD("parseMaxPTS:mDurationUs=%lld, mOffset=%lld", (long long)mDurationUs, (long long)mOffset);
            break;
        }
    }

    setDequeueState(true);
    mFinalResult = OK;
    mBuffer->setRange(0, 0);

    //Init Max PTS
    for (index=0; index<mTracks.size(); index++) {
      mTracks.valueAt(index)->mMaxTimeUs = 0x0;
    }

    mAverageByteRate = (mDurationUs>0)?(mFileSize*1000000/mDurationUs):0;
    mParseMaxTime = true;
    ALOGD("getMaxPTS->mDurationUs:%lld, Track Number: %d, AverageByteRate %lld/s", (long long)mDurationUs, (int)mTracks.size(), (long long)mAverageByteRate);
    ALOGD("parseMaxPTS out \n");
}

uint64_t MPEG2PSExtractor::getDurationUs() {
    return mDurationUs;
}
void MPEG2PSExtractor::init() {
    bool haveAudio = false;
    bool haveVideo = false;
    int numPacketsParsed = 0;
    ALOGD("*************init in*********** \n");

    mOffset = 0;
    int64_t maxInitStart = systemTime() / 1000;
    while (feedMore() == OK)
    {
        size_t formatCount = 0;
        for (size_t index=0; index < mTracks.size(); index++)
        {
            if (mTracks.valueAt(index)->getFormat() != NULL) {
                formatCount ++;
            }

            if (mTracks.valueAt(index)->isVideo() &&
                (mTracks.valueAt(index)->getFormat() != NULL))
            {
                haveVideo = true;
                //ALOGV("haveVideo=%d", haveVideo);
            }
            else
            {
                if (mTracks.valueAt(index)->isVideo())
                {
                    ALOGV("have Video, But no format !! \n");
                }
            }

            if (mTracks.valueAt(index)->isAudio() &&
                (mTracks.valueAt(index)->getFormat() != NULL))
            {
                haveAudio = true;
                //ALOGV("haveAudio=%d", haveAudio);
            }
            else
            {
                if (mTracks.valueAt(index)->isAudio())
                {
                    ALOGV("have Audio, But no format !! \n");
                }
            }

        }

        if ((mTracks.size() != 0) && (mStreamTypeByESID.size() !=0) &&
            (mTracks.size() >= mStreamTypeByESID.size())            &&
            (formatCount == mTracks.size())) {
            //ALOGD("***init***:  mTracks size:%d, mStreamESID sie:%d",
                //mTracks.size(), mStreamTypeByESID.size());
            break;
        }

        int64_t maxInitDuration = systemTime()/1000 - maxInitStart;
        if ((maxInitDuration > kMaxInitTimeOutUs) || (numPacketsParsed>1500)) {
            ALOGD("Time out for init");
            break;
        }

        ++numPacketsParsed;
    }

    mFinalResult = OK;
    mBuffer->setRange(0, 0);
    if (haveAudio == 0 && haveVideo == 0) {
        ALOGD("bisplable is false");
        bisPlayable = false;
    }

    ALOGI("haveAudio=%d, haveVideo=%d, numPacketsParsed %d, mOffset %lld", (int)haveAudio, (int)haveVideo, (int)numPacketsParsed, (long long)mOffset);
    ALOGD("************ init out *****************\n");
}
bool MPEG2PSExtractor::getSeeking() {
    return mSeeking;
}

void MPEG2PSExtractor::signalDiscontinuity(const bool bKeepFormat)
{
  mBuffer->setRange(0, 0);

  for (size_t i = 0; i < mTracks.size(); ++i)
  {
    mTracks.valueAt(i)->signalDiscontinuity(bKeepFormat);
  }
}
////////// mtk 02420 refine seeking/////////////
int64_t MPEG2PSExtractor::getLastPESWithIFrame(off64_t end)
{
    unsigned stream_id_Video = 0xFF;
    int streamType = -1;
    bool mfindI = false;
    int mMaxTryCount = 0;
    //off64_t PESlength = 0;
    off64_t offset = end; //chunk start
    off64_t displace; //PES start(count from the chunk start)
    off64_t length = 0; //supposed to be the chunck size, unless it's not enought bits there
    off64_t lastPESoffset = -1;
    off64_t lastValidPES = -1;

    int bufsize = kChunkSize;
    char* buf = new char[bufsize];
    char* last5 = new char[5];
    size_t index = 0;

    //the state in this chunck helping memorize STATE mashine status
    bool hasPES = false;
    bool hasI = false;    //for H264 files, this variable stands for SPS
    bool hasH264I = false;    //for H264 files, this variable stands for I
    bool hasH264SPSnI = false;
    bool firstChunk = true;
    bool waitForTheLastPES = false;

    //int previousPESSCState = 0;
    int previousMPEGState = 0;
    int previousMPEG4State = 0;
    int previousH264State = 0;

    enum {
        NEXT_MPEG_INIT_STATE,
        NEXT_MPEG_FOUND00_STATE,
        NEXT_MPEG_FOUND0000_STATE,
        NEXT_MPEG_FOUND000001_STATE,
        NEXT_MPEG_FOUND00000100_STATE,
        NEXT_MPEG_FOUND00000100XX_STATE,
        NEXT_MPEG_FOUNDI_STATE,
    } MPEGNextSeekState;
    enum {
        NEXT_MPEG4_INIT_STATE,
        NEXT_MPEG4_FOUND00_STATE,
        NEXT_MPEG4_FOUND0000_STATE,
        NEXT_MPEG4_FOUND000001_STATE,
        NEXT_MPEG4_FOUND000001B6_STATE,
        NEXT_MPEG4_FOUNDI_STATE,
    } MPEG4NextSeekState;

    enum {
        NEXT_H264_INIT_STATE,
        NEXT_H264_FOUND00_STATE,
        NEXT_H264_FOUND0000_STATE,
        NEXT_H264_FOUND000001_STATE,
        //NEXT_H264_FOUND_SPS_STATE,
        NEXT_H264_FOUND_I_STATE,
        NEXT_H264_FOUND_SPSnI_STATE,
    } H264NextSeekState;

  for (index=0; index<mTracks.size(); index++)
  {
    if (mTracks.valueAt(index)->isVideo())
    {
      stream_id_Video = mTracks.valueAt(index)->mStreamID;
      streamType = mTracks.valueAt(index)->mStreamType;
    }
  }
    MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
    MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
    H264NextSeekState = NEXT_H264_INIT_STATE;

  //get bits from DataSource indexed by "offset"
    while(1){
        mMaxTryCount++;
        //ALOGD("[MP2PS]TryCount is %d, offset 0x%x\n",mMaxTryCount, offset);
            if(mMaxTryCount>50) {
                return -1;
        }

        if( offset >= 0 ) {
            if ( (length = mDataSource->readAt(offset, buf, bufsize)) <= 0){
                //ALOGD("read file error length=%lld",length);
                return 0;
            }
        }

        //initialization for a new chunk
        displace = 0;           //index of the chunk bytes
        lastPESoffset = -1;
        lastValidPES = -1;
        hasPES = false;
        hasI = false;
        hasH264I = false;
        hasH264SPSnI = false;
        waitForTheLastPES = false;



        previousMPEGState = MPEGNextSeekState;
        previousMPEG4State = MPEG4NextSeekState;
        previousH264State = H264NextSeekState;

        MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
        MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
        H264NextSeekState = NEXT_H264_INIT_STATE;
        //ALOGD("[MP2PS] new offset from file is %lld length is%lld result is %lld",offset,length,displace);

        //check edge data; peek 5
        //see if PES start code
        if(firstChunk == true){
          firstChunk = false;
        }
        else{
            if( ( (streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO) && previousMPEGState == NEXT_MPEG_FOUNDI_STATE )||
                ( streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO && previousMPEG4State == NEXT_MPEG4_FOUNDI_STATE )||
                ( streamType == ATSParser::STREAMTYPE_H264 && previousH264State == NEXT_H264_FOUND_SPSnI_STATE ) )
            {
              //ALOGD("check if PES on the edge is %02X %02X %02X %02X",buf[length-3],buf[length-2],buf[length-1] ,last5[0]);
              if( buf[length-3] == 0x00 && buf[length-2] == 0x00 && buf[length-1] == 0x01 && last5[0] == stream_id_Video)
              {
                lastPESoffset = offset + length  - 3;
                mfindI = true;
                break;
              }
              else if(buf[length-2] == 0x00 && buf[length-1] == 0x00 && last5[0] == 0x01 && last5[1] == stream_id_Video)
              {
                lastPESoffset = offset + length - 2;
                mfindI = true;
                break;

              }
              else if(buf[length-1] == 0x00 && last5[0] == 0x00 && last5[1] == 0x01 && last5[2] == stream_id_Video)
              {
                lastPESoffset = offset + length - 1;
                mfindI = true;
                break;
              }
            }

            if(streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO)  //MPEG
            {
                 if(previousMPEGState == NEXT_MPEG_FOUNDI_STATE){
                    waitForTheLastPES = true;
                 }
                 else if( (buf[length-5] == 0x00 && buf[length-4]== 0x00 && buf[length-3]== 0x01 && buf[length-2] == 0x00 && ((last5[0]>>3)&0x07) == 0x01) ||
                     (buf[length-4] == 0x00 && buf[length-3] == 0x00 && buf[length-2] == 0x01 && buf[length-1] == 0x00 && ((last5[1]>>3)&0x07) == 0x01)||
                     (buf[length-3] == 0x00 && buf[length-2] == 0x00 && buf[length-1] == 0x01 && last5[0] == 0x00 && ((last5[2]>>3)&0x07) == 0x01)||
                     (buf[length-2] == 0x00 && buf[length-1] == 0x00 && last5[0] == 0x01 && last5[1] == 0x00 && ((last5[3]>>3)&0x07) == 0x01)||
                     (buf[length-1] == 0x00 && last5[0] == 0x00 && last5[1] == 0x01 && last5[2] == 0x00 && ((last5[4]>>3)&0x07) == 0x01))
                 {
                    //ALOGD("mpeg I on the edge");
                    previousMPEGState = NEXT_MPEG_FOUNDI_STATE;
                    waitForTheLastPES = true;
                 }
             }
            else if(streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO)        //MPEG4
            {
                    if(previousMPEG4State == NEXT_MPEG4_FOUNDI_STATE){
                       waitForTheLastPES = true;
                    }
                    else if( (buf[length-4] == 0x00 && buf[length-3]== 0x00 && buf[length-2]== 0x01 && buf[length-1] == 0xB6 && (last5[0]&0xC0) == 0x00) ||
                        (buf[length-3] == 0x00 && buf[length-2] == 0x00 && buf[length-1] == 0x01 && last5[0] == 0xB6 && (last5[1]&0xC0) == 0x00)||
                        (buf[length-2] == 0x00 && buf[length-1] == 0x00 && last5[0] == 0x01 && last5[1] == 0xB6 && (last5[2]&0xC0) == 0x00)||
                        (buf[length-1] == 0x00 && last5[0] == 0x00 && last5[1] == 0x01 && last5[2] == 0xB6 && (last5[3]&0xC0) == 0x00))
                    {
                       //ALOGD("mpeg4 I on the edge");
                       previousMPEG4State = NEXT_MPEG4_FOUNDI_STATE;
                       waitForTheLastPES = true;
                    }

             }
            else if(streamType == ATSParser::STREAMTYPE_H264)       //H264
            {
                if(previousH264State == NEXT_H264_FOUND_SPSnI_STATE){
                   waitForTheLastPES = true;
                 }
                else if( previousH264State == NEXT_H264_FOUND_I_STATE ){
                    waitForTheLastPES = true;
                    if( (buf[length-3] == 0x00 && buf[length-2] == 0x00 && buf[length-1] == 0x01 &&(last5[0]& 0x1f) == 7)||
                        (buf[length-2] == 0x00 && buf[length-1] == 0x00 && last5[0] == 0x01 &&(last5[1]& 0x1f) == 7)||
                        (buf[length-1] == 0x00 && last5[0] == 0x00 && last5[1] == 0x01 && (last5[2]& 0x1f) == 7))
                     {
                        //ALOGD("h264 SPS on the edge");
                        previousH264State = NEXT_H264_FOUND_SPSnI_STATE;
                     }
                }
                else if( (buf[length-3] == 0x00 && buf[length-2] == 0x00 && buf[length-1] == 0x01 &&(last5[0]& 0x1f) == 5)||
                     (buf[length-2] == 0x00 && buf[length-1] == 0x00 && last5[0] == 0x01 &&(last5[1]& 0x1f) == 5)||
                     (buf[length-1] == 0x00 && last5[0] == 0x00 && last5[1] == 0x01 && (last5[2]& 0x1f) == 5))
                 {
                    //ALOGD("h264 SPS on the edge");
                    previousH264State = NEXT_H264_FOUND_I_STATE;
                    waitForTheLastPES = true;
                 }
             }
        }

        //ALOGD("[MP2PS] new offset from file is %lld length is%lld result is %lld",offset,length,displace);
        //ALOGD("[MP2PS] findI frame %d from %02X %02X %02X %02X %02X %02X %02X", mfindI, buf[0], buf[1],buf[2],buf[3],buf[4],buf[5],buf[6]);

        while(displace < length){    ////////////////////////////////////////////////////////////////////////////////////////
    //ALOGD("INNER LOOP %lld",displace);
            if( streamType == ATSParser::STREAMTYPE_H264)
            {
                  if(H264NextSeekState == NEXT_H264_INIT_STATE && buf[displace]!=0x00){
                     if( displace < (length - 9) && buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00 &&
                         buf[displace + 5] != 0x00 && buf[displace + 6] != 0x00 && buf[displace + 7] != 0x00 && buf[displace + 8] != 0x00 && buf[displace + 9] != 0x00) {   // speed-up; expecting picture data contains no 0x00
                        displace += 10;
                        continue;
                    }
                    displace++;
                    continue;
                  }
                   else if(buf[displace]==0x00){
                       if( H264NextSeekState == NEXT_H264_INIT_STATE){
                         H264NextSeekState= NEXT_H264_FOUND00_STATE;
                       }
                       else if(H264NextSeekState == NEXT_H264_FOUND00_STATE){
                         H264NextSeekState = NEXT_H264_FOUND0000_STATE;
                       }
                       else if(H264NextSeekState == NEXT_H264_FOUND0000_STATE){
                         H264NextSeekState = NEXT_H264_FOUND0000_STATE;
                       }
                       else{
                         H264NextSeekState = NEXT_H264_INIT_STATE;
                       }

                   }
                   else if(buf[displace]==0x01 && H264NextSeekState == NEXT_H264_FOUND0000_STATE ){
                        if(displace<length-1){
                           if(buf[displace+1] == stream_id_Video){
                               lastPESoffset = offset + displace -2; //+1-3=-2
                               //ALOGD("lastPESoffset is %lld",lastPESoffset);
                               hasPES = true;
                               H264NextSeekState = NEXT_H264_INIT_STATE;
                               displace += 2;
                               continue;
                           }

                           else if((buf[displace+1]& 0x1f) == 0x07){//SPS
                               hasI = true;
                               H264NextSeekState = NEXT_H264_INIT_STATE;
                               if( hasPES == true){
                                 //ALOGD("[L]find SPS validlastPES is %lld,displace is %lld", lastPESoffset,displace);
                                 lastValidPES = lastPESoffset;
                                 displace += 2;
                                 continue;
                                }
                               else {
                                  //hasI = true;
                                  lastValidPES = -1;
                                  //ALOGD("find I but no PES found");
                                 displace += 2;
                                 continue;
                               }
                           }

                           else if((buf[displace+1]& 0x1f) == 0x05){//IDR
                             hasH264I = true;
                             H264NextSeekState = NEXT_H264_INIT_STATE;

                             if( hasI == true){//SPS appeared before
                                 if(hasPES == true && lastValidPES != -1){
                                     //ALOGD("[L]find I break inner while lastPESoffset is %lld,displace is %lld", lastPESoffset,displace);
                                     mfindI = true;
                                     lastPESoffset = lastValidPES;
                                     if(waitForTheLastPES != true){
                                       break;
                                     }
                                   }
                                   else{
                                       //ALOGD("no PES found before SPS is found Valid:ast %lld",lastValidPES);
                                   }
                                   hasH264SPSnI = true;
                                   displace += 2;
                                   continue;
                             }
                             displace += 2;
                             continue;
                           }
                        }
                       H264NextSeekState = NEXT_H264_INIT_STATE;
                       displace += 2;
                       continue;
                   }
                   else{
                       H264NextSeekState = NEXT_H264_INIT_STATE;
                   }

            }
            else if( streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO)
            {
                if(MPEGNextSeekState == NEXT_MPEG_INIT_STATE && buf[displace]!=0x00){
                    if( displace < (length - 9) && buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00 &&
                        buf[displace + 5] != 0x00 && buf[displace + 6] != 0x00 && buf[displace + 7] != 0x00 && buf[displace + 8] != 0x00 && buf[displace + 9] != 0x00) {   // speed-up; expecting picture data contains no 0x00
                      displace += 10;
                      continue;
                    }
                    displace++;
                    continue;

                }
                else if(MPEGNextSeekState == NEXT_MPEG_FOUND00000100_STATE)
                {
                   MPEGNextSeekState = NEXT_MPEG_FOUND00000100XX_STATE;
                }
                else if(buf[displace] == 0x00){
                  if(MPEGNextSeekState == NEXT_MPEG_INIT_STATE){
                    MPEGNextSeekState = NEXT_MPEG_FOUND00_STATE;
                  }
                  else if(MPEGNextSeekState == NEXT_MPEG_FOUND00_STATE){
                    MPEGNextSeekState = NEXT_MPEG_FOUND0000_STATE;
                  }
                  else if(MPEGNextSeekState == NEXT_MPEG_FOUND0000_STATE){
                     MPEGNextSeekState = NEXT_MPEG_FOUND0000_STATE;
                  }
                  else if(MPEGNextSeekState == NEXT_MPEG_FOUND000001_STATE){
                     MPEGNextSeekState = NEXT_MPEG_FOUND00000100_STATE;
                  }
                  else{
                     MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                  }
                }
                else if( buf[displace] == 0x01 && MPEGNextSeekState == NEXT_MPEG_FOUND0000_STATE) {
                  MPEGNextSeekState = NEXT_MPEG_FOUND000001_STATE;
                }
                else if( buf[displace] == stream_id_Video && MPEGNextSeekState == NEXT_MPEG_FOUND000001_STATE){
                    lastPESoffset = offset + displace - 3;
                    //ALOGD("lastPESoffset is %lld",lastPESoffset);
                    hasPES = true;
                    MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                }
                else if(((buf[displace]>>3) & 0x07) == 0x01 && MPEGNextSeekState == NEXT_MPEG_FOUND00000100XX_STATE){
                   if( hasPES == true)
                   {
                     //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                     mfindI = true;
                     if(waitForTheLastPES != true)
                       break;
                   }
                   else
                   {
                     hasI = true;
                     MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                     //ALOGD("find I but no PES found");
                   }
                }
                else
                {
                  MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                }
            }
            else if( streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO)
            {
                if(MPEG4NextSeekState == NEXT_MPEG4_INIT_STATE && buf[displace]!=0x00){
                    if( displace < (length - 9) && buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00 &&
                        buf[displace + 5] != 0x00 && buf[displace + 6] != 0x00 && buf[displace + 7] != 0x00 && buf[displace + 8] != 0x00 && buf[displace + 9] != 0x00) {   // speed-up; expecting picture data contains no 0x00
                      displace += 10;
                      continue;
                    }
                    displace++;
                    continue;
                 }
                 else if(buf[displace] == 0x00){
                   if(MPEG4NextSeekState == NEXT_MPEG4_INIT_STATE){
                     MPEG4NextSeekState = NEXT_MPEG4_FOUND00_STATE;
                   }
                   else if(MPEG4NextSeekState == NEXT_MPEG4_FOUND00_STATE){
                     MPEG4NextSeekState = NEXT_MPEG4_FOUND0000_STATE;
                   }
                   else if(MPEG4NextSeekState == NEXT_MPEG4_FOUND0000_STATE){
                     MPEG4NextSeekState = NEXT_MPEG4_FOUND0000_STATE;
                   }
                   else{
                      MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                   }
                 }
                  else if( buf[displace] == 0x01 && MPEG4NextSeekState == NEXT_MPEG4_FOUND0000_STATE) {
                        if(displace<length-1){
                              if( buf[displace + 1] == stream_id_Video){
                                   lastPESoffset = offset + displace - 2;
                                   //ALOGD("lastPESoffset is %lld",lastPESoffset);
                                   hasPES = true;
                                   MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                              }
                              else if( buf[displace+1] == 0xB6){
                                  MPEG4NextSeekState = NEXT_MPEG4_FOUND000001B6_STATE;
                               }
                              else{
                                  MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                              }
                              displace+=2;
                              continue;
                        }
                        else{
                            MPEG4NextSeekState = NEXT_MPEG4_FOUND000001_STATE;
                            break;
                        }
                   }

                   else if( ((buf[displace] & 0xC0) == 0x00) && MPEG4NextSeekState == NEXT_MPEG4_FOUND000001B6_STATE){
                      if( hasPES == true)
                      {
                        //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                        mfindI = true;
                        break;
                      }
                      else
                      {
                          hasI = true;
                          MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                          //ALOGD("find I but no PES found");
                      }
                  }
                  else{
                       MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                  }
            }
            else
            {
              //ALOGE("Unknow stream type.");
              return -1;
            }
            //sum up the offset and return if something found
            displace ++;
        }

        if( mfindI == true )
        {
          //ALOGD("[L] find I break outter while");
          break;        //stop read from file
        }

        //sum up all previous results and up-to-date the current chunk so far
        if(streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO)
        {
          if(previousMPEGState == NEXT_MPEG_FOUNDI_STATE){
              if( hasPES == true )  //found PES previously in this chunk
              {
                mfindI = true;
                //ALOGD("last chunck had I this chunk has PES");
                break;
              }
              MPEGNextSeekState = NEXT_MPEG_FOUNDI_STATE;
          }
          else if( hasI == 1 ){
            MPEGNextSeekState = NEXT_MPEG_FOUNDI_STATE;
          }
          else{
            MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
          }
        }
        else if(streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO)  //found PES previously in this chunk
        {
          if(previousMPEG4State == NEXT_MPEG4_FOUNDI_STATE){
              if( hasPES == true )
              {
                mfindI = true;
                //ALOGD("last chunck had I this chunk has PES");
                break;
              }
              MPEG4NextSeekState = NEXT_MPEG4_FOUNDI_STATE;
          }
          else if( hasI == 1 ){     //has i but no pes
            MPEG4NextSeekState = NEXT_MPEG4_FOUNDI_STATE;
          }
          else{
            MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
          }
        }
        else if(streamType == ATSParser::STREAMTYPE_H264)   //found PES with SPS previously in this chunk
        {
            if(previousH264State == NEXT_H264_FOUND_SPSnI_STATE)
            {

                if( hasPES == true )
                {
                  mfindI = true;
                  //ALOGD("H264 last chunck had I and SPS this chunk has PES");
                  break;
                }
                H264NextSeekState = NEXT_H264_FOUND_SPSnI_STATE;
            }
            else if(previousH264State == NEXT_H264_FOUND_I_STATE)
            {

                if( hasPES == true && hasI == true && lastValidPES != -1)
                {
                  lastPESoffset = lastValidPES;
                  mfindI = true;
                  //ALOGD("H264 last chunck had I this chunk has PES");
                  break;
                }
                H264NextSeekState = NEXT_H264_FOUND_I_STATE;
            }
            else if( hasH264SPSnI == 1 ){
              H264NextSeekState = NEXT_H264_FOUND_SPSnI_STATE;
            }
            else if( hasH264I == 1 ){
              H264NextSeekState = NEXT_H264_FOUND_I_STATE;
            }

            else{
              H264NextSeekState = NEXT_H264_INIT_STATE;
            }

        }
        //reset
         last5[0]=buf[0];
         last5[1]=buf[1];
         last5[2]=buf[2];
         last5[3]=buf[3];
         last5[4]=buf[4];

        offset -= length;          //chunck start changes for the next read file start
        if(offset < 0)
            offset = 0;
    }

      if(buf != NULL)
      {
        delete[] buf;
        buf = NULL;
      }

      if(mfindI == true && lastPESoffset != -1)
      {
        //ALOGD("getLastPESWithIreturn offset %lld %lld", offset,displace);
        return lastPESoffset;
      }
      else
      {
        //ALOGD("NEXT find returns -1: MPEGSTATE is %d MPEG4STATE is %dH264STATE is %d", MPEGNextSeekState, MPEG4NextSeekState, H264NextSeekState);
        return -1;
      }
}
int64_t MPEG2PSExtractor::getNextPESWithIFrame(off64_t begin)
{
  unsigned stream_id_Video = 0xFF;
  int streamType = -1;
  bool mfindI = false;

//  off64_t PESlength = 0;
  off64_t offset = begin;   //chunk start
  off64_t displace;     //PES start(count from the chunk start)
  off64_t length = 0;   //supposed to be the chunck size, unless it's not enought bits there
  off64_t lastPESoffset = -1;
  off64_t lastValidPES = -1;

  int bufsize = kChunkSize;
  char* buf = new char[bufsize];
  char* last5 = new char[5];
  size_t index = 0;
  bool hasPES = false;
  bool hasPESnSPS = false;
  int mMaxTryCount = 0;
  bool firstChunk = true;

  enum {
      NEXT_MPEG_INIT_STATE,
      NEXT_MPEG_FOUND00_STATE,
      NEXT_MPEG_FOUND0000_STATE,
      NEXT_MPEG_FOUND000001_STATE,
      NEXT_MPEG_FOUND00000100_STATE,
      NEXT_MPEG_FOUND00000100XX_STATE,
      NEXT_MPEG_FOUNDI_STATE,
  } MPEGNextSeekState;
  enum {
    NEXT_MPEG4_INIT_STATE,
    NEXT_MPEG4_FOUND00_STATE,
    NEXT_MPEG4_FOUND0000_STATE,
    NEXT_MPEG4_FOUND000001_STATE,
    NEXT_MPEG4_FOUND000001B6_STATE,
    NEXT_MPEG4_FOUNDI_STATE,
  } MPEG4NextSeekState;

  enum {
    NEXT_H264_INIT_STATE,
    NEXT_H264_FOUND00_STATE,
    NEXT_H264_FOUND0000_STATE,
    NEXT_H264_FOUND000001_STATE,
    //NEXT_H264_FOUND_SPS_STATE,
    //NEXT_H264_FOUND_PESnSPS_STATE,
  } H264NextSeekState;

    H264NextSeekState = NEXT_H264_INIT_STATE;
    MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
    MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;

  for (index=0; index<mTracks.size(); index++)
  {
    if (mTracks.valueAt(index)->isVideo())
    {
      stream_id_Video = mTracks.valueAt(index)->mStreamID;
      streamType = mTracks.valueAt(index)->mStreamType;
    }
  }



//get bits from DataSource indexed by "offset"
  while(1)
  {
    mMaxTryCount++;
    //ALOGD("[MP2PS]TryCount is %d",mMaxTryCount);
    //marked for I far far away
	/*if(mMaxTryCount>=200){
		return -1;
	} */

    if( offset < mFileSize - 1 ){
      if((length = mDataSource->readAt(offset, buf, bufsize )) <= 0 )
        break;
    }
    else{
        break;
    }

    displace = 0;           //index of the chunk bytes

    //ALOGD("[MP2PS] new offset from file is %lld length is%lld result is %lld",offset,length,displace);
    //ALOGD("[MP2PS] findI frame %d from %02X %02X %02X %02X %02X %02X %02X", mfindI, buf[0], buf[1],buf[2],buf[3],buf[4],buf[5],buf[6]);


    if(firstChunk == true){
      firstChunk = false;
    }
    else{
      if(hasPES==true){
          if(streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO)  //MPEG
          {
               if( (last5[0] == 0x00 && last5[1]== 0x00 && last5[2]== 0x01 && last5[3] == 0x00 && ((buf[0]>>3)&0x07) == 0x01) ||
                   (last5[1] == 0x00 && last5[2] == 0x00 && last5[3] == 0x01 && last5[4] == 0x00 && ((buf[1]>>3)&0x07) == 0x01)||
                   (last5[2] == 0x00 && last5[3] == 0x00 && last5[4] == 0x01 && buf[0] == 0x00 && ((buf[2]>>3)&0x07) == 0x01)||
                   (last5[3] == 0x00 && last5[4] == 0x00 && buf[0] == 0x01 && buf[1] == 0x00 && ((buf[3]>>3)&0x07) == 0x01)||
                   (last5[4] == 0x00 && buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == 0x00 && ((buf[4]>>3)&0x07) == 0x01))
               {
                  //ALOGD("mpeg I on the edge");
                  mfindI =true;
                  break;
               }
           }
          else if(streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO)        //MPEG4
          {
              if( (last5[1] == 0x00 && last5[2]== 0x00 && last5[3]== 0x01 && last5[4] == 0xB6 && (buf[0]&0xC0) == 0x00) ||
                  (last5[2] == 0x00 && last5[3] == 0x00 && last5[4] == 0x01 && buf[0] == 0xB6 && (buf[1]&0xC0) == 0x00)||
                  (last5[3] == 0x00 && last5[4] == 0x00 && buf[0] == 0x01 && buf[1] == 0xB6 && (buf[2]&0xC0) == 0x00)||
                  (last5[4] == 0x00 && buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == 0xB6 && (buf[3]&0xC0) == 0x00))
              {
                 //ALOGD("mpeg4 I on the edge");
                 mfindI =true;
                 break;
              }
           }
          else if(streamType == ATSParser::STREAMTYPE_H264)       //H264
          {
               if( (last5[2] == 0x00 && last5[3] == 0x00 && last5[4] == 0x01 &&(buf[0]& 0x1f) == 7)||
                   (last5[3] == 0x00 && last5[4] == 0x00 && buf[0] == 0x01 &&(buf[1]& 0x1f) == 7)||
                   (last5[4] == 0x00 && buf[0] == 0x00 && buf[1] == 0x01 && (buf[2]& 0x1f) == 7))
               {
                  //ALOGD("h264 SPS on the edge");
                  lastValidPES = lastPESoffset;
                  hasPESnSPS= true;
               }
               else if( ((hasPESnSPS == true) && (lastValidPES != -1) &&
                   (last5[2] == 0x00 && last5[3] == 0x00 && last5[4] == 0x01 &&(buf[0]& 0x1f) == 5))||
                   (last5[3] == 0x00 && last5[4] == 0x00 && buf[0] == 0x01 &&(buf[1]& 0x1f) == 5)||
                   (last5[4] == 0x00 && buf[0] == 0x00 && buf[1] == 0x01 && (buf[2]& 0x1f) == 5))
               {
                  //ALOGD("h264 I on the edge");
                  lastPESoffset = lastValidPES;
                  mfindI =true;
                  break;
               }
          }
      }


      //check PES on edge
      if( last5[2] == 0x00 && last5[3] == 0x00 && last5[4] == 0x01 && buf[0] == stream_id_Video)
      {
        lastPESoffset = offset - 3;
        hasPES = true;
      }
      else if(last5[3] == 0x00 && last5[4] == 0x00 && buf[0] == 0x01 && buf[1] == stream_id_Video)
      {
         lastPESoffset = offset - 2;
         hasPES = true;
      }
      else if(last5[4] == 0x00 && buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == stream_id_Video)
      {
         lastPESoffset = offset - 1;
         hasPES = true;
      }
    }

    //parse the chunck indexed by "result"
    while(displace < length){
        if( streamType == ATSParser::STREAMTYPE_H264)
        {
            if(H264NextSeekState == NEXT_H264_INIT_STATE && buf[displace]!=0x00){
                H264NextSeekState = NEXT_H264_INIT_STATE;
                if( (displace < (length-4) )&& buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00) {   // speed-up; expecting picture data contains no 0x00
                  displace += 5;
                  continue;
                }
            }
            else if(buf[displace]==0x00){
                if( H264NextSeekState == NEXT_H264_INIT_STATE){
                  H264NextSeekState= NEXT_H264_FOUND00_STATE;
                }
                else if(H264NextSeekState == NEXT_H264_FOUND00_STATE){
                  H264NextSeekState = NEXT_H264_FOUND0000_STATE;
                }
                else if(H264NextSeekState == NEXT_H264_FOUND0000_STATE){
                  H264NextSeekState = NEXT_H264_FOUND0000_STATE;
                }
                else{
                  H264NextSeekState = NEXT_H264_INIT_STATE;
                }

            }
            else if(buf[displace]==0x01 && H264NextSeekState == NEXT_H264_FOUND0000_STATE ){
                if(displace<length-1){
                    if(buf[displace+1] == stream_id_Video){
                        lastPESoffset = offset + displace - 2;
                        //ALOGD("lastPESoffset is %lld",lastPESoffset);
                        hasPES = true;
                        H264NextSeekState = NEXT_H264_INIT_STATE;
                    }
                    else if((buf[displace+1]& 0x1f) == 0x07){
                        if( hasPES == true){
                          //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                          //H264NextSeekState = NEXT_H264_FOUND_PESnSPS_STATE;
                          hasPESnSPS = true;
                          lastValidPES = lastPESoffset;
                         }
                        else {
                           lastValidPES = -1;
                           H264NextSeekState = NEXT_H264_INIT_STATE;
                           //ALOGD("find I but no PES found");
                        }
                    }
                    else if((buf[displace+1]& 0x1f) == 0x05){       //IDR
                        if( hasPESnSPS == true && lastValidPES != -1){
                          //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                          lastPESoffset = lastValidPES;
                          mfindI = true;
                          break;
                         }
                        else {
                           H264NextSeekState = NEXT_H264_INIT_STATE;
                           //ALOGD("find I but no PES found");
                        }
                    }
                    else{
                        H264NextSeekState = NEXT_H264_INIT_STATE;
                    }
                    displace+=2;
                    continue;
                }
                else{
                  H264NextSeekState = NEXT_H264_FOUND000001_STATE;
                  break;
                }
            }
            else
                H264NextSeekState = NEXT_H264_INIT_STATE;
        }
        else if( streamType == ATSParser::STREAMTYPE_MPEG1_VIDEO || streamType == ATSParser::STREAMTYPE_MPEG2_VIDEO)
        {
            if(MPEGNextSeekState == NEXT_MPEG_INIT_STATE && buf[displace]!=0x00){
                 MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                 if( (displace < (length-4) )&& buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00) {   // speed-up; expecting picture data contains no 0x00
                   displace += 5;
                   continue;
                 }
            }
            else if(MPEGNextSeekState == NEXT_MPEG_FOUND00000100_STATE)
            {
               MPEGNextSeekState = NEXT_MPEG_FOUND00000100XX_STATE;
            }
            else if(buf[displace] == 0x00){
              if(MPEGNextSeekState == NEXT_MPEG_INIT_STATE){
                MPEGNextSeekState = NEXT_MPEG_FOUND00_STATE;
              }
              else if(MPEGNextSeekState == NEXT_MPEG_FOUND00_STATE){
                MPEGNextSeekState = NEXT_MPEG_FOUND0000_STATE;
              }
              else if(MPEGNextSeekState == NEXT_MPEG_FOUND0000_STATE){
                 MPEGNextSeekState = NEXT_MPEG_FOUND0000_STATE;
              }
              else if(MPEGNextSeekState == NEXT_MPEG_FOUND000001_STATE){
                 MPEGNextSeekState = NEXT_MPEG_FOUND00000100_STATE;
              }
              else{
                 MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
              }
            }
            else if( buf[displace] == 0x01 && MPEGNextSeekState == NEXT_MPEG_FOUND0000_STATE) {
              MPEGNextSeekState = NEXT_MPEG_FOUND000001_STATE;
            }
            else if( buf[displace] == stream_id_Video && MPEGNextSeekState == NEXT_MPEG_FOUND000001_STATE){
                lastPESoffset = offset + displace - 3;
                //ALOGD("lastPESoffset is %lld",lastPESoffset);
                hasPES = true;
                MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
            }
            else if(((buf[displace]>>3) & 0x07) == 0x01 && MPEGNextSeekState == NEXT_MPEG_FOUND00000100XX_STATE){
               if( hasPES == true)
               {
                 //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                 mfindI = true;
                 break;
               }
               else
               {
                 MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
                 //ALOGD("find I but no PES found");
               }
            }
            else
            {
              MPEGNextSeekState = NEXT_MPEG_INIT_STATE;
            }
        }
        else if( streamType == ATSParser::STREAMTYPE_MPEG4_VIDEO)
        {
            if(MPEG4NextSeekState == NEXT_MPEG4_INIT_STATE && buf[displace]!=0x00){
                MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                if( (displace < (length-4) )&& buf[displace + 1] != 0x00 && buf[displace + 2] != 0x00 && buf[displace + 3] != 0x00 && buf[displace + 4] != 0x00 ) {  // speed-up; expecting picture data contains no 0x00
                  displace += 5;
                  continue;
                }
             }
             else if(buf[displace] == 0x00){
               if(MPEG4NextSeekState == NEXT_MPEG4_INIT_STATE){
                 MPEG4NextSeekState = NEXT_MPEG4_FOUND00_STATE;
               }
               else if(MPEG4NextSeekState == NEXT_MPEG4_FOUND00_STATE){
                 MPEG4NextSeekState = NEXT_MPEG4_FOUND0000_STATE;
               }
               else if(MPEG4NextSeekState == NEXT_MPEG4_FOUND0000_STATE){
                 MPEG4NextSeekState = NEXT_MPEG4_FOUND0000_STATE;
               }
               else{
                  MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
               }
             }
             else if( buf[displace] == 0x01 && MPEG4NextSeekState == NEXT_MPEG4_FOUND0000_STATE) {
                if(displace<length-1){
                    if( buf[displace + 1] == stream_id_Video){
                         lastPESoffset = offset + displace - 2;
                         //ALOGD("lastPESoffset is %lld",lastPESoffset);
                         hasPES = true;
                         MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                    }
                    else if( buf[displace+1] == 0xB6){
                        MPEG4NextSeekState = NEXT_MPEG4_FOUND000001B6_STATE;
                     }
                    else{
                        MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                    }
                    displace+=2;
                    continue;
                }
                else{
                    MPEG4NextSeekState = NEXT_MPEG4_FOUND000001_STATE;
                    break;
                }
             }
             else if( ((buf[displace] & 0xC0) == 0x00) && MPEG4NextSeekState == NEXT_MPEG4_FOUND000001B6_STATE){
                if( hasPES == true)
                {
                  //ALOGD("[L]find I break inner while lastPESoffset is %lld", lastPESoffset);
                  mfindI = true;
                  break;
                }
                else
                {
                  MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
                  //ALOGD("find I but no PES found");
                }
             }
             else
             {
               MPEG4NextSeekState = NEXT_MPEG4_INIT_STATE;
             }
        }
        else
        {
          ALOGE("Unknow stream type.");
          return -1;
        }
        displace++;
    }


    if( mfindI == true )

    {
      //ALOGD("Nfind I break outter while");
      break;                    //stop read from file
    }

    last5[0] = buf[length - 1];
    last5[1] = buf[length - 2];
    last5[2] = buf[length - 3];
    last5[3] = buf[length - 4];
    last5[4] = buf[length - 5];

    offset += length;          //chunck start changes for the next read file start

  }

  if(buf != NULL)
  {
    delete[] buf;
    buf = NULL;
  }

  if(mfindI == true)
  {
    //ALOGD("getNextPESWithIreturn offset %lld %lld", offset,displace);
    return lastPESoffset;
  }
  else
  {
    //ALOGD("NEXT find returns -1: MPEGSTATE is %d MPEG4STATE is %dH264STATE is %d", MPEGNextSeekState, MPEG4NextSeekState, H264NextSeekState);
    return -1;
  }

}
void MPEG2PSExtractor::updateSeekOffset(int64_t pts) {
    //ALOGV("updateSeekOffset:now pts %lld",pts);
    if (1 == mMaxcount) {
        mSeekingOffset = mSeekTimeUs * mAverageByteRate/1000000;
        if (mSeekingOffset > mFileSize){
            mSeekingOffset = mFileSize;
        }
        else if (mSeekingOffset < 0){
            mSeekingOffset = 0;
        }
        //ALOGV("updateSeekOffset:mMaxcount==1,mSeekingOffset:%lld,mAverageByteRate:%lld",mSeekingOffset,mAverageByteRate);
        return ;
    }
    if ((pts < mSeekTimeUs) && (mSeekingOffset > mMinOffset)) {
        mMinOffset = mSeekingOffset;
        //ALOGV("updateSeekOffset:(pts < mSeekTimeUs) && (mSeekingOffset > mMinOffset),mMinOffset = mSeekingOffset,%lld",mSeekingOffset);
    }
    else if ((pts > mSeekTimeUs) && (mSeekingOffset < mMaxOffset)) {
        mMaxOffset = mSeekingOffset;
        //ALOGV("updateSeekOffset:(pts > mSeekTimeUs) && (mSeekingOffset < mMaxOffset),mMaxOffset = mSeekingOffset,%lld",mSeekingOffset);
    }

    if (mMaxOffset - mMinOffset < 100000) {
        mSeekingOffset = mMinOffset;
        //ALOGV("updateSeekOffset:mMaxOffset - mMinOffset < 100kB,mSeekingOffset = mMinOffset,%lld",mSeekingOffset);
    }
    else if (mMaxOffset == mFileSize || mMinOffset == 0) {
        //ALOGV("updateSeekOffset:mMaxOffset == mFileSize || mMinOffset == 0");
        mSeekingOffset += (mSeekTimeUs - pts) * mAverageByteRate /(1000000>>1);
        //ALOGV("mSeekingOffset:%lld,mSeekTimeUs:%lld,pts:%lld,mAverageByteRate:%lld",mSeekingOffset,mSeekTimeUs,pts,mAverageByteRate);
        if ((mSeekingOffset >= mFileSize) || (mSeekingOffset <= 0)) {
            if (mSeekingOffset >= mFileSize) {
                mMaxOffset = mFileSize-1;
                //ALOGV("updateSeekOffset:mSeekingOffset >= mFileSize");
            }
            if (mSeekingOffset <= 0) {
                mMinOffset = 1;
                //ALOGV("updateSeekOffset:mSeekingOffset <= 0");
            }
            mSeekingOffset = mFileSize >>1;
            //ALOGV("updateSeekOffset:mSeekingOffset %lld",mSeekingOffset);
        }
    }
    else {
        mSeekingOffset = (mMinOffset + mMaxOffset)>>1;
        //ALOGV("updateSeekOffset:mSeekingOffset %lld",mSeekingOffset);
    }
}
int64_t MPEG2PSExtractor::SearchPES(const void* data, int size)
{
  uint8_t* p = (uint8_t*)data;
  int offset = 0;
  unsigned stream_id_Video = 0xFF;
  unsigned stream_id_Audio = 0xFF;
  size_t index = 0;

  for (index=0; index<mTracks.size(); index++)
  {
       if (mTracks.valueAt(index)->isVideo())
       {
            stream_id_Video = mTracks.valueAt(index)->mStreamID;
       }
       if (mTracks.valueAt(index)->isAudio())
       {
            stream_id_Audio = mTracks.valueAt(index)->mStreamID;
       }
  }

  while(offset < size - 4)
  {
    //find start code
    if(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01 &&
        ((mhasVTrack == true && p[3] == stream_id_Video)||(mhasATrack == true && p[3] == stream_id_Audio)))
    {
      return offset;
    }
    else
    {
      p++;
      offset++;
    }
  }

  return -1;
}
int64_t MPEG2PSExtractor::SearchValidOffset(off64_t currentoffset)
{
  //Search Start Code & StreamID
  int length = 0;
  int bufsize = kChunkSize;
  char* buf = new char[bufsize];
  off64_t offset = currentoffset;
  bool found=false;

  //ALOGE("feedMore - Search Start = %lld \n", offset);
  if (buf == NULL)
  {
    ALOGE("Working Alloc Fail for Seek\n");
  }

  while((length = mDataSource->readAt(offset, buf, bufsize)) == bufsize)
  {
    int64_t result = SearchPES(buf, length);

    if (result >= 0)
    {
      offset = offset + result;
      found = true;
      break;
    }
    else
    {
      offset = offset + length;
    }
  }

  if(buf != NULL)
  {
    free(buf);
    buf = NULL;
  }
  if(found == true)
    return offset;
  else
    return -1;
}
bool MPEG2PSExtractor::IsSeeminglyValidADTSHeader(const uint8_t *ptr, size_t size) {
    if (size < 3) {
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

    return true;
}

bool MPEG2PSExtractor::IsSeeminglyValidMPEGAudioHeader(const uint8_t *ptr, size_t size) {
    if (size < 3) {
        // Not enough data to verify header.
        return false;
    }

    /*if (ptr[0] != 0xff || (ptr[1] >> 5) != 0x07) {
        return false;
    }*/

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
unsigned MPEG2PSExtractor::findSubStreamId(const uint8_t *data, const size_t size)
{
    if (size < 3) {
        return 0;
    }

    unsigned substid = 0;
    unsigned offset = 0;
    unsigned PTS_DTS_flags = 0;

    if ((*data) >> 6 == 2u) { //MPEG2 spec
        unsigned header_length = *(data+2);
        offset += 2+header_length+1;
        substid = *(data+offset);
        //ALOGV("mpeg 2, offset: %d, header len: %d, subid: 0x%x", offset, header_length, substid);
    } else { //MPEG1 spec
        PTS_DTS_flags = (*data >> 4) & 0x3;
        if (PTS_DTS_flags == 2u) {
            offset += 5;
        } else if (PTS_DTS_flags == 3u) {
            offset += 10;
        } else {
            offset += 1;
        }
        //ALOGV("mpeg 1, pts_dts_flag: %d, offset: %d, subid: 0x%x", PTS_DTS_flags, offset, substid);
        substid = *(data+offset);
    }

    return substid;
}
#endif
////////////////////////////////////////////////////////////////////////////////

MPEG2PSExtractor::Track::Track(
        MPEG2PSExtractor *extractor, unsigned stream_id, unsigned stream_type)
    : mExtractor(extractor),
      mStreamID(stream_id),
      mStreamType(stream_type),
      #ifdef MTK_AOSP_ENHANCEMENT
      seeking(false),
      mMaxTimeUs(0),
      mFirstPTSValid(false),
      mSeekable(true),    //Default: Seekable. Will Change in getTrack() When Video/Audio Case (Disable Audio Seek)
      mTimeUsPrev(0),
      fgFirstPes(true),
      #endif
      mQueue(NULL) {
    bool supported = true;
    ElementaryStreamQueue::Mode mode;

    switch (mStreamType) {
        case ATSParser::STREAMTYPE_H264:
            mode = ElementaryStreamQueue::H264;
            break;
        case ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS:
            mode = ElementaryStreamQueue::AAC;
            break;
        case ATSParser::STREAMTYPE_MPEG1_AUDIO:
        case ATSParser::STREAMTYPE_MPEG2_AUDIO:
            mode = ElementaryStreamQueue::MPEG_AUDIO;
            break;

        case ATSParser::STREAMTYPE_MPEG1_VIDEO:
        case ATSParser::STREAMTYPE_MPEG2_VIDEO:
            mode = ElementaryStreamQueue::MPEG_VIDEO;
            break;

        case ATSParser::STREAMTYPE_MPEG4_VIDEO:
            mode = ElementaryStreamQueue::MPEG4_VIDEO;
            break;
#ifdef MTK_AOSP_ENHANCEMENT
        case ATSParser::STREAMTYPE_AUDIO_PSLPCM:
            mode = ElementaryStreamQueue::PSLPCM;
            break;
#endif
        case ATSParser::STREAMTYPE_AC3:
            mode = ElementaryStreamQueue::AC3;
            break;
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_AUDIO_DDPLUS_SUPPORT)
        case ATSParser::STREAMTYPE_EC3:
            mode = ElementaryStreamQueue::EC3;
            break;
#endif // DOLBY_END
        default:
            supported = false;
            break;
    }

    if (supported) {
        mQueue = new ElementaryStreamQueue(mode);
#if defined(MTK_AOSP_ENHANCEMENT) && defined(MTK_OGM_PLAYBACK_SUPPORT)
        if(mode == ElementaryStreamQueue::MPEG_VIDEO)
        {
            mQueue->setSearchSCOptimize(true);
        }
#endif
    } else {
        ALOGI("unsupported stream ID 0x%02x", stream_id);
    }
}

MPEG2PSExtractor::Track::~Track() {
    delete mQueue;
    mQueue = NULL;
}

status_t MPEG2PSExtractor::Track::start(MetaData *params) {
    if (mSource == NULL) {
        return NO_INIT;
    }

    return mSource->start(params);
}

status_t MPEG2PSExtractor::Track::stop() {
    if (mSource == NULL) {
        return NO_INIT;
    }

    return mSource->stop();
}

sp<MetaData> MPEG2PSExtractor::Track::getFormat() {
    if (mSource == NULL) {
        return NULL;
    }

    return mSource->getFormat();
}
#ifdef MTK_AOSP_ENHANCEMENT
status_t MPEG2PSExtractor::Track::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    if (mSource == NULL) {
        return NO_INIT;
    }

    int64_t seekTimeUs;
    ReadOptions::SeekMode seekMode;

    if (mSeekable && options && options->getSeekTo(&seekTimeUs, &seekMode)) {
      mExtractor->seekTo(seekTimeUs, mStreamID);
    }
    int64_t ReadtimeUs = 0;
    bool    ReadtimeUsSt = false;
    if (!mExtractor->getSeeking() && isAudio()) {
        ReadtimeUs = ALooper::GetNowUs();
        ReadtimeUsSt = true;
    }

    status_t err = OK;
    status_t finalResult = OK;
    while (!mSource->hasBufferAvailable(&finalResult) || mExtractor->getSeeking())
    {
        if (ReadtimeUsSt && (ALooper::GetNowUs() - ReadtimeUs > 400000ll)
            && isAudio() && !mExtractor->getSeeking()) {
            ALOGD("Time out for track read this=%p", this);
            ReadtimeUsSt = false;
            mExtractor->setDequeueState(true);
            mSource->clear(true);
            return ERROR_END_OF_STREAM;
        }
        if (finalResult != OK) {
             ALOGE("read:ERROR_END_OF_STREAM this=%p",this );
             mExtractor->setDequeueState(true);
             mSource->clear(true);
             return ERROR_END_OF_STREAM;
        }

        err = mExtractor->feedMore();

        if (err != OK) {
            ALOGE("read:signalEOS this=%p",this );
            mSource->signalEOS(err);
        }
    }
    status_t ret;
    ret = mSource->read(buffer, options);
    if (ret == OK) {
        const char *mime;
        sp<MetaData> meta = mSource->getFormat();
        CHECK(meta->findCString(kKeyMIMEType, &mime));
        if (!strncasecmp("audio/", mime, 6)) {
            MediaBuffer *out = *buffer;
            int64_t timeUs;
            CHECK(out->meta_data()->findInt64(kKeyTime, &timeUs));
            if (timeUs >= 0) {
               mExtractor->consumeData(mSource, timeUs);
            }
        }
    }
    return ret;
}
#else
status_t MPEG2PSExtractor::Track::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    if (mSource == NULL) {
        return NO_INIT;
    }

    status_t finalResult;
    while (!mSource->hasBufferAvailable(&finalResult)) {
        if (finalResult != OK) {
            return ERROR_END_OF_STREAM;
        }

        status_t err = mExtractor->feedMore();

        if (err != OK) {
            mSource->signalEOS(err);
        }
    }

    return mSource->read(buffer, options);
}
#endif

status_t MPEG2PSExtractor::Track::appendPESData(
        unsigned PTS_DTS_flags,
        uint64_t PTS, uint64_t /* DTS */,
        const uint8_t *data, size_t size) {
    if (mQueue == NULL) {
        return OK;
    }

    int64_t timeUs;
    if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3) {
#ifdef MTK_AOSP_ENHANCEMENT
        timeUs = convertPTSToTimestamp(PTS);
        //ALOGD("PTS US: %lld, ID: %x \n", timeUs, mStreamID);
        mTimeUsPrev = timeUs;
#else
        timeUs = (PTS * 100) / 9;
#endif

    }else {
#ifdef MTK_AOSP_ENHANCEMENT
        timeUs = mTimeUsPrev;//ALPS00433420, decoder will not modify PTS value, use 0 or mTimeUsPrev are able to display;
#else
        timeUs = 0;
#endif
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if ((timeUs > mMaxTimeUs) && (timeUs != 0xFFFFFFFF) && (timeUs != 0))
    {
      mMaxTimeUs = timeUs;
    }
    if (!mExtractor->getDequeueState())
    {
      return OK;
    }

    bool fgInvalidPTS = false;
    if (PTS_DTS_flags == 0) {
        /*if can not get pts, let the second frame to valid, video codec could be pull pts*/
        if (!fgFirstPes) {
            fgInvalidPTS = true;
        }
    }
    //status_t err = mQueue->appendData(data, size, timeUs, fgInvalidPTS);
    status_t err = mQueue->appendData(data, size, timeUs);
    fgFirstPes = false;
#else
    status_t err = mQueue->appendData(data, size, timeUs);
#endif

    if (err != OK) {
        return err;
    }

    sp<ABuffer> accessUnit;
    while ((accessUnit = mQueue->dequeueAccessUnit()) != NULL) {
        if (mSource == NULL) {
            sp<MetaData> meta = mQueue->getFormat();

            if (meta != NULL) {
                ALOGD("Stream ID 0x%02x now has data.", mStreamID);

                mSource = new AnotherPacketSource(meta);
                mSource->queueAccessUnit(accessUnit);
            }
        } else if (mQueue->getFormat() != NULL) {
            mSource->queueAccessUnit(accessUnit);
        }
    }

    return OK;
}
#ifdef MTK_AOSP_ENHANCEMENT
int64_t MPEG2PSExtractor::Track::getPTS() {
  return mMaxTimeUs;
}

bool MPEG2PSExtractor::Track::isVideo(){
    switch (mStreamType) {
        case ATSParser::STREAMTYPE_H264:
        case ATSParser::STREAMTYPE_MPEG1_VIDEO:
        case ATSParser::STREAMTYPE_MPEG2_VIDEO:
        case ATSParser::STREAMTYPE_MPEG4_VIDEO:
            return true;

        default:
            return false;
    }
}

bool MPEG2PSExtractor::Track::isAudio(){
    switch (mStreamType) {
        case ATSParser::STREAMTYPE_MPEG1_AUDIO:
        case ATSParser::STREAMTYPE_MPEG2_AUDIO:
        case ATSParser::STREAMTYPE_MPEG2_AUDIO_ADTS:
        case ATSParser::STREAMTYPE_AUDIO_PSLPCM:
        case ATSParser::STREAMTYPE_AC3://STREAMTYPE_AC3_AUDIO

            return true;

        default:
            return false;
    }
}

int64_t MPEG2PSExtractor::Track::convertPTSToTimestamp(uint64_t PTS) {
  if (!mFirstPTSValid)
  {
    mFirstPTSValid = true;
    mFirstPTS = PTS;
    PTS = 0;
  }
  else if (PTS < mFirstPTS)
  {
    PTS = 0;
  }
  else
  {
    PTS -= mFirstPTS;
  }

  return (PTS * 100) / 9;
}

void MPEG2PSExtractor::Track::signalDiscontinuity(const bool bKeepFormat)
{
    mTimeUsPrev = 0;

    if (mQueue == NULL) {
        return;
    }

    if (!mExtractor->getDequeueState())
    {
      mMaxTimeUs = 0;
      return;
    }

    mQueue->clear(false);

    mQueue->setSeeking();

    if(mSource.get()) {
        mSource->clear(bKeepFormat);
    }
    else {
        ALOGE("[error]this stream has no source\n");
    }

    return;
}
#endif

////////////////////////////////////////////////////////////////////////////////

MPEG2PSExtractor::WrappedTrack::WrappedTrack(
        const sp<MPEG2PSExtractor> &extractor, const sp<Track> &track)
    : mExtractor(extractor),
      mTrack(track) {
}

MPEG2PSExtractor::WrappedTrack::~WrappedTrack() {
}

status_t MPEG2PSExtractor::WrappedTrack::start(MetaData *params) {
    return mTrack->start(params);
}

status_t MPEG2PSExtractor::WrappedTrack::stop() {
    return mTrack->stop();
}

sp<MetaData> MPEG2PSExtractor::WrappedTrack::getFormat() {
#ifdef MTK_AOSP_ENHANCEMENT
    sp<MetaData> meta = mTrack->getFormat();

    int64_t durationUs;
    if (!meta->findInt64(kKeyDuration, &durationUs)) {
        meta->setInt64(kKeyDuration, mExtractor->getDurationUs());//Need to Enable Seek Feature
    }
    meta->setInt64(kKeyThumbnailTime,0);                         //Need to Enable Seek Feature
    meta->setPointer('anps', (mTrack->mSource).get());
    return meta;
#else
    return mTrack->getFormat();
#endif
}

status_t MPEG2PSExtractor::WrappedTrack::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    return mTrack->read(buffer, options);
}
////////////////////////////////////////////////////////////////////////////////

#ifdef MTK_AOSP_ENHANCEMENT

#define  PACK_START_CODE              0X000001BA
#define  SYSTEM_START_CODE          0X000001BB

int  parsePackHeader(ABitReader * br)
{
    int currentByte = -1;
    int length = -1;
    enum {
        INIT_STATE,
        FIND00_STATE,
        FIND0000_STATE,
        FIND000001_STATE,
        FIND000001BA_STATE,

    } currentState;
    currentState = INIT_STATE;
   //search for pack header start code
   while (1) {
      length = br->numBitsLeft()/8;
      //ALOGD("*********************parsePackHeader in :length = %d*********************",length);
      if(length < 9)
      {
          ALOGD("data too less");
          return 0;
      }

      currentByte = br->getBits(8);

      switch(currentState)
      {
        case INIT_STATE:
        {
            if(length < 12)             //can't be less than 12 byte( including startcode)
            {
              ALOGD("********************* parsePackHeader can't find pack header start code *********************");
              return 0;
            }
            if(currentByte == 0x00)
            {
                currentState = FIND00_STATE;
                if(length < 11)           //can't be less than 12 byte( including startcode)
                {
                  ALOGD("********************* parsePackHeader can't find pack header start code *********************");
                  return 0;
                }
            }
            else
            {
                currentState = INIT_STATE;  //reset
            }
            break;
        }
        case FIND00_STATE:
        {
            if(length < 11)             //can't be less than 12 byte( including startcode)
            {
              ALOGD("********************* parsePackHeader can't find pack header start code *********************");
              return 0;
            }

            if(currentByte == 0x00)
            {
                currentState = FIND0000_STATE;
                if(length < 10)             //can't be less than 12 byte( including startcode)
                {
                  ALOGD("********************* parsePackHeader can't find pack header start code *********************");
                  return 0;
                }
            }
            else
            {
                currentState = INIT_STATE;      //reset
            }
            break;
        }
        case FIND0000_STATE:
        {
            if(length < 10)             //can't be less than 12 byte( including startcode)
            {
              ALOGD("********************* parsePackHeader can't find pack header start code *********************");
              return 0;
            }

            if(currentByte == 0x01)
            {
                currentState = FIND000001_STATE;
                if(length < 9)                    //can't be less than 12 byte( including startcode)
                {
                  ALOGD("********************* parsePackHeader can't find pack header start code *********************");
                  return 0;
                }
            }
            else if(currentByte == 0x00)
            {
                currentState = FIND0000_STATE;
                if(length < 10)             //can't be less than 12 byte( including startcode)
                {
                  ALOGD("********************* parsePackHeader can't find pack header start code *********************");
                  return 0;
                }
            }
            else
            {
                currentState = INIT_STATE;  //reset
            }
            break;
        }
        case FIND000001_STATE:
        {
            if(length < 9)          //can't be less than 12 byte( including startcode)
            {
              ALOGD("********************* parsePackHeader can't find pack header start code *********************");
              return 0;
            }

            if(currentByte == 0xBA)
            {
                currentState = FIND000001BA_STATE;
                if(length < 10)             //can't be less than 12 byte( including startcode)
                {
                  ALOGD("********************* parsePackHeader can't find pack header start code *********************");
                  return 0;
                }
            }
            else
            {
                currentState = INIT_STATE;  //reset
            }
            break;
        }
        default:
        {
            ALOGD("Unknow Pack header parsing state.");
            return 0;
        }
      }
      if(currentState == FIND000001BA_STATE){
        ALOGD("PACK HEADER FOUND");
        break;
      }
    }

    uint64_t SRC = 0;
    int muxrate = 0;
    if (br->getBits(2) == 1u)//mpeg2
    {
        if (length < 14)
            return 0;
        SRC = ((uint64_t)br->getBits(3)) << 30;

        if(br->getBits(1) != 1u){
            return 0;
        }

        SRC |= ((uint64_t)br->getBits(15)) << 15;

        if(br->getBits(1) != 1u){
            return 0;
        }

        SRC |= br->getBits(15);

        if(br->getBits(1) != 1u){
            return 0;
        }

        /*int SRC_Ext = */br->getBits(9);

        if(br->getBits(1) != 1u){
            return 0;
        }

        muxrate = br->getBits(22);
        br->skipBits(7);
        size_t pack_stuffing_length = br->getBits(3);

        if (pack_stuffing_length <= 7)
        {
            if (br->numBitsLeft() < pack_stuffing_length * 8)
                return 0;
            br->skipBits(pack_stuffing_length * 8);
        }

    }
    else//mpeg1
    {
        br->skipBits(2);
        SRC = ((uint64_t)br->getBits(3)) << 30;
        if(br->getBits(1)!= 1u){
            return 0;
        }
        SRC |= ((uint64_t)br->getBits(15)) << 15;
        if(br->getBits(1) != 1u){
            return 0;
        }
        SRC |= br->getBits(15);

        if(br->getBits(1) != 1u){
            return 0;
        }

        if(br->getBits(1) != 1u){
            return 0;
        }

        muxrate = br->getBits(22);

        if(br->getBits(1) != 1u){
            return 0;
        }
    }
    int offset = length - br->numBitsLeft()/8;
    return offset;
}


int  parseSystemHeader(ABitReader * br)
{
        int currentByte = -1;

        int length = br->numBitsLeft()/8;
        if(length < 6)
        {
            ALOGD("data too less");
            return 0;
        }

        enum {
            INIT_STATE,
            FIND00_STATE,
            FIND0000_STATE,
            FIND000001_STATE,
            FIND000001BB_STATE,

        } currentState;
        currentState = INIT_STATE;

       //search for pack header start code
       while (1) {
          currentByte = br->getBits(8);
          length--;
          switch(currentState)
          {
            case INIT_STATE:
            {
                if(length < 5)
                {
                    ALOGD("data too less");
                    return 0;
                }

                if(currentByte == 0x00)
                {
                    currentState = FIND00_STATE;
                }
                else
                {
                    currentState = INIT_STATE;  //reset
                }
                break;
            }
            case FIND00_STATE:
            {
                if(length < 4)
                {
                    ALOGD("data too less");
                    return 0;
                }

                if(currentByte == 0x00)
                {
                    currentState = FIND0000_STATE;
                }
                else
                {
                    currentState = INIT_STATE;      //reset
                }
                break;
            }
            case FIND0000_STATE:
            {
                if(length < 3)
                {
                    ALOGD("data too less");
                    return 0;
                }

                if(currentByte == 0x01)
                {
                    currentState = FIND000001_STATE;
                }
                else if(currentByte == 0x00)
                {
                    currentState = FIND0000_STATE;
                }
                else
                {
                    currentState = INIT_STATE;  //reset
                }
                break;
            }
            case FIND000001_STATE:
            {
                if(length < 2)
                {
                    ALOGD("data too less");
                    return 0;
                }

                if(currentByte == 0xBB)
                {
                    currentState = FIND000001BB_STATE;
                }
                else
                {
                    currentState = INIT_STATE;  //reset
                }
                break;
            }
            default:
            {
                ALOGD("Unknow Pack header parsing state.");
                return 0;
            }
          }
          if(currentState == FIND000001BB_STATE) {
            ALOGD("SYSTEM HEADER FOUND");
            break;
          }
        }

    size_t header_lenth = br->getBits(16);
    if(header_lenth > br->numBitsLeft()/8)
    {
        ALOGD("data too less");
        return 0;
    }
    if(br->getBits(1) != 1u){
      return 0;
    }

    /*int rate_bound = */br->getBits(22);

    if(br->getBits(1) != 1u){
      return 0;
    }
    /*int audio_bound = */br->getBits(6);
    /*unsigned fixed_flag = */br->getBits(1);
    /*unsigned CSPS_flag = */br->getBits(1);
    /*unsigned system_audio_lock_flag = */br->getBits(1);
    /*unsigned system_video_lock_flag = */br->getBits(1);

    if(br->getBits(1) != 1u){
        return 0;
    }

    /*unsigned video_bound = */br->getBits(5);
    /*unsigned packet_rate_restriction_flag = */br->getBits(1);
    br->skipBits(7);//skip reserved_bits
    int leftsize = header_lenth - 6;
    while(leftsize >= 3)
    {
        unsigned stream_id = br->getBits(8);
        if(stream_id >= 0xBC || stream_id == 0xB8 || stream_id == 0xB9)
        {
            br->skipBits(16);
            leftsize -= 3;
            continue;
        }
        if(br->getBits(2) != 3u){
            return 0;
        }
        /*unsigned P_STD_buffer_bound_scale = */br->getBits(1);
        /*int P_STD_buffer_size_bound = */br->getBits(13);
        //save stream info

        leftsize -= 3;
    }
    return 1;

}
#ifdef MTK_MTKPS_PLAYBACK_SUPPORT
bool fastSniffPS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {

    ALOGD("+fastSniffPS in");
    uint8_t *readbuff = NULL;
    size_t  sniff_length = SNIFF_LENGTH_900K;//for performance issue, only sniff 900k for dat files

    if(source->flags() & DataSource::kIsCachingDataSource){
        sniff_length = SNIFF_LENGTH_1K;
        ALOGD("caching data source, sniff length: 1K");
    }

    readbuff = (uint8_t *)malloc(sniff_length);
    if (NULL == readbuff) {
        ALOGE("fail to allocate memory for readbuff");
        return false;
    }

    memset(readbuff, 0, sniff_length);
    int length = source->readAt(0, readbuff, sniff_length);
    if (length < 0) {
        free(readbuff);
        return false;
    }
    ABitReader br(readbuff,(size_t)length);
    if (!parsePackHeader(&br)) {
        free(readbuff);
        return false;
    }
    *confidence = 0.4f;
    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2PS);
    free(readbuff);
    ALOGD("-fastSniffPS out");
    return true;
}
#endif
#endif
bool SniffMPEG2PS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {
#ifdef MTK_AOSP_ENHANCEMENT


    ALOGD("+SniffMPEGPS in");

    uint8_t *readbuff = NULL;
    size_t  sniff_length = PS_SNIFF_LENGTH;

    //for streaming source, always sets sniff length as 1K for good performance.
    #ifdef MTK_MTKPS_PLAYBACK_SUPPORT
    if(source->flags() & DataSource::kIsCachingDataSource){
        sniff_length = SNIFF_LENGTH_1K;
        ALOGD("caching data source, sniff length: 1K");
    }
    #endif

    readbuff = (uint8_t *)malloc(sniff_length);
    if (NULL == readbuff) {
        ALOGE("fail to allocate memory for readbuff");
        return false;
    }

    //ALOGD("sniff length: 0x%x", sniff_length);
    memset(readbuff, 0, sniff_length);
    int length = source->readAt(0, readbuff, sniff_length);
    if(length < 0)
    {
        //ALOGV("*********************source->readAt ERROR:length = %d*********************",length);
        free(readbuff);
        return false;
    }
    ABitReader br(readbuff,(size_t)length);
    if (!parsePackHeader(&br))
    {
        ALOGV("*********************parsePackHeader ERROR*********************");
        free(readbuff);
        return false;
    }
    *confidence = 0.4f;
    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2PS);
    free(readbuff);
#else
    uint8_t header[5];
    if (source->readAt(0, header, sizeof(header)) < (ssize_t)sizeof(header)) {
         ALOGE("SniffMPEG2PS: error1");
        return false;
    }

    if (memcmp("\x00\x00\x01\xba", header, 4) || (header[4] >> 6) != 1) {
        ALOGE("SniffMPEG2PS: error2");
        return false;
    }

    *confidence = 0.25f;  // Slightly larger than .mp3 extractor's confidence

    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2PS);
    ALOGE("SniffMPEG2PS: is MPEG2PS");
#endif
    ALOGD("-SniffMPEGPS out");
    return true;
}

}  // namespace android
