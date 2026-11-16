/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Fetch::Infrastructure {

FetchTimingInfo::FetchTimingInfo() = default;

NonnullRefPtr<FetchTimingInfo> FetchTimingInfo::create()
{
    return adopt_ref(*new FetchTimingInfo);
}

NonnullRefPtr<FetchTimingInfo> FetchTimingInfo::create(Engine::FetchTimingSeed const& seed)
{
    auto timing_info = create();
    timing_info->m_start_time = seed.start_time;
    timing_info->m_redirect_start_time = seed.redirect_start_time;
    timing_info->m_redirect_end_time = seed.redirect_end_time;
    timing_info->m_post_redirect_start_time = seed.post_redirect_start_time;
    timing_info->m_final_service_worker_start_time = seed.final_service_worker_start_time;
    timing_info->m_server_timing_headers = seed.server_timing_headers;
    timing_info->m_render_blocking = seed.render_blocking;
    return timing_info;
}

Engine::FetchTimingSeed FetchTimingInfo::seed() const
{
    return {
        .start_time = m_start_time,
        .redirect_start_time = m_redirect_start_time,
        .redirect_end_time = m_redirect_end_time,
        .post_redirect_start_time = m_post_redirect_start_time,
        .final_service_worker_start_time = m_final_service_worker_start_time,
        .server_timing_headers = m_server_timing_headers,
        .render_blocking = m_render_blocking,
    };
}

// https://fetch.spec.whatwg.org/#create-an-opaque-timing-info
NonnullRefPtr<FetchTimingInfo> create_opaque_timing_info(FetchTimingInfo const& timing_info)
{
    // To create an opaque timing info, given a fetch timing info timingInfo, return a new fetch timing info whose start
    // time and post-redirect start time are timingInfo’s start time, end time is timingInfo’s end time, and
    // render-blocking is timingInfo’s render-blocking.
    auto new_timing_info = FetchTimingInfo::create();
    new_timing_info->set_start_time(timing_info.start_time());
    new_timing_info->set_post_redirect_start_time(timing_info.start_time());
    new_timing_info->set_end_time(timing_info.end_time());
    new_timing_info->set_render_blocking(timing_info.render_blocking());

    return new_timing_info;
}

Engine::FetchNetworkPhases FetchTimingInfo::final_network_phases(Requests::RequestTimingInfo const& final_timings, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability) const
{
    // RequestServer measures each hop from the start of its own request, so the final hop's offsets are relative to
    // the post-redirect start time.
    auto final_request_start_time = m_post_redirect_start_time;

    auto domain_lookup_start_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.domain_lookup_start_microseconds) / 1000.0);
    auto coarsened_domain_lookup_start_time = HighResolutionTime::coarsen_time(domain_lookup_start_time_milliseconds, cross_origin_isolated_capability);

    auto domain_lookup_end_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.domain_lookup_end_microseconds) / 1000.0);
    auto coarsened_domain_lookup_end_time = HighResolutionTime::coarsen_time(domain_lookup_end_time_milliseconds, cross_origin_isolated_capability);

    auto connect_start_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.connect_start_microseconds) / 1000.0);
    auto coarsened_connection_start_time = HighResolutionTime::coarsen_time(connect_start_time_milliseconds, cross_origin_isolated_capability);

    auto connect_end_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.connect_end_microseconds) / 1000.0);
    auto coarsened_connection_end_time = HighResolutionTime::coarsen_time(connect_end_time_milliseconds, cross_origin_isolated_capability);

    // A secure connection start of 0 means no TLS handshake took place, which stays 0 rather than becoming the
    // request start.
    HighResolutionTime::DOMHighResTimeStamp coarsened_secure_connection_start_time = 0;
    if (final_timings.secure_connect_start_microseconds != 0) {
        auto secure_connect_start_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.secure_connect_start_microseconds) / 1000.0);
        coarsened_secure_connection_start_time = HighResolutionTime::coarsen_time(secure_connect_start_time_milliseconds, cross_origin_isolated_capability);
    }

    auto request_start_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.request_start_microseconds) / 1000.0);
    auto coarsened_request_start_time = HighResolutionTime::coarsen_time(request_start_time_milliseconds, cross_origin_isolated_capability);

    auto response_start_time_milliseconds = final_request_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.response_start_microseconds) / 1000.0);
    auto coarsened_response_start_time = HighResolutionTime::coarsen_time(response_start_time_milliseconds, cross_origin_isolated_capability);

    return {
        .final_connection_timing_info = {
            .domain_lookup_start_time = coarsened_domain_lookup_start_time,
            .domain_lookup_end_time = coarsened_domain_lookup_end_time,
            .connection_start_time = coarsened_connection_start_time,
            .connection_end_time = coarsened_connection_end_time,
            .secure_connection_start_time = coarsened_secure_connection_start_time,
            .alpn_negotiated_protocol = alpn_http_version_to_byte_string(final_timings.http_version_alpn_identifier),
        },
        .final_network_request_start_time = coarsened_request_start_time,
        .final_network_response_start_time = coarsened_response_start_time,
    };
}

