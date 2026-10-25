/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::EventTiming {

// https://w3c.github.io/event-timing/#sec-performance-event-timing
class PerformanceEventTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_WRAPPABLE(PerformanceEventTiming, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(PerformanceEventTiming);

public:
    static GC::Ref<PerformanceEventTiming> create(DOM::Event const&, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id);

    virtual ~PerformanceEventTiming();

    // A copy of this entry, with the given entry type.
    GC::Ref<PerformanceEventTiming> copy_with_entry_type(Utf16FlyString const& entry_type) const;

    virtual Utf16FlyString const& entry_type() const override { return m_entry_type; }

    virtual HighResolutionTime::DOMHighResTimeStamp duration() const override { return m_duration; }
    void set_duration(HighResolutionTime::DOMHighResTimeStamp duration)
    {
        m_duration = duration;
        m_has_duration = true;
    }
    bool has_duration() const { return m_has_duration; }

    HighResolutionTime::DOMHighResTimeStamp processing_start() const { return m_processing_start; }

    HighResolutionTime::DOMHighResTimeStamp processing_end() const { return m_processing_end; }
    void set_processing_end(HighResolutionTime::DOMHighResTimeStamp processing_end) { m_processing_end = processing_end; }

    bool cancelable() const { return m_cancelable; }

    GC::Ptr<DOM::Node> target() const;
    void set_event_target(GC::Ptr<DOM::Node> event_target) { m_event_target = event_target; }

    Utf16String const& target_selector() const { return m_target_selector; }
    void set_target_selector(Utf16String target_selector) { m_target_selector = move(target_selector); }

    WebIDL::UnsignedLongLong interaction_id() const { return m_interaction_id; }
    void set_interaction_id(WebIDL::UnsignedLongLong interaction_id) { m_interaction_id = interaction_id; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override;

private:
    PerformanceEventTiming(Utf16String const& name, Utf16FlyString const& entry_type, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp processing_start, bool cancelable, WebIDL::UnsignedLongLong interaction_id);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // NB: An entry is "event", unless it is the copy of an entry that is queued as the first input.
    Utf16FlyString m_entry_type;

    HighResolutionTime::DOMHighResTimeStamp m_duration { 0 };
    bool m_has_duration { false };
    HighResolutionTime::DOMHighResTimeStamp m_processing_start { 0 };
    HighResolutionTime::DOMHighResTimeStamp m_processing_end { 0 };
    bool m_cancelable { false };

    // https://w3c.github.io/event-timing/#eventtarget
    GC::Ptr<DOM::Node> m_event_target;

    Utf16String m_target_selector;
    WebIDL::UnsignedLongLong m_interaction_id { 0 };
};

// NOTE: These two functions are answered by the registry for the given entry type.
// https://w3c.github.io/timing-entrytypes-registry/#registry
// NB: PerformanceEventTiming objects have one of two entry types, which the registry answers for separately.
struct EventEntryTypeRegistry {
    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::No; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return 150; }
};

struct FirstInputEntryTypeRegistry {
    // https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    static PerformanceTimeline::AvailableFromTimeline available_from_timeline() { return PerformanceTimeline::AvailableFromTimeline::Yes; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    static Optional<u64> max_buffer_size() { return 1; }
};

}
