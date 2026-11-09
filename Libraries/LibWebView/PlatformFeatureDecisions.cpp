/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PlatformFeatureDecisions.h>

namespace WebView {

ReadonlySpan<BlocklistRule> blocklist_rules()
{
    static constexpr Array<BlocklistRule, 0> rules {};
    return rules;
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
