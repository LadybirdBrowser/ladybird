/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AutocompleteRanker.h>

namespace {

constexpr i64 now_seconds = 2'000'000;

WebView::HistoryEntry entry(StringView url, Optional<StringView> title, u64 visit_count, i64 age_in_days = 0, u64 direct_visit_count = 2)
{
    auto last_visited_time = UnixDateTime::from_seconds_since_epoch(now_seconds - age_in_days * 86'400);
    return {
        .url = MUST(String::from_utf8(url)),
        .title = title.map([](auto value) { return MUST(String::from_utf8(value)); }),
        .favicon_base64_png = {},
        .visit_count = visit_count,
        .direct_visit_count = direct_visit_count,
        .last_visited_time = last_visited_time,
        .last_qualifying_visit_time = last_visited_time,
        .last_direct_visit_time = last_visited_time,
        .decayed_visit_score = static_cast<double>(visit_count),
        .decayed_direct_score = static_cast<double>(direct_visit_count),
        .score_updated_at = last_visited_time,
    };
}

Vector<WebView::AutocompleteSuggestion> rank(StringView query, Vector<WebView::HistoryEntry> entries)
{
    return WebView::rank_history_suggestions(query, move(entries), 8, UnixDateTime::from_seconds_since_epoch(now_seconds));
}

}

TEST_CASE(url_prefix_is_ranked_and_can_complete)
{
    auto suggestions = rank("lad"sv, {
                                         entry("https://ladybird.org/"sv, "Ladybird"sv, 4),
                                         entry("https://example.com/ladybird"sv, "Ladybird notes"sv, 20),
                                     });

    EXPECT_EQ(suggestions.size(), 2u);
    EXPECT_EQ(suggestions[0].text, "https://ladybird.org/"sv);
    EXPECT_EQ(suggestions[0].match_class, WebView::AutocompleteMatchClass::URLPrefix);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(title_match_never_becomes_default_or_completion)
{
    auto suggestions = rank("manual"sv, {
                                            entry("https://example.com/guide"sv, "Manual for Ladybird"sv, 100),
                                        });

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].match_class, WebView::AutocompleteMatchClass::TitlePrefix);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(passive_visit_frequency_does_not_become_navigation_intent)
{
    auto suggestions = rank("example"sv, {
                                             entry("https://example.com/"sv, "Example"sv, 1000, 0, 0),
                                         });

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(deep_page_does_not_own_an_origin_prefix)
{
    auto suggestions = rank("example"sv, {
                                             entry("https://example.com/often/reloaded"sv, "Example"sv, 1000),
                                         });

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(typed_path_allows_a_deep_page_completion)
{
    auto suggestions = rank("example.com/ma"sv, {
                                                    entry("https://example.com/manual/"sv, "Manual"sv, 2),
                                                });

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(title_matching_uses_unicode_case_folding)
{
    auto suggestions = rank("strasse"sv, {
                                             entry("https://example.com/"sv, "Stra\xC3\x9F"
                                                                             "e guide"sv,
                                                 2),
                                         });

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].match_class, WebView::AutocompleteMatchClass::TitlePrefix);
}
