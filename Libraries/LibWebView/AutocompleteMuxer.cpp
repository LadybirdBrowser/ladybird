/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/URL.h>

namespace WebView {

namespace {

// OPTIMIZATION: Rows are compared with each other row, so where each row goes is worked out once, rather than for each
//               pair of rows.
struct SuggestionDestination {
    bool is_search { false };
    // A row whose text goes nowhere has none, and goes where no other row goes.
    Optional<String> key;
    // The address the key was made from, for a row that goes to one.
    Optional<String> serialized_url;
    bool has_tracking_query_parameters { false };
};

struct QueryParameter {
    StringView name;
    StringView value;
};

}

// Query parameters that only say where a visit came from, or keep a cache from answering, and never change the page.
static bool is_tracking_query_parameter(StringView name)
{
    static constexpr Array tracking_parameter_names {
        "_ga"sv, "_gl"sv, "dclid"sv, "fbclid"sv, "gbraid"sv, "gclid"sv, "igshid"sv, "mc_cid"sv, "mc_eid"sv, "msclkid"sv,
        "wbraid"sv, "yclid"sv, "zx"sv
    };
    if (name.starts_with("utm_"sv, CaseSensitivity::CaseInsensitive))
        return true;
    return any_of(tracking_parameter_names, [&](auto tracking_name) { return name.equals_ignoring_ascii_case(tracking_name); });
}

static QueryParameter split_query_parameter(StringView parameter)
{
    auto equals = parameter.find('=');
    if (!equals.has_value())
        return { parameter, {} };
    return { parameter.substring_view(0, *equals), parameter.substring_view(*equals + 1) };
}

// Query parameters that search engines add to their results pages, which say how the search was made but do not change
// what it finds.
static bool is_search_bookkeeping_query_parameter(StringView name, StringView value)
{
    static constexpr Array bookkeeping_parameter_names {
        "aqs"sv, "atb"sv, "bih"sv, "biw"sv, "client"sv, "cvid"sv, "dpr"sv, "ei"sv, "form"sv, "ghc"sv, "gs_lcrp"sv,
        "gs_lp"sv, "ie"sv, "iflsig"sv, "oe"sv, "oq"sv, "pq"sv, "qs"sv, "rlz"sv, "sa"sv, "sc"sv, "sca_esv"sv, "sca_upv"sv,
        "sclient"sv, "sk"sv, "source"sv, "sourceid"sv, "sxsrf"sv, "t"sv, "uact"sv, "ved"sv
    };
    // NB: These name the results' tab. Only the tab of web results is the plain search.
    if (name.equals_ignoring_ascii_case("ia"sv) || name.equals_ignoring_ascii_case("cat"sv))
        return value.equals_ignoring_ascii_case("web"sv);
    return any_of(bookkeeping_parameter_names, [&](auto bookkeeping_name) { return name.equals_ignoring_ascii_case(bookkeeping_name); });
}

static Optional<String> decode_query_parameter_value(StringView value)
{
    auto plus_as_space = ByteString { value }.replace("+"sv, " "sv, ReplaceMode::All);
    auto decoded = URL::percent_decode(plus_as_space);
    auto result = String::from_byte_string(decoded);
    if (result.is_error())
        return {};
    return result.release_value();
}

// The search terms of a page of the search engine's results, when the page shows nothing but the results for them.
static Optional<String> search_terms_of_results_page(URL::URL const& url, URL::URL const& results_url)
{
    if (!results_url.query().has_value() || url.fragment().has_value() || !url.query().has_value())
        return {};
    if (url.scheme() != results_url.scheme() || url.port() != results_url.port() || url.serialize_path() != results_url.serialize_path())
        return {};
    if (autocomplete_host_family(url) != autocomplete_host_family(results_url))
        return {};

    Optional<StringView> terms_parameter_name;
    Vector<StringView> fixed_parameters;
    for (auto parameter : results_url.query()->split_view('&')) {
        auto [name, value] = split_query_parameter(parameter);
        if (value == "%s"sv)
            terms_parameter_name = name;
        // NB: A parameter that is ignored on a results page, such as which client made the search, is not required of
        //     one either.
        else if (!is_tracking_query_parameter(name) && !is_search_bookkeeping_query_parameter(name, value))
            fixed_parameters.append(parameter);
    }
    if (!terms_parameter_name.has_value())
        return {};

    // NB: Each fixed parameter is crossed off once, so that a repeated one cannot stand in for one that is missing.
    Optional<String> terms;
    for (auto parameter : url.query()->split_view('&')) {
        auto [name, value] = split_query_parameter(parameter);
        if (name == *terms_parameter_name) {
            if (terms.has_value())
                return {};
            terms = decode_query_parameter_value(value);
            if (!terms.has_value())
                return {};
        } else if (auto fixed_index = fixed_parameters.find_first_index(parameter); fixed_index.has_value()) {
            fixed_parameters.remove(*fixed_index);
        } else if (!is_tracking_query_parameter(name) && !is_search_bookkeeping_query_parameter(name, value)) {
            return {};
        }
    }
    if (!fixed_parameters.is_empty() || !terms.has_value())
        return {};

    auto trimmed_terms = terms->bytes_as_string_view().trim_whitespace();
    if (trimmed_terms.is_empty())
        return {};
    return MUST(String::from_utf8(trimmed_terms));
}

// NB: Search terms are raw phrases, so case is folded beyond ASCII: "über" starts a word of "Über alles".
static bool folded_query_starts_a_word_of(StringView folded_query, StringView terms)
{
    auto folded_terms = MUST(MUST(String::from_utf8(terms)).to_casefold());
    auto text = folded_terms.bytes_as_string_view();
    if (text.starts_with(folded_query))
        return true;
    for (size_t index = 1; index < text.length(); ++index) {
        // NB: A word starts after a space or a mark such as a quote or hyphen. Bytes beyond ASCII are part of a letter.
        auto previous = text[index - 1];
        if (is_ascii(previous) && !is_ascii_alphanumeric(previous) && text.substring_view(index).starts_with(folded_query))
            return true;
    }
    return false;
}

void show_search_results_pages_as_searches(StringView query, Vector<AutocompleteSuggestion>& suggestions, Optional<SearchEngine> const& search_engine)
{
    if (!search_engine.has_value())
        return;
    auto results_url = URL::Parser::basic_parse(search_engine->query_url);
    if (!results_url.has_value())
        return;
    auto folded_query = MUST(MUST(String::from_utf8(query)).to_casefold());

    for (auto& suggestion : suggestions) {
        if (suggestion.source != AutocompleteSuggestionSource::History && suggestion.source != AutocompleteSuggestionSource::Adaptive)
            continue;
        auto url = URL::Parser::basic_parse(suggestion.text);
        if (!url.has_value())
            continue;
        auto terms = search_terms_of_results_page(*url, *results_url);
        // NB: A results page stays a page unless the query starts a word of its terms, so one found only by its address
        //     or title is not shown as a search.
        if (!terms.has_value() || !folded_query_starts_a_word_of(folded_query, *terms))
            continue;

        suggestion.source = AutocompleteSuggestionSource::Search;
        suggestion.text = *terms;
        suggestion.title = terms.release_value();
        suggestion.subtitle = {};
        suggestion.favicon_png = {};
        suggestion.match_class = AutocompleteMatchClass::None;
        // NB: A past search is offered, but Return goes to the text as typed or to an address it completes to.
        suggestion.can_be_automatically_selected = false;
        suggestion.can_be_inline_completed = false;
    }
}

static Optional<URL::URL> url_of_suggestion(AutocompleteSuggestion const& suggestion)
{
    // NB: Most rows hold serialized URLs. The row for the text as typed holds that text, and a learned row holds the
    //     address as the user typed it, unless they chose a row for it. Only such text is classified, as that looks at
    //     the file system.
    Optional<URL::URL> url;
    if (suggestion.source != AutocompleteSuggestionSource::LiteralURL)
        url = URL::Parser::basic_parse(suggestion.text);
    // NB: The URL parser takes typed text such as "localhost:3000" for a scheme and a path, so a learned row that
    //     parses that way is classified as Return would take it.
    if (url.has_value() && suggestion.source == AutocompleteSuggestionSource::Adaptive && url->has_an_opaque_path())
        url.clear();
    if (!url.has_value())
        url = classify_user_input(suggestion.text).url;
    if (!url.has_value())
        url = URL::Parser::basic_parse(MUST(String::formatted("https://{}", suggestion.text)));
    return url;
}

static SuggestionDestination destination_of_suggestion(AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == AutocompleteSuggestionSource::Search)
        return { .is_search = true, .key = normalize_omnibox_destination(suggestion.text, OmniboxDestinationKind::Search), .serialized_url = {} };

