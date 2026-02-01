/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderGraphDescription.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#render-quantum-size
// FIXME: 128 is a terrific number, but there may be others out there.
static constexpr size_t RENDER_QUANTUM_SIZE { 128 };

struct RenderProcessContext {
    f32 sample_rate { 44100.0f };
    size_t quantum_size { RENDER_QUANTUM_SIZE };
    size_t current_frame { 0 };
};

}
