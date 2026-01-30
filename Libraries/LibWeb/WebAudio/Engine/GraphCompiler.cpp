/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCompiler.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

using namespace Render;

namespace {

struct CycleSearchState {
    Vector<int> index;
    Vector<int> lowlink;
    Vector<u8> on_stack;
    Vector<size_t> stack;
    int next_index { 0 };
    Vector<Vector<size_t>> components;
};

void strong_connect(size_t node_index, Vector<Vector<size_t>> const& edges, CycleSearchState& state)
{
    ASSERT_CONTROL_THREAD();
    state.index[node_index] = state.next_index;
    state.lowlink[node_index] = state.next_index;
    ++state.next_index;
    state.stack.append(node_index);
    state.on_stack[node_index] = 1;

    for (size_t neighbor_index : edges[node_index]) {
        if (state.index[neighbor_index] == -1) {
            strong_connect(neighbor_index, edges, state);
            state.lowlink[node_index] = min(state.lowlink[node_index], state.lowlink[neighbor_index]);
        } else if (state.on_stack[neighbor_index]) {
            state.lowlink[node_index] = min(state.lowlink[node_index], state.index[neighbor_index]);
        }
    }

    if (state.lowlink[node_index] != state.index[node_index])
        return;

    Vector<size_t> component;
    while (!state.stack.is_empty()) {
        size_t const member = state.stack.take_last();
        state.on_stack[member] = 0;
        component.append(member);
        if (member == node_index)
            break;
    }
    state.components.append(move(component));
}

Vector<u8> compute_nodes_in_cycles(size_t node_count, Vector<Vector<size_t>> const& edges)
{
    ASSERT_CONTROL_THREAD();
    Vector<u8> in_cycle;
    in_cycle.resize(node_count);
    for (auto& value : in_cycle)
        value = 0;

    CycleSearchState state;
    state.index.resize(node_count);
    state.lowlink.resize(node_count);
    state.on_stack.resize(node_count);
    for (size_t i = 0; i < node_count; ++i) {
        state.index[i] = -1;
        state.lowlink[i] = -1;
        state.on_stack[i] = 0;
    }

    for (size_t i = 0; i < node_count; ++i) {
        if (state.index[i] == -1)
            strong_connect(i, edges, state);
    }

    Vector<u8> has_self_edge;
    has_self_edge.resize(node_count);
    for (auto& value : has_self_edge)
        value = 0;
    for (size_t i = 0; i < node_count; ++i) {
        for (size_t neighbor_index : edges[i]) {
            if (neighbor_index == i) {
                has_self_edge[i] = 1;
                break;
            }
        }
    }

    for (auto const& component : state.components) {
        if (component.size() > 1) {
            for (size_t const member : component)
                in_cycle[member] = 1;
            continue;
        }
        if (component.size() == 1) {
            size_t const member = component[0];
            if (has_self_edge[member])
                in_cycle[member] = 1;
        }
    }

    return in_cycle;
}

OwnPtr<Render::RenderNode> make_render_node(GraphDescription const&, NodeID node_id, GraphNodeDescription const& node, size_t quantum_size, GraphResourceResolver const& resources)
{
    ASSERT_CONTROL_THREAD();
    return node.visit([&](auto const& payload) {
        return payload.make_render_node(node_id, quantum_size, resources);
    });
}

GraphUpdateKind classify_graph_update(GraphDescription const& old_desc, GraphDescription const& new_desc)
{
    ASSERT_CONTROL_THREAD();
    if (old_desc.destination_node_id != new_desc.destination_node_id)
        return GraphUpdateKind::RebuildRequired;

    if (old_desc.nodes.size() != new_desc.nodes.size())
        return GraphUpdateKind::RebuildRequired;

    bool any_connections_changed = false;
    if (old_desc.connections.size() != new_desc.connections.size()) {
        any_connections_changed = true;
    } else {
        for (size_t i = 0; i < old_desc.connections.size(); ++i) {
            auto const& a = old_desc.connections[i];
            auto const& b = new_desc.connections[i];
            if (a.source != b.source || a.destination != b.destination || a.source_output_index != b.source_output_index || a.destination_input_index != b.destination_input_index) {
                any_connections_changed = true;
                break;
            }
        }
    }

    if (!any_connections_changed) {
        if (old_desc.param_connections.size() != new_desc.param_connections.size()) {
            any_connections_changed = true;
        } else {
            for (size_t i = 0; i < old_desc.param_connections.size(); ++i) {
                auto const& a = old_desc.param_connections[i];
                auto const& b = new_desc.param_connections[i];
                if (a.source != b.source || a.destination != b.destination || a.source_output_index != b.source_output_index || a.destination_param_index != b.destination_param_index) {
                    any_connections_changed = true;
                    break;
                }
            }
        }
    }

    bool any_node_changed = false;
    bool any_topology_affecting = false;

    bool any_param_automation_changed = false;
    if (old_desc.param_automations.size() != new_desc.param_automations.size()) {
        any_param_automation_changed = true;
    } else {
        for (size_t i = 0; i < old_desc.param_automations.size(); ++i) {
            auto const& a = old_desc.param_automations[i];
            auto const& b = new_desc.param_automations[i];
            if (a.destination != b.destination
                || a.destination_param_index != b.destination_param_index
                || a.initial_value != b.initial_value
                || a.default_value != b.default_value
                || a.min_value != b.min_value
                || a.max_value != b.max_value
                || a.automation_rate != b.automation_rate
                || a.segments.size() != b.segments.size()) {
                any_param_automation_changed = true;
                break;
            }

            for (size_t j = 0; j < a.segments.size(); ++j) {
                auto const& sa = a.segments[j];
                auto const& sb = b.segments[j];
                if (sa.type != sb.type
                    || sa.start_frame != sb.start_frame
                    || sa.end_frame != sb.end_frame
                    || sa.start_value != sb.start_value
                    || sa.end_value != sb.end_value
                    || sa.time_constant != sb.time_constant
                    || sa.target != sb.target
                    || sa.curve.size() != sb.curve.size()) {
                    any_param_automation_changed = true;
                    break;
                }

                for (size_t k = 0; k < sa.curve.size(); ++k) {
                    if (sa.curve[k] != sb.curve[k]) {
                        any_param_automation_changed = true;
                        break;
                    }
                }
                if (any_param_automation_changed)
                    break;
            }
            if (any_param_automation_changed)
                break;
        }
    }

    for (auto const& it : old_desc.nodes) {
        auto node_id = it.key;
        auto const& old_node = it.value;
        auto maybe_new = new_desc.nodes.get(node_id);
        if (!maybe_new.has_value())
            return GraphUpdateKind::RebuildRequired;

        auto const& new_node = *maybe_new;

        auto safety = GraphCompiler::classify_node_update(old_node, new_node);
        if (safety == GraphUpdateKind::None)
            continue;

        any_node_changed = true;

        if (safety == GraphUpdateKind::Topology) {
            any_topology_affecting = true;
            continue;
        }
        if (safety == GraphUpdateKind::RebuildRequired)
            return GraphUpdateKind::RebuildRequired;
    }

    if (!any_connections_changed && !any_node_changed)
        return any_param_automation_changed ? GraphUpdateKind::Parameter : GraphUpdateKind::None;

    if (any_connections_changed || any_topology_affecting)
        return GraphUpdateKind::Topology;

    return GraphUpdateKind::Parameter;
}

}

