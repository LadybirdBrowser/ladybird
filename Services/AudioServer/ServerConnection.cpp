/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AudioServer/ServerConnection.h>
#include <math.h>

namespace Audio {

static MixerClientId s_next_mixer_client_id { 1 };

ServerConnection::ServerConnection(NonnullOwnPtr<IPC::Transport> transport, int client_id, NonnullRefPtr<PlaybackStreamMixer> mixer, DeviceEnumeration device_enumeration, NonnullRefPtr<CaptureDevices> capture_devices, CapturePermissionRequest request_capture_permission)
    : IPC::ConnectionFromClient<AudioClientEndpoint, AudioServerEndpoint>(*this, move(transport), client_id)
    , m_mixer(move(mixer))
    , m_capture_devices(move(capture_devices))
    , m_request_capture_permission(move(request_capture_permission))
    , m_device_enumeration(device_enumeration)
{
}

ServerConnection::~ServerConnection()
{
    stop_listening_for_device_changes();
    // Removing the streams settles their drains, which must not find a connection that is going away.
    revoke_weak_ptrs();
    remove_streams_from_mixer();
    remove_record_streams();
}

void ServerConnection::die()
{
    stop_listening_for_device_changes();
    remove_streams_from_mixer();
    remove_record_streams();
    if (on_death)
        on_death();
}

void ServerConnection::remove_record_streams()
{
    for (auto& [stream_id, stream] : m_record_streams) {
        if (stream.subscriber_id.has_value())
            m_capture_devices->unsubscribe(stream.subscriber_id.value());
    }
    m_record_streams.clear();
}

void ServerConnection::set_capture_permitted(bool permitted)
{
    if (m_capture_permitted == permitted)
        return;
    m_capture_permitted = permitted;
    if (!permitted) {
        for (auto& [stream_id, stream] : m_record_streams)
            async_record_stream_closed(stream_id);
        remove_record_streams();
    }
    if (m_client_watches_devices)
        send_device_list();
}

void ServerConnection::stop_listening_for_device_changes()
{
    if (m_devices_changed_listener_id.has_value())
        AudioDevices::the().remove_devices_changed_listener(m_devices_changed_listener_id.release_value());
}

void ServerConnection::remove_streams_from_mixer()
{
    for (auto& [stream_id, stream] : m_streams) {
        if (stream.has_ring)
            m_mixer->remove_client(stream.mixer_client_id);
    }
    m_streams.clear();
}

ServerConnection::StreamState* ServerConnection::find_stream(u64 stream_id, StringView operation)
{
    auto stream = m_streams.get(stream_id);
    if (!stream.has_value()) {
        did_misbehave(ByteString::formatted("{}: unknown stream", operation).characters());
        return nullptr;
    }
    return &stream.value();
}

void ServerConnection::declare_session(u64 session_id, PlaybackGroupId group)
{
    if (m_session_groups.contains(session_id)) {
        did_misbehave("declare_session: session ID already exists");
        return;
    }
    m_session_groups.set(session_id, group);
}

void ServerConnection::rebind_session(u64 session_id, PlaybackGroupId group)
{
    auto session_group = m_session_groups.get(session_id);
    if (!session_group.has_value()) {
        did_misbehave("rebind_session: unknown session");
        return;
    }
    session_group.value() = group;
    move_streams_of_session_to(session_id, group);
}

void ServerConnection::destroy_session(u64 session_id)
{
    if (!m_session_groups.remove(session_id)) {
        did_misbehave("destroy_session: unknown session");
        return;
    }
    move_streams_of_session_to(session_id, {});
}

void ServerConnection::move_streams_of_session_to(u64 session_id, Optional<PlaybackGroupId> group)
{
    for (auto& [stream_id, stream] : m_streams) {
        if (stream.session_id == session_id && stream.has_ring)
            m_mixer->set_client_group(stream.mixer_client_id, group);
    }
}

void ServerConnection::create_stream(u64 stream_id, u64 session_id)
{
    if (m_streams.contains(stream_id)) {
        did_misbehave("create_stream: stream ID already exists");
        return;
    }
    if (!m_session_groups.contains(session_id)) {
        did_misbehave("create_stream: unknown session");
        return;
    }
    m_streams.set(stream_id, StreamState { .mixer_client_id = s_next_mixer_client_id++, .session_id = session_id });

    m_mixer->when_device_ready([weak_self = make_weak_ptr<ServerConnection>(), stream_id](ErrorOr<SampleSpecification> const& specification) {
        // The client may have given up on the stream while the device was opening.
        auto self = weak_self.strong_ref();
        if (!self || !self->m_streams.contains(stream_id))
            return;
        if (specification.is_error()) {
            self->m_streams.remove(stream_id);
            self->async_stream_creation_failed(stream_id);
            return;
        }
        self->async_stream_created(stream_id, specification.value(), self->m_mixer->device_target_latency_ms());
    });
}

void ServerConnection::destroy_stream(u64 stream_id)
{
    auto stream = m_streams.take(stream_id);
    if (!stream.has_value())
        return;
    if (stream->has_ring)
        m_mixer->remove_client(stream->mixer_client_id);
}

void ServerConnection::attach_stream_ring(u64 stream_id, SharedAudioFrameRing ring)
{
    auto* stream = find_stream(stream_id, "attach_stream_ring"sv);
    if (!stream)
        return;
    if (stream->has_ring) {
        did_misbehave("attach_stream_ring: stream already has a ring");
        return;
    }
    auto const& device_specification = m_mixer->device_sample_specification();
    if (!device_specification.has_value() || ring.sample_rate() != device_specification->sample_rate() || ring.channel_count() != device_specification->channel_count()) {
        did_misbehave("attach_stream_ring: ring does not match the device format");
        return;
    }
    stream->has_ring = true;
    m_mixer->add_client(stream->mixer_client_id, move(ring), stream->volume, m_session_groups.get(stream->session_id));
}

void ServerConnection::resume_stream(u64 stream_id, u64 request_id)
{
    auto* stream = find_stream(stream_id, "resume_stream"sv);
    if (!stream)
        return;
    if (!stream->has_ring) {
        did_misbehave("resume_stream: stream has no ring");
        return;
    }
    m_mixer->resume_client(stream->mixer_client_id);
    async_stream_request_completed(stream_id, request_id);
}

void ServerConnection::drain_stream(u64 stream_id, u64 request_id)
{
    auto* stream = find_stream(stream_id, "drain_stream"sv);
    if (!stream)
        return;
    if (!stream->has_ring) {
        async_stream_request_completed(stream_id, request_id);
        return;
    }
    m_mixer->drain_client(stream->mixer_client_id, [weak_self = make_weak_ptr<ServerConnection>(), stream_id, request_id] {
        auto self = weak_self.strong_ref();
        if (self && self->m_streams.contains(stream_id))
            self->async_stream_request_completed(stream_id, request_id);
    });
}

void ServerConnection::discard_stream(u64 stream_id, u64 request_id)
{
    auto* stream = find_stream(stream_id, "discard_stream"sv);
    if (!stream)
        return;
    if (stream->has_ring)
        m_mixer->discard_client(stream->mixer_client_id);
    async_stream_request_completed(stream_id, request_id);
}

void ServerConnection::set_stream_volume(u64 stream_id, float volume)
{
    auto* stream = find_stream(stream_id, "set_stream_volume"sv);
    if (!stream)
        return;
    if (isnan(volume) || volume < 0.0f || volume > 1.0f) {
        did_misbehave("set_stream_volume: volume is outside [0, 1]");
        return;
    }
    stream->volume = volume;
    if (stream->has_ring)
        m_mixer->set_client_gain(stream->mixer_client_id, volume);
}

void ServerConnection::watch_devices()
{
    m_client_watches_devices = true;
    if (m_device_enumeration == DeviceEnumeration::Platform && !m_devices_changed_listener_id.has_value())
        m_devices_changed_listener_id = AudioDevices::the().add_devices_changed_listener([this] { devices_changed(); });
    // A list not yet available follows as soon as the platform reports one.
    if (m_device_enumeration == DeviceEnumeration::None || AudioDevices::the().has_device_list())
        send_device_list();
}

static Vector<AudioDeviceInfo> reduced_to_their_existence(Vector<AudioDeviceInfo> const& devices)
{
    if (devices.is_empty())
        return {};
    return { AudioDeviceInfo { .dom_device_id = {}, .label = {}, .group_id = {}, .sample_rate_hz = 0, .channel_count = 0, .is_default = true } };
}

void ServerConnection::send_device_list()
{
    if (m_device_enumeration == DeviceEnumeration::None) {
        async_devices_changed({}, {});
        return;
    }
    auto& devices = AudioDevices::the();
    // A page without the permission may learn only that devices exist, so that is all its process learns.
    if (!m_capture_permitted) {
        async_devices_changed(reduced_to_their_existence(devices.input_devices()), reduced_to_their_existence(devices.output_devices()));
        return;
    }
    async_devices_changed(devices.input_devices(), devices.output_devices());
}

void ServerConnection::devices_changed()
{
    if (m_client_watches_devices)
        send_device_list();
}

void ServerConnection::create_record_stream(u64 stream_id, ByteString device_id)
{
    if (m_record_streams.contains(stream_id)) {
        did_misbehave("create_record_stream: stream ID already exists");
        return;
    }

    // The Browser rules on every request, so a client once allowed cannot simply start another.
    m_record_streams.set(stream_id, RecordStreamState {});
    m_request_capture_permission(device_id, [weak_self = make_weak_ptr<ServerConnection>(), stream_id, device_id](bool allowed) {
        auto self = weak_self.strong_ref();
        if (self)
            self->capture_permission_decided(stream_id, device_id, allowed);
    });
}

void ServerConnection::capture_permission_decided(u64 stream_id, ByteString const& device_id, bool allowed)
{
    // The client may have given up on the stream, or the tab lost the permission, while the Browser was asked.
    if (!m_record_streams.contains(stream_id))
        return;
    if (!allowed) {
        m_record_streams.remove(stream_id);
        async_record_stream_creation_failed(stream_id);
        return;
    }

    // The state exists before subscribing, since a device that is already open reports at once.
    auto subscriber_id = m_capture_devices->subscribe(device_id, [weak_self = make_weak_ptr<ServerConnection>(), stream_id](CaptureSubscriberId subscriber_id, ErrorOr<SampleSpecification> const& result) {
        auto self = weak_self.strong_ref();
        if (self)
            self->capture_device_ready(stream_id, subscriber_id, result);
    });
    // The callback may have failed and removed the stream already.
    if (auto stream = m_record_streams.get(stream_id); stream.has_value())
        stream->subscriber_id = subscriber_id;
}

void ServerConnection::capture_device_ready(u64 stream_id, CaptureSubscriberId subscriber_id, ErrorOr<SampleSpecification> const& result)
{
    // The client may have given up on the stream while the device was opening.
    if (!m_record_streams.contains(stream_id))
        return;

    auto fail = [&] {
        m_capture_devices->unsubscribe(subscriber_id);
        m_record_streams.remove(stream_id);
        async_record_stream_creation_failed(stream_id);
    };
    if (result.is_error()) {
        fail();
        return;
    }

    // A quarter of a second of room: the client pops every fragment, so this only ever absorbs scheduling hiccups.
    auto const& specification = result.value();
    auto ring = SharedAudioFrameRing::create(specification.sample_rate(), specification.channel_count(), specification.sample_rate() / 4);
    if (ring.is_error()) {
        fail();
        return;
    }
    m_capture_devices->attach_ring(subscriber_id, ring.value());
    async_record_stream_created(stream_id, ring.release_value());
}

void ServerConnection::destroy_record_stream(u64 stream_id)
{
    auto stream = m_record_streams.take(stream_id);
    if (!stream.has_value() || !stream->subscriber_id.has_value())
        return;
    m_capture_devices->unsubscribe(stream->subscriber_id.value());
}

}
