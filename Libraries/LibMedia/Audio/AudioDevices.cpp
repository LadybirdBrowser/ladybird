/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>

#if defined(LADYBIRD_AUDIO_BACKEND_PULSE)
#    include <LibMedia/Audio/PulseAudioWrappers.h>
#    include <pulse/pulseaudio.h>
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
#else
    // FIXME: Implement device enumeration for the AudioUnit (macOS) and WASAPI (Windows)
    //        backends.
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
