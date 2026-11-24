/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/Function.h>
#include <AK/IPv4Address.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibCore/UDPServer.h>
#include <LibDNS/Resolver.h>
#include <LibTest/TestCase.h>

#include <AK/Windows.h>

namespace {

// The resolver's wire format lives in Rust and has tests of its own there. These only need a canned answer: the query
// echoed back as a response, with one A record for the queried name.
ByteBuffer build_dns_response(ReadonlyBytes query)
{
    auto response = MUST(ByteBuffer::copy(query));
    // QR: this is a response. RA: recursion is available.
    response[2] |= 0x80;
    response[3] |= 0x80;
    // ANCOUNT = 1, and no additional records (the query may have carried an OPT record).
    response[6] = 0;
    response[7] = 1;
    response[10] = 0;
    response[11] = 0;

    // Drop whatever follows the question: name, QTYPE, QCLASS.
    size_t offset = 12;
    while (response[offset] != 0)
        offset += response[offset] + 1;
    offset += 1 + 4;
    response.trim(offset, false);

    u8 const answer[] = {
        0xc0,
        0x0c, // NAME: a pointer to the question's name
        0x00,
        0x01, // TYPE: A
        0x00,
        0x01, // CLASS: IN
        0x00,
        0x00,
        0x01,
        0x2c, // TTL: 300
        0x00,
        0x04, // RDLENGTH
        192,
        0,
        2,
        1, // RDATA
    };
    response.append(answer, sizeof(answer));
    return response;
}

ByteBuffer frame(ReadonlyBytes message)
{
    ByteBuffer framed;
    BigEndian<u16> length = message.size();
    framed.append(&length, sizeof(length));
    framed.append(message);
    return framed;
}

// System-lookup workers post results to their originating event loop and expect it to outlive them.
// Share one process-lifetime loop; each test runs it until its goal is reached.
Core::EventLoop& event_loop()
{
    static auto& event_loop = *new Core::EventLoop;
    VERIFY(&Core::EventLoop::current() == &event_loop);
    return event_loop;
}

// Runs the event loop until the goal is reached, or fails the test once that takes too long.
[[nodiscard]] bool spin_until(Function<bool()> goal)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
    timer->start();
    event_loop().spin_until([&] { return timed_out || goal(); });
    timer->stop();
    if (timed_out) {
        FAIL("Timed out waiting on the event loop");
        return false;
    }
    return true;
}

ErrorOr<NonnullRefPtr<DNS::LookupResult const>> await_lookup(NonnullRefPtr<DNS::Resolver::ResultPromise> const& promise)
{
    Optional<ErrorOr<NonnullRefPtr<DNS::LookupResult const>>> result;
    promise->when_resolved([&](auto& lookup_result) { result = lookup_result; })
        .when_rejected([&](auto& error) { result = Error::copy(error); });
    auto settled = spin_until([&] { return result.has_value(); });
    // NB: A lookup that settles after all must not reach into this frame.
    promise->on_resolution = nullptr;
    promise->on_rejection = nullptr;
    if (!settled)
        return Error::from_string_literal("The lookup did not settle");
    return result.release_value();
}

bool is_pending(DNS::Resolver::ResultPromise& promise)
{
    return !promise.is_resolved() && !promise.is_rejected();
}

// A stream the test drives by hand: it connects when the test says so, and answers what is written to it.
class FakeStream final : public DNS::StreamConnection {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual void write(ReadonlyBytes bytes) override
    {
        written.append(bytes);
        while (written.size() >= 2) {
            size_t length = written[0] << 8 | written[1];
            if (written.size() < 2 + length)
                break;
            auto query = MUST(ByteBuffer::copy(written.bytes().slice(2, length)));
            written = MUST(ByteBuffer::copy(written.bytes().slice(2 + length)));
            ++queries;
            if (answer_queries) {
                auto response = build_dns_response(query);
                auto framed = frame(response);
                on_data(framed.bytes());
            }
        }
    }

    ByteBuffer written;
    size_t queries { 0 };
    bool answer_queries { true };
};

}

TEST_CASE(test_udp)
{
    event_loop();
    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    size_t queries = 0;
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        ++queries;
        auto response = build_dns_response(query.bytes());
        MUST(server->send(response, from));
    };

    DNS::Resolver resolver;
    resolver.set_server(DNS::Resolver::Server { "127.0.0.1", server->local_port().value(), DNS::Resolver::ServerTransport::UDP });

    auto result = TRY_OR_FAIL(await_lookup(resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })));
    auto addresses = result->cached_addresses();
    EXPECT_EQ(addresses.size(), 1u);
    EXPECT_EQ(addresses.first().get<IPv4Address>(), (IPv4Address { 192, 0, 2, 1 }));
    EXPECT_EQ(result->records().first().text, "192.0.2.1"sv);

    // The second lookup comes from the cache.
    (void)TRY_OR_FAIL(await_lookup(resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })));
    EXPECT_EQ(queries, 1u);
}

