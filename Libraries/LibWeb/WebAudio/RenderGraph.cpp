/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphExecutor.h>
#include <LibWeb/WebAudio/RenderGraph.h>

namespace Web::WebAudio {

using namespace Render;

RenderGraph::RenderGraph(GraphDescription const& description, f32 sample_rate, size_t quantum_size, GraphResourceResolver const* resources)
    : m_sample_rate(sample_rate)
    , m_quantum_size(quantum_size)
    , m_resources(resources)
{
    m_impl = new GraphExecutor(description, sample_rate, m_quantum_size, resources);
}

RenderGraph::~RenderGraph()
{
    auto collect_retired_updates_for_teardown = [&] {
        for (auto& slot : m_retired_impls)
            delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
        if (m_impl)
            m_impl->collect_retired_updates_for_teardown();
    };

    collect_retired_updates_for_teardown();
    delete m_pending_impl.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    collect_retired_updates_for_teardown();
    delete m_impl;
    m_impl = nullptr;
}

RenderContext& RenderGraph::process_context()
{
    ASSERT_RENDER_THREAD();
    return m_impl->process_context();
}

AudioBus const& RenderGraph::render_destination_for_current_quantum()
{
    ASSERT_RENDER_THREAD();
    return m_impl->render_destination_for_current_quantum();
}

void RenderGraph::render_analysers_for_current_quantum()
{
    ASSERT_RENDER_THREAD();
    m_impl->render_analysers_for_current_quantum();
}

void RenderGraph::begin_new_quantum(size_t current_frame)
{
    ASSERT_RENDER_THREAD();
    try_commit_pending_update();
    m_impl->begin_new_quantum(current_frame);
}

void RenderGraph::commit_pending_updates(size_t current_frame)
{
    ASSERT_RENDER_THREAD();
    try_commit_pending_update();
    m_impl->commit_pending_updates(current_frame);
}

void RenderGraph::schedule_source_start(NodeID node_id, Optional<size_t> start_frame)
{
    ASSERT_RENDER_THREAD();
    m_impl->schedule_source_start(node_id, start_frame);
}

void RenderGraph::schedule_source_stop(NodeID node_id, Optional<size_t> stop_frame)
{
    ASSERT_RENDER_THREAD();
    m_impl->schedule_source_stop(node_id, stop_frame);
}

void RenderGraph::enqueue_full_rebuild(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    // If a prepared graph was already pending, discard it here.
    auto* new_impl = new GraphExecutor(description, m_sample_rate, m_quantum_size, m_resources);
    delete m_pending_impl.exchange(new_impl, AK::MemoryOrder::memory_order_acq_rel);
}

bool RenderGraph::enqueue_topology_update(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    return m_impl->enqueue_topology_update(description);
}

bool RenderGraph::enqueue_parameter_update(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    return m_impl->enqueue_parameter_update(description);
}

GraphUpdateKind RenderGraph::classify_update(GraphDescription const& description) const
{
    ASSERT_CONTROL_THREAD();
    return m_impl->classify_update(description);
}

GraphUpdateKind RenderGraph::enqueue_update(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    auto classification = classify_update(description);

    WA_DBGLN("[WebAudio] enqueue_update: kind={} nodes={} conns={} param_conns={} param_autos={} dest_id={}",
        static_cast<u8>(classification),
        description.nodes.size(),
        description.connections.size(),
        description.param_connections.size(),
        description.param_automations.size(),
        description.destination_node_id);

    if (classification == GraphUpdateKind::None) {
        // If a rebuild is pending, but the newest snapshot matches the current graph, the pending
        // rebuild is necessarily stale (it would move the graph away from the current state).
        // Cancel it to avoid committing a transient disconnected graph.
        if (auto* pending = m_pending_impl.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel)) {
            WA_DBGLN("[WebAudio] enqueue_update: canceled stale pending rebuild (classification=None)");
            delete pending;
        }
        return classification;
    }
    if (classification == GraphUpdateKind::Parameter) {
        if (enqueue_parameter_update(description)) {
            if (auto* pending = m_pending_impl.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel)) {
                WA_DBGLN("[WebAudio] enqueue_update: canceled stale pending rebuild (applied ParameterUpdate)");
                delete pending;
            }
            WA_DBGLN("[WebAudio] enqueue_update: applied ParameterUpdate");
            return classification;
        }
    } else if (classification == GraphUpdateKind::Topology) {
        if (enqueue_topology_update(description)) {
            if (auto* pending = m_pending_impl.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel)) {
                WA_DBGLN("[WebAudio] enqueue_update: canceled stale pending rebuild (applied TopologyUpdate)");
                delete pending;
            }
            WA_DBGLN("[WebAudio] enqueue_update: applied TopologyUpdate");
            return classification;
        }
    }

    WA_DBGLN("[WebAudio] enqueue_update: fell back to full rebuild");

    enqueue_full_rebuild(description);
    return GraphUpdateKind::RebuildRequired;
}

void RenderGraph::collect_retired_updates()
{
    ASSERT_CONTROL_THREAD();
    for (auto& slot : m_retired_impls) {
        delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    }
    if (m_impl)
        m_impl->collect_retired_updates();
}

size_t RenderGraph::analyser_count() const
{
    ASSERT_RENDER_THREAD();
    return m_impl->analyser_count();
}

NodeID RenderGraph::analyser_node_id(size_t analyser_index) const
{
    ASSERT_RENDER_THREAD();
    return m_impl->analyser_node_id(analyser_index);
}

bool RenderGraph::copy_analyser_time_domain_data(size_t analyser_index, Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    return m_impl->copy_analyser_time_domain_data(analyser_index, output);
}

bool RenderGraph::copy_analyser_frequency_data_db(size_t analyser_index, Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    return m_impl->copy_analyser_frequency_data_db(analyser_index, output);
}

void RenderGraph::apply_update_offline(GraphDescription const& description)
{
    ASSERT_RENDER_THREAD();
    m_impl->apply_update_offline(description);
}

void RenderGraph::try_commit_pending_update()
{
    ASSERT_RENDER_THREAD();
    size_t free_slot_index = retired_slot_count;
    for (size_t i = 0; i < retired_slot_count; ++i) {
        if (m_retired_impls[i].load(AK::MemoryOrder::memory_order_acquire) == nullptr) {
            free_slot_index = i;
            break;
        }
    }
    if (free_slot_index == retired_slot_count) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) > 1000 && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
            WA_DBGLN("[WebAudio] commit stalled: rebuild retired slots full");
        return;
    }

    auto* pending = m_pending_impl.load(AK::MemoryOrder::memory_order_acquire);
    if (!pending)
        return;

    pending = m_pending_impl.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (!pending)
        return;

    // Only the render thread writes non-null pointers into retired slots.
    // The control thread may concurrently exchange() them back to nullptr for deletion.
    m_retired_impls[free_slot_index].store(m_impl, AK::MemoryOrder::memory_order_release);
    m_impl = pending;

    auto new_generation = m_generation.fetch_add(1, AK::MemoryOrder::memory_order_acq_rel) + 1;
    WA_DBGLN("[WebAudio] committed full rebuild: generation={} retired_slot={}", new_generation, free_slot_index);
}

}
