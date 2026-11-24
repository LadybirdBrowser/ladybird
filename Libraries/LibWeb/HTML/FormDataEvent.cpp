/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/FormDataEvent.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/form_data_event.rs.
DOM::Event* web_rust_form_data_event_create(JSVM*, void* realm, FlatPtr type, bool bubbles, bool cancelable, bool composed, XHR::FormData* form_data);
}

GC::Ref<DOM::Event> create_form_data_event(JS::Realm& realm, Utf16FlyString const& type, DOM::EventInit const& event_init, XHR::FormData& form_data)
{
    return *web_rust_form_data_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, &form_data);
}

}
