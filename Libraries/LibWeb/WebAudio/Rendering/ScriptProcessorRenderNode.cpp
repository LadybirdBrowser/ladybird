/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/ScriptProcessorRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

ScriptProcessorRenderNode::ScriptProcessorRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<ScriptProcessorPipe> pipe)
    : RenderNode(RustFFI::web_audio_node_create_script_processor(node_id.value(), quantum_size, pipe->rust_pipe()))
{
}

}
