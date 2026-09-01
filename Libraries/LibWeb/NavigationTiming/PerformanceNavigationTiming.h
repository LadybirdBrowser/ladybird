/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceNavigationTiming.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::NavigationTiming {

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
class PerformanceNavigationTiming final : public ResourceTiming::PerformanceResourceTiming {
    WEB_WRAPPABLE(PerformanceNavigationTiming, ResourceTiming::PerformanceResourceTiming);
    GC_DECLARE_ALLOCATOR(PerformanceNavigationTiming);

public:
    virtual ~PerformanceNavigationTiming() override;

    static void create_navigation_timing_entry(DOM::Document&, GC::Ref<Fetch::Infrastructure::FetchTimingInfo>, u16 redirect_count, Bindings::NavigationTimingType, Optional<Fetch::Infrastructure::Response::CacheState> const&, Fetch::Infrastructure::Response::BodyInfo, Fetch::Infrastructure::Status);

    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }
    static Optional<u64> max_buffer_size() { return OptionalNone {}; }
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual Utf16FlyString const& entry_type() const override;
    virtual HighResolutionTime::DOMHighResTimeStamp duration() const override;

    HighResolutionTime::DOMHighResTimeStamp unload_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp unload_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_interactive() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_complete() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_end() const;
    Bindings::NavigationTimingType type() const { return m_navigation_type; }
    u16 redirect_count() const { return m_redirect_count; }
    HighResolutionTime::DOMHighResTimeStamp critical_ch_restart() const { return 0; }

private:
    PerformanceNavigationTiming(DOM::Document&, Utf16String const& name, GC::Ref<Fetch::Infrastructure::FetchTimingInfo>, HighResolutionTime::DOMHighResTimeStamp time_origin, u16 redirect_count, Bindings::NavigationTimingType);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    u16 m_redirect_count { 0 };
    Bindings::NavigationTimingType m_navigation_type { Bindings::NavigationTimingType::Navigate };
};

}
