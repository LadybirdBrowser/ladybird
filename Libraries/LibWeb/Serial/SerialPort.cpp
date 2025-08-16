/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Serial/SerialPort.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Serial {

GC_DEFINE_ALLOCATOR(SerialPort);

SerialPort::SerialPort(JS::Realm& realm, serial_cpp::PortInfo device)
    : DOM::EventTarget(realm)
    , m_device(move(device))
{
}

void SerialPort::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SerialPort);
    Base::initialize(realm);
}

// https://wicg.github.io/serial/#getinfo-method
SerialPortInfo SerialPort::get_info() const
{
    // 1. Let info be an empty ordered map.
    auto info = SerialPortInfo {};

    // FIXME: 2. If the port is part of a USB device, perform the following steps:
    {
        // FIXME: 1. Set info["usbVendorId"] to the vendor ID of the device.

        // FIXME: 2. Set info["usbProductId"] to the product ID of the device.
    }

    // FIXME: 3. If the port is a service on a Bluetooth device, perform the following steps:
    {
        // FIXME: 1. Set info["bluetoothServiceClassId"] to the service class UUID of the Bluetooth service.
    }

    // 4. Return info.
    return info;
}

// https://wicg.github.io/serial/#open-method
GC::Ref<WebIDL::Promise> SerialPort::open(SerialOptions)
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "closed", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. If options["dataBits"] is not 7 or 8, reject promise with TypeError and return promise.

    // FIXME: 4. If options["stopBits"] is not 1 or 2, reject promise with TypeError and return promise.

    // FIXME: 5. If options["bufferSize"] is 0, reject promise with TypeError and return promise.

    // FIXME: 6. Optionally, if options["bufferSize"] is larger than the implementation is able to support, reject promise with a TypeError and return promise.

    // FIXME: 7. Set this.[[state]] to "opening".

    // FIXME: 8. Perform the following steps in parallel.
    {
        // FIXME: 1. Invoke the operating system to open the serial port using the connection parameters (or their defaults) specified in options.

        // FIXME: 2. If this fails for any reason, queue a global task on the relevant global object of this using the serial port task source to reject promise with a "NetworkError" DOMException and abort these steps.

        // FIXME: 3. Set this.[[state]] to "opened".

        // FIXME: 4. Set this.[[bufferSize]] to options["bufferSize"].

        // FIXME: 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
    }

    // FIXME: 9. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::open()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#setsignals-method
GC::Ref<WebIDL::Promise> SerialPort::set_signals(SerialOutputSignals)
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. If all of the specified members of signals are not present reject promise with TypeError and return promise.

    // FIXME: 4. Perform the following steps in parallel:
    {
        // FIXME: 1. If signals["dataTerminalReady"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "data terminal ready" or "DTR" signal on the serial port.

        // FIXME: 2. If signals["requestToSend"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "request to send" or "RTS" signal on the serial port.

        // FIXME: 3. If signals["break"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "break" signal on the serial port.

        // FIXME: 4. If the operating system fails to change the state of any of these signals for any reason, queue a global task
        //           on the relevant global object of this using the serial port task source to reject promise with a "NetworkError" DOMException.

        // FIXME: 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
    }

    // 5. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::set_signals()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#getsignals-method
GC::Ref<WebIDL::Promise> SerialPort::get_signals() const
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. Perform the following steps in parallel:
    {
        // FIXME: 1. Query the operating system for the status of the control signals that may be asserted by the device connected to the serial port.

        // FIXME: 2. If the operating system fails to determine the status of these signals for any reason, queue a global task on the relevant global object of
        //           this using the serial port task source to reject promise with a "NetworkError" DOMException and abort these steps.

        // FIXME: 3. Let dataCarrierDetect be true if the "data carrier detect" or "DCD" signal has been asserted by the device, and false otherwise.

        // FIXME: 4. Let clearToSend be true if the "clear to send" or "CTS" signal has been asserted by the device, and false otherwise.

        // FIXME: 5. Let ringIndicator be true if the "ring indicator" or "RI" signal has been asserted by the device, and false otherwise.

        // FIXME: 6. Let dataSetReady be true if the "data set ready" or "DSR" signal has been asserted by the device, and false otherwise.

        // FIXME: 7. Let signals be the ordered map «[ "dataCarrierDetect" → dataCarrierDetect, "clearToSend" → clearToSend, "ringIndicator" → ringIndicator, "dataSetReady" → dataSetReady ]».

        // FIXME: 8. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with signals.
    }

    // 4. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::get_signals()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#close-method
GC::Ref<WebIDL::Promise> SerialPort::close()
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. Let cancelPromise be the result of invoking cancel on this.[[readable]] or a promise resolved with undefined if this.[[readable]] is null.

    // FIXME: 4. Let abortPromise be the result of invoking abort on this.[[writable]] or a promise resolved with undefined if this.[[writable]] is null.

    // FIXME: 5. Let pendingClosePromise be a new promise.

    // FIXME: 6. If this.[[readable]] and this.[[writable]] are null, resolve pendingClosePromise with undefined.

    // FIXME: 7. Set this.[[pendingClosePromise]] to pendingClosePromise.

    // FIXME: 8. Let combinedPromise be the result of getting a promise to wait for all with «cancelPromise, abortPromise, pendingClosePromise».

    // FIXME: 9. Set this.[[state]] to "closing".

    // FIXME: 10. React to combinedPromise.
    {
        // If combinedPromise was fulfilled, then:
        // FIXME: 1. Run the following steps in parallel:
        {
            // FIXME: 1. Invoke the operating system to close the serial port and release any associated resources.

            // FIXME: 2. Set this.[[state]] to "closed".

            // FIXME: 3. Set this.[[readFatal]] and this.[[writeFatal]] to false.

            // FIXME: 4. Set this.[[pendingClosePromise]] to null.

            // FIXME: 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
        }

        // If combinedPromise was rejected with reason r, then:
        {
            // FIXME: 1. Set this.[[pendingClosePromise]] to null.

            // FIXME: 2. Queue a global task on the relevant global object of this using the serial port task source to reject promise with r.
        }
    }

    // 11. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::close()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#forget-method
GC::Ref<WebIDL::Promise> SerialPort::forget()
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 1. If the user agent can't perform this action (e.g. permission was granted by administrator policy), return a promise resolved with undefined.

    // FIXME: 2. Run the following steps in parallel:
    {
        // FIXME: 1. Set this.[[state]] to "forgetting".

        // FIXME: 2. Remove this from the sequence of serial ports on the system which the user has allowed the site to access as the result of a previous call to requestPort().

        // FIXME: 3. Set this.[[state]] to "forgotten".

        // FIXME: 4. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
    }

    // 7. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::forget()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

void SerialPort::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
    visitor.visit(m_pending_close_promise);
}

// https://wicg.github.io/serial/#onconnect-attribute-0
void SerialPort::set_onconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::connect, event_handler);
}

WebIDL::CallbackType* SerialPort::onconnect()
{
    return event_handler_attribute(HTML::EventNames::connect);
}

// https://wicg.github.io/serial/#ondisconnect-attribute-0
void SerialPort::set_ondisconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::disconnect, event_handler);
}

WebIDL::CallbackType* SerialPort::ondisconnect()
{
    return event_handler_attribute(HTML::EventNames::disconnect);
}

}
