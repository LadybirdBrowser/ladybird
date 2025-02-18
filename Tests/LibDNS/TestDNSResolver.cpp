/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibDNS/Resolver.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTLS/TLSv12.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_udp)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(53) };
            return DNS::Resolver::SocketResult {
                MUST(Core::BufferedSocket<Core::UDPSocket>::create(MUST(Core::UDPSocket::connect(addr)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };

    MUST(resolver.when_socket_ready()->await());

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
            return DNS::Resolver::SocketResult {
                MUST(Core::BufferedSocket<Core::TCPSocket>::create(MUST(Core::TCPSocket::connect(addr)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }
    };

    MUST(resolver.when_socket_ready()->await());

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

static StringView ca_certs_file = "./cacert.pem"sv;
static Optional<ByteString> locate_ca_certs_file()
{
    if (FileSystem::exists(ca_certs_file)) {
        return ca_certs_file;
    }
    auto on_target_path = ByteString("/etc/cacert.pem");
    if (FileSystem::exists(on_target_path)) {
        return on_target_path;
    }
    return {};
}

TEST_CASE(test_tls)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress addr = { IPv4Address::from_string("1.1.1.1"sv).value(), static_cast<u16>(853) };

            TLS::Options options;
            options.set_root_certificates_path(locate_ca_certs_file());

            return DNS::Resolver::SocketResult {
                MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(addr, "1.1.1.1", move(options)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }
    };

    MUST(resolver.when_socket_ready()->await());

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
