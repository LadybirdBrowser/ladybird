/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/HeapVector.h>
#include <LibWeb/Forward.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#dfn-request-response-list
struct RequestResponse : public GC::Cell {
    GC_CELL(RequestResponse, GC::Cell);
    GC_DECLARE_ALLOCATOR(RequestResponse);

    RequestResponse(GC::Ref<Fetch::Infrastructure::Request> request, GC::Ref<Fetch::Infrastructure::Response> response)
        : request(request)
        , response(response)
    {
    }

    virtual void visit_edges(Visitor&) override;

    GC::Ref<Fetch::Infrastructure::Request> request;
    GC::Ref<Fetch::Infrastructure::Response> response;
};

// https://w3c.github.io/ServiceWorker/#dfn-request-response-list
using RequestResponseList = GC::HeapVector<GC::Ref<RequestResponse>>;

// https://w3c.github.io/ServiceWorker/#dfn-name-to-cache-map
using NameToCacheMap = OrderedHashMap<String, GC::Ref<RequestResponseList>>;

}
