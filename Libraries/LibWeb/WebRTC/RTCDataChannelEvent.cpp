/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCDataChannel.h>
#include <LibWeb/WebRTC/RTCDataChannelEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCDataChannelEvent);

GC::Ref<RTCDataChannelEvent> RTCDataChannelEvent::create(Utf16FlyString const& event_name, RTCDataChannelEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCDataChannelEvent>(event_name, event_init, time_stamp);
}

RTCDataChannelEvent::RTCDataChannelEvent(Utf16FlyString const& event_name, RTCDataChannelEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_channel(event_init.channel)
{
}

RTCDataChannelEvent::~RTCDataChannelEvent() = default;

void RTCDataChannelEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_channel);
}

}
