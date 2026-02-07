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
#include <WebAudioWorker/SessionResampler.h>
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

struct WebAudioSession::ThreadLoopState {
    u64 last_level_log_frame { 0 };
    f64 level_sum_squares { 0.0 };
    u64 level_sample_count { 0 };
    f32 level_peak { 0.0f };

    RefPtr<PreparedGraph> current_graph;
    u64 last_seen_generation { 0 };

    bool current_graph_has_script_processor { false };
    bool current_graph_has_media_element_source { false };
    bool current_graph_has_audio_worklet { false };
    u64 graph_swap_output_frame { 0 };
    bool logged_script_processor_never_ran_for_graph { false };

    u32 output_queue_backpressure_loops { 0 };
    u32 output_queue_insufficient_space_loops { 0 };

    bool drop_output_pacing_initialized { false };
    u64 drop_output_pacing_start_time_ns { 0 };
    u64 drop_output_pacing_start_rendered_frames { 0 };
    u32 drop_output_pacing_sample_rate_hz { 0 };

    bool suspended_pacing_initialized { false };
    u64 suspended_pacing_next_time_ns { 0 };
    u32 suspended_pacing_sample_rate_hz { 0 };
};

static u64 monotonic_time_ns()
{
    timespec ts {
        .tv_sec = 0,
        .tv_nsec = 0,
    };
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<u64>(ts.tv_nsec);
}

WebAudioSession::WebAudioSession(u64 session_id, u64 audio_output_session_id, u32 device_sample_rate_hz, u32 device_channel_count, Core::SharedSingleProducerCircularBuffer output_ring, Core::AnonymousBuffer timing_buffer, int timing_notify_write_fd, int client_id)
    : m_session_id(session_id)
    , m_client_id(client_id)
    , m_audio_output_session_id(audio_output_session_id)
    , m_device_sample_rate_hz(device_sample_rate_hz)
    , m_device_channel_count(device_channel_count)
    , m_ring(move(output_ring))
    , m_timing_buffer(move(timing_buffer))
    , m_timing_notify_write_fd(timing_notify_write_fd)
{
    m_worklet.control_event_loop = Core::EventLoop::current_weak();

    // The host is used from the render thread. Initialize it before any path can start the render thread
    // (AudioOutputDevice::when_ready() may invoke the callback synchronously).
    m_script_processor_host = make<SessionScriptProcessorHost>(*this);

    if (m_timing_buffer.is_valid() && m_timing_buffer.size() >= sizeof(WebAudioTimingPage))
        m_timing_page = m_timing_buffer.data<WebAudioTimingPage>();

    if (m_timing_page)
        __builtin_memset(m_timing_page, 0, sizeof(*m_timing_page));

    ensure_started();
}

WebAudioSession::~WebAudioSession()
{
    stop();
}

intptr_t WebAudioSession::render_thread_main()
{
    Web::WebAudio::mark_current_thread_as_render_thread();

    ensure_render_thread_scratch_initialized();
    ThreadLoopState state;

    while (!m_should_stop.load(AK::MemoryOrder::memory_order_acquire)) {
        u64 applied_suspend_state = m_requested_suspend_state.load(AK::MemoryOrder::memory_order_acquire);
        bool const is_suspended = decode_webaudio_suspend_state_is_suspended(applied_suspend_state);
        if (!m_ring.has_value()) {
            (void)Core::System::sleep_ms(1);
            continue;
        }

        maybe_swap_graph(state, m_graph_generation.load(AK::MemoryOrder::memory_order_acquire));
        maybe_log_script_processor_never_ran(state);

        auto available = m_ring->available_to_write();
        auto quantum_frames = RENDER_QUANTUM_SIZE;
        auto quantum_bytes = quantum_frames * m_scratch.bytes_per_frame;

        // If the output ring is backpressured, we normally pause rendering to avoid running
        // far ahead of the audio device. However, ScriptProcessor depends on the render thread
        // continuing to pump requests/responses even when the output sink is slow or absent.
        // In that case, keep processing the graph and simply drop output writes.
        size_t queued_bytes = m_ring->available_to_read();

        auto write_mode = decide_output_write_mode(state, available, queued_bytes, quantum_bytes);
        if (write_mode == OutputWriteMode::SleepAndContinue)
            continue;

        bool const drop_output_write = write_mode == OutputWriteMode::DropAndPace;
        auto device_channel_count = static_cast<size_t>(m_device_channel_count);

        prepare_output_buffers(device_channel_count, quantum_frames);
        pump_worklet_host(is_suspended, state.current_graph_has_audio_worklet);
        render_graph_quantum(state, is_suspended);
        if (!is_suspended) {
            publish_analyser_snapshots(state);
            publish_dynamics_compressor_snapshots(state);
        }
        update_timing_page_and_notify(applied_suspend_state);
        update_and_maybe_log_output_levels(state);
        commit_or_drop_output_and_pace(state, drop_output_write);
        pace_when_suspended(state, is_suspended);
    }

    return 0;
}

