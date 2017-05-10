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

#ifndef NUPLAYER_SOURCE_H_

#define NUPLAYER_SOURCE_H_

#include "NuPlayer.h"

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MetaData.h>
#include <media/mediaplayer.h>
#include <utils/Vector.h>

namespace android {

struct ABuffer;
class MediaBuffer;

#ifdef MTK_AOSP_ENHANCEMENT
// To add Parcel into an AMessage as an object, it should be 'RefBase'.
struct ParcelEvent : public RefBase {
    Parcel parcel;
};
#endif


struct NuPlayer::Source : public AHandler {
    enum Flags {
        FLAG_CAN_PAUSE          = 1,
        FLAG_CAN_SEEK_BACKWARD  = 2,  // the "10 sec back button"
        FLAG_CAN_SEEK_FORWARD   = 4,  // the "10 sec forward button"
        FLAG_CAN_SEEK           = 8,  // the "seek bar"
        FLAG_DYNAMIC_DURATION   = 16,
        FLAG_SECURE             = 32,
        FLAG_PROTECTED          = 64,
    };

    enum {
        kWhatPrepared,
        kWhatFlagsChanged,
        kWhatVideoSizeChanged,
        kWhatBufferingUpdate,
        kWhatBufferingStart,
        kWhatBufferingEnd,
        kWhatPauseOnBufferingStart,
        kWhatResumeOnBufferingEnd,
        kWhatCacheStats,
        kWhatSubtitleData,
        kWhatTimedTextData,
        kWhatTimedMetaData,
#ifdef MTK_AOSP_ENHANCEMENT
        kWhatTimedTextData2,
#endif
        kWhatQueueDecoderShutdown,
        kWhatDrmNoLicense,
        kWhatInstantiateSecureDecoders,
#ifdef MTK_AOSP_ENHANCEMENT
        kWhatConnDone       = 'cdon',
        kWhatBufferNotify   = 'buff',
        kWhatSeekDone       = 'sdon',
        kWhatPauseDone      = 'psdn',
        kWhatPlayDone       = 'pldn',
        kWhatPicture        = 'pict', // orange
        kWhatSourceError    = 'serr',
        kWhatDurationUpdate = 'dura'
#endif
    };

    // The provides message is used to notify the player about various
    // events.
    Source(const sp<AMessage> &notify)
        : mNotify(notify) {
    }

    virtual void prepareAsync() = 0;

    virtual void start() = 0;
    virtual void stop() {}
#ifndef MTK_AOSP_ENHANCEMENT
    virtual void pause() {}
    virtual void resume() {}
#endif
    // Explicitly disconnect the underling data source
    virtual void disconnect() {}

    // Returns OK iff more data was available,
    // an error or ERROR_END_OF_STREAM if not.
    virtual status_t feedMoreTSData() = 0;

    virtual sp<AMessage> getFormat(bool audio);
    virtual sp<MetaData> getFormatMeta(bool /* audio */) { return NULL; }
    virtual sp<MetaData> getFileFormatMeta() const { return NULL; }

    virtual status_t dequeueAccessUnit(
            bool audio, sp<ABuffer> *accessUnit) = 0;

    virtual status_t getDuration(int64_t * /* durationUs */) {
        return INVALID_OPERATION;
    }

    virtual size_t getTrackCount() const {
        return 0;
    }

    virtual sp<AMessage> getTrackInfo(size_t /* trackIndex */) const {
        return NULL;
    }

    virtual ssize_t getSelectedTrack(media_track_type /* type */) const {
        return INVALID_OPERATION;
    }

    virtual status_t selectTrack(size_t /* trackIndex */, bool /* select */, int64_t /* timeUs*/) {
        return INVALID_OPERATION;
    }

    virtual status_t seekTo(int64_t /* seekTimeUs */) {
        return INVALID_OPERATION;
    }

    virtual status_t setBuffers(bool /* audio */, Vector<MediaBuffer *> &/* buffers */) {
        return INVALID_OPERATION;
    }

    virtual bool isRealTime() const {
        return false;
    }

    virtual bool isStreaming() const {
        return true;
    }

protected:
    virtual ~Source() {}

    virtual void onMessageReceived(const sp<AMessage> &msg);

    sp<AMessage> dupNotify() const { return mNotify->dup(); }

    void notifyFlagsChanged(uint32_t flags);
    void notifyVideoSizeChanged(const sp<AMessage> &format = NULL);
    void notifyInstantiateSecureDecoders(const sp<AMessage> &reply);
    void notifyPrepared(status_t err = OK);

private:
    sp<AMessage> mNotify;

#ifdef MTK_AOSP_ENHANCEMENT
public:
    virtual bool hasVideo() { return false;}
    // mtk80902: just keep default defination..
    virtual void pause() {
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", kWhatPauseDone);
        notify->setInt32("result", OK);
        notify->post();
    }
    virtual void resume() {
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", kWhatPlayDone);
        notify->setInt32("result", OK);
        notify->post();
    }
    //  return -EWOULDBLOCK: not ready
    //  return OK: is ready
    //  virtual status_t allTracksPresent() {return INVALID_OPERATION;};
    virtual status_t initCheck() const {return OK;}
    virtual void setParams(const sp<MetaData> &) {};
    virtual status_t getFinalStatus() const {return OK;}
    virtual status_t getBufferedDuration(bool , int64_t *) {return INVALID_OPERATION;};
    virtual sp<MetaData> getMetaData() {return mMetaData;};
    virtual void stopTrack(bool ) { return; }
    virtual bool notifyCanNotConnectServerIfPossible(int64_t /*curPositionUs*/) {return false;}

    enum DataSourceType {
        SOURCE_Default,
        SOURCE_HttpLive,
        SOURCE_Local,
        SOURCE_Rtsp,
        SOURCE_Http,
    };

    virtual DataSourceType getDataSourceType() { return SOURCE_Default; }

    enum {
        NOT_USE_RENDEREDPOSITIONUS = -1
    };

protected:
    sp<MetaData> mMetaData;
#endif
    DISALLOW_EVIL_CONSTRUCTORS(Source);
};

}  // namespace android

#endif  // NUPLAYER_SOURCE_H_

