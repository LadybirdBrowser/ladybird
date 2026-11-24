/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/SubmitEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/submit_event.rs.
DOM::Event* web_rust_submit_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, HTMLElement* submitter);
}

GC::Ref<DOM::Event> create_submit_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, GC::Ptr<HTMLElement> submitter)
{
    return *web_rust_submit_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, submitter.ptr());
}

}
