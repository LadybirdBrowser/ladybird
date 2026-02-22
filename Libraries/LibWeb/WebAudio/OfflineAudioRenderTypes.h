/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>

namespace Web::WebAudio {

struct OfflineAudioRenderRequest {
    RenderGraphDescription graph;
    u32 number_of_channels { 2 };
    u32 length_in_sample_frames { 0 };
    f32 sample_rate { 44100.0f };
};

struct OfflineAudioRenderResult {
    Vector<Vector<f32>> rendered_channels;
};

}
