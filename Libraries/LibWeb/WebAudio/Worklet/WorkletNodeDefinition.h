/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

struct WorkletParameterDataEntry {
    String name;
    double value { 0.0 };
};

struct WorkletNodeDefinition {
    NodeID node_id;
    String processor_name;
    size_t number_of_inputs { 1 };
    size_t number_of_outputs { 1 };
    Optional<Vector<size_t>> output_channel_count;
    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::Max };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };
    Vector<String> parameter_names;

    Optional<Vector<WorkletParameterDataEntry>> parameter_data;
    Optional<HTML::SerializationRecord> serialized_processor_options;
};

}
