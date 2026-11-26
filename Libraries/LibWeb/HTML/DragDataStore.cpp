/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

struct DragDataStoreItemView {
    FlatPtr type_string;
    FlatPtr data;
    bool is_file;
};

struct NativeFileBytes {
    ByteBuffer* owner;
    u8 const* data;
    size_t length;
};

extern "C" {

// Defined in Bindings/Rust/src/html/drag_data_store.rs.
RustDragDataStore* web_rust_drag_data_store_create();
void web_rust_drag_data_store_unref(RustDragDataStore*);
void web_rust_drag_data_store_set_mode(RustDragDataStore const*, DragDataStore::Mode);
size_t web_rust_drag_data_store_size(RustDragDataStore const*);
bool web_rust_drag_data_store_has_text_item(RustDragDataStore const*);
DragDataStoreItemView web_rust_drag_data_store_item(RustDragDataStore const*, size_t index);
void web_rust_drag_data_store_file_data_as_base64(RustDragDataStore const*, size_t index, ErrorOr<String>*);
void web_rust_drag_data_store_add_text(RustDragDataStore const*, FlatPtr type_string, FlatPtr data);
void web_rust_drag_data_store_add_file(RustDragDataStore const*, FlatPtr type_string, NativeFileBytes data, FlatPtr name);

JSObject* web_drag_data_store_create_file_entry(FlatPtr file_name, JS::Realm*);
void web_drag_data_store_delete_byte_buffer(ByteBuffer*);
void web_drag_data_store_encode_file_data(ReadonlyBytes, ErrorOr<String>*);
}

NonnullRefPtr<DragDataStore> DragDataStore::create()
{
    return adopt_ref(*new DragDataStore(web_rust_drag_data_store_create()));
}

DragDataStore::DragDataStore(RustDragDataStore* store)
    : m_store(store)
{
}

DragDataStore::~DragDataStore()
{
    web_rust_drag_data_store_unref(m_store);
}

size_t DragDataStore::size() const
{
    return web_rust_drag_data_store_size(m_store);
}

bool DragDataStore::has_text_item() const
{
    return web_rust_drag_data_store_has_text_item(m_store);
}

void DragDataStore::set_mode(Mode mode)
{
    // These values match Mode in Bindings/Rust/src/html/drag_data_store.rs.
    static_assert(to_underlying(Mode::ReadWrite) == 0);
    static_assert(to_underlying(Mode::ReadOnly) == 1);
    static_assert(to_underlying(Mode::Protected) == 2);
    web_rust_drag_data_store_set_mode(m_store, mode);
}

void DragDataStore::add_text(Utf16FlyString const& type_string, Utf16String const& data)
{
    web_rust_drag_data_store_add_text(m_store, type_string.raw_identity(), data.raw_identity());
}

void DragDataStore::add_file(Utf16FlyString const& type_string, ByteBuffer data, Utf16String const& name)
{
    auto* owner = new ByteBuffer(move(data));
    auto bytes = owner->bytes();
    web_rust_drag_data_store_add_file(m_store, type_string.raw_identity(), { owner, bytes.data(), bytes.size() }, name.raw_identity());
}

DragDataStore::Item DragDataStore::item_at(size_t index) const
{
    auto item = web_rust_drag_data_store_item(m_store, index);
    return {
        Utf16FlyString::adopt_raw(item.type_string),
        Utf16String::adopt_raw(item.data),
        item.is_file,
    };
}

ErrorOr<String> DragDataStore::file_data_as_base64(size_t index) const
{
    ErrorOr<String> result { String {} };
    web_rust_drag_data_store_file_data_as_base64(m_store, index, &result);
    return result;
}

void web_drag_data_store_delete_byte_buffer(ByteBuffer* owner)
{
    delete owner;
}

void web_drag_data_store_encode_file_data(ReadonlyBytes bytes, ErrorOr<String>* result)
{
    *result = AK::encode_base64(bytes);
}

JSObject* web_drag_data_store_create_file_entry(FlatPtr file_name, JS::Realm* realm)
{
    auto name = Utf16String::adopt_raw(file_name);
    auto entry = EntriesAPI::FileSystemEntry::create(EntriesAPI::EntryType::File, name.to_utf8().to_byte_string());
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, entry).ptr());
}

}
