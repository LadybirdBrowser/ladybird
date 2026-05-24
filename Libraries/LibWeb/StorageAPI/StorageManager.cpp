/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StorageManager.h>
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
    auto promise = WebIDL::create_promise(realm);

    // Return a StorageEstimate dictionary with stub values. reCAPTCHA and other scripts
    // call navigator.storage.estimate() during fingerprinting and break if it rejects
    // or is unavailable. We return a generic quota/usage pair.
    auto estimate = JS::Object::create(realm, realm.intrinsics().object_prototype());
    estimate->define_direct_property("usage"_utf16, JS::Value(0.0), JS::default_attributes);
    estimate->define_direct_property("quota"_utf16, JS::Value(10'000'000'000.0), JS::default_attributes);

    WebIDL::resolve_promise(realm, *promise, estimate);
    return *promise;
}

}
