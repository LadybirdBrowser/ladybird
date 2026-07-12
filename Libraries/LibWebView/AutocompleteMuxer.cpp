/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/URL.h>

namespace WebView {

bool autocomplete_suggestions_have_same_destination(AutocompleteSuggestion const& left, AutocompleteSuggestion const& right)
{
    if (left.source == AutocompleteSuggestionSource::Search || right.source == AutocompleteSuggestionSource::Search) {
        return left.source == right.source
            && normalize_omnibox_destination(left.text, OmniboxDestinationKind::Search) == normalize_omnibox_destination(right.text, OmniboxDestinationKind::Search);
    }

    return autocomplete_urls_match(left.text, right.text);
}

static void merge_suggestion(AutocompleteSuggestion& existing, AutocompleteSuggestion suggestion)
{
    auto is_verbatim = existing.is_verbatim || suggestion.is_verbatim;
    auto can_be_automatically_selected = existing.can_be_automatically_selected || suggestion.can_be_automatically_selected;
    auto can_be_inline_completed = existing.can_be_inline_completed || suggestion.can_be_inline_completed;
    auto match_relevance = max(existing.match_relevance, suggestion.match_relevance);
    auto history_relevance = max(existing.history_relevance, suggestion.history_relevance);
    auto bookmark_relevance = max(existing.bookmark_relevance, suggestion.bookmark_relevance);
    auto adaptive_relevance = max(existing.adaptive_relevance, suggestion.adaptive_relevance);
    auto relevance = max(existing.relevance, suggestion.relevance);
    if (match_relevance != 0 || history_relevance != 0 || bookmark_relevance != 0 || adaptive_relevance != 0)
        relevance = max(relevance, match_relevance + history_relevance + bookmark_relevance + adaptive_relevance);

    auto suggestion_has_preferred_presentation = suggestion.source == AutocompleteSuggestionSource::Bookmark
        || (existing.source == AutocompleteSuggestionSource::Adaptive && suggestion.source == AutocompleteSuggestionSource::History)
        || (existing.source == AutocompleteSuggestionSource::LiteralURL && suggestion.source != AutocompleteSuggestionSource::LiteralURL);
    if (suggestion_has_preferred_presentation) {
        existing = move(suggestion);
    } else {
        if (!existing.title.has_value() && suggestion.title.has_value())
            existing.title = move(suggestion.title);
        if (!existing.subtitle.has_value() && suggestion.subtitle.has_value())
            existing.subtitle = move(suggestion.subtitle);
        if (!existing.favicon_base64_png.has_value() && suggestion.favicon_base64_png.has_value())
            existing.favicon_base64_png = move(suggestion.favicon_base64_png);
    }

    existing.is_verbatim = is_verbatim;
    existing.can_be_automatically_selected = can_be_automatically_selected;
    existing.can_be_inline_completed = can_be_inline_completed;
    existing.relevance = relevance;
    existing.match_relevance = match_relevance;
    existing.history_relevance = history_relevance;
    existing.bookmark_relevance = bookmark_relevance;
    existing.adaptive_relevance = adaptive_relevance;
}

static void append_or_merge(Vector<AutocompleteSuggestion>& suggestions, AutocompleteSuggestion suggestion)
{
    for (auto& existing : suggestions) {
        if (!autocomplete_suggestions_have_same_destination(existing, suggestion))
            continue;
        merge_suggestion(existing, move(suggestion));
        return;
    }
    suggestions.append(move(suggestion));
}

static Optional<String> origin_family(AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == AutocompleteSuggestionSource::Search)
        return {};
    auto url = URL::Parser::basic_parse(suggestion.text);
    if (!url.has_value() || !url->host().has_value())
        return {};
    auto host = MUST(url->serialized_host().to_lowercase());
    if (host.bytes_as_string_view().starts_with("www."sv))
        return MUST(String::from_utf8(host.bytes_as_string_view().substring_view(4)));
    return host;
}

static bool is_local_navigation(AutocompleteSuggestion const& suggestion)
{
    return suggestion.source != AutocompleteSuggestionSource::Search;
}

static bool is_remote_search(AutocompleteSuggestion const& suggestion)
{
    return suggestion.source == AutocompleteSuggestionSource::Search && !suggestion.is_verbatim;
}

static bool suggestion_is_better(AutocompleteSuggestion const& left, AutocompleteSuggestion const& right)
{
    if (left.relevance != right.relevance)
        return left.relevance > right.relevance;
    if (left.is_verbatim != right.is_verbatim)
        return left.is_verbatim;
    return left.text < right.text;
}

static bool is_origin_navigation(AutocompleteSuggestion const& suggestion)
{
    auto url = URL::Parser::basic_parse(suggestion.text);
    return url.has_value()
        && url->host().has_value()
        && url->serialize_path() == "/"sv
        && !url->query().has_value();
}

