/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/PlatformFeatureDecisions.h>

using PlatformFeatures::Build;
using PlatformFeatures::FFmpegFacts;
using PlatformFeatures::GpuFacts;
using PlatformFeatures::ProcessFeatures;
using PlatformFeatures::VideoDecodeFacts;
using WebView::BlocklistRule;
using WebView::Feature;
using WebView::FeatureOverrides;
using WebView::FeatureProcess;
using WebView::FeatureStatus;
using WebView::PlatformFeatureDecisions;
using WebView::SystemFacts;

static constexpr FFmpegFacts ffmpeg_9_0 { .avcodec_version = FFmpegFacts::version(63, 1, 102), .avutil_version = FFmpegFacts::version(61, 1, 102), .shares_bundled_struct_layouts = true };

// As NVIDIA's driver reports them: references are decoded into the output, and no modifier tiles a DPB.
static SystemFacts facts_with_a_coinciding_dpb()
{
    VideoDecodeFacts coinciding_dpb { .supported = true, .dpb_and_output_coincide = true, .dpb_and_output_distinct = false, .dpb_supports_drm_format_modifiers = false };

    GpuFacts gpu;
    gpu.vendor_id = 0x10de;
    gpu.device_id = 0x2204;
    gpu.device_name = "NVIDIA GeForce RTX 3090"_string;
    gpu.driver_id = 4;
    gpu.driver_name = "NVIDIA"_string;
    gpu.driver_info = "615.78.08"_string;
    gpu.driver_version = 0x99e3a200;
    gpu.api_version = 0x4040df;
    gpu.device_identity = "7709ab0ea273f34e0bb993d4328b5309"_string;
    gpu.supports_ycbcr_sampling = true;
    gpu.supports_video_maintenance2 = true;
    gpu.h264 = coinciding_dpb;
    gpu.h265 = coinciding_dpb;
    gpu.vp9 = coinciding_dpb;
    gpu.av1 = coinciding_dpb;

    SystemFacts facts;
    facts.gpu = move(gpu);
    facts.bundled_ffmpeg = ffmpeg_9_0;
    facts.system_ffmpeg = ffmpeg_9_0;
    return facts;
}

// RADV on RDNA2 decodes into images apart from its references, which it cannot tile under a modifier.
static SystemFacts facts_with_a_separate_dpb()
{
    VideoDecodeFacts separate_dpb { .supported = true, .dpb_and_output_coincide = false, .dpb_and_output_distinct = true, .dpb_supports_drm_format_modifiers = false };

    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->vendor_id = 0x1002;
    facts.gpu->device_id = 0x73df;
    facts.gpu->device_name = "AMD Radeon RX 6700 XT (RADV NAVI22)"_string;
    facts.gpu->driver_id = 3;
    facts.gpu->driver_name = "radv"_string;
    facts.gpu->driver_info = "Mesa 26.2.5"_string;
    facts.gpu->h264 = separate_dpb;
    facts.gpu->h265 = separate_dpb;
    facts.gpu->vp9 = separate_dpb;
    facts.gpu->av1 = separate_dpb;
    return facts;
}

static bool matches_nvidia(SystemFacts const& facts)
{
    return facts.gpu.has_value() && facts.gpu->vendor_id == 0x10de;
}

static constexpr Array test_rules {
    BlocklistRule { "test-nvidia-ycbcr"sv, Feature::YCbCrImport, "NVIDIA cannot import video, for this test"sv, matches_nvidia },
};

static PlatformFeatureDecisions evaluate_with_test_rules(SystemFacts const& facts, FeatureOverrides const& overrides = {}, HashMap<Feature, String> const& runtime_failures = {})
{
    return PlatformFeatureDecisions::evaluate(facts, overrides, runtime_failures, test_rules, Build::with_every_feature());
}

static PlatformFeatureDecisions evaluate(SystemFacts const& facts, FeatureOverrides const& overrides = {})
{
    return PlatformFeatureDecisions::evaluate(facts, overrides, {}, WebView::blocklist_rules(), Build::with_every_feature());
}

TEST_CASE(every_dependency_is_declared_before_its_dependents)
{
    for (auto feature : PlatformFeatures::all_features) {
        if (auto dependency = PlatformFeatures::dependency_of(feature); dependency.has_value())
            EXPECT(to_underlying(*dependency) < to_underlying(feature));
        EXPECT_EQ(PlatformFeatures::feature_from_id(PlatformFeatures::feature_id(feature)), feature);
    }
}

