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
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorHost.h>

namespace Web::WebAudio::Render {

// Invokes AudioWorkletProcessor.process() for one render quantum.
//
// This helper is shared by offline and realtime AudioWorklet backends.
JS::ThrowCompletionOr<bool> invoke_audio_worklet_processor_process(
    JS::Realm& worklet_realm,
    JS::Object& processor_instance,
    Vector<Vector<AudioBus const*>> const& inputs,
    Span<AudioBus*> outputs,
    Span<AudioWorkletProcessorHost::ParameterSpan const> parameters,
    size_t quantum_size);

}
