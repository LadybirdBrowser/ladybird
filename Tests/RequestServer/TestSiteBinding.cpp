/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/NetworkIsolationKey.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

namespace {

enum class Navigation {
    None,
    TopLevel,
    CrossSiteTopLevel,
    Subframe,
};

HTTP::NetworkIsolationKey key(StringView top_level_site, Optional<StringView> frame_site, Navigation navigation = Navigation::None)
{
    return HTTP::NetworkIsolationKey {
        .top_level_site = Utf16String::from_utf8(top_level_site),
        .frame_site = frame_site.map([](auto site) { return Utf16String::from_utf8(site); }),
        .is_subframe_document = navigation == Navigation::Subframe,
        .is_cross_site_main_frame_navigation = navigation == Navigation::CrossSiteTopLevel,
    };
}

struct TestServer {
    TestServer()
    {
        static bool libcurl_initialized = [] {
            MUST(RequestServer::initialize_libcurl());
            return true;
        }();
        (void)libcurl_initialized;

        auto cache_root = LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv);
        disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, cache_root));

        auto control_pair = MUST(IPC::Transport::create_paired());
        control_remote_transport = MUST(control_pair.remote_handle.create_transport());
        control_connection = RequestServer::ControlConnectionFromClient::construct(
            move(control_pair.local), connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

        auto pair = MUST(IPC::Transport::create_paired());
        remote_transport = MUST(pair.remote_handle.create_transport());
        connection = RequestServer::ConnectionFromClient::construct(
            move(pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Bound,
            connections, request_transfer_leases, *disk_cache);

#ifdef AK_OS_WINDOWS
        auto pid = Core::System::getpid();
        control_connection->transport().set_peer_pid(pid);
        control_remote_transport->set_peer_pid(pid);
        connection->transport().set_peer_pid(pid);
        remote_transport->set_peer_pid(pid);
#endif
    }

    ~TestServer()
    {
        if (connection->is_open())
            connection->shutdown();
        if (control_connection->is_open())
            control_connection->shutdown();
    }

    void bind_to_site(StringView top_level_site, Optional<StringView> frame_site)
    {
        auto message = make<Messages::RequestServerControl::BindClientToSite>(
            connection->client_id(), Utf16String::from_utf8(top_level_site), frame_site.map([](auto site) { return Utf16String::from_utf8(site); }));
        auto response = MUST(static_cast<RequestServerControlEndpoint::Stub&>(*control_connection).handle(move(message)));
        VERIFY(response);
    }

    // Asks RequestServer to create a cache entry in the partition of the key, and returns whether the entry exists.
    bool create_synthetic_cache_entry(HTTP::NetworkIsolationKey const& network_isolation_key)
    {
        auto url = URL::Parser::basic_parse("https://cdn.example/library.js"sv).release_value();
        auto message = make<Messages::RequestServer::CreateSyntheticCacheEntry>(network_isolation_key, url, ByteString { "GET" });
        VERIFY(MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));

        // NB: Associated data can be stored only for an entry that exists.
        auto request_headers = HTTP::HeaderList::create();
        return MUST(disk_cache->store_associated_data(*network_isolation_key.disk_cache_partition(), url, "GET"sv, *request_headers, 0, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, "data"sv.bytes()));
    }

    void start_request(u64 request_id, StringView url, HTTP::NetworkIsolationKey const& network_isolation_key)
    {
        auto message = make<Messages::RequestServer::StartRequest>(
            request_id, ByteString { "GET" }, URL::Parser::basic_parse(url).release_value(), Vector<HTTP::Header> {}, ByteBuffer {},
            HTTP::CacheMode::NoStore, network_isolation_key, HTTP::Cookie::IncludeCredentials::No, false, Optional<u32> {}, false, 0, 0);
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    }

    // Runs the event loop until RequestServer reports the request as finished, and returns its network error.
    Optional<Requests::NetworkError> wait_for_request_finished(u64 request_id)
    {
        // The client transport is not part of the event loop here, so keep the loop from sleeping until it has news.
        auto wake_timer = Core::Timer::create_repeating(10, [] { });
        wake_timer->start();

        Optional<Optional<Requests::NetworkError>> network_error;
        event_loop.spin_until([&] {
            (void)remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (message->message_id() != Messages::RequestClient::RequestFinished::static_message_id())
                    return;
                auto& finished = static_cast<Messages::RequestClient::RequestFinished&>(*message);
                if (finished.request_id() == request_id)
                    network_error = finished.network_error();
            });
            return network_error.has_value();
        });
        return network_error.release_value();
    }

    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    Optional<HTTP::DiskCache> disk_cache;
    OwnPtr<IPC::Transport> control_remote_transport;
    RefPtr<RequestServer::ControlConnectionFromClient> control_connection;
    OwnPtr<IPC::Transport> remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> connection;
};

}

