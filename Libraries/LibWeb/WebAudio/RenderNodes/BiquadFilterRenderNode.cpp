/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/BiquadFilterRenderNode.h>

namespace Web::WebAudio::Render {

BiquadFilterRenderNode::BiquadFilterRenderNode(NodeID node_id, BiquadFilterGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_type(desc.type)
    , m_frequency_hz(desc.frequency_hz)
    , m_detune_cents(desc.detune_cents)
    , m_q(desc.q)
    , m_gain_db(desc.gain_db)
    , m_output(1, quantum_size, max_channel_count)
    , m_frequency_input(1, quantum_size)
    , m_detune_input(1, quantum_size)
    , m_q_input(1, quantum_size)
    , m_gain_input(1, quantum_size)
{
    m_output.set_channel_count(1);

    m_x1.resize(max_channel_count);
    m_x2.resize(max_channel_count);
    m_y1.resize(max_channel_count);
    m_y2.resize(max_channel_count);
    m_x1.fill(0.0);
    m_x2.fill(0.0);
    m_y1.fill(0.0);
    m_y2.fill(0.0);
}

void BiquadFilterRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#BiquadFilterNode

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channels = mixed_input ? mixed_input->channel_count() : 1;
    size_t const output_channels = min(input_channels, max_channel_count);
    m_output.set_channel_count(output_channels);

    if (param_inputs.size() > BiquadFilterParamIndex::frequency)
        mix_inputs_into(m_frequency_input, param_inputs[BiquadFilterParamIndex::frequency].span());
    else
        m_frequency_input.zero();

    if (param_inputs.size() > BiquadFilterParamIndex::detune)
        mix_inputs_into(m_detune_input, param_inputs[BiquadFilterParamIndex::detune].span());
    else
        m_detune_input.zero();

    if (param_inputs.size() > BiquadFilterParamIndex::q)
        mix_inputs_into(m_q_input, param_inputs[BiquadFilterParamIndex::q].span());
    else
        m_q_input.zero();

    if (param_inputs.size() > BiquadFilterParamIndex::gain)
        mix_inputs_into(m_gain_input, param_inputs[BiquadFilterParamIndex::gain].span());
    else
        m_gain_input.zero();

    bool const has_freq_param = param_inputs.size() > BiquadFilterParamIndex::frequency && !param_inputs[BiquadFilterParamIndex::frequency].is_empty();
    bool const has_detune_param = param_inputs.size() > BiquadFilterParamIndex::detune && !param_inputs[BiquadFilterParamIndex::detune].is_empty();
    bool const has_q_param = param_inputs.size() > BiquadFilterParamIndex::q && !param_inputs[BiquadFilterParamIndex::q].is_empty();
    bool const has_gain_param = param_inputs.size() > BiquadFilterParamIndex::gain && !param_inputs[BiquadFilterParamIndex::gain].is_empty();

    bool const any_audio_rate_param = has_freq_param || has_detune_param || has_q_param || has_gain_param;

    size_t const frames = m_output.frame_count();

    auto const& freq_in = m_frequency_input.channel(0);
    auto const& detune_in = m_detune_input.channel(0);
    auto const& q_in = m_q_input.channel(0);
    auto const& gain_in = m_gain_input.channel(0);

    auto effective_frequency = [&](size_t i) {
        f32 freq = has_freq_param ? freq_in[i] : m_frequency_hz;
        f32 detune = has_detune_param ? detune_in[i] : m_detune_cents;

        if (!__builtin_isfinite(freq) || __builtin_isnan(freq))
            freq = 0.0f;
        if (!__builtin_isfinite(detune) || __builtin_isnan(detune))
            detune = 0.0f;

        return compute_biquad_computed_frequency(context.sample_rate, freq, detune);
    };

    if (!any_audio_rate_param) {
        BiquadCoefficients const c = compute_biquad_normalized_coefficients(m_type, context.sample_rate, effective_frequency(0), m_q, m_gain_db);

        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            auto out = m_output.channel(ch);

            f64 x1 = m_x1[ch];
            f64 x2 = m_x2[ch];
            f64 y1 = m_y1[ch];
            f64 y2 = m_y2[ch];
            for (size_t i = 0; i < frames; ++i) {
                f64 const x = static_cast<f64>(mixed_input ? mixed_input->channel(ch)[i] : 0.0f);
                f64 const y = (c.b0 * x)
                    + (c.b1 * x1)
                    + (c.b2 * x2)
                    - (c.a1 * y1)
                    - (c.a2 * y2);
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                out[i] = static_cast<f32>(y);
            }
            m_x1[ch] = x1;
            m_x2[ch] = x2;
            m_y1[ch] = y1;
            m_y2[ch] = y2;
        }
        return;
    }

