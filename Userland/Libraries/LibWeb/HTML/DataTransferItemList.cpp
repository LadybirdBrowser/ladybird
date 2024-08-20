/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferItemListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DataTransferItemList.h>

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

}
