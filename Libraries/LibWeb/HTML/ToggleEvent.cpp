/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ToggleEventPrototype.h>
#include <LibWeb/HTML/ToggleEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ToggleEvent);

GC::Ref<ToggleEvent> ToggleEvent::create(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init, GC::Ptr<DOM::Element> source)
{
    return realm.create<ToggleEvent>(realm, event_name, move(event_init), source);
}

WebIDL::ExceptionOr<GC::Ref<ToggleEvent>> ToggleEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

ToggleEvent::ToggleEvent(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init, GC::Ptr<DOM::Element> source)
    : DOM::Event(realm, event_name, event_init)
    , m_old_state(move(event_init.old_state))
    , m_new_state(move(event_init.new_state))
    , m_source(source)
{
}

void ToggleEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ToggleEvent);
    Base::initialize(realm);
}

void ToggleEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
