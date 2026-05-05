/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

class AudioBus;
struct RenderContext;

class AudioWorkletProcessorHost {
public:
    virtual ~AudioWorkletProcessorHost() = default;

    struct ParameterSpan {
        StringView name;
        ReadonlySpan<f32> values;
    };

    virtual bool process_audio_worklet(NodeID node_id, RenderContext&, String const& processor_name,
        size_t number_of_inputs, size_t number_of_outputs,
        Span<size_t const> output_channel_count,
        Vector<Vector<AudioBus const*>> const& inputs, Span<AudioBus*> outputs,
        Span<ParameterSpan const> parameters)
        = 0;
};

} // namespace Web::WebAudio::Render
