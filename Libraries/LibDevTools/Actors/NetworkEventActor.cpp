/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/NetworkEventActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<NetworkEventActor> NetworkEventActor::create(DevToolsServer& devtools, String name, u64 request_id)
{
    return adopt_ref(*new NetworkEventActor(devtools, move(name), request_id));
}

NetworkEventActor::NetworkEventActor(DevToolsServer& devtools, String name, u64 request_id)
    : Actor(devtools, move(name))
    , m_request_id(request_id)
{
}

NetworkEventActor::~NetworkEventActor() = default;

void NetworkEventActor::set_request_info(String url, String method, UnixDateTime start_time, Vector<HTTP::Header> request_headers)
{
    m_url = move(url);
    m_method = move(method);
    m_start_time = start_time;
    m_request_headers = move(request_headers);
}

void NetworkEventActor::set_response_start(u32 status_code, Optional<String> reason_phrase)
{
    m_status_code = status_code;
    m_reason_phrase = move(reason_phrase);
}

void NetworkEventActor::set_response_headers(Vector<HTTP::Header> response_headers)
{
    m_response_headers = move(response_headers);
}

void NetworkEventActor::set_request_complete(u64 body_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error)
{
    m_body_size = body_size;
    m_timing_info = timing_info;
    m_network_error = network_error;
    m_complete = true;
}

JsonObject NetworkEventActor::serialize_initial_event() const
{
    // FIXME: Detect actual cause type (xhr, fetch, script, stylesheet, image, etc.)
    JsonObject cause;
    cause.set("type"sv, "document"sv);

    JsonObject event;
    event.set("resourceType"sv, "network-event"sv);
    event.set("resourceId"sv, static_cast<i64>(m_request_id));
    event.set("actor"sv, name());
    event.set("startedDateTime"sv, MUST(m_start_time.to_string("%Y-%m-%dT%H:%M:%S.000Z"sv)));
    event.set("timeStamp"sv, m_start_time.milliseconds_since_epoch());
    event.set("url"sv, m_url);
    event.set("method"sv, m_method);
    // FIXME: Detect if request is XHR/fetch
    event.set("isXHR"sv, false);
    event.set("cause"sv, move(cause));
    event.set("private"sv, false);
    // FIXME: Detect if response is from cache
    event.set("fromCache"sv, false);
    event.set("fromServiceWorker"sv, false);
    event.set("isThirdPartyTrackingResource"sv, false);
    // FIXME: Get actual referrer policy from request
    event.set("referrerPolicy"sv, "strict-origin-when-cross-origin"sv);
    event.set("blockedReason"sv, 0);
    event.set("blockingExtension"sv, JsonValue {});
    event.set("channelId"sv, static_cast<i64>(m_request_id));
    // FIXME: Get actual browsing context ID from the page
    event.set("browsingContextID"sv, 1);
    // FIXME: Get actual inner window ID
    event.set("innerWindowId"sv, 1);
    // FIXME: Get request priority
    event.set("priority"sv, 0);
    // FIXME: Detect if this is a navigation request
    event.set("isNavigationRequest"sv, false);
    event.set("chromeContext"sv, false);

    return event;
}

