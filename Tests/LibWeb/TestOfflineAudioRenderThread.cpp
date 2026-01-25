/*
 * Copyright (c) 2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/OfflineAudioRenderThread.h>
#include <LibWeb/WebAudio/OfflineAudioRenderTypes.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>
#include <LibWeb/WebAudio/RenderProcessContext.h>
#include <LibWeb/WebAudio/Types.h>

using namespace Web::WebAudio;

namespace {

constexpr float sample_rate = 48000.0f;

void wait_until_finished(OfflineAudioRenderThread& thread, int timeout_ms = 1000)
{
    for (int elapsed_ms = 0; elapsed_ms < timeout_ms; ++elapsed_ms) {
        if (thread.is_finished())
            return;

        (void)Core::System::sleep_ms(1);
    }

    FAIL("Timed out waiting for OfflineAudioRenderThread to finish");
}

RenderGraphDescription make_constant_source_to_destination_graph(float offset, u32 channel_count = 1)
{
    RenderGraphDescription desc;
    desc.destination_node_id = NodeID { 1 };

    RenderNodeDescription destination;
    destination.type = RenderNodeType::Destination;
    destination.destination = DestinationRenderNodeDescription {
        .channel_count = channel_count,
    };
    desc.nodes.set(NodeID { 1 }, destination);

    RenderNodeDescription constant;
    constant.type = RenderNodeType::ConstantSource;
    constant.constant_source = ConstantSourceRenderNodeDescription {
        .start_frame = 0u,
        .stop_frame = {},
        .offset = offset,
    };
    desc.nodes.set(NodeID { 2 }, constant);

    desc.connections.append(RenderConnection {
        .source = NodeID { 2 },
        .destination = NodeID { 1 },
        .source_output_index = 0,
        .destination_input_index = 0,
    });

    return desc;
}

}

TEST_CASE(offline_audio_render_thread_publishes_result_for_constant_source)
{
    constexpr float expected = 0.5f;
    constexpr u32 channel_count = 1;
    constexpr u32 length_in_frames = (static_cast<u32>(RENDER_QUANTUM_SIZE) * 2) + 13;

    OfflineAudioRenderRequest request;
    request.graph = make_constant_source_to_destination_graph(expected, channel_count);
    request.number_of_channels = channel_count;
    request.length_in_sample_frames = length_in_frames;
    request.sample_rate = sample_rate;

    OfflineAudioRenderThread thread(move(request), -1);
    thread.start();

    wait_until_finished(thread);

    auto result = thread.take_result();
    EXPECT(result.has_value());

    EXPECT_EQ(result->rendered_channels.size(), channel_count);
    EXPECT_EQ(result->rendered_channels[0].size(), length_in_frames);

    for (u32 i = 0; i < length_in_frames; ++i)
        EXPECT_EQ(result->rendered_channels[0][i], expected);

    // Ensure one-shot semantics: second take returns empty.
    auto result2 = thread.take_result();
    EXPECT(!result2.has_value());
}
