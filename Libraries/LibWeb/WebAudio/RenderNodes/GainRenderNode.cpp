/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/GainRenderNode.h>

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>

namespace Web::WebAudio::Render {

GainRenderNode::GainRenderNode(NodeID node_id, GainGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_gain(desc.gain)
    , m_output(1, quantum_size, max_channel_count)
    , m_gain_input(1, quantum_size)
{
}

void GainRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#GainNode

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const desired_output_channels = mixed_input ? mixed_input->channel_count() : 1;
    m_output.set_channel_count(desired_output_channels);

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
        size_t mixed_input_channels = mixed_input ? mixed_input->channel_count() : 0;
        WA_NODE_DBGLN("[WebAudio][GainNode:{}] out_ch={} connections={} mixed0_ch={} gain={}",
            node_id(), m_output.channel_count(), connection_count, mixed_input_channels, m_gain);
    }

    // Audio-rate input to gain AudioParam.
    if (param_inputs.size() > GainParamIndex::gain) {
        mix_inputs_into(m_gain_input, param_inputs[GainParamIndex::gain].span());
    } else {
        m_gain_input.zero();
    }

    bool const has_gain_param_input = param_inputs.size() > GainParamIndex::gain && !param_inputs[GainParamIndex::gain].is_empty();

    if (!mixed_input) {
        m_output.zero();
        return;
    }

    size_t const frames = m_output.frame_count();
    auto gain_in = m_gain_input.channel(0);
    for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
        auto in = mixed_input->channel(ch);
        auto out = m_output.channel(ch);
        if (has_gain_param_input) {
            for (size_t i = 0; i < frames; ++i)
                out[i] = in[i] * gain_in[i];
        } else {
            for (size_t i = 0; i < frames; ++i)
                out[i] = in[i] * m_gain;
        }
    }
}

void GainRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<GainGraphNode>())
        return;
    m_gain = node.get<GainGraphNode>().gain;
}

}
