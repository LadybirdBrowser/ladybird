/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/OscillatorRenderNode.h>

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio::Render {

static f32 oscillator_sample_from_phase(OscillatorType type, f64 phase)
{
    f64 const two_pi = 2.0 * AK::Pi<f64>;

    switch (type) {
    case OscillatorType::Sine:
        return static_cast<f32>(AK::sin(phase));
    case OscillatorType::Square:
        return phase < AK::Pi<f64> ? 1.0f : -1.0f;
    case OscillatorType::Sawtooth: {
        f64 const t = phase / two_pi;
        return static_cast<f32>((2.0 * t) - 1.0);
    }
    case OscillatorType::Triangle: {
        f64 const t = phase / two_pi;
        return static_cast<f32>(1.0 - (4.0 * AK::fabs(t - 0.5)));
    }
    case OscillatorType::Custom:
        return 0.0f;
    }

    return 0.0f;
}

static f32 oscillator_sample_from_custom(PeriodicWaveCoefficients const& coeffs, f32 phase, f32 normalization_gain)
{
    // Sum DC + harmonics using stored real/imag coefficients.
    size_t const harmonic_count = min(coeffs.real.size(), coeffs.imag.size());
    f32 sample = 0.0f;
    for (size_t harmonic = 0; harmonic < harmonic_count; ++harmonic) {
        f32 harmonic_phase = phase * static_cast<f32>(harmonic);
        sample += coeffs.real[harmonic] * AK::cos(harmonic_phase) + coeffs.imag[harmonic] * AK::sin(harmonic_phase);
    }
    return sample * normalization_gain;
}

static f32 normalization_gain_for(PeriodicWaveCoefficients const& coeffs)
{
    if (!coeffs.normalize)
        return 1.0f;

    size_t const harmonic_count = min(coeffs.real.size(), coeffs.imag.size());
    f32 max_magnitude = 0.0f;
    for (size_t harmonic = 0; harmonic < harmonic_count; ++harmonic) {
        f32 magnitude = AK::sqrt((coeffs.real[harmonic] * coeffs.real[harmonic]) + (coeffs.imag[harmonic] * coeffs.imag[harmonic]));
        max_magnitude = max(max_magnitude, magnitude);
    }

    if (max_magnitude <= AK::NumericLimits<f32>::epsilon())
        return 1.0f;
    return 1.0f / max_magnitude;
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
    f64& in_out_phase)
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

    f64 const two_pi = 2.0 * AK::Pi<f64>;

    f64 const nyquist = sample_rate * 0.5;

    for (size_t i = render_start; i < render_end; ++i) {
        // Detune is in cents. Effective frequency: f * 2^(detune/1200).
        f64 const frequency_hz = static_cast<f64>(has_frequency_param_input ? frequency_input_hz[i] : base_frequency_hz);
        f64 const detune_cents = static_cast<f64>(has_detune_param_input ? detune_input_cents[i] : base_detune_cents);
        f64 effective_frequency = frequency_hz * AK::exp2(detune_cents / 1200.0);

        // [from-spec] Oscillator output is silent at or above Nyquist.
        if (AK::fabs(effective_frequency) >= nyquist || !__builtin_isfinite(effective_frequency)) {
            out[i] = 0.0f;
            continue;
        }

        out[i] = oscillator_sample_from_phase(type, in_out_phase) * amplitude;

        // [from-spec] The instantaneous phase is the time integral of computed frequency,
        // with phase angle zero at the exact start time.
        f64 const phase_increment = two_pi * effective_frequency / sample_rate;
        in_out_phase += phase_increment;
        if (in_out_phase >= two_pi || in_out_phase < 0.0) {
            in_out_phase = AK::fmod(in_out_phase, two_pi);
            if (in_out_phase < 0.0)
                in_out_phase += two_pi;
        }
    }
}

static void render_custom_oscillator_mono_in_range(
    PeriodicWaveCoefficients const& coeffs,
    f32 normalization_gain,
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
    f64& in_out_phase)
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

    f64 const two_pi = 2.0 * AK::Pi<f64>;
    f64 const nyquist = sample_rate * 0.5;

    for (size_t i = render_start; i < render_end; ++i) {
        f64 const frequency_hz = has_frequency_param_input ? frequency_input_hz[i] : base_frequency_hz;
        f64 const detune_cents = has_detune_param_input ? detune_input_cents[i] : base_detune_cents;
        f64 effective_frequency = frequency_hz * AK::exp2(detune_cents / 1200.0);

        if (AK::fabs(effective_frequency) >= nyquist || !__builtin_isfinite(effective_frequency)) {
            out[i] = 0.0f;
            continue;
        }

        out[i] = oscillator_sample_from_custom(coeffs, in_out_phase, normalization_gain) * amplitude;

        f64 const phase_increment = two_pi * effective_frequency / sample_rate;
        in_out_phase += phase_increment;
        if (in_out_phase >= two_pi || in_out_phase < 0.0) {
            in_out_phase = AK::fmod(in_out_phase, two_pi);
            if (in_out_phase < 0.0)
                in_out_phase += two_pi;
        }
    }
}

OscillatorRenderNode::OscillatorRenderNode(NodeID node_id, OscillatorGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_type(desc.type)
    , m_frequency(desc.frequency)
    , m_detune_cents(desc.detune_cents)
    , m_start_frame(desc.start_frame)
    , m_stop_frame(desc.stop_frame)
    , m_periodic_wave(desc.periodic_wave)
    , m_custom_normalization_gain(desc.periodic_wave.has_value() ? normalization_gain_for(desc.periodic_wave.value()) : 1.0f)
    , m_output(1, quantum_size)
    , m_frequency_input(1, quantum_size)
    , m_detune_input(1, quantum_size)
{
}

void OscillatorRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
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
        m_output.set_channel_count(0);
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
        m_output.set_channel_count(0);
        if (should_log_this_quantum)
            WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start={} stop={} type={} freq={} output_peak=0 (before start window)",
                node_id(), context.current_frame, context.sample_rate, static_cast<u64>(m_start_frame.value()), m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency);
        return;
    }
    if (quantum_start < m_start_frame.value())
        render_start = m_start_frame.value() - quantum_start;

    size_t render_end = frames;
    if (m_stop_frame.has_value()) {
        if (quantum_start >= m_stop_frame.value()) {
            m_output.set_channel_count(0);
            return;
        }
        if (quantum_start + frames > m_stop_frame.value())
            render_end = m_stop_frame.value() - quantum_start;
    }

    if (render_start >= render_end) {
        m_output.set_channel_count(0);
        return;
    }

    // Oscillator output is mono when active and has no output channels when inactive for this quantum.
    m_output.set_channel_count(1);

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
        if (!m_periodic_wave.has_value()) {
            if (should_log_this_quantum)
                WA_NODE_DBGLN("[WebAudio][OscillatorNode:{}] frame={} sr={} start={} stop={} type={} freq={} output_peak=0 (missing periodic wave)",
                    node_id(), context.current_frame, context.sample_rate, static_cast<u64>(m_start_frame.value()), m_stop_frame.has_value() ? static_cast<u64>(m_stop_frame.value()) : 0, static_cast<u32>(m_type), m_frequency);
            return;
        }

        render_custom_oscillator_mono_in_range(
            m_periodic_wave.value(),
            m_custom_normalization_gain,
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
    } else {
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
    }

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
    ASSERT_RENDER_THREAD();
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
