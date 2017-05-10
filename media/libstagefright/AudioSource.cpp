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

#include <inttypes.h>
#include <stdlib.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioSource"
#include <utils/Log.h>

#include <media/AudioRecord.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <cutils/properties.h>

namespace android {

static void AudioRecordCallbackFunction(int event, void *user, void *info) {
    AudioSource *source = (AudioSource *) user;
    switch (event) {
        case AudioRecord::EVENT_MORE_DATA: {
            source->dataCallback(*((AudioRecord::Buffer *) info));
            break;
        }
        case AudioRecord::EVENT_OVERRUN: {
            ALOGW("AudioRecord reported overrun!");
            break;
        }
        default:
            // does nothing
            break;
    }
}

AudioSource::AudioSource(
        audio_source_t inputSource, const String16 &opPackageName,
        uint32_t sampleRate, uint32_t channelCount, uint32_t outSampleRate)
    : mStarted(false),
      mSampleRate(sampleRate),
      mOutSampleRate(outSampleRate > 0 ? outSampleRate : sampleRate),
      mTrackMaxAmplitude(false),
      mStartTimeUs(0),
      mMaxAmplitude(0),
      mPrevSampleTimeUs(0),
      mFirstSampleTimeUs(-1ll),
      mInitialReadTimeUs(0),
      mNumFramesReceived(0),
      mNumClientOwnedBuffers(0) {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("sampleRate: %u, outSampleRate: %u, channelCount: %u",
            sampleRate, outSampleRate, channelCount);
#else
    ALOGV("sampleRate: %u, outSampleRate: %u, channelCount: %u",
            sampleRate, outSampleRate, channelCount);
#endif
    CHECK(channelCount == 1 || channelCount == 2);
    CHECK(sampleRate > 0);

    size_t minFrameCount;
    status_t status = AudioRecord::getMinFrameCount(&minFrameCount,
                                           sampleRate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           audio_channel_in_mask_from_count(channelCount));
    if (status == OK) {
        // make sure that the AudioRecord callback never returns more than the maximum
        // buffer size
        uint32_t frameCount = kMaxBufferSize / sizeof(int16_t) / channelCount;

        // make sure that the AudioRecord total buffer size is large enough
        size_t bufCount = 2;
        while ((bufCount * frameCount) < minFrameCount) {
            bufCount++;
        }

        mRecord = new AudioRecord(
                    inputSource, sampleRate, AUDIO_FORMAT_PCM_16_BIT,
                    audio_channel_in_mask_from_count(channelCount),
                    opPackageName,
                    (size_t) (bufCount * frameCount),
                    AudioRecordCallbackFunction,
                    this,
                    frameCount /*notificationFrames*/);
        mInitCheck = mRecord->initCheck();
        if (mInitCheck != OK) {
            mRecord.clear();
        }
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGI("AudioSource constructor, getMinFrameCount return minFrameCount =%zu",minFrameCount);
        ALOGI("AudioSource constructor, buffer requirment: frameCount,=%d,bufCount =%zu,mInitCheck=%d",\
                frameCount,bufCount,mInitCheck);
#endif
    } else {
        mInitCheck = status;
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGI("AudioSource constructor, getMinFrameCount fail !!!,mInitCheck=%d",mInitCheck);
#endif
    }
}



AudioSource::~AudioSource() {
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("~AudioSource destructor");
#endif
    if (mStarted) {
        reset();
    }
}

status_t AudioSource::initCheck() const {
    return mInitCheck;
}

status_t AudioSource::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);
    if (mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    mTrackMaxAmplitude = false;
    mMaxAmplitude = 0;
    mInitialReadTimeUs = 0;
    mStartTimeUs = 0;
    int64_t startTimeUs;
    if (params && params->findInt64(kKeyTime, &startTimeUs)) {
        mStartTimeUs = startTimeUs;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("start, call AudioRecord start+++,mStartTimeUs=%" PRId64 "",mStartTimeUs);
#endif
    status_t err = mRecord->start();
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("start, call AudioRecord start ---,err=%d",err);
#endif
    if (err == OK) {
        mStarted = true;
    } else {
        mRecord.clear();
    }


    return err;
}

void AudioSource::releaseQueuedFrames_l() {
    ALOGV("releaseQueuedFrames_l");
    List<MediaBuffer *>::iterator it;
    while (!mBuffersReceived.empty()) {
        it = mBuffersReceived.begin();
        (*it)->release();
        mBuffersReceived.erase(it);
    }
}

void AudioSource::waitOutstandingEncodingFrames_l() {
    ALOGV("waitOutstandingEncodingFrames_l: %" PRId64, mNumClientOwnedBuffers);
    while (mNumClientOwnedBuffers > 0) {
        mFrameEncodingCompletionCondition.wait(mLock);
    }
}

status_t AudioSource::reset() {
    Mutex::Autolock autoLock(mLock);
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("reset");
#endif
    if (!mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    mStarted = false;
    mFrameAvailableCondition.signal();

#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("reset, call AudioRecord stop+++");
#endif
    mRecord->stop();
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGI("reset, call AudioRecord stop---");
#endif
    waitOutstandingEncodingFrames_l();
    releaseQueuedFrames_l();

    return OK;
}

sp<MetaData> AudioSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mInitCheck != OK) {
        return 0;
    }

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
    meta->setInt32(kKeySampleRate, mSampleRate);
    meta->setInt32(kKeyChannelCount, mRecord->channelCount());
    meta->setInt32(kKeyMaxInputSize, kMaxBufferSize);

    return meta;
}

