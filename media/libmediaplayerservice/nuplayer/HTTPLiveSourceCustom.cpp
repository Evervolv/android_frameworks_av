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
#define LOG_TAG "HTTPLiveSourceCustom"
#include <utils/Log.h>

#include "HTTPLiveSourceCustom.h"

#include "AnotherPacketSource.h"
#include "LiveDataSource.h"
#include "LiveSessionCustom.h"

#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>

#include "ExtendedUtils.h"

namespace android {

NuPlayer::HTTPLiveSourceCustom::HTTPLiveSourceCustom(
        const sp<AMessage> &notify,
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers)
    : Source(notify),
      mHTTPService(httpService),
      mURL(url),
      mFlags(0),
      mFinalResult(OK),
      mOffset(0),
      mFetchSubtitleDataGeneration(0) {
    if (headers) {
        mExtraHeaders = *headers;

        ssize_t index =
            mExtraHeaders.indexOfKey(String8("x-hide-urls-from-log"));

        if (index >= 0) {
            mFlags |= kFlagIncognito;

            mExtraHeaders.removeItemsAt(index);
        }
    }
}

NuPlayer::HTTPLiveSourceCustom::~HTTPLiveSourceCustom() {
    if (mLiveSession != NULL) {
        mLiveSession->disconnect();

        mLiveLooper->unregisterHandler(mLiveSession->id());
        mLiveLooper->unregisterHandler(id());
        mLiveLooper->stop();

        mLiveSession.clear();
        mLiveLooper.clear();
    }
}

void NuPlayer::HTTPLiveSourceCustom::prepareAsync() {
    if (mLiveLooper == NULL) {
        mLiveLooper = new ALooper;
        mLiveLooper->setName("http live");
        mLiveLooper->start();

        mLiveLooper->registerHandler(this);
    }

    sp<AMessage> notify = new AMessage(kWhatSessionNotify, id());

    mLiveSession = new LiveSessionCustom(
            notify,
            (mFlags & kFlagIncognito) ? LiveSessionCustom::kFlagIncognito : 0,
            mHTTPService);

    mLiveLooper->registerHandler(mLiveSession);

    mLiveSession->connectAsync(
            mURL.c_str(), mExtraHeaders.isEmpty() ? NULL : &mExtraHeaders);
}

void NuPlayer::HTTPLiveSourceCustom::start() {
}

sp<AMessage> NuPlayer::HTTPLiveSourceCustom::getFormat(bool audio) {
    sp<AMessage> format;
    status_t err = mLiveSession->getStreamFormat(
            audio ? LiveSessionCustom::STREAMTYPE_AUDIO
                  : LiveSessionCustom::STREAMTYPE_VIDEO,
            &format);

    if (err != OK) {
        return NULL;
    }

    return format;
}

status_t NuPlayer::HTTPLiveSourceCustom::feedMoreTSData() {
    return OK;
}

status_t NuPlayer::HTTPLiveSourceCustom::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    status_t err = mLiveSession->dequeueAccessUnit(
            audio ? LiveSessionCustom::STREAMTYPE_AUDIO
                  : LiveSessionCustom::STREAMTYPE_VIDEO,
            accessUnit);

    if (err == OK && audio) {
        sp<AMessage> format;
        if (OK != mLiveSession->getStreamFormat(LiveSessionCustom::STREAMTYPE_VIDEO, &format)) {
            //Detect the image in audio only clip
            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatShowImage);
            ExtendedUtils::detectAndPostImage(*accessUnit, notify);
        }
    }
    return err;
}

status_t NuPlayer::HTTPLiveSourceCustom::getDuration(int64_t *durationUs) {
    return mLiveSession->getDuration(durationUs);
}

size_t NuPlayer::HTTPLiveSourceCustom::getTrackCount() const {
    return mLiveSession->getTrackCount();
}

sp<AMessage> NuPlayer::HTTPLiveSourceCustom::getTrackInfo(size_t trackIndex) const {
    return mLiveSession->getTrackInfo(trackIndex);
}

