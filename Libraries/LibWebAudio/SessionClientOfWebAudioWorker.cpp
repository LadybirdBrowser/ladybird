/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/SessionClientOfWebAudioWorker.h>

#include <LibIPC/Transport.h>

namespace Web::WebAudio {

NonnullRefPtr<SessionClientOfWebAudioWorker>
SessionClientOfWebAudioWorker::create(NonnullOwnPtr<IPC::Transport> transport)
{
    return adopt_ref(*new SessionClientOfWebAudioWorker(move(transport)));
}

SessionClientOfWebAudioWorker::SessionClientOfWebAudioWorker(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToSessionClientFromWebAudioWorkerEndpoint,
          ToWebAudioWorkerFromSessionClientEndpoint>(*this, move(transport))
{
}

void SessionClientOfWebAudioWorker::shutdown()
{
    fail_pending_create_session_requests();

    [[maybe_unused]] auto self = NonnullRefPtr(*this);
    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

void SessionClientOfWebAudioWorker::worklet_processor_error(u64 session_id, u64 node_id)
{
    if (on_worklet_processor_error)
        on_worklet_processor_error(session_id, node_id);
}

void SessionClientOfWebAudioWorker::worklet_processor_registered(
    u64 session_id, String name, Vector<Web::WebAudio::Render::AudioParamDescriptor> descriptors,
    u64 generation)
{
    if (on_worklet_processor_registered)
        on_worklet_processor_registered(session_id, name, descriptors, generation);
}

void SessionClientOfWebAudioWorker::worklet_module_evaluated(u64 session_id, u64 module_id,
    u64 required_generation, bool success,
    String error_name, String error_message,
    Vector<String> failed_processor_registrations)
{
    if (on_worklet_module_evaluated)
        on_worklet_module_evaluated(session_id, module_id, required_generation, success, error_name,
            error_message, failed_processor_registrations);
}

ErrorOr<void>
SessionClientOfWebAudioWorker::create_session_async(u32 target_latency_ms,
    Function<void(ErrorOr<WebAudioSession>&&)> on_complete)
{
    u64 request_id = m_next_create_session_request_id++;
    m_pending_create_session_requests.set(request_id, PendingCreateSessionRequest {
                                                          .on_complete = move(on_complete),
                                                      });

    auto post_result = post_message(Messages::ToWebAudioWorkerFromSessionClient::CreateSession(request_id, target_latency_ms));
    if (post_result.is_error()) {
        m_pending_create_session_requests.remove(request_id);
        return post_result.release_error();
    }

    return {};
}

ErrorOr<void> SessionClientOfWebAudioWorker::destroy_session(u64 session_id)
{
    return post_message(Messages::ToWebAudioWorkerFromSessionClient::DestroySession(session_id));
}

ErrorOr<void> SessionClientOfWebAudioWorker::add_worklet_module(u64 session_id, u64 module_id, ByteString url,
    ByteString source_text)
{
    return post_message(Messages::ToWebAudioWorkerFromSessionClient::AddWorkletModule(
        session_id, module_id, move(url), move(source_text)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_render_graph(
    u64 session_id, ByteBuffer encoded_graph,
    Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers)
{
    return post_message(Messages::ToWebAudioWorkerFromSessionClient::SetRenderGraph(
        session_id, move(encoded_graph), move(shared_audio_buffers)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_suspended(u64 session_id, bool suspended, u64 generation)
{
    return post_message(
        Messages::ToWebAudioWorkerFromSessionClient::SetSuspended(session_id, suspended, generation));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_media_element_audio_source_streams(
    u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    return post_message(Messages::ToWebAudioWorkerFromSessionClient::SetMediaElementAudioSourceStreams(
        session_id, move(streams)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_media_stream_audio_source_streams(
    u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    return post_message(Messages::ToWebAudioWorkerFromSessionClient::SetMediaStreamAudioSourceStreams(
        session_id, move(streams)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_script_processor_streams(
    u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    return post_message(
        Messages::ToWebAudioWorkerFromSessionClient::SetScriptProcessorStreams(session_id, move(streams)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_worklet_node_ports(
    u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    return post_message(
        Messages::ToWebAudioWorkerFromSessionClient::SetWorkletNodePorts(session_id, move(ports)));
}

ErrorOr<void> SessionClientOfWebAudioWorker::set_worklet_node_definitions(
    u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    return post_message(
        Messages::ToWebAudioWorkerFromSessionClient::SetWorkletNodeDefinitions(session_id, move(definitions)));
}

ErrorOr<SharedBufferStream> SessionClientOfWebAudioWorker::create_analyser_stream(u64 session_id,
    u64 analyser_node_id,
    u32 fft_size,
    u32 block_count)
{
    auto response = send_sync_but_allow_failure<Messages::ToWebAudioWorkerFromSessionClient::CreateAnalyserStream>(
        session_id, analyser_node_id, fft_size, block_count);
    if (!response)
        return Error::from_string_literal("SessionClientOfWebAudioWorker: create analyser stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal(
            "SessionClientOfWebAudioWorker: server returned invalid analyser stream buffers");

    return SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

ErrorOr<SharedBufferStream>
SessionClientOfWebAudioWorker::create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id,
    u32 block_count)
{
    auto response = send_sync_but_allow_failure<
        Messages::ToWebAudioWorkerFromSessionClient::CreateDynamicsCompressorStream>(
        session_id, compressor_node_id, block_count);
    if (!response)
        return Error::from_string_literal(
            "SessionClientOfWebAudioWorker: create dynamics compressor stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid()) {
        return Error::from_string_literal(
            "SessionClientOfWebAudioWorker: server returned invalid dynamics compressor stream buffers");
    }

    return SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

void SessionClientOfWebAudioWorker::session_created(u64 request_id, u64 session_id, u32 sample_rate,
    u32 channel_count, Core::AnonymousBuffer timing_buffer,
    IPC::File timing_notify_fd)
{
    auto pending = m_pending_create_session_requests.take(request_id);
    if (!pending.has_value())
        return;

    if (session_id == 0 || sample_rate == 0 || channel_count == 0 || !timing_buffer.is_valid() || timing_notify_fd.fd() < 0) {
        pending->on_complete(Error::from_string_literal(
            "SessionClientOfWebAudioWorker: server returned invalid session parameters"));
        return;
    }

    pending->on_complete(WebAudioSession {
        .session_id = session_id,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .timing_buffer = move(timing_buffer),
        .timing_notify_fd = move(timing_notify_fd),
    });
}

void SessionClientOfWebAudioWorker::session_failed(u64 request_id, ByteString)
{
    auto pending = m_pending_create_session_requests.take(request_id);
    if (!pending.has_value())
        return;
    pending->on_complete(Error::from_string_literal("SessionClientOfWebAudioWorker: session creation failed"));
}

void SessionClientOfWebAudioWorker::fail_pending_create_session_requests()
{
    Vector<u64> pending_request_ids;
    pending_request_ids.ensure_capacity(m_pending_create_session_requests.size());
    for (auto const& it : m_pending_create_session_requests)
        pending_request_ids.append(it.key);

    for (u64 request_id : pending_request_ids) {
        auto pending = m_pending_create_session_requests.take(request_id);
        if (!pending.has_value())
            continue;

        pending->on_complete(Error::from_string_literal("SessionClientOfWebAudioWorker: connection closed"));
    }
}

} // namespace Web::WebAudio
