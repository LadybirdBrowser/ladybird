/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebAudio/Engine/WireCodec.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResources;

struct ConvolverGraphNode {
    bool normalize { true };

    // Optional impulse response payload.
    ResourceID buffer_id;

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::ClampedMax };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<ConvolverGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResources const&) const;
    GraphUpdateKind classify_update(ConvolverGraphNode const& new_desc) const;
};

} // namespace Web::WebAudio::Render
