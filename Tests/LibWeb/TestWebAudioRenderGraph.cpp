/*
 * Copyright (c) 2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>
#include <LibWeb/WebAudio/RenderProcessContext.h>
#include <LibWeb/WebAudio/Types.h>

using namespace Web::WebAudio;

namespace {

constexpr float sample_rate = 48000.0f;

RenderGraphDescription make_destination_only_graph(u32 channel_count = 1)
{
    RenderGraphDescription desc;
    desc.destination_node_id = NodeID { 1 };

    RenderNodeDescription destination;
    destination.type = RenderNodeType::Destination;
    destination.destination = DestinationRenderNodeDescription {
        .channel_count = channel_count,
    };
    desc.nodes.set(NodeID { 1 }, destination);

    return desc;
}

RenderGraphDescription make_constant_source_to_destination_graph(
    float offset,
    Optional<size_t> start_frame,
    Optional<size_t> stop_frame,
    bool connect = true)
{
    auto desc = make_destination_only_graph();

    RenderNodeDescription constant;
    constant.type = RenderNodeType::ConstantSource;
    constant.constant_source = ConstantSourceRenderNodeDescription {
        .start_frame = start_frame,
        .stop_frame = stop_frame,
        .offset = offset,
    };
    desc.nodes.set(NodeID { 2 }, constant);

    if (connect) {
        desc.connections.append(RenderConnection {
            .source = NodeID { 2 },
            .destination = NodeID { 1 },
            .source_output_index = 0,
            .destination_input_index = 0,
        });
    }

    return desc;
}

Vector<float> render_one_quantum(RenderGraph& graph, u64 current_frame)
{
    graph.begin_quantum(current_frame);
    AudioBus const& out = graph.render_destination_for_current_quantum();

    EXPECT(out.channel_count() >= 1);
    auto ch0 = out.channel(0);
    EXPECT_EQ(ch0.size(), static_cast<size_t>(RENDER_QUANTUM_SIZE));

    Vector<float> samples;
    samples.ensure_capacity(ch0.size());
    for (size_t i = 0; i < ch0.size(); ++i)
        samples.append(ch0[i]);

    return samples;
}

void expect_all_zero(Vector<float> const& samples)
{
    for (size_t i = 0; i < samples.size(); ++i)
        EXPECT_EQ(samples[i], 0.0f);
}

void expect_window(Vector<float> const& samples, size_t start, size_t end, float expected)
{
    size_t const n = samples.size();
    end = min(end, n);
    start = min(start, end);

    for (size_t i = 0; i < start; ++i)
        EXPECT_EQ(samples[i], 0.0f);
    for (size_t i = start; i < end; ++i)
        EXPECT_EQ(samples[i], expected);
    for (size_t i = end; i < n; ++i)
        EXPECT_EQ(samples[i], 0.0f);
}

}

TEST_CASE(render_graph_destination_only_renders_silence)
{
    auto desc = make_destination_only_graph();
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_without_start_renders_silence)
{
    auto desc = make_constant_source_to_destination_graph(1.0f, {}, {});
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_start_on_quantum_boundary_fills_quantum)
{
    constexpr float expected = 0.25f;

    auto desc = make_constant_source_to_destination_graph(expected, 0u, {});
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_window(s, 0, s.size(), expected);
}

TEST_CASE(render_graph_constant_source_start_mid_quantum_renders_partial)
{
    constexpr float expected = 1.0f;
    constexpr u64 start_frame = 10;

    auto desc = make_constant_source_to_destination_graph(expected, start_frame, {});
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_window(s, 10, s.size(), expected);
}

TEST_CASE(render_graph_constant_source_start_after_quantum_ends_renders_silence)
{
    constexpr float expected = 1.0f;

    auto desc = make_constant_source_to_destination_graph(expected, static_cast<u64>(RENDER_QUANTUM_SIZE), {});
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_stop_mid_quantum_renders_partial)
{
    constexpr float expected = 1.0f;
    constexpr u64 stop_frame = 10;

    auto desc = make_constant_source_to_destination_graph(expected, 0u, stop_frame);
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_window(s, 0, 10, expected);
}

TEST_CASE(render_graph_constant_source_start_and_stop_inside_quantum_renders_window)
{
    constexpr float expected = 1.0f;
    constexpr u64 start_frame = 20;
    constexpr u64 stop_frame = 50;

    auto desc = make_constant_source_to_destination_graph(expected, start_frame, stop_frame);
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_window(s, 20, 50, expected);
}

TEST_CASE(render_graph_constant_source_start_equals_stop_renders_silence)
{
    constexpr float expected = 1.0f;

    auto desc = make_constant_source_to_destination_graph(expected, 10u, 10u);
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_stop_before_start_renders_silence)
{
    constexpr float expected = 1.0f;

    auto desc = make_constant_source_to_destination_graph(expected, 50u, 20u);
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_unconnected_does_not_affect_destination)
{
    auto desc = make_constant_source_to_destination_graph(1.0f, 0u, {}, false);
    RenderGraph graph(desc, sample_rate);

    auto s = render_one_quantum(graph, 0);
    expect_all_zero(s);
}

TEST_CASE(render_graph_constant_source_cross_quantum_start_stop_behaves_correctly)
{
    constexpr float expected = 1.0f;
    constexpr u64 q = RENDER_QUANTUM_SIZE;

    constexpr u64 start_frame = q + 10;
    constexpr u64 stop_frame = (2 * q) + 20;

    auto desc = make_constant_source_to_destination_graph(expected, start_frame, stop_frame);
    RenderGraph graph(desc, sample_rate);

    auto s0 = render_one_quantum(graph, 0);
    expect_all_zero(s0);

    auto s1 = render_one_quantum(graph, q);
    expect_window(s1, 10, s1.size(), expected);

    auto s2 = render_one_quantum(graph, 2 * q);
    expect_window(s2, 0, 20, expected);
}
