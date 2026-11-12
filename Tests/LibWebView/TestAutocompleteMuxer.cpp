/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/OmniboxEngagement.h>
#include <LibWebView/SearchEngine.h>

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
        .favicon_png = {},
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

TEST_CASE(literal_url_remains_the_default_over_adaptive_results)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "mailto:hello@ladybird.org"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "mailto:hello@ladybird.org"sv, 900, true, true),
        { suggestion(WebView::AutocompleteSuggestionSource::Adaptive, "https://ladybird.org/"sv, 1400, true) },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].text, "mailto:hello@ladybird.org"sv);
    EXPECT(results[0].is_verbatim);
}

TEST_CASE(literal_url_and_search_remain_distinct_actions)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "steam://launch/1536610"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "steam://launch/1536610"sv, 900, true, true),
        {},
        { search("steam://launch/1536610"sv, 700) },
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::LiteralURL);
    EXPECT_EQ(results[1].source, WebView::AutocompleteSuggestionSource::Search);
}

TEST_CASE(literal_external_url_is_not_replaced_by_a_guessed_https_url)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "steam://launch/1536610"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "steam://launch/1536610"sv, 900, true, true),
        { history("https://steam://launch/1536610"sv, 1000) },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::LiteralURL);
    EXPECT_EQ(results[0].text, "steam://launch/1536610"sv);
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

TEST_CASE(addresses_differing_only_by_a_trailing_slash_show_as_one_row)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "exa"sv,
        {},
        {
            history("https://example.com/page/"sv, 1000),
            history("https://example.com/page"sv, 990),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].text, "https://example.com/page/"sv);
}

TEST_CASE(addresses_differing_only_by_tracking_parameters_show_as_one_row)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "goo"sv,
        {},
        {
            history("https://www.google.com/search?q=ladybird&zx=1731370000000"sv, 1000),
            history("https://www.google.com/search?q=ladybird&zx=1731370999999"sv, 990),
            history("https://www.google.com/search?utm_source=feed&q=ladybird"sv, 980),
            history("https://www.google.com/search?q=browser"sv, 970),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].text, "https://www.google.com/search?q=ladybird&zx=1731370000000"sv);
    EXPECT_EQ(results[1].text, "https://www.google.com/search?q=browser"sv);
}

TEST_CASE(a_row_for_addresses_differing_by_tracking_parameters_goes_to_the_one_without)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "exa"sv,
        {},
        {
            history("https://example.com/page?fbclid=1234"sv, 1000),
            history("https://example.com/page"sv, 990),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].text, "https://example.com/page"sv);
}

TEST_CASE(typed_address_keeps_its_text_when_merged_with_a_history_row_and_a_bookmark)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "example.com/page/?utm_source=feed"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "example.com/page/?utm_source=feed"sv, 900, true, true),
        { history("https://example.com/page"sv, 1000), bookmark("https://example.com/page"sv, 1000) },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::Bookmark);
    EXPECT_EQ(results[0].text, "https://example.com/page/?utm_source=feed"sv);
    EXPECT(results[0].is_verbatim);
}

static Optional<WebView::SearchEngine> duckduckgo()
{
    return WebView::builtin_search_engines().first_matching([](auto const& engine) { return engine.name == "DuckDuckGo"sv; }).copy();
}

TEST_CASE(a_results_page_of_the_search_engine_shows_as_its_search)
{
    Vector<WebView::AutocompleteSuggestion> local_rows {
        history("https://duckduckgo.com/?q=ladybird+browser&t=h_&ia=web"sv, 1000),
        history("https://duckduckgo.com/?q=ladybird%20browser"sv, 990),
    };
    WebView::show_search_results_pages_as_searches("lady"sv, local_rows, duckduckgo());

    auto results = WebView::mux_autocomplete_suggestions(
        "lady"sv,
        search("lady"sv, 900, true),
        move(local_rows),
        WebView::make_remote_autocomplete_suggestions({ "ladybird browser"_string }),
        8);

    // The two pages and the search engine's suggestion are one search, and Return still searches for the typed text.
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].text, "lady"sv);
    EXPECT_EQ(results[1].source, WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(results[1].text, "ladybird browser"sv);
    EXPECT(!results[1].can_be_automatically_selected);
}

