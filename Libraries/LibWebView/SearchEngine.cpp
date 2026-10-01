/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWebView/SearchEngine.h>

namespace WebView {

static auto const& s_builtin_search_engines = *new auto(to_array<SearchEngine>({
    { "Bing"_string, "https://www.bing.com/search?q=%s"_string, SearchSuggestions { "https://www.bing.com/osjson.aspx?query=%s"_string } },
    { "Brave"_string, "https://search.brave.com/search?q=%s"_string, SearchSuggestions { "https://search.brave.com/api/suggest?q=%s"_string } },
    { "DuckDuckGo"_string, "https://duckduckgo.com/?q=%s"_string, SearchSuggestions { "https://duckduckgo.com/ac/?q=%s"_string, SearchSuggestionsFormat::DuckDuckGo } },
    { "Ecosia"_string, "https://ecosia.org/search?q=%s"_string, SearchSuggestions { "https://ac.ecosia.org/autocomplete?q=%s&type=list"_string } },
    { "Google"_string, "https://www.google.com/search?q=%s"_string, SearchSuggestions { "https://www.google.com/complete/search?client=chrome&q=%s"_string } },
    { "Kagi"_string, "https://kagi.com/search?q=%s"_string, SearchSuggestions { "https://kagisuggest.com/api/autosuggest?q=%s"_string } },
    { "Mojeek"_string, "https://www.mojeek.com/search?q=%s"_string },
    { "Startpage"_string, "https://startpage.com/search?q=%s"_string },
    { "Yahoo"_string, "https://search.yahoo.com/search?p=%s"_string, SearchSuggestions { "https://search.yahoo.com/sugg/gossip/gossip-us-ura/?output=sd1&command=%s"_string, SearchSuggestionsFormat::Yahoo } },
    { "Yandex"_string, "https://yandex.com/search/?text=%s"_string, SearchSuggestions { "https://suggest.yandex.com/suggest-ff.cgi?part=%s&uil=en&v=3"_string } },
}));

ReadonlySpan<SearchEngine> builtin_search_engines()
{
    return s_builtin_search_engines;
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
