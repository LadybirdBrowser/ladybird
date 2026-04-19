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
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/QuickSort.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebAudio/GraphNodes/AnalyserGraphNode.h>
#include <LibWebAudio/GraphNodes/AudioBufferSourceGraphNode.h>
#include <LibWebAudio/GraphNodes/AudioListenerGraphNode.h>
#include <LibWebAudio/GraphNodes/AudioWorkletGraphNode.h>
#include <LibWebAudio/GraphNodes/BiquadFilterGraphNode.h>
#include <LibWebAudio/GraphNodes/ChannelMergerGraphNode.h>
#include <LibWebAudio/GraphNodes/ChannelSplitterGraphNode.h>
#include <LibWebAudio/GraphNodes/ConstantSourceGraphNode.h>
#include <LibWebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWebAudio/GraphNodes/DelayGraphNode.h>
#include <LibWebAudio/GraphNodes/DestinationGraphNode.h>
#include <LibWebAudio/GraphNodes/DynamicsCompressorGraphNode.h>
#include <LibWebAudio/GraphNodes/GainGraphNode.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWebAudio/GraphNodes/IIRFilterGraphNode.h>
#include <LibWebAudio/GraphNodes/MediaElementAudioSourceGraphNode.h>
#include <LibWebAudio/GraphNodes/MediaStreamAudioDestinationGraphNode.h>
#include <LibWebAudio/GraphNodes/MediaStreamAudioSourceGraphNode.h>
#include <LibWebAudio/GraphNodes/OhNoesGraphNode.h>
#include <LibWebAudio/GraphNodes/OscillatorGraphNode.h>
#include <LibWebAudio/GraphNodes/PannerGraphNode.h>
#include <LibWebAudio/GraphNodes/ScriptProcessorGraphNode.h>
#include <LibWebAudio/GraphNodes/StereoPannerGraphNode.h>
#include <LibWebAudio/GraphNodes/WaveShaperGraphNode.h>

namespace Web::WebAudio::Render {

class AudioBus;
class RenderNode;
class WireDecoder;
class WireEncoder;

struct UnknownGraphNode {
    static ErrorOr<void> encode_wire_payload(WireEncoder&) { VERIFY_NOT_REACHED(); }
    static ErrorOr<UnknownGraphNode> decode_wire_payload(WireDecoder&) { VERIFY_NOT_REACHED(); }
    static GraphUpdateKind classify_update(UnknownGraphNode const&) { VERIFY_NOT_REACHED(); }
    static OwnPtr<RenderNode> make_render_node(NodeID, size_t, GraphResources const&) { VERIFY_NOT_REACHED(); }
};

#define __ENUMERATE_GRAPH_NODE_DESCRIPTION_TYPE(name, _debug_name) name##GraphNode,
using GraphNodeDescription = AK::Variant<ENUMERATE_GRAPH_NODE_TYPES(__ENUMERATE_GRAPH_NODE_DESCRIPTION_TYPE) UnknownGraphNode>;
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

struct RenderNodeSet {
    HashMap<NodeID, size_t> node_index_by_id;
    Vector<NodeID> node_ids;
    Vector<GraphNodeType> node_types_by_index;
    Vector<OwnPtr<RenderNode>> nodes;
    Vector<size_t> analyser_node_indices;
};

struct IndexedConnection {
    size_t source_node_index { 0 };
    size_t source_output { 0 };
};

struct GraphTopology {
    enum class ProcessingNodeKind : u8 {
        Real,
        DelayWriter,
        DelayReader,
    };

    struct ProcessingNode {
        ProcessingNodeKind kind { ProcessingNodeKind::Real };
        size_t real_node_index { 0 };
        size_t param_owner_node_index { 0 };
        GraphNodeType node_type { GraphNodeType::Unknown };
        RenderNode* render_node { nullptr };
    };

    struct ChannelMixingSettings {
        size_t channel_count { 1 };
        ChannelCountMode channel_count_mode { ChannelCountMode::Max };
        ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };
    };

    size_t destination_node_index { 0 };

    Vector<ProcessingNode> nodes;
    Vector<GraphConnection> connections;
    Vector<GraphParamConnection> param_connections;
    Vector<Vector<Vector<IndexedConnection>>> inputs_by_input;
    Vector<Vector<Vector<IndexedConnection>>> param_inputs_by_param;
    Vector<Vector<Vector<AudioBus const*>>> input_buses_scratch;
    Vector<ChannelMixingSettings> channel_mixing_by_node;
    Vector<Vector<NonnullOwnPtr<AudioBus>>> input_mix_buses;
    Vector<Vector<Vector<AudioBus const*>>> param_input_buses_scratch;
    Vector<Vector<size_t>> dependents;
    Vector<size_t> processing_order;
};

} // namespace Web::WebAudio::Render
