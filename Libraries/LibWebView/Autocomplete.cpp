/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/Find.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibURL/URL.h>
#include <LibWebCommon/MimeSniff/MimeType.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/URL.h>
#include <LibWebView/WebUI.h>

namespace WebView {

static constexpr auto file_url_prefix = "file://"sv;
static constexpr auto about_url_prefix = "about:"sv;

Autocomplete::Autocomplete(AutocompleteHost& host, IsPrivate is_private)
    : SettingsObserver(host.autocomplete_settings())
    , m_host(host)
    , m_is_private(is_private)
{
    m_service_client_id = m_host.autocomplete_service().register_client([this](auto query_id, auto entries) {
        local_query_complete(query_id, move(entries));
    });
}

Autocomplete::~Autocomplete()
{
    cancel_pending_query();
    m_host.autocomplete_service().unregister_client(m_service_client_id);
}

void Autocomplete::cancel_pending_query()
{
    if (m_remote_query_timer) {
        m_remote_query_timer->stop();
        m_remote_query_timer.clear();
    }

    if (auto cancel_remote_query = move(m_cancel_remote_query))
        cancel_remote_query();

    // Buffered callbacks may still arrive after we stop the request, so clear
    // the active query as well and let the stale-response check discard them.
    m_host.autocomplete_service().cancel(m_service_client_id);
    m_query_id = {};
    m_query = {};
    m_trimmed_query = {};
    m_classified_query = {};
    m_local_suggestions.clear();
    m_remote_suggestions.clear();
    m_visited_host_families.clear();
    m_external_url.clear();
    m_external_url_handler_query_complete = true;
    m_external_url_has_handler = false;
}

void Autocomplete::record_engagement(OmniboxEngagement engagement)
{
    if (m_is_private == IsPrivate::Yes)
        return;
    if (engagement.destination_kind == OmniboxDestinationKind::Search && !m_host.autocomplete_settings().search_engine_settings().engine.has_value())
        return;
    m_host.autocomplete_service().record_engagement(move(engagement));
}

size_t autocomplete_preliminary_limit(size_t max_suggestions)
{
    return max(max_suggestions * 4, 32uz);
}

String autocomplete_suggestion_display_text(AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == AutocompleteSuggestionSource::Search)
        return suggestion.text;
    // NB: Choosing a search engine leaves the query in the field, rather than the address of the settings.
    if (suggestion.source == AutocompleteSuggestionSource::ChooseSearchEngine)
        return suggestion.highlight_input.is_empty() ? suggestion.text : suggestion.highlight_input;

    auto url = classify_user_input(suggestion.text).url;
    if (!url.has_value())
        url = URL::create_with_url_or_path(suggestion.text.to_byte_string());
    if (!url.has_value())
        return suggestion.text;
    return url_for_display(*url);
}

Vector<AutocompleteMatchRange> autocomplete_match_ranges(StringView input, StringView text)
{
    Vector<AutocompleteMatchRange> ranges;

    for (auto token : input.split_view_if([](char ch) { return is_ascii_space(ch); })) {
        if (token.is_empty())
            continue;

        for (size_t offset = 0; offset + token.length() <= text.length();) {
            auto candidate = text.substring_view(offset, token.length());
            if (candidate.equals_ignoring_ascii_case(token)) {
                ranges.append({ offset, token.length() });
                offset += token.length();
            } else {
                ++offset;
            }
        }
    }

    quick_sort(ranges, [](auto const& left, auto const& right) {
        return left.start < right.start;
    });

    Vector<AutocompleteMatchRange> merged_ranges;
    for (auto const& range : ranges) {
        if (merged_ranges.is_empty() || merged_ranges.last().start + merged_ranges.last().length < range.start) {
            merged_ranges.append(range);
            continue;
        }
        auto end = max(merged_ranges.last().start + merged_ranges.last().length, range.start + range.length);
        merged_ranges.last().length = end - merged_ranges.last().start;
    }
    return merged_ranges;
}

bool remote_autocomplete_suggestion_matches_input(StringView suggestion, StringView input)
{
    return suggestion.starts_with(input, CaseSensitivity::CaseInsensitive);
}

