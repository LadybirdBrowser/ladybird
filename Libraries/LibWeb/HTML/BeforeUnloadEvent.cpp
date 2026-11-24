/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/before_unload_event.rs.
DOM::Event* web_rust_before_unload_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed);
DOM::Event* web_rust_before_unload_event_create_for_create_event(JSVM*, void* realm);
FlatPtr web_rust_before_unload_event_return_value(Bindings::RustInterfaceCell* event);
void web_rust_before_unload_event_set_return_value(Bindings::RustInterfaceCell* event, FlatPtr return_value);
}

GC::Ref<DOM::Event> create_before_unload_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init)
{
    return *web_rust_before_unload_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed);
}

GC::Ref<DOM::Event> create_before_unload_event(JS::Realm& realm)
{
    return *web_rust_before_unload_event_create_for_create_event(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

Optional<Utf16String> before_unload_event_return_value(DOM::Event const& event)
{
    auto* owner = Bindings::rust_event_owner(event, Bindings::InterfaceName::BeforeUnloadEvent);
    if (!owner)
        return {};
    return Utf16String::adopt_raw(web_rust_before_unload_event_return_value(owner));
}

void set_before_unload_event_return_value(DOM::Event& event, Utf16String const& return_value)
{
    auto* owner = Bindings::rust_event_owner(event, Bindings::InterfaceName::BeforeUnloadEvent);
    VERIFY(owner);
    web_rust_before_unload_event_set_return_value(owner, return_value.raw_identity());
}

}
