/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/PlatformFeatureDecisions.h>

using PlatformFeatures::Build;
using PlatformFeatures::GpuFacts;
using PlatformFeatures::ProcessFeatures;
using WebView::BlocklistRule;
using WebView::Feature;
using WebView::FeatureOverrides;
using WebView::FeatureProcess;
using WebView::FeatureStatus;
using WebView::PlatformFeatureDecisions;
using WebView::SystemFacts;

static SystemFacts facts_with_a_gpu()
{
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

    SystemFacts facts;
    facts.gpu = move(gpu);
    return facts;
}

static bool matches_nvidia(SystemFacts const& facts)
{
    return facts.gpu.has_value() && facts.gpu->vendor_id == 0x10de;
}

static constexpr Array test_rules {
    BlocklistRule { "test-nvidia-ycbcr"sv, Feature::YCbCrImport, "NVIDIA cannot import video, for this test"sv, matches_nvidia },
};

static PlatformFeatureDecisions evaluate(SystemFacts const& facts, FeatureOverrides const& overrides = {}, HashMap<Feature, String> const& runtime_failures = {})
{
    return PlatformFeatureDecisions::evaluate(facts, overrides, runtime_failures, test_rules, Build::with_every_feature());
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
    auto decisions = evaluate(facts_with_a_gpu());
    EXPECT(decisions.is_enabled(Feature::GpuPainting));

    auto const& status = decisions.status_of(Feature::YCbCrImport);
    EXPECT_EQ(status.state, FeatureStatus::State::Blocklisted);
    EXPECT_EQ(status.rule_id, "test-nvidia-ycbcr"sv);

    auto facts = facts_with_a_gpu();
    facts.gpu->vendor_id = 0x1002;
    EXPECT(evaluate(facts).is_enabled(Feature::YCbCrImport));
}

TEST_CASE(without_a_gpu_nothing_is_enabled)
{
    SystemFacts facts;
    auto decisions = evaluate(facts);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::Unsupported);
    auto const& status = decisions.status_of(Feature::YCbCrImport);
    EXPECT_EQ(status.state, FeatureStatus::State::BlockedByDependency);
    EXPECT_EQ(status.blocking_feature, Feature::GpuPainting);
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

TEST_CASE(a_missing_capability_is_unsupported_rather_than_blocklisted)
{
    auto facts = facts_with_a_gpu();
    facts.gpu->supports_ycbcr_sampling = false;
    EXPECT_EQ(evaluate(facts).status_of(Feature::YCbCrImport).state, FeatureStatus::State::Unsupported);
}

TEST_CASE(disabling_a_feature_turns_off_its_dependents_with_the_root_cause)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::GpuPainting);
    auto decisions = evaluate(facts_with_a_gpu(), overrides);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::DisabledByUser);
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).blocking_feature, Feature::GpuPainting);
}

TEST_CASE(force_enabling_overrides_rules_and_disables)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::YCbCrImport);
    overrides.disabled.set(Feature::YCbCrImport);
    auto decisions = evaluate(facts_with_a_gpu(), overrides);

    EXPECT(decisions.is_enabled(Feature::YCbCrImport));
    EXPECT(decisions.status_of(Feature::YCbCrImport).forced);
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).rule_id, "test-nvidia-ycbcr"sv);

    overrides.disabled.clear();
    auto facts = facts_with_a_gpu();
    facts.gpu->vendor_id = 0x1002;
    decisions = evaluate(facts, overrides);
    EXPECT(!decisions.status_of(Feature::YCbCrImport).forced);
    EXPECT(decisions.status_of(Feature::YCbCrImport).reason.is_empty());
}

TEST_CASE(a_disabled_feature_still_names_the_rule_that_matched_it)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::YCbCrImport);
    auto decisions = evaluate(facts_with_a_gpu(), overrides);
    auto const& status = decisions.status_of(Feature::YCbCrImport);
    EXPECT_EQ(status.state, FeatureStatus::State::DisabledByUser);
    EXPECT_EQ(status.rule_id, "test-nvidia-ycbcr"sv);
}

