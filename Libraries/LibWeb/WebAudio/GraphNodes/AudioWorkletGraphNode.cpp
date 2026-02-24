/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/AudioWorkletGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/AudioWorkletRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> AudioWorkletGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_string(processor_name.bytes_as_string_view()));
    TRY(encoder.append_u32(clamp_size_to_u32(number_of_inputs)));
    TRY(encoder.append_u32(clamp_size_to_u32(number_of_outputs)));

    size_t output_channel_count_size = 0;
    if (output_channel_count.has_value())
        output_channel_count_size = output_channel_count->size();
    TRY(encoder.append_u32(clamp_size_to_u32(output_channel_count_size)));
    if (output_channel_count.has_value()) {
        for (auto count : *output_channel_count)
            TRY(encoder.append_u32(clamp_size_to_u32(count)));
    }

    TRY(encoder.append_u8(output_channel_count.has_value() ? 1 : 0));

    TRY(encoder.append_u32(clamp_size_to_u32(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));

    TRY(encoder.append_u32(clamp_size_to_u32(parameter_names.size())));
    for (auto const& name : parameter_names)
        TRY(encoder.append_string(name.bytes_as_string_view()));

    return {};
}

ErrorOr<AudioWorkletGraphNode> AudioWorkletGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    AudioWorkletGraphNode node;

    auto processor_name = TRY(decoder.read_string());
    node.processor_name = String::from_utf8_with_replacement_character(processor_name.view());

    node.number_of_inputs = TRY(decoder.read_u32());
    node.number_of_outputs = TRY(decoder.read_u32());

    auto output_count = TRY(decoder.read_u32());
    Vector<size_t> output_channel_count;
    output_channel_count.resize(output_count);
    for (u32 i = 0; i < output_count; ++i)
        output_channel_count[i] = TRY(decoder.read_u32());

    bool output_channel_count_was_provided = TRY(decoder.read_u8()) != 0;
    if (output_channel_count_was_provided)
        node.output_channel_count = move(output_channel_count);

    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));

    auto parameter_count = TRY(decoder.read_u32());
    node.parameter_names.ensure_capacity(parameter_count);
    for (u32 i = 0; i < parameter_count; ++i) {
        auto name = TRY(decoder.read_string());
        node.parameter_names.append(String::from_utf8_with_replacement_character(name.view()));
    }

    return node;
}

OwnPtr<RenderNode> AudioWorkletGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<AudioWorkletRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind AudioWorkletGraphNode::classify_update(AudioWorkletGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (processor_name == new_desc.processor_name
        && number_of_inputs == new_desc.number_of_inputs
        && number_of_outputs == new_desc.number_of_outputs
        && output_channel_count == new_desc.output_channel_count
        && channel_count == new_desc.channel_count
        && channel_count_mode == new_desc.channel_count_mode
        && channel_interpretation == new_desc.channel_interpretation
        && parameter_names == new_desc.parameter_names)
        return GraphUpdateKind::None;

    return GraphUpdateKind::RebuildRequired;
}

}
