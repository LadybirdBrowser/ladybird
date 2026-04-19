/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/Optional.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibWebAudio/Debug.h>
#include <LibWebAudio/Engine/AudioContextPlaybackPolicy.h>
#include <LibWebAudio/Engine/GraphCodec.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/SharedBufferStream.h>
#include <WebAudioWorker/WebAudioBrokerConnection.h>
#include <WebAudioWorker/WebAudioRenderThread.h>
#include <WebAudioWorker/WebAudioSessionConnection.h>

namespace Web::WebAudio {

static Atomic<u64> s_next_webaudio_session_id { 1 };

static IDAllocator s_client_ids;
static HashMap<int, RefPtr<WebAudioSessionConnection>> s_connections;
static bool s_audio_server_ready { false };
static Optional<ByteString> s_audio_server_connect_error;

struct PendingCreateSessionRequest {
    int connection_id { -1 };
    u64 request_id { 0 };
    u32 target_latency_ms { 0 };
};

static Vector<PendingCreateSessionRequest> s_pending_create_session_requests;

static void log_webaudio_graph_summary(int client_id, u64 session_id,
    Web::WebAudio::Render::WireGraphBuildResult const& build)
{
    bool contains_external = (build.flags & Web::WebAudio::Render::WireFlags::contains_external_resources) != 0;

    char const* external_text = contains_external ? "+external" : "";

    dbgln("cid={}: WebAudio graph update session={} flags=0x{:x}{} sr={}Hz dest={} nodes={} conns={} pconns={} "
          "automation={}",
        client_id, session_id, build.flags, external_text, build.context_sample_rate_hz,
        build.description.destination_node_id.value(), build.description.nodes.size(),
        build.description.connections.size(), build.description.param_connections.size(),
        build.param_automation_event_count);
}

WebAudioSessionConnection::WebAudioSessionConnection(NonnullOwnPtr<IPC::Transport> transport,
    int owner_client_id)
    : IPC::ConnectionFromClient<ToSessionClientFromWebAudioWorkerEndpoint,
          ToWebAudioWorkerFromSessionClientEndpoint>(*this, move(transport),
          s_client_ids.allocate())
    , m_owner_client_id(owner_client_id)
{
    s_connections.set(client_id(), *this);
}

WebAudioSessionConnection::~WebAudioSessionConnection()
{
    for (auto const& it : m_webaudio_sessions)
        WebAudioRenderThread::the().unregister_session(it.key);
    m_webaudio_sessions.clear();
}

bool WebAudioSessionConnection::has_any_connection() { return !s_connections.is_empty(); }

void WebAudioSessionConnection::audio_server_did_become_ready()
{
    s_audio_server_ready = true;
    s_audio_server_connect_error.clear();

    auto pending_requests = move(s_pending_create_session_requests);
    s_pending_create_session_requests.clear();

    for (auto const& pending_request : pending_requests) {
        auto connection = s_connections.get(pending_request.connection_id);
        if (!connection.has_value())
            continue;
        connection.value()->begin_create_session(pending_request.request_id, pending_request.target_latency_ms);
    }
}

void WebAudioSessionConnection::audio_server_did_fail_to_connect(ByteString const& error)
{
    s_audio_server_ready = false;
    s_audio_server_connect_error = error;

    auto pending_requests = move(s_pending_create_session_requests);
    s_pending_create_session_requests.clear();

    for (auto const& pending_request : pending_requests) {
        auto connection = s_connections.get(pending_request.connection_id);
        if (!connection.has_value())
            continue;
        warnln("client_cid={}: WebAudio output setup failed before session start: {}",
            connection.value()->m_owner_client_id, error);
        (void)connection.value()->post_message(
            Messages::ToSessionClientFromWebAudioWorker::SessionFailed(pending_request.request_id, error));
    }
}

void WebAudioSessionConnection::shutdown()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    s_pending_create_session_requests.remove_all_matching(
        [id](auto const& request) { return request.connection_id == id; });

    WebAudioBrokerConnection::maybe_quit_event_loop_if_unused();
}

