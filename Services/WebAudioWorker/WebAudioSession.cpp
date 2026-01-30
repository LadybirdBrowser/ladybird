/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibThreading/Thread.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphExecutor.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/SharedMemory.h>
#include <LibWeb/WebAudio/Engine/StreamTransportValidation.h>
#include <LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <WebAudioWorker/SessionSampler.h>
#include <WebAudioWorker/SessionScriptProcessorHost.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace WebAudioWorker {

using namespace Web::WebAudio::Render;

struct WebAudioSession::PreparedGraph : public RefCounted<WebAudioSession::PreparedGraph> {
public:
    PreparedGraph(WireGraphBuildResult build_result, OwnPtr<GraphExecutor> graph_executor)
        : build(move(build_result))
        , executor(move(graph_executor))
    {
    }
    ~PreparedGraph() = default;
    WireGraphBuildResult build;
    OwnPtr<GraphExecutor> executor;
};

struct StreamState::AnalyserStreamMap : public AtomicRefCounted<StreamState::AnalyserStreamMap> {
public:
    HashMap<u64, StreamState::Analyser> streams;
};

struct StreamState::DynamicsCompressorStreamMap : public AtomicRefCounted<StreamState::DynamicsCompressorStreamMap> {
public:
    HashMap<u64, StreamState::DynamicsCompressor> streams;
};

struct WebAudioSession::RetiredGraphNode {
    PreparedGraph* graph { nullptr };
    RetiredGraphNode* next { nullptr };
};

WebAudioSession::WebAudioSession(u64 session_id, u32 device_sample_rate_hz, u32 device_channel_count, Core::AnonymousBuffer timing_buffer, int timing_notify_write_fd, int client_id)
    : m_session_id(session_id)
    , m_client_id(client_id)
    , m_device_sample_rate_hz(device_sample_rate_hz)
    , m_device_channel_count(device_channel_count)
    , m_timing_buffer(move(timing_buffer))
    , m_timing_notify_write_fd(timing_notify_write_fd)
{
    m_requested_suspend_state.store(encode_webaudio_suspend_state(true, 0), AK::MemoryOrder::memory_order_release);
    m_worklet.control_event_loop = Core::EventLoop::current_weak();

    // The host is used from the render thread. Initialize it before any path can start the render thread
    // (AudioOutputDevice::when_ready() may invoke the callback synchronously).
    m_script_processor_host = make<SessionScriptProcessorHost>(*this);

    if (m_timing_buffer.is_valid() && m_timing_buffer.size() >= sizeof(WebAudioTimingPage))
        m_timing_page = m_timing_buffer.data<WebAudioTimingPage>();

    if (m_timing_page)
        __builtin_memset(m_timing_page, 0, sizeof(*m_timing_page));

    initialize_render_state();
}

WebAudioSession::~WebAudioSession()
{
    shutdown();
}

void WebAudioSession::render_graph_quantum(ThreadLoopState& state, bool quantum_is_suspended)
{
    if (quantum_is_suspended || !state.current_graph || !state.current_graph->executor)
        return;

    u32 device_sample_rate_hz = m_device_sample_rate_hz;
    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = device_sample_rate_hz;

    ResampleRenderContext ctx {
        .scratch = m_scratch,
        .executor = *state.current_graph->executor,
        .device_channel_count = static_cast<size_t>(m_device_channel_count),
        .context_sample_rate_hz = context_sample_rate_hz,
        .device_sample_rate_hz = device_sample_rate_hz,
    };

    if (ctx.context_sample_rate_hz == ctx.device_sample_rate_hz) {
        render_at_device_sample_rate(ctx);
    } else {
        render_with_resampler(ctx);
    }
}

bool WebAudioSession::render_one_quantum()
{
    ensure_render_thread_scratch_initialized();

    u64 quantum_was_suspended = m_requested_suspend_state.load(AK::MemoryOrder::memory_order_acquire);
    bool const quantum_is_suspended = decode_webaudio_suspend_state_is_suspended(quantum_was_suspended);

    maybe_swap_graph(m_thread_state, m_graph_generation.load(AK::MemoryOrder::memory_order_acquire));
    maybe_log_script_processor_never_ran(m_thread_state);
    service_audio_worklet_host();

    bool const should_render_audio = !quantum_is_suspended;
    bool did_render = false;

    if (should_render_audio) {
        auto device_channel_count = static_cast<size_t>(m_device_channel_count);
        prepare_output_buffers(device_channel_count, RENDER_QUANTUM_SIZE);
        render_graph_quantum(m_thread_state, false);
        publish_analyser_snapshots(m_thread_state);
        publish_dynamics_compressor_snapshots(m_thread_state);
        update_and_maybe_log_output_levels(m_thread_state);
        did_render = m_thread_state.current_graph && m_thread_state.current_graph->executor;
    }

    if (!quantum_is_suspended || !m_thread_state.last_timing_page_was_suspended)
        update_timing_page_and_notify(quantum_was_suspended);
    m_thread_state.last_timing_page_was_suspended = quantum_is_suspended;

    return did_render;
}

ReadonlySpan<f32> WebAudioSession::interleaved_output() const
{
    return m_scratch.interleaved.span();
}

void WebAudioSession::publish_analyser_snapshots(ThreadLoopState const& state) const
{
    if (!state.current_graph || !state.current_graph->executor)
        return;

    StreamState::AnalyserStreamMap* analyser_streams_index = m_streams.analyser_streams.load(AK::MemoryOrder::memory_order_acquire);
    if (!analyser_streams_index)
        return;

    analyser_streams_index->ref();
    if (!analyser_streams_index->streams.is_empty()) {
        auto analyser_count = state.current_graph->executor->analyser_count();
        for (size_t analyser_index = 0; analyser_index < analyser_count; ++analyser_index) {
            auto analyser_node_id = state.current_graph->executor->analyser_node_id(analyser_index);
            auto it = analyser_streams_index->streams.find(analyser_node_id.value());
            if (it == analyser_streams_index->streams.end())
                continue;

            auto& stream_state = it->value;
            if (!stream_state.stream.is_valid())
                continue;
            if (stream_state.fft_size == 0)
                continue;

            auto block_index = stream_state.stream.try_acquire_block_index();
            if (!block_index.has_value())
                continue;

            auto block = stream_state.stream.block_bytes(block_index.value());
            auto expected_used_size = webaudio_analyser_snapshot_size_bytes(stream_state.fft_size);
            if (block.size() < expected_used_size) {
                (void)stream_state.stream.try_release_block_index(block_index.value());
                continue;
            }

            auto* header = reinterpret_cast<WebAudioAnalyserSnapshotHeader*>(block.data());
            header->version = webaudio_analyser_snapshot_version;
            header->fft_size = stream_state.fft_size;
            header->analyser_node_id = analyser_node_id.value();
            header->rendered_frames_total = m_scratch.rendered_frames;

            auto* floats = reinterpret_cast<f32*>(header + 1);
            auto time_domain = Span<f32> { floats, stream_state.fft_size };
            auto frequency_db = Span<f32> { floats + stream_state.fft_size, stream_state.fft_size / 2 };

            if (!state.current_graph->executor->copy_analyser_time_domain_data(analyser_index, time_domain))
                time_domain.fill(0.0f);

            if (!state.current_graph->executor->copy_analyser_frequency_data_db(analyser_index, frequency_db)) {
                for (auto& v : frequency_db)
                    v = -AK::Infinity<f32>;
            }

            if (!stream_state.stream.try_submit_ready_block(block_index.value(), static_cast<u32>(expected_used_size)))
                (void)stream_state.stream.try_release_block_index(block_index.value());
        }
    }
    analyser_streams_index->unref();
}

