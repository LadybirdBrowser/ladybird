/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Document.h>
#include <LibWeb/HTML/VisibilityStateEntry.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/visibility_state_entry.rs.
VisibilityStateEntry* web_rust_html_create_visibility_state_entry(JSVM*, FlatPtr visibility_state, double timestamp);
}

GC::Ref<VisibilityStateEntry> VisibilityStateEntry::create(VisibilityState visibility_state, HighResolutionTime::DOMHighResTimeStamp timestamp)
{
    return *web_rust_html_create_visibility_state_entry(
        JS::HostABI::vm_to_abi(JS::VM::the()),
        Bindings::idl_enum_to_string(visibility_state).into_raw(),
        timestamp);
}

}