void NetworkEventActor::handle_message(Message const& message)
{
    if (message.type == "getRequestHeaders"sv) {
        get_request_headers(message);
        return;
    }

    if (message.type == "getRequestCookies"sv) {
        get_request_cookies(message);
        return;
    }

    if (message.type == "getRequestPostData"sv) {
        get_request_post_data(message);
        return;
    }

    if (message.type == "getResponseHeaders"sv) {
        get_response_headers(message);
        return;
    }

    if (message.type == "getResponseCookies"sv) {
        get_response_cookies(message);
        return;
    }

    if (message.type == "getResponseContent"sv) {
        get_response_content(message);
        return;
    }

    if (message.type == "getEventTimings"sv) {
        get_event_timings(message);
        return;
    }

    if (message.type == "getSecurityInfo"sv) {
        get_security_info(message);
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void NetworkEventActor::get_request_headers(Message const& message)
{
    JsonArray headers;
    i64 header_size = 0;

    for (auto const& header : m_request_headers) {
        JsonObject header_obj;
        header_obj.set("name"sv, MUST(String::from_byte_string(header.name)));
        header_obj.set("value"sv, MUST(String::from_byte_string(header.value)));
        headers.must_append(move(header_obj));
        header_size += static_cast<i64>(header.name.bytes().size() + header.value.bytes().size() + 4); // ": " and "\r\n"
    }

    JsonObject response;
    response.set("headers"sv, move(headers));
    response.set("headersSize"sv, header_size);
    response.set("rawHeaders"sv, String {});
    send_response(message, move(response));
}

void NetworkEventActor::get_request_cookies(Message const& message)
{
    JsonObject response;
    response.set("cookies"sv, JsonArray {});
    send_response(message, move(response));
}

void NetworkEventActor::get_request_post_data(Message const& message)
{
    JsonObject post_data;
    post_data.set("text"sv, String {});

    JsonObject response;
    response.set("postData"sv, move(post_data));
    response.set("postDataDiscarded"sv, false);
    send_response(message, move(response));
}

void NetworkEventActor::get_response_headers(Message const& message)
{
    JsonArray headers;
    i64 header_size = 0;

    for (auto const& header : m_response_headers) {
        JsonObject header_obj;
        header_obj.set("name"sv, MUST(String::from_byte_string(header.name)));
        header_obj.set("value"sv, MUST(String::from_byte_string(header.value)));
        headers.must_append(move(header_obj));
        header_size += static_cast<i64>(header.name.bytes().size() + header.value.bytes().size() + 4);
    }

    JsonObject response;
    response.set("headers"sv, move(headers));
    response.set("headersSize"sv, header_size);
    response.set("rawHeaders"sv, String {});
    send_response(message, move(response));
}

void NetworkEventActor::get_response_cookies(Message const& message)
{
    JsonObject response;
    response.set("cookies"sv, JsonArray {});
    send_response(message, move(response));
}

void NetworkEventActor::get_response_content(Message const& message)
{
    // FIXME: Store and return actual response body content
    JsonObject content;
    content.set("text"sv, String {});
    // FIXME: Get actual MIME type from response headers
    content.set("mimeType"sv, "text/html"sv);
    content.set("size"sv, static_cast<i64>(m_body_size));

    JsonObject response;
    response.set("content"sv, move(content));
    response.set("contentDiscarded"sv, true);
    send_response(message, move(response));
}

void NetworkEventActor::get_event_timings(Message const& message)
{
    // Convert microseconds to milliseconds for HAR format
    auto dns_time = (m_timing_info.domain_lookup_end_microseconds - m_timing_info.domain_lookup_start_microseconds) / 1000;
    auto connect_time = (m_timing_info.connect_end_microseconds - m_timing_info.connect_start_microseconds) / 1000;
    auto ssl_time = m_timing_info.secure_connect_start_microseconds > 0
        ? (m_timing_info.connect_end_microseconds - m_timing_info.secure_connect_start_microseconds) / 1000
        : 0;
    auto send_time = (m_timing_info.response_start_microseconds - m_timing_info.request_start_microseconds) / 1000;
    // FIXME: Calculate actual time waiting for server response (TTFB)
    auto wait_time = 0;
    auto receive_time = (m_timing_info.response_end_microseconds - m_timing_info.response_start_microseconds) / 1000;

    JsonObject timings;
    timings.set("blocked"sv, 0);
    timings.set("dns"sv, dns_time);
    timings.set("connect"sv, connect_time);
    timings.set("ssl"sv, ssl_time);
    timings.set("send"sv, send_time);
    timings.set("wait"sv, wait_time);
    timings.set("receive"sv, receive_time);

    auto total_time = dns_time + connect_time + send_time + wait_time + receive_time;

    JsonObject response;
    response.set("timings"sv, move(timings));
    response.set("totalTime"sv, total_time);
    response.set("offsets"sv, JsonObject {});
    send_response(message, move(response));
}

void NetworkEventActor::get_security_info(Message const& message)
{
    // FIXME: Get actual TLS/SSL security information from the connection
    JsonObject response;
    response.set("securityInfo"sv, JsonObject {});
    response.set("state"sv, "insecure"sv);
    send_response(message, move(response));
}

}
