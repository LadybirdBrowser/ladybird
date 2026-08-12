/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletPipe.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

// Script runs on the main-thread pump, `prime_level` quanta behind this render node; this side
// only exchanges preallocated pipe slots and renders silence on underrun.
class AudioWorkletRenderNode final : public RenderNode {
public:
    AudioWorkletRenderNode(NodeID, size_t input_count, size_t output_count, size_t quantum_size,
        Vector<u32> output_channel_capacities,
        size_t channel_count, Bindings::ChannelCountMode, Bindings::ChannelInterpretation,
        NonnullRefPtr<AudioWorkletPipe>,
        Vector<NonnullRefPtr<RenderAudioParam>> params);

    virtual ~AudioWorkletRenderNode() override;

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void for_each_param(Function<void(RenderAudioParam&)> const&) override;

private:
    NonnullRefPtr<AudioWorkletPipe> m_pipe;
    Vector<NonnullRefPtr<RenderAudioParam>> m_params;
    Vector<float> m_param_scratch;
    Vector<u32> m_output_channel_capacities;
};

}
