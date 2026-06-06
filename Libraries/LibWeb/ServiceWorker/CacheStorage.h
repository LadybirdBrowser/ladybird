/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#cachestorage-interface
class CacheStorage : public Bindings::Wrappable {
    WEB_WRAPPABLE(CacheStorage, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CacheStorage);

public:
    GC::Ref<WebIDL::Promise> match(JS::Realm&, Fetch::RequestInfo, Bindings::MultiCacheQueryOptions);
    GC::Ref<WebIDL::Promise> has(JS::Realm&, String const& cache_name);
    GC::Ref<WebIDL::Promise> open(JS::Realm&, String const& cache_name);
    GC::Ref<WebIDL::Promise> delete_(JS::Realm&, String const& cache_name);
    GC::Ref<WebIDL::Promise> keys(JS::Realm&);

private:
    CacheStorage();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    NameToCacheMap& relevant_name_to_cache_map();

    NameToCacheMap m_relevant_name_to_cache_map;
};

}
