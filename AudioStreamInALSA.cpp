/* AudioStreamInALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_TAG "AudioStreamInALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

extern "C" {
#include "csd_client.h"
}

namespace android_audio_legacy
{

AudioStreamInALSA::AudioStreamInALSA(AudioHardwareALSA *parent,
        alsa_handle_t *handle,
        AudioSystem::audio_in_acoustics audio_acoustics) :
    ALSAStreamOps(parent, handle),
    mFramesLost(0),
    mParent(parent),
    mAcoustics(audio_acoustics)
{
}

AudioStreamInALSA::~AudioStreamInALSA()
{
    close();
}

status_t AudioStreamInALSA::setGain(float gain)
{
    return 0; //mixer() ? mixer()->setMasterGain(gain) : (status_t)NO_INIT;
}

ssize_t AudioStreamInALSA::read(void *buffer, ssize_t bytes)
{
    int period_size;

    LOGV("read:: buffer %p, bytes %d", buffer, bytes);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioInLock");
        mPowerLock = true;
    }

    int n;
    status_t          err;
    size_t            read = 0;
    char *use_case;
    int newMode = mParent->mode();

    if((mHandle->handle == NULL) && (mHandle->rxHandle == NULL) &&
         (strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) &&
         (strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
        mParent->mLock.lock();
        snd_use_case_get(mHandle->ucMgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if ((mHandle->devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("read:: mParent->mIncallMode=%d", mParent->mIncallMode);
                if ((mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    if (mParent->mFusion3Platform) {
                        mParent->mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(mHandle->useCase));
                        csd_client_start_record(INCALL_REC_STEREO);
                    } else {
                        strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
                                sizeof(mHandle->useCase));
                    }
                } else if (mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                    if (mParent->mFusion3Platform) {
                        mParent->mALSADevice->setVocRecMode(INCALL_REC_MONO);
                        strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(mHandle->useCase));
                        csd_client_start_record(INCALL_REC_MONO);
                    } else {
                        strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
                                sizeof(mHandle->useCase));
                    }
                }
            } else if(mHandle->devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(mHandle->useCase));
            } else if (mHandle->devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, sizeof(mHandle->useCase));
            } else if(!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP)) {
                strlcpy(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(mHandle->useCase));
            }else {
                strlcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(mHandle->useCase));
            }
        } else {
            if ((mHandle->devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("read:: ---- mParent->mIncallMode=%d", mParent->mIncallMode);
                if ((mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    if (mParent->mFusion3Platform) {
                        mParent->mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(mHandle->useCase, SND_USE_CASE_VERB_INCALL_REC,
                                sizeof(mHandle->useCase));
                        csd_client_start_record(INCALL_REC_STEREO);
                    } else {
                        strlcpy(mHandle->useCase, SND_USE_CASE_VERB_UL_DL_REC,
                                sizeof(mHandle->useCase));
                    }
                } else if (mParent->mIncallMode & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                   if (mParent->mFusion3Platform) {
                       mParent->mALSADevice->setVocRecMode(INCALL_REC_MONO);
                       strlcpy(mHandle->useCase, SND_USE_CASE_VERB_INCALL_REC,
                               sizeof(mHandle->useCase));
                       csd_client_start_record(INCALL_REC_MONO);
                   } else {
                       strlcpy(mHandle->useCase, SND_USE_CASE_VERB_DL_REC,
                               sizeof(mHandle->useCase));
                   }
                }
            } else if(mHandle->devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(mHandle->useCase, SND_USE_CASE_VERB_FM_REC, sizeof(mHandle->useCase));
        } else if (mHandle->devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(mHandle->useCase, SND_USE_CASE_VERB_FM_A2DP_REC, sizeof(mHandle->useCase));
            } else if(!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)){
                    strlcpy(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(mHandle->useCase));
            } else {
                strlcpy(mHandle->useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(mHandle->useCase));
            }
        }
        free(use_case);
        if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
            (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
            if((mDevices & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET) ||
               (mDevices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)) {
                mHandle->module->route(mHandle, (mDevices | AudioSystem::DEVICE_IN_PROXY) , AudioSystem::MODE_IN_COMMUNICATION);
            }else{
                mHandle->module->route(mHandle, mDevices , AudioSystem::MODE_IN_COMMUNICATION);
            }
        } else {
            if((mHandle->devices == AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET)||
               (mHandle->devices == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)){
                mHandle->module->route(mHandle, AudioSystem::DEVICE_IN_PROXY , mParent->mode());
            } else {
                mHandle->module->route(mHandle, mDevices , mParent->mode());
            }
        }
        if (!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI_REC) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_FM_REC) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_FM_A2DP_REC) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_UL_DL_REC) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_DL_REC) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_INCALL_REC)) {
            snd_use_case_set(mHandle->ucMgr, "_verb", mHandle->useCase);
        } else {
            snd_use_case_set(mHandle->ucMgr, "_enamod", mHandle->useCase);
        }
       if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
           (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
            err = mHandle->module->startVoipCall(mHandle);
        }
        else
            mHandle->module->open(mHandle);
        if(mHandle->handle == NULL) {
            LOGE("read:: PCM device open failed");
            mParent->mLock.unlock();

            return 0;
        }
        if((mHandle->devices == AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET)||
           (mHandle->devices == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)){
            if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
               (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                mParent->musbRecordingState |= USBRECBIT_VOIPCALL;
            } else {
                mParent->startUsbRecordingIfNotStarted();
                mParent->musbRecordingState |= USBRECBIT_REC;
            }
        }
        mParent->mLock.unlock();
    }

    if(((mDevices & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET) ||
       (mDevices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)) &&
       (!mParent->musbRecordingState)) {
        mParent->mLock.lock();
        LOGD("Starting UsbRecording thread");
        mParent->startUsbRecordingIfNotStarted();
        if(!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL) ||
           !strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP)) {
            LOGD("Enabling voip recording bit");
            mParent->musbRecordingState |= USBRECBIT_VOIPCALL;
        }else{
            LOGD("Enabling HiFi Recording bit");
            mParent->musbRecordingState |= USBRECBIT_REC;
        }
        mParent->mLock.unlock();
    }

    period_size = mHandle->periodSize;
    int read_pending = bytes;
    do {
        if (read_pending < period_size) {
            read_pending = period_size;
        }

        n = pcm_read(mHandle->handle, buffer,
            period_size);
        LOGV("pcm_read() returned n = %d", n);
        if (n && (n == -EIO || n == -EAGAIN || n == -EPIPE || n == -EBADFD)) {
            mParent->mLock.lock();
            LOGW("pcm_read() returned error n %d, Recovering from error\n", n);
            pcm_close(mHandle->handle);
            mHandle->handle = NULL;
            if((!strncmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL, strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
              (!strncmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP, strlen(SND_USE_CASE_MOD_PLAY_VOIP)))) {
                 pcm_close(mHandle->rxHandle);
                 mHandle->rxHandle = NULL;
                 mHandle->module->startVoipCall(mHandle);
            }
            else
                 mHandle->module->open(mHandle);
            mParent->mLock.unlock();
            continue;
        }
        else if (n < 0) {
            LOGD("pcm_read() returned n < 0");
            return static_cast<ssize_t>(n);
        }
        else {
            read += static_cast<ssize_t>((period_size));
            read_pending -= period_size;
            buffer += period_size;
        }

    } while (mHandle->handle && read < bytes);

    return read;
}

status_t AudioStreamInALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamInALSA::open(int mode)
{
    Mutex::Autolock autoLock(mParent->mLock);

    status_t status = ALSAStreamOps::open(mode);

    return status;
}

status_t AudioStreamInALSA::close()
{
    Mutex::Autolock autoLock(mParent->mLock);

    LOGD("close");
    if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
        (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
        if((mParent->mVoipStreamCount)) {
            LOGD("musbRecordingState: %d, mVoipStreamCount:%d",mParent->musbRecordingState,
                  mParent->mVoipStreamCount );
            if(mParent->mVoipStreamCount == 1) {
                LOGE("Deregistering VOIP Call bit, musbPlaybackState:%d,"
                       "musbRecordingState:%d", mParent->musbPlaybackState, mParent->musbRecordingState);
                mParent->musbPlaybackState &= ~USBPLAYBACKBIT_VOIPCALL;
                mParent->musbRecordingState &= ~USBRECBIT_VOIPCALL;
                mParent->closeUsbRecordingIfNothingActive();
                mParent->closeUsbPlaybackIfNothingActive();
            }
               return NO_ERROR;
        }
        mParent->mVoipStreamCount = 0;
        mParent->mVoipMicMute = 0;
    } else {
        LOGD("Deregistering REC bit, musbRecordingState:%d", mParent->musbRecordingState);
        mParent->musbRecordingState &= ~USBRECBIT_REC;
     }

    if (mParent->mFusion3Platform) {
       if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_INCALL_REC)) ||
           (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE))) {
           csd_client_stop_record();
       }
    }

    LOGD("close");
    mParent->closeUsbRecordingIfNothingActive();

    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

status_t AudioStreamInALSA::standby()
{
    Mutex::Autolock autoLock(mParent->mLock);

    LOGD("standby");

    if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
        (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
         return NO_ERROR;
    }

    LOGD("standby");
    if (mParent->mFusion3Platform) {
       if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_INCALL_REC)) ||
           (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE))) {
           LOGD(" into standby, stop record");
           csd_client_stop_record();
       }
    }

    mHandle->module->standby(mHandle);

    LOGD("Checking for musbRecordingState %d", mParent->musbRecordingState);
    mParent->musbRecordingState &= ~USBRECBIT_REC;
    mParent->closeUsbRecordingIfNothingActive();

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

void AudioStreamInALSA::resetFramesLost()
{
    mFramesLost = 0;
}

unsigned int AudioStreamInALSA::getInputFramesLost() const
{
    unsigned int count = mFramesLost;
    // Stupid interface wants us to have a side effect of clearing the count
    // but is defined as a const to prevent such a thing.
    ((AudioStreamInALSA *)this)->resetFramesLost();
    return count;
}

status_t AudioStreamInALSA::setAcousticParams(void *params)
{
    Mutex::Autolock autoLock(mParent->mLock);

    return (status_t)NO_ERROR;
}

}       // namespace android_audio_legacy
