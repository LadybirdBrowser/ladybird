/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class GainRenderNode final : public RenderNode {
public:
    GainRenderNode(NodeID node_id, GainGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    f32 m_gain { 1.0f };
    AudioBus m_output;
    AudioBus m_gain_input;
};

}
