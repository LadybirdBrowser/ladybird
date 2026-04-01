/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CachePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ServiceWorker/Cache.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Cache);

Cache::Cache(JS::Realm& realm, GC::Ref<RequestResponseList> request_response_list)
    : Bindings::PlatformObject(realm)
    , m_request_response_list(request_response_list)
{
}

void Cache::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Cache);
}

void Cache::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request_response_list);
}

}
