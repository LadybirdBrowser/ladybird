/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/ConditionVariable.h>
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
    response.header.options.set_is_question(false);
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
    response.header.options.set_is_question(false);
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
    response.header.options.set_is_question(false);
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
    response.header.options.set_is_question(false);
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

ErrorOr<ByteBuffer> build_empty_response(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_is_question(false);
    response.header.options.set_recursion_available(true);
    response.header.options.set_response_code(DNS::Messages::Options::ResponseCode::NameError);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

    ByteBuffer out;
    TRY(response.to_raw(out));
    return out;
}

// Echoes the question with the TC bit set and no answers, as a server does when the answer does not fit in a datagram.
ErrorOr<ByteBuffer> build_truncated_response(ReadonlyBytes query_bytes)
{
    FixedMemoryStream stream { query_bytes };
    auto query = TRY(DNS::Messages::Message::from_raw(stream));

    DNS::Messages::Message response;
    response.header.id = query.header.id;
    response.header.options.set_is_question(false);
    response.header.options.set_is_truncated(true);
    response.header.question_count = query.questions.size();
    response.questions = move(query.questions);

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
                TRY(Core::UDPSocket::connect(address)),
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
                TRY(Core::UDPSocket::connect(address)),
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
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1000), [&] { loop.quit(2); });
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
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::UDPSocket::connect(address)),
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
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1000), [&] { loop.quit(3); });
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

TEST_CASE(test_loopback_domains_resolve_to_loopback_without_a_socket)
{
    Core::EventLoop loop;

    auto resolves_to_loopback = [](DNS::Resolver& resolver, StringView name) {
        auto result = resolver.lookup(name, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await();
        if (result.is_error())
            return false;
        bool saw_loopback_v4 = false;
        bool saw_loopback_v6 = false;
        for (auto const& record : result.value()->records()) {
            if (auto const* a = record.record.get_pointer<DNS::Messages::Records::A>())
                saw_loopback_v4 = a->address == IPv4Address { 127, 0, 0, 1 };
            else if (auto const* aaaa = record.record.get_pointer<DNS::Messages::Records::AAAA>())
                saw_loopback_v6 = aaaa->address == IPv6Address::loopback();
        }
        return saw_loopback_v4 && saw_loopback_v6;
    };

    DNS::Resolver resolver {
        [&] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            return Error::from_string_literal("DNS socket should not be created for a loopback lookup");
        }
    };
    resolver.set_loopback_domains({ "test"sv });
    EXPECT(resolves_to_loopback(resolver, "test"sv));
    EXPECT(resolves_to_loopback(resolver, "web-platform.test"sv));
    EXPECT(resolves_to_loopback(resolver, "a.b.test"sv));
    EXPECT(resolves_to_loopback(resolver, "Web-Platform.Test."sv));
    EXPECT(!resolves_to_loopback(resolver, "nottest"sv));
    EXPECT(!resolves_to_loopback(resolver, "xtest"sv));
    EXPECT(!resolves_to_loopback(resolver, "test.example"sv));

    DNS::Resolver resolver_without_loopback_domains {
        [&] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            return Error::from_string_literal("DNS socket should not be created for this lookup");
        }
    };
    EXPECT(!resolves_to_loopback(resolver_without_loopback_domains, "web-platform.test"sv));
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
                TRY(Core::UDPSocket::connect(address)),
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
                TRY(Core::UDPSocket::connect(address)),
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
    // One query per type, shared by all three callers.
    EXPECT_EQ(queries, 2u);
}

TEST_CASE(test_validated_lookup_settles_on_a_negative_response)
{
    Core::EventLoop loop;

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        auto response = MUST(build_empty_response(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::UDPSocket::connect(address)),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    bool settled = false;
    resolver.lookup("nonexistent.example"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A }, { .validate_dnssec_locally = true })
        ->when_resolved([&](auto& result) {
            EXPECT(!result->is_dnssec_validated());
            EXPECT(!result->has_cached_addresses());
            settled = true;
        })
        .when_rejected([&](auto&) { settled = true; });

    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(2000), [&] { loop.quit(1); });
    deadline->start();
    loop.spin_until([&] { return settled || loop.was_exit_requested(); });
    EXPECT(settled);
}

