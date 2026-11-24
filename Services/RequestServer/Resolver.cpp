/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/EventLoop.h>
#include <LibTLS/TLSv12.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static ByteString g_default_certificate_path;

ByteString const& default_certificate_path()
{
    return g_default_certificate_path;
}

void set_default_certificate_path(ByteString default_certificate_path)
{
    g_default_certificate_path = move(default_certificate_path);
}

static HTTP::ProxyConfiguration g_proxy_configuration;
static Optional<Utf16String> g_process_top_level_site;

HTTP::ProxyConfiguration const& proxy_configuration()
{
    return g_proxy_configuration;
}

void set_proxy_configuration(HTTP::ProxyConfiguration proxy_configuration)
{
    g_proxy_configuration = move(proxy_configuration);
}

Optional<Utf16String> const& process_top_level_site()
{
    return g_process_top_level_site;
}

void set_process_top_level_site(Optional<Utf16String> site)
{
    g_process_top_level_site = move(site);
}

bool process_keeps_responses_under(Utf16String const& top_level_site)
{
    return !g_process_top_level_site.has_value() || top_level_site == *g_process_top_level_site;
}

DNSInfo& DNSInfo::the()
{
    static DNSInfo g_dns_info;
    return g_dns_info;
}

static bool s_resolves_system_names_remotely = false;

static void use_remote_system_resolver(DNS::Resolver& resolver)
{
    resolver.set_remote_system_resolver([](ByteString const& host, Core::Socket::AddressFamily family, Function<void(DNS::Resolver::SystemResolution)> on_resolved) -> DNS::Resolver::CancelRemoteSystemResolution {
        if (auto control_connection = ControlConnectionFromClient::the(); control_connection.has_value())
            return control_connection->resolve_host_with_system_resolver(host, family, move(on_resolved));
        // The resolver expects its answer later, never from inside this call.
        Core::deferred_invoke([on_resolved = move(on_resolved)] mutable {
            on_resolved(Error::from_string_literal("No process to resolve the name with the system resolver"));
        });
        return {};
    });
}

// System resolver to resolve the resolvers themselves.
static DNS::Resolver& system_resolver()
{
    static NeverDestroyed<DNS::Resolver> resolver { [] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
        return OptionalNone {};
    } };
    return *resolver;
}

static bool proxy_is_given_by_name(HTTP::Proxy const& proxy)
{
    // A name never contains a colon, so a host with one is an IPv6 address, even with a zone, e.g. fe80::1%eth0.
    return !proxy.host.contains(':') && !IPv4Address::from_string(proxy.host).has_value();
}

static ByteString proxy_key(HTTP::Proxy const& proxy)
{
    return ByteString::formatted("{}:{}", proxy.host, proxy.port);
}

static HashMap<ByteString, ByteString> s_proxy_resolve_entries;

void resolve_proxy_host(Optional<HTTP::Proxy> const& proxy, Function<void(ErrorOr<void>)> on_resolved)
{
    if (!proxy.has_value() || !proxy_is_given_by_name(*proxy)) {
        on_resolved({});
        return;
    }

    // Both outcomes of the lookup call back, so they share the one callback.
    struct Lookup : RefCounted<Lookup> {
        explicit Lookup(Function<void(ErrorOr<void>)> on_resolved)
            : on_resolved(move(on_resolved))
        {
        }
        Function<void(ErrorOr<void>)> on_resolved;
    };
    auto lookup = make_ref_counted<Lookup>(move(on_resolved));

    // Like curl, which this stands in for, look the proxy up with the system resolver, never with a configured DNS
    // server: A proxy is often a local host, which only the system resolver knows.
    auto promise = system_resolver().lookup(proxy->host, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA }, { .query_aaaa_without_global_ipv6 = true });
    promise
        ->when_rejected([lookup](auto const& error) {
            lookup->on_resolved(Error::copy(error));
        })
        .when_resolved([lookup, host = proxy->host, port = proxy->port, key = proxy_key(*proxy)](auto const& dns_result) {
            if (dns_result->is_empty() || !dns_result->has_cached_addresses()) {
                lookup->on_resolved(Error::from_string_literal("The proxy's host has no addresses"));
                return;
            }
            s_proxy_resolve_entries.set(key, build_curl_resolve_list(*dns_result, host, port));
            lookup->on_resolved({});
        });
}

