/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/Engine/SharedAudioBuffer.h>

using namespace Web::WebAudio;
using namespace Web::WebAudio::Render;

namespace {

GraphDescription make_destination_only_graph(u8 channel_count)
{
    GraphDescription graph;
    graph.destination_node_id = NodeID { 1 };

    graph.nodes.set(graph.destination_node_id, GraphNodeDescription { DestinationGraphNode { .channel_count = channel_count } });
    return graph;
}

GraphDescription make_graph_with_audio_worklet()
{
    GraphDescription graph = make_destination_only_graph(2);

    AudioWorkletGraphNode payload;
    payload.processor_name = MUST(String::from_utf8("processor"sv));
    payload.number_of_inputs = 1;
    payload.number_of_outputs = 2;
    payload.output_channel_count = { 1, 2 };

    payload.parameter_names = {
        MUST(String::from_utf8("a"sv)),
        MUST(String::from_utf8("b"sv)),
    };

    graph.nodes.set(NodeID { 2 }, GraphNodeDescription { move(payload) });
    graph.connections.append(GraphConnection {
        .source = NodeID { 2 },
        .destination = graph.destination_node_id,
        .source_output_index = 0,
        .destination_input_index = 0,
    });

    return graph;
}

GraphDescription make_graph_with_buffer_source_node()
{
    GraphDescription graph = make_destination_only_graph(2);

    AudioBufferSourceGraphNode payload;
    payload.playback_rate = 1.0f;
    payload.detune_cents = 0.0f;
    payload.loop = true;
    payload.start_frame = Optional<size_t> { 128 };
    payload.stop_frame = Optional<size_t> { 256 };
    payload.duration_in_sample_frames = Optional<size_t> { 512 };
    payload.offset_frame = 0;
    payload.loop_start_frame = 0;
    payload.loop_end_frame = 0;

    payload.sample_rate = 48000.0f;
    payload.channel_count = 1;
    payload.length_in_sample_frames = 16;

    payload.buffer_id = 1;

    graph.nodes.set(NodeID { 2 }, GraphNodeDescription { payload });
    graph.connections.append(GraphConnection {
        .source = NodeID { 2 },
        .destination = graph.destination_node_id,
        .source_output_index = 0,
        .destination_input_index = 0,
    });

    return graph;
}

GraphDescription make_graph_with_inline_buffer_source_node()
{
    GraphDescription graph = make_destination_only_graph(2);

    AudioBufferSourceGraphNode payload;
    payload.playback_rate = 1.0f;
    payload.detune_cents = 0.0f;
    payload.loop = false;

    payload.sample_rate = 48000.0f;
    payload.channel_count = 1;
    payload.length_in_sample_frames = 4;

    payload.buffer_id = 2;

    graph.nodes.set(NodeID { 2 }, GraphNodeDescription { payload });
    graph.connections.append(GraphConnection {
        .source = NodeID { 2 },
        .destination = graph.destination_node_id,
        .source_output_index = 0,
        .destination_input_index = 0,
    });

    return graph;
}

Web::WebAudio::Render::GraphResourceRegistry make_resources_for_buffer_source_test_graph()
{
    Web::WebAudio::Render::GraphResourceRegistry resources;
    Vector<Vector<f32>> channels;
    channels.resize(1);
    channels[0].resize(16);
    channels[0].fill(0.0f);
    resources.set_audio_buffer(1, Web::WebAudio::Render::SharedAudioBuffer::create(48000.0f, 1, 16, move(channels)));
    return resources;
}

Web::WebAudio::Render::GraphResourceRegistry make_resources_for_inline_buffer_table_test_graph()
{
    Web::WebAudio::Render::GraphResourceRegistry resources;
    Vector<Vector<f32>> channels;
    channels.resize(1);
    channels[0] = { 0.25f, -0.5f, 0.75f, -1.0f };
    resources.set_audio_buffer(2, Web::WebAudio::Render::SharedAudioBuffer::create(48000.0f, 1, 4, move(channels)));
    return resources;
}

}

