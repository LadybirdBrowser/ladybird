/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "HeadlessTest.h"
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWebView/Application.h>

namespace ReportServerTest {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }
};

struct ReceivedRequest {
    ByteString path;
    HashMap<ByteString, ByteString> headers;
    ByteString body;
};

struct Response {
    u32 status { 200 };
    ByteString body;
    ByteString extra_headers;
};

inline Response challenge(StringView token)
{
    return { 200, ByteString::formatted(R"({{"algorithm":"sha256-seed-nonce-le-v1","token":"{}","expected_work":1}})", token), {} };
}

// A report server on the loopback interface that answers each request with the next of the responses it was given. A
// request that comes in once they have run out is left waiting.
class ReportServer {
public:
    ReportServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { read_from(connection); };
        };
    }

    ByteString endpoint() const { return ByteString::formatted("http://127.0.0.1:{}", *m_server->local_port()); }

    void respond_with(Response response) { m_responses.append(move(response)); }
    bool has_answered_everything() const { return m_responses.is_empty(); }

    Vector<ReceivedRequest> requests;

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
        bool sent_continue { false };
    };

    void read_from(Connection& connection)
    {
        auto buffer = MUST(ByteBuffer::create_uninitialized(64 * KiB));
        auto received = connection.socket->read_some(buffer);
        if (received.is_error() || received.value().is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }
        connection.data.append(received.value());

        auto data = StringView { connection.data.bytes() };
        auto header_end = data.find("\r\n\r\n"sv);
        if (!header_end.has_value())
            return;

        ReceivedRequest request;
        auto lines = data.substring_view(0, *header_end).split_view("\r\n"sv);
        request.path = lines[0].split_view(' ')[1];
        for (size_t i = 1; i < lines.size(); ++i) {
            auto separator = lines[i].find(':').value();
            request.headers.set(ByteString { lines[i].substring_view(0, separator) }.to_lowercase(),
                lines[i].substring_view(separator + 1).trim_whitespace());
        }

        auto content_length = request.headers.get("content-length"sv).value_or("0"sv).to_number<size_t>().value();
        auto body = data.substring_view(*header_end + 4);
        if (body.length() < content_length) {
            if (request.headers.get("expect"sv) == "100-continue"sv && !connection.sent_continue) {
                connection.sent_continue = true;
                MUST(connection.socket->write_until_depleted("HTTP/1.1 100 Continue\r\n\r\n"sv.bytes()));
            }
            return;
        }
        request.body = body.substring_view(0, content_length);
        requests.append(move(request));

        connection.socket->on_ready_to_read = nullptr;
        if (m_responses.is_empty())
            return;
        auto response = m_responses.take_first();
        MUST(connection.socket->set_blocking(true));
        auto response_text = ByteString::formatted(
            "HTTP/1.1 {} Status\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
            response.status, response.body.length(), response.extra_headers, response.body);
        MUST(connection.socket->write_until_depleted(response_text.bytes()));
        connection.socket->close();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    Vector<Response> m_responses;
};

inline ByteString sha256_hex(StringView text)
{
    return encode_hex(Crypto::Hash::SHA256::hash(text.bytes()).bytes());
}

// The content of the multipart part with the given name.
inline StringView multipart_part(StringView body, StringView name)
{
    auto disposition = ByteString::formatted("Content-Disposition: form-data; name=\"{}\"", name);
    auto start = body.find(disposition).value();
    auto content_start = body.find("\r\n\r\n"sv, start).value() + 4;
    auto content_end = body.find("\r\n--"sv, content_start).value();
    return body.substring_view(content_start, content_end - content_start);
}

inline Optional<JsonValue> manifest_field(JsonObject const& manifest, StringView key)
{
    Optional<JsonValue> value;
    manifest.get_array("fields"sv)->for_each([&](JsonValue const& field) {
        if (field.as_object().get_string("key"sv) == key)
            value = field.as_object().get_object("value"sv)->get("value"sv).value();
    });
    return value;
}

}
