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

WebView::AutocompleteBookmark bookmark(StringView url, Optional<StringView> title = {}, Optional<StringView> folder = {})
{
    return {
        .url = MUST(String::from_utf8(url)),
        .title = title.map([](auto value) { return MUST(String::from_utf8(value)); }),
        .folder = folder.map([](auto value) { return MUST(String::from_utf8(value)); }),
        .favicon_base64_png = {},
    };
}

WebView::StoredOmniboxEngagement engagement(StringView input, WebView::OmniboxDestinationKind kind, StringView destination, u64 explicit_use_count, u64 default_use_count = 0)
{
    return {
        .normalized_input = MUST(String::from_utf8(input)),
        .destination_kind = kind,
        .destination = MUST(String::from_utf8(destination)),
        .explicit_use_count = explicit_use_count,
        .default_use_count = default_use_count,
        .last_used_time = UnixDateTime::from_seconds_since_epoch(now_seconds),
    };
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
                                             entry("https://example.com/often/reloaded"sv, "Example"sv, 1000, 0, 100),
                                         });

    EXPECT_EQ(suggestions.size(), 2u);
    auto origin = suggestions.find_if([](auto const& suggestion) {
        return suggestion.text == "https://example.com/"sv;
    });
    VERIFY(origin != suggestions.end());
    EXPECT(!origin->can_be_automatically_selected);
    EXPECT(!origin->can_be_inline_completed);

    auto deep_page = suggestions.find_if([](auto const& suggestion) {
        return suggestion.text == "https://example.com/often/reloaded"sv;
    });
    VERIFY(deep_page != suggestions.end());
    EXPECT(!deep_page->can_be_automatically_selected);
    EXPECT(!deep_page->can_be_inline_completed);
}

TEST_CASE(distinct_deep_pages_can_establish_origin_intent)
{
    auto suggestions = rank("exa"sv, {
                                         entry("https://example.com/one"sv, "One"sv, 1, 0, 1),
                                         entry("https://example.com/two"sv, "Two"sv, 1, 0, 1),
                                     });

    auto origin = suggestions.find_if([](auto const& suggestion) {
        return suggestion.text == "https://example.com/"sv;
    });
    VERIFY(origin != suggestions.end());
    EXPECT(origin->can_be_automatically_selected);
    EXPECT(origin->can_be_inline_completed);
    EXPECT_EQ(suggestions.first().text, "https://example.com/"sv);
}

TEST_CASE(aggregated_origin_uses_a_page_favicon)
{
    auto deep_page = entry("https://example.com/page"sv, "Page"sv, 1);
    deep_page.favicon_base64_png = "favicon"_string;
    auto suggestions = rank("exa"sv, { move(deep_page) });

    auto origin = suggestions.find_if([](auto const& suggestion) {
        return suggestion.text == "https://example.com/"sv;
    });
    VERIFY(origin != suggestions.end());
    VERIFY(origin->favicon_base64_png.has_value());
    EXPECT_EQ(*origin->favicon_base64_png, "favicon"sv);
}

