/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/AutoplaySettings.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

AutoplaySettings& AutoplaySettings::the()
{
    static auto& settings = *new AutoplaySettings;
    return settings;
}

AutoplaySettings::AutoplaySettings() = default;
AutoplaySettings::~AutoplaySettings() = default;

AutoplayDecision AutoplaySettings::decision_for_origin(DOM::Document const& document, URL::Origin const& origin) const
{
    auto policy = m_policy;
    auto has_site_policy = false;

    for (auto const& site_policy : m_site_policies) {
        if (site_policy.origin.is_same_origin_domain(origin)) {
            policy = site_policy.policy;
            has_site_policy = true;
            break;
        }
    }

    // AD-HOC: Allow autoplay for file:// URLs if the document is also from a file:// URL.
    if (!has_site_policy && origin.is_opaque_file_origin() && document.origin().is_opaque_file_origin())
        return AutoplayDecision::Allowed;

    switch (policy) {
    case AutoplayPolicy::AllowAudioAndVideo:
        return AutoplayDecision::Allowed;
    case AutoplayPolicy::BlockAudio:
        return AutoplayDecision::AllowedIfInaudible;
    case AutoplayPolicy::BlockAudioAndVideo:
        return AutoplayDecision::Blocked;
    }

    VERIFY_NOT_REACHED();
}

void AutoplaySettings::set_policy(AutoplayPolicy policy, OrderedHashMap<String, AutoplayPolicy> const& site_policies)
{
    m_policy = policy;

    m_site_policies.clear_with_capacity();
    m_site_policies.ensure_capacity(site_policies.size());

    for (auto const& [filter, site_policy] : site_policies) {
        auto origin = Utf16String::from_utf8(filter);
        auto url = DOMURL::parse(origin);

        if (!url.has_value()) {
            dbgln("Invalid origin for autoplay exception: {}", origin);
            continue;
        }

        m_site_policies.append({ url->origin(), site_policy });
    }
}

bool system_allows_media_playback(DOM::Document const& document, IsAudible is_audible)
{
    // NB: We allow playback once the document has been activated by the user, with an exception for inaudible media.
    //     Gating on transient activation instead pauses audible media once the activation expires, e.g. between ads,
    //     or media in a playlist.
    if (auto window = document.window(); window && window->has_sticky_activation())
        return true;

    // NB: The sticky activation of an ancestor also counts if the document is allowed to use the "autoplay" feature.
    //     A cross-origin iframe is only allowed to use it if its container allows it.
    if (document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Autoplay)) {
        for (auto const& ancestor : document.ancestor_navigables()) {
            if (ancestor->active_window_has_sticky_activation())
                return true;
        }
    }

    switch (AutoplaySettings::the().decision_for_origin(document, document.origin())) {
    case AutoplayDecision::Allowed:
        return true;
    case AutoplayDecision::AllowedIfInaudible:
        return is_audible == IsAudible::No;
    case AutoplayDecision::Blocked:
        return false;
    }

    VERIFY_NOT_REACHED();
}

}
