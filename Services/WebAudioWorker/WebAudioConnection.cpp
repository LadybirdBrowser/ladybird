/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/IDAllocator.h>
#include <AK/Optional.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/SharedMemory.h>
#include <WebAudioWorker/WebAudioConnection.h>
#include <WebAudioWorker/WebAudioRenderThread.h>
#include <WebAudioWorker/WebAudioWorkerConnection.h>
#include <bit>

namespace WebAudioWorker {

static IDAllocator s_client_ids;
static HashMap<int, RefPtr<WebAudioConnection>> s_connections;

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

WebAudioConnection::WebAudioConnection(NonnullOwnPtr<IPC::Transport> transport, int owner_client_id)
    : IPC::ConnectionFromClient<WebAudioClientEndpoint, WebAudioServerEndpoint>(*this, move(transport), s_client_ids.allocate())
    , m_owner_client_id(owner_client_id)
{
    s_connections.set(client_id(), *this);
}

WebAudioConnection::~WebAudioConnection()
{
    for (auto const& it : m_webaudio_sessions)
        WebAudioRenderThread::the().unregister_session(it.key);
    m_webaudio_sessions.clear();
}

bool WebAudioConnection::has_any_connection()
{
    return !s_connections.is_empty();
}

void WebAudioConnection::die()
{
    auto id = client_id();
    s_connections.remove(id);
    s_client_ids.deallocate(id);

    WebAudioWorkerConnection::maybe_quit_event_loop_if_unused();
}

Messages::WebAudioServer::GetOutputDeviceFormatResponse WebAudioConnection::get_output_device_format()
{
    RefPtr<AudioServerClient::Client> audio_server = AudioServerClient::Client::default_client();
    if (!audio_server)
        return { 0, 0 };

    auto format_or_error = audio_server->get_output_device_format();
    if (format_or_error.is_error())
        return { 0, 0 };

    auto format = format_or_error.release_value();
    return { format.sample_rate, format.channel_count };
}

Messages::WebAudioServer::CreateWebaudioSessionResponse WebAudioConnection::create_webaudio_session(u32 target_latency_ms)
{
    auto output_format_or_error = WebAudioRenderThread::the().ensure_output_open(target_latency_ms);
    if (output_format_or_error.is_error()) {
        warnln("client_cid={}: failed to ensure WebAudio output: {}", m_owner_client_id, output_format_or_error.error());
        return { 0, 0, 0, Core::AnonymousBuffer {}, IPC::File {} };
    }
    auto output_format = output_format_or_error.release_value();
    u32 sample_rate = output_format.sample_rate_hz;
    u32 channel_count = output_format.channel_count;
    if (sample_rate == 0 || channel_count == 0)
        return { 0, 0, 0, Core::AnonymousBuffer {}, IPC::File {} };

    auto session_id = m_next_webaudio_session_id++;

    auto timing_buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Web::WebAudio::Render::WebAudioTimingPage));
    if (timing_buffer_or_error.is_error()) {
        warnln("client_cid={}: failed to allocate WebAudio timing buffer: {}", m_owner_client_id, timing_buffer_or_error.error());
        return { 0, 0, 0, Core::AnonymousBuffer {}, IPC::File {} };
    }
    auto timing_buffer = timing_buffer_or_error.release_value();

    auto pipe_fds_or_error = Core::System::pipe2(O_CLOEXEC | O_NONBLOCK);
    if (pipe_fds_or_error.is_error()) {
        warnln("client_cid={}: failed to allocate WebAudio timing notifier pipe: {}", m_owner_client_id, pipe_fds_or_error.error());
        return { 0, 0, 0, Core::AnonymousBuffer {}, IPC::File {} };
    }
    auto pipe_fds = pipe_fds_or_error.release_value();
    auto timing_notify_read_fd = IPC::File::adopt_fd(pipe_fds[0]);
    int timing_notify_write_fd = pipe_fds[1];

    auto session = adopt_ref(*new WebAudioSession(session_id, sample_rate, channel_count, timing_buffer, timing_notify_write_fd, m_owner_client_id));
    session->set_worklet_processor_error_callback([this, session_id](Web::WebAudio::NodeID node_id) {
        auto result = post_message(Messages::WebAudioClient::WebaudioSessionWorkletProcessorError(session_id, node_id.value()));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet processor error: {}", m_owner_client_id, session_id, result.error());
    });
    session->set_worklet_processor_registration_callback([this, session_id](String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        auto result = post_message(Messages::WebAudioClient::WebaudioSessionWorkletProcessorRegistered(session_id, name, descriptors, generation));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet processor registration: {}", m_owner_client_id, session_id, result.error());
    });
    session->set_worklet_module_evaluation_callback([this, session_id](u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations) {
        auto result = post_message(Messages::WebAudioClient::WebaudioSessionWorkletModuleEvaluated(session_id, module_id, required_generation, success, error_name, error_message, move(failed_processor_registrations)));
        if (result.is_error())
            warnln("cid={}: WebAudio session={} failed to post worklet module evaluation: {}", m_owner_client_id, session_id, result.error());
    });

    WebAudioRenderThread::the().register_session(session);
    m_webaudio_sessions.set(session_id, session);

    return { session_id, sample_rate, channel_count, move(timing_buffer), move(timing_notify_read_fd) };
}

