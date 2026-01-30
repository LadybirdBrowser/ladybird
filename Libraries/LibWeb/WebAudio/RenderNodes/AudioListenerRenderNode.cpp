/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/RenderNodes/AudioListenerRenderNode.h>

namespace Web::WebAudio::Render {

AudioListenerRenderNode::AudioListenerRenderNode(NodeID node_id, size_t quantum_size, AudioListenerGraphNode const& desc)
    : RenderNode(node_id)
    , m_dummy_output(0, quantum_size)
    , m_intrinsic_position_x(desc.position_x)
    , m_intrinsic_position_y(desc.position_y)
    , m_intrinsic_position_z(desc.position_z)
    , m_intrinsic_forward_x(desc.forward_x)
    , m_intrinsic_forward_y(desc.forward_y)
    , m_intrinsic_forward_z(desc.forward_z)
    , m_intrinsic_up_x(desc.up_x)
    , m_intrinsic_up_y(desc.up_y)
    , m_intrinsic_up_z(desc.up_z)
{
    m_position_x.resize(quantum_size);
    m_position_y.resize(quantum_size);
    m_position_z.resize(quantum_size);
    m_forward_x.resize(quantum_size);
    m_forward_y.resize(quantum_size);
    m_forward_z.resize(quantum_size);
    m_up_x.resize(quantum_size);
    m_up_y.resize(quantum_size);
    m_up_z.resize(quantum_size);
}

void AudioListenerRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();

    auto copy_param = [&](size_t index, Vector<f32>& dest, f32 intrinsic) {
        if (param_inputs.size() > index && !param_inputs[index].is_empty()) {
            AudioBus const* bus = param_inputs[index][0];
            auto data = bus->channel(0);
            size_t count = min(dest.size(), bus->frame_count());
            for (size_t i = 0; i < count; ++i)
                dest[i] = data[i];
        } else {
            for (auto& v : dest)
                v = intrinsic;
        }
    };

    copy_param(AudioListenerParamIndex::position_x, m_position_x, m_intrinsic_position_x);
    copy_param(AudioListenerParamIndex::position_y, m_position_y, m_intrinsic_position_y);
    copy_param(AudioListenerParamIndex::position_z, m_position_z, m_intrinsic_position_z);
    copy_param(AudioListenerParamIndex::forward_x, m_forward_x, m_intrinsic_forward_x);
    copy_param(AudioListenerParamIndex::forward_y, m_forward_y, m_intrinsic_forward_y);
    copy_param(AudioListenerParamIndex::forward_z, m_forward_z, m_intrinsic_forward_z);
    copy_param(AudioListenerParamIndex::up_x, m_up_x, m_intrinsic_up_x);
    copy_param(AudioListenerParamIndex::up_y, m_up_y, m_intrinsic_up_y);
    copy_param(AudioListenerParamIndex::up_z, m_up_z, m_intrinsic_up_z);
}

void AudioListenerRenderNode::apply_description(GraphNodeDescription const& desc)
{
    ASSERT_RENDER_THREAD();

    if (!desc.has<AudioListenerGraphNode>())
        return;

    AudioListenerGraphNode const& listener = desc.get<AudioListenerGraphNode>();

    m_intrinsic_position_x = listener.position_x;
    m_intrinsic_position_y = listener.position_y;
    m_intrinsic_position_z = listener.position_z;
    m_intrinsic_forward_x = listener.forward_x;
    m_intrinsic_forward_y = listener.forward_y;
    m_intrinsic_forward_z = listener.forward_z;
    m_intrinsic_up_x = listener.up_x;
    m_intrinsic_up_y = listener.up_y;
    m_intrinsic_up_z = listener.up_z;
}

size_t AudioListenerRenderNode::output_count() const
{
    ASSERT_WEBAUDIO_THREAD();
    return 0;
}

AudioBus const& AudioListenerRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_dummy_output;
}

Span<f32 const> AudioListenerRenderNode::position_x() const
{
    ASSERT_RENDER_THREAD();
    return m_position_x;
}

Span<f32 const> AudioListenerRenderNode::position_y() const
{
    ASSERT_RENDER_THREAD();
    return m_position_y;
}

Span<f32 const> AudioListenerRenderNode::position_z() const
{
    ASSERT_RENDER_THREAD();
    return m_position_z;
}

Span<f32 const> AudioListenerRenderNode::forward_x() const
{
    ASSERT_RENDER_THREAD();
    return m_forward_x;
}

Span<f32 const> AudioListenerRenderNode::forward_y() const
{
    ASSERT_RENDER_THREAD();
    return m_forward_y;
}

Span<f32 const> AudioListenerRenderNode::forward_z() const
{
    ASSERT_RENDER_THREAD();
    return m_forward_z;
}

Span<f32 const> AudioListenerRenderNode::up_x() const
{
    ASSERT_RENDER_THREAD();
    return m_up_x;
}

Span<f32 const> AudioListenerRenderNode::up_y() const
{
    ASSERT_RENDER_THREAD();
    return m_up_y;
}

Span<f32 const> AudioListenerRenderNode::up_z() const
{
    ASSERT_RENDER_THREAD();
    return m_up_z;
}

}
