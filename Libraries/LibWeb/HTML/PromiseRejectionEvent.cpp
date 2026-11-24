/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/promise_rejection_event.rs.
DOM::Event* web_rust_promise_rejection_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, JSObject* promise, JSValue reason);
}

GC::Ref<DOM::Event> create_promise_rejection_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, JS::Object& promise, JS::Value reason)
{
    return *web_rust_promise_rejection_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, JS::HostABI::object_to_abi(&promise), JS::HostABI::value_to_abi(reason));
}

}
