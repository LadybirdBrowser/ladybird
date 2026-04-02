/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

struct MultiCacheQueryOptions : public CacheQueryOptions {
    Optional<String> cache_name;
};

// https://w3c.github.io/ServiceWorker/#cachestorage-interface
class CacheStorage : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CacheStorage, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CacheStorage);

public:
    GC::Ref<WebIDL::Promise> match(Fetch::RequestInfo, MultiCacheQueryOptions);
    GC::Ref<WebIDL::Promise> has(String const& cache_name);
    GC::Ref<WebIDL::Promise> open(String const& cache_name);
    GC::Ref<WebIDL::Promise> delete_(String const& cache_name);
    GC::Ref<WebIDL::Promise> keys();

private:
    explicit CacheStorage(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    NameToCacheMap& relevant_name_to_cache_map();

    NameToCacheMap m_relevant_name_to_cache_map;
};

}
