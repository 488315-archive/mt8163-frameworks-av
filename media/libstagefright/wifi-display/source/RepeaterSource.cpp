//#define LOG_NDEBUG 0
#define LOG_TAG "RepeaterSource"
#include <utils/Log.h>

#include "RepeaterSource.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>

#ifdef MTK_AOSP_ENHANCEMENT
#include <media/stagefright/foundation/AUtils.h>

#include <cutils/properties.h>
#include "DataPathTrace.h"

#define ATRACE_TAG ATRACE_TAG_VIDEO
#define REPEATERSOURCE_ATRACE_START_TRIGGER 0
#define REPEATERSOURCE_ATRACE_END_TRIGGER 1
#include <utils/Trace.h>

//for display debug
#ifdef MTK_AOSP_ENHANCEMENT
#include <ui/gralloc_extra.h>
#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>

#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/GraphicBufferExtra.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>

#include <../../include/SoftwareRenderer.h>

#endif
#define WFD_LOGI(fmt,arg...) ALOGI(fmt,##arg)

#define VP_ONLY_MIN_COUNT 50
#else
#define WFD_LOGI(fmt,arg...)
#endif

namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
static const nsecs_t kNanosIn1s = 1000000000;

///-->for display debug
SoftwareRenderer* mSoftRenderer;
sp<Surface> mSurface ;
sp<SurfaceComposerClient> mComposerClient ;
sp<SurfaceControl> mControl;
bool mEnableRender ;
///<---///-->for display debug

///-->for display debug
void displayABuffer(MediaBuffer *buffer){
    mEnableRender = false;
    char logRpt[PROPERTY_VALUE_MAX];
    if (property_get("media.wfd.render", logRpt, NULL)){
        mEnableRender = atoi(logRpt);
        ALOGV("log mEnableRender   %d",mEnableRender);
    }

    sp<AMessage> format = new AMessage();

    intptr_t handle_ptr = (intptr_t)(buffer->data());

    buffer_handle_t _handle = *((buffer_handle_t *)(handle_ptr + 4));
    gralloc_extra_ion_sf_info_t ext_info;

    // get token (sequence) from buffer handle
    gralloc_extra_query(_handle,
    GRALLOC_EXTRA_GET_IOCTL_ION_SF_INFO, &ext_info);

    gralloc_buffer_info_t bufInfo;
    gralloc_extra_getBufInfo(_handle, &bufInfo);

    format->setInt32("color-format", OMX_COLOR_FormatYUV420Planar);//HAL_PIXEL_FORMAT_YV12
    format->setInt32("stride", bufInfo.stride);
    format->setInt32("slice-height", bufInfo.height);


    if(mEnableRender && mSoftRenderer == NULL){
        mComposerClient = new SurfaceComposerClient;
        CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);

        sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(
                                    ISurfaceComposer::eDisplayIdMain));

        DisplayInfo info;
        SurfaceComposerClient::getDisplayInfo(display, &info);
        ssize_t displayWidth = bufInfo.stride/2;//info.w;
        ssize_t displayHeight = bufInfo.height/2;//info.h;

        ALOGD("display is %zd x %zd\n", displayWidth, displayHeight);

        mControl =mComposerClient->createSurface(
                    String8("A Surface"),
                    displayWidth,
                    displayHeight,
                    bufInfo.format,
                    0);

        CHECK(mControl != NULL);
        CHECK(mControl->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        CHECK_EQ(mControl->setLayer(INT_MAX), (status_t)OK);
        CHECK_EQ(mControl->show(), (status_t)OK);
        mControl->setPosition(10, 10);
        SurfaceComposerClient::closeGlobalTransaction();

        mSurface = mControl->getSurface();

        CHECK(mSurface != NULL);

        mSoftRenderer = new SoftwareRenderer(mSurface);
        ALOGI("enable display debug");
    }
    if(!mEnableRender && mSoftRenderer != NULL){
        mSurface.clear();
        mComposerClient.clear();
        mControl.clear();
        free(mSoftRenderer);
        mSoftRenderer = NULL;
        ALOGI("close display debug");
    }
    if(mEnableRender && mSoftRenderer != NULL){
        // dump to file with final path and file type
        void* ptr = NULL;
        Rect  mRect(bufInfo.stride, bufInfo.height);
        int err = GraphicBufferMapper::getInstance().lock(_handle,
                                                  GraphicBuffer::USAGE_SW_READ_OFTEN,
                                                  mRect,
                                                  &ptr);
        ALOGI(" format %x,W %d stride %d  H %d, VStride %d ptr va %p ,err %d",
        bufInfo.format,bufInfo.width,bufInfo.stride,bufInfo.height,bufInfo.vertical_stride,ptr,err);

        mSoftRenderer->render(ptr, bufInfo.stride*bufInfo.height*3/2,  ALooper::GetNowUs(),
                                (ALooper::GetNowUs())*1000 ,NULL, format);
        GraphicBufferMapper::getInstance().unlock(_handle);
    }
}

