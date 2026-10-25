/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::EventTiming {

// https://w3c.github.io/event-timing/#sec-modifications-HTML
// Each Window has the following associated concepts:
struct WindowEventTimingState {
    // entries to be queued, a list that stores PerformanceEventTiming objects, which will initially be empty.
    Vector<GC::Ref<PerformanceEventTiming>> entries_to_be_queued;

    // has dispatched input event, a boolean which is initially set to false.
    bool has_dispatched_input_event { false };

    // user interaction value, an integer which is initially set to a random integer between 100 and 10000.
    u64 user_interaction_value { 0 };

    // pending key downs, a map of integers to PerformanceEventTimings which is initially empty.
    HashMap<u32, GC::Ref<PerformanceEventTiming>> pending_key_downs;

    // pointer interaction value map, a map of integers which is initially empty.
    HashMap<WebIDL::Long, u64> pointer_interaction_value_map;

    // pending pointer downs, a map of integers to PerformanceEventTimings which is initially empty.
    HashMap<WebIDL::Long, GC::Ref<PerformanceEventTiming>> pending_pointer_downs;

    // is contextmenu triggered, a boolean which is initially set to false.
    bool is_contextmenu_triggered { false };

    // eventCounts, a map with entries of the form type → numEvents.
    // NB: This is created along with the Window's Performance object.
    GC::Ptr<EventCounts> event_counts;

    // interactionCount, an integer which counts the total number of distinct user interactions, for which there was a
    // unique interactionId computed via computing interactionId.
    u64 interaction_count { 0 };

    WindowEventTimingState();

    void visit_edges(GC::Cell::Visitor&);
};

// NB: The relevant global object of the given event target, if it is a Window. Only nodes and windows are the targets
//     of events that are timed.
HTML::Window* relevant_window_of_event_target(DOM::EventTarget&);

// https://w3c.github.io/event-timing/#considered-for-event-timing
bool is_considered_for_event_timing(DOM::Event const&);

// Calls the given callback with every event type that is considered for Event Timing.
void for_each_event_type_considered_for_event_timing(Function<void(Utf16FlyString const&)> const&);

// https://w3c.github.io/event-timing/#compute-interactionid
WebIDL::UnsignedLongLong compute_interaction_id(DOM::Event const&, HTML::Window&);

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ptr<PerformanceEventTiming> initialize_event_timing(DOM::Event const&, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id);

// https://w3c.github.io/event-timing/#finalize-event-timing
void finalize_event_timing(GC::Ptr<PerformanceEventTiming>, DOM::Event const&, DOM::EventTarget& target, HighResolutionTime::DOMHighResTimeStamp processing_end);

// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
void dispatch_pending_event_timing_entries(DOM::Document&);

}
