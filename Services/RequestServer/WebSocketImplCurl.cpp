/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Notifier.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/WebSocketImplCurl.h>

namespace RequestServer {

static constexpr long s_connect_timeout_seconds = 90L;

NonnullRefPtr<WebSocketImplCurl> WebSocketImplCurl::create(CURLM* multi_handle)
{
    return adopt_ref(*new WebSocketImplCurl(multi_handle));
}

WebSocketImplCurl::WebSocketImplCurl(CURLM* multi_handle)
    : m_multi_handle(multi_handle)
{
}

WebSocketImplCurl::~WebSocketImplCurl()
{
    if (m_read_notifier)
        m_read_notifier->close();
    if (m_write_notifier)
        m_write_notifier->close();
    if (m_error_notifier)
        m_error_notifier->close();

    if (m_easy_handle) {
        curl_multi_remove_handle(m_multi_handle, m_easy_handle);
        curl_easy_cleanup(m_easy_handle);
    }

    for (auto* list : m_curl_string_lists) {
        curl_slist_free_all(list);
    }
}

void WebSocketImplCurl::connect(WebSocket::ConnectionInfo const& info)
{
    VERIFY(!m_easy_handle);
    VERIFY(on_connected);
    VERIFY(on_connection_error);
    VERIFY(on_ready_to_read);

    m_easy_handle = curl_easy_init();
    VERIFY(m_easy_handle); // FIXME: Allow failure, and return ENOMEM

    auto set_option = [this](auto option, auto value) -> bool {
        auto result = curl_easy_setopt(m_easy_handle, option, value);
        if (result == CURLE_OK)
            return true;
        dbgln("WebSocketImplCurl::connect: Failed to set curl option {}={}: {}", to_underlying(option), value, curl_easy_strerror(result));
        return false;
    };

    set_option(CURLOPT_PRIVATE, reinterpret_cast<uintptr_t>(this) | websocket_private_tag);
    set_option(CURLOPT_WS_OPTIONS, CURLWS_RAW_MODE);
    set_option(CURLOPT_CONNECT_ONLY, 2); // WebSocket mode

    // FIXME: Add a header function to validate the Sec-WebSocket headers that curl currently doesn't validate

    auto const& url = info.url();
    set_option(CURLOPT_URL, url.to_byte_string().characters());
    set_option(CURLOPT_PORT, url.port_or_default());
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);

    if (auto root_certs = info.root_certificates_path(); root_certs.has_value())
        set_option(CURLOPT_CAINFO, root_certs->characters());

    auto const origin_header = ByteString::formatted("Origin: {}", info.origin());
    curl_slist* curl_headers = curl_slist_append(nullptr, origin_header.characters());

    for (auto const& [name, value] : info.headers().headers()) {
        // curl will discard headers with empty values unless we pass the header name followed by a semicolon.
        ByteString header_string;
        if (value.is_empty())
            header_string = ByteString::formatted("{};", name);
        else
            header_string = ByteString::formatted("{}: {}", name, value);
        curl_headers = curl_slist_append(curl_headers, header_string.characters());
    }

    if (auto const& protocols = info.protocols(); !protocols.is_empty()) {
        StringBuilder protocol_builder;
        protocol_builder.append("Sec-WebSocket-Protocol: "sv);
        protocol_builder.append(ByteString::join(","sv, protocols));
        curl_headers = curl_slist_append(curl_headers, protocol_builder.to_byte_string().characters());
    }

    if (auto const& extensions = info.extensions(); !extensions.is_empty()) {
        StringBuilder protocol_builder;
        protocol_builder.append("Sec-WebSocket-Extensions: "sv);
        protocol_builder.append(ByteString::join(","sv, extensions));
        curl_headers = curl_slist_append(curl_headers, protocol_builder.to_byte_string().characters());
    }

    set_option(CURLOPT_HTTPHEADER, curl_headers);
    m_curl_string_lists.append(curl_headers);

    if (auto const& dns_info = info.dns_result(); dns_info.has_value()) {
        auto* resolve_list = curl_slist_append(nullptr, build_curl_resolve_list(*dns_info, url.serialized_host(), url.port_or_default()).characters());
        set_option(CURLOPT_RESOLVE, resolve_list);
        m_curl_string_lists.append(resolve_list);
    }

    CURLMcode const err = curl_multi_add_handle(m_multi_handle, m_easy_handle);
    VERIFY(err == CURLM_OK);
}

bool WebSocketImplCurl::can_read_line()
{
    VERIFY_NOT_REACHED();
}

ErrorOr<ByteBuffer> WebSocketImplCurl::read(int max_size)
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(max_size));
    auto const read_bytes = TRY(m_read_buffer.read_some(buffer));
    return buffer.slice(0, read_bytes.size());
}

