/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/FFmpeg/BundledFFmpeg.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVulkanVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#include <LibPlatformFeatures/Build.h>
#include <SystemProbe/FFmpegDescription.h>

static PlatformFeatures::FFmpegFacts describe_ffmpeg(Media::FFmpeg::FFmpegFunctions const& functions, bool has_vulkan_decoder_for_its_version)
{
    return {
        .avcodec_version = functions.avcodec_version(),
        .avutil_version = functions.avutil_version(),
        .has_vulkan_decoder_for_its_version = has_vulkan_decoder_for_its_version,
    };
}

Optional<PlatformFeatures::FFmpegFacts> describe_bundled_ffmpeg()
{
    if (auto result = Media::FFmpeg::load_bundled_ffmpeg(); result.is_error()) {
        warnln("Could not load the bundled FFmpeg: {}", result.error());
        return {};
    }
    return describe_ffmpeg(Media::FFmpeg::FFmpegFunctions::bundled(), true);
}

Optional<PlatformFeatures::FFmpegFacts> describe_system_ffmpeg()
{
    if constexpr (PlatformFeatures::this_build.has_system_ffmpeg) {
        Media::FFmpeg::SystemFFmpeg::load();
        if (auto const* system_ffmpeg = Media::FFmpeg::SystemFFmpeg::the()) {
            auto has_vulkan_decoder_for_its_version = false;
#ifdef USE_VULKAN_DMABUF_IMAGES
            has_vulkan_decoder_for_its_version = Media::FFmpeg::FFmpegVulkanVideoDecoder::has_vulkan_decoder_for_system_ffmpeg();
#endif
            return describe_ffmpeg(system_ffmpeg->functions(), has_vulkan_decoder_for_its_version);
        }
    }
    return {};
}
