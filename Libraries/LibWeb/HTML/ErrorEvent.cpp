/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/HTML/ErrorEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/error_event.rs.
DOM::Event* web_rust_error_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, FlatPtr message, FlatPtr filename, u32 lineno, u32 colno, JSValue error);
void web_rust_error_event_attributes(Bindings::RustInterfaceCell* event, FlatPtr* message, FlatPtr* filename, u32* lineno, u32* colno, JSValue* error);
}

GC::Ref<DOM::Event> create_error_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, ErrorInformation const& error_information)
{
    return *web_rust_error_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed,
        error_information.message.raw_identity(), error_information.filename.raw_identity(), static_cast<u32>(error_information.lineno), static_cast<u32>(error_information.colno), JS::HostABI::value_to_abi(error_information.error));
}

bool is_error_event(DOM::Event const& event)
{
    return Bindings::rust_event_owner(event, Bindings::InterfaceName::ErrorEvent);
}

Optional<ErrorInformation> error_event_information(DOM::Event const& event)
{
    auto* owner = Bindings::rust_event_owner(event, Bindings::InterfaceName::ErrorEvent);
    if (!owner)
        return {};

    FlatPtr message = 0;
    FlatPtr filename = 0;
    u32 lineno = 0;
    u32 colno = 0;
    JSValue error {};
    web_rust_error_event_attributes(owner, &message, &filename, &lineno, &colno, &error);
    return ErrorInformation {
        .message = Utf16String::adopt_raw(message),
        .filename = Utf16String::adopt_raw(filename),
        .error = JS::HostABI::value_from_abi(error),
        .lineno = lineno,
        .colno = colno,
    };
}

}
