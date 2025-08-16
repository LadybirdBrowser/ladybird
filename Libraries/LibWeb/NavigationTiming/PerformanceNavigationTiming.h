/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceNavigationTimingPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::NavigationTiming {

class PerformanceNavigationTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_PLATFORM_OBJECT(PerformanceNavigationTiming, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceNavigationTiming);

public:
    virtual ~PerformanceNavigationTiming() override;

    static GC::Ref<PerformanceNavigationTiming> create(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, Bindings::NavigationTimingType type, u16 redirect_count);

    // Helper to create navigation timing entry for a document after load completes
    static void create_and_queue_navigation_timing_entry_for_document(DOM::Document& document);

    // NOTE: These three functions are answered by the registry for the given entry type.
    // https://w3c.github.io/timing-entrytypes-registry/#registry

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    // NOTE: For navigation timing, there should be only one entry (for the current navigation)
    static Optional<u64> max_buffer_size() { return 1; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual FlyString const& entry_type() const override;

    // Resource timing attributes (from PerformanceResourceTiming concept)
    String initiator_type() const;
    String delivery_type() const;
    String next_hop_protocol() const;
    HighResolutionTime::DOMHighResTimeStamp worker_start() const;
    HighResolutionTime::DOMHighResTimeStamp redirect_start() const;
    HighResolutionTime::DOMHighResTimeStamp redirect_end() const;
    HighResolutionTime::DOMHighResTimeStamp fetch_start() const;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_start() const;
    HighResolutionTime::DOMHighResTimeStamp domain_lookup_end() const;
    HighResolutionTime::DOMHighResTimeStamp connect_start() const;
    HighResolutionTime::DOMHighResTimeStamp connect_end() const;
    HighResolutionTime::DOMHighResTimeStamp secure_connection_start() const;
    HighResolutionTime::DOMHighResTimeStamp request_start() const;
    HighResolutionTime::DOMHighResTimeStamp response_start() const;
    HighResolutionTime::DOMHighResTimeStamp response_end() const;
    u64 transfer_size() const;
    u64 encoded_body_size() const;
    u64 decoded_body_size() const;

    // Navigation-specific timing attributes
    HighResolutionTime::DOMHighResTimeStamp unload_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp unload_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_interactive() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_end() const;
    HighResolutionTime::DOMHighResTimeStamp dom_complete() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_start() const;
    HighResolutionTime::DOMHighResTimeStamp load_event_end() const;
    Bindings::NavigationTimingType type() const { return m_type; }
    u16 redirect_count() const { return m_redirect_count; }
    HighResolutionTime::DOMHighResTimeStamp critical_ch_restart() const { return m_critical_ch_restart; }

private:
    PerformanceNavigationTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration, GC::Ref<Fetch::Infrastructure::FetchTimingInfo> timing_info, Bindings::NavigationTimingType type, u16 redirect_count);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // Helper methods for navigation timing attributes
    // TODO: Add implementations for the getter methods

    GC::Ref<Fetch::Infrastructure::FetchTimingInfo> m_timing_info;
    Bindings::NavigationTimingType m_type;
    u16 m_redirect_count;
    HighResolutionTime::DOMHighResTimeStamp m_critical_ch_restart { 0.0 };
};

}
