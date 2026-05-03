/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Find.h>
#include <LibCore/EventLoop.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/HistoryDebug.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/URL.h>

namespace WebView {

static constexpr auto file_url_prefix = "file://"sv;

static constexpr auto builtin_autocomplete_engines = to_array<AutocompleteEngine>({
    { "DuckDuckGo"sv, "https://duckduckgo.com/ac/?q={}"sv },
    { "Google"sv, "https://www.google.com/complete/search?client=chrome&q={}"sv },
    { "Yahoo"sv, "https://search.yahoo.com/sugg/gossip/gossip-us-ura/?output=sd1&command={}"sv },
});

ReadonlySpan<AutocompleteEngine> autocomplete_engines()
{
    return builtin_autocomplete_engines;
}

Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name)
{
    return find_value(builtin_autocomplete_engines, [&](auto const& engine) {
        return engine.name == name;
    });
}

Autocomplete::Autocomplete() = default;
Autocomplete::~Autocomplete() = default;

void Autocomplete::cancel_pending_query()
{
    if (m_request) {
        m_request->stop();
        m_request.clear();
    }

    // Buffered callbacks may still arrive after we stop the request, so clear
    // the active query as well and let the stale-response check discard them.
    m_query = {};
    m_history_suggestions.clear();
}

StringView autocomplete_section_title(AutocompleteSuggestionSection section)
{
    switch (section) {
    case AutocompleteSuggestionSection::None:
        return {};
    case AutocompleteSuggestionSection::History:
        return "History"sv;
    case AutocompleteSuggestionSection::SearchSuggestions:
        return "Search Suggestions"sv;
    }

    VERIFY_NOT_REACHED();
}

[[maybe_unused]] static ByteString log_autocomplete_suggestions(Vector<AutocompleteSuggestion> const& suggestions)
{
    Vector<ByteString> values;
    values.ensure_capacity(suggestions.size());

    for (auto const& suggestion : suggestions)
        values.unchecked_append(suggestion.text.bytes_as_string_view());

    return ByteString::formatted("[{}]", ByteString::join(", "sv, values));
}

static Vector<AutocompleteSuggestion> make_history_suggestions(Vector<HistoryEntry> history_entries)
{
    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(history_entries.size());

    for (auto& entry : history_entries) {
        suggestions.unchecked_append({
            .source = AutocompleteSuggestionSource::History,
            .section = AutocompleteSuggestionSection::History,
            .text = move(entry.url),
            .title = move(entry.title),
            .subtitle = {},
            .favicon_base64_png = move(entry.favicon_base64_png),
        });
    }

    return suggestions;
}

static Optional<AutocompleteSuggestion> search_for_query_suggestion(StringView query)
{
    if (query.is_empty() || location_looks_like_url(query))
        return {};

    auto const& search_engine = Application::settings().search_engine();
    if (!search_engine.has_value())
        return {};

    auto query_string = MUST(String::from_utf8(query));
    auto subtitle = MUST(String::formatted("Search with {}", search_engine->name));

    return AutocompleteSuggestion {
        .source = AutocompleteSuggestionSource::Search,
        .section = AutocompleteSuggestionSection::SearchSuggestions,
        .text = query_string,
        .title = query_string,
        .subtitle = move(subtitle),
        .favicon_base64_png = {},
    };
}

static Optional<AutocompleteSuggestion> literal_url_suggestion(StringView query)
{
    if (query.is_empty() || !location_looks_like_url(query))
        return {};

    return AutocompleteSuggestion {
        .source = AutocompleteSuggestionSource::LiteralURL,
        .section = AutocompleteSuggestionSection::None,
        .text = MUST(String::from_utf8(query)),
        .title = {},
        .subtitle = {},
        .favicon_base64_png = {},
    };
}

static Optional<AutocompleteSuggestion> preferred_literal_url_suggestion(StringView query, Vector<AutocompleteSuggestion> const& history_suggestions)
{
    auto literal_suggestion = literal_url_suggestion(query);
    if (!literal_suggestion.has_value())
        return {};

    // Once history still provides a richer completion for the typed prefix,
    // keep that row instead of promoting a raw literal URL suggestion.
    if (!history_suggestions.is_empty() && autocomplete_url_can_complete(query, history_suggestions.first().text)) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Suppressing literal URL suggestion '{}' because top history suggestion '{}' still completes the query",
            literal_suggestion->text,
            history_suggestions.first().text);
        return {};
    }

    return literal_suggestion;
}

static bool suggestions_match_for_deduplication(AutocompleteSuggestion const& existing_suggestion, AutocompleteSuggestion const& suggestion)
{
    if (existing_suggestion.text == suggestion.text)
        return true;

    if (existing_suggestion.source == AutocompleteSuggestionSource::Search
        || suggestion.source == AutocompleteSuggestionSource::Search)
        return false;

    return autocomplete_urls_match(existing_suggestion.text, suggestion.text);
}

