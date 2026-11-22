/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/exports.rs.
DOM::Event* web_rust_html_create_message_event(JSVM*, void* realm, FlatPtr type, JSValue data, MessagePort* const* ports, size_t count, MessagePort* source, URL::FFI::FfiOrigin const* origin);
DOM::Event* web_rust_html_create_message_event_for_create_event(JSVM*, void* realm);
}

GC::Ref<DOM::Event> create_message_event(JS::Realm& realm, Utf16FlyString const& type, MessageEventInit const& event_init, Optional<URL::Origin> const& origin)
{
    GC::RootVector<MessagePort*> ports;
    ports.ensure_capacity(event_init.ports.size());
    for (auto const& port : event_init.ports)
        ports.unchecked_append(port.ptr());

    auto create = [&](URL::FFI::FfiOrigin const* ffi_origin) {
        return web_rust_html_create_message_event(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), JS::HostABI::value_to_abi(event_init.data), ports.data(), ports.size(), event_init.source.ptr(), ffi_origin);
    };
    if (!origin.has_value())
        return *create(nullptr);

    DOM::Event* event = nullptr;
    URL::RustIntegration::lend_origin_to_ffi(*origin, [&](URL::FFI::FfiOrigin const& ffi_origin) {
        event = create(&ffi_origin);
    });
    return *event;
}

GC::Ref<DOM::Event> create_message_event(JS::Realm& realm)
{
    return *web_rust_html_create_message_event_for_create_event(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

}
