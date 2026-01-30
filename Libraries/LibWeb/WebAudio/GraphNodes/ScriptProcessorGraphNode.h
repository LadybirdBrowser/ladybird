/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API ScriptProcessorGraphNode {
    size_t buffer_size { 1024 };
    size_t input_channel_count { 1 };
    size_t output_channel_count { 1 };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<ScriptProcessorGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(ScriptProcessorGraphNode const& new_desc) const;
};

}
