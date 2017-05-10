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
#define LOG_TAG "ARTPConnection"
#include <utils/Log.h>

#include "ARTPAssembler.h"
#include "ARTPConnection.h"

#include "ARTPSource.h"
#include "ASessionDescription.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#ifdef MTK_AOSP_ENHANCEMENT
#include <sys/ioctl.h>
#include "APacketSource.h"
#include "AnotherPacketSource.h" //for bitrate-adaptation
#endif // #ifdef MTK_AOSP_ENHANCEMENT

namespace android {

static const size_t kMaxUDPSize = 1500;

static uint16_t u16at(const uint8_t *data) {
    return data[0] << 8 | data[1];
}

static uint32_t u32at(const uint8_t *data) {
    return u16at(data) << 16 | u16at(&data[2]);
}

static uint64_t u64at(const uint8_t *data) {
    return (uint64_t)(u32at(data)) << 32 | u32at(&data[4]);
}

// static
const int64_t ARTPConnection::kSelectTimeoutUs = 1000ll;
#ifdef MTK_AOSP_ENHANCEMENT
static int64_t kAccessUnitTimeoutUs = ARTPSource::kAccessUnitTimeoutUs;
static int64_t kCheckAliveInterval = 500000ll;
static int64_t kAccessUnitTimeoutUsMargin = 500000ll;
static int64_t kRRInterval = 4000000ll;
static int64_t kRRIntervalBitrateAdap = 2000000ll;
static int64_t kInjectPollInterval = 100000ll;
#endif // #ifdef MTK_AOSP_ENHANCEMENT

struct ARTPConnection::StreamInfo {
    int mRTPSocket;
    int mRTCPSocket;
    sp<ASessionDescription> mSessionDesc;
    size_t mIndex;
    sp<AMessage> mNotifyMsg;
    KeyedVector<uint32_t, sp<ARTPSource> > mSources;

    int64_t mNumRTCPPacketsReceived;
    int64_t mNumRTPPacketsReceived;
    struct sockaddr_in mRemoteRTCPAddr;

    bool mIsInjected;
#ifdef MTK_AOSP_ENHANCEMENT
    bool mCheckPending;
    int32_t mCheckGeneration;
    int64_t mLastPacketTimeUs;
    bool mTimeUpdated;
    uint32_t mRTPSeqNo;
    AString mCName;
    bool mIsSSRCSet;
    int32_t mSSRC;
    size_t mNaduFrequence;
    sp<APacketSource> mPacketSource;
    uint8_t mBitAdaptSentRRCount;
    //sp<AnotherPacketSource> mAnotherPacketSource;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
};

ARTPConnection::ARTPConnection(uint32_t flags)
    : mFlags(flags),
      mPollEventPending(false),
      mLastReceiverReportTimeUs(-1) {
}

ARTPConnection::~ARTPConnection() {
}

void ARTPConnection::addStream(
        int rtpSocket, int rtcpSocket,
        const sp<ASessionDescription> &sessionDesc,
        size_t index,
        const sp<AMessage> &notify,

#ifdef MTK_AOSP_ENHANCEMENT
        bool injected, ARTPConnectionParam* connParam)
#else
        bool injected)
#endif // #ifdef MTK_AOSP_ENHANCEMENT
{
    sp<AMessage> msg = new AMessage(kWhatAddStream, this);
    msg->setInt32("rtp-socket", rtpSocket);
    msg->setInt32("rtcp-socket", rtcpSocket);
    msg->setObject("session-desc", sessionDesc);
    msg->setSize("index", index);
    msg->setMessage("notify", notify);
    msg->setInt32("injected", injected);
#ifdef MTK_AOSP_ENHANCEMENT
    setConnParam(connParam, msg);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    msg->post();
}

void ARTPConnection::removeStream(int rtpSocket, int rtcpSocket) {
    sp<AMessage> msg = new AMessage(kWhatRemoveStream, this);
    msg->setInt32("rtp-socket", rtpSocket);
    msg->setInt32("rtcp-socket", rtcpSocket);
    msg->post();
}

static void bumpSocketBufferSize(int s) {
    int size = 256 * 1024;
    CHECK_EQ(setsockopt(s, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)), 0);
}

// static
#ifdef MTK_AOSP_ENHANCEMENT
void ARTPConnection::MakePortPair(
        int *rtpSocket, int *rtcpSocket, unsigned *rtpPort, int min, int max)
