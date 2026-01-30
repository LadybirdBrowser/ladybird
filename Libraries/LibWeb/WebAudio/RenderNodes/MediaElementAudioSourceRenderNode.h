/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceProvider.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class MediaElementAudioSourceRenderNode final : public RenderNode {
public:
    MediaElementAudioSourceRenderNode(NodeID node_id, NonnullRefPtr<MediaElementAudioSourceProvider> provider, size_t quantum_size);

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    virtual AudioBus const& output(size_t output_index) const override;

private:
    // https://webaudio.github.io/web-audio-api/#audionode-channelcount
    // NOTE: In the realtime graph we keep AudioBus storage preallocated to avoid allocations on the
    // render thread. Media element sources can be multi-channel, so we provision a reasonable cap.
    static constexpr size_t max_channel_capacity = 8;

    NonnullRefPtr<MediaElementAudioSourceProvider> m_provider;
    AudioBus m_output;
    u64 m_last_timeline_generation { 0 };
    Optional<AK::Duration> m_media_to_context_offset;

    SampleRateConverter m_resampler;
    Vector<Vector<f32>> m_resample_input_channels;
    size_t m_resample_input_start_frame { 0 };
    size_t m_resample_input_pending_frames { 0 };
    f64 m_resample_ratio_smoothed { 1.0 };
    u32 m_resample_last_provider_sample_rate { 0 };
    size_t m_resample_last_channel_count { 0 };
    bool m_resampler_initialized { false };
};

}
