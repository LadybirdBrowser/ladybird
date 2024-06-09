/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/Span.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCore/DateTime.h>
#include <LibHTTP/HttpResponse.h>
#include <LibWeb/WebDriver/Client.h>

namespace Web::WebDriver {

using RouteHandler = Response (*)(Client&, Parameters, JsonValue);

struct Route {
    HTTP::HttpRequest::Method method {};
    StringView path;
    RouteHandler handler { nullptr };
};

struct MatchedRoute {
    RouteHandler handler;
    Vector<String> parameters;
};

#define ROUTE(method, path, handler)                              \
    Route                                                         \
    {                                                             \
        HTTP::HttpRequest::method,                                \
            path,                                                 \
            [](auto& client, auto parameters, auto payload) {     \
                return client.handler(parameters, move(payload)); \
            }                                                     \
    }

// https://w3c.github.io/webdriver/#dfn-endpoints
static constexpr auto s_webdriver_endpoints = Array {
    ROUTE(POST, "/session"sv, new_session),
    ROUTE(DELETE, "/session/:session_id"sv, delete_session),
    ROUTE(GET, "/status"sv, get_status),
    ROUTE(GET, "/session/:session_id/timeouts"sv, get_timeouts),
    ROUTE(POST, "/session/:session_id/timeouts"sv, set_timeouts),
    ROUTE(POST, "/session/:session_id/url"sv, navigate_to),
    ROUTE(GET, "/session/:session_id/url"sv, get_current_url),
    ROUTE(POST, "/session/:session_id/back"sv, back),
    ROUTE(POST, "/session/:session_id/forward"sv, forward),
    ROUTE(POST, "/session/:session_id/refresh"sv, refresh),
    ROUTE(GET, "/session/:session_id/title"sv, get_title),
    ROUTE(GET, "/session/:session_id/window"sv, get_window_handle),
    ROUTE(DELETE, "/session/:session_id/window"sv, close_window),
    ROUTE(POST, "/session/:session_id/window"sv, switch_to_window),
    ROUTE(GET, "/session/:session_id/window/handles"sv, get_window_handles),
    ROUTE(POST, "/session/:session_id/window/new"sv, new_window),
    ROUTE(GET, "/session/:session_id/window/rect"sv, get_window_rect),
    ROUTE(POST, "/session/:session_id/window/rect"sv, set_window_rect),
    ROUTE(POST, "/session/:session_id/window/maximize"sv, maximize_window),
    ROUTE(POST, "/session/:session_id/window/minimize"sv, minimize_window),
    ROUTE(POST, "/session/:session_id/window/fullscreen"sv, fullscreen_window),
    ROUTE(POST, "/session/:session_id/window/consume-user-activation"sv, consume_user_activation),
    ROUTE(POST, "/session/:session_id/element"sv, find_element),
    ROUTE(POST, "/session/:session_id/elements"sv, find_elements),
    ROUTE(POST, "/session/:session_id/element/:element_id/element"sv, find_element_from_element),
    ROUTE(POST, "/session/:session_id/element/:element_id/elements"sv, find_elements_from_element),
    ROUTE(POST, "/session/:session_id/shadow/:shadow_id/element"sv, find_element_from_shadow_root),
    ROUTE(POST, "/session/:session_id/shadow/:shadow_id/elements"sv, find_elements_from_shadow_root),
    ROUTE(GET, "/session/:session_id/element/active"sv, get_active_element),
    ROUTE(GET, "/session/:session_id/element/:element_id/shadow"sv, get_element_shadow_root),
    ROUTE(GET, "/session/:session_id/element/:element_id/selected"sv, is_element_selected),
    ROUTE(GET, "/session/:session_id/element/:element_id/attribute/:name"sv, get_element_attribute),
    ROUTE(GET, "/session/:session_id/element/:element_id/property/:name"sv, get_element_property),
    ROUTE(GET, "/session/:session_id/element/:element_id/css/:name"sv, get_element_css_value),
    ROUTE(GET, "/session/:session_id/element/:element_id/text"sv, get_element_text),
    ROUTE(GET, "/session/:session_id/element/:element_id/name"sv, get_element_tag_name),
    ROUTE(GET, "/session/:session_id/element/:element_id/rect"sv, get_element_rect),
    ROUTE(GET, "/session/:session_id/element/:element_id/enabled"sv, is_element_enabled),
    ROUTE(GET, "/session/:session_id/element/:element_id/computedrole"sv, get_computed_role),
    ROUTE(GET, "/session/:session_id/element/:element_id/computedlabel"sv, get_computed_label),
    ROUTE(POST, "/session/:session_id/element/:element_id/click"sv, element_click),
    ROUTE(GET, "/session/:session_id/source"sv, get_source),
    ROUTE(POST, "/session/:session_id/execute/sync"sv, execute_script),
    ROUTE(POST, "/session/:session_id/execute/async"sv, execute_async_script),
    ROUTE(GET, "/session/:session_id/cookie"sv, get_all_cookies),
    ROUTE(GET, "/session/:session_id/cookie/:name"sv, get_named_cookie),
    ROUTE(POST, "/session/:session_id/cookie"sv, add_cookie),
    ROUTE(DELETE, "/session/:session_id/cookie/:name"sv, delete_cookie),
    ROUTE(DELETE, "/session/:session_id/cookie"sv, delete_all_cookies),
    ROUTE(DELETE, "/session/:session_id/actions"sv, release_actions),
    ROUTE(POST, "/session/:session_id/alert/dismiss"sv, dismiss_alert),
    ROUTE(POST, "/session/:session_id/alert/accept"sv, accept_alert),
    ROUTE(GET, "/session/:session_id/alert/text"sv, get_alert_text),
    ROUTE(POST, "/session/:session_id/alert/text"sv, send_alert_text),
    ROUTE(GET, "/session/:session_id/screenshot"sv, take_screenshot),
    ROUTE(GET, "/session/:session_id/element/:element_id/screenshot"sv, take_element_screenshot),
    ROUTE(POST, "/session/:session_id/print"sv, print_page),
};

// https://w3c.github.io/webdriver/#dfn-match-a-request
static ErrorOr<MatchedRoute, Error> match_route(HTTP::HttpRequest const& request)
{
    dbgln_if(WEBDRIVER_ROUTE_DEBUG, "match_route({}, {})", HTTP::to_string_view(request.method()), request.resource());

    auto request_path = request.resource().view();
    Vector<String> parameters;

    auto next_segment = [](auto& path) -> Optional<StringView> {
        if (auto index = path.find('/'); index.has_value() && (*index + 1) < path.length()) {
            path = path.substring_view(*index + 1);

            if (index = path.find('/'); index.has_value())
                return path.substring_view(0, *index);
            return path;
        }

        path = {};
        return {};
    };

    for (auto const& route : s_webdriver_endpoints) {
        dbgln_if(WEBDRIVER_ROUTE_DEBUG, "- Checking {} {}", HTTP::to_string_view(route.method), route.path);
        if (route.method != request.method())
            continue;

        auto route_path = route.path;
        Optional<bool> match;

        auto on_failed_match = [&]() {
            request_path = request.resource();
            parameters.clear();
            match = false;
        };

        while (!match.has_value()) {
            auto request_segment = next_segment(request_path);
            auto route_segment = next_segment(route_path);

            if (!request_segment.has_value() && !route_segment.has_value())
                match = true;
            else if (request_segment.has_value() != route_segment.has_value())
                on_failed_match();
            else if (route_segment->starts_with(':'))
                TRY(parameters.try_append(TRY(String::from_utf8(*request_segment))));
            else if (request_segment != route_segment)
                on_failed_match();
        }

        if (*match) {
            dbgln_if(WEBDRIVER_ROUTE_DEBUG, "- Found match with parameters={}", parameters);
            return MatchedRoute { route.handler, move(parameters) };
        }
    }

    return Error::from_code(ErrorCode::UnknownCommand, "The command was not recognized.");
}

static JsonValue make_success_response(JsonValue value)
{
    JsonObject result;
    result.set("value", move(value));
    return result;
}

Client::Client(NonnullOwnPtr<Core::BufferedTCPSocket> socket, Core::EventReceiver* parent)
    : Core::EventReceiver(parent)
    , m_socket(move(socket))
{
    m_socket->on_ready_to_read = [this] {
        if (auto result = on_ready_to_read(); result.is_error()) {
            result.error().visit(
                [](AK::Error const& error) {
                    warnln("Internal error: {}", error);
                },
                [](HTTP::HttpRequest::ParseError const& error) {
                    warnln("HTTP request parsing error: {}", HTTP::HttpRequest::parse_error_to_string(error));
                },
                [this](WebDriver::Error const& error) {
                    if (send_error_response(error).is_error())
                        warnln("Could not send error response");
                });

            die();
        }

        m_request = {};
    };
}

Client::~Client()
{
    m_socket->close();
}

void Client::die()
{
    deferred_invoke([this] { remove_from_parent(); });
}

ErrorOr<void, Client::WrappedError> Client::on_ready_to_read()
{
    // FIXME: All this should be moved to LibHTTP and be made spec compliant.
    auto buffer = TRY(ByteBuffer::create_uninitialized(m_socket->buffer_size()));

    for (;;) {
        if (!TRY(m_socket->can_read_without_blocking()))
            break;

        auto data = TRY(m_socket->read_some(buffer));
        TRY(m_remaining_request.try_append(StringView { data }));

        if (m_socket->is_eof()) {
            die();
            break;
        }
    }

    if (m_remaining_request.is_empty())
        return {};

    auto maybe_parsed_request = HTTP::HttpRequest::from_raw_request(TRY(m_remaining_request.to_byte_buffer()));
    if (maybe_parsed_request.is_error()) {
        if (maybe_parsed_request.error() == HTTP::HttpRequest::ParseError::RequestIncomplete) {
            // If request is not complete we need to wait for more data to arrive
            return {};
        }
        return maybe_parsed_request.error();
    }

    m_remaining_request.clear();
    m_request = maybe_parsed_request.value();

    auto body = TRY(read_body_as_json());
    TRY(handle_request(move(body)));

    return {};
}

ErrorOr<JsonValue, Client::WrappedError> Client::read_body_as_json()
{
    // FIXME: If we received a multipart body here, this would fail badly.
    // FIXME: Check the Content-Type is actually application/json.
    size_t content_length = 0;

    for (auto const& header : m_request->headers().headers()) {
        if (header.name.equals_ignoring_ascii_case("Content-Length"sv)) {
            content_length = header.value.to_number<size_t>(TrimWhitespace::Yes).value_or(0);
            break;
        }
    }

    if (content_length == 0)
        return JsonValue {};

    JsonParser json_parser(m_request->body());
    return TRY(json_parser.parse());
}

ErrorOr<void, Client::WrappedError> Client::handle_request(JsonValue body)
{
    if constexpr (WEBDRIVER_DEBUG) {
        dbgln("Got HTTP request: {} {}", m_request->method_name(), m_request->resource());
        dbgln("Body: {}", body);
    }

    auto [handler, parameters] = TRY(match_route(*m_request));
    auto result = TRY((*handler)(*this, move(parameters), move(body)));
    return send_success_response(move(result));
}

ErrorOr<void, Client::WrappedError> Client::send_success_response(JsonValue result)
{
    bool keep_alive = false;
    if (auto it = m_request->headers().headers().find_if([](auto& header) { return header.name.equals_ignoring_ascii_case("Connection"sv); }); !it.is_end())
        keep_alive = it->value.trim_whitespace().equals_ignoring_ascii_case("keep-alive"sv);

    result = make_success_response(move(result));
    auto content = result.serialized<StringBuilder>();

    StringBuilder builder;
    builder.append("HTTP/1.0 200 OK\r\n"sv);
    builder.append("Server: WebDriver (SerenityOS)\r\n"sv);
    builder.append("X-Frame-Options: SAMEORIGIN\r\n"sv);
    builder.append("X-Content-Type-Options: nosniff\r\n"sv);
    builder.append("Pragma: no-cache\r\n"sv);
    if (keep_alive)
        builder.append("Connection: keep-alive\r\n"sv);
    builder.append("Content-Type: application/json; charset=utf-8\r\n"sv);
    builder.appendff("Content-Length: {}\r\n", content.length());
    builder.append("\r\n"sv);

    auto builder_contents = TRY(builder.to_byte_buffer());
    TRY(m_socket->write_until_depleted(builder_contents));

    while (!content.is_empty()) {
        auto bytes_sent = TRY(m_socket->write_some(content.bytes()));
        content = content.substring_view(bytes_sent);
    }

    if (!keep_alive)
        die();

    log_response(200);
    return {};
}

ErrorOr<void, Client::WrappedError> Client::send_error_response(Error const& error)
{
    // FIXME: Implement to spec.
    dbgln_if(WEBDRIVER_DEBUG, "Sending error response: {} {}: {}", error.http_status, error.error, error.message);
    auto reason = HTTP::HttpResponse::reason_phrase_for_code(error.http_status);

    JsonObject error_response;
    error_response.set("error", error.error);
    error_response.set("message", error.message);
    error_response.set("stacktrace", "");
    if (error.data.has_value())
        error_response.set("data", *error.data);

    JsonObject result;
    result.set("value", move(error_response));

    StringBuilder content_builder;
    result.serialize(content_builder);

    StringBuilder header_builder;
    header_builder.appendff("HTTP/1.0 {} {}\r\n", error.http_status, reason);
    header_builder.append("Content-Type: application/json; charset=UTF-8\r\n"sv);
    header_builder.appendff("Content-Length: {}\r\n", content_builder.length());
    header_builder.append("\r\n"sv);

    TRY(m_socket->write_until_depleted(TRY(header_builder.to_byte_buffer())));
    TRY(m_socket->write_until_depleted(TRY(content_builder.to_byte_buffer())));

    log_response(error.http_status);
    return {};
}

void Client::log_response(unsigned code)
{
    outln("{} :: {:03d} :: {} {}", Core::DateTime::now().to_byte_string(), code, m_request->method_name(), m_request->resource());
}

}
