/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/NavigationActivation.h>
#include <LibWeb/HTML/PageSwapEvent.h>
#include <LibWeb/ViewTransition/ViewTransition.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PageSwapEvent);

GC::Ref<PageSwapEvent> PageSwapEvent::create(FlyString const& event_name, PageSwapEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PageSwapEvent>(event_name, event_init, time_stamp);
}

PageSwapEvent::PageSwapEvent(FlyString const& event_name, PageSwapEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
