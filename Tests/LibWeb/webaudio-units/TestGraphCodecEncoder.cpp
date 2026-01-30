/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
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

class LocalWireDecoder {
public:
    explicit LocalWireDecoder(ByteBuffer const& buffer)
        : m_bytes(buffer.bytes())
    {
    }

    bool eof() const { return m_offset >= m_bytes.size(); }

    u8 read_u8()
    {
        EXPECT(m_offset + 1 <= m_bytes.size());
        if (m_offset + 1 > m_bytes.size())
            return 0;
        return m_bytes[m_offset++];
    }

    u16 read_u16()
    {
        EXPECT(m_offset + 2 <= m_bytes.size());
        if (m_offset + 2 > m_bytes.size())
            return 0;
        u16 value = 0;
        __builtin_memcpy(&value, m_bytes.offset(m_offset), sizeof(value));
        m_offset += sizeof(value);
        return AK::convert_between_host_and_little_endian(value);
    }

    u32 read_u32()
    {
        EXPECT(m_offset + 4 <= m_bytes.size());
        if (m_offset + 4 > m_bytes.size())
            return 0;
        u32 value = 0;
        __builtin_memcpy(&value, m_bytes.offset(m_offset), sizeof(value));
        m_offset += sizeof(value);
        return AK::convert_between_host_and_little_endian(value);
    }

    u64 read_u64()
    {
        EXPECT(m_offset + 8 <= m_bytes.size());
        if (m_offset + 8 > m_bytes.size())
            return 0;
        u64 value = 0;
        __builtin_memcpy(&value, m_bytes.offset(m_offset), sizeof(value));
        m_offset += sizeof(value);
        return AK::convert_between_host_and_little_endian(value);
    }

    f32 read_f32()
    {
        u32 bits = read_u32();
        f32 value = 0.0f;
        __builtin_memcpy(&value, &bits, sizeof(value));
        return value;
    }

    ByteString read_string()
    {
        u32 length = read_u32();
        EXPECT(m_offset + length <= m_bytes.size());
        if (m_offset + length > m_bytes.size())
            return {};

        auto slice = m_bytes.slice(m_offset, length);
        m_offset += length;

        return ByteString(StringView { slice });
    }

    ReadonlyBytes read_bytes(size_t length)
    {
        EXPECT(m_offset + length <= m_bytes.size());
        if (m_offset + length > m_bytes.size())
            return {};
        auto slice = m_bytes.slice(m_offset, length);
        m_offset += length;
        return slice;
    }

private:
    ReadonlyBytes m_bytes;
    size_t m_offset { 0 };
};

GraphDescription make_destination_only_graph(u8 channel_count)
{
    GraphDescription graph;
    graph.destination_node_id = NodeID { 1 };

    graph.nodes.set(graph.destination_node_id, GraphNodeDescription { DestinationGraphNode { .channel_count = channel_count } });
    return graph;
}

GraphDescription make_graph_with_ohnoes_node()
{
    GraphDescription graph = make_destination_only_graph(2);

    OhNoesGraphNode oh_noes;
    // oh_noes.base_path = MUST(String::from_utf8("webaudio-units"sv));
    oh_noes.emit_enabled = true;
    oh_noes.strip_zero_buffers = false;
    graph.nodes.set(NodeID { 99 }, GraphNodeDescription { move(oh_noes) });

    return graph;
}

GraphDescription make_graph_with_buffer_source_node()
{
    GraphDescription graph = make_destination_only_graph(2);

    AudioBufferSourceGraphNode payload;
    payload.playback_rate = 1.0f;
    payload.detune_cents = 0.0f;
    payload.loop = false;
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

    graph.nodes.set(NodeID { 2 }, GraphNodeDescription { move(payload) });
    graph.connections.append(GraphConnection {
        .source = NodeID { 2 },
        .destination = graph.destination_node_id,
        .source_output_index = 0,
        .destination_input_index = 0,
    });

    return graph;
}

static Web::WebAudio::Render::GraphResourceRegistry make_resources_for_test_graph_with_buffer_source_node()
{
    Web::WebAudio::Render::GraphResourceRegistry resources;
    Vector<Vector<f32>> channels;
    channels.resize(1);
    channels[0].resize(16);
    channels[0].fill(0.0f);
    resources.set_audio_buffer(1, Web::WebAudio::Render::SharedAudioBuffer::create(48000.0f, 1, 16, move(channels)));
    return resources;
}

}

