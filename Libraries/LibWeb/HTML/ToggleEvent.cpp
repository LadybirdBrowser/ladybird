/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/ToggleEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/toggle_event.rs.
DOM::Event* web_rust_toggle_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, FlatPtr old_state, FlatPtr new_state, DOM::Element* source);
}

GC::Ref<DOM::Event> create_toggle_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, Utf16FlyString const& old_state, Utf16FlyString const& new_state, GC::Ptr<DOM::Element> source)
{
    return *web_rust_toggle_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, old_state.raw_identity(), new_state.raw_identity(), source.ptr());
}

}
