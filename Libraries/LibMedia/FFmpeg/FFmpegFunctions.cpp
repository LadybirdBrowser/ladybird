/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegFunctions.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>

namespace Media::FFmpeg {

FFmpegFunctions const& FFmpegFunctions::bundled()
{
    static constexpr FFmpegFunctions functions {
#define FFMPEG_LINKED_FUNCTION(name) .name = &::name,
        FFMPEG_ENUMERATE_FUNCTIONS(FFMPEG_LINKED_FUNCTION)
#undef FFMPEG_LINKED_FUNCTION
        .codec_ids = [] {
            CodecIDTable codec_ids {};
            for (auto codec_id : all_codec_ids)
                codec_ids[to_underlying(codec_id)] = ffmpeg_codec_id_from_media_codec_id(codec_id);
            return codec_ids;
        }(),
    };
    return functions;
}

}
