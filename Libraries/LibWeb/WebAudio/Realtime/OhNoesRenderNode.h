/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Realtime/RenderNode.h>

namespace Web::WebAudio::Realtime {

class OhNoesRenderNode final : public RenderNode {
public:
    OhNoesRenderNode(NodeID node_id, size_t quantum_size);

    void process(RenderProcessContext&, Vector<Vector<AudioBus const*>> const&) override;
    AudioBus const& output(size_t) const override { return m_output; }

private:
    AudioBus m_output;
};

}
