/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/RecordStream.h>

#include <AudioToolbox/AudioConverter.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

namespace Audio {

// AUHAL units address the input (capture) side as element 1 and the output side as element 0.
static constexpr AudioUnitElement AUDIO_UNIT_INPUT_ELEMENT = 1;
static constexpr AudioUnitElement AUDIO_UNIT_OUTPUT_ELEMENT = 0;

#define AU_TRY(expression)                                                         \
    ({                                                                             \
        /* Ignore -Wshadow to allow nesting the macro. */                          \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto&& _temporary_result = (expression)); \
        if (_temporary_result != noErr) [[unlikely]]                               \
            return Error::from_string_literal("Core Audio operation failed");      \
    })

static ErrorOr<AudioDeviceID> find_input_device(StringView device_id)
{
    // AudioDevices encodes Core Audio input devices as "coreaudio:input:<uid>". Unrecognized
    // ids and devices that have since vanished fall through to the default input device.
    constexpr auto core_audio_input_prefix = "coreaudio:input:"sv;
    if (device_id.starts_with(core_audio_input_prefix)) {
        auto uid = ByteString { device_id.substring_view(core_audio_input_prefix.length()) };
        auto* uid_string = CFStringCreateWithCString(kCFAllocatorDefault, uid.characters(), kCFStringEncodingUTF8);
        if (uid_string != nullptr) {
            ScopeGuard release_uid_string { [&] { CFRelease(uid_string); } };
            AudioObjectPropertyAddress translate_address {
                kAudioHardwarePropertyTranslateUIDToDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain,
            };
            AudioDeviceID device = kAudioObjectUnknown;
            UInt32 size = sizeof(device);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate_address, sizeof(uid_string), &uid_string, &size, &device) == noErr
                && device != kAudioObjectUnknown) {
                return device;
            }
        }
    }

    AudioObjectPropertyAddress default_input_address {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AU_TRY(AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_input_address, 0, nullptr, &size, &device));
    if (device == kAudioObjectUnknown)
        return Error::from_string_literal("No default audio input device is available");
    return device;
}

class RecordStreamAudioUnit final : public RecordStream {
public:
    static ErrorOr<NonnullRefPtr<RecordStreamAudioUnit>> create(SampleSpecification const& requested_specification, StringView device_id, RecordCallback callback)
    {
        auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RecordStreamAudioUnit(move(callback))));
        TRY(stream->initialize(requested_specification, device_id));
        return stream;
    }

    virtual ~RecordStreamAudioUnit() override
    {
        // AudioOutputUnitStop() waits for any in-flight input callback to return, so the
        // capture thread can never observe a partially destroyed stream.
        if (m_audio_unit != nullptr) {
            AudioOutputUnitStop(m_audio_unit);
            AudioUnitUninitialize(m_audio_unit);
            AudioComponentInstanceDispose(m_audio_unit);
        }
        if (m_converter != nullptr)
            AudioConverterDispose(m_converter);
    }

    virtual SampleSpecification const& sample_specification() const override { return m_output_specification; }

