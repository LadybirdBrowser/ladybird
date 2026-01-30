/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/ConstantSourceRenderNode.h>

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>

namespace Web::WebAudio::Render {

ConstantSourceRenderNode::ConstantSourceRenderNode(NodeID node_id, ConstantSourceGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_offset(desc.offset)
    , m_start_frame(desc.start_frame)
    , m_stop_frame(desc.stop_frame)
    , m_output(1, quantum_size)
    , m_offset_input(1, quantum_size)
{
}

void ConstantSourceRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#ConstantSourceNode
    m_output.zero();
    // ConstantSourceNode produces output only after it has been started.
    if (!m_start_frame.has_value()) {
        m_output.set_channel_count(0);
        return;
    }

    // Determine active range within this quantum using the graph timeline.
    // NOTE: We rely on m_output.frame_count() and param bus sizes matching quantum size.
    size_t const frames = m_output.frame_count();
    size_t const quantum_start = context.current_frame;

    size_t render_start = 0;
    if (quantum_start + frames <= m_start_frame.value()) {
        m_output.set_channel_count(0);
        return;
    }
    if (quantum_start < m_start_frame.value())
        render_start = m_start_frame.value() - quantum_start;

    size_t render_end = frames;
    if (m_stop_frame.has_value()) {
        if (quantum_start >= m_stop_frame.value()) {
            m_output.set_channel_count(0);
            return;
        }
        if (quantum_start + frames > m_stop_frame.value())
            render_end = m_stop_frame.value() - quantum_start;
    }

    if (render_start >= render_end) {
        m_output.set_channel_count(0);
        return;
    }

    // ConstantSourceNode output is mono when active and has no channels when inactive.
    m_output.set_channel_count(1);

    if (param_inputs.size() > ConstantSourceParamIndex::offset)
        mix_inputs_into(m_offset_input, param_inputs[ConstantSourceParamIndex::offset].span());
    else
        m_offset_input.zero();

    auto offset_in = m_offset_input.channel(0);
    auto out = m_output.channel(0);

    bool const has_offset_param_input = param_inputs.size() > ConstantSourceParamIndex::offset && !param_inputs[ConstantSourceParamIndex::offset].is_empty();

    for (size_t i = render_start; i < render_end; ++i)
        out[i] = has_offset_param_input ? offset_in[i] : m_offset;
}

void ConstantSourceRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<ConstantSourceGraphNode>())
        return;

    auto const& desc = node.get<ConstantSourceGraphNode>();
    m_offset = desc.offset;
    m_start_frame = desc.start_frame;
    m_stop_frame = desc.stop_frame;
}

}
