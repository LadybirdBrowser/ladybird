/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/RenderGraph.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphCompiler;

class WEB_API GraphExecutor {
public:
    explicit GraphExecutor(GraphDescription const& description, f32 sample_rate, size_t quantum_size, GraphResourceResolver const* resources = nullptr);
    ~GraphExecutor();

    RenderContext& process_context();

    AudioBus const& render_destination_for_current_quantum();
    void render_analysers_for_current_quantum();

    void begin_new_quantum(size_t current_frame);

    // Commit pending topology/parameter updates and set the render-thread current frame without
    // advancing the per-quantum cache generation. This is used to make control-thread changes
    // visible promptly even when we are not producing new audio frames.
    void commit_pending_updates(size_t current_frame);

    // Render-thread hook for AudioScheduledSourceNode control messages.
    void schedule_source_start(NodeID node_id, Optional<size_t> start_frame);
    void schedule_source_stop(NodeID node_id, Optional<size_t> stop_frame);

    bool enqueue_topology_update(GraphDescription const& description);
    bool enqueue_parameter_update(GraphDescription const& description);

    GraphUpdateKind classify_update(GraphDescription const& description) const;

    void collect_retired_updates();

    // analyser index is stable within a graph snapshot (until the next rebuild).
    size_t analyser_count() const;
    NodeID analyser_node_id(size_t analyser_index) const;
    bool copy_analyser_time_domain_data(size_t analyser_index, Span<f32> output) const;
    bool copy_analyser_frequency_data_db(size_t analyser_index, Span<f32> output) const;

    bool try_copy_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db) const;

    // apply_update_offline rebuilds the graph and preserves node state but is not realtime-safe.
    // Only use it in offline contexts.
    void apply_update_offline(GraphDescription const& description);

private:
    friend class ::Web::WebAudio::RenderGraph;
    friend class GraphCompiler;

    void collect_retired_updates_for_teardown();

    GraphResourceResolver const& resources() const
    {
        return m_resources ? *m_resources : NullGraphResourceResolver::the();
    }
    struct IndexedConnection {
        size_t source_node_index { 0 };
        size_t source_output { 0 };
    };

    struct Topology {
        enum class ProcessingNodeKind : u8 {
            Real,
            DelayWriter,
            DelayReader,
        };

        struct ProcessingNode {
            ProcessingNodeKind kind { ProcessingNodeKind::Real };
            size_t real_node_index { 0 };
            size_t param_owner_node_index { 0 };
            GraphNodeType node_type { GraphNodeType::Unknown };
            RenderNode* render_node { nullptr };
        };

        struct ChannelMixingSettings {
            size_t channel_count { 1 };
            ChannelCountMode channel_count_mode { ChannelCountMode::Max };
            ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };
        };

        size_t destination_node_index { 0 };

        Vector<ProcessingNode> nodes;

        // Snapshot of the connection lists used to build this topology.
        // Stored so committing a topology update can also update m_description.
        Vector<GraphConnection> connections;
        Vector<GraphParamConnection> param_connections;

        // Node inputs, grouped by destination input index.
        Vector<Vector<Vector<IndexedConnection>>> inputs_by_input;

        // AudioParam inputs, grouped by destination param id.
        Vector<Vector<Vector<IndexedConnection>>> param_inputs_by_param;

        // Preallocated scratch buffers to pass to RenderNode::process() (filled per quantum, no allocations).
        Vector<Vector<Vector<AudioBus const*>>> input_buses_scratch;

        // Per-node input mixing settings (used to mix all incoming audio at graph edges).
        Vector<ChannelMixingSettings> channel_mixing_by_node;

        // Per-node per-input mixed audio buses (slot 0 of input_buses_scratch).
        Vector<Vector<NonnullOwnPtr<AudioBus>>> input_mix_buses;

        // Preallocated scratch buffers for AudioParam inputs (filled per quantum, no allocations).
        Vector<Vector<Vector<AudioBus const*>>> param_input_buses_scratch;

        // Adjacency list for topological ordering: source -> destinations.
        Vector<Vector<size_t>> dependents;

        // Topological order for processing needed nodes.
        Vector<size_t> processing_order;
    };

    struct ParameterUpdateBatch {
        Vector<GraphNodeDescription> nodes_by_index;
        Vector<GraphParamAutomation> param_automations;
    };

    struct ParamAutomationState {
        f32 initial_value { 0.0f };
        f32 default_value { 0.0f };
        f32 min_value { 0.0f };
        f32 max_value { 0.0f };
        AutomationRate automation_rate { AutomationRate::ARate };

        size_t current_segment_index { 0 };
        Vector<GraphAutomationSegment> segments;
    };

    struct CachedOutput {
        u64 generation { 0 };
        AudioBus const* bus { nullptr };
    };

    void process_graph_if_needed();

    void rebuild_output_cache_capacity_for_topology();

    void try_commit_pending_topology();
    void try_commit_pending_parameter_updates();

    GraphDescription m_description;
    GraphResourceResolver const* m_resources { nullptr };
    RenderContext m_context;

    HashMap<NodeID, size_t> m_node_index_by_id;
    Vector<NodeID> m_node_ids;
    Vector<GraphNodeType> m_node_types_by_index;
    Vector<OwnPtr<RenderNode>> m_nodes;

    Topology* m_topology { nullptr };

    // Retired update slots must be large enough that the render thread does not stall applying
    // topology/parameter updates if the control thread hasn't collected retired pointers yet.
    // Stalling commits can leave the graph in an old (potentially disconnected) state.
    static constexpr size_t topology_retired_slot_count = 16;
    Atomic<Topology*> m_pending_topology { nullptr };
    Array<Atomic<Topology*>, topology_retired_slot_count> m_retired_topologies {};

    Atomic<ParameterUpdateBatch*> m_pending_parameter_updates { nullptr };
    Array<Atomic<ParameterUpdateBatch*>, topology_retired_slot_count> m_retired_parameter_updates {};

    Vector<size_t> m_analyser_node_indices;

    // Per-node per-param automation state and corresponding implicit param buses.
    Vector<Vector<ParamAutomationState>> m_param_automation_state;
    Vector<Vector<NonnullOwnPtr<AudioBus>>> m_param_automation_buses;

    mutable Vector<Vector<CachedOutput>> m_cached_outputs;
    u64 m_cache_generation { 1 };
    u64 m_last_processed_generation { 0 };
};

}
