/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/DelayRenderNode.h>

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>

namespace Web::WebAudio::Render {

DelayRenderNode::DelayRenderNode(NodeID node_id, DelayGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_delay_time_seconds(desc.delay_time_seconds)
    , m_max_delay_time_seconds(max(0.0f, desc.max_delay_time_seconds))
    , m_channel_count(max(1u, desc.channel_count))
    , m_output(m_channel_count, quantum_size)
    , m_delay_time_input(1, quantum_size)
{
}

void DelayRenderNode::ensure_buffer_capacity(RenderContext const& context)
{
    ASSERT_RENDER_THREAD();
    // This is a circular buffer adapted from the description at:
    // https://en.wikipedia.org/wiki/Circular_buffer

    // Allocate ring buffer based on max delay in frames (+2 for interpolation safety).
    f64 const max_delay_frames_d = max(0.0, static_cast<f64>(m_max_delay_time_seconds) * static_cast<f64>(context.sample_rate));
    size_t const max_delay_frames = static_cast<size_t>(AK::ceil(max_delay_frames_d));
    size_t const desired_ring_size = max_delay_frames + 2;

    if (desired_ring_size == 0 || desired_ring_size == m_ring_size)
        return;

    m_ring_size = desired_ring_size;
    m_write_index = 0;
    m_frames_written = 0;

    m_ring.resize(m_channel_count);
    for (size_t ch = 0; ch < m_channel_count; ++ch) {
        m_ring[ch].resize(m_ring_size);
        m_ring[ch].fill(0.0f);
    }
}

void DelayRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#DelayNode
    // Keep channel count in sync.
    // This is a delay line adapted from the description at:
    // https://en.wikipedia.org/wiki/Delay_line

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const mixed_input_channels = mixed_input ? mixed_input->channel_count() : 1;
    m_last_input_channels = max<size_t>(1, mixed_input_channels);

    ensure_buffer_capacity(context);

    // Audio-rate input to delayTime AudioParam.
    if (param_inputs.size() > DelayParamIndex::delay_time)
        mix_inputs_into(m_delay_time_input, param_inputs[DelayParamIndex::delay_time].span());
    else
        m_delay_time_input.zero();

    bool const has_delay_time_param_input = param_inputs.size() > DelayParamIndex::delay_time && !param_inputs[DelayParamIndex::delay_time].is_empty();

    size_t const frames = m_output.frame_count();
    auto const& delay_time_in = m_delay_time_input.channel(0);

    // If the entire quantum reads from the unfilled history, output should be mono silence.
    // Otherwise, the output channel count tracks the input.
    f32 min_delay_seconds = has_delay_time_param_input ? delay_time_in[0] : m_delay_time_seconds;
    if (has_delay_time_param_input) {
        for (size_t i = 1; i < frames; ++i)
            min_delay_seconds = min(min_delay_seconds, delay_time_in[i]);
    }
    if (!__builtin_isfinite(min_delay_seconds) || __builtin_isnan(min_delay_seconds))
        min_delay_seconds = 0.0f;
    min_delay_seconds = clamp(min_delay_seconds, 0.0f, m_max_delay_time_seconds);

    f64 const min_delay_frames = static_cast<f64>(min_delay_seconds) * static_cast<f64>(context.sample_rate);
    bool const unfilled_for_entire_quantum = min_delay_frames > static_cast<f64>(m_frames_written + frames - 1);
    size_t const output_channels_this_quantum = unfilled_for_entire_quantum ? 1 : mixed_input_channels;
    m_output.set_channel_count(output_channels_this_quantum);

    // Throttled debug logging (render-thread safe).
    // WEBAUDIO_NODE_LOG=1 enables these logs.
    static Atomic<i64> s_last_log_ms { 0 };
    bool const should_log_this_quantum = [&] {
        if (!::Web::WebAudio::should_log_nodes())
            return false;
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) < 250)
            return false;
        return s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed);
    }();

    if (should_log_this_quantum) {
        size_t connection_count = 0;
        if (!inputs.is_empty() && inputs[0].size() >= 1)
            connection_count = inputs[0].size() - 1;
        size_t input_channels = 0;
        if (mixed_input)
            input_channels = mixed_input->channel_count();
        WA_NODE_DBGLN("[WebAudio][DelayNode:{}] frames={} sr={} out_ch={} node_ch={} ring_size={} frames_written={} max_delay={} base_delay={} has_param_input={} connections={} mixed0_ch={}",
            node_id(), frames, context.sample_rate, m_output.channel_count(), m_channel_count, m_ring_size, m_frames_written, m_max_delay_time_seconds, m_delay_time_seconds, has_delay_time_param_input, connection_count, input_channels);
    }

    for (size_t i = 0; i < frames; ++i) {
        f32 delay_seconds = has_delay_time_param_input ? delay_time_in[i] : m_delay_time_seconds;
        if (!__builtin_isfinite(delay_seconds) || __builtin_isnan(delay_seconds))
            delay_seconds = 0.0f;

        delay_seconds = clamp(delay_seconds, 0.0f, m_max_delay_time_seconds);

        f64 delay_frames_d = static_cast<f64>(delay_seconds) * static_cast<f64>(context.sample_rate);

        f64 read_pos = static_cast<f64>(m_write_index) - delay_frames_d;
        while (read_pos < 0.0)
            read_pos += static_cast<f64>(m_ring_size);
        while (read_pos >= static_cast<f64>(m_ring_size))
            read_pos -= static_cast<f64>(m_ring_size);

        size_t const idx0 = static_cast<size_t>(AK::floor(read_pos));
        size_t const idx1 = (idx0 + 1) % m_ring_size;
        f32 const frac = static_cast<f32>(read_pos - static_cast<f64>(idx0));

        // Write current input sample for all configured channels so that when the delay line
        // becomes filled, we can output the full channel set.
        for (size_t ch = 0; ch < m_channel_count; ++ch) {
            f32 sample = 0.0f;
            if (mixed_input && ch < mixed_input_channels)
                sample = mixed_input->channel(ch)[i];
            m_ring[ch][m_write_index] = sample;
        }

        // Read delayed sample with linear interpolation for the channels this quantum exposes.
        // This is linear interpolation adapted from the definition at:
        // https://en.wikipedia.org/wiki/Linear_interpolation
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            f32 const s0 = m_ring[ch][idx0];
            f32 const s1 = m_ring[ch][idx1];
            m_output.channel(ch)[i] = s0 + ((s1 - s0) * frac);
        }

        if (should_log_this_quantum && i < 4) {
            f32 in0 = (mixed_input && mixed_input_channels > 0) ? mixed_input->channel(0)[i] : 0.0f;
            f32 out0 = m_output.channel(0)[i];
            WA_NODE_DBGLN("[WebAudio][DelayNode:{}] i={} delay_s={} delay_frames={} write={} read_pos={} idx0={} idx1={} frac={} in0={} out0={}",
                node_id(), i, delay_seconds, delay_frames_d, m_write_index, read_pos, idx0, idx1, frac, in0, out0);
        }

        m_write_index = (m_write_index + 1) % m_ring_size;
        if (m_frames_written < m_ring_size)
            ++m_frames_written;
    }
}

