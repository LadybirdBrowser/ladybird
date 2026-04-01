/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CacheStoragePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
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

void CacheStorage::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_relevant_name_to_cache_map);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-has
GC::Ref<WebIDL::Promise> CacheStorage::has(String const& cache_name)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise, cache_name]() {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. For each key → value of the relevant name to cache map:
        //     1. If cacheName matches key, resolve promise with true and abort these steps.
        // 2. Resolve promise with false.
        WebIDL::resolve_promise(realm, promise, JS::Value { relevant_name_to_cache_map().contains(cache_name) });
    }));

    // 3. Return promise.
    return promise;
}

// https://w3c.github.io/ServiceWorker/#cache-storage-open
GC::Ref<WebIDL::Promise> CacheStorage::open(String const& cache_name)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise, cache_name]() {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        auto& relevant_name_to_cache_map = this->relevant_name_to_cache_map();

        // 1. For each key → value of the relevant name to cache map:
        //     1. If cacheName matches key, then:
        if (auto value = relevant_name_to_cache_map.get(cache_name); value.has_value()) {
            // 1. Resolve promise with a new Cache object that represents value.
            WebIDL::resolve_promise(realm, promise, realm.create<Cache>(realm, value.release_value()));

            // 2. Abort these steps.
            return;
        }

        // 2. Let cache be a new request response list.
        auto cache = realm.heap().allocate<RequestResponseList>();

        // 3. Set the relevant name to cache map[cacheName] to cache. If this cache write operation failed due to
        //    exceeding the granted quota limit, reject promise with a QuotaExceededError and abort these steps.
        // FIXME: Handle cache quotas.
        relevant_name_to_cache_map.set(cache_name, cache);

        // 4. Resolve promise with a new Cache object that represents cache.
        WebIDL::resolve_promise(realm, promise, realm.create<Cache>(realm, cache));
    }));

    // 3. Return promise.
    return promise;
}

// https://w3c.github.io/ServiceWorker/#relevant-name-to-cache-map
NameToCacheMap& CacheStorage::relevant_name_to_cache_map()
{
    // The relevant name to cache map for a CacheStorage object is the name to cache map associated with the result of
    // running obtain a local storage bottle map with the object’s relevant settings object and "caches".

    // FIXME: We don't yet have a way to serialize request/response pairs for storage in the UI process. For now, we
    //        use an ephemeral name to cache map.
    return m_relevant_name_to_cache_map;
}

}