TEST_CASE(short_queries_return_origins_instead_of_deep_pages)
{
    auto suggestions = rank("g"sv, {
                                       entry("https://github.com/LadybirdBrowser/ladybird/pulls"sv, "PRs"sv, 20),
                                       entry("https://www.google.com/search?q=pulls"sv, "pulls - Search with Google"sv, 20),
                                   });

    EXPECT_EQ(suggestions.size(), 2u);
    EXPECT(suggestions.contains([](auto const& suggestion) {
        return suggestion.text == "https://github.com/"sv;
    }));
    EXPECT(suggestions.contains([](auto const& suggestion) {
        return suggestion.text == "https://www.google.com/"sv;
    }));
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

TEST_CASE(history_does_not_append_query_strings)
{
    auto host_suggestions = rank("exa"sv, {
                                              entry("https://example.com/?session=secret"sv, "Example"sv, 10, 0, 10),
                                          });
    auto host_match = host_suggestions.find_if([](auto const& suggestion) {
        return suggestion.text == "https://example.com/?session=secret"sv;
    });
    VERIFY(host_match != host_suggestions.end());
    EXPECT(!host_match->can_be_automatically_selected);
    EXPECT(!host_match->can_be_inline_completed);

    auto query_suggestions = rank("example.com/?ses"sv, {
                                                            entry("https://example.com/?session=secret"sv, "Example"sv, 10, 0, 10),
                                                        });
    VERIFY(query_suggestions.size() == 1);
    EXPECT(query_suggestions[0].can_be_automatically_selected);
    EXPECT(!query_suggestions[0].can_be_inline_completed);
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

TEST_CASE(bookmark_url_prefix_is_strong_navigation_intent)
{
    auto suggestions = WebView::rank_bookmark_suggestions("lady"sv, {
                                                                        bookmark("https://ladybird.org/"sv, "Ladybird"sv),
                                                                    },
        8);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].source, WebView::AutocompleteSuggestionSource::Bookmark);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(bookmark_title_and_folder_matches_are_not_automatic)
{
    auto title_suggestions = WebView::rank_bookmark_suggestions("browser"sv, {
                                                                                 bookmark("https://example.com/"sv, "Browser project"sv),
                                                                             },
        8);
    auto folder_suggestions = WebView::rank_bookmark_suggestions("reading"sv, {
                                                                                  bookmark("https://example.com/"sv, "Example"sv, "Reading List"sv),
                                                                              },
        8);

    EXPECT_EQ(title_suggestions.size(), 1u);
    EXPECT(!title_suggestions[0].can_be_automatically_selected);
    EXPECT(!title_suggestions[0].can_be_inline_completed);
    EXPECT_EQ(folder_suggestions.size(), 1u);
    EXPECT(!folder_suggestions[0].can_be_automatically_selected);
    EXPECT(!folder_suggestions[0].can_be_inline_completed);
}

TEST_CASE(exact_short_bookmark_titles_are_matched_case_insensitively)
{
    auto suggestions = WebView::rank_bookmark_suggestions("GH"sv, {
                                                                      bookmark("https://example.com/projects/ladybird"sv, "gh"sv),
                                                                      bookmark("https://example.net/"sv, "Ghost"sv),
                                                                  },
        8);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].text, "https://example.com/projects/ladybird"sv);
    EXPECT_EQ(suggestions[0].match_class, WebView::AutocompleteMatchClass::ExactTitle);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(bookmark_does_not_append_a_query_string)
{
    auto suggestions = WebView::rank_bookmark_suggestions("exa"sv, {
                                                                       bookmark("https://example.com/?session=secret"sv, "Example"sv),
                                                                   },
        8);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(short_url_prefixes_keep_deep_bookmarks_eligible)
{
    auto suggestions = WebView::rank_bookmark_suggestions("gi"sv, {
                                                                      bookmark("https://github.com/LadybirdBrowser/ladybird"sv, "GH"sv),
                                                                      bookmark("https://google.com/"sv, "Google"sv),
                                                                  },
        8);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].text, "https://github.com/LadybirdBrowser/ladybird"sv);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(one_character_bookmark_prefix_is_not_automatic)
{
    auto suggestions = WebView::rank_bookmark_suggestions("g"sv, {
                                                                     bookmark("https://github.com/LadybirdBrowser/ladybird"sv, "GH"sv),
                                                                 },
        8);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(exact_adaptive_association_can_navigate_without_completing)
{
    auto suggestions = WebView::rank_engagement_suggestions("docs"sv, {
                                                                          engagement("docs"sv, WebView::OmniboxDestinationKind::URL, "https://example.com/manual/"sv, 1),
                                                                      },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].source, WebView::AutocompleteSuggestionSource::Adaptive);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}

TEST_CASE(adaptive_url_prefix_can_complete)
{
    auto suggestions = WebView::rank_engagement_suggestions("lady"sv, {
                                                                          engagement("ladybird"sv, WebView::OmniboxDestinationKind::URL, "https://ladybird.org/"sv, 2),
                                                                      },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].match_class, WebView::AutocompleteMatchClass::URLPrefix);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(query_string_completion_requires_an_exact_adaptive_association)
{
    auto suggestions = WebView::rank_engagement_suggestions("exa"sv, {
                                                                         engagement("exa"sv, WebView::OmniboxDestinationKind::URL, "https://example.com/?session=secret"sv, 2),
                                                                     },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(short_adaptive_prefixes_require_explicit_evidence)
{
    auto weak_suggestions = WebView::rank_engagement_suggestions("gi"sv, {
                                                                             engagement("git"sv, WebView::OmniboxDestinationKind::URL, "https://github.com/LadybirdBrowser/ladybird"sv, 0, 4),
                                                                         },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));
    auto strong_suggestions = WebView::rank_engagement_suggestions("gi"sv, {
                                                                               engagement("git"sv, WebView::OmniboxDestinationKind::URL, "https://github.com/LadybirdBrowser/ladybird"sv, 2),
                                                                           },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT(weak_suggestions.is_empty());
    EXPECT_EQ(strong_suggestions.size(), 1u);
    EXPECT(strong_suggestions[0].can_be_automatically_selected);
    EXPECT(strong_suggestions[0].can_be_inline_completed);
}

TEST_CASE(search_like_adaptive_url_requires_two_explicit_uses)
{
    auto weak = WebView::rank_engagement_suggestions("lady docs"sv, {
                                                                        engagement("lady docs"sv, WebView::OmniboxDestinationKind::URL, "https://ladybird.org/docs/"sv, 1),
                                                                    },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));
    auto strong = WebView::rank_engagement_suggestions("lady docs"sv, {
                                                                          engagement("lady docs"sv, WebView::OmniboxDestinationKind::URL, "https://ladybird.org/docs/"sv, 2),
                                                                      },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT_EQ(weak.size(), 1u);
    EXPECT(!weak[0].can_be_automatically_selected);
    EXPECT_EQ(strong.size(), 1u);
    EXPECT(strong[0].can_be_automatically_selected);
    EXPECT(!strong[0].can_be_inline_completed);
}

TEST_CASE(previous_searches_never_become_automatic_completions)
{
    auto suggestions = WebView::rank_engagement_suggestions("lady"sv, {
                                                                          engagement("ladybird browser"sv, WebView::OmniboxDestinationKind::Search, "ladybird browser"sv, 4),
                                                                      },
        8, UnixDateTime::from_seconds_since_epoch(now_seconds));

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].source, WebView::AutocompleteSuggestionSource::Search);
    EXPECT(!suggestions[0].can_be_automatically_selected);
    EXPECT(!suggestions[0].can_be_inline_completed);
}
