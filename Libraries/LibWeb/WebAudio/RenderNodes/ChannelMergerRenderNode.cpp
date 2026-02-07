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

    if (should_log_this_quantum) {
        size_t provided_input_groups = inputs.size();
        size_t max_input_channels = 0;
        if (!inputs.is_empty()) {
            for (auto const& group : inputs) {
                for (auto const* bus : group) {
                    if (!bus)
                        continue;
                    max_input_channels = max(max_input_channels, bus->channel_count());
                }
            }
        }
        WA_NODE_DBGLN("[WebAudio][ChannelMerger:{}] inputs={} provided_groups={} max_input_ch={} out_ch={}",
            node_id(), m_number_of_inputs, provided_input_groups, max_input_channels, m_output.channel_count());
    }

    for (size_t input_index = 0; input_index < m_number_of_inputs; ++input_index) {
        // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
        AudioBus const* mixed_input = nullptr;
        if (input_index < inputs.size() && !inputs[input_index].is_empty())
            mixed_input = inputs[input_index][0];

        auto out = m_output.channel(input_index);
        if (!mixed_input)
            continue;

        // ChannelMerger inputs are expected to be mono by the time they reach the node.
        auto in = mixed_input->channel(0);
        in.slice(0, m_quantum_size).copy_to(out.slice(0, m_quantum_size));
    }
}

void ChannelMergerRenderNode::apply_description(GraphNodeDescription const& node)
{
    if (!node.has<ChannelMergerGraphNode>())
        return;
    size_t const new_input_count = max(1u, node.get<ChannelMergerGraphNode>().number_of_inputs);
    if (new_input_count == m_number_of_inputs)
        return;

    m_number_of_inputs = new_input_count;
    m_output.set_channel_count(m_number_of_inputs);
}

}