GraphUpdateKind GraphCompiler::classify_node_update(GraphNodeDescription const& old_desc, GraphNodeDescription const& new_desc)
{
    ASSERT_CONTROL_THREAD();
    auto old_type = graph_node_type(old_desc);
    auto new_type = graph_node_type(new_desc);
    if (old_type != new_type)
        return GraphUpdateKind::RebuildRequired;

    return old_desc.visit([&](auto const& old_payload) -> GraphUpdateKind {
        using T = AK::Detail::RemoveCVReference<decltype(old_payload)>;
        if (!new_desc.has<T>())
            return GraphUpdateKind::RebuildRequired;
        return old_payload.classify_update(new_desc.get<T>());
    });
}

GraphUpdateKind GraphCompiler::classify_update(GraphDescription const& old_description, GraphDescription const& new_description)
{
    ASSERT_CONTROL_THREAD();
    return classify_graph_update(old_description, new_description);
}

void GraphCompiler::build_nodes(GraphExecutor& executor, GraphResourceResolver const& resources)
{
    ASSERT_CONTROL_THREAD();
    executor.m_node_ids.ensure_capacity(executor.m_description.nodes.size());
    executor.m_node_types_by_index.ensure_capacity(executor.m_description.nodes.size());
    executor.m_nodes.ensure_capacity(executor.m_description.nodes.size());

    Vector<NodeID> sorted_node_ids;
    sorted_node_ids.ensure_capacity(executor.m_description.nodes.size());
    for (auto const& it : executor.m_description.nodes)
        sorted_node_ids.append(it.key);
    quick_sort(sorted_node_ids, [](NodeID const& a, NodeID const& b) {
        return a.value() < b.value();
    });

    for (auto const& node_id : sorted_node_ids) {
        auto maybe_node = executor.m_description.nodes.get(node_id);
        if (!maybe_node.has_value())
            continue;
        GraphNodeDescription const& node = maybe_node.value();

        GraphNodeType const node_type = graph_node_type(node);

        size_t const node_index = executor.m_nodes.size();
        executor.m_node_ids.append(node_id);
        executor.m_node_types_by_index.append(node_type);
        executor.m_node_index_by_id.set(node_id, node_index);

        executor.m_nodes.append(make_render_node(executor.m_description, node_id, node, executor.m_context.quantum_size, resources));

        if (node_type == GraphNodeType::Analyser)
            executor.m_analyser_node_indices.append(node_index);

        VERIFY(executor.m_nodes.size() == node_index + 1);
    }
}