TEST_CASE(render_graph_wire_round_trip_destination_only)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_destination_only_graph(2);

    Web::WebAudio::Render::GraphResourceRegistry resources;

    auto bytes_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    auto decoded_or_error = Web::WebAudio::Render::decode_render_graph_wire_format(bytes_or_error.value().bytes());
    EXPECT(!decoded_or_error.is_error());

    auto build = decoded_or_error.release_value();
    EXPECT_EQ(build.flags, 0u);
    EXPECT_EQ(build.context_sample_rate_hz, 48000.0f);
    EXPECT_EQ(build.description.destination_node_id.value(), 1u);

    EXPECT_EQ(build.description.nodes.size(), 1u);
    EXPECT_EQ(build.description.connections.size(), 0u);
    EXPECT_EQ(build.description.param_connections.size(), 0u);
    EXPECT_EQ(build.param_automation_event_count, 0u);

    auto it = build.description.nodes.find(NodeID { 1 });
    EXPECT(it != build.description.nodes.end());
    auto const& node = it->value;
    EXPECT(node.has<DestinationGraphNode>());
    EXPECT_EQ(node.get<DestinationGraphNode>().channel_count, 2u);
}

TEST_CASE(render_graph_wire_round_trip_audio_worklet_parameters)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_graph_with_audio_worklet();

    Web::WebAudio::Render::GraphResourceRegistry resources;

    auto bytes_or_error = encode_render_graph_for_media_server(graph, 44100.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    auto decoded_or_error = decode_render_graph_wire_format(bytes_or_error.value().bytes());
    EXPECT(!decoded_or_error.is_error());

    auto build = decoded_or_error.release_value();
    EXPECT_EQ(build.description.destination_node_id.value(), 1u);
    EXPECT_EQ(build.description.nodes.size(), 2u);
    EXPECT_EQ(build.description.connections.size(), 1u);

    auto it = build.description.nodes.find(NodeID { 2 });
    EXPECT(it != build.description.nodes.end());
    auto const& node = it->value;
    EXPECT(node.has<AudioWorkletGraphNode>());

    auto const& aw = node.get<AudioWorkletGraphNode>();
    EXPECT_EQ(aw.processor_name, "processor");
    EXPECT_EQ(aw.number_of_inputs, 1u);
    EXPECT_EQ(aw.number_of_outputs, 2u);
    EXPECT(aw.output_channel_count.has_value());
    auto const& output_channel_count = aw.output_channel_count.value();
    EXPECT_EQ(output_channel_count.size(), 2u);
    EXPECT_EQ(output_channel_count[0], 1u);
    EXPECT_EQ(output_channel_count[1], 2u);

    EXPECT_EQ(aw.parameter_names.size(), 2u);
    EXPECT_EQ(aw.parameter_names[0], "a");
    EXPECT_EQ(aw.parameter_names[1], "b");
}

TEST_CASE(render_graph_wire_round_trip_sets_external_resources_flag)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_graph_with_buffer_source_node();
    auto resources = make_resources_for_buffer_source_test_graph();

    auto bytes_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    auto decoded_or_error = decode_render_graph_wire_format(bytes_or_error.value().bytes());
    EXPECT(!decoded_or_error.is_error());

    auto build = decoded_or_error.release_value();

    EXPECT((build.flags & WireFlags::contains_external_resources) != 0);

    auto it = build.description.nodes.find(NodeID { 2 });
    EXPECT(it != build.description.nodes.end());
    auto const& node = it->value;
    EXPECT(node.has<AudioBufferSourceGraphNode>());
    auto const& buffer_source = node.get<AudioBufferSourceGraphNode>();
    EXPECT_EQ(buffer_source.sample_rate, 48000.0f);
    EXPECT_EQ(buffer_source.channel_count, 1u);
    EXPECT_EQ(buffer_source.length_in_sample_frames, 16u);
    EXPECT_EQ(buffer_source.buffer_id, 1u);

    auto buffer = build.resources->resolve_audio_buffer(1);
    EXPECT(buffer != nullptr);
}