void AudioSource::rampVolume(
        int32_t startFrame, int32_t rampDurationFrames,
        uint8_t *data,   size_t bytes) {

    const int32_t kShift = 14;
    int32_t fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
    const int32_t nChannels = mRecord->channelCount();
    int32_t stopFrame = startFrame + bytes / sizeof(int16_t);
    int16_t *frame = (int16_t *) data;
    if (stopFrame > rampDurationFrames) {
        stopFrame = rampDurationFrames;
    }

    while (startFrame < stopFrame) {
        if (nChannels == 1) {  // mono
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            ++frame;
            ++startFrame;
        } else {               // stereo
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            frame[1] = (frame[1] * fixedMultiplier) >> kShift;
            frame += 2;
            startFrame += 2;
        }

        // Update the multiplier every 4 frames
        if ((startFrame & 3) == 0) {
            fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
        }
    }
}

status_t AudioSource::read(
        MediaBuffer **out, const ReadOptions * /* options */) {
    Mutex::Autolock autoLock(mLock);
    *out = NULL;

    if (mInitCheck != OK) {
        return NO_INIT;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("read");
#endif
    while (mStarted && mBuffersReceived.empty()) {
        mFrameAvailableCondition.wait(mLock);
    }
    if (!mStarted) {
        return OK;
    }
    MediaBuffer *buffer = *mBuffersReceived.begin();
    mBuffersReceived.erase(mBuffersReceived.begin());
    ++mNumClientOwnedBuffers;
    buffer->setObserver(this);
    buffer->add_ref();

    // Mute/suppress the recording sound
    int64_t timeUs;
    CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("read, buffer kKeyTime timeUs=%" PRId64 "",timeUs);
#endif
    int64_t elapsedTimeUs = timeUs - mStartTimeUs;
    if (elapsedTimeUs < kAutoRampStartUs) {
        memset((uint8_t *) buffer->data(), 0, buffer->range_length());
    } else if (elapsedTimeUs < kAutoRampStartUs + kAutoRampDurationUs) {
        int32_t autoRampDurationFrames =
                    ((int64_t)kAutoRampDurationUs * mSampleRate + 500000LL) / 1000000LL; //Need type casting

        int32_t autoRampStartFrames =
                    ((int64_t)kAutoRampStartUs * mSampleRate + 500000LL) / 1000000LL; //Need type casting

        int32_t nFrames = mNumFramesReceived - autoRampStartFrames;
        rampVolume(nFrames, autoRampDurationFrames,
                (uint8_t *) buffer->data(), buffer->range_length());
    }

    // Track the max recording signal amplitude.
    if (mTrackMaxAmplitude) {
        trackMaxAmplitude(
            (int16_t *) buffer->data(), buffer->range_length() >> 1);
    }

    if (mSampleRate != mOutSampleRate) {
        if (mFirstSampleTimeUs < 0) {
            mFirstSampleTimeUs = timeUs;
        }
        timeUs = mFirstSampleTimeUs + (timeUs - mFirstSampleTimeUs)
                * (int64_t)mSampleRate / (int64_t)mOutSampleRate;
        buffer->meta_data()->setInt64(kKeyTime, timeUs);
    }

    *out = buffer;
    return OK;
}

void AudioSource::signalBufferReturned(MediaBuffer *buffer) {
    ALOGV("signalBufferReturned: %p", buffer->data());
    Mutex::Autolock autoLock(mLock);
    --mNumClientOwnedBuffers;
    buffer->setObserver(0);
    buffer->release();
    mFrameEncodingCompletionCondition.signal();
    return;
}

status_t AudioSource::dataCallback(const AudioRecord::Buffer& audioBuffer) {
    int64_t timeUs = systemTime() / 1000ll;

    ALOGV("dataCallbackTimestamp: %" PRId64 " us", timeUs);
    Mutex::Autolock autoLock(mLock);
    if (!mStarted) {
        ALOGW("Spurious callback from AudioRecord. Drop the audio data.");
        return OK;
    }

    // Drop retrieved and previously lost audio data.
    if (mNumFramesReceived == 0 && timeUs < mStartTimeUs) {
        (void) mRecord->getInputFramesLost();
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("dataCallback,Drop audio data at %" PRId64 "/%" PRId64 " us", timeUs, mStartTimeUs);
#else
        ALOGV("Drop audio data at %" PRId64 "/%" PRId64 " us", timeUs, mStartTimeUs);
#endif
        return OK;
    }

    if (mNumFramesReceived == 0 && mPrevSampleTimeUs == 0) {
        mInitialReadTimeUs = timeUs;
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("dataCallback, receiving time of the first frame mInitialReadTimeUs =%" PRId64 "",mInitialReadTimeUs);
        ALOGD("mStartTimeUs = %" PRId64 "",mStartTimeUs);
#endif
        // Initial delay
        if (mStartTimeUs > 0) {
            mStartTimeUs = timeUs - mStartTimeUs;
        } else {
            // Assume latency is constant.
            mStartTimeUs += mRecord->latency() * 1000;
        }

        mPrevSampleTimeUs = mStartTimeUs;
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("dataCallback, the revised mStartTimeUs =%" PRId64 "",mStartTimeUs);
#endif
    }

    size_t numLostBytes = 0;
    if (mNumFramesReceived > 0) {  // Ignore earlier frame lost
        // getInputFramesLost() returns the number of lost frames.
        // Convert number of frames lost to number of bytes lost.
        numLostBytes = mRecord->getInputFramesLost() * mRecord->frameSize();
    }

    CHECK_EQ(numLostBytes & 1, 0u);
    CHECK_EQ(audioBuffer.size & 1, 0u);
    if (numLostBytes > 0) {
        // Loss of audio frames should happen rarely; thus the LOGW should
        // not cause a logging spam
        ALOGW("Lost audio record data: %zu bytes", numLostBytes);
    }

    while (numLostBytes > 0) {
        size_t bufferSize = numLostBytes;
        if (numLostBytes > kMaxBufferSize) {
            numLostBytes -= kMaxBufferSize;
            bufferSize = kMaxBufferSize;
        } else {
            numLostBytes = 0;
        }
        MediaBuffer *lostAudioBuffer = new MediaBuffer(bufferSize);
        memset(lostAudioBuffer->data(), 0, bufferSize);
        lostAudioBuffer->set_range(0, bufferSize);
        queueInputBuffer_l(lostAudioBuffer, timeUs);
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGD("dataCallback,queue one input buffer with all 0 data for lost data");
#endif
    }

    if (audioBuffer.size == 0) {
        ALOGW("Nothing is available from AudioRecord callback buffer");
        return OK;
    }

    const size_t bufferSize = audioBuffer.size;
    MediaBuffer *buffer = new MediaBuffer(bufferSize);
    memcpy((uint8_t *) buffer->data(),
            audioBuffer.i16, audioBuffer.size);
    buffer->set_range(0, bufferSize);
    queueInputBuffer_l(buffer, timeUs);
#ifdef MTK_AOSP_ENHANCEMENT
    ALOGV("dataCallback,receive one audio buffer (size =%zu,timeUs=%" PRId64 ")", bufferSize, timeUs);
#endif
    return OK;
}

#ifndef MTK_AOSP_ENHANCEMENT
void AudioSource::queueInputBuffer_l(MediaBuffer *buffer, int64_t timeUs) {
    const size_t bufferSize = buffer->range_length();
    const size_t frameSize = mRecord->frameSize();
    const int64_t timestampUs =
                mPrevSampleTimeUs +
                    ((1000000LL * (bufferSize / frameSize)) +
                        (mSampleRate >> 1)) / mSampleRate;

    if (mNumFramesReceived == 0) {
        buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
    }

    buffer->meta_data()->setInt64(kKeyTime, mPrevSampleTimeUs);
    buffer->meta_data()->setInt64(kKeyDriftTime, timeUs - mInitialReadTimeUs);
    mPrevSampleTimeUs = timestampUs;
    mNumFramesReceived += bufferSize / frameSize;
    mBuffersReceived.push_back(buffer);
    mFrameAvailableCondition.signal();
}
#endif
void AudioSource::trackMaxAmplitude(int16_t *data, int nSamples) {
    for (int i = nSamples; i > 0; --i) {
        int16_t value = *data++;
        if (value < 0) {
            value = -value;
        }
        if (mMaxAmplitude < value) {
            mMaxAmplitude = value;
        }
    }
}

int16_t AudioSource::getMaxAmplitude() {
    // First call activates the tracking.
    if (!mTrackMaxAmplitude) {
        mTrackMaxAmplitude = true;
    }
    int16_t value = mMaxAmplitude;
    mMaxAmplitude = 0;
    ALOGV("max amplitude since last call: %d", value);
    return value;
}

#ifdef MTK_AOSP_ENHANCEMENT //qiushi modify gap reduction
void AudioSource::queueInputBuffer_l(MediaBuffer *buffer, int64_t timeUs) {
    const size_t bufferSize = buffer->range_length();
    const size_t frameSize = mRecord->frameSize();

    if (mNumFramesReceived == 0) {
        buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
        ALOGD("queueInputBuffer_l,first fram kKeyAnchorTime =%" PRId64 "",mStartTimeUs);
    }
    mNumFramesReceived += bufferSize / frameSize;
    const int64_t timestampUs =  mStartTimeUs + ((1000000LL * mNumFramesReceived) + (mSampleRate >> 1)) / mSampleRate;
    ALOGV("queueInputBuffer_l,containing %" PRId64 " frams in this buffer,mPrevSampleTimeUs( %" PRId64 " )," \
            "receiving drift timeUs( %" PRId64 " ),new calculated timestampUs( %" PRId64 " ),", \
            mNumFramesReceived, mPrevSampleTimeUs, timeUs - mInitialReadTimeUs, timestampUs);
    buffer->meta_data()->setInt64(kKeyTime, mPrevSampleTimeUs);
    buffer->meta_data()->setInt64(kKeyDriftTime, timeUs - mInitialReadTimeUs);
    mPrevSampleTimeUs = timestampUs;

    mBuffersReceived.push_back(buffer);
    mFrameAvailableCondition.signal();
}


//MTK80721 HDRecord 2011-12-23
//#ifdef MTK_AUDIO_HD_REC_SUPPORT
AudioSource::AudioSource(
        audio_source_t inputSource, const String16 &opPackageName,
        uint32_t sampleRate, String8 Params, uint32_t channelCount, uint32_t outSampleRate)
    : mRecord(NULL),
      mStarted(false),
      mSampleRate(sampleRate),
      mOutSampleRate(outSampleRate > 0 ? outSampleRate : sampleRate),
      mTrackMaxAmplitude(false),
      mStartTimeUs(0),
      mMaxAmplitude(0),
      mPrevSampleTimeUs(0),
      mFirstSampleTimeUs(-1ll),
      mInitialReadTimeUs(0),
      mNumFramesReceived(0),
      mNumClientOwnedBuffers(0) {

    ALOGI("sampleRate: %u, outSampleRate: %u, channelCount: %u",
            sampleRate, outSampleRate, channelCount);
    CHECK(channelCount == 1 || channelCount == 2);
    CHECK(sampleRate > 0);

    size_t minFrameCount;
    status_t status = AudioRecord::getMinFrameCount(&minFrameCount,
                                           sampleRate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           audio_channel_in_mask_from_count(channelCount));
    if (status == OK) {
        // make sure that the AudioRecord callback never returns more than the maximum
        // buffer size
        uint32_t frameCount = kMaxBufferSize / sizeof(int16_t) / channelCount;

        // make sure that the AudioRecord total buffer size is large enough
        size_t bufCount = 2;
        while ((bufCount * frameCount) < minFrameCount) {
            bufCount++;
        }
        int iframecount = bufCount * frameCount;
        iframecount >>=1;

        ALOGD("minFrameCount=%zu,iframecount=%d,total framecount=%d,notify framecount=%d",
            minFrameCount,iframecount,iframecount*3,iframecount>>1);

        mRecord = new AudioRecord(
                    inputSource, Params, sampleRate, AUDIO_FORMAT_PCM_16_BIT,
                    audio_channel_in_mask_from_count(channelCount),
                    opPackageName,
                    3*iframecount,
                    AudioRecordCallbackFunction,
                    this,
                    iframecount);

        mInitCheck = mRecord->initCheck();
        if (mInitCheck != OK) {
            mRecord.clear();
        }
        ALOGI("AudioSource constructor, getMinFrameCount return minFrameCount =%zu",minFrameCount);
        ALOGI("AudioSource constructor, buffer requirment: frameCount,=%d,bufCount =%zu,mInitCheck=%d",\
                frameCount,bufCount,mInitCheck);
    } else {
        mInitCheck = status;
        ALOGI("AudioSource constructor, getMinFrameCount fail !!!,mInitCheck=%d",mInitCheck);
    }
}


#endif //qiushi modify gap reduction


}  // namespace android
