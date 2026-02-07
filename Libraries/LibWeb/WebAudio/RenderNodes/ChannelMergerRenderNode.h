/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class ChannelMergerRenderNode final : public RenderNode {
public:
    ChannelMergerRenderNode(NodeID node_id, ChannelMergerGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    size_t m_number_of_inputs { 1 };
    size_t m_quantum_size { 0 };
    AudioBus m_output;
};

}
