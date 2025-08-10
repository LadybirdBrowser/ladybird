/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <serial_cpp/serial.h>

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
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> Serial::request_port(SerialPortRequestOptions const options)
{
    auto& realm = this->realm();
    auto const& relevant_global_object = as<HTML::Window>(HTML::relevant_global_object(*this));

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "serial",
    //    reject promise with a "SecurityError" DOMException and return promise.
    if (!relevant_global_object.associated_document().is_allowed_to_use_feature(DOM::PolicyControlledFeature::WebSerial)) {
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Failed to execute 'requestPort' on 'Serial': WebSerial feature is not enabled."_utf16));
    }

    // 3. If the relevant global object of this does not have transient activation, reject promise with a "SecurityError" DOMException and return promise.
    if (!relevant_global_object.has_transient_activation()) {
        return WebIDL::create_rejected_promise(realm, WebIDL::SecurityError::create(realm, "Failed to execute 'requestPort' on 'Serial': Must be handling a user gesture to show a permission request."_utf16));
    }

    // 4. If options["filters"] is present, then for each filter in options["filters"] run the following steps:
    if (options.filters.has_value()) {
        for (auto const& filter : options.filters.value()) {
            // 1. If filter["bluetoothServiceClassId"] is present:
            if (filter.bluetooth_service_class_id.has_value()) {
                // 1. If filter["usbVendorId"] is present, reject promise with a TypeError and return promise.
                if (filter.usb_vendor_id.has_value()) {
                    return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "A filter cannot specify both bluetoothServiceClassId and usbVendorId or usbProductId."_utf16));
                }

                // 2. If filter["usbProductId"] is present, reject promise with a TypeError and return promise.
                if (filter.usb_product_id.has_value()) {
                    return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "A filter cannot specify both bluetoothServiceClassId and usbVendorId or usbProductId."_utf16));
                }
            }

            // 2. If filter["usbVendorId"] is not present, reject promise with a TypeError and return promise.
            if (filter.usb_vendor_id.has_value()) {
                return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "A filter containing a usbProductId must also specify a usbVendorId."_utf16));
            }
        }
    }

    // 5. Run the following steps in parallel
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, &relevant_global_object, promise, options]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. Let allPorts be an empty list.
        Vector<GC::Ref<SerialPort>> all_ports;

        // FIXME: 2. For each Bluetooth device registered with the system:

        // 3. For each available non-Bluetooth serial port:
        for (auto const& device : serial_cpp::list_ports()) {
            // 1. Let port be a SerialPort representing the port.
            auto port = realm.create<SerialPort>(realm, device);

            // 2. Append port to allPorts.
            all_ports.append(port);
        }

        // 4. Prompt the user to grant the site access to a serial port by presenting them with a list of ports
        //    in allPorts that match any filter in options["filters"] if present and allPorts otherwise.
        // NOTE: Since we don't have a UI prompt we will just select the first matching port presented by the user on the command line.
        auto const& configured_device_path = relevant_global_object.associated_document().page().webserial_device_path().value();
        auto const& selected_port = all_ports.first_matching([&configured_device_path, options](auto const& serial_port) {
            // FIXME: Filter ports by options["filters"] if present.
            return serial_port->device().port.c_str() == configured_device_path;
        });

        // 5. If the user does not choose a port, queue a global task on the relevant global object of this using the
        //    serial port task source to reject promise with a "NotFoundError" DOMException and abort these steps.
        if (!selected_port.has_value()) {
            queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() {
                HTML::TemporaryExecutionContext context(realm);
                WebIDL::reject_promise(realm, promise, WebIDL::NotFoundError::create(realm, "Failed to execute 'requestPort' on 'Serial': No port selected by the user."_utf16));
            }));
            return;
        }

        // 6. Let port be a SerialPort representing the port chosen by the user.
        auto port = selected_port.value();

        // 7. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with port.
        queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, port]() {
            HTML::TemporaryExecutionContext context(realm);
            WebIDL::resolve_promise(realm, promise, move(port));
        }));
    }));

    // 6. Return promise.
    return promise;
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
