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
#define LOG_TAG "ARTSPConnection"
#include <utils/Log.h>

#include "ARTSPConnection.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/base64.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/md5.h>
#include <sys/socket.h>

#ifdef MTK_AOSP_ENHANCEMENT
#include <cutils/properties.h>
#include "ARTPSource.h"
#ifdef CUSTOM_UASTRING_FROM_PROPERTY
#include "custom_prop.h"
#endif
#include <linux/tcp.h>
#define MTK_SEND_FLAG (MSG_NOSIGNAL)
#endif // #ifdef MTK_AOSP_ENHANCEMENT
#include "include/HTTPBase.h"

namespace android {

// static
const int64_t ARTSPConnection::kSelectTimeoutUs = 1000ll;

#ifdef MTK_AOSP_ENHANCEMENT
// 15s is service ANR timeout, we choose 14s for request timeout
const int64_t ARTSPConnection::kRequestTimeout = 60000000ll;
static int64_t kAccessUnitTimeoutUs = ARTPSource::kAccessUnitTimeoutUs;
#else
// static
const AString ARTSPConnection::sUserAgent =
    AStringPrintf("User-Agent: %s\r\n", MakeUserAgent().c_str());
#endif // #ifdef MTK_AOSP_ENHANCEMENT

ARTSPConnection::ARTSPConnection(bool uidValid, uid_t uid)
    : mUIDValid(uidValid),
      mUID(uid),
      mState(DISCONNECTED),
      mAuthType(NONE),
      mSocket(-1),
      mConnectionID(0),
      mNextCSeq(0),
      mReceiveResponseEventPending(false) {
#ifdef MTK_AOSP_ENHANCEMENT
    init();
#endif // #ifdef MTK_AOSP_ENHANCEMENT
}

ARTSPConnection::~ARTSPConnection() {
    if (mSocket >= 0) {
        ALOGE("Connection is still open, closing the socket.");
        if (mUIDValid) {
            HTTPBase::UnRegisterSocketUserTag(mSocket);
            HTTPBase::UnRegisterSocketUserMark(mSocket);
        }
        close(mSocket);
        mSocket = -1;
    }
}

void ARTSPConnection::connect(const char *url, const sp<AMessage> &reply) {
    sp<AMessage> msg = new AMessage(kWhatConnect, this);
    msg->setString("url", url);
    msg->setMessage("reply", reply);
    msg->post();
}

void ARTSPConnection::disconnect(const sp<AMessage> &reply) {
    sp<AMessage> msg = new AMessage(kWhatDisconnect, this);
    msg->setMessage("reply", reply);
    msg->post();
}

void ARTSPConnection::sendRequest(
        const char *request, const sp<AMessage> &reply) {
    sp<AMessage> msg = new AMessage(kWhatSendRequest, this);
    msg->setString("request", request);
    msg->setMessage("reply", reply);
#ifdef MTK_AOSP_ENHANCEMENT
    msg->setInt32("backup-keep-tcp", mForceQuitTCP);
    int keep;
    if (!reply->findInt32("keep-tcp", &keep)) {
        mForceQuitTCP = true;
    }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    msg->post();
}

void ARTSPConnection::observeBinaryData(const sp<AMessage> &reply) {
    sp<AMessage> msg = new AMessage(kWhatObserveBinaryData, this);
    msg->setMessage("reply", reply);
    msg->post();
}

void ARTSPConnection::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatConnect:
            onConnect(msg);
            break;

        case kWhatDisconnect:
            onDisconnect(msg);
            break;

        case kWhatCompleteConnection:
            onCompleteConnection(msg);
            break;

        case kWhatSendRequest:
            onSendRequest(msg);
            break;

        case kWhatReceiveResponse:
            onReceiveResponse();
            break;

        case kWhatObserveBinaryData:
        {
            CHECK(msg->findMessage("reply", &mObserveBinaryMessage));
            break;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        case kWhatTimeout:
        {
            onTimeout(msg);
            break;
        }
        case kWhatInjectPacket:
        {
            onInjectPacket(msg);
            break;
        }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        default:
            TRESPASS();
            break;
    }
}

