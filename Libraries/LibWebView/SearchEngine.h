/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct SearchEngine {
    WEBVIEW_API String format_search_query_for_display(StringView query) const;
    WEBVIEW_API String format_search_query_for_navigation(StringView query) const;

    String name;
    String query_url;
};

WEBVIEW_API ReadonlySpan<SearchEngine> builtin_search_engines();

}
