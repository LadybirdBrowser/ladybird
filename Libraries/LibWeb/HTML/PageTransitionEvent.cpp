/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/PageTransitionEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/page_transition_event.rs.
DOM::Event* web_rust_page_transition_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, bool persisted);
}

GC::Ref<DOM::Event> create_page_transition_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, bool persisted)
{
    return *web_rust_page_transition_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, persisted);
}

}