TEST_CASE(a_results_page_that_is_more_than_the_search_stays_a_page)
{
    Vector<WebView::AutocompleteSuggestion> local_rows {
        history("https://duckduckgo.com/?q=ladybird&ia=images"sv, 1000),
        history("https://www.google.com/search?q=ladybird"sv, 990),
        history("https://duckduckgo.com/?q=ladybird#more"sv, 980),
    };
    WebView::show_search_results_pages_as_searches("lady"sv, local_rows, duckduckgo());
    for (auto const& row : local_rows)
        EXPECT_EQ(row.source, WebView::AutocompleteSuggestionSource::History);

    // The page was found by its address, not by what was searched for.
    Vector<WebView::AutocompleteSuggestion> found_by_address { history("https://duckduckgo.com/?q=ladybird"sv, 1000) };
    WebView::show_search_results_pages_as_searches("duck"sv, found_by_address, duckduckgo());
    EXPECT_EQ(found_by_address[0].source, WebView::AutocompleteSuggestionSource::History);
}

TEST_CASE(a_results_page_shows_as_its_search_only_for_a_query_that_starts_a_word)
{
    auto row_for = [](StringView query, StringView url) {
        Vector<WebView::AutocompleteSuggestion> rows { history(url, 1000) };
        WebView::show_search_results_pages_as_searches(query, rows, duckduckgo());
        return rows.take_first();
    };

    auto other_case = row_for("über"sv, "https://duckduckgo.com/?q=%C3%9Cber+alles"sv);
    EXPECT_EQ(other_case.source, WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(other_case.text, "Über alles"sv);
    EXPECT_EQ(row_for("ladybird"sv, "https://duckduckgo.com/?q=%22ladybird%22+browser"sv).source, WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(row_for("bersee"sv, "https://duckduckgo.com/?q=%C3%BCbersee"sv).source, WebView::AutocompleteSuggestionSource::History);
}

TEST_CASE(a_bookmark_without_a_favicon_or_title_keeps_those_of_the_history_row)
{
    auto visited = history("https://example.com/"sv, 900);
    visited.title = "Example"_string;
    visited.favicon_png = MUST(ByteBuffer::copy("favicon"sv.bytes()));

    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        {},
        { move(visited), bookmark("https://example.com/"sv, 925) },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::Bookmark);
    EXPECT_EQ(results[0].title, "Example"sv);
    EXPECT(results[0].favicon_png.has_value());
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

TEST_CASE(fragment_literal_is_not_replaced_by_fragmentless_history)
{
    auto const fragment_url = "https://example.com/page#target"sv;
    auto literal = suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, fragment_url, 900, true, true);
    auto adaptive = suggestion(WebView::AutocompleteSuggestionSource::Adaptive, "https://example.com/page"sv, 1050, true);

    auto results = WebView::mux_autocomplete_suggestions(
        fragment_url,
        move(literal),
        { move(adaptive) },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::LiteralURL);
    EXPECT_EQ(results[0].text, fragment_url);
    EXPECT(results[0].is_verbatim);
}

TEST_CASE(fragment_omnibox_destinations_are_preserved)
{
    EXPECT_EQ(
        WebView::normalize_omnibox_destination("https://example.com/page#section"sv, WebView::OmniboxDestinationKind::URL),
        "https://example.com/page#section"sv);
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

TEST_CASE(equivalent_origin_presentations_do_not_crowd_out_deep_pages)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "frag"sv,
        search("frag"sv, 900, true),
        {
            history("https://www.fragrantica.com/"sv, 1484),
            history("https://fragnado.com/"sv, 1009),
            history("https://fragonard.com/"sv, 1005),
            history("https://fragrantica.com/"sv, 934),
            history("https://www.fragrantica.com/perfume/Indult/Manakara-4345.html"sv, 746),
        },
        { search("fragrantica perfume"sv, 700) },
        8);

    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://www.fragrantica.com/"sv;
    }));
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://www.fragrantica.com/perfume/Indult/Manakara-4345.html"sv;
    }));
    EXPECT(!results.contains([](auto const& result) {
        return result.text == "https://fragrantica.com/"sv;
    }));
}

