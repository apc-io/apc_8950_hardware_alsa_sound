/* ALSAStreamOps.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
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

#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

namespace android
{

// ----------------------------------------------------------------------------

ALSAStreamOps::ALSAStreamOps(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    mParent(parent),
    mHandle(handle),
    mPowerLock(false)
{
}

ALSAStreamOps::~ALSAStreamOps()
{
    AutoMutex lock(mLock);

    close();
}

// use emulated popcount optimization
// http://www.df.lth.se/~john_e/gems/gem002d.html
static inline uint32_t popCount(uint32_t u)
{
    u = ((u&0x55555555) + ((u>>1)&0x55555555));
    u = ((u&0x33333333) + ((u>>2)&0x33333333));
    u = ((u&0x0f0f0f0f) + ((u>>4)&0x0f0f0f0f));
    u = ((u&0x00ff00ff) + ((u>>8)&0x00ff00ff));
    u = ( u&0x0000ffff) + (u>>16);
    return u;
}

acoustic_device_t *ALSAStreamOps::acoustics()
{
    return mParent->mAcousticDevice;
}

ALSAMixer *ALSAStreamOps::mixer()
{
    return mParent->mMixer;
}

status_t ALSAStreamOps::set(int      format,
                            uint32_t channelCount,
                            uint32_t rate)
{
    if (channelCount > 0)
        mHandle->channels   = channelCount;

    if (rate > 0)
        mHandle->sampleRate = rate;

    snd_pcm_format_t iformat = mHandle->format;

    switch(format) {
        case AudioSystem::FORMAT_DEFAULT:
            break;

        case AudioSystem::PCM_16_BIT:
            iformat = SND_PCM_FORMAT_S16_LE;
            break;

        case AudioSystem::PCM_8_BIT:
            iformat = SND_PCM_FORMAT_S8;
            break;

        default:
            LOGE("Unknown PCM format %i. Forcing default", format);
            break;
    }

    mHandle->format = iformat;

    return NO_ERROR;
}

uint32_t ALSAStreamOps::sampleRate() const
{
    return mHandle->sampleRate;
}

//
// Return the number of bytes (not frames)
//
size_t ALSAStreamOps::bufferSize() const
{
    snd_pcm_uframes_t bufferSize = mHandle->bufferSize;
    snd_pcm_uframes_t periodSize;

    snd_pcm_get_params(mHandle->handle, &bufferSize, &periodSize);

    size_t bytes = static_cast<size_t>(snd_pcm_frames_to_bytes(mHandle->handle, bufferSize));

    // Not sure when this happened, but unfortunately it now
    // appears that the bufferSize must be reported as a
    // power of 2. This might be for OSS compatibility.
    for (size_t i = 1; (bytes & ~i) != 0; i<<=1)
        bytes &= ~i;

    return bytes;
}

int ALSAStreamOps::format() const
{
    int pcmFormatBitWidth;
    int audioSystemFormat;

    snd_pcm_format_t ALSAFormat = mHandle->format;

    pcmFormatBitWidth = snd_pcm_format_physical_width(ALSAFormat);
    switch(pcmFormatBitWidth) {
        case 8:
            audioSystemFormat = AudioSystem::PCM_8_BIT;
            break;

        default:
            LOG_FATAL("Unknown AudioSystem bit width %i!", pcmFormatBitWidth);

        case 16:
            audioSystemFormat = AudioSystem::PCM_16_BIT;
            break;
    }

    return audioSystemFormat;
}

int ALSAStreamOps::channelCount() const
{
    return mHandle->channels;
}

void ALSAStreamOps::close()
{
    mParent->mALSADevice->close(mHandle);
}

//
// Set playback or capture PCM device.  It's possible to support audio output
// or input from multiple devices by using the ALSA plugins, but this is
// not supported for simplicity.
//
// The AudioHardwareALSA API does not allow one to set the input routing.
//
// If the "routes" value does not map to a valid device, the default playback
// device is used.
//
status_t ALSAStreamOps::open(int mode)
{
    return mParent->mALSADevice->open(mHandle, mHandle->curDev, mode);
}

}       // namespace android
