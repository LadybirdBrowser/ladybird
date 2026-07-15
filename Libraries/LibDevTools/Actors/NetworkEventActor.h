/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibHTTP/Header.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace DevTools {

class DEVTOOLS_API NetworkEventActor final : public Actor {
public:
    static constexpr auto base_name = "netEvent"sv;

    static NonnullRefPtr<NetworkEventActor> create(DevToolsServer&, String name, u64 request_id);
    virtual ~NetworkEventActor() override;

    u64 request_id() const { return m_request_id; }
    u64 browsing_context_id() const { return m_browsing_context_id; }
    u64 inner_window_id() const { return m_inner_window_id; }

    void set_request_info(String url, String method, UnixDateTime start_time, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type);
    void set_browsing_context_ids(u64 browsing_context_id, u64 inner_window_id);
    void set_referrer_policy(String);
    void set_is_navigation_request(bool);
    void set_priority(Web::Fetch::Infrastructure::Request::Priority);
    void set_response_start(u32 status_code, Optional<String> reason_phrase);
    void set_response_headers(Vector<HTTP::Header> response_headers);
    void set_loaded_from_cache(bool);
    void append_response_body(ByteBuffer data);
    void set_request_complete(u64 body_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error);

    JsonObject serialize_initial_event() const;

private:
    NetworkEventActor(DevToolsServer&, String name, u64 request_id);

    virtual void handle_message(Message const&) override;

    void get_request_headers(Message const&);
    void get_request_cookies(Message const&);
    void get_request_post_data(Message const&);
    void get_response_headers(Message const&);
    void get_response_cookies(Message const&);
    void get_response_content(Message const&);
    void get_event_timings(Message const&);
    void get_security_info(Message const&);

    u64 m_request_id { 0 };
    String m_url;
    String m_method;
    UnixDateTime m_start_time;
    Vector<HTTP::Header> m_request_headers;
    ByteBuffer m_request_body;
    Optional<String> m_initiator_type;

    Optional<u32> m_status_code;
    Optional<String> m_reason_phrase;
    Vector<HTTP::Header> m_response_headers;
    bool m_loaded_from_cache { false };
    u64 m_browsing_context_id { 1 };
    u64 m_inner_window_id { 1 };
    String m_referrer_policy;
    bool m_is_navigation_request { false };
    Web::Fetch::Infrastructure::Request::Priority m_priority { Web::Fetch::Infrastructure::Request::Priority::Auto };

    ByteBuffer m_response_body;
    u64 m_body_size { 0 };
    Requests::RequestTimingInfo m_timing_info {};
    Optional<Requests::NetworkError> m_network_error;
    bool m_complete { false };

    static constexpr size_t MAX_RESPONSE_BODY_SIZE = 10 * MiB;
};

}