    auto url = url_of_suggestion(suggestion);
    if (!url.has_value())
        return {};
    SuggestionDestination destination { .is_search = false, .key = {}, .serialized_url = url->serialize() };

    // Addresses that differ only by tracking query parameters, or by a trailing slash, go to the same page, so their rows
    // show as one.
    if (auto query = url->query(); query.has_value()) {
        StringBuilder kept_query;
        for (auto parameter : query->split_view('&')) {
            if (is_tracking_query_parameter(split_query_parameter(parameter).name)) {
                destination.has_tracking_query_parameters = true;
                continue;
            }
            if (!kept_query.is_empty())
                kept_query.append('&');
            kept_query.append(parameter);
        }
        auto kept = kept_query.to_byte_string();
        url->set_query(kept.is_empty() ? Optional<StringView> {} : kept.view());
    }
    if (!url->has_an_opaque_path()) {
        auto path = ByteString { url->serialize_path() };
        if (path.length() > 1 && path.ends_with('/')) {
            auto segments = path.view().substring_view(1, path.length() - 2).split_view('/', SplitBehavior::KeepEmpty);
            url->set_path(segments);
        }
    }

    destination.key = url->serialize();
    return destination;
}

static bool suggestion_destinations_are_the_same(SuggestionDestination const& left, SuggestionDestination const& right)
{
    return left.is_search == right.is_search && left.key.has_value() && left.key == right.key;
}