GraphExecutor::Topology* GraphCompiler::build_topology(GraphExecutor& executor, GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    auto* topology = new GraphExecutor::Topology;
    topology->connections = description.connections;
    topology->param_connections = description.param_connections;
    rebuild_processing_order(executor, *topology, description);
    return topology;
}

void GraphCompiler::rebuild_processing_order(GraphExecutor& executor, GraphExecutor::Topology& topology, GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // 4.2: order the AudioNodes of the BaseAudioContext to be processed.

    struct VirtualNode {
        enum class Kind : u8 {
            Real,
            DelayWriter,
            DelayReader,
        };

        Kind kind { Kind::Real };
        size_t real_node_index { 0 };
        GraphNodeType node_type { GraphNodeType::Unknown };
    };

    struct VirtualAudioConnection {
        size_t source_node_index { 0 };
        size_t source_output { 0 };
        size_t destination_node_index { 0 };
        size_t destination_input { 0 };
    };

    struct VirtualParamConnection {
        size_t source_node_index { 0 };
        size_t source_output { 0 };
        size_t destination_node_index { 0 };
        size_t destination_param_index { 0 };
    };

    size_t const real_node_count = executor.m_nodes.size();
    if (real_node_count == 0) {
        topology.nodes.clear();
        topology.processing_order.clear();
        topology.inputs_by_input.clear();
        topology.param_inputs_by_param.clear();
        topology.input_buses_scratch.clear();
        topology.param_input_buses_scratch.clear();
        topology.channel_mixing_by_node.clear();
        topology.input_mix_buses.clear();
        topology.dependents.clear();
        topology.destination_node_index = 0;
        return;
    }

    // 4.2.1: Let ordered node list be an empty list of AudioNodes and AudioListener. It will contain an ordered list of AudioNodes and the AudioListener when this ordering algorithm terminates.
    Vector<size_t> ordered_node_list;

    // 4.2.2: Let nodes be the set of all nodes created by this BaseAudioContext, and still alive.
    Vector<size_t> nodes;
    nodes.ensure_capacity(real_node_count);
    for (size_t node_index = 0; node_index < real_node_count; ++node_index)
        nodes.append(node_index);

    // 4.2.3: Add the AudioListener to nodes.

    Vector<Vector<size_t>> real_edges;
    real_edges.resize(real_node_count);
    for (GraphConnection const& c : topology.connections) {
        auto source_index_it = executor.m_node_index_by_id.find(c.source);
        if (source_index_it == executor.m_node_index_by_id.end())
            continue;
        auto destination_index_it = executor.m_node_index_by_id.find(c.destination);
        if (destination_index_it == executor.m_node_index_by_id.end())
            continue;
        real_edges[source_index_it->value].append(destination_index_it->value);
    }
    for (GraphParamConnection const& c : topology.param_connections) {
        auto source_index_it = executor.m_node_index_by_id.find(c.source);
        if (source_index_it == executor.m_node_index_by_id.end())
            continue;
        auto destination_index_it = executor.m_node_index_by_id.find(c.destination);
        if (destination_index_it == executor.m_node_index_by_id.end())
            continue;
        real_edges[source_index_it->value].append(destination_index_it->value);
    }

    Vector<u8> real_nodes_in_cycles = compute_nodes_in_cycles(real_node_count, real_edges);

    // 4.2.4: Let cycle breakers be an empty set of DelayNodes. It will contain all the DelayNodes that are part of a cycle.
    Vector<size_t> cycle_breakers;
    cycle_breakers.ensure_capacity(real_node_count);

    Vector<u8> delay_in_cycle;
    delay_in_cycle.resize(real_node_count);
    for (auto& value : delay_in_cycle)
        value = 0;

    // 4.2.5: For each AudioNode node in nodes: If node is a DelayNode that is part of a cycle, add it to cycle breakers and remove it from nodes.
    for (size_t const node_index : nodes) {
        if (executor.m_node_types_by_index[node_index] != GraphNodeType::Delay)
            continue;
        if (!real_nodes_in_cycles[node_index])
            continue;
        delay_in_cycle[node_index] = 1;
        cycle_breakers.append(node_index);
    }

    Vector<VirtualNode> virtual_nodes;
    virtual_nodes.ensure_capacity(real_node_count + (cycle_breakers.size() * 2));

    Vector<Optional<size_t>> virtual_index_for_real;
    virtual_index_for_real.resize(real_node_count);

    Vector<Optional<size_t>> delay_writer_index_for_real;
    delay_writer_index_for_real.resize(real_node_count);

    Vector<Optional<size_t>> delay_reader_index_for_real;
    delay_reader_index_for_real.resize(real_node_count);

    // 4.2.6: For each DelayNode delay in cycle breakers: Let delayWriter and delayReader respectively be a DelayWriter and a DelayReader, for delay. Add delayWriter and delayReader to nodes. Disconnect delay from all its input and outputs.
    for (size_t node_index = 0; node_index < real_node_count; ++node_index) {
        GraphNodeType const node_type = executor.m_node_types_by_index[node_index];
        if (node_type == GraphNodeType::Delay && delay_in_cycle[node_index]) {
            size_t const writer_index = virtual_nodes.size();
            virtual_nodes.append(VirtualNode {
                .kind = VirtualNode::Kind::DelayWriter,
                .real_node_index = node_index,
                .node_type = node_type,
            });
            size_t const reader_index = virtual_nodes.size();
            virtual_nodes.append(VirtualNode {
                .kind = VirtualNode::Kind::DelayReader,
                .real_node_index = node_index,
                .node_type = node_type,
            });
            delay_writer_index_for_real[node_index] = writer_index;
            delay_reader_index_for_real[node_index] = reader_index;
            continue;
        }

        size_t const virtual_index = virtual_nodes.size();
        virtual_nodes.append(VirtualNode {
            .kind = VirtualNode::Kind::Real,
            .real_node_index = node_index,
            .node_type = node_type,
        });
        virtual_index_for_real[node_index] = virtual_index;
    }

    Vector<VirtualAudioConnection> virtual_audio_connections;
    virtual_audio_connections.ensure_capacity(topology.connections.size());

    Vector<VirtualParamConnection> virtual_param_connections;
    virtual_param_connections.ensure_capacity(topology.param_connections.size());

    Vector<Vector<size_t>> virtual_edges;
    virtual_edges.resize(virtual_nodes.size());

    for (GraphConnection const& c : topology.connections) {
        auto source_index_it = executor.m_node_index_by_id.find(c.source);
        if (source_index_it == executor.m_node_index_by_id.end())
            continue;
        auto destination_index_it = executor.m_node_index_by_id.find(c.destination);
        if (destination_index_it == executor.m_node_index_by_id.end())
            continue;
        size_t const source_real = source_index_it->value;
        size_t const destination_real = destination_index_it->value;

        Optional<size_t> source_virtual = delay_in_cycle[source_real] ? delay_reader_index_for_real[source_real] : virtual_index_for_real[source_real];
        Optional<size_t> destination_virtual = delay_in_cycle[destination_real] ? delay_writer_index_for_real[destination_real] : virtual_index_for_real[destination_real];
        if (!source_virtual.has_value() || !destination_virtual.has_value())
            continue;

        virtual_audio_connections.append(VirtualAudioConnection {
            .source_node_index = source_virtual.value(),
            .source_output = c.source_output_index,
            .destination_node_index = destination_virtual.value(),
            .destination_input = c.destination_input_index,
        });
        virtual_edges[source_virtual.value()].append(destination_virtual.value());
    }

    for (GraphParamConnection const& c : topology.param_connections) {
        auto source_index_it = executor.m_node_index_by_id.find(c.source);
        if (source_index_it == executor.m_node_index_by_id.end())
            continue;
        auto destination_index_it = executor.m_node_index_by_id.find(c.destination);
        if (destination_index_it == executor.m_node_index_by_id.end())
            continue;
        size_t const source_real = source_index_it->value;
        size_t const destination_real = destination_index_it->value;

        Optional<size_t> source_virtual = delay_in_cycle[source_real] ? delay_reader_index_for_real[source_real] : virtual_index_for_real[source_real];
        Optional<size_t> destination_virtual = delay_in_cycle[destination_real] ? delay_reader_index_for_real[destination_real] : virtual_index_for_real[destination_real];
        if (!source_virtual.has_value() || !destination_virtual.has_value())
            continue;

        virtual_param_connections.append(VirtualParamConnection {
            .source_node_index = source_virtual.value(),
            .source_output = c.source_output_index,
            .destination_node_index = destination_virtual.value(),
            .destination_param_index = c.destination_param_index,
        });
        virtual_edges[source_virtual.value()].append(destination_virtual.value());
    }

    Optional<size_t> listener_virtual_index;
    for (size_t node_index = 0; node_index < virtual_nodes.size(); ++node_index) {
        if (virtual_nodes[node_index].node_type == GraphNodeType::AudioListener) {
            listener_virtual_index = node_index;
            break;
        }
    }
    if (listener_virtual_index.has_value()) {
        for (size_t node_index = 0; node_index < virtual_nodes.size(); ++node_index) {
            if (virtual_nodes[node_index].node_type != GraphNodeType::Panner)
                continue;
            virtual_edges[listener_virtual_index.value()].append(node_index);
        }
    }

    // 4.2.7: If nodes contains cycles, mute all the AudioNodes that are part of this cycle, and remove them from nodes.
    Vector<u8> virtual_nodes_in_cycles = compute_nodes_in_cycles(virtual_nodes.size(), virtual_edges);
    Vector<u8> virtual_node_removed;
    virtual_node_removed.resize(virtual_nodes.size());
    for (size_t node_index = 0; node_index < virtual_nodes.size(); ++node_index) {
        if (!virtual_nodes_in_cycles[node_index]) {
            virtual_node_removed[node_index] = 0;
            continue;
        }
        if (virtual_nodes[node_index].node_type == GraphNodeType::AudioListener) {
            virtual_node_removed[node_index] = 0;
            continue;
        }
        virtual_node_removed[node_index] = 1;
    }

    Vector<Optional<size_t>> virtual_to_topology;
    virtual_to_topology.resize(virtual_nodes.size());

    topology.nodes.clear();
    topology.nodes.ensure_capacity(virtual_nodes.size());

    for (size_t node_index = 0; node_index < virtual_nodes.size(); ++node_index) {
        if (virtual_node_removed[node_index])
            continue;

        size_t const topology_index = topology.nodes.size();
        virtual_to_topology[node_index] = topology_index;

        VirtualNode const& virtual_node = virtual_nodes[node_index];
        GraphExecutor::Topology::ProcessingNodeKind kind = GraphExecutor::Topology::ProcessingNodeKind::Real;
        if (virtual_node.kind == VirtualNode::Kind::DelayWriter)
            kind = GraphExecutor::Topology::ProcessingNodeKind::DelayWriter;
        else if (virtual_node.kind == VirtualNode::Kind::DelayReader)
            kind = GraphExecutor::Topology::ProcessingNodeKind::DelayReader;

        topology.nodes.append(GraphExecutor::Topology::ProcessingNode {
            .kind = kind,
            .real_node_index = virtual_node.real_node_index,
            .param_owner_node_index = virtual_node.real_node_index,
            .node_type = virtual_node.node_type,
            .render_node = executor.m_nodes[virtual_node.real_node_index].ptr(),
        });
    }

    size_t const node_count = topology.nodes.size();
    topology.inputs_by_input.clear();
    topology.param_inputs_by_param.clear();
    topology.input_buses_scratch.clear();
    topology.param_input_buses_scratch.clear();
    topology.channel_mixing_by_node.clear();
    topology.input_mix_buses.clear();
    topology.dependents.clear();

    topology.inputs_by_input.resize(node_count);
    topology.param_inputs_by_param.resize(node_count);
    topology.input_buses_scratch.resize(node_count);
    topology.param_input_buses_scratch.resize(node_count);
    topology.channel_mixing_by_node.resize(node_count);
    topology.input_mix_buses.resize(node_count);
    topology.dependents.resize(node_count);

    static constexpr size_t max_mixing_channel_count = 32;

    auto param_count_for_node = [&](GraphExecutor::Topology::ProcessingNode const& node) -> size_t {
        if (node.kind == GraphExecutor::Topology::ProcessingNodeKind::DelayWriter)
            return 0;
        if (node.kind == GraphExecutor::Topology::ProcessingNodeKind::DelayReader)
            return RenderParamLayout::delay_param_count;

        size_t param_count = RenderParamLayout::param_count(node.node_type);
        if (node.node_type == GraphNodeType::AudioWorklet) {
            NodeID const node_id = executor.m_node_ids[node.real_node_index];
            auto maybe_node_desc = description.nodes.get(node_id);
            if (maybe_node_desc.has_value() && maybe_node_desc->has<AudioWorkletGraphNode>())
                param_count = maybe_node_desc->get<AudioWorkletGraphNode>().parameter_names.size();
        }
        return param_count;
    };

    // Size param inputs and channel mixing settings per processing node.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        GraphExecutor::Topology::ProcessingNode const& node = topology.nodes[node_index];
        size_t const param_count = param_count_for_node(node);
        topology.param_inputs_by_param[node_index].resize(param_count);
        topology.param_input_buses_scratch[node_index].resize(param_count);

        NodeID const node_id = executor.m_node_ids[node.real_node_index];
        auto maybe_node_desc = description.nodes.get(node_id);
        if (!maybe_node_desc.has_value()) {
            topology.channel_mixing_by_node[node_index] = {};
            continue;
        }

        auto const& node_desc = maybe_node_desc.value();
        GraphExecutor::Topology::ChannelMixingSettings settings {};

        switch (graph_node_type(node_desc)) {
        case GraphNodeType::Destination:
            settings.channel_count = node_desc.get<DestinationGraphNode>().channel_count;
            settings.channel_count_mode = ChannelCountMode::Explicit;
            settings.channel_interpretation = ChannelInterpretation::Speakers;
            break;
        case GraphNodeType::Gain: {
            auto const& desc = node_desc.get<GainGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::Convolver: {
            auto const& desc = node_desc.get<ConvolverGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::Delay: {
            auto const& desc = node_desc.get<DelayGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::StereoPanner: {
            auto const& desc = node_desc.get<StereoPannerGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::Analyser: {
            auto const& desc = node_desc.get<AnalyserGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::AudioWorklet: {
            auto const& desc = node_desc.get<AudioWorkletGraphNode>();
            settings.channel_count = desc.channel_count;
            settings.channel_count_mode = desc.channel_count_mode;
            settings.channel_interpretation = desc.channel_interpretation;
            break;
        }
        case GraphNodeType::ChannelSplitter: {
            auto const& desc = node_desc.get<ChannelSplitterGraphNode>();
            settings.channel_count = desc.number_of_outputs;
            settings.channel_count_mode = ChannelCountMode::Explicit;
            settings.channel_interpretation = ChannelInterpretation::Discrete;
            break;
        }
        case GraphNodeType::ChannelMerger:
            settings.channel_count = 1;
            settings.channel_count_mode = ChannelCountMode::Explicit;
            settings.channel_interpretation = ChannelInterpretation::Speakers;
            break;
        default:
            // Many nodes currently do their own (node-specific) channel handling. For now,
            // we still mix their incoming connections at the graph edge using default rules.
            settings.channel_count = 1;
            settings.channel_count_mode = ChannelCountMode::Max;
            settings.channel_interpretation = ChannelInterpretation::Speakers;
            break;
        }

        settings.channel_count = clamp(settings.channel_count, 1ul, max_mixing_channel_count);
        topology.channel_mixing_by_node[node_index] = settings;
    }

    for (VirtualAudioConnection const& connection : virtual_audio_connections) {
        if (virtual_node_removed[connection.source_node_index])
            continue;
        if (virtual_node_removed[connection.destination_node_index])
            continue;

        size_t const source_index = virtual_to_topology[connection.source_node_index].value();
        size_t const destination_index = virtual_to_topology[connection.destination_node_index].value();

        auto& per_input_connections = topology.inputs_by_input[destination_index];
        if (per_input_connections.size() <= connection.destination_input)
            per_input_connections.resize(connection.destination_input + 1);
        per_input_connections[connection.destination_input].append(GraphExecutor::IndexedConnection {
            .source_node_index = source_index,
            .source_output = connection.source_output,
        });

        topology.dependents[source_index].append(destination_index);
    }

    for (VirtualParamConnection const& connection : virtual_param_connections) {
        if (virtual_node_removed[connection.source_node_index])
            continue;
        if (virtual_node_removed[connection.destination_node_index])
            continue;

        size_t const source_index = virtual_to_topology[connection.source_node_index].value();
        size_t const destination_index = virtual_to_topology[connection.destination_node_index].value();

        auto& per_param_connections = topology.param_inputs_by_param[destination_index];
        size_t const param_index = connection.destination_param_index;
        if (param_index >= per_param_connections.size())
            continue;
        per_param_connections[param_index].append(GraphExecutor::IndexedConnection {
            .source_node_index = source_index,
            .source_output = connection.source_output,
        });

        topology.dependents[source_index].append(destination_index);
    }

    // Ensure AudioWorklet nodes always expose the declared number of inputs,
    // even when they have no incoming connections.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        if (topology.nodes[node_index].node_type != GraphNodeType::AudioWorklet)
            continue;
        NodeID const node_id = executor.m_node_ids[topology.nodes[node_index].real_node_index];
        auto node_desc = description.nodes.get(node_id);
        if (node_desc.has_value() && node_desc->has<AudioWorkletGraphNode>()) {
            size_t const declared_inputs = node_desc->get<AudioWorkletGraphNode>().number_of_inputs;
            auto& per_input_connections = topology.inputs_by_input[node_index];
            if (per_input_connections.size() < declared_inputs)
                per_input_connections.resize(declared_inputs);
        }
    }

    // Pre-size scratch input bus pointer vectors so process() never resizes/appends.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto& per_input_connections = topology.inputs_by_input[node_index];
        auto& per_input_buses = topology.input_buses_scratch[node_index];
        auto& per_input_mix_buses = topology.input_mix_buses[node_index];
        per_input_buses.resize(per_input_connections.size());
        per_input_mix_buses.ensure_capacity(per_input_connections.size());
        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index)
            per_input_buses[input_index].resize(per_input_connections[input_index].size() + 1);

        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index)
            per_input_mix_buses.append(make<AudioBus>(1, executor.m_context.quantum_size, max_mixing_channel_count));

        auto& per_param_connections = topology.param_inputs_by_param[node_index];
        auto& per_param_buses = topology.param_input_buses_scratch[node_index];
        per_param_buses.resize(per_param_connections.size());
        for (size_t param_index = 0; param_index < per_param_connections.size(); ++param_index)
            per_param_buses[param_index].resize(per_param_connections[param_index].size() + 1);
    }

    size_t destination_node_index = 0;
    auto destination_index_it = executor.m_node_index_by_id.find(description.destination_node_id);
    if (destination_index_it != executor.m_node_index_by_id.end()) {
        size_t const destination_real = destination_index_it->value;
        Optional<size_t> destination_virtual = delay_in_cycle[destination_real] ? delay_reader_index_for_real[destination_real] : virtual_index_for_real[destination_real];
        if (destination_virtual.has_value() && !virtual_node_removed[destination_virtual.value()])
            destination_node_index = virtual_to_topology[destination_virtual.value()].value();
    }
    topology.destination_node_index = destination_node_index;

    Optional<size_t> listener_topology_index;
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        if (topology.nodes[node_index].node_type == GraphNodeType::AudioListener) {
            listener_topology_index = node_index;
            break;
        }
    }

    Vector<u8> marked;
    marked.resize(node_count);
    for (auto& value : marked)
        value = 0;

    ordered_node_list.ensure_capacity(node_count);

    auto visit_node = [&](auto& self, size_t node_index) -> void {
        // 4.2.8.1: If node is marked, abort these steps.
        if (marked[node_index])
            return;

        // 4.2.8.2: Mark node.
        marked[node_index] = 1;

        // 4.2.8.3: If node is an AudioNode, Visit each AudioNode connected to the input of node.
        if (topology.nodes[node_index].node_type != GraphNodeType::AudioListener) {
            auto const& per_input_connections = topology.inputs_by_input[node_index];
            for (auto const& connections : per_input_connections) {
                for (auto const& connection : connections)
                    self(self, connection.source_node_index);
            }
        }

        if (topology.nodes[node_index].node_type == GraphNodeType::Panner && listener_topology_index.has_value())
            self(self, listener_topology_index.value());

        // 4.2.8.4: For each AudioParam param of node: For each AudioNode param input node connected to param: Visit param input node.
        auto const& per_param_connections = topology.param_inputs_by_param[node_index];
        for (auto const& connections : per_param_connections) {
            for (auto const& connection : connections)
                self(self, connection.source_node_index);
        }

        // 4.2.8.5: Add node to the beginning of ordered node list.
        ordered_node_list.prepend(node_index);
    };

    // 4.2.8: Consider all elements in nodes to be unmarked. While there are unmarked elements in nodes: Choose an element node in nodes. Visit node.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        if (!marked[node_index])
            visit_node(visit_node, node_index);
    }

    // 4.2.9: Reverse the order of ordered node list.
    ordered_node_list.reverse();

    topology.processing_order = move(ordered_node_list);
}

void GraphCompiler::rebuild_output_cache_capacity(GraphExecutor& executor)
{
    ASSERT_CONTROL_THREAD();
    size_t const node_count = executor.m_topology ? executor.m_topology->nodes.size() : executor.m_nodes.size();
    executor.m_cached_outputs.resize(node_count);
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto& per_node_cache = executor.m_cached_outputs[node_index];
        RenderNode* node = nullptr;
        if (executor.m_topology && node_index < executor.m_topology->nodes.size())
            node = executor.m_topology->nodes[node_index].render_node;
        else if (node_index < executor.m_nodes.size())
            node = executor.m_nodes[node_index].ptr();

        size_t const output_count = node ? node->output_count() : 0;
        per_node_cache.resize(output_count);
        for (auto& entry : per_node_cache) {
            entry.generation = 0;
            entry.bus = nullptr;
        }
    }
}

}
