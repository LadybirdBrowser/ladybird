/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/GenericLexer.h>
#include <AK/JsonArray.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibGC/Heap.h>
#include <LibRequests/Request.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/Network.h>

namespace Web::WebDriver::BiDi {

using namespace Fetch::Infrastructure;

GC_DEFINE_ALLOCATOR(NetworkRequest);

bool g_network_events_enabled { false };

NetworkRequest::NetworkRequest()
    : id(generate_random_uuid())
{
}

void initialize_network_request(Request& request, GC::Ptr<HTML::Navigable> navigable, Optional<Utf16String> const& navigation)
{
    if (!g_network_events_enabled || request.webdriver_bidi_network_request())
        return;
    auto state = GC::Heap::the().allocate<NetworkRequest>();
    if (navigable) {
        state->navigable = navigable;
    } else if (auto client = request.client()) {
        if (auto* window = HTML::window_from_global_object(client->global_object()))
            state->navigable = window->associated_document().navigable();
    }
    if (navigation.has_value())
        state->navigation = navigation->to_utf8();
    request.set_webdriver_bidi_network_request(state);
}

// https://w3c.github.io/webdriver-bidi/#serialize-protocol-bytes
static JsonObject serialize_protocol_bytes(StringView bytes)
{
    JsonObject result;
    if (auto string = String::from_utf8(bytes); !string.is_error()) {
        result.set("type"sv, "string"sv);
        result.set("value"sv, string.release_value());
    } else {
        result.set("type"sv, "base64"sv);
        result.set("value"sv, MUST(encode_base64(bytes.bytes())));
    }
    return result;
}

// https://w3c.github.io/webdriver-bidi/#serialize-header
static JsonArray serialize_headers(HTTP::HeaderList const& headers)
{
    JsonArray result;
    for (auto const& header : headers) {
        JsonObject entry;
        // 1. Let name be the result of UTF-8 decode with name bytes.
        entry.set("name"sv, String::from_utf8_with_replacement_character(header.name.view()));
        // 2. Let value be serialize protocol bytes with value bytes.
        entry.set("value"sv, serialize_protocol_bytes(header.value));
        MUST(result.append(move(entry)));
    }
    return result;
}

static u64 headers_size(HTTP::HeaderList const& headers)
{
    u64 size = 2;
    for (auto const& header : headers)
        size += header.name.length() + header.value.length() + 4;
    return size;
}

// https://w3c.github.io/webdriver-bidi/#get-the-fetch-timings
static JsonObject get_the_fetch_timings(FetchTimingInfo const& timings, Optional<double> time_origin, double epoch_origin)
{
    JsonObject result;
    result.set("timeOrigin"sv, epoch_origin);
    auto convert = [&](double time) {
        if (!time_origin.has_value() || time == 0)
            return 0.0;
        return HighResolutionTime::coarsen_time(time) - *time_origin;
    };
    auto connection = timings.final_connection_timing_info().value_or(ConnectionTimingInfo {});
    result.set("requestTime"sv, convert(timings.start_time()));
    result.set("redirectStart"sv, convert(timings.redirect_start_time()));
    result.set("redirectEnd"sv, convert(timings.redirect_end_time()));
    result.set("fetchStart"sv, convert(timings.post_redirect_start_time()));
    result.set("dnsStart"sv, convert(connection.domain_lookup_start_time));
    result.set("dnsEnd"sv, convert(connection.domain_lookup_end_time));
    result.set("connectStart"sv, convert(connection.connection_start_time));
    result.set("connectEnd"sv, convert(connection.connection_end_time));
    result.set("tlsStart"sv, convert(connection.secure_connection_start_time));
    result.set("requestStart"sv, convert(timings.final_network_request_start_time()));
    result.set("responseStart"sv, convert(timings.final_network_response_start_time()));
    result.set("responseEnd"sv, convert(timings.end_time()));
    return result;
}

static JsonObject get_the_fetch_timings(Request const& request, FetchTimingInfo const& timings)
{
    auto client = request.client();
    return get_the_fetch_timings(timings, client ? Optional<double>(client->time_origin()) : Optional<double> {}, client ? HighResolutionTime::get_time_origin_timestamp(client->global_object()) : 0);
}

// https://w3c.github.io/webdriver-bidi/#get-the-request-data
static JsonObject get_the_request_data(Request const& request, FetchTimingInfo const& timings)
{
    JsonObject result;
    result.set("request"sv, request.webdriver_bidi_network_request()->id);
    result.set("url"sv, request.current_url().serialize());
    result.set("method"sv, MUST(String::from_utf8(request.method().view())));
    result.set("headers"sv, serialize_headers(request.header_list()));
    // FIXME: Include the cookies attached by RequestServer.
    result.set("cookies"sv, JsonArray {});
    result.set("headersSize"sv, headers_size(request.header_list()));
    JsonValue body_size;
    if (auto const* bytes = request.body().get_pointer<ByteBuffer>())
        body_size = bytes->size();
    else if (auto const* body = request.body().get_pointer<GC::Ref<Body>>(); body && (*body)->length().has_value())
        body_size = *(*body)->length();
    result.set("bodySize"sv, move(body_size));
    result.set("destination"sv, request.destination().has_value() ? request_destination_to_string(*request.destination()) : ""sv);
    result.set("initiatorType"sv, request.initiator_type().has_value() ? JsonValue(initiator_type_to_string(*request.initiator_type()).to_utf16_string().to_utf8()) : JsonValue {});
    result.set("timings"sv, get_the_fetch_timings(request, timings));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#extract-challenges
static JsonArray extract_challenges(Response const& response)
{
    JsonArray challenges;
    auto header_name = response.status() == 407 ? "Proxy-Authenticate"sv : "WWW-Authenticate"sv;
    for (auto const& header : *response.header_list()) {
        if (!header.name.equals_ignoring_ascii_case(header_name))
            continue;
        GenericLexer lexer(header.value.view());
        Optional<JsonObject> challenge;
        while (!lexer.is_eof()) {
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t' || ch == ','; });
            auto name = lexer.consume_while([](char ch) { return HTTP::is_header_name({ &ch, 1 }); });
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t'; });
            if (name.is_empty()) {
                if (!lexer.is_eof())
                    lexer.ignore();
                continue;
            }
            if (!lexer.consume_specific('=')) {
                if (challenge.has_value())
                    MUST(challenges.append(challenge.release_value()));
                challenge = JsonObject {};
                challenge->set("scheme"sv, MUST(String::from_utf8(name)));
                challenge->set("realm"sv, ""sv);
                continue;
            }
            lexer.ignore_while([](char ch) { return ch == ' ' || ch == '\t'; });
            StringBuilder value;
            if (lexer.consume_specific('"')) {
                while (!lexer.is_eof() && lexer.peek() != '"') {
                    if (lexer.consume_specific('\\') && lexer.is_eof())
                        break;
                    value.append(lexer.consume());
                }
                lexer.consume_specific('"');
            } else {
                value.append(lexer.consume_until([](char ch) { return ch == ',' || ch == ' ' || ch == '\t'; }));
            }
            if (challenge.has_value() && name.equals_ignoring_ascii_case("realm"sv))
                challenge->set("realm"sv, String::from_utf8_with_replacement_character(value.string_view()));
        }
        if (challenge.has_value())
            MUST(challenges.append(challenge.release_value()));
    }
    return challenges;
}

// https://w3c.github.io/webdriver-bidi/#get-the-response-data
static JsonObject get_the_response_data(Response const& response, FetchTimingInfo const& timings)
{
    JsonObject result;
    auto url = response.url();
    result.set("url"sv, url.has_value() ? url->serialize() : String {});
    ByteString protocol;
    if (timings.final_connection_timing_info().has_value())
        protocol = timings.final_connection_timing_info()->alpn_negotiated_protocol;
    if (protocol.is_empty()) {
        if (auto const& network_request = response.request_server_request(); network_request.has_value() && network_request->request)
            protocol = network_request->request->http_version();
    }
    if (protocol.is_empty())
        protocol = url.has_value() ? url->scheme().to_byte_string() : ByteString {};
    if (protocol == "http/2"sv)
        protocol = "h2"sv;
    else if (protocol == "http/3"sv)
        protocol = "h3"sv;
    result.set("protocol"sv, MUST(String::from_utf8(protocol.view())));
    result.set("status"sv, response.status());
    // INTEROP: Data URL responses have an "OK" status text in Chromium and Firefox.
    auto status_text = response.status_message();
    if (status_text.is_empty() && url.has_value() && url->scheme() == "data"sv)
        status_text = "OK"sv;
    result.set("statusText"sv, String::from_utf8_with_replacement_character(status_text.view()));
    result.set("fromCache"sv, response.cache_state() == Response::CacheState::Local);
    result.set("headers"sv, serialize_headers(response.header_list()));
    auto mime_type = extract_mime_type(response.header_list());
    result.set("mimeType"sv, mime_type.has_value() ? mime_type->essence() : String {});
    auto body_info = response.body_info();
    if (auto body = response.body(); body && body->source().has_value() && body_info.decoded_size == 0) {
        body_info.decoded_size = body->source()->size();
        if (url.has_value() && url->scheme() == "data"sv)
            body_info.encoded_size = body_info.decoded_size;
    }
    auto header_size = headers_size(response.header_list());
    result.set("bytesReceived"sv, body_info.encoded_size + header_size);
    result.set("headersSize"sv, header_size);
    result.set("bodySize"sv, body_info.encoded_size);
    JsonObject content;
    content.set("size"sv, body_info.decoded_size);
    result.set("content"sv, move(content));
    if (response.status() == 401 || response.status() == 407)
        result.set("authChallenges"sv, extract_challenges(response));
    return result;
}

// https://w3c.github.io/webdriver-bidi/#process-a-network-event
static JsonObject process_a_network_event(Request const& request, FetchTimingInfo const& timing_info)
{
    auto state = request.webdriver_bidi_network_request();
    JsonObject params;
    params.set("context"sv, navigable_id(*state->navigable));
    params.set("userContext"sv, "default"sv);
    params.set("isBlocked"sv, false);
    params.set("navigation"sv, state->navigation.has_value() ? JsonValue(*state->navigation) : JsonValue {});
    params.set("redirectCount"sv, request.redirect_count());
    JsonObject request_data;
    if (auto cached = state->request_data.get(request.redirect_count()); cached.has_value())
        request_data = *cached;
    else
        request_data = get_the_request_data(request, timing_info);
    request_data.set("timings"sv, get_the_fetch_timings(request, timing_info));
    params.set("request"sv, move(request_data));
    params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    return params;
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-before-request-sent
void network_before_request_sent(FetchParams const& fetch_params)
{
    auto request = fetch_params.request();
    initialize_network_request(*request);
    auto state = request->webdriver_bidi_network_request();
    if (!state || !state->navigable || state->before_request_sent.contains(request->redirect_count()))
        return;
    // 4. Let redirect count be request's redirect count.
    // 5. Add redirect count to before request sent map[request].
    state->before_request_sent.set(request->redirect_count());
    state->request_data.set(request->redirect_count(), get_the_request_data(*request, *fetch_params.timing_info()));
    if (is_event_enabled("network.beforeRequestSent"sv))
        emit_event(*state->navigable, "network.beforeRequestSent"_string, process_a_network_event(*fetch_params.request(), *fetch_params.timing_info()));
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseStarted
void network_response_started(FetchParams const& fetch_params, Response& response)
{
    network_before_request_sent(fetch_params);
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (!state || !state->navigable || state->response_started.contains(request->redirect_count()) || response.is_network_error())
        return;
    state->response_started.set(request->redirect_count());
    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseStarted" and related navigables.
    if (is_event_enabled("network.responseStarted"sv)) {
        auto params = process_a_network_event(*fetch_params.request(), *fetch_params.timing_info());
        params.set("response"sv, get_the_response_data(response, *fetch_params.timing_info()));
        emit_event(*state->navigable, "network.responseStarted"_string, move(params));
    }
}

// https://w3c.github.io/webdriver-bidi/#event-network-responseCompleted
void network_response_completed(FetchParams const& fetch_params, Response const& response)
{
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (!state || !state->navigable || (state->completed.contains(request->redirect_count()) || state->transferred.contains(request->redirect_count())))
        return;
    state->completed.set(request->redirect_count());
    if (!fetch_params.timing_info()->end_time())
        fetch_params.timing_info()->set_end_time(HighResolutionTime::unsafe_shared_current_time());
    // 5. Let sessions be the set of sessions for which an event is enabled given "network.responseCompleted" and related navigables.
    if (!is_event_enabled("network.responseCompleted"sv))
        return;
    auto params = process_a_network_event(*fetch_params.request(), *fetch_params.timing_info());
    params.set("response"sv, get_the_response_data(response, *fetch_params.timing_info()));
    emit_event(*state->navigable, "network.responseCompleted"_string, move(params));
}

// https://w3c.github.io/webdriver-bidi/#event-network-fetchError
void network_fetch_error(FetchParams const& fetch_params, StringView error_text)
{
    // 1. If before request sent map[request] does not contain request's redirect count, then run the WebDriver BiDi before request sent steps with request.
    network_before_request_sent(fetch_params);
    auto request = fetch_params.request();
    auto state = request->webdriver_bidi_network_request();
    if (!state || !state->navigable || (state->completed.contains(request->redirect_count()) || state->transferred.contains(request->redirect_count())))
        return;
    state->completed.set(request->redirect_count());
    if (!is_event_enabled("network.fetchError"sv))
        return;
    auto params = process_a_network_event(*fetch_params.request(), *fetch_params.timing_info());
    // 4.2. Set the errorText field of params to an implementation-defined string describing the error which caused the request to be aborted.
    params.set("errorText"sv, String::from_utf8_with_replacement_character(error_text));
    emit_event(*state->navigable, "network.fetchError"_string, move(params));
}

Optional<JsonObject> network_response_for_navigation(Request const& request, FetchTimingInfo const& timing_info,
    Response const& response)
{
    if (!g_network_events_enabled)
        return {};
    auto state = request.webdriver_bidi_network_request();
    if (!state || !state->navigable || state->completed.contains(request.redirect_count()))
        return {};
    state->transferred.set(request.redirect_count());
    JsonObject result;
    result.set("parameters"sv, process_a_network_event(request, timing_info));
    result.set("response"sv, get_the_response_data(response, timing_info));
    result.set("postRedirectStartTime"sv, timing_info.post_redirect_start_time());
    result.set("timeOrigin"sv, request.client() ? JsonValue(request.client()->time_origin()) : JsonValue {});
    return result;
}

void network_navigation_response_finished(HTML::Navigable const& navigable, JsonObject const& metadata, Response const& response, FetchTimingInfo const* timing_info, Optional<StringView> error)
{
    auto name = error.has_value() ? "network.fetchError"sv : "network.responseCompleted"sv;
    if (!g_network_events_enabled || !is_event_enabled(name))
        return;
    auto parameters = metadata.get_object("parameters"sv);
    if (!parameters.has_value())
        return;
    auto params = *parameters;
    params.set("timestamp"sv, UnixDateTime::now().milliseconds_since_epoch());
    if (error.has_value()) {
        params.set("errorText"sv, String::from_utf8_with_replacement_character(*error));
    } else {
        auto fallback_timings = FetchTimingInfo::create();
        auto const& timings = timing_info ? *timing_info : *fallback_timings;
        if (auto request = params.get_object("request"sv); request.has_value()) {
            auto old_timings = request->get_object("timings"sv);
            auto epoch_origin = old_timings->get("timeOrigin"sv)->get_double_with_precision_loss().value();
            auto origin = metadata.get("timeOrigin"sv);
            request->set("timings"sv, get_the_fetch_timings(timings, origin->is_null() ? Optional<double> {} : origin->get_double_with_precision_loss(), epoch_origin));
        }
        params.set("response"sv, get_the_response_data(response, timings));
    }
    emit_event(navigable, MUST(String::from_utf8(name)), move(params));
}

}
