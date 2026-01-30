/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/WeakPtr.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

namespace WebAudioWorker {

class WebAudioSession;

class WebAudioRenderThread {
public:
    struct OutputFormat {
        u32 sample_rate_hz { 0 };
        u32 channel_count { 0 };
    };

    static WebAudioRenderThread& the();

    ErrorOr<OutputFormat> ensure_output_open(u32 target_latency_ms);
    OutputFormat output_format() const;

    void register_session(NonnullRefPtr<WebAudioSession> const&);
    void unregister_session(u64 session_id);

private:
    WebAudioRenderThread() = default;

    void ensure_thread_started();
    void stop_thread_if_unused();
    intptr_t render_thread_main();
    Vector<NonnullRefPtr<WebAudioSession>> snapshot_sessions();

    struct OutputLoopState {
        bool drop_output_pacing_initialized { false };
        u64 drop_output_pacing_start_time_ns { 0 };
        u64 drop_output_pacing_start_rendered_frames { 0 };
        u32 drop_output_pacing_sample_rate_hz { 0 };
        u64 rendered_frames { 0 };
    };

    static void pace_when_output_dropped(OutputLoopState&, u32 sample_rate_hz);

    Threading::Mutex m_sessions_mutex;
    HashMap<u64, WeakPtr<WebAudioSession>> m_sessions;

    Threading::Mutex m_ring_mutex;
    Optional<Core::SharedSingleProducerCircularBuffer> m_ring;
    u64 m_audio_output_session_id { 0 };
    u32 m_device_sample_rate_hz { 0 };
    u32 m_device_channel_count { 0 };
    u32 m_target_latency_ms { 0 };
    size_t m_bytes_per_frame { 0 };

    Vector<float> m_mix_interleaved;

    Atomic<bool> m_should_stop { false };
    RefPtr<Threading::Thread> m_render_thread;
    OutputLoopState m_output_state;
};

}
