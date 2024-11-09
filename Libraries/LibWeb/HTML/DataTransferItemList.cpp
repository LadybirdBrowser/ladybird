/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferItemListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DataTransferItem.h>
#include <LibWeb/HTML/DataTransferItemList.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DataTransferItemList);

JS::NonnullGCPtr<DataTransferItemList> DataTransferItemList::create(JS::Realm& realm, JS::NonnullGCPtr<DataTransfer> data_transfer)
{
    return realm.heap().allocate<DataTransferItemList>(realm, realm, data_transfer);
}

DataTransferItemList::DataTransferItemList(JS::Realm& realm, JS::NonnullGCPtr<DataTransfer> data_transfer)
    : PlatformObject(realm)
    , m_data_transfer(data_transfer)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

DataTransferItemList::~DataTransferItemList() = default;

void DataTransferItemList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransferItemList);
}

void DataTransferItemList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data_transfer);
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-length
WebIDL::UnsignedLong DataTransferItemList::length() const
{
    // The length attribute must return zero if the object is in the disabled mode; otherwise it must return the number
    // of items in the drag data store item list.
    return m_data_transfer->length();
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-add
WebIDL::ExceptionOr<JS::GCPtr<DataTransferItem>> DataTransferItemList::add(String const& data, String const& type)
{
    auto& realm = this->realm();

    // 1. If the DataTransferItemList object is not in the read/write mode, return null.
    if (m_data_transfer->mode() != DragDataStore::Mode::ReadWrite)
        return nullptr;

    // 2. Jump to the appropriate set of steps from the following list:
    //    -> If the first argument to the method is a string

    // If there is already an item in the drag data store item list whose kind is text and whose type string is equal
    // to the value of the method's second argument, converted to ASCII lowercase, then throw a "NotSupportedError"
    // DOMException.
    if (m_data_transfer->contains_item_with_type(DragDataStoreItem::Kind::Text, type)) {
        auto error = MUST(String::formatted("There is already a DataTransferItem with type {}", type));
        return WebIDL::NotSupportedError::create(realm, error);
    }

    // Otherwise, add an item to the drag data store item list whose kind is text, whose type string is equal to the
    // value of the method's second argument, converted to ASCII lowercase, and whose data is the string given by the
    // method's first argument.
    auto item = m_data_transfer->add_item({
        .kind = HTML::DragDataStoreItem::Kind::Text,
        .type_string = type.to_ascii_lowercase(),
        .data = MUST(ByteBuffer::copy(data.bytes())),
        .file_name = {},
    });

    // 3. Determine the value of the indexed property corresponding to the newly added item, and return that value (a
    //    newly created DataTransferItem object).
    return item;
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-add
JS::GCPtr<DataTransferItem> DataTransferItemList::add(JS::NonnullGCPtr<FileAPI::File> file)
{
    // 1. If the DataTransferItemList object is not in the read/write mode, return null.
    if (m_data_transfer->mode() != DragDataStore::Mode::ReadWrite)
        return nullptr;

    // 2. Jump to the appropriate set of steps from the following list:
    //     -> If the first argument to the method is a File

    // Add an item to the drag data store item list whose kind is File, whose type string is the type of the File,
    // converted to ASCII lowercase, and whose data is the same as the File's data.
    auto item = m_data_transfer->add_item({
        .kind = HTML::DragDataStoreItem::Kind::File,
        .type_string = file->type().to_ascii_lowercase(),
        .data = MUST(ByteBuffer::copy(file->raw_bytes())),
        .file_name = file->name().to_byte_string(),
    });

    // 3. Determine the value of the indexed property corresponding to the newly added item, and return that value (a
    //    newly created DataTransferItem object).
    return item;
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitemlist-item
Optional<JS::Value> DataTransferItemList::item_value(size_t index) const
{
    // To determine the value of an indexed property i of a DataTransferItemList object, the user agent must return a
    // DataTransferItem object representing the ith item in the drag data store. The same object must be returned each
    // time a particular item is obtained from this DataTransferItemList object. The DataTransferItem object must be
    // associated with the same DataTransfer object as the DataTransferItemList object when it is first created.
    if (index < m_data_transfer->length())
        return m_data_transfer->item(index);
    return {};
}

}
