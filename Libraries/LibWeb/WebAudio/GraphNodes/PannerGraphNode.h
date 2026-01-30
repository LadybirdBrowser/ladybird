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

struct WEB_API PannerGraphNode {
    PanningModelType panning_model { PanningModelType::EqualPower };
    DistanceModelType distance_model { DistanceModelType::Inverse };

    f64 ref_distance { 1.0 };
    f64 max_distance { 10000.0 };
    f64 rolloff_factor { 1.0 };
    f64 cone_inner_angle { 360.0 };
    f64 cone_outer_angle { 360.0 };
    f64 cone_outer_gain { 0.0 };

    f32 position_x { 0.0f };
    f32 position_y { 0.0f };
    f32 position_z { 0.0f };
    f32 orientation_x { 1.0f };
    f32 orientation_y { 0.0f };
    f32 orientation_z { 0.0f };

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::ClampedMax };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<PannerGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(PannerGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(PannerParamIndex::position_x, position_x, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(PannerParamIndex::position_y, position_y, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(PannerParamIndex::position_z, position_z, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(PannerParamIndex::orientation_x, orientation_x, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(PannerParamIndex::orientation_y, orientation_y, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
        set_state(PannerParamIndex::orientation_z, orientation_z, NumericLimits<f32>::lowest(), NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(PannerParamIndex::position_x, position_x);
        update_intrinsic(PannerParamIndex::position_y, position_y);
        update_intrinsic(PannerParamIndex::position_z, position_z);
        update_intrinsic(PannerParamIndex::orientation_x, orientation_x);
        update_intrinsic(PannerParamIndex::orientation_y, orientation_y);
        update_intrinsic(PannerParamIndex::orientation_z, orientation_z);
    }
};

}
