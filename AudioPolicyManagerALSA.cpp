/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "AudioPolicyManagerALSA"
//#define LOG_NDEBUG 0
#include <utils/Log.h>
#include "AudioPolicyManagerALSA.h"
#include <media/mediarecorder.h>

namespace android {

extern "C"
{
    AudioPolicyInterface* createAudioPolicyManager(AudioPolicyClientInterface *clientInterface)
    {
        return new AudioPolicyManagerALSA(clientInterface);
    }

    void destroyAudioPolicyManager(AudioPolicyInterface *interface)
    {
        delete (AudioPolicyManagerALSA *)interface;
    }
}


// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------


status_t AudioPolicyManagerALSA::setDeviceConnectionState(AudioSystem::audio_devices device,
                                                  AudioSystem::device_connection_state state,
                                                  const char *device_address)
{

    LOGV("setDeviceConnectionState() device: %x, state %d, address %s", device, state, device_address);

    // connect/disconnect only 1 device at a time
    if (AudioSystem::popCount(device) != 1) return BAD_VALUE;

    if (strlen(device_address) >= MAX_DEVICE_ADDRESS_LEN) {
        LOGE("setDeviceConnectionState() invalid address: %s", device_address);
        return BAD_VALUE;
    }

    // handle output devices
    if (AudioSystem::isOutputDevice(device)) {
        switch (state)
        {
        // handle output device connection
        case AudioSystem::DEVICE_STATE_AVAILABLE:
            if (mAvailableOutputDevices & device) {
                LOGW("setDeviceConnectionState() device already connected: %x", device);
                return INVALID_OPERATION;
            }
            LOGV("setDeviceConnectionState() connecting device %x", device);

            // register new device as available
            mAvailableOutputDevices |= device;
            break;
        // handle output device disconnection
        case AudioSystem::DEVICE_STATE_UNAVAILABLE:
            if (!(mAvailableOutputDevices & device)) {
                LOGW("setDeviceConnectionState() device not connected: %x", device);
                return INVALID_OPERATION;
            }
            LOGV("setDeviceConnectionState() disconnecting device %x", device);
            // remove device from available output devices
            mAvailableOutputDevices &= ~device;
            break;

        default:
            LOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }
        return NO_ERROR;
    }
    // handle input devices
    if (AudioSystem::isInputDevice(device)) {
        switch (state)
        {
        // handle input device connection
        case AudioSystem::DEVICE_STATE_AVAILABLE:
            if (mAvailableInputDevices & device) {
                LOGW("setDeviceConnectionState() device already connected: %d", device);
                return INVALID_OPERATION;
            }
            mAvailableInputDevices |= device;
            break;

        // handle input device disconnection
        case AudioSystem::DEVICE_STATE_UNAVAILABLE:
            if (!(mAvailableInputDevices & device)) {
                LOGW("setDeviceConnectionState() device not connected: %d", device);
                return INVALID_OPERATION;
            }
            mAvailableInputDevices &= ~device;
            break;

        default:
            LOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }
        return NO_ERROR;
    }

    LOGW("setDeviceConnectionState() invalid device: %x", device);
    return BAD_VALUE;
}

AudioSystem::device_connection_state AudioPolicyManagerALSA::getDeviceConnectionState(AudioSystem::audio_devices device,
                                                  const char *device_address)
{
    AudioSystem::device_connection_state state = AudioSystem::DEVICE_STATE_UNAVAILABLE;
    String8 address = String8(device_address);
    if (AudioSystem::isOutputDevice(device)) {
        if (device & mAvailableOutputDevices) {
            state = AudioSystem::DEVICE_STATE_AVAILABLE;
        }
    } else if (AudioSystem::isInputDevice(device)) {
        if (device & mAvailableInputDevices) {
            state = AudioSystem::DEVICE_STATE_AVAILABLE;
        }
    }

    return state;
}

void AudioPolicyManagerALSA::setPhoneState(int state)
{
    LOGV("setPhoneState() state %d", state);
    uint32_t newDevice = 0;
    if (state < 0 || state >= AudioSystem::NUM_MODES) {
        LOGW("setPhoneState() invalid state %d", state);
        return;
    }

    if (state == mPhoneState ) {
        LOGW("setPhoneState() setting same state %d", state);
        return;
    }
    // store previous phone state for management of sonification strategy below
    int oldState = mPhoneState;
    mPhoneState = state;

    // if leaving or entering in call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (state == AudioSystem::MODE_IN_CALL ||
        oldState == AudioSystem::MODE_IN_CALL) {
        bool starting = (state == AudioSystem::MODE_IN_CALL) ? true : false;
        LOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AudioSystem::NUM_STREAM_TYPES; stream++) {
            handleIncallSonification(stream, starting);
        }
    }
}

