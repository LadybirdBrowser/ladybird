/*
 * Copyright (c) 2025, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::ResourceTiming {

GC_DEFINE_ALLOCATOR(PerformanceResourceTiming);

PerformanceResourceTiming::PerformanceResourceTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info)
    : PerformanceTimeline::PerformanceEntry(realm, name, start_time, duration)
    , m_timing_info(timing_info)
{
}

PerformanceResourceTiming::~PerformanceResourceTiming() = default;

// https://w3c.github.io/resource-timing/#dfn-entrytype
FlyString const& PerformanceResourceTiming::entry_type() const
{
    // entryType
    //  The entryType getter steps are to return the DOMString "resource".
    return PerformanceTimeline::EntryTypes::resource;
}

void PerformanceResourceTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceResourceTiming);
}

void PerformanceResourceTiming::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_timing_info);
}

// https://w3c.github.io/resource-timing/#dfn-convert-fetch-timestamp
HighResolutionTime::DOMHighResTimeStamp convert_fetch_timestamp(HighResolutionTime::DOMHighResTimeStamp time_stamp, JS::Object const& global)
{
    // 1. If ts is zero, return zero.
    if (time_stamp == 0.0)
        return 0.0;

    // 2. Otherwise, return the relative high resolution coarse time given ts and global.
    return HighResolutionTime::relative_high_resolution_coarsen_time(time_stamp, global);
}

// https://w3c.github.io/resource-timing/#dfn-mark-resource-timing
void PerformanceResourceTiming::mark_resource_timing(GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, String const& requested_url, FlyString const& initiator_type, JS::Object& global, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status, FlyString delivery_type)
{
    // 1. Create a PerformanceResourceTiming object entry in global's realm.
    auto& window_or_worker = as<HTML::WindowOrWorkerGlobalScopeMixin>(global);
    auto& realm = window_or_worker.this_impl().realm();

    // https://w3c.github.io/resource-timing/#dfn-name
    // name
    //  The name getter steps are to return this's requested URL.

    // https://w3c.github.io/resource-timing/#dfn-starttime
    // startTime
    //  The startTime getter steps are to convert fetch timestamp for this's timing info's start time and this's relevant global object.

    // https://w3c.github.io/resource-timing/#dfn-duration
    // duration
    //  The duration getter steps are to return this's timing info's end time minus this's timing info's start time.
    auto converted_start_time = convert_fetch_timestamp(timing_info->start_time(), global);
    auto converted_end_time = convert_fetch_timestamp(timing_info->end_time(), global);
    auto entry = realm.create<PerformanceResourceTiming>(realm, requested_url, converted_start_time, converted_end_time - converted_start_time, timing_info);

    // Setup the resource timing entry for entry, given initiatorType, requestedURL, timingInfo, cacheMode, bodyInfo, responseStatus, and deliveryType.
    entry->setup_the_resource_timing_entry(initiator_type, requested_url, timing_info, cache_mode, move(body_info), response_status, delivery_type);

    // 3. Queue entry.
    window_or_worker.queue_performance_entry(entry);

    // 4. Add entry to global's performance entry buffer.
    window_or_worker.add_resource_timing_entry({}, entry);
}

// https://www.w3.org/TR/resource-timing/#dfn-setup-the-resource-timing-entry
void PerformanceResourceTiming::setup_the_resource_timing_entry(FlyString const& initiator_type, String const& requested_url, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status, FlyString delivery_type)
{
    // 2. Setup the resource timing entry for entry, given initiatorType, requestedURL, timingInfo, cacheMode, bodyInfo, responseStatus, and deliveryType.
    // https://w3c.github.io/resource-timing/#dfn-setup-the-resource-timing-entry

    // 1. Assert that cacheMode is the empty string, "local", or "validated".

    // 2. Set entry's initiator type to initiatorType.
    m_initiator_type = initiator_type;

    // 3. Set entry's requested URL to requestedURL.
    m_requested_url = requested_url;

    // 4. Set entry's timing info to timingInfo.
    m_timing_info = timing_info;

    // 5. Set entry's response body info to bodyInfo.
    m_response_body_info = move(body_info);

    // 6. Set entry's cache mode to cacheMode.
    m_cache_mode = cache_mode;

    // 7. Set entry's response status to responseStatus.
    m_response_status = response_status;

    // 8. If deliveryType is the empty string and cacheMode is not, then set deliveryType to "cache".
    if (delivery_type.is_empty() && cache_mode.has_value())
        delivery_type = "cache"_fly_string;

    // 9. Set entry's delivery type to deliveryType.
    m_delivery_type = delivery_type;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-nexthopprotocol
FlyString PerformanceResourceTiming::next_hop_protocol() const
{
    // The nextHopProtocol getter steps are to isomorphic decode this's timing info's final connection timing info's
    // ALPN negotiated protocol. See Recording connection timing info for more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return empty string in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return ""_fly_string;

    return m_timing_info->final_connection_timing_info()->alpn_negotiated_protocol;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-workerstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::worker_start() const
{
    // The workerStart getter steps are to convert fetch timestamp for this's timing info's final service worker start
    // time and the relevant global object for this. See HTTP fetch for more info.
    return convert_fetch_timestamp(m_timing_info->final_service_worker_start_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-redirectstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::redirect_start() const
{
    // The redirectStart getter steps are to convert fetch timestamp for this's timing info's redirect start time and
    // the relevant global object for this. See HTTP-redirect fetch for more info.
    return convert_fetch_timestamp(m_timing_info->redirect_start_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-redirectend
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::redirect_end() const
{
    // The redirectEnd getter steps are to convert fetch timestamp for this's timing info's redirect end time and the
    // relevant global object for this. See HTTP-redirect fetch for more info.
    return convert_fetch_timestamp(m_timing_info->redirect_end_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-fetchstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::fetch_start() const
{
    // The fetchStart getter steps are to convert fetch timestamp for this's timing info's post-redirect start time and
    // the relevant global object for this. See HTTP fetch for more info.
    return convert_fetch_timestamp(m_timing_info->post_redirect_start_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-domainlookupstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::domain_lookup_start() const
{
    // The domainLookupStart getter steps are to convert fetch timestamp for this's timing info's final connection
    // timing info's domain lookup start time and the relevant global object for this. See Recording connection timing
    // info for more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return 0.0;

    return convert_fetch_timestamp(m_timing_info->final_connection_timing_info()->domain_lookup_start_time, HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-domainlookupend
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::domain_lookup_end() const
{
    // The domainLookupEnd getter steps are to convert fetch timestamp for this's timing info's final connection timing
    // info's domain lookup end time and the relevant global object for this. See Recording connection timing info for
    // more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return 0.0;

    return convert_fetch_timestamp(m_timing_info->final_connection_timing_info()->domain_lookup_end_time, HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-connectstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::connect_start() const
{
    // The connectStart getter steps are to convert fetch timestamp for this's timing info's final connection timing
    // info's connection start time and the relevant global object for this. See Recording connection timing info for
    // more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return 0.0;

    return convert_fetch_timestamp(m_timing_info->final_connection_timing_info()->connection_start_time, HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-connectend
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::connect_end() const
{
    // The connectEnd getter steps are to convert fetch timestamp for this's timing info's final connection timing
    // info's connection end time and the relevant global object for this. See Recording connection timing info for
    // more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return 0.0;

    return convert_fetch_timestamp(m_timing_info->final_connection_timing_info()->connection_end_time, HTML::relevant_global_object(*this));
}

HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::secure_connection_start() const
{
    // The secureConnectionStart getter steps are to convert fetch timestamp for this's timing info's final connection
    // timing info's secure connection start time and the relevant global object for this. See Recording connection
    // timing info for more info.
    // NOTE: "final connection timing info" can be null, e.g. if this is the timing of a cross-origin resource and
    //       the Timing-Allow-Origin check fails. We return 0.0 in this case.
    if (!m_timing_info->final_connection_timing_info().has_value())
        return 0.0;

    return convert_fetch_timestamp(m_timing_info->final_connection_timing_info()->secure_connection_start_time, HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-requeststart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::request_start() const
{
    // The requestStart getter steps are to convert fetch timestamp for this's timing info's final network-request
    // start time and the relevant global object for this. See HTTP fetch for more info.
    return convert_fetch_timestamp(m_timing_info->final_network_request_start_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-finalresponseheadersstart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::final_response_headers_start() const
{
    // The finalResponseHeadersStart getter steps are to convert fetch timestamp for this's timing info's final
    // network-response start time and the relevant global object for this. See HTTP fetch for more info.
    return convert_fetch_timestamp(m_timing_info->final_network_response_start_time(), HTML::relevant_global_object(*this));
}

HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::first_interim_response_start() const
{
    // The firstInterimResponseStart getter steps are to convert fetch timestamp for this's timing info's first interim
    // network-response start time and the relevant global object for this. See HTTP fetch for more info.
    return convert_fetch_timestamp(m_timing_info->first_interim_network_response_start_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responsestart
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::response_start() const
{
    // The responseStart getter steps are to return this's firstInterimResponseStart if it is not 0;
    // Otherwise this's finalResponseHeadersStart.
    auto first_interim_response_start_time = first_interim_response_start();
    if (first_interim_response_start_time != 0.0)
        return first_interim_response_start_time;

    return final_response_headers_start();
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responseend
HighResolutionTime::DOMHighResTimeStamp PerformanceResourceTiming::response_end() const
{
    // The responseEnd getter steps are to convert fetch timestamp for this's timing info's end time and the relevant
    // global object for this. See fetch for more info.
    return convert_fetch_timestamp(m_timing_info->end_time(), HTML::relevant_global_object(*this));
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-encodedbodysize
u64 PerformanceResourceTiming::encoded_body_size() const
{
    // The encodedBodySize getter steps are to return this's resource info's encoded size.
    return m_response_body_info.encoded_size;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-decodedbodysize
u64 PerformanceResourceTiming::decoded_body_size() const
{
    // The decodedBodySize getter steps are to return this's resource info's decoded size.
    return m_response_body_info.decoded_size;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-transfersize
u64 PerformanceResourceTiming::transfer_size() const
{
    if (m_cache_mode.has_value()) {
        // 1. If this's cache mode is "local", then return 0.
        if (m_cache_mode.value() == Fetch::Infrastructure::Response::CacheState::Local)
            return 0;

        // 2. If this's cache mode is "validated", then return 300.
        if (m_cache_mode.value() == Fetch::Infrastructure::Response::CacheState::Validated)
            return 300;
    }

    // 3. Return this's response body info's encoded size plus 300.
    // Spec Note: The constant number added to transferSize replaces exposing the total byte size of the HTTP headers,
    //            as that may expose the presence of certain cookies. See this issue: https://github.com/w3c/resource-timing/issues/238
    return m_response_body_info.encoded_size + 300;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-responsestatus
Fetch::Infrastructure::Status PerformanceResourceTiming::response_status() const
{
    // The responseStatus getter steps are to return this's response status.
    // Spec Note: responseStatus is determined in Fetch. For a cross-origin no-cors request it would be 0 because the
    // response would be an opaque filtered response.
    return m_response_status;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-renderblockingstatus
Bindings::RenderBlockingStatusType PerformanceResourceTiming::render_blocking_status() const
{
    // The renderBlockingStatus getter steps are to return blocking if this's timing info's render-blocking is true;
    // otherwise non-blocking.
    if (m_timing_info->render_blocking())
        return Bindings::RenderBlockingStatusType::Blocking;

    return Bindings::RenderBlockingStatusType::NonBlocking;
}

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming-contenttype
String const& PerformanceResourceTiming::content_type() const
{
    // The contentType getter steps are to return this's resource info's content type.
    return m_response_body_info.content_type;
}

}
