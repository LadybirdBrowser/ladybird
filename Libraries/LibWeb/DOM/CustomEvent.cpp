/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/CustomEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::DOM {

extern "C" {
// Defined in Bindings/Rust/src/dom/custom_event.rs.
Event* web_rust_custom_event_create(JSVM*, void* realm, FlatPtr type, JSValue detail);
Event* web_rust_custom_event_create_for_create_event(JSVM*, void* realm);
}

GC::Ref<Event> create_custom_event(JS::Realm& realm, Utf16FlyString const& type, JS::Value detail)
{
    return *web_rust_custom_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), JS::HostABI::value_to_abi(detail));
}

GC::Ref<Event> create_custom_event(JS::Realm& realm)
{
    return *web_rust_custom_event_create_for_create_event(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

}