void RepeaterSource::read_pro(int64_t timeUs){
    ATRACE_INT64("Repeater, TS:  ms", timeUs/1000);
    int32_t usedTimes=0;
    if(mBuffer->meta_data()->findInt32('used', &usedTimes)){
        mBuffer->meta_data()->setInt32('used', usedTimes+1);
        mReadOutCountRpt++;
        if(mEableLogRepeatUseCount){
            WFD_LOGI("[video buffer] mBuffer=%p usedCount=%d,refcnt= %d ", mBuffer,usedTimes+1,mBuffer->refcount());
        }

    }else{
#ifdef MTK_AOSP_ENHANCEMENT
        ATRACE_NAME("WFD_VFrame");
#endif
        mBuffer->meta_data()->setInt32('used', 1);
        mReadOutCountNew++;
    }

    int64_t gotTime,delayTime,readTime;
    sp<WfdDebugInfo> debugInfo= defaultWfdDebugInfo();
    if( mBuffer->meta_data()->findInt64('RpIn', &gotTime) ){
        int64_t nowUs = ALooper::GetNowUs();
        if(usedTimes > 0) {
            delayTime = 0;
            gotTime = nowUs/1000;
            ALOGV("[WFDP]this buffer has beed used for %d times",usedTimes);
        }else{
            delayTime = (nowUs - gotTime*1000)/1000;
        }

            debugInfo->addTimeInfoByKey(1, timeUs, "RpIn", gotTime);
            debugInfo->addTimeInfoByKey(1, timeUs, "RpDisPlay", usedTimes);
            debugInfo->addTimeInfoByKey(1, timeUs, "DeMs", delayTime);
            debugInfo->addTimeInfoByKey(1, timeUs, "RpOt", nowUs/1000);
         if( mBuffer->meta_data()->findInt64('RtMs', &readTime) ){
            debugInfo->addTimeInfoByKey(1, timeUs, "RpReadTimeMs", readTime);
        }

    }
    int32_t latencyToken = 0;
    if(mBuffer->meta_data()->findInt32(kKeyWFDLatency, &latencyToken)){
        debugInfo->addTimeInfoByKey(1, timeUs, "LatencyToken", latencyToken);
#ifdef MTK_AOSP_ENHANCEMENT
        int32_t usedTimes=0;
        if(mBuffer->meta_data()->findInt32('used', &usedTimes) && usedTimes == 1){
            ATRACE_ASYNC_END("STG-MPR", latencyToken);
        }
        ATRACE_ASYNC_BEGIN("MPR-CNV", latencyToken);
#endif
    }

    int64_t nowUs = ALooper::GetNowUs();
    int64_t totalTimeUs = nowUs - mStartCountTime ;
    if( (mReadOutCountNew+mReadOutCountRpt ) % 30 == 0){
        ALOGI("[WFD_P]:FPS %lld, read FPS %lld, repeat/new /drop/total =  %d/%d/%d/%d, pll fps %.2f, scenario %d ",
            mReadInCount*1000000ll / totalTimeUs,
            mReadOutCountNew*1000000ll / totalTimeUs,
            mReadOutCountRpt,mReadOutCountNew,mDropFrames,mReadOutCountNew+mReadOutCountRpt,
            mVideoRateHz, scenario);

        mReadInCount = 0;
        mReadOutCountRpt = 0;
        mReadOutCountNew = 0;
        mDropFrames = 0;
        mStartCountTime = -1;
    }




    //workaround for encoder init slow
    if(mFrameCount == 1)
    {
        mFrameCount = 6;
        ALOGI("read deley 5frames times");
    }
}


void RepeaterSource::read_fps(int64_t /*timeUs*/,int64_t readTimeUs){

    mBuffer->meta_data()->setInt64('RpIn', (mLastBufferUpdateUs / 1000));
    mBuffer->meta_data()->setInt64('RtMs', (readTimeUs / 1000));
    WFD_LOGI("[WFDP][video]read MediaBuffer %p,readtime=%lld ms , fps = %.2f",mBuffer, (long long)(readTimeUs/1000), mVideoRateHz);

    int32_t latencyToken = 0;
    if(mBuffer->meta_data()->findInt32(kKeyWFDLatency, &latencyToken)){
#ifdef MTK_AOSP_ENHANCEMENT
        ATRACE_ASYNC_BEGIN("STG-MPR", latencyToken);
#endif
    }


    if(mStartCountTime <  0){
        mStartCountTime = mLastBufferUpdateUs;
        mReadInCount = 0;
        mReadOutCountRpt = 0;
        mReadOutCountNew = 0;
    }
    mReadInCount++;
}
#ifdef MTK_USE_BUFFERQUEUE
void RepeaterSource::clearBuffers() {
     if (!mBuffers.empty()) {
         WFD_LOGI("releasing mBuffers");
         for (size_t i = 0; i < mBuffers.size(); ++i){
             WFD_LOGI("releasing mBuffers[%zu]:%p", i, mBuffers[i]);
             mBuffers[i]->release();
         }
         mBuffers.clear();
    }
}
#endif
status_t RepeaterSource::stop_l() {
    CHECK(mStarted);

    {
        Mutex::Autolock autoLock(mLock);

        mStopping = true;

        WFD_LOGI("stopping");
#ifdef MTK_USE_BUFFERQUEUE
        clearBuffers();
#else
        if (mBuffer != NULL) {
            WFD_LOGI("releasing mbuf %p refcnt= %d ", mBuffer,mBuffer->refcount());
            mBuffer->release();
            mBuffer = NULL;
        }
#endif
    }

    status_t err = mSource->stop();
    WFD_LOGI("stopped source ");


    if (mLooper != NULL) {
        mLooper->stop();
        mLooper.clear();
        mReflector.clear();
    }
    WFD_LOGI("stopped repeater looper ");
    mStarted = false;
    WFD_LOGI("stopped");
    return err;
}