TEST_CASE(a_build_includes_the_dependency_of_every_feature_it_includes)
{
    for (auto const& build : { PlatformFeatures::this_build, Build::with_every_feature(), Build {} }) {
        for (auto feature : PlatformFeatures::all_features) {
            if (auto dependency = PlatformFeatures::dependency_of(feature); dependency.has_value() && build.includes(feature))
                EXPECT(build.includes(*dependency));
        }
    }
}

TEST_CASE(a_feature_the_build_lacks_is_never_enabled)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::YCbCrImport);
    auto decisions = PlatformFeatureDecisions::evaluate({}, overrides, {}, test_rules, Build {});
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).state, FeatureStatus::State::NotBuilt);
    // Without Vulkan, nothing the probe reports says that the GPU cannot paint.
    EXPECT_EQ(decisions.features_for(FeatureProcess::Compositor).to_byte_string(), "gpu_painting"sv);
}

TEST_CASE(a_matching_rule_blocklists_its_feature)
{
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb());
    EXPECT(decisions.is_enabled(Feature::GpuPainting));

    auto const& status = decisions.status_of(Feature::YCbCrImport);
    EXPECT_EQ(status.state, FeatureStatus::State::Blocklisted);
    EXPECT_EQ(status.rule_id, "test-nvidia-ycbcr"sv);

    EXPECT(evaluate_with_test_rules(facts_with_a_separate_dpb()).is_enabled(Feature::YCbCrImport));
}

TEST_CASE(without_a_gpu_nothing_is_enabled)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu.clear();
    auto decisions = evaluate(facts);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::Unsupported);
    for (auto feature : PlatformFeatures::all_features) {
        if (feature != Feature::GpuPainting)
            EXPECT_EQ(decisions.status_of(feature).blocking_feature, Feature::GpuPainting);
    }
}

TEST_CASE(a_failed_probe_turns_off_gpu_painting)
{
    SystemFacts facts;
    facts.probe_failure = "The probe crashed"_string;
    auto decisions = evaluate(facts);
    auto const& status = decisions.status_of(Feature::GpuPainting);
    EXPECT_EQ(status.state, FeatureStatus::State::Unsupported);
    EXPECT(status.reason.contains("The probe crashed"sv));
}

TEST_CASE(a_missing_capability_blocks_dependents_with_the_root_cause)
{
    auto facts = facts_with_a_separate_dpb();
    facts.gpu->supports_ycbcr_sampling = false;
    auto decisions = evaluate(facts);

    EXPECT(decisions.is_enabled(Feature::GpuPainting));
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).state, FeatureStatus::State::Unsupported);
    for (auto feature : { Feature::VulkanVideo, Feature::VulkanVideoSystemFFmpeg, Feature::VulkanVideoSystemFFmpegDirectExport }) {
        auto const& status = decisions.status_of(feature);
        EXPECT_EQ(status.state, FeatureStatus::State::BlockedByDependency);
        EXPECT_EQ(status.blocking_feature, Feature::YCbCrImport);
    }
}

TEST_CASE(old_system_ffmpeg_with_a_separate_dpb_blocks_only_its_direct_export)
{
    auto decisions = evaluate(facts_with_a_separate_dpb());

    auto const& status = decisions.status_of(Feature::VulkanVideoSystemFFmpegDirectExport);
    EXPECT_EQ(status.state, FeatureStatus::State::Blocklisted);
    EXPECT_EQ(status.rule_id, "system-ffmpeg-separate-dpb-modifiers"sv);

    EXPECT(decisions.is_enabled(Feature::VulkanVideoSystemFFmpeg));
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpegDirectExport));
}

TEST_CASE(fixed_system_ffmpeg_exports_directly)
{
    auto facts = facts_with_a_separate_dpb();
    facts.system_ffmpeg->avcodec_version = FFmpegFacts::version(63, 20, 101);
    EXPECT(evaluate(facts).is_enabled(Feature::VulkanVideoSystemFFmpegDirectExport));
}

TEST_CASE(a_coinciding_dpb_exports_directly_on_old_system_ffmpeg)
{
    EXPECT(evaluate(facts_with_a_coinciding_dpb()).is_enabled(Feature::VulkanVideoSystemFFmpegDirectExport));
}

TEST_CASE(a_gpu_that_cannot_decode_turns_off_vulkan_video)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->cannot_decode_video_because = "The GPU's Vulkan is older than 1.3"_string;
    auto decisions = evaluate(facts);

    auto const& status = decisions.status_of(Feature::VulkanVideo);
    EXPECT_EQ(status.state, FeatureStatus::State::Unsupported);
    EXPECT_EQ(status.reason, "The GPU's Vulkan is older than 1.3"sv);
    EXPECT(decisions.is_enabled(Feature::YCbCrImport));
}

