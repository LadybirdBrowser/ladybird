/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Realtime/RenderNode.h>

namespace Web::WebAudio::Realtime {

class ConstantSourceRenderNode final : public RenderNode {
public:
    ConstantSourceRenderNode(NodeID node_id, ConstantSourceRenderNodeDescription const& desc, size_t quantum_size);

    void process(RenderProcessContext& context, Vector<Vector<AudioBus const*>> const& inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }

private:
    f32 m_offset { 1.0f };
    Optional<size_t> m_start_frame;
    Optional<size_t> m_stop_frame;

    AudioBus m_output;
};

}
