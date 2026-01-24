/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Realtime/DestinationRenderNode.h>

#include <AK/StdLibExtras.h>

namespace Web::WebAudio::Realtime {

DestinationRenderNode::DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size)
    : RenderNode(node_id)
    , m_output(channel_count, quantum_size)
{
}

void DestinationRenderNode::process(RenderProcessContext&, Vector<Vector<AudioBus const*>> const& inputs)
{
    // FIXME: Implement summing / channel mixing rules. Currently copies only the first connected input.
    AudioBus const* input_bus = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        input_bus = inputs[0][0];

    m_output.zero();
    if (!input_bus)
        return;

    size_t const frames_to_copy = min(m_output.frame_count(), input_bus->frame_count());

    size_t const copy_channels = min(m_output.channel_count(), input_bus->channel_count());
    for (size_t ch = 0; ch < copy_channels; ++ch) {
        auto in = input_bus->channel(ch);
        auto out = m_output.channel(ch);
        in.slice(0, frames_to_copy).copy_to(out.slice(0, frames_to_copy));
    }
}

}
