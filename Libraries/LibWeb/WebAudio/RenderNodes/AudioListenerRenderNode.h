/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class AudioListenerRenderNode final : public RenderNode {
public:
    AudioListenerRenderNode(NodeID, size_t quantum_size, AudioListenerGraphNode const&);
    virtual ~AudioListenerRenderNode() override = default;

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    virtual void apply_description(GraphNodeDescription const&) override;
    virtual AudioBus const& output(size_t) const override;
    virtual size_t output_count() const override;

    Span<f32 const> position_x() const;
    Span<f32 const> position_y() const;
    Span<f32 const> position_z() const;
    Span<f32 const> forward_x() const;
    Span<f32 const> forward_y() const;
    Span<f32 const> forward_z() const;
    Span<f32 const> up_x() const;
    Span<f32 const> up_y() const;
    Span<f32 const> up_z() const;

private:
    AudioBus m_dummy_output;

    f32 m_intrinsic_position_x { 0.0f };
    f32 m_intrinsic_position_y { 0.0f };
    f32 m_intrinsic_position_z { 0.0f };
    f32 m_intrinsic_forward_x { 0.0f };
    f32 m_intrinsic_forward_y { 0.0f };
    f32 m_intrinsic_forward_z { -1.0f };
    f32 m_intrinsic_up_x { 0.0f };
    f32 m_intrinsic_up_y { 1.0f };
    f32 m_intrinsic_up_z { 0.0f };

    Vector<f32> m_position_x;
    Vector<f32> m_position_y;
    Vector<f32> m_position_z;
    Vector<f32> m_forward_x;
    Vector<f32> m_forward_y;
    Vector<f32> m_forward_z;
    Vector<f32> m_up_x;
    Vector<f32> m_up_y;
    Vector<f32> m_up_z;
};

}
