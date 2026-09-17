/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibCore/EventLoop.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <LibWebSocket/WebSocket.h>

namespace {

class TestWebSocketImpl final : public WebSocket::WebSocketImpl {
public:
    virtual void connect(WebSocket::ConnectionInfo const&) override { on_connected(); }
    virtual bool can_read_line() override { return !m_lines.is_empty(); }
    virtual ErrorOr<ByteString> read_line(size_t) override { return m_lines.take_first(); }
    virtual ErrorOr<ByteBuffer> read(int) override { return ByteBuffer {}; }
    virtual bool send(ReadonlyBytes bytes) override
    {
        m_request = ByteString(bytes);
        return true;
    }
    virtual bool eof() override { return false; }
    virtual void discard_connection() override { }

    void receive(Vector<ByteString> lines)
    {
        m_lines = move(lines);
        on_ready_to_read();
    }

    ByteString expected_accept() const
    {
        auto request = m_request.view();
        auto marker = "Sec-WebSocket-Key: "sv;
        auto key_start = request.find(marker).value() + marker.length();
        auto key_end = request.find("\r\n"sv, key_start).value();
        auto content = ByteString::formatted("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", request.substring_view(key_start, key_end - key_start));
        Crypto::Hash::Manager hash;
        hash.initialize(Crypto::Hash::HashKind::SHA1);
        hash.update(content);
        auto digest = hash.digest();
        auto encoded = MUST(encode_base64({ digest.immutable_data(), digest.data_length() }));
        return ByteString(encoded.bytes_as_string_view());
    }

private:
    ByteString m_request;
    Vector<ByteString> m_lines;
};

struct HandshakeResult {
    bool opened { false };
    bool reported_error { false };
};

HandshakeResult run_handshake(bool append_proof_suffix)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    HandshakeResult result;
    websocket->on_open = [&] { result.opened = true; };
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->start();

    auto accept_header = ByteString::formatted("Sec-WebSocket-Accept: {}{}", implementation->expected_accept(), append_proof_suffix ? ":conflict"sv : ""sv);
    implementation->receive({
        "HTTP/1.1 101 Switching Protocols",
        "Upgrade: websocket",
        "Connection: Upgrade",
        move(accept_header),
        "",
    });
    return result;
}

}

TEST_CASE(accept_proof_suffix_fails_handshake)
{
    Core::EventLoop event_loop;

    auto result = run_handshake(true);
    EXPECT(!result.opened);
    EXPECT(result.reported_error);

    auto control = run_handshake(false);
    EXPECT(control.opened);
    EXPECT(!control.reported_error);
}
