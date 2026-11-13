/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <LibPlatformFeatures/Feature.h>

namespace PlatformFeatures {

// What a build of Ladybird can do, which decides the features that exist in it at all. CMake picks the code only some
// builds can compile, and everything else asks this_build.
struct Build {
    // The Compositor paints through Vulkan, on the GPU the system probe describes.
    bool paints_through_vulkan { false };
    // Vulkan images are shared between processes as dma-bufs.
    bool shares_vulkan_images { false };
    // H.264 and HEVC can be decoded through an FFmpeg the system has, rather than the bundled one.
    bool has_system_ffmpeg { false };

    // Video decoded through Vulkan reaches the Compositor as shared images.
    constexpr bool decodes_video_through_vulkan() const { return shares_vulkan_images; }

    constexpr bool includes(Feature feature) const
    {
        switch (feature) {
        case Feature::GpuPainting:
            return true;
        case Feature::YCbCrImport:
            return shares_vulkan_images;
        case Feature::VulkanVideo:
        case Feature::VulkanVideoBundledFFmpeg:
        case Feature::VulkanVideoBundledFFmpegDirectExport:
        case Feature::VulkanVideoBundledFFmpegVP9:
        case Feature::VulkanVideoBundledFFmpegAV1:
        case Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag:
            return decodes_video_through_vulkan();
        case Feature::VulkanVideoSystemFFmpeg:
        case Feature::VulkanVideoSystemFFmpegDirectExport:
        case Feature::VulkanVideoSystemFFmpegH264:
        case Feature::VulkanVideoSystemFFmpegH265:
            return decodes_video_through_vulkan() && has_system_ffmpeg;
        }
        VERIFY_NOT_REACHED();
    }

    // A build with every feature, so that tests can decide them all on any platform.
    static constexpr Build with_every_feature()
    {
        return { .paints_through_vulkan = true, .shares_vulkan_images = true, .has_system_ffmpeg = true };
    }
};

namespace Detail {

#ifdef USE_VULKAN
constexpr bool paints_through_vulkan = true;
#else
constexpr bool paints_through_vulkan = false;
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
constexpr bool shares_vulkan_images = true;
#else
constexpr bool shares_vulkan_images = false;
#endif

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
constexpr bool has_system_ffmpeg = true;
#else
constexpr bool has_system_ffmpeg = false;
#endif

}

constexpr Build this_build {
    .paints_through_vulkan = Detail::paints_through_vulkan,
    .shares_vulkan_images = Detail::shares_vulkan_images,
    .has_system_ffmpeg = Detail::has_system_ffmpeg,
};

}
