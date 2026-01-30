/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/AudioBufferSourceRenderNode.h>

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>

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
{
    // Initialize the resampler coefficient table off the render thread.
    // This is windowed-sinc interpolation adapted from the description at:
    // https://en.wikipedia.org/wiki/Sinc_interpolation
    // https://en.wikipedia.org/wiki/Window_function#Blackman_window
    prepare_sinc_resampler_kernel(m_resampler_table, 1.0);
}

void AudioBufferSourceRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    // https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
    m_output.zero();
    if (!m_has_start || m_finished)
        return;

    // If we don't have any buffer data, render silence.
    if (!m_buffer || m_channel_count == 0 || m_buffer->channel(0).is_empty())
        return;

    // playbackRate and detune are k-rate AudioParams in WebAudio.
    // For now, we treat any connected signal as a k-rate modulation sampled at the start of the quantum.
    if (param_inputs.size() > BufferSourceParamIndex::playback_rate)
        mix_inputs_into(m_playback_rate_input, param_inputs[BufferSourceParamIndex::playback_rate].span());
    else
        m_playback_rate_input.zero();

    if (param_inputs.size() > BufferSourceParamIndex::detune)
        mix_inputs_into(m_detune_input, param_inputs[BufferSourceParamIndex::detune].span());
    else
        m_detune_input.zero();

    bool const has_playback_rate_param_input = param_inputs.size() > BufferSourceParamIndex::playback_rate && !param_inputs[BufferSourceParamIndex::playback_rate].is_empty();
    bool const has_detune_param_input = param_inputs.size() > BufferSourceParamIndex::detune && !param_inputs[BufferSourceParamIndex::detune].is_empty();

    f32 effective_playback_rate = has_playback_rate_param_input ? m_playback_rate_input.channel(0)[0] : m_playback_rate;
    if (__builtin_isnan(effective_playback_rate) || !__builtin_isfinite(effective_playback_rate))
        effective_playback_rate = 0.0f;

    f32 effective_detune_cents = has_detune_param_input ? m_detune_input.channel(0)[0] : m_detune_cents;
    if (__builtin_isnan(effective_detune_cents) || !__builtin_isfinite(effective_detune_cents))
        effective_detune_cents = 0.0f;

    f64 const detune_multiplier = AK::exp2(static_cast<f64>(effective_detune_cents) / 1200.0);
    f64 const buffer_to_context_ratio = static_cast<f64>(m_sample_rate) / static_cast<f64>(context.sample_rate);
    f64 const increment = buffer_to_context_ratio * static_cast<f64>(effective_playback_rate) * detune_multiplier;

    // Update coefficient generation for downsampling ratios without allocating.
    prepare_sinc_resampler_kernel(m_resampler_table, increment);

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

    size_t const frames = m_output.frame_count();
    size_t const graph_start = context.current_frame;

    // Stop takes precedence within this quantum.
    if (m_stop_frame.has_value() && graph_start >= m_stop_frame.value()) {
        m_finished = true;
        return;
    }

    // Determine the earliest sample within this quantum that can produce output.
    size_t quantum_render_start = 0;
    if (!m_is_playing) {
        if (graph_start + frames <= m_start_frame)
            return;
        quantum_render_start = graph_start < m_start_frame ? m_start_frame - graph_start : 0;
    }

    size_t quantum_render_end = frames;
    if (m_stop_frame.has_value() && graph_start + frames > m_stop_frame.value())
        quantum_render_end = m_stop_frame.value() - graph_start;

    auto const& table = m_resampler_table;

    size_t const channels_to_render = min(m_channel_count, m_buffer->channel_count());

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

    auto wrap_playhead_if_needed = [&](f64& playhead) {
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

    auto should_stop_before_rendering = [&]() -> bool {
        if (m_duration_in_sample_frames.has_value()) {
            // Duration is in buffer sample frames; stop after consuming that much buffer timeline.
            return m_progress_in_sample_frames >= static_cast<f64>(m_duration_in_sample_frames.value());
        }

        if (m_loop && loop_length > 0)
            return false;

        if (increment >= 0.0)
            return m_playhead_in_sample_frames >= static_cast<f64>(buffer_length);
        return m_playhead_in_sample_frames < 0.0;
    };

    // Render sample-by-sample to support SRC and fractional playhead.
    for (size_t out_index = quantum_render_start; out_index < quantum_render_end; ++out_index) {
        size_t const graph_frame = graph_start + out_index;

        if (!m_is_playing) {
            // Start at the first sample >= start_frame.
            if (graph_frame < m_start_frame)
                continue;

            m_is_playing = true;
            m_finished = false;
            m_progress_in_sample_frames = 0.0;

            // Initialize playhead in buffer sample frames.
            // If we begin rendering after start_frame, advance by the elapsed context frames using the
            // current effective increment (k-rate parameters).
            f64 const elapsed_context_frames = static_cast<f64>(graph_frame - m_start_frame);
            m_playhead_in_sample_frames = static_cast<f64>(offset) + (elapsed_context_frames * increment);
            wrap_playhead_if_needed(m_playhead_in_sample_frames);
        }

        if (should_stop_before_rendering()) {
            m_finished = true;
            break;
        }

        // This is windowed-sinc interpolation adapted from the description at:
        // https://en.wikipedia.org/wiki/Sinc_interpolation
        // https://en.wikipedia.org/wiki/Window_function#Blackman_window
        //
        // Interpolate at the current playhead.
        f64 const playhead = m_playhead_in_sample_frames;
        f64 const base_index_d = AK::floor(playhead);
        f64 const frac = playhead - base_index_d; // in [0,1)

        // If the playhead is effectively on an integer sample frame, ideal sinc interpolation
        // reconstructs the original sample exactly. Special-case this to avoid tiny DC drift
        // from the truncated/windowed kernel and floating-point coefficient sums.
        constexpr f64 integer_snap_epsilon = 1e-12;
        bool const snapped_to_integer = (frac < integer_snap_epsilon) || ((1.0 - frac) < integer_snap_epsilon);
        long const snapped_index = static_cast<long>(base_index_d) + ((1.0 - frac) < integer_snap_epsilon ? 1 : 0);

        for (size_t ch = 0; ch < channels_to_render; ++ch) {
            auto src_span = m_buffer->channel(ch);

            if (snapped_to_integer) {
                m_output.channel(ch)[out_index] = sample_from_channel(src_span, playhead, snapped_index);
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

        wrap_playhead_if_needed(m_playhead_in_sample_frames);
    }
}

void AudioBufferSourceRenderNode::apply_description(GraphNodeDescription const& node)
{
    if (!node.has<AudioBufferSourceGraphNode>())
        return;

    auto const& desc = node.get<AudioBufferSourceGraphNode>();

    m_has_start = desc.start_frame.has_value();
    m_playback_rate = desc.playback_rate;
    m_detune_cents = desc.detune_cents;
    m_start_frame = desc.start_frame.value_or(0);
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

    // FIXME: Swapping the underlying PCM buffer here would require copying/resizing potentially large
    // vectors and changing the output channel count. For now, treat buffer changes as a rebuild concern.
    // Parameter updates still cover scheduling/looping behavior.
    if (desc.sample_rate != m_sample_rate || desc.channel_count != m_channel_count || desc.length_in_sample_frames != m_length_in_sample_frames)
        return;
}

}