Optional<size_t> index_of_suggestion_with_same_destination(AutocompleteSuggestion const& needle, ReadonlySpan<AutocompleteSuggestion> suggestions)
{
    // OPTIMIZATION: The row looked for is usually still there as it was, and working out where the row for the text as
    //               typed goes looks at the file system, so rows with the same text are looked for first. The muxer
    //               never gives two rows that go to the same place, so this is still the first that goes there.
    for (size_t index = 0; index < suggestions.size(); ++index) {
        if (suggestions[index].source == needle.source && suggestions[index].text == needle.text)
            return index;
    }

    auto needle_destination = destination_of_suggestion(needle);
    if (!needle_destination.key.has_value())
        return {};
    for (size_t index = 0; index < suggestions.size(); ++index) {
        if (suggestion_destinations_are_the_same(needle_destination, destination_of_suggestion(suggestions[index])))
            return index;
    }
    return {};
}

// A local row for the same page as a remote address ranks as the remote address did, so the popup keeps the rank the
// search engine gave it. Only a row that cannot become the default takes it, though: the search engine must not choose
// the default row or the inline completion.
static i32 relevance_of_local_row_for_remote_address(AutocompleteSuggestion const& one, AutocompleteSuggestion const& other)
{
    // NB: The two rows may come in either order.
    auto const& local = one.source == AutocompleteSuggestionSource::RemoteURL ? other : one;
    auto const& remote = one.source == AutocompleteSuggestionSource::RemoteURL ? one : other;
    if (local.can_be_automatically_selected || local.can_be_inline_completed)
        return local.relevance;
    return max(local.relevance, remote.relevance);
}

static bool is_remote_address_meeting_local_row(AutocompleteSuggestion const& one, AutocompleteSuggestion const& other)
{
    return (one.source == AutocompleteSuggestionSource::RemoteURL) != (other.source == AutocompleteSuggestionSource::RemoteURL);
}

