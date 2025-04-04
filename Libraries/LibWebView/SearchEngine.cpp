/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

static auto builtin_search_engines = to_array<SearchEngine>({
    { "Bing"_string, "https://www.bing.com/search?q={}"_string },
    { "Brave"_string, "https://search.brave.com/search?q={}"_string },
    { "DuckDuckGo"_string, "https://duckduckgo.com/?q={}"_string },
    { "Ecosia"_string, "https://ecosia.org/search?q={}"_string },
    { "Google"_string, "https://www.google.com/search?q={}"_string },
    { "Kagi"_string, "https://kagi.com/search?q={}"_string },
    { "Mojeek"_string, "https://www.mojeek.com/search?q={}"_string },
    { "Startpage"_string, "https://startpage.com/search?q={}"_string },
    { "Yahoo"_string, "https://search.yahoo.com/search?p={}"_string },
    { "Yandex"_string, "https://yandex.com/search/?text={}"_string },
});

ReadonlySpan<SearchEngine> search_engines()
{
    return builtin_search_engines;
}

Optional<SearchEngine> find_search_engine_by_name(StringView name)
{
    return find_value(builtin_search_engines, [&](auto const& engine) {
        return engine.name == name;
    });
}

Optional<SearchEngine const&> find_search_engine_by_query_url(StringView query_url)
{
    return find_value(builtin_search_engines, [&](auto const& engine) {
        return engine.query_url == query_url;
    });
}

String format_search_query_for_display(StringView query_url, StringView query)
{
    static constexpr auto MAX_SEARCH_STRING_LENGTH = 32;

    if (auto search_engine = find_search_engine_by_query_url(query_url); search_engine.has_value()) {
        return MUST(String::formatted("Search {} for \"{:.{}}{}\"",
            search_engine->name,
            query,
            MAX_SEARCH_STRING_LENGTH,
            query.length() > MAX_SEARCH_STRING_LENGTH ? "..."sv : ""sv));
    }

    return MUST(String::formatted("Search for \"{:.{}}{}\"",
        query,
        MAX_SEARCH_STRING_LENGTH,
        query.length() > MAX_SEARCH_STRING_LENGTH ? "..."sv : ""sv));
}

}
