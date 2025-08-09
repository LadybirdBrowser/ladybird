/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PerformanceNavigationTimingPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigationTiming);

PerformanceNavigationTiming::PerformanceNavigationTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, Bindings::NavigationTimingType type, u16 redirect_count)
    : PerformanceTimeline::PerformanceEntry(realm, name, start_time, duration)
    , m_timing_info(timing_info)
    , m_type(type)
    , m_redirect_count(redirect_count)
{
}

PerformanceNavigationTiming::~PerformanceNavigationTiming() = default;

GC::Ref<PerformanceNavigationTiming> PerformanceNavigationTiming::create(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, Bindings::NavigationTimingType type, u16 redirect_count)
{
    return realm.create<PerformanceNavigationTiming>(realm, name, start_time, duration, timing_info, type, redirect_count);
}

// Helper to create navigation timing entry for a document after load completes
void PerformanceNavigationTiming::create_and_queue_navigation_timing_entry_for_document(DOM::Document& document)
{
    auto window = document.window();
    if (!window)
        return;

    auto& realm = window->realm();

    // Create minimal fetch timing info for navigation (many fields will be 0 for now)
    auto fetch_timing_info = Fetch::Infrastructure::FetchTimingInfo::create(realm.vm());

    // Set basic timing - for now, use navigation start time as both start and end
    auto navigation_start_time = document.load_timing_info().navigation_start_time;
    fetch_timing_info->set_start_time(navigation_start_time);
    fetch_timing_info->set_post_redirect_start_time(navigation_start_time);

    // Use current time as end time
    auto current_time = HighResolutionTime::current_high_resolution_time(*window);
    fetch_timing_info->set_end_time(current_time);

    // Create the navigation timing entry
    // Use document URL as name, start time is 0 (relative to time origin)
    // Duration is loadEventEnd - startTime (0) = loadEventEnd
    auto load_event_end_time = HighResolutionTime::relative_high_resolution_time(
        document.load_timing_info().load_event_end_time, *window);

    auto entry = create(
        realm,
        document.url().to_string(), // name
        0.0,                        // start time (relative to time origin)
        load_event_end_time,        // duration = loadEventEnd - 0
        fetch_timing_info,
        Bindings::NavigationTimingType::Navigate, // Default navigation type
        0                                         // redirect count (TODO: track actual redirects)
    );

    // Queue the entry to performance timeline
    window->queue_performance_entry(entry);

    // Add to performance entry buffer
    window->add_performance_entry(entry);
}

void PerformanceNavigationTiming::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceNavigationTiming);
    Base::initialize(realm);
}

void PerformanceNavigationTiming::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_timing_info);
}

// https://w3c.github.io/navigation-timing/#dfn-entrytype
FlyString const& PerformanceNavigationTiming::entry_type() const
{
    // The entryType getter steps are to return the DOMString "navigation".
    return PerformanceTimeline::EntryTypes::navigation;
}

// TODO: Implement proper navigation timing methods that access document timing info
// For now, these return placeholder values to get the basic implementation compiling

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_start() const
{
    // TODO: Access document unload timing info
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-unloadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_end() const
{
    // TODO: Access document unload timing info
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-dominteractive
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_interactive() const
{
    // TODO: Access document load timing info for dom_interactive_time
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_start() const
{
    // TODO: Access document load timing info for dom_content_loaded_event_start_time
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcontentloadedeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_end() const
{
    // TODO: Access document load timing info for dom_content_loaded_event_end_time
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-domcomplete
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_complete() const
{
    // TODO: Access document load timing info for dom_complete_time
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventstart
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_start() const
{
    // TODO: Access document load timing info for load_event_start_time
    return 0.0;
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming-loadeventend
HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_end() const
{
    // TODO: Access document load timing info for load_event_end_time
    return 0.0;
}

// Resource timing attribute implementations (placeholder implementations)
// TODO: These should access actual fetch timing info for navigation requests

String PerformanceNavigationTiming::initiator_type() const
{
    // For navigation timing, the initiator type is always "navigation"
    return "navigation"_string;
}

String PerformanceNavigationTiming::delivery_type() const
{
    // TODO: Implement actual delivery type detection
    return ""_string;
}

String PerformanceNavigationTiming::next_hop_protocol() const
{
    // TODO: Access fetch timing info for next hop protocol
    return ""_string;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::worker_start() const
{
    // TODO: Access fetch timing info for worker start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_start() const
{
    // TODO: Access fetch timing info for redirect start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::redirect_end() const
{
    // TODO: Access fetch timing info for redirect end time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::fetch_start() const
{
    // TODO: Access fetch timing info for fetch start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::domain_lookup_start() const
{
    // TODO: Access fetch timing info for domain lookup start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::domain_lookup_end() const
{
    // TODO: Access fetch timing info for domain lookup end time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::connect_start() const
{
    // TODO: Access fetch timing info for connect start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::connect_end() const
{
    // TODO: Access fetch timing info for connect end time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::secure_connection_start() const
{
    // TODO: Access fetch timing info for secure connection start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::request_start() const
{
    // TODO: Access fetch timing info for request start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::response_start() const
{
    // TODO: Access fetch timing info for response start time
    return 0.0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::response_end() const
{
    // TODO: Access fetch timing info for response end time
    return 0.0;
}

u64 PerformanceNavigationTiming::transfer_size() const
{
    // TODO: Access fetch timing info for transfer size
    return 0;
}

u64 PerformanceNavigationTiming::encoded_body_size() const
{
    // TODO: Access fetch timing info for encoded body size
    return 0;
}

u64 PerformanceNavigationTiming::decoded_body_size() const
{
    // TODO: Access fetch timing info for decoded body size
    return 0;
}

}
