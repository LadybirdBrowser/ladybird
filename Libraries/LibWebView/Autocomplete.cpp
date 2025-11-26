/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void Autocomplete::query_autocomplete_engine(String query)
{
    if (m_request) {
        m_request->stop();
        m_request.clear();
    }

    auto trimmed_query = query.bytes_as_string_view().trim_whitespace();
    if (trimmed_query.is_empty() || trimmed_query.starts_with(file_url_prefix)) {
        invoke_autocomplete_query_complete({});
        return;
    }

    auto engine = Application::settings().autocomplete_engine();
    if (!engine.has_value()) {
        invoke_autocomplete_query_complete({});
        return;
    }

    auto url_string = MUST(String::formatted(engine->query_url, URL::percent_encode(query)));
    auto url = URL::Parser::basic_parse(url_string);

    if (!url.has_value()) {
        invoke_autocomplete_query_complete({});
        return;
    }

    m_request = Application::request_server_client().start_request("GET"sv, *url);
    m_query = move(query);

    m_request->set_buffered_request_finished_callback(
        [this, engine = engine.release_value()](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const& reason_phrase, ReadonlyBytes payload) {
            Core::deferred_invoke([this]() { m_request.clear(); });

            if (network_error.has_value()) {
                warnln("Unable to fetch autocomplete suggestions: {}", Requests::network_error_to_string(*network_error));
                invoke_autocomplete_query_complete({});
                return;
            }
            if (response_code.has_value() && *response_code >= 400) {
                warnln("Received error response code {} from autocomplete engine: {}", *response_code, reason_phrase);
                invoke_autocomplete_query_complete({});
                return;
            }

            auto content_type = response_headers.get("Content-Type"sv);

            if (auto result = received_autocomplete_respsonse(engine, content_type, payload); result.is_error()) {
                warnln("Unable to handle autocomplete response: {}", result.error());
                invoke_autocomplete_query_complete({});
            } else {
                invoke_autocomplete_query_complete(result.release_value());
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

void Autocomplete::invoke_autocomplete_query_complete(Vector<String> suggestions) const
{
    if (on_autocomplete_query_complete)
        on_autocomplete_query_complete(move(suggestions));
}

}
