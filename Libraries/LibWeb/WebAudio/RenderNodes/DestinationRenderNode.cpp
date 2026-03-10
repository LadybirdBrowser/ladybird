/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/DestinationRenderNode.h>

#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio::Render {

DestinationRenderNode::DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size)
    : RenderNode(node_id)
    , m_output(channel_count, quantum_size)
{
}

void DestinationRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    m_output.zero();
    if (!mixed_input)
        return;

    size_t const frames = m_output.frame_count();
    size_t const copy_channels = min(m_output.channel_count(), mixed_input->channel_count());
    for (size_t ch = 0; ch < copy_channels; ++ch) {
        auto in = mixed_input->channel(ch);
        auto out = m_output.channel(ch);
        in.slice(0, frames).copy_to(out.slice(0, frames));
    }
}

AudioBus const& DestinationRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_output;
}

}
