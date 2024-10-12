/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <AK/String.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

static constexpr auto builtin_search_engines = Array {
    SearchEngine { "Bing"sv, "https://www.bing.com/search?q={}"sv },
    SearchEngine { "Brave"sv, "https://search.brave.com/search?q={}"sv },
    SearchEngine { "DuckDuckGo"sv, "https://duckduckgo.com/?q={}"sv },
    SearchEngine { "Ecosia"sv, "https://ecosia.org/search?q={}"sv },
    SearchEngine { "GitHub"sv, "https://github.com/search?q={}"sv },
    SearchEngine { "Google"sv, "https://www.google.com/search?q={}"sv },
    SearchEngine { "GoogleScholar"sv, "https://scholar.google.com/scholar?q={}"sv },
    SearchEngine { "Kagi"sv, "https://kagi.com/search?q={}"sv },
    SearchEngine { "Mojeek"sv, "https://www.mojeek.com/search?q={}"sv },
    SearchEngine { "Startpage"sv, "https://startpage.com/search?q={}"sv },
    SearchEngine { "Wikipedia"sv, "https://en.wikipedia.org/w/index.php?title=Special:Search&search={}"sv },
    SearchEngine { "Yahoo"sv, "https://search.yahoo.com/search?p={}"sv },
    SearchEngine { "Yandex"sv, "https://yandex.com/search/?text={}"sv },
};

ReadonlySpan<SearchEngine> search_engines()
{
    return builtin_search_engines;
}

SearchEngine const& default_search_engine()
{
    static auto default_engine = find_search_engine_by_name("Google"sv);
    VERIFY(default_engine.has_value());

    return *default_engine;
}

Optional<SearchEngine const&> find_search_engine_by_name(StringView name)
{
    auto it = AK::find_if(builtin_search_engines.begin(), builtin_search_engines.end(),
        [&](auto const& engine) {
            return engine.name == name;
        });

    if (it == builtin_search_engines.end())
        return {};

    return *it;
}

Optional<SearchEngine const&> find_search_engine_by_query_url(StringView query_url)
{
    auto it = AK::find_if(builtin_search_engines.begin(), builtin_search_engines.end(),
        [&](auto const& engine) {
            return engine.query_url == query_url;
        });

    if (it == builtin_search_engines.end())
        return {};

    return *it;
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
