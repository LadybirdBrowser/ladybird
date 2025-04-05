/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>

namespace WebView {

struct SearchEngine {
    String format_search_query_for_display(StringView query) const;
    String format_search_query_for_navigation(StringView query) const;

    String name;
    String query_url;
};

ReadonlySpan<SearchEngine> builtin_search_engines();

}
