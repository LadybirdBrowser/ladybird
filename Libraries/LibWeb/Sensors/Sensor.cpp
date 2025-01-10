/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/SensorPrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Sensors/Sensor.h>

namespace Web::Sensors {

GC_DEFINE_ALLOCATOR(Sensor);

WebIDL::ExceptionOr<GC::Ref<Sensor>> Sensor::construct_impl(JS::Realm& realm)
{
    return realm.create<Sensor>(realm);
}

Sensor::Sensor(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void Sensor::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Sensor);
}

void Sensor::set_onreading(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::reading, value);
}

WebIDL::CallbackType* Sensor::onreading()
{
    return event_handler_attribute(HTML::EventNames::reading);
}

void Sensor::set_onactivate(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::activate, value);
}

WebIDL::CallbackType* Sensor::onactivate()
{
    return event_handler_attribute(HTML::EventNames::activate);
}

void Sensor::set_onerror(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::error, value);
}

WebIDL::CallbackType* Sensor::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

}