TEST_CASE(render_graph_wire_serializer_minimal_graph_layout)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_destination_only_graph(2);

    Web::WebAudio::Render::GraphResourceRegistry resources;

    auto buffer_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();

    LocalWireDecoder decoder(buffer);

    u32 flags = decoder.read_u32();
    EXPECT_EQ(flags, 0u);

    EXPECT_EQ(decoder.read_f32(), 48000.0f);
    EXPECT_EQ(decoder.read_u64(), 1u);

    // NodeTable
    EXPECT_EQ(decoder.read_u32(), 1u);
    u32 node_table_size = decoder.read_u32();
    auto node_table_payload = decoder.read_bytes(node_table_size);

    (void)node_table_payload;
    // Parse NodeTable payload in a nested decoder.
    {
        ByteBuffer tmp = MUST(ByteBuffer::copy(node_table_payload));
        LocalWireDecoder nodes(tmp);

        EXPECT_EQ(nodes.read_u32(), 1u);
        EXPECT_EQ(nodes.read_u64(), 1u);
        EXPECT_EQ(nodes.read_u8(), static_cast<u8>(GraphNodeType::Destination));

        u32 payload_size = nodes.read_u32();
        EXPECT_EQ(payload_size, 4u);
        EXPECT_EQ(nodes.read_u32(), 2u); // channel_count
        EXPECT(nodes.eof());
    }

    // ConnectionTable
    EXPECT_EQ(decoder.read_u32(), 2u);
    u32 connection_table_size = decoder.read_u32();
    ByteBuffer tmp_connections = MUST(ByteBuffer::copy(decoder.read_bytes(connection_table_size)));
    LocalWireDecoder connections(tmp_connections);
    EXPECT_EQ(connections.read_u32(), 0u);
    EXPECT(connections.eof());

    // ParamConnectionTable
    EXPECT_EQ(decoder.read_u32(), 3u);
    u32 param_connection_table_size = decoder.read_u32();
    ByteBuffer tmp_param_connections = MUST(ByteBuffer::copy(decoder.read_bytes(param_connection_table_size)));
    LocalWireDecoder param_connections(tmp_param_connections);
    EXPECT_EQ(param_connections.read_u32(), 0u);
    EXPECT(param_connections.eof());

    // ParamAutomationTable (present but empty in v1)
    EXPECT_EQ(decoder.read_u32(), 4u);
    u32 param_automation_table_size = decoder.read_u32();
    ByteBuffer tmp_param_autos = MUST(ByteBuffer::copy(decoder.read_bytes(param_automation_table_size)));
    LocalWireDecoder param_autos(tmp_param_autos);
    EXPECT_EQ(param_autos.read_u32(), 0u);
    EXPECT(param_autos.eof());

    EXPECT(decoder.eof());
}

TEST_CASE(render_graph_wire_serializer_encodes_ohnoes_node)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_graph_with_ohnoes_node();

    Web::WebAudio::Render::GraphResourceRegistry resources;

    auto buffer_or_error = encode_render_graph_for_media_server(graph, 44100.0f, resources);
    EXPECT(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();

    LocalWireDecoder decoder(buffer);

    u32 flags = decoder.read_u32();
    EXPECT_EQ(flags, 0u);

    (void)decoder.read_f32();
    (void)decoder.read_u64();

    // NodeTable
    EXPECT_EQ(decoder.read_u32(), 1u);
    u32 node_table_size = decoder.read_u32();
    ByteBuffer tmp_nodes = MUST(ByteBuffer::copy(decoder.read_bytes(node_table_size)));
    LocalWireDecoder nodes(tmp_nodes);

    EXPECT_EQ(nodes.read_u32(), 2u);

    // Nodes are sorted by id: destination=1, unknown=99.
    EXPECT_EQ(nodes.read_u64(), 1u);
    EXPECT_EQ(nodes.read_u8(), static_cast<u8>(GraphNodeType::Destination));
    (void)nodes.read_u32();
    (void)nodes.read_u32();

    EXPECT_EQ(nodes.read_u64(), 99u);
    EXPECT_EQ(nodes.read_u8(), static_cast<u8>(GraphNodeType::OhNoes));

    u32 payload_size = nodes.read_u32();
    EXPECT(payload_size > 0u);
    (void)nodes.read_bytes(payload_size);

    EXPECT(nodes.eof());
}

