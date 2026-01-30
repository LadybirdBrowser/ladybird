/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/AudioBufferSourceRenderNode.h>

namespace Web::WebAudio::Render {

AudioBufferSourceRenderNode::AudioBufferSourceRenderNode(NodeID node_id, AudioBufferSourceGraphNode const& desc, RefPtr<SharedAudioBuffer> buffer, size_t quantum_size)
    : RenderNode(node_id)
    , m_has_start(desc.start_frame.has_value())
    , m_playback_rate(desc.playback_rate)
    , m_detune_cents(desc.detune_cents)
    , m_start_frame(desc.start_frame.value_or(0))
    , m_stop_frame(desc.stop_frame)
    , m_offset_frame(desc.offset_frame)
    , m_duration_in_sample_frames(desc.duration_in_sample_frames)
    , m_loop(desc.loop)
    , m_loop_start_frame(desc.loop_start_frame)
    , m_loop_end_frame(desc.loop_end_frame)
    , m_sample_rate(buffer ? buffer->sample_rate() : desc.sample_rate)
    , m_channel_count(buffer ? buffer->channel_count() : desc.channel_count)
    , m_length_in_sample_frames(buffer ? buffer->length_in_sample_frames() : desc.length_in_sample_frames)
    , m_buffer(move(buffer))
    , m_output(max(1u, m_channel_count), quantum_size)
    , m_playback_rate_input(1, quantum_size)
    , m_detune_input(1, quantum_size)
    , m_start_time_in_context_frames(desc.start_time_in_context_frames)
{
    // Initialize the resampler coefficient table off the render thread.
    prepare_sinc_resampler_kernel(m_resampler_table, 1.0);
}

void AudioBufferSourceRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
    m_output.zero();
    m_output.set_channel_count(0);
    if (!m_has_start || m_finished)
        return;

    // If we don't have any buffer data, render silence.
    if (!m_buffer || m_channel_count == 0 || m_buffer->channel(0).is_empty())
        return;

    // playbackRate and detune are k-rate AudioParams in WebAudio.
    // For now, we treat any connected signal as a k-rate modulation sampled at the start of the quantum.
    if (param_inputs.size() > AudioBufferSourceParamIndex::playback_rate)
        mix_inputs_into(m_playback_rate_input, param_inputs[AudioBufferSourceParamIndex::playback_rate].span());
    else
        m_playback_rate_input.zero();

    if (param_inputs.size() > AudioBufferSourceParamIndex::detune)
        mix_inputs_into(m_detune_input, param_inputs[AudioBufferSourceParamIndex::detune].span());
    else
        m_detune_input.zero();

    bool const has_playback_rate_param_input = param_inputs.size() > AudioBufferSourceParamIndex::playback_rate && !param_inputs[AudioBufferSourceParamIndex::playback_rate].is_empty();
    bool const has_detune_param_input = param_inputs.size() > AudioBufferSourceParamIndex::detune && !param_inputs[AudioBufferSourceParamIndex::detune].is_empty();

    auto sanitize_k_rate = [](f64 value) {
        constexpr f64 scale = 1'000'000.0;
        if (__builtin_isnan(value) || !__builtin_isfinite(value))
            return 0.0;
        return AK::floor((value * scale) + 0.5) / scale;
    };

    auto playback_rate_value_for = [&](size_t sample_index) {
        if (has_playback_rate_param_input)
            return sanitize_k_rate(static_cast<f64>(m_playback_rate_input.channel(0)[sample_index]));
        return sanitize_k_rate(static_cast<f64>(m_playback_rate));
    };

    auto detune_value_for = [&](size_t sample_index) {
        if (has_detune_param_input)
            return sanitize_k_rate(static_cast<f64>(m_detune_input.channel(0)[sample_index]));
        return sanitize_k_rate(static_cast<f64>(m_detune_cents));
    };

    f64 const initial_playback_rate = playback_rate_value_for(0);
    f64 const initial_detune_cents = detune_value_for(0);

