/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/GraphNodes/PannerGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class PannerRenderNode final : public RenderNode {
public:
    PannerRenderNode(NodeID, size_t quantum_size, PannerGraphNode const&);
    virtual ~PannerRenderNode() override = default;

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const&) override;
    virtual void apply_description(GraphNodeDescription const&) override;
    virtual AudioBus const& output(size_t index) const override;

private:
    // Snapshot state
    PanningModelType m_panning_model { PanningModelType::EqualPower };
    DistanceModelType m_distance_model { DistanceModelType::Inverse };

    f64 m_ref_distance { 1.0 };
    f64 m_max_distance { 10000.0 };
    f64 m_rolloff_factor { 1.0 };
    f64 m_cone_inner_angle { 360.0 };
    f64 m_cone_outer_angle { 360.0 };
    f64 m_cone_outer_gain { 0.0 };

    f32 m_position_x { 0.0f };
    f32 m_position_y { 0.0f };
    f32 m_position_z { 0.0f };
    f32 m_orientation_x { 1.0f };
    f32 m_orientation_y { 0.0f };
    f32 m_orientation_z { 0.0f };

    size_t m_channel_count { 2 };
    ChannelCountMode m_channel_count_mode { ChannelCountMode::ClampedMax };
    ChannelInterpretation m_channel_interpretation { ChannelInterpretation::Speakers };

    // Processing buffers
    AudioBus m_output_bus;
};

}
