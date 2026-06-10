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
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

GC::Ref<NavigationCurrentEntryChangeEvent> NavigationCurrentEntryChangeEvent::create(JS::Realm& realm, FlyString const& event_name, Bindings::NavigationCurrentEntryChangeEventInit const& event_init)
{
    Optional<NavigationType> navigation_type;
    if (event_init.navigation_type.has_value() && event_init.navigation_type->has_value())
        navigation_type = event_init.navigation_type->value();

    NavigationCurrentEntryChangeEventInit init {
        { .bubbles = event_init.bubbles, .cancelable = event_init.cancelable, .composed = event_init.composed },
        event_init.from,
        navigation_type,
    };

    auto& window = HTML::relevant_window(realm.global_object());
    return create(event_name, init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

GC::Ref<NavigationCurrentEntryChangeEvent> NavigationCurrentEntryChangeEvent::create(FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<NavigationCurrentEntryChangeEvent>(event_name, event_init, time_stamp);
}

NavigationCurrentEntryChangeEvent::NavigationCurrentEntryChangeEvent(FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_navigation_type(event_init.navigation_type)
    , m_from(event_init.from)
{
}

NavigationCurrentEntryChangeEvent::~NavigationCurrentEntryChangeEvent() = default;

void NavigationCurrentEntryChangeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_from);
}

}
