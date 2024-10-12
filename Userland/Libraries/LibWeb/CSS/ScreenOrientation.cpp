/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ScreenOrientationPrototype.h>
#include <LibWeb/CSS/ScreenOrientation.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(ScreenOrientation);

ScreenOrientation::ScreenOrientation(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void ScreenOrientation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ScreenOrientation);
}

JS::NonnullGCPtr<ScreenOrientation> ScreenOrientation::create(JS::Realm& realm)
{
    return realm.heap().allocate<ScreenOrientation>(realm, realm);
}

// https://w3c.github.io/screen-orientation/#lock-method
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> ScreenOrientation::lock(Bindings::OrientationLockType)
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: ScreenOrientation::lock() is not implemented"_string);
}

// https://w3c.github.io/screen-orientation/#unlock-method
void ScreenOrientation::unlock()
{
    dbgln("FIXME: Stubbed ScreenOrientation::unlock()");
}

// https://w3c.github.io/screen-orientation/#type-attribute
Bindings::OrientationType ScreenOrientation::type() const
{
    dbgln("FIXME: Stubbed ScreenOrientation::type()");
    return Bindings::OrientationType::LandscapePrimary;
}

// https://w3c.github.io/screen-orientation/#angle-attribute
WebIDL::UnsignedShort ScreenOrientation::angle() const
{
    dbgln("FIXME: Stubbed ScreenOrientation::angle()");
    return 0;
}

// https://w3c.github.io/screen-orientation/#onchange-event-handler-attribute
void ScreenOrientation::set_onchange(JS::GCPtr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://w3c.github.io/screen-orientation/#onchange-event-handler-attribute
JS::GCPtr<WebIDL::CallbackType> ScreenOrientation::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
