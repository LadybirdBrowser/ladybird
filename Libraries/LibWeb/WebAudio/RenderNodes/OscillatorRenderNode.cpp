/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/OscillatorRenderNode.h>

#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio::Render {

static f32 oscillator_sample_from_phase(OscillatorType type, f32 phase)
{
    f32 const two_pi = 2.0f * AK::Pi<f32>;

    switch (type) {
    case OscillatorType::Sine:
        return AK::sin(phase);
    case OscillatorType::Square:
        return phase < AK::Pi<f32> ? 1.0f : -1.0f;
    case OscillatorType::Sawtooth: {
        f32 const t = phase / two_pi;
        return (2.0f * t) - 1.0f;
    }
    case OscillatorType::Triangle: {
        f32 const t = phase / two_pi;
        return 1.0f - (4.0f * AK::fabs(t - 0.5f));
    }
    case OscillatorType::Custom:
        return 0.0f;
    }

    return 0.0f;
}

static void render_oscillator_mono_in_range(
    OscillatorType type,
    Span<f32> out,
    size_t render_start,
    size_t render_end,
    f32 sample_rate,
    f32 base_frequency_hz,
    f32 base_detune_cents,
    bool has_frequency_param_input,
    ReadonlySpan<f32> frequency_input_hz,
    bool has_detune_param_input,
    ReadonlySpan<f32> detune_input_cents,
    f32 amplitude,
    f32& in_out_phase)
{
    size_t const frames = out.size();
    if (frames == 0)
        return;
    if (sample_rate <= 0.0f || !__builtin_isfinite(sample_rate) || __builtin_isnan(sample_rate))
        return;
    if (render_start >= render_end)
        return;

    if (has_frequency_param_input)
        VERIFY(frequency_input_hz.size() >= frames);
    if (has_detune_param_input)
        VERIFY(detune_input_cents.size() >= frames);

    f32 const two_pi = 2.0f * AK::Pi<f32>;

    for (size_t i = render_start; i < render_end; ++i) {
        out[i] = oscillator_sample_from_phase(type, in_out_phase) * amplitude;

        // Detune is in cents. Effective frequency: f * 2^(detune/1200).
        f32 const frequency_hz = has_frequency_param_input ? frequency_input_hz[i] : base_frequency_hz;
        f32 const detune_cents = has_detune_param_input ? detune_input_cents[i] : base_detune_cents;
        f32 effective_frequency = frequency_hz * AK::exp2(detune_cents / 1200.0f);
        effective_frequency = max(effective_frequency, 0.0f);

        f32 const phase_increment = two_pi * effective_frequency / sample_rate;
        in_out_phase += phase_increment;
        if (in_out_phase >= two_pi)
            in_out_phase = AK::fmod(in_out_phase, two_pi);
    }
}

OscillatorRenderNode::OscillatorRenderNode(NodeID node_id, OscillatorGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_type(desc.type)
    , m_frequency(desc.frequency)
    , m_detune_cents(desc.detune_cents)
    , m_start_frame(desc.start_frame)
    , m_stop_frame(desc.stop_frame)
    , m_output(1, quantum_size)
    , m_frequency_input(1, quantum_size)
    , m_detune_input(1, quantum_size)
{
}

void OscillatorRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    // https://webaudio.github.io/web-audio-api/#OscillatorNode
    m_output.zero();

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

    if (!m_start_frame.has_value()) {
        if (should_log_this_quantum)
            WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start=none stop={} type={} freq={} output_peak=0 (not started)",
                node_id(), context.current_frame, context.sample_rate, m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency);
        return;
    }

    size_t const quantum_start = context.current_frame;
    size_t const frames = m_output.frame_count();

    // Determine active range within this quantum.
    size_t render_start = 0;
    if (quantum_start + frames <= m_start_frame.value()) {
        if (should_log_this_quantum)
            WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start={} stop={} type={} freq={} output_peak=0 (before start window)",
                node_id(), context.current_frame, context.sample_rate, static_cast<u64>(m_start_frame.value()), m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency);
        return;
    }
    if (quantum_start < m_start_frame.value())
        render_start = m_start_frame.value() - quantum_start;

    size_t render_end = frames;
    if (m_stop_frame.has_value()) {
        if (quantum_start >= m_stop_frame.value())
            return;
        if (quantum_start + frames > m_stop_frame.value())
            render_end = m_stop_frame.value() - quantum_start;
    }

    if (render_start >= render_end)
        return;

    if (param_inputs.size() > OscillatorParamIndex::frequency)
        mix_inputs_into(m_frequency_input, param_inputs[OscillatorParamIndex::frequency].span());
    else
        m_frequency_input.zero();

    if (param_inputs.size() > OscillatorParamIndex::detune)
        mix_inputs_into(m_detune_input, param_inputs[OscillatorParamIndex::detune].span());
    else
        m_detune_input.zero();

    auto out = m_output.channel(0);

    auto frequency_in = m_frequency_input.channel(0);
    auto detune_in = m_detune_input.channel(0);

    bool const has_frequency_param_input = param_inputs.size() > OscillatorParamIndex::frequency && !param_inputs[OscillatorParamIndex::frequency].is_empty();
    bool const has_detune_param_input = param_inputs.size() > OscillatorParamIndex::detune && !param_inputs[OscillatorParamIndex::detune].is_empty();

    if (m_type == OscillatorType::Custom) {
        if (should_log_this_quantum)
            WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start={} stop={} type={} freq={} output_peak=0 (unsupported type)",
                node_id(), context.current_frame, context.sample_rate, static_cast<u64>(m_start_frame.value()), m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency);
        return;
    }

    render_oscillator_mono_in_range(
        m_type,
        out,
        render_start,
        render_end,
        context.sample_rate,
        m_frequency,
        m_detune_cents,
        has_frequency_param_input,
        frequency_in,
        has_detune_param_input,
        detune_in,
        1.0f,
        m_phase);

    if (should_log_this_quantum) {
        f32 peak = 0.0f;
        for (auto sample : out)
            peak = max(peak, AK::fabs(sample));
        WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start={} stop={} type={} freq={} output_peak={:.6f}",
            node_id(), context.current_frame, context.sample_rate, static_cast<u64>(m_start_frame.value()), m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency, peak);
    }
}

void OscillatorRenderNode::apply_description(GraphNodeDescription const& node)
{
    if (!node.has<OscillatorGraphNode>())
        return;

    auto const& desc = node.get<OscillatorGraphNode>();
    m_type = desc.type;
    m_frequency = desc.frequency;
    m_detune_cents = desc.detune_cents;
    m_start_frame = desc.start_frame;
    m_stop_frame = desc.stop_frame;
}

}
