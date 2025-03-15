/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LockManager.h"

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebLocks/Lock.h>

#include <utility>

namespace Web::WebLocks {

GC_DEFINE_ALLOCATOR(LockManager);

WebIDL::ExceptionOr<GC::Ref<LockManager>> LockManager::construct_impl(JS::Realm& realm)
{
    return realm.create<LockManager>(realm);
}

LockManager::LockManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void LockManager::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(LockManager);
}

GC::Ref<WebIDL::Promise> LockManager::request(String name, GC::Ref<WebIDL::CallbackType> callback) const
{
    LockOptions const defaultOptions = {
        .mode = Bindings::LockMode::Exclusive,
        .if_available = false,
        .steal = false,
        .signal = nullptr
    };
    return request(std::move(name), defaultOptions, callback);
}

GC::Ref<WebIDL::Promise> LockManager::request(String name, LockOptions options, GC::Ref<WebIDL::CallbackType> callback) const
{
    auto& realm = this->realm();

    auto const promise = WebIDL::create_promise(realm);

    // Validate options
    if (options.steal && options.if_available) {
        reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Cannot use both 'steal' and 'ifAvailable' together."_string));
        return promise;
    }
    if (options.steal && options.mode == Bindings::LockMode::Shared) {
        reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "The 'steal' option requires 'mode' to be \"exclusive\"."_string));
        return promise;
    }
    if (options.steal && options.signal) {
        reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Cannot use both 'steal' and 'signal' together."_string));
        return promise;
    }
    if (options.if_available && options.signal) {
        reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Cannot use both 'ifAvailable' and 'signal' together."_string));
        return promise;
    }

    auto const lock = realm.create<Lock>(realm, name, options.mode);

    // FIXME: This should only be invoked when the lock has been acquired.
    auto callback_result = invoke_callback(*callback, {}, WebIDL::ExceptionBehavior::Rethrow, lock);

    if (callback_result.is_error()) {
        WebIDL::reject_promise(realm, promise, callback_result.release_value().value());
    } else {
        WebIDL::resolve_promise(realm, promise, callback_result.release_value().value());
    }

    return promise;
}

}
