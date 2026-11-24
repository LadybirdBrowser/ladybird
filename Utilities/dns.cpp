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
#include <RequestServer/DNSStreamCurl.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h> // Needed because curl.h includes winsock2.h
#endif

#include <curl/curl.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    struct Request {
        Vector<DNS::Messages::ResourceType> types;
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
                current_request.types.append(DNS::Messages::ResourceType::ANY);
                current_request.name = parts[0];
            } else {
                auto rr_parts = parts[0].split_view(',');
                for (auto& rr : rr_parts) {
                    ByteString rr_name = rr;
                    auto type = DNS::Messages::resource_type_from_string(rr_name.to_uppercase());
                    if (!type.has_value())
                        return Error::from_string_literal("Invalid resource type");
                    current_request.types.append(type.value());
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

    auto& loop = Core::EventLoop::initialize_for_current_thread();

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
        return Error::from_string_literal("Failed to initialize libcurl");

    DNS::Resolver resolver;
    resolver.set_stream_connector([&](Core::SocketAddress const& address, ByteString const& server_name, DNS::StreamSecurity security) {
        return RequestServer::connect_dns_stream(address, server_name, security, { .root_certificates_path = cert_path });
    });
    resolver.set_server(DNS::Resolver::Server {
        .host = server_address,
        .port = static_cast<u16>(use_tls ? 853 : 53),
        .transport = use_tls ? DNS::Resolver::ServerTransport::TLS : DNS::Resolver::ServerTransport::UDP,
    });

    size_t pending_requests = requests.size();
    Vector<NonnullRefPtr<Core::Promise<NonnullRefPtr<DNS::LookupResult const>>>> promises;
    for (auto& request : requests) {
        promises.append(resolver.lookup(request.name, DNS::Messages::Class::IN, request.types, { .validate_dnssec_locally = dnssec })
                ->when_resolved([&](auto& result) {
                    outln("Resolved {}:", request.name);
                    HashTable<DNS::Messages::ResourceType> types;
                    auto const& recs = result->records();
                    for (auto& record : recs)
                        types.set(record.type);

                    for (auto& type : types) {
                        outln("  - {} IN {}:", request.name, DNS::Messages::to_string(type));
                        for (auto& record : recs) {
                            if (type != record.type)
                                continue;

                            outln("    - {}", record.text);
                        }
                    }

                    if (--pending_requests == 0)
                        loop.quit(0);
                })
                .when_rejected([&](auto& error) {
                    outln("Failed to resolve {} IN {}: {}", request.name, DNS::Messages::to_string(request.types.first()), error);
                    if (--pending_requests == 0)
                        loop.quit(1);
                }));
    }

    return loop.exec();
}
