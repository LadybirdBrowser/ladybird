/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HashChangeEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/hash_change_event.rs.
DOM::Event* web_rust_hash_change_event_create(JSVM*, void* realm, FlatPtr type, FlatPtr old_url, FlatPtr new_url);
DOM::Event* web_rust_hash_change_event_create_for_create_event(JSVM*, void* realm);
}

GC::Ref<DOM::Event> create_hash_change_event(JS::Realm& realm, Utf16FlyString const& type, String const& old_url, String const& new_url)
{
    auto old_url_string = Utf16String::from_utf8(old_url);
    auto new_url_string = Utf16String::from_utf8(new_url);
    return *web_rust_hash_change_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), old_url_string.raw_identity(), new_url_string.raw_identity());
}

GC::Ref<DOM::Event> create_hash_change_event(JS::Realm& realm)
{
    return *web_rust_hash_change_event_create_for_create_event(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

}
