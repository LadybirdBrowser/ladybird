/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_rust_html_create_storage(JSVM*, void* realm, u8 type);
void web_rust_html_storage_connect(Storage*);
void web_rust_html_storage_for_each_item(Storage*, void* ctx, void (*on_item)(void*, FlatPtr key, FlatPtr value));

// Declared on the Rust side in Bindings/Rust/src/html/storage.rs.
Storage* web_html_window_storage(Window*, u8 type);
}

GC::Ptr<Storage> Storage::create(Window& window, Type type)
{
    auto& realm = relevant_realm(window);
    return static_cast<Storage*>(web_rust_html_create_storage(JS::HostABI::vm_to_abi(realm.vm()), &realm, to_underlying(type)));
}

void Storage::connect()
{
    web_rust_html_storage_connect(this);
}

void Storage::for_each_item(Function<void(Utf16String const& key, Utf16String const& value)> const& callback)
{
    web_rust_html_storage_for_each_item(this, const_cast<void*>(static_cast<void const*>(&callback)), [](void* ctx, FlatPtr key, FlatPtr value) {
        (*static_cast<Function<void(Utf16String const&, Utf16String const&)> const*>(ctx))(Utf16String::from_raw(key), Utf16String::from_raw(value));
    });
}

void Storage::dump()
{
    Vector<String> lines;
    for_each_item([&](auto const& key, auto const& value) {
        lines.append(MUST(String::formatted("[{}] \"{}\": \"{}\"", lines.size(), key, value)));
    });
    dbgln("Storage ({} key(s))", lines.size());
    for (auto const& line : lines)
        dbgln("{}", line);
}

Storage* web_html_window_storage(Window* window, u8 type)
{
    auto storage = static_cast<Storage::Type>(type) == Storage::Type::Local ? window->local_storage() : window->session_storage();
    if (storage.is_exception())
        return nullptr;
    return storage.release_value().ptr();
}

}
