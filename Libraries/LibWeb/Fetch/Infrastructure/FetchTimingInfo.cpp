/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchTimingInfo);

FetchTimingInfo::FetchTimingInfo() = default;

GC::Ref<FetchTimingInfo> FetchTimingInfo::create(JS::VM& vm)
{
    return vm.heap().allocate<FetchTimingInfo>();
}

void FetchTimingInfo::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://fetch.spec.whatwg.org/#create-an-opaque-timing-info
GC::Ref<FetchTimingInfo> create_opaque_timing_info(JS::VM& vm, FetchTimingInfo const& timing_info)
{
    // To create an opaque timing info, given a fetch timing info timingInfo, return a new fetch timing info whose
    // start time and post-redirect start time are timingInfo’s start time.
    auto new_timing_info = FetchTimingInfo::create(vm);
    new_timing_info->set_start_time(timing_info.start_time());
    new_timing_info->set_post_redirect_start_time(timing_info.start_time());
    return new_timing_info;
}

void FetchTimingInfo::update_final_timings(Requests::RequestTimingInfo const& final_timings, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability)
{
    bool has_cross_origin_isolated_capability = cross_origin_isolated_capability == HTML::CanUseCrossOriginIsolatedAPIs::Yes;

    auto domain_lookup_start_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.domain_lookup_start_microseconds) / 1000.0);
    auto coarsened_domain_lookup_start_time = HighResolutionTime::coarsen_time(domain_lookup_start_time_milliseconds, has_cross_origin_isolated_capability);

    auto domain_lookup_end_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.domain_lookup_end_microseconds) / 1000.0);
    auto coarsened_domain_lookup_end_time = HighResolutionTime::coarsen_time(domain_lookup_end_time_milliseconds, has_cross_origin_isolated_capability);

    auto connect_start_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.connect_start_microseconds) / 1000.0);
    auto coarsened_connection_start_time = HighResolutionTime::coarsen_time(connect_start_time_milliseconds, has_cross_origin_isolated_capability);

    auto connect_end_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.connect_end_microseconds) / 1000.0);
    auto coarsened_connection_end_time = HighResolutionTime::coarsen_time(connect_end_time_milliseconds, has_cross_origin_isolated_capability);

    auto secure_connect_start_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.secure_connect_start_microseconds) / 1000.0);
    auto coarsened_secure_connection_start_time = HighResolutionTime::coarsen_time(secure_connect_start_time_milliseconds, has_cross_origin_isolated_capability);

    m_final_connection_timing_info = ConnectionTimingInfo {
        .domain_lookup_start_time = coarsened_domain_lookup_start_time,
        .domain_lookup_end_time = coarsened_domain_lookup_end_time,
        .connection_start_time = coarsened_connection_start_time,
        .connection_end_time = coarsened_connection_end_time,
        .secure_connection_start_time = coarsened_secure_connection_start_time,
        .alpn_negotiated_protocol = alpn_http_version_to_fly_string(final_timings.http_version_alpn_identifier),
    };

    auto request_start_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.request_start_microseconds) / 1000.0);
    auto coarsened_request_start_time = HighResolutionTime::coarsen_time(request_start_time_milliseconds, has_cross_origin_isolated_capability);
    m_final_network_request_start_time = coarsened_request_start_time;

    auto response_start_time_milliseconds = m_start_time + (static_cast<HighResolutionTime::DOMHighResTimeStamp>(final_timings.response_start_microseconds) / 1000.0);
    auto coarsened_response_start_time = HighResolutionTime::coarsen_time(response_start_time_milliseconds, has_cross_origin_isolated_capability);
    m_final_network_response_start_time = coarsened_response_start_time;
}

}
