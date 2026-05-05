/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibWebAudio/Debug.h>
#include <LibWebAudio/Engine/Mixing.h>
#include <LibWebAudio/RenderNodes/AudioBufferSourceRenderNode.h>

namespace Web::WebAudio::Render {

namespace {

constexpr f64 start_time_snap_epsilon = 1e-9;
constexpr f64 integer_snap_epsilon = 1e-9;
constexpr f64 playhead_snap_epsilon = 1e-9;
constexpr f64 k_rate_sanitize_scale = 1'000'000.0;

struct LoopBoundaries {
    bool enabled { false };
    size_t start { 0 };
    size_t end { 0 };
    size_t length { 0 };
};

f64 sanitize_k_rate_value(f64 value)
{
    if (__builtin_isnan(value) || !__builtin_isfinite(value))
        return 0.0;
    return AK::floor((value * k_rate_sanitize_scale) + 0.5) / k_rate_sanitize_scale;
}

void mix_param_input_if_present(AudioBus& destination, Vector<Vector<AudioBus const*>> const& param_inputs,
    size_t param_index)
{
    destination.zero();
    if (param_inputs.size() > param_index)
        mix_inputs_into(destination, param_inputs[param_index].span());
}

bool has_param_input(Vector<Vector<AudioBus const*>> const& param_inputs, size_t param_index)
{
    return param_inputs.size() > param_index && !param_inputs[param_index].is_empty();
}

LoopBoundaries compute_loop_boundaries(bool loop, size_t requested_start, size_t requested_end,
    size_t buffer_length)
{
    if (!loop || buffer_length == 0)
        return {};

    size_t loop_start = min(requested_start, buffer_length);
    size_t loop_end = requested_end == 0 ? buffer_length : min(requested_end, buffer_length);
    if (loop_end <= loop_start) {
        loop_start = 0;
        loop_end = buffer_length;
    }

    size_t const loop_length = loop_end - loop_start;
    if (loop_length == 0)
        return {};

    return {
        .enabled = true,
        .start = loop_start,
        .end = loop_end,
        .length = loop_length,
    };
}

size_t resolve_start_frame(size_t start_frame, Optional<f64> exact_start_time)
{
    if (!exact_start_time.has_value())
        return start_frame;

    f64 const start_time_in_context_frames = exact_start_time.value();
    if (!__builtin_isfinite(start_time_in_context_frames))
        return start_time_in_context_frames > 0.0 ? AK::NumericLimits<size_t>::max() : 0;
    if (start_time_in_context_frames <= 0.0)
        return 0;

    f64 const nearest = AK::round(start_time_in_context_frames);
    if (AK::fabs(start_time_in_context_frames - nearest) <= start_time_snap_epsilon)
        return static_cast<size_t>(nearest);

    f64 const rounded = AK::ceil(start_time_in_context_frames);
    if (rounded >= static_cast<f64>(AK::NumericLimits<size_t>::max()))
        return AK::NumericLimits<size_t>::max();
    return static_cast<size_t>(rounded);
}

size_t resolve_quantum_render_start(bool is_playing, size_t graph_start, size_t quantum_size,
    size_t start_frame)
{
    if (is_playing)
        return 0;
    if (graph_start + quantum_size <= start_frame)
        return quantum_size;
    return graph_start < start_frame ? start_frame - graph_start : 0;
}

size_t resolve_quantum_render_end(size_t graph_start, size_t quantum_size, Optional<size_t> stop_frame)
{
    if (!stop_frame.has_value() || graph_start + quantum_size <= stop_frame.value())
        return quantum_size;
    return stop_frame.value() - graph_start;
}

Optional<size_t> effective_duration_in_sample_frames(Optional<size_t> requested_duration, bool loop_enabled,
    size_t offset, size_t buffer_length)
{
    if (!requested_duration.has_value() || loop_enabled)
        return requested_duration;

    size_t const max_duration = buffer_length > offset ? buffer_length - offset : 0;
    return min(requested_duration.value(), max_duration);
}

bool should_stop_before_rendering(Optional<size_t> effective_duration, bool loop_enabled,
    size_t buffer_length, f64 playhead_in_sample_frames,
    f64 progress_in_sample_frames, f64 increment)
{
    if (effective_duration.has_value())
        return progress_in_sample_frames >= static_cast<f64>(effective_duration.value());
    if (loop_enabled)
        return false;
    if (increment >= 0.0)
        return (playhead_in_sample_frames + playhead_snap_epsilon) >= static_cast<f64>(buffer_length);
    return playhead_in_sample_frames <= -playhead_snap_epsilon;
}

void wrap_playhead_if_needed(f64& playhead_in_sample_frames, f64 increment, bool loop_enabled,
    size_t loop_start, size_t loop_end, size_t loop_length)
{
    if (!loop_enabled || loop_length == 0)
        return;

    if (increment > 0.0) {
        if (playhead_in_sample_frames >= static_cast<f64>(loop_end)) {
            f64 const start = static_cast<f64>(loop_start);
            f64 const length = static_cast<f64>(loop_length);
            f64 relative_position = playhead_in_sample_frames - start;
            relative_position = AK::fmod(relative_position, length);
            if (relative_position < 0.0)
                relative_position += length;
            playhead_in_sample_frames = start + relative_position;
        }
        return;
    }

    if (increment < 0.0 && playhead_in_sample_frames < static_cast<f64>(loop_start)) {
        f64 const start = static_cast<f64>(loop_start);
        f64 const length = static_cast<f64>(loop_length);
        f64 relative_position = playhead_in_sample_frames - start;
        relative_position = AK::fmod(relative_position, length);
        if (relative_position < 0.0)
            relative_position += length;
        playhead_in_sample_frames = start + relative_position;
    }
}

f32 sample_from_channel(ReadonlySpan<f32> channel, f64 playhead_in_sample_frames, long sample_index,
    bool loop_enabled, size_t loop_start, size_t loop_end, size_t loop_length)
{
    if (loop_enabled && loop_length > 0) {
        if (playhead_in_sample_frames >= static_cast<f64>(loop_start) && playhead_in_sample_frames < static_cast<f64>(loop_end)) {
            long const loop_start_index = static_cast<long>(loop_start);
            long const loop_length_in_samples = static_cast<long>(loop_length);
            long relative_index = sample_index - loop_start_index;
            relative_index %= loop_length_in_samples;
            if (relative_index < 0)
                relative_index += loop_length_in_samples;
            sample_index = loop_start_index + relative_index;
        }
    }

    if (channel.is_empty())
        return 0.0f;
    if (channel.size() == 1)
        return channel[0];

    if (sample_index < 0) {
        f32 const slope = channel[1] - channel[0];
        return channel[0] + (static_cast<f32>(sample_index) * slope);
    }

    size_t const index = static_cast<size_t>(sample_index);
    if (index >= channel.size()) {
        f32 const slope = channel[channel.size() - 1] - channel[channel.size() - 2];
        f32 const delta = static_cast<f32>(sample_index - static_cast<long>(channel.size() - 1));
        return channel[channel.size() - 1] + (delta * slope);
    }

    return channel[index];
}

bool position_is_exact_sample_frame(f64 playhead_in_sample_frames, long& sample_frame)
{
    f64 const floored_position = AK::floor(playhead_in_sample_frames);
    f64 const fractional_position = playhead_in_sample_frames - floored_position;
    if (fractional_position < integer_snap_epsilon) {
        sample_frame = static_cast<long>(floored_position);
        return true;
    }
    if ((1.0 - fractional_position) < integer_snap_epsilon) {
        sample_frame = static_cast<long>(floored_position) + 1;
        return true;
    }
    return false;
}

f64 start_playhead_for_sample(size_t offset, size_t graph_frame, Optional<f64> exact_start_time,
    f64 increment)
{
    // https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode-start
    // The when parameter describes at what time (in seconds) the sound should start playing.
    // If 0 is passed in for this value or if the value is less than currentTime, then the sound will start
    // playing immediately.
    if (!exact_start_time.has_value())
        return static_cast<f64>(offset);

    f64 const start_time_in_context_frames = exact_start_time.value();
    f64 const elapsed_context_frames = max(0.0, static_cast<f64>(graph_frame) - start_time_in_context_frames);
    return static_cast<f64>(offset) + (elapsed_context_frames * increment);
}

} // namespace

AudioBufferSourceRenderNode::AudioBufferSourceRenderNode(NodeID node_id,
    AudioBufferSourceGraphNode const& desc,
    RefPtr<SharedAudioBuffer> buffer,
    size_t quantum_size)
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
    prepare_sinc_resampler_kernel(m_resampler_table, 1.0);
}

void AudioBufferSourceRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&,
    Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    m_output.zero();
    m_output.set_channel_count(0);

    if (!m_has_start || m_finished || !m_buffer || m_channel_count == 0 || m_buffer->channel(0).is_empty()) {

        return;
    }

    mix_param_input_if_present(m_playback_rate_input, param_inputs, AudioBufferSourceParamIndex::playback_rate);
    mix_param_input_if_present(m_detune_input, param_inputs, AudioBufferSourceParamIndex::detune);

    bool const has_playback_rate_input = has_param_input(param_inputs, AudioBufferSourceParamIndex::playback_rate);
    bool const has_detune_input = has_param_input(param_inputs, AudioBufferSourceParamIndex::detune);

    f64 const buffer_to_context_ratio = static_cast<f64>(m_sample_rate) / static_cast<f64>(context.sample_rate);
    f64 const initial_increment = increment_for_sample(0, buffer_to_context_ratio, has_playback_rate_input, has_detune_input);
    if (buffer_to_context_ratio != 1.0) {
        bool const should_rebuild_kernel = !m_last_resampler_increment.has_value() || AK::fabs(m_last_resampler_increment.value() - initial_increment) > 1e-12;
        if (should_rebuild_kernel) {
            prepare_sinc_resampler_kernel(m_resampler_table, initial_increment);
            m_last_resampler_increment = initial_increment;
        }
    }

    size_t const buffer_length = m_length_in_sample_frames;
    if (buffer_length == 0)
        return;

    size_t const offset = min(m_offset_frame, buffer_length);
    LoopBoundaries const loop_boundaries = compute_loop_boundaries(m_loop, m_loop_start_frame, m_loop_end_frame, buffer_length);

    size_t const channels_to_render = min(m_channel_count, m_buffer->channel_count());
    if (channels_to_render == 0)
        return;

    size_t const quantum_size = m_output.frame_count();
    size_t const graph_start = context.current_frame;
    size_t const start_frame = resolve_start_frame(m_start_frame, m_start_time_in_context_frames);

    if (m_stop_frame.has_value() && graph_start >= m_stop_frame.value()) {
        m_finished = true;
        return;
    }

    size_t const quantum_render_start = resolve_quantum_render_start(m_is_playing, graph_start, quantum_size, start_frame);
    size_t const quantum_render_end = resolve_quantum_render_end(graph_start, quantum_size, m_stop_frame);
    if (quantum_render_start >= quantum_render_end)
        return;

    m_output.set_channel_count(channels_to_render);

    Optional<size_t> const effective_duration = effective_duration_in_sample_frames(
        m_duration_in_sample_frames, loop_boundaries.enabled, offset, buffer_length);
    render_samples(context, graph_start, quantum_render_start, quantum_render_end, offset, channels_to_render,
        buffer_length, loop_boundaries.enabled, loop_boundaries.start, loop_boundaries.end,
        loop_boundaries.length, effective_duration, buffer_to_context_ratio, has_playback_rate_input,
        has_detune_input);
}