// static
bool ARTSPConnection::ParseURL(
        const char *url, AString *host, unsigned *port, AString *path,
        AString *user, AString *pass) {
    host->clear();
    *port = 0;
    path->clear();
    user->clear();
    pass->clear();

    if (strncasecmp("rtsp://", url, 7)) {
        return false;
    }

    const char *slashPos = strchr(&url[7], '/');

    if (slashPos == NULL) {
        host->setTo(&url[7]);
        path->setTo("/");
    } else {
        host->setTo(&url[7], slashPos - &url[7]);
        path->setTo(slashPos);
    }

    ssize_t atPos = host->find("@");

    if (atPos >= 0) {
        // Split of user:pass@ from hostname.

        AString userPass(*host, 0, atPos);
        host->erase(0, atPos + 1);

        ssize_t colonPos = userPass.find(":");

        if (colonPos < 0) {
            *user = userPass;
        } else {
            user->setTo(userPass, 0, colonPos);
            pass->setTo(userPass, colonPos + 1, userPass.size() - colonPos - 1);
        }
    }

    const char *colonPos = strchr(host->c_str(), ':');

    if (colonPos != NULL) {
        unsigned long x;
        if (!ParseSingleUnsignedLong(colonPos + 1, &x) || x >= 65536) {
            return false;
        }

        *port = x;

        size_t colonOffset = colonPos - host->c_str();
        size_t trailing = host->size() - colonOffset;
        host->erase(colonOffset, trailing);
    } else {
        *port = 554;
    }

    return true;
}

static status_t MakeSocketBlocking(int s, bool blocking) {
    // Make socket non-blocking.
    int flags = fcntl(s, F_GETFL, 0);

    if (flags == -1) {
        return UNKNOWN_ERROR;
    }

    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }

    flags = fcntl(s, F_SETFL, flags);

    return flags == -1 ? UNKNOWN_ERROR : OK;
}