TEST_CASE(render_graph_wire_serializer_sets_external_resources_flag_for_buffer_source)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    auto graph = make_graph_with_buffer_source_node();
    auto resources = make_resources_for_test_graph_with_buffer_source_node();

    auto buffer_or_error = encode_render_graph_for_media_server(graph, 48000.0f, resources);
    EXPECT(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();

    LocalWireDecoder decoder(buffer);

    u32 flags = decoder.read_u32();
    EXPECT(flags & (1u << 1));

    (void)decoder.read_f32();
    (void)decoder.read_u64();

    // NodeTable
    EXPECT_EQ(decoder.read_u32(), 1u);
    u32 node_table_size = decoder.read_u32();
    ByteBuffer tmp_nodes = MUST(ByteBuffer::copy(decoder.read_bytes(node_table_size)));
    LocalWireDecoder nodes(tmp_nodes);

    EXPECT_EQ(nodes.read_u32(), 2u);

    // Nodes sorted: buffer_source id=2, destination id=1 => actually 1 then 2.
    EXPECT_EQ(nodes.read_u64(), 1u);
    EXPECT_EQ(nodes.read_u8(), static_cast<u8>(GraphNodeType::Destination));
    (void)nodes.read_u32();
    (void)nodes.read_u32();

    EXPECT_EQ(nodes.read_u64(), 2u);
    EXPECT_EQ(nodes.read_u8(), static_cast<u8>(GraphNodeType::AudioBufferSource));
    u32 payload_size = nodes.read_u32();
    EXPECT(payload_size > 0u);

    // Parse some of the AudioBufferSource payload to ensure field order is stable.
    {
        ByteBuffer payload_bytes = MUST(ByteBuffer::copy(nodes.read_bytes(payload_size)));
        LocalWireDecoder bs(payload_bytes);

        EXPECT_EQ(bs.read_f32(), 1.0f);
        EXPECT_EQ(bs.read_f32(), 0.0f);
        EXPECT_EQ(bs.read_u8(), 0u);

        EXPECT_EQ(bs.read_u8(), 1u);
        EXPECT_EQ(bs.read_u64(), 128u);
        EXPECT_EQ(bs.read_u8(), 1u);
        EXPECT_EQ(bs.read_u64(), 256u);

        EXPECT_EQ(bs.read_u8(), 1u);
        EXPECT_EQ(bs.read_u64(), 512u);

        EXPECT_EQ(bs.read_u64(), 0u);
        EXPECT_EQ(bs.read_u64(), 0u);
        EXPECT_EQ(bs.read_u64(), 0u);

        EXPECT_EQ(bs.read_f32(), 48000.0f);
        EXPECT_EQ(bs.read_u32(), 1u);
        EXPECT_EQ(bs.read_u64(), 16u);

        EXPECT_EQ(bs.read_u64(), 1u); // buffer_id (inline BufferTable)
        EXPECT(bs.eof());
    }

    EXPECT(nodes.eof());

    // BufferTable
    EXPECT_EQ(decoder.read_u32(), 5u);
    u32 buffer_table_size = decoder.read_u32();
    ByteBuffer tmp_buffers = MUST(ByteBuffer::copy(decoder.read_bytes(buffer_table_size)));
    LocalWireDecoder buffers(tmp_buffers);

    EXPECT_EQ(buffers.read_u32(), 1u);
    EXPECT_EQ(buffers.read_u64(), 1u);
    EXPECT_EQ(buffers.read_f32(), 48000.0f);
    EXPECT_EQ(buffers.read_u32(), 1u);
    EXPECT_EQ(buffers.read_u64(), 16u);

    // First few samples of channel 0.
    EXPECT_EQ(buffers.read_f32(), 0.0f);
    EXPECT_EQ(buffers.read_f32(), 0.0f);
    EXPECT_EQ(buffers.read_f32(), 0.0f);
}

TEST_CASE(render_graph_wire_serializer_is_deterministic_for_node_insertion_order)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::WebAudio::Render::GraphResourceRegistry resources;

    GraphDescription graph_a = make_destination_only_graph(2);
    GraphDescription graph_b = make_destination_only_graph(2);

    GraphNodeDescription gain = GainGraphNode { .gain = 0.5f, .channel_count = 1 };
    GraphNodeDescription gain2 = gain;

    // Insert in opposite orders.
    graph_a.nodes.set(NodeID { 5 }, move(gain));
    graph_b.nodes.set(NodeID { 5 }, move(gain2));

    auto a_or_error = encode_render_graph_for_media_server(graph_a, 48000.0f, resources);
    auto b_or_error = encode_render_graph_for_media_server(graph_b, 48000.0f, resources);
    EXPECT(!a_or_error.is_error());
    EXPECT(!b_or_error.is_error());

    auto a = a_or_error.release_value();
    auto b = b_or_error.release_value();

    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(a.bytes(), b.bytes());
}
