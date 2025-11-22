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
        [] -> NonnullRefPtr<Core::Promise<DNS::Resolver::SocketResult>> {
            auto promise = Core::Promise<DNS::Resolver::SocketResult>::construct();

            auto make_resolver = [] -> ErrorOr<DNS::Resolver::SocketResult> {
                Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(53) };
                return DNS::Resolver::SocketResult {
                    TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(addr)))),
                    DNS::Resolver::ConnectionMode::UDP,
                };
            };

            auto result = make_resolver();
            if (!result.is_error()) {
                promise->resolve(result.release_value());
            } else {
                promise->reject(result.release_error());
            }

            return promise;
        }
    };

    auto when_socket_ready_promise = resolver.when_socket_ready();

    when_socket_ready_promise->when_resolved([&loop, &resolver, when_socket_ready_promise](auto&) {
        auto lookup_promise = resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
        lookup_promise->when_resolved([&](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        });

        lookup_promise->when_rejected([&](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

        when_socket_ready_promise->add_child(move(lookup_promise));
    });

    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_tcp)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [] -> NonnullRefPtr<Core::Promise<DNS::Resolver::SocketResult>> {
            auto promise = Core::Promise<DNS::Resolver::SocketResult>::construct();

            auto make_resolver = [] -> ErrorOr<DNS::Resolver::SocketResult> {
                Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(53) };

                auto tcp_socket = TRY(Core::TCPSocket::connect(addr));
                TRY(tcp_socket->set_blocking(false));

                return DNS::Resolver::SocketResult {
                    TRY(Core::BufferedSocket<Core::TCPSocket>::create(move(tcp_socket))),
                    DNS::Resolver::ConnectionMode::TCP,
                };
            };

            auto result = make_resolver();
            if (!result.is_error()) {
                promise->resolve(result.release_value());
            } else {
                promise->reject(result.release_error());
            }

            return promise;
        }
    };

    auto when_socket_ready_promise = resolver.when_socket_ready();

    when_socket_ready_promise->when_resolved([&loop, &resolver, when_socket_ready_promise](auto&) {
        auto lookup_promise = resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
        lookup_promise->when_resolved([&loop](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        });

        lookup_promise->when_rejected([&loop](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

        when_socket_ready_promise->add_child(move(lookup_promise));
    });

    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_tls)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [] -> NonnullRefPtr<Core::Promise<DNS::Resolver::SocketResult>> {
            auto promise = Core::Promise<DNS::Resolver::SocketResult>::construct();

            auto make_resolver = [] -> ErrorOr<DNS::Resolver::SocketResult> {
                Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(853) };

                TLS::Options options = {};

                return DNS::Resolver::SocketResult {
                    MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(addr, "1.1.1.1", move(options)))),
                    DNS::Resolver::ConnectionMode::TCP,
                };
            };

            auto result = make_resolver();
            if (!result.is_error()) {
                promise->resolve(result.release_value());
            } else {
                promise->reject(result.release_error());
            }

            return promise;
        }
    };

    auto when_socket_ready_promise = resolver.when_socket_ready();

    when_socket_ready_promise->when_resolved([&loop, &resolver, when_socket_ready_promise](auto&) {
        auto lookup_promise = resolver.lookup("google.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
        lookup_promise->when_resolved([&loop](auto& result) {
            EXPECT(!result->records().is_empty());
            loop.quit(0);
        });

        lookup_promise->when_rejected([&loop](auto& error) {
            outln("Failed to resolve: {}", error);
            loop.quit(1);
        });

        when_socket_ready_promise->add_child(move(lookup_promise));
    });

    EXPECT_EQ(0, loop.exec());
}