static void merge_suggestion(AutocompleteSuggestion& existing, SuggestionDestination& existing_destination, AutocompleteSuggestion suggestion, SuggestionDestination destination)
{
    auto is_verbatim = existing.is_verbatim || suggestion.is_verbatim;
    auto can_be_automatically_selected = existing.can_be_automatically_selected || suggestion.can_be_automatically_selected;
    auto can_be_inline_completed = existing.can_be_inline_completed || suggestion.can_be_inline_completed;
    auto match_relevance = max(existing.match_relevance, suggestion.match_relevance);
    auto history_relevance = max(existing.history_relevance, suggestion.history_relevance);
    auto bookmark_relevance = max(existing.bookmark_relevance, suggestion.bookmark_relevance);
    auto adaptive_relevance = max(existing.adaptive_relevance, suggestion.adaptive_relevance);
    auto relevance = is_remote_address_meeting_local_row(existing, suggestion)
        ? relevance_of_local_row_for_remote_address(existing, suggestion)
        : max(existing.relevance, suggestion.relevance);
    if (match_relevance != 0 || history_relevance != 0 || bookmark_relevance != 0 || adaptive_relevance != 0)
        relevance = max(relevance, match_relevance + history_relevance + bookmark_relevance + adaptive_relevance);

    auto suggestion_has_preferred_presentation = suggestion.source == AutocompleteSuggestionSource::Bookmark
        || (existing.source == AutocompleteSuggestionSource::Adaptive && suggestion.source == AutocompleteSuggestionSource::History)
        || (existing.source == AutocompleteSuggestionSource::LiteralURL && suggestion.source != AutocompleteSuggestionSource::LiteralURL && suggestion.source != AutocompleteSuggestionSource::RemoteURL)
        // NB: A remote address has no title or favicon, so a local row for the same page is shown in its place.
        || (existing.source == AutocompleteSuggestionSource::RemoteURL && suggestion.source != AutocompleteSuggestionSource::RemoteURL);
    if (suggestion_has_preferred_presentation) {
        swap(existing, suggestion);
        swap(existing_destination, destination);
    }

    // NB: The addresses of the two rows may differ by a trailing slash or tracking parameters. The row goes to the one
    //     the user typed, or else to one without tracking parameters.
    auto other_address_is_preferred = suggestion.is_verbatim
        || (!is_verbatim && existing_destination.has_tracking_query_parameters && !destination.has_tracking_query_parameters);
    if (other_address_is_preferred && destination.serialized_url.has_value() && destination.serialized_url != existing_destination.serialized_url) {
        existing.text = *destination.serialized_url;
        existing_destination = move(destination);
    }

    // NB: The row whose presentation is kept may lack parts of it, such as a bookmark saved without a favicon, so it
    //     takes those of the other row.
    if (!existing.title.has_value() && suggestion.title.has_value())
        existing.title = move(suggestion.title);
    if (!existing.subtitle.has_value() && suggestion.subtitle.has_value())
        existing.subtitle = move(suggestion.subtitle);
    if (!existing.favicon_png.has_value() && suggestion.favicon_png.has_value())
        existing.favicon_png = move(suggestion.favicon_png);

    existing.is_verbatim = is_verbatim;
    existing.can_be_automatically_selected = can_be_automatically_selected;
    existing.can_be_inline_completed = can_be_inline_completed;
    existing.relevance = relevance;
    existing.match_relevance = match_relevance;
    existing.history_relevance = history_relevance;
    existing.bookmark_relevance = bookmark_relevance;
    existing.adaptive_relevance = adaptive_relevance;
}

static void append_or_merge(Vector<AutocompleteSuggestion>& suggestions, Vector<SuggestionDestination>& destinations, AutocompleteSuggestion suggestion)
{
    auto destination = destination_of_suggestion(suggestion);
    for (size_t index = 0; index < suggestions.size(); ++index) {
        if (!suggestion_destinations_are_the_same(destinations[index], destination))
            continue;
        merge_suggestion(suggestions[index], destinations[index], move(suggestion), move(destination));
        return;
    }
    suggestions.append(move(suggestion));
    destinations.append(move(destination));
}

