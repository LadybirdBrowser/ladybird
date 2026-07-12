/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AutocompleteMuxer.h>

namespace {

WebView::AutocompleteSuggestion suggestion(
    WebView::AutocompleteSuggestionSource source,
    StringView text,
    i32 relevance,
    bool can_be_automatically_selected,
    bool is_verbatim = false)
{
    return {
        .source = source,
        .text = MUST(String::from_utf8(text)),
        .title = {},
        .subtitle = {},
        .favicon_base64_png = {},
        .highlight_input = {},
        .relevance = relevance,
        .is_verbatim = is_verbatim,
        .can_be_automatically_selected = can_be_automatically_selected,
    };
}

WebView::AutocompleteSuggestion search(StringView text, i32 relevance, bool is_verbatim = false)
{
    return suggestion(WebView::AutocompleteSuggestionSource::Search, text, relevance, is_verbatim, is_verbatim);
}

WebView::AutocompleteSuggestion history(StringView text, i32 relevance, bool can_be_automatically_selected = true)
{
    return suggestion(WebView::AutocompleteSuggestionSource::History, text, relevance, can_be_automatically_selected);
}

WebView::AutocompleteSuggestion bookmark(StringView text, i32 relevance, bool can_be_automatically_selected = true)
{
    auto result = suggestion(WebView::AutocompleteSuggestionSource::Bookmark, text, relevance, can_be_automatically_selected);
    result.match_class = WebView::AutocompleteMatchClass::URLPrefix;
    return result;
}

}

TEST_CASE(default_selection_is_independent_of_display_relevance)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "manual"sv,
        search("manual"sv, 1000, true),
        { history("https://example.com/manual"sv, 1200, false) },
        { search("manual pages"sv, 1300) },
        8);

    EXPECT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].text, "manual"sv);
    EXPECT(results[0].is_verbatim);
    EXPECT_EQ(results[1].text, "manual pages"sv);
}

TEST_CASE(strong_local_navigation_can_replace_an_ambiguous_default)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "lady"sv,
        search("lady"sv, 900, true),
        { history("https://ladybird.org/"sv, 930) },
        {},
        8);

    EXPECT_EQ(results[0].text, "https://ladybird.org/"sv);
    EXPECT_EQ(results[1].text, "lady"sv);
}

TEST_CASE(verbatim_action_remains_visible_when_history_fills_the_list)
{
    Vector<WebView::AutocompleteSuggestion> history_suggestions;
    for (size_t index = 0; index < 8; ++index)
        history_suggestions.append(history(MUST(String::formatted("https://{}.example/", index)), 1000 - static_cast<i32>(index), false));

    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        search("example"sv, 900, true),
        move(history_suggestions),
        {},
        5);

    EXPECT_EQ(results.size(), 5u);
    EXPECT(results.contains([](auto const& result) { return result.text == "example"sv; }));
}

TEST_CASE(source_caps_are_soft)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "example/"sv,
        {},
        {
            history("https://one.example/"sv, 1000),
            history("https://two.example/"sv, 990),
            history("https://three.example/"sv, 980),
            history("https://four.example/"sv, 970),
            history("https://five.example/"sv, 960),
        },
        {},
        5);

    EXPECT_EQ(results.size(), 5u);
}

TEST_CASE(origin_diversity_precedes_extra_pages_from_one_site)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "news"sv,
        {},
        {
            history("https://news.example/one"sv, 1000),
            history("https://news.example/two"sv, 990),
            history("https://news.example/three"sv, 980),
            history("https://other.example/news"sv, 970),
            history("https://third.example/news"sv, 960),
        },
        {},
        4);

    EXPECT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0].text, "https://news.example/one"sv);
    EXPECT_EQ(results[1].text, "https://news.example/two"sv);
    EXPECT_EQ(results[2].text, "https://other.example/news"sv);
    EXPECT_EQ(results[3].text, "https://third.example/news"sv);
}

TEST_CASE(origin_limit_does_not_refill_with_redundant_pages)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "news"sv,
        {},
        {
            history("https://news.example/one"sv, 1000),
            history("https://news.example/two"sv, 990),
            history("https://news.example/three"sv, 980),
            history("https://news.example/four"sv, 970),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
}

TEST_CASE(short_queries_use_one_result_per_origin_and_five_total)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "g"sv,
        search("g"sv, 1000, true),
        {
            history("https://github.com/"sv, 900),
            history("https://github.com/LadybirdBrowser/ladybird"sv, 890),
            history("https://google.com/"sv, 880),
            history("https://goodreads.com/"sv, 870),
            history("https://gitlab.com/"sv, 860),
            history("https://gnu.org/"sv, 850),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0].text, "g"sv);
    EXPECT(!results.contains([](auto const& result) {
        return result.text == "https://github.com/LadybirdBrowser/ladybird"sv;
    }));
}