    f64 const detune_multiplier = AK::exp2(initial_detune_cents / 1200.0);
    f64 const buffer_to_context_ratio = static_cast<f64>(m_sample_rate) / static_cast<f64>(context.sample_rate);
    f64 const initial_increment = buffer_to_context_ratio * initial_playback_rate * detune_multiplier;

    // Update coefficient generation for downsampling ratios without allocating.
    // Only rebuild when the effective increment changes and SRC is used.
    if (buffer_to_context_ratio != 1.0) {
        bool const should_rebuild = !m_last_resampler_increment.has_value()
            || AK::fabs(m_last_resampler_increment.value() - initial_increment) > 1e-12;
        if (should_rebuild) {
            prepare_sinc_resampler_kernel(m_resampler_table, initial_increment);
            m_last_resampler_increment = initial_increment;
        }
    }

    size_t const buffer_length = m_length_in_sample_frames;
    if (buffer_length == 0)
        return;

    size_t const offset = min(m_offset_frame, buffer_length);

    size_t loop_start = min(m_loop_start_frame, buffer_length);
    size_t loop_end = m_loop_end_frame == 0 ? buffer_length : min(m_loop_end_frame, buffer_length);
    if (loop_end <= loop_start) {
        loop_start = 0;
        loop_end = buffer_length;
    }
    size_t const loop_length = loop_end - loop_start;

    size_t const channels_to_render = min(m_channel_count, m_buffer->channel_count());
    if (channels_to_render == 0)
        return;

    size_t const frames = m_output.frame_count();
    size_t const graph_start = context.current_frame;

    // The start time uses the exact time without rounding to sample frames.
    f64 start_time_in_context_frames = m_start_time_in_context_frames.value_or(static_cast<f64>(m_start_frame));
    size_t start_frame = m_start_frame;
    if (m_start_time_in_context_frames.has_value()) {
        if (!__builtin_isfinite(start_time_in_context_frames))
            start_frame = start_time_in_context_frames > 0.0 ? AK::NumericLimits<size_t>::max() : 0;
        else if (start_time_in_context_frames <= 0.0)
            start_frame = 0;
        else {
            // [from-spec] The exact value of when is always used without rounding to the nearest sample frame.
            // When it corresponds to an exact sample frame, begin on that frame.
            constexpr f64 start_time_snap_epsilon = 1e-9;
            f64 const nearest = AK::round(start_time_in_context_frames);
            if (AK::fabs(start_time_in_context_frames - nearest) <= start_time_snap_epsilon) {
                start_time_in_context_frames = nearest;
                start_frame = static_cast<size_t>(nearest);
            } else {
                f64 const rounded = AK::ceil(start_time_in_context_frames);
                if (rounded >= static_cast<f64>(AK::NumericLimits<size_t>::max()))
                    start_frame = AK::NumericLimits<size_t>::max();
                else
                    start_frame = static_cast<size_t>(rounded);
            }
        }
    }

    // Stop takes precedence within this quantum.
    if (m_stop_frame.has_value() && graph_start >= m_stop_frame.value()) {
        m_finished = true;
        return;
    }

    // Determine the earliest sample within this quantum that can produce output.
    size_t quantum_render_start = 0;
    if (!m_is_playing) {
        if (graph_start + frames <= start_frame)
            return;
        quantum_render_start = graph_start < start_frame ? start_frame - graph_start : 0;
    }

    size_t quantum_render_end = frames;
    if (m_stop_frame.has_value() && graph_start + frames > m_stop_frame.value())
        quantum_render_end = m_stop_frame.value() - graph_start;

    if (quantum_render_start >= quantum_render_end) {
        // No active frames in this quantum; remain inactive.
        return;
    }

    m_output.set_channel_count(channels_to_render);

    auto const& table = m_resampler_table;

