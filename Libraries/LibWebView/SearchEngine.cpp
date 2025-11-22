/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <LibURL/URL.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

static auto s_builtin_search_engines = to_array<SearchEngine>({
    { "Bing"_string, "https://www.bing.com/search?q=%s"_string, "b"_string },
    { "Brave"_string, "https://search.brave.com/search?q=%s"_string, "brave"_string },
    { "DuckDuckGo"_string, "https://duckduckgo.com/?q=%s"_string, "ddg"_string },
    { "Ecosia"_string, "https://ecosia.org/search?q=%s"_string, "ec"_string },
    { "Google"_string, "https://www.google.com/search?q=%s"_string, "g"_string },
    { "Kagi"_string, "https://kagi.com/search?q=%s"_string, "kagi"_string },
    { "Mojeek"_string, "https://www.mojeek.com/search?q=%s"_string, "mojeek"_string },
    { "Startpage"_string, "https://startpage.com/search?q=%s"_string, "startpage"_string },
    { "Yahoo"_string, "https://search.yahoo.com/search?p=%s"_string, "y"_string },
    { "Yandex"_string, "https://yandex.com/search/?text=%s"_string, "ya"_string },
    { "YouTube"_string, "https://www.youtube.com/results?search_query=%s"_string, "yt"_string },
});

ReadonlySpan<SearchEngine> builtin_search_engines()
{
    return s_builtin_search_engines;
}

Optional<SearchEngine const&> find_search_engine_by_bang(String bang)
{
    bang = MUST(bang.substring_from_byte_offset(1));
    if (bang.length_in_code_units() == 0)
        return {};

    auto it = AK::find_if(s_builtin_search_engines.begin(), s_builtin_search_engines.end(),
        [&](auto const& engine) {
            return engine.bang == bang;
        });

    if (it == s_builtin_search_engines.end())
        return {};

    return *it;
}

String SearchEngine::format_search_query_for_display(StringView query) const
{
    static constexpr auto MAX_SEARCH_STRING_LENGTH = 32;

    return MUST(String::formatted("Search {} for \"{:.{}}{}\"",
        name,
        query,
        MAX_SEARCH_STRING_LENGTH,
        query.length() > MAX_SEARCH_STRING_LENGTH ? "..."sv : ""sv));
}

String SearchEngine::format_search_query_for_navigation(StringView query) const
{
    return MUST(query_url.replace("%s"sv, URL::percent_encode(query), ReplaceMode::All));
}

}
