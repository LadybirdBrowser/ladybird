/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CacheStoragePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ServiceWorker/CacheStorage.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(CacheStorage);

CacheStorage::CacheStorage(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void CacheStorage::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CacheStorage);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-open
GC::Ref<WebIDL::Promise> CacheStorage::open(String const&)
{
    return WebIDL::create_rejected_promise(realm(), WebIDL::NotSupportedError::create(realm(), "CacheStorage.open() is not yet implemented"_string));
}

}
