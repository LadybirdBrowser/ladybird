/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AudioServer/Debug.h>
#include <AudioServer/Platform/CoreAudio.h>
#include <AudioServer/Server.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <LibMedia/Audio/ChannelMap.h>

namespace AudioServer {

using Audio::Channel;
using Audio::ChannelMap;

static OSStatus coreaudio_device_change_listener([[maybe_unused]] AudioObjectID in_object_id, [[maybe_unused]] UInt32 in_number_addresses, [[maybe_unused]] AudioObjectPropertyAddress const in_addresses[], [[maybe_unused]] void* in_client_data)
{
    Server::the().update_devices();
    return noErr;
}

static void ensure_coreaudio_device_change_notifications_registered()
{
    static bool registered = false;
    static bool attempted = false;
    if (registered || attempted)
        return;
    attempted = true;

    AudioObjectPropertyAddress devices_address {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioObjectPropertyAddress default_output_address {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioObjectPropertyAddress default_input_address {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    OSStatus devices_status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices_address, coreaudio_device_change_listener, nullptr);
    OSStatus output_status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &default_output_address, coreaudio_device_change_listener, nullptr);
    OSStatus input_status = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &default_input_address, coreaudio_device_change_listener, nullptr);

    registered = (devices_status == noErr && output_status == noErr && input_status == noErr);
}

static ChannelMap create_unknown_channel_layout(u32 channel_count)
{
    if (channel_count > ChannelMap::capacity())
        return ChannelMap::invalid();

    Vector<Audio::Channel, ChannelMap::capacity()> channel_layout;
    channel_layout.resize(channel_count);

    for (auto& channel : channel_layout)
        channel = Audio::Channel::Unknown;

    return ChannelMap(channel_layout);
}

static ChannelMap channel_layout_from_channel_count(u32 channel_count)
{
    if (channel_count == 1)
        return ChannelMap(Vector<Audio::Channel, ChannelMap::capacity()> { Audio::Channel::FrontCenter });

    if (channel_count == 2)
        return ChannelMap(Vector<Audio::Channel, ChannelMap::capacity()> { Audio::Channel::FrontLeft, Audio::Channel::FrontRight });

    return create_unknown_channel_layout(channel_count);
}

// Keep this order in sync with AudioChannelBitmap.
#define ENUMERATE_CHANNEL_POSITIONS(C)                 \
    C(Left, Audio::Channel::FrontLeft)                 \
    C(Right, Audio::Channel::FrontRight)               \
    C(Center, Audio::Channel::FrontCenter)             \
    C(LFEScreen, Audio::Channel::LowFrequency)         \
    C(LeftSurround, Audio::Channel::BackLeft)          \
    C(RightSurround, Audio::Channel::BackRight)        \
    C(LeftCenter, Audio::Channel::FrontLeftOfCenter)   \
    C(RightCenter, Audio::Channel::FrontRightOfCenter) \
    C(CenterSurround, Audio::Channel::BackCenter)      \
    C(LeftSurroundDirect, Audio::Channel::SideLeft)    \
    C(RightSurroundDirect, Audio::Channel::SideRight)  \
    C(TopCenterSurround, Audio::Channel::TopCenter)    \
    C(TopBackLeft, Audio::Channel::TopBackLeft)        \
    C(TopBackCenter, Audio::Channel::TopBackCenter)    \
    C(TopBackRight, Audio::Channel::TopBackRight)      \
    C(LeftTopFront, Audio::Channel::TopFrontLeft)      \
    C(CenterTopFront, Audio::Channel::TopFrontCenter)  \
    C(RightTopFront, Audio::Channel::TopFrontRight)

static void check_audio_channel_layout_size(AudioChannelLayout const& layout, u32 size)
{
    auto minimum_layout_size = Checked(sizeof(AudioChannelLayout));
    if (layout.mNumberChannelDescriptions == 0) {
        minimum_layout_size -= sizeof(layout.mChannelDescriptions[0]);
    } else {
        auto extra_descriptions = Checked(layout.mNumberChannelDescriptions);
        extra_descriptions--;
        extra_descriptions *= sizeof(layout.mChannelDescriptions[0]);
        minimum_layout_size += extra_descriptions.value();
    }
    VERIFY(size >= minimum_layout_size.value());
}

static Optional<ByteString> device_name(AudioObjectID device_id);

ChannelMap device_channel_layout(AudioChannelLayout const& layout, u32 layout_size, u32 expected_channel_count)
{
    check_audio_channel_layout_size(layout, layout_size);

    if (should_log_audio_server())
        dbgln("CoreAudio layout input: tag={}, bitmap={:#x}, descriptions={}, expected_channels={}", layout.mChannelLayoutTag, layout.mChannelBitmap, layout.mNumberChannelDescriptions, expected_channel_count);

    if (layout.mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelBitmap
        && layout.mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
        u32 explicit_layout_size = 0;
        OSStatus status = AudioFormatGetPropertyInfo(
            kAudioFormatProperty_ChannelLayoutForTag,
            sizeof(AudioChannelLayoutTag),
            &layout.mChannelLayoutTag,
            &explicit_layout_size);
        if (status != noErr || explicit_layout_size < sizeof(AudioChannelLayout)) {
            if (should_log_audio_server())
                dbgln("CoreAudio layout output: {}", create_unknown_channel_layout(expected_channel_count));
            return create_unknown_channel_layout(expected_channel_count);
        }

        auto* explicit_layout = reinterpret_cast<AudioChannelLayout*>(malloc(explicit_layout_size));
        if (!explicit_layout) {
            if (should_log_audio_server())
                dbgln("CoreAudio layout output: {}", create_unknown_channel_layout(expected_channel_count));
            return create_unknown_channel_layout(expected_channel_count);
        }
        ScopeGuard free_explicit_layout = [&] { free(explicit_layout); };

        status = AudioFormatGetProperty(
            kAudioFormatProperty_ChannelLayoutForTag,
            sizeof(AudioChannelLayoutTag),
            &layout.mChannelLayoutTag,
            &explicit_layout_size,
            explicit_layout);
        if (status != noErr) {
            if (should_log_audio_server())
                dbgln("CoreAudio layout output: {}", create_unknown_channel_layout(expected_channel_count));
            return create_unknown_channel_layout(expected_channel_count);
        }

        return device_channel_layout(*explicit_layout, explicit_layout_size, expected_channel_count);
    }

    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG(core_audio_channel_name, audio_channel) \
    if ((layout.mChannelBitmap & kAudioChannelBit_##core_audio_channel_name) != 0) \
        channels.append(audio_channel);

#define MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION(core_audio_channel_name, audio_channel) \
    case kAudioChannelLabel_##core_audio_channel_name:                                     \
        channels.append(audio_channel);                                                    \
        break;

    if (layout.mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG)
    } else {
        VERIFY(layout.mNumberChannelDescriptions > 0);
        auto const* channel_descriptions = &layout.mChannelDescriptions[0];
        for (u32 i = 0; i < layout.mNumberChannelDescriptions; ++i) {
            if (should_log_audio_server())
                dbgln("CoreAudio layout label[{}]: {}", i, channel_descriptions[i].mChannelLabel);
            switch (channel_descriptions[i].mChannelLabel) {
                ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_CHANNEL_DESCRIPTION)
            case kAudioChannelLabel_LeftSideSurround:
                channels.append(Audio::Channel::SideLeft);
                break;
            case kAudioChannelLabel_RightSideSurround:
                channels.append(Audio::Channel::SideRight);
                break;
            case kAudioChannelLabel_RearSurroundLeft:
            case kAudioChannelLabel_LeftBackSurround:
                channels.append(Audio::Channel::BackLeft);
                break;
            case kAudioChannelLabel_RearSurroundRight:
            case kAudioChannelLabel_RightBackSurround:
                channels.append(Audio::Channel::BackRight);
                break;
            case kAudioChannelLabel_Mono:
                channels.append(Audio::Channel::FrontCenter);
                break;
            default:
                channels.append(Audio::Channel::Unknown);
                break;
            }
        }
    }

    if (channels.size() > ChannelMap::capacity())
        return ChannelMap::invalid();

    ChannelMap channel_layout(channels);
    if (!channel_layout.is_valid() || channel_layout.channel_count() != expected_channel_count) {
        if (should_log_audio_server())
            dbgln("CoreAudio layout output: {}", create_unknown_channel_layout(expected_channel_count));
        return create_unknown_channel_layout(expected_channel_count);
    }

    if (should_log_audio_server())
        dbgln("CoreAudio layout output: {}", channel_layout);

    return channel_layout;
}

static ChannelMap device_channel_layout(AudioObjectID device_id, AudioObjectPropertyScope scope, u32 channel_count)
{
    AudioObjectPropertyAddress streams_address {
        kAudioDevicePropertyStreams,
        scope,
        kAudioObjectPropertyElementMain,
    };
    UInt32 streams_size = 0;
    OSStatus streams_status = AudioObjectGetPropertyDataSize(device_id, &streams_address, 0, nullptr, &streams_size);
    if (streams_status == noErr && streams_size >= sizeof(AudioObjectID)) {
        auto streams_buffer_or_error = ByteBuffer::create_uninitialized(streams_size);
        if (!streams_buffer_or_error.is_error()) {
            auto streams_buffer = streams_buffer_or_error.release_value();
            auto* stream_ids = reinterpret_cast<AudioObjectID*>(streams_buffer.data());
            streams_status = AudioObjectGetPropertyData(device_id, &streams_address, 0, nullptr, &streams_size, stream_ids);
            if (streams_status == noErr) {
                size_t stream_count = streams_size / sizeof(AudioObjectID);
                u32 total_stream_channels = 0;
                for (size_t i = 0; i < stream_count; ++i) {
                    if (should_log_audio_server())
                        dbgln("CoreAudio: Stream probe: {}, device_id={}, object_id={}, scope={}", device_name(device_id).value_or(ByteString()), device_id, stream_ids[i], scope);

                    AudioObjectPropertyAddress stream_format_address {
                        kAudioStreamPropertyVirtualFormat,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain,
                    };
                    AudioStreamBasicDescription stream_format {};
                    UInt32 stream_format_size = sizeof(stream_format);
                    OSStatus stream_status = AudioObjectGetPropertyData(stream_ids[i], &stream_format_address, 0, nullptr, &stream_format_size, &stream_format);
                    if (stream_status != noErr || stream_format_size != sizeof(stream_format))
                        continue;

                    total_stream_channels += stream_format.mChannelsPerFrame;
                }

                if (total_stream_channels == channel_count)
                    return channel_layout_from_channel_count(channel_count);
            }
        }
    }

    return channel_layout_from_channel_count(channel_count);
}

static ByteString cfstring_to_bytes(CFStringRef string)
{
    if (string == nullptr)
        return {};

    CFIndex length = CFStringGetLength(string);
    if (length == 0)
        return {};

    CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    if (max_size <= 1)
        return {};

    auto buffer_or_error = ByteBuffer::create_uninitialized(static_cast<size_t>(max_size));
    if (buffer_or_error.is_error())
        return {};

    auto buffer = buffer_or_error.release_value();
    auto* bytes = reinterpret_cast<char*>(buffer.data());
    if (!CFStringGetCString(string, bytes, max_size, kCFStringEncodingUTF8))
        return {};

    return ByteString(bytes);
}

static Optional<AudioObjectID> default_device_id(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    AudioObjectID device_id = kAudioObjectUnknown;
    UInt32 size = sizeof(device_id);
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device_id);
    if (status != noErr)
        return {};

