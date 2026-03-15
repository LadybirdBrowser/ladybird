/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>
#include <LibWeb/WebAudio/RenderProcessContext.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Realtime {

class RenderNode;

class RenderGraphImpl {
public:
    explicit RenderGraphImpl(RenderGraphDescription const& description, f32 sample_rate);
    ~RenderGraphImpl();

    AudioBus const& render_destination_for_current_quantum();

    void begin_quantum(size_t current_frame);

private:
    friend class ::Web::WebAudio::RenderGraph;

    struct IndexedConnection {
        size_t source_node_index { 0 };
        size_t source_output { 0 };
    };

    struct Topology {
        size_t destination_node_index { 0 };

        // Node inputs, grouped by destination input index.
        Vector<Vector<Vector<IndexedConnection>>> inputs_by_input;

        // Preallocated scratch buffers to pass to RenderNode::process() (filled per quantum, no allocations).
        Vector<Vector<Vector<AudioBus const*>>> input_buses;

        // Topological order for processing needed nodes.
        Vector<size_t> ordered_node_list;
    };

    enum class VisitMark : u8 {
        Unvisited = 0,
        Visiting = 1,
        Visited = 2,
    };

    void process_a_render_quantum();

    void build_nodes(RenderGraphDescription const& description);
    OwnPtr<Topology> build_topology(RenderGraphDescription const& description);
    void compute_processing_order(Topology&);
    void visit_node(size_t node_index, Topology const& topology, Vector<VisitMark>& marks, bool& found_cycle, Vector<size_t>& ordered_nodes);
    RenderProcessContext m_context;

    HashMap<NodeID, size_t> m_node_index_by_id;
    Vector<OwnPtr<RenderNode>> m_nodes;

    OwnPtr<Topology> m_topology;
};

}
