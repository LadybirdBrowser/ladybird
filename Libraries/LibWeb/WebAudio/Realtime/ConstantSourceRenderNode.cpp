/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Realtime/ConstantSourceRenderNode.h>

namespace Web::WebAudio::Realtime {

ConstantSourceRenderNode::ConstantSourceRenderNode(NodeID node_id, ConstantSourceRenderNodeDescription const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_offset(desc.offset)
    , m_start_frame(desc.start_frame)
    , m_stop_frame(desc.stop_frame)
    , m_output(1, quantum_size)
{
}

void ConstantSourceRenderNode::process(RenderProcessContext& context, Vector<Vector<AudioBus const*>> const&)
{
    m_output.zero();

    if (!m_start_frame.has_value())
        return;

    size_t const frames = m_output.frame_count();
    size_t const quantum_start = context.current_frame;

    size_t render_start = 0;
    if (quantum_start + frames <= m_start_frame.value())
        return;
    if (quantum_start < m_start_frame.value())
        render_start = m_start_frame.value() - quantum_start;

    size_t render_end = frames;
    if (m_stop_frame.has_value()) {
        if (quantum_start >= m_stop_frame.value())
            return;
        if (quantum_start + frames > m_stop_frame.value())
            render_end = m_stop_frame.value() - quantum_start;
    }

    if (render_start >= render_end)
        return;

    auto out = m_output.channel(0);

    for (size_t i = render_start; i < render_end; ++i)
        out[i] = m_offset;
}

}
