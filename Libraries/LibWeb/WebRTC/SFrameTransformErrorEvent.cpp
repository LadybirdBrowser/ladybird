/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/SFrameTransformErrorEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(SFrameTransformErrorEvent);

GC::Ref<SFrameTransformErrorEvent> SFrameTransformErrorEvent::create(Utf16FlyString const& event_name, SFrameTransformErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<SFrameTransformErrorEvent>(event_name, event_init, time_stamp);
}

// FIXME: SFrameTransformErrorEvent is a stub — store the errorType/keyID/frame once the attributes are implemented.
SFrameTransformErrorEvent::SFrameTransformErrorEvent(Utf16FlyString const& event_name, SFrameTransformErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

SFrameTransformErrorEvent::~SFrameTransformErrorEvent() = default;

}
