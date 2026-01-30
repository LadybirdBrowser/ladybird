/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/GraphNodes/AnalyserGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/AudioBufferSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/AudioListenerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/AudioWorkletGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/BiquadFilterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelMergerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelSplitterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ConstantSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DelayGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DestinationGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DynamicsCompressorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/GainGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/GraphNodes/IIRFilterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/MediaElementAudioSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/MediaStreamAudioSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/OhNoesGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/OscillatorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/PannerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ScriptProcessorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/StereoPannerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/WaveShaperGraphNode.h>

namespace Web::WebAudio::Render {

class WireDecoder;
class WireEncoder;

struct UnknownGraphNode {
    static ErrorOr<void> encode_wire_payload(WireEncoder&) { VERIFY_NOT_REACHED(); }
    static ErrorOr<UnknownGraphNode> decode_wire_payload(WireDecoder&) { VERIFY_NOT_REACHED(); }
    static GraphUpdateKind classify_update(UnknownGraphNode const&) { VERIFY_NOT_REACHED(); }
    static OwnPtr<RenderNode> make_render_node(NodeID, size_t, GraphResourceResolver const&) { VERIFY_NOT_REACHED(); }
};

#define __ENUMERATE_GRAPH_NODE_DESCRIPTION_TYPE(name, _debug_name) name##GraphNode,
using GraphNodeDescription = AK::Variant<
    ENUMERATE_GRAPH_NODE_TYPES(__ENUMERATE_GRAPH_NODE_DESCRIPTION_TYPE)
        UnknownGraphNode>;
#undef __ENUMERATE_GRAPH_NODE_DESCRIPTION_TYPE

inline GraphNodeType graph_node_type(GraphNodeDescription const& node)
{
    constexpr auto type_by_index = AK::Array {
#define __ENUMERATE_GRAPH_NODE_TYPE_BY_INDEX(name, _debug_name) GraphNodeType::name,
        ENUMERATE_GRAPH_NODE_TYPES(__ENUMERATE_GRAPH_NODE_TYPE_BY_INDEX)
#undef __ENUMERATE_GRAPH_NODE_TYPE_BY_INDEX
            GraphNodeType::Unknown,
    };

    size_t const index = node.index();
    if (index >= type_by_index.size())
        return GraphNodeType::Unknown;
    return type_by_index[index];
}

struct GraphDescription {
    NodeID destination_node_id { 0 };
    HashMap<NodeID, GraphNodeDescription> nodes;
    Vector<GraphConnection> connections;
    Vector<GraphParamConnection> param_connections;
    Vector<GraphParamAutomation> param_automations;

    void normalize()
    {
        AK::quick_sort(connections, [](auto const& a, auto const& b) {
            if (a.destination != b.destination)
                return a.destination < b.destination;
            if (a.destination_input_index != b.destination_input_index)
                return a.destination_input_index < b.destination_input_index;
            if (a.source != b.source)
                return a.source < b.source;
            return a.source_output_index < b.source_output_index;
        });

        AK::quick_sort(param_connections, [](auto const& a, auto const& b) {
            if (a.destination != b.destination)
                return a.destination < b.destination;
            if (a.destination_param_index != b.destination_param_index)
                return a.destination_param_index < b.destination_param_index;
            if (a.source != b.source)
                return a.source < b.source;
            return a.source_output_index < b.source_output_index;
        });

        // This doesn't change semantics, but makes "no change" comparisons stable.
        AK::quick_sort(param_automations, [](auto const& a, auto const& b) {
            if (a.destination != b.destination)
                return a.destination < b.destination;
            return a.destination_param_index < b.destination_param_index;
        });
    }
};

}