Vector<AutocompleteSuggestion> web_ui_autocomplete_suggestions(StringView input)
{
    if (!input.starts_with(about_url_prefix, CaseSensitivity::CaseInsensitive))
        return {};

    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(WebUI::pages().size());

    for (auto const& page : WebUI::pages()) {
        auto url = MUST(String::formatted("about:{}", page.host));
        if (!url.bytes_as_string_view().starts_with(input, CaseSensitivity::CaseInsensitive))
            continue;

        suggestions.unchecked_append({
            .source = AutocompleteSuggestionSource::WebUI,
            .text = move(url),
            .title = MUST(String::from_utf8(page.title)),
            .subtitle = {},
            .favicon_png = {},
            .highlight_input = {},
            .match_class = AutocompleteMatchClass::URLPrefix,
            .relevance = 1100,
            .is_verbatim = false,
            .can_be_automatically_selected = true,
            .can_be_inline_completed = true,
        });
    }

    return suggestions;
}

[[maybe_unused]] static ByteString log_autocomplete_suggestions(Vector<AutocompleteSuggestion> const& suggestions)
{
    Vector<ByteString> values;
    values.ensure_capacity(suggestions.size());

    for (auto const& suggestion : suggestions)
        values.unchecked_append(suggestion.text.bytes_as_string_view());

    return ByteString::formatted("[{}]", ByteString::join(", "sv, values));
}

static Optional<AutocompleteSuggestion> search_for_query_suggestion(Optional<SearchEngine> const& search_engine, StringView query)
{
    if (query.is_empty())
        return {};

    auto query_string = MUST(String::from_utf8(query));
    auto contains_whitespace = any_of(query, [](auto code_unit) { return is_ascii_space(code_unit); });

    if (!search_engine.has_value()) {
        return AutocompleteSuggestion {
            .source = AutocompleteSuggestionSource::ChooseSearchEngine,
            .text = "about:settings#search"_string,
            .title = "Choose a search engine…"_string,
            .subtitle = MUST(String::formatted("to search for “{}”", query_string)),
            .favicon_png = {},
            .highlight_input = {},
            .relevance = contains_whitespace ? 1000 : 900,
            .is_verbatim = true,
            .can_be_automatically_selected = true,
            .can_be_inline_completed = false,
        };
    }

    return AutocompleteSuggestion {
        .source = AutocompleteSuggestionSource::Search,
        .text = query_string,
        .title = query_string,
        .subtitle = {},
        .favicon_png = {},
        .highlight_input = {},
        .relevance = contains_whitespace ? 1000 : 900,
        .is_verbatim = true,
        .can_be_automatically_selected = true,
        .can_be_inline_completed = false,
    };
}

static AutocompleteSuggestion make_literal_url_suggestion(StringView query)
{
    return {
        .source = AutocompleteSuggestionSource::LiteralURL,
        .text = MUST(String::from_utf8(query)),
        .title = {},
        .subtitle = {},
        .favicon_png = {},
        .highlight_input = {},
        .relevance = 900,
        .is_verbatim = true,
        .can_be_automatically_selected = true,
        .can_be_inline_completed = false,
    };
}

// Whether the text starts with a scheme for the web, rather than one being guessed for it.
static bool starts_with_web_scheme(StringView text)
{
    text = text.trim_whitespace();
    return text.starts_with("http://"sv, CaseSensitivity::CaseInsensitive) || text.starts_with("https://"sv, CaseSensitivity::CaseInsensitive);
}

Optional<AutocompleteSuggestion> make_verbatim_autocomplete_suggestion(StringView query, ClassifiedUserInput const& classified_query, Optional<SearchEngine> const& search_engine)
{
    if (!query.is_empty() && classified_query.classification == UserInputClassification::InternalURL)
        return make_literal_url_suggestion(query);
    return search_for_query_suggestion(search_engine, query);
}

// A search for the text as typed, offered below the address that the text is, which is where Return goes. It is not
// the row for the text as typed, so it never becomes the default, even when it would outrank the address.
static Optional<AutocompleteSuggestion> search_below_address_for_query_suggestion(Optional<SearchEngine> const& search_engine, StringView query)
{
    auto suggestion = search_for_query_suggestion(search_engine, query);
    if (!suggestion.has_value())
        return {};
    suggestion->relevance = 850;
    suggestion->is_verbatim = false;
    suggestion->can_be_automatically_selected = false;
    return suggestion;
}

Optional<AutocompleteSuggestion> make_search_suggestion_for_address_like_text(StringView query, ClassifiedUserInput const& classified_query, Optional<SearchEngine> const& search_engine)
{
    if (!search_engine.has_value() || starts_with_web_scheme(query))
        return {};
    if (classified_query.classification != UserInputClassification::InternalURL || !classified_query.url.has_value())
        return {};
    if (!classified_query.url->scheme().is_one_of("http"sv, "https"sv))
        return {};

    return search_below_address_for_query_suggestion(search_engine, query);
}

