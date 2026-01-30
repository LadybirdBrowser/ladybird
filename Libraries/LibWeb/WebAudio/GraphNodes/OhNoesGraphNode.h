/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API OhNoesGraphNode {
    String base_path;
    bool emit_enabled { true };
    bool strip_zero_buffers { false };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<OhNoesGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(OhNoesGraphNode const& new_desc) const;
};

}
