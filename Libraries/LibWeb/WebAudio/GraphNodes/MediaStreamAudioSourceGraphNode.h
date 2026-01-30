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

struct WEB_API MediaStreamAudioSourceGraphNode {
    MediaStreamAudioSourceProviderID provider_id { 0 };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<MediaStreamAudioSourceGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(MediaStreamAudioSourceGraphNode const& new_desc) const;
};

}
