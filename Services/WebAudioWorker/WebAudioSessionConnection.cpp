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
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/SharedBufferStream.h>
#include <WebAudioWorker/WebAudioBrokerConnection.h>
#include <WebAudioWorker/WebAudioRenderThread.h>
#include <WebAudioWorker/WebAudioSessionConnection.h>
#include <bit>

namespace Web::WebAudio {

static IDAllocator s_client_ids;
static HashMap<int, RefPtr<WebAudioSessionConnection>> s_connections;

static void log_webaudio_graph_summary(int client_id, u64 session_id, Web::WebAudio::Render::WireGraphBuildResult const& build)
{
    bool contains_external = (build.flags & Web::WebAudio::Render::WireFlags::contains_external_resources) != 0;

    char const* external_text = contains_external ? "+external" : "";

    dbgln("cid={}: WebAudio graph update session={} flags=0x{:x}{} sr={}Hz dest={} nodes={} conns={} pconns={} automation={}",
        client_id,
        session_id,
        build.flags,
        external_text,
        build.context_sample_rate_hz,
        build.description.destination_node_id.value(),
        build.description.nodes.size(),
        build.description.connections.size(),
        build.description.param_connections.size(),
        build.param_automation_event_count);
}

WebAudioSessionConnection::WebAudioSessionConnection(NonnullOwnPtr<IPC::Transport> transport, int owner_client_id)
    : IPC::ConnectionFromClient<ToSessionClientFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromSessionClientEndpoint>(*this, move(transport), s_client_ids.allocate())
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

bool WebAudioSessionConnection::has_any_connection()
{
    return !s_connections.is_empty();
}

void WebAudioSessionConnection::shutdown()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    WebAudioBrokerConnection::maybe_quit_event_loop_if_unused();
}

void WebAudioSessionConnection::create_session(u64 request_id, u32 target_latency_ms)
{
    auto output_open_or_error = WebAudioRenderThread::the().open_output_stream(target_latency_ms, [request_id, connection_id = client_id()](ErrorOr<WebAudioRenderThread::OutputFormat> output_format_or_error) {
        auto connection = s_connections.get(connection_id);
        if (!connection.has_value())
            return;
        connection.value()->finish_create_session(request_id, move(output_format_or_error));
    });
    if (output_open_or_error.is_error()) {
        warnln("client_cid={}: failed to begin async WebAudio output setup: {}", m_owner_client_id, output_open_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(request_id, ByteString::formatted("{}", output_open_or_error.error())));
    }
}

void WebAudioSessionConnection::finish_create_session(u64 request_id, ErrorOr<WebAudioRenderThread::OutputFormat> output_format_or_error)
{
    if (output_format_or_error.is_error()) {
        warnln("client_cid={}: failed to ensure WebAudio output: {}", m_owner_client_id, output_format_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(request_id, ByteString::formatted("{}", output_format_or_error.error())));
        return;
    }
    auto output_format = output_format_or_error.release_value();
    u32 sample_rate = output_format.sample_rate_hz;
    u32 channel_count = output_format.channel_count;
    if (sample_rate == 0 || channel_count == 0) {
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(request_id, ByteString::formatted("Invalid output device format")));
        return;
    }

    auto session_id = m_next_webaudio_session_id++;

    auto timing_buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Web::WebAudio::Render::TimingFeedbackPage));
    if (timing_buffer_or_error.is_error()) {
        warnln("client_cid={}: failed to allocate WebAudio timing buffer: {}", m_owner_client_id, timing_buffer_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(request_id, ByteString::formatted("{}", timing_buffer_or_error.error())));
        return;
    }
    auto timing_buffer = timing_buffer_or_error.release_value();

    auto pipe_fds_or_error = Core::System::pipe2(O_CLOEXEC | O_NONBLOCK);
    if (pipe_fds_or_error.is_error()) {
        warnln("client_cid={}: failed to allocate WebAudio timing notifier pipe: {}", m_owner_client_id, pipe_fds_or_error.error());
        (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionFailed(request_id, ByteString::formatted("{}", pipe_fds_or_error.error())));
        return;
    }
    auto pipe_fds = pipe_fds_or_error.release_value();
    auto timing_notify_read_fd = IPC::File::adopt_fd(pipe_fds[0]);
    int timing_notify_write_fd = pipe_fds[1];

    auto session = adopt_ref(*new WebAudioSession(session_id, sample_rate, channel_count, timing_buffer, timing_notify_write_fd, m_owner_client_id));
    session->set_worklet_processor_error_callback([this, session_id](Web::WebAudio::NodeID node_id) {
        auto result = post_message(Messages::ToSessionClientFromWebAudioWorker::WorkletProcessorError(session_id, node_id.value()));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet processor error: {}", m_owner_client_id, session_id, result.error());
    });
    session->set_worklet_processor_registration_callback([this, session_id](String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        auto result = post_message(Messages::ToSessionClientFromWebAudioWorker::WorkletProcessorRegistered(session_id, String { name }, Vector<Web::WebAudio::AudioParamDescriptor> { descriptors }, generation));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet processor registration: {}", m_owner_client_id, session_id, result.error());
    });
    session->set_worklet_module_evaluation_callback([this, session_id](u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations) {
        auto result = post_message(Messages::ToSessionClientFromWebAudioWorker::WorkletModuleEvaluated(session_id, module_id, required_generation, success, error_name, error_message, move(failed_processor_registrations)));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet module evaluation: {}", m_owner_client_id, session_id, result.error());
    });

    WebAudioRenderThread::the().register_session(session);
    m_webaudio_sessions.set(session_id, session);

    (void)post_message(Messages::ToSessionClientFromWebAudioWorker::SessionCreated(request_id, session_id, sample_rate, channel_count, move(timing_buffer), move(timing_notify_read_fd)));
}