TEST_CASE(untitled_origin_presentation_does_not_bridge_distinct_titles)
{
    auto first_titled = history("https://example.com/"sv, 1000);
    first_titled.title = "First title"_string;
    auto untitled = history("https://www.example.com/"sv, 1100);
    auto second_titled = history("https://user@example.com/"sv, 900);
    second_titled.title = "Second title"_string;

    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        {},
        { move(first_titled), move(untitled), move(second_titled) },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://www.example.com/"sv;
    }));
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://user@example.com/"sv;
    }));
}

TEST_CASE(untitled_origin_group_adopts_a_later_title)
{
    auto untitled = history("https://www.example.com/"sv, 1100);
    auto first_titled = history("https://example.com/"sv, 1000);
    first_titled.title = "First title"_string;
    auto second_titled = history("https://user@example.com/"sv, 900);
    second_titled.title = "Second title"_string;

    auto results = WebView::mux_autocomplete_suggestions(
        "example"sv,
        {},
        { move(untitled), move(first_titled), move(second_titled) },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://www.example.com/"sv;
    }));
    EXPECT(results.contains([](auto const& result) {
        return result.text == "https://user@example.com/"sv;
    }));
}

TEST_CASE(origin_presentation_keeps_the_favicon_and_title_of_the_folded_row)
{
    auto learned = suggestion(WebView::AutocompleteSuggestionSource::Adaptive, "https://example.com/"sv, 1200, true);
    auto visited = history("https://www.example.com/"sv, 1000);
    visited.title = "Example"_string;
    visited.favicon_png = MUST(ByteBuffer::copy("favicon"sv.bytes()));

    auto results = WebView::mux_autocomplete_suggestions(
        "exam"sv,
        {},
        { move(learned), move(visited) },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].text, "https://example.com/"sv);
    EXPECT_EQ(results[0].title, "Example"sv);
    EXPECT(results[0].favicon_png.has_value());
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
    EXPECT(WebView::remote_autocomplete_suggestion_matches_input("reddit soccer"sv, "reddit soc"sv));
    EXPECT(WebView::remote_autocomplete_suggestion_matches_input("Reddit social"sv, "reddit soc"sv));
    EXPECT(!WebView::remote_autocomplete_suggestion_matches_input("reddit streams"sv, "reddit soc"sv));
    EXPECT(!WebView::remote_autocomplete_suggestion_matches_input("soccer"sv, "reddit soc"sv));
}

