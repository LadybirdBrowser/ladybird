/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace PlatformFeatures {

// The process that uses a feature, and the only one told whether it is enabled.
enum class FeatureProcess : u8 {
    Compositor,
    VideoDecoderServer,
};

// A feature is only enabled while the feature it depends on is, so each one is declared after its dependency.
enum class Feature : u8 {
    GpuPainting,
    YCbCrImport,
    VulkanVideo,
    VulkanVideoBundledFFmpeg,
    VulkanVideoBundledFFmpegDirectExport,
    VulkanVideoBundledFFmpegVP9,
    VulkanVideoBundledFFmpegAV1,
    VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag,
    VulkanVideoSystemFFmpeg,
    VulkanVideoSystemFFmpegDirectExport,
    VulkanVideoSystemFFmpegH264,
    VulkanVideoSystemFFmpegH265,
};

constexpr Array all_features {
    Feature::GpuPainting,
    Feature::YCbCrImport,
    Feature::VulkanVideo,
    Feature::VulkanVideoBundledFFmpeg,
    Feature::VulkanVideoBundledFFmpegDirectExport,
    Feature::VulkanVideoBundledFFmpegVP9,
    Feature::VulkanVideoBundledFFmpegAV1,
    Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag,
    Feature::VulkanVideoSystemFFmpeg,
    Feature::VulkanVideoSystemFFmpegDirectExport,
    Feature::VulkanVideoSystemFFmpegH264,
    Feature::VulkanVideoSystemFFmpegH265,
};

StringView feature_id(Feature);
Optional<Feature> feature_from_id(StringView);
FeatureProcess process_of(Feature);
Optional<Feature> dependency_of(Feature);

StringView process_name(FeatureProcess);

}

template<>
struct AK::Formatter<PlatformFeatures::Feature> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, PlatformFeatures::Feature feature)
    {
        return Formatter<StringView>::format(builder, PlatformFeatures::feature_id(feature));
    }
};