// A remote suggestion that is a web address goes to that address rather than to a search for it. Only a public site
// counts: the search engine must not name the user's own machine or network. Text without a scheme, such as
// "google.com", may well be a phrase like "setup.py" instead, so it counts only for a site the user has been to.
static RemoteAutocompleteSuggestion classify_remote_autocomplete_suggestion(String suggestion)
{
    auto as_search = [&] { return RemoteAutocompleteSuggestion { .text = move(suggestion), .url = {}, .host_family_the_user_must_have_visited = {} }; };

    auto classified_suggestion = classify_user_input_ignoring_local_files(suggestion);
    if (classified_suggestion.classification != UserInputClassification::InternalURL || !classified_suggestion.url.has_value())
        return as_search();

    // NB: Rows do not show the scheme, so an http address would look like the https one for the same site. An address
    //     with a user name, such as "https://bank.example@evil.example/", names one site and goes to another.
    auto const& url = *classified_suggestion.url;
    if (url.scheme() != "https"sv || url.port().has_value() || !url.username().is_empty() || !url.password().is_empty())
        return as_search();

    auto const& host = url.host();
    if (!host.has_value() || !host->is_domain() || host->is_loopback_or_localhost())
        return as_search();

    auto const& domain = host->get<String>();
    auto public_suffix = URL::PublicSuffixData::find_matching_public_suffix(domain, URL::PublicSuffixData::IncludeStarRule::No);
    if (!public_suffix.has_value() || *public_suffix == domain)
        return as_search();

    Optional<String> host_family_the_user_must_have_visited;
    if (!starts_with_web_scheme(suggestion)) {
        host_family_the_user_must_have_visited = autocomplete_host_family(url);
        if (!host_family_the_user_must_have_visited.has_value())
            return as_search();
    }

    return { .text = move(suggestion), .url = url.serialize(), .host_family_the_user_must_have_visited = move(host_family_the_user_must_have_visited) };
}

static Vector<RemoteAutocompleteSuggestion> classify_remote_autocomplete_suggestions(Vector<String> suggestions)
{
    static constexpr auto maximum_remote_candidates = 50uz;
    Vector<RemoteAutocompleteSuggestion> classified_suggestions;
    classified_suggestions.ensure_capacity(min(suggestions.size(), maximum_remote_candidates));
    for (size_t index = 0; index < suggestions.size() && index < maximum_remote_candidates; ++index)
        classified_suggestions.unchecked_append(classify_remote_autocomplete_suggestion(move(suggestions[index])));
    return classified_suggestions;
}

static void add_visited_host_families(HashTable<String>& visited_host_families, ReadonlySpan<AutocompleteSuggestion> local_suggestions)
{
    for (auto const& suggestion : local_suggestions) {
        if (suggestion.source == AutocompleteSuggestionSource::Search || suggestion.source == AutocompleteSuggestionSource::ChooseSearchEngine)
            continue;
        auto url = URL::Parser::basic_parse(suggestion.text);
        // NB: A learned row holds the address the user went to as they typed it, such as "google.com" or
        //     "google.com:8443", which the URL parser takes for a scheme and a path. Such text is classified as Return
        //     would take it.
        if (suggestion.source == AutocompleteSuggestionSource::Adaptive && (!url.has_value() || url->has_an_opaque_path()))
            url = classify_user_input(suggestion.text).url;
        if (!url.has_value())
            continue;
        if (auto family = autocomplete_host_family(*url); family.has_value())
            visited_host_families.set(family.release_value());
    }
}

static Vector<AutocompleteSuggestion> make_remote_autocomplete_suggestions(ReadonlySpan<RemoteAutocompleteSuggestion> remote_suggestions, HashTable<String> const& visited_host_families, LocalSuggestionsAreComplete local_suggestions_are_complete)
{
    if (remote_suggestions.is_empty())
        return {};

    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(remote_suggestions.size());

    for (size_t index = 0; index < remote_suggestions.size(); ++index) {
        auto const& remote_suggestion = remote_suggestions[index];

        auto is_url = false;
        if (remote_suggestion.url.has_value()) {
            auto const& required_host_family = remote_suggestion.host_family_the_user_must_have_visited;
            is_url = !required_host_family.has_value() || visited_host_families.contains(*required_host_family);
            // NB: Shown as a search now, it could turn into a page under the user once the local rows arrive.
            if (!is_url && local_suggestions_are_complete == LocalSuggestionsAreComplete::No)
                continue;
        }

        suggestions.unchecked_append({
            .source = is_url ? AutocompleteSuggestionSource::RemoteURL : AutocompleteSuggestionSource::Search,
            .text = is_url ? *remote_suggestion.url : remote_suggestion.text,
            .title = {},
            .subtitle = {},
            .favicon_png = {},
            .highlight_input = {},
            .relevance = 700 - static_cast<i32>(index),
            .is_verbatim = false,
            .can_be_automatically_selected = false,
            .can_be_inline_completed = false,
        });
    }

    return suggestions;
}

