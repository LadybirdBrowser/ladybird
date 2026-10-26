/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCIceCandidate.h>
#include <LibWeb/WebRTC/RTCPeerConnectionIceEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCPeerConnectionIceEvent);

GC::Ref<RTCPeerConnectionIceEvent> RTCPeerConnectionIceEvent::create(Utf16FlyString const& event_name, RTCPeerConnectionIceEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCPeerConnectionIceEvent>(event_name, event_init, time_stamp);
}

RTCPeerConnectionIceEvent::RTCPeerConnectionIceEvent(Utf16FlyString const& event_name, RTCPeerConnectionIceEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_candidate(event_init.candidate)
    , m_url(event_init.url.value_or(Optional<Utf16String> {}))
{
}

RTCPeerConnectionIceEvent::~RTCPeerConnectionIceEvent() = default;

void RTCPeerConnectionIceEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_candidate);
}

}
