/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/Find.h>
#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWebView/Application.h>
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

Autocomplete::Autocomplete(IsPrivate is_private)
    : m_is_private(is_private)
{
    m_service_client_id = Application::autocomplete_service().register_client([this](auto query_id, auto entries) {
        local_query_complete(query_id, move(entries));
    });
}

Autocomplete::~Autocomplete()
{
    cancel_pending_query();
    Application::autocomplete_service().unregister_client(m_service_client_id);
}

void Autocomplete::cancel_pending_query()
{
    if (m_remote_query_timer) {
        m_remote_query_timer->stop();
        m_remote_query_timer.clear();
    }

    if (m_request) {
        // This can be reached synchronously from inside the request's own completion callback: activating a suggestion
        // clears the location bar's focus, canceling the query. Stopping the request inline would destroy a still-
        // running callback — so defer the teardown (just as set_buffered_request_finished_callback defers its clear).
        Core::deferred_invoke([request = move(m_request)] {
            request->stop();
        });
    }

    // Buffered callbacks may still arrive after we stop the request, so clear
    // the active query as well and let the stale-response check discard them.
    Application::autocomplete_service().cancel(m_service_client_id);
    m_query_id = {};
    m_query = {};
    m_trimmed_query = {};
    m_local_suggestions.clear();
    m_remote_suggestions.clear();
}

void Autocomplete::record_engagement(OmniboxEngagement engagement)
{
    if (m_is_private == IsPrivate::Yes)
        return;
    if (engagement.destination_kind == OmniboxDestinationKind::Search && !Application::settings().search_engine().has_value())
        return;
    Application::autocomplete_service().record_engagement(move(engagement));
}

String autocomplete_suggestion_display_text(AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == AutocompleteSuggestionSource::Search)
        return suggestion.text;

    auto url = URL::create_with_url_or_path(suggestion.text.to_byte_string());
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

