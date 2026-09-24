/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegFunctions.h>

namespace Media::FFmpeg {

FFmpegFunctions const& FFmpegFunctions::bundled()
{
    static constexpr FFmpegFunctions functions {
#define FFMPEG_LINKED_FUNCTION(name) .name = &::name,
        FFMPEG_ENUMERATE_FUNCTIONS(FFMPEG_LINKED_FUNCTION)
#undef FFMPEG_LINKED_FUNCTION
    };
    return functions;
}

}
