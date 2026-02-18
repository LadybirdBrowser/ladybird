/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <AudioServer/Debug.h>
#include <AudioServer/Server.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibThreading/Thread.h>
#include <pulse/pulseaudio.h>

namespace AudioServer {

static Audio::ChannelMap create_unknown_channel_layout(u32 channel_count)
{
    if (channel_count > Audio::ChannelMap::capacity())
        return Audio::ChannelMap::invalid();

    Vector<Audio::Channel, Audio::ChannelMap::capacity()> channel_layout;
    channel_layout.resize(channel_count);

    for (auto& channel : channel_layout)
        channel = Audio::Channel::Unknown;

    return Audio::ChannelMap(channel_layout);
}

static Audio::ChannelMap pulse_output_channel_layout(pa_channel_map const& channel_map)
{
    if (channel_map.channels <= 0)
        return Audio::ChannelMap::invalid();

    if (static_cast<size_t>(channel_map.channels) > Audio::ChannelMap::capacity())
        return Audio::ChannelMap::invalid();

    Vector<Audio::Channel, Audio::ChannelMap::capacity()> channel_layout;
    channel_layout.resize(static_cast<size_t>(channel_map.channels));

    auto pulse_position_to_channel = [](pa_channel_position_t position) -> Audio::Channel {
        switch (position) {
        case PA_CHANNEL_POSITION_FRONT_LEFT:
            return Audio::Channel::FrontLeft;
        case PA_CHANNEL_POSITION_FRONT_RIGHT:
            return Audio::Channel::FrontRight;
        case PA_CHANNEL_POSITION_FRONT_CENTER:
            return Audio::Channel::FrontCenter;
        case PA_CHANNEL_POSITION_LFE:
            return Audio::Channel::LowFrequency;
        case PA_CHANNEL_POSITION_REAR_LEFT:
            return Audio::Channel::BackLeft;
        case PA_CHANNEL_POSITION_REAR_RIGHT:
            return Audio::Channel::BackRight;
        case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
            return Audio::Channel::FrontLeftOfCenter;
        case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
            return Audio::Channel::FrontRightOfCenter;
        case PA_CHANNEL_POSITION_REAR_CENTER:
            return Audio::Channel::BackCenter;
        case PA_CHANNEL_POSITION_SIDE_LEFT:
            return Audio::Channel::SideLeft;
        case PA_CHANNEL_POSITION_SIDE_RIGHT:
            return Audio::Channel::SideRight;
        case PA_CHANNEL_POSITION_TOP_CENTER:
            return Audio::Channel::TopCenter;
        case PA_CHANNEL_POSITION_TOP_FRONT_LEFT:
            return Audio::Channel::TopFrontLeft;
        case PA_CHANNEL_POSITION_TOP_FRONT_CENTER:
            return Audio::Channel::TopFrontCenter;
        case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT:
            return Audio::Channel::TopFrontRight;
        case PA_CHANNEL_POSITION_TOP_REAR_LEFT:
            return Audio::Channel::TopBackLeft;
        case PA_CHANNEL_POSITION_TOP_REAR_CENTER:
            return Audio::Channel::TopBackCenter;
        case PA_CHANNEL_POSITION_TOP_REAR_RIGHT:
            return Audio::Channel::TopBackRight;
        default:
            return Audio::Channel::Unknown;
        }
    };

    for (int index = 0; index < channel_map.channels; ++index)
        channel_layout[index] = pulse_position_to_channel(channel_map.map[index]);

    return Audio::ChannelMap(channel_layout);
}

static bool wait_until_context_ready(pa_mainloop* mainloop, pa_context* context)
{
    while (true) {
        pa_mainloop_iterate(mainloop, 1, nullptr);
        pa_context_state_t state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY)
            return true;
        if (!PA_CONTEXT_IS_GOOD(state))
            return false;
    }
}

static bool wait_for_operation(pa_mainloop* mainloop, pa_operation* operation)
{
    if (!operation)
        return false;
    ScopeGuard unref_operation = [&] { pa_operation_unref(operation); };

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        pa_mainloop_iterate(mainloop, 1, nullptr);

    return true;
}

static void ensure_pulse_device_change_notifications_registered()
{
    static RefPtr<Threading::Thread> monitor_thread;
    if (monitor_thread)
        return;

    monitor_thread = Threading::Thread::construct("AudioDeviceMon"sv, [] -> intptr_t {
        pa_mainloop* mainloop = pa_mainloop_new();
        if (!mainloop) {
            warnln("Can't create PulseAudio mainloop for device change notifications");
            return 0;
        }
        ScopeGuard cleanup_mainloop = [&] { pa_mainloop_free(mainloop); };

        pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
        if (!api) {
            warnln("Failed to get PulseAudio mainloop API for device change notifications");
            return 0;
        }
        pa_context* context = pa_context_new(api, "Ladybird AudioServer Device Monitor");
        if (!context) {
            warnln("Couldn't make PulseAudio context for device change notifications");
            return 0;
        }
        ScopeGuard cleanup_context = [&] {
            pa_context_disconnect(context);
            pa_context_unref(context);
        };

        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            warnln("Can't connect PulseAudio context for device change notifications");
            return 0;
        }

        if (!wait_until_context_ready(mainloop, context)) {
            warnln("Cannot wait for PulseAudio context to be ready (and not in a good way)");
            return 0;
        }
        pa_context_set_subscribe_callback(
            context,
            [](pa_context*, pa_subscription_event_type_t event_type, [[maybe_unused]] u32 index, [[maybe_unused]] void*) {
                u32 operation = static_cast<u32>(event_type) & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

                if (operation == PA_SUBSCRIPTION_EVENT_NEW || operation == PA_SUBSCRIPTION_EVENT_REMOVE)
                    Server::the().update_devices();
            },
            nullptr);

        constexpr pa_subscription_mask_t notification_mask = static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE);
        pa_operation* subscribe_op = pa_context_subscribe(
            context,
            notification_mask,
            nullptr,
            nullptr);
        if (!wait_for_operation(mainloop, subscribe_op)) {
            warnln("Failed to wait for PulseAudio subscribe");
            return 0;
        }

        while (PA_CONTEXT_IS_GOOD(pa_context_get_state(context))) {
            pa_mainloop_iterate(mainloop, 1, nullptr);
            // this thread is eternal until a reason to join shows up.
        }
        warnln("Exiting PulseAudio device change monitor thread");

        return 0;
    });
    monitor_thread->start();
}

