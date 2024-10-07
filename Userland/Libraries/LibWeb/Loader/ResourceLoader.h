/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <LibCore/EventReceiver.h>
#include <LibJS/SafeFunction.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/Resource.h>
#include <LibWeb/Loader/UserAgent.h>

namespace Web {

class ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)
public:
    static void initialize(NonnullRefPtr<Requests::RequestClient>);
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

    Requests::RequestClient& request_client() { return *m_request_client; }

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
    explicit ResourceLoader(NonnullRefPtr<Requests::RequestClient>);

    RefPtr<Requests::Request> start_network_request(LoadRequest const&);
    void handle_network_response_headers(LoadRequest const&, HTTP::HeaderMap const&);
    void finish_network_request(NonnullRefPtr<Requests::Request> const&);

    int m_pending_loads { 0 };

    NonnullRefPtr<Requests::RequestClient> m_request_client;
    HashTable<NonnullRefPtr<Requests::Request>> m_active_requests;

    String m_user_agent;
    String m_platform;
    Vector<String> m_preferred_languages = { "en"_string };
    NavigatorCompatibilityMode m_navigator_compatibility_mode;
    bool m_enable_do_not_track { false };
    Optional<JS::GCPtr<Page>> m_page {};
};

}
