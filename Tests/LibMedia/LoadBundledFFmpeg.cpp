/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Try.h>
#include <LibMediaCodecs/FFmpeg/BundledFFmpeg.h>

// The tests decode, encode and convert through the bundled FFmpeg, which every process loads before using it.
[[maybe_unused]] static auto const bundled_ffmpeg_is_loaded = [] {
    MUST(Media::FFmpeg::load_bundled_ffmpeg());
    return true;
}();