TEST_CASE(test_tls_lookups_wait_for_the_handshake_without_blocking)
{
    event_loop();
    FakeStream* stream = nullptr;
    Optional<ByteString> server_name;

    DNS::Resolver resolver;
    resolver.set_stream_connector([&](Core::SocketAddress const& address, ByteString const& name, DNS::StreamSecurity security) -> ErrorOr<NonnullOwnPtr<DNS::StreamConnection>> {
        EXPECT_EQ(address.port(), 853);
        EXPECT(security == DNS::StreamSecurity::TLS);
        server_name = name;
        auto fake = make<FakeStream>();
        stream = fake.ptr();
        return fake;
    });
    resolver.set_server(DNS::Resolver::Server { "9.9.9.9", 853, DNS::Resolver::ServerTransport::TLS });

    auto promise = resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A });

    // The event loop keeps going while the handshake is under way.
    bool timer_fired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] { timer_fired = true; });
    timer->start();
    if (!spin_until([&] { return timer_fired; }))
        return;
    if (!stream) {
        FAIL("The lookup did not connect to the DNS server");
        return;
    }
    EXPECT_EQ(server_name, "9.9.9.9"sv);
    EXPECT(is_pending(*promise));
    EXPECT_EQ(stream->queries, 0u);

    stream->on_connected();
    (void)TRY_OR_FAIL(await_lookup(promise));
    EXPECT_EQ(stream->queries, 1u);
}

TEST_CASE(test_tls_handshake_failure_fails_lookups)
{
    event_loop();
    FakeStream* stream = nullptr;
    DNS::Resolver resolver;
    resolver.set_stream_connector([&](auto const&, auto const&, auto) -> ErrorOr<NonnullOwnPtr<DNS::StreamConnection>> {
        auto fake = make<FakeStream>();
        stream = fake.ptr();
        return fake;
    });
    resolver.set_server(DNS::Resolver::Server { "9.9.9.9", 853, DNS::Resolver::ServerTransport::TLS });

    auto promise = resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A });
    if (!spin_until([&] { return stream != nullptr; }))
        return;
    stream->on_error(Error::from_string_literal("certificate verify failed"));
    EXPECT(await_lookup(promise).is_error());
}

TEST_CASE(test_tls_without_a_stream_connector_fails_lookups)
{
    event_loop();
    DNS::Resolver resolver;
    resolver.set_server(DNS::Resolver::Server { "9.9.9.9", 853, DNS::Resolver::ServerTransport::TLS });
    EXPECT(await_lookup(resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })).is_error());
}

TEST_CASE(test_localhost_resolves_to_loopback_without_a_socket)
{
    event_loop();
    DNS::Resolver resolver;
    resolver.set_server(DNS::Resolver::Server { "192.0.2.53", 53, DNS::Resolver::ServerTransport::UDP });
    resolver.set_loopback_domains({ "test" });

    for (auto name : { "localhost"sv, "app.localhost"sv, "web-platform.test"sv }) {
        auto result = TRY_OR_FAIL(await_lookup(resolver.lookup(name)));
        auto addresses = result->cached_addresses();
        EXPECT_EQ(addresses.size(), 2u);
        EXPECT(addresses.contains_slow(IPv4Address { 127, 0, 0, 1 }));
        EXPECT(addresses.contains_slow(IPv6Address::loopback()));
    }
}

TEST_CASE(test_remote_system_resolver_answers_lookups)
{
    event_loop();
    DNS::Resolver resolver;
    Vector<ByteString> asked;
    resolver.set_remote_system_resolver([&](ByteString const& name, Core::Socket::AddressFamily family, Function<void(DNS::Resolver::SystemResolution)> on_resolved) -> DNS::Resolver::CancelRemoteSystemResolution {
        asked.append(name);
        if (family == Core::Socket::AddressFamily::IPv4Only)
            on_resolved(Vector<DNS::LookupResult::Address> { IPv4Address { 192, 0, 2, 9 } });
        else
            on_resolved(Vector<DNS::LookupResult::Address> {});
        return {};
    });

    auto result = TRY_OR_FAIL(await_lookup(resolver.lookup("example.com", DNS::Messages::Class::IN, { .query_aaaa_without_global_ipv6 = true })));
    EXPECT_EQ(result->cached_addresses().first().get<IPv4Address>(), (IPv4Address { 192, 0, 2, 9 }));
    EXPECT_EQ(asked.size(), 2u);
}

TEST_CASE(test_lookup_fails_when_the_remote_system_resolver_fails)
{
    event_loop();
    DNS::Resolver resolver;
    resolver.set_remote_system_resolver([](auto const&, auto, Function<void(DNS::Resolver::SystemResolution)> on_resolved) -> DNS::Resolver::CancelRemoteSystemResolution {
        on_resolved(Error::from_string_literal("No such host"));
        return {};
    });
    EXPECT(await_lookup(resolver.lookup("example.com")).is_error());
}

