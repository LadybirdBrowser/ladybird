/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/Window.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_rust_html_create_storage(JSVM*, void* realm, u8 type, StorageAPI::StorageBottle* map);
StorageAPI::StorageBottle* web_rust_html_storage_map(Storage*);
void web_rust_html_dispatch_storage_events_for_remote_change(JSVM*, u8 type, URL::FFI::FfiOrigin const* origin, bool has_session_page, u64 session_page, bool has_key, FlatPtr key, bool has_old_value, FlatPtr old_value, bool has_new_value, FlatPtr new_value, ReadonlyBytes url);

// Declared on the Rust side in Bindings/Rust/src/html/storage.rs.
Storage* web_html_window_storage(Window*, u8 type);
}

GC::Ref<Storage> Storage::create(Window& window, Type type, GC::Ref<StorageAPI::StorageBottle> storage_bottle)
{
    auto& realm = relevant_realm(window);
    return *static_cast<Storage*>(web_rust_html_create_storage(JS::HostABI::vm_to_abi(realm.vm()), &realm, to_underlying(type), storage_bottle.ptr()));
}

StorageAPI::StorageBottle& Storage::map()
{
    return *web_rust_html_storage_map(this);
}

void Storage::dump()
{
    auto keys = map().keys();
    dbgln("Storage ({} key(s))", keys.size());
    size_t i = 0;
    for (auto const& key : keys) {
        auto value = map().get(key);
        dbgln("[{}] \"{}\": \"{}\"", i, key, value.value());
        ++i;
    }
}

void Storage::dispatch_storage_events_for_remote_change(Type type, StorageAPI::StorageKey const& storage_key, Optional<PageId> session_page, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url)
{
    auto raw = [](Optional<Utf16String> const& string) -> FlatPtr {
        if (!string.has_value())
            return 0;
        return Utf16String { *string }.into_raw();
    };
    URL::RustIntegration::lend_origin_to_ffi(storage_key.origin, [&](URL::FFI::FfiOrigin const& origin) {
        web_rust_html_dispatch_storage_events_for_remote_change(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), to_underlying(type), &origin, session_page.has_value(), session_page.has_value() ? session_page->value() : 0, key.has_value(), raw(key), old_value.has_value(), raw(old_value), new_value.has_value(), raw(new_value), url.bytes());
    });
}

Storage* web_html_window_storage(Window* window, u8 type)
{
    auto storage = static_cast<Storage::Type>(type) == Storage::Type::Local ? window->local_storage() : window->session_storage();
    if (storage.is_exception())
        return nullptr;
    return storage.release_value().ptr();
}

}