#endif

RepeaterSource::RepeaterSource(const sp<MediaSource> &source, double rateHz)
    : mStarted(false),
      mSource(source),
      mRateHz(rateHz),
      mBuffer(NULL),
      mResult(OK),
      mLastBufferUpdateUs(-1ll),
      mStartTimeUs(-1ll),
      mFrameCount(0) {
      WFD_LOGI("FrameRate %.2f",mRateHz);
#ifdef MTK_AOSP_ENHANCEMENT
    mStartCountTime  = -1;
    mReadInCount   = 0;
    mReadOutCountNew  = 0;
    mDropFrames = 0;
    mReadOutCountRpt  = 0;
    mEableLogRepeatUseCount = false;
    mStopping = false;
    char logRpt[PROPERTY_VALUE_MAX];
    if (property_get("media.wfd.log.rpt", logRpt, NULL)){
        mEableLogRepeatUseCount = atoi(logRpt);
        ALOGD("log Repeat count %d",mEableLogRepeatUseCount);
    }

    scenario_enable = true;

    char scenario_en[PROPERTY_VALUE_MAX];
    if (property_get("media.wfd.scenario.mode", scenario_en, NULL)){
        if(!strcmp(scenario_en, "1")){
            scenario_enable = true;
        }
        else if(!strcmp(scenario_en, "0")){
            scenario_enable = false;
        }
    } else {
        property_set("media.wfd.scenario.mode", "1");
    }

    ALOGI("WFD_property scenario_enable=%d", scenario_enable);
    scenario = UI_ONLY;
    mLastGotVideoTimeMs = -1;
    mVideoRateHz = rateHz;
    mPLL.reset(rateHz);
    mLastTimeUs = -1ll;
#ifdef MTK_USE_BUFFERQUEUE
    mBuffers.clear();
#endif

#endif
}

RepeaterSource::~RepeaterSource() {
    ALOGD("~RepeaterSource");
    CHECK(!mStarted);
}

double RepeaterSource::getFrameRate() const {
    return mRateHz;
}

void RepeaterSource::setFrameRate(double rateHz) {
    Mutex::Autolock autoLock(mLock);

    if (rateHz == mRateHz) {
        return;
    }

    if (mStartTimeUs >= 0ll) {
        int64_t nextTimeUs = mStartTimeUs + (mFrameCount * 1000000ll) / mRateHz;
        mStartTimeUs = nextTimeUs;
        mFrameCount = 0;
    }
    mRateHz = rateHz;
#ifdef MTK_AOSP_ENHANCEMENT
    mLastGotVideoTimeMs = -1;
    scenario = UI_ONLY;
    mVideoRateHz = rateHz;
    mPLL.reset(rateHz);
    mLastTimeUs = -1ll;
    mDropFrames = 0;
#endif
}

status_t RepeaterSource::start(MetaData *params) {
    CHECK(!mStarted);
#ifdef MTK_AOSP_ENHANCEMENT
    mPLL.restart();
    mStopping = false;
    vp_only_count = 0;
    mDropFrames = 0;
#endif
#ifdef MTK_AOSP_ENHANCEMENT
    ATRACE_CALL();
#endif
    WFD_LOGI("start++");

    status_t err = mSource->start(params);

    if (err != OK) {
         WFD_LOGI("surfaceMediaSource start err");
        return err;
    }
#if defined(MTK_USE_BUFFERQUEUE) && defined(MTK_AOSP_ENHANCEMENT)
    clearBuffers();
#endif
    mBuffer = NULL;
    mResult = OK;
    mStartTimeUs = -1ll;
    mFrameCount = 0;

    mLooper = new ALooper;
    mLooper->setName("repeater_looper");
    mLooper->start();

    mReflector = new AHandlerReflector<RepeaterSource>(this);
    mLooper->registerHandler(mReflector);

    postRead();

    mStarted = true;
    WFD_LOGI("start ---");
    return OK;
}

