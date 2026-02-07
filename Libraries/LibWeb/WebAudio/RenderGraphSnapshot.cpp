/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/RenderGraphSnapshot.h>

namespace Web::WebAudio {

static Optional<size_t> seconds_to_context_frames(Optional<double> seconds, double context_sample_rate)
{
    if (!seconds.has_value())
        return {};
    if (seconds.value() < 0)
        return {};
    return static_cast<size_t>(seconds.value() * context_sample_rate);
}

RenderGraphDescription snapshot_render_graph(GC::Ref<AudioNode> destination_node, double context_sample_rate)
{
    RenderGraphDescription graph;
    graph.destination_node_id = destination_node->node_id();

    HashTable<NodeID> visited;
    Vector<GC::Ref<AudioNode>> stack;
    stack.append(destination_node);

    while (!stack.is_empty()) {
        GC::Ref<AudioNode> node = stack.take_last();
        NodeID const node_id = node->node_id();
        if (visited.contains(node_id))
            continue;
        visited.set(node_id);

        RenderNodeDescription node_description;
        if (is<AudioDestinationNode>(*node)) {
            node_description.type = RenderNodeType::Destination;
            DestinationRenderNodeDescription dest_desc;
            dest_desc.channel_count = static_cast<size_t>(node->channel_count());
            node_description.destination = dest_desc;
        } else if (is<ConstantSourceNode>(*node)) {
            node_description.type = RenderNodeType::ConstantSource;
            ConstantSourceNode const& constant_source = static_cast<ConstantSourceNode const&>(*node);

            ConstantSourceRenderNodeDescription constant_desc;
            AudioScheduledSourceNode const& scheduled = static_cast<AudioScheduledSourceNode const&>(*node);
            constant_desc.start_frame = seconds_to_context_frames(scheduled.start_when_for_rendering(), context_sample_rate);
            constant_desc.stop_frame = seconds_to_context_frames(scheduled.stop_when_for_rendering(), context_sample_rate);
            constant_desc.offset = constant_source.offset()->value();

            node_description.constant_source = constant_desc;
        } else {
            node_description.type = RenderNodeType::Unknown;
        }

        graph.nodes.set(node_id, node_description);

        for (AudioNodeConnection const& connection : node->input_connections()) {
            NodeID const source_id = connection.destination_node->node_id();
            RenderConnection rc;
            rc.source = source_id;
            rc.destination = node_id;
            rc.source_output_index = static_cast<size_t>(connection.output);
            rc.destination_input_index = static_cast<size_t>(connection.input);
            graph.connections.append(rc);
            stack.append(connection.destination_node);
        }

        // FIXME: handle AudioParam connections
    }

    // Ensure deterministic ordering so realtime RenderGraph updates can be classified reliably.
    quick_sort(graph.connections, [](auto const& a, auto const& b) {
        if (a.source != b.source)
            return a.source < b.source;
        if (a.destination != b.destination)
            return a.destination < b.destination;
        if (a.source_output_index != b.source_output_index)
            return a.source_output_index < b.source_output_index;
        return a.destination_input_index < b.destination_input_index;
    });

    return graph;
}

}
