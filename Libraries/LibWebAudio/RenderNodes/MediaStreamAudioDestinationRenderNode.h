/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWebAudio/RenderNodes/MediaElementAudioSourceProvider.h>
#include <LibWebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class MediaStreamAudioDestinationRenderNode final : public RenderNode {
public:
    MediaStreamAudioDestinationRenderNode(NodeID node_id,
        NonnullRefPtr<MediaElementAudioSourceProvider> provider, size_t quantum_size);

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs,
        Vector<Vector<AudioBus const*>> const& param_inputs) override;
    virtual size_t output_count() const override { return 0; }
    virtual AudioBus const& output(size_t output_index) const override;

private:
    NonnullRefPtr<MediaElementAudioSourceProvider> m_provider;
    mutable AudioBus m_dummy_output;
    Vector<ReadonlySpan<f32>> m_planar_channels;
    Vector<f32> m_interleaved_samples;
};

} // namespace Web::WebAudio::Render