static bool should_replace_existing_suggestion(AutocompleteSuggestion const& existing_suggestion, AutocompleteSuggestion const& suggestion)
{
    return existing_suggestion.source == AutocompleteSuggestionSource::LiteralURL
        && suggestion.source == AutocompleteSuggestionSource::History;
}

static void append_suggestion_if_unique(Vector<AutocompleteSuggestion>& suggestions, size_t max_suggestions, AutocompleteSuggestion suggestion)
{
    if (suggestions.size() >= max_suggestions)
        return;

    for (auto& existing_suggestion : suggestions) {
        if (!suggestions_match_for_deduplication(existing_suggestion, suggestion))
            continue;

        if (should_replace_existing_suggestion(existing_suggestion, suggestion))
            existing_suggestion = move(suggestion);

        return;
    }

    suggestions.unchecked_append(move(suggestion));
}

static Vector<AutocompleteSuggestion> merge_suggestions(
    Optional<AutocompleteSuggestion> search_for_query_suggestion,
    Optional<AutocompleteSuggestion> literal_url_suggestion,
    Vector<AutocompleteSuggestion> history_suggestions,
    Vector<String> remote_suggestions,
    size_t max_suggestions)
{
    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(min(max_suggestions, history_suggestions.size() + remote_suggestions.size() + (literal_url_suggestion.has_value() ? 1 : 0) + (search_for_query_suggestion.has_value() ? 1 : 0)));

    // Reserve a slot for the synthesized "Search with <engine>" row so it
    // is always visible, even when history results would otherwise fill the
    // popup. Without this, a query with plenty of history hits leaves the
    // user with no explicit search fallback.
    auto reserved_for_search = search_for_query_suggestion.has_value() ? 1u : 0u;
    auto history_and_url_cap = reserved_for_search < max_suggestions
        ? max_suggestions - reserved_for_search
        : max_suggestions;

    if (literal_url_suggestion.has_value())
        append_suggestion_if_unique(suggestions, history_and_url_cap, literal_url_suggestion.release_value());

    for (auto& suggestion : history_suggestions)
        append_suggestion_if_unique(suggestions, history_and_url_cap, move(suggestion));

    if (search_for_query_suggestion.has_value())
        append_suggestion_if_unique(suggestions, max_suggestions, search_for_query_suggestion.release_value());

    for (auto& suggestion : remote_suggestions) {
        auto remote_suggestion = AutocompleteSuggestion {
            .source = AutocompleteSuggestionSource::Search,
            .section = AutocompleteSuggestionSection::SearchSuggestions,
            .text = move(suggestion),
            .title = {},
            .subtitle = {},
            .favicon_base64_png = {},
        };
        append_suggestion_if_unique(suggestions, max_suggestions, move(remote_suggestion));
    }

    return suggestions;
}

static bool should_defer_intermediate_suggestions(Vector<AutocompleteSuggestion> const& suggestions)
{
    // A lone history row tends to be a transient placeholder while remote
    // suggestions are still in flight, so wait for the merged final list.
    return suggestions.size() == 1
        && suggestions.first().source == AutocompleteSuggestionSource::History;
}

