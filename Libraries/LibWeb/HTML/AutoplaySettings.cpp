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

}
