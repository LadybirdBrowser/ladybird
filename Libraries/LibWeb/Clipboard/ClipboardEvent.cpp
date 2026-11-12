/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Clipboard/ClipboardEvent.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::Clipboard {

extern "C" {
// Defined in Bindings/Rust/src/clipboard/exports.rs.
DOM::Event* web_rust_clipboard_create_clipboard_event(JSVM*, void* realm, FlatPtr event_name, bool bubbles, bool cancelable, bool composed, HTML::DataTransfer* clipboard_data);

// Called by Bindings/Rust/src/clipboard/clipboard_event.rs.
HTML::DataTransfer* web_clipboard_data_transfer_from_wrapper(JS::Object*);
JS::Object* web_clipboard_data_transfer_wrap(JS::Realm*, HTML::DataTransfer*);
}

GC::Ref<DOM::Event> create_clipboard_event(JS::Realm& realm, Utf16FlyString const& event_name, DOM::EventInit const& event_init, GC::Ptr<HTML::DataTransfer> clipboard_data)
{
    return *web_rust_clipboard_create_clipboard_event(JS::HostABI::vm_to_abi(realm.vm()), &realm, event_name.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, clipboard_data.ptr());
}

HTML::DataTransfer* web_clipboard_data_transfer_from_wrapper(JS::Object* object)
{
    return Bindings::impl_from<HTML::DataTransfer>(object);
}

JS::Object* web_clipboard_data_transfer_wrap(JS::Realm* realm, HTML::DataTransfer* data_transfer)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *data_transfer }).ptr();
}

}
