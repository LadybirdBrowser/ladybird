/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/CacheStoragePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
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

// https://w3c.github.io/ServiceWorker/#cache-storage-match
GC::Ref<WebIDL::Promise> CacheStorage::match(Fetch::RequestInfo request, MultiCacheQueryOptions options)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. If options["cacheName"] exists, then:
    if (options.cache_name.has_value()) {
        // 1. Return a new promise promise and run the following substeps in parallel:
        auto promise = WebIDL::create_promise(realm);

        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise, request = move(request), options = move(options)]() mutable {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. For each cacheName → cache of the relevant name to cache map:
            //     1. If options["cacheName"] matches cacheName, then:
            if (auto result = relevant_name_to_cache_map().get(*options.cache_name); result.has_value()) {
                // 1. Resolve promise with the result of running the algorithm specified in match(request, options)
                //    method of Cache interface with request and options (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = realm.create<Cache>(realm, result.release_value());
                auto match = cache->match(move(request), move(options));
                WebIDL::resolve_promise(realm, promise, match->promise());

                // 2. Abort these steps.
                return;
            }

            // 2. Resolve promise with undefined.
            WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        }));

        return promise;
    }
    // 2. Else:
    else {
        // 1. Let promise be a promise resolved with undefined.
        auto promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // 2. For each cacheName → cache of the relevant name to cache map:
        for (auto const& [cache_name, cache_list] : relevant_name_to_cache_map()) {
            // 1. Set promise to the result of reacting to itself with a fulfillment handler that, when called with
            //    argument response, performs the following substeps:
            promise = WebIDL::upon_fulfillment(promise, GC::create_function(realm.heap(), [&realm, cache_list, request, options](JS::Value response) mutable -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If response is not undefined, return response.
                if (!response.is_undefined())
                    return response;

                // 2. Return the result of running the algorithm specified in match(request, options) method of Cache
                //    interface with request and options as the arguments (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = realm.create<Cache>(realm, cache_list);
                auto match = cache->match(move(request), move(options));
                return match->promise();
            }));
        }

        // 3. Return promise.
        return *promise;
    }
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

// https://w3c.github.io/ServiceWorker/#cache-storage-open
GC::Ref<WebIDL::Promise> CacheStorage::delete_(String const& cache_name)
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let promise be the result of running the algorithm specified in has(cacheName) method with cacheName.
    auto promise = has(cache_name);

    // 2. Return the result of reacting to promise with a fulfillment handler that, when called with argument cacheExists,
    //    performs the following substeps:
    return WebIDL::upon_fulfillment(promise, GC::create_function(realm.heap(), [this, &realm, cache_name](JS::Value cache_exists) mutable -> WebIDL::ExceptionOr<JS::Value> {
        // 1. If cacheExists is false, then:
        if (!cache_exists.as_bool()) {
            // 1. Return false.
            return false;
        }

        HTML::TemporaryExecutionContext context { realm };

        // 1. Let cacheJobPromise be a new promise.
        auto cache_job_promise = WebIDL::create_promise(realm);

        // 2. Run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, cache_job_promise, cache_name]() {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Remove the relevant name to cache map[cacheName].
            relevant_name_to_cache_map().remove(cache_name);

            // 2. Resolve cacheJobPromise with true.
            WebIDL::resolve_promise(realm, cache_job_promise, JS::Value { true });

            // Note: After this step, the existing DOM objects (i.e. the currently referenced Cache, Request, and
            //       Response objects) should remain functional.
        }));

        // 3. Return cacheJobPromise.
        return cache_job_promise->promise();
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-keys
GC::Ref<WebIDL::Promise> CacheStorage::keys()
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise]() {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let cacheKeys be the result of getting the keys of the relevant name to cache map.
        auto cache_keys = relevant_name_to_cache_map().keys();

        // Note: The items in the result ordered set are in the order that their corresponding entry was added to the
        //       name to cache map.

        // 2. Resolve promise with cacheKeys.
        WebIDL::resolve_promise(realm, promise, JS::Array::create_from<String>(realm, cache_keys, [&](String const& cache_key) {
            return JS::PrimitiveString::create(realm.vm(), cache_key);
        }));
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
