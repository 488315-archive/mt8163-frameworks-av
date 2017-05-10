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

#ifndef MPEG2_PS_EXTRACTOR_H_

#define MPEG2_PS_EXTRACTOR_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/MediaExtractor.h>
#include <utils/threads.h>
#include <utils/KeyedVector.h>

namespace android {

struct ABuffer;
struct AMessage;
struct Track;
class String8;

#ifdef MTK_AOSP_ENHANCEMENT
struct AnotherPacketSource;
#endif

struct MPEG2PSExtractor : public MediaExtractor {
    MPEG2PSExtractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);

    virtual sp<MetaData> getMetaData();

    virtual uint32_t flags() const;
#ifdef MTK_AOSP_ENHANCEMENT
    bool bisPlayable;
    virtual ~MPEG2PSExtractor();
#else
protected:
    virtual ~MPEG2PSExtractor();
#endif

private:
    struct Track;
    struct WrappedTrack;

    mutable Mutex mLock;
    sp<DataSource> mDataSource;

    off64_t mOffset;
    status_t mFinalResult;
    sp<ABuffer> mBuffer;
    KeyedVector<unsigned, sp<Track> > mTracks;
    bool mScanning;

#ifdef MTK_AOSP_ENHANCEMENT
    int64_t mDurationUs;
    int64_t mSeekTimeUs;
    bool mSeeking;
    off64_t mSeekingOffset;
    off64_t mFileSize;
    off64_t mMinOffset;
    off64_t mMaxOffset;
    uint64_t mMaxcount;
    unsigned mSeekStreamID;
    off64_t mlastValidPESSCOffset;
    bool mIsCrossChunk;
    bool mMPEG1Flag;
    bool mhasVTrack;
    bool mhasATrack;
    bool mValidESFrame;
    int64_t mSearchPTS;
    off64_t mSearchPTSOffset;
    off64_t mAverageByteRate;
    bool mSystemHeaderValid;
    bool mParseMaxTime;
    bool mNeedDequeuePES;
    void setDequeueState(bool needDequeuePES);
    bool getDequeueState();
    int64_t getMaxPTS();
    int64_t getMaxVideoPTS();
    void seekTo(int64_t seekTimeUs, unsigned StreamID);
    void parseMaxPTS();
    uint64_t getDurationUs();
    void init();
    bool getSeeking();
    void signalDiscontinuity(const bool bKeepFormat = false);
    int findNextPES(const void* data,int length);
    int64_t getLastPESWithIFrame(off64_t end);
    int64_t getNextPESWithIFrame(off64_t begin);
    int64_t SearchPES(const void* data, int size);
    int64_t SearchValidOffset(off64_t currentoffset);
    bool IsSeeminglyValidADTSHeader(const uint8_t *ptr, size_t size);
    bool IsSeeminglyValidMPEGAudioHeader(const uint8_t *ptr, size_t size);
    unsigned findSubStreamId(const uint8_t *data, const size_t size);
    void updateSeekOffset(int64_t pts);
    bool consumeData(sp<AnotherPacketSource> pSource, int64_t timeUS);
    bool needRemoveData(sp<AnotherPacketSource> pSource, int64_t timeUs);
#endif //MTK_AOSP_ENHANCEMENT

    bool mProgramStreamMapValid;
    KeyedVector<unsigned, unsigned> mStreamTypeByESID;

    status_t feedMore();

    status_t dequeueChunk();
    ssize_t dequeuePack();
    ssize_t dequeueSystemHeader();
    ssize_t dequeuePES();

    DISALLOW_EVIL_CONSTRUCTORS(MPEG2PSExtractor);
};

bool SniffMPEG2PS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *);

#ifdef MTK_AOSP_ENHANCEMENT
bool fastSniffPS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *);
#endif

}  // namespace android

#endif  // MPEG2_PS_EXTRACTOR_H_