TEST_CASE(render_graph_wire_round_trip_inline_buffer_table)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_graph_with_inline_buffer_source_node();
    auto resources = make_resources_for_inline_buffer_table_test_graph();

    auto bytes_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    auto decoded_or_error = decode_render_graph_wire_format(bytes_or_error.value().bytes());
    EXPECT(!decoded_or_error.is_error());

    auto build = decoded_or_error.release_value();

    EXPECT((build.flags & WireFlags::contains_external_resources) != 0);

    auto it = build.description.nodes.find(NodeID { 2 });
    EXPECT(it != build.description.nodes.end());
    auto const& node = it->value;
    EXPECT(node.has<AudioBufferSourceGraphNode>());
    EXPECT(node.get<AudioBufferSourceGraphNode>().buffer_id != 0u);

    auto buffer = build.resources->resolve_audio_buffer(node.get<AudioBufferSourceGraphNode>().buffer_id);
    EXPECT(buffer != nullptr);
    EXPECT_EQ(buffer->sample_rate(), 48000.0f);
    EXPECT_EQ(buffer->channel_count(), 1u);
    EXPECT_EQ(buffer->length_in_sample_frames(), 4u);
    auto channel0 = buffer->channel(0);
    EXPECT_EQ(channel0.size(), 4u);
    EXPECT_EQ(channel0[0], 0.25f);
    EXPECT_EQ(channel0[1], -0.5f);
    EXPECT_EQ(channel0[2], 0.75f);
    EXPECT_EQ(channel0[3], -1.0f);
}

TEST_CASE(render_graph_wire_decode_fails_on_unknown_node_type)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::WebAudio::Render::GraphResourceRegistry resources;

    auto graph = make_destination_only_graph(2);

    auto bytes_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    ByteBuffer mutated = MUST(ByteBuffer::copy(bytes_or_error.value().bytes()));

    auto read_u32_le = [&](size_t offset) -> u32 {
        u32 value = 0;
        __builtin_memcpy(&value, mutated.bytes().offset(offset), sizeof(value));
        return AK::convert_between_host_and_little_endian(value);
    };

    // Header: u32 flags + f32 sample_rate + u64 destination_node_id.
    constexpr size_t header_size = 16;
    size_t offset = header_size;

    // First section must be NodeTable.
    u32 tag = read_u32_le(offset);
    EXPECT_EQ(tag, 1u);
    offset += 4;
    u32 node_table_size = read_u32_le(offset);
    offset += 4;
    (void)node_table_size;

    // NodeTable payload: u32 node_count, then per-node u64 id, u8 type, u32 payload_size, payload bytes.
    u32 node_count = read_u32_le(offset);
    EXPECT_EQ(node_count, 1u);
    offset += 4;
    offset += 8; // node id

    // Overwrite node type byte with an unknown value.
    mutated.data()[offset] = 0xFF;

    auto decoded_or_error = decode_render_graph_wire_format(mutated.bytes());
    EXPECT(decoded_or_error.is_error());
}

TEST_CASE(render_graph_wire_round_trip_skips_unknown_sections)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_destination_only_graph(2);
    Web::WebAudio::Render::GraphResourceRegistry resources;
    auto bytes_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!bytes_or_error.is_error());

    auto const& original = bytes_or_error.value();
    auto original_bytes = original.bytes();

    // Header: u32 flags + f32 sample_rate + u64 destination_node_id.
    constexpr size_t header_size = 16;
    EXPECT(original_bytes.size() >= header_size);

    ByteBuffer mutated;

    auto append_u32 = [&](u32 value) {
        u32 le = AK::convert_between_host_and_little_endian(value);
        MUST(mutated.try_append(&le, sizeof(le)));
    };

    MUST(mutated.try_append(original_bytes.slice(0, header_size)));

    // Inject an unknown section before the known sections.
    append_u32(999); // tag
    append_u32(4);   // size
    MUST(mutated.try_append("ABCD"sv.bytes()));

    MUST(mutated.try_append(original_bytes.slice(header_size)));

    auto decoded_or_error = decode_render_graph_wire_format(mutated.bytes());
    EXPECT(!decoded_or_error.is_error());

    auto build = decoded_or_error.release_value();

    EXPECT_EQ(build.description.destination_node_id.value(), 1u);
    EXPECT_EQ(build.description.nodes.size(), 1u);
    auto it = build.description.nodes.find(NodeID { 1 });
    EXPECT(it != build.description.nodes.end());
    EXPECT_EQ(graph_node_type(it->value), GraphNodeType::Destination);
}
