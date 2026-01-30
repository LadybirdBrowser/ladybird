/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

namespace Render {

class GraphExecutor;
class AudioBus;

}

// WEB_API for tests
class WEB_API RenderGraph {

public:
    explicit RenderGraph(Render::GraphDescription const& description, f32 sample_rate, size_t quantum_size = Render::RENDER_QUANTUM_SIZE, Render::GraphResourceResolver const* resources = nullptr);
    ~RenderGraph();

    Render::RenderContext& process_context();

    Render::AudioBus const& render_destination_for_current_quantum();
    void render_analysers_for_current_quantum();

    void begin_new_quantum(size_t current_frame);

    // Commit any pending graph updates at the current quantum boundary without processing audio.
    // This allows control-thread changes (graph/params) to become visible promptly on the render
    // thread even if the output buffering policy decides not to render more frames yet.
    void commit_pending_updates(size_t current_frame);

    // Incremented on the render thread when a full graph rebuild swap is committed.
    u32 generation() const { return m_generation.load(AK::MemoryOrder::memory_order_acquire); }

    // Render-thread hooks for AudioScheduledSourceNode control messages.
    // Scheduled frames are in the graph's timeline.
    void schedule_source_start(NodeID node_id, Optional<size_t> start_frame);
    void schedule_source_stop(NodeID node_id, Optional<size_t> stop_frame);

    // enqueue_full_rebuild rebuilds the whole graph and resets all node state. This works for
    // initialization and offline rendering, but it's not desirable for live AudioContext graph updates.
    void enqueue_full_rebuild(Render::GraphDescription const& description);

    // enqueue_topology_update tries to preserve per-node state. It returns false if the new description
    // changes the nodes beyond a pure connection update. Use enqueue_full_rebuild() in that case.
    bool enqueue_topology_update(Render::GraphDescription const& description);

    // enqueue_parameter_update preserves node state provided the update has no connection or
    // node set/type changes.
    bool enqueue_parameter_update(Render::GraphDescription const& description);

    // Uses classify_update to choose the update method, calls it, and returns what happened
    Render::GraphUpdateKind enqueue_update(Render::GraphDescription const& description);

    // apply_update rebuilds the graph and preserves node state but is not realtime-safe.
    // Only use it in offline contexts.
    void apply_update_offline(Render::GraphDescription const& description);
    // classify_update compares the provided description against the graph's current description and
    // returns the appropriate update mechanism.
    Render::GraphUpdateKind classify_update(Render::GraphDescription const& description) const;

    void collect_retired_updates();

    // analyser index is stable within a graph snapshot (until the next rebuild).
    size_t analyser_count() const;
    NodeID analyser_node_id(size_t analyser_index) const;
    bool copy_analyser_time_domain_data(size_t analyser_index, Span<f32> output) const;
    bool copy_analyser_frequency_data_db(size_t analyser_index, Span<f32> output) const;

private:
    void try_commit_pending_update();

    f32 m_sample_rate { 44100.0f };
    size_t m_quantum_size { Render::RENDER_QUANTUM_SIZE };
    Render::GraphResourceResolver const* m_resources { nullptr };

    Render::GraphExecutor* m_impl { nullptr };

    // Latest pending render graph prepared off-thread.
    Atomic<Render::GraphExecutor*> m_pending_impl { nullptr };

    // Retired graphs awaiting deletion.
    // The render thread can only commit a pending rebuild if there is a free retired slot to
    // hand the old implementation back to the control thread for deletion.
    static constexpr size_t retired_slot_count = 16;
    Array<Atomic<Render::GraphExecutor*>, retired_slot_count> m_retired_impls {};

    Atomic<u32> m_generation { 0 };
};

}
