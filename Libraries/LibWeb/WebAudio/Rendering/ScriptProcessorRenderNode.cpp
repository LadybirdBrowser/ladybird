/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorRenderNode.h>

namespace Web::WebAudio::Rendering {

ScriptProcessorRenderNode::ScriptProcessorRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<ScriptProcessorPipe> pipe)
    : RenderNode(node_id, 1, 1, quantum_size, max(pipe->config().output_channel_count, 1uz))
    , m_pipe(move(pipe))
{
    VERIFY(m_pipe->config().buffer_size % quantum_size == 0);
}

ScriptProcessorRenderNode::~ScriptProcessorRenderNode() = default;

void ScriptProcessorRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto& output_bus = output(0);
    output_bus.set_channel_count(max(m_pipe->config().output_channel_count, 1uz));
    output_bus.zero();
    if (m_pipe->is_shut_down())
        return;

    // https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
    // audioprocess events are only dispatched if the ScriptProcessorNode has at least one input or one output
    // connected.
    if (input_connections(0).is_empty() && outgoing_node_connections().is_empty() && outgoing_param_connections().is_empty())
        return;

    auto const& config = m_pipe->config();
    if (m_frame_in_block == 0) {
        m_block_start_frame = context.quantum_start_frame;
        m_block_is_owned = m_pipe->is_block_owned_by_rendering_thread(m_block);
    }

    if (m_block_is_owned) {
        if (config.input_channel_count > 0) {
            auto const& input_bus = pull_input(graph, context, 0);
            for (size_t channel = 0; channel < config.input_channel_count; ++channel) {
                auto destination = m_pipe->input_channel(m_block, channel).slice(m_frame_in_block, context.quantum_size);
                if (channel < input_bus.channel_count())
                    input_bus.channel(channel).copy_to(destination);
                else
                    destination.fill(0);
            }
        }

        if (m_pipe->has_output(m_block)) {
            for (size_t channel = 0; channel < config.output_channel_count; ++channel)
                m_pipe->output_channel(m_block, channel).slice(m_frame_in_block, context.quantum_size).copy_to(output_bus.channel(channel));
        }
    }

    m_frame_in_block += context.quantum_size;
    if (m_frame_in_block < config.buffer_size)
        return;

    if (m_block_is_owned)
        m_pipe->hand_block_to_control_thread(m_block, m_block_start_frame);
    m_block = (m_block + 1) % config.block_count;
    m_frame_in_block = 0;
}

}
