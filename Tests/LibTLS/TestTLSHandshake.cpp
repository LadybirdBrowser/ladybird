/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibTLS/TLSv12.h>
#include <LibTest/TestCase.h>

static int port = 443;

constexpr auto DEFAULT_SERVER = "www.google.com"sv;

static ByteBuffer operator""_b(char const* string, size_t length)
{
    return ByteBuffer::copy(string, length).release_value();
}

TEST_CASE(test_TLS_hello_handshake)
{
    Core::EventLoop loop;

    TLS::Options options = {};

    auto tls = TRY_OR_FAIL(Core::BufferedSocket<TLS::TLSv12>::create(TRY_OR_FAIL(TLS::TLSv12::connect(DEFAULT_SERVER, port, move(options)))));

    TRY_OR_FAIL(tls->write_until_depleted("GET /generate_204 HTTP/1.1\r\nHost: "_b));

    auto the_server = DEFAULT_SERVER;
    TRY_OR_FAIL(tls->write_until_depleted(the_server.bytes()));
    TRY_OR_FAIL(tls->write_until_depleted("\r\nConnection: close\r\n\r\n"_b));

    tls->on_ready_to_read = [&]() mutable -> void {
        auto read_buffer = ByteBuffer::create_uninitialized(4096).release_value();
        auto err = tls->can_read_up_to_delimiter("\r\n"sv.bytes());
        if (err.is_error() && err.error().code() != EAGAIN) {
            FAIL("Unexpected socket error during read");
            loop.quit(1);
            return;
        }
        if (!err.value())
            return;

        auto line = TRY_OR_FAIL(tls->read_until_any_of(read_buffer, Array { "\r\n"sv }));
        auto no_content_prefix = "HTTP/1.1 204 No Content"_b;
        EXPECT(line.starts_with(no_content_prefix));
        loop.quit(0);
    };
    tls->set_notifications_enabled(true);

    auto timeout = Core::Timer::create_single_shot(10'000, [&loop] {
        loop.quit(1);
    });

    EXPECT_EQ(loop.exec(), 0);
}
