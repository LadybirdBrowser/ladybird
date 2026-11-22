/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Clipboard/ClipboardEvent.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::Clipboard {

extern "C" {
// Defined in Bindings/Rust/src/clipboard/exports.rs.
DOM::Event* web_rust_clipboard_create_clipboard_event(JSVM*, void* realm, FlatPtr event_name, bool bubbles, bool cancelable, bool composed, HTML::DataTransfer* clipboard_data);
}

GC::Ref<DOM::Event> create_clipboard_event(JS::Realm& realm, Utf16FlyString const& event_name, DOM::EventInit const& event_init, GC::Ptr<HTML::DataTransfer> clipboard_data)
{
    return *web_rust_clipboard_create_clipboard_event(JS::HostABI::vm_to_abi(realm.vm()), &realm, event_name.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, clipboard_data.ptr());
}

}
