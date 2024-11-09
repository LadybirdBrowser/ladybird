/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Fetch/Infrastructure/ConnectionTimingInfo.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#fetch-timing-info
class FetchTimingInfo : public JS::Cell {
    JS_CELL(FetchTimingInfo, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchTimingInfo);

public:
    [[nodiscard]] static JS::NonnullGCPtr<FetchTimingInfo> create(JS::VM&);

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp start_time() const { return m_start_time; }
    void set_start_time(HighResolutionTime::DOMHighResTimeStamp start_time) { m_start_time = start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp redirect_start_time() const { return m_redirect_start_time; }
    void set_redirect_start_time(HighResolutionTime::DOMHighResTimeStamp redirect_start_time) { m_redirect_start_time = redirect_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp redirect_end_time() const { return m_redirect_end_time; }
    void set_redirect_end_time(HighResolutionTime::DOMHighResTimeStamp redirect_end_time) { m_redirect_end_time = redirect_end_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp post_redirect_start_time() const { return m_post_redirect_start_time; }
    void set_post_redirect_start_time(HighResolutionTime::DOMHighResTimeStamp post_redirect_start_time) { m_post_redirect_start_time = post_redirect_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp final_service_worker_start_time() const { return m_final_service_worker_start_time; }
    void set_final_service_worker_start_time(HighResolutionTime::DOMHighResTimeStamp final_service_worker_start_time) { m_final_service_worker_start_time = final_service_worker_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp final_network_request_start_time() const { return m_final_network_request_start_time; }
    void set_final_network_request_start_time(HighResolutionTime::DOMHighResTimeStamp final_network_request_start_time) { m_final_network_request_start_time = final_network_request_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp final_network_response_start_time() const { return m_final_network_response_start_time; }
    void set_final_network_response_start_time(HighResolutionTime::DOMHighResTimeStamp final_network_response_start_time) { m_final_network_response_start_time = final_network_response_start_time; }

    [[nodiscard]] HighResolutionTime::DOMHighResTimeStamp end_time() const { return m_end_time; }
    void set_end_time(HighResolutionTime::DOMHighResTimeStamp end_time) { m_end_time = end_time; }

    [[nodiscard]] JS::GCPtr<ConnectionTimingInfo> final_connection_timing_info() const { return m_final_connection_timing_info; }
    void set_final_connection_timing_info(JS::GCPtr<ConnectionTimingInfo> final_connection_timing_info) { m_final_connection_timing_info = final_connection_timing_info; }

    [[nodiscard]] Vector<String>& server_timing_headers() { return m_server_timing_headers; }
    [[nodiscard]] Vector<String> const& server_timing_headers() const { return m_server_timing_headers; }
    void set_server_timing_headers(Vector<String> server_timing_headers) { m_server_timing_headers = move(server_timing_headers); }

    [[nodiscard]] bool render_blocking() const { return m_render_blocking; }
    void set_render_blocking(bool render_blocking) { m_render_blocking = render_blocking; }

private:
    FetchTimingInfo();

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#fetch-timing-info-start-time
    // start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-redirect-start-time
    // redirect start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_redirect_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-redirect-end-time
    // redirect end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_redirect_end_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-post-redirect-start-time
    // post-redirect start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_post_redirect_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-final-service-worker-start-time
    // final service worker start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_final_service_worker_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-final-network-request-start-time
    // final network-request start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_final_network_request_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-final-network-response-start-time
    // final network-response start time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_final_network_response_start_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-end-time
    // end time (default 0)
    //     A DOMHighResTimeStamp.
    HighResolutionTime::DOMHighResTimeStamp m_end_time { 0 };

    // https://fetch.spec.whatwg.org/#fetch-timing-info-final-connection-timing-info
    // final connection timing info (default null)
    //     Null or a connection timing info.
    JS::GCPtr<ConnectionTimingInfo> m_final_connection_timing_info;

    // https://fetch.spec.whatwg.org/#fetch-timing-info-server-timing-headers
    // server-timing headers (default « »)
    //     A list of strings.
    Vector<String> m_server_timing_headers;

    // https://fetch.spec.whatwg.org/#fetch-timing-info-render-blocking
    // render-blocking (default false)
    //     A boolean.
    bool m_render_blocking { false };
};

JS::NonnullGCPtr<FetchTimingInfo> create_opaque_timing_info(JS::VM&, FetchTimingInfo const& timing_info);

}
