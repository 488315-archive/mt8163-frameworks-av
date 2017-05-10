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
#define LOG_TAG "ASessionDescription"
#include <utils/Log.h>
#include <cutils/log.h>

#include "ASessionDescription.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

#include <stdlib.h>
#ifdef MTK_AOSP_ENHANCEMENT
#include <media/stagefright/MediaErrors.h>
#endif // #ifdef MTK_AOSP_ENHANCEMENT

namespace android {

ASessionDescription::ASessionDescription()
    : mIsValid(false) {
}

ASessionDescription::~ASessionDescription() {
}

bool ASessionDescription::setTo(const void *data, size_t size) {
    mIsValid = parse(data, size);

    if (!mIsValid) {
        mTracks.clear();
        mFormats.clear();
    }

    return mIsValid;
}

bool ASessionDescription::parse(const void *data, size_t size) {
    mTracks.clear();
    mFormats.clear();

    mTracks.push(Attribs());
    mFormats.push(AString("[root]"));

    AString desc((const char *)data, size);

#ifdef MTK_AOSP_ENHANCEMENT
    int rtpmapNum = 0;
    bool unsupported = false;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    size_t i = 0;
    for (;;) {
#ifdef MTK_AOSP_ENHANCEMENT
        if (i >= desc.size()) {
            break;
        }
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        ssize_t eolPos = desc.find("\n", i);

        if (eolPos < 0) {
#ifdef MTK_AOSP_ENHANCEMENT
            eolPos = desc.size();
#else
            break;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        }

        AString line;
        if ((size_t)eolPos > i && desc.c_str()[eolPos - 1] == '\r') {
            // We accept both '\n' and '\r\n' line endings, if it's
            // the latter, strip the '\r' as well.
            line.setTo(desc, i, eolPos - i - 1);
        } else {
            line.setTo(desc, i, eolPos - i);
        }

        if (line.empty()) {
            i = eolPos + 1;
            continue;
        }

        if (line.size() < 2 || line.c_str()[1] != '=') {
            return false;
        }

#ifdef MTK_AOSP_ENHANCEMENT
        if (unsupported && line.c_str()[0] != 'm') {
            ALOGI("skip %s in unsupported media description", line.c_str());
            i = eolPos + 1;
            continue;
        } else
#endif // #ifdef MTK_AOSP_ENHANCEMENT
        ALOGI("%s", line.c_str());

        switch (line.c_str()[0]) {
            case 'v':
            {
                if (strcmp(line.c_str(), "v=0")) {
                    return false;
                }
                break;
            }

            case 'a':
            case 'b':
            {
                AString key, value;

                ssize_t colonPos = line.find(":", 2);
                if (colonPos < 0) {
                    key = line;
                } else {
                    key.setTo(line, 0, colonPos);

                    if (key == "a=fmtp" || key == "a=rtpmap"
                            || key == "a=framesize") {
                        ssize_t spacePos = line.find(" ", colonPos + 1);
                        if (spacePos < 0) {
                            return false;
                        }

#ifdef MTK_AOSP_ENHANCEMENT
                        if(!parseRtpMap(key, rtpmapNum, unsupported))
                            break;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
                        key.setTo(line, 0, spacePos);

                        colonPos = spacePos;
                    }

                    value.setTo(line, colonPos + 1, line.size() - colonPos - 1);
                }

                key.trim();
                value.trim();

                ALOGV("adding '%s' => '%s'", key.c_str(), value.c_str());

                mTracks.editItemAt(mTracks.size() - 1).add(key, value);
                break;
            }

            case 'm':
            {
                ALOGV("new section '%s'",
                     AString(line, 2, line.size() - 2).c_str());

#ifdef MTK_AOSP_ENHANCEMENT
                rtpmapNum = 0;
                unsupported = false;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
                mTracks.push(Attribs());
                mFormats.push(AString(line, 2, line.size() - 2));
                break;
            }

            default:
            {
                AString key, value;

                ssize_t equalPos = line.find("=");

                key = AString(line, 0, equalPos + 1);
                value = AString(line, equalPos + 1, line.size() - equalPos - 1);

                key.trim();
                value.trim();

                ALOGV("adding '%s' => '%s'", key.c_str(), value.c_str());

                mTracks.editItemAt(mTracks.size() - 1).add(key, value);
                break;
            }
        }

        i = eolPos + 1;
    }

    return true;
}

bool ASessionDescription::isValid() const {
    return mIsValid;
}

size_t ASessionDescription::countTracks() const {
    return mTracks.size();
}

void ASessionDescription::getFormat(size_t index, AString *value) const {
    CHECK_GE(index, 0u);
    CHECK_LT(index, mTracks.size());

    *value = mFormats.itemAt(index);
}

bool ASessionDescription::findAttribute(
        size_t index, const char *key, AString *value) const {
    CHECK_GE(index, 0u);
    CHECK_LT(index, mTracks.size());

    value->clear();

    const Attribs &track = mTracks.itemAt(index);
    ssize_t i = track.indexOfKey(AString(key));

    if (i < 0) {
        return false;
    }

    *value = track.valueAt(i);

    return true;
}

void ASessionDescription::getFormatType(
        size_t index, unsigned long *PT,
        AString *desc, AString *params) const {
    AString format;
    getFormat(index, &format);

    const char *lastSpacePos = strrchr(format.c_str(), ' ');
    CHECK(lastSpacePos != NULL);

    char *end;
    unsigned long x = strtoul(lastSpacePos + 1, &end, 10);
    CHECK_GT(end, lastSpacePos + 1);
    CHECK_EQ(*end, '\0');

    *PT = x;

    char key[32];
    snprintf(key, sizeof(key), "a=rtpmap:%lu", x);

    CHECK(findAttribute(index, key, desc));

    snprintf(key, sizeof(key), "a=fmtp:%lu", x);
    if (!findAttribute(index, key, params)) {
        params->clear();
    }
}

bool ASessionDescription::getDimensions(
        size_t index, unsigned long PT,
        int32_t *width, int32_t *height) const {
    *width = 0;
    *height = 0;

    char key[33];
    snprintf(key, sizeof(key), "a=framesize:%lu", PT);
    if (PT > 9999999) {
        android_errorWriteLog(0x534e4554, "25747670");
    }
    AString value;
    if (!findAttribute(index, key, &value)) {
#ifdef MTK_AOSP_ENHANCEMENT
        return tryGetWH(index, width, height, key, value);
#else
        return false;
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    }

    const char *s = value.c_str();
    char *end;
    *width = strtoul(s, &end, 10);
    CHECK_GT(end, s);
    CHECK_EQ(*end, '-');

    s = end + 1;
    *height = strtoul(s, &end, 10);
    CHECK_GT(end, s);
    CHECK_EQ(*end, '\0');

    return true;
}

bool ASessionDescription::getDurationUs(int64_t *durationUs) const {
    *durationUs = 0;

    CHECK(mIsValid);

    AString value;
    if (!findAttribute(0, "a=range", &value)) {
        return false;
    }

#ifdef MTK_AOSP_ENHANCEMENT
    if (strncmp(value.c_str(), "npt=", 4) && strncmp(value.c_str(), "npt:", 4))
#else
    if (strncmp(value.c_str(), "npt=", 4))
#endif // #ifdef MTK_AOSP_ENHANCEMENT
    {
        return false;
    }

    float from, to;
    if (!parseNTPRange(value.c_str() + 4, &from, &to)) {
        return false;
    }

    *durationUs = (int64_t)((to - from) * 1E6);

    return true;
}

// static
void ASessionDescription::ParseFormatDesc(
        const char *desc, int32_t *timescale, int32_t *numChannels) {
    const char *slash1 = strchr(desc, '/');
    CHECK(slash1 != NULL);

    const char *s = slash1 + 1;
    char *end;
    unsigned long x = strtoul(s, &end, 10);
    CHECK_GT(end, s);
    CHECK(*end == '\0' || *end == '/');

    *timescale = x;
    *numChannels = 1;

    if (*end == '/') {
        s = end + 1;
        unsigned long x = strtoul(s, &end, 10);
        CHECK_GT(end, s);
        CHECK_EQ(*end, '\0');

        *numChannels = x;
    }
}

// static
bool ASessionDescription::parseNTPRange(
        const char *s, float *npt1, float *npt2) {
    if (s[0] == '-') {
        return false;  // no start time available.
    }

    if (!strncmp("now", s, 3)) {
        return false;  // no absolute start time available
    }

    char *end;
    *npt1 = strtof(s, &end);

    if (end == s || *end != '-') {
        // Failed to parse float or trailing "dash".
        return false;
    }

    s = end + 1;  // skip the dash.

    if (*s == '\0') {
#ifdef MTK_AOSP_ENHANCEMENT
                // change for ALPS01771091 by mtk08585
                // if come here, it is a live streaming
                return false;
#else
        *npt2 = FLT_MAX;  // open ended.
        return true;
#endif
    }

    if (!strncmp("now", s, 3)) {
        return false;  // no absolute end time available
    }

    *npt2 = strtof(s, &end);

    if (end == s || *end != '\0') {
        return false;
    }

    return *npt2 > *npt1;
}

#ifdef MTK_AOSP_ENHANCEMENT
bool ASessionDescription::parseRtpMap(AString key, int &rtpmapNum, bool &unsupported) {
    if (key == "a=rtpmap") {
        if (rtpmapNum > 0) {
            mTracks.pop();
            mFormats.pop();
            unsupported = true;
            ALOGW("ASessionDescription: multiple rtpmap"
                    " for one media is not supported yet");
            return false;
        } else {
            rtpmapNum++;
        }
    }
    return true;
}

int ASessionDescription::parseString(const char* s) const {
    ALOGI("parseString %s", s);

    int len = strlen(s);
    if (len < 9)
        return -1;

    if (strncmp(s, "integer;", 8))
        return -1;

    //const char *tmp = s + 8;
    int v;
    sscanf(s + 8, "%d", &v);
    return v;
}

bool ASessionDescription::tryGetWH(size_t index, int32_t *width, int32_t *height, char *key, AString &value) const {
    // try to get dimensions from cliprect if no framesize
    strcpy(key, "a=cliprect");
    if (!findAttribute(index, key, &value)) {
        ALOGW("no framesize and cliprect, try Width/Height");
        strcpy(key, "a=Width");
        if (!findAttribute(index, key, &value)) {
            return false;
        }
        int w = parseString(value.c_str());

        strcpy(key, "a=Height");
        if (!findAttribute(index, key, &value)) {
            return false;
        }
        int h = parseString(value.c_str());

        if (w > 0 && h > 0) {
            *width = w;
            *height = h;
            return true;
        }
        return false;
    }

    const char *s = value.c_str();
    int a = -1, b = -1, c = -1, d = -1;
    sscanf(s, "%d,%d,%d,%d", &a, &b, &c, &d);
    if (a == -1 || b == -1 || c == -1 || d == -1)
        return false;
    *height = c - a;
    *width = d - b;
    return true;
}

bool ASessionDescription::getBitrate(size_t index, int32_t* bitrate) const {
    char key[] = "b=AS";
    AString value;
    if (!findAttribute(index, key, &value))
        return false;
    int32_t b = atoi(value.c_str());
    b *= 1000;
    if (b < 0)
        return false;
    *bitrate = b;
    return true;
}

status_t ASessionDescription::getSessionUrl(String8& uri) const{
    AString line;
    if(findAttribute(0, "a=control", &line)) {
        // rtsp without aggregation control url will be considered as pure RTP
        if (!line.startsWith("rtsp://"))
            return ERROR_UNSUPPORTED;

        uri.setTo(line.c_str());
    } else {
        // assume as rtp streaming
        uri.setTo("rtp://0.0.0.0");
    }
    return OK;
}

#endif // #ifdef MTK_AOSP_ENHANCEMENT

}  // namespace android