void AudioPolicyManagerALSA::setRingerMode(uint32_t mode, uint32_t mask)
{
    LOGV("setRingerMode() mode %x, mask %x", mode, mask);

    mRingerMode = mode;
}

void AudioPolicyManagerALSA::setForceUse(AudioSystem::force_use usage, AudioSystem::forced_config config)
{
    LOGV("setForceUse) usage %d, config %d, mPhoneState %d", usage, config, mPhoneState);
    mForceUse[usage] = config;
}

AudioSystem::forced_config AudioPolicyManagerALSA::getForceUse(AudioSystem::force_use usage)
{
    return mForceUse[usage];
}

void AudioPolicyManagerALSA::setSystemProperty(const char* property, const char* value)
{
    LOGV("setSystemProperty() property %s, value %s", property, value);
    if (strcmp(property, "ro.camera.sound.forced") == 0) {
        if (atoi(value)) {
            LOGV("ENFORCED_AUDIBLE cannot be muted");
            mStreams[AudioSystem::ENFORCED_AUDIBLE].mCanBeMuted = false;
        } else {
            LOGV("ENFORCED_AUDIBLE can be muted");
            mStreams[AudioSystem::ENFORCED_AUDIBLE].mCanBeMuted = true;
        }
    }
}

audio_io_handle_t AudioPolicyManagerALSA::getOutput(AudioSystem::stream_type stream,
                                    uint32_t samplingRate,
                                    uint32_t format,
                                    uint32_t channels,
                                    AudioSystem::output_flags flags)
{
    LOGV("getOutput() stream %d, samplingRate %d, format %d, channels %x, flags %x", stream, samplingRate, format, channels, flags);

    if ((flags & AudioSystem::OUTPUT_FLAG_DIRECT) ||
        (format != 0 && !AudioSystem::isLinearPCM(format)) ||
        (channels != 0 && channels != AudioSystem::CHANNEL_OUT_MONO && channels != AudioSystem::CHANNEL_OUT_STEREO)) {
        return 0;
    }

    return mHardwareOutput;
}

status_t AudioPolicyManagerALSA::startOutput(audio_io_handle_t output, AudioSystem::stream_type stream)
{
    LOGV("startOutput() output %d, stream %d", output, stream);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        LOGW("startOutput() unknow output %d", output);
        return BAD_VALUE;
    }

    AudioOutputDescriptor *outputDesc = mOutputs.valueAt(index);

    // handle special case for sonification while in call
    if (mPhoneState == AudioSystem::MODE_IN_CALL) {
        handleIncallSonification(stream, true);
    }

    // incremenent usage count for this stream on the requested output:
    outputDesc->changeRefCount(stream, 1);
    return NO_ERROR;
}

status_t AudioPolicyManagerALSA::stopOutput(audio_io_handle_t output, AudioSystem::stream_type stream)
{
    LOGV("stopOutput() output %d, stream %d", output, stream);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        LOGW("stopOutput() unknow output %d", output);
        return BAD_VALUE;
    }

    AudioOutputDescriptor *outputDesc = mOutputs.valueAt(index);

    // handle special case for sonification while in call
    if (mPhoneState == AudioSystem::MODE_IN_CALL) {
        handleIncallSonification(stream, false);
    }

    if (outputDesc->isUsedByStream(stream)) {
        // decrement usage count of this stream on the output
        outputDesc->changeRefCount(stream, -1);
        return NO_ERROR;
    } else {
        LOGW("stopOutput() refcount is already 0 for output %d", output);
        return INVALID_OPERATION;
    }
}

void AudioPolicyManagerALSA::releaseOutput(audio_io_handle_t output)
{
    LOGV("releaseOutput() %d", output);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        LOGW("releaseOutput() releasing unknown output %d", output);
        return;
    }
}

