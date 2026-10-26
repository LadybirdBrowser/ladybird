/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCDTMFToneChangeEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCDTMFToneChangeEvent);

GC::Ref<RTCDTMFToneChangeEvent> RTCDTMFToneChangeEvent::create(Utf16FlyString const& event_name, RTCDTMFToneChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCDTMFToneChangeEvent>(event_name, event_init, time_stamp);
}

// FIXME: RTCDTMFToneChangeEvent is a stub — store the tone once the attribute is implemented.
RTCDTMFToneChangeEvent::RTCDTMFToneChangeEvent(Utf16FlyString const& event_name, RTCDTMFToneChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

RTCDTMFToneChangeEvent::~RTCDTMFToneChangeEvent() = default;

}
