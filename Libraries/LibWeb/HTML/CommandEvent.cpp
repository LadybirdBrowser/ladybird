/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CommandEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/command_event.rs.
DOM::Event* web_rust_command_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, DOM::Element* source, FlatPtr command);
}

GC::Ref<DOM::Event> create_command_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, DOM::Element& source, Utf16String const& command)
{
    return *web_rust_command_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, &source, command.raw_identity());
}

}
