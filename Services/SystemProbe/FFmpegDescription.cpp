/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#include <LibPlatformFeatures/Build.h>
#include <SystemProbe/FFmpegDescription.h>

static PlatformFeatures::FFmpegFacts describe_ffmpeg(Media::FFmpeg::FFmpegFunctions const& functions, bool shares_bundled_struct_layouts)
{
    return {
        .avcodec_version = functions.avcodec_version(),
        .avutil_version = functions.avutil_version(),
        .shares_bundled_struct_layouts = shares_bundled_struct_layouts,
    };
}

PlatformFeatures::FFmpegFacts describe_bundled_ffmpeg()
{
    return describe_ffmpeg(Media::FFmpeg::FFmpegFunctions::bundled(), true);
}

Optional<PlatformFeatures::FFmpegFacts> describe_system_ffmpeg()
{
    if constexpr (PlatformFeatures::this_build.has_system_ffmpeg) {
        Media::FFmpeg::SystemFFmpeg::load();
        if (auto const* system_ffmpeg = Media::FFmpeg::SystemFFmpeg::the())
            return describe_ffmpeg(system_ffmpeg->functions(), system_ffmpeg->shares_bundled_struct_layouts());
    }
    return {};
}
