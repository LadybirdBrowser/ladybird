/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Realtime/ConstantSourceRenderNode.h>
#include <LibWeb/WebAudio/Realtime/DestinationRenderNode.h>
#include <LibWeb/WebAudio/Realtime/OhNoesRenderNode.h>
#include <LibWeb/WebAudio/Realtime/RenderGraphImpl.h>

namespace Web::WebAudio::Realtime {

namespace {

OwnPtr<RenderNode> make_render_node(NodeID node_id, RenderNodeDescription const& node, size_t quantum_size)
{
    switch (node.type) {
    case RenderNodeType::Destination: {
        auto const desc = node.destination.value_or(DestinationRenderNodeDescription {});
        return make<DestinationRenderNode>(node_id, desc.channel_count, quantum_size);
    }
    case RenderNodeType::ConstantSource: {
        auto const desc = node.constant_source.value_or(ConstantSourceRenderNodeDescription {});
        return make<ConstantSourceRenderNode>(node_id, desc, quantum_size);
    }
    // FIXME: Add more node types once implemented.
    case RenderNodeType::Unknown:
    default:
        return make<OhNoesRenderNode>(node_id, quantum_size);
    }
}

}

RenderGraphImpl::RenderGraphImpl(RenderGraphDescription const& description, f32 sample_rate)
{
    m_context.sample_rate = sample_rate;
    m_context.quantum_size = static_cast<size_t>(RENDER_QUANTUM_SIZE);
    build_nodes(description);

    m_topology = build_topology(description);
}

RenderGraphImpl::~RenderGraphImpl() = default;

AudioBus const& RenderGraphImpl::render_destination_for_current_quantum()
{
    process_a_render_quantum();
    return m_nodes[m_topology->destination_node_index]->output(0);
}

void RenderGraphImpl::begin_quantum(size_t current_frame)
{
    // FIXME: update topology and AudioParams once implemented.
    m_context.current_frame = current_frame;
}

void RenderGraphImpl::process_a_render_quantum()
{
    if (!m_topology)
        return;

    auto& topology = *m_topology;

    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // 4.4. For each AudioNode in ordered node list, execute these steps:
    for (auto const node_index : topology.ordered_node_list) {
        auto* node = m_nodes[node_index].ptr();
        VERIFY(node != nullptr);

        // FIXME: 4.4.1. For each AudioParam of this AudioNode, execute these steps:

        // 4.4.2. If this AudioNode has any AudioNodes connected to its input, sum the buffers made available
        //        for reading by all AudioNodes connected to this AudioNode. The resulting buffer is called the input buffer.
        //        Up or down-mix it to match if number of input channels of this AudioNode.
        // NB: At this level, the render graph only resolves and wires upstream AudioBus pointers.
        //     Any summing/mixing should be performed by RenderNode::process().
        auto& per_input_connections = topology.inputs_by_input[node_index];
        auto& per_input_buses = topology.input_buses[node_index];
        VERIFY(per_input_buses.size() == per_input_connections.size());

        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index) {
            auto const& connections = per_input_connections[input_index];
            auto& buses = per_input_buses[input_index];
            VERIFY(buses.size() == connections.size());

            for (size_t i = 0; i < connections.size(); ++i) {
                auto const& c = connections[i];
                VERIFY(c.source_node_index < m_nodes.size());
                auto* source_node = m_nodes[c.source_node_index].ptr();
                VERIFY(source_node != nullptr);
                size_t const source_output_count = source_node->output_count();
                VERIFY(source_output_count > 0);
                size_t const clamped_source_output = min(c.source_output, source_output_count - 1);
                buses[i] = &source_node->output(clamped_source_output);
            }
        }
        // FIXME: 4.4.4. If this AudioNode is an AudioWorkletNode, execute these substeps:

        // 4.4.5. If this AudioNode is a destination node, record the input of this AudioNode.
        // 4.4.6. Else, process the input buffer, and make available for reading the resulting buffer.
        // NB: These cases are handled by RenderNode::process()

        node->process(m_context, per_input_buses);
    }
}

void RenderGraphImpl::build_nodes(RenderGraphDescription const& description)
{
    // Called on the control thread.
    m_nodes.clear();
    m_node_index_by_id.clear();

    auto const node_count = description.nodes.size();
    m_nodes.ensure_capacity(node_count);
    m_node_index_by_id.ensure_capacity(node_count);

    for (auto const& it : description.nodes) {
        NodeID const node_id = it.key;
        RenderNodeDescription const& node = it.value;

        size_t const node_index = m_nodes.size();
        m_node_index_by_id.set(node_id, node_index);

        m_nodes.append(make_render_node(node_id, node, m_context.quantum_size));
    }

    VERIFY(m_nodes.size() == node_count);
}

