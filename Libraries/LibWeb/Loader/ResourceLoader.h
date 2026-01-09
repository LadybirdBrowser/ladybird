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
#include <LibGC/Function.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Forward.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/NavigatorCompatibilityMode.h>

namespace Web {

class WEB_API ResourceLoader : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(ResourceLoader)

public:
    static void initialize(GC::Heap&, NonnullRefPtr<Requests::RequestClient>);
    static ResourceLoader& the();

    void set_client(NonnullRefPtr<Requests::RequestClient>);

    using OnHeadersReceived = GC::Function<void(HTTP::HeaderList const& response_headers, Optional<u32> status_code, Optional<String> const& reason_phrase)>;
    using OnDataReceived = GC::Function<void(ReadonlyBytes data)>;
    using OnComplete = GC::Function<void(bool success, Requests::RequestTimingInfo const& timing_info, Optional<StringView> error_message)>;

    RefPtr<Requests::Request> load(LoadRequest&, GC::Root<OnHeadersReceived>, GC::Root<OnDataReceived>, GC::Root<OnComplete>);

    RefPtr<Requests::RequestClient>& request_client() { return m_request_client; }

    void prefetch_dns(URL::URL const&);
    void preconnect(URL::URL const&);

    Function<void()> on_load_counter_change;

    int pending_loads() const { return m_pending_loads; }

    String const& user_agent() const { return m_user_agent; }
    void set_user_agent(String user_agent) { m_user_agent = move(user_agent); }

    String const& platform() const { return m_platform; }
    void set_platform(String platform) { m_platform = move(platform); }

    Vector<String> const& preferred_languages() const { return m_preferred_languages; }
    void set_preferred_languages(Vector<String> preferred_languages)
    {
        m_preferred_languages = move(preferred_languages);
        VERIFY(!m_preferred_languages.is_empty());
    }

    NavigatorCompatibilityMode navigator_compatibility_mode() { return m_navigator_compatibility_mode; }
    void set_navigator_compatibility_mode(NavigatorCompatibilityMode mode) { m_navigator_compatibility_mode = mode; }

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
    template<typename Callback>
    void handle_about_load_request(LoadRequest const& request, Callback callback);
    template<typename ResourceHandler, typename ErrorHandler>
    void handle_resource_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error);

    RefPtr<Requests::Request> start_network_request(LoadRequest const&);
    void handle_network_response_headers(LoadRequest const&, HTTP::HeaderList const&);
    void finish_network_request(NonnullRefPtr<Requests::Request>);

    int m_pending_loads { 0 };

    GC::Heap& m_heap;
    RefPtr<Requests::RequestClient> m_request_client;
    HashTable<NonnullRefPtr<Requests::Request>> m_active_requests;

    String m_user_agent;
    String m_platform;
    Vector<String> m_preferred_languages = { "en"_string };
    NavigatorCompatibilityMode m_navigator_compatibility_mode;
    bool m_enable_global_privacy_control { false };
};

}