status_t RepeaterSource::stop() {

#ifdef MTK_AOSP_ENHANCEMENT
    mDropFrames = 0;
    return stop_l();
#endif

    CHECK(mStarted);

    WFD_LOGI("stopping");

    status_t err = mSource->stop();

    if (mLooper != NULL) {
        mLooper->stop();
        mLooper.clear();

        mReflector.clear();
    }

    if (mBuffer != NULL) {
        WFD_LOGI("releasing mbuf %p", mBuffer);
        mBuffer->release();
        mBuffer = NULL;
    }


    WFD_LOGI("stopped");

    mStarted = false;

    return err;
}

sp<MetaData> RepeaterSource::getFormat() {
    return mSource->getFormat();
}

status_t RepeaterSource::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    int64_t seekTimeUs;
#ifdef MTK_AOSP_ENHANCEMENT
    bool bRepeatVideoBuffer = false;

#endif
    ReadOptions::SeekMode seekMode;
    CHECK(options == NULL || !options->getSeekTo(&seekTimeUs, &seekMode));

    for (;;) {
        int64_t bufferTimeUs = -1ll;

        if (mStartTimeUs < 0ll) {
            Mutex::Autolock autoLock(mLock);
            while ((mLastBufferUpdateUs < 0ll || mBuffer == NULL)
                    && mResult == OK) {
                mCondition.wait(mLock);
            }

            ALOGV("now resuming.");
            mStartTimeUs = ALooper::GetNowUs();
            bufferTimeUs = mStartTimeUs;
#ifdef MTK_AOSP_ENHANCEMENT
            mLastTimeUs= bufferTimeUs;
            previousScenario = UI_ONLY;
            timeStamp = mLastTimeUs;
#endif
         WFD_LOGI("now resuming.mStartTimeUs=%lld ms",(long long)(mStartTimeUs/1000));
        } else {

            bufferTimeUs = mStartTimeUs + (mFrameCount * 1000000ll) / mRateHz;
#ifdef MTK_AOSP_ENHANCEMENT
        if(scenario_enable == true){
            Mutex::Autolock autoLock(mLock);
            //protect scenario
            if(previousScenario != scenario){
                ALOGI("scenario change at the read begin, 2 fast %d %d", previousScenario, scenario);
            }


            if (scenario == VP_ONLY){
                double sampleRate = mVideoRateHz + mVideoRateHz;
                bufferTimeUs = mLastTimeUs+ 1000000ll /sampleRate;
            }
            else{
                if(mFrameCount == 6){
                    bufferTimeUs = mLastTimeUs + (1000000ll /mRateHz) * 6ll;
                }else{
                    bufferTimeUs = mLastTimeUs + 1000000ll /mRateHz;
                }
                timeStamp = bufferTimeUs;
            }
            previousScenario = scenario;
        }
#endif

            int64_t nowUs = ALooper::GetNowUs();
            int64_t delayUs = bufferTimeUs - nowUs;

            if (delayUs > 0ll) {
                usleep(delayUs);
            }

        }

        bool stale = false;

        {

            Mutex::Autolock autoLock(mLock);
            if (mResult != OK) {
                //CHECK(mBuffer == NULL); //In the case exit when no video buffer is arrived
                WFD_LOGI("read return error %d",mResult);
                return mResult;
            }

#if defined(MTK_USE_BUFFERQUEUE) && defined(MTK_AOSP_ENHANCEMENT)
            ///M: get no used or the latest buffer from mBuffers
            int32_t index = mBuffers.size()-1;
            for(size_t i=0; i<mBuffers.size(); i++) {
                int32_t used = 0;
                if(!mBuffers[i]->meta_data()->findInt32('used', &used)){
                    WFD_LOGI("[read video buffer] mBuffer[%zu]=%p, used=%d, refcnt= %d ", i,mBuffers[i],used,mBuffers[i]->refcount());
                    index = i;
                    break;
                }
            }

            if (index >= 0) {
                mBuffer = mBuffers[index];
            } else {
                WFD_LOGI("read return error");
                return UNKNOWN_ERROR;
            }
 #endif

 #ifdef MTK_AOSP_ENHANCEMENT
            if(scenario_enable == true){
            if((previousScenario == VP_ONLY) && (scenario == VP_ONLY)){
                int64_t videoTimeUs;
                if(mBuffer->meta_data()->findInt64('RSUs', &videoTimeUs)){
                    if(videoTimeUs == mLastSendTimeUs){
                        ALOGI("VP repeat buffer");
                        bRepeatVideoBuffer = true;
                    }else{
                        int64_t nowTimeUs = ALooper::GetNowUs();
                        int64_t gap = 1000000ll /mVideoRateHz;
                        int64_t tolerance;
/*
                        if(mVideoRateHz < 26){
                            tolerance = gap/2 + 1000;
                        }else{
                            tolerance = gap/3;
                        }
*/
                        tolerance = gap/2 + 1000;
                        timeStamp = timeStamp + gap;
                        int64_t delta;
                        if(timeStamp >= nowTimeUs){
                            delta = timeStamp - nowTimeUs;
                            if(delta > tolerance){
                                ALOGI("timeStamp delta 2 big fps %.2f, %lld us, %lld us +1000", mVideoRateHz,(long long)delta, (long long)tolerance);
                                timeStamp =  nowTimeUs;

                                bufferTimeUs = nowTimeUs;
                            }else{
//                                ALOGI("timeStamp is normal >");
                            }
                        }else{
                            delta = nowTimeUs- timeStamp;
                            if(delta > tolerance){
                                timeStamp = nowTimeUs;
                                ALOGI("timeStamp delta smaller %.2f, %lld us, %lld us", mVideoRateHz, (long long)delta, (long long)tolerance);
                                bufferTimeUs = nowTimeUs;
                            }else{
//                                ALOGI("timeStamp is normal <");
                            }
                        }
                        //if(delta > gap*1.1) || (delta < gap*0.9) ){
                        //    ALOGI("VP TS ");
                        //}
                        //
                        ALOGI("VP new buffer timestamp %0.2f, %lld us", mVideoRateHz, (long long)timeStamp);
                    }
                    mLastSendTimeUs = videoTimeUs;
                }else{
                    WFD_LOGI("VP_ONLY but no timestamp, error");
                }
            }
            else if (scenario != previousScenario){
                int64_t timeUs = ALooper::GetNowUs();
//                if(timeStamp > timeUs){
//                    WFD_LOGI("scenario change, time stampe is bigger");
//                }
                WFD_LOGI("scenario change from %d to %d, ts %lld us, sys time %lld us ",previousScenario, scenario, (long long)timeStamp, (long long)timeUs);
                timeStamp = timeUs;
                bufferTimeUs = timeUs;
                if(scenario == VP_ONLY){
                    //In this case, we update the mLastSendTimeUs at the 1st VP_ONLY buffer
                    //to aviod the second VP_ONLY Buffer is the repeat one
                    int64_t videoTimeUs;
                    if(mBuffer->meta_data()->findInt64('RSUs', &videoTimeUs)){
                        mLastSendTimeUs = videoTimeUs;
                    }
                }
            }else{
//                ALOGI("VP UI and UI_VP timestamp %lld us", timeStamp);
                bRepeatVideoBuffer = false;
             }
            }
#endif


#if SUSPEND_VIDEO_IF_IDLE
            int64_t nowUs = ALooper::GetNowUs();
            if (nowUs - mLastBufferUpdateUs > 1000000ll) {
                mLastBufferUpdateUs = -1ll;
                stale = true;
                WFD_LOGI("[video buffer] has not  been updated than >1S");
            } else
#endif
            {
#ifdef MTK_AOSP_ENHANCEMENT
                if(!bRepeatVideoBuffer)
#endif
                    mBuffer->add_ref();


                *buffer = mBuffer;
#ifdef MTK_AOSP_ENHANCEMENT
                if(scenario_enable == true){
                    //WFD_LOGI("output time stamp %lld us ",timeStamp);
                    (*buffer)->meta_data()->setInt64(kKeyTime, timeStamp);
                }
                else{
                    (*buffer)->meta_data()->setInt64(kKeyTime, bufferTimeUs);
                }
#else
                (*buffer)->meta_data()->setInt64(kKeyTime, bufferTimeUs);
#endif

                ++mFrameCount;
#ifdef MTK_AOSP_ENHANCEMENT
                mLastTimeUs = bufferTimeUs;
                if(!bRepeatVideoBuffer)
                        read_pro(bufferTimeUs);
                previousScenario = scenario;
#endif
            }

        }

        if (!stale) {
            break;
        }

        mStartTimeUs = -1ll;
        mFrameCount = 0;
        ALOGI("now dormant");
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if(bRepeatVideoBuffer){
        return 1;
    }else{
        return OK;
    }
#else
    return OK;
#endif

}

