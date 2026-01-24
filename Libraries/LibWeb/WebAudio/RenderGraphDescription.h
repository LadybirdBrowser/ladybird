/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

// Render-thread snapshot description of an audio graph.
// https://webaudio.github.io/web-audio-api/#rendering-thread

enum class RenderNodeType : u8 {
    Destination,
    ConstantSource,
    Unknown,
};

struct RenderConnection {
    NodeID source;
    NodeID destination;
    size_t source_output_index { 0 };
    size_t destination_input_index { 0 };
};

struct DestinationRenderNodeDescription {
    size_t channel_count { 2 };
};

struct ConstantSourceRenderNodeDescription {
    Optional<size_t> start_frame;
    Optional<size_t> stop_frame;

    f32 offset { 1.0f };
};

struct RenderNodeDescription {
    RenderNodeType type {};

    Optional<DestinationRenderNodeDescription> destination;
    Optional<ConstantSourceRenderNodeDescription> constant_source;
};

struct RenderGraphDescription {
    NodeID destination_node_id { 0 };
    HashMap<NodeID, RenderNodeDescription> nodes;
    Vector<RenderConnection> connections;
};

}
