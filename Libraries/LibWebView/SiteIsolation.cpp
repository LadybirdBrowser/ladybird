/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWebView/SiteIsolation.h>

namespace WebView {

static bool s_site_isolation_enabled = true;

void disable_site_isolation()
{
    s_site_isolation_enabled = false;
}

bool is_url_suitable_for_same_process_navigation(URL::URL const& current_url, URL::URL const& target_url)
{
    if (!s_site_isolation_enabled)
        return true;

    // Allow navigating from about:blank to any site.
    if (Web::HTML::url_matches_about_blank(current_url))
        return true;

    // Make sure JavaScript URLs run in the same process.
    if (target_url.scheme() == "javascript"sv)
        return true;

    // Allow cross-scheme non-HTTP(S) navigation. Disallow cross-scheme HTTP(s) navigation.
    auto current_url_is_http = Web::Fetch::Infrastructure::is_http_or_https_scheme(current_url.scheme());
    auto target_url_is_http = Web::Fetch::Infrastructure::is_http_or_https_scheme(target_url.scheme());

    if (!current_url_is_http || !target_url_is_http)
        return !current_url_is_http && !target_url_is_http;

    // Disallow cross-site HTTP(S) navigation.
    return current_url.origin().is_same_site(target_url.origin());
}

}
