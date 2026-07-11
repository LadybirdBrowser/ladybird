/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/URL.h>

namespace WebView {

static StringView url_without_scheme(StringView url)
{
    auto scheme_separator = url.find("://"sv);
    if (!scheme_separator.has_value())
        return url;
    return url.substring_view(*scheme_separator + 3);
}

static StringView searchable_url(StringView url)
{
    auto searchable = url_without_scheme(url);
    if (searchable.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        searchable = searchable.substring_view(4);
    if (searchable.ends_with('/') && searchable.find('/') == searchable.length() - 1)
        searchable = searchable.substring_view(0, searchable.length() - 1);
    return searchable;
}

static StringView searchable_query(StringView query)
{
    auto searchable = url_without_scheme(query);
    if (searchable.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        searchable = searchable.substring_view(4);
    return searchable;
}

static bool contains_word_prefix(StringView text, StringView token)
{
    size_t offset = 0;
    while (true) {
        auto match = text.find(token, offset);
        if (!match.has_value())
            return false;
        if (*match == 0 || !is_ascii_alphanumeric(text[*match - 1]))
            return true;
        offset = *match + 1;
    }
}

static bool all_tokens_match(StringView query, auto callback)
{
    bool saw_token = false;
    for (auto token : query.split_view_if([](char ch) { return is_ascii_space(ch); })) {
        if (token.is_empty())
            continue;
        saw_token = true;
        if (!callback(token))
            return false;
    }
    return saw_token;
}

static AutocompleteMatchClass classify_match(StringView folded_query, StringView folded_url, Optional<StringView> folded_title)
{
    auto query_url = searchable_query(folded_query);
    auto url = searchable_url(folded_url);

    if (url == query_url)
        return AutocompleteMatchClass::ExactURL;
    if (url.starts_with(query_url))
        return AutocompleteMatchClass::URLPrefix;

    if (folded_title.has_value() && all_tokens_match(folded_query, [&](auto token) { return contains_word_prefix(*folded_title, token); }))
        return AutocompleteMatchClass::TitlePrefix;
    if (all_tokens_match(folded_query, [&](auto token) { return url.contains(token); }))
        return AutocompleteMatchClass::URLSubstring;
    if (folded_title.has_value() && all_tokens_match(folded_query, [&](auto token) { return folded_title->contains(token); }))
        return AutocompleteMatchClass::TitleSubstring;
    return AutocompleteMatchClass::None;
}

static i32 match_relevance(AutocompleteMatchClass match_class)
{
    switch (match_class) {
    case AutocompleteMatchClass::ExactURL:
        return 1000;
    case AutocompleteMatchClass::URLPrefix:
        return 850;
    case AutocompleteMatchClass::TitlePrefix:
        return 650;
    case AutocompleteMatchClass::URLSubstring:
        return 450;
    case AutocompleteMatchClass::TitleSubstring:
        return 450;
    case AutocompleteMatchClass::None:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

static i32 page_quality(HistoryEntry const& entry, UnixDateTime now)
{
    auto age = now - entry.last_qualifying_visit_time;
    auto age_in_days = max(0.0, static_cast<double>(age.to_seconds()) / 86'400.0);
    auto score_age = now - entry.score_updated_at;
    auto score_age_in_days = max(0.0, static_cast<double>(score_age.to_seconds()) / 86'400.0);
    auto decayed_visit_score = entry.decayed_visit_score * AK::pow(0.5, score_age_in_days / 30.0);
    auto decayed_direct_score = entry.decayed_direct_score * AK::pow(0.5, score_age_in_days / 60.0);
    auto visit_strength = 1.0 - AK::exp(-decayed_visit_score / 8.0);
    auto direct_strength = 1.0 - AK::exp(-decayed_direct_score / 3.0);
    auto recent_strength = AK::pow(0.5, age_in_days / 14.0);
    return static_cast<i32>(150.0 * direct_strength + 75.0 * visit_strength + 25.0 * recent_strength);
}

static bool query_contains_path(StringView query)
{
    return searchable_query(query).contains('/');
}

Vector<AutocompleteSuggestion> rank_history_suggestions(StringView query, Vector<HistoryEntry> history_entries, size_t limit, UnixDateTime now)
{
    auto query_string = MUST(String::from_utf8(query));
    auto folded_query = MUST(query_string.to_casefold());
    auto query_length = Utf8View { query }.length();
    auto query_is_url = location_looks_like_url(query);

    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(history_entries.size());

    for (auto& entry : history_entries) {
        auto folded_url = MUST(entry.url.to_casefold());
        Optional<String> folded_title;
        if (entry.title.has_value())
            folded_title = MUST(entry.title->to_casefold());

        auto match_class = classify_match(
            folded_query,
            folded_url,
            folded_title.map([](auto const& title) { return title.bytes_as_string_view(); }));
        if (match_class == AutocompleteMatchClass::None)
            continue;

        auto relevance = match_relevance(match_class) + page_quality(entry, now);
        if (match_class == AutocompleteMatchClass::TitlePrefix)
            relevance -= 100;
        else if (match_class == AutocompleteMatchClass::URLSubstring)
            relevance -= 150;
        else if (match_class == AutocompleteMatchClass::TitleSubstring)
            relevance -= 200;

        auto parsed_url = URL::Parser::basic_parse(entry.url);
        auto is_deep_page_without_path_input = parsed_url.has_value()
            && parsed_url->serialize_path() != "/"sv
            && !query_contains_path(query);
        if (is_deep_page_without_path_input && match_class == AutocompleteMatchClass::URLPrefix)
            relevance -= 200;

        auto is_url_prefix = match_class == AutocompleteMatchClass::ExactURL || match_class == AutocompleteMatchClass::URLPrefix;
        auto has_strong_intent = entry.direct_visit_count >= (query_is_url ? 1u : 2u);
        auto can_be_automatically_selected = is_url_prefix
            && has_strong_intent
            && query_length >= 2
            && !is_deep_page_without_path_input;
        auto can_be_inline_completed = can_be_automatically_selected
            && match_class == AutocompleteMatchClass::URLPrefix
            && autocomplete_url_can_complete(query, entry.url);

        suggestions.append({
            .source = AutocompleteSuggestionSource::History,
            .section = AutocompleteSuggestionSection::History,
            .text = move(entry.url),
            .title = move(entry.title),
            .subtitle = {},
            .favicon_base64_png = move(entry.favicon_base64_png),
            .match_class = match_class,
            .relevance = relevance,
            .is_verbatim = false,
            .can_be_automatically_selected = can_be_automatically_selected,
            .can_be_inline_completed = can_be_inline_completed,
        });
    }

    quick_sort(suggestions, [](auto const& left, auto const& right) {
        if (left.relevance != right.relevance)
            return left.relevance > right.relevance;
        if (left.match_class != right.match_class)
            return static_cast<u8>(left.match_class) < static_cast<u8>(right.match_class);
        return left.text < right.text;
    });

    if (suggestions.size() > limit)
        suggestions.resize(limit);
    return suggestions;
}

}
