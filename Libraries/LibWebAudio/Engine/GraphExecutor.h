/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebAudio/Engine/GraphDescription.h>
#include <LibWebAudio/Engine/GraphResources.h>
#include <LibWebAudio/Engine/RenderContext.h>

namespace Web::WebAudio::Render {

class AudioBus;

class GraphExecutor {
public:
    explicit GraphExecutor(GraphDescription const& description, f32 sample_rate, size_t quantum_size,
        GraphResources const& resources);
    ~GraphExecutor();

    RenderContext& process_context();

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
    void collect_retired_updates_for_teardown();

    // analyser indices are stable only within a graph snapshot (until the next rebuild).
    size_t analyser_count() const;
    NodeID analyser_node_id(size_t analyser_index) const;
    bool copy_analyser_time_domain_data(size_t analyser_index, Span<f32> output) const;
    bool copy_analyser_frequency_data_db(size_t analyser_index, Span<f32> output) const;
    void render_analysers_for_current_quantum();

    AudioBus const& render_destination_for_current_quantum();
    bool try_copy_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db) const;

    // apply_update_offline rebuilds the graph and preserves node state but is not realtime-safe.
    void apply_update_offline(GraphDescription const& description);

private:
    struct ParameterUpdateBatch;

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
    GraphResources const& m_resources;
    RenderContext m_context;

    RenderNodeSet m_nodes;

    GraphTopology* m_topology { nullptr };

    // Retired update slots must be large enough that the render thread does not stall applying
    // topology/parameter updates if the control thread hasn't collected retired pointers yet.
    // Stalling commits can leave the graph in an old (potentially disconnected) state.
    static constexpr size_t topology_retired_slot_count = 16;
    Atomic<GraphTopology*> m_pending_topology { nullptr };
    Array<Atomic<GraphTopology*>, topology_retired_slot_count> m_retired_topologies {};

    Atomic<ParameterUpdateBatch*> m_pending_parameter_updates { nullptr };
    Array<Atomic<ParameterUpdateBatch*>, topology_retired_slot_count> m_retired_parameter_updates {};

    // Per-node per-param automation state and corresponding implicit param buses.
    Vector<Vector<ParamAutomationState>> m_param_automation_state;
    Vector<Vector<NonnullOwnPtr<AudioBus>>> m_param_automation_buses;

    mutable Vector<Vector<CachedOutput>> m_cached_outputs;
    u64 m_cache_generation { 1 };
    u64 m_last_processed_generation { 0 };
};

} // namespace Web::WebAudio::Render
