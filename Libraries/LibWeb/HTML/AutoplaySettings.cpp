/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
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

void AutoplaySettings::set_policy(AutoplayPolicy policy, ReadonlySpan<String> allowlist)
{
    m_policy = policy;

    m_allowlist.clear_with_capacity();
    m_allowlist.ensure_capacity(allowlist.size());

    for (auto const& origin : allowlist) {
        auto url = URL::Parser::basic_parse(origin);

        if (!url.has_value())
            url = URL::Parser::basic_parse(MUST(String::formatted("https://{}", origin)));
        if (!url.has_value()) {
            dbgln("Invalid origin for autoplay allowlist: {}", origin);
            continue;
        }

        m_allowlist.append(url->origin());
    }
}

Optional<AutoplayPolicy> autoplay_policy_from_string(StringView string)
{
    if (string == "allow-audio-and-video"sv)
        return AutoplayPolicy::AllowAudioAndVideo;
    if (string == "block-audio"sv)
        return AutoplayPolicy::BlockAudio;
    if (string == "block-audio-and-video"sv)
        return AutoplayPolicy::BlockAudioAndVideo;
    return {};
}

StringView autoplay_policy_to_string(AutoplayPolicy policy)
{
    switch (policy) {
    case AutoplayPolicy::AllowAudioAndVideo:
        return "allow-audio-and-video"sv;
    case AutoplayPolicy::BlockAudio:
        return "block-audio"sv;
    case AutoplayPolicy::BlockAudioAndVideo:
        return "block-audio-and-video"sv;
    }

    VERIFY_NOT_REACHED();
}

}
