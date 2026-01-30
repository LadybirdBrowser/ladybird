/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class OscillatorRenderNode final : public RenderNode {
public:
    OscillatorRenderNode(NodeID node_id, OscillatorGraphNode const& desc, size_t quantum_size);

    void process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;
    void schedule_start(Optional<size_t> start_frame) override { m_start_frame = start_frame; }
    void schedule_stop(Optional<size_t> stop_frame) override { m_stop_frame = stop_frame; }

private:
    OscillatorType m_type { OscillatorType::Sine };
    f32 m_frequency { 440.0f };
    f32 m_detune_cents { 0.0f };
    Optional<size_t> m_start_frame;
    Optional<size_t> m_stop_frame;
    f64 m_phase { 0.0 };

    Optional<PeriodicWaveCoefficients> m_periodic_wave;
    f32 m_custom_normalization_gain { 1.0f };

    AudioBus m_output;
    AudioBus m_frequency_input;
    AudioBus m_detune_input;
};

}
