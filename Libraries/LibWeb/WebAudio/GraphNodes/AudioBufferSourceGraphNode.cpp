/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/GraphNodes/AudioBufferSourceGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/AudioBufferSourceRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> AudioBufferSourceGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(playback_rate));
    TRY(encoder.append_f32(detune_cents));
    TRY(encoder.append_u8(loop ? 1 : 0));

    TRY(append_optional_size_as_u64(encoder, start_frame));
    TRY(append_optional_size_as_u64(encoder, stop_frame));
    TRY(append_optional_f64(encoder, start_time_in_context_frames));
    TRY(append_optional_size_as_u64(encoder, duration_in_sample_frames));

    TRY(encoder.append_u64(static_cast<u64>(offset_frame)));
    TRY(encoder.append_u64(static_cast<u64>(loop_start_frame)));
    TRY(encoder.append_u64(static_cast<u64>(loop_end_frame)));

    TRY(encoder.append_f32(sample_rate));
    TRY(encoder.append_u32(clamp_size_to_u32(channel_count)));
    TRY(encoder.append_u64(static_cast<u64>(length_in_sample_frames)));

    return encoder.append_u64(buffer_id);
}

ErrorOr<AudioBufferSourceGraphNode> AudioBufferSourceGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    AudioBufferSourceGraphNode node;
    node.playback_rate = TRY(decoder.read_f32());
    node.detune_cents = TRY(decoder.read_f32());
    node.loop = TRY(decoder.read_u8()) != 0;

    node.start_frame = TRY(read_optional_size_from_u64(decoder));
    node.stop_frame = TRY(read_optional_size_from_u64(decoder));
    node.start_time_in_context_frames = TRY(read_optional_f64(decoder));
    node.duration_in_sample_frames = TRY(read_optional_size_from_u64(decoder));

    node.offset_frame = clamp_u64_to_size(TRY(decoder.read_u64()));
    node.loop_start_frame = clamp_u64_to_size(TRY(decoder.read_u64()));
    node.loop_end_frame = clamp_u64_to_size(TRY(decoder.read_u64()));

    node.sample_rate = TRY(decoder.read_f32());
    node.channel_count = TRY(decoder.read_u32());
    node.length_in_sample_frames = clamp_u64_to_size(TRY(decoder.read_u64()));
    node.buffer_id = TRY(decoder.read_u64());
    return node;
}

OwnPtr<RenderNode> AudioBufferSourceGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const& resources) const
{
    ASSERT_CONTROL_THREAD();
    RefPtr<SharedAudioBuffer> buffer;
    if (buffer_id != 0)
        buffer = resources.resolve_audio_buffer(buffer_id);

    return make<AudioBufferSourceRenderNode>(node_id, *this, move(buffer), quantum_size);
}

GraphUpdateKind AudioBufferSourceGraphNode::classify_update(AudioBufferSourceGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (sample_rate != new_desc.sample_rate)
        return GraphUpdateKind::RebuildRequired;
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::RebuildRequired;
    if (length_in_sample_frames != new_desc.length_in_sample_frames)
        return GraphUpdateKind::RebuildRequired;
    if (buffer_id != new_desc.buffer_id)
        return GraphUpdateKind::RebuildRequired;

    if (start_frame != new_desc.start_frame)
        return GraphUpdateKind::Parameter;
    if (stop_frame != new_desc.stop_frame)
        return GraphUpdateKind::Parameter;
    if (start_time_in_context_frames != new_desc.start_time_in_context_frames)
        return GraphUpdateKind::Parameter;

    if (playback_rate != new_desc.playback_rate)
        return GraphUpdateKind::Parameter;
    if (detune_cents != new_desc.detune_cents)
        return GraphUpdateKind::Parameter;

    if (duration_in_sample_frames != new_desc.duration_in_sample_frames)
        return GraphUpdateKind::Parameter;

    if (offset_frame != new_desc.offset_frame)
        return GraphUpdateKind::Parameter;
    if (loop != new_desc.loop)
        return GraphUpdateKind::Parameter;
    if (loop_start_frame != new_desc.loop_start_frame)
        return GraphUpdateKind::Parameter;
    if (loop_end_frame != new_desc.loop_end_frame)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