OwnPtr<RenderGraphImpl::Topology> RenderGraphImpl::build_topology(RenderGraphDescription const& description)
{
    // Called on the control thread.
    auto topology = make<Topology>();

    size_t const node_count = m_nodes.size();
    topology->inputs_by_input.resize(node_count);
    topology->input_buses.resize(node_count);

    auto destination_it = m_node_index_by_id.find(description.destination_node_id);
    VERIFY(destination_it != m_node_index_by_id.end());
    topology->destination_node_index = destination_it->value;

    // Build per-destination-input connection buckets.
    for (RenderConnection const& c : description.connections) {
        auto destination_index_it = m_node_index_by_id.find(c.destination);
        if (destination_index_it == m_node_index_by_id.end())
            continue;
        auto source_index_it = m_node_index_by_id.find(c.source);
        if (source_index_it == m_node_index_by_id.end())
            continue;

        auto& per_input_connections = topology->inputs_by_input[destination_index_it->value];
        if (per_input_connections.size() <= c.destination_input_index)
            per_input_connections.resize(c.destination_input_index + 1);
        per_input_connections[c.destination_input_index].append(IndexedConnection {
            .source_node_index = source_index_it->value,
            .source_output = c.source_output_index,
        });
    }

    // Pre-size scratch input bus pointer vectors so process() never resizes/appends.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto const& per_input_connections = topology->inputs_by_input[node_index];
        auto& per_input_buses = topology->input_buses[node_index];
        per_input_buses.resize(per_input_connections.size());
        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index)
            per_input_buses[input_index].resize(per_input_connections[input_index].size());
    }

    compute_processing_order(*topology);
    return topology;
}

void RenderGraphImpl::compute_processing_order(Topology& topology)
{
    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // 4.2. order the AudioNodes of the BaseAudioContext to be processed.

    // 4.2.1. Let ordered node list be an empty list of AudioNodes and AudioListener.
    //        It will contain an ordered list of AudioNodes and the AudioListener when this ordering algorithm terminates.
    // FIXME: Does not currently include the AudioListener.
    auto& ordered_node_list = topology.ordered_node_list;
    ordered_node_list.clear();
    ordered_node_list.ensure_capacity(m_nodes.size());

    // 4.2.2. Let nodes be the set of all nodes created by this BaseAudioContext, and still alive.
    // NB: This is represented by m_nodes.

    // FIXME: 4.2.3. Add the AudioListener to nodes.
    // FIXME: 4.2.4–4.2.7: Delay cycle breaking and muting/removal of remaining cycles.

    // 4.2.8. Consider all elements in nodes to be unmarked. While there are unmarked elements in nodes:
    // 4.2.8.1. Choose an element node in nodes.
    // NB: Our implementation only orders the destination node connected subgraph.
    // 4.2.8.2. Visit node.
    Vector<VisitMark> marks;
    marks.resize(m_nodes.size());
    marks.fill(VisitMark::Unvisited);
    bool found_cycle = false;

    visit_node(topology.destination_node_index, topology, marks, found_cycle, ordered_node_list);

    if (found_cycle)
        ordered_node_list.clear();
}

void RenderGraphImpl::visit_node(size_t node_index, Topology const& topology, Vector<VisitMark>& marks, bool& found_cycle, Vector<size_t>& ordered_nodes)
{
    // Visiting a node means performing the following steps:

    // 4.2.8.2.1. If node is marked, abort these steps.
    auto& mark = marks[node_index];
    if (mark == VisitMark::Visited)
        return;

    // ADHOC: Temporarily marked node encountered again means there is a cycle.
    if (mark == VisitMark::Visiting) {
        found_cycle = true;
        return;
    }

    // 4.2.8.2.2. Mark node.
    mark = VisitMark::Visiting;

    // 4.2.8.2.3. If node is an AudioNode, Visit each AudioNode connected to the input of node.
    auto const& per_input_connections = topology.inputs_by_input[node_index];
    for (auto const& input_connections : per_input_connections) {
        for (auto const& c : input_connections) {
            visit_node(c.source_node_index, topology, marks, found_cycle, ordered_nodes);
            if (found_cycle)
                return;
        }
    }

    // FIXME: 4.2.8.2.4. For each AudioParam param of node:

    // FIXME: 4.2.8.2.4.1. For each AudioNode param input node connected to param:

    // FIXME: 4.2.8.2.4.1.1. Visit param input node

    mark = VisitMark::Visited;

    // 4.2.8.2.4.5. Add node to the beginning of ordered node list.
    // NB: This implementation already traverses upstream dependencies first, so we can just append here.
    ordered_nodes.append(node_index);
}

}
