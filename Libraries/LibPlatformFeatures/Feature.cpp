/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPlatformFeatures/Feature.h>

namespace PlatformFeatures {

StringView feature_id(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
        return "gpu_painting"sv;
    case Feature::YCbCrImport:
        return "ycbcr_import"sv;
    case Feature::VulkanVideo:
        return "vulkan_video"sv;
    case Feature::VulkanVideoBundledFFmpeg:
        return "vulkan_video.bundled_ffmpeg"sv;
    case Feature::VulkanVideoBundledFFmpegDirectExport:
        return "vulkan_video.bundled_ffmpeg.direct_export"sv;
    case Feature::VulkanVideoBundledFFmpegVP9:
        return "vulkan_video.bundled_ffmpeg.vp9"sv;
    case Feature::VulkanVideoBundledFFmpegAV1:
        return "vulkan_video.bundled_ffmpeg.av1"sv;
    case Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag:
        return "vulkan_video.bundled_ffmpeg.av1.keep_sequence_header_enable_ref_mvs_flag"sv;
    case Feature::VulkanVideoSystemFFmpeg:
        return "vulkan_video.system_ffmpeg"sv;
    case Feature::VulkanVideoSystemFFmpegDirectExport:
        return "vulkan_video.system_ffmpeg.direct_export"sv;
    case Feature::VulkanVideoSystemFFmpegH264:
        return "vulkan_video.system_ffmpeg.h264"sv;
    case Feature::VulkanVideoSystemFFmpegH265:
        return "vulkan_video.system_ffmpeg.h265"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Feature> feature_from_id(StringView id)
{
    for (auto feature : all_features) {
        if (feature_id(feature) == id)
            return feature;
    }
    return {};
}

FeatureProcess process_of(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
    case Feature::YCbCrImport:
        return FeatureProcess::Compositor;
    case Feature::VulkanVideo:
    case Feature::VulkanVideoBundledFFmpeg:
    case Feature::VulkanVideoBundledFFmpegDirectExport:
    case Feature::VulkanVideoBundledFFmpegVP9:
    case Feature::VulkanVideoBundledFFmpegAV1:
    case Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag:
    case Feature::VulkanVideoSystemFFmpeg:
    case Feature::VulkanVideoSystemFFmpegDirectExport:
    case Feature::VulkanVideoSystemFFmpegH264:
    case Feature::VulkanVideoSystemFFmpegH265:
        return FeatureProcess::VideoDecoderServer;
    }
    VERIFY_NOT_REACHED();
}

Optional<Feature> dependency_of(Feature feature)
{
    switch (feature) {
    case Feature::GpuPainting:
        return {};
    case Feature::YCbCrImport:
        return Feature::GpuPainting;
    case Feature::VulkanVideo:
        return Feature::YCbCrImport;
    case Feature::VulkanVideoBundledFFmpeg:
    case Feature::VulkanVideoSystemFFmpeg:
        return Feature::VulkanVideo;
    case Feature::VulkanVideoBundledFFmpegDirectExport:
    case Feature::VulkanVideoBundledFFmpegVP9:
    case Feature::VulkanVideoBundledFFmpegAV1:
        return Feature::VulkanVideoBundledFFmpeg;
    case Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag:
        return Feature::VulkanVideoBundledFFmpegAV1;
    case Feature::VulkanVideoSystemFFmpegDirectExport:
    case Feature::VulkanVideoSystemFFmpegH264:
    case Feature::VulkanVideoSystemFFmpegH265:
        return Feature::VulkanVideoSystemFFmpeg;
    }
    VERIFY_NOT_REACHED();
}

StringView process_name(FeatureProcess process)
{
    switch (process) {
    case FeatureProcess::Compositor:
        return "Compositor"sv;
    case FeatureProcess::VideoDecoderServer:
        return "VideoDecoderServer"sv;
    }
    VERIFY_NOT_REACHED();
}

}
