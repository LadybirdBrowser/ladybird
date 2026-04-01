/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(RequestResponse);

void RequestResponse::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(request);
    visitor.visit(response);
}

}
