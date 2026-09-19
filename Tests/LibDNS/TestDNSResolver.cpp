/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/MemoryStream.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibCore/UDPServer.h>
#include <LibDNS/Resolver.h>
#include <LibTest/TestCase.h>

#include <AK/Windows.h>

namespace {

// Builds a canned DNS response for a query: It echoes the question section and answers with one fixed A and one fixed
// AAAA record. The resolver correlates responses to lookups by header ID, and reads the answer section. Enough to drive
// a successful lookup while exercising the real wire encode and decode paths — without depending on a live DNS server.
ErrorOr<ByteBuffer> build_dns_response(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_recursion_available(true);
    response.header.options.set_response_code(DNS::Messages::Options::ResponseCode::NoError);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

    auto name = response.questions.is_empty()
        ? DNS::Messages::DomainName::from_string("example.com"sv)
        : response.questions.first().name;

    response.answers.append(DNS::Messages::ResourceRecord {
        name, DNS::Messages::ResourceType::A, DNS::Messages::Class::IN, 300,
        DNS::Messages::Records::A { IPv4Address { 192, 0, 2, 1 } }, {} });
    response.answers.append(DNS::Messages::ResourceRecord {
        name, DNS::Messages::ResourceType::AAAA, DNS::Messages::Class::IN, 300,
        DNS::Messages::Records::AAAA { IPv6Address::loopback() }, {} });
    response.header.answer_count = response.answers.size();

    ByteBuffer out;
    TRY(response.to_raw(out));
    return out;
}

ErrorOr<ByteBuffer> build_response_with_unknown_key_tag(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_is_question(true);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

    auto owner = response.questions.first().name;
    response.answers.append({ owner, DNS::Messages::ResourceType::A, DNS::Messages::Class::IN, 60,
        DNS::Messages::Records::A { IPv4Address { 192, 0, 2, 1 } }, {} });
    u8 signature_byte = 0;
    auto now = UnixDateTime::now();
    response.answers.append({ owner, DNS::Messages::ResourceType::RRSIG, DNS::Messages::Class::IN, 60,
        DNS::Messages::Records::RRSIG { DNS::Messages::ResourceType::A, DNS::Messages::DNSSEC::Algorithm::RSASHA256,
            0, 60, now + AK::Duration::from_seconds(60), now - AK::Duration::from_seconds(60), 1, owner,
            TRY(ByteBuffer::copy(ReadonlyBytes { &signature_byte, 1 })) },
        {} });
    response.header.answer_count = response.answers.size();

    ByteBuffer out;
    TRY(response.to_raw(out));
    return out;
}

ErrorOr<ByteBuffer> build_response_with_unhandled_dnssec_algorithm(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_is_question(true);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

    auto algorithm = static_cast<DNS::Messages::DNSSEC::Algorithm>(250);
    auto owner = response.questions.first().name;
    u8 data_byte = 0;
    response.answers.append({ owner, DNS::Messages::ResourceType::DNSKEY, DNS::Messages::Class::IN, 60,
        DNS::Messages::Records::DNSKEY { 0, 0, algorithm, TRY(ByteBuffer::copy(ReadonlyBytes { &data_byte, 1 })), 0 }, {} });
    auto now = UnixDateTime::now();
    response.answers.append({ owner, DNS::Messages::ResourceType::RRSIG, DNS::Messages::Class::IN, 60,
        DNS::Messages::Records::RRSIG { DNS::Messages::ResourceType::DNSKEY, algorithm, 0, 60,
            now + AK::Duration::from_seconds(60), now - AK::Duration::from_seconds(60), 250, owner,
            TRY(ByteBuffer::copy(ReadonlyBytes { &data_byte, 1 })) },
        {} });
    response.header.answer_count = response.answers.size();

    ByteBuffer out;
    TRY(response.to_raw(out));
    return out;
}

// Answers with one TXT record for the queried owner and one A record for a different owner; neither answers the question.
ErrorOr<ByteBuffer> build_response_with_unrelated_answers(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_recursion_available(true);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

    auto owner = response.questions.first().name;
    response.answers.append({ owner, DNS::Messages::ResourceType::TXT, DNS::Messages::Class::IN, 3600,
        DNS::Messages::Records::TXT { "x"sv }, {} });
    response.answers.append({ DNS::Messages::DomainName::from_string("other.example"sv), DNS::Messages::ResourceType::A, DNS::Messages::Class::IN, 3600,
        DNS::Messages::Records::A { IPv4Address { 192, 0, 2, 1 } }, {} });
    response.header.answer_count = response.answers.size();

    ByteBuffer out;
    TRY(response.to_raw(out));
    return out;
}

void expect_successful_lookup(DNS::Resolver& resolver, Core::EventLoop& loop)
{
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
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

}

TEST_CASE(test_udp)
{
    Core::EventLoop loop;

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();

    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        auto response = MUST(build_dns_response(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };

    expect_successful_lookup(resolver, loop);
}

TEST_CASE(test_tcp)
{
    Core::EventLoop loop;

    auto server = MUST(Core::TCPServer::try_create());
    MUST(server->listen(IPv4Address { 127, 0, 0, 1 }, 0, Core::TCPServer::AllowAddressReuse::Yes));
    auto server_port = server->local_port().value();

    Vector<NonnullOwnPtr<Core::TCPSocket>> connections;
    ByteBuffer inbox;
    server->on_ready_to_accept = [&] {
        // The listening socket is non-blocking — so a readiness notification can outpace the connection. Tolerate
        // accept() reporting nothing is ready yet.
        auto accepted = server->accept();
        if (accepted.is_error())
            return;
        connections.append(accepted.release_value());
        auto& socket = *connections.last();
        socket.on_ready_to_read = [&socket, &inbox] {
            // The accepted socket is non-blocking and TCP is a stream — so read what is available, and accumulate it
            // until the framed message is complete.
            u8 chunk[1024];
            auto read = socket.read_some({ chunk, sizeof(chunk) });
            if (read.is_error() || inbox.try_append(read.value()).is_error())
                return;

            // DNS over TCP frames each message with a 2-byte length prefix.
            if (inbox.size() < sizeof(u16))
                return;
            u16 const message_size = (static_cast<u16>(inbox[0]) << 8) | inbox[1];
            if (inbox.size() < sizeof(u16) + message_size)
                return;

            auto response = MUST(build_dns_response(inbox.bytes().slice(sizeof(u16), message_size)));
            ByteBuffer framed;
            NetworkOrdered<u16> framed_size = response.size();
            MUST(framed.try_append(&framed_size, sizeof(framed_size)));
            MUST(framed.try_append(response));
            MUST(socket.write_until_depleted(framed.bytes()));
            inbox.clear();
        };
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            auto socket = TRY(Core::TCPSocket::connect(address));
            TRY(socket->set_blocking(false));
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::TCPSocket>::create(move(socket))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }
    };

    expect_successful_lookup(resolver, loop);
}

TEST_CASE(test_dnssec_response_rejects_unknown_key_tag)
{
    Core::EventLoop loop;

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        auto response = MUST(build_response_with_unknown_key_tag(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };

    TRY_OR_FAIL(resolver.when_socket_ready()->await());
    resolver.lookup(""sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A }, { .validate_dnssec_locally = true })
        ->when_resolved([&](auto&) {
            loop.quit(1);
        })
        .when_rejected([&](auto&) {
            loop.quit(0);
        });
    auto deadline = Core::Timer::create_single_shot(1000, [&] { loop.quit(2); });
    deadline->start();
    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_dnssec_response_rejects_unhandled_algorithm)
{
    Core::EventLoop loop;

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        auto response = MUST(build_response_with_unhandled_dnssec_algorithm(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<DNS::Resolver::SocketResult> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };

    TRY_OR_FAIL(resolver.when_socket_ready()->await());
    resolver.lookup(""sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::DNSKEY }, { .validate_dnssec_locally = true })
        ->when_resolved([&](auto& result) {
            loop.quit(result->is_dnssec_validated() ? 1 : 2);
        })
        .when_rejected([&](auto&) {
            loop.quit(0);
        });
    auto deadline = Core::Timer::create_single_shot(1000, [&] { loop.quit(3); });
    deadline->start();
    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_localhost_resolves_to_loopback_without_a_socket)
{
    Core::EventLoop loop;

    // The socket factory must never run: localhost names are answered in-process, never sent upstream.
    DNS::Resolver resolver {
        [&] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
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
    // Absolute forms take the same path.
    expect_loopback("localhost."sv);
    expect_loopback("test-host.localhost."sv);
    // URL hosts may have labels longer than DNS allows; these never reach the wire.
    expect_loopback(ByteString::formatted("{}.localhost", ByteString::repeated('a', 64)));
    expect_loopback("Test-Host.LocalHost"sv);
}

TEST_CASE(test_lookup_rejects_names_that_must_not_go_upstream)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            return Error::from_string_literal("DNS socket should not be created for this lookup");
        }
    };

    auto expect_rejection = [&](StringView name) {
        auto result = resolver.lookup(name, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await();
        EXPECT(result.is_error());
    };

    expect_rejection("service.onion"sv);
    expect_rejection("Service.ONION"sv);
    expect_rejection("service.onion.."sv);
    expect_rejection("localhost.."sv);
    expect_rejection("victim..example"sv);
    expect_rejection(".example"sv);
}