void DelayRenderNode::process_cycle_writer(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs)
{
    ASSERT_RENDER_THREAD();

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const mixed_input_channels = mixed_input ? mixed_input->channel_count() : 1;
    m_last_input_channels = max<size_t>(1, mixed_input_channels);

    ensure_buffer_capacity(context);

    size_t const frames = m_output.frame_count();
    for (size_t i = 0; i < frames; ++i) {
        for (size_t ch = 0; ch < m_channel_count; ++ch) {
            f32 sample = 0.0f;
            if (mixed_input && ch < mixed_input_channels)
                sample = mixed_input->channel(ch)[i];
            m_ring[ch][m_write_index] = sample;
        }

        m_write_index = (m_write_index + 1) % m_ring_size;
        if (m_frames_written < m_ring_size)
            ++m_frames_written;
    }
}

void DelayRenderNode::process_cycle_reader(RenderContext& context, Vector<Vector<AudioBus const*>> const& param_inputs, bool clamp_to_quantum)
{
    ASSERT_RENDER_THREAD();

    ensure_buffer_capacity(context);

    if (param_inputs.size() > DelayParamIndex::delay_time)
        mix_inputs_into(m_delay_time_input, param_inputs[DelayParamIndex::delay_time].span());
    else
        m_delay_time_input.zero();

    bool const has_delay_time_param_input = param_inputs.size() > DelayParamIndex::delay_time && !param_inputs[DelayParamIndex::delay_time].is_empty();

    size_t const frames = m_output.frame_count();
    auto const& delay_time_in = m_delay_time_input.channel(0);

    f32 min_delay_seconds = has_delay_time_param_input ? delay_time_in[0] : m_delay_time_seconds;
    if (has_delay_time_param_input) {
        for (size_t i = 1; i < frames; ++i)
            min_delay_seconds = min(min_delay_seconds, delay_time_in[i]);
    }
    if (!__builtin_isfinite(min_delay_seconds) || __builtin_isnan(min_delay_seconds))
        min_delay_seconds = 0.0f;

    f32 const quantum_min_delay = clamp_to_quantum ? static_cast<f32>(context.quantum_size) / context.sample_rate : 0.0f;
    min_delay_seconds = clamp(min_delay_seconds, quantum_min_delay, m_max_delay_time_seconds);

    f64 const min_delay_frames = static_cast<f64>(min_delay_seconds) * static_cast<f64>(context.sample_rate);
    bool const unfilled_for_entire_quantum = min_delay_frames > static_cast<f64>(m_frames_written + frames - 1);
    size_t const output_channels_this_quantum = unfilled_for_entire_quantum ? 1 : m_last_input_channels;
    m_output.set_channel_count(output_channels_this_quantum);

    for (size_t i = 0; i < frames; ++i) {
        f32 delay_seconds = has_delay_time_param_input ? delay_time_in[i] : m_delay_time_seconds;
        if (!__builtin_isfinite(delay_seconds) || __builtin_isnan(delay_seconds))
            delay_seconds = 0.0f;

        delay_seconds = clamp(delay_seconds, quantum_min_delay, m_max_delay_time_seconds);

        f64 delay_frames_d = static_cast<f64>(delay_seconds) * static_cast<f64>(context.sample_rate);

        f64 read_pos = static_cast<f64>(m_write_index) - delay_frames_d;
        while (read_pos < 0.0)
            read_pos += static_cast<f64>(m_ring_size);
        while (read_pos >= static_cast<f64>(m_ring_size))
            read_pos -= static_cast<f64>(m_ring_size);

        size_t const idx0 = static_cast<size_t>(AK::floor(read_pos));
        size_t const idx1 = (idx0 + 1) % m_ring_size;
        f32 const frac = static_cast<f32>(read_pos - static_cast<f64>(idx0));

        // Read delayed sample with linear interpolation for the channels this quantum exposes.
        // This is linear interpolation adapted from the definition at:
        // https://en.wikipedia.org/wiki/Linear_interpolation
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            f32 const s0 = m_ring[ch][idx0];
            f32 const s1 = m_ring[ch][idx1];
            m_output.channel(ch)[i] = s0 + ((s1 - s0) * frac);
        }
    }
}

void DelayRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<DelayGraphNode>())
        return;

    auto const& desc = node.get<DelayGraphNode>();

    // If max delay changes, the graph should have been rebuilt.
    m_delay_time_seconds = desc.delay_time_seconds;
    m_max_delay_time_seconds = max(0.0f, desc.max_delay_time_seconds);

    size_t const new_channel_count = max(1u, desc.channel_count);
    if (new_channel_count != m_channel_count) {
        m_channel_count = new_channel_count;
        m_last_input_channels = min(m_last_input_channels, m_channel_count);
        // m_output channel count is computed per quantum.
        m_ring_size = 0;
        m_frames_written = 0;
        m_ring.clear();
    }
}

}
