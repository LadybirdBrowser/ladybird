/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StringView.h>

namespace WebView {

struct SearchEngine {
    StringView name;
    StringView query_url;
};

ReadonlySpan<SearchEngine> search_engines();
SearchEngine const& default_search_engine();
Optional<SearchEngine const&> find_search_engine_by_name(StringView name);
Optional<SearchEngine const&> find_search_engine_by_query_url(StringView query_url);
String format_search_query_for_display(StringView query_url, StringView query);

}
