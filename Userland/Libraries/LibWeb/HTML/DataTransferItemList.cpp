/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferItemListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransferItemList.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DataTransferItemList);

DataTransferItemList::DataTransferItemList(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DataTransferItemList::~DataTransferItemList() = default;

void DataTransferItemList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransferItemList);
}

}
