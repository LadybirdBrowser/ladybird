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
    String name;
    String query_url;
};

ReadonlySpan<SearchEngine> search_engines();
Optional<SearchEngine> find_search_engine_by_name(StringView name);
Optional<SearchEngine const&> find_search_engine_by_query_url(StringView query_url);
String format_search_query_for_display(StringView query_url, StringView query);

}
