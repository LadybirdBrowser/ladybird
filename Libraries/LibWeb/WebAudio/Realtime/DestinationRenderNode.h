/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Realtime/RenderNode.h>

namespace Web::WebAudio::Realtime {

// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
// The destination node is the final sink of the audio graph.
class DestinationRenderNode final : public RenderNode {
public:
    DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size);

    void process(RenderProcessContext&, Vector<Vector<AudioBus const*>> const& inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }

private:
    AudioBus m_output;
};

}
