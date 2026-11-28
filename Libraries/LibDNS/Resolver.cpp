/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibDNS/Resolver.h>
#include <LibThreading/ThreadPool.h>
#include <RustFFI.h>

namespace DNS {

// What outlives the resolver: work on a thread pool worker, and calls deferred to the event loop.
struct Resolver::Shared : public AtomicRefCounted<Shared> {
    Atomic<bool> alive { true };
};

struct Resolver::Connection {
    AK_ALLOC_WITH_KMALLOC;

    Variant<NonnullOwnPtr<Core::UDPSocket>, NonnullOwnPtr<StreamConnection>> socket;
    ByteBuffer receive_buffer {};
};

static u64 now_ms()
{
    return static_cast<u64>(MonotonicTime::now().milliseconds());
}

static FFI::FfiBytes ffi_bytes(StringView string)
{
    return { reinterpret_cast<u8 const*>(string.characters_without_null_termination()), string.length() };
}

static FFI::FfiBytes ffi_bytes(ReadonlyBytes bytes)
{
    return { bytes.data(), bytes.size() };
}

static StringView string_view(FFI::FfiBytes bytes)
{
    return { reinterpret_cast<char const*>(bytes.data), bytes.length };
}

static Core::SocketAddress socket_address(FFI::FfiAddress const& address)
{
    if (address.is_ipv6)
        return { IPv6Address { address.octets }, address.port };
    return { IPv4Address { address.octets[0], address.octets[1], address.octets[2], address.octets[3] }, address.port };
}

static LookupResult::Address ip_address(FFI::FfiAddress const& address)
{
    if (address.is_ipv6)
        return IPv6Address { address.octets };
    return IPv4Address { address.octets[0], address.octets[1], address.octets[2], address.octets[3] };
}

static FFI::FfiAddress ffi_address(LookupResult::Address const& address)
{
    FFI::FfiAddress result {};
    address.visit(
        [&](IPv4Address const& v4) {
            result.is_ipv6 = false;
            auto octets = v4.to_u32();
            memcpy(result.octets, &octets, sizeof(octets));
        },
        [&](IPv6Address const& v6) {
            result.is_ipv6 = true;
            memcpy(result.octets, v6.to_in6_addr_t(), sizeof(result.octets));
        });
    return result;
}

static void append_string(void* context, FFI::FfiBytes bytes)
{
    *static_cast<String*>(context) = MUST(String::from_utf8(string_view(bytes)));
}

static NonnullRefPtr<LookupResult const> take_lookup_result(FFI::LookupResult const* result)
{
    ScopeGuard release = [&] { FFI::dns_lookup_result_release(result); };

    String name;
    FFI::dns_lookup_result_name(result, append_string, &name);

    Vector<LookupResult::Record> records;
    auto count = FFI::dns_lookup_result_record_count(result);
    records.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i) {
        auto record = FFI::dns_lookup_result_record(result, i);
        String text;
        FFI::dns_lookup_result_record_text(result, i, append_string, &text);
        records.unchecked_append({
            .type = static_cast<Messages::ResourceType>(record.rtype),
            .ttl = record.ttl,
            .address = record.has_address ? Optional<LookupResult::Address> { ip_address(record.address) } : OptionalNone {},
            .text = move(text),
        });
    }

    Optional<SecurityStatus> security_status;
    switch (FFI::dns_lookup_result_security_status(result)) {
    case FFI::FfiSecurityStatus::NotValidated:
        break;
    case FFI::FfiSecurityStatus::Secure:
        security_status = SecurityStatus::Secure;
        break;
    case FFI::FfiSecurityStatus::Insecure:
        security_status = SecurityStatus::Insecure;
        break;
    }

    return adopt_ref(*new LookupResult(name.to_byte_string(), move(records), security_status));
}

Resolver::Resolver()
    : m_shared(adopt_ref(*new Shared))
{
    m_resolver = FFI::dns_resolver_create(now_ms(), [](void*) { return Core::Socket::is_ipv6_globally_reachable(); }, nullptr);
    m_tick_timer = Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
        FFI::dns_resolver_on_tick(m_resolver, now_ms());
        run_actions();
    });
}

Resolver::~Resolver()
{
    m_shared->alive.store(false);
    m_tick_timer->stop();
    m_connections.clear();
    FFI::dns_resolver_destroy(m_resolver);
}

