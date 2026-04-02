/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

struct CacheQueryOptions {
    bool ignore_search { false };
    bool ignore_method { false };
    bool ignore_vary { false };
};

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
class Cache : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Cache, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Cache);

public:
    GC::Ref<WebIDL::Promise> match(Fetch::RequestInfo, CacheQueryOptions);
    GC::Ref<WebIDL::Promise> match_all(Optional<Fetch::RequestInfo>, CacheQueryOptions);
    GC::Ref<WebIDL::Promise> add(Fetch::RequestInfo);
    GC::Ref<WebIDL::Promise> add_all(ReadonlySpan<Fetch::RequestInfo>);
    GC::Ref<WebIDL::Promise> put(Fetch::RequestInfo, GC::Ref<Fetch::Response>);
    GC::Ref<WebIDL::Promise> delete_(Fetch::RequestInfo, CacheQueryOptions);
    GC::Ref<WebIDL::Promise> keys(Optional<Fetch::RequestInfo>, CacheQueryOptions);

private:
    Cache(JS::Realm&, GC::Ref<RequestResponseList>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    enum class CloneCache {
        No,
        Yes,
    };
    GC::Ref<RequestResponseList> query_cache(GC::Ref<Fetch::Infrastructure::Request> request_query, CacheQueryOptions options = {}, GC::Ptr<RequestResponseList> = {}, CloneCache = Cache::CloneCache::Yes);
    WebIDL::ExceptionOr<bool> batch_cache_operations(GC::Ref<GC::HeapVector<GC::Ref<CacheBatchOperation>>>);

    GC::Ref<RequestResponseList> m_request_response_list;
};

}