TEST_CASE(each_ffmpeg_needs_a_codec_the_gpu_decodes)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->h264.supported = false;
    facts.gpu->h265.supported = false;
    auto decisions = evaluate(facts);
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoSystemFFmpeg).state, FeatureStatus::State::Unsupported);
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpeg));
}

TEST_CASE(system_ffmpeg_needs_matching_struct_layouts)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.system_ffmpeg->shares_bundled_struct_layouts = false;
    auto decisions = evaluate(facts);
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoSystemFFmpeg).state, FeatureStatus::State::Unsupported);
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpeg));

    facts.system_ffmpeg.clear();
    EXPECT_EQ(evaluate(facts).status_of(Feature::VulkanVideoSystemFFmpeg).state, FeatureStatus::State::Unsupported);
}

TEST_CASE(force_enabling_cannot_supply_a_capability)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->supports_ycbcr_sampling = false;
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::YCbCrImport);
    EXPECT_EQ(evaluate(facts, overrides).status_of(Feature::YCbCrImport).state, FeatureStatus::State::Unsupported);
}

TEST_CASE(force_enabling_overrides_the_blocklist)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::VulkanVideoSystemFFmpegDirectExport);
    auto decisions = evaluate(facts_with_a_separate_dpb(), overrides);
    EXPECT(decisions.is_enabled(Feature::VulkanVideoSystemFFmpegDirectExport));
    EXPECT(decisions.status_of(Feature::VulkanVideoSystemFFmpegDirectExport).forced);
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoSystemFFmpegDirectExport).rule_id, "system-ffmpeg-separate-dpb-modifiers"sv);
}

TEST_CASE(each_process_is_given_only_its_own_features)
{
    auto decisions = evaluate(facts_with_a_separate_dpb());
    EXPECT_EQ(decisions.features_for(FeatureProcess::Compositor).to_byte_string(), "gpu_painting,ycbcr_import"sv);
    EXPECT_EQ(decisions.features_for(FeatureProcess::VideoDecoderServer).to_byte_string(),
        "vulkan_video,vulkan_video.bundled_ffmpeg,vulkan_video.bundled_ffmpeg.direct_export,vulkan_video.bundled_ffmpeg.vp9,vulkan_video.bundled_ffmpeg.av1,vulkan_video.bundled_ffmpeg.av1.keep_sequence_header_enable_ref_mvs_flag,vulkan_video.system_ffmpeg,vulkan_video.system_ffmpeg.h264,vulkan_video.system_ffmpeg.h265"sv);
}

TEST_CASE(nvidia_av1_decoding_works_around_the_tiles_bug)
{
    auto decisions = evaluate(facts_with_a_coinciding_dpb());
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpegAV1));
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag).rule_id, "nvidia-av1-tiles"sv);
    EXPECT(evaluate(facts_with_a_separate_dpb()).is_enabled(Feature::VulkanVideoBundledFFmpegAV1KeepSequenceHeaderEnableRefMVsFlag));
}

TEST_CASE(nvidia_av1_decoding_without_the_workaround_is_blocklisted)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->supports_video_maintenance2 = false;
    auto decisions = evaluate(facts);
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoBundledFFmpegAV1).rule_id, "nvidia-av1-tiles-without-video-maintenance2"sv);
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpegVP9));

    facts = facts_with_a_separate_dpb();
    facts.gpu->supports_video_maintenance2 = false;
    EXPECT(evaluate(facts).is_enabled(Feature::VulkanVideoBundledFFmpegAV1));
}

TEST_CASE(a_codec_the_gpu_does_not_decode_is_unsupported)
{
    auto facts = facts_with_a_coinciding_dpb();
    facts.gpu->av1.supported = false;
    auto decisions = evaluate(facts);
    EXPECT_EQ(decisions.status_of(Feature::VulkanVideoBundledFFmpegAV1).state, FeatureStatus::State::Unsupported);
    EXPECT(decisions.is_enabled(Feature::VulkanVideoBundledFFmpegVP9));
}

TEST_CASE(disabling_a_feature_turns_off_its_dependents_with_the_root_cause)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::GpuPainting);
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb(), overrides);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::DisabledByUser);
    for (auto feature : PlatformFeatures::all_features) {
        if (feature != Feature::GpuPainting)
            EXPECT_EQ(decisions.status_of(feature).blocking_feature, Feature::GpuPainting);
    }
}