audio_io_handle_t AudioPolicyManagerALSA::getInput(int inputSource,
                                    uint32_t samplingRate,
                                    uint32_t format,
                                    uint32_t channels,
                                    AudioSystem::audio_in_acoustics acoustics)
{
    audio_io_handle_t input = 0;
    uint32_t device;

    LOGV("getInput() inputSource %d, samplingRate %d, format %d, channels %x, acoustics %x", inputSource, samplingRate, format, channels, acoustics);

    AudioInputDescriptor *inputDesc = new AudioInputDescriptor();
    inputDesc->mDevice = AudioSystem::DEVICE_IN_BUILTIN_MIC;
    inputDesc->mSamplingRate = samplingRate;
    inputDesc->mFormat = format;
    inputDesc->mChannels = channels;
    inputDesc->mAcoustics = acoustics;
    inputDesc->mRefCount = 0;
    input = mpClientInterface->openInput(&inputDesc->mDevice,
                                    &inputDesc->mSamplingRate,
                                    &inputDesc->mFormat,
                                    &inputDesc->mChannels,
                                    inputDesc->mAcoustics);

    // only accept input with the exact requested set of parameters
    if ((samplingRate != inputDesc->mSamplingRate) ||
        (format != inputDesc->mFormat) ||
        (channels != inputDesc->mChannels)) {
        LOGV("getOutput() failed opening input: samplingRate %d, format %d, channels %d",
                samplingRate, format, channels);
        mpClientInterface->closeInput(input);
        delete inputDesc;
        return 0;
    }
    mInputs.add(input, inputDesc);
    return input;
}

status_t AudioPolicyManagerALSA::startInput(audio_io_handle_t input)
{
    LOGV("startInput() input %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        LOGW("startInput() unknow input %d", input);
        return BAD_VALUE;
    }
    AudioInputDescriptor *inputDesc = mInputs.valueAt(index);

    // refuse 2 active AudioRecord clients at the same time
    for (size_t i = 0; i < mInputs.size(); i++) {
        if (mInputs.valueAt(i)->mRefCount > 0) {
            LOGW("startInput() input %d, other input %d already started", input, mInputs.keyAt(i));
            return INVALID_OPERATION;
        }
    }

    inputDesc->mRefCount = 1;
    return NO_ERROR;
}

status_t AudioPolicyManagerALSA::stopInput(audio_io_handle_t input)
{
    LOGV("stopInput() input %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        LOGW("stopInput() unknow input %d", input);
        return BAD_VALUE;
    }
    AudioInputDescriptor *inputDesc = mInputs.valueAt(index);

    if (inputDesc->mRefCount == 0) {
        LOGW("stopInput() input %d already stopped", input);
        return INVALID_OPERATION;
    } else {
        inputDesc->mRefCount = 0;
        return NO_ERROR;
    }
}

void AudioPolicyManagerALSA::releaseInput(audio_io_handle_t input)
{
    LOGV("releaseInput() %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        LOGW("releaseInput() releasing unknown input %d", input);
        return;
    }
    mpClientInterface->closeInput(input);
    delete mInputs.valueAt(index);
    mInputs.removeItem(input);
}



void AudioPolicyManagerALSA::initStreamVolume(AudioSystem::stream_type stream,
                                            int indexMin,
                                            int indexMax)
{
    LOGV("initStreamVolume() stream %d, min %d, max %d", stream , indexMin, indexMax);
    mStreams[stream].mIndexMin = indexMin;
    mStreams[stream].mIndexMax = indexMax;
}

status_t AudioPolicyManagerALSA::setStreamVolumeIndex(AudioSystem::stream_type stream, int index)
{

    if ((index < mStreams[stream].mIndexMin) || (index > mStreams[stream].mIndexMax)) {
        return BAD_VALUE;
    }

    LOGV("setStreamVolumeIndex() stream %d, index %d", stream, index);
    mStreams[stream].mIndexCur = index;

    // do not change actual stream volume if the stream is muted
    if (mStreams[stream].mMuteCount != 0) {
        return NO_ERROR;
    }

    // Do not changed in call volume if bluetooth is connected and vice versa
    if ((stream == AudioSystem::VOICE_CALL && mForceUse[AudioSystem::FOR_COMMUNICATION] == AudioSystem::FORCE_BT_SCO) ||
        (stream == AudioSystem::BLUETOOTH_SCO && mForceUse[AudioSystem::FOR_COMMUNICATION] != AudioSystem::FORCE_BT_SCO)) {
        LOGV("setStreamVolumeIndex() cannot set stream %d volume with force use = %d for comm",
             stream, mForceUse[AudioSystem::FOR_COMMUNICATION]);
        return INVALID_OPERATION;
    }

    // compute and apply stream volume on all outputs according to connected device
    for (size_t i = 0; i < mOutputs.size(); i++) {
        AudioOutputDescriptor *outputDesc = mOutputs.valueAt(i);
        uint32_t device = outputDesc->device();

        float volume = computeVolume((int)stream, index, device);

        LOGV("setStreamVolume() for output %d stream %d, volume %f", mOutputs.keyAt(i), stream, volume);
        mpClientInterface->setStreamVolume(stream, volume, mOutputs.keyAt(i));
    }
    return NO_ERROR;
}

