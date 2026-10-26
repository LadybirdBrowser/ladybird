/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCErrorEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCErrorEvent);

GC::Ref<RTCErrorEvent> RTCErrorEvent::create(Utf16FlyString const& event_name, RTCErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCErrorEvent>(event_name, event_init, time_stamp);
}

// FIXME: RTCErrorEvent is a stub — store the error once the attribute is implemented.
RTCErrorEvent::RTCErrorEvent(Utf16FlyString const& event_name, RTCErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

RTCErrorEvent::~RTCErrorEvent() = default;

}