TEST_CASE(force_enabling_does_not_reach_past_a_dependency)
{
    FeatureOverrides overrides;
    overrides.disabled.set(Feature::GpuPainting);
    overrides.force_enabled.set(Feature::YCbCrImport);
    auto decisions = evaluate(facts_with_a_gpu(), overrides);

    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).state, FeatureStatus::State::BlockedByDependency);
}

TEST_CASE(force_enabling_cannot_supply_a_capability)
{
    auto facts = facts_with_a_gpu();
    facts.gpu->supports_ycbcr_sampling = false;
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::YCbCrImport);
    EXPECT_EQ(evaluate(facts, overrides).status_of(Feature::YCbCrImport).state, FeatureStatus::State::Unsupported);
}

TEST_CASE(runtime_failures_win_over_force_enabling)
{
    FeatureOverrides overrides;
    overrides.force_enabled.set(Feature::GpuPainting);
    HashMap<Feature, String> runtime_failures;
    runtime_failures.set(Feature::GpuPainting, "The Compositor crashed repeatedly"_string);
    auto decisions = evaluate(facts_with_a_gpu(), overrides, runtime_failures);

    EXPECT_EQ(decisions.status_of(Feature::GpuPainting).state, FeatureStatus::State::FailedAtRuntime);
    EXPECT_EQ(decisions.status_of(Feature::YCbCrImport).blocking_feature, Feature::GpuPainting);
}

TEST_CASE(overrides_that_turn_everything_off_need_no_facts)
{
    EXPECT(!PlatformFeatureDecisions::decides_without_facts(FeatureProcess::Compositor, {}, Build::with_every_feature()));

    FeatureOverrides overrides;
    overrides.disabled.set(Feature::GpuPainting);
    EXPECT(PlatformFeatureDecisions::decides_without_facts(FeatureProcess::Compositor, overrides, Build::with_every_feature()));

    overrides.force_enabled.set(Feature::GpuPainting);
    EXPECT(!PlatformFeatureDecisions::decides_without_facts(FeatureProcess::Compositor, overrides, Build::with_every_feature()));
}

TEST_CASE(each_process_is_given_its_enabled_features)
{
    auto facts = facts_with_a_gpu();
    facts.gpu->vendor_id = 0x1002;
    EXPECT_EQ(evaluate(facts).features_for(FeatureProcess::Compositor).to_byte_string(), "gpu_painting,ycbcr_import"sv);
    EXPECT_EQ(evaluate(facts_with_a_gpu()).features_for(FeatureProcess::Compositor).to_byte_string(), "gpu_painting"sv);
}

TEST_CASE(process_features_round_trip_through_their_text_form)
{
    auto features = MUST(ProcessFeatures::from_string(FeatureProcess::Compositor, "ycbcr_import,gpu_painting"sv));
    EXPECT(features.is_enabled(Feature::GpuPainting));
    EXPECT(features.is_enabled(Feature::YCbCrImport));
    EXPECT_EQ(features.to_byte_string(), "gpu_painting,ycbcr_import"sv);

    auto none = MUST(ProcessFeatures::from_string(FeatureProcess::Compositor, ""sv));
    EXPECT(!none.is_enabled(Feature::GpuPainting));
}

TEST_CASE(process_features_reject_unknown_features)
{
    EXPECT(ProcessFeatures::from_string(FeatureProcess::Compositor, "gpu_painting,teleportation"sv).is_error());
}

TEST_CASE(system_facts_round_trip_through_json)
{
    auto facts = facts_with_a_gpu();
    EXPECT(MUST(SystemFacts::from_json(facts.to_json().serialized())) == facts);

    facts.gpu.clear();
    EXPECT(MUST(SystemFacts::from_json(facts.to_json().serialized())) == facts);
}

TEST_CASE(system_facts_reject_malformed_json)
{
    EXPECT(SystemFacts::from_json("[]"sv).is_error());
    EXPECT(SystemFacts::from_json("{\"gpu\":3}"sv).is_error());

    auto facts = facts_with_a_gpu();
    auto json = facts.to_json();
    json.get_object("gpu"sv)->remove("supports_ycbcr_sampling"sv);
    EXPECT(SystemFacts::from_json(json.serialized()).is_error());

    facts.gpu->driver_info = MUST(String::repeated('x', 1025));
    EXPECT(SystemFacts::from_json(facts.to_json().serialized()).is_error());
}
