/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class AudioBus;
struct RenderContext;

// Provides access to AudioWorklet processor instances and runs their process() hook.
// This is intentionally injected via RenderContext so offline and realtime backends
// can share the same RenderGraph node implementation.
class AudioWorkletProcessorHost {
public:
    virtual ~AudioWorkletProcessorHost() = default;

    struct ParameterSpan {
        StringView name;
        ReadonlySpan<f32> values;
    };

    // Returns whether the processor should continue to be called for subsequent quanta.
    // When this returns false, the node becomes silent for future quanta.
    virtual bool process_audio_worklet(
        NodeID node_id,
        RenderContext&,
        String const& processor_name,
        size_t number_of_inputs,
        size_t number_of_outputs,
        Span<size_t const> output_channel_count,
        Vector<Vector<AudioBus const*>> const& inputs,
        Span<AudioBus*> outputs,
        Span<ParameterSpan const> parameters)
        = 0;
};

}
