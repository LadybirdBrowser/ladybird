/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SensorErrorEventPrototype.h>
#include <LibWeb/Sensors/SensorErrorEvent.h>

namespace Web::Sensors {

GC_DEFINE_ALLOCATOR(SensorErrorEvent);

GC::Ref<SensorErrorEvent> SensorErrorEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, SensorErrorEventInit const& event_init)
{
    return realm.create<SensorErrorEvent>(realm, event_name, event_init);
}

SensorErrorEvent::SensorErrorEvent(JS::Realm& realm, FlyString const& event_name, SensorErrorEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_error(event_init.error)
{
}

void SensorErrorEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SensorErrorEvent);
}

void SensorErrorEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
}

}
