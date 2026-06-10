/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/CacheStorage.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(CacheStorage);

CacheStorage::CacheStorage()
{
}

void CacheStorage::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_relevant_name_to_cache_map);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-match
void CacheStorage::match(JS::Realm& realm, Fetch::RequestInfo request, MultiCacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. If options["cacheName"] exists, then:
    if (options.cache_name.has_value()) {
        // 1. Return a new promise promise and run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this, &realm, promise, request = move(request), options = move(options)]() mutable {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. For each cacheName → cache of the relevant name to cache map:
            //     1. If options["cacheName"] matches cacheName, then:
            if (auto result = relevant_name_to_cache_map().get(*options.cache_name); result.has_value()) {
                // 1. Resolve promise with the result of running the algorithm specified in match(request, options)
                //    method of Cache interface with request and options (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = GC::Heap::the().allocate<Cache>(result.release_value());
                auto match_promise = WebIDL::create_promise(realm);
                cache->match(realm, move(request), move(options), match_promise);
                WebIDL::resolve_promise(realm, promise, match_promise->promise());

                // 2. Abort these steps.
                return;
            }

            // 2. Resolve promise with undefined.
            WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        }));
    }
    // 2. Else:
    else {
        // 1. Let promise be a promise resolved with undefined.
        auto internal_promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // 2. For each cacheName → cache of the relevant name to cache map:
        for (auto const& [cache_name, cache_list] : relevant_name_to_cache_map()) {
            // 1. Set promise to the result of reacting to itself with a fulfillment handler that, when called with
            //    argument response, performs the following substeps:
            internal_promise = WebIDL::upon_fulfillment(internal_promise, GC::create_function(GC::Heap::the(), [&realm, cache_list, request, options](JS::Value response) mutable -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If response is not undefined, return response.
                if (!response.is_undefined())
                    return response;

                // 2. Return the result of running the algorithm specified in match(request, options) method of Cache
                //    interface with request and options as the arguments (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = GC::Heap::the().allocate<Cache>(cache_list);
                auto match_promise = WebIDL::create_promise(realm);
                cache->match(realm, move(request), move(options), match_promise);
                return match_promise->promise();
            }));
        }

        WebIDL::react_to_promise(internal_promise,
            GC::create_function(GC::Heap::the(), [&realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                WebIDL::resolve_promise(realm, promise, value);
                return JS::js_undefined();
            }),
            GC::create_function(GC::Heap::the(), [&realm, promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                WebIDL::reject_promise(realm, promise, reason);
                return JS::js_undefined();
            }));
    }
}

// https://w3c.github.io/ServiceWorker/#cache-storage-has
void CacheStorage::has(JS::Realm& realm, String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, this, promise, cache_name] {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. For each key -> value of the relevant name to cache map:
        //     1. If cacheName matches key, resolve promise with true and abort these steps.
        // 2. Resolve promise with false.
        WebIDL::resolve_promise(realm, promise, JS::Value { contains_cache(cache_name) });
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-open
void CacheStorage::open(JS::Realm& realm, String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, this, promise, cache_name] {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. If cacheName matches an existing cache, resolve promise with a new Cache object that represents it.
        // 2. Otherwise, create a new request response list and set the relevant name to cache map[cacheName] to it.
        //    If this cache write operation failed due to exceeding the granted quota limit, reject promise with a
        //    QuotaExceededError and abort these steps.
        // FIXME: Handle cache quotas in CacheStorage::get_or_create_cache().
        auto cache = get_or_create_cache(cache_name);

        // 4. Resolve promise with a new Cache object that represents cache.
        auto cache_object = GC::Heap::the().allocate<Cache>(cache);
        WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, cache_object));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-delete
void CacheStorage::delete_(JS::Realm& realm, String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, this, promise, cache_name] {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. If cacheName does not match an entry, resolve promise with false and abort these steps.
        // 2. Otherwise, remove the relevant name to cache map[cacheName] and resolve promise with true.
        auto const removed_cache = delete_cache(cache_name);
        WebIDL::resolve_promise(realm, promise, JS::Value { removed_cache });

        // Note: After this step, the existing DOM objects (i.e. the currently referenced Cache, Request, and Response
        //       objects) should remain functional.
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-keys
void CacheStorage::keys(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, this, promise] {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let cacheKeys be the result of getting the keys of the relevant name to cache map.
        auto cache_keys = this->cache_keys();

        // Note: The items in the result ordered set are in the order that their corresponding entry was added to the
        //       name to cache map.

        // 2. Resolve promise with cacheKeys.
        WebIDL::resolve_promise(realm, promise, JS::Array::create_from<String>(realm, cache_keys, [&](String const& cache_key) {
            return JS::PrimitiveString::create(realm.vm(), cache_key);
        }));
    }));
}

bool CacheStorage::contains_cache(String const& cache_name)
{
    return relevant_name_to_cache_map().contains(cache_name);
}

bool CacheStorage::delete_cache(String const& cache_name)
{
    if (!contains_cache(cache_name))
        return false;

    relevant_name_to_cache_map().remove(cache_name);
    return true;
}

GC::Ref<RequestResponseList> CacheStorage::get_or_create_cache(String const& cache_name)
{
    if (auto cache = relevant_name_to_cache_map().get(cache_name); cache.has_value())
        return cache.release_value();

    auto cache = GC::Heap::the().allocate<RequestResponseList>();
    relevant_name_to_cache_map().set(cache_name, cache);
    return cache;
}

Vector<String> CacheStorage::cache_keys()
{
    return relevant_name_to_cache_map().keys();
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
