/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Bindings/CacheStorage.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

using CacheQueryOptions = Bindings::CacheQueryOptions;
using MultiCacheQueryOptions = Bindings::MultiCacheQueryOptions;

// https://w3c.github.io/ServiceWorker/#dfn-cache-batch-operation
struct CacheBatchOperation : public GC::Cell {
    GC_CELL(CacheBatchOperation, GC::Cell);
    GC_DECLARE_ALLOCATOR(CacheBatchOperation);

    enum class Type : u8 {
        Delete,
        Put,
    };

    CacheBatchOperation(Type type, GC::Ref<Fetch::Infrastructure::Request> request, GC::Ptr<Fetch::Infrastructure::Response> response = {}, CacheQueryOptions options = {})
        : type(type)
        , request(request)
        , response(response)
        , options(options)
    {
    }

    virtual void visit_edges(Visitor&) override;

    Type type;
    GC::Ref<Fetch::Infrastructure::Request> request;
    GC::Ptr<Fetch::Infrastructure::Response> response;
    CacheQueryOptions options;
};

// https://w3c.github.io/ServiceWorker/#cache-interface
class Cache : public Bindings::Wrappable {
    WEB_WRAPPABLE(Cache, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Cache);

public:
    void match(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void match_all(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void add(JS::Realm&, Fetch::RequestInfo, GC::Ref<WebIDL::Promise>);
    void add_all(JS::Realm&, ReadonlySpan<Fetch::RequestInfo>, GC::Ref<WebIDL::Promise>);
    void put(JS::Realm&, Fetch::RequestInfo, GC::Ref<Fetch::Response>, GC::Ref<WebIDL::Promise>);
    void delete_(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void keys(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);

private:
    explicit Cache(GC::Ref<RequestResponseList>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    enum class CloneCache {
        No,
        Yes,
    };
    GC::Ref<RequestResponseList> query_cache(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request> request_query, CacheQueryOptions options = {}, GC::Ptr<RequestResponseList> = {}, CloneCache = Cache::CloneCache::Yes);
    WebIDL::ExceptionOr<bool> batch_cache_operations(JS::Realm&, GC::Ref<GC::HeapVector<GC::Ref<CacheBatchOperation>>>);

    GC::Ref<RequestResponseList> m_request_response_list;
};

}
