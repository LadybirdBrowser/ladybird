/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/String.h>
#include <LibPlatformFeatures/Build.h>
#include <LibPlatformFeatures/Feature.h>
#include <LibPlatformFeatures/ProcessFeatures.h>
#include <LibPlatformFeatures/SystemFacts.h>
#include <LibWebView/Export.h>

namespace WebView {

using PlatformFeatures::Feature;
using PlatformFeatures::FeatureProcess;
using PlatformFeatures::SystemFacts;

// A feature that a driver or library claims to support, but which misbehaves under the conditions it matches.
struct BlocklistRule {
    StringView id;
    Feature feature;
    StringView description;
    bool (*matches)(SystemFacts const&);
};

WEBVIEW_API ReadonlySpan<BlocklistRule> blocklist_rules();

struct FeatureOverrides {
    HashTable<Feature> disabled;
    // Overrides the blocklist and the disabled set, but not a missing capability or a dependency that is off.
    HashTable<Feature> force_enabled;
};

struct FeatureStatus {
    enum class State : u8 {
        Enabled,
        Unsupported,
        Blocklisted,
        DisabledByUser,
        BlockedByDependency,
        FailedAtRuntime,
        // Not part of this build at all, so it is left out wherever features are listed.
        NotBuilt,
    };

    State state { State::Enabled };
    // What the state alone does not say: the missing capability, the failure, the rule that matched, or what forcing
    // overrode.
    String reason;
    // Set when a rule matched, even if the feature was disabled or forced on regardless.
    StringView rule_id;
    // Whether --force-enable-features turned the feature on past a rule or a disable.
    bool forced { false };
    // The feature whose own state turned this one off, when blocked by a dependency.
    Optional<Feature> blocking_feature;
};

WEBVIEW_API StringView feature_state_name(FeatureStatus::State);

class WEBVIEW_API PlatformFeatureDecisions {
public:
    static PlatformFeatureDecisions evaluate(SystemFacts const&, FeatureOverrides const&, HashMap<Feature, String> const& runtime_failures, ReadonlySpan<BlocklistRule> = blocklist_rules(), PlatformFeatures::Build const& = PlatformFeatures::this_build);
    // Whether the overrides alone decide every feature of the process, so that it need not wait for the probe.
    static bool decides_without_facts(FeatureProcess, FeatureOverrides const&, PlatformFeatures::Build const& = PlatformFeatures::this_build);

    FeatureStatus const& status_of(Feature feature) const { return m_statuses[to_underlying(feature)]; }
    bool is_enabled(Feature feature) const { return status_of(feature).state == FeatureStatus::State::Enabled; }

    PlatformFeatures::ProcessFeatures features_for(FeatureProcess) const;

private:
    Array<FeatureStatus, PlatformFeatures::all_features.size()> m_statuses;
};

}

template<>
struct AK::Formatter<WebView::FeatureStatus::State> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, WebView::FeatureStatus::State state)
    {
        return Formatter<StringView>::format(builder, WebView::feature_state_name(state));
    }
};