void WebAudioConnection::destroy_webaudio_session(u64 session_id)
{
    WebAudioRenderThread::the().unregister_session(session_id);
    m_webaudio_sessions.remove(session_id);
}

void WebAudioConnection::webaudio_session_add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->add_worklet_module(module_id, move(url), move(source_text));
}

void WebAudioConnection::webaudio_session_set_render_graph(u64 session_id, ByteBuffer encoded_graph)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    auto decoded_or_error = Web::WebAudio::Render::decode_render_graph_wire_format(encoded_graph.bytes());
    if (decoded_or_error.is_error()) {
        warnln("client_cid={}: invalid WebAudio render graph for session {}: {}", m_owner_client_id, session_id, decoded_or_error.error());
        return;
    }

    auto build = decoded_or_error.release_value();
    if (Web::WebAudio::should_log_info())
        log_webaudio_graph_summary(m_owner_client_id, session_id, build);
    session_or_error.value()->set_render_graph(move(build));
}

void WebAudioConnection::webaudio_session_set_suspended(u64 session_id, bool suspended, u64 generation)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_suspended(suspended, generation);
}

void WebAudioConnection::webaudio_session_set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_element_audio_source_streams(move(streams));
}

void WebAudioConnection::webaudio_session_set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_media_stream_audio_source_streams(streams);
}

void WebAudioConnection::webaudio_session_set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_script_processor_streams(move(streams));
}

void WebAudioConnection::webaudio_session_set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_ports(ports);
}

void WebAudioConnection::webaudio_session_set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;

    session_or_error.value()->set_worklet_node_definitions(definitions);
}

Messages::WebAudioServer::WebaudioSessionCreateAudioInputStreamResponse WebAudioConnection::webaudio_session_create_audio_input_stream(u64 session_id, AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { 0 };

    auto stream_id = session_or_error.value()->create_audio_input_stream(device_id, sample_rate_hz, channel_count, capacity_frames, overflow_policy);
    return { stream_id };
}

