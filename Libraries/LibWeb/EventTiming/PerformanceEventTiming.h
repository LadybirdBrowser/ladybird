/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::EventTiming {

// https://www.w3.org/TR/event-timing/#sec-performance-event-timing
class PerformanceEventTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_PLATFORM_OBJECT(PerformanceEventTiming, PerformanceTimeline::PerformanceEntry);
    JS_DECLARE_ALLOCATOR(PerformanceEventTiming);

public:
    virtual ~PerformanceEventTiming();

    HighResolutionTime::DOMHighResTimeStamp processing_start() const;
    HighResolutionTime::DOMHighResTimeStamp processing_end() const;
    bool cancelable() const;
    JS::ThrowCompletionOr<JS::GCPtr<DOM::Node>> target();
    unsigned long long interaction_id();

    // from the registry:
    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline();
    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size();
    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override;

    virtual FlyString const& entry_type() const override;

private:
    PerformanceEventTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration,
        DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, unsigned long long interaction_id);

    // m_entry_type defined here for both "event"s and "first-input"s
    // this is the only PerformanceEntry that has two event types it could represent
    // That complicates implementing the registry functions if they remain static
    FlyString m_entry_type;
    JS::GCPtr<DOM::EventTarget> m_event_target;
    HighResolutionTime::DOMHighResTimeStamp m_start_time;
    HighResolutionTime::DOMHighResTimeStamp m_processing_start;
    bool m_cancelable;
    unsigned long long m_interaction_id;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<PerformanceEventTiming>> construct_impl(DOM::Event const&, HighResolutionTime::DOMHighResTimeStamp, unsigned long long);
    virtual void initialize(JS::Realm&) override;

    PerformanceTimeline::ShouldAddEntry should_add_performance_event_timing() const;

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // FIXME: remaining algorithms described in this spec:
    // https://www.w3.org/TR/event-timing/#sec-increasing-interaction-count
    // https://www.w3.org/TR/event-timing/#sec-computing-interactionid
    // https://www.w3.org/TR/event-timing/#sec-fin-event-timing
    // https://www.w3.org/TR/event-timing/#sec-dispatch-pending
};
}
