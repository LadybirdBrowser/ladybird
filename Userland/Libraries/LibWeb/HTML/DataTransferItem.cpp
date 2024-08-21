/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferItemPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DataTransferItem.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DataTransferItem);

JS::NonnullGCPtr<DataTransferItem> DataTransferItem::create(JS::Realm& realm, JS::NonnullGCPtr<DataTransfer> data_transfer, size_t item_index)
{
    return realm.heap().allocate<DataTransferItem>(realm, realm, data_transfer, item_index);
}

DataTransferItem::DataTransferItem(JS::Realm& realm, JS::NonnullGCPtr<DataTransfer> data_transfer, size_t item_index)
    : PlatformObject(realm)
    , m_data_transfer(data_transfer)
    , m_item_index(item_index)
{
}

DataTransferItem::~DataTransferItem() = default;

void DataTransferItem::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransferItem);
}

void DataTransferItem::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data_transfer);
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-kind
String DataTransferItem::kind() const
{
    // The kind attribute must return the empty string if the DataTransferItem object is in the disabled mode; otherwise
    // it must return the string given in the cell from the second column of the following table from the row whose cell
    // in the first column contains the drag data item kind of the item represented by the DataTransferItem object:
    //
    //     Kind | String
    //     ---------------
    //     Text | "string"
    //     File | "file"
    if (!mode().has_value())
        return {};

    auto const& item = m_data_transfer->drag_data(*m_item_index);

    switch (item.kind) {
    case DragDataStoreItem::Kind::Text:
        return "string"_string;
    case DragDataStoreItem::Kind::File:
        return "file"_string;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransferitem-type
String DataTransferItem::type() const
{
    // The type attribute must return the empty string if the DataTransferItem object is in the disabled mode; otherwise
    // it must return the drag data item type string of the item represented by the DataTransferItem object.
    if (!mode().has_value())
        return {};

    auto const& item = m_data_transfer->drag_data(*m_item_index);
    return item.type_string;
}

Optional<DragDataStore::Mode> DataTransferItem::mode() const
{
    if (!m_item_index.has_value())
        return {};
    return m_data_transfer->mode();
}

}
