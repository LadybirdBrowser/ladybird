/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::EventTiming {

// NB: The relevant global object of the given event target, if it is a Window. Only nodes and windows are the targets
//     of events that are timed.
HTML::Window* relevant_window_of_event_target(DOM::EventTarget&);

// https://w3c.github.io/event-timing/#considered-for-event-timing
bool is_considered_for_event_timing(DOM::Event const&);

// https://w3c.github.io/event-timing/#compute-interactionid
WebIDL::UnsignedLongLong compute_interaction_id(DOM::Event const&, HTML::Window&);

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ptr<PerformanceEventTiming> initialize_event_timing(DOM::Event const&, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id);

// https://w3c.github.io/event-timing/#finalize-event-timing
void finalize_event_timing(GC::Ptr<PerformanceEventTiming>, DOM::Event const&, DOM::EventTarget& target, HighResolutionTime::DOMHighResTimeStamp processing_end);

// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
void dispatch_pending_event_timing_entries(DOM::Document&);

// Whether dispatching the document's pending Event Timing entries would deliver anything to script: an entry it would
// queue, or the duration and event count of a pending pointerdown or keydown it would set.
bool has_pending_event_timing_entries(DOM::Document const&);

// https://w3c.github.io/event-timing/#sec-modifications-HTML
bool has_dispatched_input_event(HTML::Window&);

}
