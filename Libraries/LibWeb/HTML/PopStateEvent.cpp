/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PopStateEventPrototype.h>
#include <LibWeb/HTML/PopStateEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(PopStateEvent);

[[nodiscard]] JS::NonnullGCPtr<PopStateEvent> PopStateEvent::create(JS::Realm& realm, FlyString const& event_name, PopStateEventInit const& event_init)
{
    return realm.heap().allocate<PopStateEvent>(realm, realm, event_name, event_init);
}

JS::NonnullGCPtr<PopStateEvent> PopStateEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, PopStateEventInit const& event_init)
{
    return realm.heap().allocate<PopStateEvent>(realm, realm, event_name, event_init);
}

PopStateEvent::PopStateEvent(JS::Realm& realm, FlyString const& event_name, PopStateEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_state(event_init.state)
{
}

void PopStateEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PopStateEvent);
}

void PopStateEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_state);
}

}
