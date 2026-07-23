/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessage.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Rendering {

// The rendering thread's mirror of the audio graph. Nodes and connections are added and updated exclusively through
// control messages, which are applied at the start of each render quantum.
// https://webaudio.github.io/web-audio-api/#rendering-loop
class WEB_API RenderGraph {
    AK_MAKE_NONCOPYABLE(RenderGraph);
    AK_MAKE_NONMOVABLE(RenderGraph);

public:
    RenderGraph() = default;

    void apply_control_messages(Vector<ControlMessage>);

    void render_quantum(RenderContext const&);
    AudioBus const* pull_output(NodeID source, size_t output_index, RenderContext const&);

    RenderNode* node(NodeID node_id);

    void set_destination(NodeID node_id) { m_destination_id = node_id; }
    RenderNode* destination() { return node(m_destination_id); }

    // Drains the set of nodes that stopped playing since the last call.
    Vector<NodeID> take_ended_nodes();

private:
    void rebuild_incoming_connections();

    HashMap<NodeID, NonnullOwnPtr<RenderNode>> m_nodes;
    NodeID m_destination_id { 0 };
    u64 m_current_quantum { 0 };
    bool m_incoming_connections_dirty { false };
};

}
