/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Rendering/NodeMessage.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

// Transfers a freshly constructed render node to the rendering thread.
struct AddNode {
    NonnullOwnPtr<Rendering::RenderNode> node;
};

// Retires the render node of an AudioNode that is no longer alive on the control thread. The rendering loop only
// considers "the set of all nodes created by this BaseAudioContext, and still alive", so a node that was garbage
// collected must leave the render graph as well.
// https://webaudio.github.io/web-audio-api/#rendering-loop
struct RemoveNode {
    NodeID node_id { 0 };
};

// Replaces the source node's outgoing connections with a full snapshot taken after a connect() or disconnect() call.
struct ReplaceConnections {
    NodeID source { 0 };
    Vector<Rendering::RenderNode::OutgoingNodeConnection> node_connections;
    Vector<Rendering::RenderNode::OutgoingParamConnection> param_connections;
};

struct SetChannelConfig {
    NodeID node_id { 0 };
    size_t channel_count { 0 };
    Bindings::ChannelCountMode channel_count_mode { Bindings::ChannelCountMode::Max };
    Bindings::ChannelInterpretation channel_interpretation { Bindings::ChannelInterpretation::Speakers };
};

// https://webaudio.github.io/web-audio-api/#control-message
using ControlMessage = Variant<AddNode, RemoveNode, ReplaceConnections, SetChannelConfig, NodeMessage>;

}
