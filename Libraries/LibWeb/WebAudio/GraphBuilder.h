/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWebAudio/Engine/GraphDescription.h>
#include <LibWebAudio/Engine/GraphResources.h>

namespace Web::WebAudio {

class AnalyserNode;
class AudioWorkletNode;
class AudioNode;
class ScriptProcessorNode;

// Creates a render-thread-friendly snapshot of the current audio graph, rooted at the destination node.
//
// AudioBuffer contents are externalized into resources_out and referenced via buffer ids.
Render::GraphDescription build_graph(
    GC::Ref<AudioNode> destination_node,
    double context_sample_rate,
    HashMap<NodeID, GC::Ref<AnalyserNode>>* analyser_nodes_out,
    HashMap<NodeID, GC::Ref<AudioWorkletNode>>* audio_worklet_nodes_out = nullptr,
    HashMap<NodeID, GC::Ref<ScriptProcessorNode>>* script_processor_nodes_out = nullptr,
    Render::GraphResources* resources_out = nullptr);

}
