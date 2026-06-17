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
        [server_port] -> ErrorOr<DNS::Resolver::SocketResult> {
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
        [server_port] -> ErrorOr<DNS::Resolver::SocketResult> {
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
