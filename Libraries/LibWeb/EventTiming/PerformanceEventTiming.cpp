/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>

namespace Web::EventTiming {

extern "C" {
// Defined in Bindings/Rust/src/event_timing/performance_event_timing.rs.
PerformanceEventTiming* web_rust_event_timing_create_performance_event_timing(JSVM*, FlatPtr name, double start_time, double processing_start, bool cancelable, u64 interaction_id);
PerformanceEventTiming* web_rust_event_timing_copy_with_entry_type(JSVM*, PerformanceEventTiming const*, FlatPtr entry_type);
bool web_rust_event_timing_has_duration(PerformanceEventTiming const*);
void web_rust_event_timing_set_duration(PerformanceEventTiming*, double duration);
void web_rust_event_timing_set_processing_end(PerformanceEventTiming*, double processing_end);
void web_rust_event_timing_set_event_target(JSVM*, PerformanceEventTiming*, DOM::Node*);
void web_rust_event_timing_set_target_selector(PerformanceEventTiming*, FlatPtr target_selector);
u64 web_rust_event_timing_interaction_id(PerformanceEventTiming const*);
void web_rust_event_timing_set_interaction_id(PerformanceEventTiming*, u64 interaction_id);
}

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ref<PerformanceEventTiming> PerformanceEventTiming::create(DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id)
{
    return *web_rust_event_timing_create_performance_event_timing(
        JS::HostABI::vm_to_abi(JS::VM::the()),
        Utf16String::from_utf16(event.type().view()).into_raw(),
        event.time_stamp(),
        processing_start,
        event.cancelable(),
        interaction_id);
}

GC::Ref<PerformanceEventTiming> PerformanceEventTiming::copy_with_entry_type(Utf16FlyString const& entry_type) const
{
    return *web_rust_event_timing_copy_with_entry_type(JS::HostABI::vm_to_abi(JS::VM::the()), this, entry_type.raw_identity());
}

bool PerformanceEventTiming::has_duration() const
{
    return web_rust_event_timing_has_duration(this);
}

void PerformanceEventTiming::set_duration(HighResolutionTime::DOMHighResTimeStamp duration)
{
    web_rust_event_timing_set_duration(this, duration);
}

void PerformanceEventTiming::set_processing_end(HighResolutionTime::DOMHighResTimeStamp processing_end)
{
    web_rust_event_timing_set_processing_end(this, processing_end);
}

void PerformanceEventTiming::set_event_target(GC::Ptr<DOM::Node> event_target)
{
    web_rust_event_timing_set_event_target(JS::HostABI::vm_to_abi(JS::VM::the()), this, event_target.ptr());
}

void PerformanceEventTiming::set_target_selector(Utf16String target_selector)
{
    web_rust_event_timing_set_target_selector(this, move(target_selector).into_raw());
}

WebIDL::UnsignedLongLong PerformanceEventTiming::interaction_id() const
{
    return web_rust_event_timing_interaction_id(this);
}

void PerformanceEventTiming::set_interaction_id(WebIDL::UnsignedLongLong interaction_id)
{
    web_rust_event_timing_set_interaction_id(this, interaction_id);
}

}