void WebAudioSessionConnection::destroy_session(u64 session_id)
{
    WebAudioRenderThread::the().unregister_session(session_id);
    m_webaudio_sessions.remove(session_id);
}

void WebAudioSessionConnection::add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->add_worklet_module(module_id, move(url), move(source_text));
}

void WebAudioSessionConnection::set_render_graph(u64 session_id, ByteBuffer encoded_graph, Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    auto decoded_or_error = Web::WebAudio::Render::decode_render_graph_wire_format(encoded_graph.bytes(), shared_audio_buffers.span());
    if (decoded_or_error.is_error()) {
        warnln("client_cid={}: invalid WebAudio render graph for session {}: {}", m_owner_client_id, session_id, decoded_or_error.error());
        return;
    }

    auto build = decoded_or_error.release_value();
    if (Web::WebAudio::should_log_info())
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

void WebAudioSessionConnection::set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_element_audio_source_streams(move(streams));
}

void WebAudioSessionConnection::set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_stream_audio_source_streams(streams);
}

void WebAudioSessionConnection::set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_script_processor_streams(move(streams));
}

void WebAudioSessionConnection::set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_ports(ports);
}

void WebAudioSessionConnection::set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_definitions(definitions);
}

struct SharedBufferStreamBuffers {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
};

static Optional<SharedBufferStreamBuffers> create_shared_buffer_stream_buffers(u32 block_size, u32 block_count, char const* debug_name, int client_id)
{
    if (block_size == 0 || block_count == 0)
        return {};

    auto pool_bytes = SharedBufferStream::pool_buffer_size_bytes(block_size, block_count);
    auto pool_buffer_or_error = Core::AnonymousBuffer::create_with_size(pool_bytes);
    if (pool_buffer_or_error.is_error()) {
        warnln("cid={}: failed to allocate {} stream pool: {}", client_id, debug_name, pool_buffer_or_error.error());
        return {};
    }

    auto pool_buffer = pool_buffer_or_error.release_value();
    auto* header = reinterpret_cast<SharedBufferStream::PoolHeader*>(pool_buffer.data<void>());
    if (!header)
        return {};

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = SharedBufferStream::s_pool_magic;
    header->version = SharedBufferStream::s_pool_version;
    header->block_size = block_size;
    header->block_count = block_count;

    size_t ring_capacity_bytes = std::bit_ceil(static_cast<size_t>(block_count) * sizeof(SharedBufferStream::Descriptor));
    ring_capacity_bytes = max(ring_capacity_bytes, 64ul);

    auto ready_ring_or_error = AudioServer::SharedCircularBuffer::create(ring_capacity_bytes);
    if (ready_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream ready ring: {}", client_id, debug_name, ready_ring_or_error.error());
        return {};
    }

    auto free_ring_or_error = AudioServer::SharedCircularBuffer::create(ring_capacity_bytes);
    if (free_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream free ring: {}", client_id, debug_name, free_ring_or_error.error());
        return {};
    }

    auto ready_ring = ready_ring_or_error.release_value();
    auto free_ring = free_ring_or_error.release_value();

    for (u32 i = 0; i < block_count; ++i) {
        SharedBufferStream::Descriptor desc { i, 0 };
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&desc), sizeof(desc) };
        if (free_ring.try_write(bytes) != sizeof(desc)) {
            warnln("cid={}: failed to seed {} stream free ring (i={})", client_id, debug_name, i);
            return {};
        }
    }

    return SharedBufferStreamBuffers {
        .pool_buffer = move(pool_buffer),
        .ready_ring_buffer = ready_ring.anonymous_buffer(),
        .free_ring_buffer = free_ring.anonymous_buffer(),
    };
}

Messages::ToWebAudioWorkerFromSessionClient::CreateAnalyserStreamResponse WebAudioSessionConnection::create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count)
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

    auto buffers = create_shared_buffer_stream_buffers(block_size, block_count, "analyser", m_owner_client_id);
    if (!buffers.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto pool_for_stream = buffers->pool_buffer;
    auto ready_for_stream = buffers->ready_ring_buffer;
    auto free_for_stream = buffers->free_ring_buffer;

    auto stream_or_error = SharedBufferStream::attach(move(pool_for_stream), move(ready_for_stream), move(free_for_stream));
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to attach analyser stream: {}", m_owner_client_id, stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }

    session_or_error.value()->set_analyser_stream(analyser_node_id, fft_size, stream_or_error.release_value());
    return { move(buffers->pool_buffer), move(buffers->ready_ring_buffer), move(buffers->free_ring_buffer) };
}

Messages::ToWebAudioWorkerFromSessionClient::CreateDynamicsCompressorStreamResponse WebAudioSessionConnection::create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (block_count == 0)
        block_count = 4;
    block_count = clamp(block_count, 2u, 32u);

    u32 block_size = static_cast<u32>(sizeof(Web::WebAudio::Render::CompressorFeedbackPage));
    auto buffers = create_shared_buffer_stream_buffers(block_size, block_count, "dynamics_compressor", m_owner_client_id);
    if (!buffers.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto pool_for_stream = buffers->pool_buffer;
    auto ready_for_stream = buffers->ready_ring_buffer;
    auto free_for_stream = buffers->free_ring_buffer;

    auto stream_or_error = SharedBufferStream::attach(move(pool_for_stream), move(ready_for_stream), move(free_for_stream));
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to attach dynamics compressor stream: {}", m_owner_client_id, stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }

    session_or_error.value()->set_dynamics_compressor_stream(compressor_node_id, stream_or_error.release_value());
    return { move(buffers->pool_buffer), move(buffers->ready_ring_buffer), move(buffers->free_ring_buffer) };
}

}
