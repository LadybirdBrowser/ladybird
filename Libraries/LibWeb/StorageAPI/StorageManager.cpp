/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StorageManagerPrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

JS_DEFINE_ALLOCATOR(StorageManager);

WebIDL::ExceptionOr<JS::NonnullGCPtr<StorageManager>> StorageManager::create(JS::Realm& realm)
{
    return realm.heap().allocate<StorageManager>(realm, realm);
}

StorageManager::StorageManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void StorageManager::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StorageManager);
}

}