TEST_CASE(short_queries_use_the_best_representative_for_an_origin)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "gi"sv,
        search("gi"sv, 900, true),
        {
            history("https://github.com/"sv, 1000),
            bookmark("https://github.com/LadybirdBrowser/ladybird"sv, 925),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].text, "https://github.com/LadybirdBrowser/ladybird"sv);
    EXPECT(!results.contains([](auto const& result) {
        return result.text == "https://github.com/"sv;
    }));
}

TEST_CASE(exact_short_bookmark_titles_survive_origin_collapsing)
{
    auto bookmark_suggestion = bookmark("https://example.com/projects/ladybird"sv, 900, false);
    bookmark_suggestion.title = "GH"_string;
    bookmark_suggestion.match_class = WebView::AutocompleteMatchClass::ExactTitle;

    auto results = WebView::mux_autocomplete_suggestions(
        "GH"sv,
        search("GH"sv, 1000, true),
        {
            history("https://example.com/"sv, 950),
            move(bookmark_suggestion),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://example.com/projects/ladybird"sv;
    }));
    EXPECT(!results.contains([](auto const& result) {
        return result.text == "https://example.com/"sv;
    }));
}

TEST_CASE(exact_remote_query_merges_into_the_verbatim_search)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "ladybird browser"sv,
        search("ladybird browser"sv, 1000, true),
        {},
        { search("Ladybird Browser"sv, 700) },
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT(results[0].is_verbatim);
    EXPECT(results[0].can_be_automatically_selected);
}

TEST_CASE(search_identity_folds_case_and_whitespace)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "Ladybird Browser"sv,
        search("  Ladybird   Browser  "sv, 1000, true),
        {},
        { search("ladybird browser"sv, 700) },
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].text, "  Ladybird   Browser  "sv);
    EXPECT(results[0].is_verbatim);
}

TEST_CASE(http_and_https_destinations_are_not_deduplicated)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "example.com"sv,
        {},
        {
            history("http://example.com/"sv, 1000),
            history("https://example.com/"sv, 990),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
}

TEST_CASE(history_and_bookmark_evidence_is_combined)
{
    auto history_suggestion = history("https://example.com/"sv, 900);
    history_suggestion.match_relevance = 850;
    history_suggestion.history_relevance = 50;
    auto bookmark_suggestion = suggestion(WebView::AutocompleteSuggestionSource::Bookmark, "https://example.com/"sv, 925, true);
    bookmark_suggestion.title = "Example bookmark"_string;
    bookmark_suggestion.match_relevance = 850;
    bookmark_suggestion.bookmark_relevance = 75;

    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        {},
        { move(history_suggestion), move(bookmark_suggestion) },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::Bookmark);
    EXPECT_EQ(results[0].title, Optional<String> { "Example bookmark"_string });
    EXPECT_EQ(results[0].relevance, 975);
}

TEST_CASE(www_and_bare_hosts_share_a_diversity_family)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        {},
        {
            history("https://example.com/one"sv, 1000),
            history("https://www.example.com/two"sv, 990),
            history("https://example.com/three"sv, 980),
            history("https://other.example/example"sv, 970),
            history("https://third.example/example"sv, 960),
        },
        {},
        4);

    EXPECT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0].text, "https://example.com/one"sv);
    EXPECT_EQ(results[1].text, "https://www.example.com/two"sv);
    EXPECT_EQ(results[2].text, "https://other.example/example"sv);
    EXPECT_EQ(results[3].text, "https://third.example/example"sv);
}

TEST_CASE(a_scheme_does_not_disable_origin_diversity)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "https://news"sv,
        {},
        {
            history("https://news.example/one"sv, 1000),
            history("https://news.example/two"sv, 990),
            history("https://news.example/three"sv, 980),
            history("https://other.example/news"sv, 970),
        },
        {},
        3);

    EXPECT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].text, "https://news.example/one"sv);
    EXPECT_EQ(results[1].text, "https://news.example/two"sv);
    EXPECT_EQ(results[2].text, "https://other.example/news"sv);
}

TEST_CASE(presentation_match_ranges_are_case_insensitive_and_merged)
{
    auto ranges = WebView::autocomplete_match_ranges("git hub"sv, "GitHub github"sv);

    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].length, 6u);
    EXPECT_EQ(ranges[1].start, 7u);
    EXPECT_EQ(ranges[1].length, 6u);
}

TEST_CASE(remote_suggestions_are_filtered_for_a_new_input)
{
    auto suggestions = WebView::filter_remote_autocomplete_suggestions("reddit soc"sv, {
                                                                                           "reddit soccer"_string,
                                                                                           "Reddit social"_string,
                                                                                           "reddit streams"_string,
                                                                                           "soccer"_string,
                                                                                       });

    EXPECT_EQ(suggestions.size(), 2u);
    EXPECT_EQ(suggestions[0], "reddit soccer"sv);
    EXPECT_EQ(suggestions[1], "Reddit social"sv);
}
