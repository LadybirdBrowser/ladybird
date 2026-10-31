/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCPeerConnectionIceErrorEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCPeerConnectionIceErrorEvent);

GC::Ref<RTCPeerConnectionIceErrorEvent> RTCPeerConnectionIceErrorEvent::create(Utf16FlyString const& event_name, RTCPeerConnectionIceErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCPeerConnectionIceErrorEvent>(event_name, event_init, time_stamp);
}

// FIXME: RTCPeerConnectionIceErrorEvent is a stub — store the address/port/url/errorCode/errorText once the attributes are implemented.
RTCPeerConnectionIceErrorEvent::RTCPeerConnectionIceErrorEvent(Utf16FlyString const& event_name, RTCPeerConnectionIceErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

RTCPeerConnectionIceErrorEvent::~RTCPeerConnectionIceErrorEvent() = default;

}
