/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::WebAudio::Render {

class GraphExecutor;

}

namespace WebAudioWorker {

struct RenderState;

struct ResampleRenderContext {
    RenderState& scratch;
    Web::WebAudio::Render::GraphExecutor& executor;
    size_t device_channel_count { 0 };
    u32 context_sample_rate_hz { 0 };
    u32 device_sample_rate_hz { 0 };
};

void render_at_device_sample_rate(ResampleRenderContext&);
void render_with_resampler(ResampleRenderContext&);

}