ErrorOr<ByteString> WebSocketImplCurl::read_line(size_t)
{
    VERIFY_NOT_REACHED();
}

bool WebSocketImplCurl::send(ReadonlyBytes bytes)
{
    if (auto const error = m_pending_write_buffer.try_append(bytes); error.is_error()) {
        dbgln("Failed to queue WebSocket write: {}", error.error());
        on_connection_error();
        return false;
    }

    if (flush_pending_write_buffer())
        return true;

    on_connection_error();
    return false;
}

bool WebSocketImplCurl::eof()
{
    return m_read_buffer.is_eof();
}

void WebSocketImplCurl::discard_connection()
{
    if (m_read_notifier) {
        m_read_notifier->close();
        m_read_notifier = nullptr;
    }
    if (m_write_notifier) {
        m_write_notifier->close();
        m_write_notifier = nullptr;
    }
    if (m_error_notifier) {
        m_error_notifier->close();
        m_error_notifier = nullptr;
    }
    if (m_easy_handle) {
        curl_multi_remove_handle(m_multi_handle, m_easy_handle);
        curl_easy_cleanup(m_easy_handle);
        m_easy_handle = nullptr;
    }
    m_pending_write_buffer.clear();
    m_pending_write_buffer_offset = 0;
}

void WebSocketImplCurl::read_from_socket()
{
    bool received_data = false;

    // "Wait on the socket only if curl_easy_recv returns CURLE_AGAIN. The reason for this is libcurl or the SSL
    // library may internally cache some data, therefore you should call curl_easy_recv until all data is read which
    // would include any cached data."
    for (;;) {
        u8 buffer[65536];
        size_t nread = 0;
        CURLcode const result = curl_easy_recv(m_easy_handle, buffer, sizeof(buffer), &nread);
        if (result == CURLE_AGAIN)
            break;

        if (result != CURLE_OK) {
            dbgln("Failed to read from WebSocket: {}", curl_easy_strerror(result));
            // Process any successfully buffered data (e.g., the server's close frame) before reporting the error. This
            // handles cases where the server drops the connection immediately after sending the close frame, causing a
            // subsequent curl_easy_recv call to fail.
            if (received_data)
                on_ready_to_read();
            on_connection_error();
            return;
        }

        // "Reading exactly 0 bytes indicates a closed connection." which
        // may be part of the closing handshake

        received_data = true;
        if (0 == nread)
            break;

        if (auto const err = m_read_buffer.write_until_depleted({ buffer, nread }); err.is_error()) {
            on_connection_error();
            return;
        }
    }

    if (received_data)
        on_ready_to_read();
}

bool WebSocketImplCurl::flush_pending_write_buffer()
{
    while (m_pending_write_buffer_offset < m_pending_write_buffer.size()) {
        auto pending_bytes = m_pending_write_buffer.bytes().slice(m_pending_write_buffer_offset);

        size_t sent = 0;
        auto const result = curl_easy_send(m_easy_handle, pending_bytes.data(), pending_bytes.size(), &sent);
        if (result != CURLE_OK && result != CURLE_AGAIN) {
            dbgln("Failed to send to WebSocket: {}", curl_easy_strerror(result));
            return false;
        }

        m_pending_write_buffer_offset += sent;
        if (sent > 0)
            continue;

        if (result == CURLE_AGAIN) {
            if (m_write_notifier)
                m_write_notifier->set_enabled(true);
            return true;
        }

        dbgln("Failed to make progress sending to WebSocket");
        return false;
    }

    m_pending_write_buffer.clear();
    m_pending_write_buffer_offset = 0;
    if (m_write_notifier)
        m_write_notifier->set_enabled(false);
    return true;
}

bool WebSocketImplCurl::did_connect()
{
    curl_socket_t socket_fd = CURL_SOCKET_BAD;
    auto res = curl_easy_getinfo(m_easy_handle, CURLINFO_ACTIVESOCKET, &socket_fd);
    if (res != CURLE_OK || socket_fd == CURL_SOCKET_BAD)
        return false;

    m_read_notifier = Core::Notifier::construct(socket_fd, Core::Notifier::Type::Read);
    m_read_notifier->on_activation = [this] {
        read_from_socket();
    };
    m_write_notifier = Core::Notifier::construct(socket_fd, Core::Notifier::Type::Write);
    m_write_notifier->on_activation = [this] {
        if (!flush_pending_write_buffer())
            on_connection_error();
    };
    m_write_notifier->set_enabled(false);
    m_error_notifier = Core::Notifier::construct(socket_fd, Core::Notifier::Type::Error | Core::Notifier::Type::HangUp);
    m_error_notifier->on_activation = [this] {
        on_connection_error();
    };

    on_connected();

    // There may be data waiting for us already (e.g. if the server sends us data immediately upon opening a WebSocket),
    // so try reading immediately.
    read_from_socket();

    return true;
}

}
