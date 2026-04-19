/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/RenderNodes/MediaStreamAudioDestinationRenderNode.h>

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibWebAudio/Debug.h>
#include <LibWebAudio/Engine/Mixing.h>

namespace Web::WebAudio::Render {

MediaStreamAudioDestinationRenderNode::MediaStreamAudioDestinationRenderNode(NodeID node_id,
    NonnullRefPtr<MediaElementAudioSourceProvider> provider, size_t quantum_size)
    : RenderNode(node_id)
    , m_provider(move(provider))
    , m_dummy_output(1, quantum_size)
{
}

void MediaStreamAudioDestinationRenderNode::process(RenderContext& context,
    Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];
    if (!mixed_input)
        return;

    size_t const channel_count = mixed_input->channel_count();
    size_t const frame_count = mixed_input->frame_count();
    if (channel_count == 0 || frame_count == 0)
        return;

    m_planar_channels.resize(channel_count);
    for (size_t channel_index = 0; channel_index < channel_count; ++channel_index)
        m_planar_channels[channel_index] = mixed_input->channel(channel_index);

    size_t const sample_count = channel_count * frame_count;
    m_interleaved_samples.resize(sample_count);
    copy_planar_to_interleaved(m_planar_channels.span(), m_interleaved_samples.span(), frame_count);

    u32 const sample_rate = AK::round_to<u32>(context.sample_rate);
    auto const media_time = AK::Duration::from_time_units(static_cast<i64>(context.current_frame), 1, sample_rate);
    m_provider->push_interleaved(m_interleaved_samples.span(), sample_rate,
        static_cast<u32>(channel_count), media_time);
}

AudioBus const& MediaStreamAudioDestinationRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_dummy_output;
}

} // namespace Web::WebAudio::Render