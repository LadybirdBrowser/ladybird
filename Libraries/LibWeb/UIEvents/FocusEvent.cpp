/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/UIEvents/FocusEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(FocusEvent);

GC::Ref<FocusEvent> FocusEvent::create(FlyString const& event_name, FocusEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<FocusEvent>(event_name, event_init, time_stamp);
}

FocusEvent::FocusEvent(FlyString const& event_name, FocusEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, event_init, time_stamp)
{
    set_related_target(const_cast<DOM::EventTarget*>(event_init.related_target.ptr()));
}

FocusEvent::~FocusEvent() = default;

}
