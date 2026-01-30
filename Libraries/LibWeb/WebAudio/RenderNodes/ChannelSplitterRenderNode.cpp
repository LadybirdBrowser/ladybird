/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/ChannelSplitterRenderNode.h>

#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio::Render {

ChannelSplitterRenderNode::ChannelSplitterRenderNode(NodeID node_id, ChannelSplitterGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_number_of_outputs(max(1u, desc.number_of_outputs))
    , m_quantum_size(quantum_size)
{
    m_outputs.ensure_capacity(max_channel_count);
    for (size_t i = 0; i < max_channel_count; ++i)
        m_outputs.unchecked_append(make<AudioBus>(1, m_quantum_size));
}

void ChannelSplitterRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

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
        size_t connection_count = 0;
        if (!inputs.is_empty() && inputs[0].size() >= 1)
            connection_count = inputs[0].size() - 1;
        size_t mixed_input_channels = (!inputs.is_empty() && !inputs[0].is_empty() && inputs[0][0]) ? inputs[0][0]->channel_count() : 0;
        WA_NODE_DBGLN("[WebAudio][ChannelSplitter:{}] outputs={} connections={} mixed0_ch={}",
            node_id(), m_number_of_outputs, connection_count, mixed_input_channels);
    }

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    for (size_t output_index = 0; output_index < m_number_of_outputs; ++output_index) {
        auto& out_bus = *m_outputs[output_index];
        if (!mixed_input || output_index >= mixed_input->channel_count()) {
            out_bus.zero();
            continue;
        }

        auto in = mixed_input->channel(output_index);
        auto out = out_bus.channel(0);
        in.slice(0, m_quantum_size).copy_to(out.slice(0, m_quantum_size));
    }
}

AudioBus const& ChannelSplitterRenderNode::output(size_t output_index) const
{
    ASSERT_RENDER_THREAD();

    if (output_index >= m_number_of_outputs)
        return *m_outputs[0];
    return *m_outputs[output_index];
}

void ChannelSplitterRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<ChannelSplitterGraphNode>())
        return;
    size_t const new_count = max(1u, node.get<ChannelSplitterGraphNode>().number_of_outputs);
    if (new_count == m_number_of_outputs)
        return;

    m_number_of_outputs = new_count;
}

}
