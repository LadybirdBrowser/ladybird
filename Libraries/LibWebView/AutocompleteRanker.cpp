/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <AK/HashMap.h>
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
    case AutocompleteMatchClass::ExactTitle:
        return 800;
    case AutocompleteMatchClass::URLPrefix:
        return 850;
    case AutocompleteMatchClass::TitlePrefix:
        return 650;
    case AutocompleteMatchClass::URLSubstring:
        return 450;
    case AutocompleteMatchClass::TitleSubstring:
        return 450;
    case AutocompleteMatchClass::AdaptiveExact:
        return 550;
    case AutocompleteMatchClass::AdaptivePrefix:
        return 350;
    case AutocompleteMatchClass::None:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

static double decayed_score_at(double score, UnixDateTime updated_at, UnixDateTime now, double half_life_days)
{
    auto age = now - updated_at;
    auto age_in_days = max(0.0, static_cast<double>(age.to_seconds()) / 86'400.0);
    return score * AK::pow(0.5, age_in_days / half_life_days);
}

static i32 page_quality(HistoryEntry const& entry, UnixDateTime now)
{
    auto age = now - entry.last_qualifying_visit_time;
    auto age_in_days = max(0.0, static_cast<double>(age.to_seconds()) / 86'400.0);
    auto decayed_visit_score = decayed_score_at(entry.decayed_visit_score, entry.score_updated_at, now, 30.0);
    auto decayed_direct_score = decayed_score_at(entry.decayed_direct_score, entry.score_updated_at, now, 60.0);
    auto visit_strength = 1.0 - AK::exp(-decayed_visit_score / 8.0);
    auto direct_strength = 1.0 - AK::exp(-decayed_direct_score / 3.0);
    auto recent_strength = AK::pow(0.5, age_in_days / 14.0);
    return static_cast<i32>(150.0 * direct_strength + 75.0 * visit_strength + 25.0 * recent_strength);
}

static bool query_contains_url_suffix(StringView query)
{
    return searchable_query(query).find_any_of("/?#"sv).has_value();
}

static bool url_has_non_origin_components(URL::URL const& url)
{
    return url.serialize_path() != "/"sv || url.query().has_value();
}

static Optional<String> origin_url_for_history_entry(HistoryEntry const& entry)
{
    auto parsed_url = URL::Parser::basic_parse(entry.url);
    if (!parsed_url.has_value() || !parsed_url->host().has_value())
        return {};
    if (parsed_url->scheme() != "http"sv && parsed_url->scheme() != "https"sv)
        return {};
    return MUST(String::formatted("{}/", parsed_url->origin().serialize()));
}

static void add_aggregated_origin_entries(StringView folded_query, Vector<HistoryEntry>& history_entries, UnixDateTime now)
{
    HashMap<String, HistoryEntry> origins;

    for (auto const& entry : history_entries) {
        auto origin_url = origin_url_for_history_entry(entry);
        if (!origin_url.has_value())
            continue;

        auto folded_origin_url = MUST(origin_url->to_casefold());
        auto match_class = classify_match(folded_query, folded_origin_url, {});
        if (match_class != AutocompleteMatchClass::ExactURL && match_class != AutocompleteMatchClass::URLPrefix)
            continue;

        auto& origin = origins.ensure(*origin_url, [&] {
            return HistoryEntry {
                .url = *origin_url,
                .title = {},
                .favicon_base64_png = {},
                .visit_count = 0,
                .direct_visit_count = 0,
                .last_visited_time = entry.last_visited_time,
                .last_qualifying_visit_time = entry.last_qualifying_visit_time,
                .last_direct_visit_time = entry.last_direct_visit_time,
                .decayed_visit_score = 0,
                .decayed_direct_score = 0,
                .score_updated_at = now,
            };
        });

        // A visit to the origin itself is strong origin evidence. Each deep page contributes at most
        // one direct visit and a small amount of page quality, so one frequently reloaded page cannot
        // claim the whole host.
        auto is_origin_entry = entry.url == *origin_url;
        auto visit_count_contribution = min(entry.visit_count, is_origin_entry ? 8u : 2u);
        auto direct_visit_count_contribution = min(entry.direct_visit_count, is_origin_entry ? 3u : 1u);
        origin.visit_count = min(100u, origin.visit_count + visit_count_contribution);
        origin.direct_visit_count = min(10u, origin.direct_visit_count + direct_visit_count_contribution);

        auto visit_score = decayed_score_at(entry.decayed_visit_score, entry.score_updated_at, now, 30.0);
        auto direct_score = decayed_score_at(entry.decayed_direct_score, entry.score_updated_at, now, 60.0);
        origin.decayed_visit_score = min(50.0, origin.decayed_visit_score + min(visit_score, is_origin_entry ? 8.0 : 2.0));
        origin.decayed_direct_score = min(12.0, origin.decayed_direct_score + min(direct_score, is_origin_entry ? 3.0 : 1.0));
        if (entry.favicon_base64_png.has_value()
            && (!origin.favicon_base64_png.has_value() || entry.last_visited_time >= origin.last_visited_time))
            origin.favicon_base64_png = entry.favicon_base64_png;
        origin.last_visited_time = max(origin.last_visited_time, entry.last_visited_time);
        origin.last_qualifying_visit_time = max(origin.last_qualifying_visit_time, entry.last_qualifying_visit_time);
        origin.last_direct_visit_time = max(origin.last_direct_visit_time, entry.last_direct_visit_time);
    }

    for (auto& origin : origins) {
        auto existing_origin = history_entries.find_if([&](auto const& entry) {
            return entry.url == origin.key;
        });
        if (existing_origin != history_entries.end()) {
            origin.value.title = move(existing_origin->title);
            origin.value.favicon_base64_png = move(existing_origin->favicon_base64_png);
            *existing_origin = move(origin.value);
        } else {
            history_entries.append(move(origin.value));
        }
    }
}

Vector<AutocompleteSuggestion> rank_history_suggestions(StringView query, Vector<HistoryEntry> history_entries, size_t limit, UnixDateTime now)
{
    auto query_string = MUST(String::from_utf8(query));
    auto folded_query = MUST(query_string.to_casefold());
    auto query_length = Utf8View { query }.length();
    auto query_is_url = location_looks_like_url(query);

    if (!query_contains_url_suffix(query))
        add_aggregated_origin_entries(folded_query, history_entries, now);

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

        auto adjusted_match_relevance = match_relevance(match_class);
        if (match_class == AutocompleteMatchClass::TitlePrefix)
            adjusted_match_relevance -= 100;
        else if (match_class == AutocompleteMatchClass::URLSubstring)
            adjusted_match_relevance -= 150;
        else if (match_class == AutocompleteMatchClass::TitleSubstring)
            adjusted_match_relevance -= 200;

        auto parsed_url = URL::Parser::basic_parse(entry.url);
        auto is_deep_page_without_path_input = parsed_url.has_value()
            && url_has_non_origin_components(*parsed_url)
            && !query_contains_url_suffix(query);
        if (query_length <= 2 && is_deep_page_without_path_input)
            continue;
        if (is_deep_page_without_path_input && match_class == AutocompleteMatchClass::URLPrefix)
            adjusted_match_relevance -= 200;

        auto history_relevance = page_quality(entry, now);
        auto relevance = adjusted_match_relevance + history_relevance;

        auto is_url_prefix = match_class == AutocompleteMatchClass::ExactURL || match_class == AutocompleteMatchClass::URLPrefix;
        auto has_strong_intent = entry.direct_visit_count >= (query_is_url ? 1u : 2u);
        auto can_be_automatically_selected = is_url_prefix
            && has_strong_intent
            && query_length >= 2
            && !is_deep_page_without_path_input;
        auto can_be_inline_completed = can_be_automatically_selected
            && match_class == AutocompleteMatchClass::URLPrefix
            && (!parsed_url.has_value() || !parsed_url->query().has_value())
            && autocomplete_url_can_complete(query, entry.url);

        suggestions.append({
            .source = AutocompleteSuggestionSource::History,
            .text = move(entry.url),
            .title = move(entry.title),
            .subtitle = {},
            .favicon_base64_png = move(entry.favicon_base64_png),
            .highlight_input = {},
            .match_class = match_class,
            .relevance = relevance,
            .match_relevance = adjusted_match_relevance,
            .history_relevance = history_relevance,
            .bookmark_relevance = 0,
            .adaptive_relevance = 0,
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

Vector<AutocompleteSuggestion> rank_bookmark_suggestions(StringView query, Vector<AutocompleteBookmark> const& bookmarks, size_t limit)
{
    if (query.is_empty())
        return {};

    auto query_string = MUST(String::from_utf8(query));
    auto folded_query = MUST(query_string.to_casefold());
    auto query_length = Utf8View { query }.length();

    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(bookmarks.size());

    for (auto const& bookmark : bookmarks) {
        auto folded_url = MUST(bookmark.url.to_casefold());
        auto folded_title = bookmark.title.map([](auto const& title) { return MUST(title.to_casefold()); });

        auto match_class = classify_match(folded_query, folded_url, {});
        auto title_match_class = AutocompleteMatchClass::None;
        if (folded_title.has_value() && *folded_title == folded_query)
            title_match_class = AutocompleteMatchClass::ExactTitle;
        else if (query_length >= 3 && folded_title.has_value())
            title_match_class = classify_match(folded_query, ""sv, folded_title->bytes_as_string_view());
        if (match_relevance(title_match_class) > match_relevance(match_class))
            match_class = title_match_class;

        Optional<String> folded_folder;
        if (query_length >= 3 && bookmark.folder.has_value())
            folded_folder = MUST(bookmark.folder->to_casefold());
        auto folder_match_class = folded_folder.has_value()
            ? classify_match(folded_query, ""sv, folded_folder->bytes_as_string_view())
            : AutocompleteMatchClass::None;
        if (match_relevance(folder_match_class) > match_relevance(match_class))
            match_class = folder_match_class;
        if (match_class == AutocompleteMatchClass::None)
            continue;

        auto adjusted_match_relevance = match_relevance(match_class);
        if (match_class == AutocompleteMatchClass::TitlePrefix)
            adjusted_match_relevance -= 100;
        else if (match_class == AutocompleteMatchClass::URLSubstring)
            adjusted_match_relevance -= 150;
        else if (match_class == AutocompleteMatchClass::TitleSubstring)
            adjusted_match_relevance -= 200;

        auto parsed_url = URL::Parser::basic_parse(bookmark.url);
        auto is_deep_page_without_path_input = parsed_url.has_value()
            && url_has_non_origin_components(*parsed_url)
            && !query_contains_url_suffix(query);
        if (query_length == 1 && is_deep_page_without_path_input && match_class == AutocompleteMatchClass::URLPrefix)
            adjusted_match_relevance -= 200;

        auto bookmark_relevance = 75;
        if (title_match_class == AutocompleteMatchClass::ExactTitle || title_match_class == AutocompleteMatchClass::TitlePrefix)
            bookmark_relevance += 25;

        auto is_url_prefix = match_class == AutocompleteMatchClass::ExactURL || match_class == AutocompleteMatchClass::URLPrefix;
        auto can_be_automatically_selected = is_url_prefix
            && query_length >= 2
            && (!parsed_url.has_value() || !parsed_url->query().has_value());
        auto can_be_inline_completed = can_be_automatically_selected
            && match_class == AutocompleteMatchClass::URLPrefix
            && (!parsed_url.has_value() || !parsed_url->query().has_value())
            && autocomplete_url_can_complete(query, bookmark.url);

        suggestions.append({
            .source = AutocompleteSuggestionSource::Bookmark,
            .text = bookmark.url,
            .title = bookmark.title,
            .subtitle = bookmark.folder,
            .favicon_base64_png = bookmark.favicon_base64_png,
            .highlight_input = {},
            .match_class = match_class,
            .relevance = adjusted_match_relevance + bookmark_relevance,
            .match_relevance = adjusted_match_relevance,
            .history_relevance = 0,
            .bookmark_relevance = bookmark_relevance,
            .adaptive_relevance = 0,
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

static bool query_contains_whitespace(StringView query)
{
    return any_of(query, [](auto code_unit) { return is_ascii_space(code_unit); });
}

Vector<AutocompleteSuggestion> rank_engagement_suggestions(StringView query, Vector<StoredOmniboxEngagement> engagements, size_t limit, UnixDateTime now)
{
    if (query.is_empty())
        return {};

    auto query_length = Utf8View { query }.length();
    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(engagements.size());

    for (auto& engagement : engagements) {
        auto normalized_query = normalize_omnibox_input(query, engagement.destination_kind);
        if (normalized_query.is_empty() || !engagement.normalized_input.starts_with_bytes(normalized_query))
            continue;

        auto current_length = Utf8View { normalized_query }.length();
        auto stored_length = Utf8View { engagement.normalized_input }.length();
        if (stored_length == 0 || current_length > stored_length)
            continue;

        auto exact_association = normalized_query == engagement.normalized_input;
        auto prefix_strength = AK::sqrt(static_cast<double>(current_length) / static_cast<double>(stored_length));
        auto weighted_uses = 2.0 * static_cast<double>(engagement.explicit_use_count) + static_cast<double>(engagement.default_use_count);
        auto use_strength = 1.0 - AK::exp(-weighted_uses / 2.0);
        auto age = now - engagement.last_used_time;
        auto age_in_days = max(0.0, static_cast<double>(age.to_seconds()) / 86'400.0);
        auto recency = AK::pow(0.5, age_in_days / 30.0);
        auto exact_bonus = exact_association ? 1.15 : 1.0;
        auto adaptive_relevance = static_cast<i32>(min(500.0, 500.0 * use_strength * prefix_strength * recency * exact_bonus));

        auto match_class = exact_association ? AutocompleteMatchClass::AdaptiveExact : AutocompleteMatchClass::AdaptivePrefix;
        auto adjusted_match_relevance = exact_association
            ? match_relevance(match_class)
            : static_cast<i32>(static_cast<double>(match_relevance(match_class)) * prefix_strength);
        auto source = AutocompleteSuggestionSource::Adaptive;
        bool can_be_automatically_selected = false;
        bool can_be_inline_completed = false;

        if (engagement.destination_kind == OmniboxDestinationKind::URL) {
            auto folded_query = MUST(MUST(String::from_utf8(query)).to_casefold());
            auto folded_destination = MUST(engagement.destination.to_casefold());
            auto url_match_class = classify_match(folded_query, folded_destination, {});
            if (url_match_class == AutocompleteMatchClass::ExactURL || url_match_class == AutocompleteMatchClass::URLPrefix) {
                match_class = url_match_class;
                adjusted_match_relevance = match_relevance(url_match_class);
            }

            auto parsed_url = URL::Parser::basic_parse(engagement.destination);
            auto is_deep_page_without_path_input = parsed_url.has_value()
                && url_has_non_origin_components(*parsed_url)
                && !query_contains_url_suffix(query);
            auto short_deep_prefix_explicit_threshold = query_length == 1 ? 3u : 2u;
            auto short_deep_prefix_has_evidence = query_length <= 2
                && engagement.explicit_use_count >= short_deep_prefix_explicit_threshold;
            if (query_length <= 2 && is_deep_page_without_path_input && !exact_association && !short_deep_prefix_has_evidence)
                continue;
            if (is_deep_page_without_path_input && match_class == AutocompleteMatchClass::URLPrefix
                && !exact_association && !short_deep_prefix_has_evidence)
                adjusted_match_relevance -= 200;
            auto syntactic_url_prefix = autocomplete_url_can_complete(query, engagement.destination);
            auto explicit_threshold = query_length == 1 ? 3u : query_contains_whitespace(query) ? 2u
                                                                                                : 1u;
            if (exact_association) {
                can_be_automatically_selected = engagement.explicit_use_count >= explicit_threshold
                    || (query_length >= 2 && weighted_uses >= 3.0);
            } else {
                can_be_automatically_selected = query_length >= 2
                    && syntactic_url_prefix
                    && (short_deep_prefix_has_evidence || weighted_uses >= 3.0);
            }

            auto deep_page_has_inline_intent = !is_deep_page_without_path_input
                || (exact_association && engagement.explicit_use_count >= 2)
                || short_deep_prefix_has_evidence;
            can_be_inline_completed = can_be_automatically_selected
                && syntactic_url_prefix
                && match_class == AutocompleteMatchClass::URLPrefix
                && deep_page_has_inline_intent;
        } else {
            source = AutocompleteSuggestionSource::Search;
        }

        suggestions.append({
            .source = source,
            .text = move(engagement.destination),
            .title = {},
            .subtitle = {},
            .favicon_base64_png = {},
            .highlight_input = {},
            .match_class = match_class,
            .relevance = adjusted_match_relevance + adaptive_relevance,
            .match_relevance = adjusted_match_relevance,
            .history_relevance = 0,
            .bookmark_relevance = 0,
            .adaptive_relevance = adaptive_relevance,
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
