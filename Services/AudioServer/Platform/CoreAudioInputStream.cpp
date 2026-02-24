/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/Server.h>
#include <AudioToolbox/AudioToolbox.h>

namespace AudioServer {

class CoreAudioInputStream final : public InputStream {
public:
    static ErrorOr<NonnullRefPtr<InputStream>> create(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
    {
        auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) CoreAudioInputStream()));
        TRY(stream->initialize_shared_ring_storage(sample_rate_hz, channel_count, capacity_frames));
        TRY(stream->initialize_unit(device_handle, sample_rate_hz, channel_count));
        return stream;
    }

    ~CoreAudioInputStream() override
    {
        if (m_unit) {
            AudioOutputUnitStop(m_unit);
            AudioUnitUninitialize(m_unit);
            AudioComponentInstanceDispose(m_unit);
        }
    }

    explicit CoreAudioInputStream() = default;

    ErrorOr<void> initialize_unit(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count)
    {
        AudioComponentDescription description {};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;

        auto* component = AudioComponentFindNext(nullptr, &description);
        if (!component)
            return Error::from_string_literal("failed to locate HAL output audio unit");

        OSStatus status = AudioComponentInstanceNew(component, &m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        UInt32 enable_input = 1;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable_input, sizeof(enable_input));
        if (status != noErr)
            return Error::from_errno(status);

        UInt32 disable_output = 0;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disable_output, sizeof(disable_output));
        if (status != noErr)
            return Error::from_errno(status);

        AudioObjectID device = static_cast<AudioObjectID>(device_handle);
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
        if (status != noErr)
            return Error::from_errno(status);

        AudioStreamBasicDescription format {};
        format.mSampleRate = static_cast<Float64>(sample_rate_hz);
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mBitsPerChannel = 32;
        format.mChannelsPerFrame = channel_count;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = channel_count * sizeof(float);
        format.mBytesPerPacket = format.mFramesPerPacket * format.mBytesPerFrame;

        status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, sizeof(format));
        if (status != noErr)
            return Error::from_errno(status);

        AURenderCallbackStruct callback {};
        callback.inputProc = &CoreAudioInputStream::input_callback;
        callback.inputProcRefCon = this;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof(callback));
        if (status != noErr)
            return Error::from_errno(status);

        status = AudioUnitInitialize(m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        status = AudioOutputUnitStart(m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        return {};
    }

    static OSStatus input_callback(void* user_data, AudioUnitRenderActionFlags* flags, AudioTimeStamp const* timestamp, UInt32 bus_number, UInt32 frames, [[maybe_unused]] AudioBufferList* io_data)
    {
        auto* stream = static_cast<CoreAudioInputStream*>(user_data);
        if (!stream || !stream->m_unit)
            return noErr;

        size_t required_samples = static_cast<size_t>(frames) * stream->channel_count();
        if (stream->m_input_buffer.size() < required_samples)
            stream->m_input_buffer.resize(required_samples);

        AudioBufferList buffer_list {};
        buffer_list.mNumberBuffers = 1;
        buffer_list.mBuffers[0].mNumberChannels = stream->channel_count();
        buffer_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(required_samples * sizeof(float));
        buffer_list.mBuffers[0].mData = stream->m_input_buffer.data();

        OSStatus status = AudioUnitRender(stream->m_unit, flags, timestamp, bus_number, frames, &buffer_list);
        if (status != noErr)
            return status;

        ReadonlySpan<float> interleaved { stream->m_input_buffer.data(), required_samples };
        (void)stream->try_push_interleaved(interleaved, stream->channel_count());
        return noErr;
    }

    AudioComponentInstance m_unit { nullptr };
    Vector<float> m_input_buffer;
};

ErrorOr<NonnullRefPtr<InputStream>> create_platform_input_stream(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
{
    DeviceHandle raw_handle = static_cast<DeviceHandle>(Server::device_handle_to_os_device_id(device_handle));
    return CoreAudioInputStream::create(raw_handle, sample_rate_hz, channel_count, capacity_frames);
}

}