void WebAudioSessionConnection::begin_create_session(u64 request_id, u32 target_latency_ms)
{
    auto output_open_or_error = WebAudioRenderThread::the().open_output_stream(
        target_latency_ms, [request_id, connection_id = client_id()](ErrorOr<WebAudioRenderThread::OutputFormat> output_format_or_error) {
            auto connection = s_connections.get(connection_id);
            if (!connection.has_value())
                return;
            connection.value()->finish_create_session(request_id, move(output_format_or_error));
        });
    if (output_open_or_error.is_error()) {
        warnln("client_cid={}: failed to begin async WebAudio output setup: {}", m_owner_client_id,
            output_open_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(
            request_id, ByteString::formatted("{}", output_open_or_error.error())));
    }
}

void WebAudioSessionConnection::create_session(u64 request_id, u32 target_latency_ms)
{
    if (s_audio_server_ready) {
        begin_create_session(request_id, target_latency_ms);
        return;
    }

    if (s_audio_server_connect_error.has_value()) {
        warnln("client_cid={}: failed to begin async WebAudio output setup: {}", m_owner_client_id,
            s_audio_server_connect_error.value());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(
            request_id, s_audio_server_connect_error.value()));
        return;
    }

    if (should_log_webaudio(LOG_GENERAL))
        dbgln("client_cid={}: queueing WebAudio session creation until AudioServer is ready", m_owner_client_id);
    s_pending_create_session_requests.append({
        .connection_id = client_id(),
        .request_id = request_id,
        .target_latency_ms = target_latency_ms,
    });
}

void WebAudioSessionConnection::finish_create_session(
    u64 request_id, ErrorOr<WebAudioRenderThread::OutputFormat> output_format_or_error)
{
    if (output_format_or_error.is_error()) {
        warnln("client_cid={}: failed to ensure WebAudio output: {}", m_owner_client_id,
            output_format_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(
            request_id, ByteString::formatted("{}", output_format_or_error.error())));
        return;
    }
    auto output_format = output_format_or_error.release_value();
    u32 sample_rate = output_format.sample_rate_hz;
    u32 channel_count = output_format.channel_count;
    if (sample_rate == 0 || channel_count == 0) {
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(
            request_id, ByteString::formatted("Invalid output device format")));
        return;
    }

    u64 session_id = s_next_webaudio_session_id.fetch_add(1, AK::MemoryOrder::memory_order_acq_rel);
    if (should_log_webaudio(LOG_GENERAL))
        dbgln("client_cid={}: allocated WebAudio session_id={} for connection_id={} request_id={}", m_owner_client_id, session_id, client_id(), request_id);

    auto timing_transport_or_error = Render::AudioContextPlaybackPolicy::create_timing_transport();
    if (timing_transport_or_error.is_error()) {
        warnln("client_cid={}: failed to allocate WebAudio timing transport: {}", m_owner_client_id,
            timing_transport_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(
            request_id, ByteString::formatted("{}", timing_transport_or_error.error())));
        return;
    }
    auto timing_transport = timing_transport_or_error.release_value();

    auto session = adopt_ref(*new WebAudioSession(session_id, sample_rate, channel_count,
        timing_transport.timing_buffer, timing_transport.timing_notify_write_fd, m_owner_client_id));
    session->set_worklet_processor_error_callback([this, session_id](Web::WebAudio::NodeID node_id) {
        auto result = post_message(
            Messages::ToSessionClientFromWebAudioWorker::WorkletProcessorError(session_id, node_id.value()));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet processor error: {}", m_owner_client_id,
                session_id, result.error());
    });
    session->set_worklet_processor_registration_callback(
        [this, session_id](String const& name,
            Vector<Web::WebAudio::Render::AudioParamDescriptor> const& descriptors,
            u64 generation) {
            auto result = post_message(Messages::ToSessionClientFromWebAudioWorker::WorkletProcessorRegistered(
                session_id, String { name }, Vector<Web::WebAudio::Render::AudioParamDescriptor> { descriptors },
                generation));
            if (result.is_error())
                warnln("cid={}: WebAudio session={} failed to post worklet processor registration: {}",
                    m_owner_client_id, session_id, result.error());
        });
    session->set_worklet_module_evaluation_callback(
        [this, session_id](u64 module_id, u64 required_generation, bool success, String const& error_name,
            String const& error_message, Vector<String> failed_processor_registrations) {
            auto result = post_message(Messages::ToSessionClientFromWebAudioWorker::WorkletModuleEvaluated(
                session_id, module_id, required_generation, success, error_name, error_message,
                move(failed_processor_registrations)));
            if (result.is_error())
                warnln("cid={}: WebAudio session={} failed to post worklet module evaluation: {}",
                    m_owner_client_id, session_id, result.error());
        });

    WebAudioRenderThread::the().register_session(session);
    m_webaudio_sessions.set(session_id, session);

    (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionCreated(
        request_id, session_id, sample_rate, channel_count, move(timing_transport.timing_buffer),
        move(timing_transport.timing_notify_read_fd)));
}

