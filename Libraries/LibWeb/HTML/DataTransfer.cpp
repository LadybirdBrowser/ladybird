/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::HTML {

struct DragDataStoreItemView {
    FlatPtr type_string;
    FlatPtr data;
    bool is_file;
};

extern "C" {

// Defined in Bindings/Rust/src/html/data_transfer.rs.
DataTransfer* web_rust_data_transfer_create(JSVM*, DragDataStore*);
void web_rust_data_transfer_disassociate(DataTransfer*);
FlatPtr web_rust_data_transfer_drop_effect(DataTransfer const*);
FlatPtr web_rust_data_transfer_effect_allowed(DataTransfer const*);
void web_rust_data_transfer_set_drop_effect(DataTransfer*, FlatPtr);
void web_rust_data_transfer_set_effect_allowed_internal(DataTransfer*, FlatPtr);

DragDataStore* web_data_transfer_create_writable_store();
void web_data_transfer_store_unref(DragDataStore*);
u8 web_data_transfer_store_mode(DragDataStore const*);
size_t web_data_transfer_store_size(DragDataStore const*);
DragDataStoreItemView web_data_transfer_store_item(DragDataStore const*, size_t index);
void web_data_transfer_store_add_text(DragDataStore*, FlatPtr type, FlatPtr data);
void web_data_transfer_store_remove_item(DragDataStore*, size_t index);
void web_data_transfer_store_add_file(DragDataStore*, FileAPI::File const*);
FileAPI::File* web_data_transfer_create_file(DragDataStore const*, size_t index);
JS::Object* web_data_transfer_create_file_entry(DragDataStore const*, size_t index, JS::Realm*);
}

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

GC::Ref<DataTransfer> DataTransfer::create(NonnullRefPtr<DragDataStore> store)
{
    return *web_rust_data_transfer_create(JS::HostABI::vm_to_abi(JS::VM::the()), &store.leak_ref());
}

Utf16FlyString DataTransfer::drop_effect() const
{
    return Utf16FlyString::adopt_raw(web_rust_data_transfer_drop_effect(this));
}

void DataTransfer::set_drop_effect(Utf16FlyString const& effect)
{
    web_rust_data_transfer_set_drop_effect(this, effect.raw_identity());
}

Utf16FlyString DataTransfer::effect_allowed() const
{
    return Utf16FlyString::adopt_raw(web_rust_data_transfer_effect_allowed(this));
}

void DataTransfer::set_effect_allowed_internal(Utf16FlyString const& effect)
{
    web_rust_data_transfer_set_effect_allowed_internal(this, effect.raw_identity());
}

void DataTransfer::disassociate_with_drag_data_store()
{
    web_rust_data_transfer_disassociate(this);
}

DragDataStore* web_data_transfer_create_writable_store()
{
    auto store = DragDataStore::create();
    store->set_mode(DragDataStore::Mode::ReadWrite);
    return &store.leak_ref();
}

void web_data_transfer_store_unref(DragDataStore* store)
{
    store->unref();
}

u8 web_data_transfer_store_mode(DragDataStore const* store)
{
    // These values match the mode queries in Bindings/Rust/src/html/data_transfer.rs.
    static_assert(to_underlying(DragDataStore::Mode::ReadWrite) == 0);
    static_assert(to_underlying(DragDataStore::Mode::ReadOnly) == 1);
    static_assert(to_underlying(DragDataStore::Mode::Protected) == 2);
    return to_underlying(store->mode());
}

size_t web_data_transfer_store_size(DragDataStore const* store)
{
    return store->size();
}

DragDataStoreItemView web_data_transfer_store_item(DragDataStore const* store, size_t index)
{
    auto const& item = store->item_list()[index];
    return { item.type_string.raw_identity(), item.data.raw_identity(), item.kind == DragDataStoreItem::Kind::File };
}

void web_data_transfer_store_add_text(DragDataStore* store, FlatPtr type, FlatPtr data)
{
    store->add_item({
        .kind = DragDataStoreItem::Kind::Text,
        .type_string = Utf16FlyString::from_raw(type),
        .data = Utf16String::from_raw(data),
        .file_data = {},
        .file_name = {},
    });
}

void web_data_transfer_store_remove_item(DragDataStore* store, size_t index)
{
    store->remove_item_at(index);
}

void web_data_transfer_store_add_file(DragDataStore* store, FileAPI::File const* file)
{
    store->add_item({
        .kind = DragDataStoreItem::Kind::File,
        .type_string = file->type().to_ascii_lowercase(),
        .data = {},
        .file_data = MUST(ByteBuffer::copy(file->raw_bytes())),
        .file_name = file->name(),
    });
}

FileAPI::File* web_data_transfer_create_file(DragDataStore const* store, size_t index)
{
    auto const& item = store->item_list()[index];
    VERIFY(item.kind == DragDataStoreItem::Kind::File);
    // FIXME: Fill in other fields (e.g. last_modified).
    return FileAPI::File::create(item.file_data, item.file_name, item.type_string.to_utf16_string()).ptr();
}

JS::Object* web_data_transfer_create_file_entry(DragDataStore const* store, size_t index, JS::Realm* realm)
{
    auto const& item = store->item_list()[index];
    VERIFY(item.kind == DragDataStoreItem::Kind::File);
    auto entry = EntriesAPI::FileSystemEntry::create(EntriesAPI::EntryType::File, item.file_name.to_utf8().to_byte_string());
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, entry).ptr();
}

}
