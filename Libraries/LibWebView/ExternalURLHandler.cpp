/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWebView/ExternalURLHandler.h>

namespace WebView {

static constexpr Array denied_schemes {
    "afp"sv,
    "data"sv,
    "disk"sv,
    "disks"sv,
    "file"sv,
    "hcp"sv,
    "ie.http"sv,
    "javascript"sv,
    "mk"sv,
    "ms-help"sv,
    "nntp"sv,
    "res"sv,
    "shell"sv,
    "vbscript"sv,
    "view-source"sv,
    "vnd.ms.radio"sv,
};

bool ExternalURLRequestPolicy::is_denied_scheme(StringView scheme)
{
    // Block all single-letter schemes.
    if (scheme.length() == 1)
        return true;
    return denied_schemes.contains_slow(scheme);
}

static constexpr auto request_allowance_lifetime = AK::Duration::from_seconds(5);

ExternalURLAction ExternalURLRequestPolicy::decide(URL::URL const& url, bool has_transient_activation, MonotonicTime now)
{
    if (is_denied_scheme(url.scheme()))
        return ExternalURLAction::Block;

    if (!m_request_allowance.has_value())
        return ExternalURLAction::Block;

    if (now - m_request_allowance->granted_at > request_allowance_lifetime) {
        m_request_allowance.clear();
        return ExternalURLAction::Block;
    }

    if (m_request_allowance->source == ExternalURLRequestSource::BrowserUI
        && m_request_allowance->expected_url != url)
        return ExternalURLAction::Block;

    // Don't prompt when the user activated a mailto: link. This matches other browsers.
    if (url.scheme() == "mailto"sv
        && (has_transient_activation || m_request_allowance->source == ExternalURLRequestSource::BrowserUI))
        return ExternalURLAction::Launch;

    return ExternalURLAction::Prompt;
}

Optional<ExternalURLRequestSource> ExternalURLRequestPolicy::take_request_allowance()
{
    if (!m_request_allowance.has_value())
        return {};
    return m_request_allowance.release_value().source;
}

void ExternalURLRequestPolicy::clear_page_request_allowance()
{
    if (m_request_allowance.has_value() && m_request_allowance->source == ExternalURLRequestSource::Page)
        m_request_allowance.clear();
}

}
