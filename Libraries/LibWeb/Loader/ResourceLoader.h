/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Utf16String.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/Forward.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkLoadRequest.h>
#include <LibWeb/Fetch/Engine/NetworkLoader.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>

namespace Web {

WEB_API String network_error_description(Requests::NetworkError);

// Each agent has a loader of its own, which holds its connections to the RequestServer and its requests, and only its
// thread can use it. The static settings are the process's.
class WEB_API ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)

public:
    static void initialize(GC::Heap&);
    static bool is_initialized();

    // Forgets the loader of this agent, once its requests are stopped.
    static void shut_down();
    static ResourceLoader& the();

    static void set_proxy_autoconfig_url(URL::URL);
    static Optional<URL::URL> const& proxy_autoconfig_url();

    // Route requests through their top-level site's client, falling back to the server with no site
    // for requests without a site or without a client for that site.
    void set_client(Optional<Utf16String> const& top_level_site, NonnullRefPtr<Requests::RequestClient>);

    using OnHeadersReceived = Fetch::Engine::NetworkLoader::OnHeadersReceived;
    using OnDataReceived = Fetch::Engine::NetworkLoader::OnDataReceived;
    using OnCachedBodyAvailable = Fetch::Engine::NetworkLoader::OnCachedBodyAvailable;
    using NetworkRequestEnd = Fetch::Engine::NetworkLoader::NetworkRequestEnd;
    using OnComplete = Fetch::Engine::NetworkLoader::OnComplete;

    // A request keeps roots into the heap of the agent that made it.
    void stop_all_requests();

    // The callbacks hold the cells they use in roots.
    // The page, if any, decides whether content blocking applies and reads local files.
    RefPtr<Fetch::Engine::NetworkTransportHandle> load(Fetch::Engine::NetworkLoadRequest&, GC::Ptr<Page>, OnHeadersReceived, OnDataReceived, OnCachedBodyAvailable, OnComplete, Requests::RequestClient::TransferLease = Requests::RequestClient::TransferLease::No);

    RefPtr<Requests::RequestClient> request_client_for(Optional<HTTP::NetworkIsolationKey> const&) const;
    RefPtr<Requests::RequestClient> request_client_for_top_level_site(Optional<Utf16String> const&) const;

    // Sites with clients, excluding the server with no site.
    Vector<Utf16String> top_level_sites() const;
    RefPtr<Requests::RequestClient> take_client_for_testing(Optional<Utf16String> const& top_level_site);

    void prefetch_dns(GC::Ptr<DOM::Document>, URL::URL const&, URL::URL const& source_url);
    void preconnect(GC::Ptr<DOM::Document>, URL::URL const&, URL::URL const& source_url);

    static bool is_preloaded_hsts_host(String const& host);

    // Calls back with true when no RequestServer serves the key's requests.
    void look_up_known_hsts_host(Optional<HTTP::NetworkIsolationKey> const&, String const& host, Function<void(bool is_known)> on_answer);

    // The engine the navigator claims to be. A process-wide setting, readable before the loader exists: the bindings
    // consult it while they build a realm's global object.
    static NavigatorCompatibilityMode navigator_compatibility_mode() { return s_navigator_compatibility_mode; }
    static void set_navigator_compatibility_mode(NavigatorCompatibilityMode);

private:
    explicit ResourceLoader(GC::Heap&);

    RefPtr<Requests::RequestClient>& ensure_client_for(Optional<Utf16String> const& top_level_site);

    struct FileLoadResult {
        ReadonlyBytes data;
        NonnullRefPtr<HTTP::HeaderList> response_headers;
        Requests::RequestTimingInfo timing_info {};
    };
    template<typename FileHandler, typename ErrorHandler>
    void handle_file_load_request(Fetch::Engine::NetworkLoadRequest& request, GC::Ptr<Page>, FileHandler on_file, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_about_load_request(Fetch::Engine::NetworkLoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_resource_load_request(Fetch::Engine::NetworkLoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);

    RefPtr<Requests::Request> start_network_request(Fetch::Engine::NetworkLoadRequest const&, Requests::RequestClient::TransferLease);
    void finish_network_request(NonnullRefPtr<Requests::Request>);

    GC::Heap& m_heap;
    RefPtr<Requests::RequestClient> m_request_client;
    HashMap<Utf16String, RefPtr<Requests::RequestClient>> m_site_request_clients;
    HashTable<NonnullRefPtr<Requests::Request>> m_active_requests;

    static Atomic<NavigatorCompatibilityMode, AK::MemoryOrder::memory_order_relaxed> s_navigator_compatibility_mode;
};

// Loads a fetch's network requests through its agent's resource loader, for the fetch's page.
class PageNetworkLoader final : public Fetch::Engine::NetworkLoader {
public:
    [[nodiscard]] static NonnullRefPtr<PageNetworkLoader> create(Page&);

    virtual bool can_load() const override;
    virtual RefPtr<Fetch::Engine::NetworkTransportHandle> load(Fetch::Engine::NetworkLoadRequest&, OnHeadersReceived, OnDataReceived, OnCachedBodyAvailable, OnComplete, Requests::RequestClient::TransferLease) override;

private:
    explicit PageNetworkLoader(Page&);

    GC::Root<Page> m_page;
};

}