void WebAudioSession::render_graph_quantum(ThreadLoopState& state, bool is_suspended)
{
    if (is_suspended || !state.current_graph || !state.current_graph->executor)
        return;

    u32 const device_sample_rate_hz = m_device_sample_rate_hz;
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

void WebAudioSession::update_timing_page_and_notify(u64 applied_suspend_state)
{
    if (!m_timing_page)
        return;

    auto underruns = m_scratch.underrun_frames.load(AK::MemoryOrder::memory_order_relaxed);
    auto graph_generation = static_cast<u32>(m_graph_generation.load(AK::MemoryOrder::memory_order_relaxed));
    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_relaxed);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;
    write_webaudio_timing_page(*m_timing_page, context_sample_rate_hz, m_device_channel_count, m_scratch.rendered_frames, underruns, graph_generation, applied_suspend_state);

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

void WebAudioSession::commit_or_drop_output_and_pace(ThreadLoopState& state, bool drop_output_write)
{
    if (!m_ring.has_value())
        return;

    if (!drop_output_write) {
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(m_scratch.interleaved.data()), m_scratch.interleaved.size() * sizeof(float) };
        (void)m_ring->try_write(bytes);

        // Reset any drop-output pacing state once the sink is consuming again.
        state.drop_output_pacing_initialized = false;
        return;
    }

    // When the output sink is backpressured or absent, keep pumping ScriptProcessor requests,
    // but pace graph execution to real time to avoid running far ahead and exhausting the
    // non-blocking ScriptProcessor request/response queues.
    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_relaxed);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    if (context_sample_rate_hz > 0) {
        if (!state.drop_output_pacing_initialized || state.drop_output_pacing_sample_rate_hz != context_sample_rate_hz) {
            state.drop_output_pacing_initialized = true;
            state.drop_output_pacing_sample_rate_hz = context_sample_rate_hz;
            state.drop_output_pacing_start_time_ns = monotonic_time_ns();
            state.drop_output_pacing_start_rendered_frames = m_scratch.rendered_frames;
        }

        u64 frames_since_start = m_scratch.rendered_frames - state.drop_output_pacing_start_rendered_frames;
        u64 target_ns = state.drop_output_pacing_start_time_ns + ((frames_since_start * 1'000'000'000ULL) / static_cast<u64>(context_sample_rate_hz));

        u64 now_ns = monotonic_time_ns();
        // If we fell behind by a noticeable amount (scheduler jitter, pause, etc), rebase the
        // pacing window instead of trying to "catch up" by rendering faster than real time.
        // Catch-up bursts can flood ScriptProcessor callbacks and cause timeouts.
        u64 const max_behind_ns = 20'000'000ULL;
        if (now_ns > target_ns && (now_ns - target_ns) > max_behind_ns) {
            state.drop_output_pacing_start_time_ns = now_ns;
            state.drop_output_pacing_start_rendered_frames = m_scratch.rendered_frames;
            target_ns = now_ns;
        }

        while (target_ns > now_ns) {
            u64 remaining_ns = target_ns - now_ns;
            u64 remaining_ms = remaining_ns / 1'000'000ULL;
            if (remaining_ms > 0) {
                (void)Core::System::sleep_ms(static_cast<unsigned>(min<u64>(remaining_ms, 10)));
            } else {
                sched_yield();
            }
            now_ns = monotonic_time_ns();
        }
    } else {
        sched_yield();
    }
}

void WebAudioSession::pace_when_suspended(ThreadLoopState& state, bool is_suspended)
{
    if (!is_suspended) {
        state.suspended_pacing_initialized = false;
        return;
    }

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_relaxed);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    if (context_sample_rate_hz > 0) {
        u64 quantum_ns = (static_cast<u64>(RENDER_QUANTUM_SIZE) * 1'000'000'000ULL) / static_cast<u64>(context_sample_rate_hz);
        if (!state.suspended_pacing_initialized || state.suspended_pacing_sample_rate_hz != context_sample_rate_hz) {
            state.suspended_pacing_initialized = true;
            state.suspended_pacing_sample_rate_hz = context_sample_rate_hz;
            state.suspended_pacing_next_time_ns = monotonic_time_ns() + quantum_ns;
        }

        u64 now_ns = monotonic_time_ns();
        u64 target_ns = state.suspended_pacing_next_time_ns;
        u64 const max_behind_ns = 20'000'000ULL;
        if (now_ns > target_ns && (now_ns - target_ns) > max_behind_ns)
            target_ns = now_ns;

        while (target_ns > now_ns) {
            u64 remaining_ns = target_ns - now_ns;
            u64 remaining_ms = remaining_ns / 1'000'000ULL;
            if (remaining_ms > 0) {
                (void)Core::System::sleep_ms(static_cast<unsigned>(min<u64>(remaining_ms, 10)));
            } else {
                sched_yield();
            }
            now_ns = monotonic_time_ns();
        }

        state.suspended_pacing_next_time_ns = target_ns + quantum_ns;
    } else {
        sched_yield();
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
            retired_graph->unref();
    } else {
        state.current_graph = nullptr;
        PreparedGraph* retired_graph = m_active_graph.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (retired_graph)
            retired_graph->unref();
    }

    if (!state.current_graph || !state.current_graph->executor)
        return;

    state.current_graph_has_script_processor = false;
    state.current_graph_has_media_element_source = false;
    state.current_graph_has_audio_worklet = false;
    for (auto const& it : state.current_graph->build.description.nodes) {
        if (graph_node_type(it.value) == GraphNodeType::ScriptProcessor)
            state.current_graph_has_script_processor = true;
        if (graph_node_type(it.value) == GraphNodeType::AudioWorklet)
            state.current_graph_has_audio_worklet = true;
        if (graph_node_type(it.value) == GraphNodeType::MediaElementSource)
            state.current_graph_has_media_element_source = true;
        if (state.current_graph_has_script_processor && state.current_graph_has_media_element_source && state.current_graph_has_audio_worklet)
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

    if (Web::WebAudio::should_log_graph_updates()) {
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

WebAudioSession::OutputWriteMode WebAudioSession::decide_output_write_mode(ThreadLoopState& state, size_t available_to_write_bytes, size_t queued_bytes, size_t quantum_bytes)
{
    // If the output ring is backpressured, we normally pause rendering to avoid running
    // far ahead of the audio device. However, ScriptProcessor depends on the render thread
    // continuing to pump requests/responses even when the output sink is slow or absent.
    // In that case, keep processing the graph and simply drop output writes.

    size_t const quantum_frames = RENDER_QUANTUM_SIZE;
    u32 const output_sample_rate_hz = m_device_sample_rate_hz;
    u32 const max_queued_ms = state.current_graph_has_media_element_source
        ? WEBAUDIO_WORKER_MAX_QUEUED_MS_WITH_MEDIA_ELEMENT_SOURCE
        : WEBAUDIO_WORKER_MAX_QUEUED_MS_DEFAULT;
    u64 max_queued_frames = output_sample_rate_hz > 0
        ? (static_cast<u64>(output_sample_rate_hz) * static_cast<u64>(max_queued_ms)) / 1000
        : static_cast<u64>(quantum_frames);
    u64 const quantum_frames_u64 = static_cast<u64>(quantum_frames);
    max_queued_frames = max(max_queued_frames, quantum_frames_u64);
    max_queued_frames = (max_queued_frames / quantum_frames_u64) * quantum_frames_u64;
    max_queued_frames = max(max_queued_frames, quantum_frames_u64);

    size_t max_queued_bytes = static_cast<size_t>(max_queued_frames) * m_scratch.bytes_per_frame;
    if (m_ring.has_value()) {
        size_t ring_capacity_bytes = m_ring->capacity();
        max_queued_bytes = min(max_queued_bytes, ring_capacity_bytes);
    }
    max_queued_bytes = max(max_queued_bytes, quantum_bytes);

    if (queued_bytes >= max_queued_bytes) {
        if (!(state.current_graph_has_script_processor || state.current_graph_has_audio_worklet)) {
            if (Web::WebAudio::should_log_output_driver()) {
                if (++state.output_queue_backpressure_loops >= 250) {
                    dbgln("cid={}: WebAudio session={} stalled: queued_bytes={} max_queued_bytes={} available_to_write={} quantum_bytes={}",
                        m_client_id, m_session_id,
                        queued_bytes, max_queued_bytes, available_to_write_bytes, quantum_bytes);
                    state.output_queue_backpressure_loops = 0;
                }
            }
            (void)Core::System::sleep_ms(1);
            return OutputWriteMode::SleepAndContinue;
        }

        if (Web::WebAudio::should_log_output_driver() && (state.current_graph_has_script_processor || state.current_graph_has_audio_worklet)) {
            if (++state.output_queue_backpressure_loops >= 250) {
                dbgln("cid={}: WebAudio session={} dropping output (backpressured): queued_bytes={} max_queued_bytes={} available_to_write={} quantum_bytes={}",
                    m_client_id, m_session_id,
                    queued_bytes, max_queued_bytes, available_to_write_bytes, quantum_bytes);
                state.output_queue_backpressure_loops = 0;
            }
        }
    }
    state.output_queue_backpressure_loops = 0;

    if (available_to_write_bytes < quantum_bytes) {
        if (!(state.current_graph_has_script_processor || state.current_graph_has_audio_worklet)) {
            if (Web::WebAudio::should_log_output_driver()) {
                if (++state.output_queue_insufficient_space_loops >= 250) {
                    dbgln("cid={}: WebAudio session={} stalled: available_to_write={} quantum_bytes={} queued_bytes={}",
                        m_client_id, m_session_id,
                        available_to_write_bytes, quantum_bytes, queued_bytes);
                    state.output_queue_insufficient_space_loops = 0;
                }
            }
            (void)Core::System::sleep_ms(1);
            return OutputWriteMode::SleepAndContinue;
        }

        if (Web::WebAudio::should_log_output_driver() && (state.current_graph_has_script_processor || state.current_graph_has_audio_worklet)) {
            if (++state.output_queue_insufficient_space_loops >= 250) {
                dbgln("cid={}: WebAudio session={} dropping output (no space): available_to_write={} quantum_bytes={} queued_bytes={}",
                    m_client_id, m_session_id,
                    available_to_write_bytes, quantum_bytes, queued_bytes);
                state.output_queue_insufficient_space_loops = 0;
            }
        }
    }
    state.output_queue_insufficient_space_loops = 0;

    if (queued_bytes >= max_queued_bytes)
        return OutputWriteMode::DropAndPace;
    if (available_to_write_bytes < quantum_bytes)
        return OutputWriteMode::DropAndPace;
    return OutputWriteMode::WriteToRing;
}

void WebAudioSession::prepare_output_buffers(size_t device_channel_count, size_t quantum_frames)
{
    if (!m_scratch.mix_bus || m_scratch.mix_bus->channel_capacity() != device_channel_count || m_scratch.mix_bus->frame_count() != quantum_frames)
        m_scratch.mix_bus = make<AudioBus>(device_channel_count, quantum_frames, device_channel_count);

    if (m_scratch.interleaved.size() != quantum_frames * device_channel_count)
        m_scratch.interleaved.resize(quantum_frames * device_channel_count);
    m_scratch.interleaved.fill(0.0f);
}

void WebAudioSession::pump_worklet_host(bool is_suspended, bool current_graph_has_audio_worklet)
{
    auto* host = m_worklet.host_ptr.load(AK::MemoryOrder::memory_order_acquire);
    if (!host)
        return;

    u32 context_sample_rate_hz = m_context_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    if (current_graph_has_audio_worklet) {
        if (is_suspended) {
            if (Web::WebAudio::should_log_all())
                dbgln("[WebAudio] AudioWorklet: suspended pump frame={}", m_scratch.rendered_frames);
            host->prepare_render_thread_state(m_scratch.rendered_frames, static_cast<float>(context_sample_rate_hz), true);
        } else {
            host->prepare_render_thread_state(m_scratch.rendered_frames, static_cast<float>(context_sample_rate_hz), false);
        }
    } else {
        if (Web::WebAudio::should_log_all())
            dbgln("[WebAudio] AudioWorklet: global port pump frame={}", m_scratch.rendered_frames);
        host->service_worklet_event_loop(m_scratch.rendered_frames, static_cast<float>(context_sample_rate_hz));
    }
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

void WebAudioSession::add_worklet_module(ByteString url, ByteString source_text)
{
    if (Web::WebAudio::should_log_graph_updates())
        dbgln("cid={}: WebAudio session={} received worklet module '{}' ({} bytes)", m_client_id, m_session_id, url, source_text.length());

    bool had_worklet_host = false;
    {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        had_worklet_host = m_worklet.host != nullptr;
    }

    m_worklet.modules.append(WorkletModule {
        .url = move(url),
        .source_text = move(source_text),
    });

    m_worklet.generation.fetch_add(1, AK::MemoryOrder::memory_order_release);

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

    if (Web::WebAudio::should_log_graph_updates()) {
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
    ensure_started();
    if (!m_started.load(AK::MemoryOrder::memory_order_acquire)) {
        Threading::MutexLocker locker(m_graph_mutex);
        m_deferred_graph = move(graph);
        return;
    }

    apply_render_graph(move(graph));
}

void WebAudioSession::set_media_element_audio_source_streams(Vector<MediaElementAudioSourceStreamDescriptor> streams)
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

    for (auto& binding : streams) {
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

    if (!m_started.load(AK::MemoryOrder::memory_order_acquire))
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

    // MediaElementAudioSource provider resolution happens at compile time (see GraphCompiler).
    // If bindings change without a graph update message, we need to rebuild the executor so
    // MediaElementSource nodes stop being compiled as OhNoesRenderNode.
    Web::WebAudio::mark_current_thread_as_control_thread();

    WireGraphBuildResult build {
        .description = base_graph->build.description,
        .resources = base_graph->build.resources->clone(),
    };
    build.resources->clear_media_element_audio_sources();
    for (auto const& b : new_bindings)
        build.resources->set_media_element_audio_source(b.provider_id, b.provider);
    build.resources->set_script_processor_host(m_script_processor_host.ptr());

    auto executor = make<GraphExecutor>(
        build.description,
        static_cast<f32>(context_sample_rate_hz),
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
    if (!m_started.load(AK::MemoryOrder::memory_order_acquire))
        return;

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
    Web::WebAudio::mark_current_thread_as_control_thread();

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

        if (Web::WebAudio::should_log_graph_updates()) {
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
                            graph_node_type_name(GraphNodeType::MediaElementSource),
                            source.provider_id,
                            source.channel_count);
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

    graph.resources->set_script_processor_host(m_script_processor_host.ptr());

    auto executor = make<GraphExecutor>(
        graph.description,
        static_cast<f32>(context_sample_rate_hz),
        graph.resources.ptr());

    ensure_worklet_host();

    RefPtr<PreparedGraph> prepared_graph = adopt_ref(*new PreparedGraph(move(graph), move(executor)));
    prepared_graph->ref();
    PreparedGraph* retired_graph = m_pending_graph.exchange(prepared_graph.ptr(), AK::MemoryOrder::memory_order_acq_rel);
    if (retired_graph)
        retired_graph->unref();
    m_graph_generation.fetch_add(1, AK::MemoryOrder::memory_order_release);
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
    if (Web::WebAudio::should_log_graph_updates())
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

        if (Web::WebAudio::should_log_graph_updates())
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

    m_worklet.generation.fetch_add(1, AK::MemoryOrder::memory_order_release);

    ensure_worklet_host();

    if (had_worklet_host) {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        if (m_worklet.host)
            m_worklet.host->enqueue_port_bindings(host_port_bindings);
    }
}

void WebAudioSession::set_worklet_node_definitions(Vector<WorkletNodeDefinition> const& definitions)
{
    if (Web::WebAudio::should_log_graph_updates())
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

    m_worklet.generation.fetch_add(1, AK::MemoryOrder::memory_order_release);

    ensure_worklet_host();

    if (m_worklet.host)
        m_worklet.host->synchronize_node_definitions(definitions);

    if (had_worklet_host) {
        Threading::MutexLocker locker(m_worklet.host_mutex);
        if (m_worklet.host)
            m_worklet.host->enqueue_node_definitions(move(definitions_for_host));
    }
}

void WebAudioSession::set_suspended(bool suspended, u64 generation)
{
    // This is a low-frequency control-plane request, safe to store atomically.
    // The render thread will apply it at a quantum boundary.
    m_requested_suspend_state.store(encode_webaudio_suspend_state(suspended, generation), AK::MemoryOrder::memory_order_release);
}

void WebAudioSession::ensure_started()
{
    if (m_started.load(AK::MemoryOrder::memory_order_acquire))
        return;

    bool expected = false;
    if (!m_start_in_progress.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel))
        return;

    if (!m_ring.has_value()) {
        m_start_in_progress.store(false, AK::MemoryOrder::memory_order_release);
        return;
    }

    u32 sample_rate_hz = m_device_sample_rate_hz;
    static constexpr u32 max_supported_output_channels = 32;
    u32 channel_count = m_device_channel_count;
    if (channel_count > max_supported_output_channels) {
        warnln("cid={}: WebAudio session clamping output channels {} -> {}", m_client_id, channel_count, max_supported_output_channels);
        channel_count = max_supported_output_channels;
    }
    if (sample_rate_hz == 0 || channel_count == 0) {
        warnln("cid={}: WebAudio session invalid sample specification {} Hz, {} channels", m_client_id, sample_rate_hz, channel_count);
        m_start_in_progress.store(false, AK::MemoryOrder::memory_order_release);
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

    m_render_thread = Threading::Thread::construct("WebAudioWorker"sv, [this] {
        return render_thread_main();
    });
    m_render_thread->start();

    m_started.store(true, AK::MemoryOrder::memory_order_release);
    m_start_in_progress.store(false, AK::MemoryOrder::memory_order_release);

    Optional<WireGraphBuildResult> deferred_graph;
    {
        Threading::MutexLocker locker(m_graph_mutex);
        deferred_graph = move(m_deferred_graph);
    }
    if (deferred_graph.has_value())
        apply_render_graph(move(*deferred_graph));
}

void WebAudioSession::stop()
{
    m_should_stop.store(true, AK::MemoryOrder::memory_order_release);

    if (m_render_thread && m_render_thread->needs_to_be_joined())
        (void)m_render_thread->join();

    m_render_thread = nullptr;

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

    if (m_audio_output_session_id != 0) {
        if (auto client = AudioServerClient::Client::default_client(); client)
            (void)client->destroy_audio_output_session(m_audio_output_session_id);
        m_audio_output_session_id = 0;
    }

    m_ring.clear();

    if (m_timing_notify_write_fd != -1) {
        (void)Core::System::close(m_timing_notify_write_fd);
        m_timing_notify_write_fd = -1;
    }

    m_start_in_progress.store(false, AK::MemoryOrder::memory_order_release);

    m_started.store(false, AK::MemoryOrder::memory_order_release);
}

}