void Resolver::set_server(Optional<Server> server)
{
    if (!server.has_value()) {
        FFI::dns_resolver_set_server(m_resolver, FFI::FfiServerTransport::None, {}, 0, now_ms());
    } else {
        auto transport = server->transport == ServerTransport::TLS ? FFI::FfiServerTransport::Tls : FFI::FfiServerTransport::Udp;
        FFI::dns_resolver_set_server(m_resolver, transport, ffi_bytes(server->host), server->port, now_ms());
    }
    run_actions();
}

void Resolver::set_stream_connector(StreamConnector connector)
{
    m_stream_connector = move(connector);
}

void Resolver::set_remote_system_resolver(RemoteSystemResolver resolver)
{
    m_remote_system_resolver = move(resolver);
}

void Resolver::set_loopback_domains(Vector<ByteString> const& domains)
{
    Vector<FFI::FfiBytes> ffi_domains;
    for (auto const& domain : domains)
        ffi_domains.append(ffi_bytes(domain));
    FFI::dns_resolver_set_loopback_domains(m_resolver, ffi_domains.data(), ffi_domains.size());
}

void Resolver::reset_connection()
{
    FFI::dns_resolver_reset_connection(m_resolver, now_ms());
    run_actions();
}

NonnullRefPtr<Resolver::ResultPromise> Resolver::lookup(ByteString name, Messages::Class class_, Vector<Messages::ResourceType> desired_types, LookupOptions options)
{
    Vector<u16> types;
    for (auto type : desired_types)
        types.append(to_underlying(type));

    auto promise = ResultPromise::construct();
    auto id = FFI::dns_resolver_lookup(m_resolver, ffi_bytes(name), to_underlying(class_), types.data(), types.size(), options.validate_dnssec_locally, options.query_aaaa_without_global_ipv6, now_ms());
    m_lookups.set(id, promise);

    // A lookup the resolver can answer at once, from the cache or because the name needs no lookup, settles before
    // this returns.
    TemporaryChange starting { m_lookup_being_started, Optional<u64> { id } };
    run_actions();
    return promise;
}

NonnullRefPtr<Resolver::ResultPromise> Resolver::lookup(ByteString name, Messages::Class class_, LookupOptions options)
{
    return lookup(move(name), class_, Vector { Messages::ResourceType::A, Messages::ResourceType::AAAA }, options);
}

void Resolver::run_actions()
{
    // The outer call drains actions queued by callbacks below.
    if (m_running_actions)
        return;
    TemporaryChange running { m_running_actions, true };

    FFI::FfiAction action {};
    while (FFI::dns_resolver_take_action(m_resolver, &action)) {
        // NB: What the action points to is only valid until the next call on the resolver, so it gets copied first.
        switch (action.kind) {
        case FFI::FfiActionKind::Connect:
            connect(action.connection, to_underlying(action.connect_kind), socket_address(action.address), string_view(action.server_name));
            break;
        case FFI::FfiActionKind::Write:
            write(action.connection, { action.bytes.data, action.bytes.length });
            break;
        case FFI::FfiActionKind::Close:
            // Defer destruction: the connection may be inside its own callback.
            if (auto connection = m_connections.take(action.connection); connection.has_value())
                Core::deferred_invoke([connection = connection.release_value()] { });
            break;
        case FFI::FfiActionKind::SystemResolve:
            resolve_with_system_resolver(action.request, string_view(action.name), action.ipv6);
            break;
        case FFI::FfiActionKind::CancelSystemResolve:
            cancel_system_resolution(action.request);
            break;
        case FFI::FfiActionKind::LookupSucceeded:
        case FFI::FfiActionKind::LookupFailed: {
            ErrorOr<NonnullRefPtr<LookupResult const>> result = Error::from_string_literal("DNS lookup failed");
            if (action.kind == FFI::FfiActionKind::LookupSucceeded) {
                result = take_lookup_result(action.result);
            } else {
                // NB: The message lives as long as the process does; the detail goes away with the action.
                if (action.error_detail.length > 0)
                    dbgln("DNS: {}: {}", string_view(action.error), string_view(action.error_detail));
                result = Error::from_string_view(string_view(action.error));
            }
            auto promise = m_lookups.take(action.lookup);
            if (!promise.has_value())
                break;
            auto settle = [promise = promise.release_value(), result = move(result)] mutable {
                if (result.is_error())
                    promise->reject(result.release_error());
                else
                    promise->resolve(result.release_value());
            };
            // Nobody waits on a promise that lookup() has yet to return, so it can settle right away. Otherwise,
            // callers hear back from the top of the event loop, not from inside whatever made the lookup settle.
            if (action.lookup == m_lookup_being_started)
                settle();
            else
                Core::deferred_invoke(move(settle));
            break;
        }
        }
    }

    schedule_tick();
}