Optional<String> autocomplete_host_family(URL::URL const& url)
{
    if (!url.host().has_value())
        return {};
    auto host = MUST(String::from_utf8(url.serialized_host())).to_ascii_lowercase();
    if (host.bytes_as_string_view().starts_with("www."sv))
        return MUST(String::from_utf8(host.bytes_as_string_view().substring_view(4)));
    return host;
}

static Optional<String> origin_family(AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == AutocompleteSuggestionSource::Search)
        return {};
    auto url = URL::Parser::basic_parse(suggestion.text);
    if (!url.has_value())
        return {};
    return autocomplete_host_family(*url);
}

static bool is_local_navigation(AutocompleteSuggestion const& suggestion)
{
    return suggestion.source != AutocompleteSuggestionSource::Search && suggestion.source != AutocompleteSuggestionSource::RemoteURL;
}

// A search the user made before, found in their history or learned from the omnibox, ranks by how well it matches.
static bool is_past_search(AutocompleteSuggestion const& suggestion)
{
    return suggestion.source == AutocompleteSuggestionSource::Search
        && (suggestion.match_relevance != 0 || suggestion.history_relevance != 0 || suggestion.adaptive_relevance != 0);
}

static bool is_remote_suggestion(AutocompleteSuggestion const& suggestion, String const& normalized_query)
{
    if (suggestion.source == AutocompleteSuggestionSource::RemoteURL)
        return true;
    // NB: The search that follows a typed address like "setup.py" is for the text as typed, not a remote suggestion.
    // NB: A past search is the user's own, so it does not take the place of what the search engine suggests.
    return suggestion.source == AutocompleteSuggestionSource::Search
        && !suggestion.is_verbatim
        && !is_past_search(suggestion)
        && normalize_omnibox_destination(suggestion.text, OmniboxDestinationKind::Search) != normalized_query;
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

static bool are_equivalent_origin_presentations(AutocompleteSuggestion const& left, AutocompleteSuggestion const& right)
{
    if (left.source == AutocompleteSuggestionSource::Search || right.source == AutocompleteSuggestionSource::Search)
        return false;
    // NB: The row for the text as typed goes to the host the user typed, so it is not folded into a row for another.
    if (left.is_verbatim || right.is_verbatim)
        return false;
    if (!is_origin_navigation(left) || !is_origin_navigation(right))
        return false;

    auto left_url = URL::Parser::basic_parse(left.text);
    auto right_url = URL::Parser::basic_parse(right.text);
    VERIFY(left_url.has_value() && right_url.has_value());
    if (!left_url->scheme().is_one_of("http"sv, "https"sv) || !right_url->scheme().is_one_of("http"sv, "https"sv))
        return false;

    return left_url->scheme() == right_url->scheme()
        && left_url->port() == right_url->port()
        && origin_family(left) == origin_family(right);
}

static void coalesce_equivalent_origin_presentations(Vector<AutocompleteSuggestion>& candidates)
{
    Vector<AutocompleteSuggestion> representatives;
    representatives.ensure_capacity(candidates.size());
    Vector<Optional<String>> group_titles;
    group_titles.ensure_capacity(candidates.size());

    for (auto& candidate : candidates) {
        Optional<size_t> existing_index;
        for (size_t index = 0; index < representatives.size(); ++index) {
            if (!are_equivalent_origin_presentations(representatives[index], candidate))
                continue;
            if (group_titles[index].has_value() && candidate.title.has_value() && group_titles[index] != candidate.title)
                continue;
            existing_index = index;
            break;
        }

        if (!existing_index.has_value()) {
            group_titles.append(candidate.title);
            representatives.append(move(candidate));
            continue;
        }

        if (!group_titles[*existing_index].has_value() && candidate.title.has_value())
            group_titles[*existing_index] = candidate.title;

        // NB: A remote address has no title or favicon, so a local presentation of the same origin is kept over it.
        auto& existing = representatives[*existing_index];
        if (is_remote_address_meeting_local_row(existing, candidate)) {
            auto relevance = relevance_of_local_row_for_remote_address(existing, candidate);
            if (existing.source == AutocompleteSuggestionSource::RemoteURL)
                swap(existing, candidate);
            existing.relevance = relevance;
        } else if (suggestion_is_better(candidate, existing)) {
            swap(existing, candidate);
        }

        // NB: A learned row has no favicon or title, so the row kept takes those of the folded row.
        if (!existing.favicon_png.has_value() && candidate.favicon_png.has_value())
            existing.favicon_png = move(candidate.favicon_png);
        if (!existing.title.has_value() && candidate.title.has_value())
            existing.title = move(candidate.title);
    }

    candidates = move(representatives);
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

    auto verbatim_is_literal_url = verbatim_suggestion.has_value()
        && verbatim_suggestion->source == AutocompleteSuggestionSource::LiteralURL;

    Vector<AutocompleteSuggestion> candidates;
    Vector<SuggestionDestination> candidate_destinations;
    auto candidate_capacity = local_suggestions.size() + remote_suggestions.size() + (verbatim_suggestion.has_value() ? 1 : 0);
    candidates.ensure_capacity(candidate_capacity);
    candidate_destinations.ensure_capacity(candidate_capacity);
    if (verbatim_suggestion.has_value())
        append_or_merge(candidates, candidate_destinations, verbatim_suggestion.release_value());
    for (auto& suggestion : local_suggestions)
        append_or_merge(candidates, candidate_destinations, move(suggestion));
    for (auto& suggestion : remote_suggestions)
        append_or_merge(candidates, candidate_destinations, move(suggestion));

    auto query_contains_path = query_contains_url_suffix(query);
    auto query_length = Utf8View { query }.length();
    auto is_short_host_query = !query_contains_path && query_length <= 2;

    if (!query_contains_path)
        coalesce_equivalent_origin_presentations(candidates);

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

            // NB: A local row is kept over a remote address for another page of the site, with its own rank.
            auto& existing = representatives[*existing_index];
            if (is_remote_address_meeting_local_row(existing, candidate)) {
                if (existing.source == AutocompleteSuggestionSource::RemoteURL)
                    existing = move(candidate);
                continue;
            }
            auto candidate_preference = short_query_origin_preference(candidate, query_length);
            auto existing_preference = short_query_origin_preference(existing, query_length);
            if (candidate_preference > existing_preference
                || (candidate_preference == existing_preference && suggestion_is_better(candidate, existing)))
                existing = move(candidate);
        }

        candidates = move(representatives);
    }

    quick_sort(candidates, suggestion_is_better);

    auto default_index = candidates.find_first_index_if([verbatim_is_literal_url](auto const& suggestion) {
        if (verbatim_is_literal_url)
            return suggestion.is_verbatim;
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
    size_t remote_suggestion_count = 0;
    size_t past_search_count = 0;
    auto normalized_query = normalize_omnibox_destination(query, OmniboxDestinationKind::Search);
    auto verbatim_index = candidates.find_first_index_if([](auto const& candidate) { return candidate.is_verbatim; });

    for (size_t index = 0; index < candidates.size() && result.size() < result_limit; ++index) {
        auto const& candidate = candidates[index];
        auto origin = origin_family(candidate);
        auto exceeds_origin_limit = !query_contains_path
            && origin.has_value()
            && origin_counts.get(*origin).value_or(0) >= origin_limit;
        auto candidate_is_remote_suggestion = is_remote_suggestion(candidate, normalized_query);
        // NB: Past searches take no remote slot, but have a limit of their own, so that many results pages in history
        //     cannot fill the list.
        auto candidate_is_past_search = !candidate.is_verbatim && is_past_search(candidate);
        auto exceeds_source_limit = (is_local_navigation(candidate) && local_navigation_count >= 4)
            || (candidate_is_remote_suggestion && remote_suggestion_count >= 3)
            || (candidate_is_past_search && past_search_count >= 3);
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
        remote_suggestion_count += candidate_is_remote_suggestion ? 1 : 0;
        past_search_count += candidate_is_past_search ? 1 : 0;
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
