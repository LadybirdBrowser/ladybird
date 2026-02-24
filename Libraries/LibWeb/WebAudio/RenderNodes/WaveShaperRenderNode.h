/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/SincResampler.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

struct WaveShaperGraphNode;

class WaveShaperRenderNode final : public RenderNode {
public:
    WaveShaperRenderNode(NodeID node_id, WaveShaperGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;
    void apply_description_offline(GraphNodeDescription const& node) override;

private:
    static constexpr size_t max_channel_count { 32 };

    size_t oversample_factor() const;
    f32 shape_sample(f32 input) const;
    void ensure_oversample_storage(size_t channel_count, size_t oversampled_frames, size_t factor);

    OverSampleType m_oversample { OverSampleType::None };
    Vector<f32> m_curve;

    AudioBus m_output;
    AudioBus m_oversampled;

    SampleRateConverter m_upsampler;
    SampleRateConverter m_downsampler;
    bool m_resampler_initialized { false };
    size_t m_resampler_channel_count { 0 };
    size_t m_resampler_factor { 1 };
};

}
