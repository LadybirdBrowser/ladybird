/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Engine/GraphCompiler.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

using namespace Render;

namespace {

OwnPtr<Render::RenderNode> make_render_node(GraphDescription const&, NodeID node_id, GraphNodeDescription const& node, size_t quantum_size, GraphResourceResolver const& resources)
{
    return node.visit([&](auto const& payload) {
        return payload.make_render_node(node_id, quantum_size, resources);
    });
}

GraphUpdateKind classify_graph_update(GraphDescription const& old_desc, GraphDescription const& new_desc)
{
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
    return classify_graph_update(old_description, new_description);
}

void GraphCompiler::build_nodes(GraphExecutor& executor, GraphResourceResolver const& resources)
{
    // control thread
    executor.m_node_ids.ensure_capacity(executor.m_description.nodes.size());
    executor.m_node_types_by_index.ensure_capacity(executor.m_description.nodes.size());
    executor.m_nodes.ensure_capacity(executor.m_description.nodes.size());

    for (auto const& it : executor.m_description.nodes) {
        NodeID const node_id = it.key;
        GraphNodeDescription const& node = it.value;

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
    // control thread
    auto* topology = new GraphExecutor::Topology;

    static constexpr size_t max_mixing_channel_count = 32;

    topology->connections = description.connections;
    topology->param_connections = description.param_connections;

    size_t const node_count = executor.m_nodes.size();
    topology->inputs_by_input.resize(node_count);
    topology->param_inputs_by_param.resize(node_count);
    topology->input_buses_scratch.resize(node_count);
    topology->param_input_buses_scratch.resize(node_count);
    topology->channel_mixing_by_node.resize(node_count);
    topology->input_mix_buses.resize(node_count);
    topology->dependents.resize(node_count);
    topology->is_needed.resize(node_count);

    // Build local id->index mapping.
    HashMap<NodeID, size_t> node_index_by_id;
    node_index_by_id.ensure_capacity(node_count);
    for (size_t i = 0; i < node_count; ++i)
        node_index_by_id.set(executor.m_node_ids[i], i);

    // Ensure AudioWorklet nodes always expose the declared number of inputs,
    // even when they have no incoming connections.
    auto destination_it = node_index_by_id.find(description.destination_node_id);
    if (destination_it == node_index_by_id.end()) {
        topology->destination_node_index = 0;
    } else {
        topology->destination_node_index = destination_it->value;
    }

    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        topology->inputs_by_input[node_index].clear();
        topology->param_inputs_by_param[node_index].clear();
        topology->input_buses_scratch[node_index].clear();
        topology->param_input_buses_scratch[node_index].clear();
        topology->input_mix_buses[node_index].clear();
        topology->dependents[node_index].clear();

        if (executor.m_node_types_by_index[node_index] == GraphNodeType::AudioWorklet) {
            auto const node_id = executor.m_node_ids[node_index];
            auto node_desc = description.nodes.get(node_id);
            if (node_desc.has_value() && node_desc->has<AudioWorkletGraphNode>()) {
                size_t const declared_inputs = node_desc->get<AudioWorkletGraphNode>().number_of_inputs;
                topology->inputs_by_input[node_index].resize(declared_inputs);
            }
        }

        // Ensure param inputs use a stable per-node layout.
        auto const node_type = executor.m_node_types_by_index[node_index];
        size_t param_count = RenderParamLayout::param_count(node_type);
        if (node_type == GraphNodeType::AudioWorklet) {
            auto const node_id = executor.m_node_ids[node_index];
            auto maybe_node_desc = description.nodes.get(node_id);
            if (maybe_node_desc.has_value() && maybe_node_desc->has<AudioWorkletGraphNode>())
                param_count = maybe_node_desc->get<AudioWorkletGraphNode>().parameter_names.size();
        }
        topology->param_inputs_by_param[node_index].resize(param_count);
        topology->param_input_buses_scratch[node_index].resize(param_count);
    }

    // Capture per-node channel mixing settings from the description.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto const node_id = executor.m_node_ids[node_index];
        auto maybe_node_desc = description.nodes.get(node_id);
        if (!maybe_node_desc.has_value()) {
            topology->channel_mixing_by_node[node_index] = {};
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
        topology->channel_mixing_by_node[node_index] = settings;
    }

    // Build per-destination-input connection buckets.
    for (GraphConnection const& c : description.connections) {
        auto destination_index_it = node_index_by_id.find(c.destination);
        if (destination_index_it == node_index_by_id.end())
            continue;
        auto source_index_it = node_index_by_id.find(c.source);
        if (source_index_it == node_index_by_id.end())
            continue;

        auto& per_input_connections = topology->inputs_by_input[destination_index_it->value];
        if (per_input_connections.size() <= c.destination_input_index)
            per_input_connections.resize(c.destination_input_index + 1);
        per_input_connections[c.destination_input_index].append(GraphExecutor::IndexedConnection {
            .source_node_index = source_index_it->value,
            .source_output = c.source_output_index,
        });

        topology->dependents[source_index_it->value].append(destination_index_it->value);
    }

    // Build per-destination-param connection buckets.
    for (GraphParamConnection const& c : description.param_connections) {
        auto destination_index_it = node_index_by_id.find(c.destination);
        if (destination_index_it == node_index_by_id.end())
            continue;
        auto source_index_it = node_index_by_id.find(c.source);
        if (source_index_it == node_index_by_id.end())
            continue;

        auto& per_param_connections = topology->param_inputs_by_param[destination_index_it->value];
        size_t const param_index = c.destination_param_index;
        if (param_index >= per_param_connections.size())
            continue;
        per_param_connections[param_index].append(GraphExecutor::IndexedConnection {
            .source_node_index = source_index_it->value,
            .source_output = c.source_output_index,
        });

        topology->dependents[source_index_it->value].append(destination_index_it->value);
    }

    // Pre-size scratch input bus pointer vectors so process() never resizes/appends.
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto& per_input_connections = topology->inputs_by_input[node_index];
        auto& per_input_buses = topology->input_buses_scratch[node_index];
        auto& per_input_mix_buses = topology->input_mix_buses[node_index];
        per_input_buses.resize(per_input_connections.size());
        per_input_mix_buses.ensure_capacity(per_input_connections.size());
        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index)
            per_input_buses[input_index].resize(per_input_connections[input_index].size() + 1);

        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index)
            per_input_mix_buses.append(make<AudioBus>(1, executor.m_context.quantum_size, max_mixing_channel_count));

        auto& per_param_connections = topology->param_inputs_by_param[node_index];
        auto& per_param_buses = topology->param_input_buses_scratch[node_index];
        per_param_buses.resize(per_param_connections.size());
        for (size_t param_index = 0; param_index < per_param_connections.size(); ++param_index)
            per_param_buses[param_index].resize(per_param_connections[param_index].size() + 1);
    }

    rebuild_processing_order(executor, *topology);
    return topology;
}