    if (device_id == kAudioObjectUnknown)
        return {};

    return device_id;
}

static u32 device_channel_count(AudioObjectID device_id, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(device_id, &address, 0, nullptr, &size);
    if (status != noErr || size == 0)
        return 0;

    auto buffer_or_error = ByteBuffer::create_uninitialized(size);
    if (buffer_or_error.is_error())
        return 0;

    auto buffer = buffer_or_error.release_value();
    auto* list = reinterpret_cast<AudioBufferList*>(buffer.data());
    status = AudioObjectGetPropertyData(device_id, &address, 0, nullptr, &size, list);
    if (status != noErr)
        return 0;

    u32 channel_count = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channel_count += list->mBuffers[i].mNumberChannels;

    return channel_count;
}

static Optional<ByteString> device_name(AudioObjectID device_id)
{
    AudioObjectPropertyAddress address {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    CFStringRef value = nullptr;
    UInt32 size = sizeof(CFStringRef);
    OSStatus status = AudioObjectGetPropertyData(device_id, &address, 0, nullptr, &size, reinterpret_cast<void*>(&value));
    if (status != noErr)
        return {};

    auto result = cfstring_to_bytes(value);
    if (value)
        CFRelease(value);
    return result;
}

static Optional<ByteString> device_uid(AudioObjectID device_id)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    CFStringRef value = nullptr;
    UInt32 size = sizeof(CFStringRef);
    OSStatus status = AudioObjectGetPropertyData(device_id, &address, 0, nullptr, &size, reinterpret_cast<void*>(&value));
    if (status != noErr)
        return {};

    auto result = cfstring_to_bytes(value);
    if (value)
        CFRelease(value);
    return result;
}

