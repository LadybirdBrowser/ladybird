/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransformer.h>
#include <LibWeb/WebRTC/RTCTransformEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCTransformEvent);

GC::Ref<RTCTransformEvent> RTCTransformEvent::create(Utf16FlyString const& event_name, GC::Ref<RTCRtpScriptTransformer> transformer, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<RTCTransformEvent>(event_name, transformer, time_stamp);
}

RTCTransformEvent::RTCTransformEvent(Utf16FlyString const& event_name, GC::Ref<RTCRtpScriptTransformer> transformer, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_transformer(transformer)
{
}

RTCTransformEvent::~RTCTransformEvent() = default;

void RTCTransformEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_transformer);
}

GC::Ref<RTCRtpScriptTransformer> RTCTransformEvent::transformer() const { return m_transformer; }

}