status_t AudioPolicyManagerALSA::getStreamVolumeIndex(AudioSystem::stream_type stream, int *index)
{
    if (index == 0) {
        return BAD_VALUE;
    }
    LOGV("getStreamVolumeIndex() stream %d", stream);
    *index =  mStreams[stream].mIndexCur;
    return NO_ERROR;
}

// ----------------------------------------------------------------------------
// AudioPolicyManagerALSA
// ----------------------------------------------------------------------------

// ---  class factory

AudioPolicyManagerALSA::AudioPolicyManagerALSA(AudioPolicyClientInterface *clientInterface)
    :
    mPhoneState(AudioSystem::MODE_NORMAL), mRingerMode(0)
{
    mpClientInterface = clientInterface;

    for (int i = 0; i < AudioSystem::NUM_FORCE_USE; i++) {
        mForceUse[i] = AudioSystem::FORCE_NONE;
    }

    // devices available by default are speaker, ear piece and microphone
    mAvailableOutputDevices = AudioSystem::DEVICE_OUT_SPEAKER;
    mAvailableInputDevices = AudioSystem::DEVICE_IN_BUILTIN_MIC;

    // open hardware output
    AudioOutputDescriptor *outputDesc = new AudioOutputDescriptor();
    outputDesc->mDevice = (uint32_t)AudioSystem::DEVICE_OUT_SPEAKER;
    mHardwareOutput = mpClientInterface->openOutput(&outputDesc->mDevice,
                                    &outputDesc->mSamplingRate,
                                    &outputDesc->mFormat,
                                    &outputDesc->mChannels,
                                    &outputDesc->mLatency,
                                    outputDesc->mFlags);

    if (mHardwareOutput == 0) {
        LOGE("Failed to initialize hardware output stream, samplingRate: %d, format %d, channels %d",
                outputDesc->mSamplingRate, outputDesc->mFormat, outputDesc->mChannels);
    } else {
        mOutputs.add(mHardwareOutput, outputDesc);
    }

}

AudioPolicyManagerALSA::~AudioPolicyManagerALSA()
{
   for (size_t i = 0; i < mOutputs.size(); i++) {
        mpClientInterface->closeOutput(mOutputs.keyAt(i));
        delete mOutputs.valueAt(i);
   }
   mOutputs.clear();
   for (size_t i = 0; i < mInputs.size(); i++) {
        mpClientInterface->closeInput(mInputs.keyAt(i));
        delete mInputs.valueAt(i);
   }
   mInputs.clear();
}

// ---

AudioPolicyManagerALSA::routing_strategy AudioPolicyManagerALSA::getStrategy(AudioSystem::stream_type stream)
{
    // stream to strategy mapping
    switch (stream) {
    case AudioSystem::VOICE_CALL:
    case AudioSystem::BLUETOOTH_SCO:
        return STRATEGY_PHONE;
    case AudioSystem::RING:
    case AudioSystem::NOTIFICATION:
    case AudioSystem::ALARM:
    case AudioSystem::ENFORCED_AUDIBLE:
        return STRATEGY_SONIFICATION;
    case AudioSystem::DTMF:
        return STRATEGY_DTMF;
    default:
        LOGE("unknown stream type");
    case AudioSystem::SYSTEM:
        // NOTE: SYSTEM stream uses MEDIA strategy because muting music and switching outputs
        // while key clicks are played produces a poor result
    case AudioSystem::TTS:
    case AudioSystem::MUSIC:
        return STRATEGY_MEDIA;
    }
}