status_t NuPlayer::HTTPLiveSourceCustom::selectTrack(size_t trackIndex, bool select) {
    status_t err = mLiveSession->selectTrack(trackIndex, select);

    if (err == OK) {
        mFetchSubtitleDataGeneration++;
        if (select) {
            sp<AMessage> msg = new AMessage(kWhatFetchSubtitleData, id());
            msg->setInt32("generation", mFetchSubtitleDataGeneration);
            msg->post();
        }
    }

    // LiveSessionCustom::selectTrack returns BAD_VALUE when selecting the currently
    // selected track, or unselecting a non-selected track. In this case it's an
    // no-op so we return OK.
    return (err == OK || err == BAD_VALUE) ? (status_t)OK : err;
}

status_t NuPlayer::HTTPLiveSourceCustom::seekTo(int64_t seekTimeUs) {
    return mLiveSession->seekTo(seekTimeUs);
}

void NuPlayer::HTTPLiveSourceCustom::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSessionNotify:
        {
            onSessionNotify(msg);
            break;
        }

        case kWhatFetchSubtitleData:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mFetchSubtitleDataGeneration) {
                // stale
                break;
            }

            sp<ABuffer> buffer;
            if (mLiveSession->dequeueAccessUnit(
                    LiveSessionCustom::STREAMTYPE_SUBTITLES, &buffer) == OK) {
                sp<AMessage> notify = dupNotify();
                notify->setInt32("what", kWhatSubtitleData);
                notify->setBuffer("buffer", buffer);
                notify->post();

                int64_t timeUs, baseUs, durationUs, delayUs;
                CHECK(buffer->meta()->findInt64("baseUs", &baseUs));
                CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
                CHECK(buffer->meta()->findInt64("durationUs", &durationUs));
                delayUs = baseUs + timeUs - ALooper::GetNowUs();

                msg->post(delayUs > 0ll ? delayUs : 0ll);
            } else {
                // try again in 1 second
                msg->post(1000000ll);
            }

            break;
        }

        default:
            Source::onMessageReceived(msg);
            break;
    }
}

void NuPlayer::HTTPLiveSourceCustom::onSessionNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case LiveSessionCustom::kWhatPrepared:
        {
            // notify the current size here if we have it, otherwise report an initial size of (0,0)
            sp<AMessage> format = getFormat(false /* audio */);
            int32_t width;
            int32_t height;
            if (format != NULL &&
                    format->findInt32("width", &width) && format->findInt32("height", &height)) {
                notifyVideoSizeChanged(format);
            } else {
                notifyVideoSizeChanged();
            }

            uint32_t flags = FLAG_CAN_PAUSE;
            if (mLiveSession->isSeekable()) {
                flags |= FLAG_CAN_SEEK;
                flags |= FLAG_CAN_SEEK_BACKWARD;
                flags |= FLAG_CAN_SEEK_FORWARD;
            }

            if (mLiveSession->hasDynamicDuration()) {
                flags |= FLAG_DYNAMIC_DURATION;
            }

            notifyFlagsChanged(flags);

            notifyPrepared();
            break;
        }

        case LiveSessionCustom::kWhatPreparationFailed:
        {
            status_t err;
            CHECK(msg->findInt32("err", &err));

            notifyPrepared(err);
            break;
        }

        case LiveSessionCustom::kWhatStreamsChanged:
        {
            uint32_t changedMask;
            CHECK(msg->findInt32(
                        "changedMask", (int32_t *)&changedMask));

            bool audio = changedMask & LiveSessionCustom::STREAMTYPE_AUDIO;
            bool video = changedMask & LiveSessionCustom::STREAMTYPE_VIDEO;

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));

            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatQueueDecoderShutdown);
            notify->setInt32("audio", audio);
            notify->setInt32("video", video);
            notify->setMessage("reply", reply);
            notify->post();
            break;
        }

        case LiveSessionCustom::kWhatError:
        {
            break;
        }

        default:
            TRESPASS();
    }
}

}  // namespace android