Vector<AutocompleteSuggestion> make_remote_autocomplete_suggestions(Vector<String> remote_suggestions, ReadonlySpan<AutocompleteSuggestion> local_suggestions, LocalSuggestionsAreComplete local_suggestions_are_complete)
{
    HashTable<String> visited_host_families;
    add_visited_host_families(visited_host_families, local_suggestions);
    return make_remote_autocomplete_suggestions(classify_remote_autocomplete_suggestions(move(remote_suggestions)).span(), visited_host_families, local_suggestions_are_complete);
}

void Autocomplete::search_engine_settings_changed()
{
    cancel_pending_query();
}

void Autocomplete::query_autocomplete_engine(AutocompleteQueryID query_id, String query, size_t max_suggestions, Vector<AutocompleteSuggestion> additional_suggestions)
{
    if (m_remote_query_timer) {
        m_remote_query_timer->stop();
        m_remote_query_timer.clear();
    }
    if (auto cancel_remote_query = move(m_cancel_remote_query))
        cancel_remote_query();
    m_host.autocomplete_service().cancel(m_service_client_id);

    m_max_suggestions = max_suggestions;
    m_query_id = query_id;

    auto trimmed_query = MUST(String::from_utf8(query.bytes_as_string_view().trim_whitespace()));
    m_remote_suggestions.remove_all_matching([&](auto const& suggestion) {
        return !remote_autocomplete_suggestion_matches_input(suggestion.text, trimmed_query);
    });
    // NB: The sites the user visited only matter for remote rows that carry over to this query.
    if (m_remote_suggestions.is_empty())
        m_visited_host_families.clear();
    m_trimmed_query = move(trimmed_query);
    m_query = move(query);
    m_local_query_complete = false;
    m_remote_query_complete = false;
    m_classified_query = classify_user_input(m_trimmed_query);
    m_external_url = m_classified_query.classification == UserInputClassification::ExternalURL
        ? m_classified_query.url
        : Optional<URL::URL> {};
    m_external_url_handler_query_complete = !m_external_url.has_value();
    m_external_url_has_handler = false;
    m_local_suggestions.clear();

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Autocomplete query='{}' trimmed='{}'", m_query, m_trimmed_query);

    // Private windows may read normal-profile history but never write through this worker connection.
    m_host.autocomplete_service().query(m_service_client_id, query_id, m_trimmed_query, m_max_suggestions, move(additional_suggestions));

    if (m_external_url.has_value()) {
        auto weak_this = make_weak_ptr();
        m_host.resolve_external_url_handler(*m_external_url, [weak_this, query_id](RefPtr<ExternalURLHandler> handler) mutable {
            if (!weak_this)
                return;
            weak_this->external_url_handler_query_complete(query_id, move(handler));
        });
    }

    if (m_trimmed_query.is_empty()) {
        m_remote_suggestions.clear();
        m_remote_query_complete = true;
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete for empty query");
        return;
    }

    if (m_trimmed_query.starts_with_bytes(file_url_prefix)) {
        m_remote_suggestions.clear();
        m_remote_query_complete = true;
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete for file URL query '{}'", m_trimmed_query);
        return;
    }

    if (m_trimmed_query.bytes_as_string_view().starts_with(about_url_prefix, CaseSensitivity::CaseInsensitive)) {
        m_remote_suggestions.clear();
        m_remote_query_complete = true;
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete for about URL query '{}'", m_trimmed_query);
        return;
    }

    auto const& settings = m_host.autocomplete_settings();
    auto engine = settings.search_engine_settings().engine;
    if (!settings.search_engine_settings().suggestions || !engine.has_value() || !engine->suggestions.has_value()) {
        m_remote_suggestions.clear();
        m_remote_query_complete = true;
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete because search suggestions are disabled or unavailable");
        return;
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Scheduling remote autocomplete suggestions from {} for '{}'", engine->name, m_query);

    static constexpr auto remote_query_debounce_delay_ms = 100;
    m_remote_query_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(remote_query_debounce_delay_ms), [this, query_id, engine = engine.release_value(), query = m_query] {
        if (m_query_id != query_id)
            return;
        start_remote_query(query_id, engine, move(query));
    });
    m_remote_query_timer->start();
}

