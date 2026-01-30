/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/AudioOutputDeviceManager.h>

#include <AK/ByteBuffer.h>

#if defined(AK_OS_MACOS)
#    include <CoreAudio/CoreAudio.h>
#    include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(HAVE_PULSEAUDIO)
#    include <pulse/pulseaudio.h>
#endif

namespace AudioServer {

#if defined(AK_OS_MACOS)
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

static Optional<AudioObjectID> default_output_device_id()
{
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDefaultOutputDevice,
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

static u32 output_channel_count(AudioObjectID device_id)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput,
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
#endif

Vector<AudioOutputDeviceInfo> AudioOutputDeviceManager::enumerate_devices()
{
    Vector<AudioOutputDeviceInfo> devices;

#if defined(AK_OS_MACOS)
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

    auto default_device = default_output_device_id();
    size_t count = size / sizeof(AudioObjectID);
    devices.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto device_id = device_ids[i];
        u32 channel_count = output_channel_count(device_id);
        if (channel_count == 0)
            continue;

        auto label = device_name(device_id).value_or(ByteString());
        auto persistent_id = device_uid(device_id).value_or(ByteString());
        u32 sample_rate_hz = device_sample_rate(device_id);
        bool is_default = default_device.has_value() && default_device.value() == device_id;

        devices.append(AudioOutputDeviceInfo {
            .device_id = static_cast<AudioOutputDeviceID>(device_id),
            .label = move(label),
            .persistent_id = move(persistent_id),
            .sample_rate_hz = sample_rate_hz,
            .channel_count = channel_count,
            .is_default = is_default,
        });
    }
#endif

#if defined(HAVE_PULSEAUDIO)
    pa_mainloop* mainloop = pa_mainloop_new();
    if (!mainloop)
        return devices;

    auto* api = pa_mainloop_get_api(mainloop);
    pa_context* context = pa_context_new(api, "Ladybird AudioServer");
    if (!context) {
        pa_mainloop_free(mainloop);
        return devices;
    }

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(context);
        pa_mainloop_free(mainloop);
        return devices;
    }

    while (true) {
        pa_mainloop_iterate(mainloop, 1, nullptr);
        auto state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            pa_mainloop_free(mainloop);
            return devices;
        }
    }

    ByteString default_sink;
    auto server_info_callback = [](pa_context*, pa_server_info const* info, void* userdata) {
        auto* data = static_cast<ByteString*>(userdata);
        if (info && info->default_sink_name)
            *data = info->default_sink_name;
    };

    auto* server_info_op = pa_context_get_server_info(context, server_info_callback, &default_sink);
    while (pa_operation_get_state(server_info_op) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(mainloop, 1, nullptr);
    pa_operation_unref(server_info_op);

    struct SinkListData {
        Vector<AudioOutputDeviceInfo>* devices;
        ByteString* default_sink;
    };

    auto sink_info_callback = [](pa_context*, pa_sink_info const* info, int eol, void* userdata) {
        if (eol != 0)
            return;
        if (!info || !info->name)
            return;

        auto* data = static_cast<SinkListData*>(userdata);
        auto& devices = *data->devices;

        bool is_default = data->default_sink && !data->default_sink->is_empty() && ByteString(info->name) == *data->default_sink;

        devices.append(AudioOutputDeviceInfo {
            .device_id = static_cast<AudioOutputDeviceID>(info->index),
            .label = info->description ? ByteString(info->description) : ByteString(info->name),
            .persistent_id = ByteString(info->name),
            .sample_rate_hz = static_cast<u32>(info->sample_spec.rate),
            .channel_count = static_cast<u32>(info->sample_spec.channels),
            .is_default = is_default,
        });
    };

    SinkListData data {
        .devices = &devices,
        .default_sink = &default_sink,
    };

    auto* sink_op = pa_context_get_sink_info_list(context, sink_info_callback, &data);
    while (pa_operation_get_state(sink_op) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(mainloop, 1, nullptr);
    pa_operation_unref(sink_op);

    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
#endif

    return devices;
}

}