void WebAudioSession::publish_dynamics_compressor_snapshots(ThreadLoopState const& state) const
{
    if (!state.current_graph || !state.current_graph->executor)
        return;

    StreamState::DynamicsCompressorStreamMap* compressor_streams_index = m_streams.dynamics_compressor_streams.load(AK::MemoryOrder::memory_order_acquire);
    if (!compressor_streams_index)
        return;

    compressor_streams_index->ref();
    if (!compressor_streams_index->streams.is_empty()) {
        for (auto& it : compressor_streams_index->streams) {
            u64 const compressor_node_id = it.key;
            auto& stream_state = it.value;
            if (!stream_state.stream.is_valid())
                continue;

            auto block_index = stream_state.stream.try_acquire_block_index();
            if (!block_index.has_value())
                continue;

            auto block = stream_state.stream.block_bytes(block_index.value());
            auto expected_used_size = webaudio_dynamics_compressor_snapshot_size_bytes();
            if (block.size() < expected_used_size) {
                (void)stream_state.stream.try_release_block_index(block_index.value());
                continue;
            }

            auto* header = reinterpret_cast<WebAudioDynamicsCompressorSnapshotHeader*>(block.data());
            header->version = webaudio_dynamics_compressor_snapshot_version;
            header->compressor_node_id = compressor_node_id;
            header->rendered_frames_total = m_scratch.rendered_frames;

            f32 reduction_db = 0.0f;
            (void)state.current_graph->executor->try_copy_dynamics_compressor_reduction(Web::WebAudio::NodeID { compressor_node_id }, reduction_db);
            header->reduction_db = reduction_db;

            if (!stream_state.stream.try_submit_ready_block(block_index.value(), static_cast<u32>(expected_used_size)))
                (void)stream_state.stream.try_release_block_index(block_index.value());
        }
    }
    compressor_streams_index->unref();
}

void WebAudioSession::update_timing_page_and_notify(u64 quantum_was_suspended)
{
    if (!m_timing_page)
        return;

    auto underruns = m_scratch.underrun_frames.load(AK::MemoryOrder::memory_order_relaxed);
    auto graph_generation = static_cast<u32>(m_graph_generation.load(AK::MemoryOrder::memory_order_relaxed));
    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_relaxed);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;
    write_webaudio_timing_page(*m_timing_page, context_sample_rate_hz, m_device_channel_count, m_scratch.rendered_frames, underruns, graph_generation, quantum_was_suspended);

    if (m_timing_notify_write_fd == -1)
        return;

    u8 byte = 0;
    auto result = Core::System::write(m_timing_notify_write_fd, ReadonlyBytes { &byte, 1 });
    if (result.is_error()) {
        auto const& error = result.error();
        if (error.is_errno() && (error.code() == EAGAIN || error.code() == EWOULDBLOCK)) {
            // Coalesce notifications when the pipe is full.
        } else {
            // Disable notifications if the client went away.
            (void)Core::System::close(m_timing_notify_write_fd);
            m_timing_notify_write_fd = -1;
        }
    }
}

void WebAudioSession::update_and_maybe_log_output_levels(ThreadLoopState& state)
{
    if (!Web::WebAudio::should_log_output_driver())
        return;

    // Lightweight output-level probe: helps confirm we're producing non-zero samples.
    // Logged at most once per second (render-thread).
    for (auto sample : m_scratch.interleaved) {
        auto abs_sample = AK::fabs(sample);
        state.level_peak = max(state.level_peak, abs_sample);
        state.level_sum_squares += static_cast<f64>(sample) * static_cast<f64>(sample);
        ++state.level_sample_count;
    }

    auto const sample_rate = static_cast<u64>(m_device_sample_rate_hz);
    if (sample_rate > 0 && m_scratch.frames_written - state.last_level_log_frame >= sample_rate) {
        auto rms = state.level_sample_count > 0 ? AK::sqrt(state.level_sum_squares / static_cast<f64>(state.level_sample_count)) : 0.0;
        dbgln("cid={}: WebAudio session={} output level peak={:.6f} rms={:.6f}", m_client_id, m_session_id, state.level_peak, rms);
        state.last_level_log_frame = m_scratch.frames_written;
        state.level_sum_squares = 0.0;
        state.level_sample_count = 0;
        state.level_peak = 0.0f;
    }
}

void WebAudioSession::ensure_render_thread_scratch_initialized()
{
    // Render-loop scratch buffers are preallocated on the control thread, but be defensive.
    if (!m_scratch.mix_bus)
        m_scratch.mix_bus = make<AudioBus>(m_device_channel_count, RENDER_QUANTUM_SIZE, m_device_channel_count);
    if (!m_scratch.context_mix_bus)
        m_scratch.context_mix_bus = make<AudioBus>(m_device_channel_count, RENDER_QUANTUM_SIZE, m_device_channel_count);
    if (m_scratch.interleaved.is_empty())
        m_scratch.interleaved.resize(static_cast<size_t>(m_device_channel_count) * RENDER_QUANTUM_SIZE);
    if (m_scratch.planar_spans.is_empty())
        m_scratch.planar_spans.resize(m_device_channel_count);
}

void WebAudioSession::maybe_swap_graph(ThreadLoopState& state, u64 generation)
{
    if (generation == state.last_seen_generation)
        return;

    state.last_seen_generation = generation;

    PreparedGraph* pending_graph_ptr = m_pending_graph.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (pending_graph_ptr) {
        state.current_graph = adopt_ref(*pending_graph_ptr);

        state.current_graph->ref();
        PreparedGraph* retired_graph = m_active_graph.exchange(state.current_graph.ptr(), AK::MemoryOrder::memory_order_acq_rel);
        if (retired_graph)
            retire_graph_on_control_thread(retired_graph);
    } else {
        state.current_graph = nullptr;
        PreparedGraph* retired_graph = m_active_graph.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (retired_graph)
            retire_graph_on_control_thread(retired_graph);
    }

    if (!state.current_graph || !state.current_graph->executor)
        return;

    state.current_graph_has_script_processor = false;
    state.current_graph_has_media_element_source = false;
    state.current_graph_has_worklet_render_nodes = false;
    for (auto const& it : state.current_graph->build.description.nodes) {
        if (graph_node_type(it.value) == GraphNodeType::ScriptProcessor)
            state.current_graph_has_script_processor = true;
        if (graph_node_type(it.value) == GraphNodeType::AudioWorklet)
            state.current_graph_has_worklet_render_nodes = true;
        if (graph_node_type(it.value) == GraphNodeType::MediaElementAudioSource)
            state.current_graph_has_media_element_source = true;
        if (state.current_graph_has_script_processor && state.current_graph_has_media_element_source && state.current_graph_has_worklet_render_nodes)
            break;
    }

    // GraphExecutor::process_context() is render-thread-only.
    // Wire the ScriptProcessor host here so ScriptProcessorRenderNode can call into it.
    state.current_graph->executor->process_context().script_processor_host = m_script_processor_host.ptr();
    state.current_graph->executor->process_context().worklet_processor_host = m_worklet.host_ptr.load(AK::MemoryOrder::memory_order_acquire);

    // Reset output resampler state on graph swaps.
    m_scratch.resampler_initialized = false;
    m_scratch.resample_input_read_index = 0;
    m_scratch.resample_input_available_frames = 0;

    if (Web::WebAudio::should_log_info()) {
        dbgln("cid={}: WebAudio session={} swapped engine graph (nodes={} connections={})",
            m_client_id, m_session_id,
            state.current_graph->build.description.nodes.size(),
            state.current_graph->build.description.connections.size());
    }

    state.graph_swap_output_frame = m_scratch.frames_written;
    state.logged_script_processor_never_ran_for_graph = false;
}