void Autocomplete::start_remote_query(AutocompleteQueryID query_id, SearchEngine engine, String query)
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Fetching remote autocomplete suggestions from {} for '{}'", engine.name, query);

    if (!engine.suggestions_url_for_query(query).has_value()) {
        m_remote_query_complete = true;
        deliver_current_result();
        return;
    }

    m_cancel_remote_query = m_host.fetch_remote_suggestions(engine, query, m_is_private, [this, query_id, engine, query](AutocompleteHost::RemoteResponse response) {
        // NB: The request is over. Canceling the query from inside the callbacks below must not cancel it again.
        m_cancel_remote_query = nullptr;

        if (m_query_id != query_id) {
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Discarding stale remote autocomplete response for '{}' while current query is '{}'", query, m_query);
            return;
        }

        m_remote_query_complete = true;

        if (response.had_network_error) {
            warnln("Unable to fetch autocomplete suggestions");
            deliver_current_result();
            return;
        }
        if (response.response_code.has_value() && *response.response_code >= 400) {
            warnln("Received error response code {} from autocomplete engine", *response.response_code);
            deliver_current_result();
            return;
        }

        Optional<ByteString const&> content_type;
        if (response.content_type.has_value())
            content_type = *response.content_type;

        if (auto result = received_autocomplete_response(*engine.suggestions, content_type, StringView { response.body.bytes() }); result.is_error()) {
            warnln("Unable to handle autocomplete response: {}", result.error());
            deliver_current_result();
        } else {
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Remote autocomplete suggestions for '{}': {}", query, history_log_suggestions(result.value()));
            m_remote_suggestions = classify_remote_autocomplete_suggestions(result.release_value());

            deliver_current_result();
        }
    });
}

void Autocomplete::local_query_complete(AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions)
{
    if (m_query_id != query_id)
        return;

    m_local_suggestions = move(suggestions);
    show_search_results_pages_as_searches(m_trimmed_query, m_local_suggestions, m_host.autocomplete_settings().search_engine_settings().engine);
    add_visited_host_families(m_visited_host_families, m_local_suggestions);
    m_local_query_complete = true;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Local autocomplete suggestions for '{}': {}", m_trimmed_query, log_autocomplete_suggestions(m_local_suggestions));
    deliver_current_result();
}

void Autocomplete::external_url_handler_query_complete(AutocompleteQueryID query_id, RefPtr<ExternalURLHandler> handler)
{
    if (m_query_id != query_id)
        return;

    m_external_url_handler_query_complete = true;
    m_external_url_has_handler = handler && !handler->is_ladybird();
    deliver_current_result();
}

void Autocomplete::deliver_current_result()
{
    if (!m_query_id.has_value())
        return;

    auto additional_suggestions = web_ui_autocomplete_suggestions(m_trimmed_query);
    Optional<AutocompleteSuggestion> verbatim_suggestion;
    if (additional_suggestions.is_empty() && m_external_url_handler_query_complete) {
        if (m_external_url_has_handler) {
            verbatim_suggestion = make_literal_url_suggestion(m_query);
            // NB: The app that handles the address is where Return goes, so a search for text with a space, such as
            //     "tel:+1 555 1234", must not outrank it.
            if (auto search_suggestion = search_below_address_for_query_suggestion(m_host.autocomplete_settings().search_engine_settings().engine, m_query); search_suggestion.has_value())
                additional_suggestions.append(search_suggestion.release_value());
        } else {
            auto const& search_engine = m_host.autocomplete_settings().search_engine_settings().engine;
            verbatim_suggestion = make_verbatim_autocomplete_suggestion(m_query, m_classified_query, search_engine);
            if (auto search_suggestion = make_search_suggestion_for_address_like_text(m_query, m_classified_query, search_engine); search_suggestion.has_value())
                additional_suggestions.append(search_suggestion.release_value());
        }
    }
    additional_suggestions.extend(m_local_suggestions);
    auto remote_suggestions = make_remote_autocomplete_suggestions(m_remote_suggestions.span(), m_visited_host_families,
        m_local_query_complete ? LocalSuggestionsAreComplete::Yes : LocalSuggestionsAreComplete::No);
    auto merged_suggestions = mux_autocomplete_suggestions(
        m_trimmed_query,
        move(verbatim_suggestion),
        move(additional_suggestions),
        move(remote_suggestions),
        m_max_suggestions);
    for (auto& suggestion : merged_suggestions)
        suggestion.highlight_input = m_trimmed_query;
    auto result_kind = m_local_query_complete && m_remote_query_complete && m_external_url_handler_query_complete
        ? AutocompleteResultKind::Final
        : AutocompleteResultKind::Intermediate;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Merged autocomplete suggestions for '{}': {}", m_query, log_autocomplete_suggestions(merged_suggestions));
    invoke_autocomplete_query_complete(*m_query_id, move(merged_suggestions), result_kind);
}

