/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Serial {

// https://wicg.github.io/serial/#serialportfilter-dictionary
struct SerialPortFilter {
    Optional<WebIDL::UnsignedShort> usb_vendor_id;
    Optional<WebIDL::UnsignedShort> usb_product_id;
    Optional<String> bluetooth_service_class_id;
};

// https://wicg.github.io/serial/#serialportrequestoptions-dictionary
struct SerialPortRequestOptions {
    Optional<Vector<SerialPortFilter>> filters;
    Optional<Vector<String>> allowed_bluetooth_service_class_ids;
};

// https://wicg.github.io/serial/#serial-interface
class Serial : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Serial, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Serial);

public:
    // https://wicg.github.io/serial/#requestport-method
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> request_port(SerialPortRequestOptions const& = {});
    // https://wicg.github.io/serial/#getports-method
    GC::Ref<WebIDL::Promise> get_ports();

    // https://wicg.github.io/serial/#onconnect-attribute
    void set_onconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* onconnect();

    // https://wicg.github.io/serial/#ondisconnect-attribute
    void set_ondisconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* ondisconnect();

private:
    explicit Serial(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
