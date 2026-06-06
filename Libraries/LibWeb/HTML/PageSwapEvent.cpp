/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PageSwapEvent.h>
#include <LibWeb/HTML/NavigationActivation.h>
#include <LibWeb/HTML/PageSwapEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ViewTransition/ViewTransition.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PageSwapEvent);

WebIDL::ExceptionOr<GC::Ref<PageSwapEvent>> PageSwapEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::PageSwapEventInit const& event_init)
{
    return GC::Heap::the().allocate<PageSwapEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

PageSwapEvent::PageSwapEvent(FlyString const& event_name, Bindings::PageSwapEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_activation(event_init.activation)
    , m_view_transition(event_init.view_transition)
{
}

PageSwapEvent::~PageSwapEvent() = default;

void PageSwapEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_activation);
    visitor.visit(m_view_transition);
}

}