void WebAudioSession::maybe_log_script_processor_never_ran(ThreadLoopState& state)
{
    if (!state.current_graph_has_script_processor || state.logged_script_processor_never_ran_for_graph)
        return;

    u32 sample_rate_hz = m_device_sample_rate_hz;
    if (sample_rate_hz == 0)
        return;

    u64 frames_since_swap = m_scratch.frames_written - state.graph_swap_output_frame;
    u64 processed_blocks = m_script_processor_processed_blocks.load(AK::MemoryOrder::memory_order_acquire);
    if (frames_since_swap >= static_cast<u64>(sample_rate_hz / 2) && processed_blocks == 0) {
        if (Web::WebAudio::should_log_output_driver() || Web::WebAudio::should_log_script_processor_bridge())
            dbgln("cid={}: WebAudio session={} ScriptProcessor has not run after {} frames", m_client_id, m_session_id, frames_since_swap);
        state.logged_script_processor_never_ran_for_graph = true;
    }
}

void WebAudioSession::prepare_output_buffers(size_t device_channel_count, size_t quantum_frames)
{
    if (!m_scratch.mix_bus || m_scratch.mix_bus->channel_capacity() != device_channel_count || m_scratch.mix_bus->frame_count() != quantum_frames)
        m_scratch.mix_bus = make<AudioBus>(device_channel_count, quantum_frames, device_channel_count);

    if (m_scratch.interleaved.size() != quantum_frames * device_channel_count)
        m_scratch.interleaved.resize(quantum_frames * device_channel_count);
    m_scratch.interleaved.fill(0.0f);
}

void WebAudioSession::service_audio_worklet_host()
{
    auto* host = m_worklet.host_ptr.load(AK::MemoryOrder::memory_order_acquire);
    if (!host)
        return;

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    host->service_render_thread_state(m_scratch.rendered_frames, static_cast<float>(context_sample_rate_hz));
}

void WebAudioSession::set_analyser_stream(u64 analyser_node_id, u32 fft_size, Core::SharedBufferStream stream)
{
    if (!stream.is_valid())
        return;

    Threading::MutexLocker locker(m_streams.analyser_streams_mutex);

    auto* new_index = new StreamState::AnalyserStreamMap;
    StreamState::AnalyserStreamMap* old_index = m_streams.analyser_streams.load(AK::MemoryOrder::memory_order_acquire);
    if (old_index) {
        old_index->ref();
        new_index->streams.ensure_capacity(old_index->streams.size() + 1);
        for (auto const& it : old_index->streams)
            new_index->streams.set(it.key, it.value);
        old_index->unref();
    } else {
        new_index->streams.ensure_capacity(1);
    }

    new_index->streams.set(analyser_node_id, StreamState::Analyser { .fft_size = fft_size, .stream = move(stream) });

    StreamState::AnalyserStreamMap* retired = m_streams.analyser_streams.exchange(new_index, AK::MemoryOrder::memory_order_acq_rel);
    if (retired)
        retired->unref();
}

void WebAudioSession::set_dynamics_compressor_stream(u64 compressor_node_id, Core::SharedBufferStream stream)
{
    if (!stream.is_valid())
        return;

    Threading::MutexLocker locker(m_streams.dynamics_compressor_streams_mutex);

    auto* new_index = new StreamState::DynamicsCompressorStreamMap;
    StreamState::DynamicsCompressorStreamMap* old_index = m_streams.dynamics_compressor_streams.load(AK::MemoryOrder::memory_order_acquire);
    if (old_index) {
        old_index->ref();
        new_index->streams.ensure_capacity(old_index->streams.size() + 1);
        for (auto const& it : old_index->streams)
            new_index->streams.set(it.key, it.value);
        old_index->unref();
    } else {
        new_index->streams.ensure_capacity(1);
    }

    new_index->streams.set(compressor_node_id, StreamState::DynamicsCompressor { .stream = move(stream) });

    StreamState::DynamicsCompressorStreamMap* retired = m_streams.dynamics_compressor_streams.exchange(new_index, AK::MemoryOrder::memory_order_acq_rel);
    if (retired)
        retired->unref();
}

void WebAudioSession::add_worklet_module(u64 module_id, ByteString url, ByteString source_text)
{
    if (Web::WebAudio::should_log_info())
        dbgln("cid={}: WebAudio session={} received worklet module id={} '{}' ({} bytes)", m_client_id, m_session_id, module_id, url, source_text.length());

    bool had_worklet_host = false;
    {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        had_worklet_host = m_worklet.host != nullptr;
    }

    m_worklet.modules.append(WorkletModule {
        .module_id = module_id,
        .url = move(url),
        .source_text = move(source_text),
    });

    if (had_worklet_host) {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        if (m_worklet.host)
            m_worklet.host->enqueue_worklet_module(m_worklet.modules.last());
    } else {
        ensure_worklet_host();
    }
}

void WebAudioSession::ensure_worklet_host()
{
    Threading::MutexLocker locker(m_worklet.host_mutex);
    if (m_worklet.host)
        return;

    Vector<WorkletModule> modules;
    modules.ensure_capacity(m_worklet.modules.size());
    for (auto const& module : m_worklet.modules)
        modules.append(module);

    Vector<WorkletNodeDefinition> node_definitions;
    {
        Threading::MutexLocker defs_locker(m_worklet.definitions_mutex);
        node_definitions = m_worklet.node_definitions;
    }

    Vector<WorkletPortBinding> port_bindings;
    {
        Threading::MutexLocker ports_locker(m_worklet.ports_mutex);
        port_bindings.ensure_capacity(m_worklet.processor_port_fds.size());
        for (auto const& it : m_worklet.processor_port_fds) {
            if (it.value < 0)
                continue;
            auto dup_fd_or_error = Core::System::dup(it.value);
            if (dup_fd_or_error.is_error())
                continue;
            port_bindings.append(WorkletPortBinding {
                .node_id = Web::WebAudio::NodeID { it.key },
                .processor_port_fd = dup_fd_or_error.release_value(),
            });
        }
    }

    // Even without AudioWorklet nodes, we still need a host to evaluate modules and service the
    // AudioWorkletGlobalScope's shared port.
    if (node_definitions.is_empty() && port_bindings.is_empty() && modules.is_empty())
        return;

    if (Web::WebAudio::should_log_info()) {
        dbgln("cid={}: WebAudio session={} creating worklet host (modules={} node_definitions={} port_bindings={})",
            m_client_id,
            m_session_id,
            modules.size(),
            node_definitions.size(),
            port_bindings.size());
        for (auto const& binding : port_bindings)
            dbgln("cid={}: WebAudio session={} worklet host port binding node_id={} fd={}", m_client_id, m_session_id, binding.node_id.value(), binding.processor_port_fd);
    }

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    m_worklet.host = make<RealtimeAudioWorkletProcessorHost>(m_scratch.rendered_frames, static_cast<float>(context_sample_rate_hz), move(modules), move(node_definitions), move(port_bindings));
    m_worklet.host->set_processor_error_callback([weak_self = make_weak_ptr()](Web::WebAudio::NodeID node_id) {
        if (auto self = weak_self.strong_ref())
            self->notify_worklet_processor_error_from_render_thread(node_id);
    });
    m_worklet.host->set_processor_registration_callback([weak_self = make_weak_ptr()](String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        if (auto self = weak_self.strong_ref())
            self->notify_worklet_processor_registered_from_render_thread(name, descriptors, generation);
    });
    m_worklet.host->set_worklet_module_evaluation_callback([weak_self = make_weak_ptr()](u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations) {
        if (auto self = weak_self.strong_ref())
            self->notify_worklet_module_evaluated_from_render_thread(module_id, required_generation, success, error_name, error_message, move(failed_processor_registrations));
    });
    m_worklet.host_ptr.store(m_worklet.host.ptr(), AK::MemoryOrder::memory_order_release);
}