Vector<DeviceInfo> Server::enumerate_platform_devices()
{
    ensure_pulse_device_change_notifications_registered();

    Vector<DeviceInfo> devices;

    pa_mainloop* mainloop = pa_mainloop_new();
    if (!mainloop)
        return devices;

    auto* api = pa_mainloop_get_api(mainloop);
    pa_context* context = pa_context_new(api, "Ladybird AudioServer");
    if (!context) {
        pa_mainloop_free(mainloop);
        return devices;
    }
    ScopeGuard cleanup_mainloop = [&] { pa_mainloop_free(mainloop); };
    ScopeGuard cleanup_context = [&] {
        pa_context_disconnect(context);
        pa_context_unref(context);
    };

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        return devices;

    if (!wait_until_context_ready(mainloop, context))
        return devices;

    ByteString default_sink;
    auto server_info_callback = [](pa_context*, pa_server_info const* info, void* userdata) {
        auto* data = static_cast<ByteString*>(userdata);
        if (info && info->default_sink_name)
            *data = info->default_sink_name;
    };

    auto* server_info_op = pa_context_get_server_info(context, server_info_callback, &default_sink);
    if (!wait_for_operation(mainloop, server_info_op))
        return devices;

    struct SinkListData {
        Vector<DeviceInfo>* devices;
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
        ByteString backend_id = ByteString(info->name);
        auto channel_layout = pulse_output_channel_layout(info->channel_map);

        devices.append(DeviceInfo {
            .type = DeviceInfo::Type::Output,
            .device_handle = Server::make_device_handle(static_cast<u64>(info->index), DeviceInfo::Type::Output),
            .label = info->description ? ByteString(info->description) : backend_id,
            .dom_device_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("audiooutput"sv, backend_id, static_cast<u64>(info->index)),
            .group_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("group"sv, backend_id, static_cast<u64>(info->index)),
            .sample_rate_hz = static_cast<u32>(info->sample_spec.rate),
            .channel_count = static_cast<u32>(info->sample_spec.channels),
            .channel_layout = channel_layout,
            .is_default = is_default,
        });
    };

    SinkListData data {
        .devices = &devices,
        .default_sink = &default_sink,
    };

    auto* sink_op = pa_context_get_sink_info_list(context, sink_info_callback, &data);
    if (!wait_for_operation(mainloop, sink_op))
        return devices;

    ByteString default_source;
    auto source_default_callback = [](pa_context*, pa_server_info const* info, void* userdata) {
        auto* data = static_cast<ByteString*>(userdata);
        if (info && info->default_source_name)
            *data = info->default_source_name;
    };

    auto* source_default_op = pa_context_get_server_info(context, source_default_callback, &default_source);
    if (!wait_for_operation(mainloop, source_default_op))
        return devices;

    struct SourceListData {
        Vector<DeviceInfo>* devices;
        ByteString* default_source;
    };

    auto source_info_callback = [](pa_context*, pa_source_info const* info, int eol, void* userdata) {
        if (eol != 0)
            return;
        if (!info || !info->name)
            return;
        if (info->monitor_of_sink != PA_INVALID_INDEX)
            return;

        auto* data = static_cast<SourceListData*>(userdata);
        auto& devices = *data->devices;

        bool is_default = data->default_source && !data->default_source->is_empty() && ByteString(info->name) == *data->default_source;
        ByteString backend_id = ByteString(info->name);
        u32 channel_count = static_cast<u32>(info->sample_spec.channels);

        devices.append(DeviceInfo {
            .type = DeviceInfo::Type::Input,
            .device_handle = Server::make_device_handle(static_cast<u64>(info->index), DeviceInfo::Type::Input),
            .label = info->description ? ByteString(info->description) : backend_id,
            .dom_device_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("audioinput"sv, backend_id, static_cast<u64>(info->index)),
            .group_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("group"sv, backend_id, static_cast<u64>(info->index)),
            .sample_rate_hz = static_cast<u32>(info->sample_spec.rate),
            .channel_count = channel_count,
            .channel_layout = create_unknown_channel_layout(channel_count),
            .is_default = is_default,
        });
    };

    SourceListData source_data {
        .devices = &devices,
        .default_source = &default_source,
    };

    auto* source_op = pa_context_get_source_info_list(context, source_info_callback, &source_data);
    if (!wait_for_operation(mainloop, source_op))
        return devices;

    return devices;
}

}
