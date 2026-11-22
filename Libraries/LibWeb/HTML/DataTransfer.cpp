/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DataTransferItem.h>
#include <LibWeb/HTML/DataTransferItemList.h>

namespace Web::HTML {

struct DragDataStoreItemView {
    FlatPtr type_string;
    FlatPtr data;
    bool is_file;
};

extern "C" {

// Defined in Bindings/Rust/src/html/data_transfer.rs.
DataTransfer* web_rust_data_transfer_create(JSVM*, DragDataStore*);
DragDataStore* web_rust_data_transfer_drag_data_store(DataTransfer const*);
void web_rust_data_transfer_disassociate(DataTransfer*);
FlatPtr web_rust_data_transfer_drop_effect(DataTransfer const*);
FlatPtr web_rust_data_transfer_effect_allowed(DataTransfer const*);
void web_rust_data_transfer_set_drop_effect(DataTransfer*, FlatPtr);
void web_rust_data_transfer_set_effect_allowed_internal(DataTransfer*, FlatPtr);
DataTransferItem* web_rust_data_transfer_did_add_item(JSVM*, DataTransfer*);
void web_rust_data_transfer_remove_item(JSVM*, DataTransfer*, size_t index);
DataTransferItem* web_rust_data_transfer_item(DataTransfer const*, size_t index);

DragDataStore* web_data_transfer_create_writable_store();
void web_data_transfer_store_unref(DragDataStore*);
u8 web_data_transfer_store_mode(DragDataStore const*);
size_t web_data_transfer_store_size(DragDataStore const*);
DragDataStoreItemView web_data_transfer_store_item(DragDataStore const*, size_t index);
void web_data_transfer_store_add_text(DragDataStore*, FlatPtr type, FlatPtr data);
void web_data_transfer_store_remove_item(DragDataStore*, size_t index);
DataTransferItem* web_data_transfer_create_item(DataTransfer*, size_t index);
void web_data_transfer_set_item_index(DataTransferItem*, size_t index, bool has_index);
DataTransferItemList* web_data_transfer_create_item_list(DataTransfer*);
JS::Object* web_data_transfer_wrap_item_list(JS::Realm*, DataTransferItemList*);
FileAPI::File* web_data_transfer_create_file(DragDataStore const*, size_t index);
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

DragDataStore* DataTransfer::drag_data_store() const
{
    return web_rust_data_transfer_drag_data_store(this);
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

Optional<DragDataStore::Mode> DataTransfer::mode() const
{
    if (auto* store = drag_data_store())
        return store->mode();
    return {};
}

void DataTransfer::disassociate_with_drag_data_store()
{
    web_rust_data_transfer_disassociate(this);
}

GC::Ref<DataTransferItem> DataTransfer::add_item(DragDataStoreItem item)
{
    auto* store = drag_data_store();
    VERIFY(store);
    store->add_item(move(item));
    return *web_rust_data_transfer_did_add_item(JS::HostABI::vm_to_abi(JS::VM::the()), this);
}

void DataTransfer::remove_item(size_t index)
{
    web_rust_data_transfer_remove_item(JS::HostABI::vm_to_abi(JS::VM::the()), this, index);
}

void DataTransfer::set_item_index(DataTransferItem& item, Optional<size_t> index)
{
    item.set_item_index({}, index);
}

bool DataTransfer::contains_item_with_type(DragDataStoreItem::Kind kind, Utf16View type) const
{
    auto* store = drag_data_store();
    VERIFY(store);
    for (auto const& item : store->item_list()) {
        if (item.kind == kind && item.type_string.equals_ignoring_ascii_case(type))
            return true;
    }
    return false;
}

GC::Ref<DataTransferItem> DataTransfer::item(size_t index) const
{
    return *web_rust_data_transfer_item(this, index);
}

DragDataStoreItem const& DataTransfer::drag_data(size_t index) const
{
    auto* store = drag_data_store();
    VERIFY(store);
    return store->item_list()[index];
}

size_t DataTransfer::length() const
{
    auto* store = drag_data_store();
    return store ? store->size() : 0;
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

DataTransferItem* web_data_transfer_create_item(DataTransfer* transfer, size_t index)
{
    return DataTransferItem::create(*transfer, index).ptr();
}

void web_data_transfer_set_item_index(DataTransferItem* item, size_t index, bool has_index)
{
    DataTransfer::set_item_index(*item, has_index ? Optional<size_t> { index } : Optional<size_t> {});
}

DataTransferItemList* web_data_transfer_create_item_list(DataTransfer* transfer)
{
    return DataTransferItemList::create(*transfer).ptr();
}

JS::Object* web_data_transfer_wrap_item_list(JS::Realm* realm, DataTransferItemList* items)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *items }).ptr();
}

FileAPI::File* web_data_transfer_create_file(DragDataStore const* store, size_t index)
{
    auto const& item = store->item_list()[index];
    VERIFY(item.kind == DragDataStoreItem::Kind::File);
    // FIXME: Fill in other fields (e.g. last_modified).
    return FileAPI::File::create(item.file_data, item.file_name, item.type_string.to_utf16_string()).ptr();
}

}
