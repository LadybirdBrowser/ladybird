/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(ResponseView);

GC::Ref<ResponseView> ResponseView::create(GC::Ref<Response> record)
{
    return GC::Heap::the().allocate<ResponseView>(record);
}

GC::Ref<ResponseView> ResponseView::network_error(String message)
{
    return create(Response::network_error(move(message)));
}

GC::Ref<ResponseView> ResponseView::aborted_network_error()
{
    return create(Response::aborted_network_error());
}

ResponseView::ResponseView(GC::Ref<Response> record)
    : m_record(record)
{
}

void ResponseView::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_record);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#unsafe-response
GC::Ref<ResponseView> ResponseView::unsafe_response()
{
    auto unsafe_response = m_record->unsafe_response();
    if (unsafe_response == m_record)
        return *this;
    return create(unsafe_response);
}

// https://fetch.spec.whatwg.org/#concept-response-clone
GC::Ref<ResponseView> ResponseView::clone(JS::Realm& realm) const
{
    return create(m_record->clone(realm));
}

}
