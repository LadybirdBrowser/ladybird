/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SerialPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Serial/Serial.h>
#include <LibWeb/Serial/SerialPort.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Serial {

GC_DEFINE_ALLOCATOR(Serial);

Serial::Serial(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void Serial::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Serial);
    Base::initialize(realm);
}

// https://wicg.github.io/serial/#requestport-method
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> Serial::request_port(SerialPortRequestOptions const&)
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "serial",
    //           reject promise with a "SecurityError" DOMException and return promise.

    // FIXME: 3. If the relevant global object of this does not have transient activation, reject promise with a "SecurityError" DOMException and return promise.

    // FIXME: 4. If options["filters"] is present, then for each filter in options["filters"] run the following steps:

    // FIXME: 5. Run the following steps in parallel:
    {
        // FIXME: 1. Let allPorts be an empty list.

        // FIXME: 2. For each Bluetooth device registered with the system:

        // FIXME: 3. For each available non-Bluetooth serial port:
        {
            // FIXME: 1. Let port be a SerialPort representing the port.

            // FIXME: 2. Append port to allPorts.
        }

        // FIXME: 4. Prompt the user to grant the site access to a serial port by presenting them with a list of ports
        //           in allPorts that match any filter in options["filters"] if present and allPorts otherwise.

        // FIXME: 5. If the user does not choose a port, queue a global task on the relevant global object of this using the
        //           serial port task source to reject promise with a "NotFoundError" DOMException and abort these steps.

        // FIXME: 6. Let port be a SerialPort representing the port chosen by the user.

        // FIXME: 7. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with port.
    }

    // 6. Return promise.
    dbgln("FIXME: Unimplemented Serial::request_port()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#getports-method
GC::Ref<WebIDL::Promise> Serial::get_ports()
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "serial",
    //         reject promise with a "SecurityError" DOMException and return promise.

    // FIXME: 3. Run the following steps in parallel:
    {
        // FIXME: 1. Let availablePorts be the sequence of available serial ports which the user has allowed the site to
        //           access as the result of a previous call to requestPort().

        // FIXME: 2. Let ports be the sequence of the SerialPorts representing the ports in availablePorts.

        // FIXME: 3. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with ports.
    }

    // 4. Return promise.
    dbgln("FIXME: Unimplemented Serial::get_ports()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#onconnect-attribute
void Serial::set_onconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::connect, event_handler);
}

WebIDL::CallbackType* Serial::onconnect()
{
    return event_handler_attribute(HTML::EventNames::connect);
}

// https://wicg.github.io/serial/#ondisconnect-attribute
void Serial::set_ondisconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::disconnect, event_handler);
}

WebIDL::CallbackType* Serial::ondisconnect()
{
    return event_handler_attribute(HTML::EventNames::disconnect);
}

}
