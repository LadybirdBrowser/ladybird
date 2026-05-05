/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibAudioServer/SingleSinkSessionClient.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibWebAudio/Engine/AudioContextPlaybackPolicy.h>

namespace Web::WebAudio {

class WebAudioSession;

}

namespace AudioServer {

class SingleSinkSessionClient;

}

namespace Web::WebAudio {

class WebAudioRenderThread {
public:
    struct OutputFormat {
        u32 sample_rate_hz { 0 };
        u32 channel_count { 0 };
    };

    static WebAudioRenderThread& the();

    using OutputStreamIsOpenCallback = Function<void(ErrorOr<OutputFormat>)>;

    ErrorOr<void> open_output_stream(u32 target_latency_ms, OutputStreamIsOpenCallback);
    OutputFormat output_format() const;

    void register_session(NonnullRefPtr<WebAudioSession> const&);
    void unregister_session(u64 session_id);

private:
    WebAudioRenderThread() = default;

    void ensure_thread_started();
    void stop_thread_if_unused();
    intptr_t render_thread_main();
    Vector<NonnullRefPtr<WebAudioSession>> snapshot_sessions();

    Threading::Mutex m_sessions_mutex;
    HashMap<u64, WeakPtr<WebAudioSession>> m_sessions;

    Threading::Mutex m_ring_mutex;
    Optional<Audio::SharedCircularBuffer> m_ring;
    RefPtr<Audio::SingleSinkSessionClient> m_output_session;
    u32 m_device_sample_rate_hz { 0 };
    u32 m_device_channel_count { 0 };
    u32 m_target_latency_ms { 0 };
    size_t m_bytes_per_frame { 0 };
    bool m_output_open_request_in_flight { false };
    u32 m_pending_target_latency_ms { 0 };
    Vector<OutputStreamIsOpenCallback> m_pending_output_open_callbacks;

    Vector<float> m_mix_interleaved;

    Atomic<bool> m_should_stop { false };
    RefPtr<Threading::Thread> m_render_thread;
    Render::AudioContextPlaybackPolicy::OutputSchedulingState m_output_state;
};

} // namespace Web::WebAudio