    auto sample_from_channel = [&](ReadonlySpan<f32> channel, f64 playhead, long sample_index) -> f32 {
        if (m_loop && loop_length > 0) {
            // Only treat the loop section as periodic once the playhead is within the loop region.
            if (playhead >= static_cast<f64>(loop_start) && playhead < static_cast<f64>(loop_end)) {
                long const start = static_cast<long>(loop_start);
                long const end = static_cast<long>(loop_end);
                long const len = end - start;
                if (len > 0) {
                    long rel = sample_index - start;
                    rel %= len;
                    if (rel < 0)
                        rel += len;
                    sample_index = start + rel;
                }
            }
        }

        // Extrapolate samples outside the buffer range by linearly extending the endpoint slope.
        // This avoids introducing spurious zeros when resampling near the buffer edges.
        if (channel.is_empty())
            return 0.0f;

        if (channel.size() == 1)
            return channel[0];

        if (sample_index < 0) {
            // Backward extrapolation from the first two samples.
            f32 const slope = channel[1] - channel[0];
            return channel[0] + (static_cast<f32>(sample_index) * slope);
        }

        size_t const index = static_cast<size_t>(sample_index);
        if (index >= channel.size()) {
            // Forward extrapolation from the last two samples.
            f32 const slope = channel[channel.size() - 1] - channel[channel.size() - 2];
            f32 const delta = static_cast<f32>(sample_index - static_cast<long>(channel.size() - 1));
            return channel[channel.size() - 1] + (delta * slope);
        }

        return channel[index];
    };

    auto wrap_playhead_if_needed = [&](f64& playhead, f64 increment) {
        if (!m_loop || loop_length == 0)
            return;

        // Match the block-based behavior: wrap only when crossing a loop boundary.
        if (increment > 0.0) {
            if (playhead >= static_cast<f64>(loop_end)) {
                f64 const start = static_cast<f64>(loop_start);
                f64 const len = static_cast<f64>(loop_length);
                f64 rel = playhead - start;
                rel = AK::fmod(rel, len);
                if (rel < 0.0)
                    rel += len;
                playhead = start + rel;
            }
        } else if (increment < 0.0) {
            if (playhead < static_cast<f64>(loop_start)) {
                f64 const start = static_cast<f64>(loop_start);
                f64 const len = static_cast<f64>(loop_length);
                f64 rel = playhead - start;
                rel = AK::fmod(rel, len);
                if (rel < 0.0)
                    rel += len;
                playhead = start + rel;
            }
        }
    };

    Optional<size_t> effective_duration_in_sample_frames = m_duration_in_sample_frames;
    if (effective_duration_in_sample_frames.has_value() && !m_loop) {
        size_t const max_duration = buffer_length > offset ? buffer_length - offset : 0;
        if (effective_duration_in_sample_frames.value() > max_duration)
            effective_duration_in_sample_frames = max_duration;
    }

    constexpr f64 playhead_snap_epsilon = 1e-9;
    auto should_stop_before_rendering = [&](f64 increment) -> bool {
        if (effective_duration_in_sample_frames.has_value()) {
            // Duration is in buffer sample frames; stop after consuming that much buffer timeline.
            return m_progress_in_sample_frames >= static_cast<f64>(effective_duration_in_sample_frames.value());
        }

        if (m_loop && loop_length > 0)
            return false;

        if (increment >= 0.0)
            return (m_playhead_in_sample_frames + playhead_snap_epsilon) >= static_cast<f64>(buffer_length);
        return m_playhead_in_sample_frames <= -playhead_snap_epsilon;
    };

    auto increment_for_sample = [&](size_t sample_index) {
        f64 const playback_rate = playback_rate_value_for(sample_index);
        f64 const detune_cents = detune_value_for(sample_index);
        f64 const detune_multiplier = AK::exp2(detune_cents / 1200.0);
        return buffer_to_context_ratio * playback_rate * detune_multiplier;
    };

