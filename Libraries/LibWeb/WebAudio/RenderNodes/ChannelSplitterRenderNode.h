/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class ChannelSplitterRenderNode final : public RenderNode {
public:
    ChannelSplitterRenderNode(NodeID node_id, ChannelSplitterGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;

    size_t output_count() const override { return m_number_of_outputs; }
    AudioBus const& output(size_t output_index) const override;

    void apply_description(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    size_t m_number_of_outputs { 1 };
    size_t m_quantum_size { 0 };
    Vector<OwnPtr<AudioBus>> m_outputs;
};

}
