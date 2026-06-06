/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/HTML/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

GC::Ref<NavigationCurrentEntryChangeEvent> NavigationCurrentEntryChangeEvent::create(FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<NavigationCurrentEntryChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<NavigationCurrentEntryChangeEvent> NavigationCurrentEntryChangeEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const& event_init)
{
    return GC::Heap::the().allocate<NavigationCurrentEntryChangeEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

NavigationCurrentEntryChangeEvent::NavigationCurrentEntryChangeEvent(FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_navigation_type(event_init.navigation_type.value_or({}))
    , m_from(*event_init.from)
{
}

NavigationCurrentEntryChangeEvent::~NavigationCurrentEntryChangeEvent() = default;

void NavigationCurrentEntryChangeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_from);
}

}
