/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWebAudio/Engine/WireCodec.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResources;

struct OhNoesGraphNode {
    String base_path;
    bool emit_enabled { true };
    bool strip_zero_buffers { false };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<OhNoesGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResources const&) const;
    GraphUpdateKind classify_update(OhNoesGraphNode const& new_desc) const;
};

} // namespace Web::WebAudio::Render
