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

#ifndef MPEG4_WRITER_H_

#define MPEG4_WRITER_H_

#include <stdio.h>

#include <media/stagefright/MediaWriter.h>
#include <utils/List.h>
#include <utils/threads.h>

#ifdef MTK_AOSP_ENHANCEMENT
#include <utils/String8.h>
#include <media/stagefright/MediaBuffer.h>
#endif

namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
#define USE_FILE_CACHE
#define SD_FULL_PROTECT
#define LOW_MEM_PROTECT_THRESHOLD   70*1024*1024LL
#define CHECK_LOW_MEM_BY_MEM_FREE

//make the method to record the  start time offset of two tracks optional
//#define WRITER_ENABLE_EDTS_BOX
#ifdef USE_FILE_CACHE
class MPEG4FileCacheWriter;
#endif
#endif

class AMessage;
class MediaBuffer;
class MediaSource;
class MetaData;

class MPEG4Writer : public MediaWriter {
public:
    MPEG4Writer(int fd);

    // Limitations
    // 1. No more than 2 tracks can be added
    // 2. Only video or audio source can be added
    // 3. No more than one video and/or one audio source can be added.
    virtual status_t addSource(const sp<MediaSource> &source);

    // Returns INVALID_OPERATION if there is no source or track.
    virtual status_t start(MetaData *param = NULL);
    virtual status_t stop() { return reset(); }
    virtual status_t pause();
    virtual bool reachedEOS();
    virtual status_t dump(int fd, const Vector<String16>& args);

    void beginBox(const char *fourcc);
    void beginBox(uint32_t id);
    void writeInt8(int8_t x);
    void writeInt16(int16_t x);
    void writeInt32(int32_t x);
    void writeInt64(int64_t x);
    void writeCString(const char *s);
    void writeFourcc(const char *fourcc);
    void write(const void *data, size_t size);
    inline size_t write(const void *ptr, size_t size, size_t nmemb);
    void endBox();
    uint32_t interleaveDuration() const { return mInterleaveDurationUs; }
    status_t setInterleaveDuration(uint32_t duration);
    int32_t getTimeScale() const { return mTimeScale; }

    status_t setGeoData(int latitudex10000, int longitudex10000);
    status_t setCaptureRate(float captureFps);
    virtual void setStartTimeOffsetMs(int ms) { mStartTimeOffsetMs = ms; }
    virtual int32_t getStartTimeOffsetMs() const { return mStartTimeOffsetMs; }

protected:
    virtual ~MPEG4Writer();

private:
    class Track;

    int  mFd;
    status_t mInitCheck;
    bool mIsRealTimeRecording;
    bool mUse4ByteNalLength;
    bool mUse32BitOffset;
    bool mIsFileSizeLimitExplicitlyRequested;
    bool mPaused;
    bool mStarted;  // Writer thread + track threads started successfully
    bool mWriterThreadStarted;  // Only writer thread started successfully
    off64_t mOffset;
    off_t mMdatOffset;
    uint8_t *mMoovBoxBuffer;
    off64_t mMoovBoxBufferOffset;
    bool  mWriteMoovBoxToMemory;
    off64_t mFreeBoxOffset;
    bool mStreamableFile;
    off64_t mEstimatedMoovBoxSize;
    off64_t mMoovExtraSize;
    uint32_t mInterleaveDurationUs;
    int32_t mTimeScale;
    int64_t mStartTimestampUs;
    int mLatitudex10000;
    int mLongitudex10000;
    bool mAreGeoTagsAvailable;
    int32_t mStartTimeOffsetMs;

    Mutex mLock;

    List<Track *> mTracks;

    List<off64_t> mBoxes;

    sp<AMessage> mMetaKeys;

    void setStartTimestampUs(int64_t timeUs);
    int64_t getStartTimestampUs();  // Not const
    status_t startTracks(MetaData *params);
    size_t numTracks();
    int64_t estimateMoovBoxSize(int32_t bitRate);

