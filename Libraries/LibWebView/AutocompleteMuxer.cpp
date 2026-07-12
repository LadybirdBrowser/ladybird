/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <LibURL/Parser.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/URL.h>

namespace WebView {

static bool suggestions_have_same_destination(AutocompleteSuggestion const& left, AutocompleteSuggestion const& right)
{
    if (left.source == AutocompleteSuggestionSource::Search || right.source == AutocompleteSuggestionSource::Search)
        return left.source == right.source && left.text.equals_ignoring_case(right.text);

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
    auto relevance = max(existing.relevance, suggestion.relevance);
    if (match_relevance != 0 || history_relevance != 0 || bookmark_relevance != 0)
        relevance = max(relevance, match_relevance + history_relevance + bookmark_relevance);

    auto suggestion_has_preferred_presentation = suggestion.source == AutocompleteSuggestionSource::Bookmark
        || (existing.source == AutocompleteSuggestionSource::LiteralURL && suggestion.source == AutocompleteSuggestionSource::History);
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
}

static void append_or_merge(Vector<AutocompleteSuggestion>& suggestions, AutocompleteSuggestion suggestion)
{
    for (auto& existing : suggestions) {
        if (!suggestions_have_same_destination(existing, suggestion))
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
    return MUST(url->serialized_host().to_lowercase());
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

    quick_sort(candidates, suggestion_is_better);

    auto default_index = candidates.find_first_index_if([](auto const& suggestion) {
        return suggestion.can_be_automatically_selected;
    });
    if (default_index.has_value() && *default_index != 0) {
        auto default_suggestion = candidates.take(*default_index);
        candidates.insert(0, move(default_suggestion));
    }

    Vector<AutocompleteSuggestion> result;
    result.ensure_capacity(min(limit, candidates.size()));
    HashMap<String, size_t> origin_counts;
    Vector<size_t> deferred_indices;
    size_t local_navigation_count = 0;
    size_t remote_search_count = 0;
    auto query_contains_path = query.contains('/');
    auto verbatim_index = candidates.find_first_index_if([](auto const& candidate) { return candidate.is_verbatim; });

    for (size_t index = 0; index < candidates.size() && result.size() < limit; ++index) {
        auto const& candidate = candidates[index];
        auto origin = origin_family(candidate);
        auto exceeds_origin_limit = !query_contains_path
            && origin.has_value()
            && origin_counts.get(*origin).value_or(0) >= 2;
        auto exceeds_source_limit = (is_local_navigation(candidate) && local_navigation_count >= 4)
            || (is_remote_search(candidate) && remote_search_count >= 3);
        auto must_reserve_verbatim_slot = verbatim_index.has_value()
            && index < *verbatim_index
            && result.size() + 1 == limit;

        if (index != 0 && !candidate.is_verbatim && (exceeds_origin_limit || exceeds_source_limit || must_reserve_verbatim_slot)) {
            deferred_indices.append(index);
            continue;
        }

        if (origin.has_value())
            origin_counts.set(*origin, origin_counts.get(*origin).value_or(0) + 1);
        local_navigation_count += is_local_navigation(candidate) ? 1 : 0;
        remote_search_count += is_remote_search(candidate) ? 1 : 0;
        result.append(candidate);
    }

    for (auto index : deferred_indices) {
        if (result.size() >= limit)
            break;
        result.append(candidates[index]);
    }

    return result;
}

}