static u32 device_sample_rate(AudioObjectID device_id)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    Float64 sample_rate = 0;
    UInt32 size = sizeof(sample_rate);
    OSStatus status = AudioObjectGetPropertyData(device_id, &address, 0, nullptr, &size, &sample_rate);
    if (status != noErr || sample_rate <= 0.0)
        return 0;

    return static_cast<u32>(sample_rate);
}

Vector<DeviceInfo> Server::enumerate_platform_devices()
{
    ensure_coreaudio_device_change_notifications_registered();

    Vector<DeviceInfo> devices;

    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if (status != noErr || size == 0)
        return devices;

    auto buffer_or_error = ByteBuffer::create_uninitialized(size);
    if (buffer_or_error.is_error())
        return devices;

    auto buffer = buffer_or_error.release_value();
    auto* device_ids = reinterpret_cast<AudioObjectID*>(buffer.data());
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, device_ids);
    if (status != noErr)
        return devices;

    Optional<AudioObjectID> default_output = default_device_id(kAudioHardwarePropertyDefaultOutputDevice);
    Optional<AudioObjectID> default_input = default_device_id(kAudioHardwarePropertyDefaultInputDevice);

    size_t count = size / sizeof(AudioObjectID);
    devices.ensure_capacity(count * 2);

    for (size_t i = 0; i < count; ++i) {
        AudioObjectID device_id = device_ids[i];

        ByteString label = device_name(device_id).value_or(ByteString());
        ByteString persistent_id = device_uid(device_id).value_or(ByteString());
        u32 sample_rate_hz = device_sample_rate(device_id);

        u32 output_channels = device_channel_count(device_id, kAudioDevicePropertyScopeOutput);
        if (output_channels > 0) {
            bool is_default = default_output.has_value() && default_output.value() == device_id;
            auto channel_layout = device_channel_layout(device_id, kAudioDevicePropertyScopeOutput, output_channels);
            if (should_log_audio_server())
                dbgln("Enumerating output device \"{}\", channels: {}, sample rate: {}, default: {}", label, output_channels, sample_rate_hz, is_default);

            devices.append(DeviceInfo {
                .type = DeviceInfo::Type::Output,
                .device_handle = Server::make_device_handle(static_cast<u64>(device_id), DeviceInfo::Type::Output),
                .label = label,
                .dom_device_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("audiooutput"sv, persistent_id, static_cast<u64>(device_id)),
                .group_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("group"sv, persistent_id, static_cast<u64>(device_id)),
                .sample_rate_hz = sample_rate_hz,
                .channel_count = output_channels,
                .channel_layout = channel_layout,
                .is_default = is_default,
            });
        }

        u32 input_channels = device_channel_count(device_id, kAudioDevicePropertyScopeInput);
        if (input_channels > 0) {
            bool is_default = default_input.has_value() && default_input.value() == device_id;
            auto channel_layout = device_channel_layout(device_id, kAudioDevicePropertyScopeInput, input_channels);
            if (should_log_audio_server())
                dbgln("Enumerating input device \"{}\", channels: {}, sample rate: {}, default: {}", label, input_channels, sample_rate_hz, is_default);

            devices.append(DeviceInfo {
                .type = DeviceInfo::Type::Input,
                .device_handle = Server::make_device_handle(static_cast<u64>(device_id), DeviceInfo::Type::Input),
                .label = move(label),
                .dom_device_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("audioinput"sv, persistent_id, static_cast<u64>(device_id)),
                .group_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("group"sv, persistent_id, static_cast<u64>(device_id)),
                .sample_rate_hz = sample_rate_hz,
                .channel_count = input_channels,
                .channel_layout = channel_layout,
                .is_default = is_default,
            });
        }
    }

    return devices;
}

}
