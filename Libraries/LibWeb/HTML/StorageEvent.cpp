/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/StorageEvent.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/exports.rs.
DOM::Event* web_rust_html_create_storage_event(JSVM*, void* realm);
}

GC::Ref<DOM::Event> create_storage_event(JS::Realm& realm)
{
    return *web_rust_html_create_storage_event(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

}
