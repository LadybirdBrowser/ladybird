/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigationCurrentEntryChangeEventPrototype.h>
#include <LibWeb/HTML/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationCurrentEntryChangeEvent);

JS::NonnullGCPtr<NavigationCurrentEntryChangeEvent> NavigationCurrentEntryChangeEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init)
{
    return realm.heap().allocate<NavigationCurrentEntryChangeEvent>(realm, realm, event_name, event_init);
}

NavigationCurrentEntryChangeEvent::NavigationCurrentEntryChangeEvent(JS::Realm& realm, FlyString const& event_name, NavigationCurrentEntryChangeEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_navigation_type(event_init.navigation_type)
    , m_from(*event_init.from)
{
}

NavigationCurrentEntryChangeEvent::~NavigationCurrentEntryChangeEvent() = default;

void NavigationCurrentEntryChangeEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NavigationCurrentEntryChangeEvent);
}

void NavigationCurrentEntryChangeEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_from);
}

}
