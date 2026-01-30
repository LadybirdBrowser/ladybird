/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinitionIPCExtras.h>

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::WorkletNodeDefinition const& definition)
{
    TRY(encoder.encode(static_cast<u64>(definition.node_id)));
    TRY(encoder.encode(definition.processor_name));
    TRY(encoder.encode(static_cast<u64>(definition.number_of_inputs)));
    TRY(encoder.encode(static_cast<u64>(definition.number_of_outputs)));

    Vector<u64> output_channel_count;
    if (definition.output_channel_count.has_value()) {
        output_channel_count.ensure_capacity(definition.output_channel_count->size());
        for (size_t value : *definition.output_channel_count)
            output_channel_count.unchecked_append(static_cast<u64>(value));
    }
    TRY(encoder.encode(output_channel_count));

    TRY(encoder.encode(definition.output_channel_count.has_value()));

    TRY(encoder.encode(static_cast<u64>(definition.channel_count)));
    TRY(encoder.encode(static_cast<u8>(definition.channel_count_mode)));
    TRY(encoder.encode(static_cast<u8>(definition.channel_interpretation)));

    TRY(encoder.encode(definition.parameter_names));

    TRY(encoder.encode(definition.parameter_data));
    TRY(encoder.encode(definition.serialized_processor_options));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::WorkletNodeDefinition> decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletNodeDefinition definition;
    definition.node_id = Web::WebAudio::NodeID { TRY(decoder.decode<u64>()) };
    definition.processor_name = TRY(decoder.decode<String>());
    definition.number_of_inputs = static_cast<size_t>(TRY(decoder.decode<u64>()));
    definition.number_of_outputs = static_cast<size_t>(TRY(decoder.decode<u64>()));

    Vector<u64> output_channel_count = TRY(decoder.decode<Vector<u64>>());
    bool output_channel_count_was_provided = TRY(decoder.decode<bool>());
    if (output_channel_count_was_provided) {
        Vector<size_t> counts;
        counts.ensure_capacity(output_channel_count.size());
        for (u64 value : output_channel_count)
            counts.unchecked_append(static_cast<size_t>(value));
        definition.output_channel_count = move(counts);
    }

    definition.channel_count = static_cast<size_t>(TRY(decoder.decode<u64>()));
    definition.channel_count_mode = static_cast<Web::WebAudio::Render::ChannelCountMode>(TRY(decoder.decode<u8>()));
    definition.channel_interpretation = static_cast<Web::WebAudio::Render::ChannelInterpretation>(TRY(decoder.decode<u8>()));

    definition.parameter_names = TRY(decoder.decode<Vector<String>>());

    definition.parameter_data = TRY(decoder.decode<Optional<Vector<Web::WebAudio::Render::WorkletParameterDataEntry>>>());
    definition.serialized_processor_options = TRY(decoder.decode<Optional<Web::HTML::SerializationRecord>>());
    return definition;
}

}