TEST_CASE(remote_web_addresses_are_not_searches)
{
    auto suggestions = WebView::make_remote_autocomplete_suggestions({
        "https://google.com/"_string,
        "http://example.com/page"_string,
        "google.com"_string,
        "google"_string,
        "https google"_string,
    });

    EXPECT_EQ(suggestions.size(), 5u);
    EXPECT(suggestions[0].source == WebView::AutocompleteSuggestionSource::RemoteURL);
    EXPECT_EQ(suggestions[0].text, "https://google.com/"sv);
    // Rows do not show the scheme, so an http address would look like the https site.
    EXPECT(suggestions[1].source == WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(suggestions[1].text, "http://example.com/page"sv);
    // An address without a scheme may be a phrase, so it is a search unless the user has been to the site.
    EXPECT(suggestions[2].source == WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(suggestions[2].text, "google.com"sv);
    EXPECT(suggestions[3].source == WebView::AutocompleteSuggestionSource::Search);
    EXPECT(suggestions[4].source == WebView::AutocompleteSuggestionSource::Search);
}

TEST_CASE(remote_web_address_without_a_scheme_is_a_page_for_a_visited_site)
{
    Vector<WebView::AutocompleteSuggestion> local_suggestions;
    local_suggestions.append(history("https://www.google.com/maps"sv, 800));
    local_suggestions.append(search("setup.py"sv, 900, true));

    auto suggestions = WebView::make_remote_autocomplete_suggestions({
                                                                         "google.com"_string,
                                                                         "setup.py"_string,
                                                                         "readme.md"_string,
                                                                     },
        local_suggestions);

    EXPECT_EQ(suggestions.size(), 3u);
    EXPECT(suggestions[0].source == WebView::AutocompleteSuggestionSource::RemoteURL);
    EXPECT_EQ(suggestions[0].text, "https://google.com/"sv);
    EXPECT(suggestions[1].source == WebView::AutocompleteSuggestionSource::Search);
    EXPECT(suggestions[2].source == WebView::AutocompleteSuggestionSource::Search);
}

TEST_CASE(remote_web_address_without_a_scheme_is_a_page_for_a_site_typed_before)
{
    for (auto typed_address : { "google.com"sv, "google.com:8443"sv }) {
        Vector<WebView::AutocompleteSuggestion> local_suggestions;
        local_suggestions.append(suggestion(WebView::AutocompleteSuggestionSource::Adaptive, typed_address, 800, false));

        auto suggestions = WebView::make_remote_autocomplete_suggestions({ "google.com"_string }, local_suggestions);

        EXPECT_EQ(suggestions.size(), 1u);
        EXPECT(suggestions[0].source == WebView::AutocompleteSuggestionSource::RemoteURL);
    }
}

TEST_CASE(remote_bare_web_address_waits_for_the_local_rows)
{
    // Before the local rows arrive, a bare address is left out, as it could still turn out to be a page. One for a site
    // among the rows so far is a page already, and the rest keep their rank.
    Vector<WebView::AutocompleteSuggestion> local_suggestions;
    local_suggestions.append(history("https://www.google.com/maps"sv, 800));
    auto suggestions = WebView::make_remote_autocomplete_suggestions({ "ladybird.org"_string, "google.com"_string, "google maps"_string },
        local_suggestions, WebView::LocalSuggestionsAreComplete::No);

    EXPECT_EQ(suggestions.size(), 2u);
    EXPECT(suggestions[0].source == WebView::AutocompleteSuggestionSource::RemoteURL);
    EXPECT_EQ(suggestions[0].text, "https://google.com/"sv);
    EXPECT_EQ(suggestions[0].relevance, 699);
    EXPECT(suggestions[1].source == WebView::AutocompleteSuggestionSource::Search);
    EXPECT_EQ(suggestions[1].text, "google maps"sv);
    EXPECT_EQ(suggestions[1].relevance, 698);
}

TEST_CASE(remote_web_address_merges_with_history_for_the_same_address)
{
    auto visited = history("https://google.com/"sv, 1000);
    visited.title = "Google"_string;
    visited.favicon_png = MUST(ByteBuffer::copy("favicon"sv.bytes()));

    auto results = WebView::mux_autocomplete_suggestions(
        "https://goo"sv,
        {},
        { move(visited) },
        WebView::make_remote_autocomplete_suggestions({ "https://google.com/"_string }),
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT(results[0].source == WebView::AutocompleteSuggestionSource::History);
    EXPECT_EQ(results[0].title, "Google"sv);
    EXPECT(results[0].favicon_png.has_value());
}

TEST_CASE(a_remote_address_before_a_local_row_for_the_same_page_shows_as_the_local_row)
{
    auto visited = history("https://example.com/"sv, 900, false);
    visited.title = "Example"_string;

    auto results = WebView::mux_autocomplete_suggestions(
        "exa"sv,
        {},
        { suggestion(WebView::AutocompleteSuggestionSource::RemoteURL, "https://example.com/"sv, 950, false), move(visited) },
        {},
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].source, WebView::AutocompleteSuggestionSource::History);
    EXPECT_EQ(results[0].title, "Example"sv);
}

TEST_CASE(remote_web_address_yields_to_a_local_presentation_of_its_origin)
{
    auto visited = history("https://www.google.com/"sv, 200, false);
    visited.favicon_png = MUST(ByteBuffer::copy("favicon"sv.bytes()));

    auto results = WebView::mux_autocomplete_suggestions(
        "https://goo"sv,
        {},
        { move(visited) },
        WebView::make_remote_autocomplete_suggestions({ "https://google.com/"_string }),
        8);

    // The local row cannot be the default, so it takes the rank of the remote address.
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].text, "https://www.google.com/"sv);
    EXPECT(results[0].favicon_png.has_value());
    EXPECT_EQ(results[0].relevance, 700);
}

TEST_CASE(a_short_query_keeps_a_local_row_over_a_remote_address_of_the_same_site)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "fo"sv,
        {},
        { history("https://foo.com/a"sv, 300, false) },
        WebView::make_remote_autocomplete_suggestions({ "https://foo.com/b"_string }),
        8);

    EXPECT(results.contains([](auto const& result) { return result.text == "https://foo.com/a"sv; }));
    EXPECT(!results.contains([](auto const& result) { return result.text == "https://foo.com/b"sv; }));
    // The remote address is for another page, so its rank is not the local row's.
    for (auto const& result : results) {
        if (result.text == "https://foo.com/a"sv)
            EXPECT_EQ(result.relevance, 300);
    }
}