private:
    explicit RecordStreamAudioUnit(RecordCallback callback)
        : m_callback(move(callback))
    {
    }

    ErrorOr<void> initialize(SampleSpecification const& requested_specification, StringView device_id)
    {
        auto device = TRY(find_input_device(device_id));

        AudioComponentDescription component_description;
        component_description.componentType = kAudioUnitType_Output;
        component_description.componentSubType = kAudioUnitSubType_HALOutput;
        component_description.componentManufacturer = kAudioUnitManufacturer_Apple;
        component_description.componentFlags = 0;
        component_description.componentFlagsMask = 0;

        auto* component = AudioComponentFindNext(nullptr, &component_description);
        if (component == nullptr)
            return Error::from_string_literal("Unable to find the AUHAL audio component");
        AU_TRY(AudioComponentInstanceNew(component, &m_audio_unit));

        // Capture only: enable IO on the input element and disable the output element.
        UInt32 enable_io = 1;
        AU_TRY(AudioUnitSetProperty(m_audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, AUDIO_UNIT_INPUT_ELEMENT, &enable_io, sizeof(enable_io)));
        UInt32 disable_io = 0;
        AU_TRY(AudioUnitSetProperty(m_audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, AUDIO_UNIT_OUTPUT_ELEMENT, &disable_io, sizeof(disable_io)));

        AU_TRY(AudioUnitSetProperty(m_audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, AUDIO_UNIT_OUTPUT_ELEMENT, &device, sizeof(device)));

        AudioStreamBasicDescription device_format {};
        UInt32 size = sizeof(device_format);
        AU_TRY(AudioUnitGetProperty(m_audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, AUDIO_UNIT_INPUT_ELEMENT, &device_format, &size));

        auto device_sample_rate = static_cast<u32>(device_format.mSampleRate);
        if (device_sample_rate == 0)
            return Error::from_string_literal("Audio input device reports an invalid sample rate");

        // Requesting more channels than the device provides would only pad with silence;
        // deliver the smaller layout and let consumers up-mix as they see fit.
        // FIXME: Support channel layouts beyond mono and stereo.
        u32 channel_count = min(max(requested_specification.channel_count(), u8 { 1 }), min(max(device_format.mChannelsPerFrame, 1u), 2u));
        auto channel_map = channel_count == 1 ? ChannelMap::mono() : ChannelMap::stereo();

        // The AUHAL converts channel count and sample format between the device format and
        // this client format, but not the sample rate, so capture happens at the device rate
        // and the converter below resamples to the requested rate.
        AudioStreamBasicDescription client_format {};
        client_format.mSampleRate = device_format.mSampleRate;
        client_format.mFormatID = kAudioFormatLinearPCM;
        client_format.mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
        client_format.mFramesPerPacket = 1;
        client_format.mChannelsPerFrame = channel_count;
        client_format.mBitsPerChannel = 32;
        client_format.mBytesPerFrame = channel_count * sizeof(float);
        client_format.mBytesPerPacket = client_format.mBytesPerFrame;
        AU_TRY(AudioUnitSetProperty(m_audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, AUDIO_UNIT_INPUT_ELEMENT, &client_format, sizeof(client_format)));

        m_device_specification = SampleSpecification(device_sample_rate, channel_map);
        m_output_specification = SampleSpecification(requested_specification.sample_rate(), channel_map);

        if (device_sample_rate != m_output_specification.sample_rate()) {
            auto converter_output_format = client_format;
            converter_output_format.mSampleRate = m_output_specification.sample_rate();
            AU_TRY(AudioConverterNew(&client_format, &converter_output_format, &m_converter));
        }

        AURenderCallbackStruct callback_description;
        callback_description.inputProc = &RecordStreamAudioUnit::on_input_available;
        callback_description.inputProcRefCon = this;
        AU_TRY(AudioUnitSetProperty(m_audio_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, AUDIO_UNIT_OUTPUT_ELEMENT, &callback_description, sizeof(callback_description)));

        AU_TRY(AudioUnitInitialize(m_audio_unit));

        // Pre-size the capture buffer so the input callback normally never allocates.
        UInt32 maximum_frames_per_slice = 0;
        size = sizeof(maximum_frames_per_slice);
        AU_TRY(AudioUnitGetProperty(m_audio_unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, AUDIO_UNIT_OUTPUT_ELEMENT, &maximum_frames_per_slice, &size));
        TRY(m_capture_buffer.try_resize(static_cast<size_t>(maximum_frames_per_slice) * channel_count));

        AU_TRY(AudioOutputUnitStart(m_audio_unit));

        return {};
    }

    static OSStatus on_input_available(void* user_data, AudioUnitRenderActionFlags* action_flags, AudioTimeStamp const* time_stamp, UInt32 element, UInt32 frame_count, AudioBufferList*)
    {
        auto& stream = *static_cast<RecordStreamAudioUnit*>(user_data);

        auto channel_count = stream.m_device_specification.channel_count();
        auto required_samples = static_cast<size_t>(frame_count) * channel_count;
        if (stream.m_capture_buffer.size() < required_samples)
            stream.m_capture_buffer.resize(required_samples);

        AudioBufferList buffer_list;
        buffer_list.mNumberBuffers = 1;
        buffer_list.mBuffers[0].mNumberChannels = channel_count;
        buffer_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(required_samples * sizeof(float));
        buffer_list.mBuffers[0].mData = stream.m_capture_buffer.data();

        auto status = AudioUnitRender(stream.m_audio_unit, action_flags, time_stamp, element, frame_count, &buffer_list);
        if (status != noErr)
            return status;

        stream.deliver_frames(stream.m_capture_buffer.data(), frame_count);
        return noErr;
    }

    void deliver_frames(float const* samples, UInt32 frame_count)
    {
        if (frame_count == 0 || !m_callback)
            return;

        auto channel_count = m_device_specification.channel_count();

        if (m_converter == nullptr) {
            m_callback(ReadonlyBytes { samples, static_cast<size_t>(frame_count) * channel_count * sizeof(float) }, m_output_specification);
            return;
        }

        // Resample to the requested rate. The converter pulls its input through
        // supply_converter_input(), which hands over the pending capture buffer exactly once
        // per fill; the sentinel status stops the pull without being treated as an error.
        m_pending_input = samples;
        m_pending_input_frames = frame_count;

        auto ratio = static_cast<double>(m_output_specification.sample_rate()) / m_device_specification.sample_rate();
        auto output_capacity_frames = static_cast<UInt32>(static_cast<double>(frame_count) * ratio) + 32;
        auto output_capacity_samples = static_cast<size_t>(output_capacity_frames) * channel_count;
        if (m_converted_buffer.size() < output_capacity_samples)
            m_converted_buffer.resize(output_capacity_samples);

        while (true) {
            AudioBufferList output_list;
            output_list.mNumberBuffers = 1;
            output_list.mBuffers[0].mNumberChannels = channel_count;
            output_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(output_capacity_samples * sizeof(float));
            output_list.mBuffers[0].mData = m_converted_buffer.data();

            UInt32 output_frames = output_capacity_frames;
            auto status = AudioConverterFillComplexBuffer(m_converter, &RecordStreamAudioUnit::supply_converter_input, this, &output_frames, &output_list, nullptr);

            if (output_frames > 0)
                m_callback(ReadonlyBytes { m_converted_buffer.data(), static_cast<size_t>(output_frames) * channel_count * sizeof(float) }, m_output_specification);

            if (status != noErr || output_frames == 0)
                break;
        }
    }

    // Arbitrary nonzero status ("lbnd") signaling that the pending capture buffer has been
    // consumed; AudioConverterFillComplexBuffer() surfaces it after producing whatever
    // output it can, distinguishing "out of input for now" from a real conversion error.
    static constexpr OSStatus no_more_input_status = 0x6c626e64;

    static OSStatus supply_converter_input(AudioConverterRef, UInt32* io_packet_count, AudioBufferList* io_data, AudioStreamPacketDescription**, void* user_data)
    {
        auto& stream = *static_cast<RecordStreamAudioUnit*>(user_data);

        if (stream.m_pending_input_frames == 0) {
            *io_packet_count = 0;
            return no_more_input_status;
        }

        auto channel_count = stream.m_device_specification.channel_count();
        io_data->mNumberBuffers = 1;
        io_data->mBuffers[0].mNumberChannels = channel_count;
        io_data->mBuffers[0].mDataByteSize = static_cast<UInt32>(stream.m_pending_input_frames * channel_count * sizeof(float));
        io_data->mBuffers[0].mData = const_cast<float*>(stream.m_pending_input);

        *io_packet_count = stream.m_pending_input_frames;
        stream.m_pending_input_frames = 0;
        return noErr;
    }

    AudioComponentInstance m_audio_unit { nullptr };
    AudioConverterRef m_converter { nullptr };

    // The client format the AUHAL delivers (device sample rate) and the format handed to the
    // record callback (requested sample rate); they differ only when the converter is active.
    SampleSpecification m_device_specification;
    SampleSpecification m_output_specification;

    // Only touched on the capture thread.
    Vector<float> m_capture_buffer;
    Vector<float> m_converted_buffer;
    float const* m_pending_input { nullptr };
    UInt32 m_pending_input_frames { 0 };

    RecordCallback m_callback;
};

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create(SampleSpecification const& specification, u32, StringView device_id, RecordCallback callback)
{
    auto promise = CreatePromise::construct();
    auto stream_or_error = RecordStreamAudioUnit::create(specification, device_id, move(callback));
    if (stream_or_error.is_error()) {
        promise->reject(stream_or_error.release_error());
        return promise;
    }

    NonnullRefPtr<RecordStream> stream = stream_or_error.release_value();
    promise->resolve(move(stream));
    return promise;
}

}
