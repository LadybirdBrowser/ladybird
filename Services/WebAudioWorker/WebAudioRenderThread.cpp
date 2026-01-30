/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <WebAudioWorker/WebAudioRenderThread.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace WebAudioWorker {

static u64 monotonic_time_ns()
{
    timespec ts {
        .tv_sec = 0,
        .tv_nsec = 0,
    };
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<u64>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<u64>(ts.tv_nsec);
}

WebAudioRenderThread& WebAudioRenderThread::the()
{
    static WebAudioRenderThread instance;
    return instance;
}

WebAudioRenderThread::OutputFormat WebAudioRenderThread::output_format() const
{
    return OutputFormat {
        .sample_rate_hz = m_device_sample_rate_hz,
        .channel_count = m_device_channel_count,
    };
}

ErrorOr<WebAudioRenderThread::OutputFormat> WebAudioRenderThread::ensure_output_open(u32 target_latency_ms)
{
    bool needs_reopen = false;
    {
        Threading::MutexLocker locker(m_ring_mutex);
        if (m_audio_output_session_id != 0 && m_ring.has_value()) {
            if (target_latency_ms <= m_target_latency_ms)
                return output_format();
            needs_reopen = true;
        }
    }

    if (needs_reopen) {
        if (auto client = AudioServerClient::Client::default_client(); client)
            (void)client->destroy_audio_output_session(m_audio_output_session_id);
        Threading::MutexLocker locker(m_ring_mutex);
        m_audio_output_session_id = 0;
        m_ring.clear();
        m_device_sample_rate_hz = 0;
        m_device_channel_count = 0;
        m_bytes_per_frame = 0;
        m_target_latency_ms = 0;
    }

    RefPtr<AudioServerClient::Client> audio_server = AudioServerClient::Client::default_client();
    if (!audio_server)
        return Error::from_string_literal("WebAudioWorker has no AudioServer client");

    auto output_session_or_error = audio_server->create_audio_output_session(target_latency_ms);
    if (output_session_or_error.is_error())
        return output_session_or_error.release_error();

    auto output_session = output_session_or_error.release_value();
    u32 sample_rate = output_session.sample_rate;
    u32 channel_count = output_session.channel_count;
    if (sample_rate == 0 || channel_count == 0) {
        (void)audio_server->destroy_audio_output_session(output_session.session_id);
        return Error::from_string_literal("WebAudioWorker invalid output format");
    }

    {
        Threading::MutexLocker locker(m_ring_mutex);
        m_audio_output_session_id = output_session.session_id;
        m_device_sample_rate_hz = sample_rate;
        m_device_channel_count = channel_count;
        m_target_latency_ms = target_latency_ms;
        m_bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);
        m_ring = move(output_session.ring);
        m_mix_interleaved.resize(static_cast<size_t>(channel_count) * Web::WebAudio::Render::RENDER_QUANTUM_SIZE);
        m_mix_interleaved.fill(0.0f);
    }

    ensure_thread_started();

    return output_format();
}

void WebAudioRenderThread::register_session(NonnullRefPtr<WebAudioSession> const& session)
{
    {
        Threading::MutexLocker locker(m_sessions_mutex);
        m_sessions.set(session->session_id(), session->make_weak_ptr());
    }
    ensure_thread_started();
}

void WebAudioRenderThread::unregister_session(u64 session_id)
{
    {
        Threading::MutexLocker locker(m_sessions_mutex);
        m_sessions.remove(session_id);
    }
    stop_thread_if_unused();
}

void WebAudioRenderThread::ensure_thread_started()
{
    if (m_render_thread)
        return;

    m_should_stop.store(false, AK::MemoryOrder::memory_order_release);
    m_render_thread = Threading::Thread::construct("RenderThread"sv, [this] {
        return render_thread_main();
    });
    m_render_thread->start();
}