TEST_CASE(test_short_udp_datagram_does_not_block_the_event_loop)
{
    Core::EventLoop loop;

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        // A datagram that is not even a header, then the real answer.
        u8 junk = 0;
        MUST(server->send({ &junk, 1 }, from));
        auto response = MUST(build_dns_response(query.bytes()));
        MUST(server->send(response.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::UDPSocket::connect(address)),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    auto result = TRY_OR_FAIL(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await());
    EXPECT(result->has_cached_addresses());
}

TEST_CASE(test_partial_tcp_frame_does_not_block_the_event_loop)
{
    Core::EventLoop loop;

    auto server = MUST(Core::TCPServer::try_create());
    MUST(server->listen(IPv4Address { 127, 0, 0, 1 }, 0, Core::TCPServer::AllowAddressReuse::Yes));
    auto server_port = server->local_port().value();

    Vector<NonnullOwnPtr<Core::TCPSocket>> connections;
    ByteBuffer inbox;
    ByteBuffer pending_response;
    server->on_ready_to_accept = [&] {
        auto accepted = server->accept();
        if (accepted.is_error())
            return;
        connections.append(accepted.release_value());
        auto& socket = *connections.last();
        socket.on_ready_to_read = [&] {
            u8 chunk[1024];
            auto read = socket.read_some({ chunk, sizeof(chunk) });
            if (read.is_error() || inbox.try_append(read.value()).is_error())
                return;
            if (inbox.size() < sizeof(u16))
                return;
            u16 const message_size = (static_cast<u16>(inbox[0]) << 8) | inbox[1];
            if (inbox.size() < sizeof(u16) + message_size)
                return;

            auto response = MUST(build_dns_response(inbox.bytes().slice(sizeof(u16), message_size)));
            NetworkOrdered<u16> framed_size = response.size();
            MUST(pending_response.try_append(&framed_size, sizeof(framed_size)));
            MUST(pending_response.try_append(response));
            inbox.clear();

            // Only the first byte of the length prefix for now; the rest follows later.
            MUST(socket.write_until_depleted(pending_response.bytes().slice(0, 1)));
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
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    auto promise = resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A });

    // The event loop must keep running while the frame is incomplete.
    bool timer_fired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(100), [&] { timer_fired = true; });
    timer->start();
    loop.spin_until([&] { return timer_fired; });
    EXPECT(!promise->is_resolved());

    MUST(connections.last()->write_until_depleted(pending_response.bytes().slice(1)));
    auto result = TRY_OR_FAIL(promise->await());
    EXPECT(result->has_cached_addresses());
}