#else
void ARTPConnection::MakePortPair(
        int *rtpSocket, int *rtcpSocket, unsigned *rtpPort)
#endif
{
    *rtpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK_GE(*rtpSocket, 0);

    bumpSocketBufferSize(*rtpSocket);

    *rtcpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK_GE(*rtcpSocket, 0);

    bumpSocketBufferSize(*rtcpSocket);

#ifdef MTK_AOSP_ENHANCEMENT
    int start = (min + 1) & ~1;
    int range = max - start;
    if (range > 0)
        start += rand() % range;
    start &= ~1;
    for (int port = start; port < max + 1; port += 2)
#else
    /* rand() * 1000 may overflow int type, use long long */
    unsigned start = (unsigned)((rand()* 1000ll)/RAND_MAX) + 15550;
    start &= ~1;

    for (unsigned port = start; port < 65536; port += 2)
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    {
        struct sockaddr_in addr;
        memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(*rtpSocket,
                 (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            continue;
        }

        addr.sin_port = htons(port + 1);

        if (bind(*rtcpSocket,
                 (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
            *rtpPort = port;
            return;
        }
    }

    TRESPASS();
}

void ARTPConnection::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatAddStream:
        {
            onAddStream(msg);
            break;
        }

        case kWhatRemoveStream:
        {
            onRemoveStream(msg);
            break;
        }

        case kWhatPollStreams:
        {
            onPollStreams();
            break;
        }

        case kWhatInjectPacket:
        {
            onInjectPacket(msg);
            break;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        case kWhatStartCheckAlives:
        {
            onStartCheckAlives();
            break;
        }

        case kWhatStopCheckAlives:
        {
            onStopCheckAlives();
            break;
        }

        case kWhatCheckAlive:
        {
            onCheckAlive(msg);
            break;
        }

        case kWhatSeqUpdate:
        {
            onSetHighestSeqNumber(msg);
            break;
        }

        case kWhatInjectPollStreams:
        {
            onPostInjectEvent();
            break;
        }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        default:
        {
            TRESPASS();
            break;
        }
    }
}

void ARTPConnection::onAddStream(const sp<AMessage> &msg) {
    mStreams.push_back(StreamInfo());
    StreamInfo *info = &*--mStreams.end();

    int32_t s;
    CHECK(msg->findInt32("rtp-socket", &s));
    info->mRTPSocket = s;
    CHECK(msg->findInt32("rtcp-socket", &s));
    info->mRTCPSocket = s;

    int32_t injected;
    CHECK(msg->findInt32("injected", &injected));

    info->mIsInjected = injected;

    sp<RefBase> obj;
    CHECK(msg->findObject("session-desc", &obj));
    info->mSessionDesc = static_cast<ASessionDescription *>(obj.get());

    CHECK(msg->findSize("index", &info->mIndex));
#ifdef MTK_AOSP_ENHANCEMENT
    setStreamInfo(msg, info);
#endif
    CHECK(msg->findMessage("notify", &info->mNotifyMsg));

    info->mNumRTCPPacketsReceived = 0;
    info->mNumRTPPacketsReceived = 0;
    memset(&info->mRemoteRTCPAddr, 0, sizeof(info->mRemoteRTCPAddr));

    if (!injected) {
        postPollEvent();
#ifdef MTK_AOSP_ENHANCEMENT
    } else {
        postInjectEvent();
#endif
    }
}

void ARTPConnection::onRemoveStream(const sp<AMessage> &msg) {
    int32_t rtpSocket, rtcpSocket;
    CHECK(msg->findInt32("rtp-socket", &rtpSocket));
    CHECK(msg->findInt32("rtcp-socket", &rtcpSocket));

    List<StreamInfo>::iterator it = mStreams.begin();
    while (it != mStreams.end()
           && (it->mRTPSocket != rtpSocket || it->mRTCPSocket != rtcpSocket)) {
        ++it;
    }

    if (it == mStreams.end()) {
        return;
    }

    mStreams.erase(it);
}

void ARTPConnection::postPollEvent() {
    if (mPollEventPending) {
        return;
    }

    sp<AMessage> msg = new AMessage(kWhatPollStreams, this);
    msg->post();

    mPollEventPending = true;
}

void ARTPConnection::onPollStreams() {
    mPollEventPending = false;

    if (mStreams.empty()) {
        return;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = kSelectTimeoutUs;

    fd_set rs;
    FD_ZERO(&rs);

    int maxSocket = -1;
    for (List<StreamInfo>::iterator it = mStreams.begin();
         it != mStreams.end(); ++it) {
        if ((*it).mIsInjected) {
            continue;
        }

        FD_SET(it->mRTPSocket, &rs);
        FD_SET(it->mRTCPSocket, &rs);

        if (it->mRTPSocket > maxSocket) {
            maxSocket = it->mRTPSocket;
        }
        if (it->mRTCPSocket > maxSocket) {
            maxSocket = it->mRTCPSocket;
        }
    }

    if (maxSocket == -1) {
        return;
    }

    int res = select(maxSocket + 1, &rs, NULL, NULL, &tv);
#ifdef MTK_AOSP_ENHANCEMENT
    if (res < 0) {
        ALOGE("select error %d, stop streaming", errno);
    }
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    if (res > 0) {
        List<StreamInfo>::iterator it = mStreams.begin();
        while (it != mStreams.end()) {
            if ((*it).mIsInjected) {
                ++it;
                continue;
            }

            status_t err = OK;
            if (FD_ISSET(it->mRTPSocket, &rs)) {
                err = receive(&*it, true);
            }
            if (err == OK && FD_ISSET(it->mRTCPSocket, &rs)) {
                err = receive(&*it, false);
            }

            if (err == -ECONNRESET) {
                // socket failure, this stream is dead, Jim.

                ALOGW("failed to receive RTP/RTCP datagram.");
                it = mStreams.erase(it);
                continue;
            }

            ++it;
        }
    }

#ifdef MTK_AOSP_ENHANCEMENT
    sendRR();
#else
    int64_t nowUs = ALooper::GetNowUs();
    if (mLastReceiverReportTimeUs <= 0
            || mLastReceiverReportTimeUs + 5000000ll <= nowUs) {
        sp<ABuffer> buffer = new ABuffer(kMaxUDPSize);
        List<StreamInfo>::iterator it = mStreams.begin();
        while (it != mStreams.end()) {
            StreamInfo *s = &*it;

            if (s->mIsInjected) {
                ++it;
                continue;
            }

            if (s->mNumRTCPPacketsReceived == 0) {
                // We have never received any RTCP packets on this stream,
                // we don't even know where to send a report.
                ++it;
                continue;
            }

            buffer->setRange(0, 0);

            for (size_t i = 0; i < s->mSources.size(); ++i) {
                sp<ARTPSource> source = s->mSources.valueAt(i);

                source->addReceiverReport(buffer);

                if (mFlags & kRegularlyRequestFIR) {
                    source->addFIR(buffer);
                }
            }

            if (buffer->size() > 0) {
                ALOGV("Sending RR...");

                ssize_t n;
                do {
                    n = sendto(
                        s->mRTCPSocket, buffer->data(), buffer->size(), 0,
                        (const struct sockaddr *)&s->mRemoteRTCPAddr,
                        sizeof(s->mRemoteRTCPAddr));
                } while (n < 0 && errno == EINTR);

                if (n <= 0) {
                    ALOGW("failed to send RTCP receiver report (%s).",
                         n == 0 ? "connection gone" : strerror(errno));

                    it = mStreams.erase(it);
                    continue;
                }

                CHECK_EQ(n, (ssize_t)buffer->size());

                mLastReceiverReportTimeUs = nowUs;
            }

            ++it;
        }
    }
#endif
    if (!mStreams.empty()) {
        postPollEvent();
    }
}

status_t ARTPConnection::receive(StreamInfo *s, bool receiveRTP) {
    ALOGV("receiving %s", receiveRTP ? "RTP" : "RTCP");

    CHECK(!s->mIsInjected);

#ifdef MTK_AOSP_ENHANCEMENT
    int size = getReadSize(s, receiveRTP);
    sp<ABuffer> buffer = new ABuffer(size);
    socklen_t remoteAddrLen =
        (s->mNumRTCPPacketsReceived == 0)
        ? sizeof(s->mRemoteRTCPAddr) : 0;
#else
    sp<ABuffer> buffer = new ABuffer(65536);

    socklen_t remoteAddrLen =
        (!receiveRTP && s->mNumRTCPPacketsReceived == 0)
        ? sizeof(s->mRemoteRTCPAddr) : 0;
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    ssize_t nbytes;
    do {
        nbytes = recvfrom(
            receiveRTP ? s->mRTPSocket : s->mRTCPSocket,
            buffer->data(),
            buffer->capacity(),
            0,
            remoteAddrLen > 0 ? (struct sockaddr *)&s->mRemoteRTCPAddr : NULL,
            remoteAddrLen > 0 ? &remoteAddrLen : NULL);
    } while (nbytes < 0 && errno == EINTR);

#ifdef MTK_AOSP_ENHANCEMENT
    if (receiveRTP && s->mNumRTCPPacketsReceived == 0) {
        int port = ntohs(s->mRemoteRTCPAddr.sin_port);
        s->mRemoteRTCPAddr.sin_port = htons(++port);
    }
#endif
    if (nbytes <= 0) {
        return -ECONNRESET;
    }

    buffer->setRange(0, nbytes);

    // ALOGI("received %d bytes.", buffer->size());

    status_t err;
    if (receiveRTP) {
#ifdef MTK_AOSP_ENHANCEMENT
        // touch on  RTP packets
        s->mLastPacketTimeUs = ALooper::GetNowUs();
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        err = parseRTP(s, buffer);
    } else {
        err = parseRTCP(s, buffer);
    }

    return err;
}

status_t ARTPConnection::parseRTP(StreamInfo *s, const sp<ABuffer> &buffer) {
    if (s->mNumRTPPacketsReceived++ == 0) {
        sp<AMessage> notify = s->mNotifyMsg->dup();
        notify->setInt32("first-rtp", true);
        notify->post();
    }

    size_t size = buffer->size();

    if (size < 12) {
        // Too short to be a valid RTP header.
        return -1;
    }

    const uint8_t *data = buffer->data();

    if ((data[0] >> 6) != 2) {
        // Unsupported version.
        return -1;
    }

    if (data[0] & 0x20) {
        // Padding present.

        size_t paddingLength = data[size - 1];

        if (paddingLength + 12 > size) {
            // If we removed this much padding we'd end up with something
            // that's too short to be a valid RTP header.
            return -1;
        }

        size -= paddingLength;
    }

    int numCSRCs = data[0] & 0x0f;

    size_t payloadOffset = 12 + 4 * numCSRCs;

    if (size < payloadOffset) {
        // Not enough data to fit the basic header and all the CSRC entries.
        return -1;
    }

    if (data[0] & 0x10) {
        // Header eXtension present.

        if (size < payloadOffset + 4) {
            // Not enough data to fit the basic header, all CSRC entries
            // and the first 4 bytes of the extension header.

            return -1;
        }

        const uint8_t *extensionData = &data[payloadOffset];

        size_t extensionLength =
            4 * (extensionData[2] << 8 | extensionData[3]);

        if (size < payloadOffset + 4 + extensionLength) {
            return -1;
        }

        payloadOffset += 4 + extensionLength;
    }

    uint32_t srcId = u32at(&data[8]);

#ifdef MTK_AOSP_ENHANCEMENT
    if( (data[1]&0x7f) == 73){
        ALOGW("playload type is 73(Reserved for RTCP conflict avoidance), not supported, ignore it");
        return OK;
    }
    sp<ARTPSource> source = findSource(s, srcId);
    if (source == NULL) {
        ALOGE("found no source by srcId %u", srcId);
        return OK;
    }
#else
    sp<ARTPSource> source = findSource(s, srcId);
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    uint32_t rtpTime = u32at(&data[4]);

    sp<AMessage> meta = buffer->meta();
    meta->setInt32("ssrc", srcId);
    meta->setInt32("rtp-time", rtpTime);
    meta->setInt32("PT", data[1] & 0x7f);
    meta->setInt32("M", data[1] >> 7);

    buffer->setInt32Data(u16at(&data[2]));
    buffer->setRange(payloadOffset, size - payloadOffset);

    source->processRTPPacket(buffer);

    return OK;
}

status_t ARTPConnection::parseRTCP(StreamInfo *s, const sp<ABuffer> &buffer) {
    if (s->mNumRTCPPacketsReceived++ == 0) {
        sp<AMessage> notify = s->mNotifyMsg->dup();
        notify->setInt32("first-rtcp", true);
        notify->post();
    }

    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    while (size > 0) {
        if (size < 8) {
            // Too short to be a valid RTCP header
            return -1;
        }

        if ((data[0] >> 6) != 2) {
            // Unsupported version.
            return -1;
        }

        if (data[0] & 0x20) {
            // Padding present.

            size_t paddingLength = data[size - 1];

            if (paddingLength + 12 > size) {
                // If we removed this much padding we'd end up with something
                // that's too short to be a valid RTP header.
                return -1;
            }

            size -= paddingLength;
        }

        size_t headerLength = 4 * (data[2] << 8 | data[3]) + 4;

        if (size < headerLength) {
            // Only received a partial packet?
            return -1;
        }

        switch (data[1]) {
            case 200:
            {
                parseSR(s, data, headerLength);
                break;
            }

            case 201:  // RR
            case 202:  // SDES
            case 204:  // APP
                break;

            case 205:  // TSFB (transport layer specific feedback)
            case 206:  // PSFB (payload specific feedback)
                // hexdump(data, headerLength);
                break;

            case 203:
            {
                parseBYE(s, data, headerLength);
                break;
            }

            default:
            {
                ALOGW("Unknown RTCP packet type %u of size %zu",
                     (unsigned)data[1], headerLength);
                break;
            }
        }

        data += headerLength;
        size -= headerLength;
    }

    return OK;
}

status_t ARTPConnection::parseBYE(
        StreamInfo *s, const uint8_t *data, size_t size) {
    size_t SC = data[0] & 0x3f;

    if (SC == 0 || size < (4 + SC * 4)) {
        // Packet too short for the minimal BYE header.
        return -1;
    }

    uint32_t id = u32at(&data[4]);

    sp<ARTPSource> source = findSource(s, id);
#ifdef MTK_AOSP_ENHANCEMENT
    if (source == NULL)
        return OK;
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    source->byeReceived();

    return OK;
}

status_t ARTPConnection::parseSR(
        StreamInfo *s, const uint8_t *data, size_t size) {
    size_t RC = data[0] & 0x1f;

    if (size < (7 + RC * 6) * 4) {
        // Packet too short for the minimal SR header.
        return -1;
    }

    uint32_t id = u32at(&data[4]);
    uint64_t ntpTime = u64at(&data[8]);
    uint32_t rtpTime = u32at(&data[16]);

#if 0
    ALOGI("XXX timeUpdate: ssrc=0x%08x, rtpTime %u == ntpTime %.3f",
         id,
         rtpTime,
         (ntpTime >> 32) + (double)(ntpTime & 0xffffffff) / (1ll << 32));
#endif

    sp<ARTPSource> source = findSource(s, id);

#ifdef MTK_AOSP_ENHANCEMENT
    if (source == NULL)
        return 0;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    source->timeUpdate(rtpTime, ntpTime);

    return 0;
}

sp<ARTPSource> ARTPConnection::findSource(StreamInfo *info, uint32_t srcId) {
#ifdef MTK_AOSP_ENHANCEMENT
    if (info->mIsSSRCSet && srcId != (uint32_t)info->mSSRC) {
        ALOGW("ignore invalid ssrc %x, expect %x", srcId, info->mSSRC);
        return NULL;
    }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    sp<ARTPSource> source;
    ssize_t index = info->mSources.indexOfKey(srcId);
    if (index < 0) {
        index = info->mSources.size();

        source = new ARTPSource(
                srcId, info->mSessionDesc, info->mIndex, info->mNotifyMsg);

        info->mSources.add(srcId, source);
#ifdef MTK_AOSP_ENHANCEMENT
        onRecvNewSsrc(info, srcId, source);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    } else {
        source = info->mSources.valueAt(index);
    }

    return source;
}

void ARTPConnection::injectPacket(int index, const sp<ABuffer> &buffer) {
    sp<AMessage> msg = new AMessage(kWhatInjectPacket, this);
    msg->setInt32("index", index);
    msg->setBuffer("buffer", buffer);
    msg->post();
}

void ARTPConnection::onInjectPacket(const sp<AMessage> &msg) {
    int32_t index;
    CHECK(msg->findInt32("index", &index));

    sp<ABuffer> buffer;
    CHECK(msg->findBuffer("buffer", &buffer));

    List<StreamInfo>::iterator it = mStreams.begin();
    while (it != mStreams.end()
           && it->mRTPSocket != index && it->mRTCPSocket != index) {
        ++it;
    }

    if (it == mStreams.end()) {
#ifdef MTK_AOSP_ENHANCEMENT
        ALOGE("receive wrong index(socket id), ignore this packet");
        return;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        TRESPASS();
    }

    StreamInfo *s = &*it;

    if (it->mRTPSocket == index) {
#ifdef MTK_AOSP_ENHANCEMENT
        // touch on  RTP packets
        s->mLastPacketTimeUs = ALooper::GetNowUs();
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        parseRTP(s, buffer);
    } else {
        parseRTCP(s, buffer);
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
void ARTPConnection::sendRR() {
    int64_t nowUs = ALooper::GetNowUs();
    if (mLastReceiverReportTimeUs <= 0
            || mLastReceiverReportTimeUs + kRRInterval <= nowUs) {
        List<StreamInfo>::iterator it = mStreams.begin();
        while (it != mStreams.end()) {
            // for rtp over rtsp ...
            sp<ABuffer> buffer = new ABuffer(kMaxUDPSize);
            StreamInfo *s = &*it;

            if (s->mIsInjected) {
                buffer->data()[0] = 0x24;
                buffer->data()[1] = s->mRTCPSocket;
            }

            if (s->mNumRTPPacketsReceived == 0 && s->mNumRTCPPacketsReceived == 0) {
                // We have never received any RTCP packets on this stream,
                // we don't even know where to send a report.
                ++it;
                continue;
            }

            bool b_needSendNADU = needSendNADU(s);

            if (s->mIsInjected) {
                buffer->setRange(4, 0);
            } else {
                buffer->setRange(0, 0);
            }

            for (size_t i = 0; i < s->mSources.size(); ++i) {
                sp<ARTPSource> source = s->mSources.valueAt(i);

                source->addReceiverReport(buffer);
                source->addSDES(s->mCName, buffer);

                if(b_needSendNADU){
                    addNADUApp(source, s, buffer);
                }

                if (mFlags & kRegularlyRequestFIR) {
                    source->addFIR(buffer);
                }
            }

            if (buffer->size() > 0) {
                ALOGV("Sending RR...");
                if (s->mIsInjected) {
                    postRecvReport(s, buffer);
                } else {
                    ssize_t n;
                    do {
                        n = sendto(
                            s->mRTCPSocket, buffer->data(), buffer->size(), 0,
                            (const struct sockaddr *)&s->mRemoteRTCPAddr,
                            sizeof(s->mRemoteRTCPAddr));
                    } while (n < 0 && errno == EINTR);

                    if (n <= 0) {
                        ALOGW("failed to send RTCP receiver report (%s).",
                             n == 0 ? "connection gone" : strerror(errno));
                        mLastReceiverReportTimeUs = nowUs;
                        ++it;
                        continue;
                    }

                    if (n != (ssize_t)buffer->size()) {
                        ALOGW("Sending RR error: sent bytes %d, expected bytes %zu, errno %d",
                                (int)n, buffer->size(), errno);
                    }
                }
                mLastReceiverReportTimeUs = nowUs;
            }

            ++it;
        }
    }

    if (!mStreams.empty()) {
        postPollEvent();
    }
}


void ARTPConnection::setConnParam(ARTPConnectionParam* connParam, sp<AMessage> &msg) {
    if(connParam) {
        msg->setInt32("ssrc", connParam->mSSRC);
        msg->setPointer("apacket-source",(connParam->mAPacketSource).get());
        msg->setSize("naduFreq",connParam->mNaduFreq);
    } else {
        ALOGE("addStream,connParam pointer is NULL");
    }
}


void ARTPConnection::setStreamInfo(const sp<AMessage> &msg, StreamInfo *info) {
    info->mTimeUpdated = false;

    struct sockaddr_in addr;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    info->mCName.setTo("stagefright@");
    info->mCName.append(inet_ntoa(addr.sin_addr));
      msg->findInt32("ssrc", &info->mSSRC);
    info->mIsSSRCSet = (info->mSSRC == -1) ? false : true;

    info->mNaduFrequence = 0;
    msg->findSize("naduFreq",&info->mNaduFrequence);
    if((info->mNaduFrequence) > 0){
        //mSupportBitrateAdapt = true;
        kRRInterval = kRRIntervalBitrateAdap; //change the sent RR frequency
    }

    ALOGI("onAddStream,info->mIndex=%zu,info->mNaduFrequence=%zu",info->mIndex,info->mNaduFrequence);

    void* pTempSource = NULL;
    APacketSource* pApacketSource = NULL;
    msg->findPointer("apacket-source",&pTempSource);
    pApacketSource = (APacketSource*)pTempSource;
    info->mPacketSource = pApacketSource;
    info->mBitAdaptSentRRCount = 0;
    info->mCheckGeneration =0; //init

}

void ARTPConnection::onRecvNewSsrc(StreamInfo *info, uint32_t srcId, sp<ARTPSource> source) {
    ALOGI("new source ssrc %x", srcId);
    info->mIsSSRCSet = true;
    info->mSSRC = srcId;
    if (info->mTimeUpdated) {
        ALOGI("RTP comes back later than response of PLAY, do a timeUpdate");
        source->setHighestSeqNumber(info->mRTPSeqNo);
    }
}

int ARTPConnection::getReadSize(StreamInfo *s, bool receiveRTP) {
    // save the space
    int fd = receiveRTP ? s->mRTPSocket : s->mRTCPSocket;
    int size = 65536;
    int ret = ioctl(fd, FIONREAD, &size);
    if (ret < 0 || size == 0) {
        ALOGE("ret %d err %d size %d", ret, errno, size);
        return 65536;
    }
    return size;
}

void ARTPConnection::postRecvReport(StreamInfo *s, sp<ABuffer> &buffer) {
    int size = buffer->size();
    buffer->setRange(0, 4 + size);
    buffer->data()[2] = (size & 0xff00) >> 8;
    buffer->data()[3] = (size & 0xff);

    sp<AMessage> notify = s->mNotifyMsg->dup();
    notify->setInt32("receiver-report", true);
    notify->setObject("buffer", buffer);
    notify->post();
}

void ARTPConnection::addNADUApp(sp<ARTPSource> source, StreamInfo *s, sp<ABuffer> buffer) {
    //if(!((s->mAnotherPacketSource).get())){
    if(mAnotherPacketSourceMap.isEmpty()){
         source->addNADUApp(s->mPacketSource,buffer);
        ALOGD("SendRR,stagefright mode");
    }
    else{
        size_t trackIndex;
        (s->mNotifyMsg)->findSize("track-index", &trackIndex);

        source->addNADUApp(mAnotherPacketSourceMap.valueFor(trackIndex),buffer);
        ALOGD("SendRR,Nuplayer mode,track-index=%zu",trackIndex);
    }
}

bool ARTPConnection::needSendNADU(StreamInfo *s) {
    bool b_needSendNADU = false;
    if(s->mNaduFrequence > 0 && (++(s->mBitAdaptSentRRCount) == s->mNaduFrequence) )
    {
        b_needSendNADU = true;
        s->mBitAdaptSentRRCount = 0;
    }
    return b_needSendNADU;
}

void ARTPConnection::startCheckAlives() {
    (new AMessage(kWhatStartCheckAlives, this))->post();
}

void ARTPConnection::stopCheckAlives() {
    (new AMessage(kWhatStopCheckAlives, this))->post();
}

void ARTPConnection::onStartCheckAlives() {
    List<StreamInfo>::iterator it = mStreams.begin();
    while (it != mStreams.end()) {
        it->mCheckPending = false;
        it->mLastPacketTimeUs = ALooper::GetNowUs();

        sp<AMessage> check = new AMessage(kWhatCheckAlive, this);
        it->mCheckGeneration++;
        check->setInt32("generation", it->mCheckGeneration);
        check->setSize("stream-index", it->mIndex);
        check->post(kAccessUnitTimeoutUs);
        ALOGD("start check alives %d %zu", it->mCheckGeneration, it->mIndex);
        ++it;
    }
}

void ARTPConnection::onStopCheckAlives() {
    List<StreamInfo>::iterator it = mStreams.begin();
    while (it != mStreams.end()) {
        ALOGD("stop check alives %d %zu", it->mCheckGeneration, it->mIndex);
        (it++)->mCheckPending = true;
    }
}

void ARTPConnection::onCheckAlive(const sp<AMessage> &msg) {
    size_t streamIndex;
    int32_t generation;
    CHECK(msg->findSize("stream-index", &streamIndex));
    CHECK(msg->findInt32("generation", &generation));

    List<StreamInfo>::iterator info = mStreams.begin();
    while (info != mStreams.end()) {
        if (info->mIndex == streamIndex)
            break;
        info++;
    }
    if (info == mStreams.end())
        return;

    if (info->mCheckPending || generation != info->mCheckGeneration)
        return;

    int64_t nowUs = ALooper::GetNowUs();
    if (nowUs - info->mLastPacketTimeUs <= kAccessUnitTimeoutUs) {
        msg->post(kCheckAliveInterval);
        return;
    }

    int64_t maxExpectedTimeoutUs = 0;
    for (size_t j = 0; j < info->mSources.size(); ++j) {
        sp<ARTPSource> source = info->mSources.valueAt(j);
        int64_t timeoutUs = source->getExpectedTimeoutUs();
        if (timeoutUs > maxExpectedTimeoutUs)
            maxExpectedTimeoutUs = timeoutUs;
    }

    if (maxExpectedTimeoutUs > 0) {
        // in case we have received a packet of big duration
        // we may receive nothing during the period, don't trigger EOS
        int64_t diff = maxExpectedTimeoutUs + kAccessUnitTimeoutUsMargin - nowUs;
        if (diff > 0) {
            ALOGI("ARTPConnection: don't trigger timeout, our expect timeout is"
                   " %lld, now: %lld", (long long)maxExpectedTimeoutUs, (long long)nowUs);
            msg->post(diff);
            return;
        }
    }

    for (size_t j = 0; j < info->mSources.size(); ++j) {
        sp<ARTPSource> source = info->mSources.valueAt(j);
        source->flushRTPPackets();
    }

    msg->post(kCheckAliveInterval);
    info->mLastPacketTimeUs = nowUs;

    sp<AMessage> notify = info->mNotifyMsg->dup();
    notify->setInt32("stream-timeout", true);
    notify->post();
}

void ARTPConnection::setHighestSeqNumber(int socket, uint32_t rtpSeq) {
    ALOGD("socket %d set rtp-seq %d", socket, rtpSeq);
    sp<AMessage> msg = new AMessage(kWhatSeqUpdate, this);
    msg->setInt32("rtp-socket", socket);
    msg->setInt32("rtp-seq", rtpSeq);
    msg->post();
}

void ARTPConnection::onSetHighestSeqNumber(const sp<AMessage> &msg) {
    int socket;
    uint32_t rtpSeq;
    CHECK(msg->findInt32("rtp-socket", &socket));
    CHECK(msg->findInt32("rtp-seq", (int32_t*)&rtpSeq));

    List<StreamInfo>::iterator it = mStreams.begin();
    while (it != mStreams.end()) {
        StreamInfo &info = *it++;
        if (info.mRTPSocket != socket)
            continue;

        info.mTimeUpdated = true;
        info.mRTPSeqNo = rtpSeq;
        for (size_t j = 0; j < info.mSources.size(); ++j) {
            sp<ARTPSource> source = info.mSources.valueAt(j);
            source->setHighestSeqNumber(rtpSeq);
        ALOGD("source %zu set rtpseq %d", j, rtpSeq);
        }
    }
}

void ARTPConnection::postInjectEvent() {
    if (mPollEventPending) {
        return;
    }

    sp<AMessage> msg = new AMessage(kWhatInjectPollStreams, this);
    msg->post(kInjectPollInterval);

    mPollEventPending = true;
}

void ARTPConnection::onPostInjectEvent() {
    mPollEventPending = false;
    postInjectEvent();
    sendRR();
}

void ARTPConnection::setAnotherPacketSource(int iMyHandlerTrackIndex, sp<AnotherPacketSource> pAnotherPacketSource){
    /*
    List<StreamInfo>::iterator it = mStreams.begin();
    ALOGI("setAnotherPacketSource,mStreams.size=%d",mStreams.size());
    while (it != mStreams.end()){

        size_t trackIndex;
        //notifymsg 'accu' contain the corresponding track-index of myhandler tracks
        (it->mNotifyMsg)->findSize("track-index", &trackIndex);
        ALOGI("setAnotherPacketSource,trackIndex=%d",trackIndex);
        if(trackIndex == iMyHandlerTrackIndex )
            break;

        ++it;
    }

    if (it == mStreams.end()) {
        TRESPASS();
    }

    StreamInfo *s = &*it;
    s->mAnotherPacketSource = pAnotherPacketSource;
    */

    mAnotherPacketSourceMap.add(iMyHandlerTrackIndex,pAnotherPacketSource);
    ALOGI("setAnotherPacketSource,iMyHandlerTrackIndex=%d",iMyHandlerTrackIndex);
}

#endif // #ifdef MTK_AOSP_ENHANCEMENT

}  // namespace android