static ErrorOr<Vector<String>> parse_duckduckgo_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected DuckDuckGo autocomplete response to be a JSON array");

    Vector<String> results;
    results.ensure_capacity(json.as_array().size());

    TRY(json.as_array().try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid DuckDuckGo autocomplete response, expected value to be an object");

        if (auto value = suggestion.as_object().get_string("phrase"sv); value.has_value())
            results.unchecked_append(*value);

        return {};
    }));

    return results;
}

static ErrorOr<Vector<String>> parse_opensearch_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected OpenSearch autocomplete response to be a JSON array");

    auto const& values = json.as_array();
    if (values.size() < 2 || !values[1].is_array())
        return Error::from_string_literal("Invalid OpenSearch autocomplete response, expected suggestions in the second element");

    auto const& suggestions = values[1].as_array();
    Vector<String> results;
    results.ensure_capacity(suggestions.size());

    TRY(suggestions.try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_string())
            return Error::from_string_literal("Invalid OpenSearch autocomplete response, expected value to be a string");
        results.unchecked_append(suggestion.as_string());
        return {};
    }));

    return results;
}

static ErrorOr<Vector<String>> parse_yahoo_autocomplete(JsonValue const& json)
{
    if (!json.is_object())
        return Error::from_string_literal("Expected Yahoo autocomplete response to be a JSON array");

    auto suggestions = json.as_object().get_array("r"sv);
    if (!suggestions.has_value())
        return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"r\" to be an object");

    Vector<String> results;
    results.ensure_capacity(suggestions->size());

    TRY(suggestions->try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected value to be an object");

        auto result = suggestion.as_object().get_string("k"sv);
        if (!result.has_value())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"k\" to be a string");

        results.unchecked_append(*result);
        return {};
    }));

    return results;
}

ErrorOr<Vector<String>> Autocomplete::received_autocomplete_response(SearchSuggestions const& provider, Optional<ByteString const&> content_type, StringView response)
{
    auto decoder = [&]() -> Optional<TextCodec::Decoder> {
        if (!content_type.has_value())
            return {};

        auto mime_type = Web::MimeSniff::MimeType::parse(*content_type);
        if (!mime_type.has_value())
            return {};

        auto charset = mime_type->parameters().get("charset"sv);
        if (!charset.has_value())
            return {};

        return TextCodec::decoder_for(*charset);
    }();

    if (!decoder.has_value())
        decoder = TextCodec::decoder_for("UTF-8"sv);

    auto decoded_response = TRY(TextCodec::decode_to_utf8(response.bytes(), *decoder));
    auto json = TRY(JsonValue::from_string(decoded_response));

    switch (provider.response_format) {
    case SearchSuggestionsFormat::DuckDuckGo:
        return parse_duckduckgo_autocomplete(json);
    case SearchSuggestionsFormat::OpenSearch:
        return parse_opensearch_autocomplete(json);
    case SearchSuggestionsFormat::Yahoo:
        return parse_yahoo_autocomplete(json);
    }
    VERIFY_NOT_REACHED();
}

void Autocomplete::invoke_autocomplete_query_complete(AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind result_kind) const
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Delivering {} autocomplete suggestion(s) as a {} result: {}",
        suggestions.size(),
        result_kind == AutocompleteResultKind::Final ? "final"sv : "intermediate"sv,
        log_autocomplete_suggestions(suggestions));

    if (on_autocomplete_query_complete)
        on_autocomplete_query_complete(query_id, move(suggestions), result_kind);
}

}
