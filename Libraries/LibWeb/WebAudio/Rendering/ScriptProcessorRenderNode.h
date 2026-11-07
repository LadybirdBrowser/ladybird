/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
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
};

}
