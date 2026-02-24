/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NumericLimits.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API AudioListenerGraphNode {
    f32 position_x { 0.0f };
    f32 position_y { 0.0f };
    f32 position_z { 0.0f };
    f32 forward_x { 0.0f };
    f32 forward_y { 0.0f };
    f32 forward_z { -1.0f };
    f32 up_x { 0.0f };
    f32 up_y { 1.0f };
    f32 up_z { 0.0f };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<AudioListenerGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(AudioListenerGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(AudioListenerParamIndex::position_x, position_x, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::position_y, position_y, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::position_z, position_z, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::forward_x, forward_x, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::forward_y, forward_y, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::forward_z, forward_z, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::up_x, up_x, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::up_y, up_y, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(AudioListenerParamIndex::up_z, up_z, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(AudioListenerParamIndex::position_x, position_x);
        update_intrinsic(AudioListenerParamIndex::position_y, position_y);
        update_intrinsic(AudioListenerParamIndex::position_z, position_z);
        update_intrinsic(AudioListenerParamIndex::forward_x, forward_x);
        update_intrinsic(AudioListenerParamIndex::forward_y, forward_y);
        update_intrinsic(AudioListenerParamIndex::forward_z, forward_z);
        update_intrinsic(AudioListenerParamIndex::up_x, up_x);
        update_intrinsic(AudioListenerParamIndex::up_y, up_y);
        update_intrinsic(AudioListenerParamIndex::up_z, up_z);
    }
};

}
