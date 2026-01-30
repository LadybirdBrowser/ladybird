/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class AudioWorkletProcessorHost;
class ScriptProcessorHost;
class AudioListenerRenderNode;

struct RenderContext {
    f32 sample_rate { 44100.0f };
    size_t quantum_size { RENDER_QUANTUM_SIZE };
    size_t current_frame { 0 };

    AudioWorkletProcessorHost* worklet_processor_host { nullptr };
    ScriptProcessorHost* script_processor_host { nullptr };
    AudioListenerRenderNode* listener { nullptr };
};

}