void ARTSPConnection::onConnect(const sp<AMessage> &msg) {
    ++mConnectionID;

    if (mState != DISCONNECTED) {
        if (mUIDValid) {
            HTTPBase::UnRegisterSocketUserTag(mSocket);
            HTTPBase::UnRegisterSocketUserMark(mSocket);
        }
        close(mSocket);
        mSocket = -1;

        flushPendingRequests();
    }

    mState = CONNECTING;

    AString url;
    CHECK(msg->findString("url", &url));

    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    AString host, path;
    unsigned port;
    if (!ParseURL(url.c_str(), &host, &port, &path, &mUser, &mPass)
            || (mUser.size() > 0 && mPass.size() == 0)) {
        // If we have a user name but no password we have to give up
        // right here, since we currently have no way of asking the user
        // for this information.

        ALOGE("Malformed rtsp url %s", uriDebugString(url).c_str());

        reply->setInt32("result", ERROR_MALFORMED);
        reply->post();

        mState = DISCONNECTED;
        return;
    }

    if (mUser.size() > 0) {
        ALOGV("user = '%s', pass = '%s'", mUser.c_str(), mPass.c_str());
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (!mProxyHost.empty()) {
        ALOGI("connect through proxy %s:%d", mProxyHost.c_str(), mProxyPort);
        host = mProxyHost;
        port = mProxyPort;
    }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    struct hostent *ent = gethostbyname(host.c_str());
    if (ent == NULL) {
        ALOGE("Unknown host %s", host.c_str());

#ifdef MTK_AOSP_ENHANCEMENT
        reply->setInt32("result", -EHOSTUNREACH);
#else
        reply->setInt32("result", -ENOENT);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        reply->post();

        mState = DISCONNECTED;
        return;
    }

    mSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (mUIDValid) {
        HTTPBase::RegisterSocketUserTag(mSocket, mUID,
                                        (uint32_t)*(uint32_t*) "RTSP");
        HTTPBase::RegisterSocketUserMark(mSocket, mUID);
    }

    MakeSocketBlocking(mSocket, false);

    struct sockaddr_in remote;
    memset(remote.sin_zero, 0, sizeof(remote.sin_zero));
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = *(in_addr_t *)ent->h_addr;
    remote.sin_port = htons(port);
#ifdef MTK_AOSP_ENHANCEMENT
    mRemote = remote;
    setSocketFlag(msg);
    ALOGI("connecting %s, %s:%d now",
        host.c_str(), inet_ntoa(remote.sin_addr), port);
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    int err = ::connect(
            mSocket, (const struct sockaddr *)&remote, sizeof(remote));

    reply->setInt32("server-ip", ntohl(remote.sin_addr.s_addr));

    if (err < 0) {
        if (errno == EINPROGRESS) {
            sp<AMessage> msg = new AMessage(kWhatCompleteConnection, this);
            msg->setMessage("reply", reply);
            msg->setInt32("connection-id", mConnectionID);
#ifdef MTK_AOSP_ENHANCEMENT
            msg->setInt64("timestamp", ALooper::GetNowUs());
            ALOGI("connection EINPROGRESS");
#endif // #ifdef MTK_AOSP_ENHANCEMENT
            msg->post();
            return;
        }

        reply->setInt32("result", -errno);
        mState = DISCONNECTED;

        if (mUIDValid) {
            HTTPBase::UnRegisterSocketUserTag(mSocket);
            HTTPBase::UnRegisterSocketUserMark(mSocket);
        }
        close(mSocket);
        mSocket = -1;
    } else {
        reply->setInt32("result", OK);
        mState = CONNECTED;
        mNextCSeq = 1;

        postReceiveReponseEvent();
    }

    reply->post();
}

void ARTSPConnection::performDisconnect() {
    if (mUIDValid) {
        HTTPBase::UnRegisterSocketUserTag(mSocket);
        HTTPBase::UnRegisterSocketUserMark(mSocket);
    }
    close(mSocket);
    mSocket = -1;

    flushPendingRequests();

    mUser.clear();
    mPass.clear();
    mAuthType = NONE;
    mNonce.clear();
#ifdef MTK_AOSP_ENHANCEMENT
    mRealm.clear();
    ALOGI("performDisconnect");
#endif // #ifdef MTK_AOSP_ENHANCEMENT

    mState = DISCONNECTED;
}

void ARTSPConnection::onDisconnect(const sp<AMessage> &msg) {
    if (mState == CONNECTED || mState == CONNECTING) {
        performDisconnect();
    }

    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    reply->setInt32("result", OK);

    reply->post();
}

void ARTSPConnection::onCompleteConnection(const sp<AMessage> &msg) {
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    int32_t connectionID;
    CHECK(msg->findInt32("connection-id", &connectionID));

    if ((connectionID != mConnectionID) || mState != CONNECTING) {
        // While we were attempting to connect, the attempt was
        // cancelled.
        reply->setInt32("result", -ECONNABORTED);
        reply->post();
        return;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = kSelectTimeoutUs;

    fd_set ws;
    FD_ZERO(&ws);
    FD_SET(mSocket, &ws);

    int res = select(mSocket + 1, NULL, &ws, NULL, &tv);
    CHECK_GE(res, 0);

    if (res == 0) {
        // Timed out. Not yet connected.
#ifdef MTK_AOSP_ENHANCEMENT
        if(isConnTimeout(msg))
            return;

        if(mExited)
            return;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        msg->post();
        return;
    }

    int err;
    socklen_t optionLen = sizeof(err);
    CHECK_EQ(getsockopt(mSocket, SOL_SOCKET, SO_ERROR, &err, &optionLen), 0);
    CHECK_EQ(optionLen, (socklen_t)sizeof(err));

    if (err != 0) {
        ALOGE("err = %d (%s)", err, strerror(err));

        reply->setInt32("result", -err);

        mState = DISCONNECTED;
        if (mUIDValid) {
            HTTPBase::UnRegisterSocketUserTag(mSocket);
            HTTPBase::UnRegisterSocketUserMark(mSocket);
        }
        close(mSocket);
        mSocket = -1;
    } else {
        reply->setInt32("result", OK);
        mState = CONNECTED;
        mNextCSeq = 1;

        postReceiveReponseEvent();
    }

    reply->post();
}

void ARTSPConnection::onSendRequest(const sp<AMessage> &msg) {
#ifdef MTK_AOSP_ENHANCEMENT
    int32_t backup;
    if (msg->findInt32("backup-keep-tcp", &backup)) {
        mForceQuitTCP = backup;
    }
#endif
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    if (mState != CONNECTED) {
        reply->setInt32("result", -ENOTCONN);
        reply->post();
        return;
    }

    AString request;
    CHECK(msg->findString("request", &request));

    // Just in case we need to re-issue the request with proper authentication
    // later, stash it away.
    reply->setString("original-request", request.c_str(), request.size());
#ifdef MTK_AOSP_ENHANCEMENT
    // mtk80902: ALPS01139972 - SETUP resp with a wrong Session "xxx\0\0\0.."
    // so the followed \r\n is ignored by find's strstr operation
    if (request.find("\r\n\r\n") < 0) {
        ALOGW("what the hell with this req?");  // seems print str is useless..
        reply->setInt32("result", -EBADMSG);
        reply->post();
        return;
    }
#endif
    addAuthentication(&request);
    addUserAgent(&request);

    // Find the boundary between headers and the body.
    ssize_t i = request.find("\r\n\r\n");
    CHECK_GE(i, 0);

    int32_t cseq = mNextCSeq++;

    AString cseqHeader = "CSeq: ";
    cseqHeader.append(cseq);
    cseqHeader.append("\r\n");

    request.insert(cseqHeader, i + 2);

    ALOGV("request: '%s'", request.c_str());

    size_t numBytesSent = 0;
    while (numBytesSent < request.size()) {
        ssize_t n =
#ifdef MTK_AOSP_ENHANCEMENT
            send(mSocket, request.c_str() + numBytesSent,
                 request.size() - numBytesSent, MTK_SEND_FLAG);
#else
            send(mSocket, request.c_str() + numBytesSent,
                 request.size() - numBytesSent, 0);
#endif

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            performDisconnect();

            if (n == 0) {
                // Server closed the connection.
                ALOGE("Server unexpectedly closed the connection.");

                reply->setInt32("result", ERROR_IO);
                reply->post();
            } else {
                ALOGE("Error sending rtsp request. (%s)", strerror(errno));
                reply->setInt32("result", -errno);
                reply->post();
            }

            return;
        }

        numBytesSent += (size_t)n;
    }

    mPendingRequests.add(cseq, reply);
#ifdef MTK_AOSP_ENHANCEMENT
    postTimeoutMsg(reply, cseq);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
}

void ARTSPConnection::onReceiveResponse() {
    mReceiveResponseEventPending = false;

    if (mState != CONNECTED) {
        return;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = kSelectTimeoutUs;

    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(mSocket, &rs);

    int res = select(mSocket + 1, &rs, NULL, NULL, &tv);

    if (res == 1) {
        MakeSocketBlocking(mSocket, true);

        bool success = receiveRTSPReponse();

        MakeSocketBlocking(mSocket, false);

        if (!success) {
            // Something horrible, irreparable has happened.
            flushPendingRequests();
            return;
        }
    }

    postReceiveReponseEvent();
}

void ARTSPConnection::flushPendingRequests() {
    for (size_t i = 0; i < mPendingRequests.size(); ++i) {
        sp<AMessage> reply = mPendingRequests.valueAt(i);

        reply->setInt32("result", -ECONNABORTED);
        reply->post();
    }

    mPendingRequests.clear();
}

void ARTSPConnection::postReceiveReponseEvent() {
    if (mReceiveResponseEventPending) {
        return;
    }

    sp<AMessage> msg = new AMessage(kWhatReceiveResponse, this);
    msg->post();

    mReceiveResponseEventPending = true;
}

status_t ARTSPConnection::receive(void *data, size_t size) {
    size_t offset = 0;
#ifdef MTK_AOSP_ENHANCEMENT
     while (offset < size && !mExited)
#else
    while (offset < size)
#endif
  {
        ssize_t n = recv(mSocket, (uint8_t *)data + offset, size - offset, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGW("receive %zd errno %d trying %d, force %d", n, errno, mKeepTCPTrying, mForceQuitTCP);
            if (errno == EAGAIN && mKeepTCPTrying && !mForceQuitTCP) {
                continue;
            }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
            performDisconnect();

            if (n == 0) {
                // Server closed the connection.
                ALOGE("Server unexpectedly closed the connection.");
                return ERROR_IO;
            } else {
                ALOGE("Error reading rtsp response. (%s)", strerror(errno));
                return -errno;
            }
        }

        offset += (size_t)n;
    }
#ifdef MTK_AOSP_ENHANCEMENT
     if(offset < size)
              return ERROR_IO;
#endif
    return OK;
}

bool ARTSPConnection::receiveLine(AString *line) {
    line->clear();

    bool sawCR = false;
    for (;;) {
        char c;
        if (receive(&c, 1) != OK) {
            return false;
        }

        if (sawCR && c == '\n') {
            line->erase(line->size() - 1, 1);
            return true;
        } else if (c == '\n') {
            // some reponse line ended with '\n', instead of '\r\n'.
            return true;
        }

        line->append(&c, 1);

        if (c == '$' && line->size() == 1) {
            // Special-case for interleaved binary data.
            return true;
        }

        sawCR = (c == '\r');
    }
}

sp<ABuffer> ARTSPConnection::receiveBinaryData() {
    uint8_t x[3];
    if (receive(x, 3) != OK) {
        return NULL;
    }

    sp<ABuffer> buffer = new ABuffer((x[1] << 8) | x[2]);
    if (receive(buffer->data(), buffer->size()) != OK) {
        return NULL;
    }

    buffer->meta()->setInt32("index", (int32_t)x[0]);

    return buffer;
}

static bool IsRTSPVersion(const AString &s) {
#ifdef MTK_AOSP_ENHANCEMENT
    return s.find("/1.0") >= 0;
#endif
    return s == "RTSP/1.0";
}

bool ARTSPConnection::receiveRTSPReponse() {
    AString statusLine;

    if (!receiveLine(&statusLine)) {
        return false;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if(statusLine.empty()){
        ALOGW("received blank row at the beginning of a response, ignore it.");
        if (!receiveLine(&statusLine)) {
            return false;
        }
    }
#endif


    if (statusLine == "$") {
#ifdef MTK_AOSP_ENHANCEMENT
        mKeepTCPTrying = true;
        sp<ABuffer> buffer = receiveBinaryData();
        mKeepTCPTrying = false;
#else
        sp<ABuffer> buffer = receiveBinaryData();
#endif // #ifdef MTK_AOSP_ENHANCEMENT

        if (buffer == NULL) {
            return false;
        }

        if (mObserveBinaryMessage != NULL) {
            sp<AMessage> notify = mObserveBinaryMessage->dup();
            notify->setBuffer("buffer", buffer);
            notify->post();
        } else {
            ALOGW("received binary data, but no one cares.");
        }

        return true;
    }

    sp<ARTSPResponse> response = new ARTSPResponse;
    response->mStatusLine = statusLine;

    ALOGI("status: %s", response->mStatusLine.c_str());

    ssize_t space1 = response->mStatusLine.find(" ");
    if (space1 < 0) {
        return false;
    }
    ssize_t space2 = response->mStatusLine.find(" ", space1 + 1);
    if (space2 < 0) {
        return false;
    }

    bool isRequest = false;

    if (!IsRTSPVersion(AString(response->mStatusLine, 0, space1))) {
#ifdef MTK_AOSP_ENHANCEMENT
        // mtk80902: ALPS01122651: received mess..
        if (!IsRTSPVersion(
                   AString(
                       response->mStatusLine,
                       space2 + 1,
                       response->mStatusLine.size() - space2 - 1))) {
            ALOGW("what mess r u receiving?");
            return false;
        }
#else
        CHECK(IsRTSPVersion(
                    AString(
                        response->mStatusLine,
                        space2 + 1,
                        response->mStatusLine.size() - space2 - 1)));
#endif
        isRequest = true;

        response->mStatusCode = 0;
    } else {
        AString statusCodeStr(
                response->mStatusLine, space1 + 1, space2 - space1 - 1);

        if (!ParseSingleUnsignedLong(
                    statusCodeStr.c_str(), &response->mStatusCode)
                || response->mStatusCode < 100 || response->mStatusCode > 999) {
            return false;
        }
    }

    AString line;
    ssize_t lastDictIndex = -1;
    for (;;) {
        if (!receiveLine(&line)) {
            break;
        }

        if (line.empty()) {
            break;
        }

        ALOGV("line: '%s'", line.c_str());

        if (line.c_str()[0] == ' ' || line.c_str()[0] == '\t') {
            // Support for folded header values.

            if (lastDictIndex < 0) {
                // First line cannot be a continuation of the previous one.
                return false;
            }

            AString &value = response->mHeaders.editValueAt(lastDictIndex);
            value.append(line);

            continue;
        }

        ssize_t colonPos = line.find(":");
        if (colonPos < 0) {
            // Malformed header line.
            return false;
        }

        AString key(line, 0, colonPos);
        key.trim();
        key.tolower();

        line.erase(0, colonPos + 1);

        lastDictIndex = response->mHeaders.add(key, line);
    }

    for (size_t i = 0; i < response->mHeaders.size(); ++i) {
        response->mHeaders.editValueAt(i).trim();
    }

    unsigned long contentLength = 0;

    ssize_t i = response->mHeaders.indexOfKey("content-length");

    if (i >= 0) {
        AString value = response->mHeaders.valueAt(i);
        if (!ParseSingleUnsignedLong(value.c_str(), &contentLength)) {
            return false;
        }
    }

    if (contentLength > 0) {
        response->mContent = new ABuffer(contentLength);

        if (receive(response->mContent->data(), contentLength) != OK) {
            return false;
        }
    }

    if (response->mStatusCode == 401) {
        if (mAuthType == NONE && mUser.size() > 0
                && parseAuthMethod(response)) {
            ssize_t i;
            CHECK_EQ((status_t)OK, findPendingRequest(response, &i));
            CHECK_GE(i, 0);

            sp<AMessage> reply = mPendingRequests.valueAt(i);
            mPendingRequests.removeItemsAt(i);

            AString request;
            CHECK(reply->findString("original-request", &request));

            sp<AMessage> msg = new AMessage(kWhatSendRequest, this);
            msg->setMessage("reply", reply);
            msg->setString("request", request.c_str(), request.size());

            ALOGI("re-sending request with authentication headers...");
            onSendRequest(msg);

            return true;
        }
    }

    return isRequest
        ? handleServerRequest(response)
        : notifyResponseListener(response);
}

bool ARTSPConnection::handleServerRequest(const sp<ARTSPResponse> &request) {
    // Implementation of server->client requests is optional for all methods
    // but we do need to respond, even if it's just to say that we don't
    // support the method.

    ssize_t space1 = request->mStatusLine.find(" ");
    CHECK_GE(space1, 0);

    AString response;
    response.append("RTSP/1.0 501 Not Implemented\r\n");

    ssize_t i = request->mHeaders.indexOfKey("cseq");

    if (i >= 0) {
        AString value = request->mHeaders.valueAt(i);

        unsigned long cseq;
        if (!ParseSingleUnsignedLong(value.c_str(), &cseq)) {
            return false;
        }

        response.append("CSeq: ");
        response.append(cseq);
        response.append("\r\n");
    }

    response.append("\r\n");

    size_t numBytesSent = 0;
    while (numBytesSent < response.size()) {
        ssize_t n =
#ifdef MTK_AOSP_ENHANCEMENT
            send(mSocket, response.c_str() + numBytesSent,
                 response.size() - numBytesSent, MTK_SEND_FLAG);
#else
            send(mSocket, response.c_str() + numBytesSent,
                 response.size() - numBytesSent, 0);
#endif

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            if (n == 0) {
                // Server closed the connection.
                ALOGE("Server unexpectedly closed the connection.");
            } else {
                ALOGE("Error sending rtsp response (%s).", strerror(errno));
            }

            performDisconnect();

            return false;
        }

        numBytesSent += (size_t)n;
    }

    return true;
}

// static
bool ARTSPConnection::ParseSingleUnsignedLong(
        const char *from, unsigned long *x) {
    char *end;
    *x = strtoul(from, &end, 10);

    if (end == from || *end != '\0') {
        return false;
    }

    return true;
}

status_t ARTSPConnection::findPendingRequest(
        const sp<ARTSPResponse> &response, ssize_t *index) const {
    *index = 0;

    ssize_t i = response->mHeaders.indexOfKey("cseq");

    if (i < 0) {
        // This is an unsolicited server->client message.
        *index = -1;
        return OK;
    }

    AString value = response->mHeaders.valueAt(i);

    unsigned long cseq;
    if (!ParseSingleUnsignedLong(value.c_str(), &cseq)) {
        return ERROR_MALFORMED;
    }

    i = mPendingRequests.indexOfKey(cseq);

    if (i < 0) {
        return -ENOENT;
    }

    *index = i;

    return OK;
}

bool ARTSPConnection::notifyResponseListener(
        const sp<ARTSPResponse> &response) {
    ssize_t i;
    status_t err = findPendingRequest(response, &i);

    if (err == OK && i < 0) {
        // An unsolicited server response is not a problem.
        return true;
    }

    if (err != OK) {
#ifdef MTK_AOSP_ENHANCEMENT
    // mtk80902: for ALPS00868762
    // the 'OPTION' resp arrived after timeout
    // and we have already removed it
    // here just do nothing and return true
        ALOGW("No corresponding req pending..it's all right.");
        return true;
#else
        return false;
#endif
    }

    sp<AMessage> reply = mPendingRequests.valueAt(i);
    mPendingRequests.removeItemsAt(i);

    reply->setInt32("result", OK);
    reply->setObject("response", response);
    reply->post();

    return true;
}

bool ARTSPConnection::parseAuthMethod(const sp<ARTSPResponse> &response) {
    ssize_t i = response->mHeaders.indexOfKey("www-authenticate");

    if (i < 0) {
        return false;
    }

    AString value = response->mHeaders.valueAt(i);

    if (!strncmp(value.c_str(), "Basic", 5)) {
        mAuthType = BASIC;
    } else {
#if !defined(HAVE_ANDROID_OS)
        // We don't have access to the MD5 implementation on the simulator,
        // so we won't support digest authentication.
        return false;
#endif

        CHECK(!strncmp(value.c_str(), "Digest", 6));
        mAuthType = DIGEST;

#ifdef MTK_AOSP_ENHANCEMENT
        getRealm(value);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        i = value.find("nonce=");
        CHECK_GE(i, 0);
        CHECK_EQ(value.c_str()[i + 6], '\"');
        ssize_t j = value.find("\"", i + 7);
        CHECK_GE(j, 0);

        mNonce.setTo(value, i + 7, j - i - 7);
    }

    return true;
}

#if defined(HAVE_ANDROID_OS)
static void H(const AString &s, AString *out) {
    out->clear();

    MD5_CTX m;
    MD5_Init(&m);
    MD5_Update(&m, s.c_str(), s.size());

    uint8_t key[16];
    MD5_Final(key, &m);

    for (size_t i = 0; i < 16; ++i) {
        char nibble = key[i] >> 4;
        if (nibble <= 9) {
            nibble += '0';
        } else {
            nibble += 'a' - 10;
        }
        out->append(&nibble, 1);

        nibble = key[i] & 0x0f;
        if (nibble <= 9) {
            nibble += '0';
        } else {
            nibble += 'a' - 10;
        }
        out->append(&nibble, 1);
    }
}
#endif

static void GetMethodAndURL(
        const AString &request, AString *method, AString *url) {
    ssize_t space1 = request.find(" ");
    CHECK_GE(space1, 0);

    ssize_t space2 = request.find(" ", space1 + 1);
    CHECK_GE(space2, 0);

    method->setTo(request, 0, space1);
#ifdef MTK_AOSP_ENHANCEMENT
    url->setTo(request, space1 + 1, space2 - space1 - 1);
#else
    url->setTo(request, space1 + 1, space2 - space1);
#endif // #ifdef MTK_AOSP_ENHANCEMENT
}

void ARTSPConnection::addAuthentication(AString *request) {
    if (mAuthType == NONE) {
        return;
    }

    // Find the boundary between headers and the body.
    ssize_t i = request->find("\r\n\r\n");
    CHECK_GE(i, 0);

    if (mAuthType == BASIC) {
        AString tmp;
        tmp.append(mUser);
        tmp.append(":");
        tmp.append(mPass);

        AString out;
        encodeBase64(tmp.c_str(), tmp.size(), &out);

        AString fragment;
        fragment.append("Authorization: Basic ");
        fragment.append(out);
        fragment.append("\r\n");

        request->insert(fragment, i + 2);

        return;
    }

#if defined(HAVE_ANDROID_OS)
    CHECK_EQ((int)mAuthType, (int)DIGEST);

    AString method, url;
    GetMethodAndURL(*request, &method, &url);

    AString A1;
    A1.append(mUser);
    A1.append(":");
#ifdef MTK_AOSP_ENHANCEMENT
    A1.append(mRealm);
#else
    A1.append("Streaming Server");
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    A1.append(":");
    A1.append(mPass);

    AString A2;
    A2.append(method);
    A2.append(":");
    A2.append(url);

    AString HA1, HA2;
    H(A1, &HA1);
    H(A2, &HA2);

    AString tmp;
    tmp.append(HA1);
    tmp.append(":");
    tmp.append(mNonce);
    tmp.append(":");
    tmp.append(HA2);

    AString digest;
    H(tmp, &digest);

    AString fragment;
    fragment.append("Authorization: Digest ");
#ifndef MTK_AOSP_ENHANCEMENT
    fragment.append("nonce=\"");
    fragment.append(mNonce);
    fragment.append("\", ");
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    fragment.append("username=\"");
    fragment.append(mUser);
    fragment.append("\", ");
#ifdef MTK_AOSP_ENHANCEMENT
    fragment.append("realm=\"");
    fragment.append(mRealm);
    fragment.append("\", ");
    fragment.append("nonce=\"");
    fragment.append(mNonce);
    fragment.append("\", ");
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    fragment.append("uri=\"");
    fragment.append(url);
    fragment.append("\", ");
    fragment.append("response=\"");
    fragment.append(digest);
    fragment.append("\"");
    fragment.append("\r\n");

    request->insert(fragment, i + 2);
#endif
}

void ARTSPConnection::addUserAgent(AString *request) const {
    // Find the boundary between headers and the body.
    ssize_t i = request->find("\r\n\r\n");
    CHECK_GE(i, 0);

#ifdef MTK_AOSP_ENHANCEMENT
    request->insert(mUserAgent, i + 2);
#else
    request->insert(sUserAgent, i + 2);
#endif  //#ifdef MTK_AOSP_ENHANCEMENT
}

#ifdef MTK_AOSP_ENHANCEMENT
void ARTSPConnection::init() {
    mKeepTCPTrying = false;
    mForceQuitTCP = false;
    mExited = false;

    MakeUserAgent(&mUserAgent);

}

void ARTSPConnection::setSocketFlag(const sp<AMessage> &msg) {

    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    int ret_tmp = 0;
    int flag_t = 1;
    ret_tmp = setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag_t, sizeof(flag_t));
    ALOGI("set nodelay return %d\n", ret_tmp);

    struct timeval tv;
    tv.tv_sec = kAccessUnitTimeoutUs / 1000000LL;
    tv.tv_usec = 0;
    if (setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)) < 0) {
        ALOGE("can not set recv timeout");
        reply->setInt32("result", -errno);
        mState = DISCONNECTED;

        close(mSocket);
        mSocket = -1;
        reply->post();
        return;
    }
}