void WebAudioSession::set_worklet_processor_error_callback(Function<void(Web::WebAudio::NodeID)> callback)
{
    m_worklet.processor_error_callback = move(callback);
}

void WebAudioSession::set_worklet_processor_registration_callback(Function<void(String const&, Vector<Web::WebAudio::AudioParamDescriptor> const&, u64)> callback)
{
    m_worklet.processor_registration_callback = move(callback);
}

void WebAudioSession::set_worklet_module_evaluation_callback(Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)> callback)
{
    m_worklet.module_evaluation_callback = move(callback);
}

void WebAudioSession::notify_worklet_processor_error_from_render_thread(Web::WebAudio::NodeID node_id)
{
    if (!m_worklet.control_event_loop)
        return;

    (void)m_worklet.error_queue.try_push(node_id);

    bool expected = false;
    if (!m_worklet.error_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.error_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_processor_errors();
    });
}

void WebAudioSession::flush_worklet_processor_errors()
{
    m_worklet.error_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
    if (!m_worklet.processor_error_callback)
        return;

    Web::WebAudio::NodeID node_id { 0 };
    while (m_worklet.error_queue.try_pop(node_id))
        m_worklet.processor_error_callback(node_id);

    if (m_worklet.error_queue.is_empty())
        return;

    bool expected = false;
    if (!m_worklet.error_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    if (!m_worklet.control_event_loop) {
        m_worklet.error_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.error_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_processor_errors();
    });
}

void WebAudioSession::notify_worklet_processor_registered_from_render_thread(String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    if (!m_worklet.control_event_loop)
        return;

    (void)m_worklet.registration_queue.try_push(WorkletState::ProcessorRegistration { name, descriptors, generation });

    bool expected = false;
    if (!m_worklet.registration_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.registration_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_processor_registrations();
    });
}

void WebAudioSession::notify_worklet_module_evaluated_from_render_thread(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)
{
    if (!m_worklet.control_event_loop)
        return;

    (void)m_worklet.module_evaluation_queue.try_push(WorkletState::ModuleEvaluation { module_id, required_generation, success, error_name, error_message, move(failed_processor_registrations) });

    bool expected = false;
    if (!m_worklet.module_evaluation_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.module_evaluation_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_module_evaluations();
    });
}

void WebAudioSession::flush_worklet_module_evaluations()
{
    m_worklet.module_evaluation_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
    if (!m_worklet.module_evaluation_callback)
        return;

    WorkletState::ModuleEvaluation evaluation;
    while (m_worklet.module_evaluation_queue.try_pop(evaluation))
        m_worklet.module_evaluation_callback(evaluation.module_id, evaluation.required_generation, evaluation.success, evaluation.error_name, evaluation.error_message, move(evaluation.failed_processor_registrations));

    if (m_worklet.module_evaluation_queue.is_empty())
        return;

    bool expected = false;
    if (!m_worklet.module_evaluation_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    if (!m_worklet.control_event_loop) {
        m_worklet.module_evaluation_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.module_evaluation_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_module_evaluations();
    });
}

void WebAudioSession::flush_worklet_processor_registrations()
{
    m_worklet.registration_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
    if (!m_worklet.processor_registration_callback)
        return;

    WorkletState::ProcessorRegistration registration;
    while (m_worklet.registration_queue.try_pop(registration))
        m_worklet.processor_registration_callback(registration.name, registration.descriptors, registration.generation);

    if (m_worklet.registration_queue.is_empty())
        return;

    bool expected = false;
    if (!m_worklet.registration_task_scheduled.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    if (!m_worklet.control_event_loop) {
        m_worklet.registration_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_worklet.registration_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->flush_worklet_processor_registrations();
    });
}

void WebAudioSession::set_render_graph(WireGraphBuildResult graph)
{
    if (!Web::WebAudio::current_thread_is_control_thread()) {
        {
            Threading::MutexLocker locker(m_graph_mutex);
            m_deferred_graph = move(graph);
        }

        if (m_worklet.control_event_loop) {
            if (auto strong_loop = m_worklet.control_event_loop->take()) {
                strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
                    if (auto self = weak_self.strong_ref())
                        self->apply_deferred_graph_if_any();
                });
            }
        }
        return;
    }

    apply_render_graph(move(graph));
}

void WebAudioSession::set_media_element_audio_source_streams(Vector<MediaElementAudioSourceStreamDescriptor>&& streams)
{
    // Called on the control thread.
    if (Web::WebAudio::should_log_media_element_bridge())
        dbgln("cid={}: WebAudio session received {} media element stream binding(s)", m_client_id, streams.size());

    struct ProviderBinding {
        u64 provider_id { 0 };
        NonnullRefPtr<Web::WebAudio::MediaElementAudioSourceProvider> provider;
    };

    HashMap<u64, StreamState::MediaElement> new_streams;
    Vector<ProviderBinding> new_bindings;

    for (size_t i = 0; i < streams.size(); ++i) {
        auto binding = move(streams[i]);
        if (binding.provider_id == 0)
            continue;

        if (Web::WebAudio::should_log_media_element_bridge())
            dbgln("cid={}: WebAudio session bind provider {}", m_client_id, binding.provider_id);

        RingStreamDescriptor descriptor = move(binding.ring_stream);

        auto view_or_error = validate_ring_stream_descriptor(descriptor);
        if (view_or_error.is_error()) {
            warnln("cid={}: WebAudio session rejected media element stream for provider {}: {}", m_client_id, binding.provider_id, view_or_error.error());
            continue;
        }

        auto view = view_or_error.release_value();
        if (Web::WebAudio::should_log_media_element_bridge()) {
            auto& header = *view.header;
            u64 read_frame = ring_stream_load_read_frame(header);
            u64 write_frame = ring_stream_load_write_frame(header);
            u64 flags = ring_stream_load_flags(header);
            u32 timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
            u64 timeline_generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
            dbgln("cid={}: WebAudio session bind provider {} stream: sr={}Hz ch={} cap_ch={} cap_frames={} read={} write={} flags=0x{:x} timeline_gen={} timeline_sr={}",
                m_client_id,
                binding.provider_id,
                header.sample_rate_hz,
                header.channel_count,
                header.channel_capacity,
                header.capacity_frames,
                read_frame,
                write_frame,
                flags,
                timeline_generation,
                timeline_sample_rate);
        }
        int notify_read_fd = descriptor.notify_fd.take_fd();
        if (Web::WebAudio::should_log_media_element_bridge())
            dbgln("cid={}: WebAudio session bind provider {} notify_read_fd={}", m_client_id, binding.provider_id, notify_read_fd);
        auto provider = Web::WebAudio::MediaElementAudioSourceProvider::create_for_remote_consumer(binding.provider_id, view, move(descriptor.shared_memory), notify_read_fd);
        provider->set_debug_connection_info(m_client_id, m_session_id);

        new_bindings.append(ProviderBinding {
            .provider_id = binding.provider_id,
            .provider = provider,
        });

        new_streams.set(binding.provider_id, StreamState::MediaElement {
                                                 .provider = move(provider),
                                             });
    }

    {
        Threading::MutexLocker locker(m_streams.media_element_streams_mutex);
        m_streams.media_element_streams = move(new_streams);
    }

    u32 device_sample_rate_hz = m_device_sample_rate_hz;
    if (device_sample_rate_hz == 0)
        return;

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = device_sample_rate_hz;

    RefPtr<PreparedGraph> base_graph;
    {
        PreparedGraph* pending_graph = m_pending_graph.load(AK::MemoryOrder::memory_order_acquire);
        if (pending_graph) {
            pending_graph->ref();
            base_graph = adopt_ref(*pending_graph);
        } else {
            PreparedGraph* active_graph = m_active_graph.load(AK::MemoryOrder::memory_order_acquire);
            if (active_graph) {
                active_graph->ref();
                base_graph = adopt_ref(*active_graph);
            }
        }
    }

    if (!base_graph || !base_graph->executor)
        return;

    // MediaElementAudioSource provider resolution happens at compile time (see GraphCompiler).
    // If bindings change without a graph update message, we need to rebuild the executor so
    // MediaElementSource nodes stop being compiled as OhNoesRenderNode.
    WireGraphBuildResult build {
        .description = base_graph->build.description,
        .resources = base_graph->build.resources->clone(),
    };
    build.resources->clear_media_element_audio_sources();
    for (auto const& b : new_bindings)
        build.resources->set_media_element_audio_source(b.provider_id, b.provider);
    build.resources->clear_media_stream_audio_sources();
    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        for (auto const& it : m_streams.media_stream_streams)
            build.resources->set_media_stream_audio_source(it.key, it.value.provider);
    }
    build.resources->set_script_processor_host(m_script_processor_host.ptr());

    auto executor = make<GraphExecutor>(
        build.description,
        static_cast<f32>(context_sample_rate_hz),
        static_cast<size_t>(RENDER_QUANTUM_SIZE),
        build.resources.ptr());

    ensure_worklet_host();

    RefPtr<PreparedGraph> prepared_graph = adopt_ref(*new PreparedGraph(move(build), move(executor)));
    prepared_graph->ref();
    PreparedGraph* retired_graph = m_pending_graph.exchange(prepared_graph.ptr(), AK::MemoryOrder::memory_order_acq_rel);
    if (retired_graph)
        retired_graph->unref();
    m_graph_generation.fetch_add(1, AK::MemoryOrder::memory_order_release);
}

