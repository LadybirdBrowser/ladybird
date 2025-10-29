/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <LibCore/System.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>

namespace Requests {

RequestClient::RequestClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<RequestClientEndpoint, RequestServerEndpoint>(*this, move(transport))
{
}

RequestClient::~RequestClient() = default;

void RequestClient::die()
{
    for (auto& [id, request] : m_requests) {
        if (request)
            request->did_finish({}, {}, {}, NetworkError::RequestServerDied);
    }

    m_requests.clear();
}

void RequestClient::ensure_connection(URL::URL const& url, ::RequestServer::CacheLevel cache_level)
{
    async_ensure_connection(url, cache_level);
}

RefPtr<Request> RequestClient::start_request(ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& request_headers, ReadonlyBytes request_body, Core::ProxyData const& proxy_data, u64 page_id)
{
    auto body_result = ByteBuffer::copy(request_body);
    if (body_result.is_error())
        return nullptr;

    static i32 s_next_request_id = 0;
    auto request_id = s_next_request_id++;

    IPCProxy::async_start_request(request_id, method, url, request_headers, body_result.release_value(), proxy_data, page_id);
    auto request = Request::create_from_id({}, *this, request_id);
    m_requests.set(request_id, request);
    return request;
}

void RequestClient::request_started(i32 request_id, IPC::File response_file)
{
    auto request = m_requests.get(request_id);
    if (!request.has_value()) {
        warnln("Received response for non-existent request {}", request_id);
        return;
    }

    auto response_fd = response_file.take_fd();
    request.value()->set_request_fd({}, response_fd);
}

bool RequestClient::stop_request(Badge<Request>, Request& request)
{
    if (!m_requests.contains(request.id()))
        return false;
    return IPCProxy::stop_request(request.id());
}

bool RequestClient::set_certificate(Badge<Request>, Request& request, ByteString certificate, ByteString key)
{
    if (!m_requests.contains(request.id()))
        return false;
    return IPCProxy::set_certificate(request.id(), move(certificate), move(key));
}

void RequestClient::request_finished(i32 request_id, u64 total_size, RequestTimingInfo timing_info, Optional<NetworkError> network_error)
{
    RefPtr<Request> request;
    if ((request = m_requests.get(request_id).value_or(nullptr))) {
        request->did_finish({}, total_size, timing_info, network_error);
    }
    m_requests.remove(request_id);
}

void RequestClient::headers_became_available(i32 request_id, HTTP::HeaderMap response_headers, Optional<u32> status_code, Optional<String> reason_phrase)
{
    auto request = const_cast<Request*>(m_requests.get(request_id).value_or(nullptr));
    if (!request) {
        warnln("Received headers for non-existent request {}", request_id);
        return;
    }
    request->did_receive_headers({}, response_headers, status_code, reason_phrase);
}

void RequestClient::security_alert(i32 request_id, u64 page_id, ByteString alert_json)
{
    auto request = const_cast<Request*>(m_requests.get(request_id).value_or(nullptr));
    if (!request) {
        warnln("Received security alert for non-existent request {}", request_id);
        return;
    }

    dbgln("RequestClient: Security threat detected in download (request {}, page_id {})", request_id, page_id);
    dbgln("Alert details: {}", alert_json);

    // Parse the alert JSON to log specific details
    auto json_result = JsonValue::from_string(alert_json);
    if (!json_result.is_error() && json_result.value().is_object()) {
        auto alert_obj = json_result.value().as_object();
        if (auto matched_rules = alert_obj.get_array("matched_rules"sv); matched_rules.has_value()) {
            dbgln("Matched {} YARA rule(s):", matched_rules->size());
            for (auto const& rule : matched_rules->values()) {
                if (rule.is_object()) {
                    auto rule_obj = rule.as_object();
                    auto rule_name = rule_obj.get_string("rule_name"sv).value_or("Unknown"_string);
                    auto severity = rule_obj.get_string("severity"sv).value_or("unknown"_string);
                    auto description = rule_obj.get_string("description"sv).value_or("No description"_string);
                    dbgln("  - {} [{}]: {}", rule_name, severity, description);
                }
            }
        }
    }

    // Call the on_security_alert callback if set (routes to ViewImplementation → Tab)
    if (request->on_security_alert) {
        request->on_security_alert(alert_json, request_id);
    }
}

void RequestClient::certificate_requested(i32 request_id)
{
    if (auto request = const_cast<Request*>(m_requests.get(request_id).value_or(nullptr))) {
        request->did_request_certificates({});
    }
}

RefPtr<WebSocket> RequestClient::websocket_connect(URL::URL const& url, ByteString const& origin, Vector<ByteString> const& protocols, Vector<ByteString> const& extensions, HTTP::HeaderMap const& request_headers)
{
    auto websocket_id = m_next_websocket_id++;
    IPCProxy::async_websocket_connect(websocket_id, url, origin, protocols, extensions, request_headers);
    auto connection = WebSocket::create_from_id({}, *this, websocket_id);
    m_websockets.set(websocket_id, connection);
    return connection;
}

void RequestClient::websocket_connected(i64 websocket_id)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value())
        maybe_connection.value()->did_open({});
}

void RequestClient::websocket_received(i64 websocket_id, bool is_text, ByteBuffer data)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value())
        maybe_connection.value()->did_receive({}, move(data), is_text);
}

void RequestClient::websocket_errored(i64 websocket_id, i32 message)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value())
        maybe_connection.value()->did_error({}, message);
}

void RequestClient::websocket_closed(i64 websocket_id, u16 code, ByteString reason, bool clean)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value())
        maybe_connection.value()->did_close({}, code, move(reason), clean);
}

void RequestClient::websocket_ready_state_changed(i64 websocket_id, u32 ready_state)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value()) {
        VERIFY(ready_state <= static_cast<u32>(WebSocket::ReadyState::Closed));
        maybe_connection.value()->set_ready_state(static_cast<WebSocket::ReadyState>(ready_state));
    }
}

void RequestClient::websocket_subprotocol(i64 websocket_id, ByteString subprotocol)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value()) {
        maybe_connection.value()->set_subprotocol_in_use(move(subprotocol));
    }
}

void RequestClient::websocket_certificate_requested(i64 websocket_id)
{
    auto maybe_connection = m_websockets.get(websocket_id);
    if (maybe_connection.has_value())
        maybe_connection.value()->did_request_certificates({});
}

}
