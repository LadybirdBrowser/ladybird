/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

struct RenderContext;

class ScriptProcessorHost {
public:
    virtual ~ScriptProcessorHost() = default;

    virtual bool process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds,
        size_t buffer_size, size_t input_channel_count,
        size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels,
        Span<Span<f32>> output_channels)
        = 0;
};

} // namespace Web::WebAudio::Render
