/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DataTransferPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DataTransfer);

GC::Ref<DataTransfer> DataTransfer::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<DataTransfer>(realm, realm);
}

DataTransfer::DataTransfer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DataTransfer::~DataTransfer() = default;

void DataTransfer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DataTransfer);
}

}