TEST_CASE(force_enabling_overrides_rules_and_disables)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::YCbCrImport);
    overrides.disabled.set(Feature::YCbCrImport);
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb(), overrides);

    EXPECT(decisions.is_enabled(Feature::YCbCrImport));
    EXPECT(decisions.status_of(Feature::YCbCrImport).forced);
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).rule_id, "test-nvidia-ycbcr"sv);

    overrides.disabled.clear();
    decisions = evaluate_with_test_rules(facts_with_a_separate_dpb(), overrides);
    EXPECT(!decisions.status_of(Feature::YCbCrImport).forced);
    EXPECT(decisions.status_of(Feature::YCbCrImport).reason.is_empty());
}

TEST_CASE(a_disabled_feature_still_names_the_rule_that_matched_it)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::YCbCrImport);
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb(), overrides);
    auto const& status = decisions.status_of(Feature::YCbCrImport);
    EXPECT_EQ(status.state, FeatureStatus::State::DisabledByUser);
    EXPECT_EQ(status.rule_id, "test-nvidia-ycbcr"sv);
}

TEST_CASE(force_enabling_does_not_reach_past_a_dependency)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::GpuPainting);
    overrides.force_enabled.set(Feature::YCbCrImport);
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb(), overrides);

    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).state, FeatureStatus::State::BlockedByDependency);
}

TEST_CASE(runtime_failures_win_over_force_enabling)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::GpuPainting);
    HashMap<Feature, String> runtime_failures;
    runtime_failures.set(Feature::GpuPainting, "The Compositor crashed repeatedly"_string);
    auto decisions = evaluate_with_test_rules(facts_with_a_coinciding_dpb(), overrides, runtime_failures);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::FailedAtRuntime);
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).blocking_feature, Feature::GpuPainting);
}

TEST_CASE(overrides_that_turn_everything_off_need_no_facts)
{
    for (auto process : { FeatureProcess::Compositor, FeatureProcess::VideoDecoderServer }) {
        EXPECT(!PlatformFeatureDecisions::decides_without_facts(process, {}, Build::with_every_feature()));

        FeatureOverrides overrides;
        overrides.disabled.set(Feature::GpuPainting);
        EXPECT(PlatformFeatureDecisions::decides_without_facts(process, overrides, Build::with_every_feature()));

        overrides.force_enabled.set(Feature::GpuPainting);
        EXPECT(!PlatformFeatureDecisions::decides_without_facts(process, overrides, Build::with_every_feature()));
    }
}

TEST_CASE(process_features_round_trip_through_their_text_form)
{
    auto features = MUST(ProcessFeatures::from_string(FeatureProcess::VideoDecoderServer, "vulkan_video.system_ffmpeg,vulkan_video"sv));
    EXPECT(features.is_enabled(Feature::VulkanVideo));
    EXPECT(features.is_enabled(Feature::VulkanVideoSystemFFmpeg));
    EXPECT(!features.is_enabled(Feature::VulkanVideoBundledFFmpeg));
    EXPECT_EQ(features.to_byte_string(), "vulkan_video,vulkan_video.system_ffmpeg"sv);

    auto none = MUST(ProcessFeatures::from_string(FeatureProcess::Compositor, ""sv));
    EXPECT(!none.is_enabled(Feature::GpuPainting));
}

TEST_CASE(process_features_reject_unknown_and_foreign_features)
{
    EXPECT(ProcessFeatures::from_string(FeatureProcess::Compositor, "gpu_painting,teleportation"sv).is_error());
    EXPECT(ProcessFeatures::from_string(FeatureProcess::Compositor, "vulkan_video"sv).is_error());
}

TEST_CASE(system_facts_round_trip_through_json)
{
    auto facts = facts_with_a_separate_dpb();
    facts.gpu->cannot_decode_video_because = "Nothing, this is a test"_string;
    EXPECT(MUST(SystemFacts::from_json(facts.to_json().serialized())) == facts);

    facts.gpu.clear();
    facts.system_ffmpeg.clear();
    EXPECT(MUST(SystemFacts::from_json(facts.to_json().serialized())) == facts);
}

TEST_CASE(system_facts_reject_malformed_json)
{
    EXPECT(SystemFacts::from_json("[]"sv).is_error());
    EXPECT(SystemFacts::from_json("{\"gpu\":3}"sv).is_error());

    auto facts = facts_with_a_separate_dpb();
    auto json = facts.to_json();
    json.get_object("gpu"sv)->remove("h264"sv);
    EXPECT(SystemFacts::from_json(json.serialized()).is_error());

    facts.gpu->driver_info = MUST(String::repeated('x', 1025));
    EXPECT(SystemFacts::from_json(facts.to_json().serialized()).is_error());
}
