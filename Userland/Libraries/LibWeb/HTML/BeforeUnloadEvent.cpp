/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/BeforeUnloadEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(BeforeUnloadEvent);

JS::NonnullGCPtr<BeforeUnloadEvent> BeforeUnloadEvent::create(JS::Realm& realm, FlyString const& event_name, DOM::EventInit const& event_init)
{
    return realm.heap().allocate<BeforeUnloadEvent>(realm, realm, event_name, event_init);
}

BeforeUnloadEvent::BeforeUnloadEvent(JS::Realm& realm, FlyString const& event_name, DOM::EventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
{
}

BeforeUnloadEvent::~BeforeUnloadEvent() = default;

void BeforeUnloadEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BeforeUnloadEvent);
}

}
