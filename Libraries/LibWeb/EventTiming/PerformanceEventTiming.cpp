/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceEventTimingPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::EventTiming {

JS_DEFINE_ALLOCATOR(PerformanceEventTiming);

// https://www.w3.org/TR/event-timing/#sec-init-event-timing
PerformanceEventTiming::PerformanceEventTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration,
    DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, unsigned long long interaction_id)
    : PerformanceTimeline::PerformanceEntry(realm, name, start_time, duration)
    , m_entry_type(PerformanceTimeline::EntryTypes::event)
    , m_start_time(event.time_stamp())
    , m_processing_start(processing_start)
    , m_cancelable(event.cancelable())
    , m_interaction_id(interaction_id)

{
}

PerformanceEventTiming::~PerformanceEventTiming() = default;

FlyString const& PerformanceEventTiming::entry_type() const
{
    return m_entry_type;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEventTiming::processing_end() const
{
    dbgln("FIXME: Implement PeformanceEventTiming processing_end()");
    return 0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEventTiming::processing_start() const
{
    dbgln("FIXME: Implement PeformanceEventTiming processing_start()");
    return 0;
}

bool PerformanceEventTiming::cancelable() const
{
    return m_cancelable;
}

JS::ThrowCompletionOr<JS::GCPtr<DOM::Node>> PerformanceEventTiming::target()
{
    dbgln("FIXME: Implement PerformanceEventTiming::PeformanceEventTiming target()");
    return nullptr;
}

unsigned long long PerformanceEventTiming::interaction_id()
{
    dbgln("FIXME: Implement PeformanceEventTiming interaction_id()");
    return 0;
}

// https://www.w3.org/TR/event-timing/#sec-should-add-performanceeventtiming
PerformanceTimeline::ShouldAddEntry PerformanceEventTiming::should_add_performance_event_timing() const
{
    dbgln("FIXME: Implement PeformanceEventTiming should_add_performance_event_timing()");
    // 1. If entry’s entryType attribute value equals to "first-input", return true.
    if (entry_type() == "first-input")
        return PerformanceTimeline::ShouldAddEntry::Yes;

    // 2. Assert that entry’s entryType attribute value equals "event".
    VERIFY(entry_type() == "event");

    // FIXME: 3. Let minDuration be computed as follows:
    // FIXME: 3.1. If options is not present or if options’s durationThreshold is not present, let minDuration be 104.
    // FIXME: 3.2. Otherwise, let minDuration be the maximum between 16 and options’s durationThreshold value.

    // FIXME: 4. If entry’s duration attribute value is greater than or equal to minDuration, return true.

    // 5. Otherwise, return false.
    return PerformanceTimeline::ShouldAddEntry::No;
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
// FIXME: the output here depends on the type of the object instance, but this function is static
//        the commented out if statement won't compile
PerformanceTimeline::AvailableFromTimeline PerformanceEventTiming::available_from_timeline()
{
    dbgln("FIXME: Implement PeformanceEventTiming available_from_timeline()");
    // if (entry_type() == "first-input")
    return PerformanceTimeline::AvailableFromTimeline::Yes;
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
// FIXME: Same issue as available_from_timeline() above
Optional<u64> PerformanceEventTiming::max_buffer_size()
{
    dbgln("FIXME: Implement PeformanceEventTiming max_buffer_size()");
    if (true) //(entry_type() == "first-input")
        return 1;
    // else return 150;
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
PerformanceTimeline::ShouldAddEntry PerformanceEventTiming::should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&>) const
{
    return should_add_performance_event_timing();
}

void PerformanceEventTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceEventTiming);
}

void PerformanceEventTiming::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_target);
}

}