TEST_CASE(bound_client_uses_only_the_cache_partitions_of_its_sites)
{
    TestServer server;

    EXPECT(!server.create_synthetic_cache_entry(key("https://a.example"sv, "https://a.example"sv)));

    server.bind_to_site("https://a.example"sv, "https://a.example"sv);
    EXPECT(server.create_synthetic_cache_entry(key("https://a.example"sv, "https://a.example"sv)));

    // Neither another top-level site nor another frame site under the bound one is reachable.
    EXPECT(!server.create_synthetic_cache_entry(key("https://b.example"sv, "https://b.example"sv)));
    EXPECT(!server.create_synthetic_cache_entry(key("https://b.example"sv, "https://a.example"sv)));
    EXPECT(!server.create_synthetic_cache_entry(key("https://a.example"sv, "https://b.example"sv)));

    server.bind_to_site("https://a.example"sv, "https://b.example"sv);
    EXPECT(server.create_synthetic_cache_entry(key("https://a.example"sv, "https://b.example"sv)));
}

TEST_CASE(bound_client_requests_only_for_its_sites)
{
    TestServer server;
    server.bind_to_site("http://localhost"sv, "http://localhost"sv);

    // NB: Port 2 has no listener. Port 1 would fail the bad-port check before reaching the network.
    server.start_request(1, "http://localhost:2/"sv, key("http://localhost"sv, "http://localhost"sv));
    EXPECT(server.wait_for_request_finished(1) == Requests::NetworkError::UnableToConnect);

    // A subresource request for a site the client is not bound to is refused.
    server.start_request(2, "http://localhost:2/"sv, key("https://b.example"sv, "https://c.example"sv));
    EXPECT(server.wait_for_request_finished(2) == Requests::NetworkError::Unknown);

    // A cross-site navigation request may be made for the site of its URL, as the process of the navigating document
    // makes it.
    server.start_request(3, "http://127.0.0.1:2/"sv, key("http://127.0.0.1"sv, "http://127.0.0.1"sv, Navigation::CrossSiteTopLevel));
    EXPECT(server.wait_for_request_finished(3) == Requests::NetworkError::UnableToConnect);

    // ...but a key without the cross-site flag, which the site's own documents use for their subresources, is refused.
    server.start_request(6, "http://127.0.0.1:2/"sv, key("http://127.0.0.1"sv, "http://127.0.0.1"sv, Navigation::TopLevel));
    EXPECT(server.wait_for_request_finished(6) == Requests::NetworkError::Unknown);

    // A child navigable's navigation request may be made under a bound top-level site, for the site of its URL.
    server.start_request(4, "http://127.0.0.1:2/"sv, key("http://localhost"sv, "http://127.0.0.1"sv, Navigation::Subframe));
    EXPECT(server.wait_for_request_finished(4) == Requests::NetworkError::UnableToConnect);

    // ...but not under a top-level site the client is not bound to.
    server.start_request(5, "http://127.0.0.1:2/"sv, key("https://b.example"sv, "http://127.0.0.1"sv, Navigation::Subframe));
    EXPECT(server.wait_for_request_finished(5) == Requests::NetworkError::Unknown);
}
