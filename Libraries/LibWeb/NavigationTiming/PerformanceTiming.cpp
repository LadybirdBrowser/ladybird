/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/NavigationTiming/PerformanceTiming.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceTiming);

GC::Ref<PerformanceTiming> PerformanceTiming::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<PerformanceTiming>(window);
}

PerformanceTiming::PerformanceTiming(HTML::Window& window)
    : m_window(window)
{
}

PerformanceTiming::~PerformanceTiming() = default;

void PerformanceTiming::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// All time values are measured in milliseconds since midnight of January 1, 1970 (UTC).
u64 PerformanceTiming::monotonic_timestamp_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp timestamp) const
{
    if (timestamp == 0)
        return 0;

    auto wall_time = timestamp - HighResolutionTime::estimated_monotonic_time_of_the_unix_epoch();
    auto coarsened_time = HighResolutionTime::coarsen_time(wall_time);
    return static_cast<u64>(coarsened_time);
}

// The document's time origin is its navigation start time, so a timestamp relative to it becomes monotonic by adding
// that back. A fetch phase coinciding with navigation start is 0 relative to the time origin without being unset.
u64 PerformanceTiming::fetch_phase_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp relative_timestamp) const
{
    return monotonic_timestamp_to_wall_time_milliseconds(relative_timestamp + document().load_timing_info().navigation_start_time);
}

u64 PerformanceTiming::relative_timestamp_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp relative_timestamp) const
{
    if (relative_timestamp == 0)
        return 0;

    return fetch_phase_to_wall_time_milliseconds(relative_timestamp);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-navigationstart
u64 PerformanceTiming::navigation_start() const
{
    // This attribute must return the time immediately after the user agent finishes prompting to unload the previous
    // document. If there is no previous document, this attribute must return the time the current document is created.
    return monotonic_timestamp_to_wall_time_milliseconds(document().load_timing_info().navigation_start_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-unloadeventstart
u64 PerformanceTiming::unload_event_start() const
{
    // If the previous document and the current document have the same origin, this attribute must return the time
    // immediately before the user agent starts the unload event of the previous document. If there is no previous
    // document or the previous document has a different origin than the current document, this attribute must return
    // zero.
    return relative_timestamp_to_wall_time_milliseconds(document().previous_document_unload_timing().unload_event_start_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-unloadeventend
u64 PerformanceTiming::unload_event_end() const
{
    // If the previous document and the current document have the same origin, this attribute must return the time
    // immediately after the user agent finishes the unload event of the previous document. If there is no previous
    // document or the previous document has a different origin than the current document or the unload is not yet
    // completed, this attribute must return zero.
    return relative_timestamp_to_wall_time_milliseconds(document().previous_document_unload_timing().unload_event_end_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-redirectstart
u64 PerformanceTiming::redirect_start() const
{
    // If there are HTTP redirects when navigating and if all the redirects are from the same origin, this attribute
    // must return the starting time of the fetch that initiates the redirect. Otherwise, this attribute must return
    // zero.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;

    if (entry->redirect_count() == 0)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->redirect_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-redirectend
u64 PerformanceTiming::redirect_end() const
{
    // If there are HTTP redirects when navigating and all redirects are from the same origin, this attribute must
    // return the time immediately after receiving the last byte of the response of the last redirect. Otherwise, this
    // attribute must return zero.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;

    if (entry->redirect_count() == 0)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->redirect_end());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-fetchstart
u64 PerformanceTiming::fetch_start() const
{
    // If the new resource is to be fetched using a "GET" request method, fetchStart must return the time immediately
    // before the user agent starts checking the HTTP cache. Otherwise, it must return the time when the user agent
    // starts fetching the resource.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;

    return fetch_phase_to_wall_time_milliseconds(entry->fetch_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domainlookupstart
u64 PerformanceTiming::domain_lookup_start() const
{
    // This attribute must return the time immediately before the user agent starts the domain name lookup for the
    // current document.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->domain_lookup_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domainlookupend
u64 PerformanceTiming::domain_lookup_end() const
{
    // This attribute must return the time immediately after the user agent finishes the domain name lookup for the
    // current document.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->domain_lookup_end());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-connectstart
u64 PerformanceTiming::connect_start() const
{
    // This attribute must return the time immediately before the user agent start establishing the connection to the
    // server to retrieve the document.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->connect_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-connectend
u64 PerformanceTiming::connect_end() const
{
    // This attribute must return the time immediately after the user agent finishes establishing the connection to
    // the server to retrieve the current document.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->connect_end());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-secureconnectionstart
u64 PerformanceTiming::secure_connection_start() const
{
    // When this attribute is available, if the scheme of the current page is "https", this attribute must return the
    // time immediately before the user agent starts the handshake process to secure the current connection. If this
    // attribute is available but HTTPS is not used, this attribute must return zero.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return relative_timestamp_to_wall_time_milliseconds(entry->secure_connection_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-requeststart
u64 PerformanceTiming::request_start() const
{
    // This attribute must return the time immediately before the user agent starts requesting the current document
    // from the server, or HTTP cache or from local resources.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->request_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-responsestart
u64 PerformanceTiming::response_start() const
{
    // This attribute must return the time immediately after the user agent receives the first byte of the response
    // from the server, or HTTP cache or from local resources.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return fetch_phase_to_wall_time_milliseconds(entry->response_start());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-responseend
u64 PerformanceTiming::response_end() const
{
    // This attribute must return the time immediately after the user agent receives the last byte of the current
    // document or immediately before the transport connection is closed, whichever comes first.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;
    return relative_timestamp_to_wall_time_milliseconds(entry->response_end());
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domloading
u64 PerformanceTiming::dom_loading() const
{
    // This attribute must return the time immediately before the user agent sets the current document readiness to
    // "loading".
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().dom_loading_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-dominteractive
u64 PerformanceTiming::dom_interactive() const
{
    // This attribute must return the time immediately before the user agent sets the current document readiness to
    // "interactive".
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().dom_interactive_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domcontentloadedeventstart
u64 PerformanceTiming::dom_content_loaded_event_start() const
{
    // This attribute must return the time immediately before the user agent fires the DOMContentLoaded event at the
    // Document.
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().dom_content_loaded_event_start_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domcontentloadedeventend
u64 PerformanceTiming::dom_content_loaded_event_end() const
{
    // This attribute must return the time immediately after the document's DOMContentLoaded event completes.
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().dom_content_loaded_event_end_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-domcomplete
u64 PerformanceTiming::dom_complete() const
{
    // This attribute must return the time immediately before the user agent sets the current document readiness to
    // "complete".
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().dom_complete_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-loadeventstart
u64 PerformanceTiming::load_event_start() const
{
    // This attribute must return the time immediately before the load event of the current document is fired. It must
    // return zero when the load event is not fired yet.
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().load_event_start_time);
}

// https://w3c.github.io/navigation-timing/#dom-performancetiming-loadeventend
u64 PerformanceTiming::load_event_end() const
{
    // This attribute must return the time when the load event of the current document is completed. It must return
    // zero when the load event is not fired or is not completed.
    return relative_timestamp_to_wall_time_milliseconds(document().load_timing_info().load_event_end_time);
}

}
