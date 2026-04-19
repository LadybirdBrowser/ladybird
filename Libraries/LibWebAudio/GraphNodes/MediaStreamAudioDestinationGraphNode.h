/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebAudio/Engine/WireCodec.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResources;

struct MediaStreamAudioDestinationGraphNode {
    MediaStreamAudioSourceProviderID provider_id { 0 };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<MediaStreamAudioDestinationGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID node_id, size_t quantum_size, GraphResources const&) const;
    GraphUpdateKind classify_update(MediaStreamAudioDestinationGraphNode const& new_desc) const;
};

} // namespace Web::WebAudio::Render