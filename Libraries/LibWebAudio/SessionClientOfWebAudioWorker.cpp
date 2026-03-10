/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/SessionClientOfWebAudioWorker.h>

#include <LibCore/Socket.h>
#include <LibIPC/Transport.h>

namespace Web::WebAudio {

NonnullRefPtr<SessionClientOfWebAudioWorker> SessionClientOfWebAudioWorker::create()
{
    return adopt_ref(*new SessionClientOfWebAudioWorker);
}

SessionClientOfWebAudioWorker::WebAudioSessionConnection::WebAudioSessionConnection(SessionClientOfWebAudioWorker& client, NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToSessionClientFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromSessionClientEndpoint>(*this, move(transport))
    , m_client(client)
{
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::shutdown()
{
    m_client.fail_pending_create_session_requests(ByteString::formatted("WebAudioClient: connection closed"));

    auto self = NonnullRefPtr(*this);
    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::session_created(u64 request_id, u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer timing_buffer, IPC::File timing_notify_fd)
{
    m_client.handle_session_created(request_id, session_id, sample_rate, channel_count, move(timing_buffer), move(timing_notify_fd));
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::session_failed(u64 request_id, ByteString error_message)
{
    m_client.handle_session_failed(request_id, error_message);
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::worklet_processor_error(u64 session_id, u64 node_id)
{
    if (m_client.on_worklet_processor_error)
        m_client.on_worklet_processor_error(session_id, node_id);
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::worklet_processor_registered(u64 session_id, String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation)
{
    if (m_client.on_worklet_processor_registered)
        m_client.on_worklet_processor_registered(session_id, name, descriptors, generation);
}

void SessionClientOfWebAudioWorker::WebAudioSessionConnection::worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String error_name, String error_message, Vector<String> failed_processor_registrations)
{
    if (m_client.on_worklet_module_evaluated)
        m_client.on_worklet_module_evaluated(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
}

void SessionClientOfWebAudioWorker::set_socket_provider(Function<ErrorOr<IPC::File>(u64 page_id)> provider)
{
    m_socket_provider = move(provider);
}

ErrorOr<void> SessionClientOfWebAudioWorker::ensure_connection(u64 page_id)
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
    m_connection = adopt_ref(*new WebAudioSessionConnection(*this, move(transport)));

    m_connection->on_death = [weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref()) {
            self->m_connection = nullptr;
            self->m_active_session_ids.clear();
            auto death_callback = move(self->on_death);
            self->on_death = {};
            if (death_callback)
                death_callback();
        }
    };

    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::create_session_async(u32 target_latency_ms, u64 page_id, Function<void(ErrorOr<WebAudioSession>&&)> on_complete)
{
    TRY(ensure_connection(page_id));

    struct CallbackState final : public RefCounted<CallbackState> {
        explicit CallbackState(Function<void(ErrorOr<WebAudioSession>&&)> callback)
            : on_complete(move(callback))
        {
        }

        void complete(ErrorOr<WebAudioSession>&& result)
        {
            if (!on_complete)
                return;
            auto callback = move(on_complete);
            callback(move(result));
        }

        Function<void(ErrorOr<WebAudioSession>&&)> on_complete;
    };

    auto callback_state = adopt_ref(*new CallbackState(move(on_complete)));

    u64 request_id = m_next_create_session_request_id++;
    m_pending_create_session_requests.set(request_id, PendingCreateSessionRequest {
                                                          .on_success = [callback_state](WebAudioSession&& session) { callback_state->complete(move(session)); },
                                                          .on_error = [callback_state](ByteString const&) { callback_state->complete(Error::from_string_literal("WebAudioClient: create webaudio session failed")); },
                                                      });

    auto post_result = m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::CreateSession(request_id, target_latency_ms));
    if (post_result.is_error()) {
        m_pending_create_session_requests.remove(request_id);
        return post_result.release_error();
    }

    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::destroy_session(u64 session_id)
{
    if (!m_connection || !m_connection->is_open())
        return Error::from_string_literal("WebAudioClient: no active connection for destroy_session");

    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::DestroySession(session_id));
    m_active_session_ids.remove(session_id);
    maybe_release_idle_connection();
    return {};
}

void SessionClientOfWebAudioWorker::maybe_release_idle_connection()
{
    if (!m_connection)
        return;
    if (!m_active_session_ids.is_empty())
        return;
    if (!m_pending_create_session_requests.is_empty())
        return;

    m_connection = nullptr;
}

ErrorOr<void> SessionClientOfWebAudioWorker::add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::AddWorkletModule(session_id, module_id, move(url), move(source_text)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_render_graph(u64 session_id, ByteBuffer encoded_graph, Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetRenderGraph(session_id, move(encoded_graph), move(shared_audio_buffers)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_suspended(u64 session_id, bool suspended, u64 generation)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetSuspended(session_id, suspended, generation));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetMediaElementAudioSourceStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetMediaStreamAudioSourceStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetScriptProcessorStreams(session_id, move(streams)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetWorkletNodePorts(session_id, move(ports)));
    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    TRY(ensure_connection(0));
    (void)m_connection->post_message(Messages::ToWebAudioWorkerFromSessionClient::SetWorkletNodeDefinitions(session_id, move(definitions)));
    return {};
}

ErrorOr<SharedBufferStream> SessionClientOfWebAudioWorker::create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count)
{
    TRY(ensure_connection(0));

    auto response = m_connection->send_sync_but_allow_failure<Messages::ToWebAudioWorkerFromSessionClient::CreateAnalyserStream>(session_id, analyser_node_id, fft_size, block_count);
    if (!response)
        return Error::from_string_literal("WebAudioClient: create analyser stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal("WebAudioClient: server returned invalid analyser stream buffers");

    return SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

ErrorOr<SharedBufferStream> SessionClientOfWebAudioWorker::create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count)
{
    TRY(ensure_connection(0));

    auto response = m_connection->send_sync_but_allow_failure<Messages::ToWebAudioWorkerFromSessionClient::CreateDynamicsCompressorStream>(session_id, compressor_node_id, block_count);
    if (!response)
        return Error::from_string_literal("WebAudioClient: create dynamics compressor stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal("WebAudioClient: server returned invalid dynamics compressor stream buffers");

    return SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

void SessionClientOfWebAudioWorker::handle_session_created(u64 request_id, u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer timing_buffer, IPC::File timing_notify_fd)
{
    auto pending = m_pending_create_session_requests.take(request_id);
    if (!pending.has_value())
        return;

    if (session_id == 0) {
        pending->on_error(ByteString::formatted("WebAudioClient: server returned invalid webaudio session"));
        return;
    }
    if (sample_rate == 0 || channel_count == 0) {
        pending->on_error(ByteString::formatted("WebAudioClient: server returned invalid webaudio device format"));
        return;
    }
    if (!timing_buffer.is_valid()) {
        pending->on_error(ByteString::formatted("WebAudioClient: server returned invalid webaudio timing buffer"));
        return;
    }
    if (timing_notify_fd.fd() < 0) {
        pending->on_error(ByteString::formatted("WebAudioClient: server returned invalid webaudio timing notifier fd"));
        return;
    }

    pending->on_success(WebAudioSession {
        .session_id = session_id,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .timing_buffer = move(timing_buffer),
        .timing_notify_fd = move(timing_notify_fd),
    });

    m_active_session_ids.set(session_id);
}

void SessionClientOfWebAudioWorker::handle_session_failed(u64 request_id, ByteString const& error_message)
{
    auto pending = m_pending_create_session_requests.take(request_id);
    if (!pending.has_value())
        return;
    pending->on_error(error_message);
}

void SessionClientOfWebAudioWorker::fail_pending_create_session_requests(ByteString const& error_message)
{
    Vector<u64> pending_request_ids;
    pending_request_ids.ensure_capacity(m_pending_create_session_requests.size());
    for (auto const& it : m_pending_create_session_requests)
        pending_request_ids.append(it.key);

    for (u64 request_id : pending_request_ids) {
        auto pending = m_pending_create_session_requests.take(request_id);
        if (!pending.has_value())
            continue;
        pending->on_error(error_message);
    }
}

}