TEST_CASE(test_tcp_connection_is_replaced_after_the_server_closes_it)
{
    Core::EventLoop loop;

    auto server = MUST(Core::TCPServer::try_create());
    MUST(server->listen(IPv4Address { 127, 0, 0, 1 }, 0, Core::TCPServer::AllowAddressReuse::Yes));
    auto server_port = server->local_port().value();

    size_t accepted_connections = 0;
    Vector<NonnullOwnPtr<Core::TCPSocket>> connections;
    ByteBuffer inbox;
    server->on_ready_to_accept = [&] {
        auto accepted = server->accept();
        if (accepted.is_error())
            return;
        ++accepted_connections;
        connections.append(accepted.release_value());
        auto& socket = *connections.last();
        socket.on_ready_to_read = [&] {
            u8 chunk[1024];
            auto read = socket.read_some({ chunk, sizeof(chunk) });
            if (read.is_error() || inbox.try_append(read.value()).is_error())
                return;
            if (inbox.size() < sizeof(u16))
                return;
            u16 const message_size = (static_cast<u16>(inbox[0]) << 8) | inbox[1];
            if (inbox.size() < sizeof(u16) + message_size)
                return;

            // The first connection is closed without an answer; every later one answers.
            if (accepted_connections == 1) {
                inbox.clear();
                socket.close();
                return;
            }

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
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    auto result = TRY_OR_FAIL(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await());
    EXPECT(result->has_cached_addresses());
    EXPECT_EQ(accepted_connections, 2u);
}

TEST_CASE(test_truncated_udp_response_is_retried_over_tcp)
{
    Core::EventLoop loop;

    // The server answers on one port over both UDP and TCP. The port the system picks for TCP isn't reserved for UDP,
    // though, so another socket can already hold it for UDP — in which case, try again with a fresh port.
    RefPtr<Core::TCPServer> tcp_server;
    RefPtr<Core::UDPServer> udp_server;
    u16 server_port = 0;
    for (size_t attempt = 0; attempt < 10 && !udp_server; ++attempt) {
        tcp_server = MUST(Core::TCPServer::try_create());
        MUST(tcp_server->listen(IPv4Address { 127, 0, 0, 1 }, 0, Core::TCPServer::AllowAddressReuse::Yes));
        server_port = tcp_server->local_port().value();

        auto candidate = Core::UDPServer::construct();
        if (candidate->bind(IPv4Address { 127, 0, 0, 1 }, server_port))
            udp_server = move(candidate);
    }
    if (!udp_server) {
        FAIL("Couldn't bind UDP to the port of a TCP server");
        return;
    }

    size_t udp_queries = 0;
    udp_server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(udp_server->receive(4096, from));
        ++udp_queries;
        auto response = MUST(build_truncated_response(query.bytes()));
        MUST(udp_server->send(response.bytes(), from));
    };

    size_t tcp_queries = 0;
    Vector<NonnullOwnPtr<Core::TCPSocket>> connections;
    ByteBuffer inbox;
    tcp_server->on_ready_to_accept = [&] {
        auto accepted = tcp_server->accept();
        if (accepted.is_error())
            return;
        connections.append(accepted.release_value());
        auto& socket = *connections.last();
        socket.on_ready_to_read = [&] {
            u8 chunk[1024];
            auto read = socket.read_some({ chunk, sizeof(chunk) });
            if (read.is_error() || inbox.try_append(read.value()).is_error())
                return;
            if (inbox.size() < sizeof(u16))
                return;
            u16 const message_size = (static_cast<u16>(inbox[0]) << 8) | inbox[1];
            if (inbox.size() < sizeof(u16) + message_size)
                return;

            ++tcp_queries;
            auto response = MUST(build_dns_response(inbox.bytes().slice(sizeof(u16), message_size)));
            ByteBuffer framed;
            NetworkOrdered<u16> framed_size = response.size();
            MUST(framed.try_append(&framed_size, sizeof(framed_size)));
            MUST(framed.try_append(response));
            MUST(socket.write_until_depleted(framed.bytes()));
            inbox.clear();
        };
    };

    auto socket_factory = [server_port](bool with_tcp_fallback) {
        return [server_port, with_tcp_fallback] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            DNS::Resolver::SocketResult result { TRY(Core::UDPSocket::connect(address)), DNS::Resolver::ConnectionMode::UDP };
            if (with_tcp_fallback) {
                result.connect_tcp = [address] -> ErrorOr<NonnullOwnPtr<Core::Socket>> {
                    return TRY(Core::TCPSocket::connect(address));
                };
            }
            return result;
        };
    };

    DNS::Resolver resolver { socket_factory(true) };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());
    auto result = TRY_OR_FAIL(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await());
    EXPECT(result->has_cached_addresses());
    EXPECT_EQ(udp_queries, 1u);
    EXPECT_EQ(tcp_queries, 1u);

    // Without a way to reach the server over TCP, a truncated answer is still an error rather than an empty one.
    DNS::Resolver udp_only_resolver { socket_factory(false) };
    TRY_OR_FAIL(udp_only_resolver.when_socket_ready()->await());
    EXPECT(udp_only_resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->await().is_error());
    EXPECT_EQ(tcp_queries, 1u);
}

TEST_CASE(test_expired_addresses_are_not_mistaken_for_a_negative_answer)
{
    Core::EventLoop loop;

    // A long-lived CNAME to short-lived addresses: Once the addresses expire, the CNAME is still cached.
    size_t queries = 0;
    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    auto server_port = server->local_port().value();
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query_bytes = MUST(server->receive(4096, from));
        ++queries;

        FixedMemoryStream stream { query_bytes.bytes() };
        auto query = MUST(DNS::Messages::Message::from_raw(stream));

        DNS::Messages::Message response;
        response.header.id = query.header.id;
        response.header.options.set_is_question(false);
        response.header.question_count = query.questions.size();
        response.questions = move(query.questions);

        auto owner = response.questions.first().name;
        auto target = DNS::Messages::DomainName::from_string("target.example"sv);
        response.answers.append({ owner, DNS::Messages::ResourceType::CNAME, DNS::Messages::Class::IN, 3600,
            DNS::Messages::Records::CNAME { target }, {} });
        if (response.questions.first().type == DNS::Messages::ResourceType::A) {
            response.answers.append({ target, DNS::Messages::ResourceType::A, DNS::Messages::Class::IN, 1,
                DNS::Messages::Records::A { IPv4Address { 192, 0, 2, 1 } }, {} });
        }
        response.header.answer_count = response.answers.size();

        ByteBuffer out;
        MUST(response.to_raw(out));
        MUST(server->send(out.bytes(), from));
    };

    DNS::Resolver resolver {
        [server_port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, server_port };
            return DNS::Resolver::SocketResult {
                TRY(Core::UDPSocket::connect(address)),
                DNS::Resolver::ConnectionMode::UDP,
            };
        }
    };
    TRY_OR_FAIL(resolver.when_socket_ready()->await());

    auto lookup = [&] {
        return resolver.lookup("expiring.example"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await();
    };

    auto first = TRY_OR_FAIL(lookup());
    EXPECT_EQ(first->cached_addresses().size(), 1u);
    auto queries_for_first_lookup = queries;

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(2100), [&] { loop.quit(0); });
    timer->start();
    loop.exec();

    auto second = TRY_OR_FAIL(lookup());
    EXPECT_EQ(second->cached_addresses().size(), 1u);
    EXPECT(queries > queries_for_first_lookup);
}

