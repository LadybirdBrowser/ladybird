/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>
#include <LibWeb/PaintTiming/PaintTiming.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::EventTiming {

GC_DEFINE_ALLOCATOR(PerformanceEventTiming);

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ref<PerformanceEventTiming> PerformanceEventTiming::create(DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id)
{
    // 2. Let timingEntry be a new PerformanceEventTiming object with event's relevant realm.
    // 3. Set timingEntry's name to event's type attribute value.
    // 4. Set timingEntry's entryType to "event".
    // 5. Set timingEntry's startTime to event's timeStamp attribute value.
    // 6. Set timingEntry's processingStart to processingStart.
    // 7. Set timingEntry's cancelable to event's cancelable attribute value.
    // 8. Set timingEntry's interactionId to interactionId.
    return GC::Heap::the().allocate<PerformanceEventTiming>(Utf16String::from_utf16(event.type().view()), PerformanceTimeline::EntryTypes::event, event.time_stamp(), processing_start, event.cancelable(), interaction_id);
}

PerformanceEventTiming::PerformanceEventTiming(Utf16String const& name, Utf16FlyString const& entry_type, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp processing_start, bool cancelable, WebIDL::UnsignedLongLong interaction_id)
    : PerformanceTimeline::PerformanceEntry(name, start_time, 0)
    , m_entry_type(entry_type)
    , m_processing_start(processing_start)
    , m_cancelable(cancelable)
    , m_interaction_id(interaction_id)
{
}

PerformanceEventTiming::~PerformanceEventTiming() = default;

GC::Ref<PerformanceEventTiming> PerformanceEventTiming::copy_with_entry_type(Utf16FlyString const& entry_type) const
{
    auto copy = GC::Heap::the().allocate<PerformanceEventTiming>(name(), entry_type, start_time(), m_processing_start, m_cancelable, m_interaction_id);
    copy->m_duration = m_duration;
    copy->m_has_duration = m_has_duration;
    copy->m_processing_end = m_processing_end;
    copy->m_event_target = m_event_target;
    copy->m_target_selector = m_target_selector;
    return copy;
}

// https://w3c.github.io/event-timing/#dom-performanceeventtiming-target
GC::Ptr<DOM::Node> PerformanceEventTiming::target() const
{
    // 1. If this's eventTarget is not exposed for paint timing given null, return null.
    // NB: Only elements can be exposed for paint timing.
    auto const* element = as_if<DOM::Element>(m_event_target.ptr());
    if (!element || !PaintTiming::is_exposed_for_paint_timing(*element, nullptr))
        return nullptr;

    // 2. Return this's eventTarget.
    return m_event_target;
}

// https://w3c.github.io/event-timing/#should-add-performanceeventtiming
PerformanceTimeline::ShouldAddEntry PerformanceEventTiming::should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> options) const
{
    // 1. If entry's entryType attribute value equals to "first-input", return true.
    if (entry_type() == PerformanceTimeline::EntryTypes::first_input)
        return PerformanceTimeline::ShouldAddEntry::Yes;

    // 2. Assert that entry's entryType attribute value equals "event".
    VERIFY(entry_type() == PerformanceTimeline::EntryTypes::event);

    // 3. Let minDuration be computed as follows:
    HighResolutionTime::DOMHighResTimeStamp min_duration = 0;

    // 1. If options is not present or if options's durationThreshold is not present, let minDuration be 104.
    if (!options.has_value() || !options->duration_threshold.has_value())
        min_duration = 104;
    // 2. Otherwise, let minDuration be the maximum between 16 and options's durationThreshold value.
    else
        min_duration = max(16.0, options->duration_threshold.value());

    // 4. If entry's duration attribute value is greater than or equal to minDuration, return true.
    if (duration() >= min_duration)
        return PerformanceTimeline::ShouldAddEntry::Yes;

    // 5. Otherwise, return false.
    return PerformanceTimeline::ShouldAddEntry::No;
}

void PerformanceEventTiming::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_target);
}

}
