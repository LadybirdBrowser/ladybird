/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebRTC/KeyFrameRequestEvent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(KeyFrameRequestEvent);

GC::Ref<KeyFrameRequestEvent> KeyFrameRequestEvent::create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<KeyFrameRequestEvent>(event_name, time_stamp);
}

// FIXME: KeyFrameRequestEvent is a stub — store the rid once the attribute is implemented.
GC::Ref<KeyFrameRequestEvent> KeyFrameRequestEvent::create_for_constructor(Utf16String const& type, Optional<Utf16String> const&, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(Utf16FlyString { type }, time_stamp);
}
KeyFrameRequestEvent::KeyFrameRequestEvent(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
{
}
KeyFrameRequestEvent::~KeyFrameRequestEvent() = default;

}