void RepeaterSource::postRead() {

#ifdef MTK_AOSP_ENHANCEMENT
     if(mStopping){
        ALOGI("Stopping now, ingore read commad");
        return;
     }
#endif
    (new AMessage(kWhatRead, mReflector))->post();
}

void RepeaterSource::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRead:
        {
            MediaBuffer *buffer;
#ifdef MTK_AOSP_ENHANCEMENT
            ATRACE_BEGIN("Repeater, KWhatRead");
            int64_t startUs = ALooper::GetNowUs();
#endif
            status_t err = mSource->read(&buffer);
#ifdef MTK_AOSP_ENHANCEMENT
            if(err != OK){
                ALOGE("[SMS is ERROR EOS now!!!],should not err %d",err);
                return;
            }

            displayABuffer(buffer);
#endif
            ALOGV("read mbuf %p", buffer);


            Mutex::Autolock autoLock(mLock);

#ifdef MTK_AOSP_ENHANCEMENT
        if(mStopping){
            if (buffer != NULL) {
                WFD_LOGI("read while stopping buffer=%p,refcnt= %d ",
                    buffer,static_cast<MediaBuffer *>(buffer)->refcount());
                buffer->release();
                buffer = NULL;
            }
        return;
        }
#endif



#if  defined(MTK_USE_BUFFERQUEUE) && defined(MTK_AOSP_ENHANCEMENT)
            ///M: remove the used buffers
            if (!mBuffers.empty()) {
                for(size_t i=0; i<mBuffers.size();) {
                    int32_t used=0;
                    if(mBuffers[i]->meta_data()->findInt32('used', &used) && used >= 1){
                        WFD_LOGI("[video buffer] mBuffer=%p removed from buffers,used=%d,refcnt= %d ", mBuffers[i], used, mBuffers[i]->refcount());
                        mBuffers[i]->release();
                        mBuffers.removeAt(i);
                        continue;
                    }
                    i++;
               }
            }

            ///M:SurfaceMediaSource default has 4 buffers,
            ///M:so mBuffers size must not more than 3(because variable buffer hold 1)
            if (mBuffers.size() > 2) {
                int32_t used = 0;
                if(!mBuffers[0]->meta_data()->findInt32('used', &used)){
                   mDropFrames++;
                   ATRACE_INT("RptSrc_DropFrm", REPEATERSOURCE_ATRACE_START_TRIGGER);
                   ATRACE_INT("RptSrc_DropFrm", REPEATERSOURCE_ATRACE_END_TRIGGER);
                   WFD_LOGI("[video buffer] mBuffer=%p is not used before release,used=%d,refcnt= %d ", mBuffers[0],used,mBuffers[0]->refcount());
                }
                mBuffers[0]->release();
                mBuffers.removeAt(0);
            }
            mBuffers.push_back(buffer);
#else
            if (mBuffer != NULL) {
#ifdef MTK_AOSP_ENHANCEMENT
            int32_t used=0;
            if(!mBuffer->meta_data()->findInt32('used', &used) ){
                mDropFrames++;
                ATRACE_INT("RptSrc_DropFrm", REPEATERSOURCE_ATRACE_START_TRIGGER);
                ATRACE_INT("RptSrc_DropFrm", REPEATERSOURCE_ATRACE_END_TRIGGER);
                WFD_LOGI("[video buffer] mBuffer=%p is not used before release,used=%d,refcnt= %d ", mBuffer,used,mBuffer->refcount());
            }
#endif
                mBuffer->release();
                mBuffer = NULL;
            }
#endif
            mBuffer = buffer;
            mResult = err;
            mLastBufferUpdateUs = ALooper::GetNowUs();
#ifdef MTK_AOSP_ENHANCEMENT
        int32_t videoTimeMs = 0;
        if(scenario_enable == true){
        if (mBuffer->meta_data()->findInt32(kKeyVideoTime, &videoTimeMs)) {
            int64_t videoTimeUs = 0;
            if (videoTimeMs != 0) {
                    if (mLastGotVideoTimeMs != videoTimeMs){

                        if((mVideoRateHz > 0 ) && ((videoTimeMs - mLastGotVideoTimeMs) > (1200ll/mVideoRateHz))){
                            WFD_LOGI("VP time diff 2 large %dms, %dms", videoTimeMs, mLastGotVideoTimeMs);
                        }else if((mVideoRateHz > 0  ) && ((videoTimeMs - mLastGotVideoTimeMs) < (800ll/mVideoRateHz))){
                            WFD_LOGI("VP time diff 2 small %dms, %dms", videoTimeMs, mLastGotVideoTimeMs);
                        }

                    videoTimeUs = videoTimeMs * 1000;
                    //ALOGD("Get kKeyVideoTime %d ms", videoTimeMs);
                    mBuffer->meta_data()->setInt64('RSUs', videoTimeUs);
                    nsecs_t videoPeriod = mPLL.addSample(videoTimeUs*1000);
                    mVideoRateHz = (double)kNanosIn1s/videoPeriod;
//                        ALOGD("video fps %.2f, UP_ONLY", mVideoRateHz);
                        if (mVideoRateHz > mRateHz){
                            mVideoRateHz = mRateHz;
                        }
                        mLastGotVideoTimeMs = videoTimeMs;
                        if(vp_only_count > VP_ONLY_MIN_COUNT){
                            //ALOGD("vp_only_count change to VP_ONLY %d",vp_only_count);
                            scenario = VP_ONLY;
                        }else{
                            vp_only_count++;
                            ALOGD("vp_only_count %d",vp_only_count);
                        }
                    }else{
                    //ALOGD("Multiple frame w/ video time %d, set mVideoRateHz %.2f->%.2f",
//                    videoTimeMs, mVideoRateHz, mRateHz);
                        mVideoRateHz = mRateHz;
                        scenario = VP_UI;
                        vp_only_count = 0;
//                        ALOGD("video fps %.2f, VP_UI", mVideoRateHz);
                    //ALOGD("eason 2");
                    }
                }
                else {
                    //ALOGD("eason ui");
                    scenario = UI_ONLY;
                    mLastGotVideoTimeMs = -1;
                    mVideoRateHz = mRateHz;
                    mPLL.reset(mRateHz);
                }
            }else        {
                ALOGE("Surfaceflinger send a data without timestamp!!!!");
            }
        }

        read_fps(mLastBufferUpdateUs,mLastBufferUpdateUs-startUs);
#endif

            mCondition.broadcast();

            if (err == OK) {
                postRead();
            }
#ifdef MTK_AOSP_ENHANCEMENT
            ATRACE_END();
#endif
            break;
        }

        default:
            TRESPASS();
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
void RepeaterSource::wakeUp(bool bExit) {
#else
void RepeaterSource::wakeUp() {
#endif
    ALOGD("wakeUp");
    Mutex::Autolock autoLock(mLock);

#ifdef MTK_AOSP_ENHANCEMENT
    if(bExit){
        ALOGD("exit and wakeup");
        mCondition.broadcast();
        mResult = ERROR_END_OF_STREAM;
        return;
    }
#endif

#ifdef MTK_USE_BUFFERQUEUE
    if (mLastBufferUpdateUs < 0ll && !mBuffers.empty()) {
#else
    if (mLastBufferUpdateUs < 0ll && mBuffer != NULL) {
#endif
        mLastBufferUpdateUs = ALooper::GetNowUs();
        mCondition.broadcast();
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
template<class T>
static int compare(const T *lhs, const T *rhs) {
    if (*lhs < *rhs) {
        return -1;
    } else if (*lhs > *rhs) {
        return 1;
    } else {
        return 0;
    }
}

static const size_t kMinSamplesToStartPrime = 3;
static const size_t kMinSamplesToStopPrime = RepeaterSource::kHistorySize;
static const size_t kMinSamplesToEstimatePeriod = 3;
static const size_t kMaxSamplesToEstimatePeriod = RepeaterSource::kHistorySize;

static const size_t kPrecision = 12;
static const size_t kErrorThreshold = (1 << (kPrecision * 2)) / 10;
static const int64_t kMultiplesThresholdDiv = 4;            // 25%
static const int64_t kReFitThresholdDiv = 100;              // 1%
static const nsecs_t kMaxAllowedFrameSkip = kNanosIn1s;     // 1 sec
static const nsecs_t kMinPeriod = kNanosIn1s / 120;         // 120Hz
static const nsecs_t kRefitRefreshPeriod = 10 * kNanosIn1s; // 10 sec


RepeaterSource::PLL::PLL()
    : mPeriod(-1),
      mPhase(0),
      mPrimed(false),
      mSamplesUsedForPriming(0),
      mLastTime(-1),
      mNumSamples(0) {
}

void RepeaterSource::PLL::reset(float fps) {
    //test();

    mSamplesUsedForPriming = 0;
    mLastTime = -1;

    // set up or reset video PLL
    if (fps <= 0.f) {
        mPeriod = -1;
        mPrimed = false;
    } else {
        ALOGV("reset at %.1f fps", fps);
        mPeriod = (nsecs_t)(1e9 / fps + 0.5);
        mPrimed = true;
    }

    restart();
}

// reset PLL but keep previous period estimate
void RepeaterSource::PLL::restart() {
    mNumSamples = 0;
    mPhase = -1;
}

bool RepeaterSource::PLL::fit(
        nsecs_t phase, nsecs_t period, size_t numSamplesToUse,
        int64_t *a, int64_t *b, int64_t *err) {
    if (numSamplesToUse > mNumSamples) {
        numSamplesToUse = mNumSamples;
    }

    int64_t sumX = 0;
    int64_t sumXX = 0;
    int64_t sumXY = 0;
    int64_t sumYY = 0;
    int64_t sumY = 0;

    int64_t x = 0; // x usually is in [0..numSamplesToUse)
    nsecs_t lastTime;
    for (size_t i = 0; i < numSamplesToUse; i++) {
        size_t ix = (mNumSamples - numSamplesToUse + i) % kHistorySize;
        nsecs_t time = mTimes[ix];
        if (i > 0) {
            x += divRound(time - lastTime, period);
        }
        // y is usually in [-numSamplesToUse..numSamplesToUse+kRefitRefreshPeriod/kMinPeriod) << kPrecision
        //   ideally in [0..numSamplesToUse), but shifted by -numSamplesToUse during
        //   priming, and possibly shifted by up to kRefitRefreshPeriod/kMinPeriod
        //   while we are not refitting.
        int64_t y = divRound(time - phase, period >> kPrecision);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        sumYY += y * y;
        lastTime = time;
    }

    int64_t div   = numSamplesToUse * sumXX - sumX * sumX;
    if (div == 0) {
        return false;
    }

    int64_t a_nom = numSamplesToUse * sumXY - sumX * sumY;
    int64_t b_nom = sumXX * sumY            - sumX * sumXY;
    *a = divRound(a_nom, div);
    *b = divRound(b_nom, div);
    // don't use a and b directly as the rounding error is significant
    *err = sumYY - divRound(a_nom * sumXY + b_nom * sumY, div);
    ALOGV("fitting[%zu] a=%lld (%.6f), b=%lld (%.6f), err=%lld (%.6f)",
            numSamplesToUse,
            (long long)*a,   (*a / (float)(1 << kPrecision)),
            (long long)*b,   (*b / (float)(1 << kPrecision)),
            (long long)*err, (*err / (float)(1 << (kPrecision * 2))));
    return true;
}

void RepeaterSource::PLL::prime(size_t numSamplesToUse) {
    if (numSamplesToUse > mNumSamples) {
        numSamplesToUse = mNumSamples;
    }
    CHECK(numSamplesToUse >= 3);  // must have at least 3 samples

    // estimate video framerate from deltas between timestamps, and
    // 2nd order deltas
    Vector<nsecs_t> deltas;
    nsecs_t lastTime, firstTime;
    for (size_t i = 0; i < numSamplesToUse; ++i) {
        size_t index = (mNumSamples - numSamplesToUse + i) % kHistorySize;
        nsecs_t time = mTimes[index];
        if (i > 0) {
            if (time - lastTime > kMinPeriod) {
                //ALOGV("delta: %lld", (long long)(time - lastTime));
                deltas.push(time - lastTime);
            }
        } else {
            firstTime = time;
        }
        lastTime = time;
    }
    deltas.sort(compare<nsecs_t>);
    size_t numDeltas = deltas.size();
    if (numDeltas > 1) {
        nsecs_t deltaMinLimit = max(deltas[0] / kMultiplesThresholdDiv, kMinPeriod);
        nsecs_t deltaMaxLimit = deltas[numDeltas / 2] * kMultiplesThresholdDiv;
        for (size_t i = numDeltas / 2 + 1; i < numDeltas; ++i) {
            if (deltas[i] > deltaMaxLimit) {
                deltas.resize(i);
                numDeltas = i;
                break;
            }
        }
        for (size_t i = 1; i < numDeltas; ++i) {
            nsecs_t delta2nd = deltas[i] - deltas[i - 1];
            if (delta2nd >= deltaMinLimit) {
                //ALOGV("delta2: %lld", (long long)(delta2nd));
                deltas.push(delta2nd);
            }
        }
    }

    // use the one that yields the best match
    int64_t bestScore;
    for (size_t i = 0; i < deltas.size(); ++i) {
        nsecs_t delta = deltas[i];
        int64_t score = 0;

        // simplest score: number of deltas that are near multiples
        size_t matches = 0;
        for (size_t j = 0; j < deltas.size(); ++j) {
            nsecs_t err = periodicError(deltas[j], delta);
            if (err < delta / kMultiplesThresholdDiv) {
                ++matches;
            }
        }

        score = matches;
        if (i == 0 || score > bestScore) {
            bestScore = score;
            mPeriod = delta;
            mPhase = firstTime;
        }
    }
    ALOGV("priming[%zu] phase:%lld period:%lld", numSamplesToUse, (long long)mPhase, (long long)mPeriod);
}

nsecs_t RepeaterSource::PLL::addSample(nsecs_t time) {
    if (mLastTime >= 0
            // if time goes backward, or we skipped rendering
            && (time > mLastTime + kMaxAllowedFrameSkip || time < mLastTime)) {
        restart();
    }

    mLastTime = time;
    mTimes[mNumSamples % kHistorySize] = time;
    ++mNumSamples;

    bool doFit = time > mRefitAt;
    if ((mPeriod <= 0 || !mPrimed) && mNumSamples >= kMinSamplesToStartPrime) {
        prime(kMinSamplesToStopPrime);
        ++mSamplesUsedForPriming;
        doFit = true;
    }
    if (mPeriod > 0 && mNumSamples >= kMinSamplesToEstimatePeriod) {
        if (mPhase < 0) {
            // initialize phase to the current render time
            mPhase = time;
            doFit = true;
        } else if (!doFit) {
            int64_t err = periodicError(time - mPhase, mPeriod);
            doFit = err > mPeriod / kReFitThresholdDiv;
        }

        if (doFit) {
            int64_t a, b, err;
            if (!fit(mPhase, mPeriod, kMaxSamplesToEstimatePeriod, &a, &b, &err)) {
                // samples are not suitable for fitting.  this means they are
                // also not suitable for priming.
                ALOGV("could not fit - keeping old period:%lld", (long long)mPeriod);
                return mPeriod;
            }

            mRefitAt = time + kRefitRefreshPeriod;

            mPhase += (mPeriod * b) >> kPrecision;
            mPeriod = (mPeriod * a) >> kPrecision;
            ALOGV("new phase:%lld period:%lld", (long long)mPhase, (long long)mPeriod);

            if (err < (int64_t)kErrorThreshold) {
                if (!mPrimed && mSamplesUsedForPriming >= kMinSamplesToStopPrime) {
                    mPrimed = true;
                }
            } else {
                mPrimed = false;
                mSamplesUsedForPriming = 0;
            }
        }
    }
    return mPeriod;
}
#endif //MTK_AOSP_ENHANCEMENT

}  // namespace android