TEST_CASE(test_resource_type_names)
{
    event_loop();
    EXPECT_EQ(DNS::Messages::to_string(DNS::Messages::ResourceType::AAAA), "AAAA"sv);
    EXPECT(DNS::Messages::resource_type_from_string("HTTPS"sv) == DNS::Messages::ResourceType::HTTPS);
    EXPECT(!DNS::Messages::resource_type_from_string("NOPE"sv).has_value());
}

// Callers may rely on a lookup the resolver can answer at once having settled when lookup() returns.
TEST_CASE(test_lookups_answered_at_once_settle_before_lookup_returns)
{
    event_loop();

    auto server = Core::UDPServer::construct();
    EXPECT(server->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    server->on_ready_to_receive = [&] {
        sockaddr_in from {};
        auto query = MUST(server->receive(4096, from));
        auto response = build_dns_response(query.bytes());
        MUST(server->send(response, from));
    };

    DNS::Resolver resolver;
    resolver.set_server(DNS::Resolver::Server { "127.0.0.1", server->local_port().value(), DNS::Resolver::ServerTransport::UDP });

    EXPECT(resolver.lookup("localhost")->is_resolved());
    EXPECT(resolver.lookup("192.0.2.7")->is_resolved());

    auto first = resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A });
    EXPECT(!first->is_resolved());
    (void)TRY_OR_FAIL(await_lookup(first));

    EXPECT(resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A })->is_resolved());
}

TEST_CASE(test_remote_system_resolver_lookups_given_up_on_are_cancelled)
{
    event_loop();
    DNS::Resolver resolver;
    resolver.set_system_resolution_timeout_for_testing(AK::Duration::from_milliseconds(100));
    // The remote resolver never answers, and keeps what it was asked to answer with. Both families are asked about,
    // whether or not this host reaches the IPv6 internet.
    Vector<Function<void(DNS::Resolver::SystemResolution)>> unanswered;
    size_t cancelled = 0;
    resolver.set_remote_system_resolver([&](auto const&, auto, Function<void(DNS::Resolver::SystemResolution)> on_resolved) -> DNS::Resolver::CancelRemoteSystemResolution {
        unanswered.append(move(on_resolved));
        return [&] { ++cancelled; };
    });

    EXPECT(await_lookup(resolver.lookup("example.com", DNS::Messages::Class::IN, { .query_aaaa_without_global_ipv6 = true })).is_error());
    EXPECT_EQ(unanswered.size(), 2u);
    EXPECT_EQ(cancelled, 2u);

    // Answers that come anyway are dropped.
    for (auto& on_resolved : unanswered)
        on_resolved(Vector<DNS::LookupResult::Address> { IPv4Address { 192, 0, 2, 9 } });
    bool spun = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::zero(), [&] { spun = true; });
    timer->start();
    EXPECT(spin_until([&] { return spun; }));

    // So the next lookup asks again.
    unanswered.clear();
    auto promise = resolver.lookup("example.com", DNS::Messages::Class::IN, { .query_aaaa_without_global_ipv6 = true });
    EXPECT_EQ(unanswered.size(), 2u);
    for (auto& on_resolved : unanswered)
        on_resolved(Vector<DNS::LookupResult::Address> { IPv4Address { 192, 0, 2, 10 } });
    auto result = TRY_OR_FAIL(await_lookup(promise));
    EXPECT_EQ(result->cached_addresses().first().get<IPv4Address>(), (IPv4Address { 192, 0, 2, 10 }));
}

// A closed connection goes away from the top of the event loop, and its callbacks must leave a resolver that is gone
// by then alone.
TEST_CASE(test_closed_stream_outliving_the_resolver_is_ignored)
{
    event_loop();

    FakeStream* stream = nullptr;
    {
        DNS::Resolver resolver;
        resolver.set_stream_connector([&](auto const&, auto const&, auto) -> ErrorOr<NonnullOwnPtr<DNS::StreamConnection>> {
            auto fake = make<FakeStream>();
            stream = fake.ptr();
            return fake;
        });
        resolver.set_server(DNS::Resolver::Server { "9.9.9.9", 853, DNS::Resolver::ServerTransport::TLS });
        (void)resolver.lookup("example.com", DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A });
        resolver.reset_connection();
    }
    if (!stream) {
        FAIL("The lookup did not connect to the DNS server");
        return;
    }

    stream->on_connected();
    u8 const bytes[] = { 0, 1, 0 };
    stream->on_data({ bytes, sizeof(bytes) });
    stream->on_eof();

    bool spun = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::zero(), [&] { spun = true; });
    timer->start();
    EXPECT(spin_until([&] { return spun; }));
}