void Autocomplete::query_autocomplete_engine(String query, size_t max_suggestions)
{
    if (m_request) {
        m_request->stop();
        m_request.clear();
    }

    m_max_suggestions = max_suggestions;

    auto trimmed_query = MUST(String::from_utf8(query.bytes_as_string_view().trim_whitespace()));
    m_query = move(query);

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Autocomplete query='{}' trimmed='{}'", m_query, trimmed_query);

    m_history_suggestions = make_history_suggestions(Application::history_store().autocomplete_entries(trimmed_query, m_max_suggestions));
    auto literal_suggestion = preferred_literal_url_suggestion(trimmed_query, m_history_suggestions);
    auto search_suggestion = search_for_query_suggestion(trimmed_query);

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] History autocomplete suggestions for '{}': {}", trimmed_query, log_autocomplete_suggestions(m_history_suggestions));

    auto immediate_suggestions = merge_suggestions(search_suggestion, literal_suggestion, m_history_suggestions, {}, m_max_suggestions);

    if (trimmed_query.is_empty()) {
        invoke_autocomplete_query_complete(move(immediate_suggestions), AutocompleteResultKind::Final);
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete for empty query");
        return;
    }

    if (trimmed_query.starts_with_bytes(file_url_prefix)) {
        invoke_autocomplete_query_complete(move(immediate_suggestions), AutocompleteResultKind::Final);
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete for file URL query '{}'", trimmed_query);
        return;
    }

    auto engine = Application::settings().autocomplete_engine();
    if (!engine.has_value()) {
        invoke_autocomplete_query_complete(move(immediate_suggestions), AutocompleteResultKind::Final);
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete because no engine is configured");
        return;
    }

    if (!immediate_suggestions.is_empty() && !should_defer_intermediate_suggestions(immediate_suggestions)) {
        invoke_autocomplete_query_complete(move(immediate_suggestions), AutocompleteResultKind::Intermediate);
    } else if (!immediate_suggestions.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Deferring singleton history intermediate result for '{}' until remote autocomplete responds", trimmed_query);
    } else {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Deferring empty history autocomplete results for '{}' until remote autocomplete responds", trimmed_query);
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Fetching remote autocomplete suggestions from {} for '{}'", engine->name, m_query);

    auto url_string = MUST(String::formatted(engine->query_url, URL::percent_encode(m_query)));
    auto url = URL::Parser::basic_parse(url_string);

    if (!url.has_value())
        return;

    m_request = Application::request_server_client().start_request("GET"sv, *url);

    m_request->set_buffered_request_finished_callback(
        [this, engine = engine.release_value(), query = m_query, literal_suggestion, search_suggestion](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::AnonymousBuffer>, Optional<u64>, ReadonlyBytes payload) {
            Core::deferred_invoke([this]() { m_request.clear(); });

            if (m_query != query) {
                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Discarding stale remote autocomplete response for '{}' while current query is '{}'", query, m_query);
                return;
            }

            if (network_error.has_value()) {
                warnln("Unable to fetch autocomplete suggestions: {}", Requests::network_error_to_string(*network_error));
                invoke_autocomplete_query_complete(merge_suggestions(search_suggestion, literal_suggestion, m_history_suggestions, {}, m_max_suggestions), AutocompleteResultKind::Final);
                return;
            }
            if (response_code.has_value() && *response_code >= 400) {
                warnln("Received error response code {} from autocomplete engine: {}", *response_code, reason_phrase);
                invoke_autocomplete_query_complete(merge_suggestions(search_suggestion, literal_suggestion, m_history_suggestions, {}, m_max_suggestions), AutocompleteResultKind::Final);
                return;
            }

            auto content_type = response_headers.get("Content-Type"sv);

            if (auto result = received_autocomplete_respsonse(engine, content_type, payload); result.is_error()) {
                warnln("Unable to handle autocomplete response: {}", result.error());
                invoke_autocomplete_query_complete(merge_suggestions(search_suggestion, literal_suggestion, m_history_suggestions, {}, m_max_suggestions), AutocompleteResultKind::Final);
            } else {
                auto remote_suggestions = result.release_value();

                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Remote autocomplete suggestions for '{}': {}", query, history_log_suggestions(remote_suggestions));

                auto merged_suggestions = merge_suggestions(search_suggestion, literal_suggestion, m_history_suggestions, move(remote_suggestions), m_max_suggestions);

                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Merged autocomplete suggestions for '{}': {}", query, log_autocomplete_suggestions(merged_suggestions));

                invoke_autocomplete_query_complete(move(merged_suggestions), AutocompleteResultKind::Final);
            }
        });
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

static ErrorOr<Vector<String>> parse_google_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected Google autocomplete response to be a JSON array");

    auto const& values = json.as_array();

    if (values.size() != 5)
        return Error::from_string_literal("Invalid Google autocomplete response, expected 5 elements in array");
    if (!values[1].is_array())
        return Error::from_string_literal("Invalid Google autocomplete response, expected second element to be an array");

    auto const& suggestions = values[1].as_array();

    Vector<String> results;
    results.ensure_capacity(suggestions.size());

    TRY(suggestions.try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_string())
            return Error::from_string_literal("Invalid Google autocomplete response, expected value to be a string");

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

ErrorOr<Vector<String>> Autocomplete::received_autocomplete_respsonse(AutocompleteEngine const& engine, Optional<ByteString const&> content_type, StringView response)
{
    auto decoder = [&]() -> Optional<TextCodec::Decoder&> {
        if (!content_type.has_value())
            return {};

        auto mime_type = Web::MimeSniff::MimeType::parse(*content_type);
        if (!mime_type.has_value())
            return {};

        auto charset = mime_type->parameters().get("charset"sv);
        if (!charset.has_value())
            return {};

        return TextCodec::decoder_for_exact_name(*charset);
    }();

    if (!decoder.has_value())
        decoder = TextCodec::decoder_for_exact_name("UTF-8"sv);

    auto decoded_response = TRY(decoder->to_utf8(response));
    auto json = TRY(JsonValue::from_string(decoded_response));

    if (engine.name == "DuckDuckGo")
        return parse_duckduckgo_autocomplete(json);
    if (engine.name == "Google")
        return parse_google_autocomplete(json);
    if (engine.name == "Yahoo")
        return parse_yahoo_autocomplete(json);

    return Error::from_string_literal("Invalid engine name");
}

void Autocomplete::invoke_autocomplete_query_complete(Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind result_kind) const
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Delivering {} autocomplete suggestion(s) as a {} result: {}",
        suggestions.size(),
        result_kind == AutocompleteResultKind::Final ? "final"sv : "intermediate"sv,
        log_autocomplete_suggestions(suggestions));

    if (on_autocomplete_query_complete)
        on_autocomplete_query_complete(move(suggestions), result_kind);
}

}
