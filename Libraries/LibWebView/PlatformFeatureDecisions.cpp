/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PlatformFeatureDecisions.h>

namespace WebView {

using PlatformFeatures::FFmpegFacts;
using PlatformFeatures::VideoDecodeFacts;

// Until 63.20.101, FFmpeg gives a separate DPB the output's DRM format modifiers, which RADV lays out wrongly at some
// resolutions (#12902). FFmpeg decodes into a separate DPB on drivers that cannot decode into the output.
static bool system_ffmpeg_gives_a_separate_dpb_the_output_modifiers(SystemFacts const& facts)
{
    if (!facts.gpu.has_value() || !facts.system_ffmpeg.has_value())
        return false;
    if (facts.system_ffmpeg->avcodec_version >= FFmpegFacts::version(63, 20, 101))
        return false;
    auto uses_a_separate_dpb_without_modifiers = [](VideoDecodeFacts const& codec) {
        return codec.supported && !codec.dpb_and_output_coincide && !codec.dpb_supports_drm_format_modifiers;
    };
    return uses_a_separate_dpb_without_modifiers(facts.gpu->h264) || uses_a_separate_dpb_without_modifiers(facts.gpu->h265);
}

static constexpr u32 nvidia_proprietary_driver_id = 4; // VK_DRIVER_ID_NVIDIA_PROPRIETARY

// NVIDIA's driver decodes AV1 frames with 8 or more tiles and no reference frame motion vectors as copies of an earlier
// frame while the sequence header allows them (#14510, https://bugzilla.mozilla.org/show_bug.cgi?id=2084563). Clearing
// the flag for those frames avoids it, and does not change what they decode to.
static bool uses_nvidia_proprietary_driver(SystemFacts const& facts)
{
    return facts.gpu.has_value() && facts.gpu->driver_id == nvidia_proprietary_driver_id;
}

// The flag can only be cleared frame by frame where the sequence header is passed with each frame.
static bool cannot_work_around_nvidia_av1_tiles(SystemFacts const& facts)
{
    return uses_nvidia_proprietary_driver(facts) && !facts.gpu->supports_video_maintenance2;
}

static constexpr Array s_blocklist_rules {
    BlocklistRule {
        "system-ffmpeg-separate-dpb-modifiers"sv,
        Feature::VulkanVideoSystemFFmpegDirectExport,
        "The system FFmpeg predates 63.20.101 and gives a separate DPB the output's DRM format modifiers, which corrupts video on RADV (#12902)"sv,
        system_ffmpeg_gives_a_separate_dpb_the_output_modifiers,
    },
    BlocklistRule {
        "nvidia-av1-tiles"sv,
        Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag,
        "NVIDIA driver bug repeats AV1 frames with many tiles and no reference frame motion vectors (#14510)"sv,
        uses_nvidia_proprietary_driver,
    },
    BlocklistRule {
        "nvidia-av1-tiles-without-video-maintenance2"sv,
        Feature::VulkanVideoBundledFFmpegAV1,
        "NVIDIA driver bug in AV1 decoding cannot be worked around without VK_KHR_video_maintenance2 (#14510)"sv,
        cannot_work_around_nvidia_av1_tiles,
    },
};

ReadonlySpan<BlocklistRule> blocklist_rules()
{
    return s_blocklist_rules;
}

static Optional<String> missing_codec(SystemFacts const& facts, VideoDecodeFacts PlatformFeatures::GpuFacts::* codec, StringView name)
{
    if (!facts.gpu.has_value() || !((*facts.gpu).*codec).supported)
        return MUST(String::formatted("The GPU does not decode {} through Vulkan", name));
    return {};
}

static Optional<String> missing_capability(Feature feature, SystemFacts const& facts, PlatformFeatures::Build const& build)
{
    switch (feature) {
    case Feature::GpuPainting:
        // The probe only describes the GPU through Vulkan, so otherwise the Compositor finds out for itself whether it
        // can paint on it.
        if (!build.paints_through_vulkan)
            return {};
        if (facts.probe_failure.has_value())
            return MUST(String::formatted("The system probe failed: {}", *facts.probe_failure));
        if (!facts.gpu.has_value())
            return "No Vulkan device was found"_string;
        return {};
    case Feature::YCbCrImport:
        if (!facts.gpu.has_value() || !facts.gpu->supports_ycbcr_sampling)
            return "The GPU does not support YCbCr sampling"_string;
        return {};
    case Feature::VulkanVideo:
        if (!facts.gpu.has_value())
            return "No Vulkan device was found"_string;
        if (facts.gpu->cannot_decode_video_because.has_value())
            return *facts.gpu->cannot_decode_video_because;
        return {};
    case Feature::VulkanVideoBundledFFmpeg:
        if (!facts.bundled_ffmpeg.has_value())
            return "The bundled FFmpeg was not described"_string;
        if (!facts.gpu.has_value() || !(facts.gpu->vp9.supported || facts.gpu->av1.supported))
            return "The GPU decodes neither VP9 nor AV1 through Vulkan"_string;
        return {};
    case Feature::VulkanVideoSystemFFmpeg:
        if (!facts.system_ffmpeg.has_value())
            return "No system FFmpeg was found"_string;
        if (!facts.system_ffmpeg->shares_bundled_struct_layouts)
            return "The system FFmpeg's structs are not laid out like the bundled one's"_string;
        if (!facts.gpu.has_value() || !(facts.gpu->h264.supported || facts.gpu->h265.supported))
            return "The GPU decodes neither H.264 nor HEVC through Vulkan"_string;
        return {};
    case Feature::VulkanVideoBundledFFmpegVP9:
        return missing_codec(facts, &PlatformFeatures::GpuFacts::vp9, "VP9"sv);
    case Feature::VulkanVideoBundledFFmpegAV1:
        return missing_codec(facts, &PlatformFeatures::GpuFacts::av1, "AV1"sv);
    case Feature::VulkanVideoSystemFFmpegH264:
        return missing_codec(facts, &PlatformFeatures::GpuFacts::h264, "H.264"sv);
    case Feature::VulkanVideoSystemFFmpegH265:
        return missing_codec(facts, &PlatformFeatures::GpuFacts::h265, "HEVC"sv);
    case Feature::VulkanVideoBundledFFmpegDirectExport:
    case Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag:
    case Feature::VulkanVideoSystemFFmpegDirectExport:
        return {};
    }
    VERIFY_NOT_REACHED();
}

StringView feature_state_name(FeatureStatus::State state)
{
    switch (state) {
    case FeatureStatus::State::Enabled:
        return "Enabled"sv;
    case FeatureStatus::State::Unsupported:
        return "Unsupported"sv;
    case FeatureStatus::State::Blocklisted:
        return "Blocklisted"sv;
    case FeatureStatus::State::DisabledByUser:
        return "Disabled by user"sv;
    case FeatureStatus::State::BlockedByDependency:
        return "Blocked by dependency"sv;
    case FeatureStatus::State::FailedAtRuntime:
        return "Failed at runtime"sv;
    case FeatureStatus::State::NotBuilt:
        return "Not built"sv;
    }
    VERIFY_NOT_REACHED();
}

PlatformFeatureDecisions PlatformFeatureDecisions::evaluate(SystemFacts const& facts, FeatureOverrides const& overrides, HashMap<Feature, String> const& runtime_failures, ReadonlySpan<BlocklistRule> rules, PlatformFeatures::Build const& build)
{
    PlatformFeatureDecisions decisions;

    // Every dependency is declared before the features that depend on it, so it has been decided by the time they are.
    for (auto feature : PlatformFeatures::all_features) {
        auto& status = decisions.m_statuses[to_underlying(feature)];

        if (!build.includes(feature)) {
            status.state = FeatureStatus::State::NotBuilt;
            continue;
        }

        if (auto dependency = PlatformFeatures::dependency_of(feature); dependency.has_value()) {
            VERIFY(to_underlying(*dependency) < to_underlying(feature));
            auto const& dependency_status = decisions.status_of(*dependency);
            if (dependency_status.state != FeatureStatus::State::Enabled) {
                status.state = FeatureStatus::State::BlockedByDependency;
                status.blocking_feature = dependency_status.blocking_feature.value_or(*dependency);
                continue;
            }
        }

        if (auto capability = missing_capability(feature, facts, build); capability.has_value()) {
            status.state = FeatureStatus::State::Unsupported;
            status.reason = capability.release_value();
            continue;
        }

        if (auto failure = runtime_failures.get(feature); failure.has_value()) {
            status.state = FeatureStatus::State::FailedAtRuntime;
            status.reason = *failure;
            continue;
        }

        BlocklistRule const* matching_rule = nullptr;
        for (auto const& rule : rules) {
            if (rule.feature == feature && rule.matches(facts)) {
                matching_rule = &rule;
                break;
            }
        }

        // The rule is kept whatever the overrides decide, so that reports say which way the blocklist went.
        if (matching_rule != nullptr) {
            status.rule_id = matching_rule->id;
            status.reason = MUST(String::from_utf8(matching_rule->description));
        }

        if (overrides.force_enabled.contains(feature)) {
            status.forced = matching_rule != nullptr || overrides.disabled.contains(feature);
            if (matching_rule == nullptr && overrides.disabled.contains(feature))
                status.reason = "Overrides --disable-features"_string;
            continue;
        }

        if (overrides.disabled.contains(feature)) {
            status.state = FeatureStatus::State::DisabledByUser;
            continue;
        }

        if (matching_rule != nullptr)
            status.state = FeatureStatus::State::Blocklisted;
    }

    return decisions;
}

bool PlatformFeatureDecisions::decides_without_facts(FeatureProcess process, FeatureOverrides const& overrides, PlatformFeatures::Build const& build)
{
    // A process waits for the probe unless the overrides alone turn off each of its features, directly or through a
    // dependency that is off.
    for (auto feature : PlatformFeatures::all_features) {
        if (PlatformFeatures::process_of(feature) != process || !build.includes(feature))
            continue;
        auto is_turned_off = [&](Feature candidate) {
            return overrides.disabled.contains(candidate) && !overrides.force_enabled.contains(candidate);
        };
        bool turned_off = is_turned_off(feature);
        for (auto dependency = PlatformFeatures::dependency_of(feature); !turned_off && dependency.has_value(); dependency = PlatformFeatures::dependency_of(*dependency))
            turned_off = is_turned_off(*dependency);
        if (!turned_off)
            return false;
    }
    return true;
}

PlatformFeatures::ProcessFeatures PlatformFeatureDecisions::features_for(FeatureProcess process) const
{
    PlatformFeatures::ProcessFeatures features { process };
    for (auto feature : PlatformFeatures::all_features) {
        if (PlatformFeatures::process_of(feature) == process && is_enabled(feature))
            features.set_enabled(feature);
    }
    return features;
}

}
