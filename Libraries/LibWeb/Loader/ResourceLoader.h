/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Function.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/Forward.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>

namespace Web {

class WEB_API ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)

public:
    static void initialize(GC::Heap&, NonnullRefPtr<Requests::RequestClient>);
    static bool is_initialized();
    static ResourceLoader& the();

    static void set_proxy_autoconfig_url(URL::URL);
    static Optional<URL::URL> const& proxy_autoconfig_url();

    void set_client(NonnullRefPtr<Requests::RequestClient>);

    using OnHeadersReceived = GC::Function<void(Requests::Request*, HTTP::HeaderList const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase, Optional<Core::ImmutableBytes> javascript_bytecode, Optional<u64> javascript_bytecode_cache_vary_key, Requests::CacheState cache_state)>;
    using OnDataReceived = GC::Function<void(Requests::ResponseData data)>;
    using OnCachedBodyAvailable = GC::Function<void(Core::ImmutableBytes data)>;
    using OnComplete = GC::Function<void(bool success, Requests::RequestTimingInfo const& timing_info, Optional<StringView> error_message)>;

    RefPtr<Requests::Request> load(LoadRequest&, GC::Root<OnHeadersReceived>, GC::Root<OnDataReceived>, GC::Root<OnCachedBodyAvailable>, GC::Root<OnComplete>, Requests::RequestClient::TransferLease = Requests::RequestClient::TransferLease::No);

    RefPtr<Requests::RequestClient>& request_client() { return m_request_client; }

    void prefetch_dns(GC::Ptr<DOM::Document>, URL::URL const&, URL::URL const& source_url);
    void preconnect(GC::Ptr<DOM::Document>, URL::URL const&, URL::URL const& source_url);

    static bool is_known_hsts_host(Page&, String const& host);

    String const& user_agent() const { return m_user_agent; }
    String user_agent_for_url(URL::URL const&) const;
    String user_agent_for_websocket_url(URL::URL const&) const;
    void set_user_agent(String user_agent) { m_user_agent = move(user_agent); }

    // The registrable domain of a URL's host, or the host itself for one that has none (an IP address, "localhost").
    static Optional<String> site_for_url(URL::URL const&);
    bool forces_ladybird_user_agent_for_site(StringView site) const { return m_sites_forced_to_ladybird_user_agent.contains(site); }
    // A navigation that was challenged for a spoofed UA and is about to retry as Ladybird begins; its retry's response
    // ends it, as loaded or failed. The site keeps our own UA while any retry is pending, and for good once one loaded.
    enum class RetryAsLadybirdOutcome {
        Loaded,
        Failed,
    };
    void begin_retry_as_ladybird(String site);
    void end_retry_as_ladybird(StringView site, RetryAsLadybirdOutcome);
    void set_site_compatibility_data(SiteCompatibilityData data) { m_site_compatibility_data = move(data); }
    bool site_compatibility_exposes_experimental_interface(URL::URL const& url, StringView name) const { return m_site_compatibility_data.exposes_experimental_interface(url, name); }

    String const& platform() const { return m_platform; }
    void set_platform(String platform) { m_platform = move(platform); }

    Vector<String> const& preferred_languages() const { return m_preferred_languages; }
    void set_preferred_languages(Vector<String> preferred_languages)
    {
        m_preferred_languages = move(preferred_languages);
        VERIFY(!m_preferred_languages.is_empty());
    }

    // The engine the navigator claims to be. A process-wide setting, readable before the loader exists: the bindings
    // consult it while they build a realm's global object.
    static NavigatorCompatibilityMode navigator_compatibility_mode() { return s_navigator_compatibility_mode; }
    static void set_navigator_compatibility_mode(NavigatorCompatibilityMode);

    bool enable_global_privacy_control() const { return m_enable_global_privacy_control; }
    void set_enable_global_privacy_control(bool enable) { m_enable_global_privacy_control = enable; }

private:
    explicit ResourceLoader(GC::Heap&, NonnullRefPtr<Requests::RequestClient>);

    struct FileLoadResult {
        ReadonlyBytes data;
        NonnullRefPtr<HTTP::HeaderList> response_headers;
        Requests::RequestTimingInfo timing_info {};
    };
    template<typename FileHandler, typename ErrorHandler>
    void handle_file_load_request(LoadRequest& request, FileHandler on_file, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_about_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_resource_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);

    RefPtr<Requests::Request> start_network_request(LoadRequest const&, Requests::RequestClient::TransferLease);
    void finish_network_request(NonnullRefPtr<Requests::Request>);

    GC::Heap& m_heap;
    RefPtr<Requests::RequestClient> m_request_client;
    HashTable<NonnullRefPtr<Requests::Request>> m_active_requests;

    String m_user_agent;
    SiteCompatibilityData m_site_compatibility_data;
    // Sites whose navigation was challenged while the user had UA spoofing on, and are being, or were, re-requested
    // with our own UA. We send our own UA to these sites in every request while a retry is pending, and from then on
    // once a retry has loaded.
    struct SiteForcedToLadybirdUserAgent {
        size_t pending_retries { 0 };
        bool loaded { false };
    };
    HashMap<String, SiteForcedToLadybirdUserAgent> m_sites_forced_to_ladybird_user_agent;
    String m_platform;
    Vector<String> m_preferred_languages = { "en"_string };
    static NavigatorCompatibilityMode s_navigator_compatibility_mode;
    bool m_enable_global_privacy_control { false };
};

}
