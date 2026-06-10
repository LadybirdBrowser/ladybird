/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/MediaSourceExtensions/BufferedChangeEvent.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(BufferedChangeEvent);

GC::Ref<BufferedChangeEvent> BufferedChangeEvent::create(AK::FlyString const& type, BufferedChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<BufferedChangeEvent>(type, event_init, time_stamp);
}

BufferedChangeEvent::BufferedChangeEvent(AK::FlyString const& type, BufferedChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
{
}

BufferedChangeEvent::~BufferedChangeEvent() = default;

}