void GraphCompiler::rebuild_processing_order(GraphExecutor& executor, GraphExecutor::Topology& topology)
{
    // control thread
    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // 4.2: order the AudioNodes of the BaseAudioContext to be processed.
    // NOTE: The spec's ordering algorithm includes:
    // - splitting DelayNodes that are part of a cycle into DelayWriter/DelayReader, and
    // - muting and removing remaining cycles.
    // Our current implementation produces a topological ordering for all nodes; it does not yet
    // implement DelayNode cycle-breakers or the cycle muting algorithm.
    VERIFY(topology.is_needed.size() == executor.m_nodes.size());

    for (auto& value : topology.is_needed)
        value = 1;

    size_t const needed_count = topology.is_needed.size();

    Vector<size_t> indegree;
    indegree.resize(executor.m_nodes.size());
    for (auto& value : indegree)
        value = 0;

    for (size_t destination_index = 0; destination_index < executor.m_nodes.size(); ++destination_index) {
        if (!topology.is_needed[destination_index])
            continue;
        auto const& per_input_connections = topology.inputs_by_input[destination_index];
        for (auto const& connections : per_input_connections) {
            for (auto const& c : connections) {
                if (topology.is_needed[c.source_node_index])
                    ++indegree[destination_index];
            }
        }

        auto const& per_param_connections = topology.param_inputs_by_param[destination_index];
        for (auto const& connections : per_param_connections) {
            for (auto const& c : connections) {
                if (topology.is_needed[c.source_node_index])
                    ++indegree[destination_index];
            }
        }
    }

    Vector<size_t> ready;
    ready.ensure_capacity(executor.m_nodes.size());
    for (size_t node_index = 0; node_index < executor.m_nodes.size(); ++node_index) {
        if (topology.is_needed[node_index] && indegree[node_index] == 0)
            ready.append(node_index);
    }

    topology.processing_order.clear();
    topology.processing_order.ensure_capacity(needed_count);

    while (!ready.is_empty()) {
        auto const node_index = ready.take_last();
        topology.processing_order.append(node_index);
        for (auto const dependent_index : topology.dependents[node_index]) {
            if (!topology.is_needed[dependent_index])
                continue;
            VERIFY(indegree[dependent_index] > 0);
            if (--indegree[dependent_index] == 0)
                ready.append(dependent_index);
        }
    }
}

void GraphCompiler::rebuild_output_cache_capacity(GraphExecutor& executor)
{
    // control thread
    executor.m_cached_outputs.resize(executor.m_nodes.size());
    for (size_t node_index = 0; node_index < executor.m_nodes.size(); ++node_index) {
        auto& per_node_cache = executor.m_cached_outputs[node_index];
        auto output_count = executor.m_nodes[node_index]->output_count();
        per_node_cache.resize(output_count);
        for (auto& entry : per_node_cache) {
            entry.generation = 0;
            entry.bus = nullptr;
        }
    }
}

}
