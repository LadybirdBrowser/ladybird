/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransitionEvent.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TransitionEventPrototype.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(TransitionEvent);

GC::Ref<TransitionEvent> TransitionEvent::create(JS::Realm& realm, FlyString const& type, TransitionEventInit const& event_init)
{
    auto event = realm.create<TransitionEvent>(realm, type, event_init);
    event->set_is_trusted(true);
    return event;
}

GC::Ref<TransitionEvent> TransitionEvent::construct_impl(JS::Realm& realm, FlyString const& type, TransitionEventInit const& event_init)
{
    return realm.create<TransitionEvent>(realm, type, event_init);
}

TransitionEvent::TransitionEvent(JS::Realm& realm, FlyString const& type, TransitionEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_property_name(event_init.property_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

TransitionEvent::~TransitionEvent() = default;

void TransitionEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TransitionEvent);
}

}