static u8 short_query_origin_preference(AutocompleteSuggestion const& suggestion, size_t query_length)
{
    if (suggestion.adaptive_relevance > 0 && suggestion.can_be_automatically_selected)
        return 3;
    if (suggestion.source == AutocompleteSuggestionSource::Bookmark
        && suggestion.match_class == AutocompleteMatchClass::ExactTitle)
        return 2;
    if (query_length >= 2
        && suggestion.source == AutocompleteSuggestionSource::Bookmark
        && suggestion.match_class == AutocompleteMatchClass::URLPrefix
        && suggestion.can_be_automatically_selected)
        return 2;
    if (is_origin_navigation(suggestion))
        return 1;
    return 0;
}

static bool query_contains_url_suffix(StringView query)
{
    if (auto scheme_end = query.find("://"sv); scheme_end.has_value())
        query = query.substring_view(*scheme_end + 3);
    return query.find_any_of("/?#"sv).has_value();
}

Vector<AutocompleteSuggestion> mux_autocomplete_suggestions(
    StringView query,
    Optional<AutocompleteSuggestion> verbatim_suggestion,
    Vector<AutocompleteSuggestion> local_suggestions,
    Vector<AutocompleteSuggestion> remote_suggestions,
    size_t limit)
{
    if (limit == 0)
        return {};

    Vector<AutocompleteSuggestion> candidates;
    candidates.ensure_capacity(local_suggestions.size() + remote_suggestions.size() + (verbatim_suggestion.has_value() ? 1 : 0));
    if (verbatim_suggestion.has_value())
        append_or_merge(candidates, verbatim_suggestion.release_value());
    for (auto& suggestion : local_suggestions)
        append_or_merge(candidates, move(suggestion));
    for (auto& suggestion : remote_suggestions)
        append_or_merge(candidates, move(suggestion));

    auto query_contains_path = query_contains_url_suffix(query);
    auto query_length = Utf8View { query }.length();
    auto is_short_host_query = !query_contains_path && query_length <= 2;

    if (is_short_host_query) {
        Vector<AutocompleteSuggestion> representatives;
        representatives.ensure_capacity(candidates.size());
        HashMap<String, size_t> origin_indices;

        for (auto& candidate : candidates) {
            auto origin = origin_family(candidate);
            if (!origin.has_value()) {
                representatives.append(move(candidate));
                continue;
            }

            auto existing_index = origin_indices.get(*origin);
            if (!existing_index.has_value()) {
                origin_indices.set(*origin, representatives.size());
                representatives.append(move(candidate));
                continue;
            }

            auto& existing = representatives[*existing_index];
            auto candidate_preference = short_query_origin_preference(candidate, query_length);
            auto existing_preference = short_query_origin_preference(existing, query_length);
            if (candidate_preference > existing_preference
                || (candidate_preference == existing_preference && suggestion_is_better(candidate, existing)))
                existing = move(candidate);
        }

        candidates = move(representatives);
    }

    quick_sort(candidates, suggestion_is_better);

    auto default_index = candidates.find_first_index_if([](auto const& suggestion) {
        return suggestion.can_be_automatically_selected;
    });
    if (default_index.has_value() && *default_index != 0) {
        auto default_suggestion = candidates.take(*default_index);
        candidates.insert(0, move(default_suggestion));
    }

    Vector<AutocompleteSuggestion> result;
    auto result_limit = is_short_host_query ? min(limit, 5uz) : limit;
    auto origin_limit = is_short_host_query ? 1uz : 2uz;
    result.ensure_capacity(min(result_limit, candidates.size()));
    HashMap<String, size_t> origin_counts;
    Vector<size_t> deferred_source_indices;
    size_t local_navigation_count = 0;
    size_t remote_search_count = 0;
    auto verbatim_index = candidates.find_first_index_if([](auto const& candidate) { return candidate.is_verbatim; });

    for (size_t index = 0; index < candidates.size() && result.size() < result_limit; ++index) {
        auto const& candidate = candidates[index];
        auto origin = origin_family(candidate);
        auto exceeds_origin_limit = !query_contains_path
            && origin.has_value()
            && origin_counts.get(*origin).value_or(0) >= origin_limit;
        auto exceeds_source_limit = (is_local_navigation(candidate) && local_navigation_count >= 4)
            || (is_remote_search(candidate) && remote_search_count >= 3);
        auto must_reserve_verbatim_slot = verbatim_index.has_value()
            && index < *verbatim_index
            && result.size() + 1 == result_limit;

        if (index != 0 && !candidate.is_verbatim && (exceeds_origin_limit || must_reserve_verbatim_slot))
            continue;
        if (index != 0 && !candidate.is_verbatim && exceeds_source_limit) {
            deferred_source_indices.append(index);
            continue;
        }

        if (origin.has_value())
            origin_counts.set(*origin, origin_counts.get(*origin).value_or(0) + 1);
        local_navigation_count += is_local_navigation(candidate) ? 1 : 0;
        remote_search_count += is_remote_search(candidate) ? 1 : 0;
        result.append(candidate);
    }

    for (auto index : deferred_source_indices) {
        if (result.size() >= result_limit)
            break;
        result.append(candidates[index]);
    }

    return result;
}

}
