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

#ifndef RTSP_SOURCE_H_

#define RTSP_SOURCE_H_

#include "NuPlayerSource.h"

#include "ATSParser.h"

namespace android {

struct ALooper;
struct AReplyToken;
struct AnotherPacketSource;
struct MyHandler;
struct SDPLoader;

struct NuPlayer::RTSPSource : public NuPlayer::Source {
    RTSPSource(
            const sp<AMessage> &notify,
            const sp<IMediaHTTPService> &httpService,
            const char *url,
            const KeyedVector<String8, String8> *headers,
            bool uidValid = false,
            uid_t uid = 0,
            bool isSDP = false);

    virtual void prepareAsync();
    virtual void start();
    virtual void stop();
    virtual void pause();
    virtual void resume();

    virtual status_t feedMoreTSData();

    virtual status_t dequeueAccessUnit(bool audio, sp<ABuffer> *accessUnit);

    virtual status_t getDuration(int64_t *durationUs);
    virtual status_t seekTo(int64_t seekTimeUs);

    void onMessageReceived(const sp<AMessage> &msg);

protected:
    virtual ~RTSPSource();

    virtual sp<MetaData> getFormatMeta(bool audio);

private:
    enum {
        kWhatNotify          = 'noti',
        kWhatDisconnect      = 'disc',
        kWhatPerformSeek     = 'seek',
#ifdef MTK_AOSP_ENHANCEMENT
        kWhatSendPlay        = 'play',
        kWhatSendPause         = 'paus',
        kWhatBufferingUpdate = 'buff',
        kWhatStopTrack       = 'rmtk'
#endif
    };

    enum State {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        SEEKING,
    };

    enum Flags {
        // Don't log any URLs.
        kFlagIncognito = 1,
    };

    struct TrackInfo {
        sp<AnotherPacketSource> mSource;

        int32_t mTimeScale;
        uint32_t mRTPTime;
        int64_t mNormalPlaytimeUs;
        bool mNPTMappingValid;
    };

    sp<IMediaHTTPService> mHTTPService;
    AString mURL;
    KeyedVector<String8, String8> mExtraHeaders;
    bool mUIDValid;
    uid_t mUID;
    uint32_t mFlags;
    bool mIsSDP;
    State mState;
    status_t mFinalResult;
    sp<AReplyToken> mDisconnectReplyID;
    Mutex mBufferingLock;
    bool mBuffering;

    sp<ALooper> mLooper;
    sp<MyHandler> mHandler;
    sp<SDPLoader> mSDPLoader;

    Vector<TrackInfo> mTracks;
    sp<AnotherPacketSource> mAudioTrack;
    sp<AnotherPacketSource> mVideoTrack;

    sp<ATSParser> mTSParser;

    int32_t mSeekGeneration;

    int64_t mEOSTimeoutAudio;
    int64_t mEOSTimeoutVideo;

    sp<AReplyToken> mSeekReplyID;

    sp<AnotherPacketSource> getSource(bool audio);

    void onConnected();
    void onSDPLoaded(const sp<AMessage> &msg);
    void onDisconnected(const sp<AMessage> &msg);
    void finishDisconnectIfPossible();

    void performSeek(int64_t seekTimeUs);

    bool haveSufficientDataOnAllTracks();

    void setEOSTimeout(bool audio, int64_t timeout);
    void setError(status_t err);
    void startBufferingIfNecessary();
    bool stopBufferingIfNecessary();
    void finishSeek(status_t err);



#ifdef MTK_AOSP_ENHANCEMENT
public:
    //as in the RTSPSOurce contruction API, this httpService param is a reference sp ,
    //it can not be passed with NULL.so add a new construction
    RTSPSource(
        const sp<AMessage> &notify,
        const char *url,
        const KeyedVector<String8, String8> *headers,
        bool uidValid = false,
        uid_t uid = 0,
     bool isSDP = false);


    sp<RefBase>        msdp;

    virtual void    setParams(const sp<MetaData>& meta);
    void            setSDP(sp<RefBase> &sdp);
    void            stopTrack(bool audio);
    virtual DataSourceType getDataSourceType();
    virtual bool notifyCanNotConnectServerIfPossible(int64_t curPositionUs);

private:
    bool            mAllTrackEOS;
    size_t          mEOSTrackCount;
    int64_t         mSeekPosition;
    int64_t         mHighWaterMarkUs;
    bool            mQuitRightNow;
        //The following are for sync call
    // >>>
    Mutex             mLock;
    Condition         mCondition;
    status_t         mSyncCallResult;
    bool             mSyncCallDone;
    void             prepareSyncCall();
    void             completeSyncCall(const sp<AMessage>& msg);
    status_t         finishSyncCall();
    // <<<
    int64_t         mLastSeekCompletedTimeUs;
    //for bitrate adaptation
    size_t m_BufQueSize; //Whole Buffer queue size
    size_t m_TargetTime;  // target protected time of buffer queue duration for interrupt-free playback
    // mtk80902: standalone looper for MyHander
    sp<ALooper> mHandlerLooper;

    void            init();
    void            registerHandlerLooper();
    void            setHandler();
    void            setMeta(bool audio, sp<MetaData>& meta);
    void            notifyBufRate(int64_t durationUs);
    status_t         generalBufferedDurationUs(int64_t *durationUs);
    void             notifyAsyncDone(uint32_t notify, status_t err = OK);
    bool             removeSpecificHeaders(const String8 MyKey,
                                                                                 KeyedVector<String8,
                                                                                 String8> *headers,
                                                                                 String8 *pMyHeader);
    //The following are sync call method, using prepareSyncCall+finishSyncCall
    status_t         preSeekSync(int64_t timeUs);
    void             prepareMeta();
    void            onStopTrack(const sp<AMessage> &msg);
    void            notifySourceError(int32_t err);
    void            prepareMyHandler();
    status_t        mtkSeekTo(int64_t seekTimeUs);
    void            notifyBeginningVideoSizeChanged();
    sp<AnotherPacketSource> initAnotherPacketSource(int32_t index, sp<MetaData> format, bool isAudio);
#endif

    DISALLOW_EVIL_CONSTRUCTORS(RTSPSource);
};

}  // namespace android

#endif  // RTSP_SOURCE_H_
