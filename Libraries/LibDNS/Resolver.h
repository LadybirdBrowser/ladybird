/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Variant.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibCore/Socket.h>
#include <LibCore/SocketAddress.h>
#include <LibDNS/Export.h>
#include <LibDNS/LookupResult.h>
#include <LibDNS/Types.h>

namespace DNS {

namespace FFI {

struct DnsResolver;

}

// A byte stream to a DNS server that someone else connects, such as over TLS. It must not block: it reports what
// happens through its callbacks, from the event loop it was made on.
class DNS_API StreamConnection {
public:
    virtual ~StreamConnection() = default;

    virtual void write(ReadonlyBytes) = 0;

    // Once connected, and done with the TLS handshake if there is one.
    Function<void()> on_connected;
    Function<void(ReadonlyBytes)> on_data;
    Function<void()> on_eof;
    Function<void(Error)> on_error;
};

enum class StreamSecurity : u8 {
    None,
    TLS,
};

using StreamConnector = Function<ErrorOr<NonnullOwnPtr<StreamConnection>>(Core::SocketAddress const&, ByteString const& server_name, StreamSecurity)>;

// A stub resolver. It never blocks: sockets, timers and the system resolver all report back through the event loop.
class DNS_API Resolver {
    AK_MAKE_NONCOPYABLE(Resolver);
    AK_MAKE_NONMOVABLE(Resolver);

public:
    struct LookupOptions {
        bool validate_dnssec_locally { false };
        // Asks the system resolver for AAAA records even when no global IPv6 route exists, as a host on the local
        // network may still be reachable over IPv6.
        bool query_aaaa_without_global_ipv6 { false };

        static LookupOptions default_() { return {}; }
    };

    enum class ServerTransport : u8 {
        // UDP, asked again over TCP when a response comes back truncated.
        UDP,
        // RFC 7858 DNS over TLS.
        TLS,
    };

    struct Server {
        // A host name, which the system resolver looks up, or an address.
        ByteString host;
        u16 port { 0 };
        ServerTransport transport { ServerTransport::UDP };

        bool operator==(Server const&) const = default;
    };

    using ResultPromise = Core::Promise<NonnullRefPtr<LookupResult const>>;

    using SystemResolution = ErrorOr<Vector<LookupResult::Address>>;
    // Resolves a name to the addresses of one family somewhere else, such as in another process, and calls back on the
    // thread that asked, once it has them.
    // Forgets a lookup handed to the remote resolver, whose answer, if any comes, is then dropped.
    using CancelRemoteSystemResolution = Function<void()>;
    using RemoteSystemResolver = Function<CancelRemoteSystemResolution(ByteString const&, Core::Socket::AddressFamily, Function<void(SystemResolution)>)>;

    Resolver();
    ~Resolver();

    // Without a server, every name goes to the system resolver.
    void set_server(Optional<Server>);

    // Where streams to the server come from: DNS over TLS, and TCP for responses too large for UDP.
    void set_stream_connector(StreamConnector);

    // Once set, the system resolver's lookups go to the remote resolver, rather than to getaddrinfo() on a thread pool.
    void set_remote_system_resolver(RemoteSystemResolver);

    // Names within these domains resolve to loopback, as names within "localhost" do.
    void set_loopback_domains(Vector<ByteString> const&);

    // How long the system resolver may take to answer, for tests that cannot wait for it.
    void set_system_resolution_timeout_for_testing(AK::Duration);

    // Drops the connection, everything in flight on it, and everything cached.
    void reset_connection();

    NonnullRefPtr<ResultPromise> lookup(ByteString name, Messages::Class, Vector<Messages::ResourceType> desired_types, LookupOptions = LookupOptions::default_());
    NonnullRefPtr<ResultPromise> lookup(ByteString name, Messages::Class = Messages::Class::IN, LookupOptions = LookupOptions::default_());

private:
    struct Shared;
    struct Connection;
    struct SystemRequest;

    void run_actions();
    void schedule_tick();
    void connect(u64 connection, u8 kind, Core::SocketAddress const&, ByteString const& server_name);
    void write(u64 connection, ReadonlyBytes);
    void resolve_with_system_resolver(u64 request, ByteString name, bool ipv6);
    void cancel_system_resolution(u64 request);

    FFI::DnsResolver* m_resolver { nullptr };
    NonnullRefPtr<Shared> m_shared;
    HashMap<u64, NonnullRefPtr<ResultPromise>> m_lookups;
    HashMap<u64, NonnullOwnPtr<Connection>> m_connections;
    HashMap<u64, NonnullOwnPtr<SystemRequest>> m_system_requests;
    StreamConnector m_stream_connector;
    RemoteSystemResolver m_remote_system_resolver;
    RefPtr<Core::Timer> m_tick_timer;
    Optional<u64> m_lookup_being_started;
    bool m_running_actions { false };
};

}