    struct Chunk {
        Track               *mTrack;        // Owner
        int64_t             mTimeStampUs;   // Timestamp of the 1st sample
        List<MediaBuffer *> mSamples;       // Sample data
#ifdef MTK_AOSP_ENHANCEMENT
        int64_t             mDataSize;
#endif

        // Convenient constructor
        Chunk(): mTrack(NULL), mTimeStampUs(0) {}

        Chunk(Track *track, int64_t timeUs, List<MediaBuffer *> samples)
            : mTrack(track), mTimeStampUs(timeUs), mSamples(samples) {
#ifdef MTK_AOSP_ENHANCEMENT
            mDataSize = 0;

            for (List<MediaBuffer *>::iterator it = mSamples.begin(); it != mSamples.end(); it++) {
                mDataSize += (*it)->range_length();
            }
#endif
        }

    };
    struct ChunkInfo {
        Track               *mTrack;        // Owner
        List<Chunk>         mChunks;        // Remaining chunks to be written

        // Previous chunk timestamp that has been written
        int64_t mPrevChunkTimestampUs;

        // Max time interval between neighboring chunks
        int64_t mMaxInterChunkDurUs;

    };

    bool            mIsFirstChunk;
    volatile bool   mDone;                  // Writer thread is done?
    pthread_t       mThread;                // Thread id for the writer
    List<ChunkInfo> mChunkInfos;            // Chunk infos
    Condition       mChunkReadyCondition;   // Signal that chunks are available

    // Writer thread handling
    status_t startWriterThread();
    void stopWriterThread();
    static void *ThreadWrapper(void *me);
    void threadFunc();

    // Buffer a single chunk to be written out later.
    void bufferChunk(const Chunk& chunk);

    // Write all buffered chunks from all tracks
    void writeAllChunks();

    // Retrieve the proper chunk to write if there is one
    // Return true if a chunk is found; otherwise, return false.
    bool findChunkToWrite(Chunk *chunk);

    // Actually write the given chunk to the file.
    void writeChunkToFile(Chunk* chunk);

    // Adjust other track media clock (presumably wall clock)
    // based on audio track media clock with the drift time.
    int64_t mDriftTimeUs;
    void setDriftTimeUs(int64_t driftTimeUs);
    int64_t getDriftTimeUs();

    // Return whether the nal length is 4 bytes or 2 bytes
    // Only makes sense for H.264/AVC
    bool useNalLengthFour();

    // Return whether the writer is used for real time recording.
    // In real time recording mode, new samples will be allowed to buffered into
    // chunks in higher priority thread, even though the file writer has not
    // drained the chunks yet.
    // By default, real time recording is on.
    bool isRealTimeRecording() const;

    void lock();
    void unlock();

    // Acquire lock before calling these methods
    off64_t addSample_l(MediaBuffer *buffer);
    off64_t addLengthPrefixedSample_l(MediaBuffer *buffer);

    bool exceedsFileSizeLimit();
    bool use32BitFileOffset() const;
    bool exceedsFileDurationLimit();
    bool isFileStreamable() const;
    void trackProgressStatus(size_t trackId, int64_t timeUs, status_t err = OK);
    void writeCompositionMatrix(int32_t degrees);
    void writeMvhdBox(int64_t durationUs);
    void writeMoovBox(int64_t durationUs);
    void writeFtypBox(MetaData *param);
    void writeUdtaBox();
    void writeGeoDataBox();
    void writeLatitude(int degreex10000);
    void writeLongitude(int degreex10000);

    void addDeviceMeta();
    void writeHdlr();
    void writeKeys();
    void writeIlst();
    void writeMetaBox();
    void sendSessionSummary();
    void release();
    status_t reset();

    static uint32_t getMpeg4Time();