f64 AudioBufferSourceRenderNode::increment_for_sample(size_t sample_index, f64 buffer_to_context_ratio,
    bool has_playback_rate_input,
    bool has_detune_input) const
{
    f64 playback_rate = sanitize_k_rate_value(static_cast<f64>(m_playback_rate));
    if (has_playback_rate_input)
        playback_rate = sanitize_k_rate_value(static_cast<f64>(m_playback_rate_input.channel(0)[sample_index]));

    f64 detune_cents = sanitize_k_rate_value(static_cast<f64>(m_detune_cents));
    if (has_detune_input)
        detune_cents = sanitize_k_rate_value(static_cast<f64>(m_detune_input.channel(0)[sample_index]));

    f64 const detune_multiplier = AK::exp2(detune_cents / 1200.0);
    return buffer_to_context_ratio * playback_rate * detune_multiplier;
}

void AudioBufferSourceRenderNode::render_samples(
    RenderContext const&, size_t graph_start, size_t quantum_render_start, size_t quantum_render_end,
    size_t offset, size_t channels_to_render, size_t buffer_length, bool loop_enabled, size_t loop_start,
    size_t loop_end, size_t loop_length, Optional<size_t> effective_duration, f64 buffer_to_context_ratio,
    bool has_playback_rate_input, bool has_detune_input)
{
    SincResamplerKernel const& resampler_table = m_resampler_table;

    for (size_t output_index = quantum_render_start; output_index < quantum_render_end; ++output_index) {
        size_t const graph_frame = graph_start + output_index;
        f64 const increment = increment_for_sample(output_index, buffer_to_context_ratio, has_playback_rate_input,
            has_detune_input);

        if (!m_is_playing) {
            m_is_playing = true;
            m_finished = false;
            m_progress_in_sample_frames = 0.0;
            m_playhead_in_sample_frames = start_playhead_for_sample(offset, graph_frame, m_start_time_in_context_frames, increment);
            wrap_playhead_if_needed(m_playhead_in_sample_frames, increment, loop_enabled, loop_start, loop_end,
                loop_length);
        }

        if (should_stop_before_rendering(effective_duration, loop_enabled, buffer_length,
                m_playhead_in_sample_frames, m_progress_in_sample_frames, increment)) {
            m_finished = true;
            break;
        }

        f64 const playhead_in_sample_frames = m_playhead_in_sample_frames;
        f64 const floored_playhead = AK::floor(playhead_in_sample_frames);
        f64 const fractional_playhead = playhead_in_sample_frames - floored_playhead;

        long exact_sample_frame = 0;
        bool const is_exact_sample_frame = position_is_exact_sample_frame(playhead_in_sample_frames, exact_sample_frame);

        for (size_t channel_index = 0; channel_index < channels_to_render; ++channel_index) {
            ReadonlySpan<f32> const channel = m_buffer->channel(channel_index);

            // https://webaudio.github.io/web-audio-api/#playback-AudioBufferSourceNode
            // If position corresponds to the location of an exact sample frame in the buffer, this function returns
            // that frame. Otherwise, its return value is determined by a UA-supplied algorithm that interpolates
            // sample frames in the neighborhood of position.
            if (is_exact_sample_frame) {
                m_output.channel(channel_index)[output_index] = sample_from_channel(channel, playhead_in_sample_frames, exact_sample_frame, loop_enabled,
                    loop_start, loop_end, loop_length);
                continue;
            }

            if (buffer_to_context_ratio == 1.0) {
                long const base_sample_frame = static_cast<long>(floored_playhead);
                f64 const first_sample = static_cast<f64>(sample_from_channel(channel, playhead_in_sample_frames, base_sample_frame,
                    loop_enabled, loop_start, loop_end, loop_length));
                f64 const second_sample = static_cast<f64>(sample_from_channel(channel, playhead_in_sample_frames, base_sample_frame + 1,
                    loop_enabled, loop_start, loop_end, loop_length));
                m_output.channel(channel_index)[output_index] = static_cast<f32>(first_sample + ((second_sample - first_sample) * fractional_playhead));
                continue;
            }

            auto sample_at = [&](size_t, i64 sample_index) -> f32 {
                return sample_from_channel(channel, playhead_in_sample_frames, static_cast<long>(sample_index),
                    loop_enabled, loop_start, loop_end, loop_length);
            };
            m_output.channel(channel_index)[output_index] = sinc_resampler_interpolate_at(resampler_table, playhead_in_sample_frames, 0, sample_at);
        }

        m_playhead_in_sample_frames += increment;
        m_progress_in_sample_frames += AK::fabs(increment);
        wrap_playhead_if_needed(m_playhead_in_sample_frames, increment, loop_enabled, loop_start, loop_end,
            loop_length);
    }
}

void AudioBufferSourceRenderNode::schedule_start(Optional<size_t> start_frame)
{
    m_start_frame = start_frame.value_or(0);
    m_has_start = start_frame.has_value();
    m_start_time_in_context_frames = {};
    m_is_playing = false;
    m_finished = false;
    m_playhead_in_sample_frames = 0.0;
    m_progress_in_sample_frames = 0.0;
}

void AudioBufferSourceRenderNode::schedule_stop(Optional<size_t> stop_frame) { m_stop_frame = stop_frame; }

void AudioBufferSourceRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<AudioBufferSourceGraphNode>())
        return;

    AudioBufferSourceGraphNode const& desc = node.get<AudioBufferSourceGraphNode>();

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

} // namespace Web::WebAudio::Render
