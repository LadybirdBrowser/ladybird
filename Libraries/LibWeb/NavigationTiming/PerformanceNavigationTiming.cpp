/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigationTiming);

PerformanceNavigationTiming::PerformanceNavigationTiming(DOM::Document& document, Utf16String const& name, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, HighResolutionTime::DOMHighResTimeStamp time_origin, u16 redirect_count, Bindings::NavigationTimingType navigation_type)
    : PerformanceResourceTiming(name, 0, 0, timing_info, time_origin)
    , m_document(document)
    , m_redirect_count(redirect_count)
    , m_navigation_type(navigation_type)
{
}

PerformanceNavigationTiming::~PerformanceNavigationTiming() = default;

void PerformanceNavigationTiming::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://w3c.github.io/navigation-timing/#dfn-create-the-navigation-timing-entry
void PerformanceNavigationTiming::create_navigation_timing_entry(DOM::Document& document, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo body_info, Fetch::Infrastructure::Status response_status)
{
    // 1. Let global be document's relevant global object.
    auto& global = HTML::relevant_global_object(document);

    // 2. Let navigationTimingEntry be a new PerformanceNavigationTiming object in global's realm.
    auto time_origin = HTML::relevant_settings_object(global).time_origin();
    auto entry = GC::Heap::the().allocate<PerformanceNavigationTiming>(document, utf16_string_from_url_ascii(document.url().serialize()), timing_info, time_origin, redirect_count, navigation_type);

    // 3. Setup the resource timing entry for navigationTimingEntry given "navigation", document's URL, fetchTiming,
    //    cacheMode, and bodyInfo.
    entry->setup_the_resource_timing_entry(PerformanceTimeline::EntryTypes::navigation, utf16_string_from_url_ascii(document.url().serialize()), timing_info, cache_mode, move(body_info), response_status);

    // 4-8. The document load timing, previous document unload timing, redirect count, and navigation type are stored by
    //      the entry. Service worker timing is not yet implemented.

    // 9. Set document's navigation timing entry to navigationTimingEntry.
    document.set_navigation_timing_entry(entry);

    // 10-11. Critical-CH restart timing and not-restored reasons are not yet implemented.

    // 12. Add navigationTimingEntry to global's performance entry buffer.
    HTML::relevant_window_or_worker_global_scope(global).add_performance_entry(entry);
}

Utf16FlyString const& PerformanceNavigationTiming::entry_type() const
{
    return PerformanceTimeline::EntryTypes::navigation;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::duration() const
{
    return load_event_end();
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_start() const
{
    return m_document->previous_document_unload_timing().unload_event_start_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::unload_event_end() const
{
    return m_document->previous_document_unload_timing().unload_event_end_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_interactive() const
{
    return m_document->load_timing_info().dom_interactive_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_start() const
{
    return m_document->load_timing_info().dom_content_loaded_event_start_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_content_loaded_event_end() const
{
    return m_document->load_timing_info().dom_content_loaded_event_end_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::dom_complete() const
{
    return m_document->load_timing_info().dom_complete_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_start() const
{
    return m_document->load_timing_info().load_event_start_time;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceNavigationTiming::load_event_end() const
{
    return m_document->load_timing_info().load_event_end_time;
}

}
