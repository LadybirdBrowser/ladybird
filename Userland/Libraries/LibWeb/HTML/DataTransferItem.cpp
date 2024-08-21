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

}
