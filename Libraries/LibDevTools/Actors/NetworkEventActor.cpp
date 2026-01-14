/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
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

void NetworkEventActor::set_request_info(String url, String method, UnixDateTime start_time, Vector<HTTP::Header> request_headers, ByteBuffer request_body)
{
    m_url = move(url);
    m_method = move(method);
    m_start_time = start_time;
    m_request_headers = move(request_headers);
    m_request_body = move(request_body);
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

void NetworkEventActor::append_response_body(ByteBuffer data)
{
    // Limit response body size to prevent memory issues
    if (m_response_body.size() >= MAX_RESPONSE_BODY_SIZE)
        return;

    auto remaining_capacity = MAX_RESPONSE_BODY_SIZE - m_response_body.size();
    auto bytes_to_append = min(data.size(), remaining_capacity);

    if (bytes_to_append > 0)
        m_response_body.append(data.bytes().slice(0, bytes_to_append));
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
    post_data.set("text"sv, MUST(String::from_utf8(m_request_body)));

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
    // Get MIME type from Content-Type header
    String mime_type = "application/octet-stream"_string;
    for (auto const& header : m_response_headers) {
        if (header.name.equals_ignoring_ascii_case("content-type"sv)) {
            auto content_type = StringView { header.value };
            // Extract just the MIME type, ignoring charset etc.
            if (auto semicolon = content_type.find(';'); semicolon.has_value())
                content_type = content_type.substring_view(0, *semicolon);
            mime_type = MUST(String::from_utf8(content_type.trim_whitespace()));
            break;
        }
    }

    bool content_discarded = m_response_body.size() >= MAX_RESPONSE_BODY_SIZE;

    // Check if content is text-based (can be displayed as UTF-8)
    bool is_text = mime_type.starts_with_bytes("text/"sv)
        || mime_type == "application/json"sv
        || mime_type == "application/javascript"sv
        || mime_type == "application/xml"sv
        || mime_type.ends_with_bytes("+xml"sv)
        || mime_type.ends_with_bytes("+json"sv);

    JsonObject content;
    if (is_text) {
        // Try to interpret as UTF-8, fall back to empty if invalid
        auto text_or_error = String::from_utf8(m_response_body);
        content.set("text"sv, text_or_error.is_error() ? String {} : text_or_error.release_value());
        content.set("encoding"sv, JsonValue {});
    } else {
        // Base64 encode binary content
        content.set("text"sv, MUST(encode_base64(m_response_body)));
        content.set("encoding"sv, "base64"sv);
    }
    content.set("mimeType"sv, mime_type);
    content.set("size"sv, static_cast<i64>(m_body_size));

    JsonObject response;
    response.set("content"sv, move(content));
    response.set("contentDiscarded"sv, content_discarded);
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