namespace {

// A system resolver that answers for IPv6 right away, and for IPv4 only once the test lets it.
class SystemResolverWithHeldBackIPv4 {
public:
    DNS::Resolver::SystemResolverForTesting system_resolver(bool is_ipv6_globally_reachable = true)
    {
        m_ipv6_is_globally_reachable.store(is_ipv6_globally_reachable);
        return {
            .resolve_host = [this](ByteString const&, Core::Socket::AddressFamily family) -> ErrorOr<Vector<Variant<IPv4Address, IPv6Address>>> {
                if (family == Core::Socket::AddressFamily::IPv6Only) {
                    m_ipv6_answered.store(true);
                    return Vector<Variant<IPv4Address, IPv6Address>> { IPv6Address::loopback() };
                }
                MutexLocker locker { m_mutex };
                m_condition.wait_while([&] { return !m_ipv4_may_answer; });
                return Vector<Variant<IPv4Address, IPv6Address>> { IPv4Address { 192, 0, 2, 1 } };
            },
            .is_ipv6_globally_reachable = [this] { return m_ipv6_is_globally_reachable.load(); },
        };
    }

    bool ipv6_answered() const { return m_ipv6_answered.load(); }
    void set_ipv6_is_globally_reachable(bool reachable) { m_ipv6_is_globally_reachable.store(reachable); }

    void let_ipv4_answer()
    {
        MutexLocker locker { m_mutex };
        m_ipv4_may_answer = true;
        m_condition.broadcast();
    }

private:
    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    bool m_ipv4_may_answer { false };
    Atomic<bool> m_ipv6_answered { false };
    Atomic<bool> m_ipv6_is_globally_reachable { true };
};

bool has_address_of_family(DNS::LookupResult const& result, bool ipv6)
{
    return any_of(result.cached_addresses(), [ipv6](auto const& address) { return address.template has<IPv6Address>() == ipv6; });
}

}

TEST_CASE(test_system_resolver_waits_for_ipv4_addresses_after_ipv6_addresses)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(),
    };

    auto promise = resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
    loop.spin_until([&] { return system_resolver.ipv6_answered(); });

    // Well past the 50 ms that the AAAA side gets after the A side, and well short of the 1 s the A side gets.
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(200), [&] { loop.quit(0); });
    timer->start();
    loop.exec();

    // IPv6 addresses alone are no use if they turn out to be unreachable, so the lookup waits for the IPv4 addresses.
    EXPECT(!promise->is_resolved());
    EXPECT(!promise->is_rejected());

    system_resolver.let_ipv4_answer();
    auto result = TRY_OR_FAIL(promise->await());
    EXPECT_EQ(result->cached_addresses().size(), 2u);
    EXPECT(has_address_of_family(*result, false));
    EXPECT(has_address_of_family(*result, true));
}

TEST_CASE(test_system_resolver_goes_ahead_with_ipv6_addresses_when_ipv4_addresses_take_too_long)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(),
    };

    auto result = TRY_OR_FAIL(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await());
    EXPECT_EQ(result->cached_addresses().size(), 1u);
    EXPECT(has_address_of_family(*result, true));

    // The IPv4 addresses that come later are still cached for later lookups.
    system_resolver.let_ipv4_answer();
    loop.spin_until([&] { return resolver.lookup_in_cache("example.com"sv); });
    EXPECT_EQ(resolver.expect_cached("example.com"sv)->cached_addresses().size(), 2u);
}

TEST_CASE(test_system_resolver_skips_ipv6_addresses_when_ipv6_is_unreachable)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(false),
    };

    system_resolver.let_ipv4_answer();
    auto result = TRY_OR_FAIL(resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await());
    EXPECT_EQ(result->cached_addresses().size(), 1u);
    EXPECT(has_address_of_family(*result, false));
    EXPECT(!system_resolver.ipv6_answered());
}