void WebAudioSessionConnection::destroy_session(u64 session_id)
{
    WebAudioRenderThread::the().unregister_session(session_id);
    m_webaudio_sessions.remove(session_id);
}

void WebAudioSessionConnection::add_worklet_module(u64 session_id, u64 module_id, ByteString url,
    ByteString source_text)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->add_worklet_module(module_id, move(url), move(source_text));
}

void WebAudioSessionConnection::set_render_graph(
    u64 session_id, ByteBuffer encoded_graph,
    Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    auto decoded_or_error = Web::WebAudio::Render::decode_render_graph_wire_format(encoded_graph.bytes(),
        shared_audio_buffers.span());
    if (decoded_or_error.is_error()) {
        warnln("client_cid={}: invalid WebAudio render graph for session {}: {}", m_owner_client_id, session_id,
            decoded_or_error.error());
        return;
    }

    auto build = decoded_or_error.release_value();
    if (Web::WebAudio::should_log_webaudio(LOG_GENERAL))
        log_webaudio_graph_summary(m_owner_client_id, session_id, build);
    session_or_error.value()->set_render_graph(move(build));
}

void WebAudioSessionConnection::set_suspended(u64 session_id, bool suspended, u64 generation)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_suspended(suspended, generation);
}

void WebAudioSessionConnection::set_media_element_audio_source_streams(
    u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_element_audio_source_streams(move(streams));
}

void WebAudioSessionConnection::set_media_stream_audio_source_streams(
    u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_stream_audio_source_streams(streams);
}

void WebAudioSessionConnection::set_script_processor_streams(
    u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_script_processor_streams(move(streams));
}

void WebAudioSessionConnection::set_worklet_node_ports(
    u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_ports(move(ports));
}

void WebAudioSessionConnection::set_worklet_node_definitions(
    u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_definitions(definitions);
}

Messages::ToWebAudioWorkerFromSessionClient::CreateAnalyserStreamResponse
WebAudioSessionConnection::create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size,
    u32 block_count)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (fft_size < 32 || fft_size > 32768 || !is_power_of_two(fft_size))
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (block_count == 0)
        block_count = 4;
    block_count = clamp(block_count, 2u, 32u);

    u32 block_size = static_cast<u32>(Web::WebAudio::Render::analyser_feedback_page_size(fft_size));
    if (block_size == 0)
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto stream_or_error = SharedBufferStream::create(block_size, block_count);
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to create analyser stream: {}", m_owner_client_id, stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }
    auto stream = stream_or_error.release_value();

    session_or_error.value()->set_analyser_stream(analyser_node_id, fft_size, move(stream.stream));
    return { move(stream.buffers.pool_buffer), move(stream.buffers.ready_ring_buffer),
        move(stream.buffers.free_ring_buffer) };
}

Messages::ToWebAudioWorkerFromSessionClient::CreateDynamicsCompressorStreamResponse
WebAudioSessionConnection::create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id,
    u32 block_count)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (block_count == 0)
        block_count = 4;
    block_count = clamp(block_count, 2u, 32u);

    u32 block_size = static_cast<u32>(sizeof(Web::WebAudio::Render::CompressorFeedbackPage));
    auto stream_or_error = SharedBufferStream::create(block_size, block_count);
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to create dynamics compressor stream: {}", m_owner_client_id,
            stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }
    auto stream = stream_or_error.release_value();

    session_or_error.value()->set_dynamics_compressor_stream(compressor_node_id, move(stream.stream));
    return { move(stream.buffers.pool_buffer), move(stream.buffers.ready_ring_buffer),
        move(stream.buffers.free_ring_buffer) };
}

} // namespace Web::WebAudio