bool ARTSPConnection::isConnTimeout(const sp<AMessage> &msg) {
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

    int64_t then, now = ALooper::GetNowUs();
    if (msg->findInt64("timestamp", &then) && now - then > kRequestTimeout) {
        ALOGE("connection timeout %lld > %lld", (long long)now, (long long)then);
        reply->setInt32("result", -110 /*ETIMEDOUT*/);
        reply->post();
        mState = DISCONNECTED;
        close(mSocket);
        mSocket = -1;
        return true;
    }

    return false;
}

void ARTSPConnection::postTimeoutMsg(sp<AMessage> msg, int32_t cseq) {
    sp<AMessage> timeout = new AMessage(kWhatTimeout, this);
    timeout->setInt32("cseq", cseq);
    timeout->setMessage("reply", msg);
    int64_t t;
    if (msg->findInt64("timeout", &t)) {
        timeout->post(t);
    } else {
        timeout->post(kRequestTimeout);
    }
}

void ARTSPConnection::getRealm(AString value) {
    ssize_t i = value.find("realm=");
    CHECK_GE(i, 0);
    CHECK_EQ(value.c_str()[i + 6], '\"');
    ssize_t j = value.find("\"", i + 7);
    CHECK_GE(j, 0);

    mRealm.setTo(value, i + 7, j - i - 7);
}


