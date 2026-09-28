/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    // An origin in the allowlist may always autoplay, with or without audio.
    for (auto const& allowed : m_allowlist) {
        if (allowed.is_same_origin_domain(origin))
            return AutoplayDecision::Allowed;
    }

    // AD-HOC: Allow autoplay for file:// URLs if the document is also from a file:// URL.
    if (origin.is_opaque_file_origin() && document.origin().is_opaque_file_origin())
        return AutoplayDecision::Allowed;

    switch (m_policy) {
    case AutoplayPolicy::AllowAudioAndVideo:
        return AutoplayDecision::Allowed;
    case AutoplayPolicy::BlockAudio:
        return AutoplayDecision::AllowedIfInaudible;
    case AutoplayPolicy::BlockAudioAndVideo:
        return AutoplayDecision::Blocked;
    }

    VERIFY_NOT_REACHED();
}

void AutoplaySettings::set_policy(AutoplayPolicy policy, ReadonlySpan<Utf16String> allowlist)
{
    m_policy = policy;

    m_allowlist.clear_with_capacity();
    m_allowlist.ensure_capacity(allowlist.size());

    for (auto const& origin : allowlist) {
        auto url = DOMURL::parse(origin);

        if (!url.has_value())
            url = DOMURL::parse(Utf16String::formatted("https://{}", origin));
        if (!url.has_value()) {
            dbgln("Invalid origin for autoplay allowlist: {}", origin);
            continue;
        }

        m_allowlist.append(url->origin());
    }
}

}