void Resolver::schedule_tick()
{
    auto wait = FFI::dns_resolver_next_deadline(m_resolver, now_ms());
    if (wait < 0) {
        m_tick_timer->stop();
        return;
    }
    m_tick_timer->restart(AK::Duration::from_milliseconds(wait));
}

void Resolver::connect(u64 connection, u8 kind, Core::SocketAddress const& address, ByteString const& server_name)
{
    auto fail_later = [this, shared = m_shared, connection](Error error) {
        Core::deferred_invoke([this, shared, connection, error = ByteString::formatted("{}", error)] {
            if (!shared->alive.load())
                return;
            FFI::dns_resolver_on_connect_failed(m_resolver, connection, ffi_bytes(error), now_ms());
            run_actions();
        });
    };

    if (kind == to_underlying(FFI::FfiConnectKind::Udp)) {
        auto socket_or_error = Core::UDPSocket::connect(address);
        if (socket_or_error.is_error())
            return fail_later(socket_or_error.release_error());
        auto socket = socket_or_error.release_value();
        // NB: A closed connection goes away from the top of the event loop, so its callbacks can outlive the resolver.
        socket->on_ready_to_read = [this, connection, shared = m_shared] {
            if (!shared->alive.load())
                return;
            auto it = m_connections.find(connection);
            if (it == m_connections.end())
                return;
            auto& socket = *it->value->socket.get<NonnullOwnPtr<Core::UDPSocket>>();
            auto& buffer = it->value->receive_buffer;
            while (true) {
                auto readable = socket.can_read_without_blocking();
                if (readable.is_error() || !readable.value())
                    break;
                auto datagram = socket.read_some(buffer);
                if (datagram.is_error()) {
                    FFI::dns_resolver_on_connect_failed(m_resolver, connection, ffi_bytes(ByteString::formatted("{}", datagram.error())), now_ms());
                    break;
                }
                // Core treats an empty read as EOF and stops notifying us, so this socket is done even on UDP.
                if (datagram.value().is_empty()) {
                    FFI::dns_resolver_on_eof(m_resolver, connection, now_ms());
                    break;
                }
                FFI::dns_resolver_on_received(m_resolver, connection, ffi_bytes(datagram.value()), now_ms());
                if (!m_connections.contains(connection))
                    break;
            }
            run_actions();
        };
        socket->set_notifications_enabled(true);
        m_connections.set(connection, make<Connection>(move(socket), MUST(ByteBuffer::create_uninitialized(NumericLimits<u16>::max()))));

        // A UDP socket has nothing to wait for.
        Core::deferred_invoke([this, shared = m_shared, connection] {
            if (!shared->alive.load())
                return;
            FFI::dns_resolver_on_connected(m_resolver, connection, now_ms());
            run_actions();
        });
        return;
    }

    if (!m_stream_connector)
        return fail_later(Error::from_string_literal("No way to connect a stream to the DNS server"));

    auto security = kind == to_underlying(FFI::FfiConnectKind::Tls) ? StreamSecurity::TLS : StreamSecurity::None;
    auto stream_or_error = m_stream_connector(address, server_name, security);
    if (stream_or_error.is_error())
        return fail_later(stream_or_error.release_error());

    auto stream = stream_or_error.release_value();
    // NB: A closed connection goes away from the top of the event loop, so its callbacks can outlive the resolver.
    stream->on_connected = [this, connection, shared = m_shared] {
        if (!shared->alive.load())
            return;
        FFI::dns_resolver_on_connected(m_resolver, connection, now_ms());
        run_actions();
    };
    stream->on_data = [this, connection, shared = m_shared](ReadonlyBytes bytes) {
        if (!shared->alive.load())
            return;
        FFI::dns_resolver_on_received(m_resolver, connection, ffi_bytes(bytes), now_ms());
        run_actions();
    };
    stream->on_eof = [this, connection, shared = m_shared] {
        if (!shared->alive.load())
            return;
        FFI::dns_resolver_on_eof(m_resolver, connection, now_ms());
        run_actions();
    };
    stream->on_error = [this, connection, shared = m_shared](Error error) {
        if (!shared->alive.load())
            return;
        FFI::dns_resolver_on_connect_failed(m_resolver, connection, ffi_bytes(ByteString::formatted("{}", error)), now_ms());
        run_actions();
    };
    m_connections.set(connection, make<Connection>(move(stream)));
}