TEST_CASE(test_system_resolver_resolves_ipv6_addresses_of_local_names_when_ipv6_is_unreachable)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(false),
    };

    // Link-local IPv6 addresses work without a route to the IPv6 internet, so ".local" names keep the AAAA side.
    system_resolver.let_ipv4_answer();
    auto result = TRY_OR_FAIL(resolver.lookup("printer.local"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await());
    EXPECT(system_resolver.ipv6_answered());
    EXPECT(has_address_of_family(*result, true));
}

TEST_CASE(test_system_resolver_looks_up_ipv6_addresses_again_once_ipv6_is_reachable)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(false),
    };

    system_resolver.let_ipv4_answer();
    auto lookup = [&] {
        return resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })->await();
    };

    auto first = TRY_OR_FAIL(lookup());
    EXPECT_EQ(first->cached_addresses().size(), 1u);
    EXPECT(!system_resolver.ipv6_answered());

    // While IPv6 stays unreachable, the cached result without IPv6 addresses is used.
    auto second = TRY_OR_FAIL(lookup());
    EXPECT_EQ(second.ptr(), first.ptr());

    // Reachability is checked again at most once a second.
    system_resolver.set_ipv6_is_globally_reachable(true);
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1100), [&] { loop.quit(0); });
    timer->start();
    loop.exec();

    // The cached result never asked for IPv6 addresses, so it does not answer for them now that IPv6 is reachable.
    auto third = TRY_OR_FAIL(lookup());
    EXPECT(system_resolver.ipv6_answered());
    EXPECT_EQ(third->cached_addresses().size(), 2u);
    EXPECT(has_address_of_family(*third, false));
    EXPECT(has_address_of_family(*third, true));
}

TEST_CASE(test_system_resolver_does_not_join_a_lookup_without_ipv6_addresses_once_ipv6_is_reachable)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(false),
    };

    auto lookup = [&] {
        return resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
    };

    auto first = lookup();

    // Reachability is checked again at most once a second.
    system_resolver.set_ipv6_is_globally_reachable(true);
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1100), [&] { loop.quit(0); });
    timer->start();
    loop.exec();

    // The first lookup is still waiting for IPv4 addresses, but it never asked for IPv6 addresses.
    auto second = lookup();
    bool gave_up = false;
    timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(500), [&] { gave_up = true; });
    timer->start();
    loop.spin_until([&] { return system_resolver.ipv6_answered() || gave_up; });
    EXPECT(system_resolver.ipv6_answered());

    system_resolver.let_ipv4_answer();
    auto first_result = TRY_OR_FAIL(first->await());
    EXPECT_EQ(first_result->cached_addresses().size(), 1u);
    EXPECT(has_address_of_family(*first_result, false));

    auto second_result = TRY_OR_FAIL(second->await());
    EXPECT_EQ(second_result->cached_addresses().size(), 2u);
    EXPECT(has_address_of_family(*second_result, false));
    EXPECT(has_address_of_family(*second_result, true));
}

TEST_CASE(test_system_resolver_does_not_join_a_lookup_without_ipv4_addresses_once_ipv6_is_unreachable)
{
    Core::EventLoop loop;
    SystemResolverWithHeldBackIPv4 system_resolver;
    DNS::Resolver resolver {
        [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> { return OptionalNone {}; },
        system_resolver.system_resolver(),
    };

    auto lookup = [&] {
        return resolver.lookup("example.com"sv, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA });
    };

    // The first lookup goes ahead with IPv6 addresses alone while it keeps waiting for IPv4 addresses.
    auto first = TRY_OR_FAIL(lookup()->await());
    EXPECT_EQ(first->cached_addresses().size(), 1u);
    EXPECT(has_address_of_family(*first, true));

    // Reachability is checked again at most once a second.
    system_resolver.set_ipv6_is_globally_reachable(false);
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(1100), [&] { loop.quit(0); });
    timer->start();
    loop.exec();

    // IPv6 addresses are of no use now, so the second lookup waits for its own IPv4 addresses.
    auto second = lookup();
    system_resolver.let_ipv4_answer();
    auto second_result = TRY_OR_FAIL(second->await());
    EXPECT_EQ(second_result->cached_addresses().size(), 1u);
    EXPECT(has_address_of_family(*second_result, false));

    loop.spin_until([&] { return has_address_of_family(*first, false); });
}
