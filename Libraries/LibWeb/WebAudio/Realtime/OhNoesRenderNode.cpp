/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Realtime/OhNoesRenderNode.h>

namespace Web::WebAudio::Realtime {

OhNoesRenderNode::OhNoesRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(node_id)
    , m_output(1, quantum_size)
{
}

void OhNoesRenderNode::process(RenderProcessContext&, Vector<Vector<AudioBus const*>> const&)
{
    m_output.zero();
}

}
