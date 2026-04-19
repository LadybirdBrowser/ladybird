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

struct GainGraphNode {
    f32 gain { 1.0f };
    size_t channel_count { 1 };
    ChannelCountMode channel_count_mode { ChannelCountMode::Max };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<GainGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResources const&) const;
    GraphUpdateKind classify_update(GainGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(GainParamIndex::gain, gain, 0.0f, AK::NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(GainParamIndex::gain, gain);
    }
};

} // namespace Web::WebAudio::Render