    MPEG4Writer(const MPEG4Writer &);
    MPEG4Writer &operator=(const MPEG4Writer &);
#ifdef MTK_AOSP_ENHANCEMENT
public:
    int64_t     getMaxDurationUs();
    void        writeMetaData();
    int64_t     getPausedDuration();
    void        setPausedDuration(int64_t paudedDurationUs);
    void        setAudioTrackResumed();
    void        setVideoTrackResumed();
    void        signalResumed();

#ifdef SD_FULL_PROTECT
    void        setSDFull() { mIsSDFull = true; }
    bool        isSDFull() { return mIsSDFull; }
    void        processSDFull();
    void        finishHandleSDFull();
    status_t    getTrackResetStatus(){ return mTrackResetStatus; }// add for check track reset status ok or err
                                                                  // if err do not write moov
#endif

#ifdef WRITER_ENABLE_EDTS_BOX
    //make the method to record the  start time offset of two tracks optional
    void        EnableEdtsBoxForTimeOffset(bool enable) { mEnableEdtsBoxForTimeOffset = enable;}
    bool        isEnableEdtsBoxForTimeOffset() {return mEnableEdtsBoxForTimeOffset;}
#endif

private:
    void        init();
    void        releaseEx();  // release in ~MPEG4Writer
    status_t    resume(MetaData *param);
    void        initStart(MetaData *param);
    void        waitWriterThreadExit();
    bool        isEnable64BitDuration(int64_t durationUs);
    off64_t     writeSEIbuffer(MediaBuffer *buffer);
    void        addSample_lUseCacheWriter(MediaBuffer *buffer);
    void        checkBufferedMem(const Chunk& chunk);
    bool        isLengthPrefixed(Chunk* chunk);
    void        eraseChunkSamples(Chunk* chunk);
    void        addWritedChunk(Chunk* chunk, int32_t chunkSize);
    void        writeArtAlbBox();
    bool        isNearLowMemory();
    long        getMinFreeMem();
    long        getSysRetainMem();
    void        setVideoStartTimeUs(int64_t timeUs);
    int64_t     getVideoStartTimeUs();
private:
    int         mMemInfoFd;
    long        mMinFreeMem;
    int32_t     mTotalBitrate;
    long        mSysRetainMem;
    bool        useMemFreeCheckLM;
    bool        mResumed;
    int64_t     mPausedDurationUs;
    bool        mAudioTrackResumed;
    bool        mVideoTrackResumed;
    int64_t     mVideoStartTimeUs;
    Condition   mResumedCondition;
    bool        mWriterThreadExit;
    Condition   mWriterThreadExitCondition;
    int64_t     mMaxDuration;
    String8     mArtistTag;
    String8     mAlbumTag;
    int64_t     mLowMemoryProtectThreshold;
    int64_t     mBufferedDataSize;
    uint32_t    mNotifyCounter;
    int32_t     mMediaInfoFlag;//add for mtk defined infos in mediarecorder.h.
    void        notifyEstimateSize(int64_t nTotalBytesEstimate);
#ifdef USE_FILE_CACHE
    friend class MPEG4FileCacheWriter;
    MPEG4FileCacheWriter *mCacheWriter;
    size_t      mWriterCacheSize;
#endif

#ifdef SD_FULL_PROTECT
    bool        mIsSDFull;
    bool        mSDHasFull;
    status_t    mTrackResetStatus;// add for check track reset status ok or err
                                  // if err do not write moov

    struct WritedChunk {
        Track       *mTrack;        //Owner
        int32_t     mSize;          //Chunk size

        WritedChunk(Track *track, int32_t size) : mTrack(track), mSize(size) {}
    };

    List<WritedChunk*>  mWritedChunks;
#endif

#ifdef WRITER_ENABLE_EDTS_BOX
    //make the method to record the  start time offset of two tracks optional
    bool        mEnableEdtsBoxForTimeOffset;
#endif

#endif
};

}  // namespace android

#endif  // MPEG4_WRITER_H_