Vector<String> filter_remote_autocomplete_suggestions(StringView input, Vector<String> suggestions)
{
    suggestions.remove_all_matching([input](auto const& suggestion) {
        return !suggestion.bytes_as_string_view().starts_with(input, CaseSensitivity::CaseInsensitive);
    });
    return suggestions;
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
            .favicon_base64_png = {},
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

static Optional<AutocompleteSuggestion> search_for_query_suggestion(StringView query)
{
    if (query.is_empty() || location_looks_like_url(query))
        return {};

    auto const& search_engine = Application::settings().search_engine();
    if (!search_engine.has_value())
        return {};

    auto query_string = MUST(String::from_utf8(query));
    auto subtitle = MUST(String::formatted("Search with {}", search_engine->name));
    auto contains_whitespace = any_of(query, [](auto code_unit) { return is_ascii_space(code_unit); });

    return AutocompleteSuggestion {
        .source = AutocompleteSuggestionSource::Search,
        .text = query_string,
        .title = query_string,
        .subtitle = move(subtitle),
        .favicon_base64_png = {},
        .highlight_input = {},
        .relevance = contains_whitespace ? 1000 : 900,
        .is_verbatim = true,
        .can_be_automatically_selected = true,
        .can_be_inline_completed = false,
    };
}

static Optional<AutocompleteSuggestion> literal_url_suggestion(StringView query)
{
    if (query.is_empty() || !location_looks_like_url(query))
        return {};

    return AutocompleteSuggestion {
        .source = AutocompleteSuggestionSource::LiteralURL,
        .text = MUST(String::from_utf8(query)),
        .title = {},
        .subtitle = {},
        .favicon_base64_png = {},
        .highlight_input = {},
        .relevance = 900,
        .is_verbatim = true,
        .can_be_automatically_selected = true,
        .can_be_inline_completed = false,
    };
}

static Vector<AutocompleteSuggestion> make_remote_suggestions(Vector<String> remote_suggestions)
{
    static constexpr auto maximum_remote_candidates = 50uz;
    Vector<AutocompleteSuggestion> suggestions;
    suggestions.ensure_capacity(min(remote_suggestions.size(), maximum_remote_candidates));

    i32 relevance = 700;
    for (size_t index = 0; index < remote_suggestions.size() && index < maximum_remote_candidates; ++index) {
        suggestions.unchecked_append({
            .source = AutocompleteSuggestionSource::Search,
            .text = move(remote_suggestions[index]),
            .title = {},
            .subtitle = {},
            .favicon_base64_png = {},
            .highlight_input = {},
            .relevance = relevance,
            .is_verbatim = false,
            .can_be_automatically_selected = false,
            .can_be_inline_completed = false,
        });
        --relevance;
    }

    return suggestions;
}

void Autocomplete::query_autocomplete_engine(AutocompleteQueryID query_id, String query, size_t max_suggestions)
{
    if (m_remote_query_timer) {
        m_remote_query_timer->stop();
        m_remote_query_timer.clear();
    }
    if (m_request) {
        m_request->stop();
        m_request.clear();
    }
    Application::autocomplete_service().cancel(m_service_client_id);

    m_max_suggestions = max_suggestions;
    m_query_id = query_id;

    auto trimmed_query = MUST(String::from_utf8(query.bytes_as_string_view().trim_whitespace()));
    m_remote_suggestions = filter_remote_autocomplete_suggestions(trimmed_query, move(m_remote_suggestions));
    m_trimmed_query = move(trimmed_query);
    m_query = move(query);
    m_local_query_complete = false;
    m_remote_query_complete = false;
    m_local_suggestions.clear();

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Autocomplete query='{}' trimmed='{}'", m_query, m_trimmed_query);

    // Private windows may read normal-profile history but never write through this worker connection.
    Application::autocomplete_service().query(m_service_client_id, query_id, m_trimmed_query, m_max_suggestions);

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

    auto engine = Application::settings().autocomplete_engine();
    if (!engine.has_value()) {
        m_remote_suggestions.clear();
        m_remote_query_complete = true;
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Skipping remote autocomplete because no engine is configured");
        return;
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Scheduling remote autocomplete suggestions from {} for '{}'", engine->name, m_query);

    static constexpr auto remote_query_debounce_delay_ms = 100;
    m_remote_query_timer = Core::Timer::create_single_shot(remote_query_debounce_delay_ms, [this, query_id, engine = engine.release_value(), query = m_query] {
        if (m_query_id != query_id)
            return;
        start_remote_query(query_id, engine, move(query));
    });
    m_remote_query_timer->start();
}

void Autocomplete::start_remote_query(AutocompleteQueryID query_id, AutocompleteEngine engine, String query)
{
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Fetching remote autocomplete suggestions from {} for '{}'", engine.name, query);

    auto url_string = MUST(String::formatted(engine.query_url, URL::percent_encode(query)));
    auto url = URL::Parser::basic_parse(url_string);

    if (!url.has_value()) {
        m_remote_query_complete = true;
        deliver_current_result();
        return;
    }

    m_request = Application::request_server_client(m_is_private).start_request("GET"sv, *url);

    m_request->set_buffered_request_finished_callback(
        [this, query_id, engine, query = move(query)](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache, Core::ImmutableBytes payload) {
            Core::deferred_invoke([this]() { m_request.clear(); });

            if (m_query_id != query_id) {
                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Discarding stale remote autocomplete response for '{}' while current query is '{}'", query, m_query);
                return;
            }

            m_remote_query_complete = true;

            if (network_error.has_value()) {
                warnln("Unable to fetch autocomplete suggestions: {}", Requests::network_error_to_string(*network_error));
                deliver_current_result();
                return;
            }
            if (response_code.has_value() && *response_code >= 400) {
                warnln("Received error response code {} from autocomplete engine: {}", *response_code, reason_phrase);
                deliver_current_result();
                return;
            }

            auto content_type = response_headers.get("Content-Type"sv);

            if (auto result = received_autocomplete_respsonse(engine, content_type, payload.bytes()); result.is_error()) {
                warnln("Unable to handle autocomplete response: {}", result.error());
                deliver_current_result();
            } else {
                m_remote_suggestions = result.release_value();

                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Remote autocomplete suggestions for '{}': {}", query, history_log_suggestions(m_remote_suggestions));
                deliver_current_result();
            }
        });
}

void Autocomplete::local_query_complete(AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions)
{
    if (m_query_id != query_id)
        return;

    m_local_suggestions = move(suggestions);
    m_local_query_complete = true;

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Local autocomplete suggestions for '{}': {}", m_trimmed_query, log_autocomplete_suggestions(m_local_suggestions));
    deliver_current_result();
}

void Autocomplete::deliver_current_result()
{
    if (!m_query_id.has_value())
        return;

    auto web_ui_suggestions = web_ui_autocomplete_suggestions(m_trimmed_query);
    auto verbatim_suggestion = web_ui_suggestions.is_empty() ? literal_url_suggestion(m_query) : Optional<AutocompleteSuggestion> {};
    if (web_ui_suggestions.is_empty() && !verbatim_suggestion.has_value())
        verbatim_suggestion = search_for_query_suggestion(m_query);
    web_ui_suggestions.extend(m_local_suggestions);
    auto merged_suggestions = mux_autocomplete_suggestions(
        m_trimmed_query,
        move(verbatim_suggestion),
        move(web_ui_suggestions),
        make_remote_suggestions(m_remote_suggestions),
        m_max_suggestions);
    for (auto& suggestion : merged_suggestions)
        suggestion.highlight_input = m_trimmed_query;
    auto result_kind = m_local_query_complete && m_remote_query_complete
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

    auto decoded_response = TRY(decoder->to_utf8(response, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement));
    auto json = TRY(JsonValue::from_string(decoded_response));

    if (engine.name == "DuckDuckGo")
        return parse_duckduckgo_autocomplete(json);
    if (engine.name == "Google")
        return parse_google_autocomplete(json);
    if (engine.name == "Yahoo")
        return parse_yahoo_autocomplete(json);

    return Error::from_string_literal("Invalid engine name");
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