float AudioPolicyManagerALSA::computeVolume(int stream, int index, uint32_t device)
{
    float volume = 1.0;

    StreamDescriptor &streamDesc = mStreams[stream];

    // Force max volume if stream cannot be muted
    if (!streamDesc.mCanBeMuted) index = streamDesc.mIndexMax;

    int volInt = (100 * (index - streamDesc.mIndexMin)) / (streamDesc.mIndexMax - streamDesc.mIndexMin);
    volume = AudioSystem::linearToLog(volInt);

    return volume;
}

void AudioPolicyManagerALSA::setStreamMute(int stream, bool on, audio_io_handle_t output)
{
    LOGV("setStreamMute() stream %d, mute %d, output %d", stream, on, output);

    StreamDescriptor &streamDesc = mStreams[stream];

    if (on) {
        if (streamDesc.mMuteCount++ == 0) {
            if (streamDesc.mCanBeMuted) {
                mpClientInterface->setStreamVolume((AudioSystem::stream_type)stream, 0, output);
            }
        }
    } else {
        if (streamDesc.mMuteCount == 0) {
            LOGW("setStreamMute() unmuting non muted stream!");
            return;
        }
        if (--streamDesc.mMuteCount == 0) {
            uint32_t device = mOutputs.valueFor(output)->mDevice;
            float volume = computeVolume(stream, streamDesc.mIndexCur, device);
            mpClientInterface->setStreamVolume((AudioSystem::stream_type)stream, volume, output);
        }
    }
}

void AudioPolicyManagerALSA::handleIncallSonification(int stream, bool starting)
{
    // if the stream pertains to sonification strategy and we are in call we must
    // mute the stream if it is low visibility. If it is high visibility, we must play a tone
    // in the device used for phone strategy and play the tone if the selected device does not
    // interfere with the device used for phone strategy
    if (getStrategy((AudioSystem::stream_type)stream) == STRATEGY_SONIFICATION) {
        AudioOutputDescriptor *outputDesc = mOutputs.valueFor(mHardwareOutput);
        LOGV("handleIncallSonification() stream %d starting %d device %x", stream, starting, outputDesc->mDevice);
        if (outputDesc->isUsedByStream((AudioSystem::stream_type)stream)) {
            if (AudioSystem::isLowVisibility((AudioSystem::stream_type)stream)) {
                LOGV("handleIncallSonification() low visibility");
                setStreamMute(stream, starting, mHardwareOutput);
            } else {
                if (starting) {
                    mpClientInterface->startTone(ToneGenerator::TONE_SUP_CALL_WAITING, AudioSystem::VOICE_CALL);
                } else {
                    mpClientInterface->stopTone();
                }
            }
        }
    }
}


// --- AudioOutputDescriptor class implementation

AudioPolicyManagerALSA::AudioOutputDescriptor::AudioOutputDescriptor()
    : mSamplingRate(0), mFormat(0), mChannels(0), mLatency(0),
    mFlags((AudioSystem::output_flags)0), mDevice(0)
{
    // clear usage count for all stream types
    for (int i = 0; i < AudioSystem::NUM_STREAM_TYPES; i++) {
        mRefCount[i] = 0;
    }
}

uint32_t AudioPolicyManagerALSA::AudioOutputDescriptor::device()
{
    return mDevice;
}

void AudioPolicyManagerALSA::AudioOutputDescriptor::changeRefCount(AudioSystem::stream_type stream, int delta)
{
    if ((delta + (int)mRefCount[stream]) < 0) {
        LOGW("changeRefCount() invalid delta %d for stream %d, refCount %d", delta, stream, mRefCount[stream]);
        mRefCount[stream] = 0;
        return;
    }
    mRefCount[stream] += delta;
    LOGV("changeRefCount() stream %d, count %d", stream, mRefCount[stream]);
}

uint32_t AudioPolicyManagerALSA::AudioOutputDescriptor::refCount()
{
    uint32_t refcount = 0;
    for (int i = 0; i < (int)AudioSystem::NUM_STREAM_TYPES; i++) {
        refcount += mRefCount[i];
    }
    return refcount;
}

// --- AudioInputDescriptor class implementation

AudioPolicyManagerALSA::AudioInputDescriptor::AudioInputDescriptor()
    : mSamplingRate(0), mFormat(0), mChannels(0),
     mAcoustics((AudioSystem::audio_in_acoustics)0), mDevice(0), mRefCount(0)
{
}

}; // namespace android
