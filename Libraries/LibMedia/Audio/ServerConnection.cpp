/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ServerConnection.h>

namespace Audio {

static MixerClientId s_next_mixer_client_id { 1 };

ServerConnection::ServerConnection(NonnullOwnPtr<IPC::Transport> transport, int client_id, NonnullRefPtr<PlaybackStreamMixer> mixer)
    : IPC::ConnectionFromClient<AudioClientEndpoint, AudioServerEndpoint>(*this, move(transport), client_id)
    , m_mixer(move(mixer))
{
}

ServerConnection::~ServerConnection()
{
    // Removing the streams settles their drains, which must not find a connection that is going away.
    revoke_weak_ptrs();
    remove_streams_from_mixer();
}

void ServerConnection::die()
{
    remove_streams_from_mixer();
    if (on_death)
        on_death();
}

void ServerConnection::remove_streams_from_mixer()
{
    for (auto& [stream_id, stream] : m_streams) {
        if (stream.has_ring)
            m_mixer->remove_client(stream.mixer_client_id);
    }
    m_streams.clear();
}

Messages::AudioServer::InitTransportResponse ServerConnection::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected audio server transport initialization");
    return 0;
#endif
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

void ServerConnection::create_stream(u64 stream_id)
{
    if (m_streams.contains(stream_id)) {
        did_misbehave("create_stream: stream ID already exists");
        return;
    }
    m_streams.set(stream_id, StreamState { .mixer_client_id = s_next_mixer_client_id++ });

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
    m_mixer->add_client(stream->mixer_client_id, move(ring), stream->volume);
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
    stream->volume = volume;
    if (stream->has_ring)
        m_mixer->set_client_gain(stream->mixer_client_id, volume);
}

}