static bool media_stream_metadata_matches(Web::WebAudio::Render::AudioInputStreamMetadata const& a, Web::WebAudio::Render::AudioInputStreamMetadata const& b)
{
    return a.device_id == b.device_id
        && a.sample_rate_hz == b.sample_rate_hz
        && a.channel_count == b.channel_count
        && a.capacity_frames == b.capacity_frames
        && a.overflow_policy == b.overflow_policy;
}

void WebAudioSession::set_media_stream_audio_source_streams(Vector<MediaStreamAudioSourceStreamDescriptor> const& streams)
{
    // Called on the control thread.
    if (Web::WebAudio::should_log_media_element_bridge())
        dbgln("cid={}: WebAudio session received {} media stream source binding(s)", m_client_id, streams.size());

    HashMap<u64, StreamState::MediaStream> old_streams;
    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        old_streams = move(m_streams.media_stream_streams);
    }

    HashMap<u64, StreamState::MediaStream> new_streams;
    bool bindings_changed = false;

    for (auto const& binding : streams) {
        if (binding.provider_id == 0)
            continue;

        auto const& metadata = binding.metadata;
        if (metadata.device_id == 0)
            continue;

        auto existing = old_streams.find(binding.provider_id);
        if (existing != old_streams.end() && media_stream_metadata_matches(existing->value.metadata, metadata)) {
            new_streams.set(binding.provider_id, move(existing->value));
            old_streams.remove(existing);
            continue;
        }

        if (existing != old_streams.end()) {
            destroy_audio_input_stream(existing->value.stream_id);
            old_streams.remove(existing);
            bindings_changed = true;
        }

        auto stream_id = create_audio_input_stream(metadata.device_id, metadata.sample_rate_hz, metadata.channel_count, metadata.capacity_frames, metadata.overflow_policy);
        if (stream_id == 0)
            continue;

        auto descriptor_it = m_audio_input_streams.find(stream_id);
        if (descriptor_it == m_audio_input_streams.end()) {
            destroy_audio_input_stream(stream_id);
            continue;
        }

        AudioServer::AudioInputStreamDescriptor descriptor = move(descriptor_it->value);
        descriptor_it->value = {};

        RingStreamDescriptor ring_descriptor;
        ring_descriptor.stream_id = descriptor.stream_id;
        ring_descriptor.format.sample_rate_hz = descriptor.format.sample_rate_hz;
        ring_descriptor.format.channel_count = descriptor.format.channel_count;
        ring_descriptor.format.channel_capacity = descriptor.format.channel_capacity;
        ring_descriptor.format.capacity_frames = descriptor.format.capacity_frames;
        ring_descriptor.overflow_policy = static_cast<StreamOverflowPolicy>(descriptor.overflow_policy);
        ring_descriptor.shared_memory = descriptor.shared_memory;

        auto view_or_error = validate_ring_stream_descriptor(ring_descriptor);
        if (view_or_error.is_error()) {
            warnln("cid={}: WebAudio session rejected media stream source for provider {}: {}", m_client_id, binding.provider_id, view_or_error.error());
            destroy_audio_input_stream(stream_id);
            continue;
        }

        int notify_read_fd = descriptor.notify_fd.take_fd();
        if (Web::WebAudio::should_log_media_element_bridge())
            dbgln("cid={}: WebAudio session bind media stream provider {} stream_id={} notify_read_fd={}", m_client_id, binding.provider_id, stream_id, notify_read_fd);

        auto provider = Web::WebAudio::MediaElementAudioSourceProvider::create_for_remote_consumer(binding.provider_id, view_or_error.release_value(), move(ring_descriptor.shared_memory), notify_read_fd);
        provider->set_debug_connection_info(m_client_id, m_session_id);

        new_streams.set(binding.provider_id, StreamState::MediaStream {
                                                 .metadata = metadata,
                                                 .stream_id = stream_id,
                                                 .provider = move(provider),
                                             });
        bindings_changed = true;
    }

    for (auto const& it : old_streams) {
        destroy_audio_input_stream(it.value.stream_id);
        bindings_changed = true;
    }

    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        m_streams.media_stream_streams = move(new_streams);
    }

    if (!bindings_changed)
        return;

    u32 device_sample_rate_hz = m_device_sample_rate_hz;
    if (device_sample_rate_hz == 0)
        return;

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = device_sample_rate_hz;

    RefPtr<PreparedGraph> base_graph;
    {
        PreparedGraph* pending_graph = m_pending_graph.load(AK::MemoryOrder::memory_order_acquire);
        if (pending_graph) {
            pending_graph->ref();
            base_graph = adopt_ref(*pending_graph);
        } else {
            PreparedGraph* active_graph = m_active_graph.load(AK::MemoryOrder::memory_order_acquire);
            if (active_graph) {
                active_graph->ref();
                base_graph = adopt_ref(*active_graph);
            }
        }
    }

    if (!base_graph || !base_graph->executor)
        return;

    WireGraphBuildResult build {
        .description = base_graph->build.description,
        .resources = base_graph->build.resources->clone(),
    };
    build.resources->clear_media_element_audio_sources();
    {
        Threading::MutexLocker locker(m_streams.media_element_streams_mutex);
        for (auto const& it : m_streams.media_element_streams)
            build.resources->set_media_element_audio_source(it.key, it.value.provider);
    }

    build.resources->clear_media_stream_audio_sources();
    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        for (auto const& it : m_streams.media_stream_streams)
            build.resources->set_media_stream_audio_source(it.key, it.value.provider);
    }

    build.resources->set_script_processor_host(m_script_processor_host.ptr());

    auto executor = make<GraphExecutor>(
        build.description,
        static_cast<f32>(context_sample_rate_hz),
        static_cast<size_t>(RENDER_QUANTUM_SIZE),
        build.resources.ptr());

    ensure_worklet_host();

    RefPtr<PreparedGraph> prepared_graph = adopt_ref(*new PreparedGraph(move(build), move(executor)));
    prepared_graph->ref();
    PreparedGraph* retired_graph = m_pending_graph.exchange(prepared_graph.ptr(), AK::MemoryOrder::memory_order_acq_rel);
    if (retired_graph)
        retired_graph->unref();
    m_graph_generation.fetch_add(1, AK::MemoryOrder::memory_order_release);
}

