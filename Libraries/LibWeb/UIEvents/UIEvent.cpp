/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/UIEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(UIEvent);

static HighResolutionTime::DOMHighResTimeStamp event_time_stamp(HTML::Window& window)
{
    return HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));
}

GC::Ref<UIEvent> UIEvent::create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<UIEvent>(event_name, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<UIEvent>> UIEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::UIEventInit const& event_init)
{
    return GC::Heap::the().allocate<UIEvent>(event_name, event_init, event_time_stamp(window));
}

UIEvent::UIEvent(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, time_stamp)
{
}

UIEvent::UIEvent(FlyString const& event_name, Bindings::UIEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, event_init, time_stamp)
    , m_view(event_init.view)
    , m_detail(event_init.detail)
{
}

UIEvent::~UIEvent() = default;

void UIEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_view);
}

}
