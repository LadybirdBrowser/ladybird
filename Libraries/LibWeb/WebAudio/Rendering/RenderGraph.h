/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
namespace Web::WebAudio::Rendering {

class WEB_API RenderGraph {
    AK_MAKE_NONCOPYABLE(RenderGraph);
    AK_MAKE_NONMOVABLE(RenderGraph);

public:
    RenderGraph();
    RenderGraph(void* graph, bool owned)
        : m_graph(graph)
        , m_owned(owned)
    {
    }
    ~RenderGraph();
    void apply_control_messages(void* messages);
    void render_quantum(RenderContext const&);
    Optional<AudioBus> pull_output(NodeID source, size_t output_index, RenderContext const&);
    Optional<AudioBus> destination_output();
    void set_destination(NodeID);
    Vector<NodeID> take_ended_nodes();
    void* rust_graph() const { return m_graph; }

private:
    void* m_graph { nullptr };
    bool m_owned { true };
};

}