void WebAudioRenderThread::stop_thread_if_unused()
{
    {
        Threading::MutexLocker locker(m_sessions_mutex);
        if (!m_sessions.is_empty())
            return;
    }

    m_should_stop.store(true, AK::MemoryOrder::memory_order_release);
    if (m_render_thread && m_render_thread->needs_to_be_joined())
        (void)m_render_thread->join();
    m_render_thread = nullptr;

    if (m_audio_output_session_id != 0) {
        if (auto client = AudioServerClient::Client::default_client(); client)
            (void)client->destroy_audio_output_session(m_audio_output_session_id);
        m_audio_output_session_id = 0;
    }

    Threading::MutexLocker locker(m_ring_mutex);
    m_ring.clear();
    m_device_sample_rate_hz = 0;
    m_device_channel_count = 0;
    m_bytes_per_frame = 0;
    m_mix_interleaved.clear();
    m_output_state = {};
}

Vector<NonnullRefPtr<WebAudioSession>> WebAudioRenderThread::snapshot_sessions()
{
    Vector<NonnullRefPtr<WebAudioSession>> sessions;
    Threading::MutexLocker locker(m_sessions_mutex);
    sessions.ensure_capacity(m_sessions.size());
    for (auto const& it : m_sessions) {
        if (auto session = it.value.strong_ref())
            sessions.unchecked_append(session.release_nonnull());
    }
    return sessions;
}

