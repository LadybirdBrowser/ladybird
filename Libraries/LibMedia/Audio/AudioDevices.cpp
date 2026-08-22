/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>

#if defined(LADYBIRD_AUDIO_BACKEND_PULSE)
#    include <LibMedia/Audio/PulseAudioWrappers.h>
#    include <pulse/pulseaudio.h>
#elif defined(LADYBIRD_AUDIO_BACKEND_AUDIO_UNIT)
#    include <AK/ScopeGuard.h>
#    include <AK/kmalloc.h>
#    include <CoreAudio/CoreAudio.h>
#    include <CoreFoundation/CoreFoundation.h>
#endif

namespace Media {

#if defined(LADYBIRD_AUDIO_BACKEND_PULSE)

static void enumerate_devices_via_pulseaudio(Vector<AudioDeviceInfo>& inputs, Vector<AudioDeviceInfo>& outputs)
{
    struct EnumerationState {
        Audio::PulseAudioContext& context;
        Vector<AudioDeviceInfo> inputs;
        Vector<AudioDeviceInfo> outputs;
        ByteString default_source_name;
        ByteString default_sink_name;
    };

    auto context_or_error = Audio::PulseAudioContext::the();
    if (context_or_error.is_error())
        return;
    auto context = context_or_error.release_value();

    EnumerationState state { .context = *context, .inputs = {}, .outputs = {}, .default_source_name = {}, .default_sink_name = {} };

    auto locker = context->main_loop_locker();

    auto wait_for_operation = [&](pa_operation* operation) {
        if (operation == nullptr)
            return;
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
            context->wait_for_signal();
        pa_operation_unref(operation);
    };

    // Fetch the defaults first so the enumeration callbacks can mark them.
    wait_for_operation(pa_context_get_server_info(
        context->native_handle(), [](pa_context*, pa_server_info const* info, void* user_data) {
            auto& state = *static_cast<EnumerationState*>(user_data);
            if (info != nullptr) {
                if (info->default_source_name != nullptr)
                    state.default_source_name = info->default_source_name;
                if (info->default_sink_name != nullptr)
                    state.default_sink_name = info->default_sink_name;
            }
            state.context.signal_to_wake();
        },
        &state));

    wait_for_operation(pa_context_get_source_info_list(
        context->native_handle(), [](pa_context*, pa_source_info const* info, int is_end_of_list, void* user_data) {
            auto& state = *static_cast<EnumerationState*>(user_data);
            if (is_end_of_list != 0) {
                state.context.signal_to_wake();
                return;
            }
            if (info == nullptr || info->name == nullptr)
                return;
            AudioDeviceInfo entry {
                .dom_device_id = ByteString::formatted("pulse:source:{}", info->name),
                .label = info->description != nullptr ? ByteString { info->description } : ByteString { info->name },
                .group_id = {},
                .sample_rate_hz = info->sample_spec.rate,
                .channel_count = info->sample_spec.channels,
                .is_default = state.default_source_name == info->name,
            };
            state.inputs.append(move(entry));
        },
        &state));

    wait_for_operation(pa_context_get_sink_info_list(
        context->native_handle(), [](pa_context*, pa_sink_info const* info, int is_end_of_list, void* user_data) {
            auto& state = *static_cast<EnumerationState*>(user_data);
            if (is_end_of_list != 0) {
                state.context.signal_to_wake();
                return;
            }
            if (info == nullptr || info->name == nullptr)
                return;
            AudioDeviceInfo entry {
                .dom_device_id = ByteString::formatted("pulse:sink:{}", info->name),
                .label = info->description != nullptr ? ByteString { info->description } : ByteString { info->name },
                .group_id = {},
                .sample_rate_hz = info->sample_spec.rate,
                .channel_count = info->sample_spec.channels,
                .is_default = state.default_sink_name == info->name,
            };
            state.outputs.append(move(entry));
        },
        &state));

    inputs = move(state.inputs);
    outputs = move(state.outputs);
}

#elif defined(LADYBIRD_AUDIO_BACKEND_AUDIO_UNIT)

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

static void enumerate_devices_via_core_audio(Vector<AudioDeviceInfo>& inputs, Vector<AudioDeviceInfo>& outputs)
{
    AudioObjectPropertyAddress devices_address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices_address, 0, nullptr, &size) != noErr || size == 0)
        return;

    Vector<AudioDeviceID> devices;
    devices.resize(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_address, 0, nullptr, &size, devices.data()) != noErr)
        return;
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
            inputs.append({
                .dom_device_id = ByteString::formatted("coreaudio:input:{}", uid),
                .label = label,
                .group_id = {},
                .sample_rate_hz = sample_rate,
                .channel_count = input_channels,
                .is_default = device == default_input,
            });
        }
        if (auto output_channels = device_channel_count(device, kAudioObjectPropertyScopeOutput); output_channels > 0) {
            outputs.append({
                .dom_device_id = ByteString::formatted("coreaudio:output:{}", uid),
                .label = label,
                .group_id = {},
                .sample_rate_hz = sample_rate,
                .channel_count = output_channels,
                .is_default = device == default_output,
            });
        }
    }
}

#endif

AudioDevices& AudioDevices::the()
{
    static AudioDevices& devices = *new AudioDevices;

    static bool did_initial_refresh = false;
    if (!did_initial_refresh) {
        did_initial_refresh = true;
        devices.refresh();
    }

    return devices;
}

void AudioDevices::refresh()
{
    Vector<AudioDeviceInfo> inputs;
    Vector<AudioDeviceInfo> outputs;

#if defined(LADYBIRD_AUDIO_BACKEND_PULSE)
    enumerate_devices_via_pulseaudio(inputs, outputs);
#elif defined(LADYBIRD_AUDIO_BACKEND_AUDIO_UNIT)
    enumerate_devices_via_core_audio(inputs, outputs);
#else
    // FIXME: Implement device enumeration for the WASAPI (Windows) backend.
#endif

    m_cached_input_devices = move(inputs);
    m_cached_output_devices = move(outputs);
    notify_listeners();
}

Vector<AudioDeviceInfo> AudioDevices::input_devices() const
{
    return m_cached_input_devices;
}

Vector<AudioDeviceInfo> AudioDevices::output_devices() const
{
    return m_cached_output_devices;
}

AudioDevices::ListenerId AudioDevices::add_devices_changed_listener(Function<void()> listener)
{
    ListenerId listener_id = m_next_listener_id++;
    m_listeners.set(listener_id, move(listener));
    return listener_id;
}

void AudioDevices::remove_devices_changed_listener(ListenerId listener_id)
{
    m_listeners.remove(listener_id);
}

void AudioDevices::notify_listeners()
{
    Vector<ListenerId> listener_ids;
    listener_ids.ensure_capacity(m_listeners.size());
    for (auto const& listener : m_listeners)
        listener_ids.append(listener.key);

    for (auto listener_id : listener_ids) {
        auto callback = m_listeners.get(listener_id);
        if (!callback.has_value())
            continue;
        callback.value()();
    }
}

}