TEST_CASE(remote_web_address_keeps_a_typed_address)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "https://google.com/"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "https://google.com/"sv, 900, true, true),
        {},
        WebView::make_remote_autocomplete_suggestions({ "https://google.com/"_string }),
        8);

    EXPECT_EQ(results.size(), 1u);
    EXPECT(results[0].source == WebView::AutocompleteSuggestionSource::LiteralURL);
    EXPECT(results[0].is_verbatim);
}

TEST_CASE(remote_suggestions_do_not_link_into_the_users_network)
{
    auto suggestions = WebView::make_remote_autocomplete_suggestions({
        "localhost:631"_string,
        "192.168.1.1"_string,
        "http://10.0.0.1/admin"_string,
        "http://router.local/"_string,
        "https://example.com:8443/"_string,
        "http://localhost/"_string,
    });

    for (auto const& suggestion : suggestions)
        EXPECT_EQ(suggestion.source, WebView::AutocompleteSuggestionSource::Search);
}

TEST_CASE(learned_address_with_a_port_is_one_row_with_its_page)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "local"sv,
        search("local"sv, 900, true),
        {
            suggestion(WebView::AutocompleteSuggestionSource::Adaptive, "localhost:3000"sv, 1100, true),
            history("http://localhost:3000/"sv, 1000),
        },
        {},
        8);

    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].text, "http://localhost:3000/"sv);
    EXPECT_EQ(results[1].text, "local"sv);
}

TEST_CASE(the_row_for_a_typed_address_is_not_folded_into_another_host)
{
    auto results = WebView::mux_autocomplete_suggestions(
        "example.com"sv,
        suggestion(WebView::AutocompleteSuggestionSource::LiteralURL, "example.com"sv, 900, true, true),
        { history("https://example.com/"sv, 1000), history("https://www.example.com/"sv, 1300) },
        {},
        8);

    // The typed address merges with the row for its page. That row goes to the host the user typed, so it is not folded
    // into the stronger row for www.example.com.
    EXPECT_EQ(results.size(), 2u);
    auto typed_row = results.first_matching([](auto const& result) { return result.is_verbatim; });
    EXPECT(typed_row.has_value() && typed_row->text == "https://example.com/"sv);
}

TEST_CASE(a_remote_address_does_not_choose_the_default_row)
{
    // The remote addresses rank above both local rows, but the default stays the stronger local row.
    auto results = WebView::mux_autocomplete_suggestions(
        "https://foo"sv,
        {},
        { history("https://foo.org/"sv, 600), history("https://www.foo.com/"sv, 200), history("https://foo.com/page"sv, 300) },
        WebView::make_remote_autocomplete_suggestions({ "https://foo.com/"_string, "https://foo.com/page"_string }),
        8);

    EXPECT(results.size() >= 3u);
    EXPECT_EQ(results[0].text, "https://foo.org/"sv);
    for (auto const& result : results) {
        if (result.text == "https://www.foo.com/"sv)
            EXPECT_EQ(result.relevance, 200);
        if (result.text == "https://foo.com/page"sv)
            EXPECT_EQ(result.relevance, 300);
    }
}

TEST_CASE(the_search_for_a_typed_address_does_not_take_a_remote_slot)
{
    auto typed_search = search("setup.py"sv, 850, false);
    typed_search.can_be_automatically_selected = false;
    Vector<WebView::AutocompleteSuggestion> local_suggestions;
    local_suggestions.append(move(typed_search));
    local_suggestions.append(history("https://a.example/setup.py"sv, 600, false));
    local_suggestions.append(history("https://b.example/setup.py"sv, 600, false));
    auto results = WebView::mux_autocomplete_suggestions(
        "setup.py"sv,
        WebView::make_verbatim_autocomplete_suggestion("setup.py"sv, WebView::classify_user_input("setup.py"sv), {}),
        move(local_suggestions),
        WebView::make_remote_autocomplete_suggestions({ "setup.py tutorial"_string, "setup.py install"_string, "setup.py develop"_string }),
        6);

    EXPECT_EQ(results.size(), 6u);
    EXPECT(results.contains([](auto const& result) { return result.text == "setup.py develop"sv; }));
}
