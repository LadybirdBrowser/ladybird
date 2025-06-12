/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>

namespace Gfx {

struct VP8XHeader {
    bool has_icc { false };
    bool has_alpha { false };
    bool has_exif { false };
    bool has_xmp { false };
    bool has_animation { false };
    u32 width { 0 };
    u32 height { 0 };
};

struct ANIMChunk {
    u32 background_color { 0 };
    u16 loop_count { 0 };
};

struct ANMFChunkHeader {
    u32 frame_x { 0 };
    u32 frame_y { 0 };
    u32 frame_width { 0 };
    u32 frame_height { 0 };
    u32 frame_duration_in_milliseconds { 0 };

    enum class BlendingMethod {
        UseAlphaBlending = 0,
        DoNotBlend = 1,
    };
    BlendingMethod blending_method { BlendingMethod::UseAlphaBlending };

    enum class DisposalMethod {
        DoNotDispose = 0,
        DisposeToBackgroundColor = 1,
    };
    DisposalMethod disposal_method { DisposalMethod::DoNotDispose };
};

struct ANMFChunk {
    ANMFChunkHeader header;
    ReadonlyBytes frame_data;
};

}
