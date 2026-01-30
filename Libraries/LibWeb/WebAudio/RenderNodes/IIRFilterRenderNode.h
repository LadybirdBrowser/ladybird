/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

struct IIRFilterGraphNode;

class IIRFilterRenderNode final : public RenderNode {
public:
    IIRFilterRenderNode(NodeID node_id, IIRFilterGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    void set_coefficients(IIRFilterGraphNode const& desc);
    void ensure_history_channels(size_t channels);

    Vector<f64> m_feedforward;
    Vector<f64> m_feedback;

    size_t m_input_history_length { 0 };
    size_t m_output_history_length { 0 };

    AudioBus m_output;

    Vector<Vector<f64>> m_input_history;
    Vector<Vector<f64>> m_output_history;
};

}