void WebAudioSession::apply_render_graph(WireGraphBuildResult graph)
{
    ASSERT_CONTROL_THREAD();
    u32 sample_rate_hz = m_device_sample_rate_hz;
    u32 channel_count = m_device_channel_count;
    if (sample_rate_hz == 0 || channel_count == 0)
        return;

    // The incoming wire graph is authored at the WebAudio context's sample rate.
    // This may differ from the output device sample rate.
    u32 context_sample_rate_hz = AK::round_to<u32>(graph.context_sample_rate_hz);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = sample_rate_hz;
    m_context_sample_rate_hz.store(context_sample_rate_hz, AK::MemoryOrder::memory_order_release);

    // Preparing a new executor can allocate and should happen on the control thread.
    graph.description.normalize();

    // Ensure the destination node's channel count matches the output device.
    Web::WebAudio::NodeID destination_id = graph.description.destination_node_id;
    if (auto it = graph.description.nodes.find(destination_id); it != graph.description.nodes.end()) {
        auto& node = it->value;
        if (node.has<DestinationGraphNode>())
            node.get<DestinationGraphNode>().channel_count = channel_count;
    }

    RefPtr<PreparedGraph> active_graph;
    {
        PreparedGraph* active_graph_ptr = m_active_graph.load(AK::MemoryOrder::memory_order_acquire);
        if (active_graph_ptr) {
            active_graph_ptr->ref();
            active_graph = adopt_ref(*active_graph_ptr);
        }
    }

    if (active_graph && active_graph->executor) {
        active_graph->build.resources->set_script_processor_host(m_script_processor_host.ptr());

        // Keep the active graph's external providers up to date.
        {
            Threading::MutexLocker locker(m_streams.media_element_streams_mutex);
            if (!m_streams.media_element_streams.is_empty()) {
                for (auto const& it : m_streams.media_element_streams)
                    active_graph->build.resources->set_media_element_audio_source(it.key, it.value.provider);
            }
        }
        {
            Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
            if (!m_streams.media_stream_streams.is_empty()) {
                for (auto const& it : m_streams.media_stream_streams)
                    active_graph->build.resources->set_media_stream_audio_source(it.key, it.value.provider);
            }
        }

        GraphUpdateKind update_kind = active_graph->executor->classify_update(graph.description);
        bool applied_incrementally = false;
        switch (update_kind) {
        case GraphUpdateKind::None:
            applied_incrementally = true;
            break;
        case GraphUpdateKind::Parameter:
            applied_incrementally = active_graph->executor->enqueue_parameter_update(graph.description);
            break;
        case GraphUpdateKind::Topology:
            applied_incrementally = active_graph->executor->enqueue_topology_update(graph.description);
            break;
        case GraphUpdateKind::RebuildRequired:
            applied_incrementally = false;
            break;
        }

        if (Web::WebAudio::should_log_info()) {
            dbgln("cid={}: WebAudio session {} update kind={} incremental={} nodes={} conns={} pconns={} automation={}",
                m_client_id,
                m_session_id,
                static_cast<u32>(update_kind),
                applied_incrementally,
                graph.description.nodes.size(),
                graph.description.connections.size(),
                graph.description.param_connections.size(),
                graph.description.param_automations.size());

            if (graph.description.nodes.size() <= 8) {
                for (auto const& it : graph.description.nodes) {
                    auto const& node = it.value;
                    if (node.has<ScriptProcessorGraphNode>()) {
                        auto const& sp = node.get<ScriptProcessorGraphNode>();
                        dbgln("cid={}: WebAudio session {} node id={} type={} bs={} in_ch={} out_ch={}",
                            m_client_id,
                            m_session_id,
                            it.key.value(),
                            graph_node_type_name(GraphNodeType::ScriptProcessor),
                            sp.buffer_size,
                            sp.input_channel_count,
                            sp.output_channel_count);
                        continue;
                    }

                    if (node.has<MediaElementAudioSourceGraphNode>()) {
                        auto const& source = node.get<MediaElementAudioSourceGraphNode>();
                        dbgln("cid={}: WebAudio session {} node id={} type={} provider_id={} ch={}",
                            m_client_id,
                            m_session_id,
                            it.key.value(),
                            graph_node_type_name(GraphNodeType::MediaElementAudioSource),
                            source.provider_id,
                            source.channel_count);
                        continue;
                    }

                    if (node.has<MediaStreamAudioSourceGraphNode>()) {
                        auto const& source = node.get<MediaStreamAudioSourceGraphNode>();
                        dbgln("cid={}: WebAudio session {} node id={} type={} provider_id={}",
                            m_client_id,
                            m_session_id,
                            it.key.value(),
                            graph_node_type_name(GraphNodeType::MediaStreamAudioSource),
                            source.provider_id);
                        continue;
                    }

                    if (node.has<DestinationGraphNode>()) {
                        auto const& dest = node.get<DestinationGraphNode>();
                        dbgln("cid={}: WebAudio session {} node id={} type={} ch={}",
                            m_client_id,
                            m_session_id,
                            it.key.value(),
                            graph_node_type_name(GraphNodeType::Destination),
                            dest.channel_count);
                        continue;
                    }

                    dbgln("cid={}: WebAudio session {} node id={} type={}",
                        m_client_id,
                        m_session_id,
                        it.key.value(),
                        graph_node_type_name(graph_node_type(node)));
                }

                if (graph.description.connections.size() <= 16) {
                    for (auto const& connection : graph.description.connections) {
                        dbgln("cid={}: WebAudio session {} conn {}:{} -> {}:{}",
                            m_client_id,
                            m_session_id,
                            connection.source.value(),
                            connection.source_output_index,
                            connection.destination.value(),
                            connection.destination_input_index);
                    }
                }
            }
        }

        if (applied_incrementally) {
            // Prevent update retirement slots from filling up if graph updates are frequent.
            active_graph->executor->collect_retired_updates();
            return;
        }
    }

    {
        Threading::MutexLocker locker(m_streams.media_element_streams_mutex);
        if (!m_streams.media_element_streams.is_empty()) {
            for (auto const& it : m_streams.media_element_streams)
                graph.resources->set_media_element_audio_source(it.key, it.value.provider);
        }
    }
    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        if (!m_streams.media_stream_streams.is_empty()) {
            for (auto const& it : m_streams.media_stream_streams)
                graph.resources->set_media_stream_audio_source(it.key, it.value.provider);
        }
    }

    graph.resources->set_script_processor_host(m_script_processor_host.ptr());

    auto executor = make<GraphExecutor>(
        graph.description,
        static_cast<f32>(context_sample_rate_hz),
        static_cast<size_t>(RENDER_QUANTUM_SIZE),
        graph.resources.ptr());

    ensure_worklet_host();

    RefPtr<PreparedGraph> prepared_graph = adopt_ref(*new PreparedGraph(move(graph), move(executor)));
    prepared_graph->ref();
    PreparedGraph* retired_graph = m_pending_graph.exchange(prepared_graph.ptr(), AK::MemoryOrder::memory_order_acq_rel);
    if (retired_graph)
        retired_graph->unref();
    m_graph_generation.fetch_add(1, AK::MemoryOrder::memory_order_release);
}

