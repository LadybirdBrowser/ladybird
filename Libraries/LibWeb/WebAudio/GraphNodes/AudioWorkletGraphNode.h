/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API AudioWorkletGraphNode {
    String processor_name;
    size_t number_of_inputs { 1 };
    size_t number_of_outputs { 1 };
    Optional<Vector<size_t>> output_channel_count;

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::Max };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    // Stable, ordered list of AudioWorkletNode AudioParam names.
    // The index into this vector is used as destination_param_index in RenderParamAutomation.
    Vector<String> parameter_names;

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<AudioWorkletGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(AudioWorkletGraphNode const& new_desc) const;
};

}