void Resolver::write(u64 connection, ReadonlyBytes bytes)
{
    auto it = m_connections.find(connection);
    if (it == m_connections.end())
        return;
    it->value->socket.visit(
        [&](NonnullOwnPtr<Core::UDPSocket>& socket) {
            if (auto result = socket->write_until_depleted(bytes); result.is_error()) {
                auto error = ByteString::formatted("{}", result.error());
                Core::deferred_invoke([this, shared = m_shared, connection, error] {
                    if (!shared->alive.load())
                        return;
                    FFI::dns_resolver_on_connect_failed(m_resolver, connection, ffi_bytes(error), now_ms());
                    run_actions();
                });
            }
        },
        [&](NonnullOwnPtr<StreamConnection>& stream) {
            stream->write(bytes);
        });
}

// A system lookup the resolver may give up on: then the remote resolver forgets it, and a worker skips it.
struct Resolver::SystemRequest {
    AK_ALLOC_WITH_KMALLOC;

    struct Abandoned : public AtomicRefCounted<Abandoned> {
        Atomic<bool> value { false };
    };

    CancelRemoteSystemResolution cancel_remote_resolution;
    NonnullRefPtr<Abandoned> abandoned { adopt_ref(*new Abandoned) };
};

void Resolver::set_system_resolution_timeout_for_testing(AK::Duration timeout)
{
    FFI::dns_resolver_set_system_resolver_timeout(m_resolver, timeout.to_milliseconds());
}

void Resolver::cancel_system_resolution(u64 request)
{
    auto system_request = m_system_requests.take(request);
    if (!system_request.has_value())
        return;
    (*system_request)->abandoned->value.store(true);
    if (auto& cancel = (*system_request)->cancel_remote_resolution)
        cancel();
}

void Resolver::resolve_with_system_resolver(u64 request, ByteString name, bool ipv6)
{
    auto family = ipv6 ? Core::Socket::AddressFamily::IPv6Only : Core::Socket::AddressFamily::IPv4Only;
    auto& system_request = *m_system_requests.ensure(request, [] { return make<SystemRequest>(); });
    auto answer = [this, shared = m_shared, request](SystemResolution resolution) {
        if (!shared->alive.load())
            return;
        // An answer to a lookup the resolver gave up on is dropped.
        if (!m_system_requests.remove(request))
            return;
        Vector<FFI::FfiAddress> addresses;
        if (!resolution.is_error()) {
            for (auto const& address : resolution.value())
                addresses.append(ffi_address(address));
        }
        FFI::dns_resolver_on_system_resolved(m_resolver, request, !resolution.is_error(), addresses.data(), addresses.size(), now_ms());
        run_actions();
    };

    if (m_remote_system_resolver) {
        // The remote resolver answers later, never from inside this call; still, nothing here relies on that.
        system_request.cancel_remote_resolution = m_remote_system_resolver(name, family, [answer = move(answer)](SystemResolution resolution) mutable {
            Core::deferred_invoke([answer = move(answer), resolution = move(resolution)] mutable {
                answer(move(resolution));
            });
        });
        return;
    }

    // Run blocking getaddrinfo() on a worker.
    Threading::ThreadPool::for_name_resolution().submit([name = move(name), family, abandoned = system_request.abandoned, event_loop = Core::EventLoop::current_weak(), answer = move(answer)] mutable {
        // A lookup given up on while it waited in the queue is not made, so the worker goes on to the next one.
        if (abandoned->value.load())
            return;
        auto resolution = Core::Socket::resolve_host(name, Core::Socket::SocketType::Stream, family);
        event_loop->deferred_invoke([answer = move(answer), resolution = move(resolution)] mutable {
            answer(move(resolution));
        });
    });
}

}
