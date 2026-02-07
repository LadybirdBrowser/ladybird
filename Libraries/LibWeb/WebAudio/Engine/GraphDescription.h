/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/GraphNodes/AnalyserGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/AudioBufferSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/AudioWorkletGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/BiquadFilterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelMergerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelSplitterGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ConstantSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DelayGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DestinationGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/DynamicsCompressorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/GainGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/GraphNodes/MediaElementAudioSourceGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/OhNoesGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/OscillatorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/ScriptProcessorGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/StereoPannerGraphNode.h>

namespace Web::WebAudio::Render {

using GraphNodeDescription = AK::Variant<
    DestinationGraphNode,             // 0
    OscillatorGraphNode,              // 1
    AudioBufferSourceGraphNode,       // 2
    MediaElementAudioSourceGraphNode, // 3
    ConstantSourceGraphNode,          // 4
    BiquadFilterGraphNode,            // 5
    DynamicsCompressorGraphNode,      // 6
    GainGraphNode,                    // 7
    DelayGraphNode,                   // 8
    StereoPannerGraphNode,            // 9
    ChannelSplitterGraphNode,         // 10
    ChannelMergerGraphNode,           // 11
    AnalyserGraphNode,                // 12
    AudioWorkletGraphNode,            // 13
    ScriptProcessorGraphNode,         // 14
    OhNoesGraphNode                   // 15
    >;

inline GraphNodeType graph_node_type(GraphNodeDescription const& node)
{
    constexpr AK::Array<GraphNodeType, 16> type_by_index = {
        GraphNodeType::Destination,
        GraphNodeType::Oscillator,
        GraphNodeType::BufferSource,
        GraphNodeType::MediaElementSource,
        GraphNodeType::ConstantSource,
        GraphNodeType::BiquadFilter,
        GraphNodeType::DynamicsCompressor,
        GraphNodeType::Gain,
        GraphNodeType::Delay,
        GraphNodeType::StereoPanner,
        GraphNodeType::ChannelSplitter,
        GraphNodeType::ChannelMerger,
        GraphNodeType::Analyser,
        GraphNodeType::AudioWorklet,
        GraphNodeType::ScriptProcessor,
        GraphNodeType::OhNoes,
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
