/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferItemPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransferItem.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DataTransferItem);

JS::NonnullGCPtr<DataTransferItem> DataTransferItem::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<DataTransferItem>(realm, realm);
}

DataTransferItem::DataTransferItem(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DataTransferItem::~DataTransferItem() = default;

void DataTransferItem::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransferItem);
}

}