void FetchTimingInfo::set_network_phases(Engine::FetchNetworkPhases network_phases)
{
    m_final_connection_timing_info = move(network_phases.final_connection_timing_info);
    m_final_network_request_start_time = network_phases.final_network_request_start_time;
    m_final_network_response_start_time = network_phases.final_network_response_start_time;
}

void FetchTimingInfo::update_final_timings(Requests::RequestTimingInfo const& final_timings, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability)
{
    set_network_phases(final_network_phases(final_timings, cross_origin_isolated_capability));
}

// The values of a FetchTimingInfo, as Bindings/Rust/src/fetch/infrastructure/fetch_timing_info.rs reads them.
struct FetchTimingInfoABI {
    HighResolutionTime::DOMHighResTimeStamp start_time;
    HighResolutionTime::DOMHighResTimeStamp redirect_start_time;
    HighResolutionTime::DOMHighResTimeStamp redirect_end_time;
    HighResolutionTime::DOMHighResTimeStamp post_redirect_start_time;
    HighResolutionTime::DOMHighResTimeStamp final_service_worker_start_time;
    HighResolutionTime::DOMHighResTimeStamp final_network_request_start_time;
    HighResolutionTime::DOMHighResTimeStamp first_interim_network_response_start_time;
    HighResolutionTime::DOMHighResTimeStamp final_network_response_start_time;
    HighResolutionTime::DOMHighResTimeStamp end_time;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_start_time;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_end_time;
    HighResolutionTime::DOMHighResTimeStamp connection_start_time;
    HighResolutionTime::DOMHighResTimeStamp connection_end_time;
    HighResolutionTime::DOMHighResTimeStamp secure_connection_start_time;
    bool has_final_connection_timing_info;
    bool render_blocking;
};

extern "C" {
void web_fetch_timing_info_read(FetchTimingInfo const*, FetchTimingInfoABI*);
void web_fetch_timing_info_alpn_negotiated_protocol(FetchTimingInfo const*, void* context, void (*callback)(void*, ReadonlyBytes));
void web_fetch_timing_info_for_each_server_timing_header(FetchTimingInfo const*, void* context, void (*callback)(void*, ReadonlyBytes));
void web_fetch_timing_info_unref(FetchTimingInfo*);
}

extern "C" {

void web_fetch_timing_info_read(FetchTimingInfo const* timing_info, FetchTimingInfoABI* values)
{
    auto const& connection_timing_info = timing_info->final_connection_timing_info();
    *values = {
        .start_time = timing_info->start_time(),
        .redirect_start_time = timing_info->redirect_start_time(),
        .redirect_end_time = timing_info->redirect_end_time(),
        .post_redirect_start_time = timing_info->post_redirect_start_time(),
        .final_service_worker_start_time = timing_info->final_service_worker_start_time(),
        .final_network_request_start_time = timing_info->final_network_request_start_time(),
        .first_interim_network_response_start_time = timing_info->first_interim_network_response_start_time(),
        .final_network_response_start_time = timing_info->final_network_response_start_time(),
        .end_time = timing_info->end_time(),
        .domain_lookup_start_time = connection_timing_info.has_value() ? connection_timing_info->domain_lookup_start_time : 0,
        .domain_lookup_end_time = connection_timing_info.has_value() ? connection_timing_info->domain_lookup_end_time : 0,
        .connection_start_time = connection_timing_info.has_value() ? connection_timing_info->connection_start_time : 0,
        .connection_end_time = connection_timing_info.has_value() ? connection_timing_info->connection_end_time : 0,
        .secure_connection_start_time = connection_timing_info.has_value() ? connection_timing_info->secure_connection_start_time : 0,
        .has_final_connection_timing_info = connection_timing_info.has_value(),
        .render_blocking = timing_info->render_blocking(),
    };
}

void web_fetch_timing_info_alpn_negotiated_protocol(FetchTimingInfo const* timing_info, void* context, void (*callback)(void*, ReadonlyBytes))
{
    auto const& connection_timing_info = timing_info->final_connection_timing_info();
    if (connection_timing_info.has_value())
        callback(context, connection_timing_info->alpn_negotiated_protocol.bytes());
}

void web_fetch_timing_info_for_each_server_timing_header(FetchTimingInfo const* timing_info, void* context, void (*callback)(void*, ReadonlyBytes))
{
    for (auto const& header : timing_info->server_timing_headers())
        callback(context, header.bytes());
}

void web_fetch_timing_info_unref(FetchTimingInfo* timing_info)
{
    timing_info->unref();
}
}

}
