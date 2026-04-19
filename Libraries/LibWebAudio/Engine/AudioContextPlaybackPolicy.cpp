/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCore/System.h>
#include <LibWebAudio/Engine/AudioContextPlaybackPolicy.h>

#include <errno.h>
#include <fcntl.h>

namespace Web::WebAudio::Render {

ErrorOr<AudioContextPlaybackPolicy::TimingTransport> AudioContextPlaybackPolicy::create_timing_transport()
{
    auto timing_buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(TimingFeedbackPage)));
    auto pipe_fds = TRY(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));

    return TimingTransport {
        .timing_buffer = timing_buffer,
        .timing_notify_read_fd = IPC::File::adopt_fd(pipe_fds[0]),
        .timing_notify_write_fd = pipe_fds[1],
    };
}

AudioContextPlaybackPolicy::OutputWaitDecision AudioContextPlaybackPolicy::decide_when_output_ring_has_room(
    size_t available_bytes, size_t quantum_bytes, size_t bytes_per_frame, u32 sample_rate_hz,
    double render_cost_ema_ns)
{
    if (available_bytes >= quantum_bytes)
        return {};

    if (bytes_per_frame == 0 || sample_rate_hz == 0)
        return OutputWaitDecision { .action = OutputWaitDecision::Action::Yield };

    size_t const available_frames = available_bytes / bytes_per_frame;
    size_t const quantum_frames = quantum_bytes / bytes_per_frame;
    size_t const missing_frames = available_frames >= quantum_frames ? 0 : (quantum_frames - available_frames);
    u64 const time_until_ready_ns = static_cast<u64>(missing_frames) * 1'000'000'000ULL / static_cast<u64>(sample_rate_hz);

    if (render_cost_ema_ns <= 0.0 || time_until_ready_ns <= static_cast<u64>(render_cost_ema_ns))
        return {};

    u64 const sleep_ns = time_until_ready_ns - static_cast<u64>(render_cost_ema_ns);
    u64 const sleep_ms = sleep_ns / 1'000'000ULL;
    if (sleep_ms == 0)
        return OutputWaitDecision { .action = OutputWaitDecision::Action::Yield };

    return OutputWaitDecision {
        .action = OutputWaitDecision::Action::Sleep,
        .sleep_ms = static_cast<u32>(min<u64>(sleep_ms, 5)),
    };
}

AudioContextPlaybackPolicy::OutputWaitDecision AudioContextPlaybackPolicy::decide_drop_output_pacing(
    OutputSchedulingState& state, u32 sample_rate_hz, u64 now_ns)
{
    if (sample_rate_hz == 0)
        return OutputWaitDecision { .action = OutputWaitDecision::Action::Yield };

    if (!state.drop_output_pacing_initialized || state.drop_output_pacing_sample_rate_hz != sample_rate_hz) {
        state.drop_output_pacing_initialized = true;
        state.drop_output_pacing_sample_rate_hz = sample_rate_hz;
        state.drop_output_pacing_start_time_ns = now_ns;
        state.drop_output_pacing_start_rendered_frames = state.rendered_frames;
        return {};
    }

    u64 const frames_since_start = state.rendered_frames - state.drop_output_pacing_start_rendered_frames;
    u64 target_ns = state.drop_output_pacing_start_time_ns + ((frames_since_start * 1'000'000'000ULL) / static_cast<u64>(sample_rate_hz));

    u64 const max_behind_ns = 20'000'000ULL;
    if (now_ns > target_ns && (now_ns - target_ns) > max_behind_ns) {
        state.drop_output_pacing_start_time_ns = now_ns;
        state.drop_output_pacing_start_rendered_frames = state.rendered_frames;
        return {};
    }

    if (target_ns <= now_ns)
        return {};

    u64 const remaining_ns = target_ns - now_ns;
    u64 const remaining_ms = remaining_ns / 1'000'000ULL;
    if (remaining_ms == 0)
        return OutputWaitDecision { .action = OutputWaitDecision::Action::Yield };

    return OutputWaitDecision {
        .action = OutputWaitDecision::Action::Sleep,
        .sleep_ms = static_cast<u32>(min<u64>(remaining_ms, 10)),
    };
}

AudioContextPlaybackPolicy::AudioContextPlaybackPolicy(Core::AnonymousBuffer timing_buffer,
    int timing_notify_write_fd, u32 device_sample_rate_hz, u32 device_channel_count)
    : m_timing_buffer(move(timing_buffer))
    , m_timing_page(m_timing_buffer.data<TimingFeedbackPage>())
    , m_timing_notify_write_fd(timing_notify_write_fd)
    , m_device_sample_rate_hz(device_sample_rate_hz)
    , m_device_channel_count(device_channel_count)
{
    if (m_timing_page)
        __builtin_memset(m_timing_page, 0, sizeof(*m_timing_page));
}

AudioContextPlaybackPolicy::~AudioContextPlaybackPolicy()
{
    if (m_timing_notify_write_fd >= 0)
        (void)Core::System::close(m_timing_notify_write_fd);
}

void AudioContextPlaybackPolicy::set_device_format(u32 device_sample_rate_hz, u32 device_channel_count)
{
    m_device_sample_rate_hz = device_sample_rate_hz;
    m_device_channel_count = device_channel_count;
}

AudioContextPlaybackPolicy::PlaybackStatsHooks AudioContextPlaybackPolicy::publish_timing_update(
    u32 context_sample_rate_hz, u64 rendered_frames_total, u64 underrun_frames_total,
    u64 graph_generation, bool is_suspended, u64 suspend_generation)
{
    if (context_sample_rate_hz == 0)
        context_sample_rate_hz = m_device_sample_rate_hz;

    m_playback_stats_hooks = PlaybackStatsHooks {
        .context_sample_rate_hz = context_sample_rate_hz,
        .device_channel_count = m_device_channel_count,
        .rendered_frames_total = rendered_frames_total,
        .underrun_frames_total = underrun_frames_total,
        .graph_generation = graph_generation,
        .is_suspended = is_suspended,
        .suspend_generation = suspend_generation,
    };

    if (m_timing_page)
        write_timing_page(*m_timing_page, context_sample_rate_hz, m_device_channel_count,
            rendered_frames_total, underrun_frames_total, graph_generation, is_suspended,
            suspend_generation);

    if (m_timing_notify_write_fd >= 0) {
        u8 byte = 0;
        auto result = Core::System::write(m_timing_notify_write_fd, ReadonlyBytes { &byte, 1 });
        if (result.is_error()) {
            auto const& error = result.error();
            if (!error.is_errno() || (error.code() != EAGAIN && error.code() != EWOULDBLOCK)) {
                (void)Core::System::close(m_timing_notify_write_fd);
                m_timing_notify_write_fd = -1;
            }
        }
    }

    return m_playback_stats_hooks;
}

}
