/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <LibCore/EventReceiver.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/Resource.h>
#include <LibWeb/Loader/UserAgent.h>

namespace Web {

class ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)
public:
    static void initialize(JS::Heap&, NonnullRefPtr<Requests::RequestClient>);
    static ResourceLoader& the();

    RefPtr<Resource> load_resource(Resource::Type, LoadRequest&);

    using SuccessCallback = JS::HeapFunction<void(ReadonlyBytes, HTTP::HeaderMap const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase)>;
    using ErrorCallback = JS::HeapFunction<void(ByteString const&, Optional<u32> status_code, Optional<String> const& reason_phrase, ReadonlyBytes payload, HTTP::HeaderMap const& response_headers)>;
    using TimeoutCallback = JS::HeapFunction<void()>;

    void load(LoadRequest&, JS::Handle<SuccessCallback> success_callback, JS::Handle<ErrorCallback> error_callback = nullptr, Optional<u32> timeout = {}, JS::Handle<TimeoutCallback> timeout_callback = nullptr);

    using OnHeadersReceived = JS::HeapFunction<void(HTTP::HeaderMap const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase)>;
    using OnDataReceived = JS::HeapFunction<void(ReadonlyBytes data)>;
    using OnComplete = JS::HeapFunction<void(bool success, Optional<StringView> error_message)>;

    void load_unbuffered(LoadRequest&, JS::Handle<OnHeadersReceived>, JS::Handle<OnDataReceived>, JS::Handle<OnComplete>);

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

    JS::Heap& heap() { return m_heap; }

private:
    explicit ResourceLoader(JS::Heap&, NonnullRefPtr<Requests::RequestClient>);

    RefPtr<Requests::Request> start_network_request(LoadRequest const&);
    void handle_network_response_headers(LoadRequest const&, HTTP::HeaderMap const&);
    void finish_network_request(NonnullRefPtr<Requests::Request>);

    int m_pending_loads { 0 };

    JS::Heap& m_heap;
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
