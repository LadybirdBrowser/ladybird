/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorPipe.h>

namespace Web::WebAudio::Rendering {

class ScriptProcessorRenderNode final : public RenderNode {
public:
    ScriptProcessorRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<ScriptProcessorPipe>);
    virtual ~ScriptProcessorRenderNode() override;

    virtual void process(RenderGraph&, RenderContext const&) override;

private:
    NonnullRefPtr<ScriptProcessorPipe> m_pipe;
    size_t m_block { 0 };
    size_t m_frame_in_block { 0 };
    u64 m_block_start_frame { 0 };
    bool m_block_is_owned { false };
};

}