    for (size_t i = 0; i < frames; ++i) {
        f32 q = has_q_param ? q_in[i] : m_q;
        f32 gain_db = has_gain_param ? gain_in[i] : m_gain_db;

        if (!__builtin_isfinite(q) || __builtin_isnan(q))
            q = 1.0f;
        if (!__builtin_isfinite(gain_db) || __builtin_isnan(gain_db))
            gain_db = 0.0f;

        // NOTE: This calls into BiquadFilterNode to single-source the spec math.
        // It is safe here because it does not touch any GC-managed state.
        BiquadCoefficients const c = compute_biquad_normalized_coefficients(m_type, context.sample_rate, effective_frequency(i), q, gain_db);

        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            f64 const x = static_cast<f64>(mixed_input ? mixed_input->channel(ch)[i] : 0.0f);
            f64 const y = (c.b0 * x)
                + (c.b1 * m_x1[ch])
                + (c.b2 * m_x2[ch])
                - (c.a1 * m_y1[ch])
                - (c.a2 * m_y2[ch]);
            m_x2[ch] = m_x1[ch];
            m_x1[ch] = x;
            m_y2[ch] = m_y1[ch];
            m_y1[ch] = y;
            m_output.channel(ch)[i] = static_cast<f32>(y);
        }
    }
}

void BiquadFilterRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<BiquadFilterGraphNode>())
        return;

    auto const& desc = node.get<BiquadFilterGraphNode>();
    m_type = desc.type;
    m_frequency_hz = desc.frequency_hz;
    m_detune_cents = desc.detune_cents;
    m_q = desc.q;
    m_gain_db = desc.gain_db;
}

// https://webaudio.github.io/web-audio-api/#filters-characteristics
f32 compute_biquad_computed_frequency(f64 sample_rate, f32 frequency_hz, f32 detune_cents)
{
    // computedFrequency = frequency * pow(2, detune / 1200)

    if (!__builtin_isfinite(sample_rate) || sample_rate <= 0.0)
        return 0.0f;

    f32 freq = frequency_hz;
    f32 detune = detune_cents;

    if (!__builtin_isfinite(freq) || __builtin_isnan(freq))
        freq = 0.0f;
    if (!__builtin_isfinite(detune) || __builtin_isnan(detune))
        detune = 0.0f;

    // Bound detune so pow(2, detune/1200) stays finite.
    f32 const detune_limit = 1200.0f * AK::log2(NumericLimits<f32>::max());
    detune = clamp(detune, -detune_limit, detune_limit);

    f64 const ratio = AK::pow(2.0, static_cast<f64>(detune) / 1200.0);
    f64 const computed = static_cast<f64>(freq) * ratio;

    f32 const nyquist = static_cast<f32>(sample_rate * 0.5);
    if (!__builtin_isfinite(computed) || __builtin_isnan(computed))
        return 0.0f;

    return clamp(static_cast<f32>(computed), 0.0f, nyquist);
}