void WebAudioConnection::webaudio_session_destroy_audio_input_stream(u64 session_id, AudioServer::AudioInputStreamID stream_id)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;
    session_or_error.value()->destroy_audio_input_stream(stream_id);
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

    auto pool_bytes = Core::SharedBufferStream::pool_buffer_size_bytes(block_size, block_count);
    auto pool_buffer_or_error = Core::AnonymousBuffer::create_with_size(pool_bytes);
    if (pool_buffer_or_error.is_error()) {
        warnln("cid={}: failed to allocate {} stream pool: {}", client_id, debug_name, pool_buffer_or_error.error());
        return {};
    }

    auto pool_buffer = pool_buffer_or_error.release_value();
    auto* header = reinterpret_cast<Core::SharedBufferStream::PoolHeader*>(pool_buffer.data<void>());
    if (!header)
        return {};

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = Core::SharedBufferStream::s_pool_magic;
    header->version = Core::SharedBufferStream::s_pool_version;
    header->block_size = block_size;
    header->block_count = block_count;

    size_t ring_capacity_bytes = std::bit_ceil(static_cast<size_t>(block_count) * sizeof(Core::SharedBufferStream::Descriptor));
    ring_capacity_bytes = max(ring_capacity_bytes, 64ul);

    auto ready_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (ready_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream ready ring: {}", client_id, debug_name, ready_ring_or_error.error());
        return {};
    }

    auto free_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (free_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream free ring: {}", client_id, debug_name, free_ring_or_error.error());
        return {};
    }

    auto ready_ring = ready_ring_or_error.release_value();
    auto free_ring = free_ring_or_error.release_value();

    for (u32 i = 0; i < block_count; ++i) {
        Core::SharedBufferStream::Descriptor desc { i, 0 };
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

Messages::WebAudioServer::WebaudioSessionCreateAnalyserStreamResponse WebAudioConnection::webaudio_session_create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (fft_size < 32 || fft_size > 32768 || !is_power_of_two(fft_size))
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (block_count == 0)
        block_count = 4;
    block_count = clamp(block_count, 2u, 32u);

    u32 block_size = static_cast<u32>(Web::WebAudio::Render::webaudio_analyser_snapshot_size_bytes(fft_size));
    if (block_size == 0)
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto buffers = create_shared_buffer_stream_buffers(block_size, block_count, "analyser", m_owner_client_id);
    if (!buffers.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto pool_for_stream = buffers->pool_buffer;
    auto ready_for_stream = buffers->ready_ring_buffer;
    auto free_for_stream = buffers->free_ring_buffer;

    auto stream_or_error = Core::SharedBufferStream::attach(move(pool_for_stream), move(ready_for_stream), move(free_for_stream));
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to attach analyser stream: {}", m_owner_client_id, stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }

    session_or_error.value()->set_analyser_stream(analyser_node_id, fft_size, stream_or_error.release_value());
    return { move(buffers->pool_buffer), move(buffers->ready_ring_buffer), move(buffers->free_ring_buffer) };
}

Messages::WebAudioServer::WebaudioSessionCreateDynamicsCompressorStreamResponse WebAudioConnection::webaudio_session_create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count)
{
    auto session_or_error = m_webaudio_sessions.get(session_id);
    if (!session_or_error.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    if (block_count == 0)
        block_count = 4;
    block_count = clamp(block_count, 2u, 32u);

    u32 block_size = static_cast<u32>(Web::WebAudio::Render::webaudio_dynamics_compressor_snapshot_size_bytes());
    if (block_size == 0)
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto buffers = create_shared_buffer_stream_buffers(block_size, block_count, "dynamics_compressor", m_owner_client_id);
    if (!buffers.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    auto pool_for_stream = buffers->pool_buffer;
    auto ready_for_stream = buffers->ready_ring_buffer;
    auto free_for_stream = buffers->free_ring_buffer;

    auto stream_or_error = Core::SharedBufferStream::attach(move(pool_for_stream), move(ready_for_stream), move(free_for_stream));
    if (stream_or_error.is_error()) {
        warnln("cid={}: failed to attach dynamics compressor stream: {}", m_owner_client_id, stream_or_error.error());
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };
    }

    session_or_error.value()->set_dynamics_compressor_stream(compressor_node_id, stream_or_error.release_value());
    return { move(buffers->pool_buffer), move(buffers->ready_ring_buffer), move(buffers->free_ring_buffer) };
}

}
