/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudioWorkerClient/WebAudioClient.h>

#include <LibCore/Socket.h>
#include <LibIPC/Transport.h>

namespace WebAudioWorkerClient {

NonnullRefPtr<WebAudioClient> WebAudioClient::create()
{
    return adopt_ref(*new WebAudioClient);
}

WebAudioClient::WebAudioConnection::WebAudioConnection(WebAudioClient& client, NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebAudioClientEndpoint, WebAudioServerEndpoint>(*this, move(transport))
    , m_client(client)
{
}

void WebAudioClient::WebAudioConnection::die()
{
    auto self = NonnullRefPtr(*this);
    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

void WebAudioClient::WebAudioConnection::webaudio_session_worklet_processor_error(u64 session_id, u64 node_id)
{
    if (m_client.on_worklet_processor_error)
        m_client.on_worklet_processor_error(session_id, node_id);
}

void WebAudioClient::WebAudioConnection::webaudio_session_worklet_processor_registered(u64 session_id, String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation)
{
    if (m_client.on_worklet_processor_registered)
        m_client.on_worklet_processor_registered(session_id, name, descriptors, generation);
}

void WebAudioClient::WebAudioConnection::webaudio_session_worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String error_name, String error_message, Vector<String> failed_processor_registrations)
{
    if (m_client.on_worklet_module_evaluated)
        m_client.on_worklet_module_evaluated(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
}

void WebAudioClient::set_socket_provider(Function<ErrorOr<IPC::File>(u64 page_id)> provider)
{
    m_socket_provider = move(provider);
}

ErrorOr<void> WebAudioClient::ensure_connection(u64 page_id)
{
    if (m_connection && m_connection->is_open())
        return {};

    if (!m_socket_provider) {
        return Error::from_string_literal("WebAudioClient: no socket provider installed");
    }

    IPC::File socket = TRY(m_socket_provider(page_id));
    if (socket.fd() < 0) {
        return Error::from_string_literal("WebAudioClient: socket provider returned invalid socket");
    }

    TRY(socket.clear_close_on_exec());

    auto local_socket = TRY(Core::LocalSocket::adopt_fd(socket.take_fd()));
    auto transport = make<IPC::Transport>(move(local_socket));
    m_connection = adopt_ref(*new WebAudioConnection(*this, move(transport)));

    m_connection->on_death = [weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref()) {
            self->m_connection = nullptr;
            auto death_callback = move(self->on_death);
            self->on_death = {};
            if (death_callback)
                death_callback();
        }
    };

    return {};
}

ErrorOr<WebAudioClient::WebAudioSession> WebAudioClient::create_webaudio_session(u32 target_latency_ms, u64 page_id)
{
    TRY(ensure_connection(page_id));

    auto response = m_connection->send_sync_but_allow_failure<Messages::WebAudioServer::CreateWebaudioSession>(target_latency_ms);
    if (!response)
        return Error::from_string_literal("WebAudioClient: create webaudio session IPC failed");

    u64 session_id = response->session_id();
    u32 sample_rate = response->sample_rate();
    u32 channel_count = response->channel_count();
    Core::AnonymousBuffer timing_buffer = response->timing_buffer();
    int timing_notify_fd = response->timing_notify_fd().take_fd();

    if (session_id == 0)
        return Error::from_string_literal("WebAudioClient: server returned invalid webaudio session");
    if (sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("WebAudioClient: server returned invalid webaudio device format");
    if (!timing_buffer.is_valid())
        return Error::from_string_literal("WebAudioClient: server returned invalid webaudio timing buffer");
    if (timing_notify_fd < 0)
        return Error::from_string_literal("WebAudioClient: server returned invalid webaudio timing notifier fd");

    return WebAudioSession {
        .session_id = session_id,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .timing_buffer = move(timing_buffer),
        .timing_notify_fd = IPC::File::adopt_fd(timing_notify_fd),
    };
}

ErrorOr<void> WebAudioClient::destroy_webaudio_session(u64 session_id)
{
    if (!m_connection || !m_connection->is_open()) {
        // Best-effort cleanup: if we are tearing down and the connection is gone,
        // skip re-establishing it.
        return {};
    }

    (void)m_connection->post_message(Messages::WebAudioServer::DestroyWebaudioSession(session_id));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionAddWorkletModule(session_id, module_id, move(url), move(source_text)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_render_graph(u64 session_id, ByteBuffer encoded_graph)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetRenderGraph(session_id, move(encoded_graph)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_suspended(u64 session_id, bool suspended, u64 generation)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetSuspended(session_id, suspended, generation));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetMediaElementAudioSourceStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetMediaStreamAudioSourceStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetScriptProcessorStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetWorkletNodePorts(session_id, move(ports)));
    return {};
}

ErrorOr<void> WebAudioClient::webaudio_session_set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::WebAudioServer::WebaudioSessionSetWorkletNodeDefinitions(session_id, move(definitions)));
    return {};
}

ErrorOr<Core::SharedBufferStream> WebAudioClient::webaudio_session_create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count)
{
    TRY(ensure_connection(0));

    auto response = m_connection->send_sync_but_allow_failure<Messages::WebAudioServer::WebaudioSessionCreateAnalyserStream>(session_id, analyser_node_id, fft_size, block_count);
    if (!response)
        return Error::from_string_literal("WebAudioClient: create analyser stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal("WebAudioClient: server returned invalid analyser stream buffers");

    return Core::SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

ErrorOr<Core::SharedBufferStream> WebAudioClient::webaudio_session_create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count)
{
    TRY(ensure_connection(0));

    auto response = m_connection->send_sync_but_allow_failure<Messages::WebAudioServer::WebaudioSessionCreateDynamicsCompressorStream>(session_id, compressor_node_id, block_count);
    if (!response)
        return Error::from_string_literal("WebAudioClient: create dynamics compressor stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal("WebAudioClient: server returned invalid dynamics compressor stream buffers");

    return Core::SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

}