// https://webaudio.github.io/web-audio-api/#filters-characteristics
BiquadCoefficients compute_biquad_normalized_coefficients(BiquadFilterType type, f64 sample_rate, f32 computed_frequency_hz, f32 q, f32 gain_db)
{
    auto const passthrough = [] {
        return BiquadCoefficients {};
    };

    if (!__builtin_isfinite(sample_rate) || sample_rate <= 0.0)
        return passthrough();

    f32 f0 = computed_frequency_hz;
    if (!__builtin_isfinite(f0) || __builtin_isnan(f0))
        f0 = 0.0f;

    f32 Q = q;
    if (!__builtin_isfinite(Q) || __builtin_isnan(Q))
        Q = 1.0f;

    f32 G = gain_db;
    if (!__builtin_isfinite(G) || __builtin_isnan(G))
        G = 0.0f;

    f32 const nyquist = static_cast<f32>(sample_rate * 0.5);
    f0 = clamp(f0, 0.0f, nyquist);

    // WPT reference behavior for the classic biquad-filters.js helpers:
    // - Some filter types have explicit special-cases at normalized frequency 0 and 1 (Nyquist),
    //   and at Q == 0.
    // - These special-cases differ from just evaluating the closed-form equations at the limit.
    //   In particular, bandpass at Nyquist should return all-zero coefficients (not a recursive
    //   section with b0/b1/b2 zero but a1/a2 non-zero), otherwise the filter can have a tail.
    bool const at_min_frequency = f0 <= 0.0f;
    bool const at_max_frequency = f0 >= nyquist;

    auto const all_zero = [] {
        BiquadCoefficients out;
        out.b0 = 0.0;
        out.b1 = 0.0;
        out.b2 = 0.0;
        out.a1 = 0.0;
        out.a2 = 0.0;
        return out;
    };

    auto const wire = [] {
        return BiquadCoefficients {};
    };

    // Intermediate variables from the spec.
    // A = 10^(G/40)
    // w0 = 2*pi*f0/Fs
    // cos_w0 = cos(w0)
    // sin_w0 = sin(w0)
    f64 const A = AK::pow(10.0, static_cast<f64>(G) / 40.0);

    // Frequency and Q boundary behavior used by the WPT reference implementation.
    if (type == BiquadFilterType::Lowpass) {
        if (at_max_frequency)
            return wire();
    } else if (type == BiquadFilterType::Highpass) {
        if (at_max_frequency)
            return all_zero();
        if (at_min_frequency)
            return wire();
    } else if (type == BiquadFilterType::Bandpass) {
        if (at_min_frequency || at_max_frequency)
            return all_zero();
        if (Q <= 0.0f)
            return wire();
    } else if (type == BiquadFilterType::Notch) {
        if (at_min_frequency || at_max_frequency)
            return wire();
        if (Q <= 0.0f)
            return all_zero();
    } else if (type == BiquadFilterType::Allpass) {
        if (at_min_frequency || at_max_frequency)
            return wire();
        if (Q <= 0.0f) {
            BiquadCoefficients out;
            out.b0 = -1.0;
            out.b1 = 0.0;
            out.b2 = 0.0;
            out.a1 = 0.0;
            out.a2 = 0.0;
            return out;
        }
    } else if (type == BiquadFilterType::Peaking) {
        if (at_min_frequency || at_max_frequency)
            return wire();
        if (Q <= 0.0f) {
            BiquadCoefficients out;
            out.b0 = A * A;
            out.b1 = 0.0;
            out.b2 = 0.0;
            out.a1 = 0.0;
            out.a2 = 0.0;
            return out;
        }
    } else if (type == BiquadFilterType::Lowshelf) {
        if (at_max_frequency) {
            BiquadCoefficients out;
            out.b0 = A * A;
            out.b1 = 0.0;
            out.b2 = 0.0;
            out.a1 = 0.0;
            out.a2 = 0.0;
            return out;
        }
        if (at_min_frequency)
            return wire();
    } else if (type == BiquadFilterType::Highshelf) {
        if (at_max_frequency)
            return wire();
        if (at_min_frequency) {
            BiquadCoefficients out;
            out.b0 = A * A;
            out.b1 = 0.0;
            out.b2 = 0.0;
            out.a1 = 0.0;
            out.a2 = 0.0;
            return out;
        }
    }

    f64 const w0 = 2.0 * AK::Pi<f64> * (static_cast<f64>(f0) / sample_rate);
    f64 const cos_w0 = AK::cos(w0);
    f64 const sin_w0 = AK::sin(w0);

    // alpha_Q = sin(w0)/(2*Q)
    // alpha_Q_dB = sin(w0)/(2*10^(Q/20))
    // S = 1
    // alpha_S = sin(w0)/2*sqrt((A + 1/A)*(1/S - 1) + 2)

    f64 alpha = 0.0;
    if (type == BiquadFilterType::Lowpass || type == BiquadFilterType::Highpass) {
        // Q is in dB for lowpass and highpass.
        f64 clamped_q_db = clamp(static_cast<f64>(Q), -770.63678, 770.63678);
        f64 const q_linear = AK::pow(10.0, clamped_q_db / 20.0);
        alpha = sin_w0 / (2.0 * q_linear);
    } else if (type == BiquadFilterType::Lowshelf || type == BiquadFilterType::Highshelf) {
        // S = 1 for shelf filters.
        f64 const S = 1.0;
        alpha = (sin_w0 / 2.0) * AK::sqrt(((A + (1.0 / A)) * ((1.0 / S) - 1.0)) + 2.0);
    } else {
        f64 const q_safe = max(static_cast<f64>(Q), 0.0001);
        alpha = sin_w0 / (2.0 * q_safe);
    }

    if (!__builtin_isfinite(alpha) || __builtin_isnan(alpha))
        return passthrough();

    f64 b0 = 1.0;
    f64 b1 = 0.0;
    f64 b2 = 0.0;
    f64 a0 = 1.0;
    f64 a1 = 0.0;
    f64 a2 = 0.0;

    switch (type) {
    case BiquadFilterType::Lowpass:
        b0 = (1.0 - cos_w0) / 2.0;
        b1 = 1.0 - cos_w0;
        b2 = (1.0 - cos_w0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case BiquadFilterType::Highpass:
        b0 = (1.0 + cos_w0) / 2.0;
        b1 = -(1.0 + cos_w0);
        b2 = (1.0 + cos_w0) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case BiquadFilterType::Bandpass:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case BiquadFilterType::Notch:
        b0 = 1.0;
        b1 = -2.0 * cos_w0;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case BiquadFilterType::Allpass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cos_w0;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - alpha;
        break;
    case BiquadFilterType::Peaking:
        b0 = 1.0 + (alpha * A);
        b1 = -2.0 * cos_w0;
        b2 = 1.0 - (alpha * A);
        a0 = 1.0 + (alpha / A);
        a1 = -2.0 * cos_w0;
        a2 = 1.0 - (alpha / A);
        break;
    case BiquadFilterType::Lowshelf: {
        f64 const beta = 2.0 * AK::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + beta);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
        b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - beta);
        a0 = (A + 1.0) + (A - 1.0) * cos_w0 + beta;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
        a2 = (A + 1.0) + (A - 1.0) * cos_w0 - beta;
        break;
    }
    case BiquadFilterType::Highshelf: {
        f64 const beta = 2.0 * AK::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + beta);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
        b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - beta);
        a0 = (A + 1.0) - (A - 1.0) * cos_w0 + beta;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
        a2 = (A + 1.0) - (A - 1.0) * cos_w0 - beta;
        break;
    }
    default:
        return passthrough();
    }

    if (!__builtin_isfinite(a0) || __builtin_isnan(a0) || a0 == 0.0)
        return passthrough();

    // normalize coefficients by a0.
    f64 const inv_a0 = 1.0 / a0;

    BiquadCoefficients out;
    out.b0 = b0 * inv_a0;
    out.b1 = b1 * inv_a0;
    out.b2 = b2 * inv_a0;
    out.a1 = a1 * inv_a0;
    out.a2 = a2 * inv_a0;

    if (!__builtin_isfinite(out.b0) || !__builtin_isfinite(out.b1) || !__builtin_isfinite(out.b2) || !__builtin_isfinite(out.a1) || !__builtin_isfinite(out.a2))
        return passthrough();

    return out;
}

}
