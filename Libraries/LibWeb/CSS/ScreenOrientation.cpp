/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ScreenOrientation.h>
#include <LibWeb/CSS/ScreenOrientation.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(ScreenOrientation);

ScreenOrientation::ScreenOrientation()
    : DOM::EventTarget()
{
}

GC::Ref<ScreenOrientation> ScreenOrientation::create()
{
    return GC::Heap::the().allocate<ScreenOrientation>();
}

// https://w3c.github.io/screen-orientation/#lock-method
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> ScreenOrientation::lock(JS::Realm& realm, Bindings::OrientationLockType)
{
    return WebIDL::NotSupportedError::create(realm, "FIXME: ScreenOrientation::lock() is not implemented"_utf16);
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
void ScreenOrientation::set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://w3c.github.io/screen-orientation/#onchange-event-handler-attribute
GC::Ptr<WebIDL::CallbackType> ScreenOrientation::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
