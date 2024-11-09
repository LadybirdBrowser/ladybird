/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ToggleEventPrototype.h>
#include <LibWeb/HTML/ToggleEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ToggleEvent);

JS::NonnullGCPtr<ToggleEvent> ToggleEvent::create(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init)
{
    return realm.heap().allocate<ToggleEvent>(realm, realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<ToggleEvent>> ToggleEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

ToggleEvent::ToggleEvent(JS::Realm& realm, FlyString const& event_name, ToggleEventInit event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_old_state(move(event_init.old_state))
    , m_new_state(move(event_init.new_state))
{
}

void ToggleEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ToggleEvent);
}

}
