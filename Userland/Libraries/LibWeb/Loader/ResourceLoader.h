/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Proxy.h>
#include <LibJS/SafeFunction.h>
#include <LibProtocol/Request.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/Resource.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWeb/Page/Page.h>

namespace Web {

namespace WebSockets {
class WebSocketClientSocket;
}

class ResourceLoaderConnectorRequest : public RefCounted<ResourceLoaderConnectorRequest> {
public:
    virtual ~ResourceLoaderConnectorRequest();

    struct CertificateAndKey {
        ByteString certificate;
        ByteString key;
    };

    // Configure the request such that the entirety of the response data is buffered. The callback receives that data and
    // the response headers all at once. Using this method is mutually exclusive with `set_unbuffered_data_received_callback`.
    virtual void set_buffered_request_finished_callback(Protocol::Request::BufferedRequestFinished) = 0;

    // Configure the request such that the response data is provided unbuffered as it is received. Using this method is
    // mutually exclusive with `set_buffered_request_finished_callback`.
    virtual void set_unbuffered_request_callbacks(Protocol::Request::HeadersReceived, Protocol::Request::DataReceived, Protocol::Request::RequestFinished) = 0;

    virtual bool stop() = 0;

    Function<void(Optional<u64> total_size, u64 downloaded_size)> on_progress;
    Function<CertificateAndKey()> on_certificate_requested;

protected:
    explicit ResourceLoaderConnectorRequest();
};

class ResourceLoaderConnector : public RefCounted<ResourceLoaderConnector> {
public:
    virtual ~ResourceLoaderConnector();

    virtual void prefetch_dns(URL::URL const&) = 0;
    virtual void preconnect(URL::URL const&) = 0;

    virtual RefPtr<ResourceLoaderConnectorRequest> start_request(ByteString const& method, URL::URL const&, HTTP::HeaderMap const& request_headers = {}, ReadonlyBytes request_body = {}, Core::ProxyData const& = {}) = 0;
    virtual RefPtr<Web::WebSockets::WebSocketClientSocket> websocket_connect(const URL::URL&, ByteString const& origin, Vector<ByteString> const& protocols) = 0;

protected:
    explicit ResourceLoaderConnector();
};

class ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)
public:
    static void initialize(RefPtr<ResourceLoaderConnector>);
    static ResourceLoader& the();

    RefPtr<Resource> load_resource(Resource::Type, LoadRequest&);

    using SuccessCallback = JS::SafeFunction<void(ReadonlyBytes, HTTP::HeaderMap const& response_headers, Optional<u32> status_code)>;
    using ErrorCallback = JS::SafeFunction<void(ByteString const&, Optional<u32> status_code, ReadonlyBytes payload, HTTP::HeaderMap const& response_headers)>;
    using TimeoutCallback = JS::SafeFunction<void()>;

    void load(LoadRequest&, SuccessCallback success_callback, ErrorCallback error_callback = nullptr, Optional<u32> timeout = {}, TimeoutCallback timeout_callback = nullptr);

    using OnHeadersReceived = JS::SafeFunction<void(HTTP::HeaderMap const& response_headers, Optional<u32> status_code)>;
    using OnDataReceived = JS::SafeFunction<void(ReadonlyBytes data)>;
    using OnComplete = JS::SafeFunction<void(bool success, Optional<StringView> error_message)>;

    void load_unbuffered(LoadRequest&, OnHeadersReceived, OnDataReceived, OnComplete);

    ResourceLoaderConnector& connector() { return *m_connector; }

    void prefetch_dns(URL::URL const&);
    void preconnect(URL::URL const&);

    Function<void()> on_load_counter_change;

    int pending_loads() const { return m_pending_loads; }

    String const& user_agent() const { return m_user_agent; }
    void set_user_agent(String user_agent) { m_user_agent = move(user_agent); }

    String const& platform() const { return m_platform; }
    void set_platform(String platform) { m_platform = move(platform); }

    Vector<String> preferred_languages() const { return m_preferred_languages; }
    void set_preferred_languages(Vector<String> preferred_languages)
    {
        // Default to "en" if no preferred languages are specified.
        if (preferred_languages.is_empty() || (preferred_languages.size() == 1 && preferred_languages[0].is_empty())) {
            m_preferred_languages = { "en"_string };
        } else {
            m_preferred_languages = move(preferred_languages);
        }
    }

    NavigatorCompatibilityMode navigator_compatibility_mode() { return m_navigator_compatibility_mode; }
    void set_navigator_compatibility_mode(NavigatorCompatibilityMode mode) { m_navigator_compatibility_mode = mode; }

    bool enable_do_not_track() const { return m_enable_do_not_track; }
    void set_enable_do_not_track(bool enable) { m_enable_do_not_track = enable; }

    void clear_cache();
    void evict_from_cache(LoadRequest const&);

private:
    ResourceLoader(NonnullRefPtr<ResourceLoaderConnector>);
    static ErrorOr<NonnullRefPtr<ResourceLoader>> try_create(NonnullRefPtr<ResourceLoaderConnector>);

    RefPtr<ResourceLoaderConnectorRequest> start_network_request(LoadRequest const&);
    void handle_network_response_headers(LoadRequest const&, HTTP::HeaderMap const&);
    void finish_network_request(NonnullRefPtr<ResourceLoaderConnectorRequest> const&);

    int m_pending_loads { 0 };

    HashTable<NonnullRefPtr<ResourceLoaderConnectorRequest>> m_active_requests;
    NonnullRefPtr<ResourceLoaderConnector> m_connector;
    String m_user_agent;
    String m_platform;
    Vector<String> m_preferred_languages = { "en"_string };
    NavigatorCompatibilityMode m_navigator_compatibility_mode;
    bool m_enable_do_not_track { false };
    Optional<JS::GCPtr<Page>> m_page {};
};

}
