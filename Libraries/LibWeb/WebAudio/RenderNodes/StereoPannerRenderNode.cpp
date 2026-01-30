/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/StereoPannerRenderNode.h>

namespace Web::WebAudio::Render {

StereoPannerRenderNode::StereoPannerRenderNode(NodeID node_id, StereoPannerGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_pan(desc.pan)
    , m_output(2, quantum_size)
    , m_pan_input(1, quantum_size)
{
    m_output.set_channel_count(2);
}

// https://webaudio.github.io/web-audio-api/#stereopanner-algorithm
void StereoPannerRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();
    // FIXME: Optimize

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];
    if (!mixed_input) {
        m_output.zero();
        return;
    }

    bool const mono_to_stereo = mixed_input->channel_count() == 1;

    if (param_inputs.size() > StereoPannerParamIndex::pan)
        mix_inputs_into(m_pan_input, param_inputs[StereoPannerParamIndex::pan].span());
    else {
        // No audio-rate input connected: use base pan value.
        auto pan_out = m_pan_input.channel(0);
        for (size_t i = 0; i < m_pan_input.frame_count(); ++i)
            pan_out[i] = m_pan;
    }

    auto out_l = m_output.channel(0);
    auto out_r = m_output.channel(1);

    auto pan_in = m_pan_input.channel(0);

    size_t const frames = m_output.frame_count();

    auto const half_pi = AK::Pi<f64> * 0.5;

    if (mono_to_stereo) {
        auto in = mixed_input->channel(0);
        for (size_t i = 0; i < frames; ++i) {
            f64 pan = AK::clamp(static_cast<f64>(pan_in[i]), -1.0, 1.0);

            f64 const angle = (pan + 1.0) * 0.5 * half_pi;
            f64 gain_l;
            f64 gain_r;
            AK::sincos(angle, gain_r, gain_l);

            out_l[i] = in[i] * static_cast<f32>(gain_l);
            out_r[i] = in[i] * static_cast<f32>(gain_r);
        }
        return;
    }

    auto in_l = mixed_input->channel(0);
    auto in_r = mixed_input->channel(1);

    for (size_t i = 0; i < frames; ++i) {
        f64 pan = AK::clamp(static_cast<f64>(pan_in[i]), -1.0, 1.0);

        f64 x { pan };
        if (pan <= 0.0)
            x = pan + 1.0;
        f64 gain_l;
        f64 gain_r;
        AK::sincos(x * half_pi, gain_r, gain_l);

        if (pan <= 0.0) {
            out_l[i] = in_l[i] + (in_r[i] * static_cast<f32>(gain_l));
            out_r[i] = in_r[i] * static_cast<f32>(gain_r);
        } else {
            out_l[i] = in_l[i] * static_cast<f32>(gain_l);
            out_r[i] = in_r[i] + (in_l[i] * static_cast<f32>(gain_r));
        }
    }
}

void StereoPannerRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<StereoPannerGraphNode>())
        return;
    m_pan = node.get<StereoPannerGraphNode>().pan;
}

}
