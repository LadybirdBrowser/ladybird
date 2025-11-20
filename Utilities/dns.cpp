/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibDNS/Resolver.h>
#include <LibMain/Main.h>
#include <LibTLS/TLSv12.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    struct Request {
        Vector<Vector<DNS::Messages::ResourceType>> types;
        ByteString name;
    };
    Vector<Request> requests;
    Request current_request;
    StringView server_address;
    StringView cert_path;
    bool use_tls = false;
    bool dnssec = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(cert_path, "Path to a root CA certificate file", "ca-certs", 'C', "file");
    args_parser.add_option(server_address, "The address of the DNS server to query", "server", 's', "addr");
    args_parser.add_option(use_tls, "Use TLS to connect to the server", "tls", 0);
    args_parser.add_option(dnssec, "Validate DNSSEC records locally", "dnssec", 0);
    args_parser.add_positional_argument(Core::ArgsParser::Arg {
        .help_string = "The resource types and name of the DNS record to query",
        .name = "rr,rr@name",
        .min_values = 1,
        .max_values = 99999,
        .accept_value = [&](StringView name) -> ErrorOr<bool> {
            auto parts = name.split_view('@');
            if (parts.size() > 2 || parts.is_empty())
                return Error::from_string_literal("Invalid record/name format");

            if (parts.size() == 1) {
                current_request.types.append({ DNS::Messages::ResourceType::ANY });
                current_request.name = parts[0];
            } else {
                auto rr_parts = parts[0].split_view(',');
                for (auto& rr : rr_parts) {
                    ByteString rr_name = rr;
                    auto type = DNS::Messages::resource_type_from_string(rr_name.to_uppercase());
                    if (!type.has_value())
                        return Error::from_string_literal("Invalid resource type");
                    current_request.types.append({ type.value() });
                }
                current_request.name = parts[1];
            }
            requests.append(move(current_request));
            current_request = {};
            return true;
        },
    });

    args_parser.parse(arguments);

    if (server_address.is_empty()) {
        outln("You must specify a server address to query");
        return 1;
    }

    if (current_request.name.is_empty() && !current_request.types.is_empty()) {
        outln("You must specify at least one DNS record to query");
        return 1;
    }

    Core::EventLoop loop;

    DNS::Resolver resolver {
        [&] -> NonnullRefPtr<Core::Promise<DNS::Resolver::SocketResult>> {
            auto promise = Core::Promise<DNS::Resolver::SocketResult>::construct();

            auto make_resolver = [&](Core::SocketAddress const& address) -> ErrorOr<DNS::Resolver::SocketResult> {
                if (use_tls) {
                    TLS::Options options;
                    if (!cert_path.is_empty())
                        options.root_certificates_path = cert_path;

                    auto tls = TRY(TLS::TLSv12::connect(address, server_address, move(options)));
                    return DNS::Resolver::SocketResult { move(tls), DNS::Resolver::ConnectionMode::TCP };
                }

                return DNS::Resolver::SocketResult {
                    TRY(Core::BufferedSocket<Core::UDPSocket>::create(TRY(Core::UDPSocket::connect(address)))),
                    DNS::Resolver::ConnectionMode::UDP,
                };
            };

            if (auto v4 = IPv4Address::from_string(server_address); v4.has_value()) {
                auto result = make_resolver({ v4.value(), static_cast<u16>(use_tls ? 853 : 53) });
                if (!result.is_error()) {
                    promise->resolve(result.release_value());
                } else {
                    promise->reject(result.release_error());
                }
            } else if (auto v6 = IPv6Address::from_string(server_address); v6.has_value()) {
                auto result = make_resolver({ v6.value(), static_cast<u16>(use_tls ? 853 : 53) });
                if (!result.is_error()) {
                    promise->resolve(result.release_value());
                } else {
                    promise->reject(result.release_error());
                }
            } else {
                auto lookup_promise = resolver.lookup(server_address);
                lookup_promise->when_resolved([use_tls, promise, make_resolver = move(make_resolver)](NonnullRefPtr<DNS::LookupResult const> const& result) {
                    result->cached_addresses().first().visit([use_tls, promise, make_resolver = move(make_resolver)](auto& address) {
                        auto resolver_result = make_resolver({ address, static_cast<u16>(use_tls ? 853 : 53) });
                        if (!resolver_result.is_error()) {
                            promise->resolve(resolver_result.release_value());
                        } else {
                            promise->reject(resolver_result.release_error());
                        }
                    });
                }).when_rejected([promise](Error const& error) {
                    promise->reject(Error::copy(error));
                });

                promise->add_child(move(lookup_promise));
            }

            return promise;
        }
    };

    TRY(resolver.when_socket_ready()->await());

    size_t pending_requests = requests.size();
    Vector<NonnullRefPtr<Core::Promise<NonnullRefPtr<DNS::LookupResult const>>>> promises;
    for (auto& request : requests) {
        promises.append(resolver.lookup(request.name, DNS::Messages::Class::IN, request.types, { .validate_dnssec_locally = dnssec })
                ->when_resolved([&](auto& result) {
                    outln("Resolved {}:", request.name);
                    HashTable<DNS::Messages::ResourceType> types;
                    auto recs = result->records();
                    for (auto& record : recs)
                        types.set(record.type);

                    for (auto& type : types) {
                        outln("  - {} IN {}:", request.name, DNS::Messages::to_string(type));
                        for (auto& record : recs) {
                            if (type != record.type)
                                continue;

                            outln("    - {}", record.to_string());
                        }
                    }

                    if (--pending_requests == 0)
                        loop.quit(0);
                })
                .when_rejected([&](auto& error) {
                    outln("Failed to resolve {} IN {}: {}", request.name, DNS::Messages::to_string(request.types.first().first()), error);
                    if (--pending_requests == 0)
                        loop.quit(1);
                }));
    }

    return loop.exec();
}
