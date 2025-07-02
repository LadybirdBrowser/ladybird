/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

enum class AppendTLD {
    No,
    Yes,
};
WEBVIEW_API Optional<URL::URL> sanitize_url(StringView, Optional<SearchEngine> const& search_engine = {}, AppendTLD = AppendTLD::No);
WEBVIEW_API Vector<URL::URL> sanitize_urls(ReadonlySpan<ByteString> raw_urls, URL::URL const& new_tab_page_url);

struct URLParts {
    StringView scheme_and_subdomain;
    StringView effective_tld_plus_one;
    StringView remainder;
};
WEBVIEW_API Optional<URLParts> break_url_into_parts(StringView url);

// These are both used for the "right-click -> copy FOO" interaction for links.
enum class URLType {
    Email,
    Telephone,
    Other,
};
WEBVIEW_API URLType url_type(URL::URL const&);
WEBVIEW_API String url_text_to_copy(URL::URL const&);

}