Optional<ByteString> curl_resolve_entry_for_proxy(Optional<HTTP::Proxy> const& proxy)
{
    if (!proxy.has_value() || !proxy_is_given_by_name(*proxy))
        return {};
    return s_proxy_resolve_entries.get(proxy_key(*proxy)).copy();
}

static WeakPtr<Resolver> g_default_resolver;
static WeakPtr<Resolver> g_private_resolver;

NonnullRefPtr<Resolver> Resolver::default_resolver()
{
    if (auto resolver = g_default_resolver.strong_ref())
        return *resolver;

    auto resolver = create();
    g_default_resolver = resolver;
    return resolver;
}

NonnullRefPtr<Resolver> Resolver::private_resolver()
{
    if (auto resolver = g_private_resolver.strong_ref())
        return *resolver;

    auto resolver = create();
    g_private_resolver = resolver;
    return resolver;
}

void Resolver::resolve_system_names_remotely()
{
    if (s_resolves_system_names_remotely)
        return;
    s_resolves_system_names_remotely = true;

    use_remote_system_resolver(system_resolver());
    for (auto* weak_resolver : { &g_default_resolver, &g_private_resolver }) {
        if (auto resolver = weak_resolver->strong_ref())
            use_remote_system_resolver(resolver->dns);
    }
}

void Resolver::reset_connections()
{
    for (auto* weak_resolver : { &g_default_resolver, &g_private_resolver }) {
        if (auto resolver = weak_resolver->strong_ref())
            resolver->dns.reset_connection();
    }
}

NonnullRefPtr<Resolver> Resolver::create()
{
    auto resolver = adopt_ref(*new Resolver([] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
        auto& dns_info = DNSInfo::the();

        if (!dns_info.server_address.has_value()) {
            if (!dns_info.server_hostname.has_value())
                return OptionalNone {};

            auto resolved = TRY(system_resolver().lookup(*dns_info.server_hostname)->await());
            if (!resolved->has_cached_addresses())
                return Error::from_string_literal("Failed to resolve DNS server hostname");

            auto address = resolved->cached_addresses().first().visit([&](auto& addr) -> Core::SocketAddress { return { addr, dns_info.port }; });
            dns_info.server_address = address;
        }

        if (dns_info.use_dns_over_tls) {
            TLS::Options options;

            if (!g_default_certificate_path.is_empty())
                options.root_certificates_path = g_default_certificate_path;

            return DNS::Resolver::SocketResult {
                MaybeOwned<Core::Socket>(TRY(TLS::TLSv12::connect(*dns_info.server_address, *dns_info.server_hostname, move(options)))),
                DNS::Resolver::ConnectionMode::TCP,
            };
        }

        return DNS::Resolver::SocketResult {
            MaybeOwned<Core::Socket>(TRY(Core::UDPSocket::connect(*dns_info.server_address))),
            DNS::Resolver::ConnectionMode::UDP,
            [address = *dns_info.server_address] -> ErrorOr<NonnullOwnPtr<Core::Socket>> {
                return TRY(Core::TCPSocket::connect(address));
            },
        };
    }));
    resolver->dns.set_loopback_domains(DNSInfo::the().loopback_domains);
    if (s_resolves_system_names_remotely)
        use_remote_system_resolver(resolver->dns);
    return resolver;
}

Resolver::Resolver(Function<ErrorOr<Optional<DNS::Resolver::SocketResult>>()> create_socket)
    : dns(move(create_socket))
{
}

}