void WebAudioRenderThread::pace_when_output_dropped(OutputLoopState& state, u32 sample_rate_hz)
{
    if (sample_rate_hz == 0) {
        sched_yield();
        return;
    }

    if (!state.drop_output_pacing_initialized || state.drop_output_pacing_sample_rate_hz != sample_rate_hz) {
        state.drop_output_pacing_initialized = true;
        state.drop_output_pacing_sample_rate_hz = sample_rate_hz;
        state.drop_output_pacing_start_time_ns = monotonic_time_ns();
        state.drop_output_pacing_start_rendered_frames = state.rendered_frames;
    }

    u64 frames_since_start = state.rendered_frames - state.drop_output_pacing_start_rendered_frames;
    u64 target_ns = state.drop_output_pacing_start_time_ns + ((frames_since_start * 1'000'000'000ULL) / static_cast<u64>(sample_rate_hz));

    u64 now_ns = monotonic_time_ns();
    u64 const max_behind_ns = 20'000'000ULL;
    if (now_ns > target_ns && (now_ns - target_ns) > max_behind_ns) {
        state.drop_output_pacing_start_time_ns = now_ns;
        state.drop_output_pacing_start_rendered_frames = state.rendered_frames;
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
}

intptr_t WebAudioRenderThread::render_thread_main()
{
    Web::WebAudio::mark_current_thread_as_render_thread();

    static bool perf_log_enabled = Core::Environment::has("WEBAUDIO_PERF_LOG"sv);
    double render_cost_ema_ns = 0.0;
    double render_cost_alpha = 0.1;
    i64 last_perf_log_ms = 0;

    while (!m_should_stop.load(AK::MemoryOrder::memory_order_acquire)) {
        auto sessions = snapshot_sessions();
        if (sessions.is_empty()) {
            (void)Core::System::sleep_ms(5);
            continue;
        }

        Optional<Core::SharedSingleProducerCircularBuffer> ring;
        size_t bytes_per_frame = 0;
        {
            Threading::MutexLocker locker(m_ring_mutex);
            ring = m_ring;
            bytes_per_frame = m_bytes_per_frame;
        }

        if (!ring.has_value()) {
            (void)Core::System::sleep_ms(5);
            continue;
        }

        size_t quantum_frames = Web::WebAudio::Render::RENDER_QUANTUM_SIZE;
        size_t quantum_bytes = static_cast<size_t>(m_device_channel_count) * quantum_frames * sizeof(float);
        if (bytes_per_frame == 0 || quantum_bytes == 0) {
            (void)Core::System::sleep_ms(5);
            continue;
        }

        u32 sample_rate_hz = m_device_sample_rate_hz;
        if (sample_rate_hz == 0) {
            (void)Core::System::sleep_ms(5);
            continue;
        }

        for (;;) {
            size_t available_bytes = ring->available_to_write();
            if (available_bytes >= quantum_bytes)
                break;

            size_t available_frames = available_bytes / bytes_per_frame;
            size_t missing_frames = available_frames >= quantum_frames ? 0 : (quantum_frames - available_frames);
            u64 time_until_ready_ns = static_cast<u64>(missing_frames) * 1'000'000'000ULL / static_cast<u64>(sample_rate_hz);

            if (render_cost_ema_ns > 0.0 && time_until_ready_ns > static_cast<u64>(render_cost_ema_ns)) {
                u64 sleep_ns = time_until_ready_ns - static_cast<u64>(render_cost_ema_ns);
                u64 sleep_ms = sleep_ns / 1'000'000ULL;
                if (sleep_ms > 0) {
                    (void)Core::System::sleep_ms(static_cast<unsigned>(min<u64>(sleep_ms, 5)));
                } else {
                    sched_yield();
                }
                continue;
            }

            break;
        }

        u64 render_start_ns = monotonic_time_ns();
        bool any_rendered_audio = false;
        Vector<NonnullRefPtr<WebAudioSession>> rendered_sessions; // FIXME: cap
        rendered_sessions.ensure_capacity(sessions.size());

        for (auto& session : sessions) {
            bool did_render = session->render_one_quantum();
            if (did_render) {
                any_rendered_audio = true;
                rendered_sessions.unchecked_append(session);
            }
        }

        if (m_mix_interleaved.size() != static_cast<size_t>(m_device_channel_count) * Web::WebAudio::Render::RENDER_QUANTUM_SIZE)
            m_mix_interleaved.resize(static_cast<size_t>(m_device_channel_count) * Web::WebAudio::Render::RENDER_QUANTUM_SIZE);
        m_mix_interleaved.fill(0.0f);

        if (any_rendered_audio) {
            for (auto& session : rendered_sessions) {
                auto session_output = session->interleaved_output();
                if (session_output.size() != m_mix_interleaved.size())
                    continue;
                for (size_t i = 0; i < m_mix_interleaved.size(); ++i)
                    m_mix_interleaved[i] += session_output[i];
            }
        }

        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(m_mix_interleaved.data()), m_mix_interleaved.size() * sizeof(float) };
        quantum_bytes = bytes.size();
        while (ring->available_to_write() < quantum_bytes)
            (void)Core::System::sleep_ms(1);
        (void)ring->try_write(bytes);
        m_output_state.drop_output_pacing_initialized = false;
        m_output_state.rendered_frames += Web::WebAudio::Render::RENDER_QUANTUM_SIZE;

        u64 render_end_ns = monotonic_time_ns();
        if (render_end_ns >= render_start_ns) {
            double sample_ns = static_cast<double>(render_end_ns - render_start_ns);
            if (render_cost_ema_ns == 0.0)
                render_cost_ema_ns = sample_ns;
            else
                render_cost_ema_ns += (sample_ns - render_cost_ema_ns) * render_cost_alpha;
        }

        if (perf_log_enabled) {
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            if (last_perf_log_ms == 0 || (now_ms - last_perf_log_ms) >= 2000) {
                last_perf_log_ms = now_ms;
                double quantum_duration_ms = (static_cast<double>(quantum_frames) * 1000.0) / static_cast<double>(sample_rate_hz);
                double render_cost_ms = render_cost_ema_ns / 1'000'000.0;
                double render_ratio = quantum_duration_ms > 0.0 ? (render_cost_ms / quantum_duration_ms) : 0.0;
                dbgln("[WebAudio][Perf] render_ema_ms={:.3f} quantum_ms={:.3f} ratio={:.3f}", render_cost_ms, quantum_duration_ms, render_ratio);
            }
        }
    }

    return 0;
}

}