void WebAudioSession::retire_graph_on_control_thread(PreparedGraph* graph)
{
    ASSERT_RENDER_THREAD();
    if (!graph)
        return;

    auto* node = new (nothrow) RetiredGraphNode { .graph = graph, .next = nullptr };
    if (!node) {
        graph->unref();
        return;
    }

    auto* expected_node = m_retired_graphs.load(AK::MemoryOrder::memory_order_acquire);
    do {
        node->next = expected_node;
    } while (!m_retired_graphs.compare_exchange_strong(expected_node, node, AK::MemoryOrder::memory_order_release));

    bool expected_scheduled = false;
    if (!m_retired_graph_task_scheduled.compare_exchange_strong(expected_scheduled, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    if (!m_worklet.control_event_loop) {
        m_retired_graph_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    auto strong_loop = m_worklet.control_event_loop->take();
    if (!strong_loop) {
        m_retired_graph_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    strong_loop->deferred_invoke([weak_self = make_weak_ptr()] {
        if (auto self = weak_self.strong_ref())
            self->drain_retired_graphs_on_control_thread();
    });
}

void WebAudioSession::drain_retired_graphs_on_control_thread()
{
    ASSERT_CONTROL_THREAD();
    m_retired_graph_task_scheduled.store(false, AK::MemoryOrder::memory_order_release);

    size_t retired_count = 0;
    auto* node = m_retired_graphs.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    while (node) {
        auto* next = node->next;
        if (node->graph)
            node->graph->unref();
        delete node;
        node = next;
        ++retired_count;
    }

    if (retired_count > 0 && Web::WebAudio::should_log_info()) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 1000 && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
            WA_DBGLN("[WebAudio] WebAudioSession retired {} graph(s) on control thread", retired_count);
    }
}

void WebAudioSession::apply_deferred_graph_if_any()
{
    ASSERT_CONTROL_THREAD();
    Optional<WireGraphBuildResult> deferred_graph;
    {
        Threading::MutexLocker locker(m_graph_mutex);
        deferred_graph = move(m_deferred_graph);
    }
    if (deferred_graph.has_value())
        apply_render_graph(move(*deferred_graph));
}

void WebAudioSession::set_script_processor_streams(Vector<ScriptProcessorStreamDescriptor> streams)
{
    // Called on the control thread.
    if (Web::WebAudio::should_log_script_processor_bridge())
        dbgln("cid={}: WebAudio session received {} ScriptProcessor stream binding(s) for session={}", m_client_id, streams.size(), m_session_id);

    auto* new_index = new StreamState::ScriptProcessorStreamMap;
    new_index->streams.ensure_capacity(streams.size());

    for (auto& stream : streams) {
        if (stream.node_id == 0)
            continue;

        auto request_pool = stream.request_stream.pool_buffer;
        auto request_ready = stream.request_stream.ready_ring_buffer;
        auto request_free = stream.request_stream.free_ring_buffer;
        auto request_stream_or_error = Core::SharedBufferStream::attach(move(request_pool), move(request_ready), move(request_free));
        if (request_stream_or_error.is_error()) {
            if (Web::WebAudio::should_log_script_processor_bridge())
                dbgln("cid={}: WebAudio session={} failed to attach ScriptProcessor request stream node={} error={}", m_client_id, m_session_id, stream.node_id, request_stream_or_error.error());
            continue;
        }

        auto response_pool = stream.response_stream.pool_buffer;
        auto response_ready = stream.response_stream.ready_ring_buffer;
        auto response_free = stream.response_stream.free_ring_buffer;
        auto response_stream_or_error = Core::SharedBufferStream::attach(move(response_pool), move(response_ready), move(response_free));
        if (response_stream_or_error.is_error()) {
            if (Web::WebAudio::should_log_script_processor_bridge())
                dbgln("cid={}: WebAudio session={} failed to attach ScriptProcessor response stream node={} error={}", m_client_id, m_session_id, stream.node_id, response_stream_or_error.error());
            continue;
        }

        if (Web::WebAudio::should_log_script_processor_bridge()) {
            dbgln("cid={}: WebAudio session={} attached ScriptProcessor streams node={} req(blocks={}, block_size={}) resp(blocks={}, block_size={})",
                m_client_id,
                m_session_id,
                stream.node_id,
                request_stream_or_error.value().block_count(),
                request_stream_or_error.value().block_size(),
                response_stream_or_error.value().block_count(),
                response_stream_or_error.value().block_size());
        }

        new_index->streams.set(stream.node_id, StreamState::ScriptProcessorStreamMap::StreamState {
                                                   .descriptor = move(stream),
                                                   .request_stream = request_stream_or_error.release_value(),
                                                   .response_stream = response_stream_or_error.release_value(),
                                               });
    }

    {
        Threading::MutexLocker locker(m_streams.script_processor_streams_mutex);
        StreamState::ScriptProcessorStreamMap* old_index = m_streams.script_processor_streams.exchange(new_index, AK::MemoryOrder::memory_order_acq_rel);
        if (old_index)
            old_index->unref();
    }
}

void WebAudioSession::set_worklet_node_ports(Vector<WorkletNodePortDescriptor> const& ports)
{
    if (Web::WebAudio::should_log_info())
        dbgln("cid={}: WebAudio session={} received {} worklet port binding(s)", m_client_id, m_session_id, ports.size());

    bool had_worklet_host = false;
    {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        had_worklet_host = m_worklet.host != nullptr;
    }

    Vector<WorkletPortBinding> host_port_bindings;
    if (had_worklet_host)
        host_port_bindings.ensure_capacity(ports.size());

    HashMap<u64, int> new_fds;
    new_fds.ensure_capacity(ports.size());

    for (auto const& port : ports) {
        int fd = port.processor_port_fd.fd();
        if (fd < 0)
            continue;

        auto dup_fd_or_error = Core::System::dup(fd);
        if (dup_fd_or_error.is_error())
            continue;
        int owned_fd = dup_fd_or_error.release_value();

        if (had_worklet_host) {
            auto host_dup_fd_or_error = Core::System::dup(fd);
            if (!host_dup_fd_or_error.is_error()) {
                host_port_bindings.append(Web::WebAudio::Render::WorkletPortBinding {
                    .node_id = Web::WebAudio::NodeID { port.node_id },
                    .processor_port_fd = host_dup_fd_or_error.release_value(),
                });
            }
        }

        if (Web::WebAudio::should_log_info())
            dbgln("cid={}: WebAudio session={} bind worklet port node_id={} fd={}", m_client_id, m_session_id, port.node_id, owned_fd);
        new_fds.set(port.node_id, owned_fd);
    }

    {
        Threading::MutexLocker locker(m_worklet.ports_mutex);
        for (auto const& it : m_worklet.processor_port_fds) {
            if (it.value >= 0)
                (void)Core::System::close(it.value);
        }
        m_worklet.processor_port_fds = move(new_fds);
    }

    ensure_worklet_host();

    if (had_worklet_host) {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        if (m_worklet.host)
            m_worklet.host->enqueue_port_bindings(host_port_bindings);
    }
}

void WebAudioSession::set_worklet_node_definitions(Vector<WorkletNodeDefinition> const& definitions)
{
    if (Web::WebAudio::should_log_info())
        dbgln("cid={}: WebAudio session={} received {} worklet node definition(s)", m_client_id, m_session_id, definitions.size());

    bool had_worklet_host = false;
    {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        had_worklet_host = m_worklet.host != nullptr;
    }

    Vector<WorkletNodeDefinition> definitions_for_host = definitions;
    {
        Threading::MutexLocker locker(m_worklet.definitions_mutex);
        m_worklet.node_definitions = definitions;
    }

    ensure_worklet_host();

    if (m_worklet.host)
        m_worklet.host->synchronize_node_definitions(definitions);

    if (had_worklet_host) {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        if (m_worklet.host)
            m_worklet.host->enqueue_node_definitions(move(definitions_for_host));
    }
}

AudioServer::AudioInputStreamID WebAudioSession::create_audio_input_stream(AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy)
{
    if (device_id == 0)
        return 0;

    auto client = AudioServerClient::Client::default_client();
    if (!client)
        return 0;

    AudioServer::StreamOverflowPolicy policy = AudioServer::StreamOverflowPolicy::DropOldest;
    if (overflow_policy <= static_cast<u8>(AudioServer::StreamOverflowPolicy::Lossless))
        policy = static_cast<AudioServer::StreamOverflowPolicy>(overflow_policy);

    auto descriptor_or_error = client->create_audio_input_stream(device_id, sample_rate_hz, channel_count, capacity_frames, policy);
    if (descriptor_or_error.is_error())
        return 0;

    auto descriptor = descriptor_or_error.release_value();
    if (descriptor.stream_id == 0)
        return 0;

    m_audio_input_streams.set(descriptor.stream_id, move(descriptor));
    return descriptor.stream_id;
}

void WebAudioSession::destroy_audio_input_stream(AudioServer::AudioInputStreamID stream_id)
{
    if (stream_id == 0)
        return;

    m_audio_input_streams.remove(stream_id);

    if (auto client = AudioServerClient::Client::default_client(); client)
        (void)client->destroy_audio_input_stream(stream_id);
}

void WebAudioSession::set_suspended(bool suspended, u64 generation)
{
    // This is a low-frequency control-plane request, safe to store atomically.
    // The render thread will apply it at a quantum boundary.
    m_requested_suspend_state.store(encode_webaudio_suspend_state(suspended, generation), AK::MemoryOrder::memory_order_release);
}

void WebAudioSession::initialize_render_state()
{
    u32 sample_rate_hz = m_device_sample_rate_hz;
    static constexpr u32 max_supported_output_channels = 32;
    u32 channel_count = m_device_channel_count;
    if (channel_count > max_supported_output_channels) {
        warnln("cid={}: WebAudio session clamping output channels {} -> {}", m_client_id, channel_count, max_supported_output_channels);
        channel_count = max_supported_output_channels;
    }
    if (sample_rate_hz == 0 || channel_count == 0) {
        warnln("cid={}: WebAudio session invalid sample specification {} Hz, {} channels", m_client_id, sample_rate_hz, channel_count);
        return;
    }

    m_device_sample_rate_hz = sample_rate_hz;
    m_device_channel_count = channel_count;

    m_scratch.bytes_per_frame = channel_count * sizeof(float);

    // Preallocate output resampler staging buffers and scratch spans. These are used by the render
    // thread when the WebAudio graph sample rate differs from the device sample rate.
    {
        size_t const input_capacity_frames = RENDER_QUANTUM_SIZE * 64;
        size_t const channel_count_for_buffers = channel_count;

        m_scratch.resample_input_channels.resize(channel_count_for_buffers);
        for (auto& channel : m_scratch.resample_input_channels)
            channel.resize(input_capacity_frames);

        m_scratch.resample_input_scratch_channels.resize(channel_count_for_buffers);
        for (auto& channel : m_scratch.resample_input_scratch_channels)
            channel.resize(input_capacity_frames);

        m_scratch.resample_input_spans.resize(channel_count_for_buffers);
        m_scratch.resample_output_spans.resize(channel_count_for_buffers);
    }

    // Preallocate per-quantum render scratch buffers on the control thread.
    {
        size_t const channels = channel_count;
        size_t const frames = RENDER_QUANTUM_SIZE;
        m_scratch.interleaved.resize(channels * frames);
        m_scratch.planar_spans.resize(channels);

        // AudioBus allocation is non-trivial; keep it off the render thread.
        m_scratch.mix_bus = make<AudioBus>(channels, frames, channels);
        m_scratch.context_mix_bus = make<AudioBus>(channels, frames, channels);
    }

    Optional<WireGraphBuildResult> deferred_graph;
    {
        Threading::MutexLocker locker(m_graph_mutex);
        deferred_graph = move(m_deferred_graph);
    }
    if (deferred_graph.has_value())
        apply_render_graph(move(*deferred_graph));
}

void WebAudioSession::shutdown()
{
    // Ensure the AudioWorklet host thread is stopped before shutdown proceeds.
    // Otherwise, process exit can destroy global runtime state while the host thread
    // is still finalizing JS/GC objects (e.g. MessagePort), leading to UAF.
    {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        m_worklet.host_ptr.store(nullptr, AK::MemoryOrder::memory_order_release);
        m_worklet.host = nullptr;
    }

    m_script_processor_host = nullptr;

    {
        Threading::MutexLocker locker(m_streams.script_processor_streams_mutex);
        StreamState::ScriptProcessorStreamMap* index = m_streams.script_processor_streams.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (index)
            index->unref();
    }

    {
        Threading::MutexLocker locker(m_streams.analyser_streams_mutex);
        StreamState::AnalyserStreamMap* index = m_streams.analyser_streams.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (index)
            index->unref();
    }

    {
        Threading::MutexLocker locker(m_streams.dynamics_compressor_streams_mutex);
        StreamState::DynamicsCompressorStreamMap* index = m_streams.dynamics_compressor_streams.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (index)
            index->unref();
    }

    {
        Threading::MutexLocker locker(m_worklet.ports_mutex);
        for (auto const& it : m_worklet.processor_port_fds) {
            if (it.value >= 0)
                (void)Core::System::close(it.value);
        }
        m_worklet.processor_port_fds.clear();
    }

    m_worklet.modules.clear();

    PreparedGraph* retired_pending_graph = m_pending_graph.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (retired_pending_graph)
        retired_pending_graph->unref();

    PreparedGraph* retired_active_graph = m_active_graph.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (retired_active_graph)
        retired_active_graph->unref();

    {
        Threading::MutexLocker locker(m_graph_mutex);
        m_deferred_graph.clear();
    }
    m_graph_generation.store(0, AK::MemoryOrder::memory_order_release);

    drain_retired_graphs_on_control_thread();

    {
        Threading::MutexLocker locker(m_streams.media_element_streams_mutex);
        m_streams.media_element_streams.clear();
    }
    {
        Threading::MutexLocker locker(m_streams.media_stream_streams_mutex);
        m_streams.media_stream_streams.clear();
    }

    for (auto const& it : m_audio_input_streams) {
        if (auto client = AudioServerClient::Client::default_client(); client)
            (void)client->destroy_audio_input_stream(it.key);
    }
    m_audio_input_streams.clear();

    if (m_timing_notify_write_fd != -1) {
        (void)Core::System::close(m_timing_notify_write_fd);
        m_timing_notify_write_fd = -1;
    }
}

}
