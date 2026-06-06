/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Serial.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Serial {

// https://wicg.github.io/serial/#serial-interface
class Serial : public DOM::EventTarget {
    WEB_WRAPPABLE(Serial, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Serial);

public:
    [[nodiscard]] static GC::Ref<Serial> create();

    // https://wicg.github.io/serial/#requestport-method
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> request_port(JS::Realm&, Bindings::SerialPortRequestOptions const& = {});
    // https://wicg.github.io/serial/#getports-method
    GC::Ref<WebIDL::Promise> get_ports(JS::Realm&);

    // https://wicg.github.io/serial/#onconnect-attribute
    void set_onconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* onconnect();

    // https://wicg.github.io/serial/#ondisconnect-attribute
    void set_ondisconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* ondisconnect();

private:
    explicit Serial();
};

}
