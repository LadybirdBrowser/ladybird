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

struct WEB_API DynamicsCompressorGraphNode {
    // Base values for AudioParams.
    f32 threshold_db { -24.0f };
    f32 knee_db { 30.0f };
    f32 ratio { 12.0f };
    f32 attack_seconds { 0.003f };
    f32 release_seconds { 0.25f };

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::ClampedMax };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<DynamicsCompressorGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(DynamicsCompressorGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(DynamicsCompressorParamIndex::threshold, threshold_db, -100.0f, 0.0f);
        set_state(DynamicsCompressorParamIndex::knee, knee_db, 0.0f, 40.0f);
        set_state(DynamicsCompressorParamIndex::ratio, ratio, 1.0f, 20.0f);
        set_state(DynamicsCompressorParamIndex::attack, attack_seconds, 0.0f, 1.0f);
        set_state(DynamicsCompressorParamIndex::release, release_seconds, 0.0f, 1.0f);
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(DynamicsCompressorParamIndex::threshold, threshold_db);
        update_intrinsic(DynamicsCompressorParamIndex::knee, knee_db);
        update_intrinsic(DynamicsCompressorParamIndex::ratio, ratio);
        update_intrinsic(DynamicsCompressorParamIndex::attack, attack_seconds);
        update_intrinsic(DynamicsCompressorParamIndex::release, release_seconds);
    }
};

}
