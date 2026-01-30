/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/QuickSort.h>
#include <AK/StringView.h>

#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/SharedAudioBuffer.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/IIRFilterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/WaveShaperGraphNode.h>

namespace Web::WebAudio::Render {

using Encoder = WireEncoder;
using Decoder = WireDecoder;

enum class WireSectionTag : u8 {
    NodeTable = 1,
    ConnectionTable = 2,
    ParamConnectionTable = 3,
    ParamAutomationTable = 4,
    BufferTable = 5,
};

static ErrorOr<void> begin_section(Encoder& encoder, WireSectionTag tag, size_t& out_size_field_offset, size_t& out_payload_start)
{
    TRY(encoder.append_u32(static_cast<u32>(tag)));
    out_size_field_offset = encoder.size();
    TRY(encoder.append_u32(0));
    out_payload_start = encoder.size();
    return {};
}

static void end_section(Encoder& encoder, size_t size_field_offset, size_t payload_start)
{
    auto payload_size = encoder.size() - payload_start;
    encoder.overwrite_u32_at(size_field_offset, static_cast<u32>(payload_size));
}

static ErrorOr<void> append_node_payload(Encoder& encoder, GraphNodeDescription const& node, u32& flags)
{
    return node.visit([&](auto const& payload) -> ErrorOr<void> {
        using T = AK::Detail::RemoveCVReference<decltype(payload)>;

        if constexpr (AK::Detail::IsSame<T, AudioBufferSourceGraphNode>) {
            // A BufferSource always depends on external buffer data, even if the payload is not sent.
            flags |= WireFlags::contains_external_resources;
        }
        if constexpr (AK::Detail::IsSame<T, MediaElementAudioSourceGraphNode>) {
            // The provider is process-local. Encode only the channel count and an opaque provider id.
            flags |= WireFlags::contains_external_resources;
        }
        if constexpr (AK::Detail::IsSame<T, MediaStreamAudioSourceGraphNode>) {
            // MediaStream sources depend on external providers.
            flags |= WireFlags::contains_external_resources;
        }
        if constexpr (AK::Detail::IsSame<T, ConvolverGraphNode>) {
            if (payload.buffer_id != 0)
                flags |= WireFlags::contains_external_resources;
        }

        return payload.encode_wire_payload(encoder);
    });
}

static ErrorOr<void> append_buffer_table_section(Encoder& encoder, GraphDescription const& graph, Vector<u64> const& sorted_node_ids, GraphResourceResolver const& resources, u32& flags)
{
    Vector<u64> buffer_ids;
    buffer_ids.ensure_capacity(graph.nodes.size());
    for (auto node_id_value : sorted_node_ids) {
        auto node_or_missing = graph.nodes.get(NodeID { node_id_value });
        if (!node_or_missing.has_value())
            continue;
        auto const& node = node_or_missing.value();
        if (node.has<AudioBufferSourceGraphNode>()) {
            u64 buffer_id = node.get<AudioBufferSourceGraphNode>().buffer_id;
            if (buffer_id != 0)
                buffer_ids.unchecked_append(buffer_id);
            continue;
        }

        if (node.has<ConvolverGraphNode>()) {
            u64 buffer_id = node.get<ConvolverGraphNode>().buffer_id;
            if (buffer_id != 0)
                buffer_ids.unchecked_append(buffer_id);
            continue;
        }
    }

    if (buffer_ids.is_empty())
        return {};

    quick_sort(buffer_ids);
    {
        Vector<u64> unique;
        unique.ensure_capacity(buffer_ids.size());
        bool has_last = false;
        u64 last = 0;
        for (auto value : buffer_ids) {
            if (!has_last || value != last)
                unique.unchecked_append(value);
            has_last = true;
            last = value;
        }
        buffer_ids = move(unique);
    }

    Vector<u64> present_buffer_ids;
    present_buffer_ids.ensure_capacity(buffer_ids.size());
    for (auto buffer_id : buffer_ids) {
        if (auto buffer = resources.resolve_audio_buffer(buffer_id))
            present_buffer_ids.unchecked_append(buffer_id);
    }

    if (present_buffer_ids.is_empty())
        return {};

    flags |= WireFlags::contains_external_resources;

    size_t section_size_offset = 0;
    size_t payload_start = 0;
    TRY(begin_section(encoder, WireSectionTag::BufferTable, section_size_offset, payload_start));

    TRY(encoder.append_u32(static_cast<u32>(present_buffer_ids.size())));

    for (auto buffer_id : present_buffer_ids) {
        auto buffer = resources.resolve_audio_buffer(buffer_id);
        if (!buffer)
            continue;

        TRY(encoder.append_u64(buffer_id));
        TRY(encoder.append_f32(buffer->sample_rate()));
        TRY(encoder.append_u32(static_cast<u32>(buffer->channel_count())));
        TRY(encoder.append_u64(buffer->length_in_sample_frames()));

        // Encode planar f32 samples, channel-major.
        size_t const channel_count = buffer->channel_count();
        size_t const length_in_sample_frames = buffer->length_in_sample_frames();
        for (size_t ch = 0; ch < channel_count; ++ch) {
            auto samples = buffer->channel(ch);
            size_t const frames_to_write = min(length_in_sample_frames, samples.size());
            for (size_t i = 0; i < frames_to_write; ++i)
                TRY(encoder.append_f32(samples[i]));
            for (size_t i = frames_to_write; i < length_in_sample_frames; ++i)
                TRY(encoder.append_f32(0.0f));
        }
    }

    end_section(encoder, section_size_offset, payload_start);
    return {};
}

ErrorOr<ByteBuffer> encode_render_graph_for_media_server(GraphDescription const& graph, f32 context_sample_rate, GraphResourceResolver const& resources)
{
    Encoder encoder;

    u32 flags = 0;

    // Header
    TRY(encoder.append_u32(flags)); // patched at end
    TRY(encoder.append_f32(context_sample_rate));
    TRY(encoder.append_u64(graph.destination_node_id.value()));

    size_t const flags_field_offset = 0;

    // Node table
    size_t section_size_offset = 0;
    size_t payload_start = 0;
    TRY(begin_section(encoder, WireSectionTag::NodeTable, section_size_offset, payload_start));

    Vector<u64> sorted_node_ids;
    sorted_node_ids.ensure_capacity(graph.nodes.size());
    for (auto const& it : graph.nodes)
        sorted_node_ids.unchecked_append(it.key.value());
    quick_sort(sorted_node_ids);

    TRY(encoder.append_u32(static_cast<u32>(sorted_node_ids.size())));

    for (auto node_id_value : sorted_node_ids) {
        auto node_id = NodeID { node_id_value };
        auto node_or_missing = graph.nodes.get(node_id);
        if (!node_or_missing.has_value())
            continue;

        auto const& node = node_or_missing.value();

        TRY(encoder.append_u64(node_id_value));
        TRY(encoder.append_u8(static_cast<u8>(graph_node_type(node))));

        // Per-node payload (length prefixed)
        auto payload_size_field_offset = encoder.size();
        TRY(encoder.append_u32(0));
        auto node_payload_start = encoder.size();

        TRY(append_node_payload(encoder, node, flags));

        auto node_payload_size = encoder.size() - node_payload_start;
        encoder.overwrite_u32_at(payload_size_field_offset, static_cast<u32>(node_payload_size));
    }

    end_section(encoder, section_size_offset, payload_start);

    // Optional inline buffer payloads (v1.1+ extension).
    TRY(append_buffer_table_section(encoder, graph, sorted_node_ids, resources, flags));

    // Connection table
    TRY(begin_section(encoder, WireSectionTag::ConnectionTable, section_size_offset, payload_start));
    TRY(encoder.append_u32(static_cast<u32>(graph.connections.size())));
    for (auto const& c : graph.connections) {
        TRY(encoder.append_u64(c.source.value()));
        TRY(encoder.append_u64(c.destination.value()));
        TRY(encoder.append_u32(static_cast<u32>(c.source_output_index)));
        TRY(encoder.append_u32(static_cast<u32>(c.destination_input_index)));
    }
    end_section(encoder, section_size_offset, payload_start);

    // ParamConnection table
    TRY(begin_section(encoder, WireSectionTag::ParamConnectionTable, section_size_offset, payload_start));
    TRY(encoder.append_u32(static_cast<u32>(graph.param_connections.size())));
    for (auto const& c : graph.param_connections) {
        TRY(encoder.append_u64(c.source.value()));
        TRY(encoder.append_u64(c.destination.value()));
        TRY(encoder.append_u32(static_cast<u32>(c.source_output_index)));
        TRY(encoder.append_u32(static_cast<u32>(c.destination_param_index)));
    }
    end_section(encoder, section_size_offset, payload_start);

    // ParamAutomation table
    TRY(begin_section(encoder, WireSectionTag::ParamAutomationTable, section_size_offset, payload_start));
    TRY(encoder.append_u32(static_cast<u32>(graph.param_automations.size())));
    for (auto const& automation : graph.param_automations) {
        TRY(encoder.append_u64(automation.destination.value()));
        TRY(encoder.append_u32(static_cast<u32>(automation.destination_param_index)));
        TRY(encoder.append_f32(automation.initial_value));
        TRY(encoder.append_f32(automation.default_value));
        TRY(encoder.append_f32(automation.min_value));
        TRY(encoder.append_f32(automation.max_value));
        TRY(encoder.append_u8(static_cast<u8>(automation.automation_rate)));

        TRY(encoder.append_u32(static_cast<u32>(automation.segments.size())));
        for (auto const& segment : automation.segments) {
            TRY(encoder.append_u8(static_cast<u8>(segment.type)));
            TRY(encoder.append_f64(segment.start_time));
            TRY(encoder.append_f64(segment.end_time));
            TRY(encoder.append_f64(segment.curve_start_time));
            TRY(encoder.append_f64(segment.curve_duration));
            TRY(encoder.append_u64(static_cast<u64>(segment.start_frame)));
            TRY(encoder.append_u64(static_cast<u64>(segment.end_frame)));
            TRY(encoder.append_f32(segment.start_value));
            TRY(encoder.append_f32(segment.end_value));
            TRY(encoder.append_f32(segment.time_constant));
            TRY(encoder.append_f32(segment.target));

            TRY(encoder.append_u32(static_cast<u32>(segment.curve.size())));
            for (auto v : segment.curve)
                TRY(encoder.append_f32(v));
        }
    }
    end_section(encoder, section_size_offset, payload_start);

    // Patch header flags.
    encoder.overwrite_u32_at(flags_field_offset, flags);

    return encoder.take();
}
static ErrorOr<GraphNodeDescription> decode_node_payload(GraphNodeType type, ReadonlyBytes payload)
{
    Decoder payload_decoder { payload };

    switch (type) {
    case GraphNodeType::Destination:
        return GraphNodeDescription { TRY(DestinationGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Oscillator:
        return GraphNodeDescription { TRY(OscillatorGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::AudioBufferSource:
        return GraphNodeDescription { TRY(AudioBufferSourceGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::MediaElementAudioSource:
        return GraphNodeDescription { TRY(MediaElementAudioSourceGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::MediaStreamAudioSource:
        return GraphNodeDescription { TRY(MediaStreamAudioSourceGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::ConstantSource:
        return GraphNodeDescription { TRY(ConstantSourceGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Convolver:
        return GraphNodeDescription { TRY(ConvolverGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::BiquadFilter:
        return GraphNodeDescription { TRY(BiquadFilterGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::IIRFilter:
        return GraphNodeDescription { TRY(IIRFilterGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::WaveShaper:
        return GraphNodeDescription { TRY(WaveShaperGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::DynamicsCompressor:
        return GraphNodeDescription { TRY(DynamicsCompressorGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Gain:
        return GraphNodeDescription { TRY(GainGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Delay:
        return GraphNodeDescription { TRY(DelayGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Panner:
        return GraphNodeDescription { TRY(PannerGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::StereoPanner:
        return GraphNodeDescription { TRY(StereoPannerGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::ChannelSplitter:
        return GraphNodeDescription { TRY(ChannelSplitterGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::ChannelMerger:
        return GraphNodeDescription { TRY(ChannelMergerGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Analyser:
        return GraphNodeDescription { TRY(AnalyserGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::AudioListener:
        return GraphNodeDescription { TRY(AudioListenerGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::AudioWorklet:
        return GraphNodeDescription { TRY(AudioWorkletGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::ScriptProcessor:
        return GraphNodeDescription { TRY(ScriptProcessorGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::OhNoes:
        return GraphNodeDescription { TRY(OhNoesGraphNode::decode_wire_payload(payload_decoder)) };
    case GraphNodeType::Unknown:
    default:
        return Error::from_string_literal("Unknown GraphNodeType in node table");
    }
}

ErrorOr<WireGraphBuildResult> decode_render_graph_wire_format(ReadonlyBytes bytes)
{
    Decoder decoder { bytes };

    WireGraphBuildResult result {
        .description = {},
        .resources = make<GraphResourceRegistry>(),
    };

    result.flags = TRY(decoder.read_u32());
    result.context_sample_rate_hz = TRY(decoder.read_f32());
    result.description.destination_node_id = NodeID { TRY(decoder.read_u64()) };

    // Sections: tag (u32), payload_size (u32), payload bytes.
    while (!decoder.at_end()) {
        auto tag_u32 = TRY(decoder.read_u32());
        auto payload_size = TRY(decoder.read_u32());
        auto payload = TRY(decoder.read_bytes(payload_size));

        Decoder section { payload };
        Optional<WireSectionTag> tag;
        if (tag_u32 <= 0xFF)
            tag = static_cast<WireSectionTag>(static_cast<u8>(tag_u32));
        if (!tag.has_value())
            continue;

        switch (tag.value()) {
        case WireSectionTag::NodeTable: {
            auto node_count = TRY(section.read_u32());
            result.description.nodes.ensure_capacity(node_count);

            for (u32 i = 0; i < node_count; ++i) {
                auto node_id = TRY(section.read_u64());
                auto node_type_u8 = TRY(section.read_u8());
                auto node_payload_size = TRY(section.read_u32());
                auto node_payload = TRY(section.read_bytes(node_payload_size));

                auto type = static_cast<GraphNodeType>(node_type_u8);
                GraphNodeDescription node = TRY(decode_node_payload(type, node_payload));
                result.description.nodes.set(NodeID { node_id }, move(node));
            }
            break;
        }
        case WireSectionTag::ConnectionTable: {
            auto count = TRY(section.read_u32());
            result.description.connections.ensure_capacity(count);
            for (u32 i = 0; i < count; ++i) {
                result.description.connections.append(GraphConnection {
                    .source = NodeID { TRY(section.read_u64()) },
                    .destination = NodeID { TRY(section.read_u64()) },
                    .source_output_index = TRY(section.read_u32()),
                    .destination_input_index = TRY(section.read_u32()),
                });
            }
            break;
        }
        case WireSectionTag::ParamConnectionTable: {
            auto count = TRY(section.read_u32());
            result.description.param_connections.ensure_capacity(count);
            for (u32 i = 0; i < count; ++i) {
                result.description.param_connections.append(GraphParamConnection {
                    .source = NodeID { TRY(section.read_u64()) },
                    .destination = NodeID { TRY(section.read_u64()) },
                    .source_output_index = TRY(section.read_u32()),
                    .destination_param_index = TRY(section.read_u32()),
                });
            }
            break;
        }
        case WireSectionTag::ParamAutomationTable: {
            auto automation_count = TRY(section.read_u32());
            result.description.param_automations.ensure_capacity(automation_count);
            u32 event_count = 0;

            for (u32 i = 0; i < automation_count; ++i) {
                GraphParamAutomation automation;
                automation.destination = NodeID { TRY(section.read_u64()) };
                automation.destination_param_index = TRY(section.read_u32());
                automation.initial_value = TRY(section.read_f32());
                automation.default_value = TRY(section.read_f32());
                automation.min_value = TRY(section.read_f32());
                automation.max_value = TRY(section.read_f32());
                automation.automation_rate = static_cast<AutomationRate>(TRY(section.read_u8()));

                auto segment_count = TRY(section.read_u32());
                automation.segments.ensure_capacity(segment_count);
                for (u32 s = 0; s < segment_count; ++s) {
                    GraphAutomationSegment seg;
                    seg.type = static_cast<GraphAutomationSegmentType>(TRY(section.read_u8()));
                    seg.start_time = TRY(section.read_f64());
                    seg.end_time = TRY(section.read_f64());
                    seg.curve_start_time = TRY(section.read_f64());
                    seg.curve_duration = TRY(section.read_f64());
                    seg.start_frame = clamp_u64_to_size(TRY(section.read_u64()));
                    seg.end_frame = clamp_u64_to_size(TRY(section.read_u64()));
                    seg.start_value = TRY(section.read_f32());
                    seg.end_value = TRY(section.read_f32());
                    seg.time_constant = TRY(section.read_f32());
                    seg.target = TRY(section.read_f32());

                    auto curve_count = TRY(section.read_u32());
                    if (curve_count > 0) {
                        seg.curve.resize(curve_count);
                        for (u32 c = 0; c < curve_count; ++c)
                            seg.curve[c] = TRY(section.read_f32());
                    }

                    automation.segments.append(move(seg));
                }

                event_count += static_cast<u32>(automation.segments.size());
                result.description.param_automations.append(move(automation));
            }

            result.param_automation_event_count = event_count;
            break;
        }
        case WireSectionTag::BufferTable: {
            auto buffer_count = TRY(section.read_u32());
            for (u32 i = 0; i < buffer_count; ++i) {
                u64 buffer_id = TRY(section.read_u64());
                f32 sample_rate_hz = TRY(section.read_f32());
                u32 channel_count = TRY(section.read_u32());
                u64 length_frames_u64 = TRY(section.read_u64());

                size_t length_frames = clamp_u64_to_size(length_frames_u64);
                Vector<Vector<f32>> channels;
                channels.resize(channel_count);
                for (u32 ch = 0; ch < channel_count; ++ch) {
                    auto& channel = channels[ch];
                    channel.resize(length_frames);
                    for (size_t frame = 0; frame < length_frames; ++frame)
                        channel[frame] = TRY(section.read_f32());
                }

                auto shared = SharedAudioBuffer::create(sample_rate_hz, channel_count, length_frames, move(channels));
                result.resources->set_audio_buffer(buffer_id, move(shared));
            }
            break;
        }
        default:
            break;
        }
    }

    return result;
}

}
