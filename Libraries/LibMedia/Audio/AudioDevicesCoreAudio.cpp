/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/kmalloc.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <LibMedia/Audio/AudioDevices.h>

namespace Media {

static ByteString core_foundation_string_to_byte_string(CFStringRef string)
{
    if (string == nullptr)
        return {};
    if (auto const* fast_path = CFStringGetCStringPtr(string, kCFStringEncodingUTF8); fast_path != nullptr)
        return fast_path;
    auto maximum_size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    Vector<char> buffer;
    buffer.resize(maximum_size);
    if (!CFStringGetCString(string, buffer.data(), maximum_size, kCFStringEncodingUTF8))
        return {};
    return ByteString { buffer.data() };
}

static ByteString device_string_property(AudioDeviceID device, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};
    ScopeGuard release_value { [&] { CFRelease(value); } };
    return core_foundation_string_to_byte_string(value);
}

static u32 device_channel_count(AudioDeviceID device, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList))
        return 0;
    auto* buffer_list = static_cast<AudioBufferList*>(kmalloc(size));
    if (buffer_list == nullptr)
        return 0;
    ScopeGuard free_buffer_list { [&] { kfree(buffer_list); } };
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, buffer_list) != noErr)
        return 0;
    u32 channel_count = 0;
    for (UInt32 i = 0; i < buffer_list->mNumberBuffers; ++i)
        channel_count += buffer_list->mBuffers[i].mNumberChannels;
    return channel_count;
}

static u32 device_nominal_sample_rate(AudioDeviceID device)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 sample_rate = 0;
    UInt32 size = sizeof(sample_rate);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &sample_rate) != noErr)
        return 0;
    return static_cast<u32>(sample_rate);
}

static AudioDeviceID default_device(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}

static ErrorOr<AudioDeviceEnumeration> enumerate_core_audio_devices()
{
    AudioDeviceEnumeration enumeration;
    AudioObjectPropertyAddress devices_address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices_address, 0, nullptr, &size) != noErr || size == 0)
        return Error::from_string_literal("Unable to list Core Audio devices");

    Vector<AudioDeviceID> devices;
    devices.resize(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_address, 0, nullptr, &size, devices.data()) != noErr)
        return Error::from_string_literal("Unable to read Core Audio devices");
    devices.resize(size / sizeof(AudioDeviceID));

    auto default_input = default_device(kAudioHardwarePropertyDefaultInputDevice);
    auto default_output = default_device(kAudioHardwarePropertyDefaultOutputDevice);

    for (auto device : devices) {
        auto uid = device_string_property(device, kAudioDevicePropertyDeviceUID);
        if (uid.is_empty())
            continue;
        auto label = device_string_property(device, kAudioObjectPropertyName);
        if (label.is_empty())
            label = uid;
        auto sample_rate = device_nominal_sample_rate(device);

        if (auto input_channels = device_channel_count(device, kAudioObjectPropertyScopeInput); input_channels > 0) {
            enumeration.inputs.append({
                .dom_device_id = ByteString::formatted("coreaudio:input:{}", uid),
                .label = label,
                .group_id = {},
                .sample_rate_hz = sample_rate,
                .channel_count = input_channels,
                .is_default = device == default_input,
            });
        }
        if (auto output_channels = device_channel_count(device, kAudioObjectPropertyScopeOutput); output_channels > 0) {
            enumeration.outputs.append({
                .dom_device_id = ByteString::formatted("coreaudio:output:{}", uid),
                .label = label,
                .group_id = {},
                .sample_rate_hz = sample_rate,
                .channel_count = output_channels,
                .is_default = device == default_output,
            });
        }
    }
    return enumeration;
}

NonnullRefPtr<AudioDeviceEnumerationPromise> enumerate_platform_audio_devices()
{
    auto enumeration_or_error = enumerate_core_audio_devices();
    if (enumeration_or_error.is_error())
        return AudioDeviceEnumerationPromise::rejected(enumeration_or_error.release_error());
    return AudioDeviceEnumerationPromise::resolved(enumeration_or_error.release_value());
}

}