// static
void ARTSPConnection::MakeUserAgent(AString *userAgent) {
    userAgent->clear();
#ifdef CUSTOM_UASTRING_FROM_PROPERTY
    char value[MAX_VALUE_LEN];
    if (0 < custom_get_string(MODULE_RTSP_STREAMING, USER_AGENT, value, NULL)) {
        userAgent->setTo("User-Agent: ");
        userAgent->append(value);
        userAgent->append("\r\n");
    } else {
        userAgent->setTo("User-Agent: stagefright/1.1 (Linux;Android ");
        #if (PROPERTY_VALUE_MAX < 8)
        #error "PROPERTY_VALUE_MAX must be at least 8"
        #endif
        char prop[PROPERTY_VALUE_MAX];
        property_get("ro.build.version.release", prop, "Unknown");
        userAgent->append(prop);
        userAgent->append(")\r\n");
    }
    memset(value, 0, sizeof(value));
    if (0 < custom_get_string(MODULE_RTSP_STREAMING, UAPROF_URL, value, NULL)) {
        userAgent->append("x-wap-profile: ");
        userAgent->append(value);
        userAgent->append("\r\n");
    } else {
        userAgent->append("x-wap-profile: http://218.249.47.94/Xianghe/MTK_Athens15_UAProfile.xml\r\n");
    }
#else //#ifdef CUSTOM_UASTRING_FROM_PROPERTY
#ifdef STR_RTSP_USER_AGENT
    //userAgent->setTo("User-Agent: Athens15_TD/V2 Linux/3.0.13 Android/4.0 Release/02.15.2012 Browser/AppleWebKit534.30 Mobile Safari/534.30 MBBMS/2.2\r\n");
    userAgent->setTo("User-Agent: ");
    userAgent->append(STR_RTSP_USER_AGENT);
    userAgent->append("\r\n");
#else
    userAgent->setTo("User-Agent: stagefright/1.1 (Linux;Android ");
    #if (PROPERTY_VALUE_MAX < 8)
    #error "PROPERTY_VALUE_MAX must be at least 8"
    #endif
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.build.version.release", value, "Unknown");
    userAgent->append(value);
    userAgent->append(")\r\n");
#endif

#ifdef STR_RTSP_WAP_PROFILE
    userAgent->append("x-wap-profile: ");
    userAgent->append(STR_RTSP_WAP_PROFILE);
    userAgent->append("\r\n");
#else
    userAgent->append("x-wap-profile: http://218.249.47.94/Xianghe/MTK_Athens15_UAProfile.xml\r\n");
#endif
#endif //#ifdef CUSTOM_UASTRING_FROM_PROPERTY
}

