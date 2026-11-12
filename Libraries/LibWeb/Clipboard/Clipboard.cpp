/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>

namespace Web::Clipboard {

struct SystemClipboardRepresentationABI {
    ReadonlyBytes name;
    ReadonlyBytes data;
};

extern "C" {
// Defined in Bindings/Rust/src/clipboard/exports.rs.
void* web_rust_clipboard_create_clipboard(JSVM*, void* realm);

// Called by Bindings/Rust/src/clipboard/clipboard.rs.
bool web_clipboard_system_clipboard_data_being_pasted(HTML::Window*, void* data, void (*on_item)(void*), void (*on_representation)(void*, ReadonlyBytes, ReadonlyBytes));
void web_clipboard_request_system_clipboard_data(HTML::Window*, GC::Cell* request, void (*steps)(JSVM*, GC::Cell*, Vector<SystemClipboardItem> const*));
void web_clipboard_for_each_system_clipboard_item(Vector<SystemClipboardItem> const*, void* data, void (*on_item)(void*), void (*on_representation)(void*, ReadonlyBytes, ReadonlyBytes));
void web_clipboard_insert_clipboard_item(HTML::Window*, ReadonlySpan<SystemClipboardRepresentationABI>, ReadonlyBytes presentation_style);
}

GC::Ref<Clipboard> Clipboard::create(JS::Realm& realm)
{
    return *static_cast<Clipboard*>(web_rust_clipboard_create_clipboard(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

void web_clipboard_for_each_system_clipboard_item(Vector<SystemClipboardItem> const* system_clipboard_data, void* data, void (*on_item)(void*), void (*on_representation)(void*, ReadonlyBytes, ReadonlyBytes))
{
    for (auto const& system_clipboard_item : *system_clipboard_data) {
        on_item(data);
        for (auto const& system_clipboard_representation : system_clipboard_item.system_clipboard_representations) {
            // AD-HOC: We do not expose file-backed clipboard entries to scripts.
            auto const* representation_data = system_clipboard_representation.data.get_pointer<ByteString>();
            if (!representation_data)
                continue;
            on_representation(data, system_clipboard_representation.name.bytes(), representation_data->bytes());
        }
    }
}

bool web_clipboard_system_clipboard_data_being_pasted(HTML::Window* window, void* data, void (*on_item)(void*), void (*on_representation)(void*, ReadonlyBytes, ReadonlyBytes))
{
    auto const* system_clipboard_data = window->system_clipboard_data_being_pasted();
    if (!system_clipboard_data)
        return false;
    web_clipboard_for_each_system_clipboard_item(system_clipboard_data, data, on_item, on_representation);
    return true;
}

void web_clipboard_request_system_clipboard_data(HTML::Window* window, GC::Cell* request, void (*steps)(JSVM*, GC::Cell*, Vector<SystemClipboardItem> const*))
{
    auto& document = window->associated_document();
    document.page().request_clipboard_entries(GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *request }, steps), [](auto& request, auto& steps, Vector<SystemClipboardItem> data) {
        steps(JS::HostABI::vm_to_abi(JS::VM::the()), request.ptr(), &data);
    }),
        document.id());
}

void web_clipboard_insert_clipboard_item(HTML::Window* window, ReadonlySpan<SystemClipboardRepresentationABI> representations, ReadonlyBytes presentation_style)
{
    Vector<SystemClipboardRepresentation> system_clipboard_representations;
    system_clipboard_representations.ensure_capacity(representations.size());
    for (auto const& representation : representations)
        system_clipboard_representations.empend(MUST(String::from_utf8(StringView { representation.name })), ByteString { representation.data });

    window->page().client().page_did_insert_clipboard_item({ move(system_clipboard_representations) }, StringView { presentation_style }, window->associated_document().id());
}

}
