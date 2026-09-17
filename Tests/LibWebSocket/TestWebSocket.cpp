/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <LibWebSocket/WebSocket.h>

namespace {

class TestWebSocketImpl final : public WebSocket::WebSocketImpl {
public:
    virtual void connect(WebSocket::ConnectionInfo const&) override { on_connected(); }
    virtual bool can_read_line() override { return false; }
    virtual ErrorOr<ByteString> read_line(size_t) override { return Error::from_errno(ENOTSUP); }
    virtual ErrorOr<ByteBuffer> read(int) override { return move(m_pending_data); }
    virtual bool send(ReadonlyBytes) override { return true; }
    virtual bool eof() override { return false; }
    virtual void discard_connection() override { }
    virtual bool handshake_complete_when_connected() const override { return true; }

    void receive(ByteBuffer data)
    {
        m_pending_data = move(data);
        on_ready_to_read();
    }

private:
    ByteBuffer m_pending_data;
};

struct CloseResult {
    bool reported_error { false };
    bool reflected_invalid_reason { false };
    u16 close_code { 0 };
};

CloseResult receive_close(bool valid_reason)
{
    auto implementation = adopt_ref(*new TestWebSocketImpl);
    auto url = URL::Parser::basic_parse("ws://localhost/"sv).release_value();
    auto websocket = WebSocket::WebSocket::create(WebSocket::ConnectionInfo(move(url)), implementation);

    CloseResult result;
    websocket->on_error = [&](auto) { result.reported_error = true; };
    websocket->on_close = [&](auto code, auto reason, auto) {
        result.close_code = code;
        result.reflected_invalid_reason = reason.length() == 1 && static_cast<u8>(reason.characters()[0]) == 0xff;
    };
    websocket->start();
    EXPECT(websocket->ready_state() == WebSocket::ReadyState::Open);

    u8 frame[] { 0x88, 3, 0x03, 0xe8, static_cast<u8>(valid_reason ? 'A' : 0xff) };
    implementation->receive(MUST(ByteBuffer::copy(frame)));
    if (websocket->ready_state() != WebSocket::ReadyState::Closed)
        implementation->on_connection_error();
    return result;
}

}

TEST_CASE(invalid_close_reason_fails_connection)
{
    Core::EventLoop event_loop;

    auto result = receive_close(false);
    EXPECT(result.reported_error);
    EXPECT_EQ(result.close_code, to_underlying(WebSocket::CloseStatusCode::InvalidPayload));
    EXPECT(!result.reflected_invalid_reason);

    auto control = receive_close(true);
    EXPECT(!control.reported_error);
    EXPECT_EQ(control.close_code, to_underlying(WebSocket::CloseStatusCode::Normal));
    EXPECT(!control.reflected_invalid_reason);
}
