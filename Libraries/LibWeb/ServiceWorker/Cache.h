/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>

namespace Web::ServiceWorker {

struct CacheQueryOptions {
    bool ignore_search { false };
    bool ignore_method { false };
    bool ignore_vary { false };
};

// https://w3c.github.io/ServiceWorker/#cache-interface
class Cache : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Cache, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Cache);

private:
    Cache(JS::Realm&, GC::Ref<RequestResponseList>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<RequestResponseList> m_request_response_list;
};

}
