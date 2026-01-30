/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/ChannelMergerRenderNode.h>

#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio::Render {

ChannelMergerRenderNode::ChannelMergerRenderNode(NodeID node_id, ChannelMergerGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_number_of_inputs(max(1u, desc.number_of_inputs))
    , m_quantum_size(quantum_size)
    , m_output(m_number_of_inputs, quantum_size, max_channel_count)
{
    m_output.set_channel_count(m_number_of_inputs);
}

void ChannelMergerRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    m_output.zero();

    bool has_active_input = false;
    for (size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        if (inputs[input_index].is_empty())
            continue;
        auto const* mixed_input = inputs[input_index][0];
        if (!mixed_input)
            continue;
        if (mixed_input->channel_count() == 0)
            continue;
        has_active_input = true;
        break;
    }

    m_output.set_channel_count(has_active_input ? m_number_of_inputs : 0);

    if (!has_active_input)
        return;

    for (size_t input_index = 0; input_index < m_number_of_inputs; ++input_index) {
        // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
        AudioBus const* mixed_input = nullptr;
        if (input_index < inputs.size() && !inputs[input_index].is_empty())
            mixed_input = inputs[input_index][0];

        auto out = m_output.channel(input_index);
        if (!mixed_input || mixed_input->channel_count() == 0)
            continue;

        // ChannelMerger inputs are expected to be mono by the time they reach the node.
        auto in = mixed_input->channel(0);
        in.slice(0, m_quantum_size).copy_to(out.slice(0, m_quantum_size));
    }
}

void ChannelMergerRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<ChannelMergerGraphNode>())
        return;
    size_t const new_input_count = max(1u, node.get<ChannelMergerGraphNode>().number_of_inputs);
    if (new_input_count == m_number_of_inputs)
        return;

    m_number_of_inputs = new_input_count;
    m_output.set_channel_count(m_number_of_inputs);
}

}
