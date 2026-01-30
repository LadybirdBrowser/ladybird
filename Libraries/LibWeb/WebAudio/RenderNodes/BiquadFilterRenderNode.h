/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

// Coefficients for a normalized second-order IIR section used by WebAudio's BiquadFilterNode.
// The corresponding difference equation is:
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
struct BiquadCoefficients {
    f64 b0 { 1.0 };
    f64 b1 { 0.0 };
    f64 b2 { 0.0 };
    f64 a1 { 0.0 };
    f64 a2 { 0.0 };
};

// Spec helpers
// Source: Web Audio API, section "Filters Characteristics".
// These are pure math helpers used by both the control-thread API surface (getFrequencyResponse)
// and the render-thread DSP implementation.
f32 compute_biquad_computed_frequency(f64 sample_rate, f32 frequency_hz, f32 detune_cents);
BiquadCoefficients compute_biquad_normalized_coefficients(BiquadFilterType type, f64 sample_rate, f32 computed_frequency_hz, f32 q, f32 gain_db);

class BiquadFilterRenderNode final : public RenderNode {
public:
    BiquadFilterRenderNode(NodeID node_id, BiquadFilterGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    BiquadFilterType m_type { BiquadFilterType::Lowpass };
    f32 m_frequency_hz { 350.0f };
    f32 m_detune_cents { 0.0f };
    f32 m_q { 1.0f };
    f32 m_gain_db { 0.0f };

    AudioBus m_output;

    AudioBus m_frequency_input;
    AudioBus m_detune_input;
    AudioBus m_q_input;
    AudioBus m_gain_input;

    // Per-channel IIR state (direct-form I): previous inputs and outputs.
    Vector<f64> m_x1;
    Vector<f64> m_x2;
    Vector<f64> m_y1;
    Vector<f64> m_y2;
};

}
