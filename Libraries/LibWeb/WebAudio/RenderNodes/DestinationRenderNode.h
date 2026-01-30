/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

// https://webaudio.github.io/web-audio-api/#AudioDestinationNode
// The destination node is the final sink of the audio graph.
class DestinationRenderNode final : public RenderNode {
public:
    DestinationRenderNode(NodeID node_id, size_t channel_count, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override;

private:
    AudioBus m_output;
};

}
