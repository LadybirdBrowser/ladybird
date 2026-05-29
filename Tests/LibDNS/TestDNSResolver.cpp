/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibDNS/Resolver.h>
#include <LibTLS/TLSv12.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_udp)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(53) };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(addr)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };

    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
        ->when_resolved([&](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        })
        .when_rejected([&](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_tcp)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(53) };

            auto tcp_socket = TRY(Core::TCPSocket::connect(addr));
            TRY(tcp_socket->set_blocking(false));

            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::TCPSocket>::create(move(tcp_socket))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }
    };

    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
        ->when_resolved([&](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        })
        .when_rejected([&](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

    EXPECT_EQ(0, loop.exec());
}

// https://www.rfc-editor.org/rfc/rfc6761#section-6.3
TEST_CASE(test_localhost_resolves_to_loopback_without_a_socket)
{
    Core::EventLoop loop;

    // The socket factory must never run: localhost names are answered in-process, never sent upstream.
    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            return Error::from_string_literal("DNS socket should not be created for a localhost lookup");
        }
    };

    auto expect_loopback = [&](StringView name) {
        bool saw_loopback_v4 = false;
        bool saw_loopback_v6 = false;
        resolver.lookup(name, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
            ->when_resolved([&](auto& result) {
                for (auto const& record : result->records()) {
                    if (auto const* a = record.record.template get_pointer<DNS::Messages::Records::A>())
                        saw_loopback_v4 = a->address == IPv4Address { 127, 0, 0, 1 };
                    else if (auto const* aaaa = record.record.template get_pointer<DNS::Messages::Records::AAAA>())
                        saw_loopback_v6 = aaaa->address == IPv6Address::loopback();
                }
                loop.quit(0);
            })
            .when_rejected([&](auto& error) {
                outln("Failed to resolve {}: {}", name, error);
                loop.quit(1);
            });
        EXPECT_EQ(0, loop.exec());
        EXPECT(saw_loopback_v4);
        EXPECT(saw_loopback_v6);
    };

    expect_loopback("localhost"sv);
    // A multi-label subdomain: the case that the host resolver / upstream server may not map to loopback.
    expect_loopback("test-host.localhost"sv);
}

TEST_CASE(test_tls)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(853) };

            TLS::Options options = {};

            return DNS::Resolver::SocketResult {
                MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(addr, "1.1.1.1", move(options)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }
    };

    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
        ->when_resolved([&](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        })
        .when_rejected([&](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

    EXPECT_EQ(0, loop.exec());
}
