/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/RenderGraph.h>

namespace Web::WebAudio::Rendering {

void RenderGraph::apply_control_messages(Vector<ControlMessage> messages)
{
    for (auto& message : messages) {
        message.visit(
            [&](AddNode& add_node) {
                auto node_id = add_node.node->node_id();
                m_nodes.set(node_id, move(add_node.node));
                m_incoming_connections_dirty = true;
            },
            [&](RemoveNode const& remove_node) {
                if (m_nodes.remove(remove_node.node_id))
                    m_incoming_connections_dirty = true;
            },
            [&](ReplaceConnections& replace_connections) {
                if (auto* source_node = node(replace_connections.source)) {
                    source_node->set_outgoing_connections(move(replace_connections.node_connections), move(replace_connections.param_connections));
                    m_incoming_connections_dirty = true;
                }
            },
            [&](SetChannelConfig const& config) {
                if (auto* target_node = node(config.node_id)) {
                    target_node->set_channel_count(config.channel_count);
                    target_node->set_channel_count_mode(config.channel_count_mode);
                    target_node->set_channel_interpretation(config.channel_interpretation);
                }
            },
            [&](NodeMessage const& node_message) {
                if (auto* target_node = node(node_message_target(node_message)))
                    target_node->handle_message(node_message);
            });
    }
}

// Renders a single quantum by pulling the destination node and any active source nodes.
void RenderGraph::render_quantum(RenderContext const& context)
{
    ++m_current_quantum;
    if (m_incoming_connections_dirty) {
        rebuild_incoming_connections();
        m_incoming_connections_dirty = false;
    }

    // Pull the destination first so the main graph is processed in dependency order; then process any remaining nodes
    // so unconnected sources still advance their playback state and report ended events.
    if (auto* destination_node = destination())
        pull_output(destination_node->node_id(), 0, context);
    for (auto& [node_id, graph_node] : m_nodes) {
        if (graph_node->output_count() > 0)
            pull_output(node_id, 0, context);
    }
}

// Returns the source node's output bus, processing the node first if it has not been processed in this quantum.
// Re-entrant pulls caused by cycles return the output rendered in the previous quantum.
AudioBus const* RenderGraph::pull_output(NodeID source, size_t output_index, RenderContext const& context)
{
    auto* source_node = node(source);
    if (!source_node || output_index >= source_node->output_count())
        return nullptr;

    if (!source_node->processed_in_quantum(m_current_quantum)) {
        // A re-entrant pull means this node is part of a cycle; its output bus still holds the previous quantum's
        // samples, which effectively inserts one render quantum of delay into the cycle.
        if (source_node->is_processing())
            return &source_node->output(output_index);

        source_node->set_is_processing(true);
        source_node->process(*this, context);
        source_node->set_is_processing(false);
        source_node->mark_processed_in_quantum(m_current_quantum);
    }
    return &source_node->output(output_index);
}

RenderNode* RenderGraph::node(NodeID node_id)
{
    auto it = m_nodes.find(node_id);
    if (it == m_nodes.end())
        return nullptr;
    return it->value.ptr();
}

// Drains the set of nodes that stopped playing since the last call and retires their permanently silent render nodes.
Vector<NodeID> RenderGraph::take_ended_nodes()
{
    Vector<NodeID> ended_nodes;
    for (auto& [node_id, graph_node] : m_nodes) {
        if (graph_node->take_ended_notification())
            ended_nodes.append(node_id);
    }
    for (auto node_id : ended_nodes)
        m_nodes.remove(node_id);
    if (!ended_nodes.is_empty())
        m_incoming_connections_dirty = true;
    return ended_nodes;
}

void RenderGraph::rebuild_incoming_connections()
{
    for (auto& [node_id, graph_node] : m_nodes) {
        for (size_t input_index = 0; input_index < graph_node->input_count(); ++input_index)
            graph_node->input_connections(input_index).clear_with_capacity();
        graph_node->for_each_param([](RenderAudioParam& param) {
            param.inputs().clear_with_capacity();
        });
    }

    for (auto& [node_id, graph_node] : m_nodes) {
        for (auto const& connection : graph_node->outgoing_node_connections()) {
            auto* destination_node = node(connection.destination);
            if (destination_node && connection.input_index < destination_node->input_count())
                destination_node->input_connections(connection.input_index).append({ node_id, connection.output_index });
        }
        for (auto const& connection : graph_node->outgoing_param_connections())
            connection.destination->inputs().append({ node_id, connection.output_index });
    }
}

}