    // Render sample-by-sample to support SRC and fractional playhead.
    for (size_t out_index = quantum_render_start; out_index < quantum_render_end; ++out_index) {
        size_t const graph_frame = graph_start + out_index;

        if (!m_is_playing) {
            // Start at the first sample >= start_frame.
            if (graph_frame < start_frame)
                continue;

            m_is_playing = true;
            m_finished = false;
            m_progress_in_sample_frames = 0.0;

            // Initialize playhead in buffer sample frames.
            // If we begin rendering after start_frame, advance by the elapsed context frames using the
            // current effective increment (k-rate parameters).
            f64 const elapsed_context_frames = static_cast<f64>(graph_frame) - start_time_in_context_frames;
            f64 const increment = increment_for_sample(out_index);
            m_playhead_in_sample_frames = static_cast<f64>(offset) + (elapsed_context_frames * increment);
            wrap_playhead_if_needed(m_playhead_in_sample_frames, increment);
        }

        f64 const increment = increment_for_sample(out_index);

        if (should_stop_before_rendering(increment)) {
            m_finished = true;
            break;
        }

        // Interpolate at the current playhead.
        f64 const playhead = m_playhead_in_sample_frames;
        f64 const base_index_d = AK::floor(playhead);
        f64 const frac = playhead - base_index_d; // in [0,1)

        // If the playhead is effectively on an integer sample frame, ideal sinc interpolation
        // reconstructs the original sample exactly. Special-case this to avoid tiny DC drift
        // from the truncated/windowed kernel and floating-point coefficient sums.
        // [from-spec] If the playhead corresponds to the location of an exact sample frame in the buffer,
        // the sample returned must be that exact frame.
        constexpr f64 integer_snap_epsilon = 1e-9;
        bool const snapped_to_integer = (frac < integer_snap_epsilon) || ((1.0 - frac) < integer_snap_epsilon);
        long const snapped_index = static_cast<long>(base_index_d) + ((1.0 - frac) < integer_snap_epsilon ? 1 : 0);

        for (size_t ch = 0; ch < channels_to_render; ++ch) {
            auto src_span = m_buffer->channel(ch);

            if (snapped_to_integer) {
                m_output.channel(ch)[out_index] = sample_from_channel(src_span, playhead, snapped_index);
                continue;
            }

            // [from-spec] Sub-sample offsets must interpolate between adjacent sample frames.
            if (buffer_to_context_ratio == 1.0) {
                long const base_index = static_cast<long>(base_index_d);
                f64 const s0 = static_cast<f64>(sample_from_channel(src_span, playhead, base_index));
                f64 const s1 = static_cast<f64>(sample_from_channel(src_span, playhead, base_index + 1));
                m_output.channel(ch)[out_index] = static_cast<f32>(s0 + ((s1 - s0) * frac));
                continue;
            }

            auto sample_at = [&](size_t, i64 sample_index) -> f32 {
                return sample_from_channel(src_span, playhead, static_cast<long>(sample_index));
            };

            m_output.channel(ch)[out_index] = sinc_resampler_interpolate_at(table, playhead, 0, sample_at);
        }

        // Advance playhead.
        m_playhead_in_sample_frames += increment;
        if (increment >= 0.0)
            m_progress_in_sample_frames += increment;
        else
            m_progress_in_sample_frames += -increment;

        wrap_playhead_if_needed(m_playhead_in_sample_frames, increment);
    }
}

void AudioBufferSourceRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<AudioBufferSourceGraphNode>())
        return;

    auto const& desc = node.get<AudioBufferSourceGraphNode>();

    m_has_start = desc.start_frame.has_value();
    m_playback_rate = desc.playback_rate;
    m_detune_cents = desc.detune_cents;
    m_start_frame = desc.start_frame.value_or(0);
    m_start_time_in_context_frames = desc.start_time_in_context_frames;
    m_stop_frame = desc.stop_frame;
    m_offset_frame = desc.offset_frame;
    m_duration_in_sample_frames = desc.duration_in_sample_frames;
    m_loop = desc.loop;
    m_loop_start_frame = desc.loop_start_frame;
    m_loop_end_frame = desc.loop_end_frame;

    if (!m_has_start) {
        m_is_playing = false;
        m_finished = false;
        m_playhead_in_sample_frames = 0.0;
        m_progress_in_sample_frames = 0.0;
    }
}

}