TEST_CASE(test_configured_server_failure_does_not_fall_back_to_the_system_resolver)
{
    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            return Error::from_string_literal("Configured DNS server is unreachable");
        }
    };

    resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })
        ->when_resolved([&](auto&) { loop.quit(1); })
        .when_rejected([&](auto&) { loop.quit(0); });
    EXPECT_EQ(0, loop.exec());
}

TEST_CASE(test_unrelated_answers_are_not_cached)
{
    Core::EventLoop loop;

    size_t queries = 0;
    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        ++queries;
        auto response = MUST(build_response_with_unrelated_answers(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    auto lookup = [&] {
        auto result = resolver.lookup("victim.example"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await();
        if (!result.is_error())
            EXPECT(!result.value()->has_cached_addresses());
    };

    lookup();
    EXPECT_EQ(queries, 1u);
    // Nothing answered the question, so nothing should have been cached for it.
    lookup();
    EXPECT_EQ(queries, 2u);
}

TEST_CASE(test_concurrent_lookups_for_one_name_all_resolve)
{
    Core::EventLoop loop;

    size_t queries = 0;
    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        ++queries;
        auto response = MUST(build_dns_response(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    Vector<NonnullRefPtr<Core::Promise<NonnullRefPtr<DNS::LookupResult const>>>> promises;
    for (size_t i = 0; i < 3; ++i)
        promises.append(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA }));

    for (auto& promise : promises) {
        auto result = TRY_OR_FAIL(promise->await());
        EXPECT(result->has_cached_addresses());
    }
    EXPECT_EQ(queries, 1u);
}