void ARTSPConnection::onTimeout(const sp<AMessage> &msg) {

    unsigned long cseq;
    CHECK(msg->findInt32("cseq", (int32_t*)&cseq));
    sp<AMessage> reply1;
    CHECK(msg->findMessage("reply", &reply1));

    ssize_t i = mPendingRequests.indexOfKey(cseq);
    if (i < 0) {
        return;
    }
    sp<AMessage> reply2 = mPendingRequests.valueAt(i);

    if(reply1->what() != reply2->what()){
        //ALOGE("timeout is old(cseq=%lu), ignore", cseq);
        return;
    }

    mPendingRequests.removeItemsAt(i);

    reply2->setInt32("result", -110 /*ETIMEDOUT*/);
    reply2->post();
}

void ARTSPConnection::injectPacket(const sp<ABuffer> &buffer) {
    sp<AMessage> msg = new AMessage(kWhatInjectPacket, this);
    msg->setObject("buffer", buffer);
    msg->post();
}

void ARTSPConnection::onInjectPacket(const sp<AMessage> &msg) {
    sp<RefBase> obj;
    CHECK(msg->findObject("buffer", &obj));

    sp<ABuffer> buffer = static_cast<ABuffer *>(obj.get());
    ssize_t n = send(mSocket, buffer->data(), buffer->size(), MTK_SEND_FLAG);
    if (n == 0) {
        ALOGW("Server unexpectedly closed the connection when sending RTCP.");
    } else if (n < 0) {
        ALOGW("errno when sending RTCP %d (%s)", errno, strerror(errno));
    }
    return;
}

void ARTSPConnection::stopTCPTrying() {
    ALOGI("stopTCPTrying");
    mForceQuitTCP = true;
}

void ARTSPConnection::exit(){
    ALOGI("exit");
    mExited = true;
}

#endif // #ifdef MTK_AOSP_ENHANCEMENT
}  // namespace android
