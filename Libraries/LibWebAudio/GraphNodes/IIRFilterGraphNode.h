/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWebAudio/Engine/WireCodec.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResources;

struct IIRFilterGraphNode {
    Vector<f64> feedforward;
    Vector<f64> feedback;

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::Max };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<IIRFilterGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResources const&) const;
    GraphUpdateKind classify_update(IIRFilterGraphNode const& new_desc) const;
};

} // namespace Web::WebAudio::Render
