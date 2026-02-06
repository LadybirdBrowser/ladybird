/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StorageManagerPrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageManager.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageManager);

WebIDL::ExceptionOr<GC::Ref<StorageManager>> StorageManager::create(JS::Realm& realm)
{
    return realm.create<StorageManager>(realm);
}

StorageManager::StorageManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void StorageManager::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StorageManager);
    Base::initialize(realm);
}

// https://storage.spec.whatwg.org/#dom-storagemanager-estimate
GC::Ref<WebIDL::Promise> StorageManager::estimate()
{
    auto& realm = this->realm();

    // TODO: Calculate this from actual per-origin persisted data.
    constexpr WebIDL::UnsignedLongLong usage = 0;
    constexpr WebIDL::UnsignedLongLong quota = 1ULL * 1024 * 1024 * 1024;

    auto estimate = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(estimate->create_data_property("usage"_utf16_fly_string, JS::Value(usage)));
    MUST(estimate->create_data_property("quota"_utf16_fly_string, JS::Value(quota)));

    return WebIDL::create_resolved_promise(realm, estimate);
}

}
