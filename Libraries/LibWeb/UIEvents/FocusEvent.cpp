/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FocusEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/FocusEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(FocusEvent);

GC::Ref<FocusEvent> FocusEvent::create(FlyString const& event_name, Bindings::FocusEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<FocusEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<FocusEvent>> FocusEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::FocusEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

FocusEvent::FocusEvent(FlyString const& event_name, Bindings::FocusEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, event_init, time_stamp)
{
    set_related_target(const_cast<DOM::EventTarget*>(event_init.related_target.ptr()));
}

FocusEvent::~FocusEvent() = default;

}
