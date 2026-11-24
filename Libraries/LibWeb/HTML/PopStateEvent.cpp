/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/PopStateEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/pop_state_event.rs.
DOM::Event* web_rust_pop_state_event_create(JSVM*, void* realm, FlatPtr type, JSValue state);
}

GC::Ref<DOM::Event> create_pop_state_event(JS::Realm& realm, Utf16FlyString const& type, JS::Value state)
{
    return *web_rust_pop_state_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), JS::HostABI::value_to_abi(state));
}

}
