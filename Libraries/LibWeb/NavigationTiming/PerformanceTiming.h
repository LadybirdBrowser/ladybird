/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>

namespace Web::NavigationTiming {

// https://w3c.github.io/navigation-timing/#the-performancetiming-interface
class PerformanceTiming final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(PerformanceTiming, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(PerformanceTiming);

public:
    using AllowOwnPtr = TrueType;

    static GC::Ref<PerformanceTiming> create(HTML::Window&);

    ~PerformanceTiming();

    u64 navigation_start() const;
    u64 unload_event_start() const;
    u64 unload_event_end() const;
    u64 redirect_start() const;
    u64 redirect_end() const;
    u64 fetch_start() const;
    u64 domain_lookup_start() const;
    u64 domain_lookup_end() const;
    u64 connect_start() const;
    u64 connect_end() const;
    u64 secure_connection_start() const;
    u64 request_start() const;
    u64 response_start() const;
    u64 response_end() const;
    u64 dom_loading() const;
    u64 dom_interactive() const;
    u64 dom_content_loaded_event_start() const;
    u64 dom_content_loaded_event_end() const;
    u64 dom_complete() const;
    u64 load_event_start() const;
    u64 load_event_end() const;

private:
    explicit PerformanceTiming(HTML::Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    DOM::Document const& document() const { return m_window->associated_document(); }
    GC::Ptr<PerformanceNavigationTiming> navigation_timing_entry() const { return document().navigation_timing_entry(); }

    u64 monotonic_timestamp_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp) const;
    u64 fetch_phase_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp) const;
    u64 relative_timestamp_to_wall_time_milliseconds(HighResolutionTime::DOMHighResTimeStamp) const;

    GC::Ref<HTML::Window> m_window;
};

}
