/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

class AudioContextPlaybackPolicy {
public:
    struct TimingTransport {
        Core::AnonymousBuffer timing_buffer;
        IPC::File timing_notify_read_fd;
        int timing_notify_write_fd { -1 };
    };

    struct OutputWaitDecision {
        enum class Action : u8 {
            RenderNow,
            Sleep,
            Yield,
        };

        Action action { Action::RenderNow };
        u32 sleep_ms { 0 };
    };

    struct OutputSchedulingState {
        bool drop_output_pacing_initialized { false };
        u64 drop_output_pacing_start_time_ns { 0 };
        u64 drop_output_pacing_start_rendered_frames { 0 };
        u32 drop_output_pacing_sample_rate_hz { 0 };
        u64 rendered_frames { 0 };
    };

    struct PlaybackStatsHooks {
        u32 context_sample_rate_hz { 0 };
        u32 device_channel_count { 0 };
        u64 rendered_frames_total { 0 };
        u64 underrun_frames_total { 0 };
        u64 graph_generation { 0 };
        bool is_suspended { false };
        u64 suspend_generation { 0 };
    };

    static ErrorOr<TimingTransport> create_timing_transport();

    static OutputWaitDecision decide_when_output_ring_has_room(size_t available_bytes,
        size_t quantum_bytes, size_t bytes_per_frame, u32 sample_rate_hz, double render_cost_ema_ns);
    static OutputWaitDecision decide_drop_output_pacing(OutputSchedulingState&, u32 sample_rate_hz,
        u64 now_ns);

    AudioContextPlaybackPolicy(Core::AnonymousBuffer timing_buffer, int timing_notify_write_fd,
        u32 device_sample_rate_hz = 0, u32 device_channel_count = 0);
    ~AudioContextPlaybackPolicy();

    void set_device_format(u32 device_sample_rate_hz, u32 device_channel_count);
    PlaybackStatsHooks publish_timing_update(u32 context_sample_rate_hz, u64 rendered_frames_total,
        u64 underrun_frames_total, u64 graph_generation, bool is_suspended, u64 suspend_generation);

    PlaybackStatsHooks const& playback_stats_hooks() const { return m_playback_stats_hooks; }

private:
    Core::AnonymousBuffer m_timing_buffer;
    TimingFeedbackPage* m_timing_page { nullptr };
    int m_timing_notify_write_fd { -1 };
    u32 m_device_sample_rate_hz { 0 };
    u32 m_device_channel_count { 0 };
    PlaybackStatsHooks m_playback_stats_hooks;
};

}
