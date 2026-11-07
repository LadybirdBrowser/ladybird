/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio::Rendering {

RenderGraph::RenderGraph()
    : m_graph(RustFFI::web_audio_graph_create())
{
}
RenderGraph::~RenderGraph()
{
    if (m_owned)
        RustFFI::web_audio_graph_destroy(m_graph);
}
void RenderGraph::apply_control_messages(void* messages) { RustFFI::web_audio_graph_apply_control_messages(m_graph, messages); }
void RenderGraph::set_destination(NodeID node) { RustFFI::web_audio_graph_set_destination(m_graph, node.value()); }
void RenderGraph::render_quantum(RenderContext const& context)
{
    RustFFI::RenderContext rust_context { context.sample_rate, context.quantum_size, context.quantum_start_frame };
    RustFFI::web_audio_graph_render_quantum(m_graph, &rust_context);
}
Optional<AudioBus> RenderGraph::pull_output(NodeID source, size_t output_index, RenderContext const& context)
{
    RustFFI::RenderContext rust_context { context.sample_rate, context.quantum_size, context.quantum_start_frame };
    auto* bus = RustFFI::web_audio_graph_pull_output(m_graph, source.value(), output_index, &rust_context);
    if (!bus)
        return {};
    return AudioBus::borrow(bus);
}
Optional<AudioBus> RenderGraph::destination_output()
{
    auto* bus = RustFFI::web_audio_graph_destination_output(m_graph);
    if (!bus)
        return {};
    return AudioBus::borrow(bus);
}
Vector<NodeID> RenderGraph::take_ended_nodes()
{
    Vector<NodeID> nodes;
    RustFFI::web_audio_graph_take_ended_nodes(m_graph, &nodes, [](void* context, ReadonlySpan<u64> ended) {
        auto& nodes = *static_cast<Vector<NodeID>*>(context);
        for (auto node : ended)
            nodes.append(NodeID { node });
    });
    return nodes;
}

}
